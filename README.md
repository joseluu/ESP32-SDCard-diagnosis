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
| Industrial-card health (CMD56 / vendor SMART) hook | ❌ | ⭐ stretch |

## Deliverables in this directory

- `README.md` — this file (orientation, scope).
- `IMPLEMENTATION_PLAN.md` — architecture, proposed file tree, module-by-module spec,
  milestones, and acceptance criteria. **Start here for building.**
- `SD_REFERENCE.md` — exact register/command bit layouts, decode tables, and the
  manufacturer-ID database. The implementer should follow this for parsing.

## Hardware assumptions (confirm before coding)

- ESP32 board of type CYD ESP32-2432S032C made by Shenxhen Jincai Intelligent Co. documentation is here 
https://github.com/joseluu/Demo-CYD-3.2inch-ESP32-2432S032/blob/main/documentation but infortunately has no indication
on how the SDcard connector is connected, look elsewhere
- The SD slot has two electrical modes:
  - **SD/SDMMC host** (1- or 4-bit): available on **classic ESP32** and **ESP32-S3**.
    Full command set, fast. Preferred when the board wires it.
  - **SPI mode**: available on **all** ESP32 variants (incl. S2/C3). Simplest wiring,
    slower, 1-bit. All identity registers are still readable.
- Card is 3.3 V. Provide clean 3.3 V power and correct pull-ups.
- Console over the board's USB-serial (UART or USB-CDC) for reports.

## Recommended framework

**ESP-IDF** (not Arduino) — its `sdmmc`/`sdspi` host drivers already parse CID/CSD/SCR/OCR
into `sdmmc_card_t`, and `sdmmc_send_cmd()` gives raw command access needed for SSR
(ACMD13) and CMD6. Arduino-esp32 wraps the same driver but exposes less raw access.
See `IMPLEMENTATION_PLAN.md` §"Framework & SD access".

## Safety model

- **Default = non-destructive**: register dump + decode, read-only surface scan,
  read benchmark. Never writes to the card.
- **Destructive tests** (capacity verification, write benchmark) are **gated** behind an
  explicit build flag *and* a runtime confirmation, and print a clear data-loss warning.
