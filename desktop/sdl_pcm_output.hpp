#pragma once
#include "core/audio.hpp"

#include <prism/audio/output.hpp>

#include <array>
#include <atomic>
#include <memory>

namespace player {

// The sound card, through PRISM's audio::Output: its render thread pops the ring and turns the
// int16 samples into floats at the volume. With no sound card, audio::Output renders on a timer:
// the ring still drains at the playback rate, silently.
class SdlPcmOutput final : public PcmOutput {
public:
  bool accepts(PcmFormat) const override { return true; }  // SDL converts any rate for the card
  bool start(PcmFormat, PcmRing&) override;
  void stop() override;
  bool connected() const override { return true; }
  std::uint32_t underruns() const override { return ring_ ? ring_->underruns() : 0; }
  void set_volume(float v) override { volume_.store(v, std::memory_order_relaxed); }

private:
  void render(std::span<float> out);

  PcmRing* ring_ = nullptr;  // kept after stop(): underruns() still reads it
  std::atomic<float> volume_{1.f};
  std::array<std::int16_t, prism::audio::kMaxBlockFrames * 2> scratch_{};
  std::unique_ptr<prism::audio::Output> device_;  // last: stopped before the rest goes
};

}  // namespace player
