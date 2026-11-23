#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "core/pcm_ring.hpp"
#include "core/ring_tap.hpp"

#include <doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

using namespace player;

namespace {

std::vector<std::int16_t> ramp(std::size_t count) {
  std::vector<std::int16_t> out(count);
  for (std::size_t i = 0; i < count; ++i) out[i] = static_cast<std::int16_t>(i + 1);
  return out;
}

}  // namespace

TEST_CASE("pop with no ring writes silence") {
  RingTap tap;
  std::vector<std::int16_t> out(64, std::int16_t{7});
  CHECK(tap.pop_or_silence(out) == 0);
  CHECK(std::ranges::all_of(out, [](std::int16_t sample) { return sample == 0; }));
  CHECK(tap.underruns() == 0);
}

TEST_CASE("pop after attach returns the ring's samples, then zeros past them") {
  PcmRing ring{128};
  const auto pushed = ramp(100);
  REQUIRE(ring.push(pushed) == pushed.size());

  RingTap tap;
  tap.attach(ring);
  std::vector<std::int16_t> out(256);
  CHECK(tap.pop_or_silence(out) == 100);
  CHECK(std::equal(pushed.begin(), pushed.end(), out.begin()));
  CHECK(std::ranges::all_of(out.begin() + 100, out.end(),
                            [](std::int16_t sample) { return sample == 0; }));
  CHECK(ring.size() == 0);
}

// Two threads and many attach/detach rounds: the puller loops on the tap while the logic thread
// hands the ring back and forth and finally destroys it right after detach() returns. The judge is
// ThreadSanitizer; the assertion only says the puller was really pulling.
TEST_CASE("detach while a puller loops: after detach returns, the ring can be destroyed safely") {
  auto ring = std::make_unique<PcmRing>(4096);
  RingTap tap;
  tap.attach(*ring);

  std::atomic<bool> stop{false};
  std::atomic<std::size_t> popped{0};
  std::thread puller([&tap, &stop, &popped] {
    std::vector<std::int16_t> out(256);
    while (!stop.load(std::memory_order_relaxed)) popped += tap.pop_or_silence(out);
  });

  const std::vector<std::int16_t> sound(256, std::int16_t{11});
  // The rounds below can all end before the puller's thread is scheduled: wait for its first pull.
  ring->push(sound);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
  while (popped.load() == 0 && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
  REQUIRE(popped.load() > 0);
  for (int round = 0; round < 200; ++round) {
    ring->push(sound);
    tap.detach();
    tap.attach(*ring);
  }
  tap.detach();
  ring.reset();  // the puller may still be looping: nothing may touch the old ring from here on

  const auto mark = popped.load();
  for (int spin = 0; spin < 1000 && popped.load() < mark + 2048; ++spin) std::this_thread::yield();
  stop = true;
  puller.join();
}

TEST_CASE("underruns are the ring's, and survive detach") {
  PcmRing ring{256};
  RingTap tap;
  std::vector<std::int16_t> out(64);
  CHECK(tap.pop_or_silence(out) == 0);  // no ring yet: silence, and no ring to count it
  CHECK(tap.underruns() == 0);

  tap.attach(ring);
  tap.pop_or_silence(out);  // the ring is empty: that is an underrun
  CHECK(tap.underruns() == 1);
  tap.pop_or_silence(out);
  CHECK(tap.underruns() == 2);

  tap.detach();
  CHECK(tap.underruns() == 2);  // the ring is gone, the count is not
}
