# PRISM MP3 player — prototype

**Status:** Draft, for the user's approval.

## Why

The user will build a portable music player around a bare ESP32-S31: an updated iPod Classic.
Before designing that board, this prototype answers two questions on the ESP32-S31-Korvo-1
(the same chip, already running PRISM):

1. **Can the chip do it all at once?** Read MP3 files from an SD card, decode them, stream them
   to a Bluetooth headset (the user's Sony WH-1000XM3), and run a PRISM UI, within its internal
   RAM and CPU.
2. **What should the UI be?** Start shaping the player's screens and its click-wheel feel early,
   on the desktop and on the board.

"Updated" here means: a colour screen with album covers, and Bluetooth headphones next to a
wired output. More can come later.

## Goals

- iPod Classic experience: nested menus (Music → Artists → Albums → Songs), Now Playing with
  cover, title, artist, progress; driven by a click wheel (rotate to scroll, centre to select,
  Menu / Play-Pause / Previous / Next on the ring).
- The player's screen is **320×240**, the size of the future screen, whatever the panel it is
  shown on.
- Music from the SD card's MP3 files, organized by their ID3 tags.
- Playback to the WH-1000XM3 over Bluetooth Classic (A2DP, SBC), with pairing from the UI and
  reconnection at boot.
- **Portable:** the player's logic and UI are plain C++ on PRISM, with no ESP-IDF call; the
  hardware sits behind a board layer, so another MCU is a new board layer, not a rewrite.
- A **desktop build** of the same UI with audio through the PC, for fast UI work.
- Measured on the board: internal RAM left, CPU per core, and no audible dropout over a whole
  album with the UI in use.

## Non-goals (this prototype)

- The VLSI codec (likely a VS1073) and the user's own board: no board exists yet. The design
  leaves room for it (below), no driver is written.
- AAC to the headset (better sound than SBC; ESP-IDF has an example): after SBC works.
- Battery, power management, USB mass storage, streaming services, playlists editing, gapless
  playback, an equalizer.
- An Embassy-like hardware abstraction for PRISM: a PRISM-wide topic for later.

## Anti-goals

- A UI drawn for the Korvo's 800×480 panel: it would not carry over to the real player.
- Audio that drops out when the UI is busy: a player that stutters fails, however good it looks.
- Player code that calls ESP-IDF directly: it would tie the product to one chip family.

## Constraints

- ESP32-S31, ESP-IDF pinned at ad20ded (prism-esp32), PRISM on main, C++ as PRISM builds it.
- **Internal RAM is the scarce resource.** On the Korvo, the display's two 48-row bands
  (2 × 75 KB) and the render thread's stack already take most of it; the scope needed small
  allocations moved to PSRAM to start. Bluetooth Classic needs its controller and host memory on
  top. Bluedroid can allocate from PSRAM first (`CONFIG_BT_ALLOCATION_FROM_SPIRAM_FIRST`).
- Builds run on the Coder workspace `prism`, one per build directory.

## Approaches considered

- **A. Our own small audio chain, a portable core, and a desktop build (chosen).** File reader →
  MP3 decoder → output; ESP-IDF's Bluedroid for A2DP. We write a few hundred lines of audio
  code; in return we control every allocation, which is where this prototype can fail, and the
  core moves to another MCU unchanged.
- **B. ESP-ADF pipelines.** Ready-made "file → MP3 → Bluetooth"; large, owns its tasks and
  memory, and likely not supporting the S31 on our ESP-IDF yet. Rejected: opaque exactly where
  the risk is, and ties the code to Espressif.
- **C. Like A, UI on the board only.** Rejected: every UI change would need a flash.

## Design

### Project

A new repository, **prism-mp3-player** (local first; GitHub when the user says so):

```
core/        portable C++: library, tags, queue, player state, UI (PRISM widgets)
boards/
  korvo1/    the ESP-IDF app: SD, Bluetooth, the Korvo's codec, the click wheel on the panel
desktop/     SDL window: the 320×240 screen and a wheel; a folder of MP3s as the "SD card"
tests/       doctest, host: tags, library, queue, player state, wheel, UI pixels
```

*Our call:* the Korvo app reuses prism-esp32's board code (`boards/korvo1`: display, touch,
buttons, port hooks) by taking prism-esp32 as a git submodule and adding that directory as an
ESP-IDF component. PRISM comes the same way prism-esp32 takes it.

### The board layer (what core needs from a board)

Small interfaces, one implementation per board; the desktop has its own.

- **Storage:** list a directory, open and read a file (the SD card through FatFS on the board, a
  folder on the desktop).
- **Audio outputs**, two kinds, because of the VLSI chip:
  - `PcmOutput`: takes 16-bit stereo PCM at the file's rate. Bluetooth (A2DP, SBC encoded by
    Bluedroid), the Korvo's own codec as a stand-in for wired, the PC's sound card (PRISM's
    `audio::Output`).
  - `CompressedOutput`: takes the file's bytes as they are. The future VLSI chip decodes MP3
    itself; the MCU only streams bytes to it over SPI. Declared now, implemented with the board.
- **Bluetooth control:** scan, pair, connect, disconnect, status; the bonded headset kept in NVS.
  *Our call:* the player is also an AVRCP target: the headset's own play/pause and skip buttons
  drive the player, and volume is AVRCP absolute volume, so the wheel and the XM3's touch
  controls change the same level. Software volume (scaling the PCM) is used only where a sink has
  no volume of its own.
- **Wheel input:** events `Rotate(steps)` and `Press(Menu | PlayPause | Previous | Next |
  Select)`. On the Korvo, a wheel drawn on the touch panel produces them; on the desktop, the
  mouse wheel and keys; on the real player, the S31's capacitive touch pads.

### Playback chain

1. A reader thread fills a ring buffer in PSRAM from the file (large reads, so the SD card
   stays streaming).
2. For a `PcmOutput`, a decode thread turns MP3 frames into PCM into a second PSRAM ring.
   The output pulls from it (A2DP's data callback on the board, SDL's audio callback on the
   desktop). The rings absorb SD latency and UI load: *our call*, 2 s of PCM.
3. For a `CompressedOutput`, the decode step is skipped.

*Our call:* sample rates 44.1 and 48 kHz play; other rates (32, 22.05 kHz: old rips, podcasts)
are listed but refused with a message in this prototype. A resampler is later work.

*Our call:* a dropout is counted, not listened for: each `PcmOutput` counts underruns (the sink
asked for data the ring did not have). Step 4 passes with zero underruns over a 45-minute album
while the wheel scrolls the Songs list continuously and covers load.

*Our call:* the decoder is **minimp3** (public domain, single header, portable), behind a
`Decoder` interface. If it costs too much CPU on the S31, Espressif's `esp_audio_codec` replaces
it on that board only.

*Our call:* threads and cores on the Korvo: render on core 1 (the SIMD core, as now); logic,
reader and decoder on core 0; Bluedroid's tasks where ESP-IDF puts them. Measured, then moved.

### Library

- At boot, scan `/music` on the card (recursively) for `.mp3` files and read their ID3v2 and
  ID3v1 tags: title, artist, album, track number, and where the cover image is.
- **Size: a large card, gigabytes of MP3, 10,000 tracks or more** (the user's answer). So:
  - the index lives on the card (`/music/.prism-library`), sorted by artist, album and title;
    menus read the rows they show from it, and only a small cache sits in RAM (PSRAM);
  - it is rebuilt only when the card's files change (names, sizes, dates), and a rebuild runs in
    the background: the old index stays usable and playback never waits for the scan;
  - on the desktop, a test folder with generated tags stands in for a full card, so the
    host tests run the 10,000-track case.
- Menus come from the index: Artists → Albums → Songs, Albums → Songs, Songs (all).
- Covers: the ID3 picture (JPEG) decoded to the size shown; by the S31's hardware JPEG decoder
  on the board, by PRISM's image loading on the desktop. *Our call:* a picture the hardware
  refuses (progressive JPEG, PNG) shows a placeholder in this prototype; covers decode on their
  own low-priority thread, never on the audio path.

### UI (320×240)

- **Main menu:** Music, Now Playing, Settings (Bluetooth, Output).
- **Output (the user's answer: the simplest):** one output, chosen in Settings and applied at the
  next boot; no switching while a song plays. If the Bluetooth headset drops, playback pauses,
  as an iPod pauses when the headphones are pulled.
- **Lists:** the iPod's: a title bar (screen name, play state, battery placeholder), a highlighted
  row moved by the wheel, Select to enter, Menu to go back.
- **Now Playing:** cover, title, artist, album, a progress bar with elapsed and remaining time;
  the wheel sets the volume; Previous/Next change track; Play/Pause.
- **Bluetooth:** scan results, pick a headset, connection state; the bonded headset reconnects at
  boot.

On the Korvo, the 800×480 panel shows the 320×240 screen and, beside it, the wheel drawn as a
touch control (finger around the ring = rotate, taps = buttons). Only the 320×240 screen is the
product; the wheel stands in for hardware.

### Order

1. **Spike (throwaway):** Bluetooth (A2DP and AVRCP) playing a test tone to the WH-1000XM3
   while PRISM draws a 320×240 screen on the Korvo, with bands sized for that screen (320 wide,
   about 2 × 30 KB) and Bluedroid allocating from PSRAM first. *Our call:* it passes when, with
   the headset connected and playing, at least 64 KB of internal RAM is free and the largest
   free block is at least 32 KB (room for the decoder's and reader's stacks). If not, stop and
   rethink before anything else.
2. Core: tags, library index, queue, player state, wheel; host tests.
3. Desktop: the UI on a folder of MP3s, sound through the PC.
4. Korvo: SD, decoding, Bluetooth output, the touch wheel; board measurements.
5. Covers, then AAC if SBC sounds too poor.

## Risks

- **Internal RAM** (above). The spike exists for it.
- **CPU on core 0:** MP3 decoding (~10–20% of one core on similar chips) plus PRISM's logic
  thread plus Bluedroid's SBC encoding. Measured with the trace page.
- **SD read stalls** (card garbage collection: hundreds of ms): the 2 s rings cover them.
- **Bluetooth on ESP-IDF master for the S31:** new, possibly buggy; the example says it supports
  the chip.

## Left to the builder

- Exact interface names and the threading primitives (PRISM's `Channel`/`Shared` where they fit).
- ID3 parsing details (versions 2.3 and 2.4, text encodings).
- The index file's format.
- How the Korvo app shows a 320×240 screen region on the 800×480 panel.
