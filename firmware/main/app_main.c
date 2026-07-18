// app_main.c — orchestration + serial query language.
//
// Per project requirement: NO display facility. Everything is reported over the
// USB-serial console, and the tool is driven by a small line-based query
// language. On boot it brings up the card and auto-runs `info`. If bring-up
// fails (the card here is a known-failed card), it reports exactly why and the
// query language stays available so individual probes can still be retried.

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_vfs_dev.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"

#include "config.h"
#include "sd_hal.h"
#include "sd_decode.h"
#include "diag_read.h"
#include "diag_write.h"
#include "report.h"

static sd_hal_t   s_hal;
static sd_decoded_t s_dec;
static bool       s_card_ok = false;

static void scan_progress(int pct)
{
    static int last = -1;
    if (pct / 10 != last / 10) { printf("  ...%d%%\n", pct); last = pct; }
}

// ---- command handlers -----------------------------------------------------

static void cmd_help(void)
{
    printf(
      "\nSD-Card Diagnosis — serial query language\n"
      "  info     register dump + decode (Identity)\n"
      "  caps     SCR/OCR/SSR/CMD6 capability summary\n"
      "  status   CMD13 card status + decoded error flags\n"
      "  scan     read-only surface scan (whole card, per-region speed/errors)\n"
      "  bench    read-only sequential + random read benchmark\n"
      "  json     full machine-readable JSON document\n"
      "  reinit   re-attempt card bring-up\n"
#if CONFIG_SDDIAG_ALLOW_DESTRUCTIVE
      "  wtest    DESTRUCTIVE full write/verify (overwrites card; needs `wtest DESTROY`)\n"
#endif
      "  help     this message\n"
      "\nDestructive write tests are %s in this build.\n",
#if CONFIG_SDDIAG_ALLOW_DESTRUCTIVE
      "ENABLED (require runtime confirm)"
#else
      "DISABLED (safe default build)"
#endif
    );
}

static void refresh_decode(void)
{
    if (s_card_ok) sd_decode_all(&s_hal.raw, &s_dec);
}

static void cmd_info(void)
{
    if (!s_card_ok) { report_init_failure_human(&s_hal); return; }
    report_identity_human(&s_hal, &s_dec);
}

static void cmd_caps(void)
{
    if (!s_card_ok) { printf("No card. Run `reinit`.\n"); return; }
    report_caps_human(&s_dec);
}

static void cmd_status(void)
{
    if (!s_card_ok) { printf("No card. Run `reinit`.\n"); return; }
    uint32_t st = 0;
    esp_err_t e = sd_hal_card_status(&s_hal, &st);
    if (e != ESP_OK) { printf("CMD13 failed: %s\n", esp_err_to_name(e)); return; }
    char names[256];
    int n = diag_cmd13_errors(st, names, sizeof(names));
    printf("\n=== CARD STATUS (CMD13) ===\n");
    printf("  R1 status         : 0x%08lx\n", (unsigned long)st);
    printf("  Error flags set   : %d  %s\n", n, n ? names : "(none)");
}

static void cmd_scan(void)
{
    if (!s_card_ok) { printf("No card. Run `reinit`.\n"); return; }
    printf("\nStarting surface scan (read-only)...\n");
    scan_result_t res;
    esp_err_t e = diag_surface_scan(&s_hal, &res, scan_progress);
    if (e != ESP_OK) { printf("scan error: %s\n", esp_err_to_name(e)); return; }
    report_scan_human(&res);
}

static void cmd_bench(void)
{
    if (!s_card_ok) { printf("No card. Run `reinit`.\n"); return; }
    bench_result_t res;
    esp_err_t e = diag_read_bench(&s_hal, &res);
    if (e != ESP_OK) { printf("bench error: %s\n", esp_err_to_name(e)); return; }
    report_bench_human(&res);
}

static void cmd_json(void)
{
    if (!s_card_ok) {
        printf("{\"error\": \"card bring-up failed\", \"stage\": \"%s\", "
               "\"code\": \"%s\"}\n",
               s_hal.last_err_stage, esp_err_to_name(s_hal.last_err));
        return;
    }
    report_json(&s_hal, &s_dec);
}

#if CONFIG_SDDIAG_ALLOW_DESTRUCTIVE
static void write_progress(int pct)
{
    static int last = -1;
    if (pct / 10 != last / 10) { printf("  ...%d%%\n", pct); last = pct; }
}

static void cmd_wtest(const char *arg)
{
    if (!s_card_ok) { printf("No card. Run `reinit`.\n"); return; }
    while (*arg == ' ') arg++;
    if (strcmp(arg, "DESTROY") != 0) {
        printf("\n*** DESTRUCTIVE write/verify test ***\n");
        printf("This OVERWRITES THE ENTIRE CARD. ALL DATA WILL BE LOST.\n");
        printf("It writes a per-sector pattern over every sector, then reads the\n");
        printf("whole card back to verify retention and detect fake capacity.\n");
        printf("To proceed, run:  wtest DESTROY\n");
        return;
    }
    printf("\n=== DESTRUCTIVE WRITE/VERIFY (h2testw-style) ===\n");
    printf("Overwriting and verifying the whole card. This takes many minutes.\n");
    write_result_t res;
    esp_err_t e = diag_write_verify(&s_hal, &res, write_progress);
    if (e != ESP_OK) { printf("wtest error: %s\n", esp_err_to_name(e)); return; }
    report_write_human(&res);
}
#endif

static void cmd_reinit(void)
{
    printf("Re-initialising card...\n");
    if (s_hal.initialized || s_hal.backend) sd_hal_deinit(&s_hal);
    esp_err_t e = sd_hal_init(&s_hal);
    s_card_ok = (e == ESP_OK);
    if (s_card_ok) { refresh_decode(); printf("Card ready.\n"); }
    else           { report_init_failure_human(&s_hal); }
}

// ---- dispatch -------------------------------------------------------------

static void dispatch(char *line)
{
    // trim
    while (*line && isspace((unsigned char)*line)) line++;
    char *end = line + strlen(line);
    while (end > line && isspace((unsigned char)end[-1])) *--end = '\0';
    if (!*line) return;

    if      (!strcmp(line, "help"))   cmd_help();
    else if (!strcmp(line, "info"))   cmd_info();
    else if (!strcmp(line, "caps"))   cmd_caps();
    else if (!strcmp(line, "status")) cmd_status();
    else if (!strcmp(line, "scan"))   cmd_scan();
    else if (!strcmp(line, "bench"))  cmd_bench();
    else if (!strcmp(line, "json"))   cmd_json();
    else if (!strcmp(line, "reinit")) cmd_reinit();
#if CONFIG_SDDIAG_ALLOW_DESTRUCTIVE
    else if (!strncmp(line, "wtest", 5)) cmd_wtest(line + 5);
#endif
    else printf("Unknown command '%s'. Type `help`.\n", line);

    printf("\nsd> ");
    fflush(stdout);
}

// ---- console setup --------------------------------------------------------

static void console_init(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    // Configure UART0 driver so we can read lines reliably from the CH340.
    const uart_config_t cfg = {
        .baud_rate = CONFIG_ESP_CONSOLE_UART_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);
    uart_param_config(UART_NUM_0, &cfg);
    uart_vfs_dev_use_driver(UART_NUM_0);
    uart_vfs_dev_port_set_rx_line_endings(UART_NUM_0, ESP_LINE_ENDINGS_CR);
    uart_vfs_dev_port_set_tx_line_endings(UART_NUM_0, ESP_LINE_ENDINGS_CRLF);
}

void app_main(void)
{
    console_init();

    printf("\n\n================================================\n");
    printf(" ESP32 SD-Card Diagnosis Tool  (serial, no display)\n");
    printf(" Board: CYD ESP32-2432S032C   Backend: SPI\n");
    printf("================================================\n");

    printf("Bringing up SD card (CMD0/CMD8/ACMD41)... "
           "this can take a few seconds on a marginal card.\n");
    fflush(stdout);
    esp_err_t e = sd_hal_init(&s_hal);
    s_card_ok = (e == ESP_OK);
    if (s_card_ok) {
        refresh_decode();
        printf("Card initialised OK. Auto-running `info`:\n");
        cmd_info();
        cmd_caps();
    } else {
        printf("Card bring-up FAILED — reporting diagnostics:\n");
        report_init_failure_human(&s_hal);
        printf("\nThe query language is still available; try `reinit`, `info`, or `status`.\n");
    }

    cmd_help();
    printf("\nsd> ");
    fflush(stdout);

    // Line reader loop.
    char line[128];
    int len = 0;
    while (1) {
        int ch = fgetc(stdin);
        if (ch == EOF) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        if (ch == '\r' || ch == '\n') {
            putchar('\n');
            line[len] = '\0';
            dispatch(line);
            len = 0;
        } else if (ch == 0x7f || ch == 0x08) {   // backspace
            if (len > 0) { len--; printf("\b \b"); fflush(stdout); }
        } else if (len < (int)sizeof(line) - 1) {
            line[len++] = (char)ch;
            putchar(ch);            // echo
            fflush(stdout);
        }
    }
}
