/* loc_lobbylist.c - W443 LOC-LOBBY-LIST (GitHub #79): Modus und Ping in der Suchliste des Spiels ("Search Match").
 *
 * Die Suchliste zeigt oeffentliche steelnet-Lobbys, die unsere Netz-HLE dem Spiel als Host-Infos liefert (W373). Je
 * sichtbarer Zeile liest der Spiel-Thread die Lobby-Id aus dem Listeneintrag (XNKID, loc_lobbylist.h) und holt Modus
 * und Ping-Schaetzung aus dem Transport (loctun_pub_lookup). Der Render-Thread zeichnet links neben dem Namen eine
 * Marke SIM/ARCADE und im Detailfeld des gewaehlten Eintrags zwei Zeilen "LOBBY MODE" und "PING (EST.)".
 * Nur Lesen von Spielspeicher (SEH-geschuetzt), keine Patches, kein Eingriff in Eingaben. Keine Namen im Log
 * (nur Zahlen). Zeichnung wie der Chat (W437): nach Clip/Video/F11-Bild, Bereich gesichert. */
#include "loc_tun.h"
#include "render_int.h"
#include "sb_overlay.h"
#include "overlay_style.h" /* W461 OVERLAY-CORE */
#include "loc_net.h"
#include "loc_net_core.h"
#include "loc_lobbylist.h"
#include "loc_lobbylist_ovl.h"
#include "sb_profile.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef uint32_t u32;

/* ------------------------------------------------------------------ Schnappschuss (Spiel-Thread -> Render-Thread) */
typedef struct llv_snap {
    llv_view v;
    int mode[LLV_ROWS], est[LLV_ROWS], sel_mode, sel_est;
    u32 t;  /* GetTickCount der Aufnahme */
    u32 gen;
} llv_snap;

static SRWLOCK s_lock = SRWLOCK_INIT;
static llv_snap s_snap;
static volatile LONG s_shot_req, s_shot_n;

static int llv_read(llv_view *v)
{
    static uint8_t buf[LLV_MAX_ENTRIES * LLV_ENTRY];
    int kind = -1, sub = -1, alpha = 0, count = 0, top = -1, sel = -1;
    __try {
        const uint8_t *g = *(const uint8_t *const *)(uintptr_t)LLV_G_PTR;
        const uint8_t *lk = *(const uint8_t *const *)(uintptr_t)LLV_LISTKIND_PTR;
        const uint8_t *m = *(const uint8_t *const *)(uintptr_t)LLV_MENU_PTR;
        if (g && lk && m) {
            const uint8_t *lko = *(const uint8_t *const *)lk;
            kind = lko ? lko[0x14] : -1;
            sub = *(const int32_t *)(m + LLV_M_SUB);
            alpha = (int8_t)m[LLV_M_ALPHA];
            top = (int8_t)m[LLV_M_TOP];
            count = (int8_t)g[LLV_G_COUNT];
            sel = (int8_t)g[LLV_G_SEL];
            if (kind == 2 && sub == 1 && count >= 1 && count <= LLV_MAX_ENTRIES)
                memcpy(buf, g + LLV_G_LIST, (size_t)count * LLV_ENTRY);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        kind = -1;
    }
    return llv_view_from(kind, sub, alpha, count, top, sel, buf, v);
}

void loc_lobbylist_frame(unsigned f)
{
    static int was_active;
    static u32 last_sig;
    llv_snap n;
    locnet *ln;
    int lids[LLV_ROWS + 1], md[LLV_ROWS + 1], es[LLV_ROWS + 1], k, nsim = 0, narc = 0, nother = 0;
    u32 sig;
    if (!g_cfg.loc_lobby_list || (f & 3u) != 0) return;
    memset(&n, 0, sizeof n);
    if (!llv_read(&n.v)) {
        if (was_active) {
            AcquireSRWLockExclusive(&s_lock);
            s_snap.v.active = 0;
            ReleaseSRWLockExclusive(&s_lock);
            was_active = 0;
        }
        return;
    }
    ln = loc_net_core();
    for (k = 0; k < n.v.nvis; k++) lids[k] = n.v.lid[k];
    lids[n.v.nvis] = n.v.sel_lid;
    for (k = 0; k <= n.v.nvis; k++) {
        md[k] = 0;
        es[k] = -1;
    }
    if (ln && ln->tun) loctun_pub_lookup(ln->tun, lids, n.v.nvis + 1, md, es);
    for (k = 0; k < n.v.nvis; k++) {
        n.mode[k] = md[k];
        n.est[k] = es[k];
        if (md[k] == 2) narc++;
        else if (md[k] == 1) nsim++;
        else nother++;
    }
    n.sel_mode = md[n.v.nvis];
    n.sel_est = es[n.v.nvis];
    n.t = GetTickCount();
    sig = 2166136261u;
    sig = (sig ^ (u32)n.v.top) * 16777619u;
    sig = (sig ^ (u32)n.v.sel) * 16777619u;
    sig = (sig ^ (u32)n.v.count) * 16777619u;
    for (k = 0; k < n.v.nvis; k++) sig = (sig ^ (u32)(n.v.lid[k] * 7 + n.mode[k] * 3 + n.est[k])) * 16777619u;
    sig = (sig ^ (u32)(n.sel_mode * 31 + n.sel_est)) * 16777619u;
    AcquireSRWLockExclusive(&s_lock);
    n.gen = s_snap.gen + (sig != last_sig || !was_active ? 1u : 0u);
    s_snap = n;
    ReleaseSRWLockExclusive(&s_lock);
    if (!was_active) /* nur Zahlen, keine Namen */
        HLOG_I("LOCLIST: Suchliste sichtbar - %d Eintraege, sichtbar %d (steelnet sim %d, arcade %d, ohne Lobby-Modus %d)",
               n.v.count, n.v.nvis, nsim, narc, nother);
    if (g_cfg.loc_lobby_list_test && sig != last_sig) {
        HLOG_I("LOCLIST test: Bild %u erste=%d gewaehlt=%d Modus=%s Ping=%d", f, n.v.top, n.v.sel,
               llv_mode_label(n.sel_mode) ? llv_mode_label(n.sel_mode) : "-", n.sel_est);
        for (k = 0; k < n.v.nvis; k++)
            HLOG_I("LOCLIST test: Zeile %d Lobby %d Modus=%s Ping=%d", n.v.top + k, n.v.lid[k],
                   llv_mode_label(n.mode[k]) ? llv_mode_label(n.mode[k]) : "-", n.est[k]);
    }
    was_active = 1;
    last_sig = sig;
}

/* ------------------------------------------------------------------ Anzeige (Render-Thread) */
static ovs_canvas_t s_cv;    /* W461: GDI-Flaeche, Schriften und Textur aus overlay_style */
static ovs_font_t s_fn[3];
static ovs_tex_t s_tx;
static unsigned *s_pm;
static unsigned s_w, s_h, s_pmcap;
static sb_ovl_save_t s_save;
static u32 s_drawn_gen, s_drawn_box, s_shot_gen;
static int s_tex_dirty, s_have;
static unsigned s_ox, s_oy; /* Lage des Inhalts im Backbuffer */

/* Bereich im Menueraum 640x480, den die Anzeige abdeckt (Marken links, Detailfeld rechts). */
#define LLV_AX0 0
#define LLV_AY0 (LLV_ROW_Y0 - 2)
#define LLV_AX1 536
#define LLV_AY1 (LLV_ROW_Y0 + LLV_ROWS * LLV_ROW_H + 2)
/* Detailfeld: zwei Zeilen unter "ROOM STATUS" (Zeilenabstand 15, Bezeichnung ab x 238, Wert rechtsbuendig bis 521). */
#define LLV_DX0 238
#define LLV_DX1 521
#define LLV_DY0 221
#define LLV_DLH 15

int loc_lobbylist_visible(void)
{
    int a;
    u32 t;
    if (!g_cfg.loc_lobby_list || !g_sba_loc || g_cfg.vr_enable || sb_ovl_open()) return 0;
    AcquireSRWLockShared(&s_lock);
    a = s_snap.v.active;
    t = s_snap.t;
    ReleaseSRWLockShared(&s_lock);
    return a && (u32)(GetTickCount() - t) < 600u; /* Spiel-Thread steht (Ladebild, Pause): nicht stehen lassen */
}

typedef struct llv_item {
    RECT r;          /* Pixel im Inhalt */
    unsigned col;    /* Textfarbe 0xRRGGBB */
    unsigned bg;     /* Hintergrund 0xRRGGBB (nur mit bga > 0) */
    float bga;
    int font;        /* 0 Marke, 1 Bezeichnung, 2 Wert */
    UINT fmt;
    wchar_t txt[24];
} llv_item;

#define llv_px ovs_px

/* Inhalt (s_w x s_h, vormultipliziertes BGRA) aus dem Schnappschuss. 1 = ok (0 = nichts zu zeigen). */
static int llv_render(const llv_snap *sn, float fx, float fy)
{
    llv_item it[LLV_ROWS + 4];
    int ni = 0, k, i;
    unsigned w = (unsigned)llv_px(fx, (float)(LLV_AX1 - LLV_AX0)), h = (unsigned)llv_px(fy, (float)(LLV_AY1 - LLV_AY0));
    HFONT fn[3], fold;
    for (k = 0; k < sn->v.nvis; k++) { /* Marke je Zeile links neben dem Namen */
        const char *lb = llv_mode_label(sn->mode[k]);
        int y0, y1;
        llv_item *p;
        if (!lb) continue;
        llv_row_y(k, &y0, &y1);
        p = &it[ni++];
        memset(p, 0, sizeof *p);
        p->r.left = llv_px(fx, 3.0f);
        p->r.right = llv_px(fx, (float)(LLV_NAME_X - 6));
        p->r.top = llv_px(fy, (float)(y0 + 5 - LLV_AY0));
        p->r.bottom = llv_px(fy, (float)(y1 - 4 - LLV_AY0));
        p->col = OVS_RGB_TAG_TXT;
        p->bg = sn->mode[k] == 2 ? OVS_RGB_ARCADE_BG : OVS_RGB_SIM_BG; /* W467: Tokens */
        p->bga = OVS_TAG_ALPHA;
        p->font = 0;
        p->fmt = DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX;
        MultiByteToWideChar(CP_ACP, 0, lb, -1, p->txt, 24);
    }
    if (llv_mode_label(sn->sel_mode)) { /* Detailfeld: Modus und Ping-Schaetzung des gewaehlten Eintrags */
        static const char *const lab[2] = {"LOBBY MODE", "PING (EST.)"};
        char val[2][16];
        strcpy_s(val[0], sizeof val[0], llv_mode_label(sn->sel_mode));
        llv_ping_text(sn->sel_est, val[1], sizeof val[1]);
        for (i = 0; i < 2; i++) {
            llv_item *p = &it[ni++];
            int yt = LLV_DY0 + i * LLV_DLH - LLV_AY0;
            memset(p, 0, sizeof *p);
            p->r.left = llv_px(fx, (float)(LLV_DX0 - LLV_AX0));
            p->r.right = llv_px(fx, (float)(LLV_DX0 + 150 - LLV_AX0));
            p->r.top = llv_px(fy, (float)yt);
            p->r.bottom = llv_px(fy, (float)(yt + 13));
            p->col = OVS_RGB_TEXT;
            p->font = 1;
            p->fmt = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX;
            MultiByteToWideChar(CP_ACP, 0, lab[i], -1, p->txt, 24);
            p = &it[ni++];
            memset(p, 0, sizeof *p);
            p->r.left = llv_px(fx, (float)(LLV_DX0 + 150 - LLV_AX0));
            p->r.right = llv_px(fx, (float)(LLV_DX1 - LLV_AX0));
            p->r.top = llv_px(fy, (float)(yt - 1));
            p->r.bottom = llv_px(fy, (float)(yt + 14));
            p->col = i == 0 && sn->sel_mode == 2 ? OVS_RGB_BAD : OVS_RGB_MID;
            p->font = 2;
            p->fmt = DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX;
            MultiByteToWideChar(CP_ACP, 0, val[i], -1, p->txt, 24);
        }
    }
    if (!ni || w < 8u || h < 8u) return 0;
    if (!ovs_canvas_size(&s_cv, w, h) || !ovs_buf(&s_pm, &s_pmcap, w * h)) return 0;
    fn[0] = ovs_font_get(&s_fn[0], OVS_FACE_COND, FW_BOLD, llv_px(fy, 10.0f));
    fn[1] = ovs_font_get(&s_fn[1], OVS_FACE_UI, FW_NORMAL, llv_px(fy, 10.0f));
    fn[2] = ovs_font_get(&s_fn[2], OVS_FACE_UI, FW_SEMIBOLD, llv_px(fy, 13.0f));
    if (!fn[0] || !fn[1] || !fn[2]) return 0;
    memset(s_cv.bits, 0, (size_t)w * h * 4u);
    memset(s_pm, 0, (size_t)w * h * 4u);
    fold = (HFONT)SelectObject(s_cv.dc, fn[0]);
    SetTextColor(s_cv.dc, RGB(255, 255, 255));
    for (i = 0; i < ni; i++) {
        SelectObject(s_cv.dc, fn[it[i].font]);
        DrawTextW(s_cv.dc, it[i].txt, -1, &it[i].r, it[i].fmt);
    }
    GdiFlush();
    /* je Element: Hintergrund (vormultipliziert), darueber Text (Abdeckung = groesster Kanal des weissen Texts) */
    for (i = 0; i < ni; i++) {
        const llv_item *p = &it[i];
        int y, x1 = p->r.right < (int)w ? p->r.right : (int)w;
        if (x1 <= p->r.left) continue;
        for (y = p->r.top; y < p->r.bottom && y < (int)h; y++)
            ovs_pm_span(s_pm + (unsigned)y * w + (unsigned)p->r.left, s_cv.bits + (unsigned)y * w + (unsigned)p->r.left,
                        x1 - p->r.left, p->col, p->bg, p->bga);
    }
    SelectObject(s_cv.dc, fold);
    s_w = w;
    s_h = h;
    s_tex_dirty = 1;
    return 1;
}

static int llv_upload(void)
{
    int r = ovs_tex_ensure(&s_tx, s_w, s_h); /* Quad zeigt die ganze Textur: Groesse = Inhalt */
    if (!r) return 0;
    if (r == 2) s_tex_dirty = 1;
    if (!s_tex_dirty) return 1;
    if (!ovs_tex_write(&s_tx, s_pm)) return 0;
    s_tex_dirty = 0;
    return 1;
}

void loc_lobbylist_draw(ID3D11RenderTargetView *rtv, int save)
{
    unsigned bx, by, bw, bh;
    float fx, fy;
    llv_snap sn;
    u32 box;
    if (!loc_lobbylist_visible() || !rtv || !g_dev || !g_ctx || !g_bb_w || !g_bb_h) return;
    ovs_loc_box(&bx, &by, &bw, &bh);
    if (bw < 64u || bh < 48u) return;
    fx = (float)bw / 640.0f;
    fy = (float)bh / 480.0f;
    box = (u32)bw * 7919u + (u32)bh * 31u + (u32)bx * 3u + (u32)by;
    AcquireSRWLockShared(&s_lock);
    sn = s_snap;
    ReleaseSRWLockShared(&s_lock);
    if (sn.gen != s_drawn_gen || box != s_drawn_box || !s_pm) {
        s_have = llv_render(&sn, fx, fy);
        s_drawn_gen = sn.gen;
        s_drawn_box = box;
        if (s_have && g_cfg.loc_lobby_list_test && sn.gen != s_shot_gen && s_shot_n < 6) {
            s_shot_gen = sn.gen;
            InterlockedExchange(&s_shot_req, 1); /* TEST: dieses Bild (mit Anzeige) aufnehmen */
        }
    }
    if (!s_have || !llv_upload()) return;
    s_ox = bx + (unsigned)((float)LLV_AX0 * fx + 0.5f);
    s_oy = by + (unsigned)((float)LLV_AY0 * fy + 0.5f);
    if (s_ox + s_w > g_bb_w || s_oy + s_h > g_bb_h) return;
    ovs_layout_claim(OVS_SLOT_LOBBYLIST, s_ox, s_oy, s_w, s_h);
    if (save) sb_ovl_save_take(&s_save, rtv, s_ox, s_oy, s_w, s_h);
    sb_ovl_quad(rtv, s_tx.srv, s_ox, s_oy, s_w, s_h);
}

void loc_lobbylist_restore(void)
{
    if (!s_save.saved) return;
    sb_ctx_enter();
    sb_ovl_save_put(&s_save);
    sb_ctx_leave();
}

int loc_lobbylist_shot_due(char *name, unsigned cap)
{
    if (!s_shot_req || !name || cap < 16u) return 0;
    InterlockedExchange(&s_shot_req, 0);
    _snprintf_s(name, cap, _TRUNCATE, "w443_lobbylist_%02ld", InterlockedIncrement(&s_shot_n));
    return 1;
}
