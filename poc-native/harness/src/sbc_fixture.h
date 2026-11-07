#pragma once
/* sbc_fixture.h - Reports aus einer Textdatei statt vom USB-Geraet (W75).
 *
 * Reines Modul (nur C-STDIO, kein Harness, kein WinUSB): laedt eine
 * Fixture-Datei und waehlt pro Abfrage den gueltigen Eintrag. Laufzeitpfad
 * ist derselbe wie beim echten Geraet (sbc_usb_parse_report ->
 * sbc_usb_state_to_block); nur die Quelle der 26 Bytes ist die Datei.
 *
 * Format (s. harness/fixtures/sbc/*.sbc):
 *   # Kommentar, Leerzeilen ok
 *   <when> <hex26>              Daten: 52 Hexziffern (26 B inkl. 00 1A),
 *                               Leerzeichen erlaubt; when = fN/fN... Frames
 *                               (Default), Nms = Millisekunden seit Start
 *   mem:0xADDR=0xVAL            Anker: setzt den Zeitursprung folgender
 *                               fN-Eintraege auf die erste Flanke nach
 *                               u32[ADDR]==VAL (Syntax wie replay_steps)
 *   memall:0xA1,...=LINIE       Anker: erste Flanke nach "alle Floats
 *                               strikt > LINIE" (0.7 oder Hex-Bits,
 *                               max. 8 Adressen). Anker feuern der Reihe
 *                               nach (Flanke + Ordnung: kein Feuern auf
 *                               Null-Speicher am Boot).
 * Gueltig ist der letzte Eintrag in Dateireihenfolge, dessen Zeitpunkt
 * erreicht ist (ms-Eintraege absolut, fN-Eintraege relativ zum letzten
 * Anker darueber bzw. zu Frame 0). Vor dem ersten gueltigen Eintrag und
 * vor dem Feuern eines Ankers: kein Zustand (Tastatur pur).
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SBC_FIX_MAX 4096 /* max. Dateneintraege je Datei */
#define SBC_FIX_ANCHORS 32 /* max. Anker je Datei */
#define SBC_FIX_MEMALL 8 /* max. Adressen je memall (wie replay) */

typedef uint32_t (*sbc_fix_memread_fn)(uint32_t addr); /* NULL = nie wahr */

typedef struct {
    uint64_t when; /* Frame (relativ zum Anker) oder ms (absolut) */
    int is_ms;
    int anchor; /* Anker-Index darueber (-1 = Laufstart) */
    uint8_t raw[26]; /* IN-Report inkl. Header (sbc_usb_parse_report) */
} sbc_fix_entry_t;

typedef struct {
    int is_memall;
    uint32_t addr[SBC_FIX_MEMALL];
    unsigned naddr;
    uint32_t val; /* mem: Vergleichswert (u32) */
    float line; /* memall: Linie (strikt >) */
    int fired; /* 1 = Bedingung traf je zu */
    unsigned fire_frame; /* erster Frame mit Bedingung wahr */
    int seen_false; /* 1 = Bedingung war je falsch (Flanken-Erkennung) */
} sbc_fix_anchor_t;

typedef struct {
    sbc_fix_entry_t *e;
    unsigned n;
    sbc_fix_anchor_t *a;
    unsigned na;
    unsigned skipped; /* uebersprungene defekte Zeilen */
} sbc_fixture_t;

/* Datei laden. 0 = ok, <0 = Fehler (sbc_fixture_strerror). */
int sbc_fixture_load(const char *path, sbc_fixture_t *fx);
void sbc_fixture_free(sbc_fixture_t *fx);

/* Gueltigen Eintrag fuer (frame, ms_seit_start) waehlen. Anker werden per
 * memread geprueft und feuern einmalig (latcht). 1 = *idx gueltig,
 * 0 = noch kein Eintrag aktiv. memread NULL: Anker feuern nie. */
int sbc_fixture_at(sbc_fixture_t *fx, unsigned frame, uint64_t ms,
                   sbc_fix_memread_fn memread, unsigned *idx);

const char *sbc_fixture_strerror(int err);

#ifdef __cplusplus
}
#endif
