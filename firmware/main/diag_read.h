// diag_read.h — non-destructive diagnostics (default, never writes).
#pragma once
#include "sd_hal.h"

typedef struct {
    uint64_t total_sectors;
    uint64_t scanned_sectors;
    uint32_t chunks_total;
    uint32_t chunks_ok;
    uint32_t chunks_failed;
    double   min_mbps, median_mbps, max_mbps;
    uint32_t slow_regions;       // chunks slower than 5x median
    uint32_t first_bad_lba;      // first failing chunk LBA, or 0xFFFFFFFF
    uint32_t cmd13_error_events; // CMD13 statuses with any error flag set
    bool     aborted;            // stopped early because the card wedged
} scan_result_t;

typedef struct {
    double   seq_read_mbps;
    double   rand_read_iops;
    bool     seq_ok;            // sequential read completed without error
    uint32_t seq_fail_lba;      // LBA of first sequential failure (else 0xFFFFFFFF)
    uint32_t rand_ops;          // random ops attempted
    uint32_t rand_fail;         // random ops that errored
    bool     aborted;           // bailed out because the card stopped responding
} bench_result_t;

// Surface read scan: read the whole card in chunks, time each, count failures
// and CMD13 error flags. Streams; never buffers the whole card. `progress` (may
// be NULL) is called periodically with percent complete.
esp_err_t diag_surface_scan(sd_hal_t *h, scan_result_t *res,
                            void (*progress)(int pct));

// Read benchmark: sequential MB/s + random 4 KiB IOPS (read-only).
esp_err_t diag_read_bench(sd_hal_t *h, bench_result_t *res);

#define DIAG_PROBE_POINTS      5
#define DIAG_PROBE_INIT_CYCLES 3

typedef struct {
    // Repeated bring-up cycles (catches intermittent controllers).
    int       init_fails;
    uint32_t  init_ms[DIAG_PROBE_INIT_CYCLES];
    esp_err_t init_err[DIAG_PROBE_INIT_CYCLES];
    bool      card_usable;               // final bring-up left a working card

    // Spot reads across the address space.
    int       points;                    // probe reads attempted
    int       fails;                     // failed reads
    uint32_t  lba[DIAG_PROBE_POINTS];
    esp_err_t err[DIAG_PROBE_POINTS];    // ESP_OK or the failure
    uint32_t  ms[DIAG_PROBE_POINTS];     // per-read latency

    // Sustained sequential burst from LBA 0.
    bool      burst_ok;
    uint32_t  burst_kb;                  // volume actually read
    uint32_t  burst_fail_lba;            // first failing LBA (else 0xFFFFFFFF)
    double    burst_mbps;

    bool      status_ok;                 // CMD13 answered afterwards
    uint32_t  status;                    // raw R1 status
    int       status_errs;               // decoded error-flag count
} probe_result_t;

// Quick health probe (~5 s). A failed card can serve its identity registers
// fine, so this exercises what actually breaks: several timed bring-up cycles
// (intermittent power-up is the classic failed-card signature), single-sector
// reads spread across the card (start, 25%, 50%, 75%, last), a multi-MiB
// sequential burst, and CMD13. Re-initialises the card as a side effect; the
// caller must refresh its decode/card-ok state from h->initialized.
esp_err_t diag_quick_probe(sd_hal_t *h, probe_result_t *res);

// Decode a CMD13 R1 status word into a count of error flags + a names string.
// Returns the number of error flags set; fills `names` (caller-sized buffer).
int diag_cmd13_errors(uint32_t status, char *names, int names_len);
