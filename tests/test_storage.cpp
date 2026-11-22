#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "core/memory_storage.hpp"
#include "desktop/dir_storage.hpp"

#include <doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <ostream>

using namespace player;

namespace {

std::vector<std::byte> bytes(std::size_t n) {
  std::vector<std::byte> v(n);
  for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<std::byte>(i);
  return v;
}

const DirEntry* find(const std::vector<DirEntry>& es, std::string_view name) {
  const auto it = std::ranges::find(es, name, &DirEntry::name);
  return it == es.end() ? nullptr : &*it;
}

void check_lists_files_and_dirs(Storage& s) {
  const auto es = s.list("/music");
  REQUIRE(es.size() == 3);
  const auto* a = find(es, "a.mp3");
  REQUIRE(a);
  CHECK_FALSE(a->is_dir);
  CHECK(a->size == 5);
  const auto* x = find(es, "x");
  REQUIRE(x);
  CHECK(x->is_dir);
}

void check_reads_at_offset(Storage& s) {
  auto f = s.open("/music/ten.bin");
  REQUIRE(f);
  CHECK(f->size() == 10);
  std::vector<std::byte> out(4);
  CHECK(f->read(8, out) == 2);
  CHECK(out[0] == std::byte{8});
  CHECK(f->read(10, out) == 0);
}

void check_missing(Storage& s) {
  CHECK(s.list("/nope").empty());
  CHECK(s.open("/nope/a.mp3") == nullptr);
}

void check_create_then_open(Storage& s) {
  {
    auto out = s.create("/index.bin");
    REQUIRE(out);
    *out << "hello";
  }
  auto f = s.open("/index.bin");
  REQUIRE(f);
  std::vector<std::byte> out(8);
  CHECK(f->read(0, out) == 5);
  CHECK(out[1] == std::byte{'e'});
}

void fill(MemoryStorage& s) {
  s.add("/music/a.mp3", bytes(5));
  s.add("/music/x/b.mp3", bytes(3));
  s.add("/music/ten.bin", bytes(10));
}

struct TempDir {
  std::filesystem::path path = std::filesystem::temp_directory_path() / "player_storage_test";
  TempDir() {
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path / "music" / "x");
  }
  ~TempDir() { std::filesystem::remove_all(path); }
  void write(const std::filesystem::path& rel, const std::vector<std::byte>& data) const {
    std::ofstream(path / rel, std::ios::binary)
        .write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  }
};

void fill(const TempDir& d) {
  d.write("music/a.mp3", bytes(5));
  d.write("music/x/b.mp3", bytes(3));
  d.write("music/ten.bin", bytes(10));
}

}  // namespace

TEST_CASE("MemoryStorage lists a directory's files and subdirectories") {
  MemoryStorage s;
  fill(s);
  check_lists_files_and_dirs(s);
}

TEST_CASE("MemoryStorage reads at an offset and stops at the end") {
  MemoryStorage s;
  fill(s);
  check_reads_at_offset(s);
}

TEST_CASE("a missing path lists empty and opens null") {
  MemoryStorage s;
  check_missing(s);
}

TEST_CASE("DirStorage sees the same tree as the files on disk") {
  TempDir d;
  fill(d);
  DirStorage s{d.path};
  check_lists_files_and_dirs(s);
  check_reads_at_offset(s);
  check_missing(s);
}

TEST_CASE("create then open reads back what was written, in memory") {
  MemoryStorage s;
  check_create_then_open(s);
}

TEST_CASE("create then open reads back what was written, on disk") {
  TempDir d;
  DirStorage s{d.path};
  check_create_then_open(s);
}
