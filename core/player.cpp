#include "core/player.hpp"

#include "core/path.hpp"
#include "core/tags.hpp"

#include <algorithm>
#include <format>

namespace player {
namespace {

bool supported_rate(int hz) { return hz == kRate44k1 || hz == kRate48k; }

// poll() decodes this many frames at most: a few milliseconds of audio, so a command from the UI
// never waits behind a long decode.
constexpr std::size_t kFramesPerPoll = 8;

}  // namespace

using State = Player::Status::State;

Player::~Player() {
  const std::lock_guard lock(mutex_);
  output_.stop();
}

void Player::play(std::vector<TrackRow> tracks, std::size_t index) {
  const std::lock_guard lock(mutex_);
  queue_ = std::move(tracks);
  start_at(index);
}

void Player::next() {
  const std::lock_guard lock(mutex_);
  start_at(current_.index + 1);
}

void Player::previous() {
  const std::lock_guard lock(mutex_);
  start_at(position_s() > 3.0 || current_.index == 0 ? current_.index : current_.index - 1);
}

void Player::toggle_pause() {
  const std::lock_guard lock(mutex_);
  switch (state_) {
    case State::Playing:
      output_.stop();
      state_ = State::Paused;
      break;
    case State::Paused:
      message_.clear();
      start_output();
      break;
    case State::Stopped:
      start_at(current_.index);
      break;
  }
}

void Player::set_volume(float volume) {
  const std::lock_guard lock(mutex_);
  output_.set_volume(std::clamp(volume, 0.0f, 1.0f));
}

void Player::poll() {
  const std::lock_guard lock(mutex_);
  if (state_ != State::Playing) return;
  if (!output_.connected()) return pause_disconnected();
  fill_ring(kFramesPerPoll);
  promote_played_tracks();
  // Reached only when the queue ends or the next track's format differs: the ring drained.
  if (decoded_all_ && ring_.size() == 0) start_at(current_.index + 1, kFramesPerPoll);
}

Player::Status Player::status() const {
  const std::lock_guard lock(mutex_);
  return {state_, current_.index, position_s(), current_.duration_s, message_};
}

std::vector<TrackRow> Player::queue() const {
  const std::lock_guard lock(mutex_);
  return queue_;
}

std::optional<TrackRow> Player::current_track() const {
  const std::lock_guard lock(mutex_);
  if (current_.index >= queue_.size()) return std::nullopt;
  return queue_[current_.index];
}

std::size_t Player::queue_size() const {
  const std::lock_guard lock(mutex_);
  return queue_.size();
}

// Starts the first playable track from `index`; each one skipped leaves its reason as the
// message, which the next normal start clears. `max_frames` caps what this refills: the public
// commands pass the default (a whole ring), poll() passes its own budget so the UI never waits.
void Player::start_at(std::size_t index, std::size_t max_frames, Then then) {
  halt();
  message_.clear();
  for (; index < queue_.size(); ++index) {
    if (auto error = open_track(index); !error.empty()) {
      message_ = std::move(error);
      continue;
    }
    format_ = decoder_->format();
    current_ = {index, 0, decoder_->duration_s(), {}};
    push(first_frame_);  // a frame already decoded, so one off the budget
    fill_ring(max_frames > 0 ? max_frames - 1 : 0);
    if (then == Then::hold) {
      state_ = State::Paused;
      return;
    }
    return start_output();
  }
  decoder_.reset();
  state_ = State::Stopped;
}

void Player::start_output() {
  if (!output_.connected()) return pause_disconnected();
  if (!output_.start(format_, ring_)) {
    halt();
    state_ = State::Stopped;
    message_ = std::format("Output refused {} Hz", format_.sample_rate);
    return;
  }
  state_ = State::Playing;
}

void Player::pause_disconnected() {
  output_.stop();
  state_ = State::Paused;
  message_ = "Headset disconnected";
}

// Opens the track into decoder_ and decodes its first frame into first_frame_; returns why it
// cannot play. With `at` in seconds, the track starts there rather than at its beginning.
std::string Player::open_track(std::size_t index, double at) {
  const auto& path = queue_[index].path;
  auto file = storage_.open(path);
  if (!file) return std::format("Missing {}", path);
  const auto offset = audio_offset(*file);
  const auto size = file->size();
  decoder_.emplace(std::move(file), offset);
  first_frame_ = decoder_->next_frame();
  if (first_frame_.empty()) return std::format("Cannot decode {}", file_name(path));
  const auto hz = decoder_->format().sample_rate;
  if (!supported_rate(hz)) return std::format("Unsupported sample rate {} Hz", hz);
  if (!output_.accepts(decoder_->format())) return std::format("Output cannot play {} Hz", hz);
  if (at <= 0) return {};
  // simplify: the byte is proportional to the time, which is exact only for a constant bitrate
  // (VBR heads closer or further off the mark). Upgrade: the Xing header's 100-entry TOC.
  const auto duration = decoder_->duration_s();
  if (duration <= 0) return {};
  decoder_->seek_to(offset + static_cast<std::uint64_t>(at / duration * static_cast<double>(size - offset)));
  first_frame_ = decoder_->next_frame();
  if (first_frame_.empty()) return std::format("Cannot decode {}", file_name(path));
  return {};
}

void Player::fill_ring(std::size_t max_frames) {
  for (std::size_t frames = 0; !decoded_all_ && frames < max_frames &&
                              ring_.free_space() >= MINIMP3_MAX_SAMPLES_PER_FRAME;) {
    if (const auto pcm = decoder_->next_frame(); !pcm.empty()) {
      push(pcm);
      ++frames;
      continue;
    }
    if (!chain_next()) {
      decoded_all_ = true;
      ring_.finish();
      continue;
    }
    ++frames;  // the next track's first frame, which chain_next() just pushed
  }
}

// Opens the next playable track behind the last decoded one when its format matches.
bool Player::chain_next() {
  std::string skipped;
  const auto last = queued_.empty() ? current_.index : queued_.back().index;
  for (auto index = last + 1; index < queue_.size(); ++index) {
    if (auto error = open_track(index); !error.empty()) {
      skipped = std::move(error);
      continue;
    }
    if (decoder_->format() != format_) break;
    queued_.push_back({index, pushed_, decoder_->duration_s(), std::move(skipped)});
    push(first_frame_);
    return true;
  }
  decoder_.reset();
  return false;
}

void Player::push(std::span<const std::int16_t> pcm) { pushed_ += ring_.push(pcm); }

void Player::promote_played_tracks() {
  while (!queued_.empty() && played() >= queued_.front().start) {
    current_ = std::move(queued_.front());
    queued_.pop_front();
    message_ = std::move(current_.message);
  }
}

void Player::halt() {
  output_.stop();
  ring_.clear();
  decoder_.reset();
  queued_.clear();
  decoded_all_ = false;
  pushed_ = 0;
  current_.start = 0;
  current_.duration_s = 0;
}

std::uint64_t Player::played() const { return pushed_ - ring_.size(); }

double Player::position_s() const {
  if (format_.sample_rate == 0 || played() < current_.start) return 0;
  return double(played() - current_.start) / 2 / format_.sample_rate;
}

void Player::seek(double seconds) {
  const std::lock_guard lock(mutex_);
  if (state_ == State::Stopped) return;
  const auto total = current_.duration_s;
  if (total <= 0) return;
  const auto target = std::max(0.0, seconds);
  const auto index = current_.index;
  const bool was_playing = state_ == State::Playing;
  halt();
  // At or past the end, or no frame left where the byte lands (an ID3v1 tag, say): the track is
  // over, and the queue moves on as when a track plays to its end. A paused player moves on too,
  // but it must not start playing behind a paused screen.
  if (target >= total || !open_track(index, target).empty())
    return start_at(index + 1, kWholeRing, was_playing ? Then::play : Then::hold);
  format_ = decoder_->format();
  current_ = {index, 0, total, {}};
  message_.clear();
  // Count the audio before the target as played, so position_s() reports it from now on.
  pushed_ = static_cast<std::uint64_t>(target * 2 * format_.sample_rate);
  push(first_frame_);
  fill_ring();
  if (was_playing) start_output();
}

}  // namespace player
