// Bluetooth Classic audio out: the headset as the player's output. Grown from prism-esp32's
// main/btspike.cpp, which proved the sequence on this board (find the headset by name in the EIR or
// BDNAME, connect as A2DP source, stream from the data callback), and from ESP-IDF's
// classic_bt/a2dp_source example.

#include "bt_output.hpp"

#include "app_support.hpp"

#include <esp_a2dp_api.h>
#include <esp_avrc_api.h>
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_err.h>
#include <esp_gap_bt_api.h>
#include <esp_timer.h>
#include <nvs_flash.h>
#include <sdkconfig.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace player {
namespace {

constexpr std::string_view kTargetName = CONFIG_PLAYER_BT_NAME;
constexpr std::uint32_t kBytesPerFrame = 4;  // two interleaved 16-bit channels
constexpr int kGainBits = 8;                 // the volume is a Q8 fraction of the samples: 256 is 1
constexpr int kUnityGain = 1 << kGainBits;
constexpr int kBondedList = 4;  // simplify: connect to the first of a handful of bonds

// Where the link is, for state_text().
enum class State { off, scanning, connecting, connected };

// What the three threads share: the player's ring with the data callback, and what the logic
// thread asks of the Bluetooth stack. One headset, one instance: the ESP-IDF callbacks are plain
// function pointers with no user context to pass.
struct Shared {
  std::atomic<PcmRing*> ring{nullptr};       // what the callback pops from, while the player plays
  std::atomic<PcmRing*> last_ring{nullptr};  // the last one, so underruns() survives a pause
  std::atomic<bool> in_callback{false};      // the data callback is inside the ring
  std::atomic<int> gain{kUnityGain};

  std::atomic<bool> connected{false};
  std::atomic<bool> streaming{false};       // the media stream is running, and stays running
  std::atomic<bool> starting{false};        // a START is in flight
  std::atomic<bool> source_ready{false};    // CHECK_SRC_RDY was acknowledged
  std::atomic<bool> checking_ready{false};  // a CHECK_SRC_RDY is in flight
  std::atomic<bool> retried{false};         // the one retry of a refused START was used
  std::atomic<esp_a2d_conn_hdl_t> handle{ESP_A2D_INVALID_CONN_HDL};

  std::atomic<State> link{State::off};
  std::atomic<std::uint32_t> revision{1};  // changes when state_text() would say something else
  std::mutex text_mutex;                   // the name and the handler, never taken by the callback
  std::string name;
  std::function<void(Button)> remote;
};

Shared shared;

void check(const char* call, esp_err_t err) {
  if (err != ESP_OK) std::printf("bt: %s failed: %s\n", call, esp_err_to_name(err));
}

void set_link(State next) {
  shared.link = next;
  shared.revision.fetch_add(1);
}

// The link's 3 s tick (one connect attempt or one discovery at a time) and one 500 ms retry of a
// refused START. Both run on the esp_timer task, never on the Bluetooth task.
constexpr std::uint64_t kLinkTickUs = 3'000'000;
constexpr std::uint64_t kStreamRetryUs = 500'000;

esp_timer_handle_t link_timer = nullptr;
esp_timer_handle_t stream_timer = nullptr;

esp_timer_handle_t open_timer(esp_timer_cb_t callback, const char* name) {
  esp_timer_create_args_t args{};
  args.callback = callback;
  args.arg = nullptr;
  args.dispatch_method = ESP_TIMER_TASK;
  args.name = name;
  esp_timer_handle_t timer = nullptr;
  check("esp_timer_create", esp_timer_create(&args, &timer));
  return timer;
}

// --- discovery -------------------------------------------------------------------------------

esp_bd_addr_t peer{};
bool peer_found = false;

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
void remember_name(const esp_bt_gap_cb_param_t::disc_res_param& found) {
  for (int i = 0; i < found.num_prop; ++i) {
    const auto name = property_name(found.prop[i]);
    if (name.empty()) continue;
    const std::lock_guard lock(shared.text_mutex);
    shared.name = std::string{name};
    shared.revision.fetch_add(1);
    return;
  }
}

void start_discovery();

void on_device_found(const esp_bt_gap_cb_param_t::disc_res_param& found) {
  if (!is_target(found)) return;
  std::memcpy(peer, found.bda, ESP_BD_ADDR_LEN);
  peer_found = true;
  remember_name(found);
  check("esp_bt_gap_cancel_discovery", esp_bt_gap_cancel_discovery());
}

void on_discovery_stopped() {
  if (!peer_found) return start_discovery();  // nothing matched yet: look again
  set_link(State::connecting);
  if (const auto err = esp_a2d_source_connect(peer); err != ESP_OK) {
    check("esp_a2d_source_connect", err);
    start_discovery();
  }
}

// Discovery, unless the radio is busy with something else: the tick retries it in 3 s.
void start_discovery() {
  peer_found = false;
  set_link(State::scanning);
  check("esp_bt_gap_start_discovery",
        esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0));
}

void gap_callback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param) {
  switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT:
      if (shared.link == State::scanning) on_device_found(param->disc_res);
      break;
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
      if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) on_discovery_stopped();
      break;
    case ESP_BT_GAP_CFM_REQ_EVT: esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true); break;
    default: break;
  }
}

// A bonded headset connects without discovery. False: none bonded, or the connect was refused.
bool connect_bonded() {
  const int count = esp_bt_gap_get_bond_device_num();
  if (count <= 0) return false;
  esp_bd_addr_t bonded[kBondedList]{};
  int listed = std::min(count, kBondedList);  // in: how many fit, out: how many came back
  if (esp_bt_gap_get_bond_device_list(&listed, bonded) != ESP_OK || listed <= 0) return false;
  std::memcpy(peer, bonded[0], ESP_BD_ADDR_LEN);
  peer_found = true;
  set_link(State::connecting);
  if (const auto err = esp_a2d_source_connect(peer); err != ESP_OK) {
    check("esp_a2d_source_connect", err);
    return false;
  }
  return true;
}

void on_link_tick(void*);

void start_link_tick() {
  if (!link_timer) link_timer = open_timer(on_link_tick, "bt link");
  if (link_timer && !esp_timer_is_active(link_timer))
    check("esp_timer_start_periodic", esp_timer_start_periodic(link_timer, kLinkTickUs));
}

void stop_link_tick() {
  if (link_timer && esp_timer_is_active(link_timer)) esp_timer_stop(link_timer);
}

// One attempt at a time, every 3 s while nothing is connected: the bonded headset when there is one
// (it is connectable even when it is not discoverable), discovery when there is not. A connect that
// goes nowhere comes back as a disconnect, so this is also the retry loop.
void on_link_tick(void*) {
  if (shared.link == State::connected || shared.link == State::connecting) return;
  if (!connect_bonded()) start_discovery();
}

// --- the stream ------------------------------------------------------------------------------

void start_media() {
  if (shared.starting.exchange(true)) return;  // one START at a time
  const auto err = esp_a2d_media_ctrl_by_handle(shared.handle.load(), ESP_A2D_MEDIA_CTRL_START);
  if (err == ESP_OK) return;
  check("esp_a2d_media_ctrl_by_handle START", err);
  shared.starting = false;
}

// The spike's order: the source must be acknowledged ready before the stream starts. Once started
// the stream stays running -- stop() does not suspend it -- because the stack acks a START sent
// right after a SUSPEND as busy and never sends it again.
void request_stream() {
  if (shared.handle.load() == ESP_A2D_INVALID_CONN_HDL) return;
  if (!shared.source_ready) {
    if (shared.checking_ready.exchange(true)) return;  // its ack takes it from here
    check("esp_a2d_media_ctrl_by_handle CHECK_SRC_RDY",
          esp_a2d_media_ctrl_by_handle(shared.handle.load(), ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY));
    return;
  }
  if (shared.streaming || shared.starting) return;  // already running, or on its way
  if (!shared.ring.load()) return;                  // ready, but nothing to play yet
  start_media();
}

void retry_stream(void*) {
  shared.starting = false;
  request_stream();
}

// A START the stack refused is never resent: try the check-ready/START sequence once more, half a
// second later. simplify: one retry per connection; a headset that keeps refusing needs a reconnect.
void schedule_stream_retry() {
  if (shared.retried.exchange(true)) return;
  if (!stream_timer) stream_timer = open_timer(retry_stream, "bt stream");
  if (stream_timer)
    check("esp_timer_start_once", esp_timer_start_once(stream_timer, kStreamRetryUs));
}

void on_media_ack(const esp_a2d_cb_param_t::media_ctrl_stat_param& ack) {
  if (ack.cmd == ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY) {
    shared.checking_ready = false;
    if (ack.status != ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
      std::printf("bt: source check ack %d\n", static_cast<int>(ack.status));
      return;
    }
    shared.source_ready = true;
    request_stream();
    return;
  }
  if (ack.cmd != ESP_A2D_MEDIA_CTRL_START) return;
  shared.starting = false;
  if (ack.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
    shared.streaming = true;
    return;
  }
  std::printf("bt: media start ack %d\n", static_cast<int>(ack.status));
  schedule_stream_retry();
}

void on_connected(esp_a2d_conn_hdl_t handle) {
  shared.handle = handle;
  shared.connected = true;
  shared.source_ready = false;
  shared.checking_ready = false;
  shared.streaming = false;
  shared.starting = false;
  shared.retried = false;
  set_link(State::connected);
  stop_link_tick();  // the headset is here: no more attempts
  request_stream();
}

void on_disconnected() {
  shared.connected = false;
  shared.handle = ESP_A2D_INVALID_CONN_HDL;
  shared.source_ready = false;
  shared.checking_ready = false;
  shared.streaming = false;
  shared.starting = false;
  shared.retried = false;
  set_link(State::off);
  start_link_tick();  // every 3 s from here, on the timer task: never from inside this callback
}

void a2d_callback(esp_a2d_cb_event_t event, esp_a2d_cb_param_t* param) {
  switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT:
      if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED)
        on_connected(param->conn_stat.conn_hdl);
      else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED)
        on_disconnected();
      break;
    case ESP_A2D_MEDIA_CTRL_ACK_EVT: on_media_ack(param->media_ctrl_stat); break;
    default: break;
  }
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

void start_bluetooth() {
  init_flash();
  start_bluedroid();
  korvo1::print_internal_heap("after bluedroid");
  start_profiles();
  on_link_tick(nullptr);  // the first attempt, now
  start_link_tick();      // and every 3 s while nothing is connected
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
  request_stream();  // starts the stream if it is not running yet, else it keeps running
  return true;
}

// core/audio.hpp: the player clears the ring once this returns, so the callback must be out of it
// and unable to reach it again. It never blocks, so the spin is one buffer long at most.
// The stream itself keeps running: it sends silence while no ring is attached (see request_stream).
void BtOutput::stop() {
  shared.ring = nullptr;
  while (shared.in_callback) {
  }
}

bool BtOutput::connected() const { return shared.connected; }

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
    case State::off: return "Off";
    case State::scanning: return "Scanning…";
    case State::connecting: return "Connecting…";
    case State::connected: break;
  }
  const std::lock_guard lock(shared.text_mutex);
  return "Connected to " + (shared.name.empty() ? std::string{kTargetName} : shared.name);
}

}  // namespace player
