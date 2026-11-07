#pragma once
/* launcher_u8.h - UTF-8-Schicht des Launchers (W82 LAUNCHER-UI2).
 * Intern sind alle Zeichenketten UTF-8 (char *). An der Windows-Grenze
 * wird nach UTF-16 gewandelt (W-API), damit Pfade mit Nicht-ASCII-Zeichen
 * funktionieren. cfg-Dateien bleiben in der ANSI-Codepage (wie bisher, der
 * Client liest sie mit A-API, s. hcfg.c): Pfadwerte werden beim Lesen
 * ANSI -> UTF-8 und beim Schreiben UTF-8 -> ANSI gewandelt (Fallback:
 * 8.3-Kurzname). Quelltext bleibt ASCII: Umlaute als UTF-8-Escapes
 * (Makros U8_AE ... unten). */
#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdio.h>

/* Umlaute als UTF-8 (getrennte Literale, damit Hex-Escapes enden). */
#define U8_AE "\xC3\xA4"   /* ae */
#define U8_OE "\xC3\xB6"   /* oe */
#define U8_UE "\xC3\xBC"   /* ue */
#define U8_AEC "\xC3\x84"  /* Ae */
#define U8_OEC "\xC3\x96"  /* Oe */
#define U8_UEC "\xC3\x9C"  /* Ue */
#define U8_SZ "\xC3\x9F"   /* ss */
#define U8_PARA "\xC2\xA7" /* Paragraph */
#define U8_TIMES "\xC3\x97" /* Malzeichen */
#define U8_DOT "\xC2\xB7"  /* Mittelpunkt */
#define U8_DASH "\xE2\x80\x93" /* Halbgeviertstrich */
#define U8_ARROW "\xE2\x86\x92" /* Pfeil rechts */
#define U8_ELL "\xE2\x80\xA6"  /* Auslassung */
#define U8_LQ "\xE2\x80\x9E"   /* Anfuehrung unten */
#define U8_RQ "\xE2\x80\x9C"   /* Anfuehrung oben */

/* UTF-8 <-> UTF-16. Rueckgabe = Zeichen ohne NUL (0 bei Fehler/leer).
 * Ziel wird immer terminiert (gekuerzt, wenn zu klein). */
int u8_to_w(const char *s, wchar_t *out, int cap);
int w_to_u8(const wchar_t *s, char *out, int cap);
/* Neu allokiert (free()), NULL bei Fehler. */
wchar_t *u8_wdup(const char *s);

/* ANSI-Codepage (cfg-Dateien) <-> UTF-8. */
void u8_from_acp(const char *acp, char *out, size_t cap);
/* UTF-8 -> ANSI fuer cfg-Werte. Rueckgabe 0 = verlustfrei, 1 = 8.3-
 * Kurzname benutzt (verlustfrei, Pfad existiert), 2 = nicht darstellbar
 * (out enthaelt Ersatzzeichen; Client kann den Pfad nicht oeffnen). */
int u8_to_acp(const char *u8, char *out, size_t cap);
/* 1 = rein ASCII. */
int u8_is_ascii(const char *s);

/* Datei-Wrapper (Pfade UTF-8). */
int u8_fopen_s(FILE **f, const char *path, const char *mode);
unsigned long u8_GetFileAttributes(const char *path); /* INVALID_FILE_ATTRIBUTES */
int u8_DeleteFile(const char *path);                  /* 1 = ok */
int u8_CreateDirectory(const char *path);             /* 1 = ok */
/* Umgebungsvariable lesen; Rueckgabe = Laenge (0 = fehlt). */
unsigned long u8_GetEnv(const char *name, char *out, size_t cap);
/* Temp-Ordner mit abschliessendem Trenner. */
void u8_GetTempPath(char *out, size_t cap);
/* Ordner der eigenen Exe (ohne Trenner am Ende). 1 = ok. */
int u8_ExeDir(char *out, size_t cap);

/* Verzeichnis-Iteration: Muster UTF-8, Namen UTF-8. */
typedef struct {
    void *h;
    char name[260 * 3];
    unsigned long attr;
} u8_find_t;
int u8_find_first(const char *pattern, u8_find_t *fd); /* 1 = Eintrag */
int u8_find_next(u8_find_t *fd);                        /* 1 = Eintrag */
void u8_find_close(u8_find_t *fd);

/* Prozess starten (cmd/cwd UTF-8). out_pid/proc/thread duerfen NULL sein
 * (Handles werden dann geschlossen). 1 = ok. */
int u8_CreateProcess(const char *cmdline, const char *cwd, unsigned long flags,
                     int inherit, void *si_stdout, int hide,
                     unsigned long *out_pid, void **out_proc);

#ifdef __cplusplus
}
#endif
