#pragma once
/* launcher_alpha.h - W272 ALPHA-TEST: Testwerkzeuge der Alpha (Seite
 * "Alpha Test" im Launcher, CLI --alpha-*). Nicht Teil des Originalspiels.
 *  - VT-Liste aus den Spieldaten (Media/EngData/ENGDATnn.ENG, Abbild ueber
 *    iso_vfs oder Spielordner), Cockpit-Generation = u32 an Offset 0xBC.
 *  - Gemeinsame Pruefung Mission/VT/Schwierigkeit (UI + CLI): Ergebnis =
 *    Sprachschluessel + naechstgueltige Wahl.
 *  - Teststart in einem frischen, isolierten Testordner
 *    <Datenordner>\alpha_test\HDD (+ HDD_US); der echte Spielstand-Ordner
 *    wird nie gelesen/geschrieben (Schutz: gleich/innen/enthaelt -> Abbruch,
 *    Loeschen folgt keinen Reparse-Points).
 *  - A/B-Schalter der Zwischenbild-Fixes (interp_*) in user.cfg.
 * Zeichenketten UTF-8, Pfade UTF-8 (Windows-Aufrufe ueber UTF-16). */
#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include "launcher_cfg.h"

#define LA_MISSION_MAX 23 /* Spiel-Nummer mno 0..23 ("MISSION %02d", 0 = Training) */
#define LA_VT_MAX 22      /* Modell 1..22 (0 = Platzhalter, 12 fehlt im Spiel) */
#define LA_DIFF_MAX 5     /* 0 Rookie .. 5 Armageddon, -1 = Spielvorgabe */

typedef struct {
    int mission;    /* 0..23 */
    int vt;         /* -1 = Vorgabe der Mission, sonst Modell 1..22 */
    int diff;       /* -1 = Spielvorgabe, 0..5 */
    int skip_intro; /* 0/1 */
} la_sel_t;

typedef struct {
    int present;   /* ENGDATnn.ENG vorhanden */
    int gen;       /* Feld 0xBC (0/1/2 = Cockpit-Generation 1/2/3), -1 unbekannt */
    unsigned size; /* Dateigroesse (268 normal, 240 = reduzierte Daten) */
} la_vt_t;

enum { LA_SRC_FALLBACK = 0, LA_SRC_IMAGE = 1, LA_SRC_FOLDER = 2 };

typedef struct {
    la_vt_t m[LA_VT_MAX + 1]; /* Index = Modell; 0 wird nie angeboten */
    int src;                  /* LA_SRC_* */
    int n;                    /* angebotene Modelle (1..22 vorhanden) */
} la_vtlist_t;

#define LA_VT_FULL_SIZE 268u

/* Name des VT-Modells (feste Tabelle, Index = Modell), "" ausserhalb 1..22. */
const char *la_vt_name(int model);
/* Name der Schwierigkeit (Spielbegriff, nicht uebersetzt), NULL fuer -1/ungueltig. */
const char *la_diff_name(int diff);
/* Feste Tabelle (gemessene EU-Werte), src = LA_SRC_FALLBACK. */
void la_vt_fallback(la_vtlist_t *l);
/* Eine ENGDAT-Datei auswerten (rein). */
void la_vt_parse(const uint8_t *data, uint32_t n, la_vt_t *out);
/* Spielordner (enthaelt Media\) bzw. Abbild lesen. 1 = mind. ein Modell. */
int la_vt_scan_folder(const char *game_root_u8, la_vtlist_t *l);
int la_vt_scan_image(const char *image_u8, la_vtlist_t *l);
/* Eingestelltes Spiel (Abbild > Ordner); nicht lesbar -> feste Tabelle. */
void la_vt_scan(const launcher_settings_t *s, la_vtlist_t *l);

/* Pruefung (UI + CLI). 0 = gueltig; 1 = ungueltig: key = Sprachschluessel des
 * ersten Fehlers, suggest = naechstgueltige Wahl (alle Felder korrigiert),
 * alt_vt = zweiter VT-Vorschlag (0 = keiner, z. B. 12 -> 11 oder 13). */
typedef struct {
    const char *key;
    int bad_mission, bad_vt, bad_diff;
    la_sel_t suggest;
    int alt_vt;
} la_check_t;
int la_check(const la_sel_t *sel, const la_vtlist_t *l, la_check_t *res);
/* Meldungstext (alle Fehler + Vorschlag, Sprache aktiv), UTF-8. */
void la_check_text(const la_sel_t *sel, const la_check_t *res, char *out, size_t cap);
/* CLI-Vorschlag: "--alpha-mission N --alpha-vt V --alpha-diff D". */
void la_suggest_cli(const la_check_t *res, char *out, size_t cap);

/* Zusatz-Kette fuer do_start_ex (nur dieser Start, user.cfg bleibt):
 * "mission_autostart=<m>;mission_vt=<v>;mission_difficulty=<d>;
 *  mission_skip_intro=<0|1>;hdd_root=<eu>;hdd_root_us=<us>"
 * Pfade UTF-8 (die Kette geht ueber UTF-16 an den Client, der sie per
 * GetCommandLineA in der ANSI-Codepage liest); ';' wird als "\;" maskiert.
 * 0 = ok, 1 = Pfad nicht nutzbar ('"' oder ausserhalb der ANSI-Codepage),
 * 2 = zu lang. */
int la_extra_args(const la_sel_t *sel, const char *hdd_eu_u8, const char *hdd_us_u8,
                  char *out, size_t cap);

/* Nutzer-Datenordner wie der Launcher (USER-DATA W150: Profil/portabel =
 * Datenordner, legacy = Client-Ordner). */
void la_data_root(const char *client_dir, char *out, size_t cap);
/* <root>\alpha_test, ...\HDD, ...\HDD_US (je cap Bytes, darf NULL sein). */
void la_test_dirs(const char *data_root, char *root, char *eu, char *us, size_t cap);
/* Lage zweier Ordner (normalisiert, Gross/Klein egal; zusaetzlich echte
 * Zielpfade, falls beide existieren): 0 getrennt, 1 gleich, 2 a liegt in b,
 * 3 a enthaelt b. */
int la_path_rel(const char *a_u8, const char *b_u8);
/* Testordner frisch anlegen: Schutz gegen jeden echten Spielstand-Ordner
 * (real[0..nreal-1]), alten Inhalt nur innerhalb test_root loeschen (keine
 * Reparse-Points folgen; ist test_root selbst einer, wird nur die
 * Verknuepfung entfernt), dann test_root\HDD + \HDD_US leer anlegen.
 * dry = 1: nur Schutzpruefung. 0 = ok, sonst *err_key (Sprachschluessel). */
int la_prepare(const char *test_root_u8, const char *const *real_u8, int nreal, int dry,
               const char **err_key);

/* A/B: Wert eines Client-Schluessels (user.cfg gewinnt, letzte Zeile gilt,
 * sonst sb_native.cfg, sonst dflt). */
int la_cfg_get_int(const char *user_cfg, const char *native_cfg, const char *key, int dflt);
/* Schluessel in user.cfg setzen (erste Zeile ersetzen, Dubletten entfernen,
 * sonst unter [render] anhaengen; andere Zeilen bleiben). 0 = ok. */
int la_cfg_set(const char *path, const char *key, const char *val);
/* Kennt der Client den Schluessel? Zeile "key=" in sb_native.cfg oder das
 * Literal "key\0" in sb_emu.dll (der Parser vergleicht per strcmp). */
int la_client_knows_key(const char *client_dir, const char *key);

/* CLI: nur pruefen (VT-Liste des Spiels). 0 = gueltig, 3 = ungueltig
 * (Meldung + Vorschlag auf stdout). */
int la_cli_check(const launcher_settings_t *s, const la_sel_t *sel);
/* CLI --start --alpha-mission ...: pruefen, Testordner, Kette an set_args.
 * game_s = effektive Einstellungen (Spiel, hdd_root), client_dir normiert.
 * 0 = ok, 1 = Fehler, 3 = ungueltige Kombination (Meldung auf stdout). */
int la_cli_apply(const char *client_dir, const launcher_settings_t *s, const la_sel_t *sel,
                 int dry, char *set_args, size_t cap);

/* Selbsttest (rein, Temp-Ordner): VT-Liste, Pruefung, Kette, Testordner. */
void la_selftest(int *pass, int *fail);

#ifdef __cplusplus
}
#endif
