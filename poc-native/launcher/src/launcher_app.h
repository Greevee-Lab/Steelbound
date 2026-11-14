#pragma once
/* launcher_app.h - Schnittstelle CLI (launcher.c) <-> Oberflaeche
 * (launcher_gui.c), W82 LAUNCHER-UI2. Zeichenketten UTF-8. */
#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <windows.h> /* W217: HMODULE */
#include "launcher_cfg.h"

/* Konsolen-Ausgabe (UTF-8 -> WriteConsoleW bzw. UTF-8-Bytes in Pipes). */
int cli_printf(const char *fmt, ...);

/* Effektive Einstellungen: Defaults -> sb_native.cfg -> user.cfg ->
 * Standard-Profil. user_info (darf NULL sein) beschreibt die Lage.
 * kind_out (darf NULL sein): 0 = Standardwerte (keine user.cfg),
 * 1 = user.cfg, 2 = Standard-Profil. lang_override (darf NULL sein):
 * "en"/"de" gewinnt ueber die cfg (CLI --lang, Shots). Stellt zugleich
 * die aktive sb_lang-Sprache (fuer user_info/Fehltexte). */
void app_load_effective(const char *client_dir, launcher_settings_t *s,
                        char *user_info, size_t user_cap, int *kind_out,
                        const char *lang_override);
/* Client starten (CWD = Client-Ordner). 0 = ok (msg = "Client gestartet
 * (PID n)."), 1 = Fehler (msg = Text). */
int app_launch_client(const char *client_dir, const char *xbe_path,
                      char *msg, size_t cap);
/* W159: Start mit --set-Kette des Startwegs (Einzelstart-Override,
 * user.cfg bleibt unveraendert). set_args darf NULL/leer sein. */
int app_launch_client_set(const char *client_dir, const char *xbe_path,
                          const char *set_args, char *msg, size_t cap);
/* W231: wie app_launch_client_set, liefert zusaetzlich den Prozess-Handle
 * (proc != NULL; Aufrufer schliesst ihn) fuer die Exitcode-Auswertung. */
int app_launch_client_proc(const char *client_dir, const char *xbe_path,
                           const char *set_args, char *msg, size_t cap,
                           void **proc);
/* W172/P7 + W231: Spielstand-Grundordner wie der Client (UTF-8). */
void app_hdd_base(const char *client_dir, const char *hdd_root, char *out, size_t cap);
/* W231 US-LAUNCHER: --set-Zusatz der Spielfassung (US: allow_ntsc_u=1 solange
 * experimental, hdd_root=<Saves>\us). 0 = ok, 1 = Fehler (Text in err). */
int app_edition_start_args(const char *client_dir, const launcher_settings_t *s,
                           const lcfg_gameinfo_t *gi, char *set_args, size_t cap,
                           char *err, size_t errcap);
/* W159: Start-Kommandozeile bauen (rein, testbar). */
void app_client_cmdline(const char *client_dir, const char *xbe_path,
                        const char *set_args, char *out, size_t cap);
/* W159: --set-Kette je Startweg (rein, testbar): vr=0 -> "vr_enable=0",
 * vr=1 -> "vr_enable=1;vr_preview=<vorschau>" (leer -> "left"). */
void app_vr_set_args(int vr, const char *vr_preview, char *out, size_t cap);
/* W159: OpenXR-Runtime vorhanden? 1 = ja (Manifest-Pfad in path_out),
 * 0 = nein. Nur Registry lesen + Datei-Existenz, nichts laden/aendern.
 * SB_TEST_XR_ABSENT=1 erzwingt 0 (Test-Haken). */
int app_xr_runtime_present(char *path_out, size_t cap);

/* W217 FIRST-RUN: Standardwerte beim allerersten Start (keine user.cfg,
 * kein Standard-Profil; rein, testbar). langid = GetUserDefaultUILanguage()
 * (Community: Deutsch -> "de", sonst bleibt "en"); work_w/work_h = Arbeits-
 * flaeche des Hauptmonitors (0 = unbekannt): passt das Fenster samt Rahmen
 * nicht hinein, wird "randlos" gewaehlt (window_mode=1). Rueckgabe: Bitmaske
 * 1 = Sprache gesetzt, 2 = randlos gewaehlt. */
int app_firstrun_defaults(launcher_settings_t *s, unsigned langid, int work_w,
                          int work_h);
/* W217: Importiert das Modul (Datei oder geladenes Modul) die DLL `dll`?
 * 1 = ja, 0 = nein, -1 = nicht lesbar. Liest nur die Importtabelle. */
int app_pe_imports(const wchar_t *file, HMODULE mod, const char *dll);
/* W217: Fehlt eine Laufzeit-DLL, die sb_emu.dll im Client-Ordner braucht?
 * Prueft die VC++-Laufzeit (VCRUNTIME140.dll) nur, wenn sb_emu.dll sie
 * importiert. 0 = alles da; 1 = fehlt (Name in missing). probe_name (darf
 * NULL sein) ersetzt den gesuchten DLL-Namen (Selbsttest). */
int app_client_runtime_missing(const char *client_dir, const char *probe_name,
                               char *missing, size_t cap);

/* Oberflaeche. shot: 0 = normal, 1 = --shot (BMP, alt), 2 = --ui-shot (PNG),
 * 3 = --bind-shot, 4 = --gui-start-dry (GUI-Startpfad ohne Clientstart),
 * 5 = --assist-shot (W143, Assistent offscreen), 6 = --joy-shot (W221,
 * Joystick/HOTAS-Dialog offscreen mit Beispieldaten). */
typedef struct {
    int shot;
    int page;          /* -1 = alle, sonst Seite */
    char out_dir[1024];
    int force_dpi;     /* 0 = Monitor, sonst z. B. 144 (150 %) */
    int bg;            /* -1 = Einstellung, 0 = aus, 1 = an */
    int bg_both;       /* --ui-shot: je Seite mit und ohne Hintergrund */
    char lang[8];      /* W142: --lang en|de (leer = aus cfg) */
    char assist_mode[16];      /* W143: shot=5 Modus */
    char assist_fixture[1024]; /* W143: shot=5 Fixture (leer = Geraet/keine) */
    int bind_joydemo;          /* W226: --bind-shot=joy (Beispiel-HOTAS) */
    int bind_learn;            /* W228: --bind-learn (Lernmodus im Bild) */
    int bind_sel;              /* W159: shot=3 Element-Index vorwaehlen + in
                                * Sicht scrollen (-1 = Standard: erstes) */
    char state[32];            /* W171: --ui-shot --state NAME (Regelzustand,
                                * nur im Speicher; leer = keiner) */
} gui_opts_t;

int gui_main(const char *client_override, const gui_opts_t *o);
/* W171 LAUNCHER-P2P3: Seiten (7) fuer CLI/Tests. gui_page_from_name kennt
 * die neuen Namen (play|display|graphics|packs|controls|audio|advanced),
 * die alten --ui-shot-Namen als Aliase (start, grafik, remaster, ton,
 * steuerung, profile, pfade) und Ziffern 0..6. -1 = unbekannt. */
int gui_page_count(void);
const char *gui_page_key(int p);
int gui_page_from_name(const char *v);
/* W171: --state-Name bekannt? 1 = ja. */
int gui_state_known(const char *name);

/* Selbsttest-Faelle der Oberflaeche (Fenster unsichtbar, nie gezeigt).
 * Meldet PASS/FAIL per cli_printf, zaehlt in *pass/*fail. */
void gui_selftest(const char *client_override, int *pass, int *fail);
/* W148 PACK-LAUNCHER: Selbsttest Packs-Seite + Render-Skalierung (launcher_packtest.c). */
void packs_selftest(int *pass, int *fail);

#ifdef __cplusplus
}
#endif
