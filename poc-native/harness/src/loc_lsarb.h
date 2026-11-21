/* loc_lsarb.h - LOC-LOCKSTEP-ARBITER: Pufferregler des Original-Lockstep (Rueckfall ohne Rollback), nur LoC-Profil.
 *
 * Reines Modul ohne Spielspeicher, Log oder Konfiguration (Selbsttest tests/loc_rbi_selftest.c). Die Anbindung an
 * FUN_0001D010 (Thunk @0x1D0BF, Spielvariablen, Log, F3) macht loc_lsarb_game.c.
 *
 * Befund (LoC 1.0, LOC_BOOT §69): FUN_0001D010 baut je Tick den eigenen 0x34-Rahmen. Zwischen 0x1D0BF und 0x1D381
 * liegt die Regelung. Messgroesse ist der Fuellstand q[i] der Empfangsringe ([[0x361708]+0x5180+4i], gesetzt in
 * FUN_0004EDC0, 128 Plaetze je Spieler; der eigene Ring fuellt sich beim Senden, q[self] = Vorlauf L = wirksame
 * Eingabeverzoegerung in Ticks). Nur der Taktgeber (niedrigster aktiver Platz, FUN_00019D10, meist der Host) setzt in
 * seinem Rahmen die Bits 0x40000000 (alle schicken einen Zusatzrahmen: L+1) bzw. 0x10000000 (alle entnehmen einen
 * Rahmen mehr: L-1); jeder Client wendet sie an, wenn er diesen Rahmen entnimmt (FUN_0001D4C0) - also alle im selben
 * Tick. Deshalb ist jede Entscheidung des Taktgebers fuer alle Seiten gleich (synchron), egal was die anderen
 * Clients einstellen.
 *
 * Original (lsarb_orig_step, Referenz fuer den Selbsttest): D = [0x306F04] ist der Soll-Mindestfuellstand; +1 alle
 * 20 Ticks bei Spanne qmax-qmin > 2 (die Spanne enthaelt den eigenen, immer vollsten Ring, misst also die Laufzeit,
 * nicht den Jitter), -1 alle 300..900 Ticks bei Spanne < 2, Grenzen 2..clamp(3*delay0,6,10). Taktgeber: qmin < D in
 * zwei Durchgaengen -> Zusatzrahmen, alle q >= D+1 -> Rahmen weg, danach qmax Ticks Pause. Bei qmax > 45 (@0x1D31D)
 * nimmt der Taktgeber jeden Spieler mit q == qmax aus der Aktiv-Maske seines Rahmens -> FUN_0001C400 auf allen
 * Clients: Spieler entfernt bzw. eigene Mission endet. Der vollste Ring ist der des Taktgebers selbst oder des am
 * besten verbundenen Mitspielers, nie der des zurueckliegenden.
 *
 * Neu (lsarb_tk_step): Vorlauf L direkt regeln. Untergrenze aus gemessener RTT + Jitter (lsarb_need), sonst
 * Lmin = 2 (100 ms). Zusatzrahmen bei wiederholtem Stau des Taktgebers oder L unter der Untergrenze; Rahmen weg nur,
 * wenn der knappste Mitspieler-Ring lange genug Reserve zeigt und kein Stau war, mit wachsender Sperrzeit nach
 * einem Hin und Her (Hysterese). Kein Rauswurf nach Warteschlangenlaenge: grosser Ueberhang wird begrenzt abgebaut
 * (hoechstens ein Rahmen je fast_cd Ticks). Echte Ausfaelle trennt weiter die Stille-Regel des Spiels. */
#pragma once
#include <stdint.h>

#define LSARB_MAXP 10
#define LSARB_HIST 256
#define LSARB_F_EXTRA 0x40000000u
#define LSARB_F_DROP 0x10000000u
#define LSARB_KICK_Q 45 /* Original-Grenze @0x1D31D */

typedef struct lsarb_cfg {
    int lmin, lmax;              /* Vorlauf-Grenzen in Ticks (Standard 2, 60) */
    int add_win, add_stalls;     /* Zusatzrahmen, wenn im Fenster add_win Ticks >= add_stalls Stau-Bilder (40, 3) */
    int add_cd;                  /* Pause nach Zusatzrahmen: L + add_cd Ticks (10) */
    int add_max;                 /* Stau-Zusatzrahmen nur bis Untergrenze + add_max (4): ein zu langsamer Mitspieler
                                  * (Rechenleistung) laesst sich mit Puffer nicht beheben, nur mit mehr Verzoegerung */
    int drop_win, drop_rmin;     /* Rahmen weg: Fenster ohne Stau und knappster Ring >= drop_rmin (100, 2) */
    int drop_cd;                 /* Pause nach Rahmen weg (20) */
    int fast_win, fast_rmin;     /* Ueberhang abbauen: Fenster ohne Stau, knappster Ring >= fast_rmin (20, 5) */
    int fast_cd;                 /* Pause beim Abbau (10 = hoechstens 2 Rahmen/s) */
    int hold0, hold_max;         /* Sperrzeit Rahmen-weg nach Zusatzrahmen, waechst x2 bei Hin und Her (100, 1200) */
    int need_add_ms;             /* Untergrenze L = ceil((RTT+2*Jitter + need_add_ms)/100) (50); 0 = RTT nicht nutzen */
} lsarb_cfg;

typedef struct lsarb_in {
    int npl;                     /* Spielerplaetze [0x3071A0] */
    uint32_t mask;               /* Aktiv-Maske [0x306830] */
    int self;                    /* eigener Platz [0x3071B0] */
    int local;                   /* eigener Spieler [0x306A84] (Nachholen) */
    int q[LSARB_MAXP];           /* Ringfuellstaende */
    uint32_t tick;               /* Spieltick [0x3070F4] */
    uint32_t stall_total;        /* Stau-Bilder gesamt [0x306C34] */
} lsarb_in;

typedef struct lsarb_out {
    uint32_t flags_or;           /* in den eigenen Rahmen: LSARB_F_EXTRA / LSARB_F_DROP */
    uint32_t mask_clear;         /* nur Original: Bits, die aus der Maske fallen (Rauswurf) */
    int steps;                   /* Schritte je Bild ([0x306944], 1 oder 2) */
    int tk_pass;                 /* 1 = Taktgeber-Durchgang in diesem Tick */
    int qmax, qmin, r, lead;     /* Messwerte (r = knappster fremder Ring, -1 ohne Mitspieler) */
} lsarb_out;

/* Original-Variablen (Spielspeicher), fuer die Referenz im Selbsttest */
typedef struct lsarb_ovars {
    int D, delay0, cool, xcnt;
    uint32_t lasttick;
    int have_last;
} lsarb_ovars;

typedef struct lsarb_st {
    uint32_t last_tick, last_stall, last_add, last_drop;
    int have, cd, hold, need;
    uint32_t hn;
    uint8_t hr[LSARB_HIST], hs[LSARB_HIST];
    uint32_t n_add, n_add_rtt, n_drop, n_fast, n_spike; /* Entscheidungen; n_spike = qmax > 45 (Original haette entfernt) */
} lsarb_st;

void lsarb_cfg_default(lsarb_cfg *c);
/* "k=v,k=v" (Schluessel wie die Feldnamen); unbekannte Schluessel zaehlen als Fehler (Rueckgabe Anzahl) */
int lsarb_cfg_parse(lsarb_cfg *c, const char *s);
void lsarb_reset(lsarb_st *st, const lsarb_cfg *c);
/* Untergrenze aus RTT-Schaetzung (ms, inkl. Jitter-Reserve) mit Hysterese 40 ms nach unten; rtt <= 0 = unbekannt */
int lsarb_need_update(lsarb_st *st, const lsarb_cfg *c, int rtt_eff_ms);
/* Taktgeber suchen (niedrigster aktiver Platz, FUN_00019D10), -1 = keiner */
int lsarb_timekeeper(int npl, uint32_t mask);
/* Gemeinsamer Teil beider Regler: Messwerte, Nachholen (Original-Regel), Taktgeber-Erkennung */
void lsarb_measure(const lsarb_in *in, lsarb_out *o);
/* Original-Regelung 0x1D0BF..0x1D381 als C (Referenz, im Spiel laeuft der Originalcode) */
void lsarb_orig_step(lsarb_ovars *ov, const lsarb_in *in, lsarb_out *o);
/* Neuer Regler: nur im Taktgeber-Durchgang wirksam (o->tk_pass), sonst nur Messung */
void lsarb_tk_step(const lsarb_cfg *c, lsarb_st *st, lsarb_ovars *ov, const lsarb_in *in, lsarb_out *o);
