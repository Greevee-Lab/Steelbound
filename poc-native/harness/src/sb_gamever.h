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
    int loc;              /* W275 LOC-BOOT: 1 = Line of Contact (World-Disc, Title-ID 43430009) erkannt
                           * (W378: 2 = dieselbe XBE unveraendert, ohne Media-Patch; gleiches Profil).
                           * supported bleibt 0 (SB-Profil gilt nicht); der Client startet LoC nur mit
                           * [expert] loc_boot=1 ueber das eigene LoC-Profil. */
    char sha256[65];
    char label[64];       /* z. B. "Steel Battalion (Europe, PAL)" oder "" */
    char text[320];       /* Log-Zeile (deutsch, unuebersetzt; Launcher nutzt Schluessel) */
} sb_gamever_t;

/* Region masks are permissions, not proof of a particular release. */
const char *sb_gamever_region_name(uint32_t region);

/* 1 = unterstuetzt, 0 = nicht (out->text erklaert). */
int sb_gamever_check(const uint8_t *xbe, size_t n, sb_gamever_t *out);

/* W275 LOC-BOOT: SHA-256 der bekannten LoC-default.xbe (World-Disc). */
#define SB_GAMEVER_LOC_SHA "696db4056941039934f22e9b8bcbc1407575b64749c14c6032e2a049b34d8957"
#define SB_GAMEVER_LOC_TITLE 0x43430009u
/* W378 LOC-IMAGE-VARIANT: dieselbe 1.0-XBE unveraendert (ohne Media-Patch). Sie
 * weicht von SB_GAMEVER_LOC_SHA nur in einem Sprungbyte der XAPI-Medienpruefung ab
 * (0x12E71E jge statt jmp); bei diesem Zertifikat kehrt die Pruefung ohne
 * Laufwerkszugriff mit Erfolg zurueck -> gleicher Ablauf, gleiches LoC-Profil. */
#define SB_GAMEVER_LOC_SHA_STOCK "b75faaf36947f967c33435ec64fa792f9f99b9bc40254cf91484fa13347e4117"
/* W378: 1 = SB_GAMEVER_LOC_SHA (Media-Patch), 2 = SB_GAMEVER_LOC_SHA_STOCK, 0 = unbekannt
 * (64 Hex, Gross/Klein egal). */
int sb_gamever_loc_variant(const char *sha256_hex);

/* W256 PACK-ANYVER: Paket-Familie "steel-battalion" = alle bekannten,
 * unterstuetzten Fassungen (Texturen sind per Inhalts-Hash gleich).
 * sb_gamever_family_idx: Index der Fassung zum XBE-SHA-256 (64 Hex,
 * Gross/Klein egal) oder -1 = unbekannt. sb_gamever_family_sha/_name:
 * SHA (klein) bzw. Kurzname "Europe"/"USA" zum Index (NULL ausserhalb). */
#define SB_GAMEVER_FAMILY "steel-battalion"
int sb_gamever_family_count(void);
int sb_gamever_family_idx(const char *sha256_hex);
const char *sb_gamever_family_sha(int i);
const char *sb_gamever_family_name(int i);

#ifdef __cplusplus
}
#endif
