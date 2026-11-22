#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "core/fold.hpp"
#include "core/library.hpp"
#include "core/memory_storage.hpp"
#include "tests/tag_builder.hpp"

#include <doctest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <malloc.h>
#include <new>

using namespace player;
using namespace player::testing;

namespace {

std::atomic<long long> live_heap_bytes{0};

}  // namespace

// Counts the bytes alive on the heap, to show the Library keeps no per-track container.
void* operator new(std::size_t n) {
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc{};
  live_heap_bytes += static_cast<long long>(malloc_usable_size(p));
  return p;
}
void operator delete(void* p) noexcept {
  if (!p) return;
  live_heap_bytes -= static_cast<long long>(malloc_usable_size(p));
  std::free(p);
}
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }

namespace {

Bytes song(std::string_view title, std::string_view artist, std::string_view album, int track) {
  auto b = id3v2(3, {frame(3, "TIT2", utf8_text(title)), frame(3, "TPE1", utf8_text(artist)),
                     frame(3, "TALB", utf8_text(album)),
                     frame(3, "TRCK", latin1_text(std::to_string(track)))});
  append(b, fake_audio(16));
  return b;
}

void add_song(MemoryStorage& s, std::string path, std::string_view title, std::string_view artist,
              std::string_view album, int track) {
  s.add(std::move(path), song(title, artist, album, track), 1000);
}

const auto keep_going = [](int) { return true; };

void fill_six(MemoryStorage& s) {
  add_song(s, "/music/b/zeta2.mp3", "Second", "Beta Band", "Zeta", 2);
  add_song(s, "/music/b/zeta1.mp3", "First", "Beta Band", "Zeta", 1);
  add_song(s, "/music/b/alpha.mp3", "Only", "Beta Band", "Alpha", 1);
  add_song(s, "/music/a/g3.mp3", "Three", "abba", "Gold", 3);
  add_song(s, "/music/a/g1.mp3", "One", "abba", "Gold", 1);
  add_song(s, "/music/a/g2.mp3", "Two", "abba", "Gold", 2);
}

std::unique_ptr<Library> built(Storage& s) {
  REQUIRE(Library::build(s, "/music", keep_going));
  auto lib = Library::open(s, "/music");
  REQUIRE(lib);
  return lib;
}

std::vector<std::string> titles(const std::vector<TrackRow>& rows) {
  std::vector<std::string> out;
  for (const auto& r : rows) out.push_back(r.title);
  return out;
}

using Names = std::vector<std::string>;

Bytes read_all(Storage& s, std::string_view path) {
  auto f = s.open(path);
  REQUIRE(f);
  Bytes b(f->size());
  REQUIRE(f->read(0, b) == b.size());
  return b;
}

class CountingFile final : public File {
public:
  CountingFile(std::unique_ptr<File> inner, std::size_t& bytes) : inner_(std::move(inner)), bytes_(bytes) {}
  std::uint64_t size() const override { return inner_->size(); }
  std::size_t read(std::uint64_t offset, std::span<std::byte> out) override {
    const auto n = inner_->read(offset, out);
    bytes_ += n;
    return n;
  }

private:
  std::unique_ptr<File> inner_;
  std::size_t& bytes_;
};

class CountingStorage final : public Storage {
public:
  explicit CountingStorage(Storage& inner) : inner_(inner) {}
  std::vector<DirEntry> list(std::string_view dir) override { return inner_.list(dir); }
  std::unique_ptr<File> open(std::string_view path) override {
    auto f = inner_.open(path);
    return f ? std::make_unique<CountingFile>(std::move(f), bytes_read) : nullptr;
  }
  std::unique_ptr<std::ostream> create(std::string_view path) override { return inner_.create(path); }
  bool rename(std::string_view from, std::string_view to) override { return inner_.rename(from, to); }
  std::size_t bytes_read = 0;

private:
  Storage& inner_;
};

void fill_generated(MemoryStorage& s, int artists, int per_artist) {
  for (int a = 0; a < artists; ++a)
    for (int t = 0; t < per_artist; ++t) {
      const int n = a * per_artist + t;
      add_song(s, std::format("/music/{:03}/{:05}.mp3", a, n), std::format("Song {:05}", n),
               std::format("Artist {:03}", a), std::format("Album {:03}", a), t + 1);
    }
}

}  // namespace

TEST_CASE("build then open lists artists, their albums and songs in order") {
  MemoryStorage s;
  fill_six(s);
  const auto lib = built(s);

  CHECK(lib->count({View::Artists}) == 2);
  CHECK(lib->names({View::Artists}, 0, 10) == Names{"abba", "Beta Band"});
  CHECK(lib->names({View::Albums}, 0, 10) == Names{"Alpha", "Gold", "Zeta"});
  CHECK(lib->count({View::AlbumsOfArtist, "Beta Band"}) == 2);
  CHECK(lib->names({View::AlbumsOfArtist, "Beta Band"}, 0, 10) == Names{"Alpha", "Zeta"});
  CHECK(lib->names({View::AlbumsOfArtist, "Beta Band"}, 1, 10) == Names{"Zeta"});

  const ViewKey gold{View::SongsOfAlbum, "abba", "Gold"};
  CHECK(lib->count(gold) == 3);
  const auto rows = lib->tracks(gold, 0, 10);
  CHECK(titles(rows) == Names{"One", "Two", "Three"});
  CHECK(rows[0].path == "/music/a/g1.mp3");
  CHECK(rows[0].artist == "abba");
  CHECK(rows[0].album == "Gold");
  CHECK(rows[2].track == 3);

  CHECK(titles(lib->tracks({View::SongsOfAlbum, "", "Zeta"}, 0, 10)) == Names{"First", "Second"});
  CHECK(lib->count({View::Songs}) == 6);
  CHECK(titles(lib->tracks({View::Songs}, 0, 10)) ==
        Names{"First", "One", "Only", "Second", "Three", "Two"});
  CHECK(titles(lib->tracks({View::Songs}, 4, 10)) == Names{"Three", "Two"});
  CHECK(lib->count({View::AlbumsOfArtist, "Nobody"}) == 0);
  CHECK(lib->names({View::AlbumsOfArtist, "Nobody"}, 0, 10).empty());
  CHECK(lib->tracks({View::SongsOfAlbum, "Nobody", "Gold"}, 0, 10).empty());
  CHECK(lib->tracks({View::SongsOfAlbum, "", "Nothing"}, 0, 10).empty());
  CHECK(lib->tracks({View::SongsOfAlbum, "abba", "Zeta"}, 0, 10).empty());
}

TEST_CASE("accented names sort with their base letter") {
  MemoryStorage s;
  add_song(s, "/music/1.mp3", "a", "Fred", "x", 1);
  add_song(s, "/music/2.mp3", "b", "\xc3\x89milie", "y", 1);
  add_song(s, "/music/3.mp3", "c", "Eddy", "z", 1);
  CHECK(built(s)->names({View::Artists}, 0, 10) == Names{"Eddy", "\xc3\x89milie", "Fred"});
}

TEST_CASE("the sort fold lowercases ASCII and drops Latin accents") {
  CHECK(fold_for_sort("\xc3\x89t\xc3\xa9") == "ete");                // Été
  CHECK(fold_for_sort("\xc3\x98resund Stra\xc3\x9f" "e") == "oresund strasse");  // Øresund Straße
  CHECK(fold_for_sort("\xc5\x81\xc3\xb3" "d\xc5\xba") == "lodz");    // Łódź
  CHECK(fold_for_sort("\xc3\x97") == "\xc3\x97");                    // × stays
  CHECK(fold_for_sort("\xe6\x97\xa5") == "\xe6\x97\xa5");            // 日 stays
}

TEST_CASE("a track without artist or album sorts last with an empty name") {
  MemoryStorage s;
  add_song(s, "/music/1.mp3", "a", "Zed", "Zoo", 1);
  s.add("/music/untagged.mp3", fake_audio(64), 1000);
  const auto lib = built(s);
  CHECK(lib->names({View::Artists}, 0, 10) == Names{"Zed", ""});
  CHECK(lib->names({View::Albums}, 0, 10) == Names{"Zoo", ""});
  const auto rows = lib->tracks({View::SongsOfAlbum, "", ""}, 0, 10);
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].title == "untagged");
}

TEST_CASE("no /music or an empty one: count is 0 everywhere and open after build works") {
  MemoryStorage none;
  MemoryStorage empty;
  empty.add("/music/readme.txt", bytes_of("hi"));
  for (Storage* s : {static_cast<Storage*>(&none), static_cast<Storage*>(&empty)}) {
    const auto lib = built(*s);
    for (const auto v : {View::Artists, View::Albums, View::Songs, View::AlbumsOfArtist,
                         View::SongsOfAlbum})
      CHECK(lib->count({v, "a", "b"}) == 0);
    CHECK(lib->names({View::Artists}, 0, 10).empty());
    CHECK(lib->tracks({View::Songs}, 0, 10).empty());
  }
}

TEST_CASE("open without an index returns null") {
  MemoryStorage s;
  fill_six(s);
  CHECK(Library::open(s, "/music") == nullptr);
}

TEST_CASE("a changed card makes open return null") {
  MemoryStorage s;
  fill_six(s);
  REQUIRE(Library::build(s, "/music", keep_going));
  add_song(s, "/music/new.mp3", "New", "abba", "Gold", 4);
  CHECK(Library::open(s, "/music") == nullptr);
}

TEST_CASE("a truncated index makes open return null") {
  MemoryStorage s;
  fill_six(s);
  REQUIRE(Library::build(s, "/music", keep_going));
  const auto index = read_all(s, "/music/.prism-library");
  for (const std::size_t cut : {std::size_t{0}, std::size_t{3}, std::size_t{8}, std::size_t{39},
                                std::size_t{40}, std::size_t{100}, index.size() / 2,
                                index.size() - 1}) {
    s.add("/music/.prism-library", Bytes(index.begin(), index.begin() + static_cast<long>(cut)));
    CHECK_MESSAGE(Library::open(s, "/music") == nullptr, "cut at ", cut);
  }
}

TEST_CASE("a damaged index never crashes a query") {
  MemoryStorage s;
  fill_six(s);
  REQUIRE(Library::build(s, "/music", keep_going));
  auto index = read_all(s, "/music/.prism-library");
  std::fill(index.begin() + 40, index.end(), std::byte{0xff});
  s.add("/music/.prism-library", index);
  const auto lib = Library::open(s, "/music");
  REQUIRE(lib);
  for (const auto v : {View::Artists, View::Albums, View::Songs, View::AlbumsOfArtist,
                       View::SongsOfAlbum}) {
    const ViewKey key{v, "abba", "Gold"};
    CHECK_NOTHROW(lib->count(key));
    CHECK_NOTHROW(lib->names(key, 0, 10));
    CHECK_NOTHROW(lib->tracks(key, 0, 10));
  }
  index[0] = std::byte{'X'};
  s.add("/music/.prism-library", index);
  CHECK(Library::open(s, "/music") == nullptr);
}

TEST_CASE("10,000 tracks: rows come from the index, not from memory") {
  MemoryStorage s;
  fill_generated(s, 1000, 10);

  const auto start = std::chrono::steady_clock::now();
  REQUIRE(Library::build(s, "/music", keep_going));
  const auto build_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);

  const auto before = live_heap_bytes.load();
  auto lib = Library::open(s, "/music");
  const auto held = live_heap_bytes.load() - before;
  REQUIRE(lib);
  std::printf("10,000-track build: %lld ms; heap held by the open Library: %lld bytes\n",
              static_cast<long long>(build_ms.count()), held);
  CHECK(held < 64 * 1024);

  CHECK(lib->count({View::Songs}) == 10000);
  CHECK(lib->count({View::Artists}) == 1000);
  const auto last = lib->tracks({View::Songs}, 9990, 10);
  REQUIRE(last.size() == 10);
  for (int i = 0; i < 10; ++i) CHECK(last[i].title == std::format("Song {:05}", 9990 + i));
  CHECK(last[9].artist == "Artist 999");

  CountingStorage counting{s};
  const auto paged = Library::open(counting, "/music");
  REQUIRE(paged);
  counting.bytes_read = 0;
  CHECK(paged->tracks({View::Songs}, 9990, 10).size() == 10);
  CHECK(counting.bytes_read <= 64 * 1024);
  counting.bytes_read = 0;
  CHECK(paged->names({View::AlbumsOfArtist, "Artist 500"}, 0, 10) == Names{"Album 500"});
  CHECK(paged->tracks({View::SongsOfAlbum, "Artist 500", "Album 500"}, 0, 20).size() == 10);
  CHECK(counting.bytes_read <= 64 * 1024);
}

TEST_CASE("a stopped build keeps the old index") {
  MemoryStorage s;
  fill_generated(s, 15, 10);
  REQUIRE(Library::build(s, "/music", keep_going));

  for (int i = 0; i < 100; ++i)
    add_song(s, std::format("/music/more/{}.mp3", i), "More", "Later", "Later", 1);
  int calls = 0;
  CHECK_FALSE(Library::build(s, "/music", [&](int done) {
    ++calls;
    CHECK(done == 100);
    return false;
  }));
  CHECK(calls == 1);
  for (int i = 0; i < 100; ++i)
    REQUIRE(s.rename(std::format("/music/more/{}.mp3", i), std::format("/away/{}.mp3", i)));

  const auto lib = Library::open(s, "/music");
  REQUIRE(lib);
  CHECK(lib->count({View::Songs}) == 150);
}
