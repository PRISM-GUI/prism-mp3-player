#define MINIMP3_IMPLEMENTATION
#include "core/mp3_decoder.hpp"

#include <algorithm>
#include <cstring>
#include <string_view>

namespace player {
namespace {

using FrameBytes = std::span<const std::uint8_t>;

std::uint32_t be32_at(FrameBytes f, std::size_t at) {
  return std::uint32_t{f[at]} << 24 | std::uint32_t{f[at + 1]} << 16 | std::uint32_t{f[at + 2]} << 8 | f[at + 3];
}

bool tag_at(FrameBytes f, std::size_t at, std::string_view tag) {
  return f.size() >= at + tag.size() && std::memcmp(f.data() + at, tag.data(), tag.size()) == 0;
}

// The Xing header follows the side information, whose size depends on the MPEG version and the
// channel mode: http://gabriel.mp3-tech.org/mp3infotag.html
std::size_t xing_offset(FrameBytes f) {
  const bool mpeg1 = (f[1] & 0x18) == 0x18;
  const bool mono = (f[3] & 0xc0) == 0xc0;
  const bool crc = (f[1] & 0x01) == 0;
  return 4 + (crc ? 2 : 0) + (mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17));
}

// The frame count a Layer III first frame declares, 0 when it declares none. Info, the CBR form
// of Xing, is left out: the bitrate is exact for CBR, and a file of joined CBR files keeps the
// first one's Info. VBRI (Fraunhofer) sits 32 bytes after the header:
// https://www.codeproject.com/Articles/8295/MPEG-Audio-Frame-Header#VBRIHeader
std::uint32_t vbr_frame_count(FrameBytes f) {
  if (f.size() < 4) return 0;
  const auto xing = xing_offset(f);
  if (tag_at(f, xing, "Xing") && f.size() >= xing + 12 && (f[xing + 7] & 0x01)) return be32_at(f, xing + 8);
  if (tag_at(f, 36, "VBRI") && f.size() >= 36 + 18) return be32_at(f, 36 + 14);
  return 0;
}

}  // namespace

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
    const auto frame = std::span{buf_}.subspan(pos_ + static_cast<std::size_t>(info.frame_offset),
                                               static_cast<std::size_t>(info.frame_bytes - info.frame_offset));
    pos_ += static_cast<std::size_t>(info.frame_bytes);
    if (info.frame_bytes == 0) return {};
    if (samples == 0) continue;
    if (sample_rate_ == 0 && info.layer == 3) vbr_samples_ = std::uint64_t{vbr_frame_count(frame)} * static_cast<std::uint64_t>(samples);
    sample_rate_ = info.hz;
    bitrate_kbps_ = info.bitrate_kbps;
    const auto n = static_cast<std::size_t>(samples);
    if (info.channels == 1)
      for (std::size_t i = n; i-- > 0;) pcm_[2 * i] = pcm_[2 * i + 1] = pcm_[i];
    return std::span{pcm_}.first(2 * n);
  }
}

Seconds Mp3Decoder::duration() const {
  if (vbr_samples_ != 0) return Seconds{static_cast<double>(vbr_samples_) / sample_rate_};
  if (bitrate_kbps_ == 0) return Seconds{0};
  return Seconds{static_cast<double>(file_->size() - audio_offset_) * 8.0 / (bitrate_kbps_ * 1000.0)};
}

// Drops what the buffer still holds and reads on from `byte`: minimp3 resyncs on the next frame
// header it finds there, so a byte in the middle of a frame costs one frame at most.
void Mp3Decoder::seek_to(std::uint64_t byte) {
  file_pos_ = std::clamp(byte, audio_offset_, file_->size());
  pos_ = end_ = 0;
  mp3dec_init(&dec_);
}

}  // namespace player
