// Bluetooth Classic audio out: the headset as the player's output. Grown from prism-esp32's
// main/btspike.cpp, which proved the sequence on this board (find the headset by name in the EIR or
// BDNAME, connect as A2DP source, stream from the data callback), and from ESP-IDF's
// classic_bt/a2dp_source example.
//
// One task, the link task, owns the link: which headset, whether it is connected, the stream's
// handshake and the retry deadlines. The GAP and A2DP callbacks (on the Bluetooth task) and
// BtOutput::start() (on the logic thread) only post events to its queue, and its timeouts are the
// queue's own wait, so every decision is taken on one thread, one event at a time.

#include "bt_output.hpp"

#include "app_support.hpp"

#include <esp_a2dp_api.h>
#include <esp_avrc_api.h>
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_err.h>
#include <esp_gap_bt_api.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <nvs_flash.h>
#include <sdkconfig.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace player {
namespace {

constexpr std::string_view kTargetName = CONFIG_PLAYER_BT_NAME;
constexpr std::uint32_t kBytesPerFrame = 4;  // two interleaved 16-bit channels
constexpr int kGainBits = 8;                 // the volume is a Q8 fraction of the samples: 256 is 1
constexpr int kUnityGain = 1 << kGainBits;
constexpr int kBondedList = 4;  // simplify: connect to the first of a handful of bonds
constexpr std::int64_t kLinkRetryUs = 3'000'000;
constexpr std::int64_t kStreamRetryUs = 500'000;
constexpr std::uint32_t kLinkTaskStack = 6144;
constexpr UBaseType_t kLinkTaskPriority = 5;
constexpr UBaseType_t kQueueDepth = 32;
constexpr std::size_t kNameBytes = 32;

// Where the link is. waiting: nothing in flight, the next attempt is due in 3 s.
enum class Link { waiting, scanning, connecting, connected };

// What the link task shares with the other threads: the ring with the data callback, the link's
// state and the headset's name with the logic thread. One headset, one instance: the ESP-IDF
// callbacks are plain function pointers with no user context to pass.
struct Shared {
  std::atomic<PcmRing*> ring{nullptr};       // what the callback pops from, while the player plays
  std::atomic<PcmRing*> last_ring{nullptr};  // the last one, so underruns() survives a pause
  std::atomic<bool> in_callback{false};      // the data callback is inside the ring
  std::atomic<int> gain{kUnityGain};
  std::atomic<Link> link{Link::waiting};     // written by the link task only
  std::atomic<std::uint32_t> revision{1};    // changes when state_text() would say something else
  std::mutex text_mutex;                     // the name and the handler, never taken by the callback
  std::string name;
  std::function<void(Button)> remote;
};

Shared shared;

void check(const char* call, esp_err_t err) {
  if (err != ESP_OK) std::printf("bt: %s failed: %s\n", call, esp_err_to_name(err));
}

std::int64_t now_us() { return esp_timer_get_time(); }

// --- events: what the callbacks and the logic thread tell the link task -----------------------

struct Event {
  enum class Kind {
    found,              // discovery saw the headset: address, name
    discovery_stopped,  // finished, or cancelled
    connected,          // A2DP is up: handle
    disconnected,       // A2DP is down, or a connect went nowhere
    audio_started,      // the media stream runs
    audio_suspended,    // the headset suspended it
    ack,                // a media command was answered: command, status
    play,               // the player attached a ring
  };
  Kind kind = Kind::play;
  esp_bd_addr_t address{};
  char name[kNameBytes]{};  // null-terminated, possibly cut short
  esp_a2d_conn_hdl_t handle = ESP_A2D_INVALID_CONN_HDL;
  esp_a2d_media_ctrl_t command = ESP_A2D_MEDIA_CTRL_NONE;
  esp_a2d_media_ctrl_ack_t status = ESP_A2D_MEDIA_CTRL_ACK_SUCCESS;
};

QueueHandle_t events = nullptr;

Event event(Event::Kind kind) {
  Event out;
  out.kind = kind;
  return out;
}

// Never waits: this runs on the Bluetooth task, which the link task itself calls into, so a stall
// here stalls the link task too.
// simplify: an event dropped on a full queue is not recovered; the queue is 32 deep and a dropped
// play means the stream starts at the next attach. Upgrade: a bigger queue, or a flag the link
// task reads on every wake.
void post(const Event& e) {
  if (events && xQueueSend(events, &e, 0) == pdTRUE) return;
  std::printf("bt: event %d dropped\n", static_cast<int>(e.kind));
}

// --- the callbacks: read the parameters, post an event, decide nothing -----------------------

std::string_view eir_name(std::uint8_t* eir) {
  std::uint8_t length = 0;
  auto* name = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &length);
  if (!name) name = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &length);
  return name ? std::string_view{reinterpret_cast<const char*>(name), length} : std::string_view{};
}

std::string_view property_name(const esp_bt_gap_dev_prop_t& prop) {
  if (prop.type == ESP_BT_GAP_DEV_PROP_EIR) return eir_name(static_cast<std::uint8_t*>(prop.val));
  if (prop.type == ESP_BT_GAP_DEV_PROP_BDNAME)
    return {static_cast<const char*>(prop.val), static_cast<std::size_t>(prop.len)};
  return {};
}

bool is_target(const esp_bt_gap_cb_param_t::disc_res_param& found) {
  return std::any_of(found.prop, found.prop + found.num_prop, [](const auto& prop) {
    return property_name(prop).find(kTargetName) != std::string_view::npos;
  });
}

// The whole name, which may be longer than the part we matched on: "Connected to <name>".
Event found_event(const esp_bt_gap_cb_param_t::disc_res_param& found) {
  auto out = event(Event::Kind::found);
  std::memcpy(out.address, found.bda, ESP_BD_ADDR_LEN);
  for (int i = 0; i < found.num_prop; ++i) {
    const auto name = property_name(found.prop[i]);
    if (name.empty()) continue;
    name.copy(out.name, std::min(name.size(), kNameBytes - 1));
    break;
  }
  return out;
}

void gap_callback(esp_bt_gap_cb_event_t event_id, esp_bt_gap_cb_param_t* param) {
  switch (event_id) {
    case ESP_BT_GAP_DISC_RES_EVT:
      if (is_target(param->disc_res)) post(found_event(param->disc_res));
      break;
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
      if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED)
        post(event(Event::Kind::discovery_stopped));
      break;
    case ESP_BT_GAP_CFM_REQ_EVT: esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true); break;
    default: break;
  }
}

void post_connection(const esp_a2d_cb_param_t::a2d_conn_stat_param& connection) {
  if (connection.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
    auto e = event(Event::Kind::connected);
    e.handle = connection.conn_hdl;
    post(e);
  } else if (connection.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
    post(event(Event::Kind::disconnected));
  }
}

void post_audio_state(esp_a2d_audio_state_t state) {
  post(event(state == ESP_A2D_AUDIO_STATE_STARTED ? Event::Kind::audio_started
                                                   : Event::Kind::audio_suspended));
}

void post_ack(const esp_a2d_cb_param_t::media_ctrl_stat_param& ack) {
  auto e = event(Event::Kind::ack);
  e.command = ack.cmd;
  e.status = ack.status;
  post(e);
}

void a2d_callback(esp_a2d_cb_event_t event_id, esp_a2d_cb_param_t* param) {
  switch (event_id) {
    case ESP_A2D_CONNECTION_STATE_EVT: post_connection(param->conn_stat); break;
    case ESP_A2D_AUDIO_STATE_EVT: post_audio_state(param->audio_stat.state); break;
    case ESP_A2D_MEDIA_CTRL_ACK_EVT: post_ack(param->media_ctrl_stat); break;
    default: break;
  }
}

// --- the link task ---------------------------------------------------------------------------

// The first bonded headset, or false when nothing is bonded. Reads bluedroid's in-memory bond
// table: no flash access, so it is safe on a task whose stack is in PSRAM.
bool bonded_address(esp_bd_addr_t out) {
  if (esp_bt_gap_get_bond_device_num() <= 0) return false;
  esp_bd_addr_t bonded[kBondedList]{};
  int listed = kBondedList;  // in: how many fit, out: how many came back
  if (esp_bt_gap_get_bond_device_list(&listed, bonded) != ESP_OK || listed <= 0) return false;
  std::memcpy(out, bonded[0], ESP_BD_ADDR_LEN);
  return true;
}

void remember_name(const char* name) {
  if (name[0] == '\0') return;
  const std::lock_guard lock(shared.text_mutex);
  shared.name = name;
  shared.revision.fetch_add(1);
}

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
    if (shared.ring.load()) send_media(ESP_A2D_MEDIA_CTRL_START, stream_.starting);
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
TickType_t ticks_until(std::optional<std::int64_t> deadline) {
  if (!deadline) return portMAX_DELAY;
  const auto left_us = *deadline - now_us();
  if (left_us <= 0) return 0;
  return pdMS_TO_TICKS(left_us / 1000) + 1;
}

void run_link_task(void*) {
  LinkManager link;
  link.begin();
  for (;;) {
    Event e;
    if (xQueueReceive(events, &e, ticks_until(link.next_deadline())) == pdTRUE) link.handle(e);
    link.fire_due(now_us());
  }
}

// PSRAM for the stack, as the logic thread's: the task calls the Bluetooth API, which only posts
// to the Bluetooth task and reads RAM, never flash (ESP-IDF's rule for PSRAM stacks: no code that
// disables the cache, see api-guides/external-ram).
void start_link_task() {
  TaskHandle_t task = nullptr;
  if (xTaskCreatePinnedToCoreWithCaps(run_link_task, "bt link", kLinkTaskStack, nullptr,
                                      kLinkTaskPriority, &task, 0,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
    std::printf("bt: cannot start the link task\n");
}

// The Bluetooth task, a buffer of ~100 frames at a time: pop, scale, hand the bytes back. Nothing
// here allocates, blocks or takes a lock (core/audio.hpp).
int32_t data_callback(std::uint8_t* data, int32_t length) {
  if (!data || length <= 0) return 0;  // a null buffer means the stream is stopping
  shared.in_callback = true;
  const auto frames = static_cast<std::size_t>(length) / kBytesPerFrame;
  const auto samples = std::span<std::int16_t>{reinterpret_cast<std::int16_t*>(data), frames * 2};
  if (auto* ring = shared.ring.load())
    ring->pop(samples);
  else
    std::fill(samples.begin(), samples.end(), std::int16_t{0});
  const auto gain = shared.gain.load();
  if (gain != kUnityGain)
    for (auto& sample : samples)
      sample = static_cast<std::int16_t>((static_cast<std::int32_t>(sample) * gain) >> kGainBits);
  shared.in_callback = false;
  return static_cast<int32_t>(samples.size() * sizeof(std::int16_t));
}

// --- AVRCP: the headset's buttons -------------------------------------------------------------

void on_passthrough(std::uint8_t key_code, std::uint8_t key_state) {
  if (key_state != ESP_AVRC_PT_CMD_STATE_PRESSED) return;
  Button button = Button::PlayPause;
  switch (key_code) {
    case ESP_AVRC_PT_CMD_PLAY:
    case ESP_AVRC_PT_CMD_PAUSE:
    case ESP_AVRC_PT_CMD_STOP: break;
    case ESP_AVRC_PT_CMD_FORWARD: button = Button::Next; break;
    case ESP_AVRC_PT_CMD_BACKWARD: button = Button::Previous; break;
    default: return;
  }
  std::function<void(Button)> handler;
  {
    const std::lock_guard lock(shared.text_mutex);
    handler = shared.remote;
  }
  if (handler) handler(button);
}

void avrc_tg_callback(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t* param) {
  if (event == ESP_AVRC_TG_PASSTHROUGH_CMD_EVT)
    on_passthrough(param->psth_cmd.key_code, param->psth_cmd.key_state);
}

// We send nothing over AVRCP yet (the volume scales the samples), but the profile must be up for
// the headset to send its own buttons.
void avrc_ct_callback(esp_avrc_ct_cb_event_t, esp_avrc_ct_cb_param_t*) {}

// --- bring-up --------------------------------------------------------------------------------

void init_flash() {
  auto err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    check("nvs_flash_erase", nvs_flash_erase());
    err = nvs_flash_init();
  }
  check("nvs_flash_init", err);
}

// IDF's default-config macros are C initializers that leave fields out: C++ warns, -Werror stops.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
void start_bluedroid() {
  check("esp_bt_controller_mem_release", esp_bt_controller_mem_release(ESP_BT_MODE_BLE));
  esp_bt_controller_config_t controller = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  check("esp_bt_controller_init", esp_bt_controller_init(&controller));
  check("esp_bt_controller_enable", esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));
  esp_bluedroid_config_t bluedroid = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
  check("esp_bluedroid_init_with_cfg", esp_bluedroid_init_with_cfg(&bluedroid));
  check("esp_bluedroid_enable", esp_bluedroid_enable());
}
#pragma GCC diagnostic pop

void start_profiles() {
  check("esp_bt_gap_set_device_name", esp_bt_gap_set_device_name("PRISM"));
  check("esp_bt_gap_register_callback", esp_bt_gap_register_callback(gap_callback));
  check("esp_avrc_ct_init", esp_avrc_ct_init());
  check("esp_avrc_ct_register_callback", esp_avrc_ct_register_callback(avrc_ct_callback));
  check("esp_avrc_tg_init", esp_avrc_tg_init());
  check("esp_avrc_tg_register_callback", esp_avrc_tg_register_callback(avrc_tg_callback));
  check("esp_a2d_source_init", esp_a2d_source_init());
  check("esp_a2d_register_callback", esp_a2d_register_callback(a2d_callback));
  check("esp_a2d_source_register_data_callback",
        esp_a2d_source_register_data_callback(data_callback));
  // Connectable but not discoverable: a headset that knows us may come back by itself, while
  // nothing new can pair without the headset's pairing mode.
  check("esp_bt_gap_set_scan_mode",
        esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE));
}

// The queue first: the callbacks may post as soon as they are registered.
void start_bluetooth() {
  events = xQueueCreate(kQueueDepth, sizeof(Event));
  init_flash();
  start_bluedroid();
  korvo1::print_internal_heap("after bluedroid");
  start_profiles();
  start_link_task();  // the first attempt, now, and every retry from there
}

}  // namespace

BtOutput& BtOutput::open() {
  static BtOutput output;
  static std::once_flag once;
  std::call_once(once, [] { start_bluetooth(); });
  return output;
}

bool BtOutput::accepts(PcmFormat format) const {
  return format.sample_rate == 44100 && format.channels == 2;
}

bool BtOutput::start(PcmFormat format, PcmRing& ring) {
  if (!accepts(format)) return false;
  shared.last_ring = &ring;
  shared.ring = &ring;
  post(event(Event::Kind::play));  // the link task starts the stream if it is not running yet
  return true;
}

// core/audio.hpp: the player clears the ring once this returns, so the callback must be out of it
// and unable to reach it again. It never blocks, so the spin is one buffer long at most.
// The stream itself keeps running: it sends silence while no ring is attached (see Stream).
void BtOutput::stop() {
  shared.ring = nullptr;
  while (shared.in_callback) std::this_thread::yield();
}

bool BtOutput::connected() const { return shared.link == Link::connected; }

std::uint32_t BtOutput::underruns() const {
  auto* ring = shared.ring.load();  // the last attached one when the player is paused
  if (!ring) ring = shared.last_ring.load();
  return ring ? ring->underruns() : 0;
}

std::uint32_t BtOutput::state_revision() const { return shared.revision.load(); }

// simplify: the volume scales the samples in the callback. Absolute volume over AVRCP needs the
// headset to register for it, an exchange that cannot be exercised here; scaling works with every
// headset and costs one multiply per sample. Upgrade: esp_avrc_ct_send_set_absolute_volume_cmd()
// once the peer's features are read.
void BtOutput::set_volume(float volume) {
  shared.gain = static_cast<int>(std::clamp(volume, 0.f, 1.f) * kUnityGain);
}

void BtOutput::on_remote(std::function<void(Button)> handler) {
  const std::lock_guard lock(shared.text_mutex);
  shared.remote = std::move(handler);
}

std::string BtOutput::state_text() const {
  switch (shared.link.load()) {
    case Link::waiting: return "Off";
    case Link::scanning: return "Scanning…";
    case Link::connecting: return "Connecting…";
    case Link::connected: break;
  }
  const std::lock_guard lock(shared.text_mutex);
  return "Connected to " + (shared.name.empty() ? std::string{kTargetName} : shared.name);
}

}  // namespace player
