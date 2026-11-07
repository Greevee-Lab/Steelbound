/* launcher_gui.c - Oberflaeche des Launchers (W82 LAUNCHER-UI2).
 * Randloses Fenster (WS_POPUP, eigene Titelleiste, DWM-Schatten),
 * Per-Monitor-DPI v2, W-API/UTF-16 an der Fenstergrenze (intern UTF-8),
 * eigene Zeichnung (dunkle Palette, halbtransparente Karten ueber einem
 * ausgeblichenen Hintergrundbild aus der Ressource, WIC). Funktionen wie
 * W26/W47/W78/W79: Grafik, Ton, Steuerung (Belegungs-Dialog), Profile,
 * Pfade, Start (user.cfg + Spielerbetrieb-Block + sb_ldr.exe).
 * Quelltext ASCII, Nutzertexte UTF-8 (Makros U8_* aus launcher_u8.h). */
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#define COBJMACROS
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <wincodec.h>
#include <math.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "launcher_app.h"
#include "launcher_cfg.h"
#include "launcher_ctrl.h"
#include "launcher_u8.h"
#include "sb_lang.h"
#include "launcher_res.h"
#include "sb_edition.h" /* W116 EDITIONS: Badge, Hintergrund, Remaster-Paket */
#include "launcher_assist.h" /* W143 CTRL-ASSIST: Controller-Test-Dialog */
#include "sb_pack.h" /* W148 PACK-LAUNCHER: Seite "Packs" (Scan/Pruefen/packs.json) */
#include "sb_sha256.h" /* W148: XBE-Hash fuer V2 */
#include "sb_userdata.h" /* W150 USER-DATA */
#include "launcher_rules.h" /* W171 LAUNCHER-P2P3: Regelwerk R1-R30 */
#include <shellapi.h> /* W148: ShellExecuteW (Ordner oeffnen) */
#include "input_sbc_usb.h"
#include "sbc_test.h"

#define printf cli_printf

/* ------------------------------------------------------------------ */
/* Layout (logische 96-dpi-Einheiten)                                  */
/* ------------------------------------------------------------------ */
#define WIN_W 960
#define WIN_H 640
#define TITLE_H 48
#define NAV_W 208
#define BOT_H 64
#define CX0 (NAV_W + 28)           /* Inhalt links */
#define CW (WIN_W - CX0 - 28)      /* Inhalt Breite (696) */
#define CY0 (TITLE_H + 24)         /* Inhalt oben */
#define COLW ((CW - 16) / 2)       /* Spalte (340) */
#define COL2 (CX0 + COLW + 16)
/* W172 LAUNCHER-P5: Seite Packs = Liste links (breit), Detail rechts */
#define PKL_W 400                  /* linke Karten (Liste, Texturdetail) */
#define PKR_X (CX0 + PKL_W + 16)   /* rechte Karte (gewaehltes Pack) */
#define PKR_W (CW - PKL_W - 16)    /* 280 */
#define PK_ROW_H 38                /* Listenzeile (zwei Textzeilen) */

/* W171 LAUNCHER-P2P3: sieben Seiten (UX-Konzept 3.2). Remaster, Profile
 * und Pfade sind in Grafik, Packs und Erweitert aufgegangen. */
enum { P_PLAY, P_DISPLAY, P_GFX, P_PACKS, P_CTRL, P_AUDIO, P_ADV, P_N };
#define PG_ALL (-1)
#define PG_NOTPLAY (-2)
#define PG_DIALOG (-3)

/* W142: Sprachschluessel (Anzeige via LT), nicht direkt ausgeben. */
static const char *const kPageName[P_N] = {
    "gui.page_play", "gui.page_display", "gui.page_graphics", "gui.page_packs",
    "gui.page_controls", "gui.page_audio", "gui.page_advanced"
};
static const char *const kPageKey[P_N] = {
    "play", "display", "graphics", "packs", "controls", "audio", "advanced"
};
static const wchar_t kPageIcon[P_N] = { 0xE768, 0xE7F4, 0xE790, 0xE7B8,
                                        0xE7FC, 0xE767, 0xE713 };
/* Alte --ui-shot-Namen (bis W170, acht Seiten) -> neue Zielseite. */
static const struct {
    const char *name;
    int page;
} kPageAlias[] = {
    { "start", P_PLAY },     { "grafik", P_DISPLAY }, { "remaster", P_GFX },
    { "ton", P_AUDIO },      { "steuerung", P_CTRL }, { "profile", P_ADV },
    { "pfade", P_ADV },
};

int gui_page_count(void)
{
    return P_N;
}

const char *gui_page_key(int p)
{
    return (p >= 0 && p < P_N) ? kPageKey[p] : "";
}

int gui_page_from_name(const char *v)
{
    int i;
    if (!v || !*v) {
        return -1;
    }
    for (i = 0; i < P_N; i++) {
        if (strcmp(v, kPageKey[i]) == 0) {
            return i;
        }
    }
    for (i = 0; i < (int)(sizeof kPageAlias / sizeof kPageAlias[0]); i++) {
        if (strcmp(v, kPageAlias[i].name) == 0) {
            return kPageAlias[i].page;
        }
    }
    if (v[0] >= '0' && v[0] < '0' + P_N && !v[1]) {
        return v[0] - '0';
    }
    return -1;
}

/* --- IDs ---------------------------------------------------------- */
#define ID_NAV0 1000
#define ID_MIN 1010
#define ID_CLOSE 1011
#define ID_START_BIG 1020
#define ID_START 1021
#define ID_START_VR 1038   /* W159: Spielen in VR (nur mit OpenXR-Runtime) */
#define ID_START_MENU 1039 /* W171: Fussleiste "Launch v" (Bildschirm / VR) */
#define ID_UNDO 1040       /* W171: Rueckgaengig (Muster C, Strg+Z) */
#define ID_DIAG 1041       /* W171: Diagnose ein-/ausklappen (Erweitert) */
#define ID_SAVE 1022
#define ID_BG 1023
#define ID_PRESET 101
#define ID_W 102
#define ID_H 103
#define ID_VSYNC 104
#define ID_FPS 105
#define ID_WINMODE 106
#define ID_ASPECT 107
#define ID_IPFPS 108
#define ID_MONFX 1024 /* W105: Monitor-Effekte */
#define ID_AAMODE 1032   /* W131: Kantenglaettung */
#define ID_VR 1033       /* W134 VR-PROTO, W159: VR-Vorschau (sbs / links / rechts / nur Brille) */
#define ID_LANG 1034     /* W142: Sprache (Sidebar, alle Seiten) */
#define ID_LANG_LBL 1035 /* W142: Beschriftung dazu */
#define ID_CTRL_ASSIST 1036 /* W143: Controller-Assistent oeffnen */
#define ID_REM_LABEL 1037 /* W153: HUD-Etikett weicht aus (W173: eigene Option Grafik > HUD, nicht im Preset) */
#define ID_FSHZ 110 /* W81: Bildwiederholrate (exklusiv) */
#define ID_REM_SCALE 112 /* W172: unsichtbarer Traeger fuer hd_scale (Texturdetail auf Packs) */
#define ID_EDGE 117   /* W107 EDGE-MODE: Bildraender */
#define ID_EDGE_W 118 /* W107: Verlaufsbreite */
#define ID_EDGE_S 119 /* W107: Abdunklung */
#define ID_AUDIO 120
#define ID_LIVE 121
#define ID_RTSCALE 123     /* W148: Render-Skalierung (Grafik) */
#define ID_PK_LIST 300     /* W148: Packs-Seite */
#define ID_PK_UP 301
#define ID_PK_DOWN 302
#define ID_PK_TOGGLE 303
#define ID_PK_ADD 304
#define ID_PK_DEL 305
#define ID_PK_CHECK 306
#define ID_PK_FOLDER 307
#define ID_PK_CAPRESET 311
#define ID_CTRL_PROF 140
#define ID_CTRL_REFRESH 141
#define ID_CTRL_BIND 142
#define ID_CTRL_SBC 143
#define ID_CTRL_SBCCHK 144
#define ID_CTRL_MOUSEOFF 1030 /* W128: Maus aus bei aktivem Controller */
#define ID_KEYS 145
#define ID_MOUSE_MODE 146
#define ID_MOUSE_SENS 147
#define ID_PROF_COMBO 160
#define ID_PROF_LOAD 161
#define ID_PROF_SAVE 162
#define ID_PROF_DEL 163
#define ID_PROF_DFLT 164
#define ID_XBE 180
#define ID_XBE_B 181
#define ID_MEDIA 182
#define ID_MEDIA_B 183
#define ID_HDD 184
#define ID_IMAGE 185   /* W150 ISO-DIRECT: Spiel-Abbild */
#define ID_IMAGE_B 186
#define ID_IMAGE_C 187
/* W172 LAUNCHER-P567 */
#define ID_SUM_GO0 1050    /* Spielen: "Aendern" je Bereitschaftszeile (5) */
#define SUM_GO_N 5
#define ID_PLAY_FOLDER 1055 /* Einrichtungskarte: "Ordner verwenden" */
#define ID_HDD_OPEN 1056   /* Erweitert: Spielstaende oeffnen */
#define ID_DATA_OPEN 1057  /* Erweitert: Datenort oeffnen */
#define ID_PK_LVL2 1058    /* Packs: Texturdetail 2x (Segment) */
#define ID_PK_LVL4 1059    /* Packs: Texturdetail 4x (Segment) */
#define ID_PK_CAT0 1060    /* Packs: Grenze je Kategorie (SB_PACK_NCAT Kombis) */
#define ID_PK_CREATOR 1069 /* Packs: Pack Creator oeffnen */
#define ID_PK_CHECKALL 1070
#define ID_GFX_PACKS 1071  /* Grafik: Sprung zur Seite Packs (Texturdetail) */
/* W173 LAUNCHER-P4: ein Grafik-Preset (Konzept 3.3/4.3) */
#define ID_GP0 1080        /* Optionsfelder Original/dezent/stark/Benutzerdefiniert (1080..1083) */
#define ID_GP_RESET 1084   /* Benutzerdefiniert: auf die Basis zuruecksetzen */
#define ID_GFX_LIGHT 1085  /* Weltbeleuchtung (shading) */
#define ID_GFX_SHADOW 1086 /* Schatten (shade_shadow) */
#define ID_GFX_SOFT 1087   /* Weiche Schatten (shadow_soft) */
#define ID_GFX_FX 1088     /* Effekte (Stufe der fx_*) */
#define ID_GFX_TEX 1089    /* Texturen: Original / HD aus Packs */
#define ID_CTRL_BROWSER 1090 /* W179: Tastenuebersicht als HTML im Browser */
#define ID_VRPRESET 1091   /* W183 VR-FIX3: VR-Leistung (Leistung/Ausgewogen/Qualitaet/Eigene) */
#define ID_VRRES 1092      /* W183 VR-FIX3: VR-Render-Aufloesung (% der Runtime-Empfehlung) */
#define ID_CTRL_ZADIG 1093 /* W185: Einrichtungsanleitung als HTML im Browser */
static char g_img_msg[600]; /* W150: Ergebnis der letzten Abbild-Pruefung (UTF-8) */
static int g_img_ok;
static void check_image_ex(int status);

/* --- Farben ------------------------------------------------------- */
#define C_BASE RGB(11, 13, 17)
#define C_PANEL RGB(21, 24, 30)
#define C_TEXT RGB(234, 237, 242)
#define C_DIM RGB(152, 160, 172)
#define C_FAINT RGB(108, 116, 128)
#define C_ACCENT RGB(226, 150, 58)
#define C_ACCENT_HI RGB(242, 168, 78)
#define C_ACCENT_LO RGB(196, 126, 42)
#define C_ON_ACCENT RGB(24, 18, 10)
#define C_INPUT RGB(30, 34, 42)
#define C_INPUT_BRD RGB(62, 69, 82)
#define C_BTN RGB(44, 49, 60)
#define C_BTN_HI RGB(58, 64, 77)
#define C_OK RGB(92, 196, 124)
#define C_WARN RGB(232, 176, 64)
#define C_ERR RGB(236, 96, 84)
#define C_DIALOG RGB(24, 27, 33)

/* --- Steuerelement-Register ---------------------------------------- */
enum {
    K_BTN = 1, K_ACCENT, K_BIG, K_NAV, K_TOGGLE, K_TBMIN, K_TBCLOSE,
    K_SEG, /* W172: Segment-Knopf (gewaehlt = Akzent) */
    K_RADIO, /* W173: Optionsfeld mit Erklaerung ("Titel\nHilfe") */
    K_EDIT = 10, K_COMBO, K_COMBOEDIT, K_MEMO, K_STATIC, K_LIST,
    K_TRACK = 17, /* W143: Schieberegler (Totzonen) */
    K_PKLIST = 18 /* W172: Packs-Liste (eigene Zeichnung, Ziehen) */
};

typedef struct {
    HWND h;
    HWND parent;
    int id;
    int page;
    int kind;
    int x, y, w, h_; /* logisch */
    int checked;
    int hover;
} uictl_t;

#define UI_MAX 200
static uictl_t g_ui[UI_MAX];
static int g_ui_n;

/* --- Zustand -------------------------------------------------------- */
static gui_opts_t g_o;
static HWND g_hwnd;
static int g_dpi = 96;
static int g_page;
static int g_updating;
static char g_client_dir[1024];
static char g_client_disp[1024]; /* normalisiert, nur Anzeige */
static int g_has_ldr;
static char g_note[256];       /* user.cfg-/Profil-Lage beim Start */
static int g_note_kind;        /* W173: 2 = gespeicherte Einstellung beim Start geladen */
static char g_status[1024];    /* Statuszeile unten */
static int g_status_kind;      /* 0 = normal, 1 = ok, 2 = Fehler */
static char g_aspect_info[512];
static char g_sbc_txt[512];
static char g_pad_txt[160];
static char g_padlive_txt[512];
static char g_prof_info[256];
static char g_profile_line[256];
static int g_focus_id;         /* Edit mit Fokus (Rahmenfarbe) */
static unsigned g_prev_w = 2560, g_prev_h = 1080;
static int g_prev_edge, g_prev_ew = 12, g_prev_es = 100; /* W107: Vorschau Bildraender */
static int g_bg_on = 1;
/* W171 LAUNCHER-P2P3: Regelwerk (ui_apply_rules) + Laufzeitwissen */
static lr_res_t g_rr[LR_N];       /* letzte Auswertung R1..R30 */
static const lr_env_t *g_env_fix; /* Selbsttest: feste Umgebung statt Messung */
static int g_diag_open;           /* Erweitert: Diagnose aufgeklappt (UI-cfg) */
static int g_sbc_rc = -1;         /* lctrl_sbc_status: 0 = gefunden, -1 = ungeprueft */
static int g_disp_hz;             /* Bildwiederholrate des Hauptmonitors */
static unsigned g_mon_w, g_mon_h; /* Aufloesung des Hauptmonitors */
static char g_dflt_setup[64];     /* Standard-Profil (profiles/standard.txt) */
static int g_confirm_auto;        /* Selbsttest: Rueckfragen mit Ja beantworten */
static launcher_settings_t g_undo_s; /* Muster C: Stand vor dem Umschalten */
static int g_undo_on;
/* W172 LAUNCHER-P567 */
static int g_setup_force;         /* Spielen: Einrichtungskarte offen halten */
static int g_data_ok = 1;         /* letzte Spieldaten-Pruefung (Regelwerk) */
static char g_data_err[300];
static HWND g_tip;                /* Tooltips (Begruendung gesperrter Elemente) */
static int g_creator = -1;        /* sb_creator.exe neben dem Launcher: -1 ungeprueft */
static void ui_apply_rules(void);
static lgp_mask_t gfx_lock_now(void); /* W173: Effect-Pack-Schluessel (Packs-Engine) */
static launcher_settings_t g_gfx; /* W173: Werte des Grafik-Presets (s. gfx_sync_ui) */
static void gfx_sync_ui(void);
static int g_tex_src;             /* W173: aktive Texturquellen (Packs + lokaler Ordner) */
static int play_setup_shown(void);
static int rem_avail(void);
static int packs_count(void);
static int packs_status_of(int i);
static int packs_enabled_of(int i);
static int packs_is_fx(int i);
static const char *packs_level_word(void);
static void snap_settings(launcher_settings_t *out);
static void packs_draw_row(const DRAWITEMSTRUCT *d);
static void xbe_cue_update(void);
static void setup_auto_sync(int from_list);
static void xbe_derived(const launcher_settings_t *s, char *out, size_t cap);
static int path_eq(const char *a, const char *b);
static void tips_place(void);
static void ui_refresh_visibility(void);
static void show_page(int p);
static int ui_confirm(const char *text, const char *title);

/* W116 EDITIONS: Remaster-Paket lokal vorhanden? Die Seite Remaster zeigt
 * ihre Inhalte nur dann. Ordner wie im Client (sb_remaster.c,
 * rem_resolve_dir): remaster_dir aus sb_native.cfg/user.cfg des
 * Client-Ordners (user.cfg gewinnt, letzte Zeile gilt), sonst
 * <erster Vorfahr mit poc-native/>/remaster, sonst <Client>/remaster.
 * Paket = textures/x4, x2 oder manual (Ordner) oder textures/<hash>.dds. */
static int g_rem_pkg = -1; /* -1 unbekannt, 0 keins, 1 vorhanden */
static char g_rem_dir[1024];

static int cfg_read_key(const char *path, const char *key, char *out,
                        size_t cap)
{
    FILE *f = NULL;
    char ln[1200];
    size_t kl = strlen(key);
    int found = 0;
    if (u8_fopen_s(&f, path, "r") != 0 || !f) {
        return 0;
    }
    while (fgets(ln, sizeof ln, f)) {
        char *s = ln, *e;
        while (*s == ' ' || *s == '\t') {
            s++;
        }
        if (strncmp(s, key, kl) != 0) {
            continue;
        }
        s += kl;
        while (*s == ' ' || *s == '\t') {
            s++;
        }
        if (*s != '=') {
            continue;
        }
        s++;
        while (*s == ' ' || *s == '\t') {
            s++;
        }
        e = s + strlen(s);
        while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' ||
                         e[-1] == '\t')) {
            *--e = '\0';
        }
        strncpy_s(out, cap, s, _TRUNCATE);
        found = 1;
    }
    fclose(f);
    return found;
}

static int is_dir_u8(const char *p)
{
    unsigned long a = u8_GetFileAttributes(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static int rem_pkg_detect(const char *client_dir, char *dir, size_t cap)
{
    static const char *const sub[] = { "x4", "x2", "manual" };
    char p[1300], v[1024], u[1024], base[1024], *s;
    u8_find_t fd;
    int i;
    v[0] = '\0';
    _snprintf_s(p, sizeof p, _TRUNCATE, "%s/sb_native.cfg", client_dir);
    cfg_read_key(p, "remaster_dir", v, sizeof v);
    _snprintf_s(p, sizeof p, _TRUNCATE, "%s/user.cfg", client_dir);
    if (cfg_read_key(p, "remaster_dir", u, sizeof u)) {
        strcpy_s(v, sizeof v, u);
    }
    if (v[0]) {
        strcpy_s(dir, cap, v);
    } else {
        _snprintf_s(dir, cap, _TRUNCATE, "%s/remaster", client_dir);
        strcpy_s(base, sizeof base, client_dir);
        for (s = base; *s; s++) {
            if (*s == '\\') {
                *s = '/';
            }
        }
        for (;;) {
            _snprintf_s(p, sizeof p, _TRUNCATE, "%s/poc-native", base);
            if (is_dir_u8(p)) {
                _snprintf_s(dir, cap, _TRUNCATE, "%s/remaster", base);
                break;
            }
            s = strrchr(base, '/');
            if (!s) {
                break;
            }
            *s = '\0';
        }
    }
    for (s = dir; *s; s++) {
        if (*s == '\\') {
            *s = '/';
        }
    }
    for (i = 0; i < 3; i++) {
        _snprintf_s(p, sizeof p, _TRUNCATE, "%s/textures/%s", dir, sub[i]);
        if (is_dir_u8(p)) {
            return 1;
        }
    }
    _snprintf_s(p, sizeof p, _TRUNCATE, "%s/textures/*.dds", dir);
    if (u8_find_first(p, &fd)) {
        u8_find_close(&fd);
        return 1;
    }
    return 0;
}
static launcher_settings_t g_s;
static launcher_settings_t g_cur; /* W172: Oberflaechen-Stand der letzten Regelauswertung */

/* Schriften */
enum { F_BODY, F_SMALL, F_SEMI, F_CARD, F_BRAND, F_HERO, F_BIG, F_ICON,
       F_MONO, F_TINY, F_ICONSM, F_N };
static HFONT g_f[F_N];
static int g_has_icons;
static wchar_t g_face_body[64];

#define SX(v) (MulDiv((v), g_dpi, 96))

/* ------------------------------------------------------------------ */
/* Kleinkram                                                          */
/* ------------------------------------------------------------------ */
static void ui_settext(HWND h, const char *u8)
{
    wchar_t *w = u8_wdup(u8);
    SetWindowTextW(h, w ? w : L"");
    free(w);
}

static void ui_gettext(HWND h, char *out, size_t cap)
{
    wchar_t w[2048];
    GetWindowTextW(h, w, 2048);
    w_to_u8(w, out, (int)cap);
}

static void trim_u8(char *s)
{
    char *p = s, *e;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (p != s) {
        memmove(s, p, strlen(p) + 1);
    }
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t')) {
        e--;
    }
    *e = '\0';
}

static uictl_t *ui_find(int id)
{
    int i;
    for (i = 0; i < g_ui_n; i++) {
        if (g_ui[i].id == id && g_ui[i].parent == g_hwnd) {
            return &g_ui[i];
        }
    }
    return NULL;
}

static uictl_t *ui_by_hwnd(HWND h)
{
    int i;
    for (i = 0; i < g_ui_n; i++) {
        if (g_ui[i].h == h) {
            return &g_ui[i];
        }
    }
    return NULL;
}

static HWND ctl(int id)
{
    uictl_t *u = ui_find(id);
    return u ? u->h : NULL;
}

static int ui_checked(int id)
{
    uictl_t *u = ui_find(id);
    return u ? u->checked : 0;
}

static void ui_set_checked(int id, int on)
{
    uictl_t *u = ui_find(id);
    if (u) {
        u->checked = on ? 1 : 0;
        InvalidateRect(u->h, NULL, FALSE);
    }
}

/* Nachrichtenfenster nur im Normalbetrieb (Test-/Shot-Modi: Text auf
 * die Konsole, Antwort = Standard, nie ein Fenster). */
static int ui_msgbox(HWND owner, const char *text, const char *title,
                     UINT flags)
{
    if (g_o.shot != 0) {
        printf("[%s] %s\n", title, text);
        return (flags & MB_YESNO) ? IDNO : IDOK;
    }
    {
        wchar_t *wt = u8_wdup(text), *wc = u8_wdup(title);
        int r = MessageBoxW(owner, wt, wc, flags);
        free(wt);
        free(wc);
        return r;
    }
}

/* ------------------------------------------------------------------ */
/* Zeichenflaeche (32-bit-DIB, eigenes Alpha-Mischen mit Kantenglaettung) */
/* ------------------------------------------------------------------ */
typedef struct {
    HDC dc;
    HBITMAP bmp;
    HGDIOBJ old;
    uint32_t *px;
    int w, h;
} canvas_t;

static canvas_t g_base, g_bb; /* Hintergrund-Schicht / fertiges Bild */

static void cv_free(canvas_t *c)
{
    if (c->dc) {
        SelectObject(c->dc, c->old);
        DeleteDC(c->dc);
    }
    if (c->bmp) {
        DeleteObject(c->bmp);
    }
    memset(c, 0, sizeof *c);
}

static int cv_make(canvas_t *c, int w, int h)
{
    BITMAPINFO bi;
    void *bits = NULL;
    if (c->dc && c->w == w && c->h == h) {
        return 1;
    }
    cv_free(c);
    if (w <= 0 || h <= 0) {
        return 0;
    }
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    c->dc = CreateCompatibleDC(NULL);
    c->bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!c->dc || !c->bmp || !bits) {
        cv_free(c);
        return 0;
    }
    c->px = (uint32_t *)bits;
    c->old = SelectObject(c->dc, c->bmp);
    c->w = w;
    c->h = h;
    return 1;
}

static float rr_cov(float px, float py, float x, float y, float w, float h,
                    float r)
{
    float hw = w * 0.5f, hh = h * 0.5f, qx, qy, ox, oy, d, m;
    if (w <= 0 || h <= 0) {
        return 0.f;
    }
    if (r > hw) {
        r = hw;
    }
    if (r > hh) {
        r = hh;
    }
    qx = fabsf(px - (x + hw)) - (hw - r);
    qy = fabsf(py - (y + hh)) - (hh - r);
    ox = qx > 0 ? qx : 0;
    oy = qy > 0 ? qy : 0;
    m = qx > qy ? qx : qy;
    d = sqrtf(ox * ox + oy * oy) + (m < 0 ? m : 0) - r;
    d = 0.5f - d;
    return d < 0 ? 0.f : d > 1 ? 1.f : d;
}

static void px_blend(uint32_t *p, int r, int g, int b, int a256)
{
    uint32_t d = *p;
    int dr = (int)((d >> 16) & 255), dg = (int)((d >> 8) & 255),
        db = (int)(d & 255);
    dr += ((r - dr) * a256) >> 8;
    dg += ((g - dg) * a256) >> 8;
    db += ((b - db) * a256) >> 8;
    *p = ((uint32_t)dr << 16) | ((uint32_t)dg << 8) | (uint32_t)db;
}

/* Gefuelltes (abgerundetes) Rechteck, alpha 0..255, Pixel-Koordinaten. */
static void cv_fill(canvas_t *c, int x, int y, int w, int h, COLORREF col,
                    int alpha, int rad)
{
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > c->w ? c->w : x + w, y1 = y + h > c->h ? c->h : y + h;
    int R = GetRValue(col), G = GetGValue(col), B = GetBValue(col);
    int xx, yy;
    if (!c->px || x0 >= x1 || y0 >= y1) {
        return;
    }
    GdiFlush();
    for (yy = y0; yy < y1; yy++) {
        uint32_t *row = c->px + (size_t)yy * (size_t)c->w;
        int inner_y = rad <= 0 || (yy >= y + rad && yy < y + h - rad);
        for (xx = x0; xx < x1; xx++) {
            int a;
            if (inner_y || (xx >= x + rad && xx < x + w - rad)) {
                a = alpha * 256 / 255;
            } else {
                float cv = rr_cov((float)xx + 0.5f, (float)yy + 0.5f,
                                  (float)x, (float)y, (float)w, (float)h,
                                  (float)rad);
                a = (int)(alpha * cv * 256.f / 255.f);
            }
            if (a > 0) {
                px_blend(row + xx, R, G, B, a);
            }
        }
    }
}

/* Rahmen der Dicke t (abgerundet). */
static void cv_ring(canvas_t *c, int x, int y, int w, int h, COLORREF col,
                    int alpha, int rad, int t)
{
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > c->w ? c->w : x + w, y1 = y + h > c->h ? c->h : y + h;
    int R = GetRValue(col), G = GetGValue(col), B = GetBValue(col);
    int xx, yy;
    float ir = (float)(rad - t);
    if (!c->px || x0 >= x1 || y0 >= y1) {
        return;
    }
    if (ir < 0) {
        ir = 0;
    }
    GdiFlush();
    for (yy = y0; yy < y1; yy++) {
        uint32_t *row = c->px + (size_t)yy * (size_t)c->w;
        for (xx = x0; xx < x1; xx++) {
            float co, ci, cv;
            int a;
            /* Innenbereich weit weg von Kanten: ueberspringen */
            if (xx >= x + rad + t && xx < x + w - rad - t && yy >= y + t + 1 &&
                yy < y + h - t - 1) {
                continue;
            }
            if (yy >= y + rad + t && yy < y + h - rad - t && xx >= x + t + 1 &&
                xx < x + w - t - 1) {
                continue;
            }
            co = rr_cov((float)xx + 0.5f, (float)yy + 0.5f, (float)x,
                        (float)y, (float)w, (float)h, (float)rad);
            ci = rr_cov((float)xx + 0.5f, (float)yy + 0.5f, (float)(x + t),
                        (float)(y + t), (float)(w - 2 * t),
                        (float)(h - 2 * t), ir);
            cv = co - ci;
            if (cv <= 0) {
                continue;
            }
            a = (int)(alpha * cv * 256.f / 255.f);
            if (a > 0) {
                px_blend(row + xx, R, G, B, a);
            }
        }
    }
}

static void cv_text(canvas_t *c, HFONT f, const char *u8, int x, int y,
                    int w, int h, COLORREF col, UINT fmt)
{
    RECT r;
    wchar_t *wt;
    if (!c->dc || !u8 || !*u8) {
        return;
    }
    wt = u8_wdup(u8);
    if (!wt) {
        return;
    }
    r.left = x;
    r.top = y;
    r.right = x + w;
    r.bottom = y + h;
    SelectObject(c->dc, f);
    SetBkMode(c->dc, TRANSPARENT);
    SetTextColor(c->dc, col);
    DrawTextW(c->dc, wt, -1, &r, fmt | DT_NOPREFIX);
    free(wt);
}

static void cv_textw(canvas_t *c, HFONT f, const wchar_t *wt, int n, int x,
                     int y, int w, int h, COLORREF col, UINT fmt)
{
    RECT r;
    r.left = x;
    r.top = y;
    r.right = x + w;
    r.bottom = y + h;
    SelectObject(c->dc, f);
    SetBkMode(c->dc, TRANSPARENT);
    SetTextColor(c->dc, col);
    DrawTextW(c->dc, wt, n, &r, fmt | DT_NOPREFIX);
}

/* Logische Wrapper auf das fertige Bild g_bb. */
static void L_fill(int x, int y, int w, int h, COLORREF c, int a, int r)
{
    cv_fill(&g_bb, SX(x), SX(y), SX(x + w) - SX(x), SX(y + h) - SX(y), c, a,
            SX(r));
}

static void L_ring(int x, int y, int w, int h, COLORREF c, int a, int r)
{
    int t = SX(1) < 1 ? 1 : SX(1);
    cv_ring(&g_bb, SX(x), SX(y), SX(x + w) - SX(x), SX(y + h) - SX(y), c, a,
            SX(r), t);
}

static void L_text(int font, const char *u8, int x, int y, int w, int h,
                   COLORREF c, UINT fmt)
{
    cv_text(&g_bb, g_f[font], u8, SX(x), SX(y), SX(x + w) - SX(x),
            SX(y + h) - SX(y), c, fmt);
}

/* ------------------------------------------------------------------ */
/* Schriften                                                          */
/* ------------------------------------------------------------------ */
static HFONT mkfont(const wchar_t *const *faces, int pt10, int weight,
                    wchar_t *used, int used_cap)
{
    HDC dc = GetDC(NULL);
    HFONT f = NULL;
    int i;
    for (i = 0; faces[i]; i++) {
        wchar_t got[64];
        f = CreateFontW(-MulDiv(pt10, g_dpi, 720), 0, 0, 0, weight, FALSE,
                        FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH | FF_DONTCARE, faces[i]);
        if (!f) {
            continue;
        }
        if (!faces[i + 1]) {
            break; /* letzte Wahl: nehmen */
        }
        {
            HGDIOBJ o = SelectObject(dc, f);
            got[0] = 0;
            GetTextFaceW(dc, 64, got);
            SelectObject(dc, o);
        }
        if (_wcsicmp(got, faces[i]) == 0) {
            break;
        }
        DeleteObject(f);
        f = NULL;
    }
    if (f && used) {
        HGDIOBJ o = SelectObject(dc, f);
        GetTextFaceW(dc, used_cap, used);
        SelectObject(dc, o);
    }
    ReleaseDC(NULL, dc);
    if (!f) {
        f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    }
    return f;
}

static void fonts_free(void)
{
    int i;
    for (i = 0; i < F_N; i++) {
        if (g_f[i]) {
            DeleteObject(g_f[i]);
            g_f[i] = NULL;
        }
    }
}

static void fonts_make(void)
{
    static const wchar_t *const text[] = { L"Segoe UI Variable Text",
                                           L"Segoe UI", NULL };
    static const wchar_t *const semi[] = {
        L"Segoe UI Variable Text Semibold", L"Segoe UI Semibold",
        L"Segoe UI", NULL
    };
    static const wchar_t *const disp[] = {
        L"Segoe UI Variable Display Semib", L"Segoe UI Variable Display",
        L"Segoe UI Semibold", L"Segoe UI", NULL
    };
    static const wchar_t *const bold[] = { L"Segoe UI Variable Display",
                                           L"Segoe UI", NULL };
    static const wchar_t *const icon[] = { L"Segoe Fluent Icons",
                                           L"Segoe MDL2 Assets", NULL };
    static const wchar_t *const mono[] = { L"Cascadia Mono", L"Consolas",
                                           NULL };
    wchar_t used[64];
    fonts_free();
    g_f[F_BODY] = mkfont(text, 100, FW_NORMAL, g_face_body, 64);
    g_f[F_SMALL] = mkfont(text, 90, FW_NORMAL, NULL, 0);
    g_f[F_TINY] = mkfont(text, 82, FW_NORMAL, NULL, 0);
    g_f[F_SEMI] = mkfont(semi, 100, FW_SEMIBOLD, NULL, 0);
    g_f[F_CARD] = mkfont(disp, 110, FW_SEMIBOLD, NULL, 0);
    g_f[F_BRAND] = mkfont(disp, 105, FW_SEMIBOLD, NULL, 0);
    g_f[F_HERO] = mkfont(bold, 300, FW_BOLD, NULL, 0);
    g_f[F_BIG] = mkfont(disp, 140, FW_SEMIBOLD, NULL, 0);
    g_f[F_MONO] = mkfont(mono, 90, FW_NORMAL, NULL, 0);
    used[0] = 0;
    g_f[F_ICON] = mkfont(icon, 110, FW_NORMAL, used, 64);
    g_f[F_ICONSM] = mkfont(icon, 75, FW_NORMAL, NULL, 0);
    g_has_icons = _wcsicmp(used, L"Segoe Fluent Icons") == 0 ||
                  _wcsicmp(used, L"Segoe MDL2 Assets") == 0;
}

/* ------------------------------------------------------------------ */
/* Hintergrundbild (Ressource IDR_BG, PNG, per WIC)                    */
/* ------------------------------------------------------------------ */
static const GUID kCLSID_WICFactory = {
    0xcacaf262, 0x9370, 0x4615, { 0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a }
};
static const GUID kIID_WICFactory = {
    0xec5ec8a9, 0xc395, 0x4314, { 0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70 }
};
static const GUID kFmt32BGR = {
    0x6fddc324, 0x4e03, 0x4bfe, { 0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0e }
};
static const GUID kPngContainer = {
    0x1b7cfaf4, 0x713f, 0x473c, { 0xbb, 0xcd, 0x61, 0x37, 0x42, 0x5f, 0xae, 0xaf }
};

static IWICImagingFactory *g_wic;
static uint32_t *g_img;  /* Quelle 32bpp BGR */
static int g_img_w, g_img_h;
static uint32_t *g_bgs;  /* skaliert + ausgeblichen fuer g_bgs_w x g_bgs_h */
static int g_bgs_w, g_bgs_h;

static int wic_init(void)
{
    if (g_wic) {
        return 1;
    }
    if (FAILED(CoCreateInstance(&kCLSID_WICFactory, NULL,
                                CLSCTX_INPROC_SERVER, &kIID_WICFactory,
                                (void **)&g_wic))) {
        g_wic = NULL;
        return 0;
    }
    return 1;
}

/* Ressource dekodieren. 1 = ok. */
static int bg_load(void)
{
    HRSRC r;
    HGLOBAL hg;
    void *p;
    DWORD n;
    IWICStream *st = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *fr = NULL;
    IWICFormatConverter *cvt = NULL;
    UINT w = 0, h = 0;
    int ok = 0;
    if (g_img) {
        return 1;
    }
    if (!wic_init()) {
        return 0;
    }
    r = FindResourceW(NULL, MAKEINTRESOURCEW(IDR_BG), RT_RCDATA);
    if (!r) {
        return 0;
    }
    hg = LoadResource(NULL, r);
    p = hg ? LockResource(hg) : NULL;
    n = SizeofResource(NULL, r);
    if (!p || !n) {
        return 0;
    }
    if (SUCCEEDED(IWICImagingFactory_CreateStream(g_wic, &st)) &&
        SUCCEEDED(IWICStream_InitializeFromMemory(st, (BYTE *)p, n)) &&
        SUCCEEDED(IWICImagingFactory_CreateDecoderFromStream(
            g_wic, (IStream *)st, NULL, WICDecodeMetadataCacheOnDemand,
            &dec)) &&
        SUCCEEDED(IWICBitmapDecoder_GetFrame(dec, 0, &fr)) &&
        SUCCEEDED(IWICImagingFactory_CreateFormatConverter(g_wic, &cvt)) &&
        SUCCEEDED(IWICFormatConverter_Initialize(
            cvt, (IWICBitmapSource *)fr, &kFmt32BGR, WICBitmapDitherTypeNone,
            NULL, 0.0, WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(IWICFormatConverter_GetSize(cvt, &w, &h)) && w > 0 &&
        h > 0 && w <= 8192 && h <= 8192) {
        g_img = (uint32_t *)malloc((size_t)w * h * 4);
        if (g_img &&
            SUCCEEDED(IWICFormatConverter_CopyPixels(cvt, NULL, w * 4,
                                                     w * h * 4,
                                                     (BYTE *)g_img))) {
            g_img_w = (int)w;
            g_img_h = (int)h;
            ok = 1;
        } else {
            free(g_img);
            g_img = NULL;
        }
    }
    if (cvt) {
        IWICFormatConverter_Release(cvt);
    }
    if (fr) {
        IWICBitmapFrameDecode_Release(fr);
    }
    if (dec) {
        IWICBitmapDecoder_Release(dec);
    }
    if (st) {
        IWICStream_Release(st);
    }
    return ok;
}

/* Zuschnitt "fuellen ohne Strecken": groesster Ausschnitt der Quelle mit
 * dem Seitenverhaeltnis des Ziels (Mitte, vertikal leicht oben). */
static void bg_cover_rect(int sw, int sh, int dw, int dh, WICRect *out)
{
    double sa = (double)sw / sh, da = (double)dw / dh;
    if (da > sa) { /* Ziel breiter: Hoehe beschneiden */
        int ch = (int)(sw / da + 0.5);
        out->X = 0;
        out->Width = sw;
        out->Height = ch > sh ? sh : ch;
        out->Y = (int)((sh - out->Height) * 0.45);
    } else {       /* Ziel schmaler: Breite beschneiden */
        int cw = (int)(sh * da + 0.5);
        out->Y = 0;
        out->Height = sh;
        out->Width = cw > sw ? sw : cw;
        out->X = (sw - out->Width) / 2;
    }
}

/* Skaliertes, ausgeblichenes Bild fuer dw x dh bauen (Cache). */
static int bg_scaled(int dw, int dh)
{
    IWICBitmap *src = NULL;
    IWICBitmapClipper *clip = NULL;
    IWICBitmapScaler *sc = NULL;
    WICRect cr;
    int ok = 0;
    if (g_bgs && g_bgs_w == dw && g_bgs_h == dh) {
        return 1;
    }
    if (!g_img || !wic_init() || dw <= 0 || dh <= 0) {
        return 0;
    }
    free(g_bgs);
    g_bgs = (uint32_t *)malloc((size_t)dw * dh * 4);
    if (!g_bgs) {
        return 0;
    }
    bg_cover_rect(g_img_w, g_img_h, dw, dh, &cr);
    if (SUCCEEDED(IWICImagingFactory_CreateBitmapFromMemory(
            g_wic, (UINT)g_img_w, (UINT)g_img_h, &kFmt32BGR,
            (UINT)g_img_w * 4, (UINT)(g_img_w * g_img_h * 4), (BYTE *)g_img,
            &src)) &&
        SUCCEEDED(IWICImagingFactory_CreateBitmapClipper(g_wic, &clip)) &&
        SUCCEEDED(IWICBitmapClipper_Initialize(clip, (IWICBitmapSource *)src,
                                               &cr)) &&
        SUCCEEDED(IWICImagingFactory_CreateBitmapScaler(g_wic, &sc)) &&
        SUCCEEDED(IWICBitmapScaler_Initialize(sc, (IWICBitmapSource *)clip,
                                              (UINT)dw, (UINT)dh,
                                              WICBitmapInterpolationModeFant)) &&
        SUCCEEDED(IWICBitmapScaler_CopyPixels(sc, NULL, (UINT)dw * 4,
                                              (UINT)(dw * dh * 4),
                                              (BYTE *)g_bgs))) {
        /* Ausbleichen/abdunkeln: Graustufe -> dunkle Palette, links
         * (Navigation) und unten staerker, leichter Kaltton. */
        int x, y;
        for (y = 0; y < dh; y++) {
            double fy = (double)y / dh;
            double vy = 1.0 - 0.30 * fy * fy;
            for (x = 0; x < dw; x++) {
                uint32_t p = g_bgs[(size_t)y * dw + x];
                int g = (int)(((p >> 16) & 255) * 30 + ((p >> 8) & 255) * 59 +
                              (p & 255) * 11) / 100;
                double fx = (double)x / dw;
                double k = (0.20 + 0.24 * fx) * vy;
                int r = 11 + (int)(g * k * 0.96);
                int gg = 13 + (int)(g * k * 1.00);
                int b = 17 + (int)(g * k * 1.08);
                if (r > 255) r = 255;
                if (gg > 255) gg = 255;
                if (b > 255) b = 255;
                g_bgs[(size_t)y * dw + x] =
                    ((uint32_t)r << 16) | ((uint32_t)gg << 8) | (uint32_t)b;
            }
        }
        g_bgs_w = dw;
        g_bgs_h = dh;
        ok = 1;
    }
    if (sc) {
        IWICBitmapScaler_Release(sc);
    }
    if (clip) {
        IWICBitmapClipper_Release(clip);
    }
    if (src) {
        IWICBitmap_Release(src);
    }
    if (!ok) {
        free(g_bgs);
        g_bgs = NULL;
    }
    return ok;
}

/* 32bpp-Pixel als PNG speichern (WIC). 0 = ok. */
static int png_save(const uint32_t *px, int w, int h, const char *path)
{
    IWICStream *st = NULL;
    IWICBitmapEncoder *enc = NULL;
    IWICBitmapFrameEncode *fr = NULL;
    IPropertyBag2 *props = NULL;
    IWICBitmap *bmp = NULL;
    wchar_t wp[1024];
    int rc = 1;
    if (!wic_init()) {
        return 1;
    }
    u8_to_w(path, wp, 1024);
    if (SUCCEEDED(IWICImagingFactory_CreateBitmapFromMemory(
            g_wic, (UINT)w, (UINT)h, &kFmt32BGR, (UINT)w * 4,
            (UINT)(w * h * 4), (BYTE *)px, &bmp)) &&
        SUCCEEDED(IWICImagingFactory_CreateStream(g_wic, &st)) &&
        SUCCEEDED(IWICStream_InitializeFromFilename(st, wp, GENERIC_WRITE)) &&
        SUCCEEDED(IWICImagingFactory_CreateEncoder(g_wic, &kPngContainer,
                                                   NULL, &enc)) &&
        SUCCEEDED(IWICBitmapEncoder_Initialize(enc, (IStream *)st,
                                               WICBitmapEncoderNoCache)) &&
        SUCCEEDED(IWICBitmapEncoder_CreateNewFrame(enc, &fr, &props)) &&
        SUCCEEDED(IWICBitmapFrameEncode_Initialize(fr, props)) &&
        SUCCEEDED(IWICBitmapFrameEncode_SetSize(fr, (UINT)w, (UINT)h)) &&
        SUCCEEDED(IWICBitmapFrameEncode_WriteSource(
            fr, (IWICBitmapSource *)bmp, NULL)) &&
        SUCCEEDED(IWICBitmapFrameEncode_Commit(fr)) &&
        SUCCEEDED(IWICBitmapEncoder_Commit(enc))) {
        rc = 0;
    }
    if (props) {
        IPropertyBag2_Release(props);
    }
    if (fr) {
        IWICBitmapFrameEncode_Release(fr);
    }
    if (enc) {
        IWICBitmapEncoder_Release(enc);
    }
    if (st) {
        IWICStream_Release(st);
    }
    if (bmp) {
        IWICBitmap_Release(bmp);
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* Launcher-eigene Einstellung (Hintergrundbild): launcher_ui.cfg      */
/* ------------------------------------------------------------------ */
static void ui_cfg_path(const char *client_dir, char *out, size_t cap)
{
    _snprintf_s(out, cap, _TRUNCATE, "%s/launcher_ui.cfg", client_dir);
}

/* key=0/1 aus der UI-cfg (dflt, wenn Datei/Schluessel fehlt). */
static int ui_cfg_load_key(const char *client_dir, const char *key, int dflt)
{
    char p[1200], line[256];
    FILE *f = NULL;
    size_t kl = strlen(key);
    int v = dflt;
    ui_cfg_path(client_dir, p, sizeof p);
    if (u8_fopen_s(&f, p, "r") == 0 && f) {
        while (fgets(line, sizeof line, f)) {
            if (strncmp(line, key, kl) == 0 && line[kl] == '=') {
                v = atoi(line + kl + 1) ? 1 : 0;
            }
        }
        fclose(f);
    }
    return v;
}

static int ui_cfg_load_bg(const char *client_dir)
{
    return ui_cfg_load_key(client_dir, "bg_image", 1); /* Standard: an */
}

static int ui_cfg_save_bg(const char *client_dir, int bg)
{
    char p[1200];
    FILE *f = NULL;
    ui_cfg_path(client_dir, p, sizeof p);
    if (u8_fopen_s(&f, p, "w") != 0 || !f) {
        return 1;
    }
    /* W171: + diag_open (Erweitert > Diagnose aufgeklappt) */
    fprintf(f, "# Launcher-Oberflaeche (W82), nicht vom Client gelesen\n"
               "bg_image=%d\n"
               "diag_open=%d\n",
            bg ? 1 : 0, g_diag_open ? 1 : 0);
    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Texte                                                              */
/* ------------------------------------------------------------------ */
static const struct {
    const char *label; /* NULL = lkey uebersetzen */
    unsigned w, h;
    const char *lkey;
} kPresets[] = {
    { "640 " U8_TIMES " 480 (4:3)", 640, 480, NULL },
    { "1280 " U8_TIMES " 720 (16:9)", 1280, 720, NULL },
    { "1600 " U8_TIMES " 900 (16:9)", 1600, 900, NULL },
    { "1920 " U8_TIMES " 1080 (16:9)", 1920, 1080, NULL },
    { "2560 " U8_TIMES " 1440 (16:9)", 2560, 1440, NULL },
    { "2560 " U8_TIMES " 1080 (21:9)", 2560, 1080, NULL },
    { "3440 " U8_TIMES " 1440 (21:9)", 3440, 1440, NULL },
    { NULL, 0, 0, "gui.preset_custom" },
};
#define KPRESETS_N ((int)(sizeof kPresets / sizeof kPresets[0]))

/* W81: Bildwiederholrate im exklusiven Vollbild (0 = Desktop-Rate). */
static const struct {
    int hz;
    const char *label; /* NULL = lkey uebersetzen */
    const char *lkey;
} kFsHz[] = {
    { 0, NULL, "gui.fshz_desktop" }, { 60, "60 Hz", NULL },   { 75, "75 Hz", NULL },   { 100, "100 Hz", NULL },
    { 120, "120 Hz", NULL }, { 144, "144 Hz", NULL }, { 165, "165 Hz", NULL }, { 240, "240 Hz", NULL },
};
#define KFSHZ_N ((int)(sizeof kFsHz / sizeof kFsHz[0]))

/* W107 EDGE-MODE: Verlaufsbreite (% der 4:3-Breite) und Abdunklung (%).
 * W170 LAUNCHER-P1/B19: Standard 10 % ist ein Listeneintrag (kein Roheintrag). */
static const int kEdgeW[] = { 5, 8, 10, 12, 18, 25 };
#define KEDGEW_N ((int)(sizeof kEdgeW / sizeof kEdgeW[0]))
static const int kEdgeS[] = { 60, 80, 90, 100 };
#define KEDGES_N ((int)(sizeof kEdgeS / sizeof kEdgeS[0]))

/* ------------------------------------------------------------------ */
/* Steuerelemente anlegen / anordnen                                   */
/* ------------------------------------------------------------------ */
static LRESULT CALLBACK hover_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp,
                                   UINT_PTR sid, DWORD_PTR ref)
{
    uictl_t *u;
    (void)sid;
    (void)ref;
    switch (msg) {
    case WM_MOUSEMOVE:
        u = ui_by_hwnd(h);
        if (u && !u->hover) {
            TRACKMOUSEEVENT t;
            u->hover = 1;
            memset(&t, 0, sizeof t);
            t.cbSize = sizeof t;
            t.dwFlags = TME_LEAVE;
            t.hwndTrack = h;
            TrackMouseEvent(&t);
            InvalidateRect(h, NULL, FALSE);
        }
        break;
    case WM_MOUSELEAVE:
        u = ui_by_hwnd(h);
        if (u) {
            u->hover = 0;
            InvalidateRect(h, NULL, FALSE);
        }
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_NCDESTROY:
        RemoveWindowSubclass(h, hover_proc, 1);
        break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

static void ui_place(uictl_t *u);

static HWND mk(HWND parent, int kind, int id, int page, int x, int y, int w,
               int h, const char *text, DWORD extra)
{
    uictl_t *u;
    const wchar_t *cls = L"BUTTON";
    DWORD st = WS_CHILD | extra;
    HWND hw;
    wchar_t *wt;
    if (g_ui_n >= UI_MAX) {
        return NULL;
    }
    switch (kind) {
    case K_EDIT:
        cls = L"EDIT";
        st |= ES_AUTOHSCROLL | WS_TABSTOP;
        break;
    case K_MEMO:
        cls = L"EDIT";
        st |= ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL |
              WS_TABSTOP;
        break;
    case K_COMBO:
        cls = WC_COMBOBOXW;
        st |= CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP;
        break;
    case K_COMBOEDIT:
        cls = WC_COMBOBOXW;
        st |= CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL | WS_TABSTOP;
        break;
    case K_STATIC:
        cls = L"STATIC";
        st |= SS_LEFT | SS_NOPREFIX;
        break;
    case K_LIST:
        cls = L"LISTBOX";
        st |= LBS_NOTIFY | WS_VSCROLL | WS_HSCROLL | WS_BORDER | WS_TABSTOP | /* W148: +HScroll */
              LBS_NOINTEGRALHEIGHT;
        break;
    case K_TRACK:
        cls = TRACKBAR_CLASSW;
        st |= TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP;
        break;
    case K_PKLIST: /* W172: Zeilen zeichnet packs_draw_row, Daten = Index */
        cls = L"LISTBOX";
        st |= LBS_NOTIFY | LBS_OWNERDRAWFIXED | WS_VSCROLL | WS_TABSTOP |
              LBS_NOINTEGRALHEIGHT;
        break;
    case K_TBMIN:
    case K_TBCLOSE:
        st |= BS_OWNERDRAW;
        break;
    default:
        st |= BS_OWNERDRAW | WS_TABSTOP;
        break;
    }
    wt = u8_wdup(text ? text : "");
    hw = CreateWindowExW(0, cls, wt, st, 0, 0, 10, 10, parent,
                         (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    free(wt);
    if (!hw) {
        return NULL;
    }
    u = &g_ui[g_ui_n++];
    memset(u, 0, sizeof *u);
    u->h = hw;
    u->parent = parent;
    u->id = id;
    u->page = page;
    u->kind = kind;
    u->x = x;
    u->y = y;
    u->w = w;
    u->h_ = h;
    if (kind < K_EDIT) {
        SetWindowSubclass(hw, hover_proc, 1, 0);
    }
    if (kind == K_COMBO || kind == K_COMBOEDIT) {
        SetWindowTheme(hw, L"DarkMode_CFD", NULL);
        {
            COMBOBOXINFO ci;
            memset(&ci, 0, sizeof ci);
            ci.cbSize = sizeof ci;
            if (GetComboBoxInfo(hw, &ci) && ci.hwndList) {
                SetWindowTheme(ci.hwndList, L"DarkMode_Explorer", NULL);
            }
        }
    } else if (kind == K_MEMO || kind == K_LIST || kind == K_PKLIST) {
        SetWindowTheme(hw, L"DarkMode_Explorer", NULL);
    }
    ui_place(u);
    return hw;
}

/* Pixel-Rechteck eines Elements (Rahmen; Edits liegen innen). */
static void ui_place(uictl_t *u)
{
    int x = SX(u->x), y = SX(u->y), w = SX(u->x + u->w) - SX(u->x),
        h = SX(u->y + u->h_) - SX(u->y);
    HFONT f = g_f[F_BODY];
    if (u->kind == K_MEMO) {
        f = g_f[F_SMALL];
    }
    if (u->kind == K_STATIC && u->parent != g_hwnd) {
        f = g_f[F_SMALL];
    }
    SendMessageW(u->h, WM_SETFONT, (WPARAM)f, FALSE);
    if (u->kind == K_EDIT) {
        /* Text vertikal mittig: Edit innen, Rahmen zeichnet compose(). */
        TEXTMETRICW tm;
        HDC dc = GetDC(u->h);
        HGDIOBJ o = SelectObject(dc, f);
        int th;
        GetTextMetricsW(dc, &tm);
        SelectObject(dc, o);
        ReleaseDC(u->h, dc);
        th = tm.tmHeight + 2;
        SetWindowPos(u->h, NULL, x + SX(9), y + (h - th) / 2, w - SX(18), th,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    } else if (u->kind == K_MEMO) {
        SetWindowPos(u->h, NULL, x + SX(8), y + SX(6), w - SX(10), h - SX(12),
                     SWP_NOZORDER | SWP_NOACTIVATE);
    } else if (u->kind == K_COMBO || u->kind == K_COMBOEDIT) {
        SetWindowPos(u->h, NULL, x, y, w, SX(320),
                     SWP_NOZORDER | SWP_NOACTIVATE);
        SendMessageW(u->h, CB_SETITEMHEIGHT, (WPARAM)-1, h - SX(6));
        SendMessageW(u->h, CB_SETITEMHEIGHT, 0, SX(24));
    } else if (u->kind == K_PKLIST) { /* W172: Zeilenhoehe folgt der DPI */
        SetWindowPos(u->h, NULL, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        SendMessageW(u->h, LB_SETITEMHEIGHT, 0, SX(PK_ROW_H));
    } else {
        SetWindowPos(u->h, NULL, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

static int ui_visible_on(const uictl_t *u, int page)
{
    /* W173 LAUNCHER-P4: "Zuruecksetzen" nur bei Benutzerdefiniert. Die
     * Texturen-Auswahl bleibt immer sichtbar (ohne Pack: Hinweis R30). */
    if (u->id == ID_GP_RESET && !ui_checked(ID_GP0 + LGP_CUSTOM)) {
        return 0;
    }
    /* W172/P5: Texturstufe nur noch auf der Seite Packs (Texturdetail);
     * die alte Kombi bleibt unsichtbar als Traeger fuer hd_scale. */
    if (u->id == ID_REM_SCALE) {
        return 0;
    }
    /* W172/P6: Spielen zeigt entweder die Einrichtungskarte (Disc-Abbild)
     * oder die Bereitschaft mit Sprunglinks. */
    if (u->id == ID_IMAGE || u->id == ID_IMAGE_B || u->id == ID_IMAGE_C ||
        u->id == ID_PLAY_FOLDER) {
        if (!play_setup_shown()) {
            return 0;
        }
    }
    if (u->id >= ID_SUM_GO0 && u->id < ID_SUM_GO0 + SUM_GO_N && play_setup_shown()) {
        return 0;
    }
    /* W172/P5: Liste + Detailknoepfe nur mit Packs, Pack Creator nur im
     * leeren Zustand und nur, wenn die Anwendung neben dem Launcher liegt. */
    if ((u->id == ID_PK_LIST || u->id == ID_PK_UP || u->id == ID_PK_DOWN ||
         u->id == ID_PK_CHECKALL || u->id == ID_PK_TOGGLE ||
         u->id == ID_PK_CHECK || u->id == ID_PK_DEL) && packs_count() == 0) {
        return 0;
    }
    if (u->id == ID_PK_CREATOR && (packs_count() != 0 || g_creator != 1)) {
        return 0;
    }
    /* W170 LAUNCHER-P1/B4+B7: Schalter ohne Wirkung sind keine Bedienung.
     * Die Elemente bleiben angelegt, damit Speichern/Laden/Voreinstellungen
     * unveraendert laufen (W171: auch keine Infozeile mehr, Konzept 0.8). */
    if (u->id == ID_ASPECT) {
        return 0;
    }
    /* W171: Live-Ton liegt in der eingeklappten Diagnose (Erweitert) */
    if (u->id == ID_LIVE && !g_diag_open) {
        return 0;
    }
    /* W171: Rueckgaengig nur, solange ein automatisches Umschalten offen ist */
    if (u->id == ID_UNDO && !g_undo_on) {
        return 0;
    }
    return u->page == PG_ALL || u->page == page ||
           (u->page == PG_NOTPLAY && page != P_PLAY);
}

/* Tabstopps der Tastenuebersicht (Dialog-Einheiten aus der Schrift). */
static void keys_tabstops(void)
{
    HWND e = ctl(ID_KEYS);
    TEXTMETRICW tm;
    HDC dc;
    HGDIOBJ o;
    int du;
    if (!e) {
        return;
    }
    dc = GetDC(e);
    o = SelectObject(dc, g_f[F_SMALL]);
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, o);
    ReleaseDC(e, dc);
    du = MulDiv(SX(112), 4, tm.tmAveCharWidth ? tm.tmAveCharWidth : 7);
    SendMessageW(e, EM_SETTABSTOPS, 1, (LPARAM)&du);
}

static void ui_relayout(void)
{
    int i;
    for (i = 0; i < g_ui_n; i++) {
        if (g_ui[i].parent == g_hwnd) {
            ui_place(&g_ui[i]);
        }
    }
    keys_tabstops();
    tips_place(); /* W172 */
}

/* ------------------------------------------------------------------ */
/* Bild zusammensetzen                                                 */
/* ------------------------------------------------------------------ */
static void base_build(void)
{
    int W = SX(WIN_W), H = SX(WIN_H), x, y;
    if (!cv_make(&g_base, W, H)) {
        return;
    }
    GdiFlush();
    if (g_bg_on && bg_load() && bg_scaled(W, H)) {
        memcpy(g_base.px, g_bgs, (size_t)W * H * 4);
    } else {
        for (y = 0; y < H; y++) {
            double fy = (double)y / H;
            int r = 17 - (int)(6 * fy), g = 19 - (int)(6 * fy),
                b = 24 - (int)(7 * fy);
            uint32_t c = ((uint32_t)r << 16) | ((uint32_t)g << 8) |
                         (uint32_t)b;
            for (x = 0; x < W; x++) {
                g_base.px[(size_t)y * W + x] = c;
            }
        }
    }
    /* Titelleiste, Navigation, Fussleiste: halbtransparente Flaechen. */
    cv_fill(&g_base, 0, 0, W, SX(TITLE_H), RGB(4, 5, 7), 150, 0);
    cv_fill(&g_base, 0, SX(TITLE_H), SX(NAV_W), H - SX(TITLE_H),
            RGB(6, 7, 10), 165, 0);
    cv_fill(&g_base, SX(NAV_W) - 1, SX(TITLE_H), 1, H - SX(TITLE_H),
            RGB(255, 255, 255), 16, 0);
    cv_fill(&g_base, 0, SX(TITLE_H) - 1, W, 1, RGB(255, 255, 255), 16, 0);
    cv_fill(&g_base, SX(NAV_W), H - SX(BOT_H), W - SX(NAV_W), SX(BOT_H),
            RGB(4, 5, 7), 120, 0);
    cv_fill(&g_base, SX(NAV_W), H - SX(BOT_H), W - SX(NAV_W), 1,
            RGB(255, 255, 255), 14, 0);
    /* Marke in der Titelleiste */
    cv_fill(&g_base, SX(20), SX(17), SX(4), SX(14), C_ACCENT, 255, SX(1));
    SetTextCharacterExtra(g_base.dc, SX(2));
    cv_text(&g_base, g_f[F_BRAND], "STEEL BATTALION", SX(32), 0, SX(260),
            SX(TITLE_H), C_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SetTextCharacterExtra(g_base.dc, 0);
    cv_text(&g_base, g_f[F_SMALL], "Game Preservation Project",
            SX(214), 0, SX(300), SX(TITLE_H), C_DIM,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    /* W116 EDITIONS: Edition sichtbar (Badge rechts neben dem Untertitel) */
    cv_fill(&g_base, SX(398), SX(14), SX(160), SX(20), C_ACCENT, 34, SX(3));
    cv_fill(&g_base, SX(398), SX(14), SX(2), SX(20), C_ACCENT, 255, 0);
    SetTextCharacterExtra(g_base.dc, SX(1));
    cv_text(&g_base, g_f[F_TINY], SB_EDITION_BADGE, SX(398), SX(14), SX(160),
            SX(20), C_ACCENT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SetTextCharacterExtra(g_base.dc, 0);
    /* Fuss der Navigation */
    cv_text(&g_base, g_f[F_TINY], LT("gui.foot_hint"),
            SX(20), H - SX(58), SX(NAV_W - 28), SX(44), C_FAINT,
            DT_LEFT | DT_WORDBREAK);
}

static void card(int x, int y, int w, int h, const char *title)
{
    L_fill(x, y, w, h, C_PANEL, 214, 10);
    L_ring(x, y, w, h, RGB(255, 255, 255), 20, 10);
    if (title && *title) {
        L_text(F_CARD, title, x + 18, y + 12, w - 36, 24, C_TEXT,
               DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    }
}

static void label(int font, const char *t, int x, int y, int w, int h,
                  COLORREF c)
{
    L_text(font, t, x, y, w, h, c, DT_LEFT | DT_WORDBREAK);
}

static void label1(int font, const char *t, int x, int y, int w, int h,
                   COLORREF c)
{
    L_text(font, t, x, y, w, h, c,
           DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
}

static void dot(int x, int y, COLORREF c)
{
    L_fill(x, y, 10, 10, c, 255, 5);
}

/* Spiel-Box: 4:3, maximal in w x h (Pillar-/Letterbox). */
static void aspect_box_for(unsigned w, unsigned h, unsigned *bw, unsigned *bh)
{
    if (w == 0 || h == 0) {
        *bw = 0;
        *bh = 0;
        return;
    }
    *bw = h * 4 / 3;
    *bh = h;
    if (*bw > w) {
        *bw = w;
        *bh = w * 3 / 4;
    }
}

static void paint_preview(int x, int y, int w, int h)
{
    L_fill(x, y, w, h, RGB(0, 0, 0), 235, 8);
    if (g_prev_w > 0 && g_prev_h > 0) {
        int ow = SX(w) - SX(20), oh = SX(h) - SX(20);
        long pw = (long)g_prev_w, ph = (long)g_prev_h;
        int ox, oy;
        unsigned bw, bh;
        if ((long)ow * ph > (long)oh * pw) {
            ow = (int)((long)oh * pw / ph);
        } else {
            oh = (int)((long)ow * ph / pw);
        }
        ox = SX(x) + (SX(w) - ow) / 2;
        oy = SX(y) + (SX(h) - oh) / 2;
        cv_fill(&g_bb, ox, oy, ow, oh, RGB(10, 10, 10), 255, 0);
        aspect_box_for((unsigned)ow, (unsigned)oh, &bw, &bh);
        { /* W107: Hor+-Rand der Mission je nach Bildraender-Einstellung */
            int ix = ox + (ow - (int)bw) / 2, iy = oy + (oh - (int)bh) / 2, side;
            int pill = (int)bw < ow;
            int len = pill ? ix - ox : iy - oy;
            int fw = (int)((long)g_prev_ew * (long)(pill ? (int)bw : (int)bh) / 100);
            for (side = 0; side < 2 && len > 0 && g_prev_edge != 1; side++) {
                int d; /* harter Schnitt: Rand bleibt schwarz */
                for (d = 0; d < len; d++) {
                    double f = 1.0, u;
                    int r, g, b;
                    if (g_prev_edge == 2) {
                        u = fw > 0 ? ((double)d + 0.5) / (double)fw : 1.0;
                        if (u > 1.0) { u = 1.0; }
                        u = u * u * (3.0 - 2.0 * u);
                        f = 1.0 - (double)g_prev_es / 100.0 * u;
                    }
                    r = 10 + (int)((58 - 10) * f);
                    g = 10 + (int)((72 - 10) * f);
                    b = 10 + (int)((88 - 10) * f);
                    if (pill) {
                        cv_fill(&g_bb, side ? ix + (int)bw + d : ix - 1 - d, oy, 1, oh,
                                RGB(r, g, b), 255, 0);
                    } else {
                        cv_fill(&g_bb, ox, side ? iy + (int)bh + d : iy - 1 - d, ow, 1,
                                RGB(r, g, b), 255, 0);
                    }
                }
            }
        }
        cv_ring(&g_bb, ox, oy, ow, oh, RGB(255, 255, 255), 40, 0, 1);
        {
            int ix = ox + (ow - (int)bw) / 2, iy = oy + (oh - (int)bh) / 2;
            int sh = (int)bh < 70 || (int)bw < SX(200); /* W171: kurz, wenn der Text nicht passt */
            cv_fill(&g_bb, ix, iy, (int)bw, (int)bh, RGB(44, 92, 128), 255, 0);
            cv_text(&g_bb, g_f[sh ? F_TINY : F_SEMI], sh ? "4:3" : LT("gui.preview_43"), ix, /* W148 */
                    iy, (int)bw, (int)bh, C_TEXT,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    } else {
        L_text(F_BODY, LT("gui.preview_nores"), x, y, w, h, C_DIM,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

/* Seite Spielen: Kurzstatus (Zeilen) */
typedef struct {
    const char *label;
    char value[400];
    COLORREF col;
} sumrow_t;
/* W172/P6: Spiel, Grafik, Anzeige, Packs, Steuerung (Konzept 4.1) */
#define SUM_N 5
static sumrow_t g_sum[SUM_N];
static const int kSumPage[SUM_N] = { P_ADV, P_GFX, P_DISPLAY, P_PACKS, P_CTRL };
/* W159: VR-Hinweis unter dem VR-Knopf + Runtime da (update_vr_button). */
static char g_vr_hint[256];
static int g_vr_present;

/* W171: Begruendungszeile einer Regel (Muster A/B/D/E), nur wenn sie greift.
 * Rueckgabe = belegte Hoehe (0 = nichts gezeichnet). */
static int paint_reason(int rule, int x, int y, int w, int h)
{
    const lr_res_t *r;
    COLORREF c;
    int tx = x;
    if (rule < LR_FIRST || rule > LR_LAST || !g_rr[rule].active) {
        return 0;
    }
    r = &g_rr[rule];
    c = r->level == LR_LVL_ERR    ? C_ERR
        : r->level == LR_LVL_WARN ? C_WARN
        : r->level == LR_LVL_LOCK ? C_ACCENT_HI
                                  : C_DIM;
    if (r->level == LR_LVL_LOCK && g_has_icons) { /* Muster D: Schloss */
        wchar_t ic = 0xE72E;
        cv_textw(&g_bb, g_f[F_ICONSM], &ic, 1, SX(x), SX(y), SX(14), SX(16), c,
                 DT_LEFT | DT_TOP | DT_SINGLELINE);
        tx += 16;
    }
    L_text(F_TINY, r->text, tx, y, w - (tx - x), h, c,
           h > 18 ? (DT_LEFT | DT_WORDBREAK)
                  : (DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS));
    return h;
}

/* Erster greifender Grund einer Liste (Startsperre: R4/R3 vor R5). */
static int first_rule(const int *rules, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (g_rr[rules[i]].active) {
            return rules[i];
        }
    }
    return 0;
}

static int launch_blocked(void)
{
    return g_rr[3].disable || g_rr[4].disable;
}

static void paint_play(void)
{
    int i, y;
    static const int kBig[] = { 4, 3 }, kVr[] = { 5 };
    SetTextCharacterExtra(g_bb.dc, SX(3));
    L_text(F_HERO, "STEEL BATTALION", CX0 - 2, CY0 - 2, CW, 50, C_TEXT,
           DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    SetTextCharacterExtra(g_bb.dc, 0);
    label1(F_BODY, LT("gui.start_sub"), CX0, CY0 + 50, CW, 22, C_DIM);
    /* Nutzer 7.11.: keine Schnellhilfe neben dem Startknopf (schlichter);
     * Tastenhilfe steht auf der Seite Controls. */
    /* Grund unter den Startknoepfen (Muster E/A), sonst VR-Hinweis */
    {
        int r = first_rule(kBig, 2);
        if (!r) {
            r = first_rule(kVr, 1);
        }
        if (r) {
            paint_reason(r, CX0, CY0 + 190, 330, 18);
        } else {
            label1(F_TINY, g_vr_hint, CX0, CY0 + 190, 330, 18, C_DIM);
        }
    }
    if (play_setup_shown()) {
        /* W172/P6: Einrichtungskarte (Spieldaten fehlen/ungueltig oder
         * "Aendern" an der Spiel-Zeile): Disc-Abbild waehlen + pruefen,
         * Ordner-Modus nur als Verweis nach Erweitert (Konzept 4.1). */
        card(CX0, CY0 + 216, CW, 232, LT("gui.play_setup"));
        label1(F_SMALL, LT("gui.play_setup_txt"), CX0 + 18, CY0 + 250, CW - 36,
               20, C_DIM);
        if (g_img_msg[0]) {
            label(F_SMALL, g_img_msg, CX0 + 18, CY0 + 318, CW - 36, 36,
                  g_img_ok ? C_OK : C_ERR);
        } else if (g_cur.game_image[0] || g_cur.media_root[0] || g_cur.xbe_path[0]) {
            /* konkreter Fehler (Grundtext steht schon unter den Knoepfen) */
            if (!paint_reason(3, CX0 + 18, CY0 + 318, CW - 36, 36) &&
                !g_cur.game_image[0] && g_cur.media_root[0]) {
                label(F_SMALL, LT("gui.play_folder_used"), CX0 + 18, CY0 + 318,
                      CW - 36, 36, C_DIM);
            }
        }
        label1(F_SMALL, LT("gui.play_folder_q"), CX0 + 18, CY0 + 400,
               CW - 36 - 196, 24, C_DIM);
#if SB_EDITION_COMMUNITY
        label(F_TINY, LT("gui.play_own_disc"), CX0 + 18, CY0 + 360, CW - 36, 30,
              C_FAINT);
#endif
    } else {
        /* W172/P6: Bereitschaft mit Sprunglinks ("Aendern" = Seite der
         * Ursache; Punkt gruen/gelb/rot). */
        card(CX0, CY0 + 216, CW, 232, LT("gui.sum_title"));
        for (i = 0, y = CY0 + 256; i < SUM_N; i++, y += 36) {
            dot(CX0 + 20, y + 6, g_sum[i].col);
            label1(F_SMALL, g_sum[i].label, CX0 + 40, y, 100, 22, C_DIM);
            label1(F_SMALL, g_sum[i].value, CX0 + 142, y, CW - 142 - 18 - 112, 22,
                   C_TEXT);
        }
    }
    /* Primaerfeuer deutlich (nur Tastatur-Standardbelegung, R26) */
    if (!g_rr[26].active) {
        L_fill(CX0, CY0 + 456, CW, 40, C_ACCENT, 46, 10);
        L_ring(CX0, CY0 + 456, CW, 40, C_ACCENT, 140, 10);
        label1(F_SEMI, LT("gui.primfeuer"), CX0 + 18, CY0 + 465, 130, 22,
               C_ACCENT_HI);
        label1(F_BODY, LT("gui.primfeuer_hint"), CX0 + 140, CY0 + 465, CW - 156,
               22, C_TEXT);
    }
}

/* W183 VR-FIX3: Stufen der VR-Render-Aufloesung (Kombi ID_VRRES) */
static const int kVrRes[] = { 50, 60, 65, 70, 80, 85, 90, 100, 120 };
#define KVRRES_N ((int)(sizeof kVrRes / sizeof kVrRes[0]))
static int vrres_index(int pct)
{
    int k, best = 0;
    for (k = 0; k < KVRRES_N; k++) {
        if (abs(kVrRes[k] - pct) < abs(kVrRes[best] - pct)) { best = k; }
    }
    return best;
}

/* W171: Seite Anzeige (Bildschirm, Bildrate, VR-Vorschau, Bildformat,
 * Render-Skalierung). */
static void paint_display(void)
{
    card(CX0, CY0, COLW, 232, LT("gui.disp_screen"));
    label1(F_SMALL, LT("gui.gfx_winmode"), CX0 + 18, CY0 + 44, 100, 30, C_DIM);
    label1(F_SMALL, LT("gui.disp_hz"), CX0 + 18, CY0 + 80, 100, 30, C_DIM);
    paint_reason(8, CX0 + 120, CY0 + 112, COLW - 138, 16);
    label1(F_SMALL, LT("gui.gfx_res"), CX0 + 18, CY0 + 132, 100, 30, C_DIM);
    label1(F_SMALL, LT("gui.gfx_wh"), CX0 + 18, CY0 + 168, 100, 30, C_DIM);
    label1(F_BODY, U8_TIMES, CX0 + 215, CY0 + 168, 14, 30, C_DIM);
    paint_reason(7, CX0 + 18, CY0 + 204, COLW - 36, 16);

    card(CX0, CY0 + 244, COLW, 112, LT("gui.disp_smooth"));
    label1(F_SMALL, LT("gui.gfx_ipfps"), CX0 + 18, CY0 + 288, 100, 30, C_DIM);
    if (!paint_reason(10, CX0 + 18, CY0 + 322, COLW - 36, 30)) {
        paint_reason(9, CX0 + 18, CY0 + 322, COLW - 36, 30);
    }

    card(CX0, CY0 + 368, COLW, 128, LT("gui.disp_vr"));
    label1(F_SMALL, LT("gui.gfx_vr"), CX0 + 18, CY0 + 412, 100, 30, C_DIM);
    label1(F_SMALL, LT("gui.vr_perf"), CX0 + 18, CY0 + 446, 100, 30, C_DIM); /* W183 */
    paint_reason(6, CX0 + 18, CY0 + 478, COLW - 36, 16);

    card(COL2, CY0, COLW, 376, LT("gui.gfx_aspect"));
    /* R13: festes Bildformat (S3.10) als Info, kein Schalter */
    label(F_SMALL, g_rr[13].text, COL2 + 18, CY0 + 42, COLW - 36, 52, C_DIM);
    label1(F_SMALL, LT("gui.gfx_edge"), COL2 + 18, CY0 + 100, 100, 30, C_DIM);
    label1(F_SMALL, LT("gui.gfx_edgefade"), COL2 + 18, CY0 + 136, 100, 30, C_DIM);
    if (!paint_reason(11, COL2 + 120, CY0 + 168, COLW - 138, 16)) {
        paint_reason(12, COL2 + 120, CY0 + 168, COLW - 138, 16);
    }
    paint_preview(COL2 + 18, CY0 + 190, COLW - 36, 80);
    label(F_SMALL, g_aspect_info, COL2 + 18, CY0 + 276, COLW - 36, 94, C_TEXT);

    card(COL2, CY0 + 388, COLW, 108, LT("gui.disp_perf"));
    label1(F_SMALL, LT("gui.rs_label"), COL2 + 18, CY0 + 432, 100, 30, C_DIM);
    label(F_TINY, LT("gui.rs_hint"), COL2 + 18, CY0 + 466, COLW - 36, 28, C_DIM);
}

/* W171: Seite Grafik (Voreinstellungen, Bild, Remaster, HUD). */
/* W173: Abweichungs-Punkt (Konzept 4.3 "*") vor der Beschriftung. */
static void gfx_label(const char *key, int x, int y, int mark)
{
    if (mark) {
        L_fill(x + 6, y + 12, 6, 6, C_ACCENT_HI, 255, 3);
    }
    label1(F_SMALL, LT(key), x + 18, y, 100, 30, C_DIM);
}

static void paint_gfx(void)
{
    lgp_mask_t lock = gfx_lock_now(), dm = 0;
    int p = lcfg_gfx_preset(&g_gfx, lock);
    int fxbits = 0, k;
    if (p == LGP_CUSTOM) {
        lcfg_gfx_diff(&g_gfx, lcfg_gfx_base(&g_gfx, lock), lock, &dm);
    }
    for (k = 0; k < LFX_N; k++) {
        if (dm & ((lgp_mask_t)1 << (LGK_FX0 + k))) {
            fxbits = 1;
        }
    }
    card(CX0, CY0, COLW, 292, LT("gui.gfx_preset_card"));
    if (g_rr[20].active) { /* Muster D an der Voreinstellung */
        L_text(F_TINY, g_rr[20].text2, CX0 + 18, CY0 + 238, COLW - 36, 18, C_ACCENT_HI,
               DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    { /* Texturen-Zeile (Konzept 4.3): Quelle und Detail, Sprung zu Packs */
        char t[240];
        if (!lcfg_gfx_value(&g_gfx, LGK_HD)) {
            strcpy_s(t, sizeof t, LT("gui.gfx_tex_orig"));
        } else if (g_rr[30].active) {
            strcpy_s(t, sizeof t, g_rr[30].text);
        } else {
            _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.gfx_tex_on"), packs_level_word(),
                        g_tex_src);
        }
        label(F_TINY, t, CX0 + 18, CY0 + 256, COLW - 36 - 104, 30, C_DIM);
    }

    card(CX0, CY0 + 304, COLW, 120, LT("gui.gfx_image"));
    gfx_label("gui.gfx_aa", CX0, CY0 + 348, (dm & ((lgp_mask_t)1 << LSHX_AA)) != 0);
    label1(F_SMALL, LT("gui.gfx_monitor"), CX0 + 18, CY0 + 384, 100, 30, C_DIM);
    label(F_SMALL, LT("gui.gfx_esc_hint"), CX0 + 4, CY0 + 432, COLW - 8, 40,
          C_FAINT);

    card(COL2, CY0, COLW, 292, LT("gui.gfx_world"));
    gfx_label("gui.gfx_light", COL2, CY0 + 44, (dm & ((lgp_mask_t)1 << LSHX_SHADING)) != 0);
    gfx_label("gui.gfx_shadow", COL2, CY0 + 80, (dm & ((lgp_mask_t)1 << LSHX_SHADOW)) != 0);
    gfx_label("gui.gfx_soft", COL2, CY0 + 116, (dm & ((lgp_mask_t)1 << LSHX_SOFT)) != 0);
    gfx_label("gui.gfx_fx", COL2, CY0 + 152, fxbits);
    label(F_TINY, LT("gui.gfx_fx_txt"), COL2 + 120, CY0 + 184, COLW - 138, 28, C_DIM);
    gfx_label("gui.gfx_tex", COL2, CY0 + 212, (dm & ((lgp_mask_t)1 << LGK_HD)) != 0);
    { /* Begruendungen (Muster A/D): hoechstens zwei Zeilen; eine allein
       * darf umbrechen */
        static const int rr[3] = { 20, 15, 16 };
        int i, y = CY0 + 248, na = 0;
        for (i = 0; i < 3; i++) {
            na += g_rr[rr[i]].active;
        }
        for (i = 0; i < 3 && y < CY0 + 284; i++) {
            y += paint_reason(rr[i], COL2 + 18, y, COLW - 36, na == 1 ? 34 : 16) ? 18 : 0;
        }
    }

    card(COL2, CY0 + 304, COLW, 192, LT("gui.gfx_hud"));
    label(F_SMALL, LT("gui.rem_label_txt"), COL2 + 18, CY0 + 384, COLW - 36, 104,
          C_DIM);
}

static void paint_audio(void)
{
    card(CX0, CY0, CW, 140, LT("gui.audio_card"));
    label(F_SMALL, LT("gui.audio_dev_txt"), CX0 + 18, CY0 + 90, CW - 36, 40,
          C_DIM);
}

static void paint_ctrl(void)
{
    card(CX0, CY0, COLW, 248, LT("gui.ctl_device"));
    label(F_SMALL, g_sbc_txt, CX0 + 18, CY0 + 80, COLW - 36, 34, C_TEXT);
    paint_reason(25, CX0 + 18, CY0 + 116, COLW - 36, 30);
    paint_reason(23, CX0 + 18, CY0 + 180, COLW - 36, 16);

    card(CX0, CY0 + 260, COLW, 120, LT("gui.ctl_profile"));

    card(CX0, CY0 + 392, COLW, 104, LT("gui.ctl_pad"));
    label(F_BODY, g_pad_txt, CX0 + 18, CY0 + 432, COLW - 36, 22, C_TEXT);
    label(F_SMALL, g_padlive_txt, CX0 + 18, CY0 + 456, COLW - 36, 36, C_DIM);

    card(COL2, CY0, COLW, 496, LT("gui.ctl_keymap"));
    L_fill(COL2 + 18, CY0 + 46, COLW - 36, 50, C_ACCENT, 46, 8);
    L_ring(COL2 + 18, CY0 + 46, COLW - 36, 50, C_ACCENT, 150, 8);
    label1(F_TINY, LT("gui.ctl_prim"), COL2 + 32, CY0 + 50, COLW - 60, 18,
           C_ACCENT_HI);
    label1(F_SEMI, LT("gui.ctl_prim_keys"), COL2 + 32, CY0 + 68, COLW - 60, 24,
           C_TEXT);
    label1(F_SMALL, LT("gui.ctl_mouse"), COL2 + 18, CY0 + 108, 56, 30, C_DIM);
    label1(F_SMALL, LT("gui.ctl_sens"), COL2 + COLW - 18 - 70 - 52, CY0 + 108,
           48, 30, C_DIM);
    paint_reason(24, COL2 + 18, CY0 + 142, COLW - 36, 16);
}

/* Erweitert: Diagnose-Zeile (Bezeichnung klein, Wert darunter). */
static void diag_row(const char *lbl, const char *val, COLORREF c, int y)
{
    label1(F_TINY, lbl, COL2 + 18, y, COLW - 36, 14, C_DIM);
    label1(F_SMALL, val, COL2 + 18, y + 14, COLW - 36, 20, c);
}

/* W178 COMM-BG: beide Editionen haben die Launcher-Karte (Hintergrundbild). */
#define ADV_DIAG_Y (CY0 + 224)

/* W172/P7: Datenort im Klartext (gleiche Quelle wie --userdata:
 * sb_userdata_mode + sb_userdata_path_peek). mode = Sprachschluessel. */
static void adv_data_loc(char *path, size_t cap, const char **mode)
{
    char p[1024];
    sb_ud_mode_t m = sb_userdata_mode();
    *mode = m == SB_UD_PROFILE    ? "gui.ud_profile"
            : m == SB_UD_PORTABLE ? "gui.ud_portable"
                                  : "gui.ud_legacy";
    if (sb_userdata_path_peek(SB_UD_ROOT, p, sizeof p) == 1) {
        u8_from_acp(p, path, cap);
    } else {
        strcpy_s(path, cap, g_client_disp);
    }
}

/* W172/P7: Ordner der Spielstaende wie der Client (hcfg.c: "HDD" im
 * Profil-/Portabel-Modus = <Datenordner>\hdd, sonst relativ zum Client). */
static void adv_hdd_path(char *out, size_t cap)
{
    const char *h = g_cur.hdd_root;
    char p[1024];
    if (sb_userdata_mode() != SB_UD_LEGACY && strcmp(h, "HDD") == 0 &&
        sb_userdata_path_peek(SB_UD_HDD, p, sizeof p) == 1) {
        u8_from_acp(p, out, cap);
    } else if ((h[0] && h[1] == ':') || h[0] == '\\' || h[0] == '/') {
        strcpy_s(out, cap, h);
    } else {
        _snprintf_s(out, cap, _TRUNCATE, "%s\\%s", g_client_disp, h[0] ? h : "HDD");
    }
}

static void paint_adv(void)
{
    char t[1400];
    /* Spieldateien (Ordner-Modus, Konzept 4.7) */
    card(CX0, CY0, COLW, 184, LT("gui.adv_files"));
    label1(F_TINY, LT("gui.adv_files_txt"), CX0 + 18, CY0 + 38, COLW - 36, 16, C_DIM);
    label1(F_TINY, LT("gui.adv_folder"), CX0 + 18, CY0 + 58, COLW - 36, 16, C_DIM);
    label1(F_TINY, LT("gui.adv_xbe"), CX0 + 18, CY0 + 108, COLW - 36, 16, C_DIM);
    if (!paint_reason(1, CX0 + 18, CY0 + 160, COLW - 36, 16)) {
        if (g_cur.media_root[0] && !g_data_ok) { /* Ordner gesetzt, aber ungueltig */
            label1(F_TINY, g_data_err, CX0 + 18, CY0 + 160, COLW - 36, 16, C_ERR);
        } else if (g_cur.media_root[0]) {
            label1(F_TINY, LT(g_rr[2].active ? "gui.adv_folder_ok" : "gui.adv_folder_ok2"),
                   CX0 + 18, CY0 + 160, COLW - 36, 16, C_OK);
        }
    }

    /* Spielstaende + Datenort */
    card(CX0, CY0 + 196, COLW, 112, LT("gui.adv_saves"));
    label1(F_SMALL, LT("gui.adv_hdd"), CX0 + 18, CY0 + 236, 84, 30, C_DIM);
    {
        char path[1024];
        const char *mk2;
        adv_data_loc(path, sizeof path, &mk2);
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.adv_data_fmt"), LT(mk2));
        label1(F_TINY, t, CX0 + 18, CY0 + 270, COLW - 36 - 118, 15, C_DIM);
        L_text(F_TINY, path, CX0 + 18, CY0 + 285, COLW - 36 - 118, 15, C_TEXT,
               DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_PATH_ELLIPSIS);
    }

    /* Gespeicherte Einstellungen (Glossar: "Saved setup") */
    card(CX0, CY0 + 320, COLW, 176, LT("gui.adv_setups"));
    if (!paint_reason(28, CX0 + 18, CY0 + 468, COLW - 36, 16)) {
        label1(F_TINY, g_prof_info, CX0 + 18, CY0 + 468, COLW - 36, 16, C_DIM);
    }

    card(COL2, CY0, COLW, 124, LT("gui.adv_finetune"));
    label1(F_SMALL, LT("gui.adv_vsync"), COL2 + 18, CY0 + 44, 150, 30, C_DIM);
    label1(F_SMALL, LT("gui.adv_fpslimit"), COL2 + 18, CY0 + 82, 150, 30, C_DIM);
    card(COL2, CY0 + 136, COLW, 76, LT("gui.adv_launcher"));

    card(COL2, ADV_DIAG_Y, COLW, CY0 + 496 - ADV_DIAG_Y, LT("gui.adv_diag"));
    if (!g_diag_open) {
        label(F_TINY, LT("gui.adv_diag_txt"), COL2 + 18, ADV_DIAG_Y + 44,
              COLW - 36, 32, C_FAINT);
        return;
    }
    diag_row(LT("gui.path_client_dir"), g_client_disp, C_TEXT, ADV_DIAG_Y + 42);
    diag_row(LT("gui.path_exe"),
             g_has_ldr ? LT("gui.path_found") : LT("gui.path_missing"),
             g_has_ldr ? C_OK : C_ERR, ADV_DIAG_Y + 78);
    if (sb_userdata_mode() != SB_UD_LEGACY) { /* W150 USER-DATA */
        lcfg_user_cfg_path(g_client_dir, t, sizeof t);
    } else {
        _snprintf_s(t, sizeof t, _TRUNCATE, "%s\\user.cfg", g_client_disp);
    }
    diag_row(LT("gui.path_usercfg"), t, C_TEXT, ADV_DIAG_Y + 114);
    diag_row(LT("gui.sum_settings"), g_profile_line, C_TEXT, ADV_DIAG_Y + 150);
    paint_reason(27, COL2 + 18, ADV_DIAG_Y + 218, COLW - 36, 16);
}

static void paint_packs(void); /* W148: unten definiert (Packs-Engine) */
static void compose(void)
{
    int W = SX(WIN_W), H = SX(WIN_H), i;
    if (!g_base.px || g_base.w != W || g_base.h != H) {
        base_build();
    }
    if (!cv_make(&g_bb, W, H) || !g_base.px) {
        return;
    }
    GdiFlush();
    memcpy(g_bb.px, g_base.px, (size_t)W * H * 4);
    switch (g_page) {
    case P_PLAY: paint_play(); break;
    case P_DISPLAY: paint_display(); break;
    case P_GFX: paint_gfx(); break;
    case P_PACKS: paint_packs(); break;
    case P_CTRL: paint_ctrl(); break;
    case P_AUDIO: paint_audio(); break;
    case P_ADV: paint_adv(); break;
    }
    /* Eingabefelder: Rahmen (Fokus = Akzent) */
    for (i = 0; i < g_ui_n; i++) {
        uictl_t *u = &g_ui[i];
        if (u->parent != g_hwnd || !ui_visible_on(u, g_page)) {
            continue;
        }
        if (u->kind == K_EDIT || u->kind == K_MEMO) {
            int en = IsWindowEnabled(u->h);
            L_fill(u->x, u->y, u->w, u->h_, C_INPUT, 255, 6);
            L_ring(u->x, u->y, u->w, u->h_,
                   g_focus_id == u->id ? C_ACCENT : C_INPUT_BRD,
                   en ? 255 : 120, 6);
        }
    }
    /* W171: Startsperre (Muster E) in der Fussleiste der Unterseiten:
     * Grund unter der Statuszeile, Statuszeile darueber. */
    {
        int lw = WIN_W - NAV_W - 28 - 330 - (g_undo_on ? 122 : 0);
        int blk = g_page != P_PLAY && launch_blocked();
        if (blk) {
            static const int kBlk[] = { 4, 3 };
            paint_reason(first_rule(kBlk, 2), NAV_W + 28, WIN_H - 22, lw, 16);
        }
    if (g_status[0]) {
        wchar_t *wt = u8_wdup(g_status);
        if (wt) {
            RECT r;
            int bw = SX(lw), th;
            int area = blk ? SX(BOT_H) - SX(22) : SX(BOT_H);
            r.left = 0;
            r.top = 0;
            r.right = bw;
            r.bottom = 0;
            SelectObject(g_bb.dc, g_f[F_SMALL]);
            DrawTextW(g_bb.dc, wt, -1, &r,
                      DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
            th = r.bottom - r.top;
            if (th > area - SX(8)) {
                th = area - SX(8);
            }
            cv_textw(&g_bb, g_f[F_SMALL], wt, -1, SX(NAV_W + 28),
                     SX(WIN_H - BOT_H) + (area - th) / 2, bw, th,
                     g_status_kind == 2   ? C_ERR
                     : g_status_kind == 1 ? C_OK
                                          : C_DIM,
                     DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
            free(wt);
        }
    }
    }
}

static void redraw_all(void)
{
    compose();
    if (g_hwnd) {
        RedrawWindow(g_hwnd, NULL, NULL,
                     RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }
}

/* Nur Hintergrund neu (Text-Aenderung ohne Elemente darueber). */
static void redraw_rect(int x, int y, int w, int h)
{
    RECT r;
    compose();
    r.left = SX(x);
    r.top = SX(y);
    r.right = SX(x + w);
    r.bottom = SX(y + h);
    if (g_hwnd) {
        InvalidateRect(g_hwnd, &r, FALSE);
    }
}

static void set_status_k(const char *msg, int kind)
{
    strcpy_s(g_status, sizeof g_status, msg ? msg : "");
    g_status_kind = kind;
    if (g_undo_on) { /* W171: neue Meldung beendet das Rueckgaengig-Angebot */
        HWND u;
        g_undo_on = 0;
        u = ctl(ID_UNDO);
        if (u) {
            ShowWindow(u, SW_HIDE);
        }
    }
    redraw_rect(NAV_W, WIN_H - BOT_H, WIN_W - NAV_W, BOT_H);
}

/* W171 Muster C: Meldung zum automatischen Umschalten + "Rueckgaengig"
 * (Knopf in der Fussleiste, Strg+Z). before = Stand vor dem Umschalten. */
static void set_status_undo(const char *msg, const launcher_settings_t *before)
{
    HWND u;
    set_status_k(msg, 0);
    g_undo_s = *before;
    g_undo_on = 1;
    u = ctl(ID_UNDO);
    if (u) {
        ShowWindow(u, SW_SHOWNA);
    }
    redraw_rect(NAV_W, WIN_H - BOT_H, WIN_W - NAV_W, BOT_H);
}

static void set_status(const char *msg)
{
    set_status_k(msg, 0);
}

/* ------------------------------------------------------------------ */
/* Eigene Knoepfe zeichnen                                             */
/* ------------------------------------------------------------------ */
static void draw_item(const DRAWITEMSTRUCT *d)
{
    uictl_t *u = ui_by_hwnd(d->hwndItem);
    canvas_t c;
    int w = d->rcItem.right - d->rcItem.left, h = d->rcItem.bottom - d->rcItem.top;
    int pressed = (d->itemState & ODS_SELECTED) != 0;
    int focus = (d->itemState & ODS_FOCUS) && !(d->itemState & ODS_NOFOCUSRECT);
    int dis = (d->itemState & ODS_DISABLED) != 0;
    wchar_t txt[256];
    int n;
    if (d->CtlType == ODT_LISTBOX) { /* W172: Packs-Liste */
        packs_draw_row(d);
        return;
    }
    memset(&c, 0, sizeof c);
    if (!u || !cv_make(&c, w, h)) {
        return;
    }
    /* Hintergrund: aus dem fertigen Bild (Hauptfenster) oder einfarbig */
    GdiFlush();
    if (u->parent == g_hwnd && g_bb.px) {
        POINT p = { 0, 0 };
        int y;
        MapWindowPoints(d->hwndItem, g_hwnd, &p, 1);
        for (y = 0; y < h; y++) {
            int sy = p.y + y;
            if (sy < 0 || sy >= g_bb.h || p.x < 0 || p.x + w > g_bb.w) {
                cv_fill(&c, 0, y, w, 1, C_BASE, 255, 0);
                continue;
            }
            memcpy(c.px + (size_t)y * w, g_bb.px + (size_t)sy * g_bb.w + p.x,
                   (size_t)w * 4);
        }
    } else {
        cv_fill(&c, 0, 0, w, h, C_DIALOG, 255, 0);
    }
    n = GetWindowTextW(d->hwndItem, txt, 256);
    switch (u->kind) {
    case K_NAV: {
        int sel = u->id - ID_NAV0 == g_page;
        if (sel) {
            cv_fill(&c, 0, 0, w, h, RGB(255, 255, 255), 22, SX(6));
            cv_fill(&c, 0, SX(9), SX(3), h - SX(18), C_ACCENT, 255, SX(1));
        } else if (u->hover) {
            cv_fill(&c, 0, 0, w, h, RGB(255, 255, 255), 12, SX(6));
        }
        if (g_has_icons) {
            wchar_t ic = kPageIcon[u->id - ID_NAV0];
            cv_textw(&c, g_f[F_ICON], &ic, 1, SX(14), 0, SX(24), h,
                     sel ? C_ACCENT_HI : C_DIM,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        cv_textw(&c, sel ? g_f[F_SEMI] : g_f[F_BODY], txt, n,
                 SX(g_has_icons ? 46 : 18), 0, w, h, sel ? C_TEXT : C_DIM,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        break;
    }
    case K_TBMIN:
    case K_TBCLOSE: {
        if (u->hover) {
            cv_fill(&c, 0, 0, w, h,
                    u->kind == K_TBCLOSE ? RGB(196, 43, 28) : RGB(255, 255, 255),
                    u->kind == K_TBCLOSE ? 255 : 26, 0);
        }
        if (g_has_icons) {
            wchar_t ic = u->kind == K_TBCLOSE ? 0xE8BB : 0xE921;
            cv_textw(&c, g_f[F_ICONSM], &ic, 1, 0, 0, w, h, C_TEXT,
                     DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            cv_textw(&c, g_f[F_BODY], u->kind == K_TBCLOSE ? L"x" : L"_", 1,
                     0, 0, w, h, C_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        break;
    }
    case K_TOGGLE: {
        int tw = SX(38), th = SX(20), ty = (h - th) / 2;
        COLORREF track = u->checked ? C_ACCENT : RGB(70, 77, 90);
        if (u->hover && !dis) {
            track = u->checked ? C_ACCENT_HI : RGB(84, 92, 106);
        }
        cv_fill(&c, 0, ty, tw, th, track, dis ? 110 : 255, th / 2);
        {
            int kd = th - SX(6);
            int kx = u->checked ? tw - SX(3) - kd : SX(3);
            cv_fill(&c, kx, ty + SX(3), kd, kd,
                    u->checked ? C_ON_ACCENT : RGB(230, 233, 238),
                    dis ? 140 : 255, kd / 2);
        }
        if (focus) {
            cv_ring(&c, 0, ty - SX(2), tw + SX(4), th + SX(4), C_ACCENT_HI,
                    200, th / 2 + SX(2), 1);
        }
        cv_textw(&c, g_f[F_BODY], txt, n, tw + SX(12), 0, w - tw - SX(12), h,
                 dis ? C_FAINT : C_TEXT,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        break;
    }
    case K_RADIO: { /* W173: Kreis + Titel, darunter Hilfe (gedimmt) */
        int d0 = SX(18), rx = SX(2), ry = SX(3), tx = d0 + SX(14), n1 = n;
        wchar_t *nl = wcschr(txt, L'\n');
        COLORREF ring = u->checked ? C_ACCENT : RGB(120, 128, 140);
        if (nl) {
            n1 = (int)(nl - txt);
        }
        if (u->hover && !dis) {
            ring = u->checked ? C_ACCENT_HI : C_DIM;
        }
        cv_ring(&c, rx, ry, d0, d0, ring, dis ? 110 : 255, d0 / 2, SX(2) < 2 ? 2 : SX(2));
        if (u->checked) {
            int di = d0 - SX(10);
            cv_fill(&c, rx + SX(5), ry + SX(5), di, di, C_ACCENT, dis ? 140 : 255, di / 2);
        }
        if (focus) {
            cv_ring(&c, 0, ry - SX(2), d0 + SX(4), d0 + SX(4), C_ACCENT_HI, 200,
                    d0 / 2 + SX(2), 1);
        }
        cv_textw(&c, g_f[u->checked ? F_SEMI : F_BODY], txt, n1, tx, 0, w - tx, SX(24),
                 dis ? C_FAINT : C_TEXT,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (nl) {
            cv_textw(&c, g_f[F_TINY], nl + 1, n - n1 - 1, tx, SX(21), w - tx, h - SX(21),
                     C_DIM, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
        }
        break;
    }
    default: { /* K_BTN, K_ACCENT, K_BIG, K_SEG (W172: gewaehlt = Akzent) */
        int acc = u->kind == K_ACCENT || u->kind == K_BIG ||
                  (u->kind == K_SEG && u->checked);
        COLORREF bg = acc ? C_ACCENT : C_BTN;
        COLORREF fg = acc ? C_ON_ACCENT : C_TEXT;
        int rad = SX(u->kind == K_BIG ? 10 : 6);
        if (!dis && u->hover) {
            bg = acc ? C_ACCENT_HI : C_BTN_HI;
        }
        if (!dis && pressed) {
            bg = acc ? C_ACCENT_LO : RGB(36, 40, 49);
        }
        if (dis) {
            bg = RGB(40, 44, 52);
            fg = C_FAINT;
        }
        cv_fill(&c, 0, 0, w, h, bg, u->parent == g_hwnd ? 240 : 255, rad);
        if (!acc) {
            cv_ring(&c, 0, 0, w, h, RGB(255, 255, 255), 22, rad, 1);
        }
        if (focus) {
            cv_ring(&c, 0, 0, w, h, acc ? RGB(255, 236, 200) : C_ACCENT, 255,
                    rad, SX(2) < 2 ? 2 : SX(2));
        }
        if (u->kind == K_BIG) {
            SetTextCharacterExtra(c.dc, SX(2));
        }
        if (u->id == ID_START_MENU) { /* W171: Pfeil "v" (Launch-Menue) */
            wchar_t ic = g_has_icons ? 0xE70D : L'v';
            cv_textw(&c, g_has_icons ? g_f[F_ICONSM] : g_f[F_SEMI], &ic, 1, 0,
                     0, w, h, fg, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SetTextCharacterExtra(c.dc, 0);
            break;
        }
        cv_textw(&c, u->kind == K_BIG ? g_f[F_BIG] : g_f[F_SEMI], txt, n, 0,
                 0, w, h, fg, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SetTextCharacterExtra(c.dc, 0);
        break;
    }
    }
    GdiFlush();
    BitBlt(d->hDC, d->rcItem.left, d->rcItem.top, w, h, c.dc, 0, 0, SRCCOPY);
    cv_free(&c);
}

/* Farben der System-Elemente (Edits, Listen, Combos) */
static HBRUSH g_br_input, g_br_dialog;

static LRESULT ctlcolor(HDC dc, HWND child, int is_static_bg)
{
    uictl_t *u = ui_by_hwnd(child);
    if (!g_br_input) {
        g_br_input = CreateSolidBrush(C_INPUT);
    }
    if (!g_br_dialog) {
        g_br_dialog = CreateSolidBrush(C_DIALOG);
    }
    if (u && u->kind == K_STATIC) {
        SetTextColor(dc, C_TEXT);
        SetBkColor(dc, C_DIALOG);
        return (LRESULT)g_br_dialog;
    }
    (void)is_static_bg;
    SetTextColor(dc, (u && !IsWindowEnabled(child)) ? C_FAINT : C_TEXT);
    SetBkColor(dc, C_INPUT);
    return (LRESULT)g_br_input;
}

/* ------------------------------------------------------------------ */
/* Einstellungen <-> Oberflaeche                                       */
/* ------------------------------------------------------------------ */
static void set_num(int id, unsigned v)
{
    char b[32];
    _snprintf_s(b, sizeof b, _TRUNCATE, "%u", v);
    ui_settext(ctl(id), b);
}

static unsigned get_num(int id)
{
    char b[32];
    ui_gettext(ctl(id), b, sizeof b);
    return (unsigned)atoi(b);
}

static void combo_add(HWND cb, const char *u8)
{
    wchar_t *w = u8_wdup(u8);
    SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)(w ? w : L""));
    free(w);
}

static void sync_preset(void)
{
    unsigned w = get_num(ID_W), h = get_num(ID_H);
    int i, sel = KPRESETS_N - 1;
    for (i = 0; i < KPRESETS_N - 1; i++) {
        if (kPresets[i].w == w && kPresets[i].h == h) {
            sel = i;
            break;
        }
    }
    g_updating++;
    ComboBox_SetCurSel(ctl(ID_PRESET), sel);
    g_updating--;
}

/* W107 EDGE-MODE: Prozent-Combo (Voreinstellungen + Roheintrag fuer Werte
 * von Hand in user.cfg, wie fs_hz). */
static void edge_pct_set(HWND c, const int *pre, int n, int v, const char *fmt)
{
    int k, sel = -1;
    while (ComboBox_GetCount(c) > n) {
        ComboBox_DeleteString(c, n);
    }
    for (k = 0; k < n; k++) {
        if (pre[k] == v) { sel = k; }
    }
    if (sel < 0) {
        char t[32];
        _snprintf_s(t, sizeof t, _TRUNCATE, fmt, v);
        combo_add(c, t);
        sel = n;
    }
    ComboBox_SetCurSel(c, sel);
}

static int edge_pct_get(HWND c, const int *pre, int n, int dflt)
{
    int sel = ComboBox_GetCurSel(c);
    if (sel >= 0 && sel < n) { return pre[sel]; }
    if (sel == n) {
        char t[64];
        const char *q;
        ui_gettext(c, t, sizeof t);
        for (q = t; *q && (*q < '0' || *q > '9'); q++) {}
        return atoi(q);
    }
    return dflt;
}

static void edge_from_settings(const launcher_settings_t *s)
{
    g_updating++;
    ComboBox_SetCurSel(ctl(ID_EDGE), (s->edge_mode >= 0 && s->edge_mode <= 2) ? s->edge_mode : 0);
    edge_pct_set(ctl(ID_EDGE_W), kEdgeW, KEDGEW_N, s->edge_fade_width, LT("gui.edge_w_fmt"));
    edge_pct_set(ctl(ID_EDGE_S), kEdgeS, KEDGES_N, s->edge_fade_strength, LT("gui.edge_s_fmt"));
    g_updating--; /* W171: Freigabe Verlauf = Regel R11/R12 (ui_apply_rules) */
}

static void edge_to_settings(launcher_settings_t *s)
{
    int sel = ComboBox_GetCurSel(ctl(ID_EDGE));
    s->edge_mode = (sel >= 0 && sel <= 2) ? sel : 0;
    s->edge_fade_width = edge_pct_get(ctl(ID_EDGE_W), kEdgeW, KEDGEW_N, 12);
    s->edge_fade_strength = edge_pct_get(ctl(ID_EDGE_S), kEdgeS, KEDGES_N, 100);
}

static void update_aspect_info(void)
{
    unsigned w = get_num(ID_W), h = get_num(ID_H), bw, bh;
    { /* W171 (R7): im Vollbild bestimmt der Monitor die Groesse -> Vorschau
       * und Text nach der Monitoraufloesung */
        int wm = ComboBox_GetCurSel(ctl(ID_WINMODE));
        unsigned mw = g_env_fix ? g_env_fix->mon_w : g_mon_w;
        unsigned mh = g_env_fix ? g_env_fix->mon_h : g_mon_h;
        if ((wm == 1 || wm == 2) && mw && mh) {
            w = mw;
            h = mh;
        }
    }
    aspect_box_for(w, h, &bw, &bh);
    g_prev_w = w;
    g_prev_h = h;
    if (bw == 0) {
        strcpy_s(g_aspect_info, sizeof g_aspect_info, LT("gui.aspect_bad"));
    } else if (bw == w && bh == h) {
        _snprintf_s(g_aspect_info, sizeof g_aspect_info, _TRUNCATE, LT("gui.aspect_43"),
                    w, h);
    } else if (bh == h) {
        _snprintf_s(g_aspect_info, sizeof g_aspect_info, _TRUNCATE, LT("gui.aspect_wide"),
                    w, h, bw, bh, (w - bw) / 2);
    } else {
        _snprintf_s(g_aspect_info, sizeof g_aspect_info, _TRUNCATE, LT("gui.aspect_tall"),
                    w, h, bw, bh, (h - bh) / 2);
    }
    { /* W107 EDGE-MODE: Vorschau + Hinweis */
        launcher_settings_t t;
        size_t n = strlen(g_aspect_info);
        edge_to_settings(&t);
        g_prev_edge = t.edge_mode;
        g_prev_ew = t.edge_fade_width;
        g_prev_es = t.edge_fade_strength;
        if (bw && (bw != w || bh != h) && t.edge_mode) {
            _snprintf_s(g_aspect_info + n, sizeof g_aspect_info - n, _TRUNCATE, " %s",
                        t.edge_mode == 1 ? LT("gui.aspect_cut")
                                         : LT("gui.aspect_fade"));
        }
    }
}

static void refresh_ctrl_profiles(const char *keep)
{
    HWND cb = ctl(ID_CTRL_PROF);
    char names[64][64], cur[64] = { 0 };
    int n, i, sel = 0;
    if (keep) {
        strcpy_s(cur, sizeof cur, keep);
    } else {
        int s = ComboBox_GetCurSel(cb);
        if (s > 0) {
            ui_gettext(cb, cur, sizeof cur);
        }
    }
    g_updating++;
    ComboBox_ResetContent(cb);
    combo_add(cb, LT("gui.legacy_label"));
    n = lctrl_input_profiles(g_client_dir, names, 64);
    for (i = 0; i < n; i++) {
        combo_add(cb, names[i]);
        if (cur[0] && strcmp(names[i], cur) == 0) {
            sel = i + 1;
        }
    }
    if (cur[0] && sel == 0) {
        combo_add(cb, cur); /* unbekannter Wert bleibt als Roheintrag */
        sel = n + 1;
    }
    ComboBox_SetCurSel(cb, sel);
    g_updating--;
}

static void ctrl_profile_from_ui(char *out, size_t cap)
{
    HWND cb = ctl(ID_CTRL_PROF);
    int sel = ComboBox_GetCurSel(cb);
    out[0] = '\0';
    if (sel <= 0) {
        return;
    }
    {
        wchar_t w[128];
        if (ComboBox_GetLBText(cb, sel, w) != CB_ERR) {
            w_to_u8(w, out, (int)cap);
        }
    }
    if (strcmp(out, LT("gui.legacy_label")) == 0) {
        out[0] = '\0';
    }
}

static void update_ctrl_overview(void)
{
    char prof[64], path[1200];
    HWND e = ctl(ID_KEYS);
    ctrl_profile_from_ui(prof, sizeof prof);
    if (!prof[0]) {
        ui_settext(e, LT("gui.keys_memo"));
        return;
    }
    if (lctrl_input_profile_path(g_client_dir, prof, path, sizeof path) != 0) {
        char b[256];
        _snprintf_s(b, sizeof b, _TRUNCATE, LT("gui.ctl_notname"),
                    prof);
        ui_settext(e, b);
        return;
    }
    {
        static char txt[12288];
        static char shown[sizeof txt * 2 + 1024];
        FILE *f = NULL;
        size_t n = 0, i, o;
        if (u8_fopen_s(&f, path, "rb") == 0 && f) {
            n = fread(txt, 1, sizeof txt - 1, f);
            fclose(f);
        }
        if (n == 0) {
            _snprintf_s(shown, sizeof shown, _TRUNCATE, LT("gui.ctl_notfound"),
                        prof);
            ui_settext(e, shown);
            return;
        }
        txt[n] = '\0';
        o = (size_t)_snprintf_s(
            shown, sizeof shown, _TRUNCATE, LT("gui.ctl_always"));
        for (i = 0; i < n && o + 3 < sizeof shown; i++) {
            if (txt[i] == '\n' && (i == 0 || txt[i - 1] != '\r')) {
                shown[o++] = '\r';
            }
            shown[o++] = txt[i];
        }
        shown[o] = '\0';
        {
            /* Profil-Dateien sind ASCII/ANSI: ANSI -> UTF-8 */
            static char u8[sizeof shown * 2];
            u8_from_acp(shown, u8, sizeof u8);
            ui_settext(e, u8);
        }
    }
}

/* W179 KEYS-HTML: Profil-Datei lesen (gemeinsame Quelle fuer die
 * Tastenuebersicht und den Browser-Export). Rueckgabe 1 = ok (n Bytes
 * in buf, NUL-terminiert), 0 = kein einfacher Name, -1 = fehlt/leer. */
static int ctrl_profile_load(const char *prof, char *buf, size_t cap,
                             size_t *n_out)
{
    char path[1200];
    FILE *f = NULL;
    size_t n = 0;
    if (!prof || !prof[0] || !buf || cap < 2) {
        return -1;
    }
    if (lctrl_input_profile_path(g_client_dir, prof, path, sizeof path) != 0) {
        return 0;
    }
    if (u8_fopen_s(&f, path, "rb") == 0 && f) {
        n = fread(buf, 1, cap - 1, f);
        fclose(f);
    }
    if (n == 0) {
        return -1;
    }
    buf[n] = '\0';
    if (n_out) {
        *n_out = n;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* W179 KEYS-HTML: Tastenuebersicht als lokale HTML-Datei              */
/* ------------------------------------------------------------------ */
/* Gleiche Quelle wie update_ctrl_overview (keine doppelte Pflege):
 * Profilname aus der UI, Profiltext aus profiles/<name>.ini, sonst das
 * Memo LT("gui.keys_memo"); Maus-Modus/Empfindlichkeit, Gamepad- und
 * Controller-Status live. Eine Datei mit eingebettetem CSS, keine
 * externen Verweise, Sprache = Launcher-Sprache, druckbar (hell).
 * Quelltext ASCII (U8_*), Ausgabe UTF-8. */

/* Abschnitte des Memos (feste Reihenfolge im HTML). */
enum { KH_MENUS, KH_START, KH_COMBAT, KH_MOVE, KH_COCKPIT, KH_MOUSEX,
       KH_N };

typedef struct {
    int sec;
    char key[96];
    char act[320];
} kh_row_t;

#define KH_MAXROWS 96
static kh_row_t kh_rows[KH_MAXROWS];
static int kh_n;
static char kh_footer[2048];

static void keyshtml_escape(const char *s, char *out, size_t cap)
{
    size_t o = 0;
    if (!s) {
        s = "";
    }
    if (!out || cap < 1) {
        return;
    }
    while (*s && o + 1 < cap) {
        const char *rep = NULL;
        size_t rl = 0;
        switch (*s) {
        case '&': rep = "&amp;"; rl = 5; break;
        case '<': rep = "&lt;"; rl = 4; break;
        case '>': rep = "&gt;"; rl = 4; break;
        case '"': rep = "&quot;"; rl = 6; break;
        default: break;
        }
        if (rep) {
            if (o + rl >= cap) {
                break;
            }
            memcpy(out + o, rep, rl);
            o += rl;
        } else {
            out[o++] = *s;
        }
        s++;
    }
    out[o] = '\0';
}

/* Anhaengen (still gekuerzt bei Platzmangel). */
static void keyshtml_add(char *out, size_t cap, size_t *o, const char *s)
{
    size_t n;
    if (!out || !o || !s || *o + 1 >= cap) {
        return;
    }
    n = strlen(s);
    if (*o + n >= cap) {
        n = cap - *o - 1;
    }
    memcpy(out + *o, s, n);
    *o += n;
    out[*o] = '\0';
}

static void keyshtml_addf(char *out, size_t cap, size_t *o, const char *fmt,
                          int v)
{
    char b[64];
    _snprintf_s(b, sizeof b, _TRUNCATE, fmt, v);
    keyshtml_add(out, cap, o, b);
}

/* Primaerfeuer-Zeile? (Memo: "primary fire" / "Primaerfeuer"; die
 * Suffixe sind ASCII, daher ohne UTF-8-Fallunterscheidung.) */
static int keyshtml_is_primary(const char *act)
{
    return act && (strstr(act, "mary fire") || strstr(act, "rfeuer")) != 0;
}

/* Cockpit-Zeile im Kampf-Block? (Chaff/Override, Nacht/FSS, Zoom, Mode;
 * Woerter in beiden Sprachen gleich.) */
static int keyshtml_is_cockpit(const char *act)
{
    return act && (strstr(act, "Chaff") || strstr(act, "FSS") ||
                   strstr(act, "Zoom") || strstr(act, "Mode")) != 0;
}

static char *keyshtml_trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t')) {
        *--e = '\0';
    }
    return s;
}

/* Ein Keycap (Inhalt schon escapen). */
static void keyshtml_cap(char *out, size_t cap, size_t *o, const char *t)
{
    char e[256];
    keyshtml_escape(t, e, sizeof e);
    keyshtml_add(out, cap, o, "<kbd>");
    keyshtml_add(out, cap, o, e);
    keyshtml_add(out, cap, o, "</kbd>");
}

/* Nur Einzelzeichen, je durch genau ein Leerzeichen getrennt?
 * ("1 2 3" ja, "Mouse left" nein.) */
static int keyshtml_all_single(const char *t)
{
    int n = 0;
    const char *p = t ? t : "";
    if (!*p) {
        return 0;
    }
    for (;;) {
        const char *sp = strchr(p, ' ');
        size_t len = sp ? (size_t)(sp - p) : strlen(p);
        if (len != 1) {
            return 0;
        }
        n++;
        if (!sp) {
            break;
        }
        p = sp + 1;
        if (!*p) {
            return 0;
        }
    }
    return n >= 2;
}

/* Taste(n) als Keycaps: "A / B" -> je ein Cap, "1 2 3" (nur
 * Einzelzeichen) -> je Zeichen ein Cap, sonst ein Cap. */
static void keyshtml_keys(char *out, size_t cap, size_t *o, const char *key)
{
    const char *p = key ? key : "";
    int first = 1;
    while (*p) {
        const char *sep = strstr(p, " / ");
        char part[128];
        size_t n = sep ? (size_t)(sep - p) : strlen(p);
        char *t, *q;
        int single = 1;
        if (n >= sizeof part) {
            n = sizeof part - 1;
        }
        memcpy(part, p, n);
        part[n] = '\0';
        t = keyshtml_trim(part);
        single = keyshtml_all_single(t);
        if (single) {
            /* "1 2 3 4 5": jedes Zeichen ein Cap */
            for (q = t; *q; q++) {
                if (*q != ' ') {
                    char c[2];
                    if (!first) {
                        keyshtml_add(out, cap, o, " ");
                    }
                    first = 0;
                    c[0] = *q;
                    c[1] = '\0';
                    keyshtml_cap(out, cap, o, c);
                }
            }
        } else if (*t) {
            char *plus;
            if (!first) {
                keyshtml_add(out, cap, o, " / ");
            }
            first = 0;
            /* "Shift+Arrows": je Summand ein Cap */
            while ((plus = strchr(t, '+')) != NULL) {
                *plus = '\0';
                keyshtml_cap(out, cap, o, keyshtml_trim(t));
                keyshtml_add(out, cap, o, "+");
                t = keyshtml_trim(plus + 1);
            }
            keyshtml_cap(out, cap, o, t);
        }
        if (!sep) {
            break;
        }
        p = sep + 3;
    }
}

static void keyshtml_row(char *out, size_t cap, size_t *o, const char *key,
                         const char *act)
{
    char ea[1600];
    if ((!key || !*key) && (!act || !*act)) {
        return;
    }
    keyshtml_add(out, cap, o,
                 keyshtml_is_primary(act) ? "<tr class=\"prim\">" : "<tr>");
    keyshtml_add(out, cap, o, "<td class=\"k\">");
    keyshtml_keys(out, cap, o, key);
    keyshtml_add(out, cap, o, "</td><td>");
    keyshtml_escape(act ? act : "", ea, sizeof ea);
    keyshtml_add(out, cap, o, ea);
    keyshtml_add(out, cap, o, "</td></tr>\n");
}

/* Memo in kh_rows/kh_footer zerlegen (gleiche Quelle wie das Memo). */
static void keyshtml_parse(const char *memo)
{
    static char work[16384];
    char *p;
    int sec = -1, blank = 0;
    kh_n = 0;
    kh_footer[0] = '\0';
    if (!memo) {
        return;
    }
    strcpy_s(work, sizeof work, memo);
    p = work;
    while (*p) {
        char *line = p, *tab;
        while (*p && *p != '\n' && *p != '\r') {
            p++;
        }
        if (*p == '\r') {
            *p++ = '\0';
            if (*p == '\n') {
                p++;
            }
        } else if (*p == '\n') {
            *p++ = '\0';
        }
        tab = strchr(line, '\t');
        if (!*keyshtml_trim(line)) {
            blank = 1;
            continue;
        }
        if (!tab) {
            /* Kopf oder Fussnote (Fuss: lang oder mit EJECT). */
            if (strstr(line, "EJECT") || strlen(line) > 100) {
                size_t fl = strlen(kh_footer);
                if (fl + strlen(line) + 2 < sizeof kh_footer) {
                    if (fl) {
                        strcpy_s(kh_footer + fl, sizeof kh_footer - fl, " ");
                        fl++;
                    }
                    strcpy_s(kh_footer + fl, sizeof kh_footer - fl, line);
                }
            } else if (strncmp(line, "MEN", 3) == 0) {
                sec = KH_MENUS;
            } else if (strstr(line, "MECH")) {
                sec = KH_START;
            } else if (strncmp(line, "COMBAT", 6) == 0 ||
                       strncmp(line, "KAMPF", 5) == 0) {
                sec = KH_COMBAT;
            } else if (strncmp(line, "DRIVING", 7) == 0 ||
                       strncmp(line, "FAHREN", 6) == 0) {
                sec = KH_MOVE;
            } else {
                size_t fl = strlen(kh_footer);
                if (fl + strlen(line) + 2 < sizeof kh_footer) {
                    if (fl) {
                        strcpy_s(kh_footer + fl, sizeof kh_footer - fl, " ");
                        fl++;
                    }
                    strcpy_s(kh_footer + fl, sizeof kh_footer - fl, line);
                }
            }
            blank = 0;
            continue;
        }
        *tab = '\0';
        if (kh_n < KH_MAXROWS) {
            int s = sec;
            if (blank) {
                s = KH_MOUSEX; /* kopflose Zeile (F10) -> Maus */
            } else if (s == KH_COMBAT &&
                       keyshtml_is_cockpit(keyshtml_trim(tab + 1))) {
                s = KH_COCKPIT;
            }
            if (s < 0) {
                s = KH_MOUSEX;
            }
            kh_rows[kh_n].sec = s;
            strcpy_s(kh_rows[kh_n].key, sizeof kh_rows[kh_n].key,
                     keyshtml_trim(line));
            strcpy_s(kh_rows[kh_n].act, sizeof kh_rows[kh_n].act,
                     keyshtml_trim(tab + 1));
            kh_n++;
        }
        blank = 0;
    }
}

/* Abschnitt mit Tabelle (nichts bei null Zeilen). */
static void keyshtml_table(char *out, size_t cap, size_t *o, const char *h,
                           int sec)
{
    int i, n = 0;
    for (i = 0; i < kh_n; i++) {
        n += kh_rows[i].sec == sec;
    }
    if (!n) {
        return;
    }
    keyshtml_add(out, cap, o, "<section><h2>");
    keyshtml_add(out, cap, o, h);
    keyshtml_add(out, cap, o, "</h2><table>\n");
    for (i = 0; i < kh_n; i++) {
        if (kh_rows[i].sec == sec) {
            keyshtml_row(out, cap, o, kh_rows[i].key, kh_rows[i].act);
        }
    }
    keyshtml_add(out, cap, o, "</table></section>\n");
}

/* Eingebettetes CSS (Launcher-Farbwelt, Keycaps, zwei Spalten,
 * Druck hell). Keine externen Verweise. */
static void keyshtml_css(char *out, size_t cap, size_t *o)
{
    static const char *const css =
        "*{box-sizing:border-box}\n"
        "body{margin:0;background:#0b0d11;color:#eaedf2;"
        "font-family:system-ui,\"Segoe UI\",SegoeUI,Roboto,Arial,sans-serif;"
        "line-height:1.5}\n"
        ".wrap{max-width:1240px;margin:0 auto;padding:24px 20px 40px}\n"
        "h1{font-size:26px;margin:0 0 4px;letter-spacing:.5px}\n"
        ".sub{color:#98a0ac;margin:0 0 16px;font-size:14px}\n"
        ".prim{background:#2a1f10;border:1px solid #e2963a;"
        "border-left-width:6px;border-radius:8px;padding:10px 14px;"
        "margin:0 0 20px}\n"
        ".prim-h{color:#f2a84e;font-size:12px;letter-spacing:1.5px;"
        "font-weight:700}\n"
        ".prim-k{font-size:17px;font-weight:600}\n"
        ".grid{display:grid;gap:14px;grid-template-columns:1fr}\n"
        "@media(min-width:900px){.grid{grid-template-columns:1fr 1fr}}\n"
        "section{background:#15181e;border:1px solid #2a2f3a;"
        "border-radius:10px;padding:12px 14px;break-inside:avoid}\n"
        "section.full{grid-column:1/-1}\n"
        "h2{font-size:15px;margin:0 0 8px;color:#f2a84e;letter-spacing:.4px}\n"
        "p{margin:0 0 8px;font-size:14px;color:#c9cfd9}\n"
        "table{width:100%;border-collapse:collapse;font-size:13.5px}\n"
        "td{padding:4px 6px;vertical-align:top;border-top:1px solid #242932}\n"
        "tr:first-child td{border-top:0}\n"
        "td.k{white-space:nowrap;width:38%}\n"
        "tr.prim td{background:#2a1f10}\n"
        "kbd{display:inline-block;min-width:22px;text-align:center;"
        "background:#1e222a;border:1px solid #3e4552;border-bottom-width:2px;"
        "border-radius:6px;padding:1px 7px;margin:1px 1px;"
        "font-family:Consolas,\"Courier New\",monospace;font-size:12.5px;"
        "color:#fff}\n"
        "pre{background:#0e1116;border:1px solid #2a2f3a;border-radius:8px;"
        "padding:10px 12px;font-size:12.5px;overflow-x:auto;"
        "white-space:pre-wrap;word-break:break-word;color:#d6dbe3}\n"
        "footer{margin-top:18px;color:#6c7480;font-size:12.5px}\n"
        "@media print{\n"
        "body{background:#fff;color:#111}\n"
        ".wrap{max-width:none}\n"
        ".prim{background:#fff7ec;border-color:#b26a1b}\n"
        ".prim-h{color:#8a4d00}\n"
        "section{background:#fff;border-color:#bbb}\n"
        "h2{color:#8a4d00}\n"
        "p{color:#333}\n"
        "td{border-top-color:#ddd}\n"
        "tr.prim td{background:#fff3e2}\n"
        "kbd{background:#f2f2f2;border-color:#999;color:#000}\n"
        "pre{background:#f7f7f7;border-color:#bbb;color:#222}\n"
        "footer{color:#555}\n"
        "}\n";
    keyshtml_add(out, cap, o, "<style>\n");
    keyshtml_add(out, cap, o, css);
    keyshtml_add(out, cap, o, "</style>\n");
}

/* ------------------------------------------------------------------ */
/* W182 KEYS-SVG: Original-Controller als schematische SVG-Zeichnung    */
/* ------------------------------------------------------------------ */
/* Eigene Zeichnung (keine Fotos, keine fremden Grafiken, keine Logos),
 * inline im HTML, ohne Verweise. Elemente/Bezeichnungen nach
 * CONTROLLER.md S8 (Labels bind.*). Belegung je Element aus derselben
 * Quelle wie W179: aktives Profil (Profiltext, lctrl_bind-Parser wie
 * der Belegungs-Dialog); Standardbelegung = eingebautes keyboard_default
 * (bitgenau Legacy; Selbsttest 54 vergleicht mit
 * harness/profiles/keyboard_default.ini). Tasten 1-5 rasten die fuenf
 * Kippschalter immer (Launcher-Start key_toggles=1, imap_toggles_apply).
 * Hover: CSS (:hover/:has), kein Skript. Quelltext ASCII, Ausgabe UTF-8. */

#define KS_LARR "\xE2\x86\x90"  /* Pfeil links */
#define KS_UARR "\xE2\x86\x91"  /* Pfeil hoch */
#define KS_RARR "\xE2\x86\x92"  /* Pfeil rechts */
#define KS_DARR "\xE2\x86\x93"  /* Pfeil runter */
#define KS_HARR "\xE2\x86\x94"  /* links-rechts */
#define KS_VARR "\xE2\x86\x95"  /* hoch-runter */
#define KS_TRI_U "\xE2\x96\xB2" /* Dreieck hoch */
#define KS_TRI_D "\xE2\x96\xBC" /* Dreieck runter */
#define KS_TRI_L "\xE2\x97\x80" /* Dreieck links */
#define KS_TRI_R "\xE2\x96\xB6" /* Dreieck rechts */
#define KS_EMDASH "\xE2\x80\x94" /* Geviertstrich = nicht belegt */

/* Standardbelegung (Legacy) = keyboard_default (input_map.c eingebaut). */
static const char ks_legacy_text[] =
    "[buttons]\n"
    "bi0 = KEY:SPACE, MOUSE:LEFT\n"
    "bi1 = KEY:F, MOUSE:RIGHT\n"
    "bi2 = KEY:L, MOUSE:MIDDLE\n"
    "bi4 = KEY:H\n"
    "bi5 = KEY:I\n"
    "bi6 = KEY:ENTER\n"
    "bi8 = KEY:M\n"
    "bi9 = KEY:TAB\n"
    "bi11 = KEY:Z\n"
    "bi12 = KEY:X\n"
    "bi13 = KEY:V\n"
    "bi18 = KEY:C\n"
    "bi20 = KEY:O\n"
    "bi21 = KEY:N\n"
    "bi27 = KEY:R\n"
    "[axes]\n"
    "aim_x.neg = KEY:LEFT:cond=!SHIFT\n"
    "aim_x.pos = KEY:RIGHT:cond=!SHIFT\n"
    "aim_x = MOUSE:X:mode=aim\n"
    "aim_y.neg = KEY:UP:cond=!SHIFT\n"
    "aim_y.pos = KEY:DOWN:cond=!SHIFT\n"
    "aim_y = MOUSE:Y:mode=aim\n"
    "rot.neg = KEY:A\n"
    "rot.pos = KEY:D\n"
    "sight_x.neg = KEY:LEFT\n"
    "sight_x.pos = KEY:RIGHT\n"
    "sight_x = MOUSE:X:mode=sight\n"
    "sight_y.neg = KEY:UP\n"
    "sight_y.pos = KEY:DOWN\n"
    "sight_y = MOUSE:Y:mode=sight\n"
    "pedal_l = KEY:LCTRL, MOUSE:X1, MOUSE:X2\n"
    "pedal_m = KEY:S\n"
    "pedal_r = KEY:W\n"
    "[gear]\n"
    "gear.up = KEY:E\n"
    "gear.down = KEY:Q\n"
    "tuner.up = KEY:T\n"
    "tuner.down = KEY:G\n"
    "[vr]\n"
    "vr_recenter = KEY:F8\n"
    "[client]\n"
    "pause = KEY:PAUSE\n";

/* Formen der Zeichnung. KS_LEDR/G/B = Taste mit roter/gruener/blauer
 * Leuchtleiste (3x3-Raster im Mittelblock). */
enum { KS_BTN, KS_TOG, KS_EJECT, KS_DIAL, KS_GEAR, KS_LEVER, KS_TRIG,
       KS_KNOB, KS_SIGHT, KS_SBTN, KS_PEDAL, KS_ROT, KS_LEDR, KS_LEDG,
       KS_LEDB };
/* Baugruppen (Reihenfolge der Tabelle). */
enum { KS_G_LEFT, KS_G_CENTER, KS_G_RIGHT, KS_G_PEDALS, KS_G_N };

typedef struct {
    const char *id;    /* CSS-Kennung: el-<id>, co-<id>, e-<id> */
    const char *lkey;  /* Name (Tabelle, Tooltip) */
    const char *skey;  /* Kurzname in der Zeichnung (NULL = lkey) */
    const char *elems; /* Profil-Elemente, je ein Leerzeichen getrennt */
    unsigned char shape, grp;
    short x, y, w, h;  /* Form (Knopf/Hebel: Rechteck; Kreis: Mitte+r) */
    short lx, ly, lw;  /* Callout-Kasten; lw = 0: Zeilen unter der Kappe */
    short ax, ay;      /* Leader-Start (0 = keiner) */
} ks_elem_t;

/* W184 KEYS-LAYOUT: physische Anordnung des Original-Controllers (eigene
 * Zeichnung; Lage der Bedienelemente nach oeffentlichen Beschreibungen
 * und einem Belegungsplan als Lagehinweis, nichts davon uebernommen).
 * Linker Block: Schaltkulisse 5..R mit Gang-Leuchten, Drehhebel in
 * waagrechter Kulisse mit Sicht-Stick (Daumen, druckbar) oben drauf,
 * Feld KIPPSCHALTER (2 + 3 schraeg versetzt). Mittelblock: COM1-5 oben
 * links, Tuner-Rad darunter, 3x3-Raster rechts (Leuchtleiste rot/gruen/
 * blau), unten Feld FUNKTION (FSS/Manipulator/Linienfarbe) und WAFFENWAHL
 * (Haupt/Sub/Magazin). Rechter Block: Zielhebel (Daumentaste Hauptwaffe +
 * Lock-On oben, Abzug vorn), Eject unter Schutzklappe oben rechts, Feld
 * MULTI-MONITOR (2x3, untere Reihe Hauptmonitor-Zoom), senkrechte Reihe
 * Luke/Zuendung/START. Pedale: Slide-Step, Bremse, Gas.
 * Zellen (lw = 0): Hover-Flaeche x-3,y-3,w+6,h+40; Name auf der Kappe,
 * Belegung darunter. Selbsttest 55 prueft Bloecke + Ueberlappung. */
#define KS_VIEWBOX "0 0 1320 964"
#define KS_CO_H 62 /* Hoehe Callout-Kasten */
#define KS_C(id, lk, sk, el, sh, g, x, y, w, h) \
    { id, lk, sk, el, sh, g, x, y, w, h, 0, 0, 0, 0, 0 }
/* Mittelblock: COM-Reihe (5 x 40, Schritt 46) und 3x3-Raster (72/78). */
#define KS_COM(id, c) \
    KS_C(id, "bind." id, NULL, id, KS_BTN, KS_G_CENTER, 428 + (c) * 46, 76, \
         40, 30)
#define KS_LED(id, sk, sh, c, r) \
    KS_C(id, "bind." id, sk, id, sh, KS_G_CENTER, 668 + (c) * 78, \
         76 + (r) * 80, 72, 26)
#define KS_ROW(id, c, y) \
    KS_C(id, "bind." id, NULL, id, KS_BTN, KS_G_CENTER, 434 + (c) * 152, y, \
         140, 24)
#define KS_MM(id, sk, c, r) \
    KS_C(id, "bind." id, sk, id, KS_BTN, KS_G_RIGHT, 936 + (c) * 116, \
         384 + (r) * 80, 108, 24)
#define KS_TG(id, sk, x, y) \
    KS_C(id, "bind." id, sk, id, KS_TOG, KS_G_LEFT, x, y, 88, 24)

static const ks_elem_t ks_elems[] = {
    /* Linker Block */
    { "gear", "gui.svg_gear", NULL,
      "gear.up gear.down gear.r gear.n gear.1 gear.2 gear.3 gear.4 gear.5",
      KS_GEAR, KS_G_LEFT, 26, 92, 116, 360, 24, 470, 120, 84, 452 },
    { "rot", "gui.svg_rot", NULL, "rot rot.neg rot.pos",
      KS_ROT, KS_G_LEFT, 176, 196, 204, 16, 160, 68, 114, 252, 182 },
    { "sight", "gui.svg_sight", NULL,
      "sight_x sight_y sight_x.neg sight_x.pos sight_y.neg sight_y.pos",
      KS_SIGHT, KS_G_LEFT, 278, 204, 14, 0, 282, 250, 114, 284, 217 },
    { "bi33", "bind.bi33", NULL, "bi33",
      KS_SBTN, KS_G_LEFT, 278, 204, 6, 0, 282, 68, 114, 281, 199 },
    KS_TG("bi34", "gui.svg_t_filter", 158, 552),
    KS_TG("bi35", "gui.svg_t_oxygen", 194, 472),
    KS_TG("bi36", "gui.svg_t_fuel", 254, 552),
    KS_TG("bi37", "gui.svg_t_buffer", 288, 472),
    KS_TG("bi38", "gui.svg_t_vtloc", 310, 392),
    /* Mittelblock */
    KS_COM("bi28", 0), KS_COM("bi29", 1), KS_COM("bi30", 2),
    KS_COM("bi31", 3), KS_COM("bi32", 4),
    { "tuner", "gui.svg_tuner", NULL, "tuner.up tuner.down",
      KS_DIAL, KS_G_CENTER, 540, 250, 58, 0, 480, 344, 120, 540, 312 },
    KS_LED("bi16", NULL, KS_LEDR, 0, 0),
    KS_LED("bi17", "gui.svg_s_ext", KS_LEDR, 1, 0),
    KS_LED("bi18", NULL, KS_LEDR, 2, 0),
    KS_LED("bi19", NULL, KS_LEDG, 0, 1),
    KS_LED("bi20", NULL, KS_LEDG, 1, 1),
    KS_LED("bi21", NULL, KS_LEDG, 2, 1),
    KS_LED("bi22", NULL, KS_LEDB, 0, 2),
    KS_LED("bi23", NULL, KS_LEDB, 1, 2),
    KS_LED("bi24", NULL, KS_LEDB, 2, 2),
    KS_ROW("bi13", 0, 456), KS_ROW("bi14", 1, 456), KS_ROW("bi15", 2, 456),
    KS_ROW("bi25", 0, 540), KS_ROW("bi26", 1, 540), KS_ROW("bi27", 2, 540),
    /* Rechter Block: Zielhebel. bi0 (Report "MainWeapon") = grosse
     * Daumentaste oben, bi1 ("Fire", Sub-Waffe) = Abzug vorn, bi2 =
     * Lock-On oben (Quellen uneinig, s. BERICHT_M2 W184). */
    { "aim", "gui.svg_aim", NULL,
      "aim_x aim_y aim_x.neg aim_x.pos aim_y.neg aim_y.pos",
      KS_LEVER, KS_G_RIGHT, 1046, 104, 66, 220, 926, 236, 96, 1046, 200 },
    { "bi0", "bind.bi0", NULL, "bi0",
      KS_TRIG, KS_G_RIGHT, 1080, 112, 22, 18, 1124, 76, 96, 1102, 121 },
    { "bi1", "bind.bi1", NULL, "bi1",
      KS_TRIG, KS_G_RIGHT, 1030, 146, 16, 44, 926, 150, 96, 1030, 168 },
    { "bi2", "bind.bi2", NULL, "bi2",
      KS_KNOB, KS_G_RIGHT, 1062, 121, 8, 0, 926, 76, 96, 1054, 121 },
    { "bi3", "bind.bi3", "gui.svg_eject", "bi3",
      KS_EJECT, KS_G_RIGHT, 1250, 84, 36, 40, 1206, 152, 94, 1268, 136 },
    KS_MM("bi7", "gui.svg_s_open", 0, 0), KS_MM("bi8", NULL, 1, 0),
    KS_MM("bi9", NULL, 0, 1), KS_MM("bi10", NULL, 1, 1),
    KS_MM("bi11", NULL, 0, 2), KS_MM("bi12", NULL, 1, 2),
    KS_C("bi4", "bind.bi4", NULL, "bi4", KS_BTN, KS_G_RIGHT, 1192, 380, 100,
         34),
    KS_C("bi5", "bind.bi5", NULL, "bi5", KS_BTN, KS_G_RIGHT, 1192, 464, 100,
         34),
    KS_C("bi6", "bind.bi6", NULL, "bi6", KS_BTN, KS_G_RIGHT, 1192, 548, 100,
         34),
    /* Pedale: Slide-Step links, Bremse Mitte, Gas rechts */
    { "pedal_l", "gui.svg_pedal_l", NULL, "pedal_l",
      KS_PEDAL, KS_G_PEDALS, 452, 716, 84, 124, 420, 868, 150, 0, 0 },
    { "pedal_m", "gui.svg_pedal_m", NULL, "pedal_m",
      KS_PEDAL, KS_G_PEDALS, 590, 738, 140, 102, 586, 868, 150, 0, 0 },
    { "pedal_r", "gui.svg_pedal_r", NULL, "pedal_r",
      KS_PEDAL, KS_G_PEDALS, 780, 704, 84, 136, 752, 868, 150, 0, 0 },
};
#define KS_N ((int)(sizeof ks_elems / sizeof ks_elems[0]))

static const char *const ks_grp_key[KS_G_N] = {
    "gui.svg_left", "gui.svg_center", "gui.svg_right", "gui.svg_pedals",
};

/* Baugruppen-Rahmen x, y, w, h (Reihenfolge wie KS_G_*); Beschriftung
 * oben mittig. */
static const short ks_blk[KS_G_N][4] = {
    { 12, 30, 392, 610 }, { 414, 30, 492, 610 }, { 916, 30, 392, 610 },
    { 390, 660, 540, 292 },
};

/* Felder innerhalb der Bloecke: Rahmen (w = 0: nur Beschriftung) und
 * Beschriftung (lkey NULL: keine) an tx/ty. */
typedef struct {
    short x, y, w, h;
    const char *lkey;
    short tx, ty;
} ks_field_t;

static const ks_field_t ks_fields[] = {
    { 152, 330, 246, 296, "gui.svg_f_tog", 162, 348 },
    { 424, 426, 472, 194, "gui.svg_f_func", 436, 448 },
    { 0, 0, 0, 0, "gui.svg_f_wpn", 436, 532 },
    { 926, 356, 246, 262, "gui.svg_f_mm", 936, 374 },
    { 0, 0, 0, 0, "gui.svg_f_zoom", 936, 537 },
    { 1182, 356, 120, 262, NULL, 0, 0 },
};
#define KS_NF ((int)(sizeof ks_fields / sizeof ks_fields[0]))

/* Quellen je Callout-Zeile/Tabellenspalte. SBC-Quellen (W159) laufen
 * als "SBC:<Name>" in der Tastatur-Zeile mit. */
enum { KS_SRC_KEY, KS_SRC_MOUSE, KS_SRC_PAD, KS_SRC_N };
#define KS_TOKN 24

typedef struct {
    char t[KS_SRC_N][KS_TOKN][64];
    int n[KS_SRC_N];
} ks_toks_t;

static void ks_printf(char *out, size_t cap, size_t *o, const char *fmt, ...)
{
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(b, sizeof b, _TRUNCATE, fmt, ap);
    va_end(ap);
    keyshtml_add(out, cap, o, b);
}

/* Zeichen (Codepoints) eines UTF-8-Textes. */
static int ks_cps(const char *s)
{
    int n = 0;
    for (; s && *s; s++) {
        n += ((unsigned char)*s & 0xC0) != 0x80;
    }
    return n;
}

static void ks_tok_add(ks_toks_t *k, int src, const char *s)
{
    int i;
    if (src < 0 || src >= KS_SRC_N || !s || !*s) {
        return;
    }
    for (i = 0; i < k->n[src]; i++) {
        if (strcmp(k->t[src][i], s) == 0) {
            return; /* doppelt (z. B. Pfeile fuer X und Sicht) */
        }
    }
    if (k->n[src] < KS_TOKN) {
        strncpy_s(k->t[src][k->n[src]], sizeof k->t[src][0], s, _TRUNCATE);
        k->n[src]++;
    }
}

/* "LS <->" + "LS ^v" -> "LS" (beide Achsen derselben Quelle). */
static void ks_tok_merge(ks_toks_t *k)
{
    static const char hs[] = " " KS_HARR, vs[] = " " KS_VARR;
    int s, i, j;
    for (s = 0; s < KS_SRC_N; s++) {
        for (i = 0; i < k->n[s]; i++) {
            size_t n = strlen(k->t[s][i]);
            if (n <= sizeof hs - 1 ||
                strcmp(k->t[s][i] + n - (sizeof hs - 1), hs) != 0) {
                continue;
            }
            for (j = 0; j < k->n[s]; j++) {
                size_t m = strlen(k->t[s][j]);
                if (j == i || m != n ||
                    strncmp(k->t[s][j], k->t[s][i], n - (sizeof hs - 1)) ||
                    strcmp(k->t[s][j] + m - (sizeof vs - 1), vs) != 0) {
                    continue;
                }
                k->t[s][i][n - (sizeof hs - 1)] = '\0';
                memmove(k->t[s][j], k->t[s][j + 1],
                        (size_t)(k->n[s] - j - 1) * sizeof k->t[s][0]);
                k->n[s]--;
                if (j < i) {
                    i--;
                }
                break;
            }
        }
    }
}

/* Funktions-Praefix je Profil-Element (mehrteilige Elemente). */
static const char *ks_prefix(const char *key)
{
    static const struct { const char *k, *p; } t[] = {
        { "gear.up", KS_TRI_U }, { "gear.down", KS_TRI_D },
        { "gear.r", "R:" }, { "gear.n", "N:" }, { "gear.1", "1:" },
        { "gear.2", "2:" }, { "gear.3", "3:" }, { "gear.4", "4:" },
        { "gear.5", "5:" }, { "tuner.up", KS_TRI_U },
        { "tuner.down", KS_TRI_D }, { "rot.neg", KS_TRI_L },
        { "rot.pos", KS_TRI_R },
    };
    int i;
    for (i = 0; i < (int)(sizeof t / sizeof t[0]); i++) {
        if (strcmp(key, t[i].k) == 0) {
            return t[i].p;
        }
    }
    return "";
}

/* Eine Quelle ("KEY:LEFT:cond=!SHIFT", "PAD:LX:dz=0.15") lesbar machen;
 * *src = Gruppe. Parameter fallen weg (cond=SHIFT -> "Shift+"). */
static void ks_atom(const char *a, char *out, size_t cap, int *src)
{
    static const struct { const char *n, *t; } keys[] = {
        { "LEFT", KS_LARR }, { "RIGHT", KS_RARR }, { "UP", KS_UARR },
        { "DOWN", KS_DARR }, { "LCTRL", "LCtrl" }, { "RCTRL", "RCtrl" },
        { "CTRL", "Ctrl" }, { "SHIFT", "Shift" }, { "ALT", "Alt" },
        { "ESC", "Esc" },
    }, pads[] = {
        { "LX", "LS " KS_HARR }, { "LY", "LS " KS_VARR },
        { "RX", "RS " KS_HARR }, { "RY", "RS " KS_VARR },
        { "DU", "D" KS_UARR }, { "DUP", "D" KS_UARR },
        { "DD", "D" KS_DARR }, { "DDOWN", "D" KS_DARR },
        { "DL", "D" KS_LARR }, { "DLEFT", "D" KS_LARR },
        { "DR", "D" KS_RARR }, { "DRIGHT", "D" KS_RARR },
    };
    char ty[16], nm[64];
    const char *c1, *c2, *par = "";
    size_t n;
    int i;
    *src = KS_SRC_KEY;
    out[0] = '\0';
    c1 = strchr(a, ':');
    if (!c1 || (size_t)(c1 - a) >= sizeof ty) {
        strncpy_s(out, cap, a, _TRUNCATE); /* unbekannte Form: roh */
        return;
    }
    memcpy(ty, a, (size_t)(c1 - a));
    ty[c1 - a] = '\0';
    c2 = strchr(c1 + 1, ':');
    n = c2 ? (size_t)(c2 - c1 - 1) : strlen(c1 + 1);
    if (n >= sizeof nm) {
        n = sizeof nm - 1;
    }
    memcpy(nm, c1 + 1, n);
    nm[n] = '\0';
    if (c2) {
        par = c2;
    }
    if (_stricmp(ty, "MOUSE") == 0) {
        static const struct { const char *n, *k; } ms[] = {
            { "LEFT", "gui.svg_m_left" }, { "RIGHT", "gui.svg_m_right" },
            { "MIDDLE", "gui.svg_m_middle" }, { "X1", "gui.svg_m_x1" },
            { "X2", "gui.svg_m_x2" },
        };
        *src = KS_SRC_MOUSE;
        if (_stricmp(nm, "X") == 0 || _stricmp(nm, "Y") == 0) {
            _snprintf_s(out, cap, _TRUNCATE, "%s %s", LT("gui.svg_mouse"),
                        _stricmp(nm, "X") == 0 ? KS_HARR : KS_VARR);
            return;
        }
        for (i = 0; i < (int)(sizeof ms / sizeof ms[0]); i++) {
            if (_stricmp(nm, ms[i].n) == 0) {
                strncpy_s(out, cap, LT(ms[i].k), _TRUNCATE);
                return;
            }
        }
        _snprintf_s(out, cap, _TRUNCATE, "%s %s", LT("gui.svg_mouse"), nm);
        return;
    }
    if (_stricmp(ty, "PAD") == 0) {
        *src = KS_SRC_PAD;
        for (i = 0; i < (int)(sizeof pads / sizeof pads[0]); i++) {
            if (_stricmp(nm, pads[i].n) == 0) {
                strncpy_s(out, cap, pads[i].t, _TRUNCATE);
                return;
            }
        }
        strncpy_s(out, cap, nm, _TRUNCATE);
        return;
    }
    if (_stricmp(ty, "KEY") == 0) {
        const char *t = nm;
        for (i = 0; i < (int)(sizeof keys / sizeof keys[0]); i++) {
            if (_stricmp(nm, keys[i].n) == 0) {
                t = keys[i].t;
                break;
            }
        }
        _snprintf_s(out, cap, _TRUNCATE, "%s%s",
                    (strstr(par, "cond=SHIFT") || strstr(par, "cond=shift"))
                        ? "Shift+" : "", t);
        return;
    }
    if (_stricmp(ty, "SBC") == 0) {
        _snprintf_s(out, cap, _TRUNCATE, "SBC:%s", nm);
        return;
    }
    strncpy_s(out, cap, a, _TRUNCATE);
}

/* Alternative (Akkord "A+B" erlaubt) -> Text, Gruppe des ersten Atoms. */
static void ks_alt(const char *alt, char *out, size_t cap, int *src)
{
    char work[128], *p, *plus;
    size_t o = 0;
    int first = 1;
    out[0] = '\0';
    *src = KS_SRC_KEY;
    strncpy_s(work, sizeof work, alt, _TRUNCATE);
    p = work;
    for (;;) {
        char t[64];
        int s;
        plus = strchr(p, '+');
        if (plus) {
            *plus = '\0';
        }
        ks_atom(keyshtml_trim(p), t, sizeof t, &s);
        if (first) {
            *src = s;
        } else {
            keyshtml_add(out, cap, &o, "+");
        }
        first = 0;
        keyshtml_add(out, cap, &o, t);
        if (!plus) {
            break;
        }
        p = plus + 1;
    }
}

/* Belegung eines gezeichneten Elements einsammeln. */
static void ks_collect(const lctrl_bind_t *b, const ks_elem_t *e,
                       ks_toks_t *k)
{
    char names[160], *nm, *ctx = NULL;
    memset(k, 0, sizeof *k);
    strncpy_s(names, sizeof names, e->elems, _TRUNCATE);
    for (nm = strtok_s(names, " ", &ctx); nm; nm = strtok_s(NULL, " ", &ctx)) {
        int idx = lctrl_bind_elem_index(nm), a;
        const char *pre = ks_prefix(nm);
        if (idx < 0) {
            continue;
        }
        for (a = 0; a < b->row[idx].n_alt; a++) {
            char t[64], u[80];
            int s;
            ks_alt(b->row[idx].alt[a], t, sizeof t, &s);
            _snprintf_s(u, sizeof u, _TRUNCATE, "%s%s", pre, t);
            ks_tok_add(k, s, u);
        }
    }
    /* Tasten 1-5 rasten die Kippschalter immer (key_toggles=1). */
    if (strncmp(e->id, "bi", 2) == 0) {
        int bi = atoi(e->id + 2);
        if (bi >= 34 && bi <= 38) {
            char t[4];
            t[0] = (char)('1' + bi - 34);
            t[1] = '\0';
            ks_tok_add(k, KS_SRC_KEY, t);
        }
    }
    ks_tok_merge(k);
}

static const char *ks_name(const ks_elem_t *e, int shortname)
{
    return LT(shortname && e->skey ? e->skey : e->lkey);
}

/* Eine Callout-Zeile (Quellen-Maske; leer = Geviertstrich). Zu lang ->
 * textLength (Schrift staucht, Kasten bleibt). */
static void ks_svg_line(char *out, size_t cap, size_t *o, int x, int y,
                        int mid, int avail, const ks_toks_t *k, int mask)
{
    static const char *const cls[KS_SRC_N] = { "tk", "tm", "tp" };
    char buf[8192];
    size_t bo = 0;
    int cps = 0, first = 1, s, i;
    buf[0] = '\0';
    for (s = 0; s < KS_SRC_N; s++) {
        if (!(mask & (1 << s))) {
            continue;
        }
        for (i = 0; i < k->n[s]; i++) {
            char e[256];
            if (!first) {
                keyshtml_add(buf, sizeof buf, &bo,
                             "<tspan class=\"sep\"> " U8_DOT " </tspan>");
                cps += 3;
            }
            first = 0;
            keyshtml_escape(k->t[s][i], e, sizeof e);
            ks_printf(buf, sizeof buf, &bo, "<tspan class=\"%s\">%s</tspan>",
                      cls[s], e);
            cps += ks_cps(k->t[s][i]);
        }
    }
    if (first) {
        keyshtml_add(buf, sizeof buf, &bo,
                     "<tspan class=\"un\">" KS_EMDASH "</tspan>");
        cps = 1;
    }
    ks_printf(out, cap, o, "<text class=\"cl\" x=\"%d\" y=\"%d\"%s", x, y,
              mid ? " text-anchor=\"middle\"" : "");
    if (cps * 70 > avail * 10) { /* Halbfett 12: ~7 je Zeichen */
        ks_printf(out, cap, o,
                  " textLength=\"%d\" lengthAdjust=\"spacingAndGlyphs\"",
                  avail);
    }
    keyshtml_add(out, cap, o, ">");
    keyshtml_add(out, cap, o, buf);
    keyshtml_add(out, cap, o, "</text>");
}

/* Name (proportional, ~6.6 je Zeichen bei 11.5). */
static void ks_svg_name(char *out, size_t cap, size_t *o, int x, int y,
                        int mid, int avail, const char *name)
{
    char e[512];
    keyshtml_escape(name, e, sizeof e);
    ks_printf(out, cap, o, "<text class=\"nm\" x=\"%d\" y=\"%d\"%s", x, y,
              mid ? " text-anchor=\"middle\"" : "");
    if (ks_cps(name) * 66 > avail * 10) {
        ks_printf(out, cap, o,
                  " textLength=\"%d\" lengthAdjust=\"spacingAndGlyphs\"",
                  avail);
    }
    keyshtml_add(out, cap, o, ">");
    keyshtml_add(out, cap, o, e);
    keyshtml_add(out, cap, o, "</text>");
}

/* Form zeichnen (Klassen: sh = hervorhebbare Flaeche). */
static void ks_svg_shape(char *out, size_t cap, size_t *o, const ks_elem_t *e)
{
    int x = e->x, y = e->y, w = e->w, h = e->h, i;
    switch (e->shape) {
    case KS_TOG:
        ks_printf(out, cap, o,
                  "<rect class=\"sh\" x=\"%d\" y=\"%d\" width=\"%d\" "
                  "height=\"%d\" rx=\"5\"/>"
                  "<rect class=\"ic\" x=\"%d\" y=\"%d\" width=\"9\" "
                  "height=\"16\" rx=\"3\"/>"
                  "<line class=\"ic\" x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\"/>",
                  x, y, w, h, x + 6, y + 4, x + 10, y + 12, x + 14, y + 3);
        break;
    case KS_LEDR: /* Taste mit Leuchtleiste (rot/gruen/blau) */
    case KS_LEDG:
    case KS_LEDB:
        ks_printf(out, cap, o,
                  "<rect class=\"sh\" x=\"%d\" y=\"%d\" width=\"%d\" "
                  "height=\"%d\" rx=\"5\"/>"
                  "<rect class=\"%s\" x=\"%d\" y=\"%d\" width=\"%d\" "
                  "height=\"3\" rx=\"1.5\"/>",
                  x, y, w, h,
                  e->shape == KS_LEDR ? "ldr"
                  : e->shape == KS_LEDG ? "ldg" : "ldb",
                  x + 8, y + 4, w - 16);
        break;
    case KS_EJECT: /* Schutzklappe mit Warnstreifen, Scharnier oben, Kappe */
        ks_printf(out, cap, o,
                  "<rect class=\"hzf\" x=\"%d\" y=\"%d\" width=\"%d\" "
                  "height=\"%d\" rx=\"6\"/>"
                  "<line class=\"cvh\" x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\"/>"
                  "<rect class=\"sh ej\" x=\"%d\" y=\"%d\" width=\"%d\" "
                  "height=\"%d\" rx=\"5\"/>",
                  x - 12, y - 12, w + 24, h + 24, x - 4, y - 12, x + w + 4,
                  y - 12, x, y, w, h);
        break;
    case KS_DIAL: { /* Tuner-Rad (Mitte x/y, Radius w): 13 Rasten von 9 Uhr
                     * im Uhrzeigersinn bis 6 Uhr (CONTROLLER.md S3.4) */
        double r1 = w + 5, r2 = w + 13;
        ks_printf(out, cap, o,
                  "<circle class=\"bs\" cx=\"%d\" cy=\"%d\" r=\"%d\"/>"
                  "<circle class=\"sh\" cx=\"%d\" cy=\"%d\" r=\"%d\"/>"
                  "<circle class=\"ic\" cx=\"%d\" cy=\"%d\" r=\"%d\"/>",
                  x, y, w + 16, x, y, w, x, y, w * 62 / 100);
        for (i = 0; i <= 12; i++) {
            double a = i * 3.14159265358979 / 8.0;
            double dx = -cos(a), dy = -sin(a);
            ks_printf(out, cap, o,
                      "<line class=\"ic\" x1=\"%d\" y1=\"%d\" x2=\"%d\" "
                      "y2=\"%d\"/>",
                      x + (int)lround(dx * r1), y + (int)lround(dy * r1),
                      x + (int)lround(dx * r2), y + (int)lround(dy * r2));
        }
        ks_printf(out, cap, o,
                  "<line class=\"ic\" x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\"/>",
                  x, y - w * 30 / 100, x, y - w * 90 / 100);
        break;
    }
    case KS_GEAR: { /* Kulisse senkrecht (oben 5 ... unten R), rechts davon
                     * die Gang-Leuchten mit Ziffern, Knauf auf N */
        static const char *const pos[7] = { "5", "4", "3", "2", "1", "N",
                                            "R" };
        int sx = x + 18, st = y + 14, sb = y + h - 14;
        ks_printf(out, cap, o,
                  "<rect class=\"sh\" x=\"%d\" y=\"%d\" width=\"%d\" "
                  "height=\"%d\" rx=\"12\"/>"
                  "<rect class=\"bs\" x=\"%d\" y=\"%d\" width=\"22\" "
                  "height=\"%d\" rx=\"11\"/>",
                  x, y, w, h, sx, st, sb - st);
        for (i = 0; i < 7; i++) {
            int py = st + 16 + i * (sb - st - 32) / 6;
            ks_printf(out, cap, o,
                      "<line class=\"ic\" x1=\"%d\" y1=\"%d\" x2=\"%d\" "
                      "y2=\"%d\"/>"
                      "<rect class=\"ic\" x=\"%d\" y=\"%d\" width=\"20\" "
                      "height=\"12\" rx=\"2\"/>"
                      "<text class=\"gl\" x=\"%d\" y=\"%d\">%s</text>",
                      sx + 26, py, sx + 34, py, x + 58, py - 6, x + 96,
                      py + 4, pos[i]);
            if (i == 5) {
                ks_printf(out, cap, o,
                          "<circle class=\"sh kn\" cx=\"%d\" cy=\"%d\" "
                          "r=\"12\"/>",
                          sx + 11, py);
            }
        }
        break;
    }
    case KS_ROT: { /* Drehhebel von oben: waagrechte Kulisse, Griffkopf
                    * in der Mitte (traegt den Sicht-Stick) */
        int cx = x + w / 2, cy = y + h / 2;
        ks_printf(out, cap, o,
                  "<rect class=\"bs\" x=\"%d\" y=\"%d\" width=\"%d\" "
                  "height=\"%d\" rx=\"%d\"/>"
                  "<text class=\"gl\" x=\"%d\" y=\"%d\">" KS_TRI_L "</text>"
                  "<text class=\"gl\" x=\"%d\" y=\"%d\">" KS_TRI_R "</text>"
                  "<rect class=\"sh\" x=\"%d\" y=\"%d\" width=\"64\" "
                  "height=\"46\" rx=\"14\"/>",
                  x, y, w, h, h / 2, x + 14, cy + 4, x + w - 14, cy + 4,
                  cx - 32, cy - 23);
        break;
    }
    case KS_LEVER: { /* Griff, Schaft, Sockel (Seitenansicht) */
        int cx = x + w / 2, gh = h * 55 / 100, sh = h * 36 / 100;
        ks_printf(out, cap, o,
                  "<ellipse class=\"bs\" cx=\"%d\" cy=\"%d\" rx=\"%d\" "
                  "ry=\"13\"/>"
                  "<rect class=\"bs\" x=\"%d\" y=\"%d\" width=\"16\" "
                  "height=\"%d\" rx=\"4\"/>"
                  "<rect class=\"sh\" x=\"%d\" y=\"%d\" width=\"%d\" "
                  "height=\"%d\" rx=\"%d\"/>",
                  cx, y + gh + sh + 6, w * 78 / 100, cx - 8, y + gh, sh,
                  x, y, w, gh, w * 38 / 100);
        break;
    }
    case KS_TRIG:
        ks_printf(out, cap, o,
                  "<rect class=\"sh\" x=\"%d\" y=\"%d\" width=\"%d\" "
                  "height=\"%d\" rx=\"%d\"/>",
                  x, y, w, h, w < h ? 6 : 4);
        break;
    case KS_KNOB:
    case KS_SBTN:
        ks_printf(out, cap, o,
                  "<circle class=\"sh\" cx=\"%d\" cy=\"%d\" r=\"%d\"/>", x, y,
                  w);
        break;
    case KS_SIGHT: /* Sicht-Stick von oben: Kopf mit vier Richtungsmarken */
        ks_printf(out, cap, o,
                  "<circle class=\"sh\" cx=\"%d\" cy=\"%d\" r=\"%d\"/>"
                  "<path class=\"ic\" d=\"M%d %dv4M%d %dv-4M%d %dh4M%d %dh-4\"/>",
                  x, y, w, x, y - w + 2, x, y + w - 2, x - w + 2, y,
                  x + w - 2, y);
        break;
    case KS_PEDAL:
        ks_printf(out, cap, o,
                  "<rect class=\"sh\" x=\"%d\" y=\"%d\" width=\"%d\" "
                  "height=\"%d\" rx=\"10\"/>",
                  x, y, w, h);
        for (i = 1; i <= 4; i++) {
            ks_printf(out, cap, o,
                      "<line class=\"ic\" x1=\"%d\" y1=\"%d\" x2=\"%d\" "
                      "y2=\"%d\"/>",
                      x + 10, y + i * h / 5, x + w - 10, y + i * h / 5);
        }
        break;
    default: /* KS_BTN */
        ks_printf(out, cap, o,
                  "<rect class=\"sh\" x=\"%d\" y=\"%d\" width=\"%d\" "
                  "height=\"%d\" rx=\"5\"/>",
                  x, y, w, h);
        break;
    }
}

/* Masken der belegten Zeilen (kompakt, leer = eine Strich-Zeile). */
static int ks_masks(const ks_toks_t *k, int cell, int m[3])
{
    int n = 0;
    if (cell) {
        if (k->n[KS_SRC_KEY] || k->n[KS_SRC_MOUSE]) {
            m[n++] = (1 << KS_SRC_KEY) | (1 << KS_SRC_MOUSE);
        }
        if (k->n[KS_SRC_PAD]) {
            m[n++] = 1 << KS_SRC_PAD;
        }
    } else {
        int s;
        for (s = 0; s < KS_SRC_N; s++) {
            if (k->n[s]) {
                m[n++] = 1 << s;
            }
        }
    }
    if (!n) {
        m[n++] = 0;
    }
    return n;
}

/* Hover-Kopplung Element <-> Callout <-> Tabellenzeile (nur CSS). */
static void ks_css(char *out, size_t cap, size_t *o)
{
    static const char *const css =
        ".ctl{margin:0 0 20px}\n"
        ".ctl svg{display:block;width:100%;height:auto;margin:6px 0 4px;"
        "font-family:system-ui,\"Segoe UI\",Roboto,Arial,sans-serif}\n"
        ".ctl .bd{fill:#15181e;stroke:#343a46;stroke-width:2}\n"
        ".ctl .bdc{fill:#6c7480;font-size:12px;font-weight:700;"
        "letter-spacing:2px;text-anchor:middle}\n"
        ".ctl .sh{fill:#1e222a;stroke:#4a5260;stroke-width:1.5}\n"
        ".ctl .bs{fill:#191c22;stroke:#3a404c;stroke-width:1.2}\n"
        ".ctl .ic{fill:none;stroke:#98a0ac;stroke-width:1.5;"
        "stroke-linecap:round}\n"
        ".ctl .sl{fill:none;stroke:#5a6270;stroke-width:5;"
        "stroke-linecap:round}\n"
        ".ctl .kn{fill:#2b303a}\n"
        ".ctl .gl{fill:#98a0ac;font-size:12px;font-weight:700;"
        "text-anchor:middle}\n"
        ".ctl .ej{fill:#4a1a1a;stroke:#d9534f}\n"
        ".ctl .cv{fill:none;stroke:#d9534f;stroke-width:1.2;"
        "stroke-dasharray:4 3}\n"
        ".ctl .cvh{stroke:#d9534f;stroke-width:3;stroke-linecap:round}\n"
        ".ctl .hzf{fill:url(#ks-hz);stroke:#d9534f;stroke-width:1.2}\n"
        ".ctl .hz{fill:#7a6418}\n.ctl .hzb{fill:#1a1c20}\n"
        ".ctl .ldr{fill:#e0524c}\n.ctl .ldg{fill:#5cc15a}\n"
        ".ctl .ldb{fill:#3d9be0}\n"
        ".ctl .fd{fill:none;stroke:#3a414d;stroke-width:1.2}\n"
        ".ctl .fdc{fill:#7d8592;font-size:10px;font-weight:700;"
        "letter-spacing:1.5px}\n"
        ".ctl .lb{fill:#11141a;stroke:#2f3541;stroke-width:1}\n"
        ".ctl .lbc{fill:transparent;stroke:none}\n"
        ".ctl .ld{fill:none;stroke:#6c7480;stroke-width:1;"
        "stroke-dasharray:3 3}\n"
        ".ctl .nm{fill:#eaedf2;font-size:11.5px;font-weight:600}\n"
        ".ctl .cl{font-size:12px;font-weight:600}\n"
        ".ctl svg .tk{fill:#ffffff}\n.ctl svg .tm{fill:#7cc4ff}\n"
        ".ctl svg .tp{fill:#8fdc8a}\n"
        ".ctl svg .un,.ctl svg .sep{fill:#5c6470}\n"
        ".ctl .pf .sh{fill:#3a2810;stroke:#e2963a;stroke-width:2}\n"
        ".ctl .pf .lb{stroke:#e2963a}\n"
        ".ctl .el:hover .sh{stroke:#f2a84e;stroke-width:2.6}\n"
        ".ctl .el:hover .lb{stroke:#f2a84e;fill:#2a1f10}\n"
        ".lg{display:flex;flex-wrap:wrap;gap:6px 18px;margin:0 0 6px;"
        "font-size:13px;color:#c9cfd9}\n"
        ".lg .sw{display:inline-block;width:12px;height:12px;"
        "border-radius:3px;margin-right:6px;vertical-align:-1px;"
        "border:1px solid #3e4552}\n"
        ".sw.kb{background:#fff}.sw.ms{background:#7cc4ff}"
        ".sw.pd{background:#8fdc8a}.sw.pf{background:#e2963a}"
        ".sw.un{background:#5c6470}\n"
        ".note{font-size:12.5px;color:#98a0ac}\n"
        "table.et th{text-align:left;font-size:12px;color:#98a0ac;"
        "padding:4px 6px;font-weight:600}\n"
        "table.et tr.grp th{color:#f2a84e;padding-top:10px;"
        "letter-spacing:.4px}\n"
        "table.et td:first-child{width:30%}\n"
        "table.et .tm,table.et .tp{display:inline-block;margin:1px 2px;"
        "padding:0 6px;border-radius:6px;font-size:12.5px;"
        "font-family:Consolas,\"Courier New\",monospace}\n"
        "table.et .tm{color:#7cc4ff;border:1px solid #2c4a66}\n"
        "table.et .tp{color:#8fdc8a;border:1px solid #2f5230}\n"
        "table.et .un{color:#5c6470}\n"
        "table.et tr:hover td{background:#1f2430}\n"
        "@media print{\n"
        ".ctl svg{break-inside:avoid}\n"
        ".ctl .bd{fill:#f6f6f6;stroke:#999}\n.ctl .bdc{fill:#555}\n"
        ".ctl .sh{fill:#fff;stroke:#444}\n.ctl .bs{fill:#eee;stroke:#888}\n"
        ".ctl .ic{stroke:#555}\n.ctl .sl{stroke:#888}\n.ctl .kn{fill:#ddd}\n"
        ".ctl .gl{fill:#333}\n"
        ".ctl .ej{fill:#fde8e8;stroke:#b22}\n.ctl .cv,.ctl .cvh{stroke:#b22}\n"
        ".ctl .hzf{stroke:#b22}\n.ctl .hz{fill:#e8c440}\n"
        ".ctl .hzb{fill:#fff}\n"
        ".ctl .ldr{fill:#c62828}\n.ctl .ldg{fill:#2e7d32}\n"
        ".ctl .ldb{fill:#1565c0}\n"
        ".ctl .fd{stroke:#aaa}\n.ctl .fdc{fill:#555}\n"
        ".ctl .lb{fill:#fff;stroke:#bbb}\n.ctl .lbc{fill:none;stroke:none}\n"
        ".ctl .ld{stroke:#999}\n.ctl .nm{fill:#111}\n"
        ".ctl svg .tk{fill:#000}\n.ctl svg .tm{fill:#0a58a8}\n"
        ".ctl svg .tp{fill:#1d7a1d}\n"
        ".ctl svg .un,.ctl svg .sep{fill:#888}\n"
        ".ctl .pf .sh{fill:#fff3e2;stroke:#b26a1b}\n"
        ".ctl .pf .lb{stroke:#b26a1b}\n"
        ".lg{color:#333}\n.note{color:#555}\n"
        "table.et .tm{color:#0a58a8;border-color:#9bb8d6}\n"
        "table.et .tp{color:#1d7a1d;border-color:#a8cfa6}\n"
        "table.et tr.grp th{color:#8a4d00}\n"
        "}\n";
    int i, r;
    keyshtml_add(out, cap, o, "<style>\n");
    keyshtml_add(out, cap, o, css);
    /* Kopplung per :has (Element/Zeile ueberfahren -> beide markiert). */
    for (r = 0; r < 3; r++) {
        for (i = 0; i < KS_N; i++) {
            ks_printf(out, cap, o, "%sbody:has(.e-%s:hover) .e-%s%s",
                      i ? "," : "", ks_elems[i].id, ks_elems[i].id,
                      r == 0 ? " .sh" : r == 1 ? " .lb" : " td");
        }
        keyshtml_add(out, cap, o,
                     r == 0 ? "{stroke:#f2a84e;stroke-width:2.6}\n"
                     : r == 1 ? "{stroke:#f2a84e;fill:#2a1f10}\n"
                              : "{background:#2a1f10}\n");
    }
    keyshtml_add(out, cap, o, "</style>\n");
}

/* Tokens einer Quelle als Tabellenzelle. */
static void ks_td(char *out, size_t cap, size_t *o, const ks_toks_t *k,
                  int s)
{
    int i;
    keyshtml_add(out, cap, o, "<td>");
    if (!k->n[s]) {
        keyshtml_add(out, cap, o, "<span class=\"un\">" KS_EMDASH "</span>");
    }
    for (i = 0; i < k->n[s]; i++) {
        if (s == KS_SRC_KEY) {
            if (i) {
                keyshtml_add(out, cap, o, " ");
            }
            keyshtml_cap(out, cap, o, k->t[s][i]);
        } else {
            char e[256];
            keyshtml_escape(k->t[s][i], e, sizeof e);
            ks_printf(out, cap, o, "<span class=\"%s\">%s</span>",
                      s == KS_SRC_MOUSE ? "tm" : "tp", e);
        }
    }
    keyshtml_add(out, cap, o, "</td>");
}

/* Abschnitt "Controller und Belegung": Legende, SVG, Hinweis, Tabelle.
 * prof_text NULL bei Standardbelegung (has_prof = 0) oder fehlendem
 * Profil (dann keyboard_default eingebaut, sonst alles unbelegt). */
static void keysvg_build(const char *profile, const char *prof_text,
                         char *out, size_t cap, size_t *o)
{
    static lctrl_bind_t b;
    static ks_toks_t tok[sizeof ks_elems / sizeof ks_elems[0]];
    int i, g, has_prof = profile && profile[0];
    if (!has_prof || (!prof_text && strcmp(profile, "keyboard_default") == 0)) {
        lctrl_bind_load_text(ks_legacy_text, &b);
    } else {
        lctrl_bind_load_text(prof_text ? prof_text : "", &b);
    }
    for (i = 0; i < KS_N; i++) {
        ks_collect(&b, &ks_elems[i], &tok[i]);
    }
    keyshtml_add(out, cap, o, "<section class=\"ctl\"><h2>");
    keyshtml_add(out, cap, o, LT("gui.svg_h"));
    keyshtml_add(out, cap, o, "</h2>\n<div class=\"lg\">");
    ks_printf(out, cap, o, "<span><span class=\"sw kb\"></span>%s</span>",
              LT("gui.svg_kbd"));
    ks_printf(out, cap, o, "<span><span class=\"sw ms\"></span>%s</span>",
              LT("gui.svg_mouse"));
    ks_printf(out, cap, o, "<span><span class=\"sw pd\"></span>%s</span>",
              LT("gui.svg_pad"));
    ks_printf(out, cap, o, "<span><span class=\"sw pf\"></span>%s</span>",
              LT("gui.ctl_prim"));
    ks_printf(out, cap, o,
              "<span><span class=\"sw un\"></span>" KS_EMDASH " %s</span>",
              LT("gui.svg_unset"));
    keyshtml_add(out, cap, o, "</div>\n");
    /* Zeichnung (kein xmlns noetig: inline-SVG in HTML) */
    keyshtml_add(out, cap, o,
                 "<svg viewBox=\"" KS_VIEWBOX "\" role=\"img\" aria-label=\"");
    {
        char e[256];
        keyshtml_escape(LT("gui.svg_h"), e, sizeof e);
        keyshtml_add(out, cap, o, e);
    }
    /* Warnstreifen der Eject-Klappe (lokales Muster, kein Verweis) */
    keyshtml_add(out, cap, o, "\">\n<defs><pattern id=\"ks-hz\" "
                 "width=\"10\" height=\"10\" patternUnits=\"userSpaceOnUse\" "
                 "patternTransform=\"rotate(45)\"><rect class=\"hzb\" "
                 "width=\"10\" height=\"10\"/><rect class=\"hz\" width=\"5\" "
                 "height=\"10\"/></pattern></defs>\n");
    for (g = 0; g < KS_G_N; g++) {
        char e[256];
        ks_printf(out, cap, o,
                  "<rect class=\"bd\" x=\"%d\" y=\"%d\" width=\"%d\" "
                  "height=\"%d\" rx=\"%d\"/>\n",
                  ks_blk[g][0], ks_blk[g][1], ks_blk[g][2], ks_blk[g][3],
                  g == KS_G_CENTER ? 18 : g == KS_G_PEDALS ? 20 : 26);
        keyshtml_escape(LT(ks_grp_key[g]), e, sizeof e);
        ks_printf(out, cap, o,
                  "<text class=\"bdc\" x=\"%d\" y=\"%d\">%s</text>\n",
                  ks_blk[g][0] + ks_blk[g][2] / 2, ks_blk[g][1] + 22, e);
    }
    /* Felder (Rahmen + Beschriftung) und Sockelplatte des Zielhebels */
    for (i = 0; i < KS_NF; i++) {
        const ks_field_t *f = &ks_fields[i];
        if (f->w) {
            ks_printf(out, cap, o,
                      "<rect class=\"fd\" x=\"%d\" y=\"%d\" width=\"%d\" "
                      "height=\"%d\" rx=\"8\"/>",
                      f->x, f->y, f->w, f->h);
        }
        if (f->lkey) {
            char e[256];
            keyshtml_escape(LT(f->lkey), e, sizeof e);
            ks_printf(out, cap, o,
                      "<text class=\"fdc\" x=\"%d\" y=\"%d\">%s</text>",
                      f->tx, f->ty, e);
        }
        keyshtml_add(out, cap, o, "\n");
    }
    keyshtml_add(out, cap, o,
                 "<rect class=\"bs\" x=\"1009\" y=\"288\" width=\"140\" "
                 "height=\"44\" rx=\"8\"/>\n");
    for (i = 0; i < KS_N; i++) {
        const ks_elem_t *e = &ks_elems[i];
        const ks_toks_t *k = &tok[i];
        int cell = e->lw == 0, m[3], nl, l, s, j, any = 0;
        char t[512];
        nl = ks_masks(k, cell, m);
        ks_printf(out, cap, o, "<g class=\"el e-%s%s\" id=\"el-%s\"><title>",
                  e->id, strcmp(e->id, "bi0") == 0 ? " pf" : "", e->id);
        keyshtml_escape(ks_name(e, 0), t, sizeof t);
        keyshtml_add(out, cap, o, t);
        if (e->shape == KS_EJECT) {
            keyshtml_add(out, cap, o, " (");
            keyshtml_escape(LT("gui.svg_cover"), t, sizeof t);
            keyshtml_add(out, cap, o, t);
            keyshtml_add(out, cap, o, ")");
        }
        keyshtml_add(out, cap, o, ": ");
        for (s = 0; s < KS_SRC_N; s++) {
            for (j = 0; j < k->n[s]; j++) {
                keyshtml_escape(k->t[s][j], t, sizeof t);
                keyshtml_add(out, cap, o, any ? ", " : "");
                keyshtml_add(out, cap, o, t);
                any = 1;
            }
        }
        if (!any) {
            keyshtml_add(out, cap, o, KS_EMDASH);
        }
        keyshtml_add(out, cap, o, "</title>");
        if (cell) {
            /* Hintergrund (Hover-Flaeche), Kappe mit Name, Zeilen darunter */
            int nx = e->x + e->w / 2, off = e->shape == KS_TOG ? 9 : 0;
            ks_printf(out, cap, o,
                      "<rect class=\"lb lbc\" x=\"%d\" y=\"%d\" width=\"%d\" "
                      "height=\"%d\" rx=\"6\"/>",
                      e->x - 3, e->y - 3, e->w + 6, e->h + 40);
            ks_svg_shape(out, cap, o, e);
            ks_printf(out, cap, o, "<g class=\"co\" id=\"co-%s\">", e->id);
            ks_svg_name(out, cap, o, nx + off,
                        e->y + e->h / 2 + 4 + (e->shape >= KS_LEDR ? 2 : 0),
                        1, e->w - 12 - 2 * off, ks_name(e, 1));
            for (l = 0; l < nl; l++) {
                ks_svg_line(out, cap, o, nx, e->y + e->h + 14 + l * 14, 1,
                            e->w, k, m[l]);
            }
            keyshtml_add(out, cap, o, "</g>");
        } else {
            ks_svg_shape(out, cap, o, e);
            if (e->ax) {
                int px = e->ax < e->lx ? e->lx
                         : e->ax > e->lx + e->lw ? e->lx + e->lw : e->ax;
                int py = e->ay < e->ly ? e->ly
                         : e->ay > e->ly + KS_CO_H ? e->ly + KS_CO_H : e->ay;
                ks_printf(out, cap, o,
                          "<line class=\"ld\" x1=\"%d\" y1=\"%d\" x2=\"%d\" "
                          "y2=\"%d\"/>",
                          e->ax, e->ay, px, py);
            }
            ks_printf(out, cap, o,
                      "<g class=\"co\" id=\"co-%s\"><rect class=\"lb\" "
                      "x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" rx=\"7\"/>",
                      e->id, e->lx, e->ly, e->lw, KS_CO_H);
            ks_svg_name(out, cap, o, e->lx + 6, e->ly + 15, 0, e->lw - 12,
                        ks_name(e, 1));
            for (l = 0; l < nl; l++) {
                ks_svg_line(out, cap, o, e->lx + 6, e->ly + 30 + l * 13, 0,
                            e->lw - 12, k, m[l]);
            }
            keyshtml_add(out, cap, o, "</g>");
        }
        keyshtml_add(out, cap, o, "</g>\n");
    }
    keyshtml_add(out, cap, o, "</svg>\n<p class=\"note\">");
    keyshtml_add(out, cap, o, LT("gui.svg_note"));
    keyshtml_add(out, cap, o, " ");
    keyshtml_add(out, cap, o, LT("gui.svg_tog"));
    keyshtml_add(out, cap, o, "</p>\n");
    /* Tabelle je Element (gleiche Reihenfolge, Hover gekoppelt) */
    ks_printf(out, cap, o,
              "<table class=\"et\"><tr><th>%s</th><th>%s</th><th>%s</th>"
              "<th>%s</th></tr>\n",
              LT("gui.svg_elem"), LT("gui.svg_kbd"), LT("gui.svg_mouse"),
              LT("gui.svg_pad"));
    for (g = 0; g < KS_G_N; g++) {
        char e[256];
        keyshtml_escape(LT(ks_grp_key[g]), e, sizeof e);
        ks_printf(out, cap, o,
                  "<tr class=\"grp\"><th colspan=\"4\">%s</th></tr>\n", e);
        for (i = 0; i < KS_N; i++) {
            if (ks_elems[i].grp != g) {
                continue;
            }
            ks_printf(out, cap, o, "<tr class=\"e-%s%s\"><td>", ks_elems[i].id,
                      strcmp(ks_elems[i].id, "bi0") == 0 ? " prim" : "");
            keyshtml_escape(ks_name(&ks_elems[i], 0), e, sizeof e);
            keyshtml_add(out, cap, o, e);
            keyshtml_add(out, cap, o, "</td>");
            ks_td(out, cap, o, &tok[i], KS_SRC_KEY);
            ks_td(out, cap, o, &tok[i], KS_SRC_MOUSE);
            ks_td(out, cap, o, &tok[i], KS_SRC_PAD);
            keyshtml_add(out, cap, o, "</tr>\n");
        }
    }
    keyshtml_add(out, cap, o, "</table></section>\n");
}

/* W182: Fixture-Profil (eigener Text, keine Spieldaten) fuer Selbsttest
 * 54 und die Beispiel-HTML (Tastatur + Maus + Gamepad gemischt). */
static const char ks_fixture_text[] =
    "# W182 Fixture (keine Spieldaten)\n"
    "[buttons]\n"
    "bi0 = KEY:SPACE, MOUSE:LEFT, PAD:RB\n"
    "bi1 = KEY:F, MOUSE:RIGHT, PAD:B\n"
    "bi2 = KEY:L, MOUSE:MIDDLE, PAD:LB\n"
    "bi3 = PAD:BACK+PAD:Y\n"
    "bi4 = KEY:H, PAD:BACK+PAD:X\n"
    "bi5 = KEY:I, PAD:BACK+PAD:A\n"
    "bi6 = KEY:ENTER, PAD:START\n"
    "bi8 = KEY:M\n"
    "bi9 = KEY:TAB\n"
    "bi10 = PAD:DL, PAD:DR\n"
    "bi11 = KEY:Z\n"
    "bi12 = KEY:X\n"
    "bi13 = KEY:V\n"
    "bi17 = PAD:L3\n"
    "bi18 = KEY:C, PAD:A\n"
    "bi20 = KEY:O, PAD:BACK+PAD:B\n"
    "bi21 = KEY:N\n"
    "bi25 = PAD:Y\n"
    "bi26 = PAD:X\n"
    "bi27 = KEY:R, PAD:LB+PAD:RB\n"
    "bi28 = KEY:F5\n"
    "bi33 = PAD:R3\n"
    "bi34 = PAD:BACK+PAD:DL\n"
    "bi35 = PAD:BACK+PAD:DR\n"
    "[axes]\n"
    "aim_x.neg = KEY:LEFT:cond=!SHIFT\n"
    "aim_x.pos = KEY:RIGHT:cond=!SHIFT\n"
    "aim_y.neg = KEY:UP:cond=!SHIFT\n"
    "aim_y.pos = KEY:DOWN:cond=!SHIFT\n"
    "aim_x = MOUSE:X:mode=aim, PAD:LX:dz=0.15:curve=1.5\n"
    "aim_y = MOUSE:Y:mode=aim, PAD:LY:dz=0.15:inv=1\n"
    "rot.neg = KEY:A\n"
    "rot.pos = KEY:D\n"
    "rot = PAD:RX:dz=0.12\n"
    "sight_x.neg = KEY:LEFT:cond=SHIFT\n"
    "sight_x.pos = KEY:RIGHT:cond=SHIFT\n"
    "sight_y = PAD:RY:inv=1\n"
    "pedal_l = KEY:LCTRL, MOUSE:X1, PAD:BACK+PAD:LT\n"
    "pedal_m = KEY:S, PAD:LT:dz=0.05\n"
    "pedal_r = KEY:W, PAD:RT:dz=0.05\n"
    "[gear]\n"
    "gear.up = KEY:E, PAD:BACK+PAD:RB\n"
    "gear.down = KEY:Q, PAD:BACK+PAD:LB\n"
    "gear.n = KEY:0\n"
    "tuner.up = KEY:T, PAD:DU\n"
    "tuner.down = KEY:G, PAD:DD\n";

/* W182 Selbsttest: Callout-Gruppe co-<id> (bis zum naechsten </g>). */
static int ks_co(const char *html, const char *id, char *buf, size_t cap)
{
    char pat[64];
    const char *a, *e;
    size_t n;
    buf[0] = '\0';
    _snprintf_s(pat, sizeof pat, _TRUNCATE, "id=\"co-%s\"", id);
    a = strstr(html, pat);
    if (!a) {
        return 0;
    }
    e = strstr(a, "</g>");
    if (!e) {
        return 0;
    }
    n = (size_t)(e - a);
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(buf, a, n);
    buf[n] = '\0';
    return 1;
}

/* W182 Selbsttest: Struktur pruefen (prof = Profil- statt Standard-
 * Variante). 1 = ok, sonst Grund in why. */
static int ks_selfcheck(const char *html, int prof, char *why, size_t cap)
{
    static char co[8192];
    const char *sv = strstr(html, "<svg viewBox=\"" KS_VIEWBOX "\"");
    const char *se = sv ? strstr(sv, "</svg>") : NULL;
    size_t hl = strlen(html);
    int i;
    if (!sv || !se) {
        strcpy_s(why, cap, "SVG fehlt");
        return 0;
    }
    if (hl < 8 || strcmp(html + hl - 8, "</html>\n") != 0) {
        strcpy_s(why, cap, "HTML abgeschnitten (Puffer)");
        return 0;
    }
    if (strstr(html, "http") || strstr(html, "HTTP") ||
        strstr(html, "<script") || strstr(html, "src=")) {
        strcpy_s(why, cap, "Verweis/Skript enthalten");
        return 0;
    }
    for (i = 0; i < KS_N; i++) {
        char p1[64], p2[64], p3[64];
        const char *a, *b, *c;
        _snprintf_s(p1, sizeof p1, _TRUNCATE, "id=\"el-%s\"", ks_elems[i].id);
        _snprintf_s(p2, sizeof p2, _TRUNCATE, "id=\"co-%s\"", ks_elems[i].id);
        _snprintf_s(p3, sizeof p3, _TRUNCATE, "<tr class=\"e-%s",
                    ks_elems[i].id);
        a = strstr(sv, p1);
        b = strstr(sv, p2);
        c = strstr(se, p3);
        if (!a || a > se || !b || b > se || !c ||
            !ks_co(html, ks_elems[i].id, co, sizeof co) ||
            !strstr(co, "<text class=\"cl\"")) {
            _snprintf_s(why, cap, _TRUNCATE, "Element/Callout/Zeile fehlt: %s",
                        ks_elems[i].id);
            return 0;
        }
    }
    if (!strstr(sv, "class=\"el e-bi0 pf\"") ||
        !strstr(se, "<tr class=\"e-bi0 prim\"")) {
        strcpy_s(why, cap, "Primaerfeuer nicht hervorgehoben");
        return 0;
    }
    if (!strstr(html, "body:has(.e-pedal_r:hover) .e-pedal_r td")) {
        strcpy_s(why, cap, "Hover-Kopplung fehlt");
        return 0;
    }
    if (!strstr(html, LT("gui.svg_kbd")) || !strstr(html, LT("gui.svg_pad")) ||
        !strstr(html, LT("gui.svg_unset"))) {
        strcpy_s(why, cap, "Legende fehlt");
        return 0;
    }
    ks_co(html, "bi34", co, sizeof co);
    if (!strstr(co, ">1<")) {
        strcpy_s(why, cap, "Kippschalter-Taste 1 fehlt");
        return 0;
    }
    ks_co(html, "bi19", co, sizeof co);
    if (!strstr(co, "class=\"un\">" KS_EMDASH)) {
        strcpy_s(why, cap, "Unbelegt-Strich fehlt (bi19)");
        return 0;
    }
    ks_co(html, "bi0", co, sizeof co);
    if (!strstr(co, ">SPACE<") || !strstr(co, LT("gui.svg_m_left")) ||
        (prof ? !strstr(co, ">RB<") : strstr(co, ">RB<") != NULL)) {
        strcpy_s(why, cap, "Belegung bi0 falsch");
        return 0;
    }
    if (prof) {
        ks_co(html, "aim", co, sizeof co);
        if (!strstr(co, ">LS<") || strstr(co, "LS " KS_HARR) ||
            !strstr(co, ">" KS_LARR "<")) {
            strcpy_s(why, cap, "Achsen-Zusammenfassung falsch (aim)");
            return 0;
        }
        ks_co(html, "bi34", co, sizeof co);
        if (!strstr(co, ">BACK+D" KS_LARR "<")) {
            strcpy_s(why, cap, "Akkord falsch (bi34)");
            return 0;
        }
        ks_co(html, "gear", co, sizeof co);
        if (!strstr(co, ">" KS_TRI_U "BACK+RB<") || !strstr(co, ">N:0<")) {
            strcpy_s(why, cap, "Gang-Callout falsch");
            return 0;
        }
        ks_co(html, "sight", co, sizeof co);
        if (!strstr(co, ">Shift+" KS_LARR "<") || !strstr(co, ">RS " KS_VARR
                                                          "<")) {
            strcpy_s(why, cap, "Sicht-Callout falsch");
            return 0;
        }
    } else {
        if (strstr(sv, "class=\"tp\"")) {
            strcpy_s(why, cap, "Standard-Variante mit Gamepad");
            return 0;
        }
        ks_co(html, "bi3", co, sizeof co);
        if (!strstr(co, KS_EMDASH)) {
            strcpy_s(why, cap, "Eject belegt (Standard)");
            return 0;
        }
        ks_co(html, "pedal_r", co, sizeof co);
        if (!strstr(co, ">W<")) {
            strcpy_s(why, cap, "Gaspedal falsch (Standard)");
            return 0;
        }
        ks_co(html, "gear", co, sizeof co);
        if (!strstr(co, ">" KS_TRI_U "E<") || !strstr(co, ">" KS_TRI_D "Q<")) {
            strcpy_s(why, cap, "Gang falsch (Standard)");
            return 0;
        }
    }
    return 1;
}

/* W184 Selbsttest: Kasten eines Elements (Callout bzw. Zelle). */
static void ks_box(const ks_elem_t *e, int r[4])
{
    if (e->lw) {
        r[0] = e->lx, r[1] = e->ly, r[2] = e->lw, r[3] = KS_CO_H;
    } else {
        r[0] = e->x - 3, r[1] = e->y - 3, r[2] = e->w + 6, r[3] = e->h + 40;
    }
}

static int ks_in(const int r[4], const short b[4])
{
    return r[0] >= b[0] && r[1] >= b[1] && r[0] + r[2] <= b[0] + b[2] &&
           r[1] + r[3] <= b[1] + b[3];
}

/* W184 Selbsttest: physische Anordnung. Jedes Element liegt mit Form-
 * Anker, Leader-Start und Kasten in seinem Block, kein Kasten ueberdeckt
 * einen anderen, Felder liegen in einem Block, und die Bloecke stimmen
 * mit der Zuordnung des Original-Controllers ueberein (Regressionsschutz:
 * Kippschalter + Sicht-Stick links, Luke/Zuendung/START/Eject/MultiMon
 * rechts, Magazin + Waffenwahl + COM + Tuner in der Mitte). 1 = ok. */
static int ks_layoutcheck(char *why, size_t cap)
{
    static const struct { const char *id; int g; } want[] = {
        { "gear", KS_G_LEFT }, { "rot", KS_G_LEFT }, { "sight", KS_G_LEFT },
        { "bi33", KS_G_LEFT }, { "bi34", KS_G_LEFT }, { "bi38", KS_G_LEFT },
        { "bi28", KS_G_CENTER }, { "bi32", KS_G_CENTER },
        { "tuner", KS_G_CENTER }, { "bi16", KS_G_CENTER },
        { "bi24", KS_G_CENTER }, { "bi13", KS_G_CENTER },
        { "bi25", KS_G_CENTER }, { "bi27", KS_G_CENTER },
        { "aim", KS_G_RIGHT }, { "bi0", KS_G_RIGHT }, { "bi3", KS_G_RIGHT },
        { "bi4", KS_G_RIGHT }, { "bi6", KS_G_RIGHT }, { "bi7", KS_G_RIGHT },
        { "bi12", KS_G_RIGHT }, { "pedal_m", KS_G_PEDALS },
    };
    int i, j;
    for (i = 0; i < KS_N; i++) {
        const ks_elem_t *e = &ks_elems[i];
        const short *b = ks_blk[e->grp];
        int r[4], pt[4];
        ks_box(e, r);
        pt[0] = e->x, pt[1] = e->y, pt[2] = 0, pt[3] = 0;
        if (!ks_in(r, b) || !ks_in(pt, b)) {
            _snprintf_s(why, cap, _TRUNCATE, "%s ausserhalb seines Blocks",
                        e->id);
            return 0;
        }
        pt[0] = e->ax, pt[1] = e->ay;
        if (e->ax && !ks_in(pt, b)) {
            _snprintf_s(why, cap, _TRUNCATE, "Leader %s ausserhalb", e->id);
            return 0;
        }
        for (j = 0; j < i; j++) {
            int q[4];
            ks_box(&ks_elems[j], q);
            if (r[0] < q[0] + q[2] && q[0] < r[0] + r[2] &&
                r[1] < q[1] + q[3] && q[1] < r[1] + r[3]) {
                _snprintf_s(why, cap, _TRUNCATE, "Kasten %s ueberdeckt %s",
                            e->id, ks_elems[j].id);
                return 0;
            }
        }
    }
    for (i = 0; i < KS_NF; i++) {
        const ks_field_t *f = &ks_fields[i];
        int r[4], hit = 0, g;
        r[0] = f->w ? f->x : f->tx, r[1] = f->w ? f->y : f->ty;
        r[2] = f->w, r[3] = f->h;
        for (g = 0; g < KS_G_N; g++) {
            hit |= ks_in(r, ks_blk[g]);
        }
        if (!hit) {
            _snprintf_s(why, cap, _TRUNCATE, "Feld %d ausserhalb", i);
            return 0;
        }
    }
    for (i = 0; i < (int)(sizeof want / sizeof want[0]); i++) {
        for (j = 0; j < KS_N; j++) {
            if (strcmp(ks_elems[j].id, want[i].id) == 0) {
                break;
            }
        }
        if (j == KS_N || ks_elems[j].grp != want[i].g) {
            _snprintf_s(why, cap, _TRUNCATE, "%s im falschen Block",
                        want[i].id);
            return 0;
        }
    }
    return 1;
}

/* W182 Selbsttest: jedes Element der Mapping-Liste (kCtrlElems ohne
 * Client-Aktionen) ist einem gezeichneten Element zugeordnet. */
static const char *ks_uncovered(void)
{
    int i, j;
    for (i = 0; i < LCTRL_ELEM_N; i++) {
        int hit = 0;
        if (kCtrlElems[i].kind == LCTRL_ACT) {
            continue;
        }
        for (j = 0; j < KS_N && !hit; j++) {
            char names[160], *nm, *ctx = NULL;
            strcpy_s(names, sizeof names, ks_elems[j].elems);
            for (nm = strtok_s(names, " ", &ctx); nm;
                 nm = strtok_s(NULL, " ", &ctx)) {
                if (strcmp(nm, kCtrlElems[i].key) == 0) {
                    hit = 1;
                    break;
                }
            }
        }
        if (!hit) {
            return kCtrlElems[i].key;
        }
    }
    return NULL;
}

/* HTML bauen (rein bis auf LT; ohne Fenster pruefbar). profile = ""
 * heisst Standardbelegung (kein Gamepad-Abschnitt). prof_text = UTF-8
 * oder NULL (mit prof_missing als Hinweis). */
static void keyshtml_build(const char *memo, const char *profile,
                           const char *prof_text, const char *prof_missing,
                           const char *mouse_lbl, int mouse_sens,
                           const char *pad_txt, const char *pad_live,
                           const char *sbc_txt, char *out, size_t cap)
{
    size_t o = 0;
    const char *brand = SB_EDITION_COMMUNITY ? "Steelbound" : "Steel Battalion";
    char e[4096];
    int i, has_prof = profile && profile[0];
    keyshtml_parse(memo);
    keyshtml_add(out, cap, &o, "<!DOCTYPE html>\n<html lang=\"");
    keyshtml_add(out, cap, &o, sb_lang_code());
    keyshtml_add(out, cap, &o, "\">\n<head>\n<meta charset=\"utf-8\">\n"
                 "<meta name=\"viewport\" content=\"width=device-width,"
                 "initial-scale=1\">\n<title>");
    keyshtml_add(out, cap, &o, brand);
    keyshtml_add(out, cap, &o, " " U8_DASH " ");
    keyshtml_add(out, cap, &o, LT("gui.page_controls"));
    keyshtml_add(out, cap, &o, "</title>\n");
    keyshtml_css(out, cap, &o);
    ks_css(out, cap, &o); /* W182 KEYS-SVG */
    keyshtml_add(out, cap, &o, "</head>\n<body><div class=\"wrap\">\n<h1>");
    keyshtml_add(out, cap, &o, brand);
    keyshtml_add(out, cap, &o, " " U8_DASH " ");
    keyshtml_add(out, cap, &o, LT("gui.page_controls"));
    keyshtml_add(out, cap, &o, "</h1>\n<p class=\"sub\">");
    if (has_prof) {
        keyshtml_add(out, cap, &o, LT("gui.ctl_profile"));
        keyshtml_add(out, cap, &o, ": ");
        keyshtml_escape(profile, e, sizeof e);
        keyshtml_add(out, cap, &o, e);
    } else {
        keyshtml_escape(LT("gui.legacy_label"), e, sizeof e);
        keyshtml_add(out, cap, &o, e);
    }
    keyshtml_add(out, cap, &o, "</p>\n<div class=\"prim\">"
                 "<div class=\"prim-h\">");
    keyshtml_add(out, cap, &o, LT("gui.ctl_prim"));
    keyshtml_add(out, cap, &o, "</div><div class=\"prim-k\">");
    keyshtml_add(out, cap, &o, LT("gui.ctl_prim_keys"));
    keyshtml_add(out, cap, &o, "</div></div>\n");
    keysvg_build(profile, prof_text, out, cap, &o); /* W182 KEYS-SVG */
    keyshtml_add(out, cap, &o, "<div class=\"grid\">\n");
    if (has_prof) {
        keyshtml_add(out, cap, &o, "<section class=\"full\"><h2>");
        keyshtml_add(out, cap, &o, LT("gui.ctl_profile"));
        keyshtml_add(out, cap, &o, ": ");
        keyshtml_escape(profile, e, sizeof e);
        keyshtml_add(out, cap, &o, e);
        keyshtml_add(out, cap, &o, "</h2>");
        if (prof_text && prof_text[0]) {
            static char pre[65536];
            keyshtml_escape(prof_text, pre, sizeof pre);
            keyshtml_add(out, cap, &o, "<pre>");
            keyshtml_add(out, cap, &o, pre);
            keyshtml_add(out, cap, &o, "</pre>");
        } else if (prof_missing && prof_missing[0]) {
            keyshtml_escape(prof_missing, e, sizeof e);
            keyshtml_add(out, cap, &o, "<p>");
            keyshtml_add(out, cap, &o, e);
            keyshtml_add(out, cap, &o, "</p>");
        }
        keyshtml_add(out, cap, &o, "</section>\n");
    }
    keyshtml_table(out, cap, &o, LT("gui.keys_h_menus"), KH_MENUS);
    { /* Start: Reihenfolge als Einleitung (gleiche Quelle: Startseite) */
        int n = 0;
        for (i = 0; i < kh_n; i++) {
            n += kh_rows[i].sec == KH_START;
        }
        if (n) {
            keyshtml_add(out, cap, &o, "<section><h2>");
            keyshtml_add(out, cap, &o, LT("gui.start_mech"));
            keyshtml_add(out, cap, &o, "</h2><p>");
            keyshtml_add(out, cap, &o, LT("gui.start_seq"));
            keyshtml_add(out, cap, &o, "</p><table>\n");
            for (i = 0; i < kh_n; i++) {
                if (kh_rows[i].sec == KH_START) {
                    keyshtml_row(out, cap, &o, kh_rows[i].key, kh_rows[i].act);
                }
            }
            keyshtml_add(out, cap, &o, "</table></section>\n");
        }
    }
    keyshtml_table(out, cap, &o, LT("gui.keys_h_combat"), KH_COMBAT);
    keyshtml_table(out, cap, &o, LT("gui.keys_h_move"), KH_MOVE);
    keyshtml_table(out, cap, &o, LT("gui.keys_h_cockpit"), KH_COCKPIT);
    { /* Maus: Modus/Empfindlichkeit live + F10 aus dem Memo */
        int n = 0;
        keyshtml_add(out, cap, &o, "<section><h2>");
        keyshtml_add(out, cap, &o, LT("gui.ctl_mouse"));
        keyshtml_add(out, cap, &o, "</h2><p>");
        keyshtml_escape(mouse_lbl ? mouse_lbl : "", e, sizeof e);
        keyshtml_add(out, cap, &o, e);
        keyshtml_add(out, cap, &o, " " U8_DOT " ");
        keyshtml_add(out, cap, &o, LT("gui.ctl_sens"));
        keyshtml_add(out, cap, &o, " ");
        keyshtml_addf(out, cap, &o, "%d", mouse_sens);
        keyshtml_add(out, cap, &o, "</p>");
        for (i = 0; i < kh_n; i++) {
            n += kh_rows[i].sec == KH_MOUSEX;
        }
        if (n) {
            keyshtml_add(out, cap, &o, "<table>\n");
            for (i = 0; i < kh_n; i++) {
                if (kh_rows[i].sec == KH_MOUSEX) {
                    keyshtml_row(out, cap, &o, kh_rows[i].key, kh_rows[i].act);
                }
            }
            keyshtml_add(out, cap, &o, "</table>");
        }
        keyshtml_add(out, cap, &o, "</section>\n");
    }
    if (has_prof) {
        /* Gamepad: nur bei aktivem Profil (PAD-Zeilen daraus). */
        keyshtml_add(out, cap, &o, "<section><h2>");
        keyshtml_add(out, cap, &o, LT("gui.ctl_pad"));
        keyshtml_add(out, cap, &o, "</h2><p>");
        keyshtml_escape(pad_txt ? pad_txt : "", e, sizeof e);
        keyshtml_add(out, cap, &o, e);
        if (pad_live && pad_live[0]) {
            keyshtml_add(out, cap, &o, "<br>");
            keyshtml_escape(pad_live, e, sizeof e);
            keyshtml_add(out, cap, &o, e);
        }
        keyshtml_add(out, cap, &o, "</p>");
        if (prof_text && strstr(prof_text, "PAD:")) {
            static char plines[16384];
            char *p;
            strcpy_s(plines, sizeof plines, prof_text);
            keyshtml_add(out, cap, &o, "<table>\n");
            p = plines;
            while (*p) {
                char *line = p, *eq;
                while (*p && *p != '\n' && *p != '\r') {
                    p++;
                }
                if (*p == '\r') {
                    *p++ = '\0';
                    if (*p == '\n') {
                        p++;
                    }
                } else if (*p == '\n') {
                    *p++ = '\0';
                }
                if (!strstr(line, "PAD:")) {
                    continue;
                }
                eq = strchr(line, '=');
                if (!eq) {
                    continue;
                }
                *eq = '\0';
                keyshtml_row(out, cap, &o, keyshtml_trim(line),
                             keyshtml_trim(eq + 1));
            }
            keyshtml_add(out, cap, &o, "</table>");
        }
        keyshtml_add(out, cap, &o, "</section>\n");
    }
    { /* Original-Controller: Kurzinfo */
        keyshtml_add(out, cap, &o, "<section><h2>");
        keyshtml_add(out, cap, &o, LT("gui.ctl_device"));
        keyshtml_add(out, cap, &o, "</h2><p>");
        keyshtml_escape(sbc_txt ? sbc_txt : "", e, sizeof e);
        keyshtml_add(out, cap, &o, e);
        keyshtml_add(out, cap, &o, "</p><p>");
        keyshtml_add(out, cap, &o, LT("gui.keys_sbc_info"));
        keyshtml_add(out, cap, &o, "</p></section>\n");
    }
    keyshtml_add(out, cap, &o, "</div>\n<footer>");
    if (kh_footer[0]) {
        keyshtml_escape(kh_footer, e, sizeof e);
        keyshtml_add(out, cap, &o, e);
        keyshtml_add(out, cap, &o, "<br>");
    }
    keyshtml_add(out, cap, &o, LT("gui.keys_local"));
    keyshtml_add(out, cap, &o, "</footer></div></body>\n</html>\n");
}

/* W179: Zielpfad controls.html (W150-Datenorte, sonst Client-Ordner). */
static void keyshtml_path(char *out, size_t cap)
{
    char acp[1024];
    if (sb_userdata_path(SB_UD_ROOT, acp, sizeof acp) == 1) {
        char u8[1024];
        u8_from_acp(acp, u8, sizeof u8);
        _snprintf_s(out, cap, _TRUNCATE, "%s/controls.html", u8);
        return;
    }
    _snprintf_s(out, cap, _TRUNCATE, "%s/controls.html", g_client_dir);
}

/* W179: Knopf "Im Browser oeffnen": HTML schreiben + oeffnen. Fehler
 * als Klartext in die Statuszeile. Test/Shot: nur schreiben, nie
 * oeffnen (stiehlt nie den Fokus). */
static void keyshtml_open(void)
{
    char prof[64], miss[512], path[1200];
    static char praw[12288], pu8[24576], html[393216];
    const char *prof_text = NULL, *mouse_lbl;
    int r = 1, sens, msel, ssel;
    FILE *f = NULL;
    size_t hlen;
    miss[0] = '\0';
    ctrl_profile_from_ui(prof, sizeof prof);
    if (prof[0]) {
        size_t n = 0;
        r = ctrl_profile_load(prof, praw, sizeof praw, &n);
        if (r > 0) {
            u8_from_acp(praw, pu8, sizeof pu8);
            prof_text = pu8;
        } else {
            _snprintf_s(miss, sizeof miss, _TRUNCATE,
                        LT(r == 0 ? "gui.ctl_notname" : "gui.ctl_notfound"),
                        prof);
        }
    }
    msel = ComboBox_GetCurSel(ctl(ID_MOUSE_MODE));
    mouse_lbl = LT(msel == 1 ? "gui.mouse_rel" : "gui.mouse_abs");
    ssel = ComboBox_GetCurSel(ctl(ID_MOUSE_SENS));
    sens = (ssel >= 0 && ssel < 16) ? ssel + 1 : 4;
    keyshtml_build(LT("gui.keys_memo"), prof, prof_text,
                   miss[0] ? miss : NULL, mouse_lbl, sens, g_pad_txt,
                   g_padlive_txt, g_sbc_txt, html, sizeof html);
    keyshtml_path(path, sizeof path);
    if (u8_fopen_s(&f, path, "wb") != 0 || !f) {
        char t[1500];
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.keys_write_err"), path);
        set_status_k(t, 2);
        return;
    }
    hlen = strlen(html);
    if (hlen && fwrite(html, 1, hlen, f) != hlen) {
        char t[1500];
        fclose(f);
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.keys_write_err"), path);
        set_status_k(t, 2);
        return;
    }
    fclose(f);
    if (g_o.shot != 0) {
        printf("[open] %s\n", path);
        set_status(LT("gui.keys_open_ok"));
        return;
    }
    {
        wchar_t w[1200];
        HINSTANCE rc;
        u8_to_w(path, w, 1200);
        rc = ShellExecuteW(g_hwnd, L"open", w, NULL, NULL, SW_SHOWNORMAL);
        if ((INT_PTR)rc <= 32) {
            char t[1500];
            _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.keys_open_err"), path);
            set_status_k(t, 2);
        } else {
            set_status(LT("gui.keys_open_ok"));
        }
    }
}

/* ------------------------------------------------------------------ */
/* W185 ZADIG-HELP: Anleitung "Original-Controller anschliessen"       */
/* ------------------------------------------------------------------ */
/* Lokale HTML-Seite wie W179 (controls.html): Erzeugen im Benutzer-
 * daten-Ordner, im Browser oeffnen, eingebettetes CSS im Launcher-
 * Look (keyshtml_css), druckbar, Sprache = Launcher-Sprache, kein
 * Netzwerk durch den Launcher. Inhalt aus lang-Schluesseln
 * (gui.zadig_*), genau ein externer Verweis (Zadig-Seite als <a href>,
 * der Nutzer laedt selbst), keine Skripte. Quelltext ASCII (U8_*),
 * Ausgabe UTF-8. */
#define ZADIG_URL "https://zadig.akeo.ie/"

/* Zusatz-CSS: Listen + die eine Link-Farbe (Schirm, Druck). */
static void zadig_css(char *out, size_t cap, size_t *o)
{
    keyshtml_add(out, cap, o,
                 "<style>\n"
                 "ol,ul{margin:0 0 8px;padding-left:22px;font-size:14px;"
                 "color:#c9cfd9}\n"
                 "li{margin:0 0 4px}\n"
                 "a{color:#f2a84e}\n"
                 "@media print{\n"
                 "ol,ul{color:#333}\n"
                 "a{color:#8a4d00}\n"
                 "}\n"
                 "</style>\n");
}

/* Eine Anleitungszeile als <li> (escapen, Zadig-Adresse verlinken). */
static void zadig_li(char *out, size_t cap, size_t *o, const char *line)
{
    static char e[4096];
    char *p, *q;
    keyshtml_escape(line ? line : "", e, sizeof e);
    keyshtml_add(out, cap, o, "<li>");
    p = e;
    for (;;) {
        char sv;
        q = strstr(p, ZADIG_URL);
        if (!q) {
            keyshtml_add(out, cap, o, p);
            break;
        }
        sv = *q;
        *q = '\0';
        keyshtml_add(out, cap, o, p);
        *q = sv;
        keyshtml_add(out, cap, o, "<a href=\"" ZADIG_URL "\">" ZADIG_URL "</a>");
        p = q + sizeof ZADIG_URL - 1;
    }
    keyshtml_add(out, cap, o, "</li>\n");
}

/* Abschnitt oeffnen: <section><h2>Kopf</h2><ol/ul> Zeilen </..>.
 * zadig_section_end schliesst (dazwischen passt Hinweis-HTML, s.
 * Warnung in Schritt 2, Zentrier-Hinweis in Schritt 4). */
static void zadig_section_open(char *out, size_t cap, size_t *o,
                               const char *head, const char *body, int ordered)
{
    static char b[4096], e[256];
    char *p;
    keyshtml_add(out, cap, o, "<section><h2>");
    keyshtml_escape(head ? head : "", e, sizeof e);
    keyshtml_add(out, cap, o, e);
    keyshtml_add(out, cap, o, ordered ? "</h2>\n<ol>\n" : "</h2>\n<ul>\n");
    strcpy_s(b, sizeof b, body ? body : "");
    p = b;
    while (*p) {
        char *line = p, *nl = strchr(p, '\n');
        if (nl) {
            *nl = '\0';
        }
        if (*keyshtml_trim(line)) {
            zadig_li(out, cap, o, line);
        }
        if (!nl) {
            break;
        }
        p = nl + 1;
    }
    keyshtml_add(out, cap, o, ordered ? "</ol>\n" : "</ul>\n");
}

static void zadig_section_end(char *out, size_t cap, size_t *o)
{
    keyshtml_add(out, cap, o, "</section>\n");
}

/* Hinweis-Absatz in einem Abschnitt. */
static void zadig_p(char *out, size_t cap, size_t *o, const char *text)
{
    static char e[2048];
    keyshtml_add(out, cap, o, "<p>");
    keyshtml_escape(text ? text : "", e, sizeof e);
    keyshtml_add(out, cap, o, e);
    keyshtml_add(out, cap, o, "</p>\n");
}

/* Warnung in einem Abschnitt (Warnkasten wie Primaerfeuer). */
static void zadig_warn(char *out, size_t cap, size_t *o, const char *text)
{
    static char e[2048];
    keyshtml_add(out, cap, o, "<div class=\"prim\"><div class=\"prim-k\">");
    keyshtml_escape(text ? text : "", e, sizeof e);
    keyshtml_add(out, cap, o, e);
    keyshtml_add(out, cap, o, "</div></div>\n");
}

static void zadig_build(char *out, size_t cap)
{
    size_t o = 0;
    const char *brand = SB_EDITION_COMMUNITY ? "Steelbound" : "Steel Battalion";
    keyshtml_add(out, cap, &o, "<!DOCTYPE html>\n<html lang=\"");
    keyshtml_add(out, cap, &o, sb_lang_code());
    keyshtml_add(out, cap, &o, "\">\n<head>\n<meta charset=\"utf-8\">\n"
                 "<meta name=\"viewport\" content=\"width=device-width,"
                 "initial-scale=1\">\n<title>");
    keyshtml_add(out, cap, &o, brand);
    keyshtml_add(out, cap, &o, " " U8_DASH " ");
    keyshtml_add(out, cap, &o, LT("gui.zadig_title"));
    keyshtml_add(out, cap, &o, "</title>\n");
    keyshtml_css(out, cap, &o);
    zadig_css(out, cap, &o);
    keyshtml_add(out, cap, &o, "</head>\n<body><div class=\"wrap\">\n<h1>");
    keyshtml_add(out, cap, &o, brand);
    keyshtml_add(out, cap, &o, " " U8_DASH " ");
    keyshtml_add(out, cap, &o, LT("gui.zadig_title"));
    keyshtml_add(out, cap, &o, "</h1>\n<div class=\"grid\">\n");
    zadig_section_open(out, cap, &o, LT("gui.zadig_need_h"),
                       LT("gui.zadig_need"), 0);
    zadig_section_end(out, cap, &o);
    zadig_section_open(out, cap, &o, LT("gui.zadig_driver_h"),
                       LT("gui.zadig_driver"), 1);
    zadig_warn(out, cap, &o, LT("gui.zadig_warn"));
    zadig_p(out, cap, &o, LT("gui.zadig_admin"));
    zadig_section_end(out, cap, &o);
    zadig_section_open(out, cap, &o, LT("gui.zadig_launch_h"),
                       LT("gui.zadig_launch"), 1);
    zadig_section_end(out, cap, &o);
    zadig_section_open(out, cap, &o, LT("gui.zadig_assist_h"),
                       LT("gui.zadig_assist"), 1);
    zadig_p(out, cap, &o, LT("gui.zadig_center"));
    zadig_section_end(out, cap, &o);
    zadig_section_open(out, cap, &o, LT("gui.zadig_start_h"),
                       LT("gui.zadig_start"), 1);
    zadig_section_end(out, cap, &o);
    zadig_section_open(out, cap, &o, LT("gui.zadig_fix_h"),
                       LT("gui.zadig_fix"), 0);
    zadig_section_end(out, cap, &o);
    keyshtml_add(out, cap, &o, "</div>\n<footer>");
    keyshtml_add(out, cap, &o, LT("gui.keys_local"));
    keyshtml_add(out, cap, &o, "</footer></div></body>\n</html>\n");
}

/* W185: Zielpfad zadig_guide.html (W150-Datenorte, sonst Client-Ordner). */
static void zadig_path(char *out, size_t cap)
{
    char acp[1024];
    if (sb_userdata_path(SB_UD_ROOT, acp, sizeof acp) == 1) {
        char u8[1024];
        u8_from_acp(acp, u8, sizeof u8);
        _snprintf_s(out, cap, _TRUNCATE, "%s/zadig_guide.html", u8);
        return;
    }
    _snprintf_s(out, cap, _TRUNCATE, "%s/zadig_guide.html", g_client_dir);
}

/* W185: Knopf "Setup guide": HTML schreiben + oeffnen. Fehler als
 * Klartext in die Statuszeile. Test/Shot: nur schreiben, nie oeffnen
 * (stiehlt nie den Fokus). */
static void zadig_open(void)
{
    char path[1200];
    static char html[65536];
    FILE *f = NULL;
    size_t hlen;
    zadig_build(html, sizeof html);
    zadig_path(path, sizeof path);
    if (u8_fopen_s(&f, path, "wb") != 0 || !f) {
        char t[1500];
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.zadig_write_err"), path);
        set_status_k(t, 2);
        return;
    }
    hlen = strlen(html);
    if (hlen && fwrite(html, 1, hlen, f) != hlen) {
        char t[1500];
        fclose(f);
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.zadig_write_err"), path);
        set_status_k(t, 2);
        return;
    }
    fclose(f);
    if (g_o.shot != 0) {
        printf("[open] %s\n", path);
        set_status(LT("gui.zadig_open_ok"));
        return;
    }
    {
        wchar_t w[1200];
        HINSTANCE rc;
        u8_to_w(path, w, 1200);
        rc = ShellExecuteW(g_hwnd, L"open", w, NULL, NULL, SW_SHOWNORMAL);
        if ((INT_PTR)rc <= 32) {
            char t[1500];
            _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.zadig_open_err"), path);
            set_status_k(t, 2);
        } else {
            set_status(LT("gui.zadig_open_ok"));
        }
    }
}

static void update_sbc_status(void)
{
    /* W171: Ergebnis merken (R25: Controller gewaehlt, Geraet fehlt) */
    g_sbc_rc = lctrl_sbc_status(g_sbc_txt, sizeof g_sbc_txt);
}

/* W159: VR-Startknopf je OpenXR-Runtime freischalten + Hinweistext.
 * Nur Registry lesen (app_xr_runtime_present), nichts laden/aendern. */
static void update_vr_button(void)
{
    char path[1024];
    g_vr_present = app_xr_runtime_present(path, sizeof path);
    /* W171: Sperre + Grund ohne Runtime = Regel R5 (ui_apply_rules) */
    _snprintf_s(g_vr_hint, sizeof g_vr_hint, _TRUNCATE, "%s", LT("gui.vr_hint"));
}

static int update_pad_status(void)
{
    lctrl_pad_t p;
    int port = lctrl_pad_first(&p);
    char a[160], b[512];
    if (port < 0) {
        strcpy_s(a, sizeof a, LT("gui.pad_off"));
        strcpy_s(b, sizeof b, LT("gui.pad_none"));
    } else {
        _snprintf_s(a, sizeof a, _TRUNCATE, LT("gui.pad_on"), port);
        lctrl_pad_format(&p, b, sizeof b);
    }
    if (strcmp(a, g_pad_txt) == 0 && strcmp(b, g_padlive_txt) == 0) {
        return 0;
    }
    strcpy_s(g_pad_txt, sizeof g_pad_txt, a);
    strcpy_s(g_padlive_txt, sizeof g_padlive_txt, b);
    return 1;
}

static void refresh_profiles(void)
{
    HWND cb = ctl(ID_PROF_COMBO);
    char names[64][64], dflt[64], cur[128];
    int n, i, sel = -1;
    ui_gettext(cb, cur, sizeof cur);
    trim_u8(cur);
    g_updating++;
    ComboBox_ResetContent(cb);
    n = lcfg_profile_list(g_client_dir, names, 64);
    for (i = 0; i < n; i++) {
        combo_add(cb, names[i]);
        if (cur[0] && strcmp(names[i], cur) == 0) {
            sel = i;
        }
    }
    if (lcfg_profile_get_default(g_client_dir, dflt, sizeof dflt) != 0) {
        dflt[0] = '\0';
    }
    strcpy_s(g_dflt_setup, sizeof g_dflt_setup, dflt); /* W171: R28 */
    if (sel >= 0) {
        ComboBox_SetCurSel(cb, sel);
    } else if (cur[0]) {
        ui_settext(cb, cur);
    } else if (dflt[0]) {
        for (i = 0; i < n; i++) {
            if (strcmp(names[i], dflt) == 0) {
                ComboBox_SetCurSel(cb, i);
            }
        }
    }
    g_updating--;
    _snprintf_s(g_prof_info, sizeof g_prof_info, _TRUNCATE, LT("gui.prof_line"), n,
                dflt[0] ? dflt : LT("gui.prof_none"));
    setup_auto_sync(0); /* W172/P7 */
}

static void profile_name_from_ui(char *out, size_t cap)
{
    ui_gettext(ctl(ID_PROF_COMBO), out, cap);
    trim_u8(out);
}

/* W173 LAUNCHER-P4: g_gfx = Werte des einheitlichen Grafik-Presets (shx[]
 * inkl. hud_label_avoid, fx[], HD). Die Seite Grafik zeigt einen Teil; alle
 * anderen Werte (Feinregler aus dem ESC-Menue) bleiben erhalten und zaehlen
 * fuer Benutzerdefiniert. */

/* Echte Bildrate fuer den Namen des Standards (PO 7.11.): 30 = Original,
 * sonst Zwischenbilder; exklusiv folgt sie der Bildwiederholrate (R9,
 * Client fs_interp_match=1). */
static int gfx_fps(const launcher_settings_t *s)
{
    int fps = s->interp_fps > 0 ? s->interp_fps : 30;
    if (s->window_mode == 2 && s->interp_fps > 0) {
        int hz = s->fs_hz > 0 ? s->fs_hz : g_disp_hz;
        if (hz >= 60 && hz <= 240) {
            fps = hz;
        }
    }
    return fps;
}

/* Name eines Presets (LGP_*), Original mit Bildrate. */
static void gfx_name(int p, const launcher_settings_t *s, char *out, size_t cap)
{
    if (p == LGP_ORIGINAL) {
        _snprintf_s(out, cap, _TRUNCATE, LT("gui.gfx_p_orig"), gfx_fps(s));
    } else {
        strcpy_s(out, cap, LT(p == LGP_SUBTLE ? "gui.gfx_p_subtle"
                              : p == LGP_STRONG ? "gui.gfx_p_strong" : "gui.gfx_p_custom"));
    }
}

/* Name des aktuellen Zustands (Bereitschaft, Statuszeile): Preset bzw.
 * "Benutzerdefiniert - basiert auf X". */
static void gfx_state_name(const launcher_settings_t *s, char *out, size_t cap)
{
    lgp_mask_t lock = gfx_lock_now();
    int p = lcfg_gfx_preset(s, lock);
    if (p == LGP_CUSTOM) {
        char b[96];
        gfx_name(lcfg_gfx_base(s, lock), s, b, sizeof b);
        _snprintf_s(out, cap, _TRUNCATE, LT("gui.gfx_custom_of"), b);
    } else {
        gfx_name(p, s, out, cap);
    }
}

static void ui_settext_if(HWND h, const char *u8)
{
    char cur[600];
    if (!h) {
        return;
    }
    ui_gettext(h, cur, sizeof cur);
    if (strcmp(cur, u8) != 0) {
        ui_settext(h, u8);
        InvalidateRect(h, NULL, FALSE);
    }
}

/* Steuerelemente der Seite Grafik aus g_gfx (Optionsfelder, Einzeloptionen). */
static void gfx_sync_ui(void)
{
    lgp_mask_t lock = gfx_lock_now(), dm = 0;
    int p = lcfg_gfx_preset(&g_gfx, lock), i, fl, aa = g_gfx.shx[LSHX_AA];
    launcher_settings_t cur;
    char t[600], n[160];
    if (!ctl(ID_GP0)) {
        return;
    }
    cur = g_cur;
    if (ctl(ID_IPFPS)) { /* aktuelle Bildrate der Oberflaeche */
        static const int ip_map[3] = { 0, 60, 120 };
        int sel = ComboBox_GetCurSel(ctl(ID_IPFPS));
        cur.interp_fps = (sel >= 0 && sel < 3) ? ip_map[sel] : cur.interp_fps;
        sel = ComboBox_GetCurSel(ctl(ID_WINMODE));
        cur.window_mode = (sel >= 0 && sel <= 2) ? sel : cur.window_mode;
    }
    g_updating++;
    for (i = 0; i < 4; i++) {
        if (i < 3) {
            gfx_name(i, &cur, n, sizeof n);
            _snprintf_s(t, sizeof t, _TRUNCATE, "%s\n%s", n,
                        LT(i == 0 ? "gui.gfx_p_orig_txt" : i == 1 ? "gui.gfx_p_subtle_txt"
                                                                 : "gui.gfx_p_strong_txt"));
        } else if (p == LGP_CUSTOM) {
            int base = lcfg_gfx_base(&g_gfx, lock), k, hidden = 0;
            lcfg_gfx_diff(&g_gfx, base, lock, &dm);
            for (k = 0; k < LGK_N; k++) { /* Abweichungen ohne Element (ESC-Feinregler) */
                if ((dm & ((lgp_mask_t)1 << k)) && k != LSHX_SHADING && k != LSHX_SHADOW &&
                    k != LSHX_SOFT && k != LSHX_AA && k != LGK_HD &&
                    !(k >= LGK_FX0 && k < LGK_FX0 + LFX_N)) {
                    hidden++;
                }
            }
            gfx_name(base, &cur, n, sizeof n);
            {
                char sub[300];
                if (hidden) {
                    _snprintf_s(sub, sizeof sub, _TRUNCATE, LT("gui.gfx_p_custom_esc"), n, hidden);
                } else {
                    _snprintf_s(sub, sizeof sub, _TRUNCATE, LT("gui.gfx_p_custom_txt"), n);
                }
                _snprintf_s(t, sizeof t, _TRUNCATE, "%s\n%s", LT("gui.gfx_p_custom"), sub);
            }
        } else {
            _snprintf_s(t, sizeof t, _TRUNCATE, "%s\n%s", LT("gui.gfx_p_custom"),
                        LT("gui.gfx_p_custom_off"));
        }
        ui_settext_if(ctl(ID_GP0 + i), t);
        if (ui_checked(ID_GP0 + i) != (p == i)) {
            ui_set_checked(ID_GP0 + i, p == i);
        }
    }
    ComboBox_SetCurSel(ctl(ID_GFX_LIGHT), g_gfx.shx[LSHX_SHADING] ? 0 : 1);
    ComboBox_SetCurSel(ctl(ID_GFX_SHADOW), g_gfx.shx[LSHX_SHADOW] ? 1 : 0);
    ComboBox_SetCurSel(ctl(ID_GFX_SOFT), g_gfx.shx[LSHX_SOFT] ? 1 : 0);
    fl = lcfg_fx_level(&g_gfx, lock);
    { /* Eintrag "Benutzerdefiniert" nur, wenn die Werte keiner Stufe entsprechen */
        HWND c = ctl(ID_GFX_FX);
        int want = fl == LGP_CUSTOM ? 4 : 3;
        while (ComboBox_GetCount(c) > want) {
            ComboBox_DeleteString(c, ComboBox_GetCount(c) - 1);
        }
        if (ComboBox_GetCount(c) < want) {
            combo_add(c, LT("gui.gfx_fx_custom"));
        }
        ComboBox_SetCurSel(c, fl);
    }
    ComboBox_SetCurSel(ctl(ID_GFX_TEX), lcfg_gfx_value(&g_gfx, LGK_HD) ? 1 : 0);
    ComboBox_SetCurSel(ctl(ID_AAMODE), (aa >= 0 && aa <= 3) ? aa : 0);
    ui_set_checked(ID_REM_LABEL, g_gfx.shx[LSHX_LABEL] ? 1 : 0); /* nicht im Preset */
    g_updating--;
}

/* Preset-Werte in die Einstellungen (Ableitungen: remaster = HD, fx_enhanced). */
static void gfx_to_settings(launcher_settings_t *s)
{
    memcpy(s->shx, g_gfx.shx, sizeof s->shx);
    memcpy(s->fx, g_gfx.fx, sizeof s->fx);
    s->remaster = g_gfx.remaster;
    s->hd_textures = g_gfx.hd_textures;
    /* W114: nur 2x/4x; die Freigaben aus der Bewertung gelten bei beiden */
    s->hd_scale = ComboBox_GetCurSel(ctl(ID_REM_SCALE)) == 0 ? 2 : 4;
    lcfg_gfx_normalize(s);
}

/* W105 MONITOR-FX: Auswahl -> [render] monitor_grain/monitor_glitch (%). */
static const int kMonFx[4][2] = { { 100, 100 }, { 100, 0 }, { 0, 100 }, { 0, 0 } };
static int g_monfx_grain = 100, g_monfx_glitch = 100;
static int monfx_index(int grain, int glitch)
{
    return grain > 0 ? (glitch > 0 ? 0 : 1) : (glitch > 0 ? 2 : 3);
}

static void ui_from_settings(const launcher_settings_t *s)
{
    HWND c;
    g_updating++;
    g_setup_force = 0; /* W172: neue Werte -> Einrichtungskarte nur per Regel */
    { /* W172/P6: Programmdatei = <Spielordner>\default.xbe -> Feld leer
       * (automatisch, Hinweis im Feld); user.cfg bekommt beim Speichern
       * wieder den aufgeloesten Pfad (lcfg_save_user). */
        char x[1200];
        xbe_derived(s, x, sizeof x);
        ui_settext(ctl(ID_XBE), (x[0] && path_eq(x, s->xbe_path)) ? "" : s->xbe_path);
    }
    ui_settext(ctl(ID_MEDIA), s->media_root);
    ui_settext(ctl(ID_HDD), s->hdd_root);
    ui_settext(ctl(ID_IMAGE), s->game_image); /* W150 */
    if (s->game_image[0]) {
        check_image_ex(0); /* Ergebnis gleich sichtbar (Seite Pfade) */
    } else {
        g_img_msg[0] = 0;
    }
    set_num(ID_W, s->width);
    set_num(ID_H, s->height);
    ComboBox_SetCurSel(ctl(ID_VSYNC), s->present_interval ? 1 : 0);
    c = ctl(ID_FPS);
    ComboBox_SetCurSel(c, s->fps_limit == 30 ? 1 : s->fps_limit == 60 ? 2
                          : s->fps_limit == 120 ? 3 : 0);
    ui_set_checked(ID_AUDIO, s->audio);
    ui_set_checked(ID_LIVE, s->audio_live);
    ui_set_checked(ID_ASPECT, s->aspect_mode ? 1 : 0);
    refresh_ctrl_profiles(s->input_profile);
    ui_set_checked(ID_CTRL_SBC, s->sbc_usb);
    ui_set_checked(ID_CTRL_MOUSEOFF, s->sbc_mouse_off);
    ComboBox_SetCurSel(ctl(ID_IPFPS), s->interp_fps == 60 ? 1
                                      : s->interp_fps == 120 ? 2 : 0);
    ComboBox_SetCurSel(ctl(ID_WINMODE),
                       (s->window_mode >= 0 && s->window_mode <= 2) ? s->window_mode : 0);
    /* W159: Kombi = Vorschau-Art (Startweg entscheidet an/aus, s. do_start) */
    ComboBox_SetCurSel(ctl(ID_VR),
                       _stricmp(s->vr_preview, "sbs") == 0 ? 0 :
                       _stricmp(s->vr_preview, "right") == 0 ? 2 :
                       (_stricmp(s->vr_preview, "off") == 0 || _stricmp(s->vr_preview, "xr") == 0) ? 3 : 1);
    { /* W183 VR-FIX3: Preset (0..2 = Leistung/Ausgewogen/Qualitaet, 3 = Eigene) + Aufloesung */
        int pr = lcfg_vr_preset_match(s);
        ComboBox_SetCurSel(ctl(ID_VRPRESET), pr >= 1 ? pr - 1 : 3);
        ComboBox_SetCurSel(ctl(ID_VRRES), vrres_index(s->vr_render_scale));
    }
    ComboBox_SetCurSel(ctl(ID_RTSCALE), s->rt_scale_dyn ? 0 : /* W148 */
                       (s->rt_scale <= 0 ? 1 : s->rt_scale <= 4 ? s->rt_scale + 1 : 5));
    {
        int k, hsel = 0;
        c = ctl(ID_FSHZ);
        /* unbekannte Rate (von Hand in user.cfg) bleibt als Roheintrag */
        while (ComboBox_GetCount(c) > KFSHZ_N) {
            ComboBox_DeleteString(c, KFSHZ_N);
        }
        for (k = 0; k < KFSHZ_N; k++) {
            if (kFsHz[k].hz == s->fs_hz) { hsel = k; }
        }
        if (s->fs_hz && kFsHz[hsel].hz != s->fs_hz) {
            char t[32];
            _snprintf_s(t, sizeof t, _TRUNCATE, "%d Hz", s->fs_hz);
            combo_add(c, t);
            hsel = KFSHZ_N;
        }
        ComboBox_SetCurSel(c, hsel);
    }
    ComboBox_SetCurSel(ctl(ID_MONFX), monfx_index(s->monitor_grain, s->monitor_glitch));
    g_monfx_grain = s->monitor_grain; /* W105: Zwischenwerte (von Hand) bleiben erhalten */
    g_monfx_glitch = s->monitor_glitch;
    edge_from_settings(s); /* W107 */
    g_gfx = *s; /* W173: alle Preset-Werte bleiben (Benutzerdefiniert) */
    lcfg_gfx_migrate(&g_gfx); /* alte remaster+fx_enhanced -> Effekte dezent */
    g_gfx.remaster = g_gfx.hd_textures = (s->remaster && s->hd_textures) ? 1 : 0;
    ComboBox_SetCurSel(ctl(ID_REM_SCALE), s->hd_scale == 2 ? 0 : 1); /* alt 0 -> 4x */
    ComboBox_SetCurSel(ctl(ID_MOUSE_MODE), s->mouse_mode ? 0 : 1);
    ComboBox_SetCurSel(ctl(ID_MOUSE_SENS),
                       (s->mouse_sens >= 1 && s->mouse_sens <= 16)
                           ? s->mouse_sens - 1 : 3);
    ComboBox_SetCurSel(ctl(ID_LANG), /* W142 */
                       _stricmp(s->lang, "de") == 0 ? 1 : 0);
    update_ctrl_overview();
    xbe_cue_update(); /* W172 */
    g_updating--;
    gfx_sync_ui(); /* W173 */
    sync_preset();
    update_aspect_info();
    ui_apply_rules(); /* W171: alle Freigaben/Begruendungen aus dem Regelwerk */
}

static void ui_to_settings(launcher_settings_t *s)
{
    static const int fps_map[4] = { 0, 30, 60, 120 };
    static const int ip_map[3] = { 0, 60, 120 };
    int sel;
    ui_gettext(ctl(ID_XBE), s->xbe_path, sizeof s->xbe_path);
    ui_gettext(ctl(ID_MEDIA), s->media_root, sizeof s->media_root);
    ui_gettext(ctl(ID_HDD), s->hdd_root, sizeof s->hdd_root);
    ui_gettext(ctl(ID_IMAGE), s->game_image, sizeof s->game_image); /* W150 */
    s->width = get_num(ID_W);
    s->height = get_num(ID_H);
    s->present_interval = ComboBox_GetCurSel(ctl(ID_VSYNC)) == 1 ? 1 : 0;
    sel = ComboBox_GetCurSel(ctl(ID_FPS));
    s->fps_limit = (sel >= 0 && sel < 4) ? fps_map[sel] : 0;
    s->audio = ui_checked(ID_AUDIO);
    s->audio_live = ui_checked(ID_LIVE);
    /* Bildformat fest (S3.10, nie strecken). */
    s->aspect_mode = 1;
    ctrl_profile_from_ui(s->input_profile, sizeof s->input_profile);
    s->sbc_usb = ui_checked(ID_CTRL_SBC);
    s->sbc_mouse_off = ui_checked(ID_CTRL_MOUSEOFF);
    sel = ComboBox_GetCurSel(ctl(ID_IPFPS));
    s->interp_fps = (sel >= 0 && sel < 3) ? ip_map[sel] : 0;
    sel = ComboBox_GetCurSel(ctl(ID_WINMODE));
    s->window_mode = (sel >= 0 && sel <= 2) ? sel : 0;
    /* W159: Kombi setzt nur die Vorschau-Art; s->vr_enable bleibt wie
     * geladen (alte user.cfg mit 1 aendert keinen Startweg mehr). */
    sel = ComboBox_GetCurSel(ctl(ID_VR));
    {
        static const char *const prev[4] = { "sbs", "left", "right", "off" };
        if (sel >= 0 && sel <= 3) {
            strcpy_s(s->vr_preview, sizeof s->vr_preview, prev[sel]);
        }
    }
    { /* W183 VR-FIX3: Preset setzt die sechs Werte, die Aufloesung-Kombi gilt zuletzt */
        int ps = ComboBox_GetCurSel(ctl(ID_VRPRESET)), rs = ComboBox_GetCurSel(ctl(ID_VRRES));
        if (ps >= 0 && ps <= 2 && lcfg_vr_preset_match(s) != ps + 1) { lcfg_vr_preset_apply(s, ps + 1); }
        if (rs >= 0 && rs < KVRRES_N) { s->vr_render_scale = kVrRes[rs]; }
        s->vr_preset = lcfg_vr_preset_match(s);
    }
    sel = ComboBox_GetCurSel(ctl(ID_RTSCALE)); /* W148 */
    if (sel == 0) { s->rt_scale_dyn = 1; s->rt_scale = 0; }
    else if (sel == 1) { s->rt_scale_dyn = 0; s->rt_scale = 0; }
    else if (sel >= 2 && sel <= 5) { s->rt_scale_dyn = 0; s->rt_scale = sel - 1; }
    sel = ComboBox_GetCurSel(ctl(ID_MONFX));
    if (sel >= 0 && sel < 4 && sel != monfx_index(g_monfx_grain, g_monfx_glitch)) {
        s->monitor_grain = kMonFx[sel][0];
        s->monitor_glitch = kMonFx[sel][1];
    } else {
        s->monitor_grain = g_monfx_grain;
        s->monitor_glitch = g_monfx_glitch;
    }
    sel = ComboBox_GetCurSel(ctl(ID_FSHZ));
    if (sel >= 0 && sel < KFSHZ_N) {
        s->fs_hz = kFsHz[sel].hz;
    } else if (sel == KFSHZ_N) {
        char t[32];
        ui_gettext(ctl(ID_FSHZ), t, sizeof t);
        s->fs_hz = atoi(t);
    } else {
        s->fs_hz = 0;
    }
    edge_to_settings(s); /* W107 */
    s->mouse_mode = ComboBox_GetCurSel(ctl(ID_MOUSE_MODE)) == 1 ? 0 : 1;
    sel = ComboBox_GetCurSel(ctl(ID_MOUSE_SENS));
    s->mouse_sens = (sel >= 0 && sel < 16) ? sel + 1 : 4;
    sel = ComboBox_GetCurSel(ctl(ID_LANG)); /* W142 */
    if (sel == 1) {
        strcpy_s(s->lang, sizeof s->lang, "de");
    } else if (sel == 0) {
        strcpy_s(s->lang, sizeof s->lang, "en");
    }
    gfx_to_settings(s); /* W173: einheitliches Grafik-Preset */
}

/* Kurzstatus der Startseite aus dem aktuellen Oberflaechen-Stand. */
static void update_summary(void)
{
    launcher_settings_t s;
    char err[1024];
    s = g_s;
    ui_to_settings(&s);
    /* W172/P6: Zeilen Spiel, Grafik, Anzeige, Packs, Steuerung; jede mit
     * "Aendern" (Sprung zur Seite der Ursache). Client fehlt = Startsperre
     * R4 unter den Startknoepfen, Einstellungsdatei = Erweitert > Diagnose. */
    g_sum[0].label = LT("gui.sum_game");
    if (lcfg_validate(&s, err, sizeof err) == 0) {
        if (s.game_image[0]) { /* W150: Abbild-Modus */
            const char *b = s.game_image + strlen(s.game_image);
            while (b > s.game_image && b[-1] != '\\' && b[-1] != '/') {
                b--;
            }
            _snprintf_s(g_sum[0].value, sizeof g_sum[0].value, _TRUNCATE,
                        LT("gui.play_sum_image"), b);
        } else {
            _snprintf_s(g_sum[0].value, sizeof g_sum[0].value, _TRUNCATE,
                        LT("gui.play_sum_folder"), s.media_root);
        }
        g_sum[0].col = C_OK;
    } else {
        strcpy_s(g_sum[0].value, sizeof g_sum[0].value, err);
        g_sum[0].col = C_ERR;
    }
    g_sum[2].label = LT("gui.page_display");
    {
        const char *fps = s.interp_fps == 0    ? LT("gui.sum_30")
                          : s.interp_fps == 60 ? LT("gui.sum_60")
                                               : LT("gui.sum_120");
        if (s.window_mode == 1 || s.window_mode == 2) {
            _snprintf_s(g_sum[2].value, sizeof g_sum[2].value, _TRUNCATE,
                        LT("gui.play_sum_fs"),
                        s.window_mode == 1 ? LT("gui.wm_borderless") : LT("gui.wm_excl"),
                        fps);
        } else {
            _snprintf_s(g_sum[2].value, sizeof g_sum[2].value, _TRUNCATE,
                        LT("gui.play_sum_win"), s.width, s.height, fps);
        }
    }
    g_sum[2].col = (s.width >= 320 && s.height >= 200) ? C_OK : C_ERR;
    if (g_rr[10].active) { /* R10: 120 fps auf langsamerem Monitor */
        g_sum[2].col = C_WARN;
    }
    g_sum[3].label = LT("gui.page_packs");
    {
        int i, act = 0, warn = 0, bad = 0, tex = 0;
        for (i = 0; i < packs_count(); i++) {
            int st = packs_status_of(i);
            if (!packs_enabled_of(i)) {
                continue;
            }
            if (st < 1) {
                bad++;
                continue;
            }
            act++;
            warn += st == 1;
            tex += !packs_is_fx(i);
        }
        if (packs_count() == 0) {
            strcpy_s(g_sum[3].value, sizeof g_sum[3].value, LT("gui.play_sum_nopacks"));
            g_sum[3].col = C_FAINT;
        } else if (act == 0) {
            strcpy_s(g_sum[3].value, sizeof g_sum[3].value, LT("gui.play_sum_packsoff"));
            g_sum[3].col = bad ? C_WARN : C_FAINT;
        } else {
            char lv[32];
            strcpy_s(lv, sizeof lv, packs_level_word());
            if (tex) {
                _snprintf_s(g_sum[3].value, sizeof g_sum[3].value, _TRUNCATE,
                            LT("gui.play_sum_packs"), act, lv);
            } else {
                _snprintf_s(g_sum[3].value, sizeof g_sum[3].value, _TRUNCATE,
                            LT("gui.play_sum_fxonly"), act);
            }
            if (warn || bad) {
                size_t n = strlen(g_sum[3].value);
                _snprintf_s(g_sum[3].value + n, sizeof g_sum[3].value - n, _TRUNCATE,
                            LT("gui.play_sum_packwarn"), warn + bad);
            }
            g_sum[3].col = (warn || bad) ? C_WARN : C_OK;
        }
    }
    g_sum[1].label = LT("gui.page_graphics");
    gfx_state_name(&s, g_sum[1].value, sizeof g_sum[1].value); /* W173: ein Preset */
    g_sum[1].col = C_OK;
    g_sum[4].label = LT("gui.page_controls");
    if (s.input_profile[0]) {
        _snprintf_s(g_sum[4].value, sizeof g_sum[4].value, _TRUNCATE, LT("gui.sum_prof"), s.input_profile,
                    s.sbc_usb ? LT("gui.sum_sbc") : "");
    } else {
        _snprintf_s(g_sum[4].value, sizeof g_sum[4].value, _TRUNCATE, LT("gui.sum_legacy"),
                    s.sbc_usb ? LT("gui.sum_sbc") : "");
    }
    g_sum[4].col = g_rr[25].active ? C_WARN : C_OK; /* R25: Controller fehlt */
}

/* ------------------------------------------------------------------ */
/* W171 LAUNCHER-P2P3: Regelwerk anwenden (launcher_rules.c)            */
/* ------------------------------------------------------------------ */
/* Steuerelemente je Regel (Muster A/D/E sperren sie). Ein Element, das
 * mehrere Regeln trifft, ist nur frei, wenn keine davon sperrt.
 * W172: Packs-Seite neu (Segment, Kategorie-Grenzen, Pack-Grenze R31). */
#define KRC_MAX 13
static const struct {
    int rule;
    int ids[KRC_MAX];
} kRuleCtl[] = {
    { 1, { ID_MEDIA, ID_MEDIA_B, ID_XBE, ID_XBE_B, 0 } },
    { 3, { ID_START_BIG, ID_START_VR, ID_START, ID_START_MENU, 0 } },
    { 4, { ID_START_BIG, ID_START_VR, ID_START, ID_START_MENU, 0 } },
    { 5, { ID_START_VR, 0 } },
    { 7, { ID_PRESET, ID_W, ID_H, 0 } },
    { 8, { ID_FSHZ, 0 } },
    { 11, { ID_EDGE, ID_EDGE_W, ID_EDGE_S, 0 } },
    { 12, { ID_EDGE_W, ID_EDGE_S, 0 } },
    { 15, { ID_GFX_SHADOW, ID_GFX_SOFT, 0 } }, /* W173: Schatten brauchen Weltbeleuchtung */
    { 16, { ID_GFX_SOFT, 0 } },
    { 18, { ID_PK_LVL2, ID_PK_LVL4, ID_PK_CAT0, ID_PK_CAT0 + 1, ID_PK_CAT0 + 2,
            ID_PK_CAT0 + 3, ID_PK_CAT0 + 4, ID_PK_CAT0 + 5, ID_PK_CAT0 + 6,
            ID_PK_CAT0 + 7, ID_PK_CAT0 + 8, ID_PK_CAPRESET, 0 } },
    { 19, { ID_PK_CAT0, ID_PK_CAT0 + 1, ID_PK_CAT0 + 2, ID_PK_CAT0 + 3,
            ID_PK_CAT0 + 4, ID_PK_CAT0 + 5, ID_PK_CAT0 + 6, ID_PK_CAT0 + 7,
            ID_PK_CAT0 + 8, ID_PK_CAPRESET, 0 } },
    { 20, { ID_GFX_FX, 0 } }, /* W173: Effekt-Stufe (Muster D) */
    { 21, { ID_PK_TOGGLE, 0 } },
    { 23, { ID_CTRL_MOUSEOFF, 0 } },
    { 24, { ID_MOUSE_MODE, ID_MOUSE_SENS, 0 } },
    { 27, { ID_LIVE, 0 } },
    { 31, { ID_PK_ADD, 0 } },
};
#define KRULECTL_N ((int)(sizeof kRuleCtl / sizeof kRuleCtl[0]))

static void packs_env(lr_env_t *e); /* unten (Packs-Engine) */

/* Pfadvergleich ohne Gross/Klein und / gegen \ */
static int path_eq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a == '/' ? '\\' : *a, y = *b == '/' ? '\\' : *b;
        if (x >= 'A' && x <= 'Z') { x = (char)(x - 'A' + 'a'); }
        if (y >= 'A' && y <= 'Z') { y = (char)(y - 'A' + 'a'); }
        if (x != y) {
            return 0;
        }
    }
    return *a == *b;
}

/* W172/P6: abgeleitete Programmdatei (<Spielordner>/default.xbe), "" ohne
 * Ordner. */
static void xbe_derived(const launcher_settings_t *s, char *out, size_t cap)
{
    launcher_settings_t t = *s;
    t.xbe_path[0] = '\0';
    t.game_image[0] = '\0';
    lcfg_xbe_effective(&t, out, cap);
}

/* Spieldateien pruefen (nur Pfade; teuer: Abbild/XBE lesen -> Ergebnis
 * je Pfadsatz merken). */
static void rules_data(const launcher_settings_t *s, lr_env_t *e)
{
    static char key[2200];
    static int ok = -1;
    static char err[300];
    char k[2200], eff[1024];
    _snprintf_s(k, sizeof k, _TRUNCATE, "%s|%s|%s|%s|%s", s->game_image,
                s->xbe_path, s->media_root, s->hdd_root, sb_lang_code());
    if (ok < 0 || strcmp(k, key) != 0) {
        launcher_settings_t t;
        lcfg_defaults(&t);
        strcpy_s(t.game_image, sizeof t.game_image, s->game_image);
        strcpy_s(t.xbe_path, sizeof t.xbe_path, s->xbe_path);
        strcpy_s(t.media_root, sizeof t.media_root, s->media_root);
        strcpy_s(t.hdd_root, sizeof t.hdd_root, s->hdd_root);
        err[0] = '\0';
        ok = lcfg_validate(&t, err, sizeof err) == 0;
        strcpy_s(key, sizeof key, k);
    }
    e->data_ok = ok;
    strcpy_s(e->data_err, sizeof e->data_err, err);
    { /* W172 (R2): leeres Feld oder Feld = <Ordner>/default.xbe */
        char x[1200];
        xbe_derived(s, x, sizeof x);
        e->xbe_auto = s->media_root[0] &&
                      (!s->xbe_path[0] || path_eq(x, s->xbe_path)) &&
                      u8_GetFileAttributes(x) != INVALID_FILE_ATTRIBUTES;
    }
    lcfg_xbe_effective(s, eff, sizeof eff);
    e->game_for_check = s->game_image[0] != '\0' ||
                        (eff[0] && u8_GetFileAttributes(eff) !=
                                       INVALID_FILE_ATTRIBUTES);
}

/* W172: Pack Creator neben dem Launcher (sb_creator.exe im Exe-Ordner). */
static int creator_path(char *out, size_t cap)
{
    wchar_t w[MAX_PATH];
    char u8[1100], *s;
    DWORD n = GetModuleFileNameW(NULL, w, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return 0;
    }
    w_to_u8(w, u8, (int)sizeof u8);
    s = strrchr(u8, '\\');
    if (s) {
        *s = '\0';
    }
    _snprintf_s(out, cap, _TRUNCATE, "%s\\sb_creator.exe", u8);
    return u8_GetFileAttributes(out) != INVALID_FILE_ATTRIBUTES;
}

static void rules_env(lr_env_t *e, launcher_settings_t *s)
{
    *s = g_s;
    ui_to_settings(s);
    g_cur = *s; /* W172: Stand fuer das Zeichnen (Spielen, Erweitert) */
    if (g_env_fix) { /* Selbsttest: feste Umgebung */
        *e = *g_env_fix;
        e->s = s;
        g_tex_src = e->tex_packs + e->legacy_tex;
        g_data_ok = e->data_ok;
        strcpy_s(g_data_err, sizeof g_data_err, e->data_err);
        return;
    }
    memset(e, 0, sizeof *e);
    e->s = s;
    rules_data(s, e);
    e->has_ldr = g_has_ldr;
    e->vr_runtime = g_vr_present;
    e->sbc_found = g_sbc_rc < 0 ? -1 : g_sbc_rc == 0;
    e->display_hz = g_disp_hz;
    e->mon_w = g_mon_w;
    e->mon_h = g_mon_h;
    e->legacy_tex = g_rem_pkg == 1;
    strcpy_s(e->default_setup, sizeof e->default_setup, g_dflt_setup);
    if (g_creator < 0) {
        char cp[1200];
        g_creator = creator_path(cp, sizeof cp);
    }
    e->has_creator = g_creator == 1;
    packs_env(e);
    g_tex_src = e->tex_packs + e->legacy_tex; /* W173: Texturen-Zeile */
    g_data_ok = e->data_ok;
    strcpy_s(g_data_err, sizeof g_data_err, e->data_err);
}

/* W172/B5: Remaster-Bedienung sichtbar mit lokalem Remaster-Ordner (W116)
 * oder mindestens einem installierten Pack. */
static int rem_avail(void)
{
    return g_rem_pkg == 1 || packs_count() > 0;
}

/* W172/P6: Einrichtungskarte auf Spielen (statt Bereitschaft). */
static int play_setup_shown(void)
{
    return g_setup_force || g_rr[3].active;
}

/* W172: Sichtbarkeit der aktuellen Seite nachziehen (Zustand kann sich
 * durch Regeln/Packs aendern: Einrichtungskarte, leere Packs-Liste). */
static void ui_refresh_visibility(void)
{
    int i;
    for (i = 0; i < g_ui_n; i++) {
        uictl_t *u = &g_ui[i];
        int want, has;
        if (u->parent != g_hwnd) {
            continue;
        }
        want = ui_visible_on(u, g_page);
        has = (GetWindowLongW(u->h, GWL_STYLE) & WS_VISIBLE) != 0;
        if (want != has) {
            ShowWindow(u->h, want ? SW_SHOWNA : SW_HIDE);
        }
    }
}

/* ------------------------------------------------------------------ */
/* W172: Tooltips = Begruendung gesperrter Elemente (Muster A, Konzept  */
/* 5.1 "Tooltip gleichlautend"). Werkzeuge am Hauptfenster mit dem      */
/* Rechteck des Elements: gesperrte Kinder bekommen keine Maus, das     */
/* Hauptfenster schon (TTF_SUBCLASS).                                   */
/* ------------------------------------------------------------------ */
/* Text der sperrenden Regel eines Elements ("" = keine Sperre/sichtbar frei). */
static void tip_text(int id, char *out, size_t cap)
{
    int i, k;
    HWND h = ctl(id);
    if (id == ID_CTRL_BROWSER) { /* W179: immer aktiv, fester Hinweistext */
        if (h && (GetWindowLongW(h, GWL_STYLE) & WS_VISIBLE)) {
            strcpy_s(out, cap, LT("gui.keys_browser_tip"));
        } else {
            out[0] = '\0';
        }
        return;
    }
    out[0] = '\0';
    if (!h || !(GetWindowLongW(h, GWL_STYLE) & WS_VISIBLE) ||
        IsWindowEnabled(h)) {
        return;
    }
    for (i = 0; i < KRULECTL_N; i++) {
        for (k = 0; k < KRC_MAX && kRuleCtl[i].ids[k]; k++) {
            if (kRuleCtl[i].ids[k] == id && g_rr[kRuleCtl[i].rule].disable &&
                g_rr[kRuleCtl[i].rule].text[0]) {
                strcpy_s(out, cap, g_rr[kRuleCtl[i].rule].text);
                return;
            }
        }
    }
}

static void tips_place(void)
{
    int i, k;
    if (!g_tip) {
        return;
    }
    for (i = 0; i < KRULECTL_N; i++) {
        for (k = 0; k < KRC_MAX && kRuleCtl[i].ids[k]; k++) {
            uictl_t *u = ui_find(kRuleCtl[i].ids[k]);
            TTTOOLINFOW ti;
            if (!u) {
                continue;
            }
            memset(&ti, 0, sizeof ti);
            ti.cbSize = sizeof ti;
            ti.hwnd = g_hwnd;
            ti.uId = (UINT_PTR)u->id;
            ti.rect.left = SX(u->x);
            ti.rect.top = SX(u->y);
            ti.rect.right = SX(u->x + u->w);
            ti.rect.bottom = SX(u->y + u->h_);
            SendMessageW(g_tip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
        }
    }
    { /* W179: Rechteck des Browser-Knopfes nachfuehren */
        uictl_t *u = ui_find(ID_CTRL_BROWSER);
        if (u) {
            TTTOOLINFOW ti;
            memset(&ti, 0, sizeof ti);
            ti.cbSize = sizeof ti;
            ti.hwnd = g_hwnd;
            ti.uId = (UINT_PTR)u->id;
            ti.rect.left = SX(u->x);
            ti.rect.top = SX(u->y);
            ti.rect.right = SX(u->x + u->w);
            ti.rect.bottom = SX(u->y + u->h_);
            SendMessageW(g_tip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
        }
    }
    SendMessageW(g_tip, TTM_SETMAXTIPWIDTH, 0, SX(340));
}

static void tips_build(void)
{
    int i, k;
    g_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL,
                            WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0,
                            g_hwnd, NULL, GetModuleHandleW(NULL), NULL);
    if (!g_tip) {
        return;
    }
    for (i = 0; i < KRULECTL_N; i++) {
        for (k = 0; k < KRC_MAX && kRuleCtl[i].ids[k]; k++) {
            TTTOOLINFOW ti;
            memset(&ti, 0, sizeof ti);
            ti.cbSize = sizeof ti;
            ti.uFlags = TTF_SUBCLASS;
            ti.hwnd = g_hwnd;
            ti.uId = (UINT_PTR)kRuleCtl[i].ids[k];
            ti.lpszText = LPSTR_TEXTCALLBACKW;
            SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti); /* doppelt = egal */
            if (i == 0 && k == 0) { /* W179: Hinweis am Browser-Knopf (einmal) */
                TTTOOLINFOW tb;
                memset(&tb, 0, sizeof tb);
                tb.cbSize = sizeof tb;
                tb.uFlags = TTF_SUBCLASS;
                tb.hwnd = g_hwnd;
                tb.uId = (UINT_PTR)ID_CTRL_BROWSER;
                tb.lpszText = LPSTR_TEXTCALLBACKW;
                SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&tb);
            }
        }
    }
    tips_place();
}

/* TTN_GETDISPINFOW: Text erst beim Zeigen (Sprache/Regelstand aktuell). */
static void tips_dispinfo(NMTTDISPINFOW *di)
{
    static wchar_t wbuf[400];
    char t[400];
    tip_text((int)di->hdr.idFrom, t, sizeof t);
    u8_to_w(t, wbuf, 400);
    di->lpszText = wbuf;
}

/* Alle Regeln auswerten, Freigaben setzen; geaenderte Begruendungen ->
 * neu zeichnen. Einzige Stelle fuer EnableWindow der Hauptseiten. */
static void ui_apply_rules(void)
{
    lr_env_t e;
    launcher_settings_t s;
    lr_res_t old[LR_N];
    int i, k;
    if (!g_hwnd || !ctl(ID_START)) {
        return;
    }
    memcpy(old, g_rr, sizeof old);
    rules_env(&e, &s);
    lrules_eval(&e, g_rr);
    gfx_sync_ui(); /* W173: Bildrate im Namen, Benutzerdefiniert, Pack-Sperren */
    /* W172/P6: Einrichtungskarte bleibt offen, solange Spielen gezeigt wird
     * (kein Umspringen waehrend der Eingabe des Abbild-Pfads). */
    if (g_rr[3].active && g_page == P_PLAY) {
        g_setup_force = 1;
    }
    for (i = 0; i < KRULECTL_N; i++) {
        for (k = 0; k < KRC_MAX && kRuleCtl[i].ids[k]; k++) {
            int id = kRuleCtl[i].ids[k], j, m, en = 1;
            for (j = 0; j < KRULECTL_N && en; j++) {
                for (m = 0; m < KRC_MAX && kRuleCtl[j].ids[m]; m++) {
                    if (kRuleCtl[j].ids[m] == id && g_rr[kRuleCtl[j].rule].disable) {
                        en = 0;
                        break;
                    }
                }
            }
            if (ctl(id) && (IsWindowEnabled(ctl(id)) != FALSE) != en) {
                EnableWindow(ctl(id), en ? TRUE : FALSE);
            }
        }
    }
    ui_refresh_visibility();
    if (memcmp(old, g_rr, sizeof old) != 0) {
        redraw_all();
    }
}

/* Rueckfrage (Muster C, R14). Test-/Shot-Betrieb: nie ein Fenster. */
static int ui_confirm(const char *text, const char *title)
{
    if (g_o.shot != 0) {
        printf("[%s] %s -> %s\n", title, text, g_confirm_auto ? "ja" : "nein");
        return g_confirm_auto;
    }
    return ui_msgbox(g_hwnd, text, title, MB_ICONQUESTION | MB_YESNO) == IDYES;
}

/* ------------------------------------------------------------------ */
/* Speichern / Starten                                                 */
/* ------------------------------------------------------------------ */
static void user_cfg_path(char *out, size_t cap)
{
    lcfg_user_cfg_path(g_client_dir, out, cap); /* W150 USER-DATA */
}

static int do_save(int complain)
{
    char err[1024], upath[1200];
    ui_to_settings(&g_s);
    if (lcfg_validate(&g_s, err, sizeof err) != 0) {
        set_status_k(err, 2);
        if (complain) {
            ui_msgbox(g_hwnd, err, LT("gui.msg_checkdata"),
                      MB_ICONWARNING | MB_OK);
        }
        return 1;
    }
    user_cfg_path(upath, sizeof upath);
    if (lcfg_save_user(upath, &g_s) != 0) {
        _snprintf_s(err, sizeof err, _TRUNCATE, LT("gui.msg_nowrite"), upath);
        set_status_k(err, 2);
        if (complain) {
            ui_msgbox(g_hwnd, err, LT("gui.btn_save"), MB_ICONERROR | MB_OK);
        }
        return 1;
    }
    return 0;
}

/* GUI-Start (W26/W79): pruefen, user.cfg + Spielerbetrieb-Block, Client.
 * dry = 1: alles ausser dem Clientstart (Test --gui-start-dry).
 * W159: vr = 1: "Spielen in VR" (vr_enable=1 + Vorschau); vr = 0:
 * Bildschirm (vr_enable=0 immer, egal was gespeichert ist). Die --set-Kette
 * gilt nur fuer diesen Start, user.cfg bleibt unveraendert. */
static char g_start_args[128]; /* W171: --set-Kette des letzten Starts */
static int do_start(int dry, int vr)
{
    char err[1400], set_args[128];
    if (!g_has_ldr) {
        _snprintf_s(err, sizeof err, _TRUNCATE, LT("gui.msg_noldr"),
                    g_client_dir);
        set_status_k(err, 2);
        ui_msgbox(g_hwnd, err, LT("gui.btn_start"), MB_ICONERROR | MB_OK);
        return 1;
    }
    if (do_save(1) != 0) {
        return 1;
    }
    {
        char upath[1200];
        user_cfg_path(upath, sizeof upath);
        if (lcfg_write_player_block(upath) != 0) {
            _snprintf_s(err, sizeof err, _TRUNCATE, LT("gui.msg_nowrite"), upath);
            set_status_k(err, 2);
            ui_msgbox(g_hwnd, err, LT("gui.btn_start"), MB_ICONERROR | MB_OK);
            return 1;
        }
    }
    app_vr_set_args(vr, g_s.vr_preview, set_args, sizeof set_args);
    strcpy_s(g_start_args, sizeof g_start_args, set_args); /* W171: Trockenlauf-Ausgabe */
    if (dry) {
        set_status_k(LT("gui.msg_dry"),
                     1);
        return 0;
    }
    {
        char xbe[1024]; /* W172/P6: im Ordner-Modus abgeleitet */
        lcfg_xbe_effective(&g_s, xbe, sizeof xbe);
        if (app_launch_client_set(g_client_dir, xbe, set_args, err,
                                  sizeof err) != 0) {
            set_status_k(err, 2);
            ui_msgbox(g_hwnd, err, LT("gui.btn_start"), MB_ICONERROR | MB_OK);
            return 1;
        }
    }
    {
        char st[1500];
        _snprintf_s(st, sizeof st, _TRUNCATE, "%s user.cfg gespeichert.", err);
        set_status_k(st, 1);
    }
    return 0;
}

static void browse_xbe(void)
{
    OPENFILENAMEW ofn;
    static wchar_t file[1024];
    char cur[1024];
    ui_gettext(ctl(ID_XBE), cur, sizeof cur);
    u8_to_w(cur, file, 1024);
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFile = file;
    ofn.nMaxFile = 1024;
    { /* W142: Filter/Titel uebersetzt (NULs baut der Code, nicht die Datei). */
        static wchar_t filter[256], title[128];
        wchar_t f1[128], f2[64];
        wchar_t *w;
        size_t n1, n2;
        u8_to_w(LT("gui.dlg_xbe_filter"), f1, 128);
        u8_to_w(LT("gui.dlg_allfiles"), f2, 64);
        u8_to_w(LT("gui.dlg_xbe_title"), title, 128);
        w = filter;
        n1 = wcslen(f1) + 1;
        n2 = wcslen(f2) + 1;
        if (n1 + 18 + n2 + 4 < 256) {
            wcscpy_s(w, 256, f1);
            w += n1;
            wcscpy_s(w, 256 - n1, L"default.xbe;*.xbe");
            w += 18;
            wcscpy_s(w, 256 - n1 - 18, f2);
            w += n2;
            wcscpy_s(w, 256 - n1 - 18 - n2, L"*.*");
            w[4] = 0; /* Doppel-NUL am Ende (Puffer wird wiederverwendet) */
        } else {
            wcscpy_s(filter, 256, L"default.xbe");
        }
        ofn.lpstrFilter = filter;
        ofn.lpstrTitle = title;
    }
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) {
        SetWindowTextW(ctl(ID_XBE), file);
    }
}

/* W150 ISO-DIRECT: Spiel-Abbild waehlen und pruefen (gleicher Leser wie der Client). */
static void check_image_ex(int status)
{
    char p[1024];
    ui_gettext(ctl(ID_IMAGE), p, sizeof p);
    if (!p[0]) {
        g_img_msg[0] = 0;
        g_img_ok = 0;
    } else {
        g_img_ok = lcfg_image_check(p, g_img_msg, sizeof g_img_msg) == 0;
        if (status) {
            set_status_k(g_img_msg, g_img_ok ? 1 : 2);
        }
    }
    redraw_rect(CX0, CY0, CW, 280);
}

static void check_image(void)
{
    check_image_ex(1);
}

static void browse_image(void)
{
    OPENFILENAMEW ofn;
    static wchar_t file[1024], filter[200], title[128];
    char cur[1024];
    wchar_t f1[128], f2[64];
    size_t n1, n2;
    ui_gettext(ctl(ID_IMAGE), cur, sizeof cur);
    u8_to_w(cur, file, 1024);
    u8_to_w(LT("gui.dlg_image_filter"), f1, 128);
    u8_to_w(LT("gui.dlg_allfiles"), f2, 64);
    u8_to_w(LT("gui.dlg_image_title"), title, 128);
    n1 = wcslen(f1) + 1;
    n2 = wcslen(f2) + 1;
    memset(filter, 0, sizeof filter);
    if (n1 + 13 + n2 + 5 < 200) { /* "<f1>|*.iso;*.xiso|<f2>|*.*||" */
        wcscpy_s(filter, 200, f1);
        wcscpy_s(filter + n1, 200 - n1, L"*.iso;*.xiso");
        wcscpy_s(filter + n1 + 13, 200 - n1 - 13, f2);
        wcscpy_s(filter + n1 + 13 + n2, 200 - n1 - 13 - n2, L"*.*");
    }
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFile = file;
    ofn.nMaxFile = 1024;
    ofn.lpstrFilter = filter;
    ofn.lpstrTitle = title;
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) {
        SetWindowTextW(ctl(ID_IMAGE), file);
        check_image();
        if (g_img_ok) { /* W172/P6: gueltig -> Bereitschaft statt Einrichtung */
            g_setup_force = 0;
        }
    }
}

/* W172/P6: Hinweistext im leeren Programmdatei-Feld ("automatisch:
 * <Spielordner>\default.xbe"). */
static void xbe_cue_update(void)
{
    char m[1024], x[1300], t[1500];
    wchar_t w[1500];
    HWND e = ctl(ID_XBE);
    launcher_settings_t s;
    if (!e) {
        return;
    }
    ui_gettext(ctl(ID_MEDIA), m, sizeof m);
    lcfg_defaults(&s);
    s.game_image[0] = s.xbe_path[0] = '\0';
    strcpy_s(s.media_root, sizeof s.media_root, m);
    lcfg_xbe_effective(&s, x, sizeof x);
    if (x[0]) {
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.adv_xbe_cue"), x);
    } else {
        strcpy_s(t, sizeof t, LT("gui.adv_xbe_cue0"));
    }
    u8_to_w(t, w, 1500);
    SendMessageW(e, EM_SETCUEBANNER, TRUE, (LPARAM)w);
}

/* W172/P7: Schalter "Beim Launcher-Start laden" = gewaehlte Einstellung
 * ist die gespeicherte Standard-Einstellung (profiles/standard.txt). */
static void setup_auto_sync(int from_list)
{
    char name[128];
    HWND cb = ctl(ID_PROF_COMBO);
    if (!cb) {
        return;
    }
    if (from_list) { /* CBN_SELCHANGE: Text steht noch nicht im Feld */
        wchar_t w[128];
        int sel = ComboBox_GetCurSel(cb);
        name[0] = '\0';
        if (sel >= 0 && ComboBox_GetLBText(cb, sel, w) != CB_ERR) {
            w_to_u8(w, name, (int)sizeof name);
        }
    } else {
        ui_gettext(cb, name, sizeof name);
        trim_u8(name);
    }
    ui_set_checked(ID_PROF_DFLT, name[0] && strcmp(name, g_dflt_setup) == 0);
}

static void browse_media(void)
{
    BROWSEINFOW bi;
    LPITEMIDLIST pidl;
    static wchar_t dir[MAX_PATH];
    memset(&bi, 0, sizeof bi);
    bi.hwndOwner = g_hwnd;
    {
        static wchar_t title[160];
        u8_to_w(LT("gui.dlg_media_title"), title, 160);
        bi.lpszTitle = title;
    }
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        if (SHGetPathFromIDListW(pidl, dir)) {
            char cur[1024];
            SetWindowTextW(ctl(ID_MEDIA), dir);
            /* W172 (R2): Programmdatei wird aus dem Spielordner abgeleitet;
             * ein Feld, das auf keine Datei zeigt, wird geleert (= automatisch). */
            ui_gettext(ctl(ID_XBE), cur, sizeof cur);
            if (cur[0] && u8_GetFileAttributes(cur) == INVALID_FILE_ATTRIBUTES) {
                ui_settext(ctl(ID_XBE), "");
            }
            xbe_cue_update();
        }
        CoTaskMemFree(pidl);
    }
}

/* ------------------------------------------------------------------ */
/* Belegungs-Dialog (W78, jetzt W-API + dunkel)                        */
/* ------------------------------------------------------------------ */
#define IDB_LIST 501
#define IDB_CUR 502
#define IDB_SET 503
#define IDB_ADD 504
#define IDB_MX 505
#define IDB_MY 506
#define IDB_DEL 507
#define IDB_RST 508
#define IDB_PROMPT 509
#define IDB_CONF 510
#define IDB_NAME 511
#define IDB_SAVE 512
#define IDB_CHECK 513
#define IDB_RESULT 514
#define IDB_CLOSE 515
#define IDB_TITLE 516

static lctrl_bind_t g_bind, g_bind_base;
static char g_bind_base_name[64];
static char g_bind_saved[64];
static int g_bind_sel = -1;
static int g_cap_mode; /* 0 = idle, 1 = ersetzen, 2 = hinzu */
static lctrl_pad_t g_cap_prev;
static int g_cap_have_pad;
static int g_bind_done;
static HWND g_bind_hd;
/* W159: Original-Controller als Quelle (eigener Thread: sbc_usb_poll
 * blockiert bis 100 ms, der UI-Thread darf nie warten). */
#define WM_APP_BIND_SBC (WM_APP + 8) /* w = bi 0..38 (Flanke) */
static HANDLE g_cap_sbc_thr;
static volatile LONG g_cap_sbc_stop;

static DWORD WINAPI bind_sbc_thread(LPVOID arg)
{
    HWND hd = (HWND)arg;
    sbc_usb_dev_t *dev = NULL;
    sbc_state_t st, base;
    int have = 0;
    if (sbc_usb_open(&dev) != SBC_OK || !dev) {
        return 1; /* kein Geraet: still, andere Quellen laufen weiter */
    }
    while (InterlockedCompareExchange(&g_cap_sbc_stop, 0, 0) == 0) {
        int i, bi = -1;
        if (sbc_usb_poll(dev, &st) != SBC_OK) {
            Sleep(50); /* Fehler/Abzug: nicht heiss drehen */
            continue;
        }
        if (!have) {
            base = st;
            have = 1;
            continue;
        }
        for (i = 0; i < 39 && bi < 0; i++) { /* Flanke je Taste */
            int was = (base.buttons[i / 16] >> (i % 16)) & 1;
            int is = (st.buttons[i / 16] >> (i % 16)) & 1;
            if (is && !was) {
                bi = i;
            }
        }
        base = st;
        if (bi >= 0) {
            PostMessageW(hd, WM_APP_BIND_SBC, (WPARAM)bi, 0);
        }
    }
    sbc_usb_close(dev);
    return 0;
}

static HWND bctl(HWND hd, int id)
{
    return GetDlgItem(hd, id);
}

static void bind_refresh_cur(HWND hd)
{
    char t[512], row[420];
    HWND lb = bctl(hd, IDB_LIST);
    int sel = ListBox_GetCurSel(lb);
    g_bind_sel = (sel >= 0) ? (int)ListBox_GetItemData(lb, sel) : -1;
    if (g_bind_sel < 0 || g_bind_sel >= LCTRL_ELEM_N) {
        int ids[6] = { IDB_SET, IDB_ADD, IDB_MX, IDB_MY, IDB_DEL, IDB_RST };
        int i;
        ui_settext(bctl(hd, IDB_CUR), LT("gui.bind_noelem"));
        for (i = 0; i < 6; i++) {
            EnableWindow(bctl(hd, ids[i]), FALSE);
        }
        return;
    }
    lctrl_bind_row_text(&g_bind, g_bind_sel, row, sizeof row);
    _snprintf_s(t, sizeof t, _TRUNCATE, "%s (%s): %s",
                kCtrlElems[g_bind_sel].key, LT(kCtrlElems[g_bind_sel].lkey), row);
    ui_settext(bctl(hd, IDB_CUR), t);
    {
        int is_axis = kCtrlElems[g_bind_sel].kind == LCTRL_AXIS;
        int is_aimsight =
            is_axis && (strncmp(kCtrlElems[g_bind_sel].key, "aim_", 4) == 0 ||
                        strncmp(kCtrlElems[g_bind_sel].key, "sight_", 6) == 0);
        int en = !g_cap_mode;
        EnableWindow(bctl(hd, IDB_SET), en);
        EnableWindow(bctl(hd, IDB_ADD), en);
        EnableWindow(bctl(hd, IDB_MX), en && is_aimsight);
        EnableWindow(bctl(hd, IDB_MY), en && is_aimsight);
        EnableWindow(bctl(hd, IDB_DEL), en);
        EnableWindow(bctl(hd, IDB_RST), en);
        EnableWindow(bctl(hd, IDB_LIST), en);
        EnableWindow(bctl(hd, IDB_SAVE), en);
        EnableWindow(bctl(hd, IDB_CHECK), en);
    }
}

static void list_add(HWND lb, const char *u8, LPARAM data, int with_data)
{
    wchar_t *w = u8_wdup(u8);
    int at = (int)SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)(w ? w : L""));
    free(w);
    if (with_data && at >= 0) {
        SendMessageW(lb, LB_SETITEMDATA, (WPARAM)at, data);
    }
}

static void bind_refresh_conf(HWND hd)
{
    char conf[32][160];
    HWND lb = bctl(hd, IDB_CONF);
    int n, i;
    ListBox_ResetContent(lb);
    n = lctrl_bind_conflicts(&g_bind, conf, 32);
    if (n == 0) {
        list_add(lb, LT("gui.bind_noconf"), 0, 0);
        return;
    }
    for (i = 0; i < n; i++) {
        list_add(lb, conf[i], 0, 0);
    }
}

static void bind_end_capture(HWND hd, const char *prompt)
{
    g_cap_mode = 0;
    KillTimer(hd, 7);
    if (g_cap_sbc_thr) { /* W159: SBC-Thread stoppen (max. 1 Poll ~100 ms) */
        InterlockedExchange(&g_cap_sbc_stop, 1);
        WaitForSingleObject(g_cap_sbc_thr, 500);
        CloseHandle(g_cap_sbc_thr);
        g_cap_sbc_thr = NULL;
    }
    ui_settext(bctl(hd, IDB_PROMPT),
         prompt ? prompt : LT("gui.bind_pick"));
    ui_settext(bctl(hd, IDB_CLOSE), LT("gui.btn_close"));
    bind_refresh_cur(hd);
}

static void bind_start_capture(HWND hd, int mode)
{
    char t[256], sbc_st[256];
    lctrl_pad_t cur;
    if (g_bind_sel < 0 || g_bind_sel >= LCTRL_ELEM_N) {
        return;
    }
    g_cap_mode = mode;
    g_cap_have_pad = lctrl_pad_first(&cur) >= 0;
    g_cap_prev = cur;
    /* W159: Original-Controller nur bei angeschlossenem Geraet (kurz
     * oeffnen wie die Statuszeile, Poll laeuft im Thread). */
    g_cap_sbc_thr = NULL;
    if (lctrl_sbc_status(sbc_st, sizeof sbc_st) == SBC_OK) {
        g_cap_sbc_stop = 0;
        g_cap_sbc_thr = CreateThread(NULL, 0, bind_sbc_thread, hd, 0, NULL);
    }
    _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.bind_press"),
                kCtrlElems[g_bind_sel].key, LT(kCtrlElems[g_bind_sel].lkey));
    ui_settext(bctl(hd, IDB_PROMPT), t);
    ui_settext(bctl(hd, IDB_CLOSE), LT("gui.btn_cancel"));
    bind_refresh_cur(hd);
    SetTimer(hd, 7, 50, NULL);
}

/* W159: 0 = uebernommen, 1 = Fehler (voll/doppelt), 2 = Warnung abgelehnt. */
static int bind_apply(HWND hd, const char *src)
{
    int rc;
    if (g_bind_sel < 0) {
        return 1;
    }
    if (lctrl_bind_alt_is_sbc(src)) {
        /* Doppelbelegung: jede Original-Controller-Taste hat eine
         * Spielfunktion (CONTROLLER.md S8) und behaelt sie - nur mit
         * ausdruecklicher Bestaetigung des Nutzers belegen. */
        char q[640], fn[160];
        const char *nm = src + 4; /* hinter "SBC:" */
        int bi = lctrl_bind_elem_index(nm);
        if (bi >= 0 && bi < 39) {
            _snprintf_s(fn, sizeof fn, _TRUNCATE, LT("gui.bind_sbc_fn"),
                        LT(kCtrlElems[bi].lkey));
        } else {
            fn[0] = '\0';
        }
        _snprintf_s(q, sizeof q, _TRUNCATE, LT("gui.bind_sbcwarn"), nm,
                    fn, LT(kCtrlElems[g_bind_sel].lkey));
        if (ui_msgbox(hd, q, LT("gui.bind_sbctitle"),
                      MB_YESNO | MB_ICONWARNING) != IDYES) {
            return 2;
        }
    }
    rc = g_cap_mode == 2 ? lctrl_bind_add(&g_bind, g_bind_sel, src)
                         : lctrl_bind_set(&g_bind, g_bind_sel, src);
    if (rc == 0) {
        char t[256];
        _snprintf_s(t, sizeof t, _TRUNCATE, "%s = %s.",
                    kCtrlElems[g_bind_sel].key, src);
        ui_settext(bctl(hd, IDB_RESULT), t);
    }
    return rc;
}

static void bind_ana_default(int idx, const char *src, char *out, size_t cap)
{
    const char *key = kCtrlElems[idx].key;
    int is_y = strcmp(src, "LY") == 0 || strcmp(src, "RY") == 0;
    if (strcmp(src, "LT") == 0 || strcmp(src, "RT") == 0) {
        _snprintf_s(out, cap, _TRUNCATE, "PAD:%s:dz=0.05", src);
    } else if (strncmp(key, "aim_", 4) == 0) {
        _snprintf_s(out, cap, _TRUNCATE, "PAD:%s:dz=0.15:curve=1.5%s", src,
                    is_y ? ":inv=1" : "");
    } else if (strncmp(key, "pedal", 5) == 0) {
        _snprintf_s(out, cap, _TRUNCATE, "PAD:%s:dz=0.15:curve=1.5", src);
    } else {
        _snprintf_s(out, cap, _TRUNCATE, "PAD:%s:dz=0.12:curve=1.3%s", src,
                    is_y ? ":inv=1" : "");
    }
}

static const char *kCapFull = "gui.bind_dup"; /* Sprachschluessel, Anzeige via LT() */

/* W159: Capture-Ergebnis eintragen (alle Quellen teilen sich das). */
static void bind_cap_done(HWND hd, int rc)
{
    if (rc == 0) {
        bind_end_capture(hd, NULL);
        bind_refresh_conf(hd);
    } else if (rc == 2) {
        /* Warnung abgelehnt: weiter lauschen. */
        ui_settext(bctl(hd, IDB_PROMPT), LT("gui.bind_sbckeep"));
    } else {
        ui_settext(bctl(hd, IDB_PROMPT), LT(kCapFull));
    }
}

static void bind_cap_key(HWND hd, int vk, LPARAM lp)
{
    char nm[32], src[48];
    if (vk == VK_ESCAPE) {
        bind_end_capture(hd, LT("gui.bind_abort"));
        return;
    }
    if (vk == VK_CONTROL) { /* W110: links/rechts per Ext-Bit (wie Client) */
        vk = (lp & 0x01000000) ? VK_RCONTROL : VK_LCONTROL;
    }
    if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON) {
        return;
    }
    lctrl_key_name(vk, nm, sizeof nm);
    _snprintf_s(src, sizeof src, _TRUNCATE, "KEY:%s", nm);
    bind_cap_done(hd, bind_apply(hd, src));
}

static void bind_cap_mouse(HWND hd, UINT msg, WPARAM wp)
{
    const char *nm = msg == WM_LBUTTONDOWN   ? "MOUSE:LEFT"
                     : msg == WM_RBUTTONDOWN ? "MOUSE:RIGHT"
                     : msg == WM_MBUTTONDOWN ? "MOUSE:MIDDLE"
                     : HIWORD(wp) == XBUTTON2 ? "MOUSE:X2" : "MOUSE:X1";
    bind_cap_done(hd, bind_apply(hd, nm));
}

/* W159: Original-Controller-Taste aus dem Capture-Thread (WM_APP_BIND_SBC). */
static void bind_cap_sbc(HWND hd, int bi)
{
    const char *nm = lctrl_sbc_btn_name(bi);
    char src[32];
    if (!g_cap_mode || !nm) {
        return;
    }
    _snprintf_s(src, sizeof src, _TRUNCATE, "SBC:%s", nm);
    bind_cap_done(hd, bind_apply(hd, src));
}

static void bind_cap_pad(HWND hd)
{
    lctrl_pad_t cur;
    char nm[16];
    if (!g_cap_have_pad || lctrl_pad_first(&cur) < 0) {
        return;
    }
    if (lctrl_pad_new_press(&g_cap_prev, &cur, nm, sizeof nm)) {
        char src[32];
        _snprintf_s(src, sizeof src, _TRUNCATE, "PAD:%s", nm);
        g_cap_prev = cur;
        bind_cap_done(hd, bind_apply(hd, src));
        return;
    }
    if (g_bind_sel >= 0 && kCtrlElems[g_bind_sel].kind == LCTRL_AXIS &&
        lctrl_pad_deflected(&cur, nm, sizeof nm)) {
        char src[64];
        bind_ana_default(g_bind_sel, nm, src, sizeof src);
        g_cap_prev = cur;
        bind_cap_done(hd, bind_apply(hd, src));
        return;
    }
    g_cap_prev = cur;
}

static void bind_save(HWND hd)
{
    char rest[64], fn[72], pname[68], dir[1100], path[1200], err[512];
    ui_gettext(bctl(hd, IDB_NAME), rest, sizeof rest);
    trim_u8(rest);
    if (!lctrl_user_rest_ok(rest)) {
        ui_settext(bctl(hd, IDB_RESULT), LT("gui.bind_badname"));
        return;
    }
    lctrl_user_filename(rest, fn, sizeof fn);
    strcpy_s(pname, sizeof pname, fn);
    pname[strlen(pname) - 4] = '\0';
    if (lctrl_name_shipped(pname)) {
        ui_settext(bctl(hd, IDB_RESULT), LT("gui.bind_shipped"));
        return;
    }
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s/profiles", g_client_dir);
    lcfg_mkdir_p(dir);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s/%s", dir, fn);
    if (u8_GetFileAttributes(path) != INVALID_FILE_ATTRIBUTES) {
        char q[300];
        _snprintf_s(q, sizeof q, _TRUNCATE, LT("gui.bind_overwrite"),
                    pname);
        if (ui_msgbox(hd, q, LT("gui.title_profsave"),
                      MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) != IDYES) {
            return;
        }
    }
    if (lctrl_bind_save(path, &g_bind, g_bind_base_name, err, sizeof err) !=
        0) {
        ui_settext(bctl(hd, IDB_RESULT), err);
        return;
    }
    strcpy_s(g_bind_saved, sizeof g_bind_saved, pname);
    {
        char t[256];
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.bind_saved"), fn);
        ui_settext(bctl(hd, IDB_RESULT), t);
    }
    refresh_ctrl_profiles(pname);
    update_ctrl_overview();
}

static void bind_check(HWND hd)
{
    char t[512];
    if (!g_bind_saved[0]) {
        ui_settext(bctl(hd, IDB_RESULT), LT("gui.bind_savefirst"));
        return;
    }
    ui_settext(bctl(hd, IDB_RESULT), LT("gui.bind_checking"));
    UpdateWindow(hd);
    (void)lctrl_run_dump(g_client_dir, g_bind_saved, t, sizeof t);
    ui_settext(bctl(hd, IDB_RESULT), t);
}

static int bind_shot_save(HWND hd);

static LRESULT CALLBACK bind_proc(HWND hd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (id == IDB_CLOSE || id == IDCANCEL) {
            if (g_cap_mode) {
                bind_end_capture(hd, LT("gui.bind_abort"));
            } else {
                g_bind_done = 1;
            }
        } else if (id == IDB_LIST && code == LBN_SELCHANGE) {
            if (g_cap_mode) {
                bind_end_capture(hd, LT("gui.bind_abort_sel"));
            }
            bind_refresh_cur(hd);
        } else if (id == IDB_SET) {
            bind_start_capture(hd, 1);
        } else if (id == IDB_ADD) {
            bind_start_capture(hd, 2);
        } else if (id == IDB_MX || id == IDB_MY) {
            if (g_bind_sel >= 0) {
                char src[40];
                const char *mode =
                    strncmp(kCtrlElems[g_bind_sel].key, "aim_", 4) == 0
                        ? "aim"
                        : "sight";
                _snprintf_s(src, sizeof src, _TRUNCATE, "MOUSE:%s:mode=%s",
                            id == IDB_MX ? "X" : "Y", mode);
                lctrl_bind_set(&g_bind, g_bind_sel, src);
                bind_refresh_cur(hd);
                bind_refresh_conf(hd);
            }
        } else if (id == IDB_DEL) {
            lctrl_bind_clear(&g_bind, g_bind_sel);
            bind_refresh_cur(hd);
            bind_refresh_conf(hd);
        } else if (id == IDB_RST) {
            if (g_bind_sel >= 0) {
                g_bind.row[g_bind_sel] = g_bind_base.row[g_bind_sel];
                bind_refresh_cur(hd);
                bind_refresh_conf(hd);
            }
        } else if (id == IDB_SAVE) {
            bind_save(hd);
        } else if (id == IDB_CHECK) {
            bind_check(hd);
        }
        return 0;
    }
    case WM_DRAWITEM:
        draw_item((const DRAWITEMSTRUCT *)lp);
        return TRUE;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORSTATIC:
        return ctlcolor((HDC)wp, (HWND)lp, msg == WM_CTLCOLORSTATIC);
    case WM_ERASEBKGND: {
        RECT r;
        GetClientRect(hd, &r);
        if (!g_br_dialog) {
            g_br_dialog = CreateSolidBrush(C_DIALOG);
        }
        FillRect((HDC)wp, &r, g_br_dialog);
        return 1;
    }
    case WM_TIMER:
        if (wp == 7 && g_cap_mode) {
            bind_cap_pad(hd);
        } else if (wp == 9 && g_o.shot == 3) {
            KillTimer(hd, 9);
            bind_shot_save(hd);
            g_bind_done = 1;
        }
        return 0;
    case WM_APP_BIND_SBC: /* W159: Original-Controller-Taste (Thread) */
        if (g_cap_mode) {
            bind_cap_sbc(hd, (int)wp);
        }
        return 0;
    case WM_CLOSE:
        if (g_cap_mode) {
            bind_end_capture(hd, LT("gui.bind_abort"));
        } else {
            g_bind_done = 1;
        }
        return 0;
    }
    return DefWindowProcW(hd, msg, wp, lp);
}

static void bind_build(HWND hd, const char *base_name)
{
    HWND lb;
    char t[200];
    int i;
    _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.bind_base"),
                base_name && *base_name ? base_name : LT("gui.bind_empty"), LCTRL_ELEM_N);
    mk(hd, K_STATIC, IDB_TITLE, PG_DIALOG, 12, 10, 620, 18, t, WS_VISIBLE);
    lb = mk(hd, K_LIST, IDB_LIST, PG_DIALOG, 12, 34, 250, 400, "", WS_VISIBLE);
    for (i = 0; i < LCTRL_ELEM_N; i++) {
        char e[128];
        if (kCtrlElems[i].kind == LCTRL_ACT && /* W159: Gruppe VR */
            (i == 0 || kCtrlElems[i - 1].kind != LCTRL_ACT)) {
            list_add(lb, LT("gui.bind_group_vr"), (LPARAM)-2, 1);
        }
        _snprintf_s(e, sizeof e, _TRUNCATE, "%s " U8_DASH " %s",
                    kCtrlElems[i].key, LT(kCtrlElems[i].lkey));
        list_add(lb, e, (LPARAM)i, 1);
    }
    ListBox_SetCurSel(lb, 0);
    mk(hd, K_STATIC, -1, PG_DIALOG, 274, 34, 350, 18, LT("gui.bind_current"),
       WS_VISIBLE);
    mk(hd, K_STATIC, IDB_CUR, PG_DIALOG, 274, 54, 354, 36, "", WS_VISIBLE);
    mk(hd, K_ACCENT, IDB_SET, PG_DIALOG, 274, 94, 172, 30, LT("gui.bind_replace"), WS_VISIBLE);
    mk(hd, K_BTN, IDB_ADD, PG_DIALOG, 456, 94, 172, 30, LT("gui.bind_add"), WS_VISIBLE);
    mk(hd, K_BTN, IDB_MX, PG_DIALOG, 274, 130, 108, 30, LT("gui.bind_mousex"), WS_VISIBLE);
    mk(hd, K_BTN, IDB_MY, PG_DIALOG, 392, 130, 108, 30, LT("gui.bind_mousey"), WS_VISIBLE);
    mk(hd, K_BTN, IDB_DEL, PG_DIALOG, 510, 130, 118, 30, LT("gui.btn_delete"),
       WS_VISIBLE);
    mk(hd, K_BTN, IDB_RST, PG_DIALOG, 274, 166, 172, 30, LT("gui.bind_reset"), WS_VISIBLE);
    mk(hd, K_STATIC, IDB_PROMPT, PG_DIALOG, 274, 204, 354, 40, LT("gui.bind_pick"), WS_VISIBLE);
    mk(hd, K_STATIC, -1, PG_DIALOG, 274, 248, 354, 18, LT("gui.bind_conflicts"), WS_VISIBLE);
    mk(hd, K_LIST, IDB_CONF, PG_DIALOG, 274, 268, 354, 88, "", WS_VISIBLE);
    mk(hd, K_STATIC, -1, PG_DIALOG, 274, 368, 72, 18, LT("gui.bind_name"),
       WS_VISIBLE);
    mk(hd, K_EDIT, IDB_NAME, PG_DIALOG, 350, 364, 190, 24, "",
       WS_VISIBLE | WS_BORDER);
    mk(hd, K_STATIC, -1, PG_DIALOG, 546, 368, 40, 18, ".ini", WS_VISIBLE);
    mk(hd, K_ACCENT, IDB_SAVE, PG_DIALOG, 274, 396, 170, 30, LT("gui.title_profsave"), WS_VISIBLE);
    mk(hd, K_BTN, IDB_CHECK, PG_DIALOG, 454, 396, 174, 30, LT("gui.btn_check"), WS_VISIBLE);
    mk(hd, K_STATIC, IDB_RESULT, PG_DIALOG, 274, 432, 354, 34, "", WS_VISIBLE);
    mk(hd, K_BTN, IDB_CLOSE, PG_DIALOG, 12, 446, 130, 30, LT("gui.btn_close"), WS_VISIBLE);
}

/* Offscreen-Position (rechts ausserhalb aller Monitore). */
static void offscreen_pos(int *x, int *y)
{
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    *x = vx + vw + 4000;
    *y = vy + 100;
}

static int is_offscreen(HWND h)
{
    RECT r, v;
    GetWindowRect(h, &r);
    v.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    v.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    v.right = v.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    v.bottom = v.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return r.left >= v.right || r.right <= v.left || r.top >= v.bottom ||
           r.bottom <= v.top;
}

static void bind_dialog_modal(const char *base_name, const char *base_path)
{
    static int reg;
    HWND hd;
    RECT rc = { 0, 0, 0, 0 };
    MSG msg;
    int ui_mark = g_ui_n;
    int shot = g_o.shot != 0;
    DWORD exs = shot ? (WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW) : 0;
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    if (!reg) {
        WNDCLASSW wc;
        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = bind_proc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = L"SbBind";
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        RegisterClassW(&wc);
        reg = 1;
    }
    lctrl_bind_init(&g_bind);
    if (base_path && *base_path) {
        lctrl_bind_load_file(base_path, &g_bind);
    }
    g_bind_base = g_bind;
    strcpy_s(g_bind_base_name, sizeof g_bind_base_name,
             base_name && *base_name ? base_name : "");
    g_bind_saved[0] = '\0';
    g_bind_sel = -1;
    g_cap_mode = 0;
    g_bind_done = 0;
    rc.right = SX(640);
    rc.bottom = SX(488);
    AdjustWindowRectEx(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE,
                       exs);
    if (shot) {
        offscreen_pos(&x, &y);
    } else if (g_hwnd) {
        RECT pr;
        GetWindowRect(g_hwnd, &pr);
        x = pr.left + ((pr.right - pr.left) - (rc.right - rc.left)) / 2;
        y = pr.top + ((pr.bottom - pr.top) - (rc.bottom - rc.top)) / 2;
    }
    {
        wchar_t title[64];
        u8_to_w(LT("gui.bind_title"), title, 64);
        hd = CreateWindowExW(exs, L"SbBind", title,
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, x, y,
                             rc.right - rc.left, rc.bottom - rc.top, g_hwnd, NULL,
                             GetModuleHandleW(NULL), NULL);
    }
    if (!hd) {
        ui_msgbox(g_hwnd, LT("gui.bind_nodlg"), LT("gui.bind_btn"),
                  MB_ICONERROR | MB_OK);
        return;
    }
    g_bind_hd = hd;
    {
        BOOL dark = TRUE;
        DwmSetWindowAttribute(hd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */,
                              &dark, sizeof dark);
    }
    bind_build(hd, base_name);
    if (g_o.shot == 3 && g_o.bind_sel >= 0) { /* W159: --bind-shot=vr */
        HWND lb = bctl(hd, IDB_LIST);
        int n = ListBox_GetCount(lb), at;
        for (at = 0; at < n; at++) {
            if ((int)ListBox_GetItemData(lb, at) == g_o.bind_sel) {
                ListBox_SetCurSel(lb, at);
                ListBox_SetTopIndex(lb, at > 1 ? at - 1 : 0);
                break;
            }
        }
    }
    bind_refresh_cur(hd);
    bind_refresh_conf(hd);
    if (g_o.shot == 3) {
        SetTimer(hd, 9, 400, NULL);
    }
    if (g_hwnd) {
        EnableWindow(g_hwnd, FALSE);
    }
    if (shot) {
        if (!is_offscreen(hd)) {
            printf("bind-shot: FEHLER Fenster nicht au" U8_SZ
                   "erhalb des Bildschirms - Abbruch\n");
            g_bind_done = 1;
        } else {
            ShowWindow(hd, SW_SHOWNOACTIVATE);
        }
    } else {
        ShowWindow(hd, SW_SHOW);
        SetFocus(bctl(hd, IDB_LIST));
    }
    UpdateWindow(hd);
    while (!g_bind_done && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (g_cap_mode &&
            (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN)) {
            bind_cap_key(hd, (int)msg.wParam, msg.lParam);
            continue;
        }
        if (g_cap_mode && (msg.message == WM_LBUTTONDOWN ||
                           msg.message == WM_RBUTTONDOWN ||
                           msg.message == WM_MBUTTONDOWN ||
                           msg.message == WM_XBUTTONDOWN)) {
            HWND tgt = WindowFromPoint(msg.pt);
            if (tgt == bctl(hd, IDB_CLOSE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            } else {
                bind_cap_mouse(hd, msg.message, msg.wParam);
            }
            continue;
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
            uictl_t *u = ui_by_hwnd(GetFocus());
            if (u && u->kind < K_EDIT && u->parent == hd &&
                IsWindowEnabled(u->h)) {
                SendMessageW(hd, WM_COMMAND, MAKEWPARAM(u->id, BN_CLICKED),
                             (LPARAM)u->h);
                continue;
            }
        }
        if (IsDialogMessageW(hd, &msg)) {
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_hwnd) {
        EnableWindow(g_hwnd, TRUE);
        if (!shot) {
            SetFocus(ctl(ID_CTRL_BIND));
        }
    }
    DestroyWindow(hd);
    g_bind_hd = NULL;
    g_ui_n = ui_mark; /* Dialog-Elemente austragen */
}

/* ------------------------------------------------------------------ */
/* Controller-Assistent (W143 CTRL-ASSIST)                              */
/* Live-Ansicht, gefuehrter Test, Kalibrierung, Lampen, Gamepad.         */
/* ------------------------------------------------------------------ */
static void pump(void);
static int shot_window(HWND w, const char *path, int png);
static void assist_setmemo(HWND h, const char *u8);
#define IDA_TITLE 2000
#define IDA_SRC 2001
#define IDA_M_LIVE 2002
#define IDA_M_TEST 2003
#define IDA_M_CAL 2004
#define IDA_M_LAMPS 2005
#define IDA_M_PAD 2006
#define IDA_DEVICE 2007
#define IDA_FIXTURE 2008
#define IDA_CLOSE 2009
#define IDA_LIVE_CV 2010
#define IDA_LIVE_ST 2011
#define IDA_NODEV 2012
#define IDA_T_PROMPT 2020
#define IDA_T_PROG 2021
#define IDA_T_START 2022
#define IDA_T_STOP 2023
#define IDA_T_SKIP 2024
#define IDA_T_BACK 2025
#define IDA_T_EXPORT 2026
#define IDA_T_YES 2027
#define IDA_T_NO 2028
#define IDA_T_LIST 2029
#define IDA_T_SUM 2030
#define IDA_T_LOG 2031
#define IDA_C_AXIS 2040
#define IDA_C_MEASURE 2041
#define IDA_C_VALS 2042
#define IDA_C_DEAD 2043
#define IDA_C_DEADV 2044
#define IDA_C_PREV 2045
#define IDA_C_HINT 2046
#define IDA_C_APPLY 2047
#define IDA_C_TUNER 2048
#define IDA_C_GEAR 2049
#define IDA_C_EXTRA 2050
#define IDA_C_LIST 2051
#define IDA_C_SAVE 2052
#define IDA_C_RESET 2053
#define IDA_C_STATUS 2054
#define IDA_C_LBL 2055
#define IDA_L_CV 2060
#define IDA_L_NAME 2061
#define IDA_L_SEQ 2062
#define IDA_L_ON 2063
#define IDA_L_OFF 2064
#define IDA_L_HINT 2065
#define IDA_L_MOCK 2066
#define IDA_P_CV 2070
#define IDA_P_NONE 2071
#define IDA_P_DZS 2072
#define IDA_P_DZSV 2073
#define IDA_P_DZT 2074
#define IDA_P_DZTV 2075
#define IDA_P_SAVE 2076
#define IDA_P_STATUS 2077

enum { AM_LIVE, AM_TEST, AM_CAL, AM_LAMPS, AM_PAD };

#define WM_APP_AE (WM_APP + 7) /* Engine-Ereignis (w=ev, l=assist_evt_t*) */

typedef struct {
    int a, b;
    char text[1024];
} assist_evt_t;

typedef struct {
    HWND hd;
    assist_t *as;
    int mode;
    int done;
    char fx_path[1024]; /* letzte Fixture (Anzeige) */
    /* Test */
    int t_running;
    char t_log[8][220];
    int t_logn;
    /* Kalibrierung */
    int c_axis;
    int c_dead[8];
    int c_has[8];
    int c_lo[8], c_mid[8], c_hi[8];
    char c_lines[16][96];
    int c_nlines;
    /* Lampen */
    int l_hover;
    int l_seq;
    /* Gamepad */
    lctrl_pad_t pad;
    int pad_present;
    int p_dzs, p_dzt;
    /* Shots */
    ULONGLONG shot_t0;
    ULONGLONG shot_led_t0;
    int shot_phase;
} assist_dlg_t;

static assist_dlg_t g_ad;
static HWND g_assist_hd;

static HWND actl(HWND hd, int id)
{
    /* Dialog-Kinder per GetDlgItem (ui_find verlangt g_hwnd als Parent). */
    return GetDlgItem(hd, id);
}

static void assist_evt_free(assist_evt_t *e)
{
    free(e);
}

/* Engine-Callback (Arbeiter-Thread!): als Fensternachricht marshallen.
 * EV_STATE kommt bis 250/s und setzt nur ein Flag (Timer zeichnet neu). */
static volatile LONG g_as_state_flag;

static void assist_cb(void *ctx, int ev, int a, int b, const char *text)
{
    assist_dlg_t *d = (assist_dlg_t *)ctx;
    assist_evt_t *e;
    if (!d || !d->hd) {
        return;
    }
    if (ev == ASSIST_EV_STATE) {
        InterlockedExchange(&g_as_state_flag, 1);
        return;
    }
    e = (assist_evt_t *)malloc(sizeof *e);
    if (!e) {
        return;
    }
    e->a = a;
    e->b = b;
    strncpy_s(e->text, sizeof e->text, text ? text : "", sizeof e->text - 1);
    PostMessageW(d->hd, WM_APP_AE, (WPARAM)ev, (LPARAM)e);
}

/* Tastenname (uebersetzt, bind.bi0..38) bzw. Fallback (Konsolen-Name). */
static const char *assist_btn_name(int bi)
{
    static char key[24];
    const char *n;
    if (bi < 0 || bi > 38) {
        return "?";
    }
    _snprintf_s(key, sizeof key, _TRUNCATE, "bind.bi%d", bi);
    n = LT(key);
    return (n && n[0]) ? n : sbc_test_btn_name[bi];
}

/* LED-Name (uebersetzt): 0..29 = Tastenname, 30..36 = Gang-Lampen. */
static const char *assist_led_name(int led)
{
    static const int map[30] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
        16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26,
        27, 28, 29, 30, 31, 32
    };
    static const char *const gk[7] = {
        "as.lg_r", "as.lg_n", "as.lg_1", "as.lg_2", "as.lg_3",
        "as.lg_4", "as.lg_5"
    };
    if (led < 0 || led >= SBC_LED_COUNT) {
        return "?";
    }
    if (led < 30) {
        return assist_btn_name(map[led]);
    }
    return LT(gk[led - 30]);
}

/* Achsennamen (Literale fuer lang_check). */
static const char *const kAxKey[8] = {
    "as.ax0", "as.ax1", "as.ax2", "as.ax3",
    "as.ax4", "as.ax5", "as.ax6", "as.ax7"
};

/* Statusname (uebersetzt). */
static const char *assist_st_name(int st)
{
    switch (st) {
    case SBC_TR_OK: return LT("as.st_ok");
    case SBC_TR_MISSING: return LT("as.st_missing");
    case SBC_TR_UNEXPECTED: return LT("as.st_unexp");
    case SBC_TR_PARTIAL: return LT("as.st_part");
    default: return LT("as.st_notrun");
    }
}

/* Schrittname (uebersetzt). Notizen bleiben Berichtssprache Deutsch
 * (wie die Exportdatei, W142: Berichte unuebersetzt). */
static void assist_step_name(const sbc_test_step_t *st, char *buf, size_t cap)
{
    int bi = st->idx >= 0 && st->idx < 39 ? st->idx : -1;
    int ax = st->idx >= 0 && st->idx < 8 ? st->idx : -1;
    switch (st->kind) {
    case SBC_TK_BTN:
        _snprintf_s(buf, cap, _TRUNCATE, "%s %s (bi%d)", LT("as.k_btn"),
                    bi >= 0 ? sbc_test_btn_name[bi] : "?", st->idx);
        break;
    case SBC_TK_TOG:
        _snprintf_s(buf, cap, _TRUNCATE, "%s %s (bi%d)", LT("as.k_tog"),
                    bi >= 0 ? sbc_test_btn_name[bi] : "?", st->idx);
        break;
    case SBC_TK_AXIS:
        _snprintf_s(buf, cap, _TRUNCATE, "%s %s", LT("as.k_axis"),
                    ax >= 0 ? LT(kAxKey[ax]) : "?");
        break;
    case SBC_TK_TUNER:
        strcpy_s(buf, cap, LT("as.k_tuner"));
        break;
    case SBC_TK_GEAR:
        strcpy_s(buf, cap, LT("as.k_gear"));
        break;
    default:
        strcpy_s(buf, cap, LT("as.k_led"));
        break;
    }
}

/* Schrittanzeige (uebersetzt): "Schritt i/50: Aufforderung". */
static void assist_step_prompt(int idx, char *out, size_t cap)
{
    const sbc_test_step_t *st = assist_steps(g_ad.as);
    char nm[96];
    if (idx < 0 || idx >= SBC_TEST_N_STEPS || !st) {
        _snprintf_s(out, cap, _TRUNCATE, LT("as.test_nores"));
        return;
    }
    switch (st[idx].kind) {
    case SBC_TK_BTN:
        _snprintf_s(out, cap, _TRUNCATE, LT("as.p_btn"),
                    assist_btn_name(st[idx].idx));
        break;
    case SBC_TK_TOG:
        _snprintf_s(out, cap, _TRUNCATE, LT("as.p_tog"),
                    assist_btn_name(st[idx].idx));
        break;
    case SBC_TK_AXIS:
        _snprintf_s(nm, sizeof nm, _TRUNCATE, "%s",
                    LT(kAxKey[st[idx].idx]));
        if (sbc_test_axes[st[idx].idx].kind == 2) {
            _snprintf_s(out, cap, _TRUNCATE, LT("as.p_pedal"), nm);
        } else {
            _snprintf_s(out, cap, _TRUNCATE, LT("as.p_axis"), nm);
        }
        break;
    case SBC_TK_TUNER:
        strcpy_s(out, cap, LT("as.p_tuner"));
        break;
    case SBC_TK_GEAR:
        strcpy_s(out, cap, LT("as.p_gear"));
        break;
    default:
        strcpy_s(out, cap, LT("as.p_led"));
        break;
    }
}

/* ---- Assistent: Zeichnung (GDI, W82-Palette) ---- */
#define APX(v) (MulDiv((v), g_dpi, 96))

static void a_text(HDC dc, int x, int y, int w, int h, const char *u8,
                   COLORREF col, HFONT f, UINT align)
{
    wchar_t b[256];
    RECT r;
    HGDIOBJ of;
    int n;
    r.left = x;
    r.top = y;
    r.right = x + w;
    r.bottom = y + h;
    SetTextColor(dc, col);
    SetBkMode(dc, TRANSPARENT);
    of = SelectObject(dc, f);
    n = u8_to_w(u8 ? u8 : "", b, 256);
    (void)n;
    DrawTextW(dc, b, -1, &r, align | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    SelectObject(dc, of);
}

static void a_fill(HDC dc, int x, int y, int w, int h, COLORREF col)
{
    HBRUSH br = CreateSolidBrush(col);
    RECT r;
    r.left = x;
    r.top = y;
    r.right = x + w;
    r.bottom = y + h;
    FillRect(dc, &r, br);
    DeleteObject(br);
}

static void a_frame(HDC dc, int x, int y, int w, int h, COLORREF col)
{
    HBRUSH br = CreateSolidBrush(col);
    RECT r;
    r.left = x;
    r.top = y;
    r.right = x + w;
    r.bottom = y + h;
    FrameRect(dc, &r, br);
    DeleteObject(br);
}

/* Kurznamen Lampenraster (Bezeichner wie Konsole; volle Namen uebersetzt). */
static const char *const kLedShort[SBC_LED_COUNT] = {
    "EJECT", "HATCH", "IGN", "START", "OPNCLS", "MAPZM", "MODE",
    "SUBMON", "ZMIN", "ZMOUT", "FSS", "MANIP", "LINE", "WASH",
    "EXTING", "CHAFF", "TANK", "OVR", "NSCOPE", "F1", "F2", "F3",
    "MAINWC", "SUBWC", "MAG", "COMM1", "COMM2", "COMM3", "COMM4",
    "COMM5", "GEAR R", "GEAR N", "G1", "G2", "G3", "G4", "G5"
};

/* W148: Kurznamen Tastenraster (volle Namen: Hover-Status, Testliste, Export). */
static const char *const kBtnShort[39] = {
    "TRIG", "FIRE", "LOCK", "EJECT", "HATCH", "IGN",
    "START", "OPEN", "MAPZM", "MODE", "SUBMON",
    "ZMIN", "ZMOUT", "FSS", "MANIP",
    "LINE", "WASH", "EXTING", "CHAFF", "TANK",
    "OVR", "NSCOPE", "F1", "F2", "F3", "MAINWC",
    "SUBWC", "MAG", "COMM1", "COMM2", "COMM3",
    "COMM4", "COMM5", "SIGHT", "FILTER",
    "O2", "FUEL", "BUFFER", "VTLOC"
};

/* Live-Geometrie (logisch, s. assist_build). */
#define ALV_BX0 8
#define ALV_BY0 24
#define ALV_BCW 76
#define ALV_BCH 32
#define ALV_BCG 3
#define ALV_BCOLS 7
#define ALV_TX0 8
#define ALV_TY0 220
#define ALV_TCW 106
#define ALV_TCH 30
#define ALV_TCG 3

/* Zelle (Taste/Schalter) aus Dialogpunkt (logisch). -1 = keine. */
static int assist_live_hit(int x, int y)
{
    int c, r;
    for (r = 0; r < 5; r++) {
        for (c = 0; c < ALV_BCOLS; c++) {
            int bi = r * ALV_BCOLS + c;
            int cx = ALV_BX0 + c * (ALV_BCW + ALV_BCG);
            int cy = ALV_BY0 + r * (ALV_BCH + ALV_BCG);
            if (bi > 33) {
                break;
            }
            if (x >= cx && x < cx + ALV_BCW && y >= cy && y < cy + ALV_BCH) {
                return bi;
            }
        }
    }
    for (c = 0; c < 5; c++) {
        int cx = ALV_TX0 + c * (ALV_TCW + ALV_TCG);
        if (x >= cx && x < cx + ALV_TCW && y >= ALV_TY0 &&
            y < ALV_TY0 + ALV_TCH) {
            return 34 + c;
        }
    }
    return -1;
}

static void assist_paint_live(HDC dc, int W, int H)
{
    uint8_t raw[SBC_USB_IN_LEN];
    uint64_t btns = 0;
    int have = 0, c, r;
    int aimx = 0x8000, aimy = 0x8000, rot = 0, sgx = 0, sgy = 0;
    int ped[3] = { 0, 0, 0 };
    int tuner = 0, gear = 0;
    (void)W;
    (void)H;
    if (g_ad.as && assist_has_report(g_ad.as)) {
        assist_raw(g_ad.as, raw);
        have = 1;
        btns = sbc_test_raw_btns(raw);
        aimx = sbc_test_raw_axis(raw, 0);
        aimy = sbc_test_raw_axis(raw, 1);
        rot = sbc_test_raw_axis(raw, 2);
        sgx = sbc_test_raw_axis(raw, 3);
        sgy = sbc_test_raw_axis(raw, 4);
        ped[0] = sbc_test_raw_axis(raw, 5);
        ped[1] = sbc_test_raw_axis(raw, 6);
        ped[2] = sbc_test_raw_axis(raw, 7);
        tuner = raw[24] & 0x0F;
        gear = raw[25];
    }
    /* Tasten 0..33 */
    a_text(dc, APX(8), APX(4), APX(300), APX(16), LT("as.g_btns"), C_DIM,
           g_f[F_SMALL], DT_LEFT);
    for (r = 0; r < 5; r++) {
        for (c = 0; c < ALV_BCOLS; c++) {
            int bi = r * ALV_BCOLS + c;
            int x = APX(ALV_BX0 + c * (ALV_BCW + ALV_BCG));
            int y = APX(ALV_BY0 + r * (ALV_BCH + ALV_BCG));
            int w = APX(ALV_BCW), h = APX(ALV_BCH);
            if (bi > 33) {
                break;
            }
            if (have && (btns & (1ULL << bi))) {
                a_fill(dc, x, y, w, h, C_ACCENT);
                a_text(dc, x, y, w, h, kBtnShort[bi], C_ON_ACCENT, /* W148: Kurzname */
                       g_f[F_SMALL], DT_CENTER | DT_END_ELLIPSIS);
            } else {
                a_fill(dc, x, y, w, h, C_INPUT);
                a_frame(dc, x, y, w, h, C_INPUT_BRD);
                a_text(dc, x, y, w, h, kBtnShort[bi], C_TEXT, /* W148: Kurzname */
                       g_f[F_SMALL], DT_CENTER | DT_END_ELLIPSIS);
            }
        }
    }
    /* Schalter 34..38 */
    a_text(dc, APX(8), APX(202), APX(300), APX(16), LT("as.g_tog"), C_DIM,
           g_f[F_SMALL], DT_LEFT);
    for (c = 0; c < 5; c++) {
        int x = APX(ALV_TX0 + c * (ALV_TCW + ALV_TCG));
        int y = APX(ALV_TY0), w = APX(ALV_TCW), h = APX(ALV_TCH);
        int on = have && (btns & (1ULL << (34 + c)));
        a_fill(dc, x, y, w, h, on ? C_ACCENT : C_INPUT);
        if (!on) {
            a_frame(dc, x, y, w, h, C_INPUT_BRD);
        }
        a_text(dc, x, y, w, h, kBtnShort[34 + c], /* W148: Kurzname */
               on ? C_ON_ACCENT : C_TEXT, g_f[F_SMALL],
               DT_CENTER | DT_END_ELLIPSIS);
    }
    /* Tuner (13 Rasten) */
    {
        char t[64];
        _snprintf_s(t, sizeof t, _TRUNCATE, "%s: %d", LT("as.tuner"), tuner);
        a_text(dc, APX(8), APX(258), APX(300), APX(16), t, C_DIM,
               g_f[F_SMALL], DT_LEFT);
        for (c = 0; c < 13; c++) {
            int x = APX(8 + c * 33), y = APX(276), w = APX(30), h = APX(24);
            int on = have && tuner == c;
            a_fill(dc, x, y, w, h, on ? C_ACCENT : C_INPUT);
            if (!on) {
                a_frame(dc, x, y, w, h, C_INPUT_BRD);
            }
            {
                char n[8];
                _snprintf_s(n, sizeof n, _TRUNCATE, "%d", c);
                a_text(dc, x, y, w, h, n, on ? C_ON_ACCENT : C_TEXT,
                       g_f[F_SMALL], DT_CENTER);
            }
        }
    }
    /* Gang (R N 1..5) */
    {
        static const char *const gl[7] = { "R", "N", "1", "2", "3", "4", "5" };
        static const int gv[7] = { 7, 8, 9, 10, 11, 12, 13 };
        char t[80];
        int e = have ? sbc_usb_gear_effective(gear) : -3;
        _snprintf_s(t, sizeof t, _TRUNCATE, "%s: %s (%d)", LT("as.gear"),
                    have ? sbc_test_gear_name(gear) : "-", gear);
        a_text(dc, APX(8), APX(310), APX(300), APX(16), t, C_DIM,
               g_f[F_SMALL], DT_LEFT);
        for (c = 0; c < 7; c++) {
            int x = APX(8 + c * 56), y = APX(328), w = APX(52), h = APX(30);
            int on = have && e == c;
            (void)gv;
            a_fill(dc, x, y, w, h, on ? C_ACCENT : C_INPUT);
            if (!on) {
                a_frame(dc, x, y, w, h, C_INPUT_BRD);
            }
            a_text(dc, x, y, w, h, gl[c], on ? C_ON_ACCENT : C_TEXT,
                   g_f[F_SEMI], DT_CENTER);
        }
        if (have && gear == 0) {
            a_text(dc, APX(408), APX(328), APX(140), APX(30), "0", C_WARN,
                   g_f[F_SEMI], DT_LEFT);
        }
    }
    /* Zielhebel-Kreuz */
    {
        int x0 = APX(576), y0 = APX(24), s = APX(150);
        int dx = have ? (int)((aimx * (s - APX(12))) / 65535) : (s - APX(12)) / 2;
        int dy = have ? (int)((aimy * (s - APX(12))) / 65535) : (s - APX(12)) / 2;
        char t[96];
        _snprintf_s(t, sizeof t, _TRUNCATE, "%s: %d/%d", LT("as.ax_aim"),
                    aimx / 256, aimy / 256);
        a_text(dc, APX(576), APX(4), APX(160), APX(16), t, C_DIM,
               g_f[F_SMALL], DT_LEFT);
        a_fill(dc, x0, y0, s, s, C_INPUT);
        a_frame(dc, x0, y0, s, s, C_INPUT_BRD);
        {
            HPEN pn = CreatePen(PS_SOLID, 1, C_INPUT_BRD);
            HGDIOBJ op = SelectObject(dc, pn);
            MoveToEx(dc, x0 + s / 2, y0, NULL);
            LineTo(dc, x0 + s / 2, y0 + s);
            MoveToEx(dc, x0, y0 + s / 2, NULL);
            LineTo(dc, x0 + s, y0 + s / 2);
            SelectObject(dc, op);
            DeleteObject(pn);
        }
        if (have) {
            HBRUSH br = CreateSolidBrush(C_ACCENT);
            HGDIOBJ ob = SelectObject(dc, br);
            Ellipse(dc, x0 + APX(6) + dx - APX(5), y0 + APX(6) + dy - APX(5),
                    x0 + APX(6) + dx + APX(5), y0 + APX(6) + dy + APX(5));
            SelectObject(dc, ob);
            DeleteObject(br);
        }
    }
    /* Drehhebel-Balken */
    {
        int x0 = APX(576), y0 = APX(208), w = APX(150), h = APX(22);
        int fill = have ? (int)(((rot + 32768) * w) / 65535) : w / 2;
        char t[96];
        _snprintf_s(t, sizeof t, _TRUNCATE, "%s: %+d", LT("as.ax_rot"),
                    rot / 256);
        a_text(dc, APX(576), APX(188), APX(200), APX(16), t, C_DIM,
               g_f[F_SMALL], DT_LEFT);
        a_fill(dc, x0, y0, w, h, C_INPUT);
        a_frame(dc, x0, y0, w, h, C_INPUT_BRD);
        if (have) {
            int cx = x0 + w / 2;
            if (fill < w / 2) {
                a_fill(dc, x0 + fill, y0 + 1, w / 2 - fill, h - 2, C_ACCENT);
            } else if (fill > w / 2) {
                a_fill(dc, cx, y0 + 1, fill - w / 2, h - 2, C_ACCENT);
            }
        }
    }
    /* Sight-Kreuz */
    {
        int x0 = APX(738), y0 = APX(24), s = APX(150);
        int dx = have ? (int)(((sgx + 32768) * (s - APX(12))) / 65535)
                      : (s - APX(12)) / 2;
        int dy = have ? (int)(((sgy + 32768) * (s - APX(12))) / 65535)
                      : (s - APX(12)) / 2;
        char t[96];
        _snprintf_s(t, sizeof t, _TRUNCATE, "%s: %+d/%+d", LT("as.ax_sight"),
                    sgx / 256, sgy / 256);
        a_text(dc, APX(738), APX(4), APX(158), APX(16), t,
               C_DIM, g_f[F_SMALL], DT_LEFT);
        a_fill(dc, x0, y0, s, s, C_INPUT);
        a_frame(dc, x0, y0, s, s, C_INPUT_BRD);
        {
            HPEN pn = CreatePen(PS_SOLID, 1, C_INPUT_BRD);
            HGDIOBJ op = SelectObject(dc, pn);
            MoveToEx(dc, x0 + s / 2, y0, NULL);
            LineTo(dc, x0 + s / 2, y0 + s);
            MoveToEx(dc, x0, y0 + s / 2, NULL);
            LineTo(dc, x0 + s, y0 + s / 2);
            SelectObject(dc, op);
            DeleteObject(pn);
        }
        if (have) {
            HBRUSH br = CreateSolidBrush(C_ACCENT);
            HGDIOBJ ob = SelectObject(dc, br);
            Ellipse(dc, x0 + APX(6) + dx - APX(5), y0 + APX(6) + dy - APX(5),
                    x0 + APX(6) + dx + APX(5), y0 + APX(6) + dy + APX(5));
            SelectObject(dc, ob);
            DeleteObject(br);
        }
        (void)t;
    }
    /* Pedale */
    {
        static const char *const pl[3] = { "L", "M", "R" };
        a_text(dc, APX(576), APX(258), APX(300), APX(16), LT("as.ax_ped"),
               C_DIM, g_f[F_SMALL], DT_LEFT);
        for (c = 0; c < 3; c++) {
            int x0 = APX(600 + c * 100), y0 = APX(278);
            int w = APX(44), h = APX(140);
            int fill = have ? (int)((ped[c] * h) / 65535) : 0;
            char t[32];
            a_fill(dc, x0, y0, w, h, C_INPUT);
            a_frame(dc, x0, y0, w, h, C_INPUT_BRD);
            if (have && fill > 0) {
                a_fill(dc, x0 + 1, y0 + h - fill, w - 2, fill, C_ACCENT);
            }
            _snprintf_s(t, sizeof t, _TRUNCATE, "%s %d", pl[c],
                        have ? ped[c] / 256 : 0);
            a_text(dc, x0 - APX(28), y0 + h + APX(2), w + APX(56), APX(16), t,
                   C_DIM, g_f[F_SMALL], DT_CENTER);
        }
    }
    if (!have) {
        a_text(dc, 0, APX(420), W, APX(20), LT("as.src_none"), C_FAINT,
               g_f[F_SMALL], DT_CENTER);
    }
}

/* Lampen-Geometrie */
#define ALM_X0 8
#define ALM_Y0 4
#define ALM_CW 98
#define ALM_CH 60
#define ALM_CG 6
#define ALM_COLS 6

static int assist_lamp_hit(int x, int y)
{
    int c, r;
    for (r = 0; r < 7; r++) {
        for (c = 0; c < ALM_COLS; c++) {
            int led = r * ALM_COLS + c;
            int cx = ALM_X0 + c * (ALM_CW + ALM_CG);
            int cy = ALM_Y0 + r * (ALM_CH + ALM_CG);
            if (led >= SBC_LED_COUNT) {
                return -1;
            }
            if (x >= cx && x < cx + ALM_CW && y >= cy && y < cy + ALM_CH) {
                return led;
            }
        }
    }
    return -1;
}

static void assist_paint_lamps(HDC dc, int W, int H)
{
    int led;
    (void)W;
    (void)H;
    for (led = 0; led < SBC_LED_COUNT; led++) {
        int c = led % ALM_COLS, r = led / ALM_COLS;
        int x = APX(ALM_X0 + c * (ALM_CW + ALM_CG));
        int y = APX(ALM_Y0 + r * (ALM_CH + ALM_CG));
        int w = APX(ALM_CW), h = APX(ALM_CH);
        int lvl = g_ad.as ? assist_led_level(g_ad.as, led) : 0;
        if (lvl > 0) {
            a_fill(dc, x, y, w, h, C_ACCENT);
            a_text(dc, x, y, w, h, kLedShort[led], C_ON_ACCENT,
                   g_f[F_SMALL], DT_CENTER);
        } else {
            a_fill(dc, x, y, w, h, C_INPUT);
            a_frame(dc, x, y, w, h, C_INPUT_BRD);
            a_text(dc, x, y, w, h, kLedShort[led], C_DIM, g_f[F_SMALL],
                   DT_CENTER);
        }
        if (led == g_ad.l_hover) {
            a_frame(dc, x, y, w, h, C_TEXT);
        }
    }
}

/* Gamepad-Geometrie */
static void assist_paint_pad(HDC dc, int W, int H)
{
    int i;
    static const char *const bn[14] = {
        "A", "B", "X", "Y", "LB", "RB", "BK", "ST",
        "L3", "R3", "DU", "DD", "DL", "DR"
    };
    static const unsigned bb[14] = {
        0x1000, 0x2000, 0x4000, 0x8000, 0x0100, 0x0200, 0x0020, 0x0010,
        0x0040, 0x0080, 0x0001, 0x0002, 0x0004, 0x0008
    };
    (void)W;
    (void)H;
    /* Sticks */
    {
        struct { int x, y; int16_t vx, vy; const char *t; } st[2] = {
            { 20, 40, 0, 0, NULL }, { 200, 40, 0, 0, NULL }
        };
        st[0].t = LT("as.stick_l");
        st[1].t = LT("as.stick_r");
        if (g_ad.pad_present) {
            st[0].vx = g_ad.pad.lx;
            st[0].vy = g_ad.pad.ly;
            st[1].vx = g_ad.pad.rx;
            st[1].vy = g_ad.pad.ry;
        }
        for (i = 0; i < 2; i++) {
            int x0 = APX(st[i].x), y0 = APX(st[i].y), s = APX(150);
            int dx = (int)(((st[i].vx + 32768) * (s - APX(12))) / 65535);
            int dy = (int)(((32767 - st[i].vy) * (s - APX(12))) / 65535);
            char t[96];
            _snprintf_s(t, sizeof t, _TRUNCATE, "%s: %+0.2f/%+0.2f", st[i].t,
                        st[i].vx / 32768.0, st[i].vy / 32768.0);
            a_text(dc, x0, y0 - APX(20), s + APX(40), APX(16), t, C_DIM,
                   g_f[F_SMALL], DT_LEFT);
            a_fill(dc, x0, y0, s, s, C_INPUT);
            a_frame(dc, x0, y0, s, s, C_INPUT_BRD);
            {
                HPEN pn = CreatePen(PS_SOLID, 1, C_INPUT_BRD);
                HGDIOBJ op = SelectObject(dc, pn);
                MoveToEx(dc, x0 + s / 2, y0, NULL);
                LineTo(dc, x0 + s / 2, y0 + s);
                MoveToEx(dc, x0, y0 + s / 2, NULL);
                LineTo(dc, x0 + s, y0 + s / 2);
                SelectObject(dc, op);
                DeleteObject(pn);
            }
            if (g_ad.pad_present) {
                HBRUSH br = CreateSolidBrush(C_ACCENT);
                HGDIOBJ ob = SelectObject(dc, br);
                Ellipse(dc, x0 + APX(6) + dx - APX(5),
                        y0 + APX(6) + dy - APX(5), x0 + APX(6) + dx + APX(5),
                        y0 + APX(6) + dy + APX(5));
                SelectObject(dc, ob);
                DeleteObject(br);
            }
        }
    }
    /* Trigger */
    {
        int lt = g_ad.pad_present ? g_ad.pad.lt : 0;
        int rt = g_ad.pad_present ? g_ad.pad.rt : 0;
        a_text(dc, APX(390), APX(20), APX(150), APX(16), LT("as.trig"), C_DIM,
               g_f[F_SMALL], DT_LEFT);
        for (i = 0; i < 2; i++) {
            int x0 = APX(390 + i * 60), y0 = APX(40);
            int w = APX(44), h = APX(150);
            int v = i ? rt : lt;
            int fill = (v * h) / 255;
            a_fill(dc, x0, y0, w, h, C_INPUT);
            a_frame(dc, x0, y0, w, h, C_INPUT_BRD);
            if (g_ad.pad_present && fill > 0) {
                a_fill(dc, x0 + 1, y0 + h - fill, w - 2, fill, C_ACCENT);
            }
            {
                char t[32];
                _snprintf_s(t, sizeof t, _TRUNCATE, "%s %.2f",
                            i ? "RT" : "LT", v / 255.0);
                a_text(dc, x0 - APX(8), y0 + h + APX(2), w + APX(16),
                       APX(16), t, C_DIM, g_f[F_SMALL], DT_CENTER);
            }
        }
    }
    /* Tasten */
    for (i = 0; i < 14; i++) {
        int x = APX(20 + i * 38), y = APX(230), w = APX(34), h = APX(28);
        int on = g_ad.pad_present && (g_ad.pad.buttons & bb[i]);
        a_fill(dc, x, y, w, h, on ? C_ACCENT : C_INPUT);
        if (!on) {
            a_frame(dc, x, y, w, h, C_INPUT_BRD);
        }
        a_text(dc, x, y, w, h, bn[i], on ? C_ON_ACCENT : C_TEXT,
               g_f[F_SMALL], DT_CENTER);
    }
}

/* ---- Assistent: Aufbau ---- */
#define AD_W 920
#define AD_H 656

static void assist_show_mode(HWND hd, int mode);

static void assist_build(HWND hd)
{
    HWND cb;
    int i;
    mk(hd, K_STATIC, IDA_TITLE, PG_DIALOG, 12, 10, 700, 20, LT("as.title"),
       WS_VISIBLE);
    mk(hd, K_STATIC, IDA_SRC, PG_DIALOG, 12, 32, 560, 18, "", WS_VISIBLE);
    mk(hd, K_BTN, IDA_M_LIVE, PG_DIALOG, 12, 56, 104, 30, LT("as.live"),
       WS_VISIBLE);
    mk(hd, K_BTN, IDA_M_TEST, PG_DIALOG, 122, 56, 130, 30, LT("as.test"),
       WS_VISIBLE);
    mk(hd, K_BTN, IDA_M_CAL, PG_DIALOG, 258, 56, 120, 30, LT("as.cal"),
       WS_VISIBLE);
    mk(hd, K_BTN, IDA_M_LAMPS, PG_DIALOG, 384, 56, 110, 30, LT("as.lamps"),
       WS_VISIBLE);
    mk(hd, K_BTN, IDA_M_PAD, PG_DIALOG, 500, 56, 110, 30, LT("as.pad"),
       WS_VISIBLE);
    mk(hd, K_BTN, IDA_DEVICE, PG_DIALOG, 616, 56, 96, 30,
       LT("as.device_btn"), WS_VISIBLE);
    mk(hd, K_BTN, IDA_FIXTURE, PG_DIALOG, 718, 56, 96, 30,
       LT("as.fixture_btn"), WS_VISIBLE);
    mk(hd, K_BTN, IDA_CLOSE, PG_DIALOG, 820, 56, 88, 30, LT("as.close"),
       WS_VISIBLE);
    /* Live */
    mk(hd, K_STATIC, IDA_LIVE_ST, PG_DIALOG, 12, 570, 896, 36, "",
       WS_VISIBLE);
    mk(hd, K_MEMO, IDA_NODEV, PG_DIALOG, 12, 94, 896, 470, "", 0);
    /* Test */
    mk(hd, K_STATIC, IDA_T_PROMPT, PG_DIALOG, 12, 94, 640, 50, "", 0);
    mk(hd, K_STATIC, IDA_T_PROG, PG_DIALOG, 660, 94, 248, 50, "", 0);
    mk(hd, K_ACCENT, IDA_T_START, PG_DIALOG, 12, 150, 100, 30,
       LT("as.test_start"), 0);
    mk(hd, K_BTN, IDA_T_STOP, PG_DIALOG, 118, 150, 100, 30,
       LT("as.test_stop"), 0);
    mk(hd, K_BTN, IDA_T_SKIP, PG_DIALOG, 224, 150, 130, 30,
       LT("as.test_skip"), 0);
    mk(hd, K_BTN, IDA_T_BACK, PG_DIALOG, 360, 150, 100, 30,
       LT("as.test_back"), 0);
    mk(hd, K_BTN, IDA_T_EXPORT, PG_DIALOG, 466, 150, 130, 30,
       LT("as.test_export"), 0);
    mk(hd, K_BTN, IDA_T_YES, PG_DIALOG, 660, 150, 110, 30, LT("as.yes"), 0);
    mk(hd, K_BTN, IDA_T_NO, PG_DIALOG, 776, 150, 110, 30, LT("as.no"), 0);
    {
        HWND lb = mk(hd, K_LIST, IDA_T_LIST, PG_DIALOG, 12, 190, 640, 374,
                     "", 0);
        const sbc_test_step_t *st = assist_steps(g_ad.as);
        for (i = 0; i < SBC_TEST_N_STEPS; i++) {
            char e[256], nm[96];
            _snprintf_s(e, sizeof e, _TRUNCATE, "%2d %-30s -",
                        i + 1, (assist_step_name(&st[i], nm, sizeof nm), nm));
            list_add(lb, e, (LPARAM)i, 1);
        }
    }
    mk(hd, K_STATIC, IDA_T_SUM, PG_DIALOG, 660, 190, 248, 24, "", 0);
    mk(hd, K_MEMO, IDA_T_LOG, PG_DIALOG, 660, 220, 248, 344, "", 0);
    /* Kalibrierung */
    cb = mk(hd, K_COMBO, IDA_C_AXIS, PG_DIALOG, 12, 94, 220, 200, "", 0);
    for (i = 0; i < 8; i++) {
        combo_add(cb, LT(kAxKey[i]));
    }
    SendMessageW(cb, CB_SETCURSEL, (WPARAM)2, 0);
    mk(hd, K_ACCENT, IDA_C_MEASURE, PG_DIALOG, 240, 94, 110, 30,
       LT("as.cal_measure"), 0);
    mk(hd, K_STATIC, IDA_C_VALS, PG_DIALOG, 12, 130, 340, 20, "", 0);
    {
        HWND tr = mk(hd, K_TRACK, IDA_C_DEAD, PG_DIALOG, 12, 156, 220, 30,
                     "", 0);
        SendMessageW(tr, TBM_SETRANGE, (WPARAM)TRUE, MAKELPARAM(0, 100));
        SendMessageW(tr, TBM_SETPOS, (WPARAM)TRUE, (LPARAM)6);
        SendMessageW(tr, TBM_SETTICFREQ, (WPARAM)10, 0);
    }
    mk(hd, K_STATIC, IDA_C_DEADV, PG_DIALOG, 240, 160, 110, 22, "", 0);
    mk(hd, K_STATIC, IDA_C_PREV, PG_DIALOG, 12, 192, 340, 22, "", 0);
    mk(hd, K_STATIC, IDA_C_HINT, PG_DIALOG, 12, 220, 340, 40, "", 0);
    mk(hd, K_BTN, IDA_C_APPLY, PG_DIALOG, 12, 266, 140, 30,
       LT("as.cal_apply"), 0);
    mk(hd, K_BTN, IDA_C_TUNER, PG_DIALOG, 12, 306, 164, 30,
       LT("as.cal_tuner"), 0);
    mk(hd, K_BTN, IDA_C_GEAR, PG_DIALOG, 182, 306, 164, 30,
       LT("as.cal_gear"), 0);
    mk(hd, K_STATIC, IDA_C_EXTRA, PG_DIALOG, 12, 342, 340, 48, "", 0);
    mk(hd, K_STATIC, IDA_C_LBL, PG_DIALOG, 372, 94, 536, 20,
       LT("as.cal_lines"), 0);
    mk(hd, K_LIST, IDA_C_LIST, PG_DIALOG, 372, 118, 536, 350, "", 0);
    mk(hd, K_ACCENT, IDA_C_SAVE, PG_DIALOG, 372, 474, 150, 30,
       LT("as.cal_save"), 0);
    mk(hd, K_BTN, IDA_C_RESET, PG_DIALOG, 528, 474, 220, 30,
       LT("as.cal_reset"), 0);
    mk(hd, K_STATIC, IDA_C_STATUS, PG_DIALOG, 372, 510, 536, 54, "", 0);
    /* Lampen */
    mk(hd, K_STATIC, IDA_L_NAME, PG_DIALOG, 660, 94, 248, 40, "", 0);
    mk(hd, K_ACCENT, IDA_L_SEQ, PG_DIALOG, 660, 140, 248, 30,
       LT("as.led_seq"), 0);
    mk(hd, K_BTN, IDA_L_ON, PG_DIALOG, 660, 176, 248, 30,
       LT("as.led_all_on"), 0);
    mk(hd, K_BTN, IDA_L_OFF, PG_DIALOG, 660, 212, 248, 30,
       LT("as.led_all_off"), 0);
    mk(hd, K_STATIC, IDA_L_HINT, PG_DIALOG, 660, 248, 248, 60,
       LT("as.led_hint"), 0);
    mk(hd, K_STATIC, IDA_L_MOCK, PG_DIALOG, 660, 312, 248, 40,
       LT("as.led_mock"), 0);
    /* Gamepad */
    mk(hd, K_STATIC, IDA_P_NONE, PG_DIALOG, 12, 500, 560, 24,
       LT("as.pad_none"), 0);
    mk(hd, K_STATIC, IDA_P_DZSV, PG_DIALOG, 580, 94, 328, 20, "", 0);
    {
        HWND tr = mk(hd, K_TRACK, IDA_P_DZS, PG_DIALOG, 580, 118, 328, 30,
                     "", 0);
        SendMessageW(tr, TBM_SETRANGE, (WPARAM)TRUE, MAKELPARAM(0, 90));
        SendMessageW(tr, TBM_SETPOS, (WPARAM)TRUE, (LPARAM)0);
        SendMessageW(tr, TBM_SETTICFREQ, (WPARAM)10, 0);
    }
    mk(hd, K_STATIC, IDA_P_DZTV, PG_DIALOG, 580, 154, 328, 20, "", 0);
    {
        HWND tr = mk(hd, K_TRACK, IDA_P_DZT, PG_DIALOG, 580, 178, 328, 30,
                     "", 0);
        SendMessageW(tr, TBM_SETRANGE, (WPARAM)TRUE, MAKELPARAM(0, 90));
        SendMessageW(tr, TBM_SETPOS, (WPARAM)TRUE, (LPARAM)0);
        SendMessageW(tr, TBM_SETTICFREQ, (WPARAM)10, 0);
    }
    mk(hd, K_ACCENT, IDA_P_SAVE, PG_DIALOG, 580, 214, 150, 30,
       LT("as.pad_save"), 0);
    mk(hd, K_STATIC, IDA_P_STATUS, PG_DIALOG, 580, 250, 328, 100, "", 0);
}

static const int kModeIds[][17] = {
    { IDA_LIVE_ST, IDA_NODEV, 0 }, /* AM_LIVE */
    { IDA_T_PROMPT, IDA_T_PROG, IDA_T_START, IDA_T_STOP, IDA_T_SKIP,
      IDA_T_BACK, IDA_T_EXPORT, IDA_T_LIST, IDA_T_SUM, IDA_T_LOG, 0 }, /* TEST */
    { IDA_C_AXIS, IDA_C_MEASURE, IDA_C_VALS, IDA_C_DEAD, IDA_C_DEADV,
      IDA_C_PREV, IDA_C_HINT, IDA_C_APPLY, IDA_C_TUNER, IDA_C_GEAR,
      IDA_C_EXTRA, IDA_C_LBL, IDA_C_LIST, IDA_C_SAVE, IDA_C_RESET,
      IDA_C_STATUS }, /* CAL (16, voll) */
    { IDA_L_NAME, IDA_L_SEQ, IDA_L_ON, IDA_L_OFF, IDA_L_HINT, IDA_L_MOCK,
      0 }, /* LAMPS */
    { IDA_P_NONE, IDA_P_DZSV, IDA_P_DZS, IDA_P_DZTV, IDA_P_DZT, IDA_P_SAVE,
      IDA_P_STATUS, 0 }, /* PAD */
};

static void assist_show_mode(HWND hd, int mode)
{
    int m, i;
    /* Arbeiter anhalten (Messung laeuft nicht in den naechsten Modus). */
    if (g_ad.as && assist_test_busy(g_ad.as)) {
        assist_test_stop(g_ad.as);
    }
    /* Lampen aus beim Verlassen der Lampen-Seite (auch Fixture-Mock). */
    if (g_ad.mode == AM_LAMPS && mode != AM_LAMPS && g_ad.as) {
        assist_led_all(g_ad.as, 0);
        g_ad.l_seq = 0;
    }
    g_ad.mode = mode;
    for (m = 0; m < 5; m++) {
        for (i = 0; kModeIds[m][i]; i++) {
            HWND h = actl(hd, kModeIds[m][i]);
            if (h) {
                ShowWindow(h, m == mode ? SW_SHOW : SW_HIDE);
            }
        }
    }
    /* Ja/Nein nur waehrend der Lampenbestaetigung. */
    ShowWindow(actl(hd, IDA_T_YES), SW_HIDE);
    ShowWindow(actl(hd, IDA_T_NO), SW_HIDE);
    /* Nodev-Hinweis nur ohne Quelle. */
    if (mode == AM_LIVE && g_ad.as) {
        int src = assist_source(g_ad.as);
        ShowWindow(actl(hd, IDA_NODEV), src == ASSIST_SRC_NONE ? SW_SHOW : SW_HIDE);
        if (src == ASSIST_SRC_NONE) {
            assist_setmemo(actl(hd, IDA_NODEV), LT("as.nodev_hint"));
        }
    }
    /* Lampen-Mock-Hinweis nur ohne Geraet. */
    if (mode == AM_LAMPS && g_ad.as) {
        ShowWindow(actl(hd, IDA_L_MOCK),
                   assist_source(g_ad.as) == ASSIST_SRC_DEVICE ? SW_HIDE : SW_SHOW);
    }
    InvalidateRect(hd, NULL, FALSE);
}

/* ---- Assistent: Aktionen ---- */

/* Memo-Text setzen (EDIT braucht \r\n; \n expandieren). */
static void assist_setmemo(HWND h, const char *u8)
{
    char b[4096];
    size_t i, o = 0;
    if (!h) {
        return;
    }
    for (i = 0; u8[i] && o + 2 < sizeof b; i++) {
        if (u8[i] == '\n' && (i == 0 || u8[i - 1] != '\r')) {
            b[o++] = '\r';
        }
        b[o++] = u8[i];
    }
    b[o] = '\0';
    ui_settext(h, b);
}

static void assist_src_text(HWND hd)
{
    char t[320];
    int src = g_ad.as ? assist_source(g_ad.as) : ASSIST_SRC_NONE;
    if (src == ASSIST_SRC_DEVICE) {
        strcpy_s(t, sizeof t, LT("as.src_device"));
    } else if (src == ASSIST_SRC_FIXTURE) {
        const char *b = strrchr(g_ad.fx_path, '\\');
        const char *b2 = strrchr(g_ad.fx_path, '/');
        if (b2 && (!b || b2 > b)) {
            b = b2;
        }
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("as.src_fixture"),
                    b ? b + 1 : g_ad.fx_path);
    } else {
        strcpy_s(t, sizeof t, LT("as.src_none"));
    }
    ui_settext(actl(hd, IDA_SRC), t);
    if (g_ad.mode == AM_LIVE) {
        ShowWindow(actl(hd, IDA_NODEV),
                   src == ASSIST_SRC_NONE ? SW_SHOW : SW_HIDE);
        if (src == ASSIST_SRC_NONE) {
            assist_setmemo(actl(hd, IDA_NODEV), LT("as.nodev_hint"));
        }
    }
    if (g_ad.mode == AM_LAMPS) {
        ShowWindow(actl(hd, IDA_L_MOCK),
                   src == ASSIST_SRC_DEVICE ? SW_HIDE : SW_SHOW);
    }
}

/* Testliste + Zusammenfassung neu. */
static void assist_test_refresh(HWND hd)
{
    const sbc_test_res_t *res = assist_results(g_ad.as);
    const sbc_test_step_t *st = assist_steps(g_ad.as);
    HWND lb = actl(hd, IDA_T_LIST);
    int cnt[5] = { 0, 0, 0, 0, 0 };
    int i;
    for (i = 0; i < SBC_TEST_N_STEPS; i++) {
        char e[256], nm[96];
        cnt[res[i].status]++;
        _snprintf_s(e, sizeof e, _TRUNCATE, "%2d %-30s %-10s %s", i + 1,
                    (assist_step_name(&st[i], nm, sizeof nm), nm),
                    assist_st_name(res[i].status), res[i].note);
        SendMessageW(lb, LB_DELETESTRING, (WPARAM)i, 0);
        {
            wchar_t *w = u8_wdup(e);
            SendMessageW(lb, LB_INSERTSTRING, (WPARAM)i, (LPARAM)w);
            free(w);
        }
    }
    {
        char s[160];
        _snprintf_s(s, sizeof s, _TRUNCATE, LT("as.test_sum"), cnt[0],
                    SBC_TEST_N_STEPS, cnt[1], cnt[2], cnt[3]);
        ui_settext(actl(hd, IDA_T_SUM), s);
    }
}

static void assist_test_line(HWND hd, const char *text)
{
    int i;
    char all[8 * 222];
    size_t o = 0;
    if (g_ad.t_logn >= 8) {
        for (i = 1; i < 8; i++) {
            strcpy_s(g_ad.t_log[i - 1], sizeof g_ad.t_log[0], g_ad.t_log[i]);
        }
        g_ad.t_logn = 7;
    }
    {
        const char *nl = strchr(text, '\n');
        size_t n = nl ? (size_t)(nl - text) : strlen(text);
        if (n > sizeof g_ad.t_log[0] - 1) {
            n = sizeof g_ad.t_log[0] - 1;
        }
        memcpy(g_ad.t_log[g_ad.t_logn], text, n);
        g_ad.t_log[g_ad.t_logn][n] = '\0';
        g_ad.t_logn++;
    }
    all[0] = '\0';
    for (i = 0; i < g_ad.t_logn; i++) {
        o += _snprintf_s(all + o, sizeof all - o, _TRUNCATE, "%s\r\n",
                         g_ad.t_log[i]);
    }
    ui_settext(actl(hd, IDA_T_LOG), all);
}

static void assist_test_start(HWND hd, int idx)
{
    char p[220], pr[64];
    assist_step_prompt(idx, p, sizeof p);
    _snprintf_s(pr, sizeof pr, _TRUNCATE, LT("as.test_step"), idx + 1,
                SBC_TEST_N_STEPS);
    ui_settext(actl(hd, IDA_T_PROMPT), p);
    ui_settext(actl(hd, IDA_T_PROG), pr);
    SendMessageW(actl(hd, IDA_T_LIST), LB_SETCURSEL, (WPARAM)idx, 0);
    {
        const sbc_test_step_t *st = assist_steps(g_ad.as);
        int is_led = st && st[idx].kind == SBC_TK_LED;
        ShowWindow(actl(hd, IDA_T_YES), is_led ? SW_SHOW : SW_HIDE);
        ShowWindow(actl(hd, IDA_T_NO), is_led ? SW_SHOW : SW_HIDE);
    }
}

/* Kalibrier-Zeilen aus Messung + Regler in die Liste stellen. */
static void assist_cal_stage(HWND hd, int a)
{
    char cal[64], dead[64];
    const char *nm = sbc_usb_axis_name(sbc_test_axes[a].cal_axis);
    int i, w = 0;
    char lines[16][96];
    if (!g_ad.c_has[a]) {
        return;
    }
    /* Zeilen aus Spiel-Einheiten (gleiche Formel wie der Kern:
     * Raender 2 nach innen, s. sbc_test_axis_suggest). */
    if (sbc_usb_axis_is_aim(sbc_test_axes[a].cal_axis)) {
        _snprintf_s(cal, sizeof cal, _TRUNCATE, "sbc_cal_%s=%d,%d", nm,
                    g_ad.c_lo[a], g_ad.c_hi[a]);
        dead[0] = '\0';
    } else if (sbc_test_axes[a].kind == 2) {
        _snprintf_s(cal, sizeof cal, _TRUNCATE, "sbc_cal_%s=%d,%d", nm,
                    g_ad.c_lo[a], g_ad.c_hi[a]);
        _snprintf_s(dead, sizeof dead, _TRUNCATE, "sbc_dead_%s=%d", nm,
                    g_ad.c_dead[a]);
    } else {
        _snprintf_s(cal, sizeof cal, _TRUNCATE, "sbc_cal_%s=%d,%d,%d", nm,
                    g_ad.c_lo[a], g_ad.c_mid[a], g_ad.c_hi[a]);
        _snprintf_s(dead, sizeof dead, _TRUNCATE, "sbc_dead_%s=%d", nm,
                    g_ad.c_dead[a]);
    }
    /* Alte Zeilen dieser Achse ersetzen. */
    for (i = 0; i < g_ad.c_nlines; i++) {
        char k[96], *eq;
        strcpy_s(k, sizeof k, g_ad.c_lines[i]);
        eq = strchr(k, '=');
        if (eq) {
            *eq = '\0';
        }
        if ((strncmp(k, "sbc_cal_", 8) == 0 || strncmp(k, "sbc_dead_", 9) == 0) &&
            strcmp(k + (k[4] == 'c' ? 8 : 9), nm) == 0) {
            continue;
        }
        strcpy_s(lines[w], sizeof lines[0], g_ad.c_lines[i]);
        w++;
    }
    strcpy_s(lines[w++], sizeof lines[0], cal);
    if (dead[0] && w < 16) {
        strcpy_s(lines[w++], sizeof lines[0], dead);
    }
    memcpy(g_ad.c_lines, lines, sizeof lines);
    g_ad.c_nlines = w;
    {
        HWND lb = actl(hd, IDA_C_LIST);
        SendMessageW(lb, LB_RESETCONTENT, 0, 0);
        for (i = 0; i < w; i++) {
            list_add(lb, g_ad.c_lines[i], 0, 0);
        }
    }
}

/* user.cfg-Pfad des Clients. */
static void assist_user_cfg(char *out, size_t cap)
{
    lcfg_user_cfg_path(g_client_dir, out, cap); /* W150 USER-DATA */
}

/* Gestufte Zeilen + Reglerwerte speichern (Kalibrier- und Pad-Seite). */
static void assist_save_all(HWND hd, int id_status)
{
    char lines[18][96];
    int n = 0, i;
    char path[1200];
    assist_user_cfg(path, sizeof path);
    for (i = 0; i < g_ad.c_nlines && n < 18; i++) {
        strcpy_s(lines[n++], sizeof lines[0], g_ad.c_lines[i]);
    }
    if (n + 2 <= 18) {
        _snprintf_s(lines[n++], sizeof lines[0], _TRUNCATE, "pad_dz_stick=%d",
                    g_ad.p_dzs);
        _snprintf_s(lines[n++], sizeof lines[0], _TRUNCATE, "pad_dz_trig=%d",
                    g_ad.p_dzt);
    }
    if (assist_cfg_apply(path, lines, n) == 0) {
        char t[1300];
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("as.cal_saved"), path);
        ui_settext(actl(hd, id_status), t);
    } else {
        char t[1300];
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("as.cal_failed"), path);
        ui_settext(actl(hd, id_status), t);
    }
}

/* Datei-Dialoge (Filter/Titel uebersetzt). 1 = Pfad gewaehlt. */
static int assist_open_fixture(HWND hd, char *out, size_t cap)
{
    OPENFILENAMEW ofn;
    static wchar_t file[1024];
    wchar_t filter[256], title[128], f1[128];
    wchar_t *w;
    const char *flt = LT("as.filter_sbc");
    const char *bar = strchr(flt, '|');
    size_t n1;
    file[0] = 0;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = hd;
    ofn.lpstrFile = file;
    ofn.nMaxFile = 1024;
    u8_to_w(LT("as.fixture_title"), title, 128);
    if (bar) {
        char a[128];
        size_t n = (size_t)(bar - flt);
        if (n > sizeof a - 1) {
            n = sizeof a - 1;
        }
        memcpy(a, flt, n);
        a[n] = '\0';
        u8_to_w(a, f1, 128);
        n1 = wcslen(f1) + 1;
        w = filter;
        wcscpy_s(w, 256, f1);
        w += n1;
        u8_to_w(bar + 1, w, (int)(256 - n1));
        w += wcslen(w) + 1;
        *w = 0;
        ofn.lpstrFilter = filter;
    }
    ofn.lpstrTitle = title;
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) {
        return 0;
    }
    w_to_u8(file, out, (int)cap);
    return 1;
}

static int assist_save_report(HWND hd, char *out, size_t cap)
{
    OPENFILENAMEW ofn;
    static wchar_t file[1024];
    wchar_t filter[256], title[128], f1[128];
    wchar_t *w;
    const char *flt = LT("as.filter_txt");
    const char *bar = strchr(flt, '|');
    size_t n1;
    char def[64], ts[32];
    sbc_test_stamp(ts, sizeof ts, 1);
    _snprintf_s(def, sizeof def, _TRUNCATE, "sbc_test_%s.txt", ts);
    u8_to_w(def, file, 1024);
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = hd;
    ofn.lpstrFile = file;
    ofn.nMaxFile = 1024;
    u8_to_w(LT("as.export_title"), title, 128);
    if (bar) {
        char a[128];
        size_t n = (size_t)(bar - flt);
        if (n > sizeof a - 1) {
            n = sizeof a - 1;
        }
        memcpy(a, flt, n);
        a[n] = '\0';
        u8_to_w(a, f1, 128);
        n1 = wcslen(f1) + 1;
        w = filter;
        wcscpy_s(w, 256, f1);
        w += n1;
        u8_to_w(bar + 1, w, (int)(256 - n1));
        w += wcslen(w) + 1;
        *w = 0;
        ofn.lpstrFilter = filter;
    }
    ofn.lpstrTitle = title;
    ofn.nFilterIndex = 1;
    ofn.lpstrDefExt = L"txt";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) {
        return 0;
    }
    w_to_u8(file, out, (int)cap);
    return 1;
}

/* ---- Assistent: Vorgaben aus user.cfg lesen ---- */
static void assist_load_staged(HWND hd)
{
    char lines[24][96];
    char path[1200];
    int n, i, a;
    assist_user_cfg(path, sizeof path);
    n = assist_cfg_read(path, lines, 24);
    for (i = 0; i < n; i++) {
        char *eq = strchr(lines[i], '=');
        const char *v;
        if (!eq) {
            continue;
        }
        *eq = '\0';
        v = eq + 1;
        if (strcmp(lines[i], "pad_dz_stick") == 0) {
            g_ad.p_dzs = atoi(v);
            if (g_ad.p_dzs < 0) g_ad.p_dzs = 0;
            if (g_ad.p_dzs > 90) g_ad.p_dzs = 90;
            SendMessageW(actl(hd, IDA_P_DZS), TBM_SETPOS, TRUE,
                         (LPARAM)g_ad.p_dzs);
            continue;
        }
        if (strcmp(lines[i], "pad_dz_trig") == 0) {
            g_ad.p_dzt = atoi(v);
            if (g_ad.p_dzt < 0) g_ad.p_dzt = 0;
            if (g_ad.p_dzt > 90) g_ad.p_dzt = 90;
            SendMessageW(actl(hd, IDA_P_DZT), TBM_SETPOS, TRUE,
                         (LPARAM)g_ad.p_dzt);
            continue;
        }
        for (a = 0; a < 8; a++) {
            const char *nm = sbc_usb_axis_name(sbc_test_axes[a].cal_axis);
            char kc[48], kd[48];
            _snprintf_s(kc, sizeof kc, _TRUNCATE, "sbc_cal_%s", nm);
            _snprintf_s(kd, sizeof kd, _TRUNCATE, "sbc_dead_%s", nm);
            if (strcmp(lines[i], kc) == 0) {
                int x, y, z;
                if (sscanf_s(v, "%d , %d , %d", &x, &y, &z) == 3) {
                    g_ad.c_lo[a] = x;
                    g_ad.c_mid[a] = y;
                    g_ad.c_hi[a] = z;
                    g_ad.c_has[a] = 1;
                } else if (sscanf_s(v, "%d , %d", &x, &y) == 2) {
                    g_ad.c_lo[a] = x;
                    g_ad.c_mid[a] = 0;
                    g_ad.c_hi[a] = y;
                    g_ad.c_has[a] = 1;
                }
            } else if (strcmp(lines[i], kd) == 0) {
                g_ad.c_dead[a] = atoi(v);
                if (g_ad.c_dead[a] < 0) g_ad.c_dead[a] = 0;
                if (g_ad.c_dead[a] > 100) g_ad.c_dead[a] = 100;
            }
        }
    }
    /* Gestufte Liste = gelesene Kalibrier-Zeilen (ohne Pad). Die obige
     * Schleife hat lines[] zerstoert ('=' -> 0): erneut einlesen. */
    {
        int m = assist_cfg_read(path, g_ad.c_lines, 16);
        int w = 0;
        HWND lb = actl(hd, IDA_C_LIST);
        for (i = 0; i < m; i++) {
            if (strncmp(g_ad.c_lines[i], "pad_dz_", 7) == 0) {
                continue;
            }
            if (w != i) {
                strcpy_s(g_ad.c_lines[w], sizeof g_ad.c_lines[0],
                         g_ad.c_lines[i]);
            }
            w++;
        }
        g_ad.c_nlines = w;
        for (i = 0; i < w; i++) {
            list_add(lb, g_ad.c_lines[i], 0, 0);
        }
    }
}

/* ---- Assistent: Canvas-Rechtecke (logisch) ---- */
static void assist_canvas_rect(int mode, RECT *lr)
{
    switch (mode) {
    case AM_LIVE:
        lr->left = 12;
        lr->top = 94;
        lr->right = 908;
        lr->bottom = 564;
        break;
    case AM_LAMPS:
        lr->left = 12;
        lr->top = 94;
        lr->right = 652;
        lr->bottom = 564;
        break;
    case AM_PAD:
        lr->left = 12;
        lr->top = 94;
        lr->right = 572;
        lr->bottom = 494;
        break;
    default:
        lr->left = lr->top = lr->right = lr->bottom = 0;
        break;
    }
}

static void assist_invalidate_canvas(HWND hd)
{
    RECT lr, r;
    assist_canvas_rect(g_ad.mode, &lr);
    if (lr.right > lr.left) {
        r.left = SX(lr.left);
        r.top = SX(lr.top);
        r.right = SX(lr.right);
        r.bottom = SX(lr.bottom);
        InvalidateRect(hd, &r, FALSE);
    }
}

/* ---- Assistent: Dialog-Prozedur ---- */
static LRESULT CALLBACK assist_proc(HWND hd, UINT msg, WPARAM wp, LPARAM lp);
static void assist_shot_tick(HWND hd);

static void assist_on_command(HWND hd, int id, int code)
{
    (void)code;
    switch (id) {
    case IDA_CLOSE:
    case IDCANCEL:
        g_ad.done = 1;
        break;
    case IDA_M_LIVE:
        assist_show_mode(hd, AM_LIVE);
        break;
    case IDA_M_TEST:
        assist_show_mode(hd, AM_TEST);
        break;
    case IDA_M_CAL:
        assist_show_mode(hd, AM_CAL);
        break;
    case IDA_M_LAMPS:
        assist_show_mode(hd, AM_LAMPS);
        break;
    case IDA_M_PAD:
        assist_show_mode(hd, AM_PAD);
        break;
    case IDA_DEVICE:
        if (g_ad.as && !assist_test_busy(g_ad.as)) {
            g_ad.fx_path[0] = '\0';
            assist_open(g_ad.as, NULL);
            assist_src_text(hd);
            assist_invalidate_canvas(hd);
        }
        break;
    case IDA_FIXTURE:
        if (g_ad.as && !assist_test_busy(g_ad.as)) {
            char p[1024];
            if (assist_open_fixture(hd, p, sizeof p)) {
                strcpy_s(g_ad.fx_path, sizeof g_ad.fx_path, p);
                assist_open(g_ad.as, p);
                assist_src_text(hd);
                assist_invalidate_canvas(hd);
            }
        }
        break;
    case IDA_T_START:
        if (g_ad.as && !assist_test_busy(g_ad.as)) {
            const sbc_test_res_t *res = assist_results(g_ad.as);
            int from = 0, i;
            for (i = 0; i < SBC_TEST_N_STEPS; i++) {
                if (res[i].status == SBC_TR_NOTRUN) {
                    from = i;
                    break;
                }
                from = i + 1;
            }
            if (from >= SBC_TEST_N_STEPS) {
                from = 0;
            }
            g_ad.t_running = 1;
            assist_test_run(g_ad.as, from);
        }
        break;
    case IDA_T_STOP:
        if (g_ad.as) {
            assist_test_stop(g_ad.as);
            g_ad.t_running = 0;
        }
        break;
    case IDA_T_SKIP:
        if (g_ad.as && assist_test_busy(g_ad.as)) {
            assist_test_key(g_ad.as, '\r');
        }
        break;
    case IDA_T_BACK: {
        if (g_ad.as && !assist_test_busy(g_ad.as)) {
            const sbc_test_res_t *res = assist_results(g_ad.as);
            int i, tgt = 0;
            for (i = 0; i < SBC_TEST_N_STEPS; i++) {
                if (res[i].status != SBC_TR_NOTRUN) {
                    tgt = i;
                }
            }
            assist_results_clear(g_ad.as, tgt);
            assist_test_refresh(hd);
            assist_test_step(g_ad.as, tgt);
        }
        break;
    }
    case IDA_T_EXPORT:
        if (g_ad.as && !assist_test_busy(g_ad.as)) {
            char p[1024];
            if (assist_save_report(hd, p, sizeof p)) {
                assist_write_report(g_ad.as, p);
            }
        }
        break;
    case IDA_T_YES:
        if (g_ad.as) {
            assist_test_key(g_ad.as, 'j');
        }
        break;
    case IDA_T_NO:
        if (g_ad.as) {
            assist_test_key(g_ad.as, 'n');
        }
        break;
    case IDA_C_MEASURE:
        if (g_ad.as && !assist_test_busy(g_ad.as)) {
            int sel = (int)SendMessageW(actl(hd, IDA_C_AXIS), CB_GETCURSEL,
                                        0, 0);
            if (sel >= 0 && sel < 8) {
                char p[220];
                g_ad.c_axis = sel;
                _snprintf_s(p, sizeof p, _TRUNCATE,
                            LT(sbc_test_axes[sel].kind == 2 ? "as.p_pedal"
                                                            : "as.p_axis"),
                            LT(kAxKey[sel]));
                ui_settext(actl(hd, IDA_C_VALS), p);
                assist_cal_axis(g_ad.as, sel);
            }
        }
        break;
    case IDA_C_APPLY:
        assist_cal_stage(hd, g_ad.c_axis);
        break;
    case IDA_C_TUNER:
        if (g_ad.as && !assist_test_busy(g_ad.as)) {
            assist_cal_extra(g_ad.as, ASSIST_CAL_TUNER);
        }
        break;
    case IDA_C_GEAR:
        if (g_ad.as && !assist_test_busy(g_ad.as)) {
            assist_cal_extra(g_ad.as, ASSIST_CAL_GEAR);
        }
        break;
    case IDA_C_SAVE:
        assist_save_all(hd, IDA_C_STATUS);
        break;
    case IDA_C_RESET: {
        char path[1200];
        assist_user_cfg(path, sizeof path);
        if (assist_cfg_reset(path) == 0) {
            ui_settext(actl(hd, IDA_C_STATUS), LT("as.cal_reset_done"));
        } else {
            char t[1300];
            _snprintf_s(t, sizeof t, _TRUNCATE, LT("as.cal_failed"), path);
            ui_settext(actl(hd, IDA_C_STATUS), t);
        }
        break;
    }
    case IDA_L_SEQ:
        if (g_ad.as && !assist_test_busy(g_ad.as)) {
            g_ad.l_seq = 1;
            assist_led_seq_start(g_ad.as);
        }
        break;
    case IDA_L_ON:
        if (g_ad.as && !assist_test_busy(g_ad.as)) {
            assist_led_all(g_ad.as, 0xF);
            assist_invalidate_canvas(hd);
        }
        break;
    case IDA_L_OFF:
        if (g_ad.as && !assist_test_busy(g_ad.as)) {
            assist_led_all(g_ad.as, 0);
            assist_invalidate_canvas(hd);
        }
        break;
    case IDA_P_SAVE:
        assist_save_all(hd, IDA_P_STATUS);
        break;
    }
}

/* Engine-Ereignis (UI-Thread, via WM_APP_AE). */
static void assist_on_event(HWND hd, int ev, assist_evt_t *e)
{
    switch (ev) {
    case ASSIST_EV_SOURCE:
        assist_src_text(hd);
        assist_invalidate_canvas(hd);
        break;
    case ASSIST_EV_TEST_START:
        assist_test_start(hd, e->a);
        if (g_o.shot == 5 && strcmp(g_o.assist_mode, "test") == 0 && e->a == 1) {
            /* Nur bei Schritt 2 schaerfen (Schritt 1 steht, Rest ausstehend). */
            g_ad.shot_phase = 1;
            g_ad.shot_t0 = GetTickCount64();
        }
        if (g_o.shot == 5 && strcmp(g_o.assist_mode, "result") == 0 &&
            e->a == SBC_TEST_N_STEPS - 1) {
            g_ad.shot_led_t0 = GetTickCount64();
        }
        break;
    case ASSIST_EV_TEST_LINE:
        assist_test_line(hd, e->text);
        break;
    case ASSIST_EV_TEST_DONE:
        assist_test_refresh(hd);
        ShowWindow(actl(hd, IDA_T_YES), SW_HIDE);
        ShowWindow(actl(hd, IDA_T_NO), SW_HIDE);
        break;
    case ASSIST_EV_TEST_END:
        g_ad.t_running = 0;
        g_ad.l_seq = 0;
        assist_test_refresh(hd);
        assist_invalidate_canvas(hd);
        if (g_o.shot == 5 && strcmp(g_o.assist_mode, "result") == 0) {
            g_ad.shot_phase = 2;
        }
        break;
    case ASSIST_EV_CAL_DONE: {
        int what = 0;
        sbc_test_res_t r;
        if (g_ad.as && assist_cal_result(g_ad.as, &what, &r)) {
            if (what >= 0 && what < 8 && r.measured) {
                int a = what;
                int lo = sbc_test_game_units(a, r.mn);
                int hi = sbc_test_game_units(a, r.mx);
                int rl = sbc_test_game_units(a, r.rest_lo);
                int rh = sbc_test_game_units(a, r.rest_hi);
                int mid = sbc_test_game_units_round(a,
                                                    (r.rest_lo + r.rest_hi) / 2);
                char t[220];
                if (sbc_usb_axis_is_aim(sbc_test_axes[a].cal_axis)) {
                    g_ad.c_lo[a] = lo + 2;
                    g_ad.c_hi[a] = hi - 2;
                    g_ad.c_has[a] = 1;
                } else if (sbc_test_axes[a].kind == 2) {
                    g_ad.c_lo[a] = rl;
                    g_ad.c_hi[a] = hi - 2;
                    g_ad.c_dead[a] = (rh - rl) + 2;
                    g_ad.c_has[a] = 1;
                } else {
                    int d = (rh - mid > mid - rl ? rh - mid : mid - rl) + 2;
                    g_ad.c_lo[a] = lo + 2;
                    g_ad.c_mid[a] = mid;
                    g_ad.c_hi[a] = hi - 2;
                    g_ad.c_dead[a] = d;
                    g_ad.c_has[a] = 1;
                }
                _snprintf_s(t, sizeof t, _TRUNCATE, "%s %d..%d %s %d %s %d",
                            LT("as.cal_rest"), rl, rh, LT("as.cal_min"), lo,
                            LT("as.cal_max"), hi);
                ui_settext(actl(hd, IDA_C_VALS), t);
                SendMessageW(actl(hd, IDA_C_DEAD), TBM_SETPOS, TRUE,
                             (LPARAM)g_ad.c_dead[a]);
            } else if (what >= 0 && what < 8) {
                ui_settext(actl(hd, IDA_C_VALS), r.note);
            } else {
                char t[256];
                _snprintf_s(t, sizeof t, _TRUNCATE, "%s: %s",
                            assist_st_name(r.status), r.note);
                ui_settext(actl(hd, IDA_C_EXTRA), t);
            }
        }
        if (g_o.shot == 5 && strcmp(g_o.assist_mode, "cal") == 0) {
            g_ad.shot_phase = 2;
        }
        break;
    }
    case ASSIST_EV_LED:
        assist_invalidate_canvas(hd);
        if (e->a == SBC_TEST_LED_SEQ_N - 1 && g_ad.mode == AM_TEST) {
            /* Folge zu Ende: Bestaetigungsfrage zeigen. */
            ui_settext(actl(hd, IDA_T_PROMPT), LT("as.p_led_confirm"));
        }
        break;
    default:
        break;
    }
    assist_evt_free(e);
}

/* Timer (100 ms): Quelle, Pad, Vorschau, Neuzeichnen. */
static void assist_on_timer(HWND hd)
{
    if (g_ad.as) {
        assist_poll(g_ad.as);
    }
    {
        lctrl_pad_t cur;
        int present = g_ad.as ? lctrl_pad_first(&cur) >= 0 : 0;
        if (present) {
            g_ad.pad = cur;
        }
        if (present != g_ad.pad_present) {
            g_ad.pad_present = present;
            if (g_ad.mode == AM_PAD) {
                ShowWindow(actl(hd, IDA_P_NONE),
                           present ? SW_HIDE : SW_SHOW);
                assist_invalidate_canvas(hd);
            }
        } else if (present && g_ad.mode == AM_PAD) {
            assist_invalidate_canvas(hd);
        }
    }
    if (InterlockedExchange(&g_as_state_flag, 0)) {
        assist_invalidate_canvas(hd);
    }
    /* Live-Status (Hover setzt eigenen Text; sonst Achswerte). */
    if (g_ad.mode == AM_LIVE && g_ad.as && assist_has_report(g_ad.as)) {
        uint8_t raw[SBC_USB_IN_LEN];
        char t[300];
        assist_raw(g_ad.as, raw);
        _snprintf_s(t, sizeof t, _TRUNCATE,
                    "aim %d/%d rot %+d sight %+d/%+d ped %d/%d/%d tun %d gear %d",
                    sbc_test_raw_axis(raw, 0) / 256,
                    sbc_test_raw_axis(raw, 1) / 256,
                    sbc_test_raw_axis(raw, 2) / 256,
                    sbc_test_raw_axis(raw, 3) / 256,
                    sbc_test_raw_axis(raw, 4) / 256,
                    sbc_test_raw_axis(raw, 5) / 256,
                    sbc_test_raw_axis(raw, 6) / 256,
                    sbc_test_raw_axis(raw, 7) / 256, raw[24] & 0x0F,
                    raw[25]);
        ui_settext(actl(hd, IDA_LIVE_ST), t);
    }
    /* Totzonen-Vorschau (Kalibrierseite). */
    if (g_ad.mode == AM_CAL && g_ad.as) {
        int a = g_ad.c_axis, rawv = 0, gamev = 0;
        char t[64];
        int dead = (int)SendMessageW(actl(hd, IDA_C_DEAD), TBM_GETPOS, 0, 0);
        int is_aim = sbc_usb_axis_is_aim(sbc_test_axes[a].cal_axis);
        g_ad.c_dead[a] = dead;
        _snprintf_s(t, sizeof t, _TRUNCATE, "%s: %d", LT("as.cal_dead"),
                    dead);
        ui_settext(actl(hd, IDA_C_DEADV), t);
        EnableWindow(actl(hd, IDA_C_DEAD), !is_aim);
        if (assist_preview(g_ad.as, a, g_ad.c_has[a], g_ad.c_lo[a],
                           g_ad.c_mid[a], g_ad.c_hi[a], dead, &rawv,
                           &gamev)) {
            _snprintf_s(t, sizeof t, _TRUNCATE, "%s %d -> %s %d",
                        LT("as.cal_raw"), rawv, LT("as.cal_game"), gamev);
        } else {
            _snprintf_s(t, sizeof t, _TRUNCATE, "%s - -> %s -",
                        LT("as.cal_raw"), LT("as.cal_game"));
        }
        ui_settext(actl(hd, IDA_C_PREV), t);
        ui_settext(actl(hd, IDA_C_HINT),
                   LT(is_aim ? "as.cal_hint_aim" : "as.cal_hint_nocal"));
    }
    /* Pad-Reglertexte. */
    if (g_ad.mode == AM_PAD) {
        char t[96];
        g_ad.p_dzs = (int)SendMessageW(actl(hd, IDA_P_DZS), TBM_GETPOS, 0,
                                       0);
        g_ad.p_dzt = (int)SendMessageW(actl(hd, IDA_P_DZT), TBM_GETPOS, 0,
                                       0);
        _snprintf_s(t, sizeof t, _TRUNCATE, "%s: %d %%", LT("as.pad_dz_stick"),
                    g_ad.p_dzs);
        ui_settext(actl(hd, IDA_P_DZSV), t);
        _snprintf_s(t, sizeof t, _TRUNCATE, "%s: %d %%", LT("as.pad_dz_trig"),
                    g_ad.p_dzt);
        ui_settext(actl(hd, IDA_P_DZTV), t);
    }
}

/* Maus ueber Canvas (logische Koordinaten). */
static void assist_on_mouse(HWND hd, int x, int y, int down)
{
    RECT lr, r;
    assist_canvas_rect(g_ad.mode, &lr);
    if (lr.right <= lr.left) {
        return;
    }
    r.left = SX(lr.left);
    r.top = SX(lr.top);
    r.right = SX(lr.right);
    r.bottom = SX(lr.bottom);
    if (x < r.left || x >= r.right || y < r.top || y >= r.bottom) {
        return;
    }
    {
        int lx = (x - r.left) * 96 / g_dpi;
        int ly = (y - r.top) * 96 / g_dpi;
        if (g_ad.mode == AM_LIVE) {
            int bi = assist_live_hit(lx, ly);
            if (bi >= 0) {
                char t[200];
                uint8_t raw[SBC_USB_IN_LEN];
                int on = 0;
                if (g_ad.as && assist_has_report(g_ad.as)) {
                    assist_raw(g_ad.as, raw);
                    on = (sbc_test_raw_btns(raw) & (1ULL << bi)) != 0;
                }
                _snprintf_s(t, sizeof t, _TRUNCATE, "%s (bi%d): %s",
                            assist_btn_name(bi), bi,
                            on ? LT("as.st_ok") : LT("as.st_notrun"));
                ui_settext(actl(hd, IDA_LIVE_ST), t);
            }
        } else if (g_ad.mode == AM_LAMPS) {
            int led = assist_lamp_hit(lx, ly);
            if (led != g_ad.l_hover) {
                g_ad.l_hover = led;
                ui_settext(actl(hd, IDA_L_NAME),
                           led >= 0 ? assist_led_name(led) : "");
                assist_invalidate_canvas(hd);
            }
            if (down && led >= 0 && g_ad.as && !assist_test_busy(g_ad.as)) {
                int cur = assist_led_level(g_ad.as, led);
                assist_led_set(g_ad.as, led, cur ? 0 : 0xF);
                assist_invalidate_canvas(hd);
            }
        }
    }
}

static void assist_paint(HWND hd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hd, &ps);
    RECT lr;
    assist_canvas_rect(g_ad.mode, &lr);
    if (lr.right > lr.left && dc) {
        RECT r;
        HDC md = CreateCompatibleDC(dc);
        HBITMAP bm = NULL, ob = NULL;
        r.left = SX(lr.left);
        r.top = SX(lr.top);
        r.right = SX(lr.right);
        r.bottom = SX(lr.bottom);
        if (md) {
            bm = CreateCompatibleBitmap(dc, r.right - r.left,
                                        r.bottom - r.top);
            if (bm) {
                ob = (HBITMAP)SelectObject(md, bm);
            }
        }
        if (md && bm) {
            HBRUSH bg = CreateSolidBrush(C_DIALOG);
            RECT fr;
            fr.left = 0;
            fr.top = 0;
            fr.right = r.right - r.left;
            fr.bottom = r.bottom - r.top;
            FillRect(md, &fr, bg);
            DeleteObject(bg);
            /* Canvas-Koordinaten sind canvas-lokal (kein Viewport-Versatz). */
            if (g_ad.mode == AM_LIVE) {
                assist_paint_live(md, r.right - r.left,
                                  r.bottom - r.top);
            } else if (g_ad.mode == AM_LAMPS) {
                assist_paint_lamps(md, r.right - r.left,
                                   r.bottom - r.top);
            } else if (g_ad.mode == AM_PAD) {
                assist_paint_pad(md, r.right - r.left,
                                 r.bottom - r.top);
            }
            BitBlt(dc, r.left, r.top, r.right - r.left,
                   r.bottom - r.top, md, 0, 0, SRCCOPY);
            SelectObject(md, ob);
            DeleteObject(bm);
            DeleteDC(md);
        }
    }
    EndPaint(hd, &ps);
}

static LRESULT CALLBACK assist_proc(HWND hd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        assist_on_command(hd, LOWORD(wp), HIWORD(wp));
        return 0;
    case WM_DRAWITEM:
        draw_item((const DRAWITEMSTRUCT *)lp);
        return TRUE;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORSTATIC:
        return ctlcolor((HDC)wp, (HWND)lp, msg == WM_CTLCOLORSTATIC);
    case WM_ERASEBKGND: {
        RECT r;
        GetClientRect(hd, &r);
        if (!g_br_dialog) {
            g_br_dialog = CreateSolidBrush(C_DIALOG);
        }
        FillRect((HDC)wp, &r, g_br_dialog);
        return 1;
    }
    case WM_PAINT:
        assist_paint(hd);
        return 0;
    case WM_TIMER:
        if (wp == 7) {
            assist_on_timer(hd);
        } else if (wp == 9 && g_o.shot == 5) {
            assist_shot_tick(hd);
        }
        return 0;
    case WM_HSCROLL:
        /* Regler: Texte frischt der Timer auf; sofort neu zeichnen. */
        assist_on_timer(hd);
        return 0;
    case WM_MOUSEMOVE:
        assist_on_mouse(hd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), 0);
        return 0;
    case WM_LBUTTONDOWN:
        assist_on_mouse(hd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), 1);
        return 0;
    case WM_DEVICECHANGE:
        /* Geraet ein/aus: naechster Timer-Poll erkennt es. */
        return 0;
    case WM_APP_AE:
        assist_on_event(hd, (int)wp, (assist_evt_t *)lp);
        return 0;
    case WM_CLOSE:
        g_ad.done = 1;
        return 0;
    }
    return DefWindowProcW(hd, msg, wp, lp);
}

/* ---- Assistent: Shot-Treiber (shot=5, nie sichtbar) ---- */
static void assist_shot_save(HWND hd, const char *mode)
{
    char path[1200];
    /* Ruhe vor dem Shot: Arbeiter anhalten, Timer aus, Schlange leeren,
     * alles synchron malen (kein Tearing im DWM-Bild). */
    if (g_ad.as && assist_test_busy(g_ad.as)) {
        assist_test_stop(g_ad.as);
    }
    KillTimer(hd, 7);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s/w143_assist_%s_%s_%d.png",
                g_o.out_dir, mode, sb_lang_code(), g_dpi * 100 / 96);
    RedrawWindow(hd, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    pump();
    Sleep(150);
    {
        int r = shot_window(hd, path, 1);
        printf("assist-shot: %s -> %s%s\n", mode, r ? "FEHLER " : "", path);
    }
}

/* Shot-Setup je Modus (nach Aufbau, vor der Schleife). */
static void assist_shot_setup(HWND hd, const char *mode)
{
    const char *fx = g_o.assist_fixture[0] ? g_o.assist_fixture : NULL;
    g_ad.shot_phase = 0;
    g_ad.shot_t0 = GetTickCount64();
    g_ad.shot_led_t0 = 0;
    if (strcmp(mode, "live") == 0) {
        assist_show_mode(hd, AM_LIVE);
        if (fx) {
            strcpy_s(g_ad.fx_path, sizeof g_ad.fx_path, fx);
            assist_open(g_ad.as, fx);
        }
        assist_set_fast(g_ad.as, 1);
        assist_src_text(hd);
    } else if (strcmp(mode, "test") == 0) {
        assist_show_mode(hd, AM_TEST);
        if (fx) {
            strcpy_s(g_ad.fx_path, sizeof g_ad.fx_path, fx);
            assist_open(g_ad.as, fx);
        }
        /* Echtzeit (kein fast): Schritt 2 steht beim Shot noch an. */
        assist_src_text(hd);
        g_ad.t_running = 1;
        assist_test_run(g_ad.as, 0);
    } else if (strcmp(mode, "result") == 0) {
        assist_show_mode(hd, AM_TEST);
        if (fx) {
            strcpy_s(g_ad.fx_path, sizeof g_ad.fx_path, fx);
            assist_open(g_ad.as, fx);
        }
        assist_set_fast(g_ad.as, 1);
        assist_src_text(hd);
        g_ad.t_running = 1;
        assist_test_run(g_ad.as, 0);
    } else if (strcmp(mode, "cal") == 0) {
        assist_show_mode(hd, AM_CAL);
        if (fx) {
            strcpy_s(g_ad.fx_path, sizeof g_ad.fx_path, fx);
            assist_open(g_ad.as, fx);
        }
        assist_set_fast(g_ad.as, 1);
        assist_src_text(hd);
        g_ad.c_axis = 2;
        SendMessageW(actl(hd, IDA_C_AXIS), CB_SETCURSEL, (WPARAM)2, 0);
        assist_cal_axis(g_ad.as, 2);
    } else if (strcmp(mode, "lamps") == 0) {
        assist_show_mode(hd, AM_LAMPS);
        if (fx) {
            strcpy_s(g_ad.fx_path, sizeof g_ad.fx_path, fx);
            assist_open(g_ad.as, fx);
        }
        assist_src_text(hd);
        assist_led_set(g_ad.as, 1, 0xF);
        assist_led_set(g_ad.as, 5, 0xF);
        assist_led_set(g_ad.as, 9, 0xF);
        assist_invalidate_canvas(hd);
    } else if (strcmp(mode, "gamepad") == 0) {
        assist_show_mode(hd, AM_PAD);
    } else { /* nodev */
        assist_show_mode(hd, AM_LIVE);
        assist_open(g_ad.as, "__none__"); /* laedt nie: Hinweis zeigen */
        assist_src_text(hd);
    }
}

/* Shot-Tick (50 ms): Warten, ggf. 'j' einspeisen, Shot, fertig. */
static void assist_shot_tick(HWND hd)
{
    const char *mode = g_o.assist_mode;
    ULONGLONG now = GetTickCount64();
    if (strcmp(mode, "live") == 0 || strcmp(mode, "lamps") == 0 ||
        strcmp(mode, "gamepad") == 0 || strcmp(mode, "nodev") == 0) {
        if (now - g_ad.shot_t0 > 600) {
            KillTimer(hd, 9);
            assist_shot_save(hd, mode);
            g_ad.done = 1;
        }
        return;
    }
    if (strcmp(mode, "test") == 0) {
        /* Schritt 2 laeuft (Schritt 1 OK), dann Shot (Test bricht danach ab). */
        if (g_ad.shot_phase == 1 && now - g_ad.shot_t0 > 800) {
            KillTimer(hd, 9);
            assist_shot_save(hd, mode);
            assist_test_stop(g_ad.as);
            g_ad.done = 1;
        } else if (now - g_ad.shot_t0 > 30000) {
            KillTimer(hd, 9);
            printf("assist-shot: test Zeit abgelaufen\n");
            assist_test_stop(g_ad.as);
            g_ad.done = 1;
        }
        return;
    }
    if (strcmp(mode, "result") == 0) {
        /* Lampenfrage automatisch mit Ja beantworten (Folge + Reserve). */
        if (g_ad.shot_led_t0 && now - g_ad.shot_led_t0 > 2500 &&
            assist_test_busy(g_ad.as) &&
            assist_test_current(g_ad.as) == SBC_TEST_N_STEPS - 1) {
            g_ad.shot_led_t0 = 0;
            assist_test_key(g_ad.as, 'j');
        }
        if (g_ad.shot_phase == 2) {
            char path[1200];
            KillTimer(hd, 9);
            _snprintf_s(path, sizeof path, _TRUNCATE,
                        "%s/w143_assist_result_%s.txt", g_o.out_dir,
                        sb_lang_code());
            if (assist_write_report(g_ad.as, path) == 0) {
                printf("assist-shot: Ergebnisdatei %s\n", path);
            } else {
                printf("assist-shot: Ergebnisdatei FEHLER\n");
            }
            assist_shot_save(hd, mode);
            g_ad.done = 1;
        } else if (now - g_ad.shot_t0 > 180000) {
            KillTimer(hd, 9);
            printf("assist-shot: result Zeit abgelaufen\n");
            assist_test_stop(g_ad.as);
            g_ad.done = 1;
        }
        return;
    }
    if (strcmp(mode, "cal") == 0) {
        if (g_ad.shot_phase == 2) {
            KillTimer(hd, 9);
            assist_cal_stage(hd, 2);
            assist_shot_save(hd, mode);
            g_ad.done = 1;
        } else if (now - g_ad.shot_t0 > 120000) {
            KillTimer(hd, 9);
            printf("assist-shot: cal Zeit abgelaufen\n");
            assist_test_stop(g_ad.as);
            g_ad.done = 1;
        }
        return;
    }
}

static void assist_dialog_modal(const char *fixture)
{
    static int reg;
    HWND hd;
    RECT rc = { 0, 0, 0, 0 };
    MSG msg;
    int ui_mark = g_ui_n;
    int shot = g_o.shot != 0;
    DWORD exs = shot ? (WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW) : 0;
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    assist_cb_t cb;
    if (!reg) {
        WNDCLASSW wc;
        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = assist_proc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = L"SbAssist";
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        RegisterClassW(&wc);
        reg = 1;
    }
    memset(&g_ad, 0, sizeof g_ad);
    g_ad.mode = AM_LIVE;
    g_ad.l_hover = -1;
    g_ad.c_axis = 2;
    cb.event = assist_cb;
    cb.ctx = &g_ad;
    g_ad.as = assist_create(&cb);
    if (!g_ad.as) {
        return;
    }
    rc.right = SX(AD_W);
    rc.bottom = SX(AD_H);
    AdjustWindowRectEx(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE,
                       exs);
    if (shot) {
        offscreen_pos(&x, &y);
    } else if (g_hwnd) {
        RECT pr;
        GetWindowRect(g_hwnd, &pr);
        x = pr.left + ((pr.right - pr.left) - (rc.right - rc.left)) / 2;
        y = pr.top + ((pr.bottom - pr.top) - (rc.bottom - rc.top)) / 2;
    }
    {
        wchar_t title[64];
        u8_to_w(LT("as.title"), title, 64);
        hd = CreateWindowExW(exs, L"SbAssist", title,
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, x, y,
                             rc.right - rc.left, rc.bottom - rc.top, g_hwnd, NULL,
                             GetModuleHandleW(NULL), NULL);
    }
    if (!hd) {
        assist_destroy(g_ad.as);
        g_ad.as = NULL;
        ui_msgbox(g_hwnd, LT("as.title"), LT("as.title"),
                  MB_ICONERROR | MB_OK);
        return;
    }
    g_ad.hd = hd;
    g_assist_hd = hd;
    {
        BOOL dark = TRUE;
        DwmSetWindowAttribute(hd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */,
                              &dark, sizeof dark);
    }
    if (fixture && fixture[0]) {
        strcpy_s(g_ad.fx_path, sizeof g_ad.fx_path, fixture);
    }
    if (g_o.shot == 5 && (!fixture || !fixture[0])) {
        /* Shots greifen nie aufs echte Geraet zu (nur Fixtures). */
        assist_open(g_ad.as, "__none__");
    } else {
        assist_open(g_ad.as, fixture && fixture[0] ? fixture : NULL);
    }
    assist_build(hd);
    assist_load_staged(hd);
    assist_src_text(hd);
    assist_show_mode(hd, AM_LIVE);
    SetTimer(hd, 7, 100, NULL);
    if (g_o.shot == 5) {
        SetTimer(hd, 9, 50, NULL);
        assist_shot_setup(hd, g_o.assist_mode);
    }
    if (g_hwnd) {
        EnableWindow(g_hwnd, FALSE);
    }
    if (shot) {
        if (!is_offscreen(hd)) {
            printf("assist-shot: FEHLER Fenster nicht au" U8_SZ
                   "erhalb des Bildschirms - Abbruch\n");
            g_ad.done = 1;
        } else {
            ShowWindow(hd, SW_SHOWNOACTIVATE);
        }
    } else {
        ShowWindow(hd, SW_SHOW);
    }
    UpdateWindow(hd);
    while (!g_ad.done && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
            uictl_t *u = ui_by_hwnd(GetFocus());
            if (u && u->kind < K_EDIT && u->parent == hd &&
                IsWindowEnabled(u->h)) {
                SendMessageW(hd, WM_COMMAND, MAKEWPARAM(u->id, BN_CLICKED),
                             (LPARAM)u->h);
                continue;
            }
        }
        if (IsDialogMessageW(hd, &msg)) {
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    KillTimer(hd, 7);
    KillTimer(hd, 9);
    assist_test_stop(g_ad.as);
    assist_led_all(g_ad.as, 0); /* Lampen aus beim Schliessen */
    assist_destroy(g_ad.as);
    g_ad.as = NULL;
    if (g_hwnd) {
        EnableWindow(g_hwnd, TRUE);
    }
    DestroyWindow(hd);
    g_assist_hd = NULL;
    g_ui_n = ui_mark; /* Dialog-Elemente austragen */
}

static void open_assist_dialog(void)
{
    assist_dialog_modal(NULL);
}

static void open_bind_dialog(void)
{
    char prof[64], path[1200];
    const char *base = NULL;
    const char *name;
    ctrl_profile_from_ui(prof, sizeof prof);
    name = prof[0] ? prof : "keyboard_default"; /* Legacy-Basis */
    if (lctrl_input_profile_path(g_client_dir, name, path, sizeof path) == 0 &&
        u8_GetFileAttributes(path) != INVALID_FILE_ATTRIBUTES) {
        base = path;
    }
    bind_dialog_modal(name, base);
}

/* ------------------------------------------------------------------ */
/* Seiten                                                             */
/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */
/* W148 PACK-LAUNCHER: Seite "Packs" (Zustand + Engine)                 */
/* W172 LAUNCHER-P5: Liste mit Zustand je Zeile (Schalter, Typ,          */
/* Abdeckung, Status), Ziehen + Alt+Pfeil, Texturdetail als Segment,     */
/* Grenzen je Kategorie mit Klarnamen, Detailbereich (Abdeckung,         */
/* Konflikte, Effektwerte mit Schloss, passt zum Spiel), Klartext-       */
/* Pruefung, Import per Knopf/Ziehen & Ablegen, leerer Zustand.          */
/* ------------------------------------------------------------------ */
#define PK_MAXN 32
#define PK_MAXCAP 32
#define PK_MAXOVER 512
static sb_pack_order_t g_pk_ord[PK_MAXN];
static int g_pk_n;
static char g_pk_level[8];
static sb_pack_cap_t g_pk_caps[PK_MAXCAP];
static int g_pk_ncaps;
static sb_pack_over_t g_pk_overs[PK_MAXOVER];
static int g_pk_novers;
static sb_pack_scan_t g_pk_scan[PK_MAXN];
static int g_pk_ne[PK_MAXN], g_pk_sh[PK_MAXN];
static int g_pk_chk[PK_MAXN]; /* -2 = ungeprueft, sonst 0/1/2 */
static char g_pk_chk_reason[PK_MAXN][64];
static char g_pk_chk_detail[PK_MAXN][300];
static int g_pk_chk_xbe[PK_MAXN];        /* W172: mit Spielversion geprueft */
static sb_pack_cov_t g_pk_cov[PK_MAXN];  /* W172: Abdeckung je Kategorie */
static int g_pk_covby[PK_MAXN * PK_MAXN]; /* W172: [i * N + w] = verdeckt durch w */
static int g_pk_fx_hit[SB_FXN], g_pk_fx_val[SB_FXN]; /* W172: Effektwerte (Gewinner) */
static char g_pk_fx_src[SB_FXN][128];
static int g_pk_sel = -1;
static char g_pk_pj[1100]; /* packs.json (UTF-8, Anzeige + Dialoge) */
static int g_pk_have;      /* 1 = packs.json lesbar */
static int g_pk_drag = -1, g_pk_drop = -1; /* W172: Ziehen (Quelle, Ziel) */
static int g_pk_dragging;

/* Klarnamen der Kategorien (Reihenfolge = kSbPackCat, Glossar 7.1). */
static const char *const kPkCatKey[SB_PACK_NCAT] = {
    "gui.pkcat_hud", "gui.pkcat_cockpit", "gui.pkcat_ground",
    "gui.pkcat_buildings", "gui.pkcat_vehicles", "gui.pkcat_effects",
    "gui.pkcat_menu", "gui.pkcat_world", "gui.pkcat_other"
};
/* Klarnamen der Effektwerte (Reihenfolge = kSbFxParams). */
static const char *const kPkFxKey[SB_FXN] = {
    "gui.fxp_soft", "gui.fxp_embers", "gui.fxp_light", "gui.fxp_lightckp",
    "gui.fxp_interp", "gui.fxp_bloom", "gui.fxp_sat", "gui.fxp_temp",
    "gui.fxp_lift", "gui.fxp_gamma", "gui.fxp_gain"
};

/* UTF-8 -> ANSI fuer die sb_pack-Aufrufe (A-API, ASCII verlustfrei). */
static void pk_acp(const char *u8, char *out, size_t cap)
{
    u8_to_acp(u8, out, cap);
}

static void packs_json_path(char *out, size_t cap)
{
    char dir[1024], env[1024];
    if (u8_GetEnv("SB_PACKS_DIR", env, sizeof env) > 0 && env[0]) {
        /* Test-Haken (Shots, Proben): Packs-Ordner vorgeben. */
        _snprintf_s(out, cap, _TRUNCATE, "%s/packs.json", env);
        return;
    }
    { /* W150 USER-DATA: Profil/portabel -> <Datenordner>/packs */
        char acp[1024];
        if (sb_userdata_path(SB_UD_PACKS, acp, sizeof acp) == 1) {
            u8_from_acp(acp, dir, sizeof dir);
            _snprintf_s(out, cap, _TRUNCATE, "%s/packs.json", dir);
            return;
        }
    }
    rem_pkg_detect(g_client_disp, dir, sizeof dir);
    _snprintf_s(out, cap, _TRUNCATE, "%s/packs/packs.json", dir);
}

static void packs_dir_of(char *out, size_t cap)
{
    char *s;
    strncpy_s(out, cap, g_pk_pj, _TRUNCATE);
    for (s = out; *s; s++) {
        if (*s == '\\') {
            *s = '/';
        }
    }
    s = strrchr(out, '/');
    if (s) {
        *s = '\0';
    }
}

static int packs_count(void)
{
    return g_pk_n;
}

/* Pruefstand gewinnt ueber den Schnellstatus (2 gruen, 1 gelb, 0 rot). */
static int packs_status_of(int i)
{
    if (i < 0 || i >= g_pk_n) {
        return -1;
    }
    return g_pk_chk[i] >= -1 ? g_pk_chk[i] : g_pk_scan[i].status;
}

static int packs_enabled_of(int i)
{
    return i >= 0 && i < g_pk_n && g_pk_ord[i].enabled;
}

static int packs_is_fx(int i)
{
    return i >= 0 && i < g_pk_n && strcmp(g_pk_scan[i].type, "effect") == 0;
}

static int packs_level_num(void)
{
    return strcmp(g_pk_level, "orig") == 0 ? 0
           : strcmp(g_pk_level, "x2") == 0 ? 2 : 4;
}

static const char *packs_cap_str(int cap)
{
    if (cap <= 0) {
        return LT("gui.pk_orig");
    }
    return cap <= 2 ? LT("gui.pk_x2") : LT("gui.pk_x4");
}

static const char *packs_level_word(void)
{
    return packs_cap_str(packs_level_num());
}

/* Gespeicherte Grenze einer Kategorie (-1 = keine). "effects" + "effect"
 * sind eine Kategorie (Kunden-Grenze gilt nur exakt je Name, s. Speichern). */
static int packs_cap_stored(int c)
{
    int i;
    for (i = 0; i < g_pk_ncaps; i++) {
        if (strcmp(g_pk_caps[i].name, kSbPackCat[c]) == 0 ||
            (c == 5 && strcmp(g_pk_caps[i].name, "effect") == 0)) {
            return g_pk_caps[i].cap;
        }
    }
    return -1;
}

/* Wirksame Grenze je Kategorie (nie ueber dem Texturdetail). */
static int packs_cap_of(int c)
{
    int lv = packs_level_num(), cap = packs_cap_stored(c);
    return (cap >= 0 && cap < lv) ? cap : lv;
}

/* Grenze setzen (v >= Texturdetail = keine Grenze). "effects" schreibt
 * auch "effect" (Manifeste nutzen beide Namen, der Client vergleicht exakt). */
static void packs_cap_name(const char *name, int v)
{
    int i, k;
    for (i = 0; i < g_pk_ncaps; i++) {
        if (strcmp(g_pk_caps[i].name, name) == 0) {
            if (v < 0) {
                for (k = i; k + 1 < g_pk_ncaps; k++) {
                    g_pk_caps[k] = g_pk_caps[k + 1];
                }
                g_pk_ncaps--;
            } else {
                g_pk_caps[i].cap = v;
            }
            return;
        }
    }
    if (v >= 0 && g_pk_ncaps < PK_MAXCAP) {
        strncpy_s(g_pk_caps[g_pk_ncaps].name, sizeof g_pk_caps[g_pk_ncaps].name,
                  name, _TRUNCATE);
        g_pk_caps[g_pk_ncaps].cap = v;
        g_pk_ncaps++;
    }
}

static void packs_cap_set(int c, int v)
{
    int lim = v >= packs_level_num() ? -1 : v;
    packs_cap_name(kSbPackCat[c], lim);
    if (c == 5) {
        packs_cap_name("effect", lim);
    }
}

/* Pruefcode -> Klartext (Konzept 6.6); Code bleibt fuer die Diagnose. */
static const char *packs_reason_text(const char *code)
{
    static const struct {
        const char *code;
        const char *key;
    } kMap[] = {
        { "game_mismatch", "gui.pkv_game" },
        { "bad_variant_path", "gui.pkv_files" }, { "deny_suffix", "gui.pkv_files" },
        { "zip_encrypted", "gui.pkv_files" },
        { "file_missing", "gui.pkv_missing" }, { "file_unreadable", "gui.pkv_missing" },
        { "entries_file_missing", "gui.pkv_missing" },
        { "missing_manifest", "gui.pkv_missing" },
        { "pkg_unreadable", "gui.pkv_unreadable" },
        { "aspect_mismatch", "gui.pkv_shape" },
        { "sha_mismatch", "gui.pkv_sha" },
        { "dds_bad_header", "gui.pkv_dds" }, { "fmt_mismatch", "gui.pkv_dds" },
        { "size_mismatch", "gui.pkv_dds" }, { "dds_not_stored", "gui.pkv_dds" },
        { "dup_hash", "gui.pkv_dup" },
        { "size_not_factor", "gui.pkv_minor" }, { "mips_mismatch", "gui.pkv_minor" },
        { "unknown_category", "gui.pkv_minor" },
        { "unknown_param", "gui.pkv_fx" }, { "params_ignored", "gui.pkv_fx" },
        { "param_range", "gui.pkv_fx" }, { "params_bad_value", "gui.pkv_fx" },
        { "params_not_int", "gui.pkv_fx" },
    };
    int i;
    if (!code || !code[0]) {
        return "";
    }
    for (i = 0; i < (int)(sizeof kMap / sizeof kMap[0]); i++) {
        if (strcmp(code, kMap[i].code) == 0) {
            return LT(kMap[i].key);
        }
    }
    return LT("gui.pkv_manifest"); /* uebrige V1-Codes: Beschreibung ungueltig */
}

/* Statuswort, Farbe und Rohgrund eines Eintrags. */
static void packs_state(int i, const char **word, const char **reason, COLORREF *col)
{
    int s = packs_status_of(i);
    COLORREF c;
    if (!g_pk_ord[i].enabled) {
        *word = LT("gui.pk_off");
        c = C_FAINT;
    } else if (s == 2) {
        *word = LT("gui.pk_ok");
        c = C_OK;
    } else if (s == 1) {
        *word = LT("gui.pk_warn");
        c = C_WARN;
    } else if (s == 0) {
        *word = LT("gui.pk_err");
        c = C_ERR;
    } else {
        *word = LT("gui.pk_unchecked");
        c = C_DIM;
    }
    if (col) {
        *col = c;
    }
    *reason = g_pk_chk[i] >= -1 ? g_pk_chk_reason[i] : g_pk_scan[i].reason;
}

/* Pruefstand ueber das Neuladen retten (per Quelle zugeordnet). */
typedef struct {
    char file[600];
    int chk, xbe;
    char reason[64], detail[300];
} pk_stash_t;

static void packs_load(void)
{
    static pk_stash_t old[PK_MAXN];
    int nold, i;
    char pj[1100];
    nold = g_pk_n;
    for (i = 0; i < nold && i < PK_MAXN; i++) {
        strcpy_s(old[i].file, sizeof old[i].file, g_pk_ord[i].file);
        old[i].chk = g_pk_chk[i];
        old[i].xbe = g_pk_chk_xbe[i];
        strcpy_s(old[i].reason, sizeof old[i].reason, g_pk_chk_reason[i]);
        strcpy_s(old[i].detail, sizeof old[i].detail, g_pk_chk_detail[i]);
    }
    packs_json_path(g_pk_pj, sizeof g_pk_pj);
    pk_acp(g_pk_pj, pj, sizeof pj);
    memset(g_pk_caps, 0, sizeof g_pk_caps);
    memset(g_pk_overs, 0, sizeof g_pk_overs);
    g_pk_n = sb_pack_read_cfg(pj, g_pk_ord, PK_MAXN, g_pk_level,
                              sizeof g_pk_level, g_pk_caps, PK_MAXCAP,
                              g_pk_overs, PK_MAXOVER);
    if (g_pk_n < 0) {
        g_pk_n = 0;
        g_pk_have = 0;
        strcpy_s(g_pk_level, sizeof g_pk_level, "x4");
    } else {
        g_pk_have = 1;
        if (g_pk_n > PK_MAXN) {
            g_pk_n = PK_MAXN;
        }
    }
    g_pk_ncaps = 0;
    for (i = 0; i < PK_MAXCAP && g_pk_caps[i].name[0]; i++) {
        g_pk_ncaps++;
    }
    g_pk_novers = 0;
    for (i = 0; i < PK_MAXOVER && g_pk_overs[i].hash[0]; i++) {
        g_pk_novers++;
    }
    for (i = 0; i < g_pk_n; i++) {
        int k;
        g_pk_chk[i] = -2;
        g_pk_chk_xbe[i] = 0;
        g_pk_chk_reason[i][0] = '\0';
        g_pk_chk_detail[i][0] = '\0';
        for (k = 0; k < nold; k++) { /* Pruefstand je Quelle retten */
            if (strcmp(g_pk_ord[i].file, old[k].file) == 0) {
                g_pk_chk[i] = old[k].chk;
                g_pk_chk_xbe[i] = old[k].xbe;
                strcpy_s(g_pk_chk_reason[i], 64, old[k].reason);
                strcpy_s(g_pk_chk_detail[i], 300, old[k].detail);
                break;
            }
        }
    }
    memset(g_pk_scan, 0, sizeof g_pk_scan);
    sb_pack_scan(pj, g_pk_scan, g_pk_n);
    sb_pack_shadow(pj, g_pk_ne, g_pk_sh, g_pk_n);
    /* W172: Abdeckung je Kategorie + "verdeckt durch" (gleiche Aufloesung
     * wie der Lader) und die gewinnenden Effektwerte. */
    memset(g_pk_cov, 0, sizeof g_pk_cov);
    if (sb_pack_coverage(pj, g_pk_cov, g_pk_covby, PK_MAXN) < 0) {
        memset(g_pk_covby, 0, sizeof g_pk_covby);
    }
    memset(g_pk_fx_hit, 0, sizeof g_pk_fx_hit);
    memset(g_pk_fx_src, 0, sizeof g_pk_fx_src);
    if (g_pk_n > 0) {
        sb_pack_fxparams(pj, g_pk_fx_val, g_pk_fx_hit, g_pk_fx_src);
    }
    if (g_pk_sel >= g_pk_n) {
        g_pk_sel = g_pk_n - 1;
    }
    if (g_pk_sel < 0 && g_pk_n > 0) {
        g_pk_sel = 0;
    }
}

static void packs_save(void)
{
    char dir[1100], pj[1100];
    packs_dir_of(dir, sizeof dir);
    if (dir[0]) {
        lcfg_mkdir_p(dir);
    }
    pk_acp(g_pk_pj, pj, sizeof pj);
    if (sb_pack_write_cfg(pj, g_pk_ord, g_pk_n, g_pk_level, g_pk_caps,
                          g_pk_ncaps, g_pk_overs, g_pk_novers)) {
        g_pk_have = 1;
    }
}

/* Varianten eines geladenen Packs: Bit 1 = 2x, Bit 2 = 4x. */
static int packs_variants(int i)
{
    int c, m = 0;
    for (c = 0; c < SB_PACK_NCAT; c++) {
        if (g_pk_cov[i].cat2[c] > 0) {
            m |= 1;
        }
        if (g_pk_cov[i].cat4[c] > 0) {
            m |= 2;
        }
    }
    return m;
}

/* Effekt-Pack setzt Farben (color_*, bloom) oder nur Effekte? */
static int packs_fx_colors(int i)
{
    return strstr(g_pk_scan[i].paramsum, "color_") != NULL ||
           strstr(g_pk_scan[i].paramsum, "bloom_") != NULL;
}

/* Zweite Zeile einer Listenzeile: Version, Abdeckung, verdeckt. */
static void packs_row2(int i, char *out, size_t cap)
{
    int nt = g_pk_ne[i] >= 0 ? g_pk_ne[i] : g_pk_scan[i].nentries;
    size_t n;
    if (nt < 0) {
        nt = 0;
    }
    _snprintf_s(out, cap, _TRUNCATE, "%s", g_pk_scan[i].version);
    n = strlen(out);
    if (packs_is_fx(i)) {
        _snprintf_s(out + n, cap - n, _TRUNCATE, " \xC2\xB7 %s",
                    LT(packs_fx_colors(i) ? "gui.pk_cov_colfx" : "gui.pk_cov_fx"));
        n = strlen(out);
        if (nt > 0) {
            _snprintf_s(out + n, cap - n, _TRUNCATE, LT("gui.pk_cov_plustex"), nt);
        }
        return;
    }
    _snprintf_s(out + n, cap - n, _TRUNCATE, LT("gui.pk_cov_tex"), nt);
    n = strlen(out);
    {
        int v = g_pk_ord[i].enabled ? packs_variants(i) : 0;
        if (v) {
            _snprintf_s(out + n, cap - n, _TRUNCATE, " \xC2\xB7 %s",
                        v == 3 ? "2\xC3\x97/4\xC3\x97" : v == 2 ? "4\xC3\x97" : "2\xC3\x97");
            n = strlen(out);
        }
    }
    if (g_pk_sh[i] > 0) {
        _snprintf_s(out + n, cap - n, _TRUNCATE, LT("gui.pk_cov_covered"), g_pk_sh[i]);
    }
}

/* Listenzeile (eigene Zeichnung): Schalter, Typ T/E, Name + Zeile 2,
 * Status rechts. Ziehen: Ziel-Linie ueber der Zeile g_pk_drop. */
static void packs_draw_row(const DRAWITEMSTRUCT *d)
{
    canvas_t c;
    int i = (int)d->itemID, w = d->rcItem.right - d->rcItem.left,
        h = d->rcItem.bottom - d->rcItem.top;
    int sel = (d->itemState & ODS_SELECTED) != 0;
    int focus = (d->itemState & ODS_FOCUS) != 0;
    const char *word, *reason;
    COLORREF scol;
    char row2[300];
    memset(&c, 0, sizeof c);
    if (i < 0 || i >= g_pk_n || w <= 0 || h <= 0 || !cv_make(&c, w, h)) {
        return;
    }
    GdiFlush();
    cv_fill(&c, 0, 0, w, h, C_INPUT, 255, 0);
    if (sel) {
        cv_fill(&c, 0, 0, w, h, RGB(255, 255, 255), 18, SX(5));
        cv_fill(&c, 0, SX(6), SX(3), h - SX(12), C_ACCENT, 255, SX(1));
    }
    packs_state(i, &word, &reason, &scol);
    { /* Schalter (wie K_TOGGLE): abgelehnt + aus = blass */
        int tw = SX(30), th = SX(16), ty = (h - th) / 2, x0 = SX(10);
        int on = g_pk_ord[i].enabled, rej = packs_status_of(i) == 0;
        COLORREF track = on ? C_ACCENT : RGB(70, 77, 90);
        int kd = th - SX(6), kx = on ? x0 + tw - SX(3) - kd : x0 + SX(3);
        cv_fill(&c, x0, ty, tw, th, track, rej && !on ? 110 : 255, th / 2);
        cv_fill(&c, kx, ty + SX(3), kd, kd, on ? C_ON_ACCENT : RGB(230, 233, 238),
                rej && !on ? 140 : 255, kd / 2);
    }
    { /* Typ-Symbol T/E */
        int bx = SX(48), by = (h - SX(18)) / 2, fx = packs_is_fx(i);
        cv_fill(&c, bx, by, SX(18), SX(18), fx ? RGB(120, 92, 200) : RGB(44, 92, 128),
                255, SX(3));
        cv_text(&c, g_f[F_TINY], fx ? "E" : "T", bx, by, SX(18), SX(18), C_TEXT,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    {
        int nx = SX(74), sw = SX(92), nw = w - nx - sw - SX(6);
        cv_text(&c, g_f[F_SMALL], g_pk_scan[i].name, nx, SX(3), nw, SX(18),
                g_pk_ord[i].enabled ? C_TEXT : C_DIM,
                DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        packs_row2(i, row2, sizeof row2);
        cv_text(&c, g_f[F_TINY], row2, nx, SX(20), nw, SX(15), C_DIM,
                DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        cv_fill(&c, w - sw, (h - SX(8)) / 2, SX(8), SX(8), scol, 255, SX(4));
        cv_text(&c, g_f[F_TINY], word, w - sw + SX(12), 0, sw - SX(14), h, scol,
                DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }
    if (g_pk_dragging && g_pk_drop >= 0) { /* Ziel-Linie */
        if (g_pk_drop == i) {
            cv_fill(&c, 0, 0, w, SX(2), C_ACCENT_HI, 255, 0);
        } else if (g_pk_drop == g_pk_n && i == g_pk_n - 1) {
            cv_fill(&c, 0, h - SX(2), w, SX(2), C_ACCENT_HI, 255, 0);
        }
    }
    if (focus && !(d->itemState & ODS_NOFOCUSRECT)) {
        cv_ring(&c, 0, 0, w, h, C_ACCENT, 160, SX(5), 1);
    }
    cv_fill(&c, SX(8), h - 1, w - SX(16), 1, RGB(255, 255, 255), 12, 0);
    GdiFlush();
    BitBlt(d->hDC, d->rcItem.left, d->rcItem.top, w, h, c.dc, 0, 0, SRCCOPY);
    cv_free(&c);
}

static void packs_refresh_lists(void)
{
    HWND lb = ctl(ID_PK_LIST);
    int i, c, lv = packs_level_num();
    g_updating++;
    if (lb) {
        SendMessageW(lb, WM_SETREDRAW, FALSE, 0);
        ListBox_ResetContent(lb);
        for (i = 0; i < g_pk_n; i++) {
            SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)i); /* Daten = Index */
        }
        ListBox_SetCurSel(lb, g_pk_sel);
        SendMessageW(lb, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(lb, NULL, FALSE);
    }
    /* Segment Texturdetail (alte Stufe "orig": keines gewaehlt) */
    ui_set_checked(ID_PK_LVL2, lv == 2);
    ui_set_checked(ID_PK_LVL4, lv == 4);
    /* Grenzen je Kategorie: nur Werte <= Texturdetail in der Liste */
    for (c = 0; c < SB_PACK_NCAT; c++) {
        HWND cb = ctl(ID_PK_CAT0 + c);
        int cap = packs_cap_of(c);
        if (!cb) {
            continue;
        }
        ComboBox_ResetContent(cb);
        combo_add(cb, LT("gui.pk_orig"));
        if (lv >= 2) {
            combo_add(cb, LT("gui.pk_x2"));
        }
        if (lv >= 4) {
            combo_add(cb, LT("gui.pk_x4"));
        }
        ComboBox_SetCurSel(cb, cap <= 0 ? 0 : cap <= 2 ? 1 : 2);
    }
    g_updating--;
}

static void packs_refresh(int load)
{
    if (load) {
        packs_load();
    }
    packs_refresh_lists();
    ui_apply_rules(); /* W171: R17-R22/R29-R32 haengen am Packs-Zustand */
    redraw_all();
}

/* Auswahl (Liste) -> g_pk_sel. */
static int packs_sel(void)
{
    HWND lb = ctl(ID_PK_LIST);
    int sel = lb ? (int)SendMessageW(lb, LB_GETCURSEL, 0, 0) : -1;
    if (sel >= 0 && sel < g_pk_n) {
        g_pk_sel = sel;
    } else if (g_pk_n == 0) {
        g_pk_sel = -1;
    }
    return g_pk_sel;
}

/* id eines Pakets per Scan bestimmen (nach dem Hinzufuegen). */
static void packs_fix_id(int idx)
{
    char pj[1100];
    static sb_pack_scan_t s[PK_MAXN];
    int n, i;
    pk_acp(g_pk_pj, pj, sizeof pj);
    n = sb_pack_scan(pj, s, PK_MAXN);
    for (i = 0; i < n && i < PK_MAXN; i++) {
        if (strcmp(s[i].src, g_pk_ord[idx].file) == 0 && s[i].id[0]) {
            strncpy_s(g_pk_ord[idx].id, sizeof g_pk_ord[idx].id, s[i].id,
                      _TRUNCATE);
            return;
        }
    }
}

static const char *packs_xbe(char *hex, size_t cap);
static void packs_check_idx(int i, int quiet);

/* W172: Pack hinzufuegen (Dialog, Ziehen & Ablegen, Selbsttest).
 * Datei (.sbpack) wird in den Packs-Ordner kopiert, Ordner verknuepft.
 * Danach sofort pruefen (Konzept 6.6). 0 = ok. */
static int packs_add_path(const char *src_u8, int is_dir)
{
    char u8[2048], dir[1100], base[600], dest[1400], *s;
    int i;
    if (g_pk_n >= PK_MAXN) {
        set_status_k(LT("gui.rule_r31"), 2);
        return 1;
    }
    strncpy_s(u8, sizeof u8, src_u8, _TRUNCATE);
    for (s = u8; *s; s++) {
        if (*s == '\\') {
            *s = '/';
        }
    }
    s = strrchr(u8, '/');
    strncpy_s(base, sizeof base, s ? s + 1 : u8, _TRUNCATE);
    for (i = 0; i < g_pk_n; i++) {
        if (_stricmp(g_pk_ord[i].file, is_dir ? u8 : base) == 0) {
            g_pk_sel = i;
            set_status_k(LT("gui.pk_dup"), 2);
            packs_refresh(0);
            return 1;
        }
    }
    if (!is_dir) {
        wchar_t wsrc[MAX_PATH * 2], wdst[MAX_PATH * 2];
        packs_dir_of(dir, sizeof dir);
        if (dir[0]) {
            lcfg_mkdir_p(dir);
        }
        _snprintf_s(dest, sizeof dest, _TRUNCATE, "%s/%s", dir, base);
        u8_to_w(u8, wsrc, MAX_PATH * 2);
        u8_to_w(dest, wdst, MAX_PATH * 2);
        if (!path_eq(u8, dest) && !CopyFileW(wsrc, wdst, FALSE)) {
            set_status_k(LT("gui.pk_nocopy"), 2);
            return 1;
        }
        strncpy_s(g_pk_ord[g_pk_n].file, sizeof g_pk_ord[g_pk_n].file, base,
                  _TRUNCATE);
    } else {
        strncpy_s(g_pk_ord[g_pk_n].file, sizeof g_pk_ord[g_pk_n].file, u8,
                  _TRUNCATE);
    }
    strcpy_s(g_pk_ord[g_pk_n].id, sizeof g_pk_ord[g_pk_n].id, "?");
    g_pk_ord[g_pk_n].enabled = 1;
    g_pk_n++;
    packs_save();
    packs_fix_id(g_pk_n - 1);
    packs_save();
    g_pk_sel = g_pk_n - 1;
    packs_load();
    packs_check_idx(g_pk_sel, 1); /* sofort pruefen, Ergebnis in der Liste */
    {
        const char *w, *r;
        char t[400];
        packs_state(g_pk_sel, &w, &r, NULL);
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.pk_added"),
                    g_pk_scan[g_pk_sel].name, w);
        set_status_k(t, packs_status_of(g_pk_sel) >= 1 ? 1 : 2);
    }
    packs_refresh(0);
    return 0;
}

static void packs_add_file(void)
{
    OPENFILENAMEW ofn;
    static wchar_t file[MAX_PATH * 2];
    wchar_t filter[256], f1[128], f2[64], title[128], *w;
    size_t n1, n2;
    char u8[2048];
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g_hwnd;
    file[0] = 0;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH * 2;
    u8_to_w(LT("gui.pk_dlg_filter"), f1, 128);
    u8_to_w(LT("gui.dlg_allfiles"), f2, 64);
    u8_to_w(LT("gui.pk_dlg_file"), title, 128);
    w = filter;
    n1 = wcslen(f1) + 1;
    n2 = wcslen(f2) + 1;
    if (n1 + 10 + n2 + 4 < 256) {
        wcscpy_s(w, 256, f1);
        w += n1;
        wcscpy_s(w, 256 - n1, L"*.sbpack");
        w += 9;
        wcscpy_s(w, 256 - n1 - 9, f2);
        w += n2;
        wcscpy_s(w, 256 - n1 - 9 - n2, L"*.*");
        w[4] = 0;
    } else {
        wcscpy_s(filter, 256, L"*.sbpack");
    }
    ofn.lpstrFilter = filter;
    ofn.lpstrTitle = title;
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) {
        return;
    }
    w_to_u8(file, u8, sizeof u8);
    packs_add_path(u8, 0);
}

static void packs_add_dir(void)
{
    BROWSEINFOW bi;
    LPITEMIDLIST pidl;
    static wchar_t dir[MAX_PATH];
    char u8[2048];
    memset(&bi, 0, sizeof bi);
    bi.hwndOwner = g_hwnd;
    {
        static wchar_t title[160];
        u8_to_w(LT("gui.pk_dlg_dir"), title, 160);
        bi.lpszTitle = title;
    }
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    pidl = SHBrowseForFolderW(&bi);
    if (!pidl) {
        return;
    }
    dir[0] = 0;
    if (SHGetPathFromIDListW(pidl, dir)) {
        w_to_u8(dir, u8, sizeof u8);
        packs_add_path(u8, 1);
    }
    CoTaskMemFree(pidl);
}

static void packs_add_menu(void)
{
    HMENU m = CreatePopupMenu();
    RECT r;
    wchar_t w1[128], w2[128];
    int cmd;
    if (!m) {
        return;
    }
    u8_to_w(LT("gui.pk_add_file"), w1, 128);
    u8_to_w(LT("gui.pk_add_dir"), w2, 128);
    AppendMenuW(m, MF_STRING, 1, w1);
    AppendMenuW(m, MF_STRING, 2, w2);
    GetWindowRect(ctl(ID_PK_ADD), &r);
    cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                              r.left, r.bottom, 0, g_hwnd, NULL);
    DestroyMenu(m);
    if (cmd == 1) {
        packs_add_file();
    } else if (cmd == 2) {
        packs_add_dir();
    }
}

/* W172: Ziehen & Ablegen aus dem Explorer (.sbpack-Datei oder Pack-Ordner
 * mit manifest.json). */
static void packs_drop(HDROP hd)
{
    UINT n = DragQueryFileW(hd, 0xFFFFFFFF, NULL, 0), k;
    int added = 0, bad = 0;
    for (k = 0; k < n; k++) {
        wchar_t w[MAX_PATH * 2];
        char u8[2048], mf[2100];
        size_t L;
        if (!DragQueryFileW(hd, k, w, MAX_PATH * 2)) {
            continue;
        }
        w_to_u8(w, u8, sizeof u8);
        L = strlen(u8);
        _snprintf_s(mf, sizeof mf, _TRUNCATE, "%s\\manifest.json", u8);
        if (is_dir_u8(u8) && u8_GetFileAttributes(mf) != INVALID_FILE_ATTRIBUTES) {
            added += packs_add_path(u8, 1) == 0;
        } else if (L > 7 && _stricmp(u8 + L - 7, ".sbpack") == 0) {
            added += packs_add_path(u8, 0) == 0;
        } else {
            bad++;
        }
    }
    DragFinish(hd);
    if (g_page != P_PACKS) {
        show_page(P_PACKS);
    }
    if (bad && !added) {
        set_status_k(LT("gui.pk_drop_bad"), 2);
    }
}

/* W172: Entfernen mit Rueckfrage. Datei-Pack: kopierte Datei loeschen;
 * Ordner-Pack: nur die Verknuepfung, Dateien bleiben (Konzept 6.5). */
static void packs_del(void)
{
    int i = packs_sel(), k, is_file;
    char q[600], full[1400], dir[1100];
    if (i < 0 || i >= g_pk_n) {
        set_status(LT("gui.pk_nosel"));
        return;
    }
    packs_dir_of(dir, sizeof dir);
    is_file = !((strlen(g_pk_ord[i].file) > 1 && g_pk_ord[i].file[1] == ':') ||
                g_pk_ord[i].file[0] == '/' || g_pk_ord[i].file[0] == '\\');
    _snprintf_s(full, sizeof full, _TRUNCATE, "%s/%s", dir, g_pk_ord[i].file);
    if (is_file && is_dir_u8(full)) {
        is_file = 0; /* Ordner im Packs-Ordner: nicht loeschen */
    }
    _snprintf_s(q, sizeof q, _TRUNCATE,
                LT(is_file ? "gui.pk_ask_delfile" : "gui.pk_ask_unlink"),
                g_pk_scan[i].name);
    if (!ui_confirm(q, LT("gui.pk_del"))) {
        return;
    }
    if (is_file) {
        u8_DeleteFile(full);
    }
    for (k = i; k + 1 < g_pk_n; k++) {
        g_pk_ord[k] = g_pk_ord[k + 1];
        g_pk_chk[k] = g_pk_chk[k + 1];
        g_pk_chk_xbe[k] = g_pk_chk_xbe[k + 1];
        strcpy_s(g_pk_chk_reason[k], 64, g_pk_chk_reason[k + 1]);
        strcpy_s(g_pk_chk_detail[k], 300, g_pk_chk_detail[k + 1]);
    }
    g_pk_n--;
    packs_save();
    packs_refresh(1);
}

/* W172: Eintrag from an Stelle to (0..n, to = vor diesem Eintrag). */
static void packs_move_to(int from, int to)
{
    sb_pack_order_t t;
    int k;
    if (from < 0 || from >= g_pk_n || to < 0 || to > g_pk_n) {
        return;
    }
    if (to > from) {
        to--; /* Einfuegestelle nach dem Herausnehmen */
    }
    if (to == from) {
        return;
    }
    t = g_pk_ord[from];
    if (to < from) {
        for (k = from; k > to; k--) {
            g_pk_ord[k] = g_pk_ord[k - 1];
        }
    } else {
        for (k = from; k < to; k++) {
            g_pk_ord[k] = g_pk_ord[k + 1];
        }
    }
    g_pk_ord[to] = t;
    g_pk_sel = to;
    packs_save();
    packs_refresh(1); /* Konflikte sofort neu (Konzept 6.3) */
    {
        char m[300];
        _snprintf_s(m, sizeof m, _TRUNCATE, LT("gui.pk_moved"),
                    g_pk_scan[to].name, to + 1, g_pk_n);
        set_status(m);
    }
}

static void packs_move(int dir)
{
    int i = packs_sel();
    if (i < 0 || i >= g_pk_n) {
        set_status(LT("gui.pk_nosel"));
        return;
    }
    if (i + dir < 0 || i + dir >= g_pk_n) {
        return;
    }
    packs_move_to(i, dir < 0 ? i - 1 : i + 2);
}

/* An/aus (R21: abgelehnt laesst sich nicht einschalten, aus geht immer). */
static void packs_toggle_idx(int i)
{
    if (i < 0 || i >= g_pk_n) {
        set_status(LT("gui.pk_nosel"));
        return;
    }
    if (!g_pk_ord[i].enabled && packs_status_of(i) == 0) {
        char t[400];
        const char *w, *r;
        packs_state(i, &w, &r, NULL);
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.rule_r21"), packs_reason_text(r));
        set_status_k(t, 2);
        return;
    }
    g_pk_sel = i;
    g_pk_ord[i].enabled = !g_pk_ord[i].enabled;
    packs_save();
    packs_refresh(1);
}

static void packs_toggle(void)
{
    packs_toggle_idx(packs_sel());
}

/* XBE-Hash fuer V2 (NULL = unbekannt, V2 entfaellt). */
static const char *packs_xbe(char *hex, size_t cap)
{
    launcher_settings_t s;
    uint8_t dg[32];
    char xp[1024], full[1200], eff[1024];
    s = g_s;
    ui_to_settings(&s);
    if (s.game_image[0]) { /* W150 ISO-DIRECT: default.xbe aus dem Abbild */
        return lcfg_image_xbe_sha256(s.game_image, hex) ? hex : NULL;
    }
    lcfg_xbe_effective(&s, eff, sizeof eff); /* W172: auch abgeleitet */
    if (!eff[0]) {
        return NULL;
    }
    if ((strlen(eff) > 1 && eff[1] == ':') || eff[0] == '/' || eff[0] == '\\') {
        strncpy_s(full, sizeof full, eff, _TRUNCATE);
    } else {
        _snprintf_s(full, sizeof full, _TRUNCATE, "%s/%s", g_client_dir, eff);
    }
    pk_acp(full, xp, sizeof xp);
    if (!sb_sha256_file(xp, dg)) {
        return NULL;
    }
    sb_sha256_hex(dg, hex);
    (void)cap;
    return hex;
}

/* Vollpruefung V1-V14 eines Eintrags (quiet = keine Statuszeile). */
static void packs_check_idx(int i, int quiet)
{
    char full[1400], afull[1400], dir[1100];
    char xhex[65], reason[64], detail[300];
    const char *xh;
    int st;
    if (i < 0 || i >= g_pk_n) {
        if (!quiet) {
            set_status(LT("gui.pk_nosel"));
        }
        return;
    }
    packs_dir_of(dir, sizeof dir);
    if ((strlen(g_pk_ord[i].file) > 1 && g_pk_ord[i].file[1] == ':') ||
        g_pk_ord[i].file[0] == '/' || g_pk_ord[i].file[0] == '\\') {
        strncpy_s(full, sizeof full, g_pk_ord[i].file, _TRUNCATE);
    } else {
        _snprintf_s(full, sizeof full, _TRUNCATE, "%s/%s", dir,
                    g_pk_ord[i].file);
    }
    pk_acp(full, afull, sizeof afull);
    xh = packs_xbe(xhex, sizeof xhex);
    reason[0] = detail[0] = '\0';
    st = sb_pack_check(afull, xh, reason, sizeof reason, detail,
                       sizeof detail);
    g_pk_chk[i] = st;
    g_pk_chk_xbe[i] = xh != NULL;
    strcpy_s(g_pk_chk_reason[i], sizeof g_pk_chk_reason[i], reason);
    strcpy_s(g_pk_chk_detail[i], sizeof g_pk_chk_detail[i], detail);
    if (!quiet) {
        char t[400];
        const char *w, *r;
        packs_state(i, &w, &r, NULL);
        if (reason[0]) {
            _snprintf_s(t, sizeof t, _TRUNCATE, "%s: %s \xE2\x80\x93 %s",
                        g_pk_scan[i].name, w, packs_reason_text(reason));
        } else {
            _snprintf_s(t, sizeof t, _TRUNCATE, "%s: %s", g_pk_scan[i].name, w);
        }
        if (!xh) {
            size_t n = strlen(t);
            _snprintf_s(t + n, sizeof t - n, _TRUNCATE, " \xC2\xB7 %s", LT("gui.rule_r22"));
        }
        set_status_k(t, st == 0 ? 2 : 1);
    }
}

static void packs_check(void)
{
    packs_check_idx(packs_sel(), 0);
    packs_refresh_lists();
    ui_apply_rules();
    redraw_all();
}

/* W172: alle Packs pruefen (Konzept 6.6 "Check all"). */
static void packs_check_all(void)
{
    int i, ok = 0, warn = 0, bad = 0;
    char t[300];
    for (i = 0; i < g_pk_n; i++) {
        int st;
        packs_check_idx(i, 1);
        st = packs_status_of(i);
        ok += st == 2;
        warn += st == 1;
        bad += st == 0;
    }
    _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.pk_checked_all"), ok, warn, bad);
    set_status_k(t, bad ? 2 : 1);
    packs_refresh_lists();
    ui_apply_rules();
    redraw_all();
}

static void packs_folder(void)
{
    char dir[1100];
    wchar_t w[1100];
    packs_dir_of(dir, sizeof dir);
    if (dir[0]) {
        lcfg_mkdir_p(dir);
    }
    if (g_o.shot != 0) { /* Test/Shot: nie ein Explorer-Fenster */
        printf("[open] %s\n", dir);
        return;
    }
    u8_to_w(dir[0] ? dir : ".", w, 1100);
    ShellExecuteW(g_hwnd, L"open", w, NULL, NULL, SW_SHOWNORMAL);
}

/* W172: Pack Creator starten (nur wenn neben dem Launcher, PO-Entsch. 6). */
static void packs_creator_open(void)
{
    char p[1200];
    wchar_t w[1200];
    if (!creator_path(p, sizeof p)) {
        set_status_k(LT("gui.rule_r32"), 2);
        return;
    }
    if (g_o.shot != 0) {
        printf("[open] %s\n", p);
        return;
    }
    u8_to_w(p, w, 1200);
    ShellExecuteW(g_hwnd, L"open", w, NULL, NULL, SW_SHOWNORMAL);
}

/* W172: Texturdetail 2x/4x (einzige Stelle, Konzept 6.4). Wirkt im Client
 * ueber hd_scale/hd_textures (wie bisher gespiegelt); Grenzen ueber dem
 * neuen Detail entfallen (nur senken). */
static void packs_set_level(int lv)
{
    if (lv != 2 && lv != 4) {
        return;
    }
    strcpy_s(g_pk_level, sizeof g_pk_level, lv == 2 ? "x2" : "x4");
    {
        int c;
        for (c = 0; c < SB_PACK_NCAT; c++) {
            int cap = packs_cap_stored(c);
            if (cap >= lv) {
                packs_cap_set(c, lv); /* entfernt die Grenze */
            }
        }
    }
    packs_save();
    /* W173: nur die Stufe (hd_scale); HD-Texturen an/aus gehoert dem
     * Grafik-Preset (Konzept 8.2: hd_textures = Preset != Original). */
    g_updating++;
    ComboBox_SetCurSel(ctl(ID_REM_SCALE), lv == 2 ? 0 : 1);
    g_updating--;
    ui_to_settings(&g_s);
    update_summary();
    packs_refresh(0);
}

static void packs_cap_reset(void)
{
    g_pk_ncaps = 0;
    packs_save();
    packs_refresh(0);
    set_status(LT("gui.pk_caps_reset"));
}

/* W173 LAUNCHER-P4: Preset-Schluessel, die aktive Effect Packs setzen
 * (Effect-Pack-Ausnahme, Konzept 3.3; Selbsttest: feste Umgebung). */
static lgp_mask_t gfx_lock_now(void)
{
    lgp_mask_t m = 0;
    int k;
    if (g_env_fix) {
        return g_env_fix->gfx_lock;
    }
    for (k = 0; k < SB_FXN; k++) {
        if (g_pk_fx_hit[k]) {
            lcfg_gfx_lock_key(&m, kSbFxParams[k].key);
        }
    }
    return m;
}

/* W171: Packs-Zustand fuer das Regelwerk (R17-R22, R29-R31). */
static void packs_env(lr_env_t *e)
{
    int i;
    e->gfx_lock = gfx_lock_now(); /* W173 */
    e->n_packs = g_pk_n;
    e->tex_packs = 0;
    e->fx_pack[0] = '\0';
    for (i = 0; i < g_pk_n; i++) {
        int st = packs_status_of(i);
        if (!g_pk_ord[i].enabled || st < 1) {
            continue; /* aus oder abgelehnt */
        }
        if (packs_is_fx(i)) {
            if (!e->fx_pack[0]) { /* oberstes aktives Effekt-Pack gewinnt */
                strncpy_s(e->fx_pack, sizeof e->fx_pack,
                          g_pk_scan[i].name[0] ? g_pk_scan[i].name
                                               : g_pk_ord[i].id,
                          _TRUNCATE);
            }
        } else {
            e->tex_packs++;
        }
    }
    e->pk_level = packs_level_num();
    e->pk_sel = (g_pk_sel >= 0 && g_pk_sel < g_pk_n) ? g_pk_sel : -1;
    e->pk_sel_rejected = 0;
    e->pk_sel_enabled = 0;
    e->pk_sel_reason[0] = '\0';
    if (e->pk_sel >= 0) {
        const char *w, *r;
        packs_state(e->pk_sel, &w, &r, NULL);
        e->pk_sel_rejected = packs_status_of(e->pk_sel) == 0;
        e->pk_sel_enabled = g_pk_ord[e->pk_sel].enabled;
        /* W172: Grund im Klartext (Code nur in der Diagnose) */
        strncpy_s(e->pk_sel_reason, sizeof e->pk_sel_reason,
                  packs_reason_text(r), _TRUNCATE);
    }
}

/* W172: Listen-Unterklasse: Klick auf den Schalter = an/aus, Ziehen =
 * Reihenfolge, Leertaste = an/aus, Alt+Pfeil = verschieben. */
static int packs_item_at(HWND lb, int y)
{
    int top = (int)SendMessageW(lb, LB_GETTOPINDEX, 0, 0);
    int ih = (int)SendMessageW(lb, LB_GETITEMHEIGHT, 0, 0);
    int i;
    if (ih <= 0) {
        return -1;
    }
    i = top + y / ih;
    return i;
}

static LRESULT CALLBACK pklist_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp,
                                    UINT_PTR sid, DWORD_PTR ref)
{
    (void)sid;
    (void)ref;
    switch (msg) {
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        int i = packs_item_at(h, y);
        if (i >= 0 && i < g_pk_n && x < SX(44)) {
            SetFocus(h);
            ListBox_SetCurSel(h, i);
            packs_toggle_idx(i);
            return 0;
        }
        g_pk_drag = (i >= 0 && i < g_pk_n) ? i : -1;
        g_pk_drop = -1;
        g_pk_dragging = 0;
        break;
    }
    case WM_MOUSEMOVE:
        if ((wp & MK_LBUTTON) && g_pk_drag >= 0) {
            int y = GET_Y_LPARAM(lp), ih = (int)SendMessageW(h, LB_GETITEMHEIGHT, 0, 0);
            int top = (int)SendMessageW(h, LB_GETTOPINDEX, 0, 0), to;
            if (ih <= 0) {
                break;
            }
            to = top + (y + ih / 2) / ih;
            if (to < 0) {
                to = 0;
            }
            if (to > g_pk_n) {
                to = g_pk_n;
            }
            if (!g_pk_dragging && to != g_pk_drag && to != g_pk_drag + 1) {
                g_pk_dragging = 1;
            }
            if (g_pk_dragging && to != g_pk_drop) {
                g_pk_drop = to;
                InvalidateRect(h, NULL, FALSE);
            }
            if (g_pk_dragging) {
                SetCursor(LoadCursor(NULL, IDC_SIZENS));
                return 0; /* keine Mehrfach-Auswahl durch Ziehen */
            }
        }
        break;
    case WM_LBUTTONUP:
        if (g_pk_dragging) {
            int from = g_pk_drag, to = g_pk_drop;
            g_pk_dragging = 0;
            g_pk_drag = g_pk_drop = -1;
            DefSubclassProc(h, msg, wp, lp);
            packs_move_to(from, to);
            return 0;
        }
        g_pk_drag = -1;
        break;
    case WM_CAPTURECHANGED:
        if (g_pk_dragging) {
            g_pk_dragging = 0;
            g_pk_drag = g_pk_drop = -1;
            InvalidateRect(h, NULL, FALSE);
        }
        break;
    case WM_KEYDOWN:
        if (wp == VK_SPACE) {
            packs_toggle();
            return 0;
        }
        break;
    case WM_SYSKEYDOWN: /* Alt+Pfeil hoch/runter = verschieben */
        if (wp == VK_UP || wp == VK_DOWN) {
            packs_move(wp == VK_UP ? -1 : 1);
            SetFocus(ctl(ID_PK_LIST));
            return 0;
        }
        break;
    case WM_GETDLGCODE: /* Leertaste/Pfeile selbst behandeln */
        return DefSubclassProc(h, msg, wp, lp) | DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_NCDESTROY:
        RemoveWindowSubclass(h, pklist_proc, 2);
        break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

/* Zeile im Detail: Bezeichnung links, Wert rechts. */
static int pk_line(int font, const char *t, int x, int y, int w, COLORREF c)
{
    label1(font, t, x, y, w, 16, c);
    return y + 16;
}

static void paint_packs(void)
{
    int i = (g_pk_sel >= 0 && g_pk_sel < g_pk_n) ? g_pk_sel : -1;
    int x = PKR_X + 18, w = PKR_W - 36, y, ymax = CY0 + 426;
    char t[700];
    /* --- links oben: installierte Packs --- */
    card(CX0, CY0, PKL_W, 284, LT("gui.pk_list"));
    if (g_pk_n > 0) {
        if (!paint_reason(31, CX0 + 18, CY0 + 38, PKL_W - 36, 16)) {
            label1(F_TINY, LT("gui.pk_list_hint"), CX0 + 18, CY0 + 38, PKL_W - 36,
                   16, C_DIM);
        }
    } else { /* leerer Zustand (Konzept 6.7, R29/R32) */
        if (g_rr[29].active) { /* R29 als Ueberschrift des leeren Zustands */
            label(F_SEMI, g_rr[29].text, CX0 + 18, CY0 + 52, PKL_W - 36, 48, C_TEXT);
        }
        if (!g_rr[32].active) {
            label(F_TINY, LT("gui.pk_creator_txt"), CX0 + 18, CY0 + 152,
                  PKL_W - 36, 30, C_DIM);
        } else {
            paint_reason(32, CX0 + 18, CY0 + 118, PKL_W - 36, 64);
        }
        label(F_TINY, LT("gui.pk_drop_hint"), CX0 + 18, CY0 + 192, PKL_W - 36, 44,
              C_FAINT);
    }
    /* --- links unten: Texturdetail + Grenzen je Kategorie --- */
    card(CX0, CY0 + 296, PKL_W, 200, LT("gui.pk_level"));
    if (!paint_reason(18, CX0 + 18, CY0 + 334, PKL_W - 36, 16)) {
        label1(F_TINY, LT(packs_level_num() == 0 ? "gui.pk_level_orig"
                                                 : "gui.pk_level_hint"),
               CX0 + 18, CY0 + 334, PKL_W - 36, 16,
               packs_level_num() == 0 ? C_WARN : C_DIM);
    }
    if (!paint_reason(19, CX0 + 18, CY0 + 354, PKL_W - 36 - 118, 18)) {
        label1(F_SMALL, LT("gui.pk_caps"), CX0 + 18, CY0 + 354, PKL_W - 36 - 118,
               18, C_DIM);
    }
    {
        int c;
        for (c = 0; c < SB_PACK_NCAT; c++) {
            int cx = CX0 + 18 + (c % 3) * 122, cy = CY0 + 377 + (c / 3) * 37;
            label1(F_TINY, LT(kPkCatKey[c]), cx, cy, 112, 14, C_DIM);
        }
    }
    /* --- rechts: gewaehltes Pack / "Was sind Packs?" --- */
    if (i < 0) {
        card(PKR_X, CY0, PKR_W, 496, LT("gui.pk_what"));
        label(F_SMALL, LT("gui.pk_what_txt"), x, CY0 + 44, w, 300, C_DIM);
        return;
    }
    card(PKR_X, CY0, PKR_W, 496, NULL);
    {
        const char *word, *reason;
        COLORREF scol;
        int fx = packs_is_fx(i);
        packs_state(i, &word, &reason, &scol);
        label1(F_CARD, g_pk_scan[i].name, x, CY0 + 12, w, 24, C_TEXT);
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.pk_head"),
                    LT(fx ? "gui.pk_type_fx" : "gui.pk_type_tex"),
                    g_pk_scan[i].version, g_pk_scan[i].author);
        y = pk_line(F_TINY, t, x, CY0 + 40, w, C_DIM);
        if (strstr(g_pk_scan[i].license, "rivate") ||
            strstr(g_pk_scan[i].license, "not distribute")) {
            y = pk_line(F_TINY, LT("gui.pk_private"), x, y, w, C_WARN);
        } else {
            _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.pk_lic"), g_pk_scan[i].license);
            y = pk_line(F_TINY, t, x, y, w, C_DIM);
        }
        /* Passt zum Spiel (V2) */
        {
            const char *fit;
            COLORREF fc = C_DIM;
            if (g_pk_chk[i] >= 0 && strcmp(g_pk_chk_reason[i], "game_mismatch") == 0) {
                fit = LT("gui.pk_fit_no");
                fc = C_ERR;
            } else if (g_pk_chk[i] >= 1 && g_pk_chk_xbe[i]) {
                fit = LT("gui.pk_fit_yes");
                fc = C_OK;
            } else if (g_rr[22].active) {
                fit = g_rr[22].text;
            } else {
                fit = LT("gui.pk_fit_unk");
            }
            _snprintf_s(t, sizeof t, _TRUNCATE, "%s: %s", LT("gui.pk_fit"), fit);
            y = pk_line(F_TINY, t, x, y, w, fc);
        }
        /* Status im Klartext (Code nur mit offener Diagnose) */
        if (reason && reason[0]) {
            _snprintf_s(t, sizeof t, _TRUNCATE, "%s \xE2\x80\x93 %s", word,
                        packs_reason_text(reason));
        } else {
            strcpy_s(t, sizeof t, word);
        }
        label(F_SMALL, t, x, y + 4, w, 34, scol);
        y += 40;
        if (g_diag_open && reason && reason[0]) {
            _snprintf_s(t, sizeof t, _TRUNCATE, "%s %s", reason,
                        g_pk_chk[i] >= -1 ? g_pk_chk_detail[i] : "");
            y = pk_line(F_TINY, t, x, y, w, C_FAINT);
        }
        if (!g_pk_ord[i].enabled || g_pk_cov[i].cat[0] < 0) {
            label(F_TINY, LT(g_pk_ord[i].enabled ? "gui.pk_cov_unread" : "gui.pk_cov_off"),
                  x, y + 6, w, 32, C_FAINT);
        } else if (fx) {
            /* Effektwerte mit Schloss (Muster D): gelten vor den eigenen
             * Einstellungen; gewinnt ein hoeheres Pack, steht es dabei. */
            int k, any = 0;
            label1(F_SEMI, LT("gui.pk_fx_title"), x, y + 4, w, 20, C_TEXT);
            y += 26;
            for (k = 0; k < SB_FXN && y < ymax - 16; k++) {
                char key[40], *p;
                int v;
                _snprintf_s(key, sizeof key, _TRUNCATE, "%s=", kSbFxParams[k].key);
                p = strstr(g_pk_scan[i].paramsum, key);
                if (!p || (p != g_pk_scan[i].paramsum && p[-1] != ' ')) {
                    continue;
                }
                v = atoi(p + strlen(key));
                any = 1;
                if (k == 4) {
                    _snprintf_s(t, sizeof t, _TRUNCATE, "%s: %s", LT(kPkFxKey[k]),
                                LT(v ? "gui.on" : "gui.off"));
                } else if (k <= 3 || k == 6) {
                    _snprintf_s(t, sizeof t, _TRUNCATE, "%s: %d %%", LT(kPkFxKey[k]), v);
                } else {
                    _snprintf_s(t, sizeof t, _TRUNCATE, "%s: %+d", LT(kPkFxKey[k]), v);
                }
                if (g_pk_fx_hit[k] && g_pk_fx_src[k][0] &&
                    strcmp(g_pk_fx_src[k], g_pk_ord[i].id) != 0) {
                    int wi, n = (int)strlen(t);
                    const char *wn = g_pk_fx_src[k];
                    for (wi = 0; wi < g_pk_n; wi++) {
                        if (strcmp(g_pk_ord[wi].id, g_pk_fx_src[k]) == 0) {
                            wn = g_pk_scan[wi].name;
                        }
                    }
                    _snprintf_s(t + n, sizeof t - n, _TRUNCATE, LT("gui.pk_fx_also"), wn);
                }
                if (g_has_icons) {
                    wchar_t ic = 0xE72E;
                    cv_textw(&g_bb, g_f[F_ICONSM], &ic, 1, SX(x), SX(y), SX(14), SX(16),
                             C_ACCENT_HI, DT_LEFT | DT_TOP | DT_SINGLELINE);
                }
                y = pk_line(F_TINY, t, x + 16, y, w - 16, C_TEXT);
            }
            if (!any) {
                y = pk_line(F_TINY, LT("gui.pk_fx_none"), x, y, w, C_DIM);
            }
            label(F_TINY, LT("gui.pk_fx_over"), x, y + 4, w, 30, C_DIM);
            y += 36;
        } else {
            /* Abdeckung: Kategorie, Anzahl, wirksame Stufe (* = eigene Grenze) */
            int c, lv = packs_level_num(), star = 0;
            label1(F_SEMI, LT("gui.pk_cov_title"), x, y + 4, w, 20, C_TEXT);
            y += 26;
            for (c = 0; c < SB_PACK_NCAT && y < ymax - 16; c++) {
                int n = g_pk_cov[i].cat[c], eff = packs_cap_of(c);
                char num[16];
                if (n <= 0) {
                    continue;
                }
                if (eff >= 4 && g_pk_cov[i].cat4[c] == 0) {
                    eff = g_pk_cov[i].cat2[c] ? 2 : 0; /* faellt zurueck */
                } else if (eff == 2 && g_pk_cov[i].cat2[c] == 0) {
                    eff = 0;
                }
                label1(F_TINY, LT(kPkCatKey[c]), x, y, w - 110, 16, C_DIM);
                _snprintf_s(num, sizeof num, _TRUNCATE, "%d", n);
                L_text(F_TINY, num, x + w - 110, y, 50, 16, C_TEXT,
                       DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
                _snprintf_s(t, sizeof t, _TRUNCATE, "%s%s", packs_cap_str(eff),
                            packs_cap_stored(c) >= 0 && packs_cap_stored(c) < lv ? " *" : "");
                if (packs_cap_stored(c) >= 0 && packs_cap_stored(c) < lv) {
                    star = 1;
                }
                L_text(F_TINY, t, x + w - 50, y, 50, 16, C_TEXT,
                       DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
                y += 16;
            }
            if (star) {
                y = pk_line(F_TINY, LT("gui.pk_cov_star"), x, y, w, C_FAINT);
            }
        }
        /* Konflikte: "verdeckt durch" (hoeher) und "verdeckt selbst" */
        if (g_pk_ord[i].enabled && y < ymax - 36) {
            int k, any = 0;
            label1(F_SEMI, LT("gui.pk_conf_title"), x, y + 6, w, 20, C_TEXT);
            y += 28;
            for (k = 0; k < g_pk_n && y < ymax - 16; k++) {
                int a = g_pk_covby[i * PK_MAXN + k], b = g_pk_covby[k * PK_MAXN + i];
                if (k == i) {
                    continue;
                }
                if (a > 0) {
                    _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.pk_conf_by"), a,
                                g_pk_scan[k].name);
                    label(F_TINY, t, x, y, w, 30, C_WARN);
                    y += 30;
                    any = 1;
                }
                if (b > 0 && y < ymax - 16) {
                    _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.pk_conf_over"), b,
                                g_pk_scan[k].name);
                    label(F_TINY, t, x, y, w, 30, C_DIM);
                    y += 30;
                    any = 1;
                }
            }
            if (!any) {
                pk_line(F_TINY, LT("gui.pk_conf_none"), x, y, w, C_DIM);
            }
        }
    }
    paint_reason(21, x, CY0 + 430, w, 16);
}

static void show_page(int p)
{
    int i;
    if (p < 0 || p >= P_N) {
        p = P_PLAY;
    }
    g_page = p;
    g_setup_force = 0; /* W172: Einrichtungskarte nur, wenn Regel R3 greift */
    for (i = 0; i < g_ui_n; i++) {
        uictl_t *u = &g_ui[i];
        if (u->parent != g_hwnd) {
            continue;
        }
        ShowWindow(u->h, ui_visible_on(u, p) ? SW_SHOWNA : SW_HIDE);
    }
    if (p == P_CTRL) {
        static int once;
        if (!once) {
            once = 1;
            update_sbc_status();
        }
        update_pad_status();
    }
    if (p == P_DISPLAY) {
        update_aspect_info();
    }
    if (p == P_PLAY) {
        update_summary();
        update_vr_button(); /* W159 */
    }
    if (p == P_PACKS) { /* W148 */
        packs_refresh(1);
    }
    if (p == P_ADV) {
        refresh_profiles();
    }
    ui_apply_rules(); /* W171 */
    redraw_all();
}

/* W171: Strg+Tab / Strg+Umschalt+Tab = naechste/vorige Seite (zyklisch). */
static int nav_next(int page, int dir)
{
    return (page + (dir < 0 ? P_N - 1 : 1)) % P_N;
}

/* W142: Fenster-Texte in der aktiven Sprache neu setzen (ohne Neustart).
 * Auswahl und Nutzereingaben bleiben; gemalte Texte folgen per Neuzeichnen. */
static void combo_refill(int id, const char *const *keys, int n)
{
    HWND c = ctl(id);
    int sel, i;
    if (!c) {
        return;
    }
    sel = ComboBox_GetCurSel(c);
    ComboBox_ResetContent(c);
    for (i = 0; i < n; i++) {
        combo_add(c, LT(keys[i]));
    }
    ComboBox_SetCurSel(c, (sel >= 0 && sel < n) ? sel : 0);
}

/* Nur Eintrag 0 ersetzen (Rest sind Nutzerdaten: Profile, Roheintraege). */
static void combo_set0(int id, const char *key)
{
    HWND c = ctl(id);
    wchar_t *w;
    int sel;
    if (!c || ComboBox_GetCount(c) < 1) {
        return;
    }
    sel = ComboBox_GetCurSel(c);
    ComboBox_DeleteString(c, 0);
    w = u8_wdup(LT(key));
    if (w) {
        ComboBox_InsertString(c, 0, w);
        free(w);
    }
    ComboBox_SetCurSel(c, sel);
}

static void ui_apply_language(void)
{
    static const char *const kVsync[] = { "gui.off", "gui.on" };
    static const char *const kWinmode[] = { "gui.wm_win", "gui.wm_borderless", "gui.wm_excl" };
    static const char *const kIp[] = { "gui.ip_30", "gui.ip_60", "gui.ip_120" };
    static const char *const kMonfx[] = { "gui.monfx_orig", "gui.monfx_grain", "gui.monfx_glitch", "gui.off" };
    static const char *const kAa[] = { "gui.aa_off", "gui.aa_final", "gui.aa_world", "gui.aa_both" };
    static const char *const kLight[] = { "gui.gfx_light_on", "gui.gfx_light_off" }; /* W173 */
    static const char *const kOnOff[] = { "gui.off", "gui.on" };
    static const char *const kFxl[] = { "gui.gfx_fx_orig", "gui.gfx_fx_subtle", "gui.gfx_fx_strong" };
    static const char *const kTex[] = { "gui.gfx_tex_o", "gui.gfx_tex_hd" };
    static const char *const kScale[] = { "gui.scale2", "gui.scale4" };
    static const char *const kEdge[] = { "gui.edge_off", "gui.edge_cut", "gui.edge_fade" };
    static const char *const kVr[] = { "gui.vr_prev_sbs", "gui.vr_prev_left", "gui.vr_prev_right", "gui.vr_prev_off" };
    static const char *const kVrPre[] = { "gui.vrp_perf", "gui.vrp_bal", "gui.vrp_qual", "gui.vrp_custom" }; /* W183 */
    static const char *const kMouse[] = { "gui.mouse_abs", "gui.mouse_rel" };
    static const char *const kLang[] = { "gui.lang_en", "gui.lang_de" };
    HWND c;
    int i, sel;
    /* Navigation + Titelleiste + Start */
    for (i = 0; i < P_N; i++) {
        ui_settext(ctl(ID_NAV0 + i), LT(kPageName[i]));
    }
    ui_settext(ctl(ID_MIN), LT("gui.win_min"));
    ui_settext(ctl(ID_CLOSE), LT("gui.btn_close"));
    ui_settext(ctl(ID_START_BIG), LT("gui.btn_bigstart"));
    ui_settext(ctl(ID_START_VR), LT("gui.btn_vrstart")); /* W159 */
    ui_settext(ctl(ID_BG), LT("gui.bg"));
    /* Grafik-Kombis (Reihenfolge wie build_controls) */
    c = ctl(ID_PRESET);
    if (c) {
        sel = ComboBox_GetCurSel(c);
        ComboBox_ResetContent(c);
        for (i = 0; i < KPRESETS_N; i++) {
            combo_add(c, kPresets[i].lkey ? LT(kPresets[i].lkey) : kPresets[i].label);
        }
        ComboBox_SetCurSel(c, (sel >= 0 && sel < KPRESETS_N) ? sel : KPRESETS_N - 1);
    }
    combo_refill(ID_VSYNC, kVsync, 2);
    c = ctl(ID_FPS);
    if (c) { /* 0 = Text, 1..3 = Zahlen */
        static const char *const n[] = { "30", "60", "120" };
        sel = ComboBox_GetCurSel(c);
        ComboBox_ResetContent(c);
        combo_add(c, LT("gui.fps_off"));
        for (i = 0; i < 3; i++) {
            combo_add(c, n[i]);
        }
        ComboBox_SetCurSel(c, (sel >= 0 && sel < 4) ? sel : 0);
    }
    combo_refill(ID_WINMODE, kWinmode, 3);
    combo_set0(ID_FSHZ, "gui.fshz_desktop");
    combo_refill(ID_IPFPS, kIp, 3);
    combo_refill(ID_MONFX, kMonfx, 4);
    combo_refill(ID_AAMODE, kAa, 4);
    combo_refill(ID_REM_SCALE, kScale, 2);
    combo_refill(ID_GFX_LIGHT, kLight, 2); /* W173 */
    combo_refill(ID_GFX_SHADOW, kOnOff, 2);
    combo_refill(ID_GFX_SOFT, kOnOff, 2);
    while (ctl(ID_GFX_FX) && ComboBox_GetCount(ctl(ID_GFX_FX)) > 3) { /* "Benutzerdefiniert" setzt gfx_sync_ui */
        ComboBox_DeleteString(ctl(ID_GFX_FX), ComboBox_GetCount(ctl(ID_GFX_FX)) - 1);
    }
    combo_refill(ID_GFX_FX, kFxl, 3);
    combo_refill(ID_GFX_TEX, kTex, 2);
    ui_settext(ctl(ID_GP_RESET), LT("gui.gfx_reset"));
    ui_settext(ctl(ID_REM_LABEL), LT("gui.rem_label"));
    combo_refill(ID_EDGE, kEdge, 3);
    c = ctl(ID_EDGE_W);
    if (c) { /* Zahlenformate */
        sel = ComboBox_GetCurSel(c);
        ComboBox_ResetContent(c);
        for (i = 0; i < KEDGEW_N; i++) {
            char t[32];
            _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.edge_w_fmt"), kEdgeW[i]);
            combo_add(c, t);
        }
        ComboBox_SetCurSel(c, (sel >= 0 && sel < KEDGEW_N) ? sel : 2);
    }
    c = ctl(ID_EDGE_S);
    if (c) {
        sel = ComboBox_GetCurSel(c);
        ComboBox_ResetContent(c);
        for (i = 0; i < KEDGES_N; i++) {
            char t[32];
            _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.edge_s_fmt"), kEdgeS[i]);
            combo_add(c, t);
        }
        ComboBox_SetCurSel(c, (sel >= 0 && sel < KEDGES_N) ? sel : KEDGES_N - 1);
    }
    combo_refill(ID_VR, kVr, 4);
    combo_refill(ID_VRPRESET, kVrPre, 4); /* W183 */
    c = ctl(ID_RTSCALE); /* W148: 2 Texte + Zahlen 1..4 */
    if (c) {
        sel = ComboBox_GetCurSel(c);
        ComboBox_ResetContent(c);
        combo_add(c, LT("gui.rs_dyn"));
        combo_add(c, LT("gui.rs_autof"));
        combo_add(c, "1");
        combo_add(c, "2");
        combo_add(c, "3");
        combo_add(c, "4");
        ComboBox_SetCurSel(c, (sel >= 0 && sel < 6) ? sel : 0);
    }
    /* Ton */
    ui_settext(ctl(ID_AUDIO), LT("gui.ton_on"));
    ui_settext(ctl(ID_LIVE), LT("gui.ton_live"));
    ui_settext(ctl(ID_DIAG), LT(g_diag_open ? "gui.adv_diag_hide" : "gui.adv_diag_show"));
    ui_settext(ctl(ID_UNDO), LT("gui.undo"));
    /* Steuerung */
    combo_set0(ID_CTRL_PROF, "gui.legacy_label");
    ui_settext(ctl(ID_CTRL_REFRESH), LT("gui.ctl_reload"));
    ui_settext(ctl(ID_CTRL_BIND), LT("gui.ctl_bind"));
    ui_settext(ctl(ID_CTRL_ZADIG), LT("gui.zadig_btn")); /* W185 */
    ui_settext(ctl(ID_CTRL_SBC), LT("gui.sbc"));
    ui_settext(ctl(ID_CTRL_SBCCHK), LT("gui.btn_check"));
    ui_settext(ctl(ID_CTRL_MOUSEOFF), LT("gui.ctl_mouseoff"));
    ui_settext(ctl(ID_CTRL_ASSIST), LT("gui.ctl_assist"));
    ui_settext(ctl(ID_CTRL_BROWSER), LT("gui.keys_browser")); /* W179 */
    combo_refill(ID_MOUSE_MODE, kMouse, 2);
    /* Profile + Pfade + Fussleiste */
    ui_settext(ctl(ID_PROF_LOAD), LT("gui.adv_setup_load"));
    ui_settext(ctl(ID_PROF_SAVE), LT("gui.adv_setup_save"));
    ui_settext(ctl(ID_PROF_DEL), LT("gui.btn_delete"));
    ui_settext(ctl(ID_PROF_DFLT), LT("gui.adv_setup_auto"));
    ui_settext(ctl(ID_XBE_B), LT("gui.btn_browse"));
    ui_settext(ctl(ID_IMAGE_B), LT("gui.btn_browse")); /* W150 */
    ui_settext(ctl(ID_IMAGE_C), LT("gui.btn_check"));
    ui_settext(ctl(ID_MEDIA_B), LT("gui.btn_browse"));
    ui_settext(ctl(ID_SAVE), LT("gui.btn_save"));
    /* W172 LAUNCHER-P567: Spielen, Erweitert, Packs */
    for (i = 0; i < SUM_GO_N; i++) {
        ui_settext(ctl(ID_SUM_GO0 + i), LT("gui.play_change"));
    }
    ui_settext(ctl(ID_PLAY_FOLDER), LT("gui.play_use_folder"));
    ui_settext(ctl(ID_HDD_OPEN), LT("gui.adv_open"));
    ui_settext(ctl(ID_DATA_OPEN), LT("gui.adv_open_folder"));
    ui_settext(ctl(ID_GFX_PACKS), LT("gui.gfx_to_packs"));
    ui_settext(ctl(ID_PK_UP), "\xE2\x86\x91");
    ui_settext(ctl(ID_PK_DOWN), "\xE2\x86\x93");
    ui_settext(ctl(ID_PK_TOGGLE), LT("gui.pk_toggle"));
    ui_settext(ctl(ID_PK_ADD), LT("gui.pk_add"));
    ui_settext(ctl(ID_PK_DEL), LT("gui.pk_del"));
    ui_settext(ctl(ID_PK_CHECK), LT("gui.pk_check"));
    ui_settext(ctl(ID_PK_CHECKALL), LT("gui.pk_checkall"));
    ui_settext(ctl(ID_PK_FOLDER), LT("gui.pk_folder"));
    ui_settext(ctl(ID_PK_CAPRESET), LT("gui.pk_capreset"));
    ui_settext(ctl(ID_PK_LVL2), LT("gui.pk_x2"));
    ui_settext(ctl(ID_PK_LVL4), LT("gui.pk_x4best"));
    ui_settext(ctl(ID_PK_CREATOR), LT("gui.pk_creator"));
    xbe_cue_update();
    packs_refresh_lists();
    ui_settext(ctl(ID_START), LT("gui.btn_start"));
    /* Sprachwahl selbst */
    ui_settext(ctl(ID_LANG_LBL), LT("gui.lang_label"));
    combo_refill(ID_LANG, kLang, 2);
    /* Abgeleitete Texte neu (liest nur, aendert keine Eingaben) */
    {
        launcher_settings_t tmp;
        char note[256] = { 0 };
        int kind = 0;
        app_load_effective(g_client_dir, &tmp, note, sizeof note, &kind,
                           sb_lang_code());
        strcpy_s(g_note, sizeof g_note, note);
        g_note_kind = kind; /* W173 */
        if (kind == 2) {
            strcpy_s(g_profile_line, sizeof g_profile_line, note);
        } else if (kind == 1) {
            _snprintf_s(g_profile_line, sizeof g_profile_line, _TRUNCATE,
                        LT("gui.fill_writes"), note);
        } else {
            _snprintf_s(g_profile_line, sizeof g_profile_line, _TRUNCATE,
                        "%s", LT("gui.fill_nodefaults"));
        }
    }
    update_aspect_info();
    update_sbc_status();
    update_pad_status();
    update_ctrl_overview();
    refresh_profiles();
    update_summary();
    update_vr_button(); /* W159: Hinweistext in neuer Sprache */
    if (!g_has_ldr) {
        set_status_k(LT("gui.fill_noldr"), 2);
    } else { /* W173 LAUNCHER-P8: ohne Schluesselzahl (steht unter Diagnose) */
        set_status(g_note_kind == 2 ? g_note : LT("gui.fill_ready"));
    }
    base_build(); /* statischer Hintergrund (Fusshinweis) */
    redraw_all();
}

static void build_controls(void)
{
    HWND c;
    int i;
    HWND w = g_hwnd;
    /* W171 LAUNCHER-P2P3: Reihenfolge = Tab-Reihenfolge (je Seite links
     * oben nach unten, dann rechte Spalte); Freigaben setzt ausschliesslich
     * ui_apply_rules (Regelwerk), hier kein EnableWindow. */
    /* Titelleiste */
    mk(w, K_TBMIN, ID_MIN, PG_ALL, WIN_W - 92, 0, 46, 40, LT("gui.win_min"),
       0);
    mk(w, K_TBCLOSE, ID_CLOSE, PG_ALL, WIN_W - 46, 0, 46, 40, LT("gui.btn_close"), 0);
    /* Navigation (7 Seiten) */
    for (i = 0; i < P_N; i++) {
        mk(w, K_NAV, ID_NAV0 + i, PG_ALL, 12, TITLE_H + 16 + i * 44,
           NAV_W - 24, 38, LT(kPageName[i]), 0);
    }
    /* W142: Sprachwahl (Sidebar unten, alle Seiten, wirkt sofort) */
    mk(w, K_STATIC, ID_LANG_LBL, PG_ALL, 12, 506, NAV_W - 24, 24,
       LT("gui.lang_label"), 0);
    c = mk(w, K_COMBO, ID_LANG, PG_ALL, 12, 532, NAV_W - 24, 30, "", 0);
    combo_add(c, LT("gui.lang_en"));
    combo_add(c, LT("gui.lang_de"));
    ComboBox_SetCurSel(c, 0);

    /* --- Spielen: Startknoepfe (W159: Bildschirm + VR), Disc-Abbild --- */
    mk(w, K_BIG, ID_START_BIG, P_PLAY, CX0, CY0 + 84, 308, 56, LT("gui.btn_bigstart"), 0);
    mk(w, K_BIG, ID_START_VR, P_PLAY, CX0, CY0 + 146, 308, 40, LT("gui.btn_vrstart"), 0);
    /* W150 ISO-DIRECT, W172/P6: Einrichtungskarte (nur ohne gueltige
     * Spieldaten oder nach "Aendern"): Abbild-Zeile + Verweis Ordner-Modus */
    mk(w, K_EDIT, ID_IMAGE, P_PLAY, CX0 + 18, CY0 + 276, CW - 300, 32, "", 0);
    mk(w, K_BTN, ID_IMAGE_C, P_PLAY, CX0 + CW - 272, CY0 + 276, 122, 32, LT("gui.btn_check"), 0);
    mk(w, K_BTN, ID_IMAGE_B, P_PLAY, CX0 + CW - 140, CY0 + 276, 122, 32, LT("gui.btn_browse"), 0);
    mk(w, K_BTN, ID_PLAY_FOLDER, P_PLAY, CX0 + CW - 18 - 186, CY0 + 396, 186, 30,
       LT("gui.play_use_folder"), 0);
    /* W172/P6: Bereitschaft: "Aendern" je Zeile springt zur Ursache */
    for (i = 0; i < SUM_GO_N; i++) {
        mk(w, K_BTN, ID_SUM_GO0 + i, P_PLAY, CX0 + CW - 18 - 104, CY0 + 252 + i * 36,
           104, 28, LT("gui.play_change"), 0);
    }

    /* --- Anzeige: Bildschirm --- */
    /* W81 WINMODE: [display] window_mode + fs_hz (Alt+Enter schaltet im Spiel). */
    c = mk(w, K_COMBO, ID_WINMODE, P_DISPLAY, CX0 + 120, CY0 + 44, COLW - 138,
           30, "", 0);
    combo_add(c, LT("gui.wm_win"));
    combo_add(c, LT("gui.wm_borderless"));  /* randloses Vollbild */
    combo_add(c, LT("gui.wm_excl")); /* exklusives Vollbild */
    ComboBox_SetCurSel(c, 0);
    c = mk(w, K_COMBO, ID_FSHZ, P_DISPLAY, CX0 + 120, CY0 + 80, 110, 30, "", 0);
    for (i = 0; i < KFSHZ_N; i++) {
        combo_add(c, kFsHz[i].lkey ? LT(kFsHz[i].lkey) : kFsHz[i].label);
    }
    ComboBox_SetCurSel(c, 0);
    c = mk(w, K_COMBO, ID_PRESET, P_DISPLAY, CX0 + 120, CY0 + 132, COLW - 138,
           30, "", 0);
    for (i = 0; i < KPRESETS_N; i++) {
        combo_add(c, kPresets[i].lkey ? LT(kPresets[i].lkey) : kPresets[i].label);
    }
    mk(w, K_EDIT, ID_W, P_DISPLAY, CX0 + 120, CY0 + 168, 90, 30, "",
       ES_NUMBER);
    mk(w, K_EDIT, ID_H, P_DISPLAY, CX0 + 232, CY0 + 168, 90, 30, "",
       ES_NUMBER);
    /* Bildrate (Mission) */
    c = mk(w, K_COMBO, ID_IPFPS, P_DISPLAY, CX0 + 120, CY0 + 288, COLW - 138,
           30, "", 0);
    combo_add(c, LT("gui.ip_30"));
    combo_add(c, LT("gui.ip_60"));
    combo_add(c, LT("gui.ip_120"));
    /* W134 VR-PROTO, W159: Vorschau-Art (Reihenfolge = vr_from/to_settings) */
    c = mk(w, K_COMBO, ID_VR, P_DISPLAY, CX0 + 120, CY0 + 412, COLW - 138, 30, "", 0);
    combo_add(c, LT("gui.vr_prev_sbs"));
    combo_add(c, LT("gui.vr_prev_left"));
    combo_add(c, LT("gui.vr_prev_right"));
    combo_add(c, LT("gui.vr_prev_off"));
    ComboBox_SetCurSel(c, 1);
    /* W183 VR-FIX3: VR-Leistung (Preset) + Render-Aufloesung nebeneinander */
    c = mk(w, K_COMBO, ID_VRPRESET, P_DISPLAY, CX0 + 120, CY0 + 446, (COLW - 138 - 8) * 3 / 5, 30, "", 0);
    combo_add(c, LT("gui.vrp_perf"));
    combo_add(c, LT("gui.vrp_bal"));
    combo_add(c, LT("gui.vrp_qual"));
    combo_add(c, LT("gui.vrp_custom"));
    ComboBox_SetCurSel(c, 1);
    c = mk(w, K_COMBO, ID_VRRES, P_DISPLAY, CX0 + 120 + (COLW - 138 - 8) * 3 / 5 + 8, CY0 + 446,
           (COLW - 138 - 8) - (COLW - 138 - 8) * 3 / 5, 30, "", 0);
    for (i = 0; i < KVRRES_N; i++) {
        char t[16];
        _snprintf_s(t, sizeof t, _TRUNCATE, "%d %%", kVrRes[i]);
        combo_add(c, t);
    }
    ComboBox_SetCurSel(c, vrres_index(80));
    /* Bildformat: fest (S3.10), Schalter bleibt nur fuer den Roundtrip */
    mk(w, K_TOGGLE, ID_ASPECT, P_DISPLAY, COL2 + 18, CY0 + 46, COLW - 36, 28, "", 0);
    /* W107 EDGE-MODE: [render] edge_mode / edge_fade_width / edge_fade_strength */
    c = mk(w, K_COMBO, ID_EDGE, P_DISPLAY, COL2 + 120, CY0 + 100, COLW - 138,
           30, "", 0);
    combo_add(c, LT("gui.edge_off"));
    combo_add(c, LT("gui.edge_cut"));
    combo_add(c, LT("gui.edge_fade"));
    ComboBox_SetCurSel(c, 0);
    c = mk(w, K_COMBO, ID_EDGE_W, P_DISPLAY, COL2 + 120, CY0 + 136,
           (COLW - 138 - 8) / 2, 30, "", 0);
    for (i = 0; i < KEDGEW_N; i++) {
        char t[32];
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.edge_w_fmt"), kEdgeW[i]);
        combo_add(c, t);
    }
    ComboBox_SetCurSel(c, 2);
    c = mk(w, K_COMBO, ID_EDGE_S, P_DISPLAY, COL2 + 120 + (COLW - 138 - 8) / 2 + 8,
           CY0 + 136, (COLW - 138 - 8) / 2, 30, "", 0);
    for (i = 0; i < KEDGES_N; i++) {
        char t[32];
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.edge_s_fmt"), kEdgeS[i]);
        combo_add(c, t);
    }
    ComboBox_SetCurSel(c, KEDGES_N - 1);
    /* W148: Render-Skalierung (Index: 0 = Auto dyn, 1 = Auto fest, 2..5 = 1..4) */
    c = mk(w, K_COMBO, ID_RTSCALE, P_DISPLAY, COL2 + 120, CY0 + 432, COLW - 138, 30, "", 0);
    combo_add(c, LT("gui.rs_dyn"));
    combo_add(c, LT("gui.rs_autof"));
    combo_add(c, "1");
    combo_add(c, "2");
    combo_add(c, "3");
    combo_add(c, "4");
    ComboBox_SetCurSel(c, 0);

    /* --- Grafik (W173 LAUNCHER-P4, Konzept 4.3): links Preset (vier
     * Optionsfelder, Zuruecksetzen, Sprung Packs) + Bild, rechts Welt + HUD.
     * Anlage-Reihenfolge = Tab-Reihenfolge (links oben -> unten, dann rechts). --- */
    for (i = 0; i < 4; i++) {
        mk(w, K_RADIO, ID_GP0 + i, P_GFX, CX0 + 18, CY0 + 42 + i * 48,
           i == 3 ? COLW - 36 - 92 : COLW - 36, 48, "", 0);
    }
    mk(w, K_BTN, ID_GP_RESET, P_GFX, CX0 + COLW - 18 - 84, CY0 + 42 + 3 * 48 + 4, 84, 26,
       LT("gui.gfx_reset"), 0);
    mk(w, K_BTN, ID_GFX_PACKS, P_GFX, CX0 + COLW - 18 - 96, CY0 + 254, 96, 26,
       LT("gui.gfx_to_packs"), 0);
    c = mk(w, K_COMBO, ID_AAMODE, P_GFX, CX0 + 120, CY0 + 348, COLW - 138,
           30, "", 0); /* W131: aa_mode 0..3 */
    combo_add(c, LT("gui.aa_off"));
    combo_add(c, LT("gui.aa_final"));
    combo_add(c, LT("gui.aa_world"));
    combo_add(c, LT("gui.aa_both"));
    ComboBox_SetCurSel(c, 0);
    c = mk(w, K_COMBO, ID_MONFX, P_GFX, CX0 + 120, CY0 + 384, COLW - 138,
           30, "", 0); /* W105: Reihenfolge = kMonFx */
    combo_add(c, LT("gui.monfx_orig"));
    combo_add(c, LT("gui.monfx_grain"));
    combo_add(c, LT("gui.monfx_glitch"));
    combo_add(c, LT("gui.off"));
    ComboBox_SetCurSel(c, 0);
    SendMessageW(c, CB_SETDROPPEDWIDTH, (WPARAM)SX(260), 0);
    c = mk(w, K_COMBO, ID_GFX_LIGHT, P_GFX, COL2 + 120, CY0 + 44, COLW - 138, 30, "", 0);
    combo_add(c, LT("gui.gfx_light_on"));
    combo_add(c, LT("gui.gfx_light_off"));
    ComboBox_SetCurSel(c, 0);
    c = mk(w, K_COMBO, ID_GFX_SHADOW, P_GFX, COL2 + 120, CY0 + 80, COLW - 138, 30, "", 0);
    combo_add(c, LT("gui.off"));
    combo_add(c, LT("gui.on"));
    ComboBox_SetCurSel(c, 0);
    c = mk(w, K_COMBO, ID_GFX_SOFT, P_GFX, COL2 + 120, CY0 + 116, COLW - 138, 30, "", 0);
    combo_add(c, LT("gui.off"));
    combo_add(c, LT("gui.on"));
    ComboBox_SetCurSel(c, 0);
    c = mk(w, K_COMBO, ID_GFX_FX, P_GFX, COL2 + 120, CY0 + 152, COLW - 138, 30, "", 0);
    combo_add(c, LT("gui.gfx_fx_orig"));
    combo_add(c, LT("gui.gfx_fx_subtle"));
    combo_add(c, LT("gui.gfx_fx_strong"));
    ComboBox_SetCurSel(c, 0);
    c = mk(w, K_COMBO, ID_GFX_TEX, P_GFX, COL2 + 120, CY0 + 212, COLW - 138, 30, "", 0);
    combo_add(c, LT("gui.gfx_tex_o"));
    combo_add(c, LT("gui.gfx_tex_hd"));
    ComboBox_SetCurSel(c, 0);
    SendMessageW(c, CB_SETDROPPEDWIDTH, (WPARAM)SX(260), 0);
    /* W172/P5: Texturdetail nur auf Packs; die Kombi bleibt unsichtbar als
     * Traeger fuer hd_scale. */
    c = mk(w, K_COMBO, ID_REM_SCALE, P_GFX, COL2 + 120, CY0 + 212,
           COLW - 138, 30, "", 0);
    combo_add(c, LT("gui.scale2"));
    combo_add(c, LT("gui.scale4"));
    ComboBox_SetCurSel(c, 1);
    mk(w, K_TOGGLE, ID_REM_LABEL, P_GFX, COL2 + 18, CY0 + 348, COLW - 36, 28, LT("gui.rem_label"), 0); /* W153 */

    /* --- Packs (W172 LAUNCHER-P5): Liste links, Texturdetail links unten,
     * gewaehltes Pack rechts (Tab: Hinzufuegen, Liste, Fuss, Detail) --- */
    mk(w, K_ACCENT, ID_PK_ADD, P_PACKS, CX0 + PKL_W - 18 - 150, CY0 + 9, 150, 28,
       LT("gui.pk_add"), 0);
    c = mk(w, K_PKLIST, ID_PK_LIST, P_PACKS, CX0 + 18, CY0 + 58, PKL_W - 36, 180,
           "", 0);
    if (c) {
        SetWindowSubclass(c, pklist_proc, 2, 0);
    }
    mk(w, K_BTN, ID_PK_UP, P_PACKS, CX0 + 18, CY0 + 246, 40, 28, "\xE2\x86\x91", 0);
    mk(w, K_BTN, ID_PK_DOWN, P_PACKS, CX0 + 62, CY0 + 246, 40, 28, "\xE2\x86\x93", 0);
    mk(w, K_BTN, ID_PK_CREATOR, P_PACKS, CX0 + 18, CY0 + 118, 196, 30,
       LT("gui.pk_creator"), 0);
    mk(w, K_BTN, ID_PK_CHECKALL, P_PACKS, CX0 + PKL_W - 18 - 116 - 6 - 110, CY0 + 246,
       110, 28, LT("gui.pk_checkall"), 0);
    mk(w, K_BTN, ID_PK_FOLDER, P_PACKS, CX0 + PKL_W - 18 - 116, CY0 + 246, 116, 28,
       LT("gui.pk_folder"), 0);
    mk(w, K_SEG, ID_PK_LVL2, P_PACKS, CX0 + PKL_W - 18 - 196, CY0 + 305, 98, 26,
       LT("gui.pk_x2"), 0);
    mk(w, K_SEG, ID_PK_LVL4, P_PACKS, CX0 + PKL_W - 18 - 98, CY0 + 305, 98, 26,
       LT("gui.pk_x4best"), 0);
    mk(w, K_BTN, ID_PK_CAPRESET, P_PACKS, CX0 + PKL_W - 18 - 112, CY0 + 352, 112, 23,
       LT("gui.pk_capreset"), 0);
    for (i = 0; i < SB_PACK_NCAT; i++) {
        mk(w, K_COMBO, ID_PK_CAT0 + i, P_PACKS, CX0 + 18 + (i % 3) * 122,
           CY0 + 391 + (i / 3) * 37, 112, 23, "", 0);
    }
    mk(w, K_BTN, ID_PK_TOGGLE, P_PACKS, PKR_X + 18, CY0 + 452, 78, 30,
       LT("gui.pk_toggle"), 0);
    mk(w, K_BTN, ID_PK_CHECK, P_PACKS, PKR_X + 18 + 83, CY0 + 452, 78, 30,
       LT("gui.pk_check"), 0);
    mk(w, K_BTN, ID_PK_DEL, P_PACKS, PKR_X + 18 + 166, CY0 + 452, 78, 30,
       LT("gui.pk_del"), 0);

    /* --- Steuerung: Eingabegeraet, Belegung, Tastenuebersicht/Maus --- */
    /* W185: Anleitung in der Titelzeile der Geraete-Karte (Titel links,
     * Freiflaeche rechts; einziger freier Platz ohne Umbau). */
    mk(w, K_BTN, ID_CTRL_ZADIG, P_CTRL, CX0 + COLW - 18 - 156, CY0 + 10,
       156, 26, LT("gui.zadig_btn"), 0);
    mk(w, K_TOGGLE, ID_CTRL_SBC, P_CTRL, CX0 + 18, CY0 + 44, 210, 28, LT("gui.sbc"), 0);
    mk(w, K_BTN, ID_CTRL_SBCCHK, P_CTRL, CX0 + 232, CY0 + 42, 90, 32, LT("gui.btn_check"), 0);
    /* W128: Maus-Achsen aus, solange der Original-Controller aktiv ist */
    mk(w, K_TOGGLE, ID_CTRL_MOUSEOFF, P_CTRL, CX0 + 18, CY0 + 150, 304, 28, LT("gui.ctl_mouseoff"), 0);
    mk(w, K_ACCENT, ID_CTRL_ASSIST, P_CTRL, CX0 + 18, CY0 + 204, COLW - 36, 32, LT("gui.ctl_assist"), 0);
    mk(w, K_COMBO, ID_CTRL_PROF, P_CTRL, CX0 + 18, CY0 + 302, COLW - 36, 30,
       "", 0);
    mk(w, K_BTN, ID_CTRL_REFRESH, P_CTRL, CX0 + 18, CY0 + 340, 146, 32, LT("gui.ctl_reload"), 0);
    mk(w, K_ACCENT, ID_CTRL_BIND, P_CTRL, CX0 + 176, CY0 + 340, 146, 32, LT("gui.ctl_bind"), 0);
    /* W87: Maus-Zielen absolut/relativ + Empfindlichkeit */
    c = mk(w, K_COMBO, ID_MOUSE_MODE, P_CTRL, COL2 + 78, CY0 + 108,
           COLW - 18 - 70 - 52 - 8 - 78, 30, "", 0);
    SendMessageW(c, CB_SETDROPPEDWIDTH, (WPARAM)SX(240), 0);
    combo_add(c, LT("gui.mouse_abs"));
    combo_add(c, LT("gui.mouse_rel"));
    c = mk(w, K_COMBO, ID_MOUSE_SENS, P_CTRL, COL2 + COLW - 18 - 70,
           CY0 + 108, 70, 30, "", 0);
    {
        char t[8];
        for (i = 1; i <= 16; i++) {
            _snprintf_s(t, sizeof t, _TRUNCATE, "%d", i);
            combo_add(c, t);
        }
    }
    mk(w, K_MEMO, ID_KEYS, P_CTRL, COL2 + 18, CY0 + 162, COLW - 36, 276,
       "", 0);

    /* W179 KEYS-HTML: Tastenuebersicht als HTML (immer aktiv, ohne Regel) */
    mk(w, K_BTN, ID_CTRL_BROWSER, P_CTRL, COL2 + 18, CY0 + 446, COLW - 36, 30,
       LT("gui.keys_browser"), 0);

    /* --- Ton --- */
    mk(w, K_TOGGLE, ID_AUDIO, P_AUDIO, CX0 + 18, CY0 + 50, 300, 28, LT("gui.ton_on"),
       0);

    /* --- Erweitert: Spieldateien (Ordner-Modus), Spielstaende, gespeicherte
     * Einstellungen, Feinabstimmung, Launcher, Diagnose --- */
    /* W172/P7: Spieldateien (Programmdatei optional), Spielstaende +
     * Datenort (Oeffnen), gespeicherte Einstellungen (Laden/Speichern als/
     * Loeschen + Schalter "beim Start laden") */
    mk(w, K_EDIT, ID_MEDIA, P_ADV, CX0 + 18, CY0 + 74, COLW - 36 - 120, 30, "", 0);
    mk(w, K_BTN, ID_MEDIA_B, P_ADV, CX0 + COLW - 18 - 112, CY0 + 74, 112, 30, LT("gui.btn_browse"), 0);
    mk(w, K_EDIT, ID_XBE, P_ADV, CX0 + 18, CY0 + 124, COLW - 36 - 120, 30, "", 0);
    mk(w, K_BTN, ID_XBE_B, P_ADV, CX0 + COLW - 18 - 112, CY0 + 124, 112, 30, LT("gui.btn_browse"), 0);
    mk(w, K_EDIT, ID_HDD, P_ADV, CX0 + 104, CY0 + 236, COLW - 104 - 18 - 90, 30, "", 0);
    mk(w, K_BTN, ID_HDD_OPEN, P_ADV, CX0 + COLW - 18 - 82, CY0 + 236, 82, 30,
       LT("gui.adv_open"), 0);
    mk(w, K_BTN, ID_DATA_OPEN, P_ADV, CX0 + COLW - 18 - 112, CY0 + 272, 112, 26,
       LT("gui.adv_open_folder"), 0);
    mk(w, K_COMBOEDIT, ID_PROF_COMBO, P_ADV, CX0 + 18, CY0 + 358, COLW - 36, 30,
       "", 0);
    mk(w, K_ACCENT, ID_PROF_LOAD, P_ADV, CX0 + 18, CY0 + 394, 96, 30, LT("gui.adv_setup_load"), 0);
    mk(w, K_BTN, ID_PROF_SAVE, P_ADV, CX0 + 122, CY0 + 394, 96, 30, LT("gui.adv_setup_save"), 0);
    mk(w, K_BTN, ID_PROF_DEL, P_ADV, CX0 + 226, CY0 + 394, 96, 30, LT("gui.btn_delete"), 0);
    mk(w, K_TOGGLE, ID_PROF_DFLT, P_ADV, CX0 + 18, CY0 + 432, COLW - 36, 28,
       LT("gui.adv_setup_auto"), 0);
    c = mk(w, K_COMBO, ID_VSYNC, P_ADV, COL2 + 170, CY0 + 44, COLW - 188,
           30, "", 0);
    combo_add(c, LT("gui.off"));
    combo_add(c, LT("gui.on"));
    c = mk(w, K_COMBO, ID_FPS, P_ADV, COL2 + 170, CY0 + 82, COLW - 188,
           30, "", 0);
    combo_add(c, LT("gui.fps_off"));
    combo_add(c, "30");
    combo_add(c, "60");
    combo_add(c, "120");
    mk(w, K_TOGGLE, ID_BG, P_ADV, COL2 + 18, CY0 + 176, COLW - 36, 28, LT("gui.bg"), 0);
    mk(w, K_BTN, ID_DIAG, P_ADV, COL2 + COLW - 18 - 130, ADV_DIAG_Y + 9, 130, 28,
       LT(g_diag_open ? "gui.adv_diag_hide" : "gui.adv_diag_show"), 0);
    mk(w, K_TOGGLE, ID_LIVE, P_ADV, COL2 + 18, ADV_DIAG_Y + 188, COLW - 36, 28, LT("gui.ton_live"), 0);

    /* --- Fussleiste: Rueckgaengig, Speichern, Launch v --- */
    mk(w, K_BTN, ID_UNDO, PG_ALL, WIN_W - 28 - 150 - 12 - 130 - 10 - 112,
       WIN_H - BOT_H + 14, 112, 36, LT("gui.undo"), 0);
    mk(w, K_BTN, ID_SAVE, PG_ALL, WIN_W - 28 - 150 - 12 - 130,
       WIN_H - BOT_H + 14, 130, 36, LT("gui.btn_save"), 0);
    mk(w, K_ACCENT, ID_START, PG_NOTPLAY, WIN_W - 28 - 150,
       WIN_H - BOT_H + 14, 118, 36, LT("gui.btn_start"), 0);
    mk(w, K_ACCENT, ID_START_MENU, PG_NOTPLAY, WIN_W - 28 - 30,
       WIN_H - BOT_H + 14, 30, 36, "", 0);
    keys_tabstops();
}

/* ------------------------------------------------------------------ */
/* Hauptfenster                                                        */
/* ------------------------------------------------------------------ */
/* W171: Launch-Menue der Fussleiste (Bildschirm / VR, R5 graut VR aus). */
static int launch_menu(void)
{
    HMENU m = CreatePopupMenu();
    RECT r;
    wchar_t w1[128], w2[128];
    int cmd;
    if (!m) {
        return 0;
    }
    u8_to_w(LT("gui.launch_screen"), w1, 128);
    u8_to_w(LT("gui.launch_vr"), w2, 128);
    AppendMenuW(m, MF_STRING, 1, w1);
    AppendMenuW(m, MF_STRING | (g_rr[5].disable ? MF_GRAYED : 0), 2, w2);
    GetWindowRect(ctl(ID_START), &r);
    cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_BOTTOMALIGN,
                              r.right + (r.bottom - r.top), r.top, 0, g_hwnd, NULL);
    DestroyMenu(m);
    return cmd;
}

/* W171 Muster C: Stand vor einer automatischen Umschaltung. */
static void snap_settings(launcher_settings_t *out)
{
    *out = g_s;
    ui_to_settings(out);
}

static void on_command_inner(int id, int code);

static void on_command(int id, int code)
{
    if (id >= ID_NAV0 && id < ID_NAV0 + P_N) {
        show_page(id - ID_NAV0);
        return;
    }
    on_command_inner(id, code);
    /* W171: nach jeder Bedienung Regelwerk neu (Freigaben + Begruendungen) */
    if (!g_updating && id != ID_CLOSE && id != ID_MIN) {
        ui_apply_rules();
    }
}

static void on_command_inner(int id, int code)
{
    switch (id) {
    case ID_MIN:
        ShowWindow(g_hwnd, SW_MINIMIZE);
        break;
    case ID_CLOSE:
        PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
        break;
    case IDOK: { /* Enter (IsDialogMessage) */
        /* In Eingabefeldern (Edit/Combo) startet Enter nicht. */
        HWND f = GetFocus();
        wchar_t cls[32] = { 0 };
        if (f) {
            HWND p = GetParent(f);
            GetClassNameW((p && p != g_hwnd) ? p : f, cls, 32);
        }
        if (_wcsicmp(cls, L"Edit") == 0 || _wcsicmp(cls, WC_COMBOBOXW) == 0) {
            set_status(LT("gui.msg_enter"));
            break;
        }
    }
    /* fall through */
    case ID_START_BIG:
    case ID_START:
    case ID_START_VR: /* W159: Spielen in VR */
    case ID_START_MENU: {
        int vr = id == ID_START_VR;
        if (launch_blocked()) { /* W171 Muster E: Grund statt Startversuch */
            static const int kBlk[] = { 4, 3 };
            set_status_k(g_rr[first_rule(kBlk, 2)].text, 2);
            break;
        }
        if (id == ID_START_MENU) {
            int cmd = launch_menu();
            if (cmd != 1 && cmd != 2) {
                break;
            }
            vr = cmd == 2;
        }
        if (vr && g_rr[5].disable) {
            set_status_k(g_rr[5].text, 2);
            break;
        }
        do_start(0, vr); /* W159: Bildschirm (Enter bleibt hier) bzw. VR */
        if (g_page == P_PLAY) {
            update_summary();
            redraw_all();
        }
        break;
    }
    case ID_UNDO: /* W171 Muster C: automatisches Umschalten zuruecknehmen */
        if (g_undo_on) {
            launcher_settings_t u = g_undo_s;
            ui_from_settings(&u);
            set_status_k(LT("gui.undo_done"), 1);
            redraw_all();
        }
        break;
    case ID_DIAG: /* W171: Diagnose ein-/ausklappen (Zustand in der UI-cfg) */
        g_diag_open = !g_diag_open;
        ui_settext(ctl(ID_DIAG), LT(g_diag_open ? "gui.adv_diag_hide" : "gui.adv_diag_show"));
        ui_cfg_save_bg(g_client_dir, g_bg_on);
        show_page(g_page);
        break;
    case IDCANCEL: /* Esc: Unterseite -> Spielen, Spielen -> Beenden */
        if (g_page != P_PLAY) {
            show_page(P_PLAY);
            SetFocus(ctl(ID_START_BIG));
        } else {
            PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
        }
        break;
    case ID_SAVE:
        if (do_save(1) == 0) {
            set_status_k(LT("gui.msg_saved"), 1);
        }
        break;
    case ID_BG:
        g_bg_on = !g_bg_on;
        ui_set_checked(ID_BG, g_bg_on);
        ui_cfg_save_bg(g_client_dir, g_bg_on);
        base_build();
        redraw_all();
        break;
    case ID_LANG: /* W142: Sprache wirkt sofort (Speichern/Starten persistiert) */
        if (code == CBN_SELCHANGE && !g_updating) {
            int de = ComboBox_GetCurSel(ctl(ID_LANG)) == 1;
            ui_to_settings(&g_s);
            strcpy_s(g_s.lang, sizeof g_s.lang, de ? "de" : "en");
            sb_lang_set(g_s.lang);
            ui_apply_language();
        }
        break;
    case ID_ASPECT:
        /* W170 LAUNCHER-P1/B7: nur noch Sicherheit (Schalter ist ausgeblendet,
         * fest auf Pillarbox S3.10). */
        ui_set_checked(ID_ASPECT, 1);
        break;
    case ID_AUDIO: /* W171: Live-Ton folgt per Regel R27 */
        ui_set_checked(ID_AUDIO, !ui_checked(ID_AUDIO));
        break;
    case ID_LIVE:
        ui_set_checked(ID_LIVE, !ui_checked(ID_LIVE));
        break;
    case ID_CTRL_SBC:
        ui_set_checked(ID_CTRL_SBC, !ui_checked(ID_CTRL_SBC));
        /* W170/B10, W171: Maus-Optionen folgen per Regel R23/R24 */
        break;
    case ID_CTRL_MOUSEOFF:
        ui_set_checked(ID_CTRL_MOUSEOFF, !ui_checked(ID_CTRL_MOUSEOFF));
        break;
    case ID_REM_LABEL: { /* W153; W173 (PO 7.11.): eigene Option, nicht im Grafik-Preset */
        g_gfx.shx[LSHX_LABEL] = ui_checked(ID_REM_LABEL) ? 0 : 1;
        ui_set_checked(ID_REM_LABEL, g_gfx.shx[LSHX_LABEL]);
        set_status(LT(g_gfx.shx[LSHX_LABEL] ? "gui.rem_label_on" : "gui.rem_label_off"));
        break;
    }
    case ID_GP0: /* W173 LAUNCHER-P4: Grafik-Preset waehlen (setzt alle Optik-Werte) */
    case ID_GP0 + 1:
    case ID_GP0 + 2:
    case ID_GP_RESET: {
        lgp_mask_t lock = gfx_lock_now();
        int was = lcfg_gfx_preset(&g_gfx, lock);
        int sel = id == ID_GP_RESET ? lcfg_gfx_base(&g_gfx, lock) : id - ID_GP0;
        launcher_settings_t before;
        char m[400], nm[160];
        if (sel == was) {
            break;
        }
        snap_settings(&before);
        if (was == LGP_CUSTOM && id != ID_GP_RESET &&
            !ui_confirm(LT("gui.rule_r14"), LT("gui.gfx_preset_card"))) {
            break; /* R14: Rueckfrage "Nein" -> eigene Werte bleiben */
        }
        lcfg_gfx_apply(&g_gfx, sel);
        gfx_sync_ui();
        gfx_name(sel, &before, nm, sizeof nm);
        if (was == LGP_CUSTOM) { /* Muster C: eigene Werte ersetzt -> Rueckgaengig */
            _snprintf_s(m, sizeof m, _TRUNCATE, LT("gui.rule_c_preset"), nm);
            set_status_undo(m, &before);
        } else {
            _snprintf_s(m, sizeof m, _TRUNCATE, LT("gui.gfx_applied"), nm);
            set_status(m);
        }
        ui_refresh_visibility();
        redraw_all();
        break;
    }
    case ID_GP0 + 3: /* Benutzerdefiniert entsteht durch Einzelaenderung */
        if (!ui_checked(ID_GP0 + 3)) {
            set_status(LT("gui.gfx_p_custom_off"));
        }
        break;
    case ID_GFX_LIGHT: /* W173: Einzeloptionen des Presets (-> Benutzerdefiniert) */
    case ID_GFX_SHADOW:
    case ID_GFX_SOFT:
    case ID_GFX_FX:
    case ID_GFX_TEX:
    case ID_AAMODE:
        if (code == CBN_SELCHANGE && !g_updating) {
            lgp_mask_t lock = gfx_lock_now();
            int sel = ComboBox_GetCurSel(ctl(id));
            int was = lcfg_gfx_preset(&g_gfx, lock);
            launcher_settings_t before;
            snap_settings(&before);
            if (sel < 0) {
                break;
            }
            switch (id) {
            case ID_GFX_LIGHT: g_gfx.shx[LSHX_SHADING] = sel == 0 ? 1 : 0; break;
            case ID_GFX_SHADOW: g_gfx.shx[LSHX_SHADOW] = sel ? 1 : 0; break;
            case ID_GFX_SOFT: g_gfx.shx[LSHX_SOFT] = sel ? 1 : 0; break;
            case ID_GFX_FX: lcfg_fx_apply_level(&g_gfx, sel); break; /* 3 = eigene Werte bleiben */
            case ID_GFX_TEX: g_gfx.remaster = g_gfx.hd_textures = sel ? 1 : 0; break;
            default: if (sel <= 3) { g_gfx.shx[LSHX_AA] = sel; } break;
            }
            gfx_sync_ui();
            if (lcfg_gfx_preset(&g_gfx, lock) != was) { /* Muster C: Preset folgt */
                char m[400], nm[200];
                gfx_state_name(&g_gfx, nm, sizeof nm);
                _snprintf_s(m, sizeof m, _TRUNCATE, LT("gui.rule_c_shader"), nm);
                set_status_undo(m, &before);
            }
            ui_refresh_visibility();
            redraw_all();
        }
        break;
    case ID_EDGE: /* W107 EDGE-MODE (W171: Verlauf frei per Regel R11/R12) */
    case ID_EDGE_W:
    case ID_EDGE_S:
        if (code == CBN_SELCHANGE && !g_updating) {
            update_aspect_info();
            redraw_rect(COL2, CY0, COLW, 376);
        }
        break;
    case ID_VRPRESET: /* W183 VR-FIX3: Preset -> Aufloesung mitfuehren */
        if (code == CBN_SELCHANGE && !g_updating) {
            int ps = ComboBox_GetCurSel(ctl(ID_VRPRESET));
            if (ps >= 0 && ps <= 2) {
                g_updating++;
                ComboBox_SetCurSel(ctl(ID_VRRES), vrres_index(lcfg_vr_preset_value(ps + 1, 0)));
                g_updating--;
            }
        }
        break;
    case ID_VRRES: /* W183: eigene Aufloesung -> Preset neu bestimmen */
        if (code == CBN_SELCHANGE && !g_updating) {
            launcher_settings_t t = g_s;
            int ps = ComboBox_GetCurSel(ctl(ID_VRPRESET)), rs = ComboBox_GetCurSel(ctl(ID_VRRES)), m;
            if (ps >= 0 && ps <= 2) { lcfg_vr_preset_apply(&t, ps + 1); }
            if (rs >= 0 && rs < KVRRES_N) { t.vr_render_scale = kVrRes[rs]; }
            m = lcfg_vr_preset_match(&t);
            g_updating++;
            ComboBox_SetCurSel(ctl(ID_VRPRESET), m >= 1 ? m - 1 : 3);
            g_updating--;
        }
        break;
    case ID_WINMODE: /* W171: Hz/Aufloesung frei per Regel R7/R8 */
        if (code == CBN_SELCHANGE && !g_updating) {
            update_aspect_info();
            redraw_all();
        }
        break;
    case ID_PRESET:
        if (code == CBN_SELCHANGE && !g_updating) {
            int sel = ComboBox_GetCurSel(ctl(ID_PRESET));
            if (sel >= 0 && sel < KPRESETS_N - 1) {
                g_updating++;
                set_num(ID_W, kPresets[sel].w);
                set_num(ID_H, kPresets[sel].h);
                g_updating--;
                update_aspect_info();
                redraw_rect(COL2, CY0, COLW, 376);
            }
        }
        break;
    case ID_W:
    case ID_H:
        if (code == EN_CHANGE && !g_updating) {
            sync_preset();
            update_aspect_info();
            redraw_rect(COL2, CY0, COLW, 376);
        } else if (code == EN_SETFOCUS || code == EN_KILLFOCUS) {
            g_focus_id = code == EN_SETFOCUS ? id : 0;
            redraw_all();
        }
        break;
    case ID_XBE:
    case ID_MEDIA:
    case ID_HDD:
    case ID_IMAGE: /* W150 */
        if (code == EN_SETFOCUS || code == EN_KILLFOCUS) {
            g_focus_id = code == EN_SETFOCUS ? id : 0;
            redraw_all();
        } else if (code == EN_CHANGE && id == ID_MEDIA && !g_updating) {
            xbe_cue_update(); /* W172: "automatisch: <Ordner>\default.xbe" */
        }
        break;
    case ID_PROF_COMBO: /* W172: Schalter "beim Start laden" folgt der Auswahl */
        if ((code == CBN_SELCHANGE || code == CBN_EDITCHANGE) && !g_updating) {
            setup_auto_sync(code == CBN_SELCHANGE);
        }
        break;
    case ID_SUM_GO0: /* W172/P6: Spiel -> Einrichtungskarte (Abbild) bzw. Erweitert */
        if (g_cur.game_image[0] || !g_cur.media_root[0]) {
            g_setup_force = 1;
            ui_refresh_visibility();
            redraw_all();
            SetFocus(ctl(ID_IMAGE_B));
        } else {
            show_page(P_ADV);
            SetFocus(ctl(ID_MEDIA));
        }
        break;
    case ID_SUM_GO0 + 1:
    case ID_SUM_GO0 + 2:
    case ID_SUM_GO0 + 3:
    case ID_SUM_GO0 + 4:
        show_page(kSumPage[id - ID_SUM_GO0]);
        break;
    case ID_PLAY_FOLDER: /* W172/P6: Ordner-Modus liegt unter Erweitert */
        show_page(P_ADV);
        SetFocus(ctl(ID_MEDIA));
        break;
    case ID_GFX_PACKS:
        show_page(P_PACKS);
        break;
    case ID_HDD_OPEN:
    case ID_DATA_OPEN: { /* W172/P7: Ordner im Explorer */
        char path[1100];
        const char *mk2;
        if (id == ID_HDD_OPEN) {
            adv_hdd_path(path, sizeof path);
        } else {
            adv_data_loc(path, sizeof path, &mk2);
        }
        if (!is_dir_u8(path)) {
            char t[1300];
            _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.adv_nofolder"), path);
            set_status_k(t, 2);
        } else if (g_o.shot != 0) {
            printf("[open] %s\n", path);
        } else {
            wchar_t wp[1100];
            u8_to_w(path, wp, 1100);
            ShellExecuteW(g_hwnd, L"open", wp, NULL, NULL, SW_SHOWNORMAL);
        }
        break;
    }
    case ID_CTRL_PROF:
        if (code == CBN_SELCHANGE && !g_updating) {
            update_ctrl_overview();
        }
        break;
    case ID_CTRL_REFRESH:
        refresh_ctrl_profiles(NULL);
        update_ctrl_overview();
        update_pad_status();
        set_status(LT("gui.msg_prof_reload"));
        break;
    case ID_CTRL_BIND:
        open_bind_dialog();
        break;
    case ID_CTRL_SBCCHK:
        update_sbc_status();
        set_status(LT("gui.msg_sbc"));
        break;
    case ID_CTRL_ASSIST:
        open_assist_dialog();
        break;
    case ID_CTRL_BROWSER: /* W179 KEYS-HTML */
        keyshtml_open();
        break;
    case ID_CTRL_ZADIG: /* W185 ZADIG-HELP */
        zadig_open();
        break;
    case ID_PK_LIST: /* W148 PACK-LAUNCHER, W172: Detail rechts folgt */
        if (code == LBN_SELCHANGE) {
            packs_sel();
            ui_apply_rules();
            redraw_all();
        } else if (code == LBN_DBLCLK) {
            packs_toggle();
        }
        break;
    case ID_PK_UP:
        packs_move(-1);
        break;
    case ID_PK_DOWN:
        packs_move(1);
        break;
    case ID_PK_TOGGLE:
        packs_toggle();
        break;
    case ID_PK_ADD:
        packs_add_menu();
        break;
    case ID_PK_DEL:
        packs_del();
        break;
    case ID_PK_CHECK:
        packs_check();
        break;
    case ID_PK_CHECKALL:
        packs_check_all();
        break;
    case ID_PK_FOLDER:
        packs_folder();
        break;
    case ID_PK_CREATOR:
        packs_creator_open();
        break;
    case ID_PK_LVL2:
    case ID_PK_LVL4:
        packs_set_level(id == ID_PK_LVL2 ? 2 : 4);
        break;
    case ID_PK_CAPRESET:
        packs_cap_reset();
        break;
    case ID_PK_CAT0:
    case ID_PK_CAT0 + 1:
    case ID_PK_CAT0 + 2:
    case ID_PK_CAT0 + 3:
    case ID_PK_CAT0 + 4:
    case ID_PK_CAT0 + 5:
    case ID_PK_CAT0 + 6:
    case ID_PK_CAT0 + 7:
    case ID_PK_CAT0 + 8: /* W172: Grenze je Kategorie (nur senken) */
        if (code == CBN_SELCHANGE && !g_updating) {
            int sel = ComboBox_GetCurSel(ctl(id));
            packs_cap_set(id - ID_PK_CAT0, sel <= 0 ? 0 : sel == 1 ? 2 : 4);
            packs_save();
            packs_refresh(0);
        }
        break;
    case ID_XBE_B:
        browse_xbe();
        break;
    case ID_MEDIA_B:
        browse_media();
        break;
    case ID_IMAGE_B: /* W150 ISO-DIRECT */
        browse_image();
        break;
    case ID_IMAGE_C:
        check_image();
        break;
    case ID_PROF_LOAD: {
        char name[128], err[1024];
        launcher_settings_t ps;
        profile_name_from_ui(name, sizeof name);
        if (lcfg_profile_load(g_client_dir, name, &ps, err, sizeof err) != 0) {
            set_status_k(err, 2);
            ui_msgbox(g_hwnd, err, LT("gui.title_profload"), MB_ICONWARNING | MB_OK);
        } else {
            char st[256];
            g_s = ps;
            ui_from_settings(&g_s);
            _snprintf_s(g_profile_line, sizeof g_profile_line, _TRUNCATE, LT("gui.msg_prof_loaded"), name);
            _snprintf_s(st, sizeof st, _TRUNCATE, LT("gui.msg_prof_loaded_s"), name);
            set_status_k(st, 1);
        }
        break;
    }
    case ID_PROF_SAVE: {
        char name[128], err[1024];
        profile_name_from_ui(name, sizeof name);
        ui_to_settings(&g_s);
        if (lcfg_validate(&g_s, err, sizeof err) != 0) {
            set_status_k(err, 2);
            ui_msgbox(g_hwnd, err, LT("gui.title_profsave"),
                      MB_ICONWARNING | MB_OK);
        } else if (lcfg_profile_save(g_client_dir, name, &g_s, err,
                                     sizeof err) != 0) {
            set_status_k(err, 2);
            ui_msgbox(g_hwnd, err, LT("gui.title_profsave"), MB_ICONERROR | MB_OK);
        } else {
            char st[256];
            _snprintf_s(st, sizeof st, _TRUNCATE, LT("gui.msg_prof_saved"), name);
            refresh_profiles();
            ui_settext(ctl(ID_PROF_COMBO), name);
            set_status_k(st, 1);
        }
        break;
    }
    case ID_PROF_DEL: {
        char name[128], err[1024], q[300];
        profile_name_from_ui(name, sizeof name);
        if (!name[0]) {
            set_status(LT("gui.msg_prof_none"));
            break;
        }
        _snprintf_s(q, sizeof q, _TRUNCATE, LT("gui.msg_prof_askdel"), name);
        if (ui_msgbox(g_hwnd, q, LT("gui.title_profdel"),
                      MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) == IDYES) {
            if (lcfg_profile_delete(g_client_dir, name, err, sizeof err) !=
                0) {
                set_status_k(err, 2);
                ui_msgbox(g_hwnd, err, LT("gui.title_profdel"),
                          MB_ICONERROR | MB_OK);
            } else {
                char st[256];
                _snprintf_s(st, sizeof st, _TRUNCATE, LT("gui.msg_prof_deleted"), name);
                ui_settext(ctl(ID_PROF_COMBO), "");
                refresh_profiles();
                set_status_k(st, 1);
            }
        }
        break;
    }
    case ID_PROF_DFLT: { /* W172/P7: Schalter "beim Launcher-Start laden" */
        char name[128], err[1024];
        profile_name_from_ui(name, sizeof name);
        if (ui_checked(ID_PROF_DFLT)) { /* aus: Markierung entfernen */
            lcfg_profile_set_default(g_client_dir, NULL, err, sizeof err);
            refresh_profiles();
            ui_settext(ctl(ID_PROF_COMBO), name);
            setup_auto_sync(0);
            set_status_k(LT("gui.msg_prof_nodefault"), 1);
        } else if (lcfg_profile_set_default(g_client_dir, name, err, sizeof err) !=
                   0) {
            set_status_k(err, 2);
            ui_msgbox(g_hwnd, err, LT("gui.prof_default"), MB_ICONWARNING | MB_OK);
        } else {
            char st[256];
            _snprintf_s(st, sizeof st, _TRUNCATE, LT("gui.msg_prof_isdefault"), name);
            refresh_profiles();
            ui_settext(ctl(ID_PROF_COMBO), name);
            setup_auto_sync(0);
            set_status_k(st, 1);
        }
        break;
    }
    }
}

static void on_dpi(int dpi)
{
    g_dpi = dpi;
    fonts_make();
    ui_relayout();
    base_build();
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_NCCALCSIZE:
        if (wp) {
            return 0; /* ganze Flaeche ist Client (randlos) */
        }
        break;
    case WM_NCACTIVATE:
        return DefWindowProcW(h, msg, wp, -1); /* keinen Rahmen malen */
    case WM_NCHITTEST: {
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(h, &p);
        if (p.y >= 0 && p.y < SX(TITLE_H)) {
            return HTCAPTION; /* Ziehen, Aero-Snap */
        }
        return HTCLIENT;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (g_bb.dc) {
            GdiFlush();
            BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top,
                   ps.rcPaint.right - ps.rcPaint.left,
                   ps.rcPaint.bottom - ps.rcPaint.top, g_bb.dc,
                   ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_PRINTCLIENT:
        if (g_bb.dc) {
            GdiFlush();
            BitBlt((HDC)wp, 0, 0, g_bb.w, g_bb.h, g_bb.dc, 0, 0, SRCCOPY);
        }
        return 0;
    case WM_DRAWITEM:
        draw_item((const DRAWITEMSTRUCT *)lp);
        return TRUE;
    case WM_MEASUREITEM: { /* W172: Packs-Liste (eigene Zeichnung) */
        MEASUREITEMSTRUCT *m = (MEASUREITEMSTRUCT *)lp;
        if (m->CtlType == ODT_LISTBOX) {
            m->itemHeight = (UINT)SX(PK_ROW_H);
            return TRUE;
        }
        break;
    }
    case WM_DROPFILES: /* W172: .sbpack/Pack-Ordner aus dem Explorer */
        packs_drop((HDROP)wp);
        return 0;
    case WM_NOTIFY: { /* W172: Tooltip-Text = Begruendung der Sperre */
        NMHDR *nh = (NMHDR *)lp;
        if (nh && nh->hwndFrom == g_tip && nh->code == TTN_GETDISPINFOW) {
            tips_dispinfo((NMTTDISPINFOW *)lp);
            return 0;
        }
        break;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORSTATIC:
        return ctlcolor((HDC)wp, (HWND)lp, msg == WM_CTLCOLORSTATIC);
    case WM_COMMAND:
        on_command(LOWORD(wp), HIWORD(wp));
        return 0;
    case DM_GETDEFID:
        return MAKELRESULT(ID_START_BIG, DC_HASDEFID);
    case WM_TIMER:
        if (wp == 1 && g_page == P_CTRL && update_pad_status()) {
            redraw_rect(CX0, CY0 + 392, COLW, 104);
        }
        return 0;
    case WM_DPICHANGED:
        if (!g_o.force_dpi) {
            RECT *r = (RECT *)lp;
            on_dpi(HIWORD(wp));
            SetWindowPos(h, NULL, r->left, r->top, r->right - r->left,
                         r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            redraw_all();
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        KillTimer(h, 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static int g_inited;

static int gui_init(const char *client_override)
{
    WNDCLASSW wc;
    INITCOMMONCONTROLSEX icc;
    if (g_inited) {
        return 1;
    }
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    memset(&icc, 0, sizeof icc);
    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_BAR_CLASSES; /* W143: + Trackbar */
    InitCommonControlsEx(&icc);
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"SbLauncher2";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    if (!RegisterClassW(&wc)) {
        return 0;
    }
    lcfg_find_client_dir(client_override, g_client_dir, sizeof g_client_dir,
                         &g_has_ldr);
    {
        wchar_t a[1024], b[1024];
        u8_to_w(g_client_dir, a, 1024);
        if (GetFullPathNameW(a, 1024, b, NULL) > 0) {
            w_to_u8(b, g_client_disp, (int)sizeof g_client_disp);
        } else {
            strcpy_s(g_client_disp, sizeof g_client_disp, g_client_dir);
        }
    }
    g_rem_pkg = rem_pkg_detect(g_client_disp, g_rem_dir, sizeof g_rem_dir);
    { /* W171: Hauptmonitor (R7 Vollbildaufloesung, R10 Bildwiederholrate) */
        DEVMODEW dm;
        memset(&dm, 0, sizeof dm);
        dm.dmSize = sizeof dm;
        if (EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm)) {
            g_mon_w = dm.dmPelsWidth;
            g_mon_h = dm.dmPelsHeight;
            g_disp_hz = dm.dmDisplayFrequency > 1 ? (int)dm.dmDisplayFrequency : 0;
        }
    }
    g_inited = 1;
    return 1;
}

/* Fenster erzeugen (unsichtbar). offscreen = Test-/Shot-Betrieb. */
static HWND gui_create(int offscreen)
{
    int x, y, W, H;
    DWORD exs = offscreen ? (WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW)
                          : WS_EX_APPWINDOW;
    if (g_o.force_dpi) {
        g_dpi = g_o.force_dpi;
    } else {
        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        typedef UINT(WINAPI * SysDpiFn)(void);
        SysDpiFn fn = u32 ? (SysDpiFn)GetProcAddress(u32, "GetDpiForSystem")
                          : NULL;
        g_dpi = fn ? (int)fn() : 96;
    }
    fonts_make();
    W = SX(WIN_W);
    H = SX(WIN_H);
    if (offscreen) {
        offscreen_pos(&x, &y);
    } else {
        RECT wa;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        x = wa.left + ((wa.right - wa.left) - W) / 2;
        y = wa.top + ((wa.bottom - wa.top) - H) / 2;
    }
    g_hwnd = CreateWindowExW(exs, L"SbLauncher2",
                             L"Steel Battalion \x2013 Launcher",
                             WS_POPUP | WS_CAPTION | WS_SYSMENU |
                                 WS_MINIMIZEBOX | WS_CLIPCHILDREN,
                             x, y, W, H, NULL, NULL, GetModuleHandleW(NULL),
                             NULL);
    if (!g_hwnd) {
        return NULL;
    }
    {
        MARGINS m = { 0, 0, 1, 0 };
        int corner = 2; /* DWMWCP_ROUND */
        BOOL dark = TRUE;
        DwmExtendFrameIntoClientArea(g_hwnd, &m);
        DwmSetWindowAttribute(g_hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */,
                              &corner, sizeof corner);
        DwmSetWindowAttribute(g_hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */,
                              &dark, sizeof dark);
    }
    SetWindowPos(g_hwnd, NULL, x, y, W, H,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    DragAcceptFiles(g_hwnd, TRUE); /* W172: Packs per Ziehen & Ablegen */
    if (!g_o.force_dpi) {
        /* Monitor-DPI des tatsaechlichen Platzes uebernehmen. */
        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        typedef UINT(WINAPI * WinDpiFn)(HWND);
        WinDpiFn fn = u32 ? (WinDpiFn)GetProcAddress(u32, "GetDpiForWindow")
                          : NULL;
        int d = fn ? (int)fn(g_hwnd) : 96;
        if (d >= 96 && d <= 576 && d != g_dpi) {
            g_dpi = d;
            fonts_make();
            W = SX(WIN_W);
            H = SX(WIN_H);
            SetWindowPos(g_hwnd, NULL, x, y, W, H,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    return g_hwnd;
}

/* W171: Regelzustaende fuer Shots (--ui-shot ... --state NAME, Konzept 8.4).
 * Wirkt nur auf die geladenen Werte im Speicher, nichts wird gespeichert. */
static const struct {
    const char *name;
    const char *set;
    int diag;
    int pksel; /* W172: gewaehltes Pack (Shots der Packs-Seite), 0 = erstes */
} kStates[] = {
    { "nodata", "game_image=;xbe_path=;media_root=", 0 },
    { "fullscreen", "window_mode=2;fs_hz=144;interp_fps=120", 0 },
    { "window43", "window_mode=0;width=1600;height=1200", 0 },
    { "controller", "sbc_usb=1;sbc_mouse_off=1;input_profile=", 0 },
    { "custom", "remaster=1;hd_textures=1;fx_enhanced=0;aa_mode=2;shading=0", 0 },
    { "diag", "audio=0", 1 },
    { "pack2", "", 0, 1 },  /* W172: zweites Pack gewaehlt (verdeckt durch ...) */
    { "pack3", "", 0, 2 },  /* W172: drittes Pack (Effekt-Pack mit Schloss) */
    /* W173: Remastered dezent, Effect Pack setzt color_sat/fx_embers (Effect-Pack-
     * Ausnahme: Anzeige bleibt "dezent", Schloss an Preset und Effekten) */
    { "effectpack", "shading=1;world_sharpen=60;aa_mode=3;ssao=25;color_sat=110;color_gain=103;"
                    "shade_shadow=1;fx_soft=60;fx_embers=160;fx_light=40;fx_light_cockpit=30;"
                    "fx_interp=1;remaster=1;hd_textures=1", 0 },
};

int gui_state_known(const char *name)
{
    int i;
    for (i = 0; i < (int)(sizeof kStates / sizeof kStates[0]); i++) {
        if (strcmp(name, kStates[i].name) == 0) {
            return 1;
        }
    }
    return 0;
}

static void gui_apply_state(const char *name)
{
    int i;
    for (i = 0; i < (int)(sizeof kStates / sizeof kStates[0]); i++) {
        if (strcmp(name, kStates[i].name) == 0) {
            lcfg_apply_set(&g_s, kStates[i].set);
            if (kStates[i].diag) {
                g_diag_open = 1;
            }
            g_pk_sel = kStates[i].pksel; /* W172 */
            return;
        }
    }
}

static void gui_fill(void)
{
    char note[256] = { 0 };
    int kind = 0;
    app_load_effective(g_client_dir, &g_s, note, sizeof note, &kind,
                       g_o.lang[0] ? g_o.lang : NULL);
    strcpy_s(g_note, sizeof g_note, note);
    g_note_kind = kind; /* W173 */
    if (kind == 2) {
        strcpy_s(g_profile_line, sizeof g_profile_line, note);
    } else if (kind == 1) {
        _snprintf_s(g_profile_line, sizeof g_profile_line, _TRUNCATE,
                    LT("gui.fill_writes"), note);
    } else {
        _snprintf_s(g_profile_line, sizeof g_profile_line, _TRUNCATE,
                    "%s", LT("gui.fill_nodefaults"));
    }
    if (g_o.state[0]) { /* W171: --state (nur Shots, nie gespeichert) */
        gui_apply_state(g_o.state);
    }
    build_controls();
    tips_build(); /* W172: Begruendungen als Tooltip */
    packs_load(); /* W171: Packs-Zustand fuer das Regelwerk (R17-R22/R29/R30) */
    update_sbc_status();
    update_vr_button(); /* W159: VR-Knopf je Runtime */
    ui_from_settings(&g_s);
    refresh_profiles();
    ui_set_checked(ID_BG, g_bg_on);
    update_pad_status();
    update_summary();
    ui_apply_rules();
    if (!g_has_ldr) {
        set_status_k(LT("gui.fill_noldr"), 2);
    } else { /* W173 LAUNCHER-P8: ohne Schluesselzahl (steht unter Diagnose) */
        set_status(g_note_kind == 2 ? g_note : LT("gui.fill_ready"));
    }
}

static void gui_teardown(void)
{
    if (g_hwnd) {
        DestroyWindow(g_hwnd);
        g_hwnd = NULL;
    }
    g_ui_n = 0;
    g_tip = NULL; /* W172: gehoert zum Fenster (mit zerstoert) */
    g_undo_on = 0; /* W171: Rueckgaengig gehoert zum Fenster */
    memset(g_rr, 0, sizeof g_rr);
    cv_free(&g_bb);
    cv_free(&g_base);
}

static void pump(void);

/* Shot: Fenster (eigener Prozess) per PrintWindow sichern. */
static int window_pixels(HWND w, canvas_t *out)
{
    RECT rc;
    int bw, bh;
    GetWindowRect(w, &rc);
    bw = rc.right - rc.left;
    bh = rc.bottom - rc.top;
    if (bw <= 0 || bh <= 0 || bw > 8192 || bh > 8192 || !cv_make(out, bw, bh)) {
        return 1;
    }
    {
        int tries;
        for (tries = 0; tries < 6; tries++) {
            int k, lit = 0, n = bw * bh;
            DwmFlush();
            if (!PrintWindow(w, out->dc, 2 /* PW_RENDERFULLCONTENT */)) {
                PrintWindow(w, out->dc, 0);
            }
            GdiFlush();
            /* Stichprobe: frisch gezeigte Fenster liefert DWM anfangs
             * schwarz -> warten und erneut sichern. */
            for (k = 0; k < n; k += 97) {
                if ((out->px[k] & 0xFFFFFF) > 0x080808) {
                    lit++;
                }
            }
            if (lit > n / 97 / 4) {
                return 0;
            }
            pump();
            Sleep(150);
        }
    }
    return 0;
}

static int bmp_save(const canvas_t *c, const char *path)
{
    BITMAPFILEHEADER bfh;
    BITMAPINFOHEADER bih;
    FILE *f = NULL;
    unsigned stride = (unsigned)c->w * 4;
    int ok;
    if (u8_fopen_s(&f, path, "wb") != 0 || !f) {
        return 1;
    }
    memset(&bih, 0, sizeof bih);
    bih.biSize = sizeof bih;
    bih.biWidth = c->w;
    bih.biHeight = -c->h;
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;
    memset(&bfh, 0, sizeof bfh);
    bfh.bfType = 0x4D42;
    bfh.bfOffBits = sizeof bfh + sizeof bih;
    bfh.bfSize = bfh.bfOffBits + stride * (unsigned)c->h;
    ok = fwrite(&bfh, sizeof bfh, 1, f) == 1 &&
         fwrite(&bih, sizeof bih, 1, f) == 1 &&
         fwrite(c->px, stride * (unsigned)c->h, 1, f) == 1;
    fclose(f);
    return ok ? 0 : 1;
}

static int shot_window(HWND w, const char *path, int png)
{
    canvas_t c;
    int rc;
    memset(&c, 0, sizeof c);
    if (!is_offscreen(w)) {
        printf("shot: FEHLER Fenster nicht au" U8_SZ "erhalb des "
               "Bildschirms - nichts gesichert\n");
        return 1;
    }
    if (window_pixels(w, &c) != 0) {
        return 1;
    }
    rc = png ? png_save(c.px, c.w, c.h, path) : bmp_save(&c, path);
    cv_free(&c);
    return rc;
}

static int bind_shot_save(HWND hd)
{
    char path[1200];
    int rc;
    UpdateWindow(hd);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s/w78_bind.bmp", g_o.out_dir);
    rc = shot_window(hd, path, 0);
    printf("bind-shot: %s%s\n", rc == 0 ? "" : "FEHLER ", path);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s/w82_bind_%d.png",
                g_o.out_dir, g_dpi * 100 / 96);
    rc |= shot_window(hd, path, 1);
    printf("bind-shot: %s%s\n", rc == 0 ? "" : "FEHLER ", path);
    return rc;
}

static void pump(void)
{
    /* nur eigene Nachrichten (eigener Thread), kein Fokuswechsel */
    MSG m;
    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

int gui_main(const char *client_override, const gui_opts_t *o)
{
    MSG msg;
    int rc = 0;
    memset(&msg, 0, sizeof msg);
    g_o = *o;
    if (!gui_init(client_override)) {
        ui_msgbox(NULL, LT("gui.err_noclass"), LT("gui.title_launcher"),
                  MB_ICONERROR | MB_OK);
        return 1;
    }
    g_bg_on = g_o.bg >= 0 ? g_o.bg : ui_cfg_load_bg(g_client_dir);
    g_diag_open = ui_cfg_load_key(g_client_dir, "diag_open", 0); /* W171 */
    if (!gui_create(g_o.shot != 0)) {
        ui_msgbox(NULL, LT("gui.err_nowindow"), LT("gui.title_launcher"),
                  MB_ICONERROR | MB_OK);
        return 1;
    }
    gui_fill();
    show_page(P_PLAY);
    if (g_o.shot == 4) {
        /* GUI-Startpfad ohne Clientstart (Abnahme: user.cfg-Vergleich). */
        rc = do_start(1, 0); /* W159: Trockenlauf = Bildschirmweg */
        printf("gui-start-dry: %s (%s)\n", rc == 0 ? "ok" : "FEHLER",
               g_status);
        if (rc == 0) { /* W171: Startparameter sichtbar (Vergleich vorher/nachher) */
            char cmd[2400];
            char xbe[1024]; /* W172/P6: im Ordner-Modus abgeleitet */
            lcfg_xbe_effective(&g_s, xbe, sizeof xbe);
            app_client_cmdline(g_client_dir, xbe, g_start_args, cmd,
                               sizeof cmd);
            printf("gui-start-dry: set=%s\ngui-start-dry: cmdline=%s\n",
                   g_start_args, cmd);
        }
        gui_teardown();
        return rc;
    }
    if (g_o.shot == 3) {
        ShowWindow(g_hwnd, SW_HIDE);
        open_bind_dialog();
        gui_teardown();
        return 0;
    }
    if (g_o.shot == 5) {
        /* W143: Assistenten-Shots (einzeln oder alle Modi). */
        static const char *const modes[7] = { "live", "test", "result",
                                              "cal", "lamps", "gamepad",
                                              "nodev" };
        ShowWindow(g_hwnd, SW_HIDE);
        if (strcmp(g_o.assist_mode, "alle") == 0) {
            int i;
            for (i = 0; i < 7; i++) {
                strcpy_s(g_o.assist_mode, sizeof g_o.assist_mode, modes[i]);
                assist_dialog_modal(g_o.assist_fixture[0]
                                        ? g_o.assist_fixture
                                        : NULL);
            }
        } else {
            assist_dialog_modal(g_o.assist_fixture[0] ? g_o.assist_fixture
                                                      : NULL);
        }
        gui_teardown();
        return 0;
    }
    if (g_o.shot == 1 || g_o.shot == 2) {
        int pass, p, nbg = (g_o.shot == 2 && g_o.bg_both) ? 2 : 1;
        if (!is_offscreen(g_hwnd)) {
            printf("shot: FEHLER Fenster nicht au" U8_SZ "erhalb des "
                   "Bildschirms - Abbruch\n");
            gui_teardown();
            return 1;
        }
        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
        printf("shot: dpi=%d (%d %%), Schrift '%ls', Symbole %s\n", g_dpi,
               g_dpi * 100 / 96, g_face_body, g_has_icons ? "ja" : "nein");
        for (pass = 0; pass < nbg; pass++) {
            if (nbg == 2) {
                g_bg_on = pass == 0;
                ui_set_checked(ID_BG, g_bg_on);
                base_build();
            }
            for (p = 0; p < P_N; p++) {
                char path[1200];
                int r;
                if (g_o.page >= 0 && g_o.page != p) {
                    continue;
                }
                show_page(p);
                pump();
                RedrawWindow(g_hwnd, NULL, NULL,
                             RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
                pump();
                Sleep(120);
                if (g_o.shot == 1) {
                    _snprintf_s(path, sizeof path, _TRUNCATE,
                                "%s/w47_shot_%d.bmp", g_o.out_dir, p);
                } else if (g_o.state[0]) { /* W171: Regelzustand im Namen */
                    _snprintf_s(path, sizeof path, _TRUNCATE,
                                "%s/w82_%s_%s_%s_%d.png", g_o.out_dir,
                                kPageKey[p], g_o.state, g_bg_on ? "bg" : "ohne",
                                g_dpi * 100 / 96);
                } else {
                    _snprintf_s(path, sizeof path, _TRUNCATE,
                                "%s/w82_%s_%s_%d.png", g_o.out_dir,
                                kPageKey[p], g_bg_on ? "bg" : "ohne",
                                g_dpi * 100 / 96);
                }
                r = shot_window(g_hwnd, path, g_o.shot == 2);
                printf("shot: Seite %d (%s) -> %s%s\n", p, kPageKey[p],
                       r ? "FEHLER " : "", path);
                rc |= r;
            }
        }
        gui_teardown();
        return rc;
    }
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);
    SetFocus(ctl(ID_START_BIG));
    SetTimer(g_hwnd, 1, 100, NULL); /* Gamepad-Live-Anzeige (10 Hz) */
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_TAB &&
            GetKeyState(VK_CONTROL) < 0) {
            show_page(nav_next(g_page, GetKeyState(VK_SHIFT) < 0 ? -1 : 1));
            continue;
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == 'Z' &&
            GetKeyState(VK_CONTROL) < 0 && g_undo_on) { /* W171 Muster C */
            on_command(ID_UNDO, BN_CLICKED);
            continue;
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
            /* Enter klickt den fokussierten eigenen Knopf (sonst
             * IsDialogMessage: Standard = Starten). */
            uictl_t *u = ui_by_hwnd(GetFocus());
            if (u && u->kind < K_EDIT && u->parent == g_hwnd &&
                IsWindowEnabled(u->h)) {
                on_command(u->id, BN_CLICKED);
                continue;
            }
        }
        if (IsDialogMessageW(g_hwnd, &msg)) {
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    g_hwnd = NULL;
    fonts_free();
    return (int)msg.wParam;
}

/* ------------------------------------------------------------------ */
/* Selbsttest (Faelle 20-25)                                            */
/* ------------------------------------------------------------------ */
#define GT_OK(name)                    \
    do {                               \
        printf("PASS %s\n", name);     \
        (*pass)++;                     \
    } while (0)
#define GT_FAIL(name, fmt, ...)                          \
    do {                                                 \
        printf("FAIL %s: " fmt "\n", name, __VA_ARGS__); \
        (*fail)++;                                       \
    } while (0)

static void write_file(const char *path, const char *text)
{
    FILE *f = NULL;
    if (u8_fopen_s(&f, path, "wb") == 0 && f) {
        fputs(text, f);
        fclose(f);
    }
}

/* W172: Umgebungsvariable (UTF-8) fuer die Selbsttests setzen. */
static void gt_setenv(const char *name, const char *v)
{
    wchar_t wn[64], wv[1100];
    u8_to_w(name, wn, 64);
    u8_to_w(v, wv, 1100);
    SetEnvironmentVariableW(wn, wv);
}

void gui_selftest(const char *client_override, int *pass, int *fail)
{
    char tmp[1024], dir[1100], p[1300], err[1024];
    /* Ordnername mit Umlauten und sz: "w82_uemlaeut_ss" */
    const char *uml = "w82_" U8_UE "ml" U8_AE "ut_" U8_SZ;
    u8_GetTempPath(tmp, sizeof tmp);

    /* 20: UTF-8-Pfade (Datei, Suche, Profile in Umlaut-Ordner) */
    {
        int ok = 1;
        char names[8][64];
        launcher_settings_t a, b;
        u8_find_t fd;
        _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s%s", tmp, uml);
        lcfg_mkdir_p(dir);
        _snprintf_s(p, sizeof p, _TRUNCATE, "%s/datei_%s.txt", dir, uml);
        write_file(p, "x");
        if (u8_GetFileAttributes(p) == INVALID_FILE_ATTRIBUTES) {
            ok = 0;
        }
        _snprintf_s(p, sizeof p, _TRUNCATE, "%s/datei_*.txt", dir);
        if (!u8_find_first(p, &fd)) {
            ok = 0;
        } else {
            char want[200];
            _snprintf_s(want, sizeof want, _TRUNCATE, "datei_%s.txt", uml);
            if (strcmp(fd.name, want) != 0) {
                ok = 0;
            }
            u8_find_close(&fd);
        }
        lcfg_defaults(&a);
        a.width = 1600;
        a.height = 900;
        if (lcfg_profile_save(dir, "u8test", &a, err, sizeof err) != 0 ||
            lcfg_profile_load(dir, "u8test", &b, err, sizeof err) != 0 ||
            memcmp(&a, &b, sizeof a) != 0 ||
            lcfg_profile_list(dir, names, 8) != 1) {
            ok = 0;
        }
        lcfg_profile_delete(dir, "u8test", NULL, 0);
        _snprintf_s(p, sizeof p, _TRUNCATE, "%s/datei_%s.txt", dir, uml);
        u8_DeleteFile(p);
        if (ok) {
            GT_OK("20 u8-paths");
        } else {
            GT_FAIL("20 u8-paths", "%s", "Umlaut-Pfad nicht lesbar/schreibbar");
        }
    }

    /* 21: cfg-Kodierung: Pfad mit Umlaut -> ANSI in der Datei, zurueck
     * UTF-8; nicht darstellbares Zeichen -> Kurzname oder Ablehnung. */
    {
        int ok = 1;
        char xbe[1300], cfg[1300], raw[4096];
        launcher_settings_t a, b;
        FILE *f = NULL;
        size_t n = 0;
        _snprintf_s(xbe, sizeof xbe, _TRUNCATE, "%s/default.xbe", dir);
        write_file(xbe, "XBEH");
        _snprintf_s(cfg, sizeof cfg, _TRUNCATE, "%sw82_acp_test.cfg", tmp);
        u8_DeleteFile(cfg);
        lcfg_defaults(&a);
        strcpy_s(a.xbe_path, sizeof a.xbe_path, xbe);
        if (lcfg_save_user(cfg, &a) != 0) {
            ok = 0;
        }
        memset(raw, 0, sizeof raw);
        if (u8_fopen_s(&f, cfg, "rb") == 0 && f) {
            n = fread(raw, 1, sizeof raw - 1, f);
            fclose(f);
        }
        (void)n;
        if (GetACP() == 1252 && !strstr(raw, "w82_\xFCml\xE4ut_\xDF")) {
            ok = 0; /* ANSI-Bytes (cp1252) erwartet, kein UTF-8 */
        }
        lcfg_defaults(&b);
        lcfg_load_file(cfg, &b);
        if (strcmp(a.xbe_path, b.xbe_path) != 0) {
            ok = 0;
        }
        u8_DeleteFile(cfg);
        {
            /* Kyrillisch (nicht in cp1252): Kurzname oder klare Ablehnung */
            char cy[1200], cyx[1300], acp[1200];
            int r;
            _snprintf_s(cy, sizeof cy, _TRUNCATE,
                        "%sw82_\xD0\x96\xD0\xB8\xD0\xBB", tmp);
            lcfg_mkdir_p(cy);
            _snprintf_s(cyx, sizeof cyx, _TRUNCATE, "%s/default.xbe", cy);
            write_file(cyx, "XBEH");
            r = u8_to_acp(cyx, acp, sizeof acp);
            if (r == 1) {
                if (GetFileAttributesA(acp) == INVALID_FILE_ATTRIBUTES) {
                    ok = 0; /* Kurzname muss per A-API oeffnen */
                }
            } else if (r == 2) {
                lcfg_defaults(&b);
                strcpy_s(b.xbe_path, sizeof b.xbe_path, cyx);
                if (lcfg_validate(&b, err, sizeof err) == 0 ||
                    !strstr(err, "Zeichen")) {
                    ok = 0;
                }
            } else if (GetACP() == 1252) {
                ok = 0; /* Kyrillisch kann in cp1252 nicht verlustfrei sein */
            }
            printf("     (21: Kyrillisch -> %s)\n",
                   r == 0 ? "ANSI" : r == 1 ? "8.3-Kurzname" : "abgelehnt");
            u8_DeleteFile(cyx);
            {
                wchar_t w[1200];
                u8_to_w(cy, w, 1200);
                RemoveDirectoryW(w);
            }
        }
        if (ok) {
            GT_OK("21 cfg-ansi");
        } else {
            GT_FAIL("21 cfg-ansi", "%.200s", raw);
        }
    }

    /* 22: Oberflaeche <-> Einstellungen (unsichtbares Fenster, nie gezeigt) */
    {
        launcher_settings_t a, b;
        int ok = 0;
        gui_opts_t o;
        memset(&o, 0, sizeof o);
        static lr_env_t fe; /* W171: feste Umgebung (Monitor, Packs, VR ...) */
        o.shot = 5; /* Test: keine Meldungsfenster */
        g_o = o;
        lrules_env_default(&fe, NULL);
        g_env_fix = &fe;
        g_confirm_auto = 1; /* R14-Rueckfrage: Ja */
        if (gui_init(client_override) && gui_create(1)) {
            build_controls();
            lcfg_defaults(&a);
            _snprintf_s(a.media_root, sizeof a.media_root, _TRUNCATE, "%s",
                        dir);
            _snprintf_s(a.xbe_path, sizeof a.xbe_path, _TRUNCATE,
                        "%s/default.xbe", dir);
            strcpy_s(a.hdd_root, sizeof a.hdd_root, "HDD_" U8_UE);
            a.width = 1920;
            a.height = 1080;
            a.fps_limit = 60;
            a.present_interval = 1;
            a.audio = 0;
            a.audio_live = 1;
            a.interp_fps = 60;
            a.window_mode = 2; /* W81 */
            a.fs_hz = 144;
            a.sbc_usb = 1;
            a.mouse_mode = 0; /* W87: vom Standard abweichend */
            a.mouse_sens = 7;
            a.remaster = 1; /* W103: Benutzerdefiniert */
            a.hd_textures = 1;
            a.hd_scale = 2;
            a.fx_enhanced = 0;
            a.ultrawide = 0; /* W173: Platzhalter schreibt der Launcher immer 0 */
            a.postfx = 0;
            a.monitor_grain = 0; /* W105: "Nur Signalstoerung" */
            a.monitor_glitch = 100;
            a.edge_mode = 2; /* W107: Abdunklung, vom Standard abweichend */
            a.edge_fade_width = 18;
            a.edge_fade_strength = 80;
            strcpy_s(a.input_profile, sizeof a.input_profile,
                     "user_gibts_nicht");
            ui_from_settings(&a);
            lcfg_defaults(&b);
            ui_to_settings(&b);
            { /* W172/P6: Programmdatei = <Ordner>/default.xbe -> Feld leer
               * (automatisch), wirksam bleibt derselbe Pfad */
                char ea[1200], eb[1200], fx[64];
                lcfg_xbe_effective(&a, ea, sizeof ea);
                lcfg_xbe_effective(&b, eb, sizeof eb);
                ui_gettext(ctl(ID_XBE), fx, sizeof fx);
                ok = path_eq(ea, eb) && fx[0] == '\0' && b.xbe_path[0] == '\0';
                if (!ok) {
                    printf("     (22: Programmdatei '%s' -> '%s', Feld '%s')\n", ea, eb, fx);
                }
            }
            ok = ok &&
                 strcmp(a.media_root, b.media_root) == 0 &&
                 strcmp(a.hdd_root, b.hdd_root) == 0 &&
                 strcmp(a.input_profile, b.input_profile) == 0 &&
                 a.width == b.width && a.height == b.height &&
                 a.present_interval == b.present_interval &&
                 a.fps_limit == b.fps_limit && a.audio == b.audio &&
                 a.audio_live == b.audio_live &&
                 a.aspect_mode == b.aspect_mode && a.sbc_usb == b.sbc_usb &&
                 a.interp_fps == b.interp_fps &&
                 a.window_mode == b.window_mode && a.fs_hz == b.fs_hz &&
                 a.mouse_mode == b.mouse_mode && a.mouse_sens == b.mouse_sens &&
                 a.remaster == b.remaster && a.hd_textures == b.hd_textures &&
                 a.hd_scale == b.hd_scale && a.fx_enhanced == b.fx_enhanced &&
                 a.ultrawide == b.ultrawide && a.postfx == b.postfx &&
                 a.edge_mode == b.edge_mode && a.edge_fade_width == b.edge_fade_width &&
                 a.edge_fade_strength == b.edge_fade_strength &&
                 IsWindowEnabled(ctl(ID_EDGE_W)) != FALSE &&
                 ui_checked(ID_GP0 + LGP_CUSTOM) && /* W173: HD ohne Remaster-Werte */
                 a.monitor_grain == b.monitor_grain && a.monitor_glitch == b.monitor_glitch;
            if (ok) { /* W114: nur 2x/4x; alt hd_scale=0 (W111 Auswahl) zeigt 4x */
                a.hd_scale = 0;
                ui_from_settings(&a);
                lcfg_defaults(&b);
                ui_to_settings(&b);
                ok = b.hd_scale == 4 && ComboBox_GetCurSel(ctl(ID_REM_SCALE)) == 1 &&
                     ComboBox_GetCount(ctl(ID_REM_SCALE)) == 2;
                a.hd_scale = 2;
                ui_from_settings(&a);
                if (!ok) { printf("     (22: Stufe alt 0 -> 4x falsch)\n"); }
            }
            if (ok) { /* W173 LAUNCHER-P4: Optionsfelder setzen alle Optik-Werte */
                char t[300];
                on_command(ID_GP0 + LGP_STRONG, BN_CLICKED); /* ueber Benutzerdefiniert: Ja */
                lcfg_defaults(&b);
                ui_to_settings(&b);
                ok = ui_checked(ID_GP0 + LGP_STRONG) && b.remaster == 1 && b.hd_textures == 1 &&
                     b.fx_enhanced == 1 && b.ultrawide == 0 && b.postfx == 0 && b.hd_scale == 2 &&
                     b.fx[LFX_EMBERS] == 120 && b.shx[LSHX_SOFT] == 1 && b.shx[LSHX_LABEL] == 0 &&
                     ComboBox_GetCurSel(ctl(ID_GFX_FX)) == LGP_STRONG && g_undo_on;
                if (ok) { /* Original: wirklich original (shading/fx/HD gesetzt) */
                    on_command(ID_GP0 + LGP_ORIGINAL, BN_CLICKED);
                    lcfg_defaults(&b);
                    ui_to_settings(&b);
                    ok = ui_checked(ID_GP0 + LGP_ORIGINAL) && b.remaster == 0 &&
                         b.hd_textures == 0 && b.fx_enhanced == 0 && b.shx[LSHX_SHADING] == 1 &&
                         b.fx[LFX_SOFT] == 0 && b.shx[LSHX_AA] == 0 &&
                         !ui_visible_on(ui_find(ID_GP_RESET), P_GFX);
                }
                if (ok) { /* Einzeloption -> Benutzerdefiniert (Basis Original) + Zuruecksetzen */
                    ComboBox_SetCurSel(ctl(ID_GFX_FX), LGP_SUBTLE);
                    on_command(ID_GFX_FX, CBN_SELCHANGE);
                    lcfg_defaults(&b);
                    ui_to_settings(&b);
                    ok = b.fx[LFX_SOFT] == 60 && b.fx_enhanced == 1 &&
                         ui_checked(ID_GP0 + LGP_CUSTOM) && g_undo_on &&
                         ui_visible_on(ui_find(ID_GP_RESET), P_GFX);
                    on_command(ID_GP_RESET, BN_CLICKED);
                    lcfg_defaults(&b);
                    ui_to_settings(&b);
                    ok = ok && ui_checked(ID_GP0 + LGP_ORIGINAL) && b.fx[LFX_SOFT] == 0;
                }
                if (ok) { /* Weltbeleuchtung aus -> Schatten grau (R15), Custom */
                    ComboBox_SetCurSel(ctl(ID_GFX_LIGHT), 1);
                    on_command(ID_GFX_LIGHT, CBN_SELCHANGE);
                    lcfg_defaults(&b);
                    ui_to_settings(&b);
                    ok = b.shx[LSHX_SHADING] == 0 && ui_checked(ID_GP0 + LGP_CUSTOM) &&
                         IsWindowEnabled(ctl(ID_GFX_SHADOW)) == FALSE &&
                         IsWindowEnabled(ctl(ID_GFX_SOFT)) == FALSE;
                    on_command(ID_GP_RESET, BN_CLICKED);
                    ok = ok && IsWindowEnabled(ctl(ID_GFX_SHADOW)) != FALSE &&
                         IsWindowEnabled(ctl(ID_GFX_SOFT)) == FALSE; /* R16: Schatten aus */
                }
                if (ok) { /* HUD-Etikett unabhaengig vom Preset (PO 7.11.) */
                    on_command(ID_REM_LABEL, BN_CLICKED);
                    ok = g_gfx.shx[LSHX_LABEL] == 1 && ui_checked(ID_GP0 + LGP_ORIGINAL);
                    on_command(ID_GP0 + LGP_STRONG, BN_CLICKED);
                    ok = ok && g_gfx.shx[LSHX_LABEL] == 1 && ui_checked(ID_GP0 + LGP_STRONG) &&
                         ui_checked(ID_REM_LABEL);
                    on_command(ID_REM_LABEL, BN_CLICKED);
                    ok = ok && g_gfx.shx[LSHX_LABEL] == 0;
                }
                if (ok) { /* Name nach echter Bildrate: exklusiv 144 Hz -> 144 FPS */
                    ui_gettext(ctl(ID_GP0), t, sizeof t);
                    ok = strstr(t, "144 FPS") != NULL;
                    if (!ok) { printf("     (22: Name '%s')\n", t); }
                }
                if (!ok) { printf("     (22: Grafik-Preset falsch)\n"); }
                ui_from_settings(&a);
            }
            if (ok) { /* W81: randlos + Roheintrag-Rate (von Hand gesetzt) */
                a.window_mode = 1;
                a.fs_hz = 85;
                ui_from_settings(&a);
                lcfg_defaults(&b);
                ui_to_settings(&b);
                ok = b.window_mode == 1 && b.fs_hz == 85 &&
                     IsWindowEnabled(ctl(ID_FSHZ)) == FALSE;
            }
            if (ok) { /* W107: Roheintraege (von Hand) + harter Schnitt sperrt Verlauf */
                a.edge_mode = 1;
                a.edge_fade_width = 33;
                a.edge_fade_strength = 45;
                ui_from_settings(&a);
                lcfg_defaults(&b);
                ui_to_settings(&b);
                ok = b.edge_mode == 1 && b.edge_fade_width == 33 &&
                     b.edge_fade_strength == 45 &&
                     IsWindowEnabled(ctl(ID_EDGE_W)) == FALSE &&
                     IsWindowEnabled(ctl(ID_EDGE_S)) == FALSE;
                if (ok) { /* Auswahl Abdunklung gibt den Verlauf frei */
                    ComboBox_SetCurSel(ctl(ID_EDGE), 2);
                    on_command(ID_EDGE, CBN_SELCHANGE);
                    lcfg_defaults(&b);
                    ui_to_settings(&b);
                    ok = b.edge_mode == 2 && IsWindowEnabled(ctl(ID_EDGE_W)) != FALSE &&
                         IsWindowEnabled(ctl(ID_EDGE_S)) != FALSE;
                }
                if (!ok) { printf("     (22: Bildraender falsch)\n"); }
            }
            if (ok) { /* W170 LAUNCHER-P1/B4+B7+B19: Schalter ohne Wirkung sind
                 * Infozeilen (ausgeblendet), Standard 10 % ist Listeneintrag. */
                uictl_t *as = ui_find(ID_ASPECT); /* W173: Platzhalter-Schalter entfernt */
                ok = as && !ui_visible_on(as, P_DISPLAY);
                a.edge_mode = 2;
                a.edge_fade_width = 10;
                a.edge_fade_strength = 100;
                ui_from_settings(&a);
                lcfg_defaults(&b);
                ui_to_settings(&b);
                ok = ok && ComboBox_GetCount(ctl(ID_EDGE_W)) == KEDGEW_N &&
                     b.edge_fade_width == 10;
                if (!ok) { printf("     (22: B4/B7/B19 falsch)\n"); }
            }
            if (ok) { /* W170 LAUNCHER-P1/B10: Maus-Schalter folgt dem Controller */
                a.sbc_usb = 0;
                ui_from_settings(&a);
                ok = IsWindowEnabled(ctl(ID_CTRL_MOUSEOFF)) == FALSE;
                on_command(ID_CTRL_SBC, 0); /* Controller an -> Schalter frei */
                ok = ok && IsWindowEnabled(ctl(ID_CTRL_MOUSEOFF)) != FALSE;
                lcfg_defaults(&b);
                ui_to_settings(&b);
                ok = ok && b.sbc_usb == 1;
                if (!ok) { printf("     (22: Maus-Schalter folgt Controller nicht)\n"); }
            }
            if (ok) { /* W159: Kombi = Vorschau-Art; vr_enable kommt nicht aus der UI */
                strcpy_s(a.vr_preview, sizeof a.vr_preview, "sbs");
                a.vr_enable = 1; /* alte user.cfg: Startweg unberuehrt */
                ui_from_settings(&a);
                lcfg_defaults(&b);
                ui_to_settings(&b);
                ok = ComboBox_GetCurSel(ctl(ID_VR)) == 0 &&
                     strcmp(b.vr_preview, "sbs") == 0 && b.vr_enable == 0;
                strcpy_s(a.vr_preview, sizeof a.vr_preview, "right");
                ui_from_settings(&a);
                lcfg_defaults(&b);
                ui_to_settings(&b);
                ok = ok && ComboBox_GetCurSel(ctl(ID_VR)) == 2 &&
                     strcmp(b.vr_preview, "right") == 0;
                if (!ok) { printf("     (22: VR-Vorschau falsch)\n"); }
            }
            gui_teardown();
        }
        g_env_fix = NULL;
        g_confirm_auto = 0;
        if (ok) {
            GT_OK("22 ui-roundtrip");
        } else {
            GT_FAIL("22 ui-roundtrip", "%s", "Werte nach UI ungleich");
        }
    }

    /* 43-45 (W171 LAUNCHER-P2P3): Regelwerk in der Oberflaeche (je Regel
     * Fixture -> erwarteter UI-Zustand), 7-Seiten-Navigation + Aliase,
     * Tab-Reihenfolge je Seite (Fenster ausserhalb des Bildschirms, ohne
     * Fokus, wie --ui-shot). */
    {
        static lr_env_t fe;
        gui_opts_t o;
        int ok43 = 0, ok44 = 0, ok45 = 0;
        char why43[200] = "", why44[200] = "", why45[300] = "";
        memset(&o, 0, sizeof o);
        o.shot = 5;
        g_o = o;
        if (gui_init(client_override) && gui_create(1)) {
            int rule, v, i, k, p;
            build_controls();
            ok43 = 1;
            for (rule = LR_FIRST; rule <= LR_LAST && ok43; rule++) {
                for (v = 1; v >= 0 && ok43; v--) {
                    launcher_settings_t s;
                    if (v == 0 && lrules_always(rule)) {
                        continue;
                    }
                    lcfg_defaults(&s);
                    lrules_env_default(&fe, &s);
                    lrules_fixture(rule, v, &s, &fe);
                    g_env_fix = &fe;
                    ui_from_settings(&s); /* ruft ui_apply_rules */
                    if (g_rr[rule].active != v) {
                        ok43 = 0;
                        _snprintf_s(why43, sizeof why43, _TRUNCATE,
                                    "R%d v=%d: Regel greift=%d", rule, v,
                                    g_rr[rule].active);
                        break;
                    }
                    for (i = 0; i < KRULECTL_N && ok43; i++) {
                        if (kRuleCtl[i].rule != rule) {
                            continue;
                        }
                        for (k = 0; k < 5 && kRuleCtl[i].ids[k]; k++) {
                            int want = !(v && lrules_locks(rule));
                            int en = IsWindowEnabled(ctl(kRuleCtl[i].ids[k])) != FALSE;
                            if (en != want) {
                                ok43 = 0;
                                _snprintf_s(why43, sizeof why43, _TRUNCATE,
                                            "R%d v=%d: Element %d frei=%d, erwartet %d",
                                            rule, v, kRuleCtl[i].ids[k], en, want);
                                break;
                            }
                        }
                    }
                }
            }
            /* Muster C: Voreinstellung ueber Benutzerdefiniert -> Rueckfrage
             * (Nein = bleibt), Ja = ersetzt + Rueckgaengig stellt her. */
            if (ok43) {
                launcher_settings_t s, b;
                lcfg_defaults(&s);
                lrules_env_default(&fe, &s);
                g_env_fix = &fe;
                s.shx[LSHX_SAT] = 77; /* Benutzerdefiniert (W173: ein Preset) */
                ui_from_settings(&s);
                g_confirm_auto = 0;
                on_command(ID_GP0 + LGP_SUBTLE, BN_CLICKED);
                ok43 = ui_checked(ID_GP0 + LGP_CUSTOM) && g_gfx.shx[LSHX_SAT] == 77 && !g_undo_on;
                g_confirm_auto = 1;
                on_command(ID_GP0 + LGP_SUBTLE, BN_CLICKED);
                ok43 = ok43 && ui_checked(ID_GP0 + LGP_SUBTLE) && g_undo_on;
                on_command(ID_UNDO, BN_CLICKED);
                lcfg_defaults(&b);
                ui_to_settings(&b);
                ok43 = ok43 && !g_undo_on && b.shx[LSHX_SAT] == 77 &&
                       ui_checked(ID_GP0 + LGP_CUSTOM);
                g_confirm_auto = 0;
                /* W173: Effect-Pack-Ausnahme - der vom Pack gesetzte Wert zaehlt
                 * nicht (Anzeige Original, keine R14-Rueckfrage) */
                lcfg_gfx_lock_key(&fe.gfx_lock, "color_sat");
                ui_from_settings(&s);
                ok43 = ok43 && ui_checked(ID_GP0 + LGP_ORIGINAL) && !g_rr[14].active;
                fe.gfx_lock = 0;
                if (!ok43) {
                    strcpy_s(why43, sizeof why43, "Muster C (R14 Rueckfrage/Undo)");
                }
            }
            g_env_fix = NULL;
            /* 44: sieben Seiten, Aliase, Strg+Tab-Zyklus, jede Seite hat
             * sichtbare Elemente */
            {
                static const struct { const char *n; int p; } al[] = {
                    { "play", P_PLAY }, { "display", P_DISPLAY }, { "graphics", P_GFX },
                    { "packs", P_PACKS }, { "controls", P_CTRL }, { "audio", P_AUDIO },
                    { "advanced", P_ADV }, { "start", P_PLAY }, { "grafik", P_DISPLAY },
                    { "remaster", P_GFX }, { "ton", P_AUDIO }, { "steuerung", P_CTRL },
                    { "profile", P_ADV }, { "pfade", P_ADV }, { "6", P_ADV },
                    { "7", -1 }, { "xyz", -1 },
                };
                int seen[P_N] = { 0 }, cur = P_PLAY, n;
                ok44 = gui_page_count() == 7;
                for (i = 0; i < (int)(sizeof al / sizeof al[0]); i++) {
                    if (gui_page_from_name(al[i].n) != al[i].p) {
                        ok44 = 0;
                        _snprintf_s(why44, sizeof why44, _TRUNCATE, "Alias '%s'", al[i].n);
                    }
                }
                for (n = 0; n < P_N; n++) {
                    seen[cur] = 1;
                    cur = nav_next(cur, 1);
                }
                for (n = 0; n < P_N; n++) {
                    ok44 = ok44 && seen[n];
                }
                ok44 = ok44 && cur == P_PLAY && nav_next(P_PLAY, -1) == P_ADV;
                for (p = 0; p < P_N && ok44; p++) {
                    int vis = 0;
                    for (i = 0; i < g_ui_n; i++) {
                        if (g_ui[i].parent == g_hwnd && g_ui[i].page == p &&
                            ui_visible_on(&g_ui[i], p)) {
                            vis++;
                        }
                    }
                    if (vis == 0) {
                        ok44 = 0;
                        _snprintf_s(why44, sizeof why44, _TRUNCATE,
                                    "Seite %s ohne Elemente", kPageKey[p]);
                    }
                }
                if (!ok44 && !why44[0]) {
                    strcpy_s(why44, sizeof why44, "Seitenzahl/Zyklus");
                }
            }
            /* 45: Tab-Reihenfolge: jedes sichtbare, freie Element mit
             * Tabstopp ist per Tab erreichbar (Kreis), Reihenfolge = Anlage
             * (build_controls: Seite oben links -> unten, dann rechts). */
            if (is_offscreen(g_hwnd)) {
                ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
                ok45 = 1;
                for (p = 0; p < P_N && ok45; p++) {
                    HWND h0, h;
                    int want = 0, got = 0, steps, last = -1, order_ok = 1;
                    char ord[600] = "";
                    show_page(p);
                    pump();
                    for (i = 0; i < g_ui_n; i++) {
                        uictl_t *u = &g_ui[i];
                        if (u->parent == g_hwnd && ui_visible_on(u, p) &&
                            (GetWindowLongW(u->h, GWL_STYLE) & WS_TABSTOP) &&
                            IsWindowEnabled(u->h)) {
                            want++;
                        }
                    }
                    h0 = GetNextDlgTabItem(g_hwnd, NULL, FALSE);
                    h = h0;
                    for (steps = 0; h && steps < 200; steps++) {
                        uictl_t *u = ui_by_hwnd(h);
                        if (!u) {
                            u = ui_by_hwnd(GetParent(h));
                        }
                        if (u && u->parent == g_hwnd) {
                            int idx = (int)(u - g_ui);
                            size_t ol = strlen(ord);
                            got++;
                            if (u->page == p) {
                                if (idx < last) {
                                    order_ok = 0;
                                }
                                last = idx;
                            }
                            _snprintf_s(ord + ol, sizeof ord - ol, _TRUNCATE,
                                        "%s%d", ol ? "," : "", u->id);
                        }
                        h = GetNextDlgTabItem(g_hwnd, h, FALSE);
                        if (h == h0) {
                            break;
                        }
                    }
                    printf("     (45: %s: %d Tab-Stopps: %s)\n", kPageKey[p], got, ord);
                    if (got != want || !order_ok) {
                        ok45 = 0;
                        _snprintf_s(why45, sizeof why45, _TRUNCATE,
                                    "Seite %s: %d erreichbar, %d erwartet, Reihenfolge %s",
                                    kPageKey[p], got, want, order_ok ? "ok" : "falsch");
                    }
                }
                ShowWindow(g_hwnd, SW_HIDE);
            } else {
                strcpy_s(why45, sizeof why45, "Fenster nicht ausserhalb des Bildschirms");
            }
            gui_teardown();
        }
        g_env_fix = NULL;
        if (ok43) {
            GT_OK("43 rules-ui (R1-R32 Fixture -> Freigabe/Begruendung, Muster C)");
        } else {
            GT_FAIL("43 rules-ui", "%s", why43);
        }
        if (ok44) {
            GT_OK("44 nav-7pages (Aliase, Strg+Tab-Zyklus)");
        } else {
            GT_FAIL("44 nav-7pages", "%s", why44);
        }
        if (ok45) {
            GT_OK("45 tab-order");
        } else {
            GT_FAIL("45 tab-order", "%s", why45);
        }
    }

    /* 48-51 (W172 LAUNCHER-P567): Spielen (Einrichtungskarte, Sprunglinks),
     * Packs-Seite (Liste, Reihenfolge, Konflikte, Grenzen, abgelehnt,
     * leer, Hinzufuegen/Entfernen, B5), Erweitert (Datenort wie --userdata,
     * Schalter "beim Start laden"), Tooltips = Begruendung. Fenster ausserhalb
     * des Bildschirms, ohne Fokus; Test-Packs im Temp-Ordner (keine
     * Spieldaten). */
    {
        static lr_env_t fe;
        gui_opts_t o;
        int ok48 = 0, ok49 = 0, ok50 = 0, ok51 = 0;
        char why48[200] = "", why49[240] = "", why50[200] = "", why51[200] = "";
        char pkd[1100], pke[1100], q[1300], senv[1100];
        int had_env = u8_GetEnv("SB_PACKS_DIR", senv, sizeof senv) > 0;
        memset(&o, 0, sizeof o);
        o.shot = 5;
        g_o = o;
        _snprintf_s(pkd, sizeof pkd, _TRUNCATE, "%sw172_gui_packs", tmp);
        _snprintf_s(pke, sizeof pke, _TRUNCATE, "%sw172_gui_empty", tmp);
        lcfg_mkdir_p(pke);
        { /* Test-Packs: ta + tb (gleicher Hash -> Konflikt), Effekt-Pack fx,
           * kaputtes Pack bad (ohne manifest.json -> abgelehnt) */
            static const char *const nm[2] = { "ta", "tb" };
            static const char *const cat[2] = { "font_hud", "effect" };
            char z[65];
            int k;
            memset(z, '0', 64);
            z[64] = '\0';
            for (k = 0; k < 2; k++) {
                char m[2000];
                _snprintf_s(q, sizeof q, _TRUNCATE, "%s/%s", pkd, nm[k]);
                lcfg_mkdir_p(q);
                _snprintf_s(q, sizeof q, _TRUNCATE, "%s/%s/a.dds", pkd, nm[k]);
                write_file(q, "DDS ");
                _snprintf_s(m, sizeof m, _TRUNCATE,
                    "{ \"format\": \"sbpack\", \"format_version\": 1,\n"
                    "  \"id\": \"t.%s\", \"name\": \"Test %s\", \"version\": \"1.0.0\",\n"
                    "  \"author\": \"W172\", \"license\": \"CC0\", \"type\": \"texture\",\n"
                    "  \"hash_algo\": \"sbt1-fnv64\", \"game\": { \"xbe_sha256\": [\"%s\"] },\n"
                    "  \"entries\": [ { \"hash\": \"1111111111111111\", \"category\": \"%s\",\n"
                    "    \"orig\": { \"w\": 2, \"h\": 2 }, \"variants\": { \"x2\": { \"file\": \"a.dds\",\n"
                    "    \"w\": 4, \"h\": 4, \"fmt\": \"BC1\", \"mips\": 1, \"sha256\": \"%s\" } } }%s ] }",
                    nm[k], nm[k], z, cat[k], z,
                    k ? ",\n  { \"hash\": \"2222222222222222\", \"category\": \"cockpit\",\n"
                        "    \"orig\": { \"w\": 2, \"h\": 2 }, \"variants\": { \"x2\": { \"file\": \"a.dds\",\n"
                        "    \"w\": 4, \"h\": 4, \"fmt\": \"BC1\", \"mips\": 1, \"sha256\": \"" : "");
                if (k) { /* zweiten Eintrag schliessen */
                    size_t L = strlen(m);
                    _snprintf_s(m + L - 4, sizeof m - L + 4, _TRUNCATE,
                                "%s\" } } } ] }", z);
                }
                _snprintf_s(q, sizeof q, _TRUNCATE, "%s/%s/manifest.json", pkd, nm[k]);
                write_file(q, m);
            }
            _snprintf_s(q, sizeof q, _TRUNCATE, "%s/fx", pkd);
            lcfg_mkdir_p(q);
            {
                char m[1200];
                _snprintf_s(m, sizeof m, _TRUNCATE,
                    "{ \"format\": \"sbpack\", \"format_version\": 1,\n"
                    "  \"id\": \"t.fx\", \"name\": \"Test FX\", \"version\": \"0.3.0\",\n"
                    "  \"author\": \"W172\", \"license\": \"CC0\", \"type\": \"effect\",\n"
                    "  \"hash_algo\": \"sbt1-fnv64\", \"game\": { \"xbe_sha256\": [\"%s\"] },\n"
                    "  \"entries\": [] }", z);
                _snprintf_s(q, sizeof q, _TRUNCATE, "%s/fx/manifest.json", pkd);
                write_file(q, m);
                _snprintf_s(q, sizeof q, _TRUNCATE, "%s/fx/params.json", pkd);
                write_file(q, "{ \"params\": { \"fx_light\": 40, \"color_sat\": 110 } }");
            }
            _snprintf_s(q, sizeof q, _TRUNCATE, "%s/bad", pkd);
            lcfg_mkdir_p(q);
            _snprintf_s(q, sizeof q, _TRUNCATE, "%s/packs.json", pkd);
            write_file(q, "{ \"version\": 1, \"order\": [\n"
                          "  { \"id\": \"t.ta\", \"file\": \"ta\" },\n"
                          "  { \"id\": \"t.tb\", \"file\": \"tb\" },\n"
                          "  { \"id\": \"t.fx\", \"file\": \"fx\" },\n"
                          "  { \"id\": \"bad\", \"file\": \"bad\" } ],\n"
                          "  \"level\": \"x4\" }");
        }
        if (gui_init(client_override) && gui_create(1)) {
            launcher_settings_t s, b;
            int i;
            build_controls();
            tips_build();
            ShowWindow(g_hwnd, SW_SHOWNOACTIVATE); /* ausserhalb des Bildschirms */
            /* --- 48: Spielen --- */
            gt_setenv("SB_PACKS_DIR", pke);
            packs_load();
            lcfg_defaults(&s);
            lrules_env_default(&fe, &s);
            lrules_fixture(3, 1, &s, &fe); /* keine Spieldaten */
            g_env_fix = &fe;
            show_page(P_PLAY);
            ui_from_settings(&s);
            ok48 = play_setup_shown() && ui_visible_on(ui_find(ID_IMAGE), P_PLAY) &&
                   ui_visible_on(ui_find(ID_PLAY_FOLDER), P_PLAY) &&
                   !ui_visible_on(ui_find(ID_SUM_GO0), P_PLAY);
            if (!ok48) {
                strcpy_s(why48, sizeof why48, "nodata: keine Einrichtungskarte");
            }
            if (ok48) { /* gueltige Daten -> Bereitschaft mit 5 Sprunglinks */
                fe.data_ok = 1;
                strcpy_s(s.game_image, sizeof s.game_image, "C:/spiel.xiso");
                show_page(P_PLAY);
                ui_from_settings(&s);
                ok48 = !play_setup_shown() && !ui_visible_on(ui_find(ID_IMAGE), P_PLAY);
                for (i = 0; i < SUM_GO_N && ok48; i++) {
                    ok48 = ui_visible_on(ui_find(ID_SUM_GO0 + i), P_PLAY) &&
                           g_sum[i].label && g_sum[i].value[0];
                }
                if (!ok48) {
                    strcpy_s(why48, sizeof why48, "Bereitschaft/Sprunglinks fehlen");
                }
            }
            if (ok48) { /* Sprung zur Ursache; Spiel-Zeile: Abbild -> Karte */
                on_command(ID_SUM_GO0 + 3, BN_CLICKED);
                ok48 = g_page == P_PACKS;
                show_page(P_PLAY);
                on_command(ID_SUM_GO0 + 2, BN_CLICKED);
                ok48 = ok48 && g_page == P_DISPLAY;
                show_page(P_PLAY);
                on_command(ID_SUM_GO0, BN_CLICKED);
                ok48 = ok48 && g_page == P_PLAY && play_setup_shown() &&
                       ui_visible_on(ui_find(ID_IMAGE), P_PLAY);
                s.game_image[0] = '\0'; /* Ordner-Modus -> Erweitert */
                strcpy_s(s.media_root, sizeof s.media_root, dir);
                show_page(P_PLAY);
                ui_from_settings(&s);
                on_command(ID_SUM_GO0, BN_CLICKED);
                ok48 = ok48 && g_page == P_ADV;
                show_page(P_PLAY);
                on_command(ID_PLAY_FOLDER, BN_CLICKED);
                ok48 = ok48 && g_page == P_ADV;
                if (!ok48) {
                    strcpy_s(why48, sizeof why48, "Sprunglinks springen falsch");
                }
            }
            /* --- 51: Tooltips = Begruendung der Sperre (Muster A) --- */
            {
                char tt[400];
                lcfg_defaults(&s);
                lrules_env_default(&fe, &s);
                lrules_fixture(7, 1, &s, &fe); /* Vollbild -> Aufloesung gesperrt */
                show_page(P_DISPLAY);
                ui_from_settings(&s);
                tip_text(ID_W, tt, sizeof tt);
                ok51 = g_rr[7].disable && !IsWindowEnabled(ctl(ID_W)) &&
                       tt[0] && strcmp(tt, g_rr[7].text) == 0;
                lrules_fixture(7, 0, &s, &fe);
                ui_from_settings(&s);
                tip_text(ID_W, tt, sizeof tt);
                ok51 = ok51 && tt[0] == '\0';
                lrules_fixture(23, 1, &s, &fe); /* Maus-aus ohne Controller */
                show_page(P_CTRL);
                ui_from_settings(&s);
                tip_text(ID_CTRL_MOUSEOFF, tt, sizeof tt);
                ok51 = ok51 && strcmp(tt, g_rr[23].text) == 0;
                show_page(P_DISPLAY); /* unsichtbar -> kein Tooltip */
                tip_text(ID_CTRL_MOUSEOFF, tt, sizeof tt);
                ok51 = ok51 && tt[0] == '\0' && g_tip != NULL;
                if (!ok51) {
                    _snprintf_s(why51, sizeof why51, _TRUNCATE, "Tooltip '%.80s'", tt);
                }
            }
            g_env_fix = NULL;
            /* --- 49: Packs-Seite --- */
            gt_setenv("SB_PACKS_DIR", pkd);
            g_pk_sel = 0;
            show_page(P_PACKS);
            ok49 = g_pk_n == 4 &&
                   (int)SendMessageW(ctl(ID_PK_LIST), LB_GETCOUNT, 0, 0) == 4 &&
                   packs_status_of(0) == 2 && packs_status_of(3) == 0 &&
                   packs_is_fx(2) && g_pk_covby[1 * PK_MAXN + 0] == 1 &&
                   g_pk_cov[1].cat[5] == 1 && g_pk_cov[1].cat[1] == 1 &&
                   g_pk_fx_hit[2] && g_pk_fx_val[2] == 40 &&
                   ui_visible_on(ui_find(ID_PK_LIST), P_PACKS) &&
                   !ui_visible_on(ui_find(ID_PK_CREATOR), P_PACKS);
            if (!ok49) {
                _snprintf_s(why49, sizeof why49, _TRUNCATE,
                            "Laden: n=%d st=%d/%d fx=%d covby=%d cat=%d/%d fxhit=%d/%d",
                            g_pk_n, packs_status_of(0), packs_status_of(3), packs_is_fx(2),
                            g_pk_covby[PK_MAXN], g_pk_cov[1].cat[5], g_pk_cov[1].cat[1],
                            g_pk_fx_hit[2], g_pk_fx_val[2]);
            }
            if (ok49) { /* Alt+Pfeil runter: ta <-> tb, packs.json + Konflikt neu */
                sb_pack_order_t ord[8];
                char lv[8], pj[1200];
                ListBox_SetCurSel(ctl(ID_PK_LIST), 0);
                g_pk_sel = 0;
                SendMessageW(ctl(ID_PK_LIST), WM_SYSKEYDOWN, VK_DOWN, 0);
                _snprintf_s(pj, sizeof pj, _TRUNCATE, "%s/packs.json", pkd);
                ok49 = sb_pack_read_cfg(pj, ord, 8, lv, sizeof lv, NULL, 0, NULL, 0) == 4 &&
                       strcmp(ord[0].id, "t.tb") == 0 && strcmp(ord[1].id, "t.ta") == 0 &&
                       g_pk_sel == 1 && g_pk_covby[1 * PK_MAXN + 0] == 1 &&
                       g_pk_covby[0 * PK_MAXN + 1] == 0;
                packs_move_to(1, 0); /* Ziehen-Ziel: zurueck nach oben */
                ok49 = ok49 && strcmp(g_pk_ord[0].id, "t.ta") == 0 && g_pk_sel == 0;
                if (!ok49) {
                    strcpy_s(why49, sizeof why49, "Reihenfolge (Alt+Pfeil/Ziehen)");
                }
            }
            if (ok49) { /* Klick auf den Schalter: aus; Leertaste: wieder an */
                int ih = (int)SendMessageW(ctl(ID_PK_LIST), LB_GETITEMHEIGHT, 0, 0);
                SendMessageW(ctl(ID_PK_LIST), WM_LBUTTONDOWN, MK_LBUTTON,
                             MAKELPARAM(SX(20), ih + ih / 2));
                ok49 = g_pk_ord[1].enabled == 0 && g_pk_sel == 1;
                SendMessageW(ctl(ID_PK_LIST), WM_KEYDOWN, VK_SPACE, 0);
                ok49 = ok49 && g_pk_ord[1].enabled == 1;
                if (!ok49) {
                    strcpy_s(why49, sizeof why49, "Schalter (Klick/Leertaste)");
                }
            }
            if (ok49) { /* abgelehnt: aus geht, an nicht (R21, Klartext-Grund) */
                ListBox_SetCurSel(ctl(ID_PK_LIST), 3);
                on_command(ID_PK_LIST, LBN_SELCHANGE);
                on_command(ID_PK_TOGGLE, BN_CLICKED); /* aus */
                ok49 = g_pk_ord[3].enabled == 0 && g_rr[21].active &&
                       !IsWindowEnabled(ctl(ID_PK_TOGGLE)) &&
                       strstr(g_rr[21].text, LT("gui.pkv_missing")) != NULL;
                packs_toggle_idx(3); /* an -> verweigert */
                ok49 = ok49 && g_pk_ord[3].enabled == 0 && g_status_kind == 2;
                if (!ok49) {
                    _snprintf_s(why49, sizeof why49, _TRUNCATE, "abgelehnt: '%.80s'",
                                g_rr[21].text);
                }
            }
            if (ok49) { /* Texturdetail-Segment + Grenzen (nur senken, effects+effect) */
                char lv[8], pj[1200];
                sb_pack_order_t ord[8];
                sb_pack_cap_t caps[8];
                int ncap;
                memset(caps, 0, sizeof caps);
                on_command(ID_PK_LVL2, BN_CLICKED);
                lcfg_defaults(&b);
                ui_to_settings(&b);
                ok49 = strcmp(g_pk_level, "x2") == 0 && b.hd_scale == 2 &&
                       ui_checked(ID_PK_LVL2) && !ui_checked(ID_PK_LVL4) &&
                       ComboBox_GetCount(ctl(ID_PK_CAT0)) == 2;
                ComboBox_SetCurSel(ctl(ID_PK_CAT0 + 5), 0); /* Effekte: Original */
                on_command(ID_PK_CAT0 + 5, CBN_SELCHANGE);
                on_command(ID_PK_LVL4, BN_CLICKED);
                _snprintf_s(pj, sizeof pj, _TRUNCATE, "%s/packs.json", pkd);
                sb_pack_read_cfg(pj, ord, 8, lv, sizeof lv, caps, 8, NULL, 0);
                for (ncap = 0; ncap < 8 && caps[ncap].name[0]; ncap++) {
                }
                ok49 = ok49 && strcmp(lv, "x4") == 0 && ncap == 2 &&
                       packs_cap_stored(5) == 0 && packs_cap_of(0) == 4 &&
                       ComboBox_GetCount(ctl(ID_PK_CAT0)) == 3 &&
                       ComboBox_GetCurSel(ctl(ID_PK_CAT0 + 5)) == 0;
                ComboBox_SetCurSel(ctl(ID_PK_CAT0), 1); /* HUD: 2x */
                on_command(ID_PK_CAT0, CBN_SELCHANGE);
                ok49 = ok49 && packs_cap_stored(0) == 2 && packs_cap_of(0) == 2;
                ComboBox_SetCurSel(ctl(ID_PK_CAT0), 2); /* HUD: 4x = keine Grenze */
                on_command(ID_PK_CAT0, CBN_SELCHANGE);
                ok49 = ok49 && packs_cap_stored(0) < 0;
                on_command(ID_PK_CAPRESET, BN_CLICKED);
                ok49 = ok49 && g_pk_ncaps == 0;
                if (!ok49) {
                    _snprintf_s(why49, sizeof why49, _TRUNCATE,
                                "Texturdetail/Grenzen: lv=%s ncap=%d cap5=%d", lv, ncap,
                                packs_cap_stored(5));
                }
            }
            if (ok49) { /* B5: installierte Packs zeigen die Remaster-Bedienung */
                int keep = g_rem_pkg;
                g_rem_pkg = 0;
                ok49 = rem_avail() && g_tex_src > 0 && ui_visible_on(ui_find(ID_GFX_TEX), P_GFX) &&
                       !ui_visible_on(ui_find(ID_REM_SCALE), P_GFX);
                gt_setenv("SB_PACKS_DIR", pke);
                show_page(P_PACKS); /* leer */
                ok49 = ok49 && !rem_avail() && g_tex_src == 0 &&
                       ui_visible_on(ui_find(ID_GFX_TEX), P_GFX) &&
                       !ui_visible_on(ui_find(ID_PK_LIST), P_PACKS) &&
                       g_rr[29].active &&
                       ui_visible_on(ui_find(ID_PK_CREATOR), P_PACKS) == (g_creator == 1) &&
                       g_rr[32].active == (g_creator != 1);
                g_rem_pkg = keep;
                if (!ok49) {
                    strcpy_s(why49, sizeof why49, "B5/leerer Zustand");
                }
            }
            if (ok49) { /* Hinzufuegen (Ordner verknuepfen) + sofort geprueft; Entfernen */
                char src[1300];
                _snprintf_s(src, sizeof src, _TRUNCATE, "%s/ta", pkd);
                g_confirm_auto = 1;
                ok49 = packs_add_path(src, 1) == 0 && g_pk_n == 1 &&
                       g_pk_chk[0] >= 0 && strcmp(g_pk_ord[0].id, "t.ta") == 0 &&
                       packs_add_path(src, 1) != 0 && g_pk_n == 1; /* doppelt */
                g_pk_sel = 0;
                packs_del();
                ok49 = ok49 && g_pk_n == 0 && is_dir_u8(src); /* Ordner bleibt */
                g_confirm_auto = 0;
                if (!ok49) {
                    _snprintf_s(why49, sizeof why49, _TRUNCATE,
                                "Hinzufuegen/Entfernen: n=%d chk=%d", g_pk_n,
                                g_pk_n ? g_pk_chk[0] : -9);
                }
            }
            /* --- 50: Erweitert --- */
            {
                char path[1100], peek[1100], u8p[1100], root[1100];
                const char *mk2;
                adv_data_loc(path, sizeof path, &mk2); /* Selbsttest: legacy */
                ok50 = sb_userdata_mode() == SB_UD_LEGACY &&
                       strcmp(mk2, "gui.ud_legacy") == 0 && strcmp(path, g_client_disp) == 0;
                _snprintf_s(root, sizeof root, _TRUNCATE, "%sw172_ud_root", tmp);
                SetEnvironmentVariableA("SB_USERDATA", "profile");
                gt_setenv("SB_USERDATA_ROOT", root);
                sb_userdata_reset();
                adv_data_loc(path, sizeof path, &mk2);
                peek[0] = '\0';
                if (sb_userdata_path_peek(SB_UD_ROOT, peek, sizeof peek) == 1) {
                    u8_from_acp(peek, u8p, sizeof u8p);
                } else {
                    u8p[0] = '\0';
                }
                ok50 = ok50 && strcmp(mk2, "gui.ud_profile") == 0 && u8p[0] &&
                       strcmp(path, u8p) == 0; /* = --userdata "root" */
                SetEnvironmentVariableA("SB_USERDATA", "legacy");
                SetEnvironmentVariableA("SB_USERDATA_ROOT", NULL);
                sb_userdata_reset();
                if (!ok50) {
                    _snprintf_s(why50, sizeof why50, _TRUNCATE, "Datenort '%.70s' / '%.70s'",
                                path, u8p);
                }
                /* Schalter "beim Start laden" folgt der Auswahl (ohne Dateien) */
                strcpy_s(g_dflt_setup, sizeof g_dflt_setup, "mein_4k");
                ui_settext(ctl(ID_PROF_COMBO), "mein_4k");
                setup_auto_sync(0);
                ok50 = ok50 && ui_checked(ID_PROF_DFLT);
                ui_settext(ctl(ID_PROF_COMBO), "anderes");
                setup_auto_sync(0);
                ok50 = ok50 && !ui_checked(ID_PROF_DFLT) &&
                       ui_find(ID_PROF_DFLT)->kind == K_TOGGLE;
                g_dflt_setup[0] = '\0';
                if (ok50 == 0 && !why50[0]) {
                    strcpy_s(why50, sizeof why50, "Schalter beim Start laden");
                }
            }
            ShowWindow(g_hwnd, SW_HIDE);
            gui_teardown();
        }
        g_env_fix = NULL;
        if (had_env) {
            gt_setenv("SB_PACKS_DIR", senv);
        } else {
            SetEnvironmentVariableA("SB_PACKS_DIR", NULL);
        }
        g_pk_n = 0;
        { /* Test-Packs entfernen */
            static const char *const f[] = { "ta/a.dds", "ta/manifest.json",
                "tb/a.dds", "tb/manifest.json", "fx/manifest.json", "fx/params.json",
                "packs.json" };
            static const char *const d[] = { "ta", "tb", "fx", "bad", "" };
            wchar_t w[1300];
            int k;
            for (k = 0; k < (int)(sizeof f / sizeof f[0]); k++) {
                _snprintf_s(q, sizeof q, _TRUNCATE, "%s/%s", pkd, f[k]);
                u8_DeleteFile(q);
            }
            for (k = 0; k < (int)(sizeof d / sizeof d[0]); k++) {
                _snprintf_s(q, sizeof q, _TRUNCATE, "%s/%s", pkd, d[k]);
                u8_to_w(q, w, 1300);
                RemoveDirectoryW(w);
            }
            _snprintf_s(q, sizeof q, _TRUNCATE, "%s/packs.json", pke);
            u8_DeleteFile(q);
            u8_to_w(pke, w, 1300);
            RemoveDirectoryW(w);
        }
        if (ok48) {
            GT_OK("48 play-setup (Einrichtungskarte, Sprunglinks)");
        } else {
            GT_FAIL("48 play-setup", "%s", why48);
        }
        if (ok49) {
            GT_OK("49 packs-ui (Liste, Reihenfolge, Konflikte, Grenzen, abgelehnt, leer, B5)");
        } else {
            GT_FAIL("49 packs-ui", "%s", why49);
        }
        if (ok50) {
            GT_OK("50 advanced (Datenort wie --userdata, beim Start laden)");
        } else {
            GT_FAIL("50 advanced", "%s", why50);
        }
        if (ok51) {
            GT_OK("51 tooltips (Begruendung gesperrter Elemente)");
        } else {
            GT_FAIL("51 tooltips", "%s", why51);
        }
    }

    /* 23 (W178 COMM-BG: beide Editionen; W162: groessen-unabhaengig):
     * Hintergrundbild: Ressource dekodiert
     * (beliebige Quellgroesse, z.B. 1672x941 seit 6.11.), Zuschnitt ohne
     * Strecken (Ausschnitt-Seitenverhaeltnis = Ziel-Seitenverhaeltnis, im
     * Quellbild), ausgeblichen (dunkler als die Quelle). */
    {
        int ok = 1;
        WICRect cr;
        int sizes[4][2] = { { 960, 640 }, { 1440, 960 }, { 400, 900 },
                                { 1920, 1080 } };
        int i;
        if (!bg_load() || g_img_w <= 0 || g_img_h <= 0) {
            ok = 0;
        }
        for (i = 0; ok && i < 4; i++) {
            double da = (double)sizes[i][0] / sizes[i][1];
            double ca;
            bg_cover_rect(g_img_w, g_img_h, sizes[i][0], sizes[i][1], &cr);
            ca = (double)cr.Width / cr.Height;
            if (fabs(ca / da - 1.0) > 0.005 || cr.X < 0 || cr.Y < 0 ||
                cr.X + cr.Width > g_img_w || cr.Y + cr.Height > g_img_h) {
                ok = 0;
            }
        }
        if (ok && bg_scaled(960, 640)) {
            double ms = 0, md = 0;
            int k;
            for (k = 0; k < g_img_w * g_img_h; k += 7) {
                ms += (g_img[k] >> 8) & 255;
            }
            ms /= (g_img_w * g_img_h + 6) / 7;
            for (k = 0; k < 960 * 640; k += 7) {
                md += (g_bgs[k] >> 8) & 255;
            }
            md /= (960 * 640 + 6) / 7;
            if (!(md < ms * 0.6)) {
                ok = 0;
            }
            printf("     (23: Quelle %dx%d, Helligkeit %.0f -> %.0f)\n",
                   g_img_w, g_img_h, ms, md);
        } else {
            ok = 0;
        }
        if (ok) {
            GT_OK("23 bg-image");
        } else {
            GT_FAIL("23 bg-image", "%s", "Ressource/Zuschnitt/Abdunkeln");
        }
    }

    /* 24: Launcher-eigene Einstellung (Hintergrund an/aus) */
    {
        int ok = ui_cfg_load_bg(dir) == 1 && ui_cfg_save_bg(dir, 0) == 0 &&
                 ui_cfg_load_bg(dir) == 0 && ui_cfg_save_bg(dir, 1) == 0 &&
                 ui_cfg_load_bg(dir) == 1;
        ui_cfg_path(dir, p, sizeof p);
        u8_DeleteFile(p);
        if (ok) {
            GT_OK("24 ui-cfg");
        } else {
            GT_FAIL("24 ui-cfg", "%s", "bg_image nicht gespeichert");
        }
    }

    /* 25: Vorschau-Mathematik (nie strecken) + Offscreen-Position */
    {
        unsigned bw, bh;
        int ok = 1, x, y;
        aspect_box_for(2560, 1080, &bw, &bh);
        if (bw != 1440 || bh != 1080) {
            ok = 0;
        }
        aspect_box_for(1280, 1024, &bw, &bh);
        if (bw != 1280 || bh != 960) {
            ok = 0;
        }
        offscreen_pos(&x, &y);
        if (x < GetSystemMetrics(SM_XVIRTUALSCREEN) +
                    GetSystemMetrics(SM_CXVIRTUALSCREEN)) {
            ok = 0;
        }
        if (ok) {
            GT_OK("25 aspect+offscreen");
        } else {
            GT_FAIL("25 aspect+offscreen", "bw=%u bh=%u x=%d", bw, bh, x);
        }
    }
    /* 27 (W116 EDITIONS): Badge passt zur Edition; Remaster-Paket wird
     * nur mit textures/x2|x4|manual erkannt (auto: Vorfahr mit poc-native,
     * sonst remaster_dir aus user.cfg). */
    {
        int ok = 1, r0, r1, r2;
        char root[1100], cl[1200], rd[1024], q[1300];
        wchar_t w[1300];
        ok = strcmp(SB_EDITION_BADGE, SB_EDITION_COMMUNITY ? "COMMUNITY EDITION"
                                                            : "PRIVATE EDITION") == 0;
        _snprintf_s(root, sizeof root, _TRUNCATE, "%sw116_rem_test", tmp);
        _snprintf_s(cl, sizeof cl, _TRUNCATE, "%s/client", root);
        _snprintf_s(q, sizeof q, _TRUNCATE, "%s/poc-native", root);
        lcfg_mkdir_p(cl);
        lcfg_mkdir_p(q);
        r0 = rem_pkg_detect(cl, rd, sizeof rd);
        _snprintf_s(q, sizeof q, _TRUNCATE, "%s/remaster/textures/x4", root);
        lcfg_mkdir_p(q);
        r1 = rem_pkg_detect(cl, rd, sizeof rd);
        if (!strstr(rd, "w116_rem_test/remaster")) {
            ok = 0;
        }
        _snprintf_s(q, sizeof q, _TRUNCATE, "%s/user.cfg", cl);
        write_file(q, "remaster_dir=w116_gibt_es_nicht/remaster\n");
        r2 = rem_pkg_detect(cl, rd, sizeof rd);
        u8_DeleteFile(q);
        if (r0 != 0 || r1 != 1 || r2 != 0) {
            ok = 0;
        }
        printf("     (27: %s, Paket ohne/mit/fremd = %d/%d/%d)\n", SB_EDITION_BADGE,
               r0, r1, r2);
        {
            static const char *const rm[] = { "remaster/textures/x4",
                                              "remaster/textures", "remaster",
                                              "poc-native", "client", "" };
            int k;
            for (k = 0; k < 6; k++) {
                _snprintf_s(q, sizeof q, _TRUNCATE, "%s/%s", root, rm[k]);
                u8_to_w(q, w, 1300);
                RemoveDirectoryW(w);
            }
        }
        if (ok) {
            GT_OK("27 edition+remaster-paket");
        } else {
            GT_FAIL("27 edition+remaster-paket", "%s", "Badge/Paketerkennung falsch");
        }
    }
    /* 53 (W179 KEYS-HTML): Browser-Export ohne Fenster (Fixture, kein
     * Fenster, nie oeffnen, nie Fokus): alle Abschnitte, Primaerfeuer,
     * Escaping (<&> im Profilnamen), keine Verweise. */
    {
        static char html[393216];
        char fx[1300], why53[300] = "";
        int ok53 = 1;
        const char *fprof = "w179_<t&>st";
        const char *ftext =
            "# fixture (keine Spieldaten)\n"
            "[buttons]\n"
            "bi0 = KEY:SPACE, MOUSE:LEFT, PAD:RB\n"
            "bi3 = PAD:BACK+PAD:Y # <tag> & \"anf\"\n";
        keyshtml_build(LT("gui.keys_memo"), fprof, ftext, NULL,
                       LT("gui.mouse_abs"), 4, "fixture-pad <x>",
                       "live & werte", "sbc <ok> & bereit",
                       html, sizeof html);
        _snprintf_s(fx, sizeof fx, _TRUNCATE, "%sw179_keyshtml.html", tmp);
        write_file(fx, html);
        { /* Datei-Roundtrip (wie keyshtml_open schreibt) */
            FILE *f = NULL;
            static char back[393216];
            size_t n = 0;
            if (u8_fopen_s(&f, fx, "rb") == 0 && f) {
                n = fread(back, 1, sizeof back - 1, f);
                fclose(f);
            }
            back[n] = '\0';
            if (n == 0 || strcmp(back, html) != 0) {
                ok53 = 0;
                strcpy_s(why53, sizeof why53, "Datei-Roundtrip");
            }
        }
        if (ok53) {
            static const char *const secs[] = {
                "gui.keys_h_menus", "gui.start_mech", "gui.keys_h_combat",
                "gui.keys_h_move", "gui.keys_h_cockpit", "gui.ctl_mouse",
                "gui.ctl_pad", "gui.ctl_device",
            };
            int k;
            for (k = 0; k < 8; k++) {
                if (!strstr(html, LT(secs[k]))) {
                    ok53 = 0;
                    _snprintf_s(why53, sizeof why53, _TRUNCATE,
                                "Abschnitt fehlt: %s", secs[k]);
                    break;
                }
            }
        }
        if (ok53 && (!strstr(html, LT("gui.ctl_prim")) ||
                     !strstr(html, LT("gui.ctl_prim_keys")) ||
                     !strstr(html, "class=\"prim\""))) {
            ok53 = 0;
            strcpy_s(why53, sizeof why53, "Primaerfeuer fehlt");
        }
        if (ok53 && (!strstr(html, "w179_&lt;t&amp;&gt;st") ||
                     strstr(html, "w179_<t&>st") ||
                     !strstr(html, "&lt;tag&gt; &amp; &quot;anf&quot;") ||
                     !strstr(html, "fixture-pad &lt;x&gt;"))) {
            ok53 = 0;
            strcpy_s(why53, sizeof why53, "Escaping falsch");
        }
        if (ok53 && (strstr(html, "http") || strstr(html, "HTTP"))) {
            ok53 = 0;
            strcpy_s(why53, sizeof why53, "Verweis enthalten");
        }
        if (ok53 && (!strstr(html, SB_EDITION_COMMUNITY ? "Steelbound"
                                                        : "Steel Battalion") ||
                     !strstr(html, LT("gui.page_controls")))) {
            ok53 = 0;
            strcpy_s(why53, sizeof why53, "Titel/Edition falsch");
        }
        if (ok53) { /* Standardbelegung: kein Gamepad, Escaping (&) */
            keyshtml_build(LT("gui.keys_memo"), "", NULL, NULL,
                           LT("gui.mouse_abs"), 4, "", "", "",
                           html, sizeof html);
            if (strstr(html, LT("gui.ctl_pad")) ||
                !strstr(html, LT("gui.keys_h_menus")) ||
                !strstr(html, "&amp;")) {
                ok53 = 0;
                strcpy_s(why53, sizeof why53, "Standard-Variante falsch");
            }
        }
        if (ok53) {
            GT_OK("53 keys-html (Abschnitte, Primaerfeuer, Escaping, kein http)");
        } else {
            GT_FAIL("53 keys-html", "%s", why53);
        }
        { /* Beispiel-HTML (englisch, Fixture) fuers Doku-Archiv */
            char keep[8];
            strcpy_s(keep, sizeof keep, sb_lang_code());
            sb_lang_set("en");
            keyshtml_build(sb_lang("gui.keys_memo"), fprof, ftext, NULL,
                           sb_lang("gui.mouse_abs"), 4, "fixture-pad <x>",
                           "live & werte", "sbc <ok> & bereit",
                           html, sizeof html);
            _snprintf_s(fx, sizeof fx, _TRUNCATE,
                        "%sw179_controls_example.html", tmp);
            write_file(fx, html);
            printf("     (53: Beispiel %s)\n", fx);
            sb_lang_set(keep);
        }
        _snprintf_s(fx, sizeof fx, _TRUNCATE, "%sw179_keyshtml.html", tmp);
        u8_DeleteFile(fx);
    }
    /* 54 (W182 KEYS-SVG): Controller-Zeichnung im Browser-Export (Fixture,
     * nie oeffnen, nie Fokus): SVG vorhanden, jedes Mapping-Element hat
     * Form + Callout + Tabellenzeile, Belegung je Element, Escaping, keine
     * Verweise; Profil- und Standard-Variante; eingebaute Standard-
     * belegung == harness/profiles/keyboard_default.ini (privat). */
    {
        static char html[393216], ftext[8192];
        char fx[1300], why54[300] = "";
        const char *unc = ks_uncovered();
        int ok54 = 1;
        if (unc) {
            ok54 = 0;
            _snprintf_s(why54, sizeof why54, _TRUNCATE,
                        "Mapping-Element ohne Zeichnung: %s", unc);
        }
        if (ok54) { /* Profil-Variante + Escaping-Probe */
            _snprintf_s(ftext, sizeof ftext, _TRUNCATE, "%s%s",
                        ks_fixture_text, "bi15 = KEY:A<&> # Probe\n");
            keyshtml_build(LT("gui.keys_memo"), "w182_<p&d>", ftext, NULL,
                           LT("gui.mouse_abs"), 4, "", "", "", html,
                           sizeof html);
            ok54 = ks_selfcheck(html, 1, why54, sizeof why54);
            if (ok54 && (!strstr(html, "A&lt;&amp;&gt;") ||
                         strstr(html, "A<&>") ||
                         !strstr(html, "w182_&lt;p&amp;d&gt;") ||
                         strstr(html, "w182_<p&d>"))) {
                ok54 = 0;
                strcpy_s(why54, sizeof why54, "Escaping falsch");
            }
        }
        if (ok54) { /* Standardbelegung (kein Profil) */
            keyshtml_build(LT("gui.keys_memo"), "", NULL, NULL,
                           LT("gui.mouse_abs"), 4, "", "", "", html,
                           sizeof html);
            ok54 = ks_selfcheck(html, 0, why54, sizeof why54);
            if (!ok54) {
                char t[300];
                _snprintf_s(t, sizeof t, _TRUNCATE, "Standard: %s", why54);
                strcpy_s(why54, sizeof why54, t);
            }
        }
#ifdef SB_HARNESS_DIR_DEFAULT
        if (ok54) { /* eingebaut == Datei (nur privat, Datei vorhanden) */
            static lctrl_bind_t bf, bb;
            char kp[1200];
            _snprintf_s(kp, sizeof kp, _TRUNCATE,
                        "%s/../../profiles/keyboard_default.ini",
                        SB_HARNESS_DIR_DEFAULT);
            if (lctrl_bind_load_file(kp, &bf) == 0) {
                int r, a;
                lctrl_bind_load_text(ks_legacy_text, &bb);
                for (r = 0; r < LCTRL_ELEM_N && ok54; r++) {
                    if (bf.row[r].n_alt != bb.row[r].n_alt) {
                        ok54 = 0;
                    }
                    for (a = 0; ok54 && a < bf.row[r].n_alt; a++) {
                        ok54 = strcmp(bf.row[r].alt[a], bb.row[r].alt[a]) == 0;
                    }
                    if (!ok54) {
                        _snprintf_s(why54, sizeof why54, _TRUNCATE,
                                    "Standardbelegung != keyboard_default.ini "
                                    "(%s)", kCtrlElems[r].key);
                    }
                }
            } else {
                printf("     (54: %s fehlt, Abgleich uebersprungen)\n", kp);
            }
        }
#endif
        if (ok54) {
            GT_OK("54 keys-svg (Zeichnung, Callouts je Element, Escaping, "
                  "kein http, Profil + Standard)");
        } else {
            GT_FAIL("54 keys-svg", "%s", why54);
        }
        { /* 55 (W184 KEYS-LAYOUT): physische Anordnung, keine Ueberlappung */
            char why55[200] = "";
            if (ks_layoutcheck(why55, sizeof why55)) {
                GT_OK("55 keys-layout (Bloecke wie Original, Felder, keine "
                      "Callout-Ueberlappung)");
            } else {
                GT_FAIL("55 keys-layout", "%s", why55);
            }
        }
        { /* Beispiel-HTML (englisch, Fixture) fuers Doku-Archiv */
            char keep[8];
            strcpy_s(keep, sizeof keep, sb_lang_code());
            sb_lang_set("en");
            keyshtml_build(sb_lang("gui.keys_memo"), "w182_fixture",
                           ks_fixture_text, NULL, sb_lang("gui.mouse_abs"), 4,
                           "fixture: gamepad connected (port 0)", "",
                           "fixture: original controller not connected",
                           html, sizeof html);
            _snprintf_s(fx, sizeof fx, _TRUNCATE,
                        "%sw184_controls_example.html", tmp);
            write_file(fx, html);
            printf("     (54: Beispiel %s)\n", fx);
            sb_lang_set(keep);
        }
    }
    /* 56 (W185 ZADIG-HELP): Einrichtungsanleitung als lokale HTML-Seite
     * (nur bauen, nie oeffnen, nie Fokus): alle 6 Abschnitte, Warnung
     * mit 0A7B:D000, genau ein Link (<a href> auf die Zadig-Seite),
     * keine Skripte/externen Verweise, Escaping (& -> &amp;);
     * Datei-Roundtrip wie zadig_open schreibt; beide Sprachen. */
    {
        static char html[65536];
        static const char *const zsecs[] = {
            "gui.zadig_need_h", "gui.zadig_driver_h", "gui.zadig_launch_h",
            "gui.zadig_assist_h", "gui.zadig_start_h", "gui.zadig_fix_h",
        };
        static const char *const zbad[] = {
            "<script", "SCRIPT", "<link", "<img", "src=", "url(",
            "javascript:", "onload=", "onclick=", "onerror=",
        };
        static const char *const zlangs[] = { "en", "de" };
        char fx[1300], keep[8], why56[300] = "", e[2048], hl[32];
        int ok56 = 1, li, k;
        strcpy_s(keep, sizeof keep, sb_lang_code());
        for (li = 0; li < 2 && ok56; li++) {
            const char *q;
            int nlink = 0, nurl = 0;
            sb_lang_set(zlangs[li]);
            zadig_build(html, sizeof html);
            for (k = 0; k < 6 && ok56; k++) {
                keyshtml_escape(sb_lang(zsecs[k]), e, sizeof e);
                if (!strstr(html, e)) {
                    ok56 = 0;
                    _snprintf_s(why56, sizeof why56, _TRUNCATE,
                                "%s: Abschnitt fehlt: %s", zlangs[li],
                                zsecs[k]);
                }
            }
            if (ok56 && !strstr(html, "0A7B:D000")) {
                ok56 = 0;
                _snprintf_s(why56, sizeof why56, _TRUNCATE,
                            "%s: Warnung 0A7B:D000 fehlt", zlangs[li]);
            }
            if (ok56) { /* genau ein Link, sonst kein http */
                for (q = html; (q = strstr(q, "<a href=\"")) != NULL; q++) {
                    nlink++;
                }
                for (q = html; (q = strstr(q, ZADIG_URL)) != NULL; q++) {
                    nurl++;
                }
                if (nlink != 1 || nurl != 2 ||
                    !strstr(html, "<a href=\"" ZADIG_URL "\">" ZADIG_URL
                            "</a>")) {
                    ok56 = 0;
                    _snprintf_s(why56, sizeof why56, _TRUNCATE,
                                "%s: Link falsch (a=%d, url=%d)", zlangs[li],
                                nlink, nurl);
                }
            }
            if (ok56) { /* keine Skripte/externen Verweise */
                for (k = 0; k < 10 && ok56; k++) {
                    if (strstr(html, zbad[k])) {
                        ok56 = 0;
                        _snprintf_s(why56, sizeof why56, _TRUNCATE,
                                    "%s: verbotener Verweis: %s", zlangs[li],
                                    zbad[k]);
                    }
                }
            }
            if (ok56) { /* Escaping: jedes & gehoert zu einer Entitaet */
                for (q = html; (q = strchr(q, '&')) != NULL; q++) {
                    if (strncmp(q, "&amp;", 5) && strncmp(q, "&lt;", 4) &&
                        strncmp(q, "&gt;", 4) && strncmp(q, "&quot;", 6)) {
                        ok56 = 0;
                        _snprintf_s(why56, sizeof why56, _TRUNCATE,
                                    "%s: rohes &", zlangs[li]);
                        break;
                    }
                }
                if (ok56 && !strstr(html, "&amp;")) {
                    ok56 = 0; /* kein & im Text = Probe wirkungslos */
                    _snprintf_s(why56, sizeof why56, _TRUNCATE,
                                "%s: kein &amp; (Probe leer)", zlangs[li]);
                }
            }
            if (ok56) { /* Titel/Edition, Sprache, Fuss, Druck */
                keyshtml_escape(sb_lang("gui.zadig_title"), e, sizeof e);
                _snprintf_s(hl, sizeof hl, _TRUNCATE, "<html lang=\"%s\">",
                            sb_lang_code());
                if (!strstr(html, SB_EDITION_COMMUNITY ? "Steelbound"
                                                      : "Steel Battalion") ||
                    !strstr(html, e) || !strstr(html, hl) ||
                    !strstr(html, sb_lang("gui.keys_local")) ||
                    !strstr(html, "@media print")) {
                    ok56 = 0;
                    _snprintf_s(why56, sizeof why56, _TRUNCATE,
                                "%s: Titel/Sprache/Fuss/Druck falsch",
                                zlangs[li]);
                }
            }
        }
        if (ok56) { /* Datei-Roundtrip (wie zadig_open schreibt) */
            FILE *f = NULL;
            static char back[65536];
            size_t n = 0;
            _snprintf_s(fx, sizeof fx, _TRUNCATE, "%sw185_zadig_self.html",
                        tmp);
            write_file(fx, html);
            if (u8_fopen_s(&f, fx, "rb") == 0 && f) {
                n = fread(back, 1, sizeof back - 1, f);
                fclose(f);
            }
            back[n] = '\0';
            if (n == 0 || strcmp(back, html) != 0) {
                ok56 = 0;
                strcpy_s(why56, sizeof why56, "Datei-Roundtrip");
            }
            u8_DeleteFile(fx);
        }
        if (ok56) {
            GT_OK("56 zadig-guide (6 Abschnitte, Warnung 0A7B:D000, ein "
                  "Link, kein Skript, Escaping, en+de)");
        } else {
            GT_FAIL("56 zadig-guide", "%s", why56);
        }
        { /* Beispiel-HTML (englisch) fuers Doku-Archiv */
            sb_lang_set("en");
            zadig_build(html, sizeof html);
            _snprintf_s(fx, sizeof fx, _TRUNCATE,
                        "%sw185_zadig_guide.html", tmp);
            write_file(fx, html);
            printf("     (56: Beispiel %s)\n", fx);
            sb_lang_set(keep);
        }
    }
    _snprintf_s(p, sizeof p, _TRUNCATE, "%s/default.xbe", dir);
    u8_DeleteFile(p);
    {
        wchar_t w[1300];
        _snprintf_s(p, sizeof p, _TRUNCATE, "%s/profiles", dir);
        u8_to_w(p, w, 1300);
        RemoveDirectoryW(w);
        u8_to_w(dir, w, 1300);
        RemoveDirectoryW(w);
    }
}
