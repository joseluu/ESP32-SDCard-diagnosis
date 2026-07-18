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
                            diag_progress_fn progress)
{
    if (!h->initialized) return ESP_ERR_INVALID_STATE;
    memset(res, 0, sizeof(*res));
    res->first_write_fail_lba  = 0xFFFFFFFF;
    res->first_verify_fail_lba = 0xFFFFFFFF;
    res->wrap_detected_lba     = 0xFFFFFFFF;
    diag_stop_clear();

    // Two buffers; halve the chunk if DMA heap is tight (the LVGL UI uses some).
    uint32_t chunk = SDDIAG_SCAN_CHUNK_SECTORS;         // 128 sectors / 64 KiB
    uint8_t *wbuf = NULL, *rbuf = NULL;
    for (; chunk >= 16; chunk /= 2) {
        wbuf = heap_caps_malloc(chunk * 512, MALLOC_CAP_DMA);
        rbuf = heap_caps_malloc(chunk * 512, MALLOC_CAP_DMA);
        if (wbuf && rbuf) break;
        heap_caps_free(wbuf); wbuf = NULL;
        heap_caps_free(rbuf); rbuf = NULL;
    }
    uint8_t exp[512];
    if (!wbuf || !rbuf) {
        heap_caps_free(wbuf);
        heap_caps_free(rbuf);
        return ESP_ERR_NO_MEM;
    }

    uint64_t total = h->card.csd.capacity;              // sectors
    res->total_sectors = total;
    uint64_t nchunks = (total + chunk - 1) / chunk;

    // ---- Phase 1: WRITE the whole card -----------------------------------
    int consec = 0;
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
        if (progress) progress((int)((ci + 1) * 50 / nchunks), lba);  // 0..50
        if (diag_stop_requested()) { res->stopped = true; break; }
    }
    int64_t t1 = esp_timer_get_time();
    double wsecs = (t1 - t0) / 1e6;
    if (wsecs > 0 && res->sectors_written)
        res->write_mbps = res->sectors_written * 512.0 / (wsecs * 1e6);

    // ---- Phase 2: READ BACK and verify -----------------------------------
    consec = 0;
    int64_t r0 = esp_timer_get_time();
    for (uint64_t ci = 0; !res->stopped && ci < nchunks; ci++) {
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
        if (progress) progress(50 + (int)((ci + 1) * 50 / nchunks), lba);
        if (diag_stop_requested()) { res->stopped = true; break; }
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

    if (r->stopped)
        printf("  ** STOPPED by user — results are partial. **\n");

    bool clean = !r->stopped && !r->write_aborted && !r->verify_aborted &&
                 r->write_failures == 0 && r->read_failures == 0 &&
                 r->verify_mismatches == 0 &&
                 r->sectors_verified == r->total_sectors;
    printf("\n  VERDICT: %s\n", clean
        ? "PASS — full capacity is real and every sector retains written data."
        : (r->wrap_detected_lba != 0xFFFFFFFF
            ? "FAIL — FAKE CAPACITY (wrap detected); usable size is smaller than reported."
            : "FAIL — write/verify errors; card is unreliable for data storage."));
}

// ---------------------------------------------------------------------------
// Capacity check (f3probe-style backup/restore)
// ---------------------------------------------------------------------------

#define CAPCHK_MAGIC 0x53444347u    // "SDCG"

// 512-byte probe block: magic + self-LBA tag + PRNG body.
static void capchk_fill(uint8_t *p, uint32_t lba)
{
    uint32_t *w = (uint32_t *)p;
    uint32_t x = lba * 2246822519u + 0x3C6EF372u;
    for (int i = 0; i < 128; i++) {
        x = x * 1664525u + 1013904223u;
        w[i] = x;
    }
    w[0] = CAPCHK_MAGIC;
    w[1] = lba;
    w[2] = ~lba;
}

static bool capchk_tag(const uint8_t *p, uint32_t *lba_out)
{
    const uint32_t *w = (const uint32_t *)p;
    if (w[0] != CAPCHK_MAGIC || w[2] != ~w[1]) return false;
    *lba_out = w[1];
    return true;
}

static int capchk_add_unique(uint32_t *list, int n, uint32_t lba)
{
    for (int i = 0; i < n; i++)
        if (list[i] == lba) return n;
    list[n] = lba;
    return n + 1;
}

esp_err_t diag_capacity_check(sd_hal_t *h, capchk_result_t *res)
{
    if (!h->initialized) return ESP_ERR_INVALID_STATE;
    memset(res, 0, sizeof(*res));

    uint64_t total = h->card.csd.capacity;
    res->announced_sectors = total;
    if (total < (1ULL << CAPCHK_MIN_SHIFT)) return ESP_ERR_INVALID_SIZE;

    // Probe points: powers of two from 32 MiB up, plus the last sector.
    int n = 0;
    for (uint64_t p = 1ULL << CAPCHK_MIN_SHIFT;
         p < total && n < CAPCHK_MAX_POINTS - 1; p <<= 1)
        res->lba[n++] = (uint32_t)p;
    res->lba[n++] = (uint32_t)(total - 1);
    res->points = n;

    // Sectors to back up: sector 0, every probe, and every low sector a
    // wrapped write of the last probe could land on ((total-1) mod 2^k).
    uint32_t blist[2 * CAPCHK_MAX_POINTS + 2];
    int bn = 0;
    bn = capchk_add_unique(blist, bn, 0);
    for (int i = 0; i < n; i++) bn = capchk_add_unique(blist, bn, res->lba[i]);
    for (uint64_t m = 1ULL << CAPCHK_MIN_SHIFT; m < total; m <<= 1)
        bn = capchk_add_unique(blist, bn, (uint32_t)((total - 1) % m));

    // Sort ascending so the restore pass (descending) writes the true low
    // sectors last — on a wrapping card, aliased high restores land low and
    // are then overwritten by the direct low restores.
    for (int i = 1; i < bn; i++)
        for (int j = i; j > 0 && blist[j - 1] > blist[j]; j--) {
            uint32_t t = blist[j]; blist[j] = blist[j - 1]; blist[j - 1] = t;
        }

    uint8_t *backup = heap_caps_malloc(bn * 512, MALLOC_CAP_DMA);
    uint8_t *io = heap_caps_malloc(512, MALLOC_CAP_DMA);
    if (!backup || !io) {
        heap_caps_free(backup);
        heap_caps_free(io);
        return ESP_ERR_NO_MEM;
    }

    // 1) Backup. Nothing has been written yet, so a failure aborts harmlessly.
    for (int i = 0; i < bn; i++) {
        esp_err_t e = sd_hal_read_blocks(h, blist[i], backup + 512 * i, 1);
        if (e != ESP_OK) {
            heap_caps_free(backup);
            heap_caps_free(io);
            return e;
        }
    }

    // 2) Tag every probe, highest LBA first: on a wrapping card, sector 0
    //    ends up holding the tag of the smallest aliasing probe — which is
    //    the real size (for the usual power-of-two flash).
    for (int i = n - 1; i >= 0; i--) {
        capchk_fill(io, res->lba[i]);
        if (sd_hal_write_blocks(h, res->lba[i], io, 1) != ESP_OK)
            res->state[i] = CAPCHK_WRITE_ERR;
    }

    // 3) Reset the card before reading back: a fake card's controller cache
    //    (RAM) can serve the just-written sectors and mask missing flash; the
    //    cache does not survive a CMD0/full re-init, so reads below must come
    //    from actual NAND (same trick as f3probe's device reset).
    sd_hal_deinit(h);
    esp_err_t reinit_err = ESP_FAIL;
    for (int try = 0; try < 3 && reinit_err != ESP_OK; try++)
        reinit_err = sd_hal_init(h);
    if (reinit_err != ESP_OK) {
        // Card died mid-test: restore is impossible. Report that loudly.
        res->restore_ok = false;
        heap_caps_free(backup);
        heap_caps_free(io);
        return reinit_err;
    }

    // 4) Read back and classify each probe.
    for (int i = 0; i < n; i++) {
        if (res->state[i] == CAPCHK_WRITE_ERR) continue;
        if (sd_hal_read_blocks(h, res->lba[i], io, 1) != ESP_OK) {
            res->state[i] = CAPCHK_READ_ERR;
            continue;
        }
        uint32_t t;
        if (!capchk_tag(io, &t))       res->state[i] = CAPCHK_LOST;
        else if (t == res->lba[i])     res->state[i] = CAPCHK_OK;
        else { res->state[i] = CAPCHK_ALIAS; res->found_lba[i] = t; }
    }

    // 5) Wrap check: did some probe's tag land on sector 0?
    if (sd_hal_read_blocks(h, 0, io, 1) == ESP_OK) {
        uint32_t t;
        if (capchk_tag(io, &t) && t != 0) {
            res->wrap_detected = true;
            res->wrap_modulus = t;
        }
    }

    uint64_t est = res->wrap_detected ? res->wrap_modulus : 0;
    for (int i = 0; i < n; i++)
        if ((res->state[i] == CAPCHK_LOST || res->state[i] == CAPCHK_ALIAS) &&
            (!est || res->lba[i] < est))
            est = res->lba[i];
    res->est_real_sectors = est;

    // 6) Restore everything, highest LBA first (see the sort note above).
    res->restore_ok = true;
    for (int i = bn - 1; i >= 0; i--) {
        if (sd_hal_write_blocks(h, blist[i], backup + 512 * i, 1) != ESP_OK) {
            res->restore_ok = false;
            continue;
        }
        if (sd_hal_read_blocks(h, blist[i], io, 1) != ESP_OK ||
            memcmp(io, backup + 512 * i, 512) != 0)
            res->restore_ok = false;
    }

    heap_caps_free(backup);
    heap_caps_free(io);
    return ESP_OK;
}

void report_capchk_human(const capchk_result_t *r)
{
    static const char *kState[] = {
        "OK    (holds its data)",
        "ALIAS (wraps to a lower address)",
        "LOST  (write silently discarded)",
        "WRITE FAILED",
        "READ FAILED",
    };
    printf("\n=== CAPACITY CHECK (write+restore probe) ===\n");
    printf("  Announced       : %.2f GB (%llu sectors)\n",
           r->announced_sectors * 512.0 / 1e9,
           (unsigned long long)r->announced_sectors);
    for (int i = 0; i < r->points; i++) {
        printf("  LBA %10lu : %s", (unsigned long)r->lba[i],
               kState[r->state[i]]);
        if (r->state[i] == CAPCHK_ALIAS)
            printf(" [found tag of LBA %lu]", (unsigned long)r->found_lba[i]);
        printf("\n");
    }
    if (r->wrap_detected)
        printf("  Wrap-around     : tag of LBA %lu landed on sector 0\n",
               (unsigned long)r->wrap_modulus);
    if (r->est_real_sectors)
        printf("  VERDICT         : ** FAKE CAPACITY — real ~= %.2f GB, "
               "announced %.2f GB **\n",
               r->est_real_sectors * 512.0 / 1e9,
               r->announced_sectors * 512.0 / 1e9);
    else
        printf("  VERDICT         : all probe points hold data — capacity "
               "plausible\n                    (a large controller cache can "
               "mask this; wtest is definitive)\n");
    printf("  Restore         : %s\n", r->restore_ok
        ? "original content written back and verified"
        : "** INCOMPLETE — some saved sectors could not be restored **");
}

#endif // CONFIG_SDDIAG_ALLOW_DESTRUCTIVE
