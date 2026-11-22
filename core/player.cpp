#include "core/player.hpp"

#include "core/tags.hpp"

#include <algorithm>
#include <format>

namespace player {
namespace {

std::string_view file_name(std::string_view path) {
  const auto slash = path.rfind('/');
  return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

bool supported_rate(int hz) { return hz == 44100 || hz == 48000; }

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
  if (decoded_all_ && ring_.size() == 0) start_at(current_.index + 1);
}

Player::Status Player::status() const {
  const std::lock_guard lock(mutex_);
  return {state_, current_.index, position_s(), current_.duration_s, message_};
}

std::vector<TrackRow> Player::queue() const {
  const std::lock_guard lock(mutex_);
  return queue_;
}

// Starts the first playable track from `index`; each one skipped leaves its reason as the
// message, which the next normal start clears.
void Player::start_at(std::size_t index) {
  halt();
  message_.clear();
  for (; index < queue_.size(); ++index) {
    if (auto error = open_track(index); !error.empty()) {
      message_ = std::move(error);
      continue;
    }
    format_ = decoder_->format();
    current_ = {index, 0, decoder_->duration_s(), {}};
    push(first_frame_);
    fill_ring();
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
// cannot play.
std::string Player::open_track(std::size_t index) {
  const auto& path = queue_[index].path;
  auto file = storage_.open(path);
  if (!file) return std::format("Missing {}", path);
  const auto offset = audio_offset(*file);
  decoder_.emplace(std::move(file), offset);
  first_frame_ = decoder_->next_frame();
  if (first_frame_.empty()) return std::format("Cannot decode {}", file_name(path));
  const auto hz = decoder_->format().sample_rate;
  if (!supported_rate(hz)) return std::format("Unsupported sample rate {} Hz", hz);
  if (!output_.accepts(decoder_->format())) return std::format("Output cannot play {} Hz", hz);
  return {};
}

void Player::fill_ring(std::size_t max_frames) {
  for (std::size_t frames = 0; !decoded_all_ && frames < max_frames &&
                              ring_.free_space() >= MINIMP3_MAX_SAMPLES_PER_FRAME;) {
    if (const auto pcm = decoder_->next_frame(); !pcm.empty()) {
      push(pcm);
      ++frames;
    } else if (!chain_next()) {
      decoded_all_ = true;
      ring_.finish();
    }
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

}  // namespace player
