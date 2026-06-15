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
// We deliberately do NOT touch the TFT/display at all (per project requirement:
// report everything over serial, no display facility).

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
