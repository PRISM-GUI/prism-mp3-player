#pragma once
#include "core/pcm_ring.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace player {

struct PcmFormat {
  int sample_rate;
  int channels;  // always 2 out of the decoder
  bool operator==(const PcmFormat&) const = default;
};

// A sink that pulls PCM from a ring on its own thread (A2DP on the board, audio::Output on the
// desktop).
class PcmOutput {
public:
  virtual ~PcmOutput() = default;
  virtual bool accepts(PcmFormat) const = 0;     // false: the player skips tracks in this format
  virtual bool start(PcmFormat, PcmRing&) = 0;  // false: cannot play this format
  virtual void stop() = 0;                      // after it returns, the ring is no longer read
  virtual bool connected() const = 0;           // false: the headset is gone
  virtual std::uint32_t underruns() const = 0;
  virtual void set_volume(float) = 0;  // 0..1; the sink's own volume when it has one
};

// A sink that takes the MP3 stream itself (the VLSI decoder chip). Declared only.
class CompressedOutput {
public:
  virtual ~CompressedOutput() = default;
  virtual bool write(std::span<const std::byte>) = 0;
};

}  // namespace player
