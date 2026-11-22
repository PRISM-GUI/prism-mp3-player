#pragma once
#include "core/audio.hpp"
#include "core/library.hpp"
#include "core/mp3_decoder.hpp"
#include "core/pcm_ring.hpp"
#include "core/storage.hpp"

#include <optional>
#include <string>
#include <vector>

namespace player {

// Plays a queue of tracks into a PcmOutput. Has no thread: the app calls every method,
// poll() included, from one logic thread; only the ring is shared with the output's thread.
class Player {
public:
  Player(Storage& storage, PcmOutput& output) : storage_(storage), output_(output) {}

  void play(std::vector<TrackRow> queue, std::size_t index);
  void toggle_pause();
  void next();
  void previous();  // restarts the track if more than 3 s in, else plays the one before
  void set_volume(float volume);  // 0..1
  // Reads, decodes, refills the ring, handles the end of a track and a disconnect.
  void poll();

  struct Status {
    enum class State { Stopped, Playing, Paused } state;
    std::size_t index;
    double position_s, duration_s;
    std::string message;  // why a track was skipped or playback paused; empty when all is well
  };
  Status status() const;

private:
  void start_at(std::size_t index);
  std::string open_track(std::size_t index);
  void fill_ring();
  void halt();
  double position_s() const;

  Storage& storage_;
  PcmOutput& output_;
  PcmRing ring_{2 * 48000 * 2};
  std::vector<TrackRow> queue_;
  std::size_t index_ = 0;
  Status::State state_ = Status::State::Stopped;
  std::optional<Mp3Decoder> decoder_;
  bool decoded_all_ = false;
  PcmFormat format_{};
  std::uint64_t frames_pushed_ = 0;
  double duration_s_ = 0;
  std::string message_;
};

}  // namespace player
