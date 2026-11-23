// The headset as the player's output: the public BtOutput surface, and the data callback the
// A2DP stack calls for the stream's bytes. Grown from prism-esp32's main/btspike.cpp, which proved
// the sequence on this board, and from ESP-IDF's classic_bt/a2dp_source example.
//
// The state machine behind it (which headset, when to retry, the stream's handshake) is
// bt_link.hpp; the callbacks that feed it are bt_callbacks.cpp; the bring-up is bt_bringup.cpp.
// These methods run on the logic thread, the callback on the Bluetooth task.

#include "bt_output.hpp"

#include "bt_shared.hpp"

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>

namespace player::bt_link {

// One buffer, the Bluetooth task: pop, scale, hand the bytes back. Nothing here allocates, blocks
// or takes a lock (core/audio.hpp).
std::int32_t data_callback(std::uint8_t* data, std::int32_t length) {
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
  return static_cast<std::int32_t>(samples.size() * sizeof(std::int16_t));
}

}  // namespace player::bt_link

namespace player {

BtOutput& BtOutput::open() {
  static BtOutput output;
  static std::once_flag once;
  std::call_once(once, [] { bt_link::start_bluetooth(); });
  return output;
}

bool BtOutput::accepts(PcmFormat format) const {
  return format.sample_rate == player::kRate44k1 && format.channels == 2;
}

bool BtOutput::start(PcmFormat format, PcmRing& ring) {
  if (!accepts(format)) return false;
  bt_link::shared.last_ring = &ring;
  bt_link::shared.ring = &ring;
  // The link task starts the stream if it is not running yet.
  bt_link::post(bt_link::event(bt_link::Event::Kind::play));
  return true;
}

// core/audio.hpp: the player clears the ring once this returns, so the callback must be out of it
// and unable to reach it again. It never blocks, so the spin is one buffer long at most.
// The stream itself keeps running: it sends silence while no ring is attached (see Stream).
void BtOutput::stop() {
  bt_link::shared.ring = nullptr;
  while (bt_link::shared.in_callback) std::this_thread::yield();
}

bool BtOutput::connected() const { return bt_link::shared.link == bt_link::Link::connected; }

std::uint32_t BtOutput::underruns() const {
  auto* ring = bt_link::shared.ring.load();  // the last attached one when the player is paused
  if (!ring) ring = bt_link::shared.last_ring.load();
  return ring ? ring->underruns() : 0;
}

std::uint32_t BtOutput::state_revision() const { return bt_link::shared.revision.load(); }

// simplify: the volume scales the samples in the callback. Absolute volume over AVRCP needs the
// headset to register for it, an exchange that cannot be exercised here; scaling works with every
// headset and costs one multiply per sample. Upgrade: esp_avrc_ct_send_set_absolute_volume_cmd()
// once the peer's features are read.
void BtOutput::set_volume(float volume) {
  bt_link::shared.gain =
      static_cast<int>(std::clamp(volume, 0.f, 1.f) * bt_link::kUnityGain);
}

void BtOutput::on_remote(std::function<void(Button)> handler) {
  const std::lock_guard lock(bt_link::shared.text_mutex);
  bt_link::shared.remote = std::move(handler);
}

std::string BtOutput::state_text() const {
  switch (bt_link::shared.link.load()) {
    case bt_link::Link::waiting: return "Off";
    case bt_link::Link::scanning: return "Scanning…";
    case bt_link::Link::connecting: return "Connecting…";
    case bt_link::Link::connected: break;
  }
  const std::lock_guard lock(bt_link::shared.text_mutex);
  return "Connected to " +
         (bt_link::shared.name.empty() ? std::string{bt_link::kTargetName} : bt_link::shared.name);
}

}  // namespace player
