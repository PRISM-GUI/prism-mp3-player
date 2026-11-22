#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "core/audio.hpp"
#include "core/memory_storage.hpp"
#include "core/pcm_ring.hpp"
#include "core/player.hpp"
#include "tests/tag_builder.hpp"

#include <doctest.h>

#include <algorithm>
#include <cmath>
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
  bool accepts(PcmFormat format) const override { return rates.contains(format.sample_rate); }
  bool start(PcmFormat format, PcmRing& ring) override {
    if (start_fails || !accepts(format)) return false;
    format_ = format;
    ring_ = &ring;
    queued_at_start = ring.size();
    ++starts;
    return true;
  }
  void stop() override { ring_ = nullptr; }
  bool connected() const override { return is_connected && (!connects_on_start || ring_ != nullptr); }
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
  std::size_t buffered() const { return ring_ ? ring_->size() : 0; }
  PcmFormat format() const { return format_; }

  std::set<int> rates{44100, 48000};
  bool start_fails = false;
  bool is_connected = true;
  bool connects_on_start = false;  // a sink that reports connected() only after start()
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

// Counts the files open and the bytes read below `limit` in them.
class WatchedStorage final : public Storage {
public:
  explicit WatchedStorage(Storage& inner) : inner_(inner) {}
  std::vector<DirEntry> list(std::string_view dir) override { return inner_.list(dir); }
  std::unique_ptr<File> open(std::string_view path) override {
    auto f = inner_.open(path);
    return f ? std::make_unique<WatchedFile>(std::move(f), *this) : nullptr;
  }
  std::unique_ptr<std::ostream> create(std::string_view path) override { return inner_.create(path); }
  bool rename(std::string_view from, std::string_view to) override { return inner_.rename(from, to); }

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
  Storage& inner_;
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
  return d.duration_s();
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

// The contract Player relies on: an output reports connected() before it is asked to start(), so
// the sink must be open before the first track is chosen. One that connects only in start() never
// gets there: play() pauses with the disconnect message instead.
TEST_CASE("an output that connects only in start() never plays") {
  Fixture f;
  f.out.connects_on_start = true;
  Player p{f.storage, f.out};
  p.play({row("/44k.mp3")}, 0);
  CHECK(p.status().state == State::Paused);
  CHECK(p.status().message == "Headset disconnected");
  CHECK_FALSE(f.out.started());
  CHECK(f.out.starts == 0);
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
  f.out.start_fails = true;
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

TEST_CASE("starting a track reads the tag's header, not its 500 KB cover") {
  MemoryStorage memory;
  const auto tag = testing::id3v2(3, {testing::frame(3, "APIC", testing::apic_payload("image/jpeg", testing::Bytes(500 * 1024, std::byte{1})))});
  auto data = tag;
  testing::append(data, load("sine440_44k.mp3"));
  memory.add("/cover.mp3", std::move(data));
  WatchedStorage storage{memory};
  storage.limit = tag.size();
  FakeOutput out;
  Player p{storage, out};
  p.play({row("/cover.mp3")}, 0);
  CHECK(p.status().state == State::Playing);
  CHECK(storage.bytes_below_limit < 1024);
}

TEST_CASE("a track the output cannot play is skipped, as an unsupported one is") {
  Fixture f;
  f.out.rates = {44100};
  Player p{f.storage, f.out};
  p.play({row("/44k.mp3"), row("/48k.mp3"), row("/44k.mp3")}, 0);
  std::set<std::size_t> seen;
  std::set<std::string> messages;
  for (int i = 0; i < 2000 && p.status().state != State::Stopped; ++i) {
    seen.insert(p.status().index);
    if (!p.status().message.empty()) messages.insert(p.status().message);
    p.poll();
    f.out.pull(4096);
  }
  CHECK(p.status().state == State::Stopped);
  CHECK(seen == std::set<std::size_t>{0, 2});
  CHECK(messages == std::set<std::string>{"Output cannot play 48000 Hz"});
  CHECK(f.out.underruns() == 0);
}

TEST_CASE("a queue that ends on a bad track leaves no file open") {
  MemoryStorage memory;
  memory.add("/junk.mp3", random_bytes(8192));
  WatchedStorage storage{memory};
  FakeOutput out;
  Player p{storage, out};
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

TEST_CASE("a poll decodes at most 8 frames") {
  Fixture f;
  Player p{f.storage, f.out};
  p.play({row("/4s.mp3")}, 0);
  f.out.pull(20'000);  // makes room, so a poll that ignores the cap shows in the count
  const auto before = f.out.buffered();
  p.poll();  // nothing pulls during it
  const auto pushed = f.out.buffered() - before;
  CHECK(pushed > 0);
  CHECK(pushed <= 8 * 1152 * 2);  // 8 frames of 1152 stereo samples
}

// The 4 s track is longer than the 2 s ring, so play() cannot prefill all of it: every step
// below really decodes. Run with kFramesPerPoll at 0, the ring drains and underruns() is not 0.
TEST_CASE("a 5 ms poll cadence keeps up") {
  Fixture f;
  Player p{f.storage, f.out};
  p.play({row("/4s.mp3")}, 0);
  REQUIRE(f.out.buffered() > 190'000);  // a nearly full ring, and 4 s of track behind it
  int steps = 0;
  for (; steps < 2000 && p.status().state != State::Stopped; ++steps) {
    p.poll();
    p.poll();
    f.out.pull(220);  // 5 ms of audio at 44.1 kHz
  }
  CHECK(p.status().state == State::Stopped);
  CHECK(steps > 700);  // the whole 4 s played, not a truncated run
  CHECK(f.out.underruns() == 0);
}

TEST_CASE("the restart at a format change respects the poll budget") {
  Fixture f;
  Player p{f.storage, f.out};
  p.play({row("/44k.mp3"), row("/48k.mp3")}, 0);
  f.out.pull(50'000);  // drains the 1 s track; past its finish(), so no underrun is counted
  REQUIRE(f.out.buffered() == 0);
  p.poll();  // the queue moves on to the 48 kHz track
  CHECK(p.status().index == 1);
  CHECK(p.status().state == State::Playing);
  CHECK(f.out.buffered() > 0);
  CHECK(f.out.buffered() <= 8 * 1152 * 2);
}

TEST_CASE("current_track and queue_size follow play and next") {
  Fixture f;
  Player p{f.storage, f.out};
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
  Fixture f;
  Player p{f.storage, f.out};
  p.play({row("/4s.mp3")}, 0);
  const auto total = p.status().duration_s;
  REQUIRE(total > 3.5);
  p.seek(2.0);
  CHECK(std::abs(p.status().position_s - 2.0) < 0.1);
  f.out.pull(1000);  // 1000 frames: about 23 ms of audio
  CHECK(p.status().position_s > 2.0);
  CHECK(p.status().position_s < 2.2);
  p.seek(total - 0.25);  // a quarter of a second left to play
  f.out.pull(22'050);    // half a second of audio, so the track runs out
  p.poll();
  CHECK(p.status().state == State::Stopped);
}

// A seek at or past the end leaves the track over, exactly as playing to its end does: the next
// poll moves the queue on. Seeking before 0 is a seek to the start.
TEST_CASE("a seek at or past the end ends the track; before 0 it goes to the start") {
  Fixture f;
  Player p{f.storage, f.out};
  p.play({row("/44k.mp3"), row("/48k.mp3")}, 0);
  REQUIRE(p.status().index == 0);
  p.seek(100.0);
  p.poll();
  CHECK(p.status().index == 1);
  CHECK(p.status().state == State::Playing);
  CHECK(p.status().position_s < 0.5);

  p.seek(-5.0);
  CHECK(p.status().position_s < 0.1);
  CHECK(p.status().state == State::Playing);
  CHECK(p.status().index == 1);
}

TEST_CASE("seek while paused stays paused") {
  Fixture f;
  Player p{f.storage, f.out};
  p.play({row("/4s.mp3")}, 0);
  p.toggle_pause();
  REQUIRE(p.status().state == State::Paused);
  p.seek(1.5);
  CHECK(p.status().state == State::Paused);
  CHECK(std::abs(p.status().position_s - 1.5) < 0.1);
}

TEST_CASE("seek when stopped does nothing") {
  Fixture f;
  Player p{f.storage, f.out};
  p.seek(5.0);
  CHECK(p.status().state == State::Stopped);
  CHECK(p.status().position_s == 0.0);
  CHECK(p.queue_size() == 0U);
  CHECK(f.out.starts == 0);
}
