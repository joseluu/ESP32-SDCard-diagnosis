// config.h — build-time flags and pin maps for the SD-Card Diagnosis tool.
//
// Target board: CYD ESP32-2432S032C (classic ESP32). Its micro-SD slot is wired
// to a dedicated SPI bus, NOT the TFT bus. The widely documented CYD SD pinout:
//
//     SD_CS   = GPIO5
//     SD_MOSI = GPIO23
//     SD_MISO = GPIO19
//     SD_SCK  = GPIO18
//
// The serial console is the primary interface; the optional LVGL touchscreen
// UI below (SDDIAG_UI) additionally drives the TFT/GT911 for standalone use.

#pragma once

// ---------------------------------------------------------------------------
// SD access backend
// ---------------------------------------------------------------------------
// The CYD routes the SD slot over SPI only (no 4-bit SDMMC wiring exposed), so
// the SPI backend is the working default. SDMMC is left as a compile option for
// boards that wire it (ESP32/S3 with the slot on the SDMMC peripheral).
#define SDDIAG_BACKEND_SPI    1
#define SDDIAG_BACKEND_SDMMC  0

// ---------------------------------------------------------------------------
// SPI pin map (CYD ESP32-2432S032C SD slot)
// ---------------------------------------------------------------------------
#define SDDIAG_SPI_HOST   SPI2_HOST   // VSPI on the classic ESP32
#define SDDIAG_PIN_CS     5
#define SDDIAG_PIN_MOSI   23
#define SDDIAG_PIN_MISO   19
#define SDDIAG_PIN_SCK    18

// Initial SPI clock for probing a possibly-failing card. We start slow for
// reliability during identification, then a benchmark may push higher.
#define SDDIAG_PROBE_KHZ        400     // SD spec init clock (<=400 kHz)
#define SDDIAG_RUN_KHZ          20000   // 20 MHz nominal run clock over SPI

// ---------------------------------------------------------------------------
// Touchscreen UI (LVGL) — optional, for standalone use without a host PC.
// Wiring from the Arduino demo repo (Demo-CYD-3.2inch-ESP32-2432S032):
// TFT ST7789 on its own SPI bus (the SD is on SPI2/pins 5-23-19-18, no clash),
// GT911 capacitive touch on I2C. Set SDDIAG_UI to 0 for a serial-only build.
// ---------------------------------------------------------------------------
#define SDDIAG_UI             1

#define SDDIAG_TFT_SPI_HOST   SPI3_HOST
#define SDDIAG_PIN_TFT_SCK    14
#define SDDIAG_PIN_TFT_MOSI   13
#define SDDIAG_PIN_TFT_CS     15
#define SDDIAG_PIN_TFT_DC     2
#define SDDIAG_PIN_TFT_RST    (-1)     // panel reset tied to board EN
#define SDDIAG_PIN_TFT_BL     27      // backlight, active high
#define SDDIAG_TFT_CLK_HZ     (40 * 1000 * 1000)

// The panel is a native-portrait 240x320 ST7789 (IPS, colours inverted), used
// here in portrait, rotation 2 (MADCTL MX|MY — rotation 0 was upside-down on
// this board). For the other portrait, zero both mirrors and see the touch note.
#define SDDIAG_TFT_HRES       240
#define SDDIAG_TFT_VRES       320
#define SDDIAG_TFT_SWAP_XY    0
#define SDDIAG_TFT_MIRROR_X   1
#define SDDIAG_TFT_MIRROR_Y   1

#define SDDIAG_PIN_TOUCH_SDA  33
#define SDDIAG_PIN_TOUCH_SCL  32
#define SDDIAG_PIN_TOUCH_RST  25
#define SDDIAG_PIN_TOUCH_INT  (-1)    // not wired on this board
// The GT911 factory config reports RAW coordinates in a portrait-native
// 240x320 frame aligned with display rotation 0 (established empirically,
// axis by axis). esp_lcd_touch applies the mirrors on the RAW axes first,
// then the swap; x_max/y_max are the raw frame. Display rotation 2 (current)
// is 180° from the raw frame, so both mirrors; rotation 0 would need none.
#define SDDIAG_TOUCH_RAW_X    240
#define SDDIAG_TOUCH_RAW_Y    320
#define SDDIAG_TOUCH_SWAP_XY  0
#define SDDIAG_TOUCH_MIRROR_X 1
#define SDDIAG_TOUCH_MIRROR_Y 1

// ---------------------------------------------------------------------------
// Safety: destructive tests
// ---------------------------------------------------------------------------
// Off unless explicitly defined at build time AND confirmed at runtime.
#ifndef CONFIG_SDDIAG_ALLOW_DESTRUCTIVE
#define CONFIG_SDDIAG_ALLOW_DESTRUCTIVE 0
#endif

// ---------------------------------------------------------------------------
// Diagnostics tuning
// ---------------------------------------------------------------------------
#define SDDIAG_SCAN_CHUNK_SECTORS   128   // 64 KiB chunks for the surface scan
#define SDDIAG_BENCH_SEQ_SECTORS    256   // 128 KiB sequential bench transfer
#define SDDIAG_BENCH_RAND_IOPS_OPS  256   // random 4 KiB ops for IOPS measure
