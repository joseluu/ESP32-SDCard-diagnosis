// sd_mfg_db.h — manufacturer-ID / OEM-ID lookup (SD_REFERENCE.md §8).
#pragma once
#include <stdint.h>
#include <stdbool.h>

// Returns a human manufacturer name for a MID, or NULL if unknown.
const char *sd_mfg_name(uint8_t mid);

// Confidence label for a MID lookup ("high"/"med"/"low"), or "unknown".
const char *sd_mfg_confidence(uint8_t mid);

// Typical OID string for a MID (for counterfeit cross-check), or NULL.
const char *sd_mfg_typical_oid(uint8_t mid);

// Heuristic: does the decoded OID look consistent with the MID's typical OID?
// Returns true if consistent or if we have no reference to compare.
bool sd_mfg_oid_consistent(uint8_t mid, const char oid[2]);
