#pragma once
#include "core/library.hpp"
#include "core/player.hpp"
#include "core/wheel.hpp"

#include <prism/core/field.hpp>
#include <prism/input/input_event.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace prism::app { class ViewBuilder; }
namespace prism::render { struct DrawList; class Image; }
namespace prism::ui { struct WidgetNode; }

namespace player {

class CoverLoader;

namespace app = prism::app;
namespace input = prism::input;
namespace render = prism::render;
namespace ui = prism::ui;

// The 320×240 screen of an iPod Classic: menus, the library lists, Now Playing and Settings,
// driven by WheelEvents. A PRISM model with a canvas. Call every method from the logic thread,
// the one that also runs the Player.
class PlayerUi {
public:
  static constexpr float kScreenWidth = 320.f;
  static constexpr float kScreenHeight = 240.f;
  static constexpr float kTitleHeight = 26.f;
  static constexpr float kRowHeight = 26.f;
  static constexpr std::size_t kVisibleRows = 8;  // on a 240-high screen; a taller one shows more
  static constexpr int kVolumeSteps = 20;  // 5% per wheel step
  static constexpr core::Color kHighlight = core::Color::rgba(0x2F, 0x6F, 0xD0);
  static constexpr core::Color kTitleBar = core::Color::rgba(0xE4, 0xE6, 0xEA);
  static constexpr core::Color kBattery = core::Color::rgba(0x4A, 0x4E, 0x54);  // its outline

  struct Settings {
    std::vector<std::string> outputs{"Speakers"};
    std::size_t output = 0;  // shown here, applied at the next start
    std::string bluetooth = "Not available";
  };

  enum class Screen { Main, Music, List, NowPlaying, Settings, Folders };
  // A list row: its label, and a value shown right-aligned (a setting's, an album's artist).
  struct Row {
    std::string label, value;
  };
  // A screen on the stack, with its own selection so Menu comes back to where it was.
  struct Place {
    Screen screen;
    ViewKey key{View::Artists};  // what a List shows
    std::size_t count = 0;       // its rows
    std::size_t selected = 0;
    std::size_t top = 0;  // the first visible row
    // Folders: the directory on screen, and its entries read once when it opened. Subfolders come
    // first with a trailing '/', then the .mp3 files. Initialised here too: every Place is built as
    // an aggregate, and -Wmissing-field-initializers wants each member either set or defaulted.
    std::string dir{};
    std::vector<Row> rows{};
  };
  struct Pen;  // where a screen draws (ui_draw.cpp)

  PlayerUi(Library*, Player&);  // library may be null ("No music")

  void handle(WheelEvent);
  // Polls the player (unless the board polls on a thread of its own) and repaints when what the
  // screen shows changed (Now Playing's clock).
  void tick();
  // A board that runs poll() on its own thread turns this off, or two decoders fight over the
  // ring. On by default: the desktop has no other thread to poll from.
  void poll_on_tick(bool on) { polls_on_tick_ = on; }
  // A rebuilt library replaces the old one: its lists leave the stack (to Main when one is on
  // screen).
  void set_library(Library*);
  // The card, for the Folders browser: it lists a folder straight from storage, so it works while
  // the library is missing or rebuilding. Null: the Folders row says "No SD card".
  void set_storage(Storage*);
  // What a library is doing ("Updating library…"): the Music screen says it in place of No
  // music, and with a library, the title bar of the library screens says it. set_library()
  // clears it.
  void set_library_status(std::string);
  const std::string& library_status() const { return library_status_; }
  // Settings → Output cycles `settings.outputs`; `settings.output` is what the next start uses,
  // never the running output. The board saves it from here.
  void on_output_changed(std::function<void(std::size_t)> handler) {
    output_changed_ = std::move(handler);
  }

  // Where covers come from. Null: Now Playing keeps its placeholder box.
  void set_cover_loader(CoverLoader* loader) { cover_loader_ = loader; }

  void canvas(render::DrawList&, core::Rect, const ui::WidgetNode&);
  void view(app::ViewBuilder&);

  std::string_view screen_title() const;
  std::size_t selected() const { return here().selected; }

  Settings settings;
  prism::core::Field<int> revision{0};  // bumped on every change: the canvas repaints

private:
  Place& here() { return stack_.back(); }
  const Place& here() const { return stack_.back(); }
  void changed() { revision.set(revision.get() + 1); }
  void open(Place p) { stack_.push_back(std::move(p)); }
  Place menu(Screen) const;
  Place list(ViewKey) const;
  std::vector<std::string> menu_rows(Screen) const;
  bool has_music() const { return library_ && library_->count({View::Songs}) > 0; }

  void rotate(int steps);
  void select();
  void select_menu_row();
  void select_list_row();
  void play_from(std::size_t row);

  void select_folders();
  void select_folder_row();
  void open_folder(std::string_view dir);
  void play_folder(std::string_view dir, std::size_t row);
  std::vector<Row> read_folder(std::string_view dir) const;

  void draw_title(const Pen&) const;
  void draw_menu(const Pen&) const;
  void draw_list(const Pen&) const;
  void draw_now_playing(const Pen&) const;
  std::vector<Row> visible_rows() const;
  void fit_rows(float height);

  // Asks the loader for the current track's cover when that changed, and repaints when a load
  // finished. Does nothing without a loader.
  void watch_cover();
  // The current track's cover, or null while it loads or when it has none.
  std::shared_ptr<const render::Image> cover_image() const;

  Library* library_;
  Storage* storage_ = nullptr;  // the card, for Folders; not the same as the library
  CoverLoader* cover_loader_ = nullptr;
  std::string cover_path_;              // the track last asked for
  std::uint64_t cover_revision_ = 0;    // the loader's revision when we last repainted for it
  Player& player_;
  int volume_ = kVolumeSteps / 2;
  bool polls_on_tick_ = true;
  std::size_t rows_on_screen_ = kVisibleRows;  // what the last drawn canvas had room for
  std::vector<Place> stack_;
  std::string library_status_;
  std::function<void(std::size_t)> output_changed_;  // the board's, to save the choice
  Player::Status shown_{};  // what the screen shows of the player, for tick()
};

// The click wheel on screen: draws the ring and turns the pointer into WheelEvents for the UI.
class WheelWidget {
public:
  explicit WheelWidget(PlayerUi& ui) : ui_(ui) {}
  void canvas(render::DrawList&, core::Rect, const ui::WidgetNode&);
  void handle_canvas_input(const input::InputEvent&, ui::WidgetNode&, core::Rect);
  // The ring is the largest that fits in `size`, centred.
  void view(app::ViewBuilder&, core::Size size = {core::Width{200}, core::Height{200}});

private:
  PlayerUi& ui_;
  WheelTracker tracker_{core::Rect{}};
};

}  // namespace player
