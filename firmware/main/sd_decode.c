// sd_decode.c — bit-level register decoders. Layouts per SD_REFERENCE.md.
//
// Bit numbering follows the SD spec: bit 0 is the LSB of the whole register,
// the highest bit index is the MSB. Our raw blobs are MSB-first byte arrays,
// so for an N-bit register: bit i lives in byte (N-1 - i)/8.

#include "sd_decode.h"
#include <string.h>

// Extract `width` bits starting at bit position `lo` (LSB index) from an
// MSB-first byte array of `total_bits` bits. Up to 32-bit results.
static uint32_t bits(const uint8_t *blob, int total_bits, int lo, int width)
{
    uint32_t val = 0;
    for (int i = 0; i < width; i++) {
        int bitpos = lo + i;                       // LSB-indexed
        int byte_idx = (total_bits - 1 - bitpos) / 8;
        int bit_in_byte = bitpos % 8;
        uint32_t b = (blob[byte_idx] >> bit_in_byte) & 1u;
        val |= b << i;
    }
    return val;
}

// 64-bit variant for wider extractions (e.g. capacity intermediate, PSN spans).
static uint64_t bits64(const uint8_t *blob, int total_bits, int lo, int width)
{
    uint64_t val = 0;
    for (int i = 0; i < width; i++) {
        int bitpos = lo + i;
        int byte_idx = (total_bits - 1 - bitpos) / 8;
        int bit_in_byte = bitpos % 8;
        uint64_t b = (blob[byte_idx] >> bit_in_byte) & 1u;
        val |= b << i;
    }
    return val;
}

// --------------------------------------------------------------------------
// CID (128 bits)
// --------------------------------------------------------------------------
bool sd_decode_cid(const sd_raw_t *raw, sd_cid_t *out)
{
    if (!raw->has_cid) return false;
    const uint8_t *c = raw->cid;
    memset(out, 0, sizeof(*out));

    out->mid = bits(c, 128, 120, 8);
    out->oid[0] = (char)bits(c, 128, 112, 8);
    out->oid[1] = (char)bits(c, 128, 104, 8);
    out->oid[2] = '\0';
    for (int i = 0; i < 5; i++)
        out->pnm[4 - i] = (char)bits(c, 128, 64 + i * 8, 8);
    out->pnm[5] = '\0';
    uint8_t prv = bits(c, 128, 56, 8);
    out->prv_major = prv >> 4;
    out->prv_minor = prv & 0xF;
    out->psn = (uint32_t)bits64(c, 128, 24, 32);
    uint32_t mdt = bits(c, 128, 8, 12);
    out->mdt_year = 2000 + ((mdt >> 4) & 0xFF);
    out->mdt_month = mdt & 0xF;
    return true;
}

// --------------------------------------------------------------------------
// CSD (128 bits)
// --------------------------------------------------------------------------
static float decode_tran_speed(uint8_t ts)
{
    static const float units[]   = { 0.1f, 1.0f, 10.0f, 100.0f };  // Mbit/s
    static const float mult[]    = { 0, 1.0f, 1.2f, 1.3f, 1.5f, 2.0f, 2.5f, 3.0f,
                                     3.5f, 4.0f, 4.5f, 5.0f, 5.5f, 6.0f, 7.0f, 8.0f };
    float u = units[ts & 0x7];
    float m = mult[(ts >> 3) & 0xF];
    return u * m;
}

bool sd_decode_csd(const sd_raw_t *raw, sd_csd_t *out)
{
    if (!raw->has_csd) return false;
    const uint8_t *c = raw->csd;
    memset(out, 0, sizeof(*out));

    out->structure      = bits(c, 128, 126, 2);
    out->taac           = bits(c, 128, 112, 8);
    out->nsac           = bits(c, 128, 104, 8);
    out->tran_speed_raw = bits(c, 128, 96, 8);
    out->tran_speed_mhz = decode_tran_speed(out->tran_speed_raw);
    out->ccc            = bits(c, 128, 84, 12);
    out->read_bl_len    = bits(c, 128, 80, 4);
    out->erase_blk_en   = bits(c, 128, 46, 1);
    out->sector_size    = bits(c, 128, 39, 7);
    out->wp_grp_size    = bits(c, 128, 32, 7);
    out->wp_grp_enable  = bits(c, 128, 31, 1);
    out->r2w_factor     = bits(c, 128, 26, 3);
    out->write_bl_len   = bits(c, 128, 22, 4);
    out->copy               = bits(c, 128, 14, 1);
    out->perm_write_protect = bits(c, 128, 13, 1);
    out->tmp_write_protect  = bits(c, 128, 12, 1);
    out->file_format        = bits(c, 128, 10, 2);

    if (out->structure == 0) {
        // v1 SDSC
        uint32_t c_size      = bits(c, 128, 62, 12);
        uint32_t c_size_mult = bits(c, 128, 47, 3);
        uint32_t read_bl     = out->read_bl_len;
        out->capacity_bytes = (uint64_t)(c_size + 1)
                            * (1ULL << (c_size_mult + 2))
                            * (1ULL << read_bl);
    } else if (out->structure == 1) {
        // v2 SDHC/SDXC
        uint32_t c_size = bits(c, 128, 48, 22);
        out->capacity_bytes = (uint64_t)(c_size + 1) * 512ULL * 1024ULL;
    } else {
        // v3 SDUC (C_SIZE 28-bit)
        uint64_t c_size = bits64(c, 128, 48, 28);
        out->capacity_bytes = (c_size + 1) * 512ULL * 1024ULL;
    }
    out->capacity_sectors = out->capacity_bytes / 512ULL;
    return true;
}

// --------------------------------------------------------------------------
// SCR (64 bits)
// --------------------------------------------------------------------------
static const char *phys_version(uint8_t sd_spec, uint8_t s3, uint8_t s4, uint8_t sx)
{
    if (sd_spec == 0) return "1.0/1.01";
    if (sd_spec == 1) return "1.10";
    if (sd_spec == 2 && s3 == 0) return "2.00";
    if (sd_spec == 2 && s3 == 1 && s4 == 0) {
        switch (sx) {
            case 1: return "5.xx";
            case 2: return "6.xx";
            case 3: return "7.xx";
            case 4: return "8.xx";
            default: return "3.0x";
        }
    }
    if (s4 == 1) return "4.xx";
    return "unknown";
}

bool sd_decode_scr(const sd_raw_t *raw, sd_scr_t *out)
{
    if (!raw->has_scr) return false;
    const uint8_t *s = raw->scr;
    memset(out, 0, sizeof(*out));

    out->scr_structure         = bits(s, 64, 60, 4);
    out->sd_spec               = bits(s, 64, 56, 4);
    out->data_stat_after_erase = bits(s, 64, 55, 1);
    out->sd_security           = bits(s, 64, 52, 3);
    out->bus_widths            = bits(s, 64, 48, 4);
    out->sd_spec3              = bits(s, 64, 47, 1);
    out->sd_spec4              = bits(s, 64, 42, 1);
    out->sd_specx              = bits(s, 64, 38, 4);
    out->cmd_support           = bits(s, 64, 32, 2);
    out->phys_version = phys_version(out->sd_spec, out->sd_spec3,
                                     out->sd_spec4, out->sd_specx);
    return true;
}

// --------------------------------------------------------------------------
// SSR (512 bits)
// --------------------------------------------------------------------------
static uint32_t au_code_to_bytes(uint8_t code)
{
    // AU_SIZE code -> bytes (SD spec table). 1=16KiB doubling up to 64MiB.
    static const uint32_t tbl[] = {
        0,            // 0 = not defined
        16u*1024,     // 1
        32u*1024,     // 2
        64u*1024,     // 3
        128u*1024,    // 4
        256u*1024,    // 5
        512u*1024,    // 6
        1024u*1024,   // 7
        2u*1024*1024, // 8
        4u*1024*1024, // 9
        8u*1024*1024, // 10
        12u*1024*1024,// 11
        16u*1024*1024,// 12
        24u*1024*1024,// 13
        32u*1024*1024,// 14
        64u*1024*1024,// 15
    };
    return (code < 16) ? tbl[code] : 0;
}

static uint8_t vsc_decode(uint8_t raw)
{
    // VIDEO_SPEED_CLASS field holds the class number directly (0,6,10,30,60,90).
    return raw;
}

bool sd_decode_ssr(const sd_raw_t *raw, sd_ssr_t *out)
{
    if (!raw->has_ssr) return false;
    const uint8_t *s = raw->ssr;
    memset(out, 0, sizeof(*out));

    out->dat_bus_width    = bits(s, 512, 510, 2);
    out->secured_mode     = bits(s, 512, 509, 1);
    out->sd_card_type     = bits(s, 512, 480, 16);
    out->protected_area   = bits(s, 512, 448, 32);
    out->speed_class      = bits(s, 512, 440, 8);
    out->performance_move = bits(s, 512, 432, 8);
    out->au_size_code     = bits(s, 512, 428, 4);
    out->au_size_bytes    = au_code_to_bytes(out->au_size_code);
    out->erase_size       = bits(s, 512, 408, 16);
    out->erase_timeout    = bits(s, 512, 402, 6);
    out->erase_offset     = bits(s, 512, 400, 2);
    out->uhs_speed_grade  = bits(s, 512, 396, 4);
    out->uhs_au_size_code = bits(s, 512, 392, 4);
    out->video_speed_class= vsc_decode(bits(s, 512, 384, 8));
    out->app_perf_class   = bits(s, 512, 340, 4);
    out->discard_support  = bits(s, 512, 313, 1);
    out->fule_support     = bits(s, 512, 312, 1);
    return true;
}

// --------------------------------------------------------------------------
// OCR (32 bits)
// --------------------------------------------------------------------------
bool sd_decode_ocr(const sd_raw_t *raw, sd_ocr_t *out)
{
    if (!raw->has_ocr) return false;
    const uint8_t *o = raw->ocr;
    memset(out, 0, sizeof(*out));
    out->powered_up     = bits(o, 32, 31, 1);
    out->ccs            = bits(o, 32, 30, 1);
    out->uhs_ii         = bits(o, 32, 29, 1);
    out->s18a           = bits(o, 32, 24, 1);
    out->voltage_window = bits(o, 32, 15, 9);
    return true;
}

// --------------------------------------------------------------------------
// CMD6 SWITCH_FUNC status (512 bits)
// --------------------------------------------------------------------------
bool sd_decode_cmd6(const sd_raw_t *raw, sd_cmd6_t *out)
{
    if (!raw->has_cmd6) return false;
    const uint8_t *d = raw->cmd6;
    memset(out, 0, sizeof(*out));

    out->max_current_ma = bits(d, 512, 496, 16);
    // Function group support bitmaps (16 bits each), groups 1..6 from [415:400]
    // upward. Group 1 = access mode at [415:400].
    out->grp1_supported = bits(d, 512, 400, 16);
    out->grp3_supported = bits(d, 512, 432, 16);  // driver strength
    out->grp4_supported = bits(d, 512, 448, 16);  // current limit

    out->sdr12  = out->grp1_supported & (1 << 0);
    out->sdr25  = out->grp1_supported & (1 << 1);
    out->sdr50  = out->grp1_supported & (1 << 2);
    out->sdr104 = out->grp1_supported & (1 << 3);
    out->ddr50  = out->grp1_supported & (1 << 4);
    return true;
}

// --------------------------------------------------------------------------
void sd_decode_all(const sd_raw_t *raw, sd_decoded_t *out)
{
    memset(out, 0, sizeof(*out));
    sd_decode_cid (raw, &out->cid);
    sd_decode_csd (raw, &out->csd);
    sd_decode_scr (raw, &out->scr);
    sd_decode_ssr (raw, &out->ssr);
    sd_decode_ocr (raw, &out->ocr);
    sd_decode_cmd6(raw, &out->cmd6);
}
