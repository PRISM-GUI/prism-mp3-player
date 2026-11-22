# prism-mp3-player

A portable MP3 player (iPod Classic style) built on [PRISM](https://github.com/PRISM-GUI/PRISM).
`core/` is the player logic (storage, tags, library, playback), `desktop/` the desktop app and
its directory-backed storage. Everything is in namespace `player`.

## Build

    meson setup builddir
    ninja -C builddir
    meson test -C builddir

PRISM comes in as the subproject `subprojects/prism` (a git wrap; symlink a local PRISM checkout
there to use local changes). Needs GCC 16+ and Meson >= 1.5. PRISM fetches its own dependencies
(SDL3, fonts, doctest...) as Meson wraps; the desktop app needs nothing more. The smoke test
runs on SDL's offscreen video and dummy audio drivers, so the suite needs no display or sound card.

## Run

    builddir/desktop/prism-mp3-player-desktop ~/Music

The window shows the 320×240 screen on the left and the click wheel on the right. The first run
indexes the folder's MP3s (subfolders included) into `<folder>/.prism-library`; the menus say
"Updating library…" meanwhile. Later runs open that index at once and rebuild it in the
background when the folder changed. With no folder given, it prints its usage and exits with 2.

| Key | Wheel |
| --- | --- |
| Up / Down | turn back / forward (one row) |
| Enter | Select (centre button) |
| Esc or Backspace | Menu |
| Space | Play/Pause |
| Left / Right | Previous / Next |

With the mouse: drag around the ring to turn it, click the centre or the ring's four buttons,
or roll the mouse wheel over the click wheel. In Now Playing, turning the wheel sets the volume.

## The Korvo-1 board

The same player runs on an ESP32-S3 Korvo-1 board: SD card, touch wheel, and Bluetooth headphones
or the board's speaker. [boards/korvo1/README.md](boards/korvo1/README.md) has the build, the card
format, pairing and the console status line.
