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
#include "launcher_ctrl.h"
#include "launcher_u8.h"
#include "launcher_app.h"
#include "launcher_rules.h" /* W171 */
#include "sb_lang.h" /* W142 L10N */
#include "launcher_assist.h" /* W143 Assistent-cfg */
#include "sb_edition.h" /* W116 EDITIONS */
#include "sb_userdata.h" /* W150 USER-DATA */

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
           "  --check                 effektive Einstellungen + Validierung\n"
           "  --check-image PFAD      Spiel-Abbild pruefen (Version, lesbar)\n"
           "  --userdata              Datenorte (Modus, Pfade) + Selbsttest\n"
           "  --write-user-cfg        user.cfg schreiben (startet nicht)\n"
           "  --start                 user.cfg schreiben + Client starten\n"
           "  --dry-run               mit --start: nur schreiben, nicht starten\n"
           "  --vr                    mit --start: VR-Start (sonst Bildschirm)\n"
           "  --profile NAME          Profil statt user.cfg als Basis laden\n"
           "  --save-profile NAME     Profil aus Basis + --set speichern\n"
           "  --list-profiles         Profile + Standard-Profil auflisten\n"
           "  --set \"k=v;k=v\"         Einstellungen " U8_AE "ndern\n"
           "  --client-dir DIR        Client-Ordner (statt Suche)\n"
           "  --shot N|alle           Test: Seite N (0..6) als BMP sichern\n"
           "  --shot-out DIR          Ziel f" U8_UE "r --shot (Default: %%TEMP%%)\n"
           "  --write-user-profile R    Benutzerprofil user_R.ini schreiben\n"
           "                            (Basis --from B, --set-bind \"e=a;...\", --force)\n"
           "  --bind-shot[=vr]        Test: Belegungs-Dialog sichern (BMP+PNG)\n"
           "  --ui-shot SEITE|alle    Test: Seiten als PNG (play, display, graphics, packs,\n"
           "                          controls, audio, advanced; alte Namen start, grafik,\n"
           "                          remaster, ton, steuerung, profile, pfade gelten\n"
           "                          weiter); Fenster immer au" U8_SZ "erhalb des\n"
           "                          Bildschirms, ohne Fokus\n"
           "  --state NAME            mit --ui-shot: Regelzustand (nodata, fullscreen,\n"
           "                          window43, controller, custom, diag, pack2, pack3,\n"
           "                          effectpack),\n"
           "                          nur Anzeige (Packs-Ordner: Umgebung SB_PACKS_DIR)\n"
           "  --dpi N                 mit --ui-shot/--bind-shot: DPI erzwingen\n"
           "                          (144 = 150 %%)\n"
           "  --bg 0|1|beide          mit --ui-shot: Hintergrundbild\n"
           "  --lang en|de            Sprache (statt [ui] lang aus cfg)\n"
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
    }
    lcfg_user_cfg_path(client_dir, p, sizeof p); /* W150 USER-DATA */
    if (lcfg_load_file_count(p, s, &keys) == 0) {
        kind = 1;
    }
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
            strcpy_s(user_info, user_cap, LT("app.no_usercfg"));
        }
    }
    if (kind_out) {
        *kind_out = kind;
    }
}

#define load_effective app_load_effective

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
        lctrl_bind_t bd;
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
                lctrl_bind_t bd2;
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
            lctrl_bind_t bc;
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
        lcfg_gfx_apply(&a, LGP_STRONG);
        if (lcfg_gfx_preset(&a, 0) != LGP_STRONG || a.shx[LSHX_SHADING] != 1 ||
            a.shx[LSHX_AA] != 3 || a.shx[LSHX_SHADOW] != 1 || a.shx[LSHX_SOFT] != 1 ||
            a.shx[LSHX_FOG] != 100 || a.fx[LFX_SOFT] != 100 || a.fx[LFX_EMBERS] != 120 ||
            !a.hd_textures || a.shx[LSHX_LABEL] != 0) {
            ok = 0;
        }
        if (ok && lcfg_save_user(tmp, &a) == 0) {
            FILE *g = NULL;
            lcfg_defaults(&b);
            lcfg_load_file(tmp, &b);
            if (memcmp(&a, &b, sizeof a) != 0 || lcfg_gfx_preset(&b, 0) != LGP_STRONG) {
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
            if (lcfg_gfx_preset(&b, 0) != LGP_CUSTOM || lcfg_gfx_base(&b, 0) != LGP_STRONG) {
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
            if (lcfg_gfx_preset(&b, 0) != LGP_SUBTLE || b.shx[LSHX_SSAO] != 25 ||
                b.fx[LFX_SOFT] != 60 || b.shx[LSHX_SHADING] != 1) {
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
            lcfg_defaults(&a);
            lcfg_gfx_apply(&a, p);
            if (lcfg_gfx_preset(&a, 0) != p) {
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
                } else {
                    b.remaster = b.hd_textures = !b.hd_textures;
                }
                if (lcfg_gfx_preset(&b, 0) != LGP_CUSTOM || lcfg_gfx_base(&b, 0) != p) {
                    _snprintf_s(why, sizeof why, _TRUNCATE, "P%d: %s geaendert -> %d/Basis %d", p,
                                lcfg_gfx_key(k), lcfg_gfx_preset(&b, 0), lcfg_gfx_base(&b, 0));
                }
            }
            b = a; /* hud_label_avoid: eigene Option (PO 7.11.) */
            b.shx[LSHX_LABEL] = 1;
            if (!why[0] && lcfg_gfx_preset(&b, 0) != p) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "P%d: Etikett macht Custom", p);
            }
            lcfg_gfx_apply(&b, (p + 1) % SB_GP_NPRE);
            if (!why[0] && b.shx[LSHX_LABEL] != 1) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "P%d: Preset setzt Etikett", p);
            }
        }
        if (!why[0]) { /* Effect-Pack-Ausnahme */
            lgp_mask_t lock = 0;
            lcfg_defaults(&a);
            lcfg_gfx_apply(&a, LGP_SUBTLE);
            a.shx[LSHX_SAT] = 140;
            a.fx[LFX_EMBERS] = 200;
            p = lcfg_gfx_preset(&a, 0);
            lcfg_gfx_lock_key(&lock, "color_sat");
            k = lcfg_gfx_preset(&a, lock);
            lcfg_gfx_lock_key(&lock, "fx_embers");
            lcfg_gfx_lock_key(&lock, "unknown_key");
            if (p != LGP_CUSTOM || k != LGP_CUSTOM || lcfg_gfx_preset(&a, lock) != LGP_SUBTLE ||
                lcfg_fx_level(&a, lock) != LGP_SUBTLE || lcfg_fx_level(&a, 0) != LGP_CUSTOM) {
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
            T_OK("52 gfx-preset-p4 (4 Presets + Custom, Client-Tabelle, Effect Pack, Etikett)");
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
        if (b.interp_fps != 120 || b.rt_scale != 0 || !b.fs_interp_match) { ok = 0; }
        /* Legacy values remain valid and unchanged when loaded. */
        lcfg_defaults(&b);
        lcfg_apply_set(&b, "interp_fps=60;rt_scale=3;rt_scale_dyn=1");
        if (b.interp_fps != 60 || b.rt_scale != 3 || b.rt_scale_dyn != 1 ||
            !b.fs_interp_match) { ok = 0; }
        u8_DeleteFile(tmp);
        if (ok) { T_OK("W207 lowend (60 FPS, native RT, save/load, Custom, rules)"); }
        else { T_FAIL("W207 lowend", "%s", "values/persistence/rules"); }
    }

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
        lctrl_bind_t b1, b2;
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
        lctrl_bind_t b1, b2;
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
        lctrl_bind_t b1, b2;
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

    /* 20-25 (W82): UTF-8-Pfade, cfg-ANSI, Oberflaeche, Hintergrundbild,
     * Launcher-Einstellung, Vorschau/Offscreen (launcher_gui.c). */
    gui_selftest(client_override, &pass, &fail);
    /* 42 (W171 LAUNCHER-P2P3): Regelwerk R1-R30 als reine Logik. */
    lrules_selftest(&pass, &fail);
    /* 30-37 (W148): Packs-Seite + Render-Skalierung (launcher_packtest.c). */
    packs_selftest(&pass, &fail);

    if (!ls_selftest()) { T_OK("W199 save-import layouts, conflicts, backup, ZIP roundtrip, rejection"); }
    else { T_FAIL("W199 save-import", "%s", "failed"); }
    st_valid_cleanup(); /* COMM-SELFTEST2 */
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
int app_launch_client_set(const char *client_dir, const char *xbe_path,
                          const char *set_args, char *err, size_t errcap)
{
    char cmd[3600];
    unsigned long pid = 0;
    app_client_cmdline(client_dir, xbe_path, set_args, cmd, sizeof cmd);
    if (!u8_CreateProcess(cmd, client_dir, 0, 0, NULL, 0, &pid, NULL)) {
        _snprintf_s(err, errcap, _TRUNCATE, LT("app.launch_fail"), GetLastError());
        return 1;
    }
    _snprintf_s(err, errcap, _TRUNCATE, LT("app.launched"), pid);
    return 0;
}

int app_launch_client(const char *client_dir, const char *xbe_path,
                      char *err, size_t errcap)
{
    return app_launch_client_set(client_dir, xbe_path, NULL, err, errcap);
}

#define launch_client app_launch_client

/* --start [--vr]: user.cfg schreiben + Client starten (mit --dry-run: nur
 * schreiben, nicht starten; die Start-Kommandozeile wird gezeigt). W159:
 * Bildschirm: --set vr_enable=0 (VR immer aus); --vr: vr_enable=1 +
 * Vorschau. 0 = ok. */
static int cli_start(const char *client_override, const char *profile,
                     const char *set, int dry_run, int vr)
{
    launcher_settings_t s;
    char client[1024], upath[1200], err[1400], set_args[128], cmd[3600];
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
    /* W79: Spielerbetrieb (Replay/Turbo/Auto-Shots aus, echte Eingabe an). */
    if (lcfg_write_player_block(upath) != 0) {
        printf("FEHLER: Spielerbetrieb-Block nicht schreibbar: %s\n", upath);
        return 1;
    }
    printf("ok: %s geschrieben (%ux%u, fps=%d, aspect=%d, Spielerbetrieb).\n",
           upath, s.width, s.height, s.fps_limit, s.aspect_mode);
    app_vr_set_args(vr, s.vr_preview, set_args, sizeof set_args);
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
    if (app_launch_client_set(client, s.xbe_path, set_args, err,
                              sizeof err) != 0) {
        printf("FEHLER: %s\n", err);
        return 1;
    }
    printf("%s\n", err);
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
    } else if (cli_flag(argc, argv, "--selftest")) {
        cli_console();
        rc = cli_selftest(client_override);
    } else if (cli_value(argc, argv, "--check-image")) { /* W150 ISO-DIRECT */
        char m[1200];
        cli_console();
        rc = lcfg_image_check(cli_value(argc, argv, "--check-image"), m, sizeof m);
        printf("%s\n", m);
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
        rc = gui_main(client_override, &o);
    } else if (cli_flag(argc, argv, "--start")) {
        cli_console();
        rc = cli_start(client_override, profile, cli_value(argc, argv, "--set"),
                       cli_flag(argc, argv, "--dry-run"),
                       cli_flag(argc, argv, "--vr"));
    } else if (cli_value(argc, argv, "--write-user-profile")) {
        cli_console();
        rc = cli_write_user_profile(
            client_override, cli_value(argc, argv, "--write-user-profile"),
            cli_value(argc, argv, "--from"), cli_value(argc, argv, "--set-bind"),
            cli_flag(argc, argv, "--force"));
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
        }
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
                       "controls|audio|advanced|alle (alt: start|grafik|remaster|"
                       "ton|steuerung|profile|pfade).\n");
                return 1;
            }
        }
        if (st) {
            if (!gui_state_known(st)) {
                printf("FEHLER: --state braucht nodata|fullscreen|window43|"
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
                printf("FEHLER: --shot braucht 0..6 oder 'alle'.\n");
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
