/* loc_netpanel.h - W447 LOC-NET-AUTO (GitHub #82): kleines Netz-Panel in der LoC-Lobby.
 *
 * Zeigt beim Entstehen bzw. Beitreten einer Lobby, was der Netzmodus waehlt (Auto -> Direkt/Rollback, Rollback bzw.
 * Lockstep manuell), die Verzoegerung in Ticks und ms (Start -> Ziel), den Lobby-Modus (Sim/Arcade, W440) und den Ping
 * je Mitspieler. Aktualisiert sich (2x/s), klappt nach einigen Sekunden ohne Aenderung auf eine Zeile zusammen,
 * verschwindet in der Mission, bei offenem ESC-Menue und bei sichtbarem NET-Overlay (F3, zeigt mehr); Schalter
 * loc_net_panel (Standard 1). Lage: obere rechte Ecke des Spielbilds (frei in der Lobby, 4:3/16:9/21:9 geprueft).
 * Nur Zahlen und Pilotnamen aus der Sitzung, nichts im Log ausser Zaehlern. */
#pragma once

struct ID3D11RenderTargetView;

/* Spiel-Thread (loc_render.c, je Bild, nur LoC-Profil): Werte holen (2x/s). */
void loc_netpanel_frame(unsigned f);
/* Render-Thread (unter ctx_lock): sichtbar? / zeichnen (save = Bereich sichern) / Bereich zurueck. */
int loc_netpanel_visible(void);
void loc_netpanel_draw(struct ID3D11RenderTargetView *rtv, int save);
void loc_netpanel_restore(void);
/* TEST (loc_net_panel_test): 1 = jetzt ein Bild aufnehmen (Name in name). Render-Thread. */
int loc_netpanel_shot_due(char *name, unsigned cap);
