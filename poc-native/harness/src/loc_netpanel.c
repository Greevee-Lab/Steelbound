/* loc_netpanel.c - W447 LOC-NET-AUTO (GitHub #82): Netz-Panel in der LoC-Lobby (Beschreibung in loc_netpanel.h).
 * Zeichnung wie W443 (loc_lobbylist.c): GDI-Text in eine kleine Textur, ein Quad nach Clip/Video/F11-Bild, Bereich
 * gesichert. Keine Eingriffe ins Spiel, keine Eingaben. */
#include "render_int.h"
#include "sb_overlay.h"
#include "sb_nethud.h"
#include "sb_lang.h"
#include "loc_net.h"
#include "loc_assist.h"
#include "loc_rollback.h"
#include "loc_rbn.h"
#include "loc_netpanel.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef uint32_t u32;

#define NP_ROWS 8
#define NP_COLS 120
#define NP_FULL_MS 12000u /* so lange voll nach einer Aenderung, danach eine Zeile */

/* ------------------------------------------------------------------ Schnappschuss (Spiel-Thread -> Render-Thread) */
typedef struct np_snap {
    int show;                 /* Lobby mit Mitspieler (bzw. TEST 2) */
    int nrow, full;
    wchar_t row[NP_ROWS][NP_COLS];
    unsigned col[NP_ROWS];
    u32 t, gen;
} np_snap;

static SRWLOCK s_lock = SRWLOCK_INIT;
static np_snap s_snap;
static volatile LONG s_shot_req, s_shot_n;
static u32 s_key, s_key_t; /* Kennung des Inhalts (ohne Ping) und Zeit der letzten Aenderung */

static void np_lw(const char *key, wchar_t *o, int cap)
{
    if (sb_lang_w(key, o, cap) != 0) o[0] = 0;
}

static u32 np_hash(u32 h, int v) { return (h ^ (u32)v) * 16777619u; }

void loc_netpanel_frame(unsigned f)
{
    np_snap n;
    loc_rb_lobby lb;
    loc_net_hud nh;
    int have_rb, have_net, live, lsrc = 0, lm, k, ms_start, ms_tgt, ping_max = -1;
    wchar_t a[NP_COLS], fmt[NP_COLS], tmp[NP_COLS];
    u32 key = 2166136261u, now;
    if (!g_cfg.loc_net_panel || !g_sba_loc || (f % 30u) != 0u) return; /* 2x/s bei 60 Bildern */
    memset(&n, 0, sizeof n);
    now = GetTickCount();
    have_rb = loc_rb_lobby_read(&lb);
    have_net = loc_net_hud_read(&nh);
    live = loc_net_peer_live() && loc_net_streams() > 0; /* Sitzung des Spiels (Lobby/Match), nicht nur Transport */
    if (have_rb) {
        n.show = live && !lb.in_mission;
    } else { /* Lockstep (ohne Eingabeschicht): Lobby = verbunden, aber keine Lockstep-Rahmen in den letzten 3 s */
        long l34 = loc_net_last34_ms();
        n.show = live && (!l34 || (u32)(now - (u32)l34) > 3000u);
    }
    if (g_cfg.loc_net_panel_test == 2) n.show = 1; /* TEST: Lage ohne Mitspieler pruefen */
    if (n.show) {
        /* Zeile 1: Netzmodus und Profil */
        const char *pk = lb.net_mode == 2 ? "ovl.np_ls" : lb.net_mode == 1 ? "ovl.np_rb" :
                         lb.prof == RBN_PROF_DIRECT ? "ovl.np_auto_direct" : lb.prof == RBN_PROF_ROLLBACK ? "ovl.np_auto_rb" :
                         lb.npeer > 0 ? "ovl.np_auto_fixed" : "ovl.np_auto_wait";
        np_lw("ovl.np_title", fmt, NP_COLS);
        np_lw(pk, a, NP_COLS);
        _snwprintf_s(n.row[n.nrow], NP_COLS, _TRUNCATE, fmt, a);
        n.col[n.nrow++] = lb.net_mode == 0 && lb.prof == RBN_PROF_DIRECT ? 0x78DC8Cu : 0xFFD21Eu;
        key = np_hash(np_hash(key, lb.net_mode), lb.prof);
        /* Zeile 2: Verzoegerung (Start -> Ziel) */
        if (lb.net_mode != 2 && lb.d_start > 0) {
            ms_start = lb.d_start * 50;
            ms_tgt = lb.d_target * 50;
            if (lb.d_target > 0 && lb.d_target != lb.d_start) {
                np_lw("ovl.np_delay2", fmt, NP_COLS);
                _snwprintf_s(n.row[n.nrow], NP_COLS, _TRUNCATE, fmt, lb.d_start, lb.d_target, ms_start, ms_tgt);
            } else {
                np_lw(lb.d_start == 1 ? "ovl.np_delay1" : "ovl.np_delay", fmt, NP_COLS);
                _snwprintf_s(n.row[n.nrow], NP_COLS, _TRUNCATE, fmt, lb.d_start, ms_start);
            }
            n.col[n.nrow++] = 0xE6E6E6u;
            key = np_hash(np_hash(key, lb.d_start), lb.d_target);
        } else if (lb.net_mode == 2) {
            np_lw("ovl.np_ls_delay", n.row[n.nrow], NP_COLS);
            n.col[n.nrow++] = 0xE6E6E6u;
        }
        /* Zeile 3: Lobby-Modus (W440) */
        lm = loc_net_assist_mode(&lsrc);
        if (lm == LAS_SIM || lm == LAS_ARCADE) {
            np_lw("ovl.np_mode", fmt, NP_COLS);
            np_lw(lm == LAS_ARCADE ? "ovl.nh_lm_arcade" : "ovl.nh_lm_sim", a, NP_COLS);
            _snwprintf_s(n.row[n.nrow], NP_COLS, _TRUNCATE, fmt, a);
            n.col[n.nrow++] = lm == LAS_ARCADE ? 0xFFA028u : 0x8CC8FFu;
            key = np_hash(key, lm);
        }
        /* Ping je Mitspieler: Transport (Namen) bzw. Eingabeschicht (Platz) */
        if (have_net && nh.npeer > 0 && (nh.peer[0].name[0] || !have_rb)) { /* Namen aus der Sitzung, sonst Platz */
            for (k = 0; k < nh.npeer && n.nrow < NP_ROWS - 1; k++) {
                int p = (int)(nh.peer[k].ping + 0.5);
                if (!nh.peer[k].up) continue;
                MultiByteToWideChar(CP_UTF8, 0, nh.peer[k].name[0] ? nh.peer[k].name : "?", -1, a, NP_COLS);
                np_lw(p > 0 ? "ovl.np_peer" : "ovl.np_peer_unk", fmt, NP_COLS);
                _snwprintf_s(n.row[n.nrow], NP_COLS, _TRUNCATE, fmt, a, p);
                n.col[n.nrow++] = p > 150 ? 0xFFA028u : 0xC8C8C8u;
                if (p > ping_max) ping_max = p;
                key = np_hash(key, k + 100);
            }
        } else if (have_rb) {
            for (k = 0; k < lb.npeer && n.nrow < NP_ROWS - 1; k++) {
                np_lw(lb.peer[k].rtt >= 0 ? "ovl.np_peer_n" : "ovl.np_peer_n_unk", fmt, NP_COLS);
                _snwprintf_s(n.row[n.nrow], NP_COLS, _TRUNCATE, fmt,
                             lb.peer[k].pidx >= 0 ? lb.peer[k].pidx + 1 : k + 1, lb.peer[k].rtt); /* Platz in der Lobby oft noch unbekannt */
                n.col[n.nrow++] = lb.peer[k].rtt > 150 ? 0xFFA028u : 0xC8C8C8u;
                if (lb.peer[k].rtt > ping_max) ping_max = lb.peer[k].rtt;
                key = np_hash(key, k + 100);
            }
        }
        if (key != s_key || !s_key_t) {
            s_key = key;
            s_key_t = now | 1u;
        }
        n.full = (u32)(now - s_key_t) < NP_FULL_MS;
        if (n.full && n.nrow > 1) { /* hoechstens zwei Zeilen: passt in den freien Streifen ueber dem Lobby-Rahmen */
            wchar_t l0[NP_COLS], l1[NP_COLS];
            unsigned c1 = n.nrow > 2 ? n.col[2] : 0xC8C8C8u;
            int i;
            _snwprintf_s(l0, NP_COLS, _TRUNCATE, L"%s  \x00B7  %s", n.row[0], n.row[1]);
            l1[0] = 0;
            for (i = 2; i < n.nrow; i++) {
                size_t L = wcslen(l1);
                _snwprintf_s(l1 + L, NP_COLS - L, _TRUNCATE, L"%s%s", L ? L"  \x00B7  " : L"", n.row[i]);
            }
            memset(n.row, 0, sizeof n.row);
            memcpy(n.row[0], l0, sizeof l0);
            memcpy(n.row[1], l1, sizeof l1);
            n.nrow = l1[0] ? 2 : 1;
            n.col[1] = c1;
        }
        if (!n.full) { /* eine Zeile: Modus, Verzoegerung, groesster Ping */
            np_lw(lb.net_mode == 2 ? "ovl.np_short_ls" : "ovl.np_short", fmt, NP_COLS);
            np_lw(lb.net_mode == 2 ? "ovl.np_ls" : lb.net_mode == 1 ? "ovl.np_rb" :
                  lb.prof == RBN_PROF_DIRECT ? "ovl.np_auto_direct" : lb.prof == RBN_PROF_ROLLBACK ? "ovl.np_auto_rb" :
                  lb.npeer > 0 ? "ovl.np_auto_fixed" : "ovl.np_auto_wait", a, NP_COLS);
            if (lb.net_mode == 2)
                _snwprintf_s(tmp, NP_COLS, _TRUNCATE, fmt, a, ping_max < 0 ? 0 : ping_max);
            else
                _snwprintf_s(tmp, NP_COLS, _TRUNCATE, fmt, a, lb.d_target > 0 ? lb.d_target : lb.d_start,
                             ping_max < 0 ? 0 : ping_max);
            memcpy(n.row[0], tmp, sizeof tmp);
            n.nrow = 1;
        }
    } else {
        s_key_t = 0; /* naechste Lobby beginnt wieder voll */
    }
    n.t = now;
    AcquireSRWLockExclusive(&s_lock);
    n.gen = s_snap.gen;
    n.t = s_snap.t;
    if (memcmp(&n, &s_snap, sizeof n) != 0) n.gen++; /* nur neuer Inhalt zaehlt */
    n.t = now;
    s_snap = n;
    ReleaseSRWLockExclusive(&s_lock);
}

/* ------------------------------------------------------------------ Anzeige (Render-Thread) */
static HDC s_dc;
static HBITMAP s_bmp, s_bmp_old;
static unsigned *s_bits, *s_pm;
static unsigned s_dw, s_dh, s_w, s_h, s_pmcap;
static ID3D11Texture2D *s_tex;
static ID3D11ShaderResourceView *s_srv;
static unsigned s_tw, s_th;
static sb_ovl_save_t s_save;
static u32 s_drawn_gen, s_drawn_box, s_shot_gen;
static int s_tex_dirty, s_have;
static unsigned s_ox, s_oy;

/* Spielbild im Backbuffer (wie W437/W443: 4:3 bzw. 16:9, Balken links/rechts bzw. oben/unten) */
static void np_box(unsigned *bx, unsigned *by, unsigned *bw, unsigned *bh)
{
    float ga = g_loc_wide ? 16.0f / 9.0f : 4.0f / 3.0f, ba = (float)g_bb_w / (float)(g_bb_h ? g_bb_h : 1);
    if (ga >= ba) {
        *bw = g_bb_w;
        *bh = (unsigned)((float)g_bb_w / ga + 0.5f);
        if (*bh > g_bb_h) *bh = g_bb_h;
        *bx = 0;
        *by = (g_bb_h - *bh) / 2;
    } else {
        *bh = g_bb_h;
        *bw = (unsigned)((float)g_bb_h * ga + 0.5f);
        if (*bw > g_bb_w) *bw = g_bb_w;
        *bx = (g_bb_w - *bw) / 2;
        *by = 0;
    }
}

int loc_netpanel_visible(void)
{
    int a;
    u32 t;
    if (!g_cfg.loc_net_panel || !g_sba_loc || g_cfg.vr_enable || sb_ovl_open() || sb_nhud_visible()) return 0;
    AcquireSRWLockShared(&s_lock);
    a = s_snap.show && s_snap.nrow > 0;
    t = s_snap.t;
    ReleaseSRWLockShared(&s_lock);
    return a && (u32)(GetTickCount() - t) < 2000u; /* Spiel-Thread steht: nicht stehen lassen */
}

static int np_px(float f, float v) { int r = (int)(v * f + 0.5f); return r < 1 ? 1 : r; }

static void np_gdi_free(void)
{
    if (s_bmp) {
        SelectObject(s_dc, s_bmp_old);
        DeleteObject(s_bmp);
        s_bmp = NULL;
        s_bits = NULL;
    }
}

/* Inhalt (s_w x s_h, vormultipliziertes BGRA): halbtransparente Flaeche, Zeilen links. Groesse im Menueraum 640x480:
 * Breite 176, Zeilenhoehe 12, Rand 5. 1 = ok. */
static int np_render(const np_snap *sn, float fy)
{
    unsigned w, h, lh, pad, x, y;
    int i;
    HFONT fn[2], fold;
    BITMAPINFO bi;
    SIZE sz;
    lh = (unsigned)np_px(fy, 11.0f);
    pad = (unsigned)np_px(fy, 4.0f);
    if (sn->nrow <= 0) return 0;
    if (!s_dc) {
        s_dc = CreateCompatibleDC(NULL);
        if (!s_dc) return 0;
    }
    fn[0] = CreateFontW(-np_px(fy, 10.0f), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                        CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, VARIABLE_PITCH | FF_SWISS, L"Bahnschrift SemiCondensed");
    fn[1] = CreateFontW(-np_px(fy, 9.5f), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                        CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, VARIABLE_PITCH | FF_SWISS, L"Bahnschrift SemiCondensed");
    if (!fn[0] || !fn[1]) {
        if (fn[0]) DeleteObject(fn[0]);
        if (fn[1]) DeleteObject(fn[1]);
        return 0;
    }
    /* Breite nach dem laengsten Text (hoechstens 230 im Menueraum) */
    fold = (HFONT)SelectObject(s_dc, fn[0]);
    w = (unsigned)np_px(fy, 120.0f);
    for (i = 0; i < sn->nrow; i++) {
        SelectObject(s_dc, fn[i == 0 ? 0 : 1]);
        if (GetTextExtentPoint32W(s_dc, sn->row[i], (int)wcslen(sn->row[i]), &sz) && (unsigned)sz.cx + 2u * pad > w)
            w = (unsigned)sz.cx + 2u * pad;
    }
    if (w > (unsigned)np_px(fy, 420.0f)) w = (unsigned)np_px(fy, 420.0f);
    h = (unsigned)sn->nrow * lh + 2u * pad - (unsigned)np_px(fy, 2.0f);
    SelectObject(s_dc, fold);
    if (w < 8u || h < 8u) goto fail;
    if (!s_bmp || s_dw != w || s_dh != h) {
        np_gdi_free();
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = (LONG)w;
        bi.bmiHeader.biHeight = -(LONG)h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        s_bmp = CreateDIBSection(s_dc, &bi, DIB_RGB_COLORS, (void **)&s_bits, NULL, 0);
        if (!s_bmp || !s_bits) {
            s_bmp = NULL;
            goto fail;
        }
        s_bmp_old = (HBITMAP)SelectObject(s_dc, s_bmp);
        s_dw = w;
        s_dh = h;
    }
    if (!s_pm || s_pmcap < w * h) {
        free(s_pm);
        s_pm = (unsigned *)malloc((size_t)w * h * 4u);
        s_pmcap = s_pm ? w * h : 0;
        if (!s_pm) goto fail;
    }
    memset(s_bits, 0, (size_t)w * h * 4u);
    fold = (HFONT)SelectObject(s_dc, fn[0]);
    SetBkMode(s_dc, TRANSPARENT);
    SetTextColor(s_dc, RGB(255, 255, 255));
    for (i = 0; i < sn->nrow; i++) {
        RECT r;
        r.left = (LONG)pad;
        r.right = (LONG)(w - pad);
        r.top = (LONG)(pad + (unsigned)i * lh - (unsigned)np_px(fy, 1.0f));
        r.bottom = r.top + (LONG)lh;
        SelectObject(s_dc, fn[i == 0 ? 0 : 1]);
        DrawTextW(s_dc, sn->row[i], -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    GdiFlush();
    SelectObject(s_dc, fold);
    /* Flaeche dunkel (Alpha 0,62), Text je Zeile in seiner Farbe; schmale Linie links in der Farbe der ersten Zeile */
    for (y = 0; y < h; y++) {
        int ri = y < pad ? 0 : (int)((y - pad) / lh);
        unsigned colr;
        float cr, cg, cb;
        if (ri >= sn->nrow) ri = sn->nrow - 1;
        colr = sn->col[ri];
        cr = (float)((colr >> 16) & 255u);
        cg = (float)((colr >> 8) & 255u);
        cb = (float)(colr & 255u);
        for (x = 0; x < w; x++) {
            unsigned px = s_bits[y * w + x];
            unsigned m = (px >> 16) & 255u, g2 = (px >> 8) & 255u, b2 = px & 255u;
            float c, ba = 0.62f, br = 8.0f * ba, bg = 12.0f * ba, bb = 16.0f * ba, oa;
            if (x < (unsigned)np_px(fy, 2.0f)) { /* Linie links */
                unsigned c0 = sn->col[0];
                s_pm[y * w + x] = 0xE6000000u | ((unsigned)(((c0 >> 16) & 255u) * 0.9f) << 16) |
                                  ((unsigned)(((c0 >> 8) & 255u) * 0.9f) << 8) | (unsigned)((c0 & 255u) * 0.9f);
                continue;
            }
            if (g2 > m) m = g2;
            if (b2 > m) m = b2;
            c = (float)m / 255.0f;
            oa = c + ba * (1.0f - c);
            s_pm[y * w + x] = ((unsigned)(oa * 255.0f + 0.5f) << 24) | ((unsigned)(cr * c + br * (1.0f - c) + 0.5f) << 16) |
                              ((unsigned)(cg * c + bg * (1.0f - c) + 0.5f) << 8) | (unsigned)(cb * c + bb * (1.0f - c) + 0.5f);
        }
    }
    DeleteObject(fn[0]);
    DeleteObject(fn[1]);
    s_w = w;
    s_h = h;
    s_tex_dirty = 1;
    return 1;
fail:
    DeleteObject(fn[0]);
    DeleteObject(fn[1]);
    return 0;
}

static int np_upload(void)
{
    D3D11_MAPPED_SUBRESOURCE map;
    unsigned y;
    if (!s_tex || s_tw != s_w || s_th != s_h) {
        D3D11_TEXTURE2D_DESC td;
        if (s_srv) { ID3D11ShaderResourceView_Release(s_srv); s_srv = NULL; }
        if (s_tex) { ID3D11Texture2D_Release(s_tex); s_tex = NULL; }
        memset(&td, 0, sizeof td);
        td.Width = s_w;
        td.Height = s_h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &s_tex)) ||
            FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)s_tex, NULL, &s_srv))) {
            if (s_tex) { ID3D11Texture2D_Release(s_tex); s_tex = NULL; }
            return 0;
        }
        s_tw = s_w;
        s_th = s_h;
        s_tex_dirty = 1;
    }
    if (!s_tex_dirty) return 1;
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)s_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return 0;
    for (y = 0; y < s_th; y++)
        memcpy((unsigned char *)map.pData + (size_t)y * map.RowPitch, s_pm + (size_t)y * s_w, s_w * 4u);
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)s_tex, 0);
    s_tex_dirty = 0;
    return 1;
}

/* Lage im Menueraum 640x480: rechts oben (Abstand 10 vom rechten Rand, 4 von oben, hoechstens 2 Zeilen ~26 hoch) - der
 * Streifen ueber dem Lobby-Rahmen (beginnt bei ~40) ist frei
 * (Titelzeile links, Spielerliste/Karte darunter; Belege W447 4:3/16:9/21:9). */
#define NP_RIGHT 10.0f
#define NP_TOP 4.0f

void loc_netpanel_draw(ID3D11RenderTargetView *rtv, int save)
{
    unsigned bx, by, bw, bh;
    float fx, fy;
    np_snap sn;
    u32 box;
    if (!loc_netpanel_visible() || !rtv || !g_dev || !g_ctx || !g_bb_w || !g_bb_h) return;
    np_box(&bx, &by, &bw, &bh);
    if (bw < 64u || bh < 48u) return;
    fx = (float)bw / 640.0f;
    fy = (float)bh / 480.0f;
    box = (u32)bw * 7919u + (u32)bh * 31u + (u32)bx * 3u + (u32)by;
    AcquireSRWLockShared(&s_lock);
    sn = s_snap;
    ReleaseSRWLockShared(&s_lock);
    if (sn.gen != s_drawn_gen || box != s_drawn_box || !s_pm) {
        s_have = np_render(&sn, fy);
        s_drawn_gen = sn.gen;
        s_drawn_box = box;
        if (s_have && g_cfg.loc_net_panel_test && sn.gen != s_shot_gen && s_shot_n < 4) {
            s_shot_gen = sn.gen;
            InterlockedExchange(&s_shot_req, 1);
        }
    }
    if (!s_have || !np_upload()) return;
    if (s_w + (unsigned)(NP_RIGHT * fx) > bw) return;
    s_ox = bx + bw - s_w - (unsigned)(NP_RIGHT * fx + 0.5f);
    s_oy = by + (unsigned)(NP_TOP * fy + 0.5f);
    if (s_ox + s_w > g_bb_w || s_oy + s_h > g_bb_h) return;
    if (save) sb_ovl_save_take(&s_save, rtv, s_ox, s_oy, s_w, s_h);
    sb_ovl_quad(rtv, s_srv, s_ox, s_oy, s_w, s_h);
}

void loc_netpanel_restore(void)
{
    if (!s_save.saved) return;
    sb_ctx_enter();
    sb_ovl_save_put(&s_save);
    sb_ctx_leave();
}

int loc_netpanel_shot_due(char *name, unsigned cap)
{
    if (!s_shot_req || !name || cap < 16u) return 0;
    InterlockedExchange(&s_shot_req, 0);
    _snprintf_s(name, cap, _TRUNCATE, "w447_netpanel_%02ld", InterlockedIncrement(&s_shot_n));
    return 1;
}
