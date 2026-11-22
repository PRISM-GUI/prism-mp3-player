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

// What the UI draws on its 320×240 canvas, with no text measurer (0.6 em per character).
prism::render::DrawList drawn(PlayerUi& ui) {
  prism::render::DrawList dl;
  prism::ui::WidgetNode node;
  ui.canvas(dl, Rect{Point{X{0}, Y{0}}, Size{Width{320}, Height{240}}}, node);
  return dl;
}

// The texts the UI draws.
std::vector<std::string> texts(PlayerUi& ui) {
  const auto dl = drawn(ui);
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
  press(ui, {Button::Select});
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

TEST_CASE("Menu goes back") {
  Six f;
  PlayerUi ui{f.library.get(), f.player};
  press(ui, {Button::Select, Rotate{1}, Button::Select});
  CHECK(ui.screen_title() == "Albums");
  press(ui, {Button::Menu});
  CHECK(ui.screen_title() == "Music");
  CHECK(ui.selected() == 1);  // where it was
  press(ui, {Button::Menu});
  CHECK(ui.screen_title() == "Player");
  press(ui, {Button::Menu});
  CHECK(ui.screen_title() == "Player");
}

TEST_CASE("rotate past the end stops at the last row") {
  Six f;
  PlayerUi ui{f.library.get(), f.player};
  press(ui, {Button::Select, Button::Select, Rotate{5}});
  CHECK(ui.selected() == 1);
  press(ui, {Rotate{-9}});
  CHECK(ui.selected() == 0);
  press(ui, {Button::Menu, Rotate{7}});
  CHECK(ui.selected() == 2);
}

TEST_CASE("10,000 songs: 9,999 steps reach the last song and only visible rows were read") {
  CountingStorage storage{ten_thousand_songs()};
  const auto library = open_library(storage);
  const auto index_size = storage.bytes_read;
  FakeOutput out;
  Player player{storage, out};
  PlayerUi ui{library.get(), player};
  press(ui, {Button::Select, Rotate{2}, Button::Select});
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

TEST_CASE("Select on a song plays the album from that song") {
  Six f;
  PlayerUi ui{f.library.get(), f.player};
  press(ui, {Button::Select, Rotate{1}, Button::Select, Rotate{1}, Button::Select});
  REQUIRE(ui.screen_title() == "Gold");
  f.storage.opened.clear();
  press(ui, {Rotate{1}, Button::Select});
  CHECK(ui.screen_title() == "Now Playing");
  // The fake audio does not decode: the player tries each track left in its queue.
  CHECK(f.storage.opened == std::vector<std::string>{"/music/a/g2.mp3", "/music/a/g3.mp3"});
}

TEST_CASE("Select in Songs queues at most 500 songs from the selected one") {
  CountingStorage storage{ten_thousand_songs()};
  const auto library = open_library(storage);
  FakeOutput out;
  Player player{storage, out};
  PlayerUi ui{library.get(), player};
  press(ui, {Button::Select, Rotate{2}, Button::Select, Rotate{100}});
  storage.opened.clear();
  press(ui, {Button::Select});
  REQUIRE(storage.opened.size() == 500);
  CHECK(storage.opened.front() == "/music/010/00100.mp3");
  CHECK(storage.opened.back() == "/music/059/00599.mp3");
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
  press(ui, {Button::Select});
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
  press(ui, {Button::Select, Button::Select});
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
  press(ui, {Button::Select, Button::Select, Rotate{1}});
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
  press(ui, {Button::Select, Rotate{1}, Button::Select});
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
  press(ui, {Button::Select, Rotate{1}, Button::Select, Button::Select});
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
  press(ui, {Button::Select, Rotate{2}, Button::Select, Button::Select});
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
  press(ui, {Button::Select, Rotate{1}, Button::Select, Button::Select});
  REQUIRE(ui.screen_title() == "Alpha");
  press(ui, {Button::Select});
  REQUIRE(ui.screen_title() == "Now Playing");
  REQUIRE(f.player.status().state == Player::Status::State::Stopped);
  CHECK(shows(ui, "Nothing playing"));
  CHECK(shows(ui, "Cannot decode alpha.mp3"));
  CHECK_FALSE(shows(ui, "Only"));
}
