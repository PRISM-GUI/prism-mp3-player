#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "core/audio.hpp"
#include "core/memory_storage.hpp"
#include "core/pcm_ring.hpp"
#include "core/player.hpp"

#include <doctest.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <random>
#include <set>

using namespace player;
using State = Player::Status::State;

namespace {

std::vector<std::byte> load(std::string_view name) {
  std::ifstream in{std::string{TEST_DATA_DIR} + "/" + std::string{name}, std::ios::binary};
  REQUIRE(in);
  std::vector<char> chars{std::istreambuf_iterator<char>{in}, {}};
  const auto* p = reinterpret_cast<const std::byte*>(chars.data());
  return {p, p + chars.size()};
}

std::vector<std::byte> repeated(const std::vector<std::byte>& data, int times) {
  std::vector<std::byte> out;
  for (int i = 0; i < times; ++i) out.insert(out.end(), data.begin(), data.end());
  return out;
}

std::vector<std::byte> random_bytes(std::size_t n) {
  std::mt19937 gen{42};
  std::vector<std::byte> out(n);
  for (auto& b : out) b = std::byte(gen());
  return out;
}

class FakeOutput final : public PcmOutput {
public:
  bool start(PcmFormat format, PcmRing& ring) override {
    if (!rates.contains(format.sample_rate)) return false;
    format_ = format;
    ring_ = &ring;
    queued_at_start = ring.size();
    ++starts;
    return true;
  }
  void stop() override { ring_ = nullptr; }
  bool connected() const override { return is_connected; }
  std::uint32_t underruns() const override { return ring_ ? ring_->underruns() : last_underruns_; }
  void set_volume(float v) override { volume = v; }

  // What the sink's callback thread does: takes `frames` stereo frames, when started.
  void pull(std::size_t frames) {
    if (!ring_) return;
    std::vector<std::int16_t> buf(frames * 2);
    ring_->pop(buf);
    last_underruns_ = ring_->underruns();
    for (std::size_t i = 0; i < buf.size(); i += 2) left.push_back(buf[i]);
  }
  bool started() const { return ring_ != nullptr; }
  PcmFormat format() const { return format_; }

  std::set<int> rates{44100, 48000};
  bool is_connected = true;
  float volume = -1;
  int starts = 0;
  std::size_t queued_at_start = 0;
  std::vector<std::int16_t> left;

private:
  PcmRing* ring_ = nullptr;
  PcmFormat format_{};
  std::uint32_t last_underruns_ = 0;
};

TrackRow row(std::string path) { return {std::move(path), "t", "a", "b", 1}; }

// Sign changes between samples louder than the codec's noise around silence.
std::size_t zero_crossings(const std::vector<std::int16_t>& s) {
  std::size_t n = 0;
  bool negative = false;
  for (const auto v : s) {
    if (std::abs(v) < 100 || (v < 0) == negative) continue;
    negative = v < 0;
    ++n;
  }
  return n;
}

std::size_t loud_samples(const std::vector<std::int16_t>& s) {
  return static_cast<std::size_t>(std::ranges::count_if(s, [](auto v) { return std::abs(v) >= 100; }));
}

// Polls and pulls `frames` per step (10 ms by default) until the player stops or `max_steps`
// runs out.
void run(Player& p, FakeOutput& out, std::size_t frames = 441, int max_steps = 2000) {
  for (int i = 0; i < max_steps && p.status().state != State::Stopped; ++i) {
    p.poll();
    out.pull(frames);
  }
}

struct Fixture {
  MemoryStorage storage;
  FakeOutput out;
  Fixture() {
    storage.add("/44k.mp3", load("sine440_44k.mp3"));
    storage.add("/48k.mp3", load("sine440_48k.mp3"));
    storage.add("/32k.mp3", load("sine440_32k.mp3"));
    storage.add("/4s.mp3", repeated(load("sine440_44k.mp3"), 4));
    storage.add("/junk.mp3", random_bytes(8192));
  }
};

}  // namespace

TEST_CASE("plays a 44.1 kHz file: the output gets ~1 s of a 440 Hz sine") {
  Fixture f;
  Player p{f.storage, f.out};
  p.play({row("/44k.mp3")}, 0);
  CHECK(p.status().state == State::Playing);
  CHECK(f.out.format().sample_rate == 44100);
  CHECK(f.out.format().channels == 2);
  CHECK(p.status().duration_s == doctest::Approx(1.0).epsilon(0.1));
  run(p, f.out);
  CHECK(p.status().state == State::Stopped);
  CHECK(loud_samples(f.out.left) == doctest::Approx(44100).epsilon(0.05));
  CHECK(zero_crossings(f.out.left) == doctest::Approx(880).epsilon(10.0 / 880));
}

TEST_CASE("48 kHz plays; 32 kHz is refused with a message and the next track starts") {
  Fixture f;
  Player p{f.storage, f.out};
  p.play({row("/48k.mp3")}, 0);
  CHECK(p.status().state == State::Playing);
  CHECK(f.out.format().sample_rate == 48000);

  f.out.rates.insert(32000);
  p.play({row("/32k.mp3"), row("/44k.mp3")}, 0);
  CHECK(p.status().state == State::Playing);
  CHECK(p.status().index == 1);
  CHECK(p.status().message == "Unsupported sample rate 32000 Hz");
  CHECK(f.out.format().sample_rate == 44100);
}

TEST_CASE("a corrupt file in a queue is skipped") {
  Fixture f;
  Player p{f.storage, f.out};
  p.play({row("/junk.mp3"), row("/44k.mp3")}, 0);
  CHECK(p.status().state == State::Playing);
  CHECK(p.status().index == 1);
  CHECK(p.status().message == "Cannot decode junk.mp3");
}

TEST_CASE("a track no longer on the card is skipped with a message") {
  Fixture f;
  Player p{f.storage, f.out};
  p.play({row("/gone.mp3"), row("/44k.mp3")}, 0);
  CHECK(p.status().state == State::Playing);
  CHECK(p.status().index == 1);
  CHECK(p.status().message.find("gone.mp3") != std::string::npos);
}

TEST_CASE("a queue of only bad tracks stops") {
  Fixture f;
  Player p{f.storage, f.out};
  p.play({row("/gone.mp3"), row("/junk.mp3"), row("/32k.mp3")}, 0);
  CHECK(p.status().state == State::Stopped);
  CHECK_FALSE(f.out.started());
  CHECK_FALSE(p.status().message.empty());
}

TEST_CASE("the end of a track starts the next; the end of the queue stops") {
  Fixture f;
  Player p{f.storage, f.out};
  p.play({row("/44k.mp3"), row("/48k.mp3")}, 0);
  for (int i = 0; i < 2000 && p.status().index == 0; ++i) {
    p.poll();
    f.out.pull(441);
  }
  CHECK(p.status().index == 1);
  CHECK(p.status().state == State::Playing);
  CHECK(f.out.format().sample_rate == 48000);
  run(p, f.out);
  CHECK(p.status().state == State::Stopped);
  CHECK_FALSE(f.out.started());
}

TEST_CASE("a status message is cleared when the next track starts normally") {
  Fixture f;
  Player p{f.storage, f.out};
  p.play({row("/gone.mp3"), row("/44k.mp3"), row("/48k.mp3")}, 0);
  CHECK_FALSE(p.status().message.empty());
  p.next();
  CHECK(p.status().index == 2);
  CHECK(p.status().message.empty());
}

TEST_CASE("a disconnect pauses") {
  Fixture f;
  Player p{f.storage, f.out};
  p.play({row("/44k.mp3")}, 0);
  p.poll();
  f.out.is_connected = false;
  p.poll();
  CHECK(p.status().state == State::Paused);
  CHECK_FALSE(f.out.started());
  CHECK_FALSE(p.status().message.empty());

  f.out.is_connected = true;
  p.toggle_pause();
  CHECK(p.status().state == State::Playing);
  CHECK(f.out.started());
}

TEST_CASE("pause stops pulling and keeps the position") {
  Fixture f;
  Player p{f.storage, f.out};
  p.play({row("/44k.mp3")}, 0);
  for (int i = 0; i < 20; ++i) {
    p.poll();
    f.out.pull(441);
  }
  p.toggle_pause();
  CHECK(p.status().state == State::Paused);
  CHECK_FALSE(f.out.started());
  const double at = p.status().position_s;
  CHECK(at == doctest::Approx(0.2).epsilon(0.1));
  p.poll();
  p.toggle_pause();
  CHECK(p.status().state == State::Playing);
  CHECK(p.status().position_s == doctest::Approx(at));
}

TEST_CASE("underruns are counted when the ring runs dry") {
  Fixture f;
  Player p{f.storage, f.out};
  p.play({row("/4s.mp3")}, 0);
  p.poll();
  CHECK(f.out.underruns() == 0);
  f.out.pull(100'000);
  CHECK(f.out.underruns() == 1);
}

TEST_CASE("the ring is filled before the output starts") {
  Fixture f;
  Player p{f.storage, f.out};
  p.play({row("/4s.mp3")}, 0);
  CHECK(f.out.queued_at_start > 44100);
}

TEST_CASE("an album of same-rate tracks plays without an underrun or a restart") {
  Fixture f;
  Player p{f.storage, f.out};
  p.play({row("/44k.mp3"), row("/4s.mp3"), row("/44k.mp3")}, 0);
  std::set<std::size_t> seen;
  for (int i = 0; i < 2000 && p.status().state != State::Stopped; ++i) {
    seen.insert(p.status().index);
    p.poll();
    f.out.pull(4096);
  }
  CHECK(p.status().state == State::Stopped);
  CHECK(seen == std::set<std::size_t>{0, 1, 2});
  CHECK(f.out.underruns() == 0);
  CHECK(f.out.starts == 1);
  CHECK(zero_crossings(f.out.left) == doctest::Approx(880 * 6).epsilon(0.01));
}

TEST_CASE("a change of sample rate between tracks drains without an underrun") {
  Fixture f;
  Player p{f.storage, f.out};
  p.play({row("/44k.mp3"), row("/48k.mp3")}, 0);
  run(p, f.out, 4096);
  CHECK(p.status().state == State::Stopped);
  CHECK(f.out.starts == 2);
  CHECK(f.out.underruns() == 0);
}

TEST_CASE("a track's position counts from its own start when tracks are chained") {
  Fixture f;
  Player p{f.storage, f.out};
  p.play({row("/44k.mp3"), row("/4s.mp3")}, 0);
  for (int i = 0; i < 2000 && p.status().index == 0; ++i) {
    p.poll();
    f.out.pull(441);
  }
  CHECK(p.status().index == 1);
  CHECK(p.status().position_s < 0.05);
  CHECK(p.status().duration_s == doctest::Approx(4.0).epsilon(0.1));
}

TEST_CASE("a refusing output stops with its own message, without walking the queue") {
  Fixture f;
  f.out.rates.clear();
  Player p{f.storage, f.out};
  p.play({row("/44k.mp3"), row("/48k.mp3")}, 0);
  CHECK(p.status().state == State::Stopped);
  CHECK(p.status().index == 0);
  CHECK(p.status().message == "Output refused 44100 Hz");
}

TEST_CASE("next with the output disconnected pauses on the next track") {
  Fixture f;
  Player p{f.storage, f.out};
  p.play({row("/44k.mp3"), row("/48k.mp3"), row("/44k.mp3")}, 0);
  f.out.is_connected = false;
  p.next();
  CHECK(p.status().state == State::Paused);
  CHECK(p.status().index == 1);
  CHECK(p.status().message == "Headset disconnected");
  CHECK_FALSE(f.out.started());
}

TEST_CASE("previous within 3 s goes back a track, after 3 s restarts") {
  Fixture f;
  Player p{f.storage, f.out};
  p.play({row("/44k.mp3"), row("/4s.mp3")}, 1);
  for (int i = 0; i < 100; ++i) {
    p.poll();
    f.out.pull(441);
  }
  CHECK(p.status().position_s < 3.0);
  p.previous();
  CHECK(p.status().index == 0);

  p.play({row("/44k.mp3"), row("/4s.mp3")}, 1);
  CHECK(p.status().duration_s == doctest::Approx(4.0).epsilon(0.1));
  for (int i = 0; i < 350; ++i) {
    p.poll();
    f.out.pull(441);
  }
  CHECK(p.status().position_s > 3.0);
  p.previous();
  CHECK(p.status().index == 1);
  CHECK(p.status().position_s < 0.1);
}

TEST_CASE("volume is forwarded to the output, clamped to 0..1") {
  Fixture f;
  Player p{f.storage, f.out};
  p.set_volume(0.5f);
  CHECK(f.out.volume == 0.5f);
  p.set_volume(2.0f);
  CHECK(f.out.volume == 1.0f);
}

TEST_CASE("PcmRing: pop fills silence and counts an underrun when short") {
  PcmRing ring{8};
  const std::int16_t in[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  CHECK(ring.push(in) == 8);
  CHECK(ring.size() == 8);
  std::int16_t out[6];
  ring.pop(out);
  CHECK(out[5] == 6);
  CHECK(ring.underruns() == 0);
  CHECK(ring.push(std::span{in}.subspan(8)) == 2);
  ring.pop(out);
  CHECK(out[0] == 7);
  CHECK(out[3] == 10);
  CHECK(out[4] == 0);
  CHECK(ring.underruns() == 1);
}

TEST_CASE("PcmRing: a short read after finish() is the end of the stream, not an underrun") {
  PcmRing ring{8};
  const std::int16_t in[] = {1, 2, 3};
  ring.push(in);
  ring.finish();
  std::int16_t out[6];
  ring.pop(out);
  CHECK(out[2] == 3);
  CHECK(out[3] == 0);
  CHECK(ring.underruns() == 0);
  ring.clear();
  ring.pop(out);
  CHECK(ring.underruns() == 1);
}

TEST_CASE("a player going away stops its output: the ring it reads goes with the player") {
  Fixture f;
  {
    Player p{f.storage, f.out};
    p.play({row("/44k.mp3")}, 0);
    REQUIRE(f.out.started());
  }
  CHECK_FALSE(f.out.started());
}
