#pragma once
/* launcher_diag.h - W279 DIAG-CRASH: Launcher-Seite der Absturzberichte und das
 * Diagnosepaket (Logik ohne Fenster; Oberflaeche launcher_diag.inc).
 *
 *  - Beim Start: neuer Absturz-/Haenger-Bericht seit dem letzten Hinweis?
 *    (<crash>\last_seen.txt = zuletzt gemeldeter Ordner) -> Karte "The last
 *    session crashed" einmal, mit "Open report folder", "Report on GitHub"
 *    (ZIP des Berichts + vorbefuellte Issue-Seite, Anhang zieht der Spieler
 *    selbst hinein) und "Dismiss".
 *  - "Create diagnostic package": ZIP <Datenordner>\diagnostics\steelbound-
 *    diag-<zeit>.zip mit Systeminfo, Versionen, cfg-Abweichungen, Packs, den
 *    letzten 3 Client-Logs, den letzten 3 Absturzberichten und dem
 *    Belegungsprofil; keine Spielstaende, keine Eingaben, alles bereinigt
 *    (<data>/<client>, Benutzer-/Rechnername, Pfade).
 * Nichts wird hochgeladen. Pfade UTF-8. */
#include <stddef.h>
#include "sb_diagcore.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Ordner wie der Client: Profil/portabel = Datenordner\crash bzw. \logs,
 * legacy = <Client>\out\crash bzw. <Client>\out. data = Datenordner
 * (legacy: Client-Ordner). Jeder Ausgang darf NULL sein. */
void ld_dirs(const char *client_dir, char *crash, char *logs, char *data, size_t cap);
/* Test: feste Ordner statt ld_dirs (NULL = aus). */
void ld_test_dirs(const char *crash, const char *logs, const char *data);

/* Neuer Bericht seit dem letzten Hinweis: 1 + *e (neuester zaehlender). */
int ld_crash_new(const char *crash_dir, sb_crash_ent_t *e);
/* Hinweis gezeigt: last_seen.txt = name. 1 = ok. */
int ld_crash_mark_seen(const char *crash_dir, const char *name);
/* Kurzzeile aus info.txt ("reason" / "exception"). */
void ld_crash_summary(const char *crash_dir, const char *name, char *out, size_t cap);
/* ZIP des Berichtsordners neben dem Ordner (<crash>\<name>.zip). 1 = ok. */
int ld_crash_zip(const char *crash_dir, const char *name, char *zip, size_t cap);
/* Vorbefuellte "New issue"-Seite (bug_report.yml), keine Anhaenge. 1 = ok. */
int ld_issue_url(const char *crash_dir, const char *name, const char *zipname, char *url, size_t cap);
/* Nur https://github.com/<repo>/issues/new?... mit kodierten Zeichen. */
int ld_url_ok(const char *url);

typedef struct {
    char zip[1100];   /* fertige ZIP (UTF-8) */
    char dir[1100];   /* Ordner der ZIP */
    int files;
    unsigned long long bytes;
    int logs, crashes, profile;
    int repl;         /* Ersetzungen beim Bereinigen */
    int dropped;      /* entfernte Eingabe-Zeilen */
    int clean_hits;   /* Nachpruefung Benutzername/Profilpfad (soll 0) */
} ld_pkg_t;
/* Diagnosepaket bauen (packs_json UTF-8, darf NULL sein). 1 = ok. */
int ld_pkg_build(const char *client_dir, const char *packs_json, ld_pkg_t *r);
/* Systeminfo (OS, CPU, RAM, GPU+Treiber, Monitore) als Text. */
void ld_sysinfo(char *out, size_t cap);

/* Selbsttests (reine Logik, Temp-Ordner): Erkennung + einmalig, Paket-Inhalt,
 * Bereinigung (Testpfade mit Benutzername), Eingabe-Zeilen, URL. */
void ld_selftest(int *pass, int *fail);

#ifdef __cplusplus
}
#endif
