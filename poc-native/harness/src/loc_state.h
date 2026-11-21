/* loc_state.h - W382 LOC-ISSUES-B: Spieltakt-Messung und LoC-Zustandszeile fuer Feedback-/Issue-Pakete (rein, ohne
 * Spielspeicher; Selbsttest loc_rbi_selftest "W382").
 *
 * Anlass: Die GitHub-Issues #53/#55-#58 trugen im Feld "Game state" die SB-Zeile "loadcount=0 scene=0x0
 * in_mission=0 countdown=0" - in LoC sind das fremde Adressen ohne Aussage. Fuer Fragen wie "laeuft die Simulation
 * zu schnell?" (#53) oder "welcher Zwischenbild-Weg?" (#58, AMD) braucht das Paket den gemessenen Spieltakt. */
#pragma once
#include <stddef.h>

#define LOC_TR_N 80 /* Stuetzpunkte (je >= 1 s Abstand -> ~80 s Fenster) */

typedef struct {
    long long t[LOC_TR_N];   /* Zeitpunkt in Zaehlerschritten (z. B. QPC) */
    unsigned tick[LOC_TR_N]; /* Simulationstick des Spiels zu diesem Zeitpunkt */
    int n, head;             /* Anzahl, naechster Schreibplatz */
    long long freq;          /* Zaehlerschritte je Sekunde */
    unsigned resets;         /* Neuanfaenge (Tick kleiner als zuvor = neue Mission) */
} loc_tickrate_t;

/* Nur Ganzzahlen (Aufruf im Spiel-Thread ohne x87-Sicherung). Stuetzpunkt nur, wenn >= 1 s seit dem letzten. */
void loc_tickrate_push(loc_tickrate_t *r, long long t, unsigned tick, long long freq);
/* Ticks je Sekunde ueber hoechstens win_s Sekunden bis zum letzten Stuetzpunkt; 0 = zu wenig Daten (< 5 s). */
double loc_tickrate_hz(const loc_tickrate_t *r, double win_s, double *span_s);

typedef struct {
    int lockstep;       /* Lockstep-Maske des Spiels gesetzt (Einsatzkarte/Mission) */
    long frame;         /* Lockstep-Rahmen seit Missionsbeginn */
    unsigned tick;      /* Simulationstick */
    double hz, span_s;  /* gemessener Spieltakt und Messfenster (0 = unbekannt) */
    unsigned scene, step; /* Menue-Szene/-Schritt des Spiels */
    int interp_fps, render_thread, vblank_hz;
} loc_state_t;

/* "game=loc phase=lockstep|menu frame=… tick=… sim_hz=19.88 (60 s, original 20) scene=… interp_fps=… render_thread=…"
 * Rueckgabe: Laenge (ohne 0). */
int loc_state_line(char *out, size_t cap, const loc_state_t *s);
