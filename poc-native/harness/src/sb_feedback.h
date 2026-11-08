#pragma once
/* sb_feedback.h - W188 GAME-MENU: Feedback Stufe 1 (lokales Paket + GitHub-Seite).
 *
 * Kein Netzwerk, kein Token im Client: der Client legt nur ein Paket in den
 * Nutzerdaten an und oeffnet auf Nutzeraktion (ShellExecute) eine
 * vorausgefuellte "New issue"-Seite im Browser sowie den Paket-Ordner im
 * Explorer (Anhaenge zieht der Nutzer selbst hinein).
 *
 * Paket <nutzerdaten>\feedback\<zeit>\ (Modus profile/portable: Wurzel aus
 * sb_userdata, z. B. %LOCALAPPDATA%\Steelbound\feedback\...; legacy/Tests:
 * <Ausgabeordner>\feedback\...):
 *   screenshot.jpg  Spielbild der Pause (Client-Bild, nie der Desktop)
 *   log.txt         Ende des Client-Logs, bereinigt (Benutzername, Rechnername,
 *                   Profil-/Absolutpfade ersetzt)
 *   info.txt        Client-Build, Abbild-Version, GPU, Anzeige, Spielzustand,
 *                   wichtige Einstellungen (bereinigt)
 *   text.txt        Art + Kurztext des Nutzers (bereinigt)
 * URL: nur Titel ("[Bug] ..."/"[Idea] ...") und Kurzinfos (Version, GPU,
 * Zustand) als Parameter der Issue-Vorlage - keine Pfade, keine Namen. */
#include <stddef.h>
#include "sb_gamever.h"

/* Standard-Repo fuer "New issue" (einzige Stelle im Code; cfg [feedback] repo). */
#define SB_FEEDBACK_REPO_DEFAULT "Greevee-Lab/Steelbound"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _WINDEF_
#include <windows.h>
#endif

typedef struct {
    char dir[600];       /* Paket-Ordner (lokal) */
    char dir_show[200];  /* bereinigt fuer Anzeige/Log */
    char stamp[32];      /* JJJJMMTT-hhmmss */
    int clip;           /* 0 none, 1 MP4, 2 JPG fallback, -1 failed */
    unsigned clip_bytes, clip_w, clip_h;
    int shot_ok;
    unsigned shot_w, shot_h;
    int log_lines;       /* Zeilen in log.txt */
    int repl;            /* Ersetzungen (Log + Info + Text) */
    int clean_hits;      /* Nachpruefung: Benutzername/Profilpfad noch enthalten (soll 0) */
    int kind;            /* 0 = Bug, 1 = Idee */
    char url[2048];
} sb_fb_pkg_t;

/* emu_main.c: Ergebnis der Versionspruefung merken. */
void sb_feedback_note_gamever(const sb_gamever_t *gv);
/* Ordner anlegen (Zeitstempel). 1 = ok. */
int sb_feedback_begin(sb_fb_pkg_t *p, int kind);
/* info.txt, log.txt, text.txt schreiben, URL bauen, Nachpruefung. 1 = ok.
 * text = Kurztext des Nutzers (UTF-16, darf leer sein). */
int sb_feedback_finish(sb_fb_pkg_t *p, const wchar_t *text);
/* Bereinigen (in place): Benutzername, Rechnername, Profil-/Absolutpfade.
 * Rueckgabe: Anzahl Ersetzungen. */
int sb_feedback_sanitize(char *s, size_t cap);
/* Nutzeraktion: Browser (URL) + Explorer (Ordner) oeffnen; vorher Vollbild
 * minimieren. test=1: nichts oeffnen, nur protokollieren. which: 1 = beides,
 * 2 = nur Ordner. */
void sb_feedback_open(const sb_fb_pkg_t *p, HWND hwnd, int which, int test);

#ifdef __cplusplus
}
#endif
