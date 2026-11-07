#pragma once
/* sb_perfhud.h - W122 PERF-HUD: Leistungs-Overlay (F2).
 *
 * Kompaktes Panel im Look des ESC-Overlays (gruenes SB-UI, sb_overlay.c
 * gemeinsame API) in einer Bildecke: praesentierte FPS, Spiel-Ticks/s,
 * Bildzeit min/avg/max + 1 %-Low, Bildzeit-Graph, CPU je Tick (Spiel/Render/
 * Interp), GPU-Zeit (D3D11-Timestamp-Queries), Draws/Tick, Aufloesung/Modus.
 *
 * Taste (perf_hud_key, Standard F2): an/aus; Shift+Taste: Ecke wechseln.
 * Beide Aenderungen werden sofort in die Spieler-cfg (user.cfg, wie
 * "Als Standard speichern") geschrieben (Keys perf_hud, perf_hud_pos).
 *
 * Geschlossen (perf_hud=0) und ohne perf_log wird nichts gemessen und nichts
 * gezeichnet - Standardpfad bitgleich (nur je Present/Tick ein int-Test).
 * Messung ohne Anzeige: [overlay] perf_log=N (DIAG) schreibt alle N
 * Sekunden eine Zeile PERFHUD ... und Einzelzeilen PERFSPIKE fuer
 * Bildzeiten ueber perf_spike_ms. */
#ifdef __cplusplus
extern "C" {
#endif

#ifndef _WINDEF_
#include <windows.h>
#endif

struct ID3D11RenderTargetView;

/* 1 = Messung aktiv (HUD sichtbar oder perf_log). Hot-Path-Test. */
extern volatile int g_phud_meas;

/* Fenster-Nachricht (UI-Thread). 1 = verbraucht. */
int sb_phud_wndproc(HWND w, UINT msg, WPARAM wp, LPARAM lp);
/* Anfang sb_render_present (Spiel-Swap, Render-Thread): Tasten/Schalter,
 * Tick-Beginn (draws = Draws seit dem letzten Swap). */
void sb_phud_tick_begin(unsigned draws);
/* Ende sb_render_present (alle Rueckwege): Tick-Ende. */
void sb_phud_tick_end(void);
/* Warte-Hooks (VBlank/Feld-Status, hooks_timing.c): Ein-/Austritt. */
extern volatile int g_phud_in_wait;
void sb_phud_wait_enter(void);
void sb_phud_wait_leave(void);
/* Interp-CPU-Zeit (QPC-Einheiten) gutschreiben; where: 0 = sonst,
 * 1 = in rec_draw (in cap enthalten), 2 = im Warten. */
void sb_phud_interp_q(long long dq, int where);
/* direkt vor / nach jedem echten Present (beide Wege), unter ctx_lock. */
void sb_phud_present_pre(void);
void sb_phud_present_post(long long q_before, int direct); /* direct = Present in sb_render_present */
/* Panel auf rtv zeichnen (unter ctx_lock). save=1: Bereich sichern
 * (Spiel-BB ohne FPS-INTERP, sb_phud_restore nach dem Present). */
void sb_phud_draw(struct ID3D11RenderTargetView *rtv, int save);
void sb_phud_restore(void);
/* 1 = Panel sichtbar */
int sb_phud_visible(void);
/* Test-Hook (overlay_test PHUDPOS): naechste Ecke (Render-Thread). */
void sb_phud_cycle_pos(void);

/* DIAG(TICK-PERF,W125): Zeitmesser um Blockier-Kandidaten des Spiel-Threads
 * (perf_blk=1, nur mit perf_log; Zeile TICKBLK je perf_log-Fenster, ms je
 * Tick). Aus: ein int-Test je Stelle. */
enum { TB_BLEND, TB_VISQ, TB_IPMAP, TB_IPX, TB_RTT, TB_CLEAR, TB_SWAP, TB_LOCK, TB_N };
extern volatile int g_tb_on;
extern long long g_tb_q[TB_N];
extern unsigned g_tb_n[TB_N];
static __inline long long tb_now(void) { LARGE_INTEGER x; QueryPerformanceCounter(&x); return x.QuadPart; }
#define TB_BEGIN() (g_tb_on ? tb_now() : 0)
#define TB_END(k, q0) do { if (g_tb_on) { g_tb_q[(k)] += tb_now() - (q0); g_tb_n[(k)]++; } } while (0)
/* 1 = Spiel-Thread steht im cap-Zaehler (render_im.c RQP_CAP) */
extern int g_phud_in_cap;
/* DIAG(PERF-TICK,W168): Bereich des Spiel-Threads fuer perf_prof=2 (0 = Spiel/sonst,
 * 1 = VB-/IM-Uebergabe, 2 = Swap/Present). Nur ein int-Speicher je Aufruf. */
extern volatile int g_ph_zone;

/* DIAG(GPU-COST,W146) perf_gpass=N: GPU-Zeit je Pass einer Zwischenbild-
 * Wiedergabe (jede N-te), asynchrone Timestamp-Queries (kein GPU-Sync, Ring
 * aus 4 Saetzen), Marke bei jedem Kategorie-Wechsel. Zeile GPUPASS je
 * perf_log-Fenster, Panel-Zeilen "GPU Pass". Aus: ein int-Test je Marke.
 * Kategorien: Startkopien, Welt-RT (D3D11-Kante >= 1024: Hauptmonitor-Welt)
 * ohne/mit Shading-PS, weitere RTs (Sub-Monitore, Karte, Komposition) ohne/mit
 * Shading-PS, Backbuffer (Cockpit/HUD) ohne/mit Shading-PS, Monitor-Korn/
 * -Stoerung (MONITOR-FX), Remaster-Effekte (REM-FX), Blit, Shader-Nachbearbeitung
 * (Cockpit-Licht/Farbe/FXAA/SSAO), Explosions-Licht, Rand/Overlays. VR (OpenXR-
 * Thread) wird getrennt als Summe je VR-Bild gefuehrt. */
enum { GP_START, GP_WELT, GP_WELT_SH, GP_SUB, GP_SUB_SH, GP_BB, GP_BB_SH, GP_MFX, GP_FX,
       GP_BLIT, GP_POST, GP_FXPOST, GP_HUD, GP_N };
extern int g_gp_open; /* 1 = Messung offen (unter ctx_lock) */
int sb_gp_begin(int vr);  /* 1 = Messung dieses Bildes begonnen */
void sb_gp_mark_(int cat);
#define sb_gp_mark(c) do { if (g_gp_open) { sb_gp_mark_(c); } } while (0)
void sb_gp_end(void);
/* Kategorie eines Wiedergabe-Ziels (Groesse der D3D11-Textur, gecacht) */
int sb_gp_rt_class(struct ID3D11RenderTargetView *rtv, int is_bb, int rq);

#ifdef __cplusplus
}
#endif
