#pragma once
#include "core/audio.hpp"
#include "core/pcm_ring.hpp"

#include <cstddef>
#include <cstdint>
#include <set>
#include <vector>

namespace player::testing {

// An output that records what it was told and lets a test drain the ring by hand. The thread-safe
// sink lives in test_player_threads.cpp: it exists for a different reason.
class FakeOutput final : public PcmOutput {
public:
  bool accepts(PcmFormat format) const override { return rates.contains(format.sample_rate); }
  bool start(PcmFormat format, PcmRing& ring) override {
    if (start_fails || !accepts(format)) return false;
    format_ = format;
    ring_ = &ring;
    queued_at_start = ring.size();
    ++starts;
    return true;
  }
  void stop() override { ring_ = nullptr; }
  bool connected() const override { return is_connected && (!connects_on_start || ring_ != nullptr); }
  std::uint32_t underruns() const override { return ring_ ? ring_->underruns() : last_underruns_; }
  void set_volume(float v) override { volume = v; }

  // What the sink's callback thread does: takes `frames` stereo frames, when started.
  void pull(std::size_t frames) {
    if (!ring_) return;
    std::vector<std::int16_t> buf(frames * 2);
    ring_->pop(buf);
    last_underruns_ = ring_->underruns();
    for (std::size_t i = 0; i < buf.size(); i += 2) left.push_back(buf[i]);
  }
  bool started() const { return ring_ != nullptr; }
  std::size_t buffered() const { return ring_ ? ring_->size() : 0; }
  PcmFormat format() const { return format_; }

  std::set<int> rates{44100, 48000};
  bool start_fails = false;
  bool is_connected = true;
  bool connects_on_start = false;  // a sink that reports connected() only after start()
  float volume = -1;
  int starts = 0;
  std::size_t queued_at_start = 0;
  std::vector<std::int16_t> left;

private:
  PcmRing* ring_ = nullptr;
  PcmFormat format_{};
  std::uint32_t last_underruns_ = 0;
};

}  // namespace player::testing
