// sd_hal.c — SPI (sdspi) backend for the SD-Card Diagnosis tool.
//
// ESP-IDF's sdmmc_card_init() brings the card up and decodes CID/CSD/SCR/OCR
// into sdmmc_card_t, but it does NOT keep the *raw* register bytes. For
// byte-for-byte parity with Linux sysfs (and to run our own superset decoder),
// we re-issue the raw register commands ourselves via the private sdmmc API:
//   CMD10 (SEND_CID), CMD9 (SEND_CSD), ACMD51 (SEND_SCR), ACMD13 (SD_STATUS),
//   CMD6 (SWITCH_FUNC, check mode), CMD13 (SEND_STATUS).

#include "sd_hal.h"
#include "config.h"

#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "esp_private/sdmmc_common.h"   // sdmmc_send_cmd, sdmmc_send_app_cmd, flip

// Opcodes (sd_protocol_defs.h values).
#define OP_SEND_CSD     9    // CMD9  R2 (AC) / data read (SPI)
#define OP_SEND_CID     10   // CMD10 R2 (AC) / data read (SPI)
#define OP_SEND_STATUS  13   // CMD13 R1
#define OP_SWITCH_FUNC  6    // CMD6
#define OP_SD_STATUS    13   // ACMD13 (after CMD55)
#define OP_SEND_SCR     51   // ACMD51

static void note_err(sd_hal_t *h, const char *stage, esp_err_t e)
{
    h->last_err = e;
    strncpy(h->last_err_stage, stage, sizeof(h->last_err_stage) - 1);
    h->last_err_stage[sizeof(h->last_err_stage) - 1] = '\0';
}

// ---------------------------------------------------------------------------
// init / deinit
// ---------------------------------------------------------------------------
esp_err_t sd_hal_init(sd_hal_t *h)
{
    memset(h, 0, sizeof(*h));
    h->backend = "sdspi";
    h->bus_width = 1;

    spi_bus_config_t buscfg = {
        .mosi_io_num = SDDIAG_PIN_MOSI,
        .miso_io_num = SDDIAG_PIN_MISO,
        .sclk_io_num = SDDIAG_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t err = spi_bus_initialize(SDDIAG_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        note_err(h, "spi_bus_initialize", err);
        return err;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SDDIAG_SPI_HOST;
    host.max_freq_khz = SDDIAG_RUN_KHZ;

    err = sdspi_host_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        note_err(h, "sdspi_host_init", err);
        return err;
    }

    sdspi_device_config_t devcfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    devcfg.gpio_cs = SDDIAG_PIN_CS;
    devcfg.host_id = SDDIAG_SPI_HOST;

    sdspi_dev_handle_t dev;
    err = sdspi_host_init_device(&devcfg, &dev);
    if (err != ESP_OK) { note_err(h, "sdspi_host_init_device", err); return err; }
    host.slot = dev;

    err = sdmmc_card_init(&host, &h->card);
    if (err != ESP_OK) { note_err(h, "sdmmc_card_init", err); return err; }

    h->initialized = true;
    h->freq_khz = h->card.real_freq_khz ? h->card.real_freq_khz
                                        : (int)host.max_freq_khz;

    sd_hal_capture_raw(h);   // best effort; non-fatal
    return ESP_OK;
}

void sd_hal_deinit(sd_hal_t *h)
{
    if (h->backend && strcmp(h->backend, "sdspi") == 0) {
        sdspi_host_deinit();
        spi_bus_free(SDDIAG_SPI_HOST);
    }
    h->initialized = false;
}

// ---------------------------------------------------------------------------
// block I/O
// ---------------------------------------------------------------------------
esp_err_t sd_hal_read_blocks(sd_hal_t *h, uint32_t lba, void *buf, uint32_t count)
{
    if (!h->initialized) return ESP_ERR_INVALID_STATE;
    return sdmmc_read_sectors(&h->card, buf, lba, count);
}

esp_err_t sd_hal_write_blocks(sd_hal_t *h, uint32_t lba, const void *buf, uint32_t count)
{
    if (!h->initialized) return ESP_ERR_INVALID_STATE;
    return sdmmc_write_sectors(&h->card, buf, lba, count);
}

// ---------------------------------------------------------------------------
// raw register reads
// ---------------------------------------------------------------------------

// In SPI mode CMD9/CMD10 return the 16-byte register as a DATA READ. The bytes
// arrive on the wire MSB-first (bit 127 first), exactly like Linux sysfs hex.
// IDF's own helpers run sdmmc_flip_byte_order() only because its decode macros
// expect a uint32_t[4] in host order; our decoder reads MSB-first bytes
// directly, so we keep the wire bytes as-is (no flip).
static esp_err_t read_reg16_spi(sd_hal_t *h, uint8_t opcode, uint8_t out16[16])
{
    uint8_t *buf = heap_caps_malloc(16, MALLOC_CAP_DMA);
    if (!buf) return ESP_ERR_NO_MEM;
    sdmmc_command_t cmd = {
        .opcode = opcode,
        .arg = 0,
        .data = buf,
        .datalen = 16,
        .buflen = heap_caps_get_allocated_size(buf),
        .blklen = 16,
        .flags = SCF_CMD_READ | SCF_CMD_ADTC | SCF_RSP_R1,
    };
    esp_err_t e = sdmmc_send_cmd(&h->card, &cmd);
    if (e == ESP_OK) {
        memcpy(out16, buf, 16);   // already MSB-first on the wire
#ifdef SDDIAG_DEBUG_RAW
        printf("[dbg] cmd%u wire: ", opcode);
        for (int i = 0; i < 16; i++) printf("%02x", buf[i]);
        printf("\n");
#endif
    }
    heap_caps_free(buf);
    return e;
}

esp_err_t sd_hal_capture_raw(sd_hal_t *h)
{
    sd_raw_t *r = &h->raw;

    // CID (CMD10) and CSD (CMD9) as raw 16-byte blobs.
    if (read_reg16_spi(h, OP_SEND_CID, r->cid) == ESP_OK) r->has_cid = true;
    if (read_reg16_spi(h, OP_SEND_CSD, r->csd) == ESP_OK) r->has_csd = true;

    // OCR (32 bits) — already available from init, store MSB-first.
    uint32_t ocr = h->card.ocr;
    r->ocr[0] = (ocr >> 24) & 0xFF;
    r->ocr[1] = (ocr >> 16) & 0xFF;
    r->ocr[2] = (ocr >> 8)  & 0xFF;
    r->ocr[3] = (ocr >> 0)  & 0xFF;
    r->has_ocr = true;

    // SCR (ACMD51): 8-byte data block, MSB-first.
    {
        uint8_t *buf = heap_caps_malloc(8, MALLOC_CAP_DMA);
        if (buf) {
            sdmmc_command_t cmd = {
                .opcode = OP_SEND_SCR,
                .arg = 0,
                .data = buf,
                .datalen = 8,
                .buflen = heap_caps_get_allocated_size(buf),
                .blklen = 8,
                .flags = SCF_CMD_READ | SCF_CMD_ADTC | SCF_RSP_R1,
            };
            if (sdmmc_send_app_cmd(&h->card, &cmd) == ESP_OK) {
                memcpy(r->scr, buf, 8);
                r->has_scr = true;
            }
            heap_caps_free(buf);
        }
    }

    if (sd_hal_read_ssr(h, r->ssr) == ESP_OK)           r->has_ssr = true;
    if (sd_hal_switch_func_check(h, r->cmd6) == ESP_OK) r->has_cmd6 = true;
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// raw commands
// ---------------------------------------------------------------------------
esp_err_t sd_hal_card_status(sd_hal_t *h, uint32_t *status_out)
{
    if (!h->initialized) return ESP_ERR_INVALID_STATE;
    return sdmmc_send_cmd_send_status(&h->card, status_out);
}

esp_err_t sd_hal_switch_func_check(sd_hal_t *h, uint8_t status64[64])
{
    if (!h->initialized) return ESP_ERR_INVALID_STATE;
    uint8_t *buf = heap_caps_malloc(64, MALLOC_CAP_DMA);
    if (!buf) return ESP_ERR_NO_MEM;
    // CMD6 check mode: mode=0, groups=0xF (no change) -> arg 0x00FFFFFF.
    sdmmc_command_t cmd = {
        .opcode = OP_SWITCH_FUNC,
        .arg = 0x00FFFFFF,
        .data = buf,
        .datalen = 64,
        .buflen = heap_caps_get_allocated_size(buf),
        .blklen = 64,
        .flags = SCF_CMD_READ | SCF_CMD_ADTC | SCF_RSP_R1,
    };
    esp_err_t e = sdmmc_send_cmd(&h->card, &cmd);
    if (e == ESP_OK) memcpy(status64, buf, 64);
    heap_caps_free(buf);
    return e;
}

esp_err_t sd_hal_read_ssr(sd_hal_t *h, uint8_t ssr64[64])
{
    if (!h->initialized) return ESP_ERR_INVALID_STATE;
    uint8_t *buf = heap_caps_malloc(64, MALLOC_CAP_DMA);
    if (!buf) return ESP_ERR_NO_MEM;
    // ACMD13: sdmmc_send_app_cmd issues CMD55 first.
    sdmmc_command_t cmd = {
        .opcode = OP_SD_STATUS,
        .arg = 0,
        .data = buf,
        .datalen = 64,
        .buflen = heap_caps_get_allocated_size(buf),
        .blklen = 64,
        .flags = SCF_CMD_READ | SCF_CMD_ADTC | SCF_RSP_R1,
    };
    esp_err_t e = sdmmc_send_app_cmd(&h->card, &cmd);
    if (e == ESP_OK) memcpy(ssr64, buf, 64);
    heap_caps_free(buf);
    return e;
}
