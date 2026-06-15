// sd_mfg_db.c — manufacturer-ID database (SD_REFERENCE.md §8).
// Community-sourced; confidence is tracked and raw hex is always kept by the
// caller for unknowns. A printed-brand vs MID/OID mismatch is a counterfeit tell.

#include "sd_mfg_db.h"
#include <string.h>

typedef struct {
    uint8_t     mid;
    const char *name;
    const char *typical_oid;   // NULL if unknown / ambiguous
    const char *confidence;
} mfg_entry_t;

static const mfg_entry_t kDb[] = {
    { 0x01, "Panasonic",            "PA", "med"  },
    { 0x02, "Toshiba / Kioxia",     "TM", "high" },
    { 0x03, "SanDisk / WD",         "SD", "high" },
    { 0x09, "ATP",                  "AP", "med"  },
    { 0x13, "Micron",               NULL, "med"  },
    { 0x1b, "Samsung",              "SM", "high" },
    { 0x1d, "ADATA",                "AD", "med"  },
    { 0x27, "Phison",               "PH", "high" },
    { 0x28, "Lexar",                "BE", "high" },
    { 0x31, "Silicon Power",        NULL, "med"  },
    { 0x41, "Kingston",             "42", "high" },
    { 0x6f, "STEC",                 NULL, "low"  },
    { 0x74, "Transcend / PNY",      "JE", "med"  },
    { 0x76, "Patriot",              NULL, "low"  },
    { 0x82, "Sony / Gobe",          "JT", "med"  },
    { 0x9c, "Angelbird / Barun",    "BE", "med"  },
};
static const int kDbLen = sizeof(kDb) / sizeof(kDb[0]);

static const mfg_entry_t *find(uint8_t mid)
{
    for (int i = 0; i < kDbLen; i++)
        if (kDb[i].mid == mid) return &kDb[i];
    return NULL;
}

const char *sd_mfg_name(uint8_t mid)
{
    const mfg_entry_t *e = find(mid);
    return e ? e->name : NULL;
}

const char *sd_mfg_confidence(uint8_t mid)
{
    const mfg_entry_t *e = find(mid);
    return e ? e->confidence : "unknown";
}

const char *sd_mfg_typical_oid(uint8_t mid)
{
    const mfg_entry_t *e = find(mid);
    return e ? e->typical_oid : NULL;
}

bool sd_mfg_oid_consistent(uint8_t mid, const char oid[2])
{
    const mfg_entry_t *e = find(mid);
    if (!e || !e->typical_oid) return true;  // nothing to compare against
    return e->typical_oid[0] == oid[0] && e->typical_oid[1] == oid[1];
}
