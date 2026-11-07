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
 * wie bisher (aenderten sonst, welche Objekte gezeichnet werden). */
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

int sb_visq_active(void)
{
    return g_cfg.vis_query == 2 || (g_cfg.vis_query == 1 && g_cfg.shading);
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

/* PERF(SHADER-OPT2,W131) vis_query_async=1: Sonnentests (0x3FFE/0x3FFF) ohne
 * Warten. Je Index ein kleiner Ring laufender Queries (neueste zuletzt).
 * GetVisibilityTestResult nimmt das neueste FERTIGE Ergebnis (GetData mit
 * DONOTFLUSH, aeltere werden verworfen); ist keines fertig, gilt das letzte
 * bekannte Ergebnis (1 Bild Latenz der Blendung). Gibt es noch keines (erstes
 * Ergebnis), wird wie bisher blockierend gewartet. */
#define VQA_N 4u
static ID3D11Query *g_vqa_q[2][VQA_N];
static unsigned g_vqa_n[2];
static unsigned g_vqa_last[2];
static int g_vqa_have[2];
static unsigned g_vqa_fresh, g_vqa_stale, g_vqa_block, g_vqa_drop, g_vqa_calls;

static int visq_async_idx(unsigned index)
{
    return g_cfg.vis_query_async && index >= 0x3FFEu && index < VISQ_NIDX;
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
    if (!g_cfg.vis_query_all && index < 0x3FFEu) { /* nur Sonnentests echt: Rest wie bisher ("sichtbar") */
        visq_give(g_vq_cur);
        g_vq_st[ix] = 0;
    } else if (visq_async_idx(index)) { /* W131 vis_query_async: in den Ring statt ersetzen */
        unsigned sidx = index & 1u;
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
        unsigned sidx = index & 1u, j;
        int got = 0;
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
    if (g_vq_st[ix] == 1u || !q) { n = 0; goto done; }
    hr = S_FALSE;
    sb_ctx_enter();
    {
        DWORD t0 = GetTickCount();
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
            float fl[6] = { 0, 0, 0, 0, 0, 0 };
            static const unsigned ad[6] = { 0x356D58u, 0x351684u, 0x357DCCu, 0x357DD0u, 0x311590u, 0x31158Cu };
            unsigned q;
            for (q = 0; q < 6u; q++) { __try { fl[q] = *(volatile float *)(uintptr_t)ad[q]; } __except (EXCEPTION_EXECUTE_HANDLER) {} }
            nsun[s]++; lastf[s] = g_present_no;
            HLOG_W("VISQ sun f=%u idx=0x%X n=%u blend356D58=%.4f blend351684=%.4f pos357DCC=(%.4f,%.4f) a311590=%.4f b31158C=%.4f",
                   g_present_no, index, *out, fl[0], fl[1], fl[2], fl[3], fl[4], fl[5]);
        }
    }
    if (g_cfg.vis_diag && (g_vq_nlog < 64u || (g_vq_ncalls % 2000u) == 0u)) {
        g_vq_nlog++;
        HLOG_W("VISQ get f=%u idx=0x%X n=%u hr=0x%lX calls=%u timeouts=%u", g_present_no, index, *out,
               (long)hr, g_vq_ncalls, g_vq_timeouts);
    }
    return 1;
}
