/* loc_replayctl.c - W457 LOC-REPLAY-CTRL (GitHub #88): Replay-Steuerung in Line of Contact (Beschreibung in
 * loc_replayctl.h). Zeichnung wie W447/W453 (loc_netpanel.c): GDI-Text in eine kleine Textur, ein Quad nach
 * Clip/Video/F11-Bild, Bereich gesichert. */
#include "render_int.h"
#include "sb_overlay.h"
#include "overlay_style.h"
#include "sb_nethud.h"
#include "sb_lang.h"
#include "sb_profile.h"
#include "hcfg.h"
#include "hlog.h"
#include "xdk_hooks.h"
#include "hooks_int.h" /* replay_inject (TEST) */
#include "loc_replayctl.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef uint32_t u32;

/* LoC-VAs (Dekompilat W245, LOC_BOOT §W457) */
#define LRP_MODE 0x306B30u   /* Spielmodus: 2 = Replay-Wiedergabe (FUN_0002F100 ruft den Viewer) */
#define LRP_DEMO 0x306914u   /* != 0: Titel-Demo (jede Taste beendet, keine Menueleiste) */
#define LRP_PMENU 0x3071B4u  /* 1 = Pausenmenue (START am Pad) offen: SBC-Abfrage liefert dann 0 */
#define LRP_SEL 0x306AA0u    /* gewaehlter Menuepunkt 0..8 */
#define LRP_PAUSED 0x306BBCu /* Punkt 0: 1 = angehalten (FUN_0001C940 steht) */
#define LRP_TEMPO 0x306944u  /* Punkt 1: Teiler 1..10 (1 = normal, groesser = Zeitlupe) */
#define LRP_CAM 0x3071D0u    /* Punkt 2: Kamera-Abstand 4..18 (0 = Pilotensicht) */
#define LRP_PLAYER 0x306BB8u /* Punkt 3: betrachteter Spieler */

static u32 rp_rd(u32 a)
{
    u32 v = 0;
    __try {
        v = *(volatile const u32 *)(uintptr_t)a;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        v = 0;
    }
    return v;
}

int loc_rpc_viewer(void)
{
    if (!g_sba_loc) return 0;
    return rp_rd(LRP_MODE) == 2u && rp_rd(LRP_DEMO) == 0u && rp_rd(LRP_PMENU) != 1u;
}

/* ------------------------------------------------------------------ Eingabe */
static volatile LONG s_help = -1; /* -1 = noch nicht gesetzt (cfg loc_replay_help), 0/1 */
static int s_help_prev;

void loc_rpc_apply(const imap_profile_t *prof, const imap_snap_t *snap, uint16_t b[3])
{
    int lvl[IMAP_LRP_N], i, ex, view;
    if (!g_sba_loc || !prof || !snap || !b) return;
    view = loc_rpc_viewer();
    for (i = 0; i < IMAP_LRP_N; i++) lvl[i] = view ? imap_action_lrp(prof, snap, i) : 0;
    ex = view ? imap_action_replay_exit(prof, snap) : 0;
    if (lvl[IMAP_LRP_HELP] && !s_help_prev) {
        LONG h = s_help < 0 ? (g_cfg.loc_replay_help ? 1 : 0) : s_help;
        InterlockedExchange(&s_help, h ? 0 : 1);
        HLOG_I("LOCREPLAY: Bedienhilfe %s (f=%u)", h ? "aus" : "an", hooks_frame_count());
    }
    s_help_prev = lvl[IMAP_LRP_HELP];
    if (view) (void)imap_lrp_bits(lvl, ex, (int)rp_rd(LRP_SEL), b);
}

/* ------------------------------------------------------------------ Hilfe-Zeilen (Spiel-Thread -> Render-Thread) */
#define RP_ROWS 10
#define RP_COLS 112

typedef struct rp_snap {
    int show, nrow;
    wchar_t row[RP_ROWS][RP_COLS];
    unsigned col[RP_ROWS];
    u32 t, gen;
} rp_snap;

static SRWLOCK s_lock = SRWLOCK_INIT;
static rp_snap s_snap;
static imap_profile_t s_prof;
static volatile LONG s_have_prof;
static volatile LONG s_shot_req, s_shot_n;

void loc_rpc_set_profile(const imap_profile_t *prof)
{
    if (!prof) return;
    AcquireSRWLockExclusive(&s_lock);
    s_prof = *prof;
    ReleaseSRWLockExclusive(&s_lock);
    InterlockedExchange(&s_have_prof, 1);
}

static void rp_lw(const char *key, wchar_t *o, int cap)
{
    if (sb_lang_w(key, o, cap) != 0) o[0] = 0;
}

/* Tastenname (wie das ESC-Menue; Pfeile als Zeichen, erweiterte Tasten mit Bit 24 fuer GetKeyNameText). */
static void rp_vk_name(int vk, int mods, wchar_t *o, int cap)
{
    wchar_t k[48];
    UINT sc;
    k[0] = 0;
    if (vk == VK_LEFT) wcscpy_s(k, 48, L"\x2190");
    else if (vk == VK_UP) wcscpy_s(k, 48, L"\x2191");
    else if (vk == VK_RIGHT) wcscpy_s(k, 48, L"\x2192");
    else if (vk == VK_DOWN) wcscpy_s(k, 48, L"\x2193");
    else if (vk >= VK_F1 && vk <= VK_F24) _snwprintf_s(k, 48, _TRUNCATE, L"F%d", vk - VK_F1 + 1);
    else if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) _snwprintf_s(k, 48, _TRUNCATE, L"%c", vk);
    else if (vk == VK_PRIOR) wcscpy_s(k, 48, L"PgUp"); /* feste Kurznamen statt Tastatursprache (BILD-NACH-OBEN) */
    else if (vk == VK_NEXT) wcscpy_s(k, 48, L"PgDn");
    else if (vk == VK_SPACE) wcscpy_s(k, 48, L"Space");
    else if (vk == VK_BACK) wcscpy_s(k, 48, L"Backspace");
    else if (vk == VK_RETURN) wcscpy_s(k, 48, L"Enter");
    else if (vk == VK_TAB) wcscpy_s(k, 48, L"Tab");
    else if (vk == VK_ESCAPE) wcscpy_s(k, 48, L"Esc");
    else if (vk == VK_INSERT) wcscpy_s(k, 48, L"Ins");
    else if (vk == VK_DELETE) wcscpy_s(k, 48, L"Del");
    else if (vk == VK_HOME) wcscpy_s(k, 48, L"Home");
    else if (vk == VK_END) wcscpy_s(k, 48, L"End");
    else {
        LONG ext = (vk == VK_PRIOR || vk == VK_NEXT || vk == VK_HOME || vk == VK_END || vk == VK_INSERT ||
                    vk == VK_DELETE) ? (1L << 24) : 0;
        sc = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
        if (!sc || GetKeyNameTextW((LONG)(sc << 16) | ext, k, 48) <= 0) _snwprintf_s(k, 48, _TRUNCATE, L"0x%02X", vk);
    }
    _snwprintf_s(o, cap, _TRUNCATE, L"%ls%ls%ls%ls", (mods & 1) ? L"\x21E7" : L"", (mods & 2) ? L"Ctrl+" : L"",
                 (mods & 4) ? L"Alt+" : L"", k);
}

/* Erste Tastatur-Belegung einer Bindung, sonst "-". */
static void rp_bind_name(const imap_bind_t *b, wchar_t *o, int cap)
{
    int vks[4], mods[4], n = imap_bind_key_combos(b, vks, mods, 4);
    if (n <= 0) {
        wcscpy_s(o, cap, L"\x2013");
        return;
    }
    rp_vk_name(vks[0], mods[0], o, cap);
}

static void rp_build(rp_snap *n, int help)
{
    wchar_t fmt[RP_COLS], a[32], b[32], c[32];
    imap_profile_t *p = &s_prof;
    rp_bind_name(&p->lrp[IMAP_LRP_HELP], a, 32);
    if (!help) { /* eingeklappt: nur der Hinweis auf die Hilfe-Taste */
        rp_lw("ovl.lrp_hint", fmt, RP_COLS);
        _snwprintf_s(n->row[n->nrow], RP_COLS, _TRUNCATE, fmt, a);
        n->col[n->nrow++] = OVS_RGB_DIM;
        return;
    }
    rp_lw("ovl.lrp_title", fmt, RP_COLS);
    _snwprintf_s(n->row[n->nrow], RP_COLS, _TRUNCATE, fmt, a);
    n->col[n->nrow++] = OVS_RGB_ACCENT;
    rp_bind_name(&p->lrp[IMAP_LRP_PAUSE], a, 32);
    rp_lw(rp_rd(LRP_PAUSED) ? "ovl.lrp_pause_on" : "ovl.lrp_pause", fmt, RP_COLS);
    _snwprintf_s(n->row[n->nrow], RP_COLS, _TRUNCATE, fmt, a);
    n->col[n->nrow++] = rp_rd(LRP_PAUSED) ? OVS_RGB_MID : OVS_RGB_TEXT;
    rp_bind_name(&p->lrp[IMAP_LRP_NEXT], a, 32);
    rp_bind_name(&p->lrp[IMAP_LRP_PREV], b, 32);
    rp_lw("ovl.lrp_player", fmt, RP_COLS);
    _snwprintf_s(n->row[n->nrow], RP_COLS, _TRUNCATE, fmt, a, b);
    n->col[n->nrow++] = OVS_RGB_TEXT;
    rp_bind_name(&p->lrp[IMAP_LRP_CAMERA], a, 32);
    rp_lw("ovl.lrp_camera", fmt, RP_COLS);
    _snwprintf_s(n->row[n->nrow], RP_COLS, _TRUNCATE, fmt, a);
    n->col[n->nrow++] = OVS_RGB_TEXT;
    {
        u32 t = rp_rd(LRP_TEMPO);
        rp_bind_name(&p->lrp[IMAP_LRP_SPEED], a, 32);
        rp_lw("ovl.lrp_speed", fmt, RP_COLS);
        _snwprintf_s(n->row[n->nrow], RP_COLS, _TRUNCATE, fmt, a, (t >= 1u && t <= 10u) ? (int)t : 1);
        n->col[n->nrow++] = (t > 1u && t <= 10u) ? OVS_RGB_MID : OVS_RGB_TEXT;
    }
    rp_bind_name(&p->axis[IMAP_AX_SIGHT_X].neg, a, 32);
    rp_bind_name(&p->axis[IMAP_AX_SIGHT_X].pos, b, 32);
    rp_bind_name(&p->axis[IMAP_AX_SIGHT_Y].neg, c, 32);
    rp_lw("ovl.lrp_look", fmt, RP_COLS);
    {
        wchar_t d[32];
        rp_bind_name(&p->axis[IMAP_AX_SIGHT_Y].pos, d, 32);
        _snwprintf_s(n->row[n->nrow], RP_COLS, _TRUNCATE, fmt, a, b, c, d);
    }
    n->col[n->nrow++] = OVS_RGB_TEXT;
    rp_bind_name(&p->axis[IMAP_AX_ROT].neg, a, 32);
    rp_bind_name(&p->axis[IMAP_AX_ROT].pos, b, 32);
    rp_bind_name(&p->btn[LRP_BI_MAIN], c, 32);
    rp_lw("ovl.lrp_menu", fmt, RP_COLS);
    _snwprintf_s(n->row[n->nrow], RP_COLS, _TRUNCATE, fmt, a, b, c);
    n->col[n->nrow++] = OVS_RGB_TEXT;
    rp_bind_name(&p->replay_exit, a, 32);
    rp_lw("ovl.lrp_exit", fmt, RP_COLS);
    _snwprintf_s(n->row[n->nrow], RP_COLS, _TRUNCATE, fmt, a);
    n->col[n->nrow++] = OVS_RGB_TEXT;
    rp_lw("ovl.lrp_sbc", n->row[n->nrow], RP_COLS);
    n->col[n->nrow++] = OVS_RGB_DIM;
}

/* Zustand fuer Log und TEST-Bilder (nur Spielzustand, keine Eingaben) */
static u32 s_st[6] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
static int s_view_prev = -1, s_test_demo_done;

void loc_rpc_frame(unsigned f)
{
    rp_snap n;
    int view, help;
    u32 cur[6];
    if (!g_sba_loc) return;
    if (g_cfg.loc_replay_ctrl_test == 1 && !s_test_demo_done && rp_rd(LRP_MODE) == 2u && rp_rd(LRP_DEMO) != 0u) {
        /* TEST: Titel-Demo (eingebautes Replay) als bedienbarer Viewer - ohne Spielstand mit gespeichertem Replay */
        __try {
            *(volatile u32 *)(uintptr_t)LRP_DEMO = 0;
            s_test_demo_done = 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        HLOG_I("LOCREPLAY: TEST Titel-Demo -> bedienbarer Viewer (Demo-Flag 0, f=%u)", f);
    }
    view = loc_rpc_viewer();
    if (g_cfg.loc_replay_ctrl_test == 1 && g_cfg.input_test_hook) { /* TEST: feste Folge ueber die Test-Slots */
        static const struct { unsigned at; int vk, len; } sq[] = {
            {300, 'P', 6}, {540, 'P', 6}, {720, 'T', 6}, {900, 'G', 6}, {1080, VK_PRIOR, 6}, {1260, VK_NEXT, 6},
            {1440, VK_NEXT, 6}, {1620, VK_F1, 6}, {1800, VK_F1, 6}, {2000, VK_BACK, 90}};
        static unsigned t0;
        static int si;
        if (view && !t0) t0 = f;
        if (t0 && si < (int)(sizeof sq / sizeof sq[0]) && f >= t0 + sq[si].at) {
            replay_inject(sq[si].vk, sq[si].len);
            HLOG_I("LOCREPLAY: TEST Schritt %d: Testtaste vk=%d (%d Bilder, f=%u)", si + 1, sq[si].vk, sq[si].len, f);
            si++;
        }
        if (t0 && (f == t0 + 240u || f == t0 + 1700u)) InterlockedExchange(&s_shot_req, 1); /* Hilfe an / aus */
    }
    if (view != s_view_prev) {
        s_view_prev = view;
        HLOG_I("LOCREPLAY: Viewer %s (modus=%u demo=%u pausenmenue=%u f=%u)", view ? "an" : "aus", rp_rd(LRP_MODE),
               rp_rd(LRP_DEMO), rp_rd(LRP_PMENU), f);
        if (view && g_cfg.loc_replay_ctrl_test) InterlockedExchange(&s_shot_req, 1);
    }
    if (view) {
        cur[0] = rp_rd(LRP_PAUSED);
        cur[1] = rp_rd(LRP_TEMPO);
        cur[2] = rp_rd(LRP_CAM);
        cur[3] = rp_rd(LRP_PLAYER);
        cur[4] = rp_rd(LRP_SEL);
        cur[5] = (u32)(s_help < 0 ? g_cfg.loc_replay_help : s_help);
        if (memcmp(cur, s_st, sizeof cur) != 0) {
            HLOG_I("LOCREPLAY: pause=%u tempo=%u kamera=%u spieler=%u menuepunkt=%u hilfe=%u f=%u", cur[0], cur[1],
                   cur[2], cur[3], cur[4], cur[5], f);
            if (g_cfg.loc_replay_ctrl_test && s_st[0] != 0xFFFFFFFFu) InterlockedExchange(&s_shot_req, 1);
            memcpy(s_st, cur, sizeof cur);
        }
    }
    if ((f % 15u) != 0u) return; /* Zeilen 4x/s */
    memset(&n, 0, sizeof n);
    help = s_help < 0 ? g_cfg.loc_replay_help : (int)s_help;
    n.show = view && s_have_prof;
    if (n.show) {
        AcquireSRWLockShared(&s_lock);
        rp_build(&n, help);
        ReleaseSRWLockShared(&s_lock);
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
static ovs_font_t s_fn[2];
static ovs_tex_t s_tx;
static unsigned *s_pm;
static unsigned s_w, s_h, s_pmcap;
static sb_ovl_save_t s_save;
static u32 s_drawn_gen, s_drawn_box;
static int s_tex_dirty, s_have;
static unsigned s_ox, s_oy;

int loc_rpc_visible(void)
{
    int a;
    u32 t;
    if (!g_sba_loc || g_cfg.vr_enable || sb_ovl_open()) return 0;
    AcquireSRWLockShared(&s_lock);
    a = s_snap.show && s_snap.nrow > 0;
    t = s_snap.t;
    ReleaseSRWLockShared(&s_lock);
    return a && (u32)(GetTickCount() - t) < 2000u;
}

/* Inhalt (vormultipliziertes BGRA) im gemeinsamen LoC-Panel-Look (W467, overlay_style ovs_lp_*): ausgeklappt Zeile 0 =
 * Kopfzeile (Titel), eingeklappt nur die Hinweiszeile ohne Kopf. Breite nach Text bis OVS_LP_W_WIDE (lange
 * Tastenzeilen). fy = Boxhoehe / 480. */
static int rp_render(const rp_snap *sn, float fy)
{
    unsigned w, h, tx, rp;
    int i, head;
    HFONT fn[2], fold;
    HDC dc;
    SIZE sz;
    if (sn->nrow <= 0) return 0;
    head = sn->nrow > 1;
    dc = ovs_canvas_dc(&s_cv);
    if (!dc) return 0;
    fn[0] = ovs_font_get(&s_fn[0], OVS_FACE_COND, FW_SEMIBOLD, ovs_px(fy, OVS_LP_FS_HEAD));
    fn[1] = ovs_font_get(&s_fn[1], OVS_FACE_COND, FW_NORMAL, ovs_px(fy, OVS_LP_FS_ROW));
    if (!fn[0] || !fn[1]) return 0;
    tx = (unsigned)ovs_px(fy, OVS_LP_PAD + OVS_LP_INSET);
    rp = (unsigned)ovs_px(fy, OVS_LP_PAD);
    fold = (HFONT)SelectObject(dc, fn[0]);
    w = (unsigned)ovs_px(fy, head ? OVS_LP_W_MIN : OVS_LP_W_HINT);
    for (i = 0; i < sn->nrow; i++) {
        SelectObject(dc, fn[head && i == 0 ? 0 : 1]);
        if (GetTextExtentPoint32W(dc, sn->row[i], (int)wcslen(sn->row[i]), &sz) && (unsigned)sz.cx + tx + rp > w)
            w = (unsigned)sz.cx + tx + rp;
    }
    if (w > (unsigned)ovs_px(fy, OVS_LP_W_WIDE)) w = (unsigned)ovs_px(fy, OVS_LP_W_WIDE);
    h = ovs_lp_height(fy, head, sn->nrow);
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
        r.right = (LONG)(w - rp);
        r.top = (LONG)ovs_lp_row_top(fy, head, i, &bot);
        r.bottom = (LONG)bot;
        SelectObject(dc, fn[head && i == 0 ? 0 : 1]);
        DrawTextW(dc, sn->row[i], -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    GdiFlush();
    SelectObject(dc, fold);
    ovs_lp_finish(s_pm, s_cv.bits, w, h, fy, head, sn->col, sn->nrow);
    s_w = w;
    s_h = h;
    s_tex_dirty = 1;
    return 1;
}

static int rp_upload(void)
{
    int r = ovs_tex_ensure(&s_tx, s_w, s_h);
    if (!r) return 0;
    if (r == 2) s_tex_dirty = 1;
    if (!s_tex_dirty) return 1;
    if (!ovs_tex_write(&s_tx, s_pm)) return 0;
    s_tex_dirty = 0;
    return 1;
}

/* Lage oben rechts in der Ecke des Bildschirms (die Menueleiste des Viewers liegt unten), unter F2/F3, falls offen. */
void loc_rpc_draw(ID3D11RenderTargetView *rtv, int save)
{
    unsigned bx, by, bw, bh;
    float fy, fu;
    rp_snap sn;
    u32 box;
    if (!loc_rpc_visible() || !rtv || !g_dev || !g_ctx || !g_bb_w || !g_bb_h) return;
    ovs_loc_box(&bx, &by, &bw, &bh);
    if (bw < 64u || bh < 48u) return;
    fy = (float)bh / 480.0f;
    box = (u32)bw * 7919u + (u32)bh * 31u + (u32)bx * 3u + (u32)by;
    AcquireSRWLockShared(&s_lock);
    sn = s_snap;
    ReleaseSRWLockShared(&s_lock);
    if (sn.gen != s_drawn_gen || box != s_drawn_box || !s_pm) {
        s_have = rp_render(&sn, fy);
        s_drawn_gen = sn.gen;
        s_drawn_box = box;
    }
    if (!s_have || !rp_upload()) return;
    fu = ovs_scale_ui(bh);
    if (!ovs_layout_place(OVS_SLOT_REPLAYHELP, OVS_TOP_RIGHT, s_w, s_h, (unsigned)ovs_px(fu, OVS_MARGIN),
                          (unsigned)ovs_px(fu, OVS_LP_GAP), &s_ox, &s_oy))
        return;
    if (s_ox + s_w > g_bb_w || s_oy + s_h > g_bb_h) return;
    if (save) sb_ovl_save_take(&s_save, rtv, s_ox, s_oy, s_w, s_h);
    sb_ovl_quad(rtv, s_tx.srv, s_ox, s_oy, s_w, s_h);
}

void loc_rpc_restore(void)
{
    if (!s_save.saved) return;
    sb_ctx_enter();
    sb_ovl_save_put(&s_save);
    sb_ctx_leave();
}

int loc_rpc_shot_due(char *name, unsigned cap)
{
    if (!s_shot_req || !name || cap < 16u || s_shot_n >= 16) return 0;
    InterlockedExchange(&s_shot_req, 0);
    _snprintf_s(name, cap, _TRUNCATE, "w457_replay_%02ld", InterlockedIncrement(&s_shot_n));
    return 1;
}
