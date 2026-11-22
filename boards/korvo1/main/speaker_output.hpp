#pragma once
#include "core/audio.hpp"

#include <cstdint>
#include <memory>

namespace player {

// The board's speaker and headphone jack: the codec and amplifier the BSP brings up. One instance,
// opened at the playing track's rate. A writer thread drains the ring into the codec; the I2S DMA
// under it is what paces playback (core/audio.hpp has the contract).
class SpeakerOutput final : public PcmOutput {
public:
  SpeakerOutput();  // starts the writer, which waits for a ring
  ~SpeakerOutput() override;

  SpeakerOutput(const SpeakerOutput&) = delete;
  SpeakerOutput& operator=(const SpeakerOutput&) = delete;

  bool accepts(PcmFormat format) const override;
  bool start(PcmFormat format, PcmRing& ring) override;
  void stop() override;
  bool connected() const override;
  std::uint32_t underruns() const override;
  void set_volume(float volume) override;

private:
  struct Impl;                   // the codec, the ring and the writer thread
  std::unique_ptr<Impl> impl_;
};

}  // namespace player
