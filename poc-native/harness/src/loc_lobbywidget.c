/* loc_lobbywidget.c - W460 LOC-LOBBY-WIDGET (GitHub #95): Widget "offene Lobbys" (Beschreibung in loc_lobbywidget.h).
 * Zeichnung wie das Lobby-Infopanel (W453, loc_netpanel.c): GDI-Text in eine kleine Textur, ein Quad nach
 * Clip/Video/F11-Bild, Bereich gesichert, Werte aus overlay_style.h. Keine Eingriffe ins Spiel, keine Eingaben. */
#include "loc_tun.h"
#include "render_int.h"
#include "sb_overlay.h"
#include "overlay_style.h" /* W461 OVERLAY-CORE */
#include "sb_nethud.h"
#include "sb_lang.h"
#include "loc_net.h"
#include "loc_net_core.h"
#include "loc_assist.h"
#include "loc_lobbywidget.h"
#include "hlog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef uint32_t u32;

#define LW_COLS 64
#define LW_PING_OK 80   /* wie Lobby-Infopanel (W453) */
#define LW_PING_MID 150
#define LW_SIM_BG OVS_RGB_SIM_BG       /* Marken wie die Suchliste (W443), Werte aus overlay_style (W467) */
#define LW_ARCADE_BG OVS_RGB_ARCADE_BG

/* Spielspeicher (LoC-XBE, W460-Abzuege p3/p4): Szene u16 [0x727988] (W344), Menue-Struktur der Szene 8 ab 0x6DC678:
 * +0x14 (0x6DC68C) Text-Id der ersten Zeile, [0x6DD2B0] Zahl der Menuezeilen (loc_lobbywidget.h lw_screen_from). */
#define LW_SCENE 0x727988u
#define LW_TEXT_ID 0x6DC68Cu
#define LW_NITEMS 0x6DD2B0u

/* ------------------------------------------------------------------ Schnappschuss (Spiel-Thread -> Render-Thread) */
typedef struct lw_snap {
    int show;               /* Seite aktiv und Schalter an */
    int state;              /* >= 0 Liste, sonst LW_SRC_* */
    int total, n;
    lw_entry row[LW_MAX_ROWS];
    u32 t, gen;
} lw_snap;

static SRWLOCK s_lock = SRWLOCK_INIT;
static lw_snap s_snap;
static volatile LONG s_shot_req, s_shot_n;
static const lw_source *s_src;

/* ------------------------------------------------------------------ Standardquelle: steelnet (Transport) */
static loctun *lw_tun(void)
{
    locnet *ln = loc_net_core();
    return ln ? ln->tun : NULL;
}

static void sn_watch(int on)
{
    loctun *t = lw_tun();
    if (t) loctun_pub_watch(t, on);
}

static int sn_fetch(lw_entry *out, int cap, int *total)
{
    loctun_pub_entry e[LW_MAX_ROWS];
    loctun *t = lw_tun();
    int n, k;
    *total = 0;
    if (!t) return LW_SRC_OFF;
    if (cap > LW_MAX_ROWS) cap = LW_MAX_ROWS;
    n = loctun_pub_list(t, e, cap, total);
    if (n == LOCTUN_PL_WAIT) return LW_SRC_WAIT;
    if (n < 0) return g_cfg.loc_lobby[0] && g_cfg.loc_lobby_code[0] ? LW_SRC_CODE : LW_SRC_OFF;
    for (k = 0; k < n; k++) {
        memset(&out[k], 0, sizeof out[k]);
        out[k].mode = e[k].mode;
        out[k].est = e[k].est;
        out[k].cur = e[k].cur;
        out[k].max = e[k].max;
        if (!MultiByteToWideChar(CP_UTF8, 0, e[k].pilot, -1, out[k].name, 24)) wcscpy_s(out[k].name, 24, L"?");
        if (wcsncmp(out[k].name, L"Guest-", 6) == 0 && out[k].name[6]) { /* Gast wie in der Suchliste: "~Name" */
            memmove(out[k].name + 1, out[k].name + 6, (wcslen(out[k].name + 6) + 1) * sizeof(wchar_t));
            out[k].name[0] = L'~';
        }
    }
    return n;
}

static const lw_source kSteelnet = { "steelnet", sn_watch, sn_fetch };

void loc_lobbywidget_set_source(const lw_source *src)
{
    if (s_src && s_src != src && s_src->watch) s_src->watch(0);
    s_src = src;
}

/* ------------------------------------------------------------------ Seitenerkennung */
static int lw_screen(int *scene, int *tid, int *nit)
{
    *scene = *tid = *nit = -1;
    __try {
        *scene = *(const uint16_t *)(uintptr_t)LW_SCENE;
        *tid = (int)*(const uint32_t *)(uintptr_t)LW_TEXT_ID;
        *nit = *(const uint8_t *)(uintptr_t)LW_NITEMS;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return lw_screen_from(*scene, *tid, *nit);
}

/* ------------------------------------------------------------------ Spiel-Thread */
void loc_lobbywidget_frame(unsigned f)
{
    static int s_watch = -1, s_log[4] = { -9, -9, -9, -9 };
    const lw_source *src = s_src ? s_src : &kSteelnet;
    lw_snap n;
    int scr, on, scene, tid, nit;
    if (!g_sba_loc) return;
    if ((f % 15u) != 0u) return; /* 2x/s bei 30 Spielbildern (Menue) */
    memset(&n, 0, sizeof n);
    scr = lw_screen(&scene, &tid, &nit);
    if (g_cfg.loc_lobby_widget_diag && scene == 8) { /* DIAG: Seitenwerte bei Aenderung (nur Zahlen) */
        static int pt = -2, pn = -2;
        if (tid != pt || nit != pn) {
            pt = tid;
            pn = nit;
            HLOG_I("LOCWIDGET diag: Szene 8 Text-Id 0x%X Zeilen %d -> Seite %d (f=%u)", (unsigned)tid, nit, scr, f);
        }
    }
    if (g_cfg.loc_lobby_widget_test == 2) scr = 1; /* TEST: Lage ohne Menueerkennung pruefen */
    on = g_cfg.loc_lobby_widget && scr != 0;
    if (on != s_watch) {
        if (src->watch) src->watch(on);
        s_watch = on;
    }
    n.show = on;
    n.state = LW_SRC_OFF;
    if (on) {
        n.state = src->fetch ? src->fetch(n.row, LW_MAX_ROWS, &n.total) : LW_SRC_OFF;
        n.n = n.state > 0 ? n.state : 0;
        if (n.state >= 0 && n.total < n.n) n.total = n.n;
    }
    { /* Log nur bei Aenderung, nur Zahlen */
        int cur[4] = { scr, on, n.state < 0 ? n.state : 0, n.state >= 0 ? n.total : -1 };
        if (memcmp(cur, s_log, sizeof cur) != 0) {
            memcpy(s_log, cur, sizeof cur);
            HLOG_I("LOCWIDGET: screen=%d show=%d state=%d open=%d rows=%d src=%s f=%u", cur[0], cur[1], cur[2], cur[3], n.n,
                   src->name, f);
        }
    }
    AcquireSRWLockExclusive(&s_lock);
    n.gen = s_snap.gen;
    n.t = s_snap.t;
    if (memcmp(&n, &s_snap, sizeof n) != 0) n.gen++;
    n.t = GetTickCount();
    s_snap = n;
    ReleaseSRWLockExclusive(&s_lock);
}

/* ------------------------------------------------------------------ Anzeige (Render-Thread) */
static ovs_canvas_t s_cv;
static ovs_font_t s_fn[3];
static ovs_tex_t s_tx;
static unsigned *s_pm;
static unsigned s_w, s_h, s_pmcap;
static sb_ovl_save_t s_save;
static u32 s_drawn_gen, s_drawn_box, s_shot_t;
static int s_tex_dirty, s_have;
static unsigned s_ox, s_oy;

int loc_lobbywidget_visible(void)
{
    int a;
    u32 t;
    if (!g_cfg.loc_lobby_widget || !g_sba_loc || g_cfg.vr_enable || sb_ovl_open()) return 0; /* W474: F3 blendet nicht aus */
    AcquireSRWLockShared(&s_lock);
    a = s_snap.show;
    t = s_snap.t;
    ReleaseSRWLockShared(&s_lock);
    return a && (u32)(GetTickCount() - t) < 2000u; /* Spiel-Thread steht (Ladebild): nicht stehen lassen */
}

static unsigned lw_ping_col(int p)
{
    return p <= 0 ? OVS_RGB_DIM : p <= LW_PING_OK ? OVS_RGB_OK : p <= LW_PING_MID ? OVS_RGB_MID : OVS_RGB_BAD;
}

static void lw_lw(const char *key, wchar_t *o, int cap)
{
    if (sb_lang_w(key, o, cap) != 0) o[0] = 0;
}

typedef struct lw_item {
    RECT r;
    unsigned col, bg;
    float bga;
    int font;  /* 0 Kopf, 1 Zeile, 2 Marke */
    UINT fmt;
    wchar_t txt[LW_COLS];
} lw_item;

#define LW_W 190.0f    /* Breite im Menueraum 640x480 */
#define LW_PAD OVS_LP_PAD   /* W467: Masse des gemeinsamen LoC-Panels */
#define LW_HEAD OVS_LP_HEAD
#define LW_LH OVS_LP_LH
#define LW_X_MODE 88.0f /* Spalten (Einheiten ab linkem Rand) */
#define LW_X_PL 128.0f
#define LW_X_PING 152.0f
#define LW_ALPHA OVS_LP_ALPHA /* Deckung aller LoC-Panels (= ESC-Menue) */

static lw_item *lw_add(lw_item *it, int *ni, int cap, float fy, float x0, float x1, float y0, float y1, unsigned col,
                       int font, UINT fmt)
{
    lw_item *p;
    if (*ni >= cap) return NULL;
    p = &it[(*ni)++];
    memset(p, 0, sizeof *p);
    p->r.left = ovs_px(fy, x0);
    p->r.right = ovs_px(fy, x1);
    p->r.top = ovs_px(fy, y0);
    p->r.bottom = ovs_px(fy, y1);
    p->col = col;
    p->bg = OVS_RGB_PANEL;
    p->bga = LW_ALPHA;
    p->font = font;
    p->fmt = fmt | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX;
    return p;
}

/* Inhalt (s_w x s_h, vormultipliziertes BGRA). Groesse in Einheiten des Menueraums (f = Boxhoehe / 480). 1 = ok. */
static int lw_render(const lw_snap *sn, float fy)
{
    lw_item it[4 + LW_MAX_ROWS * 4 + 2];
    int ni = 0, k, nrow;
    unsigned w, h, lw;
    float yb;
    HFONT fn[3], fold;
    HDC dc;
    wchar_t fmt[LW_COLS];
    lw_item *p;
    nrow = sn->state > 0 ? sn->n + (sn->total > sn->n ? 1 : 0) : 1;
    w = (unsigned)ovs_px(fy, LW_W);
    h = (unsigned)ovs_px(fy, LW_PAD * 2.0f + LW_HEAD + LW_LH * (float)nrow);
    /* Kopfzeile: Titel und Zahl */
    lw_lw("ovl.lw_title", fmt, LW_COLS);
    p = lw_add(it, &ni, 32, fy, LW_PAD + OVS_LP_INSET, LW_W - LW_PAD, LW_PAD - 1.0f, LW_PAD + LW_HEAD - 2.0f, OVS_RGB_ACCENT, 0,
               DT_LEFT);
    if (p) {
        if (sn->state >= 0) _snwprintf_s(p->txt, LW_COLS, _TRUNCATE, fmt, sn->total);
        else {
            wchar_t *c;
            wcscpy_s(p->txt, LW_COLS, fmt);
            if ((c = wcsstr(p->txt, L"%d")) != NULL) { /* ohne Liste: Titel ohne Zahl */
                *c = 0;
                while (c > p->txt && (c[-1] == L' ' || c[-1] == L'(')) *--c = 0;
            }
        }
    }
    yb = LW_PAD + LW_HEAD;
    if (sn->state <= 0) { /* leer bzw. Hinweis */
        const char *key = sn->state == 0 ? "ovl.lw_empty" : sn->state == LW_SRC_WAIT ? "ovl.lw_wait" :
                          sn->state == LW_SRC_CODE ? "ovl.lw_code" : "ovl.lw_off";
        p = lw_add(it, &ni, 32, fy, LW_PAD + OVS_LP_INSET, LW_W - LW_PAD, yb, yb + LW_LH, sn->state == 0 ? OVS_RGB_TEXT : OVS_RGB_DIM,
                   1, DT_LEFT | DT_END_ELLIPSIS);
        if (p) lw_lw(key, p->txt, LW_COLS);
    } else {
        for (k = 0; k < sn->n; k++) {
            float y0 = yb + LW_LH * (float)k, y1 = y0 + LW_LH;
            const lw_entry *e = &sn->row[k];
            p = lw_add(it, &ni, 32, fy, LW_PAD + OVS_LP_INSET, LW_X_MODE - 4.0f, y0, y1, OVS_RGB_TEXT, 1, DT_LEFT | DT_END_ELLIPSIS);
            if (p) wcscpy_s(p->txt, LW_COLS, e->name);
            if (e->mode == LAS_SIM || e->mode == LAS_ARCADE) {
                p = lw_add(it, &ni, 32, fy, LW_X_MODE, LW_X_PL - 4.0f, y0 + 1.0f, y1 - 1.0f, OVS_RGB_TAG_TXT, 2, DT_CENTER);
                if (p) {
                    wcscpy_s(p->txt, LW_COLS, e->mode == LAS_ARCADE ? L"ARCADE" : L"SIM");
                    p->bg = e->mode == LAS_ARCADE ? LW_ARCADE_BG : LW_SIM_BG;
                    p->bga = OVS_TAG_ALPHA;
                }
            }
            p = lw_add(it, &ni, 32, fy, LW_X_PL, LW_X_PING - 2.0f, y0, y1, OVS_RGB_TEXT, 1, DT_CENTER);
            if (p) {
                if (e->max > 0) _snwprintf_s(p->txt, LW_COLS, _TRUNCATE, L"%d/%d", e->cur, e->max);
                else wcscpy_s(p->txt, LW_COLS, L"-");
            }
            p = lw_add(it, &ni, 32, fy, LW_X_PING, LW_W - LW_PAD, y0, y1, lw_ping_col(e->est), 1, DT_RIGHT);
            if (p) {
                if (e->est > 0) _snwprintf_s(p->txt, LW_COLS, _TRUNCATE, L"~%d ms", e->est > 999 ? 999 : e->est);
                else wcscpy_s(p->txt, LW_COLS, L"--");
            }
        }
        if (sn->total > sn->n) {
            float y0 = yb + LW_LH * (float)sn->n;
            lw_lw("ovl.lw_more", fmt, LW_COLS);
            p = lw_add(it, &ni, 32, fy, LW_PAD + OVS_LP_INSET, LW_W - LW_PAD, y0, y0 + LW_LH, OVS_RGB_DIM, 1, DT_LEFT);
            if (p) _snwprintf_s(p->txt, LW_COLS, _TRUNCATE, fmt, sn->total - sn->n);
        }
    }
    if (w < 8u || h < 8u) return 0;
    dc = ovs_canvas_dc(&s_cv);
    if (!dc) return 0;
    fn[0] = ovs_font_get(&s_fn[0], OVS_FACE_COND, FW_SEMIBOLD, ovs_px(fy, OVS_LP_FS_HEAD));
    fn[1] = ovs_font_get(&s_fn[1], OVS_FACE_COND, FW_NORMAL, ovs_px(fy, OVS_LP_FS_ROW));
    fn[2] = ovs_font_get(&s_fn[2], OVS_FACE_COND, FW_BOLD, ovs_px(fy, OVS_LP_FS_TAG));
    if (!fn[0] || !fn[1] || !fn[2]) return 0;
    if (!ovs_canvas_size(&s_cv, w, h) || !ovs_buf(&s_pm, &s_pmcap, w * h)) return 0;
    memset(s_cv.bits, 0, (size_t)w * h * 4u);
    fold = (HFONT)SelectObject(dc, fn[0]);
    SetTextColor(dc, RGB(255, 255, 255));
    for (k = 0; k < ni; k++) {
        SelectObject(dc, fn[it[k].font]);
        DrawTextW(dc, it[k].txt, -1, &it[k].r, it[k].fmt);
    }
    GdiFlush();
    SelectObject(dc, fold);
    { /* Flaeche, Akzentlinie und Kopf-Trennlinie wie alle LoC-Panels (W467 ovs_lp_finish), dann Text je Element */
        unsigned cols[LW_MAX_ROWS + 2];
        for (k = 0; k < LW_MAX_ROWS + 2; k++) cols[k] = OVS_RGB_TEXT;
        ovs_lp_finish(s_pm, s_cv.bits, w, h, fy, 1, cols, nrow + 1 < LW_MAX_ROWS + 2 ? nrow + 1 : LW_MAX_ROWS + 2);
        lw = (unsigned)ovs_px(fy, OVS_LP_BAR);
        if (lw > w) lw = w;
    }
    for (k = 0; k < ni; k++) {
        const lw_item *q = &it[k];
        int yy, x0 = q->r.left < (int)lw ? (int)lw : q->r.left, x1 = q->r.right < (int)w ? q->r.right : (int)w;
        if (x1 <= x0) continue;
        for (yy = q->r.top < 0 ? 0 : q->r.top; yy < q->r.bottom && yy < (int)h; yy++)
            ovs_pm_span(s_pm + (unsigned)yy * w + (unsigned)x0, s_cv.bits + (unsigned)yy * w + (unsigned)x0, x1 - x0, q->col,
                        q->bg, q->bga);
    }
    s_w = w;
    s_h = h;
    s_tex_dirty = 1;
    return 1;
}

static int lw_upload(void)
{
    int r = ovs_tex_ensure(&s_tx, s_w, s_h);
    if (!r) return 0;
    if (r == 2) s_tex_dirty = 1;
    if (!s_tex_dirty) return 1;
    if (!ovs_tex_write(&s_tx, s_pm)) return 0;
    s_tex_dirty = 0;
    return 1;
}

/* Lage: rechts in der Bildschirmmitte (Backbuffer, Rand OVS_MARGIN im SB-UI-Faktor), etwas ueber der Mitte, damit
 * die Tastenhilfe unten frei bleibt. Ueberlappt eine Anmeldung dieses Bildes (z. B. F2 oben rechts), weicht das
 * Widget nach unten aus; passt es nicht, entfaellt es in diesem Bild. */
void loc_lobbywidget_draw(ID3D11RenderTargetView *rtv, int save)
{
    unsigned bx, by, bw, bh, margin, step;
    float fy, fu;
    lw_snap sn;
    u32 box;
    if (!loc_lobbywidget_visible() || !rtv || !g_dev || !g_ctx || !g_bb_w || !g_bb_h) return;
    ovs_loc_box(&bx, &by, &bw, &bh);
    if (bw < 64u || bh < 48u) return;
    fy = (float)bh / 480.0f;
    box = (u32)bw * 7919u + (u32)bh * 31u + (u32)bx * 3u + (u32)by;
    AcquireSRWLockShared(&s_lock);
    sn = s_snap;
    ReleaseSRWLockShared(&s_lock);
    if (sn.gen != s_drawn_gen || box != s_drawn_box || !s_pm) {
        s_have = lw_render(&sn, fy);
        s_drawn_gen = sn.gen;
        s_drawn_box = box;
        if (s_have && g_cfg.loc_lobby_widget_test == 1 && s_shot_n < 8) InterlockedExchange(&s_shot_req, 1);
    }
    if (s_have && g_cfg.loc_lobby_widget_test == 1 && s_shot_n < 8 && (!s_shot_t || GetTickCount() - s_shot_t > 10000u)) {
        s_shot_t = GetTickCount() | 1u; /* TEST: bei jeder Aenderung und alle 10 s, hoechstens 8 Bilder */
        InterlockedExchange(&s_shot_req, 1);
    }
    if (!s_have || !lw_upload()) return;
    fu = ovs_scale_ui(bh);
    margin = (unsigned)ovs_px(fu, OVS_MARGIN);
    if (s_w + margin > g_bb_w || s_h + 2u * margin > g_bb_h) return;
    s_ox = g_bb_w - margin - s_w;
    s_oy = g_bb_h * 2u / 5u > s_h / 2u ? g_bb_h * 2u / 5u - s_h / 2u : margin;
    if (s_oy < margin) s_oy = margin;
    step = (unsigned)ovs_px(fu, OVS_LP_GAP);
    while (ovs_layout_hits(OVS_SLOT_LOBBYWIDGET, s_ox, s_oy, s_w, s_h) && s_oy + s_h + margin + step <= g_bb_h) s_oy += step;
    if (s_oy + s_h > g_bb_h || ovs_layout_hits(OVS_SLOT_LOBBYWIDGET, s_ox, s_oy, s_w, s_h)) return;
    ovs_layout_claim(OVS_SLOT_LOBBYWIDGET, s_ox, s_oy, s_w, s_h);
    if (save) sb_ovl_save_take(&s_save, rtv, s_ox, s_oy, s_w, s_h);
    sb_ovl_quad(rtv, s_tx.srv, s_ox, s_oy, s_w, s_h);
}

void loc_lobbywidget_restore(void)
{
    if (!s_save.saved) return;
    sb_ctx_enter();
    sb_ovl_save_put(&s_save);
    sb_ctx_leave();
}

int loc_lobbywidget_shot_due(char *name, unsigned cap)
{
    if (!s_shot_req || !name || cap < 16u) return 0;
    InterlockedExchange(&s_shot_req, 0);
    _snprintf_s(name, cap, _TRUNCATE, "w460_widget_%02ld", InterlockedIncrement(&s_shot_n));
    return 1;
}
