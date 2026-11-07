#pragma once
/* sb_userdata.h - W150 USER-DATA: zentrale Pfadfunktion fuer alles, was
 * Client und Launcher schreiben (COMMUNITY_KONZEPT §2.4).
 *
 * Modi:
 *   legacy   - bisherige Orte (user.cfg neben sb_native.cfg, HDD relativ,
 *              packs unter remaster_dir, shadercache neben der Exe, Logs in
 *              SB_OUT_DIR/out). Standard der Private Edition und aller Tests:
 *              sb_userdata_path() liefert 0, der Aufrufer bleibt beim alten
 *              Code (bitgleich).
 *   profile  - %LOCALAPPDATA%\SB-Preservation\ (Arbeitsname, Umbenennung bei
 *              RENAME-FINAL). Standard der Community Edition.
 *   portable - Programmordner (Ordner der Exe), ausgeloest durch eine Datei
 *              portable.txt neben der Exe.
 * Auswahl (erste Regel gewinnt): Umgebung SB_USERDATA=legacy|profile|portable
 * (Schalter fuer Tests/Launcher) -> portable.txt neben der Exe -> Edition.
 * SB_USERDATA_ROOT=<ordner> ersetzt die Wurzel im Modus profile (Tests, damit
 * kein Testlauf das echte Profil beschreibt).
 *
 * Unterordner/Dateien der Wurzel: user.cfg, hdd\, packs\, shadercache\,
 * logs\, crash\, Screenshots\ (W195). */
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SB_USERDATA_APPNAME "Steelbound" /* PO 7.11.: nutzersichtbar = Projektname (vorher SB-Preservation) */

typedef enum { SB_UD_LEGACY = 0, SB_UD_PROFILE = 1, SB_UD_PORTABLE = 2 } sb_ud_mode_t;

typedef enum {
    SB_UD_ROOT = 0,
    SB_UD_USERCFG,     /* Datei user.cfg */
    SB_UD_HDD,         /* Spielstaende (E:, TDATA/UDATA, ZCACHE) */
    SB_UD_PACKS,       /* installierte Pakete + packs.json */
    SB_UD_SHADERCACHE,
    SB_UD_LOGS,
    SB_UD_CRASH,
    SB_UD_SCREENSHOTS  /* W195 PHOTO-MODE: Fotos des Fotomodus */
} sb_ud_kind_t;

sb_ud_mode_t sb_userdata_mode(void);
const char *sb_userdata_mode_name(sb_ud_mode_t m);
/* Herkunft der Moduswahl ("SB_USERDATA", "portable.txt", "Edition"). */
const char *sb_userdata_mode_source(void);

/* 1 = neuer Ort in out (Ordner werden angelegt), 0 = legacy (Aufrufer
 * nutzt seinen bisherigen Pfad), -1 = Fehler (Wurzel nicht bestimmbar). */
int sb_userdata_path(sb_ud_kind_t k, char *out, size_t cap);

/* Wie sb_userdata_path, legt aber nichts an (Anzeige im Launcher). */
int sb_userdata_path_peek(sb_ud_kind_t k, char *out, size_t cap);

/* Modus neu bestimmen (nach Aenderung der Umgebung; Selbsttest). */
void sb_userdata_reset(void);

/* Selbsttest (Modusregeln, Pfade, Anlegen in einem Temp-Ordner). 0 = ok. */
int sb_userdata_selftest(char *msg, size_t cap);

#ifdef __cplusplus
}
#endif
