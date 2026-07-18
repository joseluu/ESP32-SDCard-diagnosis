// ui.c — standalone LVGL touchscreen UI (ST7789 + GT911).
//
// On-device access to the basic non-destructive functions — identification and
// a sniff test (reinit + identity + caps + CMD13 status, mirroring
// tools/sniff.py) — so the tool works without a host PC. The serial query
// language stays fully functional; SD access is serialised with the console
// through ctx->sd_mutex.
//
// Display/touch wiring per the Arduino demo repo (see config.h). This uses the
// native ESP-IDF stack instead of the demo's Arduino libraries: esp_lcd's
// built-in ST7789 driver (replaces Arduino_GFX), the esp_lcd_touch_gt911
// managed component (replaces TAMC_GT911), and LVGL 9 glued by esp_lvgl_port.

#include "config.h"

#if SDDIAG_UI

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lvgl_port.h"

#include "ui.h"
#include "sd_mfg_db.h"
#include "diag_read.h"

static const char *TAG = "ui";

typedef enum { UI_CMD_IDENTIFY, UI_CMD_SNIFF } ui_cmd_t;

static ui_ctx_t      s_ctx;
static lv_display_t *s_disp;
static lv_obj_t     *s_status;   // header card-state label
static lv_obj_t     *s_out;      // result text label (inside a scrollable box)
static QueueHandle_t s_cmdq;

// ---- result text buffer ---------------------------------------------------

static char s_buf[2048];
static int  s_off;

static void outf(const char *fmt, ...)
{
    if (s_off >= (int)sizeof(s_buf) - 1) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(s_buf + s_off, sizeof(s_buf) - s_off, fmt, ap);
    va_end(ap);
    if (n > 0) s_off += n;
    if (s_off > (int)sizeof(s_buf) - 1) s_off = sizeof(s_buf) - 1;
}

// ---- SD operations (called with sd_mutex held) ----------------------------

static void do_reinit(void)
{
    if (s_ctx.hal->initialized || s_ctx.hal->backend) sd_hal_deinit(s_ctx.hal);
    esp_err_t e = sd_hal_init(s_ctx.hal);
    *s_ctx.card_ok = (e == ESP_OK);
    if (*s_ctx.card_ok) sd_decode_all(&s_ctx.hal->raw, s_ctx.dec);
}

static void fmt_init_failure(void)
{
    outf("CARD BRING-UP FAILED\n\n");
    outf("stage : %s\n", s_ctx.hal->last_err_stage);
    outf("error : %s\n", esp_err_to_name(s_ctx.hal->last_err));
    outf("\nCheck the card is seated, or try\nanother card. Retry with a button.\n");
}

static void fmt_identity(void)
{
    const sd_cid_t *cid = &s_ctx.dec->cid;
    const sd_csd_t *csd = &s_ctx.dec->csd;
    const char *mfg = sd_mfg_name(cid->mid);

    outf("-- Identity --\n");
    outf("Product : %s  rev %u.%u\n", cid->pnm, cid->prv_major, cid->prv_minor);
    outf("Maker   : 0x%02X %s (%s)\n", cid->mid,
         mfg ? mfg : "unknown", sd_mfg_confidence(cid->mid));
    outf("OEM id  : \"%s\"%s\n", cid->oid,
         sd_mfg_oid_consistent(cid->mid, (const char *)cid->oid)
             ? "" : "  << MID/OID MISMATCH");
    outf("Serial  : 0x%08lX\n", (unsigned long)cid->psn);
    outf("Date    : %04u-%02u\n", cid->mdt_year, cid->mdt_month);
    outf("Capacity: %.2f GB (%llu sect)\n",
         csd->capacity_bytes / 1e9, (unsigned long long)csd->capacity_sectors);
    outf("Bus     : %s %d-bit @ %d kHz\n", s_ctx.hal->backend,
         s_ctx.hal->bus_width, s_ctx.hal->freq_khz);
}

static void fmt_caps(void)
{
    const sd_scr_t  *scr  = &s_ctx.dec->scr;
    const sd_ssr_t  *ssr  = &s_ctx.dec->ssr;
    const sd_ocr_t  *ocr  = &s_ctx.dec->ocr;
    const sd_cmd6_t *cmd6 = &s_ctx.dec->cmd6;
    const sd_raw_t  *raw  = &s_ctx.hal->raw;

    outf("\n-- Capabilities --\n");
    if (raw->has_scr) outf("Spec    : SD %s\n", scr->phys_version);
    if (raw->has_ocr)
        outf("Type    : %s%s\n", ocr->ccs ? "SDHC/SDXC" : "SDSC",
             ocr->s18a ? ", 1.8V capable" : "");
    if (raw->has_ssr) {
        static const uint8_t cls[] = { 0, 2, 4, 6, 10 };
        outf("Classes : C%u  U%u  V%u  A%u\n",
             ssr->speed_class <= 4 ? cls[ssr->speed_class] : ssr->speed_class,
             ssr->uhs_speed_grade, ssr->video_speed_class, ssr->app_perf_class);
        outf("AU size : %lu KiB\n", (unsigned long)(ssr->au_size_bytes / 1024));
    }
    if (raw->has_cmd6)
        outf("Modes   :%s%s%s%s%s\n",
             cmd6->sdr12 ? " SDR12" : "", cmd6->sdr25 ? " SDR25" : "",
             cmd6->sdr50 ? " SDR50" : "", cmd6->sdr104 ? " SDR104" : "",
             cmd6->ddr50 ? " DDR50" : "");
}

// Re-check that a supposedly-present card is still there (mutex held). A data
// read is the only reliable probe: its 0xFE start token can't be faked by a
// floating MISO line.
static uint8_t s_probe_buf[512];

static void card_recheck(void)
{
    if (!*s_ctx.card_ok) return;
    if (sd_hal_read_blocks(s_ctx.hal, 0, s_probe_buf, 1) == ESP_OK) return;
    sd_hal_deinit(s_ctx.hal);
    *s_ctx.card_ok = false;
}

static void run_identify(void)
{
    card_recheck();                 // notices a card pulled since last time
    if (!*s_ctx.card_ok) do_reinit();
    if (!*s_ctx.card_ok) { fmt_init_failure(); return; }
    fmt_identity();
    fmt_caps();
}

// Sniff = health check, deliberately NOT repeating Identify's output. A
// failed card can serve its identity registers fine, so this exercises what
// actually breaks: repeated timed bring-up cycles (intermittent power-up is
// the classic failed-card signature), spot reads, a sustained burst read,
// CMD13, and a verdict.
static void run_sniff(void)
{
    outf("Sniff test\n\n");
    probe_result_t p;
    esp_err_t e = diag_quick_probe(s_ctx.hal, &p);
    *s_ctx.card_ok = s_ctx.hal->initialized;
    if (*s_ctx.card_ok) sd_decode_all(&s_ctx.hal->raw, s_ctx.dec);
    if (e != ESP_OK) { outf("Probe could not run.\n"); return; }

    outf("-- Bring-up x%d --\n", DIAG_PROBE_INIT_CYCLES);
    for (int i = 0; i < DIAG_PROBE_INIT_CYCLES; i++) {
        if (p.init_err[i] == ESP_OK)
            outf("#%d : OK    %lu ms\n", i + 1, (unsigned long)p.init_ms[i]);
        else
            outf("#%d : FAIL  %s\n", i + 1, esp_err_to_name(p.init_err[i]));
    }
    if (!p.card_usable) {
        outf("\nVERDICT: CARD IS BAD\n(cannot complete bring-up)\n\n");
        fmt_init_failure();
        return;
    }

    static const char *kPos[DIAG_PROBE_POINTS] =
        { "start", "25%", "50%", "75%", "end" };
    outf("\n-- Spot reads --\n");
    for (int i = 0; i < p.points; i++) {
        if (p.err[i] == ESP_OK)
            outf("%-6s: OK    %lu ms\n", kPos[i], (unsigned long)p.ms[i]);
        else
            outf("%-6s: FAIL  %s\n", kPos[i], esp_err_to_name(p.err[i]));
    }
    if (p.burst_ok)
        outf("Burst : OK    %.2f MB/s\n        (%lu KiB)\n",
             p.burst_mbps, (unsigned long)p.burst_kb);
    else
        outf("Burst : FAIL  at LBA %lu\n        (%lu KiB read)\n",
             (unsigned long)p.burst_fail_lba, (unsigned long)p.burst_kb);
    if (p.status_ok)
        outf("CMD13 : %d error flag(s)\n", p.status_errs);
    else
        outf("CMD13 : no answer\n");

    bool bad = p.init_fails || p.fails || !p.burst_ok ||
               p.status_errs || !p.status_ok;
    if (!bad)
        outf("\nVERDICT: card passes\n(bring-up stable, reads OK)\n");
    else
        outf("\nVERDICT: CARD IS BAD\n(%s%s%s)\n",
             p.init_fails ? "intermittent bring-up " : "",
             (p.fails || !p.burst_ok) ? "read failures " : "",
             (p.status_errs || !p.status_ok) ? "status errors" : "");
}

// ---- UI -------------------------------------------------------------------

static void update_status_label(void)
{
    if (*s_ctx.card_ok) {
        lv_label_set_text(s_status, "CARD IN");
        lv_obj_set_style_text_color(s_status, lv_palette_main(LV_PALETTE_GREEN), 0);
    } else {
        lv_label_set_text(s_status, "NO CARD");
        lv_obj_set_style_text_color(s_status, lv_palette_main(LV_PALETTE_RED), 0);
    }
}

// Insertion poll, run when the worker is idle and no card is known. The CYD
// slot has no card-detect line, so insertion is detected by a quiet re-init
// attempt. Removal is NOT polled (a pulled card is only noticed on the next
// Identify/Sniff press): with the slot empty MISO floats low, so commands
// falsely "succeed", and probing with real data reads every couple of seconds
// costs more than it is worth.
static void detect_poll(void)
{
    if (*s_ctx.card_ok) return;
    if (xSemaphoreTake(s_ctx.sd_mutex, 0) != pdTRUE) return;   // bus busy: skip
    // The IDF sdmmc stack logs errors loudly when the slot is empty; silence
    // logging for this background attempt.
    esp_log_level_t prev = esp_log_level_get("*");
    esp_log_level_set("*", ESP_LOG_NONE);
    if (s_ctx.hal->initialized || s_ctx.hal->backend) sd_hal_deinit(s_ctx.hal);
    if (sd_hal_init(s_ctx.hal) == ESP_OK) {
        sd_decode_all(&s_ctx.hal->raw, s_ctx.dec);
        *s_ctx.card_ok = true;
    }
    esp_log_level_set("*", prev);
    bool now = *s_ctx.card_ok;
    xSemaphoreGive(s_ctx.sd_mutex);
    if (!now) return;

    if (lvgl_port_lock(0)) {
        update_status_label();
        lv_label_set_text(s_out,
            "Card inserted.\nPress Identify or Sniff test.");
        lvgl_port_unlock();
    }
}

static void ui_worker(void *arg)
{
    (void)arg;
    ui_cmd_t cmd;
    for (;;) {
        if (xQueueReceive(s_cmdq, &cmd, pdMS_TO_TICKS(2000)) != pdTRUE) {
            detect_poll();     // idle: watch for card insertion/removal
            continue;
        }
        xSemaphoreTake(s_ctx.sd_mutex, portMAX_DELAY);
        s_off = 0;
        s_buf[0] = '\0';
        switch (cmd) {
        case UI_CMD_IDENTIFY: run_identify(); break;
        case UI_CMD_SNIFF:    run_sniff();    break;
        }
        xSemaphoreGive(s_ctx.sd_mutex);
        if (lvgl_port_lock(0)) {
            lv_label_set_text(s_out, s_buf);
            lv_obj_scroll_to_y(lv_obj_get_parent(s_out), 0, LV_ANIM_OFF);
            update_status_label();
            lvgl_port_unlock();
        }
    }
}

static void btn_event_cb(lv_event_t *e)
{
    // Runs in the LVGL task: give instant feedback, then queue the real work.
    ui_cmd_t cmd = (ui_cmd_t)(uintptr_t)lv_event_get_user_data(e);
    lv_label_set_text(s_out, "Working...");
    xQueueSend(s_cmdq, &cmd, 0);
}

static void mk_btn(lv_obj_t *parent, const char *txt, ui_cmd_t cmd)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_height(b, LV_PCT(100));
    lv_obj_add_event_cb(b, btn_event_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)cmd);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
}

static void build_ui(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, 4, 0);
    lv_obj_set_style_pad_row(scr, 4, 0);
    // The screen itself must not scroll, or drags bounce the whole layout
    // around instead of scrolling the result box.
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Header: title + card state.
    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_set_size(hdr, LV_PCT(100), 30);
    lv_obj_set_style_pad_all(hdr, 4, 0);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title = lv_label_create(hdr);
    lv_label_set_text(title, "SD-Card Diagnosis");
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);
    s_status = lv_label_create(hdr);
    lv_obj_align(s_status, LV_ALIGN_RIGHT_MID, 0, 0);
    update_status_label();

    // Button row.
    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_set_size(row, LV_PCT(100), 44);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 4, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    mk_btn(row, "Identify", UI_CMD_IDENTIFY);
    mk_btn(row, "Sniff test", UI_CMD_SNIFF);

    // Scrollable result area: the whole box is drag-sensitive (the label is
    // not clickable, so presses anywhere fall through to the box and dragging
    // tracks the finger).
    lv_obj_t *box = lv_obj_create(scr);
    lv_obj_set_width(box, LV_PCT(100));
    lv_obj_set_flex_grow(box, 1);
    lv_obj_set_style_pad_all(box, 6, 0);
    lv_obj_set_scroll_dir(box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLL_ELASTIC);
    s_out = lv_label_create(box);
    lv_obj_set_width(s_out, LV_PCT(100));
#if LV_FONT_MONTSERRAT_12
    lv_obj_set_style_text_font(s_out, &lv_font_montserrat_12, 0);
#endif
    lv_label_set_text(s_out,
        "Identify : registers + decode\n"
        "Sniff    : health check —\n"
        "           fresh bring-up, spot\n"
        "           reads, verdict\n\n"
        "Destructive tests are serial-only.");
}

// ---- hardware bring-up ----------------------------------------------------

static esp_err_t lcd_init(esp_lcd_panel_io_handle_t *io_out,
                          esp_lcd_panel_handle_t *panel_out)
{
    const spi_bus_config_t bus = {
        .sclk_io_num = SDDIAG_PIN_TFT_SCK,
        .mosi_io_num = SDDIAG_PIN_TFT_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SDDIAG_TFT_HRES * 40 * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SDDIAG_TFT_SPI_HOST, &bus,
                                           SPI_DMA_CH_AUTO), TAG, "spi bus");

    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = SDDIAG_PIN_TFT_DC,
        .cs_gpio_num = SDDIAG_PIN_TFT_CS,
        .pclk_hz = SDDIAG_TFT_CLK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)SDDIAG_TFT_SPI_HOST, &io_cfg, io_out),
        TAG, "panel io");

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = SDDIAG_PIN_TFT_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(*io_out, &panel_cfg, panel_out),
                        TAG, "st7789");
    esp_lcd_panel_reset(*panel_out);
    esp_lcd_panel_init(*panel_out);
    esp_lcd_panel_invert_color(*panel_out, true);   // IPS panel
    esp_lcd_panel_disp_on_off(*panel_out, true);

    // Backlight on.
    gpio_config_t bl = {
        .pin_bit_mask = 1ULL << SDDIAG_PIN_TFT_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&bl);
    gpio_set_level(SDDIAG_PIN_TFT_BL, 1);
    return ESP_OK;
}

static esp_err_t touch_init(esp_lcd_touch_handle_t *tp_out)
{
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = SDDIAG_PIN_TOUCH_SDA,
        .scl_io_num = SDDIAG_PIN_TOUCH_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t i2c;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &i2c), TAG, "i2c bus");

    esp_lcd_panel_io_i2c_config_t tio_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    tio_cfg.scl_speed_hz = 400000;
    esp_lcd_panel_io_handle_t tio;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(i2c, &tio_cfg, &tio),
                        TAG, "touch io");

    const esp_lcd_touch_config_t tp_cfg = {
        // Raw GT911 frame (mirrors are applied on the raw axes, pre-swap).
        .x_max = SDDIAG_TOUCH_RAW_X,
        .y_max = SDDIAG_TOUCH_RAW_Y,
        .rst_gpio_num = SDDIAG_PIN_TOUCH_RST,
        .int_gpio_num = SDDIAG_PIN_TOUCH_INT,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags = {
            .swap_xy  = SDDIAG_TOUCH_SWAP_XY,
            .mirror_x = SDDIAG_TOUCH_MIRROR_X,
            .mirror_y = SDDIAG_TOUCH_MIRROR_Y,
        },
    };
    return esp_lcd_touch_new_i2c_gt911(tio, &tp_cfg, tp_out);
}

esp_err_t ui_init(const ui_ctx_t *ctx)
{
    s_ctx = *ctx;

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_handle_t panel = NULL;
    esp_err_t e = lcd_init(&io, &panel);
    if (e != ESP_OK) return e;

    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl port");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = SDDIAG_TFT_HRES * 40,
        .double_buffer = false,
        .hres = SDDIAG_TFT_HRES,
        .vres = SDDIAG_TFT_VRES,
        .monochrome = false,
        .rotation = {
            .swap_xy  = SDDIAG_TFT_SWAP_XY,
            .mirror_x = SDDIAG_TFT_MIRROR_X,
            .mirror_y = SDDIAG_TFT_MIRROR_Y,
        },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = { .buff_dma = true, .swap_bytes = true },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    if (!s_disp) return ESP_FAIL;

    // Touch is best-effort: without it the display still shows boot status.
    esp_lcd_touch_handle_t tp = NULL;
    if (touch_init(&tp) == ESP_OK) {
        const lvgl_port_touch_cfg_t touch_cfg = { .disp = s_disp, .handle = tp };
        lvgl_port_add_touch(&touch_cfg);
    } else {
        ESP_LOGW(TAG, "GT911 touch init failed — display-only UI");
    }

    s_cmdq = xQueueCreate(4, sizeof(ui_cmd_t));
    if (!s_cmdq) return ESP_ERR_NO_MEM;

    if (lvgl_port_lock(0)) {
        build_ui();
        lvgl_port_unlock();
    }

    if (xTaskCreate(ui_worker, "ui_worker", 6144, NULL, 4, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "UI ready (LVGL %d.%d)", lv_version_major(), lv_version_minor());
    return ESP_OK;
}

#endif // SDDIAG_UI
