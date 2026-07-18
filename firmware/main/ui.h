// ui.h — optional LVGL touchscreen UI (identification + sniff test).
//
// The UI gives standalone access to the basic, non-destructive functions so
// the tool can be used without a host PC. All SD access from the UI is
// serialised with the serial console through `sd_mutex`.

#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "sd_hal.h"
#include "sd_decode.h"

typedef struct {
    sd_hal_t         *hal;
    sd_decoded_t     *dec;
    bool             *card_ok;
    SemaphoreHandle_t sd_mutex;   // shared with the serial console dispatcher
} ui_ctx_t;

// Bring up the display (ST7789), touch (GT911) and LVGL, build the screen and
// start the UI worker task. Returns ESP_OK, or an error if the display could
// not be initialised (the serial console keeps working either way).
esp_err_t ui_init(const ui_ctx_t *ctx);
