#pragma once
#include "core/audio.hpp"
#include "core/library.hpp"
#include "core/mp3_decoder.hpp"
#include "core/pcm_ring.hpp"
#include "core/storage.hpp"

#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace player {

// Plays a queue of tracks into a PcmOutput. Its methods may be called from more than one thread
// (the UI's logic thread and the audio thread that runs poll()): each one takes an internal lock,
// and poll() decodes only a few frames per call so the UI never waits long for it. Only the ring
// is shared with the output's own thread, and it is lock-free for that.
// The thread that runs poll() must be joined before the Player goes: ~Player takes the lock and
// stops the output, whose ring dies with us.
// Tracks of the same format follow each other in the ring with no gap and no output restart.
class Player {
public:
  Player(Storage& storage, PcmOutput& output) : storage_(storage), output_(output) {}
  ~Player();  // the output reads our ring: it stops before the ring goes
  Player(const Player&) = delete;
  Player& operator=(const Player&) = delete;

  void play(std::vector<TrackRow> tracks, std::size_t index);
  void toggle_pause();
  void next();
  void previous();  // restarts the track if more than 3 s in, else plays the one before
  // Moves the current track to `at`, clamped to its duration: the ring is flushed and refilled from
  // there. Does nothing when stopped, and a paused player stays paused — even when the seek runs the
  // track out, the next track is opened and held, not started.
  void seek(Seconds at);
  void set_volume(float volume);  // 0..1
  // Reads, decodes, refills the ring, handles the end of a track and a disconnect.
  void poll();

  struct Status {
    enum class State { Stopped, Playing, Paused } state;
    std::size_t index;
    Seconds position, duration;
    std::string message;  // why a track was skipped or playback paused; empty when all is well
  };
  Status status() const;
  // A copy: the queue stays behind the lock.
  std::vector<TrackRow> queue() const;
  // The row the status points at, for drawing Now Playing: one copy rather than 5,000.
  std::optional<TrackRow> current_track() const;
  std::size_t queue_size() const;

private:
  // A track in the ring, from sample `start` of the stream.
  struct Track {
    std::size_t index = 0;
    std::uint64_t start = 0;
    Seconds duration{};
    std::string message;  // the status message once it plays
  };

  static constexpr auto kWholeRing = std::numeric_limits<std::size_t>::max();  // an MP3 frame budget
  enum class Then { play, hold };  // after opening a track: start the output, or stay paused
  void start_at(std::size_t index, std::size_t max_mp3_frames = kWholeRing,
                Then then = Then::play);
  void start_output();
  void pause_disconnected();
  std::string open_track(std::size_t index, Seconds at = Seconds{0});
  // Decodes at most `max_mp3_frames` MP3 frames; the whole ring's worth unless told otherwise.
  void fill_ring(std::size_t max_mp3_frames = kWholeRing);
  bool chain_next();
  void push(std::span<const std::int16_t> pcm);
  void promote_played_tracks();
  void halt();
  std::uint64_t played() const;
  Seconds position() const;

  Storage& storage_;
  PcmOutput& output_;
  // Guards every member below: the UI's thread and the audio thread both call in.
  mutable std::mutex mutex_;
  // Read by the output's thread without the lock, by design.
  PcmRing ring_{static_cast<std::size_t>(samples_at(kRingSeconds, PcmFormat{kRate48k, 2}))};
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
