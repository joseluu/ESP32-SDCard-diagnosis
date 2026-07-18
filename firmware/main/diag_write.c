// diag_write.c — DESTRUCTIVE write/verify test (h2testw-style).
//
// Compiled only when CONFIG_SDDIAG_ALLOW_DESTRUCTIVE is set. The whole card is
// overwritten with a per-sector, position-dependent pattern and then read back
// and verified. The pattern is a function of the sector's own LBA, so it is
// recomputable during verify without storing the whole card, and a sector that
// returns the wrong self-address tag exposes capacity-fraud "wrap" cards.

#include "config.h"

#if CONFIG_SDDIAG_ALLOW_DESTRUCTIVE

#include "diag_write.h"
#include <string.h>
#include <stdio.h>
#include "esp_timer.h"
#include "esp_heap_caps.h"

// Deterministic 512-byte sector content derived from its LBA. word[0] is the
// sector's own LBA (self-address tag for wrap detection), word[1] its inverse,
// and the rest an LCG stream stirred with the LBA so neighbouring sectors never
// share content (defeats cards that alias many LBAs onto one flash page).
static void fill_sector(uint8_t *p, uint32_t lba)
{
    uint32_t *w = (uint32_t *)p;
    uint32_t x = lba * 2654435761u + 0xA5A5A5A5u;
    for (int i = 0; i < 128; i++) {
        x = x * 1664525u + 1013904223u;            // LCG step
        w[i] = x ^ ((uint32_t)lba + (uint32_t)i * 0x9E3779B9u);
    }
    w[0] = lba;
    w[1] = ~lba;
}

#define ABORT_AFTER_CONSEC_FAILS 16

esp_err_t diag_write_verify(sd_hal_t *h, write_result_t *res,
                            void (*progress)(int))
{
    if (!h->initialized) return ESP_ERR_INVALID_STATE;
    memset(res, 0, sizeof(*res));
    res->first_write_fail_lba  = 0xFFFFFFFF;
    res->first_verify_fail_lba = 0xFFFFFFFF;
    res->wrap_detected_lba     = 0xFFFFFFFF;

    const uint32_t chunk = SDDIAG_SCAN_CHUNK_SECTORS;   // 128 sectors / 64 KiB
    const uint32_t bytes = chunk * 512;
    uint8_t *wbuf = heap_caps_malloc(bytes, MALLOC_CAP_DMA);
    uint8_t *rbuf = heap_caps_malloc(bytes, MALLOC_CAP_DMA);
    uint8_t exp[512];
    if (!wbuf || !rbuf) {
        if (wbuf) heap_caps_free(wbuf);
        if (rbuf) heap_caps_free(rbuf);
        return ESP_ERR_NO_MEM;
    }

    uint64_t total = h->card.csd.capacity;              // sectors
    res->total_sectors = total;
    uint64_t nchunks = (total + chunk - 1) / chunk;

    // ---- Phase 1: WRITE the whole card -----------------------------------
    int consec = 0, last_pct = -1;
    int64_t t0 = esp_timer_get_time();
    for (uint64_t ci = 0; ci < nchunks; ci++) {
        uint32_t lba = (uint32_t)(ci * chunk);
        uint32_t cnt = chunk;
        if ((uint64_t)lba + cnt > total) cnt = (uint32_t)(total - lba);
        for (uint32_t s = 0; s < cnt; s++) fill_sector(wbuf + s * 512, lba + s);

        if (sd_hal_write_blocks(h, lba, wbuf, cnt) != ESP_OK) {
            res->write_failures++;
            if (res->first_write_fail_lba == 0xFFFFFFFF) res->first_write_fail_lba = lba;
            if (++consec >= ABORT_AFTER_CONSEC_FAILS) { res->write_aborted = true; break; }
        } else {
            consec = 0;
            res->sectors_written += cnt;
        }
        if (progress) {
            int pct = (int)((ci + 1) * 50 / nchunks);   // 0..50
            if (pct != last_pct) { progress(pct); last_pct = pct; }
        }
    }
    int64_t t1 = esp_timer_get_time();
    double wsecs = (t1 - t0) / 1e6;
    if (wsecs > 0 && res->sectors_written)
        res->write_mbps = res->sectors_written * 512.0 / (wsecs * 1e6);

    // ---- Phase 2: READ BACK and verify -----------------------------------
    consec = 0;
    int64_t r0 = esp_timer_get_time();
    for (uint64_t ci = 0; ci < nchunks; ci++) {
        uint32_t lba = (uint32_t)(ci * chunk);
        uint32_t cnt = chunk;
        if ((uint64_t)lba + cnt > total) cnt = (uint32_t)(total - lba);

        if (sd_hal_read_blocks(h, lba, rbuf, cnt) != ESP_OK) {
            res->read_failures++;
            if (res->first_verify_fail_lba == 0xFFFFFFFF) res->first_verify_fail_lba = lba;
            if (++consec >= ABORT_AFTER_CONSEC_FAILS) { res->verify_aborted = true; break; }
        } else {
            consec = 0;
            for (uint32_t s = 0; s < cnt; s++) {
                uint32_t this_lba = lba + s;
                uint8_t *got = rbuf + s * 512;
                fill_sector(exp, this_lba);
                if (memcmp(got, exp, 512) != 0) {
                    res->verify_mismatches++;
                    if (res->first_verify_fail_lba == 0xFFFFFFFF)
                        res->first_verify_fail_lba = this_lba;
                    uint32_t tag;
                    memcpy(&tag, got, 4);
                    if (tag != this_lba && res->wrap_detected_lba == 0xFFFFFFFF) {
                        res->wrap_detected_lba = this_lba;
                        res->wrap_seen_lba = tag;
                    }
                } else {
                    res->sectors_verified++;
                }
            }
        }
        if (progress) {
            int pct = 50 + (int)((ci + 1) * 50 / nchunks);   // 50..100
            if (pct != last_pct) { progress(pct); last_pct = pct; }
        }
    }
    int64_t r1 = esp_timer_get_time();
    double rsecs = (r1 - r0) / 1e6;
    if (rsecs > 0 && res->sectors_verified)
        res->read_mbps = res->sectors_verified * 512.0 / (rsecs * 1e6);

    heap_caps_free(wbuf);
    heap_caps_free(rbuf);
    return ESP_OK;
}

void report_write_human(const write_result_t *r)
{
    printf("\n=== DESTRUCTIVE WRITE / VERIFY RESULT ===\n");
    printf("  Total sectors     : %llu  (%.2f GB)\n",
           (unsigned long long)r->total_sectors,
           r->total_sectors * 512.0 / 1e9);
    printf("  Written ok        : %llu sectors  @ %.2f MB/s%s\n",
           (unsigned long long)r->sectors_written, r->write_mbps,
           r->write_aborted ? "  (WRITE ABORTED — card wedged)" : "");
    printf("  Verified ok       : %llu sectors  @ %.2f MB/s%s\n",
           (unsigned long long)r->sectors_verified, r->read_mbps,
           r->verify_aborted ? "  (VERIFY ABORTED — card wedged)" : "");
    printf("  Write I/O errors  : %lu\n", (unsigned long)r->write_failures);
    printf("  Read  I/O errors  : %lu\n", (unsigned long)r->read_failures);
    printf("  Verify mismatches : %lu\n", (unsigned long)r->verify_mismatches);
    if (r->first_write_fail_lba != 0xFFFFFFFF)
        printf("  First write fail  : LBA %lu\n", (unsigned long)r->first_write_fail_lba);
    if (r->first_verify_fail_lba != 0xFFFFFFFF)
        printf("  First verify fail : LBA %lu\n", (unsigned long)r->first_verify_fail_lba);
    if (r->wrap_detected_lba != 0xFFFFFFFF) {
        printf("  *** CAPACITY FRAUD: LBA %lu read back tagged as LBA %lu ***\n",
               (unsigned long)r->wrap_detected_lba, (unsigned long)r->wrap_seen_lba);
    }

    bool clean = !r->write_aborted && !r->verify_aborted &&
                 r->write_failures == 0 && r->read_failures == 0 &&
                 r->verify_mismatches == 0 &&
                 r->sectors_verified == r->total_sectors;
    printf("\n  VERDICT: %s\n", clean
        ? "PASS — full capacity is real and every sector retains written data."
        : (r->wrap_detected_lba != 0xFFFFFFFF
            ? "FAIL — FAKE CAPACITY (wrap detected); usable size is smaller than reported."
            : "FAIL — write/verify errors; card is unreliable for data storage."));
}

#endif // CONFIG_SDDIAG_ALLOW_DESTRUCTIVE
