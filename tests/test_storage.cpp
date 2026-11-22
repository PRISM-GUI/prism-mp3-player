#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "core/memory_storage.hpp"
#include "desktop/dir_storage.hpp"
#include "tests/temp_dir.hpp"

#include <doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
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

void check_rename(Storage& s) {
  { *s.create("/music/a.tmp") << "new"; }
  { *s.create("/music/a.idx") << "old"; }
  CHECK(s.rename("/music/a.tmp", "/music/a.idx"));
  CHECK(s.open("/music/a.tmp") == nullptr);
  auto f = s.open("/music/a.idx");
  REQUIRE(f);
  std::vector<std::byte> out(8);
  CHECK(f->read(0, out) == 3);
  CHECK(out[0] == std::byte{'n'});
  CHECK_FALSE(s.rename("/music/missing", "/music/b"));
}

void write_text(Storage& s, std::string_view path, std::string_view text) {
  auto out = s.create(path);
  REQUIRE(out);
  *out << text;
}

void fill(MemoryStorage& s) {
  s.add("/music/a.mp3", bytes(5));
  s.add("/music/x/b.mp3", bytes(3));
  s.add("/music/ten.bin", bytes(10));
}

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

TEST_CASE("DirStorage skips entries it cannot stat, such as a FIFO") {
  TempDir d;
  d.write("music/a.mp3", bytes(5));
  REQUIRE(mkfifo((d.path / "music" / "pipe").c_str(), 0600) == 0);
  DirStorage s{d.path};
  std::vector<DirEntry> es;
  CHECK_NOTHROW(es = s.list("/music"));
  CHECK(find(es, "a.mp3"));
  CHECK_FALSE(find(es, "pipe"));
}

TEST_CASE("rename replaces the target, in memory") {
  MemoryStorage s;
  check_rename(s);
}

TEST_CASE("rename replaces the target, on disk") {
  TempDir d;
  DirStorage s{d.path};
  check_rename(s);
}

TEST_CASE("DirStorage rename replaces an existing target") {
  TempDir d;
  DirStorage s{d.path};
  write_text(s, "/a.bin", "aaa");
  write_text(s, "/b.bin", "bbb");
  CHECK(s.rename("/a.bin", "/b.bin"));
  CHECK(s.open("/a.bin") == nullptr);
  auto f = s.open("/b.bin");
  REQUIRE(f);
  CHECK(f->size() == 3);
  std::vector<std::byte> out(8);
  CHECK(f->read(0, out) == 3);
  CHECK(out[0] == std::byte{'a'});
  CHECK(out[1] == std::byte{'a'});
  CHECK(out[2] == std::byte{'a'});
}

TEST_CASE("a reader opened before rename keeps reading the old bytes, on disk") {
  TempDir d;
  DirStorage s{d.path};
  write_text(s, "/a.bin", "old");
  write_text(s, "/b.bin", "new");
  auto reader = s.open("/a.bin");
  REQUIRE(reader);
  CHECK(s.rename("/a.bin", "/b.bin"));
  std::vector<std::byte> out(8);
  CHECK(reader->read(0, out) == 3);
  CHECK(out[0] == std::byte{'o'});
  CHECK(out[1] == std::byte{'l'});
  CHECK(out[2] == std::byte{'d'});
}
