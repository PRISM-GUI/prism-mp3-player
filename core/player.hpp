#pragma once
#include "core/audio.hpp"
#include "core/library.hpp"
#include "core/mp3_decoder.hpp"
#include "core/pcm_ring.hpp"
#include "core/storage.hpp"

#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace player {

// Plays a queue of tracks into a PcmOutput. Has no thread: the app calls every method,
// poll() included, from one logic thread; only the ring is shared with the output's thread.
// Tracks of the same format follow each other in the ring with no gap and no output restart.
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
  const std::vector<TrackRow>& queue() const { return queue_; }

private:
  // A track in the ring, from sample `start` of the stream.
  struct Track {
    std::size_t index = 0;
    std::uint64_t start = 0;
    double duration_s = 0;
    std::string message;  // the status message once it plays
  };

  void start_at(std::size_t index);
  void start_output();
  void pause_disconnected();
  std::string open_track(std::size_t index);
  void fill_ring();
  bool chain_next();
  void push(std::span<const std::int16_t> pcm);
  void promote_played_tracks();
  void halt();
  std::uint64_t played() const;
  double position_s() const;

  Storage& storage_;
  PcmOutput& output_;
  PcmRing ring_{2 * 48000 * 2};
  std::vector<TrackRow> queue_;
  Status::State state_ = Status::State::Stopped;
  Track current_;
  std::deque<Track> queued_;  // decoded behind the current track, not yet heard
  std::optional<Mp3Decoder> decoder_;
  std::span<const std::int16_t> first_frame_;  // of the track open_track() just opened
  bool decoded_all_ = false;
  PcmFormat format_{};
  std::uint64_t pushed_ = 0;  // samples pushed since the output started
  std::string message_;
};

}  // namespace player
