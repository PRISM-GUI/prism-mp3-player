#pragma once
#include "core/library.hpp"
#include "core/player.hpp"
#include "core/ui.hpp"
#include "core/dir_storage.hpp"
#include "desktop/sdl_pcm_output.hpp"

#include <prism/input/input_event.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace prism::app { class AppContext; }

namespace player {

// The storage is rooted at the music folder, so the library's root is "/" in it.
inline constexpr std::string_view kMusicRoot = "/";

// Up/Down turn the wheel, Enter selects, Esc/Backspace is Menu, Space plays or pauses,
// Left/Right skip.
std::optional<WheelEvent> wheel_event_for_key(std::int32_t key);

// The player on a PC: the 320×240 screen beside the click wheel, a folder of MP3s, the sound
// card. Its members are used on the logic thread only; the library is built on a thread of its
// own and handed over by a post to the logic thread.
class DesktopApp {
public:
  explicit DesktopApp(std::filesystem::path music) : storage(std::move(music)) {}

  // On the logic thread, once (model_app's setup): keys, the per-frame poll, the library.
  void start(prism::app::AppContext&);
  void key(const prism::input::KeyPress&);
  void view(prism::app::ViewBuilder&);

  DirStorage storage;
  SdlPcmOutput output;
  Player player{storage, output};
  std::shared_ptr<Library> library;
  PlayerUi ui{nullptr, player};
  WheelWidget wheel{ui};

private:
  void load_library(prism::app::AppContext&);
  void use_library(std::shared_ptr<Library>, std::string failure);

  std::jthread builder_;  // last: stopped and joined before the rest goes
};

}  // namespace player
