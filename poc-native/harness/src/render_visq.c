/* render_visq.c - FIX(SHADE-M2,W117) vis_query: NV2A-Sichtbarkeitstest.
 *
 * Das Spiel misst die Sonnenblendung per Sichtbarkeitstest (BERICHT W117):
 * FUN_0001D8F0 zeichnet zwischen D3DDevice_BeginVisibilityTest (0x20EA80)
 * und EndVisibilityTest(0x3FFF) (0x20EB20) EINEN Punkt (Begin(1) POINTLIST)
 * an der Sonnenposition mit Tiefentest; FUN_0001DA60 liest per
 * GetVisibilityTestResult(0x3FFF) (0x20E4C0) die Zahl der sichtbaren Pixel:
 * != 0 -> Blendung [0x356D58] aufbauen (Hauptpass-Umgebungslicht
 * [0x358250] = Basis + [0x3514D4]*[0x356D58], Blend-Quad FUN_00015AD0),
 * 0 -> abklingen. Gleiches fuer den zweiten Pass (0x3FFE, FUN_0001D9D0 /
 * FUN_00016A40) und Lichter (FUN_0001BB20/0001BF30).
 *
 * Bisher lieferte hook_D3D_QueryStatus immer 1 ("sichtbar") -> Sonne galt
 * auch verdeckt bzw. hinter der Kamera als sichtbar, der Hauptmonitor bekam
 * dauerhaft Blendung (+0,26 Umgebungslicht, M2/M9 weiss).
 *
 * Neu: Begin/End klammern die Draws dazwischen mit einer D3D11-Occlusion-
 * Query, das Ergebnis (Pixelzahl) geht je Index an GetVisibilityTestResult.
 * Unbekannter Index -> altes Verhalten (1).
 * vis_query_all=0 (Standard): nur die Sonnentests 0x3FFE/0x3FFF echt; die
 * uebrigen Indizes (Objekt-Occlusion-Culling FUN_00025580/00025840: Objekt
 * wird bei 0 nicht gezeichnet, Licht-Flares FUN_0001B900) bleiben "sichtbar"
 * wie bisher (aenderten sonst, welche Objekte gezeichnet werden).
 *
 * W304 FLARE-OCCL: vis_query_all=2 (Standard) wertet zusaetzlich die
 * Licht-Flares echt aus. FUN_00015470 vergibt je Licht-Slot s (0..127) die
 * Indizes 0x3F7E+s (Hauptpass: Test FUN_0001B900 @0x1B9DA, Lesen
 * FUN_0001BB20 @0x1BBF9) und 0x3EFE+s (zweiter Pass: FUN_0001BA10 @0x1BAEA,
 * FUN_0001BF30 @0x1C009) -> 0x3EFE..0x3FFD. Gelesen wird einmal je neuem Licht
 * im selben Pass: Ergebnis 0 -> Slot aus ([esi] = 0, Flare nie gezeichnet),
 * sonst Bildposition + Flare. Einziger Index-Vergeber-Aufruf: FUN_000459B0
 * @0x45BEF = Sonnen-Flare an [0x358380] (Sonnenposition, FUN_0001D8F0);
 * Lichter aus FUN_000152E0 (Explosionen/Feuer) haben Index 0 = kein Test,
 * ihr Gluehen zeichnet FUN_0001BB20 mit ZEnable(0) - auch im Original ueber
 * allem. Mit "immer sichtbar" kam das Sonnen-Flare auch verdeckt. Das Objekt-Culling (FUN_00025790/00025A60, Index =
 * Objektzaehler ab 0, gelesen ueber FUN_00016A10) bleibt "sichtbar". */
#include "render_int.h"
#include "sb_perfhud.h" /* W125 TICK-PERF */
#include <d3d11_1.h>

#define VISQ_POOL 256u
#define VISQ_NIDX 0x4000u               /* Xbox-Report-Indizes 0..0x3FFF */
/* Index -> Zustand: 0 unbekannt (altes Verhalten), 1 = Test ohne Draw
 * (Ergebnis 0, kein Query-Objekt), 2 = beendete Query. Das Spiel beendet
 * beim Missionsstart alle 16384 Indizes einmal ohne Draw (W117_f). */
static unsigned char g_vq_st[VISQ_NIDX];
static ID3D11Query *g_vq_q[VISQ_NIDX];
static ID3D11Query *g_vq_free[VISQ_POOL];
static unsigned g_vq_nfree;
static ID3D11Query *g_vq_cur;           /* zwischen Begin und End */
static unsigned g_vq_ndraw;             /* Draws seit Begin */
static unsigned g_vq_ncalls, g_vq_nlog, g_vq_timeouts;

/* W304 FLARE-OCCL: Indizes mit echtem Ergebnis (s. Kopf). */
#define VISQ_FLARE_LO 0x3EFEu
static int visq_real_idx(unsigned index)
{
    return index >= 0x3FFEu || g_cfg.vis_query_all == 1 || (g_cfg.vis_query_all == 2 && index >= VISQ_FLARE_LO);
}
static int visq_flare_idx(unsigned index) { return index >= VISQ_FLARE_LO && index < 0x3FFEu; }
/* Zaehlung je Flare-Ergebnis (Zeile VISQ flare): Tests, sichtbar, verdeckt,
 * ohne Draw, Wartezeit auf die GPU (QPC). */
static unsigned g_vqf_tests, g_vqf_vis, g_vqf_occ, g_vqf_nodraw, g_vqf_frames, g_vqf_lastf, g_vqf_nlog;
static long long g_vqf_wait, g_vqf_wmax, g_vqf_qpf;
static void visq_flare_stat(unsigned index, unsigned n, int nodraw, long long wait)
{
    g_vqf_tests++;
    if (nodraw) { g_vqf_nodraw++; }
    if (n) { g_vqf_vis++; } else { g_vqf_occ++; }
    g_vqf_wait += wait;
    if (wait > g_vqf_wmax) { g_vqf_wmax = wait; }
    if (g_vqf_lastf != g_present_no) { g_vqf_lastf = g_present_no; g_vqf_frames++; }
    if (!g_vqf_qpf) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_vqf_qpf = f.QuadPart ? f.QuadPart : 1; }
    if (g_cfg.vis_diag && g_vqf_nlog < 400u) {
        g_vqf_nlog++;
        HLOG_W("VISQ flare1 f=%u idx=0x%X slot=%u pass=%d n=%u ohne_draw=%d warte_us=%.1f", g_present_no, index,
               (index - VISQ_FLARE_LO) & 0x7Fu, index >= 0x3F7Eu ? 1 : 2, n, nodraw,
               (double)wait * 1e6 / (double)g_vqf_qpf);
    }
    if ((g_vqf_tests % 500u) == 1u || (g_cfg.vis_diag && (g_vqf_tests % 50u) == 0u)) {
        HLOG_I("VISQ flare f=%u tests=%u sichtbar=%u verdeckt=%u ohne_draw=%u bilder=%u warte_us=%.1f (je Test %.2f, max %.1f)",
               g_present_no, g_vqf_tests, g_vqf_vis, g_vqf_occ, g_vqf_nodraw, g_vqf_frames,
               (double)g_vqf_wait * 1e6 / (double)g_vqf_qpf, (double)g_vqf_wait * 1e6 / (double)g_vqf_qpf / (double)g_vqf_tests,
               (double)g_vqf_wmax * 1e6 / (double)g_vqf_qpf);
    }
}

int sb_visq_active(void)
{
    return g_cfg.vis_query == 2 || (g_cfg.vis_query == 1 && g_cfg.shading) ||
           (g_sba_loc && g_cfg.loc_visq == 1); /* W297: LoC-Mission (Sichtbarkeit der Objekte) */
}

static ID3D11Query *visq_take(void)
{
    D3D11_QUERY_DESC qd;
    ID3D11Query *q = NULL;
    if (g_vq_nfree) { return g_vq_free[--g_vq_nfree]; }
    if (!g_dev) { return NULL; }
    qd.Query = D3D11_QUERY_OCCLUSION;
    qd.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &q))) { return NULL; }
    return q;
}

static void visq_give(ID3D11Query *q)
{
    if (!q) { return; }
    if (g_vq_nfree < VISQ_POOL) { g_vq_free[g_vq_nfree++] = q; }
    else { ID3D11Query_Release(q); }
}

void sb_visq_draw_note(void) { if (g_vq_cur) { g_vq_ndraw++; } }

/* W304 FLARE-OCCL vis_ztest: Punkt-Draws zwischen Begin/EndVisibilityTest
 * (Sonne FUN_0001D8F0, Flares FUN_0001B900/0001BA10) laufen als P2-Pixel ins
 * Welt-RT (rhw=1); dafuer war der Tiefentest bisher immer aus ("RHW: kein
 * Depth-Test") -> verdeckte Lichter zaehlten als sichtbar. Die NV2A testet
 * auch Bildschirm-Vertices gegen den Z-Puffer (ZENABLE/ZFUNC des Spiels). */
int sb_visq_ztest(void)
{
    extern unsigned m3d_zenable;
    return g_vq_cur && g_cfg.vis_ztest && m3d_zenable;
}
/* W313 P2-ZTEST: Draw liegt zwischen Begin/EndVisibilityTest (Sonne, Flares,
 * Objekt-Occlusion-Vorpass FUN_00025790) -> dort gilt allein vis_ztest. */
int sb_visq_in_test(void) { return g_vq_cur != NULL; }
static char g_vq_dd[700]; /* DIAG(W304): letzter Punkt-Draw im Test */
static int g_vq_onscr; /* DIAG(W304): Punkt liegt im Ziel */
float g_vqd_raw[4], g_vqd_mat[4][4]; int g_vqd_em, g_vqd_rowe; unsigned g_vqd_vs; /* DIAG(W304), render_im.c */

/* DIAG(FLARE-OCCL,W304, vis_diag=1): Draw im Sichtbarkeitstest - erster
 * Vertex (Position nach Umwandlung), rhw, Ziel, Tiefen-/Alpha-Zustand. */
void sb_visq_draw_diag(const im_vertex_t *v, unsigned n, int rhw, unsigned prim, unsigned tex)
{
    extern unsigned rs_dmask, rs_cmask;
    D3D11_VIEWPORT vp; UINT nvp = 1;
    if (!g_vq_cur || !v || !n || prim != 1u) { return; } /* nur Punkte (Sonne, Flares); Zeile bei End */
    memset(&vp, 0, sizeof vp);
    if (g_ctx) { ID3D11DeviceContext_RSGetViewports(g_ctx, &nvp, &vp); }
    g_vq_onscr = v[0].pos[0] >= vp.TopLeftX && v[0].pos[0] < vp.TopLeftX + vp.Width &&
                 v[0].pos[1] >= vp.TopLeftY && v[0].pos[1] < vp.TopLeftY + vp.Height && v[0].pos[2] >= 0.0f && v[0].pos[2] <= 1.0f;
    _snprintf_s(g_vq_dd, sizeof g_vq_dd, _TRUNCATE, "prim=%u n=%u rhw=%d zt=%d pos=(%.3f %.3f %.6f %.4f) col=(%.2f %.2f %.2f %.2f) tex=0x%X rt=%d %ux%u vp=(%.0f %.0f %.0f %.0f z %.2f..%.2f) zf=0x%X zen=0x%X dmask=%u cmask=0x%X at=%u/0x%X/%u",
           prim, n, rhw, sb_visq_ztest(), v[0].pos[0], v[0].pos[1], v[0].pos[2], v[0].pos[3],
           v[0].col[0], v[0].col[1], v[0].col[2], v[0].col[3], tex,
           rtt_active() ? g_rtt_bound : -1, rtt_active() ? g_rtt[g_rtt_bound].w : 0u, rtt_active() ? g_rtt[g_rtt_bound].h : 0u,
           vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height, vp.MinDepth, vp.MaxDepth,
           fx_nv(0x354u, 0x201u), fx_nv(0x30Cu, 0xFFFFu), rs_dmask, rs_cmask,
           fx_nv(0x300u, 0u), fx_nv(0x33Cu, 0x207u), fx_nv(0x340u, 0u));
    {
        size_t l = strlen(g_vq_dd);
        _snprintf_s(g_vq_dd + l, sizeof g_vq_dd - l, _TRUNCATE, " vs=0x%X em=%d rowe=%d raw=(%.2f %.2f %.2f %.2f) m0=(%.3f %.3f %.3f %.3f) m3=(%.3f %.3f %.3f %.3f)",
                  g_vqd_vs, g_vqd_em, g_vqd_rowe, g_vqd_raw[0], g_vqd_raw[1], g_vqd_raw[2], g_vqd_raw[3],
                  g_vqd_mat[0][0], g_vqd_mat[0][1], g_vqd_mat[0][2], g_vqd_mat[0][3],
                  g_vqd_mat[3][0], g_vqd_mat[3][1], g_vqd_mat[3][2], g_vqd_mat[3][3]);
    }
}

/* PERF(SHADER-OPT2,W131) vis_query_async=1: Sonnentests (0x3FFE/0x3FFF) ohne
 * Warten. Je Index ein kleiner Ring laufender Queries (neueste zuletzt).
 * GetVisibilityTestResult nimmt das neueste FERTIGE Ergebnis (GetData mit
 * DONOTFLUSH, aeltere werden verworfen); ist keines fertig, gilt das letzte
 * bekannte Ergebnis (1 Bild Latenz der Blendung). Gibt es noch keines (erstes
 * Ergebnis), wird wie bisher blockierend gewartet.
 * W304 FLARE-OCCL: Die Flare-Tests laufen je Pass ebenso ueber einen Ring
 * (Slot 2 = Hauptpass 0x3F7E.., Slot 3 = zweiter Pass 0x3EFE..). Belegt:
 * Test-Indizes vergibt nur FUN_00015470, und die hat genau einen Aufrufer
 * (FUN_000459B0 @0x45BEF, je Bild ein Licht an der festen Position
 * [0x358380]) - jeder Flare-Test eines Passes misst also dasselbe Licht wie
 * im Bild davor (wie der Sonnentest). Ohne Ring wartete jedes Bild mitten im
 * Pass auf die GPU (W304: 0,05-1,3 ms je Test im Testtakt). Nach einer Pause
 * (> 2 Bilder ohne Flare-Test) wird das erste Ergebnis wieder blockierend
 * geholt, damit kein Ergebnis aus einer alten Szene gilt. */
#define VQA_N 4u
#define VQA_S 4u
static ID3D11Query *g_vqa_q[VQA_S][VQA_N];
static unsigned g_vqa_n[VQA_S];
static unsigned g_vqa_last[VQA_S];
static int g_vqa_have[VQA_S];
static unsigned g_vqa_endf[VQA_S]; /* W304: Bild des letzten Tests (Flare-Pause) */
static unsigned g_vqa_fresh, g_vqa_stale, g_vqa_block, g_vqa_drop, g_vqa_calls;

static int visq_async_idx(unsigned index)
{
    return g_cfg.vis_query_async && index < VISQ_NIDX && (index >= 0x3FFEu || visq_flare_idx(index));
}
static unsigned visq_async_slot(unsigned index) /* W304: 0/1 Sonne, 2/3 Flares */
{
    return index >= 0x3FFEu ? (index & 1u) : (index >= 0x3F7Eu ? 2u : 3u);
}

static void vqa_drop_older(unsigned s, unsigned keep_from)
{
    unsigned j, k = 0;
    for (j = 0; j < g_vqa_n[s]; j++) {
        if (j < keep_from) { visq_give(g_vqa_q[s][j]); g_vqa_drop++; }
        else { g_vqa_q[s][k++] = g_vqa_q[s][j]; }
    }
    g_vqa_n[s] = k;
}

/* DIAG(TICK-PERF,W125): Abstand End -> Get je Index (perf_blk, Sonnentests) */
static long long g_vq_endq[2];
static unsigned g_vq_endf[2];
long long g_vq_e2g_q; unsigned g_vq_e2g_n, g_vq_e2g_swap;

void sb_visq_begin(void)
{
    if (!g_ctx) { return; }
    sb_ctx_enter();
    if (g_vq_cur) { /* Begin ohne End: alte Messung verwerfen */
        ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)g_vq_cur);
        visq_give(g_vq_cur);
        g_vq_cur = NULL;
    }
    g_vq_cur = visq_take();
    g_vq_ndraw = 0;
    if (g_vq_cur) { ID3D11DeviceContext_Begin(g_ctx, (ID3D11Asynchronous *)g_vq_cur); }
    sb_ctx_leave();
}

void sb_visq_end(unsigned index)
{
    unsigned ix = index & (VISQ_NIDX - 1u);
    if (!g_ctx || !g_vq_cur) { return; }
    sb_ctx_enter();
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)g_vq_cur);
    if (g_vq_q[ix]) { visq_give(g_vq_q[ix]); g_vq_q[ix] = NULL; }
    if (!visq_real_idx(index)) { /* nur Sonnentests (+ W304 Flares) echt: Rest wie bisher ("sichtbar") */
        visq_give(g_vq_cur);
        g_vq_st[ix] = 0;
    } else if (visq_async_idx(index)) { /* W131 vis_query_async: in den Ring statt ersetzen */
        unsigned sidx = visq_async_slot(index);
        if (sidx >= 2u && g_present_no - g_vqa_endf[sidx] > 2u) { /* W304: Flare-Pause -> neu blockierend */
            vqa_drop_older(sidx, g_vqa_n[sidx]);
            g_vqa_have[sidx] = 0;
        }
        g_vqa_endf[sidx] = g_present_no;
        if (g_vq_ndraw == 0u) { /* nichts gezeichnet: neuester Stand = 0 */
            visq_give(g_vq_cur);
            vqa_drop_older(sidx, g_vqa_n[sidx]);
            g_vqa_last[sidx] = 0; g_vqa_have[sidx] = 1;
            g_vq_st[ix] = 1;
        } else {
            if (g_vqa_n[sidx] >= VQA_N) { vqa_drop_older(sidx, 1u); }
            g_vqa_q[sidx][g_vqa_n[sidx]++] = g_vq_cur;
            g_vq_st[ix] = 2;
        }
    } else if (g_vq_ndraw == 0u) { /* nichts gezeichnet: Ergebnis 0 ohne Query */
        visq_give(g_vq_cur);
        g_vq_st[ix] = 1;
    } else {
        g_vq_q[ix] = g_vq_cur;
        g_vq_st[ix] = 2;
    }
    if (g_cfg.vis_diag && (g_vq_nlog < 64u || (index >= 0x3FFEu && (g_present_no % 300u) == 0u))) {
        HLOG_W("VISQ end f=%u idx=0x%X draws=%u rt=%d %ux%u", g_present_no, index, g_vq_ndraw,
               rtt_active() ? g_rtt_bound : -1, rtt_active() ? g_rtt[g_rtt_bound].w : 0u, rtt_active() ? g_rtt[g_rtt_bound].h : 0u);
    }
    if (g_cfg.vis_diag && visq_flare_idx(index)) { /* DIAG(FLARE-OCCL,W304) */
        static unsigned nfe, nfin;
        if (g_vq_onscr) { nfin++; }
        if (nfe < 200u || (g_vq_onscr && (nfin < 600u || (g_present_no % 100u) == 0u)) || (g_present_no % 600u) == 0u) {
            nfe++;
            HLOG_W("VISQ fend f=%u idx=0x%X draws=%u im_bild=%d (%u) %s", g_present_no, index, g_vq_ndraw, g_vq_onscr, nfin, g_vq_dd);
        }
    }
    g_vq_dd[0] = 0; g_vq_onscr = 0;
    g_vq_cur = NULL;
    if (g_tb_on && index >= 0x3FFEu) { g_vq_endq[index & 1u] = tb_now(); g_vq_endf[index & 1u] = g_present_no; } /* DIAG(TICK-PERF,W125) */
    sb_ctx_leave();
}

/* 1 = Index bekannt, *out = sichtbare Pixel; 0 = unbekannt (altes Verhalten). */
int sb_visq_result(unsigned index, unsigned *out)
{
    unsigned ix = index & (VISQ_NIDX - 1u);
    UINT64 n = 0;
    HRESULT hr = S_OK;
    ID3D11Query *q = g_vq_q[ix];
    if (index >= VISQ_NIDX || !g_vq_st[ix] || !g_ctx) { return 0; }
    if (g_vq_st[ix] == 2u && visq_async_idx(index)) { /* W131 vis_query_async */
        unsigned sidx = visq_async_slot(index), j;
        int got = 0;
        LARGE_INTEGER aq0; /* W304: Wartezeit (nur blockierendes erstes Ergebnis) */
        QueryPerformanceCounter(&aq0);
        g_vqa_calls++;
        sb_ctx_enter();
        for (j = g_vqa_n[sidx]; j-- > 0u;) {
            UINT64 m = 0;
            if (ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)g_vqa_q[sidx][j], &m, sizeof m,
                                            D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) {
                g_vqa_last[sidx] = m > 0xFFFFFFFFull ? 0xFFFFFFFFu : (unsigned)m;
                g_vqa_have[sidx] = 1;
                vqa_drop_older(sidx, j + 1u); /* diese und aeltere verwerfen */
                got = 1;
                break;
            }
        }
        if (!got && !g_vqa_have[sidx] && g_vqa_n[sidx]) { /* erstes Ergebnis: blockierend wie bisher */
            DWORD t0 = GetTickCount();
            long long tbq = TB_BEGIN();
            ID3D11Query *nq = g_vqa_q[sidx][g_vqa_n[sidx] - 1u];
            UINT64 m = 1;
            for (;;) {
                hr = ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)nq, &m, sizeof m, 0);
                if (hr != S_FALSE) { break; }
                if (GetTickCount() - t0 > 250u) { g_vq_timeouts++; break; }
                YieldProcessor();
            }
            TB_END(TB_VISQ, tbq);
            g_vqa_last[sidx] = hr == S_OK ? (m > 0xFFFFFFFFull ? 0xFFFFFFFFu : (unsigned)m) : 1u;
            g_vqa_have[sidx] = 1;
            vqa_drop_older(sidx, g_vqa_n[sidx]);
            g_vqa_block++;
        } else if (got) {
            g_vqa_fresh++;
        } else {
            g_vqa_stale++;
        }
        sb_ctx_leave();
        n = g_vqa_have[sidx] ? g_vqa_last[sidx] : 1u;
        hr = S_OK;
        if (sidx >= 2u) { /* W304 */
            LARGE_INTEGER aq1;
            QueryPerformanceCounter(&aq1);
            visq_flare_stat(index, n ? 1u : 0u, 0, aq1.QuadPart - aq0.QuadPart);
        }
        if (g_cfg.vis_diag || (g_vqa_calls % 3600u) == 1u) {
            static unsigned nl;
            if (nl < 40u || (g_vqa_calls % 3600u) == 1u) {
                nl++;
                HLOG_I("VISQ async f=%u idx=0x%X n=%u frisch=%u alt=%u blockierend=%u verworfen=%u ring=%u",
                       g_present_no, index, (unsigned)n, g_vqa_fresh, g_vqa_stale, g_vqa_block, g_vqa_drop,
                       g_vqa_n[sidx]);
            }
        }
        goto done;
    }
    if (g_vq_st[ix] == 1u || !q) {
        n = 0;
        if (visq_flare_idx(index)) { visq_flare_stat(index, 0u, 1, 0); } /* W304 */
        goto done;
    }
    hr = S_FALSE;
    sb_ctx_enter();
    {
        DWORD t0 = GetTickCount();
        LARGE_INTEGER fq0; /* W304: Wartezeit der Flare-Tests */
        QueryPerformanceCounter(&fq0);
        long long tbq = TB_BEGIN(); /* DIAG(TICK-PERF,W125) */
        if (g_tb_on && index >= 0x3FFEu && g_vq_endq[index & 1u]) {
            g_vq_e2g_q += tbq - g_vq_endq[index & 1u]; g_vq_e2g_n++;
            if (g_vq_endf[index & 1u] != g_present_no) { g_vq_e2g_swap++; }
            g_vq_endq[index & 1u] = 0;
        }
        for (;;) {
            hr = ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)q, &n, sizeof n, 0);
            if (hr != S_FALSE) { break; }
            if (GetTickCount() - t0 > 250u) { g_vq_timeouts++; break; } /* GPU haengt: wie bisher "sichtbar" */
            YieldProcessor();
            sb_ctx_yield(); /* W138 present_decouple: Present-Takt nicht fuer die GPU-Wartezeit sperren */
        }
        TB_END(TB_VISQ, tbq); /* DIAG(TICK-PERF,W125) */
        if (visq_flare_idx(index)) { /* W304 */
            LARGE_INTEGER fq1;
            QueryPerformanceCounter(&fq1);
            visq_flare_stat(index, hr == S_OK ? (n ? 1u : 0u) : 1u, 0, fq1.QuadPart - fq0.QuadPart);
        }
    }
    sb_ctx_leave();
    if (hr != S_OK) { n = 1; }
done:
    *out = n > 0xFFFFFFFFull ? 0xFFFFFFFFu : (unsigned)n;
    g_vq_ncalls++;
    if (g_cfg.vis_diag && index >= 0x3FFEu) { /* Sonne: Ergebnis + Blendung (Hauptpass 0x3FFF, zweiter Pass 0x3FFE) */
        static unsigned nsun[2], lastf[2];
        unsigned s = index & 1u;
        if (nsun[s] < 120u || g_present_no - lastf[s] >= 300u) {
            float fl[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
            /* W337: + Sonnen-Elevation/-Azimut (FUN_0001D8F0) */
            static const unsigned ad[8] = { 0x356D58u, 0x351684u, 0x357DCCu, 0x357DD0u, 0x311590u, 0x31158Cu, 0x352110u, 0x352168u };
            unsigned q;
            for (q = 0; q < 8u; q++) { __try { fl[q] = *(volatile float *)(uintptr_t)SBA(ad[q]); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
            nsun[s]++; lastf[s] = g_present_no;
            HLOG_W("VISQ sun f=%u idx=0x%X n=%u blend356D58=%.4f blend351684=%.4f pos357DCC=(%.4f,%.4f) a311590=%.4f b31158C=%.4f elev=%.4f azim=%.4f",
                   g_present_no, index, *out, fl[0], fl[1], fl[2], fl[3], fl[4], fl[5], fl[6], fl[7]);
        }
    }
    if (g_cfg.vis_diag && (g_vq_nlog < 64u || (g_vq_ncalls % 2000u) == 0u)) {
        g_vq_nlog++;
        HLOG_W("VISQ get f=%u idx=0x%X n=%u hr=0x%lX calls=%u timeouts=%u", g_present_no, index, *out,
               (long)hr, g_vq_ncalls, g_vq_timeouts);
    }
    return 1;
}
