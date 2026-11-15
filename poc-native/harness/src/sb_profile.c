/* sb_profile.c - W229 US-PROFILE: Adressabbildung je Spielfassung (siehe sb_profile.h). */
#include "sb_profile.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

typedef struct { uint32_t eu, us; } sb_text_map_t;
typedef struct { uint32_t us, fnv; } sb_check_t;

/* Generiert von tools/region/map_us.py --emit-c (nicht von Hand editieren):
 * k_us_text[] (sortiert nach eu), k_us_check[], SB_US_* Konstanten. */
#include "sb_profile_us.inc"
/* W275 LOC-BOOT: generiert von tools/loc/gen_loc_profile.py:
 * k_loc_map[] (sortiert nach SB-Adresse), k_loc_check[]. */
#include "loc_profile.inc"

int g_sba_us = 0;
int g_sba_loc = 0;
int g_sba_map = 0;
void (*g_sba_fatal)(const char *msg) = NULL;
void (*g_sba_loc_miss)(uint32_t eu_va) = NULL;

/* W275: Null-Seite fuer SB-Adressen ohne LoC-Gegenstueck. Ziel =
 * Basis + 0x4000 + (eu & 0xFFFF): Rand fuer kleine Indizes/Offsets nach
 * oben und unten (z. B. SBA(x) + i * 0x154). Nur Daten, nie Code. */
static __declspec(align(4096)) uint8_t g_loc_null[0x18000];
static volatile LONG g_loc_miss_n;

static int loc_find(uint32_t va, uint32_t *out)
{
    unsigned lo = 0, hi = sizeof k_loc_map / sizeof k_loc_map[0];
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2u;
        if (k_loc_map[mid].eu == va) {
            if (out) {
                *out = k_loc_map[mid].us;
            }
            return 1;
        }
        if (k_loc_map[mid].eu < va) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return 0;
}

static uint32_t sba_loc_map(uint32_t va)
{
    uint32_t t;
    if (va < 0x10000u || va >= 0x2800000u) {
        return va; /* ausserhalb des Abbilds (Konstanten, Host-Zeiger) */
    }
    if (loc_find(va, &t)) {
        return t;
    }
    InterlockedIncrement(&g_loc_miss_n);
    if (g_sba_loc_miss) {
        g_sba_loc_miss(va);
    }
    return (uint32_t)(uintptr_t)(g_loc_null + 0x4000u + (va & 0xFFFFu));
}

int sb_profile_loc_has(uint32_t eu_va)
{
    return g_sba_loc && loc_find(eu_va, NULL);
}

unsigned sb_profile_loc_misses(void)
{
    return (unsigned)g_loc_miss_n;
}

void sb_profile_select_loc(void)
{
    g_sba_loc = 1;
    g_sba_map = 1;
}

static void sba_die(const char *msg)
{
    if (g_sba_fatal) {
        g_sba_fatal(msg);
    }
    OutputDebugStringA(msg);
    ExitProcess(8);
}

uint32_t sba_us_map(uint32_t va)
{
    if (g_sba_loc) {
        return sba_loc_map(va);
    }
    if (va >= SB_US_TEXT_LO && va < SB_US_TEXT_HI) {
        unsigned lo = 0, hi = sizeof k_us_text / sizeof k_us_text[0];
        while (lo < hi) {
            unsigned mid = (lo + hi) / 2u;
            if (k_us_text[mid].eu == va) {
                return k_us_text[mid].us;
            }
            if (k_us_text[mid].eu < va) {
                lo = mid + 1u;
            } else {
                hi = mid;
            }
        }
        {
            char msg[160];
            _snprintf_s(msg, sizeof msg, _TRUNCATE,
                        "PROFILE: EU-.text-Adresse 0x%X ohne US-Eintrag - ABBRUCH (exit 8)", va);
            sba_die(msg);
        }
        return va;
    }
    if (va >= SB_US_DATA_LO && va < SB_US_DATA_HI) {
        return va + SB_US_DATA_DELTA;
    }
    return va; /* XBE-Kopf und alles ausserhalb des Abbilds */
}

void sb_profile_select_us(void)
{
    g_sba_us = 1;
    g_sba_map = 1;
}

int sb_profile_id(void)
{
    return g_sba_loc ? SB_PROFILE_LOC : g_sba_us ? SB_PROFILE_US : SB_PROFILE_EU;
}

const char *sb_profile_name(void)
{
    return g_sba_loc ? "LoC (Line of Contact, experimentell)"
                     : g_sba_us ? "US (NTSC-U, experimentell)" : "EU (PAL)";
}

unsigned sb_profile_text_entries(void)
{
    return g_sba_loc ? (unsigned)(sizeof k_loc_map / sizeof k_loc_map[0])
                     : (unsigned)(sizeof k_us_text / sizeof k_us_text[0]);
}

unsigned sb_profile_check_entries(void)
{
    return g_sba_loc ? (unsigned)(sizeof k_loc_check / sizeof k_loc_check[0])
                     : (unsigned)(sizeof k_us_check / sizeof k_us_check[0]);
}

static uint32_t fnv8(const uint8_t *p)
{
    uint32_t h = 2166136261u;
    unsigned i;
    for (i = 0; i < SB_US_CHECK_LEN; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

static int profile_verify_tab(const sb_check_t *tab, unsigned n, char *msg, unsigned cap)
{
    unsigned i, bad = 0;
    for (i = 0; i < n; i++) {
        uint32_t h = 0;
        int ok = 0;
        __try {
            h = fnv8((const uint8_t *)(uintptr_t)tab[i].us);
            ok = 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = 0;
        }
        if (!ok || h != tab[i].fnv) {
            if (!bad && msg && cap) {
                _snprintf_s(msg, cap, _TRUNCATE,
                            "PROFILE: Erstbytes @0x%X passen nicht (fnv %08X statt %08X)",
                            tab[i].us, h, tab[i].fnv);
            }
            bad++;
        }
    }
    return (int)bad;
}

int sb_profile_verify(char *msg, unsigned cap)
{
    unsigned i, bad = 0;
    if (msg && cap) {
        msg[0] = 0;
    }
    if (g_sba_loc) {
        return profile_verify_tab(k_loc_check, (unsigned)(sizeof k_loc_check / sizeof k_loc_check[0]), msg, cap);
    }
    if (!g_sba_us) {
        return 0;
    }
    for (i = 0; i < sizeof k_us_check / sizeof k_us_check[0]; i++) {
        uint32_t h = 0;
        int ok = 0;
        __try {
            h = fnv8((const uint8_t *)(uintptr_t)k_us_check[i].us);
            ok = 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = 0;
        }
        if (!ok || h != k_us_check[i].fnv) {
            if (!bad && msg && cap) {
                _snprintf_s(msg, cap, _TRUNCATE,
                            "PROFILE: Erstbytes @0x%X passen nicht (fnv %08X statt %08X)",
                            k_us_check[i].us, h, k_us_check[i].fnv);
            }
            bad++;
        }
    }
    return (int)bad;
}
