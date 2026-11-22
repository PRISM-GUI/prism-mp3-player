#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "core/memory_storage.hpp"
#include "core/pcm_ring.hpp"
#include "core/player.hpp"

#include <doctest.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace player;
using State = Player::Status::State;

namespace {

std::vector<std::byte> load(std::string_view name) {
  std::ifstream in{std::string{TEST_DATA_DIR} + "/" + std::string{name}, std::ios::binary};
  REQUIRE(in);
  std::vector<char> chars{std::istreambuf_iterator<char>{in}, {}};
  const auto* p = reinterpret_cast<const std::byte*>(chars.data());
  return {p, p + chars.size()};
}

// The output as its own callback thread sees it: pull() runs beside the logic thread's commands,
// which reach stop() and start() through the Player, so this state is guarded here too.
class Sink final : public PcmOutput {
public:
  bool accepts(PcmFormat format) const override { return format.sample_rate == 44100; }
  bool start(PcmFormat format, PcmRing& ring) override {
    const std::lock_guard lock(mutex_);
    format_ = format;
    ring_ = &ring;
    return true;
  }
  void stop() override {
    const std::lock_guard lock(mutex_);
    ring_ = nullptr;
  }
  bool connected() const override { return true; }
  std::uint32_t underruns() const override {
    const std::lock_guard lock(mutex_);
    return underruns_;
  }
  void set_volume(float volume) override {
    const std::lock_guard lock(mutex_);
    volume_ = volume;
  }

  // What the sink's callback does: takes `frames` stereo frames, when started.
  void pull(std::size_t frames) {
    const std::lock_guard lock(mutex_);
    if (!ring_) return;
    std::vector<std::int16_t> buf(frames * 2);
    ring_->pop(buf);
    underruns_ = ring_->underruns();
  }

private:
  mutable std::mutex mutex_;
  PcmRing* ring_ = nullptr;
  PcmFormat format_{};
  std::uint32_t underruns_ = 0;
  float volume_ = 1;
};

TrackRow row(std::string path) { return {std::move(path), "t", "a", "b", 1}; }

}  // namespace

// Run under ThreadSanitizer: every Player method takes its own lock, so the logic thread's
// commands and the audio thread's poll() must never touch a member at the same time.
TEST_CASE("commands from another thread while poll runs") {
  MemoryStorage storage;
  const auto mp3 = load("sine440_44k.mp3");
  for (const auto* name : {"/a.mp3", "/b.mp3", "/c.mp3"}) storage.add(name, mp3);
  Sink sink;
  Player p{storage, sink};
  p.play({row("/a.mp3"), row("/b.mp3"), row("/c.mp3")}, 0);
  REQUIRE(p.status().state == State::Playing);

  std::atomic<bool> stop{false};
  std::thread audio{[&p, &sink, &stop] {
    while (!stop.load(std::memory_order_relaxed)) {
      p.poll();
      sink.pull(4096);
    }
  }};

  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds{2};
  int commands = 0;
  bool consistent = true;
  while (std::chrono::steady_clock::now() < end) {
    consistent = consistent && p.status().index < 3 && p.queue().size() == 3U;
    p.next();
    p.toggle_pause();
    p.set_volume(0.5f);
    ++commands;
  }
  stop.store(true, std::memory_order_relaxed);
  audio.join();

  CHECK(commands > 0);
  CHECK(consistent);  // no command saw an index outside the queue, or a queue that was not three
  const auto final = p.status();
  CHECK(final.index < 3);
  CHECK(p.queue().size() == 3U);
}
