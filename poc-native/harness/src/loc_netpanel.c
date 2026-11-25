/* loc_netpanel.c - W447 LOC-NET-AUTO (GitHub #82): Netz-Panel in der LoC-Lobby (Beschreibung in loc_netpanel.h).
 * Zeichnung wie W443 (loc_lobbylist.c): GDI-Text in eine kleine Textur, ein Quad nach Clip/Video/F11-Bild, Bereich
 * gesichert. Keine Eingriffe ins Spiel, keine Eingaben. */
#include "render_int.h"
#include "sb_overlay.h"
#include "overlay_style.h" /* W461 OVERLAY-CORE */
#include "sb_nethud.h"
#include "sb_lang.h"
#include "loc_net.h"
#include "loc_assist.h"
#include "loc_rollback.h"
#include "loc_rbn.h"
#include "loc_netpanel.h"
#include "loc_rulenet.h" /* W453: Host-Regel */
#include "loc_chat.h"    /* W474: LCH_SES_* */
#include "hlog.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef uint32_t u32;

#define NP_ROWS 6   /* W453: Netzmodus, Verzoegerung, schlechtester Ping, (W494 Hinweis weite Lobby), Modus/Sichtbarkeit, Host-Regel */
#define NP_COLS 96
#define NP_PING_OK 80   /* gruen bis hier: Grenze von Auto -> Direkt (loc_rbn.c direct_rtt, W447) */
#define NP_PING_MID 150 /* gelb bis hier, darueber rot (wie W447: > 150 ms auffaellig) */
#define NP_PING_FAR 250 /* W494 LOC-FARPING: ab hier Hinweis "Match wirkt verzoegert" (adaptive Verzoegerung > 3 Ticks) */

/* ------------------------------------------------------------------ Schnappschuss (Spiel-Thread -> Render-Thread) */
typedef struct np_snap {
    int show;                 /* Lobby mit Mitspieler (bzw. TEST 2) */
    int nrow;
    wchar_t row[NP_ROWS][NP_COLS];
    unsigned col[NP_ROWS];
    u32 t, gen;
} np_snap;

static SRWLOCK s_lock = SRWLOCK_INIT;
static np_snap s_snap;
static volatile LONG s_shot_req, s_shot_n;
static int s_log[8]; /* letzter geloggter Stand (nur Zahlen, ohne Ping) */

static void np_lw(const char *key, wchar_t *o, int cap)
{
    if (sb_lang_w(key, o, cap) != 0) o[0] = 0;
}

static unsigned np_ping_col(int p)
{
    return p < 0 ? OVS_RGB_DIM : p <= NP_PING_OK ? OVS_RGB_OK : p <= NP_PING_MID ? OVS_RGB_MID : OVS_RGB_BAD;
}

/* W474: Pilotname fuer die Anzeige. Gaeste wie Suchliste/Widget als "~Name"; ein vom Dienst auf 16 Zeichen gekuerzter
 * Name (Guest-Praefix + Name, accounts.py guest_name) bekommt ein "..." statt hart abgeschnitten zu wirken. */
static void np_name(const char *pilot, wchar_t *o, int cap)
{
    wchar_t w[40];
    const char *p = pilot;
    int cut = 0;
    o[0] = 0;
    if (!p || !p[0]) return;
    if (strncmp(p, "Guest-", 6) == 0 && p[6]) {
        cut = strlen(p) >= 16u;
        p += 6;
        w[0] = L'~';
        if (!MultiByteToWideChar(CP_UTF8, 0, p, -1, w + 1, 38)) w[1] = 0;
    } else if (!MultiByteToWideChar(CP_UTF8, 0, p, -1, w, 39)) {
        w[0] = 0;
    }
    w[39] = 0;
    _snwprintf_s(o, (size_t)cap, _TRUNCATE, cut ? L"%s\x2026" : L"%s", w);
}

/* Sichtbarkeit der Lobby aus der Startkonfiguration (gleiche Felder wie der Transport, loc_net.c W324/W334/W373):
 * 0 = System Link (kein Treffpunkt), 1 = privat mit Raumcode, 2 = oeffentlich. */
static int np_vis(int tun)
{
    if (!tun || !g_cfg.loc_lobby[0]) return 0;
    if (g_cfg.loc_lobby_code[0]) return 1; /* Code geht immer privat */
    return strcmp(g_cfg.loc_lobby_vis, "public") == 0 ? 2 : 1;
}

void loc_netpanel_frame(unsigned f)
{
    np_snap n;
    loc_rb_lobby lb;
    loc_net_hud nh;
    int have_rb, have_net, live, lsrc = 0, lm, k, ping = -1, vis, r_on = 0, r_src = 0, r_mis = 0, have_rule;
    wchar_t a[NP_COLS], b[NP_COLS], fmt[NP_COLS], nm[NP_COLS];
    u32 now;
    if (!g_cfg.loc_net_panel || !g_sba_loc || (f % 30u) != 0u) return; /* 2x/s bei 60 Bildern */
    memset(&n, 0, sizeof n);
    memset(&lb, 0, sizeof lb);
    memset(&nh, 0, sizeof nh);
    nm[0] = 0;
    now = GetTickCount();
    have_rb = loc_rb_lobby_read(&lb);
    have_net = loc_net_hud_read(&nh);
    /* W474 CHATTABS: zentraler Lobby-Zustand (gemeinsam mit dem Chat-Reiter "Lobby"): nur in der Lobby, nicht im Match,
     * nicht nach dem Verlassen (frueher Sitzung/Transport -> Panel blieb im Hauptmenue stehen) */
    live = loc_net_lobby_state();
    n.show = live == LCH_SES_LOBBY;
    if (g_cfg.loc_net_panel_test == 2) n.show = 1; /* TEST: Lage ohne Mitspieler pruefen */
    lm = loc_net_assist_mode(&lsrc);
    vis = np_vis(have_net && nh.tun);
    have_rule = loc_rule_hud(&r_on, &r_src, &r_mis);
    if (n.show) {
        /* Zeile 1: Netzmodus und Profil */
        const char *pk = lb.net_mode == 2 ? "ovl.np_ls" : lb.net_mode == 1 ? "ovl.np_rb" :
                         lb.prof == RBN_PROF_DIRECT ? "ovl.np_auto_direct" : lb.prof == RBN_PROF_ROLLBACK ? "ovl.np_auto_rb" :
                         lb.npeer > 0 ? "ovl.np_auto_fixed" : "ovl.np_auto_wait";
        np_lw("ovl.np_title", fmt, NP_COLS);
        np_lw(pk, a, NP_COLS);
        _snwprintf_s(n.row[n.nrow], NP_COLS, _TRUNCATE, fmt, a);
        n.col[n.nrow++] = lb.net_mode == 0 && lb.prof == RBN_PROF_DIRECT ? OVS_RGB_OK : OVS_RGB_MID;
        /* Zeile 2: Verzoegerung (Start -> Ziel) bzw. Lockstep */
        if (lb.net_mode != 2 && lb.d_start > 0) {
            if (lb.d_target > 0 && lb.d_target != lb.d_start) {
                np_lw("ovl.np_delay2", fmt, NP_COLS);
                _snwprintf_s(n.row[n.nrow], NP_COLS, _TRUNCATE, fmt, lb.d_start, lb.d_target, lb.d_start * 50,
                             lb.d_target * 50);
            } else {
                np_lw(lb.d_start == 1 ? "ovl.np_delay1" : "ovl.np_delay", fmt, NP_COLS);
                _snwprintf_s(n.row[n.nrow], NP_COLS, _TRUNCATE, fmt, lb.d_start, lb.d_start * 50);
            }
            n.col[n.nrow++] = OVS_RGB_TEXT;
        } else if (lb.net_mode == 2) {
            np_lw("ovl.np_ls_delay", n.row[n.nrow], NP_COLS);
            n.col[n.nrow++] = OVS_RGB_TEXT;
        }
        /* Zeile 3: schlechtester Ping der Lobby - Transport (mit Pilotname) bzw. Eingabeschicht */
        if (have_net && nh.npeer > 0) {
            for (k = 0; k < nh.npeer; k++) {
                int p = (int)(nh.peer[k].ping + 0.5);
                if (!nh.peer[k].up || p <= 0 || p <= ping) continue;
                ping = p;
                np_name(nh.peer[k].name, nm, NP_COLS);
            }
        }
        if (ping < 0 && have_rb) {
            for (k = 0; k < lb.npeer; k++)
                if (lb.peer[k].rtt > ping) ping = lb.peer[k].rtt;
            nm[0] = 0;
        }
        if (ping > 0) {
            np_lw(nm[0] ? "ovl.np_ping" : "ovl.np_ping_n", fmt, NP_COLS);
            _snwprintf_s(n.row[n.nrow], NP_COLS, _TRUNCATE, fmt, ping, nm);
        } else {
            ping = -1;
            np_lw("ovl.np_ping_unk", n.row[n.nrow], NP_COLS);
        }
        n.col[n.nrow++] = np_ping_col(ping);
        if (ping > NP_PING_FAR) { /* W494: weite Lobby (z. B. AU-EU) - Spiel bleibt verbunden, reagiert aber spuerbar traeger */
            np_lw("ovl.np_far", n.row[n.nrow], NP_COLS);
            n.col[n.nrow++] = OVS_RGB_BAD;
        }
        /* Zeile 4: Sim/Arcade (W440) und Sichtbarkeit der Lobby */
        np_lw(lm == LAS_ARCADE ? "ovl.nh_lm_arcade" : "ovl.nh_lm_sim", a, NP_COLS);
        np_lw(vis == 2 ? "ovl.np_vis_pub" : vis == 1 ? "ovl.np_vis_code" : "ovl.np_vis_lan", b, NP_COLS);
        np_lw("ovl.np_lobby", fmt, NP_COLS);
        _snwprintf_s(n.row[n.nrow], NP_COLS, _TRUNCATE, fmt, a, b);
        n.col[n.nrow++] = lm == LAS_ARCADE ? OVS_RGB_BAD : OVS_RGB_INFO;
        /* Zeile 5: Host-Regel (W420) */
        if (have_rule) {
            np_lw(r_on ? "ovl.np_rule_on" : "ovl.np_rule_off", n.row[n.nrow], NP_COLS);
            n.col[n.nrow++] = OVS_RGB_DIM;
        }
    }
    { /* Log nur bei Aenderung, nur Zahlen (ohne Ping, der schwankt) */
        int cur[8] = { n.show, lb.net_mode, lb.prof, lb.d_start, lb.d_target, lm, vis, have_rule ? r_on : -1 };
        if (memcmp(cur, s_log, sizeof cur) != 0) {
            memcpy(s_log, cur, sizeof cur);
            HLOG_I("LOCNETPANEL: show=%d mode=%d prof=%d d=%d/%d ping=%d lm=%d vis=%d rule=%d rows=%d f=%u", cur[0], cur[1],
                   cur[2], cur[3], cur[4], ping, cur[5], cur[6], cur[7], n.nrow, f);
        }
    }
    AcquireSRWLockExclusive(&s_lock);
    n.gen = s_snap.gen;
    n.t = s_snap.t;
    if (memcmp(&n, &s_snap, sizeof n) != 0) n.gen++; /* nur neuer Inhalt zaehlt */
    n.t = now;
    s_snap = n;
    ReleaseSRWLockExclusive(&s_lock);
}

/* ------------------------------------------------------------------ Anzeige (Render-Thread) */
static ovs_canvas_t s_cv;    /* W461: GDI-Flaeche, Schriften und Textur aus overlay_style */
static ovs_font_t s_fn[2];
static ovs_tex_t s_tx;
static unsigned *s_pm;
static unsigned s_w, s_h, s_pmcap;
static sb_ovl_save_t s_save;
static u32 s_drawn_gen, s_drawn_box, s_shot_t;
static int s_tex_dirty, s_have;
static unsigned s_ox, s_oy;

int loc_netpanel_visible(void)
{
    int a;
    u32 t;
    if (!g_cfg.loc_net_panel || !g_sba_loc || g_cfg.vr_enable || sb_ovl_open()) return 0; /* W474: F3 blendet nicht aus */
    AcquireSRWLockShared(&s_lock);
    a = s_snap.show && s_snap.nrow > 0;
    t = s_snap.t;
    ReleaseSRWLockShared(&s_lock);
    return a && (u32)(GetTickCount() - t) < 2000u; /* Spiel-Thread steht: nicht stehen lassen */
}

#define np_px ovs_px

/* Inhalt (s_w x s_h, vormultipliziertes BGRA) im gemeinsamen LoC-Panel-Look (W467, overlay_style ovs_lp_*): Zeile 0 =
 * Kopfzeile (Netzmodus), darunter die Zeilen; Breite OVS_LP_W_MIN..OVS_LP_W_MAX nach Text. f = Boxhoehe / 480. 1 = ok. */
static int np_render(const np_snap *sn, float fy)
{
    unsigned w, h, tx;
    int i;
    HFONT fn[2], fold;
    HDC dc;
    SIZE sz;
    if (sn->nrow <= 0) return 0;
    dc = ovs_canvas_dc(&s_cv);
    if (!dc) return 0;
    fn[0] = ovs_font_get(&s_fn[0], OVS_FACE_COND, FW_SEMIBOLD, np_px(fy, OVS_LP_FS_HEAD));
    fn[1] = ovs_font_get(&s_fn[1], OVS_FACE_COND, FW_NORMAL, np_px(fy, OVS_LP_FS_ROW));
    if (!fn[0] || !fn[1]) return 0;
    tx = (unsigned)np_px(fy, OVS_LP_PAD + OVS_LP_INSET);
    /* Breite nach dem laengsten Text */
    fold = (HFONT)SelectObject(dc, fn[0]);
    w = (unsigned)np_px(fy, OVS_LP_W_MIN);
    for (i = 0; i < sn->nrow; i++) {
        SelectObject(dc, fn[i == 0 ? 0 : 1]);
        if (GetTextExtentPoint32W(dc, sn->row[i], (int)wcslen(sn->row[i]), &sz) &&
            (unsigned)sz.cx + tx + (unsigned)np_px(fy, OVS_LP_PAD) > w)
            w = (unsigned)sz.cx + tx + (unsigned)np_px(fy, OVS_LP_PAD);
    }
    if (w > (unsigned)np_px(fy, OVS_LP_W_MAX)) w = (unsigned)np_px(fy, OVS_LP_W_MAX); /* W453: lange Pilotnamen kuerzt DT_END_ELLIPSIS */
    h = ovs_lp_height(fy, 1, sn->nrow);
    SelectObject(dc, fold);
    if (w < 8u || h < 8u) return 0;
    if (!ovs_canvas_size(&s_cv, w, h) || !ovs_buf(&s_pm, &s_pmcap, w * h)) return 0;
    memset(s_cv.bits, 0, (size_t)w * h * 4u);
    fold = (HFONT)SelectObject(dc, fn[0]);
    SetTextColor(dc, RGB(255, 255, 255)); /* weisse Abdeckung, Farbe je Zeile in ovs_lp_finish */
    for (i = 0; i < sn->nrow; i++) {
        RECT r;
        int bot;
        r.left = (LONG)tx;
        r.right = (LONG)(w - (unsigned)np_px(fy, OVS_LP_PAD));
        r.top = (LONG)ovs_lp_row_top(fy, 1, i, &bot);
        r.bottom = (LONG)bot;
        SelectObject(dc, fn[i == 0 ? 0 : 1]);
        DrawTextW(dc, sn->row[i], -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    GdiFlush();
    SelectObject(dc, fold);
    ovs_lp_finish(s_pm, s_cv.bits, w, h, fy, 1, sn->col, sn->nrow);
    s_w = w;
    s_h = h;
    s_tex_dirty = 1;
    return 1;
}

static int np_upload(void)
{
    int r = ovs_tex_ensure(&s_tx, s_w, s_h);
    if (!r) return 0;
    if (r == 2) s_tex_dirty = 1;
    if (!s_tex_dirty) return 1;
    if (!ovs_tex_write(&s_tx, s_pm)) return 0;
    s_tex_dirty = 0;
    return 1;
}

/* W453 LOC-LOBBY-INFO: Lage unten rechts in der Ecke des Bildschirms (Backbuffer, nicht Spielbox), Rand OVS_MARGIN im
 * SB-UI-Faktor wie die anderen Eck-Overlays; ovs_layout_place stapelt nach oben weg, falls dort schon etwas angemeldet ist
 * (z. B. Wasserzeichen). Chat unten links (W452), Netz-HUD/F2 oben - nichts ueberlappt. */
#define NP_GAP OVS_LP_GAP

void loc_netpanel_draw(ID3D11RenderTargetView *rtv, int save)
{
    unsigned bx, by, bw, bh;
    float fy, fu;
    np_snap sn;
    u32 box;
    if (!loc_netpanel_visible() || !rtv || !g_dev || !g_ctx || !g_bb_w || !g_bb_h) return;
    ovs_loc_box(&bx, &by, &bw, &bh);
    if (bw < 64u || bh < 48u) return;
    fy = (float)bh / 480.0f;
    box = (u32)bw * 7919u + (u32)bh * 31u + (u32)bx * 3u + (u32)by;
    AcquireSRWLockShared(&s_lock);
    sn = s_snap;
    ReleaseSRWLockShared(&s_lock);
    if (sn.gen != s_drawn_gen || box != s_drawn_box || !s_pm) {
        s_have = np_render(&sn, fy);
        s_drawn_gen = sn.gen;
        s_drawn_box = box;
    }
    if (s_have && g_cfg.loc_net_panel_test && s_shot_n < 6 && (!s_shot_t || GetTickCount() - s_shot_t > 10000u)) {
        s_shot_t = GetTickCount() | 1u; /* TEST: hoechstens 6 Bilder, alle 10 s solange sichtbar */
        InterlockedExchange(&s_shot_req, 1);
    }
    if (!s_have || !np_upload()) return;
    fu = ovs_scale_ui(bh);
    if (!ovs_layout_place(OVS_SLOT_LOBBYINFO, OVS_BOTTOM_RIGHT, s_w, s_h, (unsigned)ovs_px(fu, OVS_MARGIN),
                          (unsigned)ovs_px(fu, NP_GAP), &s_ox, &s_oy))
        return; /* passt nicht (Fenster zu klein) */
    if (s_ox + s_w > g_bb_w || s_oy + s_h > g_bb_h) return;
    if (save) sb_ovl_save_take(&s_save, rtv, s_ox, s_oy, s_w, s_h);
    sb_ovl_quad(rtv, s_tx.srv, s_ox, s_oy, s_w, s_h);
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
