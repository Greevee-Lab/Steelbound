/* launcher_gui_settings.c - Einstellungen <-> Oberflaeche, Regelwerk + Tooltips (W171/W172), Speichern/Starten, Original-Optik-Hinweis (W278), Aufbau der Steuerelemente.
 * W462 LAUNCHER-SPLIT: aus launcher_gui.c verschoben (unveraendert), gemeinsamer Zustand
 * in launcher_gui_int.h. */
#include "launcher_gui_int.h"
/* Einstellungen <-> Oberflaeche                                       */
/* ------------------------------------------------------------------ */
void set_num(int id, unsigned v)
{
    char b[32];
    _snprintf_s(b, sizeof b, _TRUNCATE, "%u", v);
    ui_settext(ctl(id), b);
}

unsigned get_num(int id)
{
    char b[32];
    ui_gettext(ctl(id), b, sizeof b);
    return (unsigned)atoi(b);
}

/* W281 LAUNCHER-SIZE / W287: Groessen von Layout A (Nutzerwahl 15.11.):
 * Kompakt, Normal, Gross; Index = LLAY_SZ_*. Layout 0 und b bleiben nur
 * ueber --layout bzw. cfg (Selbsttests). */
void lay_combo_fill(HWND c)
{
    static const char *const kSz[LLAY_SZ_N] = { "gui.lay_sz_compact", "gui.lay_sz_normal",
                                                "gui.lay_sz_large" };
    int sz;
    if (!c) {
        return;
    }
    g_updating++;
    ComboBox_ResetContent(c);
    for (sz = 0; sz < LLAY_SZ_N; sz++) {
        combo_add(c, LT(kSz[sz]));
    }
    ComboBox_SetCurSel(c, g_lay.size);
    g_updating--;
}

void combo_add(HWND cb, const char *u8)
{
    wchar_t *w = u8_wdup(u8);
    SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)(w ? w : L""));
    free(w);
}

/* W445 PHOTO-CINE-SELF: Liste Aus, 1.0 m .. 12.0 m (Zehntel wie [photo] photo_cine_self, Raster LCFG_CSELF_*) */
void cself_fill(HWND cb)
{
    int k;
    combo_add(cb, LT("gui.off"));
    for (k = LCFG_CSELF_LO; k <= LCFG_CSELF_HI; k += LCFG_CSELF_STEP) {
        char t[16];
        _snprintf_s(t, sizeof t, _TRUNCATE, "%d.%d m", k / 10, k % 10);
        combo_add(cb, t);
    }
}

/* W309 FPS-MONITOR: Monitore fuer die Bildratenliste. "a/b,c" = Simulation
 * (--sim-monitors, Selbsttest/Shots): erster Eintrag = Hauptmonitor = Start-
 * monitor mit aktueller Rate a und Hoechstrate b, weitere = andere Monitore.
 * 0 = gueltig. */
int mon_sim_apply(const char *spec)
{
    int k = 0, hz = 0, mx = 0, alt = 0;
    const char *p = spec;
    if (!p || !*p) { return 1; }
    while (*p) {
        int a = atoi(p), b;
        if (a < 0 || a > 1000) { return 1; }
        while (*p >= '0' && *p <= '9') { p++; }
        b = a;
        if (*p == '/') {
            p++;
            b = atoi(p);
            while (*p >= '0' && *p <= '9') { p++; }
        }
        if (k == 0) { hz = a; mx = b > a ? b : a; }
        else if (alt == 0 && a > 0 && sb_fps_nominal_hz(a) != sb_fps_nominal_hz(hz)) { alt = a; }
        k++;
        if (*p == ',') { p++; } else if (*p) { return 1; }
    }
    g_disp_hz = hz;
    g_disp_hz_max = mx;
    g_mon_alt_hz = alt;
    return 0;
}

static BOOL CALLBACK mon_alt_cb(HMONITOR hm, HDC dc, LPRECT rc, LPARAM lp)
{
    MONITORINFO mi;
    int hz;
    (void)dc;
    (void)rc;
    (void)lp;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(hm, &mi) || (mi.dwFlags & MONITORINFOF_PRIMARY)) { return TRUE; }
    hz = sb_fps_monitor_hz(hm);
    if (hz > 0 && g_mon_alt_hz == 0 && sb_fps_nominal_hz(hz) != sb_fps_nominal_hz(g_disp_hz)) {
        g_mon_alt_hz = hz;
    }
    return TRUE;
}

/* W309: Hoechstrate des Hauptmonitors und abweichende Rate eines weiteren
 * Monitors (Hinweis; die Liste richtet sich nach dem Startmonitor). */
void mon_detect_rates(void)
{
    POINT o;
    o.x = 0;
    o.y = 0;
    g_disp_hz_max = sb_fps_monitor_max_hz(MonitorFromPoint(o, MONITOR_DEFAULTTOPRIMARY));
    g_mon_alt_hz = 0;
    EnumDisplayMonitors(NULL, NULL, mon_alt_cb, 0);
    if (g_o.sim_mon[0] && mon_sim_apply(g_o.sim_mon) != 0) {
        g_disp_hz_max = g_disp_hz;
    }
}

/* W309 FPS-MONITOR: angebotene Bildraten (sb_fps_list nach der Rate des
 * Startmonitors). Der Client oeffnet sein Fenster mit CW_USEDEFAULT auf dem
 * Hauptmonitor (fs_monitor verwaltet der Launcher nicht), auto folgt danach
 * live dem Fenster-Monitor (W223). g_ip_keep = Wert aus cfg/Preset, der
 * angeboten bleibt, auch wenn er ueber der Monitorrate liegt. */
static SbFpsItem g_ip[SB_FPS_LIST_MAX];
static int g_ip_n;
int g_ip_keep = SB_FPS_AUTO;

/* W223: Listenposition einer Bildrate; W309: in der angebotenen Liste.
 * Fehlt sie, wie bisher die Position von 30 (Original). */
int ip_sel_of(int v)
{
    int i = sb_fps_list_idx(g_ip, g_ip_n, v);
    return i >= 0 ? i : sb_fps_list_idx(g_ip, g_ip_n, 0);
}

/* W309: Wert eines Listeneintrags (sel < 0 bzw. leer: dflt). */
int ip_val(int sel, int dflt)
{
    return (sel >= 0 && sel < g_ip_n) ? g_ip[sel].v : dflt;
}

/* W223: Bildraten-Liste fuellen: Monitor (auto, n Hz), 30 (Original), feste
 * Werte. W309: nur bis zur Monitorrate; kein glatter Teiler = "(kann
 * ruckeln)", alter Wert ueber dem Monitor = "(ueber Monitorrate)". Die
 * aktuelle Wahl bleibt erhalten (nie still geaendert). */
void ip_fill(HWND c)
{
    int i, cur = ComboBox_GetCount(c) > 0 ? ip_val(ComboBox_GetCurSel(c), g_ip_keep) : g_ip_keep;
    char b[96];
    g_ip_n = sb_fps_list(g_disp_hz, cur, g_ip, SB_FPS_LIST_MAX);
    ComboBox_ResetContent(c);
    for (i = 0; i < g_ip_n; i++) {
        int v = g_ip[i].v, f = g_ip[i].flags;
        if (v == SB_FPS_AUTO) {
            if (g_disp_hz > 0) { _snprintf_s(b, sizeof b, _TRUNCATE, LT("gui.ip_auto_hz"), g_disp_hz); }
            else { strncpy_s(b, sizeof b, LT("gui.ip_auto"), _TRUNCATE); }
        } else if (v == 0) { /* W234: bei PAL50 ist das Original 25; W323: LoC-Mission taktet mit 20 Hz */
            strncpy_s(b, sizeof b, LT(g_game == LGAME_LOC ? "gui.ip_20" :
                                     ctl(ID_PAL50) && ui_checked(ID_PAL50) ? "gui.ip_25" : "gui.ip_30"), _TRUNCATE);
        }
        else if (f & SB_FPS_F_ABOVE) { _snprintf_s(b, sizeof b, _TRUNCATE, LT("gui.ip_above"), v); } /* W309 */
        else if (f & SB_FPS_F_JUDDER) { _snprintf_s(b, sizeof b, _TRUNCATE, LT("gui.ip_judder"), v); } /* W309 */
        else if (v == 60) { strncpy_s(b, sizeof b, LT("gui.ip_60"), _TRUNCATE); }
        else if (v == 120) { strncpy_s(b, sizeof b, LT("gui.ip_120"), _TRUNCATE); }
        else { _snprintf_s(b, sizeof b, _TRUNCATE, "%d", v); }
        combo_add(c, b);
    }
    i = sb_fps_list_idx(g_ip, g_ip_n, cur);
    if (i >= 0) { ComboBox_SetCurSel(c, i); }
}

/* W309: Bildrate waehlen (Laden, Preset, Test). Ein Wert, der nicht (mehr)
 * angeboten wird, kommt als eigener Eintrag dazu statt still auf 30. */
void ip_set(int v)
{
    HWND c = ctl(ID_IPFPS);
    if (!c) { return; }
    if (sb_fps_choice_idx(v) >= 0) {
        g_ip_keep = v;
        if (sb_fps_list_idx(g_ip, g_ip_n, v) < 0 || ComboBox_GetCount(c) != g_ip_n) {
            ComboBox_SetCurSel(c, -1);
            ip_fill(c);
        }
    }
    ComboBox_SetCurSel(c, ip_sel_of(v));
}

/* W309: Statuszeile nach einer Bildraten-Wahl (warum ein Wert ruckeln kann;
 * Hinweis auf einen zweiten Monitor mit anderer Rate). */
void ip_status(void)
{
    int sel = ComboBox_GetCurSel(ctl(ID_IPFPS)), v = ip_val(sel, SB_FPS_AUTO);
    int f = (sel >= 0 && sel < g_ip_n) ? g_ip[sel].flags : 0;
    char t[400];
    size_t n;
    t[0] = 0;
    if (f & SB_FPS_F_ABOVE) {
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.ip_st_above"), v, g_disp_hz);
    } else if (f & SB_FPS_F_JUDDER) {
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.ip_st_judder"), v, g_disp_hz);
    } else if (g_disp_hz > 0 && g_disp_hz_max > g_disp_hz + 1) {
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.ip_st_max"), g_disp_hz, g_disp_hz_max);
    }
    if (g_mon_alt_hz > 0 && g_disp_hz > 0) {
        n = strlen(t);
        if (n) { strcat_s(t, sizeof t, " "); n++; }
        _snprintf_s(t + n, sizeof t - n, _TRUNCATE, LT("gui.ip_st_multi"), g_disp_hz, g_mon_alt_hz);
    }
    if (t[0]) { set_status(t); }
}

void sync_preset(void)
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

/* W191 UW-PERSP: Stufen der Cockpit-Perspektive (Index = Kombi-Eintrag):
 * Original, 10..100 % in 10er-Schritten ([render] cockpit_persp, gleicher
 * Wert wie im ESC-Menue). Fremde Werte -> naechste Stufe. */
static const int kCpersp[] = { 0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100 };
#define KCPERSP_N ((int)(sizeof kCpersp / sizeof kCpersp[0]))
static int cpersp_index(int v)
{
    int i, best = 0, bd = 1000;
    for (i = 0; i < KCPERSP_N; i++) {
        int d = v > kCpersp[i] ? v - kCpersp[i] : kCpersp[i] - v;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

void cpersp_fill(HWND c)
{
    int i, sel = ComboBox_GetCurSel(c);
    ComboBox_ResetContent(c);
    combo_add(c, LT("gui.cp_off"));
    for (i = 1; i < KCPERSP_N; i++) {
        char t[16];
        _snprintf_s(t, sizeof t, _TRUNCATE, "%d %%", kCpersp[i]);
        combo_add(c, t);
    }
    ComboBox_SetCurSel(c, (sel >= 0 && sel < KCPERSP_N) ? sel : 0);
}

/* W235 COCKPIT-WIDTH: Original, +1..+10 % in 1er-Schritten ([render] cockpit_width,
 * gleicher Wert wie im ESC-Menue). Index = Wert. */
#define KCWIDTH_N 11
void cwidth_fill(HWND c)
{
    int i, sel = ComboBox_GetCurSel(c);
    ComboBox_ResetContent(c);
    combo_add(c, LT("gui.cp_off"));
    for (i = 1; i < KCWIDTH_N; i++) {
        char t[16];
        _snprintf_s(t, sizeof t, _TRUNCATE, "+%d %%", i);
        combo_add(c, t);
    }
    ComboBox_SetCurSel(c, (sel >= 0 && sel < KCWIDTH_N) ? sel : 0);
}

/* W266 PRESET-EDGE: nur die drei Rand-Auswahlen (auch beim Waehlen eines Grafik-Presets). */
void edge_ui_set(int mode, int width, int strength)
{
    g_updating++;
    ComboBox_SetCurSel(ctl(ID_EDGE), (mode >= 0 && mode <= 2) ? mode : 0);
    edge_pct_set(ctl(ID_EDGE_W), kEdgeW, KEDGEW_N, width, LT("gui.edge_w_fmt"));
    edge_pct_set(ctl(ID_EDGE_S), kEdgeS, KEDGES_N, strength, LT("gui.edge_s_fmt"));
    g_updating--;
    if (g_page == P_DISPLAY && ui_refresh_visibility()) { /* W296: Verlaufszeile folgt "Fade out" (Preset, Rueckgaengig) */
        redraw_rect(COL2, CY0, COLW, 488);
    }
}

void edge_from_settings(const launcher_settings_t *s)
{
    edge_ui_set(s->edge_mode, s->edge_fade_width, s->edge_fade_strength);
    g_updating++;
    ComboBox_SetCurSel(ctl(ID_CPERSP), cpersp_index(s->cockpit_persp)); /* W191 */
    ComboBox_SetCurSel(ctl(ID_CPANINI), cpersp_index(s->cockpit_panini));
    ComboBox_SetCurSel(ctl(ID_CWIDTH), (s->cockpit_width >= 0 && s->cockpit_width < KCWIDTH_N) ? s->cockpit_width
                                       : (s->cockpit_width < 0 ? 0 : KCWIDTH_N - 1)); /* W235 */
    g_updating--; /* W171: Freigabe Verlauf = Regel R11/R12 (ui_apply_rules) */
}

void edge_to_settings(launcher_settings_t *s)
{
    int sel = ComboBox_GetCurSel(ctl(ID_EDGE));
    s->edge_mode = (sel >= 0 && sel <= 2) ? sel : 0;
    s->edge_fade_width = edge_pct_get(ctl(ID_EDGE_W), kEdgeW, KEDGEW_N, 12);
    s->edge_fade_strength = edge_pct_get(ctl(ID_EDGE_S), kEdgeS, KEDGES_N, 100);
    { /* W191 UW-PERSP */
        int cs = ComboBox_GetCurSel(ctl(ID_CPERSP));
        s->cockpit_persp = (cs >= 0 && cs < KCPERSP_N) ? kCpersp[cs] : 0;
        cs = ComboBox_GetCurSel(ctl(ID_CPANINI));
        s->cockpit_panini = (cs >= 0 && cs < KCPERSP_N) ? kCpersp[cs] : 0;
        cs = ComboBox_GetCurSel(ctl(ID_CWIDTH)); /* W235 */
        s->cockpit_width = (cs >= 0 && cs < KCWIDTH_N) ? cs : 0;
    }
}

void update_aspect_info(void)
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
        strncpy_s(g_aspect_info, sizeof g_aspect_info, LT("gui.aspect_bad"), _TRUNCATE);
    } else if (bw == w && bh == h) {
        _snprintf_s(g_aspect_info, sizeof g_aspect_info, _TRUNCATE, LT(pergame_box_169() ? "gui.aspect_169" : "gui.aspect_43"),
                    w, h);
    } else if (bh == h) {
        _snprintf_s(g_aspect_info, sizeof g_aspect_info, _TRUNCATE,
                    LT(pergame_box_169() ? "gui.aspect_wide_169" : "gui.aspect_wide"), /* W402: LoC 16:9 */
                    w, h, bw, bh, (w - bw) / 2);
    } else {
        _snprintf_s(g_aspect_info, sizeof g_aspect_info, _TRUNCATE,
                    LT(pergame_box_169() ? "gui.aspect_tall_169" : "gui.aspect_tall"),
                    w, h, bw, bh, (h - bh) / 2);
    }
    { /* W107 EDGE-MODE: Vorschau + Hinweis */
        launcher_settings_t t;
        size_t n = strlen(g_aspect_info);
        edge_to_settings(&t);
        g_prev_edge = t.edge_mode;
        g_prev_ew = t.edge_fade_width;
        g_prev_es = t.edge_fade_strength;
        if (bw && (bw != w || bh != h) && t.edge_mode && !pergame_box_169()) {
            _snprintf_s(g_aspect_info + n, sizeof g_aspect_info - n, _TRUNCATE, " %s",
                        t.edge_mode == 1 ? LT("gui.aspect_cut")
                                         : LT("gui.aspect_fade"));
        }
    }
}

void refresh_ctrl_profiles(const char *keep)
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

void ctrl_profile_from_ui(char *out, size_t cap)
{
    HWND cb = ctl(ID_CTRL_PROF);
    int sel = ComboBox_GetCurSel(cb);
    out[0] = '\0';
    if (sel <= 0) {
        return;
    }
    {
        wchar_t w[128];
        if (SendMessageW(cb, CB_GETLBTEXT, (WPARAM)sel, (LPARAM)w) != CB_ERR) {
            w_to_u8(w, out, (int)cap);
        }
    }
    if (strcmp(out, LT("gui.legacy_label")) == 0) {
        out[0] = '\0';
    }
}

void update_ctrl_overview(void)
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
        o = 0;
        for (i = 0; i < n && o + 3 < sizeof shown; i++) {
            if (txt[i] == '\n' && (i == 0 || txt[i - 1] != '\r')) {
                shown[o++] = '\r';
            }
            shown[o++] = txt[i];
        }
        shown[o] = '\0';
        {
            /* Profil-Dateien sind ASCII/ANSI: ANSI -> UTF-8. W419: nur der Dateiteil (der Kopftext ist schon
             * UTF-8; vorher doppelt umgewandelt, z. B. "1â€“5" statt "1–5"). */
            static char u8[sizeof shown * 2], all[sizeof shown * 2 + 1024];
            u8_from_acp(shown, u8, sizeof u8);
            _snprintf_s(all, sizeof all, _TRUNCATE, "%s%s", LT("gui.ctl_always"), u8);
            ui_settext(e, all);
        }
    }
}

/* W179 KEYS-HTML: Profil-Datei lesen (gemeinsame Quelle fuer die
 * Tastenuebersicht und den Browser-Export). Rueckgabe 1 = ok (n Bytes
 * in buf, NUL-terminiert), 0 = kein einfacher Name, -1 = fehlt/leer. */
int ctrl_profile_load(const char *prof, char *buf, size_t cap,
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
/* W171 LAUNCHER-P2P3: Regelwerk anwenden (launcher_rules.c)            */
/* ------------------------------------------------------------------ */
/* Steuerelemente je Regel (Muster A/D/E sperren sie). Ein Element, das
 * mehrere Regeln trifft, ist nur frei, wenn keine davon sperrt.
 * W172: Packs-Seite neu (Segment, Kategorie-Grenzen, Pack-Grenze R31). */

void packs_env(lr_env_t *e); /* unten (Packs-Engine) */

/* Pfadvergleich ohne Gross/Klein und / gegen \ */
int path_eq(const char *a, const char *b)
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
void xbe_derived(const launcher_settings_t *s, char *out, size_t cap)
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
    _snprintf_s(k, sizeof k, _TRUNCATE, "%s|%s|%s|%s|%s|%d,%d", s->game_image,
                s->xbe_path, s->media_root, s->hdd_root, sb_lang_code(),
                g_imgfix_path[0] && strcmp(s->game_image, g_imgfix_path) == 0 ? g_imgfix_rc : -1,
                g_imgfix_ed); /* W231: Fixture-Wechsel bei gleichem Pfad neu auswerten */
    if (ok < 0 || strcmp(k, key) != 0) {
        launcher_settings_t t;
        lcfg_defaults(&t);
        strcpy_s(t.game_image, sizeof t.game_image, s->game_image);
        strcpy_s(t.xbe_path, sizeof t.xbe_path, s->xbe_path);
        strcpy_s(t.media_root, sizeof t.media_root, s->media_root);
        strcpy_s(t.hdd_root, sizeof t.hdd_root, s->hdd_root);
        err[0] = '\0';
        if (g_imgfix_path[0] && strcmp(s->game_image, g_imgfix_path) == 0) {
            ok = g_imgfix_rc == 0; /* W220: Fixture (Shots) */
            if (!ok) {
                strcpy_s(err, sizeof err, g_imgfix_msg);
            }
            g_edition = g_imgfix_ed;
        } else {
            lcfg_gameinfo_t gi;
            ok = lcfg_validate(&t, err, sizeof err) == 0;
            /* W231: Fassung (EU/US) fuer Saves-Ordner, Bereitschaft, Start */
            g_edition = ok && lcfg_game_info(&t, &gi) ? gi.edition : LCFG_ED_NONE;
        }
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
#if !SB_EDITION_COMMUNITY
int creator_path(char *out, size_t cap)
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

#endif
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
#if !SB_EDITION_COMMUNITY
    if (g_creator < 0) {
        char cp[1200];
        g_creator = creator_path(cp, sizeof cp);
    }
    e->has_creator = g_creator == 1;
#endif
    e->photo_dir_ok = extras_dir_ok(s->photo_dir); /* W195 R33 */
    packs_env(e);
    g_tex_src = e->tex_packs + e->legacy_tex; /* W173: Texturen-Zeile */
    g_data_ok = e->data_ok;
    strcpy_s(g_data_err, sizeof g_data_err, e->data_err);
}

/* W172/B5: Remaster-Bedienung sichtbar mit lokalem Remaster-Ordner (W116)
 * oder mindestens einem installierten Pack. */
int rem_avail(void)
{
    return g_rem_pkg == 1 || packs_count() > 0;
}

/* W172/P6: Einrichtungskarte auf Spielen (statt Bereitschaft). */
int play_setup_shown(void)
{
    return g_setup_force || g_rr[3].active;
}

/* W172: Sichtbarkeit der aktuellen Seite nachziehen (Zustand kann sich
 * durch Regeln/Packs aendern: Einrichtungskarte, leere Packs-Liste). */
int ui_refresh_visibility(void)
{
    int i, n = 0;
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
            n++;
        }
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* W172: Tooltips = Begruendung gesperrter Elemente (Muster A, Konzept  */
/* 5.1 "Tooltip gleichlautend"). Werkzeuge am Hauptfenster mit dem      */
/* Rechteck des Elements: gesperrte Kinder bekommen keine Maus, das     */
/* Hauptfenster schon (TTF_SUBCLASS).                                   */
/* ------------------------------------------------------------------ */
/* Text der sperrenden Regel eines Elements ("" = keine Sperre/sichtbar frei). */
/* W402 LAUNCHER-PERGAME: Optionen, die es in Line of Contact nicht gibt (sichtbar, gesperrt, Grund im
 * Tooltip). Grundlage: Client-Code (LoC-Profil g_sba_loc: SB-Hooks nicht installiert, SB-Adressen lesen die
 * Null-Seite), Tabelle in BERICHT_M2 "W402". Einzige Stelle der Liste. W440: Zielhilfe und Gang-Automatik sind
 * frei (Hinweis "nur Arcade-Lobbys", gui.loc_arcade_tip/gui.loc_arcade_only). */
static const int kLocLock[] = { ID_PAL50, ID_CPERSP, ID_CPANINI, ID_CWIDTH, ID_CHIDE, ID_MONFX,
                                ID_CTRL_ALLY, ID_RP_LABEL, ID_RP_LOGO, ID_WM_MEDIA,
                                ID_GFX_TEX, ID_REM_SCALE, ID_GFX_PACKS, ID_VR, ID_VRPRESET, ID_VRRES,
                                ID_EDGE, ID_EDGE_S, ID_EDGE_W, ID_SAVE_LOCK, ID_REM_LABEL /* W427 */ };
#define KLOCLOCK_N (sizeof kLocLock / sizeof kLocLock[0])
const char *loc_lock_key(int id) /* NULL = nicht gesperrt */
{
    if (g_game != LGAME_LOC) {
        return NULL;
    }
    switch (id) {
    case ID_PAL50: return "gui.loc_lock_tick";
    case ID_CPERSP: case ID_CPANINI: case ID_CWIDTH: case ID_CHIDE: case ID_REM_LABEL: /* W427: hud_label_avoid wie ESC */
        return "gui.loc_lock_cockpit";
    case ID_MONFX: return "gui.loc_lock_monfx";
    case ID_CTRL_ALLY: return "gui.loc_lock_ally"; /* W424 */
    case ID_RP_LABEL: case ID_RP_LOGO: case ID_WM_MEDIA: return "gui.loc_lock_replay";
    case ID_GFX_TEX: case ID_REM_SCALE: case ID_GFX_PACKS: return "gui.loc_lock_packs";
    case ID_VR: case ID_VRPRESET: case ID_VRRES: return "gui.loc_lock_vr";
    case ID_SAVE_LOCK: return "gui.loc_lock_savelock";
    case ID_EDGE: case ID_EDGE_S: case ID_EDGE_W:
        return pergame_box_169() ? "gui.loc_lock_edge" : NULL; /* 4:3: Hor+-Raender wirken auch in LoC */
    default: return NULL;
    }
}

void tip_text(int id, char *out, size_t cap)
{
    int i, k;
    HWND h = ctl(id);
    if (id == ID_CTRL_BROWSER) { /* W179: immer aktiv, fester Hinweistext */
        if (h && (GetWindowLongW(h, GWL_STYLE) & WS_VISIBLE)) {
            strncpy_s(out, cap, LT("gui.keys_browser_tip"), _TRUNCATE);
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
    if (loc_lock_key(id)) { /* W402 */
        strncpy_s(out, cap, LT(loc_lock_key(id)), _TRUNCATE);
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

void tips_place(void)
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
            ui_px_rect(u, &ti.rect); /* W281 */
            SendMessageW(g_tip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
        }
    }
    for (i = 0; i < (int)KLOCLOCK_N; i++) { /* W402: gesperrte LoC-Optionen */
        uictl_t *u = ui_find(kLocLock[i]);
        TTTOOLINFOW ti;
        if (!u) {
            continue;
        }
        memset(&ti, 0, sizeof ti);
        ti.cbSize = sizeof ti;
        ti.hwnd = g_hwnd;
        ti.uId = (UINT_PTR)u->id;
        ui_px_rect(u, &ti.rect);
        SendMessageW(g_tip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
    }
    { /* W179: Rechteck des Browser-Knopfes nachfuehren */
        uictl_t *u = ui_find(ID_CTRL_BROWSER);
        if (u) {
            TTTOOLINFOW ti;
            memset(&ti, 0, sizeof ti);
            ti.cbSize = sizeof ti;
            ti.hwnd = g_hwnd;
            ti.uId = (UINT_PTR)u->id;
            ui_px_rect(u, &ti.rect); /* W281 */
            SendMessageW(g_tip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
        }
    }
    SendMessageW(g_tip, TTM_SETMAXTIPWIDTH, 0, SX(340));
}

void tips_build(void)
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
    for (i = 0; i < (int)KLOCLOCK_N; i++) { /* W402: Grund der LoC-Sperre */
        TTTOOLINFOW ti;
        memset(&ti, 0, sizeof ti);
        ti.cbSize = sizeof ti;
        ti.uFlags = TTF_SUBCLASS;
        ti.hwnd = g_hwnd;
        ti.uId = (UINT_PTR)kLocLock[i];
        ti.lpszText = LPSTR_TEXTCALLBACKW;
        SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti); /* doppelt = egal */
    }
    { /* W287 EXP-ICON: Symbole, Preset-Optionsfelder und Navigation tragen
       * ihren Tooltip selbst (aktiv, Maus am Element; leer = kein Tooltip) */
        int j;
        for (j = 0; j < g_ui_n; j++) {
            uictl_t *u = &g_ui[j];
            TTTOOLINFOW ti;
            if (u->parent != g_hwnd ||
                !(u->kind == K_ICON || (u->id >= ID_GP0 && u->id < ID_GP0 + LGP_CUSTOM) ||
                  u->id == ID_CHIDE || u->id == ID_REM_LABEL || /* W291: Erklaerung als Tooltip statt Textzeile */
                  loc_tip_id(u->id) || /* W387: Gegner glaetten erklaert */
                  u->id == ID_NAV0 + P_DISPLAY || u->id == ID_NAV0 + P_GFX || u->id == ID_NAV0 + P_PACKS)) {
                continue;
            }
            memset(&ti, 0, sizeof ti);
            ti.cbSize = sizeof ti;
            ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            ti.hwnd = g_hwnd;
            ti.uId = (UINT_PTR)u->h;
            ti.lpszText = LPSTR_TEXTCALLBACKW;
            SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        }
    }
    { /* W403: Kopf-Chips (gezeichnet, keine Kinder): Rechteck setzt hdr_paint_status */
        int c;
        for (c = 0; c < 2; c++) {
            TTTOOLINFOW ti;
            memset(&ti, 0, sizeof ti);
            ti.cbSize = sizeof ti;
            ti.uFlags = TTF_SUBCLASS;
            ti.hwnd = g_hwnd;
            ti.uId = (UINT_PTR)(HDR_TIP_ID0 + c);
            ti.lpszText = LPSTR_TEXTCALLBACKW;
            SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        }
    }
    tips_place();
}

/* TTN_GETDISPINFOW: Text erst beim Zeigen (Sprache/Regelstand aktuell). */
void tips_dispinfo(NMTTDISPINFOW *di)
{
    static wchar_t wbuf[800];
    char t[800];
    if (di->uFlags & TTF_IDISHWND) { /* W287: Werkzeug am Element */
        uictl_t *u = ui_by_hwnd((HWND)di->hdr.idFrom);
        xtip_text(u ? u->id : 0, t, sizeof t);
    } else if (di->hdr.idFrom >= HDR_TIP_ID0 && di->hdr.idFrom < HDR_TIP_ID0 + 2) { /* W403: Kopf-Chips */
        hdr_tip_text((int)(di->hdr.idFrom - HDR_TIP_ID0), t, sizeof t);
    } else {
        tip_text((int)di->hdr.idFrom, t, sizeof t);
    }
    u8_to_w(t, wbuf, 800);
    di->lpszText = wbuf;
}

/* Alle Regeln auswerten, Freigaben setzen; geaenderte Begruendungen ->
 * neu zeichnen. Einzige Stelle fuer EnableWindow der Hauptseiten. */
void ui_apply_rules(void)
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
    { /* W278 ORIG-FLAG: experimentelle Grafik (Hinweis auf Spielen, beide Bereiche) */
        unsigned om = lorig_mask(&s, e.fx_pack[0] != 0);
        if (e.tex_packs + e.legacy_tex <= 0) { /* HD an, aber kein Texturpaket: nichts sichtbar anders */
            om &= ~(1u << SB_OFG_HD);
        }
        if (g_game == LGAME_LOC && loctab_wide_val() != LGAME_WIDE_43) {
            /* W414 (Nutzer 21.11.): 16:9 in LoC ist nicht mehr experimentell (W326 setzte hier Breitbild). Die
             * Gruppe Breitbild entfaellt im LoC-Bereich ganz: die Raender folgen der 16:9-Box (Fade out bzw.
             * schwarz, Client render_edge.c), die Cockpit-Optionen der Gruppe gibt es in LoC nicht (W402).
             * 720p (nicht angeboten) bliebe experimentell. 4:3 mit Hor+-Raendern: unveraendert experimentell. */
            om &= ~(1u << SB_OFG_WIDE);
            if (lgame_wide_experimental(loctab_wide_val())) {
                om |= 1u << SB_OFG_WIDE;
            }
        }
        if (g_game == LGAME_LOC && g_loc.loc_sblook) {
            om |= 1u << SB_OFG_LIGHT; /* W459: SB look = Bloom/Glow-Pass im Cockpit (Licht und Shader), nicht original */
        }
        if (om != g_orig_mask) {
            int p;
            g_orig_mask = om;
            xicon_texts(); /* W287: Warn-Tooltips/Namen, Navigation */
            for (p = 0; p < P_N; p++) {
                if (ctl(ID_NAV0 + p)) {
                    InvalidateRect(ctl(ID_NAV0 + p), NULL, FALSE);
                }
            }
            if (g_page == P_PLAY) { redraw_all(); }
        }
    }
    if (g_page == P_PLAY) { /* W220: Bereitschaft folgt dem aktuellen Stand */
        update_summary();
    }
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
            if (g_game == LGAME_LOC && (id == ID_START || id == ID_START_MENU)) {
                /* W276: keine SB-Sperre im Bereich LoC ("Coming soon" ohne startbaren Client);
                 * sonst grau wie Play Line of Contact (ohne Disc bzw. ohne steelnet) */
                en = loc_launch_ok();
            }
            if (loc_lock_key(id)) {
                en = 0; /* W402: gibt es in LoC nicht */
            }
            if (ctl(id) && (IsWindowEnabled(ctl(id)) != FALSE) != en) {
                EnableWindow(ctl(id), en ? TRUE : FALSE);
            }
        }
    }
    for (i = 0; i < (int)KLOCLOCK_N; i++) { /* W402: Sperr-IDs ohne Regel (mit Regel: oben erledigt) */
        int id = kLocLock[i], j, m, ruled = 0, en;
        for (j = 0; j < KRULECTL_N && !ruled; j++) {
            for (m = 0; m < KRC_MAX && kRuleCtl[j].ids[m]; m++) {
                if (kRuleCtl[j].ids[m] == id) {
                    ruled = 1;
                    break;
                }
            }
        }
        en = loc_lock_key(id) == NULL;
        if (!ruled && ctl(id) && (IsWindowEnabled(ctl(id)) != FALSE) != en) {
            EnableWindow(ctl(id), en ? TRUE : FALSE);
            InvalidateRect(ctl(id), NULL, FALSE);
        }
    }
    { /* W445 PHOTO-CINE-SELF: Reichweite eigene VT-Teile nur bei Ansicht Cinematic (SB; LoC: Cinematic = Aussenwelt) */
        HWND cs = ctl(ID_PH_CSELF);
        int en = g_game != LGAME_LOC && ctl(ID_PH_VIEW) && ComboBox_GetCurSel(ctl(ID_PH_VIEW)) == 3;
        if (cs && (IsWindowEnabled(cs) != FALSE) != en) {
            EnableWindow(cs, en ? TRUE : FALSE);
            InvalidateRect(cs, NULL, FALSE);
        }
    }
    /* W220 PLAY-LAYOUT: Sichtbarkeit geaendert (z. B. Einrichtungskarte ->
     * Bereitschaft nach "Durchsuchen") -> Hintergrund neu aufbauen. Sonst
     * blieb das alte Bild (leeres Pfadfeld, Texte der Karte) unter den neu
     * gezeigten "Aendern"-Knoepfen stehen. */
    if (ui_refresh_visibility() != 0 || memcmp(old, g_rr, sizeof old) != 0 ||
        (g_page == P_PLAY && g_bb_setup != play_setup_shown())) {
        redraw_all();
    }
}

/* Rueckfrage (Muster C, R14). Test-/Shot-Betrieb: nie ein Fenster. */
int ui_confirm(const char *text, const char *title)
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
char g_ucfg_test[1200]; /* W259: Selbsttest-Ziel statt der echten user.cfg */
void user_cfg_path(char *out, size_t cap)
{
    if (g_ucfg_test[0]) {
        strcpy_s(out, cap, g_ucfg_test);
        return;
    }
    lcfg_user_cfg_path(g_client_dir, out, cap); /* W150 USER-DATA */
}

/* W402 PERGAME: Einstellungsdatei eines Spiels (SB = user.cfg, LoC = user_loc.cfg daneben). */
void game_cfg_path(int game, char *out, size_t cap)
{
    char base[1200];
    user_cfg_path(base, sizeof base);
    lgame_cfg_path(base, game, out, cap);
}

/* W259 SETTINGS-RESET: Stand der user.cfg beim letzten Lesen/Schreiben durch
 * den Launcher (Oberflaechenwerte, ueberlagert mit der Datei). Der Launcher
 * bleibt waehrend des Spiels offen; ohne Abgleich schrieb der naechste Start
 * die alten Oberflaechenwerte zurueck und verwarf alles, was im ESC-Menue mit
 * "Als Standard speichern" gesichert wurde (z. B. light_world). */
launcher_settings_t g_disk_base;
int g_disk_base_ok;

void disk_base_sync(const launcher_settings_t *s)
{
    char upath[1200];
    game_cfg_path(g_game, upath, sizeof upath); /* W402: Datei des aktiven Spiels */
    g_disk_base = *s;
    lcfg_load_file(upath, &g_disk_base);
    g_disk_base_ok = 1;
}

/* Fremde Aenderungen der user.cfg (Spiel, zweiter Launcher) in s uebernehmen,
 * soweit die Oberflaeche den Key seit dem letzten Abgleich nicht geaendert
 * hat. Rueckgabe: Anzahl uebernommener Keys. */
int disk_merge(launcher_settings_t *s)
{
    char upath[1200];
    launcher_settings_t disk;
    if (!g_disk_base_ok) {
        return 0;
    }
    game_cfg_path(g_game, upath, sizeof upath); /* W402 */
    disk = g_disk_base;
    if (lcfg_load_file(upath, &disk) != 0) {
        return 0; /* keine Datei: nichts zu uebernehmen */
    }
    return lcfg_merge_external(s, &g_disk_base, &disk);
}

/* Speichern ohne Pruefung (do_save nach lcfg_validate, Selbsttest W259):
 * abgleichen, schreiben, neuen Basis-Stand merken, Oberflaeche nachziehen. */
int save_user_merged(const char *upath)
{
    int n = disk_merge(&g_s);
    /* W402: LoC-Datei ohne SB-Spielpfade (der LoC-Start setzt Abbild/Datenordner selbst) */
    if (lcfg_save_user_ex(upath, &g_s, g_game == LGAME_LOC ? LCFG_SAVE_NO_PATHS : 0) != 0) {
        return -1;
    }
    pergame_mark();
    if (loctab_may_write()) { /* W438: gespeicherte Installation -> kein spaeterer Steuerungs-Reset */
        char cp[1200];
        user_cfg_path(cp, sizeof cp);
        (void)lgame_ctrl040_mark(cp);
    }
    if (g_game == LGAME_LOC) { /* W402: Sprache ist global (Launcher liest sie aus user.cfg) */
        char sp[1200];
        user_cfg_path(sp, sizeof sp);
        (void)la_cfg_set(sp, "lang", g_s.lang);
    }
    disk_base_sync(&g_s);
    if (n > 0) {
        ui_from_settings(&g_s);
    }
    return n;
}

/* Client beendet: im Spiel gespeicherte Werte gleich anzeigen. */
int disk_refresh(void)
{
    launcher_settings_t cur = g_s;
    int n;
    if (!g_disk_base_ok) {
        return 0;
    }
    ui_to_settings(&cur);
    n = disk_merge(&cur);
    if (n > 0) {
        g_s = cur;
        ui_from_settings(&g_s);
        disk_base_sync(&g_s);
    }
    return n;
}

int do_save(int complain)
{
    char err[1024], upath[1200];
    ui_to_settings(&g_s);
    /* W375 (#50): Auf dem LoC-Tab gelten nur die gemeinsamen Werte (Grafik/Anzeige/Steuerung); die SB-Spieldaten
     * pruefen wir dort nicht (vorher: "No game data set" ohne SB-Abbild, nichts gespeichert). Das LoC-Abbild
     * speichert loctab_save getrennt, der LoC-Start prueft es selbst (lgame_loc_start_gate). */
    if (lcfg_validate_ex(&g_s, err, sizeof err, g_game == LGAME_LOC ? LCFG_V_NO_GAME : 0) != 0) {
        set_status_k(err, 2);
        if (complain) {
            ui_msgbox(g_hwnd, err, LT("gui.msg_checkdata"),
                      MB_ICONWARNING | MB_OK);
        }
        return 1;
    }
    game_cfg_path(g_game, upath, sizeof upath); /* W402: nur die Datei des aktiven Spiels */
    if (save_user_merged(upath) < 0) { /* W259: im Spiel gespeicherte Werte bleiben */
        _snprintf_s(err, sizeof err, _TRUNCATE, LT("gui.msg_nowrite"), upath);
        set_status_k(err, 2);
        if (complain) {
            ui_msgbox(g_hwnd, err, LT("gui.btn_save"), MB_ICONERROR | MB_OK);
        }
        return 1;
    }
    if (g_game == LGAME_LOC && loctab_may_write()) {
        /* W414: user_loc.cfg traegt das Bildformat (der Client liest dort; ohne Eintrag gaelte sb_native.cfg =
         * 4:3) und die Markierung der Einmal-Umstellung, damit eine spaetere manuelle Wahl erhalten bleibt */
        if (lgame_cfg_save_ex(upath, &g_loc, LGAME_K_WIDE, LGAME_K_LAUNCHER) != 0 ||
            lgame_loc169_mark(upath, g_loc.loc_widescreen, 0) != 0 ||
            lgame_loc_rev_mark(upath) != 0) { /* W465: Marke gfx_loc_rev - die Grafikwahl ist bewusst, auch Original */
            set_status_k(LT("gui.loc_save_err"), 2);
            return 1;
        }
        lnet_mission_flush(); /* W473: Chat im Spiel, gewaehlt bevor es user_loc.cfg gab */
    }
    return 0;
}

/* GUI-Start (W26/W79): pruefen, user.cfg + Spielerbetrieb-Block, Client.
 * dry = 1: alles ausser dem Clientstart (Test --gui-start-dry).
 * W159: vr = 1: "Spielen in VR" (vr_enable=1 + Vorschau); vr = 0:
 * Bildschirm (vr_enable=0 immer, egal was gespeichert ist). Die --set-Kette
 * gilt nur fuer diesen Start, user.cfg bleibt unveraendert. */
char g_start_args[1600]; /* W171: --set-Kette des letzten Starts */
/* W231: Fassung fuer den Start (Fixture der Shots/Selbsttests, sonst lesen). */
void game_info_ui(const launcher_settings_t *s, lcfg_gameinfo_t *gi)
{
    if (g_imgfix_path[0] && strcmp(s->game_image, g_imgfix_path) == 0) {
        memset(gi, 0, sizeof *gi);
        gi->edition = g_imgfix_ed;
        gi->startable = g_imgfix_rc == 0;
        gi->needs_allow = g_imgfix_ed == LCFG_ED_US;
        return;
    }
    lcfg_game_info(s, gi);
}

/* W231 US-LAUNCHER: Client beendet -> Exitcode 5 (Fassung unbekannt) bzw.
 * 8 (US-Profilpruefung) verstaendlich melden. show_box: 8 meldet der
 * Client selbst nicht (nur Log), 5 zeigt er schon an -> nur Statuszeile. */
int client_exit_report(unsigned long code, int edition, int show_box)
{
    const char *k = lcfg_exit_msg_key(code, edition);
    if (!k) {
        return 0;
    }
    set_status_k(LT(k), 2);
    if (show_box && code == 8) {
        ui_msgbox(g_hwnd, LT(k), LT("gui.btn_start"), MB_ICONWARNING | MB_OK);
    }
    return 1;
}

int g_vx_pending; /* W255 VIDEO-EXPORT: laufender Export (Meldung beim Client-Ende) */
void rp_export_done(unsigned long code);

void client_exit_poll(void)
{
    DWORD code = 0;
    if (!g_client_proc) {
        KillTimer(g_hwnd, 7);
        return;
    }
    if (WaitForSingleObject((HANDLE)g_client_proc, 0) != WAIT_OBJECT_0) {
        return;
    }
    GetExitCodeProcess((HANDLE)g_client_proc, &code);
    CloseHandle((HANDLE)g_client_proc);
    g_client_proc = NULL;
    KillTimer(g_hwnd, 7);
    if (g_vx_pending) { /* W255 VIDEO-EXPORT: Ergebnis melden (Exit 0 ok, 2 Replay-Start, 3 Encoder) */
        rp_export_done(code);
        return;
    }
    {
        /* W259: im ESC-Menue gespeicherte Werte sofort im Launcher zeigen */
        int n = disk_refresh();
        if (!client_exit_report(code, g_client_ed, g_o.shot == 0) && n > 0) {
            char st[300];
            _snprintf_s(st, sizeof st, _TRUNCATE, LT("gui.msg_ingame_kept"), n);
            set_status_k(st, 1);
        }
        if (n > 0) {
            redraw_all();
        }
    }
}

int do_start_ex(int dry, int vr, const char *extra)
{
    char err[1400], set_args[1600];
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
    { /* W217 FIRST-RUN: fehlende VC++-Laufzeit klar melden statt Windows-
       * Fehler "VCRUNTIME140.dll wurde nicht gefunden" beim Clientstart. */
        char miss[64];
        if (app_client_runtime_missing(g_client_dir, NULL, miss, sizeof miss)) {
            _snprintf_s(err, sizeof err, _TRUNCATE, LT("gui.msg_vcrt"), miss);
            set_status_k(LT("gui.msg_vcrt_short"), 2);
            if (!dry) {
                ui_msgbox(g_hwnd, err, LT("gui.btn_start"), MB_ICONWARNING | MB_OK);
            }
            return 1;
        }
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
    { /* W231 US-LAUNCHER: US -> allow_ntsc_u=1 (solange experimental) + eigener Saveordner */
        lcfg_gameinfo_t gi;
        game_info_ui(&g_s, &gi);
        if (app_edition_start_args(g_client_dir, &g_s, &gi, set_args, sizeof set_args,
                                   err, sizeof err) != 0) {
            set_status_k(err, 2);
            if (!dry) {
                ui_msgbox(g_hwnd, err, LT("gui.btn_start"), MB_ICONERROR | MB_OK);
            }
            return 1;
        }
        g_client_ed = gi.edition;
    }
    { /* W291 ESC-REWORK: ESC-Gruppe Debug nur mit Alpha-Tools (nur dieser Start, nicht in user.cfg) */
        char ed[24];
        _snprintf_s(ed, sizeof ed, _TRUNCATE, "%sesc_debug=%d", set_args[0] ? ";" : "", g_alpha_tools ? 1 : 0);
        strcat_s(set_args, sizeof set_args, ed);
    }
    if (extra && *extra) { /* W254: Replay-Start ("replay_autoplay=<id>;...", nur dieser Start) */
        strcat_s(set_args, sizeof set_args, ";");
        strcat_s(set_args, sizeof set_args, extra);
    }
    strcpy_s(g_start_args, sizeof g_start_args, set_args); /* W171: Trockenlauf-Ausgabe */
    if (dry) {
        set_status_k(LT("gui.msg_dry"),
                     1);
        return 0;
    }
    {
        char xbe[1024]; /* W172/P6: im Ordner-Modus abgeleitet */
        void *proc = NULL;
        lcfg_xbe_effective(&g_s, xbe, sizeof xbe);
        if (app_launch_client_proc(g_client_dir, xbe, set_args, err,
                                   sizeof err, &proc) != 0) {
            set_status_k(err, 2);
            ui_msgbox(g_hwnd, err, LT("gui.btn_start"), MB_ICONERROR | MB_OK);
            return 1;
        }
        if (g_client_proc) { /* W231: aelteren Start nicht mehr beobachten */
            CloseHandle((HANDLE)g_client_proc);
        }
        g_client_proc = proc;
        if (proc) {
            SetTimer(g_hwnd, 7, 1000, NULL); /* W231: Exitcode 5/8 deuten */
        }
    }
    {
        char st[1500];
        _snprintf_s(st, sizeof st, _TRUNCATE, "%s user.cfg gespeichert.", err);
        set_status_k(st, 1);
    }
    return 0;
}

int do_start(int dry, int vr)
{
    return do_start_ex(dry, vr, NULL);
}

void browse_xbe(void)
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
void check_image_ex(int status)
{
    char p[1024];
    ui_gettext(ctl(ID_IMAGE), p, sizeof p);
    strcpy_s(g_img_path, sizeof g_img_path, p); /* W220: Meldung gilt nur fuer diesen Pfad */
    if (!p[0]) {
        g_img_msg[0] = 0;
        g_img_ok = 0;
    } else {
        g_img_ok = img_check(p, g_img_msg, sizeof g_img_msg) == 0;
        if (status) {
            set_status_k(g_img_msg, g_img_ok ? 1 : 2);
        }
    }
    /* W220: ganze Karte (die Ergebniszeile liegt unter CY0+280) */
    redraw_rect(CX0, CY0, CW, 456);
}

void check_image(void)
{
    check_image_ex(1);
}

/* W150/W172: Abbild gewaehlt (Dialog; W220 auch Shots/Selbsttest 62).
 * Gueltig -> Bereitschaft statt Einrichtung; der Aufrufer (on_command)
 * wertet danach das Regelwerk aus und baut das Bild neu auf (W220). */
void image_chosen(const wchar_t *file)
{
    SetWindowTextW(ctl(ID_IMAGE), file);
    check_image();
    if (g_img_ok) { /* W172/P6: gueltig -> Bereitschaft statt Einrichtung */
        g_setup_force = 0;
    }
}

void browse_image(void)
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
    if (n1 + 19 + n2 + 5 < 200) { /* "<f1>|*.iso;*.xiso;*.xbe|<f2>|*.*||" (W369: + .xbe) */
        wcscpy_s(filter, 200, f1);
        wcscpy_s(filter + n1, 200 - n1, L"*.iso;*.xiso;*.xbe");
        wcscpy_s(filter + n1 + 19, 200 - n1 - 19, f2);
        wcscpy_s(filter + n1 + 19 + n2, 200 - n1 - 19 - n2, L"*.*");
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
        image_chosen(file);
    }
}

/* W172/P6: Hinweistext im leeren Programmdatei-Feld ("automatisch:
 * <Spielordner>\default.xbe"). */
void xbe_cue_update(void)
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
        strncpy_s(t, sizeof t, LT("gui.adv_xbe_cue0"), _TRUNCATE);
    }
    u8_to_w(t, w, 1500);
    SendMessageW(e, EM_SETCUEBANNER, TRUE, (LPARAM)w);
}

/* W172/P7: Schalter "Beim Launcher-Start laden" = gewaehlte Einstellung
 * ist die gespeicherte Standard-Einstellung (profiles/standard.txt). */
void setup_auto_sync(int from_list)
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
        if (sel >= 0 && SendMessageW(cb, CB_GETLBTEXT, (WPARAM)sel, (LPARAM)w) != CB_ERR) {
            w_to_u8(w, name, (int)sizeof name);
        }
    } else {
        ui_gettext(cb, name, sizeof name);
        trim_u8(name);
    }
    ui_set_checked(ID_PROF_DFLT, name[0] && strcmp(name, g_dflt_setup) == 0);
}

/* W282 ISSUE6: Der Ordner-Modus loest ein ungueltiges Abbild ab. Steht im
 * Abbild-Feld ein Pfad, den die letzte Pruefung abgelehnt hat (R3, kein
 * gueltiges Spiel), wuerde er den gewaehlten Spielordner ueberdecken
 * (Abbild hat Vorrang) - Feld leeren, Meldung verwerfen. Ein gueltiges
 * Abbild bleibt (dann ist der Ordner-Modus ohnehin per R1 gesperrt).
 * Rueckgabe 1 = geleert. */
int media_drop_bad_image(void)
{
    char img[1024];
    if (!ctl(ID_IMAGE)) {
        return 0;
    }
    ui_gettext(ctl(ID_IMAGE), img, sizeof img);
    if (!img[0] || g_data_ok) {
        return 0;
    }
    ui_settext(ctl(ID_IMAGE), "");
    g_img_msg[0] = 0;
    g_img_path[0] = 0;
    g_img_ok = 0;
    return 1;
}

void browse_media(void)
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
            media_drop_bad_image(); /* W282 ISSUE6 */
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
/* W278 ORIG-FLAG: Hinweis "nicht die Original-Optik" auf Spielen (beide */
/* Bereiche), "Back to original" (nur experimentelle Grafik), Einmal-    */
/* Start im Original-Modus (--set nur fuer diesen Start), Plaketten      */
/* "EXPERIMENTAL" an den Karten. Logik: launcher_orig.c, Tabelle:        */
/* harness/src/sb_origflag.h.                                            */
/* ------------------------------------------------------------------ */
#define OB_Y (CY0 + 456) /* SB: Band an der Stelle des Primaerfeuer-Hinweises */
#define OB_H 40
/* W293 LOC-LAYOUT: LoC-Band unten wie SB, eine Zeile hoeher fuer "These
 * settings are shared with Steel Battalion." (ohne Einmal-Start) */
#define OB_BTN_ONCE_W 172
#define OB_BTN_RESET_W 150
#define XI_H 24

int orig_banner_on(void)
{
    return g_orig_mask != 0 && (g_game == LGAME_LOC || !play_setup_shown());
}

int orig_visible(int id, int page)
{
    if (id != ID_ORIG_RESET && id != ID_ORIG_ONCE && id != ID_ORIG_RESET_LOC && id != ID_XWARN_SB &&
        id != ID_XWARN_LOC) {
        return -1;
    }
    if (page != P_PLAY || !orig_banner_on()) {
        return 0;
    }
    return (id == ID_ORIG_RESET_LOC || id == ID_XWARN_LOC) ? g_game == LGAME_LOC : g_game != LGAME_LOC;
}

/* W287 EXP-ICON: Texte der Symbole (Tooltip = zugaenglicher Name).      */
/* Experimentell-Symbol: gemeinsame Beschreibung + Grund der Karte bzw.  */
/* die Gruppen, die ein Preset aendert. Warn-Symbol: aktive Gruppen.     */
void xtip_text(int id, char *out, size_t cap)
{
    char list[400];
    out[0] = '\0';
    if (id >= ID_XEXP_FIRST && id <= ID_XEXP_LAST) {
        static const char *const kWhy[] = { "gui.exp_tip_wide", "gui.exp_tip_image", "gui.exp_tip_world",
                                            "gui.exp_tip_hud", "gui.exp_tip_packs" };
        _snprintf_s(out, cap, _TRUNCATE, "%s\n%s", LT("gui.exp_tip"), LT(kWhy[id - ID_XEXP_FIRST]));
    } else if (id == ID_CHIDE || id == ID_REM_LABEL) { /* W291: frueher Textzeilen in der Karte */
        if (id == ID_CHIDE) { /* W319: Vollbild-Ansicht und Monitoransicht erklaert */
            _snprintf_s(out, cap, _TRUNCATE, "%s\n%s", LT("gui.chide_txt"), LT("gui.mview_txt"));
        } else {
            strncpy_s(out, cap, LT("gui.rem_label_txt"), _TRUNCATE);
        }
    } else if (id == ID_XEXP_PHOTO) { /* W290: breit (Aussenwelt), REPLAY/Logo aus */
        _snprintf_s(out, cap, _TRUNCATE, "%s\n%s", LT("gui.exp_tip"), LT("gui.exp_tip_photo"));
    } else if (id == ID_LOC_SMOOTH) { /* W387 LOC-SMOOTH-OPP */
        strncpy_s(out, cap, LT("gui.loc_smooth_tip"), _TRUNCATE);
    } else if (id == ID_CTRL_AIMASSIST || id == ID_CTRL_AUTOGEAR) { /* W440: in LoC nur in Arcade-Lobbys (SB: kein Tooltip) */
        if (g_game == LGAME_LOC) {
            strncpy_s(out, cap, LT("gui.loc_arcade_tip"), _TRUNCATE);
        }
    } else if (id == ID_LOC_WIDE_XI) { /* W326 LOC-WIDE: Bildformat 16:9/720p (LoC) */
        _snprintf_s(out, cap, _TRUNCATE, "%s\n%s", LT("gui.exp_tip"), LT("gui.exp_tip_locwide"));
    } else if (id == ID_XWARN_SB || id == ID_XWARN_LOC) {
        if (g_orig_mask) {
            lorig_list_sep(g_orig_mask, "\n", "\xE2\x80\xA2 ", list, sizeof list); /* ein Punkt je Zeile */
            _snprintf_s(out, cap, _TRUNCATE, LT("gui.exp_warn_tip"), list);
        }
    } else if (id >= ID_GP0 && id < ID_GP0 + LGP_CUSTOM) {
        if (xexp_preset(id - ID_GP0)) {
            char ch[440];
            lorig_list(lorig_preset_mask(id - ID_GP0), list, sizeof list);
            _snprintf_s(ch, sizeof ch, _TRUNCATE, LT("gui.exp_tip_preset"), list);
            _snprintf_s(out, cap, _TRUNCATE, "%s\n%s", LT("gui.exp_tip"), ch);
        }
    } else if (id >= ID_NAV0 && id < ID_NAV0 + P_N) {
        unsigned m = xwarn_page_mask(id - ID_NAV0);
        if (m) {
            lorig_list(m, list, sizeof list);
            _snprintf_s(out, cap, _TRUNCATE, LT("gui.exp_warn_nav"), list);
        }
    }
}

/* Fenstertexte nachziehen (Sprache, Maske): Symbole und Navigation
 * (zweite Zeile = Hinweis fuer Bildschirmleser, gezeichnet wird das Symbol). */
void xicon_texts(void)
{
    static const int kNav[] = { P_DISPLAY, P_GFX, P_PACKS };
    char t[700];
    int id, i;
    for (id = ID_XEXP_FIRST; id <= ID_XWARN_LOC; id++) {
        xtip_text(id, t, sizeof t);
        if (ctl(id)) {
            ui_settext_if(ctl(id), t);
        }
    }
    if (ctl(ID_XEXP_PHOTO)) { /* W290 */
        xtip_text(ID_XEXP_PHOTO, t, sizeof t);
        ui_settext_if(ctl(ID_XEXP_PHOTO), t);
    }
    if (ctl(ID_LOC_WIDE_XI)) { /* W326 */
        xtip_text(ID_LOC_WIDE_XI, t, sizeof t);
        ui_settext_if(ctl(ID_LOC_WIDE_XI), t);
    }
    for (i = 0; i < (int)(sizeof kNav / sizeof kNav[0]); i++) {
        HWND h = ctl(ID_NAV0 + kNav[i]);
        char n[760];
        if (!h) {
            continue;
        }
        xtip_text(ID_NAV0 + kNav[i], t, sizeof t);
        if (t[0]) {
            _snprintf_s(n, sizeof n, _TRUNCATE, "%s\n%s", LT(page_name_key(kNav[i])), t);
        } else {
            strncpy_s(n, sizeof n, LT(page_name_key(kNav[i])), _TRUNCATE);
        }
        ui_settext_if(h, n);
    }
}

/* loc = 0: Band unten auf SB-Spielen; 1: Band unten auf LoC-Spielen (W293:
 * gleiche Optik, statt der Karte oben rechts; Zusatzzeile "gemeinsam"). */
void orig_paint_play(int loc)
{
    char list[300], t[420];
    int y = loc ? OB_Y_LOC : OB_Y, h = loc ? OB_H_LOC : OB_H;
    int tw = loc ? CW - 44 - 12 - OB_BTN_RESET_W - 12 /* ohne Einmal-Start */
                 : CW - 44 - 12 - OB_BTN_ONCE_W - 8 - OB_BTN_RESET_W - 12;
    lorig_list(g_orig_mask, list, sizeof list);
    _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.of_banner_l"), list);
    /* W287: Warn-Symbol (ID_XWARN_SB/_LOC) vorn, Text dahinter */
    L_fill(CX0, y, CW, h, C_WARN, 34, 10);
    L_ring(CX0, y, CW, h, C_WARN, 130, 10);
    label1(F_SMALL, LT("gui.of_banner_t"), CX0 + 44, y + 2, tw, 19, C_WARN);
    label1(F_TINY, t, CX0 + 44, y + 21, tw, 16, C_TEXT);
    if (loc) {
        label1(F_TINY, LT("gui.of_banner_loc"), CX0 + 44, y + 35, tw, 15, C_DIM);
    }
}

static void orig_build(HWND w)
{
    mk(w, K_BTN, ID_ORIG_RESET, P_PLAY, CX0 + CW - 12 - OB_BTN_ONCE_W - 8 - OB_BTN_RESET_W, OB_Y + 6,
       OB_BTN_RESET_W, 28, LT("gui.of_btn_reset"), 0);
    mk(w, K_BTN, ID_ORIG_ONCE, P_PLAY, CX0 + CW - 12 - OB_BTN_ONCE_W, OB_Y + 6, OB_BTN_ONCE_W, 28,
       LT("gui.of_btn_once"), 0);
    mk(w, K_BTN, ID_ORIG_RESET_LOC, P_PLAY, CX0 + CW - 12 - OB_BTN_RESET_W, OB_Y_LOC + (OB_H_LOC - 28) / 2,
       OB_BTN_RESET_W, 28, LT("gui.of_btn_reset"), 0); /* W293: rechts im Band (kein Einmal-Start) */
    /* W287 EXP-ICON: Kolben rechts in der Kopfzeile der Karten mit experimentellen
     * Einstellungen (statt der Plakette), Warn-Symbol vorn im Hinweis */
    mk(w, K_ICON, ID_XEXP_DISP, P_DISPLAY, COL2 + COLW - 18 - XI_W, CY0 + 12, XI_W, XI_H, "", 0);
    mk(w, K_ICON, ID_XEXP_IMAGE, P_GFX, CX0 + COLW - 18 - XI_W, CY0 + 352 + 12, XI_W, XI_H, "", 0);
    mk(w, K_ICON, ID_XEXP_WORLD, P_GFX, COL2 + COLW - 18 - XI_W, CY0 + 12, XI_W, XI_H, "", 0);
    mk(w, K_ICON, ID_XEXP_HUD, P_GFX, COL2 + COLW - 18 - XI_W, CY0 + 304 + 12, XI_W, XI_H, "", 0);
    mk(w, K_ICON, ID_XEXP_PACKS, P_PACKS, CX0 + PKL_W - 18 - 150 - 8 - XI_W, CY0 + 12, XI_W, XI_H, "", 0); /* links neben "Pack hinzufuegen" */
    mk(w, K_ICON, ID_XEXP_PHOTO, P_AUDIO, CX0 + CW - 18 - XI_W, CY0 + 152 + EXY + 12, XI_W, XI_H, "", 0); /* W290, W303 */
    mk(w, K_ICON, ID_XWARN_SB, P_PLAY, CX0 + 10, OB_Y + 8, 26, 24, "", 0);
    mk(w, K_ICON, ID_XWARN_LOC, P_PLAY, CX0 + 10, OB_Y_LOC + 8, 26, 24, "", 0); /* W293: wie ID_XWARN_SB */
    xicon_texts();
}

void orig_relabel(void)
{
    ui_settext(ctl(ID_ORIG_RESET), LT("gui.of_btn_reset"));
    ui_settext(ctl(ID_ORIG_ONCE), LT("gui.of_btn_once"));
    ui_settext(ctl(ID_ORIG_RESET_LOC), LT("gui.of_btn_reset"));
    xicon_texts(); /* W287 */
}

/* "Back to original": nur experimentelle Grafik, dann speichern (sonst
 * verschwaende der Hinweis, ohne dass etwas gesichert ist); Rueckgaengig
 * bleibt (Muster C). Shots speichern nie. */
void orig_reset_now(void)
{
    launcher_settings_t before, s;
    snap_settings(&before);
    s = before;
    lorig_reset(&s);
    /* W414 (Nutzer 21.11.): "Back to original" im LoC-Tab setzt die Bildgebung auf Original, das Bildformat
     * 16:9 bleibt (in LoC keine Abweichung mehr; W326 setzte hier 4:3). 4:3 waehlt der Nutzer unter
     * Erweitert > Bildformat. Mit 16:9 bleiben auch die gesperrten Raender (Fade out) unveraendert. */
    if (pergame_box_169()) {
        s.edge_mode = before.edge_mode;
        s.edge_fade_width = before.edge_fade_width;
        s.edge_fade_strength = before.edge_fade_strength;
    }
    if (g_game == LGAME_LOC) { loctab_sblook_reset(); } /* W459: SB look (Experimental) aus */
    g_s = s;
    ui_from_settings(&s);
    update_aspect_info();
    gfx_sync_ui();
    {
        int saved = 0;
        if (g_o.shot == 0 || g_ucfg_test[0]) {
            saved = do_save(0) == 0; /* ohne gueltige Spieldaten: nicht speichern, keine Meldebox */
        }
        set_status_undo(LT(saved ? "gui.of_reset_done" : "gui.of_reset_unsaved"), &before);
    }
    ui_refresh_visibility();
    redraw_all();
}

/* Einmal-Start: Override nur fuer diesen Start (user.cfg bekommt nur den
 * ueblichen Stand der Oberflaeche + Spielerbetrieb-Block). dry = Test. */
int orig_once_start(int dry)
{
    char set[2048];
    int rc;
    if (lorig_once_args(set, sizeof set) != 0) {
        return 1;
    }
    rc = do_start_ex(dry, 0, set);
    if (rc == 0) {
        set_status_k(LT("gui.of_once_done"), 1);
    }
    return rc;
}

int orig_command(int id, int code)
{
    if (id != ID_ORIG_RESET && id != ID_ORIG_ONCE && id != ID_ORIG_RESET_LOC) {
        return 0;
    }
    if (code != BN_CLICKED) {
        return 1;
    }
    if (id == ID_ORIG_ONCE) {
        if (g_game == LGAME_LOC) {
            set_status_k(LT("gui.loc_soon"), 1);
        } else if (launch_blocked()) { /* wie Launch: Grund statt Startversuch */
            static const int kBlk[] = { 4, 3 };
            set_status_k(g_rr[first_rule(kBlk, 2)].text, 2);
        } else {
            orig_once_start(0);
        }
    } else {
        orig_reset_now();
    }
    return 1;
}

void build_controls(void)
{
    HWND c;
    int i;
    HWND w = g_hwnd;
    /* W171 LAUNCHER-P2P3: Reihenfolge = Tab-Reihenfolge (je Seite links
     * oben nach unten, dann rechte Spalte); Freigaben setzt ausschliesslich
     * ui_apply_rules (Regelwerk), hier kein EnableWindow. */
    /* Titelleiste */
    mk(w, K_TBMIN, ID_MIN, PG_ALL, HDR_WB_X, 0, HDR_WB_W, HDR_WB_H, LT("gui.win_min"), 0); /* W401: kleiner */
    mk(w, K_TBCLOSE, ID_CLOSE, PG_ALL, HDR_WB_X + HDR_WB_W, 0, HDR_WB_W, HDR_WB_H, LT("gui.btn_close"), 0);
    /* W397 LAUNCHER-STRUCT: Zahnrad = Launcher-Einstellungen (gewaehlt, solange die Seite offen ist) */
    mk(w, K_SEG, ID_LSET_OPEN, PG_ALL, HDR_GEAR_X, HDR_CHIP_Y, HDR_GEAR_W, HDR_CHIP_H, LT("gui.lset_title"), 0);
    /* Navigation (7 Seiten) */
    for (i = 0; i < P_N; i++) {
        mk(w, K_NAV, ID_NAV0 + i, PG_ALL, 12, TITLE_H + 16 + i * 44,
           NAV_W - 24, 38, LT(page_name_key(i)), 0); /* W392 */
    }
    /* W399: Unterpunkte von Multiplayer (nur LoC, nur solange Multiplayer offen ist; Lage setzt nav_sub_layout) */
    mk(w, K_NAV, ID_NAVSUB_MATCH, PG_ALL, 12, TITLE_H + 16, NAV_W - 24, NAVSUB_H, LT("gui.mp_sub_match"), 0);
    mk(w, K_NAV, ID_NAVSUB_ACC, PG_ALL, 12, TITLE_H + 16, NAV_W - 24, NAVSUB_H, LT("gui.mp_sub_acc"), 0);
    hub_build(w); /* W470: Reiter der Bereiche Optionen/steelnet (eigene Navigation) */
    /* W142: Sprachwahl (wirkt sofort); W397: von der Seitenleiste auf die Launcher-Einstellungen */
    mk(w, K_STATIC, ID_LANG_LBL, P_LSET, LSET_R_X + 18, LSET_LANG_Y + 46, 120, 24, LT("gui.lang_label"),
       SS_CENTERIMAGE);
    c = mk(w, K_COMBO, ID_LANG, P_LSET, LSET_R_X + 140, LSET_LANG_Y + 42, COLW - 158, 30, "", 0);
    combo_add(c, LT("gui.lang_en"));
    combo_add(c, LT("gui.lang_de"));
    ComboBox_SetCurSel(c, 0);
    /* W281 LAUNCHER-SIZE: Layout-Vorschau (nur in a/b sichtbar, wirkt sofort) */
    /* W397: Fenstergroesse ebenfalls auf den Launcher-Einstellungen */
    mk(w, K_STATIC, ID_LAYOUT_LBL, P_LSET, LSET_R_X + 18, LSET_LANG_Y + 84, 120, 30, LT("gui.lay_size"),
       SS_CENTERIMAGE); /* W287 */
    c = mk(w, K_COMBO, ID_LAYOUT, P_LSET, LSET_R_X + 140, LSET_LANG_Y + 84, COLW - 158, 30, "", 0);
    lay_combo_fill(c);

    /* --- Spielen: Startknoepfe (W159: Bildschirm + VR), Disc-Abbild --- */
    mk(w, K_BIG, ID_START_BIG, P_PLAY, CX0, CY0 + 84, 308, 56, LT("gui.btn_bigstart"), 0);
    mk(w, K_BIG, ID_START_VR, P_PLAY, CX0, CY0 + 146, 308, 40, LT("gui.btn_vrstart"), 0);
    /* W150 ISO-DIRECT, W172/P6: Einrichtungskarte (nur ohne gueltige
     * Spieldaten oder nach "Aendern"): Abbild-Zeile + Verweis Ordner-Modus */
    mk(w, K_EDIT, ID_IMAGE, P_PLAY, CX0 + 18, CY0 + 276, CW - 300, 32, "", 0);
    mk(w, K_BTN, ID_IMAGE_C, P_PLAY, CX0 + CW - 272, CY0 + 276, 122, 32, LT("gui.btn_check"), 0);
    mk(w, K_BTN, ID_IMAGE_B, P_PLAY, CX0 + CW - 140, CY0 + 276, 122, 32, LT("gui.btn_browse"), 0);
    mk(w, K_BTN, ID_PLAY_FOLDER, P_PLAY, CX0 + CW - 18 - 230, CY0 + 396, 230, 30,
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
    c = mk(w, K_COMBO, ID_PRESET, P_DISPLAY, CX0 + 120, CY0 + 116, COLW - 138,
           30, "", 0);
    for (i = 0; i < KPRESETS_N; i++) {
        combo_add(c, kPresets[i].lkey ? LT(kPresets[i].lkey) : kPresets[i].label);
    }
    mk(w, K_EDIT, ID_W, P_DISPLAY, CX0 + 120, CY0 + 152, 90, 30, "",
       ES_NUMBER);
    mk(w, K_EDIT, ID_H, P_DISPLAY, CX0 + 232, CY0 + 152, 90, 30, "",
       ES_NUMBER);
    /* Bildrate (Mission) */
    c = mk(w, K_COMBO, ID_IPFPS, P_DISPLAY, CX0 + 120, CY0 + 188, COLW - 138, /* W291: Karte Anzeige */
           30, "", 0);
    ip_fill(c); /* W223 */
    /* W134 VR-PROTO, W159: Vorschau-Art (Reihenfolge = vr_from/to_settings) */
    c = mk(w, K_COMBO, ID_VR, P_DISPLAY, CX0 + 120, CY0 + 356, COLW - 138, 30, "", 0); /* W291 */
    combo_add(c, LT("gui.vr_prev_sbs"));
    combo_add(c, LT("gui.vr_prev_left"));
    combo_add(c, LT("gui.vr_prev_right"));
    combo_add(c, LT("gui.vr_prev_off"));
    ComboBox_SetCurSel(c, 1);
    /* W183 VR-FIX3: VR-Leistung (Preset) + Render-Aufloesung nebeneinander */
    c = mk(w, K_COMBO, ID_VRPRESET, P_DISPLAY, CX0 + 120, CY0 + 390, COLW - 138 - 8 - EDGE_W1, 30, "", 0); /* W291 */
    combo_add(c, LT("gui.vrp_perf"));
    combo_add(c, LT("gui.vrp_bal"));
    combo_add(c, LT("gui.vrp_qual"));
    combo_add(c, LT("gui.vrp_custom"));
    ComboBox_SetCurSel(c, 1);
    c = mk(w, K_COMBO, ID_VRRES, P_DISPLAY, CX0 + 120 + COLW - 138 - EDGE_W1, CY0 + 390,
           EDGE_W1, 30, "", 0);
    vrres_fill(c); /* W227: "Res. 80 %" */
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
           EDGE_W1, 30, "", 0);
    for (i = 0; i < KEDGEW_N; i++) {
        char t[32];
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.edge_w_fmt"), kEdgeW[i]);
        combo_add(c, t);
    }
    ComboBox_SetCurSel(c, 2);
    c = mk(w, K_COMBO, ID_EDGE_S, P_DISPLAY, COL2 + 120 + EDGE_W1 + 8,
           CY0 + 136, COLW - 138 - 8 - EDGE_W1, 30, "", 0);
    for (i = 0; i < KEDGES_N; i++) {
        char t[32];
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.edge_s_fmt"), kEdgeS[i]);
        combo_add(c, t);
    }
    ComboBox_SetCurSel(c, KEDGES_N - 1);
    /* W191 UW-PERSP: Cockpit-Perspektive (Kamera vor, Hauptmonitor fest, nie gestreckt) */
    c = mk(w, K_COMBO, ID_CPERSP, P_DISPLAY, COL2 + 120, CY0 + 186, COLW - 138, 30, "", 0);
    cpersp_fill(c);
    ComboBox_SetCurSel(c, 0);
    c = mk(w, K_COMBO, ID_CPANINI, P_DISPLAY, COL2 + 120, CY0 + 220,
           COLW - 138, 30, "", 0); /* W191: Panini (W227: eigene Zeile mit Beschriftung) */
    cpersp_fill(c);
    ComboBox_SetCurSel(c, 0);
    c = mk(w, K_COMBO, ID_CWIDTH, P_DISPLAY, COL2 + 120, CY0 + 254,
           COLW - 138, 30, "", 0); /* W235 COCKPIT-WIDTH: Breite (gleiche Optik wie Kamera/Panini) */
    cwidth_fill(c);
    ComboBox_SetCurSel(c, 0);
    /* W148: Render-Skalierung (Index: 0 = Auto dyn, 1 = Auto fest, 2..5 = 1..4) */
    c = mk(w, K_COMBO, ID_RTSCALE, P_DISPLAY, CX0 + 120, CY0 + 224, COLW - 138, 30, "", 0); /* W291 */
    combo_add(c, LT("gui.rs_dyn"));
    combo_add(c, LT("gui.rs_autof"));
    combo_add(c, "1");
    combo_add(c, "2");
    combo_add(c, "3");
    combo_add(c, "4");
    ComboBox_SetCurSel(c, 0);

    /* --- Grafik (W173 LAUNCHER-P4, Konzept 4.3): links Preset (fuenf
     * Optionsfelder, Zuruecksetzen, Sprung Packs) + Bild, rechts Welt + HUD.
     * Anlage-Reihenfolge = Tab-Reihenfolge (links oben -> unten, dann rechts). --- */
    for (i = 0; i <= LGP_CUSTOM; i++) {
        /* W465: hoeher (48, zwei Hilfezeilen passen); die Lage setzt gfx_preset_layout (nur sichtbare Presets,
         * ohne Luecken, Abstand GP_ROW_H) - hoechstens vier Zeilen statt sechs (W252: 40) */
        mk(w, K_RADIO, ID_GP0 + i, P_GFX, CX0 + 18, CY0 + 42 + i * 40,
           i == LGP_CUSTOM ? COLW - 36 - 92 : COLW - 36, 48, "", 0);
    }
    mk(w, K_BTN, ID_GP_RESET, P_GFX, CX0 + COLW - 18 - 84, CY0 + 42 + LGP_CUSTOM * 40 + 4, 84, 26,
       LT("gui.gfx_reset"), 0);
    mk(w, K_BTN, ID_GFX_PACKS, P_GFX, CX0 + COLW - 18 - 96, LAY_TEX_Y + 82, 96, 26,
       LT("gui.gfx_to_packs"), 0); /* W291: Karte Texturen */
    c = mk(w, K_COMBO, ID_AAMODE, P_GFX, CX0 + 120, CY0 + 396, COLW - 138,
           30, "", 0); /* W131: aa_mode 0..3 */
    combo_add(c, LT("gui.aa_off"));
    combo_add(c, LT("gui.aa_final"));
    combo_add(c, LT("gui.aa_world"));
    combo_add(c, LT("gui.aa_both"));
    ComboBox_SetCurSel(c, 0);
    c = mk(w, K_COMBO, ID_MONFX, P_GFX, CX0 + 120, CY0 + 432, COLW - 138,
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
    c = mk(w, K_COMBO, ID_GFX_TEX, P_GFX, CX0 + 120, LAY_TEX_Y + 44, COLW - 138, 30, "", 0); /* W291 */
    combo_add(c, LT("gui.gfx_tex_o"));
    combo_add(c, LT("gui.gfx_tex_hd"));
    ComboBox_SetCurSel(c, 0);
    SendMessageW(c, CB_SETDROPPEDWIDTH, (WPARAM)SX(260), 0);
    /* W172/P5: Texturdetail nur auf Packs; die Kombi bleibt unsichtbar als
     * Traeger fuer hd_scale. */
    c = mk(w, K_COMBO, ID_REM_SCALE, P_GFX, CX0 + 120, LAY_TEX_Y + 44,
           COLW - 138, 30, "", 0);
    combo_add(c, LT("gui.scale2"));
    combo_add(c, LT("gui.scale4"));
    ComboBox_SetCurSel(c, 1);
    mk(w, K_TOGGLE, ID_REM_LABEL, P_GFX, COL2 + 18, CY0 + 348, COLW - 36, 28, LT("gui.rem_label"), 0); /* W153 */
    c = mk(w, K_COMBO, ID_CHIDE, P_GFX, COL2 + 120, CY0 + 80, COLW - 138, 30, "", 0); /* W268; W319: Auswahl */
    combo_add(c, LT("gui.cview_orig"));
    combo_add(c, LT("gui.cview_flat"));
    combo_add(c, LT("gui.cview_mon"));
    combo_add(c, LT("gui.cview_mon2"));
    ComboBox_SetCurSel(c, 0);
    SendMessageW(c, CB_SETDROPPEDWIDTH, (WPARAM)SX(300), 0);
    /* W291 ESC-REWORK: Feinregler zeigen (Anzeige + Grafik, gemerkt in launcher_ui.cfg) */
    mk(w, K_TOGGLE, ID_SHOWADV, PG_ALL, CX0 + 4, LAY_ADV_Y, COLW - 8, 28, LT("gui.show_adv"), 0);

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
#if !SB_EDITION_COMMUNITY
    mk(w, K_BTN, ID_PK_CREATOR, P_PACKS, CX0 + 18, CY0 + 118, 196, 30,
       LT("gui.pk_creator"), 0);
#endif
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
    /* W419 CTRL-EXPORT: Export/Import in der Titelzeile der Karte "Tastenbelegung" (wie Einrichtungsanleitung) */
    mk(w, K_BTN, ID_CTRL_EXPORT, P_CTRL, CX0 + COLW - 18 - 84 - 6 - 84, CY0 + 270, 84, 26, LT("gui.cx_export"), 0);
    mk(w, K_BTN, ID_CTRL_IMPORT, P_CTRL, CX0 + COLW - 18 - 84, CY0 + 270, 84, 26, LT("gui.cx_import"), 0);
    /* W221 HOTAS: Joystick-Dialog in der Titelzeile der Gamepad-Karte */
    mk(w, K_BTN, ID_CTRL_JOY, P_CTRL, CX0 + COLW - 18 - 156, CY0 + 402, 156,
       26, LT("gui.joy_btn"), 0);
    /* W87: Maus-Zielen absolut/relativ + Empfindlichkeit */
    c = mk(w, K_COMBO, ID_MOUSE_MODE, P_CTRL, COL2 + 78, CY0 + 108,
           COLW - 18 - 54 - 48 - 8 - 78, 30, "", 0);
    SendMessageW(c, CB_SETDROPPEDWIDTH, (WPARAM)SX(240), 0);
    combo_add(c, LT("gui.mouse_abs"));
    combo_add(c, LT("gui.mouse_rel"));
    c = mk(w, K_COMBO, ID_MOUSE_SENS, P_CTRL, COL2 + COLW - 18 - 54,
           CY0 + 108, 54, 30, "", 0);
    {
        char t[8];
        for (i = 1; i <= 16; i++) {
            _snprintf_s(t, sizeof t, _TRUNCATE, "%d", i);
            combo_add(c, t);
        }
    }
    /* W218 SIMPLE-CONTROLS: vereinfachte Steuerung (Standard aus; nur
     * Maus/Tastatur/Gamepad, mit aktivem Original-Controller ohne Wirkung) */
    /* W236 AIM-ASSIST: Zielhilfe als Auswahl (ersetzt W218 "Fadenkreuz dreht
     * Mech"); Erklaerung je Stufe in der Statuszeile. */
    c = mk(w, K_COMBO, ID_CTRL_AIMASSIST, P_CTRL, COL2 + 18, CY0 + 158, COLW - 36, 30, "", 0);
    SendMessageW(c, CB_SETDROPPEDWIDTH, (WPARAM)SX(360), 0);
    combo_add(c, LT("gui.ctl_assist0"));
    combo_add(c, LT("gui.ctl_assist1"));
    combo_add(c, LT("gui.ctl_assist2"));
    mk(w, K_TOGGLE, ID_CTRL_AUTOGEAR, P_CTRL, COL2 + 18, CY0 + 192, COLW - 36, 28, LT("gui.ctl_autogear"), 0);
    /* W424 SB-ALLIED-SQUADS: Spiel-Option (Standard an), darunter Stick und Tastenliste je 34 px tiefer */
    mk(w, K_TOGGLE, ID_CTRL_ALLY, P_CTRL, COL2 + 18, CY0 + 226, COLW - 36, 28, LT("gui.ctl_ally"), 0);
    /* W271 STICK-RELATIVE: Gamepad-/Joystick-Stick auf dem Zielhebel
     * (Position = Original, "wie eine Maus" = haelt beim Loslassen) */
    c = mk(w, K_COMBO, ID_STICK_AIM, P_CTRL, COL2 + 78, CY0 + 262,
           COLW - 18 - 54 - 48 - 8 - 78, 30, "", 0);
    SendMessageW(c, CB_SETDROPPEDWIDTH, (WPARAM)SX(300), 0);
    combo_add(c, LT("gui.stick_pos"));
    combo_add(c, LT("gui.stick_rel"));
    c = mk(w, K_COMBO, ID_STICK_SENS, P_CTRL, COL2 + COLW - 18 - 54,
           CY0 + 262, 54, 30, "", 0);
    {
        char t[8];
        for (i = 1; i <= 16; i++) {
            _snprintf_s(t, sizeof t, _TRUNCATE, "%d", i);
            combo_add(c, t);
        }
    }
    /* W439 LOC-RETICLE-JUMP: Ausreisser-Filter (Standard an) ueber der Tastenuebersicht (Memo 34 px kuerzer) */
    mk(w, K_TOGGLE, ID_CTRL_DESPIKE, P_CTRL, COL2 + 18, CY0 + 296, COLW - 36, 28, LT("gui.ctl_despike"), 0);
    mk(w, K_MEMO, ID_KEYS, P_CTRL, COL2 + 18, CY0 + 334, COLW - 36, 104,
       "", 0);

    /* W179 KEYS-HTML: Tastenuebersicht als HTML (immer aktiv, ohne Regel) */
    mk(w, K_BTN, ID_CTRL_BROWSER, P_CTRL, COL2 + 18, CY0 + 446, COLW - 36, 30,
       LT("gui.keys_browser"), 0);

    /* --- Ton --- */
    /* W303: Schalter schmal (rechts daneben die Ausgabe), darunter die Lautstaerke-Regler */
    mk(w, K_TOGGLE, ID_AUDIO, P_AUDIO, CX0 + 18, CY0 + 46, 150, 28, LT("gui.ton_on"),
       0);
    /* W356 AUDIO-2: Kopfhoerer = 3D-Klang (HRTF) wie die Konsole; aus = Lautsprecher (Stereo bzw. 5.1/7.1) */
    mk(w, K_TOGGLE, ID_HRTF, P_AUDIO, CX0 + 178, CY0 + 46, COLW - 196, 28, LT("gui.hrtf"), 0);
    { /* W303 AUDIO-VOLUME: 2x2 Listen 100..0 % (Gesamt | Musik / Effekte | Sprache/Funk), wirken bei SB und LoC */
        int k, j;
        for (k = 0; k < 4; k++) {
            c = mk(w, K_COMBO, kVolId[k], P_AUDIO, k & 1 ? CX0 + COLW - 18 - 72 : CX0 + 86, CY0 + 82 + (k >> 1) * 36,
                   72, 30, "", 0);
            for (j = 0; j < VOL_N; j++) {
                char t[16];
                _snprintf_s(t, sizeof t, _TRUNCATE, "%d %%", 100 - j * 5);
                combo_add(c, t);
            }
        }
    }
    /* W234 PAL50: Spieltempo (wirkt beim naechsten Start, kein ESC-Schalter); W290: rechte Spalte */
    mk(w, K_TOGGLE, ID_PAL50, P_AUDIO, COL2 + 18, CY0 + 50, COLW - 36, 28, LT("gui.pal50"), 0);
    /* --- W195 PHOTO-MODE: Extras (Fotoordner, Aufnahme-Aufloesung, Format); W303: EXY tiefer --- */
    mk(w, K_EDIT, ID_PH_DIR, P_AUDIO, CX0 + 18, CY0 + 208 + EXY, CW - 36 - 250, 30, "", 0);
    mk(w, K_BTN, ID_PH_BROWSE, P_AUDIO, CX0 + CW - 18 - 242, CY0 + 208 + EXY, 112, 30, LT("gui.btn_browse"), 0);
    mk(w, K_BTN, ID_PH_OPEN, P_AUDIO, CX0 + CW - 18 - 122, CY0 + 208 + EXY, 122, 30, LT("gui.extras_open"), 0);
    /* W290 QUICK-SHOT: Ansicht | Groesse, Format | Bildformat (nur Aussenwelt), Replays & Video-Export */
    c = mk(w, K_COMBO, ID_PH_VIEW, P_AUDIO, CX0 + 130, CY0 + 250 + EXY, 226, 30, "", 0);
    combo_add(c, LT("ovl.ph_v_game"));
    combo_add(c, LT("ovl.ph_v_flat"));
    combo_add(c, LT("ovl.ph_v_world"));
    combo_add(c, LT("ovl.ph_v_cine")); /* W428 PHOTO-CINEMATIC */
    c = mk(w, K_COMBO, ID_PH_RES, P_AUDIO, CX0 + 440, CY0 + 250 + EXY, CW - 18 - 440, 30, "", 0);
    combo_add(c, LT("gui.extras_res_1"));
    combo_add(c, "4K");
    combo_add(c, "8K");
    combo_add(c, LT("gui.extras_res_2"));
    combo_add(c, LT("gui.extras_res_4"));
    combo_add(c, LT("gui.extras_res_8"));
    c = mk(w, K_COMBO, ID_PH_FMT, P_AUDIO, CX0 + 130, CY0 + PHY_FMT + EXY, 104, 30, "", 0);
    combo_add(c, "PNG");
    combo_add(c, "JPG");
    c = mk(w, K_COMBO, ID_PH_ASP, P_AUDIO, CX0 + 440, CY0 + PHY_FMT + EXY, CW - 18 - 440, 30, "", 0);
    combo_add(c, LT("ovl.ph_asp_square"));
    combo_add(c, LT("ovl.ph_asp_wide"));
    c = mk(w, K_COMBO, ID_PH_CSELF, P_AUDIO, CX0 + 250, CY0 + PHY_CS + EXY, 104, 30, "", 0); /* W445 */
    cself_fill(c);
    mk(w, K_TOGGLE, ID_RP_LABEL, P_AUDIO, CX0 + 18, CY0 + PHY_T1 + EXY, COLW - 36, 28, LT("gui.rp_label"), 0);
    mk(w, K_TOGGLE, ID_RP_LOGO, P_AUDIO, COL2 + 18, CY0 + PHY_T1 + EXY, COLW - 36, 28, LT("gui.rp_logo"), 0);
    mk(w, K_TOGGLE, ID_WM_MEDIA, P_AUDIO, CX0 + 18, CY0 + PHY_T2 + EXY, COLW - 36, 28, LT("gui.wm_media"), 0);
    mk(w, K_TOGGLE, ID_PH_HUD, P_AUDIO, COL2 + 18, CY0 + PHY_T2 + EXY, COLW - 36, 28, LT("gui.ph_hud"), 0); /* W290 */

    /* --- Erweitert: Spieldateien (Ordner-Modus), Spielstaende, gespeicherte
     * Einstellungen, Feinabstimmung, Launcher, Diagnose --- */
    /* W172/P7: Spieldateien (Programmdatei optional), Spielstaende +
     * Datenort (Oeffnen), gespeicherte Einstellungen (Laden/Speichern als/
     * Loeschen + Schalter "beim Start laden") */
    mk(w, K_EDIT, ID_MEDIA, P_ADV, CX0 + 18, CY0 + 74, COLW - 36 - 120, 30, "", 0);
    mk(w, K_BTN, ID_MEDIA_B, P_ADV, CX0 + COLW - 18 - 112, CY0 + 74, 112, 30, LT("gui.btn_browse"), 0);
    mk(w, K_EDIT, ID_XBE, P_ADV, CX0 + 18, CY0 + 124, COLW - 36 - 120, 30, "", 0);
    mk(w, K_BTN, ID_XBE_B, P_ADV, CX0 + COLW - 18 - 112, CY0 + 124, 112, 30, LT("gui.btn_browse"), 0);
    /* W187 KIA-LOAD: im Kopf der Karte (wie "Anzeigen" bei Diagnose) */
    mk(w, K_BTN, ID_SAVES_RESET, P_ADV, CX0 + COLW - 18 - 130, CY0 + 196 + 9, 130, 28,
       LT("gui.saves_reset"), 0);
    mk(w, K_EDIT, ID_HDD, P_ADV, CX0 + 104, CY0 + 236, COLW - 104 - 18 - 90, 30, "", 0);
    mk(w, K_BTN, ID_HDD_OPEN, P_ADV, CX0 + COLW - 18 - 82, CY0 + 236, 82, 30,
       LT("gui.adv_open"), 0);
    mk(w, K_BTN, ID_DATA_OPEN, P_ADV, CX0 + COLW - 18 - 112, CY0 + 272, 112, 26,
       LT("gui.adv_open_folder"), 0);
    /* W254: drei Knoepfe (Import, Export, Replays) im Raster der Zeile darunter */
    mk(w, K_BTN, ID_SAVES_IMPORT, P_ADV, CX0 + 18, CY0 + 308, 96, 28, LT("gui.si_import"), 0);
    mk(w, K_BTN, ID_SAVES_EXPORT, P_ADV, CX0 + 122, CY0 + 308, 96, 28, LT("gui.si_export"), 0);
    mk(w, K_BTN, ID_SAVES_REPLAYS, P_ADV, CX0 + 226, CY0 + 308, COLW - 244, 28, LT("gui.rp_btn"), 0);
    mk(w, K_COMBOEDIT, ID_PROF_COMBO, P_ADV, CX0 + 18, CY0 + 388, COLW - 36, 30,
       "", 0);
    mk(w, K_ACCENT, ID_PROF_LOAD, P_ADV, CX0 + 18, CY0 + 420, 96, 30, LT("gui.adv_setup_load"), 0);
    mk(w, K_BTN, ID_PROF_SAVE, P_ADV, CX0 + 122, CY0 + 420, 96, 30, LT("gui.adv_setup_save"), 0);
    mk(w, K_BTN, ID_PROF_DEL, P_ADV, CX0 + 226, CY0 + 420, 96, 30, LT("gui.btn_delete"), 0);
    mk(w, K_TOGGLE, ID_PROF_DFLT, P_ADV, CX0 + 18, CY0 + 452, COLW - 36, 28,
       LT("gui.adv_setup_auto"), 0);
    c = mk(w, K_COMBO, ID_VSYNC, P_ADV, COL2 + 170, CY0 + 40, COLW - 188,
           30, "", 0);
    combo_add(c, LT("gui.off"));
    combo_add(c, LT("gui.on"));
    c = mk(w, K_COMBO, ID_PACING, P_ADV, COL2 + 170, CY0 + 108, COLW - 188, 30, "", 0); /* W468 PERF-PACING */
    combo_add(c, LT("gui.pacing_timer"));
    combo_add(c, LT("gui.pacing_monitor"));
    combo_add(c, LT("gui.pacing_vrr"));
    c = mk(w, K_COMBO, ID_FPS, P_ADV, COL2 + 170, CY0 + 74, COLW - 188,
           30, "", 0);
    combo_add(c, LT("gui.fps_off"));
    combo_add(c, "30");
    combo_add(c, "60");
    combo_add(c, "120");
    mk(w, K_TOGGLE, ID_BG, P_LSET, COL2 + 18, LSET_LANG_Y + 130 + 12 + 40, COLW - 36, 28, LT("gui.bg"), 0); /* W397 */
    mk(w, K_BTN, ID_DIAG, P_ADV, COL2 + COLW - 18 - 130, ADV_DIAG_Y + 9, 130, 28,
       LT(g_diag_open ? "gui.adv_diag_hide" : "gui.adv_diag_show"), 0);
    mk(w, K_TOGGLE, ID_LIVE, P_ADV, COL2 + 18, ADV_DIAG_Y + ADV_DIAG_DY + 164, COLW - 36, 28, LT("gui.ton_live"), 0);
    /* W250 UPDATE-CHECK: Schalter + "Jetzt pruefen" (Launcher-Karte), Erstabfrage
     * und Hinweis rechts neben den Startknoepfen (Spielen) */
    /* W397 LAUNCHER-STRUCT: Schalter/Erstabfrage ersetzt durch die drei Stufen (lset_build); "Jetzt pruefen"
     * auf den Launcher-Einstellungen, Hinweis "neue Version" bleibt auf SB-Spielen */
    mk(w, K_BTN, ID_UPD_NOW, P_LSET, CX0 + COLW - 18 - 130, CY0 + 332 - 42, 130, 28, LT("gui.upd_now"), 0);
    mk(w, K_BTN, ID_UPD_OPEN, P_PLAY, UPD_X + UPD_W - 18 - 160, UPD_Y + 58, 160, 28, LT("gui.upd_open"), 0);
    lset_build(w);
    /* W190 SAVE-LOCK: immer sichtbar (auch bei eingeklappter Diagnose) */
    mk(w, K_TOGGLE, ID_SAVE_LOCK, P_ADV, COL2 + 18, ADV_DIAG_Y + 40, COLW - 36, 28, LT("gui.save_lock"), 0);
    alpha_build_controls(w); /* W272 ALPHA-TEST */
    dg_build_controls(w); /* W279 DIAG-CRASH */

    /* --- Fussleiste: Rueckgaengig, Speichern, Launch v --- */
    mk(w, K_BTN, ID_UNDO, PG_ALL, WIN_W - 28 - 150 - 12 - 130 - 10 - 112,
       WIN_H - BOT_H + 14, 112, 36, LT("gui.undo"), 0);
    mk(w, K_BTN, ID_SAVE, PG_ALL, WIN_W - 28 - 150 - 12 - 130,
       WIN_H - BOT_H + 14, 130, 36, LT("gui.btn_save"), 0);
    mk(w, K_ACCENT, ID_START, PG_NOTPLAY, WIN_W - 28 - 150,
       WIN_H - BOT_H + 14, 118, 36, LT("gui.btn_start"), 0);
    mk(w, K_ACCENT, ID_START_MENU, PG_NOTPLAY, WIN_W - 28 - 30,
       WIN_H - BOT_H + 14, 30, 36, "", 0);
    loctab_build(w); /* W276: Spielauswahl + LoC-Seite */
    orig_build(w); /* W278 ORIG-FLAG */
    ui_set_checked(ID_SHOWADV, g_show_adv); /* W291 */
    lay_gfx_apply(); /* W291: Kartenlage der Grafik-Seite (Symbole aus orig_build) */
    keys_tabstops();
}

/* ------------------------------------------------------------------ */
