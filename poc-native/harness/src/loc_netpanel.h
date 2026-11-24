/* loc_netpanel.h - W447 LOC-NET-AUTO (GitHub #82), seit W453 LOC-LOBBY-INFO (GitHub #90) das Lobby-Infopanel.
 *
 * Dauerhaft, solange der Lobby-Screen einer LoC-Sitzung aktiv ist (Mitspieler verbunden, keine Mission), dezent unten
 * rechts in der Ecke des Bildschirms (Backbuffer, Slot OVS_SLOT_LOBBYINFO, Chat unten links). Hoechstens 5 Zeilen:
 * Netzmodus (Auto -> Direkt/Rollback bzw. Rollback/Lockstep manuell), Verzoegerung in Ticks und ms (Start -> Ziel),
 * schlechtester Ping der Lobby (gruen bis 80 ms, gelb bis 150 ms, rot darueber), Sim/Arcade (W440) mit Sichtbarkeit der
 * Lobby (oeffentlich, privat mit Raumcode, System Link) und Host-Regel Clip dumping (W420). Aktualisiert sich 2x/s,
 * verschwindet in der Mission, bei offenem ESC-Menue und bei sichtbarem NET-Overlay (F3, zeigt mehr); Schalter
 * loc_net_panel (Standard 1, Launcher und ESC-Menue). Nur lesend: aendert nichts am Match. Log LOCNETPANEL nur Zahlen. */
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
