#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

namespace player {

// A length of time, spelled as a type so a sample count and a duration cannot be swapped by
// accident. The arithmetic is the same double arithmetic the bare seconds used.
using Seconds = std::chrono::duration<double>;

struct PcmFormat {
  int sample_rate;
  int channels;  // always 2 out of the decoder
  bool operator==(const PcmFormat&) const = default;
};

// The two rates every output plays: 44.1 kHz is what a CD rip carries and all A2DP reliably
// streams, 48 kHz is the speaker codec's other native rate.
inline constexpr int kRate44k1 = 44100;
inline constexpr int kRate48k = 48000;

// The interleaved samples a second of `format` is worth. A format with no channel count (a default
// one) is the stereo the decoder always gives.
constexpr std::uint64_t samples_per_second(PcmFormat format) {
  const auto channels = format.channels > 0 ? format.channels : 2;
  return static_cast<std::uint64_t>(format.sample_rate) * static_cast<std::uint64_t>(channels);
}

// Where seconds and interleaved sample counts meet: the only place that knows a sample is half a
// stereo frame.
constexpr Seconds seconds_of(std::uint64_t interleaved_samples, PcmFormat format) {
  const auto rate = samples_per_second(format);
  if (rate == 0) return Seconds{0};
  return Seconds{static_cast<double>(interleaved_samples) / static_cast<double>(rate)};
}

constexpr std::uint64_t samples_at(Seconds seconds, PcmFormat format) {
  const auto rate = samples_per_second(format);
  if (rate == 0 || seconds.count() <= 0) return 0;
  return static_cast<std::uint64_t>(seconds.count() * static_cast<double>(rate));
}

// The ring holds this much audio: two seconds of the larger rate, in interleaved samples.
inline constexpr Seconds kRingSeconds{2};

}  // namespace player
