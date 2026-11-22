// The player on the Korvo-1's 800x480 panel: the 320x240 screen beside a touch click wheel, as
// on the desktop. No card and no Bluetooth yet: the library is empty ("No music") and the sound
// goes nowhere. The board's MODE button is Menu, VOL+ and VOL- turn the wheel.

#include "app_support.hpp"
#include "band_display.hpp"
#include "buttons.hpp"
#include "touch.hpp"

#include "core/memory_storage.hpp"
#include "core/player.hpp"
#include "core/ui.hpp"

#include <prism/platform/frame_app.hpp>

#include <array>
#include <cstdio>
#include <utility>
#include <vector>

namespace {

// No headset yet (Bluetooth comes later): accepts no format, so the player plays nothing.
class NullOutput final : public player::PcmOutput {
public:
  bool accepts(player::PcmFormat) const override { return false; }
  bool start(player::PcmFormat, player::PcmRing&) override { return false; }
  void stop() override {}
  bool connected() const override { return false; }
  std::uint32_t underruns() const override { return 0; }
  void set_volume(float) override {}
};

struct PlayerModel {
  player::MemoryStorage storage;  // simplify: no card. Upgrade: the SD card's storage.
  NullOutput output;
  player::Player player{storage, output};
  player::PlayerUi ui{nullptr, player};
  player::WheelWidget wheel{ui};

  void view(prism::app::ViewBuilder& vb) {
    vb.hstack([&] {
      ui.view(vb);
      wheel.view(vb);
    });
  }
};

struct BoardButton {
  korvo1::Button button;
  player::WheelEvent event;
};

// The buttons that open; one that does not is printed and left out.
std::vector<BoardButton> open_buttons() {
  const std::array bindings{
      std::pair{korvo1::mode_button, player::WheelEvent{player::Button::Menu}},
      std::pair{korvo1::volume_up_button, player::WheelEvent{player::Rotate{1}}},
      std::pair{korvo1::volume_down_button, player::WheelEvent{player::Rotate{-1}}},
  };
  std::vector<BoardButton> out;
  for (const auto& [which, event] : bindings) {
    auto button = korvo1::Button::open(which);
    if (button)
      out.push_back({std::move(*button), event});
    else
      std::printf("player: button %d unavailable: %s\n", static_cast<int>(which), button.error().c_str());
  }
  return out;
}

void handle_presses(std::vector<BoardButton>& buttons, player::PlayerUi& ui) {
  for (auto& b : buttons)
    if (b.button.pressed()) ui.handle(b.event);
}

}  // namespace

extern "C" void app_main() {
  auto display = korvo1::BandDisplay::open();
  if (!display) korvo1::halt(display.error());
  korvo1::Touch input = korvo1::open_touch_or_none();

  static PlayerModel model;
  static auto buttons = open_buttons();
  prism::platform::frame_app(*display, input, model, [](prism::platform::FrameContext& ctx) {
    ctx.on_loop([] { handle_presses(buttons, model.ui); });
    ctx.on_loop([] { model.ui.tick(); });
    ctx.on_stats(korvo1::print_stats);
  }, korvo1::pinned_threads());
  korvo1::halt_runtime_stopped();
}
