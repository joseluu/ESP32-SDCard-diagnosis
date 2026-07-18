// diag_write.h — DESTRUCTIVE write/verify diagnostics.
//
// This is the ONLY module that writes to the card. It is compiled and callable
// only when CONFIG_SDDIAG_ALLOW_DESTRUCTIVE is set, and the caller must still
// gate it behind an explicit runtime confirmation (see app_main `wtest`).
#pragma once
#include "sd_hal.h"

#if CONFIG_SDDIAG_ALLOW_DESTRUCTIVE

typedef struct {
    uint64_t total_sectors;
    uint64_t sectors_written;     // sectors successfully written
    uint64_t sectors_verified;    // sectors that read back exactly as written
    uint32_t write_failures;      // chunk write errors (I/O level)
    uint32_t verify_mismatches;   // sectors whose content did NOT match
    uint32_t read_failures;       // chunk read errors during verify
    uint32_t first_write_fail_lba;
    uint32_t first_verify_fail_lba;
    uint32_t wrap_detected_lba;   // first sector returning a foreign LBA tag
    uint32_t wrap_seen_lba;       // the (wrong) self-address tag found there
    double   write_mbps;
    double   read_mbps;
    bool     write_aborted;       // write phase bailed (card wedged)
    bool     verify_aborted;      // verify phase bailed (card wedged)
} write_result_t;

// Full-card destructive write/verify (h2testw-style). Writes a deterministic,
// LBA-tagged pattern over EVERY sector, then reads the whole card back and
// verifies. Detects write corruption, dead sectors, and fake-capacity wrap
// (a high sector returning data tagged with a different/lower LBA). `progress`
// (may be NULL) is called with 0..100 (0..50 = write, 50..100 = verify).
esp_err_t diag_write_verify(sd_hal_t *h, write_result_t *res,
                            void (*progress)(int pct));

// Human-readable verdict for a write/verify run.
void report_write_human(const write_result_t *r);

#endif // CONFIG_SDDIAG_ALLOW_DESTRUCTIVE
