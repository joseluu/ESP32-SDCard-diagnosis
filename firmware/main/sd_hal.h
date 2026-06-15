// sd_hal.h — host abstraction over ESP-IDF sdspi/sdmmc.
//
// Goal: bring up a (possibly failing) SD card, expose the IDF-populated
// sdmmc_card_t plus the *raw* register blobs, and provide raw command access
// for ACMD13 (SSR), CMD6 (SWITCH_FUNC), and CMD13 (card status).

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "sdmmc_cmd.h"
#include "sd_registers.h"

typedef struct {
    bool         initialized;
    const char  *backend;       // "sdspi" or "sdmmc"
    sdmmc_card_t card;          // IDF-populated card structure
    sd_raw_t     raw;           // raw register blobs

    // Negotiated bus characteristics (best effort).
    int          bus_width;     // 1 for SPI
    int          freq_khz;      // negotiated clock

    // Last error context for reporting on a failing card.
    esp_err_t    last_err;
    char         last_err_stage[48];
} sd_hal_t;

// Initialise the SPI bus + card. Returns ESP_OK on success. On failure, the
// struct still carries last_err / last_err_stage so the caller can report why.
esp_err_t sd_hal_init(sd_hal_t *h);

// Tear down (frees the SPI host/bus). Safe to call after a failed init.
void sd_hal_deinit(sd_hal_t *h);

// Block I/O (read is always available; write only if backend supports it).
esp_err_t sd_hal_read_blocks(sd_hal_t *h, uint32_t lba, void *buf, uint32_t count);
esp_err_t sd_hal_write_blocks(sd_hal_t *h, uint32_t lba, const void *buf, uint32_t count);

// Raw register capture. Called by init; can be re-run on demand.
// Fills h->raw from the live card (CID/CSD from the card struct + raw blobs,
// SCR/SSR via ACMD51/ACMD13, OCR from init, CMD6 via SWITCH_FUNC check mode).
esp_err_t sd_hal_capture_raw(sd_hal_t *h);

// CMD13 SEND_STATUS: returns the 32-bit R1 card status.
esp_err_t sd_hal_card_status(sd_hal_t *h, uint32_t *status_out);

// CMD6 SWITCH_FUNC in *check* mode (read-only, safe). Fills 64-byte status.
esp_err_t sd_hal_switch_func_check(sd_hal_t *h, uint8_t status64[64]);

// ACMD13 SD_STATUS (SSR), 64-byte data block.
esp_err_t sd_hal_read_ssr(sd_hal_t *h, uint8_t ssr64[64]);
