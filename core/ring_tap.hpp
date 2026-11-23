#pragma once
#include "core/pcm_ring.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>

namespace player {

// The consumer side of a PcmRing that the Player attaches and detaches while an audio thread pulls.
// Both board outputs need the same handshake, and it is the kind of handshake that looks right with
// acquire/release and is not: the audio thread stores `inside` and loads the ring pointer, while the
// logic thread stores the pointer and loads `inside`. That is a Dekker pair, and it needs both
// stores to be visible to the other thread in order — with acquire/release each side may see the
// other's stale value and both proceed. Every atomic here keeps the default seq_cst.
//
// The speaker's mutex and condition variable around attach and detach stay in SpeakerOutput: they
// wake its writer, they are not part of the handshake.
class RingTap {
 public:
  // The audio thread pops from `ring` from now on. `last_` keeps the count underruns() reports
  // once the ring is detached.
  void attach(PcmRing& ring) {
    last_.store(&ring);
    ring_.store(&ring);
  }

  // Clears the pointer, then waits until no pull is inside: after this returns the Player may clear
  // or destroy the ring. The wait is one buffer long, because a pull never blocks.
  void detach() {
    ring_.store(nullptr);
    while (inside_.load()) std::this_thread::yield();
  }

  // One buffer for the audio thread: pops what the ring holds, fills the rest with silence (the
  // ring counts that as an underrun unless the stream finished), and returns how many samples came
  // from the ring. Without a ring it writes silence.
  std::size_t pop_or_silence(std::span<std::int16_t> out) {
    inside_.store(true);
    auto* ring = ring_.load();
    std::size_t popped = 0;
    if (ring) {
      popped = std::min(out.size(), ring->size());
      ring->pop(out);
    } else {
      std::fill(out.begin(), out.end(), std::int16_t{0});
    }
    inside_.store(false);
    return popped;
  }

  // For a caller that waits for a ring to play from (the speaker's writer): is one attached?
  bool attached() const { return ring_.load() != nullptr; }

  // The attached ring's count, or the last one's after detach: a paused player still shows what it
  // played, the same number the outputs reported before they shared this.
  std::uint32_t underruns() const {
    auto* ring = ring_.load();
    if (!ring) ring = last_.load();
    return ring ? ring->underruns() : 0;
  }

 private:
  std::atomic<PcmRing*> ring_{nullptr};
  std::atomic<PcmRing*> last_{nullptr};
  std::atomic<bool> inside_{false};
};

}  // namespace player
