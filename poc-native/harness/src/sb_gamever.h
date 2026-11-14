#pragma once
/* sb_gamever.h - W150 ISO-DIRECT: Spielversion aus der default.xbe erkennen.
 *
 * SHA-256 der ganzen default.xbe plus Title-ID, Region und Version aus dem
 * XBE-Zertifikat. Unterstuetzt ist die PAL-Fassung (Europe), fuer die alle
 * Patch-Adressen des Clients gelten. Gemeinsam fuer Client und Launcher.
 * Enthaelt nur den Hash (keine Spielbytes). */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int supported;        /* 1 = bekannte, unterstuetzte Fassung */
    int xbe_ok;           /* 1 = XBEH-Magic + Zertifikat lesbar */
    uint32_t title_id;
    uint32_t region;      /* Zertifikat: 1 = NA, 2 = Japan, 4 = Rest der Welt (PAL) */
    uint32_t version;
    int ntsc_u;           /* W229: 1 = bekannte NTSC-U-Fassung (USA); W230: supported=1 (experimentell, US-Profil) */
    char sha256[65];
    char label[64];       /* z. B. "Steel Battalion (Europe, PAL)" oder "" */
    char text[320];       /* Log-Zeile (deutsch, unuebersetzt; Launcher nutzt Schluessel) */
} sb_gamever_t;

/* Region masks are permissions, not proof of a particular release. */
const char *sb_gamever_region_name(uint32_t region);

/* 1 = unterstuetzt, 0 = nicht (out->text erklaert). */
int sb_gamever_check(const uint8_t *xbe, size_t n, sb_gamever_t *out);

#ifdef __cplusplus
}
#endif
