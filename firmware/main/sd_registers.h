// sd_registers.h — raw register blobs + decoded field structs.
// Layouts follow SD_REFERENCE.md (SD Physical Layer Simplified Spec subset).

#pragma once
#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Raw register blobs, captured byte-for-byte so nothing is lost.
// CID/CSD are 16 bytes, SCR 8 bytes, OCR 4 bytes, SSR 64 bytes, CMD6 64 bytes.
// All stored MSB-first (byte[0] = most significant byte) to match sysfs hex.
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t cid[16];
    uint8_t csd[16];
    uint8_t scr[8];
    uint8_t ocr[4];
    uint8_t ssr[64];
    uint8_t cmd6[64];   // CMD6 SWITCH_FUNC check-mode status
    bool    has_cid, has_csd, has_scr, has_ocr, has_ssr, has_cmd6;
} sd_raw_t;

// ---------------------------------------------------------------------------
// Decoded CID
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t  mid;          // manufacturer id
    char     oid[3];       // 2 ASCII + NUL
    char     pnm[6];       // 5 ASCII + NUL
    uint8_t  prv_major;    // product revision high nibble
    uint8_t  prv_minor;    // product revision low nibble
    uint32_t psn;          // 32-bit serial
    uint16_t mdt_year;     // 2000 + bits[19:12]
    uint8_t  mdt_month;    // bits[11:8]
} sd_cid_t;

// ---------------------------------------------------------------------------
// Decoded CSD
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t  structure;        // 0=v1,1=v2,2=v3
    uint8_t  taac, nsac;
    uint8_t  tran_speed_raw;
    float    tran_speed_mhz;
    uint16_t ccc;              // command class bitmap
    uint8_t  read_bl_len;
    uint8_t  write_bl_len;
    bool     erase_blk_en;
    uint8_t  sector_size;      // erase sector size code (+1 = units)
    uint8_t  wp_grp_size;
    bool     wp_grp_enable;
    uint8_t  r2w_factor;
    bool     copy;
    bool     perm_write_protect;
    bool     tmp_write_protect;
    uint8_t  file_format;
    uint64_t capacity_bytes;
    uint64_t capacity_sectors; // capacity_bytes / 512
} sd_csd_t;

// ---------------------------------------------------------------------------
// Decoded SCR
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t  scr_structure;
    uint8_t  sd_spec, sd_spec3, sd_spec4, sd_specx;
    const char *phys_version;   // e.g. "3.0x"
    bool     data_stat_after_erase;
    uint8_t  sd_security;
    uint8_t  bus_widths;        // bit0=1bit, bit2=4bit
    uint8_t  cmd_support;       // bit0=CMD23, bit1=CMD20
} sd_scr_t;

// ---------------------------------------------------------------------------
// Decoded SSR
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t  dat_bus_width;     // 0=1bit, 2=4bit
    bool     secured_mode;
    uint16_t sd_card_type;
    uint32_t protected_area;
    uint8_t  speed_class;       // 0,1,2,3,4 -> Class 0/2/4/6/10
    uint8_t  performance_move;
    uint8_t  au_size_code;
    uint32_t au_size_bytes;
    uint16_t erase_size;
    uint8_t  erase_timeout;
    uint8_t  erase_offset;
    uint8_t  uhs_speed_grade;   // 0,1,3
    uint8_t  uhs_au_size_code;
    uint8_t  video_speed_class; // 0,6,10,30,60,90
    uint8_t  app_perf_class;    // 0=none,1=A1,2=A2
    bool     discard_support;
    bool     fule_support;
} sd_ssr_t;

// ---------------------------------------------------------------------------
// Decoded OCR
// ---------------------------------------------------------------------------
typedef struct {
    bool     powered_up;        // bit31
    bool     ccs;               // bit30 (0=SDSC,1=SDHC/XC)
    bool     uhs_ii;            // bit29
    bool     s18a;              // bit24
    uint16_t voltage_window;    // bits 23:15
} sd_ocr_t;

// ---------------------------------------------------------------------------
// Decoded CMD6 SWITCH_FUNC (check mode)
// ---------------------------------------------------------------------------
typedef struct {
    uint16_t max_current_ma;        // bits [511:496]
    uint16_t grp1_supported;        // access (bus speed) modes bitmap
    uint16_t grp3_supported;        // driver strength bitmap
    uint16_t grp4_supported;        // current limit bitmap
    // convenience flags from group 1
    bool sdr12, sdr25, sdr50, sdr104, ddr50;
} sd_cmd6_t;

typedef struct {
    sd_cid_t  cid;
    sd_csd_t  csd;
    sd_scr_t  scr;
    sd_ssr_t  ssr;
    sd_ocr_t  ocr;
    sd_cmd6_t cmd6;
} sd_decoded_t;
