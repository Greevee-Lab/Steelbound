/* sb_gamever.c - W150 ISO-DIRECT: Spielversion erkennen (siehe sb_gamever.h). */
#include "sb_gamever.h"
#include "sb_sha256.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const char *sha256;
    uint32_t title_id;
    uint32_t region;
    const char *label;
} known_t;

/* Gepruefte Fassungen (Patch-Adressen des Clients gelten genau hierfuer). */
static const known_t k_known[] = {
    { "a8285ca55dee8e770ef180f22665981101b3818e7e00858e60daa766b8b2e8f7",
      0x43430002u, 4u, "Steel Battalion (Europe, PAL)" },
};

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

const char *sb_gamever_region_name(uint32_t r)
{
    if (r == 4u) return "PAL";
    if (r == 1u) return "NTSC-U";
    if (r == 2u) return "NTSC-J";
    return "unknown/multi-region";
}

int sb_gamever_check(const uint8_t *xbe, size_t n, sb_gamever_t *out)
{
    uint8_t d[32];
    unsigned i;
    memset(out, 0, sizeof *out);
    if (!xbe || n < 0x200 || rd32(xbe) != 0x48454258u) { /* 'XBEH' */
        _snprintf_s(out->text, sizeof out->text, _TRUNCATE,
                    "GAMEVER: keine gueltige default.xbe (XBEH fehlt, %u B)", (unsigned)n);
        return 0;
    }
    sb_sha256(xbe, n, d);
    sb_sha256_hex(d, out->sha256);
    {
        uint32_t base = rd32(xbe + 0x104), ca = rd32(xbe + 0x118);
        if (ca >= base && (size_t)(ca - base) <= n - 0xB0) {
            const uint8_t *c = xbe + (ca - base);
            out->title_id = rd32(c + 0x08);
            out->region = rd32(c + 0xA0);
            out->version = rd32(c + 0xAC);
            out->xbe_ok = 1;
        }
    }
    for (i = 0; i < sizeof k_known / sizeof k_known[0]; i++) {
        if (strcmp(out->sha256, k_known[i].sha256) == 0 && out->xbe_ok &&
            out->title_id == k_known[i].title_id && out->region == k_known[i].region) {
            out->supported = 1;
            strcpy_s(out->label, sizeof out->label, k_known[i].label);
        }
    }
    if (out->supported) {
        _snprintf_s(out->text, sizeof out->text, _TRUNCATE,
                    "GAMEVER: %s erkannt (Title-ID %08X, Region %s, Version %u, sha256 %.16s...) -> unterstuetzt",
                    out->label, out->title_id, sb_gamever_region_name(out->region), out->version, out->sha256);
    } else if (out->xbe_ok && out->title_id == 0x43430002u) {
        _snprintf_s(out->text, sizeof out->text, _TRUNCATE,
                    "GAMEVER: andere Fassung von Steel Battalion (Title-ID %08X, Region %s, Version %u, sha256 %s) -> nicht unterstuetzt (nur PAL/Europe)",
                    out->title_id, sb_gamever_region_name(out->region), out->version, out->sha256);
    } else {
        _snprintf_s(out->text, sizeof out->text, _TRUNCATE,
                    "GAMEVER: fremdes Spiel (Title-ID %08X, Region %s, sha256 %s) -> nicht unterstuetzt",
                    out->title_id, sb_gamever_region_name(out->region), out->sha256);
    }
    return out->supported;
}
