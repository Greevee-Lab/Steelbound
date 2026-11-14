/* render_photo.c - W195 PHOTO-MODE: Fotomodus (Beschreibung s. sb_photo.h).
 *
 * Bildquellen:
 *  - Cockpit an, Kamera unveraendert, x1: das Standbild der Pause (Kopie bzw. W193-Neuzeichnung)
 *    = das Spielbild ohne Menue (vor der Endbild-Nachbearbeitung, wie das Feedback-Bild).
 *  - sonst: Foto-Wiedergabe des juengsten Ticks (sb_interp_photo_render, W193-Weg mit den
 *    aktuellen Einstellungen) in eigene Kacheln:
 *      Cockpit an  -> Ziel = Schatten-BB, Viewport x N, Kachel-Versatz;
 *      Cockpit aus -> Ziel = Welt-RT des Hauptmonitors (Zensus: RT-Ziel mit den meisten
 *                     3D-Draws), dessen Spiel-Viewport auf das Ausgabebild abgebildet
 *                     (unabhaengig in x/y -> Seitenverhaeltnis der Projektion, nie gestreckt).
 *    Andere RT-Paesse (Sub-Monitore, S40-Komposition) laufen nativ mit.
 *  - freie Kamera: Welt-Draws des Welt-RT mit Sicht-Delta (render_interp.c ip_ph_cam_matrix).
 * Kacheln <= 4096 x 1024 (Streifenpuffer fuer WIC, 32-bit-Prozess), Viewport-Grenze 32767 je Seite.
 * Ohne Fotomodus wird hier nichts gerufen (Standardpfad bitgleich). */
#include "render_int.h"
#include "sb_profile.h" /* W229 SBA() */
#include "sb_photo.h"
#include "sb_userdata.h"
#include "render_qa.h" /* rq_in_mission */
#include <math.h>
#include <stdlib.h>

int sb_render_offscreen(void); /* d3d11_render.c: 1 = FPS-INTERP-Offscreen-BB */

#define PH_TW 4096u
#define PH_TH 1024u
#define PH_VPMAX 32767.0f
#define PH_PI 3.14159265f

static const int kPhScale[SB_PHOTO_NSCALE] = { 1, 2, 4, 8 };

static int ph_on;
static int ph_saved_monitor_fx, ph_saved_shading; /* W198: session-only overrides */
static int ph_cockpit = 1, ph_free;
static float ph_yaw, ph_pitch, ph_t[3];
static int ph_scale, ph_jpg, ph_init, ph_asp;
static int ph_req;
static int ph_dirty = 1;
static unsigned ph_hash_last;
static sb_ph_census_t ph_cen;
static int ph_cen_ok, ph_cen_tried;
static int ph_last_ok;
static char ph_last_name[160];
static unsigned ph_last_w, ph_last_h;
static double ph_last_ms;
static unsigned ph_nprev, ph_nshot;

/* Vorschau (Fenstergroesse) */
static ID3D11Texture2D *ph_view, *ph_view_d;
static ID3D11RenderTargetView *ph_view_rtv;
static ID3D11DepthStencilView *ph_view_dsv;
static unsigned ph_vw, ph_vh;
static int ph_view_valid;
/* Aufnahme-Kachel */
static ID3D11Texture2D *ph_tile, *ph_tile_d, *ph_stage;
static ID3D11RenderTargetView *ph_tile_rtv;
static ID3D11DepthStencilView *ph_tile_dsv;
static unsigned ph_tw, ph_th;
static DXGI_FORMAT ph_tfmt;
/* Kachel-Vorbelegung (Cockpit: Startstand des BB, gestreckt) */
static ID3D11VertexShader *ph_vs;
static ID3D11PixelShader *ph_ps;
static ID3D11Buffer *ph_cb;
static ID3D11SamplerState *ph_smp;
static int ph_sh_state;
typedef struct {
    float u0, v0, u1, v1;
    ID3D11RenderTargetView *rtv;
    unsigned w, h;
    int nsc; /* skalierte Zwischenziele: Quelle (Startstand) -> Ziel */
    ID3D11Texture2D *sc_src[8];
    ID3D11RenderTargetView *sc_rtv[8];
    unsigned sc_w[8], sc_h[8];
} ph_init_t;

/* ------------------------------------------------------------------ */
static unsigned ph_cfg_hash(void)
{
    const unsigned char *b = (const unsigned char *)&g_cfg;
    unsigned h = 2166136261u;
    size_t i;
    extern int g_shfx_bypass;
    for (i = 0; i < sizeof g_cfg; i++) { h = (h ^ b[i]) * 16777619u; }
    h = (h ^ (unsigned)g_shfx_bypass) * 16777619u;
    return h;
}

static void ph_rel(IUnknown **p)
{
    if (*p) { (*p)->lpVtbl->Release(*p); *p = NULL; }
}

static void ph_free_view(void)
{
    ph_rel((IUnknown **)&ph_view_rtv);
    ph_rel((IUnknown **)&ph_view_dsv);
    ph_rel((IUnknown **)&ph_view);
    ph_rel((IUnknown **)&ph_view_d);
    ph_vw = ph_vh = 0;
    ph_view_valid = 0;
}

static void ph_free_tile(void)
{
    ph_rel((IUnknown **)&ph_tile_rtv);
    ph_rel((IUnknown **)&ph_tile_dsv);
    ph_rel((IUnknown **)&ph_tile);
    ph_rel((IUnknown **)&ph_tile_d);
    ph_rel((IUnknown **)&ph_stage);
    ph_tw = ph_th = 0;
}

/* Farb- + Tiefenziel w x h (fmt), optional Staging. */
static int ph_mk_target(unsigned w, unsigned h, DXGI_FORMAT fmt, ID3D11Texture2D **t, ID3D11RenderTargetView **rtv,
                        ID3D11Texture2D **d, ID3D11DepthStencilView **dsv, ID3D11Texture2D **stg)
{
    D3D11_TEXTURE2D_DESC td;
    memset(&td, 0, sizeof td);
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, t)) ||
        FAILED(ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)*t, NULL, rtv))) {
        return 0;
    }
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, d)) ||
        FAILED(ID3D11Device_CreateDepthStencilView(g_dev, (ID3D11Resource *)*d, NULL, dsv))) {
        return 0;
    }
    if (stg) {
        td.Format = fmt;
        td.BindFlags = 0;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, stg))) { return 0; }
    }
    return 1;
}

static int ph_ensure_view(DXGI_FORMAT fmt)
{
    if (ph_view && ph_vw == g_bb_w && ph_vh == g_bb_h) { return 1; }
    ph_free_view();
    if (!ph_mk_target(g_bb_w, g_bb_h, fmt, &ph_view, &ph_view_rtv, &ph_view_d, &ph_view_dsv, NULL)) {
        HLOG_W("PHOTO: Vorschau-Ziel %ux%u nicht anlegbar", g_bb_w, g_bb_h);
        ph_free_view();
        return 0;
    }
    ph_vw = g_bb_w;
    ph_vh = g_bb_h;
    return 1;
}

static int ph_ensure_tile(unsigned w, unsigned h, DXGI_FORMAT fmt)
{
    if (ph_tile && ph_tw == w && ph_th == h && ph_tfmt == fmt) { return 1; }
    ph_free_tile();
    if (!ph_mk_target(w, h, fmt, &ph_tile, &ph_tile_rtv, &ph_tile_d, &ph_tile_dsv, &ph_stage)) {
        HLOG_W("PHOTO: Kachel %ux%u nicht anlegbar", w, h);
        ph_free_tile();
        return 0;
    }
    ph_tw = w;
    ph_th = h;
    ph_tfmt = fmt;
    return 1;
}

/* ---- Kachel-Vorbelegung: Quelltextur-Ausschnitt gestreckt (linear) ins Ziel ---- */
static const char kPH_VS[] =
    "cbuffer C : register(b0) { float4 uvr; };\n"
    "struct O { float4 p : SV_Position; float2 t : TEXCOORD0; };\n"
    "O main(uint i : SV_VertexID) { O o; float2 q = float2((i & 1u) ? 1.0 : 0.0, (i & 2u) ? 1.0 : 0.0);\n"
    "  o.p = float4(q.x * 2.0 - 1.0, 1.0 - q.y * 2.0, 0.0, 1.0); o.t = lerp(uvr.xy, uvr.zw, q); return o; }\n";
static const char kPH_PS[] =
    "Texture2D t : register(t0); SamplerState s : register(s0);\n"
    "struct O { float4 p : SV_Position; float2 t : TEXCOORD0; };\n"
    "float4 main(O i) : SV_Target { return float4(t.Sample(s, i.t).rgb, 1.0); }\n";

static int ph_shaders(void)
{
    ID3DBlob *vsb = NULL, *psb = NULL;
    D3D11_BUFFER_DESC bd;
    D3D11_SAMPLER_DESC sd;
    if (ph_sh_state) { return ph_sh_state > 0; }
    ph_sh_state = -1;
    if (!sb_im_compile(kPH_VS, "vs_4_0", &vsb) || !sb_im_compile(kPH_PS, "ps_4_0", &psb)) {
        if (vsb) { vsb->lpVtbl->Release(vsb); }
        return 0;
    }
    ID3D11Device_CreateVertexShader(g_dev, vsb->lpVtbl->GetBufferPointer(vsb), vsb->lpVtbl->GetBufferSize(vsb), NULL, &ph_vs);
    ID3D11Device_CreatePixelShader(g_dev, psb->lpVtbl->GetBufferPointer(psb), psb->lpVtbl->GetBufferSize(psb), NULL, &ph_ps);
    vsb->lpVtbl->Release(vsb);
    psb->lpVtbl->Release(psb);
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = 16;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &ph_cb);
    memset(&sd, 0, sizeof sd);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11Device_CreateSamplerState(g_dev, &sd, &ph_smp);
    if (!ph_vs || !ph_ps || !ph_cb || !ph_smp) { return 0; }
    ph_sh_state = 1;
    return 1;
}

/* Ein Ausschnitt (u0,v0)-(u1,v1) von src gestreckt nach rtv (w x h). Zustand setzt der Aufrufer zurueck. */
static void ph_blit1(ID3D11Texture2D *src, float u0, float v0, float u1, float v1, ID3D11RenderTargetView *rtv,
                     unsigned w, unsigned h)
{
    ID3D11ShaderResourceView *srv = NULL, *nul = NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_VIEWPORT vp;
    if (!src || !rtv) { return; }
    if (FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)src, NULL, &srv))) { return; }
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)ph_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        ID3D11ShaderResourceView_Release(srv);
        return;
    }
    ((float *)m.pData)[0] = u0; ((float *)m.pData)[1] = v0;
    ((float *)m.pData)[2] = u1; ((float *)m.pData)[3] = v1;
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)ph_cb, 0);
    vp.TopLeftX = 0.0f; vp.TopLeftY = 0.0f;
    vp.Width = (float)w; vp.Height = (float)h;
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &vp);
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, &srv);
    ID3D11DeviceContext_Draw(g_ctx, 4, 0);
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, &nul);
    ID3D11ShaderResourceView_Release(srv);
}

/* Callback aus ip_replay_ex nach den Startkopien: BB-Kachel und skalierte Zwischenziele mit dem
 * Startstand (gestreckt) vorbelegen. Zustand vollstaendig sichern/zurueck (die Wiedergabe erbt s0). */
static void ph_init_cb(void *ctx, ID3D11Texture2D *start)
{
    ph_init_t *pi = (ph_init_t *)ctx;
    D3D11_VIEWPORT s_vp;
    UINT s_nvp = 1;
    int j;
    ID3D11RenderTargetView *s_rtv = NULL;
    ID3D11DepthStencilView *s_dsv = NULL;
    ID3D11VertexShader *s_vs = NULL;
    ID3D11PixelShader *s_ps = NULL;
    ID3D11Buffer *s_vcb = NULL;
    ID3D11ShaderResourceView *s_srv = NULL;
    ID3D11SamplerState *s_smp = NULL;
    ID3D11InputLayout *s_lay = NULL;
    D3D11_PRIMITIVE_TOPOLOGY s_topo;
    ID3D11BlendState *s_bs = NULL;
    FLOAT s_bf[4];
    UINT s_mask = 0, s_sref = 0;
    ID3D11DepthStencilState *s_ds = NULL;
    ID3D11RasterizerState *s_rs = NULL;
    if (!pi || !ph_shaders()) { return; }
    ID3D11DeviceContext_OMGetRenderTargets(g_ctx, 1, &s_rtv, &s_dsv);
    ID3D11DeviceContext_RSGetViewports(g_ctx, &s_nvp, &s_vp);
    ID3D11DeviceContext_VSGetShader(g_ctx, &s_vs, NULL, NULL);
    ID3D11DeviceContext_PSGetShader(g_ctx, &s_ps, NULL, NULL);
    ID3D11DeviceContext_VSGetConstantBuffers(g_ctx, 0, 1, &s_vcb);
    ID3D11DeviceContext_PSGetShaderResources(g_ctx, 0, 1, &s_srv);
    ID3D11DeviceContext_PSGetSamplers(g_ctx, 0, 1, &s_smp);
    ID3D11DeviceContext_IAGetInputLayout(g_ctx, &s_lay);
    ID3D11DeviceContext_IAGetPrimitiveTopology(g_ctx, &s_topo);
    ID3D11DeviceContext_OMGetBlendState(g_ctx, &s_bs, s_bf, &s_mask);
    ID3D11DeviceContext_OMGetDepthStencilState(g_ctx, &s_ds, &s_sref);
    ID3D11DeviceContext_RSGetState(g_ctx, &s_rs);
    ID3D11DeviceContext_RSSetState(g_ctx, NULL);
    ID3D11DeviceContext_OMSetBlendState(g_ctx, NULL, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(g_ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D11DeviceContext_VSSetShader(g_ctx, ph_vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(g_ctx, 0, 1, &ph_cb);
    ID3D11DeviceContext_PSSetShader(g_ctx, ph_ps, NULL, 0);
    ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 1, &ph_smp);
    ph_blit1(start, pi->u0, pi->v0, pi->u1, pi->v1, pi->rtv, pi->w, pi->h);
    for (j = 0; j < pi->nsc; j++) { ph_blit1(pi->sc_src[j], 0.0f, 0.0f, 1.0f, 1.0f, pi->sc_rtv[j], pi->sc_w[j], pi->sc_h[j]); }
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &s_rtv, s_dsv);
    if (s_nvp) { ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &s_vp); }
    ID3D11DeviceContext_VSSetShader(g_ctx, s_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_ctx, s_ps, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(g_ctx, 0, 1, &s_vcb);
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, &s_srv);
    ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 1, &s_smp);
    ID3D11DeviceContext_IASetInputLayout(g_ctx, s_lay);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, s_topo);
    ID3D11DeviceContext_OMSetBlendState(g_ctx, s_bs, s_bf, s_mask);
    ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, s_ds, s_sref);
    ID3D11DeviceContext_RSSetState(g_ctx, s_rs);
    if (s_rtv) { ID3D11RenderTargetView_Release(s_rtv); }
    if (s_dsv) { ID3D11DepthStencilView_Release(s_dsv); }
    if (s_vs) { ID3D11VertexShader_Release(s_vs); }
    if (s_ps) { ID3D11PixelShader_Release(s_ps); }
    if (s_vcb) { ID3D11Buffer_Release(s_vcb); }
    if (s_srv) { ID3D11ShaderResourceView_Release(s_srv); }
    if (s_smp) { ID3D11SamplerState_Release(s_smp); }
    if (s_lay) { ID3D11InputLayout_Release(s_lay); }
    if (s_bs) { ID3D11BlendState_Release(s_bs); }
    if (s_ds) { ID3D11DepthStencilState_Release(s_ds); }
    if (s_rs) { ID3D11RasterizerState_Release(s_rs); }
}

/* ---- skalierte Zwischenziele (Cockpit-Aufnahme x N): Welt-/Monitor-RTs (Kante >= 256) N-fach ---- */
#define PH_NSC 8
static ID3D11Texture2D *ph_sc_t[PH_NSC], *ph_sc_dt[PH_NSC];
static ID3D11RenderTargetView *ph_sc_rtv[PH_NSC];
static ID3D11DepthStencilView *ph_sc_dsv[PH_NSC];
static ID3D11ShaderResourceView *ph_sc_srv[PH_NSC];
static int ph_sc_n;

static void ph_free_sc(void)
{
    int j;
    for (j = 0; j < PH_NSC; j++) {
        ph_rel((IUnknown **)&ph_sc_rtv[j]);
        ph_rel((IUnknown **)&ph_sc_dsv[j]);
        ph_rel((IUnknown **)&ph_sc_srv[j]);
        ph_rel((IUnknown **)&ph_sc_t[j]);
        ph_rel((IUnknown **)&ph_sc_dt[j]);
    }
    ph_sc_n = 0;
}

/* W233 SHOT-8K-MONITOR: Grenzen der skalierten Zwischenziele aus dem Grafikspeicher der Karte.
 * D3D11-Einzelressource <= min(max(128 MB, 1/4 VRAM), 2048 MB) (D3D11_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_*),
 * Summe <= min(3 GB, 60 % VRAM), mindestens 256 MB. Ohne Abfrage: 2048 MB / 3 GB (bisher). */
static void ph_sc_limits(double *res_max, double *total_max)
{
    IDXGIDevice *dx = NULL;
    IDXGIAdapter *ad = NULL;
    DXGI_ADAPTER_DESC ds;
    double vram = 0.0;
    *res_max = 2048.0 * 1048576.0;
    *total_max = 3.0e9;
    if (g_dev && SUCCEEDED(ID3D11Device_QueryInterface(g_dev, &IID_IDXGIDevice, (void **)&dx)) && dx) {
        if (SUCCEEDED(IDXGIDevice_GetAdapter(dx, &ad)) && ad) {
            if (SUCCEEDED(IDXGIAdapter_GetDesc(ad, &ds))) { vram = (double)ds.DedicatedVideoMemory; }
            IDXGIAdapter_Release(ad);
        }
        IDXGIDevice_Release(dx);
    }
    if (vram > 0.0) {
        double r = vram * 0.25, t = vram * 0.6;
        if (r < 128.0 * 1048576.0) { r = 128.0 * 1048576.0; }
        if (r < *res_max) { *res_max = r; }
        if (t < 256.0 * 1048576.0) { t = 256.0 * 1048576.0; }
        if (t < *total_max) { *total_max = t; }
    }
}

/* Liste anlegen und in r/pi eintragen. Rueckgabe Anzahl.
 * W233: Faktor je Ziel (bisher ein gemeinsamer): Kante <= photo_rt_max (Standard 8192, D3D11 <= 16384), Einzel-
 * ressource und Summe (Farbe + Tiefe) in den Grenzen von ph_sc_limits (groesstes Ziel gibt zuerst nach, bei Gleichstand das kleinere RT).
 * Schlaegt das Anlegen fehl (Grafikspeicher, 32-bit), werden alle Faktoren eine Stufe kleiner versucht statt
 * ganz ohne Skalierung (bisher: ein Fehler -> Monitore nativ). Ziele mit Faktor 1 laufen nativ mit. */
static int ph_setup_sc(sb_ph_rep_t *r, ph_init_t *pi, unsigned n)
{
    unsigned i, m = 0, cap, idx[PH_NSC], kk[PH_NSC], att, kmax = 1;
    double res_max, tot_max, bytes = 0.0;
    int j = 0, q;
    char lst[400];
    ph_free_sc();
    if (g_cfg.photo_rt_max <= 0 || n < 2u) { return 0; } /* 0 = Zwischenziele aus (Vergleich/alter Stand x1) */
    cap = (unsigned)g_cfg.photo_rt_max;
    if (cap < 256u) { cap = 256u; }
    if (cap > 16384u) { cap = 16384u; }
    ph_sc_limits(&res_max, &tot_max);
    for (i = 0; i < ph_cen.n_rt && m < PH_NSC; i++) {
        unsigned w = ph_cen.rt[i].w, h = ph_cen.rt[i].h, k = n;
        if ((w < h ? w : h) < 256u || (int)ph_cen.rt[i].e == ph_cen.bb) { continue; }
        while (k > 1u && (w * k > cap || h * k > cap || (double)w * k * h * k * 4.0 > res_max)) { k--; }
        idx[m] = i;
        kk[m++] = k;
    }
    for (att = 0; att < 8u; att++) {
        int fail = 0;
        for (;;) { /* Summe: groesstes Ziel eine Stufe kleiner, bis es passt */
            int big = -1;
            double bb = 0.0;
            bytes = 0.0;
            for (q = 0; q < (int)m; q++) {
                double e = (double)ph_cen.rt[idx[q]].w * ph_cen.rt[idx[q]].h * kk[q] * kk[q] *
                           (ph_cen.rt[idx[q]].d >= 0 ? 8.0 : 4.0);
                if (kk[q] < 2u) { continue; }
                bytes += e;
                /* Gleichstand (+-1 %): kleineres RT gibt zuerst nach (Welt-/Monitor-RT des Hauptmonitors sind die groessten) */
                if (e > bb * 1.01 || (e > bb * 0.99 && big >= 0 &&
                                      ph_cen.rt[idx[q]].w * ph_cen.rt[idx[q]].h < ph_cen.rt[idx[big]].w * ph_cen.rt[idx[big]].h)) {
                    bb = e > bb ? e : bb;
                    big = q;
                }
            }
            if (bytes <= tot_max || big < 0) { break; }
            kk[big]--;
        }
        j = 0;
        kmax = 1;
        lst[0] = 0;
        for (q = 0; q < (int)m && !fail; q++) {
            D3D11_TEXTURE2D_DESC td;
            unsigned k = kk[q];
            i = idx[q];
            if (k < 2u) { continue; }
            ID3D11Texture2D_GetDesc(ph_cen.rt[i].tex, &td);
            td.Width = ph_cen.rt[i].w * k;
            td.Height = ph_cen.rt[i].h * k;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.SampleDesc.Count = 1;
            td.SampleDesc.Quality = 0;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            td.CPUAccessFlags = 0;
            td.MiscFlags = 0;
            if (g_cfg.photo_rt_fail_test > 0) { /* TEST(W233): Anlegefehler nachstellen (Standard 0) */
                g_cfg.photo_rt_fail_test--;
                fail = 1;
            } else if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &ph_sc_t[j])) ||
                       FAILED(ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)ph_sc_t[j], NULL, &ph_sc_rtv[j])) ||
                       FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)ph_sc_t[j], NULL, &ph_sc_srv[j]))) {
                fail = 1;
            }
            if (!fail && ph_cen.rt[i].d >= 0) {
                td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
                td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
                if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &ph_sc_dt[j])) ||
                    FAILED(ID3D11Device_CreateDepthStencilView(g_dev, (ID3D11Resource *)ph_sc_dt[j], NULL, &ph_sc_dsv[j]))) {
                    fail = 1;
                } else {
                    ID3D11DeviceContext_ClearDepthStencilView(g_ctx, ph_sc_dsv[j], D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
                }
            }
            if (fail) {
                HLOG_W("PHOTO: Zwischenziel e%d %ux%u (x%u) nicht anlegbar - Versuch %u, eine Stufe kleiner", ph_cen.rt[i].e,
                       td.Width, td.Height, k, att + 1u);
                break;
            }
            r->sc_e[j] = ph_cen.rt[i].e;
            r->sc_d[j] = ph_cen.rt[i].d;
            r->sc_rtv[j] = ph_sc_rtv[j];
            r->sc_dsv[j] = ph_sc_dsv[j];
            r->sc_srv[j] = ph_sc_srv[j];
            r->sc_k[j] = (float)k;
            r->sc_w[j] = (LONG)td.Width;
            r->sc_h[j] = (LONG)td.Height;
            pi->sc_src[j] = ph_cen.rt[i].tex;
            pi->sc_rtv[j] = ph_sc_rtv[j];
            pi->sc_w[j] = td.Width;
            pi->sc_h[j] = td.Height;
            if (k > kmax) { kmax = k; }
            {
                size_t ln = strlen(lst);
                if (ln + 40u < sizeof lst) {
                    _snprintf_s(lst + ln, sizeof lst - ln, _TRUNCATE, "%se%d %ux%u->%ux%u", ln ? ", " : "", ph_cen.rt[i].e,
                                ph_cen.rt[i].w, ph_cen.rt[i].h, td.Width, td.Height);
                }
            }
            j++;
        }
        if (!fail) { break; }
        ph_free_sc();
        memset(r->sc_e, 0, sizeof r->sc_e);
        j = 0;
        {
            int any = 0;
            for (q = 0; q < (int)m; q++) { if (kk[q] > 1u) { kk[q]--; } if (kk[q] > 1u) { any = 1; } }
            if (!any) { break; }
        }
    }
    r->nsc = pi->nsc = ph_sc_n = j;
    if (!j) {
        if (m) { HLOG_W("PHOTO: keine skalierten Zwischenziele (x%u) - Monitore nativ", n); }
        return 0;
    }
    HLOG_I("PHOTO: %d Zwischenziele (Welt/Monitore) x%u fuer die Cockpit-Aufnahme (~%.0f MB, Grenzen Kante %u, "
           "Einzel %.0f MB, Summe %.0f MB): %s", j, kmax, bytes / 1048576.0, cap, res_max / 1048576.0,
           tot_max / 1048576.0, lst);
    return j;
}

/* ------------------------------------------------------------------ */
/* Kamera                                                              */
/* ------------------------------------------------------------------ */
static float ph_max_yaw(void) { return (float)(g_cfg.photo_cam_yaw > 0 ? g_cfg.photo_cam_yaw : 0); }
static float ph_max_pitch(void) { return (float)(g_cfg.photo_cam_pitch > 0 ? g_cfg.photo_cam_pitch : 0); }
static float ph_max_dist(void) { return (float)(g_cfg.photo_cam_dist > 0 ? g_cfg.photo_cam_dist : 0); }

/* Kamera-Orientierung (Spaltenform, Kamera-lokal -> Sicht der Spielkamera): Ry(yaw) * Rx(pitch) */
static void ph_rot(float R[9])
{
    float cy = cosf(ph_yaw * PH_PI / 180.0f), sy = sinf(ph_yaw * PH_PI / 180.0f);
    float cp = cosf(ph_pitch * PH_PI / 180.0f), sp = sinf(ph_pitch * PH_PI / 180.0f);
    /* Ry = [cy 0 sy; 0 1 0; -sy 0 cy], Rx = [1 0 0; 0 cp -sp; 0 sp cp] */
    R[0] = cy;  R[1] = sy * sp;  R[2] = sy * cp;
    R[3] = 0.0f; R[4] = cp;      R[5] = -sp;
    R[6] = -sy; R[7] = cy * sp;  R[8] = cy * cp;
}

/* Sicht-Delta (Zeilenvektor, homogen): v' = (v - t*h) * R  (= R^T (v - t h) in Spaltenform) */
static void ph_cam_delta(float D[16])
{
    float R[9];
    int i, j;
    ph_rot(R);
    memset(D, 0, 16 * sizeof(float));
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) { D[i * 4 + j] = R[i * 3 + j]; }
    }
    D[15] = 1.0f;
    for (j = 0; j < 3; j++) { /* Zeile 3: -t * R */
        D[12 + j] = -(ph_t[0] * R[0 * 3 + j] + ph_t[1] * R[1 * 3 + j] + ph_t[2] * R[2 * 3 + j]);
    }
}

void sb_photo_cam_move(float dyaw, float dpitch, float fwd, float right, float up)
{
    float R[9], my = ph_max_yaw(), mp = ph_max_pitch(), md = ph_max_dist(), d;
    if (!ph_on || !ph_free) { return; }
    if (dyaw == 0.0f && dpitch == 0.0f && fwd == 0.0f && right == 0.0f && up == 0.0f) { return; }
    ph_yaw += dyaw;
    ph_pitch += dpitch;
    if (ph_yaw > my) { ph_yaw = my; }
    if (ph_yaw < -my) { ph_yaw = -my; }
    if (ph_pitch > mp) { ph_pitch = mp; }
    if (ph_pitch < -mp) { ph_pitch = -mp; }
    ph_rot(R);
    /* vorwaerts = R * (0,0,-1), rechts = R * (1,0,0), oben = (0,1,0) der Spielkamera */
    ph_t[0] += -fwd * R[2] + right * R[0];
    ph_t[1] += -fwd * R[5] + right * R[3] + up;
    ph_t[2] += -fwd * R[8] + right * R[6];
    d = sqrtf(ph_t[0] * ph_t[0] + ph_t[1] * ph_t[1] + ph_t[2] * ph_t[2]);
    if (d > md && d > 0.0f) {
        float k = md / d;
        ph_t[0] *= k; ph_t[1] *= k; ph_t[2] *= k;
    }
    ph_dirty = 1;
}

void sb_photo_cam_reset(void)
{
    ph_yaw = ph_pitch = 0.0f;
    ph_t[0] = ph_t[1] = ph_t[2] = 0.0f;
    ph_dirty = 1;
    HLOG_I("PHOTO: Kamera zurueckgesetzt (f=%u)", g_present_no);
}

/* ------------------------------------------------------------------ */
/* Zustand / UI                                                        */
/* ------------------------------------------------------------------ */
static int ph_scale_idx_of(int n)
{
    int i;
    for (i = 0; i < SB_PHOTO_NSCALE; i++) { if (kPhScale[i] == n) { return i; } }
    return 0;
}

/* Seitenverhaeltnis des Kamerabilds: so, wie das Spiel es auf dem Hauptmonitor zeigt (Zensus: Komposition
 * Monitor-RT -> BB, gemessen 1,334 ~ 4:3, passend zur VS-Projektion 4:3 des Spiels, VR-PROTO W134);
 * ohne Messung 4:3. */
static float ph_aspect(void)
{
    if (ph_cen_ok && ph_cen.disp_asp > 0.9f) { return ph_cen.disp_asp; }
    return 4.0f / 3.0f;
}

/* Ausgabegroesse der Stufe i (Cockpit an: Fenster x N, aus: Fensterhoehe x N im Seitenverhaeltnis der Projektion) */
static void ph_out_size(int i, int cockpit, unsigned *w, unsigned *h)
{
    unsigned n = (unsigned)kPhScale[i];
    if (cockpit || ph_asp != 2) { /* Bildschirmformat (Cockpit, ohne Cockpit Ausschnitt/weit) */
        *w = g_bb_w * n;
        *h = g_bb_h * n;
    } else {
        *h = g_bb_h * n;
        *w = (unsigned)((float)*h * ph_aspect() + 0.5f) & ~1u;
    }
}

static int ph_rec_ok(void)
{
    return ph_cen_ok && !g_cfg.vr_enable;
}

/* Abbildung des Kamerabilds (Spiel-Viewport des Welt-RT) in eine Ausgabe W x H je Bildformat:
 * Rechteck (dx,dy,dw,dh) und Clip-x-Faktor fx. Nie gestreckt:
 *  0 Ausschnitt: Kamerabild deckt die Ausgabe (Ueberstand oben/unten bzw. links/rechts abgeschnitten)
 *  1 weit: Hoehe passt, Spiel-Viewport auf die volle Breite, Clip x * fx (fx = Kamerabreite / Ausgabebreite)
 *    -> mehr Sichtfeld seitlich; was das Spiel dort nicht gezeichnet hat, bleibt leer
 *  2 Kamera (wie Monitor): Kamerabild eingepasst (Ausgabe hat dessen Seitenverhaeltnis) */
static void ph_world_layout(float W, float H, int asp, float *dx, float *dy, float *dw, float *dh, float *fx)
{
    float a = ph_aspect();
    *fx = 1.0f;
    if (asp == 1 && W / H > a) {
        *dx = 0.0f; *dy = 0.0f; *dw = W; *dh = H;
        *fx = (H * a) / W;
        return;
    }
    if (asp == 2 ? (W / H > a) : (W / H < a)) { *dh = H; *dw = H * a; } else { *dw = W; *dh = W / a; }
    *dx = (W - *dw) * 0.5f;
    *dy = (H - *dh) * 0.5f;
}

static int ph_scale_ok(int i, int cockpit)
{
    unsigned w, h;
    if (i < 0 || i >= SB_PHOTO_NSCALE) { return 0; }
    if (i == 0 && cockpit) { return 1; }
    if (!ph_rec_ok()) { return 0; }
    ph_out_size(i, cockpit, &w, &h);
    if (w > (unsigned)PH_VPMAX || h > (unsigned)PH_VPMAX) { return 0; }
    if (!cockpit) { /* Welt-RT-Viewport (volles RT, auf die Ausgabe abgebildet) in den D3D11-Grenzen */
        float dx, dy, dw, dh, fx;
        float kx, ky, vx, vy, vw, vh;
        ph_world_layout((float)w, (float)h, ph_asp, &dx, &dy, &dw, &dh, &fx);
        kx = dw / (float)(ph_cen.g_w ? ph_cen.g_w : 1);
        ky = dh / (float)(ph_cen.g_h ? ph_cen.g_h : 1);
        vw = (float)ph_cen.w_lw * kx;
        vh = (float)ph_cen.w_lh * ky;
        vx = dx - (float)ph_cen.g_x * kx - (float)w;      /* schlechteste Kachel */
        vy = dy - (float)ph_cen.g_y * ky - (float)h;
        if (vw > PH_VPMAX || vh > PH_VPMAX || vx < -32768.0f || vy < -32768.0f ||
            dx - (float)ph_cen.g_x * kx + vw > PH_VPMAX || dy - (float)ph_cen.g_y * ky + vh > PH_VPMAX) {
            return 0;
        }
    }
    return 1;
}

void sb_photo_enter(void)
{
    if (ph_on) { return; }
    ph_saved_monitor_fx = g_cfg.monitor_fx;
    ph_saved_shading = g_cfg.shading;
    HLOG_I("PHOTO opts: saved monitor_fx=%d shading=%d", g_cfg.monitor_fx, g_cfg.shading);
    if (!ph_init) {
        ph_init = 1;
        ph_scale = ph_scale_idx_of(g_cfg.photo_scale);
        ph_jpg = g_cfg.photo_jpg ? 1 : 0;
        ph_asp = g_cfg.photo_aspect >= 0 && g_cfg.photo_aspect <= 2 ? g_cfg.photo_aspect : 1;
    }
    ph_on = 1;
    ph_cockpit = 1;
    ph_free = 0;
    ph_yaw = ph_pitch = 0.0f;
    ph_t[0] = ph_t[1] = ph_t[2] = 0.0f;
    ph_dirty = 1;
    ph_req = 0;
    ph_cen_ok = 0;
    ph_cen_tried = 0;
    ph_last_ok = 0;
    ph_nprev = 0;
    {
        char d[600];
        sb_photo_dir(d, sizeof d);
        HLOG_I("PHOTO: an (f=%u, Stufe x%d, %s, Ordner %s)", g_present_no, kPhScale[ph_scale], ph_jpg ? "JPG" : "PNG", d);
    }
}

void sb_photo_leave(void)
{
    if (!ph_on) { return; }
    g_cfg.monitor_fx = ph_saved_monitor_fx;
    g_cfg.shading = ph_saved_shading;
    sb_render_pause_dirty();
    HLOG_I("PHOTO opts: restored monitor_fx=%d shading=%d", g_cfg.monitor_fx, g_cfg.shading);
    ph_req = 0;
    ph_on = 0;
    ph_view_valid = 0;
    HLOG_I("PHOTO: aus (f=%u, %u Vorschau-Neuzeichnungen)", g_present_no, ph_nprev);
}

int sb_photo_active(void)
{
    return ph_on;
}

/* Both replay paths (pause and tiled capture) read these temporary values. */
void sb_photo_set_effect(int shader, int on)
{
    if (!ph_on) { return; }
    if (shader) { g_cfg.shading = on ? 1 : 0; }
    else { g_cfg.monitor_fx = on ? 1 : 0; }
    ph_dirty = 1;
    sb_render_pause_dirty();
    HLOG_I("PHOTO opts: monitor_fx=%d shading=%d", g_cfg.monitor_fx, g_cfg.shading);
}

void sb_photo_set_cockpit(int on)
{
    if (!on && !ph_rec_ok()) { return; }
    if (ph_cockpit != (on ? 1 : 0)) {
        ph_cockpit = on ? 1 : 0;
        ph_dirty = 1;
        HLOG_I("PHOTO: Cockpit %s (f=%u)", ph_cockpit ? "an" : "aus", g_present_no);
    }
}

void sb_photo_set_freecam(int on)
{
    if (on && !ph_rec_ok()) { return; }
    if (ph_free != (on ? 1 : 0)) {
        ph_free = on ? 1 : 0;
        ph_dirty = 1;
        HLOG_I("PHOTO: freie Kamera %s (f=%u, Grenzen +-%d Grad / +-%d Grad / %d Einheiten)", ph_free ? "an" : "aus",
               g_present_no, g_cfg.photo_cam_yaw, g_cfg.photo_cam_pitch, g_cfg.photo_cam_dist);
    }
}

void sb_photo_set_scale(int idx)
{
    ph_scale = (idx % SB_PHOTO_NSCALE + SB_PHOTO_NSCALE) % SB_PHOTO_NSCALE;
}

void sb_photo_set_jpg(int jpg)
{
    ph_jpg = jpg ? 1 : 0;
}

void sb_photo_set_aspect(int a)
{
    a = (a % 3 + 3) % 3;
    if (a != ph_asp) {
        static const char *const nm[3] = { "Bildschirm (Ausschnitt)", "Bildschirm (weit)", "Kamera (wie Monitor)" };
        ph_asp = a;
        ph_dirty = 1;
        HLOG_I("PHOTO: Bildformat ohne Cockpit -> %s (f=%u)", nm[a], g_present_no);
    }
}

void sb_photo_get(sb_photo_ui_t *u)
{
    int i;
    memset(u, 0, sizeof *u);
    u->active = ph_on;
    u->monitor_fx = g_cfg.monitor_fx;
    u->shading = g_cfg.shading;
    u->effect_pending = (g_cfg.shading && (!ph_saved_shading || (ph_cen_ok && !ph_cen.n_wrq))) ||
                        (g_cfg.monitor_fx && ph_saved_monitor_fx != 1);
    u->rec = ph_rec_ok();
    u->vr = g_cfg.vr_enable != 0;
    u->cockpit = ph_cockpit;
    u->freecam = ph_free;
    u->yaw = ph_yaw;
    u->pitch = ph_pitch;
    u->dist = sqrtf(ph_t[0] * ph_t[0] + ph_t[1] * ph_t[1] + ph_t[2] * ph_t[2]);
    u->max_yaw = ph_max_yaw();
    u->max_pitch = ph_max_pitch();
    u->max_dist = ph_max_dist();
    u->scale = ph_scale;
    u->jpg = ph_jpg;
    u->aspect = ph_asp;
    for (i = 0; i < SB_PHOTO_NSCALE; i++) {
        ph_out_size(i, ph_cockpit, &u->sw[i], &u->sh[i]);
        u->savail[i] = ph_scale_ok(i, ph_cockpit);
    }
    u->busy = ph_req;
    u->last_ok = ph_last_ok;
    strcpy_s(u->last_name, sizeof u->last_name, ph_last_name);
    u->last_w = ph_last_w;
    u->last_h = ph_last_h;
    u->last_ms = ph_last_ms;
    u->nshot = ph_nshot;
}

int sb_photo_request(void)
{
    if (!ph_on || g_cfg.vr_enable) { return 0; }
    ph_req = 1;
    HLOG_I("PHOTO: Aufnahme angefordert (f=%u)", g_present_no);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Ordner / Datei                                                      */
/* ------------------------------------------------------------------ */
int sb_photo_dir(char *out, size_t cap)
{
    if (!out || !cap) { return 0; }
    out[0] = 0;
    if (g_cfg.photo_dir[0]) {
        strcpy_s(out, cap, g_cfg.photo_dir);
        return 1;
    }
    if (sb_userdata_path_peek(SB_UD_SCREENSHOTS, out, cap) == 1) { return 1; }
    _snprintf_s(out, cap, _TRUNCATE, "%s\\Screenshots", hcfg_out_dir());
    return 1;
}

void sb_photo_dir_show(char *out, size_t cap)
{
    char d[600];
    const char *p, *q = NULL;
    sb_photo_dir(d, sizeof d);
    for (p = d; *p; p++) { if (*p == '\\' || *p == '/') { q = p; } }
    if (q) { /* vorletzten Trenner suchen */
        const char *r = NULL;
        for (p = d; p < q; p++) { if (*p == '\\' || *p == '/') { r = p; } }
        _snprintf_s(out, cap, _TRUNCATE, "%s%s", r ? "..." : "", r ? r : d);
    } else {
        strcpy_s(out, cap, d);
    }
}

static void ph_widen(const char *s, wchar_t *w, int cap)
{
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, cap) <= 0) {
        MultiByteToWideChar(CP_ACP, 0, s, -1, w, cap);
    }
}

static void ph_mkdirs(const wchar_t *path)
{
    wchar_t t[700];
    wchar_t *s;
    wcsncpy_s(t, 700, path, _TRUNCATE);
    for (s = t; *s; s++) {
        if ((*s == L'\\' || *s == L'/') && s > t && s[-1] != L':') {
            wchar_t c = *s;
            *s = 0;
            CreateDirectoryW(t, NULL);
            *s = c;
        }
    }
    CreateDirectoryW(t, NULL);
}

/* ------------------------------------------------------------------ */
/* WIC-Ausgabe in Streifen                                             */
/* ------------------------------------------------------------------ */
typedef struct {
    IWICImagingFactory *fac;
    IWICStream *st;
    IWICBitmapEncoder *enc;
    IWICBitmapFrameEncode *fr;
} ph_enc_t;

static void ph_enc_free(ph_enc_t *e)
{
    if (e->fr) { IWICBitmapFrameEncode_Release(e->fr); }
    if (e->enc) { IWICBitmapEncoder_Release(e->enc); }
    if (e->st) { IWICStream_Release(e->st); }
    if (e->fac) { IWICImagingFactory_Release(e->fac); }
    memset(e, 0, sizeof *e);
}

static int ph_enc_begin(ph_enc_t *e, const wchar_t *path, int jpg, unsigned w, unsigned h)
{
    IPropertyBag2 *props = NULL;
    WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
    HRESULT hr;
    memset(e, 0, sizeof *e);
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&e->fac);
    if (SUCCEEDED(hr)) { hr = IWICImagingFactory_CreateStream(e->fac, &e->st); }
    if (SUCCEEDED(hr)) { hr = IWICStream_InitializeFromFilename(e->st, path, GENERIC_WRITE); }
    if (SUCCEEDED(hr)) {
        hr = IWICImagingFactory_CreateEncoder(e->fac, jpg ? &GUID_ContainerFormatJpeg : &GUID_ContainerFormatPng, NULL, &e->enc);
    }
    if (SUCCEEDED(hr)) { hr = IWICBitmapEncoder_Initialize(e->enc, (IStream *)e->st, WICBitmapEncoderNoCache); }
    if (SUCCEEDED(hr)) { hr = IWICBitmapEncoder_CreateNewFrame(e->enc, &e->fr, &props); }
    if (SUCCEEDED(hr) && props && jpg) {
        PROPBAG2 opt;
        VARIANT v;
        memset(&opt, 0, sizeof opt);
        opt.pstrName = L"ImageQuality";
        VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = 0.95f;
        IPropertyBag2_Write(props, 1, &opt, &v);
    }
    if (SUCCEEDED(hr)) { hr = IWICBitmapFrameEncode_Initialize(e->fr, props); }
    if (props) { IPropertyBag2_Release(props); }
    if (SUCCEEDED(hr)) { hr = IWICBitmapFrameEncode_SetSize(e->fr, w, h); }
    if (SUCCEEDED(hr)) { hr = IWICBitmapFrameEncode_SetPixelFormat(e->fr, &pf); }
    if (FAILED(hr)) {
        HLOG_W("PHOTO: Bild-Encoder nicht bereit (hr=0x%08lX)", (unsigned long)hr);
        ph_enc_free(e);
        return 0;
    }
    return 1;
}

static int ph_enc_rows(ph_enc_t *e, unsigned rows, unsigned stride, BYTE *bgr)
{
    HRESULT hr = IWICBitmapFrameEncode_WritePixels(e->fr, rows, stride, stride * rows, bgr);
    if (FAILED(hr)) { HLOG_W("PHOTO: WritePixels hr=0x%08lX", (unsigned long)hr); }
    return SUCCEEDED(hr);
}

static int ph_enc_end(ph_enc_t *e)
{
    HRESULT hr = IWICBitmapFrameEncode_Commit(e->fr);
    if (SUCCEEDED(hr)) { hr = IWICBitmapEncoder_Commit(e->enc); }
    ph_enc_free(e);
    return SUCCEEDED(hr);
}

/* Textur-Kachel (mapped) -> Streifen (BGR), Spalten x0..x0+w */
static void ph_copy_rows(const D3D11_MAPPED_SUBRESOURCE *m, DXGI_FORMAT fmt, BYTE *strip, unsigned stride, unsigned x0,
                         unsigned w, unsigned h)
{
    unsigned x, y;
    int bgra = fmt == DXGI_FORMAT_B8G8R8A8_UNORM || fmt == DXGI_FORMAT_B8G8R8X8_UNORM;
    for (y = 0; y < h; y++) {
        const BYTE *s = (const BYTE *)m->pData + (size_t)y * m->RowPitch;
        BYTE *d = strip + (size_t)y * stride + (size_t)x0 * 3u;
        for (x = 0; x < w; x++) {
            d[x * 3 + 0] = s[x * 4 + (bgra ? 0 : 2)];
            d[x * 3 + 1] = s[x * 4 + 1];
            d[x * 3 + 2] = s[x * 4 + (bgra ? 2 : 0)];
        }
    }
}

/* ------------------------------------------------------------------ */
/* Foto-Wiedergabe                                                     */
/* ------------------------------------------------------------------ */
static void ph_census(void)
{
    if (ph_cen_tried) { return; }
    ph_cen_tried = 1;
    ph_cen_ok = sb_render_offscreen() && !g_cfg.vr_enable && sb_interp_photo_census(&ph_cen, 1);
    if (!ph_cen_ok) {
        HLOG_I("PHOTO: keine Aufzeichnung des Ticks (%s) - nur Cockpit-Bild x1",
               g_cfg.vr_enable ? "VR" : !sb_render_offscreen() ? "interp_fps=0" : "ausserhalb der Mission");
    } else {
        HLOG_I("PHOTO: Aussenwelt = Welt-RT e%d (Kamerabild des Hauptmonitors), Spiel-Viewport %ux%u im RT %ux%u, "
               "Monitor zeigt es %.4f:1 -> Foto-Seitenverhaeltnis %.4f (Spiel-Viewport %.4f, g_proj_game b/a %.4f), "
               "freie Kamera a=%.4f b=%.4f",
               ph_cen.wld, ph_cen.g_w, ph_cen.g_h, ph_cen.w_lw, ph_cen.w_lh, (double)ph_cen.disp_asp, (double)ph_aspect(),
               ph_cen.g_h ? (double)ph_cen.g_w / (double)ph_cen.g_h : 0.0,
               ph_cen.prj[0] > 0.0f ? (double)(ph_cen.prj[1] / ph_cen.prj[0]) : 0.0,
               (double)(ph_cen.prj[1] > 0.05f ? ph_cen.prj[1] / ph_aspect() : 0.0f), (double)ph_cen.prj[1]);
    }
}

static void ph_rep_base(sb_ph_rep_t *r)
{
    memset(r, 0, sizeof *r);
    r->tgt = r->tgtd = -1;
    r->wld = ph_cen.wld;
    r->cam = ph_free && (ph_yaw != 0.0f || ph_pitch != 0.0f || ph_t[0] != 0.0f || ph_t[1] != 0.0f || ph_t[2] != 0.0f);
    r->wcx = 1.0f;
    ph_cam_delta(r->cam_d);
    if (ph_cen.prj[1] > 0.05f && fabsf(ph_cen.prj[3]) > 1e-6f) { /* b, A, B aus dem Zensus; a = b / Seitenverhaeltnis */
        memcpy(r->def_prj, ph_cen.prj, sizeof r->def_prj);
        r->def_prj[0] = ph_cen.prj[1] / ph_aspect();
    } else { /* Spielkamera 45 Grad vertikal, 4:3, Near/Far 0,1/100 (vr.c) */
        r->def_prj[1] = 2.41421356f;
        r->def_prj[0] = r->def_prj[1] * 0.75f;
        r->def_prj[2] = 100.0f / (0.1f - 100.0f);
        r->def_prj[3] = 0.1f * 100.0f / (0.1f - 100.0f);
    }
}

/* Welt-RT -> Zielrechteck (dx,dy,dw,dh) abbilden (Kachel-Versatz tx,ty), Kachel tw x th */
static void ph_rep_world(sb_ph_rep_t *r, float dx, float dy, float dw, float dh, float fx, float tx, float ty, unsigned tw,
                         unsigned th)
{
    float sx = (float)ph_cen.w_tw / (float)(ph_cen.w_lw ? ph_cen.w_lw : 1);
    float sy = (float)ph_cen.w_th / (float)(ph_cen.w_lh ? ph_cen.w_lh : 1);
    float gx = (float)ph_cen.g_x * sx, gy = (float)ph_cen.g_y * sy;
    float gw = (float)ph_cen.g_w * sx, gh = (float)ph_cen.g_h * sy;
    float l = dx - tx, t = dy - ty;
    r->tgt = ph_cen.wld;
    r->tgtd = ph_cen.wldd;
    r->wcx = fx;
    r->kx = dw / gw;
    r->ky = dh / gh;
    r->ox = l - gx * r->kx;
    r->oy = t - gy * r->ky;
    r->cl[0] = (LONG)(l < 0.0f ? 0.0f : l);
    r->cl[1] = (LONG)(t < 0.0f ? 0.0f : t);
    r->cl[2] = (LONG)(l + dw > (float)tw ? (float)tw : l + dw);
    r->cl[3] = (LONG)(t + dh > (float)th ? (float)th : t + dh);
    if (r->cl[2] < r->cl[0]) { r->cl[2] = r->cl[0]; }
    if (r->cl[3] < r->cl[1]) { r->cl[3] = r->cl[1]; }
}

static DXGI_FORMAT ph_fmt_of(ID3D11Resource *res)
{
    D3D11_TEXTURE2D_DESC td;
    if (!res) { return DXGI_FORMAT_R8G8B8A8_UNORM; }
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)res, &td);
    return td.Format;
}

/* Vorschau neu rechnen (Fenstergroesse). 1 = ph_view gueltig. */
static int ph_preview(ID3D11Resource *still)
{
    sb_ph_rep_t r;
    sb_pl_info_t inf;
    ID3D11Resource *img;
    unsigned ncam = 0, nred = 0;
    long long q0 = rq_qpc(), q1;
    LARGE_INTEGER qf;
    float zero[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    if (!ph_ensure_view(ph_fmt_of(still))) { return 0; }
    ph_rep_base(&r);
    if (!ph_cockpit) {
        float bw = (float)g_bb_w, bh = (float)g_bb_h, dx, dy, dw, dh, fx;
        ph_world_layout(bw, bh, ph_asp, &dx, &dy, &dw, &dh, &fx); /* Fenster = Bildschirmformat; Kamera eingepasst */
        ID3D11DeviceContext_ClearRenderTargetView(g_ctx, ph_view_rtv, zero);
        ID3D11DeviceContext_ClearDepthStencilView(g_ctx, ph_view_dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        ph_rep_world(&r, dx, dy, dw, dh, fx, 0.0f, 0.0f, g_bb_w, g_bb_h);
        r.rtv = ph_view_rtv;
        r.dsv = ph_view_dsv;
        img = sb_interp_photo_render(&r, &inf, &ncam, &nred);
        if (!img) { return 0; }
    } else {
        img = sb_interp_photo_render(&r, &inf, &ncam, &nred);
        if (!img) { return 0; }
        ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)ph_view, img);
    }
    q1 = rq_qpc();
    QueryPerformanceFrequency(&qf);
    ph_nprev++;
    if (ph_nprev <= 3u || (ph_nprev % 30u) == 0u) {
        HLOG_I("PHOTO: Vorschau #%u (cockpit=%d frei=%d yaw=%.1f pitch=%.1f weg=(%.2f,%.2f,%.2f) welt-draws mit kamera=%u "
               "umgelenkt=%u, %.2f ms)",
               ph_nprev, ph_cockpit, ph_free, (double)ph_yaw, (double)ph_pitch, (double)ph_t[0], (double)ph_t[1],
               (double)ph_t[2], ncam, nred, (double)(q1 - q0) * 1000.0 / (double)qf.QuadPart);
    }
    return 1;
}

static unsigned ph_mission_no(int *inm)
{
    unsigned v = 0;
    *inm = rq_in_mission();
    __try { v = *(volatile unsigned short *)(uintptr_t)SBA(0x3B89A0u); }
    __except (EXCEPTION_EXECUTE_HANDLER) { v = 0; }
    return v + 1u;
}

/* Foto aufnehmen. 1 = geschrieben. */
static int ph_capture(ID3D11Resource *still)
{
    unsigned W, H, TW, TH, ty, tx, n = (unsigned)kPhScale[ph_scale];
    int cockpit = ph_cockpit, direct, ok = 0, inm;
    char dir[600], name[160], path8[800];
    wchar_t wdir[700], wpath[800];
    SYSTEMTIME lt;
    ph_enc_t enc;
    BYTE *strip = NULL;
    DXGI_FORMAT fmt;
    long long q0 = rq_qpc(), q1;
    LARGE_INTEGER qf;
    unsigned ntiles = 0, ncam_tot = 0, mno;
    sb_ph_rep_t sc_r;
    ph_init_t sc_pi;
    if (!ph_scale_ok(ph_scale, cockpit)) {
        HLOG_W("PHOTO: Stufe x%u hier nicht moeglich -> x1", n);
        ph_scale = 0;
        n = 1;
        if (!ph_scale_ok(0, cockpit)) { return 0; }
    }
    ph_out_size(ph_scale, cockpit, &W, &H);
    direct = cockpit && n == 1u && !(ph_free && ph_rec_ok()); /* Standbild direkt */
    fmt = direct ? ph_fmt_of(still) : cockpit ? ph_fmt_of((ID3D11Resource *)ph_cen.bb_tex) : ph_fmt_of((ID3D11Resource *)ph_cen.w_tex);
    if (fmt != DXGI_FORMAT_R8G8B8A8_UNORM && fmt != DXGI_FORMAT_B8G8R8A8_UNORM && fmt != DXGI_FORMAT_B8G8R8X8_UNORM) {
        HLOG_W("PHOTO: Bildformat %d nicht unterstuetzt", (int)fmt);
        return 0;
    }
    /* Datei */
    sb_photo_dir(dir, sizeof dir);
    ph_widen(dir, wdir, 700);
    ph_mkdirs(wdir);
    GetLocalTime(&lt);
    mno = ph_mission_no(&inm);
    {
        int k;
        for (k = 1; k < 100; k++) {
            char sfx[8];
            sfx[0] = 0;
            if (k > 1) { _snprintf_s(sfx, sizeof sfx, _TRUNCATE, "_%d", k); }
            if (inm) {
                _snprintf_s(name, sizeof name, _TRUNCATE, "Steelbound_%04u-%02u-%02u_%02u-%02u-%02u_M%02u%s.%s", lt.wYear,
                            lt.wMonth, lt.wDay, lt.wHour, lt.wMinute, lt.wSecond, mno, sfx, ph_jpg ? "jpg" : "png");
            } else {
                _snprintf_s(name, sizeof name, _TRUNCATE, "Steelbound_%04u-%02u-%02u_%02u-%02u-%02u_Menue%s.%s", lt.wYear,
                            lt.wMonth, lt.wDay, lt.wHour, lt.wMinute, lt.wSecond, sfx, ph_jpg ? "jpg" : "png");
            }
            _snprintf_s(path8, sizeof path8, _TRUNCATE, "%s\\%s", dir, name);
            ph_widen(path8, wpath, 800);
            if (GetFileAttributesW(wpath) == INVALID_FILE_ATTRIBUTES) { break; }
        }
    }
    if (direct) { TW = W; TH = H; } else { TW = W < PH_TW ? W : PH_TW; TH = H < PH_TH ? H : PH_TH; }
    strip = (BYTE *)malloc((size_t)W * 3u * (size_t)TH);
    if (!strip) {
        HLOG_W("PHOTO: kein Speicher fuer Streifen %ux%u", W, TH);
        return 0;
    }
    if (!ph_enc_begin(&enc, wpath, ph_jpg, W, H)) {
        free(strip);
        return 0;
    }
    if (!direct && !ph_ensure_tile(TW, TH, fmt)) {
        ph_enc_free(&enc);
        free(strip);
        return 0;
    }
    ok = 1;
    memset(&sc_r, 0, sizeof sc_r);
    memset(&sc_pi, 0, sizeof sc_pi);
    if (!direct && cockpit && n > 1u) { (void)ph_setup_sc(&sc_r, &sc_pi, n); } /* Monitore/Welt scharf */
    for (ty = 0; ty < H && ok; ty += TH) {
        unsigned th = H - ty < TH ? H - ty : TH;
        for (tx = 0; tx < W && ok; tx += TW) {
            unsigned tw = W - tx < TW ? W - tx : TW;
            D3D11_MAPPED_SUBRESOURCE m;
            ID3D11Texture2D *src = NULL, *stg = NULL;
            if (direct) {
                D3D11_TEXTURE2D_DESC td;
                ID3D11Texture2D_GetDesc((ID3D11Texture2D *)still, &td);
                td.Usage = D3D11_USAGE_STAGING;
                td.BindFlags = 0;
                td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                td.MiscFlags = 0;
                if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &stg))) { ok = 0; break; }
                ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)stg, still);
                src = stg;
            } else {
                sb_ph_rep_t r;
                sb_pl_info_t inf;
                ph_init_t pi;
                unsigned ncam = 0, nred = 0;
                memset(&pi, 0, sizeof pi);
                float zero[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
                D3D11_BOX bx;
                ph_rep_base(&r);
                ID3D11DeviceContext_ClearRenderTargetView(g_ctx, ph_tile_rtv, zero);
                ID3D11DeviceContext_ClearDepthStencilView(g_ctx, ph_tile_dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
                if (cockpit) {
                    r.tgt = ph_cen.bb;
                    r.tgtd = ph_cen.bbd;
                    r.kx = r.ky = (float)n;
                    r.ox = -(float)tx;
                    r.oy = -(float)ty;
                    r.cl[0] = 0; r.cl[1] = 0; r.cl[2] = (LONG)tw; r.cl[3] = (LONG)th;
                    pi.u0 = (float)tx / (float)W; pi.v0 = (float)ty / (float)H;
                    pi.u1 = (float)(tx + TW) / (float)W; pi.v1 = (float)(ty + TH) / (float)H;
                    pi.rtv = ph_tile_rtv; pi.w = TW; pi.h = TH;
                    {
                        int q;
                        pi.nsc = sc_pi.nsc;
                        r.nsc = sc_r.nsc;
                        for (q = 0; q < sc_r.nsc; q++) {
                            pi.sc_src[q] = sc_pi.sc_src[q]; pi.sc_rtv[q] = sc_pi.sc_rtv[q];
                            pi.sc_w[q] = sc_pi.sc_w[q]; pi.sc_h[q] = sc_pi.sc_h[q];
                            r.sc_e[q] = sc_r.sc_e[q]; r.sc_d[q] = sc_r.sc_d[q]; r.sc_rtv[q] = sc_r.sc_rtv[q];
                            r.sc_dsv[q] = sc_r.sc_dsv[q]; r.sc_srv[q] = sc_r.sc_srv[q]; r.sc_k[q] = sc_r.sc_k[q];
                            r.sc_w[q] = sc_r.sc_w[q]; r.sc_h[q] = sc_r.sc_h[q];
                        }
                    }
                    r.init = ph_init_cb;
                    r.ctx = &pi;
                } else {
                    float dx, dy, dw, dh, fx;
                    ph_world_layout((float)W, (float)H, ph_asp, &dx, &dy, &dw, &dh, &fx);
                    ph_rep_world(&r, dx, dy, dw, dh, fx, (float)tx, (float)ty, tw, th);
                }
                r.rtv = ph_tile_rtv;
                r.dsv = ph_tile_dsv;
                if (!sb_interp_photo_render(&r, &inf, &ncam, &nred)) { ok = 0; break; }
                ncam_tot += ncam;
                bx.left = 0; bx.top = 0; bx.front = 0; bx.right = tw; bx.bottom = th; bx.back = 1;
                ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)ph_stage, 0, 0, 0, 0,
                                                          (ID3D11Resource *)ph_tile, 0, &bx);
                src = ph_stage;
            }
            if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)src, 0, D3D11_MAP_READ, 0, &m))) {
                if (stg) { ID3D11Texture2D_Release(stg); }
                ok = 0;
                break;
            }
            ph_copy_rows(&m, fmt, strip, W * 3u, tx, tw, th);
            ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)src, 0);
            if (stg) { ID3D11Texture2D_Release(stg); }
            ntiles++;
        }
        if (ok && !ph_enc_rows(&enc, th, W * 3u, strip)) { ok = 0; }
    }
    free(strip);
    ph_free_sc(); /* grosse Zwischenziele sofort frei */
    if (ok) { ok = ph_enc_end(&enc); } else { ph_enc_free(&enc); DeleteFileW(wpath); }
    q1 = rq_qpc();
    QueryPerformanceFrequency(&qf);
    ph_last_ms = (double)(q1 - q0) * 1000.0 / (double)qf.QuadPart;
    if (ok) {
        strcpy_s(ph_last_name, sizeof ph_last_name, name);
        ph_last_w = W;
        ph_last_h = H;
        HLOG_I("PHOTO: gespeichert %s (%ux%u, x%u, %s, cockpit=%d bildformat=%d frei=%d yaw=%.1f pitch=%.1f weg=%.2f, %u Kacheln %ux%u, "
               "welt-draws mit kamera=%u, %.0f ms) -> %s",
               name, W, H, n, ph_jpg ? "JPG" : "PNG", cockpit, ph_asp, ph_free, (double)ph_yaw, (double)ph_pitch,
               (double)sqrtf(ph_t[0] * ph_t[0] + ph_t[1] * ph_t[1] + ph_t[2] * ph_t[2]), ntiles, TW, TH, ncam_tot,
               ph_last_ms, dir);
    } else {
        HLOG_W("PHOTO: Aufnahme fehlgeschlagen (%ux%u, x%u, nach %u Kacheln)", W, H, n, ntiles);
    }
    return ok;
}

ID3D11Resource *sb_photo_pause_image(ID3D11Resource *still, int *raw)
{
    unsigned h;
    if (raw) { *raw = 0; }
    if (!ph_on || !still || !g_dev) { return still; }
    ph_census();
    if (ph_req) {
        ph_req = 0;
        ph_last_ok = ph_capture(still) ? 1 : -1;
        ph_nshot++;
    }
    if (!ph_rec_ok() || (ph_cockpit && !ph_free)) {
        ph_view_valid = 0;
        return still;
    }
    h = ph_cfg_hash();
    if (ph_dirty || !ph_view_valid || h != ph_hash_last) {
        ph_dirty = 0;
        ph_hash_last = h;
        ph_view_valid = ph_preview(still);
    }
    if (!ph_view_valid) { return still; }
    if (raw) { *raw = !ph_cockpit; }
    return (ID3D11Resource *)ph_view;
}
