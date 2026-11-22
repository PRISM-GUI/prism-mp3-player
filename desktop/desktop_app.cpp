#include "desktop/desktop_app.hpp"

#include <prism/app/model_app.hpp>

#include <utility>

namespace player {
namespace keys = prism::input::keys;
namespace {

constexpr std::string_view kUpdating = "Updating library…";
constexpr std::string_view kCannotWrite = "Cannot write the library index";

// A fresh index of the card, or null when the build stopped or failed. On the builder thread.
std::shared_ptr<Library> build_library(Storage& storage, const std::stop_token& stop) {
  if (!Library::build(storage, kMusicRoot, [&](int) { return !stop.stop_requested(); })) return nullptr;
  return Library::open(storage, kMusicRoot);
}

}  // namespace

std::optional<WheelEvent> wheel_event_for_key(std::int32_t key) {
  switch (key) {
    case keys::up: return Rotate{-1};
    case keys::down: return Rotate{1};
    case keys::enter: return Button::Select;
    case keys::escape:
    case keys::backspace: return Button::Menu;
    case keys::space: return Button::PlayPause;
    case keys::left: return Button::Previous;
    case keys::right: return Button::Next;
    default: return std::nullopt;
  }
}

void DesktopApp::start(prism::app::AppContext& ctx) {
  ctx.set_global_key_handler([this](const prism::input::KeyPress& k) { key(k); });
  // simplify: the poll rides the frame clock, which stays active, so the logic thread wakes
  // every frame even when nothing plays. Upgrade: a deadline timer that runs only
  // while the player plays.
  ctx.clock().add([this](auto) {
    ui.tick();
    return true;
  });
  load_library(ctx);
}

void DesktopApp::key(const prism::input::KeyPress& k) {
  if (const auto e = wheel_event_for_key(k.key)) ui.handle(*e);
}

void DesktopApp::view(prism::app::ViewBuilder& vb) {
  vb.hstack([&] {
    ui.view(vb);
    wheel.view(vb);
  });
}

// An intact index shows at once, even of a card that changed since: the builder thread checks
// it and builds a new one when it is stale or missing. is_current() reads only the fingerprint
// the Library got when it opened, never its file, so the builder may call it while the logic
// thread reads rows.
void DesktopApp::load_library(prism::app::AppContext& ctx) {
  library = Library::open(storage, kMusicRoot);
  ui.set_library(library.get());
  if (!library) ui.set_library_status(std::string{kUpdating});  // at once: no "No music" before the builder says
  // simplify: the freshness scan does not watch the stop token, so closing the app during it
  // waits until it ends (seconds on a large card). Upgrade: a fingerprint() that takes a stop
  // callback, as Library::build does.
  builder_ = std::jthread([this, post = ctx.poster(), shown = library](std::stop_token stop) {
    if (shown && shown->is_current(storage, kMusicRoot)) return;
    post([this] { ui.set_library_status(std::string{kUpdating}); });
    auto fresh = build_library(storage, stop);
    std::string failure{fresh || stop.stop_requested() ? "" : kCannotWrite};
    post([this, fresh = std::move(fresh), failure = std::move(failure)] { use_library(fresh, failure); });
  });
}

// A failed build keeps what is shown and says why, unless it was stopped.
void DesktopApp::use_library(std::shared_ptr<Library> fresh, std::string failure) {
  if (!fresh) return ui.set_library_status(std::move(failure));
  ui.set_library(fresh.get());
  library = std::move(fresh);
}

}  // namespace player
