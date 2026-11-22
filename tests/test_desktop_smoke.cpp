#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "desktop/desktop_app.hpp"
#include "tests/tag_builder.hpp"
#include "tests/temp_dir.hpp"

#include <doctest.h>

#include <prism/app/model_app.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <unistd.h>

using namespace player;
using namespace player::testing;
using State = Player::Status::State;
namespace keys = prism::input::keys;

namespace {

Bytes load(std::string_view name) {
  std::ifstream in{std::string{TEST_DATA_DIR} + "/" + std::string{name}, std::ios::binary};
  REQUIRE(in);
  std::vector<char> chars{std::istreambuf_iterator<char>{in}, {}};
  const auto* p = reinterpret_cast<const std::byte*>(chars.data());
  return {p, p + chars.size()};
}

void write_song(const TempDir& dir, int n) {
  auto b = id3v2(3, {frame(3, "TIT2", utf8_text("Song " + std::to_string(n))), frame(3, "TPE1", utf8_text("Tone")),
                     frame(3, "TALB", utf8_text("Sines")), frame(3, "TRCK", latin1_text(std::to_string(n)))});
  append(b, load("sine440_44k.mp3"));
  dir.write("music/song" + std::to_string(n) + ".mp3", b);
}

void press(DesktopApp& app, std::initializer_list<int> keys) {
  for (const auto k : keys) app.key({k, 0});
}

}  // namespace

TEST_CASE("the desktop model runs 60 frames headless on a generated library") {
  TempDir dir;
  for (int n = 1; n <= 3; ++n) write_song(dir, n);
  auto backend = prism::app::Backend::software(prism::app::RenderConfig{});  // outlives the app, as in main()
  DesktopApp app{dir.path / "music"};
  auto& window = backend.create_window({.title = "smoke", .width = 600, .height = 300});

  int frames = 0;
  bool sent = false;
  State seen = State::Stopped;
  prism::app::model_app(backend, window, app, [&](prism::app::AppContext& ctx) {
    app.start(ctx);
    ctx.clock().add([&](auto) {
      ++frames;
      if (!sent && app.library) {  // built on the background thread
        // Music's rows are Folders, Artists, Albums, Songs: three downs reach Songs.
        press(app, {keys::enter, keys::down, keys::down, keys::down, keys::enter, keys::enter});
        sent = true;
      }
      seen = app.player.status().state;
      const bool done = (sent && frames >= 60 && seen == State::Playing) || frames >= 600;
      if (done) ctx.post_deferred([&ctx] { ctx.close_main_window(); });
      return !done;
    });
  });

  CHECK(sent);
  CHECK(frames >= 60);
  CHECK(seen == State::Playing);
  CHECK(app.ui.screen_title() == "Now Playing");
}

TEST_CASE("a read-only music folder says the index cannot be written, not No music") {
  if (::geteuid() == 0) return;  // root writes into a read-only folder anyway
  namespace fs = std::filesystem;
  TempDir dir;
  write_song(dir, 1);
  const auto music = dir.path / "music";
  fs::permissions(music, fs::perms::owner_write, fs::perm_options::remove);
  struct Writable {
    fs::path p;
    ~Writable() { fs::permissions(p, fs::perms::owner_write, fs::perm_options::add); }
  } restore{music};

  auto backend = prism::app::Backend::software(prism::app::RenderConfig{});
  DesktopApp app{music};
  auto& window = backend.create_window({.title = "read-only", .width = 600, .height = 300});
  int frames = 0;
  prism::app::model_app(backend, window, app, [&](prism::app::AppContext& ctx) {
    app.start(ctx);
    ctx.clock().add([&](auto) {
      ++frames;
      const bool done = app.ui.library_status() == "Cannot write the library index" || frames >= 600;
      if (done) ctx.post_deferred([&ctx] { ctx.close_main_window(); });
      return !done;
    });
  });

  CHECK(app.library == nullptr);
  CHECK(app.ui.library_status() == "Cannot write the library index");
}
