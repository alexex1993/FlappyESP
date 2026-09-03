# FlappyESP

<img width="450" height="640" alt="image" src="https://github.com/user-attachments/assets/1e64df17-da40-4029-a426-7216be5d26fa" />


A Flappy-Bird-style game for Waveshare's two 1.47" ESP32-C6 boards. One
source tree, one PlatformIO env per board:

| env | board | panel | flash | input |
|---|---|---|---|---|
| `esp32-c6-touch-lcd-1_47` | ESP32-C6-**Touch**-LCD-1.47 (SKU 31203) | JD9853 | 8 MB | tap the glass, or BOOT |
| `esp32-c6-lcd-1_47` | ESP32-C6-LCD-1.47 (SKU 28563) | ST7789 | 4 MB | **BOOT button only** |

The two boards share a name, a chip family and a 172×320 panel size and
almost nothing else — different controller, different flash size, and only
three pins in common — so the pin map and the feature switches live per board
in `boards/<name>/board_pins.h`. Everything else is shared.

- Portrait 172×320, targets a steady **60 fps**
- READY → PLAY → GAME OVER loop, best score saved to NVS
- Original pixel art drawn from integer primitives (the C6 has no FPU, and
  bundling the real Flappy Bird sprite sheet would be a copyright problem)
  
## How it develop?

Development with [mcu-skills](https://github.com/alexex1993/mcu-skills) — one
skill per board:
[esp32c6-touch-lcd147](https://github.com/alexex1993/mcu-skills/tree/main/skills/esp32/esp32c6-touch-lcd147)
and [esp32c6-lcd147](https://github.com/alexex1993/mcu-skills/tree/main/skills/esp32/esp32c6-lcd147).

## Build & flash

Flash and open the serial monitor:

```sh
pio run -e esp32-c6-touch-lcd-1_47 -t upload -t monitor   # touch board
pio run -e esp32-c6-lcd-1_47       -t upload -t monitor   # non-touch board
```

Drop `-t monitor` to flash without attaching; drop both `-t` flags to only
build. `pio run` with no arguments builds both envs.

**`-e` is not optional when flashing.** Without it `pio run -t upload` uploads
every env in turn and the board keeps whichever went last — and flashing the
wrong board's firmware gives a dark screen rather than an error, because none
of the pin numbers overlap.

Nothing to press: the Type-C port goes straight into the SoC's USB-Serial-JTAG,
so esptool drives reset and download mode itself. Close the monitor (`Ctrl+C`)
before re-uploading, or the port is still held.

If the board never shows up as `/dev/cu.usbmodem*` (Linux `/dev/ttyACM*`,
Windows a COM port) it is almost always a charge-only USB-C cable. If firmware
has wedged USB, hold **BOOT**, tap **RESET**, release **BOOT** to force the ROM
download loader — bad firmware cannot brick either board.

## How it renders at 60 fps

A full-screen flush on either board's SPI bus is ~22 ms (≈45 fps hard ceiling),
so the game keeps **no framebuffer**. `compose()` paints the scene from a few
state variables into a small DMA staging buffer, and each frame only the
vertical bands that actually change are re-flushed: one band per pipe, the
bird's fixed column, the ground strip, and the score row (only when the
number changes). That is ~60k px ≈ 3 ms per frame; the loop is paced to
16.667 ms with `esp_timer` (needs `CONFIG_FREERTOS_HZ=1000`).

## Status

Both envs build clean on ESP-IDF 6.1.0. **Not yet run on hardware** — each
board's display bring-up and the touch bring-up come from the verified skill
templates, but the game logic and the dirty-band flushing on top of them have
not been exercised on a board. The non-touch env in particular has only ever
been compiled. Physics constants (`GRAVITY_FP`, `FLAP_VY_FP`,
`PIPE_SPEED_FP`, `PIPE_GAP` … near the top of `src/main.c`) are first guesses
and will want tuning by feel.

## Layout

```
platformio.ini                     one env per board
CMakeLists.txt                     maps FLAPPY_BOARD -> board dir + sdkconfig
src/main.c                         game + rendering + hardware bring-up
src/CMakeLists.txt                 picks the board include dir and drivers
boards/touch_lcd147/board_pins.h   touch board: pins + BSP_HAS_TOUCH etc.
boards/lcd147/board_pins.h         non-touch board: same macros, other pins
include/font5x7.h                  5x7 ASCII font (Adafruit GFX, BSD)
components/jd9853/                 JD9853 panel driver (touch board only)
components/axs5106l/               AXS5106L touch polling driver (touch board only)
sdkconfig.defaults                 shared: USB-Serial-JTAG console, 1 ms tick
sdkconfig.defaults.touch_lcd147    8 MB flash
sdkconfig.defaults.lcd147          4 MB flash
```

The ST7789 driver for the non-touch board ships inside ESP-IDF's `esp_lcd`,
so nothing is vendored for it.

## Adding a third board

Drop a `boards/<name>/board_pins.h` defining the same `BSP_*` macros, add a
`sdkconfig.defaults.<name>`, and add an env passing
`board_build.cmake_extra_args = -DFLAPPY_BOARD=<name>`. `src/main.c` switches
only on `BSP_HAS_TOUCH` and `BSP_PANEL_JD9853`; it never names a board.
