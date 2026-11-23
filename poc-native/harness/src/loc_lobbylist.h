/* W443 LOC-LOBBY-LIST (GitHub #79): Modus (SIM/ARCADE) und Ping je Zeile der Suchliste im Spiel ("Search Match").
 *
 * Zuordnung Zeile <-> steelnet-Lobby: Die Host-Infos der Liste liefert unsere Netz-HLE selbst (W373, je offener
 * oeffentlicher Lobby ein "Geist"). Ihre XNKID traegt die Lobby-Id ("\0SBP" + Id, XNKEY "SBPUBLIC" + Id). Das Spiel
 * kopiert jede Host-Info ab +0xC in seine Liste (G+0x55E8, je 0x58 B), also steht die Id in jedem Listeneintrag.
 * Zeilen echter Hosts (System Link im LAN, Raumcode-Lobby) tragen keine solche XNKID -> keine Anzeige.
 *
 * Spielspeicher (LoC-XBE, B = Decompilat, LOC_BOOT §89):
 *   G = *(0x361708); G+0x55E4 int8 Zahl der Eintraege (<= 49), G+0x55E5 int8 gewaehlter Eintrag, G+0x55E8 Liste.
 *   *(*(0x278850)+0x14) == 2: die Ergebnisliste ist die System-Link-Liste (FUN_000DE2E0; sonst Live-Liste).
 *   M = *(0x278F58): +0 Unterzustand (1 = Liste wird gezeichnet, FUN_000FC120), +0x29 Einblendung (> 0 sichtbar),
 *   +0x2D5 int8 erste sichtbare Zeile. Bis 10 Zeilen (FUN_000FC0D0), Zeile k: Rechteck x 0..0xD7,
 *   y 0x57+28k .. 0x6E+28k, Name ab x 0x34 (FUN_000FADD0) - 640x480-Menueraum.
 * Reine Helfer (auch loc_net_selftest); keine Spielzustands-Patches, nur Lesen.
 */
#ifndef LOC_LOBBYLIST_H
#define LOC_LOBBYLIST_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>

#define LLV_G_PTR 0x361708u
#define LLV_G_COUNT 0x55E4u
#define LLV_G_SEL 0x55E5u
#define LLV_G_LIST 0x55E8u
#define LLV_ENTRY 0x58u
#define LLV_MAX_ENTRIES 49
#define LLV_LISTKIND_PTR 0x278850u
#define LLV_MENU_PTR 0x278F58u
#define LLV_M_SUB 0x0u
#define LLV_M_ALPHA 0x29u
#define LLV_M_TOP 0x2D5u
#define LLV_ROWS 10
#define LLV_ROW_Y0 0x57 /* erste Zeile oben (Menueraum 640x480) */
#define LLV_ROW_H 0x1C  /* Zeilenabstand */
#define LLV_ROW_Y1 0x6E /* erste Zeile unten */
#define LLV_NAME_X 0x34 /* Name beginnt hier; links davon ist Platz fuer die Marke */

typedef struct llv_view {
    int active;            /* Suchliste (System Link, Unterzustand Liste) sichtbar */
    int count, top, sel;   /* Eintraege, erste sichtbare Zeile, gewaehlter Eintrag */
    int nvis;              /* sichtbare Zeilen (<= LLV_ROWS) */
    int lid[LLV_ROWS];     /* Lobby-Id je sichtbarer Zeile (0 = kein steelnet-Geist, z. B. echter Host im LAN) */
    int sel_lid;           /* Lobby-Id des gewaehlten Eintrags */
} llv_view;

static __inline uint32_t llv_get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Lobby-Id aus einem Listeneintrag (0x58 B ab Host-Info +0xC): XNKID 0x00 "SBP" Id, XNKEY "SBPUBLIC" Id 0000.
 * 0 = kein Eintrag unserer Netz-HLE. */
static __inline int llv_entry_lid(const uint8_t *e)
{
    uint32_t id;
    if (!e || e[0] != 0 || memcmp(e + 1, "SBP", 3) != 0 || memcmp(e + 8, "SBPUBLIC", 8) != 0) return 0;
    id = llv_get32(e + 4);
    if (id < 1u || id > 2147483647u || llv_get32(e + 0x10) != id || llv_get32(e + 0x14) != 0u) return 0;
    return (int)id;
}

/* Ansicht aus den gelesenen Werten (alle geprueft). entries = count * LLV_ENTRY Bytes ab G+0x55E8 (oder NULL).
 * Rueckgabe v->active. */
static __inline int llv_view_from(int listkind, int sub, int alpha, int count, int top, int sel, const uint8_t *entries,
                                  llv_view *v)
{
    int k;
    memset(v, 0, sizeof *v);
    if (listkind != 2 || sub != 1 || alpha <= 0 || count < 1 || count > LLV_MAX_ENTRIES || top < 0 || top >= count ||
        sel < 0 || sel >= count || !entries)
        return 0;
    v->active = 1;
    v->count = count;
    v->top = top;
    v->sel = sel;
    v->nvis = count - top < LLV_ROWS ? count - top : LLV_ROWS;
    for (k = 0; k < v->nvis; k++) v->lid[k] = llv_entry_lid(entries + (size_t)(top + k) * LLV_ENTRY);
    v->sel_lid = llv_entry_lid(entries + (size_t)sel * LLV_ENTRY);
    return 1;
}

/* Anzeige des Modus (LAS_SIM = 1, LAS_ARCADE = 2), sonst NULL (keine Marke). */
static __inline const char *llv_mode_label(int mode)
{
    return mode == 2 ? "ARCADE" : mode == 1 ? "SIM" : NULL;
}

/* Ping-Schaetzung (ms, ueber das Relay, W373): "~120 ms", unbekannt "--". */
static __inline void llv_ping_text(int est, char *out, size_t cap)
{
    if (!out || cap < 4) return;
    if (est > 0) _snprintf_s(out, cap, _TRUNCATE, "~%d ms", est > 999 ? 999 : est);
    else strcpy_s(out, cap, "--");
}

/* Zeile k (0..LLV_ROWS-1) im Menueraum: oberer/unterer Rand. */
static __inline void llv_row_y(int k, int *y0, int *y1)
{
    *y0 = LLV_ROW_Y0 + k * LLV_ROW_H;
    *y1 = LLV_ROW_Y1 + k * LLV_ROW_H;
}

#endif
