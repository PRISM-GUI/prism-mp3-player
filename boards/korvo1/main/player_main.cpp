// The player on the Korvo-1's 800x480 panel: the 320x240 screen and a touch click wheel, the
// wheel under the screen when the board is held upright (CONFIG_PRISM_DISPLAY_ROTATION_90, as an
// iPod Classic), beside it otherwise, as on the desktop. The MP3s are the card's, from its root
// down, indexed on a thread of its own (library_loader.cpp); the sound goes to a Bluetooth headset
// (bt_output.cpp) or the board's own speaker (speaker_output.cpp), whichever the setting read at
// boot names, and is decoded on a third thread. The board's MODE button is Menu, VOL+ and VOL-
// turn the wheel.

#include "app_support.hpp"
#include "band_display.hpp"
#include "bt_output.hpp"
#include "buttons.hpp"
#include "jpeg_cover.hpp"
#include "library_loader.hpp"
#include "settings_nvs.hpp"
#include "speaker_output.hpp"
#include "thread_config.hpp"
#include "touch.hpp"

#include "core/cover_loader.hpp"
#include "core/dir_storage.hpp"
#include "core/library.hpp"
#include "core/player.hpp"
#include "core/ui.hpp"

#include "bsp/esp32_s31_korvo_1.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include <prism/platform/frame_app.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view kNoCard = "No SD card";
constexpr std::string_view kUnreadable = "SD card not readable (FAT32 or exFAT)";
constexpr std::uint32_t kPollStack = 32768;
constexpr std::uint32_t kCoverStack = 16384;  // read_tags and the shrink loop; the engine decodes
constexpr int kCoverPriority = 2;             // under the poll thread's 5
constexpr std::chrono::milliseconds kPollEvery{5};
constexpr std::int64_t kStatusEveryUs = 5'000'000;

// The output the boot setting names: a Bluetooth headset, or the board's own speaker. One for the
// session, built at boot; Settings saves a change for the next start rather than swapping the
// output under the decoder.
struct Outputs {
  Outputs() : choice{player::load_output_choice()} {
    if (choice == player::OutputChoice::speaker)
      speaker = std::make_unique<player::SpeakerOutput>();
    else  // powers the radio up and connects to the bonded headset, or scans
      radio = &player::BtOutput::open();
  }

  player::PcmOutput& get() const {
    return radio ? static_cast<player::PcmOutput&>(*radio) : *speaker;
  }

  const char* name() const { return radio ? "bluetooth" : "speaker"; }
  std::string state_text() const { return radio ? radio->state_text() : std::string{"on"}; }

  player::OutputChoice choice;
  player::BtOutput* radio = nullptr;
  std::unique_ptr<player::SpeakerOutput> speaker;
};

struct PlayerModel {
  player::DirStorage storage{BSP_SD_MOUNT_POINT};
  Outputs outputs;
  player::Player player{storage, outputs.get()};
  std::shared_ptr<player::Library> library;
  player::PlayerUi ui{nullptr, player};
  player::WheelWidget wheel{ui};
  player::LibraryLoader loader{storage};
  prism::core::Size frame{};  // the display in layout units, set before the runtime starts

  void view(prism::app::ViewBuilder& vb) {
    if (frame.h.raw() > frame.w.raw())
      vb.vstack([&] {
        ui.view(vb);
        wheel.view(vb, under_screen());
      });
    else
      vb.hstack([&] {
        ui.view(vb);
        wheel.view(vb);
      });
  }

  // All the frame below the screen: the wheel is as large as fits there.
  prism::core::Size under_screen() const {
    return {frame.w, prism::core::Height{frame.h.raw() - player::PlayerUi::kScreenHeight}};
  }
};

// The display's size in layout units: its pixels over the UI scale.
prism::core::Size layout_frame(prism::core::Size pixels, float ui_scale) {
  return {prism::core::Width{pixels.w.raw() / ui_scale}, prism::core::Height{pixels.h.raw() / ui_scale}};
}

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

// The card at /sdcard, or the reason to show instead: the player then runs with no library, rather
// than waiting on the card. ESP_FAIL is a card that answers but carries no FAT32 or exFAT volume,
// so nothing mounts: that is a card from another filesystem, or one the board cannot read at all.
std::string_view mount_card() {
  const esp_err_t mounted = bsp_sdcard_mount();
  if (mounted == ESP_OK) return {};
  std::printf("sd card: %s\n", esp_err_to_name(mounted));
  return mounted == ESP_FAIL ? kUnreadable : kNoCard;
}

// The headset's buttons arrive on the Bluetooth task: they wait here until the logic loop hands
// them to the UI, as if the wheel had sent them.
class RemoteButtons {
public:
  void post(player::Button button) {
    const std::lock_guard lock(mutex_);
    queue_.push_back(button);
  }

  void drain(player::PlayerUi& ui) {
    std::vector<player::Button> ready;
    {
      const std::lock_guard lock(mutex_);
      ready.swap(queue_);
    }
    for (const auto button : ready) ui.handle(button);
  }

private:
  std::mutex mutex_;
  std::vector<player::Button> queue_;  // guarded by mutex_: written on the Bluetooth task
};

// poll() decodes a few frames and refills the ring: it runs here, on core 0, with its stack in
// PSRAM, so nothing the user does waits for a decode. The board never shuts down, so this loop ends
// with it and is never joined.
std::thread start_poll_thread(player::Player& player) {
  return player::spawn_with(
      player::ThreadConfig{kPollStack, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, std::nullopt,
                           nullptr},
      [&player] {
        return std::thread([&player] {
          for (;;) {
            player.poll();
            std::this_thread::sleep_for(kPollEvery);
          }
        });
      });
}

// Covers load on a low-priority thread with its stack in PSRAM: the poll thread and Bluetooth, on
// the same core, always come first. The JPEG engine does the decode; the loader reads and shrinks.
std::unique_ptr<player::CoverLoader> start_cover_loader(player::Storage& storage, float ui_scale) {
  const int box_px = static_cast<int>(player::PlayerUi::kCoverSide * ui_scale);
  return player::spawn_with(
      player::ThreadConfig{kCoverStack, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, kCoverPriority,
                           nullptr},
      [&storage, box_px] {
        return std::make_unique<player::CoverLoader>(storage, player::decode_jpeg_cover, box_px);
      });
}

const char* state_name(player::Player::Status::State state) {
  switch (state) {
    case player::Player::Status::State::Stopped: return "stopped";
    case player::Player::Status::State::Playing: return "playing";
    case player::Player::Status::State::Paused: return "paused";
  }
  return "unknown";
}

// One line every 5 s, next to the heap's own: what the player and its output are doing.
void print_status_line(const PlayerModel& model) {
  static std::int64_t last_us = 0;
  const std::int64_t now = esp_timer_get_time();
  if (last_us != 0 && now - last_us < kStatusEveryUs) return;
  last_us = now;
  std::printf("player: %s out %s %s underruns %lu internal free %zu largest %zu\n",
              state_name(model.player.status().state), model.outputs.name(),
              model.outputs.state_text().c_str(),
              static_cast<unsigned long>(model.outputs.get().underruns()),
              heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
              heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

// Settings → Bluetooth shows what the radio is doing. Comparing the revision keeps the loop free of
// strings and locks; the text is rebuilt (and the canvas repainted) only when the state changes.
void show_bluetooth(player::PlayerUi& ui, const player::BtOutput& radio) {
  static std::uint32_t shown = 0;
  const auto revision = radio.state_revision();
  if (revision == shown) return;
  shown = revision;
  ui.set_bluetooth_text(radio.state_text());
}

// Settings → Output lists the outputs in OutputChoice order, so the index in the list is the byte
// stored; a change is saved for the next start, never applied under the decoder.
void wire_settings(PlayerModel& model) {
  model.ui.settings.outputs = {"Bluetooth", "Speaker"};
  model.ui.settings.output = static_cast<std::size_t>(model.outputs.choice);
  model.ui.on_output_changed([](std::size_t choice) {
    player::save_output_choice(static_cast<player::OutputChoice>(choice));
    std::printf("player: output %zu saved for the next start\n", choice);
  });
}

}  // namespace

extern "C" void app_main() {
  auto display = korvo1::BandDisplay::open();
  if (!display) korvo1::halt(display.error());
  korvo1::Touch input = korvo1::open_touch_or_none();
  const auto card_problem = mount_card();
  const bool card = card_problem.empty();

  static PlayerModel model;
  static RemoteButtons remote;
  const auto options = korvo1::pinned_threads();
  model.frame = layout_frame(display->size(), options.ui_scale);
  wire_settings(model);
  if (!card) model.ui.set_library_status(std::string{card_problem});
  else model.ui.set_storage(&model.storage);  // Folders browses the card with or without an index
  model.ui.poll_on_tick(false);  // the poll thread does that now
  if (auto* radio = model.outputs.radio)
    radio->on_remote([](player::Button button) { remote.post(button); });
  static auto buttons = open_buttons();
  static auto polling = start_poll_thread(model.player);
  static auto covers = start_cover_loader(model.storage, options.ui_scale);
  model.ui.set_cover_loader(covers.get());
  prism::platform::frame_app(*display, input, model, [card](prism::platform::FrameContext& ctx) {
    ctx.on_loop([] { remote.drain(model.ui); });
    ctx.on_loop([] { handle_presses(buttons, model.ui); });
    ctx.on_loop([card] {
      if (card) model.loader.pump(model.ui, model.library);
    });
    ctx.on_loop([] { model.ui.tick(); });
    ctx.on_loop([] {
      if (auto* radio = model.outputs.radio) show_bluetooth(model.ui, *radio);
    });
    ctx.on_loop([] { print_status_line(model); });
    ctx.on_stats(korvo1::print_stats);
  }, options);
  korvo1::halt_runtime_stopped();
}
