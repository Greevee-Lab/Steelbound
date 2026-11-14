/* render_cpanini.c - W191 UW-PERSP: Panini-Projektion der Cockpit-Ansicht als
 * Nachbearbeitung (Grafik-Option [render] cockpit_panini, Standard 0 = aus).
 *
 * Das Spiel zeichnet geradlinig (rektilinear). Die Panini-Abbildung (Sharpless
 * 2010) projiziert die Blickrichtungen erst auf einen Zylinder und den
 * Zylinder dann von einem Punkt im Abstand d hinter der Achse auf den
 * Schirm: senkrechte Linien bleiben senkrecht und gerade, die Bildmitte bleibt
 * formtreu (gleiche Skala in x und y), der Rand wird waagerecht verdichtet.
 * Der Massstab S ist so gewaehlt, dass die Bildraender fest bleiben (kein
 * Inhalt geht seitlich verloren, keine schwarzen Ecken); dafuer waechst die
 * Mitte um S = T(d + cos t)/((d + 1) sin t), t = halber Horizontalwinkel,
 * T = tan t: 16:9 und d = 1 -> 1,12, 21:9 -> 1,20, 32:9 -> 1,39. Oben und
 * unten in der Bildmitte wird um denselben Faktor beschnitten (Panini haelt
 * die Mitte formtreu - ein Bild, das die Mitte vergroessert UND den ganzen
 * Rahmen formtreu zeigt, gibt es nicht: Schwarz-Lemma).
 *
 * Umsetzung: Kopie des gezeigten Bildes, ein Vollbild-Dreieck mit der
 * inversen Abbildung (Schirm -> Quellbild), lineares Sampling. Laeuft an den
 * Stellen der Bildraender-Maske (vor der Maske, vor ESC-Menue/F2): ohne
 * FPS-INTERP auf den Spiel-BB vor dem Screenshot, mit FPS-INTERP im Blit
 * auf jedes gezeigte Bild (auch das Standbild der Pause -> live im ESC-Menue).
 * Alle Inhalte der Cockpit-Ansicht (Mesh, Monitore, HUD, 2D) laufen durch
 * dieselbe Abbildung und bleiben deckungsgleich. Nur Cockpit-Ansicht, nie VR.
 *
 * W235 COCKPIT-WIDTH: optional danach eine waagerechte Streckung um die
 * Bildmitte (Faktor W = 1 + cockpit_width %, p1.y = 1/W); laeuft auch mit
 * d = 0 (Panini-Abbildung dann exakt die Identitaet, S = 1). Inhalt jenseits
 * |x| > 1/W faellt seitlich heraus (Hor+-Rand), keine schwarzen Spalten.
 */
#include "render_int.h"
#include <math.h>

static const char kPN_VS[] =
    "struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
    "O main(uint id : SV_VertexID) { O o; float2 t = float2((id << 1) & 2, id & 2);\n"
    "  o.uv = t; o.p = float4(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, 0.0, 1.0); return o; }\n";
/* p0: T (tan halber Horizontalwinkel), b (vertikale Brennweite), d, S; p1.x: senkrechte Verschiebung (NDC),
 * p1.y: 1 / Breitenfaktor (W235, 1 = aus) */
static const char kPN_PS[] =
    "cbuffer P : register(b0) { float4 p0; float4 p1; };\n"
    "Texture2D t0 : register(t0); SamplerState s0 : register(s0);\n"
    "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
    "  float T = p0.x, b = p0.y, d = p0.z, S = p0.w;\n"
    "  float xs = (uv.x * 2.0 - 1.0) * p1.y * T, ys = ((1.0 - uv.y * 2.0) - p1.x) / b;\n"
    "  float k = xs / (S * (d + 1.0)), k2 = k * k;\n"
    "  float c = (-k2 * d + sqrt(max(1.0 + k2 * (1.0 - d * d), 0.0))) / (1.0 + k2);\n"
    "  float tx = k * (d + c) / c;\n"
    "  float ty = ys * (d + c) / (S * (d + 1.0)) / c;\n"
    "  float2 suv = float2(tx / T * 0.5 + 0.5, 0.5 - ty * b * 0.5);\n"
    "  if (any(suv < 0.0) || any(suv > 1.0)) return float4(0.0, 0.0, 0.0, 1.0);\n"
    "  return float4(t0.SampleLevel(s0, suv, 0).rgb, 1.0); }\n";

static ID3D11VertexShader *pn_vs;
static ID3D11PixelShader *pn_ps;
static ID3D11Buffer *pn_cb;
static ID3D11SamplerState *pn_smp;
static ID3D11RasterizerState *pn_rs;
static ID3D11DepthStencilState *pn_ds;
static ID3D11Texture2D *pn_tex;
static ID3D11ShaderResourceView *pn_srv;
static unsigned pn_tw, pn_th;
static DXGI_FORMAT pn_fmt;
static int pn_state;

static int pn_init(void)
{
    ID3DBlob *b = NULL;
    D3D11_BUFFER_DESC bd;
    D3D11_SAMPLER_DESC sd;
    D3D11_RASTERIZER_DESC rd;
    D3D11_DEPTH_STENCIL_DESC dsd;
    if (pn_state) { return pn_state > 0; }
    pn_state = -1;
    if (!g_dev) { return 0; }
    if (sb_im_compile(kPN_VS, "vs_4_0", &b)) {
        ID3D11Device_CreateVertexShader(g_dev, b->lpVtbl->GetBufferPointer(b), b->lpVtbl->GetBufferSize(b), NULL, &pn_vs);
        b->lpVtbl->Release(b);
        b = NULL;
    }
    if (sb_im_compile(kPN_PS, "ps_4_0", &b)) {
        ID3D11Device_CreatePixelShader(g_dev, b->lpVtbl->GetBufferPointer(b), b->lpVtbl->GetBufferSize(b), NULL, &pn_ps);
        b->lpVtbl->Release(b);
        b = NULL;
    }
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = 32;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &pn_cb))) { pn_cb = NULL; }
    memset(&sd, 0, sizeof sd);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(ID3D11Device_CreateSamplerState(g_dev, &sd, &pn_smp))) { pn_smp = NULL; }
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(ID3D11Device_CreateRasterizerState(g_dev, &rd, &pn_rs))) { pn_rs = NULL; }
    memset(&dsd, 0, sizeof dsd);
    dsd.DepthEnable = FALSE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dsd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    if (FAILED(ID3D11Device_CreateDepthStencilState(g_dev, &dsd, &pn_ds))) { pn_ds = NULL; }
    if (!pn_vs || !pn_ps || !pn_cb || !pn_smp || !pn_rs || !pn_ds) {
        HLOG_E("CPANINI: Ressourcen gescheitert -> cockpit_panini wirkungslos");
        return 0;
    }
    pn_state = 1;
    return 1;
}

static int pn_ensure_copy(ID3D11Resource *res)
{
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)res, &td);
    if (pn_tex && pn_tw == td.Width && pn_th == td.Height && pn_fmt == td.Format) { return 1; }
    if (pn_srv) { ID3D11ShaderResourceView_Release(pn_srv); pn_srv = NULL; }
    if (pn_tex) { ID3D11Texture2D_Release(pn_tex); pn_tex = NULL; }
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.SampleDesc.Quality = 0;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = 0;
    td.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &pn_tex))) { pn_tex = NULL; return 0; }
    if (FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)pn_tex, NULL, &pn_srv))) {
        ID3D11Texture2D_Release(pn_tex);
        pn_tex = NULL;
        pn_srv = NULL;
        return 0;
    }
    pn_tw = td.Width;
    pn_th = td.Height;
    pn_fmt = td.Format;
    return 1;
}

/* Mitte-Vergroesserung S der Panini-Abbildung bei d (Raender fest). */
float sb_cp_panini_scale(float d)
{
    float b = g_proj_game.m[5], T, th;
    if (!(d > 0.0f) || !g_bb_w || !g_bb_h) { return 1.0f; }
    if (!(b > 0.3f && b < 20.0f)) { b = 2.41421356f; }
    T = ((float)g_bb_w / (float)g_bb_h) / b;
    th = atanf(T);
    return T * (d + cosf(th)) / ((d + 1.0f) * sinf(th));
}

/* Panini-Abbildung (d > 0) und/oder W235-Breite (sb_cp_width() > 1) auf rtv/res
 * (Groesse g_bb_w x g_bb_h). Unter ctx_lock. */
void sb_cp_panini_apply(ID3D11RenderTargetView *rtv, ID3D11Resource *res, float d)
{
    float b = g_proj_game.m[5], T, th, S, cb[8], wf = sb_cp_width();
    D3D11_MAPPED_SUBRESOURCE map;
    D3D11_VIEWPORT vp, s_vp;
    UINT s_nvp = 1, s_mask = 0, s_sref = 0;
    ID3D11RenderTargetView *s_rtv = NULL;
    ID3D11DepthStencilView *s_dsv = NULL;
    ID3D11BlendState *s_bs = NULL;
    FLOAT s_bf[4];
    ID3D11DepthStencilState *s_ds = NULL;
    ID3D11RasterizerState *s_rs = NULL;
    ID3D11InputLayout *s_lay = NULL;
    D3D11_PRIMITIVE_TOPOLOGY s_topo;
    ID3D11VertexShader *s_vs = NULL;
    ID3D11PixelShader *s_ps = NULL;
    ID3D11ShaderResourceView *s_srv = NULL, *nul = NULL;
    ID3D11SamplerState *s_smp = NULL;
    ID3D11Buffer *s_cb = NULL;
    if (!(d > 0.0f)) { d = 0.0f; }
    if ((d == 0.0f && !(wf > 1.0f)) || !rtv || !res || !g_ctx || !g_bb_w || !g_bb_h) { return; }
    if (!(wf >= 1.0f)) { wf = 1.0f; }
    if (!pn_init() || !pn_ensure_copy(res)) { return; }
    if (!(b > 0.3f && b < 20.0f)) { b = 2.41421356f; }
    T = ((float)g_bb_w / (float)g_bb_h) / b;
    th = atanf(T);
    S = d > 0.0f ? T * (d + cosf(th)) / ((d + 1.0f) * sinf(th)) : 1.0f; /* d = 0: Identitaet (nur Breite) */
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)pn_tex, res);
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)pn_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) { return; }
    cb[0] = T;
    cb[1] = b;
    cb[2] = d;
    cb[3] = S;
    cb[4] = d > 0.0f ? sb_cp_panini_offset() : 0.0f;
    cb[5] = 1.0f / wf; /* W235: Breite (1 = aus, bitgleich zum W191-Pass) */
    cb[6] = cb[7] = 0.0f;
    memcpy(map.pData, cb, sizeof cb);
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)pn_cb, 0);
    /* Zustand sichern */
    ID3D11DeviceContext_OMGetRenderTargets(g_ctx, 1, &s_rtv, &s_dsv);
    ID3D11DeviceContext_OMGetBlendState(g_ctx, &s_bs, s_bf, &s_mask);
    ID3D11DeviceContext_OMGetDepthStencilState(g_ctx, &s_ds, &s_sref);
    ID3D11DeviceContext_RSGetState(g_ctx, &s_rs);
    ID3D11DeviceContext_RSGetViewports(g_ctx, &s_nvp, &s_vp);
    ID3D11DeviceContext_IAGetInputLayout(g_ctx, &s_lay);
    ID3D11DeviceContext_IAGetPrimitiveTopology(g_ctx, &s_topo);
    ID3D11DeviceContext_VSGetShader(g_ctx, &s_vs, NULL, NULL);
    ID3D11DeviceContext_PSGetShader(g_ctx, &s_ps, NULL, NULL);
    ID3D11DeviceContext_PSGetShaderResources(g_ctx, 0, 1, &s_srv);
    ID3D11DeviceContext_PSGetSamplers(g_ctx, 0, 1, &s_smp);
    ID3D11DeviceContext_PSGetConstantBuffers(g_ctx, 0, 1, &s_cb);
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width = (float)g_bb_w;
    vp.Height = (float)g_bb_h;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_OMSetBlendState(g_ctx, NULL, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, pn_ds, 0);
    ID3D11DeviceContext_RSSetState(g_ctx, pn_rs);
    ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &vp);
    ID3D11DeviceContext_IASetInputLayout(g_ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(g_ctx, pn_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_ctx, pn_ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, &pn_srv);
    ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 1, &pn_smp);
    ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 0, 1, &pn_cb);
    ID3D11DeviceContext_Draw(g_ctx, 3, 0);
    /* Zustand zurueck */
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, &nul);
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &s_rtv, s_dsv);
    ID3D11DeviceContext_OMSetBlendState(g_ctx, s_bs, s_bf, s_mask);
    ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, s_ds, s_sref);
    ID3D11DeviceContext_RSSetState(g_ctx, s_rs);
    if (s_nvp) { ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &s_vp); }
    ID3D11DeviceContext_IASetInputLayout(g_ctx, s_lay);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, s_topo);
    ID3D11DeviceContext_VSSetShader(g_ctx, s_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_ctx, s_ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, &s_srv);
    ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 1, &s_smp);
    ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 0, 1, &s_cb);
    if (s_rtv) { ID3D11RenderTargetView_Release(s_rtv); }
    if (s_dsv) { ID3D11DepthStencilView_Release(s_dsv); }
    if (s_bs) { ID3D11BlendState_Release(s_bs); }
    if (s_ds) { ID3D11DepthStencilState_Release(s_ds); }
    if (s_rs) { ID3D11RasterizerState_Release(s_rs); }
    if (s_lay) { ID3D11InputLayout_Release(s_lay); }
    if (s_vs) { ID3D11VertexShader_Release(s_vs); }
    if (s_ps) { ID3D11PixelShader_Release(s_ps); }
    if (s_srv) { ID3D11ShaderResourceView_Release(s_srv); }
    if (s_smp) { ID3D11SamplerState_Release(s_smp); }
    if (s_cb) { ID3D11Buffer_Release(s_cb); }
    {
        static int lk = -1, lwk = -1;
        static unsigned lw;
        int k = (int)(d * 1000.0f + 0.5f), wk = (int)(wf * 100.0f + 0.5f);
        static unsigned nlog;
        if ((k != lk || wk != lwk || lw != g_bb_w) && (!g_cfg.cockpit_persp_cycle[0] || nlog < 8u)) {
            nlog++;
            lk = k;
            lwk = wk;
            lw = g_bb_w;
            if (d > 0.0f) {
                HLOG_I("CPANINI d=%.3f bb=%ux%u halber Horizontalwinkel=%.1f Grad -> Mitte x%.3f (Raender fest) senkrecht %+.3f rahmen=%d f=%u",
                       d, g_bb_w, g_bb_h, th * 57.2958f, S, cb[4], g_cfg.cockpit_panini_frame, g_present_no);
            }
            if (wf > 1.0f) { /* W235 */
                HLOG_I("CWIDTH breite x%.2f bb=%ux%u panini d=%.3f: Inhalt |x| > %.3f (%.2f halbe 4:3-Breiten) faellt seitlich heraus f=%u",
                       wf, g_bb_w, g_bb_h, d, 1.0f / wf, (float)g_bb_w / (float)g_bb_h * 0.75f / wf, g_present_no);
            }
        }
    }
}
