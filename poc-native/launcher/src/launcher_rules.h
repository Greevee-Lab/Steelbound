#pragma once
/* launcher_rules.h - W171 LAUNCHER-P2P3: Regelwerk der Oberflaeche
 * (UX-Konzept W169, Abschnitt 5, Regeln R1-R30).
 *
 * Reine Logik ohne Fenster: Eingabe = Einstellungen + Laufzeitwissen
 * (lr_env_t), Ausgabe = je Regel "greift" / "sperrt das Ziel" / Stufe /
 * Begruendungstext in der aktiven Sprache (lr_res_t). Die Oberflaeche
 * (launcher_gui.c, ui_apply_rules) ordnet jeder Regel ihre Steuerelemente
 * und die Stelle der Begruendungszeile zu. Damit ist jede Regel einzeln im
 * Selbsttest pruefbar (lrules_selftest: Fixture -> erwarteter Zustand).
 *
 * Muster (Konzept 5.1): A = grau mit Grund, B = Hinweiszeile,
 * C = automatisch umschalten (Statuszeile + Rueckgaengig), D = durch Pack
 * gesteuert (Schloss), E = Startsperre, I = feste Info. */
#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include "launcher_cfg.h"

enum { LR_PAT_A = 1, LR_PAT_B, LR_PAT_C, LR_PAT_D, LR_PAT_E, LR_PAT_I };
/* Stufe der Begruendung (Farbe in der Oberflaeche). */
enum { LR_LVL_INFO = 0, LR_LVL_WARN = 1, LR_LVL_ERR = 2, LR_LVL_LOCK = 3 };

#define LR_FIRST 1
#define LR_LAST 33 /* W172: + R31 Pack-Grenze, R32 Pack Creator fehlt; W195: + R33 Fotoordner */
#define LR_N 34 /* Index 1..33 = R1..R33, 0 unbenutzt */

typedef struct {
    const launcher_settings_t *s; /* Oberflaechen-Stand (ui_to_settings) */
    /* Spieldateien (R1-R3, R22) */
    int data_ok;            /* 1 = Abbild bzw. Ordner gueltig (lcfg_validate, nur Pfade) */
    char data_err[300];     /* Fehlertext der Pruefung (aktive Sprache) */
    int xbe_auto;           /* 1 = Programmdatei = <Spielordner>/default.xbe */
    int game_for_check;     /* 1 = XBE fuer den Versionsabgleich vorhanden */
    /* Client, VR, Controller (R4, R5, R25) */
    int has_ldr;
    int vr_runtime;
    int sbc_found;          /* 1 = gefunden, 0 = nicht gefunden, -1 = ungeprueft */
    /* Anzeige (R7, R10, R11) */
    int display_hz;         /* 0 = unbekannt */
    unsigned mon_w, mon_h;  /* Monitoraufloesung (Vollbild) */
    /* Packs (R17-R22, R29, R30) */
    int n_packs;
    int tex_packs;          /* aktive Textur-Packs (OK/Warnung) */
    int legacy_tex;         /* 1 = remaster/textures/x2|x4|manual vorhanden */
    char fx_pack[96];       /* Name des aktiven Effekt-Packs ("" = keins) */
    lgp_mask_t gfx_lock;    /* W173: Preset-Schluessel, die aktive Effect Packs setzen (R14) */
    int pk_level;           /* packs.json level: 0 = Original, 2, 4 */
    int pk_sel;             /* gewaehlter Eintrag (-1 = keiner) */
    int pk_sel_rejected;    /* 1 = gewaehlter Eintrag abgelehnt */
    int pk_sel_enabled;
    char pk_sel_reason[96]; /* Ablehnungsgrund */
    /* Gespeicherte Einstellungen (R28) */
    char default_setup[64]; /* profiles/standard.txt ("" = keines) */
    /* W172: Pack Creator neben dem Launcher (R32) */
    int has_creator;
    /* W195 PHOTO-MODE: eingestellter Fotoordner nutzbar (absolut, vorhanden oder anlegbar) (R33) */
    int photo_dir_ok;
} lr_env_t;

typedef struct {
    int active;     /* 1 = Regel greift (Begruendung sichtbar) */
    int disable;    /* 1 = Ziel(e) gesperrt (Muster A/D/E) */
    int level;      /* LR_LVL_* */
    char text[320]; /* Begruendung (UTF-8, aktive Sprache) */
    char text2[320];/* Zweittext (R20: Hinweis an der Voreinstellung,
                     * R26: Hilfe-Variante) */
} lr_res_t;

/* Muster der Regel (LR_PAT_*), 0 = keine Regel. */
int lrules_pattern(int rule);
/* Kurzbeschreibung (ASCII, Test/Doku). */
const char *lrules_name(int rule);
/* Alle Regeln auswerten. out[0] bleibt leer. */
void lrules_eval(const lr_env_t *e, lr_res_t out[LR_N]);
/* Fester Umgebungs-Standard fuer Tests (alles vorhanden, nichts aktiv). */
void lrules_env_default(lr_env_t *e, const launcher_settings_t *s);
/* Fixture je Regel: v = 1 -> Regel greift, v = 0 -> greift nicht (auf
 * lcfg_defaults + lrules_env_default anwenden; auch fuer den UI-Test). */
void lrules_fixture(int rule, int v, launcher_settings_t *s, lr_env_t *e);
/* 1 = greifende Regel sperrt ihr Ziel (Muster A/D/E). */
int lrules_locks(int rule);
/* 1 = Regel greift immer (feste Info/Hinweis, kein "greift nicht"). */
int lrules_always(int rule);
/* Selbsttest je Regel (Fixture -> erwarteter Zustand). */
void lrules_selftest(int *pass, int *fail);

#ifdef __cplusplus
}
#endif
