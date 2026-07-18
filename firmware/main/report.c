// report.c — serial output (human + JSON). No display facility used.

#include "report.h"
#include "sd_mfg_db.h"
#include "config.h"
#include <stdio.h>
#include <string.h>
#include "esp_err.h"

static void hex_block(const char *label, const uint8_t *b, int n, bool present)
{
    printf("  %-6s ", label);
    if (!present) { printf("(unavailable)\n"); return; }
    for (int i = 0; i < n; i++) printf("%02x", b[i]);
    printf("\n");
}

static const char *speed_class_str(uint8_t c)
{
    switch (c) {
        case 0: return "Class 0";
        case 1: return "Class 2";
        case 2: return "Class 4";
        case 3: return "Class 6";
        case 4: return "Class 10";
        default: return "?";
    }
}

void report_identity_human(const sd_hal_t *h, const sd_decoded_t *d)
{
    const sd_raw_t *raw = &h->raw;
    printf("\n=== IDENTITY (raw registers, MSB-first, matches Linux sysfs) ===\n");
    hex_block("CID", raw->cid, 16, raw->has_cid);
    hex_block("CSD", raw->csd, 16, raw->has_csd);
    hex_block("SCR", raw->scr, 8, raw->has_scr);
    hex_block("OCR", raw->ocr, 4, raw->has_ocr);
    hex_block("SSR", raw->ssr, 64, raw->has_ssr);

    printf("\n=== IDENTITY (decoded) ===\n");
    if (raw->has_cid) {
        const sd_cid_t *c = &d->cid;
        const char *mfg = sd_mfg_name(c->mid);
        printf("  Manufacturer (MID): 0x%02x", c->mid);
        if (mfg) printf("  %s  [confidence: %s]", mfg, sd_mfg_confidence(c->mid));
        else     printf("  <unknown>");
        printf("\n");
        printf("  OEM/App ID (OID)  : \"%s\"\n", c->oid);
        printf("  Product (PNM)     : \"%s\"\n", c->pnm);
        printf("  Revision (PRV)    : %u.%u\n", c->prv_major, c->prv_minor);
        printf("  Serial (PSN)      : 0x%08lx\n", (unsigned long)c->psn);
        printf("  Mfg date (MDT)    : %04u-%02u\n", c->mdt_year, c->mdt_month);
        if (!sd_mfg_oid_consistent(c->mid, c->oid)) {
            printf("  ** COUNTERFEIT WARNING: OID \"%s\" != typical \"%s\" for MID 0x%02x **\n",
                   c->oid, sd_mfg_typical_oid(c->mid), c->mid);
        }
    }
    if (raw->has_csd) {
        const sd_csd_t *c = &d->csd;
        printf("  CSD version       : v%u (%s)\n", c->structure + 1,
               c->structure == 0 ? "SDSC" : c->structure == 1 ? "SDHC/SDXC" : "SDUC");
        printf("  Capacity          : %llu bytes  (%.2f GB, %llu sectors)\n",
               (unsigned long long)c->capacity_bytes,
               c->capacity_bytes / 1e9,
               (unsigned long long)c->capacity_sectors);
        printf("  TRAN_SPEED        : %.1f MHz (raw 0x%02x)\n",
               c->tran_speed_mhz, c->tran_speed_raw);
        printf("  CCC (cmd classes) : 0x%03x\n", c->ccc);
        printf("  Block len r/w     : %u / %u bytes\n",
               1u << c->read_bl_len, 1u << c->write_bl_len);
        printf("  Write protect     : perm=%d tmp=%d grp_en=%d\n",
               c->perm_write_protect, c->tmp_write_protect, c->wp_grp_enable);
    }
    printf("  Negotiated bus    : %s, %d-bit, %d kHz\n",
           h->backend, h->bus_width, h->freq_khz);

    // Cross-check against IDF's own decode (authoritative) to validate parsing.
    printf("  [IDF decode] mfg=0x%02x oem=0x%04x name=\"%.7s\" rev=0x%x "
           "serial=0x%08x date=0x%x cap=%d sec\n",
           h->card.cid.mfg_id, h->card.cid.oem_id, h->card.cid.name,
           h->card.cid.revision, h->card.cid.serial, h->card.cid.date,
           h->card.csd.capacity);
}

void report_caps_human(const sd_decoded_t *d)
{
    printf("\n=== CAPABILITIES (SCR / OCR / SSR / CMD6) ===\n");
    const sd_scr_t *s = &d->scr;
    printf("  SD spec version   : %s  (SCR struct %u)\n",
           s->phys_version ? s->phys_version : "?", s->scr_structure);
    printf("  Bus widths        : %s%s\n",
           (s->bus_widths & 1) ? "1-bit " : "",
           (s->bus_widths & 4) ? "4-bit" : "");
    printf("  CMD support       : CMD23=%d CMD20=%d\n",
           !!(s->cmd_support & 1), !!(s->cmd_support & 2));

    const sd_ocr_t *o = &d->ocr;
    printf("  OCR               : CCS(SDHC/XC)=%d  S18A(1.8V)=%d  UHS-II=%d\n",
           o->ccs, o->s18a, o->uhs_ii);

    const sd_ssr_t *ss = &d->ssr;
    printf("  Speed class       : %s\n", speed_class_str(ss->speed_class));
    printf("  UHS speed grade   : U%u\n", ss->uhs_speed_grade);
    printf("  Video speed class : V%u\n", ss->video_speed_class);
    printf("  App perf class    : %s\n",
           ss->app_perf_class == 1 ? "A1" : ss->app_perf_class == 2 ? "A2" : "none");
    printf("  AU size           : %lu bytes (code %u)\n",
           (unsigned long)ss->au_size_bytes, ss->au_size_code);
    printf("  DAT bus width     : %s\n", ss->dat_bus_width == 2 ? "4-bit" : "1-bit");

    const sd_cmd6_t *c6 = &d->cmd6;
    printf("  Access modes      : %s%s%s%s%s\n",
           c6->sdr12  ? "SDR12 "  : "",
           c6->sdr25  ? "SDR25 "  : "",
           c6->sdr50  ? "SDR50 "  : "",
           c6->sdr104 ? "SDR104 " : "",
           c6->ddr50  ? "DDR50"   : "");
    printf("  Max current       : %u mA\n", c6->max_current_ma);
}

void report_scan_human(const scan_result_t *s)
{
    printf("\n=== SURFACE READ SCAN (read-only) ===\n");
    if (s->start_lba)
        printf("  Range             : LBA %lu + %llu sectors\n",
               (unsigned long)s->start_lba,
               (unsigned long long)s->total_sectors);
    else
        printf("  Total sectors     : %llu\n",
               (unsigned long long)s->total_sectors);
    printf("  Chunks ok/failed  : %lu / %lu  (of %lu)\n",
           (unsigned long)s->chunks_ok, (unsigned long)s->chunks_failed,
           (unsigned long)s->chunks_total);
    printf("  Throughput MB/s   : min %.2f  median %.2f  max %.2f\n",
           s->min_mbps, s->median_mbps, s->max_mbps);
    printf("  Slow regions (<median/5): %lu\n", (unsigned long)s->slow_regions);
    printf("  CMD13 error events: %lu\n", (unsigned long)s->cmd13_error_events);
    if (s->first_bad_lba != 0xFFFFFFFF)
        printf("  ** First bad LBA  : %lu **\n", (unsigned long)s->first_bad_lba);
    else
        printf("  No read failures.\n");
    if (s->aborted)
        printf("  ** SCAN ABORTED   : card stopped responding (long run of "
               "read timeouts) — strong sign of a failed flash array. **\n");
    if (s->stopped)
        printf("  ** STOPPED by user at LBA %lu — resume with: scan %lu **\n",
               (unsigned long)s->next_lba, (unsigned long)s->next_lba);
}

void report_bench_human(const bench_result_t *b)
{
    printf("\n=== READ BENCHMARK (read-only) ===\n");
    if (b->seq_ok)
        printf("  Sequential read   : %.2f MB/s\n", b->seq_read_mbps);
    else
        printf("  Sequential read   : FAILED at LBA %lu (partial %.2f MB/s)\n",
               (unsigned long)b->seq_fail_lba, b->seq_read_mbps);
    printf("  Random 4KiB read  : %.0f IOPS  (%lu/%lu ops failed)\n",
           b->rand_read_iops, (unsigned long)b->rand_fail,
           (unsigned long)b->rand_ops);
    if (b->aborted)
        printf("  ** BENCH ABORTED  : card stopped responding — failed card. **\n");
}

void report_probe_human(const probe_result_t *p)
{
    printf("\n=== QUICK HEALTH PROBE ===\n");
    for (int i = 0; i < DIAG_PROBE_INIT_CYCLES; i++) {
        if (p->init_err[i] == ESP_OK)
            printf("  Bring-up #%d      : OK    (%lu ms)\n",
                   i + 1, (unsigned long)p->init_ms[i]);
        else
            printf("  Bring-up #%d      : FAIL  %s (%lu ms)\n",
                   i + 1, esp_err_to_name(p->init_err[i]),
                   (unsigned long)p->init_ms[i]);
    }
    if (!p->card_usable) {
        printf("  VERDICT          : ** CARD IS BAD — cannot complete bring-up **\n");
        return;
    }
    for (int i = 0; i < p->points; i++) {
        if (p->err[i] == ESP_OK)
            printf("  LBA %10lu   : OK    (%lu ms)\n",
                   (unsigned long)p->lba[i], (unsigned long)p->ms[i]);
        else
            printf("  LBA %10lu   : FAIL  %s (%lu ms)\n",
                   (unsigned long)p->lba[i], esp_err_to_name(p->err[i]),
                   (unsigned long)p->ms[i]);
    }
    if (p->burst_ok)
        printf("  Burst read       : OK    %.2f MB/s over %lu KiB\n",
               p->burst_mbps, (unsigned long)p->burst_kb);
    else
        printf("  Burst read       : FAIL  at LBA %lu (%lu KiB read, %.2f MB/s)\n",
               (unsigned long)p->burst_fail_lba,
               (unsigned long)p->burst_kb, p->burst_mbps);
    if (p->status_ok)
        printf("  CMD13            : 0x%08lx, %d error flag(s)\n",
               (unsigned long)p->status, p->status_errs);
    else
        printf("  CMD13            : no answer\n");

    bool bad = p->init_fails || p->fails || !p->burst_ok ||
               p->status_errs || !p->status_ok;
    if (!bad) {
        printf("  VERDICT          : card passes (bring-up stable, reads OK)\n");
    } else {
        printf("  VERDICT          : ** CARD IS BAD —%s%s%s **\n",
               p->init_fails ? " intermittent bring-up" : "",
               (p->fails || !p->burst_ok) ? " read failures" : "",
               (p->status_errs || !p->status_ok) ? " status errors" : "");
    }
}

void report_init_failure_human(const sd_hal_t *h)
{
    printf("\n=== CARD BRING-UP FAILED ===\n");
    printf("  Backend           : %s\n", h->backend ? h->backend : "?");
    printf("  Failed stage      : %s\n", h->last_err_stage);
    printf("  Error             : %s (0x%x)\n",
           esp_err_to_name(h->last_err), h->last_err);
    printf("  Pins (SPI)        : CS=%d MOSI=%d MISO=%d SCK=%d\n",
           SDDIAG_PIN_CS, SDDIAG_PIN_MOSI, SDDIAG_PIN_MISO, SDDIAG_PIN_SCK);
    printf("  Interpretation    :\n");
    if (h->last_err == ESP_ERR_TIMEOUT) {
        // The IDF SDIO probe (cmd 52/5) replied, so the card is present and
        // electrically responsive — the timeout is the ACMD41 (send_op_cond)
        // power-up handshake never clearing busy.
        printf("    The card ANSWERS commands (it is present and electrically\n"
               "    alive) but never completes power-up: ACMD41 (OCR busy bit)\n"
               "    stayed busy until timeout. This is the classic signature of a\n"
               "    FAILED card whose controller cannot bring its flash array up.\n"
               "    It is often intermittent — a cold power-cycle may enumerate\n"
               "    once, then data reads time out. Retry with `reinit`.\n");
    } else if (h->last_err == ESP_ERR_NOT_FOUND) {
        printf("    No card detected on the bus. Check insertion and wiring.\n");
    } else {
        printf("    Card responded but bring-up failed at the stage above; the\n"
               "    controller may be partially failed. Some raw registers may\n"
               "    still be readable on a retry (`reinit`).\n");
    }
}

// --------------------------------------------------------------------------
// JSON
// --------------------------------------------------------------------------
static void json_hex(const char *key, const uint8_t *b, int n, bool present, bool comma)
{
    printf("    \"%s\": ", key);
    if (!present) { printf("null%s\n", comma ? "," : ""); return; }
    printf("\"");
    for (int i = 0; i < n; i++) printf("%02x", b[i]);
    printf("\"%s\n", comma ? "," : "");
}

void report_json(const sd_hal_t *h, const sd_decoded_t *d)
{
    const sd_raw_t *raw = &h->raw;
    const sd_cid_t *c = &d->cid;
    const sd_csd_t *cs = &d->csd;
    const sd_scr_t *s = &d->scr;
    const sd_ssr_t *ss = &d->ssr;
    const sd_ocr_t *o = &d->ocr;
    const sd_cmd6_t *c6 = &d->cmd6;
    const char *mfg = sd_mfg_name(c->mid);

    printf("{\n");
    printf("  \"raw\": {\n");
    json_hex("cid", raw->cid, 16, raw->has_cid, true);
    json_hex("csd", raw->csd, 16, raw->has_csd, true);
    json_hex("scr", raw->scr, 8, raw->has_scr, true);
    json_hex("ocr", raw->ocr, 4, raw->has_ocr, true);
    json_hex("ssr", raw->ssr, 64, raw->has_ssr, false);
    printf("  },\n");

    printf("  \"cid\": {\"mid\": %u, \"mfg\": %s%s%s, \"oid\": \"%s\", \"pnm\": \"%s\", "
           "\"prv\": \"%u.%u\", \"psn\": %lu, \"mdt\": \"%04u-%02u\"},\n",
           c->mid,
           mfg ? "\"" : "", mfg ? mfg : "null", mfg ? "\"" : "",
           c->oid, c->pnm, c->prv_major, c->prv_minor,
           (unsigned long)c->psn, c->mdt_year, c->mdt_month);

    printf("  \"csd\": {\"structure\": %u, \"capacity_bytes\": %llu, "
           "\"capacity_sectors\": %llu, \"tran_speed_mhz\": %.1f, \"ccc\": %u},\n",
           cs->structure, (unsigned long long)cs->capacity_bytes,
           (unsigned long long)cs->capacity_sectors, cs->tran_speed_mhz, cs->ccc);

    printf("  \"scr\": {\"spec_version\": \"%s\", \"bus_widths\": %u, \"cmd_support\": %u},\n",
           s->phys_version ? s->phys_version : "", s->bus_widths, s->cmd_support);

    printf("  \"ocr\": {\"ccs\": %d, \"s18a\": %d, \"uhs_ii\": %d},\n",
           o->ccs, o->s18a, o->uhs_ii);

    printf("  \"ssr\": {\"speed_class\": %u, \"uhs_grade\": %u, \"video_class\": %u, "
           "\"app_class\": %u, \"au_size_bytes\": %lu, \"dat_bus_width\": %u},\n",
           ss->speed_class, ss->uhs_speed_grade, ss->video_speed_class,
           ss->app_perf_class, (unsigned long)ss->au_size_bytes, ss->dat_bus_width);

    printf("  \"cmd6\": {\"sdr12\": %d, \"sdr25\": %d, \"sdr50\": %d, \"sdr104\": %d, "
           "\"ddr50\": %d, \"max_current_ma\": %u},\n",
           c6->sdr12, c6->sdr25, c6->sdr50, c6->sdr104, c6->ddr50, c6->max_current_ma);

    printf("  \"bus\": {\"backend\": \"%s\", \"width\": %d, \"freq_khz\": %d}\n",
           h->backend, h->bus_width, h->freq_khz);
    printf("}\n");
}
