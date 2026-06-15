// report.h — human-readable + JSON output over the serial console.
#pragma once
#include "sd_hal.h"
#include "sd_decode.h"
#include "diag_read.h"

// Identity / capabilities / health, grouped human-readable block.
void report_identity_human(const sd_hal_t *h, const sd_decoded_t *d);
void report_caps_human(const sd_decoded_t *d);

// Surface scan / benchmark results.
void report_scan_human(const scan_result_t *s);
void report_bench_human(const bench_result_t *b);

// Single JSON document with every decoded field (machine consumption).
void report_json(const sd_hal_t *h, const sd_decoded_t *d);

// Report a failed card bring-up (why init died) — used when no card decode is
// possible. Still maximally informative.
void report_init_failure_human(const sd_hal_t *h);
