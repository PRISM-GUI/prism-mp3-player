#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "core/memory_storage.hpp"
#include "core/tags.hpp"
#include "tests/tag_builder.hpp"

#include <doctest.h>

using namespace player;
using namespace player::testing;

namespace {

class BoundsCheckedFile final : public File {
public:
  explicit BoundsCheckedFile(Bytes data) {
    storage_.add("/a.mp3", std::move(data));
    inner_ = storage_.open("/a.mp3");
  }
  std::uint64_t size() const override { return inner_->size(); }
  std::size_t read(std::uint64_t offset, std::span<std::byte> out) override {
    if (offset + out.size() > size()) ++out_of_bounds;
    return inner_->read(offset, out);
  }
  int out_of_bounds = 0;

private:
  MemoryStorage storage_;
  std::unique_ptr<File> inner_;
};

Bytes concat(std::initializer_list<Bytes> parts) {
  Bytes b;
  for (const auto& p : parts) append(b, p);
  return b;
}

Tags tags_of(const Bytes& data, std::string_view name = "song.mp3") {
  BoundsCheckedFile f{data};
  const auto t = read_tags(f, name);
  CHECK(f.out_of_bounds == 0);
  return t;
}

}  // namespace

TEST_CASE("ID3v2.3 with UTF-16 text") {
  const auto tag = id3v2(3, {frame(3, "TIT2", text_payload(1, utf16_with_bom(u"Été"))),
                             frame(3, "TPE1", latin1_text("Artist")),
                             frame(3, "TALB", latin1_text("Album")),
                             frame(3, "TRCK", latin1_text("3/12"))});
  const auto t = tags_of(concat({tag, fake_audio(100)}));
  CHECK(t.title == "\xc3\x89t\xc3\xa9");
  CHECK(t.artist == "Artist");
  CHECK(t.album == "Album");
  CHECK(t.track == 3);
  CHECK(t.audio_offset == tag.size());
}

TEST_CASE("ID3v2.4 with UTF-8 text and a synchsafe frame size") {
  const std::string long_title(200, 'x');
  const auto tag = id3v2(4, {frame(4, "TIT2", utf8_text(long_title)),
                             frame(4, "TPE1", utf8_text("Bj\xc3\xb6rk"))}, 16);
  const auto t = tags_of(concat({tag, fake_audio(100)}));
  CHECK(t.title == long_title);
  CHECK(t.artist == "Bj\xc3\xb6rk");
  CHECK(t.audio_offset == tag.size());
}

TEST_CASE("ID3v1 only") {
  const auto t = tags_of(concat({fake_audio(300), id3v1("Old Title", "Old Artist", "Old Album", 7)}));
  CHECK(t.title == "Old Title");
  CHECK(t.artist == "Old Artist");
  CHECK(t.album == "Old Album");
  CHECK(t.track == 7);
  CHECK(t.audio_offset == 0);
}

TEST_CASE("no tags: the title is the file name without .mp3") {
  const auto t = tags_of(fake_audio(300), "01 Intro.mp3");
  CHECK(t.title == "01 Intro");
  CHECK(t.artist.empty());
  CHECK(t.album.empty());
  CHECK(t.cover.size == 0);
}

TEST_CASE("a file shorter than any header") {
  CHECK(tags_of(Bytes{std::byte{'I'}, std::byte{'D'}}, "x.mp3").title == "x");
  CHECK(tags_of(Bytes{}, "y.mp3").title == "y");
}

TEST_CASE("APIC gives the cover's offset, size and mime") {
  const Bytes jpeg{std::byte{0xff}, std::byte{0xd8}, std::byte{1}, std::byte{2}, std::byte{3},
                   std::byte{0xff}, std::byte{0xd9}};
  const auto tag = id3v2(3, {frame(3, "TIT2", latin1_text("T")),
                             frame(3, "APIC", apic_payload("image/jpeg", jpeg))});
  const auto data = concat({tag, fake_audio(50)});
  BoundsCheckedFile f{data};
  const auto t = read_tags(f, "x.mp3");
  CHECK(t.cover.mime == "image/jpeg");
  REQUIRE(t.cover.size == jpeg.size());
  Bytes got(t.cover.size);
  CHECK(f.read(t.cover.offset, got) == got.size());
  CHECK(got == jpeg);
}

TEST_CASE("a tag whose size runs past the end of the file") {
  const auto body = frame(3, "TIT2", latin1_text("Hidden"));
  const auto data = id3v2_with_size(3, body, 100000);
  const auto t = tags_of(data, "short.mp3");
  CHECK(t.title == "short");
  CHECK(t.audio_offset == data.size());
}

TEST_CASE("a frame whose size runs past the tag") {
  Bytes bad = bytes_of("TIT2");
  append(bad, be32(5000));
  append(bad, Bytes{std::byte{0}, std::byte{0}, std::byte{0}});
  const auto tag = id3v2(3, {bad});
  CHECK(tags_of(concat({tag, fake_audio(10)}), "f.mp3").title == "f");
}

TEST_CASE("a frame with an unknown text encoding byte is skipped") {
  const auto tag = id3v2(3, {frame(3, "TIT2", text_payload(9, bytes_of("Nope"))),
                             frame(3, "TPE1", latin1_text("Kept"))});
  const auto t = tags_of(concat({tag, fake_audio(10)}), "u.mp3");
  CHECK(t.title == "u");
  CHECK(t.artist == "Kept");
}

TEST_CASE("an extended header is skipped") {
  Bytes ext = be32(6);
  append(ext, Bytes(6, std::byte{0}));
  const auto frames = frame(3, "TIT2", latin1_text("After ext"));
  const auto data = id3v2_with_size(3, concat({ext, frames}), ext.size() + frames.size(), 0x40);
  CHECK(tags_of(concat({data, fake_audio(10)})).title == "After ext");
}

TEST_CASE("text stops at the first NUL and trailing NULs are dropped") {
  const auto tag = id3v2(3, {frame(3, "TIT2", latin1_text(std::string("A\0B", 3)))});
  CHECK(tags_of(concat({tag, fake_audio(10)})).title == "A");
}

TEST_CASE("invalid UTF-8 in a UTF-8 frame never reaches the strings") {
  const auto tag = id3v2(4, {frame(4, "TIT2", utf8_text("a\xff" "b"))});
  const auto t = tags_of(concat({tag, fake_audio(10)}));
  CHECK(t.title == "a\xc3\xbf" "b");
}

TEST_CASE("an ID3v2.2 tag is not parsed but the audio offset is past it") {
  const auto data = concat({id3v2_with_size(2, Bytes(20, std::byte{'x'}), 20), fake_audio(50)});
  const auto t = tags_of(data, "old.mp3");
  CHECK(t.audio_offset == 30);
  CHECK(t.title == "old");
}

TEST_CASE("overlong and surrogate UTF-8 are not valid UTF-8 and fall back to Latin-1") {
  const auto tag = id3v2(4, {frame(4, "TIT2", utf8_text("a\xc0\x80" "b")),
                             frame(4, "TPE1", utf8_text("a\xed\xa0\x80" "b"))});
  const auto t = tags_of(concat({tag, fake_audio(10)}));
  CHECK(t.title == "a\xc3\x80\xc2\x80" "b");
  CHECK(t.artist == "a\xc3\xad\xc2\xa0\xc2\x80" "b");
}

TEST_CASE("a file name that is not UTF-8 is converted too") {
  CHECK(tags_of(fake_audio(10), "\xe9t\xe9.mp3").title == "\xc3\xa9t\xc3\xa9");
}

TEST_CASE("a text field is capped at 256 bytes on a code point boundary") {
  std::string euros;
  for (int i = 0; i < 300; ++i) euros += "\xe2\x82\xac";
  const auto tag = id3v2(4, {frame(4, "TIT2", utf8_text(euros))});
  const auto t = tags_of(concat({tag, fake_audio(10)}));
  CHECK(t.title.size() == 255);
  CHECK(t.title == euros.substr(0, 255));
}

TEST_CASE("an ID3v2.4 tag with plain (not synchsafe) frame sizes, as old iTunes wrote") {
  const auto tag = id3v2(4, {frame(3, "TIT2", latin1_text(std::string(260, 'x'))),
                             frame(4, "TALB", latin1_text("Album"))});
  const auto t = tags_of(concat({tag, fake_audio(10)}));
  CHECK(t.title == std::string(256, 'x'));
  CHECK(t.album == "Album");
}
