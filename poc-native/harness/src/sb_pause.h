#pragma once
/* sb_pause.h - W188 GAME-MENU: Spielpause (ESC-Menue, Pause-Taste, Feedback).
 *
 * Wunsch: das Overlay (sb_overlay.c) meldet je Bild, ob das Spiel stehen soll
 * (sb_pause_request). Wirksam wird die Pause am Ende des naechsten Presents
 * des Spiel-Threads (sb_pause_point, d3d11_render.c): der Spiel-Thread bleibt
 * dort in einer Schleife, bis der Wunsch endet. Damit stehen Spiellogik,
 * Missions-Countdown und Film-Ausgabe (Spiellogik: VBlank-Feldzaehler,
 * FPS120.md 1.1; Film-Uhr: XAPI-TSC + KeTickCount, W192). Waehrenddessen:
 *   - zeigt die Schleife das letzte Spielbild (Kopie) mit Menue darueber
 *     (gewaehlte Bildrate, Overlay-Tasten/Test-Hook laufen weiter),
 *   - mischt der Audio-Mischer Stille (Stimmen ruecken nicht vor, Ton steht),
 *   - ruht der Present-Takt-Thread (W138), der VR-Thread laeuft weiter
 *     (Session bleibt, zeigt das stehende Bild).
 * Beim Fortsetzen werden die Gast-Uhren um die Pausendauer verschoben
 * (VBlank-Zaehler/XAPI-TSC hooks_timing.c, KeTickCount/KeQueryInterruptTime) - kein
 * Zeitsprung, keine Nachhol-Ticks; die Zwischenbild-Aufzeichnung beginnt neu.
 * Ohne Wunsch kehrt sb_pause_point sofort zurueck (Standardpfad bitgleich). */
#ifdef __cplusplus
extern "C" {
#endif

#ifndef _WINDEF_
#include <windows.h>
#endif

/* 1 = Spiel-Thread steht in der Pause-Schleife (Mischer stumm, Takt-Thread ruht). */
extern volatile LONG g_sb_paused;

/* Pause wuenschen (on=1) bzw. aufheben; why fuer das Log (nur bei Wechsel). */
void sb_pause_request(int on, const char *why);
int sb_pause_requested(void);
/* Spiel-Thread, Ende jedes Presents: bei Wunsch hier pausieren. */
void sb_pause_point(void);
/* Anzahl Pausen bisher (Feedback-Info). */
unsigned sb_pause_count(void);

/* ---- Modulgrenzen (Implementierung in anderen Dateien) ---- */
/* hooks_timing.c: VBlank-Zaehler anhalten/um die Pausendauer verschieben */
/* W192: XAPI-Performance-Counter (TSC) fuer Bink und Spiel-Player. */
void sb_tsc_pause(int begin);
unsigned long long sb_tsc_now(void);
unsigned sb_pause_movie_frame(void);
void sb_pause_movie_log(const char *where);
void sb_timing_pause_begin(void);
void sb_timing_pause_end(void);
unsigned sb_timing_field(void); /* letzter Feldzaehler (GetDisplayFieldStatus) */
/* kernel_hle.c / krnl_sync.c: KeTickCount + KeQueryInterruptTime */
void krnl_time_pause(int begin);
/* d3d11_render.c: Bild sichern / Pausenbild zeigen / Ende (Bild zurueck, Interp neu) */
int sb_render_pause_begin(void);
void sb_render_pause_frame(void);
void sb_render_pause_end(void);
/* sb_overlay.c: Tasten/Pad/Test-Hook eines Pausenbildes */
void sb_ovl_pause_tick(void);
int sb_ovl_pause_fps(void);
void sb_ovl_present_end(void);

#ifdef __cplusplus
}
#endif
