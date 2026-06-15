// sd_decode.h — raw register blobs -> structured fields.
#pragma once
#include "sd_registers.h"

// Decode each register from raw bytes into the matching struct. Each returns
// true if the corresponding raw blob was present and decoded.
bool sd_decode_cid (const sd_raw_t *raw, sd_cid_t  *out);
bool sd_decode_csd (const sd_raw_t *raw, sd_csd_t  *out);
bool sd_decode_scr (const sd_raw_t *raw, sd_scr_t  *out);
bool sd_decode_ssr (const sd_raw_t *raw, sd_ssr_t  *out);
bool sd_decode_ocr (const sd_raw_t *raw, sd_ocr_t  *out);
bool sd_decode_cmd6(const sd_raw_t *raw, sd_cmd6_t *out);

// Decode all available registers in one call.
void sd_decode_all(const sd_raw_t *raw, sd_decoded_t *out);
