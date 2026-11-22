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

}  // namespace

using State = Player::Status::State;

void Player::play(std::vector<TrackRow> queue, std::size_t index) {
  queue_ = std::move(queue);
  start_at(index);
}

void Player::next() { start_at(index_ + 1); }

void Player::previous() {
  start_at(position_s() > 3.0 || index_ == 0 ? index_ : index_ - 1);
}

void Player::toggle_pause() {
  switch (state_) {
    case State::Playing:
      output_.stop();
      state_ = State::Paused;
      break;
    case State::Paused:
      if (output_.connected() && output_.start(format_, ring_)) {
        state_ = State::Playing;
        message_.clear();
      }
      break;
    case State::Stopped:
      start_at(index_);
      break;
  }
}

void Player::set_volume(float volume) { output_.set_volume(std::clamp(volume, 0.0f, 1.0f)); }

void Player::poll() {
  if (state_ != State::Playing) return;
  if (!output_.connected()) {
    output_.stop();
    state_ = State::Paused;
    message_ = "Headset disconnected";
    return;
  }
  fill_ring();
  // simplify: not gapless; the next track starts once the ring has drained, so each track
  // change leaves a poll period of silence (and an underrun). Upgrade: keep decoding into the
  // ring when formats match and move the index when the boundary sample is played.
  if (decoded_all_ && ring_.size() == 0) next();
}

Player::Status Player::status() const {
  return {state_, index_, position_s(), duration_s_, message_};
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
    index_ = index;
    state_ = State::Playing;
    return;
  }
  state_ = State::Stopped;
}

// Returns why the track cannot play, or an empty string once its output has started.
std::string Player::open_track(std::size_t index) {
  const auto& path = queue_[index].path;
  auto file = storage_.open(path);
  if (!file) return std::format("Missing {}", path);
  const auto offset = read_tags(*file, file_name(path)).audio_offset;
  decoder_.emplace(std::move(file), offset);
  const auto first = decoder_->next_frame();
  if (first.empty()) {
    decoder_.reset();
    return std::format("Cannot decode {}", file_name(path));
  }
  ring_.push(first);
  format_ = decoder_->format();
  if (!output_.start(format_, ring_)) {
    halt();
    return std::format("Unsupported sample rate {} Hz", format_.sample_rate);
  }
  frames_pushed_ = first.size() / 2;
  duration_s_ = decoder_->duration_s();
  return {};
}

void Player::fill_ring() {
  while (!decoded_all_ && ring_.free_space() >= MINIMP3_MAX_SAMPLES_PER_FRAME) {
    const auto pcm = decoder_->next_frame();
    decoded_all_ = pcm.empty();
    frames_pushed_ += ring_.push(pcm) / 2;
  }
}

void Player::halt() {
  output_.stop();
  ring_.clear();
  decoder_.reset();
  decoded_all_ = false;
  frames_pushed_ = 0;
  duration_s_ = 0;
}

double Player::position_s() const {
  if (format_.sample_rate == 0) return 0;
  const auto queued = ring_.size() / 2;
  return frames_pushed_ > queued ? double(frames_pushed_ - queued) / format_.sample_rate : 0;
}

}  // namespace player
