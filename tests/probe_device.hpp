#pragma once
#include <prism/audio/backend.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace player::testing {

// A PRISM audio device a test drives by hand, the way a sound card's own thread would: it registers
// itself as backend "probe", counts the Outputs opened on it and remembers the format each asked
// for, and plays only when the test pulls. PRISM's ManualDevice pulls the same way but keeps no
// counters, and these tests assert on opens (one Output per format run) and on formats; the lock is
// what lets the threads test pull beside the player's own thread.
class ProbeDevice {
public:
  // `rates` empty means any rate; the default is what the player's outputs play.
  explicit ProbeDevice(std::vector<int> rates = {44100, 48000})
      : impl_(std::make_unique<Impl>(std::move(rates))) {
    prism::audio::register_backend(*impl_);
  }
  // The Outputs playing on this device must be gone before it dies: their stream holds a reference.
  ~ProbeDevice() { prism::audio::unregister_backend(*impl_); }
  ProbeDevice(const ProbeDevice&) = delete;
  ProbeDevice& operator=(const ProbeDevice&) = delete;

  // `frames` stereo frames as the device's thread would take them: silence when no Output is open
  // on it, after lose(), or before the Output asks for the s16 block it does. Valid until the next
  // pull.
  std::span<const std::int16_t> pull(std::size_t frames) { return impl_->pull(frames); }

  void lose() { impl_->lost = true; }    // the headset goes away: opening fails, a stream reports lost
  void revive() { impl_->lost = false; } // and comes back: opening works again

  bool attached() const { return impl_->open_streams > 0; }  // an Output is open on it
  int opens() const { return impl_->opens; }                 // how many Outputs have been opened
  int rate() const { return impl_->format.sample_rate; }     // the format the last one asked for
  int channels() const { return impl_->format.channels; }
  float volume() const { return impl_->volume; }  // what the Output's set_volume reached

  struct Impl final : prism::audio::DeviceBackend {
    static constexpr std::string_view kId = "probe";

    explicit Impl(std::vector<int> device_rates) : rates(std::move(device_rates)) {}

    // The Output's side of the device: it takes the volume itself and reports the loss.
    struct Stream final : prism::audio::DeviceStream {
      // Built inside open(), which already holds the device's lock: the counters are atomics.
      explicit Stream(Impl& device) : device(device) {
        ++device.open_streams;
        ++device.opens;
      }
      ~Stream() override {
        // A pull in flight finishes first: the Output may still be in one when it lets us go.
        const std::lock_guard lock(device.mutex);
        --device.open_streams;
      }
      [[nodiscard]] bool lost() const override { return device.lost; }
      bool set_volume(float level) override {
        device.volume = level;
        return true;  // this device takes the volume, so Output never scales the samples
      }
      Impl& device;
    };

    std::vector<prism::audio::DeviceInfo> devices() override {
      return {prism::audio::DeviceInfo{.id = std::string{kId},
                                       .name = "Probe device",
                                       .rates = rates,
                                       .max_channels = 2,
                                       .native = prism::audio::SampleType::s16,
                                       .is_default = false}};
    }

    std::unique_ptr<prism::audio::DeviceStream> open(std::string_view id,
                                                     const prism::audio::Format& wanted,
                                                     prism::audio::PullFn pull, std::string& error) override {
      const std::lock_guard lock(mutex);
      if (id != kId) {
        error = "no such device";
        return nullptr;
      }
      if (lost) {
        error = "the device is gone";
        return nullptr;
      }
      if (!devices().front().accepts(wanted)) {
        error = "the device cannot play this format";
        return nullptr;
      }
      format = wanted;
      render = std::move(pull);
      block.clear();
      return std::make_unique<Stream>(*this);
    }

    std::span<const std::int16_t> pull(std::size_t frames) {
      const std::lock_guard lock(mutex);
      const auto channels = format.channels > 0 ? format.channels : 2;
      block.assign(frames * static_cast<std::size_t>(channels), 0);
      if (open_streams > 0 && !lost && render)
        render(std::as_writable_bytes(std::span{block}), static_cast<int>(frames));
      return block;
    }

    std::vector<int> rates;
    prism::audio::Format format{};
    prism::audio::PullFn render;
    std::vector<std::int16_t> block;
    std::mutex mutex;
    std::atomic<int> open_streams{0};
    std::atomic<int> opens{0};
    std::atomic<bool> lost{false};
    std::atomic<float> volume{1.f};
  };

private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace player::testing
