#include "library_loader.hpp"

#include "app_support.hpp"
#include "thread_config.hpp"

#include "core/library.hpp"
#include "core/storage.hpp"
#include "core/ui.hpp"

#include <esp_heap_caps.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace player {
namespace {

constexpr std::string_view kMusicRoot = "/";  // the card's root: the music is in folders there
constexpr std::string_view kUpdating = "Updating library…";
constexpr std::string_view kCannotWrite = "Cannot write the library index";
constexpr std::string_view kCannotOpen = "Cannot open the library index";
constexpr std::uint32_t kBuilderStack = 32768;

}  // namespace

LibraryLoader::LibraryLoader(Storage& storage) : storage_(storage) {}

LibraryLoader::~LibraryLoader() {
  if (thread_.joinable()) thread_.join();  // a std::thread must be joined before it dies
}

void LibraryLoader::pump(PlayerUi& ui, std::shared_ptr<Library>& shown) {
  if (!thread_.joinable()) begin(ui, shown);
  auto handover = take();
  if (!handover) return;
  if (handover->updating) ui.set_library_status(std::string{kUpdating});
  if (handover->built) apply(ui, shown, *handover->built);
}

void LibraryLoader::begin(PlayerUi& ui, std::shared_ptr<Library>& shown) {
  shown = Library::open(storage_, kMusicRoot);
  ui.set_library(shown.get());
  start(shown.get());
}

void LibraryLoader::start(const Library* index) {
  // The builder reads the card and builds in memory: its stack goes to PSRAM, as the logic
  // thread's does, since internal RAM is for the render thread's 48 KB stack and the bands.
  thread_ = spawn_with(
      ThreadConfig{kBuilderStack, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, std::nullopt, nullptr},
      [this, index] { return std::thread([this, index] { build(index); }); });
}

void LibraryLoader::build(const Library* index) {
  if (index && index->is_current(storage_, kMusicRoot))
    return post(Handover{false, BuildResult::current});
  post(Handover{true, std::nullopt});
  // simplify: no cancellation: the run ends only with the board.
  const bool staged =
      Library::build(storage_, kMusicRoot, [](int) { return true; }, Library::Install::staged);
  post(Handover{false, staged ? BuildResult::staged : BuildResult::failed});
}

void LibraryLoader::apply(PlayerUi& ui, std::shared_ptr<Library>& shown, BuildResult result) {
  if (result == BuildResult::current) return;
  if (result == BuildResult::failed) return ui.set_library_status(std::string{kCannotWrite});
  install(ui, shown);
}

void LibraryLoader::install(PlayerUi& ui, std::shared_ptr<Library>& shown) {
  ui.set_library(nullptr);
  shown.reset();
  if (!Library::install(storage_, kMusicRoot))
    return ui.set_library_status(std::string{kCannotWrite});
  show(ui, shown);
}

void LibraryLoader::show(PlayerUi& ui, std::shared_ptr<Library>& shown) {
  auto fresh = Library::open(storage_, kMusicRoot);
  if (!fresh) return ui.set_library_status(std::string{kCannotOpen});
  ui.set_library(fresh.get());
  shown = std::move(fresh);
  korvo1::print_internal_heap("after library");
}

void LibraryLoader::post(Handover next) {
  const std::lock_guard lock(mutex_);
  if (!handover_) {
    handover_ = next;
    return;
  }
  handover_->updating = handover_->updating || next.updating;
  if (next.built) handover_->built = next.built;
}

std::optional<Handover> LibraryLoader::take() {
  const std::lock_guard lock(mutex_);
  std::optional<Handover> out;
  out.swap(handover_);
  return out;
}

}  // namespace player
