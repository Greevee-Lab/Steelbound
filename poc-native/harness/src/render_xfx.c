/* render_xfx.c - W240 FX-EXPERIMENTAL: zwei Spass-Darstellungen der Mission,
 * zur Laufzeit im ESC-Menue (Seite "Experimental"), Standard aus.
 *
 *  fx_wireframe  Drahtgitter: Rasterizer FILL_WIREFRAME nur fuer Welt-/Mech-
 *                Draws (3D, kein RHW, Nebel an = FOGENABLE des Spiels, wie die
 *                Aussenwelt-Klasse von W121). Cockpit (3D ohne Nebel), HUD/2D
 *                (RHW) und die Monitor-Flaechen (zeigen die Welt-RTs) bleiben
 *                gefuellt - der Hauptmonitor zeigt die Aussenwelt als Gitter.
 *                Umsetzung: der aktuell gebundene RS wird je Draw gegen eine
 *                Gitter-Variante (gleiche Werte, FillMode WIREFRAME, kleiner
 *                Cache) getauscht und nach dem Draw zurueckgestellt. Der Tausch
 *                liegt VOR der FPS-INTERP-Aufzeichnung (ip_snap liest den RS):
 *                Zwischenbilder zeichnen dieselben Draws ebenfalls als Gitter.
 *  fx_vaporwave  Vaporwave: ein Vollbild-Pass auf dem gezeigten Missionsbild
 *                (Kopie + Dreieck, wie W191 Panini): Farbverlauf nach Helligkeit
 *                (Helligkeit mit Gamma 0,6, damit dunkle Szenen wie die Halle Pink erreichen)
 *                Schwarz -> Lila -> Pink -> Cyan (60 % beigemischt, Schwarz
 *                bleibt schwarz), leichte Scanlines (je nach Hoehe 1..n Zeilen,
 *                15 % dunkler), leichte radiale chromatische Aberration. Laeuft
 *                an den Stellen der uebrigen Nachbearbeitung (vor Bildraendern,
 *                ESC-Menue und F2): ohne FPS-INTERP auf den Spiel-BB vor dem
 *                Screenshot, mit FPS-INTERP im Blit auf jedes gezeigte Bild
 *                (auch das Pause-Standbild -> sofort sichtbar im ESC-Menue).
 *
 * Beide nur in der Mission (sb_in_mission), nie in Menues/Filmen, nie in VR
 * (VR zeichnet je Auge neu und zeigt das Fenster-Bild nicht; Farbsaeume und
 * Scanlines in der Brille sind unangenehm - gesperrt statt halb wirksam).
 * Beide aus: kein Speicherzugriff, kein Zustandswechsel (bitgleich). */
#include "render_int.h"
#include "sb_profile.h" /* SBA() */
#include "sb_state.h"   /* sb_in_mission */

/* ------------------------------------------------------------------ */
/* Drahtgitter                                                          */
/* ------------------------------------------------------------------ */
#define XW_CACHE 8
static ID3D11RasterizerState *xw_src[XW_CACHE], *xw_wire[XW_CACHE];
static int xw_n, xw_fail;
static unsigned xw_frame = 0xFFFFFFFFu;
static int xw_frame_on;
static unsigned xw_ndraw, xw_nlog;

/* Bild-weites Gatter (einmal je Present ausgewertet) */
static int xw_active(void)
{
    if (!g_cfg.fx_wireframe || g_cfg.vr_enable || xw_fail) { return 0; }
    if (xw_frame != g_present_no) {
        xw_frame = g_present_no;
        xw_frame_on = sb_in_mission();
        if (xw_ndraw && xw_nlog < 6u) {
            xw_nlog++;
            HLOG_I("XFX: Drahtgitter %u Welt-Draws im Bild vor f=%u (RS-Varianten %d)", xw_ndraw, g_present_no, xw_n);
        }
        xw_ndraw = 0;
    }
    return xw_frame_on;
}

static ID3D11RasterizerState *xw_variant(ID3D11RasterizerState *src)
{
    D3D11_RASTERIZER_DESC rd;
    ID3D11RasterizerState *w = NULL;
    int i;
    for (i = 0; i < xw_n; i++) {
        if (xw_src[i] == src) { return xw_wire[i]; }
    }
    if (src) {
        ID3D11RasterizerState_GetDesc(src, &rd);
    } else { /* D3D11-Grundzustand */
        memset(&rd, 0, sizeof rd);
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_BACK;
        rd.DepthClipEnable = TRUE;
    }
    rd.FillMode = D3D11_FILL_WIREFRAME;
    if (FAILED(ID3D11Device_CreateRasterizerState(g_dev, &rd, &w)) || !w) {
        HLOG_E("XFX: Drahtgitter-RS gescheitert -> fx_wireframe wirkungslos");
        xw_fail = 1;
        return NULL;
    }
    if (xw_n >= XW_CACHE) { /* unerwartet viele Quell-Zustaende: Cache leeren */
        for (i = 0; i < xw_n; i++) {
            if (xw_src[i]) { ID3D11RasterizerState_Release(xw_src[i]); }
            ID3D11RasterizerState_Release(xw_wire[i]);
        }
        xw_n = 0;
    }
    if (src) { ID3D11RasterizerState_AddRef(src); } /* Schluessel bleibt gueltig (kein Zeiger-Recycling) */
    xw_src[xw_n] = src;
    xw_wire[xw_n] = w;
    xw_n++;
    HLOG_I("XFX: Drahtgitter-RS #%d (cull=%d clip=%d scissor=%d f=%u)", xw_n, (int)rd.CullMode,
           (int)rd.DepthClipEnable, (int)rd.ScissorEnable, g_present_no);
    return w;
}

/* Vor dem Draw (render_im.c, vor der FPS-INTERP-Aufzeichnung). 1 = getauscht,
 * *saved = vorheriger RS (sb_xfx_wire_end stellt ihn zurueck). */
int sb_xfx_wire_begin(int rhw, ID3D11RasterizerState **saved)
{
    ID3D11RasterizerState *cur = NULL, *w;
    unsigned fog = 0;
    *saved = NULL;
    if (rhw || !xw_active()) { return 0; }
    __try { fog = *(volatile unsigned *)(uintptr_t)SBA(0x21AC20u); } /* D3DRS_FOGENABLE (XDK-Lazy-Fog) */
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    if (!fog) { return 0; } /* Cockpit/Monitore zeichnen ohne Nebel */
    sb_ctx_enter();
    ID3D11DeviceContext_RSGetState(g_ctx, &cur);
    w = xw_variant(cur);
    if (!w) {
        if (cur) { ID3D11RasterizerState_Release(cur); }
        sb_ctx_leave();
        return 0;
    }
    ID3D11DeviceContext_RSSetState(g_ctx, w);
    sb_ctx_leave();
    *saved = cur; /* Referenz gehoert dem Aufrufer bis sb_xfx_wire_end */
    xw_ndraw++;
    return 1;
}

void sb_xfx_wire_end(ID3D11RasterizerState *saved)
{
    sb_ctx_enter();
    ID3D11DeviceContext_RSSetState(g_ctx, saved);
    sb_ctx_leave();
    if (saved) { ID3D11RasterizerState_Release(saved); }
}

/* ------------------------------------------------------------------ */
/* Vaporwave                                                            */
/* ------------------------------------------------------------------ */
static const char kVW_VS[] =
    "struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
    "O main(uint id : SV_VertexID) { O o; float2 t = float2((id << 1) & 2, id & 2);\n"
    "  o.uv = t; o.p = float4(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, 0.0, 1.0); return o; }\n";
/* p0.x = Scanline-Hoehe in Pixeln, p0.y = Aberration (UV), p0.z = Mischung, p0.w = Scanline-Abdunklung */
static const char kVW_PS[] =
    "cbuffer P : register(b0) { float4 p0; };\n"
    "Texture2D t0 : register(t0); SamplerState s0 : register(s0);\n"
    "float3 grad(float l) {\n"
    "  float3 c0 = float3(0.00, 0.00, 0.00), c1 = float3(0.26, 0.05, 0.46), c2 = float3(0.98, 0.32, 0.78),\n"
    "         c3 = float3(0.38, 0.92, 1.00), c4 = float3(0.92, 1.00, 1.00);\n"
    "  if (l < 0.25) return lerp(c0, c1, l / 0.25);\n"
    "  if (l < 0.55) return lerp(c1, c2, (l - 0.25) / 0.30);\n"
    "  if (l < 0.85) return lerp(c2, c3, (l - 0.55) / 0.30);\n"
    "  return lerp(c3, c4, saturate((l - 0.85) / 0.15)); }\n"
    "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
    "  float2 d = (uv - 0.5) * p0.y + float2(p0.y * 0.25, 0.0);\n"
    "  float3 c;\n"
    "  c.r = t0.SampleLevel(s0, uv + d, 0).r;\n"
    "  c.g = t0.SampleLevel(s0, uv, 0).g;\n"
    "  c.b = t0.SampleLevel(s0, uv - d, 0).b;\n"
    "  float l = dot(c, float3(0.299, 0.587, 0.114));\n"
    "  float3 o = lerp(c, grad(pow(max(l, 1e-5), 0.6)), p0.z);\n"
    "  if (fmod(floor(pos.y / p0.x), 2.0) >= 1.0) o *= 1.0 - p0.w;\n"
    "  return float4(o, 1.0); }\n";

static ID3D11VertexShader *vw_vs;
static ID3D11PixelShader *vw_ps;
static ID3D11Buffer *vw_cb;
static ID3D11SamplerState *vw_smp;
static ID3D11RasterizerState *vw_rs;
static ID3D11DepthStencilState *vw_ds;
static ID3D11Texture2D *vw_tex;
static ID3D11ShaderResourceView *vw_srv;
static unsigned vw_tw, vw_th;
static DXGI_FORMAT vw_fmt;
static int vw_state;
static unsigned vw_nlog;

int sb_xfx_vapor_on(void)
{
    return g_cfg.fx_vaporwave && !g_cfg.vr_enable && vw_state >= 0 && sb_in_mission();
}

static int vw_init(void)
{
    ID3DBlob *b = NULL;
    D3D11_BUFFER_DESC bd;
    D3D11_SAMPLER_DESC sd;
    D3D11_RASTERIZER_DESC rd;
    D3D11_DEPTH_STENCIL_DESC dsd;
    if (vw_state) { return vw_state > 0; }
    vw_state = -1;
    if (!g_dev) { return 0; }
    if (sb_im_compile(kVW_VS, "vs_4_0", &b)) {
        ID3D11Device_CreateVertexShader(g_dev, b->lpVtbl->GetBufferPointer(b), b->lpVtbl->GetBufferSize(b), NULL, &vw_vs);
        b->lpVtbl->Release(b);
        b = NULL;
    }
    if (sb_im_compile(kVW_PS, "ps_4_0", &b)) {
        ID3D11Device_CreatePixelShader(g_dev, b->lpVtbl->GetBufferPointer(b), b->lpVtbl->GetBufferSize(b), NULL, &vw_ps);
        b->lpVtbl->Release(b);
        b = NULL;
    }
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = 16;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &vw_cb))) { vw_cb = NULL; }
    memset(&sd, 0, sizeof sd);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(ID3D11Device_CreateSamplerState(g_dev, &sd, &vw_smp))) { vw_smp = NULL; }
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(ID3D11Device_CreateRasterizerState(g_dev, &rd, &vw_rs))) { vw_rs = NULL; }
    memset(&dsd, 0, sizeof dsd);
    dsd.DepthEnable = FALSE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dsd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    if (FAILED(ID3D11Device_CreateDepthStencilState(g_dev, &dsd, &vw_ds))) { vw_ds = NULL; }
    if (!vw_vs || !vw_ps || !vw_cb || !vw_smp || !vw_rs || !vw_ds) {
        HLOG_E("XFX: Vaporwave-Ressourcen gescheitert -> fx_vaporwave wirkungslos");
        return 0;
    }
    vw_state = 1;
    return 1;
}

static int vw_ensure_copy(ID3D11Resource *res)
{
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)res, &td);
    if (vw_tex && vw_tw == td.Width && vw_th == td.Height && vw_fmt == td.Format) { return 1; }
    if (vw_srv) { ID3D11ShaderResourceView_Release(vw_srv); vw_srv = NULL; }
    if (vw_tex) { ID3D11Texture2D_Release(vw_tex); vw_tex = NULL; }
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.SampleDesc.Quality = 0;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = 0;
    td.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &vw_tex))) { vw_tex = NULL; return 0; }
    if (FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)vw_tex, NULL, &vw_srv))) {
        ID3D11Texture2D_Release(vw_tex);
        vw_tex = NULL;
        vw_srv = NULL;
        return 0;
    }
    vw_tw = td.Width;
    vw_th = td.Height;
    vw_fmt = td.Format;
    return 1;
}

/* Vaporwave auf rtv/res (gezeigtes Bild, g_bb_w x g_bb_h). Unter ctx_lock. */
void sb_xfx_vapor_apply(ID3D11RenderTargetView *rtv, ID3D11Resource *res)
{
    float cb[4];
    unsigned line;
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
    if (!rtv || !res || !g_ctx || !g_bb_w || !g_bb_h) { return; }
    if (!vw_init() || !vw_ensure_copy(res)) { return; }
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)vw_tex, res);
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)vw_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) { return; }
    line = g_bb_h / 540u; /* Scanlines im Raster des Originals (540 = halbe PAL-Zeilen), mind. 1 Pixel */
    if (line < 1u) { line = 1u; }
    cb[0] = (float)line;
    cb[1] = 0.004f; /* Aberration: Saum am Rand ~0,2 % der Bildbreite */
    cb[2] = 0.60f;  /* Anteil des Farbverlaufs */
    cb[3] = 0.15f;  /* Scanline-Abdunklung */
    memcpy(map.pData, cb, sizeof cb);
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)vw_cb, 0);
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
    vp.Width = (float)vw_tw;
    vp.Height = (float)vw_th;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_OMSetBlendState(g_ctx, NULL, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, vw_ds, 0);
    ID3D11DeviceContext_RSSetState(g_ctx, vw_rs);
    ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &vp);
    ID3D11DeviceContext_IASetInputLayout(g_ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(g_ctx, vw_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_ctx, vw_ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, &vw_srv);
    ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 1, &vw_smp);
    ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 0, 1, &vw_cb);
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
    if (vw_nlog < 3u) {
        vw_nlog++;
        HLOG_I("XFX: Vaporwave-Pass %ux%u scanline=%u px f=%u", vw_tw, vw_th, line, g_present_no);
    }
}
