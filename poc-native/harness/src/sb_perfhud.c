/* sb_perfhud.c - W122 PERF-HUD: Leistungs-Overlay (F2). Beschreibung s.
 * sb_perfhud.h.
 *
 * Messpunkte (nur bei g_phud_meas):
 * - jedes echte Present (sb_render_present direkt bzw. sb_render_present_raw
 *   der FPS-INTERP-Zwischenbilder): QPC -> Bildzeit (Present zu Present),
 *   Dauer des Present-Aufrufs, Art (Tick-Bild = seit dem letzten Present lief
 *   ein Spiel-Swap; sonst Zwischenbild);
 * - Spiel-Tick: Aufwachen (Feldwechsel in GetDisplayFieldStatus bzw. Ende
 *   von BlockUntilVerticalBlank, hooks_timing.c) bis Ende sb_render_present =
 *   "Tick-Last" (Rechenzeit des Spiel-Threads je Tick). Darin Render = IM-
 *   Uebergabe (QPC-Zaehler g_rqp_q VB-Schleife + Submit + Prep, ohne die
 *   Interp-Aufzeichnung), Interp = Aufzeichnung + Zuordnung + Zwischenbilder,
 *   die in dieses Fenster fallen, Present = direkter Present-Aufruf, Spiel =
 *   Rest (Spielcode, Texturen, RT-Wechsel, Hooks ohne Zaehler);
 * - GPU: D3D11-Timestamp-Queries je gezeigtem Bild (nach dem vorigen Present
 *   bis vor das naechste). Zwischenbilder werden am Stueck uebergeben (genau),
 *   Tick-Bilder enthalten Wartezeiten der GPU, waehrend die CPU noch Draws
 *   erzeugt (Obergrenze).
 *
 * Alles laeuft auf dem Spiel-/Render-Thread (Presents, Swaps, Hooks); der
 * UI-Thread meldet nur Tasten (Interlocked). */
#include "render_int.h"
#include "sb_profile.h" /* W229 SBA() */
#include "render_qa.h"
#include "sb_overlay.h"
#include "sb_perfhud.h"
#include "sb_pause.h"
#include "sb_lang.h"    /* W190: Hinweis Spielstaende geschuetzt */
#include "kernel_hle.h" /* W190: krnl_savelock_active */
#include "vr.h"         /* W197: sb_vr_hud_text */
#include <stdlib.h>
#include <stdarg.h>
#include <wchar.h>
#include <tlhelp32.h>

int sb_winmode_current(void); /* render_winmode.c */
extern unsigned g_ip_skip_tot; /* render_interp.c: verfallene Zwischenbilder (gesamt) */

volatile int g_phud_meas;
unsigned g_phud_upn;            /* sb_tex.c: Textur-Uploads (gesamt) */
extern long long g_phud_texq;  /* sb_tex.c: Zeit in sb_tex_get (QPC, nur bei Messung) */
extern unsigned g_phud_texn;   /* sb_tex.c: Aufrufe sb_tex_get */
extern unsigned g_la_n, g_la_steps; /* krnl_mem.c: low_alloc Anforderungen, VirtualQuery-Schritte */
extern long long g_la_q;            /* krnl_mem.c: Zeit in low_alloc (nur bei Messung) */

/* ---- Zustand ---- */
static volatile LONG ph_req_toggle, ph_req_pos; /* UI -> Render */
static volatile LONG ph_vk = -1;                /* Taste (-1 = noch nicht gelesen, 0 = aus) */
static int ph_init, ph_vis, ph_pos;
static LARGE_INTEGER ph_qpf;
static LONGLONG ph_last_log, ph_last_paint;
static char ph_save_path[600];

/* Bild-Ring */
#define PH_RING 1024u
typedef struct { LONGLONG q; float ft, pres; unsigned skip; unsigned char tick; } ph_frame_t;
static ph_frame_t ph_fr[PH_RING];
static unsigned ph_fn;
static LONGLONG ph_last_pres;
static int ph_tick_since_pres;

/* Tick-Ring */
#define PH_TRING 512u
typedef struct {
    LONGLONG q;
    float period, wait, iwait;       /* Tick-Periode, Warten, Zwischenbilder im Warten (ms) */
    float busy, render, interp, pres, game, draws;
    float vbloop, submit, cap, draw; /* Render-Teile (ms) */
    float tex;                       /* davon sb_tex_get (ms) */
    unsigned texn, upn;              /* sb_tex_get-Aufrufe, Uploads */
} ph_tick_t;
static ph_tick_t ph_tk[PH_TRING];
static unsigned ph_tn;
static LONGLONG ph_tick_end_q, ph_wait_acc, ph_interp_other, ph_interp_inwait, ph_pres_direct;
static int ph_in_tick;
static unsigned ph_tick_draws;
static long long ph_rq_last[RQP_N];
static long long ph_tex_last;
static unsigned ph_texn_last, ph_upn_last;

/* GPU-Ring */
#define PH_GQ 8
typedef struct { ID3D11Query *dj, *t0, *t1; int st; unsigned char tick; } ph_gq_t;
static ph_gq_t ph_gq[PH_GQ];
static int ph_gq_open = -1, ph_gq_head, ph_gq_fail;
#define PH_GRING 256u
typedef struct { LONGLONG q; float ms; unsigned char tick; } ph_gres_t;
static ph_gres_t ph_gr[PH_GRING];
static unsigned ph_gn;

/* DIAG(GPU-COST,W146): GPU je Pass (perf_gpass, s. sb_gp_begin) */
#define GP_SLOTS 4
#define GP_MAXM 192
typedef struct {
    ID3D11Query *dj, *ts[GP_MAXM];
    unsigned char cat[GP_MAXM];
    int n, st, vr; /* st: 0 frei, 1 offen, 2 wartet auf Ergebnis */
} gp_slot_t;
static gp_slot_t gp_s[GP_SLOTS];
static int gp_cur = -1, gp_cat = -1, gp_fail;
static unsigned gp_cnt;
int g_gp_open;
static double gp_acc[GP_N], gp_vr_acc;      /* Fenster-Summen (ms) */
static unsigned gp_frames, gp_vr_frames, gp_lost;
static double gp_last[GP_N], gp_last_vr;    /* letztes Fenster je Bild (Panel) */
static unsigned gp_last_n, gp_last_vrn;
static ID3D11RenderTargetView *gp_crtv;
static int gp_ccls;
static const char *const gp_nm[GP_N] = { "start", "welt", "welt_sh", "rt", "rt_sh", "bb", "bb_sh", "mfx", "fx",
                                         "blit", "post", "fxpost", "hud" };

/* DIAG-Profiler (perf_prof, s. ph_prof_start) */
static HANDLE ph_prof_th;
static volatile LONG ph_prof_run;
static void ph_prof_start(void);

/* Panel */
static HDC ph_dc;
static HBITMAP ph_bmp, ph_bmp_old;
static unsigned *ph_bits;
static unsigned ph_bw, ph_bh;
static HFONT ph_font, ph_font_old;
static int ph_font_px;
static float ph_f = 1.0f;
static ID3D11Texture2D *ph_tex;
static ID3D11ShaderResourceView *ph_srv;
static unsigned ph_tw, ph_th;
static sb_ovl_save_t ph_save;

/* ------------------------------------------------------------------ */
static LONGLONG ph_now(void)
{
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return q.QuadPart;
}

static double ph_ms(LONGLONG dq)
{
    return ph_qpf.QuadPart ? (double)dq * 1000.0 / (double)ph_qpf.QuadPart : 0.0;
}

/* ------------------------------------------------------------------ */
/* DIAG(PERF-BENCH,W137): Bild-/Tick-Spur fuer scripts/perf_bench.py   */
/* perf_trace=1: <SB_OUT_DIR>/perf_trace.csv, Zeilen                   */
/*   P,t,ft,pres,tick,miss,mode,skip,src je echtem Present (W138 src) */
/*   T,t,per,wait,busy,render,interp,game,draws,visq,up  je Spiel-Tick */
/*   G,t,ms,tick                       je GPU-Timestamp-Ergebnis       */
/* t = QPC-Sekunden wie die hlog-Zeitstempel ([%9.3f]). Puffer 1 MB,   */
/* geschrieben alle 0,5 s bzw. bei vollem Puffer (Spiel-Thread).       */
/* ------------------------------------------------------------------ */
#define PT_CAP (1u << 20)
static HANDLE pt_h = INVALID_HANDLE_VALUE;
static char *pt_buf;
static size_t pt_len;
static LONGLONG pt_flush_q;
static int pt_state; /* 0 = ungeoeffnet, 1 = offen, -1 = Fehler */
static SRWLOCK pt_lk = SRWLOCK_INIT;
static long long pt_visq0;

static void pt_flush_locked(void)
{
    DWORD w;
    if (pt_state == 1 && pt_len) { WriteFile(pt_h, pt_buf, (DWORD)pt_len, &w, NULL); }
    pt_len = 0;
}

static void pt_open_locked(void)
{
    char path[700];
    char dir[600];
    DWORD n;
    if (pt_state) { return; }
    pt_state = -1;
    n = GetEnvironmentVariableA("SB_OUT_DIR", dir, sizeof dir);
    if (!n || n >= sizeof dir) { strcpy_s(dir, sizeof dir, "."); }
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s/perf_trace.csv", dir);
    pt_buf = (char *)malloc(PT_CAP);
    if (!pt_buf) { return; }
    pt_h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (pt_h == INVALID_HANDLE_VALUE) {
        HLOG_W("PERFTRACE: %s nicht schreibbar (%lu)", path, GetLastError());
        return;
    }
    pt_state = 1;
    pt_len = (size_t)_snprintf_s(pt_buf, PT_CAP, _TRUNCATE,
                                 "# perf_trace v1 (W137) qpf=%lld interp_fps=%d vblank_hz=%d %ux%u rt_scale=%d shading=%d\n",
                                 (long long)ph_qpf.QuadPart, g_cfg.interp_fps, g_cfg.vblank_hz,
                                 (unsigned)g_cfg.width, (unsigned)g_cfg.height, g_cfg.rt_scale, g_cfg.shading);
    HLOG_I("PERFTRACE: Spur nach %s", path);
}

static void pt_add(LONGLONG now, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (!g_cfg.perf_trace || pt_state < 0) { return; }
    AcquireSRWLockExclusive(&pt_lk);
    if (!pt_state) { pt_open_locked(); }
    if (pt_state == 1) {
        if (pt_len + 256u > PT_CAP) { pt_flush_locked(); }
        va_start(ap, fmt);
        n = _vsnprintf_s(pt_buf + pt_len, PT_CAP - pt_len, _TRUNCATE, fmt, ap);
        va_end(ap);
        if (n > 0) { pt_len += (size_t)n; }
        if (now - pt_flush_q > ph_qpf.QuadPart / 2) { pt_flush_q = now; pt_flush_locked(); }
    }
    ReleaseSRWLockExclusive(&pt_lk);
}

static double pt_sec(LONGLONG q) { return (double)q / (double)ph_qpf.QuadPart; }

static int ph_parse_key(const char *s)
{
    int n;
    if (!s || !s[0] || strcmp(s, "0") == 0 || _stricmp(s, "aus") == 0 || _stricmp(s, "off") == 0) { return 0; }
    if ((s[0] == 'F' || s[0] == 'f') && (n = atoi(s + 1)) >= 1 && n <= 12) {
        if (n == 10) { return 0; } /* F10 = Maus fangen/freigeben (reserviert) */
        return VK_F1 + n - 1;
    }
    if (_strnicmp(s, "VK", 2) == 0) {
        n = (int)strtoul(s + 2, NULL, 16);
        if (n == VK_ESCAPE || n == VK_F10) { return 0; } /* ESC = Overlay, F10 = Maus */
        return n > 0 && n < 256 ? n : 0;
    }
    return 0;
}

static void ph_key_name(wchar_t *o, size_t cap)
{
    int vk = (int)ph_vk;
    if (vk >= VK_F1 && vk <= VK_F12) { _snwprintf_s(o, cap, _TRUNCATE, L"F%d", vk - VK_F1 + 1); }
    else if (vk > 0) { _snwprintf_s(o, cap, _TRUNCATE, L"VK%02X", vk); }
    else { wcsncpy_s(o, cap, L"", _TRUNCATE); }
}

/* ------------------------------------------------------------------ */
/* Fenster (UI-Thread)                                                  */
/* ------------------------------------------------------------------ */
int sb_phud_wndproc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)w;
    if (ph_vk < 0) { InterlockedExchange(&ph_vk, ph_parse_key(g_cfg.perf_hud_key)); }
    if (ph_vk <= 0 || (WPARAM)ph_vk != wp) { return 0; }
    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
        if (!(lp & 0x40000000)) { /* keine Autowiederholung */
            if (GetKeyState(VK_SHIFT) < 0) { InterlockedIncrement(&ph_req_pos); }
            else { InterlockedIncrement(&ph_req_toggle); }
        }
        return 1;
    }
    if (msg == WM_KEYUP || msg == WM_SYSKEYUP) { return 1; }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Speichern (user.cfg, Verfahren wie "Als Standard speichern")         */
/* ------------------------------------------------------------------ */
static void ph_save_cfg(void)
{
    static const char *const keys[2] = { "perf_hud", "perf_hud_pos" };
    static const char *const secs[2] = { "overlay", "overlay" };
    char v0[8], v1[8];
    const char *vals[2];
    int n;
    if (!ph_save_path[0]) {
        HLOG_I("PERFHUD: kein Speicherpfad (user.cfg) - Wert gilt nur bis zum Ende");
        return;
    }
    _snprintf_s(v0, sizeof v0, _TRUNCATE, "%d", ph_vis);
    _snprintf_s(v1, sizeof v1, _TRUNCATE, "%d", ph_pos);
    vals[0] = v0;
    vals[1] = v1;
    n = sb_ovl_cfg_write(ph_save_path, 2, keys, secs, vals);
    if (n >= 0) { HLOG_I("PERFHUD: gespeichert -> %s (perf_hud=%d perf_hud_pos=%d)", ph_save_path, ph_vis, ph_pos); }
    else { HLOG_W("PERFHUD: Speichern nach %s fehlgeschlagen", ph_save_path); }
}

/* ------------------------------------------------------------------ */
/* GPU-Zeit (Timestamp-Queries)                                         */
/* ------------------------------------------------------------------ */
static void ph_gq_poll(void)
{
    int k;
    for (k = 0; k < PH_GQ; k++) {
        ph_gq_t *g = &ph_gq[(ph_gq_head + k) % PH_GQ];
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        UINT64 t0, t1;
        if (g->st != 2) { continue; }
        if (ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)g->dj, &dj, sizeof dj,
                                        D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
            break; /* in Reihenfolge: juengere sind auch noch nicht fertig */
        }
        g->st = 0;
        if (dj.Disjoint || !dj.Frequency) { continue; }
        if (ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)g->t0, &t0, sizeof t0, 0) != S_OK ||
            ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)g->t1, &t1, sizeof t1, 0) != S_OK || t1 < t0) {
            continue;
        }
        {
            ph_gres_t *r = &ph_gr[ph_gn % PH_GRING];
            r->q = ph_now();
            r->ms = (float)((double)(t1 - t0) * 1000.0 / (double)dj.Frequency);
            r->tick = g->tick;
            ph_gn++;
            pt_add(r->q, "G,%.4f,%.3f,%u\n", pt_sec(r->q), (double)r->ms, (unsigned)r->tick); /* DIAG(PERF-BENCH,W137) */
        }
    }
}

static void ph_gq_begin(void)
{
    ph_gq_t *g;
    int i;
    if (ph_gq_fail || !g_dev || !g_ctx) { return; }
    for (i = 0; i < PH_GQ; i++) {
        g = &ph_gq[(ph_gq_head + i) % PH_GQ];
        if (g->st == 0) { break; }
    }
    if (i >= PH_GQ) { return; } /* alle unterwegs */
    if (!g->dj) {
        D3D11_QUERY_DESC qd;
        memset(&qd, 0, sizeof qd);
        qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        if (FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &g->dj))) { g->dj = NULL; ph_gq_fail = 1; return; }
        qd.Query = D3D11_QUERY_TIMESTAMP;
        if (FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &g->t0)) ||
            FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &g->t1))) {
            ph_gq_fail = 1;
            HLOG_W("PERFHUD: Timestamp-Queries nicht verfuegbar -> GPU-Zeit aus");
            return;
        }
    }
    ID3D11DeviceContext_Begin(g_ctx, (ID3D11Asynchronous *)g->dj);
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)g->t0);
    g->st = 1;
    ph_gq_open = (int)(g - ph_gq);
}

static void ph_gq_end(void)
{
    ph_gq_t *g;
    if (ph_gq_open < 0) { return; }
    g = &ph_gq[ph_gq_open];
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)g->t1);
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)g->dj);
    g->st = 2;
    g->tick = (unsigned char)(ph_tick_since_pres ? 1 : 0);
    ph_gq_open = -1;
}

/* ------------------------------------------------------------------ */
/* DIAG(GPU-COST,W146) GPU-Zeit je Pass                                */
/* ------------------------------------------------------------------ */
static void gp_poll(void)
{
    int k;
    for (k = 0; k < GP_SLOTS; k++) {
        gp_slot_t *g = &gp_s[k];
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        UINT64 ts[GP_MAXM];
        int i, ok = 1;
        if (g->st != 2) { continue; }
        if (ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)g->dj, &dj, sizeof dj,
                                        D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) { continue; }
        g->st = 0;
        if (dj.Disjoint || !dj.Frequency || g->n < 2) { gp_lost++; continue; }
        for (i = 0; i < g->n && ok; i++) {
            if (ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)g->ts[i], &ts[i], sizeof ts[i],
                                            D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) { ok = 0; }
        }
        if (!ok) { gp_lost++; continue; }
        if (g->vr) {
            gp_vr_acc += (double)(ts[g->n - 1] - ts[0]) * 1000.0 / (double)dj.Frequency;
            gp_vr_frames++;
            continue;
        }
        for (i = 0; i + 1 < g->n; i++) {
            if (ts[i + 1] >= ts[i] && g->cat[i] < GP_N) {
                gp_acc[g->cat[i]] += (double)(ts[i + 1] - ts[i]) * 1000.0 / (double)dj.Frequency;
            }
        }
        gp_frames++;
        { /* Folge der Segmente (Reihenfolge der Paesse) einmal je 2000 Messungen */
            static unsigned nseq;
            if (rq_in_mission() && (nseq++ % 2000u) == 600u) {
                char sq[1500];
                int o = 0;
                for (i = 0; i + 1 < g->n && o >= 0; i++) {
                    o += _snprintf_s(sq + o, sizeof sq - (size_t)o, _TRUNCATE, " %s=%.2f",
                                     g->cat[i] < GP_N ? gp_nm[g->cat[i]] : "?",
                                     (double)(ts[i + 1] - ts[i]) * 1000.0 / (double)dj.Frequency);
                }
                HLOG_I("GPUPASS folge f=%u segs=%d:%s", g_present_no, g->n - 1, sq);
            }
        }
    }
}

int sb_gp_begin(int vr)
{
    gp_slot_t *g = NULL;
    int i;
    if (g_cfg.perf_gpass <= 0 || gp_fail || !g_dev || !g_ctx || g_gp_open) { return 0; }
    gp_poll();
    if (!vr && (++gp_cnt % (unsigned)g_cfg.perf_gpass) != 0u) { return 0; }
    for (i = 0; i < GP_SLOTS; i++) {
        if (gp_s[i].st == 0) { g = &gp_s[i]; break; }
    }
    if (!g) { return 0; }
    if (!g->dj) {
        D3D11_QUERY_DESC qd;
        memset(&qd, 0, sizeof qd);
        qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        if (FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &g->dj))) { g->dj = NULL; gp_fail = 1; return 0; }
    }
    ID3D11DeviceContext_Begin(g_ctx, (ID3D11Asynchronous *)g->dj);
    g->n = 0;
    g->st = 1;
    g->vr = vr;
    gp_cur = i;
    gp_cat = -1;
    g_gp_open = 1;
    gp_crtv = NULL;
    sb_gp_mark_(GP_START);
    return 1;
}

void sb_gp_mark_(int cat)
{
    gp_slot_t *g;
    if (!g_gp_open || gp_cur < 0 || cat == gp_cat) { return; }
    g = &gp_s[gp_cur];
    if (g->vr && g->n >= 1 && cat >= 0) { return; } /* VR: nur Anfang/Ende */
    if (g->n >= GP_MAXM - 1 && cat >= 0) { return; } /* letzter Platz fuer das Ende */
    if (g->n >= GP_MAXM) { return; }
    if (!g->ts[g->n]) {
        D3D11_QUERY_DESC qd;
        memset(&qd, 0, sizeof qd);
        qd.Query = D3D11_QUERY_TIMESTAMP;
        if (FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &g->ts[g->n]))) { gp_fail = 1; return; }
    }
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)g->ts[g->n]);
    g->cat[g->n] = (unsigned char)(cat < 0 ? 255 : cat);
    g->n++;
    gp_cat = cat;
}

void sb_gp_end(void)
{
    gp_slot_t *g;
    if (!g_gp_open || gp_cur < 0) { return; }
    sb_gp_mark_(-1); /* Ende */
    g = &gp_s[gp_cur];
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)g->dj);
    g->st = 2;
    g_gp_open = 0;
    gp_cur = -1;
}

int sb_gp_rt_class(ID3D11RenderTargetView *rtv, int is_bb, int rq)
{
    if (is_bb) { return rq ? GP_BB_SH : GP_BB; }
    if (rtv != gp_crtv) {
        ID3D11Resource *r = NULL;
        D3D11_TEXTURE2D_DESC td;
        gp_crtv = rtv;
        gp_ccls = 0;
        if (rtv) {
            ID3D11View_GetResource((ID3D11View *)rtv, &r);
            if (r) {
                ID3D11Texture2D_GetDesc((ID3D11Texture2D *)r, &td);
                gp_ccls = (td.Width >= 1024u && td.Height >= 1024u) ? 1 : 0;
                ID3D11Resource_Release(r);
            }
        }
    }
    return gp_ccls ? (rq ? GP_WELT_SH : GP_WELT) : (rq ? GP_SUB_SH : GP_SUB);
}


/* Fenster abschliessen: Mittel je Bild fuer Panel/Log */
static void gp_window(char *line, size_t cap)
{
    int k, o = 0;
    double sum = 0.0, kf = gp_frames ? 1.0 / (double)gp_frames : 0.0;
    for (k = 0; k < GP_N; k++) {
        gp_last[k] = gp_acc[k] * kf;
        sum += gp_last[k];
    }
    gp_last_n = gp_frames;
    gp_last_vr = gp_vr_frames ? gp_vr_acc / (double)gp_vr_frames : 0.0;
    gp_last_vrn = gp_vr_frames;
    if (line && cap) {
        line[0] = 0;
        for (k = 0; k < GP_N && o >= 0; k++) {
            o += _snprintf_s(line + o, cap - (size_t)o, _TRUNCATE, " %s=%.2f", gp_nm[k], gp_last[k]);
        }
        if (o >= 0) {
            _snprintf_s(line + o, cap - (size_t)o, _TRUNCATE, " | summe=%.2f | vr n=%u ms=%.2f | verloren=%u",
                        sum, gp_vr_frames, gp_last_vr, gp_lost);
        }
    }
    memset(gp_acc, 0, sizeof gp_acc);
    gp_vr_acc = 0.0;
    gp_frames = gp_vr_frames = gp_lost = 0;
}

/* ------------------------------------------------------------------ */
/* Statistik                                                           */
/* ------------------------------------------------------------------ */
typedef struct {
    unsigned frames, frames_tick, ticks, over15, over2, gpu_nz, gpu_nt;
    double fps, ft_avg, ft_min, ft_max, p50, p99, low1, pres_avg, tps;
    double busy, render, interp, pres, game, draws, vbloop, submit, cap, draw, busy_max, tex, texn, upn, upn_max;
    double period, period_max, wait, iwait;
    double gpu_z, gpu_t, gpu_max, target;
    unsigned skip;
} ph_stat_t;

static int ph_fcmp(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static double ph_target_ms(void)
{
    if (g_sb_paused) { return 1000.0 / (double)sb_ovl_pause_fps(); }
    if (g_cfg.interp_fps > 0 && rq_in_mission()) { return 1000.0 / (double)g_cfg.interp_fps; }
    return 1000.0 / (double)g_cfg.game_tick; /* W234: 25 bei PAL50 */
}

static void ph_stats(double win_s, ph_stat_t *s)
{
    static float tmp[PH_RING];
    unsigned skip_old = 0, skip_new = 0;
    LONGLONG now = ph_now(), lim = now - (LONGLONG)(win_s * (double)ph_qpf.QuadPart);
    unsigned i, n = 0;
    double sum = 0.0, psum = 0.0;
    memset(s, 0, sizeof *s);
    s->target = ph_target_ms();
    s->ft_min = 1e9;
    for (i = 0; i < PH_RING && i < ph_fn; i++) {
        const ph_frame_t *f = &ph_fr[(ph_fn - 1u - i) % PH_RING];
        if (f->q < lim) { break; }
        if (f->ft <= 0.0f) { continue; }
        if (!n) { skip_new = f->skip; }
        skip_old = f->skip;
        tmp[n++] = f->ft;
        sum += f->ft;
        psum += f->pres;
        if (f->tick) { s->frames_tick++; }
        if (f->ft < s->ft_min) { s->ft_min = f->ft; }
        if (f->ft > s->ft_max) { s->ft_max = f->ft; }
        if (f->ft > s->target * 1.5) { s->over15++; }
        if (f->ft > s->target * 2.0) { s->over2++; }
    }
    s->frames = n;
    s->skip = skip_new - skip_old;
    if (n) {
        s->ft_avg = sum / n;
        s->fps = 1000.0 / s->ft_avg;
        s->pres_avg = psum / n;
        qsort(tmp, n, sizeof tmp[0], ph_fcmp);
        s->p50 = tmp[n / 2];
        s->p99 = tmp[(n * 99u) / 100u < n ? (n * 99u) / 100u : n - 1u];
        s->low1 = s->p99 > 0.0 ? 1000.0 / s->p99 : 0.0;
    } else {
        s->ft_min = 0.0;
    }
    for (i = 0; i < PH_TRING && i < ph_tn; i++) {
        const ph_tick_t *t = &ph_tk[(ph_tn - 1u - i) % PH_TRING];
        if (t->q < lim) { break; }
        s->ticks++;
        s->period += t->period;
        s->wait += t->wait;
        s->iwait += t->iwait;
        if (t->period > s->period_max) { s->period_max = t->period; }
        s->busy += t->busy;
        s->render += t->render;
        s->interp += t->interp;
        s->pres += t->pres;
        s->game += t->game;
        s->draws += t->draws;
        s->vbloop += t->vbloop;
        s->submit += t->submit;
        s->cap += t->cap;
        s->draw += t->draw;
        s->tex += t->tex;
        s->texn += t->texn;
        s->upn += t->upn;
        if (t->upn > s->upn_max) { s->upn_max = t->upn; }
        if (t->busy > s->busy_max) { s->busy_max = t->busy; }
    }
    if (s->ticks) {
        double k = 1.0 / s->ticks;
        s->tps = s->ticks / win_s;
        s->period *= k; s->wait *= k; s->iwait *= k;
        s->busy *= k; s->render *= k; s->interp *= k; s->pres *= k; s->game *= k; s->draws *= k;
        s->vbloop *= k; s->submit *= k; s->cap *= k; s->draw *= k; s->tex *= k; s->texn *= k; s->upn *= k;
    }
    {
        double zs = 0.0, ts = 0.0;
        for (i = 0; i < PH_GRING && i < ph_gn; i++) {
            const ph_gres_t *g = &ph_gr[(ph_gn - 1u - i) % PH_GRING];
            if (g->q < lim) { break; }
            if (g->tick) { ts += g->ms; s->gpu_nt++; }
            else { zs += g->ms; s->gpu_nz++; }
            if (g->ms > s->gpu_max) { s->gpu_max = g->ms; }
        }
        s->gpu_z = s->gpu_nz ? zs / s->gpu_nz : 0.0;
        s->gpu_t = s->gpu_nt ? ts / s->gpu_nt : 0.0;
    }
}

/* W200: existing overlay LOG hook, same measured window as the F2 panel. */
void sb_phud_test_log(void)
{
    ph_stat_t s;
    if (!g_phud_meas) { return; }
    ph_stats(1.0, &s);
    HLOG_I("FPS-SETTING: cfg=%d target=%d paused=%d fps=%.2f frames=%u "
           "ft_avg=%.3f p50=%.3f p99=%.3f max=%.3f ticks/s=%.2f",
           g_cfg.interp_fps, g_sb_paused ? sb_ovl_pause_fps() :
           (g_cfg.interp_fps > 0 ? g_cfg.interp_fps : 30), (int)g_sb_paused,
           s.fps, s.frames, s.ft_avg, s.p50, s.p99, s.ft_max, s.tps);
}

/* ------------------------------------------------------------------ */
/* Messpunkte                                                          */
/* ------------------------------------------------------------------ */
static void ph_init_once(void)
{
    const char *u;
    ph_init = 1;
    QueryPerformanceFrequency(&ph_qpf);
    if (ph_vk < 0) { InterlockedExchange(&ph_vk, ph_parse_key(g_cfg.perf_hud_key)); }
    ph_vis = g_cfg.perf_hud ? 1 : 0;
    ph_pos = g_cfg.perf_hud_pos & 3;
    if (g_cfg.overlay_save_path[0]) { strcpy_s(ph_save_path, sizeof ph_save_path, g_cfg.overlay_save_path); }
    else if ((u = hcfg_user_cfg_path()) != NULL && u[0]) { strcpy_s(ph_save_path, sizeof ph_save_path, u); }
    HLOG_I("PERFHUD: bereit (Taste %s = an/aus, Shift = Ecke; perf_hud=%d pos=%d perf_log=%d; speichern nach %s)",
           ph_vk > 0 ? g_cfg.perf_hud_key : "aus", ph_vis, ph_pos, g_cfg.perf_log,
           ph_save_path[0] ? ph_save_path : "(kein Pfad)");
}

static void ph_set_meas(int on)
{
    if (on && !g_phud_meas) {
        unsigned k;
        ph_last_pres = 0;
        ph_tick_end_q = 0;
        ph_wait_acc = ph_interp_other = ph_interp_inwait = 0;
        for (k = 0; k < RQP_N; k++) { ph_rq_last[k] = g_rqp_q[k]; }
        ph_tex_last = g_phud_texq;
        ph_texn_last = g_phud_texn;
        ph_upn_last = g_phud_upn;
    }
    if (!on && g_phud_meas && ph_gq_open >= 0 && g_ctx) {
        sb_ctx_enter();
        ph_gq_end();
        sb_ctx_leave();
    }
    g_phud_meas = on;
}

void sb_phud_cycle_pos(void)
{
    InterlockedIncrement(&ph_req_pos);
}

int sb_phud_visible(void)
{
    return ph_vis;
}

/* DIAG(TICK-PERF,W125): Blockier-Zeitmesser (sb_perfhud.h TB_*) */
volatile int g_tb_on;
long long g_tb_q[TB_N];
unsigned g_tb_n[TB_N];
int g_phud_in_cap;
static long long ph_swap_q0;

void sb_phud_tick_begin(unsigned draws)
{
    if (!ph_init) { ph_init_once(); ph_set_meas(ph_vis || g_cfg.perf_log > 0 || g_cfg.perf_trace); }
    if (ph_req_toggle) {
        InterlockedExchange(&ph_req_toggle, 0);
        ph_vis = !ph_vis;
        ph_last_paint = 0;
        HLOG_I("PERFHUD: %s (f=%u)", ph_vis ? "an" : "aus", g_present_no);
        ph_save_cfg();
    }
    if (ph_req_pos) {
        InterlockedExchange(&ph_req_pos, 0);
        ph_pos = (ph_pos + 1) & 3;
        HLOG_I("PERFHUD: Ecke -> %d (%s, f=%u)", ph_pos,
               ph_pos == 0 ? "oben rechts" : ph_pos == 1 ? "oben links" : ph_pos == 2 ? "unten links" : "unten rechts",
               g_present_no);
        ph_save_cfg();
    }
    ph_set_meas(ph_vis || g_cfg.perf_log > 0 || g_cfg.perf_trace);
    g_tb_on = (g_phud_meas && g_cfg.perf_blk && g_cfg.perf_log > 0) ? 1 : 0; /* DIAG(TICK-PERF,W125) */
    if (!g_phud_meas) { return; }
    ph_swap_q0 = TB_BEGIN(); /* DIAG(TICK-PERF,W125): Swap = sb_render_present */
    ph_tick_draws = draws;
    ph_in_tick = 1;
    if (g_cfg.perf_prof && !ph_prof_th) { ph_prof_start(); } /* DIAG(PERF-HUD,W122) */
    ph_prof_run = g_cfg.perf_prof && rq_in_mission();
    ph_tick_since_pres = 1;
}

static void ph_log(void);

void sb_phud_tick_end(void)
{
    LONGLONG now;
    ph_tick_t *t;
    long long d[RQP_N];
    unsigned k;
    if (!g_phud_meas || !ph_in_tick) { return; }
    ph_in_tick = 0;
    if (ph_swap_q0) { TB_END(TB_SWAP, ph_swap_q0); ph_swap_q0 = 0; } /* DIAG(TICK-PERF,W125) */
    now = ph_now();
    for (k = 0; k < RQP_N; k++) {
        d[k] = g_rqp_q[k] - ph_rq_last[k];
        if (d[k] < 0) { d[k] = g_rqp_q[k]; } /* rq_perf_reset dazwischen */
        ph_rq_last[k] = g_rqp_q[k];
    }
    t = &ph_tk[ph_tn % PH_TRING];
    memset(t, 0, sizeof *t);
    t->q = now;
    /* Periode = Tick-Ende zu Tick-Ende; Warten = Zeit im VBlank-/Feld-Warten
     * des Spiels (hooks_timing: Hook-Koerper + Spin-Luecken der Poll-Schleife,
     * inkl. dort gezeigter Zwischenbilder). Tick-Last = Periode - Warten. */
    t->period = ph_tick_end_q ? (float)ph_ms(now - ph_tick_end_q) : 0.0f;
    t->wait = (float)ph_ms(ph_wait_acc);
    if (t->wait > t->period) { t->wait = t->period; }
    t->busy = t->period - t->wait;
    t->vbloop = (float)ph_ms(d[RQP_VBLOOP]);
    t->submit = (float)ph_ms(d[RQP_SUBMIT]);
    t->cap = (float)ph_ms(d[RQP_CAP]);
    t->draw = (float)ph_ms(d[RQP_DRAW]);
    t->render = (float)ph_ms(d[RQP_VBLOOP] + d[RQP_SUBMIT] + d[RQP_PREP] - d[RQP_CAP]);
    if (t->render < 0.0f) { t->render = 0.0f; }
    /* Interp ausserhalb des Wartens: Aufzeichnung (cap, enthaelt die
     * Zwischenbilder aus rec_draw) + Swap-Aufzeichnung + sonstige Bilder. */
    t->interp = (float)ph_ms(ph_interp_other + d[RQP_CAP]);
    t->iwait = (float)ph_ms(ph_interp_inwait);
    t->pres = (float)ph_ms(ph_pres_direct);
    t->game = t->busy - t->render - t->interp - t->pres;
    if (t->game < 0.0f) { t->game = 0.0f; }
    ph_wait_acc = 0;
    ph_interp_other = 0;
    ph_interp_inwait = 0;
    t->draws = (float)ph_tick_draws;
    t->tex = (float)ph_ms(g_phud_texq - ph_tex_last);
    t->texn = g_phud_texn - ph_texn_last;
    t->upn = g_phud_upn - ph_upn_last;
    ph_tex_last = g_phud_texq;
    ph_texn_last = g_phud_texn;
    ph_upn_last = g_phud_upn;
    ph_tn++;
    if (g_cfg.perf_trace) { /* DIAG(PERF-BENCH,W137) */
        double vq = -1.0;
        if (g_tb_on) {
            long long dv = g_tb_q[TB_VISQ] - pt_visq0;
            vq = dv >= 0 ? ph_ms(dv) : 0.0;
        }
        pt_visq0 = g_tb_q[TB_VISQ];
        pt_add(now, "T,%.4f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%.3f,%u\n", pt_sec(now), (double)t->period, (double)t->wait,
               (double)t->busy, (double)t->render, (double)t->interp, (double)t->game, (unsigned)t->draws, vq, t->upn);
    }
    ph_pres_direct = 0;
    ph_tick_end_q = now;
    if (g_cfg.ctx_wait_diag) { /* DIAG(PERF-TICK,W168): ctx_lock-Warten je Tick (Maximum) */
        extern volatile LONGLONG g_ctxw_tick_q, g_ctxw_tick_max;
        if (g_ctxw_tick_q > g_ctxw_tick_max) { g_ctxw_tick_max = g_ctxw_tick_q; }
        g_ctxw_tick_q = 0;
    }
    if (g_cfg.perf_log > 0) {
        if (!ph_last_log) { ph_last_log = now; }
        else if (now - ph_last_log >= (LONGLONG)g_cfg.perf_log * ph_qpf.QuadPart) {
            ph_last_log = now;
            ph_log();
        }
    }
}

/* Warte-Hooks (hooks_timing.c, Spiel-Thread): Koerper = Warten; Luecke seit
 * dem letzten Austritt < 0,5 ms = Poll-Schleife des Spiels = ebenfalls Warten. */
volatile int g_phud_in_wait;
volatile int g_ph_zone; /* DIAG(PERF-TICK,W168) */
static LONGLONG ph_wait_exit_q, ph_wait_enter_q;

void sb_phud_wait_enter(void)
{
    LONGLONG now;
    if (!g_phud_meas) { return; }
    now = ph_now();
    if (ph_wait_exit_q && now - ph_wait_exit_q < ph_qpf.QuadPart / 2000) { ph_wait_acc += now - ph_wait_exit_q; }
    ph_wait_enter_q = now;
    g_phud_in_wait = 1;
}

void sb_phud_wait_leave(void)
{
    LONGLONG now;
    if (!g_phud_meas || !ph_wait_enter_q) { g_phud_in_wait = 0; return; }
    now = ph_now();
    ph_wait_acc += now - ph_wait_enter_q;
    ph_wait_exit_q = now;
    ph_wait_enter_q = 0;
    g_phud_in_wait = 0;
}

/* where: 0 = sonst (Swap-Aufzeichnung, Bild ausserhalb), 1 = in rec_draw
 * (steckt schon in cap), 2 = im Warten (steckt in wait). */
void sb_phud_interp_q(long long dq, int where)
{
    if (!g_phud_meas) { return; }
    /* W125: where = 1 (aus rec_draw/rec_clear) steckt nur im cap-Zaehler,
     * wenn der Aufruf aus render_im.c kam; Clear-Hook und Aspect-Balken
     * liegen ausserhalb -> wie sonstige Interp-Zeit zaehlen (vorher "Spiel"). */
    if (where == 1 && !g_phud_in_cap) {
        where = 0;
        if (g_tb_on) { g_tb_q[TB_IPX] += dq; g_tb_n[TB_IPX]++; }
    }
    if (where == 0) { ph_interp_other += dq; }
    else if (where == 2) { ph_interp_inwait += dq; }
}

void sb_phud_present_pre(void)
{
    if (!g_phud_meas) { return; }
    ph_gq_end();
}

void sb_phud_present_post(long long q_before, int direct)
{
    LONGLONG now;
    ph_frame_t *f;
    if (!g_phud_meas) { return; }
    now = ph_now();
    if (direct && ph_in_tick) { ph_pres_direct += now - q_before; }
    if (ph_last_pres) {
        f = &ph_fr[ph_fn % PH_RING];
        f->q = now;
        f->ft = (float)ph_ms(now - ph_last_pres);
        f->pres = (float)ph_ms(now - q_before);
        f->tick = (unsigned char)(ph_tick_since_pres ? 1 : 0);
        f->skip = g_ip_skip_tot;
        ph_fn++;
        if (g_cfg.perf_trace) { /* DIAG(PERF-BENCH,W137): Szene = u32 [0x6B96DC] (0x2C Mission, SPIELZUSTAENDE.md) */
            pt_add(now, "P,%.4f,%.3f,%.3f,%u,%d,%u,%u,%d\n", pt_sec(now), (double)f->ft, (double)f->pres, (unsigned)f->tick,
                   rq_in_mission(), *(volatile unsigned *)(uintptr_t)SBA(0x6B96DCu), g_ip_skip_tot, g_phud_src); /* W138: + Quelle src (0 Spiel-Thread, 1 Takt-Thread, 2 Wiederholung) */
        }
        if (g_cfg.perf_log > 0) { /* DIAG(PERF-HUD,W122): Spitzenbilder einzeln */
            static unsigned nsp;
            double thr = g_cfg.perf_spike_ms > 0 ? (double)g_cfg.perf_spike_ms : ph_target_ms() * 2.0;
            if (f->ft > thr && nsp < 400u) {
                const ph_tick_t *t = ph_tn ? &ph_tk[(ph_tn - 1u) % PH_TRING] : NULL;
                nsp++;
                HLOG_I("PERFSPIKE f=%u ft=%.2fms art=%s pres=%.2f letzter Tick: per=%.2f wait=%.2f busy=%.2f spiel=%.2f render=%.2f "
                       "(vb=%.2f sub=%.2f draw=%.2f tex=%.2f/%u up=%u) interp=%.2f (cap=%.2f) draws=%.0f skip=%u",
                       g_present_no, (double)f->ft, f->tick ? "tick" : "zw", (double)f->pres,
                       t ? (double)t->period : 0.0, t ? (double)t->wait : 0.0, t ? (double)t->busy : 0.0, t ? (double)t->game : 0.0, t ? (double)t->render : 0.0,
                       t ? (double)t->vbloop : 0.0, t ? (double)t->submit : 0.0, t ? (double)t->draw : 0.0,
                       t ? (double)t->tex : 0.0, t ? t->texn : 0u, t ? t->upn : 0u,
                       t ? (double)t->interp : 0.0, t ? (double)t->cap : 0.0, t ? (double)t->draws : 0.0,
                       g_ip_skip_tot);
            }
        }
    }
    ph_last_pres = now;
    ph_tick_since_pres = 0;
    ph_gq_poll();
    if (ph_vis || g_cfg.perf_log > 0 || g_cfg.perf_trace) { ph_gq_begin(); }
}

/* DIAG(PERF-HUD,W122): CPU-Zeit je Thread des Prozesses im Log-Fenster
 * (Kernel+User, GetThreadTimes), Top 6 -> Zeile PERFTHR. Zeigt, welcher
 * Thread dem Spiel-Thread den Kern nimmt (Xbox-Threads teilen sich SB_CORE). */
#define PH_THR 128
static DWORD ph_thr_id[PH_THR];
static ULONGLONG ph_thr_t[PH_THR];
static int ph_thr_n;
static HANDLE ph_thr_h[PH_THR];
static DWORD ph_thr_hid[PH_THR];
static int ph_thr_hn;
static void ph_thr_handles(void)
{
    /* Toolhelp-Snapshot ueber alle Threads des Systems kostet unter Last bis
     * ~160 ms (W122_r3) -> nur ausserhalb der Mission bzw. beim ersten Mal. */
    HANDLE sn = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    DWORD pid = GetCurrentProcessId();
    int i;
    if (sn == INVALID_HANDLE_VALUE) { return; }
    for (i = 0; i < ph_thr_hn; i++) { if (ph_thr_h[i]) { CloseHandle(ph_thr_h[i]); } }
    ph_thr_hn = 0;
    te.dwSize = sizeof te;
    if (Thread32First(sn, &te)) {
        do {
            if (te.th32OwnerProcessID == pid && ph_thr_hn < PH_THR) {
                HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
                if (h) { ph_thr_h[ph_thr_hn] = h; ph_thr_hid[ph_thr_hn] = te.th32ThreadID; ph_thr_hn++; }
            }
            te.dwSize = sizeof te;
        } while (Thread32Next(sn, &te));
    }
    CloseHandle(sn);
}

static void ph_log_threads(double win_s)
{
    THREADENTRY32 te;
    DWORD pid = GetCurrentProcessId();
    DWORD nid[PH_THR];
    ULONGLONG nt[PH_THR];
    double dms[PH_THR];
    int nn = 0, i, j, top[6], ntop = 0;
    char buf[512];
    size_t len = 0;
    double sum = 0.0;
    int x;
    if (!ph_thr_hn || (!rq_in_mission() && !g_cfg.perf_trace)) { ph_thr_handles(); } /* W137: mit perf_trace kein ~150-ms-Snapshot alle perf_log s */
    for (x = 0; x < ph_thr_hn; x++) {
        {
            {
                HANDLE h = ph_thr_h[x];
                FILETIME c, e, k, u;
                {
                    if (GetThreadTimes(h, &c, &e, &k, &u)) {
                        ULONGLONG t = (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) +
                                      (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime);
                        nid[nn] = ph_thr_hid[x];
                        nt[nn] = t;
                        dms[nn] = 0.0;
                        for (j = 0; j < ph_thr_n; j++) {
                            if (ph_thr_id[j] == nid[nn]) { dms[nn] = (double)(t - ph_thr_t[j]) / 10000.0; break; }
                        }
                        if (j >= ph_thr_n) { dms[nn] = (double)t / 10000.0; }
                        sum += dms[nn];
                        nn++;
                    }
                }
            }
        }
    }
    for (i = 0; i < 6; i++) {
        int b = -1;
        for (j = 0; j < nn; j++) {
            int used = 0, q;
            for (q = 0; q < ntop; q++) { if (top[q] == j) { used = 1; } }
            if (!used && (b < 0 || dms[j] > dms[b])) { b = j; }
        }
        if (b < 0) { break; }
        top[ntop++] = b;
    }
    for (i = 0; i < ntop; i++) {
        len += (size_t)_snprintf_s(buf + len, sizeof buf - len, _TRUNCATE, " T%04lx=%.0f%%", (unsigned long)nid[top[i]],
                                   win_s > 0.0 ? dms[top[i]] / (win_s * 10.0) : 0.0);
    }
    HLOG_I("PERFTHR f=%u fenster=%.1fs threads=%d summe=%.0f%% (eines Kerns) top:%s", g_present_no, win_s, nn,
           win_s > 0.0 ? sum / (win_s * 10.0) : 0.0, buf);
    memcpy(ph_thr_id, nid, sizeof(DWORD) * (size_t)nn);
    memcpy(ph_thr_t, nt, sizeof(ULONGLONG) * (size_t)nn);
    ph_thr_n = nn;
}

/* DIAG(PERF-HUD,W122): Stichproben-Profiler des Spiel-Threads (perf_prof=1).
 * Hilfsthread: ~1/ms SuspendThread + GetThreadContext(EIP) + ResumeThread,
 * waehrend der Pause weder Sperren noch Speicher noch Log. Auswertung in
 * ph_log (Spiel-Thread): sb_emu-Adressen per dbghelp (sb_emu.pdb) als
 * Funktion, andere Module als Modulname, XBE-Code als Adresse/256 (Zeile
 * PERFPROF, symbolize.py benennt EIP=0x...). */
#define PH_PROF_N 8192
static DWORD ph_prof_eip[PH_PROF_N];
static DWORD ph_prof_remu[PH_PROF_N], ph_prof_rxbe[PH_PROF_N]; /* erste sb_emu-/XBE-Ruecksprungadresse auf dem Stack */
static unsigned ph_prof_tn[PH_PROF_N]; /* DIAG(PERF-TICK,W168): Tick-Index der Probe (perf_prof>=2) */
static unsigned char ph_prof_zone[PH_PROF_N]; /* DIAG(PERF-TICK,W168): g_ph_zone der Probe */
static DWORD ph_emu_lo, ph_emu_hi;
static volatile LONG ph_prof_n;
static DWORD ph_prof_tid;
typedef BOOL(WINAPI *ph_syminit_t)(HANDLE, PCSTR, BOOL);
typedef DWORD(WINAPI *ph_symopt_t)(DWORD);
typedef struct { ULONG SizeOfStruct, TypeIndex; ULONG64 Reserved[2]; ULONG Index, Size; ULONG64 ModBase;
                 ULONG Flags; ULONG64 Value, Address; ULONG Register, Scope, Tag, NameLen, MaxNameLen;
                 CHAR Name[1]; } ph_sym_t; /* = SYMBOL_INFO */
typedef BOOL(WINAPI *ph_symfrom_t)(HANDLE, ULONG64, ULONG64 *, ph_sym_t *);
static ph_symfrom_t ph_symfrom;

static DWORD WINAPI ph_prof_main(void *arg)
{
    HANDLE t = (HANDLE)arg;
    (void)arg;
    for (;;) {
        LONG n;
        CONTEXT c;
        Sleep(1);
        if (!ph_prof_run) { continue; }
        n = ph_prof_n;
        if (n >= PH_PROF_N) { continue; }
        if (SuspendThread(t) == (DWORD)-1) { continue; }
        /* DIAG(PERF-TICK,W168): perf_prof>=2 = nur Proben ausserhalb des Wartens (Tick-Last) */
        if (g_cfg.perf_prof >= 2 && g_phud_in_wait) { ResumeThread(t); continue; }
        memset(&c, 0, sizeof c);
        c.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(t, &c)) {
            /* Stack (max. 4 KB) nach der ersten sb_emu- und XBE-Adresse
             * durchsuchen (Heuristik; Ziel angehalten, keine Sperren). */
            DWORD re = 0, rx = 0, k;
            const DWORD *sp = (const DWORD *)(uintptr_t)c.Esp;
            __try {
                for (k = 0; k < 1024u && (!re || !rx); k++) {
                    DWORD v = sp[k];
                    if (!re && v >= ph_emu_lo && v < ph_emu_hi) { re = v; }
                    if (!rx && v >= 0x12000u && v < 0x300000u) { rx = v; }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            ph_prof_eip[n] = c.Eip;
            ph_prof_remu[n] = re;
            ph_prof_rxbe[n] = rx;
            ph_prof_tn[n] = ph_tn;
            ph_prof_zone[n] = (unsigned char)g_ph_zone;
            InterlockedExchange(&ph_prof_n, n + 1);
        }
        ResumeThread(t);
    }
    return 0; /* unerreichbar (Endlosschleife); beseitigt C4716 */
}

static void ph_prof_start(void)
{
    HANDLE t;
    HMODULE dh;
    if (ph_prof_th || !g_cfg.perf_prof) { return; }
    ph_prof_tid = GetCurrentThreadId();
    t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, ph_prof_tid);
    if (!t) { HLOG_W("PERFPROF: OpenThread fehlgeschlagen"); return; }
    dh = LoadLibraryA("dbghelp.dll");
    if (dh) {
        ph_syminit_t si = (ph_syminit_t)GetProcAddress(dh, "SymInitialize");
        ph_symopt_t so = (ph_symopt_t)GetProcAddress(dh, "SymSetOptions");
        ph_symfrom = (ph_symfrom_t)GetProcAddress(dh, "SymFromAddr");
        if (so) { so(0x00000002u /* UNDNAME */ | 0x00000004u /* DEFERRED_LOADS */); }
        if (!si || !si(GetCurrentProcess(), NULL, TRUE)) { ph_symfrom = NULL; }
    }
    {
        HMODULE self = NULL;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)&ph_prof_start, &self);
        if (self) {
            const IMAGE_DOS_HEADER *dh2 = (const IMAGE_DOS_HEADER *)self;
            const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const char *)self + dh2->e_lfanew);
            ph_emu_lo = (DWORD)(uintptr_t)self;
            ph_emu_hi = ph_emu_lo + nt->OptionalHeader.SizeOfImage;
        }
    }
    ph_prof_th = CreateThread(NULL, 0, ph_prof_main, t, 0, NULL);
    HLOG_I("PERFPROF: Profiler an (Ziel T%04lx, dbghelp=%d)", (unsigned long)ph_prof_tid, ph_symfrom != NULL);
}

typedef struct { char name[64]; unsigned n; } ph_bucket_t;

/* DIAG(PERF-TICK,W168): perf_prof=2 - Proben nur ausserhalb des Wartens, aufgeteilt nach
 * Herkunft (XBE-Code = kein Modul, sb_emu, D3D/Treiber, sonstige Systemmodule) und nach
 * schweren Ticks (Tick-Last > perf_prof_heavy ms, 0 = 28). Zeile PERFPROF3 je Fenster:
 * alle Proben + schwere Ticks mit den haeufigsten Funktionen (XBE in 4-KB-Bloecken,
 * Systemmodule mit dem sb_emu-Aufrufer). */
static void ph_prof3(LONG n)
{
    static ph_bucket_t b[200];
    int nb = 0, i, j, k;
    unsigned cnt[2][4] = {{0}}, tot[2] = {0, 0}, zn[3] = {0, 0, 0};
    char line[1800];
    size_t len = 0;
    float heavy = g_cfg.perf_prof_heavy > 0 ? (float)g_cfg.perf_prof_heavy : 28.0f;
    HMODULE self = NULL;
    union { ph_sym_t s; char buf[sizeof(ph_sym_t) + 128]; } sym;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&ph_prof3, &self);
    for (i = 0; i < n; i++) {
        DWORD a = ph_prof_eip[i];
        unsigned tn = ph_prof_tn[i];
        int hv = 0, cls;
        char nm[64];
        HMODULE m = NULL;
        ULONG64 disp = 0;
        if (tn < ph_tn && ph_tn - tn <= PH_TRING) { hv = ph_tk[tn % PH_TRING].busy > heavy; }
        if (hv) { zn[ph_prof_zone[i] < 3 ? ph_prof_zone[i] : 0]++; }
        if (hv && ph_prof_zone[i] != 0) { hv = 2; } /* schwer, aber Uebergabe/Swap: nicht in der Spiel-Liste */
        nm[0] = 0;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)(uintptr_t)a, &m) && m) {
            char path[MAX_PATH];
            const char *base;
            if (m == self) {
                cls = 1;
                if (ph_symfrom) {
                    memset(&sym, 0, sizeof sym);
                    sym.s.SizeOfStruct = 88;
                    sym.s.MaxNameLen = 120;
                    if (ph_symfrom(GetCurrentProcess(), a, &disp, &sym.s)) { _snprintf_s(nm, sizeof nm, _TRUNCATE, "%s", sym.s.Name); }
                }
                if (!nm[0]) { _snprintf_s(nm, sizeof nm, _TRUNCATE, "emu+0x%lX", (unsigned long)(a - ph_emu_lo)); }
            } else {
                char e2[48];
                GetModuleFileNameA(m, path, sizeof path);
                base = strrchr(path, '\\');
                base = base ? base + 1 : path;
                cls = (_strnicmp(base, "d3d", 3) == 0 || _strnicmp(base, "nv", 2) == 0 || _strnicmp(base, "dxgi", 4) == 0) ? 2 : 3;
                e2[0] = 0;
                if (ph_prof_remu[i] && ph_symfrom) {
                    memset(&sym, 0, sizeof sym);
                    sym.s.SizeOfStruct = 88;
                    sym.s.MaxNameLen = 120;
                    if (ph_symfrom(GetCurrentProcess(), ph_prof_remu[i], &disp, &sym.s)) {
                        _snprintf_s(e2, sizeof e2, _TRUNCATE, "%s", sym.s.Name);
                    }
                }
                {
                    char fn[40];
                    fn[0] = 0;
                    if (ph_symfrom) {
                        memset(&sym, 0, sizeof sym);
                        sym.s.SizeOfStruct = 88;
                        sym.s.MaxNameLen = 120;
                        if (ph_symfrom(GetCurrentProcess(), a, &disp, &sym.s)) { _snprintf_s(fn, sizeof fn, _TRUNCATE, "%s", sym.s.Name); }
                    }
                    _snprintf_s(nm, sizeof nm, _TRUNCATE, "%.8s!%.22s<%s", base, fn[0] ? fn : "?", e2[0] ? e2 : "?");
                }
            }
        } else {
            cls = 0;
            _snprintf_s(nm, sizeof nm, _TRUNCATE, "XBE:0x%06lX", (unsigned long)(a & ~0xFFFu));
        }
        if (g_cfg.perf_prof >= 3) { /* DIAG(PERF-TICK,W168): Rohproben nach SB_OUT_DIR/prof.csv */
            static FILE *pf;
            static int pf_tried;
            if (!pf && !pf_tried) {
                char dir[600], path[700];
                DWORD dn = GetEnvironmentVariableA("SB_OUT_DIR", dir, sizeof dir);
                pf_tried = 1;
                if (!dn || dn >= sizeof dir) { strcpy_s(dir, sizeof dir, "."); }
                _snprintf_s(path, sizeof path, _TRUNCATE, "%s/prof.csv", dir);
                if (fopen_s(&pf, path, "w") != 0) { pf = NULL; }
                if (pf) { fprintf(pf, "# tick,busy_ms,zone,klasse,name\n"); }
            }
            if (pf) {
                float bz = (tn < ph_tn && ph_tn - tn <= PH_TRING) ? ph_tk[tn % PH_TRING].busy : -1.0f;
                fprintf(pf, "%u,%.2f,%u,%d,%s\n", tn, (double)bz, (unsigned)ph_prof_zone[i], cls, nm);
            }
            if (i == n - 1 && pf) { fflush(pf); }
        }
        cnt[0][cls]++; tot[0]++;
        if (hv == 1) { cnt[1][cls]++; tot[1]++; }
        for (j = 0; j < nb && strcmp(b[j].name, nm) != 0; j++) {}
        if (j == nb) {
            if (nb >= 200) { continue; }
            strcpy_s(b[nb].name, sizeof b[nb].name, nm);
            b[nb].n = 0;
            nb++;
        }
        /* untere 16 Bit: alle Proben, obere 16 Bit: Proben in schweren Ticks */
        b[j].n += 1u + (hv == 1 ? 0x10000u : 0u);
    }
    len += (size_t)_snprintf_s(line + len, sizeof line - len, _TRUNCATE,
                               "alle n=%u xbe=%.1f%% emu=%.1f%% d3d=%.1f%% sys=%.1f%% | schwer(>%.0fms) spiel/uebergabe/swap=%u/%u/%u | schwer-spiel n=%u xbe=%.1f%% emu=%.1f%% d3d=%.1f%% sys=%.1f%% | top:",
                               tot[0], tot[0] ? 100.0 * cnt[0][0] / tot[0] : 0.0, tot[0] ? 100.0 * cnt[0][1] / tot[0] : 0.0,
                               tot[0] ? 100.0 * cnt[0][2] / tot[0] : 0.0, tot[0] ? 100.0 * cnt[0][3] / tot[0] : 0.0, heavy,
                               zn[0], zn[1], zn[2],
                               tot[1], tot[1] ? 100.0 * cnt[1][0] / tot[1] : 0.0, tot[1] ? 100.0 * cnt[1][1] / tot[1] : 0.0,
                               tot[1] ? 100.0 * cnt[1][2] / tot[1] : 0.0, tot[1] ? 100.0 * cnt[1][3] / tot[1] : 0.0);
    for (k = 0; k < 2; k++) {
        if (k == 1) { len += (size_t)_snprintf_s(line + len, sizeof line - len, _TRUNCATE, " | schwer:"); }
        for (i = 0; i < 16 && i < nb; i++) {
            int best = i;
            unsigned vb, vi;
            for (j = i + 1; j < nb; j++) {
                vb = k ? (b[best].n >> 16) : (b[best].n & 0xFFFFu);
                vi = k ? (b[j].n >> 16) : (b[j].n & 0xFFFFu);
                if (vi > vb) { best = j; }
            }
            if (best != i) { ph_bucket_t tmp = b[i]; b[i] = b[best]; b[best] = tmp; }
            vb = k ? (b[i].n >> 16) : (b[i].n & 0xFFFFu);
            if (!vb) { break; }
            len += (size_t)_snprintf_s(line + len, sizeof line - len, _TRUNCATE, " %s=%.1f%%", b[i].name,
                                       100.0 * vb / (tot[k] ? tot[k] : 1u));
        }
    }
    HLOG_I("PERFPROF3 f=%u %s", g_present_no, line);
}
static void ph_prof_log(void)
{
    static ph_bucket_t b[160], b2[160];
    static DWORD eip[PH_PROF_N], remu[PH_PROF_N], rxbe[PH_PROF_N];
    int nb2 = 0, nsys = 0;
    int nb = 0, i, j;
    LONG n = ph_prof_n;
    char line[1400];
    size_t len = 0;
    HMODULE self = NULL;
    union { ph_sym_t s; char buf[sizeof(ph_sym_t) + 128]; } sym;
    if (!ph_prof_th) { return; }
    if (n > PH_PROF_N) { n = PH_PROF_N; }
    memcpy(eip, ph_prof_eip, sizeof(DWORD) * (size_t)n);
    memcpy(remu, ph_prof_remu, sizeof(DWORD) * (size_t)n);
    memcpy(rxbe, ph_prof_rxbe, sizeof(DWORD) * (size_t)n);
    if (g_cfg.perf_prof >= 2) { ph_prof3(n); } /* DIAG(PERF-TICK,W168) */
    InterlockedExchange(&ph_prof_n, 0);
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&ph_prof_log, &self);
    for (i = 0; i < n; i++) {
        char nm[64];
        HMODULE m = NULL;
        DWORD a = eip[i];
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)(uintptr_t)a, &m) && m) {
            ULONG64 disp = 0;
            nm[0] = 0;
            if (m == self && ph_symfrom) {
                memset(&sym, 0, sizeof sym);
                sym.s.SizeOfStruct = 88; /* sizeof(SYMBOL_INFO) */
                sym.s.MaxNameLen = 120;
                if (ph_symfrom(GetCurrentProcess(), a, &disp, &sym.s)) { _snprintf_s(nm, sizeof nm, _TRUNCATE, "%s", sym.s.Name); }
            }
            if (!nm[0]) {
                char path[MAX_PATH], e2[48], k2[64];
                const char *base;
                int q;
                GetModuleFileNameA(m, path, sizeof path);
                base = strrchr(path, '\\');
                _snprintf_s(nm, sizeof nm, _TRUNCATE, "%s", base ? base + 1 : path);
                if (m != self) { /* Systemmodul: ueber welchen Hook, von welcher Spielstelle? */
                    e2[0] = 0;
                    if (remu[i] && ph_symfrom) {
                        memset(&sym, 0, sizeof sym);
                        sym.s.SizeOfStruct = 88;
                        sym.s.MaxNameLen = 120;
                        if (ph_symfrom(GetCurrentProcess(), remu[i], &disp, &sym.s)) {
                            _snprintf_s(e2, sizeof e2, _TRUNCATE, "%s", sym.s.Name);
                        }
                    }
                    _snprintf_s(k2, sizeof k2, _TRUNCATE, "%s<%s<EIP=0x%06lX", nm[0] == 'n' ? "nt" : nm,
                                e2[0] ? e2 : "?", (unsigned long)rxbe[i]);
                    nsys++;
                    for (q = 0; q < nb2 && strcmp(b2[q].name, k2) != 0; q++) {}
                    if (q == nb2 && nb2 < 160) { strcpy_s(b2[nb2].name, sizeof b2[nb2].name, k2); b2[nb2].n = 0; nb2++; }
                    if (q < nb2) { b2[q].n++; }
                }
            }
        } else {
            _snprintf_s(nm, sizeof nm, _TRUNCATE, "EIP=0x%06lX", (unsigned long)(a & ~0xFFu));
        }
        for (j = 0; j < nb && strcmp(b[j].name, nm) != 0; j++) {}
        if (j == nb) {
            if (nb >= 160) { continue; }
            strcpy_s(b[nb].name, sizeof b[nb].name, nm);
            b[nb].n = 0;
            nb++;
        }
        b[j].n++;
    }
    for (i = 0; i < 14 && i < nb; i++) { /* Top 14 nach vorn sortieren */
        int best = i;
        for (j = i + 1; j < nb; j++) { if (b[j].n > b[best].n) { best = j; } }
        if (best != i) { ph_bucket_t tmp = b[i]; b[i] = b[best]; b[best] = tmp; }
        len += (size_t)_snprintf_s(line + len, sizeof line - len, _TRUNCATE, " %s=%.1f%%", b[i].name,
                                   n ? 100.0 * b[i].n / n : 0.0);
    }
    HLOG_I("PERFPROF f=%u miss=%d n=%ld top:%s", g_present_no, rq_in_mission(), (long)n, line);
    len = 0;
    line[0] = 0;
    for (i = 0; i < 10 && i < nb2; i++) {
        int best = i;
        for (j = i + 1; j < nb2; j++) { if (b2[j].n > b2[best].n) { best = j; } }
        if (best != i) { ph_bucket_t tmp = b2[i]; b2[i] = b2[best]; b2[best] = tmp; }
        len += (size_t)_snprintf_s(line + len, sizeof line - len, _TRUNCATE, " %s=%.1f%%", b2[i].name,
                                   n ? 100.0 * b2[i].n / n : 0.0);
    }
    HLOG_I("PERFPROF2 f=%u sys=%d (Systemmodul<Hook<Spielstelle):%s", g_present_no, nsys, line);
}

static void ph_log(void)
{
    static unsigned la_n0, la_s0;
    static long long la_q0;
    ph_stat_t s;
    ph_log_threads((double)g_cfg.perf_log);
    ph_prof_log();
    ph_stats((double)g_cfg.perf_log, &s);
    HLOG_I("PERFHUD f=%u miss=%d fps=%.1f ticks/s=%.2f ft avg=%.2f min=%.2f p50=%.2f p99=%.2f max=%.2f low1=%.1f "
           ">1.5x=%u >2x=%u n=%u tickbilder=%u skip=%u | tick per=%.2f(max %.2f) wait=%.2f (zw=%.2f) busy=%.2f(max %.2f) spiel=%.2f render=%.2f "
           "(vb=%.2f sub=%.2f draw=%.2f tex=%.2f/%.0f up=%.1f(max %.0f)) interp=%.2f (cap=%.2f) pres=%.2f draws=%.0f | gpu zw=%.2f(n=%u) "
           "tick=%.2f(n=%u) max=%.2f | present=%.3fms | lowalloc n=%u abfr=%u ms=%.2f | interp_fps=%d shading=%d bb=%ux%u",
           g_present_no, rq_in_mission(), s.fps, s.tps, s.ft_avg, s.ft_min, s.p50, s.p99, s.ft_max, s.low1,
           s.over15, s.over2, s.frames, s.frames_tick, s.skip, s.period, s.period_max, s.wait, s.iwait, s.busy,
           s.busy_max, s.game, s.render, s.vbloop,
           s.submit, s.draw, s.tex, s.texn, s.upn, s.upn_max, s.interp, s.cap, s.pres, s.draws, s.gpu_z, s.gpu_nz, s.gpu_t, s.gpu_nt, s.gpu_max,
           s.pres_avg, g_la_n - la_n0, g_la_steps - la_s0, ph_ms(g_la_q - la_q0), g_cfg.interp_fps, g_cfg.shading,
           g_bb_w, g_bb_h);
    la_n0 = g_la_n;
    la_s0 = g_la_steps;
    la_q0 = g_la_q;
    if (g_cfg.perf_blk) { /* DIAG(TICK-PERF,W125): ms je Tick (Fenster), Aufrufe je Tick */
        static long long tq0[TB_N];
        static unsigned tn0[TB_N], tk0;
        static const char *const nm[TB_N] = { "blend", "visq", "ipmap", "ipx", "rtt", "clear", "swap", "lock" };
        char line[600];
        int k, o = 0;
        unsigned tk = ph_tn - tk0;
        double kt = tk ? 1.0 / (double)tk : 0.0;
        for (k = 0; k < TB_N; k++) {
            o += _snprintf_s(line + o, sizeof line - (size_t)o, _TRUNCATE, " %s=%.2f(%.1f)", nm[k],
                             ph_ms(g_tb_q[k] - tq0[k]) * kt, (double)(g_tb_n[k] - tn0[k]) * kt);
            tq0[k] = g_tb_q[k];
            tn0[k] = g_tb_n[k];
            if (o < 0) { break; }
        }
        tk0 = ph_tn;
        { extern long long g_vq_e2g_q; extern unsigned g_vq_e2g_n, g_vq_e2g_swap;
          static long long e0; static unsigned n0, s0;
          unsigned dn = g_vq_e2g_n - n0;
          _snprintf_s(line + (o > 0 ? o : 0), sizeof line - (size_t)(o > 0 ? o : 0), _TRUNCATE, " | visq end->get=%.2fms n=%u ueber_swap=%u",
                      dn ? ph_ms(g_vq_e2g_q - e0) / dn : 0.0, dn, g_vq_e2g_swap - s0);
          e0 = g_vq_e2g_q; n0 = g_vq_e2g_n; s0 = g_vq_e2g_swap; }
        { extern unsigned g_ip_bs_idle, g_ip_bs_busy, g_ip_bs_new, g_ip_gskip, g_ip_gtight;
          static unsigned i0, b0, n0, g0, t0;
          size_t l = strlen(line);
          _snprintf_s(line + l, sizeof line - l, _TRUNCATE, " | bsets frei=%u belegt=%u neu=%u | gpu_q verfallen=%u eng=%u",
                      g_ip_bs_idle - i0, g_ip_bs_busy - b0, g_ip_bs_new - n0, g_ip_gskip - g0, g_ip_gtight - t0);
          i0 = g_ip_bs_idle; b0 = g_ip_bs_busy; n0 = g_ip_bs_new; g0 = g_ip_gskip; t0 = g_ip_gtight; }
        HLOG_I("TICKBLK f=%u ticks=%u ms/Tick(Aufrufe/Tick):%s", g_present_no, tk, line);
    }
    if (g_cfg.perf_gpass > 0) { /* DIAG(GPU-COST,W146): ms je Zwischenbild je Pass */
        char gl[400];
        unsigned n = gp_frames;
        gp_window(gl, sizeof gl);
        HLOG_I("GPUPASS f=%u n=%u ms/Bild:%s", g_present_no, n, gl);
    }
}

/* ------------------------------------------------------------------ */
/* Panel (GDI, Look des ESC-Overlays)                                  */
/* ------------------------------------------------------------------ */
#define PH_C_WARN RGB(255, 176, 64)

static int S(float x)
{
    int v = (int)(x * ph_f + 0.5f);
    return v < 1 ? 1 : v;
}

static void ph_fill(int x0, int y0, int x1, int y1, COLORREF c)
{
    RECT r;
    HBRUSH b = CreateSolidBrush(c);
    r.left = x0; r.top = y0; r.right = x1; r.bottom = y1;
    FillRect(ph_dc, &r, b);
    DeleteObject(b);
}

static void ph_text(int x, int y, const wchar_t *s, COLORREF c, int right)
{
    SetTextColor(ph_dc, c);
    SetTextAlign(ph_dc, (right ? TA_RIGHT : TA_LEFT) | TA_TOP);
    TextOutW(ph_dc, x, y, s, (int)wcslen(s));
}

#define PH_ROWS 8
static int ph_vr_paint; /* W197 VR-MENU: 1 = Zeichnen fuer die VR-Flaeche (Zusatzzeile VR) */
static void ph_layout_f(unsigned *w, unsigned *h);
static void ph_layout(unsigned bbh, unsigned *w, unsigned *h)
{
    unsigned bx, by, bw, bh;
    sb_aspect_box(&bx, &by, &bw, &bh);
    (void)bbh;
    ph_f = (float)bh / 540.0f;
    if (ph_f < 1.0f) { ph_f = 1.0f; }
    ph_layout_f(w, h);
}

/* Panel-Groesse beim aktuellen Faktor ph_f (W197: auch fuer die VR-Textur) */
static void ph_layout_f(unsigned *w, unsigned *h)
{
    *w = (unsigned)S(196.0f);
    *h = (unsigned)(S(15.0f) + S(3.0f) + 3 * S(10.0f) + S(2.0f) + S(34.0f) + S(3.0f) +
                    (g_cfg.perf_gpass > 0 ? 7 : 5) * S(10.0f) + S(4.0f)); /* W146: +2 Zeilen "GPU Pass" */
    if (krnl_savelock_active()) {
        *h += (unsigned)S(10.0f); /* W190: Zeile "Spielstaende geschuetzt" (nur mit aktivem Schutz) */
    }
    if (ph_vr_paint) {
        *h += (unsigned)S(10.0f); /* W197: Zeile "VR" (nur in der Brille) */
    }
}

static int ph_gdi_size(unsigned w, unsigned h)
{
    BITMAPINFO bi;
    if (ph_dc && w == ph_bw && h == ph_bh) { return 1; }
    if (!ph_dc) {
        ph_dc = CreateCompatibleDC(NULL);
        if (!ph_dc) { return 0; }
    }
    if (ph_bmp) { SelectObject(ph_dc, ph_bmp_old); DeleteObject(ph_bmp); ph_bmp = NULL; }
    if (ph_font) { SelectObject(ph_dc, ph_font_old); DeleteObject(ph_font); ph_font = NULL; }
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = (LONG)w;
    bi.bmiHeader.biHeight = -(LONG)h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    ph_bmp = CreateDIBSection(ph_dc, &bi, DIB_RGB_COLORS, (void **)&ph_bits, NULL, 0);
    if (!ph_bmp || !ph_bits) { return 0; }
    ph_bmp_old = (HBITMAP)SelectObject(ph_dc, ph_bmp);
    ph_font_px = S(9.0f);
    ph_font = CreateFontW(-ph_font_px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                          CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    if (ph_font) { ph_font_old = (HFONT)SelectObject(ph_dc, ph_font); }
    SetBkMode(ph_dc, TRANSPARENT);
    ph_bw = w;
    ph_bh = h;
    return 1;
}

static void ph_paint(void)
{
    int W = (int)ph_bw, H = (int)ph_bh, b = S(1.0f), pad = S(5.0f), rowh = S(10.0f), head = S(15.0f);
    int y, ty, gx0, gx1, gy0, gy1, i;
    wchar_t t[128], kn[16];
    ph_stat_t s;
    COLORREF cf;
    static const wchar_t *const wm[3] = { L"Fenster", L"Randlos", L"Exklusiv" };
    unsigned rts;
    ph_stats(1.0, &s);
    ph_fill(0, 0, W, H, SB_UI_C_BG);
    ph_fill(0, 0, W, b, SB_UI_C_LINE);
    ph_fill(0, H - b, W, H, SB_UI_C_LINE);
    ph_fill(0, 0, b, H, SB_UI_C_LINE);
    ph_fill(W - b, 0, W, H, SB_UI_C_LINE);
    ph_fill(0, 0, S(7.0f), 2 * b + 1, SB_UI_C_TXT);
    ph_fill(0, 0, 2 * b + 1, S(7.0f), SB_UI_C_TXT);
    ph_fill(W - S(7.0f), H - 2 * b - 1, W, H, SB_UI_C_TXT);
    ph_fill(W - 2 * b - 1, H - S(7.0f), W, H, SB_UI_C_TXT);
    /* Kopf */
    ph_fill(b + S(2.0f), b + S(2.0f), W - b - S(2.0f), head, SB_UI_C_HEAD);
    ty = b + S(2.0f) + (head - b - S(2.0f) - ph_font_px) / 2 - S(0.5f);
    ph_text(pad + S(1.0f), ty, L"\u25A0 SYS-PERF", SB_UI_C_BG, 0);
    ph_key_name(kn, 16);
    _snwprintf_s(t, 128, _TRUNCATE, L"LEISTUNG %s", kn);
    ph_text(W - pad - S(1.0f), ty, t, SB_UI_C_BG, 1);
    y = head + S(3.0f);
    /* Zeile 1: FPS + Ticks */
    cf = (s.frames && s.ft_avg > s.target * 1.15) ? PH_C_WARN : SB_UI_C_TXT;
    _snwprintf_s(t, 128, _TRUNCATE, L"FPS %6.1f", s.fps);
    ph_text(pad, y, t, cf, 0);
    _snwprintf_s(t, 128, _TRUNCATE, L"Ticks/s %4.1f", s.tps);
    ph_text(W - pad, y, t, (s.ticks && s.tps < 29.0 && rq_in_mission()) ? PH_C_WARN : SB_UI_C_TXT, 1);
    y += rowh;
    _snwprintf_s(t, 128, _TRUNCATE, L"Bild %5.2f ms  %5.2f\u2013%5.2f", s.ft_avg, s.ft_min, s.ft_max);
    ph_text(pad, y, t, SB_UI_C_TXT, 0);
    y += rowh;
    _snwprintf_s(t, 128, _TRUNCATE, L"1%%-Low %5.1f FPS (%5.2f ms)", s.low1, s.p99);
    ph_text(pad, y, t, (s.frames && s.p99 > s.target * 1.5) ? PH_C_WARN : SB_UI_C_TXT, 0);
    y += rowh + S(2.0f);
    /* Graph: Bildzeit der letzten Bilder (1 px je Bild, rechts = neu),
     * Skala 0..33,3 ms, Linien bei 8,3 (120) und 16,7 ms (60). */
    gx0 = pad;
    gx1 = W - pad;
    gy0 = y;
    gy1 = y + S(34.0f);
    ph_fill(gx0, gy0, gx1, gy1, RGB(2, 14, 7));
    {
        double scale = (double)(gy1 - gy0) / 33.333;
        int y120 = gy1 - (int)(8.333 * scale + 0.5), y60 = gy1 - (int)(16.667 * scale + 0.5);
        int x;
        ph_fill(gx0, y120, gx1, y120 + 1, SB_UI_C_DIM);
        ph_fill(gx0, y60, gx1, y60 + 1, SB_UI_C_DIM);
        for (x = gx1 - 1, i = 0; x >= gx0 && (unsigned)i < ph_fn && (unsigned)i < PH_RING; x--, i++) {
            const ph_frame_t *f = &ph_fr[(ph_fn - 1u - (unsigned)i) % PH_RING];
            double ft = f->ft;
            int hgt = (int)(ft * scale + 0.5);
            COLORREF c = ft > s.target * 2.0 ? PH_C_WARN : (ft > s.target * 1.5 ? SB_UI_C_TXT : SB_UI_C_LINE);
            if (hgt > gy1 - gy0) { hgt = gy1 - gy0; }
            if (hgt < 1) { hgt = 1; }
            ph_fill(x, gy1 - hgt, x + 1, gy1, c);
        }
        ph_text(gx0 + S(1.0f), gy0, L"33", SB_UI_C_DIM, 0);
        ph_text(gx1 - S(1.0f), y60 - ph_font_px + S(1.0f), L"60", SB_UI_C_DIM, 1);
        ph_text(gx1 - S(1.0f), y120 - ph_font_px + S(1.0f), L"120", SB_UI_C_DIM, 1);
    }
    y = gy1 + S(3.0f);
    /* CPU je Tick */
    _snwprintf_s(t, 128, _TRUNCATE, L"CPU Spiel%5.1f Rend%5.1f Intp%4.1f", s.game, s.render, s.interp + s.iwait);
    ph_text(pad, y, t, SB_UI_C_TXT, 0);
    y += rowh;
    _snwprintf_s(t, 128, _TRUNCATE, L"Tick-Last %5.1f ms (max %5.1f)", s.busy, s.busy_max);
    ph_text(pad, y, t, (s.busy_max > 33.3 * 0.75) ? PH_C_WARN : SB_UI_C_TXT, 0);
    y += rowh;
    if (ph_gq_fail) { _snwprintf_s(t, 128, _TRUNCATE, L"GPU  nicht messbar"); }
    else { _snwprintf_s(t, 128, _TRUNCATE, L"GPU Zw%5.2f Tick%5.1f max%5.1f", s.gpu_z, s.gpu_t, s.gpu_max); }
    ph_text(pad, y, t, SB_UI_C_TXT, 0);
    y += rowh;
    _snwprintf_s(t, 128, _TRUNCATE, L"Draws/Tick %5.0f  Pres %4.2f skip %u", s.draws, s.pres_avg, s.skip);
    ph_text(pad, y, t, SB_UI_C_TXT, 0);
    y += rowh;
    if (g_cfg.perf_gpass > 0) { /* DIAG(GPU-COST,W146): GPU je Pass eines Zwischenbilds (ms, Mittel 1 s) */
        static LONGLONG gq;
        LONGLONG nq = ph_now();
        if (g_cfg.perf_log <= 0 && (nq - gq) > ph_qpf.QuadPart) { gq = nq; gp_window(NULL, 0); }
        _snwprintf_s(t, 128, _TRUNCATE, L"Pass Welt%4.1f/%3.1f RT%4.1f BB%4.1f/%3.1f",
                     gp_last[GP_WELT] + gp_last[GP_WELT_SH], gp_last[GP_WELT_SH], gp_last[GP_SUB] + gp_last[GP_SUB_SH],
                     gp_last[GP_BB] + gp_last[GP_BB_SH], gp_last[GP_BB_SH]);
        ph_text(pad, y, t, SB_UI_C_TXT, 0);
        y += rowh;
        _snwprintf_s(t, 128, _TRUNCATE, L"MFX%4.1f FX%4.1f Post%4.1f St%4.1f VR%4.1f",
                     gp_last[GP_MFX], gp_last[GP_FX], gp_last[GP_POST] + gp_last[GP_FXPOST] + gp_last[GP_BLIT] + gp_last[GP_HUD],
                     gp_last[GP_START], gp_last_vr);
        ph_text(pad, y, t, SB_UI_C_TXT, 0);
        y += rowh;
    }
    rts = sb_rtt_dyn_factor(); /* W146: aktueller Faktor (rt_scale_dyn) */
    {
        int wmi = sb_winmode_current();
        if (wmi < 0 || wmi > 2) { wmi = 0; }
        _snwprintf_s(t, 128, _TRUNCATE, L"%u\u00D7%u %s %d Sh%d RT%u%s", g_bb_w, g_bb_h, wm[wmi],
                     g_sb_paused ? sb_ovl_pause_fps() : (g_cfg.interp_fps > 0 ? g_cfg.interp_fps : 30), g_cfg.shading, rts,
                     g_cfg.rt_scale_dyn > 0 ? L"d" : L""); /* W146: d = dynamisch */
        { /* W191 UW-PERSP: Cockpit-Perspektive + CPU-Kosten (nur wenn an) */
            double cpms = 0.0;
            int cps = sb_cp_status(&cpms);
            if (cps) {
                size_t l = wcslen(t);
                if (cps < 0) { _snwprintf_s(t + l, 128 - l, _TRUNCATE, L" CPa %.2fms", cpms); }
                else { _snwprintf_s(t + l, 128 - l, _TRUNCATE, L" CP%d %.2fms", cps, cpms); }
            }
            if (sb_cp_width() > 1.0f) { /* W235 COCKPIT-WIDTH: Breite in % (GPU-Kosten im Panini-Pass, Segment HUD/Post) */
                size_t l = wcslen(t);
                _snwprintf_s(t + l, 128 - l, _TRUNCATE, L" CW%d", (int)(sb_cp_width() * 100.0f + 0.5f) - 100);
            }
        }
    }
    ph_text(pad, y, t, SB_UI_C_DIM, 0);
    if (krnl_savelock_active()) { /* W190 SAVE-LOCK: sichtbarer Hinweis */
        y += rowh;
        sb_lang_w("ovl.sl_note", t, 128);
        ph_text(pad, y, t, PH_C_WARN, 0);
    }
    if (ph_vr_paint) { /* W197 VR-MENU: VR-Bildrate und Augengroesse (nur auf der Flaeche in der Brille) */
        const char *vt = sb_vr_hud_text();
        y += rowh;
        if (!MultiByteToWideChar(CP_UTF8, 0, vt, -1, t, 128)) { t[0] = 0; }
        ph_text(pad, y, t, SB_UI_C_TXT, 0);
    }
    GdiFlush();
    sb_ovl_ui_finish(ph_bits, ph_bw, ph_bh, ph_f);
}

static int ph_tex_upload(void)
{
    D3D11_MAPPED_SUBRESOURCE map;
    unsigned y;
    if (!ph_tex || ph_tw != ph_bw || ph_th != ph_bh) {
        D3D11_TEXTURE2D_DESC td;
        if (ph_srv) { ID3D11ShaderResourceView_Release(ph_srv); ph_srv = NULL; }
        if (ph_tex) { ID3D11Texture2D_Release(ph_tex); ph_tex = NULL; }
        memset(&td, 0, sizeof td);
        td.Width = ph_bw;
        td.Height = ph_bh;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &ph_tex)) ||
            FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)ph_tex, NULL, &ph_srv))) {
            HLOG_W("PERFHUD: Panel-Textur %ux%u gescheitert", ph_bw, ph_bh);
            return 0;
        }
        ph_tw = ph_bw;
        ph_th = ph_bh;
    }
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)ph_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        return 0;
    }
    for (y = 0; y < ph_bh; y++) {
        memcpy((unsigned char *)map.pData + (size_t)y * map.RowPitch, ph_bits + (size_t)y * ph_bw, ph_bw * 4u);
    }
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)ph_tex, 0);
    return 1;
}

void sb_phud_draw(ID3D11RenderTargetView *rtv, int save)
{
    unsigned pw, ph, x0, y0, m;
    LONGLONG now;
    static unsigned lk[4];
    if (!ph_vis || !rtv || !g_ctx || !g_dev) { return; }
    ph_layout(g_bb_h, &pw, &ph);
    if (pw + 4u > g_bb_w || ph + 4u > g_bb_h) { return; }
    if (!ph_gdi_size(pw, ph)) { return; }
    now = ph_now();
    if (!ph_last_paint || now - ph_last_paint >= ph_qpf.QuadPart / 5 || ph_tw != pw || ph_th != ph) {
        ph_last_paint = now;
        ph_paint();
        if (!ph_tex_upload()) { return; }
    }
    m = (unsigned)S(8.0f);
    x0 = (ph_pos == 1 || ph_pos == 2) ? m : g_bb_w - pw - m;
    y0 = (ph_pos <= 1) ? m : g_bb_h - ph - m;
    if (lk[0] != x0 || lk[1] != y0 || lk[2] != pw || lk[3] != ph) {
        lk[0] = x0; lk[1] = y0; lk[2] = pw; lk[3] = ph;
        HLOG_I("PERFHUD panel %ux%u @%u,%u (bb %ux%u, Ecke %d, Faktor %.2f, Schrift %d px)", pw, ph, x0, y0, g_bb_w,
               g_bb_h, ph_pos, (double)ph_f, ph_font_px);
    }
    if (save) { sb_ovl_save_take(&ph_save, rtv, x0, y0, pw, ph); }
    sb_ovl_quad(rtv, ph_srv, x0, y0, pw, ph);
}

void sb_phud_restore(void)
{
    sb_ovl_save_put(&ph_save);
}

/* ---- W197 VR-MENU: Panel fuer die kopffeste Flaeche in der Brille ----
 * Zweiter GDI-Kontext (Tausch mit den Desktop-Globalen wie sb_overlay.c), Faktor aus
 * vr_menu_px (Bezugsbox 540 Einheiten), Neuzeichnen alle 200 ms wie am Desktop. VR-Thread,
 * unter ctx_lock (dieselbe Sperre wie sb_phud_draw). */
typedef struct {
    HDC dc;
    HBITMAP bmp, bmp_old;
    unsigned *bits;
    unsigned bw, bh;
    HFONT font, font_old;
    int font_px;
    float f;
} ph_gdi_t;
static ph_gdi_t ph_vrg = { NULL, NULL, NULL, NULL, 0, 0, NULL, NULL, 0, 1.0f };
static sb_ovl_vrtex_t ph_vrt;
static LONGLONG ph_vr_last;
static float ph_vr_f;

static void ph_gdi_swap(ph_gdi_t *o)
{
    HDC dc = ph_dc;
    HBITMAP b = ph_bmp, bo = ph_bmp_old;
    unsigned *bits = ph_bits;
    unsigned bw = ph_bw, bh = ph_bh;
    HFONT fo = ph_font, foo = ph_font_old;
    int fp = ph_font_px;
    float f = ph_f;
    ph_dc = o->dc; ph_bmp = o->bmp; ph_bmp_old = o->bmp_old; ph_bits = o->bits; ph_bw = o->bw; ph_bh = o->bh;
    ph_font = o->font; ph_font_old = o->font_old; ph_font_px = o->font_px; ph_f = o->f;
    o->dc = dc; o->bmp = b; o->bmp_old = bo; o->bits = bits; o->bw = bw; o->bh = bh;
    o->font = fo; o->font_old = foo; o->font_px = fp; o->f = f;
}

int sb_phud_vr_panel(ID3D11ShaderResourceView **srv, unsigned *tw, unsigned *th, float *uw, float *uh)
{
    LONGLONG now;
    if (!ph_init || !ph_vis || !g_dev || !g_ctx) { return 0; }
    now = ph_now();
    if (!ph_vrt.srv || !ph_vr_last || now - ph_vr_last >= ph_qpf.QuadPart / 5) {
        unsigned w, h;
        float f = (float)(g_cfg.vr_menu_px > 0 ? g_cfg.vr_menu_px : 1620) / 540.0f;
        if (f < 1.0f) { f = 1.0f; } else if (f > 8.0f) { f = 8.0f; }
        ph_gdi_swap(&ph_vrg);
        ph_f = f;
        ph_vr_paint = 1;
        ph_layout_f(&w, &h);
        if (ph_gdi_size(w, h)) {
            ph_paint();
            if (sb_ovl_vrtex_upload(&ph_vrt, ph_bits, ph_bw, ph_bh)) {
                if (ph_vr_f != f) {
                    HLOG_I("VR-MENU: Leistungs-Panel %ux%u Texel (Faktor %.2f, Schrift %d px)", ph_bw, ph_bh, (double)f, ph_font_px);
                }
                ph_vr_f = f;
            }
        }
        ph_vr_paint = 0;
        ph_gdi_swap(&ph_vrg);
        ph_vr_last = now;
    }
    if (!ph_vrt.srv || ph_vr_f <= 0.0f) { return 0; }
    *srv = ph_vrt.srv;
    *tw = ph_vrt.w;
    *th = ph_vrt.h;
    *uw = (float)ph_vrt.w / ph_vr_f;
    *uh = (float)ph_vrt.h / ph_vr_f;
    return 1;
}
