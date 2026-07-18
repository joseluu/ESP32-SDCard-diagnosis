// diag_read.c — non-destructive diagnostics. Never issues a write command.

#include "diag_read.h"
#include "config.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_timer.h"
#include "esp_heap_caps.h"

// CMD13 R1 error-flag bit positions (SD_REFERENCE.md §7).
typedef struct { uint32_t mask; const char *name; } flag_t;
static const flag_t kCardStatusFlags[] = {
    { 1u << 31, "OUT_OF_RANGE"    },
    { 1u << 30, "ADDRESS_ERROR"   },
    { 1u << 29, "BLOCK_LEN_ERROR" },
    { 1u << 28, "ERASE_SEQ_ERROR" },
    { 1u << 27, "ERASE_PARAM"     },
    { 1u << 26, "WP_VIOLATION"    },
    { 1u << 23, "COM_CRC_ERROR"   },
    { 1u << 22, "ILLEGAL_COMMAND" },
    { 1u << 21, "CARD_ECC_FAILED" },
    { 1u << 20, "CC_ERROR"        },
    { 1u << 19, "ERROR"           },
    { 1u << 16, "CSD_OVERWRITE"   },
};
static const int kNumFlags = sizeof(kCardStatusFlags) / sizeof(kCardStatusFlags[0]);

int diag_cmd13_errors(uint32_t status, char *names, int names_len)
{
    int count = 0;
    if (names && names_len) names[0] = '\0';
    for (int i = 0; i < kNumFlags; i++) {
        if (status & kCardStatusFlags[i].mask) {
            count++;
            if (names) {
                if (names[0]) strncat(names, ",", names_len - strlen(names) - 1);
                strncat(names, kCardStatusFlags[i].name,
                        names_len - strlen(names) - 1);
            }
        }
    }
    return count;
}

esp_err_t diag_quick_probe(sd_hal_t *h, probe_result_t *res)
{
    memset(res, 0, sizeof(*res));
    res->burst_fail_lba = 0xFFFFFFFF;

    // 1) Repeated timed bring-up. An intermittent controller — the classic
    //    failed-card signature — is caught by cycling init, not by one try.
    for (int i = 0; i < DIAG_PROBE_INIT_CYCLES; i++) {
        if (h->initialized || h->backend) sd_hal_deinit(h);
        int64_t t0 = esp_timer_get_time();
        res->init_err[i] = sd_hal_init(h);
        res->init_ms[i] = (uint32_t)((esp_timer_get_time() - t0) / 1000);
        if (res->init_err[i] != ESP_OK) res->init_fails++;
    }
    res->card_usable = h->initialized;
    if (!res->card_usable) return ESP_OK;    // caller reports the init failure

    uint32_t chunk = SDDIAG_SCAN_CHUNK_SECTORS;
    uint8_t *buf = NULL;
    while (chunk >= 16 && !(buf = heap_caps_malloc(chunk * 512, MALLOC_CAP_DMA)))
        chunk /= 2;
    if (!buf) return ESP_ERR_NO_MEM;

    // 2) Spot reads across the address space.
    uint64_t total = h->card.csd.capacity;                 // sectors
    uint32_t last = total ? (uint32_t)(total - 1) : 0;
    const uint32_t lbas[DIAG_PROBE_POINTS] = {
        0,
        (uint32_t)(total / 4),
        (uint32_t)(total / 2),
        (uint32_t)(3 * total / 4),
        last,
    };
    for (int i = 0; i < DIAG_PROBE_POINTS; i++) {
        res->lba[i] = lbas[i];
        int64_t t0 = esp_timer_get_time();
        res->err[i] = sd_hal_read_blocks(h, lbas[i], buf, 1);
        res->ms[i] = (uint32_t)((esp_timer_get_time() - t0) / 1000);
        res->points++;
        if (res->err[i] != ESP_OK) res->fails++;
    }

    // 3) Sustained sequential burst (4 MiB) — weak flash that survives single
    //    sectors often trips on a sustained stream.
    uint32_t goal = 4 * 1024 * 2;                          // sectors
    if (goal > total) goal = (uint32_t)total;
    res->burst_ok = true;
    int64_t b0 = esp_timer_get_time();
    uint32_t done = 0;
    while (done < goal) {
        uint32_t n = (goal - done < chunk) ? (goal - done) : chunk;
        if (sd_hal_read_blocks(h, done, buf, n) != ESP_OK) {
            res->burst_ok = false;
            res->burst_fail_lba = done;
            break;
        }
        done += n;
    }
    double bsecs = (esp_timer_get_time() - b0) / 1e6;
    res->burst_kb = done / 2;
    if (bsecs > 0 && done > 0)
        res->burst_mbps = (done * 512.0) / (bsecs * 1e6);
    heap_caps_free(buf);

    // 4) Card status after the workout.
    uint32_t st = 0;
    res->status_ok = (sd_hal_card_status(h, &st) == ESP_OK);
    res->status = st;
    if (res->status_ok) res->status_errs = diag_cmd13_errors(st, NULL, 0);
    return ESP_OK;
}

static int cmp_double(const void *a, const void *b)
{
    double da = *(const double *)a, db = *(const double *)b;
    return (da > db) - (da < db);
}

esp_err_t diag_surface_scan(sd_hal_t *h, scan_result_t *res, void (*progress)(int))
{
    if (!h->initialized) return ESP_ERR_INVALID_STATE;
    memset(res, 0, sizeof(*res));
    res->first_bad_lba = 0xFFFFFFFF;

    // Degrade the chunk size if DMA heap is tight (the LVGL UI uses some).
    uint32_t chunk = SDDIAG_SCAN_CHUNK_SECTORS;             // sectors per chunk
    uint8_t *buf = NULL;
    while (chunk >= 16 && !(buf = heap_caps_malloc(chunk * 512, MALLOC_CAP_DMA)))
        chunk /= 2;
    if (!buf) return ESP_ERR_NO_MEM;

    uint64_t total = h->card.csd.capacity;                 // sectors
    res->total_sectors = total;
    uint64_t nchunks = (total + chunk - 1) / chunk;
    res->chunks_total = (uint32_t)nchunks;

    // Keep a bounded sample of per-chunk speeds for min/median/max. For very
    // large cards we sample (the scan still reads every sector).
    const int kMaxSamples = 2048;
    double *samples = malloc(sizeof(double) * kMaxSamples);
    int nsamp = 0;
    uint32_t sample_stride = (uint32_t)((nchunks + kMaxSamples - 1) / kMaxSamples);
    if (sample_stride == 0) sample_stride = 1;

    int last_pct = -1;
    int consec_fail = 0;
    for (uint64_t ci = 0; ci < nchunks; ci++) {
        uint32_t lba = (uint32_t)(ci * chunk);
        uint32_t this_count = chunk;
        if ((uint64_t)lba + this_count > total) this_count = (uint32_t)(total - lba);

        int64_t t0 = esp_timer_get_time();
        esp_err_t e = sd_hal_read_blocks(h, lba, buf, this_count);
        int64_t t1 = esp_timer_get_time();

        if (e != ESP_OK) {
            res->chunks_failed++;
            consec_fail++;
            if (res->first_bad_lba == 0xFFFFFFFF) res->first_bad_lba = lba;
            // Probe card status to count the error event.
            uint32_t st = 0;
            if (sd_hal_card_status(h, &st) == ESP_OK) {
                if (diag_cmd13_errors(st, NULL, 0) > 0) res->cmd13_error_events++;
            }
            // If the card has wedged (long run of failures), stop scanning so the
            // report returns promptly. The bad region is already recorded.
            if (consec_fail >= 16) {
                res->aborted = true;
                break;
            }
        } else {
            consec_fail = 0;
            res->chunks_ok++;
            res->scanned_sectors += this_count;
            double secs = (t1 - t0) / 1e6;
            if (secs > 0 && (ci % sample_stride == 0) && nsamp < kMaxSamples) {
                double mbps = (this_count * 512.0) / (secs * 1e6);
                samples[nsamp++] = mbps;
            }
        }

        if (progress) {
            int pct = (int)((ci + 1) * 100 / nchunks);
            if (pct != last_pct) { progress(pct); last_pct = pct; }
        }
    }

    if (nsamp > 0) {
        qsort(samples, nsamp, sizeof(double), cmp_double);
        res->min_mbps = samples[0];
        res->max_mbps = samples[nsamp - 1];
        res->median_mbps = samples[nsamp / 2];
        for (int i = 0; i < nsamp; i++)
            if (samples[i] < res->median_mbps / 5.0) res->slow_regions++;
    }

    free(samples);
    heap_caps_free(buf);
    return ESP_OK;
}

// A failing card can stop responding mid-benchmark, after which every command
// times out (~seconds each). Abort once we see this many consecutive failures
// so the report comes back promptly instead of looping for minutes.
#define ABORT_AFTER_CONSEC_FAILS 8

esp_err_t diag_read_bench(sd_hal_t *h, bench_result_t *res)
{
    if (!h->initialized) return ESP_ERR_INVALID_STATE;
    memset(res, 0, sizeof(*res));
    res->seq_fail_lba = 0xFFFFFFFF;

    uint64_t total = h->card.csd.capacity;
    if (total < SDDIAG_BENCH_SEQ_SECTORS) return ESP_ERR_INVALID_SIZE;

    // The LVGL UI eats a chunk of DMA-capable heap; degrade the transfer size
    // rather than fail (same volume read, smaller pieces).
    uint32_t seq = SDDIAG_BENCH_SEQ_SECTORS;
    uint8_t *buf = NULL;
    while (seq >= 16 && !(buf = heap_caps_malloc(seq * 512, MALLOC_CAP_DMA)))
        seq /= 2;
    if (!buf) return ESP_ERR_NO_MEM;

    // Sequential read benchmark (1 MiB from the start). Stop on first error.
    res->seq_ok = (sd_hal_read_blocks(h, 0, buf, seq) == ESP_OK);  // warm
    const int reps = (8 * SDDIAG_BENCH_SEQ_SECTORS) / seq;
    int good = 0;
    int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < reps; i++) {
        uint32_t lba = (uint32_t)(i * seq);
        if (sd_hal_read_blocks(h, lba, buf, seq) != ESP_OK) {
            res->seq_ok = false;
            if (res->seq_fail_lba == 0xFFFFFFFF) res->seq_fail_lba = lba;
            break;
        }
        good++;
    }
    int64_t t1 = esp_timer_get_time();
    double secs = (t1 - t0) / 1e6;
    if (secs > 0 && good > 0)
        res->seq_read_mbps = (good * seq * 512.0) / (secs * 1e6);

    // Random 4 KiB read IOPS. Bail out if the card wedges (consecutive fails).
    uint32_t span = (uint32_t)(total > 8 ? total - 8 : 1);
    int ops = SDDIAG_BENCH_RAND_IOPS_OPS;
    int done = 0, consec = 0;
    int64_t r0 = esp_timer_get_time();
    for (int i = 0; i < ops; i++) {
        res->rand_ops++;
        uint32_t lba = (uint32_t)(((uint64_t)rand() * 2654435761u) % span);
        lba &= ~7u;
        if (sd_hal_read_blocks(h, lba, buf, 8) == ESP_OK) {
            done++; consec = 0;
        } else {
            res->rand_fail++;
            if (++consec >= ABORT_AFTER_CONSEC_FAILS) { res->aborted = true; break; }
        }
    }
    int64_t r1 = esp_timer_get_time();
    double rsecs = (r1 - r0) / 1e6;
    if (rsecs > 0 && done > 0) res->rand_read_iops = done / rsecs;

    heap_caps_free(buf);
    return ESP_OK;
}
