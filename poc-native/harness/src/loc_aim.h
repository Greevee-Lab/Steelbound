/* loc_aim.h - W376 LOC-LOCAL-AIM: eigene Sicht bzw. eigenes Zielen sofort im Bild (nur LoC-Profil).
 *
 * Die eigene, schon bekannte, aber wegen der Eingabeverzoegerung D noch nicht simulierte Eingabe wird beim
 * Zeichnen auf Kamera bzw. HUD angewendet; die Simulation bleibt unberuehrt (kein Desync moeglich).
 * Schalter loc_local_aim (Standard 0). Diagnose loc_aim_diag (DIAG, Standard 0). */
#pragma once
#include <stdint.h>
#include "loc_aim_law.h" /* la_state_t (W394) */

/* Spiel-Thread, je Tick am Ruhepunkt (loc_rollback.c rb_after). replay = 1 waehrend des Neurechnens; rbc = aktive
 * Eingabeschicht (rbi_t *) oder NULL (Lockstep/Solo: keine Vorhersage). */
void loc_aim_game_tick(int replay, int slot, const void *rbc);

/* Ergebnis der Vorhersage des letzten Live-Ticks (Sicht- und Arm-Winkel in rad: vorhergesagt minus simuliert). */
typedef struct {
    uint32_t seq;      /* fortlaufend je Live-Tick (0 = nie) */
    uint32_t gframe;   /* hooks_frame_count beim Ruhepunkt */
    int n;             /* angewendete eigene Eingabesaetze (= wirksame Verzoegerung) */
    int ok;            /* 1 = Delta gueltig (Mission, Zustand ohne Sonderpfad) */
    float dys, dps;    /* Sicht Gier/Nick */
    float dax, day;    /* Waffenarm x/y */
    float ys, ps, ax, ay; /* simulierter Stand (Diagnose) */
    float yt, pt;      /* Soll-Gier/-Nick der Sicht (Diagnose) */
    /* W394 LOC-VIEW-PERFRAME: vorhergesagter Zustand nach den n Saetzen, juengster eigener Satz (Rahmen des Polls poll),
     * Zeit der Veroeffentlichung (QPC) und Spanne bis zum naechsten Poll (ms, geschaetzt) */
    la_state_t pst;
    int have_pst;
    uint8_t last_in[14];
    uint32_t poll;
    long long tq;
    float wspan_ms;
    float tick_ms;     /* W505: geschaetzter Abstand zweier Veroeffentlichungen (ms) - Rampe der Wiederholung (Eingabeprofil) */
    /* W442 LOC-AIMASSIST: Basis fuer den naechsten eigenen Satz = Zustand nach allen eigenen erzeugten Saetzen.
     * have_bst 1 = gueltig (Rollback-Eingabeschicht, n >= 0 Saetze), 0 = keine Basis (Lockstep: ausstehende eigene
     * Saetze unbekannt; Sonderpfad; ausserhalb der Mission) - die Zielhilfe wirkt dann nicht. */
    la_state_t bst;
    int have_bst;
} loc_aim_pub_t;

/* Render-Seite: juengstes Ergebnis kopieren. 1 = gueltig und loc_local_aim an. */
int loc_aim_get(loc_aim_pub_t *out);
/* DIAG: wie loc_aim_get, aber unabhaengig vom Schalter (0 = noch nichts veroeffentlicht). */
int loc_aim_peek(loc_aim_pub_t *out);

/* DIAG: Datei im Ausgabeordner zum Anhaengen oeffnen (NULL bei Fehler); Name ohne Pfad. */
void *loc_aim_diag_file(const char *name);
