#include "desktop/sdl_pcm_output.hpp"

#include <algorithm>

namespace player {

bool SdlPcmOutput::start(PcmFormat format, PcmRing& ring) {
  stop();
  ring_ = &ring;
  device_ = std::make_unique<prism::audio::Output>(
      prism::audio::Format{.sample_rate = format.sample_rate, .channels = format.channels},
      [this](std::span<float> out) { render(out); });
  return true;
}

void SdlPcmOutput::stop() { device_.reset(); }

// On the render thread: no lock, no allocation.
void SdlPcmOutput::render(std::span<float> out) {
  const auto pcm = std::span{scratch_}.first(std::min(out.size(), scratch_.size()));
  ring_->pop(pcm);
  const float gain = volume_.load(std::memory_order_relaxed) / 32768.f;
  std::ranges::transform(pcm, out.begin(), [gain](std::int16_t s) { return static_cast<float>(s) * gain; });
}

}  // namespace player
