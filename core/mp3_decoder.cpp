#define MINIMP3_IMPLEMENTATION
#include "core/mp3_decoder.hpp"

#include <algorithm>
#include <cstring>

namespace player {

Mp3Decoder::Mp3Decoder(std::unique_ptr<File> file, std::uint64_t audio_offset)
    : file_(std::move(file)), audio_offset_(audio_offset), file_pos_(audio_offset),
      buf_(2 * kChunk) {
  mp3dec_init(&dec_);
}

// Keeps at least one chunk ahead of the read position, so a frame and the headers after it
// (which minimp3 checks to sync) are in the buffer.
void Mp3Decoder::refill() {
  if (end_ - pos_ >= kChunk || file_pos_ >= file_->size()) return;
  std::memmove(buf_.data(), buf_.data() + pos_, end_ - pos_);
  end_ -= pos_;
  pos_ = 0;
  const auto n = file_->read(file_pos_, std::as_writable_bytes(std::span{buf_}.subspan(end_, kChunk)));
  end_ += n;
  file_pos_ = n ? file_pos_ + n : file_->size();
}

std::span<const std::int16_t> Mp3Decoder::next_frame() {
  for (;;) {
    refill();
    mp3dec_frame_info_t info{};
    const int samples = mp3dec_decode_frame(&dec_, buf_.data() + pos_, static_cast<int>(end_ - pos_),
                                            pcm_.data(), &info);
    pos_ += static_cast<std::size_t>(info.frame_bytes);
    if (info.frame_bytes == 0) return {};
    if (samples == 0) continue;
    sample_rate_ = info.hz;
    bitrate_kbps_ = info.bitrate_kbps;
    const auto n = static_cast<std::size_t>(samples);
    if (info.channels == 1)
      for (std::size_t i = n; i-- > 0;) pcm_[2 * i] = pcm_[2 * i + 1] = pcm_[i];
    return std::span{pcm_}.first(2 * n);
  }
}

// simplify: from the first frame's bitrate, exact for CBR only; read the Xing/VBRI header for VBR.
double Mp3Decoder::duration_s() const {
  if (bitrate_kbps_ == 0) return 0;
  return static_cast<double>(file_->size() - audio_offset_) * 8.0 / (bitrate_kbps_ * 1000.0);
}

}  // namespace player
