/* sb_nethud.c - W374 LOC-NET-OVERLAY: Netz-Overlay (F3). Beschreibung s. sb_nethud.h.
 *
 * Threads: UI-Thread meldet Tasten (Interlocked), Render-Pfad (sb_phud_tick_begin/sb_phud_draw) uebernimmt
 * Anforderungen und zeichnet, ein eigener Abtast-Thread liest bei sichtbarem Panel alle 500 ms die Zaehler
 * und setzt die Textzeilen (SRW-Sperre). Nichts davon laeuft im Spiel-Thread ausser zwei int-Tests je Tick. */
#include "render_int.h"
#include "sb_overlay.h"
#include "sb_perfhud.h"
#include "sb_nethud.h"
#include "loc_rulenet.h" /* W420 */
#include "sb_lang.h"
#include "loc_net.h"
#include "loc_rollback.h"
#include "loc_smooth.h" /* W387 */
#include "loc_lsarb_game.h" /* LOC-LOCKSTEP-ARBITER */
#include <stdlib.h>
#include <wchar.h>

int sb_ovl_open(void);

/* ---- Zustand ---- */
static volatile LONG nh_req_toggle, nh_req_pos; /* UI/Test -> Render */
static volatile LONG nh_vk[4] = { VK_F3, 0, 0, 0 }, nh_mod[4]; /* Belegung (Standard F3 bis zum Profil-Laden) */
static volatile int nh_vis;
static int nh_init, nh_pos;
static char nh_save_path[600];

/* Textzeilen (Abtast-Thread -> Render) */
#define NH_ROWS 16
#define NH_COLS 64
#define NH_C_WARN RGB(255, 176, 64)
typedef struct {
    int n;
    wchar_t t[NH_ROWS][NH_COLS];
    unsigned char warn[NH_ROWS];
} nh_text_t;
static SRWLOCK nh_lk = SRWLOCK_INIT;
static nh_text_t nh_txt;
static volatile LONG nh_ver;
static HANDLE nh_thr, nh_wake;

/* Panel (GDI, Look wie sb_perfhud.c) */
static HDC nh_dc;
static HBITMAP nh_bmp, nh_bmp_old;
static unsigned *nh_bits;
static unsigned nh_bw, nh_bh;
static HFONT nh_font, nh_font_old;
static int nh_font_px;
static float nh_f = 1.0f;
static ID3D11Texture2D *nh_tex;
static ID3D11ShaderResourceView *nh_srv;
static unsigned nh_tw, nh_th;
static LONG nh_painted = -1;
static int nh_paint_rows;
static sb_ovl_save_t nh_save;

/* ------------------------------------------------------------------ */
/* Tasten (UI-Thread)                                                   */
/* ------------------------------------------------------------------ */
void sb_nhud_set_binding(const int *vks, const int *mods, int n)
{
    int i;
    for (i = 0; i < 4; i++) {
        InterlockedExchange(&nh_vk[i], i < n && vks ? vks[i] : 0);
        InterlockedExchange(&nh_mod[i], i < n && mods ? mods[i] : 0);
    }
    HLOG_I("NETHUD: Belegung net_hud: Tasten 0x%02X/%ld 0x%02X/%ld (%d)", (unsigned)nh_vk[0], nh_mod[0],
           (unsigned)nh_vk[1], nh_mod[1], n);
}

static int nh_mods_now(void)
{
    return ((GetKeyState(VK_SHIFT) & 0x8000) ? 1 : 0) | ((GetKeyState(VK_CONTROL) & 0x8000) ? 2 : 0) |
           ((GetKeyState(VK_MENU) & 0x8000) ? 4 : 0);
}

int sb_nhud_wndproc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    int i, m, hit = 0;
    (void)w;
    if (msg != WM_KEYDOWN && msg != WM_SYSKEYDOWN && msg != WM_KEYUP && msg != WM_SYSKEYUP) { return 0; }
    for (i = 0; i < 4; i++) {
        if (nh_vk[i] && (WPARAM)nh_vk[i] == wp) { break; }
    }
    if (i == 4) { return 0; }
    /* offenes ESC-Menue: nur F-Tasten (Buchstaben gehoeren dem Menue, z. B. Texteingabe) */
    if (sb_ovl_open() && !(wp >= VK_F1 && wp <= VK_F24)) { return 0; }
    if (msg == WM_KEYUP || msg == WM_SYSKEYUP) { return 1; }
    if (lp & 0x40000000) { return 1; } /* keine Autowiederholung */
    m = nh_mods_now();
    for (i = 0; i < 4; i++) {
        if (!nh_vk[i] || (WPARAM)nh_vk[i] != wp) { continue; }
        if (nh_mod[i] == m) { hit = 1; break; }
        if (!(nh_mod[i] & 1) && (nh_mod[i] | 1) == m) { hit = 2; } /* Umschalt + Taste: Ecke */
    }
    if (hit == 1) { InterlockedIncrement(&nh_req_toggle); }
    else if (hit == 2) { InterlockedIncrement(&nh_req_pos); }
    else { return 0; }
    return 1;
}

void sb_nhud_toggle(void) { InterlockedIncrement(&nh_req_toggle); }
void sb_nhud_cycle_pos(void) { InterlockedIncrement(&nh_req_pos); }
int sb_nhud_visible(void) { return nh_vis; }

/* ------------------------------------------------------------------ */
/* Abtasten (eigener Thread, nur bei sichtbarem Panel, alle 500 ms)    */
/* ------------------------------------------------------------------ */
typedef struct { DWORD t; unsigned rb, taken, pred; } nh_hist_t;
#define NH_HIST 24
static nh_hist_t nh_hist[NH_HIST];
static unsigned nh_hn;
/* letzte Werte (Belegzeile LOG) */
static loc_net_hud nh_last_net;
static loc_rb_hud nh_last_rb;
static int nh_last_have_net, nh_last_have_rb;
static double nh_last_rbps, nh_last_pred;

static void nh_lw(const char *key, wchar_t *o, int cap)
{
    if (sb_lang_w(key, o, cap) != 0) { o[0] = 0; } /* 0 = ok */
}

static void nh_add(nh_text_t *x, int warn, const wchar_t *fmt, ...)
{
    va_list ap;
    if (x->n >= NH_ROWS) { return; }
    va_start(ap, fmt);
    _vsnwprintf_s(x->t[x->n], NH_COLS, _TRUNCATE, fmt, ap);
    va_end(ap);
    x->warn[x->n] = (unsigned char)(warn ? 1 : 0);
    x->n++;
}

/* W395: Relayname gross (ASCII, hoechstens 8 Zeichen), z. B. "eu" -> "EU" */
static void nh_up(const char *s, wchar_t *o, int cap)
{
    int k = 0;
    for (; s && *s && k < 8 && k < cap - 1; s++) {
        char ch = *s;
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
        if (ch >= 0x21 && ch <= 0x7E) o[k++] = (wchar_t)ch;
    }
    o[k] = 0;
}

/* W395: "EU-Relay" bzw. "EU relay" (Sprachdatei ovl.nh_r_relayn mit %s) */
static void nh_relay_label(const char *name, wchar_t *o, int cap)
{
    wchar_t up[12], fmt[48];
    nh_up(name, up, 12);
    nh_lw("ovl.nh_r_relayn", fmt, 48);
    if (!fmt[0]) { _snwprintf_s(o, (size_t)cap, _TRUNCATE, L"%s", up); return; }
    _snwprintf_s(o, (size_t)cap, _TRUNCATE, fmt, up);
}

/* Pilotname (UTF-8) gekuerzt auf 10 Zeichen, nur druckbare Zeichen */
static void nh_name(const char *u8, int idx, wchar_t *o, int cap)
{
    wchar_t w[32];
    int i, k = 0;
    if (!u8 || !u8[0] || !MultiByteToWideChar(CP_UTF8, 0, u8, -1, w, 32)) {
        _snwprintf_s(o, (size_t)cap, _TRUNCATE, L"P%d", idx + 1);
        return;
    }
    for (i = 0; w[i] && k < 10 && k < cap - 1; i++) {
        if (w[i] >= 0x20 && w[i] != 0x7F) { o[k++] = w[i]; }
    }
    o[k] = 0;
    if (!k) { _snwprintf_s(o, (size_t)cap, _TRUNCATE, L"P%d", idx + 1); }
}

static void nh_sample(void)
{
    nh_text_t x;
    loc_net_hud nt;
    loc_rb_hud rb;
    wchar_t f[96], a[48], b[48];
    int have_net, have_rb, i, peers;
    double rbps = -1.0, pred = -1.0;
    DWORD now = GetTickCount();
    memset(&x, 0, sizeof x);
    have_net = loc_net_hud_read(&nt);
    have_rb = have_net ? loc_rb_hud_read(&rb) : 0;
    if (!have_rb) { memset(&rb, 0, sizeof rb); rb.npeers = -1; }
    if (have_rb) { /* Fenster ~10 s aus der eigenen Abtast-Geschichte */
        nh_hist_t *h = &nh_hist[nh_hn % NH_HIST], *o = NULL;
        unsigned k;
        h->t = now; h->rb = rb.rb_total; h->taken = rb.taken; h->pred = rb.predicted;
        nh_hn++;
        for (k = 1; k < nh_hn && k < NH_HIST; k++) {
            nh_hist_t *c = &nh_hist[(nh_hn - 1u - k) % NH_HIST];
            if (now - c->t > 10500u) { break; }
            o = c;
        }
        if (o && now != o->t) {
            rbps = (double)(rb.rb_total - o->rb) * 1000.0 / (double)(now - o->t);
            if (rb.taken != o->taken) { pred = 100.0 * (double)(rb.predicted - o->pred) / (double)(rb.taken - o->taken); }
        }
    } else {
        nh_hn = 0;
    }
    if (!have_net) {
        nh_lw("ovl.nh_offline", f, 96);
        nh_add(&x, 0, L"%s", f);
    } else {
        /* Weg + Mitspieler */
        peers = nt.tun ? nt.up : (rb.npeers > 0 ? rb.npeers : 0);
        nh_lw(!nt.tun ? "ovl.nh_r_tcp" : nt.route == 1 ? "ovl.nh_r_direct" : nt.route == 2 ? "ovl.nh_r_relay" :
              nt.route == 3 ? "ovl.nh_r_mixed" : "ovl.nh_r_none", a, 48);
        if (nt.tun && nt.route == 2 && nt.route_name[0]) { /* W395: welches Relay ("EU-Relay" / "EU relay") */
            nh_relay_label(nt.route_name, a, 48);
        }
        nh_lw("ovl.nh_route", f, 96);
        nh_add(&x, 0, f, a, peers);
        if (nt.tun && nt.up > 0 && nt.nalt > 0) { /* W395: alle gemessenen Wege mit RTT, * = aktiv */
            wchar_t lst[64], one[24], dn[16];
            int k, o = 0;
            nh_lw("ovl.nh_p_down", dn, 16);
            lst[0] = 0;
            for (k = 0; k < nt.nalt && o < 60; k++) {
                if (strcmp(nt.alt[k].name, "direct") == 0) nh_lw("ovl.nh_r_direct", one, 24);
                else nh_up(nt.alt[k].name, one, 24);
                if (nt.alt[k].state == 0 || nt.alt[k].rtt < 0) {
                    o += _snwprintf_s(lst + o, (size_t)(64 - o), _TRUNCATE, L"%s%s %s%s", k ? L" \x00B7 " : L"", one, dn,
                                      nt.alt[k].active ? L"*" : L"");
                } else {
                    o += _snwprintf_s(lst + o, (size_t)(64 - o), _TRUNCATE, L"%s%s %d%s", k ? L" \x00B7 " : L"", one,
                                      nt.alt[k].rtt, nt.alt[k].active ? L"*" : L"");
                }
                if (o < 0) break;
            }
            nh_lw("ovl.nh_paths", f, 96);
            nh_add(&x, 0, f, lst, nt.nswitch);
        }
        /* Ping, Schwankung, Verlust */
        if (nt.tun && nt.up > 0) {
            nh_lw("ovl.nh_ping", f, 96);
            nh_add(&x, nt.ping > 150.0, f, (int)(nt.ping + 0.5), (int)(nt.ping_min + 0.5), (int)(nt.jitter + 0.5));
            nh_lw("ovl.nh_loss", f, 96);
            nh_add(&x, nt.loss_in >= 20 || nt.loss_out >= 20, f, nt.loss_in / 10, nt.loss_in % 10, nt.loss_out / 10,
                   nt.loss_out % 10);
        } else if (rb.rtt_ms > 0) {
            nh_lw("ovl.nh_ping1", f, 96);
            nh_add(&x, rb.rtt_ms > 150, f, rb.rtt_ms);
        } else if (nt.tun) {
            nh_lw("ovl.nh_nopeer", f, 96);
            nh_add(&x, 0, L"%s", f);
        }
        /* Modus, Grund, Verzoegerung */
        nh_lw(!have_rb ? "ovl.nh_m_off" : rb.mode == 1 ? "ovl.nh_m_ls" : rb.mode == 2 ? "ovl.nh_m_rb" :
              rb.mode == 3 ? "ovl.nh_m_rbend" : rb.mode == 4 ? "ovl.nh_m_offer" : rb.mode == 5 ? "ovl.nh_m_wait" :
              "ovl.nh_m_lobby", a, 48);
        nh_lw("ovl.nh_mode", f, 96);
        nh_add(&x, have_rb && rb.mode == 1, f, a);
        if (have_rb && rb.mode == 1 && rb.why > 1 && rb.why_key) {
            wchar_t r[64];
            nh_lw(rb.why_key, r, 64);
            nh_lw("ovl.nh_why", f, 96);
            nh_add(&x, 0, f, r);
        }
        { /* W420 LOC-CLIPDUMP: Match-Regel des Hosts */
            int ron = 0, rsrc = 0, rmis = 0;
            if (loc_rule_hud(&ron, &rsrc, &rmis)) {
                nh_lw(ron ? "ovl.nh_rule_on" : "ovl.nh_rule_off", a, 48);
                nh_lw(rsrc == 0 ? "ovl.nh_rule_local" : "ovl.nh_rule_host", b, 48);
                nh_lw("ovl.nh_rule", f, 96);
                nh_add(&x, 0, f, a, b);
            }
        }
        if (have_rb && rb.delay > 0) {
            nh_lw("ovl.nh_delay", f, 96);
            nh_add(&x, rb.delay >= 4, f, rb.delay, rb.delay * 50);
        }
        { /* LOC-LOCKSTEP-ARBITER: Vorlauf im Lockstep (wirksame Eingabeverzoegerung) */
            int lm = 0, ll = 0, ltk = 0;
            if ((!have_rb || rb.mode == 1) && loc_lsarb_hud(&lm, &ll, &ltk) && ll > 0) {
                wchar_t c[32], d[32];
                nh_lw(lm ? "ovl.nh_ls_new" : "ovl.nh_ls_orig", c, 32);
                d[0] = 0;
                if (ltk) nh_lw("ovl.nh_ls_tk", d, 32);
                nh_lw("ovl.nh_lsarb", f, 96);
                nh_add(&x, ll >= 5, f, ll, ll * 50, c, d);
            }
        }
        /* Rollbacks/s, Tiefe, Vorhersage, Vorsprung (nur Rollback-Modus) */
        if (have_rb && (rb.mode == 2 || rb.mode == 3)) {
            nh_lw("ovl.nh_rb", f, 96);
            nh_add(&x, rbps > 5.0, f, rbps < 0.0 ? 0.0 : rbps, (int)rb.depth_p95);
            nh_lw("ovl.nh_pred", f, 96);
            nh_add(&x, pred > 50.0, f, pred < 0.0 ? 0 : (int)(pred + 0.5),
                   (rb.adv > -0.05 && rb.adv < 0.05) ? 0.0 : rb.adv); /* kein "-0.0" */
            if (rb.tsync == 2 && rb.buf_ms > -999) { /* W372: Puffer und Zeitabgleich in ms */
                nh_lw("ovl.nh_tsync", f, 96);
                nh_add(&x, rb.buf_ms < 0, f, rb.buf_ms, (rb.tsync_ms > -0.5 && rb.tsync_ms < 0.5) ? 0.0 : rb.tsync_ms);
            }
            { /* W387 LOC-SMOOTH-OPP: aktuelle Anzeige-Verzoegerung anderer VTs */
                int sm = 0, sms = 0;
                if (loc_smooth_opp_hud(&sm, &sms) && sm > 0) {
                    nh_lw(sm == 2 ? "ovl.nh_sm_fix" : "ovl.nh_sm_auto", a, 48);
                    nh_lw("ovl.nh_smooth", f, 96);
                    nh_add(&x, 0, f, a, sms);
                }
            }
            { /* W390 LOC-OWN-PREDICT: eigene Lage voraus, Vorsprung und Kosten je Tick */
                int om = 0, oms = 0, ous = 0;
                if (loc_rb_ownpred_hud(&om, &oms, &ous)) {
                    nh_lw(om == 1 ? "ovl.nh_op_on" : (om == 2 ? "ovl.nh_op_pause" : "ovl.nh_op_off"), a, 48);
                    nh_lw("ovl.nh_ownpred", f, 96);
                    nh_add(&x, om != 1, f, a, om == 1 ? oms : 0, ous / 1000, (ous / 100) % 10);
                }
            }
            { /* W394 LOC-HUD-VIEW: HUD-Marken zur vorgezeichneten Kamera, Sicht/Fadenkreuz je Bild */
                nh_lw(g_cfg.loc_hud_predict ? "ovl.nh_hv_on" : "ovl.nh_hv_off", a, 48);
                nh_lw(g_cfg.loc_view_perframe && g_cfg.loc_local_aim ? "ovl.nh_hv_on" : "ovl.nh_hv_off", b, 48);
                nh_lw("ovl.nh_hudview", f, 96);
                nh_add(&x, 0, f, a, b);
            }
        }
        /* je Mitspieler (ab zwei) */
        if (nt.tun && nt.known >= 2) {
            for (i = 0; i < nt.npeer; i++) {
                nh_name(nt.peer[i].name, i, a, 16);
                if (nt.peer[i].up) { nh_add(&x, nt.peer[i].ping > 150.0, L"· %-10s %4d ms", a, (int)(nt.peer[i].ping + 0.5)); }
                else { nh_lw("ovl.nh_down", b, 48); nh_add(&x, 1, L"· %-10s %s", a, b); }
            }
        }
    }
    nh_last_net = nt;
    nh_last_rb = rb;
    nh_last_have_net = have_net;
    nh_last_have_rb = have_rb;
    nh_last_rbps = rbps;
    nh_last_pred = pred;
    AcquireSRWLockExclusive(&nh_lk);
    nh_txt = x;
    ReleaseSRWLockExclusive(&nh_lk);
    InterlockedIncrement(&nh_ver);
}

static DWORD WINAPI nh_thread(void *arg)
{
    (void)arg;
    for (;;) {
        if (nh_vis) { nh_sample(); }
        WaitForSingleObject(nh_wake, 500);
    }
}

void sb_nhud_test_log(void)
{
    if (!nh_vis) {
        HLOG_I("NETHUD test: aus");
        return;
    }
    /* Werte der letzten Abtastung (keine neue; dieselben wie im Bild) */
    HLOG_I("NETHUD test: netz=%d tun=%d weg=%d mitspieler=%d/%d ping=%.1f min=%.1f var=%.1f verlust ein=%d.%d%% "
           "aus=%d.%d%% | rb=%d modus=%d grund=%d verz=%d rtt=%d rollbacks/s=%.2f tiefe_p95=%u (n=%u) vorhersage=%.1f%% "
           "vorsprung=%.2f peers_rb=%d tsync=%d abgleich=%.1f ms puffer=%d ms | zeilen=%d",
           nh_last_have_net, nh_last_net.tun, nh_last_net.route, nh_last_net.up, nh_last_net.known, nh_last_net.ping,
           nh_last_net.ping_min, nh_last_net.jitter, nh_last_net.loss_in / 10, nh_last_net.loss_in % 10,
           nh_last_net.loss_out / 10, nh_last_net.loss_out % 10, nh_last_have_rb, nh_last_rb.mode, nh_last_rb.why,
           nh_last_rb.delay, nh_last_rb.rtt_ms, nh_last_rbps, nh_last_rb.depth_p95, nh_last_rb.depth_n, nh_last_pred,
           nh_last_rb.adv, nh_last_rb.npeers, nh_last_rb.tsync, nh_last_rb.tsync_ms, nh_last_rb.buf_ms, nh_txt.n);
    { /* W387: eigene Zeile (die obere wird geparst) */
        int sm = 0, sms = 0;
        if (loc_smooth_opp_hud(&sm, &sms)) HLOG_I("NETHUD test W387: gegner_glaetten=%d verz=%d ms", sm, sms);
    }
    { /* W390: eigene Zeile */
        int om = 0, oms = 0, ous = 0;
        if (loc_rb_ownpred_hud(&om, &oms, &ous)) HLOG_I("NETHUD test W390: eigene_vorhersage=%d voraus=%d ms kosten=%d us", om, oms, ous);
    }
    HLOG_I("NETHUD test W394: hud_marken=%d sicht_je_bild=%d", g_cfg.loc_hud_predict != 0,
           g_cfg.loc_view_perframe && g_cfg.loc_local_aim); /* W394: eigene Zeile */
}

/* ------------------------------------------------------------------ */
/* Anforderungen + Speichern (Render-Pfad)                              */
/* ------------------------------------------------------------------ */
static void nh_save_cfg(void)
{
    static const char *const keys[2] = { "net_hud", "net_hud_pos" };
    static const char *const secs[2] = { "overlay", "overlay" };
    char v0[8], v1[8];
    const char *vals[2];
    if (!nh_save_path[0]) {
        HLOG_I("NETHUD: kein Speicherpfad (user.cfg) - Wert gilt nur bis zum Ende");
        return;
    }
    _snprintf_s(v0, sizeof v0, _TRUNCATE, "%d", nh_vis);
    _snprintf_s(v1, sizeof v1, _TRUNCATE, "%d", nh_pos);
    vals[0] = v0;
    vals[1] = v1;
    if (sb_ovl_cfg_write(nh_save_path, 2, keys, secs, vals) >= 0) {
        HLOG_I("NETHUD: gespeichert -> %s (net_hud=%d net_hud_pos=%d)", nh_save_path, nh_vis, nh_pos);
    } else {
        HLOG_W("NETHUD: Speichern nach %s fehlgeschlagen", nh_save_path);
    }
}

static void nh_start(void)
{
    if (nh_thr) { return; }
    nh_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    nh_thr = CreateThread(NULL, 0, nh_thread, NULL, 0, NULL);
    if (nh_thr) { SetThreadPriority(nh_thr, THREAD_PRIORITY_BELOW_NORMAL); }
}

void sb_nhud_tick(void)
{
    if (!nh_init) {
        const char *u;
        nh_init = 1;
        nh_vis = g_cfg.net_hud ? 1 : 0;
        nh_pos = g_cfg.net_hud_pos & 3;
        if (g_cfg.overlay_save_path[0]) { strcpy_s(nh_save_path, sizeof nh_save_path, g_cfg.overlay_save_path); }
        else if ((u = hcfg_user_cfg_path()) != NULL && u[0]) { strcpy_s(nh_save_path, sizeof nh_save_path, u); }
        if (nh_vis) { nh_start(); }
    }
    if (nh_req_toggle) {
        InterlockedExchange(&nh_req_toggle, 0);
        nh_vis = !nh_vis;
        HLOG_I("NETHUD: %s (f=%u)", nh_vis ? "an" : "aus", g_present_no);
        if (nh_vis) {
            nh_start();
            if (nh_wake) { SetEvent(nh_wake); } /* sofort abtasten */
        }
        nh_save_cfg();
    }
    if (nh_req_pos) {
        InterlockedExchange(&nh_req_pos, 0);
        nh_pos = (nh_pos + 1) & 3;
        HLOG_I("NETHUD: Ecke -> %d (f=%u)", nh_pos, g_present_no);
        nh_save_cfg();
    }
}

/* ------------------------------------------------------------------ */
/* Panel (GDI, Look des ESC-Overlays)                                   */
/* ------------------------------------------------------------------ */
static int S(float x)
{
    int v = (int)(x * nh_f + 0.5f);
    return v < 1 ? 1 : v;
}

static void nh_fill(int x0, int y0, int x1, int y1, COLORREF c)
{
    RECT r;
    HBRUSH b = CreateSolidBrush(c);
    r.left = x0; r.top = y0; r.right = x1; r.bottom = y1;
    FillRect(nh_dc, &r, b);
    DeleteObject(b);
}

static void nh_text(int x, int y, const wchar_t *s, COLORREF c, int right)
{
    SetTextColor(nh_dc, c);
    SetTextAlign(nh_dc, (right ? TA_RIGHT : TA_LEFT) | TA_TOP);
    TextOutW(nh_dc, x, y, s, (int)wcslen(s));
}

static int nh_gdi_size(unsigned w, unsigned h)
{
    BITMAPINFO bi;
    if (nh_dc && w == nh_bw && h == nh_bh) { return 1; }
    if (!nh_dc) {
        nh_dc = CreateCompatibleDC(NULL);
        if (!nh_dc) { return 0; }
    }
    if (nh_bmp) { SelectObject(nh_dc, nh_bmp_old); DeleteObject(nh_bmp); nh_bmp = NULL; }
    if (nh_font) { SelectObject(nh_dc, nh_font_old); DeleteObject(nh_font); nh_font = NULL; }
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = (LONG)w;
    bi.bmiHeader.biHeight = -(LONG)h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    nh_bmp = CreateDIBSection(nh_dc, &bi, DIB_RGB_COLORS, (void **)&nh_bits, NULL, 0);
    if (!nh_bmp || !nh_bits) { nh_bw = nh_bh = 0; return 0; }
    nh_bmp_old = (HBITMAP)SelectObject(nh_dc, nh_bmp);
    nh_font_px = S(9.0f);
    nh_font = CreateFontW(-nh_font_px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                          CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    if (nh_font) { nh_font_old = (HFONT)SelectObject(nh_dc, nh_font); }
    SetBkMode(nh_dc, TRANSPARENT);
    nh_bw = w;
    nh_bh = h;
    return 1;
}

static void nh_key_name(wchar_t *o, size_t cap)
{
    int vk = (int)nh_vk[0], m = (int)nh_mod[0];
    o[0] = 0;
    if (vk >= VK_F1 && vk <= VK_F24) { _snwprintf_s(o, cap, _TRUNCATE, L"%sF%d", (m & 1) ? L"⇧" : L"", vk - VK_F1 + 1); }
    else if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) { _snwprintf_s(o, cap, _TRUNCATE, L"%c", vk); }
}

static void nh_paint(const nh_text_t *x)
{
    int W = (int)nh_bw, H = (int)nh_bh, b = S(1.0f), pad = S(5.0f), rowh = S(10.0f), head = S(15.0f);
    int y, ty, i;
    wchar_t t[64], kn[16];
    nh_fill(0, 0, W, H, SB_UI_C_BG);
    nh_fill(0, 0, W, b, SB_UI_C_LINE);
    nh_fill(0, H - b, W, H, SB_UI_C_LINE);
    nh_fill(0, 0, b, H, SB_UI_C_LINE);
    nh_fill(W - b, 0, W, H, SB_UI_C_LINE);
    nh_fill(0, 0, S(7.0f), 2 * b + 1, SB_UI_C_TXT);
    nh_fill(0, 0, 2 * b + 1, S(7.0f), SB_UI_C_TXT);
    nh_fill(W - S(7.0f), H - 2 * b - 1, W, H, SB_UI_C_TXT);
    nh_fill(W - 2 * b - 1, H - S(7.0f), W, H, SB_UI_C_TXT);
    nh_fill(b + S(2.0f), b + S(2.0f), W - b - S(2.0f), head, SB_UI_C_HEAD);
    ty = b + S(2.0f) + (head - b - S(2.0f) - nh_font_px) / 2 - S(0.5f);
    nh_text(pad + S(1.0f), ty, L"■ SYS-NET", SB_UI_C_BG, 0);
    nh_lw("ovl.nh_title", t, 32);
    nh_key_name(kn, 16);
    if (kn[0]) {
        size_t l = wcslen(t);
        _snwprintf_s(t + l, 64 - l, _TRUNCATE, L" %s", kn);
    }
    nh_text(W - pad - S(1.0f), ty, t, SB_UI_C_BG, 1);
    y = head + S(3.0f);
    for (i = 0; i < x->n; i++) {
        nh_text(pad, y, x->t[i], x->warn[i] ? NH_C_WARN : SB_UI_C_TXT, 0);
        y += rowh;
    }
    GdiFlush();
    sb_ovl_ui_finish(nh_bits, nh_bw, nh_bh, nh_f);
}

static int nh_tex_upload(void)
{
    D3D11_MAPPED_SUBRESOURCE map;
    unsigned y;
    if (!nh_tex || nh_tw != nh_bw || nh_th != nh_bh) {
        D3D11_TEXTURE2D_DESC td;
        if (nh_srv) { ID3D11ShaderResourceView_Release(nh_srv); nh_srv = NULL; }
        if (nh_tex) { ID3D11Texture2D_Release(nh_tex); nh_tex = NULL; }
        memset(&td, 0, sizeof td);
        td.Width = nh_bw;
        td.Height = nh_bh;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &nh_tex)) ||
            FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)nh_tex, NULL, &nh_srv))) {
            HLOG_W("NETHUD: Panel-Textur %ux%u gescheitert", nh_bw, nh_bh);
            nh_tw = nh_th = 0;
            return 0;
        }
        nh_tw = nh_bw;
        nh_th = nh_bh;
    }
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)nh_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        return 0;
    }
    for (y = 0; y < nh_bh; y++) {
        memcpy((unsigned char *)map.pData + (size_t)y * map.RowPitch, nh_bits + (size_t)y * nh_bw, nh_bw * 4u);
    }
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)nh_tex, 0);
    return 1;
}

void sb_nhud_draw(ID3D11RenderTargetView *rtv, int save, const unsigned *avoid)
{
    unsigned bx, by, bw, bh, pw, ph, x0, y0, m;
    int rows;
    LONG ver = nh_ver;
    static unsigned lk[4];
    static nh_text_t cur;
    if (!nh_vis || !rtv || !g_ctx || !g_dev) { return; }
    if (ver != nh_painted || !nh_srv) { /* neue Zeilen: Kopie unter der Sperre */
        AcquireSRWLockShared(&nh_lk);
        cur = nh_txt;
        ReleaseSRWLockShared(&nh_lk);
    }
    rows = cur.n > 0 ? cur.n : 1;
    sb_aspect_box(&bx, &by, &bw, &bh);
    nh_f = (float)bh / 540.0f;
    if (nh_f < 1.0f) { nh_f = 1.0f; }
    pw = (unsigned)S(196.0f);
    ph = (unsigned)(S(15.0f) + S(3.0f) + rows * S(10.0f) + S(4.0f));
    if (pw + 4u > g_bb_w || ph + 4u > g_bb_h) { return; }
    if (!nh_gdi_size(pw, ph)) { return; }
    if (ver != nh_painted || !nh_srv || nh_tw != pw || nh_th != ph || nh_paint_rows != rows) {
        nh_paint(&cur);
        if (!nh_tex_upload()) { return; }
        nh_painted = ver;
        nh_paint_rows = rows;
    }
    m = (unsigned)S(8.0f);
    x0 = (nh_pos == 1 || nh_pos == 2) ? m : g_bb_w - pw - m;
    y0 = (nh_pos <= 1) ? m : g_bb_h - ph - m;
    if (avoid && x0 < avoid[0] + avoid[2] && avoid[0] < x0 + pw && y0 < avoid[1] + avoid[3] && avoid[1] < y0 + ph) {
        /* F2-Panel im Weg: oben darunter, unten darueber stapeln */
        if (nh_pos <= 1) { y0 = avoid[1] + avoid[3] + m / 2u; }
        else { y0 = avoid[1] > ph + m / 2u ? avoid[1] - ph - m / 2u : 0u; }
        if (y0 + ph > g_bb_h) { return; }
    }
    if (lk[0] != x0 || lk[1] != y0 || lk[2] != pw || lk[3] != ph) {
        lk[0] = x0; lk[1] = y0; lk[2] = pw; lk[3] = ph;
        HLOG_I("NETHUD panel %ux%u @%u,%u (bb %ux%u, Ecke %d, Faktor %.2f, Zeilen %d%s)", pw, ph, x0, y0, g_bb_w, g_bb_h,
               nh_pos, (double)nh_f, rows, avoid ? ", F2 sichtbar" : "");
    }
    if (save) { sb_ovl_save_take(&nh_save, rtv, x0, y0, pw, ph); }
    sb_ovl_quad(rtv, nh_srv, x0, y0, pw, ph);
}

void sb_nhud_restore(void)
{
    sb_ovl_save_put(&nh_save);
}
