#pragma once
#include "core/audio.hpp"
#include "core/storage.hpp"
#include "third_party/minimp3/minimp3.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace player {

// Decodes MP3 frames from a file, reading it in 16 KB chunks.
class Mp3Decoder {
public:
  Mp3Decoder(std::unique_ptr<File> file, std::uint64_t audio_offset);

  // The next frame as interleaved stereo (mono duplicated); empty at the end or when the
  // stream cannot be decoded. Valid until the next call.
  std::span<const std::int16_t> next_frame();
  // Valid once a frame was decoded.
  PcmFormat format() const { return {sample_rate_, 2}; }
  // From a Xing or VBRI header's frame count, else from the first frame's bitrate.
  double duration_s() const;
  // Drops the buffered bytes and reads on from `byte` (clamped to the audio): minimp3 resyncs on the
  // next frame header it finds. Stub until the implementation lands.
  void seek_to(std::uint64_t byte);

private:
  static constexpr std::size_t kChunk = 16 * 1024;

  void refill();

  std::unique_ptr<File> file_;
  std::uint64_t audio_offset_;
  std::uint64_t file_pos_;
  std::vector<std::uint8_t> buf_;
  std::size_t pos_ = 0, end_ = 0;
  mp3dec_t dec_;
  std::array<std::int16_t, MINIMP3_MAX_SAMPLES_PER_FRAME> pcm_{};
  int sample_rate_ = 0, bitrate_kbps_ = 0;
  std::uint64_t vbr_samples_ = 0;  // per channel, from the first frame's Xing or VBRI header; 0: none
};

}  // namespace player
