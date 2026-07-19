# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

ESP-IDF firmware that turns a CYD ESP32-2432S032C board into an SD-card diagnosis tool: it sits directly on the SD bus (SPI mode), dumps and decodes every identity register (CID/CSD/SCR/SSR/OCR/CMD6), and runs read benchmarks, surface scans, and (opt-in) destructive write/verify fake-capacity tests. Everything is reported over the USB-serial console via a line-based query language (`info`, `caps`, `status`, `scan [lba [n]]`, `bench`, `sniff`, `read <lba> [n]`, `json`, `reinit`, `capcheck`, `wtest`; long tests stop cleanly on any input and can resume via a range scan). A standalone LVGL touchscreen UI (`ui.c`, gated by `SDDIAG_UI` in `config.h`) additionally exposes the basic non-destructive functions (Identify, Sniff test) on the board's ST7789 display with GT911 capacitive touch.

Reference docs at the repo root:
- `README.md` — project scope, feature comparison vs Linux `rtsx_pci`, interfaces, hardware, safety model, and possible future work.
- `SD_REFERENCE.md` — bit-level register layouts, decode tables, manufacturer-ID database. Follow this when writing/altering any decode logic.

## Building and flashing

**Critical gotcha:** PlatformIO's ESP-IDF tooling refuses to run under MSYS/Git Bash ("MSys/Mingw is not supported"). Build with `MSYSTEM` unset and Git MSYS dirs off PATH. Two wrappers handle this:

```
python tools/flash.py           # build+flash the full (destructive) env; works from any shell
python tools/flash.py --safe    # safe env; --build-only / --boot also available
pwsh firmware/bp.ps1            # build only (env cyd_esp32)
pwsh firmware/bp.ps1 upload     # build + flash to COM7
```

The board is shared with other projects — `tools/flash.py` is the one-command way to put this firmware back on it.

Direct PlatformIO (from a clean PowerShell, pio at `~/.platformio/penv/Scripts/pio.exe`):

```
pio run -d firmware -e cyd_esp32
pio run -d firmware -e cyd_esp32_destructive -t upload   # build with wtest enabled
```

This is a native ESP-IDF v5.x project (sources in `firmware/main/`, not `src/`; `src_dir = main` in `platformio.ini`). No tests, no linter — verification is done by flashing and driving the board over serial.

Serial console: **COM7, 115200 baud** (CH340 USB-serial).

## Driving the board (host-side tools, Python + pyserial)

```
python tools/sdmon.py boot            # reset board via DTR/RTS, capture boot output
python tools/sdmon.py cmd "info" "caps"   # send commands, capture replies
python tools/sniff.py                 # safe identity probes only: reinit/info/caps/status
python tools/fullrun.py               # waits for USB power-cycle, then full read-only suite
```

Only one process can hold COM7 — close any monitor before running these.

## Architecture

All firmware code is in `firmware/main/`, one flat ESP-IDF component:

- `app_main.c` — console setup (UART0 driver + VFS), serial query-language dispatch, boot sequence (init card, auto-run `info`). Bring-up failure is a first-class path: the card under test may be dead, so `sd_hal_t` carries `last_err`/`last_err_stage` and the query loop stays alive for retries.
- `sd_hal.[ch]` — abstraction over ESP-IDF `sdspi`/`sdmmc` hosts. Owns card bring-up, block I/O, and the raw commands IDF doesn't surface: ACMD13 (SSR), CMD6 check-mode (SWITCH_FUNC), CMD13 (status). Captures **raw** register blobs into `sd_raw_t` alongside the IDF-parsed `sdmmc_card_t`. Backend is selected in `config.h`; on the CYD only SPI is wired, so `SDDIAG_BACKEND_SPI=1` is the working default.
- `sd_registers.h` + `sd_decode.[ch]` — re-parse the raw register bytes into every documented field (superset of what IDF decodes). Bit layouts come from `SD_REFERENCE.md`.
- `sd_mfg_db.[ch]` — manufacturer-ID/OEM-ID → name lookup (spotting relabeled fakes).
- `diag_read.[ch]` — read-only diagnostics: surface scan, sequential/random read benchmark, CMD13 error-flag decode.
- `diag_write.[ch]` — destructive: full-card write/verify (h2testw-style fake-capacity detection). Entirely compiled out unless `CONFIG_SDDIAG_ALLOW_DESTRUCTIVE=1`.
- `report.[ch]` — all human-readable and JSON output formatting.
- `ui.[ch]` — optional LVGL 9 touchscreen UI (Identify / Sniff test buttons), built on managed components (`idf_component.yml`: lvgl, esp_lvgl_port, esp_lcd_touch_gt911) with esp_lcd's native ST7789 driver. UI and serial console share the SD bus through a mutex owned by `app_main.c`. Display/touch pins and orientation flags live in `config.h`.

Hardware pin map (in `config.h`): SD slot on its own SPI bus — CS=GPIO5, MOSI=GPIO23, MISO=GPIO19, SCK=GPIO18, `SPI2_HOST`. Probe clock starts at 400 kHz for marginal cards, 20 MHz nominal.

## Safety model (must be preserved)

- The default build (`cyd_esp32`) never issues a write command to the card. Destructive paths require **both** the `cyd_esp32_destructive` build (`-DCONFIG_SDDIAG_ALLOW_DESTRUCTIVE=1`) **and** the runtime confirmation `wtest DESTROY`.
- CMD6 must only ever be used in *check* mode (mode bit 0) for capability discovery.
- Whole-card operations stream with bounded buffers — never allocate card-sized memory.
