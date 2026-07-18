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

// ---------------------------------------------------------------------------
// Capacity check — quasi-non-destructive (f3probe-style backup/restore)
// ---------------------------------------------------------------------------
#define CAPCHK_MAX_POINTS 20
#define CAPCHK_MIN_SHIFT  16          // first probe at 2^16 sectors (32 MiB)

typedef enum {
    CAPCHK_OK = 0,       // probe sector holds its own tag: flash exists there
    CAPCHK_ALIAS,        // holds another probe's tag: address wrap-around
    CAPCHK_LOST,         // written tag vanished: no flash behind this LBA
    CAPCHK_WRITE_ERR,    // the write command itself failed
    CAPCHK_READ_ERR,     // the read-back failed
} capchk_state_t;

typedef struct {
    uint64_t announced_sectors;
    int      points;
    uint32_t lba[CAPCHK_MAX_POINTS];
    capchk_state_t state[CAPCHK_MAX_POINTS];
    uint32_t found_lba[CAPCHK_MAX_POINTS];  // tag found there when ALIAS
    bool     wrap_detected;      // some probe's tag landed on sector 0
    uint32_t wrap_modulus;       // that probe's LBA ≈ the real size
    uint64_t est_real_sectors;   // 0 = no upper bound found (looks genuine)
    bool     restore_ok;         // every saved sector written back intact
} capchk_result_t;

// Probes whether the announced capacity is real WITHOUT losing data: backs up
// ~25 sectors (sector 0, power-of-two probe points, the last sector, and the
// low sectors a wrapped last-sector write could land on), writes an LBA-tagged
// pattern to each probe, RESETS the card (so the controller cache cannot mask
// missing flash — f3probe's trick), looks for wrap-around or silently-
// discarded writes, then restores every backup. Data is only at risk if power
// fails mid-test or the card dies mid-test. Residual limits: a fake whose real
// size is not a power of two can evade the probe pattern; wtest is definitive.
esp_err_t diag_capacity_check(sd_hal_t *h, capchk_result_t *res);

void report_capchk_human(const capchk_result_t *r);

#endif // CONFIG_SDDIAG_ALLOW_DESTRUCTIVE
