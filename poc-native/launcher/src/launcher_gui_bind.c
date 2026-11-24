/* launcher_gui_bind.c - Belegungs-Dialog (W78).
 * W462 LAUNCHER-SPLIT: aus launcher_gui.c verschoben (unveraendert), gemeinsamer Zustand
 * in launcher_gui_int.h. */
#include "launcher_gui_int.h"
/* Belegungs-Dialog (W78, jetzt W-API + dunkel)                        */
/* ------------------------------------------------------------------ */
#define IDB_CUR 502
#define IDB_SET 503
#define IDB_ADD 504
#define IDB_MX 505
#define IDB_MY 506
#define IDB_DEL 507
#define IDB_RST 508
#define IDB_PROMPT 509
#define IDB_CONF 510
#define IDB_SAVE 512
#define IDB_CHECK 513
#define IDB_CLOSE 515
#define IDB_TITLE 516
#define IDB_JLBL 517   /* W221: Joystick-Achse Totzone/Invertierung */
#define IDB_JDZ 518
#define IDB_JINV 519
#define IDB_JLIVE 521  /* W226: Live Roh -> Ergebnis */
/* W228 BIND-DIALOG: Detailbereich + Fusszeile */
#define IDB_DNAME 522  /* Ueberschrift = Anzeigename der Aktion */
#define IDB_SRC 523    /* Quellen der Aktion, eine Zeile je Quelle */
#define IDB_SRCL 524
#define IDB_JHINT 525  /* Achse ohne JOY-Quelle: Hinweis */
#define IDB_CONFL 526  /* Konflikte: Ueberschrift bzw. "Keine Konflikte" */
#define IDB_FSTAT 527  /* Fusszeile: Speichern/Pruefen */
#define IDB_NAMEL 528
#define IDB_INI 529
#define IDB_JDZL 530
#define IDB_JNOW 531
#define IDB_SMODE 532  /* W271: Stick-Modus der Zielhebel-Zeile (global/Position/wie Maus) */
/* Rahmen (logische Punkte, SX skaliert) */
#define BD_W 860
#define BD_H 680
#define BD_LX 16   /* Liste */
#define BD_LW 340
#define BD_TOP 40
#define BD_BOT 594
#define BD_CX 368  /* Detail-Karte */
#define BD_CW 476
#define BD_IX 384  /* Karte innen */
#define BD_IW 444
#define BD_FSEP 606 /* Trennlinie Fusszeile */
#define BD_ROW_H 24

lctrl_bind_t g_bind, g_bind_base;
static lctrl_bind_t g_bind_clean; /* W228: Stand beim Oeffnen/Speichern */
static int g_bind_nconf, g_bind_lay = -1, g_bind_src_reset, g_bind_joyset;
int g_bind_lb_prev = -1;
static char g_bind_base_name[64];
static char g_bind_saved[64];
/* W282 ISSUE6 Selbsttest: einmal nach dem Aufbau des (offscreen)
 * Belegungsdialogs aufgerufen, danach schliesst der Dialog sofort. */
void (*g_bind_testfn)(HWND hd);
int g_bind_sel = -1;
int g_cap_mode; /* 0 = idle, 1 = ersetzen, 2 = hinzu */
static int g_cap_mod;  /* W283: gedrueckte Zusatztaste (Zeile orig_view), 0 = keine */
lctrl_pad_t g_cap_prev;
/* W365 BIND-COMBO: wartende erste Taste je Quelle (Pad, Original-Controller, Joystick). */
static lctrl_combo_t g_cap_pcombo, g_cap_scombo;
static volatile LONG g_cap_sbc_held[3]; /* Tastenstand aus dem SBC-Thread (buttons[0..2]) */
static int g_cap_jp_dev = -1;           /* Joystick-Geraet der wartenden Taste, -1 = keine */
static char g_cap_jp_part[16];          /* "B3" */
static int g_cap_have_pad;
lctrl_pad_learner_t g_cap_plrn; /* W312: Pad-Achsen ab Ruhestellung */
void joy_cal_load(void);        /* W312: Kalibrierung aus user.cfg */
static int g_bind_done;
static HWND g_bind_hd;
/* W159: Original-Controller als Quelle (eigener Thread: sbc_usb_poll
 * blockiert bis 100 ms, der UI-Thread darf nie warten). */
#define WM_APP_BIND_SBC (WM_APP + 8) /* w = bi 0..38 (Flanke) */
static HANDLE g_cap_sbc_thr;
static volatile LONG g_cap_sbc_stop;
/* W221 HOTAS: Joystick-Lernmodus (Ruhestellung beim Start, letzter Poll). */
static int g_cap_jn;
static ijoy_info_t g_cap_jl[IJOY_MAX_DEV];
/* W226: Lernen ueber mehrere Abtastungen (Basis einschwingen, Spitze). */
static lctrl_joy_learner_t g_cap_jlrn[IJOY_MAX_DEV];
/* W226: Live-Zeile im Belegungs-Dialog (Roh -> Ergebnis, Timer 10). */
static int g_live_scanned, g_live_n, g_bind_joydemo;
static ijoy_info_t g_live_jl[IJOY_MAX_DEV];

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
        for (i = 0; i < 3; i++) { /* W365: Stand fuer Kombi-Lernen (vor der Meldung) */
            InterlockedExchange(&g_cap_sbc_held[i], (LONG)st.buttons[i]);
        }
        if (bi >= 0) {
            PostMessageW(hd, WM_APP_BIND_SBC, (WPARAM)bi, 0);
        }
    }
    sbc_usb_close(dev);
    return 0;
}

HWND bctl(HWND hd, int id)
{
    return GetDlgItem(hd, id);
}

/* W228: Ergebniszeile im Detailbereich; leer = ausgeblendet (keine Luecke). */
static int g_bind_res_on;
static void bind_layout(HWND hd);
static void bind_set_result(HWND hd, const char *u8)
{
    int on = u8 && *u8;
    ui_settext(bctl(hd, IDB_RESULT), u8 ? u8 : "");
    if (on != g_bind_res_on) {
        g_bind_res_on = on;
        bind_layout(hd);
    }
}

/* W271 STICK-RELATIVE: Knopf "Stick: ..." der Zielhebel-Zeile beschriften. */
static void bind_smode_text(HWND hd)
{
    static const char *const kK[3] = { "gui.bind_smode0", "gui.bind_smode1", "gui.bind_smode2" };
    int m = g_bind_sel >= 0 ? lctrl_stick_mode_get(&g_bind, g_bind_sel) : -1;
    ui_settext(bctl(hd, IDB_SMODE), LT(kK[m >= 0 && m <= 2 ? m : 0]));
    EnableWindow(bctl(hd, IDB_SMODE), m >= 0 && !g_cap_mode);
}

static void bind_refresh(HWND hd, int keep_joy);
/* W271: Klick schaltet global -> Position -> wie eine Maus -> global. */
static void bind_smode_cycle(HWND hd)
{
    char res[256], line[400];
    int m;
    if (g_bind_sel < 0) {
        return;
    }
    m = lctrl_stick_mode_get(&g_bind, g_bind_sel);
    if (m < 0 || lctrl_stick_mode_set(&g_bind, g_bind_sel, (m + 1) % 3) != 0) {
        return;
    }
    lctrl_bind_row_text(&g_bind, g_bind_sel, res, sizeof res);
    _snprintf_s(line, sizeof line, _TRUNCATE, "%s = %s.", kCtrlElems[g_bind_sel].key, res);
    bind_set_result(hd, line);
    bind_refresh(hd, 1);
    bind_smode_text(hd);
}

/* W221 HOTAS: Totzone/Invertierung der JOY-Achsenquellen der gewaehlten
 * Belegung anzeigen (erste JOY-Achse gibt die Werte vor). */
static void bind_joy_row(HWND hd)
{
    int a, has = 0, inv = 0, en;
    char dz[16] = "0";
    uictl_t *u = ui_by_hwnd(bctl(hd, IDB_JINV));
    if (g_bind_sel >= 0 && g_bind_sel < LCTRL_ELEM_N) {
        for (a = 0; a < g_bind.row[g_bind_sel].n_alt && !has; a++) {
            const char *alt = g_bind.row[g_bind_sel].alt[a];
            int k = lctrl_joy_src_axis(alt, NULL, NULL); /* W226 */
            if (k) {
                char v[16];
                has = k;
                if (k == 2) {
                    dz[0] = '\0'; /* Achse als Taste: keine Totzone */
                } else if (!lctrl_joy_param_get(alt, "dz", dz, sizeof dz)) {
                    strcpy_s(dz, sizeof dz, "0");
                }
                inv = lctrl_joy_param_get(alt, "inv", v, sizeof v) &&
                      atoi(v) != 0;
            }
        }
    }
    en = has && !g_cap_mode;
    g_bind_joyset = 1; /* W228: EN_CHANGE beim Setzen nicht uebernehmen */
    ui_settext(bctl(hd, IDB_JDZ), has ? dz : "");
    g_bind_joyset = 0;
    if (u) {
        u->checked = inv;
        InvalidateRect(u->h, NULL, FALSE);
    }
    EnableWindow(bctl(hd, IDB_JDZ), en && has == 1);
    EnableWindow(bctl(hd, IDB_JINV), en);
    bind_smode_text(hd); /* W271 */
}

static void bind_refresh_cur(HWND hd);
static void bind_refresh(HWND hd, int keep_joy);
static void bind_refresh_conf(HWND hd);
void list_add(HWND lb, const char *u8, LPARAM data, int with_data);

/* W226: Live-Zeile "Roh -> Ergebnis" fuer die erste JOY-Achsenquelle der
 * gewaehlten Belegung (Timer 10, 100 ms; Geraete einmal bei Bedarf suchen,
 * im Shot-Modus Beispielwert ohne DirectInput). */
static void bind_joy_live(HWND hd)
{
    char t[200], r[48];
    const char *alt = NULL;
    int a, slot = 0, ax = 0, kind = 0, raw = 0, have = 0, pedal;
    float v = 0.0f;
    if (g_bind_sel < 0 || g_bind_sel >= LCTRL_ELEM_N) {
        ui_settext(bctl(hd, IDB_JLIVE), "");
        return;
    }
    for (a = 0; a < g_bind.row[g_bind_sel].n_alt && !kind; a++) {
        alt = g_bind.row[g_bind_sel].alt[a];
        kind = lctrl_joy_src_axis(alt, &slot, &ax);
    }
    if (!kind) {
        ui_settext(bctl(hd, IDB_JLIVE), "");
        return;
    }
    if (g_bind_joydemo) {
        raw = 32767; /* Beispiel: umgekehrter Schubhebel in Ruhe */
        have = 1;
    } else if (!g_o.shot) {
        int dev;
        imap_joy_state_t st;
        if (!g_live_scanned) {
            int i, n;
            g_live_scanned = 1;
            g_live_n = 0;
            if (ijoy_init() == 0) {
                n = ijoy_scan();
                for (i = 0; i < n && i < IJOY_MAX_DEV; i++) {
                    const ijoy_info_t *in = ijoy_info(i);
                    if (!in) {
                        break;
                    }
                    g_live_jl[i] = *in;
                    g_live_n = i + 1;
                }
            }
        }
        dev = ijoy_match_list(g_live_jl, g_live_n, g_bind.joy[slot], slot);
        if (dev >= 0 && ijoy_read(dev, &st)) {
            raw = st.axis[ax];
            have = 1;
        }
    }
    if (!have) {
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.joy_live_none"), slot + 1);
        ui_settext(bctl(hd, IDB_JLIVE), t);
        return;
    }
    pedal = strncmp(kCtrlElems[g_bind_sel].key, "pedal", 5) == 0;
    kind = lctrl_joy_eval(alt, pedal, raw, &v);
    if (kind == 2) {
        strncpy_s(r, sizeof r, LT(v > 0.5f ? "gui.joy_live_on" : "gui.joy_live_off"), _TRUNCATE);
    } else if (pedal) {
        _snprintf_s(r, sizeof r, _TRUNCATE, "%.0f %%", v * 100.0f);
    } else {
        _snprintf_s(r, sizeof r, _TRUNCATE, "%+.0f %%", v * 100.0f);
    }
    _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.joy_live_val"), slot + 1,
                ijoy_axis_name(ax), raw, r);
    ui_settext(bctl(hd, IDB_JLIVE), t);
}

/* W221: Totzone (0..0.9) + Invertierung in alle JOY-Achsenquellen der
 * gewaehlten Belegung schreiben (dz=0 entfernt den Parameter). W228: wirkt
 * sofort (Eingabe/Schalter), das Eingabefeld bleibt dabei unangetastet. */
static void bind_joy_apply(HWND hd)
{
    char t[32], dz[16], res[256];
    double v;
    int a, inv;
    uictl_t *u = ui_by_hwnd(bctl(hd, IDB_JINV));
    if (g_bind_sel < 0 || g_bind_sel >= LCTRL_ELEM_N) {
        return;
    }
    ui_gettext(bctl(hd, IDB_JDZ), t, sizeof t);
    trim_u8(t);
    {
        char *c = strchr(t, ','); /* deutsches Komma erlauben */
        if (c) {
            *c = '.';
        }
    }
    v = t[0] ? atof(t) : 0.0;
    if (v < 0.0 || v > 0.9) {
        bind_set_result(hd, LT("gui.joy_baddz"));
        return;
    }
    _snprintf_s(dz, sizeof dz, _TRUNCATE, "%.2f", v);
    inv = u ? u->checked : 0;
    for (a = 0; a < g_bind.row[g_bind_sel].n_alt; a++) {
        char *alt = g_bind.row[g_bind_sel].alt[a];
        int k = lctrl_joy_src_axis(alt, NULL, NULL); /* W226 */
        if (k == 1) {
            lctrl_joy_param_set(alt, sizeof g_bind.row[0].alt[0], "dz",
                                v > 0.0 ? dz : NULL);
        }
        if (k) { /* Invertieren auch fuer Z+/Z-/Zone (Weg gespiegelt) */
            lctrl_joy_param_set(alt, sizeof g_bind.row[0].alt[0], "inv",
                                inv ? "1" : NULL);
        }
    }
    lctrl_bind_row_text(&g_bind, g_bind_sel, res, sizeof res);
    {
        char line[400];
        _snprintf_s(line, sizeof line, _TRUNCATE, "%s = %s.",
                    kCtrlElems[g_bind_sel].key, res);
        bind_set_result(hd, line);
    }
    bind_refresh(hd, 1);
}

/* W228: Art der Aktion + internes Kuerzel (Zeile unter der Ueberschrift). */
static void bind_kind_text(int idx, char *out, size_t cap)
{
    const lctrl_elem_t *e = &kCtrlElems[idx];
    int ax = lctrl_helper_axis(idx);
    if (e->kind == LCTRL_ACT) {
        _snprintf_s(out, cap, _TRUNCATE, LT("gui.bind_kind_act"), e->key);
    } else if (e->kind == LCTRL_AXIS) {
        _snprintf_s(out, cap, _TRUNCATE, LT("gui.bind_kind_axis"), e->key);
    } else if (ax >= 0) {
        _snprintf_s(out, cap, _TRUNCATE, LT("gui.bind_kind_helper"), e->key,
                    LT(kCtrlElems[ax].lkey), LT(kCtrlElems[ax].lkey));
    } else {
        _snprintf_s(out, cap, _TRUNCATE, LT("gui.bind_kind_btn"), e->key);
    }
}

/* W228: Quellen der gewaehlten Aktion, eine Zeile je Quelle (Daten = Pos.). */
/* W260 MOUSE-WHEEL: Mausrad-Quellen und ihre Anzeigenamen (EN/DE). */
static const struct { const char *src, *lkey; } kWheelSrc[4] = {
    { "MOUSE:WHEEL_UP", "gui.svg_m_wup" }, { "MOUSE:WHEEL_DOWN", "gui.svg_m_wdown" },
    { "MOUSE:WHEEL_LEFT", "gui.svg_m_wleft" }, { "MOUSE:WHEEL_RIGHT", "gui.svg_m_wright" },
};

/* Lernmodus: Rad-Nachricht -> Quelle (NULL = keine Rasten-Richtung). */
const char *bind_wheel_src(UINT msg, WPARAM wp)
{
    int d = GET_WHEEL_DELTA_WPARAM(wp);
    if (d == 0 || (msg != WM_MOUSEWHEEL && msg != WM_MOUSEHWHEEL)) {
        return NULL;
    }
    if (msg == WM_MOUSEWHEEL) {
        return kWheelSrc[d > 0 ? 0 : 1].src;
    }
    return kWheelSrc[d > 0 ? 3 : 2].src;
}

/* Quellenzeile fuer die Liste: Rohtext, bei Mausrad mit Klartext dahinter
 * ("MOUSE:WHEEL_UP  (Mouse wheel up)"). */
void bind_src_disp(const char *alt, char *out, size_t cap)
{
    int i, n = 0;
    char pr[160];
    strncpy_s(out, cap, alt, _TRUNCATE);
    if (lctrl_src_pretty(alt, pr, sizeof pr)) { /* W365: Tastennamen / Kombination lesbar */
        strncat_s(out, cap, "  (", _TRUNCATE);
        strncat_s(out, cap, pr, _TRUNCATE);
        n = 1;
    }
    for (i = 0; i < 4; i++) {
        const char *hit = strstr(alt, kWheelSrc[i].src);
        size_t L = strlen(kWheelSrc[i].src);
        if (!hit) {
            char up[160];
            size_t k;
            for (k = 0; alt[k] && k + 1 < sizeof up; k++) {
                up[k] = (char)toupper((unsigned char)alt[k]);
            }
            up[k] = '\0';
            hit = strstr(up, kWheelSrc[i].src);
        }
        if (hit && (hit[L] == '\0' || hit[L] == '+' || hit[L] == ' ' || hit[L] == ':')) {
            strncat_s(out, cap, n++ ? ", " : "  (", _TRUNCATE);
            strncat_s(out, cap, LT(kWheelSrc[i].lkey), _TRUNCATE);
        }
    }
    if (n) {
        strncat_s(out, cap, ")", _TRUNCATE);
    }
}

static void bind_src_fill(HWND hd)
{
    HWND lb = bctl(hd, IDB_SRC);
    int keep = ListBox_GetCurSel(lb), a, n = 0;
    if (g_bind_src_reset) {
        keep = 0;
        g_bind_src_reset = 0;
    }
    SendMessageW(lb, WM_SETREDRAW, FALSE, 0);
    ListBox_ResetContent(lb);
    if (g_bind_sel >= 0 && g_bind_sel < LCTRL_ELEM_N) {
        n = g_bind.row[g_bind_sel].n_alt;
    }
    for (a = 0; a < n; a++) {
        char disp[256];
        bind_src_disp(g_bind.row[g_bind_sel].alt[a], disp, sizeof disp); /* W260 */
        list_add(lb, disp, (LPARAM)a, 1);
    }
    if (n == 0) {
        list_add(lb, LT(g_bind_sel >= 0 ? "gui.bind_nosrc" : "gui.bind_noelem"),
                 (LPARAM)-1, 1);
    } else {
        keep = keep < 0 ? 0 : (keep >= n ? n - 1 : keep);
        ListBox_SetCurSel(lb, keep);
    }
    SendMessageW(lb, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lb, NULL, TRUE);
}

/* W228: gewaehlte Quelle (Position) oder -1. */
static int bind_src_sel(HWND hd)
{
    HWND lb = bctl(hd, IDB_SRC);
    int at = ListBox_GetCurSel(lb);
    int a = at >= 0 ? (int)ListBox_GetItemData(lb, at) : -1;
    if (g_bind_sel < 0 || a < 0 || a >= g_bind.row[g_bind_sel].n_alt) {
        return -1;
    }
    return a;
}

static void bind_enable(HWND hd)
{
    unsigned vis = g_bind_sel >= 0 ? lctrl_bind_detail_vis(&g_bind, g_bind_sel) : 0;
    int en = !g_cap_mode, act = en && g_bind_sel >= 0;
    EnableWindow(bctl(hd, IDB_SET), act);
    EnableWindow(bctl(hd, IDB_ADD), act);
    EnableWindow(bctl(hd, IDB_MX), act && (vis & LCTRL_VIS_MOUSE));
    EnableWindow(bctl(hd, IDB_MY), act && (vis & LCTRL_VIS_MOUSE));
    EnableWindow(bctl(hd, IDB_DEL), act && bind_src_sel(hd) >= 0);
    EnableWindow(bctl(hd, IDB_RST), act);
    EnableWindow(bctl(hd, IDB_LIST), en);
    EnableWindow(bctl(hd, IDB_SRC), en);
    EnableWindow(bctl(hd, IDB_NAME), en);
    EnableWindow(bctl(hd, IDB_SAVE), en);
    EnableWindow(bctl(hd, IDB_CHECK), en);
}

static void bind_refresh_cur(HWND hd)
{
    bind_refresh(hd, 0);
}

/* W228: Detailbereich der gewaehlten Aktion neu fuellen (keep_joy = das
 * Totzonen-Feld nicht ueberschreiben, z. B. waehrend der Eingabe). */
static void bind_refresh(HWND hd, int keep_joy)
{
    char t[600];
    HWND lb = bctl(hd, IDB_LIST);
    int sel = ListBox_GetCurSel(lb), prev = g_bind_sel;
    g_bind_sel = (sel >= 0) ? (int)ListBox_GetItemData(lb, sel) : -1;
    if (g_bind_sel < 0 || g_bind_sel >= LCTRL_ELEM_N) {
        g_bind_sel = -1;
    }
    if (g_bind_sel != prev) {
        g_bind_src_reset = 1;
    }
    if (g_bind_sel < 0) {
        ui_settext(bctl(hd, IDB_DNAME), LT("gui.bind_noelem"));
        ui_settext(bctl(hd, IDB_CUR), "");
    } else {
        ui_settext(bctl(hd, IDB_DNAME), LT(kCtrlElems[g_bind_sel].lkey));
        bind_kind_text(g_bind_sel, t, sizeof t);
        ui_settext(bctl(hd, IDB_CUR), t);
    }
    bind_src_fill(hd);
    InvalidateRect(lb, NULL, FALSE); /* #75: Tasten in der Liste aktuell halten */
    if (!keep_joy) {
        bind_joy_row(hd);
    }
    bind_joy_live(hd); /* W226 */
    bind_enable(hd);
    bind_refresh_conf(hd);
}

void list_add(HWND lb, const char *u8, LPARAM data, int with_data)
{
    wchar_t *w = u8_wdup(u8);
    int at = (int)SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)(w ? w : L""));
    free(w);
    if (with_data && at >= 0) {
        SendMessageW(lb, LB_SETITEMDATA, (WPARAM)at, data);
    }
}

/* W228: Element verschieben (logisch) und zeigen bzw. verbergen. */
static void bplace(HWND hd, int id, int x, int y, int w, int h, int show)
{
    uictl_t *u = ui_by_hwnd(bctl(hd, id));
    if (!u) {
        return;
    }
    if (show) {
        u->x = x;
        u->y = y;
        u->w = w;
        u->h_ = h;
        ui_place(u);
        if (id == IDB_DNAME || id == IDB_JLBL || (id == IDB_CONFL && g_bind_nconf > 0)) {
            SendMessageW(u->h, WM_SETFONT, (WPARAM)g_f[F_SEMI], FALSE);
        }
    }
    ShowWindow(u->h, show ? SW_SHOWNA : SW_HIDE);
}

/* W228: Detailbereich je Aktionsart anordnen (nur Sichtbares, ohne Luecken):
 * Ueberschrift, Art, Quellen, Belegen/Hinzufuegen, Entfernen/Standard,
 * [Maus X/Y], Hinweis/Lernmodus, Ergebnis, [Joystick], Konflikte. */
static void bind_layout(HWND hd)
{
    unsigned vis = g_bind_sel >= 0 ? lctrl_bind_detail_vis(&g_bind, g_bind_sel) : 0;
    int sig = (int)vis | (g_bind_nconf > 0 ? 0x100 : 0) | (g_bind_sel >= 0 ? 0x200 : 0) |
              (g_bind_res_on ? 0x400 : 0);
    int tall = g_bind_sel >= 0 && lctrl_helper_axis(g_bind_sel) >= 0; /* 3 Zeilen */
    int dy = tall ? 16 : 0;
    int x = BD_IX, w = BD_IW, half = (BD_IW - 8) / 2, y, joy, hint, ch;
    sig |= tall ? 0x800 : 0;
    if (sig == g_bind_lay) {
        return;
    }
    g_bind_lay = sig;
    joy = (vis & LCTRL_VIS_JOY) != 0;
    hint = (vis & LCTRL_VIS_JOYHINT) != 0;
    bplace(hd, IDB_DNAME, x, 52, w, 24, 1);
    bplace(hd, IDB_CUR, x, 78, w, 34 + dy, 1); /* Tasten-Helfer: 3 Zeilen */
    bplace(hd, IDB_SRCL, x, 116 + dy, w, 18, 1);
    bplace(hd, IDB_SRC, x, 136 + dy, w, 72, 1);
    bplace(hd, IDB_SET, x, 216 + dy, half, 30, 1);
    bplace(hd, IDB_ADD, x + half + 8, 216 + dy, half, 30, 1);
    bplace(hd, IDB_DEL, x, 252 + dy, half, 30, 1);
    bplace(hd, IDB_RST, x + half + 8, 252 + dy, half, 30, 1);
    y = 290 + dy;
    bplace(hd, IDB_MX, x, y, half, 30, (vis & LCTRL_VIS_MOUSE) != 0);
    bplace(hd, IDB_MY, x + half + 8, y, half, 30, (vis & LCTRL_VIS_MOUSE) != 0);
    if (vis & LCTRL_VIS_MOUSE) {
        y += 38;
    }
    bplace(hd, IDB_SMODE, x, y, w, 30, (vis & LCTRL_VIS_SMODE) != 0); /* W271 */
    if (vis & LCTRL_VIS_SMODE) {
        y += 38;
    }
    bplace(hd, IDB_PROMPT, x, y, w, 40, 1);
    y += 44;
    bplace(hd, IDB_RESULT, x, y, w, 34, g_bind_res_on);
    if (g_bind_res_on) {
        y += 40;
    }
    bplace(hd, IDB_JLBL, x, y, w, 20, joy || hint);
    if (joy || hint) {
        y += 24;
    }
    bplace(hd, IDB_JDZL, x, y + 5, 70, 18, joy);
    bplace(hd, IDB_JDZ, x + 74, y, 64, 28, joy);
    bplace(hd, IDB_JINV, x + 150, y, 130, 28, joy);
    bplace(hd, IDB_JNOW, x + 288, y + 5, w - 288, 18, joy);
    bplace(hd, IDB_JLIVE, x, y + 34, w, 18, joy);
    bplace(hd, IDB_JHINT, x, y, w, 34, hint);
    if (joy) {
        y += 60;
    } else if (hint) {
        y += 40;
    }
    bplace(hd, IDB_CONFL, x, y, w, 20, g_bind_sel >= 0);
    ch = BD_BOT - 8 - (y + 24);
    bplace(hd, IDB_CONF, x, y + 24, w, ch < 64 ? ch : 64, g_bind_nconf > 0);
    RedrawWindow(hd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

/* W228: Konflikte nur der gewaehlten Aktion ("Quelle - auch auf: ..."). */
static void bind_refresh_conf(HWND hd)
{
    lctrl_conf1_t cf[LCTRL_ALT_N];
    HWND lb = bctl(hd, IDB_CONF);
    int n = 0, i, j;
    ListBox_ResetContent(lb);
    if (g_bind_sel >= 0) {
        n = lctrl_bind_conflicts_for(&g_bind, g_bind_sel, cf, LCTRL_ALT_N);
    }
    for (i = 0; i < n; i++) {
        char who[400], t[560];
        size_t wl = 0;
        who[0] = '\0';
        for (j = 0; j < cf[i].n; j++) {
            int r = _snprintf_s(who + wl, sizeof who - wl, _TRUNCATE, "%s%s",
                                j ? ", " : "", LT(kCtrlElems[cf[i].other[j]].lkey));
            if (r < 0) {
                break;
            }
            wl += (size_t)r;
        }
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.bind_conf_row"),
                    g_bind.row[g_bind_sel].alt[cf[i].alt], who);
        list_add(lb, t, 0, 0);
    }
    if (g_bind_nconf != n) {
        InvalidateRect(bctl(hd, IDB_CONFL), NULL, TRUE); /* Farbe */
    }
    g_bind_nconf = n;
    ui_settext(bctl(hd, IDB_CONFL),
               LT(n ? "gui.bind_conf_hdr" : "gui.bind_conf_none"));
    bind_layout(hd);
}

/* W228: Zeile der gegliederten Liste (Daten >= 0 = Aktion, <= -100 = Gruppe):
 * Anzeigename vorn, Art-Marke (Achse / Taste fuer Achse), Kuerzel grau. */
static const char *const kBindGrpKey[LCTRL_GROUP_N] = {
    "gui.bind_grp0", "gui.bind_grp1", "gui.bind_grp2",
    "gui.bind_grp3", "gui.bind_grp4", "gui.bind_grp5"
};

static void bind_draw_row(const DRAWITEMSTRUCT *d)
{
    canvas_t c;
    int w = d->rcItem.right - d->rcItem.left, h = d->rcItem.bottom - d->rcItem.top;
    int i = (int)d->itemData, sel = (d->itemState & ODS_SELECTED) != 0;
    int dis = !IsWindowEnabled(d->hwndItem);
    if ((int)d->itemID < 0 || w <= 0 || h <= 0) {
        return;
    }
    memset(&c, 0, sizeof c);
    if (!cv_make(&c, w, h)) {
        return;
    }
    GdiFlush();
    cv_fill(&c, 0, 0, w, h, C_INPUT, 255, 0);
    if (i <= -100 && -(i + 100) < LCTRL_GROUP_N) {
        cv_text(&c, g_f[F_SEMI], LT(kBindGrpKey[-(i + 100)]), SX(8), 0,
                w - SX(16), h - SX(3), dis ? C_FAINT : C_ACCENT,
                DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS);
        cv_fill(&c, SX(8), h - 1, w - SX(16), 1, C_ACCENT, 80, 0);
    } else if (i >= 0 && i < LCTRL_ELEM_N) {
        const lctrl_elem_t *e = &kCtrlElems[i];
        /* #75: rechts die belegten Tasten/Knoepfe (lesbar), nicht die interne Kennung */
        int cw = (w - SX(24)) * 2 / 5, cx = w - SX(6) - cw, nx = SX(18), nr = cx - SX(4);
        char keys[200];
        int unset = g_bind.row[i].n_alt == 0, a;
        keys[0] = '\0';
        if (unset) {
            strncpy_s(keys, sizeof keys, LT("gui.bind_unset"), _TRUNCATE);
        }
        for (a = 0; a < g_bind.row[i].n_alt; a++) {
            char pr[160];
            if (!lctrl_src_pretty(g_bind.row[i].alt[a], pr, sizeof pr)) {
                const char *s = g_bind.row[i].alt[a];
                size_t k, o = 0;
                int word = 1;
                if (_strnicmp(s, "KEY:", 4) == 0) {
                    s += 4; /* KEY:SPACE -> Space */
                } else if (_strnicmp(s, "MOUSE:", 6) == 0) {
                    strcpy_s(pr, sizeof pr, "Mouse ");
                    o = 6;
                    s += 6; /* MOUSE:LEFT -> Mouse Left */
                } else {
                    word = -1; /* Pad/Joystick: wie im Profil */
                }
                for (k = 0; s[k] && o + 1 < sizeof pr; k++) {
                    char ch = s[k];
                    if (word >= 0) {
                        ch = word ? (char)toupper((unsigned char)ch) : (char)tolower((unsigned char)ch);
                        word = !isalnum((unsigned char)ch);
                    }
                    pr[o++] = ch;
                }
                pr[o] = '\0';
            }
            if (a) {
                strncat_s(keys, sizeof keys, ", ", _TRUNCATE);
            }
            strncat_s(keys, sizeof keys, pr, _TRUNCATE);
        }
        if (sel) {
            cv_fill(&c, SX(2), SX(1), w - SX(4), h - SX(2), RGB(255, 255, 255),
                    dis ? 10 : 20, SX(4));
            cv_fill(&c, SX(2), SX(5), SX(3), h - SX(10), C_ACCENT, 255, SX(1));
        }
        if (e->kind == LCTRL_AXIS || e->kind == LCTRL_NEG || e->kind == LCTRL_POS) {
            int ax = e->kind == LCTRL_AXIS, tw = SX(70), th = SX(16);
            int tx = cx - SX(4) - tw, ty = (h - th) / 2;
            cv_fill(&c, tx, ty, tw, th, ax && !dis ? C_ACCENT : RGB(255, 255, 255),
                    ax && !dis ? 46 : 12, th / 2);
            cv_text(&c, g_f[F_TINY], LT(ax ? "gui.bind_tag_axis" : "gui.bind_tag_key"),
                    tx, ty, tw, th, dis ? C_FAINT : (ax ? C_ACCENT_HI : C_DIM),
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            nr = tx - SX(6);
        }
        cv_text(&c, g_f[F_SMALL], LT(e->lkey), nx, 0, nr - nx, h,
                dis ? C_FAINT : C_TEXT,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        cv_text(&c, g_f[F_TINY], keys, cx, 0, cw, h,
                (unset || dis) ? C_FAINT : C_DIM,
                DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if ((d->itemState & ODS_FOCUS) && !(d->itemState & ODS_NOFOCUSRECT)) {
            cv_ring(&c, SX(1), 0, w - SX(2), h, C_ACCENT, 140, SX(4), 1);
        }
    }
    GdiFlush();
    BitBlt(d->hDC, d->rcItem.left, d->rcItem.top, w, h, c.dc, 0, 0, SRCCOPY);
    cv_free(&c);
}

/* W228: Gruppenzeilen sind nicht waehlbar - in Bewegungsrichtung weiter. */
static void bind_list_skip_header(HWND hd)
{
    HWND lb = bctl(hd, IDB_LIST);
    int n = ListBox_GetCount(lb), at = ListBox_GetCurSel(lb), dir, k;
    if (at < 0 || (int)ListBox_GetItemData(lb, at) > -100) {
        g_bind_lb_prev = at;
        return;
    }
    dir = (g_bind_lb_prev >= 0 && at < g_bind_lb_prev) ? -1 : 1;
    for (k = at + dir; k >= 0 && k < n; k += dir) {
        if ((int)ListBox_GetItemData(lb, k) > -100) {
            break;
        }
    }
    if (k < 0 || k >= n) { /* Rand: andere Richtung */
        for (k = at - dir; k >= 0 && k < n; k -= dir) {
            if ((int)ListBox_GetItemData(lb, k) > -100) {
                break;
            }
        }
    }
    if (k >= 0 && k < n) {
        ListBox_SetCurSel(lb, k);
        if (dir < 0 && k > 0 && ListBox_GetTopIndex(lb) > k - 1) {
            ListBox_SetTopIndex(lb, k - 1); /* Gruppenkopf mit zeigen */
        }
    }
    g_bind_lb_prev = ListBox_GetCurSel(lb);
}

/* W228: Belegung gleich (Zeilen + Geraete)? Fuer "ungespeichert". */
static int bind_same(const lctrl_bind_t *a, const lctrl_bind_t *b)
{
    int i, j;
    for (i = 0; i < LCTRL_ELEM_N; i++) {
        if (a->row[i].n_alt != b->row[i].n_alt) {
            return 0;
        }
        for (j = 0; j < a->row[i].n_alt; j++) {
            if (strcmp(a->row[i].alt[j], b->row[i].alt[j]) != 0) {
                return 0;
            }
        }
    }
    for (i = 0; i < IMAP_JOY_MAX; i++) {
        if (strcmp(a->joy[i], b->joy[i]) != 0) {
            return 0;
        }
    }
    return 1;
}

void bind_end_capture(HWND hd, const char *prompt)
{
    g_cap_mode = 0;
    g_cap_mod = 0;
    KillTimer(hd, 7);
    if (g_cap_sbc_thr) { /* W159: SBC-Thread stoppen (max. 1 Poll ~100 ms) */
        InterlockedExchange(&g_cap_sbc_stop, 1);
        WaitForSingleObject(g_cap_sbc_thr, 500);
        CloseHandle(g_cap_sbc_thr);
        g_cap_sbc_thr = NULL;
    }
    ui_settext(bctl(hd, IDB_PROMPT),
         prompt ? prompt : LT("gui.bind_idle"));
    InvalidateRect(bctl(hd, IDB_PROMPT), NULL, TRUE); /* W228: Farbe */
    ui_settext(bctl(hd, IDB_CLOSE), LT("gui.btn_close"));
    bind_refresh_cur(hd);
}

/* W365 BIND-COMBO: Lern-Hinweis je Zeile (Tastatur-Kombi bzw. nur Pad-/Controller-Kombi). */
static const char *bind_combo_hint(int idx)
{
    if (lctrl_bind_key_combo_row(idx)) {
        return LT("gui.bind_combo_hint");
    }
    if (lctrl_bind_combo_row(idx)) {
        return LT("gui.bind_combo_hint_pad");
    }
    return NULL;
}

/* W221 HOTAS: Joysticks neu aufzaehlen, Ruhestellung merken (Achsen-
 * Auslenkung zaehlt relativ dazu; Schubhebel ruhen nicht mittig).
 * W432: eigene Funktion (Selbsttest ruft sie im Bild-Modus). */
void bind_cap_joy_init(void)
{
    g_cap_jn = 0;
    if (ijoy_init() == 0) {
        int i, n;
        joy_cal_load(); /* W312: Lerner sieht kalibrierte Achsen */
        n = ijoy_scan();
        for (i = 0; i < n && i < IJOY_MAX_DEV; i++) {
            const ijoy_info_t *in = ijoy_info(i);
            if (!in) {
                break;
            }
            g_cap_jl[i] = *in;
            lctrl_joy_learner_reset(&g_cap_jlrn[i]);
            g_cap_jn = i + 1;
        }
    }
}

void bind_start_capture(HWND hd, int mode)
{
    char t[256], sbc_st[256];
    lctrl_pad_t cur;
    if (g_bind_sel < 0 || g_bind_sel >= LCTRL_ELEM_N) {
        return;
    }
    g_cap_mode = mode;
    g_cap_mod = 0;
    lctrl_combo_reset(&g_cap_pcombo); /* W365 */
    lctrl_combo_reset(&g_cap_scombo);
    g_cap_jp_dev = -1;
    g_cap_sbc_held[0] = g_cap_sbc_held[1] = g_cap_sbc_held[2] = 0;
    if (g_o.shot) { /* W228: nur Anzeige im Bild, keine Geraete/Timer */
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.bind_press"),
                    kCtrlElems[g_bind_sel].key, LT(kCtrlElems[g_bind_sel].lkey));
        ui_settext(bctl(hd, IDB_PROMPT), t);
        {
            const char *hint = lctrl_joy_dir_hint(g_bind_sel);
            if (!hint) { hint = bind_combo_hint(g_bind_sel); } /* W283, W365 */
            bind_set_result(hd, hint ? hint : "");
        }
        ui_settext(bctl(hd, IDB_CLOSE), LT("gui.btn_cancel"));
        InvalidateRect(bctl(hd, IDB_PROMPT), NULL, TRUE);
        bind_refresh_cur(hd);
        return;
    }
    g_cap_have_pad = lctrl_pad_first(&cur) >= 0;
    g_cap_prev = cur;
    lctrl_pad_learner_reset(&g_cap_plrn); /* W312 */
    /* W159: Original-Controller nur bei angeschlossenem Geraet (kurz
     * oeffnen wie die Statuszeile, Poll laeuft im Thread). */
    g_cap_sbc_thr = NULL;
    if (lctrl_sbc_status(sbc_st, sizeof sbc_st) == SBC_OK) {
        g_cap_sbc_stop = 0;
        g_cap_sbc_thr = CreateThread(NULL, 0, bind_sbc_thread, hd, 0, NULL);
    }
    bind_cap_joy_init();
    _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.bind_press"),
                kCtrlElems[g_bind_sel].key, LT(kCtrlElems[g_bind_sel].lkey));
    ui_settext(bctl(hd, IDB_PROMPT), t);
    { /* W226: Richtungs-Hinweis fuer Joystick-Achsen */
        const char *hint = lctrl_joy_dir_hint(g_bind_sel);
        if (!hint) { hint = bind_combo_hint(g_bind_sel); } /* W283, W365 */
        bind_set_result(hd, hint ? hint : "");
    }
    ui_settext(bctl(hd, IDB_CLOSE), LT("gui.btn_cancel"));
    InvalidateRect(bctl(hd, IDB_PROMPT), NULL, TRUE); /* W228: Farbe */
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
        char t[640], d[256];
        char atom[64], pa[96];
        int row = -1;
        bind_src_disp(src, d, sizeof d); /* W260: Mausrad mit Klartext */
        _snprintf_s(t, sizeof t, _TRUNCATE, "%s = %s.",
                    kCtrlElems[g_bind_sel].key, d);
        if (lctrl_bind_combo_single_use(&g_bind, src, atom, sizeof atom, &row) && row >= 0) {
            /* W365: Teil der Kombination ist auch einzeln belegt -> nur Hinweis, erlaubt. */
            size_t L = strlen(t);
            if (!lctrl_src_pretty(atom, pa, sizeof pa)) {
                strcpy_s(pa, sizeof pa, atom);
            }
            if (L + 1 < sizeof t) {
                t[L++] = ' ';
                _snprintf_s(t + L, sizeof t - L, _TRUNCATE, LT("gui.bind_combo_single"), pa,
                            kCtrlElems[row].key);
            }
        }
        bind_set_result(hd, t);
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

void bind_cap_key(HWND hd, int vk, LPARAM lp)
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
    if (lctrl_vk_is_gamepad(vk)) {
        /* W282 ISSUE6: Windows meldet Xbox-kompatible Geraete zusaetzlich
         * als VK_GAMEPAD_*-Tasten (KEY:201 = linker Trigger, 209 = linker
         * Stick-Knopf ...). Keine Tastatur: ignorieren, der Pad-Pfad
         * (PAD:..., Timer 7) lernt dieselbe Eingabe richtig. */
        return;
    }
    if (lctrl_bind_key_combo_row(g_bind_sel)) {
        /* W283 ORIG-KEY-BIND, W365: Zusatztaste + Taste (Shift+F8, Strg+S, Umschalt+1).
         * Zusatztaste allein: erst beim Loslassen (bind_cap_keyup). */
        if (lctrl_vk_is_modifier(vk)) {
            g_cap_mod = vk;
            return;
        }
        lctrl_key_combo(g_cap_mod, vk, src, sizeof src);
        g_cap_mod = 0;
        bind_cap_done(hd, bind_apply(hd, src));
        return;
    }
    lctrl_key_name(vk, nm, sizeof nm);
    _snprintf_s(src, sizeof src, _TRUNCATE, "KEY:%s", nm);
    bind_cap_done(hd, bind_apply(hd, src));
}

/* W283: Loslassen im Lernmodus. Zusatztaste ohne weitere Taste -> allein
 * belegen (KEY:SHIFT). 1 = verarbeitet. */
int bind_cap_keyup(HWND hd, int vk)
{
    char src[48];
    if (g_cap_mode && vk == VK_SNAPSHOT) { /* W290: Windows meldet die Druck-Taste nur beim Loslassen */
        bind_cap_key(hd, VK_SNAPSHOT, 0);
        return 1;
    }
    if (!g_cap_mode || !g_cap_mod || !lctrl_bind_key_combo_row(g_bind_sel) ||
        !lctrl_vk_is_modifier(vk)) {
        return 0;
    }
    lctrl_key_combo(0, g_cap_mod, src, sizeof src); /* g_cap_mod: Strg schon links/rechts */
    g_cap_mod = 0;
    bind_cap_done(hd, bind_apply(hd, src));
    return 1;
}

void bind_cap_mouse(HWND hd, UINT msg, WPARAM wp)
{
    const char *nm = msg == WM_LBUTTONDOWN   ? "MOUSE:LEFT"
                     : msg == WM_RBUTTONDOWN ? "MOUSE:RIGHT"
                     : msg == WM_MBUTTONDOWN ? "MOUSE:MIDDLE"
                     : HIWORD(wp) == XBUTTON2 ? "MOUSE:X2" : "MOUSE:X1";
    if (g_cap_mod && lctrl_bind_mouse_combo_row(g_bind_sel)) {
        /* W365: Zusatztaste gehalten + Maustaste -> "KEY:SHIFT+MOUSE:LEFT". */
        char src[64], m[48];
        lctrl_key_combo(0, g_cap_mod, m, sizeof m);
        if (strcmp(m, "KEY:LCTRL") == 0 || strcmp(m, "KEY:RCTRL") == 0) {
            strcpy_s(m, sizeof m, "KEY:CTRL");
        }
        _snprintf_s(src, sizeof src, _TRUNCATE, "%s+%s", m, nm);
        g_cap_mod = 0;
        bind_cap_done(hd, bind_apply(hd, src));
        return;
    }
    bind_cap_done(hd, bind_apply(hd, nm));
}

/* W260 MOUSE-WHEEL: Rad drehen bzw. kippen im Lernmodus -> Quelle. */
static void bind_cap_wheel(HWND hd, UINT msg, WPARAM wp)
{
    const char *nm = bind_wheel_src(msg, wp);
    if (nm) {
        bind_cap_done(hd, bind_apply(hd, nm));
    }
}

/* W365: Original-Controller-Taste ("SBC:CHAFF") laut Capture-Thread gehalten -> 1. */
static int bind_sbc_held(const char *atom)
{
    int bi;
    if (!atom || !atom[0]) {
        return 0;
    }
    if (_strnicmp(atom, "SBC:", 4) == 0) {
        atom += 4;
    }
    for (bi = 0; bi < 39; bi++) {
        const char *nm = lctrl_sbc_btn_name(bi);
        if (nm && _stricmp(nm, atom) == 0) {
            return (int)((g_cap_sbc_held[bi / 16] >> (bi % 16)) & 1);
        }
    }
    return 0;
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
    if (lctrl_bind_combo_row(g_bind_sel)) {
        /* W365 BIND-COMBO: erste Taste halten + zweite = "SBC:A+SBC:B"; Kippschalter (bi34..38)
         * bleiben gesetzt und warten daher nicht als erste Taste. */
        char out[96];
        int toggle = bi >= 34 && bi <= 38;
        if (!g_cap_scombo.pend[0] && toggle) {
            bind_cap_done(hd, bind_apply(hd, src));
            return;
        }
        if (lctrl_combo_feed(&g_cap_scombo, src, bind_sbc_held(g_cap_scombo.pend), out, sizeof out)) {
            bind_cap_done(hd, bind_apply(hd, out));
        }
        return;
    }
    bind_cap_done(hd, bind_apply(hd, src));
}

/* W365: wartende Original-Controller-Taste losgelassen -> allein belegen (Timer 7). */
static void bind_cap_sbc_tick(HWND hd)
{
    char out[96];
    if (g_cap_mode && g_cap_scombo.pend[0] &&
        lctrl_combo_feed(&g_cap_scombo, NULL, bind_sbc_held(g_cap_scombo.pend), out, sizeof out)) {
        bind_cap_done(hd, bind_apply(hd, out));
    }
}

void bind_cap_pad_state(HWND hd, const lctrl_pad_t *curp);

static void bind_cap_pad(HWND hd)
{
    lctrl_pad_t cur;
    if (!g_cap_have_pad || lctrl_pad_first(&cur) < 0) {
        return;
    }
    bind_cap_pad_state(hd, &cur);
}

/* Ein Pad-Takt (Selbsttest ruft das mit erfundenen Zustaenden). */
void bind_cap_pad_state(HWND hd, const lctrl_pad_t *curp)
{
    lctrl_pad_t cur = *curp;
    char nm[16];
    if (lctrl_bind_combo_row(g_bind_sel)) {
        /* W365 BIND-COMBO: erste Taste wartet; zweite bei gehaltener erster -> "PAD:LB+PAD:X",
         * erste allein losgelassen -> "PAD:LB". */
        char out[64];
        int r = lctrl_pad_combo_step(&g_cap_pcombo, &g_cap_prev, &cur, out, sizeof out);
        g_cap_prev = cur;
        if (r) {
            bind_cap_done(hd, bind_apply(hd, out));
        }
        return;
    }
    if (lctrl_pad_new_press(&g_cap_prev, &cur, nm, sizeof nm)) {
        char src[32];
        _snprintf_s(src, sizeof src, _TRUNCATE, "PAD:%s", nm);
        g_cap_prev = cur;
        bind_cap_done(hd, bind_apply(hd, src));
        return;
    }
    /* W312: ab Ruhestellung (vorher absolut: am Anschlag ruhende Kanaele
     * wurden sofort fuer jedes Achsen-Element gelernt). */
    if (g_bind_sel >= 0 && kCtrlElems[g_bind_sel].kind == LCTRL_AXIS &&
        lctrl_pad_learner_axis(&g_cap_plrn, &cur, nm, sizeof nm)) {
        char src[64];
        int pt = lctrl_axis_partner(g_bind_sel);
        bind_ana_default(g_bind_sel, nm, src, sizeof src);
        g_cap_prev = cur;
        if (pt >= 0 && lctrl_bind_axis_used_by(&g_bind, pt, src)) {
            char t[400];
            _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.bind_same_axis"), nm, kCtrlElems[pt].key);
            bind_set_result(hd, t);
            lctrl_pad_learner_reset(&g_cap_plrn);
            return;
        }
        bind_cap_done(hd, bind_apply(hd, src));
        return;
    }
    g_cap_prev = cur;
}

/* W221 HOTAS: Joystick-Eingabe im Lernmodus (Timer 7, 50 ms). Taste ->
 * JOYn:Bk, POV -> JOYn:POVkU.., Achse -> analog bzw. Z+/Z-/Zone. Slot aus
 * passender joyN-Zeile, sonst neue Zeile (bei Fehlschlag zurueck). */
/* W365 BIND-COMBO: Joystick-Taste(n) belegen; db < 0 = nur eine Taste. Slots erst jetzt (bei
 * Fehlschlag zurueck). */
static void bind_joy_btn_apply(HWND hd, int da, const char *pa, int db, const char *pb)
{
    char keep[IMAP_JOY_MAX][96], src[96];
    int sa, sb = 0, rc;
    memcpy(keep, g_bind.joy, sizeof keep);
    sa = lctrl_joy_slot_for(&g_bind, g_cap_jl, g_cap_jn, da);
    if (db >= 0) {
        sb = lctrl_joy_slot_for(&g_bind, g_cap_jl, g_cap_jn, db);
    }
    if (sa < 0 || sb < 0) {
        memcpy(g_bind.joy, keep, sizeof keep);
        ui_settext(bctl(hd, IDB_PROMPT), LT("gui.joy_noslot"));
        return;
    }
    if (db >= 0) {
        _snprintf_s(src, sizeof src, _TRUNCATE, "JOY%d:%s+JOY%d:%s", sa, pa, sb, pb);
    } else {
        _snprintf_s(src, sizeof src, _TRUNCATE, "JOY%d:%s", sa, pa);
    }
    rc = bind_apply(hd, src);
    if (rc != 0) {
        memcpy(g_bind.joy, keep, sizeof keep);
    }
    bind_cap_done(hd, rc);
}

void bind_cap_joy(HWND hd)
{
    int i, jp_held = 0;
    for (i = 0; i < g_cap_jn && g_cap_mode; i++) {
        imap_joy_state_t cur;
        char part[48], src[64];
        int r, ok = ijoy_read(i, &cur);
        r = lctrl_joy_learner_step(&g_cap_jlrn[i], ok, &cur,
                                   g_cap_jl[i].axmask, g_cap_jl[i].npov,
                                   g_bind_sel, part, sizeof part);
        if (i == g_cap_jp_dev) { /* W365: wartende Taste noch gehalten? */
            int k = atoi(g_cap_jp_part + 1) - 1; /* "B3" = Bit 2 */
            jp_held = ok && k >= 0 && k < IJOY_BTNS && ((cur.btn[k / 32] >> (k % 32)) & 1u);
        }
        if (r == 4) { /* W226: Zone enthielte die Ruhestellung */
            bind_set_result(hd, LT("gui.joy_restzone"));
            continue;
        }
        if (r == 1 && lctrl_bind_combo_row(g_bind_sel) && (part[0] == 'B' || part[0] == 'b')) {
            /* W365 BIND-COMBO: erste Taste wartet, zweite bei gehaltener erster -> Kombination
             * (auch ueber zwei Geraete, z. B. Schubhebel + Knueppel). Achsen/POV bleiben einzeln. */
            if (g_cap_jp_dev < 0) {
                g_cap_jp_dev = i;
                strncpy_s(g_cap_jp_part, sizeof g_cap_jp_part, part, _TRUNCATE);
                jp_held = 1; /* gerade gedrueckt: nicht schon am Takt-Ende einzeln belegen */
                continue;
            }
            if (!(g_cap_jp_dev == i && _stricmp(g_cap_jp_part, part) == 0)) {
                int da = g_cap_jp_dev;
                char pa[16];
                int k = atoi(g_cap_jp_part + 1) - 1; /* "B3" = Bit 2 */
                imap_joy_state_t ps;
                int held = (da == i) ? (k >= 0 && k < IJOY_BTNS && ((cur.btn[k / 32] >> (k % 32)) & 1u))
                                     : (da < i ? jp_held
                                               : (ijoy_read(da, &ps) && k >= 0 && k < IJOY_BTNS &&
                                                  ((ps.btn[k / 32] >> (k % 32)) & 1u)));
                strcpy_s(pa, sizeof pa, g_cap_jp_part);
                g_cap_jp_dev = -1;
                if (held) {
                    bind_joy_btn_apply(hd, da, pa, i, part);
                } else {
                    bind_joy_btn_apply(hd, da, pa, -1, NULL);
                }
                return;
            }
            continue;
        }
        if (r) {
            char keep[IMAP_JOY_MAX][96];
            int slot, rc;
            memcpy(keep, g_bind.joy, sizeof keep);
            slot = lctrl_joy_slot_for(&g_bind, g_cap_jl, g_cap_jn, i);
            if (slot < 0) {
                ui_settext(bctl(hd, IDB_PROMPT), LT("gui.joy_noslot"));
                return;
            }
            _snprintf_s(src, sizeof src, _TRUNCATE, "JOY%d:%s", slot, part);
            if (r == 3 && kCtrlElems[g_bind_sel].kind == LCTRL_AXIS) {
                /* W312: dieselbe Achse schon beim Partner (aim_x/aim_y) ->
                 * im Spiel bewegten sich beide nur diagonal: ablehnen. */
                int pt = lctrl_axis_partner(g_bind_sel);
                if (pt >= 0 && lctrl_bind_axis_used_by(&g_bind, pt, src)) {
                    char t[400], an[24];
                    const char *c = strchr(part, ':');
                    size_t L = c ? (size_t)(c - part) : strlen(part);
                    if (L >= sizeof an) { L = sizeof an - 1; }
                    memcpy(an, part, L);
                    an[L] = 0;
                    memcpy(g_bind.joy, keep, sizeof keep);
                    _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.bind_same_axis"), an,
                                kCtrlElems[pt].key);
                    bind_set_result(hd, t);
                    lctrl_joy_learner_reset(&g_cap_jlrn[i]);
                    return;
                }
            }
            rc = bind_apply(hd, src);
            if (rc != 0) {
                memcpy(g_bind.joy, keep, sizeof keep);
            }
            bind_cap_done(hd, rc);
            if (rc == 0 && r == 3 && strstr(part, "inv=1")) {
                char t[300];
                _snprintf_s(t, sizeof t, _TRUNCATE, "%s = %s. %s",
                            kCtrlElems[g_bind_sel].key, src,
                            LT("gui.joy_learn_inv"));
                bind_set_result(hd, t);
            }
            return;
        }
    }
    if (g_cap_mode && g_cap_jp_dev >= 0 && !jp_held) {
        /* W365: wartende Joystick-Taste allein losgelassen -> allein belegen. */
        char pa[16];
        int da = g_cap_jp_dev;
        strcpy_s(pa, sizeof pa, g_cap_jp_part);
        g_cap_jp_dev = -1;
        bind_joy_btn_apply(hd, da, pa, -1, NULL);
    }
}

/* W282 ISSUE6: leeres Layout - nach Rueckfrage alle Spiel-Elemente
 * leeren (Client-Aktionen bleiben), danach nur Benoetigtes belegen.
 * Gespeichert wird erst mit "Belegung speichern". */
static void bind_clear_all(HWND hd)
{
    char t[256];
    int n;
    if (g_cap_mode) {
        bind_end_capture(hd, NULL);
    }
    if (!g_o.shot &&
        ui_msgbox(hd, LT("gui.bind_clear_q"), LT("gui.bind_title"),
                  MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) != IDYES) {
        return;
    }
    n = lctrl_bind_clear_game(&g_bind);
    bind_refresh_cur(hd);
    bind_refresh_conf(hd);
    _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.bind_cleared"), n);
    ui_settext(bctl(hd, IDB_FSTAT), t);
}

/* W228: 0 = gespeichert, sonst nicht (Meldung in der Fusszeile). */
int bind_save(HWND hd)
{
    char rest[64], fn[72], pname[68], dir[1100], path[1200], err[512];
    ui_gettext(bctl(hd, IDB_NAME), rest, sizeof rest);
    trim_u8(rest);
    if (!lctrl_user_rest_ok(rest)) {
        ui_settext(bctl(hd, IDB_FSTAT), LT("gui.bind_badname"));
        return 1;
    }
    lctrl_user_filename(rest, fn, sizeof fn);
    strcpy_s(pname, sizeof pname, fn);
    pname[strlen(pname) - 4] = '\0';
    if (lctrl_name_shipped(pname)) {
        ui_settext(bctl(hd, IDB_FSTAT), LT("gui.bind_shipped"));
        return 1;
    }
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s/profiles", g_client_dir);
    lcfg_mkdir_p(dir);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s/%s", dir, fn);
    /* W282 ISSUE6: das geoeffnete eigene Layout unter seinem Namen
     * speichern = erwartetes Ueberschreiben, keine Rueckfrage. */
    if (u8_GetFileAttributes(path) != INVALID_FILE_ATTRIBUTES &&
        strcmp(pname, g_bind_base_name) != 0) {
        char q[300];
        _snprintf_s(q, sizeof q, _TRUNCATE, LT("gui.bind_overwrite"),
                    pname);
        if (ui_msgbox(hd, q, LT("gui.bind_save2"),
                      MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) != IDYES) {
            return 1;
        }
    }
    if (lctrl_bind_save(path, &g_bind, g_bind_base_name, err, sizeof err) !=
        0) {
        ui_settext(bctl(hd, IDB_FSTAT), err);
        return 1;
    }
    g_bind_clean = g_bind; /* W228: Stand fuer "ungespeichert" */
    strcpy_s(g_bind_saved, sizeof g_bind_saved, pname);
    {
        char t[256];
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.bind_saved"), fn);
        ui_settext(bctl(hd, IDB_FSTAT), t);
    }
    refresh_ctrl_profiles(pname);
    update_ctrl_overview();
    return 0;
}

static void bind_check(HWND hd)
{
    char t[512];
    if (!g_bind_saved[0]) {
        ui_settext(bctl(hd, IDB_FSTAT), LT("gui.bind_savefirst"));
        return;
    }
    ui_settext(bctl(hd, IDB_FSTAT), LT("gui.bind_checking"));
    UpdateWindow(hd);
    (void)lctrl_run_dump(g_client_dir, g_bind_saved, t, sizeof t);
    ui_settext(bctl(hd, IDB_FSTAT), t);
}

/* W228: Schliessen (Knopf, ESC, X): Lernmodus nur abbrechen; ungespeicherte
 * Aenderungen erst nachfragen (Ja = speichern, Nein = verwerfen). */
static void bind_try_close(HWND hd)
{
    if (g_cap_mode) {
        bind_end_capture(hd, LT("gui.bind_abort"));
        return;
    }
    if (!g_o.shot && !bind_same(&g_bind, &g_bind_clean)) {
        int r = ui_msgbox(hd, LT("gui.bind_unsaved"), LT("gui.bind_title"),
                          MB_ICONQUESTION | MB_YESNOCANCEL);
        if (r == IDCANCEL) {
            return;
        }
        if (r == IDYES && bind_save(hd) != 0) {
            SetFocus(bctl(hd, IDB_NAME));
            return;
        }
    }
    g_bind_done = 1;
}

int bind_shot_save(HWND hd);

/* W228: Rahmen der Liste, Detail-Karte, Trennlinie der Fusszeile. */
static void bind_paint_bg(HWND hd, HDC dc)
{
    RECT r;
    canvas_t c;
    GetClientRect(hd, &r);
    memset(&c, 0, sizeof c);
    if (!cv_make(&c, r.right, r.bottom)) {
        if (!g_br_dialog) {
            g_br_dialog = CreateSolidBrush(C_DIALOG);
        }
        FillRect(dc, &r, g_br_dialog);
        return;
    }
    GdiFlush();
    cv_fill(&c, 0, 0, r.right, r.bottom, C_DIALOG, 255, 0);
    cv_ring(&c, SX(BD_LX) - 1, SX(BD_TOP) - 1, SX(BD_LX + BD_LW) - SX(BD_LX) + 2,
            SX(BD_BOT) - SX(BD_TOP) + 2, C_INPUT_BRD, 255, SX(4), 1);
    cv_ring(&c, SX(BD_CX), SX(BD_TOP), SX(BD_CX + BD_CW) - SX(BD_CX),
            SX(BD_BOT) - SX(BD_TOP), C_INPUT_BRD, 255, SX(8), 1);
    cv_fill(&c, SX(BD_LX), SX(BD_FSEP), SX(BD_CX + BD_CW) - SX(BD_LX), 1,
            C_INPUT_BRD, 255, 0);
    GdiFlush();
    BitBlt(dc, 0, 0, r.right, r.bottom, c.dc, 0, 0, SRCCOPY);
    cv_free(&c);
}

static LRESULT CALLBACK bind_proc(HWND hd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (id == IDB_CLOSE || id == IDCANCEL) {
            bind_try_close(hd);
        } else if (id == IDB_LIST && code == LBN_SELCHANGE) {
            if (g_cap_mode) {
                bind_end_capture(hd, LT("gui.bind_abort_sel"));
            }
            bind_list_skip_header(hd); /* W228 */
            bind_refresh_cur(hd);
        } else if (id == IDB_SRC && code == LBN_SELCHANGE) {
            bind_enable(hd); /* W228: Entfernen gilt der gewaehlten Quelle */
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
            }
        } else if (id == IDB_DEL) { /* W228: nur die gewaehlte Quelle */
            int a = bind_src_sel(hd);
            if (a >= 0 && lctrl_bind_remove_alt(&g_bind, g_bind_sel, a) == 0) {
                bind_refresh_cur(hd);
            }
        } else if (id == IDB_RST) {
            if (g_bind_sel >= 0) {
                g_bind.row[g_bind_sel] = g_bind_base.row[g_bind_sel];
                bind_refresh_cur(hd);
            }
        } else if (id == IDB_CLEAR) { /* W282 ISSUE6: leeres Layout */
            bind_clear_all(hd);
        } else if (id == IDB_SAVE) {
            bind_save(hd);
        } else if (id == IDB_CHECK) {
            bind_check(hd);
        } else if (id == IDB_SMODE) { /* W271: Stick-Modus umschalten */
            bind_smode_cycle(hd);
        } else if (id == IDB_JINV) { /* W221: Schalter Invertieren, W228 sofort */
            uictl_t *u = ui_by_hwnd(bctl(hd, IDB_JINV));
            if (u) {
                u->checked = !u->checked;
                InvalidateRect(u->h, NULL, FALSE);
            }
            bind_joy_apply(hd);
        } else if (id == IDB_JDZ && code == EN_CHANGE && !g_bind_joyset &&
                   !g_cap_mode) {
            bind_joy_apply(hd); /* W228: Totzone wirkt sofort */
        }
        return 0;
    }
    case WM_MEASUREITEM: { /* W228: gegliederte Liste (eigene Zeichnung) */
        MEASUREITEMSTRUCT *m = (MEASUREITEMSTRUCT *)lp;
        if (m->CtlType == ODT_LISTBOX) {
            m->itemHeight = (UINT)SX(BD_ROW_H);
            return TRUE;
        }
        break;
    }
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT *d = (const DRAWITEMSTRUCT *)lp;
        if (d->CtlType == ODT_LISTBOX && d->CtlID == IDB_LIST) {
            bind_draw_row(d);
        } else {
            draw_item(d);
        }
        return TRUE;
    }
    case WM_CTLCOLORSTATIC: { /* W228: Hinweise gedimmt, Lernmodus/Konflikt farbig */
        LRESULT br = ctlcolor((HDC)wp, (HWND)lp, 1);
        int cid = GetDlgCtrlID((HWND)lp), set = 1;
        COLORREF col = C_DIM;
        switch (cid) {
        case IDB_TITLE: case IDB_CUR: case IDB_SRCL: case IDB_JHINT:
        case IDB_JNOW: case IDB_NAMEL: case IDB_INI: case IDB_JDZL:
        case IDB_FSTAT: case IDB_JLIVE:
            break;
        case IDB_PROMPT:
            col = g_cap_mode ? C_ACCENT_HI : C_DIM;
            break;
        case IDB_CONFL:
            col = g_bind_nconf ? C_WARN : C_DIM;
            break;
        default:
            set = 0;
            break;
        }
        if (set) {
            SetTextColor((HDC)wp, col);
        }
        return br;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        return ctlcolor((HDC)wp, (HWND)lp, 0);
    case WM_ERASEBKGND:
        bind_paint_bg(hd, (HDC)wp);
        return 1;
    case WM_TIMER:
        if (wp == 7 && g_cap_mode) {
            bind_cap_pad(hd);
            if (g_cap_mode) {
                bind_cap_joy(hd); /* W221 */
            }
            if (g_cap_mode) {
                bind_cap_sbc_tick(hd); /* W365 */
            }
        } else if (wp == 10 && !g_cap_mode) { /* W226: Live-Zeile */
            bind_joy_live(hd);
        } else if (wp == 11 && g_o.shot == 3) { /* W260: Mausrad-Beispiel */
            KillTimer(hd, 11);
            PostMessageW(hd, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), 0);
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
        bind_try_close(hd);
        return 0;
    }
    return DefWindowProcW(hd, msg, wp, lp);
}

/* W228: Aufbau. Reihenfolge = Tab-Reihenfolge: Liste, Quellen, Aktionen,
 * Joystick, Konflikte, dann Fusszeile (Name, Pruefen, Speichern, Schliessen).
 * Positionen im Detailbereich setzt bind_layout je Aktionsart. */
static void bind_build(HWND hd, const char *base_name)
{
    HWND lb;
    char t[200];
    int g, i;
    _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.bind_base2"),
                base_name && *base_name ? base_name : LT("gui.bind_empty"), LCTRL_ELEM_N);
    mk(hd, K_STATIC, IDB_TITLE, PG_DIALOG, BD_LX, 12, BD_CX + BD_CW - BD_LX, 20, t,
       WS_VISIBLE);
    lb = mk(hd, K_PKLIST, IDB_LIST, PG_DIALOG, BD_LX, BD_TOP, BD_LW, BD_BOT - BD_TOP,
            "", WS_VISIBLE);
    SendMessageW(lb, LB_SETITEMHEIGHT, 0, SX(BD_ROW_H));
    for (g = 0; g < LCTRL_GROUP_N; g++) { /* Gruppen in Anzeige-Reihenfolge */
        list_add(lb, "", (LPARAM)(-100 - g), 1);
        for (i = 0; i < LCTRL_ELEM_N; i++) {
            if (lctrl_elem_group(i) == g) {
                list_add(lb, "", (LPARAM)i, 1);
            }
        }
    }
    ListBox_SetCurSel(lb, 1); /* erste Aktion (Zeile 0 = Gruppenkopf) */
    g_bind_lb_prev = 1;
    /* Detailbereich (Platz setzt bind_layout) */
    mk(hd, K_STATIC, IDB_DNAME, PG_DIALOG, BD_IX, 52, BD_IW, 24, "", WS_VISIBLE);
    mk(hd, K_STATIC, IDB_CUR, PG_DIALOG, BD_IX, 78, BD_IW, 34, "", WS_VISIBLE);
    mk(hd, K_STATIC, IDB_SRCL, PG_DIALOG, BD_IX, 118, BD_IW, 18, LT("gui.bind_sources"),
       WS_VISIBLE);
    mk(hd, K_LIST, IDB_SRC, PG_DIALOG, BD_IX, 138, BD_IW, 80, "", WS_VISIBLE);
    mk(hd, K_ACCENT, IDB_SET, PG_DIALOG, BD_IX, 226, 218, 30, LT("gui.bind_replace"),
       WS_VISIBLE);
    mk(hd, K_BTN, IDB_ADD, PG_DIALOG, BD_IX + 226, 226, 218, 30, LT("gui.bind_add"),
       WS_VISIBLE);
    mk(hd, K_BTN, IDB_DEL, PG_DIALOG, BD_IX, 262, 218, 30, LT("gui.bind_remove"),
       WS_VISIBLE);
    mk(hd, K_BTN, IDB_RST, PG_DIALOG, BD_IX + 226, 262, 218, 30, LT("gui.bind_reset2"),
       WS_VISIBLE);
    mk(hd, K_BTN, IDB_MX, PG_DIALOG, BD_IX, 300, 218, 30, LT("gui.bind_mousex"), 0);
    mk(hd, K_BTN, IDB_MY, PG_DIALOG, BD_IX + 226, 300, 218, 30, LT("gui.bind_mousey"), 0);
    mk(hd, K_BTN, IDB_SMODE, PG_DIALOG, BD_IX, 338, BD_IW, 30, LT("gui.bind_smode0"), 0); /* W271 */
    mk(hd, K_STATIC, IDB_PROMPT, PG_DIALOG, BD_IX, 338, BD_IW, 40, LT("gui.bind_idle"),
       WS_VISIBLE);
    mk(hd, K_STATIC, IDB_RESULT, PG_DIALOG, BD_IX, 382, BD_IW, 34, "", WS_VISIBLE);
    /* W221 HOTAS (experimentell): Totzone/Invertierung fuer JOY-Achsen */
    mk(hd, K_STATIC, IDB_JLBL, PG_DIALOG, BD_IX, 422, BD_IW, 20, LT("gui.bind_joyhdr"), 0);
    mk(hd, K_STATIC, IDB_JDZL, PG_DIALOG, BD_IX, 451, 70, 18, LT("gui.bind_dz"), 0);
    mk(hd, K_EDIT, IDB_JDZ, PG_DIALOG, BD_IX + 74, 446, 64, 28, "", WS_BORDER);
    mk(hd, K_TOGGLE, IDB_JINV, PG_DIALOG, BD_IX + 150, 446, 150, 28, LT("gui.joy_inv"), 0);
    mk(hd, K_STATIC, IDB_JNOW, PG_DIALOG, BD_IX + 306, 451, BD_IW - 306, 18,
       LT("gui.bind_joynow"), 0);
    mk(hd, K_STATIC, IDB_JLIVE, PG_DIALOG, BD_IX, 480, BD_IW, 18, "", 0); /* W226 */
    mk(hd, K_STATIC, IDB_JHINT, PG_DIALOG, BD_IX, 446, BD_IW, 34, LT("gui.bind_joyhint"), 0);
    mk(hd, K_STATIC, IDB_CONFL, PG_DIALOG, BD_IX, 506, BD_IW, 20, "", WS_VISIBLE);
    mk(hd, K_LIST, IDB_CONF, PG_DIALOG, BD_IX, 530, BD_IW, 56, "", 0);
    /* Fusszeile: Belegung als Datei (gilt fuer den ganzen Dialog) */
    mk(hd, K_STATIC, IDB_NAMEL, PG_DIALOG, BD_LX, 623, 84, 18, LT("gui.bind_name"),
       WS_VISIBLE);
    mk(hd, K_EDIT, IDB_NAME, PG_DIALOG, BD_LX + 84, 618, 150, 28, "",
       WS_VISIBLE | WS_BORDER);
    mk(hd, K_STATIC, IDB_INI, PG_DIALOG, BD_LX + 238, 623, 30, 18, ".ini", WS_VISIBLE);
    mk(hd, K_BTN, IDB_CHECK, PG_DIALOG, BD_LX + 272, 617, 128, 30, LT("gui.bind_check2"),
       WS_VISIBLE);
    mk(hd, K_BTN, IDB_CLEAR, PG_DIALOG, BD_LX + 406, 617, 112, 30, LT("gui.bind_clear"),
       WS_VISIBLE); /* W282 ISSUE6: leeres Layout */
    mk(hd, K_ACCENT, IDB_SAVE, PG_DIALOG, BD_CX + BD_CW - 124 - 8 - 170, 617, 170, 30,
       LT("gui.bind_save2"), WS_VISIBLE);
    mk(hd, K_BTN, IDB_CLOSE, PG_DIALOG, BD_CX + BD_CW - 124, 617, 124, 30,
       LT("gui.btn_close"), WS_VISIBLE);
    mk(hd, K_STATIC, IDB_FSTAT, PG_DIALOG, BD_LX, 654, BD_CX + BD_CW - BD_LX, 18, "",
       WS_VISIBLE);
    g_bind_lay = -1;
    g_bind_nconf = 0;
    g_bind_res_on = 0;
}

/* Offscreen-Position (rechts ausserhalb aller Monitore). */
void offscreen_pos(int *x, int *y)
{
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    *x = vx + vw + 4000;
    *y = vy + 100;
}

int is_offscreen(HWND h)
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

void bind_dialog_modal(const char *base_name, const char *base_path)
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
    rc.right = SX(BD_W); /* W228: Liste + Detail-Karte + Fusszeile */
    rc.bottom = SX(BD_H);
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
    { /* W282 ISSUE6: eigenes Layout geoeffnet -> Name bleibt im Feld */
        char rest[64];
        if (lctrl_user_rest_of(base_name, rest, sizeof rest)) {
            ui_settext(bctl(hd, IDB_NAME), rest);
        }
    }
    g_bind_clean = g_bind; /* W228: ungespeichert = Abweichung hiervon */
    g_live_scanned = 0;
    g_bind_joydemo = g_o.shot == 3 && g_o.bind_joydemo;
    if (g_bind_joydemo && g_o.bind_sel >= 0) { /* W226: --bind-shot=joy */
        strcpy_s(g_bind.joy[0], sizeof g_bind.joy[0], "Example HOTAS Throttle");
        lctrl_bind_set(&g_bind, g_o.bind_sel, "KEY:W, JOY0:Z:dz=0.05:inv=1");
        bind_set_result(hd, lctrl_joy_dir_hint(g_o.bind_sel));
    }
    if (g_o.shot == 3 && g_o.bind_combodemo && g_o.bind_sel >= 0) { /* W365: --bind-shot=combo */
        char d[256], t[400];
        lctrl_bind_set(&g_bind, g_o.bind_sel, "KEY:CTRL+KEY:S, KEY:20, PAD:LB+PAD:X");
        bind_src_disp("KEY:CTRL+KEY:S", d, sizeof d);
        _snprintf_s(t, sizeof t, _TRUNCATE, "%s = %s.", kCtrlElems[g_o.bind_sel].key, d);
        bind_set_result(hd, t);
    }
    if (g_o.shot == 3 && g_o.bind_stickdemo && g_o.bind_sel >= 0) { /* W271: --bind-shot=stick */
        lctrl_bind_set(&g_bind, g_o.bind_sel,
                       "MOUSE:X:mode=aim, PAD:LX:dz=0.15:curve=1.5:mode=rate");
    }
    if (g_o.shot == 3 && g_o.bind_sel >= 0) { /* W159: --bind-shot=vr */
        HWND lb = bctl(hd, IDB_LIST);
        int n = ListBox_GetCount(lb), at;
        for (at = 0; at < n; at++) {
            if ((int)ListBox_GetItemData(lb, at) == g_o.bind_sel) {
                ListBox_SetCurSel(lb, at);
                ListBox_SetTopIndex(lb, at > 1 ? at - 1 : 0);
                g_bind_lb_prev = at;
                break;
            }
        }
    }
    bind_refresh_cur(hd);
    if (g_o.shot == 3 && g_o.bind_learn) { /* W228: --bind-learn */
        bind_start_capture(hd, g_o.bind_wheeldemo ? 2 : 1);
        if (g_o.bind_wheeldemo) {
            /* W260: --bind-shot=wheel --bind-learn: nach dem Zeigen eine
             * Raste hoch an das eigene (unsichtbare) Dialogfenster - laeuft
             * durch den echten Lernmodus-Pfad der Nachrichtenschleife
             * (Timer 11, kein SendInput). */
            SetTimer(hd, 11, 150, NULL);
        }
    }
    if (g_o.shot == 3) {
        SetTimer(hd, 9, 400, NULL);
    } else {
        SetTimer(hd, 10, 100, NULL); /* W226: Live-Zeile */
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
    if (g_bind_testfn && shot) { /* W282: Selbsttest im offscreen-Dialog */
        g_bind_testfn(hd);
        g_bind_done = 1;
    }
    while (!g_bind_done && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (g_cap_mode &&
            (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN)) {
            bind_cap_key(hd, (int)msg.wParam, msg.lParam);
            continue;
        }
        if (g_cap_mode && (msg.message == WM_KEYUP || msg.message == WM_SYSKEYUP) &&
            bind_cap_keyup(hd, (int)msg.wParam)) { /* W283 */
            continue;
        }
        if (g_cap_mode && (msg.message == WM_MOUSEWHEEL ||
                           msg.message == WM_MOUSEHWHEEL)) { /* W260 */
            bind_cap_wheel(hd, msg.message, msg.wParam);
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
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN &&
            GetFocus() == bctl(hd, IDB_NAME)) { /* W228: Enter im Namen = Speichern */
            SendMessageW(hd, WM_COMMAND, MAKEWPARAM(IDB_SAVE, BN_CLICKED),
                         (LPARAM)bctl(hd, IDB_SAVE));
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
