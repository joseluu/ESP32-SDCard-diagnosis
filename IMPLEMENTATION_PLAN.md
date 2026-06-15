# Implementation Plan — ESP32 SD-Card Diagnosis Tool

Audience: an implementing agent with ESP-IDF build/flash tooling. This document defines
architecture, file layout, per-module behaviour, milestones, and acceptance criteria.
Bit-level register layouts and decode tables live in `SD_REFERENCE.md`.

---

## 1. Framework & SD access

Use **ESP-IDF v5.x**.

Two host backends behind one abstraction:

- **SDMMC backend** (`driver/sdmmc_host.h`): classic ESP32 & ESP32-S3. 1/4-bit, high speed.
- **SDSPI backend** (`driver/sdspi_host.h`): all variants. 1-bit SPI, simplest wiring.

Key IDF facts the implementer should rely on:

- `sdmmc_card_init(host, &card)` populates `sdmmc_card_t`:
  - `card.cid`  → `sdmmc_cid_t { mfg_id, oem_id, name[8], revision, serial, date }`
  - `card.csd`  → capacity, sector size, `tr_speed`, etc.
  - `card.scr`  → `sd_spec`, `bus_width`
  - `card.ocr`  → raw OCR
  So **CID/CSD/SCR/OCR come essentially for free** from a successful init. The decode
  layer re-parses the *raw* register bytes anyway (to expose every field, not just IDF's
  subset) — keep the raw 16-byte CID/CSD, 8-byte SCR, 4-byte OCR.
- Raw commands: `sdmmc_send_cmd(card, &sdmmc_command_t)`. Needed for:
  - **ACMD13 (SD_STATUS / SSR)**: `CMD55(APP_CMD)` → `ACMD13`, 64-byte data read block.
  - **CMD6 (SWITCH_FUNC)** in *check* mode (mode bit = 0): 64-byte data read block.
  - **CMD13 (SEND_STATUS)**: card status R1/R2 error flags after operations.
  - **CMD58 (READ_OCR)** in SPI mode (in SDMMC, OCR comes from ACMD41 during init).
- **SPI vs SDMMC response differences**: response formats differ (R1/R2 vs R1b, CMD13 is
  R2 in SD mode but R2 over a different path in SPI). The HAL must normalise these.
  Document any field unavailable in SPI mode rather than silently zero it.

> If the chosen board only supports SPI (S2/C3), all identity registers and read-only
> diagnostics still work; only raw-speed ceilings and 4-bit-only behaviours differ.

---

## 2. Proposed file tree

```
SDCard_diagnosis/
├─ README.md
├─ IMPLEMENTATION_PLAN.md
├─ SD_REFERENCE.md
└─ firmware/
   ├─ CMakeLists.txt
   ├─ sdkconfig.defaults          # console, freq, PSRAM if present
   ├─ partitions.csv
   └─ main/
      ├─ CMakeLists.txt
      ├─ app_main.c               # CLI / menu, orchestrates everything
      ├─ config.h                 # pin maps, build-time flags (DESTRUCTIVE_TESTS)
      ├─ sd_hal.h / sd_hal.c      # backend abstraction (sdmmc | sdspi), raw cmd access
      ├─ sd_registers.h           # packed structs + field enums for CID/CSD/SCR/SSR/OCR/CMD6
      ├─ sd_decode.c              # raw bytes -> structured fields -> human strings
      ├─ sd_mfg_db.h / .c         # manufacturer-ID + OEM-ID lookup tables
      ├─ diag_read.c              # read-only: surface scan, read benchmark, CMD13 monitor
      ├─ diag_write.c             # DESTRUCTIVE: capacity verify (f3-style), write benchmark
      ├─ report.c / report.h      # human + JSON output, optional file/WiFi sink
      └─ vendor_health.c          # ⭐ stretch: CMD56 / industrial SMART hooks
```

Keep the docs at the top level; put all code under `firmware/`.

---

## 3. Module specifications

### 3.1 `sd_hal` — host abstraction
- `sd_hal_init(cfg)` selects backend from `config.h`, powers/initialises the card, returns
  the populated `sdmmc_card_t` and the **raw** register blobs.
- `sd_hal_read_blocks(lba, buf, count)` / `sd_hal_write_blocks(...)`.
- `sd_hal_send_app_cmd_data(acmd, arg, buf, len)` — for ACMD13.
- `sd_hal_switch_func(check_or_set, group_args, status64)` — CMD6.
- `sd_hal_card_status(&r)` — CMD13.
- `sd_hal_get_raw(&cid16,&csd16,&scr8,&ocr4)`.
- Must expose bus width / clock actually negotiated, and a capability flag set per backend.

### 3.2 `sd_decode` + `sd_registers` — the heart of "rtsx_pci or more"
Parse **every** documented field per `SD_REFERENCE.md`:
- **CID**: MID, OID(2 ASCII), PNM(5 ASCII), PRV(n.m), PSN(hex), MDT(year/month).
- **CSD**: structure ver, TAAC/NSAC, TRAN_SPEED→MHz, CCC bitmap→class list, READ/WRITE
  block len, capacity (handle CSD v1 SDSC, v2 SDHC/SDXC, v3 SDUC), erase params, WP flags,
  file format, copy/perm flags.
- **SCR**: SCR structure, physical-layer spec version (combine SD_SPEC/SPEC3/SPEC4/SPECX
  → "2.0/3.0/4.x/5.x/6.x/7.x"), bus widths, security, CMD_SUPPORT (CMD20/23/48-49/58-59).
- **SSR**: DAT_BUS_WIDTH, SD_CARD_TYPE, protected-area size, **SPEED_CLASS**,
  PERFORMANCE_MOVE, AU_SIZE, ERASE_SIZE/TIMEOUT/OFFSET, **UHS_SPEED_GRADE**, UHS_AU_SIZE,
  **VIDEO_SPEED_CLASS**, VSC_AU_SIZE, **APP_PERF_CLASS (A1/A2)**, DISCARD/FULE support.
- **OCR**: CCS (SDSC vs SDHC/XC), S18A (1.8 V switched), UHS-II, voltage window.
- **CMD6 status**: per-group supported-function bitmaps → list supported **access modes**
  (SDR12/25/50/104, DDR50), driver strengths (B/A/C/D), current limits (200/400/600/800 mA).

Each decoded struct gets a `..._to_string()` producing an aligned human block, and a
`..._to_json()` producing machine output.

### 3.3 `sd_mfg_db` — identity lookup
- `const char* sd_mfg_name(uint8_t mid)` and `sd_oem_name(const char oid[2])`.
- Seed table from `SD_REFERENCE.md` §"Manufacturer IDs". Mark entries low/high confidence;
  unknown → print the raw hex so nothing is lost. **This is what makes a relabeled fake
  obvious** (e.g. a "Samsung Pro Plus" whose MID ≠ 0x1b / OID ≠ "SM").

### 3.4 `diag_read` — non-destructive diagnostics (default)
- **Surface read scan**: read the whole card in chunks (e.g. 64 KiB), time each chunk,
  record per-region throughput and any read failure / CMD13 error flag; flag chunks whose
  latency exceeds N× median (weak/retried regions). Report min/median/max MB/s and a
  bad-region list. Stream — never buffer the whole card.
- **Read benchmark**: sequential (large blocks) + random 4 KiB IOPS. Report real numbers —
  this finally measures the card's true speed with no USB bottleneck.
- **CMD13 monitor**: after each op, decode card status; count CRC/ECC/retry/timeout flags.

### 3.5 `diag_write` — DESTRUCTIVE diagnostics (flag-gated)
- Build-time `CONFIG_SDDIAG_ALLOW_DESTRUCTIVE` **and** runtime "type YES" confirmation.
- **Capacity verification (f3-style fake detection)**: write a unique, position-stamped
  pattern across the *entire* address space (each block carries its own LBA + a magic +
  a PRNG body keyed to the LBA), then read back and verify. Mismatch / wrap-around ⇒
  fake or overstated capacity; read error ⇒ bad block. Report announced vs verified-good
  sectors (mirrors `f3probe`/`f3write`+`f3read`).
- **Write benchmark**: sequential write MB/s + random write IOPS, with steady-state note
  (watch for SLC-cache cliff on large writes).
- Stream patterns; keep RAM bounded (single block/region buffer reused).

### 3.6 `report` — output
- Default: human-readable to the serial console, grouped (Identity / Capabilities /
  Health / Benchmarks).
- `--json` (or menu toggle): single JSON document with every decoded field + results,
  for machine consumption.
- Optional sinks (stretch): write the report to a file on the card itself (only in a
  non-destructive run), or serve it over a tiny WiFi HTTP endpoint.

### 3.7 `app_main` — orchestration
- Simple serial menu / command set:
  `info` (registers+decode), `caps` (CMD6/OCR/SCR capability summary),
  `scan` (surface read), `bench` (read bench; `bench -w` write bench if allowed),
  `verify` (capacity/fake test, destructive), `json` (dump), `help`.
- On boot: init card, auto-run `info`. Refuse destructive commands unless gated+confirmed.

### 3.8 `vendor_health` — ⭐ stretch
- Hook for **CMD56 (GEN_CMD)** and known industrial vendor health reports (ATP, Swissbit,
  Apacer, Western Digital/SanDisk industrial). Detect by MID/OID; if supported, read and
  decode the vendor health/SMART block (spare-block %, erase counts, power cycles). On
  consumer cards this is absent — degrade gracefully.

---

## 4. Milestones

| # | Milestone | Output / exit criteria |
| --- | --- | --- |
| **M0** | Project skeleton + card init | `firmware/` builds; SDMMC **and** SDSPI init a card; prints capacity & negotiated bus width/clock. |
| **M1** | Register dump + decode (CID/CSD/SCR/OCR) | `info` prints all fields matching `SD_REFERENCE.md`; cross-checked vs Linux `mmcblk` sysfs on a known card. |
| **M2** | SSR (ACMD13) + CMD6 decode | Speed class, UHS grade, video class, A1/A2, and supported access modes printed. |
| **M3** | Manufacturer DB + pretty report | MID/OID resolve to names; unknown shows raw hex; grouped human report. |
| **M4** | Read-only diagnostics | `scan` + `bench` produce real throughput/IOPS + weak-region/error report; zero writes. |
| **M5** | Destructive diagnostics (gated) | `verify` detects fake capacity & bad blocks on a deliberately-prepared/known-good card; write bench works; guard rails enforced. |
| **M6** | Reporting | `--json` emits complete machine doc; (optional) file/WiFi sink. |
| **M7** | ⭐ Vendor health | CMD56 path on at least one industrial card, or clean "unsupported" on consumer cards. |

---

## 5. Acceptance criteria

1. **Parity**: every field Linux `rtsx_pci` exposes (`cid, csd, scr, ssr, manfid, oemid,
   name, hwrev, fwrev, serial, date, type`) is produced and **matches** for a reference
   card read in both places.
2. **Superset**: CMD6 supported modes, OCR S18A/CCS/UHS, decoded SSR speed/video/app
   classes, and live read benchmarks are all present — none of which sysfs provides.
3. **Fake detection**: on a known-genuine card, `verify` reports announced == verified;
   the algorithm is capable of flagging wrap-around (validate with a simulated/loopback
   test or a known-bad card).
4. **Safety**: a default build never issues a write command; destructive paths require
   both the build flag and runtime confirmation.
5. **Portability**: builds and runs on at least one SDMMC-capable board (ESP32/S3) and one
   SPI-only board (S2/C3), with capability differences reported, not crashed on.
6. **Output**: human and JSON reports for the same run carry identical data.

---

## 6. Notes, pitfalls, references

- **MDT date**: 12-bit field; year = 2000 + `[19:12]`, month = `[11:8]`. See reference.
- **CSD capacity**: branch on CSD structure version (v1 SDSC vs v2 SDHC/XC vs v3 SDUC) —
  a frequent decode bug.
- **ACMD pattern**: always `CMD55` (with RCA) immediately before any `ACMD`.
- **CMD6 has two modes**: *check* (mode=0, read-only, safe — use for capability discovery)
  vs *set* (mode=1, actually switches). Only ever use *check* for diagnosis.
- **RAM**: stream all whole-card operations; never allocate card-sized buffers.
- **SPI quirks**: enable CRC (`CMD59`) for integrity; handle the R1 idle/illegal-command
  responses during init; OCR via `CMD58`.
- Authoritative spec: *SD Physical Layer Simplified Specification* (sdcard.org) — register
  tables there are the source of truth; `SD_REFERENCE.md` summarises the needed subset.
- ESP-IDF reference: `components/sdmmc/` (esp-idf) for `sdmmc_send_cmd`, ACMD helpers,
  and the existing CID/CSD parsers to mirror/extend.
