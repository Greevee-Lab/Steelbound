/* launcher_gui_assist.c - Controller-Assistent (W143) + HOTAS (W221).
 * W462 LAUNCHER-SPLIT: aus launcher_gui.c verschoben (unveraendert), gemeinsamer Zustand
 * in launcher_gui_int.h. */
#include "launcher_gui_int.h"
/* Controller-Assistent (W143 CTRL-ASSIST)                              */
/* Live-Ansicht, gefuehrter Test, Kalibrierung, Lampen, Gamepad.         */
/* ------------------------------------------------------------------ */
void pump(void);
int shot_window(HWND w, const char *path, int png);
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
#define IDA_L_NAME 2061
#define IDA_L_SEQ 2062
#define IDA_L_ON 2063
#define IDA_L_OFF 2064
#define IDA_L_HINT 2065
#define IDA_L_MOCK 2066
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
        strncpy_s(buf, cap, LT("as.k_tuner"), _TRUNCATE);
        break;
    case SBC_TK_GEAR:
        strncpy_s(buf, cap, LT("as.k_gear"), _TRUNCATE);
        break;
    default:
        strncpy_s(buf, cap, LT("as.k_led"), _TRUNCATE);
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
        strncpy_s(out, cap, LT("as.p_tuner"), _TRUNCATE);
        break;
    case SBC_TK_GEAR:
        strncpy_s(out, cap, LT("as.p_gear"), _TRUNCATE);
        break;
    default:
        strncpy_s(out, cap, LT("as.p_led"), _TRUNCATE);
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

/* W148: Kurznamen Tastenraster (volle Namen: Hover-Status, Testliste, Export).
 * W282 ISSUE6: bi0 = Report "MainWeapon" (Hauptwaffe, Daumentaste oben),
 * bi1 = Report "Fire" (Sub-Waffe, Abzug vorn) - frueher vertauscht als
 * "TRIG"/"FIRE" beschriftet (Tester-Befund). Funktion unveraendert. */
const char *const kBtnShort[39] = {
    "MAIN", "SUB", "LOCK","EJECT", "HATCH", "IGN",
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
        strncpy_s(t, sizeof t, LT("as.src_device"), _TRUNCATE);
    } else if (src == ASSIST_SRC_FIXTURE) {
        const char *b = strrchr(g_ad.fx_path, '\\');
        const char *b2 = strrchr(g_ad.fx_path, '/');
        if (b2 && (!b || b2 > b)) {
            b = b2;
        }
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("as.src_fixture"),
                    b ? b + 1 : g_ad.fx_path);
    } else {
        strncpy_s(t, sizeof t, LT("as.src_none"), _TRUNCATE);
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

void assist_dialog_modal(const char *fixture)
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

void open_assist_dialog(void)
{
    assist_dialog_modal(NULL);
}

/* ------------------------------------------------------------------ */
/* W221 HOTAS (experimentell): Geraeteliste + Live-Anzeige             */
/* (eigener Dialog: die Steuerungs-Seite hat keinen Platz mehr)         */
/* ------------------------------------------------------------------ */
#define IDJ_LIST 601
#define IDJ_SCAN 602
#define IDJ_INFO 603
#define IDJ_CLOSE 604
#define IDJ_EXP 605
#define IDJ_DEVH 606
/* W312 JOY-CALIB: Geraeteinfo kopieren + Kalibrierung je Achse */
#define IDJ_COPY 607
#define IDJ_CAXL 608
#define IDJ_CAX 609
#define IDJ_CREC 610
#define IDJ_CMID 611
#define IDJ_CDZL 612
#define IDJ_CDZ 613
#define IDJ_CINV 614
#define IDJ_CLIN 615
#define IDJ_CSAVE 616
#define IDJ_CRESET 617
#define IDJ_CSTAT 618
#define JOY_LIVE_X 274
#define JOY_LIVE_Y 118
#define JOY_LIVE_W 474
#define JOY_LIVE_H 330
#define JOY_TIMER 11

int g_joy_done, g_joy_n, g_joy_ok, g_joy_demo, g_joy_have;
static HWND g_joy_dlg_hd;
ijoy_info_t g_joy_list[IJOY_MAX_DEV];
/* W282 ISSUE6: Xbox-kompatible Geraete (XInput), die die Liste bewusst
 * ueberspringt - mit Erklaerung statt stillem Fehlen (Listen-Daten 1000+k). */
char g_joy_xi[8][96];
int g_joy_nxi;
static imap_joy_state_t g_joy_st;
/* W312 JOY-CALIB: Rohstand (ohne Kalibrierung, nur Anzeige - nie Log/Datei)
 * und die gerade bearbeitete Kalibrierung (Geraet g_jc_dev, Slot g_jc_ax). */
static imap_joy_state_t g_joy_raw;
static int g_jc_dev = -1, g_jc_ax = -1, g_jc_rec, g_jc_slots[IJOY_AXES], g_jc_nslots;
static ijoy_cal_t g_jc;
static int g_joy_calshot; /* --joy-shot --joy-cal: Kalibrierzustand im Bild */

int shot_window(HWND w, const char *path, int png);

/* Nur Shot-Modus: Beispielgeraete (kein Geraet am Testrechner noetig). */
static void joy_demo_fill(void)
{
    memset(g_joy_list, 0, sizeof g_joy_list);
    strcpy_s(g_joy_list[0].name, sizeof g_joy_list[0].name, "Example HOTAS Throttle");
    strcpy_s(g_joy_list[0].product, sizeof g_joy_list[0].product, "Example HOTAS Throttle");
    g_joy_list[0].axmask = 0x7F;
    g_joy_list[0].nbtn = 32;
    g_joy_list[0].npov = 1;
    strcpy_s(g_joy_list[1].name, sizeof g_joy_list[1].name, "Example Rudder Pedals");
    strcpy_s(g_joy_list[1].product, sizeof g_joy_list[1].product, "Example Rudder Pedals");
    g_joy_list[1].axmask = 0x07;
    g_joy_n = 2;
    memset(&g_joy_st, 0, sizeof g_joy_st);
    g_joy_st.axis[0] = 6000;   /* X */
    g_joy_st.axis[1] = -9000;  /* Y */
    g_joy_st.axis[2] = 21000;  /* Z: Schubhebel */
    g_joy_st.axis[3] = -32768; /* RX */
    g_joy_st.axis[4] = 0;
    g_joy_st.axis[5] = -2500;
    g_joy_st.axis[6] = 32767;  /* S0 */
    g_joy_st.btn[0] = (1u << 0) | (1u << 4) | (1u << 11);
    g_joy_st.pov[0] = 9000;
    g_joy_st.pov[1] = g_joy_st.pov[2] = g_joy_st.pov[3] = -1;
    g_joy_have = 1;
    g_joy_raw = g_joy_st; /* W312: Beispiel - roh = kalibriert, ausser X */
    g_joy_raw.axis[0] = 3000;
    g_joy_list[0].vid = 0x044F;
    g_joy_list[0].pid = 0xB10A;
}

static int joy_sel(HWND hd)
{
    HWND lb = bctl(hd, IDJ_LIST);
    int s = ListBox_GetCurSel(lb);
    int i = s >= 0 ? (int)ListBox_GetItemData(lb, s) : -1;
    return (i >= 0 && i < g_joy_n) ? i : -1;
}

/* W282: gewaehlter XInput-Eintrag (0..) oder -1. */
static int joy_sel_xi(HWND hd)
{
    HWND lb = bctl(hd, IDJ_LIST);
    int s = ListBox_GetCurSel(lb);
    int i = s >= 0 ? (int)ListBox_GetItemData(lb, s) - 1000 : -1;
    return (i >= 0 && i < g_joy_nxi) ? i : -1;
}

/* ---- W312 JOY-CALIB: Kalibrierung (user.cfg joy_calN) ---- */
void joy_cal_load(void)
{
    char uc[1100], lines[IJOY_CAL_MAX][96];
    int n, k;
    if (g_o.shot || !g_client_dir[0]) {
        return;
    }
    lcfg_user_cfg_path(g_client_dir, uc, sizeof uc);
    n = joycal_cfg_read(uc, lines, IJOY_CAL_MAX);
    ijoy_cal_clear();
    for (k = 0; k < n; k++) {
        const char *eq = strchr(lines[k], '=');
        unsigned short v, p;
        int ax;
        ijoy_cal_t c;
        if (eq && ijoy_cal_parse(eq + 1, &v, &p, &ax, &c)) {
            ijoy_cal_set(v, p, ax, &c);
        }
    }
}

/* Tabelle -> user.cfg (alle joy_cal-Zeilen ersetzt). 0 = ok. */
static int joy_cal_store(const char *user_cfg)
{
    char lines[IJOY_CAL_MAX][96];
    int k, n = 0;
    for (k = 0; k < ijoy_cal_count() && n < IJOY_CAL_MAX; k++) {
        unsigned short v, p;
        int ax;
        ijoy_cal_t c;
        char val[80];
        if (ijoy_cal_entry(k, &v, &p, &ax, &c) && ijoy_cal_format(v, p, ax, &c, val, sizeof val)) {
            _snprintf_s(lines[n], sizeof lines[n], _TRUNCATE, "joy_cal%d=%s", n, val);
            n++;
        }
    }
    return joycal_cfg_apply(user_cfg, lines, n);
}

/* Geraeteinfo aller Geraete (Metadaten, nie Werte): Knopf + Diagnosepaket. */
int joy_devinfo_all(char *out, int cap, int only)
{
    int len = 0, i, k;
    if (cap <= 0) {
        return 0;
    }
    len = _snprintf_s(out, (size_t)cap, _TRUNCATE,
                      "Steelbound device info (no input values)\r\nDirectInput devices: %d\r\n",
                      g_joy_n);
    if (len < 0) { len = 0; }
    for (i = 0; i < g_joy_n && len < cap - 64; i++) {
        const ijoy_obj_t *ob = NULL;
        int no;
        if (only >= 0 && i != only) {
            continue;
        }
        no = g_joy_demo ? 0 : ijoy_objects(i, &ob);
        len += _snprintf_s(out + len, (size_t)(cap - len), _TRUNCATE, "\r\n[device %d]\r\n", i + 1) > 0
                   ? (int)strlen(out + len) : 0;
        len += ijoy_devinfo_text(&g_joy_list[i], ob, no, out + len, cap - len);
    }
    for (k = 0; k < g_joy_nxi && len < cap - 64; k++) {
        int r = _snprintf_s(out + len, (size_t)(cap - len), _TRUNCATE,
                            "\r\nXbox-compatible (XInput, gamepad path): %s\r\n", g_joy_xi[k]);
        len += r > 0 ? r : 0;
    }
    return len;
}

static int joy_clip_u8(HWND hd, const char *u8)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, u8, -1, NULL, 0);
    HGLOBAL hg;
    wchar_t *w;
    if (n <= 0 || !OpenClipboard(hd)) {
        return 0;
    }
    EmptyClipboard();
    hg = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)n * sizeof(wchar_t));
    if (!hg) {
        CloseClipboard();
        return 0;
    }
    w = (wchar_t *)GlobalLock(hg);
    MultiByteToWideChar(CP_UTF8, 0, u8, -1, w, n);
    GlobalUnlock(hg);
    if (!SetClipboardData(CF_UNICODETEXT, hg)) {
        GlobalFree(hg);
        CloseClipboard();
        return 0;
    }
    CloseClipboard();
    return 1;
}

static void joy_cal_status(HWND hd, const char *u8)
{
    ui_settext(bctl(hd, IDJ_CSTAT), u8);
}

/* Achsen-Auswahl fuer die Kalibrierung (nur gelesene Achsen). */
static void joy_cal_fill(HWND hd)
{
    HWND cb = bctl(hd, IDJ_CAX);
    int i = g_joy_dlg_hd ? joy_sel(hd) : -1, a;
    g_updating++;
    ComboBox_ResetContent(cb);
    g_jc_nslots = 0;
    if (i >= 0) {
        for (a = 0; a < IJOY_AXES; a++) {
            if ((g_joy_list[i].axmask >> a) & 1) {
                combo_add(cb, ijoy_axis_name(a));
                g_jc_slots[g_jc_nslots++] = a;
            }
        }
    }
    if (g_jc_nslots > 0) {
        ComboBox_SetCurSel(cb, 0);
    }
    g_updating--;
    g_jc_rec = 0;
    g_jc_dev = i;
    g_jc_ax = g_jc_nslots > 0 ? g_jc_slots[0] : -1;
}

/* Kalibrierung der gewaehlten Achse in die Bedienelemente laden. */
static void joy_cal_select(HWND hd)
{
    int s = ComboBox_GetCurSel(bctl(hd, IDJ_CAX));
    char t[16];
    g_jc_ax = (s >= 0 && s < g_jc_nslots) ? g_jc_slots[s] : -1;
    g_jc_rec = 0;
    ui_settext(bctl(hd, IDJ_CREC), LT("gui.joy_cal_rec"));
    memset(&g_jc, 0, sizeof g_jc);
    g_jc.lo = -32768;
    g_jc.hi = 32767;
    g_jc.mid = 0;
    if (g_jc_dev >= 0 && g_jc_ax >= 0 &&
        ijoy_cal_get(g_joy_list[g_jc_dev].vid, g_joy_list[g_jc_dev].pid, g_jc_ax, &g_jc)) {
        joy_cal_status(hd, LT("gui.joy_cal_have"));
    } else {
        joy_cal_status(hd, g_jc_ax >= 0 ? LT("gui.joy_cal_none") : "");
    }
    _snprintf_s(t, sizeof t, _TRUNCATE, "%d", g_jc.dz);
    ui_settext(bctl(hd, IDJ_CDZ), t);
    ui_set_checked(IDJ_CINV, g_jc.inv);
    ui_set_checked(IDJ_CLIN, g_jc.mid == IJOY_CAL_NOMID);
}

static void joy_cal_cmd(HWND hd, int id)
{
    char t[300], uc[1100];
    int raw;
    if (g_jc_dev < 0 || g_jc_ax < 0) {
        joy_cal_status(hd, LT("gui.joy_cal_noaxis"));
        return;
    }
    raw = g_joy_raw.axis[g_jc_ax];
    if (id == IDJ_CREC) {
        if (!g_jc_rec) {
            g_jc_rec = 1;
            g_jc.lo = g_jc.hi = raw;
            ui_settext(bctl(hd, IDJ_CREC), LT("gui.joy_cal_stop"));
            joy_cal_status(hd, LT("gui.joy_cal_moving"));
        } else {
            g_jc_rec = 0;
            ui_settext(bctl(hd, IDJ_CREC), LT("gui.joy_cal_rec"));
            if (g_jc.hi - g_jc.lo < 2000) {
                joy_cal_status(hd, LT("gui.joy_cal_small"));
            } else {
                joy_cal_status(hd, LT("gui.joy_cal_recdone"));
            }
        }
    } else if (id == IDJ_CMID) {
        if (raw <= g_jc.lo || raw >= g_jc.hi) {
            joy_cal_status(hd, LT("gui.joy_cal_midbad"));
        } else {
            g_jc.mid = raw;
            ui_set_checked(IDJ_CLIN, 0);
            joy_cal_status(hd, LT("gui.joy_cal_midok"));
        }
    } else if (id == IDJ_CINV || id == IDJ_CLIN) {
        ui_set_checked(id, !ui_checked(id));
        if (id == IDJ_CLIN && !ui_checked(IDJ_CLIN) && g_jc.mid == IJOY_CAL_NOMID) {
            g_jc.mid = (g_jc.lo + g_jc.hi) / 2;
        }
    } else if (id == IDJ_CSAVE || id == IDJ_CRESET) {
        int ok;
        if (id == IDJ_CSAVE) {
            ijoy_cal_t c = g_jc;
            ui_gettext(bctl(hd, IDJ_CDZ), t, sizeof t);
            c.dz = atoi(t);
            if (c.dz < 0) { c.dz = 0; }
            if (c.dz > 50) { c.dz = 50; }
            c.inv = ui_checked(IDJ_CINV);
            if (ui_checked(IDJ_CLIN)) {
                c.mid = IJOY_CAL_NOMID;
            } else if (c.mid <= c.lo || c.mid >= c.hi) {
                c.mid = (c.lo + c.hi) / 2;
            }
            if (c.hi - c.lo < 2000) {
                joy_cal_status(hd, LT("gui.joy_cal_small"));
                return;
            }
            g_jc_rec = 0;
            ui_settext(bctl(hd, IDJ_CREC), LT("gui.joy_cal_rec"));
            g_jc = c;
            ok = ijoy_cal_set(g_joy_list[g_jc_dev].vid, g_joy_list[g_jc_dev].pid, g_jc_ax, &c) == 0;
        } else {
            ijoy_cal_set(g_joy_list[g_jc_dev].vid, g_joy_list[g_jc_dev].pid, g_jc_ax, NULL);
            ok = 1;
        }
        if (ok && !g_joy_demo) {
            lcfg_user_cfg_path(g_client_dir, uc, sizeof uc);
            ok = joy_cal_store(uc) == 0;
        }
        if (id == IDJ_CRESET) {
            joy_cal_select(hd);
        }
        joy_cal_status(hd, LT(!ok ? "gui.joy_cal_err" : id == IDJ_CSAVE ? "gui.joy_cal_saved"
                                                                     : "gui.joy_cal_reset"));
    }
}

static void joy_refresh_info(HWND hd)
{
    char t[320];
    int i = joy_sel(hd);
    if (i < 0) {
        int k = joy_sel_xi(hd);
        t[0] = 0;
        if (k >= 0) {
            _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.joy_xinput"), g_joy_xi[k]);
        }
        ui_settext(bctl(hd, IDJ_INFO), t);
        g_joy_have = 0;
    } else {
        int na = 0, b;
        for (b = 0; b < IJOY_AXES; b++) {
            na += (g_joy_list[i].axmask >> b) & 1;
        }
        _snprintf_s(t, sizeof t, _TRUNCATE, "%s\r\n", g_joy_list[i].product[0]
                    ? g_joy_list[i].product : g_joy_list[i].name);
        _snprintf_s(t + strlen(t), sizeof t - strlen(t), _TRUNCATE,
                    LT("gui.joy_info"), na, g_joy_list[i].nbtn,
                    g_joy_list[i].npov);
        if (g_joy_list[i].ncaps_axes > na) { /* W282: Treiber meldet mehr */
            _snprintf_s(t + strlen(t), sizeof t - strlen(t), _TRUNCATE,
                        LT("gui.joy_info_drv"), g_joy_list[i].ncaps_axes);
        }
        ui_settext(bctl(hd, IDJ_INFO), t);
        if (!g_joy_demo) {
            g_joy_have = ijoy_read2(i, &g_joy_st, &g_joy_raw); /* W312 */
        }
    }
    if (i != g_jc_dev || (i < 0 && g_jc_nslots)) { /* W312: Kalibrier-Achsen */
        joy_cal_fill(hd);
        joy_cal_select(hd);
    }
    {
        RECT r = { SX(JOY_LIVE_X), SX(JOY_LIVE_Y), SX(JOY_LIVE_X + JOY_LIVE_W),
                   SX(JOY_LIVE_Y + JOY_LIVE_H) };
        InvalidateRect(hd, &r, FALSE);
    }
}

static void joy_rescan(HWND hd)
{
    HWND lb = bctl(hd, IDJ_LIST);
    int i;
    ListBox_ResetContent(lb);
    if (g_joy_demo) {
        joy_demo_fill();
    } else {
        g_joy_ok = ijoy_init() == 0;
        if (g_joy_ok) {
            joy_cal_load(); /* W312 */
        }
        g_jc_dev = -1;
        g_joy_n = g_joy_ok ? ijoy_scan() : 0;
        if (g_joy_n > IJOY_MAX_DEV) {
            g_joy_n = IJOY_MAX_DEV;
        }
        for (i = 0; i < g_joy_n; i++) {
            const ijoy_info_t *in = ijoy_info(i);
            if (in) {
                g_joy_list[i] = *in;
            } else {
                memset(&g_joy_list[i], 0, sizeof g_joy_list[i]);
            }
        }
    }
    for (i = 0; i < g_joy_n; i++) {
        char e[128];
        _snprintf_s(e, sizeof e, _TRUNCATE, "%d: %s", i + 1,
                    g_joy_list[i].name[0] ? g_joy_list[i].name
                                          : g_joy_list[i].product);
        list_add(lb, e, (LPARAM)i, 1);
    }
    /* W282: uebersprungene Xbox-kompatible Geraete mit Hinweis listen */
    if (g_joy_demo) {
        strcpy_s(g_joy_xi[0], sizeof g_joy_xi[0], "Example Flight Stick");
        g_joy_nxi = 1;
    } else {
        g_joy_nxi = g_joy_ok ? ijoy_skipped_xinput(g_joy_xi, 8) : 0;
        if (g_joy_nxi > 8) {
            g_joy_nxi = 8;
        }
    }
    for (i = 0; i < g_joy_nxi; i++) {
        char e[160];
        _snprintf_s(e, sizeof e, _TRUNCATE, LT("gui.joy_xinput"), g_joy_xi[i]);
        list_add(lb, e, (LPARAM)(1000 + i), 1);
    }
    if (g_joy_n == 0 && g_joy_nxi == 0) {
        list_add(lb, LT(g_joy_ok || g_joy_demo ? "gui.joy_none" : "gui.joy_nodi"),
                 (LPARAM)-1, 1);
    }
    ListBox_SetCurSel(lb, 0);
    joy_refresh_info(hd);
}

static void joy_text(HDC dc, int font, COLORREF c, int x, int y, int w, int h,
                     const char *u8, UINT fl)
{
    wchar_t *wt = u8_wdup(u8);
    RECT r = { SX(x), SX(y), SX(x + w), SX(y + h) };
    SelectObject(dc, g_f[font]);
    SetTextColor(dc, c);
    DrawTextW(dc, wt ? wt : L"", -1, &r, (fl & DT_WORDBREAK) /* W282: mehrzeilig */
              ? (fl | DT_NOPREFIX)
              : (fl | DT_NOPREFIX | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS));
    free(wt);
}

static void joy_fill(HDC dc, int x, int y, int w, int h, COLORREF c)
{
    RECT r = { SX(x), SX(y), SX(x) + MulDiv(w, g_dpi, 96), SX(y + h) };
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

/* Live-Anzeige (Koordinaten relativ zum Dialog, Ursprung JOY_LIVE_X/Y). */
static void joy_paint_live(HDC dc)
{
    static const char *const dk[4] = { "gui.joy_dir_u", "gui.joy_dir_r",
                                       "gui.joy_dir_d", "gui.joy_dir_l" };
    int i = -1, a, y, X = JOY_LIVE_X, Y = JOY_LIVE_Y;
    char t[400];
    if (g_joy_dlg_hd) {
        i = joy_sel(g_joy_dlg_hd);
    }
    SetBkMode(dc, TRANSPARENT);
    joy_text(dc, F_SEMI, C_TEXT, X, Y, JOY_LIVE_W, 22, LT("gui.joy_live"), DT_LEFT);
    if (i < 0) {
        if (g_joy_dlg_hd && joy_sel_xi(g_joy_dlg_hd) >= 0) { /* W282 */
            joy_text(dc, F_SMALL, C_DIM, X, Y + 26, JOY_LIVE_W, 240,
                     LT("gui.joy_xinput_info"), DT_LEFT | DT_WORDBREAK);
        }
        return;
    }
    if (!g_joy_have) {
        joy_text(dc, F_SMALL, C_WARN, X, Y + 26, JOY_LIVE_W, 20,
                 LT("gui.joy_lost"), DT_LEFT);
        return;
    }
    for (a = 0; a < IJOY_AXES; a++) {
        int on = (g_joy_list[i].axmask >> a) & 1;
        int pct = ((int)g_joy_st.axis[a] + 32768) * 100 / 65535;
        int rpct = ((int)g_joy_raw.axis[a] + 32768) * 100 / 65535; /* W312 */
        int bw = 300;
        y = Y + 28 + a * 24;
        joy_text(dc, F_SMALL, on ? (a == g_jc_ax ? C_ACCENT_HI : C_TEXT) : C_FAINT, X, y, 34, 18,
                 ijoy_axis_name(a), DT_LEFT);
        joy_fill(dc, X + 38, y + 3, bw, 12, on ? C_INPUT_BRD : C_PANEL);
        if (on) {
            joy_fill(dc, X + 39, y + 4, (bw - 2) * pct / 100, 10, C_ACCENT);
            joy_fill(dc, X + 38 + bw / 2, y + 1, 1, 16, C_TEXT); /* Mitte */
            /* W312: Rohstellung (weisse Marke) und Kalibriergrenzen */
            joy_fill(dc, X + 38 + (bw - 2) * rpct / 100, y, 2, 18, C_TEXT);
            if (a == g_jc_ax) {
                int lp = (g_jc.lo + 32768) * 100 / 65535, hp = (g_jc.hi + 32768) * 100 / 65535;
                joy_fill(dc, X + 38 + (bw - 2) * lp / 100, y - 1, 2, 20, C_WARN);
                joy_fill(dc, X + 38 + (bw - 2) * hp / 100, y - 1, 2, 20, C_WARN);
                if (g_jc.mid != IJOY_CAL_NOMID && !ui_checked(IDJ_CLIN)) {
                    int mp = (g_jc.mid + 32768) * 100 / 65535;
                    joy_fill(dc, X + 38 + (bw - 2) * mp / 100, y - 1, 1, 20, C_ACCENT_HI);
                }
            }
            if (rpct != pct) {
                _snprintf_s(t, sizeof t, _TRUNCATE, "%d " "\xE2\x86\x92" " %d %%", rpct, pct);
            } else {
                _snprintf_s(t, sizeof t, _TRUNCATE, "%d %%", pct);
            }
        } else {
            strcpy_s(t, sizeof t, "-");
        }
        joy_text(dc, F_SMALL, on ? C_DIM : C_FAINT, X + 346, y, 124, 18, t,
                 DT_RIGHT);
    }
    y = Y + 28 + IJOY_AXES * 24 + 6;
    {
        size_t o;
        int b, any = 0;
        o = (size_t)_snprintf_s(t, sizeof t, _TRUNCATE, "%s ", LT("gui.joy_btns"));
        for (b = 0; b < IJOY_BTNS && o < sizeof t - 8; b++) {
            if ((g_joy_st.btn[b >> 5] >> (b & 31)) & 1u) {
                o += (size_t)_snprintf_s(t + o, sizeof t - o, _TRUNCATE,
                                         "%s%d", any ? " " : "", b + 1);
                any = 1;
            }
        }
        if (!any) {
            strcat_s(t, sizeof t, LT("gui.joy_btns_none"));
        }
        joy_text(dc, F_SMALL, any ? C_ACCENT_HI : C_DIM, X, y, JOY_LIVE_W, 20,
                 t, DT_LEFT);
    }
    for (a = 0; a < g_joy_list[i].npov && a < IJOY_POVS; a++) {
        int p = g_joy_st.pov[a];
        y += 22;
        if (p >= 0 && p < 36000) {
            _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.joy_pov"), a + 1);
            _snprintf_s(t + strlen(t), sizeof t - strlen(t), _TRUNCATE,
                        " %d" "\xC2\xB0" " (%s)", p / 100,
                        LT(dk[((p + 4500) / 9000) % 4]));
        } else {
            _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.joy_pov"), a + 1);
            _snprintf_s(t + strlen(t), sizeof t - strlen(t), _TRUNCATE,
                        " %s", LT("gui.joy_pov_c"));
        }
        joy_text(dc, F_SMALL, p >= 0 ? C_ACCENT_HI : C_DIM, X, y, JOY_LIVE_W,
                 20, t, DT_LEFT);
    }
    { /* W312: Objekte, die der Treiber meldet, aber nicht gelesen werden */
        const ijoy_obj_t *ob = NULL;
        int no = g_joy_demo ? 0 : ijoy_objects(i, &ob), o2, any = 0;
        size_t L = (size_t)_snprintf_s(t, sizeof t, _TRUNCATE, "%s ", LT("gui.joy_unread"));
        for (o2 = 0; o2 < no && L < sizeof t - 8; o2++) {
            if (ob[o2].read < 0 && ob[o2].kind != IJOY_K_OTHER) {
                L += (size_t)_snprintf_s(t + L, sizeof t - L, _TRUNCATE, "%s%s", any ? ", " : "",
                                         ob[o2].name[0] ? ob[o2].name : "?");
                any = 1;
            }
        }
        if (any) {
            joy_text(dc, F_SMALL, C_WARN, X, y + 22, JOY_LIVE_W, 20, t, DT_LEFT);
        }
    }
    if (g_joy_demo) {
        joy_text(dc, F_TINY, C_FAINT, X, Y + JOY_LIVE_H - 18, JOY_LIVE_W, 16,
                 LT("gui.joy_demo"), DT_LEFT);
    }
}

static LRESULT CALLBACK joy_proc(HWND hd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (id == IDJ_CLOSE || id == IDCANCEL) {
            g_joy_done = 1;
        } else if (id == IDJ_SCAN) {
            joy_rescan(hd);
        } else if (id == IDJ_LIST && code == LBN_SELCHANGE) {
            joy_refresh_info(hd);
        } else if (id == IDJ_COPY) { /* W312: Geraeteinfo (ohne Werte) */
            static char info[32768];
            joy_devinfo_all(info, (int)sizeof info, -1);
            joy_cal_status(hd, LT(joy_clip_u8(hd, info) ? "gui.joy_copy_ok" : "gui.joy_copy_err"));
        } else if (id == IDJ_CAX && code == CBN_SELCHANGE && !g_updating) {
            joy_cal_select(hd);
        } else if (id == IDJ_CREC || id == IDJ_CMID || id == IDJ_CINV || id == IDJ_CLIN ||
                   id == IDJ_CSAVE || id == IDJ_CRESET) {
            joy_cal_cmd(hd, id);
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
    case WM_ERASEBKGND:
        return 1; /* WM_PAINT fuellt (doppelt gepuffert) */
    case WM_PAINT: {
        PAINTSTRUCT ps;
        RECT r;
        HDC dc = BeginPaint(hd, &ps), mem;
        HBITMAP bm, old;
        GetClientRect(hd, &r);
        mem = CreateCompatibleDC(dc);
        bm = CreateCompatibleBitmap(dc, r.right, r.bottom);
        old = (HBITMAP)SelectObject(mem, bm);
        if (!g_br_dialog) {
            g_br_dialog = CreateSolidBrush(C_DIALOG);
        }
        FillRect(mem, &r, g_br_dialog);
        joy_paint_live(mem);
        BitBlt(dc, 0, 0, r.right, r.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bm);
        DeleteDC(mem);
        EndPaint(hd, &ps);
        return 0;
    }
    case WM_TIMER:
        if (wp == JOY_TIMER && !g_joy_demo) {
            int i = joy_sel(hd);
            if (i >= 0) {
                RECT r = { SX(JOY_LIVE_X), SX(JOY_LIVE_Y),
                           SX(JOY_LIVE_X + JOY_LIVE_W),
                           SX(JOY_LIVE_Y + JOY_LIVE_H) };
                g_joy_have = ijoy_read2(i, &g_joy_st, &g_joy_raw); /* W312 */
                if (g_joy_have && g_jc_rec && g_jc_ax >= 0 && i == g_jc_dev) {
                    int v = g_joy_raw.axis[g_jc_ax]; /* nur Anzeige/Kalibrierung */
                    if (v < g_jc.lo) { g_jc.lo = v; }
                    if (v > g_jc.hi) { g_jc.hi = v; }
                }
                InvalidateRect(hd, &r, FALSE);
            }
        } else if (wp == 9 && g_o.shot == 6) {
            char path[1200];
            int rc;
            KillTimer(hd, 9);
            RedrawWindow(hd, NULL, NULL,
                         RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
            _snprintf_s(path, sizeof path, _TRUNCATE, g_joy_calshot ? "%s/w312_joycal_%d.png"
                                                                    : "%s/w221_hotas_%d.png",
                        g_o.out_dir, g_dpi * 100 / 96);
            rc = shot_window(hd, path, 1);
            printf("joy-shot: %s%s\n", rc == 0 ? "" : "FEHLER ", path);
            g_joy_done = 1;
        }
        return 0;
    case WM_CLOSE:
        g_joy_done = 1;
        return 0;
    }
    return DefWindowProcW(hd, msg, wp, lp);
}

void joy_dialog_modal(void)
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
        wc.lpfnWndProc = joy_proc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = L"SbJoy";
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        RegisterClassW(&wc);
        reg = 1;
    }
    g_joy_done = 0;
    g_joy_demo = g_o.shot == 6;
    g_joy_calshot = g_joy_demo && g_o.joy_cal; /* W312 */
    g_joy_have = 0;
    rc.right = SX(760);  /* W312: Platz fuer Kalibrierung (vorher 640x460) */
    rc.bottom = SX(620);
    AdjustWindowRectEx(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, exs);
    if (shot) {
        offscreen_pos(&x, &y);
    } else if (g_hwnd) {
        RECT pr;
        GetWindowRect(g_hwnd, &pr);
        x = pr.left + ((pr.right - pr.left) - (rc.right - rc.left)) / 2;
        y = pr.top + ((pr.bottom - pr.top) - (rc.bottom - rc.top)) / 2;
    }
    {
        wchar_t title[96];
        u8_to_w(LT("gui.joy_title"), title, 96);
        hd = CreateWindowExW(exs, L"SbJoy", title,
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                                 WS_CLIPCHILDREN,
                             x, y, rc.right - rc.left, rc.bottom - rc.top,
                             g_hwnd, NULL, GetModuleHandleW(NULL), NULL);
    }
    if (!hd) {
        return;
    }
    g_joy_dlg_hd = hd;
    {
        BOOL dark = TRUE;
        DwmSetWindowAttribute(hd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */,
                              &dark, sizeof dark);
    }
    mk(hd, K_STATIC, IDJ_EXP, PG_DIALOG, 12, 10, 736, 52, LT("gui.joy_exp"), WS_VISIBLE);
    mk(hd, K_STATIC, IDJ_DEVH, PG_DIALOG, 12, 72, 250, 18, LT("gui.joy_devices"), WS_VISIBLE);
    mk(hd, K_LIST, IDJ_LIST, PG_DIALOG, 12, 94, 250, 300, "", WS_VISIBLE);
    mk(hd, K_BTN, IDJ_SCAN, PG_DIALOG, 12, 404, 120, 30, LT("gui.joy_scan"), WS_VISIBLE);
    mk(hd, K_BTN, IDJ_COPY, PG_DIALOG, 138, 404, 124, 30, LT("gui.joy_copy"), WS_VISIBLE); /* W312 */
    mk(hd, K_STATIC, IDJ_INFO, PG_DIALOG, 274, 72, 474, 40, "", WS_VISIBLE);
    /* W312 JOY-CALIB: Kalibrierung der gewaehlten Achse */
    mk(hd, K_STATIC, IDJ_CAXL, PG_DIALOG, 12, 464, 120, 22, LT("gui.joy_cal_axis"), WS_VISIBLE);
    mk(hd, K_COMBO, IDJ_CAX, PG_DIALOG, 136, 458, 80, 30, "", WS_VISIBLE);
    mk(hd, K_BTN, IDJ_CREC, PG_DIALOG, 226, 458, 170, 30, LT("gui.joy_cal_rec"), WS_VISIBLE);
    mk(hd, K_BTN, IDJ_CMID, PG_DIALOG, 404, 458, 130, 30, LT("gui.joy_cal_mid"), WS_VISIBLE);
    mk(hd, K_STATIC, IDJ_CDZL, PG_DIALOG, 544, 464, 120, 22, LT("gui.joy_cal_dz"), WS_VISIBLE);
    mk(hd, K_EDIT, IDJ_CDZ, PG_DIALOG, 668, 460, 60, 26, "0", WS_VISIBLE);
    mk(hd, K_TOGGLE, IDJ_CINV, PG_DIALOG, 12, 500, 160, 28, LT("gui.joy_cal_inv"), WS_VISIBLE);
    mk(hd, K_TOGGLE, IDJ_CLIN, PG_DIALOG, 180, 500, 300, 28, LT("gui.joy_cal_lin"), WS_VISIBLE);
    mk(hd, K_BTN, IDJ_CSAVE, PG_DIALOG, 488, 498, 126, 30, LT("gui.joy_cal_save"), WS_VISIBLE);
    mk(hd, K_BTN, IDJ_CRESET, PG_DIALOG, 622, 498, 126, 30, LT("gui.joy_cal_reset_btn"), WS_VISIBLE);
    mk(hd, K_STATIC, IDJ_CSTAT, PG_DIALOG, 12, 540, 600, 40, "", WS_VISIBLE);
    mk(hd, K_BTN, IDJ_CLOSE, PG_DIALOG, 618, 578, 130, 30, LT("gui.btn_close"), WS_VISIBLE);
    g_jc_dev = -1;
    joy_rescan(hd);
    if (shot && g_joy_calshot) { /* W312: Kalibrierzustand mit Beispielwerten */
        g_jc_rec = 1;
        g_jc.lo = -26000;
        g_jc.hi = 30500;
        g_jc.mid = 1500;
        ui_settext(bctl(hd, IDJ_CREC), LT("gui.joy_cal_stop"));
        ui_settext(bctl(hd, IDJ_CDZ), "5");
        joy_cal_status(hd, LT("gui.joy_cal_moving"));
    }
    if (g_hwnd) {
        EnableWindow(g_hwnd, FALSE);
    }
    if (shot) {
        if (!is_offscreen(hd)) {
            printf("joy-shot: FEHLER Fenster nicht au" U8_SZ
                   "erhalb des Bildschirms - Abbruch\n");
            g_joy_done = 1;
        } else {
            ShowWindow(hd, SW_SHOWNOACTIVATE);
            SetTimer(hd, 9, 400, NULL);
        }
    } else {
        ShowWindow(hd, SW_SHOW);
        SetFocus(bctl(hd, IDJ_LIST));
        SetTimer(hd, JOY_TIMER, 50, NULL); /* Live-Anzeige nur solange offen */
    }
    UpdateWindow(hd);
    while (!g_joy_done && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (IsDialogMessageW(hd, &msg)) {
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    KillTimer(hd, JOY_TIMER);
    if (g_hwnd) {
        EnableWindow(g_hwnd, TRUE);
        if (!shot) {
            SetFocus(ctl(ID_CTRL_JOY));
        }
    }
    DestroyWindow(hd);
    g_joy_dlg_hd = NULL;
    g_ui_n = ui_mark;
}

/* ------------------------------------------------------------------ */
