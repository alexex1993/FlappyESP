# FlappyESP

<img width="800" height="1280" alt="image" src="https://github.com/user-attachments/assets/1e64df17-da40-4029-a426-7216be5d26fa" />


A Flappy-Bird-style game for the **Waveshare ESP32-C6-Touch-LCD-1.47**
(ESP32-C6FH8, 1.47" 172×320 JD9853 IPS panel, AXS5106L capacitive touch).

- Portrait 172×320, targets a steady **60 fps**
- **Tap anywhere** on the screen to flap
- READY → PLAY → GAME OVER loop, best score saved to NVS
- Original pixel art drawn from integer primitives (the C6 has no FPU, and
  bundling the real Flappy Bird sprite sheet would be a copyright problem)
  
## How it develop?

Development with [mcu-skills ](https://github.com/alexex1993/mcu-skills/tree/main/skills/esp32/esp32c6-touch-lcd147)

## Build & flash

```sh
pio run -t upload -t monitor
```

Close the monitor before re-uploading. If nothing enumerates, it is almost
always a charge-only USB-C cable.

## How it renders at 60 fps

A full-screen flush on this board's SPI bus is ~22 ms (≈45 fps hard ceiling),
so the game keeps **no framebuffer**. `compose()` paints the scene from a few
state variables into a small DMA staging buffer, and each frame only the
vertical bands that actually change are re-flushed: one band per pipe, the
bird's fixed column, the ground strip, and the score row (only when the
number changes). That is ~60k px ≈ 3 ms per frame; the loop is paced to
16.667 ms with `esp_timer` (needs `CONFIG_FREERTOS_HZ=1000`).

## Status

Built clean on ESP-IDF 6.1.0. **Not yet run on hardware** — the display and
touch bring-up come from the verified skill template, but the game logic and
the dirty-band flushing on top of it have not been exercised on a board.
Physics constants (`GRAVITY_FP`, `FLAP_VY_FP`, `PIPE_SPEED_FP`, `PIPE_GAP` …
near the top of `firmware/src/main.c`) are first guesses and will want
tuning by feel.

## Layout

```
firmware/
  src/main.c                 game + rendering + hardware bring-up
  include/board_pins.h       Waveshare pin map (do not reuse on the non-touch board)
  include/font5x7.h          5×7 ASCII font (Adafruit GFX, BSD)
  components/jd9853/          JD9853 panel driver + Waveshare init sequence
  components/axs5106l/        AXS5106L touch polling driver
  sdkconfig.defaults         8 MB flash, USB-Serial-JTAG console, 1 ms tick
```
