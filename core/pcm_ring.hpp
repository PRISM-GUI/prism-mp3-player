#pragma once
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace player {

// A lock-free single-producer single-consumer ring of int16 samples: the player pushes, the
// output's callback thread pops. The consumer never blocks or allocates.
// Not prism::dev::SpscRing: its capacity is a template parameter held inline, and this one is
// sized at construction (2 s at 48 kHz stereo: too large for an inline array on the board).
class PcmRing {
  static_assert(std::atomic<std::size_t>::is_always_lock_free);
  static_assert(std::atomic<bool>::is_always_lock_free);

public:
  explicit PcmRing(std::size_t capacity) : slots_(capacity) {}

  // Producer side: copies what fits, returns how many samples.
  std::size_t push(std::span<const std::int16_t> in) {
    const auto w = write_.load(std::memory_order_relaxed);
    const auto r = read_.load(std::memory_order_acquire);
    const auto n = std::min(in.size(), capacity() - distance(r, w));
    copy_in(w, in.first(n));
    write_.store(advance(w, n), std::memory_order_release);
    return n;
  }

  // Producer side: no more samples come until clear(), so a short pop is the end of the
  // stream, not an underrun.
  void finish() { finished_.store(true, std::memory_order_release); }

  // Consumer side: fills `out` entirely, with silence past what the ring holds, and counts
  // an underrun when it had to before finish().
  void pop(std::span<std::int16_t> out) {
    // Before the write index: a finish() seen here comes with the final write index.
    const bool finished = finished_.load(std::memory_order_acquire);
    const auto r = read_.load(std::memory_order_relaxed);
    const auto w = write_.load(std::memory_order_acquire);
    const auto n = std::min(out.size(), distance(r, w));
    copy_out(r, out.first(n));
    read_.store(advance(r, n), std::memory_order_release);
    if (n == out.size()) return;
    std::fill(out.begin() + static_cast<std::ptrdiff_t>(n), out.end(), std::int16_t{0});
    if (!finished) underruns_.fetch_add(1, std::memory_order_relaxed);
  }

  std::size_t size() const {
    return distance(read_.load(std::memory_order_acquire), write_.load(std::memory_order_acquire));
  }
  std::size_t free_space() const { return capacity() - size(); }
  std::size_t capacity() const { return slots_.size(); }
  std::uint32_t underruns() const { return underruns_.load(std::memory_order_relaxed); }

  // Producer side, only while no consumer runs (the output is stopped).
  void clear() {
    read_.store(write_.load(std::memory_order_relaxed), std::memory_order_release);
    finished_.store(false, std::memory_order_release);
  }

private:
  // Indices run modulo twice the capacity, so a full ring (distance == capacity) differs from
  // an empty one without a spare slot or a wide counter.
  std::size_t advance(std::size_t i, std::size_t n) const {
    i += n;
    return i >= 2 * capacity() ? i - 2 * capacity() : i;
  }
  std::size_t distance(std::size_t r, std::size_t w) const {
    return w >= r ? w - r : w + 2 * capacity() - r;
  }
  std::size_t slot(std::size_t i) const { return i >= capacity() ? i - capacity() : i; }

  void copy_in(std::size_t w, std::span<const std::int16_t> in) {
    const auto start = slot(w);
    const auto first = std::min(in.size(), capacity() - start);
    std::copy_n(in.begin(), first, slots_.begin() + static_cast<std::ptrdiff_t>(start));
    std::copy(in.begin() + static_cast<std::ptrdiff_t>(first), in.end(), slots_.begin());
  }
  void copy_out(std::size_t r, std::span<std::int16_t> out) const {
    const auto start = slot(r);
    const auto first = std::min(out.size(), capacity() - start);
    std::copy_n(slots_.begin() + static_cast<std::ptrdiff_t>(start), first, out.begin());
    std::copy_n(slots_.begin(), out.size() - first, out.begin() + static_cast<std::ptrdiff_t>(first));
  }

  std::vector<std::int16_t> slots_;
  std::atomic<std::size_t> write_{0}, read_{0};
  std::atomic<std::uint32_t> underruns_{0};
  std::atomic<bool> finished_{false};
};

}  // namespace player
