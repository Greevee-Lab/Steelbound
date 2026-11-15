/* render_dsc.h - W253 DRAW-OPT: Zustands-Cache des Immediate-Contexts.
 *
 * Alle Module des Renderers binden render_int.h ein; dort wird dieser Header
 * nach <d3d11.h>/<d3d11_1.h> geladen. Er ersetzt die COBJMACROS der
 * zustandssetzenden Context-Methoden durch duenne Inline-Huellen:
 *  - Jeder Aufruf auf g_ctx aktualisiert einen Schatten der gebundenen
 *    Objekte (IA, VS/PS-Shader, Konstantenpuffer, SRVs, Sampler, Blend/
 *    Tiefe/Raster) - immer, auch mit Schalter aus (Live-Umschaltung sicher).
 *  - draw_state_cache=1: Aufrufe, die exakt den schon gebundenen Zustand
 *    setzen, entfallen (redundante Set*-Aufrufe je Draw). Der Context haelt
 *    die gebundenen Objekte selbst fest, ein Zeigervergleich ist daher
 *    eindeutig.
 *  - Gefahrenstellen der Laufzeit (eine als Renderziel gebundene Ressource
 *    wird aus allen SRV-Plaetzen entfernt): OMSetRenderTargets* verwirft den
 *    SRV-Schatten; ExecuteCommandList ohne Wiederherstellung und ClearState
 *    verwerfen alles; *SetConstantBuffers1 auf einem anderen Zeiger verwirft
 *    die Konstantenpuffer-Plaetze der Stufe.
 *  - Andere Contexte (Deferred-Contexte des Render-Threads, VR-Device)
 *    werden unveraendert durchgereicht.
 * Zaehler (immer an): weitergereichte/entfallene Set*-Aufrufe, Map, Draws -
 * je Tick in den D-Zeilen von perf_trace (draw_tick_diag=1). */
#ifndef RENDER_DSC_H
#define RENDER_DSC_H

#define DSC_VB 4  /* IA-Vertexpuffer-Plaetze im Schatten */
#define DSC_CB 4  /* VS/PS-Konstantenpuffer-Plaetze */
#define DSC_SRV 8 /* VS/PS-SRV-Plaetze */
#define DSC_SMP 8 /* VS/PS-Sampler-Plaetze */

/* W264 DRAW-OPT2: DSC_N_IDX = Draws ueber den Strip-Indexpuffer, DSC_N_STRIP = Strip/Fan/Quad-Draws des
 * schnellen Wegs, DSC_N_VBV = dort hochgeladene Vertices (E-Zeile, draw_tick_diag=1) */
enum { DSC_N_FWD, DSC_N_SKIP, DSC_N_MAP, DSC_N_DRAW, DSC_N_CBMAP, DSC_N_CBREUSE, DSC_N_IDX, DSC_N_STRIP, DSC_N_VBV, DSC_N_MAX };

typedef struct {
    /* gueltig-Bits: 0 = unbekannt (naechster Aufruf wird weitergereicht) */
    unsigned v_lay, v_topo, v_vs, v_ps, v_bl, v_ds, v_rs, v_ib;
    unsigned v_vb, v_vcb, v_pcb, v_vsrv, v_psrv, v_vsmp, v_psmp; /* Bitmasken je Platz */
    ID3D11InputLayout *lay;
    D3D11_PRIMITIVE_TOPOLOGY topo;
    ID3D11Buffer *ib; DXGI_FORMAT ibf; UINT ibo; /* W264 DRAW-OPT2: Indexpuffer */
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11Buffer *vb[DSC_VB]; UINT vbs[DSC_VB], vbo[DSC_VB];
    ID3D11Buffer *vcb[DSC_CB], *pcb[DSC_CB];
    UINT vcbf[DSC_CB], vcbn[DSC_CB], pcbf[DSC_CB], pcbn[DSC_CB]; /* 0xFFFFFFFF = ohne Versatz */
    ID3D11ShaderResourceView *vsrv[DSC_SRV], *psrv[DSC_SRV];
    ID3D11SamplerState *vsmp[DSC_SMP], *psmp[DSC_SMP];
    ID3D11BlendState *bl; FLOAT blf[4]; int blfnull; UINT blm;
    ID3D11DepthStencilState *ds; UINT dsr;
    ID3D11RasterizerState *rs;
    unsigned cnt[DSC_N_MAX];
    /* im_cb_ring (render_im.c): Ringpuffer, D3D11.1-Zeiger und zuletzt per
     * Versatz gebundene Fenster b0 (VS, PS). Stellt fremder Code eine
     * gesicherte Ring-Bindung ohne Versatz wieder her (ip_state_restore u. a.:
     * *GetConstantBuffers liefert keinen Versatz), bindet die Huelle das
     * zuletzt gebundene Fenster nach - sonst laese der naechste Draw Platz 0. */
    ID3D11DeviceContext1 *c1;
    ID3D11Buffer *ring;
    UINT ringf[2];
} sb_dsc_t;

extern sb_dsc_t g_dsc;
extern ID3D11DeviceContext *g_ctx;
void sb_dsc_reset(void); /* alles unbekannt (Device neu, Diagnose) */

#define DSC_ON() (g_cfg.draw_state_cache != 0)
#define DSC_ME(c) ((void *)(c) == (void *)g_ctx && g_ctx)

static __inline void dsc_inval_srv(void) { g_dsc.v_vsrv = 0; g_dsc.v_psrv = 0; }
static __inline void dsc_inval_all(void)
{
    g_dsc.v_lay = g_dsc.v_topo = g_dsc.v_vs = g_dsc.v_ps = g_dsc.v_bl = g_dsc.v_ds = g_dsc.v_rs = g_dsc.v_ib = 0;
    g_dsc.v_vb = g_dsc.v_vcb = g_dsc.v_pcb = g_dsc.v_vsrv = g_dsc.v_psrv = g_dsc.v_vsmp = g_dsc.v_psmp = 0;
}

/* ---- IA ---- */
static __inline void dsc_IASetInputLayout(ID3D11DeviceContext *c, ID3D11InputLayout *l)
{
    if (DSC_ME(c)) {
        if (g_dsc.v_lay && g_dsc.lay == l && DSC_ON()) { g_dsc.cnt[DSC_N_SKIP]++; return; }
        g_dsc.lay = l; g_dsc.v_lay = 1; g_dsc.cnt[DSC_N_FWD]++;
    }
    c->lpVtbl->IASetInputLayout(c, l);
}
static __inline void dsc_IASetPrimitiveTopology(ID3D11DeviceContext *c, D3D11_PRIMITIVE_TOPOLOGY t)
{
    if (DSC_ME(c)) {
        if (g_dsc.v_topo && g_dsc.topo == t && DSC_ON()) { g_dsc.cnt[DSC_N_SKIP]++; return; }
        g_dsc.topo = t; g_dsc.v_topo = 1; g_dsc.cnt[DSC_N_FWD]++;
    }
    c->lpVtbl->IASetPrimitiveTopology(c, t);
}
static __inline void dsc_IASetIndexBuffer(ID3D11DeviceContext *c, ID3D11Buffer *b, DXGI_FORMAT f, UINT o)
{
    if (DSC_ME(c)) {
        if (g_dsc.v_ib && g_dsc.ib == b && g_dsc.ibf == f && g_dsc.ibo == o && DSC_ON()) { g_dsc.cnt[DSC_N_SKIP]++; return; }
        g_dsc.ib = b; g_dsc.ibf = f; g_dsc.ibo = o; g_dsc.v_ib = 1; g_dsc.cnt[DSC_N_FWD]++;
    }
    c->lpVtbl->IASetIndexBuffer(c, b, f, o);
}
static __inline void dsc_IASetVertexBuffers(ID3D11DeviceContext *c, UINT s, UINT n, ID3D11Buffer *const *b,
                                            const UINT *st, const UINT *of)
{
    if (DSC_ME(c)) {
        UINT i;
        if (DSC_ON() && b && st && of && s + n <= DSC_VB) {
            for (i = 0; i < n; i++) {
                UINT k = s + i;
                if (!(g_dsc.v_vb & (1u << k)) || g_dsc.vb[k] != b[i] || g_dsc.vbs[k] != st[i] || g_dsc.vbo[k] != of[i]) { break; }
            }
            if (i == n) { g_dsc.cnt[DSC_N_SKIP]++; return; }
        }
        for (i = 0; i < n && s + i < DSC_VB; i++) {
            UINT k = s + i;
            if (b && st && of) { g_dsc.vb[k] = b[i]; g_dsc.vbs[k] = st[i]; g_dsc.vbo[k] = of[i]; g_dsc.v_vb |= 1u << k; }
            else { g_dsc.v_vb &= ~(1u << k); }
        }
        g_dsc.cnt[DSC_N_FWD]++;
    }
    c->lpVtbl->IASetVertexBuffers(c, s, n, b, st, of);
}

/* ---- Shader ---- */
static __inline void dsc_VSSetShader(ID3D11DeviceContext *c, ID3D11VertexShader *sh, ID3D11ClassInstance *const *ci, UINT nci)
{
    if (DSC_ME(c)) {
        if (!nci && g_dsc.v_vs && g_dsc.vs == sh && DSC_ON()) { g_dsc.cnt[DSC_N_SKIP]++; return; }
        g_dsc.vs = sh; g_dsc.v_vs = nci ? 0u : 1u; g_dsc.cnt[DSC_N_FWD]++;
    }
    c->lpVtbl->VSSetShader(c, sh, ci, nci);
}
static __inline void dsc_PSSetShader(ID3D11DeviceContext *c, ID3D11PixelShader *sh, ID3D11ClassInstance *const *ci, UINT nci)
{
    if (DSC_ME(c)) {
        if (!nci && g_dsc.v_ps && g_dsc.ps == sh && DSC_ON()) { g_dsc.cnt[DSC_N_SKIP]++; return; }
        g_dsc.ps = sh; g_dsc.v_ps = nci ? 0u : 1u; g_dsc.cnt[DSC_N_FWD]++;
    }
    c->lpVtbl->PSSetShader(c, sh, ci, nci);
}

/* ---- Konstantenpuffer (ohne Versatz: f/n = 0xFFFFFFFF) ---- */
static __inline int dsc_cb_same(unsigned v, ID3D11Buffer *const *sb, const UINT *sf, const UINT *sn,
                                UINT s, UINT n, ID3D11Buffer *const *b, const UINT *f, const UINT *nn)
{
    UINT i;
    if (!b || s + n > DSC_CB) { return 0; }
    for (i = 0; i < n; i++) {
        UINT k = s + i;
        UINT fi = f ? f[i] : 0xFFFFFFFFu, ni = nn ? nn[i] : 0xFFFFFFFFu;
        if (!(v & (1u << k)) || sb[k] != b[i] || sf[k] != fi || sn[k] != ni) { return 0; }
    }
    return 1;
}
static __inline void dsc_cb_note(unsigned *v, ID3D11Buffer **sb, UINT *sf, UINT *sn,
                                 UINT s, UINT n, ID3D11Buffer *const *b, const UINT *f, const UINT *nn)
{
    UINT i;
    for (i = 0; i < n && s + i < DSC_CB; i++) {
        UINT k = s + i;
        if (b) { sb[k] = b[i]; sf[k] = f ? f[i] : 0xFFFFFFFFu; sn[k] = nn ? nn[i] : 0xFFFFFFFFu; *v |= 1u << k; }
        else { *v &= ~(1u << k); }
    }
}
static __inline void dsc_VSSetConstantBuffers1(ID3D11DeviceContext1 *c, UINT s, UINT n, ID3D11Buffer *const *b,
                                               const UINT *f, const UINT *nn);
static __inline void dsc_PSSetConstantBuffers1(ID3D11DeviceContext1 *c, UINT s, UINT n, ID3D11Buffer *const *b,
                                               const UINT *f, const UINT *nn);
static __inline void dsc_ring_fix(int ps, UINT s, UINT n, ID3D11Buffer *const *b)
{
    UINT i, num = 16u;
    for (i = 0; i < n; i++) {
        if (b[i] == g_dsc.ring) {
            if (ps) { dsc_PSSetConstantBuffers1(g_dsc.c1, s + i, 1, &g_dsc.ring, &g_dsc.ringf[1], &num); }
            else { dsc_VSSetConstantBuffers1(g_dsc.c1, s + i, 1, &g_dsc.ring, &g_dsc.ringf[0], &num); }
        }
    }
}
static __inline void dsc_VSSetConstantBuffers(ID3D11DeviceContext *c, UINT s, UINT n, ID3D11Buffer *const *b)
{
    int fix = 0;
    if (DSC_ME(c)) {
        if (DSC_ON() && dsc_cb_same(g_dsc.v_vcb, g_dsc.vcb, g_dsc.vcbf, g_dsc.vcbn, s, n, b, NULL, NULL)) { g_dsc.cnt[DSC_N_SKIP]++; return; }
        dsc_cb_note(&g_dsc.v_vcb, g_dsc.vcb, g_dsc.vcbf, g_dsc.vcbn, s, n, b, NULL, NULL);
        g_dsc.cnt[DSC_N_FWD]++;
        fix = (g_dsc.ring && g_dsc.c1 && b);
    }
    c->lpVtbl->VSSetConstantBuffers(c, s, n, b);
    if (fix) { dsc_ring_fix(0, s, n, b); }
}
static __inline void dsc_PSSetConstantBuffers(ID3D11DeviceContext *c, UINT s, UINT n, ID3D11Buffer *const *b)
{
    int fix = 0;
    if (DSC_ME(c)) {
        if (DSC_ON() && dsc_cb_same(g_dsc.v_pcb, g_dsc.pcb, g_dsc.pcbf, g_dsc.pcbn, s, n, b, NULL, NULL)) { g_dsc.cnt[DSC_N_SKIP]++; return; }
        dsc_cb_note(&g_dsc.v_pcb, g_dsc.pcb, g_dsc.pcbf, g_dsc.pcbn, s, n, b, NULL, NULL);
        g_dsc.cnt[DSC_N_FWD]++;
        fix = (g_dsc.ring && g_dsc.c1 && b);
    }
    c->lpVtbl->PSSetConstantBuffers(c, s, n, b);
    if (fix) { dsc_ring_fix(1, s, n, b); }
}
static __inline void dsc_VSSetConstantBuffers1(ID3D11DeviceContext1 *c, UINT s, UINT n, ID3D11Buffer *const *b,
                                               const UINT *f, const UINT *nn)
{
    if (DSC_ME(c)) {
        if (DSC_ON() && f && nn && dsc_cb_same(g_dsc.v_vcb, g_dsc.vcb, g_dsc.vcbf, g_dsc.vcbn, s, n, b, f, nn)) { g_dsc.cnt[DSC_N_SKIP]++; return; }
        if (f && nn) { dsc_cb_note(&g_dsc.v_vcb, g_dsc.vcb, g_dsc.vcbf, g_dsc.vcbn, s, n, b, f, nn); }
        else { g_dsc.v_vcb = 0; }
        g_dsc.cnt[DSC_N_FWD]++;
    } else {
        g_dsc.v_vcb = 0; /* unbekannter Zeiger: koennte der Immediate-Context sein */
    }
    c->lpVtbl->VSSetConstantBuffers1(c, s, n, b, f, nn);
}
static __inline void dsc_PSSetConstantBuffers1(ID3D11DeviceContext1 *c, UINT s, UINT n, ID3D11Buffer *const *b,
                                               const UINT *f, const UINT *nn)
{
    if (DSC_ME(c)) {
        if (DSC_ON() && f && nn && dsc_cb_same(g_dsc.v_pcb, g_dsc.pcb, g_dsc.pcbf, g_dsc.pcbn, s, n, b, f, nn)) { g_dsc.cnt[DSC_N_SKIP]++; return; }
        if (f && nn) { dsc_cb_note(&g_dsc.v_pcb, g_dsc.pcb, g_dsc.pcbf, g_dsc.pcbn, s, n, b, f, nn); }
        else { g_dsc.v_pcb = 0; }
        g_dsc.cnt[DSC_N_FWD]++;
    } else {
        g_dsc.v_pcb = 0;
    }
    c->lpVtbl->PSSetConstantBuffers1(c, s, n, b, f, nn);
}

/* ---- SRVs / Sampler ---- */
#define DSC_SLOTS_FN(NAME, T, MASK, ARR, CAP, METHOD)                                              \
static __inline void NAME(ID3D11DeviceContext *c, UINT s, UINT n, T *const *p)                     \
{                                                                                                  \
    if (DSC_ME(c)) {                                                                               \
        UINT i;                                                                                    \
        if (DSC_ON() && p && s + n <= CAP) {                                                       \
            for (i = 0; i < n; i++) {                                                              \
                if (!(g_dsc.MASK & (1u << (s + i))) || g_dsc.ARR[s + i] != p[i]) { break; }        \
            }                                                                                      \
            if (i == n) { g_dsc.cnt[DSC_N_SKIP]++; return; }                                       \
        }                                                                                          \
        for (i = 0; i < n && s + i < CAP; i++) {                                                   \
            if (p) { g_dsc.ARR[s + i] = p[i]; g_dsc.MASK |= 1u << (s + i); }                       \
            else { g_dsc.MASK &= ~(1u << (s + i)); }                                               \
        }                                                                                          \
        g_dsc.cnt[DSC_N_FWD]++;                                                                    \
    }                                                                                              \
    c->lpVtbl->METHOD(c, s, n, p);                                                                 \
}
DSC_SLOTS_FN(dsc_PSSetShaderResources, ID3D11ShaderResourceView, v_psrv, psrv, DSC_SRV, PSSetShaderResources)
DSC_SLOTS_FN(dsc_VSSetShaderResources, ID3D11ShaderResourceView, v_vsrv, vsrv, DSC_SRV, VSSetShaderResources)
DSC_SLOTS_FN(dsc_PSSetSamplers, ID3D11SamplerState, v_psmp, psmp, DSC_SMP, PSSetSamplers)
DSC_SLOTS_FN(dsc_VSSetSamplers, ID3D11SamplerState, v_vsmp, vsmp, DSC_SMP, VSSetSamplers)

/* ---- OM / RS ---- */
static __inline void dsc_OMSetBlendState(ID3D11DeviceContext *c, ID3D11BlendState *b, const FLOAT f[4], UINT m)
{
    if (DSC_ME(c)) {
        if (DSC_ON() && g_dsc.v_bl && g_dsc.bl == b && g_dsc.blm == m && g_dsc.blfnull == (f == NULL) &&
            (!f || (g_dsc.blf[0] == f[0] && g_dsc.blf[1] == f[1] && g_dsc.blf[2] == f[2] && g_dsc.blf[3] == f[3]))) {
            g_dsc.cnt[DSC_N_SKIP]++; return;
        }
        g_dsc.bl = b; g_dsc.blm = m; g_dsc.blfnull = (f == NULL);
        if (f) { g_dsc.blf[0] = f[0]; g_dsc.blf[1] = f[1]; g_dsc.blf[2] = f[2]; g_dsc.blf[3] = f[3]; }
        /* NaN-Faktoren nie als gleich werten (== schlaegt dann fehl, also weitergereicht) */
        g_dsc.v_bl = 1; g_dsc.cnt[DSC_N_FWD]++;
    }
    c->lpVtbl->OMSetBlendState(c, b, f, m);
}
static __inline void dsc_OMSetDepthStencilState(ID3D11DeviceContext *c, ID3D11DepthStencilState *d, UINT r)
{
    if (DSC_ME(c)) {
        if (DSC_ON() && g_dsc.v_ds && g_dsc.ds == d && g_dsc.dsr == r) { g_dsc.cnt[DSC_N_SKIP]++; return; }
        g_dsc.ds = d; g_dsc.dsr = r; g_dsc.v_ds = 1; g_dsc.cnt[DSC_N_FWD]++;
    }
    c->lpVtbl->OMSetDepthStencilState(c, d, r);
}
static __inline void dsc_RSSetState(ID3D11DeviceContext *c, ID3D11RasterizerState *r)
{
    if (DSC_ME(c)) {
        if (DSC_ON() && g_dsc.v_rs && g_dsc.rs == r) { g_dsc.cnt[DSC_N_SKIP]++; return; }
        g_dsc.rs = r; g_dsc.v_rs = 1; g_dsc.cnt[DSC_N_FWD]++;
    }
    c->lpVtbl->RSSetState(c, r);
}

/* ---- Gefahrenstellen: nur durchreichen + Schatten verwerfen ---- */
static __inline void dsc_OMSetRenderTargets(ID3D11DeviceContext *c, UINT n, ID3D11RenderTargetView *const *rt, ID3D11DepthStencilView *ds)
{
    if (DSC_ME(c)) { dsc_inval_srv(); g_dsc.cnt[DSC_N_FWD]++; }
    c->lpVtbl->OMSetRenderTargets(c, n, rt, ds);
}
static __inline void dsc_OMSetRenderTargetsAndUnorderedAccessViews(ID3D11DeviceContext *c, UINT n, ID3D11RenderTargetView *const *rt,
    ID3D11DepthStencilView *ds, UINT us, UINT un, ID3D11UnorderedAccessView *const *u, const UINT *ui)
{
    if (DSC_ME(c)) { dsc_inval_srv(); g_dsc.cnt[DSC_N_FWD]++; }
    c->lpVtbl->OMSetRenderTargetsAndUnorderedAccessViews(c, n, rt, ds, us, un, u, ui);
}
static __inline void dsc_CSSetUnorderedAccessViews(ID3D11DeviceContext *c, UINT s, UINT n, ID3D11UnorderedAccessView *const *u, const UINT *ui)
{
    if (DSC_ME(c)) { dsc_inval_srv(); }
    c->lpVtbl->CSSetUnorderedAccessViews(c, s, n, u, ui);
}
static __inline void dsc_ExecuteCommandList(ID3D11DeviceContext *c, ID3D11CommandList *l, BOOL keep)
{
    if (DSC_ME(c) && !keep) { dsc_inval_all(); }
    c->lpVtbl->ExecuteCommandList(c, l, keep);
}
static __inline void dsc_ClearState(ID3D11DeviceContext *c)
{
    if (DSC_ME(c)) { dsc_inval_all(); }
    c->lpVtbl->ClearState(c);
}

/* ---- Zaehler ---- */
static __inline HRESULT dsc_Map(ID3D11DeviceContext *c, ID3D11Resource *r, UINT sub, D3D11_MAP t, UINT fl, D3D11_MAPPED_SUBRESOURCE *m)
{
    if (DSC_ME(c)) { g_dsc.cnt[DSC_N_MAP]++; }
    return c->lpVtbl->Map(c, r, sub, t, fl, m);
}
static __inline void dsc_Draw(ID3D11DeviceContext *c, UINT n, UINT s)
{
    if (DSC_ME(c)) { g_dsc.cnt[DSC_N_DRAW]++; }
    c->lpVtbl->Draw(c, n, s);
}

static __inline void dsc_DrawIndexed(ID3D11DeviceContext *c, UINT n, UINT s, INT b)
{
    if (DSC_ME(c)) { g_dsc.cnt[DSC_N_DRAW]++; }
    c->lpVtbl->DrawIndexed(c, n, s, b);
}

#undef ID3D11DeviceContext_IASetInputLayout
#define ID3D11DeviceContext_IASetInputLayout(c, a) dsc_IASetInputLayout(c, a)
#undef ID3D11DeviceContext_IASetPrimitiveTopology
#define ID3D11DeviceContext_IASetPrimitiveTopology(c, a) dsc_IASetPrimitiveTopology(c, a)
#undef ID3D11DeviceContext_IASetVertexBuffers
#define ID3D11DeviceContext_IASetVertexBuffers(c, a, b, d, e, f) dsc_IASetVertexBuffers(c, a, b, d, e, f)
#undef ID3D11DeviceContext_VSSetShader
#define ID3D11DeviceContext_VSSetShader(c, a, b, d) dsc_VSSetShader(c, a, b, d)
#undef ID3D11DeviceContext_PSSetShader
#define ID3D11DeviceContext_PSSetShader(c, a, b, d) dsc_PSSetShader(c, a, b, d)
#undef ID3D11DeviceContext_VSSetConstantBuffers
#define ID3D11DeviceContext_VSSetConstantBuffers(c, a, b, d) dsc_VSSetConstantBuffers(c, a, b, d)
#undef ID3D11DeviceContext_PSSetConstantBuffers
#define ID3D11DeviceContext_PSSetConstantBuffers(c, a, b, d) dsc_PSSetConstantBuffers(c, a, b, d)
#undef ID3D11DeviceContext1_VSSetConstantBuffers1
#define ID3D11DeviceContext1_VSSetConstantBuffers1(c, a, b, d, e, f) dsc_VSSetConstantBuffers1(c, a, b, d, e, f)
#undef ID3D11DeviceContext1_PSSetConstantBuffers1
#define ID3D11DeviceContext1_PSSetConstantBuffers1(c, a, b, d, e, f) dsc_PSSetConstantBuffers1(c, a, b, d, e, f)
#undef ID3D11DeviceContext_PSSetShaderResources
#define ID3D11DeviceContext_PSSetShaderResources(c, a, b, d) dsc_PSSetShaderResources(c, a, b, d)
#undef ID3D11DeviceContext_VSSetShaderResources
#define ID3D11DeviceContext_VSSetShaderResources(c, a, b, d) dsc_VSSetShaderResources(c, a, b, d)
#undef ID3D11DeviceContext_PSSetSamplers
#define ID3D11DeviceContext_PSSetSamplers(c, a, b, d) dsc_PSSetSamplers(c, a, b, d)
#undef ID3D11DeviceContext_VSSetSamplers
#define ID3D11DeviceContext_VSSetSamplers(c, a, b, d) dsc_VSSetSamplers(c, a, b, d)
#undef ID3D11DeviceContext_OMSetBlendState
#define ID3D11DeviceContext_OMSetBlendState(c, a, b, d) dsc_OMSetBlendState(c, a, b, d)
#undef ID3D11DeviceContext_OMSetDepthStencilState
#define ID3D11DeviceContext_OMSetDepthStencilState(c, a, b) dsc_OMSetDepthStencilState(c, a, b)
#undef ID3D11DeviceContext_RSSetState
#define ID3D11DeviceContext_RSSetState(c, a) dsc_RSSetState(c, a)
#undef ID3D11DeviceContext_OMSetRenderTargets
#define ID3D11DeviceContext_OMSetRenderTargets(c, a, b, d) dsc_OMSetRenderTargets(c, a, b, d)
#undef ID3D11DeviceContext_OMSetRenderTargetsAndUnorderedAccessViews
#define ID3D11DeviceContext_OMSetRenderTargetsAndUnorderedAccessViews(c, a, b, d, e, f, g, h) \
    dsc_OMSetRenderTargetsAndUnorderedAccessViews(c, a, b, d, e, f, g, h)
#undef ID3D11DeviceContext_CSSetUnorderedAccessViews
#define ID3D11DeviceContext_CSSetUnorderedAccessViews(c, a, b, d, e) dsc_CSSetUnorderedAccessViews(c, a, b, d, e)
#undef ID3D11DeviceContext_ExecuteCommandList
#define ID3D11DeviceContext_ExecuteCommandList(c, a, b) dsc_ExecuteCommandList(c, a, b)
#undef ID3D11DeviceContext_ClearState
#define ID3D11DeviceContext_ClearState(c) dsc_ClearState(c)
#undef ID3D11DeviceContext_Map
#define ID3D11DeviceContext_Map(c, a, b, d, e, f) dsc_Map(c, a, b, d, e, f)
#undef ID3D11DeviceContext_Draw
#define ID3D11DeviceContext_Draw(c, a, b) dsc_Draw(c, a, b)
#undef ID3D11DeviceContext_DrawIndexed
#define ID3D11DeviceContext_DrawIndexed(c, a, b, d) dsc_DrawIndexed(c, a, b, d)
#undef ID3D11DeviceContext_IASetIndexBuffer
#define ID3D11DeviceContext_IASetIndexBuffer(c, a, b, d) dsc_IASetIndexBuffer(c, a, b, d)

#endif
