/* render_edge.c - W107 EDGE-MODE: Bildraender bei Breitbild (Hor+).
 *
 * Das Spiel ist fuer 4:3 gebaut; im Hor+-Breitbild endet das Cockpit-Modell
 * seitlich und geht "ins Leere". [render] edge_mode waehlt, was ausserhalb
 * der 4:3-Spielbox (sb_aspect_box, dieselbe Box wie 2D/HUD/Filme) sichtbar
 * bleibt:
 *   0 = Aus (volles Hor+, bisher; nichts wird gezeichnet)
 *   1 = Harter Schnitt: ausserhalb der Box exakt schwarz (Pillarbox auch 3D)
 *   2 = Abdunklung: ab der Boxkante Verlauf ins Schwarze (smoothstep) ueber
 *       edge_fade_width % der Boxbreite, Endabdunklung edge_fade_strength %.
 * Die Box selbst (Welt, Cockpit-Mitte, HUD, Monitore, 2D) bleibt bitgleich:
 * gezeichnet werden nur Rechtecke ausserhalb der Box, die das vorhandene
 * Bild per Blend (Ziel * Faktor) abdunkeln. Kein Strecken, kein Resampling.
 *
 * Aufrufstellen (d3d11_render.c): ohne FPS-INTERP im Present direkt vor dem
 * Screenshot auf den Backbuffer; mit FPS-INTERP (Offscreen-Spiel-BB) erst im
 * Swapchain-BB nach der Kopie (sb_render_blit) - die Maske liegt damit NACH
 * Aufzeichnung, Wiedergabe und interp_verify (exakte Wiedergabe vs.
 * Spiel-Bild vergleicht weiter die unmaskierten Bilder) und gilt fuer jedes
 * gezeigte Bild (Spiel-Bild und Zwischenbilder). Eigene Shader/States/VB;
 * der gesamte beruehrte Context-Zustand wird gesichert und wiederhergestellt
 * (der Interp-Takt praesentiert auch mitten im Spiel-Tick).
 */
#include "render_int.h"

#define EG_BPTR(b) ((b)->lpVtbl->GetBufferPointer(b))
#define EG_BSIZE(b) ((b)->lpVtbl->GetBufferSize(b))
#define EG_BREL(b) ((b)->lpVtbl->Release(b))

typedef struct {
    float pos[2]; /* NDC */
    float box[4]; /* Box in BB-Pixeln: x0, x1, y0, y1 */
    float par[2]; /* Verlaufsbreite (px, 0 = harter Schnitt), Staerke 0..1 */
} eg_vtx;

static const char kEG_VS[] =
    "struct I { float2 p : POSITION; float4 b : TEXCOORD0; float2 q : TEXCOORD1; };\n"
    "struct O { float4 p : SV_Position; nointerpolation float4 b : TEXCOORD0;\n"
    "           nointerpolation float2 q : TEXCOORD1; };\n"
    "O main(I i) { O o; o.p = float4(i.p, 0.0, 1.0); o.b = i.b; o.q = i.q; return o; }\n";

/* Faktor = 1 - Staerke * smoothstep(0, Breite, Abstand zur Box). Alpha ist
 * der Helligkeitsfaktor (Blend: Ziel * SrcAlpha). Harter Schnitt: Breite 0,
 * Staerke 1 -> Faktor exakt 0. */
static const char kEG_PS[] =
    "struct O { float4 p : SV_Position; nointerpolation float4 b : TEXCOORD0;\n"
    "           nointerpolation float2 q : TEXCOORD1; };\n"
    "float4 main(O i) : SV_Target {\n"
    "  float dx = max(max(i.b.x - i.p.x, i.p.x - i.b.y), 0.0);\n"
    "  float dy = max(max(i.b.z - i.p.y, i.p.y - i.b.w), 0.0);\n"
    "  float d = max(dx, dy);\n"
    "  float u = (i.q.x > 0.0) ? saturate(d / i.q.x) : ((d > 0.0) ? 1.0 : 0.0);\n"
    "  u = u * u * (3.0 - 2.0 * u);\n"
    "  return float4(0.0, 0.0, 0.0, 1.0 - i.q.y * u);\n"
    "}\n";

static ID3D11VertexShader *eg_vs;
static ID3D11PixelShader *eg_ps;
static ID3D11InputLayout *eg_lay;
static ID3D11Buffer *eg_vb;
static ID3D11BlendState *eg_bs;
static ID3D11RasterizerState *eg_rs;
static ID3D11DepthStencilState *eg_ds;
static int eg_state; /* 0 = noch nicht, 1 = bereit, -1 = gescheitert (Maske aus) */
static unsigned eg_n_apply;

#define EG_VMAX 24

static int eg_init(void)
{
    ID3DBlob *vsb = NULL, *psb = NULL;
    D3D11_INPUT_ELEMENT_DESC ly[3];
    D3D11_BUFFER_DESC bd;
    D3D11_BLEND_DESC bld;
    D3D11_RASTERIZER_DESC rd;
    D3D11_DEPTH_STENCIL_DESC dsd;
    HRESULT hr;
    if (eg_state) { return eg_state > 0; }
    eg_state = -1;
    if (!g_dev) { return 0; }
    if (!sb_im_compile(kEG_VS, "vs_4_0", &vsb) || !sb_im_compile(kEG_PS, "ps_4_0", &psb)) {
        HLOG_E("EDGE: Shader-Uebersetzung gescheitert -> edge_mode wirkungslos");
        if (vsb) { EG_BREL(vsb); }
        return 0;
    }
    hr = ID3D11Device_CreateVertexShader(g_dev, EG_BPTR(vsb), EG_BSIZE(vsb), NULL, &eg_vs);
    memset(ly, 0, sizeof ly);
    ly[0].SemanticName = "POSITION";
    ly[0].Format = DXGI_FORMAT_R32G32_FLOAT;
    ly[0].AlignedByteOffset = 0;
    ly[1].SemanticName = "TEXCOORD";
    ly[1].SemanticIndex = 0;
    ly[1].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    ly[1].AlignedByteOffset = 8;
    ly[2].SemanticName = "TEXCOORD";
    ly[2].SemanticIndex = 1;
    ly[2].Format = DXGI_FORMAT_R32G32_FLOAT;
    ly[2].AlignedByteOffset = 24;
    if (SUCCEEDED(hr)) {
        hr = ID3D11Device_CreateInputLayout(g_dev, ly, 3, EG_BPTR(vsb), EG_BSIZE(vsb), &eg_lay);
    }
    EG_BREL(vsb);
    if (SUCCEEDED(hr)) {
        hr = ID3D11Device_CreatePixelShader(g_dev, EG_BPTR(psb), EG_BSIZE(psb), NULL, &eg_ps);
    }
    EG_BREL(psb);
    if (SUCCEEDED(hr)) {
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = sizeof(eg_vtx) * EG_VMAX;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &eg_vb);
    }
    if (SUCCEEDED(hr)) {
        /* Ziel * SrcAlpha (Farbe), Ziel-Alpha bleibt. */
        memset(&bld, 0, sizeof bld);
        bld.RenderTarget[0].BlendEnable = TRUE;
        bld.RenderTarget[0].SrcBlend = D3D11_BLEND_ZERO;
        bld.RenderTarget[0].DestBlend = D3D11_BLEND_SRC_ALPHA;
        bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
        bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        hr = ID3D11Device_CreateBlendState(g_dev, &bld, &eg_bs);
    }
    if (SUCCEEDED(hr)) {
        memset(&rd, 0, sizeof rd);
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        hr = ID3D11Device_CreateRasterizerState(g_dev, &rd, &eg_rs);
    }
    if (SUCCEEDED(hr)) {
        memset(&dsd, 0, sizeof dsd);
        dsd.DepthEnable = FALSE;
        dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        dsd.DepthFunc = D3D11_COMPARISON_ALWAYS;
        dsd.StencilEnable = FALSE;
        hr = ID3D11Device_CreateDepthStencilState(g_dev, &dsd, &eg_ds);
    }
    if (FAILED(hr)) {
        HLOG_E("EDGE: Ressourcen gescheitert (hr=0x%08lX) -> edge_mode wirkungslos", (unsigned long)hr);
        return 0;
    }
    eg_state = 1;
    return 1;
}

/* Maske (mode 1/2) auf rtv (Groesse g_bb_w x g_bb_h) zeichnen; quiet = ohne
 * Log (A/B-Bilder). Aufruf unter ctx_lock. */
static void eg_apply(ID3D11RenderTargetView *rtv, int mode, int quiet)
{
    unsigned bx, by, bw, bh, nv = 0, i;
    eg_vtx v[EG_VMAX];
    float wpx, str;
    D3D11_MAPPED_SUBRESOURCE map;
    D3D11_VIEWPORT vp;
    UINT stride = sizeof(eg_vtx), off = 0;
    /* gesicherter Zustand */
    ID3D11RenderTargetView *s_rtv = NULL;
    ID3D11DepthStencilView *s_dsv = NULL;
    ID3D11BlendState *s_bs = NULL;
    FLOAT s_bf[4];
    UINT s_mask = 0, s_sref = 0, s_nvp = 1, s_vbs = 0, s_vbo = 0;
    ID3D11DepthStencilState *s_ds = NULL;
    ID3D11RasterizerState *s_rs = NULL;
    D3D11_VIEWPORT s_vp;
    ID3D11InputLayout *s_lay = NULL;
    ID3D11Buffer *s_vb = NULL;
    D3D11_PRIMITIVE_TOPOLOGY s_topo;
    ID3D11VertexShader *s_vs = NULL;
    ID3D11PixelShader *s_ps = NULL;

    if (mode <= 0 || !rtv || !g_ctx) { return; }
    if (!eg_init()) { return; }
    sb_aspect_box(&bx, &by, &bw, &bh);
    sb_cp_edge_box(&bx, &by, &bw, &bh); /* W191 UW-PERSP: Rand beginnt an der verschobenen Cockpitkante (aus: unveraendert) */
    if (bw >= g_bb_w && bh >= g_bb_h) { return; } /* 4:3-Ausgabe: nichts ausserhalb */
    if (mode == 1) {
        wpx = 0.0f;
        str = 1.0f;
    } else {
        int pw = g_cfg.edge_fade_width, ps = g_cfg.edge_fade_strength;
        if (pw < 0) { pw = 0; }
        if (pw > 100) { pw = 100; }
        if (ps < 0) { ps = 0; }
        if (ps > 100) { ps = 100; }
        /* Breite relativ zur Boxbreite (Pillarbox) bzw. Boxhoehe (Letterbox):
         * gleiche Wirkung in jeder Aufloesung/jedem Fenster. */
        wpx = (float)pw / 100.0f * (float)((bw < g_bb_w) ? bw : bh);
        str = (float)ps / 100.0f;
        if (str <= 0.0f) { return; }
    }
    for (i = 0; i < 4; i++) {
        unsigned x0, y0, x1, y1, r;
        float nx0, ny0, nx1, ny1;
        static const float cx[6] = { 0, 1, 1, 0, 1, 0 };
        static const float cy[6] = { 0, 0, 1, 0, 1, 1 };
        if (i == 0) { x0 = 0; x1 = bx; y0 = 0; y1 = g_bb_h; }
        else if (i == 1) { x0 = bx + bw; x1 = g_bb_w; y0 = 0; y1 = g_bb_h; }
        else if (i == 2) { x0 = bx; x1 = bx + bw; y0 = 0; y1 = by; }
        else { x0 = bx; x1 = bx + bw; y0 = by + bh; y1 = g_bb_h; }
        if (x1 <= x0 || y1 <= y0) { continue; }
        nx0 = (float)x0 / (float)g_bb_w * 2.0f - 1.0f;
        nx1 = (float)x1 / (float)g_bb_w * 2.0f - 1.0f;
        ny0 = 1.0f - (float)y0 / (float)g_bb_h * 2.0f;
        ny1 = 1.0f - (float)y1 / (float)g_bb_h * 2.0f;
        for (r = 0; r < 6; r++) {
            eg_vtx *p = &v[nv++];
            p->pos[0] = cx[r] ? nx1 : nx0;
            p->pos[1] = cy[r] ? ny1 : ny0;
            p->box[0] = (float)bx;
            p->box[1] = (float)(bx + bw);
            p->box[2] = (float)by;
            p->box[3] = (float)(by + bh);
            p->par[0] = wpx;
            p->par[1] = str;
        }
    }
    if (!nv) { return; }
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)eg_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        return;
    }
    memcpy(map.pData, v, nv * sizeof v[0]);
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)eg_vb, 0);

    /* Zustand sichern */
    ID3D11DeviceContext_OMGetRenderTargets(g_ctx, 1, &s_rtv, &s_dsv);
    ID3D11DeviceContext_OMGetBlendState(g_ctx, &s_bs, s_bf, &s_mask);
    ID3D11DeviceContext_OMGetDepthStencilState(g_ctx, &s_ds, &s_sref);
    ID3D11DeviceContext_RSGetState(g_ctx, &s_rs);
    ID3D11DeviceContext_RSGetViewports(g_ctx, &s_nvp, &s_vp);
    ID3D11DeviceContext_IAGetInputLayout(g_ctx, &s_lay);
    ID3D11DeviceContext_IAGetVertexBuffers(g_ctx, 0, 1, &s_vb, &s_vbs, &s_vbo);
    ID3D11DeviceContext_IAGetPrimitiveTopology(g_ctx, &s_topo);
    ID3D11DeviceContext_VSGetShader(g_ctx, &s_vs, NULL, NULL);
    ID3D11DeviceContext_PSGetShader(g_ctx, &s_ps, NULL, NULL);

    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width = (float)g_bb_w;
    vp.Height = (float)g_bb_h;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_OMSetBlendState(g_ctx, eg_bs, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, eg_ds, 0);
    ID3D11DeviceContext_RSSetState(g_ctx, eg_rs);
    ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &vp);
    ID3D11DeviceContext_IASetInputLayout(g_ctx, eg_lay);
    ID3D11DeviceContext_IASetVertexBuffers(g_ctx, 0, 1, &eg_vb, &stride, &off);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(g_ctx, eg_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_ctx, eg_ps, NULL, 0);
    ID3D11DeviceContext_Draw(g_ctx, nv, 0);

    /* Zustand wiederherstellen */
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &s_rtv, s_dsv);
    ID3D11DeviceContext_OMSetBlendState(g_ctx, s_bs, s_bf, s_mask);
    ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, s_ds, s_sref);
    ID3D11DeviceContext_RSSetState(g_ctx, s_rs);
    if (s_nvp) { ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &s_vp); }
    ID3D11DeviceContext_IASetInputLayout(g_ctx, s_lay);
    ID3D11DeviceContext_IASetVertexBuffers(g_ctx, 0, 1, &s_vb, &s_vbs, &s_vbo);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, s_topo);
    ID3D11DeviceContext_VSSetShader(g_ctx, s_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_ctx, s_ps, NULL, 0);
    if (s_rtv) { ID3D11RenderTargetView_Release(s_rtv); }
    if (s_dsv) { ID3D11DepthStencilView_Release(s_dsv); }
    if (s_bs) { ID3D11BlendState_Release(s_bs); }
    if (s_ds) { ID3D11DepthStencilState_Release(s_ds); }
    if (s_rs) { ID3D11RasterizerState_Release(s_rs); }
    if (s_lay) { ID3D11InputLayout_Release(s_lay); }
    if (s_vb) { ID3D11Buffer_Release(s_vb); }
    if (s_vs) { ID3D11VertexShader_Release(s_vs); }
    if (s_ps) { ID3D11PixelShader_Release(s_ps); }

    if (quiet) { return; }
    eg_n_apply++;
    { /* Log: erste Anwendung und jede Aenderung von Box/Parametern (Resize, Fenstermodus) */
        static unsigned lk[6];
        static int have;
        unsigned k[6];
        k[0] = bx; k[1] = bw; k[2] = by; k[3] = bh;
        k[4] = (unsigned)(wpx + 0.5f); k[5] = (unsigned)(str * 100.0f + 0.5f);
        if (!have || memcmp(k, lk, sizeof k) != 0) {
            have = 1;
            memcpy(lk, k, sizeof k);
            HLOG_I("EDGE mode=%d (%s) bb=%ux%u box=%ux%u@%u,%u fade=%upx staerke=%u%% f=%u n=%u",
                   mode, mode == 1 ? "harter Schnitt" : "Abdunklung", g_bb_w,
                   g_bb_h, bw, bh, bx, by, k[4], k[5], g_present_no, eg_n_apply);
        }
    }
}

void sb_edge_apply(ID3D11RenderTargetView *rtv)
{
    eg_apply(rtv, g_cfg.edge_mode, 0);
}

/* DIAG(EDGE-MODE,W107): edge_ab=1 - zu jedem Screenshot des Spiel-BB (und
 * jedem Spiel-Bild einer pf_ring-Folge) zusaetzlich <base>_e1 (harter
 * Schnitt) und <base>_e2 (Abdunklung mit edge_fade_width/strength) aus
 * DEMSELBEN Bild (Kopie, Maske auf die Kopie): A/B/C-Vergleich ohne
 * Szenenunterschied. Das Spiel-Bild bleibt unberuehrt. src = NULL: Spiel-BB.
 * Aufruf unter ctx_lock. */
void sb_edge_ab_shots(const char *base, ID3D11Resource *src)
{
    ID3D11Resource *bb = NULL;
    ID3D11Texture2D *tmp = NULL;
    ID3D11RenderTargetView *trtv = NULL;
    D3D11_TEXTURE2D_DESC dd;
    int m;
    if (!g_rtv || !g_dev) { return; }
    if (src) { bb = src; ID3D11Resource_AddRef(bb); }
    else { ID3D11RenderTargetView_GetResource(g_rtv, &bb); }
    if (!bb) { return; }
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)bb, &dd);
    dd.BindFlags = D3D11_BIND_RENDER_TARGET;
    dd.Usage = D3D11_USAGE_DEFAULT;
    dd.CPUAccessFlags = 0;
    dd.MiscFlags = 0;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(g_dev, &dd, NULL, &tmp)) &&
        SUCCEEDED(ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)tmp, NULL, &trtv))) {
        for (m = 1; m <= 2; m++) {
            char nm[64];
            ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)tmp, bb);
            eg_apply(trtv, m, 1);
            _snprintf_s(nm, sizeof nm, _TRUNCATE, "%s_e%d", base, m);
            sb_render_shot_name(nm, (ID3D11Resource *)tmp);
        }
    }
    if (trtv) { ID3D11RenderTargetView_Release(trtv); }
    if (tmp) { ID3D11Texture2D_Release(tmp); }
    ID3D11Resource_Release(bb);
}
