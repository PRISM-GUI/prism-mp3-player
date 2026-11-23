#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "core/audio.hpp"
#include "core/memory_storage.hpp"
#include "core/pcm_ring.hpp"
#include "core/player.hpp"
#include "tests/forwarding_storage.hpp"
#include "tests/probe_device.hpp"
#include "tests/tag_builder.hpp"
#include "tests/test_data.hpp"

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <random>
#include <set>

using namespace player;
using testing::ForwardingStorage;
using testing::ProbeDevice;
using State = Player::Status::State;

namespace {

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

// The player with the test tracks, a device the test pulls by hand, and the left channel of
// everything it pulled: the audio assertions count that.
struct Rig {
  MemoryStorage storage;
  ProbeDevice out;
  Player player;
  std::vector<std::int16_t> left;

  explicit Rig(std::vector<int> rates = {44100, 48000})
      : out(std::move(rates)), player{storage, "probe"} {
    storage.add("/44k.mp3", testing::load("sine440_44k.mp3"));
    storage.add("/48k.mp3", testing::load("sine440_48k.mp3"));
    storage.add("/32k.mp3", testing::load("sine440_32k.mp3"));
    storage.add("/4s.mp3", repeated(testing::load("sine440_44k.mp3"), 4));
    storage.add("/junk.mp3", random_bytes(8192));
  }

  // What the device's own thread does: takes `frames` stereo frames, and remembers their left ones.
  void pull(std::size_t frames) {
    const auto block = out.pull(frames);
    for (std::size_t i = 0; i + 1 < block.size(); i += 2) left.push_back(block[i]);
  }

  // Polls and pulls `frames` per step (10 ms by default) until the player stops or `max_steps`
  // runs out.
  void run(std::size_t frames = 441, int max_steps = 2000) {
    for (int i = 0; i < max_steps && player.status().state != State::Stopped; ++i) {
      player.poll();
      pull(frames);
    }
  }
};

// Counts the files open and the bytes read below `limit` in them.
class WatchedStorage final : public ForwardingStorage {
public:
  explicit WatchedStorage(Storage& inner) : ForwardingStorage(inner) {}
  std::unique_ptr<File> open(std::string_view path) override {
    auto f = inner_.open(path);
    return f ? std::make_unique<WatchedFile>(std::move(f), *this) : nullptr;
  }

  std::uint64_t limit = 0;
  std::size_t bytes_below_limit = 0;
  int open_files = 0;

private:
  class WatchedFile final : public File {
  public:
    WatchedFile(std::unique_ptr<File> inner, WatchedStorage& s) : inner_(std::move(inner)), s_(s) { ++s_.open_files; }
    ~WatchedFile() override { --s_.open_files; }
    std::uint64_t size() const override { return inner_->size(); }
    std::size_t read(std::uint64_t offset, std::span<std::byte> out) override {
      const auto n = inner_->read(offset, out);
      if (offset < s_.limit) s_.bytes_below_limit += std::min<std::uint64_t>(n, s_.limit - offset);
      return n;
    }

  private:
    std::unique_ptr<File> inner_;
    WatchedStorage& s_;
  };
};

// MPEG-1 Layer III, 128 kbit/s, 44.1 kHz, stereo, no CRC: a 417-byte frame of silence.
std::vector<std::byte> silent_frame() {
  std::vector<std::byte> f(417);
  f[0] = std::byte{0xff};
  f[1] = std::byte{0xfb};
  f[2] = std::byte{0x90};
  return f;
}

// A first frame carrying `tag` then `fields` at byte `at`, then 20 silent frames.
std::vector<std::byte> vbr_file(std::size_t at, std::string_view tag, const std::vector<std::byte>& fields) {
  auto first = silent_frame();
  std::ranges::copy(testing::bytes_of(tag), first.begin() + static_cast<std::ptrdiff_t>(at));
  std::ranges::copy(fields, first.begin() + static_cast<std::ptrdiff_t>(at + 4));
  auto out = first;
  for (int i = 0; i < 20; ++i) testing::append(out, silent_frame());
  return out;
}

double first_frame_duration(std::vector<std::byte> data) {
  MemoryStorage s;
  s.add("/vbr.mp3", std::move(data));
  Mp3Decoder d{s.open("/vbr.mp3"), 0};
  REQUIRE_FALSE(d.next_frame().empty());
  return d.duration().count();
}

}  // namespace

TEST_CASE("plays a 44.1 kHz file: the output gets ~1 s of a 440 Hz sine") {
  Rig f;
  auto& p = f.player;
  p.play({row("/44k.mp3")}, 0);
  CHECK(p.status().state == State::Playing);
  CHECK(f.out.rate() == 44100);
  CHECK(f.out.channels() == 2);
  CHECK(p.status().duration.count() == doctest::Approx(1.0).epsilon(0.1));
  f.run();
  CHECK(p.status().state == State::Stopped);
  CHECK(loud_samples(f.left) == doctest::Approx(44100).epsilon(0.05));
  CHECK(zero_crossings(f.left) == doctest::Approx(880).epsilon(10.0 / 880));
}

TEST_CASE("48 kHz plays; 32 kHz is refused with a message and the next track starts") {
  Rig f;
  auto& p = f.player;
  p.play({row("/48k.mp3")}, 0);
  CHECK(p.status().state == State::Playing);
  CHECK(f.out.rate() == 48000);

  p.play({row("/32k.mp3"), row("/44k.mp3")}, 0);
  CHECK(p.status().state == State::Playing);
  CHECK(p.status().index == 1);
  CHECK(p.status().message == "Unsupported sample rate 32000 Hz");
  CHECK(f.out.rate() == 44100);
}

TEST_CASE("a corrupt file in a queue is skipped") {
  Rig f;
  auto& p = f.player;
  p.play({row("/junk.mp3"), row("/44k.mp3")}, 0);
  CHECK(p.status().state == State::Playing);
  CHECK(p.status().index == 1);
  CHECK(p.status().message == "Cannot decode junk.mp3");
}

TEST_CASE("a track no longer on the card is skipped with a message") {
  Rig f;
  auto& p = f.player;
  p.play({row("/gone.mp3"), row("/44k.mp3")}, 0);
  CHECK(p.status().state == State::Playing);
  CHECK(p.status().index == 1);
  CHECK(p.status().message.find("gone.mp3") != std::string::npos);
}

TEST_CASE("a queue of only bad tracks stops") {
  Rig f;
  auto& p = f.player;
  p.play({row("/gone.mp3"), row("/junk.mp3"), row("/32k.mp3")}, 0);
  CHECK(p.status().state == State::Stopped);
  CHECK_FALSE(f.out.attached());
  CHECK_FALSE(p.status().message.empty());
}

TEST_CASE("the end of a track starts the next; the end of the queue stops") {
  Rig f;
  auto& p = f.player;
  p.play({row("/44k.mp3"), row("/48k.mp3")}, 0);
  for (int i = 0; i < 2000 && p.status().index == 0; ++i) {
    p.poll();
    f.pull(441);
  }
  CHECK(p.status().index == 1);
  CHECK(p.status().state == State::Playing);
  CHECK(f.out.rate() == 48000);
  f.run();
  CHECK(p.status().state == State::Stopped);
  CHECK_FALSE(f.out.attached());
}

TEST_CASE("a status message is cleared when the next track starts normally") {
  Rig f;
  auto& p = f.player;
  p.play({row("/gone.mp3"), row("/44k.mp3"), row("/48k.mp3")}, 0);
  CHECK_FALSE(p.status().message.empty());
  p.next();
  CHECK(p.status().index == 2);
  CHECK(p.status().message.empty());
}

TEST_CASE("a disconnect pauses") {
  Rig f;
  auto& p = f.player;
  p.play({row("/44k.mp3")}, 0);
  p.poll();
  f.out.lose();
  p.poll();
  CHECK(p.status().state == State::Paused);
  CHECK_FALSE(f.out.attached());
  CHECK_FALSE(p.status().message.empty());

  f.out.revive();
  p.toggle_pause();
  CHECK(p.status().state == State::Playing);
  CHECK(f.out.attached());
}

TEST_CASE("pause stops pulling and keeps the position") {
  Rig f;
  auto& p = f.player;
  p.play({row("/44k.mp3")}, 0);
  for (int i = 0; i < 20; ++i) {
    p.poll();
    f.pull(441);
  }
  p.toggle_pause();
  CHECK(p.status().state == State::Paused);
  CHECK_FALSE(f.out.attached());
  const auto at = p.status().position;
  CHECK(at.count() == doctest::Approx(0.2).epsilon(0.1));
  p.poll();
  p.toggle_pause();
  CHECK(p.status().state == State::Playing);
  CHECK(p.status().position == at);
}

TEST_CASE("underruns are counted when the ring runs dry") {
  Rig f;
  auto& p = f.player;
  p.play({row("/4s.mp3")}, 0);
  p.poll();
  CHECK(p.underruns() == 0);
  f.pull(100'000);  // more than the ring holds; the Output pulls it in blocks, each dry one counts
  CHECK(p.underruns() > 0);
}

TEST_CASE("the ring is filled before the output starts") {
  Rig f;
  auto& p = f.player;
  p.play({row("/4s.mp3")}, 0);
  f.pull(44'100);  // a whole second, with no poll: only what the ring held when the Output opened
  CHECK(p.underruns() == 0);
}

TEST_CASE("an album of same-rate tracks plays without an underrun or a restart") {
  Rig f;
  auto& p = f.player;
  p.play({row("/44k.mp3"), row("/4s.mp3"), row("/44k.mp3")}, 0);
  std::set<std::size_t> seen;
  for (int i = 0; i < 2000 && p.status().state != State::Stopped; ++i) {
    seen.insert(p.status().index);
    p.poll();
    f.pull(4096);
  }
  CHECK(p.status().state == State::Stopped);
  CHECK(seen == std::set<std::size_t>{0, 1, 2});
  CHECK(p.underruns() == 0);
  CHECK(f.out.opens() == 1);
  CHECK(zero_crossings(f.left) == doctest::Approx(880 * 6).epsilon(0.01));
}

TEST_CASE("a change of sample rate between tracks drains without an underrun") {
  Rig f;
  auto& p = f.player;
  p.play({row("/44k.mp3"), row("/48k.mp3")}, 0);
  f.run(4096);
  CHECK(p.status().state == State::Stopped);
  CHECK(f.out.opens() == 2);
  CHECK(p.underruns() == 0);
}

TEST_CASE("a track's position counts from its own start when tracks are chained") {
  Rig f;
  auto& p = f.player;
  p.play({row("/44k.mp3"), row("/4s.mp3")}, 0);
  for (int i = 0; i < 2000 && p.status().index == 0; ++i) {
    p.poll();
    f.pull(441);
  }
  CHECK(p.status().index == 1);
  CHECK(p.status().position < Seconds{0.05});
  CHECK(p.status().duration.count() == doctest::Approx(4.0).epsilon(0.1));
}

TEST_CASE("next with the output disconnected pauses on the next track") {
  Rig f;
  auto& p = f.player;
  p.play({row("/44k.mp3"), row("/48k.mp3"), row("/44k.mp3")}, 0);
  f.out.lose();
  p.next();
  CHECK(p.status().state == State::Paused);
  CHECK(p.status().index == 1);
  CHECK(p.status().message == "Headset disconnected");
  CHECK_FALSE(f.out.attached());
}

TEST_CASE("previous within 3 s goes back a track, after 3 s restarts") {
  Rig f;
  auto& p = f.player;
  p.play({row("/44k.mp3"), row("/4s.mp3")}, 1);
  for (int i = 0; i < 100; ++i) {
    p.poll();
    f.pull(441);
  }
  CHECK(p.status().position < Seconds{3});
  p.previous();
  CHECK(p.status().index == 0);

  p.play({row("/44k.mp3"), row("/4s.mp3")}, 1);
  CHECK(p.status().duration.count() == doctest::Approx(4.0).epsilon(0.1));
  for (int i = 0; i < 350; ++i) {
    p.poll();
    f.pull(441);
  }
  CHECK(p.status().position > Seconds{3});
  p.previous();
  CHECK(p.status().index == 1);
  CHECK(p.status().position < Seconds{0.1});
}

TEST_CASE("volume is forwarded to the output, clamped to 0..1") {
  Rig f;
  auto& p = f.player;
  p.set_volume(0.5f);
  CHECK(p.volume() == 0.5f);
  p.set_volume(2.0f);
  CHECK(p.volume() == 1.0f);
  p.play({row("/44k.mp3")}, 0);
  CHECK(f.out.volume() == 1.0f);
}

// There is no Output until a track plays, so the volume is the player's own until then.
TEST_CASE("volume set before the first track reaches the first Output") {
  Rig f;
  f.player.set_volume(0.3f);
  f.player.play({row("/44k.mp3")}, 0);
  CHECK(f.out.volume() == doctest::Approx(0.3f));
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
  MemoryStorage storage;
  storage.add("/44k.mp3", testing::load("sine440_44k.mp3"));
  ProbeDevice out;
  {
    Player p{storage, "probe"};
    p.play({row("/44k.mp3")}, 0);
    REQUIRE(out.attached());
  }
  CHECK_FALSE(out.attached());
}

TEST_CASE("starting a track reads the tag's header, not its 500 KB cover") {
  MemoryStorage memory;
  const auto tag = testing::id3v2(3, {testing::frame(3, "APIC", testing::apic_payload("image/jpeg", testing::Bytes(500 * 1024, std::byte{1})))});
  auto data = tag;
  testing::append(data, testing::load("sine440_44k.mp3"));
  memory.add("/cover.mp3", std::move(data));
  WatchedStorage storage{memory};
  storage.limit = tag.size();
  ProbeDevice out;
  Player p{storage, "probe"};
  p.play({row("/cover.mp3")}, 0);
  CHECK(p.status().state == State::Playing);
  CHECK(storage.bytes_below_limit < 1024);
}

TEST_CASE("a track the output cannot play is skipped, as an unsupported one is") {
  Rig f{{44100}};
  auto& p = f.player;
  p.play({row("/44k.mp3"), row("/48k.mp3"), row("/44k.mp3")}, 0);
  std::set<std::size_t> seen;
  std::set<std::string> messages;
  for (int i = 0; i < 2000 && p.status().state != State::Stopped; ++i) {
    seen.insert(p.status().index);
    if (!p.status().message.empty()) messages.insert(p.status().message);
    p.poll();
    f.pull(4096);
  }
  CHECK(p.status().state == State::Stopped);
  CHECK(seen == std::set<std::size_t>{0, 2});
  CHECK(messages == std::set<std::string>{"Output cannot play 48000 Hz"});
  CHECK(p.underruns() == 0);
}

TEST_CASE("a queue that ends on a bad track leaves no file open") {
  MemoryStorage memory;
  memory.add("/junk.mp3", random_bytes(8192));
  WatchedStorage storage{memory};
  ProbeDevice out;
  Player p{storage, "probe"};
  p.play({row("/junk.mp3")}, 0);
  CHECK(p.status().state == State::Stopped);
  CHECK(storage.open_files == 0);
}

TEST_CASE("a Xing header's frame count gives a VBR file's duration") {
  const auto frames = testing::be32(1000);
  std::vector<std::byte> fields{std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}};
  testing::append(fields, frames);
  CHECK(first_frame_duration(vbr_file(4 + 32, "Xing", fields)) == doctest::Approx(1000 * 1152 / 44100.0));
}

TEST_CASE("a VBRI header's frame count gives a VBR file's duration") {
  std::vector<std::byte> fields(10);  // version, delay, quality, bytes
  testing::append(fields, testing::be32(1000));
  CHECK(first_frame_duration(vbr_file(36, "VBRI", fields)) == doctest::Approx(1000 * 1152 / 44100.0));
}

TEST_CASE("a Xing header without a frame count falls back to the bitrate") {
  const std::vector<std::byte> no_flags(8);
  CHECK(first_frame_duration(vbr_file(4 + 32, "Xing", no_flags)) == doctest::Approx(21 * 417 * 8 / 128000.0));
}

// What a poll decodes shows in the position once it is played: the ring is drained first, so the
// position advances by exactly the poll's own frames.
TEST_CASE("a poll decodes at most 8 frames") {
  Rig f;
  auto& p = f.player;
  p.play({row("/4s.mp3")}, 0);
  f.pull(100'000);  // drains the ring, so a poll that ignores the cap shows in the count
  const auto before = p.status().position;
  p.poll();  // nothing pulls during it
  f.pull(20'000);
  const auto pushed = p.status().position - before;
  CHECK(pushed > Seconds{0});
  CHECK(pushed < Seconds{0.21});  // 8 frames of 1152 stereo samples at 44.1 kHz
}

// The 4 s track is longer than the 2 s ring, so play() cannot prefill all of it: every step
// below really decodes. Run with kMp3FramesPerPoll at 0, the ring drains and underruns() is not 0.
TEST_CASE("a 5 ms poll cadence keeps up") {
  Rig f;
  auto& p = f.player;
  p.play({row("/4s.mp3")}, 0);
  REQUIRE(p.status().duration > Seconds{3.5});  // 4 s of track behind the filled ring
  REQUIRE(p.status().position == Seconds{0});
  int steps = 0;
  for (; steps < 2000 && p.status().state != State::Stopped; ++steps) {
    p.poll();
    p.poll();
    f.pull(220);  // 5 ms of audio at 44.1 kHz
  }
  CHECK(p.status().state == State::Stopped);
  CHECK(steps > 700);  // the whole 4 s played, not a truncated run
  CHECK(p.underruns() == 0);
}

TEST_CASE("the restart at a format change respects the poll budget") {
  Rig f;
  auto& p = f.player;
  p.play({row("/44k.mp3"), row("/48k.mp3")}, 0);
  f.pull(50'000);  // drains the 1 s track; past its finish(), so no underrun is counted
  REQUIRE(p.status().index == 0);
  p.poll();  // the queue moves on to the 48 kHz track
  CHECK(p.status().index == 1);
  CHECK(p.status().state == State::Playing);
  f.pull(20'000);  // more than the budget holds
  CHECK(p.status().position > Seconds{0});
  CHECK(p.status().position < Seconds{0.21});
  CHECK(f.out.opens() == 2);
}

TEST_CASE("current_track and queue_size follow play and next") {
  Rig f;
  auto& p = f.player;
  CHECK_FALSE(p.current_track().has_value());
  CHECK(p.queue_size() == 0U);
  p.play({row("/44k.mp3"), row("/48k.mp3"), row("/44k.mp3")}, 1);
  REQUIRE(p.current_track().has_value());
  CHECK(p.current_track()->path == "/48k.mp3");
  CHECK(p.queue_size() == 3U);
  p.next();
  REQUIRE(p.current_track().has_value());
  CHECK(p.current_track()->path == "/44k.mp3");
  CHECK(p.status().index == 2);
  CHECK(p.current_track()->title == "t");
}

// Seeking is a byte ratio (exact for a CBR file). The fixture's 4 s file repeats the same second
// of sine, so the audio itself cannot tell a seek apart from a no-op: the position, and the shorter
// run to the end of the track, are what do.
TEST_CASE("seek moves the position and the next audio comes from there") {
  Rig f;
  auto& p = f.player;
  p.play({row("/4s.mp3")}, 0);
  const auto total = p.status().duration;
  REQUIRE(total > Seconds{3.5});
  p.seek(Seconds{2.0});
  CHECK(std::abs(p.status().position.count() - 2.0) < 0.1);
  f.pull(1000);  // 1000 frames: about 23 ms of audio
  CHECK(p.status().position > Seconds{2.0});
  CHECK(p.status().position < Seconds{2.2});
  p.seek(total - Seconds{0.25});  // a quarter of a second left to play
  f.pull(22'050);    // half a second of audio, so the track runs out
  p.poll();
  CHECK(p.status().state == State::Stopped);
}

// A seek at or past the end leaves the track over, exactly as playing to its end does: the next
// poll moves the queue on. Seeking before 0 is a seek to the start.
TEST_CASE("a seek at or past the end ends the track; before 0 it goes to the start") {
  Rig f;
  auto& p = f.player;
  p.play({row("/44k.mp3"), row("/48k.mp3")}, 0);
  REQUIRE(p.status().index == 0);
  p.seek(Seconds{100});
  p.poll();
  CHECK(p.status().index == 1);
  CHECK(p.status().state == State::Playing);
  CHECK(p.status().position < Seconds{0.5});

  p.seek(Seconds{-5});
  CHECK(p.status().position < Seconds{0.1});
  CHECK(p.status().state == State::Playing);
  CHECK(p.status().index == 1);
}

TEST_CASE("seek while paused stays paused") {
  Rig f;
  auto& p = f.player;
  p.play({row("/4s.mp3")}, 0);
  p.toggle_pause();
  REQUIRE(p.status().state == State::Paused);
  p.seek(Seconds{1.5});
  CHECK(p.status().state == State::Paused);
  CHECK(std::abs(p.status().position.count() - 1.5) < 0.1);
}

TEST_CASE("a seek past the end while paused moves to the next track, still paused") {
  Rig f;
  auto& p = f.player;
  p.play({row("/44k.mp3"), row("/48k.mp3")}, 0);
  p.toggle_pause();
  REQUIRE(p.status().state == State::Paused);
  p.seek(Seconds{100});  // past the end of the first track
  CHECK(p.status().index == 1);
  CHECK(p.status().state == State::Paused);  // the queue moves on, nothing starts playing
  CHECK(p.status().position < Seconds{0.5});
  REQUIRE(p.current_track().has_value());
  CHECK(p.current_track()->path == "/48k.mp3");
}

TEST_CASE("seek when stopped does nothing") {
  Rig f;
  auto& p = f.player;
  p.seek(Seconds{5});
  CHECK(p.status().state == State::Stopped);
  CHECK(p.status().position == Seconds{0});
  CHECK(p.queue_size() == 0U);
  CHECK(f.out.opens() == 0);
}
