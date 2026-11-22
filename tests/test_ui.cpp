#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "core/library.hpp"
#include "core/memory_storage.hpp"
#include "core/player.hpp"
#include "core/ui.hpp"
#include "tests/counting_storage.hpp"
#include "tests/tag_builder.hpp"

#include <doctest.h>

#include <prism/app/widget_tree.hpp>
#include <prism/raster/canvas.hpp>
#include <prism/raster/draw_snapshot.hpp>
#include <prism/raster/fonts.hpp>
#include <prism/render/draw_list.hpp>
#include <prism/render/scene_snapshot.hpp>
#include <prism/ui/text_metrics.hpp>
#include <prism/ui/widget_node.hpp>

#include <algorithm>
#include <cstdio>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <utility>

using namespace player;
using namespace player::testing;
using prism::core::Height;
using prism::core::Point;
using prism::core::Rect;
using prism::core::Size;
using prism::core::Width;
using prism::core::X;
using prism::core::Y;

namespace {

class FakeOutput final : public PcmOutput {
public:
  bool accepts(PcmFormat) const override { return true; }
  bool start(PcmFormat, PcmRing&) override { return true; }
  void stop() override {}
  bool connected() const override { return true; }
  std::uint32_t underruns() const override { return 0; }
  void set_volume(float v) override { volume = v; }
  float volume = -1;
};

Bytes load(std::string_view name) {
  std::ifstream in{std::string{TEST_DATA_DIR} + "/" + std::string{name}, std::ios::binary};
  REQUIRE(in);
  std::vector<char> chars{std::istreambuf_iterator<char>{in}, {}};
  const auto* p = reinterpret_cast<const std::byte*>(chars.data());
  return {p, p + chars.size()};
}

void add_song(MemoryStorage& s, std::string path, std::string_view title, std::string_view artist,
              std::string_view album, int track, const Bytes& audio = fake_audio(16)) {
  auto b = id3v2(3, {frame(3, "TIT2", utf8_text(title)), frame(3, "TPE1", utf8_text(artist)),
                     frame(3, "TALB", utf8_text(album)),
                     frame(3, "TRCK", latin1_text(std::to_string(track)))});
  append(b, audio);
  s.add(std::move(path), std::move(b), 1000);
}

void fill_six(MemoryStorage& s) {
  add_song(s, "/music/b/zeta2.mp3", "Second", "Beta Band", "Zeta", 2);
  add_song(s, "/music/b/zeta1.mp3", "First", "Beta Band", "Zeta", 1);
  add_song(s, "/music/b/alpha.mp3", "Only", "Beta Band", "Alpha", 1);
  add_song(s, "/music/a/g3.mp3", "Three", "abba", "Gold", 3);
  add_song(s, "/music/a/g1.mp3", "One", "abba", "Gold", 1);
  add_song(s, "/music/a/g2.mp3", "Two", "abba", "Gold", 2);
}

// 10,000 songs, "Song 00000".."Song 09999", built once for the tests that need them.
MemoryStorage& ten_thousand_songs() {
  static MemoryStorage s = [] {
    MemoryStorage m;
    for (int n = 0; n < 10000; ++n)
      add_song(m, std::format("/music/{:03}/{:05}.mp3", n / 10, n), std::format("Song {:05}", n),
               std::format("Artist {:03}", n / 10), std::format("Album {:03}", n / 10), n % 10 + 1);
    REQUIRE(Library::build(m, "/music", [](int) { return true; }));
    return m;
  }();
  return s;
}

std::unique_ptr<Library> open_library(Storage& s) {
  auto lib = Library::open(s, "/music");
  REQUIRE(lib);
  return lib;
}

// What the UI draws on a 320-wide canvas, 240 high unless told, with no text measurer (0.6 em
// per character).
prism::render::DrawList drawn(PlayerUi& ui, float height = 240) {
  prism::render::DrawList dl;
  prism::ui::WidgetNode node;
  ui.canvas(dl, Rect{Point{X{0}, Y{0}}, Size{Width{320}, Height{height}}}, node);
  return dl;
}

// The texts the UI draws.
std::vector<std::string> texts(PlayerUi& ui, float height = 240) {
  const auto dl = drawn(ui, height);
  std::vector<std::string> out;
  for (const auto& cmd : dl.commands)
    if (const auto* t = std::get_if<prism::render::TextCmd>(&cmd)) out.push_back(t->text);
  return out;
}

// Where the draw list holding a rect of `fill` is on screen: the canvas's place in the window.
Point origin_of_list_with(const prism::render::SceneSnapshot& snap, prism::core::Color fill) {
  for (std::size_t i = 0; i < snap.draw_lists.size() && i < snap.geometry.size(); ++i) {
    if (!snap.draw_lists[i]) continue;
    for (const auto& cmd : snap.draw_lists[i]->commands)
      if (const auto* r = std::get_if<prism::render::FilledRect>(&cmd); r && r->color == fill)
        return snap.geometry[i].second.origin;
  }
  FAIL("no draw list has that fill");
  return {};
}

bool contains(const std::vector<std::string>& texts, std::string_view text) {
  return std::ranges::contains(texts, text);
}

bool shows(PlayerUi& ui, std::string_view text) { return contains(texts(ui), text); }

void press(PlayerUi& ui, std::initializer_list<WheelEvent> events) {
  for (const auto& e : events) ui.handle(e);
}

// The text command drawing `text`, and the clip it is drawn in (none: nullopt).
std::pair<const prism::render::TextCmd*, std::optional<Rect>> text_and_clip(const prism::render::DrawList& dl,
                                                                            std::string_view text) {
  std::optional<Rect> clip;
  for (const auto& cmd : dl.commands) {
    if (const auto* c = std::get_if<prism::render::ClipPush>(&cmd)) clip = c->rect;
    if (std::holds_alternative<prism::render::ClipPop>(cmd)) clip.reset();
    if (const auto* t = std::get_if<prism::render::TextCmd>(&cmd); t && t->text.starts_with(text)) return {t, clip};
  }
  return {nullptr, std::nullopt};
}

float right_of(Rect r) { return r.origin.x.raw() + r.extent.w.raw(); }

// A card whose one file cannot be opened, the way a damaged one behaves: the listing still has it.
class MissingFileStorage final : public Storage {
public:
  MissingFileStorage(Storage& inner, std::string missing) : inner_(inner), missing_(std::move(missing)) {}
  std::vector<DirEntry> list(std::string_view dir) override { return inner_.list(dir); }
  std::unique_ptr<File> open(std::string_view path) override {
    return path == missing_ ? nullptr : inner_.open(path);
  }
  std::unique_ptr<std::ostream> create(std::string_view path) override { return inner_.create(path); }
  bool rename(std::string_view from, std::string_view to) override { return inner_.rename(from, to); }

private:
  Storage& inner_;
  std::string missing_;
};

struct Six {
  MemoryStorage memory;
  CountingStorage storage{memory};
  FakeOutput out;
  Player player{storage, out};
  std::unique_ptr<Library> library;
  Six() {
    fill_six(memory);
    REQUIRE(Library::build(memory, "/music", [](int) { return true; }));
    library = open_library(storage);
  }
};

}  // namespace

TEST_CASE("Select on Music then Artists shows the artists") {
  Six f;
  PlayerUi ui{f.library.get(), f.player};
  CHECK(shows(ui, "Music"));
  press(ui, {Button::Select});
  CHECK(ui.screen_title() == "Music");
  press(ui, {Rotate{1}, Button::Select});
  CHECK(ui.screen_title() == "Artists");
  CHECK(shows(ui, "abba"));
  CHECK(shows(ui, "Beta Band"));

  press(ui, {Button::Select});
  CHECK(ui.screen_title() == "abba");
  CHECK(shows(ui, "Gold"));
  press(ui, {Button::Select});
  CHECK(ui.screen_title() == "Gold");
  CHECK(texts(ui) == std::vector<std::string>{"Gold", "One", "Two", "Three"});
}

// Settings → Output cycles the choices and tells the board, which saves them: the running output
// never changes, so the Settings screen says the choice is for the next start.
TEST_CASE("cycling the output in Settings tells the board, for the next start") {
  Six f;
  PlayerUi ui{f.library.get(), f.player};
  ui.settings.outputs = {"Bluetooth", "Speaker"};
  std::size_t told = 99;
  ui.on_output_changed([&told](std::size_t choice) { told = choice; });

  press(ui, {Rotate{2}, Button::Select});  // Main → Settings, on Output
  CHECK(ui.screen_title() == "Settings");
  CHECK(shows(ui, "The output changes at the next start."));
  CHECK(shows(ui, "Bluetooth"));
  press(ui, {Button::Select});
  CHECK(ui.settings.output == 1);
  CHECK(told == 1);
  CHECK(shows(ui, "Speaker"));
  press(ui, {Button::Select});  // back around
  CHECK(ui.settings.output == 0);
  CHECK(told == 0);
  CHECK(shows(ui, "Bluetooth"));
}

TEST_CASE("Menu goes back") {
  Six f;
  PlayerUi ui{f.library.get(), f.player};
  press(ui, {Button::Select, Rotate{2}, Button::Select});
  CHECK(ui.screen_title() == "Albums");
  press(ui, {Button::Menu});
  CHECK(ui.screen_title() == "Music");
  CHECK(ui.selected() == 2);  // where it was: Folders, Artists, Albums
  press(ui, {Button::Menu});
  CHECK(ui.screen_title() == "Player");
  press(ui, {Button::Menu});
  CHECK(ui.screen_title() == "Player");
}

// The card as it is: folders first, then its .mp3 files, both sorted the way the library sorts,
// with the card's own folders and dot names left out — the same rules the library build walks by.
// A folder row shows its name with a trailing '/', which is also what makes Select enter it.
TEST_CASE("Folders lists subfolders then mp3 files, sorted, skipping system folders") {
  MemoryStorage s;
  s.add("/Album/zeta.mp3", fake_audio(16));
  s.add("/Album/Beta.mp3", fake_audio(16));
  s.add("/Album/alpha.mp3", fake_audio(16));
  s.add("/Album/Gamma.MP3", fake_audio(16));  // upper case: still a song
  s.add("/Album/Sub/deep.mp3", fake_audio(16));
  s.add("/Album/System Volume Information/junk.mp3", fake_audio(16));
  s.add("/Album/$RECYCLE.BIN/junk.mp3", fake_audio(16));
  s.add("/Album/.hidden.mp3", fake_audio(16));
  s.add("/Album/notes.txt", fake_audio(16));
  FakeOutput out;
  Player player{s, out};
  PlayerUi ui{nullptr, player};
  ui.set_storage(&s);

  press(ui, {Button::Select, Button::Select});  // Music → Folders, at the card's root
  CHECK(ui.screen_title() == "Card");
  CHECK(texts(ui) == std::vector<std::string>{"Card", "Album/"});

  press(ui, {Button::Select});
  CHECK(ui.screen_title() == "Album");
  CHECK(texts(ui) ==
        std::vector<std::string>{"Album", "Sub/", "alpha.mp3", "Beta.mp3", "Gamma.MP3", "zeta.mp3"});
}

TEST_CASE("Select enters a folder; Menu goes back and keeps the selection") {
  MemoryStorage s;
  s.add("/Album/x.mp3", fake_audio(16));
  s.add("/Best/y.mp3", fake_audio(16));
  s.add("/a.mp3", fake_audio(16));
  FakeOutput out;
  Player player{s, out};
  PlayerUi ui{nullptr, player};
  ui.set_storage(&s);

  press(ui, {Button::Select, Button::Select});  // Music → Folders, at the root
  REQUIRE(ui.screen_title() == "Card");
  press(ui, {Rotate{1}, Button::Select});  // Best
  CHECK(ui.screen_title() == "Best");
  press(ui, {Button::Menu});
  CHECK(ui.screen_title() == "Card");
  CHECK(ui.selected() == 1);  // still on Best
  CHECK(shows(ui, "Best/"));  // a folder says so with its trailing slash
  press(ui, {Button::Menu});
  CHECK(ui.screen_title() == "Music");
}

TEST_CASE("Select on a file plays its folder from that file") {
  const auto sine = load("sine440_44k.mp3");
  MemoryStorage s;
  add_song(s, "/Album/a.mp3", "A", "Band", "Disc", 1, sine);
  add_song(s, "/Album/b.mp3", "B", "Band", "Disc", 2, sine);
  add_song(s, "/Album/c.mp3", "C", "Band", "Disc", 3, sine);
  add_song(s, "/Album/Sub/d.mp3", "D", "Band", "Disc", 4, sine);
  FakeOutput out;
  Player player{s, out};
  PlayerUi ui{nullptr, player};
  ui.set_storage(&s);

  press(ui, {Button::Select, Button::Select, Button::Select});  // Music → Folders → Album
  REQUIRE(ui.screen_title() == "Album");
  press(ui, {Rotate{2}, Button::Select});  // Sub is the first row, so this is b.mp3
  CHECK(ui.screen_title() == "Now Playing");
  CHECK(player.queue_size() == 3U);  // the folder's own files: not Sub/d.mp3
  CHECK(player.queue().front().title == "A");
  CHECK(player.queue().back().title == "C");
  CHECK(player.status().index == 1);
  CHECK(player.current_track().has_value());
  CHECK(player.current_track()->title == "B");
}

TEST_CASE("a folder of 2,000 files queues a window around the selected file") {
  MemoryStorage memory;
  const auto sine = load("sine440_44k.mp3");
  for (int n = 0; n < 2000; ++n)
    add_song(memory, std::format("/huge/{:04}.mp3", n), std::format("Track {:04}", n), "Band", "Huge", n + 1,
             n == 1000 ? sine : fake_audio(16));
  CountingStorage s{memory};
  FakeOutput out;
  Player player{s, out};
  PlayerUi ui{nullptr, player};
  ui.set_storage(&s);

  press(ui, {Button::Select, Button::Select, Button::Select});  // Music → Folders → huge
  REQUIRE(ui.screen_title() == "huge");
  press(ui, {Rotate{1000}, Button::Select});
  REQUIRE(player.queue_size() == 500);
  CHECK(player.queue().front().title == "0750");  // far from the selection: named by its file
  CHECK(player.queue()[250].title == "Track 1000");  // the selected file, in the middle: tags read
  CHECK(player.queue().back().title == "1249");
  CHECK(player.status().index == 250);
  // The 32 files around the selection are read for their tags, and no file before them is opened at
  // all. Past the window the player opens whatever it chains into to fill its ring, so only the
  // window itself can be pinned down.
  const auto was_opened = [&s](int n) {
    const auto path = std::format("/huge/{:04}.mp3", n);
    return std::ranges::find(s.opened, path) != s.opened.end();
  };
  int opened_before_the_window = 0;
  for (int n = 750; n < 984; ++n) opened_before_the_window += was_opened(n) ? 1 : 0;
  CHECK(opened_before_the_window == 0);
  int opened_for_tags = 0;
  for (int n = 984; n < 1016; ++n) opened_for_tags += was_opened(n) ? 1 : 0;
  CHECK(opened_for_tags == 32);
}

// A folder says it opens with the chevron the other list screens use; a song row has none.
TEST_CASE("a folder row shows the chevron; a file row does not") {
  MemoryStorage s;
  s.add("/Album/x.mp3", fake_audio(16));
  s.add("/Best/y.mp3", fake_audio(16));
  s.add("/a.mp3", fake_audio(16));
  FakeOutput out;
  Player player{s, out};
  PlayerUi ui{nullptr, player};
  ui.set_storage(&s);
  press(ui, {Button::Select, Button::Select});  // Music → Folders: Album/, Best/, a.mp3

  constexpr int W = 320, H = 240;
  prism::app::WidgetTree tree(ui);
  prism::raster::Fonts fonts;
  tree.set_text_measurer(&fonts);
  const auto snap = tree.build_snapshot(W, H, 1);
  REQUIRE(snap);
  std::vector<std::uint32_t> fb(std::size_t{W} * H);
  prism::raster::Canvas canvas{fb, W, H, W};
  canvas.clear(snap->window.background);
  prism::raster::draw(canvas, *snap, fonts);

  const auto at = origin_of_list_with(*snap, PlayerUi::kHighlight);
  const auto pixel = [&](float x, float y) {
    return fb[static_cast<std::size_t>(at.y.raw() + y) * W + static_cast<std::size_t>(at.x.raw() + x)];
  };
  // The chevron is the only ink at the right end of a row, past the clipped label. Rows 1 and 2 are
  // unselected (row 0 holds the highlight), so their background is white.
  const auto ink_beside_row = [&](std::size_t row) {
    int found = 0;
    const float centre =
        PlayerUi::kTitleHeight + static_cast<float>(row) * PlayerUi::kRowHeight + PlayerUi::kRowHeight / 2;
    for (float x = 288; x < 314; ++x)
      for (float y = centre - 8; y < centre + 8; ++y)
        if (pixel(x, y) != 0xFFFFFFFFu) ++found;
    return found;
  };
  CHECK(ink_beside_row(1) > 0);   // Best/, a subfolder
  CHECK(ink_beside_row(2) == 0);  // a.mp3, a file
}

TEST_CASE("with a card but no library, Music shows Folders and why there is no music") {
  MemoryStorage s;
  FakeOutput out;
  Player player{s, out};
  PlayerUi ui{nullptr, player};
  ui.set_storage(&s);
  press(ui, {Button::Select});
  CHECK(ui.screen_title() == "Music");
  CHECK(shows(ui, "Folders"));
  CHECK(shows(ui, "No music"));

  ui.set_library_status("Updating library…");
  CHECK(shows(ui, "Folders"));
  CHECK(shows(ui, "Updating library…"));
}

TEST_CASE("a file that cannot be opened keeps its place in the queue") {
  MemoryStorage memory;
  const auto sine = load("sine440_44k.mp3");
  add_song(memory, "/Album/a.mp3", "A", "Band", "Album", 1, sine);
  add_song(memory, "/Album/b.mp3", "B", "Band", "Album", 2, sine);
  add_song(memory, "/Album/c.mp3", "C", "Band", "Album", 3, sine);
  MissingFileStorage s{memory, "/Album/a.mp3"};
  FakeOutput out;
  Player player{s, out};
  PlayerUi ui{nullptr, player};
  ui.set_storage(&s);

  press(ui, {Button::Select, Button::Select, Button::Select, Rotate{2}, Button::Select});
  REQUIRE(ui.screen_title() == "Now Playing");
  CHECK(player.queue_size() == 3U);            // a.mp3 is still in it, so the numbering holds
  CHECK(player.queue().front().title == "a");  // named by file: its tags could not be read
  CHECK(player.status().index == 2);
  REQUIRE(player.current_track().has_value());
  CHECK(player.current_track()->title == "C");
}

TEST_CASE("rotate past the end stops at the last row") {
  Six f;
  PlayerUi ui{f.library.get(), f.player};
  press(ui, {Button::Select, Rotate{1}, Button::Select, Rotate{5}});
  CHECK(ui.selected() == 1);
  press(ui, {Rotate{-9}});
  CHECK(ui.selected() == 0);
  press(ui, {Button::Menu, Rotate{7}});
  CHECK(ui.selected() == 3);  // the last of Folders, Artists, Albums, Songs
}

TEST_CASE("10,000 songs: 9,999 steps reach the last song and only visible rows were read") {
  CountingStorage storage{ten_thousand_songs()};
  const auto library = open_library(storage);
  const auto index_size = storage.bytes_read;
  FakeOutput out;
  Player player{storage, out};
  PlayerUi ui{library.get(), player};
  press(ui, {Button::Select, Rotate{3}, Button::Select});
  REQUIRE(ui.screen_title() == "Songs");

  storage.bytes_read = 0;
  for (int i = 0; i < 9999; ++i) ui.handle(Rotate{1});
  CHECK(ui.selected() == 9999);
  const auto shown = texts(ui);
  CHECK(contains(shown, "Song 09999"));
  CHECK(contains(shown, "Song 09992"));
  CHECK_FALSE(contains(shown, "Song 09991"));
  std::printf("index %zu bytes; scrolling to the end and drawing read %zu bytes\n", index_size,
              storage.bytes_read);
  CHECK(storage.bytes_read <= 16 * 1024);
  CHECK(index_size >= 16 * storage.bytes_read);
}

TEST_CASE("a taller screen fills its height with rows, and scrolls by what it shows") {
  CountingStorage storage{ten_thousand_songs()};
  const auto library = open_library(storage);
  FakeOutput out;
  Player player{storage, out};
  PlayerUi ui{library.get(), player};
  press(ui, {Button::Select, Rotate{3}, Button::Select});
  REQUIRE(ui.screen_title() == "Songs");

  const float height = PlayerUi::kTitleHeight + 11 * PlayerUi::kRowHeight + 10;  // 11 rows fit
  auto shown = texts(ui, height);
  CHECK(contains(shown, "Song 00010"));
  CHECK_FALSE(contains(shown, "Song 00011"));

  press(ui, {Rotate{10}});
  CHECK(contains(texts(ui, height), "Song 00000"));  // the 11th row is on screen: no scroll yet
  press(ui, {Rotate{1}});
  shown = texts(ui, height);
  CHECK(contains(shown, "Song 00011"));
  CHECK_FALSE(contains(shown, "Song 00000"));
}

TEST_CASE("a screen drawn shorter keeps the selected row on it") {
  CountingStorage storage{ten_thousand_songs()};
  const auto library = open_library(storage);
  FakeOutput out;
  Player player{storage, out};
  PlayerUi ui{library.get(), player};
  press(ui, {Button::Select, Rotate{3}, Button::Select});
  const float tall = PlayerUi::kTitleHeight + 11 * PlayerUi::kRowHeight;
  texts(ui, tall);
  press(ui, {Rotate{10}});
  CHECK(contains(texts(ui, 240), "Song 00010"));  // 8 rows now: the view follows the selection
}

TEST_CASE("Select on a song plays the album from that song") {
  Six f;
  PlayerUi ui{f.library.get(), f.player};
  press(ui, {Button::Select, Rotate{2}, Button::Select, Rotate{1}, Button::Select});
  REQUIRE(ui.screen_title() == "Gold");
  f.storage.opened.clear();
  press(ui, {Rotate{1}, Button::Select});
  CHECK(ui.screen_title() == "Now Playing");
  // The fake audio does not decode: the player tries each track left in its queue.
  CHECK(f.storage.opened == std::vector<std::string>{"/music/a/g2.mp3", "/music/a/g3.mp3"});
}

TEST_CASE("Select in Songs queues 500 songs, up to 250 of them before the selected one") {
  CountingStorage storage{ten_thousand_songs()};
  const auto library = open_library(storage);
  FakeOutput out;
  Player player{storage, out};
  PlayerUi ui{library.get(), player};
  press(ui, {Button::Select, Rotate{3}, Button::Select, Rotate{100}});
  storage.opened.clear();
  press(ui, {Button::Select});
  // The fake audio does not decode: the player tries the selected song and each one after it.
  REQUIRE(storage.opened.size() == 400);
  CHECK(storage.opened.front() == "/music/010/00100.mp3");
  CHECK(storage.opened.back() == "/music/049/00499.mp3");
  CHECK(player.queue().front().title == "Song 00000");
}

TEST_CASE("Select in a 2,000-song album queues 500 songs around the selected one and plays it") {
  const auto sine = load("sine440_44k.mp3");
  MemoryStorage memory;
  for (int n = 0; n < 2000; ++n)
    add_song(memory, std::format("/music/huge/{:04}.mp3", n), std::format("Track {:04}", n), "Band", "Huge", n + 1, sine);
  REQUIRE(Library::build(memory, "/music", [](int) { return true; }));
  const auto library = open_library(memory);
  FakeOutput out;
  Player player{memory, out};
  PlayerUi ui{library.get(), player};
  press(ui, {Button::Select, Rotate{2}, Button::Select, Button::Select, Rotate{1000}});
  REQUIRE(ui.screen_title() == "Huge");
  press(ui, {Button::Select});
  REQUIRE(player.queue().size() == 500);
  CHECK(player.queue().front().title == "Track 0750");
  CHECK(player.queue().back().title == "Track 1249");
  REQUIRE(player.status().state == Player::Status::State::Playing);
  CHECK(player.queue()[player.status().index].title == "Track 1000");
}

TEST_CASE("Previous from the 5th song of a 12-song album plays the 4th") {
  const auto sine = load("sine440_44k.mp3");
  MemoryStorage memory;
  for (int n = 1; n <= 12; ++n)
    add_song(memory, std::format("/music/twelve/{:02}.mp3", n), std::format("Track {:02}", n), "Band", "Twelve", n, sine);
  REQUIRE(Library::build(memory, "/music", [](int) { return true; }));
  const auto library = open_library(memory);
  FakeOutput out;
  Player player{memory, out};
  PlayerUi ui{library.get(), player};
  press(ui, {Button::Select, Rotate{2}, Button::Select, Button::Select, Rotate{4}, Button::Select});
  REQUIRE(ui.screen_title() == "Now Playing");
  REQUIRE(player.queue()[player.status().index].title == "Track 05");
  press(ui, {Button::Previous});
  REQUIRE(player.status().state == Player::Status::State::Playing);
  CHECK(player.queue()[player.status().index].title == "Track 04");
}

TEST_CASE("no library: the Music screen says No music") {
  MemoryStorage s;
  FakeOutput out;
  Player player{s, out};
  PlayerUi ui{nullptr, player};
  press(ui, {Button::Select});
  CHECK(ui.screen_title() == "Music");
  CHECK(shows(ui, "No music"));
  press(ui, {Button::Select});
  CHECK(ui.screen_title() == "Music");
}

TEST_CASE("an empty card's library: the Music screen says No music") {
  MemoryStorage s;
  REQUIRE(Library::build(s, "/music", [](int) { return true; }));
  const auto library = open_library(s);
  FakeOutput out;
  Player player{s, out};
  PlayerUi ui{library.get(), player};
  press(ui, {Button::Select});
  CHECK(ui.screen_title() == "Music");
  CHECK(shows(ui, "No music"));
  CHECK_FALSE(shows(ui, "Artists"));
  press(ui, {Button::Select});
  CHECK(ui.screen_title() == "Music");
}

TEST_CASE("while the library is built, the Music screen says so instead of No music") {
  MemoryStorage s;
  FakeOutput out;
  Player player{s, out};
  PlayerUi ui{nullptr, player};
  ui.set_library_status("Updating library…");
  press(ui, {Button::Select});
  CHECK(shows(ui, "Updating library…"));
  CHECK_FALSE(shows(ui, "No music"));
  ui.set_library_status({});
  CHECK(shows(ui, "No music"));
}

TEST_CASE("while a stale library is rebuilt, its screens stay browsable and say so in the title bar") {
  Six f;
  PlayerUi ui{f.library.get(), f.player};
  ui.set_library_status("Updating library…");
  press(ui, {Button::Select});
  CHECK(shows(ui, "Updating library…"));
  CHECK(shows(ui, "Artists"));
  press(ui, {Rotate{1}, Button::Select});
  CHECK(ui.screen_title() == "Artists");
  CHECK(shows(ui, "Updating library…"));
  CHECK(shows(ui, "abba"));
  press(ui, {Button::Menu, Button::Menu});
  CHECK_FALSE(shows(ui, "Updating library…"));  // Main is no library screen

  ui.set_library(f.library.get());  // the new library handed over
  press(ui, {Button::Select});
  CHECK_FALSE(shows(ui, "Updating library…"));
  CHECK(shows(ui, "Music"));
}

TEST_CASE("a new library leaves its lists for Main") {
  Six f;
  PlayerUi ui{f.library.get(), f.player};
  press(ui, {Button::Select, Rotate{1}, Button::Select});
  REQUIRE(ui.screen_title() == "Artists");
  ui.set_library(nullptr);
  CHECK(ui.screen_title() == "Player");
  press(ui, {Button::Select});
  CHECK(shows(ui, "No music"));
}

TEST_CASE("in Now Playing, the wheel sets the volume by 5% a step") {
  Six f;
  PlayerUi ui{f.library.get(), f.player};
  press(ui, {Rotate{1}, Button::Select});
  REQUIRE(ui.screen_title() == "Now Playing");
  press(ui, {Rotate{2}});
  CHECK(f.out.volume == doctest::Approx(0.6));
  press(ui, {Rotate{-20}});
  CHECK(f.out.volume == doctest::Approx(0.0));
}

TEST_CASE("drawing a list highlights the selected row") {
  Six f;
  PlayerUi ui{f.library.get(), f.player};
  press(ui, {Button::Select, Rotate{1}, Button::Select, Rotate{1}});
  REQUIRE(ui.selected() == 1);

  constexpr int W = 320, H = 240;
  prism::app::WidgetTree tree(ui);
  prism::raster::Fonts fonts;
  tree.set_text_measurer(&fonts);
  const auto snap = tree.build_snapshot(W, H, 1);
  REQUIRE(snap);
  std::vector<std::uint32_t> fb(std::size_t{W} * H);
  prism::raster::Canvas canvas{fb, W, H, W};
  canvas.clear(snap->window.background);
  prism::raster::draw(canvas, *snap, fonts);

  const auto at = origin_of_list_with(*snap, PlayerUi::kHighlight);
  const auto pixel = [&](float x, float y) {
    return fb[static_cast<std::size_t>(at.y.raw() + y) * W + static_cast<std::size_t>(at.x.raw() + x)];
  };
  const float row0 = PlayerUi::kTitleHeight + PlayerUi::kRowHeight / 2;
  CHECK(pixel(4, row0) == 0xFFFFFFFFu);                            // white row
  CHECK(pixel(4, row0 + PlayerUi::kRowHeight) == 0xFF2F6FD0u);     // the highlight bar
  CHECK(pixel(4, row0 + 2 * PlayerUi::kRowHeight) == 0xFFFFFFFFu);
}

TEST_CASE("the title bar has a battery left of the play state") {
  Six f;
  PlayerUi ui{f.library.get(), f.player};
  prism::app::WidgetTree tree(ui);
  prism::raster::Fonts fonts;
  tree.set_text_measurer(&fonts);
  constexpr int W = 320, H = 240;
  const auto snap = tree.build_snapshot(W, H, 1);
  REQUIRE(snap);
  std::vector<std::uint32_t> fb(std::size_t{W} * H);
  prism::raster::Canvas canvas{fb, W, H, W};
  canvas.clear(snap->window.background);
  prism::raster::draw(canvas, *snap, fonts);

  const auto b = PlayerUi::kBattery;
  const std::uint32_t battery = 0xFF000000u | (b.r << 16) | (b.g << 8) | b.b;
  const auto at = origin_of_list_with(*snap, PlayerUi::kTitleBar);
  int found = 0;
  for (int y = 0; y < static_cast<int>(PlayerUi::kTitleHeight); ++y)
    for (int x = W / 2; x < W; ++x)
      found += fb[static_cast<std::size_t>(at.y.raw() + y) * W + static_cast<std::size_t>(at.x.raw() + x)] == battery;
  CHECK(found >= 20);
}

TEST_CASE("the Albums view shows each album's artist") {
  Six f;
  PlayerUi ui{f.library.get(), f.player};
  press(ui, {Button::Select, Rotate{2}, Button::Select});
  REQUIRE(ui.screen_title() == "Albums");
  CHECK(shows(ui, "abba"));
  CHECK(shows(ui, "Beta Band"));
  const auto dl = drawn(ui);
  const auto [artist, clip] = text_and_clip(dl, "abba");
  REQUIRE(artist);
  REQUIRE(clip);
  // right-aligned: it ends where its box ends, before the chevron
  CHECK(right_of(*clip) <= 320 - 24);
  CHECK(artist->origin.x.raw() + prism::ui::text_width(nullptr, "abba", artist->size) ==
        doctest::Approx(right_of(*clip)));
}

TEST_CASE("a very long title is cut before the battery") {
  MemoryStorage s;
  const std::string album(120, 'A');
  add_song(s, "/music/x.mp3", "Song", "Someone", album, 1);
  REQUIRE(Library::build(s, "/music", [](int) { return true; }));
  const auto library = open_library(s);
  FakeOutput out;
  Player player{s, out};
  PlayerUi ui{library.get(), player};
  press(ui, {Button::Select, Rotate{2}, Button::Select, Button::Select});
  REQUIRE(ui.screen_title() == album);
  const auto dl = drawn(ui);
  const auto [title, clip] = text_and_clip(dl, album);
  REQUIRE(title);
  REQUIRE(clip);
  CHECK(right_of(*clip) <= 320 - 50);  // the battery starts 50 px from the right
}

TEST_CASE("the remaining time ends at the progress bar's right edge") {
  MemoryStorage s;
  add_song(s, "/music/sine.mp3", "Sine", "Tone", "Tests", 1, load("sine440_44k.mp3"));
  REQUIRE(Library::build(s, "/music", [](int) { return true; }));
  const auto library = open_library(s);
  FakeOutput out;
  Player player{s, out};
  PlayerUi ui{library.get(), player};
  press(ui, {Button::Select, Rotate{3}, Button::Select, Button::Select});
  REQUIRE(ui.screen_title() == "Now Playing");
  REQUIRE(player.status().state == Player::Status::State::Playing);
  const auto dl = drawn(ui);
  const auto [remaining, clip] = text_and_clip(dl, "-");
  REQUIRE(remaining);
  CHECK(remaining->origin.x.raw() + prism::ui::text_width(nullptr, remaining->text, remaining->size) ==
        doctest::Approx(320 - 14));  // the bar has a 14 px margin
}

TEST_CASE("Now Playing with a stopped player says Nothing playing and why") {
  Six f;
  PlayerUi ui{f.library.get(), f.player};
  press(ui, {Button::Select, Rotate{2}, Button::Select, Button::Select});
  REQUIRE(ui.screen_title() == "Alpha");
  press(ui, {Button::Select});
  REQUIRE(ui.screen_title() == "Now Playing");
  REQUIRE(f.player.status().state == Player::Status::State::Stopped);
  CHECK(shows(ui, "Nothing playing"));
  CHECK(shows(ui, "Cannot decode alpha.mp3"));
  CHECK_FALSE(shows(ui, "Only"));
}
