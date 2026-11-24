/* render_post.c - W463 RCORE: Nachbearbeitungskette des gezeigten Bildes (Beschreibung s. render_post.h).
 *
 * Reihenfolge = Tabelle kPOST (vorher zweimal ausgeschrieben in d3d11_render.c, je Present-Weg):
 *   1 shfx   W121/W131 SHADE-FX: Cockpit-Licht/-Leuchten, Farbe, FXAA, SSAO (render_shfx.c)   SB+LoC, Remaster
 *   2 fxr    W133 REM-FX: Explosions-Licht im Cockpit (render_fx_rem.c)                          SB, Remaster
 *   3 panini W191/W235 UW-PERSP: Panini-Projektion/Breite der Cockpit-Ansicht (render_cpanini.c) SB, Experimental
 *   4 vapor  W240 FX-EXPERIMENTAL: Vaporwave (render_xfx.c)                                      SB, Experimental
 *   5 edge   W107/W319/W326 Randmaske ausserhalb der 4:3-Box bzw. LoC-16:9 (render_edge.c)      beide
 * Danach (nicht Teil der Kette, eigene Reihenfolge in d3d11_render.c): Wasserzeichen, Toast, Clip/Video-
 * Abgriff, Overlays (W461). Standard (Original-Look): nur edge, und nur bei edge_mode>0/Monitoransicht/
 * LoC breit - sonst laeuft kein Pass. */
#include "render_int.h"
#include "render_post.h"
#include "render_fx_rem.h" /* fxr_ckp_active, fxr_post */
#include "sb_perfhud.h"    /* sb_gp_mark (W146 GPU-COST) */

/* ---------- Passes: Aktiv-Bedingung und Anwendung (unveraendert aus d3d11_render.c) ---------- */
static int pp_shfx_on(void) { return shfx_post_on(); }
static void pp_shfx_apply(const sb_post_tgt_t *t) { sb_shfx_post(t->rtv, t->res); }

static int pp_fxr_on(void) { return fxr_ckp_active() && !sb_mv_on(); /* W319: kein Cockpit-Licht ohne Cockpit */ }
static void pp_fxr_apply(const sb_post_tgt_t *t) { fxr_post(t->rtv, t->res); }

static int pp_panini_on(void)
{
    return (g_cfg.cockpit_panini || g_cfg.cockpit_width || g_cfg.cockpit_persp_cycle[0]) && !sb_mv_on() &&
           (sb_cp_panini_d() > 0.0f || sb_cp_width() > 1.0f);
}
static void pp_panini_apply(const sb_post_tgt_t *t) { sb_cp_panini_apply(t->rtv, t->res, sb_cp_panini_d()); }

static int pp_vapor_on(void) { return g_cfg.fx_vaporwave && sb_xfx_vapor_on(); }
static void pp_vapor_apply(const sb_post_tgt_t *t) { sb_xfx_vapor_apply(t->rtv, t->res); }

static int pp_edge_on(void) { return g_cfg.edge_mode > 0 || sb_mv_on() || g_loc_wide; /* W326: LoC nativ 16:9 */ }
static void pp_edge_apply(const sb_post_tgt_t *t) { sb_edge_apply(t->rtv); }

typedef struct {
    const char *name;
    int (*on)(void);                       /* ausserhalb ctx_lock ausgewertet */
    void (*apply)(const sb_post_tgt_t *t); /* unter ctx_lock */
    int gp_after;                          /* GPU-COST-Marke nach dem Pass (SB_POST_SITE_SWAP), -1 = keine */
} sb_post_pass_t;

/* DIE Reihenfolge der Nachbearbeitung. Neuer Pass = eine Zeile hier (+ Modul mit on/apply). */
static const sb_post_pass_t kPOST[] = {
    { "shfx",   pp_shfx_on,   pp_shfx_apply,   GP_FXPOST },
    { "fxr",    pp_fxr_on,    pp_fxr_apply,    GP_HUD },
    { "panini", pp_panini_on, pp_panini_apply, -1 },
    { "vapor",  pp_vapor_on,  pp_vapor_apply,  -1 },
    { "edge",   pp_edge_on,   pp_edge_apply,   -1 },
};
#define POST_N (sizeof kPOST / sizeof kPOST[0])

void sb_post_chain(int site, int gate, int marks, sb_post_tgt_get_fn get, sb_post_tgt_put_fn put)
{
    unsigned i;
    (void)site;
    for (i = 0; i < POST_N; i++) {
        const sb_post_pass_t *p = &kPOST[i];
        if (gate && p->on()) {
            sb_post_tgt_t t = { NULL, NULL };
            sb_ctx_enter();
            if (get(&t)) { p->apply(&t); }
            if (put) { put(&t); }
            sb_ctx_leave();
        }
        if (marks && p->gp_after >= 0) { sb_gp_mark(p->gp_after); }
    }
}

/* ---------- gemeinsamer Vollbild-Kopierpass (aus render_xfx.c/render_cpanini.c zusammengelegt) ---------- */
int sb_post_fs_init(sb_post_fs_t *p)
{
    ID3DBlob *b = NULL;
    D3D11_BUFFER_DESC bd;
    D3D11_SAMPLER_DESC sd;
    D3D11_RASTERIZER_DESC rd;
    D3D11_DEPTH_STENCIL_DESC dsd;
    if (p->state) { return p->state > 0; }
    p->state = -1;
    if (!g_dev) { return 0; }
    if (sb_im_compile(p->vs_src, "vs_4_0", &b)) {
        ID3D11Device_CreateVertexShader(g_dev, b->lpVtbl->GetBufferPointer(b), b->lpVtbl->GetBufferSize(b), NULL, &p->vs);
        b->lpVtbl->Release(b);
        b = NULL;
    }
    if (sb_im_compile(p->ps_src, "ps_4_0", &b)) {
        ID3D11Device_CreatePixelShader(g_dev, b->lpVtbl->GetBufferPointer(b), b->lpVtbl->GetBufferSize(b), NULL, &p->ps);
        b->lpVtbl->Release(b);
        b = NULL;
    }
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = p->cb_bytes;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &p->cb))) { p->cb = NULL; }
    memset(&sd, 0, sizeof sd);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(ID3D11Device_CreateSamplerState(g_dev, &sd, &p->smp))) { p->smp = NULL; }
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(ID3D11Device_CreateRasterizerState(g_dev, &rd, &p->rs))) { p->rs = NULL; }
    memset(&dsd, 0, sizeof dsd);
    dsd.DepthEnable = FALSE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dsd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    if (FAILED(ID3D11Device_CreateDepthStencilState(g_dev, &dsd, &p->ds))) { p->ds = NULL; }
    if (!p->vs || !p->ps || !p->cb || !p->smp || !p->rs || !p->ds) {
        HLOG_E("%s", p->fail_log);
        return 0;
    }
    p->state = 1;
    return 1;
}

int sb_post_fs_ensure_copy(sb_post_fs_t *p, ID3D11Resource *res)
{
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)res, &td);
    if (p->tex && p->tw == td.Width && p->th == td.Height && p->fmt == td.Format) { return 1; }
    if (p->srv) { ID3D11ShaderResourceView_Release(p->srv); p->srv = NULL; }
    if (p->tex) { ID3D11Texture2D_Release(p->tex); p->tex = NULL; }
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.SampleDesc.Quality = 0;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = 0;
    td.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &p->tex))) { p->tex = NULL; return 0; }
    if (FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)p->tex, NULL, &p->srv))) {
        ID3D11Texture2D_Release(p->tex);
        p->tex = NULL;
        p->srv = NULL;
        return 0;
    }
    p->tw = td.Width;
    p->th = td.Height;
    p->fmt = td.Format;
    return 1;
}

void sb_post_fs_draw(sb_post_fs_t *p, ID3D11RenderTargetView *rtv, float vpw, float vph)
{
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
    vp.Width = vpw;
    vp.Height = vph;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_OMSetBlendState(g_ctx, NULL, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, p->ds, 0);
    ID3D11DeviceContext_RSSetState(g_ctx, p->rs);
    ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &vp);
    ID3D11DeviceContext_IASetInputLayout(g_ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(g_ctx, p->vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_ctx, p->ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, &p->srv);
    ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 1, &p->smp);
    ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 0, 1, &p->cb);
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
}
