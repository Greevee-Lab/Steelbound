/* loc_lobbylist_ovl.h - W443 LOC-LOBBY-LIST (GitHub #79): Anzeige SIM/ARCADE und Ping in der Suchliste des Spiels.
 * Reine Helfer und Speicherbelege: loc_lobbylist.h. Schalter loc_lobby_list (Standard 1), TEST loc_lobby_list_test. */
#pragma once

struct ID3D11RenderTargetView;

/* Spiel-Thread (loc_render.c, je Bild, nur LoC-Profil): Suchliste lesen, Modus/Ping je Zeile vom Transport holen. */
void loc_lobbylist_frame(unsigned f);
/* Render-Thread (unter ctx_lock): sichtbar? / zeichnen (save = Bereich sichern) / Bereich zurueck. */
int loc_lobbylist_visible(void);
void loc_lobbylist_draw(struct ID3D11RenderTargetView *rtv, int save);
void loc_lobbylist_restore(void);
/* TEST (loc_lobby_list_test=1): 1 = jetzt ein Bild aufnehmen (Name in name). Render-Thread. */
int loc_lobbylist_shot_due(char *name, unsigned cap);
