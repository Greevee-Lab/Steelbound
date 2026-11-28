/* launcher_gui.c - Oberflaeche des Launchers (W82 LAUNCHER-UI2).
 * Randloses Fenster (WS_POPUP, eigene Titelleiste, DWM-Schatten),
 * Per-Monitor-DPI v2, W-API/UTF-16 an der Fenstergrenze (intern UTF-8),
 * eigene Zeichnung (dunkle Palette, halbtransparente Karten ueber einem
 * ausgeblichenen Hintergrundbild aus der Ressource, WIC). Funktionen wie
 * W26/W47/W78/W79: Grafik, Ton, Steuerung (Belegungs-Dialog), Profile,
 * Pfade, Start (user.cfg + Spielerbetrieb-Block + sb_ldr.exe).
 * W462 LAUNCHER-SPLIT: Hauptfenster + Nachrichtenschleife; Module launcher_gui_*.c,
 * gemeinsamer Zustand in launcher_gui_int.h. */
#include "launcher_gui_int.h" /* W462: Includes, Makros, Typen, gemeinsamer Zustand */


/* ------------------------------------------------------------------ */
/* Layout (logische 96-dpi-Einheiten)                                  */
/* ------------------------------------------------------------------ */
/* W397 LAUNCHER-STRUCT: Kopf von links: Marke (zweizeilig) | Edition | Spielumschalter | Status | Zahnrad | _ x
 * W398 LAUNCHER-GAMESWITCH: links Projektzeile (klein) ueber den Spielnamen als Wortmarken-Tabs (aktiv = hell +
 * oranger Balken); rechts Plaketten | Status | Zahnrad | _ x. Das Logo links entfaellt.
 * W401 LAUNCHER-HEADER-POLISH: Edition/Beta als Zusatz in der Projektzeile; rechts eine Gruppe
 * [Update-Chip] [Status-Chip] [Zahnrad], vertikal mittig zur Wortmarkenzeile; _ x klein in der Ecke. */
/* W403 LAUNCHER-R1-POLISH: Gruppe rechtsbuendig mit der rechten Inhaltskante (Karten, x = 932), unter _ x */
/* W172 LAUNCHER-P5: Seite Packs = Liste links (breit), Detail rechts */

/* W171 LAUNCHER-P2P3: sieben Seiten (UX-Konzept 3.2). Remaster, Profile
 * und Pfade sind in Grafik, Packs und Erweitert aufgegangen. */
/* W397 LAUNCHER-STRUCT: Launcher-Einstellungen (Zahnrad im Kopf) = Sonderseite ausserhalb der Navigation
 * (nicht in P_N, damit alle Seitenschleifen/Navigation unveraendert bleiben). */

/* W142: Sprachschluessel (Anzeige via LT), nicht direkt ausgeben. */
static const char *const kPageName[P_N] = {
    "gui.page_play", "gui.page_display", "gui.page_graphics", "gui.page_packs",
    "gui.page_controls", "gui.page_audio_extras", "gui.page_advanced", /* W195: Ton & Extras */
    "gui.alpha_page" /* W272 */
};
const char *const kPageKey[P_N] = {
    "play", "display", "graphics", "packs", "controls", "audio", "advanced", "tools" /* W301 (alt: alpha) */
};
const wchar_t kPageIcon[P_N] = { 0xE768, 0xE7F4, 0xE790, 0xE7B8,
                                        0xE7FC, 0xE767, 0xE713, 0xEBE8 };
/* W272 ALPHA-TEST: Seite "Test tools" (W301, vorher "Alpha Test"; launcher_alpha.inc, vor den Selbsttests) */
int g_alpha_tools = 1; /* launcher_ui.cfg test_tools (W301; Rueckfall alpha_tools), 0 = Seite aus */
int g_show_adv; /* W291 ESC-REWORK: launcher_ui.cfg show_advanced (Feinregler auf Anzeige/Grafik zeigen) */
/* W351 LOC-TP-FIX: launcher_ui.cfg loctp_mode (Testpartner-Ablauf, 0 = du erstellst, 1 = Partner erstellt);
 * -1 = nie gewaehlt (nicht geschrieben). Auch die Community-Ausgabe liest und schreibt den Wert unveraendert mit. */
int g_loctp_mode_cfg = -1;
void paint_alpha(void);
void alpha_build_controls(HWND w);
void alpha_page_shown(void);
static int alpha_on_command(int id, int code);
void alpha_relabel(void);
static int alpha_start(int dry, const la_sel_t *sel);
/* W279 DIAG-CRASH (launcher_diag.inc) */
static void dg_init(void);
int dg_ctl_visible(int id);
int dg_paint_play(void);
static void dg_paint_alpha(int y, int h);
void dg_build_controls(HWND w);
void dg_relabel(void);
static int dg_on_command(int id);
/* Alte --ui-shot-Namen (bis W170, acht Seiten) -> neue Zielseite. */
static const struct {
    const char *name;
    int page;
} kPageAlias[] = {
    { "start", P_PLAY },     { "grafik", P_DISPLAY }, { "remaster", P_GFX },
    { "ton", P_AUDIO },      { "steuerung", P_CTRL }, { "profile", P_ADV },
    { "pfade", P_ADV },
    { "alpha", P_ALPHA }, /* W301 BETA-VERSION: Seite heisst jetzt "tools" */
};

int gui_page_count(void)
{
    return P_N;
}

const char *gui_page_key(int p)
{
    if (p == P_LSET) {
        return "settings"; /* W397: Launcher-Einstellungen (--ui-shot settings) */
    }
    if (p == P_NETI) {
        return "steelnet"; /* W470: steelnet > Info (--ui-shot steelnet) */
    }
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
    if (strcmp(v, "settings") == 0 || strcmp(v, "launcher") == 0) {
        return P_LSET; /* W397 */
    }
    if (strcmp(v, "steelnet") == 0) {
        return P_NETI; /* W470 */
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

/* W172 LAUNCHER-P567 */
/* W173 LAUNCHER-P4: ein Grafik-Preset (Konzept 3.3/4.3) */
/* W303 AUDIO-VOLUME: Lautstaerke Gesamt/Musik/Effekte/Sprache-Funk (Ton & Extras, Karte Ton) */
#define ID_VOL_MASTER 1118
#define ID_VOL_MUSIC 1119
#define ID_VOL_SFX 1120
/* W445: Karte "Fotos & Screenshots" mit Zeile "Cinematic: eigene VT-Teile bis" - Zeilen darunter enger */
const int kVolId[4] = { ID_VOL_MASTER, ID_VOL_MUSIC, ID_VOL_SFX, ID_VOL_VOICE };
const char *const kVolKey[4] = { "gui.vol_master", "gui.vol_music", "gui.vol_sfx", "gui.vol_voice" };
int g_vol_raw[4] = { 100, 100, 100, 100 }; /* W303: geladener Wert (nicht auf 5 gerundet bleibt erhalten) */
/* W397 LAUNCHER-STRUCT: Zahnrad im Kopf, Seite Launcher-Einstellungen (P_LSET), Erstabfrage "Online & Updates" */
#define ID_LSET_TOOLS 2907 /* Seite P_LSET: Test tools oeffnen (frueher eigener Navigationspunkt) */
#define ID_LSET_DATA 2911  /* Seite P_LSET: Datenordner oeffnen (wie Erweitert) */
#define WM_APP_UPD (WM_APP + 21) /* Ergebnis aus dem Abruf-Thread (l = upd_job_t*) */
#define UPD_TIMER 25             /* Gesamtfrist des Abrufs (LUPD_TIMEOUT_MS) */
const char *g_upd_ver = ""; /* eigene Release-Version ("" = ausgeblendet; Shots: Fixture) */
int g_upd_cfg = -1;         /* user.cfg update_check: -1 nie gefragt, 0 nein, 1 ja */
int g_upd_ask;              /* Erstabfrage sichtbar */
int g_upd_busy, g_upd_manual, g_upd_gen;
lupd_res_t g_upd_res;       /* letztes Ergebnis (NEWER -> Hinweis) */
char g_img_msg[600]; /* W150: Ergebnis der letzten Abbild-Pruefung (UTF-8) */
int g_img_ok;
void check_image_ex(int status);




uictl_t g_ui[UI_MAX];
int g_ui_n;

/* --- Zustand -------------------------------------------------------- */
gui_opts_t g_o;
HWND g_hwnd;
int g_dpi = 96;
/* W281 LAUNCHER-SIZE: g_dpi = Geometrie (Monitor-DPI x Layout-Skalierung),
 * g_dpi_font = Schrift, g_dpi_mon = Monitor. Layout 0: alle gleich. */
static int g_dpi_mon = 96;
int g_dpi_font = 96;
llay_t g_lay = { LLAY_0, LLAY_SZ_NORMAL, 100, 100, LLAY_DW, LLAY_DH, LLAY_NAV, 0, 0, 0 };
int g_page;
int g_updating;
char g_client_dir[1024];
char g_client_disp[1024]; /* normalisiert, nur Anzeige */
int g_has_ldr;
char g_note[256];       /* user.cfg-/Profil-Lage beim Start */
int g_note_kind;        /* W173: 2 = gespeicherte Einstellung beim Start geladen */
char g_status[1024];    /* Statuszeile unten */
int g_status_kind;      /* 0 = normal, 1 = ok, 2 = Fehler */
char g_aspect_info[512];
char g_sbc_txt[512];
char g_pad_txt[160];
char g_padlive_txt[512];
char g_prof_info[256];
char g_profile_line[256];
int g_focus_id;         /* Edit mit Fokus (Rahmenfarbe) */
unsigned g_prev_w = 2560, g_prev_h = 1080;
int g_prev_edge, g_prev_ew = 12, g_prev_es = 100; /* W107: Vorschau Bildraender */
int g_bg_on = 1;
/* W171 LAUNCHER-P2P3: Regelwerk (ui_apply_rules) + Laufzeitwissen */
lr_res_t g_rr[LR_N];       /* letzte Auswertung R1..R30 */
const lr_env_t *g_env_fix; /* Selbsttest: feste Umgebung statt Messung */
int g_diag_open;           /* Erweitert: Diagnose aufgeklappt (UI-cfg) */
int g_sbc_rc = -1;         /* lctrl_sbc_status: 0 = gefunden, -1 = ungeprueft */
int g_disp_hz;             /* Bildwiederholrate des Hauptmonitors */
int g_disp_hz_max;         /* W309: hoechste Rate des Hauptmonitors (aktuelle Aufloesung) */
int g_mon_alt_hz;          /* W309: Rate eines anderen Monitors, falls abweichend (0 = keiner) */
unsigned g_mon_w, g_mon_h; /* Aufloesung des Hauptmonitors */
char g_dflt_setup[64];     /* Standard-Profil (profiles/standard.txt) */
int g_confirm_auto;        /* Selbsttest: Rueckfragen mit Ja beantworten */
launcher_settings_t g_undo_s; /* Muster C: Stand vor dem Umschalten */
int g_undo_on;
/* W535 AVVT: Tastenkuerzel-Tabelle des Hauptfensters (statt GetKeyState im Nachrichtenkreis) */
static HACCEL g_kbacc;

/* Strg+Tab / Strg+Umschalt+Tab (Seite vor/zurueck), Strg+Z bzw. Strg+Umschalt+Z (W171 Muster C). */
HACCEL kbacc_create(void)
{
    ACCEL acc[] = {
        { FVIRTKEY | FCONTROL, VK_TAB, ACC_NAV_NEXT },
        { FVIRTKEY | FCONTROL | FSHIFT, VK_TAB, ACC_NAV_PREV },
        { FVIRTKEY | FCONTROL, 'Z', ID_UNDO },
        { FVIRTKEY | FCONTROL | FSHIFT, 'Z', ID_UNDO },
    };
    return CreateAcceleratorTableW(acc, (int)(sizeof(acc) / sizeof(acc[0])));
}

static void acc_command(int id)
{
    if (id == ACC_NAV_NEXT || id == ACC_NAV_PREV) {
        show_page(nav_next(g_page, id == ACC_NAV_PREV ? -1 : 1));
    } else if (id == ID_UNDO && g_undo_on) { /* W171 Muster C */
        on_command(ID_UNDO, BN_CLICKED);
    }
}
/* W172 LAUNCHER-P567 */
int g_setup_force;         /* Spielen: Einrichtungskarte offen halten */
int g_bb_setup = -1;       /* W220: Karte im zuletzt gezeichneten Bild (-1 = nicht Spielen) */
int g_data_ok = 1;         /* letzte Spieldaten-Pruefung (Regelwerk) */
int g_edition;             /* W231: LCFG_ED_* des eingestellten Spiels (Regelwerk) */
void *g_client_proc;       /* W231: zuletzt gestarteter Client (Exitcode 5/8) */
int g_client_ed;           /* W231: Fassung dieses Starts */
static int g_firstrun;            /* W217: erster Start (keine user.cfg, kein Standard-Profil) */
char g_data_err[300];
HWND g_tip;                /* Tooltips (Begruendung gesperrter Elemente) */
int g_creator = -1;        /* sb_creator.exe neben dem Launcher: -1 ungeprueft */
void ui_apply_rules(void);
lgp_mask_t gfx_lock_now(void); /* W173: Effect-Pack-Schluessel (Packs-Engine) */
launcher_settings_t g_gfx; /* W173: Werte des Grafik-Presets (s. gfx_sync_ui) */
void gfx_sync_ui(void);
int g_tex_src;             /* W173: aktive Texturquellen (Packs + lokaler Ordner) */
int play_setup_shown(void);
int rem_avail(void);
int packs_count(void);
int packs_status_of(int i);
int packs_enabled_of(int i);
int packs_is_fx(int i);
const char *packs_level_word(void);
void snap_settings(launcher_settings_t *out);
void packs_draw_row(const DRAWITEMSTRUCT *d);
void xbe_cue_update(void);
void setup_auto_sync(int from_list);
void xbe_derived(const launcher_settings_t *s, char *out, size_t cap);
int path_eq(const char *a, const char *b);
void tips_place(void);
int ui_refresh_visibility(void);
/* W227: Breite der linken Auswahl in den Zeilen Verlauf/Cockpit (Anzeige);
 * die rechte ("Strength 100 %") bekommt den Rest. */
void show_page(int p);
int acc_sub_active(void);    /* W399: 1 = Unterpunkt Account von Multiplayer aktiv */
void acc_nav_sub(int acc);   /* W399: Unterpunkt waehlen (0 = Match & network, 1 = Account) */
void nav_sub_layout(void);   /* W399: Leiste mit/ohne Unterpunkte */
int ui_confirm(const char *text, const char *title);
void set_status_k(const char *msg, int kind); /* W187 */
/* W276 LOC-LAUNCH: Spielauswahl (Block "W276 LOCTAB" vor build_controls) */
int g_game;                 /* LGAME_SB | LGAME_LOC */
void pergame_mark(void);    /* W402: launcher_pergame=1 in user.cfg */
/* W392 LAUNCHER-NETTAB: bei LoC heisst der Packs-Platz "Netzwerk" (eigenes Symbol: Globus). */
const char *page_name_key(int p)
{
    return g_game == LGAME_LOC && p == P_PACKS ? "gui.page_mp" : kPageName[p]; /* W397: Multiplayer (W392: Network) */
}
int loctab_visible(const uictl_t *u, int page); /* -1 = keine Vorgabe */
void paint_loc_play(void);
int loctab_wide_val(void); /* W326: LoC-Bildformat (LGAME_WIDE_*) fuer den Experimental-Hinweis */
int loctab_may_write(void); /* W414: do_save schreibt Bildformat + Markierung in user_loc.cfg */
lgame_state_t g_loc;        /* W414: Vorab-Deklaration (Definition im Block W276 LOCTAB) */
/* W287 EXP-ICON: Experimentell-Symbol (Kolben) an den Karten, Warn-Symbol am Hinweis */
unsigned g_orig_mask;   /* SB_OFG_* der abweichenden Gruppen (ui_apply_rules) */
int orig_visible(int id, int page); /* -1 = keine Vorgabe */
void orig_paint_play(int loc);
int orig_banner_on(void); /* W293 */
void xicon_texts(void); /* W287 */
void onl_sync_ui(void);  /* W392 */
void lnet_netmode_fill(void); /* W447 */
void paint_lnet(void);   /* W392 */
void xtip_text(int id, char *out, size_t cap);
int loc_tip_id(int id); /* W387: LoC-Bedienelemente mit eigenem Tooltip */
void orig_relabel(void);
int loctab_command(int id, int code);
void acc_page_shown(void); /* W328 STEELNET-ACCOUNT */
void acc_play_shown(void); /* W418 */
int onl_status_refresh(void); /* W334 LOC-SEC: loc_net_status.txt (Block nach loctab_save) */
void acc_apply_state(const char *name); /* W328: Shot-Zustaende loc_acc_* */
void acc_fixture(int srv, int have, int discord, int login, int view); /* W397 */
/* W397 LAUNCHER-STRUCT (Block vor on_command) */
int lset_visible(int id, int page);
void paint_lset(void);
void hdr_paint_status(void);
void hdr_paint_sub(void);        /* W401 */
void hdr_tip_text(int kind, char *out, size_t cap); /* W403 */
static void snp_netm_changed(void);     /* W403 */
void lset_open(void);
int gametab_id(int id);         /* W398 */
void netm_sync_ui(void);
int netm_get(void);
void lset_open(void);  /* W400: Sprung aus LoC > Spielen */
void loc_gate_sync(void); /* W400 */
int loc_launch_ok(void);  /* Launch in der Fussleiste: gleiche Sperre wie Play Line of Contact */
void loc_gate_msg(char *m, size_t cap);
int loc_ver_gate(void);   /* W421 LOC-VERSION-GATE: Launcher laut steelnet veraltet -> LoC-Start gesperrt */
void snp_apply(const lacc_probe_t *r); /* W421: Shots mit Pruefergebnis (Attrappe) */
void locver_fixture(const char *min_ver); /* W421 */
void ver_open_page(void); /* W421: Release-Seite (Browser, kein Download) */
int netq_should_ask(const char *ver, int upd_cfg, int online);
void netq_paint(int x, int y, int w);
void lset_build(HWND w);
void lset_relabel(void);
static int lset_command(int id, int code);
void loctab_relabel(void);
/* W281 LAUNCHER-SIZE (Block bei on_dpi) */
void lay_switch(int id, int size);
static void lay_cfg_save(void);
void lay_combo_fill(HWND c);

/* W116 EDITIONS: Remaster-Paket lokal vorhanden? Die Seite Remaster zeigt
 * ihre Inhalte nur dann. Ordner wie im Client (sb_remaster.c,
 * rem_resolve_dir): remaster_dir aus sb_native.cfg/user.cfg des
 * Client-Ordners (user.cfg gewinnt, letzte Zeile gilt), sonst
 * <erster Vorfahr mit poc-native/>/remaster, sonst <Client>/remaster.
 * Paket = textures/x4, x2 oder manual (Ordner) oder textures/<hash>.dds. */
int g_rem_pkg = -1; /* -1 unbekannt, 0 keins, 1 vorhanden */
static char g_rem_dir[1024];

int cfg_read_key(const char *path, const char *key, char *out,
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

int is_dir_u8(const char *p)
{
    unsigned long a = u8_GetFileAttributes(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

int rem_pkg_detect(const char *client_dir, char *dir, size_t cap)
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
launcher_settings_t g_s;
launcher_settings_t g_cur; /* W172: Oberflaechen-Stand der letzten Regelauswertung */

HFONT g_f[F_N];
int g_has_icons;
int g_icon_fluent; /* W287: Segoe Fluent Icons (Kolben U+F196 nur dort) */
wchar_t g_face_body[64];


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
void snap_settings(launcher_settings_t *out)
{
    *out = g_s;
    ui_to_settings(out);
}

/* ------------------------------------------------------------------ */
/* W250 UPDATE-CHECK: Abruf im Hintergrund (Oberflaeche friert nie ein) */
/* ------------------------------------------------------------------ */
typedef struct {
    HWND h;
    int gen;
    char ver[64];
    lupd_res_t r;
} upd_job_t;

static DWORD WINAPI upd_thread(LPVOID p)
{
    upd_job_t *j = (upd_job_t *)p;
    lupd_fetch(j->ver, &j->r);
    if (!PostMessageW(j->h, WM_APP_UPD, (WPARAM)j->gen, (LPARAM)j)) {
        free(j); /* Fenster schon weg */
    }
    return 0;
}

/* Abruf starten. manual = "Jetzt pruefen" (Meldung auch bei Fehlern).
 * Test-/Shot-Betrieb: nie Netz. */
static void upd_start(int manual)
{
    upd_job_t *j;
    HANDLE th;
    if (!g_upd_ver[0] || g_o.shot != 0 || !g_hwnd) {
        return;
    }
    if (g_upd_busy) {
        g_upd_manual |= manual;
        return;
    }
    j = (upd_job_t *)calloc(1, sizeof *j);
    if (!j) {
        return;
    }
    j->h = g_hwnd;
    j->gen = ++g_upd_gen;
    strcpy_s(j->ver, sizeof j->ver, g_upd_ver);
    th = CreateThread(NULL, 0, upd_thread, j, 0, NULL);
    if (!th) {
        free(j);
        return;
    }
    CloseHandle(th);
    g_upd_busy = 1;
    g_upd_manual = manual;
    SetTimer(g_hwnd, UPD_TIMER, LUPD_TIMEOUT_MS, NULL);
    if (manual) {
        set_status(LT("gui.upd_checking"));
    }
}

/* Ergebnis anwenden (UI-Thread). Fehler bleiben still, ausser manuell. */
static void upd_done(const lupd_res_t *r)
{
    char t[300], v[64];
    int manual = g_upd_manual;
    KillTimer(g_hwnd, UPD_TIMER);
    g_upd_busy = 0;
    g_upd_manual = 0;
    if (r->result == LUPD_R_NEWER || r->result == LUPD_R_CURRENT) {
        g_upd_res = *r;
    }
    if (r->result == LUPD_R_NEWER) {
        upd_tag_disp(r->rel.tag, v, sizeof v);
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.upd_new"), v);
        set_status_k(t, 1);
    } else if (!manual) {
        /* still */
    } else if (r->result == LUPD_R_CURRENT) {
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.upd_latest"), g_upd_ver);
        set_status_k(t, 1);
    } else if (r->result == LUPD_R_HTTP) {
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.upd_fail_http"), r->http_status);
        set_status_k(t, 2);
    } else {
        set_status_k(LT("gui.upd_fail"), 2);
    }
    ui_refresh_visibility();
    redraw_all();
}

/* Frist abgelaufen: spaetes Ergebnis wird verworfen (Generationszaehler). */
static void upd_timeout(void)
{
    lupd_res_t r;
    if (!g_upd_busy) {
        KillTimer(g_hwnd, UPD_TIMER);
        return;
    }
    g_upd_gen++;
    memset(&r, 0, sizeof r);
    r.result = LUPD_R_NET;
    upd_done(&r);
}

/* Antwort auf die Erstabfrage bzw. den Schalter merken (user.cfg). */
static void upd_set_cfg(int on)
{
    char p[1200];
    user_cfg_path(p, sizeof p);
    g_upd_cfg = on ? 1 : 0;
    g_upd_ask = 0;
    ui_set_checked(ID_UPD_AUTO, g_upd_cfg == 1);
    if (g_o.shot != 0) {
        return; /* Shots/Selbsttest schreiben nichts */
    }
    if (lupd_cfg_set(p, g_upd_cfg) != 0) {
        set_status_k(LT("gui.upd_save_fail"), 2);
    }
}

/* Zustand beim Start (gui_fill): Version, Antwort aus user.cfg. */
static void upd_init(void)
{
    char p[1200];
    g_upd_ver = lupd_version();
    user_cfg_path(p, sizeof p);
    g_upd_cfg = lupd_cfg_get(p);
    g_upd_ask = lupd_should_ask(g_upd_ver, g_upd_cfg);
}

/* Release-Seite oeffnen - nur mit festem Praefix (sonst nichts). */
void upd_open_page(void)
{
    if (g_upd_res.result != LUPD_R_NEWER || !lupd_url_ok(g_upd_res.rel.url)) {
        set_status_k(LT("gui.upd_badurl"), 2);
        return;
    }
    if (g_o.shot != 0) {
        printf("[upd] oeffnen: %s\n", g_upd_res.rel.url);
        return;
    }
    {
        wchar_t w[600];
        u8_to_w(g_upd_res.rel.url, w, 600);
        ShellExecuteW(g_hwnd, L"open", w, NULL, NULL, SW_SHOWNORMAL);
    }
}

/* W421: Release-Seite bei Versionssperre bzw. -ablehnung (W420): mit Ergebnis der Versionspruefung die Seite der
 * neuen Version, sonst ".../releases/latest" (fester Praefix, lupd_url_ok). Nur Browser - kein Download, kein
 * Selbst-Update. Shots/Selbsttest: nur zaehlen. */
void ver_open_page(void)
{
    const char *url = LUPD_PAGE_PREFIX "latest";
    if (g_upd_res.result == LUPD_R_NEWER && lupd_url_ok(g_upd_res.rel.url)) {
        url = g_upd_res.rel.url;
    }
    if (!lupd_url_ok(url)) {
        return;
    }
    if (g_o.shot != 0 || g_in_gui_selftest) {
        g_ver_open_n++;
        printf("[upd] oeffnen: %s\n", url);
        return;
    }
    {
        wchar_t w[600];
        u8_to_w(url, w, 600);
        ShellExecuteW(g_hwnd, L"open", w, NULL, NULL, SW_SHOWNORMAL);
    }
}

static void on_command_inner(int id, int code);

/* ------------------------------------------------------------------ */
/* W397 LAUNCHER-STRUCT: Launcher-Einstellungen (Zahnrad im Kopf, Seite  */
/* P_LSET ausserhalb der Navigation) und Erstabfrage "Online & Updates". */
/* Drei Stufen ohne neuen user.cfg-Schluessel, abgeleitet aus den alten: */
/*   launcher_loc_online=1            -> Mit steelnet verbinden          */
/*   sonst update_check=1             -> Nur Version pruefen             */
/*   sonst                            -> Offline                         */
/* Waehlen schreibt beide alten Schluessel (steelnet: online=1/update=1, */
/* Version: 0/1, Offline: 0/0). Wer die alte Update-Frage beantwortet    */
/* hat, landet damit ohne neue Frage auf der passenden Stufe; Online an  */
/* + Update nein bleibt steelnet (Update-Schluessel unveraendert, bis    */
/* neu gewaehlt wird). Update-Pruefung bleibt Hinweis + Release-Seite.   */
/* ------------------------------------------------------------------ */
#define LSET_NET_H 332
#define LSET_ABOUT_Y (CY0 + LSET_NET_H + 12)
#define LSET_LANG_H 130
#define LSET_BG_Y (LSET_LANG_Y + LSET_LANG_H + 12)
#define LSET_BG_H 76
#define LSET_TOOLS_Y (LSET_BG_Y + LSET_BG_H + 12)
#define LSET_TOOLS_H 116
#define LSET_DATA_Y (LSET_TOOLS_Y + LSET_TOOLS_H + 12)
#define LSET_RADIO_Y(i) (CY0 + 62 + (i) * 66)
static const char *const kNetmTitle[NETM_N] = { "gui.netm_steel", "gui.netm_ver", "gui.netm_off" };
static const char *const kNetmHelp[NETM_N] = { "gui.netm_steel_hint", "gui.netm_ver_hint", "gui.netm_off_hint" };
static const char *const kNetqBtn[NETM_N] = { "gui.netq_steel", "gui.netq_ver", "gui.netq_off" };
static const char *const kNetmDone[NETM_N] = { "gui.netm_done_steel", "gui.netm_done_ver", "gui.netm_done_off" };

/* Erstabfrage noetig? Wie W250 (Release-Version, update_check nie beantwortet), ausser das Online-Spiel ist
 * schon an (= Stufe steelnet gewaehlt). */
int netq_should_ask(const char *ver, int upd_cfg, int online)
{
    return lupd_should_ask(ver, upd_cfg) && !online;
}

int netm_get(void)
{
    if (g_loc_online && onl_srv_on()) {
        return NETM_STEEL;
    }
    return g_upd_cfg == 1 ? NETM_VER : NETM_OFF;
}

static void netm_radio_text(int i, char *out, size_t cap)
{
    _snprintf_s(out, cap, _TRUNCATE, "%s\n%s", LT(kNetmTitle[i]), LT(kNetmHelp[i]));
}

/* Auswahl/Grau zum Zustand (ohne steelnet-Pins: Stufe steelnet grau). */
void netm_sync_ui(void)
{
    int m = netm_get(), i, srv = onl_srv_on();
    for (i = 0; i < NETM_N; i++) {
        ui_set_checked(ID_NETM0 + i, i == m);
    }
    if (ctl(ID_NETM0 + NETM_STEEL)) {
        EnableWindow(ctl(ID_NETM0 + NETM_STEEL), srv ? TRUE : FALSE);
    }
    if (ctl(ID_NETQ0 + NETM_STEEL)) {
        EnableWindow(ctl(ID_NETQ0 + NETM_STEEL), srv ? TRUE : FALSE);
    }
    ui_set_checked(ID_LSET_OPEN, g_page == P_LSET);
}

/* Stufe waehlen (Erstabfrage oder Seite): beide alten Schluessel schreiben. */
static void netm_set(int m)
{
    int was_upd = g_upd_cfg == 1;
    if (m < 0 || m >= NETM_N) {
        return;
    }
    if (m == NETM_STEEL && !onl_srv_on()) {
        set_status_k(LT("gui.loc_onl_nosrv"), 0);
        netm_sync_ui();
        return;
    }
    g_loc_online = m == NETM_STEEL;
    onl_save();                 /* launcher_loc_online (Shots: nie geschrieben) */
    upd_set_cfg(m != NETM_OFF); /* update_check, beendet die Erstabfrage */
    onl_sync_ui();
    netm_sync_ui();
    loc_gate_sync(); /* W400: LoC-Start sofort frei bzw. gesperrt */
    snp_netm_changed(); /* W403: Kopf-Chip (steelnet -> gleich pruefen, sonst Ergebnis vergessen) */
    set_status_k(LT(kNetmDone[m]), 1);
    if (m != NETM_OFF && !was_upd) {
        upd_start(0); /* wie frueher "Ja, pruefen": gleich einmal (Test-/Shot-Betrieb: nie Netz) */
    }
    ui_refresh_visibility();
    redraw_all();
    if (g_o.shot == 0) nrq_eval(); /* W496: nach der Erstabfrage bzw. Stufe steelnet einmal fragen */
}

/* ------------------------------------------------------------------ */
/* W403 LAUNCHER-R1-POLISH: steelnet-Statuspruefung fuer den Kopf-Chip.  */
/* Nur mit Stufe "Mit steelnet verbinden" und eingerichtetem Dienst:     */
/* beim Start und dann regelmaessig die bestehende Abfrage               */
/* GET /v1/info (lacc_probe: TLS + SPKI-Pin, nur relay.steelbound.dev    */
/* bzw. Loopback-Test). Thread, Ergebnis per WM_APP_SNP; Fehler leise,   */
/* keine Adresse in Log/UI. Netz nur im echten Fensterbetrieb            */
/* (g_snp_live, gesetzt vor der Nachrichtenschleife); Shots/Selbsttest   */
/* nie (Selbsttest: g_snp_dry zaehlt nur, snp_apply = Attrappe).         */
/* Live-Zahlen (Piloten/Lobbys/Spiele, Objekt "online" neuerer Server)   */
/* veralten schnell: Abstand 60 s, solange das Fenster sichtbar und      */
/* nicht minimiert ist, sonst wie bisher 5 Minuten (snp_due).            */
/* ------------------------------------------------------------------ */
#define WM_APP_SNP (WM_APP + 31)
#define SNP_TIMER 403
#define SNP_SLACK_MS 2000            /* Timer-Ungenauigkeit: knapp zu frueh zaehlt als faellig */
typedef struct {
    HWND h;
    int gen;
    lacc_probe_t r;
} snp_job_t;
int g_snp_state = -1;   /* -1 = noch keine Pruefung (grau "steelnet ..."), 0 = offline, 1 = online */
static int g_snp_ms;           /* Antwortzeit der letzten Pruefung */
static char g_snp_ver[32];     /* Serverversion (falls geliefert) */
static SYSTEMTIME g_snp_when;  /* Zeitpunkt der letzten Pruefung (lokal) */
int g_snp_busy, g_snp_gen, g_snp_live, g_snp_dry, g_snp_dry_n;
int g_snp_has, g_snp_pl, g_snp_lob, g_snp_gam; /* Live-Zahlen der letzten Pruefung (g_snp_has 0 = keine) */
static ULONGLONG g_snp_t0;     /* GetTickCount64 des letzten Anstosses (0 = noch keiner) */
static int g_snp_today = -1, g_snp_peak = -1; /* W470: games_today/peak_7d der letzten Pruefung (-1 = keine) */

static DWORD WINAPI snp_thread(LPVOID p)
{
    snp_job_t *j = (snp_job_t *)p;
    lach_res_t *res = (lach_res_t *)calloc(1, sizeof *res); /* W537: GET /v1/info im Hilfsprogramm sb_account.exe */
    char e[48];
    memset(&j->r, 0, sizeof j->r);
    if (res && lach_run("info", NULL, NULL, NULL, NULL, 0, 30000, NULL, res, e, sizeof e) == 0) {
        j->r = res->probe;
    } else {
        if (res) j->r = res->probe; /* Fehlercode/Antwortzeit des Dienstes, falls geliefert */
        if (!j->r.error[0]) strcpy_s(j->r.error, sizeof j->r.error, res ? e : "helper");
        j->r.ok = 0;
    }
    free(res);
    if (!PostMessageW(j->h, WM_APP_SNP, (WPARAM)j->gen, (LPARAM)j)) {
        free(j);
    }
    return 0;
}

/* Pruefung anstossen (still). 1 = gestartet bzw. (Selbsttest) waere gestartet. */
int snp_kick(void)
{
    snp_job_t *j;
    HANDLE th;
    if (netm_get() != NETM_STEEL || !onl_srv_on() || g_snp_busy || !g_hwnd) {
        return 0;
    }
    if (g_snp_dry) { /* Selbsttest: nur zaehlen, nie Netz */
        g_snp_dry_n++;
        return 1;
    }
    if (!g_snp_live || g_o.shot != 0) {
        return 0;
    }
    j = (snp_job_t *)calloc(1, sizeof *j);
    if (!j) {
        return 0;
    }
    j->h = g_hwnd;
    j->gen = ++g_snp_gen;
    th = CreateThread(NULL, 0, snp_thread, j, 0, NULL);
    if (!th) {
        free(j);
        return 0;
    }
    CloseHandle(th);
    g_snp_busy = 1;
    g_snp_t0 = GetTickCount64();
    return 1;
}

/* Timer: faellig, wenn seit dem letzten Anstoss der Abstand (60 s sichtbar, 5 min minimiert/verborgen) um ist.
 * Rein (Selbsttest). */
int snp_due(int shown, ULONGLONG since_ms)
{
    return since_ms + SNP_SLACK_MS >= (ULONGLONG)(shown ? SNP_FAST_MS : SNP_SLOW_MS);
}

static void snp_tick(void)
{
    int shown = g_hwnd && IsWindowVisible(g_hwnd) && !IsIconic(g_hwnd);
    if (g_snp_t0 == 0 || snp_due(shown, GetTickCount64() - g_snp_t0)) {
        snp_kick();
    }
}

/* Ergebnis uebernehmen (UI-Thread; Selbsttest ruft direkt mit Attrappe). */
void snp_apply(const lacc_probe_t *r)
{
    int was_st = g_snp_state, was_has = g_snp_has, was_pl = g_snp_pl;
    g_snp_state = r->ok ? 1 : 0;
    g_snp_ms = r->ms;
    strcpy_s(g_snp_ver, sizeof g_snp_ver, r->ok ? r->version : "");
    g_snp_has = r->ok && r->has_counts; /* alter Server ohne "online" -> keine Zahlen (Anzeige wie vorher) */
    g_snp_pl = g_snp_has ? r->players : 0;
    g_snp_lob = g_snp_has ? r->lobbies : 0;
    g_snp_gam = g_snp_has ? r->games : 0;
    g_snp_today = g_snp_has ? r->games_today : -1; /* W470 */
    g_snp_peak = g_snp_has ? r->peak_7d : -1;
    { /* W421: Mindestversion nur aus einer erreichbaren Antwort (sonst keine Sperre); nur Speicher */
        int was_gate = loc_ver_gate();
        strcpy_s(g_locver_min, sizeof g_locver_min, r->ok && sb_netver_strict(r->min_ver) ? r->min_ver : "");
        if (loc_ver_gate() != was_gate && g_hwnd) {
            loc_gate_sync();
            ui_refresh_visibility();
            redraw_all();
        }
    }
    GetLocalTime(&g_snp_when);
    /* Kopf liegt im Grundbild; nur neu zeichnen, wenn sich der Chip aendert (Takt 60 s). Tooltip holt seinen
     * Text erst beim Zeigen (TTN_GETDISPINFO) und braucht kein Neuzeichnen. */
    if (g_hwnd && (g_snp_state != was_st || g_snp_has != was_has || g_snp_pl != was_pl)) {
        redraw_all();
    }
}

/* Stufe gewechselt: nicht steelnet -> Ergebnis vergessen (naechstes Mal wieder grau bis zur ersten Pruefung). */
static void snp_netm_changed(void)
{
    if (netm_get() != NETM_STEEL) {
        g_snp_state = -1;
        g_snp_ms = 0;
        g_snp_ver[0] = '\0';
        g_snp_has = g_snp_pl = g_snp_lob = g_snp_gam = 0;
        g_snp_today = g_snp_peak = -1; /* W470 */
        g_locver_min[0] = '\0'; /* W421: Versionssperre gilt nur mit Stufe steelnet */
        g_snp_gen++; /* laufendes Ergebnis verwerfen */
        g_snp_busy = 0;
    } else if (g_snp_state < 0) {
        snp_kick();
    }
    hub_net_changed(); /* W470: Lobbyliste der steelnet-Seite vergessen bzw. neu holen */
}

/* W470 LAUNCHER-HUB: Stand der Kopf-Pruefung fuer steelnet > Info (nur lesen). */
void snp_view(snp_view_t *v)
{
    v->state = hdr_steel_state();
    v->own = g_snp_state;
    v->ms = g_snp_ms;
    v->ver = g_snp_ver;
    v->hh = g_snp_when.wHour;
    v->mm = g_snp_when.wMinute;
    v->has = g_snp_state > 0 && g_snp_has;
    v->players = g_snp_pl;
    v->lobbies = g_snp_lob;
    v->games = g_snp_gam;
    v->today = g_snp_today;
    v->peak = g_snp_peak;
}

/* Kopf: steelnet-Zustand. W403: zuerst die eigene Pruefung (snp_*), sonst wie W401 nur aus vorhandenen
 * Ergebnissen: Statusdatei des letzten LoC-Laufs (sec=/err=) und letzte Konto-Abfrage.
 * 1 = online, 0 = offline, -1 = unbekannt. */
int hdr_steel_state(void)
{
    if (g_snp_state >= 0) {
        return g_snp_state;
    }
    if (!strcmp(g_onl_err, "dns") || !strcmp(g_onl_err, "connect") || !strcmp(g_onl_err, "lost")) {
        return 0;
    }
    if (!strcmp(g_onl_sec, "ok") || g_acc_st_ok) {
        return 1;
    }
    if (g_acc_last_rc && (!strcmp(g_acc_last_err, "net") || !strcmp(g_acc_last_err, "timeout"))) {
        return 0;
    }
    return -1;
}

/* Kopf rechts neben dem Spielumschalter: steelnet (nur Stufe steelnet) und neuere Version (nur mit
 * Versionspruefung). Leer, wenn beides nicht zutrifft. */
/* W398 LAUNCHER-GAMESWITCH: Plaketten rechts im Kopf (Edition bzw. Edition + Stufe/Version, kompakt). */
int hdr_text_w(int font, const char *u8, int extra) /* logische Breite (96 dpi) */
{
    wchar_t *wt;
    SIZE sz = { 0, 0 };
    HDC own = NULL, dc = g_bb.dc ? g_bb.dc : g_base.dc;
    if (!dc) {
        dc = own = GetDC(NULL); /* Selbsttest ohne fertiges Bild */
    }
    if (!dc || !u8 || !*u8 || (wt = u8_wdup(u8)) == NULL) {
        if (own) {
            ReleaseDC(NULL, own);
        }
        return 0;
    }
    SelectObject(dc, g_f[font]);
    SetTextCharacterExtra(dc, SX(extra));
    GetTextExtentPoint32W(dc, wt, (int)wcslen(wt), &sz);
    SetTextCharacterExtra(dc, 0);
    free(wt);
    if (own) {
        ReleaseDC(NULL, own);
    }
    return g_dpi > 0 ? MulDiv(sz.cx, 96, g_dpi) + 1 : sz.cx;
}

static void hdr_badge_parts(const char **a, char *b, size_t bcap)
{
    b[0] = '\0';
    if (!g_upd_ver[0]) {
        *a = SB_EDITION_BADGE; /* Entwicklungsstand: "PRIVATE EDITION" / "COMMUNITY EDITION" */
    } else { /* W301 BETA-VERSION: Release-Build -> "COMMUNITY | BETA 0.3.0" */
        const char *st = sb_release_stage(g_upd_ver);
        *a = SB_EDITION_COMMUNITY ? "COMMUNITY" : "PRIVATE";
        _snprintf_s(b, bcap, _TRUNCATE, "%s%s%s", st, st[0] ? " " : "", g_upd_ver);
    }
}

/* W401 LAUNCHER-HEADER-POLISH: Projektzeile = "STEELBOUND | GAME PRESERVATION PROJECT" (gedimmt), dahinter
 * " | PRIVATE EDITION" (Akzent) bzw. " | COMMUNITY | BETA 0.3.0" (Version heller). Teile in Reihenfolge,
 * Breiten logisch (Sperrung 1); Rueckgabe = Gesamtbreite. Selbsttest 65c. */
int hdr_sub_parts(const char *txt[3], COLORREF col[3], int w[3])
{
    static char ed[64], ver[80];
    const char *a;
    char b[64];
    int i, sum = 0;
    hdr_badge_parts(&a, b, sizeof b);
    _snprintf_s(ed, sizeof ed, _TRUNCATE, HDR_SUB_SEP "%s", a);
    ver[0] = '\0';
    if (b[0]) {
        _snprintf_s(ver, sizeof ver, _TRUNCATE, HDR_SUB_SEP "%s", b);
    }
    txt[0] = LT("gui.hdr_project");
    txt[1] = ed;
    txt[2] = ver;
    col[0] = C_DIM;
    col[1] = C_ACCENT;
    col[2] = C_ACCENT_HI;
    for (i = 0; i < 3; i++) {
        w[i] = txt[i][0] ? hdr_text_w(F_TINY, txt[i], 1) : 0;
        sum += w[i];
    }
    return sum;
}

/* Projektzeile auf dem Grundbild (links verankert wie W398, auch in Layout b). */
void hdr_paint_sub(void)
{
    const char *txt[3];
    COLORREF col[3];
    /* Pixel statt logischer Breiten: Schrift- und Geometrieskala koennen abweichen (UI-Groesse Kompakt) */
    int w[3], i, x = SX(HDR_GAME_X + HDR_GAME_PAD), xr = SX(HDR_WB_X - 16); /* nie unter _ x */
    hdr_sub_parts(txt, col, w);
    for (i = 0; i < 3 && x < xr; i++) {
        wchar_t *wt;
        SIZE sz = { 0, 0 };
        int ww;
        if (!txt[i][0] || (wt = u8_wdup(txt[i])) == NULL) {
            continue;
        }
        SelectObject(g_base.dc, g_f[F_TINY]);
        SetTextCharacterExtra(g_base.dc, SX(1));
        GetTextExtentPoint32W(g_base.dc, wt, (int)wcslen(wt), &sz);
        free(wt);
        ww = sz.cx + 2 > xr - x ? xr - x : sz.cx + 2;
        cv_text(&g_base, g_f[F_TINY], txt[i], x, SX(HDR_SUB_Y), ww, SX(HDR_SUB_H), col[i],
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        x += sz.cx;
    }
    SetTextCharacterExtra(g_base.dc, 0);
}

/* W401: Gruppe rechts: [Update-Chip] [Status-Chip] [Zahnrad]. Status-Chip immer (Punkt + Text), Update-Chip
 * nur mit Versionspruefung und neuerer Version. Reine Lage (logisch), Selbsttest 65c. */
#define HDR_CHIP_PAD 10
#define HDR_CHIP_GAP 8
#define HDR_CHIP_DOT 6

void hdr_chip_text(int kind, int brief, char *out, size_t cap, COLORREF *col, COLORREF *dot)
{
    if (kind == HDR_CHIP_UPD) {
        char v[64];
        upd_tag_disp(g_upd_res.rel.tag, v, sizeof v);
        _snprintf_s(out, cap, _TRUNCATE, brief ? "%s" : LT("gui.hdr_upd"), v);
        *col = C_ACCENT_HI;
        *dot = C_ACCENT;
        return;
    }
    if (netm_get() == NETM_STEEL) {
        int st = hdr_steel_state();
        /* W403: grau "steelnet ..." nur bis zur ersten Pruefung, dann gruen online bzw. warnfarben offline */
        strcpy_s(out, cap, LT(brief ? "gui.hdr_steel" : st < 0 ? "gui.hdr_steel_wait" : st > 0 ? "gui.hdr_steel_on"
                                                                                                 : "gui.hdr_steel_off"));
        if (st > 0 && g_snp_state > 0 && g_snp_has) { /* Live-Zahlen: "steelnet · 5 online", eng: "steelnet · 5" */
            if (brief) {
                _snprintf_s(out, cap, _TRUNCATE, "%s" HDR_SUB_SEP "%d", LT("gui.hdr_steel"), g_snp_pl);
            } else {
                _snprintf_s(out, cap, _TRUNCATE, LT("gui.hdr_steel_cnt"), g_snp_pl);
            }
        }
        *col = st == 0 ? C_WARN : st > 0 ? C_OK : C_DIM;
        *dot = st > 0 ? C_OK : st == 0 ? C_WARN : C_FAINT;
    } else {
        strcpy_s(out, cap, LT(netm_get() == NETM_VER ? "gui.hdr_steel_none" : "gui.hdr_offline"));
        *col = C_DIM;
        *dot = C_FAINT;
    }
}

void hdr_layout(hdr_lay_t *L)
{
    int brief, i, x = HDR_CHIP_R;
    memset(L, 0, sizeof *L);
    L->kind[L->n++] = HDR_CHIP_ST;
    if (g_upd_cfg == 1 && g_upd_res.result == LUPD_R_NEWER) {
        L->kind[L->n++] = HDR_CHIP_UPD;
    }
    for (brief = 0; brief < 2; brief++) { /* zu eng: Kurzform (nur "steelnet" bzw. Versionsnummer) */
        x = HDR_CHIP_R;
        for (i = 0; i < L->n; i++) {
            hdr_chip_text(L->kind[i], brief, L->txt[i], sizeof L->txt[i], &L->col[i], &L->dot[i]);
            L->w[i] = HDR_CHIP_PAD + HDR_CHIP_DOT + 6 + hdr_text_w(F_TINY, L->txt[i], 0) + 2 + HDR_CHIP_PAD;
            x -= L->w[i] + (i ? HDR_CHIP_GAP : 0);
            L->x[i] = x;
        }
        L->shortened = brief;
        if (x >= HDR_GAME_END + 12) {
            break;
        }
    }
    if (x < HDR_GAME_END + 12) { /* immer noch zu eng: linken Chip kappen (Text mit ...) */
        int over = HDR_GAME_END + 12 - x;
        i = L->n - 1;
        L->w[i] -= over;
        L->x[i] += over;
    }
}

/* Klickflaechen der Chips (Pixel, Client des Hauptfensters), in hdr_paint_status gesetzt. */
RECT g_hdr_hit[2];
int g_hdr_hit_kind[2], g_hdr_hit_n;

static void hdr_px(int x, int y, int w, int h, RECT *q)
{
    if (lay_b()) {
        lay_paint_px(x, y, w, h, q);
        return;
    }
    q->left = SX(x);
    q->top = SX(y);
    q->right = SX(x + w);
    q->bottom = SX(y + h);
}

/* -1 = kein Chip, sonst HDR_CHIP_* unter dem Punkt (Client-Pixel) */
int hdr_hit(POINT p)
{
    int i;
    for (i = 0; i < g_hdr_hit_n; i++) {
        if (PtInRect(&g_hdr_hit[i], p)) {
            return g_hdr_hit_kind[i];
        }
    }
    return -1;
}

/* W403: Tooltip der Chips (Zeilen mit CRLF). Status: erreichbar, Live-Zahlen (Piloten online, offene Lobbys,
 * laufende Spiele; nur wenn /v1/info das Objekt "online" liefert), Antwortzeit, Serverversion, letzte Pruefung,
 * "Klick: Online-Einstellungen". Update: installiert/neu, Klick. */
void hdr_tip_text(int kind, char *out, size_t cap)
{
    size_t k;
    out[0] = '\0';
    if (kind == HDR_CHIP_UPD) {
        char v[64];
        upd_tag_disp(g_upd_res.rel.tag, v, sizeof v);
        _snprintf_s(out, cap, _TRUNCATE, LT("gui.hdr_tip_inst"), g_upd_ver[0] ? g_upd_ver : "-");
        k = strlen(out);
        _snprintf_s(out + k, cap - k, _TRUNCATE, "\r\n");
        k = strlen(out);
        _snprintf_s(out + k, cap - k, _TRUNCATE, LT("gui.hdr_tip_new"), v);
        k = strlen(out);
        _snprintf_s(out + k, cap - k, _TRUNCATE, "\r\n%s", LT("gui.hdr_tip_rel"));
        return;
    }
    if (kind != HDR_CHIP_ST) {
        return;
    }
    if (netm_get() != NETM_STEEL) {
        strcpy_s(out, cap, LT(netm_get() == NETM_VER ? "gui.hdr_tip_none" : "gui.hdr_tip_offline"));
    } else {
        int st = hdr_steel_state();
        if (st < 0) {
            strcpy_s(out, cap, LT("gui.hdr_tip_wait"));
        } else {
            _snprintf_s(out, cap, _TRUNCATE, LT("gui.hdr_tip_reach"), LT(st > 0 ? "gui.hdr_tip_yes" : "gui.hdr_tip_no"));
            if (loc_ver_gate()) { /* W421: Launcher veraltet -> LoC-Start gesperrt */
                k = strlen(out);
                _snprintf_s(out + k, cap - k, _TRUNCATE, "\r\n");
                k = strlen(out);
                _snprintf_s(out + k, cap - k, _TRUNCATE, LT("gui.hdr_tip_locver"), g_locver_min, locver_own());
            }
            if (g_snp_state > 0 && g_snp_has) { /* Live-Zahlen (Einzahl/Mehrzahl je eigener Schluessel) */
                static const char *const kOne[3] = { "gui.hdr_tip_pil_1", "gui.hdr_tip_lob_1", "gui.hdr_tip_gam_1" };
                static const char *const kMany[3] = { "gui.hdr_tip_pil", "gui.hdr_tip_lob", "gui.hdr_tip_gam" };
                int n[3], i;
                n[0] = g_snp_pl;
                n[1] = g_snp_lob;
                n[2] = g_snp_gam;
                for (i = 0; i < 3; i++) {
                    k = strlen(out);
                    _snprintf_s(out + k, cap - k, _TRUNCATE, "\r\n");
                    k = strlen(out);
                    _snprintf_s(out + k, cap - k, _TRUNCATE, LT(n[i] == 1 ? kOne[i] : kMany[i]), n[i]);
                }
            }
            if (g_snp_state >= 0) { /* eigene Pruefung: Antwortzeit, Version, Zeitpunkt */
                k = strlen(out);
                _snprintf_s(out + k, cap - k, _TRUNCATE, "\r\n");
                k = strlen(out);
                _snprintf_s(out + k, cap - k, _TRUNCATE, LT("gui.hdr_tip_ms"), g_snp_ms);
                if (g_snp_ver[0]) {
                    k = strlen(out);
                    _snprintf_s(out + k, cap - k, _TRUNCATE, "\r\n");
                    k = strlen(out);
                    _snprintf_s(out + k, cap - k, _TRUNCATE, LT("gui.hdr_tip_ver"), g_snp_ver);
                }
                k = strlen(out);
                _snprintf_s(out + k, cap - k, _TRUNCATE, "\r\n");
                k = strlen(out);
                _snprintf_s(out + k, cap - k, _TRUNCATE, LT("gui.hdr_tip_when"), g_snp_when.wHour, g_snp_when.wMinute);
            }
        }
    }
    k = strlen(out);
    _snprintf_s(out + k, cap - k, _TRUNCATE, "\r\n%s", LT("gui.hdr_tip_click"));
}

void hdr_click(int kind)
{
    if (kind == HDR_CHIP_UPD) {
        upd_open_page(); /* wie "Release-Seite oeffnen" */
    } else if (kind == HDR_CHIP_ST) {
        show_page(P_NETI); /* W470: Bereich steelnet (ohne Stufe steelnet: Hinweis + Weg in die Einstellungen) */
    }
}

/* W398: Wortmarken-Tab (ID_GAME_SB/LOC). Aktiv: hell + oranger Balken unter dem Namen; inaktiv: gedimmt,
 * Hover heller (Handcursor in hover_proc). Schrift stufenweise kleiner, falls der Name nicht passt. */
static int gametab_font(HDC dc, const wchar_t *t, int n, int avail, int *tw)
{
    static const int fs[] = { F_GAME, F_BIG, F_CARD, F_BRAND, F_SEMI };
    int i;
    SIZE sz = { 0, 0 };
    for (i = 0; i < (int)(sizeof fs / sizeof fs[0]); i++) {
        SelectObject(dc, g_f[fs[i]]);
        SetTextCharacterExtra(dc, SX(2));
        GetTextExtentPoint32W(dc, t, n, &sz);
        SetTextCharacterExtra(dc, 0);
        if (sz.cx <= avail) {
            break;
        }
    }
    if (i == (int)(sizeof fs / sizeof fs[0])) {
        i--;
    }
    *tw = sz.cx < avail ? sz.cx : avail;
    return fs[i];
}

int gametab_tw(HWND hw, int *font) /* Breite des Namens in Pixeln im Tab (Selbsttest) */
{
    wchar_t txt[64];
    RECT r;
    int n, tw = 0, f;
    HDC dc;
    if (!hw || (dc = GetDC(hw)) == NULL) {
        return -1;
    }
    GetClientRect(hw, &r);
    n = GetWindowTextW(hw, txt, 64);
    CharUpperBuffW(txt, n);
    f = gametab_font(dc, txt, n, r.right - SX(2 * HDR_GAME_PAD), &tw);
    ReleaseDC(hw, dc);
    if (font) {
        *font = f;
    }
    return tw;
}

/* W473 MPPOLISH: Spiel-Reiter wirkt gewaehlt (hell + oranger Balken) nur im Spielbereich. In Optionen/steelnet
 * (spieluebergreifend) sehen beide Reiter wie inaktiv aus; checked bleibt = zuletzt gewaehltes Spiel (Launch). */
int gametab_active(const uictl_t *u)
{
    return u && u->checked && hub_area_of(g_page) == HUB_GAME;
}

void gametab_paint(canvas_t *c, const uictl_t *u, int w, int h, const wchar_t *txt, int n, int focus)
{
    wchar_t up[64];
    int tw = 0, f, pad = SX(HDR_GAME_PAD), bh = SX(3) < 3 ? 3 : SX(3), act = gametab_active(u);
    COLORREF fg = act ? C_TEXT : u->hover ? RGB(214, 219, 227) : C_DIM;
    if (n > 63) {
        n = 63;
    }
    memcpy(up, txt, (size_t)n * sizeof up[0]);
    up[n] = 0;
    CharUpperBuffW(up, n);
    f = gametab_font(c->dc, up, n, w - 2 * pad, &tw);
    SetTextCharacterExtra(c->dc, SX(2));
    cv_textw(c, g_f[f], up, n, pad, 0, w - 2 * pad, h - SX(10), fg, /* W401: Text 26 */
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SetTextCharacterExtra(c->dc, 0);
    if (act) {
        cv_fill(c, pad, h - SX(5), tw, bh, C_ACCENT, 255, SX(1));
    } else if (u->hover) {
        cv_fill(c, pad, h - SX(5), tw, bh, RGB(255, 255, 255), 40, SX(1));
    }
    if (focus) {
        cv_ring(c, 0, 0, tw + 2 * pad < w ? tw + 2 * pad : w, h, C_ACCENT_HI, 230, SX(4), SX(2) < 2 ? 2 : SX(2));
    }
}

void hdr_paint_status(void)
{
    hdr_lay_t L;
    int i;
    hdr_layout(&L);
    g_hdr_hit_n = 0;
    for (i = 0; i < L.n; i++) {
        int x = L.x[i], w = L.w[i], up = L.kind[i] == HDR_CHIP_UPD;
        int on = !up && g_page == P_NETI; /* W470: Bereich steelnet offen -> Chip gewaehlt (wie das Zahnrad) */
        L_fill(x, HDR_CHIP_Y, w, HDR_CHIP_H, up || on ? C_ACCENT : RGB(255, 255, 255), up ? 30 : on ? 24 : 10, 6);
        L_ring(x, HDR_CHIP_Y, w, HDR_CHIP_H, up || on ? C_ACCENT : RGB(255, 255, 255), up ? 120 : on ? 170 : 26, 6);
        L_fill(x + HDR_CHIP_PAD, HDR_CHIP_Y + (HDR_CHIP_H - HDR_CHIP_DOT) / 2, HDR_CHIP_DOT, HDR_CHIP_DOT,
               L.dot[i], 255, 3);
        L_text(F_TINY, L.txt[i], x + HDR_CHIP_PAD + HDR_CHIP_DOT + 6, HDR_CHIP_Y,
               w - 2 * HDR_CHIP_PAD - HDR_CHIP_DOT - 6 + 2, HDR_CHIP_H, L.col[i],
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        hdr_px(x, HDR_CHIP_Y, w, HDR_CHIP_H, &g_hdr_hit[g_hdr_hit_n]);
        g_hdr_hit_kind[g_hdr_hit_n++] = L.kind[i];
    }
    if (g_tip) { /* W403: Tooltip-Rechtecke = Klickflaechen (fehlender Chip: leer) */
        int c;
        for (c = 0; c < 2; c++) {
            TTTOOLINFOW ti;
            memset(&ti, 0, sizeof ti);
            ti.cbSize = sizeof ti;
            ti.hwnd = g_hwnd;
            ti.uId = (UINT_PTR)(HDR_TIP_ID0 + c);
            for (i = 0; i < g_hdr_hit_n; i++) {
                if (g_hdr_hit_kind[i] == c) {
                    ti.rect = g_hdr_hit[i];
                }
            }
            SendMessageW(g_tip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
        }
    }
}

/* Erstabfrage (Spielen, SB und LoC; Platz der frueheren Update-Karte). */
#define NETQ_H 134
void netq_paint(int x, int y, int w)
{
    card(x, y, w, NETQ_H, LT("gui.netq_title"));
    label(F_TINY, LT("gui.netq_text"), x + 18, y + 36, w - 36, 58, C_TEXT);
}

void lset_open(void)
{
    show_page(P_LSET);
}

/* Seite Launcher-Einstellungen */
void paint_lset(void)
{
    char t[600];
    const char *mode;
    /* links: Online & Updates, Ueber */
    card(CX0, CY0, COLW, LSET_NET_H, LT("gui.netm_title"));
    label1(F_TINY, LT("gui.netm_intro"), CX0 + 18, CY0 + 36, COLW - 36, 20, C_DIM);
    if (!g_upd_ver[0]) {
        label(F_TINY, LT("gui.upd_dev"), CX0 + 18, CY0 + LSET_NET_H - 70, COLW - 36, 34, C_FAINT);
    } else {
        char v[64];
        if (g_upd_res.result == LUPD_R_NEWER) {
            upd_tag_disp(g_upd_res.rel.tag, v, sizeof v);
            _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.upd_new"), v);
        } else {
            _snprintf_s(t, sizeof t, _TRUNCATE,
                        LT(g_upd_res.result == LUPD_R_CURRENT ? "gui.upd_latest" : "gui.upd_cur"), g_upd_ver);
        }
        L_text(F_TINY, t, CX0 + 18, CY0 + LSET_NET_H - 70, COLW - 36, 22,
               g_upd_res.result == LUPD_R_NEWER ? C_ACCENT_HI : C_DIM,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    card(CX0, LSET_ABOUT_Y, COLW, CY0 + 504 - LSET_ABOUT_Y, LT("gui.lset_about"));
    _snprintf_s(t, sizeof t, _TRUNCATE, "%s  \xC2\xB7  %s", SB_EDITION_BADGE,
                g_upd_ver[0] ? g_upd_ver : LT("gui.lset_devbuild"));
    label1(F_SMALL, t, CX0 + 18, LSET_ABOUT_Y + 40, COLW - 36, 22, C_TEXT);
    label1(F_TINY, "Game Preservation Project", CX0 + 18, LSET_ABOUT_Y + 64, COLW - 36, 18, C_DIM);
    label(F_TINY, LT("gui.lset_about_txt"), CX0 + 18, LSET_ABOUT_Y + 88, COLW - 36, 64, C_DIM);
    /* rechts: Sprache & Fenster, Launcher, Test tools, Datenordner */
    card(COL2, LSET_LANG_Y, COLW, LSET_LANG_H, LT("gui.lset_lang"));
    if (g_lay.id == LLAY_0) {
        label(F_TINY, LT("gui.foot_hint"), COL2 + 18, LSET_LANG_Y + 84, COLW - 36, 34, C_FAINT);
    }
    card(COL2, LSET_BG_Y, COLW, LSET_BG_H, LT("gui.adv_launcher"));
    card(COL2, LSET_TOOLS_Y, COLW, LSET_TOOLS_H, LT("gui.alpha_page"));
    label(F_TINY, LT(g_alpha_tools ? "gui.lset_tools_txt" : "gui.lset_tools_off"), COL2 + 18, LSET_TOOLS_Y + 36,
          COLW - 36, 34, C_DIM);
    card(COL2, LSET_DATA_Y, COLW, CY0 + 504 - LSET_DATA_Y, LT("gui.lset_data"));
    {
        char path[1024];
        adv_data_loc(path, sizeof path, &mode);
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.adv_data_fmt"), LT(mode));
        label(F_TINY, t, COL2 + 18, LSET_DATA_Y + 38, COLW - 36 - 124, 34, C_DIM);
    }
}

void lset_build(HWND w)
{
    int i;
    char t[600];
    for (i = 0; i < NETM_N; i++) {
        netm_radio_text(i, t, sizeof t);
        mk(w, K_RADIO, ID_NETM0 + i, P_LSET, CX0 + 18, LSET_RADIO_Y(i), COLW - 36, 60, t, 0);
        mk(w, K_BTN, ID_NETQ0 + i, P_PLAY, UPD_X + 18 + i * ((UPD_W - 36 + 8) / 3), UPD_Y + 96,
           (UPD_W - 36 + 8) / 3 - 8, 28, LT(kNetqBtn[i]), 0);
    }
    mk(w, K_BTN, ID_LSET_TOOLS, P_LSET, COL2 + 18, LSET_TOOLS_Y + 74, 190, 28, LT("gui.lset_tools_open"), 0);
    mk(w, K_BTN, ID_LSET_DATA, P_LSET, COL2 + COLW - 18 - 112, LSET_DATA_Y + 40, 112, 26, LT("gui.adv_open_folder"),
       0);
    mk(w, K_BTN, ID_LSET_RELPAGE, P_LSET, CX0 + 18, CY0 + LSET_NET_H - 42, 150, 28,
       LT("gui.upd_open"), 0);
    nrq_build(w); /* W496 */
    netm_sync_ui();
}

void lset_relabel(void)
{
    int i;
    char t[600];
    for (i = 0; i < NETM_N; i++) {
        netm_radio_text(i, t, sizeof t);
        if (ctl(ID_NETM0 + i)) ui_settext(ctl(ID_NETM0 + i), t);
        if (ctl(ID_NETQ0 + i)) ui_settext(ctl(ID_NETQ0 + i), LT(kNetqBtn[i]));
    }
    if (ctl(ID_LSET_OPEN)) ui_settext(ctl(ID_LSET_OPEN), LT("gui.lset_title"));
    if (ctl(ID_LSET_TOOLS)) ui_settext(ctl(ID_LSET_TOOLS), LT("gui.lset_tools_open"));
    if (ctl(ID_LSET_DATA)) ui_settext(ctl(ID_LSET_DATA), LT("gui.adv_open_folder"));
    if (ctl(ID_LSET_RELPAGE)) ui_settext(ctl(ID_LSET_RELPAGE), LT("gui.upd_open"));
    if (ctl(ID_MP_GOSET)) ui_settext(ctl(ID_MP_GOSET), LT("gui.mp_goset"));
    if (ctl(ID_LOC_MP_GO)) ui_settext(ctl(ID_LOC_MP_GO), LT("gui.mp_go"));
    nrq_relabel(); /* W496 */
}

/* Sichtbarkeit (-1 = nicht zustaendig). Vor dem LoC-Bereich geprueft. */
int lset_visible(int id, int page)
{
    if (id >= ID_NETQ0 && id < ID_NETQ0 + NETM_N) { /* SB: Absturz-Karte hat Vorrang (wie die alte Update-Karte) */
        return g_upd_ask && page == P_PLAY && (g_game == LGAME_LOC || dg_ctl_visible(ID_UPD_YES));
    }
    if (id == ID_LSET_OPEN) {
        return 1;
    }
    if ((id >= ID_NETM0 && id < ID_NETM0 + NETM_N) || id == ID_LSET_DATA || id == ID_BG || id == ID_LANG ||
        id == ID_LANG_LBL) {
        return page == P_LSET;
    }
    if (id == ID_LSET_TOOLS) {
        return page == P_LSET && g_alpha_tools;
    }
    if (id == ID_LSET_RELPAGE) {
        return page == P_LSET && g_upd_res.result == LUPD_R_NEWER && g_upd_ver[0];
    }
    if (id == ID_UPD_NOW) {
        return page == P_LSET && g_upd_ver[0];
    }
    if (id == ID_LAYOUT || id == ID_LAYOUT_LBL) {
        return page == P_LSET && g_lay.id != LLAY_0;
    }
    if (id == ID_NAV0 + P_ALPHA) {
        return 0; /* Test tools: jetzt unter dem Zahnrad */
    }
    if (id == ID_MP_GOSET) {
        return g_game == LGAME_LOC && page == P_LNET && netm_get() != NETM_STEEL && !g_loc_lan && !acc_view_active(); /* W454 */
    }
    if (id == ID_LOC_MP_GO) {
        return g_game == LGAME_LOC && page == P_PLAY && !g_upd_ask;
    }
    return -1;
}

/* Befehle (1 = erledigt). */
static int lset_command(int id, int code)
{
    (void)code;
    if (nrq_command(id)) { /* W496 */
        return 1;
    }
    if (id == ID_LSET_OPEN) {
        lset_open();
        return 1;
    }
    if (id >= ID_NETM0 && id < ID_NETM0 + NETM_N) {
        netm_set(id - ID_NETM0);
        return 1;
    }
    if (id >= ID_NETQ0 && id < ID_NETQ0 + NETM_N) {
        netm_set(id - ID_NETQ0);
        return 1;
    }
    if (id == ID_LSET_TOOLS) {
        show_page(P_ALPHA);
        return 1;
    }
    if (id == ID_LSET_DATA) {
        on_command_inner(ID_DATA_OPEN, BN_CLICKED);
        return 1;
    }
    if (id == ID_LSET_RELPAGE) {
        upd_open_page();
        return 1;
    }
    if (id == ID_MP_GOSET) {
        lset_open();
        return 1;
    }
    if (id == ID_LOC_MP_GO) {
        acc_nav_sub(0); /* W399: Match & network */
        return 1;
    }
    return 0;
}

/* W486 SB-GFX-OPTIONS (#98): Meldungen der Kombi-Liste (oeffnen, schliessen, Wahl bestaetigt/verworfen) aendern
 * keinen Wert. user32 sendet bei einer Mausauswahl CBN_SELENDOK (und bei offener Liste CBN_CLOSEUP) vor
 * CBN_SELCHANGE. Die Regelauswertung dazwischen lief gfx_sync_ui und setzte die Grafikfelder (Monitor-Effekt,
 * Weltbeleuchtung, Schatten, Effekte, Kantenglaettung) auf den alten Wert zurueck, CBN_SELCHANGE las dann den alten
 * Wert. Die Regeln laufen weiter nach CBN_SELCHANGE/CBN_EDITCHANGE. */
static int combo_list_note(int id, int code)
{
    const uictl_t *u;
    if (code != CBN_DROPDOWN && code != CBN_CLOSEUP && code != CBN_SELENDOK && code != CBN_SELENDCANCEL) {
        return 0;
    }
    u = ui_find(id);
    return u && (u->kind == K_COMBO || u->kind == K_COMBOEDIT);
}

void on_command(int id, int code)
{
    if (hub_command(id, code)) { /* W470: Reiter der Bereiche Optionen/steelnet */
        return;
    }
    if (id == ID_NAVSUB_MATCH || id == ID_NAVSUB_ACC) { /* W399: Unterpunkte von Multiplayer */
        if (code == BN_CLICKED) {
            acc_nav_sub(id == ID_NAVSUB_ACC);
        }
        return;
    }
    if (id >= ID_NAV0 && id < ID_NAV0 + P_N) {
        if (id == ID_NAV0 + P_LNET && g_game == LGAME_LOC) {
            acc_nav_sub(0); /* W399: Elternpunkt = erster Unterpunkt (Match & network) */
            return;
        }
        show_page(id - ID_NAV0);
        return;
    }
    if (alpha_on_command(id, code)) { /* W272: Seite Alpha Test */
        return;
    }
    if (dg_on_command(id)) { /* W279: Absturz-Karte, Diagnosepaket */
        return;
    }
    on_command_inner(id, code);
    /* W171: nach jeder Bedienung Regelwerk neu (Freigaben + Begruendungen) */
    if (!g_updating && id != ID_CLOSE && id != ID_MIN && !combo_list_note(id, code)) { /* W486 */
        ui_apply_rules();
    }
}

static void on_command_inner(int id, int code)
{
    if (lset_command(id, code)) { /* W397: Zahnrad, Online & Updates, Spruenge */
        return;
    }
    if (loctab_command(id, code)) { /* W276: Spielauswahl, LoC-Seite, LoC-Startsperre */
        return;
    }
    if (orig_command(id, code)) { /* W278 ORIG-FLAG: Back to original, Einmal-Start */
        return;
    }
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
    case ID_UPD_YES: /* W250: Erstabfrage -> ja, gleich einmal pruefen */
        upd_set_cfg(1);
        set_status_k(LT("gui.upd_on"), 1);
        upd_start(0);
        ui_refresh_visibility();
        redraw_all();
        break;
    case ID_UPD_NO: /* W250: Erstabfrage -> nein (Standard) */
        upd_set_cfg(0);
        set_status_k(LT("gui.upd_off"), 1);
        ui_refresh_visibility();
        redraw_all();
        break;
    case ID_UPD_AUTO: /* W250: Schalter (wirkt sofort, eigene Zeile in user.cfg) */
        upd_set_cfg(!ui_checked(ID_UPD_AUTO));
        set_status_k(LT(g_upd_cfg == 1 ? "gui.upd_on" : "gui.upd_off"), 1);
        ui_refresh_visibility();
        redraw_all();
        break;
    case ID_UPD_NOW: /* W250: manuell, auch bei Schalter aus */
        upd_start(1);
        break;
    case ID_UPD_OPEN:
        upd_open_page();
        break;
    case ID_BG:
        g_bg_on = !g_bg_on;
        ui_set_checked(ID_BG, g_bg_on);
        ui_cfg_save_bg(g_client_dir, g_bg_on);
        base_build();
        redraw_all();
        break;
    case ID_LAYOUT: /* W281: Layout-Vorschau wirkt sofort, gemerkt in launcher_ui.cfg */
        if (code == CBN_SELCHANGE && !g_updating) {
            int k = ComboBox_GetCurSel(ctl(ID_LAYOUT));
            if (k >= 0 && k < LLAY_SZ_N) { /* W287: nur noch Groessen von Layout A */
                lay_switch(LLAY_A, k);
                if (g_o.shot == 0) {
                    lay_cfg_save();
                }
            }
        }
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
    case ID_HRTF: /* W356 AUDIO-2: Kopfhoerer (wirkt beim naechsten Start) */
        ui_set_checked(ID_HRTF, !ui_checked(ID_HRTF));
        set_status(LT(ui_checked(ID_HRTF) ? "gui.hrtf_on" : "gui.hrtf_off"));
        break;
    case ID_VOL_MASTER: /* W303 AUDIO-VOLUME */
    case ID_VOL_MUSIC:
    case ID_VOL_SFX:
    case ID_VOL_VOICE:
        if (code == CBN_SELCHANGE && !g_updating) {
            set_status(LT("gui.vol_status"));
        }
        break;
    case ID_PH_VIEW: /* W445: Reichweite eigene VT-Teile folgt der Ansicht (nur Cinematic) */
        if (code == CBN_SELCHANGE && !g_updating) {
            ui_apply_rules();
        }
        break;
    case ID_PH_CSELF: /* W445 */
        if (code == CBN_SELCHANGE && !g_updating) {
            set_status(LT("gui.extras_cself_txt"));
        }
        break;
    case ID_RP_LABEL: /* W290: Replay-Schalter (live, Foto, Video-Export) */
    case ID_RP_LOGO:
    case ID_WM_MEDIA:
    case ID_PH_HUD:
        ui_set_checked(id, !ui_checked(id));
        break;
    case ID_SAVE_LOCK: /* W190 SAVE-LOCK: wirkt ab dem naechsten Spielstart */
        ui_set_checked(ID_SAVE_LOCK, !ui_checked(ID_SAVE_LOCK));
        set_status_k(LT(ui_checked(ID_SAVE_LOCK) ? "gui.save_lock_txt" : "gui.save_lock_off"), 0);
        break;
    case ID_CTRL_SBC:
        ui_set_checked(ID_CTRL_SBC, !ui_checked(ID_CTRL_SBC));
        /* W170/B10, W171: Maus-Optionen folgen per Regel R23/R24 */
        break;
    case ID_CTRL_MOUSEOFF:
        ui_set_checked(ID_CTRL_MOUSEOFF, !ui_checked(ID_CTRL_MOUSEOFF));
        break;
    case ID_CTRL_AIMASSIST: /* W236 */
        if (code == CBN_SELCHANGE && !g_updating) {
            static const char *const kTip[3] = { "gui.ctl_assist0_on", "gui.ctl_assist1_on",
                                                 "gui.ctl_assist2_on" };
            int sel = ComboBox_GetCurSel(ctl(ID_CTRL_AIMASSIST));
            set_status(LT(kTip[(sel >= 0 && sel <= 2) ? sel : 0]));
            if (g_game == LGAME_LOC && sel > 0) set_status(LT("gui.loc_arcade_tip")); /* W440 */
        }
        break;
    case ID_STICK_AIM: /* W271 */
        if (code == CBN_SELCHANGE && !g_updating) {
            set_status(LT(ComboBox_GetCurSel(ctl(ID_STICK_AIM)) == 1 ? "gui.stick_rel_on"
                                                                     : "gui.stick_pos_on"));
        }
        break;
    case ID_CTRL_AUTOGEAR: /* W218 */
        ui_set_checked(ID_CTRL_AUTOGEAR, !ui_checked(ID_CTRL_AUTOGEAR));
        set_status(LT(ui_checked(ID_CTRL_AUTOGEAR) ? "gui.ctl_autogear_on" : "gui.ctl_simple_off"));
        if (g_game == LGAME_LOC && ui_checked(ID_CTRL_AUTOGEAR)) set_status(LT("gui.loc_arcade_tip")); /* W440 */
        break;
    case ID_CTRL_ALLY: /* W424 */
        ui_set_checked(ID_CTRL_ALLY, !ui_checked(ID_CTRL_ALLY));
        set_status(LT(ui_checked(ID_CTRL_ALLY) ? "gui.ctl_ally_on" : "gui.ctl_ally_off"));
        break;
    case ID_CTRL_DESPIKE: /* W439 */
        ui_set_checked(ID_CTRL_DESPIKE, !ui_checked(ID_CTRL_DESPIKE));
        set_status(LT(ui_checked(ID_CTRL_DESPIKE) ? "gui.ctl_despike_on" : "gui.ctl_despike_off"));
        break;
    case ID_PAL50: { /* W234 PAL50: Spieltakt 25/30, Bildratenliste neu beschriften */
        ui_set_checked(ID_PAL50, !ui_checked(ID_PAL50));
        ip_fill(ctl(ID_IPFPS)); /* W309: behaelt die Wahl */
        set_status(LT(ui_checked(ID_PAL50) ? "gui.pal50_on" : "gui.pal50_off"));
        break;
    }
    case ID_SHOWADV: /* W291 ESC-REWORK: Feinregler zeigen/verbergen, gemerkt in launcher_ui.cfg */
        g_show_adv = !g_show_adv;
        ui_set_checked(ID_SHOWADV, g_show_adv);
        ui_cfg_save_bg(g_client_dir, g_bg_on);
        lay_gfx_apply();
        show_page(g_page);
        set_status(LT(g_show_adv ? "gui.show_adv_on" : "gui.show_adv_off"));
        break;
    case ID_CHIDE: /* W268 COCKPIT-HIDE / W319 MONITOR-VIEW: eigene Option, nicht im Grafik-Preset */
        if (code == CBN_SELCHANGE && !g_updating) {
            static const char *const kSt[] = { "gui.chide_off", "gui.chide_on", "gui.mview_on", "gui.mview2_on" };
            int sel = ComboBox_GetCurSel(ctl(ID_CHIDE));
            if (sel >= 0 && sel <= 3) { set_status(LT(kSt[sel])); }
            redraw_rect(COL2, CY0, COLW, 488);
        }
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
    case ID_GP0 + LGP_PERFORMANCE:
    case ID_GP0 + LGP_CLEAR: /* W252 */
    case ID_GP0 + LGP_ENHANCED: /* W465 (nur LoC sichtbar) */
    case ID_GP0 + LGP_VIVID: /* W478 (nur LoC sichtbar) */
    case ID_GP_RESET: {
        lgp_mask_t lock = gfx_lock_now();
        int was = lcfg_gfx_preset(&g_gfx, lock);
        int sel = id == ID_GP_RESET ? lcfg_gfx_base(&g_gfx, lock) : id - ID_GP0;
        launcher_settings_t before;
        char m[400], nm[160];
        if (sel == was || !lcfg_gfx_visible(sel)) { /* W465: im aktiven Spiel nicht waehlbar */
            break;
        }
        snap_settings(&before);
        if (was == LGP_CUSTOM && id != ID_GP_RESET &&
            lcfg_gfx_match(&g_gfx, lock) == LGP_CUSTOM && /* W476: Custom mit Preset-Werten ersetzt nichts Eigenes */
            !ui_confirm(LT("gui.rule_r14"), LT("gui.gfx_preset_card"))) {
            break; /* R14: Rueckfrage "Nein" -> eigene Werte bleiben */
        }
        {
            int e0 = g_gfx.edge_mode, e1 = g_gfx.edge_fade_width, e2 = g_gfx.edge_fade_strength;
            lcfg_gfx_apply(&g_gfx, sel);
            if (pergame_box_169()) { /* W414: LoC 16:9 - gesperrte Raender (Fade out) bleiben */
                g_gfx.edge_mode = e0;
                g_gfx.edge_fade_width = e1;
                g_gfx.edge_fade_strength = e2;
            }
        }
        ip_set(g_gfx.interp_fps); /* W223, W309 */
        ComboBox_SetCurSel(ctl(ID_RTSCALE), g_gfx.rt_scale + 1);
        edge_ui_set(g_gfx.edge_mode, g_gfx.edge_fade_width, g_gfx.edge_fade_strength); /* W266 */
        update_aspect_info();
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
    case ID_GP0 + LGP_CUSTOM: { /* W476 LAUNCHER-CUSTOM-PRESET: Custom waehlen = aktuelle Werte unveraendert
                                  * als eigene uebernehmen (Bild bleibt), Wahl wird gemerkt (launcher_gfx_custom) */
        lgp_mask_t lock = gfx_lock_now();
        launcher_settings_t before;
        char m[400], nm[160];
        if (lcfg_gfx_preset(&g_gfx, lock) == LGP_CUSTOM) {
            gfx_sync_ui(); /* schon Custom: Auswahl bleibt */
            break;
        }
        snap_settings(&before);
        gfx_name(lcfg_gfx_match(&g_gfx, lock), &before, nm, sizeof nm);
        g_gfx.gfx_custom = 1;
        gfx_sync_ui();
        _snprintf_s(m, sizeof m, _TRUNCATE, LT("gui.gfx_custom_set"), nm);
        set_status_undo(m, &before);
        ui_refresh_visibility();
        redraw_all();
        break;
    }
    case ID_GFX_LIGHT: /* W173: Einzeloptionen des Presets (-> Benutzerdefiniert) */
    case ID_GFX_SHADOW:
    case ID_GFX_SOFT:
    case ID_GFX_FX:
    case ID_GFX_TEX:
    case ID_AAMODE:
    case ID_MONFX: /* W252: "Aus" = monitor_fx 0 (Preset Klare Sicht) */
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
            case ID_GFX_FX: lcfg_fx_apply_sel(&g_gfx, sel, g_game == LGAME_LOC); break; /* letzter Eintrag "Benutzerdefiniert" = eigene Werte bleiben (W466: LoC Original/Enhanced) */
            case ID_GFX_TEX: g_gfx.remaster = g_gfx.hd_textures = sel ? 1 : 0; break;
            case ID_MONFX:
                if (sel == 3) {
                    g_gfx.monitor_fx = 0;
                } else if (sel < 3) {
                    g_gfx.monitor_fx = 1;
                    g_monfx_grain = kMonFx[sel][0];
                    g_monfx_glitch = kMonFx[sel][1];
                }
                break;
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
    case ID_CPERSP: /* W191 UW-PERSP */
    case ID_CPANINI:
    case ID_CWIDTH: /* W235 */
    case ID_EDGE_W:
    case ID_EDGE_S:
        if (code == CBN_SELCHANGE && !g_updating) {
            update_aspect_info();
            ui_refresh_visibility(); /* W296 PRESET-ORIG: Verlaufszeile bei "Fade out" */
            redraw_rect(COL2, CY0, COLW, 488);
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
    case ID_IPFPS: /* W309 FPS-MONITOR: Hinweis zur gewaehlten Rate */
        if (code == CBN_SELCHANGE && !g_updating) {
            ip_status();
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
            if (code == EN_KILLFOCUS && id == ID_MEDIA && !g_updating) {
                char m[1024]; /* W282: getippter Ordner loest ungueltiges Abbild ab */
                ui_gettext(ctl(ID_MEDIA), m, sizeof m);
                if (m[0]) {
                    media_drop_bad_image();
                }
            }
            redraw_all();
        } else if (code == EN_CHANGE && id == ID_MEDIA && !g_updating) {
            xbe_cue_update(); /* W172: "automatisch: <Ordner>\default.xbe" */
        } else if (code == EN_CHANGE && id == ID_IMAGE && g_page == P_PLAY) {
            /* W220: Pruefergebnis gilt nur fuer den gepruefen Pfad */
            redraw_rect(CX0, CY0 + 310, CW, 50);
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
    case ID_SAVES_IMPORT: adv_saves_transfer(0); break;
    case ID_SAVES_EXPORT: adv_saves_transfer(1); break;
    case ID_SAVES_REPLAYS: replay_dialog_open(); break; /* W254 */
    case ID_SAVES_RESET: { /* W187 KIA-LOAD */
        char path[1100];
        adv_hdd_path(path, sizeof path);
        adv_saves_reset_at(path, 1);
        break;
    }
    case ID_PH_BROWSE: { /* W195 PHOTO-MODE: Fotoordner waehlen */
        BROWSEINFOW bi;
        LPITEMIDLIST pidl;
        wchar_t wd[MAX_PATH], title[160];
        memset(&bi, 0, sizeof bi);
        bi.hwndOwner = g_hwnd;
        u8_to_w(LT("gui.extras_pick"), title, 160);
        bi.lpszTitle = title;
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        if (g_o.shot != 0) { /* Offscreen-/Testbetrieb: kein Dialog */
            printf("[browse] photo_dir\n");
            break;
        }
        pidl = SHBrowseForFolderW(&bi);
        if (pidl) {
            if (SHGetPathFromIDListW(pidl, wd)) { SetWindowTextW(ctl(ID_PH_DIR), wd); }
            CoTaskMemFree(pidl);
        }
        break;
    }
    case ID_PH_OPEN: { /* W195: Fotoordner im Explorer (fehlt er noch: Hinweis, er entsteht mit dem ersten Foto) */
        char path[700];
        extras_dir(path, sizeof path);
        if (!is_dir_u8(path)) {
            set_status_k(LT("gui.extras_noopen"), 1);
        } else if (g_o.shot != 0) {
            printf("[open] %s\n", path);
        } else {
            wchar_t wp[700];
            u8_to_w(path, wp, 700);
            ShellExecuteW(g_hwnd, L"open", wp, NULL, NULL, SW_SHOWNORMAL);
        }
        break;
    }
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
    case ID_CTRL_EXPORT: /* W419 CTRL-EXPORT */
        ctrl_export();
        break;
    case ID_CTRL_IMPORT:
        ctrl_import();
        break;
    case ID_CTRL_JOY: /* W221 HOTAS (experimentell) */
        joy_dialog_modal();
        joy_card_scan(); /* W282: Karte folgt dem Dialog (Neu suchen) */
        if (update_pad_status()) {
            redraw_rect(CX0, CY0 + 392, COLW, 104);
        }
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
#if !SB_EDITION_COMMUNITY
        packs_creator_open();
#endif
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
            disk_base_sync(&g_s); /* W259: Profilwerte gelten als Launcher-Aenderung */
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

/* W281 LAUNCHER-SIZE: Geometrie-/Schrift-DPI aus Monitor-DPI und Layout. */
static void lay_set_dpi(int mon)
{
    g_dpi_mon = mon;
    if (g_lay.id == LLAY_0) {
        g_dpi = mon;
        g_dpi_font = mon;
    } else {
        g_dpi = MulDiv(mon, g_lay.geo_pct, 100);
        g_dpi_font = MulDiv(mon, g_lay.font_pct, 100);
    }
}

/* W281: Arbeitsflaeche des Monitors unter dem Fenster (bzw. Hauptmonitor). */
static void lay_work_area(HWND h, RECT *wa)
{
    MONITORINFO mi;
    HMONITOR m = h ? MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST)
                   : MonitorFromPoint((POINT){ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    if (m && GetMonitorInfoW(m, &mi)) {
        *wa = mi.rcWork;
    } else {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, wa, 0);
    }
}

/* W281: Layout (neu) anwenden: Tabelle -> Einpassen -> DPI. Ohne Fenster
 * (h = NULL) nur rechnen. Shots/Tests (offscreen) passen nur bei
 * --work WxH ein, damit die Bilder nicht vom Testrechner abhaengen. */
int g_lay_work_w, g_lay_work_h; /* --work (Test), 0 = echte Arbeitsflaeche */
static void lay_choose(int id, int size, int mon_dpi, HWND h, int offscreen)
{
    llay_init(&g_lay, id, size);
    if (g_lay_work_w > 0 && g_lay_work_h > 0) {
        llay_fit(&g_lay, mon_dpi, g_lay_work_w, g_lay_work_h);
    } else if (!offscreen) {
        RECT wa;
        lay_work_area(h, &wa);
        llay_fit(&g_lay, mon_dpi, wa.right - wa.left, wa.bottom - wa.top);
    }
    lay_set_dpi(mon_dpi);
}

static void on_dpi(int dpi)
{
    if (g_lay.id != LLAY_0) { /* W281: Skalierung je Monitor neu einpassen */
        int tw = g_lay.tw, th = g_lay.th;
        lay_choose(g_lay.id, g_lay.size, dpi, g_hwnd, g_o.shot != 0);
        if (lay_b() && tw <= g_lay.tw && th <= g_lay.th) { /* gezogene Groesse behalten */
            g_lay.tw = tw < g_lay.min_w ? g_lay.min_w : tw;
            g_lay.th = th < g_lay.min_h ? g_lay.min_h : th;
        }
    } else {
        g_dpi = dpi;
        g_dpi_mon = dpi;
        g_dpi_font = dpi;
    }
    fonts_make();
    ui_relayout();
    base_build();
}

/* W281: Layout zur Laufzeit wechseln (Vorschau-Kombi, Selbsttest): wie ein
 * DPI-Wechsel - Schriften, Elemente, Grundbild, Fenstergroesse. */
void lay_switch(int id, int size)
{
    RECT wr;
    int W, H;
    DWORD st;
    lay_choose(id, size, g_dpi_mon, g_hwnd, g_o.shot != 0);
    fonts_make();
    ui_relayout();
    base_build();
    if (g_hwnd) {
        st = (DWORD)GetWindowLongPtrW(g_hwnd, GWL_STYLE);
        st = lay_b() ? (st | WS_THICKFRAME | WS_MAXIMIZEBOX) : (st & ~(DWORD)(WS_THICKFRAME | WS_MAXIMIZEBOX));
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, (LONG_PTR)st);
        GetWindowRect(g_hwnd, &wr);
        W = SX(lay_tw());
        H = SX(lay_th());
        if (g_o.shot == 0) { /* sichtbar: auf der Arbeitsflaeche halten */
            RECT wa;
            lay_work_area(g_hwnd, &wa);
            if (wr.left + W > wa.right) {
                wr.left = wa.right - W;
            }
            if (wr.top + H > wa.bottom) {
                wr.top = wa.bottom - H;
            }
            if (wr.left < wa.left) {
                wr.left = wa.left;
            }
            if (wr.top < wa.top) {
                wr.top = wa.top;
            }
        }
        SetWindowPos(g_hwnd, NULL, wr.left, wr.top, W, H,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        ui_refresh_visibility();
        lay_combo_fill(ctl(ID_LAYOUT));
        redraw_all();
    }
}

/* W281: Layout-Wahl: --layout/--ui-size (CLI) > launcher_ui.cfg > 0/normal.
 * gui_layout_override gilt auch fuer --selftest (g_o ist dort leer). */
char g_lay_ovr[4], g_lay_ovr_size[12];
void gui_layout_override(const char *layout, const char *size)
{
    strncpy_s(g_lay_ovr, sizeof g_lay_ovr, layout ? layout : "", _TRUNCATE);
    strncpy_s(g_lay_ovr_size, sizeof g_lay_ovr_size, size ? size : "", _TRUNCATE);
}

int lay_cfg_id(void)
{
    char v[32];
    int id = llay_parse(g_o.layout);
    if (id < 0) {
        id = llay_parse(g_lay_ovr);
    }
    if (id >= 0) {
        return id;
    }
    ui_cfg_load_str(g_client_dir, "launcher_layout", v, sizeof v);
    id = llay_parse(v);
    if (id == LLAY_0 || id == LLAY_B) { /* W287: alte Vorschau-Wahl 0/b -> A (Groesse bleibt, beim naechsten Speichern "a") */
        id = LLAY_A;
    }
    g_lay_cfg_id = id;
    /* Nutzer 15.11.: ohne Wahl gilt Layout A in Groesse Normal (W281-Auswahl) */
    return id >= 0 ? id : LLAY_A;
}

int lay_cfg_size(void)
{
    char v[32];
    int sz = llay_size_parse(g_o.ui_size);
    if (sz < 0) {
        sz = llay_size_parse(g_lay_ovr_size);
    }
    if (sz >= 0) {
        return sz;
    }
    ui_cfg_load_str(g_client_dir, "ui_size", v, sizeof v);
    sz = llay_size_parse(v);
    g_lay_cfg_size = sz;
    return sz >= 0 ? sz : LLAY_SZ_NORMAL;
}

static void lay_cfg_save(void)
{
    g_lay_cfg_id = g_lay.id;
    g_lay_cfg_size = g_lay.size;
    ui_cfg_save_bg(g_client_dir, g_bg_on);
}

/* W281: Shot-Dateiname je Layout ("" = heute, unveraenderte Namen). */
static const char *lay_shot_tag(void)
{
    static char t[24];
    if (g_lay.id == LLAY_0) {
        return "";
    }
    _snprintf_s(t, sizeof t, _TRUNCATE, "_%s%s", llay_name(g_lay.id),
                g_lay.size == LLAY_SZ_NORMAL ? "" : g_lay.size == LLAY_SZ_COMPACT ? "c" : "l");
    return t;
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (hub_wndproc(msg, wp, lp)) { /* W470: Lobbyliste der steelnet-Seite (Timer, Ergebnis) */
        return 0;
    }
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
        if (g_lay.resizable && !IsZoomed(h)) { /* W281: b ziehbar (randlos: eigene Raender) */
            RECT cr;
            int e = SX(6), l, r, t, b;
            GetClientRect(h, &cr);
            l = p.x < e;
            r = p.x >= cr.right - e;
            t = p.y < e;
            b = p.y >= cr.bottom - e;
            if (t && l) { return HTTOPLEFT; }
            if (t && r) { return HTTOPRIGHT; }
            if (b && l) { return HTBOTTOMLEFT; }
            if (b && r) { return HTBOTTOMRIGHT; }
            if (l) { return HTLEFT; }
            if (r) { return HTRIGHT; }
            if (t) { return HTTOP; }
            if (b) { return HTBOTTOM; }
        }
        if (hdr_hit(p) >= 0) {
            return HTCLIENT; /* W401: Status-/Update-Chip anklickbar */
        }
        if (p.y >= 0 && p.y < SX(TITLE_H)) {
            return HTCAPTION; /* Ziehen, Aero-Snap */
        }
        return HTCLIENT;
    }
    case WM_SETCURSOR: /* W401: Handcursor auf den Chips im Kopf */
        if ((HWND)wp == h && LOWORD(lp) == HTCLIENT) {
            DWORD mp = GetMessagePos(); /* W535: Position der ausloesenden Nachricht statt GetCursorPos */
            POINT p = { GET_X_LPARAM(mp), GET_Y_LPARAM(mp) };
            ScreenToClient(h, &p);
            if (hdr_hit(p) >= 0) {
                SetCursor(LoadCursor(NULL, IDC_HAND));
                return TRUE;
            }
        }
        break;
    case WM_LBUTTONUP: { /* W401: Klick auf einen Chip (Update -> Release-Seite, Status -> Zahnrad) */
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int k = hdr_hit(p);
        if (k >= 0) {
            hdr_click(k);
            return 0;
        }
        break;
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
        if (HIWORD(wp) == 1 && lp == 0) { /* W535: Tastenkuerzel aus g_kbacc */
            acc_command(LOWORD(wp));
            return 0;
        }
        on_command(LOWORD(wp), HIWORD(wp));
        return 0;
    case DM_GETDEFID:
        return MAKELRESULT(ID_START_BIG, DC_HASDEFID);
    case WM_TIMER:
        if (wp == 1 && g_page == P_CTRL && update_pad_status()) {
            redraw_rect(CX0, CY0 + 392, COLW, 104);
        }
        if (wp == 7) {
            client_exit_poll(); /* W231 */
        }
        if (wp == NRQ_CT_TIMER) {
            nrq_ct_tick(); /* W522: Ende des LoC-Spiels einordnen */
        }
        if (wp == UPD_TIMER) {
            upd_timeout(); /* W250 */
        }
#if defined(SB_HAVE_LOCTP) && SB_HAVE_LOCTP
        if (wp == LOCTP_TIMER) {
            loctp_tick(); /* W336: Testpartner-Zustand */
        }
#endif
        if (wp == ONL_TIMER) {
            onl_tick(); /* W334: loc_net_status.txt */
        }
        if (wp == ACC_TIMER) {
            acc_name_tick(); /* W418: Namenspruefung beim Tippen */
        }
        if (wp == ACC_GT_TIMER) {
            acc_gt_tick(); /* W487: Gamertag-Pruefung beim Tippen */
        }
        if (wp == ACC_HL_TIMER) {
            acc_hl_tick(); /* W502: Hervorhebung der Zielkarte beenden */
        }
        if (wp == SNP_TIMER) {
            snp_tick(); /* W403: steelnet-Chip, alle 60 s sichtbar bzw. 5 min minimiert (nur Stufe steelnet) */
        }
        return 0;
    case WM_APP_SNP: { /* W403: Ergebnis der steelnet-Pruefung */
        snp_job_t *j = (snp_job_t *)lp;
        if (j && (int)wp == g_snp_gen && g_snp_busy) {
            g_snp_busy = 0;
            snp_apply(&j->r);
        }
        free(j);
        return 0;
    }
    case WM_APP_CAMP: /* W491: Ergebnis der Kampagnen-Erreichbarkeit */
        camp_msg(lp, wp);
        return 0;
    case WM_APP_ACC: { /* W328: Ergebnis eines Konto-Auftrags */
        acc_job_t *j = (acc_job_t *)lp;
        if (j && (int)wp == g_acc_gen && g_acc_busy) {
            acc_done(j);
        }
        if (j) {
            SecureZeroMemory(j, sizeof *j);
            free(j);
        }
        return 0;
    }
    case WM_APP_UPD: { /* W250: Ergebnis des Abruf-Threads */
        upd_job_t *j = (upd_job_t *)lp;
        if (j && (int)wp == g_upd_gen && g_upd_busy) {
            upd_done(&j->r);
        }
        free(j);
        return 0;
    }
    case WM_DPICHANGED:
        if (!g_o.force_dpi) {
            RECT *r = (RECT *)lp;
            on_dpi(HIWORD(wp));
            if (g_lay.id != LLAY_0) { /* W281: Groesse aus dem Layout (eingepasst) */
                SetWindowPos(h, NULL, r->left, r->top, SX(lay_tw()), SX(lay_th()),
                             SWP_NOZORDER | SWP_NOACTIVATE);
            } else {
                SetWindowPos(h, NULL, r->left, r->top, r->right - r->left,
                             r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            }
            redraw_all();
        }
        return 0;
    case WM_GETMINMAXINFO: /* W281: Mindestgroesse b */
        if (g_lay.resizable) {
            MINMAXINFO *mm = (MINMAXINFO *)lp;
            mm->ptMinTrackSize.x = SX(g_lay.min_w);
            mm->ptMinTrackSize.y = SX(g_lay.min_h);
            return 0;
        }
        break;
    case WM_SIZE: /* W281: b passt das Layout der neuen Groesse an */
        if (lay_b() && wp != SIZE_MINIMIZED) {
            int w = LOWORD(lp), hh = HIWORD(lp);
            if (w > 0 && hh > 0 && (w != SX(lay_tw()) || hh != SX(lay_th()))) {
                llay_resize_px(&g_lay, g_dpi_mon, w, hh);
                ui_relayout();
                base_build();
                redraw_all();
            }
        }
        break;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        KillTimer(h, 1);
        KillTimer(h, 7);
        KillTimer(h, UPD_TIMER);
#if defined(SB_HAVE_LOCTP) && SB_HAVE_LOCTP
        KillTimer(h, LOCTP_TIMER);
        loctp_shutdown(); /* W336: Launcher zu = Testpartner weg (zusaetzlich Job-Objekt) */
#endif
        if (g_upd_ask && g_o.shot == 0) {
            upd_set_cfg(0); /* W250: Schliessen ohne Antwort = Nein (nie wieder ungefragt) */
        }
        if (g_nrq_open && g_o.shot == 0) {
            nrq_dismiss(); /* W496: Fenster zu ohne Antwort = Nein, naechster Start fragt hoechstens noch einmal */
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static int g_inited;

int gui_init(const char *client_override)
{
    WNDCLASSW wc;
    INITCOMMONCONTROLSEX icc;
    lcfg_gfx_bind_game(&g_game, LGAME_LOC); /* W465: Preset-Erkennung folgt dem gewaehlten Spiel */
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
        mon_detect_rates(); /* W309: Hoechstrate + weitere Monitore; --sim-monitors */
    }
    g_inited = 1;
    return 1;
}

/* Fenster erzeugen (unsichtbar). offscreen = Test-/Shot-Betrieb. */
HWND gui_create(int offscreen)
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
    lay_choose(lay_cfg_id(), lay_cfg_size(), g_dpi, NULL, offscreen); /* W281 */
    fonts_make();
    W = SX(lay_tw());
    H = SX(lay_th());
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
                                 WS_MINIMIZEBOX | WS_CLIPCHILDREN |
                                 (g_lay.resizable ? WS_THICKFRAME | WS_MAXIMIZEBOX : 0), /* W281 */
                             x, y, W, H, NULL, NULL, GetModuleHandleW(NULL),
                             NULL);
    if (!g_hwnd) {
        return NULL;
    }
    if (lupd_version()[0]) { /* W301 BETA-VERSION: Release-Version im Fenstertitel (Taskleiste) */
        char lb[96];
        wchar_t wt[128], wl[96];
        sb_version_label(lb, sizeof lb, lupd_version());
        if (MultiByteToWideChar(CP_UTF8, 0, lb, -1, wl, 96)) {
            _snwprintf_s(wt, 128, _TRUNCATE, L"Steel Battalion \x2013 Launcher (%s)", wl);
            SetWindowTextW(g_hwnd, wt);
        }
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
        if (d >= 96 && d <= 576 && d != g_dpi_mon) {
            lay_choose(g_lay.id, g_lay.size, d, g_hwnd, offscreen); /* W281 */
            fonts_make();
            W = SX(lay_tw());
            H = SX(lay_th());
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
    { "performance", "", 0 }, /* W207: shared preset below */
    { "clearview", "", 0 }, /* W252: Klare Sicht */
    { "nodata", "game_image=;xbe_path=;media_root=", 0 },
    { "fullscreen", "window_mode=2;fs_hz=144;interp_fps=120", 0 },
    { "window43", "window_mode=0;width=1600;height=1200", 0 },
    { "controller", "sbc_usb=1;sbc_mouse_off=1;input_profile=", 0 },
    { "custom", "remaster=1;hd_textures=1;fx_enhanced=0;aa_mode=2;shading=0", 0 },
    { "diag", "audio=0", 1 },
    /* W220 PLAY-LAYOUT: Einrichtungskarte je Zustand (Pruef-Fixture, kein
     * echtes Abbild; Pfad bereinigt), img_browse = Uebergang nach Durchsuchen */
    { "img_empty", "game_image=;xbe_path=;media_root=", 0 },
    { "img_unchecked", "game_image=;xbe_path=;media_root=", 0 },
    { "img_ok", "game_image=;xbe_path=;media_root=", 0 },
    { "img_version", "game_image=;xbe_path=;media_root=", 0 },
    { "img_folder", "game_image=;xbe_path=;media_root=", 0 },
    { "img_browse", "game_image=;xbe_path=;media_root=", 0 },
    { "img_browse_bad", "game_image=;xbe_path=;media_root=", 0 },
    { "img_loc", "game_image=;xbe_path=;media_root=", 0 }, /* W369: LoC-xbe in der SB-Karte */
    /* W227 LAUNCHER-POLISH: startbereit mit Beispiel-Abbild (Fixture, bereinigt) */
    { "ready", "xbe_path=;media_root=", 0 },
    { "ready_hover", "xbe_path=;media_root=", 0 }, /* W398: wie ready, Maus auf dem inaktiven Spielnamen */
    { "ready_pg", "xbe_path=;media_root=", 0 }, /* W402: SB mit eigenen Werten (2560x1440, 120 FPS, PAL, Zielhilfe) */
    { "ready_us", "xbe_path=;media_root=", 0 },  /* W231: US-Beispielabbild */
    { "exit8_us", "xbe_path=;media_root=", 0 },  /* W231: + Meldung Exitcode 8 */
    { "ready_controller", "xbe_path=;media_root=;sbc_usb=1;sbc_mouse_off=1;input_profile=", 0 },
    /* W250 UPDATE-CHECK: Erstabfrage / Hinweis neue Version / Build ohne Version
     * (Version und Ergebnis sind Fixtures, kein Netz) */
    { "update_ask", "xbe_path=;media_root=", 0 },
    { "update_new", "xbe_path=;media_root=", 0 },
    { "update_dev", "", 0 },
    { "update_steel", "xbe_path=;media_root=", 0 }, /* W397: Stufe steelnet + neue Version (Kopf zweizeilig) */
    { "update_unk", "xbe_path=;media_root=", 0 },   /* W401: Stufe steelnet, Zustand unbekannt, kein Update */
    { "update_lost", "xbe_path=;media_root=", 0 },  /* W401: Stufe steelnet, Verbindung verloren */
    { "update_snp_on", "xbe_path=;media_root=", 0 },  /* W403: Stufe steelnet, Pruefung ok (gruen) + neue Version */
    { "update_snp_cnt", "xbe_path=;media_root=", 0 }, /* steelnet ok mit Live-Zahlen (5/2/1, Attrappe) + neue Version */
    { "update_snp_off", "xbe_path=;media_root=", 0 }, /* W403: Stufe steelnet, Pruefung nicht erreichbar */
    { "update_snp_verold", "xbe_path=;media_root=", 0 }, /* W421: SB-Tab, steelnet verlangt 0.3.3 (SB unberuehrt) */
    { "update_offl", "xbe_path=;media_root=", 0 },  /* W401: Stufe offline (Release-Version) */
    { "update_net0", "xbe_path=;media_root=", 0 },  /* W470: steelnet ok, 0 offene Lobbys (Attrappe) */
    { "update_net1", "xbe_path=;media_root=", 0 },  /* W470: steelnet ok, 1 Lobby */
    { "update_net6", "xbe_path=;media_root=", 0 },  /* W470: steelnet ok, 6 Lobbys (5 offen, 1 laeuft) */
    { "update_netns", "xbe_path=;media_root=", 0 }, /* W470: steelnet ok, Server ohne Lobbyliste (404) */
    { "update_netsi", "xbe_path=;media_root=", 0 }, /* W472: steelnet ok, ohne Konto -> Hinweis + Knopf zur Konto-Seite */
    { "update_netaf", "xbe_path=;media_root=", 0 }, /* W472: steelnet ok, Anmeldung abgelehnt (auth_failed) */
    { "update_netus", "xbe_path=;media_root=", 0 }, /* W473: steelnet ok, Konto nicht gesichert (Zeile Konto warnfarben) */
    { "replays", "", 0 },   /* W254: Seite Erweitert + Replay-Dialog (Beispieldaten) */
    { "alpha_demo", "", 0 }, /* W272: Alpha Test mit Beispielwahl + Schalter interp_guard (Layout) */
    { "advanced", "", 0 }, /* W291: Anzeige/Grafik mit "Erweiterte Einstellungen zeigen" */
    { "fade", "xbe_path=;media_root=;edge_mode=2", 0 }, /* W296: Breitbild "Fade out" mit Verlaufszeile ohne Erweitert */
    { "fpslist", "xbe_path=;media_root=;window_mode=0;interp_fps=120", 0 }, /* W309: Bildratenliste offen (120 gewaehlt) */
    { "fpslist_auto", "xbe_path=;media_root=;window_mode=0;interp_fps=-1", 0 }, /* W309: Bildratenliste offen (auto) */
    { "crash", "xbe_path=;media_root=", 0 }, /* W279: Karte "The last session crashed" (Fixture) */
    { "gfx_custom", "xbe_path=;media_root=;launcher_gfx_custom=1", 0 }, /* W476: Custom ausdruecklich gewaehlt (Werte Original) */
    { "loctp_err", "", 0 }, /* W344: Testpartner-Dialog im Fehlerfall (nur interne Ausgabe, Attrappe) */
    { "cx_import", "", 0 }, /* W419: Steuerung importiert (Beispieldatei, Temp-Client-Ordner) */
    { "cx_bad", "", 0 },    /* W419: abgeschnittene Datei abgelehnt */
    { "pack2", "", 0, 1 },  /* W172: zweites Pack gewaehlt (verdeckt durch ...) */
    { "pack3", "", 0, 2 },  /* W172: drittes Pack (Effekt-Pack mit Schloss) */
    /* W173: Remastered dezent, Effect Pack setzt color_sat/fx_embers (Effect-Pack-
     * Ausnahme: Anzeige bleibt "dezent", Schloss an Preset und Effekten) */
    /* W276 LOC-LAUNCH: Bereich Line of Contact (Pruef-Fixture, bereinigte Pfade) */
    { "loc_empty", "", 0 },
    { "loc_ok", "", 0 },
    { "loc_enh", "", 0 }, /* W465: wie loc_169 mit den Werten einer frischen LoC-Installation (Preset Enhanced) */
    { "loc_vivid", "", 0 }, /* W478: wie loc_enh mit Preset Enhanced Vivid */
    { "loc_adv", "", 0 }, /* W427: wie loc_ok mit "Erweiterte Einstellungen zeigen" (gesperrte Zeilen sichtbar) */
    { "loc_hover", "", 0 }, /* W398: wie loc_ok, Maus auf dem inaktiven Spielnamen */
    { "loc_pg", "", 0 }, /* W402: LoC mit eigenen Werten (16:9, 1920x1080, 60 FPS) */
    { "loc_sb", "", 0 },
    { "loc_start", "", 0 },
    { "loc_exp", "edge_mode=2;cockpit_hide=1", 0 }, /* W293: wie loc_ok, mit Hinweisband (Breitbild, Cockpit aus) */
    { "loc_wide", "", 0 }, /* W326: wie loc_ok, Bildformat "16:9 (Spielmodus)" (Seite Erweitert) */
    { "loc_169", "edge_mode=2;edge_fade_width=8;edge_fade_strength=100", 0 }, /* W414: LoC-Standard 16:9 + Fade out */
    /* W328 STEELNET-ACCOUNT: Pilotenkonto (Fixture, kein Netz, nichts gespeichert; Seite Erweitert) */
    { "loc_acc_none", "", 0 },   /* Dienst eingerichtet, noch kein Pilot (Name + Anlegen) */
    { "loc_acc_unsec", "", 0 },  /* Pilot angelegt, nicht gesichert (Hinweis) */
    { "loc_acc_ok", "", 0 },     /* Pilot mit Discord + Login gesichert */
    { "loc_acc_view", "", 0 },   /* Verwaltungsansicht (Code angezeigt) */
    { "loc_acc_signin", "", 0 }, /* Ansicht "Auf diesem PC anmelden" (ohne Pilot) */
    { "loc_acc_view_unsec", "", 0 }, /* W399: Unterseite Account, ungesichert (Discord-Knopf im Status) */
    { "loc_acc_view_login", "", 0 }, /* W399: Unterseite Account, nur mit Login gesichert */
    { "loc_acc_view_ok", "", 0 },    /* W399: Unterseite Account, Discord + Login, Code verdeckt */
    { "loc_acc_view_manage", "", 0 }, /* W418: gesichert, Verwalten aufgeklappt */
    { "loc_acc_view_net", "", 0 },    /* W418: Konto da, Server nicht erreichbar (Status unbekannt) */
    { "loc_acc_signin_code", "", 0 }, /* W418: kein Konto, "Mit einem Code" aufgeklappt */
    { "loc_acc_view_unsec_login", "", 0 }, /* W418: ungesichert, "Login und Passwort festlegen" offen */
    { "loc_acc_view_gt", "", 0 },       /* W487: gesichert, Gamertag-Karte offen (naechste Aenderung in 3 Tagen) */
    { "loc_acc_view_nogt", "", 0 },     /* W487: gesichert, noch kein Gamertag, Gamertag-Karte offen */
    { "loc_acc_view_unsec_gt", "", 0 }, /* W487: ungesichert, Gamertag-Karte offen */
    { "loc_acc_nogt", "", 0 },          /* W487: Karte Match & network, gesichert ohne Gamertag */
    { "loc_acc_new_gt", "", 0 },        /* W487: Kontoseite "Ich bin neu" mit Gamertag-Vorschlag */
    { "loc_acc_view_unsec_nogt", "", 0 }, /* W502: ungesichert, ohne Gamertag (Fall des Nutzers) */
    { "loc_acc_view_gt_edit", "", 0 },  /* W502: gesichert, Gamertag aendern offen */
    { "loc_acc_view_gt_taken", "", 0 }, /* W502: ohne Gamertag, Pruefmeldung "vergeben" unter dem Feld */
    { "loc_acc_view_gtoff", "", 0 },    /* W502: Dienst ohne Gamertags */
    { "loc_acc_view_off", "", 0 },      /* W502: steelnet in dieser Ausgabe nicht eingerichtet */
    { "loc_acc_view_code", "", 0 },     /* W502: Kontoeinstellungen, Codeformular mit Formatfehler */
    { "loc_acc_view_login_err", "", 0 }, /* W502: ungesichert, Login-Formular mit Fehlermeldung */
    { "loc_acc_signin_login", "", 0 },  /* W502: kein Konto, Anmelden mit Login offen */
    /* W334 LOC-SEC: Karte "Online (steelnet)" (Dienst als eingerichtet vorgegeben, nichts gespeichert) */
    { "loc_onl", "", 0 },     /* Online an, Raumcode der eigenen Lobby (Kopieren) */
    { "loc_onl_err", "", 0 }, /* Online an, sec=pin_failed (Server-Pruefung fehlgeschlagen) */
    { "loc_onl_pub", "", 0 },  /* W373: Online an, oeffentlich (Standard), Vertrauenshinweis */
    { "loc_onl_priv", "", 0 }, /* W373: Online an, privat mit Raumcode (noch keine Lobby) */
    { "loc_onl_verold", "", 0 }, /* W421: steelnet verlangt 0.3.3, Launcher 0.3.2 -> LoC-Start gesperrt */
    { "loc_onl_vercur", "", 0 }, /* W421: steelnet verlangt 0.3.2 = eigene Version -> frei */
    /* W392 LAUNCHER-NETTAB: Seite Netzwerk (--ui-shot packs = Platz der Seite bei LoC) */
    { "loc_net", "", 0 },     /* Standard: alles an, Gegner glaetten Auto */
    { "loc_net_off", "", 0 }, /* Rollback aus: abhaengige Schalter grau, Hinweis auf Spielen */
    { "loc_net_priv", "", 0 }, /* W450: Seite Netzwerk + Datenschutz-Fenster (zweites Bild) */
    { "loc_net_chat", "", 0 },    /* W473: Netzwerk-Karte, Reiter "Lobby & chat" (Standard) */
    { "loc_net_chatoff", "", 0 }, /* W473: Reiter "Lobby & chat", Chat und steelnet-Chat aus (abhaengige grau) */
    { "loc_net_lan", "", 0 },     /* W473: LAN-Modus (Karte Match: Verbindung LAN, Raum, Passwort) */
    { "loc_net_lanchat", "", 0 }, /* W473: LAN-Modus, Reiter "Lobby & chat" (steelnet grau) */
    { "loc_net_live", "", 0 },      /* W487/W501: Online mit Gamertag (System Link + Xbox Live, Kampagne) */
    { "loc_net_live_campx", "", 0 },   /* W491: Live bereit, Kampagnenserver nicht erreichbar */
    { "loc_net_live_campold", "", 0 }, /* W491: Live bereit, Launcher veraltet (Mindestversion 0.3.3) */
    { "loc_net_live_campt", "", 0 },   /* W491: intern Testkampagne, Reiter Lobby & Chat */
    { "loc_net_live_nogt", "", 0 }, /* W487/W501: Online ohne Gamertag (nur System Link, Hinweis; Start frei) */
    { "loc_acc_dc", "", 0 },      /* W473: Konto-Karte gesichert nur mit Discord */
    { "loc_acc_lg", "", 0 },      /* W473: Konto-Karte gesichert nur mit Login */
    { "loc_acc_net", "", 0 },     /* W473: Konto-Karte, Server nicht erreichbar (Status unbekannt) */
    { "loc_netq", "", 0 },    /* W397: Erstabfrage "Online & Updates" auf LoC-Spielen (Release-Version) */
    { "loc_net_nrqask", "", 0 }, /* W496: Dialog "anonyme Netzwerkstatistik" (Stufe steelnet, nie beantwortet) */
    { "loc_net_nrq", "", 0 },    /* W496: Netzwerk-Seite mit Schalter an (Zustimmung) */
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

void w220_fixture(const char *path, int rc);

static void gui_apply_state(const char *name)
{
    int i;
    acc_fixture(1, 0, 0, 0, 0); /* W397: Shots zeigen nie das echte Konto (Name!), loc_acc_* setzt eigene Fixtures */
    if (!loctab_apply_state(name)) { /* W276: loc_* */
        /* W398: SB-Shots immer mit SB aktiv und ohne Stufe steelnet (nie Spielwahl/Online aus der echten cfg);
         * update_steel setzt die Stufe unten selbst */
        g_game = LGAME_SB;
        g_loc.game = LGAME_SB;
        g_loc_online = 0;
    }
    if (strcmp(name, "ready_us") == 0 || strcmp(name, "exit8_us") == 0) {
        /* W231: startbereit mit US-Beispielabbild (Fixture, bereinigt) */
        w220_fixture(W220_ISO_US, 0);
        strcpy_s(g_s.game_image, sizeof g_s.game_image, W220_ISO_US);
    } else if (strncmp(name, "ready", 5) == 0 || strncmp(name, "update_", 7) == 0 || strcmp(name, "crash") == 0) {
        /* W227: Fixture vor dem Regelwerk (W250: auch update_*) */
        w220_fixture(W220_ISO, 0);
        strcpy_s(g_s.game_image, sizeof g_s.game_image, W220_ISO);
    }
    if (strcmp(name, "loc_enh") == 0 || strcmp(name, "loc_vivid") == 0) { /* W465: frische LoC-Installation - Preset
                                                                                * Enhanced (W478: Vivid), Raender Fade out (16:9) */
        lcfg_gfx_apply(&g_s, name[4] == 'v' ? LGP_VIVID : LGP_ENHANCED);
        g_s.edge_mode = LGAME_LOC169_EDGE_MODE;
        g_s.edge_fade_width = LGAME_LOC169_EDGE_W;
        g_s.edge_fade_strength = LGAME_LOC169_EDGE_S;
    }
    if (strcmp(name, "ready_pg") == 0 || strcmp(name, "loc_pg") == 0) { /* W402: je Spiel eigene Werte (nur Speicher) */
        int loc = name[0] == 'l';
        g_s.window_mode = 0;
        g_s.width = loc ? 1920 : 2560;
        g_s.height = loc ? 1080 : 1440;
        g_s.interp_fps = loc ? 60 : 120;
        g_s.volume_music = loc ? 70 : 40;
        g_s.game_tick = loc ? 30 : 25;
        g_s.aim_assist = loc ? 0 : 1;
        g_s.auto_gear = loc ? 0 : 1;
        g_s.mouse_sens = loc ? 6 : 10;
    }
    if (strncmp(name, "update_", 7) == 0) { /* W250: Fixtures, nichts gespeichert */
        int dev = strcmp(name, "update_dev") == 0;
        g_upd_ver = dev ? "" : "0.2.0"; /* W301: erste Beta */
        g_upd_cfg = dev ? -1 : strcmp(name, "update_ask") == 0 ? -1 : 1;
        g_upd_ask = strcmp(name, "update_ask") == 0;
        memset(&g_upd_res, 0, sizeof g_upd_res);
        if (strcmp(name, "update_steel") == 0) { /* W397: Beispielzustand, kein Netz, keine Datei */
            g_onl_fix_srv = 1;
            g_onl_fixture = 1;
            g_loc_online = 1;
            strcpy_s(g_onl_sec, sizeof g_onl_sec, "ok");
        }
        if (strcmp(name, "update_unk") == 0 || strcmp(name, "update_lost") == 0) { /* W401: Kopf-Chip */
            g_onl_fix_srv = 1;
            g_onl_fixture = 1;
            g_loc_online = 1;
            g_onl_sec[0] = 0;
            strcpy_s(g_onl_err, sizeof g_onl_err, strcmp(name, "update_lost") == 0 ? "lost" : "");
        }
        if (strcmp(name, "update_offl") == 0) { /* W401: Stufe offline */
            g_upd_cfg = 0;
            g_loc_online = 0;
        }
        if (strcmp(name, "update_snp_on") == 0 || strcmp(name, "update_snp_off") == 0 ||
            strcmp(name, "update_snp_cnt") == 0 || strcmp(name, "update_snp_verold") == 0 ||
            strncmp(name, "update_net", 10) == 0) { /* W403: Pruefergebnis (Attrappe, nie Netz); W470: update_net* */
            lacc_probe_t pr;
            memset(&pr, 0, sizeof pr);
            g_onl_fix_srv = 1;
            g_onl_fixture = 1;
            g_loc_online = 1;
            g_onl_sec[0] = 0;
            g_onl_err[0] = 0;
            pr.ok = strcmp(name, "update_snp_off") != 0;
            pr.ms = pr.ok ? 38 : 5000;
            strcpy_s(pr.version, sizeof pr.version, pr.ok ? "2.4" : "");
            if (strcmp(name, "update_snp_cnt") == 0) { /* Live-Zahlen wie aus "online" von /v1/info */
                pr.has_counts = 1;
                pr.players = 5;
                pr.lobbies = 2;
                pr.games = 1;
            }
            if (strncmp(name, "update_net", 10) == 0) { /* W470: steelnet > Info mit Live-Zahlen + Lobbyliste */
                int nl = name[10] == '6' ? 6 : name[10] == '1' ? 1 : 0;
                pr.has_counts = 1;
                pr.players = 3 + 2 * nl;
                pr.lobbies = nl == 6 ? 5 : nl;
                pr.games = nl == 6 ? 1 : 0;
                pr.games_today = 4 + nl;
                pr.peak_7d = 21;
                strcpy_s(pr.min_ver, sizeof pr.min_ver, "0.4.1");
                g_locver_own_fix = "0.4.1";
                hub_lobby_fixture(strcmp(name, "update_netns") == 0 ? -1 : strcmp(name, "update_netsi") == 0 ? -4 :
                                  strcmp(name, "update_netaf") == 0 ? -5 : nl);
                /* W473: Zeile Konto der Statuskarte - Beispielkonto (nie die Datei): ohne (netsi), ungesichert (netus),
                 * sonst mit Discord gesichert */
                acc_fixture(1, strcmp(name, "update_netsi") != 0, strcmp(name, "update_netus") != 0, 0, 0);
            }
            if (strcmp(name, "update_snp_verold") == 0) { /* W421: Mindestversion ueber der eigenen */
                strcpy_s(pr.min_ver, sizeof pr.min_ver, "0.3.3");
                g_locver_own_fix = "0.3.2";
            }
            snp_apply(&pr);
            g_snp_when.wHour = 12; /* Beleg: fester Zeitpunkt */
            g_snp_when.wMinute = 5;
        } else {
            g_snp_state = -1; /* Shots: nie ein altes Ergebnis */
            g_locver_min[0] = '\0'; /* W421 */
        }
        if (strcmp(name, "update_new") == 0 || strcmp(name, "update_steel") == 0 ||
            strcmp(name, "update_snp_on") == 0 || strcmp(name, "update_snp_cnt") == 0) {
            g_upd_res.result = LUPD_R_NEWER;
            strcpy_s(g_upd_res.rel.tag, sizeof g_upd_res.rel.tag, "v0.2.1");
            strcpy_s(g_upd_res.rel.url, sizeof g_upd_res.rel.url,
                     LUPD_PAGE_PREFIX "tag/v0.2.1");
            g_upd_res.rel.prerelease = 0; /* W301: Betas ab 0.2.0 sind normale Releases */
        }
    }
    for (i = 0; i < (int)(sizeof kStates / sizeof kStates[0]); i++) {
        if (strcmp(name, kStates[i].name) == 0) {
            lcfg_apply_set(&g_s, kStates[i].set);
            if (strcmp(name, "performance") == 0) { lcfg_gfx_apply(&g_s, LGP_PERFORMANCE); }
            if (strcmp(name, "clearview") == 0) { lcfg_gfx_apply(&g_s, LGP_CLEAR); } /* W252 */
            if (kStates[i].diag) {
                g_diag_open = 1;
            }
            g_pk_sel = kStates[i].pksel; /* W172 */
            return;
        }
    }
}

/* W220 PLAY-LAYOUT: Zustaende der Einrichtungskarte (Shots + Selbsttest 62).
 * Pfade sind bereinigte Beispiele; die Pruefung liefert die Fixture. */
#define W220_LOC_XBE "D:/Games/Steel Battalion - Line of Contact/default.xbe" /* W369 */
void w220_fixture(const char *path, int rc)
{
    char m[600];
    g_imgfix_ed = LCFG_ED_NONE;
    if (rc == 0 && strcmp(path, W220_ISO_US) == 0) { /* W231: US experimental */
        _snprintf_s(m, sizeof m, _TRUNCATE, LT("gui.image_ok_us"),
                    "Steel Battalion (USA, NTSC-U)");
        g_imgfix_ed = LCFG_ED_US;
    } else if (rc == 0) {
        _snprintf_s(m, sizeof m, _TRUNCATE, LT("gui.image_ok"),
                    "Steel Battalion (Europe, PAL)");
        g_imgfix_ed = LCFG_ED_EU;
    } else if (rc == 4) { /* W231: Beispiel Japan (US ist jetzt experimental) */
        _snprintf_s(m, sizeof m, _TRUNCATE, LT("cfg.image_version"), "43430002",
                    sb_gamever_region_name(2), 0u);
        g_imgfix_ed = LCFG_ED_OTHER;
    } else if (rc == 5) { /* W369: Line of Contact im SB-Tab */
        strncpy_s(m, sizeof m, LT("cfg.image_is_loc"), _TRUNCATE);
    } else {
        _snprintf_s(m, sizeof m, _TRUNCATE, LT("cfg.image_isdir"), path);
    }
    imgfix_set(path, rc, m);
}

/* Zustand herstellen (Seite Spielen ist offen). Rueckgabe 0 = unbekannt. */
int w220_state(const char *name)
{
    wchar_t w[600];
    const char *path = NULL;
    int rc = 0, check = 1;
    if (strcmp(name, "img_browse") == 0 || strcmp(name, "img_browse_bad") == 0) {
        /* wie der Nutzer: leere Karte, Durchsuchen, danach Regelwerk (on_command) */
        int bad = name[10] == '_';
        w220_fixture(bad ? W220_ISO_JP : W220_ISO, bad ? 4 : 0);
        u8_to_w(bad ? W220_ISO_JP : W220_ISO, w, 600);
        image_chosen(w);
        ui_apply_rules();
        return 1;
    }
    if (strcmp(name, "img_empty") == 0) {
        path = "";
        check = 0;
    } else if (strcmp(name, "img_unchecked") == 0) {
        path = W220_ISO;
        check = 0;
    } else if (strcmp(name, "img_ok") == 0) {
        path = W220_ISO;
    } else if (strcmp(name, "img_version") == 0) {
        path = W220_ISO_JP;
        rc = 4;
    } else if (strcmp(name, "img_folder") == 0) {
        path = W220_DIR;
        rc = 1;
    } else if (strcmp(name, "img_loc") == 0) { /* W369 */
        path = W220_LOC_XBE;
        rc = 5;
    } else {
        return 0;
    }
    if (path[0]) {
        w220_fixture(path, rc);
    }
    ui_settext(ctl(ID_IMAGE), path);
    g_setup_force = 1; /* Karte offen (wie nach "Aendern" an der Spiel-Zeile) */
    if (check) {
        on_command(ID_IMAGE_C, BN_CLICKED);
    } else {
        ui_apply_rules();
    }
    redraw_all();
    return 1;
}

/* W438 CTRL-RESET-040: einmaliger Steuerungs-Reset beim ersten Start mit 0.4.0 (vor dem Laden der Werte, vor
 * der LoC-Uebernahme). Nur mit mitgeliefertem default_full.ini; Shots schreiben nie. Eigene user_*.ini bleiben
 * unveraendert, nur input_profile in user.cfg/user_loc.cfg wird umgestellt. Hinweis nur bei echter Umstellung. */
char g_ctrl040_msg[512];
int g_ctrl040_rc; /* letztes Ergebnis von lgame_ctrl040_reset (Selbsttest) */
void ctrl040_reset(void)
{
    char up[1200], lp[1200], path[1200], psb[96], ploc[96], names[220];
    const char *a, *b;
    g_ctrl040_msg[0] = '\0';
    g_ctrl040_rc = 0;
    if (!loctab_may_write() ||
        lctrl_input_profile_path(g_client_dir, LGAME_CTRL040_PROFILE, path, sizeof path) != 0 ||
        u8_GetFileAttributes(path) == INVALID_FILE_ATTRIBUTES) {
        return;
    }
    user_cfg_path(up, sizeof up);
    game_cfg_path(LGAME_LOC, lp, sizeof lp);
    g_ctrl040_rc = lgame_ctrl040_reset(up, lp, psb, sizeof psb, ploc, sizeof ploc);
    if (g_ctrl040_rc <= 0 || g_ctrl040_rc == 0x100) {
        return;
    }
    a = (g_ctrl040_rc & 1) ? (psb[0] ? psb : LT("gui.legacy_label")) : NULL;
    b = (g_ctrl040_rc & 2) ? (ploc[0] ? ploc : LT("gui.legacy_label")) : NULL;
    if (a && b && _stricmp(a, b) != 0) {
        _snprintf_s(names, sizeof names, _TRUNCATE, "%s / %s", a, b);
    } else {
        strcpy_s(names, sizeof names, a ? a : b);
    }
    _snprintf_s(g_ctrl040_msg, sizeof g_ctrl040_msg, _TRUNCATE, LT("gui.ctrl_reset_040"), names);
}

static void gui_fill(void)
{
    char note[256] = { 0 };
    int kind = 0;
    ctrl040_reset(); /* W438: einmal, vor dem Laden */
    app_load_effective(g_client_dir, &g_s, note, sizeof note, &kind,
                       g_o.lang[0] ? g_o.lang : NULL);
    g_firstrun = kind == 0;
    if (g_firstrun) {
        /* W217 FIRST-RUN: Sprache nach Windows (ausser --lang), Fenster nur,
         * wenn es auf den Monitor passt (sonst randlos). Der Client zeichnet
         * im Fenstermodus DPI-unbewusst -> Arbeitsflaeche in 96-dpi-Einheiten. */
        RECT wa;
        int ww = 0, wh = 0, d = g_dpi >= 96 ? g_dpi : 96;
        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
            ww = (wa.right - wa.left) * 96 / d;
            wh = (wa.bottom - wa.top) * 96 / d;
        }
        if (app_firstrun_defaults(&g_s, g_o.lang[0] ? 0u : (unsigned)GetUserDefaultUILanguage(),
                                  ww, wh) & 1) {
            sb_lang_set(g_s.lang);
            strncpy_s(note, sizeof note, LT("app.no_usercfg"), _TRUNCATE);
        }
        (void)app_firstrun_input_profile(&g_s, g_client_dir, kind); /* W431: neue Installation -> default_full */
    }
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
    upd_init(); /* W250: vor --state (Shots ueberschreiben im Speicher) */
    dg_init(); /* W279: neuer Absturzbericht seit dem letzten Hinweis? */
    loctab_load(); /* W276: gewaehltes Spiel + LoC-Abbild (user.cfg) */
    pergame_init(); /* W402: LoC gewaehlt -> Werte aus user_loc.cfg */
    nrq_load(); /* W496: Antwort auf die Frage "anonyme Netzwerkstatistik" (user.cfg) */
    g_upd_ask = netq_should_ask(g_upd_ver, g_upd_cfg, g_loc_online); /* W397: Online an = steelnet, keine Frage */
    if (g_o.state[0]) { /* W171: --state (nur Shots, nie gespeichert) */
        gui_apply_state(g_o.state);
        if (strcmp(g_o.state, "advanced") == 0 || strcmp(g_o.state, "loc_adv") == 0) { /* W291: Bild mit Feinreglern (nicht gespeichert) */
            g_show_adv = 1; /* build_controls setzt Haken und Kartenlage */
        }
    }
    build_controls();
    tips_build(); /* W172: Begruendungen als Tooltip */
    if (g_o.shot != 0 && strncmp(g_o.state, "update_", 7) == 0) { /* W403: Tooltip-Texte der Kopf-Chips als Beleg */
        char tt[1000];
        int c;
        for (c = 0; c < 2; c++) {
            hdr_tip_text(c, tt, sizeof tt);
            printf("[tip %s] %s\n", c == HDR_CHIP_ST ? "status" : "update", tt);
        }
    }
    packs_load(); /* W171: Packs-Zustand fuer das Regelwerk (R17-R22/R29/R30) */
    update_sbc_status();
    update_vr_button(); /* W159: VR-Knopf je Runtime */
    ui_from_settings(&g_s);
    /* W259: Basis fuer den Abgleich. Gewinnt ein Standard-Profil (W47) bzw.
     * --lang/Erststart, weichen diese Werte von der Datei ab und werden beim
     * Start weiter geschrieben; --state-Shots speichern nie. */
    disk_base_sync(&g_s);
    refresh_profiles();
    ui_set_checked(ID_BG, g_bg_on);
    ui_set_checked(ID_UPD_AUTO, g_upd_cfg == 1); /* W250 */
    update_pad_status();
    update_summary();
    ui_apply_rules();
    if (!g_has_ldr) {
        set_status_k(LT("gui.fill_noldr"), 2);
    } else { /* W173 LAUNCHER-P8: ohne Schluesselzahl (steht unter Diagnose) */
        /* W217: ohne Spieldaten nicht "Bereit." (Willkommen + erster Schritt) */
        set_status(g_note_kind == 2 ? g_note
                   : !g_data_ok ? LT(g_firstrun ? "gui.fill_welcome" : "gui.fill_setup")
                                : LT("gui.fill_ready"));
    }
    loctab_fill_done(); /* W276: Bereich LoC -> Navigation + Statuszeile */
    if (g_ctrl040_msg[0] && g_has_ldr) { /* W438: einmaliger Hinweis nach dem Steuerungs-Reset (Vorrang) */
        set_status_k(g_ctrl040_msg, 1);
    }
    if (g_o.shot != 0 && strncmp(g_o.state, "cx_", 3) == 0) { /* W419: Import-Ergebnis (Temp-Ordner) */
        cx_shot_state(g_o.state);
    }
}

void gui_teardown(void)
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

void pump(void);

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
                break;
            }
            pump();
            Sleep(150);
        }
    }
    { /* W227: DWM liefert nach einem Seitenwechsel gelegentlich noch das
       * vorige Bild -> erneut sichern, bis zwei Aufnahmen gleich sind. */
        canvas_t t;
        int tries;
        memset(&t, 0, sizeof t);
        if (!cv_make(&t, bw, bh)) {
            return 0;
        }
        for (tries = 0; tries < 8; tries++) {
            pump();
            Sleep(80);
            DwmFlush();
            if (!PrintWindow(w, t.dc, 2 /* PW_RENDERFULLCONTENT */)) {
                PrintWindow(w, t.dc, 0);
            }
            GdiFlush();
            if (memcmp(t.px, out->px, (size_t)bw * (size_t)bh * 4) == 0) {
                break;
            }
            memcpy(out->px, t.px, (size_t)bw * (size_t)bh * 4);
        }
        cv_free(&t);
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

/* W309 FPS-MONITOR: Seite mit aufgeklappter Bildratenliste sichern
 * (--state fpslist*). Die Liste der Combo wird unter der Combo gezeigt,
 * ohne Aktivierung und wie das Fenster ausserhalb des Bildschirms, per
 * PrintWindow gesichert und in das Seitenbild kopiert, danach versteckt. */
static int ip_list_shot(const char *path)
{
    canvas_t c, lc;
    COMBOBOXINFO ci;
    RECT wr, cr, vr, lr;
    HWND cb = ctl(ID_IPFPS), lb;
    int n, ih, w, h, rc = 1, x0, y0, yy;
    memset(&c, 0, sizeof c);
    memset(&lc, 0, sizeof lc);
    memset(&ci, 0, sizeof ci);
    ci.cbSize = sizeof ci;
    if (!cb || !GetComboBoxInfo(cb, &ci) || !ci.hwndList) { return 1; }
    lb = ci.hwndList;
    GetWindowRect(g_hwnd, &wr);
    GetWindowRect(cb, &cr);
    n = ComboBox_GetCount(cb);
    ih = (int)SendMessageW(lb, LB_GETITEMHEIGHT, 0, 0);
    w = cr.right - cr.left;
    h = n * ih + 2;
    vr.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    vr.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    vr.right = vr.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    vr.bottom = vr.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    SetRect(&lr, cr.left, cr.bottom, cr.left + w, cr.bottom + h);
    if (n <= 0 || ih <= 0 || IntersectRect(&vr, &vr, &lr)) {
        printf("shot: FEHLER Bildratenliste nicht au" U8_SZ "erhalb des Bildschirms - nichts gesichert\n");
        return 1;
    }
    if (window_pixels(g_hwnd, &c) != 0) { return 1; }
    SetWindowPos(lb, NULL, lr.left, lr.top, w, h, SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
    SendMessageW(lb, LB_SETCURSEL, (WPARAM)ComboBox_GetCurSel(cb), 0);
    RedrawWindow(lb, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
    pump();
    if (window_pixels(lb, &lc) == 0) {
        x0 = lr.left - wr.left;
        y0 = lr.top - wr.top;
        for (yy = 0; yy < lc.h; yy++) {
            int ty = y0 + yy, xa = x0 < 0 ? -x0 : 0, xb = lc.w;
            if (ty < 0 || ty >= c.h) { continue; }
            if (x0 + xb > c.w) { xb = c.w - x0; }
            if (xb > xa) {
                memcpy(&c.px[(size_t)ty * (size_t)c.w + (size_t)(x0 + xa)], &lc.px[(size_t)yy * (size_t)lc.w + (size_t)xa],
                       (size_t)(xb - xa) * 4);
            }
        }
        rc = png_save(c.px, c.w, c.h, path);
    }
    ShowWindow(lb, SW_HIDE);
    cv_free(&lc);
    cv_free(&c);
    return rc;
}

int shot_window(HWND w, const char *path, int png)
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

int bind_shot_save(HWND hd)
{
    char path[1200];
    int rc;
    /* W228: alle Kinder fertig zeichnen (Umordnen invalidiert sie). */
    RedrawWindow(hd, NULL, NULL,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    pump();
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

void pump(void)
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
    g_alpha_tools = ui_cfg_load_tools(g_client_dir); /* W272, W301: test_tools, Rueckfall alpha_tools */
    g_show_adv = ui_cfg_load_key(g_client_dir, "show_advanced", 0); /* W291 */
    g_loctp_mode_cfg = ui_cfg_load_key(g_client_dir, "loctp_mode", -1); /* W351 */
#if defined(SB_HAVE_LOCTP) && SB_HAVE_LOCTP
    loctp_mode_from_cfg();
#endif
    g_lay_work_w = g_o.lay_work_w; /* W281: --work WxH (Test des Einpassens) */
    g_lay_work_h = g_o.lay_work_h;
    if (!gui_create(g_o.shot != 0)) {
        ui_msgbox(NULL, LT("gui.err_nowindow"), LT("gui.title_launcher"),
                  MB_ICONERROR | MB_OK);
        return 1;
    }
    gui_fill();
    show_page(P_PLAY);
    if (g_o.shot == 0) {
        SetTimer(g_hwnd, ONL_TIMER, 2000, NULL); /* W334: Raumcode/Status des LoC-Online-Laufs */
    }
    if (g_o.shot == 4) {
        /* GUI-Startpfad ohne Clientstart (Abnahme: user.cfg-Vergleich). */
        if (g_o.alpha_on) { /* W272: Trockenlauf "Start test" (gleiche Kette wie die Seite) */
            la_sel_t as;
            as.mission = g_o.alpha_mission;
            as.vt = g_o.alpha_vt;
            as.diff = g_o.alpha_diff;
            as.skip_intro = g_o.alpha_skip;
            rc = alpha_start(1, &as);
        } else if (g_o.replay[0]) { /* W254: Trockenlauf "Play replay" (gleiche Kette wie der Dialog) */
            wchar_t wid[40];
            u8_to_w(g_o.replay, wid, 40);
            ls_replay_set_args(wid, g_o.replay_cam, g_rp_args, sizeof g_rp_args);
            if (g_o.video[0]) { /* W255: wie "Export video..." mit Standardordner */
                int vr = 0, vf = 30;
                char va[300];
                if (!ls_video_parse(g_o.video, &vr, &vf)) {
                    printf("gui-start-dry: FEHLER --video '%s' (1080p30, 1440p60, 2160p30 ...)\n", g_o.video);
                    return 1;
                }
                ls_video_set_args("", vr, vf, va, sizeof va);
                strcat_s(g_rp_args, sizeof g_rp_args, ";");
                strcat_s(g_rp_args, sizeof g_rp_args, va);
            }
            rc = do_start_ex(1, 0, g_rp_args);
        } else {
            rc = do_start(1, 0); /* W159: Trockenlauf = Bildschirmweg */
        }
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
    if (g_o.shot == 6) { /* W221: --joy-shot (HOTAS-Dialog, Beispieldaten) */
        ShowWindow(g_hwnd, SW_HIDE);
        joy_dialog_modal();
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
        if (g_lay.id != LLAY_0) { /* W281 */
            printf("shot: layout=%s size=%s geo=%d%% schrift=%d%% flaeche=%dx%d fenster=%dx%d\n",
                   llay_name(g_lay.id), llay_size_name(g_lay.size), g_lay.geo_pct,
                   g_lay.font_pct, g_lay.tw, g_lay.th, SX(lay_tw()), SX(lay_th()));
        }
        for (pass = 0; pass < nbg; pass++) {
            if (nbg == 2) {
                g_bg_on = pass == 0;
                ui_set_checked(ID_BG, g_bg_on);
                base_build();
            }
            for (p = 0; p <= P_HUB_LAST; p++) { /* W397: + Launcher-Einstellungen; W470: + steelnet */
                char path[1200];
                int r;
                if ((g_o.page >= 0 && g_o.page != p) || (p == P_ALPHA && !g_alpha_tools)) {
                    continue;
                }
                /* W276: Packs im Bereich LoC ausgeblendet; W392: dort jetzt die Seite Netzwerk (mit Shot) */
                show_page(p);
                loctab_state_after(g_o.state); /* W276 */
                if (p == P_PLAY && strncmp(g_o.state, "img_", 4) == 0) {
                    w220_state(g_o.state); /* W220: kein zusaetzliches Neuzeichnen */
                }
                if (strcmp(g_o.state, "exit8_us") == 0) {
                    client_exit_report(8, LCFG_ED_US, 0); /* W231: Meldung wie nach Clientende */
                }
                if (strcmp(g_o.state, "ready_hover") == 0 || strcmp(g_o.state, "loc_hover") == 0) {
                    uictl_t *hu = ui_find(g_game == LGAME_LOC ? ID_GAME_SB : ID_GAME_LOC); /* W398: Hover-Bild */
                    if (hu) {
                        hu->hover = 1;
                    }
                }
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
                                "%s/w82_%s%s_%s_%s_%d.png", g_o.out_dir,
                                gui_page_key(p), lay_shot_tag(), g_o.state, g_bg_on ? "bg" : "ohne",
                                g_dpi_mon * 100 / 96);
                } else {
                    _snprintf_s(path, sizeof path, _TRUNCATE,
                                "%s/w82_%s%s_%s_%d.png", g_o.out_dir,
                                gui_page_key(p), lay_shot_tag(), g_bg_on ? "bg" : "ohne",
                                g_dpi_mon * 100 / 96);
                }
                r = shot_window(g_hwnd, path, g_o.shot == 2);
                printf("shot: Seite %d (%s) -> %s%s\n", p, gui_page_key(p),
                       r ? "FEHLER " : "", path);
                rc |= r;
                if (p == P_DISPLAY && g_o.shot == 2 && strncmp(g_o.state, "fpslist", 7) == 0) { /* W309 */
                    size_t pl = strlen(path);
                    if (pl > 4) {
                        _snprintf_s(path + pl - 4, sizeof path - (pl - 4), _TRUNCATE, "_list_%dhz.png", g_disp_hz);
                        r = ip_list_shot(path);
                        printf("shot: Bildratenliste (%d Hz, %d Eintraege) -> %s%s\n", g_disp_hz,
                               ComboBox_GetCount(ctl(ID_IPFPS)), r ? "FEHLER " : "", path);
                        rc |= r;
                    }
                }
                if (p == P_LNET && strcmp(g_o.state, "loc_net_priv") == 0) {
                    privacy_dialog_modal(1); /* W450: Hinweisfenster neben der Seite Netzwerk */
                }
                if (p == P_ADV && strcmp(g_o.state, "replays") == 0) {
                    replay_dialog_modal(1); /* W254: Replay-Dialog mit Beispieldaten */
                }
#if defined(SB_HAVE_LOCTP) && SB_HAVE_LOCTP
                if (p == P_PLAY && strcmp(g_o.state, "loctp_err") == 0) {
                    rc |= loctp_err_shot(g_o.out_dir); /* W344: Testpartner-Dialog im Fehlerfall */
                }
#endif
            }
        }
        gui_teardown();
        return rc;
    }
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);
    SetFocus(ctl(ID_START_BIG));
    nrq_eval(); /* W496: einmaliger Dialog (Fokus auf "Yes", nur im eigenen Fenster) */
    SetTimer(g_hwnd, 1, 100, NULL); /* Gamepad-Live-Anzeige (10 Hz) */
    if (lupd_auto_check(g_upd_ver, g_upd_cfg)) {
        upd_start(0); /* W250: nur nach Zustimmung, still bei Fehlern */
    }
    g_snp_live = 1; /* W403: ab hier echter Fensterbetrieb (Shots/Selbsttest kommen nie hierher) */
    snp_kick();
    SetTimer(g_hwnd, SNP_TIMER, SNP_FAST_MS, NULL); /* Takt; Abstand je Sichtbarkeit in snp_tick */
    /* W535 AVVT: Strg+Tab / Strg+Umschalt+Tab (Seite vor/zurueck) und Strg+Z (W171 Muster C) per
     * Tastenkuerzel-Tabelle (TranslateAccelerator, fenstergebunden) statt eigener GetKeyState-Abfrage. */
    g_kbacc = kbacc_create();
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && g_kbacc &&
            (msg.wParam == VK_TAB || (msg.wParam == 'Z' && g_undo_on)) &&
            TranslateAcceleratorW(g_hwnd, g_kbacc, &msg)) {
            continue; /* -> WM_COMMAND mit HIWORD 1 (acc_command) */
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE && g_nrq_open) {
            nrq_dismiss(); /* W496: Esc = Nein (fuer jetzt) */
            continue;
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN && g_nrq_open) {
            uictl_t *u = ui_by_hwnd(GetFocus());
            on_command(u && u->id == ID_NRQ_NO ? ID_NRQ_NO : ID_NRQ_YES, BN_CLICKED); /* W496: Standard = Ja */
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
    if (g_kbacc) {
        DestroyAcceleratorTable(g_kbacc);
        g_kbacc = NULL;
    }
    g_hwnd = NULL;
    fonts_free();
    return (int)msg.wParam;
}

#include "launcher_alpha.inc" /* W272 ALPHA-TEST: Seite "Alpha Test" */
#include "launcher_diag.inc" /* W279 DIAG-CRASH: Absturz-Karte + Diagnosepaket */

/* ------------------------------------------------------------------ */
