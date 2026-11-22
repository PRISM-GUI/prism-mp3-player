// The player on the Korvo-1's 800x480 panel: the 320x240 screen and a touch click wheel, the
// wheel under the screen when the board is held upright (CONFIG_PRISM_DISPLAY_ROTATION_90, as an
// iPod Classic), beside it otherwise, as on the desktop. The MP3s are the card's, from its root down, indexed on a thread of its own; the
// sound goes to a Bluetooth headset (bt_output.cpp) or the board's own speaker
// (speaker_output.cpp), whichever the setting read at boot names, and is decoded on a third
// thread. The board's MODE button is Menu, VOL+ and VOL- turn the wheel.

#include "app_support.hpp"
#include "band_display.hpp"
#include "bt_output.hpp"
#include "buttons.hpp"
#include "settings_nvs.hpp"
#include "speaker_output.hpp"
#include "touch.hpp"

#include "core/dir_storage.hpp"
#include "core/library.hpp"
#include "core/player.hpp"
#include "core/ui.hpp"

#include "bsp/esp32_s31_korvo_1.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_pthread.h"
#include "esp_timer.h"

#include <prism/platform/frame_app.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view kMusicRoot = "/";  // the card's root: the music is in folders there
constexpr std::string_view kUpdating = "Updating library…";
constexpr std::string_view kCannotWrite = "Cannot write the library index";
constexpr std::string_view kCannotOpen = "Cannot open the library index";
constexpr std::string_view kNoCard = "No SD card";
constexpr std::string_view kUnreadable = "SD card not readable (FAT32 or exFAT)";
constexpr std::uint32_t kBuilderStack = 32768;
constexpr std::uint32_t kPollStack = 32768;
constexpr std::chrono::milliseconds kPollEvery{5};
constexpr std::int64_t kStatusEveryUs = 5'000'000;

// What the builder thread found of the card: the index it was given is current, a new one waits
// at .prism-library.new, or none could be written.
struct Build {
  enum class Kind { current, staged, failed } kind;
};

// What the builder thread leaves for the logic thread, merged until it takes it.
struct Handover {
  bool updating = false;
  std::optional<Build> built;
};

// Builds the card's index off the logic thread and hands it over. The logic thread opens the
// index and owns it throughout; the builder gets a plain pointer to ask is_current() (which
// reads only the fingerprint the Library holds, never its file) and holds no Library of its own.
class LibraryLoader {
public:
  explicit LibraryLoader(player::Storage& storage) : storage_(storage) {}
  ~LibraryLoader() {
    if (thread_.joinable()) thread_.join();  // a std::thread must be joined before it dies
  }

  // Each logic loop: the first opens and shows the index and starts the builder, then what the
  // builder left is applied.
  void pump(player::PlayerUi& ui, std::shared_ptr<player::Library>& shown) {
    if (!thread_.joinable()) begin(ui, shown);
    auto handover = take();
    if (!handover) return;
    if (handover->updating) ui.set_library_status(std::string{kUpdating});
    if (handover->built) apply(ui, shown, handover->built->kind);
  }

private:
  // The index the card has today shows at once, even stale: the old one stays usable while the
  // builder makes a new one.
  void begin(player::PlayerUi& ui, std::shared_ptr<player::Library>& shown) {
    shown = player::Library::open(storage_, kMusicRoot);
    ui.set_library(shown.get());
    start(shown.get());
  }

  // The builder's stack goes to PSRAM, as the logic thread's does: internal RAM is for the
  // render thread's 48 KB stack and the bands. The config it sets is the logic thread's own,
  // and threads it creates later would inherit it: put the previous one back.
  void start(const player::Library* index) {
    esp_pthread_cfg_t previous{};
    if (esp_pthread_get_cfg(&previous) != ESP_OK) previous = esp_pthread_get_default_config();
    esp_pthread_cfg_t cfg = previous;
    cfg.stack_size = kBuilderStack;
    cfg.pin_to_core = 0;
    cfg.stack_alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    esp_pthread_set_cfg(&cfg);
    thread_ = std::thread([this, index] { build(index); });
    esp_pthread_set_cfg(&previous);
  }

  void build(const player::Library* index) {
    if (index && index->is_current(storage_, kMusicRoot))
      return post(Handover{false, Build{Build::Kind::current}});
    post(Handover{true, std::nullopt});
    // simplify: no cancellation: the run ends only with the board.
    const bool staged =
        player::Library::build(storage_, kMusicRoot, [](int) { return true; }, false);
    post(Handover{false, Build{staged ? Build::Kind::staged : Build::Kind::failed}});
  }

  // Where the hand-over lands, on the logic thread.
  void apply(player::PlayerUi& ui, std::shared_ptr<player::Library>& shown, Build::Kind kind) {
    if (kind == Build::Kind::current) return;
    if (kind == Build::Kind::failed) return ui.set_library_status(std::string{kCannotWrite});
    install(ui, shown);
  }

  // FatFS cannot replace an index that is open: the UI stops showing the old one, its Library
  // closes it, and only then may the staged file take its place.
  void install(player::PlayerUi& ui, std::shared_ptr<player::Library>& shown) {
    ui.set_library(nullptr);
    shown.reset();
    if (!player::Library::install(storage_, kMusicRoot))
      return ui.set_library_status(std::string{kCannotWrite});
    show(ui, shown);
  }

  void show(player::PlayerUi& ui, std::shared_ptr<player::Library>& shown) {
    auto fresh = player::Library::open(storage_, kMusicRoot);
    if (!fresh) return ui.set_library_status(std::string{kCannotOpen});
    ui.set_library(fresh.get());
    shown = std::move(fresh);
    korvo1::print_internal_heap("after library");
  }

  void post(Handover next) {
    const std::lock_guard lock(mutex_);
    if (!handover_) {
      handover_ = next;
      return;
    }
    handover_->updating = handover_->updating || next.updating;
    if (next.built) handover_->built = next.built;
  }

  std::optional<Handover> take() {
    const std::lock_guard lock(mutex_);
    std::optional<Handover> out;
    out.swap(handover_);
    return out;
  }

  player::Storage& storage_;
  std::thread thread_;
  std::mutex mutex_;
  std::optional<Handover> handover_;  // guarded by mutex_: written on the builder thread
};

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
  LibraryLoader loader{storage};
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
  esp_pthread_cfg_t previous{};
  if (esp_pthread_get_cfg(&previous) != ESP_OK) previous = esp_pthread_get_default_config();
  esp_pthread_cfg_t cfg = previous;
  cfg.stack_size = kPollStack;
  cfg.pin_to_core = 0;
  cfg.stack_alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
  esp_pthread_set_cfg(&cfg);
  std::thread thread{[&player] {
    for (;;) {
      player.poll();
      std::this_thread::sleep_for(kPollEvery);
    }
  }};
  esp_pthread_set_cfg(&previous);  // the config is the creating thread's own
  return thread;
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
  ui.settings.bluetooth = radio.state_text();
  ui.revision.set(ui.revision.get() + 1);
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
  // In OutputChoice order: the index in the list is the stored byte.
  model.ui.settings.outputs = {"Bluetooth", "Speaker"};
  model.ui.settings.output = static_cast<std::size_t>(model.outputs.choice);
  model.ui.on_output_changed([](std::size_t choice) {
    player::save_output_choice(static_cast<player::OutputChoice>(choice));
    std::printf("player: output %zu saved for the next start\n", choice);
  });
  if (!card) model.ui.set_library_status(std::string{card_problem});
  else model.ui.set_storage(&model.storage);  // Folders browses the card with or without an index
  model.ui.poll_on_tick(false);  // the poll thread does that now
  if (auto* radio = model.outputs.radio)
    radio->on_remote([](player::Button button) { remote.post(button); });
  static auto buttons = open_buttons();
  static auto polling = start_poll_thread(model.player);
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
