#pragma once

// The link task's state machine: which headset to reach, when the next attempt is due, and the
// media stream's handshake. Every member runs on the link task, one event at a time; nothing here
// is called from another thread.

#include "bt_shared.hpp"

#include <algorithm>
#include <cstring>
#include <optional>

namespace player::bt_link {

constexpr std::int64_t kLinkRetryUs = 3'000'000;
constexpr std::int64_t kStreamRetryUs = 500'000;
constexpr std::uint32_t kLinkTaskStack = 6144;  // calls the Bluetooth API: RAM only, no flash
constexpr UBaseType_t kLinkTaskPriority = 5;
constexpr int kBondedList = 4;  // simplify: connect to the first of a handful of bonds

// The first bonded headset, or false when nothing is bonded. Reads bluedroid's in-memory bond
// table: no flash access, so it is safe on a task whose stack is in PSRAM.
bool bonded_address(esp_bd_addr_t out);

// The whole name, which may be longer than the part we matched on: "Connected to <name>".
void remember_name(const char* name);

// The media stream's handshake on the current connection: CHECK_SRC_RDY acked, then START.
// Once started it stays running across pauses and track changes (stop() never suspends it): the
// stack acks a START sent right after a SUSPEND as busy.
struct Stream {
  bool ready = false;      // CHECK_SRC_RDY was acked
  bool checking = false;   // a CHECK_SRC_RDY is in flight
  bool starting = false;   // a START is in flight
  bool streaming = false;  // the headset says the stream runs (audio state), or acked our START
  bool retried = false;    // the one 500 ms retry of a refused command was used
};

// All of the link's state, touched only on the link task, one event at a time. Each handler
// starts by saying in which states it acts; in any other it does nothing.
class LinkManager {
public:
  void handle(const Event& e) {
    switch (e.kind) {
      case Event::Kind::found: return on_found(e);
      case Event::Kind::discovery_stopped: return on_discovery_stopped();
      case Event::Kind::connected: return on_connected(e.handle);
      case Event::Kind::disconnected: return on_disconnected();
      case Event::Kind::audio_started: return on_audio_started();
      case Event::Kind::audio_suspended: return on_audio_suspended();
      case Event::Kind::ack: return on_ack(e.command, e.status);
      case Event::Kind::play: return on_play();
    }
  }

  // The first attempt, at boot.
  void begin() { attempt(); }

  std::optional<std::int64_t> next_deadline() const {
    if (link_retry_at_ && stream_retry_at_) return std::min(*link_retry_at_, *stream_retry_at_);
    return link_retry_at_ ? link_retry_at_ : stream_retry_at_;
  }

  void fire_due(std::int64_t now) {
    if (link_retry_at_ && *link_retry_at_ <= now) attempt();
    if (stream_retry_at_ && *stream_retry_at_ <= now) {
      stream_retry_at_.reset();
      request_stream();
    }
  }

private:
  void set_link(Link next) {
    link_ = next;
    shared.link = next;
    shared.revision.fetch_add(1);
  }

  void wait_then_retry() {
    set_link(Link::waiting);
    link_retry_at_ = now_us() + kLinkRetryUs;
  }

  // One attempt: the bonded headset when there is one (connectable even when not discoverable),
  // discovery only when nothing is bonded. A failure of either waits 3 s for the next one.
  void attempt() {
    link_retry_at_.reset();
    if (bonded_address(peer_)) return connect_peer();
    start_discovery();
  }

  void connect_peer() {
    set_link(Link::connecting);
    const auto err = esp_a2d_source_connect(peer_);
    if (err == ESP_OK) return;
    check("esp_a2d_source_connect", err);
    wait_then_retry();
  }

  void start_discovery() {
    peer_found_ = false;
    set_link(Link::scanning);
    const auto err = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
    if (err == ESP_OK) return;
    check("esp_bt_gap_start_discovery", err);
    wait_then_retry();
  }

  void on_found(const Event& e) {
    if (link_ != Link::scanning || peer_found_) return;
    std::memcpy(peer_, e.address, ESP_BD_ADDR_LEN);
    peer_found_ = true;
    remember_name(e.name);
    check("esp_bt_gap_cancel_discovery", esp_bt_gap_cancel_discovery());
  }

  // Connect only once the inquiry is over: the radio does one thing at a time.
  void on_discovery_stopped() {
    if (link_ != Link::scanning) return;
    if (peer_found_) return connect_peer();
    attempt();  // nothing matched: look again (or connect, should a bond have appeared)
  }

  // Ours, or the headset's own: it may come back by itself while we scan or wait.
  void on_connected(esp_a2d_conn_hdl_t handle) {
    if (link_ == Link::scanning)
      check("esp_bt_gap_cancel_discovery", esp_bt_gap_cancel_discovery());
    handle_ = handle;
    reset_stream();
    link_retry_at_.reset();
    set_link(Link::connected);
    request_stream();
  }

  // Only a connect we made, or the connection we had, ending: a failed incoming connect while
  // we scan or wait changes nothing.
  void on_disconnected() {
    if (link_ != Link::connecting && link_ != Link::connected) return;
    handle_ = ESP_A2D_INVALID_CONN_HDL;
    reset_stream();
    wait_then_retry();
  }

  void reset_stream() {
    stream_ = {};
    stream_retry_at_.reset();
  }

  void on_audio_started() {
    stream_.starting = false;
    stream_.streaming = true;
    stream_.retried = false;
  }

  // The headset suspended the stream: start it again if the player is playing.
  void on_audio_suspended() {
    stream_.streaming = false;
    request_stream();
  }

  // Each play gets its own retry of a refused START.
  void on_play() {
    stream_.retried = false;
    request_stream();
  }

  void on_ack(esp_a2d_media_ctrl_t command, esp_a2d_media_ctrl_ack_t status) {
    if (link_ != Link::connected) return;  // an answer from a connection that is gone
    const bool ok = status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS;
    if (command == ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY) {
      stream_.checking = false;
      stream_.ready = ok;
    } else if (command == ESP_A2D_MEDIA_CTRL_START) {
      stream_.starting = false;
      stream_.streaming = stream_.streaming || ok;
    } else {
      return;
    }
    if (ok) {
      stream_.retried = false;  // an answer that came through leaves the retry unused
      return request_stream();
    }
    std::printf("bt: media command %d ack %d\n", static_cast<int>(command), static_cast<int>(status));
    schedule_stream_retry();
  }

  // The spike's order: the source must be acknowledged ready before the stream starts, and the
  // stream starts once a ring is attached. One command in flight at a time.
  void request_stream() {
    if (link_ != Link::connected) return;
    if (stream_.checking || stream_.starting || stream_.streaming) return;
    if (!stream_.ready) return send_media(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY, stream_.checking);
    if (shared.tap.attached()) send_media(ESP_A2D_MEDIA_CTRL_START, stream_.starting);
  }

  void send_media(esp_a2d_media_ctrl_t command, bool& in_flight) {
    stream_retry_at_.reset();
    const auto err = esp_a2d_media_ctrl_by_handle(handle_, command);
    if (err == ESP_OK) {
      in_flight = true;
      return;
    }
    check("esp_a2d_media_ctrl_by_handle", err);
    schedule_stream_retry();
  }

  // A refused command is never resent by the stack: try once more, half a second later.
  // simplify: one retry per play (or connection); a headset that keeps refusing needs the next
  // play or a reconnect.
  void schedule_stream_retry() {
    if (stream_.retried) return;
    stream_.retried = true;
    stream_retry_at_ = now_us() + kStreamRetryUs;
  }

  Link link_ = Link::waiting;
  esp_bd_addr_t peer_{};
  bool peer_found_ = false;  // discovery saw the headset: connect when the inquiry stops
  esp_a2d_conn_hdl_t handle_ = ESP_A2D_INVALID_CONN_HDL;
  Stream stream_;
  std::optional<std::int64_t> link_retry_at_;    // set only while waiting
  std::optional<std::int64_t> stream_retry_at_;  // set only while connected
};

// The queue's wait is the timer: at most one tick late, never a busy loop.
TickType_t ticks_until(std::optional<std::int64_t> deadline);

void run_link_task(void* arg);

// Starts the link task, which makes the first attempt itself.
void start_link_task();

}  // namespace player::bt_link
