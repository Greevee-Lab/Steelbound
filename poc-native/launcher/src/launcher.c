#include "launcher_saves.h"
/* launcher.c - Startanwendung (A10, W26/W47/W78/W82): CLI, Selbsttest,
 * Start des Clients. Die Oberflaeche steht seit W82 in launcher_gui.c
 * (W-API, randlos, Hintergrundbild). CLI: --selftest, --check,
 * --write-user-cfg, --start [--dry-run] [--vr], --profile NAME, --list-profiles,
 * --set k=v;... [--client-dir D], --ui-shot, --bind-shot, --gui-start-dry.
 * Quelltext ASCII; Zeichenketten intern UTF-8 (launcher_u8.h), Nutzertexte
 * deutsch mit echten Umlauten. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "launcher_cfg.h"
#include "sb_gfxpreset.h" /* W173: Selbsttest 52 (gleiche Tabelle wie der Client) */
#include "sb_fpsrate.h" /* W223: Selbsttest FPS-RATES */
#include "launcher_ctrl.h"
#include "launcher_u8.h"
#include "launcher_app.h"
#include "launcher_rules.h" /* W171 */
#include "sb_lang.h" /* W142 L10N */
#include "launcher_assist.h" /* W143 Assistent-cfg */
#include "sb_edition.h" /* W116 EDITIONS */
#include "sb_userdata.h" /* W150 USER-DATA */
#include "sb_gamever.h" /* W231 US-LAUNCHER: Selbsttest */
#include "launcher_update.h" /* W250 UPDATE-CHECK */
#include "launcher_alpha.h" /* W272 ALPHA-TEST: Teststart Mission + VT */
#include "launcher_games.h" /* W276 LOC-LAUNCH */
#include "launcher_diag.h" /* W279 DIAG-CRASH */
#include "launcher_orig.h" /* W278 ORIG-FLAG */
#include "launcher_layout.h" /* W281 LAUNCHER-SIZE */
#include "launcher_account.h" /* W328 STEELNET-ACCOUNT */
#if defined(SB_HAVE_LOCTP) && SB_HAVE_LOCTP
#include "launcher_loctp.h" /* W336 LOC-TESTPARTNER (nur interne Ausgabe) */
#endif

static void dpi_init(void)
{
    /* Per-Monitor-DPI, ohne Manifest: erst V2-Kontext versuchen, dann
     * systemweit. Alles Best-Effort (alte Windows: 96 dpi). */
    HMODULE u32 = GetModuleHandleA("user32.dll");
    if (u32) {
        typedef BOOL(WINAPI * CtxFn)(DPI_AWARENESS_CONTEXT);
        typedef UINT(WINAPI * DpiFn)(HWND);
        CtxFn set_ctx = (CtxFn)GetProcAddress(
            u32, "SetProcessDpiAwarenessContext");
        if (set_ctx) {
            set_ctx((DPI_AWARENESS_CONTEXT)-4); /* PER_MONITOR_AWARE_V2 */
        } else {
            typedef BOOL(WINAPI * AwareFn)(void);
            AwareFn aware =
                (AwareFn)GetProcAddress(u32, "SetProcessDPIAware");
            if (aware) {
                aware();
            }
        }
        (void)sizeof(DpiFn);
    }
}


/* ---------------- CLI (Tests/Automatisierung) ---------------- */

static HANDLE g_out;
static int g_out_console;

static void cli_console(void)
{
    /* GUI-Subsystem: Eltern-Konsole anzapfen, damit --selftest etc. in
     * PowerShell sichtbar sind (Doppelklick: kein Handle, egal). Ein
     * geerbtes Handle (Pipe/Datei) hat Vorrang. */
    DWORD mode;
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!g_out || g_out == INVALID_HANDLE_VALUE) {
        AttachConsole(ATTACH_PARENT_PROCESS);
        g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    }
    if (g_out == INVALID_HANDLE_VALUE) {
        g_out = NULL;
    }
    g_out_console = g_out && GetConsoleMode(g_out, &mode);
}

/* W82: Ausgabe UTF-8. Konsole -> WriteConsoleW (Umlaute richtig),
 * Pipe/Datei -> UTF-8-Bytes mit CRLF (wie bisher im Textmodus). */
int cli_printf(const char *fmt, ...)
{
    static char buf[16384];
    static char crlf[32768];
    va_list ap;
    int n;
    DWORD wr;
    va_start(ap, fmt);
    n = _vsnprintf_s(buf, sizeof buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n < 0) {
        n = (int)strlen(buf);
    }
    if (!g_out) {
        return n;
    }
    if (g_out_console) {
        static wchar_t w[16384];
        int wn = u8_to_w(buf, w, 16384);
        WriteConsoleW(g_out, w, (DWORD)wn, &wr, NULL);
    } else {
        int i, o = 0;
        for (i = 0; buf[i] && o < (int)sizeof crlf - 2; i++) {
            if (buf[i] == '\n' && (i == 0 || buf[i - 1] != '\r')) {
                crlf[o++] = '\r';
            }
            crlf[o++] = buf[i];
        }
        WriteFile(g_out, crlf, (DWORD)o, &wr, NULL);
    }
    return n;
}

#define printf cli_printf

static void cli_usage(void)
{
    printf("sb_launcher - Steel-Battalion-Launcher (A10, Welle 3)\n"
           "Ohne Argumente: Fenster. Mit Argumenten (Exit 0 = ok):\n"
           "  --selftest              alle CLI-Tests (Exit 0 = gr" U8_UE "n)\n"
           "  --steelnet AKTION ...   W328 Pilotenkonto (create|check|status|login|...; Test: SB_STEELNET_API/PIN, SB_ACCOUNT_DIR)\n"
           "  --check                 effektive Einstellungen + Validierung\n"
           "  --check-image PFAD      Spiel-Abbild pruefen (Version, lesbar)\n"
           "  --userdata              Datenorte (Modus, Pfade) + Selbsttest\n"
           "  --write-user-cfg        user.cfg schreiben (startet nicht)\n"
           "  --start                 user.cfg schreiben + Client starten\n"
           "  --dry-run               mit --start: nur schreiben, nicht starten\n"
           "  --vr                    mit --start: VR-Start (sonst Bildschirm)\n"
           "  --game sb|loc           mit --start: Spiel (loc = Line of Contact: noch\n"
           "                          gesperrt, Meldung + Exit 3, W276)\n"
           "  --wait                  mit --start: auf Client warten, Exitcode deuten\n"
           "  --replay ID             mit --start: gespeichertes Replay direkt abspielen (experimentell)\n"
           "  --replay-camera N       mit --replay: Kamera fest (0 Cockpit, 1 Vollbild, 2..6 au" U8_SZ "en)\n"
           "  --alpha-mission N       mit --start/--gui-start-dry: Teststart Mission N (0 Training, 1..23)\n"
           "  --alpha-vt V            dazu VT-Modell (1..22, -1 = Vorgabe der Mission)\n"
           "  --alpha-diff D          dazu Schwierigkeit (-1 Spielvorgabe, 0 Rookie..5 Armageddon)\n"
           "  --alpha-skip-intro      dazu Intro/Startsequenz " U8_UE "berspringen (wo m" U8_OE "glich)\n"
           "  --profile NAME          Profil statt user.cfg als Basis laden\n"
           "  --save-profile NAME     Profil aus Basis + --set speichern\n"
           "  --list-profiles         Profile + Standard-Profil auflisten\n"
           "  --set \"k=v;k=v\"         Einstellungen " U8_AE "ndern\n"
           "  --client-dir DIR        Client-Ordner (statt Suche)\n"
           "  --shot N|alle           Test: Seite N (0..6) als BMP sichern\n"
           "  --shot-out DIR          Ziel f" U8_UE "r --shot (Default: %%TEMP%%)\n"
           "  --write-user-profile R    Benutzerprofil user_R.ini schreiben\n"
           "  --controls-export OUT     W419: Steuerung (aktives Profil + Werte) als .sbcontrols\n"
           "  --controls-check DATEI    W419: .sbcontrols nur pruefen (nichts aendern)\n"
           "  --controls-fuzz N [--seed S]  W430: Mutations-Fuzzer fuer den Import (Standard 200000)\n"
           "                            (Basis --from B, --set-bind \"e=a;...\", --force)\n"
           "  --bind-shot[=AKTION]    Test: Belegungs-Dialog sichern (BMP+PNG)\n"
           "  --bind-learn            mit --bind-shot: Lernmodus im Bild (W228)\n"
           "  --joy-shot              Test: Joystick/HOTAS-Dialog sichern (PNG, Beispieldaten)\n"
           "  --ui-shot SEITE|alle    Test: Seiten als PNG (play, display, graphics, packs,\n"
           "                          controls, audio, advanced; alte Namen start, grafik,\n"
           "                          remaster, ton, steuerung, profile, pfade gelten\n"
           "                          weiter); Fenster immer au" U8_SZ "erhalb des\n"
           "                          Bildschirms, ohne Fokus\n"
           "  --state NAME            mit --ui-shot: Regelzustand (nodata, fullscreen,\n"
           "                          window43, controller, custom, diag, pack2, pack3,\n"
           "                          effectpack, ready, ready_controller,\n"
           "                          update_ask, update_new, update_dev, crash,\n"
           "                          loc_empty, loc_ok, loc_sb, loc_start = Bereich\n"
           "                          Line of Contact, W276, loc_exp = loc_ok mit\n"
           "                          Hinweisband, W293, loc_wide = loc_ok mit\n"
           "                          Bildformat 16:9, W326, loc_169 = LoC-Standard\n"
           "                          16:9 + Fade out, W414, advanced = Anzeige/Grafik\n"
           "                          mit Feinreglern, W291, fade = Breitbild\n"
           "                          Fade out mit Verlaufszeile, W296, fpslist/\n"
           "                          fpslist_auto = Bildratenliste offen, W309,\n"
           "                          loc_acc_none|unsec|ok|view|signin = Pilotenkonto, W328,\n"
           "                          loc_acc_view_unsec|view_login|view_ok = Account-Seite, W399,\n"
           "                          loc_acc_view_manage|view_net|signin_code|view_unsec_login =\n"
           "                          Kontoseite mit Aufklappern bzw. Server nicht erreichbar, W418,\n"
           "                          ready_hover/loc_hover = Maus auf dem inaktiven Spielnamen, W398),\n"
           "                          nur Anzeige (Packs-Ordner: Umgebung SB_PACKS_DIR)\n"
           "  --dpi N                 mit --ui-shot/--bind-shot: DPI erzwingen\n"
           "  --sim-monitors LISTE    W309: simulierte Monitore fuer --ui-shot/Tests,\n"
           "                          z. B. 165 oder 60/144,165 (erster = Startmonitor,\n"
           "                          /N = Hoechstrate)\n"
           "  --layout 0|a|b          W281 Entwurf: Layout (0 = heute, a = groesser,\n"
           "                          b = mehr Platz); sonst launcher_ui.cfg launcher_layout,\n"
           "                          Standard a/normal\n"
           "  --ui-size compact|normal|large  W281: UI-Groesse (a/b)\n"
           "  --work WxH              W281: Arbeitsflaeche in Pixeln (Einpassen testen)\n"
           "                          (144 = 150 %%)\n"
           "  --bg 0|1|beide          mit --ui-shot: Hintergrundbild\n"
           "  --lang en|de            Sprache (statt [ui] lang aus cfg)\n"
           "  --update-check          Test: einmal auf neue Version pr" U8_UE "fen (W250,\n"
           "                          --as-version V statt der eingebauten Version)\n"
           "  --diag-package          Diagnosepaket (ZIP im Datenordner, bereinigt, W279)\n"
           "  --gui-start-dry         Test: GUI-Startpfad (user.cfg +\n"
           "                          Spielerbetrieb), Client nicht starten\n");
}

static const char *cli_value(int argc, char **argv, const char *flag)
{
    int i;
    size_t n = strlen(flag);
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], flag) == 0 && i + 1 < argc) {
            return argv[i + 1];
        }
        if (strncmp(argv[i], flag, n) == 0 && argv[i][n] == '=') {
            return argv[i] + n + 1;
        }
    }
    return NULL;
}

/* W272: --alpha-* lesen (fehlt -> Standard, keine Zahl -> -9999 = ungueltig).
 * 1 = --alpha-mission angegeben (Teststart). */
static int cli_alpha(int argc, char **argv, la_sel_t *a)
{
    static const char *const k[3] = { "--alpha-mission", "--alpha-vt", "--alpha-diff" };
    int *dst[3], i, j, on = 0;
    dst[0] = &a->mission;
    dst[1] = &a->vt;
    dst[2] = &a->diff;
    a->mission = 0;
    a->vt = -1;
    a->diff = -1;
    a->skip_intro = 0;
    for (i = 0; i < 3; i++) {
        for (j = 1; j < argc; j++) {
            if (strcmp(argv[j], k[i]) == 0) {
                const char *v = j + 1 < argc ? argv[j + 1] : "";
                char *e = NULL;
                long n = strtol(v, &e, 10);
                *dst[i] = (v[0] && e && !*e && n > -1000 && n < 1000) ? (int)n : -9999;
                on |= i == 0;
            }
        }
    }
    for (j = 1; j < argc; j++) {
        if (strcmp(argv[j], "--alpha-skip-intro") == 0) {
            a->skip_intro = 1;
        }
    }
    return on;
}

static int cli_flag(int argc, char **argv, const char *flag)
{
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], flag) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Effektive Einstellungen: Defaults -> sb_native.cfg -> user.cfg (optional)
 * -> Standard-Profil (optional, gewinnt). user_info beschreibt die
 * user.cfg-Lage fuer die Statuszeile (darf NULL sein). */
void app_load_effective(const char *client_dir, launcher_settings_t *s,
                        char *user_info, size_t user_cap, int *kind_out,
                        const char *lang_override)
{
    char p[1200];
    int keys = 0, kind = 0;
    lcfg_defaults(s);
    _snprintf_s(p, sizeof p, _TRUNCATE, "%s/sb_native.cfg", client_dir);
    lcfg_load_file(p, s);
    {
        /* W87: sb_native.cfg ist testorientiert (mouse_mode=relative =
         * bitgenau alt). Spieler-Standard des Launchers bleibt absolut,
         * bis user.cfg oder ein Profil etwas anderes sagt. */
        launcher_settings_t d;
        lcfg_defaults(&d);
        s->mouse_mode = d.mouse_mode;
        /* W170 LAUNCHER-P1/B12: ebenso rt_scale (Testlaeufe fahren 1,
         * Spieler-Standard ist auto; user.cfg/Profil gewinnen danach). */
        s->rt_scale = d.rt_scale;
        s->rt_scale_dyn = d.rt_scale_dyn;
        /* W424: Testkette faehrt sb_allied_squads=0 (bitgleich), Spieler-Standard ist 1 */
        s->sb_allied_squads = d.sb_allied_squads;
    }
    lcfg_user_cfg_path(client_dir, p, sizeof p); /* W150 USER-DATA */
    s->gfx_preset_rev = 0; /* W296 PRESET-ORIG: Stand kommt aus user.cfg (fehlt = vor W296) */
    if (lcfg_load_file_count(p, s, &keys) == 0) {
        kind = 1;
        /* W250: eine user.cfg, die nur die Update-Antwort traegt (Frage beim
         * allerersten Start beantwortet, nie gespeichert), bleibt Erststart. */
        if (keys == 1 && lupd_cfg_get(p) >= 0) {
            kind = 0;
        }
    }
    lcfg_gfx_migrate_fps(s); /* W296: altes "Original - 120 FPS" -> Original (Monitor auto); jede andere Rate bleibt */
    {
        /* Standard-Profil gewinnt ueber user.cfg (W47). */
        char dflt[64], err[512];
        launcher_settings_t ps;
        if (lcfg_profile_get_default(client_dir, dflt, sizeof dflt) == 0 &&
            lcfg_profile_load(client_dir, dflt, &ps, err, sizeof err) == 0) {
            *s = ps;
            kind = 2;
        }
    }
    /* W465 LOC-ENHANCED: "Remastered dezent/stark" ist nicht mehr waehlbar - exakt diese Werte -> Original
     * (Benutzerdefiniert bleibt). Gespeichert wird das beim naechsten Speichern/Start. */
    (void)lcfg_gfx_migrate_remaster(s);
    /* W142: Sprache steht erst nach allen Ladungen fest (Hinweis danach). */
    if (lang_override && *lang_override) {
        strcpy_s(s->lang, sizeof s->lang,
                 _stricmp(lang_override, "de") == 0 ? "de" : "en");
    }
    sb_lang_set(s->lang);
    if (user_info) {
        if (kind == 2) {
            char dflt[64];
            if (lcfg_profile_get_default(client_dir, dflt, sizeof dflt) != 0) {
                dflt[0] = '\0';
            }
            _snprintf_s(user_info, user_cap, _TRUNCATE,
                        LT("app.profile_loaded"), dflt);
        } else if (kind == 1) {
            _snprintf_s(user_info, user_cap, _TRUNCATE,
                        LT("app.usercfg_loaded"), keys);
        } else {
            strncpy_s(user_info, user_cap, LT("app.no_usercfg"), _TRUNCATE);
        }
    }
    if (kind_out) {
        *kind_out = kind;
    }
}

#define load_effective app_load_effective

/* W217 FIRST-RUN: siehe launcher_app.h. */
int app_firstrun_defaults(launcher_settings_t *s, unsigned langid, int work_w,
                          int work_h)
{
    int r = 0;
#if SB_EDITION_COMMUNITY
    if ((langid & 0x3FF) == 0x07 /* LANG_GERMAN */) {
        strcpy_s(s->lang, sizeof s->lang, "de");
        r |= 1;
    }
#else
    (void)langid; /* private Edition: Erstsprache bleibt Deutsch */
#endif
    /* Client-Fenster = width x height Innenflaeche + Rahmen/Titel
     * (WS_OVERLAPPEDWINDOW, ca. 16 x 39 px bei 100 %). Passt das nicht in
     * die Arbeitsflaeche (z. B. 2560 x 1080 auf 1920 x 1080), ragt das
     * Fenster aus dem Bild -> randlos (Monitoraufloesung, Hor+). */
    if (s->window_mode == 0 && work_w > 0 && work_h > 0 &&
        (s->width + 16 > (unsigned)work_w || s->height + 39 > (unsigned)work_h)) {
        s->window_mode = 1;
        r |= 2;
    }
    return r;
}

/* W431 DEFAULT-BINDINGS: siehe launcher_app.h. */
int app_firstrun_input_profile(launcher_settings_t *s, const char *client_dir, int kind)
{
    char path[1200];
    if (kind != 0 || !s || s->input_profile[0]) {
        return 0;
    }
    if (lctrl_input_profile_path(client_dir, "default_full", path, sizeof path) != 0 ||
        u8_GetFileAttributes(path) == INVALID_FILE_ATTRIBUTES) {
        return 0;
    }
    strcpy_s(s->input_profile, sizeof s->input_profile, "default_full");
    return 1;
}

/* W217: Importtabelle eines PE-Abbilds (als Bild abgebildet) durchsuchen. */
static int pe_imports_mapped(const unsigned char *base, const char *dll)
{
    const IMAGE_DOS_HEADER *dh = (const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS32 *nh;
    const IMAGE_DATA_DIRECTORY *dd;
    const IMAGE_IMPORT_DESCRIPTOR *id;
    if (dh->e_magic != IMAGE_DOS_SIGNATURE) {
        return -1;
    }
    nh = (const IMAGE_NT_HEADERS32 *)(base + dh->e_lfanew);
    if (nh->Signature != IMAGE_NT_SIGNATURE ||
        nh->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        return -1;
    }
    dd = &nh->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dd->VirtualAddress || !dd->Size) {
        return 0;
    }
    for (id = (const IMAGE_IMPORT_DESCRIPTOR *)(base + dd->VirtualAddress);
         id->Name; id++) {
        if (_stricmp((const char *)(base + id->Name), dll) == 0) {
            return 1;
        }
    }
    return 0;
}

int app_pe_imports(const wchar_t *file, HMODULE mod, const char *dll)
{
    int r;
    HMODULE h = mod;
    if (!h) {
        /* Als Bild-Ressource abbilden: keine Ausfuehrung, keine Abhaengig-
         * keiten laden (geht auch, wenn VCRUNTIME140.dll fehlt). */
        h = LoadLibraryExW(file, NULL, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        if (!h) {
            return -1;
        }
    }
    /* Bei Ressourcen-Abbildung markieren die unteren Bits den Handle. */
    r = pe_imports_mapped((const unsigned char *)((ULONG_PTR)h & ~(ULONG_PTR)3), dll);
    if (!mod) {
        FreeLibrary(h);
    }
    return r;
}

int app_client_runtime_missing(const char *client_dir, const char *probe_name,
                               char *missing, size_t cap)
{
    static const char kVcrt[] = "VCRUNTIME140.dll";
    char emu[1200];
    wchar_t w[1200], wn[64];
    HMODULE h;
    if (missing && cap) {
        missing[0] = '\0';
    }
    _snprintf_s(emu, sizeof emu, _TRUNCATE, "%s/sb_emu.dll", client_dir);
    u8_to_w(emu, w, 1200);
    if (!probe_name && app_pe_imports(w, NULL, kVcrt) != 1) {
        return 0; /* sb_emu.dll fehlt/statisch gelinkt: nichts zu pruefen */
    }
    if (!probe_name) {
        probe_name = kVcrt;
    }
    u8_to_w(probe_name, wn, 64);
    /* Gleiche Suche wie beim Laden von sb_emu.dll: seit W332 SEC begrenzt
     * sb_ldr.exe den DLL-Suchpfad auf System32 (bzw. SysWOW64), der
     * Client-Ordner zaehlt nicht mehr (Schutz vor untergeschobenen DLLs).
     * Echtes Laden statt Datendatei: eine 64-Bit-Kopie zaehlt nicht. */
    h = LoadLibraryExW(wn, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (h) {
        FreeLibrary(h);
        return 0;
    }
    if (missing && cap) {
        strcpy_s(missing, cap, probe_name);
    }
    return 1;
}

#if SB_EDITION_COMMUNITY
/* W116 EDITIONS: Der Community-Build hat keine Standardpfade (sb_edition.h).
 * Der Selbsttest nimmt das eigene Spiel-Image aus SB_TEST_XBE/SB_TEST_MEDIA;
 * ohne sie scheitern die Validierungs-Tests (4/5/8/8b/15: "Kein XBE-Pfad").
 * Private Edition: unveraendert (lcfg_defaults). */
static void selftest_defaults(launcher_settings_t *s)
{
    char v[1024];
    lcfg_defaults(s);
    if (u8_GetEnv("SB_TEST_XBE", v, sizeof v) > 0) {
        strcpy_s(s->xbe_path, sizeof s->xbe_path, v);
    }
    if (u8_GetEnv("SB_TEST_MEDIA", v, sizeof v) > 0) {
        strcpy_s(s->media_root, sizeof s->media_root, v);
    }
}
#define lcfg_defaults selftest_defaults
#endif

/* COMM-SELFTEST2 (W171): gueltige Spieldateien fuer die Validierungsfaelle
 * 5/8/8b/15. Private: Edition-Standardpfade (echte Daten auf dem
 * Entwicklungsrechner, wie bisher). Community: es gibt keine Standardpfade
 * (Spieler waehlt sein Abbild) -> Temp-Ordner mit leerer default.xbe und
 * Media/ (leere XBE: Versionsabgleich entfaellt, Pfadpruefung greift). */
static char g_st_game[1024];

static void st_valid_base(launcher_settings_t *b)
{
    lcfg_defaults(b);
#if SB_EDITION_COMMUNITY
    {
        char p[1200];
        FILE *f = NULL;
        if (!g_st_game[0]) {
            u8_GetTempPath(g_st_game, sizeof g_st_game);
            strcat_s(g_st_game, sizeof g_st_game, "w171_st_game");
            _snprintf_s(p, sizeof p, _TRUNCATE, "%s/Media", g_st_game);
            lcfg_mkdir_p(p);
            _snprintf_s(p, sizeof p, _TRUNCATE, "%s/default.xbe", g_st_game);
            if (u8_fopen_s(&f, p, "wb") == 0 && f) {
                fclose(f);
            }
        }
        _snprintf_s(b->xbe_path, sizeof b->xbe_path, _TRUNCATE, "%s/default.xbe",
                    g_st_game);
        strcpy_s(b->media_root, sizeof b->media_root, g_st_game);
    }
#endif
}

static void st_valid_cleanup(void)
{
    if (g_st_game[0]) {
        char p[1200];
        wchar_t w[1200];
        _snprintf_s(p, sizeof p, _TRUNCATE, "%s/default.xbe", g_st_game);
        u8_DeleteFile(p);
        _snprintf_s(p, sizeof p, _TRUNCATE, "%s/Media", g_st_game);
        u8_to_w(p, w, 1200);
        RemoveDirectoryW(w);
        u8_to_w(g_st_game, w, 1200);
        RemoveDirectoryW(w);
        g_st_game[0] = '\0';
    }
}

/* W226: Lerner mit einer Folge von Abtastungen einer Achse fuettern (alle
 * anderen Achsen ruhen auf rest_other). Rueckgabe = erstes Ergebnis != 0,
 * *at = Index der Abtastung (-1 = nichts gelernt). */
static int w226_feed(lctrl_joy_learner_t *L, int idx, unsigned mask, int ax,
                     int rest_other, const int *v, int n, char *o, size_t cap,
                     int *at)
{
    int k, i;
    for (k = 0; k < n; k++) {
        imap_joy_state_t st;
        int r;
        memset(&st, 0, sizeof st);
        for (i = 0; i < IJOY_POVS; i++) {
            st.pov[i] = -1;
        }
        for (i = 0; i < IJOY_AXES; i++) {
            st.axis[i] = (int16_t)rest_other;
        }
        st.axis[ax] = (int16_t)v[k];
        r = lctrl_joy_learner_step(L, 1, &st, mask, 0, idx, o, cap);
        if (r) {
            *at = k;
            return r;
        }
    }
    *at = -1;
    return 0;
}

/* W281: Meldungen des reinen Layout-Selbsttests zaehlen */
static int g_llay_pass, g_llay_fail;
static void llay_rep(const char *name, int ok, const char *detail)
{
    if (ok) {
        printf("PASS %s\n", name);
        g_llay_pass++;
    } else {
        printf("FAIL %s: %s\n", name, detail ? detail : "");
        g_llay_fail++;
    }
}

/* Fix Tastenbelegung (Community 17.11.): Ersetzen/Hinzufuegen stuerzte ohne
 * Original-Controller ab - gui.sbc_none (258 Byte) passte nicht in den
 * 256-Byte-Puffer, strcpy_s beendete den Prozess. Uebersetzte Texte werden
 * jetzt gekuerzt; jede Pufferlaenge muss ohne Absturz durchgehen. */
static __declspec(noinline) void st_sbc_trunc(int *pass_, int *fail_)
{
    static const size_t caps[] = {1, 2, 8, 16, 64, 128, 256};
    char buf[256];
    int i, ok = 1;
    for (i = 0; i < (int)(sizeof caps / sizeof caps[0]); i++) {
        memset(buf, 'x', sizeof buf);
        (void)lctrl_sbc_status(buf, caps[i]);
        if (strlen(buf) >= caps[i]) { ok = 0; }
    }
    if (ok) { printf("PASS sbc_status_trunc\n"); (*pass_)++; }
    else { printf("FAIL sbc_status_trunc: Text nicht gekuerzt\n"); (*fail_)++; }
}

/* W309 FPS-MONITOR: angebotene Liste je Monitorrate (sb_fps_list, gleich in
 * Launcher und ESC), Kennzeichen, alter cfg-Wert ueber dem Monitor, R10.
 * Eigene Funktion (noinline, Puffer static): der Stapelrahmen von
 * cli_selftest ist bereits gross, die GUI-Faelle laufen darunter. */
static __declspec(noinline) void st_w309_fpsmon(int *pass_, int *fail_)
{
#define ST_OK(name) do { printf("PASS %s\n", name); (*pass_)++; } while (0)
#define ST_FAIL(name, fmt, ...) do { printf("FAIL %s: " fmt "\n", name, __VA_ARGS__); (*fail_)++; } while (0)
    static launcher_settings_t a, b;
    static char tmp[1024];
    /* Erwartung je Monitor: Wert (J = kann ruckeln, A = ueber Monitor, nur keep) */
    static const struct { int hz, keep; const char *want; } c[] = {
        { 60, -1, "auto 0 60" },
        { 59, -1, "auto 0 60" },                                      /* 59,94 Hz (abgeschnitten) */
        { 144, -1, "auto 0 60J 75J 90J 100J 120J 144" },
        { 143, -1, "auto 0 60J 75J 90J 100J 120J 144" },
        { 165, -1, "auto 0 60J 75J 90J 100J 120J 144J 165" },         /* Tester: 60/120 ruckeln, 165 nicht */
        { 164, -1, "auto 0 60J 75J 90J 100J 120J 144J 165" },
        { 240, -1, "auto 0 60 75J 90J 100J 120 144J 165J 240" },
        { 120, -1, "auto 0 60 75J 90J 100J 120" },
        { 50, -1, "auto 0" },
        { 0, -1, "auto 0 60 75 90 100 120 144 165 240" },              /* unbekannt: wie bisher */
        { 144, 240, "auto 0 60J 75J 90J 100J 120J 144 240A" },         /* Alt-cfg 240 bleibt */
        { 60, 165, "auto 0 60 165A" },
        { 144, 0, "auto 0 60J 75J 90J 100J 120J 144" },
    };
    int ok = 1, i, k;
    char why[200] = "";
    lr_env_t env;
    lr_res_t rr[LR_N];
    for (i = 0; i < (int)(sizeof c / sizeof c[0]) && ok; i++) {
        SbFpsItem l[SB_FPS_LIST_MAX];
        char got[160] = "";
        int n = sb_fps_list(c[i].hz, c[i].keep, l, SB_FPS_LIST_MAX);
        for (k = 0; k < n; k++) {
            char t[16];
            if (l[k].v == SB_FPS_AUTO) { strcpy_s(t, sizeof t, "auto"); }
            else { _snprintf_s(t, sizeof t, _TRUNCATE, "%d%s%s", l[k].v, (l[k].flags & SB_FPS_F_JUDDER) ? "J" : "",
                               (l[k].flags & SB_FPS_F_ABOVE) ? "A" : ""); }
            if (k) { strcat_s(got, sizeof got, " "); }
            strcat_s(got, sizeof got, t);
        }
        if (strcmp(got, c[i].want) != 0) {
            ok = 0;
            _snprintf_s(why, sizeof why, _TRUNCATE, "%d Hz keep %d: '%s' statt '%s'", c[i].hz, c[i].keep, got, c[i].want);
        }
    }
    /* Alt-cfg mit zu hohem Wert: Laden/Speichern aendert ihn nicht (keine stille Migration) */
    if (ok) {
        u8_GetTempPath(tmp, sizeof tmp);
        strcat_s(tmp, sizeof tmp, "w309_fps.cfg");
        u8_DeleteFile(tmp);
        lcfg_defaults(&a);
        a.interp_fps = 240;
        if (lcfg_save_user(tmp, &a) != 0) { ok = 0; strcpy_s(why, sizeof why, "save 240"); }
        lcfg_defaults(&b);
        lcfg_load_file(tmp, &b);
        lcfg_gfx_migrate_fps(&b);
        if (b.interp_fps != 240) { ok = 0; _snprintf_s(why, sizeof why, _TRUNCATE, "240 geladen als %d", b.interp_fps); }
        st_valid_base(&b);
        b.interp_fps = 240;
        {
            char e[256];
            if (lcfg_validate(&b, e, sizeof e) != 0) { ok = 0; _snprintf_s(why, sizeof why, _TRUNCATE, "240 ungueltig: %.60s", e); }
        }
        u8_DeleteFile(tmp);
    }
    /* R10 (Fenster/randlos): ueber Monitor oder kein glatter Teiler warnt */
    if (ok) {
        static const struct { int hz, fps, warn; } r[] = {
            { 165, 120, 1 }, { 165, 60, 1 }, { 165, 165, 0 }, { 164, 165, 0 }, { 144, 240, 1 },
            { 240, 120, 0 }, { 240, 60, 0 }, { 120, 60, 0 }, { 59, 60, 0 }, { 144, 120, 1 }, { 60, 0, 0 },
            { 165, SB_FPS_AUTO, 0 },
        };
        lcfg_defaults(&b);
        b.window_mode = 0;
        lrules_env_default(&env, &b);
        for (i = 0; i < (int)(sizeof r / sizeof r[0]); i++) {
            env.display_hz = r[i].hz;
            b.interp_fps = r[i].fps;
            lrules_eval(&env, rr);
            if (rr[10].active != r[i].warn) {
                ok = 0;
                _snprintf_s(why, sizeof why, _TRUNCATE, "r10 %d@%d = %d", r[i].fps, r[i].hz, rr[10].active);
                break;
            }
        }
        b.window_mode = 2; /* exklusiv: Rate folgt dem Modus (R9), keine R10 */
        env.display_hz = 165;
        b.interp_fps = 120;
        lrules_eval(&env, rr);
        if (ok && rr[10].active) { ok = 0; strcpy_s(why, sizeof why, "r10 exklusiv"); }
    }
    if (ok) { ST_OK("W309 fps-monitor (Liste 60/144/165/240/120/50/unbekannt Hz, Kennzeichen, Alt-cfg 240, R10)"); }
    else { ST_FAIL("W309 fps-monitor", "%s", why); }
#undef ST_OK
#undef ST_FAIL
}

/* W312 JOY-CALIB: Pad-Lerner ab Ruhestellung (Hori-Diagonale), Partner-
 * Pruefung aim_x/aim_y, joy_cal-Zeilen in user.cfg (Temp), Kalibrierung im
 * Joystick-Lerner. Nur reine Logik, keine Geraete, keine echten Dateien. */
static __declspec(noinline) void st_w312_joycalib(int *pass_, int *fail_)
{
#define W312_OK(name) do { printf("PASS %s\n", name); (*pass_)++; } while (0)
#define W312_FAIL(name, fmt, ...) do { printf("FAIL %s: " fmt "\n", name, __VA_ARGS__); (*fail_)++; } while (0)
    static char tmp[1024], buf[4096], lines[8][96], back[40][96];
    /* 1: Flugstick im Xbox-Modus, Schub auf RY am Anschlag (-32768) */
    {
        lctrl_pad_t rest, cur;
        lctrl_pad_learner_t L;
        char nm[16], nx[16] = "", ny[16] = "";
        int i, old_rest, new_rest = 0, ok;
        memset(&rest, 0, sizeof rest);
        rest.ry = -32768;
        old_rest = lctrl_pad_deflected(&rest, nm, sizeof nm) && strcmp(nm, "RY") == 0;
        /* aim_x lernen: 3 ruhige Abtastungen, dann Stick rechts */
        lctrl_pad_learner_reset(&L);
        for (i = 0; i < 6; i++) {
            new_rest |= lctrl_pad_learner_axis(&L, &rest, nm, sizeof nm);
        }
        cur = rest;
        cur.lx = 32000;
        if (lctrl_pad_learner_axis(&L, &cur, nm, sizeof nm)) {
            strcpy_s(nx, sizeof nx, nm);
        }
        /* aim_y lernen: Stick vorn */
        lctrl_pad_learner_reset(&L);
        for (i = 0; i < 4; i++) {
            new_rest |= lctrl_pad_learner_axis(&L, &rest, nm, sizeof nm);
        }
        cur = rest;
        cur.ly = 31000;
        if (lctrl_pad_learner_axis(&L, &cur, nm, sizeof nm)) {
            strcpy_s(ny, sizeof ny, nm);
        }
        ok = old_rest && !new_rest && strcmp(nx, "LX") == 0 && strcmp(ny, "LY") == 0;
        if (ok) {
            W312_OK("W312 JOY-CALIB pad-rest (alt: Ruhe-Schub RY sofort gelernt -> diagonal; neu: LX/LY)");
        } else {
            W312_FAIL("W312 JOY-CALIB pad-rest", "old=%d new_rest=%d x=%s y=%s", old_rest, new_rest, nx, ny);
        }
    }
    /* 2: normales Pad unveraendert (Mitte ruht), Diagonale bleibt uneindeutig,
     * Trigger ab Ruhe, gehaltene Auslenkung beim Start lernt nicht sofort */
    {
        lctrl_pad_t z, c;
        lctrl_pad_learner_t L;
        char nm[16];
        int i, ok = 1;
        memset(&z, 0, sizeof z);
        lctrl_pad_learner_reset(&L);
        for (i = 0; i < 3; i++) { lctrl_pad_learner_axis(&L, &z, nm, sizeof nm); }
        c = z; c.rx = -30000;
        ok = ok && lctrl_pad_learner_axis(&L, &c, nm, sizeof nm) && strcmp(nm, "RX") == 0;
        c = z; c.lx = 25000; c.ly = 25000; /* diagonal: zwei heisse Achsen */
        ok = ok && !lctrl_pad_learner_axis(&L, &c, nm, sizeof nm);
        c = z; c.rt = 250;
        ok = ok && lctrl_pad_learner_axis(&L, &c, nm, sizeof nm) && strcmp(nm, "RT") == 0;
        c = z; c.lx = 32000; /* beim Start gehalten -> Basis, kein Lernen */
        lctrl_pad_learner_reset(&L);
        for (i = 0; i < 5; i++) { ok = ok && !lctrl_pad_learner_axis(&L, &c, nm, sizeof nm); }
        if (ok) {
            W312_OK("W312 JOY-CALIB pad-normal (Mitte/Diagonale/Trigger/gehalten)");
        } else {
            W312_FAIL("W312 JOY-CALIB pad-normal", "%s", "Abweichung");
        }
    }
    /* 3: Partner-Pruefung (gleiche Achse auf aim_x und aim_y) */
    {
        lctrl_bind_t b;
        int ax = lctrl_bind_elem_index("aim_x"), ay = lctrl_bind_elem_index("aim_y");
        int sx = lctrl_bind_elem_index("sight_x"), sy = lctrl_bind_elem_index("sight_y");
        int ok = ax >= 0 && ay >= 0 && lctrl_axis_partner(ax) == ay && lctrl_axis_partner(ay) == ax &&
                 lctrl_axis_partner(sx) == sy && lctrl_axis_partner(lctrl_bind_elem_index("rot")) == -1;
        ok = ok && lctrl_alt_same_axis("PAD:RY:dz=0.15:curve=1.5:inv=1", "PAD:RY:dz=0.15:curve=1.5") &&
             !lctrl_alt_same_axis("PAD:RY", "PAD:LY") &&
             lctrl_alt_same_axis("JOY0:X:dz=0.05", "JOY0:X:dz=0.05:inv=1") &&
             !lctrl_alt_same_axis("JOY0:X", "JOY1:X") && !lctrl_alt_same_axis("JOY0:X+", "JOY0:X") &&
             !lctrl_alt_same_axis("KEY:LEFT", "KEY:LEFT");
        lctrl_bind_init(&b);
        lctrl_bind_set(&b, ax, "JOY0:Y:dz=0.05, KEY:RIGHT");
        ok = ok && lctrl_bind_axis_used_by(&b, ax, "JOY0:Y:dz=0.05:inv=1") &&
             !lctrl_bind_axis_used_by(&b, ax, "JOY0:X:dz=0.05") && !lctrl_bind_axis_used_by(&b, ay, "JOY0:Y");
        if (ok) {
            W312_OK("W312 JOY-CALIB same-axis (aim_x/aim_y, sight_x/sight_y)");
        } else {
            W312_FAIL("W312 JOY-CALIB same-axis", "ax=%d ay=%d", ax, ay);
        }
    }
    /* 4: joy_cal-Zeilen in user.cfg (Temp): fremde Zeilen und Assistent bleiben */
    {
        FILE *f = NULL;
        int ok = 1, n, k;
        u8_GetTempPath(tmp, sizeof tmp);
        strcat_s(tmp, sizeof tmp, "w312_joycal_test.cfg");
        u8_DeleteFile(tmp);
        if (u8_fopen_s(&f, tmp, "w") != 0 || !f) {
            ok = 0;
        } else {
            fputs("# Kopf\r\n[input]\r\nsbc_usb=1\r\npad_dz_stick=10\r\njoy_cal5=0001:0002,X,-1,-,1,0,0\r\n"
                  "joy_calx=fremd\r\n\r\n[audio]\r\nvol=80\r\n", f);
            fclose(f);
        }
        strcpy_s(lines[0], sizeof lines[0], "joy_cal0=046D:C283,X,-26000,1500,30500,5,0");
        strcpy_s(lines[1], sizeof lines[1], "joy_cal1=046D:C283,S0,-32000,-,32000,0,1");
        ok = ok && joycal_cfg_apply(tmp, lines, 2) == 0;
        memset(buf, 0, sizeof buf);
        if (u8_fopen_s(&f, tmp, "r") == 0 && f) {
            (void)fread(buf, 1, sizeof buf - 1, f);
            fclose(f);
        }
        ok = ok && strstr(buf, "joy_cal0=046D:C283,X") && strstr(buf, "joy_cal1=046D:C283,S0") &&
             !strstr(buf, "joy_cal5=") && strstr(buf, "joy_calx=fremd") && strstr(buf, "pad_dz_stick=10") &&
             strstr(buf, "sbc_usb=1") && strstr(buf, "[audio]") && strstr(buf, "vol=80") && strstr(buf, "# Kopf");
        n = joycal_cfg_read(tmp, back, 40);
        ok = ok && n == 2;
        for (k = 0; k < n && ok; k++) {
            unsigned short v, p;
            int a;
            ijoy_cal_t c;
            const char *eq = strchr(back[k], '=');
            ok = eq && ijoy_cal_parse(eq + 1, &v, &p, &a, &c) && v == 0x046D && p == 0xC283;
        }
        ok = ok && assist_cfg_read(tmp, back, 8) == 1; /* Assistent sieht nur pad_dz_stick */
        ok = ok && joycal_cfg_apply(tmp, lines, 0) == 0 && joycal_cfg_read(tmp, back, 40) == 0 &&
             assist_cfg_read(tmp, back, 8) == 1;
        strcpy_s(lines[2], sizeof lines[2], "pad_dz_stick=3"); /* fremder Key -> abgelehnt */
        ok = ok && joycal_cfg_apply(tmp, (const char (*)[96])lines + 2, 1) != 0 &&
             !joycal_cfg_owns("joy_cal32") && !joycal_cfg_owns("joy_calx") && joycal_cfg_owns("joy_cal31");
        u8_DeleteFile(tmp);
        if (ok) {
            W312_OK("W312 JOY-CALIB cfg (joy_calN ersetzt, fremde Zeilen/Assistent intakt)");
        } else {
            W312_FAIL("W312 JOY-CALIB cfg", "n=%d", n);
        }
    }
    /* 5: Kalibrierung im Joystick-Lerner: Achse mit kleinem Weg (+-12000,
     * Treiber/Windows-Kalibrierung daneben) lernt erst kalibriert */
    {
        lctrl_joy_learner_t L;
        imap_joy_state_t st;
        ijoy_cal_t c = { -12000, 0, 12000, 0, 0 };
        char out[48] = "", out2[48] = "";
        int idx = lctrl_bind_elem_index("aim_x"), i, r_raw = 0, r_cal = 0, v;
        for (i = 0; i < 2; i++) {
            lctrl_joy_learner_reset(&L);
            for (v = 0; v < 12; v++) {
                int r;
                memset(&st, 0, sizeof st);
                st.axis[0] = (int16_t)(v < 4 ? 0 : 12000); /* Ruhe, dann voll rechts (roh) */
                st.axis[1] = 100;
                if (i == 1) {
                    st.axis[0] = ijoy_cal_apply(&c, st.axis[0]);
                }
                r = lctrl_joy_learner_step(&L, 1, &st, 0x03, 0, idx, i ? out2 : out, 48);
                if (r) {
                    if (i) { r_cal = r; } else { r_raw = r; }
                    break;
                }
            }
        }
        if (r_raw == 0 && r_cal == 3 && strncmp(out2, "X:", 2) == 0) {
            W312_OK("W312 JOY-CALIB cal-learn (roh 12000 nicht lernbar, kalibriert X)");
        } else {
            W312_FAIL("W312 JOY-CALIB cal-learn", "raw=%d cal=%d '%s'", r_raw, r_cal, out2);
        }
    }
#undef W312_OK
#undef W312_FAIL
}

/* W432 HOTAS-AXES (GitHub #6, 0.3.1): Joystick-Achsen im Belegungsdialog.
 * Nachbau der Tester-Geraete als Fake-DirectInput (ijoy_test_*, keine
 * Hardware, keine Eingabewerte in Dateien): WingMan Force 3D (X Y RZ S0,
 * logisch 0..255, 7 Tasten, POV) und Arduino Micro (X Y + Throttle 02:BB,
 * logisch 0..65535, 8 Tasten, POV). Beide Geraete laufen gleichzeitig, je
 * 50-ms-Takt wie bind_cap_joy: ijoy_read -> lctrl_joy_learner_step. */
#define W432_ABS 0x02u
#define W432_BTN 0x04u
#define W432_POV 0x10u
#define W432_INST(n) ((unsigned)(n) << 8)
#define W432_NDEV 2
static ijoy_obj_t g_w432_o[W432_NDEV][IJOY_OBJ_MAX];
static long g_w432_rest[W432_NDEV][IJOY_OBJ_MAX];
static long g_w432_lmax[W432_NDEV];
static int g_w432_acc[W432_NDEV], g_w432_n[W432_NDEV];
static lctrl_joy_learner_t g_w432_L[W432_NDEV];

static int w432_add(ijoy_obj_t *o, int n, int kind, unsigned type, unsigned short up, unsigned short u,
                    const char *nm, long lmax, int acc)
{
    memset(&o[n], 0, sizeof o[n]);
    o[n].kind = kind;
    o[n].type = type;
    o[n].upage = up;
    o[n].usage = u;
    strncpy_s(o[n].name, sizeof o[n].name, nm, _TRUNCATE);
    if (kind <= IJOY_K_AXIS) {
        /* acc = Treiber nimmt DIPROP_RANGE an (Fake: lo == hi -> Werte
         * -32768..32767, DirectInput skaliert), sonst Rohbereich 0..lmax */
        o[n].lo = 0;
        o[n].hi = acc ? 0 : lmax;
        o[n].llo = 0;
        o[n].lhi = lmax;
    }
    o[n].read = -1;
    return n + 1;
}

static int w432_btns(ijoy_obj_t *o, int n, int count)
{
    int b;
    for (b = 0; b < count; b++) {
        n = w432_add(o, n, IJOY_K_BTN, W432_BTN | W432_INST(b), 0x09, (unsigned short)(b + 1), "Button", 0, 0);
    }
    return n;
}

/* Geraet d einrichten: 0 = WingMan Force 3D, 1 = Arduino Micro. thr_unk =
 * Arduino-Throttle mit unbekannter GUID (nur Usage 02:BB). Ruhewerte in
 * logischen Einheiten (Mitte bzw. Schub am Ende). */
static void w432_dev(int d, int acc, int thr_unk)
{
    ijoy_obj_t *o = g_w432_o[d];
    int n = 0;
    g_w432_acc[d] = acc;
    if (d == 0) {
        g_w432_lmax[d] = 255;
        n = w432_add(o, n, IJOY_K_X, W432_ABS | W432_INST(0), 0x01, 0x30, "X Axis", 255, acc);
        n = w432_add(o, n, IJOY_K_Y, W432_ABS | W432_INST(1), 0x01, 0x31, "Y Axis", 255, acc);
        n = w432_add(o, n, IJOY_K_RZ, W432_ABS | W432_INST(5), 0x01, 0x35, "Rz Axis", 255, acc);
        n = w432_add(o, n, IJOY_K_SLIDER, W432_ABS | W432_INST(6), 0x01, 0x36, "Slider", 255, acc);
        n = w432_add(o, n, IJOY_K_POV, W432_POV | W432_INST(0), 0x01, 0x39, "Hat Switch", 0, 0);
        n = w432_btns(o, n, 7);
        g_w432_rest[d][0] = g_w432_rest[d][1] = g_w432_rest[d][2] = 128;
        g_w432_rest[d][3] = 255; /* Schub ganz zurueck */
    } else {
        g_w432_lmax[d] = 65535;
        n = w432_add(o, n, IJOY_K_X, W432_ABS | W432_INST(0), 0x01, 0x30, "X", 65535, acc);
        n = w432_add(o, n, IJOY_K_Y, W432_ABS | W432_INST(1), 0x01, 0x31, "Y", 65535, acc);
        n = w432_add(o, n, thr_unk ? IJOY_K_AXIS : IJOY_K_SLIDER, W432_ABS | W432_INST(2), 0x02, 0xBB, "Throttle",
                     65535, acc);
        n = w432_add(o, n, IJOY_K_POV, W432_POV | W432_INST(0), 0x01, 0x39, "Hat Switch", 0, 0);
        n = w432_btns(o, n, 8);
        g_w432_rest[d][0] = g_w432_rest[d][1] = 32768;
        g_w432_rest[d][2] = 6000; /* Schiebepoti nahe Minimum */
    }
    g_w432_n[d] = n;
}

/* Beide Geraete einstecken (Fake) und aufzaehlen. */
static int w432_plug(void)
{
    static const char specs[IMAP_JOY_MAX][96] = { "", "", "", "" };
    int d;
    ijoy_test_begin(W432_NDEV, specs, 0u);
    for (d = 0; d < W432_NDEV; d++) {
        ijoy_test_objects(d, g_w432_o[d], g_w432_n[d]);
    }
    return ijoy_scan();
}

static void w432_put(int d, int k, long raw)
{
    long m = g_w432_lmax[d], v = raw < 0 ? 0 : raw > m ? m : raw;
    if (g_w432_acc[d]) { /* DirectInput skaliert logisch 0..m auf -32768..32767 */
        v = -32768 + (long)(((long long)v * 65535 + m / 2) / m);
    }
    ijoy_test_obj_value(d, k, v);
}

/* Ein Lernvorgang: 8 Takte Ruhe, dann bewegt sich Objekt obj von Geraet dev
 * in 4 Takten von der Ruhe auf peak (logisch) und bleibt dort; btn >= 0:
 * statt Achse diese Taste ab Takt 8 gedrueckt. noise = Rauschen auf allen
 * Achsen beider Geraete (logisch, abwechselnd +/-). Rueckgabe wie
 * lctrl_joy_learner_step, *who = Geraet, out = Teil hinter "JOYn:". */
/* Fake-Takt t setzen (auch fuer den Dialog-Selbsttest in launcher_gui.c). */
void w432_fixture_tick(int t, int dev, int obj, long peak, int btn, long noise)
{
    int d, k;
    for (d = 0; d < W432_NDEV; d++) {
        for (k = 0; k < g_w432_n[d]; k++) {
            const ijoy_obj_t *b = &g_w432_o[d][k];
            if (b->kind <= IJOY_K_AXIS) {
                long v = g_w432_rest[d][k];
                if (d == dev && k == obj && btn < 0 && t >= 8) {
                    int s = t - 8;
                    v = s >= 3 ? peak : v + (peak - v) * (s + 1) / 4;
                }
                if (noise) {
                    v += ((t + k) & 1) ? noise : -noise;
                }
                w432_put(d, k, v);
            } else if (b->kind == IJOY_K_POV) {
                ijoy_test_obj_value(d, k, -1);
            } else if (b->kind == IJOY_K_BTN) {
                ijoy_test_obj_value(d, k, (d == dev && k == btn && t >= 8) ? 1 : 0);
            }
        }
    }
}

/* WingMan (Geraet 0) + Arduino (Geraet 1) als Fake einstecken; Anzahl. */
int w432_fixture_plug(int acc, int thr_unk)
{
    w432_dev(0, acc, 0);
    w432_dev(1, acc, thr_unk);
    return w432_plug();
}

static int w432_learn(int idx, int dev, int obj, long peak, int btn, long noise, char *out, size_t cap, int *who)
{
    int t, d, r;
    *who = -1;
    out[0] = '\0';
    for (d = 0; d < W432_NDEV; d++) {
        lctrl_joy_learner_reset(&g_w432_L[d]);
    }
    for (t = 0; t < 40; t++) {
        w432_fixture_tick(t, dev, obj, peak, btn, noise);
        for (d = 0; d < W432_NDEV; d++) {
            imap_joy_state_t st;
            const ijoy_info_t *in = ijoy_info(d);
            int ok;
            if (!in) {
                continue;
            }
            ok = ijoy_read(d, &st);
            r = lctrl_joy_learner_step(&g_w432_L[d], ok, &st, in->axmask, in->npov, idx, out, cap);
            if (r) {
                *who = d;
                return r;
            }
        }
    }
    return 0;
}

static __declspec(noinline) void st_w432_hotas(int *pass_, int *fail_)
{
#define W432_OK(name) do { printf("PASS %s\n", name); (*pass_)++; } while (0)
#define W432_FAIL(name, fmt, ...) do { printf("FAIL %s: " fmt "\n", name, __VA_ARGS__); (*fail_)++; } while (0)
    /* Erwartung je Fall: Element, Geraet, Objekt, Ziel (logisch), Ergebnis */
    static const struct {
        const char *elem;
        int dev, obj;
        long peak;
        const char *want; /* Teil hinter JOYn: */
        const char *what;
    } c[] = {
        /* WingMan: Mitte 128, Weg 75 % bzw. 55 % des Halbwegs (alt: 80 % noetig) */
        { "aim_x", 0, 0, 223, "X:dz=0.05", "WingMan X Mitte->rechts 75%" },
        { "aim_y", 0, 1, 33, "Y:dz=0.05", "WingMan Y Mitte->vorn 75%" },
        { "rot", 0, 2, 198, "RZ:dz=0.05", "WingMan RZ Drehung 55%" },
        { "rot", 0, 2, 58, "RZ:dz=0.05:inv=1", "WingMan RZ Drehung links 55%" },
        { "pedal_r", 0, 3, 0, "S0:dz=0.05:inv=1", "WingMan Schub Ende->Ende" },
        /* Arduino: Mitte 32768, Weg 70 %; Schiebepoti 6000 -> 30000 (37 %) */
        { "aim_x", 1, 0, 55705, "X:dz=0.05", "Arduino X Mitte->rechts 70%" },
        { "aim_y", 1, 1, 9830, "Y:dz=0.05", "Arduino Y Mitte->vorn 70%" },
        { "pedal_r", 1, 2, 30000, "S0:dz=0.05", "Arduino Throttle 02:BB 37%" },
    };
    static const char *const mode[4] = { "DIPROP_RANGE angenommen", "Rohbereich (Software-Umrechnung)",
                                         "Throttle ohne GUID", "Rauschen +-1 %" };
    char out[64], why[600];
    int m, i, who, r, n, ok;
    for (m = 0; m < 4; m++) {
        int acc = m != 1, nok = 0, ncase = 0;
        why[0] = '\0';
        n = w432_fixture_plug(acc, m == 2);
        if (n != 2) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "%d Geraete statt 2", n);
        }
        for (i = 0; i < (int)(sizeof c / sizeof c[0]) && n == 2; i++) { /* alle Faelle, Fehler gesammelt */
            long noise = m == 3 ? (c[i].dev ? 640 : 3) : 0; /* 16 Bit: +-1 %, 8 Bit: +-3 LSB */
            int idx = lctrl_bind_elem_index(c[i].elem);
            ncase++;
            r = w432_learn(idx, c[i].dev, c[i].obj, c[i].peak, -1, noise, out, sizeof out, &who);
            if (r == 3 && who == c[i].dev && strcmp(out, c[i].want) == 0) {
                nok++;
            } else {
                size_t L = strlen(why);
                _snprintf_s(why + L, sizeof why - L, _TRUNCATE, "%s%s->'%s'", L ? "; " : "", c[i].what, out);
            }
        }
        if (n == 2) { /* Taste am Arduino (alt bei Rauschen: keine Basis -> auch keine Taste) */
            ncase++;
            r = w432_learn(lctrl_bind_elem_index("bi0"), 1, 4 + 2, 0, 4 + 2, m == 3 ? 640 : 0, out, sizeof out,
                           &who);
            if (r == 1 && who == 1 && strcmp(out, "B3") == 0) {
                nok++;
            } else {
                size_t L = strlen(why);
                _snprintf_s(why + L, sizeof why - L, _TRUNCATE, "%sArduino Taste 3->'%s'", L ? "; " : "", out);
            }
        }
        if (!why[0]) { /* Ruhe (mit Rauschen) und kleine Bewegung (10 %) lernen nichts */
            r = w432_learn(lctrl_bind_elem_index("aim_x"), 0, 0, 128, -1, m == 3 ? 3 : 0, out, sizeof out, &who);
            if (r == 0) {
                r = w432_learn(lctrl_bind_elem_index("aim_x"), 1, 0, 32768 + 6554, -1, m == 3 ? 640 : 0, out,
                               sizeof out, &who);
            }
            if (r != 0) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "Ruhe/10 %% lernte '%s' (Geraet %d)", out, who);
            }
        }
        ijoy_test_end();
        ok = !why[0];
        {
            char name[160];
            _snprintf_s(name, sizeof name, _TRUNCATE,
                        "W432 HOTAS-AXES %s (WingMan X/Y/RZ/S0 + Arduino X/Y/Throttle, Taste, Ruhe; %d/%d)", mode[m],
                        nok, ncase);
            if (ok) {
                W432_OK(name);
            } else {
                W432_FAIL(name, "%s", why);
            }
        }
    }
    /* Gamepad-Weg (HORI Flight Stick im Xbox-Modus): Stick, Drehachse mit
     * kurzem Weg, Schub am Anschlag, Trigger als Ruder. Rest: Stick Mitte,
     * Schub RY = -32768 (Annahme, Belegung je Geraet verschieden). */
    {
        static const struct {
            short lx, ly, rx, ry;
            unsigned char lt, rt;
            const char *want; /* "" = nichts lernen */
            const char *what;
        } p[] = {
            { 24000, 11000, 0, -32768, 0, 0, "LX", "Stick rechts mit Nebenweg Y 34 %" },
            { -3000, -26000, 0, -32768, 0, 0, "LY", "Stick vorn 80 %" },
            { 0, 0, 16000, -32768, 0, 0, "RX", "Drehachse 49 %" },
            { 0, 0, 0, 0, 0, 0, "RY", "Schub Anschlag->Mitte" },
            { 0, 0, 0, -32768, 0, 120, "RT", "Trigger 47 %" },
            { 20000, 20000, 0, -32768, 0, 0, "", "Diagonale" },
            { 3000, -2000, 2500, -32000, 3, 4, "", "Rauschen/kleiner Weg" },
        };
        char why[600] = "", nm[16];
        int i, k, nok = 0;
        for (i = 0; i < (int)(sizeof p / sizeof p[0]); i++) {
            lctrl_pad_t rest, cur;
            lctrl_pad_learner_t L;
            int got = 0;
            memset(&rest, 0, sizeof rest);
            rest.ry = -32768;
            lctrl_pad_learner_reset(&L);
            nm[0] = '\0';
            for (k = 0; k < 12 && !got; k++) {
                cur = rest;
                if (k & 1) { /* leichtes Rauschen in der Ruhe (+-600) */
                    cur.lx = 600;
                    cur.rx = -600;
                }
                if (k >= 4) {
                    cur.lx = p[i].lx;
                    cur.ly = p[i].ly;
                    cur.rx = p[i].rx;
                    cur.ry = p[i].ry;
                    cur.lt = p[i].lt;
                    cur.rt = p[i].rt;
                }
                got = lctrl_pad_learner_axis(&L, &cur, nm, sizeof nm);
            }
            if (got ? strcmp(nm, p[i].want) == 0 : !p[i].want[0]) {
                nok++;
            } else {
                size_t L2 = strlen(why);
                _snprintf_s(why + L2, sizeof why - L2, _TRUNCATE, "%s%s->'%s'", L2 ? "; " : "", p[i].what,
                            got ? nm : "");
            }
        }
        if (!why[0]) {
            char name[160];
            _snprintf_s(name, sizeof name, _TRUNCATE,
                        "W432 HOTAS-AXES Gamepad-Weg (Xbox-Modus: Stick, Drehachse, Schub, Trigger, Diagonale; %d/%d)",
                        nok, (int)(sizeof p / sizeof p[0]));
            W432_OK(name);
        } else {
            W432_FAIL("W432 HOTAS-AXES Gamepad-Weg", "%s", why);
        }
    }
#undef W432_OK
#undef W432_FAIL
}

static int st_file_has(const char *path, const char *needle);

/* W438 Selbsttest: Datei (bis 16 KB) lesen; -1 = fehlt. */
static int st_file_read(const char *path, char *buf, size_t cap)
{
    FILE *f = NULL;
    size_t n;
    buf[0] = '\0';
    if (u8_fopen_s(&f, path, "rb") != 0 || !f) {
        return -1;
    }
    n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = '\0';
    return (int)n;
}

static int st_file_write(const char *path, const char *text)
{
    FILE *f = NULL;
    if (u8_fopen_s(&f, path, "wb") != 0 || !f) {
        return -1;
    }
    fputs(text, f);
    fclose(f);
    return 0;
}

static int st_count(const char *hay, const char *needle)
{
    int c = 0;
    const char *p = hay;
    size_t l = strlen(needle);
    while ((p = strstr(p, needle)) != NULL) {
        c++;
        p += l;
    }
    return c;
}

/* W438 CTRL-RESET-040: Reset genau einmal, user_*.ini unveraendert, neue Installation ohne Reset. */
static int w438_ctrl_reset_selftest(const char *tmp, char *why, size_t wcap)
{
    static char a[16384], b[16384];
    char dir[1100], uc[1200], lc[1200], ini[1200], psb[96], ploc[96];
    static const char kIni[] = "# eigene Belegung (Testdaten)\r\n[buttons]\r\nbi6 = KEY:ENTER\r\n";
    launcher_settings_t s;
    int r;
    why[0] = '\0';
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%sw438_ctrl", tmp);
    lcfg_mkdir_p(dir);
    _snprintf_s(uc, sizeof uc, _TRUNCATE, "%s/user.cfg", dir);
    _snprintf_s(lc, sizeof lc, _TRUNCATE, "%s/user_loc.cfg", dir);
    _snprintf_s(ini, sizeof ini, _TRUNCATE, "%s/user_mein.ini", dir);
    u8_DeleteFile(uc);
    u8_DeleteFile(lc);
    /* a: neue Installation (keine user.cfg): nichts, nichts angelegt */
    r = lgame_ctrl040_reset(uc, lc, psb, sizeof psb, ploc, sizeof ploc);
    if (r != 0 || st_file_read(uc, a, sizeof a) >= 0 || lgame_ctrl040_mark(uc) != 1 || st_file_read(uc, a, sizeof a) >= 0) {
        _snprintf_s(why, wcap, _TRUNCATE, "a: neue Installation rc=%d", r);
    }
    /* b: erstes Speichern legt user.cfg an + Markierung -> danach nie ein Reset */
    if (!why[0]) {
        lcfg_defaults(&s);
        strcpy_s(s.input_profile, sizeof s.input_profile, "user_mein");
        if (lcfg_save_user(uc, &s) != 0 || lgame_ctrl040_mark(uc) != 0 || !lgame_ctrl040_marked(uc) ||
            lgame_ctrl040_reset(uc, lc, psb, sizeof psb, ploc, sizeof ploc) != 0 ||
            !st_file_has(uc, "input_profile=user_mein"))
            _snprintf_s(why, wcap, _TRUNCATE, "%s", "b: gespeicherte neue Installation wurde zurueckgesetzt");
    }
    /* c: bestehende Installation (0.3.x, ohne Markierung), eigene Belegung in beiden Spielen */
    if (!why[0]) {
        st_file_write(ini, kIni);
        st_file_write(uc, "# user.cfg\r\n[input]\r\ninput_profile=user_mein\r\nfoo=1\r\n[display]\r\nwidth=1280\r\n");
        st_file_write(lc, "# user_loc.cfg\r\n[input]\r\ninput_profile = keyboard_default\r\n");
        r = lgame_ctrl040_reset(uc, lc, psb, sizeof psb, ploc, sizeof ploc);
        st_file_read(uc, a, sizeof a);
        st_file_read(lc, b, sizeof b);
        if (r != 3 || strcmp(psb, "user_mein") != 0 || strcmp(ploc, "keyboard_default") != 0)
            _snprintf_s(why, wcap, _TRUNCATE, "c: rc=%d alt='%s'/'%s'", r, psb, ploc);
        else if (st_count(a, "input_profile=") != 1 || !strstr(a, "input_profile=default_full") ||
                 st_count(a, LGAME_KEY_CTRL040 "=1") != 1 || !strstr(a, "foo=1") || !strstr(a, "width=1280") ||
                 st_count(b, "input_profile") != 1 || !strstr(b, "input_profile=default_full") ||
                 strstr(b, LGAME_KEY_CTRL040))
            _snprintf_s(why, wcap, _TRUNCATE, "%s", "c: Dateiinhalt nach dem Reset falsch");
        else if (st_file_read(ini, b, sizeof b) < 0 || strcmp(b, kIni) != 0)
            _snprintf_s(why, wcap, _TRUNCATE, "%s", "c: user_mein.ini veraendert");
    }
    /* d: zweiter Start: nichts, Dateien bitgleich */
    if (!why[0]) {
        static char a2[16384], b2[16384];
        st_file_read(lc, b, sizeof b);
        r = lgame_ctrl040_reset(uc, lc, psb, sizeof psb, ploc, sizeof ploc);
        st_file_read(uc, a2, sizeof a2);
        st_file_read(lc, b2, sizeof b2);
        if (r != 0 || strcmp(a, a2) != 0 || strcmp(b, b2) != 0 || psb[0] || ploc[0])
            _snprintf_s(why, wcap, _TRUNCATE, "d: zweiter Start rc=%d", r);
    }
    /* e: Spieler waehlt die alte Belegung, Launcher speichert (auch eine aeltere Fassung behaelt fremde
     *    launcher_*-Zeilen): Markierung bleibt, kein erneuter Reset */
    if (!why[0]) {
        lcfg_defaults(&s);
        lcfg_load_file(uc, &s);
        strcpy_s(s.input_profile, sizeof s.input_profile, "user_mein");
        if (lcfg_save_user(uc, &s) != 0 || !lgame_ctrl040_marked(uc) ||
            lgame_ctrl040_reset(uc, lc, psb, sizeof psb, ploc, sizeof ploc) != 0 ||
            !st_file_has(uc, "input_profile=user_mein"))
            _snprintf_s(why, wcap, _TRUNCATE, "%s", "e: Reset nach Speichern erneut");
    }
    /* f: schon default_full (z. B. W431-Erststart mit 0.3.4) -> nur markiert, kein Hinweis; Legacy ohne Zeile */
    if (!why[0]) {
        u8_DeleteFile(lc);
        st_file_write(uc, "[input]\r\ninput_profile=default_full\r\n");
        r = lgame_ctrl040_reset(uc, lc, psb, sizeof psb, ploc, sizeof ploc);
        if (r != 0x100 || !lgame_ctrl040_marked(uc) || st_file_read(lc, b, sizeof b) >= 0)
            _snprintf_s(why, wcap, _TRUNCATE, "f: schon default_full rc=%d", r);
    }
    if (!why[0]) {
        st_file_write(uc, "[display]\r\nwidth=1280\r\n");
        r = lgame_ctrl040_reset(uc, lc, psb, sizeof psb, ploc, sizeof ploc);
        if (r != 1 || psb[0] || !st_file_has(uc, "input_profile=default_full"))
            _snprintf_s(why, wcap, _TRUNCATE, "f: Legacy rc=%d alt='%s'", r, psb);
    }
    u8_DeleteFile(uc);
    u8_DeleteFile(lc);
    u8_DeleteFile(ini);
    RemoveDirectoryA(dir);
    return why[0] ? 1 : 0;
}

/* W332 Selbsttest: enthaelt die Datei (bis 64 KB) den Text? */
static int st_file_has(const char *path, const char *needle)
{
    static char buf[65536];
    FILE *f = NULL;
    size_t n = 0;
    if (u8_fopen_s(&f, path, "rb") != 0 || !f) {
        return 0;
    }
    n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strstr(buf, needle) != NULL;
}

static int cli_selftest(const char *client_override)
{
    /* Jeder Test meldet PASS/FAIL; Exit 0 nur wenn alle PASS. */
    int pass = 0, fail = 0;
    launcher_settings_t a, b;
    /* W150 USER-DATA: Selbsttests arbeiten in Temp-Client-Ordnern und duerfen
     * nie das echte Profil (Community: %LOCALAPPDATA%) beschreiben. */
    SetEnvironmentVariableA("SB_USERDATA", "legacy");
    sb_userdata_reset();
    sb_lang_set("de"); /* W142: feste Sprache (Fehltexte deutsch pruefen) */
    char tmp[1024], err[1024], buf[4096];
    char client[1024];
    int has_ldr;
    FILE *f;

#define T_OK(name)                                   \
    do {                                             \
        printf("PASS %s\n", name);                   \
        pass++;                                      \
    } while (0)
#define T_FAIL(name, fmt, ...)                       \
    do {                                             \
        printf("FAIL %s: " fmt "\n", name, __VA_ARGS__); \
        fail++;                                      \
    } while (0)

    /* 1: Defaults */
    lcfg_defaults(&a);
    if (a.width == 2560 && a.height == 1080 && a.fps_limit == 120 &&
        a.audio == 1 && a.audio_live == 1 && a.present_interval == 0 &&
        a.aspect_mode == 1) {
        T_OK("1 defaults");
    } else {
        T_FAIL("1 defaults", "%s", "falsche Startwerte");
    }

    /* 2: save -> load Roundtrip */
    u8_GetTempPath(tmp, sizeof tmp);
    strcat_s(tmp, sizeof tmp, "w47_lcfg_test.cfg");
    u8_DeleteFile(tmp);
    a.width = 1920;
    a.height = 1080;
    a.fps_limit = 60;
    a.audio = 0;
    a.aspect_mode = 1;
    a.edge_mode = 2; /* W107 */
    a.edge_fade_width = 8;
    a.edge_fade_strength = 90;
    a.cockpit_persp = 40; /* W191 */
    a.cockpit_panini = 30;
    a.cockpit_width = 7; /* W235 */
    if (lcfg_save_user(tmp, &a) != 0) {
        T_FAIL("2 roundtrip", "%s", "save scheiterte");
    } else {
        lcfg_defaults(&b);
        lcfg_load_file(tmp, &b);
        if (memcmp(&a, &b, sizeof a) == 0) {
            T_OK("2 roundtrip");
        } else {
            T_FAIL("2 roundtrip", "%s", "Werte ungleich nach load");
        }
    }

    /* 3: fremde Keys/Kommentare bleiben erhalten */
    f = NULL;
    if (u8_fopen_s(&f, tmp, "a") == 0 && f) {
        fputs("# Kommentar bleibt\nfremder_key=42\n", f);
        fclose(f);
    }
    lcfg_defaults(&b);
    b.width = 1280;
    b.height = 720;
    if (lcfg_save_user(tmp, &b) != 0) {
        T_FAIL("3 preserve", "%s", "save scheiterte");
    } else {
        FILE *g = NULL;
        size_t n = 0;
        memset(buf, 0, sizeof buf);
        if (u8_fopen_s(&g, tmp, "r") == 0 && g) {
            n = fread(buf, 1, sizeof buf - 1, g);
            fclose(g);
        }
        (void)n;
        if (strstr(buf, "# Kommentar bleibt") && strstr(buf, "fremder_key=42") &&
            strstr(buf, "width=1280")) {
            T_OK("3 preserve");
        } else {
            T_FAIL("3 preserve", "%s", "fremde Zeile fehlt oder Key fehlt");
        }
    }
    u8_DeleteFile(tmp);

    /* 4: --set */
    lcfg_defaults(&b);
    lcfg_apply_set(&b, "width=3440;height=1440;audio=0;unbekannt=1");
    /* COMM-SELFTEST2: nicht gesetzte Keys behalten den Edition-Standard
     * (private: Pfad, community: leer). */
    if (b.width == 3440 && b.height == 1440 && b.audio == 0 &&
        strcmp(b.media_root, SB_DEFAULT_MEDIA_ROOT) == 0 &&
        strcmp(b.xbe_path, SB_DEFAULT_XBE_PATH) == 0) {
        T_OK("4 applyset");
    } else {
        T_FAIL("4 applyset", "%s", "Keys nicht " U8_UE "bernommen");
    }

    /* 5: Validierung ok (private: echte Pfade aus den Defaults; community:
     * Standard ohne Spieldateien wird verstaendlich abgelehnt, gueltiger
     * Ordner wird angenommen - COMM-SELFTEST2) */
#if SB_EDITION_COMMUNITY
    lcfg_defaults(&b);
    if (lcfg_validate(&b, err, sizeof err) == 0 ||
        strcmp(err, LT("cfg.no_media")) != 0) { /* W172: XBE optional */
        T_FAIL("5 validate-ok", "Standard ohne Spieldateien nicht abgelehnt: %.100s", err);
    } else
#endif
    {
        st_valid_base(&b);
        if (lcfg_validate(&b, err, sizeof err) == 0) {
            T_OK("5 validate-ok");
        } else {
            T_FAIL("5 validate-ok", "%.120s", err);
        }
    }

    /* 6: Validierung schlaegt verstaendlich fehl */
    strcpy_s(b.xbe_path, sizeof b.xbe_path, "C:/gibt/es/nicht.xbe");
    strcpy_s(b.media_root, sizeof b.media_root, "C:/gibt/es/nicht");
    if (lcfg_validate(&b, err, sizeof err) != 0 && strstr(err, "existiert nicht")) {
        T_OK("6 validate-bad");
    } else {
        T_FAIL("6 validate-bad", "%s", "kein/kryptischer Fehler");
    }

    /* 7: Client-Suche */
    lcfg_find_client_dir(client_override, client, sizeof client, &has_ldr);
    if (has_ldr) {
        T_OK("7 find-client");
    } else {
        T_FAIL("7 find-client", "kein sb_ldr.exe bei '%.120s'", client);
    }

    /* 8 (W47): Strecken wird abgelehnt (Regel S3.10) */
    st_valid_base(&b);
    b.aspect_mode = 0;
    if (lcfg_validate(&b, err, sizeof err) != 0 && strstr(err, "Strecken")) {
        T_OK("8 no-stretch");
    } else {
        T_FAIL("8 no-stretch", "%s", "aspect_mode=0 nicht abgelehnt");
    }

    /* 8b (W107): Bildraender ausserhalb 0..2 / Verlauf ausserhalb abgelehnt */
    st_valid_base(&b);
    b.edge_mode = 3;
    if (lcfg_validate(&b, err, sizeof err) != 0 && strstr(err, "Breitbild")) { /* W173: Glossar "Breitbild-Raender" */
        b.edge_mode = 2;
        b.edge_fade_width = 60;
        if (lcfg_validate(&b, err, sizeof err) != 0 && strstr(err, "Verlaufsbreite")) {
            T_OK("8b edge-range");
        } else {
            T_FAIL("8b edge-range", "%s", "edge_fade_width=60 nicht abgelehnt");
        }
    } else {
        T_FAIL("8b edge-range", "%s", "edge_mode=3 nicht abgelehnt");
    }

    /* 8c (W191 UW-PERSP): Cockpit-Perspektive 0..100 gueltig, -1/101 abgelehnt;
     * Standard = Original (0) */
    st_valid_base(&b);
    lcfg_defaults(&a);
    b.cockpit_persp = 0;
    if (a.cockpit_persp == 0 && lcfg_validate(&b, err, sizeof err) == 0) {
        b.cockpit_persp = 100;
        if (lcfg_validate(&b, err, sizeof err) == 0) {
            b.cockpit_persp = 101;
            if (lcfg_validate(&b, err, sizeof err) != 0 && strstr(err, "Cockpit") &&
                (b.cockpit_persp = -1, lcfg_validate(&b, err, sizeof err) != 0)) {
                T_OK("8c cpersp-range");
            } else {
                T_FAIL("8c cpersp-range", "%s", "cockpit_persp=101/-1 nicht abgelehnt");
            }
        } else {
            T_FAIL("8c cpersp-range", "%s", "cockpit_persp=100 abgelehnt");
        }
    } else {
        T_FAIL("8c cpersp-range", "%s", "Standard != 0 oder 0 abgelehnt");
    }

    /* 8d (W235 COCKPIT-WIDTH): Cockpit-Breite 0..10 gueltig, -1/11 abgelehnt;
     * Standard = Original (0) */
    st_valid_base(&b);
    lcfg_defaults(&a);
    b.cockpit_width = 0;
    if (a.cockpit_width == 0 && lcfg_validate(&b, err, sizeof err) == 0) {
        b.cockpit_width = 10;
        if (lcfg_validate(&b, err, sizeof err) == 0) {
            b.cockpit_width = 11;
            if (lcfg_validate(&b, err, sizeof err) != 0 && strstr(err, "Cockpit") &&
                (b.cockpit_width = -1, lcfg_validate(&b, err, sizeof err) != 0)) {
                T_OK("8d cwidth-range");
            } else {
                T_FAIL("8d cwidth-range", "%s", "cockpit_width=11/-1 nicht abgelehnt");
            }
        } else {
            T_FAIL("8d cwidth-range", "%s", "cockpit_width=10 abgelehnt");
        }
    } else {
        T_FAIL("8d cwidth-range", "%s", "Standard != 0 oder 0 abgelehnt");
    }

    /* 9 (W47): fehlende + kaputte cfg sind robust */
    {
        char missing[1024];
        int keys = -1;
        u8_GetTempPath(missing, sizeof missing);
        strcat_s(missing, sizeof missing, "w47_gibt_es_nicht_12345.cfg");
        u8_DeleteFile(missing);
        lcfg_defaults(&b);
        if (lcfg_load_file_count(missing, &b, &keys) == 1 &&
            b.width == 2560) {
            /* fehlt: Defaults unberuehrt */
            char brok[1024];
            u8_GetTempPath(brok, sizeof brok);
            strcat_s(brok, sizeof brok, "w47_brok_test.cfg");
            f = NULL;
            if (u8_fopen_s(&f, brok, "w") == 0 && f) {
                fputs("das ist keine cfg-zeile\nwidth=800\n"
                      "noch mehr muell ohne gleich\nheight=600\n",
                      f);
                fclose(f);
            }
            lcfg_defaults(&b);
            keys = -1;
            if (lcfg_load_file_count(brok, &b, &keys) == 0 &&
                b.width == 800 && b.height == 600 && keys == 2) {
                T_OK("9 broken-cfg");
            } else {
                T_FAIL("9 broken-cfg", "w=%u h=%u keys=%d", b.width,
                       b.height, keys);
            }
            u8_DeleteFile(brok);
        } else {
            T_FAIL("9 broken-cfg", "%s", "fehlende Datei nicht robust");
        }
    }

    /* 10-13 (W47): Profile in einem temporaeren Client-Ordner */
    {
        char tdir[1024], ppath[1200];
        char names[8][64];
        int n;
        u8_GetTempPath(tdir, sizeof tdir);
        strcat_s(tdir, sizeof tdir, "w47_prof_test");
        _snprintf_s(ppath, sizeof ppath, _TRUNCATE, "%s/profiles/a.cfg",
                    tdir);
        u8_DeleteFile(ppath);
        _snprintf_s(ppath, sizeof ppath, _TRUNCATE, "%s/profiles/b.cfg",
                    tdir);
        u8_DeleteFile(ppath);
        _snprintf_s(ppath, sizeof ppath, _TRUNCATE,
                    "%s/profiles/standard.txt", tdir);
        u8_DeleteFile(ppath);

        lcfg_defaults(&a);
        a.width = 1920;
        a.height = 1080;
        a.fps_limit = 60;
        if (lcfg_profile_save(tdir, "b", &a, err, sizeof err) == 0 &&
            lcfg_profile_save(tdir, "a", &a, err, sizeof err) == 0) {
            lcfg_defaults(&b);
            if (lcfg_profile_load(tdir, "b", &b, err, sizeof err) == 0 &&
                memcmp(&a, &b, sizeof a) == 0) {
                n = lcfg_profile_list(tdir, names, 8);
                if (n == 2 && strcmp(names[0], "a") == 0 &&
                    strcmp(names[1], "b") == 0) {
                    T_OK("10 profile-roundtrip");
                } else {
                    T_FAIL("10 profile-roundtrip", "liste n=%d", n);
                }
            } else {
                T_FAIL("10 profile-roundtrip", "%.80s", err);
            }
        } else {
            T_FAIL("10 profile-roundtrip", "%.80s", err);
        }

        if (!lcfg_profile_name_ok("a/b") && !lcfg_profile_name_ok("") &&
            !lcfg_profile_name_ok("..") &&
            lcfg_profile_save(tdir, "x/y", &a, err, sizeof err) != 0 &&
            strstr(err, "Name") && strstr(err, "x/y")) { /* W173: Glossar */
            T_OK("11 profile-badname");
        } else {
            T_FAIL("11 profile-badname", "%s", "Name nicht abgelehnt");
        }

        if (lcfg_profile_set_default(tdir, "a", err, sizeof err) == 0) {
            char dflt[64] = { 0 };
            if (lcfg_profile_get_default(tdir, dflt, sizeof dflt) == 0 &&
                strcmp(dflt, "a") == 0 &&
                lcfg_profile_set_default(tdir, "gibt-es-nicht", err,
                                         sizeof err) != 0) {
                T_OK("12 profile-default");
            } else {
                T_FAIL("12 profile-default", "%s", "get/set falsch");
            }
        } else {
            T_FAIL("12 profile-default", "%.80s", err);
        }

        if (lcfg_profile_delete(tdir, "a", err, sizeof err) == 0) {
            char dflt[64] = { 0 };
            n = lcfg_profile_list(tdir, names, 8);
            /* "a" war Standard: Markierung muss mit weg sein. */
            if (n == 1 && strcmp(names[0], "b") == 0 &&
                lcfg_profile_get_default(tdir, dflt, sizeof dflt) != 0) {
                T_OK("13 profile-delete");
            } else {
                T_FAIL("13 profile-delete", "n=%d dflt='%s'", n, dflt);
            }
            lcfg_profile_delete(tdir, "b", NULL, 0);
        } else {
            T_FAIL("13 profile-delete", "%.80s", err);
        }
    }

    /* W218 SIMPLE-CONTROLS: aim_turn/auto_gear Standard 0, Roundtrip 1 und 0 */
    {
        int ok = 1;
        u8_GetTempPath(tmp, sizeof tmp);
        strcat_s(tmp, sizeof tmp, "w218_simple_test.cfg");
        u8_DeleteFile(tmp);
        lcfg_defaults(&a);
        ok = a.aim_assist == 0 && a.auto_gear == 0;
        a.aim_assist = 2;
        a.auto_gear = 1;
        if (ok && lcfg_save_user(tmp, &a) == 0) {
            lcfg_defaults(&b);
            lcfg_load_file(tmp, &b);
            ok = b.aim_assist == 2 && b.auto_gear == 1;
            b.aim_assist = 0;
            if (ok && lcfg_save_user(tmp, &b) == 0) {
                lcfg_defaults(&a);
                a.aim_assist = 1;
                lcfg_load_file(tmp, &a);
                ok = a.aim_assist == 0 && a.auto_gear == 1;
            } else {
                ok = 0;
            }
        } else {
            ok = 0;
        }
        if (ok) {
            T_OK("W218 simple-controls cfg (Standard 0, Roundtrip)");
        } else {
            T_FAIL("W218 simple-controls cfg", "%s", "Werte falsch");
        }
        u8_DeleteFile(tmp);
    }

    /* W424 SB-ALLIED-SQUADS: Standard 1, Roundtrip 0 -> 1 ([gameplay]) */
    {
        int ok;
        u8_GetTempPath(tmp, sizeof tmp);
        strcat_s(tmp, sizeof tmp, "w424_ally_test.cfg");
        u8_DeleteFile(tmp);
        lcfg_defaults(&a);
        ok = a.sb_allied_squads == 1;
        a.sb_allied_squads = 0;
        if (ok && lcfg_save_user(tmp, &a) == 0) {
            lcfg_defaults(&b);
            lcfg_load_file(tmp, &b);
            ok = b.sb_allied_squads == 0;
            b.sb_allied_squads = 1;
            if (ok && lcfg_save_user(tmp, &b) == 0) {
                lcfg_defaults(&a);
                a.sb_allied_squads = 0;
                lcfg_load_file(tmp, &a);
                ok = a.sb_allied_squads == 1;
            } else {
                ok = 0;
            }
        } else {
            ok = 0;
        }
        if (ok) {
            T_OK("W424 allied-squads cfg (Standard 1, Roundtrip)");
        } else {
            T_FAIL("W424 allied-squads cfg", "%s", "Werte falsch");
        }
        u8_DeleteFile(tmp);
    }

    /* W268 COCKPIT-HIDE: cockpit_hide Standard 0, Roundtrip 1 -> 0 ([render]) */
    {
        int ok;
        u8_GetTempPath(tmp, sizeof tmp);
        strcat_s(tmp, sizeof tmp, "w268_chide_test.cfg");
        u8_DeleteFile(tmp);
        lcfg_defaults(&a);
        ok = a.cockpit_hide == 0;
        a.cockpit_hide = 1;
        if (ok && lcfg_save_user(tmp, &a) == 0) {
            lcfg_defaults(&b);
            lcfg_load_file(tmp, &b);
            ok = b.cockpit_hide == 1;
            b.cockpit_hide = 0;
            if (ok && lcfg_save_user(tmp, &b) == 0) {
                lcfg_defaults(&a);
                a.cockpit_hide = 1;
                lcfg_load_file(tmp, &a);
                ok = a.cockpit_hide == 0;
            } else {
                ok = 0;
            }
        } else {
            ok = 0;
        }
        if (ok) {
            T_OK("W268 cockpit_hide cfg (Standard 0, Roundtrip)");
        } else {
            T_FAIL("W268 cockpit_hide cfg", "%s", "Werte falsch");
        }
        u8_DeleteFile(tmp);
    }
    /* W319 MONITOR-VIEW: monitor_view Standard 0, Roundtrip 2 -> 1 -> 0, ungueltige Werte -> 0 ([render]) */
    {
        int ok, k;
        static const int kv[3] = { 2, 1, 0 };
        u8_GetTempPath(tmp, sizeof tmp);
        strcat_s(tmp, sizeof tmp, "w319_mview_test.cfg");
        lcfg_defaults(&a);
        ok = a.monitor_view == 0;
        for (k = 0; k < 3 && ok; k++) {
            u8_DeleteFile(tmp);
            lcfg_defaults(&a);
            a.monitor_view = kv[k];
            ok = lcfg_save_user(tmp, &a) == 0;
            lcfg_defaults(&b);
            b.monitor_view = 7;
            if (ok) { lcfg_load_file(tmp, &b); }
            ok = ok && b.monitor_view == kv[k];
        }
        if (ok) { /* ungueltiger Wert in der Datei -> 0 */
            FILE *f = NULL;
            u8_DeleteFile(tmp);
            if (u8_fopen_s(&f, tmp, "w") == 0 && f) {
                fputs("[render]\nmonitor_view=5\n", f);
                fclose(f);
                lcfg_defaults(&b);
                lcfg_load_file(tmp, &b);
                ok = b.monitor_view == 0;
            } else {
                ok = 0;
            }
        }
        if (ok) {
            T_OK("W319 monitor_view cfg (Standard 0, Roundtrip 2/1/0, Klemmung)");
        } else {
            T_FAIL("W319 monitor_view cfg", "%s", "Werte falsch");
        }
        u8_DeleteFile(tmp);
    }
    /* W271 STICK-RELATIVE: stick_aim/stick_aim_sens (Standard position/4,
     * Roundtrip, Klemmung) + Stick-Modus je Quelle im Belegungsdialog
     * (lctrl_stick_mode_get/_set) + Aktion aim_center. */
    {
        static lctrl_bind_t sb;
        int ok = 1, ix, ir, ic;
        char why[96] = "";
        FILE *f = NULL;
        u8_GetTempPath(tmp, sizeof tmp);
        strcat_s(tmp, sizeof tmp, "w271_stick_test.cfg");
        u8_DeleteFile(tmp);
        lcfg_defaults(&a);
        if (a.stick_aim != 0 || a.stick_aim_sens != 4) { ok = 0; strcpy_s(why, sizeof why, "Standard"); }
        a.stick_aim = 1;
        a.stick_aim_sens = 9;
        if (ok && lcfg_save_user(tmp, &a) == 0) {
            char buf[8192];
            size_t n = 0;
            lcfg_defaults(&b);
            lcfg_load_file(tmp, &b);
            if (b.stick_aim != 1 || b.stick_aim_sens != 9) { ok = 0; strcpy_s(why, sizeof why, "Roundtrip"); }
            if (u8_fopen_s(&f, tmp, "rb") == 0 && f) {
                n = fread(buf, 1, sizeof buf - 1, f);
                fclose(f);
            }
            buf[n] = '\0';
            if (ok && !strstr(buf, "relative")) { ok = 0; strcpy_s(why, sizeof why, "Datei ohne relative"); }
        } else if (ok) {
            ok = 0;
            strcpy_s(why, sizeof why, "speichern");
        }
        if (ok && (f = NULL, u8_fopen_s(&f, tmp, "wb") == 0) && f) {
            fputs("[input]\nstick_aim=position\nstick_aim_sens=99\n", f);
            fclose(f);
            lcfg_defaults(&b);
            b.stick_aim = 1;
            lcfg_load_file(tmp, &b);
            if (b.stick_aim != 0 || b.stick_aim_sens != 16) { ok = 0; strcpy_s(why, sizeof why, "position/Klemmung"); }
        }
        u8_DeleteFile(tmp);
        memset(&sb, 0, sizeof sb);
        ix = lctrl_bind_elem_index("aim_x");
        ir = lctrl_bind_elem_index("rot");
        ic = lctrl_bind_elem_index("aim_center");
        if (ok && (ix < 0 || ir < 0 || ic < 0 || kCtrlElems[ic].kind != LCTRL_ACT)) {
            ok = 0; strcpy_s(why, sizeof why, "Elemente");
        }
        if (ok) {
            lctrl_bind_set(&sb, ix, "MOUSE:X:mode=aim, PAD:LX:dz=0.15:curve=1.5, JOY0:X:dz=0.05");
            lctrl_bind_set(&sb, ir, "PAD:RX:dz=0.12");
            if (lctrl_stick_mode_get(&sb, ix) != 0 || lctrl_stick_mode_get(&sb, ir) != -1 ||
                !(lctrl_bind_detail_vis(&sb, ix) & LCTRL_VIS_SMODE) ||
                (lctrl_bind_detail_vis(&sb, ir) & LCTRL_VIS_SMODE)) {
                ok = 0; strcpy_s(why, sizeof why, "get/vis");
            }
        }
        if (ok && (lctrl_stick_mode_set(&sb, ix, 2) != 0 || lctrl_stick_mode_get(&sb, ix) != 2 ||
                   strcmp(sb.row[ix].alt[0], "MOUSE:X:mode=aim") != 0 ||
                   strcmp(sb.row[ix].alt[1], "PAD:LX:dz=0.15:curve=1.5:mode=rate") != 0 ||
                   strcmp(sb.row[ix].alt[2], "JOY0:X:dz=0.05:mode=rate") != 0)) {
            ok = 0; _snprintf_s(why, sizeof why, _TRUNCATE, "set rate: %s", sb.row[ix].alt[1]);
        }
        if (ok && (lctrl_stick_mode_set(&sb, ix, 1) != 0 || lctrl_stick_mode_get(&sb, ix) != 1 ||
                   !strstr(sb.row[ix].alt[2], ":mode=position"))) {
            ok = 0; strcpy_s(why, sizeof why, "set position");
        }
        if (ok && (lctrl_stick_mode_set(&sb, ix, 0) != 0 || lctrl_stick_mode_get(&sb, ix) != 0 ||
                   strstr(sb.row[ix].alt[1], "mode=") || lctrl_stick_mode_set(&sb, ir, 2) == 0 ||
                   lctrl_stick_src("PAD:LT:dz=0.05") || lctrl_stick_src("JOY0:Z+") ||
                   !lctrl_stick_src("PAD:RY:inv=1"))) {
            ok = 0; strcpy_s(why, sizeof why, "global/rot/Quellen");
        }
        if (ok) {
            T_OK("W271 stick-aim cfg + Stick-Modus je Quelle + aim_center");
        } else {
            T_FAIL("W271 stick-aim", "%s", why);
        }
    }

    /* W236 AIM-ASSIST: Altwert aim_turn=1 (alpha.6) -> Stufe 1; Speichern
     * schreibt aim_assist und neutralisiert aim_turn (=0). */
    {
        int ok = 0;
        FILE *f = NULL;
        u8_GetTempPath(tmp, sizeof tmp);
        strcat_s(tmp, sizeof tmp, "w236_assist_test.cfg");
        u8_DeleteFile(tmp);
        if (u8_fopen_s(&f, tmp, "wb") == 0 && f) {
            fputs("[input]\r\naim_turn=1\r\n", f);
            fclose(f);
            lcfg_defaults(&a);
            lcfg_load_file(tmp, &a);
            ok = a.aim_assist == 1;
            if (ok && lcfg_save_user(tmp, &a) == 0) {
                lcfg_defaults(&b);
                lcfg_load_file(tmp, &b);
                ok = b.aim_assist == 1;
                b.aim_assist = 0;
                ok = ok && lcfg_save_user(tmp, &b) == 0;
                lcfg_defaults(&a);
                lcfg_load_file(tmp, &a);
                ok = ok && a.aim_assist == 0; /* aim_turn=0 geschrieben */
            } else {
                ok = 0;
            }
        }
        if (ok) {
            T_OK("W236 aim-assist cfg (Altwert aim_turn=1 -> 1, neutralisiert)");
        } else {
            T_FAIL("W236 aim-assist cfg", "%s", "Abbildung/Roundtrip falsch");
        }
        u8_DeleteFile(tmp);
    }

    /* W234 PAL50: game_tick Standard 30, Roundtrip 25 und 30, fremde Werte -> 30 */
    {
        int ok = 1;
        FILE *f = NULL;
        u8_GetTempPath(tmp, sizeof tmp);
        strcat_s(tmp, sizeof tmp, "w234_pal50_test.cfg");
        u8_DeleteFile(tmp);
        lcfg_defaults(&a);
        ok = a.game_tick == 30;
        a.game_tick = 25;
        if (ok && lcfg_save_user(tmp, &a) == 0) {
            lcfg_defaults(&b);
            lcfg_load_file(tmp, &b);
            ok = b.game_tick == 25;
            b.game_tick = 30;
            if (ok && lcfg_save_user(tmp, &b) == 0) {
                lcfg_defaults(&a);
                a.game_tick = 25;
                lcfg_load_file(tmp, &a);
                ok = a.game_tick == 30;
            } else {
                ok = 0;
            }
        } else {
            ok = 0;
        }
        if (ok && u8_fopen_s(&f, tmp, "w") == 0 && f) { /* Handwert 50 -> Standard 30 */
            fputs("[game]\ngame_tick=50\n", f);
            fclose(f);
            lcfg_defaults(&a);
            a.game_tick = 25;
            lcfg_load_file(tmp, &a);
            ok = a.game_tick == 30;
        }
        if (ok) {
            T_OK("W234 pal50 cfg (Standard 30, Roundtrip 25/30, 50 -> 30)");
        } else {
            T_FAIL("W234 pal50 cfg", "%s", "Werte falsch");
        }
        u8_DeleteFile(tmp);
    }

    /* 14 (W78): neue Keys Roundtrip + fremde Keys bleiben erhalten */
    {
        u8_GetTempPath(tmp, sizeof tmp);
        strcat_s(tmp, sizeof tmp, "w78_ctrl_test.cfg");
        u8_DeleteFile(tmp);
        lcfg_defaults(&a);
        strcpy_s(a.input_profile, sizeof a.input_profile, "xbox_pad_default");
        a.sbc_usb = 1;
        a.sbc_mouse_off = 0; /* W128: Roundtrip auch fuer 0 */
        a.interp_fps = 60;
        if (lcfg_save_user(tmp, &a) != 0) {
            T_FAIL("14 ctrl-roundtrip", "%s", "save scheiterte");
        } else {
            lcfg_defaults(&b);
            lcfg_load_file(tmp, &b);
            if (memcmp(&a, &b, sizeof a) == 0) {
                f = NULL;
                if (u8_fopen_s(&f, tmp, "a") == 0 && f) {
                    fputs("[input]\nfremder_ctrl_key=7\n", f);
                    fclose(f);
                }
                lcfg_defaults(&b);
                strcpy_s(b.input_profile, sizeof b.input_profile,
                         "user_mein");
                b.sbc_usb = 0;
                b.interp_fps = 0;
                if (lcfg_save_user(tmp, &b) == 0) {
                    FILE *g = NULL;
                    size_t n = 0;
                    memset(buf, 0, sizeof buf);
                    if (u8_fopen_s(&g, tmp, "r") == 0 && g) {
                        n = fread(buf, 1, sizeof buf - 1, g);
                        fclose(g);
                    }
                    (void)n;
                    if (strstr(buf, "fremder_ctrl_key=7") &&
                        strstr(buf, "input_profile=user_mein") &&
                        strstr(buf, "sbc_usb=0") &&
                        strstr(buf, "interp_fps=0")) {
                        T_OK("14 ctrl-roundtrip");
                    } else {
                        T_FAIL("14 ctrl-roundtrip", "%s", "Keys fehlen");
                    }
                } else {
                    T_FAIL("14 ctrl-roundtrip", "%s", "save 2 scheiterte");
                }
            } else {
                T_FAIL("14 ctrl-roundtrip", "%s", "Werte ungleich");
            }
        }
        u8_DeleteFile(tmp);
    }

    /* 15 (W78): Validierung der neuen Keys */
    {
        int ok = 1;
        st_valid_base(&b);
        b.interp_fps = 30;
        if (lcfg_validate(&b, err, sizeof err) == 0 ||
            !strstr(err, "Bildrate")) {
            ok = 0;
        }
        st_valid_base(&b);
        b.sbc_usb = 2;
        if (lcfg_validate(&b, err, sizeof err) == 0 ||
            !strstr(err, "sbc_usb")) {
            ok = 0;
        }
        st_valid_base(&b);
        b.sbc_mouse_off = 3; /* W128 */
        if (lcfg_validate(&b, err, sizeof err) == 0 ||
            !strstr(err, "sbc_mouse_off")) {
            ok = 0;
        }
        st_valid_base(&b);
        strcpy_s(b.input_profile, sizeof b.input_profile, "a;b");
        if (lcfg_validate(&b, err, sizeof err) == 0 ||
            !strstr(err, "Tastenbelegung")) { /* W173: Glossar "Tastenbelegung" */
            ok = 0;
        }
        st_valid_base(&b);
        strcpy_s(b.input_profile, sizeof b.input_profile, "user_mein");
        b.interp_fps = 0;
        if (lcfg_validate(&b, err, sizeof err) != 0) {
            ok = 0;
        }
        if (ok) {
            T_OK("15 ctrl-validate");
        } else {
            T_FAIL("15 ctrl-validate", "%.100s", err);
        }
    }

    /* 16 (W78): Bind-Modell parsen/setzen/speichern */
    {
        char bp[1024];
        static lctrl_bind_t bd; /* W414: static (Stapelrahmen cli_selftest) */
        char row[420];
        u8_GetTempPath(bp, sizeof bp);
        strcat_s(bp, sizeof bp, "w78_bind_test.ini");
        f = NULL;
        if (u8_fopen_s(&f, bp, "w") == 0 && f) {
            fputs("# Kommentar\n[buttons]\n"
                  "bi0 = KEY:SPACE, MOUSE:LEFT # inline\n"
                  "lockon = KEY:L\n; semikolon\nmuell ohne gleich\n"
                  "aim_x.neg = KEY:LEFT:cond=!SHIFT\n"
                  "gear.up = KEY:E, PAD:BACK+PAD:RB\n",
                  f);
            fclose(f);
        }
        lctrl_bind_init(&bd);
        if (lctrl_bind_load_file(bp, &bd) == 0 &&
            lctrl_bind_elem_index("bi0") == 0 &&
            lctrl_bind_elem_index("mainweapon") == 0 &&
            lctrl_bind_elem_index("LOCKON") == 2 &&
            lctrl_bind_elem_index("aim_x_neg") ==
                lctrl_bind_elem_index("aim_x.neg") &&
            lctrl_bind_elem_index("gibt-es-nicht") < 0) {
            lctrl_bind_row_text(&bd, 0, row, sizeof row);
            if (strcmp(row, "KEY:SPACE, MOUSE:LEFT") == 0 &&
                bd.row[2].n_alt == 1 &&
                lctrl_bind_add(&bd, 2, "PAD:LB") == 0 &&
                lctrl_bind_add(&bd, 2, "PAD:LB") == 1) {
                char sp[1024], err2[256];
                static lctrl_bind_t bd2; /* W414: static */
                u8_GetTempPath(sp, sizeof sp);
                strcat_s(sp, sizeof sp, "w78_bind_out.ini");
                u8_DeleteFile(sp);
                if (lctrl_bind_save(sp, &bd, "t", err2, sizeof err2) == 0 &&
                    lctrl_bind_load_file(sp, &bd2) == 0) {
                    char r1[420], r2[420];
                    lctrl_bind_row_text(&bd, 2, r1, sizeof r1);
                    lctrl_bind_row_text(&bd2, 2, r2, sizeof r2);
                    if (strcmp(r1, r2) == 0 &&
                        strcmp(r1, "KEY:L, PAD:LB") == 0) {
                        T_OK("16 bind-model");
                    } else {
                        T_FAIL("16 bind-model", "%.50s vs %.50s", r1, r2);
                    }
                } else {
                    T_FAIL("16 bind-model", "%.80s", err2);
                }
                u8_DeleteFile(sp);
            } else {
                T_FAIL("16 bind-model", "%.80s", row);
            }
        } else {
            T_FAIL("16 bind-model", "%s", "parse/index falsch");
        }
        u8_DeleteFile(bp);
    }

    /* 17 (W78): Element-Tabelle + Namen + Konflikte */
    {
        int i, ok = 1;
        for (i = 0; i < LCTRL_ELEM_N; i++) {
            if (lctrl_bind_elem_index(kCtrlElems[i].key) != i) {
                ok = 0;
                break;
            }
        }
        {
            char nm[32], c[4][160];
            static lctrl_bind_t bc; /* W414: static */
            int nc;
            lctrl_key_name(VK_SPACE, nm, sizeof nm);
            if (strcmp(nm, "SPACE") != 0) {
                ok = 0;
            }
            lctrl_key_name('G', nm, sizeof nm);
            if (strcmp(nm, "G") != 0) {
                ok = 0;
            }
            lctrl_key_name(VK_F5, nm, sizeof nm);
            if (strcmp(nm, "F5") != 0) {
                ok = 0;
            }
            lctrl_key_name(VK_LCONTROL, nm, sizeof nm);
            if (strcmp(nm, "LCTRL") != 0) {
                ok = 0;
            }
            lctrl_key_name(VK_RCONTROL, nm, sizeof nm);
            if (strcmp(nm, "RCTRL") != 0) {
                ok = 0;
            }
            if (!lctrl_user_rest_ok("mein") || lctrl_user_rest_ok("a/b") ||
                lctrl_user_rest_ok("") ||
                !lctrl_name_shipped("keyboard_default") ||
                !lctrl_name_shipped("xbox_pad_default") ||
                !lctrl_name_shipped("default_full") ||
                lctrl_name_shipped("user_mein")) {
                ok = 0;
            }
            lctrl_bind_init(&bc);
            lctrl_bind_set(&bc, 0, "KEY:G");
            lctrl_bind_set(&bc, 5, "KEY:G");
            lctrl_bind_set(&bc, 6, "KEY:H");
            nc = lctrl_bind_conflicts(&bc, c, 4);
            if (nc != 1 || !strstr(c[0], "KEY:G") || !strstr(c[0], "bi0") ||
                !strstr(c[0], "bi5")) {
                ok = 0;
            }
        }
        if (ok) {
            T_OK("17 bind-names");
        } else {
            T_FAIL("17 bind-names", "%s", "Tabelle/Name/Konflikt falsch");
        }
    }

    /* 18 (W78): Eingabe-Profile auflisten (*.ini, sortiert) */
    {
        char td[1024], p1[1200], p2[1200], px[1200], dd[1200];
        char names[8][64];
        int n;
        u8_GetTempPath(td, sizeof td);
        strcat_s(td, sizeof td, "w78_scan_test");
        _snprintf_s(dd, sizeof dd, _TRUNCATE, "%s/profiles", td);
        lcfg_mkdir_p(dd);
        _snprintf_s(p1, sizeof p1, _TRUNCATE, "%s/profiles/b.ini", td);
        _snprintf_s(p2, sizeof p2, _TRUNCATE, "%s/profiles/a.ini", td);
        _snprintf_s(px, sizeof px, _TRUNCATE, "%s/profiles/x.cfg", td);
        f = NULL;
        if (u8_fopen_s(&f, p1, "w") == 0 && f) {
            fputs("bi0 = KEY:G\n", f);
            fclose(f);
        }
        if (u8_fopen_s(&f, p2, "w") == 0 && f) {
            fputs("bi0 = KEY:H\n", f);
            fclose(f);
        }
        if (u8_fopen_s(&f, px, "w") == 0 && f) {
            fputs("width=1\n", f);
            fclose(f);
        }
        n = lctrl_input_profiles(td, names, 8);
        if (n == 2 && strcmp(names[0], "a") == 0 &&
            strcmp(names[1], "b") == 0) {
            T_OK("18 input-scan");
        } else {
            T_FAIL("18 input-scan", "n=%d", n);
        }
        u8_DeleteFile(px);
        u8_DeleteFile(p1);
        u8_DeleteFile(p2);
    }

    /* 19 (W79): Spielerbetrieb-Block neutralisiert Test-Vorgaben, fremde
     * Zeilen bleiben, Roundtrip der Launcher-Keys bleibt intakt. */
    {
        char pb[1024];
        int ok = 1, k;
        u8_GetTempPath(pb, sizeof pb);
        strcat_s(pb, sizeof pb, "w79_player_test.cfg");
        f = NULL;
        if (u8_fopen_s(&f, pb, "w") == 0 && f) {
            fputs("# bleibt\n[input]\nreplay_steps=f+500>73,20\n"
                  "movie_skip=1\nwidth=800\nreplay_steps=dublette\n", f);
            fclose(f);
        }
        if (lcfg_write_player_block(pb) != 0) ok = 0;
        lcfg_defaults(&a);
        a.width = 1280;
        if (ok && lcfg_save_user(pb, &a) != 0) ok = 0;
        memset(buf, 0, sizeof buf);
        f = NULL;
        if (u8_fopen_s(&f, pb, "r") == 0 && f) {
            (void)fread(buf, 1, sizeof buf - 1, f);
            fclose(f);
        }
        for (k = 0; ok && k < lcfg_player_block_n(); k++) {
            const char *v = NULL;
            const char *key = lcfg_player_block_key(k, &v);
            char want[128];
            _snprintf_s(want, sizeof want, _TRUNCATE, "\n%s=%s\n", key, v);
            if (!strstr(buf, want)) ok = 0;
        }
        if (ok && (strstr(buf, "f+500") || strstr(buf, "dublette") ||
                   !strstr(buf, "# bleibt") || !strstr(buf, "width=1280")))
            ok = 0;
        lcfg_defaults(&b);
        lcfg_load_file(pb, &b);
        if (ok && memcmp(&a, &b, sizeof a) != 0) ok = 0;
        if (ok) {
            T_OK("19 player-block");
        } else {
            T_FAIL("19 player-block", "%.200s", buf);
        }
        u8_DeleteFile(pb);
    }

    /* 19b (W87): Maus-Modus - sb_native.cfg (relative, Test-Standard)
     * ueberdeckt den Spieler-Standard absolut nicht; user.cfg gewinnt. */
    {
        char td[1024], pp[1200];
        int ok = 1;
        u8_GetTempPath(td, sizeof td);
        strcat_s(td, sizeof td, "w87_mouse_test");
        lcfg_mkdir_p(td);
        _snprintf_s(pp, sizeof pp, _TRUNCATE, "%s/sb_native.cfg", td);
        f = NULL;
        if (u8_fopen_s(&f, pp, "w") == 0 && f) {
            fputs("[input]\nmouse_mode=relative\nmouse_sens=4\n", f);
            fclose(f);
        } else {
            ok = 0;
        }
        _snprintf_s(pp, sizeof pp, _TRUNCATE, "%s/user.cfg", td);
        u8_DeleteFile(pp);
        app_load_effective(td, &a, NULL, 0, NULL, "de");
        if (a.mouse_mode != 1 || a.mouse_sens != 4) ok = 0;
        a.mouse_mode = 0;
        a.mouse_sens = 9;
        if (ok && lcfg_save_user(pp, &a) != 0) ok = 0;
        app_load_effective(td, &b, NULL, 0, NULL, "de");
        if (b.mouse_mode != 0 || b.mouse_sens != 9) ok = 0;
        memset(buf, 0, sizeof buf);
        f = NULL;
        if (u8_fopen_s(&f, pp, "r") == 0 && f) {
            (void)fread(buf, 1, sizeof buf - 1, f);
            fclose(f);
        }
        if (!strstr(buf, "\nmouse_mode=relative\n") ||
            !strstr(buf, "\nmouse_sens=9\n"))
            ok = 0;
        u8_DeleteFile(pp);
        _snprintf_s(pp, sizeof pp, _TRUNCATE, "%s/sb_native.cfg", td);
        u8_DeleteFile(pp);
        if (ok) {
            T_OK("19b mouse-mode");
        } else {
            T_FAIL("19b mouse-mode", "mode=%d/%d sens=%d/%d", a.mouse_mode,
                   b.mouse_mode, a.mouse_sens, b.mouse_sens);
        }
    }

    /* 19c (W170 LAUNCHER-P1/B12): Startfolge (save + Spielerbetrieb-Block)
     * laesst eine feste Render-Skalierung stehen; auto bleibt auto. */
    {
        char pb[1024];
        int ok = 1;
        u8_GetTempPath(pb, sizeof pb);
        strcat_s(pb, sizeof pb, "w170_rtscale_test.cfg");
        u8_DeleteFile(pb);
        lcfg_defaults(&a);
        a.rt_scale = 2;
        a.rt_scale_dyn = 0;
        if (lcfg_save_user(pb, &a) != 0) ok = 0;
        if (ok && lcfg_write_player_block(pb) != 0) ok = 0;
        memset(buf, 0, sizeof buf);
        f = NULL;
        if (u8_fopen_s(&f, pb, "r") == 0 && f) {
            (void)fread(buf, 1, sizeof buf - 1, f);
            fclose(f);
        } else {
            ok = 0;
        }
        if (ok && (!strstr(buf, "\nrt_scale=2\n") ||
                   strstr(buf, "rt_scale=auto")))
            ok = 0;
        lcfg_defaults(&b);
        lcfg_load_file(pb, &b);
        if (ok && (b.rt_scale != 2 || b.rt_scale_dyn != 0)) ok = 0;
        /* UI-Wert auto (Spieler-Standard) -> auto in der Datei. */
        lcfg_defaults(&a);
        if (ok && lcfg_save_user(pb, &a) != 0) ok = 0;
        if (ok && lcfg_write_player_block(pb) != 0) ok = 0;
        memset(buf, 0, sizeof buf);
        f = NULL;
        if (u8_fopen_s(&f, pb, "r") == 0 && f) {
            (void)fread(buf, 1, sizeof buf - 1, f);
            fclose(f);
        } else {
            ok = 0;
        }
        if (ok && !strstr(buf, "\nrt_scale=auto\n")) ok = 0;
        u8_DeleteFile(pb);
        /* Ladeweg: sb_native.cfg mit rt_scale=1 (Testlaeufe) setzt den
         * Spieler-Standard nicht auf 1; user.cfg mit 2 gewinnt. */
        {
            char td[1024], sp[1200];
            u8_GetTempPath(td, sizeof td);
            strcat_s(td, sizeof td, "w170_rtscale_load");
            lcfg_mkdir_p(td);
            _snprintf_s(sp, sizeof sp, _TRUNCATE, "%s/sb_native.cfg", td);
            f = NULL;
            if (u8_fopen_s(&f, sp, "w") == 0 && f) {
                fputs("[render]\nrt_scale=1\n", f);
                fclose(f);
            } else {
                ok = 0;
            }
            _snprintf_s(sp, sizeof sp, _TRUNCATE, "%s/user.cfg", td);
            u8_DeleteFile(sp);
            app_load_effective(td, &a, NULL, 0, NULL, "de");
            if (a.rt_scale != 0 || a.rt_scale_dyn != 0) ok = 0;
            lcfg_defaults(&b);
            b.rt_scale = 2;
            if (ok && lcfg_save_user(sp, &b) != 0) ok = 0;
            app_load_effective(td, &a, NULL, 0, NULL, "de");
            if (a.rt_scale != 2) ok = 0;
            u8_DeleteFile(sp);
            _snprintf_s(sp, sizeof sp, _TRUNCATE, "%s/sb_native.cfg", td);
            u8_DeleteFile(sp);
        }
        if (ok) {
            T_OK("19c player-rtscale");
        } else {
            T_FAIL("19c player-rtscale", "%.200s", buf);
        }
    }

    /* 26 (W103 REM-FRAME, W173): [remaster]-Keys Roundtrip, Standard =
     * Original, Ableitungen des Grafik-Presets (remaster = HD-Texturen,
     * fx_enhanced = Effekte an, Platzhalter 0), Section [remaster]. */
    {
        int ok = 1;
        u8_GetTempPath(tmp, sizeof tmp);
        strcat_s(tmp, sizeof tmp, "w103_rem_test.cfg");
        u8_DeleteFile(tmp);
        lcfg_defaults(&a);
        if (a.remaster != 0 || a.hd_textures != 0 || a.hd_scale != 4 ||
            lcfg_gfx_preset(&a, 0) != LGP_ORIGINAL) {
            ok = 0;
        }
        lcfg_gfx_apply(&a, LGP_SUBTLE);
        if (a.remaster != 1 || a.hd_textures != 1 || a.fx_enhanced != 1 ||
            a.ultrawide != 0 || a.postfx != 0 || a.fx[LFX_SOFT] != 60) {
            ok = 0;
        }
        a.hd_scale = 2;
        if (ok && lcfg_save_user(tmp, &a) == 0) {
            FILE *g = NULL;
            lcfg_defaults(&b);
            lcfg_load_file(tmp, &b);
            if (memcmp(&a, &b, sizeof a) != 0) {
                ok = 0;
            }
            memset(buf, 0, sizeof buf);
            if (u8_fopen_s(&g, tmp, "r") == 0 && g) {
                (void)fread(buf, 1, sizeof buf - 1, g);
                fclose(g);
            }
            if (!strstr(buf, "[remaster]\nremaster=1\n") ||
                !strstr(buf, "hd_scale=2") || !strstr(buf, "postfx=0") ||
                !strstr(buf, "fx_soft=60")) {
                ok = 0;
            }
            lcfg_gfx_apply(&b, LGP_ORIGINAL);
            if (b.remaster != 0 || b.hd_textures != 0 || b.fx_enhanced != 0 ||
                lcfg_save_user(tmp, &b) != 0) {
                ok = 0;
            }
            lcfg_defaults(&a);
            lcfg_load_file(tmp, &a);
            if (a.remaster != 0 || a.hd_textures != 0 || a.hd_scale != 2 ||
                a.fx[LFX_SOFT] != 0) {
                ok = 0;
            }
            /* W114: alte cfg mit hd_scale=0 (W111 Auswahl) laeuft weiter als
             * 4x (Freigaben gelten immer) */
            a.remaster = 1;
            a.hd_scale = 0;
            if (lcfg_save_user(tmp, &a) != 0) {
                ok = 0;
            }
            lcfg_defaults(&b);
            lcfg_load_file(tmp, &b);
            if (b.hd_scale != 4) {
                ok = 0;
            }
        } else {
            ok = 0;
        }
        u8_DeleteFile(tmp);
        if (ok) {
            T_OK("26 remaster-roundtrip");
        } else {
            T_FAIL("26 remaster-roundtrip", "%s", "Keys/Ableitungen falsch");
        }
    }

    /* 28 (W131 SHADER-OPT2, W173 LAUNCHER-P4): EIN Grafik-Preset setzt alle
     * Optik-Werte (Shader + Effekte + HD), Standard = Original (shading=1 wie
     * der Client), Roundtrip ueber user.cfg, Einzelwert -> Benutzerdefiniert,
     * "Original" ist wirklich original (shading/fx werden gesetzt). */
    {
        int ok = 1;
        u8_GetTempPath(tmp, sizeof tmp);
        strcat_s(tmp, sizeof tmp, "w131_shx_test.cfg");
        u8_DeleteFile(tmp);
        lcfg_defaults(&a);
        if (lcfg_gfx_preset(&a, 0) != LGP_ORIGINAL || a.shx[LSHX_AA] != 0 ||
            a.shx[LSHX_SAT] != 100 || a.shx[LSHX_SHADING] != 1 || a.fx[LFX_EMBERS] != 0) {
            ok = 0;
        }
        lcfg_gfx_apply(&a, LGP_STRONG); /* W465: Spalte bleibt, ist aber nicht waehlbar -> Erkennung Custom */
        if (lcfg_gfx_preset(&a, 0) != LGP_CUSTOM || lcfg_gfx_diff(&a, LGP_STRONG, 0, NULL) != 0 ||
            a.shx[LSHX_SHADING] != 1 ||
            a.shx[LSHX_AA] != 3 || a.shx[LSHX_SHADOW] != 1 || a.shx[LSHX_SOFT] != 1 ||
            a.shx[LSHX_FOG] != 100 || a.fx[LFX_SOFT] != 100 || a.fx[LFX_EMBERS] != 120 ||
            !a.hd_textures || a.shx[LSHX_LABEL] != 0) {
            ok = 0;
        }
        if (ok && lcfg_save_user(tmp, &a) == 0) {
            FILE *g = NULL;
            lcfg_defaults(&b);
            lcfg_load_file(tmp, &b);
            if (memcmp(&a, &b, sizeof a) != 0 || lcfg_gfx_diff(&b, LGP_STRONG, 0, NULL) != 0) {
                ok = 0;
            }
            memset(buf, 0, sizeof buf);
            if (u8_fopen_s(&g, tmp, "r") == 0 && g) {
                (void)fread(buf, 1, sizeof buf - 1, g);
                fclose(g);
            }
            if (!strstr(buf, "aa_mode=3") || !strstr(buf, "shade_shadow=1") ||
                !strstr(buf, "color_sat=115") || !strstr(buf, "fx_embers=120") ||
                !strstr(buf, "hud_label_avoid=0")) {
                ok = 0;
            }
            b.shx[LSHX_SAT] = 120; /* Einzelwert (ESC-Menue) -> Benutzerdefiniert */
            if (lcfg_gfx_preset(&b, 0) != LGP_CUSTOM || lcfg_gfx_diff(&b, LGP_STRONG, 0, NULL) != 1 ||
                lcfg_gfx_migrate_remaster(&b) != 0) { /* W465: Custom bleibt Custom */
                ok = 0;
            }
            lcfg_gfx_apply(&b, LGP_ORIGINAL); /* W173: setzt shading/fx/HD explizit */
            if (lcfg_gfx_preset(&b, 0) != LGP_ORIGINAL || b.shx[LSHX_SHADING] != 1 ||
                b.shx[LSHX_AA] != 0 || b.shx[LSHX_SHADOW] != 0 || b.fx[LFX_SOFT] != 0 ||
                b.remaster != 0 || b.hd_textures != 0) {
                ok = 0;
            }
            b.shx[LSHX_SHADING] = 0; /* Weltbeleuchtung aus = nicht mehr Original */
            if (lcfg_gfx_preset(&b, 0) != LGP_CUSTOM || lcfg_gfx_base(&b, 0) != LGP_ORIGINAL) {
                ok = 0;
            }
            lcfg_gfx_apply(&b, LGP_SUBTLE);
            if (lcfg_gfx_preset(&b, 0) != LGP_CUSTOM || lcfg_gfx_diff(&b, LGP_SUBTLE, 0, NULL) != 0 ||
                b.shx[LSHX_SSAO] != 25 || b.fx[LFX_SOFT] != 60 || b.shx[LSHX_SHADING] != 1) {
                ok = 0;
            }
            /* W465: exakt Remastered -> Original */
            if (lcfg_gfx_migrate_remaster(&b) != 1 || lcfg_gfx_preset(&b, 0) != LGP_ORIGINAL) {
                ok = 0;
            }
        } else {
            ok = 0;
        }
        u8_DeleteFile(tmp);
        if (ok) {
            T_OK("28 gfx-preset");
        } else {
            T_FAIL("28 gfx-preset", "%s", "Preset/Roundtrip falsch");
        }
    }

    /* 52 (W173 LAUNCHER-P4): Preset <-> Werte fuer alle drei + Benutzer-
     * definiert, gleiche Tabelle wie der Client (sb_gfxpreset.h), Effect-
     * Pack-Ausnahme, hud_label_avoid unabhaengig, Migration alter Dateien. */
    {
        char why[200] = "";
        int p, k;
        for (p = 0; p < SB_GP_NPRE && !why[0]; p++) {
            /* W465: Erkennung nur ueber die sichtbaren Presets des Spiels (Enhanced: LoC; Remastered: keins) */
            int loc = !sb_gp_visible(p, 0) && sb_gp_visible(p, 1), vis = sb_gp_visible(p, loc);
            int exp = vis ? p : LGP_CUSTOM;
            lcfg_gfx_set_loc(loc);
            lcfg_defaults(&a);
            lcfg_gfx_apply(&a, p);
            if (lcfg_gfx_preset(&a, 0) != exp || lcfg_gfx_diff(&a, p, 0, NULL) != 0) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "Preset %d nicht erkannt", p);
            }
            for (k = 0; k < LGK_N && !why[0]; k++) { /* Sollwerte = Client-Tabelle */
                int r = sb_gp_find(lcfg_gfx_key(k)), want;
                if (!lcfg_gfx_owned(k)) {
                    if (r >= 0 || lcfg_gfx_preset_value(p, k) != -1) {
                        _snprintf_s(why, sizeof why, _TRUNCATE, "%s gehoert nicht zum Preset",
                                    lcfg_gfx_key(k));
                    }
                    continue;
                }
                want = r >= 0 ? kSbGfxPreset[r].v[p] : -99;
                if (k == LGK_FX0 + LFX_INTERP) { want = want != 0; }
                if (r < 0 || lcfg_gfx_value(&a, k) != want) {
                    _snprintf_s(why, sizeof why, _TRUNCATE, "P%d %s=%d, Tabelle %d", p,
                                lcfg_gfx_key(k), lcfg_gfx_value(&a, k), want);
                }
            }
            for (k = 0; k < LGK_N && !why[0]; k++) { /* jede Einzelaenderung -> Custom */
                b = a;
                if (!lcfg_gfx_owned(k)) {
                    continue;
                }
                if (k < LSHX_N) {
                    b.shx[k] = b.shx[k] ? 0 : 1;
                } else if (k == LGK_FX0 + LFX_INTERP) {
                    b.fx[LFX_INTERP] = !b.fx[LFX_INTERP];
                } else if (k < LGK_HD) {
                    b.fx[k - LGK_FX0] += 7;
                } else if (k == LGK_FPS) { b.interp_fps = 0;
                } else if (k == LGK_RT) { b.rt_scale = 4;
                } else if (k == LGK_DYN) { b.rt_scale_dyn = 1;
                } else if (k == LGK_MATCH) { b.fs_interp_match = !b.fs_interp_match;
                } else if (k == LGK_MFX) { b.monitor_fx = !b.monitor_fx; /* W252 */
                } else if (k == LGK_EDGE) { b.edge_mode = b.edge_mode == 2 ? 0 : 2; /* W266 */
                } else if (k == LGK_EDGEW) { b.edge_fade_width += 4;
                } else if (k == LGK_EDGES) { b.edge_fade_strength -= 10;
                } else {
                    b.remaster = b.hd_textures = !b.hd_textures;
                }
                /* W252: Klare Sicht und Original liegen zwei Schluessel auseinander;
                 * bei Gleichstand nennt die Basis das kleinere Preset (p bleibt
                 * eine der naechsten). */
                if (lcfg_gfx_preset(&b, 0) != LGP_CUSTOM ||
                    (vis && lcfg_gfx_diff(&b, lcfg_gfx_base(&b, 0), 0, NULL) != lcfg_gfx_diff(&b, p, 0, NULL))) {
                    _snprintf_s(why, sizeof why, _TRUNCATE, "P%d: %s geaendert -> %d/Basis %d", p,
                                lcfg_gfx_key(k), lcfg_gfx_preset(&b, 0), lcfg_gfx_base(&b, 0));
                }
            }
            b = a; /* hud_label_avoid: eigene Option (PO 7.11.) */
            b.shx[LSHX_LABEL] = 1;
            if (!why[0] && (lcfg_gfx_preset(&b, 0) != exp || lcfg_gfx_diff(&b, p, 0, NULL) != 0)) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "P%d: Etikett macht Custom", p);
            }
            lcfg_gfx_apply(&b, (p + 1) % SB_GP_NPRE);
            if (!why[0] && b.shx[LSHX_LABEL] != 1) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "P%d: Preset setzt Etikett", p);
            }
        }
        lcfg_gfx_set_loc(0);
        if (!why[0]) { /* Effect-Pack-Ausnahme (W465: mit Performance, Remastered ist nicht waehlbar) */
            lgp_mask_t lock = 0;
            lcfg_defaults(&a);
            lcfg_gfx_apply(&a, LGP_PERFORMANCE);
            a.shx[LSHX_SAT] = 140;
            a.fx[LFX_EMBERS] = 200;
            p = lcfg_gfx_preset(&a, 0);
            lcfg_gfx_lock_key(&lock, "color_sat");
            k = lcfg_gfx_preset(&a, lock);
            lcfg_gfx_lock_key(&lock, "fx_embers");
            lcfg_gfx_lock_key(&lock, "unknown_key");
            if (p != LGP_CUSTOM || k != LGP_CUSTOM || lcfg_gfx_preset(&a, lock) != LGP_PERFORMANCE ||
                lcfg_fx_level(&a, lock) != LGP_ORIGINAL || lcfg_fx_level(&a, 0) != LGP_CUSTOM) {
                strcpy_s(why, sizeof why, "Effect-Pack-Ausnahme");
            }
        }
        if (!why[0]) { /* Effekt-Stufe allein, Migration, Ableitungen */
            lcfg_defaults(&a);
            lcfg_fx_apply_level(&a, LGP_STRONG);
            if (lcfg_gfx_preset(&a, 0) != LGP_CUSTOM || lcfg_fx_level(&a, 0) != LGP_STRONG ||
                lcfg_gfx_base(&a, 0) != LGP_ORIGINAL) {
                strcpy_s(why, sizeof why, "Effekt-Stufe");
            }
            lcfg_defaults(&a); /* alte Datei: Remastered-Modus ohne fx-Werte */
            a.remaster = 1;
            a.hd_textures = 1;
            a.fx_enhanced = 1;
            lcfg_gfx_migrate(&a);
            if (!why[0] && (a.fx[LFX_SOFT] != 60 || a.fx[LFX_INTERP] != 1)) {
                strcpy_s(why, sizeof why, "Migration fx_enhanced");
            }
            lcfg_defaults(&a); /* alte Datei: Original-Modus mit Haken-Resten */
            a.remaster = 0;
            a.hd_textures = 1;
            a.ultrawide = 1;
            if (!why[0] && lcfg_gfx_preset(&a, 0) != LGP_ORIGINAL) {
                strcpy_s(why, sizeof why, "remaster=0 + hd_textures=1 ist Original");
            }
            lcfg_gfx_normalize(&a);
            if (!why[0] && (a.hd_textures || a.ultrawide || a.fx_enhanced)) {
                strcpy_s(why, sizeof why, "Ableitungen");
            }
            a.fx[LFX_SOFT] = 60; /* vom ESC-Menue gespeichert: frueher "Original" */
            if (!why[0] && lcfg_gfx_preset(&a, 0) != LGP_CUSTOM) {
                strcpy_s(why, sizeof why, "fx im Original-Modus nicht erkannt");
            }
        }
        if (!why[0]) {
            T_OK("52 gfx-preset-p4 (6 Presets + Custom, sichtbar je Spiel, Client-Tabelle, Effect Pack, Etikett)");
        } else {
            T_FAIL("52 gfx-preset-p4", "%s", why);
        }
    }

    /* W207: Performance values, persistence, old cfg and exclusive rules. */
    {
        int ok = 1;
        lr_env_t env;
        lr_res_t rr[LR_N];
        u8_GetTempPath(tmp, sizeof tmp);
        strcat_s(tmp, sizeof tmp, "w207_performance.cfg");
        u8_DeleteFile(tmp);
        lcfg_defaults(&a);
        lcfg_gfx_apply(&a, LGP_PERFORMANCE);
        if (a.interp_fps != 60 || a.rt_scale != 1 || a.rt_scale_dyn || a.fs_interp_match ||
            a.shx[LSHX_SHADOW] || a.shx[LSHX_SOFT] || a.shx[LSHX_SSAO] ||
            a.shx[LSHX_SHADING] != 1 || a.aspect_mode != 1) { ok = 0; }
        if (lcfg_save_user(tmp, &a) != 0) { ok = 0; }
        lcfg_defaults(&b);
        lcfg_load_file(tmp, &b);
        if (memcmp(&a, &b, sizeof a) || lcfg_gfx_preset(&b, 0) != LGP_PERFORMANCE) { ok = 0; }
        b.window_mode = 2;
        lrules_env_default(&env, &b);
        lrules_eval(&env, rr);
        if (rr[9].active || rr[14].active || !rr[13].active) { ok = 0; }
        b.interp_fps = 120;
        lrules_eval(&env, rr);
        if (!rr[14].active || lcfg_gfx_base(&b, 0) != LGP_PERFORMANCE) { ok = 0; }
        lcfg_gfx_apply(&b, LGP_ORIGINAL);
        if (b.interp_fps != SB_FPS_AUTO || b.rt_scale != 0 || !b.fs_interp_match) { ok = 0; } /* W296: Original = auto */
        /* Legacy values remain valid and unchanged when loaded. */
        lcfg_defaults(&b);
        lcfg_apply_set(&b, "interp_fps=60;rt_scale=3;rt_scale_dyn=1");
        if (b.interp_fps != 60 || b.rt_scale != 3 || b.rt_scale_dyn != 1 ||
            !b.fs_interp_match) { ok = 0; }
        u8_DeleteFile(tmp);
        if (ok) { T_OK("W207 lowend (60 FPS, native RT, save/load, Custom, rules)"); }
        else { T_FAIL("W207 lowend", "%s", "values/persistence/rules"); }
    }

    /* W252 CLEAR-VIEW: Klare Sicht = Original ohne Weltbeleuchtung und ohne
     * Monitor-Effekte; Speichern/Laden, Standard unveraendert, alte cfg ohne
     * monitor_fx = Original, Einzelaenderungen -> Benutzerdefiniert. */
    {
        char why[96] = "";
        int k;
        u8_GetTempPath(tmp, sizeof tmp);
        strcat_s(tmp, sizeof tmp, "w252_clear.cfg");
        u8_DeleteFile(tmp);
        lcfg_defaults(&a);
        if (a.monitor_fx != 1 || lcfg_gfx_preset(&a, 0) != LGP_ORIGINAL) { strcpy_s(why, sizeof why, "Standard"); }
        b = a;
        lcfg_gfx_apply(&a, LGP_CLEAR);
        for (k = 0; k < LGK_N && !why[0]; k++) { /* nur shading und monitor_fx anders als Original */
            if (!lcfg_gfx_owned(k) || k == LGK_FPS) { continue; } /* W296: Original auto, Klare Sicht weiter 120 */
            if ((lcfg_gfx_value(&a, k) != lcfg_gfx_value(&b, k)) != (k == LSHX_SHADING || k == LGK_MFX)) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "Wert %s", lcfg_gfx_key(k));
            }
        }
        if (!why[0] && (a.shx[LSHX_SHADING] != 0 || a.monitor_fx != 0 || a.interp_fps != 120 ||
                        a.rt_scale != 0 || !a.fs_interp_match || a.edge_mode != b.edge_mode ||
                        a.monitor_grain != 100 || a.monitor_glitch != 100 ||
                        lcfg_gfx_preset(&a, 0) != LGP_CLEAR)) { strcpy_s(why, sizeof why, "Sollwerte"); }
        if (!why[0] && lcfg_save_user(tmp, &a) != 0) { strcpy_s(why, sizeof why, "speichern"); }
        if (!why[0]) {
            FILE *g = NULL;
            char buf2[16384] = "";
            if (u8_fopen_s(&g, tmp, "r") == 0 && g) {
                (void)fread(buf2, 1, sizeof buf2 - 1, g);
                fclose(g);
            }
            lcfg_defaults(&b);
            lcfg_load_file(tmp, &b);
            if (memcmp(&a, &b, sizeof a) || lcfg_gfx_preset(&b, 0) != LGP_CLEAR ||
                !strstr(buf2, "monitor_fx=0") || !strstr(buf2, "shading=0")) {
                strcpy_s(why, sizeof why, "Rundweg");
            }
        }
        lcfg_defaults(&b); /* alte cfg ohne monitor_fx: Original; 2 (DIAG) = Custom */
        lcfg_apply_set(&b, "shading=1");
        if (!why[0] && (b.monitor_fx != 1 || lcfg_gfx_preset(&b, 0) != LGP_ORIGINAL)) { strcpy_s(why, sizeof why, "alt"); }
        lcfg_apply_set(&b, "monitor_fx=2");
        if (!why[0] && (b.monitor_fx != 2 || lcfg_gfx_preset(&b, 0) != LGP_CUSTOM)) { strcpy_s(why, sizeof why, "diag 2"); }
        lcfg_apply_set(&b, "monitor_fx=0"); /* nur Monitor aus: Benutzerdefiniert */
        if (!why[0] && (lcfg_gfx_preset(&b, 0) != LGP_CUSTOM || lcfg_gfx_base(&b, 0) != LGP_ORIGINAL)) {
            strcpy_s(why, sizeof why, "nur monitor_fx");
        }
        lcfg_apply_set(&b, "shading=0;interp_fps=120"); /* beide aus = Klare Sicht (W296: mit ihrer Rate 120) */
        if (!why[0] && lcfg_gfx_preset(&b, 0) != LGP_CLEAR) { strcpy_s(why, sizeof why, "beide aus"); }
        lcfg_gfx_apply(&b, LGP_ORIGINAL);
        if (!why[0] && (b.monitor_fx != 1 || b.shx[LSHX_SHADING] != 1)) { strcpy_s(why, sizeof why, "zurueck"); }
        u8_DeleteFile(tmp);
        if (!why[0]) { T_OK("W252 clear-view (shading 0 + monitor_fx 0, save/load, alt, Custom)"); }
        else { T_FAIL("W252 clear-view", "%s", why); }
    }

    /* W266 PRESET-EDGE: Breitbild-Raender (Ausblenden, 8 %, 100 %) gehoeren zu
     * allen Grafik-Presets; Standard = Original; alte cfg mit anderen Rand-
     * werten = Benutzerdefiniert (Basis Original, nur Rand-Schluessel weichen
     * ab); Preset-Wahl setzt die Raender; Rundweg user.cfg; Namen ohne "(HD)". */
    {
        char why[96] = "", keep[8];
        int p, li;
        lgp_mask_t dm = 0;
        const lgp_mask_t em = ((lgp_mask_t)1 << LGK_EDGE) | ((lgp_mask_t)1 << LGK_EDGEW) |
                              ((lgp_mask_t)1 << LGK_EDGES);
        u8_GetTempPath(tmp, sizeof tmp);
        strcat_s(tmp, sizeof tmp, "w266_edge.cfg");
        u8_DeleteFile(tmp);
        lcfg_defaults(&a);
        if (a.edge_mode != 1 || a.edge_fade_width != 8 || a.edge_fade_strength != 100 || /* W278: 4:3 */
            lcfg_gfx_preset(&a, 0) != LGP_ORIGINAL) { strcpy_s(why, sizeof why, "Standard"); }
        for (p = 0; p < SB_GP_NPRE && !why[0]; p++) {
            b = a;
            b.edge_mode = 0;
            b.edge_fade_width = 12;
            b.edge_fade_strength = 60;
            lcfg_gfx_apply(&b, p);
            if (b.edge_mode != 1 || b.edge_fade_width != 8 || b.edge_fade_strength != 100 || /* W278: 4:3 */
                lcfg_gfx_preset_value(p, LGK_EDGE) != 1 || lcfg_gfx_preset_value(p, LGK_EDGEW) != 8 ||
                lcfg_gfx_preset_value(p, LGK_EDGES) != 100 || lcfg_gfx_diff(&b, p, 0, NULL) != 0) { /* W465: auch unsichtbare */
                _snprintf_s(why, sizeof why, _TRUNCATE, "Preset %d setzt Raender nicht", p);
            }
        }
        lcfg_defaults(&b); /* alte cfg (vor W266): Rand aus bzw. 10/12 % -> Benutzerdefiniert, Basis Original */
        lcfg_apply_set(&b, "edge_mode=0;edge_fade_width=12;edge_fade_strength=100");
        if (!why[0] && (lcfg_gfx_preset(&b, 0) != LGP_CUSTOM || lcfg_gfx_base(&b, 0) != LGP_ORIGINAL ||
                        lcfg_gfx_diff(&b, LGP_ORIGINAL, 0, &dm) != 2 || (dm & ~em) != 0)) {
            strcpy_s(why, sizeof why, "alte cfg aus/12");
        }
        lcfg_defaults(&b);
        lcfg_apply_set(&b, "edge_mode=2;edge_fade_width=10");
        if (!why[0] && (lcfg_gfx_preset(&b, 0) != LGP_CUSTOM || lcfg_gfx_base(&b, 0) != LGP_ORIGINAL)) {
            strcpy_s(why, sizeof why, "alte cfg 10 %");
        }
        lcfg_gfx_apply(&b, LGP_PERFORMANCE); /* Wahl setzt die Raender */
        if (!why[0] && (b.edge_fade_width != 8 || lcfg_gfx_preset(&b, 0) != LGP_PERFORMANCE)) {
            strcpy_s(why, sizeof why, "Wahl Performance");
        }
        if (!why[0] && lcfg_save_user(tmp, &b) != 0) { strcpy_s(why, sizeof why, "speichern"); }
        if (!why[0]) {
            FILE *g = NULL;
            char buf2[16384] = "";
            if (u8_fopen_s(&g, tmp, "r") == 0 && g) {
                (void)fread(buf2, 1, sizeof buf2 - 1, g);
                fclose(g);
            }
            lcfg_defaults(&a);
            lcfg_load_file(tmp, &a);
            if (memcmp(&a, &b, sizeof a) || lcfg_gfx_preset(&a, 0) != LGP_PERFORMANCE ||
                !strstr(buf2, "edge_mode=1") || !strstr(buf2, "edge_fade_width=8") || /* W278 */
                !strstr(buf2, "edge_fade_strength=100")) {
                strcpy_s(why, sizeof why, "Rundweg");
            }
        }
        strcpy_s(keep, sizeof keep, sb_lang_code()); /* Namen EN/DE ohne "(HD)" (W296: "Original" / "Performance") */
        for (li = 0; li < 2 && !why[0]; li++) {
            static const char *const nk[4] = { "gui.gfx_p_orig", "gui.gfx_p_performance", "ovl.v_orig_hd",
                                               "ovl.v_performance" };
            int i;
            sb_lang_set(li ? "de" : "en");
            for (i = 0; i < 4 && !why[0]; i++) {
                const char *t = LT(nk[i]);
                if (strstr(t, "(HD)") || strcmp(t, (i & 1) ? "Performance" : "Original") != 0) {
                    _snprintf_s(why, sizeof why, _TRUNCATE, "Name %s %s", li ? "de" : "en", nk[i]);
                }
            }
        }
        sb_lang_set(keep);
        u8_DeleteFile(tmp);
        if (!why[0]) { T_OK("W266 preset-edge (Raender 1/8/100 = 4:3 seit W278 in allen Presets, alte cfg Custom, Rundweg, Namen)"); }
        else { T_FAIL("W266 preset-edge", "%s", why); }
    }

    /* W296 PRESET-ORIG: ein "Original" (Name ohne Bildrate) = Monitor (auto);
     * alte Dateien (ohne gfx_preset_rev) mit dem bisherigen Original 120 +
     * fs_interp_match=1 werden beim Laden Original (auto), jede andere Rate und
     * jede neue Datei bleiben; "Performance" heisst so und bleibt 60. */
    {
        char why[120] = "", keep[8], buf2[16384] = "";
        int p, li;
        launcher_settings_t c;
        FILE *g = NULL;
        u8_GetTempPath(tmp, sizeof tmp);
        strcat_s(tmp, sizeof tmp, "w296_orig.cfg");
        lcfg_defaults(&a);
        if (a.interp_fps != SB_FPS_AUTO || a.gfx_preset_rev != SB_GP_REV || lcfg_gfx_preset(&a, 0) != LGP_ORIGINAL ||
            lcfg_gfx_preset_value(LGP_ORIGINAL, LGK_FPS) != SB_FPS_AUTO) {
            strcpy_s(why, sizeof why, "Standard/Original != auto");
        }
        for (p = 1; p < SB_GP_NPRE && !why[0]; p++) { /* uebrige Presets wie bisher */
            /* W465: Enhanced (LoC) nicht hier - seine Werte koennen sich beim Feinschliff noch aendern */
            if (p != LGP_ENHANCED && p != LGP_VIVID && /* W478: Vivid (LoC) wie Enhanced */
                lcfg_gfx_preset_value(p, LGK_FPS) != (p == LGP_PERFORMANCE ? 60 : 120)) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "Preset %d Bildrate %d", p, lcfg_gfx_preset_value(p, LGK_FPS));
            }
        }
        b = a;
        b.interp_fps = 60;
        lcfg_gfx_apply(&b, LGP_ORIGINAL);
        if (!why[0] && (b.interp_fps != SB_FPS_AUTO || b.fs_interp_match != 1 || lcfg_gfx_preset(&b, 0) != LGP_ORIGINAL)) {
            strcpy_s(why, sizeof why, "Wahl Original setzt auto nicht");
        }
        lcfg_gfx_apply(&b, LGP_PERFORMANCE);
        if (!why[0] && (b.interp_fps != 60 || lcfg_gfx_preset(&b, 0) != LGP_PERFORMANCE)) {
            strcpy_s(why, sizeof why, "Performance 60");
        }
        /* Migration: alte Datei = gespeichert und Zeile gfx_preset_rev entfernt */
        for (p = 0; p < 6 && !why[0]; p++) {
            /* 0: altes Original 120+Match -> auto; 1: alte 60 bleibt; 2: alte 120 ohne Match bleibt;
             * 3: alte 120 mit Breitbild bleibt; 4: neue Datei mit 120 bleibt (bewusst);
             * 5: alte 144 bleibt */
            static const int fps_in[6] = { 120, 60, 120, 120, 120, 144 };
            const int want = p == 0 ? SB_FPS_AUTO : fps_in[p];
            FILE *w = NULL;
            char line[512], tmp2[1100];
            int drop = p != 4, mig;
            lcfg_defaults(&a);
            a.interp_fps = fps_in[p];
            if (p == 2) { a.fs_interp_match = 0; }
            if (p == 3) { a.edge_mode = 2; }
            u8_DeleteFile(tmp);
            if (lcfg_save_user(tmp, &a) != 0) { strcpy_s(why, sizeof why, "speichern"); break; }
            _snprintf_s(tmp2, sizeof tmp2, _TRUNCATE, "%s.old", tmp);
            u8_DeleteFile(tmp2);
            if (u8_fopen_s(&g, tmp, "r") == 0 && g && u8_fopen_s(&w, tmp2, "w") == 0 && w) {
                while (fgets(line, sizeof line, g)) {
                    if (drop && strncmp(line, SB_GP_REV_KEY "=", strlen(SB_GP_REV_KEY) + 1) == 0) { continue; }
                    fputs(line, w);
                }
            }
            if (g) { fclose(g); g = NULL; }
            if (w) { fclose(w); }
            lcfg_defaults(&b);
            b.gfx_preset_rev = 0; /* wie app_load_effective / lcfg_profile_load */
            lcfg_load_file(tmp2, &b);
            c = b;
            mig = lcfg_gfx_migrate_fps(&b);
            if (b.interp_fps != want || b.gfx_preset_rev != SB_GP_REV || mig != (p == 0) ||
                c.gfx_preset_rev != (drop ? 0 : SB_GP_REV)) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "Migration Fall %d: %d -> %d (rev %d, mig %d)", p, fps_in[p],
                            b.interp_fps, c.gfx_preset_rev, mig);
            }
            if (!why[0] && p == 0 && lcfg_gfx_preset(&b, 0) != LGP_ORIGINAL) { strcpy_s(why, sizeof why, "altes Original != Original"); }
            if (!why[0] && p == 4 && (lcfg_gfx_preset(&b, 0) != LGP_CUSTOM || lcfg_gfx_base(&b, 0) != LGP_ORIGINAL)) {
                strcpy_s(why, sizeof why, "bewusste 120: Custom/Basis Original");
            }
            if (!why[0] && p == 1 && lcfg_gfx_preset(&b, 0) != LGP_CUSTOM) { strcpy_s(why, sizeof why, "alte 60 Custom"); }
            u8_DeleteFile(tmp2);
        }
        if (!why[0]) { /* gespeicherte Datei traegt den Stand, Rundweg auto */
            lcfg_defaults(&a);
            u8_DeleteFile(tmp);
            lcfg_save_user(tmp, &a);
            if (u8_fopen_s(&g, tmp, "r") == 0 && g) {
                (void)fread(buf2, 1, sizeof buf2 - 1, g);
                fclose(g);
            }
            lcfg_defaults(&b);
            b.gfx_preset_rev = 0;
            lcfg_load_file(tmp, &b);
            if (!strstr(buf2, "interp_fps=-1") || !strstr(buf2, SB_GP_REV_KEY "=1") || memcmp(&a, &b, sizeof a)) {
                strcpy_s(why, sizeof why, "Rundweg auto/gfx_preset_rev");
            }
        }
        strcpy_s(keep, sizeof keep, sb_lang_code()); /* Namen EN/DE: ohne Bildrate */
        for (li = 0; li < 2 && !why[0]; li++) {
            static const char *const nk[3] = { "gui.gfx_p_orig", "ovl.v_orig_hd", "gui.gfx_p_orig_txt" };
            int i;
            sb_lang_set(li ? "de" : "en");
            for (i = 0; i < 3 && !why[0]; i++) {
                const char *t = LT(nk[i]);
                if (strstr(t, "%") || strstr(t, "FPS") || strstr(t, "120") || (i < 2 && strcmp(t, "Original") != 0)) {
                    _snprintf_s(why, sizeof why, _TRUNCATE, "Name %s %s", li ? "de" : "en", nk[i]);
                }
            }
        }
        sb_lang_set(keep);
        u8_DeleteFile(tmp);
        if (!why[0]) { T_OK("W296 preset-orig (Original = Monitor auto, Name ohne FPS EN/DE, altes Original 120 -> auto, bewusste 60/120/144 bleiben, Performance 60, gfx_preset_rev)"); }
        else { T_FAIL("W296 preset-orig", "%s", why); }
    }

    /* W259 SETTINGS-RESET: 3-Wege-Abgleich (reine Logik). ui == base ->
     * Dateiwert (ESC "Als Standard speichern"), ui != base -> Launcher
     * gewinnt, Pfade bleiben beim Launcher. */
    {
        launcher_settings_t ui, base, disk;
        char why[96] = "";
        int n;
        lcfg_defaults(&base);
        strcpy_s(base.media_root, sizeof base.media_root, "C:/spiel");
        ui = base;
        disk = base;
        disk.shx[LSHX_LWORLD] = 150;   /* im Spiel gespeichert */
        disk.shx[LSHX_SAT] = 130;      /* im Spiel gespeichert, im Launcher auch geaendert */
        disk.cockpit_width = 4;
        strcpy_s(disk.media_root, sizeof disk.media_root, "C:/anders");
        ui.shx[LSHX_SAT] = 90;         /* Launcher-Aenderung */
        ui.width = 1600;               /* Launcher-Aenderung, Datei unveraendert */
        n = lcfg_merge_external(&ui, &base, &disk);
        if (n != 2) { _snprintf_s(why, sizeof why, _TRUNCATE, "n=%d statt 2", n); }
        if (!why[0] && (ui.shx[LSHX_LWORLD] != 150 || ui.cockpit_width != 4)) { strcpy_s(why, sizeof why, "Spielwert verloren"); }
        if (!why[0] && (ui.shx[LSHX_SAT] != 90 || ui.width != 1600)) { strcpy_s(why, sizeof why, "Launcher-Aenderung verloren"); }
        if (!why[0] && strcmp(ui.media_root, "C:/spiel") != 0) { strcpy_s(why, sizeof why, "Pfad uebernommen"); }
        if (!why[0] && lcfg_merge_external(&ui, &disk, &disk) != 0) { strcpy_s(why, sizeof why, "zweiter Abgleich nicht leer"); }
        if (!why[0]) { T_OK("W259 settings-merge (ESC-Wert bleibt, Launcher-Aenderung gewinnt, Pfade beim Launcher)"); }
        else { T_FAIL("W259 settings-merge", "%s", why); }
    }

    /* W223 FPS-RATES: Liste, auto-Aufloesung, Pruefung, Speichern, Preset, R10. */
    {
        static const int want[SB_FPS_NCHOICE] = { -1, 0, 60, 75, 90, 100, 120, 144, 165, 240 };
        static const int hz_in[] = { 0, 1, 50, 59, 60, 74, 75, 119, 143, 144, 164, 239, 240, 360 };
        static const int hz_out[] = { 60, 60, 60, 60, 60, 75, 75, 120, 144, 144, 165, 240, 240, 240 };
        static const int bad[] = { -2, 1, 30, 50, 59, 61, 120 + 1, 250, 1000 };
        int ok = 1, i;
        char e[256], why[96] = "";
        lr_env_t env;
        lr_res_t rr[LR_N];
        for (i = 0; i < SB_FPS_NCHOICE; i++) {
            if (kSbFpsChoice[i] != want[i] || sb_fps_choice_idx(want[i]) != i) { ok = 0; strcpy_s(why, sizeof why, "list"); }
            /* W227: gueltige Basis beider Editionen (community: Test-Spielordner
             * wie Fall 5; der Standard dort hat bewusst keine Spieldateien) */
            st_valid_base(&a);
            a.interp_fps = want[i];
            if (lcfg_validate(&a, e, sizeof e) != 0) { ok = 0; _snprintf_s(why, sizeof why, _TRUNCATE, "valid %d: %.60s", want[i], e); }
        }
        for (i = 0; i < (int)(sizeof bad / sizeof bad[0]); i++) {
            st_valid_base(&a); /* W227: sonst lehnt community schon die Spieldaten ab */
            a.interp_fps = bad[i];
            if (lcfg_validate(&a, e, sizeof e) == 0) { ok = 0; strcpy_s(why, sizeof why, "invalid accepted"); }
        }
        for (i = 0; i < (int)(sizeof hz_in / sizeof hz_in[0]); i++) {
            if (sb_fps_auto_from_hz(hz_in[i]) != hz_out[i]) { ok = 0; strcpy_s(why, sizeof why, "auto hz"); }
        }
        /* Speichern/Laden: 120 und 144 (W296: feste Raten = Benutzerdefiniert); Standard auto/Original (W296). */
        u8_GetTempPath(tmp, sizeof tmp);
        strcat_s(tmp, sizeof tmp, "w223_fps.cfg");
        for (i = 0; i < 2; i++) {
            u8_DeleteFile(tmp);
            lcfg_defaults(&a);
            a.interp_fps = i ? 144 : 120; /* W296: auto ist jetzt Original */
            if (lcfg_save_user(tmp, &a) != 0) { ok = 0; }
            lcfg_defaults(&b);
            lcfg_load_file(tmp, &b);
            if (memcmp(&a, &b, sizeof a) || b.interp_fps != a.interp_fps) { ok = 0; strcpy_s(why, sizeof why, "roundtrip"); }
            if (lcfg_gfx_preset(&b, 0) != LGP_CUSTOM || lcfg_gfx_base(&b, 0) != LGP_ORIGINAL) {
                ok = 0; strcpy_s(why, sizeof why, "custom");
            }
        }
        u8_DeleteFile(tmp);
        lcfg_defaults(&b);
        if (b.interp_fps != SB_FPS_AUTO || lcfg_gfx_preset(&b, 0) != LGP_ORIGINAL) { ok = 0; strcpy_s(why, sizeof why, "default"); } /* W296 */
        lcfg_gfx_apply(&b, LGP_PERFORMANCE);
        if (b.interp_fps != 60) { ok = 0; strcpy_s(why, sizeof why, "performance"); }
        lcfg_apply_set(&b, "interp_fps=-1");
        if (b.interp_fps != SB_FPS_AUTO) { ok = 0; strcpy_s(why, sizeof why, "set auto"); }
        /* R10: feste Rate ueber Monitor-Hz im Fenster warnt; auto/gleich/59 Hz nicht. */
        lcfg_defaults(&b);
        b.window_mode = 0;
        lrules_env_default(&env, &b);
        env.display_hz = 60;
        b.interp_fps = 75;
        lrules_eval(&env, rr);
        if (!rr[10].active) { ok = 0; strcpy_s(why, sizeof why, "r10 75@60"); }
        b.interp_fps = SB_FPS_AUTO;
        lrules_eval(&env, rr);
        if (rr[10].active) { ok = 0; strcpy_s(why, sizeof why, "r10 auto"); }
        env.display_hz = 59;
        b.interp_fps = 60;
        lrules_eval(&env, rr);
        if (rr[10].active) { ok = 0; strcpy_s(why, sizeof why, "r10 60@59"); }
        env.display_hz = 144;
        b.interp_fps = 144;
        lrules_eval(&env, rr);
        if (rr[10].active) { ok = 0; strcpy_s(why, sizeof why, "r10 144@144"); }
        if (ok) { T_OK("W223 fps-rates (auto + 30..240, validate, save/load, Custom, R10)"); }
        else { T_FAIL("W223 fps-rates", "%s", why); }
    }
    { /* W468 PERF-PACING: Bildtakt frame_pacing (Standard Monitor, Roundtrip, Pruefung, Namen, R10 mit VRR) */
        int ok = 1, i;
        char e[256], why[96] = "";
        lr_env_t env;
        lr_res_t rr[LR_N];
        lcfg_defaults(&b);
        if (b.frame_pacing != 1) { ok = 0; strcpy_s(why, sizeof why, "default"); }
        u8_GetTempPath(tmp, sizeof tmp);
        strcat_s(tmp, sizeof tmp, "w468_pacing.cfg");
        for (i = 0; i < 3; i++) {
            u8_DeleteFile(tmp);
            lcfg_defaults(&a);
            a.frame_pacing = i;
            if (lcfg_save_user(tmp, &a) != 0) { ok = 0; }
            lcfg_defaults(&b);
            lcfg_load_file(tmp, &b);
            if (memcmp(&a, &b, sizeof a) || b.frame_pacing != i) { ok = 0; strcpy_s(why, sizeof why, "roundtrip"); }
            st_valid_base(&a);
            a.frame_pacing = i;
            if (lcfg_validate(&a, e, sizeof e) != 0) { ok = 0; _snprintf_s(why, sizeof why, _TRUNCATE, "valid %d: %.60s", i, e); }
        }
        u8_DeleteFile(tmp);
        st_valid_base(&a);
        a.frame_pacing = 3;
        if (lcfg_validate(&a, e, sizeof e) == 0) { ok = 0; strcpy_s(why, sizeof why, "3 accepted"); }
        lcfg_defaults(&b);
        lcfg_apply_set(&b, "frame_pacing=vrr");
        if (b.frame_pacing != 2) { ok = 0; strcpy_s(why, sizeof why, "set vrr"); }
        lcfg_apply_set(&b, "frame_pacing=timer");
        if (b.frame_pacing != 0) { ok = 0; strcpy_s(why, sizeof why, "set timer"); }
        /* R10: 120 auf 165 Hz warnt mit Monitor-Takt, mit VRR nicht; 240 auf 165 Hz warnt auch mit VRR */
        lcfg_defaults(&b);
        b.window_mode = 0;
        lrules_env_default(&env, &b);
        env.display_hz = 165;
        b.interp_fps = 120;
        lrules_eval(&env, rr);
        if (!rr[10].active) { ok = 0; strcpy_s(why, sizeof why, "r10 120@165"); }
        b.frame_pacing = 2;
        lrules_eval(&env, rr);
        if (rr[10].active) { ok = 0; strcpy_s(why, sizeof why, "r10 vrr 120@165"); }
        b.interp_fps = 240;
        lrules_eval(&env, rr);
        if (!rr[10].active) { ok = 0; strcpy_s(why, sizeof why, "r10 vrr 240@165"); }
        if (ok) { T_OK("W468 frame pacing (default monitor, save/load, validate, names, R10 with VRR)"); }
        else { T_FAIL("W468 frame pacing", "%s", why); }
    }

    st_w309_fpsmon(&pass, &fail); /* W309 FPS-MONITOR */
    st_sbc_trunc(&pass, &fail); /* Tastenbelegung ohne SBC */
    st_w312_joycalib(&pass, &fail); /* W312 JOY-CALIB */
    st_w432_hotas(&pass, &fail);    /* W432 HOTAS-AXES */

    /* 29 (W143): Assistent-cfg Speichern/Zuruecksetzen erhaelt fremde Zeilen */
    {
        static const char *own[] = { "sbc_cal_rot", "sbc_dead_rot", "pad_dz_stick" };
        char lines[4][96], back[8][96];
        int ok = 1, n;
        size_t i;
        u8_GetTempPath(tmp, sizeof tmp);
        strcat_s(tmp, sizeof tmp, "w143_assist_cfg_test.cfg");
        u8_DeleteFile(tmp);
        if (u8_fopen_s(&f, tmp, "w") != 0 || !f) {
            ok = 0;
        } else {
            fputs("# Kopf\r\n[video]\r\nfs_hz=120\r\n\r\n[input]\r\nsbc_usb=1\r\n"
                  "sbc_cal_rot=-100,0,100\r\nsbc_dead_rot=5\r\npad_dz_stick=10\r\n"
                  "fremd=1\r\n\r\n[audio]\r\nvol=80\r\n", f);
            fclose(f);
        }
        strcpy_s(lines[0], sizeof lines[0], "sbc_cal_rot=0,0,240");
        strcpy_s(lines[1], sizeof lines[1], "sbc_dead_rot=7");
        strcpy_s(lines[2], sizeof lines[2], "pad_dz_stick=15");
        if (assist_cfg_apply(tmp, lines, 3) != 0) {
            ok = 0;
        }
        memset(buf, 0, sizeof buf);
        if (u8_fopen_s(&f, tmp, "r") == 0 && f) {
            (void)fread(buf, 1, sizeof buf - 1, f);
            fclose(f);
        }
        /* Eigene Zeilen ersetzt, fremde Zeilen + Abschnitte intakt. */
        if (!strstr(buf, "sbc_cal_rot=0,0,240") || !strstr(buf, "sbc_dead_rot=7") ||
            !strstr(buf, "pad_dz_stick=15") || !strstr(buf, "fremd=1") ||
            !strstr(buf, "sbc_usb=1") || !strstr(buf, "[audio]") ||
            !strstr(buf, "vol=80") || !strstr(buf, "# Kopf")) {
            ok = 0;
        }
        if (assist_cfg_reset(tmp) != 0) {
            ok = 0;
        }
        memset(buf, 0, sizeof buf);
        if (u8_fopen_s(&f, tmp, "r") == 0 && f) {
            (void)fread(buf, 1, sizeof buf - 1, f);
            fclose(f);
        }
        /* Eigene Zeilen weg, fremde Zeilen + Abschnitte intakt. */
        for (i = 0; i < sizeof own / sizeof own[0]; i++) {
            if (strstr(buf, own[i])) {
                ok = 0;
            }
        }
        if (!strstr(buf, "fremd=1") || !strstr(buf, "sbc_usb=1") ||
            !strstr(buf, "[audio]") || !strstr(buf, "vol=80")) {
            ok = 0;
        }
        /* assist_cfg_read liest das Geschriebene zurueck. */
        if (assist_cfg_apply(tmp, lines, 3) != 0) {
            ok = 0;
        }
        n = assist_cfg_read(tmp, back, 8);
        if (n != 3 || strcmp(back[0], "sbc_cal_rot=0,0,240") != 0 ||
            strcmp(back[1], "sbc_dead_rot=7") != 0 ||
            strcmp(back[2], "pad_dz_stick=15") != 0) {
            ok = 0;
        }
        u8_DeleteFile(tmp);
        if (ok) {
            T_OK("29 assist-cfg");
        } else {
            T_FAIL("29 assist-cfg", "%s", "Speichern/Ruecksetzen falsch");
        }
    }

    /* 40 (W159): Start-Parameter je Weg (Bildschirm/VR), user.cfg bleibt. */
    {
        char set_args[128], cmd[3600], before[4096], after[4096];
        size_t nb = 0, na = 0;
        int ok = 1;
        FILE *g = NULL;
        lcfg_defaults(&b);
        if (strcmp(b.vr_preview, "left") != 0) { /* Standard ein Auge */
            ok = 0;
        }
        app_vr_set_args(0, b.vr_preview, set_args, sizeof set_args);
        if (strcmp(set_args, "vr_enable=0") != 0) {
            ok = 0;
        }
        app_vr_set_args(1, "sbs", set_args, sizeof set_args);
        if (strcmp(set_args, "vr_enable=1;vr_preview=sbs") != 0) {
            ok = 0;
        }
        app_vr_set_args(1, "", set_args, sizeof set_args);
        if (strcmp(set_args, "vr_enable=1;vr_preview=left") != 0) {
            ok = 0;
        }
        /* user.cfg mit gespeichertem vr_enable=1: Start baut nur die
         * Kommandozeile, die Datei bleibt Byte-fuer-Byte gleich. */
        u8_GetTempPath(tmp, sizeof tmp);
        strcat_s(tmp, sizeof tmp, "w159_launch_test.cfg");
        u8_DeleteFile(tmp);
        b.vr_enable = 1;
        strcpy_s(b.vr_preview, sizeof b.vr_preview, "sbs");
        if (lcfg_save_user(tmp, &b) != 0) {
            ok = 0;
        } else {
            if (u8_fopen_s(&g, tmp, "r") == 0 && g) {
                nb = fread(before, 1, sizeof before - 1, g);
                fclose(g);
                g = NULL;
            }
            before[nb] = '\0';
            app_vr_set_args(0, b.vr_preview, set_args, sizeof set_args);
            app_client_cmdline("C:/client", "D:/default.xbe",
                               set_args, cmd, sizeof cmd);
            if (!strstr(cmd, "--set \"vr_enable=0\"") ||
                !strstr(cmd, "--xbe")) {
                ok = 0;
            }
            app_vr_set_args(1, b.vr_preview, set_args, sizeof set_args);
            app_client_cmdline("C:/client", "D:/default.xbe",
                               set_args, cmd, sizeof cmd);
            if (!strstr(cmd, "--set \"vr_enable=1;vr_preview=sbs\"")) {
                ok = 0;
            }
            if (u8_fopen_s(&g, tmp, "r") == 0 && g) {
                na = fread(after, 1, sizeof after - 1, g);
                fclose(g);
                g = NULL;
            }
            if (nb == 0 || na != nb || memcmp(before, after, nb) != 0 ||
                !strstr(before, "vr_enable=1")) {
                ok = 0;
            }
        }
        u8_DeleteFile(tmp);
        if (ok) {
            T_OK("40 vr-launch");
        } else {
            T_FAIL("40 vr-launch", "%s", "Start-Parameter/user.cfg falsch");
        }
    }

    /* 41 (W159): Bind-Aktion vr_recenter speichern + laden. */
    {
        static lctrl_bind_t b1, b2; /* W414: static, je ~31 KB (Stapelrahmen cli_selftest) */
        char row[256], conf[4][160], err2[256];
        int ok = 1, n, vr, bi0;
        vr = lctrl_bind_elem_index("vr_recenter");
        bi0 = lctrl_bind_elem_index("bi0");
        if (vr < 0 || vr >= LCTRL_ELEM_N || bi0 < 0 ||
            kCtrlElems[vr].kind != LCTRL_ACT ||
            strcmp(kCtrlElems[vr].key, "vr_recenter") != 0) {
            ok = 0;
        } else {
            lctrl_bind_init(&b1);
            if (lctrl_bind_set(&b1, vr, "KEY:F8, SBC:CHAFF") != 0) {
                ok = 0;
            }
            lctrl_bind_row_text(&b1, vr, row, sizeof row);
            if (!strstr(row, "KEY:F8") || !strstr(row, "SBC:CHAFF")) {
                ok = 0;
            }
            u8_GetTempPath(tmp, sizeof tmp);
            strcat_s(tmp, sizeof tmp, "w159_bind_test.ini");
            u8_DeleteFile(tmp);
            if (lctrl_bind_save(tmp, &b1, "keyboard_default", err2,
                                sizeof err2) != 0) {
                ok = 0;
            } else {
                memset(buf, 0, sizeof buf);
                if (u8_fopen_s(&f, tmp, "r") == 0 && f) {
                    (void)fread(buf, 1, sizeof buf - 1, f);
                    fclose(f);
                }
                if (!strstr(buf, "[vr]") ||
                    !strstr(buf, "vr_recenter = KEY:F8, SBC:CHAFF")) {
                    ok = 0;
                }
                lctrl_bind_init(&b2);
                if (lctrl_bind_load_file(tmp, &b2) != 0) {
                    ok = 0;
                } else {
                    char row2[256];
                    lctrl_bind_row_text(&b2, vr, row2, sizeof row2);
                    if (strcmp(row, row2) != 0) {
                        ok = 0;
                    }
                }
            }
            u8_DeleteFile(tmp);
            /* Konflikt-Suche sieht die neue Zeile (gleiche Quelle). */
            lctrl_bind_init(&b1);
            lctrl_bind_set(&b1, bi0, "KEY:F8");
            lctrl_bind_set(&b1, vr, "KEY:F8");
            n = lctrl_bind_conflicts(&b1, conf, 4);
            if (n < 1 || !strstr(conf[0], "KEY:F8") ||
                !strstr(conf[0], "vr_recenter")) {
                ok = 0;
            }
            /* SBC-Erkennung fuer die Doppelbelegungs-Warnung. */
            if (!lctrl_bind_alt_is_sbc("SBC:CHAFF") ||
                !lctrl_bind_alt_is_sbc("sbc:bi5+KEY:X") ||
                lctrl_bind_alt_is_sbc("KEY:F8") ||
                lctrl_bind_alt_is_sbc("PAD:RB") ||
                lctrl_bind_alt_is_sbc("SBCX:1") ||
                lctrl_bind_alt_is_sbc(NULL)) {
                ok = 0;
            }
        }
        if (ok) {
            T_OK("41 vr-bind");
        } else {
            T_FAIL("41 vr-bind", "%s", "Aktion speichern/laden falsch");
        }
    }

    /* 57 (W188 GAME-MENU): Client-Aktion pause - Element, Abschnitt [client],
     * Tastenname PAUSE, Konflikt mit einem Spiel-Element wird gemeldet. */
    {
        static lctrl_bind_t b1, b2; /* W414: static, je ~31 KB (Stapelrahmen cli_selftest) */
        char row[256], conf[4][160], err2[256], nm[32];
        int ok = 1, n, pa, bi0;
        const char *why = "";
        pa = lctrl_bind_elem_index("pause");
        bi0 = lctrl_bind_elem_index("bi0");
        lctrl_key_name(VK_PAUSE, nm, sizeof nm);
        if (pa < 0 || bi0 < 0 || kCtrlElems[pa].kind != LCTRL_ACT || strcmp(nm, "PAUSE") != 0) {
            ok = 0; why = "Element/Tastenname";
        } else {
            lctrl_bind_init(&b1);
            lctrl_bind_set(&b1, pa, "KEY:PAUSE, PAD:BACK+PAD:Y");
            lctrl_bind_row_text(&b1, pa, row, sizeof row);
            u8_GetTempPath(tmp, sizeof tmp);
            strcat_s(tmp, sizeof tmp, "w188_bind_test.ini");
            u8_DeleteFile(tmp);
            if (lctrl_bind_save(tmp, &b1, "keyboard_default", err2, sizeof err2) != 0) {
                ok = 0; why = "speichern";
            } else {
                memset(buf, 0, sizeof buf);
                if (u8_fopen_s(&f, tmp, "r") == 0 && f) {
                    (void)fread(buf, 1, sizeof buf - 1, f);
                    fclose(f);
                }
                if (!strstr(buf, "[client]") || !strstr(buf, "pause = KEY:PAUSE, PAD:BACK+PAD:Y")) {
                    ok = 0; why = "Abschnitt [client]";
                }
                lctrl_bind_init(&b2);
                if (lctrl_bind_load_file(tmp, &b2) != 0) {
                    ok = 0; why = "laden";
                } else {
                    char row2[256];
                    lctrl_bind_row_text(&b2, pa, row2, sizeof row2);
                    if (strcmp(row, row2) != 0) { ok = 0; why = "laden != speichern"; }
                }
            }
            u8_DeleteFile(tmp);
            lctrl_bind_init(&b1);
            lctrl_bind_set(&b1, bi0, "KEY:P");
            lctrl_bind_set(&b1, pa, "KEY:P");
            n = lctrl_bind_conflicts(&b1, conf, 4);
            if (n < 1 || !strstr(conf[0], "KEY:P") || !strstr(conf[0], "pause")) {
                ok = 0; why = "Konflikt nicht gemeldet";
            }
        }
        if (ok) {
            T_OK("57 pause-bind");
        } else {
            T_FAIL("57 pause-bind", "%s", why);
        }
    }

    /* 60 (W195 PHOTO-MODE): Client-Aktion photo - Element, Abschnitt [client],
     * Tastenname F12, Rundweg, Konflikt mit einem Spiel-Element wird gemeldet. */
    {
        static lctrl_bind_t b1, b2; /* W414: static, je ~31 KB (Stapelrahmen cli_selftest) */
        char row[256], conf[4][160], err2[256], nm[32];
        int ok = 1, n, ph, bi0;
        const char *why = "";
        ph = lctrl_bind_elem_index("photo");
        bi0 = lctrl_bind_elem_index("bi0");
        lctrl_key_name(VK_F12, nm, sizeof nm);
        if (ph < 0 || bi0 < 0 || kCtrlElems[ph].kind != LCTRL_ACT || strcmp(nm, "F12") != 0) {
            ok = 0; why = "Element/Tastenname";
        } else {
            lctrl_bind_init(&b1);
            lctrl_bind_set(&b1, ph, "KEY:F12, PAD:BACK+PAD:LB");
            lctrl_bind_row_text(&b1, ph, row, sizeof row);
            u8_GetTempPath(tmp, sizeof tmp);
            strcat_s(tmp, sizeof tmp, "w195_bind_test.ini");
            u8_DeleteFile(tmp);
            if (lctrl_bind_save(tmp, &b1, "keyboard_default", err2, sizeof err2) != 0) {
                ok = 0; why = "speichern";
            } else {
                memset(buf, 0, sizeof buf);
                if (u8_fopen_s(&f, tmp, "r") == 0 && f) {
                    (void)fread(buf, 1, sizeof buf - 1, f);
                    fclose(f);
                }
                if (!strstr(buf, "[client]") || !strstr(buf, "photo = KEY:F12, PAD:BACK+PAD:LB")) {
                    ok = 0; why = "Abschnitt [client]";
                }
                lctrl_bind_init(&b2);
                if (lctrl_bind_load_file(tmp, &b2) != 0) {
                    ok = 0; why = "laden";
                } else {
                    char row2[256];
                    lctrl_bind_row_text(&b2, ph, row2, sizeof row2);
                    if (strcmp(row, row2) != 0) { ok = 0; why = "laden != speichern"; }
                }
            }
            u8_DeleteFile(tmp);
            lctrl_bind_init(&b1);
            lctrl_bind_set(&b1, bi0, "KEY:F12");
            lctrl_bind_set(&b1, ph, "KEY:F12");
            n = lctrl_bind_conflicts(&b1, conf, 4);
            if (n < 1 || !strstr(conf[0], "KEY:F12") || !strstr(conf[0], "photo")) {
                ok = 0; why = "Konflikt nicht gemeldet";
            }
        }
        if (ok) {
            T_OK("60 photo-bind");
        } else {
            T_FAIL("60 photo-bind", "%s", why);
        }
    }

    {
        static lctrl_bind_t b1, b2; /* W414: static, je ~31 KB (Stapelrahmen cli_selftest) */
        char row[256], conf[4][160], err2[256], nm[32];
        int ok = 1, n, ph, bi0;
        const char *why = "";
        ph = lctrl_bind_elem_index("feedback_shot");
        bi0 = lctrl_bind_elem_index("bi0");
        lctrl_key_name(VK_F11, nm, sizeof nm);
        if (ph < 0 || bi0 < 0 || kCtrlElems[ph].kind != LCTRL_ACT || strcmp(nm, "F11") != 0) {
            ok = 0; why = "Element/Tastenname";
        } else {
            lctrl_bind_init(&b1);
            lctrl_bind_set(&b1, ph, "KEY:F11, PAD:BACK+PAD:LB");
            lctrl_bind_row_text(&b1, ph, row, sizeof row);
            u8_GetTempPath(tmp, sizeof tmp);
            strcat_s(tmp, sizeof tmp, "w211_feedback_shot_bind_test.ini");
            u8_DeleteFile(tmp);
            if (lctrl_bind_save(tmp, &b1, "keyboard_default", err2, sizeof err2) != 0) {
                ok = 0; why = "speichern";
            } else {
                memset(buf, 0, sizeof buf);
                if (u8_fopen_s(&f, tmp, "r") == 0 && f) {
                    (void)fread(buf, 1, sizeof buf - 1, f);
                    fclose(f);
                }
                if (!strstr(buf, "[client]") || !strstr(buf, "feedback_shot = KEY:F11, PAD:BACK+PAD:LB")) {
                    ok = 0; why = "Abschnitt [client]";
                }
                lctrl_bind_init(&b2);
                if (lctrl_bind_load_file(tmp, &b2) != 0) {
                    ok = 0; why = "laden";
                } else {
                    char row2[256];
                    lctrl_bind_row_text(&b2, ph, row2, sizeof row2);
                    if (strcmp(row, row2) != 0) { ok = 0; why = "laden != speichern"; }
                }
            }
            u8_DeleteFile(tmp);
            lctrl_bind_init(&b1);
            lctrl_bind_set(&b1, bi0, "KEY:F11");
            lctrl_bind_set(&b1, ph, "KEY:F11");
            n = lctrl_bind_conflicts(&b1, conf, 4);
            if (n < 1 || !strstr(conf[0], "KEY:F11") || !strstr(conf[0], "feedback_shot")) {
                ok = 0; why = "Konflikt nicht gemeldet";
            }
        }
        if (ok) {
            T_OK("W211 feedback_shot-bind");
        } else {
            T_FAIL("W211 feedback_shot-bind", "%s", why);
        }
    }

    {
        static lctrl_bind_t b1, b2; /* W414: static, je ~31 KB (Stapelrahmen cli_selftest) */
        char row[256], conf[4][160], err2[256], nm[32];
        int ok = 1, n, ph, bi0;
        const char *why = "";
        ph = lctrl_bind_elem_index("feedback_clip");
        bi0 = lctrl_bind_elem_index("bi0");
        lctrl_key_name(VK_F9, nm, sizeof nm);
        if (ph < 0 || bi0 < 0 || kCtrlElems[ph].kind != LCTRL_ACT || strcmp(nm, "F9") != 0) {
            ok = 0; why = "Element/Tastenname";
        } else {
            lctrl_bind_init(&b1);
            lctrl_bind_set(&b1, ph, "KEY:F9, PAD:BACK+PAD:LB");
            lctrl_bind_row_text(&b1, ph, row, sizeof row);
            u8_GetTempPath(tmp, sizeof tmp);
            strcat_s(tmp, sizeof tmp, "w211_feedback_clip_bind_test.ini");
            u8_DeleteFile(tmp);
            if (lctrl_bind_save(tmp, &b1, "keyboard_default", err2, sizeof err2) != 0) {
                ok = 0; why = "speichern";
            } else {
                memset(buf, 0, sizeof buf);
                if (u8_fopen_s(&f, tmp, "r") == 0 && f) {
                    (void)fread(buf, 1, sizeof buf - 1, f);
                    fclose(f);
                }
                if (!strstr(buf, "[client]") || !strstr(buf, "feedback_clip = KEY:F9, PAD:BACK+PAD:LB")) {
                    ok = 0; why = "Abschnitt [client]";
                }
                lctrl_bind_init(&b2);
                if (lctrl_bind_load_file(tmp, &b2) != 0) {
                    ok = 0; why = "laden";
                } else {
                    char row2[256];
                    lctrl_bind_row_text(&b2, ph, row2, sizeof row2);
                    if (strcmp(row, row2) != 0) { ok = 0; why = "laden != speichern"; }
                }
            }
            u8_DeleteFile(tmp);
            lctrl_bind_init(&b1);
            lctrl_bind_set(&b1, bi0, "KEY:F9");
            lctrl_bind_set(&b1, ph, "KEY:F9");
            n = lctrl_bind_conflicts(&b1, conf, 4);
            if (n < 1 || !strstr(conf[0], "KEY:F9") || !strstr(conf[0], "feedback_clip")) {
                ok = 0; why = "Konflikt nicht gemeldet";
            }
        }
        if (ok) {
            T_OK("W211 feedback_clip-bind");
        } else {
            T_FAIL("W211 feedback_clip-bind", "%s", why);
        }
    }

    /* 54 (W183 VR-FIX3): VR-Leistungs-Preset <-> Werte (Spiegel der Client-
     * Tabelle kVrPreset), Standard "Ausgewogen", Speichern/Laden. */
    {
        static const int want[4][6] = { { 0 }, { 65, 72, 50, 1, 0, 1 }, { 80, 0, 100, 1, 1, 2 }, { 100, 0, 100, 0, 1, 0 } };
        launcher_settings_t v1, v2;
        int ok = 1, pr, k;
        char why[96] = "";
        lcfg_defaults(&v1);
        if (v1.vr_preset != 2 || v1.vr_render_scale != 80 || lcfg_vr_preset_match(&v1) != 2) {
            ok = 0; strcpy_s(why, sizeof why, "Standard nicht Ausgewogen/80 %");
        }
        for (pr = 1; ok && pr <= 3; pr++) {
            for (k = 0; k < 6; k++) {
                if (lcfg_vr_preset_value(pr, k) != want[pr][k]) {
                    ok = 0; _snprintf_s(why, sizeof why, _TRUNCATE, "Tabelle Preset %d Wert %d", pr, k);
                }
            }
            lcfg_vr_preset_apply(&v1, pr);
            if (lcfg_vr_preset_match(&v1) != pr || v1.vr_preset != pr) {
                ok = 0; _snprintf_s(why, sizeof why, _TRUNCATE, "apply/match Preset %d", pr);
            }
        }
        lcfg_vr_preset_apply(&v1, 1);
        v1.vr_render_scale = 90;
        if (ok && lcfg_vr_preset_match(&v1) != 0) { ok = 0; strcpy_s(why, sizeof why, "eigene Werte nicht erkannt"); }
        lcfg_vr_preset_apply(&v1, 1);
        u8_GetTempPath(tmp, sizeof tmp);
        strcat_s(tmp, sizeof tmp, "w183_vrperf_test.cfg");
        u8_DeleteFile(tmp);
        if (ok && lcfg_save_user(tmp, &v1) != 0) { ok = 0; strcpy_s(why, sizeof why, "save scheiterte"); }
        if (ok) {
            memset(buf, 0, sizeof buf);
            f = NULL;
            if (u8_fopen_s(&f, tmp, "r") == 0 && f) { (void)fread(buf, 1, sizeof buf - 1, f); fclose(f); }
            lcfg_defaults(&v2);
            lcfg_load_file(tmp, &v2);
            if (!strstr(buf, "vr_preset=1") || !strstr(buf, "vr_render_scale=65") || !strstr(buf, "vr_refresh_hz=72") ||
                !strstr(buf, "vr_world_tick=1") || !strstr(buf, "vr_shading=0")) {
                ok = 0; strcpy_s(why, sizeof why, "Schluessel fehlen in user.cfg");
            } else if (v2.vr_preset != 1 || v2.vr_render_scale != 65 || v2.vr_mirror_res != 50 || v2.vr_rt_scale != 1 ||
                       lcfg_vr_preset_match(&v2) != 1) {
                ok = 0; strcpy_s(why, sizeof why, "Werte nach Laden ungleich");
            }
        }
        u8_DeleteFile(tmp);
        if (ok) {
            T_OK("54 vr-perf");
        } else {
            T_FAIL("54 vr-perf", "%s", why);
        }
    }

    /* 46 (W172 LAUNCHER-P6): Spieldaten-Validierung. Abbild hat Vorrang
     * (ungueltig -> Abbild-Fehler, Ordnerfelder egal); Ordner ohne
     * XBE-Feld -> <Ordner>/default.xbe abgeleitet, user.cfg bekommt den
     * aufgeloesten Pfad; Ordner ohne default.xbe -> cfg.no_xbe; nichts
     * gesetzt -> cfg.no_media; ein gesetztes Feld bleibt massgeblich. */
    {
        char gd[1024], gx[1200], gm[1200], cfgp[1200], eff[1024], want[1300];
        int ok = 1;
        char why[200] = "";
        u8_GetTempPath(gd, sizeof gd);
        strcat_s(gd, sizeof gd, "w172_game_test");
        {
            char *q; /* Trenner einheitlich '/' (Ableitung folgt dem Ordner) */
            for (q = gd; *q; q++) {
                if (*q == '\\') {
                    *q = '/';
                }
            }
        }
        _snprintf_s(gm, sizeof gm, _TRUNCATE, "%s/Media", gd);
        lcfg_mkdir_p(gm);
        _snprintf_s(gx, sizeof gx, _TRUNCATE, "%s/default.xbe", gd);
        f = NULL;
        if (u8_fopen_s(&f, gx, "wb") == 0 && f) {
            fclose(f);
        }
        /* a) Ordner ohne XBE-Feld */
        lcfg_defaults(&b);
        b.game_image[0] = '\0';
        b.xbe_path[0] = '\0';
        strcpy_s(b.media_root, sizeof b.media_root, gd);
        lcfg_xbe_effective(&b, eff, sizeof eff);
        _snprintf_s(want, sizeof want, _TRUNCATE, "%s/default.xbe", gd);
        if (lcfg_validate(&b, err, sizeof err) != 0 || strcmp(eff, want) != 0) {
            ok = 0;
            _snprintf_s(why, sizeof why, _TRUNCATE, "a) %.60s / %.60s", err, eff);
        }
        u8_GetTempPath(cfgp, sizeof cfgp);
        strcat_s(cfgp, sizeof cfgp, "w172_xbe_test.cfg");
        u8_DeleteFile(cfgp);
        if (ok && lcfg_save_user(cfgp, &b) == 0) {
            lcfg_defaults(&a);
            lcfg_load_file(cfgp, &a);
            if (strcmp(a.xbe_path, want) != 0) {
                ok = 0;
                _snprintf_s(why, sizeof why, _TRUNCATE, "a) user.cfg xbe_path='%.80s'",
                            a.xbe_path);
            }
        } else if (ok) {
            ok = 0;
            strcpy_s(why, sizeof why, "a) save");
        }
        u8_DeleteFile(cfgp);
        if (ok) { /* Ordner mit Backslash -> Trenner Backslash */
            char bsd[1024], *q;
            strcpy_s(bsd, sizeof bsd, gd);
            for (q = bsd; *q; q++) {
                if (*q == '/') {
                    *q = '\\';
                }
            }
            strcpy_s(b.media_root, sizeof b.media_root, bsd);
            lcfg_xbe_effective(&b, eff, sizeof eff);
            _snprintf_s(want, sizeof want, _TRUNCATE, "%s\\default.xbe", bsd);
            if (strcmp(eff, want) != 0 || lcfg_validate(&b, err, sizeof err) != 0) {
                ok = 0;
                _snprintf_s(why, sizeof why, _TRUNCATE, "a2) %.80s", eff);
            }
            strcpy_s(b.media_root, sizeof b.media_root, gd);
        }
        /* b) gesetztes Feld bleibt massgeblich */
        if (ok) {
            strcpy_s(b.xbe_path, sizeof b.xbe_path, "C:/gibt/es/nicht/x.xbe");
            lcfg_xbe_effective(&b, eff, sizeof eff);
            if (strcmp(eff, b.xbe_path) != 0 ||
                lcfg_validate(&b, err, sizeof err) == 0 || !strstr(err, "x.xbe")) {
                ok = 0;
                _snprintf_s(why, sizeof why, _TRUNCATE, "b) %.80s", err);
            }
            b.xbe_path[0] = '\0';
        }
        /* c) Ordner ohne default.xbe -> cfg.no_xbe mit Ordnername */
        if (ok) {
            strcpy_s(b.media_root, sizeof b.media_root, gm); /* Media/ ohne XBE */
            _snprintf_s(want, sizeof want, _TRUNCATE, LT("cfg.no_xbe"), gm);
            if (lcfg_validate(&b, err, sizeof err) == 0 || strcmp(err, want) != 0) {
                ok = 0;
                _snprintf_s(why, sizeof why, _TRUNCATE, "c) %.80s", err);
            }
            strcpy_s(b.media_root, sizeof b.media_root, gd);
        }
        /* d) nichts gesetzt -> cfg.no_media (Abbild empfohlen) */
        if (ok) {
            launcher_settings_t e2 = b;
            e2.xbe_path[0] = e2.media_root[0] = e2.game_image[0] = '\0';
            if (lcfg_validate(&e2, err, sizeof err) == 0 ||
                strcmp(err, LT("cfg.no_media")) != 0) {
                ok = 0;
                _snprintf_s(why, sizeof why, _TRUNCATE, "d) %.80s", err);
            }
        }
        /* e) ungueltiges Abbild hat Vorrang vor dem gueltigen Ordner */
        if (ok) {
            launcher_settings_t e3 = b;
            _snprintf_s(e3.game_image, sizeof e3.game_image, _TRUNCATE,
                        "%s/gibt_es_nicht.xiso", gd);
            lcfg_xbe_effective(&e3, eff, sizeof eff);
            if (lcfg_validate(&e3, err, sizeof err) == 0 || eff[0] ||
                strstr(err, "default.xbe")) {
                ok = 0;
                _snprintf_s(why, sizeof why, _TRUNCATE, "e) %.80s", err);
            }
        }
        u8_DeleteFile(gx);
        {
            wchar_t w[1200];
            u8_to_w(gm, w, 1200);
            RemoveDirectoryW(w);
            u8_to_w(gd, w, 1200);
            RemoveDirectoryW(w);
        }
        if (ok) {
            T_OK("46 game-data (Abbild / Ordner ohne XBE-Feld / ungueltig)");
        } else {
            T_FAIL("46 game-data", "%s", why);
        }
    }

    /* W431 DEFAULT-BINDINGS: frische Installation (kind 0) -> default_full; vorhandene cfg ohne Profil bleibt
     * Legacy; gesetztes Profil und fehlende Profildatei bleiben unberuehrt. */
    {
        char cd[1100], pd[1200], pf[1300];
        launcher_settings_t fs;
        FILE *pfh = NULL;
        int ok;
        u8_GetTempPath(tmp, sizeof tmp);
        _snprintf_s(cd, sizeof cd, _TRUNCATE, "%sw431_client", tmp);
        _snprintf_s(pd, sizeof pd, _TRUNCATE, "%s/profiles", cd);
        _snprintf_s(pf, sizeof pf, _TRUNCATE, "%s/default_full.ini", pd);
        lcfg_mkdir_p(pd);
        lcfg_defaults(&fs);
        ok = app_firstrun_input_profile(&fs, cd, 0) == 0 && !fs.input_profile[0]; /* Datei fehlt */
        if (fopen_s(&pfh, pf, "wb") == 0 && pfh) {
            fputs("[buttons]\nbi0 = KEY:SPACE\n", pfh);
            fclose(pfh);
        } else {
            ok = 0;
        }
        lcfg_defaults(&fs);
        ok = ok && app_firstrun_input_profile(&fs, cd, 0) == 1 && strcmp(fs.input_profile, "default_full") == 0;
        lcfg_defaults(&fs);
        ok = ok && app_firstrun_input_profile(&fs, cd, 1) == 0 && !fs.input_profile[0]; /* user.cfg da: Legacy */
        ok = ok && app_firstrun_input_profile(&fs, cd, 2) == 0 && !fs.input_profile[0]; /* Standard-Profil da */
        strcpy_s(fs.input_profile, sizeof fs.input_profile, "user_mein");
        ok = ok && app_firstrun_input_profile(&fs, cd, 0) == 0 && strcmp(fs.input_profile, "user_mein") == 0;
        u8_DeleteFile(pf);
        RemoveDirectoryA(pd);
        RemoveDirectoryA(cd);
        if (ok) {
            T_OK("W431 firstrun-profile (frisch -> default_full, vorhandene cfg bleibt Legacy, eigenes Profil/fehlende Datei unberuehrt)");
        } else {
            T_FAIL("W431 firstrun-profile", "%s", "Vorwahl falsch");
        }
    }

    /* W438 CTRL-RESET-040: einmaliger Reset auf default_full (genau einmal, user_*.ini unveraendert) */
    {
        char why[200];
        u8_GetTempPath(tmp, sizeof tmp);
        if (w438_ctrl_reset_selftest(tmp, why, sizeof why) == 0) {
            T_OK("W438 ctrl-reset-040 (genau einmal, user_*.ini unveraendert, neue Installation ohne Reset, Markierung bleibt beim Speichern)");
        } else {
            T_FAIL("W438 ctrl-reset-040", "%s", why);
        }
    }

    /* W217 FIRST-RUN: frischer Nutzerordner (Profil-Modus mit leerer Wurzel
     * wie ein neuer PC; nie das echte %LOCALAPPDATA%). Laden schreibt nichts,
     * Standardwerte + Erststart-Regeln, erstes Speichern legt die Wurzel an,
     * danach gilt user.cfg; Laufzeit-Pruefung und statische Launcher-CRT. */
    {
        char root[1100], td[1100], up[1200], q[1200], miss[64], why[240] = "";
        launcher_settings_t fs, f2;
        int kind = -1, r;
        DWORD at;
        u8_GetTempPath(tmp, sizeof tmp);
        _snprintf_s(root, sizeof root, _TRUNCATE, "%sw217_fresh_ud", tmp);
        _snprintf_s(td, sizeof td, _TRUNCATE, "%sw217_fresh_client", tmp);
        _snprintf_s(q, sizeof q, _TRUNCATE, "%s/user.cfg", root);
        u8_DeleteFile(q);
        _snprintf_s(q, sizeof q, _TRUNCATE, "%s/packs", root);
        RemoveDirectoryA(q);
        RemoveDirectoryA(root);
        SetEnvironmentVariableA("SB_USERDATA", "profile");
        SetEnvironmentVariableA("SB_USERDATA_ROOT", root);
        sb_userdata_reset();
        app_load_effective(td, &fs, NULL, 0, &kind, NULL);
        lcfg_user_cfg_path(td, up, sizeof up);
        at = u8_GetFileAttributes(up);
        if (kind != 0) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "kind=%d statt 0", kind);
        } else if (at != INVALID_FILE_ATTRIBUTES) {
            strcpy_s(why, sizeof why, "Laden hat user.cfg angelegt");
        } else if (_strnicmp(up, root, strlen(root)) != 0) {
            strcpy_s(why, sizeof why, "user.cfg nicht in der frischen Wurzel");
        } else if (fs.game_image[0] || fs.window_mode != 0 || fs.input_profile[0] ||
                   fs.sbc_usb != 0 || fs.interp_fps != SB_FPS_AUTO || fs.aspect_mode != 1 || /* W296: Original = auto */
                   lcfg_gfx_preset(&fs, 0) != LGP_ORIGINAL) {
            strcpy_s(why, sizeof why, "Standardwerte (Abbild leer, Fenster, Tastatur, Original auto)");
        }
        if (!why[0]) { /* Erststart-Regeln: Sprache + Fenster passt / randlos */
            f2 = fs;
            r = app_firstrun_defaults(&f2, 0x0407 /* de-DE */, 1920, 1040);
            if (f2.window_mode != 1 || !(r & 2) ||
                strcmp(f2.lang, SB_EDITION_COMMUNITY ? "de" : fs.lang) != 0) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "1920x1040 de: r=%d mode=%d lang=%s",
                            r, f2.window_mode, f2.lang);
            }
            f2 = fs;
            r = app_firstrun_defaults(&f2, 0x0409 /* en-US */, 3440, 1400);
            if (r != 0 || f2.window_mode != 0 || strcmp(f2.lang, fs.lang) != 0) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "3440x1400 en: r=%d mode=%d", r,
                            f2.window_mode);
            }
            f2 = fs;
            f2.window_mode = 2; /* Vollbild bleibt Vollbild */
            if (!why[0] && (app_firstrun_defaults(&f2, 0, 800, 600) & 2)) {
                strcpy_s(why, sizeof why, "exklusives Vollbild wurde umgestellt");
            }
        }
        if (!why[0]) { /* erstes Speichern legt Wurzel + user.cfg an */
            fs.width = 1600;
            fs.height = 900;
            if (lcfg_save_user(up, &fs) != 0) {
                strcpy_s(why, sizeof why, "Speichern in frische Wurzel scheitert");
            } else {
                app_load_effective(td, &f2, NULL, 0, &kind, NULL);
                if (kind != 1 || f2.width != 1600 || f2.height != 900) {
                    _snprintf_s(why, sizeof why, _TRUNCATE, "neu geladen: kind=%d %ux%u",
                                kind, f2.width, f2.height);
                }
            }
        }
        if (!why[0]) { /* Laufzeit-Pruefung: fehlende DLL melden, vorhandene nicht */
            if (app_client_runtime_missing(td, "sb_w217_gibt_es_nicht.dll", miss, sizeof miss) != 1 ||
                strcmp(miss, "sb_w217_gibt_es_nicht.dll") != 0 ||
                app_client_runtime_missing(td, "kernel32.dll", miss, sizeof miss) != 0 ||
                app_client_runtime_missing(td, NULL, miss, sizeof miss) != 0) {
                strcpy_s(why, sizeof why, "Laufzeit-Pruefung falsch");
            } else if (app_pe_imports(NULL, GetModuleHandleW(NULL), "KERNEL32.dll") != 1 ||
                       app_pe_imports(NULL, GetModuleHandleW(NULL), "VCRUNTIME140.dll") != 0) {
                strcpy_s(why, sizeof why, "Launcher importiert VCRUNTIME140.dll (CRT nicht statisch)");
            }
        }
        if (!why[0]) { /* Ordner statt Abbild: eigener Hinweis */
            char m[1200], want[1200];
            _snprintf_s(want, sizeof want, _TRUNCATE, LT("cfg.image_isdir"), tmp);
            if (lcfg_image_check(tmp, m, sizeof m) != 1 || strcmp(m, want) != 0) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "Ordner als Abbild: %.120s", m);
            }
        }
        _snprintf_s(q, sizeof q, _TRUNCATE, "%s/user.cfg", root);
        u8_DeleteFile(q);
        _snprintf_s(q, sizeof q, _TRUNCATE, "%s/packs", root);
        RemoveDirectoryA(q);
        RemoveDirectoryA(root);
        SetEnvironmentVariableA("SB_USERDATA", "legacy");
        SetEnvironmentVariableA("SB_USERDATA_ROOT", NULL);
        sb_userdata_reset();
        sb_lang_set("de");
        if (!why[0]) {
            T_OK("W217 first-run (frischer Nutzerordner: Standardwerte, Erststart-Regeln, Speichern, Laufzeit)");
        } else {
            T_FAIL("W217 first-run", "%s", why);
        }
    }

    /* W221 HOTAS (experimentell): joyN-Zeilen + JOY-Belegungen laden,
     * aendern, speichern, neu laden; Lernhilfe/Slot/Parameter rein (ohne
     * Hardware); Geraeteliste ohne Geraet = leer, kein Fehler. */
    {
        static const char *jt =
            "# W221-Test\n[joystick]\njoy0 = Saitek X52 Throttle # Kommentar\n"
            "joy2 = {11223344-0000-0000-0000-504944564944}@2\n"
            "[buttons]\nbi0 = KEY:SPACE, JOY0:B1\n"
            "[axes]\nrot = JOY0:X:dz=0.05:inv=1\n"
            "[gear]\ngear.3 = JOY0:Z:zone=40-60\n";
        static lctrl_bind_t j1, j2, j3;
        char why[240] = "", jerr[256], jp[1100], jb[8192], t1[400], t2[400];
        int rot = lctrl_bind_elem_index("rot");
        int pl = lctrl_bind_elem_index("pedal_l");
        int ax = lctrl_bind_elem_index("aim_x");
        int b0 = lctrl_bind_elem_index("bi0"), b1 = lctrl_bind_elem_index("bi1");
        int g3 = lctrl_bind_elem_index("gear.3"), i, n, sl, ax2 = -1;
        FILE *jf = NULL;
        lctrl_bind_load_text(jt, &j1);
        if (strcmp(j1.joy[0], "Saitek X52 Throttle") != 0 || j1.joy[1][0] ||
            strcmp(j1.joy[2], "{11223344-0000-0000-0000-504944564944}@2") != 0) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "joy-Zeilen: '%s' '%s'", j1.joy[0], j1.joy[2]);
        } else if (j1.row[rot].n_alt != 1 ||
                   strcmp(j1.row[rot].alt[0], "JOY0:X:dz=0.05:inv=1") != 0 ||
                   j1.row[b0].n_alt != 2 || j1.row[g3].n_alt != 1) {
            strcpy_s(why, sizeof why, "JOY-Belegungen nicht geladen");
        } else if (lctrl_bind_add(&j1, pl, "JOY1:Y:half=1") != 0 ||
                   lctrl_bind_set(&j1, b1, "JOY0:POV1U, JOY0:Z+") != 0) {
            strcpy_s(why, sizeof why, "lctrl_bind_add/set");
        }
        u8_GetTempPath(jp, sizeof jp);
        strcat_s(jp, sizeof jp, "w221_joy_test.ini");
        if (!why[0]) {
            u8_DeleteFile(jp);
            memset(jb, 0, sizeof jb);
            if (lctrl_bind_save(jp, &j1, "t", jerr, sizeof jerr) != 0) {
                strcpy_s(why, sizeof why, "speichern");
            } else if (u8_fopen_s(&jf, jp, "r") == 0 && jf) {
                (void)fread(jb, 1, sizeof jb - 1, jf);
                fclose(jf);
            }
        }
        if (!why[0] &&
            (!strstr(jb, "\n[joystick]\njoy0 = Saitek X52 Throttle\n"
                         "joy2 = {11223344-0000-0000-0000-504944564944}@2\n") ||
             !strstr(jb, "pedal_l = JOY1:Y:half=1\n") ||
             !strstr(jb, "bi1 = JOY0:POV1U, JOY0:Z+\n") ||
             !strstr(jb, "rot = JOY0:X:dz=0.05:inv=1\n"))) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "Datei: %.160s", jb);
        }
        if (!why[0]) {
            if (lctrl_bind_load_file(jp, &j2) != 0) {
                strcpy_s(why, sizeof why, "neu laden");
            } else {
                for (i = 0; i < IMAP_JOY_MAX && !why[0]; i++) {
                    if (strcmp(j1.joy[i], j2.joy[i]) != 0) {
                        _snprintf_s(why, sizeof why, _TRUNCATE, "joy%d nach Neuladen", i);
                    }
                }
                for (i = 0; i < LCTRL_ELEM_N && !why[0]; i++) {
                    lctrl_bind_row_text(&j1, i, t1, sizeof t1);
                    lctrl_bind_row_text(&j2, i, t2, sizeof t2);
                    if (strcmp(t1, t2) != 0) {
                        _snprintf_s(why, sizeof why, _TRUNCATE, "%s: '%s' != '%s'",
                                    kCtrlElems[i].key, t1, t2);
                    }
                }
            }
        }
        if (!why[0]) { /* ohne Joystick: kein [joystick]-Abschnitt */
            lctrl_bind_init(&j3);
            lctrl_bind_set(&j3, b0, "KEY:SPACE");
            memset(jb, 0, sizeof jb);
            if (lctrl_bind_save(jp, &j3, "t", jerr, sizeof jerr) == 0 &&
                u8_fopen_s(&jf, jp, "r") == 0 && jf) {
                (void)fread(jb, 1, sizeof jb - 1, jf);
                fclose(jf);
            }
            if (strstr(jb, "joystick") || strstr(jb, "joy0") ||
                !strstr(jb, "bi0 = KEY:SPACE\n")) {
                strcpy_s(why, sizeof why, "Profil ohne Joystick veraendert");
            }
        }
        u8_DeleteFile(jp);
        if (!why[0]) { /* Lernhilfe mit simulierten Zustaenden */
            imap_joy_state_t jb0, jpv, jc;
            char o[48];
            int r;
            memset(&jb0, 0, sizeof jb0);
            for (i = 0; i < IJOY_POVS; i++) {
                jb0.pov[i] = -1;
            }
            jpv = jb0;
            jc = jb0;
            jc.btn[0] = 1u << 2;
            r = lctrl_joy_learn(&jb0, &jpv, &jc, 0xFF, 1, b0, o, sizeof o);
            if (r != 1 || strcmp(o, "B3") != 0) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "Taste: %d '%s'", r, o);
            }
            jpv = jc;
            jc.pov[0] = 9000;
            r = lctrl_joy_learn(&jb0, &jpv, &jc, 0xFF, 1, b0, o, sizeof o);
            if (!why[0] && (r != 2 || strcmp(o, "POV1R") != 0)) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "POV: %d '%s'", r, o);
            }
            r = lctrl_joy_learn(&jb0, &jpv, &jc, 0xFF, 1, pl, o, sizeof o);
            if (!why[0] && r != 0) { /* Taste/POV auf Achsen-Element: nichts */
                _snprintf_s(why, sizeof why, _TRUNCATE, "Taste auf Achse: %d '%s'", r, o);
            }
            /* Schubhebel ruht am Minimum: Gang-Direktwahl -> Zone um die Stellung */
            jb0 = jpv = jc = (imap_joy_state_t){ 0 };
            for (i = 0; i < IJOY_POVS; i++) {
                jb0.pov[i] = jpv.pov[i] = jc.pov[i] = -1;
            }
            jb0.axis[2] = jpv.axis[2] = -32768;
            jc.axis[2] = 32767;
            r = lctrl_joy_learn(&jb0, &jpv, &jc, 0xFF, 0, g3, o, sizeof o);
            if (!why[0] && (r != 3 || strcmp(o, "Z:zone=88-100") != 0)) { /* W226: Gang = Zone */
                _snprintf_s(why, sizeof why, _TRUNCATE, "Gang voll: %d '%s'", r, o);
            }
            jc.axis[2] = 0;
            r = lctrl_joy_learn(&jb0, &jpv, &jc, 0xFF, 0, g3, o, sizeof o);
            if (!why[0] && (r != 3 || strcmp(o, "Z:zone=38-62") != 0)) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "Zone: %d '%s'", r, o);
            }
            /* Pedal ruht am Maximum, Weg zum Minimum -> invertiert */
            jb0.axis[2] = jpv.axis[2] = jc.axis[2] = 0;
            jb0.axis[1] = 32767;
            jc.axis[1] = -32768;
            r = lctrl_joy_learn(&jb0, &jpv, &jc, 0xFF, 0, pl, o, sizeof o);
            if (!why[0] && (r != 3 || strcmp(o, "Y:dz=0.05:inv=1") != 0)) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "Pedal: %d '%s'", r, o);
            }
            jb0.axis[1] = jc.axis[1] = 0;
            jc.axis[0] = 30000; /* 46 % */
            r = lctrl_joy_learn(&jb0, &jpv, &jc, 0xFF, 0, ax, o, sizeof o);
            if (!why[0] && (r != 3 || strcmp(o, "X:dz=0.05") != 0)) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "Achse: %d '%s'", r, o);
            }
            jc.axis[0] = 5000; /* unter 40 %: nichts */
            r = lctrl_joy_learn(&jb0, &jpv, &jc, 0xFF, 0, ax, o, sizeof o);
            if (!why[0] && r != 0) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "Rauschen: %d '%s'", r, o);
            }
            jc.axis[0] = 30000; /* Achse fehlt am Geraet: nichts */
            r = lctrl_joy_learn(&jb0, &jpv, &jc, 0xFE, 0, ax, o, sizeof o);
            if (!why[0] && r != 0) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "fehlende Achse: %d", r);
            }
        }
        if (!why[0]) { /* Slot-Zuordnung */
            ijoy_info_t jl[3];
            memset(jl, 0, sizeof jl);
            strcpy_s(jl[0].product, sizeof jl[0].product, "Saitek X52 Throttle");
            strcpy_s(jl[1].product, sizeof jl[1].product, "Pedals");
            strcpy_s(jl[2].product, sizeof jl[2].product, "Pedals");
            strcpy_s(jl[2].guid, sizeof jl[2].guid, "{A}");
            sl = lctrl_joy_slot_for(&j1, jl, 3, 0);
            n = lctrl_joy_slot_for(&j1, jl, 3, 1); /* JOY1 implizit = Geraet 2 */
            if (sl != 0 || n != 1 || j1.joy[1][0]) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "Slot: %d %d '%s'", sl, n, j1.joy[1]);
            }
            lctrl_bind_init(&j3);
            sl = lctrl_joy_slot_for(&j3, jl, 3, 2);
            if (!why[0] && (sl != 0 || strcmp(j3.joy[0], "Pedals@2") != 0)) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "neuer Slot: %d '%s'", sl, j3.joy[0]);
            }
            sl = lctrl_joy_slot_for(&j3, jl, 3, 2); /* zweites Mal: dieselbe Zeile */
            n = lctrl_joy_slot_for(&j3, jl, 3, 0);
            if (!why[0] && (sl != 0 || n != 1 ||
                            strcmp(j3.joy[1], "Saitek X52 Throttle") != 0)) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "Slot 2: %d %d '%s'", sl, n, j3.joy[1]);
            }
        }
        if (!why[0]) { /* Parameter + Erkennung */
            char a[96] = "JOY0:X:dz=0.05:inv=1";
            int s2 = -1;
            lctrl_joy_param_set(a, sizeof a, "dz", "0.10");
            if (strcmp(a, "JOY0:X:dz=0.10:inv=1") != 0) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "dz setzen: '%s'", a);
            }
            lctrl_joy_param_set(a, sizeof a, "inv", NULL);
            if (!why[0] && strcmp(a, "JOY0:X:dz=0.10") != 0) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "inv weg: '%s'", a);
            }
            lctrl_joy_param_set(a, sizeof a, "inv", "1");
            lctrl_joy_param_set(a, sizeof a, "dz", NULL);
            if (!why[0] && strcmp(a, "JOY0:X:inv=1") != 0) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "inv/dz: '%s'", a);
            }
            if (!why[0] &&
                (!lctrl_joy_is_axis("JOY:RX", &s2, &ax2) || s2 != 0 || ax2 != 3 ||
                 lctrl_joy_is_axis("JOY0:Z+", NULL, NULL) ||
                 lctrl_joy_is_axis("JOY0:Z-", NULL, NULL) ||
                 lctrl_joy_is_axis("JOY1:Z:zone=20-40", NULL, NULL) ||
                 lctrl_joy_is_axis("JOY0:B3", NULL, NULL) ||
                 lctrl_joy_is_axis("PAD:LX", NULL, NULL) ||
                 !lctrl_joy_is_axis("JOY3:S1:dz=0.1", &s2, &ax2) || s2 != 3 || ax2 != 7)) {
                strcpy_s(why, sizeof why, "lctrl_joy_is_axis");
            }
            if (!why[0] && (!lctrl_joy_atom_text("JOY1:B3", t1, sizeof t1) ||
                            !strstr(t1, "2") || !strstr(t1, "3") ||
                            !lctrl_joy_atom_text("JOY0:POV1L", t1, sizeof t1) ||
                            !lctrl_joy_atom_text("JOY0:Z:zone=20-40", t1, sizeof t1) ||
                            !strstr(t1, "20-40") ||
                            lctrl_joy_atom_text("KEY:A", t2, sizeof t2))) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "lesbar: '%s'", t1);
            }
        }
        if (!why[0]) { /* echte Geraeteliste: ohne Geraet leer, nie Fehler */
            if (ijoy_init() == 0) {
                n = ijoy_scan();
                if (n < 0 || n != ijoy_count() || (n > 0 && !ijoy_info(0))) {
                    _snprintf_s(why, sizeof why, _TRUNCATE, "ijoy_scan %d", n);
                }
                printf("  W221: %d DirectInput-Geraet(e) erkannt\n", n);
            }
        }
        if (!why[0]) {
            T_OK("W221 HOTAS: joyN-Zeilen + JOY-Belegungen erhalten, Lernhilfe, Slots, dz/inv, lesbar");
        } else {
            T_FAIL("W221 HOTAS", "%s", why);
        }
    }
    /* W226 JOY-AXIS-DIR: Lern-Richtung (inv automatisch), Richtung statt
     * Zone bei Tasten/.pos/.neg, Basis einschwingen, Spitze/Stabilitaet,
     * Vorschau Roh -> Ergebnis wie im Client. Ohne Hardware. */
    {
        char why[240] = "", o[48], t[200];
        imap_joy_state_t b0, c0;
        lctrl_joy_learner_t L;
        int pr = lctrl_bind_elem_index("pedal_r");
        int pl = lctrl_bind_elem_index("pedal_l");
        int ax = lctrl_bind_elem_index("aim_x");
        int ay = lctrl_bind_elem_index("aim_y");
        int rot = lctrl_bind_elem_index("rot");
        int rp = lctrl_bind_elem_index("rot.pos");
        int rn = lctrl_bind_elem_index("rot.neg");
        int b0i = lctrl_bind_elem_index("bi0");
        int g2 = lctrl_bind_elem_index("gear.2");
        int r, i, at;
        float v;
        memset(&b0, 0, sizeof b0);
        for (i = 0; i < IJOY_POVS; i++) {
            b0.pov[i] = -1;
        }
        c0 = b0;
        /* 1. Umgekehrter Schubhebel: Ruhe 32767, Vollgas -32768 -> inv=1 */
        b0.axis[2] = 32767;
        c0.axis[2] = -32768;
        r = lctrl_joy_learn(&b0, &b0, &c0, 0xFF, 0, pr, o, sizeof o);
        if (r != 3 || strcmp(o, "Z:dz=0.05:inv=1") != 0) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "Schub umgekehrt: %d '%s'", r, o);
        }
        /* Schubhebel richtig herum (Ruhe -32768) -> kein inv */
        b0.axis[2] = -32768;
        c0.axis[2] = 32767;
        r = lctrl_joy_learn(&b0, &b0, &c0, 0xFF, 0, pr, o, sizeof o);
        if (!why[0] && (r != 3 || strcmp(o, "Z:dz=0.05") != 0)) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "Schub richtig: %d '%s'", r, o);
        }
        b0.axis[2] = c0.axis[2] = 0;
        /* 2. Zentrierte Achse nach links, verlangt rechts -> inv=1 */
        c0.axis[0] = -30000;
        r = lctrl_joy_learn(&b0, &b0, &c0, 0xFF, 0, ax, o, sizeof o);
        if (!why[0] && (r != 3 || strcmp(o, "X:dz=0.05:inv=1") != 0)) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "X links: %d '%s'", r, o);
        }
        c0.axis[0] = 30000; /* richtig -> kein inv */
        r = lctrl_joy_learn(&b0, &b0, &c0, 0xFF, 0, ax, o, sizeof o);
        if (!why[0] && (r != 3 || strcmp(o, "X:dz=0.05") != 0)) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "X rechts: %d '%s'", r, o);
        }
        c0.axis[0] = 0;
        c0.axis[1] = -30000; /* Zielhebel Y: vorn verlangt, vorn = Minimum */
        r = lctrl_joy_learn(&b0, &b0, &c0, 0xFF, 0, ay, o, sizeof o);
        if (!why[0] && (r != 3 || strcmp(o, "Y:dz=0.05") != 0)) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "Y vorn: %d '%s'", r, o);
        }
        c0.axis[1] = 30000; /* gezogen -> inv */
        r = lctrl_joy_learn(&b0, &b0, &c0, 0xFF, 0, ay, o, sizeof o);
        if (!why[0] && (r != 3 || strcmp(o, "Y:dz=0.05:inv=1") != 0)) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "Y gezogen: %d '%s'", r, o);
        }
        c0.axis[1] = 0;
        /* Kombinierte Ruderachse (Ruhe Mitte) auf Pedal: half=1, links inv */
        c0.axis[5] = -30000;
        r = lctrl_joy_learn(&b0, &b0, &c0, 0xFF, 0, pl, o, sizeof o);
        if (!why[0] && (r != 3 || strcmp(o, "RZ:dz=0.05:inv=1:half=1") != 0)) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "Ruder: %d '%s'", r, o);
        }
        c0.axis[5] = 0;
        /* 3. .pos/.neg/Taste: Richtung, nie Zone um die Ruhe */
        c0.axis[5] = 32767;
        r = lctrl_joy_learn(&b0, &b0, &c0, 0xFF, 0, rp, o, sizeof o);
        if (!why[0] && (r != 3 || strcmp(o, "RZ+") != 0)) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "rot.pos: %d '%s'", r, o);
        }
        c0.axis[5] = -32768;
        r = lctrl_joy_learn(&b0, &b0, &c0, 0xFF, 0, rn, o, sizeof o);
        if (!why[0] && (r != 3 || strcmp(o, "RZ-") != 0)) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "rot.neg: %d '%s'", r, o);
        }
        /* Schub ruht am Minimum, nur bis Mitte bewegt: Taste -> Zone ab
         * halbem Weg (Ruhe ausserhalb), Gang -> Zone um die Stellung */
        b0.axis[2] = -32768;
        c0 = b0;
        c0.axis[2] = 0; /* 50 % */
        r = lctrl_joy_learn(&b0, &b0, &c0, 0xFF, 0, b0i, o, sizeof o);
        if (!why[0] && (r != 3 || strcmp(o, "Z:zone=25-100") != 0)) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "Taste halb: %d '%s'", r, o);
        }
        r = lctrl_joy_learn(&b0, &b0, &c0, 0xFF, 0, g2, o, sizeof o);
        if (!why[0] && (r != 3 || strcmp(o, "Z:zone=38-62") != 0)) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "Gang-Zone: %d '%s'", r, o);
        }
        /* 4. Nutzerbefund: Basis veraltet (alle Achsen -32768 vor dem ersten
         * Geraete-Bericht), Drehachse ruht bei 55 %. Einmal-Lernen alt:
         * Zone 43-67 um die Ruhe. Lerner: erste Aenderung = neue Basis,
         * danach Drehen nach rechts -> RZ+. */
        {
            static const int seqA[] = { -32768, -32768, -32768, -32768, 3277,
                                        3277, 9000, 20000, 30000, 32767,
                                        32767, 32767 };
            lctrl_joy_learner_reset(&L);
            r = w226_feed(&L, rp, 0x3F, 5, -32768, seqA, 12, o, sizeof o, &at);
            if (!why[0] && (r != 3 || strcmp(o, "RZ+") != 0 || at < 10)) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "veraltete Basis: %d '%s' @%d", r, o, at);
            }
        }
        /* Einzelachse exakt am Endwert, Sprung > 25 % = erster Bericht */
        {
            static const int seqB[] = { -32768, -32768, -32768, 3277, 3277,
                                        3277, 3277 };
            lctrl_joy_learner_reset(&L);
            r = w226_feed(&L, rp, 0x01, 0, 0, seqB, 7, o, sizeof o, &at);
            if (!why[0] && r != 0) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "Endwert-Sprung: %d '%s'", r, o);
            }
        }
        /* Umgekehrter Schubhebel langsam geschoben (Ruhe 32767 exakt): kein
         * Fehl-Rebase, erst bei Ruhe uebernehmen -> inv=1 */
        {
            static const int seqC[] = { 32767, 32767, 32767, 28000, 20000,
                                        8000, -6000, -20000, -32768, -32768,
                                        -32768 };
            lctrl_joy_learner_reset(&L);
            r = w226_feed(&L, pr, 0x07, 2, 0, seqC, 11, o, sizeof o, &at);
            if (!why[0] && (r != 3 || strcmp(o, "Z:dz=0.05:inv=1") != 0 || at < 9)) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "Schub langsam: %d '%s' @%d", r, o, at);
            }
        }
        /* In Bewegung (erster Wert > 40 %) nicht uebernehmen; Achse federt
         * zurueck -> Spitze zaehlt (rot.pos -> RZ+) */
        {
            static const int seqD[] = { 0, 0, 0, 27000, 32767, 12000, 0, 0 };
            lctrl_joy_learner_reset(&L);
            r = w226_feed(&L, rp, 0x3F, 5, 100, seqD, 8, o, sizeof o, &at);
            if (!why[0] && (r != 3 || strcmp(o, "RZ+") != 0 || at != 5)) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "zurueckgefedert: %d '%s' @%d", r, o, at);
            }
        }
        /* Unruhige Basis bzw. Lesefehler: kein Lernen */
        {
            static const int seqE[] = { 0, 30000, -30000, 30000, -30000 };
            imap_joy_state_t z;
            lctrl_joy_learner_reset(&L);
            r = w226_feed(&L, ax, 0x01, 0, 0, seqE, 5, o, sizeof o, &at);
            memset(&z, 0, sizeof z);
            for (i = 0; i < 10 && !r; i++) {
                r = lctrl_joy_learner_step(&L, 0, &z, 0x01, 0, ax, o, sizeof o);
            }
            if (!why[0] && (r != 0 || L.have_base)) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "ohne Basis gelernt: %d '%s'", r, o);
            }
        }
        /* 5. Vorschau Roh -> Ergebnis (wie input_map.c) */
        if (!why[0]) {
            int k1 = lctrl_joy_eval("JOY0:Z:inv=1", 1, 32767, &v);
            int ok = k1 == 1 && v < 0.01f;
            ok = ok && lctrl_joy_eval("JOY0:Z:inv=1", 1, -32768, &v) == 1 && v > 0.99f;
            ok = ok && lctrl_joy_eval("JOY0:X:dz=0.05", 0, -32768, &v) == 1 && v < -0.99f;
            ok = ok && lctrl_joy_eval("JOY0:X:dz=0.05:inv=1", 0, 32767, &v) == 1 && v < -0.99f;
            ok = ok && lctrl_joy_eval("JOY0:RZ:half=1:inv=1", 1, -32768, &v) == 1 && v > 0.99f;
            ok = ok && lctrl_joy_eval("JOY0:Z+", 0, 32767, &v) == 2 && v > 0.5f;
            ok = ok && lctrl_joy_eval("JOY0:Z+:inv=1", 0, 32767, &v) == 2 && v < 0.5f;
            ok = ok && lctrl_joy_eval("JOY0:Z+:inv=1", 0, -32768, &v) == 2 && v > 0.5f;
            ok = ok && lctrl_joy_eval("JOY0:Z:zone=0-10:inv=1", 0, 32767, &v) == 2 && v > 0.5f;
            ok = ok && lctrl_joy_eval("JOY0:B3", 0, 0, &v) == 0;
            if (!ok) {
                strcpy_s(why, sizeof why, "lctrl_joy_eval");
            }
        }
        /* 6. inv an Achse-als-Taste setzen, lesbar markiert, Hinweise */
        if (!why[0]) {
            char a[96] = "JOY0:Z+";
            lctrl_joy_param_set(a, sizeof a, "inv", "1");
            if (strcmp(a, "JOY0:Z+:inv=1") != 0 ||
                lctrl_joy_src_axis(a, NULL, NULL) != 2 ||
                lctrl_joy_src_axis("JOY0:Z++JOY0:B1", NULL, NULL) != 0 ||
                lctrl_joy_src_axis("JOY1:RZ:dz=0.1", NULL, NULL) != 1 ||
                !lctrl_joy_atom_text("JOY0:X:inv=1", t, sizeof t) ||
                !strstr(t, LT("gui.joy_src_inv")) ||
                !lctrl_joy_atom_text("JOY0:X", t, sizeof t) ||
                strstr(t, LT("gui.joy_src_inv")) ||
                !lctrl_joy_dir_hint(rot) || !lctrl_joy_dir_hint(rp) ||
                strcmp(lctrl_joy_dir_hint(rp), LT("gui.joy_hint_dig")) != 0 ||
                lctrl_joy_dir_hint(b0i) != NULL ||
                lctrl_joy_dir_expect(ay) != -1 || lctrl_joy_dir_expect(pr) != 1) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "inv/Text/Hinweis: '%s' '%s'", a, t);
            }
        }
        if (!why[0]) {
            T_OK("W226 JOY-AXIS-DIR: Lern-Richtung/inv, Richtung statt Zone, Basis einschwingen, Vorschau");
        } else {
            T_FAIL("W226 JOY-AXIS-DIR", "%s", why);
        }
    }

    /* W228 BIND-DIALOG: Gruppen der Liste, Sichtbarkeit im Detailbereich je
     * Aktionsart (Maus X/Y, Joystick, Hinweis), Quelle einzeln entfernen,
     * Konflikte nur der gewaehlten Aktion. Reine Logik, ohne Fenster. */
    {
        static lctrl_bind_t b;
        lctrl_conf1_t cf[LCTRL_ALT_N];
        char why[240] = "";
        int cnt[LCTRL_GROUP_N] = { 0 }, i, n;
        int bi0 = lctrl_bind_elem_index("bi0"), bi1 = lctrl_bind_elem_index("bi1");
        int bi2 = lctrl_bind_elem_index("bi2"), ax = lctrl_bind_elem_index("aim_x");
        int ay = lctrl_bind_elem_index("aim_y"), rot = lctrl_bind_elem_index("rot");
        int rp = lctrl_bind_elem_index("rot.pos"), pr = lctrl_bind_elem_index("pedal_r");
        int g1 = lctrl_bind_elem_index("gear.1"), tu = lctrl_bind_elem_index("tuner.up");
        int pa = lctrl_bind_elem_index("pause"), ph = lctrl_bind_elem_index("photo");
        unsigned v;
        for (i = 0; i < LCTRL_ELEM_N; i++) {
            int g = lctrl_elem_group(i);
            if (g < 0 || g >= LCTRL_GROUP_N) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "Gruppe %s = %d", kCtrlElems[i].key, g);
                break;
            }
            cnt[g]++;
        }
        if (!why[0] && (lctrl_elem_group(bi0) != 0 || lctrl_elem_group(rot) != 1 ||
                        lctrl_elem_group(rp) != 2 || lctrl_elem_group(g1) != 3 ||
                        lctrl_elem_group(tu) != 4 || lctrl_elem_group(pa) != 5 ||
                        lctrl_elem_group(ph) != 5 || cnt[0] != 39 || cnt[1] != 8 ||
                        cnt[2] != 10 || cnt[3] != 9 || cnt[4] != 2 ||
                        lctrl_elem_group(-1) != -1 || lctrl_elem_group(LCTRL_ELEM_N) != -1)) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "Gruppen %d/%d/%d/%d/%d/%d", cnt[0],
                        cnt[1], cnt[2], cnt[3], cnt[4], cnt[5]);
        }
        if (!why[0] && (lctrl_helper_axis(rp) != rot ||
                        lctrl_helper_axis(lctrl_bind_elem_index("aim_x.neg")) != ax ||
                        lctrl_helper_axis(bi0) != -1 || lctrl_helper_axis(rot) != -1)) {
            strcpy_s(why, sizeof why, "Achse zum Tasten-Helfer");
        }
        /* Sichtbarkeit je Aktionsart */
        lctrl_bind_init(&b);
        lctrl_bind_set(&b, bi0, "KEY:SPACE, MOUSE:LEFT, PAD:A");
        lctrl_bind_set(&b, ax, "MOUSE:X:mode=aim");
        lctrl_bind_set(&b, pr, "KEY:W, JOY0:Z:dz=0.05:inv=1");
        lctrl_bind_set(&b, rp, "KEY:D, JOY0:Z+");
        lctrl_bind_set(&b, ay, "MOUSE:Y:mode=aim, JOY0:Y:dz=0.10");
        if (!why[0] && (v = lctrl_bind_detail_vis(&b, bi0)) != 0) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "Taste: vis=%u (Maus/Joystick sichtbar)", v);
        }
        if (!why[0] && (v = lctrl_bind_detail_vis(&b, ax)) !=
                           (LCTRL_VIS_MOUSE | LCTRL_VIS_JOYHINT)) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "aim_x ohne JOY: vis=%u", v);
        }
        if (!why[0] && (v = lctrl_bind_detail_vis(&b, rot)) != LCTRL_VIS_JOYHINT) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "rot leer: vis=%u (Maus nur aim/sight)", v);
        }
        if (!why[0] && (v = lctrl_bind_detail_vis(&b, pr)) !=
                           (LCTRL_VIS_JOY | LCTRL_VIS_JOYDZ)) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "pedal_r mit JOY: vis=%u", v);
        }
        if (!why[0] && (v = lctrl_bind_detail_vis(&b, rp)) != LCTRL_VIS_JOY) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "rot.pos mit JOY0:Z+: vis=%u", v);
        }
        if (!why[0] && (v = lctrl_bind_detail_vis(&b, ay)) !=
                           (LCTRL_VIS_MOUSE | LCTRL_VIS_JOY | LCTRL_VIS_JOYDZ |
                            LCTRL_VIS_SMODE)) { /* W271: Stick-Modus (JOY-Achse auf aim_y) */
            _snprintf_s(why, sizeof why, _TRUNCATE, "aim_y Maus+JOY: vis=%u", v);
        }
        if (!why[0] && (lctrl_bind_detail_vis(&b, pa) != 0 ||
                        lctrl_bind_detail_vis(&b, -1) != 0 ||
                        lctrl_bind_detail_vis(NULL, bi0) != 0)) {
            strcpy_s(why, sizeof why, "Client-Aktion/ungueltig: vis != 0");
        }
        /* Konflikte nur der gewaehlten Aktion (Gross/klein egal) */
        lctrl_bind_set(&b, bi1, "key:space");
        n = lctrl_bind_conflicts_for(&b, bi0, cf, LCTRL_ALT_N);
        if (!why[0] && (n != 1 || cf[0].alt != 0 || cf[0].n != 1 || cf[0].other[0] != bi1)) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "Konflikte bi0: n=%d", n);
        }
        if (!why[0] && lctrl_bind_conflicts_for(&b, bi2, cf, LCTRL_ALT_N) != 0) {
            strcpy_s(why, sizeof why, "Konflikte bi2 (unbelegt) != 0");
        }
        /* Quelle einzeln entfernen: Rest rueckt auf, Grenzen */
        if (!why[0] && (lctrl_bind_remove_alt(&b, bi0, 1) != 0 || b.row[bi0].n_alt != 2 ||
                        strcmp(b.row[bi0].alt[0], "KEY:SPACE") != 0 ||
                        strcmp(b.row[bi0].alt[1], "PAD:A") != 0)) {
            strcpy_s(why, sizeof why, "Entfernen MOUSE:LEFT (Mitte)");
        }
        if (!why[0] && (lctrl_bind_remove_alt(&b, bi0, 2) != 1 ||
                        lctrl_bind_remove_alt(&b, bi0, -1) != 1 ||
                        lctrl_bind_remove_alt(NULL, bi0, 0) != 1)) {
            strcpy_s(why, sizeof why, "Entfernen ausserhalb nicht abgelehnt");
        }
        if (!why[0] && (lctrl_bind_remove_alt(&b, pr, 1) != 0 ||
                        lctrl_bind_detail_vis(&b, pr) != LCTRL_VIS_JOYHINT)) {
            strcpy_s(why, sizeof why, "JOY-Quelle entfernt: Hinweis statt Joystick");
        }
        if (!why[0] && (lctrl_bind_remove_alt(&b, bi0, 0) != 0 ||
                        lctrl_bind_remove_alt(&b, bi0, 0) != 0 || b.row[bi0].n_alt != 0 ||
                        lctrl_bind_remove_alt(&b, bi0, 0) != 1)) {
            strcpy_s(why, sizeof why, "Entfernen bis leer");
        }
        if (!why[0] && lctrl_bind_conflicts_for(&b, bi1, cf, LCTRL_ALT_N) != 0) {
            strcpy_s(why, sizeof why, "Konflikt nach Entfernen nicht weg");
        }
        if (!why[0]) {
            T_OK("W228 BIND-DIALOG: Gruppen, Sichtbarkeit Maus/Joystick je Art, Quelle einzeln entfernen, Konflikte je Aktion");
        } else {
            T_FAIL("W228 BIND-DIALOG", "%s", why);
        }
    }

    { /* W231 US-LAUNCHER: Einordnung, Saveordner, Startparameter, Exit-Schluessel,
       * Spielertexte EN/DE (konstruierte Pruefergebnisse, keine Spielbytes) */
        sb_gamever_t gv;
        lcfg_gameinfo_t gi;
        char why[200] = "", o[1600], m[1200], keep[16];
        int li;
        memset(&gv, 0, sizeof gv);
        gv.xbe_ok = 1; gv.title_id = 0x43430002u; gv.region = 4u; gv.supported = 1;
        lcfg_gameinfo_from(&gv, &gi);
        if (gi.edition != LCFG_ED_EU || !gi.startable || gi.needs_allow) strcpy_s(why, sizeof why, "EU");
        gv.supported = 0; gv.region = 1u; gv.ntsc_u = 1;
        lcfg_gameinfo_from(&gv, &gi);
        if (gi.edition != LCFG_ED_US || !gi.startable || !gi.needs_allow) strcpy_s(why, sizeof why, "US exp");
        gv.supported = 1; gv.ntsc_u = 0; /* spaeter: Client fuehrt US als supported */
        lcfg_gameinfo_from(&gv, &gi);
        if (gi.edition != LCFG_ED_US || !gi.startable || gi.needs_allow) strcpy_s(why, sizeof why, "US supported");
        gv.supported = 0;
        lcfg_gameinfo_from(&gv, &gi);
        if (gi.edition != LCFG_ED_US_OTHER || gi.startable) strcpy_s(why, sizeof why, "US fremd");
        gv.region = 2u;
        lcfg_gameinfo_from(&gv, &gi);
        if (gi.edition != LCFG_ED_OTHER || gi.startable) strcpy_s(why, sizeof why, "Japan");
        gv.region = 4u; gv.title_id = 0x12345678u;
        lcfg_gameinfo_from(&gv, &gi);
        if (gi.edition != LCFG_ED_OTHER || gi.startable) strcpy_s(why, sizeof why, "fremdes Spiel");
        memset(&gv, 0, sizeof gv);
        lcfg_gameinfo_from(&gv, &gi);
        if (gi.edition != LCFG_ED_NONE || gi.startable) strcpy_s(why, sizeof why, "leer");
        /* Saveordner */
        lcfg_hdd_for_edition("C:\\D\\hdd", LCFG_ED_US, o, sizeof o);
        if (strcmp(o, "C:\\D\\hdd\\us") != 0) strcpy_s(why, sizeof why, "hdd us");
        lcfg_hdd_for_edition("C:\\D\\hdd\\", LCFG_ED_US, o, sizeof o);
        if (strcmp(o, "C:\\D\\hdd\\us") != 0) strcpy_s(why, sizeof why, "hdd us Trenner");
        lcfg_hdd_for_edition("C:\\D\\hdd", LCFG_ED_EU, o, sizeof o);
        if (strcmp(o, "C:\\D\\hdd") != 0) strcpy_s(why, sizeof why, "hdd eu");
        /* Startparameter */
        memset(&gi, 0, sizeof gi);
        gi.edition = LCFG_ED_US; gi.startable = 1; gi.needs_allow = 1;
        strcpy_s(o, sizeof o, "vr_enable=0");
        if (lcfg_edition_set_args(&gi, "C:\\a;b\\hdd\\us", o, sizeof o) != 0 ||
            strcmp(o, "vr_enable=0;allow_ntsc_u=1;hdd_root_us=C:\\a\\;b\\hdd\\us") != 0)
            strcpy_s(why, sizeof why, "args us");
        gi.needs_allow = 0;
        strcpy_s(o, sizeof o, "vr_enable=0");
        if (lcfg_edition_set_args(&gi, "C:\\h\\us", o, sizeof o) != 0 ||
            strcmp(o, "vr_enable=0;hdd_root_us=C:\\h\\us") != 0)
            strcpy_s(why, sizeof why, "args us supported (ohne allow)");
        gi.edition = LCFG_ED_EU;
        strcpy_s(o, sizeof o, "vr_enable=0");
        if (lcfg_edition_set_args(&gi, "C:\\h\\us", o, sizeof o) != 0 || strcmp(o, "vr_enable=0") != 0)
            strcpy_s(why, sizeof why, "args eu");
        gi.edition = LCFG_ED_US; gi.needs_allow = 1;
        strcpy_s(o, 24, "vr_enable=0");
        if (lcfg_edition_set_args(&gi, "C:\\h\\us", o, 24) != 1 || strcmp(o, "vr_enable=0") != 0)
            strcpy_s(why, sizeof why, "args zu lang");
        /* Exit-Schluessel */
        if (strcmp(lcfg_exit_msg_key(5, LCFG_ED_EU) ? lcfg_exit_msg_key(5, LCFG_ED_EU) : "", "gui.exit5") ||
            strcmp(lcfg_exit_msg_key(5, LCFG_ED_US) ? lcfg_exit_msg_key(5, LCFG_ED_US) : "", "gui.exit5_us") ||
            strcmp(lcfg_exit_msg_key(8, LCFG_ED_US) ? lcfg_exit_msg_key(8, LCFG_ED_US) : "", "gui.exit8") ||
            strcmp(lcfg_exit_msg_key(39, LCFG_ED_EU) ? lcfg_exit_msg_key(39, LCFG_ED_EU) : "", "gui.exit39") || /* W339 */
            lcfg_exit_msg_key(0, LCFG_ED_US) || lcfg_exit_msg_key(1, LCFG_ED_EU) ||
            lcfg_exit_msg_key(4, LCFG_ED_US))
            strcpy_s(why, sizeof why, "exit-Schluessel");
        /* Spielertexte EN/DE */
        strcpy_s(keep, sizeof keep, sb_lang_code());
        for (li = 0; li < 2 && !why[0]; li++) {
            sb_lang_set(li ? "de" : "en");
            memset(&gv, 0, sizeof gv);
            gv.xbe_ok = 1; gv.title_id = 0x43430002u; gv.region = 1u; gv.ntsc_u = 1; gv.version = 7;
            strcpy_s(gv.label, sizeof gv.label, "Steel Battalion (USA, NTSC-U)");
            if (!lcfg_gamever_msg(&gv, m, sizeof m) ||
                !strstr(m, "Steel Battalion (USA, NTSC-U)") ||
                !strstr(m, li ? "experimentell" : "experimental"))
                strcpy_s(why, sizeof why, li ? "Text US de" : "Text US en");
            gv.ntsc_u = 0;
            if (lcfg_gamever_msg(&gv, m, sizeof m) || !strstr(m, li ? "Diese US-Fassung" : "This US copy") ||
                !strstr(m, "NTSC-U") || !strstr(m, "43430002"))
                strcpy_s(why, sizeof why, "Text US fremd");
            gv.region = 2u;
            if (lcfg_gamever_msg(&gv, m, sizeof m) ||
                !strstr(m, li ? "nicht unterst" : "not supported") || !strstr(m, "NTSC-J") ||
                !strstr(m, "Steel Battalion (Europe, PAL)") || !strstr(m, "Steel Battalion (USA, NTSC-U)"))
                strcpy_s(why, sizeof why, "Text Japan");
        }
        sb_lang_set(keep);
        { /* optional: echtes US-Abbild (nur lokal, Pfad aus der Umgebung, nie geloggt) */
            char us[1024];
            DWORD n = GetEnvironmentVariableA("SB_TEST_US_IMAGE", us, sizeof us);
            if (!why[0] && n > 0 && n < sizeof us) {
                launcher_settings_t su;
                lcfg_defaults(&su);
                su.xbe_path[0] = su.media_root[0] = '\0';
                strcpy_s(su.game_image, sizeof su.game_image, us);
                if (lcfg_image_check(us, m, sizeof m) != 0 || !lcfg_game_info(&su, &gi) ||
                    gi.edition != LCFG_ED_US || !gi.needs_allow)
                    strcpy_s(why, sizeof why, "echtes US-Abbild nicht erkannt");
                else
                    printf("     (W231: echtes US-Abbild erkannt: %s)\n", m);
            }
        }
        if (!why[0]) {
            T_OK("W231 US-LAUNCHER: Einordnung EU/US/US-fremd/Japan, Saveordner us, allow_ntsc_u nur solange experimental, Exit 5/8/39, Texte EN/DE");
        } else {
            T_FAIL("W231 US-LAUNCHER", "%s", why);
        }
    }

    /* 20-25 (W82): UTF-8-Pfade, cfg-ANSI, Oberflaeche, Hintergrundbild,
     * Launcher-Einstellung, Vorschau/Offscreen (launcher_gui.c). */
    gui_selftest(client_override, &pass, &fail);
    /* 42 (W171 LAUNCHER-P2P3): Regelwerk R1-R30 als reine Logik. */
    lrules_selftest(&pass, &fail);
    /* 30-37 (W148): Packs-Seite + Render-Skalierung (launcher_packtest.c). */
    packs_selftest(&pass, &fail);
    lupd_selftest(&pass, &fail); /* W250 UPDATE-CHECK (ohne Netz) */
    lgame_selftest(&pass, &fail); /* W276 LOC-LAUNCH (Spielauswahl, LoC-Abbild, Startsperre) */
    lacc_selftest(&pass, &fail); /* W328 STEELNET-ACCOUNT (Krypto, Pin, DPAPI, Discord-Rueckleitung, Kind-Umgebung) */
#if defined(SB_HAVE_LOCTP) && SB_HAVE_LOCTP
    loctp_selftest(&pass, &fail); /* W336 LOC-TESTPARTNER (Profile, Ketten, Auswertung, Start/Stopp mit Attrappe) */
#endif
    lorig_selftest(&pass, &fail); /* W278 ORIG-FLAG (Einordnung, Hinweis, Back to original, 4:3, Einmal-Start) */
    g_llay_pass = g_llay_fail = 0; /* W281 LAUNCHER-SIZE: Layout 0/a/b (rein) */
    llay_selftest(llay_rep);
    pass += g_llay_pass;
    fail += g_llay_fail;
    { /* W250: user.cfg nur mit der Update-Antwort = weiter Erststart */
        char d[1100], uc[1200];
        int k1 = -1, k2 = -1;
        launcher_settings_t s1;
        u8_GetTempPath(d, sizeof d);
        strcat_s(d, sizeof d, "w250_firstrun");
        lcfg_mkdir_p(d);
        _snprintf_s(uc, sizeof uc, _TRUNCATE, "%s/user.cfg", d);
        u8_DeleteFile(uc);
        if (lupd_cfg_set(uc, 0) == 0) {
            app_load_effective(d, &s1, NULL, 0, &k1, "de");
        }
        if ((f = NULL, u8_fopen_s(&f, uc, "a")) == 0 && f) {
            fputs("width=1280\n", f);
            fclose(f);
            app_load_effective(d, &s1, NULL, 0, &k2, "de");
        }
        u8_DeleteFile(uc);
        if (k1 == 0 && k2 == 1 && s1.width == 1280) {
            T_OK("W250 update: user.cfg nur mit Update-Antwort bleibt Erststart, mit weiteren Werten nicht");
        } else {
            T_FAIL("W250 update Erststart", "kind %d/%d", k1, k2);
        }
        sb_lang_set("de");
    }

    la_selftest(&pass, &fail); /* W272 ALPHA-TEST: VT-Liste, Pruefung, Kette, Testordner, A/B */
    alpha_gui_selftest(client_override, &pass, &fail); /* W272: Seite Alpha Test */
    ld_selftest(&pass, &fail); /* W279 DIAG-CRASH: Erkennung, einmalig, Issue-Link, Paket, Bereinigung */
    dg_gui_selftest(client_override, &pass, &fail); /* W279: Absturz-Karte */
    w282_gui_selftest(client_override, &pass, &fail); /* W282 ISSUE6 */
    w283_gui_selftest(client_override, &pass, &fail); /* W283 ORIG-KEY-BIND */
    w290_gui_selftest(client_override, &pass, &fail); /* W290 QUICK-SHOT */
    w303_gui_selftest(client_override, &pass, &fail); /* W303 AUDIO-VOLUME */
    w299_bind_selftest(&pass, &fail); /* W299 REPLAY */
    w365_gui_selftest(client_override, &pass, &fail); /* W365 BIND-COMBO */
    w432_gui_selftest(client_override, &pass, &fail); /* W432 HOTAS-AXES: Belegungsdialog */
    w419_gui_selftest(client_override, &pass, &fail); /* W419 CTRL-EXPORT */
    w438_gui_selftest(client_override, &pass, &fail); /* W438 CTRL-RESET-040 */
    w430_selftest(&pass, &fail); /* W430 CTRL-IMPORT-FUZZ (kurz) */
    if (!ls_replay_selftest()) { T_OK("W254 replay list (folders, SaveMeta name, mission, signature state, order, set args)"); }
    else { T_FAIL("W254 replay list", "%s", "failed"); }
    if (!ls_selftest()) { T_OK("W199 save-import layouts, conflicts, backup, ZIP roundtrip, rejection"); }
    else { T_FAIL("W199 save-import", "%s", "failed"); }
    { /* W332 SEC: Funde aus dem Fuzzing der Launcher-Parser (tests/fuzz/proj_launcher) - frueher Absturz
       * (strcpy_s-Abbruch) bzw. Lesen/Schreiben hinter den Puffer */
        /* static: cli_selftest hat schon einen grossen Rahmen; mehr Stack laesst die GUI-Pruefung (Fenster-
         * Callbacks) ueberlaufen (0xC000041D) */
        static char longv[3000], ln[3200], d[1100], uc[1200];
        static launcher_settings_t s1;
        static lgame_state_t g1;
        static lctrl_bind_t b1;
        char why[160] = "", u8o[64];
        size_t k;
        memset(longv, 'A', sizeof longv - 1);
        longv[sizeof longv - 1] = '\0';
        u8_GetTempPath(d, sizeof d);
        strcat_s(d, sizeof d, "w332_fuzz");
        lcfg_mkdir_p(d);
        _snprintf_s(uc, sizeof uc, _TRUNCATE, "%s/user.cfg", d);
        /* a) user.cfg / Profil: Pfadwert 600 Zeichen (Feld 512) wird verworfen, Rest gilt */
        if ((f = NULL, u8_fopen_s(&f, uc, "w")) == 0 && f) {
            fprintf(f, "xbe_path=%.600s\nwidth=1280\nlauncher_loc_image=%.1000s\n", longv, longv);
            fclose(f);
        }
        lcfg_defaults(&s1);
        if (lcfg_load_file(uc, &s1) != 0 || s1.width != 1280 || strlen(s1.xbe_path) >= 512)
            strcpy_s(why, sizeof why, "user.cfg langer Pfad");
        if (!why[0] && (lgame_cfg_load(uc, &g1) != 0 || g1.loc_image[0]))
            strcpy_s(why, sizeof why, "LoC-Abbild langer Pfad");
        u8_from_acp(longv, u8o, sizeof u8o);
        if (!why[0] && u8o[0])
            strcpy_s(why, sizeof why, "u8_from_acp zu lang");
        /* b) lcfg_save_user: leere alte Datei, und alte Datei, deren verwaltete Zeilen beim Ersetzen stark wachsen */
        if (!why[0]) {
            if ((f = NULL, u8_fopen_s(&f, uc, "w")) == 0 && f) {
                fclose(f);
            }
            lcfg_defaults(&s1);
            if (lcfg_save_user(uc, &s1) != 0 || !st_file_has(uc, "width="))
                strcpy_s(why, sizeof why, "save_user leere Datei");
        }
        if (!why[0]) {
            lcfg_defaults(&s1);
            memset(s1.xbe_path, 'x', 500);
            s1.xbe_path[500] = '\0';
            strcpy_s(s1.media_root, sizeof s1.media_root, s1.xbe_path);
            strcpy_s(s1.hdd_root, sizeof s1.hdd_root, s1.xbe_path);
            strcpy_s(s1.game_image, sizeof s1.game_image, s1.xbe_path);
            memset(s1.photo_dir, 'p', 390);
            s1.photo_dir[390] = '\0';
            if ((f = NULL, u8_fopen_s(&f, uc, "w")) == 0 && f) {
                fputs("xbe_path=\nmedia_root=\nhdd_root=\ngame_image=\n", f);
                fclose(f);
            }
            /* vorher: Reserve fest 4 KB -> letzte Schluessel ([ui] lang) abgeschnitten/verstuemmelt */
            if (lcfg_save_user(uc, &s1) != 0 || !st_file_has(uc, "\nlang=") ||
                !st_file_has(uc, "game_image=xxxxxxxxxx"))
                strcpy_s(why, sizeof why, "save_user Wachstum");
        }
        u8_DeleteFile(uc);
        /* c) Eingabe-Profil: Alternative > 95 Zeichen wird verworfen, kurze bleibt */
        if (!why[0]) {
            _snprintf_s(ln, sizeof ln, _TRUNCATE, "photo = KEY:%.200s, KEY:F11\n", longv);
            lctrl_bind_load_text(ln, &b1);
            k = (size_t)lctrl_bind_elem_index("photo");
            if (k >= LCTRL_ELEM_N || b1.row[k].n_alt != 1 || strcmp(b1.row[k].alt[0], "KEY:F11") != 0)
                strcpy_s(why, sizeof why, "Profil lange Alternative");
        }
        {
            wchar_t w1[1200];
            u8_to_w(d, w1, 1200);
            RemoveDirectoryW(w1);
        }
        if (!why[0]) {
            T_OK("W332 fuzz findings: overlong cfg path values/profile alternatives ignored (no strcpy_s abort), lcfg_save_user empty file + growth, u8_from_acp bound");
        } else {
            T_FAIL("W332 fuzz findings", "%s", why);
        }
    }
    st_valid_cleanup(); /* COMM-SELFTEST2 */
    /* W455 UNICODE-PATHS: Prozess mit UTF-8-Codepage (Manifest); Ordner mit Kyrillisch, Japanisch und Akzent
     * per A-API und CRT anlegen/schreiben/umbenennen/loeschen, cfg-Rundlauf in UTF-8, alte ANSI-cfg lesbar,
     * Umgebung (SB_USERDATA_ROOT-Weg) verlustfrei. Nur im Temp-Ordner. */
    {
#define W455_DIR "\xD0\x98" "\xD0\xB3" "\xD0\xBE" "\xD1\x80" "\xD1\x8C" " " "\xE3\x83\x86" "\xE3\x82\xB9" "\xE3\x83\x88" " " "\xC3\xA9"
        static char base[1200], dir[1400], p1[1500], p2[1500], cfgp[1500], raw[8192], ev[1500];
        static wchar_t wd[1400], we[1500];
        const char *why = NULL;
        size_t n = 0;
        FILE *fh = NULL;
        u8_GetTempPath(base, sizeof base);
        _snprintf_s(dir, sizeof dir, _TRUNCATE, "%ssb_w455_%lu", base, GetCurrentProcessId());
        CreateDirectoryA(dir, NULL);
        _snprintf_s(dir, sizeof dir, _TRUNCATE, "%ssb_w455_%lu\\" W455_DIR, base, GetCurrentProcessId());
        if (!u8_acp_is_utf8()) {
            why = "GetACP != 65001 (Manifest activeCodePage fehlt?)";
        } else if (!CreateDirectoryA(dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
            why = "CreateDirectoryA (UTF-8) scheitert";
        } else if (u8_to_w(dir, wd, 1400) <= 0 || !(GetFileAttributesW(wd) & FILE_ATTRIBUTE_DIRECTORY) ||
                   GetFileAttributesW(wd) == INVALID_FILE_ATTRIBUTES) {
            why = "Ordner per W-API nicht unter dem Unicode-Namen";
        }
        if (!why) { /* CRT (fopen, schmal): Replay-artige Datei schreiben, lesen, umbenennen, loeschen */
            _snprintf_s(p1, sizeof p1, _TRUNCATE, "%s\\replay_\xC3\xA9.bin", dir);
            _snprintf_s(p2, sizeof p2, _TRUNCATE, "%s\\replay_\xD0\x98.bin", dir);
            if (fopen_s(&fh, p1, "wb") != 0 || !fh) {
                why = "fopen(w) im Unicode-Ordner";
            } else {
                fwrite("W455", 1, 4, fh);
                fclose(fh);
                fh = NULL;
                memset(raw, 0, 8);
                if (fopen_s(&fh, p1, "rb") != 0 || !fh) {
                    why = "fopen(r)";
                } else {
                    n = fread(raw, 1, 8, fh);
                    fclose(fh);
                    if (n != 4 || memcmp(raw, "W455", 4) != 0) why = "Inhalt";
                    else if (!MoveFileA(p1, p2)) why = "MoveFileA (umbenennen)";
                    else if (!DeleteFileA(p2) || GetFileAttributesA(p2) != INVALID_FILE_ATTRIBUTES) why = "DeleteFileA";
                }
            }
        }
        if (!why) { /* cfg-Rundlauf: Pfade UTF-8 in der Datei, gleich zurueck */
            _snprintf_s(cfgp, sizeof cfgp, _TRUNCATE, "%s\\user.cfg", dir);
            lcfg_defaults(&a);
            _snprintf_s(a.hdd_root, sizeof a.hdd_root, _TRUNCATE, "%s\\HDD", dir);
            _snprintf_s(a.game_image, sizeof a.game_image, _TRUNCATE, "%s\\game.iso", dir);
            memset(raw, 0, sizeof raw);
            if (lcfg_save_user(cfgp, &a) != 0) {
                why = "lcfg_save_user";
            } else if (u8_fopen_s(&fh, cfgp, "rb") != 0 || !fh) {
                why = "cfg nicht lesbar";
            } else {
                n = fread(raw, 1, sizeof raw - 1, fh);
                fclose(fh);
                lcfg_defaults(&b);
                lcfg_load_file(cfgp, &b);
                if (!strstr(raw, W455_DIR)) why = "cfg enthaelt den Pfad nicht als UTF-8";
                else if (strcmp(a.hdd_root, b.hdd_root) != 0 || strcmp(a.game_image, b.game_image) != 0)
                    why = "cfg-Rundlauf ungleich";
            }
        }
        if (!why) { /* alte cfg aus der ANSI-Zeit: Bytes der System-Codepage -> UTF-8 */
            static const char old1252[] = "hdd_root=w455_J\xF6rg_HDD\n";
            if (u8_fopen_s(&fh, cfgp, "wb") != 0 || !fh) {
                why = "alte cfg schreiben";
            } else {
                fwrite(old1252, 1, sizeof old1252 - 1, fh);
                fclose(fh);
                lcfg_defaults(&b);
                lcfg_load_file(cfgp, &b);
                if (u8_legacy_acp() == 1252 && strcmp(b.hdd_root, "w455_J\xC3\xB6rg_HDD") != 0) why = "alte ANSI-cfg falsch gelesen";
                else if (!u8_valid(b.hdd_root)) why = "alte ANSI-cfg ergibt ungueltiges UTF-8";
            }
        }
        if (!why) { /* Umgebung (Launcher -> Client, z. B. SB_USERDATA_ROOT): A setzen, W lesen */
            SetEnvironmentVariableA("SB_W455_TEST", dir);
            if (GetEnvironmentVariableW(L"SB_W455_TEST", we, 1500) == 0 || wcscmp(we, wd) != 0) why = "Umgebung A->W";
            else if (GetEnvironmentVariableA("SB_W455_TEST", ev, sizeof ev) == 0 || strcmp(ev, dir) != 0) why = "Umgebung W->A";
            SetEnvironmentVariableA("SB_W455_TEST", NULL);
        }
        if (cfgp[0]) u8_DeleteFile(cfgp);
        RemoveDirectoryA(dir);
        _snprintf_s(dir, sizeof dir, _TRUNCATE, "%ssb_w455_%lu", base, GetCurrentProcessId());
        RemoveDirectoryA(dir);
        printf("     (W455: Codepage %u, Alt-Codepage %u)\n", GetACP(), u8_legacy_acp());
        if (!why) {
            T_OK("W455 unicode paths (UTF-8 codepage, Cyrillic/Japanese/accent folder: create/write/read/rename/delete, cfg UTF-8 roundtrip, legacy ANSI cfg, env)");
        } else {
            T_FAIL("W455 unicode paths", "%s", why);
        }
#undef W455_DIR
    }
    /* W414: Stapel-Reserve des Hauptthreads. Vorher lag die Spitze bei ~1008 von 1024 KB (cli_selftest
     * hatte 13 lctrl_bind_t a ~31 KB auf dem Stapel, Rahmen 640 KB), kleine Zusaetze liefen in C00000FD.
     * Spitze = festgeschriebener Stapel (TIB StackBase - StackLimit); Grenze 75 % der PE-Reserve,
     * damit Zuwachs als FAIL auffaellt statt als Absturz. Stapelgroesse bleibt Standard (kein /STACK). */
    {
        NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
        const IMAGE_DOS_HEADER *dh = (const IMAGE_DOS_HEADER *)GetModuleHandleW(NULL);
        const IMAGE_NT_HEADERS *nh = (const IMAGE_NT_HEADERS *)((const char *)dh + dh->e_lfanew);
        unsigned long peak = (unsigned long)(((char *)tib->StackBase - (char *)tib->StackLimit) / 1024);
        unsigned long resv = (unsigned long)(nh->OptionalHeader.SizeOfStackReserve / 1024);
        printf("     (W414: Stapel-Spitze %lu KB von %lu KB Reserve)\n", peak, resv);
        if (resv && peak * 4 <= resv * 3) {
            T_OK("W414 stack headroom (main thread peak <= 75 % of the default reserve, big test buffers static)");
        } else {
            T_FAIL("W414 stack headroom", "Spitze %lu KB von %lu KB", peak, resv);
        }
    }
    printf("selftest: %d PASS, %d FAIL\n", pass, fail);
    return fail ? 1 : 0;
#undef T_OK
#undef T_FAIL
}
#if SB_EDITION_COMMUNITY
#undef lcfg_defaults
#endif

/* --write-user-profile: Test-Hook + CLI-Weg zum Belegungs-Dialog.
 * Basis laden, --set-bind "elem=alt,alt;..." anwenden, als
 * profiles/user_<REST>.ini speichern. 0 = ok. */
static int cli_write_user_profile(const char *client_override, const char *rest,
                                  const char *base, const char *setbind,
                                  int force)
{
    char client[1024], fn[72], pname[68], dir[1100], path[1200], err[1024];
    lctrl_bind_t b;
    int has_ldr;
    lcfg_find_client_dir(client_override, client, sizeof client, &has_ldr);
    if (!has_ldr) {
        printf("FEHLER: kein sb_ldr.exe in '%s' (Client-Ordner?).\n", client);
        return 1;
    }
    if (!lctrl_user_rest_ok(rest)) {
        printf("FEHLER: Name ung" U8_UE "ltig (1-40: Buchstaben, Ziffern, _ - .).\n");
        return 1;
    }
    lctrl_user_filename(rest, fn, sizeof fn);
    strcpy_s(pname, sizeof pname, fn);
    pname[strlen(pname) - 4] = '\0'; /* ".ini" ab */
    if (lctrl_name_shipped(pname)) {
        printf("FEHLER: Standardprofile werden nie " U8_UE "berschrieben.\n");
        return 1;
    }
    lctrl_bind_init(&b);
    if (base && *base) {
        char bp[1200];
        if (lctrl_input_profile_path(client, base, bp, sizeof bp) != 0 ||
            lctrl_bind_load_file(bp, &b) != 0) {
            printf("FEHLER: Basis-Profil '%s' nicht lesbar.\n", base);
            return 1;
        }
    }
    if (setbind && *setbind) {
        static char work[4096];
        char *one, *ctx = NULL;
        strcpy_s(work, sizeof work, setbind);
        for (one = strtok_s(work, ";", &ctx); one;
             one = strtok_s(NULL, ";", &ctx)) {
            char *eq = strchr(one, '=');
            int idx;
            if (!eq) {
                printf("FEHLER: --set-bind braucht elem=wert ('%s').\n", one);
                return 1;
            }
            *eq = '\0';
            idx = lctrl_bind_elem_index(one);
            if (idx < 0) {
                printf("FEHLER: unbekanntes Element '%s'.\n", one);
                return 1;
            }
            lctrl_bind_set(&b, idx, eq + 1);
        }
    }
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s/profiles", client);
    lcfg_mkdir_p(dir);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s/%s", dir, fn);
    if (!force && u8_GetFileAttributes(path) != INVALID_FILE_ATTRIBUTES) {
        printf("FEHLER: '%s' existiert (mit --force " U8_UE "berschreiben).\n",
               path);
        return 1;
    }
    if (lctrl_bind_save(path, &b, base ? base : "", err, sizeof err) != 0) {
        printf("FEHLER: %s\n", err);
        return 1;
    }
    printf("ok: %s gespeichert (Basis '%s').\n", path,
           base ? base : "(leer)");
    return 0;
}

static int cli_check(const char *client_override, const char *profile)
{
    launcher_settings_t s;
    char client[1024], err[1024], dflt[64];
    int has_ldr, n;
    char names[32][64];
    lcfg_find_client_dir(client_override, client, sizeof client, &has_ldr);
    if (profile) {
        if (lcfg_profile_load(client, profile, &s, err, sizeof err) != 0) {
            printf("FEHLER: %s\n", err);
            return 1;
        }
        printf("profil: %s\n", profile);
    } else {
        char ui[128];
        load_effective(client, &s, ui, sizeof ui, NULL, NULL);
        printf("user.cfg: %s\n", ui);
    }
    printf("client: %s (sb_ldr.exe %s)\n", client,
           has_ldr ? "gefunden" : "FEHLT");
    printf("xbe_path=%s\nmedia_root=%s\nhdd_root=%s\n", s.xbe_path,
           s.media_root, s.hdd_root);
    printf("width=%u height=%u present_interval=%d fps_limit=%d audio=%d "
           "audio_live=%d aspect_mode=%d\n",
           s.width, s.height, s.present_interval, s.fps_limit, s.audio,
           s.audio_live, s.aspect_mode);
    printf("input_profile=%s sbc_usb=%d interp_fps=%d\n",
           s.input_profile[0] ? s.input_profile : "(Legacy)",
           s.sbc_usb, s.interp_fps);
    n = lcfg_profile_list(client, names, 32);
    if (lcfg_profile_get_default(client, dflt, sizeof dflt) != 0) {
        strcpy_s(dflt, sizeof dflt, "(keines)");
    }
    printf("profile: %d, standard: %s\n", n, dflt);
    if (lcfg_validate(&s, err, sizeof err) != 0) {
        printf("FEHLER: %s\n", err);
        return 1;
    }
    printf("ok: Einstellungen g" U8_UE "ltig.\n");
    return 0;
}

static int cli_save_profile(const char *client_override, const char *name,
                            const char *base_profile, const char *set)
{
    launcher_settings_t s;
    char client[1024], err[1024];
    int has_ldr;
    lcfg_find_client_dir(client_override, client, sizeof client, &has_ldr);
    if (!has_ldr) {
        printf("FEHLER: kein sb_ldr.exe in '%s' (Client-Ordner?).\n", client);
        return 1;
    }
    if (base_profile) {
        if (lcfg_profile_load(client, base_profile, &s, err, sizeof err) !=
            0) {
            printf("FEHLER: %s\n", err);
            return 1;
        }
    } else {
        load_effective(client, &s, NULL, 0, NULL, NULL);
    }
    if (set) {
        lcfg_apply_set(&s, set);
    }
    if (lcfg_validate(&s, err, sizeof err) != 0) {
        printf("FEHLER: %s\n", err);
        return 1;
    }
    if (lcfg_profile_save(client, name, &s, err, sizeof err) != 0) {
        printf("FEHLER: %s\n", err);
        return 1;
    }
    printf("ok: Profil '%s' gespeichert (%ux%u, fps=%d, aspect=%d).\n", name,
           s.width, s.height, s.fps_limit, s.aspect_mode);
    return 0;
}

static int cli_list_profiles(const char *client_override)
{
    char client[1024], dflt[64];
    char names[64][64];
    int has_ldr, n, i;
    lcfg_find_client_dir(client_override, client, sizeof client, &has_ldr);
    n = lcfg_profile_list(client, names, 64);
    if (lcfg_profile_get_default(client, dflt, sizeof dflt) != 0) {
        dflt[0] = '\0';
    }
    printf("client: %s\nprofile: %d\n", client, n);
    for (i = 0; i < n; i++) {
        printf("  %s%s\n", names[i],
               strcmp(names[i], dflt) == 0 ? " (Standard)" : "");
    }
    if (dflt[0]) {
        printf("standard: %s\n", dflt);
    }
    return 0;
}

/* Schreibt user.cfg aus (Profil|user.cfg-Basis) + --set. 0 = ok. */
/* W419 CTRL-EXPORT: --controls-export OUT (wie der Knopf "Export...": aktives Profil + Steuerungswerte) bzw.
 * --controls-check DATEI (nur pruefen, nichts aendern). 0 = ok. */
static int cli_controls(const char *client_override, const char *exp, const char *chk)
{
    static lcx_data_t d;
    static launcher_settings_t s;
    char client[1024], upath[1200], pp[1200], err[400];
    int has_ldr, i, ui = 0;
    if (chk) {
        int rc = lcx_read_file(chk, &d, err, sizeof err);
        if (rc != LCX_OK) {
            printf("FEHLER (%d): %s\n", rc, err);
            return 1;
        }
        for (i = 0; i < d.n_set; i++) {
            char k[48];
            const char *eq = strchr(d.set[i], '=');
            if (eq && (size_t)(eq - d.set[i]) < sizeof k) {
                memcpy(k, d.set[i], (size_t)(eq - d.set[i]));
                k[eq - d.set[i]] = '\0';
                ui += lcx_set_is_ui(k);
            }
        }
        printf("OK: format %d, game %s, profile %s, %d settings (%d page, %d user.cfg), %d unknown ignored\n",
               d.format, d.game[0] ? d.game : "-", d.profile, d.n_set, ui, d.n_set - ui, d.unknown);
        return 0;
    }
    lcfg_find_client_dir(client_override, client, sizeof client, &has_ldr);
    if (!has_ldr) {
        printf("FEHLER: kein sb_ldr.exe in '%s' (Client-Ordner?).\n", client);
        return 1;
    }
    load_effective(client, &s, NULL, 0, NULL, NULL);
    memset(&d, 0, sizeof d);
    strcpy_s(d.profile, sizeof d.profile, s.input_profile[0] ? s.input_profile : "keyboard_default");
    if (lctrl_input_profile_path(client, d.profile, pp, sizeof pp) != 0 || lctrl_bind_load_file(pp, &d.bind) != 0) {
        printf("FEHLER: Profil '%s' nicht gefunden.\n", d.profile);
        return 1;
    }
    d.format = LCX_FORMAT;
    strcpy_s(d.launcher, sizeof d.launcher, lupd_version()[0] ? lupd_version() : "dev");
    strcpy_s(d.game, sizeof d.game, "sb");
    lcx_put_settings(&d, &s);
    lcfg_user_cfg_path(client, upath, sizeof upath);
    lcx_put_cfg(&d, upath);
    if (lcx_write_file(exp, &d) != 0) {
        printf("FEHLER: Datei nicht schreibbar.\n");
        return 1;
    }
    printf("OK: profile %s, %d settings exportiert.\n", d.profile, d.n_set);
    return 0;
}

static int cli_write_user_cfg(const char *client_override,
                              const char *profile, const char *set)
{
    launcher_settings_t s;
    char client[1024], upath[1200], err[1024];
    int has_ldr;
    lcfg_find_client_dir(client_override, client, sizeof client, &has_ldr);
    if (!has_ldr) {
        printf("FEHLER: kein sb_ldr.exe in '%s' (Client-Ordner?).\n", client);
        return 1;
    }
    if (profile) {
        if (lcfg_profile_load(client, profile, &s, err, sizeof err) != 0) {
            printf("FEHLER: %s\n", err);
            return 1;
        }
    } else {
        load_effective(client, &s, NULL, 0, NULL, NULL);
    }
    if (set) {
        lcfg_apply_set(&s, set);
    }
    if (lcfg_validate(&s, err, sizeof err) != 0) {
        printf("FEHLER: %s\n", err);
        return 1;
    }
    lcfg_user_cfg_path(client, upath, sizeof upath); /* W150 USER-DATA */
    if (lcfg_save_user(upath, &s) != 0) {
        printf("FEHLER: user.cfg nicht schreibbar: %s\n", upath);
        return 1;
    }
    printf("ok: %s geschrieben (%ux%u, fps=%d, audio=%d/%d, aspect=%d)%s.\n",
           upath, s.width, s.height, s.fps_limit, s.audio, s.audio_live,
           s.aspect_mode, profile ? " (aus Profil)" : "");
    return 0;
}

/* W159: Start-Kommandozeile bauen (rein, testbar). set_args = --set-Kette
 * des Startwegs (Bildschirm: "vr_enable=0", VR: "vr_enable=1;vr_preview=..")
 * oder NULL/leer = ohne --set. Das --xbe-Argument liest der Client als ANSI
 * (argv): darum dieselbe ANSI-/Kurzname-Form wie in user.cfg (W82). */
void app_client_cmdline(const char *client_dir, const char *xbe_path,
                        const char *set_args, char *out, size_t cap)
{
    char exe[1200], acp[1024], xarg[1200];
    _snprintf_s(exe, sizeof exe, _TRUNCATE, "%s/sb_ldr.exe", client_dir);
    u8_to_acp(xbe_path, acp, sizeof acp);
    u8_from_acp(acp, xarg, sizeof xarg);
    /* Spieldaten-Pfade: user.cfg (media_root) + XBE zusaetzlich als
     * --xbe-Argument (Client-Flag, s. parse_cmdline in emu_main.c). */
    if (set_args && *set_args) {
        _snprintf_s(out, cap, _TRUNCATE, "\"%s\" --xbe \"%s\" --set \"%s\"",
                    exe, xarg, set_args);
    } else {
        _snprintf_s(out, cap, _TRUNCATE, "\"%s\" --xbe \"%s\"", exe, xarg);
    }
}

/* W159: --set-Kette je Startweg (rein, testbar). Bildschirm: VR immer aus,
 * egal was gespeichert ist. VR: an + Vorschau aus den Einstellungen. */
void app_vr_set_args(int vr, const char *vr_preview, char *out, size_t cap)
{
    if (!vr) {
        strcpy_s(out, cap, "vr_enable=0");
    } else {
        _snprintf_s(out, cap, _TRUNCATE, "vr_enable=1;vr_preview=%s",
                    (vr_preview && *vr_preview) ? vr_preview : "left");
    }
}

/* W159: OpenXR-Runtime vorhanden? 1 = ja (Manifest-Pfad in path_out),
 * 0 = nein. Liest NUR Registry ActiveRuntime wie der Client (vr.c,
 * 32-bit-Sicht) + Datei-Existenz (Attribute) - laedt nichts, aendert nichts.
 * Test-Haken: SB_TEST_XR_ABSENT=1 erzwingt "nein" (grauer VR-Knopf-Shot). */
int app_xr_runtime_present(char *path_out, size_t cap)
{
    HKEY k;
    DWORD type = 0, n;
    char json[1024], t[8];
    if (path_out && cap) {
        path_out[0] = '\0';
    }
    if (GetEnvironmentVariableA("SB_TEST_XR_ABSENT", t, sizeof t) > 0 &&
        t[0] == '1') {
        return 0;
    }
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Khronos\\OpenXR\\1", 0,
                      KEY_READ, &k) != ERROR_SUCCESS) {
        return 0;
    }
    n = sizeof json;
    if (RegQueryValueExA(k, "ActiveRuntime", NULL, &type, (LPBYTE)json, &n) !=
            ERROR_SUCCESS ||
        type != REG_SZ || !json[0]) {
        RegCloseKey(k);
        return 0;
    }
    RegCloseKey(k);
    /* Abgelaufener Eintrag (Manifest weg) zaehlt nicht. */
    if (GetFileAttributesA(json) == INVALID_FILE_ATTRIBUTES) {
        return 0;
    }
    if (path_out && cap) {
        strncpy_s(path_out, cap, json, cap - 1);
    }
    return 1;
}

/* Client starten (CWD = Client-Ordner). 0 = ok, 1 = Fehler (Text in err).
 * W82: W-API (Pfade UTF-8). W159: set_args = --set-Kette des Startwegs
 * (Einzelstart-Override, user.cfg bleibt unveraendert). */
int app_launch_client_proc(const char *client_dir, const char *xbe_path,
                           const char *set_args, char *err, size_t errcap,
                           void **proc)
{
    char cmd[3600];
    unsigned long pid = 0;
    app_client_cmdline(client_dir, xbe_path, set_args, cmd, sizeof cmd);
    if (proc) {
        *proc = NULL;
    }
    if (!u8_CreateProcess(cmd, client_dir, 0, 0, NULL, 0, &pid, proc)) {
        _snprintf_s(err, errcap, _TRUNCATE, LT("app.launch_fail"), GetLastError());
        return 1;
    }
    _snprintf_s(err, errcap, _TRUNCATE, LT("app.launched"), pid);
    return 0;
}

/* W328: Start mit eigenem Umgebungsblock (SB_LOC_AUTH nur im Kindprozess). */
int app_launch_client_env(const char *client_dir, const char *xbe_path, const char *set_args,
                          const void *env_w, char *err, size_t errcap)
{
    char cmd[3600];
    unsigned long pid = 0;
    app_client_cmdline(client_dir, xbe_path, set_args, cmd, sizeof cmd);
    if (!u8_CreateProcessEnv(cmd, client_dir, 0, 0, NULL, 0, env_w, &pid, NULL)) {
        _snprintf_s(err, errcap, _TRUNCATE, LT("app.launch_fail"), GetLastError());
        return 1;
    }
    _snprintf_s(err, errcap, _TRUNCATE, LT("app.launched"), pid);
    return 0;
}

int app_launch_client_set(const char *client_dir, const char *xbe_path,
                          const char *set_args, char *err, size_t errcap)
{
    return app_launch_client_proc(client_dir, xbe_path, set_args, err, errcap, NULL);
}

/* W172/P7 + W231: Spielstand-Ordner wie der Client (hcfg.c: "HDD" im
 * Profil-/Portabel-Modus = <Datenordner>\hdd, sonst relativ zum Client). */
void app_hdd_base(const char *client_dir, const char *hdd_root, char *out, size_t cap)
{
    const char *h = hdd_root ? hdd_root : "";
    char p[1024];
    if (sb_userdata_mode() != SB_UD_LEGACY && strcmp(h, "HDD") == 0 &&
        sb_userdata_path_peek(SB_UD_HDD, p, sizeof p) == 1) {
        u8_from_acp(p, out, cap);
    } else if ((h[0] && h[1] == ':') || h[0] == '\\' || h[0] == '/') {
        strcpy_s(out, cap, h);
    } else {
        _snprintf_s(out, cap, _TRUNCATE, "%s\\%s", client_dir, h[0] ? h : "HDD");
    }
}

/* W231 US-LAUNCHER: --set-Zusatz der Spielfassung an set_args haengen
 * (US: allow_ntsc_u=1 solange experimental + getrennter Saveordner).
 * gi = Fassung (lcfg_game_info). 0 = ok, 1 = Fehler (Text in err). */
int app_edition_start_args(const char *client_dir, const launcher_settings_t *s,
                           const lcfg_gameinfo_t *gi, char *set_args, size_t cap,
                           char *err, size_t errcap)
{
    char base[1100], hdd[1200], acp[1200];
    if (!gi || gi->edition != LCFG_ED_US) {
        return 0;
    }
    app_hdd_base(client_dir, s->hdd_root, base, sizeof base);
    lcfg_hdd_for_edition(base, gi->edition, hdd, sizeof hdd);
    if (u8_to_acp(hdd, acp, sizeof acp) == 2 ||
        lcfg_edition_set_args(gi, acp, set_args, cap) != 0) {
        _snprintf_s(err, errcap, _TRUNCATE, LT("cfg.ansi"), LT("cfg.what_hdd"), hdd);
        return 1;
    }
    return 0;
}

int app_launch_client(const char *client_dir, const char *xbe_path,
                      char *err, size_t errcap)
{
    return app_launch_client_set(client_dir, xbe_path, NULL, err, errcap);
}

/* --start [--vr]: user.cfg schreiben + Client starten (mit --dry-run: nur
 * schreiben, nicht starten; die Start-Kommandozeile wird gezeigt). W159:
 * Bildschirm: --set vr_enable=0 (VR immer aus); --vr: vr_enable=1 +
 * Vorschau. 0 = ok. */
static const la_sel_t *g_cli_alpha; /* W272: --alpha-mission (NULL = normaler Start) */
static int cli_start(const char *client_override, const char *profile,
                     const char *set, int dry_run, int vr, int wait,
                     const char *replay, int replay_cam)
{
    launcher_settings_t s;
    lcfg_gameinfo_t gi;
    void *proc = NULL;
    char client[1024], upath[1200], err[1400], set_args[1600], cmd[3600];
    int has_ldr;
    lcfg_find_client_dir(client_override, client, sizeof client, &has_ldr);
    if (!has_ldr) {
        printf("FEHLER: kein sb_ldr.exe in '%s' (Client-Ordner?).\n", client);
        return 1;
    }
    if (profile) {
        if (lcfg_profile_load(client, profile, &s, err, sizeof err) != 0) {
            printf("FEHLER: %s\n", err);
            return 1;
        }
    } else {
        load_effective(client, &s, NULL, 0, NULL, NULL);
    }
    if (set) {
        lcfg_apply_set(&s, set);
    }
    if (lcfg_validate(&s, err, sizeof err) != 0) {
        printf("FEHLER: %s\n", err);
        return 1;
    }
    if (g_cli_alpha && la_cli_check(&s, g_cli_alpha) != 0) { /* W272: ungueltig -> nichts schreiben */
        return 3;
    }
    lcfg_user_cfg_path(client, upath, sizeof upath); /* W150 USER-DATA */
    if (lcfg_save_user(upath, &s) != 0) {
        printf("FEHLER: user.cfg nicht schreibbar: %s\n", upath);
        return 1;
    }
    /* W79: Spielerbetrieb (Replay/Turbo/Auto-Shots aus, echte Eingabe an). */
    if (lcfg_write_player_block(upath) != 0) {
        printf("FEHLER: Spielerbetrieb-Block nicht schreibbar: %s\n", upath);
        return 1;
    }
    printf("ok: %s geschrieben (%ux%u, fps=%d, aspect=%d, Spielerbetrieb).\n",
           upath, s.width, s.height, s.fps_limit, s.aspect_mode);
    app_vr_set_args(vr, s.vr_preview, set_args, sizeof set_args);
    lcfg_game_info(&s, &gi); /* W231 US-LAUNCHER */
    if (app_edition_start_args(client, &s, &gi, set_args, sizeof set_args, err,
                               sizeof err) != 0) {
        printf("FEHLER: %s\n", err);
        return 1;
    }
    printf("edition: %s%s\n", gi.edition == LCFG_ED_US ? "US (NTSC-U)" : "EU (PAL)",
           gi.needs_allow ? ", experimental (allow_ntsc_u=1)" : "");
    if (replay && *replay) { /* W254 REPLAY-LAUNCHER: wie "Play replay" im Fenster */
        char base[1100], hdd[1200], ra[200];
        wchar_t wh[1200], wid[40];
        app_hdd_base(client, s.hdd_root, base, sizeof base);
        lcfg_hdd_for_edition(base, gi.edition, hdd, sizeof hdd);
        u8_to_w(hdd, wh, 1200);
        u8_to_w(replay, wid, 40);
        if (strlen(replay) > 32 || !ls_replay_exists(wh, wid)) {
            printf("FEHLER: Replay '%s' nicht gefunden (UDATA/43430002/<id>/st00_0.rep).\n", replay);
            return 1;
        }
        ls_replay_set_args(wid, replay_cam, ra, sizeof ra);
        strcat_s(set_args, sizeof set_args, ";");
        strcat_s(set_args, sizeof set_args, ra);
        printf("replay: %s (experimentell)\n", ra);
    }
    if (g_cli_alpha) { /* W272 ALPHA-TEST: Testordner + Kette hinter der Fassung */
        int ar = la_cli_apply(client, &s, g_cli_alpha, dry_run, set_args, sizeof set_args);
        if (ar != 0) {
            return ar;
        }
    }
    {
        char xbe[1024]; /* W172: im Ordner-Modus abgeleitet */
        lcfg_xbe_effective(&s, xbe, sizeof xbe);
        strcpy_s(s.xbe_path, sizeof s.xbe_path, xbe);
    }
    app_client_cmdline(client, s.xbe_path, set_args, cmd, sizeof cmd);
    if (dry_run) {
        printf("dry-run: Client nicht gestartet (--dry-run).\n");
        printf("cmdline: %s\n", cmd);
        return 0;
    }
    if (app_launch_client_proc(client, s.xbe_path, set_args, err,
                               sizeof err, wait ? &proc : NULL) != 0) {
        printf("FEHLER: %s\n", err);
        return 1;
    }
    printf("%s\n", err);
    if (wait && proc) { /* W231: auf den Client warten, Exitcode deuten */
        DWORD code = 0;
        const char *k;
        WaitForSingleObject((HANDLE)proc, INFINITE);
        GetExitCodeProcess((HANDLE)proc, &code);
        CloseHandle((HANDLE)proc);
        k = lcfg_exit_msg_key(code, gi.edition);
        printf("client exit=%lu%s%s\n", (unsigned long)code, k ? ": " : "", k ? LT(k) : "");
        return code == 0 ? 0 : 2;
    }
    return 0;
}

/* Kleine argv-Aufteilung (mit "..."-Gruppen), ohne Shell-API. */
static char **split_args(const char *cmd, int *argc_out)
{
    static char buf[8192];
    static char *argv[256];
    int argc = 0;
    const char *p = cmd;
    char *w = buf;
    strcpy_s(buf, sizeof buf, cmd);
    p = buf;
    while (*p && argc < 255) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (!*p) {
            break;
        }
        argv[argc++] = (char *)p;
        w = (char *)p;
        if (*p == '"') {
            p++;
            argv[argc - 1] = (char *)p;
            w = (char *)p;
            while (*p && *p != '"') {
                *w++ = *p++;
            }
            if (*p) {
                p++;
            }
        } else {
            while (*p && *p != ' ' && *p != '\t') {
                *w++ = *p++;
            }
        }
        if (*p) {
            p++;
        }
        *w = '\0';
    }
    argv[argc] = NULL;
    *argc_out = argc;
    return argv;
}

static void shot_dir(int argc, char **argv, gui_opts_t *o)
{
    const char *out = cli_value(argc, argv, "--shot-out");
    if (out && *out) {
        strcpy_s(o->out_dir, sizeof o->out_dir, out);
    } else if (u8_GetEnv("TEMP", o->out_dir, sizeof o->out_dir) == 0 ||
               !o->out_dir[0]) {
        strcpy_s(o->out_dir, sizeof o->out_dir, ".");
    }
}

/* W142: fehlender Sprachschluessel (sollte nie passieren: lang_check). */
static void app_lang_warn(const char *key)
{
    char b[160];
    _snprintf_s(b, sizeof b, _TRUNCATE, "sb_launcher: Sprachschluessel fehlt: %s\n",
                key ? key : "?");
    OutputDebugStringA(b);
}

int WINAPI WinMain(HINSTANCE h, HINSTANCE p, LPSTR cmd, int show)
{
    int argc = 0, rc;
    char **argv;
    const char *client_override, *profile;
    gui_opts_t o;
    static char u8cmd[8192];
    (void)h;
    (void)p;
    (void)cmd;
    (void)show;
    /* W332 SEC: DLLs nur aus System32 (keine untergeschobenen DLLs aus dem
     * Programm-/Arbeitsordner, z. B. Downloads). Statische Importe deckt der
     * Linker-Schalter /DEPENDENTLOADFLAG:0x800 ab (CMakeLists.txt). */
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    SetDllDirectoryW(L"");
    SetSearchPathMode(BASE_SEARCH_PATH_ENABLE_SAFE_SEARCHMODE | BASE_SEARCH_PATH_PERMANENT);
    dpi_init();
    /* W82: Kommandozeile als UTF-16 holen (Pfade mit Umlauten). */
    w_to_u8(GetCommandLineW(), u8cmd, (int)sizeof u8cmd);
    argv = split_args(u8cmd, &argc);
    if (!argv || argc < 1) {
        return 1;
    }
    memset(&o, 0, sizeof o);
    o.page = -1;
    o.bg = -1;
    o.bind_sel = -1; /* W159: --bind-shot=vr waehlt das VR-Element */
    {
        const char *d = cli_value(argc, argv, "--dpi");
        if (d) {
            o.force_dpi = atoi(d);
            if (o.force_dpi < 96 || o.force_dpi > 384) {
                o.force_dpi = 0;
            }
        }
    }
    { /* W309 FPS-MONITOR: simulierte Monitore fuer --ui-shot/--selftest (erster = Startmonitor) */
        const char *sm = cli_value(argc, argv, "--sim-monitors");
        if (sm) { strncpy_s(o.sim_mon, sizeof o.sim_mon, sm, _TRUNCATE); }
    }
    { /* W281 LAUNCHER-SIZE: Layout-Vorschau (Entwurf) - GUI und Shots */
        const char *lv = cli_value(argc, argv, "--layout");
        const char *sz = cli_value(argc, argv, "--ui-size");
        const char *wk = cli_value(argc, argv, "--work");
        if (lv) {
            if (llay_parse(lv) < 0) {
                cli_console();
                printf("FEHLER: --layout braucht 0|a|b.\n");
                return 1;
            }
            strncpy_s(o.layout, sizeof o.layout, lv, _TRUNCATE);
        }
        if (sz) {
            if (llay_size_parse(sz) < 0) {
                cli_console();
                printf("FEHLER: --ui-size braucht compact|normal|large.\n");
                return 1;
            }
            strncpy_s(o.ui_size, sizeof o.ui_size, sz, _TRUNCATE);
        }
        gui_layout_override(o.layout, o.ui_size);
        if (wk && sscanf_s(wk, "%dx%d", &o.lay_work_w, &o.lay_work_h) != 2) {
            o.lay_work_w = o.lay_work_h = 0;
        }
    }
    { /* W142: Sprache (lang/ neben der Exe; Code folgt aus cfg/--lang). */
        const char *lg = cli_value(argc, argv, "--lang");
        char exe[1024], ldir[1150];
        if (lg && *lg) {
            strncpy_s(o.lang, sizeof o.lang, lg, sizeof o.lang - 1);
        }
        if (u8_ExeDir(exe, sizeof exe)) {
            _snprintf_s(ldir, sizeof ldir, _TRUNCATE, "%s/lang", exe);
            sb_lang_init(ldir, "en");
        } else {
            sb_lang_init(NULL, "en");
        }
        sb_lang_set_warn(app_lang_warn);
    }
    { /* W150 USER-DATA: Modus an den Client vererben (gleiche Datenorte) */
        char ev[32];
        DWORD en = GetEnvironmentVariableA("SB_USERDATA", ev, sizeof ev);
        if (en == 0 || en >= sizeof ev) {
            SetEnvironmentVariableA("SB_USERDATA", sb_userdata_mode_name(sb_userdata_mode()));
        }
    }
    client_override = cli_value(argc, argv, "--client-dir");
    profile = cli_value(argc, argv, "--profile");
    if (cli_flag(argc, argv, "--help") || cli_flag(argc, argv, "-h") ||
        cli_flag(argc, argv, "/?")) {
        cli_console();
        cli_usage();
        rc = 0;
    } else if (cli_flag(argc, argv, "--loctp-dummy")) { /* W336: Attrappe eines Clients (Selbsttest Testpartner) */
#if defined(SB_HAVE_LOCTP) && SB_HAVE_LOCTP
        rc = loctp_dummy_main();
#else
        rc = 2;
#endif
    } else if (cli_flag(argc, argv, "--loctp-run")) { /* W336: Testpartner-Durchlauf ohne Fenster (Abnahme) */
        cli_console();
#if defined(SB_HAVE_LOCTP) && SB_HAVE_LOCTP
        rc = loctp_cli(client_override, argc, argv);
#else
        printf("loctp: in dieser Ausgabe nicht enthalten\n");
        rc = 2;
#endif
    } else if (cli_flag(argc, argv, "--selftest")) {
        cli_console();
        rc = cli_selftest(client_override);
    } else if (cli_flag(argc, argv, "--steelnet")) { /* W328: Pilotenkonto ohne Fenster (Durchspieltest) */
        int ai;
        cli_console();
        for (ai = 1; ai < argc && strcmp(argv[ai], "--steelnet") != 0; ai++) {
        }
        rc = lacc_cli(client_override, argc - ai - 1, argv + ai + 1);
    } else if (cli_flag(argc, argv, "--update-check")) { /* W250: ein echter Abruf (Beleg) */
        const char *as = cli_value(argc, argv, "--as-version");
        lupd_res_t r;
        cli_console();
        lupd_fetch(as ? as : lupd_version(), &r);
        printf("Anfrage-Kopfzeilen:\n%s", r.req_headers);
        printf("update: eigene Version %s, HTTP %d, %u Bytes, neuestes Release %s%s -> %s%s%s\n",
               as ? as : (lupd_version()[0] ? lupd_version() : "(keine)"), r.http_status,
               (unsigned)r.bytes, r.rel.tag[0] ? r.rel.tag : "-", r.rel.prerelease ? " (Prerelease)" : "",
               r.result == LUPD_R_NEWER ? "NEUER" : r.result == LUPD_R_CURRENT ? "aktuell"
               : r.result == LUPD_R_NOVER ? "keine Release-Version" : r.result == LUPD_R_HTTP ? "HTTP-Fehler"
               : r.result == LUPD_R_NET ? "kein Netz" : "unlesbar",
               r.rel.url[0] ? ", Seite " : "", r.rel.url[0] ? (lupd_url_ok(r.rel.url) ? r.rel.url : "(abgelehnt)") : "");
        rc = r.result == LUPD_R_NEWER || r.result == LUPD_R_CURRENT ? 0 : 1;
    } else if (cli_value(argc, argv, "--check-image")) { /* W150 ISO-DIRECT */
        char m[1200];
        cli_console();
        rc = lcfg_image_check(cli_value(argc, argv, "--check-image"), m, sizeof m);
        printf("%s\n", m);
#if defined(SB_TEST_HOOKS) && SB_TEST_HOOKS
    } else if (cli_value(argc, argv, "--loc169-migrate")) { /* W414: Test-Haken (Entwickler-Build) - Einmal-Umstellung */
        int r414;
        cli_console();
        r414 = lgame_loc169_migrate(cli_value(argc, argv, "--loc169-migrate"));
        printf("loc169-migrate: %d (1 = umgestellt, 0 = nichts zu tun, -1 = Fehler)\n", r414);
        rc = r414 < 0 ? 1 : 0;
#endif
    } else if (cli_flag(argc, argv, "--diag-package")) { /* W279 DIAG-CRASH: Diagnosepaket ohne Fenster */
        cli_console();
        rc = gui_diag_package_cli(client_override);
    } else if (cli_flag(argc, argv, "--userdata")) { /* W150 USER-DATA */
        static const char *const kn[7] = { "root", "user.cfg", "hdd", "packs", "shadercache", "logs", "crash" };
        char p[1024];
        int k;
        cli_console();
        printf("Modus: %s (Quelle %s)\n", sb_userdata_mode_name(sb_userdata_mode()), sb_userdata_mode_source());
        for (k = 0; k < 7; k++) {
            if (sb_userdata_path_peek((sb_ud_kind_t)k, p, sizeof p) == 1) {
                printf("  %-12s %s\n", kn[k], p);
            }
        }
        rc = sb_userdata_selftest(p, sizeof p);
        printf("%s\n", p);
    } else if (cli_flag(argc, argv, "--check")) {
        cli_console();
        rc = cli_check(client_override, profile);
    } else if (cli_flag(argc, argv, "--list-profiles")) {
        cli_console();
        rc = cli_list_profiles(client_override);
    } else if (cli_value(argc, argv, "--save-profile")) {
        cli_console();
        rc = cli_save_profile(client_override,
                              cli_value(argc, argv, "--save-profile"), profile,
                              cli_value(argc, argv, "--set"));
    } else if (cli_flag(argc, argv, "--save-profile")) {
        cli_console();
        printf("FEHLER: --save-profile braucht einen Namen.\n");
        rc = 1;
    } else if (cli_flag(argc, argv, "--gui-start-dry")) {
        cli_console();
        o.shot = 4;
        { /* W272: Startweg "Start test" (Trockenlauf) */
            la_sel_t as;
            if (cli_alpha(argc, argv, &as)) {
                o.alpha_on = 1;
                o.alpha_mission = as.mission;
                o.alpha_vt = as.vt;
                o.alpha_diff = as.diff;
                o.alpha_skip = as.skip_intro;
            }
        }
        if (cli_value(argc, argv, "--replay")) { /* W254: Startweg "Play replay" (Trockenlauf) */
            strcpy_s(o.replay, sizeof o.replay, cli_value(argc, argv, "--replay"));
            o.replay_cam = cli_value(argc, argv, "--replay-camera")
                               ? atoi(cli_value(argc, argv, "--replay-camera")) : -1;
            if (cli_value(argc, argv, "--video")) { /* W255: Startweg "Export video" (Trockenlauf, Standardordner) */
                strcpy_s(o.video, sizeof o.video, cli_value(argc, argv, "--video"));
            }
        }
        rc = gui_main(client_override, &o);
    } else if (cli_flag(argc, argv, "--start") && cli_value(argc, argv, "--game") &&
               lgame_from_key(cli_value(argc, argv, "--game")) == LGAME_LOC) {
        /* W276 LOC-LAUNCH: Line of Contact startet der Client noch nicht ->
         * Meldung statt Startversuch (Exit 3); nichts wird geschrieben. */
        char m[600];
        cli_console();
        if (cli_value(argc, argv, "--lang")) {
            sb_lang_set(cli_value(argc, argv, "--lang"));
        }
        if (lgame_loc_start_gate(NULL, m, sizeof m) == 0) {
            strncpy_s(m, sizeof m, LT("gui.loc_start_noimg"), _TRUNCATE);
        }
        printf("%s\n", m);
        rc = 3;
    } else if (cli_flag(argc, argv, "--start")) {
        la_sel_t as; /* W272: --alpha-mission N [--alpha-vt V] [--alpha-diff D] [--alpha-skip-intro] */
        cli_console();
        g_cli_alpha = cli_alpha(argc, argv, &as) ? &as : NULL;
        rc = cli_start(client_override, profile, cli_value(argc, argv, "--set"),
                       cli_flag(argc, argv, "--dry-run"),
                       cli_flag(argc, argv, "--vr"),
                       cli_flag(argc, argv, "--wait"), /* W231 */
                       cli_value(argc, argv, "--replay"), /* W254 */
                       cli_value(argc, argv, "--replay-camera")
                           ? atoi(cli_value(argc, argv, "--replay-camera")) : -1);
    } else if (cli_value(argc, argv, "--write-user-profile")) {
        cli_console();
        rc = cli_write_user_profile(
            client_override, cli_value(argc, argv, "--write-user-profile"),
            cli_value(argc, argv, "--from"), cli_value(argc, argv, "--set-bind"),
            cli_flag(argc, argv, "--force"));
    } else if (cli_flag(argc, argv, "--controls-fuzz") || cli_value(argc, argv, "--controls-fuzz")) {
        char why[400];
        long st[2];
        const char *nv = cli_value(argc, argv, "--controls-fuzz"), *sv = cli_value(argc, argv, "--seed");
        long nn = nv && atol(nv) > 0 ? atol(nv) : 200000;
        unsigned sd = sv ? (unsigned)strtoul(sv, NULL, 0) : 0x430u;
        cli_console(); /* W430 CTRL-IMPORT-FUZZ */
        rc = w430_ctrl_fuzz(nn, sd, why, sizeof why, st);
        if (rc) { printf("FUND: %s\n", why); }
        else { printf("OK: %ld Mutationen (Seed %u): %ld Importe, %ld abgelehnt\n", nn, sd, st[0], st[1]); }
    } else if (cli_value(argc, argv, "--controls-export") || cli_value(argc, argv, "--controls-check")) {
        cli_console(); /* W419 CTRL-EXPORT */
        rc = cli_controls(client_override, cli_value(argc, argv, "--controls-export"),
                          cli_value(argc, argv, "--controls-check"));
    } else if (cli_flag(argc, argv, "--write-user-cfg")) {
        cli_console();
        rc = cli_write_user_cfg(client_override, profile,
                                cli_value(argc, argv, "--set"));
    } else if (cli_flag(argc, argv, "--bind-shot") ||
               cli_value(argc, argv, "--bind-shot")) {
        const char *bv = cli_value(argc, argv, "--bind-shot");
        cli_console();
        o.shot = 3;
        if (bv && strcmp(bv, "vr") == 0) { /* W159: VR-Gruppe in Sicht */
            o.bind_sel = lctrl_bind_elem_index("vr_recenter");
        } else if (bv && strcmp(bv, "joy") == 0) { /* W226: HOTAS-Beispiel */
            o.bind_sel = lctrl_bind_elem_index("pedal_r");
            o.bind_joydemo = 1;
        } else if (bv && strcmp(bv, "stick") == 0) { /* W271: Stick-Modus-Beispiel */
            o.bind_sel = lctrl_bind_elem_index("aim_x");
            o.bind_stickdemo = 1;
        } else if (bv && strcmp(bv, "wheel") == 0) { /* W260: Mausrad-Beispiel */
            o.bind_sel = lctrl_bind_elem_index("tuner.up");
            o.bind_wheeldemo = 1;
        } else if (bv && strcmp(bv, "combo") == 0) { /* W365: Kombination + Tastenname */
            o.bind_sel = lctrl_bind_elem_index("bi1");
            o.bind_combodemo = 1;
        } else if (bv) {
            o.bind_sel = lctrl_bind_elem_index(bv);
        }
        o.bind_learn = cli_flag(argc, argv, "--bind-learn"); /* W228 */
        shot_dir(argc, argv, &o);
        rc = gui_main(client_override, &o);
    } else if (cli_flag(argc, argv, "--joy-shot")) {
        /* W221 HOTAS: Joystick-Dialog offscreen (Beispielgeraete, nie sichtbar) */
        cli_console();
        o.shot = 6;
        o.joy_cal = cli_flag(argc, argv, "--joy-cal"); /* W312: Kalibrierzustand */
        shot_dir(argc, argv, &o);
        rc = gui_main(client_override, &o);
    } else if (cli_value(argc, argv, "--assist-shot")) {
        /* W143: Offscreen-Shot des Controller-Assistenten (nie sichtbar). */
        static const char *const modes[8] = { "live", "test", "result",
                                              "cal", "lamps", "gamepad",
                                              "nodev", "alle" };
        const char *v = cli_value(argc, argv, "--assist-shot");
        const char *fx = cli_value(argc, argv, "--fixture");
        int i, ok = 0;
        cli_console();
        for (i = 0; i < 8; i++) {
            if (strcmp(v, modes[i]) == 0) {
                ok = 1;
            }
        }
        if (!ok) {
            printf("FEHLER: --assist-shot braucht live|test|result|cal|"
                   "lamps|gamepad|nodev|alle.\n");
            return 1;
        }
        o.shot = 5;
        strcpy_s(o.assist_mode, sizeof o.assist_mode, v);
        if (fx && *fx) {
            strcpy_s(o.assist_fixture, sizeof o.assist_fixture, fx);
        }
        shot_dir(argc, argv, &o);
        rc = gui_main(client_override, &o);
    } else if (cli_value(argc, argv, "--ui-shot") ||
               cli_flag(argc, argv, "--ui-shot")) {
        /* W171 LAUNCHER-P2P3: 7 Seiten, alte Namen als Aliase */
        const char *v = cli_value(argc, argv, "--ui-shot");
        const char *bg = cli_value(argc, argv, "--bg");
        const char *st = cli_value(argc, argv, "--state");
        cli_console();
        o.shot = 2;
        if (v && strcmp(v, "alle") != 0) {
            o.page = gui_page_from_name(v);
            if (o.page < 0) {
                printf("FEHLER: --ui-shot braucht play|display|graphics|packs|"
                       "controls|audio|advanced|alpha|alle (alt: start|grafik|remaster|"
                       "ton|steuerung|profile|pfade).\n");
                return 1;
            }
        }
        if (st) {
            if (!gui_state_known(st)) {
                printf("FEHLER: --state braucht nodata|fullscreen|window43|ready|img_*|"
                       "controller|custom|diag.\n");
                return 1;
            }
            strncpy_s(o.state, sizeof o.state, st, _TRUNCATE);
        }
        if (bg) {
            if (strcmp(bg, "beide") == 0) {
                o.bg_both = 1;
            } else {
                o.bg = atoi(bg) ? 1 : 0;
            }
        }
        shot_dir(argc, argv, &o);
        rc = gui_main(client_override, &o);
    } else if (cli_value(argc, argv, "--shot") ||
               cli_flag(argc, argv, "--shot")) {
        const char *v = cli_value(argc, argv, "--shot");
        cli_console();
        o.shot = 1;
        if (v && strcmp(v, "alle") != 0) {
            o.page = atoi(v);
            if (o.page < 0 || o.page >= gui_page_count()) {
                printf("FEHLER: --shot braucht 0..7 oder 'alle'.\n");
                return 1;
            }
        }
        shot_dir(argc, argv, &o);
        rc = gui_main(client_override, &o);
    } else {
        rc = gui_main(client_override, &o);
    }
    return rc;
}
