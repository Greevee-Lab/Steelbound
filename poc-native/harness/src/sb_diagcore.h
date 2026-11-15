#pragma once
/* sb_diagcore.h - W279 DIAG-CRASH: gemeinsame Diagnose-Bausteine fuer Client
 * (sb_crash.c, sb_feedback.c) und Launcher (launcher_diag.c). Kein Netz, kein
 * Fenster, keine Spielzustaende - nur Text, Dateien, Pruefsummen.
 *
 *  - Bereinigen: Datenordner/Client-Ordner -> <data>/<client> (Pfade bleiben
 *    relativ lesbar), Umgebungsordner (%LOCALAPPDATA% ...), Benutzer-,
 *    Rechner- und Domaenenname, uebrige absolute Pfade -> <pfad>\datei.ext.
 *    Gleiche Regeln wie das F11-Feedback (W188, bisher in sb_feedback.c).
 *  - Pack-Pruefsumme: SHA-256 ueber Groesse + ZIP-Zentralverzeichnis (enthaelt
 *    die CRC32 jedes Eintrags, also inhaltsgenau und schnell auch bei GB-Packs);
 *    Ordner-Packs: SHA-256 der manifest.json.
 *  - cfg-Abweichungen: Schluessel aus user.cfg bzw. der --set-Kette, deren Wert
 *    von sb_native.cfg (Standard) abweicht.
 *  - Absturzberichte: Ordner <crash>\<JJJJMMTT-hhmmss>-crash|hang\ (status.txt =
 *    fatal | first-chance | hang, recovered.txt = Spiel lief danach weiter).
 *  - Minidump bereinigen: Modulpfade -> nur Dateiname, Benutzer-/Rechnername in
 *    ASCII und UTF-16 durch gleich lange Fuellzeichen ersetzt (Format bleibt).
 *
 * Pfad-Kodierung: crash_list/dump_scrub/pack_fp nehmen UTF-8 (intern Wide-API);
 * Text in sanitize darf UTF-8 oder ANSI sein (beide Durchgaenge wie W188). */
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *path; /* UTF-8, absolut (ohne abschliessenden Trenner noetig) */
    const char *tag;  /* Ersatz, z. B. "<data>" */
} sb_diag_root_t;

/* In place bereinigen. roots (darf NULL sein) zuerst, laengste zuerst ueber-
 * geben. Rueckgabe: Anzahl Ersetzungen. */
int sb_diag_sanitize(char *s, size_t cap, const sb_diag_root_t *roots, int nroots);
/* Nachpruefung: Vorkommen von Benutzername bzw. Profilpfad (soll 0). */
int sb_diag_clean_hits(const char *s);

/* Nutzervorgabe 15.11.: keine Eingaben in Berichten/Paketen. Entfernt Log-
 * Zeilen, die Eingaben protokollieren (Kennungen INPUT, INDIAG, AIMSTEP, XIN,
 * MOUSE, SBC, JOY, HOTAS, KEY, TAP, PAD, BIND, INREC am Anfang der Meldung).
 * In place; Rueckgabe: Anzahl entfernter Zeilen. */
int sb_diag_drop_input_lines(char *s);
/* 1 = Meldung (ohne "[t] [LEVEL] [Txxxx] "-Kopf) gilt als Eingabe-Zeile. */
int sb_diag_is_input_line(const char *line);

/* Pack-Pruefsumme (s. oben) als 64 Hex; path = ACP (wie sb_pack_scan).
 * Rueckgabe 1 = .sbpack/ZIP, 2 = Ordner (manifest.json), 0 = nicht lesbar. */
int sb_diag_pack_fp(const char *path, char *hex, size_t cap);

/* cfg-Abweichungen als Zeilen "key=value  (quelle)\r\n" in out. base_cfg /
 * user_cfg = Pfade (UTF-8, duerfen NULL sein), set_chain = "k=v;k=v" (darf NULL
 * sein). Pfade UTF-8. Rueckgabe: Anzahl Zeilen (-1 = base_cfg unlesbar, Liste trotzdem). */
int sb_diag_cfg_diff(const char *base_cfg, const char *user_cfg, const char *set_chain, char *out,
                     size_t cap);

/* Absturzbericht-Ordner */
#define SB_CRASH_KIND_CRASH 0
#define SB_CRASH_KIND_HANG 1
typedef struct {
    char name[64];      /* Ordnername, z. B. 20261115-071503-crash */
    int kind;           /* SB_CRASH_KIND_* */
    int fatal;          /* status.txt = fatal */
    int recovered;      /* recovered.txt vorhanden */
    int has_dump;       /* *.dmp vorhanden */
    unsigned long long bytes; /* Summe der Dateien */
} sb_crash_ent_t;
/* Ordner unter crash_dir (UTF-8), neueste zuerst. Rueckgabe: Anzahl (cap
 * deckelt), 0 = keiner/fehlt. */
int sb_diag_crash_list(const char *crash_dir, sb_crash_ent_t *out, int cap);
/* 1 = zaehlt als Absturz/Haenger der Sitzung (fatal oder ohne recovered). */
int sb_diag_crash_counts(const sb_crash_ent_t *e);
/* Gueltiger Ordnername (JJJJMMTT-hhmmss[-nnn]-crash|hang)? */
int sb_diag_crash_name_ok(const char *name);

/* Minidump (UTF-8-Pfad) in place bereinigen. Rueckgabe: Ersetzungen,
 * -1 = kein Minidump/unlesbar. */
int sb_diag_dump_scrub(const char *dump_u8);

#ifdef __cplusplus
}
#endif
