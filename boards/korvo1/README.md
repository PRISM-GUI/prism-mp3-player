# The player on the Korvo-1 board

The same player as the desktop app, on an ESP32-S3 Korvo-1 board. The card holds the music, the
screen and the touch wheel are the player's, and the sound goes to Bluetooth headphones or to the
board's own speaker and headphone jack.

## Build

Needs ESP-IDF in the environment, and the submodules: the board support lives in
`subprojects/prism-esp32`, which brings its own PRISM checkout.

    git submodule update --init --recursive
    . /path/to/esp-idf/export.sh
    scripts/build-korvo.sh

The script builds `boards/korvo1` for `esp32s31` and merges the bootloader, the partition table and
the app into one image, `build/player.bin`.

## Flash

    esptool.py --chip esp32s31 write_flash 0x0 build/player.bin

The project's web flasher takes the same image in a browser.

## The SD card

The card holds a FAT32 or an exFAT volume; anything else has to be reformatted. A card whose
filesystem cannot be read leaves the screen on "SD card not readable (FAT32 or exFAT)" and the
console on `sd card: ESP_FAIL`; with no card at all the screen says "No SD card".

Reformatting on Linux, FAT32 or exFAT:

    sudo mkfs.vfat -F 32 -n MUSIC /dev/sdX1
    sudo mkfs.exfat -n MUSIC /dev/sdX1

Put the music anywhere on the card; folders are browsable as they are. The first boot walks the
card and writes the index `/.prism-library`. Later boots open that index at once and rebuild it in
the background when the card changed, so a big card is slow to index only the first time.

### The FatFS override

`boards/korvo1/components/fatfs` is ESP-IDF's own fatfs component, pinned at ESP-IDF `ad20ded`, with
one change: `FF_FS_EXFAT 1` in `src/ffconf.h`, because ESP-IDF ships it with exFAT off. A component
in `components/` wins over the one in ESP-IDF, so this copy is what the firmware links against.

When ESP-IDF is bumped, copy the whole component from the new IDF (it carries the FatFs fixes
ESP-IDF records in its `sbom.yml`) and re-apply that one line.

## The headset

Bluetooth Classic, A2DP. Put the headset in pairing mode the first time: the board takes the first
device it finds whose name contains the configured name, `CONFIG_PLAYER_BT_NAME` ("WH-1000XM3" by
default, changed under `Player` in `idf.py menuconfig`). Once paired the headset is remembered in
flash and the board retries it every few seconds while it is away, so it comes back on its own.

Audio is 44.1 kHz stereo. A track at another rate is skipped with "Output cannot play 48000 Hz" on
Bluetooth; the speaker plays 44.1 and 48 kHz.

The headset's own play, pause, next and previous buttons drive the player.

## Speaker

Settings → Output chooses Bluetooth or the board's speaker. The choice is written to flash and
applied at the next boot, not straight away.

## Controls

Touch wheel: the ring scrolls, the centre is Select, the top is Menu, the bottom is Play/Pause, the
left and right are Previous and Next. In Now Playing, the ring sets the volume.

Board buttons: MODE is Menu, VOL+ and VOL− scroll down and up.

## The console

Every 5 s the firmware prints one line:

    player: playing out bluetooth Connected to WH-1000XM3 underruns 0 internal free 91234 largest 57344

- `playing` / `paused` / `stopped`: what the player is doing.
- `out bluetooth` or `out speaker`: the output chosen at boot.
- `Connected to <name>`, `Scanning…`, `Connecting…`, `Off`, or `on` for the speaker.
- `underruns`: how often the ring ran dry since boot. It should stay at 0; a rising count means
  the output cannot keep up.
- `internal free` and `largest`: internal RAM free, and the largest block still available. The
  largest block is the one that matters.

Around it the firmware prints `sd card: …`, `heap after bluetooth: …`, `heap after library: …`,
`bt: <call> failed: <error>` and `speaker: <call> failed: <error>`.

## Known limits

- 44.1 kHz only over Bluetooth.
- Over Bluetooth the volume scales the samples in software, so it cannot be louder than the
  headset's own setting.
- The folder browser reads the tags of the 32 files around the selection; tracks further out show
  their file names.
