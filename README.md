# ESP32-S3 Virtual Pet

A Tamagotchi/Digimon-style virtual-pet game for the **Waveshare ESP32-S3-Touch-LCD-2.8**
development board. Built on ESP-IDF with [LovyanGFX](https://github.com/lovyan03/LovyanGFX)
for rendering (C++), with a data-driven, moddable creature roster.

## Hardware

The [Waveshare ESP32-S3-Touch-LCD-2.8](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-2.8):

- **MCU:** ESP32-S3 (dual-core, 240 MHz), 16 MB flash, 8 MB octal PSRAM
- **Display:** 2.8" 240×320 IPS, ST7789 over SPI
- **Touch:** CST328 capacitive, over I2C
- **Also on board:** QMI8658 IMU, PCM5101 I2S audio DAC, PCF85063 RTC (battery-backed),
  microSD slot, Li-ion battery charging + voltage monitor

## Prerequisites

- **[ESP-IDF v6.0.2](https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32s3/get-started/)**
  (target `esp32s3`). Install via the ESP-IDF installer, the VS Code extension, or `git`.
- A working `idf.py` on your `PATH` (after sourcing ESP-IDF's `export.sh` / `export.ps1`).

## Getting the code

LovyanGFX is a **git submodule** (pinned to v1.2.25), so clone recursively:

```sh
git clone --recursive <this-repo-url>
```

Already cloned without `--recursive`? Pull the submodule in:

```sh
git submodule update --init --recursive
```

## Build & flash

```sh
# one-time, per checkout:
idf.py set-target esp32s3

# build (the first build fetches the cJSON managed component — needs network once):
idf.py build

# flash + open the serial monitor (replace COMx / /dev/ttyUSBx with your port):
idf.py -p COMx flash monitor
```

**Faster iteration:** once flashed, use `idf.py -p COMx app-flash` — it flashes only the
app partition, skipping the bootloader, partition table, and the 1 MB `creatures` data
image (which otherwise adds ~6.5 s to every flash). Use a full `flash` only when
`partitions.csv`, the bootloader, or `flash_creatures/` change.

> This board flashes over the ESP32-S3's **native USB-Serial/JTAG** — no external
> USB-UART bridge. The `-b <baud>` flag has no effect here (USB runs at its own speed).

## Project layout

```
main/
  main.cpp                app_main: brings up drivers, then starts the game task
  CMakeLists.txt          component sources + include dirs
  game/
    core/     app, game, scene       — composition root, main loop, scene framework
    engine/   display, gfx, input,   — LovyanGFX device, back-buffer + draw helpers,
              clock, drivers           touch input, RTC clock, C-driver bridge
    sim/      pet, creatures, save    — pet simulation, creature roster, NVS persistence
    scenes/   care/ minigames/ menus/ — game screens, grouped by kind
    assets/   sprites.hpp, tiles.hpp  — GENERATED baked bitmaps (see tools/)
  drivers/    ST7789, CST328, QMI8658, PCM5101, PCF85063, SD_MMC, BAT, PWR_Key, ...
              — vendor C drivers for the board peripherals
art/          tiles/                  — SOURCE art (PNG) baked into headers by tools/
flash_creatures/  <id>/creature.json + sprite.png
              — base creature roster, packed into the `creatures` flash partition
tools/        *.py                    — asset generators (see below)
components/   LovyanGFX (submodule), chmorgan__esp-* (vendored audio)
```

Includes inside `game/` are folder-qualified from the `game/` root
(e.g. `#include "engine/gfx.hpp"`, `#include "sim/pet.hpp"`); same-folder includes stay flat.

## Assets & generators (`tools/`)

Baked bitmaps are checked in as generated C headers under `main/game/assets/`; regenerate
them from source with the pure-Python (no dependencies) scripts in `tools/`:

- **`gen_tiles.py`** — bakes `art/tiles/*.png` → `main/game/assets/tiles.hpp`
  (the grass + dirt ground tiles). Run: `python tools/gen_tiles.py`
- **`gen_fallback_sprite.py`** — generates `sprites.hpp`, the "?" placeholder drawn
  whenever a creature's real sprite can't be shown.
- `png2c.py`, `gen_placeholder.py` — earlier sprite helpers.

## Creatures (data-driven + moddable)

Each creature is a folder of `creature.json` + a sprite PNG. Base creatures live in
`flash_creatures/`, packed at build time into a read-only FAT image on the `creatures`
flash partition (mounted at `/creatures`). Extra creatures can be dropped on an SD card
under `/sdcard/creatures/` — the card overlays the base game, winning on `id` collision.

## Configuration

`sdkconfig.defaults` holds the intentional config (octal PSRAM, 16 MB flash, 240 MHz,
1 kHz FreeRTOS tick, `-O2`, custom partition table, two FAT volumes). The full `sdkconfig`
is generated from it and is **not** tracked — run `idf.py set-target esp32s3` after a fresh
clone to regenerate it.

## Dependencies

- **LovyanGFX** — rendering. Git submodule at `components/LovyanGFX` (pinned v1.2.25).
- **chmorgan/esp-audio-player, esp-libhelix-mp3** — audio, vendored in `components/`.
- **espressif/cjson** — creature-config parsing. Managed component, fetched from
  `idf_component.yml` + `dependencies.lock` into `managed_components/` on first build.
