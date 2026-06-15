# SD Register & Command Reference (decode subset)

Bit ranges use SD spec convention `[MSB:LSB]`, MSB-first within the register. Always
verify against the *SD Physical Layer Simplified Specification* (sdcard.org); this is a
working summary for the decoder. Where a field is wider than needed, the implementer
should still capture it raw so nothing is lost.

---

## 0. Physical layer, connector & bus modes

Foundational layer beneath the register/command sections: how the host is electrically
wired to the card, and which speed modes exist. **The ESP32 only ever uses the "Row-1"
legacy bus (§0.2); everything in §0.4 is out of reach and listed only so the report can
*describe* a card honestly.**

### 0.1 Pins — the same contacts carry two protocols (selected at power-up)

microSD = 8 contacts (full-size SD = 9: same signals + extra GND). 3.3 V logic; pull-ups
(~10–50 kΩ) required on CMD and DAT lines.

| Pin (µSD) | SD bus mode | SPI mode | Power |
| --- | --- | --- | --- |
| 1 | DAT2 | — (hold high) | |
| 2 | DAT3 | **CS** | |
| 3 | **CMD** | **DI** (MOSI) | |
| 4 | | | **VDD** 2.7–3.6 V |
| 5 | **CLK** | **SCLK** | |
| 6 | | | **VSS** (GND) |
| 7 | DAT0 | **DO** (MISO) | |
| 8 | DAT1 | — (hold high) | |

### 0.2 Row-1 legacy bus (what the ESP32 drives)

**SD native bus mode** (command/response, host-clocked state machine):
- **CLK** host→card; **CMD** bidirectional (open-drain during identification → push-pull);
  **DAT0–3** data (1-bit = DAT0 only; 4-bit = nibble/clock across DAT0–3, via ACMD6).
- Command frame on CMD = 48 bits: `[start 0][dir 1][6-bit index][32-bit arg][CRC7][end 1]`.
- Responses: R1 (48-bit status), R1b (+busy), **R2** (136-bit, returns CID/CSD), R3 (OCR),
  R6 (RCA), R7 (interface cond). Data blocks (512 B default) each carry a **16-bit CRC**
  per DAT line; card holds DAT0 low while busy/programming. Cards addressed by **RCA**.

**SPI mode** (legacy compatibility, SPI mode 0):
- Pin remap CS=DAT3, MOSI=CMD, MISO=DAT0, SCLK=CLK; 1-bit only, no UHS, ≤25 MHz.
- Same logical commands; token responses (R1=1 B, R2=2 B, R3/R7=5 B); data packets start
  token `0xFE` + trailing CRC16 (CRC off by default except CMD0/CMD8; enable via CMD59).
- **Mode is chosen by the CS level at CMD0**: CS low → SPI mode until power cycle.

### 0.3 Speed grades on Row-1

| Grade | Pins | Clock / signalling | Throughput |
| --- | --- | --- | --- |
| Default speed | 9 (Row-1) | ≤25 MHz, 3.3 V | ~12 MB/s |
| High speed | 9 | ≤50 MHz, 3.3 V | ~25 MB/s |
| **UHS-I** (SDR12/25/50/104, DDR50) | **9 (no extra pins)** | up to 208 MHz, **1.8 V** (switch via CMD11, gated by OCR S18A) | up to ~104 MB/s |

> UHS-I adds **no** pins — same Row-1, just faster clock + 1.8 V signalling.

### 0.4 Extra-row high-speed modes — backward-compatible, NOT usable by ESP32

These add a **second (and third) row of pins behind Row-1**; Row-1 is unchanged, so the
card still works in any legacy slot and only negotiates the fast path when both ends have
the extra rows.

| Mode | SD ver | Connector | Physical interface | Max |
| --- | --- | --- | --- | --- |
| **UHS-II** | 4.0 | +2nd row (full SD 9→**17 pins**) | **LVDS differential** pairs (D0±, D1±) + RCLK — serial SerDes, packet protocol (not CMD frames) | 156 / 312 MB/s |
| **UHS-III** | 6.0 | same 2-row | LVDS differential, full-duplex both lanes | 624 MB/s |
| **SD Express** | 7.0/7.1 | 2nd row (+3rd for ×2) | **PCIe + NVMe** over the extra pins | ~985 MB/s … ~3.9 GB/s |

### 0.5 ESP32 host limits — state this in the report

- **ESP32 / ESP32-S3**: SDMMC host drives Row-1 native bus, 1-bit/4-bit, realistically only
  up to high-speed **3.3 V** (~40–50 MHz). **ESP32-S2 / C3**: SPI mode only.
- **No** ESP32 can do UHS-II/III (no LVDS PHY) or SD Express (no PCIe/NVMe); UHS-I 1.8 V
  SDR104 is not practically available either.
- ⇒ The tool drives **Row-1 only**. It can **infer & report** a card's *claimed* UHS-II/
  Express capability (SCR physical-layer version ≥ 4.0 → UHS-II era; CMD6 supported access
  modes for UHS-I), but it **cannot exercise** those paths. Benchmarks are therefore bounded
  by the **ESP32 host**, not the card — the report must say so, so a fast card is never
  mislabeled "slow."

---

## 1. CID — Card Identification (16 bytes, 128 bits) — read via CMD10

| Field | Bits | Notes |
| --- | --- | --- |
| MID  Manufacturer ID | [127:120] | 8-bit; look up in §6 table |
| OID  OEM/Application ID | [119:104] | 2 ASCII chars |
| PNM  Product name | [103:64] | 5 ASCII chars |
| PRV  Product revision | [63:56] | BCD: high nibble.low nibble → "n.m" |
| PSN  Serial number | [55:24] | 32-bit, print hex |
| —    reserved | [23:20] | 4 bits |
| MDT  Manufacturing date | [19:8] | year = 2000 + bits[19:12]; month = bits[11:8] (1–12) |
| CRC7 | [7:1] | + end bit [0] |

> Counterfeit tell: printed brand vs MID/OID mismatch (e.g. genuine Samsung MID `0x1b`,
> OID `"SM"`; SanDisk MID `0x03`, OID `"SD"`).

---

## 2. CSD — Card-Specific Data (16 bytes) — read via CMD9

Branch on **CSD_STRUCTURE** `[127:126]`: `0`=v1 (SDSC), `1`=v2 (SDHC/SDXC), `2`=v3 (SDUC).

Common fields:
| Field | Bits | Notes |
| --- | --- | --- |
| CSD_STRUCTURE | [127:126] | version selector |
| TAAC | [119:112] | async read access time |
| NSAC | [111:104] | clock-cycle access time |
| TRAN_SPEED | [103:96] | max bus speed; decode unit×value → MHz (25/50/100/200) |
| CCC | [95:84] | card command classes bitmap → list supported classes |
| READ_BL_LEN | [83:80] | max read block len = 2^value |
| CCC/flags | [79:76] | read partial / write misalign / etc. |

Capacity:
- **v1 (SDSC)**: `C_SIZE`[73:62], `C_SIZE_MULT`[49:47], `READ_BL_LEN`[83:80];
  `capacity_bytes = (C_SIZE+1) * 2^(C_SIZE_MULT+2) * 2^READ_BL_LEN`.
- **v2 (SDHC/SDXC)**: `C_SIZE`[69:48]; `capacity_bytes = (C_SIZE+1) * 512 KiB`.
- **v3 (SDUC)**: `C_SIZE`[75:48]; `capacity_bytes = (C_SIZE+1) * 512 KiB`.

Also decode: ERASE_BLK_EN[46], SECTOR_SIZE[45:39], WP_GRP_SIZE[38:32], WP_GRP_ENABLE[31],
R2W_FACTOR[28:26], WRITE_BL_LEN[25:22], COPY[14], PERM_WRITE_PROTECT[13],
TMP_WRITE_PROTECT[12], FILE_FORMAT[11:10].

---

## 3. SCR — SD Configuration Register (8 bytes) — read via ACMD51

| Field | Bits | Notes |
| --- | --- | --- |
| SCR_STRUCTURE | [63:60] | |
| SD_SPEC | [59:56] | with SPEC3/4/X → physical-layer version |
| DATA_STAT_AFTER_ERASE | [55] | |
| SD_SECURITY | [54:52] | none / SDSC / SDHC / SDXC security |
| SD_BUS_WIDTHS | [51:48] | bit0=1-bit, bit2=4-bit |
| SD_SPEC3 | [47] | |
| EX_SECURITY | [46:43] | |
| SD_SPEC4 | [42] | |
| SD_SPECX | [41:38] | |
| CMD_SUPPORT | [33:32] | bit0=CMD23, bit1=CMD20(speed-class ctrl); newer: CMD48/49,58/59 |

**Physical-layer version mapping** (combine fields):
`SD_SPEC=0`→1.0/1.01; `=1`→1.10; `=2 & SPEC3=0`→2.00; `=2 & SPEC3=1 & SPEC4=0`→3.0x;
`SPEC4=1`→4.xx; then `SD_SPECX`: `1`→5.xx, `2`→6.xx, `3`→7.xx, `4`→8.xx.

---

## 4. SSR — SD Status (64 bytes / 512 bits) — read via ACMD13 (after CMD55)

| Field | Bits | Decode |
| --- | --- | --- |
| DAT_BUS_WIDTH | [511:510] | 0=1-bit, 2=4-bit |
| SECURED_MODE | [509] | |
| SD_CARD_TYPE | [495:480] | 0=RD/WR, 1=ROM, 2=OTP |
| SIZE_OF_PROTECTED_AREA | [479:448] | bytes |
| **SPEED_CLASS** | [447:440] | 0=Class0,1=Class2,2=Class4,3=Class6,4=Class10 |
| PERFORMANCE_MOVE | [439:432] | MB/s (0xFF = ∞/N.A.) |
| AU_SIZE | [431:428] | code → 16 KiB … 64 MiB |
| ERASE_SIZE | [423:408] | AUs erased in one op |
| ERASE_TIMEOUT | [407:402] | seconds |
| ERASE_OFFSET | [401:400] | |
| **UHS_SPEED_GRADE** | [399:396] | 0=<U1, 1=U1(10 MB/s), 3=U3(30 MB/s) |
| UHS_AU_SIZE | [395:392] | code → AU size |
| **VIDEO_SPEED_CLASS** | [391:384] | 0,6,10,30,60,90 → V6…V90 |
| VSC_AU_SIZE | [377:368] | |
| SUS_ADDR | [367:346] | |
| **APP_PERF_CLASS** | [343:340] | 0=none,1=A1,2=A2 |
| PERFORMANCE_ENHANCE | [339:332] | |
| DISCARD_SUPPORT | [313] | |
| FULE_SUPPORT | [312] | full-user-area logical erase |

---

## 5. OCR — Operating Conditions (32 bits) — CMD58 (SPI) / from ACMD41 (SD)

| Bit | Meaning |
| --- | --- |
| 31 | Card power-up busy (1=ready) |
| 30 | **CCS** Card Capacity Status: 0=SDSC, 1=SDHC/SDXC |
| 29 | UHS-II card status |
| 24 | **S18A** — 1.8 V signalling accepted (UHS-I) |
| 23:15 | VDD voltage window (2.7–3.6 V) |

---

## 6. CMD6 SWITCH_FUNC status (64 bytes) — *check mode* (mode bit = 0, read-only)

Returns supported/selectable functions per group. Decode at least:
- **Group 1 — Access (bus speed) mode**: bit0 SDR12 (default), bit1 SDR25 (HS),
  bit2 SDR50, bit3 SDR104, bit4 DDR50. → report which UHS modes the card supports.
- **Group 3 — Driver strength**: Type B(default)/A/C/D.
- **Group 4 — Current/Power limit**: 200/400/600/800 mA.
- Max current/consumption field `[511:496]`; function-group support bitmaps follow.

> Use **check mode only** for diagnosis. *Set* mode (mode bit = 1) actually switches the
> card and must not be used by a read-only report.

---

## 7. Card status (CMD13, R1/R2) — error flags to monitor

Watch and count: `OUT_OF_RANGE`, `ADDRESS_ERROR`, `BLOCK_LEN_ERROR`, `ERASE_SEQ_ERROR`,
`COM_CRC_ERROR`, `ILLEGAL_COMMAND`, `CARD_ECC_FAILED`, `CC_ERROR`, `ERROR` (general),
`WP_VIOLATION`, `CSD_OVERWRITE`. Rising CRC/ECC/timeout counts during scan/bench are the
best available proxy for a failing consumer card (no SMART exists).

---

## 8. Manufacturer IDs (MID) — starter database

Community-sourced; **mark confidence and always keep raw hex for unknowns**. High-confidence
entries first. The implementer should make this table easy to extend.

| MID (hex) | Manufacturer | Typical OID | Confidence |
| --- | --- | --- | --- |
| 0x01 | Panasonic | "PA" | med |
| 0x02 | Toshiba / Kioxia | "TM" | high |
| 0x03 | SanDisk / WD | "SD" | high |
| 0x09 | ATP | "AP" | med |
| 0x13 | Micron | — | med |
| 0x1b | Samsung | "SM" | high |
| 0x1d | ADATA | "AD" | med |
| 0x27 | Phison | "PH" | high |
| 0x28 | Lexar | "BE" | high |
| 0x31 | Silicon Power | — | med |
| 0x41 | Kingston | "42" | high |
| 0x6f | STEC | — | low |
| 0x74 | Transcend / PNY (some) | "JE"/"J`" | med |
| 0x76 | Patriot | — | low |
| 0x82 | Sony / Gobe | "JT"/"BG" | med |
| 0x9c | Angelbird / Hoodman (Barun) | "BE"/"SO" | med |

Notes:
- OID is 2 ASCII chars and is often a more reliable cross-check than MID alone.
- A **mismatch** between the printed brand and decoded MID/OID is a strong counterfeit
  signal — surface it prominently in the report.
- Several MIDs are reused/ambiguous across vendors and firmware batches; never assert a
  vendor with high confidence on MID alone — corroborate with OID + PNM.

---

## 9. Quick cross-check against Linux (for M1 validation)

On a machine with a native `rtsx_pci`/`mmc` reader, for the same card:
```
cat /sys/block/mmcblk0/device/{cid,csd,scr,ssr,manfid,oemid,name,serial,date}
```
The tool's raw CID/CSD/SCR/SSR hex must match these byte-for-byte; decoded
manfid/oemid/name/serial/date must match the sysfs values.
