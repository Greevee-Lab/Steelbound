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

int g_sba_us = 0;
void (*g_sba_fatal)(const char *msg) = NULL;

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
}

int sb_profile_id(void)
{
    return g_sba_us ? SB_PROFILE_US : SB_PROFILE_EU;
}

const char *sb_profile_name(void)
{
    return g_sba_us ? "US (NTSC-U, experimentell)" : "EU (PAL)";
}

unsigned sb_profile_text_entries(void)
{
    return (unsigned)(sizeof k_us_text / sizeof k_us_text[0]);
}

unsigned sb_profile_check_entries(void)
{
    return (unsigned)(sizeof k_us_check / sizeof k_us_check[0]);
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

int sb_profile_verify(char *msg, unsigned cap)
{
    unsigned i, bad = 0;
    if (msg && cap) {
        msg[0] = 0;
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
