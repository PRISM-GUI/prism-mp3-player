#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "core/cover_loader.hpp"
#include "core/memory_storage.hpp"
#include "tests/counting_storage.hpp"
#include "tests/tag_builder.hpp"

#include <doctest.h>
#include <prism/render/draw_list.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace player;
using namespace player::testing;

namespace {

constexpr int kMaxSide = 100;  // what these tests ask the loader for

std::shared_ptr<const prism::render::Image> unit_image() {
  static const auto image = [] {
    const auto made = prism::render::Image::from_rgba(1, 1, {255, 0, 0, 255});
    REQUIRE(made.has_value());
    return *made;
  }();
  return image;
}

// A song with `cover` as its APIC picture, or no APIC frame at all when `cover` is empty.
Bytes song(std::string_view title, const Bytes& cover = {}) {
  std::vector<Bytes> frames{frame(3, "TIT2", utf8_text(title))};
  if (!cover.empty()) frames.push_back(frame(3, "APIC", apic_payload("image/png", cover)));
  auto body = id3v2(3, frames);
  append(body, fake_audio(16));
  return body;
}

struct Call {
  Bytes bytes;
  int max_side = 0;
};

// Stands in for the board's JPEG decoder: records what it was handed and answers with a 1x1 image,
// or with nothing when told to refuse. While held, a call records itself and then waits, so a test
// decides when each picture is decoded instead of racing the loader's thread.
class FakeDecoder {
 public:
  std::shared_ptr<const prism::render::Image> operator()(std::span<const std::byte> bytes, int max_side) {
    bool refuse = false;
    {
      std::unique_lock lock(mutex_);
      calls_.push_back({Bytes{bytes.begin(), bytes.end()}, max_side});
      refuse = std::exchange(refuse_once_, false) || refuses_;
      wake_.notify_all();  // wakes wait_for_calls: "a picture is in here"
      wake_.wait(lock, [this] { return !held_; });
    }
    return refuse ? nullptr : unit_image();
  }

  void refuse() {
    const std::lock_guard lock(mutex_);
    refuses_ = true;
  }

  // Refuses the next picture only: the tag's, while the folder's is fine.
  void refuse_next() {
    const std::lock_guard lock(mutex_);
    refuse_once_ = true;
  }

  // Holds the next call inside the decoder; release() lets the blocked one finish.
  void hold() {
    const std::lock_guard lock(mutex_);
    held_ = true;
  }

  void release() {
    {
      const std::lock_guard lock(mutex_);
      held_ = false;
    }
    wake_.notify_all();
  }

  // Waits until `count` pictures have reached the decoder: fails the wait rather than hang.
  bool wait_for_calls(std::size_t count) {
    std::unique_lock lock(mutex_);
    return wake_.wait_for(lock, std::chrono::seconds{2}, [this, count] { return calls_.size() >= count; });
  }

  std::vector<Call> calls() const {
    const std::lock_guard lock(mutex_);
    return calls_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::vector<Call> calls_;
  bool refuses_ = false;
  bool refuse_once_ = false;
  bool held_ = false;
};

CoverDecoder decoding_into(FakeDecoder& decoder) {
  return [&decoder](std::span<const std::byte> bytes, int max_side) { return decoder(bytes, max_side); };
}

// The loader works on its own thread: wait until it reports a finished load instead of sleeping a
// fixed time, and fail the wait rather than hang when it never answers.
bool wait_for_load(CoverLoader& loader, std::uint64_t was) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
  while (std::chrono::steady_clock::now() < deadline) {
    if (loader.revision() != was) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  return false;
}

}  // namespace

// The loader keeps one cover: asking again for the track it already holds must also drop whatever
// was queued behind it, or that older request loads afterwards and takes the slot.
TEST_CASE("asking again for the loaded track cancels a pending other one") {
  MemoryStorage storage;
  const auto picture_a = bytes_of("picture A");
  const auto picture_c = bytes_of("picture C");
  const auto picture_d = bytes_of("picture D");
  storage.add("/music/album/a.mp3", song("A", picture_a), 1000);
  storage.add("/music/album/b.mp3", song("B", bytes_of("picture B")), 1000);
  storage.add("/music/album/c.mp3", song("C", picture_c), 1000);
  storage.add("/music/album/d.mp3", song("D", picture_d), 1000);
  FakeDecoder decoder;
  CoverLoader loader{storage, decoding_into(decoder), kMaxSide};

  loader.request("/music/album/a.mp3");
  REQUIRE(wait_for_load(loader, 0));

  decoder.hold();
  loader.request("/music/album/c.mp3");
  REQUIRE(decoder.wait_for_calls(2));  // the loader is inside C's picture, and stays there
  loader.request("/music/album/b.mp3");  // queued behind C
  loader.request("/music/album/a.mp3");  // already loaded: this must drop the queued B

  decoder.release();
  REQUIRE(wait_for_load(loader, 1));  // C is done

  loader.request("/music/album/d.mp3");
  REQUIRE(decoder.wait_for_calls(3));
  const auto calls = decoder.calls();
  REQUIRE(calls.size() >= 3U);
  CHECK(calls[2].bytes == picture_d);  // B was never decoded
}

TEST_CASE("a track's cover reaches the decoder byte for byte, at the size asked") {
  MemoryStorage storage;
  const auto cover = bytes_of("PNG-ish bytes, exactly these");
  storage.add("/music/a.mp3", song("A", cover), 1000);
  FakeDecoder decoder;
  CoverLoader loader{storage, decoding_into(decoder), kMaxSide};

  loader.request("/music/a.mp3");
  REQUIRE(wait_for_load(loader, 0));

  const auto calls = decoder.calls();
  REQUIRE(calls.size() == 1U);
  CHECK(calls[0].bytes == cover);
  CHECK(calls[0].max_side == kMaxSide);
  const auto loaded = loader.cover("/music/a.mp3");
  REQUIRE(loaded.has_value());
  REQUIRE(*loaded);
  CHECK((*loaded)->width() == 1);
}

TEST_CASE("a track without a cover loads as null without calling the decoder") {
  MemoryStorage storage;
  storage.add("/music/b.mp3", song("B"), 1000);
  FakeDecoder decoder;
  CoverLoader loader{storage, decoding_into(decoder), kMaxSide};

  loader.request("/music/b.mp3");
  REQUIRE(wait_for_load(loader, 0));

  CHECK(decoder.calls().empty());
  const auto loaded = loader.cover("/music/b.mp3");
  REQUIRE(loaded.has_value());
  CHECK_FALSE(*loaded);
}

TEST_CASE("a decoder that refuses gives null") {
  MemoryStorage storage;
  storage.add("/music/c.mp3", song("C", bytes_of("a picture nothing can decode")), 1000);
  FakeDecoder decoder;
  decoder.refuse();
  CoverLoader loader{storage, decoding_into(decoder), kMaxSide};

  loader.request("/music/c.mp3");
  REQUIRE(wait_for_load(loader, 0));

  CHECK(decoder.calls().size() == 1U);
  const auto loaded = loader.cover("/music/c.mp3");
  REQUIRE(loaded.has_value());
  CHECK_FALSE(*loaded);
}

TEST_CASE("a cover over 4 MiB is not read") {
  MemoryStorage storage;
  const auto huge = Bytes(kMaxCoverBytes + 1, std::byte{0x5a});
  storage.add("/music/d.mp3", song("D", huge), 1000);
  FakeDecoder decoder;
  CoverLoader loader{storage, decoding_into(decoder), kMaxSide};

  loader.request("/music/d.mp3");
  REQUIRE(wait_for_load(loader, 0));

  CHECK(decoder.calls().empty());
  const auto loaded = loader.cover("/music/d.mp3");
  REQUIRE(loaded.has_value());
  CHECK_FALSE(*loaded);
}

TEST_CASE("a missing file gives null") {
  MemoryStorage storage;
  FakeDecoder decoder;
  CoverLoader loader{storage, decoding_into(decoder), kMaxSide};

  loader.request("/music/nothing-here.mp3");
  REQUIRE(wait_for_load(loader, 0));

  CHECK(decoder.calls().empty());
  const auto loaded = loader.cover("/music/nothing-here.mp3");
  REQUIRE(loaded.has_value());
  CHECK_FALSE(*loaded);
}

TEST_CASE("the latest request wins: the earlier track's cover is not what is kept") {
  MemoryStorage storage;
  storage.add("/music/a.mp3", song("A", bytes_of("first picture")), 1000);
  storage.add("/music/b.mp3", song("B", bytes_of("second picture")), 1000);
  FakeDecoder decoder;
  CoverLoader loader{storage, decoding_into(decoder), kMaxSide};

  loader.request("/music/a.mp3");
  REQUIRE(wait_for_load(loader, 0));
  REQUIRE(loader.cover("/music/a.mp3").has_value());

  const auto first_revision = loader.revision();
  loader.request("/music/b.mp3");
  REQUIRE(wait_for_load(loader, first_revision));
  CHECK(loader.revision() > first_revision);

  const auto kept = loader.cover("/music/b.mp3");
  REQUIRE(kept.has_value());
  REQUIRE(*kept);
  CHECK_FALSE(loader.cover("/music/a.mp3").has_value());
}

TEST_CASE("no tag cover: the folder's cover.jpg is used") {
  MemoryStorage storage;
  const auto picture = bytes_of("the album's own cover");
  storage.add("/music/album/a.mp3", song("A"), 1000);
  storage.add("/music/album/cover.jpg", picture, 1000);
  FakeDecoder decoder;
  CoverLoader loader{storage, decoding_into(decoder), kMaxSide};

  loader.request("/music/album/a.mp3");
  REQUIRE(wait_for_load(loader, 0));

  const auto calls = decoder.calls();
  REQUIRE(calls.size() == 1U);
  CHECK(calls[0].bytes == picture);
  CHECK(calls[0].max_side == kMaxSide);
  const auto loaded = loader.cover("/music/album/a.mp3");
  REQUIRE(loaded.has_value());
  REQUIRE(*loaded);
}

TEST_CASE("a tag cover the decoder refuses falls back to the folder's picture") {
  MemoryStorage storage;
  const auto folder_picture = bytes_of("the folder's picture");
  storage.add("/music/album/a.mp3", song("A", bytes_of("a tag picture nothing decodes")), 1000);
  storage.add("/music/album/cover.jpg", folder_picture, 1000);
  FakeDecoder decoder;
  decoder.refuse_next();  // the tag's picture only
  CoverLoader loader{storage, decoding_into(decoder), kMaxSide};

  loader.request("/music/album/a.mp3");
  REQUIRE(wait_for_load(loader, 0));

  const auto calls = decoder.calls();
  REQUIRE(calls.size() == 2U);
  CHECK(calls[1].bytes == folder_picture);
  const auto loaded = loader.cover("/music/album/a.mp3");
  REQUIRE(loaded.has_value());
  REQUIRE(*loaded);
}

TEST_CASE("Folder.PNG is preferred over another picture in the folder") {
  MemoryStorage storage;
  const auto preferred = bytes_of("Folder.PNG");
  storage.add("/music/album/a.mp3", song("A"), 1000);
  storage.add("/music/album/aardvark.jpg", bytes_of("some other picture"), 1000);
  storage.add("/music/album/Folder.PNG", preferred, 1000);
  FakeDecoder decoder;
  CoverLoader loader{storage, decoding_into(decoder), kMaxSide};

  loader.request("/music/album/a.mp3");
  REQUIRE(wait_for_load(loader, 0));

  const auto calls = decoder.calls();
  REQUIRE(calls.size() == 1U);
  CHECK(calls[0].bytes == preferred);
}

TEST_CASE("a folder with no picture gives null") {
  MemoryStorage storage;
  storage.add("/music/album/a.mp3", song("A"), 1000);
  storage.add("/music/album/notes.txt", bytes_of("not a picture"), 1000);
  FakeDecoder decoder;
  CoverLoader loader{storage, decoding_into(decoder), kMaxSide};

  loader.request("/music/album/a.mp3");
  REQUIRE(wait_for_load(loader, 0));

  CHECK(decoder.calls().empty());
  const auto loaded = loader.cover("/music/album/a.mp3");
  REQUIRE(loaded.has_value());
  CHECK_FALSE(*loaded);
}

TEST_CASE("the next track of the same folder does not read the picture again") {
  MemoryStorage memory;
  CountingStorage storage{memory};
  memory.add("/music/album/a.mp3", song("A"), 1000);
  memory.add("/music/album/b.mp3", song("B"), 1000);
  memory.add("/music/album/cover.jpg", bytes_of("the album's own cover"), 1000);
  FakeDecoder decoder;
  CoverLoader loader{storage, decoding_into(decoder), kMaxSide};

  loader.request("/music/album/a.mp3");
  REQUIRE(wait_for_load(loader, 0));
  REQUIRE(std::ranges::count(storage.opened, std::string{"/music/album/cover.jpg"}) == 1);

  const auto first_revision = loader.revision();
  loader.request("/music/album/b.mp3");
  REQUIRE(wait_for_load(loader, first_revision));

  CHECK(std::ranges::count(storage.opened, std::string{"/music/album/cover.jpg"}) == 1);
  const auto loaded = loader.cover("/music/album/b.mp3");
  REQUIRE(loaded.has_value());
  REQUIRE(*loaded);
}
