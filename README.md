# ESP32 SD-Card Diagnosis Tool

## Goal

Firmware for an **ESP32 with an SD-card interface** that reports the **same information
as a Linux `rtsx_pci` native SD host (and more)**, plus active health/performance
diagnostics that no consumer USB card reader can provide.

The motivation: consumer USB card readers (Genesys, "Generic STORAGE DEVICE", etc.)
bridge USB↔SD over `usb-storage` and **hide the SD bus**, so the card's identity
registers (CID/CSD/SCR/SSR) and real speed are unreachable. Only a native SD host —
`rtsx_pci` on Linux, or **a microcontroller wired directly to the SD card**, as here —
can issue raw SD commands and read those registers. The ESP32 sits *on* the SD bus, so
it can do everything `rtsx_pci` does and then some.

## What "same as rtsx_pci, or more" means

`rtsx_pci` + Linux sysfs exposes (under `/sys/block/mmcblk0/device/`):
`cid, csd, scr, ssr, manfid, oemid, name, hwrev, fwrev, serial, date, type`.

This tool **matches all of those** and **adds**:

| Capability | rtsx_pci / sysfs | This tool |
| --- | --- | --- |
| CID decoded (maker, OEM, product, rev, serial, mfg date) | ✅ | ✅ |
| CSD decoded (capacity, classes, speeds, block sizes) | ✅ | ✅ |
| SCR decoded (SD spec version, bus widths) | ✅ | ✅ |
| SSR decoded (speed class, UHS grade, **video class**, AU size, App class A1/A2) | ✅ (raw) | ✅ **decoded** |
| OCR (CCS/SDHC/SDXC, S18A 1.8 V, UHS-II bit) | partial | ✅ |
| **CMD6 SWITCH_FUNC** — supported bus-speed modes (SDR12/25/50/104, DDR50), driver strength, current limit | ❌ | ✅ |
| **Live read benchmark** (real card speed, no USB cap) | ❌ | ✅ |
| **Live write benchmark + random IOPS** | ❌ | ✅ |
| **Fake / overstated capacity detection** (f3-style write+verify) | ❌ | ✅ |
| **Surface scan** (per-region read latency, CRC/retry counts) | ❌ | ✅ |
| **Card status (CMD13) error-flag monitoring** | ❌ | ✅ |
| Manufacturer-ID → human name lookup | ❌ | ✅ |
| Industrial-card health (CMD56 / vendor SMART) hook | ❌ | ✅ (best-effort, generic — see limitations below) |

## Interfaces

- **USB-serial query language** (primary): line-based commands over the CH340 console —
  `info`, `caps`, `status`, `scan [lba [n]]`, `bench`, `sniff`, `read <lba> [n]`, `json`,
  `reinit`, and the destructive `capcheck` / `wtest` (build- and runtime-gated). Long tests
  (`scan`, `wtest`) print an ETA and a live countdown, stop cleanly on any input, and a
  stopped scan can resume from the reported LBA.
- **Standalone touchscreen UI** (optional, `SDDIAG_UI` in `config.h`): an LVGL 9 UI on the
  CYD's ST7789 display with GT911 capacitive touch, exposing Identify / Sniff test (and, in
  destructive builds, Capacity check / Scan write) without a host PC. It shares the SD bus
  with the serial console through a mutex; destructive tests keep the same confirmation
  gating as the serial commands.

## Deliverables in this directory

- `README.md` — this file (orientation, scope).
- `SD_REFERENCE.md` — exact register/command bit layouts, decode tables, and the
  manufacturer-ID database. Follow this for any decode logic.
- `CLAUDE.md` — current build/flash instructions and firmware architecture (kept in sync
  with the code; the reference to use when working on this repo).

## Hardware

- Board: CYD ESP32-2432S032C (classic ESP32), CH340 USB-serial. Its micro-SD slot is wired
  to a **dedicated SPI bus** (CS=GPIO5, MOSI=GPIO23, MISO=GPIO19, SCK=GPIO18), separate from
  the TFT bus — this was not documented anywhere for this board and had to be determined
  empirically.
- The SD spec allows two electrical modes:
  - **SD/SDMMC host** (1- or 4-bit): available on classic ESP32 and ESP32-S3, faster. The
    backend abstraction in `sd_hal.c` supports this path (`SDDIAG_BACKEND_SDMMC` in
    `config.h`), but it is **untested** — this project has only ever run against the CYD,
    which wires the slot for SPI only.
  - **SPI mode** (used here): available on all ESP32 variants, 1-bit, simplest wiring. All
    identity registers and diagnostics still work; only the speed ceiling differs.
- Card is 3.3 V; the CYD's slot provides power and pull-ups.

## Recommended framework

**ESP-IDF** (not Arduino) — its `sdmmc`/`sdspi` host drivers already parse CID/CSD/SCR/OCR
into `sdmmc_card_t`, and `sdmmc_send_cmd()` gives raw command access needed for SSR
(ACMD13), CMD6, and CMD56. Arduino-esp32 wraps the same driver but exposes less raw access.

## Safety model

- **Default = non-destructive**: register dump + decode, read-only surface scan,
  read benchmark. Never writes to the card.
- **Destructive tests** (capacity verification, write benchmark) are **gated** behind an
  explicit build flag *and* a runtime confirmation, and print a clear data-loss warning.

## Possible future work

Ideas considered during design but not implemented:

- **Per-vendor CMD56 health decode**: CMD56 (GEN_CMD) is currently probed generically —
  a successful read is shown as raw hex, a rejection as "not supported". The SD spec
  leaves the payload entirely vendor-defined, so a real SMART-style report (spare-block %,
  erase counts, power cycles) would need per-manufacturer protocols (ATP, Swissbit, Apacer,
  SanDisk/WD industrial…), typically under NDA.
- **Report sinks beyond serial/JSON**: writing the report to a file on the card itself
  (non-destructive runs only), or serving it over a small WiFi HTTP endpoint.
- **SDMMC backend validation**: exercising the untested 4-bit SDMMC path (see Hardware
  above) on a board that actually wires it, e.g. ESP32-S3.
