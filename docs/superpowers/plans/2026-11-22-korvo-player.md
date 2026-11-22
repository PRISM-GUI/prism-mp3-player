# MP3 player on the Korvo-1 — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The player firmware for the ESP32-S31-Korvo-1: MP3s from the SD card to the WH-1000XM3 over
Bluetooth (or the board's speaker), the 320×240 iPod UI and a touch click wheel on the panel.

**Architecture:** The portable core (core/) runs unchanged. A new ESP-IDF project in `boards/korvo1/`
reuses prism-esp32's board component (display, touch, buttons, port hooks) and PRISM through a
prism-esp32 submodule, adds a POSIX storage over FatFS, a Bluetooth `PcmOutput` grown from the
spike, a speaker `PcmOutput`, and an app that runs the Player on its own thread.

**Tech Stack:** ESP-IDF (pinned ad20ded, prism-esp32's), Bluedroid A2DP + AVRCP, FatFS/SDMMC through
the BSP, PRISM frame_app, C++ as PRISM.

**Spec:** `docs/superpowers/specs/2026-11-22-mp3-player-design.md`. Inputs: the spike's measurement
(ledger of the previous plan: streaming left 55.6 KB internal RAM with 48-row full-width bands; the
tone was clean) and the final review of the core (Player threading, stack sizes, A2DP accepts 44.1 kHz).

## Global Constraints

- Builds on the Coder workspace `prism` only, one per build directory, foreground; the user flashes.
- core/ stays OS-free; board code lives in `boards/korvo1/`, POSIX code in `posix/`.
- Internal RAM: the display uses **16-row** bands (2 × 25 KB instead of 2 × 75 KB). With the headset
  streaming, at least **64 KB internal free and a 32 KB largest block** (the spike's pass mark).
- The Player and its decoder live in PSRAM (heap/static), never on a task stack. The thread that calls
  `Player::poll()` has a stack of at least **24 KB** (minimp3 uses ~16 KB per frame).
- A2DP accepts **44100 Hz** only (`accepts()`); the speaker accepts 44100 and 48000.
- Underruns are printed on the timing line; the spec's acceptance: **zero underruns over a 45-minute
  album** while scrolling Songs.
- Commit with explicit pathspecs on main; never push; `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus

1. **The SD card missing or unreadable at boot:** the UI comes up and says "No SD card", nothing hangs. → Task 3.
2. **The headset off or out of range at boot, or switched off mid-song:** the UI runs, playback pauses
   with "Headset disconnected", and it reconnects when the headset returns. → Task 5.
3. **A UI-heavy moment (fast scrolling, a library rebuild) while playing:** no underrun. → Task 4 test, Task 7 measurement.
4. **The library rebuilt on the board (FatFS rename over an open index):** the old index stays usable,
   the new one replaces it. → Task 3 test.
5. **A 48 kHz track on Bluetooth:** skipped with a message, the album continues. → covered by the core's `accepts()` (fix wave) and checked in Task 7.

---

### Task 1: Configurable band height (prism-esp32)

**Files:** Modify `prism-esp32/boards/korvo1/band_display.hpp/.cpp`, `boards/korvo1/Kconfig`.

- [ ] Replace `kBandHeight = 48` with `CONFIG_PRISM_BAND_HEIGHT` (Kconfig int, default 48, range 8..96,
  must divide 480: a `static_assert`). Everything else uses `band_height()`.
- [ ] Build all four prism-esp32 apps on Coder (`scripts/build.sh all`). Expected: four .bin, no error.
- [ ] Commit in prism-esp32.

### Task 2: ESP-IDF project skeleton showing the UI

**Files:** Create `boards/korvo1/CMakeLists.txt`, `boards/korvo1/sdkconfig.defaults`,
`boards/korvo1/main/CMakeLists.txt`, `boards/korvo1/main/player_main.cpp`,
`boards/korvo1/components/player_core/CMakeLists.txt` (compiles `core/*.cpp`, includes `core/` and
`third_party/minimp3`, REQUIRES prism), `scripts/build-korvo.sh`, `.gitmodules` (prism-esp32 at
`subprojects/prism-esp32`, url https://github.com/PRISM-GUI/prism-esp32.git).

**Interfaces:** Consumes prism-esp32's `korvo1` component (BandDisplay, Touch, pinned_threads,
print_internal_heap, print_stats, buttons) and `prism` component via `EXTRA_COMPONENT_DIRS`.

- [ ] sdkconfig.defaults: prism-esp32's panel-app settings (PSRAM, BSS in PSRAM, small allocations in
  PSRAM, exceptions), `CONFIG_PRISM_BAND_HEIGHT=16`, `CONFIG_PRISM_UI_SCALE_PERCENT=150`, the
  spike's Bluetooth settings (`sdkconfig.btspike`) and `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y`.
- [ ] `player_main.cpp`: open display and touch, run `frame_app` with a model laid out as the desktop's
  (PlayerUi's canvas beside the WheelWidget; at 150% the screen is 480×360 px, the wheel fills the
  remaining 320 px), no library (UI says "No music"), a Player with a null output stand-in.
  Board buttons: MODE = Menu, VOL+/VOL− = Rotate ±1 (korvo1 buttons API).
- [ ] Fix carried from the core's final review (core/ui.cpp `play_from`): a song view holding at most
  `kSongsQueue` songs queues the whole view and starts at the selected row; a larger one queues a
  500-song window starting `min(row, 250)` songs before the selected one. Tests: Previous from the
  5th song of a 12-song album plays the 4th; the 2,000-song album test checks the selected song is
  the one playing.
- [ ] Build on Coder (`scripts/build-korvo.sh`, on Coder `subprojects/prism-esp32` is a symlink to
  `~/prism-esp32`). Expected: `build/player.bin`, no error; fetch to `build-local/player.bin`.
- [ ] Commit.

### Task 3: POSIX storage over FatFS, SD mount

**Files:** Create `posix/posix_storage.hpp/.cpp` (Storage over opendir/readdir/stat/fopen/fseek/
rename/unlink, rooted at a prefix), `tests/test_posix_storage.cpp` (host); modify
`boards/korvo1/main/player_main.cpp` (mount with `bsp_sdcard_mount()`, root `/sdcard`).

**Interfaces:** `class PosixStorage final : public Storage { public: explicit PosixStorage(std::string root); };`

- [ ] Failing host tests (temp dir): the same contract tests as DirStorage (reuse
  tests/test_storage.cpp's check helpers), plus `"rename replaces an existing target"` and
  `"rename over a file still open for reading keeps the reader's bytes"` (Review Focus 4; FatFS's
  `f_rename` refuses an existing target, so rename = unlink target, then rename; on POSIX the open
  reader keeps its inode; on FatFS the Library reopens — see next step).
- [ ] Implement. *Our call:* FatFS cannot unlink an open file reliably, so on the board the app closes
  the old Library before handing over the new one (the UI switches first, then the old Library is
  destroyed, then rename): document this order in `player_main.cpp`.
- [ ] Board: mount the SD; on failure the UI's library status says "No SD card" (Review Focus 1).
  Open/build the library on a background thread (core 0, 32 KB stack in PSRAM), hand it to the logic
  thread as the desktop does.
- [ ] Run host tests; build the firmware. Commit.

### Task 4: The Player on its own thread

**Files:** Modify `core/player.hpp/.cpp`, `tests/test_player.cpp`; create `tests/test_player_threads.cpp`.

- [ ] *Our call (final review Important 4):* every public Player method takes an internal
  `std::mutex`; `poll()` decodes at most **8 frames per call** so the UI never waits more than a few
  ms. PlayerUi keeps calling the Player directly. On the board `poll()` runs on a dedicated thread
  (core 0, 32 KB stack, every 5 ms); on the desktop it stays in `tick()`.
- [ ] Failing test: one thread calls `poll()` in a loop while another calls `status()`, `next()`,
  `toggle_pause()` for 2 s; run once in a ThreadSanitizer build dir on Coder
  (`meson setup builddir-tsan -Db_sanitize=thread`): no report. And: with poll() limited to 8 frames,
  a FakeOutput pulling in 4096-frame chunks with a poll every 5 ms has 0 underruns.
- [ ] Implement; run both build dirs' suites. Commit.

### Task 5: Bluetooth output (A2DP + AVRCP)

**Files:** Create `boards/korvo1/main/bt_output.hpp/.cpp`; modify `player_main.cpp`, `sdkconfig.defaults`
(`CONFIG_PLAYER_BT_NAME` default "WH-1000XM3" in `main/Kconfig.projbuild`).

**Interfaces:** `class BtOutput final : public player::PcmOutput` (start/stop/connected/underruns/
set_volume/accepts = 44100 only); `void BtOutput::on_remote(std::function<void(player::Button)>)` for the
headset's buttons (AVRCP passthrough: play/pause, next, previous).

- [ ] Grow from prism-esp32's `main/btspike.cpp` (discovery by name or the bonded address in NVS,
  reconnect on disconnect, logged esp_* failures). The A2DP data callback pops the ring (bytes / 4 =
  frames); it never blocks or allocates. Volume: AVRCP absolute volume when the headset supports it,
  else software scaling. Headset buttons are posted to the logic thread, which calls PlayerUi::handle.
- [ ] Settings → Bluetooth shows the state (scanning, connecting, connected to <name>).
- [ ] Build; the user flashes: the WH-1000XM3 connects, a song plays, the headset's buttons work,
  switching the headset off pauses with "Headset disconnected", switching it on reconnects (Review Focus 2).
- [ ] Commit.

### Task 6: Speaker output and the output setting

**Files:** Create `boards/korvo1/main/speaker_output.hpp/.cpp`, `boards/korvo1/main/settings_nvs.hpp/.cpp`.

- [ ] `SpeakerOutput` over the BSP's `bsp_audio_codec_speaker_init()` / esp_codec_dev, an I2S writer
  task popping the ring; accepts 44100 and 48000. Settings → Output (Bluetooth / Speaker) is stored in
  NVS and applied at the next start (spec).
- [ ] Build; the user flashes and hears a song on the speaker. Commit.

### Task 7: Measurements and the status line

**Files:** Modify `player_main.cpp`; create `boards/korvo1/README.md`.

- [ ] Every 5 s print: internal free / largest block, PSRAM free, CPU, underruns of the active output,
  player state. The profile marks stay on (trace page works).
- [ ] The user runs the acceptance: a 45-minute album over Bluetooth while scrolling Songs continuously,
  one library rebuild started meanwhile (touch a file). Pass: zero underruns, internal free ≥ 64 KB,
  largest block ≥ 32 KB. Record the numbers in the spec's status and the README.
- [ ] Commit.
