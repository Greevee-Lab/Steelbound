/* loc_chatnet.h - W437 LOC-CHAT: Textchat in LoC-Lobby und -Match (GitHub #77). Kern in loc_chat.c; hier Kanal
 * (SBCH ueber den eigenen Transport, Spielport 1262), Eingabezeile (Fenster-Nachrichten), Anzeige (GDI -> Quad),
 * Testfolge. Nur LoC online (steelnet, sec=ok); in SB und offline tut die Chat-Taste nichts. Keine Inhalte und keine
 * Namen in Logs, Crashdumps oder F11-Paketen (nur Zaehler). */
#pragma once
#include <windows.h>

struct ID3D11RenderTargetView;

/* LoC-Start (loc_boot.c): Zustand, Thread (Kanal anmelden, Verfuegbarkeit, Testfolge). */
void loc_chat_install(void);
/* 1 = Chatzeile offen (alle Tasten gehen an den Chat, Spiel bekommt keine Eingabe). Billig, jeder Thread. */
int loc_chat_typing(void);
/* 1 = Chat verfuegbar: Lobby-Tab (loc_chat=1, Treffpunkt + Ende-zu-Ende ok, verbundener Mitspieler) oder W446
 * steelnet-Tab (loc_snchat=1, beim Dienst abonniert). Tab-Taste wechselt bei offener Zeile. */
int loc_chat_avail(void);
/* Fenster-Nachricht (UI-Thread, aus sb_ovl_wndproc vor dem Menue). 1 = verbraucht. */
int loc_chat_wndproc(HWND w, UINT msg, WPARAM wp, LPARAM lp);
/* W452: IME-Kontext an den Chat-Zustand angleichen (UI-Thread, vor jeder Fenster-Nachricht, auch bei offenem Menue):
 * IME nur bei offener Chatzeile, sonst kein Eingabekontext (Spieltasten nie im IME). */
void loc_chat_ime_tick(HWND w);
/* Tasten der Client-Aktion "chat" (Profil; Standard Enter bis zum Laden). mods: Bit 0 Umschalt, 1 Strg, 2 Alt. */
void loc_chat_set_binding(const int *vks, const int *mods, int n);
/* Anzeige (Render-Thread, unter ctx_lock): sichtbar? / zeichnen (save = Bereich sichern) / Bereich zurueck. */
int loc_chat_visible(void);
void loc_chat_draw(struct ID3D11RenderTargetView *rtv, int save);
void loc_chat_restore(void);
/* TEST (loc_chat_test Schritt P): 1 = jetzt ein Bild aufnehmen (Name in name), sonst 0. Render-Thread. */
int loc_chat_shot_due(char *name, unsigned cap);
/* Absturz (sb_crash vor dem Minidump): alle Chat-Inhalte im Speicher loeschen, danach nichts mehr annehmen. */
void loc_chat_crash_wipe(void);
