#include "core/player.hpp"

#include "core/path.hpp"
#include "core/tags.hpp"

#include <prism/audio/backend.hpp>
#include <prism/audio/output.hpp>

#include <algorithm>
#include <format>
#include <span>

namespace player {
namespace {

bool supported_rate(int hz) { return hz == kRate44k1 || hz == kRate48k; }

// poll() decodes this many MP3 frames at most: a few milliseconds of audio, so a command from the
// UI never waits behind a long decode.
constexpr std::size_t kMp3FramesPerPoll = 8;

}  // namespace

using State = Player::Status::State;

Player::Player(Storage& storage, std::string device_id)
    : storage_(storage), device_id_(std::move(device_id)) {}

Player::~Player() {
  const std::lock_guard lock(mutex_);
  output_.reset();
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
  start_at(position() > Seconds{3} || current_.index == 0 ? current_.index : current_.index - 1);
}

void Player::toggle_pause() {
  const std::lock_guard lock(mutex_);
  switch (state_) {
    case State::Playing:
      // Destroying the Output is the stop: it takes its pull away before returning, and the ring
      // keeps what it holds, so resuming picks up where it stopped.
      output_.reset();
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
  volume_ = std::clamp(volume, 0.0f, 1.0f);
  if (output_) output_->set_volume(volume_);
}

float Player::volume() const {
  const std::lock_guard lock(mutex_);
  return volume_;
}

std::uint32_t Player::underruns() const {
  const std::lock_guard lock(mutex_);
  return ring_.underruns();
}

void Player::poll() {
  const std::lock_guard lock(mutex_);
  if (state_ != State::Playing) return;
  if (!connected()) return pause_disconnected();
  fill_ring(kMp3FramesPerPoll);
  promote_played_tracks();
  // Reached only when the queue ends or the next track's format differs: the ring drained.
  if (decoded_all_ && ring_.size() == 0) start_at(current_.index + 1, kMp3FramesPerPoll);
}

Player::Status Player::status() const {
  const std::lock_guard lock(mutex_);
  return {state_, current_.index, position(), current_.duration, message_};
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
// message, which the next normal start clears. `max_mp3_frames` caps what this refills: the public
// commands pass the default (a whole ring), poll() passes its own budget so the UI never waits.
void Player::start_at(std::size_t index, std::size_t max_mp3_frames, Then then) {
  halt();
  message_.clear();
  for (; index < queue_.size(); ++index) {
    if (auto error = open_track(index); !error.empty()) {
      message_ = std::move(error);
      continue;
    }
    format_ = decoder_->format();
    current_ = {index, 0, decoder_->duration(), {}};
    push(first_frame_);  // a frame already decoded, so one off the budget
    fill_ring(max_mp3_frames > 0 ? max_mp3_frames - 1 : 0);
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
  const prism::audio::Format wanted{.sample_rate = format_.sample_rate, .channels = format_.channels};
  // The Output pulls this on its own thread; pop() pads silence when the ring runs dry.
  auto render = prism::audio::SampleRenderFn<std::int16_t>{
      [this](std::span<std::int16_t> block) { ring_.pop(block); }};
  if (device_id_.empty()) {
    output_ = std::make_unique<prism::audio::Output>(wanted, render);
  } else {
    output_ = std::make_unique<prism::audio::Output>(device_id_, wanted, render);
  }
  output_->set_volume(volume_);
  if (output_->state() == prism::audio::State::lost) return pause_disconnected();
  state_ = State::Playing;
}

void Player::pause_disconnected() {
  output_.reset();
  state_ = State::Paused;
  message_ = "Headset disconnected";
}

bool Player::accepts(PcmFormat format) const {
  const prism::audio::Format wanted{.sample_rate = format.sample_rate, .channels = format.channels};
  for (const auto& device : prism::audio::devices()) {
    if (device_id_.empty() ? device.is_default : device.id == device_id_) {
      return device.accepts(wanted);
    }
  }
  // Nobody lists it: the Output will say it is lost, and playback pauses there rather than here.
  return true;
}

bool Player::connected() const {
  return !output_ || output_->state() != prism::audio::State::lost;
}

// Opens the track into decoder_ and decodes its first frame into first_frame_; returns why it
// cannot play. With `at`, the track starts there rather than at its beginning.
std::string Player::open_track(std::size_t index, Seconds at) {
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
  if (!accepts(decoder_->format())) return std::format("Output cannot play {} Hz", hz);
  if (at <= Seconds{0}) return {};
  // simplify: the byte is proportional to the time, which is exact only for a constant bitrate
  // (VBR heads closer or further off the mark). Upgrade: the Xing header's 100-entry TOC.
  const auto duration = decoder_->duration();
  if (duration <= Seconds{0}) return {};
  const auto ratio = (at / duration) * static_cast<double>(size - offset);
  decoder_->seek_to(offset + static_cast<std::uint64_t>(ratio));
  first_frame_ = decoder_->next_frame();
  if (first_frame_.empty()) return std::format("Cannot decode {}", file_name(path));
  return {};
}

void Player::fill_ring(std::size_t max_mp3_frames) {
  for (std::size_t mp3_frames = 0; !decoded_all_ && mp3_frames < max_mp3_frames &&
                                    ring_.free_space() >= MINIMP3_MAX_SAMPLES_PER_FRAME;) {
    if (const auto pcm = decoder_->next_frame(); !pcm.empty()) {
      push(pcm);
      ++mp3_frames;
      continue;
    }
    if (!chain_next()) {
      decoded_all_ = true;
      ring_.finish();
      continue;
    }
    ++mp3_frames;  // the next track's first frame, which chain_next() just pushed
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
    queued_.push_back({index, pushed_, decoder_->duration(), std::move(skipped)});
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
  output_.reset();
  ring_.clear();
  decoder_.reset();
  queued_.clear();
  decoded_all_ = false;
  pushed_ = 0;
  current_.start = 0;
  current_.duration = Seconds{0};
}

std::uint64_t Player::played() const { return pushed_ - ring_.size(); }

Seconds Player::position() const {
  if (format_.sample_rate == 0 || played() < current_.start) return Seconds{0};
  return seconds_of(played() - current_.start, format_);
}

void Player::seek(Seconds at) {
  const std::lock_guard lock(mutex_);
  if (state_ == State::Stopped) return;
  const auto total = current_.duration;
  if (total <= Seconds{0}) return;
  const auto target = std::max(Seconds{0}, at);
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
  // Count the audio before the target as played, so position() reports it from now on.
  pushed_ = samples_at(target, format_);
  push(first_frame_);
  fill_ring();
  if (was_playing) start_output();
}

}  // namespace player
