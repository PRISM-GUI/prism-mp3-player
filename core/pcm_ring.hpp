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
public:
  explicit PcmRing(std::size_t capacity) : slots_(capacity) {}

  // Producer side: copies what fits, returns how many samples.
  std::size_t push(std::span<const std::int16_t> in) {
    const auto w = write_.load(std::memory_order_relaxed);
    const auto r = read_.load(std::memory_order_acquire);
    const auto n = std::min<std::size_t>(in.size(), slots_.size() - (w - r));
    for (std::size_t i = 0; i < n; ++i) slots_[(w + i) % slots_.size()] = in[i];
    write_.store(w + n, std::memory_order_release);
    return n;
  }

  // Consumer side: fills `out` entirely, with silence past what the ring holds, and counts
  // an underrun when it had to.
  void pop(std::span<std::int16_t> out) {
    const auto r = read_.load(std::memory_order_relaxed);
    const auto w = write_.load(std::memory_order_acquire);
    const auto n = std::min<std::size_t>(out.size(), w - r);
    for (std::size_t i = 0; i < n; ++i) out[i] = slots_[(r + i) % slots_.size()];
    read_.store(r + n, std::memory_order_release);
    if (n == out.size()) return;
    std::fill(out.begin() + static_cast<std::ptrdiff_t>(n), out.end(), std::int16_t{0});
    underruns_.fetch_add(1, std::memory_order_relaxed);
  }

  std::size_t size() const {
    return write_.load(std::memory_order_acquire) - read_.load(std::memory_order_acquire);
  }
  std::size_t free_space() const { return slots_.size() - size(); }
  std::size_t capacity() const { return slots_.size(); }
  std::uint32_t underruns() const { return underruns_.load(std::memory_order_relaxed); }

  // Producer side, only while no consumer runs (the output is stopped).
  void clear() { read_.store(write_.load(std::memory_order_relaxed), std::memory_order_release); }

private:
  std::vector<std::int16_t> slots_;
  std::atomic<std::uint64_t> write_{0}, read_{0};
  std::atomic<std::uint32_t> underruns_{0};
};

}  // namespace player
