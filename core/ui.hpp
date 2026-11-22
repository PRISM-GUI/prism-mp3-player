#pragma once
#include "core/library.hpp"
#include "core/player.hpp"
#include "core/wheel.hpp"

#include <prism/core/field.hpp>
#include <prism/input/input_event.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace prism::app { class ViewBuilder; }
namespace prism::render { struct DrawList; }
namespace prism::ui { struct WidgetNode; }

namespace player {

namespace app = prism::app;
namespace input = prism::input;
namespace render = prism::render;
namespace ui = prism::ui;

// The 320×240 screen of an iPod Classic: menus, the library lists, Now Playing and Settings,
// driven by WheelEvents. A PRISM model with a canvas. Call every method from the logic thread,
// the one that also runs the Player.
class PlayerUi {
public:
  static constexpr float kTitleHeight = 26.f;
  static constexpr float kRowHeight = 26.f;
  static constexpr std::size_t kVisibleRows = 8;
  static constexpr int kVolumeSteps = 20;  // 5% per wheel step
  static constexpr core::Color kHighlight = core::Color::rgba(0x2F, 0x6F, 0xD0);
  static constexpr core::Color kTitleBar = core::Color::rgba(0xE4, 0xE6, 0xEA);
  static constexpr core::Color kBattery = core::Color::rgba(0x4A, 0x4E, 0x54);  // its outline

  struct Settings {
    std::vector<std::string> outputs{"Speakers"};
    std::size_t output = 0;  // shown here, applied at the next start
    std::string bluetooth = "Not available";
  };

  enum class Screen { Main, Music, List, NowPlaying, Settings };
  // A screen on the stack, with its own selection so Menu comes back to where it was.
  struct Place {
    Screen screen;
    ViewKey key{View::Artists};  // what a List shows
    std::size_t count = 0;       // its rows
    std::size_t selected = 0;
    std::size_t top = 0;  // the first visible row
  };

  // A list row: its label, and a value shown right-aligned (a setting's, an album's artist).
  struct Row {
    std::string label, value;
  };
  struct Pen;  // where a screen draws (ui_draw.cpp)

  PlayerUi(Library*, Player&);  // library may be null ("No music")

  void handle(WheelEvent);
  // Polls the player and repaints when what the screen shows changed (Now Playing's clock).
  void tick();
  // A rebuilt library replaces the old one: its lists leave the stack (to Main when one is on
  // screen).
  void set_library(Library*);
  // What a library is doing ("Updating library…"): the Music screen says it in place of No
  // music, and with a library, the title bar of the library screens says it. set_library()
  // clears it.
  void set_library_status(std::string);

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

  void rotate(int steps);
  void select();
  void select_menu_row();
  void select_list_row();
  void play_from(std::size_t row);

  void draw_title(const Pen&) const;
  void draw_menu(const Pen&) const;
  void draw_list(const Pen&) const;
  void draw_now_playing(const Pen&) const;
  std::vector<Row> visible_rows() const;

  Library* library_;
  Player& player_;
  int volume_ = kVolumeSteps / 2;
  std::vector<Place> stack_;
  std::string library_status_;
  Player::Status shown_{};  // what the screen shows of the player, for tick()
};

// The click wheel on screen: draws the ring and turns the pointer into WheelEvents for the UI.
class WheelWidget {
public:
  explicit WheelWidget(PlayerUi& ui) : ui_(ui) {}
  void canvas(render::DrawList&, core::Rect, const ui::WidgetNode&);
  void handle_canvas_input(const input::InputEvent&, ui::WidgetNode&, core::Rect);
  void view(app::ViewBuilder&);

private:
  PlayerUi& ui_;
  WheelTracker tracker_{core::Rect{}};
};

}  // namespace player
