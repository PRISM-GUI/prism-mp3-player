# prism-mp3-player

A portable MP3 player (iPod Classic style) built on [PRISM](https://github.com/PRISM-GUI/PRISM).
`core/` is the player logic (storage, tags, library, playback), `desktop/` the desktop app and
its directory-backed storage. Everything is in namespace `player`.

## Build

    meson setup builddir
    ninja -C builddir
    meson test -C builddir

PRISM comes in as the subproject `subprojects/prism` (a git wrap; symlink a local PRISM checkout
there to use local changes). Needs GCC 16+ and Meson >= 1.5.

## Run

The desktop app arrives in a later step.
