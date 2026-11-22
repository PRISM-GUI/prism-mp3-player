# MP3 player: Bluetooth spike, portable core, desktop app — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prove Bluetooth audio and the PRISM display fit together on the ESP32-S31, then build the
player's portable core (tags, library, playback, iPod UI, click wheel) and run it as a desktop app.

**Architecture:** A throwaway spike app in prism-esp32 answers the RAM question on the board. The
player lives in a new repo, prism-mp3-player: `core/` is plain C++20+ on PRISM with no platform
call (storage, audio outputs and input come through small interfaces), `desktop/` implements them
with std::filesystem and PRISM's `audio::Output`, `tests/` runs everything on the host. The Korvo
app is the next plan, written after the spike's numbers.

**Tech Stack:** C++26 (as PRISM), Meson, PRISM as a Meson subproject, doctest, minimp3 (vendored,
CC0), ESP-IDF Bluedroid (spike only).

**Spec:** `docs/superpowers/specs/2026-11-22-mp3-player-design.md`

## Global Constraints

- The player's screen is **320×240**; nothing in `core/` assumes another size.
- `core/` includes no ESP-IDF, SDL or OS header; only PRISM, the C++ standard library and minimp3.
- Library size target: **10,000 tracks or more**; only the visible rows of a list are read into RAM.
- Index file: `/music/.prism-library` (under the library root); music root `/music`.
- Playable sample rates: **44100 and 48000 Hz**; any other rate is refused with a message.
- PCM: 16-bit signed, interleaved stereo (mono is duplicated to both channels).
- Ring buffers: **2 s of PCM**.
- Output: one output, chosen at start; a disconnect (headset lost) **pauses** playback.
- Underruns are counted per output (`underruns()`), never hidden.
- Builds and tests run on the Coder workspace `prism` (`~/prism-mp3-player`, PRISM at `~/PRSIM`,
  prism-esp32 at `~/prism-esp32`), one build per build directory, in the foreground, never
  backgrounded. Edit locally, rsync, build there (commands in Task 2).
- Commit with explicit pathspecs, on `main`. Never push. End commit messages with
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Code style: PRISM's AGENTS.md (KISS, small units, `simplify:` comments for deliberate cuts, no
  bare `detail` namespace).

## Review Focus

1. **Damaged or hostile tags** (truncated ID3v2, size fields past the file's end, unknown text
   encodings): the track still appears, titled by its file name; nothing crashes or over-reads.
   → Task 3 tests.
2. **Non-ASCII names** (accented artists in ID3 UTF-16, file names in UTF-8): shown correctly and
   sorted case-insensitively by their letters. → Task 3 and Task 4 tests.
3. **An empty or missing `/music`, or a card swapped between boots:** the menus say "No music"
   and the index is rebuilt; a stale index never shows tracks that are gone. → Task 4 tests.
4. **Fast wheel scrolling through 10,000 songs:** the selection follows every step and the memory
   used does not grow with the list. → Task 4 and Task 6 tests.
5. **A file at 32 kHz or a corrupt MP3 in the middle of an album:** that track is refused or
   skipped with a message and playback continues with the next. → Task 5 tests.

---

### Task 1: Bluetooth spike on the Korvo (throwaway, prism-esp32)

The stop-or-go gate. Lives in prism-esp32 as a fourth app, `btspike`; deleted once the Korvo plan
starts.

**Files:**
- Create: `prism-esp32/main/btspike.cpp`, `prism-esp32/sdkconfig.btspike`
- Modify: `prism-esp32/main/Kconfig.projbuild` (app choice), `prism-esp32/scripts/build.sh`
  (`apps=(selftest widgets scope btspike)`)

**Interfaces:** Consumes korvo1's `BandDisplay`, `open_touch_or_none`, `print_internal_heap`,
`pinned_threads`, PRISM's `frame_app` and `prism_examples` widgets demo (as `main/widgets.cpp`
uses it). Produces nothing the later tasks use; its output is a measurement.

- [ ] **Step 1: Configure Bluetooth** in `sdkconfig.btspike`: the a2dp_source example's settings
  (`CONFIG_BT_ENABLED`, `CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY=y`, `CONFIG_BT_BLUEDROID_ENABLED`,
  `CONFIG_BT_CLASSIC_ENABLED`, `CONFIG_BT_A2DP_ENABLE`, `CONFIG_BT_BLE_ENABLED=n`), plus
  `CONFIG_BT_AVRCP_ENABLED=y` and `CONFIG_BT_ALLOCATION_FROM_SPIRAM_FIRST=y`.
- [ ] **Step 2: Write `btspike.cpp`:** open the display and touch as `widgets.cpp` does; start
  Bluedroid as an A2DP source and AVRCP controller+target; discover, and connect to the first device
  whose name contains `WH-1000XM3`; when the stream starts, feed a 440 Hz sine at 44.1 kHz stereo
  from the A2DP data callback; run the widgets demo with `frame_app`. Every 5 s print one line:
  `btspike: <state> internal free N, largest block M, psram free P, underruns U` (state: scanning,
  connecting, streaming). Base the Bluetooth code on ESP-IDF's
  `examples/bluetooth/bluedroid/classic_bt/a2dp_source`; no PRISM change.
- [ ] **Step 3: Build** on Coder: `scripts/build.sh btspike`. Expected: `build/btspike.bin: N bytes`,
  no error. Fetch it into `build-local/`.
- [ ] **Step 4: The user flashes it** with the headset in pairing mode and sends the console lines
  after one minute of streaming.
- [ ] **Step 5: Judge.** Pass: streaming, the tone audible, `internal free` ≥ 64 KB and
  `largest block` ≥ 32 KB. The display here uses full-width 800-px bands (2 × 75 KB), larger than
  the product's 320-px bands (2 × 30 KB): also report the figure with the 90 KB difference added.
  Pass on the raw figure: go. Pass only with the difference: go, and the Korvo plan draws the
  screen with 320-px bands. Fail on both: stop, report to the user.
- [ ] **Step 6: Commit** `main/btspike.cpp sdkconfig.btspike main/Kconfig.projbuild scripts/build.sh`
  with the measured lines in the message.

### Task 2: Repository skeleton and the storage interface

**Files:**
- Create: `meson.build`, `subprojects/prism.wrap`,
  `subprojects/minimp3.wrap` or `third_party/minimp3/minimp3.h` + `minimp3_ex.h` (vendored, CC0),
  `core/storage.hpp`, `core/memory_storage.hpp`, `desktop/dir_storage.hpp`, `desktop/dir_storage.cpp`,
  `tests/meson.build`, `tests/test_storage.cpp`, `.gitignore` (`builddir*/`, `subprojects/*/`
  except the wraps), `README.md` (what it is, how to build, how to run).

**Interfaces:**
- Produces, in namespace `player`:
  ```cpp
  struct DirEntry { std::string name; bool is_dir; std::uint64_t size; std::int64_t mtime; };
  class File {             // random-access read-only file
  public: virtual ~File() = default;
      virtual std::uint64_t size() const = 0;
      virtual std::size_t read(std::uint64_t offset, std::span<std::byte> out) = 0;   // bytes read
  };
  class Storage {
  public: virtual ~Storage() = default;
      virtual std::vector<DirEntry> list(std::string_view dir) = 0;           // empty if missing
      virtual std::unique_ptr<File> open(std::string_view path) = 0;          // null if missing
      virtual std::unique_ptr<std::ostream> create(std::string_view path) = 0; // for the index; null on failure
  };
  class MemoryStorage final : public Storage { public: void add(std::string path, std::vector<std::byte> data, std::int64_t mtime = 0); /* + Storage */ };
  class DirStorage final : public Storage { public: explicit DirStorage(std::filesystem::path root); /* + Storage */ };
  ```
  Paths are `/`-separated and absolute from the storage's root (`/music/a.mp3`).

- [ ] **Step 1: Build setup.** `subprojects/prism.wrap` is a `[wrap-git]` on
  `https://github.com/PRISM-GUI/PRISM.git`, revision `main`; on Coder, `subprojects/prism` is a
  symlink to `~/PRSIM` (as in prism-esp32), so local PRISM changes are seen. `meson.build`:
  `project('prism-mp3-player', 'cpp', default_options: ['cpp_std=c++26', 'warning_level=2'])`,
  `prism = subproject('prism', default_options: ['tests=disabled', 'prism_debug_tools=disabled'])`,
  `prism_dep = prism.get_variable('prism_dep')`; a `player_core` static library from `core/*.cpp`;
  `tests/` and `desktop/` subdirs.
  Coder commands (use these in every later task):
  ```
  . ~/.config/lpp-coder/env
  rsync -a --exclude 'builddir*/' --exclude .git/ --exclude 'subprojects/*/' -e coder-rsh ~/Documents/prog/prism-mp3-player/ prism:prism-mp3-player/
  coder ssh prism -- 'cd ~/prism-mp3-player && ln -sfn ~/PRSIM subprojects/prism && export PATH=$HOME/PRSIM/.venv/bin:$PATH CC="ccache gcc" CXX="ccache g++" && (test -d builddir || meson setup builddir) && ninja -C builddir && meson test -C builddir --print-errorlogs 2>&1 | tail -15'
  ```
- [ ] **Step 2: Write the failing tests** in `tests/test_storage.cpp`:
  - `"MemoryStorage lists a directory's files and subdirectories"`: add `/music/a.mp3`,
    `/music/x/b.mp3`; `list("/music")` has `a.mp3` (file, its size) and `x` (dir).
  - `"MemoryStorage reads at an offset and stops at the end"`: 10-byte file, `read(8, 4 bytes)` returns 2.
  - `"a missing path lists empty and opens null"`.
  - `"DirStorage sees the same tree as the files on disk"`: in a temp dir, same three checks.
  - `"create then open reads back what was written"`, for both storages.
- [ ] **Step 3: Run**: the Coder command. Expected: compile errors (types missing).
- [ ] **Step 4: Implement** the types above.
- [ ] **Step 5: Run.** Expected: all pass.
- [ ] **Step 6: Commit** everything listed in Files.

### Task 3: ID3 tags

**Files:** Create `core/tags.hpp`, `core/tags.cpp`, `tests/test_tags.cpp`, `tests/tag_builder.hpp`
(test helper that writes ID3v2.3/2.4/ID3v1 bytes).

**Interfaces:**
- Consumes: `File` (Task 2).
- Produces:
  ```cpp
  struct Cover { std::uint64_t offset = 0; std::uint32_t size = 0; std::string mime; };  // size 0: none
  struct Tags { std::string title, artist, album; int track = 0; std::uint64_t audio_offset = 0; Cover cover; };
  Tags read_tags(File& file, std::string_view file_name);   // never throws; UTF-8 strings
  ```
  `audio_offset` is where the MP3 frames start (after an ID3v2 tag). Missing title → the file name
  without its extension; missing artist/album → empty (the menus show "Unknown Artist"/"Unknown Album").

- [ ] **Step 1: Failing tests** (`tag_builder.hpp` builds the inputs):
  - `"ID3v2.3 with UTF-16 text"`: TIT2 `"Été"` (UTF-16 with BOM), TPE1, TALB, TRCK `"3/12"` →
    title `"Été"`, track 3, `audio_offset` = 10 + tag size.
  - `"ID3v2.4 with UTF-8 text and a synchsafe frame size"`.
  - `"ID3v1 only"`: the 128-byte trailer → title, artist, album, track from byte 126.
  - `"no tags: the title is the file name without .mp3"`: `"01 Intro.mp3"` → `"01 Intro"`.
  - `"APIC gives the cover's offset, size and mime"`: a JPEG payload; `file.read(offset, size)`
    returns those exact bytes.
  - `"a tag whose size runs past the end of the file"`: title from the file name, no read past
    `size()` (a counting `File` wrapper asserts every read stays in bounds).
  - `"a frame with an unknown text encoding byte is skipped"`.
- [ ] **Step 2: Run.** Expected: fail to compile.
- [ ] **Step 3: Implement `read_tags`.** Read the 10-byte header, then the whole tag in one read
  (cap: 1 MiB, beyond it only text frames before the cap count); ISO-8859-1, UTF-16 (BOM),
  UTF-16BE, UTF-8 → UTF-8. ID3v1 read only when no ID3v2 title.
- [ ] **Step 4: Run.** Expected: pass.
- [ ] **Step 5: Commit** the four files.

### Task 4: Library index (10,000 tracks)

**Files:** Create `core/library.hpp`, `core/library.cpp`, `tests/test_library.cpp`.

**Interfaces:**
- Consumes: `Storage`, `File` (Task 2), `read_tags`, `Tags` (Task 3).
- Produces:
  ```cpp
  struct TrackRow { std::string path, title, artist, album; int track; };
  enum class View { Artists, Albums, Songs, AlbumsOfArtist, SongsOfAlbum };
  struct ViewKey { View view; std::string artist, album; };   // artist/album: the filter where the view needs one
  class Library {
  public:
      // Opens the index at <root>/.prism-library; returns null when it is missing, damaged, or
      // does not match the card (fingerprint differs).
      static std::unique_ptr<Library> open(Storage&, std::string_view root);
      // Scans <root> recursively for .mp3, reads tags, writes the index. `progress(done)` is
      // called every 100 files; returning false stops (the old index stays).
      static bool build(Storage&, std::string_view root, const std::function<bool(int)>& progress);
      std::size_t count(const ViewKey&) const;
      // Rows [first, first + n) of a view: names for Artists/Albums/AlbumsOfArtist, tracks for the song views.
      std::vector<std::string> names(const ViewKey&, std::size_t first, std::size_t n) const;
      std::vector<TrackRow> tracks(const ViewKey&, std::size_t first, std::size_t n) const;
  };
  std::uint64_t fingerprint(Storage&, std::string_view root);   // over paths, sizes, mtimes
  ```
  Sorting: artists and albums alphabetical, case-insensitive, accents folded for the comparison
  (`É` sorts with `E`); songs of an album by track number then title; Songs by title.

- [ ] **Step 1: Failing tests:**
  - `"build then open lists artists, their albums and songs in order"` (6 tracks, 2 artists).
  - `"accented names sort with their base letter"`: `"Émilie"` between `"Eddy"` and `"Fred"`.
  - `"no /music or an empty one: count is 0 everywhere and open after build works"`.
  - `"a changed card makes open return null"`: add a file after `build` → `open` is null.
  - `"10,000 tracks: rows come from the index, not from memory"`: a `MemoryStorage` with 10,000
    generated tagged files (1,000 artists × 10); after `build`, `open`; `count(Songs) == 10000`;
    `tracks(Songs, 9990, 10)` gives the last 10 in order; a counting storage shows `tracks()`
    reads at most 64 KB, and the heap the `Library` holds after `open` (measured with a counting
    global `operator new` in this test file) stays under 64 KB: no per-track container in RAM.
  - `"a stopped build keeps the old index"`: progress returns false → `open` still gives the old one.
- [ ] **Step 2: Run.** Expected: fail to compile.
- [ ] **Step 3: Implement.** *Our call (the spec leaves the format to the builder):* the index is
  one file: a header (magic `PRLB`, version 1, fingerprint, counts), a string pool, a table of
  fixed-size track records (offsets into the pool), and sorted permutation arrays for each view
  (uint32 indexes), plus artist and album tables with their first-track ranges. `open` reads only
  the header and table positions; rows are read on demand with `File::read`. `build` collects
  records in memory (PSRAM on the board: 10,000 × ~100 B ≈ 1 MB, acceptable), sorts, writes, then
  replaces the old file (`create` writes `.prism-library.new`, then renamed — add
  `virtual bool rename(std::string_view from, std::string_view to)` to `Storage` and both storages).
- [ ] **Step 4: Run.** Expected: pass; the 10,000-track test under 5 s on Coder.
- [ ] **Step 5: Commit.**

### Task 5: Playback chain (decoder, ring, outputs, player)

**Files:** Create `core/audio.hpp` (interfaces), `core/mp3_decoder.hpp/.cpp`, `core/pcm_ring.hpp`,
`core/player.hpp/.cpp`, `tests/test_player.cpp`, `tests/data/sine440_44k.mp3`,
`tests/data/sine440_48k.mp3`, `tests/data/sine440_32k.mp3`, `tests/data/README.md` (how they were
made: `ffmpeg -f lavfi -i sine=f=440:d=1 -ar <rate> -ac 2 -b:a 128k`).

**Interfaces:**
- Consumes: `Storage`, `File`, `Tags` (`audio_offset`), `TrackRow`.
- Produces:
  ```cpp
  struct PcmFormat { int sample_rate; int channels; };      // channels: always 2 out of the decoder
  class PcmOutput {            // a sink that pulls PCM
  public: virtual ~PcmOutput() = default;
      virtual bool start(PcmFormat, PcmRing&) = 0;   // false: cannot play this format
      virtual void stop() = 0;
      virtual bool connected() const = 0;             // false: the headset is gone
      virtual std::uint32_t underruns() const = 0;
      virtual void set_volume(float) = 0;             // 0..1; the sink's own volume when it has one
  };
  class CompressedOutput;      // declared only (the VLSI chip): `virtual bool write(std::span<const std::byte>)`
  class PcmRing;               // SPSC of int16 frames, capacity = 2 s at 48 kHz stereo; pop() fills silence and counts an underrun when short
  class Player {
  public:
      Player(Storage&, PcmOutput&);
      void play(std::vector<TrackRow> queue, std::size_t index);
      void toggle_pause(); void next(); void previous();   // previous: restart if > 3 s in, else the track before
      void set_volume(float);
      void poll();               // call from the logic loop: reads, decodes, refills the ring, handles track end and disconnects
      struct Status { enum class State { Stopped, Playing, Paused } state; std::size_t index;
                      double position_s, duration_s; std::string message; };   // message: "Unsupported sample rate 32000 Hz" etc.
      Status status() const;
  };
  ```
  *Our call:* the spec's reader and decoder threads become `poll()` called from a thread the app
  owns, so `core/` has no thread of its own and tests drive it step by step; the board runs
  `poll()` on its own core-0 thread.

- [ ] **Step 1: Failing tests** (a `FakeOutput` pulls from the ring on demand, counts underruns,
  can be disconnected):
  - `"plays a 44.1 kHz file: the output gets ~1 s of a 440 Hz sine"` (zero crossings ≈ 880 ± 10).
  - `"48 kHz plays; 32 kHz is refused with a message and the next track starts"`.
  - `"a corrupt file in a queue is skipped"` (random bytes named `.mp3`).
  - `"the end of a track starts the next; the end of the queue stops"`.
  - `"a disconnect pauses"`: `FakeOutput` reports `connected() == false` → state Paused.
  - `"underruns are counted when the ring runs dry"`.
  - `"previous within 3 s goes back a track, after 3 s restarts"`.
- [ ] **Step 2: Run.** Expected: fail to compile.
- [ ] **Step 3: Implement** with minimp3 (`mp3dec_decode_frame`), reading the file in 16 KB chunks
  from `audio_offset`; mono duplicated. Reuse PRISM's `dev::SpscRing` for `PcmRing` if its
  interface fits (element type `int16_t`, capacity at construction), else a minimal SPSC.
- [ ] **Step 4: Run.** Expected: pass.
- [ ] **Step 5: Commit.**

### Task 6: Click wheel and the 320×240 UI

**Files:** Create `core/wheel.hpp/.cpp`, `core/ui.hpp/.cpp` (screens and drawing),
`tests/test_wheel.cpp`, `tests/test_ui.cpp`.

**Interfaces:**
- Consumes: `Library`, `ViewKey`, `Player`, `Player::Status`.
- Produces:
  ```cpp
  enum class Button { Menu, PlayPause, Previous, Next, Select };
  struct Rotate { int steps; };            // + clockwise
  using WheelEvent = std::variant<Rotate, Button>;
  // A finger or mouse on a drawn ring: angle in, events out. 24 steps per turn (the iPod's feel:
  // one list row per 15°). Taps: centre = Select; top/bottom/left/right of the ring = Menu,
  // PlayPause, Previous, Next.
  class WheelTracker { public:
      explicit WheelTracker(core::Rect ring);    // PRISM's Rect, in the wheel widget's coordinates
      std::vector<WheelEvent> press(core::Point); std::vector<WheelEvent> move(core::Point); std::vector<WheelEvent> release(core::Point); };
  class PlayerUi {             // the 320×240 screen: a PRISM model with a canvas
  public:
      PlayerUi(Library*, Player&);           // library may be null ("No music")
      void handle(WheelEvent);
      void canvas(render::DrawList&, core::Rect, const ui::WidgetNode&);   // draws the current screen
      void view(app::ViewBuilder&);          // the canvas, min size 320×240
      std::string_view screen_title() const; std::size_t selected() const;   // for tests
  };
  class WheelWidget { /* PRISM canvas model drawing the ring; handle_canvas_input → WheelTracker → PlayerUi::handle */ };
  ```
  Screens: Main (Music, Now Playing, Settings), Music (Artists, Albums, Songs), the library views,
  Now Playing (cover placeholder box, title, artist, album, progress bar, elapsed / -remaining),
  Settings (Output: shown, applied at next start; Bluetooth: "Not available" on the desktop).
  In Now Playing, Rotate changes the volume (5% per step). Lists keep the selection visible and
  read rows with `Library::names/tracks` for the visible window only.

- [ ] **Step 1: Failing tests:**
  - `test_wheel`: `"a full clockwise turn is 24 steps"`, `"counter-clockwise is negative"`,
    `"a tap in the centre is Select; on the top of the ring, Menu"`, `"a drag that starts in the
    centre does not rotate"`.
  - `test_ui`: `"Select on Music then Artists shows the artists"`; `"Menu goes back"`;
    `"rotate past the end stops at the last row"`; `"10,000 songs: 9,999 steps reach the last
    song and only visible rows were read"` (a counting `Library` wrapper or the counting storage
    from Task 4); `"Select on a song plays the album from that song"`; `"no library: the Music
    screen says No music"`; `"drawing a list highlights the selected row"` (render the canvas into
    a `raster::Canvas` 320×240 and check the highlight colour at the selected row's y).
- [ ] **Step 2: Run.** Expected: fail to compile.
- [ ] **Step 3: Implement.** Look: iPod Classic (light theme: white rows, blue highlight bar, a
  title bar with the screen name and a play/pause glyph); text through PRISM's DrawList.
- [ ] **Step 4: Run.** Expected: pass.
- [ ] **Step 5: Commit.**

### Task 7: Desktop app

**Files:** Create `desktop/main.cpp`, `desktop/sdl_pcm_output.hpp/.cpp`, `desktop/meson.build`,
`tests/test_desktop_smoke.cpp`.

**Interfaces:**
- Consumes: everything above; PRISM `app::model_app`, `audio::Output`.
- Produces: the executable `prism-mp3-player-desktop <music folder>`.

- [ ] **Step 1: Failing smoke test** `"the desktop model runs 60 frames headless on a generated
  library"`: build the app's model (a `DesktopApp` struct holding `DirStorage`, `Library`, `Player`,
  `PlayerUi`, `WheelWidget`) against a temp folder of 3 generated MP3s (copies of
  `tests/data/sine440_44k.mp3` with tags written by `tag_builder.hpp`), run it through PRISM's
  headless backend as PRISM's own SDL tests do (`offscreen_env`), send a Select, check the status is
  Playing. Register it as a test with that environment.
- [ ] **Step 2: Run.** Expected: fail to compile.
- [ ] **Step 3: Implement.** `SdlPcmOutput` wraps `audio::Output` (float samples: int16 / 32768,
  times the volume); `connected()` is always true. `main.cpp`: window 600×300: `PlayerUi`'s 320×240
  canvas on the left, `WheelWidget` on the right; keys: Up/Down = Rotate −1/+1, Enter = Select,
  Esc/Backspace = Menu, Space = PlayPause, Left/Right = Previous/Next; mouse wheel = Rotate. At start:
  `Library::open`, else `build` on a background thread while the menus say "Updating library…".
  `Player::poll()` runs on the logic loop (`on_loop`).
- [ ] **Step 4: Run** the smoke test. Expected: pass. Then the whole suite: all pass.
- [ ] **Step 5: Commit**, and update `README.md` with the run command and the keys.

---

**Next plan (after Task 1's numbers):** the Korvo app: SD card (`bsp_sdcard_mount`), FatFS
`Storage`, Bluetooth `PcmOutput` (A2DP + AVRCP), the speaker `PcmOutput`, the 320×240 screen and
touch wheel on the panel, threads per core, then the measurements of the spec (underruns over a
45-minute album while scrolling).
