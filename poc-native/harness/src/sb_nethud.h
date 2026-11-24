#pragma once
/* sb_nethud.h - W374 LOC-NET-OVERLAY: Netz-Overlay (F3), parallel zum Leistungs-Overlay F2 (sb_perfhud.c).
 *
 * Kleines Panel im Look des ESC-Overlays: Weg (direkt/Relay), Ping (aktuell, min) und Schwankung, Verlust
 * ein/aus, Modus (Rollback/Lockstep mit Grund), Verzoegerung (Ticks und ms), Rollbacks/s, Tiefe (p95 der
 * letzten 10 s), Vorhersage-Anteil, Vorsprung (Zeitabgleich), Mitspieler-Anzahl, bei mehreren Mitspielern je
 * Mitspieler eine Zeile (Pilotname gekuerzt, Ping). Keine Adressen, kein Raumcode. Ausserhalb des Netzspiels:
 * "Offline - kein Netzspiel".
 *
 * Taste: Profil-Aktion net_hud (Abschnitt [client], Standard F3) schaltet an/aus, Umschalt + dieselbe Taste
 * wechselt die Ecke; beides wird sofort in user.cfg gemerkt ([overlay] net_hud, net_hud_pos).
 * Kosten: ein eigener Abtast-Thread liest nur bei sichtbarem Panel alle 500 ms die vorhandenen Zaehler
 * (loc_net_hud_read, loc_rb_hud_read) und setzt die Zeilen; gezeichnet wird mit sb_perfhud.c (gleiche Stellen,
 * neu gemalt nur bei neuen Zeilen). Aus: nichts gemessen, nichts gezeichnet, Spiel-Thread unberuehrt. */
#ifdef __cplusplus
extern "C" {
#endif

#ifndef _WINDEF_
#include <windows.h>
#endif

struct ID3D11RenderTargetView;

/* Tasten der Aktion net_hud (hooks_input.c beim Profil-Laden): vk/mods (Bit 0 Shift, 1 Strg, 2 Alt). n = 0: aus. */
void sb_nhud_set_binding(const int *vks, const int *mods, int n);
/* Fenster-Nachricht (UI-Thread, aus sb_phud_wndproc). 1 = verbraucht. */
int sb_nhud_wndproc(HWND w, UINT msg, WPARAM wp, LPARAM lp);
/* Anfang sb_render_present (aus sb_phud_tick_begin): Anforderungen (an/aus, Ecke) uebernehmen. */
void sb_nhud_tick(void);
/* 1 = Panel sichtbar */
int sb_nhud_visible(void);
/* Panel zeichnen (unter ctx_lock, aus sb_phud_draw). avoid = Rechteck x,y,w,h des F2-Panels (NULL = keins). */
void sb_nhud_draw(struct ID3D11RenderTargetView *rtv, int save, const unsigned *avoid);
/* W467: Flaeche des letzten Bildes im Belegungsplan vorab anmelden */
void sb_nhud_layout_pre(void);
void sb_nhud_restore(void);
/* Test-Hooks (overlay_test NHUD / NHUDPOS) und Belegzeile (overlay_test LOG). */
void sb_nhud_toggle(void);
void sb_nhud_cycle_pos(void);
void sb_nhud_test_log(void);

#ifdef __cplusplus
}
#endif
