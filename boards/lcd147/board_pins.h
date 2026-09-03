/*
 * Waveshare ESP32-C6-LCD-1.47 board map — the NON-touch board (SKU 28563).
 *
 * Source: the vendor pinout table
 * (https://www.waveshare.com/wiki/ESP32-C6-LCD-1.47), as digested by the
 * esp32c6-lcd147 skill, whose display path is hardware-verified.
 *
 * NOTE: not one of these numbers matches the ESP32-C6-Touch-LCD-1.47.  The
 * two boards share a name, a chip family and a panel size, and nothing else:
 * a ST7789 instead of a JD9853, a 4 MB C6FH4 instead of an 8 MB C6FH8, no
 * touch controller, no IMU.  Only LCD_CS, LCD_DC and SD_CS agree.  Do not
 * copy pins between this header and boards/touch_lcd147/board_pins.h — the
 * wrong map gives a dark screen, not an error.
 */

#pragma once

/* --- what the game asks the board about ------------------------------- */
#define BSP_BOARD_NAME      "ESP32-C6-LCD-1.47"
#define BSP_HAS_TOUCH       0             /* no glass: BOOT is the only input */
#define BSP_PANEL_ST7789    1

/* Hardware-damage rule, not a taste one: this board wires the panel's LEDs
 * straight to GPIO22, and Waveshare warn that sustained duty above 50 %
 * overheats the panel into permanent dark shadows. Never raise this. */
#define BSP_BL_MAX_PCT      50

/* On-screen prompts: BOOT is the whole input surface here. */
#define BSP_HINT_START      "PRESS BOOT"
#define BSP_HINT_RETRY      "BOOT TO RETRY"

/* --- 1.47" IPS panel, ST7789, 4-wire SPI ------------------------------ */
#define BSP_LCD_SPI_HOST    SPI2_HOST
#define BSP_LCD_SCLK        GPIO_NUM_7    /* shared with the TF card */
#define BSP_LCD_MOSI        GPIO_NUM_6    /* shared with the TF card */
#define BSP_LCD_CS          GPIO_NUM_14
#define BSP_LCD_DC          GPIO_NUM_15
#define BSP_LCD_RST         GPIO_NUM_21
#define BSP_LCD_BL          GPIO_NUM_22   /* drives the LEDs directly; LEDC PWM */

#define BSP_LCD_H_RES       172
#define BSP_LCD_V_RES       320
#define BSP_LCD_PIXEL_CLK   (40 * 1000 * 1000)

/* The 172-pixel glass is centred in the ST7789's 240-column RAM:
 * (240 - 172) / 2 = 34. Without the gap the image is shifted and wraps. */
#define BSP_LCD_X_GAP       34
#define BSP_LCD_Y_GAP       0

/* --- Buttons ---------------------------------------------------------- */
/* Same net name and same polarity as the touch board, and by luck the same
 * pin: Key -> GPIO9, pulled up, pressed = low. GPIO9 is a strapping pin, but
 * only at reset; reading it as a plain input afterwards is fine. */
#define BSP_BOOT_BTN        GPIO_NUM_9    /* pressed = low */

/* --- TF card (not used here, but its CS must stay high on the shared bus) */
#define BSP_SD_CS           GPIO_NUM_4
#define BSP_SD_MISO         GPIO_NUM_5
#define BSP_SD_MOSI         BSP_LCD_MOSI
#define BSP_SD_SCLK         BSP_LCD_SCLK

/* --- WS2812 RGB LED (unused by the game) ------------------------------ */
/* GPIO8 is a strapping pin: safe at the factory eFuse settings, but never
 * pull it low at reset. Wire order is GRB, and it is far brighter than its
 * numbers suggest under the clear acrylic. */
#define BSP_RGB_LED         GPIO_NUM_8

/* --- Free pins -------------------------------------------------------- */
/* GPIO0-3 are the only ADC channels and the only deep-sleep wake pins left;
 * GPIO18, 19, 20, 23 are free digital. */
