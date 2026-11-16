/* render_reproj.c - W295 WORLD-REPROJ (Kolben, [render] interp_mode=reproj, Standard aus).
 *
 * Zwischenbilder fuer die Aussenwelt per Reprojektion statt Neuzeichnen: Das Welt-RT von Tick N (B) wird mit
 * seiner Tiefe und einer Bewegung JE DRAW auf den Zwischenzeitpunkt t verschoben (Gitter-Warp, ein Gitterpunkt
 * je Pixel). Cockpit, Monitor-Komposition, HUD und Korn laufen danach wie bisher (render_interp.c).
 *
 *  - Draw-ID-Bild: render_interp.c gibt am Swap die Welt-Draws von B einmal mit dem W251-ID-Shader wieder
 *    (Pixel -> Op-Index + 1). Tiefe = Tick-Endstand der Welt-Tiefe (Sicherung), Farbe = Tick-Endstand des Welt-RT.
 *  - Bewegung je Draw als Clip-Matrix T (Zeilenvektor, c_A = c_B * T):
 *      Matrix-Lerp (gleiche Eingangsdaten):  T = M_B^-1 * M_A  (exakt fuer starre/ruhende Objekte und andere VTs)
 *      Vertex-Lerp (neue Eingangsdaten):     T = T_cam * S(d), d = mittlere Rest-Verschiebung der 4 Bezugs-Vertices
 *      ohne Partner / exakt:                 T = T_cam (Objekt ruht in der Welt)
 *      unveraendert bzw. 2D bildfest:        T = I
 *    T_cam = Kamerabewegung des Ziels: die Matrix-Lerp-Bewegung, auf die sich die meisten der 16 groessten
 *    Matrix-Lerp-Draws einigen (ruhende Objekte: M_B^-1 * M_A = P^-1 V_B^-1 V_A P, die Weltmatrix kuerzt sich).
 *  - Zwischenbild: c_t = (1-t) * c_B * T + t * c_B (gleich dem clip-linearen Lerp der Neuzeichnung), c_B aus
 *    Pixel + Tiefe (projektiv, w = 1). Reine Drehung ist damit exakt (tiefenunabhaengig).
 *  - Luecken (Verdeckung frei, Parallaxe, bewegte Objekte): Gitterdreiecke, deren Ecken sich um mehr als
 *    reproj_tear Pixel unterschiedlich verschieben, sind "gerissen" und werden hinter alle anderen gelegt und mit
 *    der Farbe der fernsten Ecke gefuellt (Hintergrund-Streckung). Bildrand: Tick-Endstand als Grundierung.
 * Alles nur bei interp_mode=reproj; sonst ruft render_interp.c nichts hiervon auf (Standard bitgleich). */
#include "render_int.h"
#include "render_reproj.h"
#include <math.h>

#define RP_LISTS 4
#define RP_ROWS 6 /* float4 je Op: T (4 Zeilen), Viewport, (minz, maxz, gueltig, Art) */

typedef struct {
    unsigned ser;
    int ok;
    unsigned w, h, nops;
    ID3D11Texture2D *col, *dep, *id;
    ID3D11ShaderResourceView *col_srv, *dep_srv, *id_srv;
    ID3D11Buffer *tab;
    ID3D11ShaderResourceView *tab_srv;
    unsigned tabcap; /* Ops */
    DXGI_FORMAT cfmt, dfmt;
    float tcam[16], vpc[6];
} rp_list_t;

static rp_list_t rp_l[RP_LISTS];
static int rp_init_done, rp_init_fail;
static ID3D11VertexShader *rp_vs;
static ID3D11GeometryShader *rp_gs;
static ID3D11PixelShader *rp_ps;
static ID3D11Buffer *rp_cb;
static ID3D11SamplerState *rp_smp;
static ID3D11RasterizerState *rp_rs;
static ID3D11DepthStencilState *rp_ds;
static ID3D11Texture2D *rp_zt;
static ID3D11DepthStencilView *rp_zdsv;
static unsigned rp_zw, rp_zh;
static float *rp_cpu;
static unsigned rp_cpucap;
/* Zaehler je Statistikfenster */
static unsigned st_ticks, st_fail, st_warps, st_mat, st_vtx, st_cam, st_ident, st_nocam, st_inst;
static double st_vote, st_dmax;
static unsigned rp_dmove; /* DIAG: Ticks mit Kamerabewegung */

int sb_rp_want(void) { return g_cfg.interp_mode == 1 && !g_cfg.vr_enable; }

static const char *kRP_COMMON =
    "cbuffer C : register(b0) { float4 g; float4 g2; row_major float4x4 Tc; float4 vpc; float4 zc; };\n"
    "struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 inf : TEXCOORD1; };\n";

static const char *kRP_VS =
    "Texture2D<float> depT : register(t1);\n"
    "Texture2D<float4> idT : register(t2);\n"
    "Buffer<float4> tab : register(t3);\n"
    "VO main(uint vid : SV_VertexID) {\n"
    "  uint W = (uint)g.x, H = (uint)g.y;\n"
    "  uint cell = vid / 6u, k = vid - cell * 6u;\n"
    "  uint cw = W - 1u;\n"
    "  uint cy = cell / cw, cx = cell - cy * cw;\n"
    "  uint ox = (k == 1u || k == 3u || k == 4u) ? 1u : 0u;\n"
    "  uint oy = (k == 2u || k == 4u || k == 5u) ? 1u : 0u;\n"
    "  int2 p = int2(cx + ox, cy + oy);\n"
    "  float d = depT.Load(int3(p, 0));\n"
    "  float4 ic = idT.Load(int3(p, 0));\n"
    "  uint id = (uint)(ic.r * 255.0 + 0.5) + ((uint)(ic.g * 255.0 + 0.5) << 8);\n"
    "  float4x4 T = Tc; float4 vp = vpc; float2 zr = zc.xy;\n"
    "  if (id != 0u && id <= (uint)g2.x) {\n"
    "    uint b = (id - 1u) * 6u;\n"
    "    float4 f = tab.Load(b + 5u);\n"
    "    if (f.z > 0.5) { T = float4x4(tab.Load(b), tab.Load(b + 1u), tab.Load(b + 2u), tab.Load(b + 3u));\n"
    "                     vp = tab.Load(b + 4u); zr = f.xy; }\n"
    "  }\n"
    "  float2 pc = float2(p) + 0.5;\n"
    "  float zs = (d - zr.x) / max(zr.y - zr.x, 1e-6);\n"
    "  float4 c = float4((pc.x - vp.x) / vp.z * 2.0 - 1.0, 1.0 - (pc.y - vp.y) / vp.w * 2.0, zs, 1.0);\n"
    "  float t = g.z;\n"
    "  float4 ct = mul(c, T) * (1.0 - t) + c * t;\n"
    "  VO o;\n"
    "  o.uv = pc / float2(W, H);\n"
    "  if (ct.w < 1e-6) { o.pos = float4(0, 0, 0, 1); o.inf = float4(0, 0, d, -1); return o; }\n"
    "  float3 n = ct.xyz / ct.w;\n"
    "  float2 q = float2(vp.x + (n.x + 1.0) * 0.5 * vp.z, vp.y + (1.0 - n.y) * 0.5 * vp.w);\n"
    "  float zt = saturate(zr.x + saturate(n.z) * (zr.y - zr.x));\n"
    "  o.pos = float4(q.x / g.x * 2.0 - 1.0, 1.0 - q.y / g.y * 2.0, zt, 1.0);\n"
    "  o.inf = float4(q - pc, d, 1.0);\n"
    "  return o;\n"
    "}\n";

static const char *kRP_GS =
    "[maxvertexcount(3)]\n"
    "void main(triangle VO v[3], inout TriangleStream<VO> s) {\n"
    "  if (v[0].inf.w < 0.0 || v[1].inf.w < 0.0 || v[2].inf.w < 0.0) { return; }\n"
    "  float m = max(max(length(v[0].inf.xy - v[1].inf.xy), length(v[1].inf.xy - v[2].inf.xy)),\n"
    "                length(v[2].inf.xy - v[0].inf.xy));\n"
    "  bool torn = m > g.w;\n"
    "  uint f = 0u;\n"
    "  if (v[1].inf.z > v[f].inf.z) { f = 1u; }\n"
    "  if (v[2].inf.z > v[f].inf.z) { f = 2u; }\n"
    "  [unroll] for (uint i = 0u; i < 3u; i++) {\n"
    "    VO o = v[i];\n"
    "    if (torn) { o.uv = v[f].uv; o.pos.z = 0.5 + 0.5 * v[i].pos.z; o.inf.w = 2.0; }\n"
    "    else { o.pos.z = 0.5 * v[i].pos.z; }\n"
    "    s.Append(o);\n"
    "  }\n"
    "}\n";

static const char *kRP_PS =
    "Texture2D colT : register(t0);\n"
    "SamplerState sm : register(s0);\n"
    "float4 main(VO i) : SV_Target {\n"
    "  if (g2.y >= 2.0) {\n" /* DIAG: Bewegungsbild (rot/gruen = Verschiebung, blau = gerissen) */
    "    return float4(saturate(0.5 + i.inf.x * 0.05), saturate(0.5 + i.inf.y * 0.05), i.inf.w > 1.5 ? 1.0 : 0.0, 1.0);\n"
    "  }\n"
    "  return colT.Sample(sm, i.uv);\n"
    "}\n";

static int rp_compile(const char *body, const char *tgt, ID3DBlob **b)
{
    static char src[8192];
    _snprintf_s(src, sizeof src, _TRUNCATE, "%s%s", kRP_COMMON, body);
    return sb_im_compile(src, tgt, b);
}

static int rp_init(void)
{
    ID3DBlob *b = NULL;
    D3D11_BUFFER_DESC bd;
    D3D11_SAMPLER_DESC sd;
    D3D11_RASTERIZER_DESC rd;
    D3D11_DEPTH_STENCIL_DESC dd;
    if (rp_init_done) { return !rp_init_fail; }
    rp_init_done = 1;
    rp_init_fail = 1;
    if (!rp_compile(kRP_VS, "vs_4_0", &b)) { return 0; }
    if (FAILED(ID3D11Device_CreateVertexShader(g_dev, b->lpVtbl->GetBufferPointer(b), b->lpVtbl->GetBufferSize(b), NULL, &rp_vs))) { b->lpVtbl->Release(b); return 0; }
    b->lpVtbl->Release(b);
    if (!rp_compile(kRP_GS, "gs_4_0", &b)) { return 0; }
    if (FAILED(ID3D11Device_CreateGeometryShader(g_dev, b->lpVtbl->GetBufferPointer(b), b->lpVtbl->GetBufferSize(b), NULL, &rp_gs))) { b->lpVtbl->Release(b); return 0; }
    b->lpVtbl->Release(b);
    if (!rp_compile(kRP_PS, "ps_4_0", &b)) { return 0; }
    if (FAILED(ID3D11Device_CreatePixelShader(g_dev, b->lpVtbl->GetBufferPointer(b), b->lpVtbl->GetBufferSize(b), NULL, &rp_ps))) { b->lpVtbl->Release(b); return 0; }
    b->lpVtbl->Release(b);
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = 16 * 7;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &rp_cb))) { return 0; }
    memset(&sd, 0, sizeof sd);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(ID3D11Device_CreateSamplerState(g_dev, &sd, &rp_smp))) { return 0; }
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(ID3D11Device_CreateRasterizerState(g_dev, &rd, &rp_rs))) { return 0; }
    memset(&dd, 0, sizeof dd);
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_LESS;
    if (FAILED(ID3D11Device_CreateDepthStencilState(g_dev, &dd, &rp_ds))) { return 0; }
    rp_init_fail = 0;
    HLOG_I("REPROJ: Kolben aktiv (interp_mode=reproj, Welt-RT per Tiefe + Bewegung je Draw, reproj_tear=%.2f)",
           (double)g_cfg.reproj_tear);
    return 1;
}

static void rp_rel(IUnknown **p)
{
    if (*p) { (*p)->lpVtbl->Release(*p); *p = NULL; }
}

static void rp_list_free(rp_list_t *L)
{
    rp_rel((IUnknown **)&L->col_srv); rp_rel((IUnknown **)&L->dep_srv); rp_rel((IUnknown **)&L->id_srv);
    rp_rel((IUnknown **)&L->col); rp_rel((IUnknown **)&L->dep); rp_rel((IUnknown **)&L->id);
    L->w = L->h = 0;
}

static int rp_depth_fmt(DXGI_FORMAT f, DXGI_FORMAT *tl, DXGI_FORMAT *sv)
{
    if (f == DXGI_FORMAT_D24_UNORM_S8_UINT || f == DXGI_FORMAT_R24G8_TYPELESS) { *tl = DXGI_FORMAT_R24G8_TYPELESS; *sv = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; return 1; }
    if (f == DXGI_FORMAT_D32_FLOAT || f == DXGI_FORMAT_R32_TYPELESS) { *tl = DXGI_FORMAT_R32_TYPELESS; *sv = DXGI_FORMAT_R32_FLOAT; return 1; }
    if (f == DXGI_FORMAT_D16_UNORM || f == DXGI_FORMAT_R16_TYPELESS) { *tl = DXGI_FORMAT_R16_TYPELESS; *sv = DXGI_FORMAT_R16_UNORM; return 1; }
    return 0;
}

static int rp_mktex(ID3D11Texture2D **t, ID3D11ShaderResourceView **v, unsigned w, unsigned h, DXGI_FORMAT tf, DXGI_FORMAT vf)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv;
    memset(&td, 0, sizeof td);
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = tf;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, t))) { *t = NULL; return 0; }
    memset(&sv, 0, sizeof sv);
    sv.Format = vf; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
    if (FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)*t, &sv, v))) { *v = NULL; return 0; }
    return 1;
}

static int rp_ensure(rp_list_t *L, const D3D11_TEXTURE2D_DESC *cd, const D3D11_TEXTURE2D_DESC *dd)
{
    DXGI_FORMAT dtl, dsv;
    if (!rp_depth_fmt(dd->Format, &dtl, &dsv)) { return 0; }
    if (L->col && L->w == cd->Width && L->h == cd->Height && L->cfmt == cd->Format && L->dfmt == dd->Format) { return 1; }
    rp_list_free(L);
    if (!rp_mktex(&L->col, &L->col_srv, cd->Width, cd->Height, cd->Format, cd->Format) ||
        !rp_mktex(&L->id, &L->id_srv, cd->Width, cd->Height, cd->Format, cd->Format) ||
        !rp_mktex(&L->dep, &L->dep_srv, dd->Width, dd->Height, dtl, dsv)) {
        rp_list_free(L);
        return 0;
    }
    L->w = cd->Width; L->h = cd->Height; L->cfmt = cd->Format; L->dfmt = dd->Format;
    return 1;
}

/* ---- Matrizen (double, Zeilenvektor c' = c * M, m[Zeile*4+Spalte]) ---- */
static int rp_inv(double o[16], const double m[16])
{
    double a[4][8];
    int i, j, k;
    for (i = 0; i < 4; i++) { for (j = 0; j < 4; j++) { a[i][j] = m[i * 4 + j]; a[i][j + 4] = (i == j) ? 1.0 : 0.0; } }
    for (i = 0; i < 4; i++) {
        int pv = i;
        double d;
        for (k = i + 1; k < 4; k++) { if (fabs(a[k][i]) > fabs(a[pv][i])) { pv = k; } }
        if (fabs(a[pv][i]) < 1e-14) { return 0; }
        if (pv != i) { for (j = 0; j < 8; j++) { double t = a[i][j]; a[i][j] = a[pv][j]; a[pv][j] = t; } }
        d = a[i][i];
        for (j = 0; j < 8; j++) { a[i][j] /= d; }
        for (k = 0; k < 4; k++) { if (k != i) { double f = a[k][i]; for (j = 0; j < 8; j++) { a[k][j] -= f * a[i][j]; } } }
    }
    for (i = 0; i < 4; i++) { for (j = 0; j < 4; j++) { o[i * 4 + j] = a[i][j + 4]; } }
    return 1;
}

static void rp_mul(double r[16], const double a[16], const double b[16])
{
    int i, j;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            r[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] + a[i * 4 + 2] * b[2 * 4 + j] + a[i * 4 + 3] * b[3 * 4 + j];
        }
    }
}

static void rp_vec(double c[4], const double p[4], const double m[16])
{
    int j;
    for (j = 0; j < 4; j++) { c[j] = p[0] * m[0 * 4 + j] + p[1] * m[1 * 4 + j] + p[2] * m[2 * 4 + j] + p[3] * m[3 * 4 + j]; }
}

static void rp_f2d(double d[16], const float f[16]) { int q; for (q = 0; q < 16; q++) { d[q] = f[q]; } }

/* T = M_B^-1 * M_A (c_A = c_B * T). */
static int rp_tmat(double T[16], const float mb[16], const float ma[16])
{
    double B[16], Bi[16], A[16];
    rp_f2d(B, mb);
    rp_f2d(A, ma);
    if (!rp_inv(Bi, B)) { return 0; }
    rp_mul(T, Bi, A);
    return 1;
}

static double rp_rel_err(const double X[16], const double Y[16])
{
    double mx = 0.0, er = 0.0;
    int q;
    for (q = 0; q < 16; q++) {
        double d = fabs(X[q] - Y[q]), m = fabs(Y[q]);
        if (m > mx) { mx = m; }
        if (d > er) { er = d; }
    }
    return mx > 0.0 ? er / mx : 1.0;
}

static void rp_put(float *row, const double T[16], const float vp[6], float kind)
{
    int q;
    for (q = 0; q < 16; q++) { row[q] = (float)T[q]; }
    row[16] = vp[0]; row[17] = vp[1]; row[18] = vp[2] > 0.0f ? vp[2] : 1.0f; row[19] = vp[3] > 0.0f ? vp[3] : 1.0f;
    row[20] = vp[4]; row[21] = vp[5]; row[22] = 1.0f; row[23] = kind;
}

int sb_rp_begin(int li, ID3D11Texture2D *col, ID3D11Texture2D *dep)
{
    rp_list_t *L;
    D3D11_TEXTURE2D_DESC cd, dd;
    if (li < 0 || li >= RP_LISTS) { return 0; }
    L = &rp_l[li];
    L->ok = 0;
    L->ser = 0;
    st_ticks++;
    if (!rp_init() || !col || !dep) { st_fail++; return 0; }
    ID3D11Texture2D_GetDesc(col, &cd);
    ID3D11Texture2D_GetDesc(dep, &dd);
    if (cd.Width < 2u || cd.Height < 2u || dd.Width != cd.Width || dd.Height != cd.Height || cd.SampleDesc.Count != 1u ||
        !(cd.Format == DXGI_FORMAT_R8G8B8A8_UNORM || cd.Format == DXGI_FORMAT_B8G8R8A8_UNORM || cd.Format == DXGI_FORMAT_B8G8R8X8_UNORM) ||
        0) {
        static unsigned nlog;
        if (nlog++ < 4u) {
            HLOG_W("REPROJ: Welt-RT nicht verwendbar (%ux%u fmt=%u, Tiefe %ux%u fmt=%u)", cd.Width, cd.Height,
                   (unsigned)cd.Format, dd.Width, dd.Height, (unsigned)dd.Format);
        }
        st_fail++;
        return 0;
    }
    if (!rp_ensure(L, &cd, &dd)) { st_fail++; return 0; }
    if (!rp_zt || rp_zw != L->w || rp_zh != L->h) {
        /* gemeinsame Warp-Tiefe (Anlegen hier auf dem Spiel-Thread, nie waehrend einer Wiedergabe) */
        D3D11_TEXTURE2D_DESC zd;
        rp_rel((IUnknown **)&rp_zdsv);
        rp_rel((IUnknown **)&rp_zt);
        memset(&zd, 0, sizeof zd);
        zd.Width = L->w; zd.Height = L->h; zd.MipLevels = 1; zd.ArraySize = 1; zd.Format = DXGI_FORMAT_D32_FLOAT;
        zd.SampleDesc.Count = 1; zd.Usage = D3D11_USAGE_DEFAULT; zd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &zd, NULL, &rp_zt)) ||
            FAILED(ID3D11Device_CreateDepthStencilView(g_dev, (ID3D11Resource *)rp_zt, NULL, &rp_zdsv))) {
            rp_rel((IUnknown **)&rp_zt);
            st_fail++;
            return 0;
        }
        rp_zw = L->w; rp_zh = L->h;
    }
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)L->col, (ID3D11Resource *)col);
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)L->dep, (ID3D11Resource *)dep);
    return 1;
}

int sb_rp_tick(int li, unsigned ser, const sb_rp_op_t *ops, unsigned n, unsigned nops, ID3D11Texture2D *idsh)
{
    rp_list_t *L;
    D3D11_TEXTURE2D_DESC cd;
    static const double I4[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    double Tc[16], T[16];
    int cand[16], nc = 0, best = -1, bestv = 0;
    unsigned i, need;
    if (li < 0 || li >= RP_LISTS || !rp_l[li].col || !idsh || nops == 0u || nops > 65535u) { st_fail++; return 0; }
    L = &rp_l[li];
    ID3D11Texture2D_GetDesc(idsh, &cd);
    if (cd.Width != L->w || cd.Height != L->h || cd.Format != L->cfmt) { st_fail++; return 0; }
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)L->id, (ID3D11Resource *)idsh);
    /* Kamerabewegung: 16 groesste Matrix-Lerp-Draws, Mehrheit (rel. Abweichung <= 1e-3) */
    for (i = 0; i < n; i++) {
        const sb_rp_op_t *o = &ops[i];
        int w;
        if (!o->lerp || !o->same || o->stat || o->flat || (o->pre && !(g_cfg.reproj_opt & 2))) { continue; }
        for (w = 0; w < 16; w++) {
            if (w >= nc || o->nv > ops[cand[w]].nv) {
                int z;
                if (nc < 16) { nc++; }
                for (z = nc - 1; z > w; z--) { cand[z] = cand[z - 1]; }
                cand[w] = (int)i;
                break;
            }
        }
    }
    {
        double Tk[16][16];
        int ok[16], a, b;
        for (a = 0; a < nc; a++) { ok[a] = rp_tmat(Tk[a], ops[cand[a]].mvp, ops[cand[a]].amvp); }
        for (a = 0; a < nc; a++) {
            int v = 0;
            if (!ok[a]) { continue; }
            for (b = 0; b < nc; b++) { if (ok[b] && rp_rel_err(Tk[b], Tk[a]) <= 1e-3) { v++; } }
            if (v > bestv) { bestv = v; best = a; }
        }
        if (best >= 0 && ((g_cfg.reproj_opt & 128) || (bestv >= 3 && 3 * bestv >= nc))) { memcpy(Tc, Tk[best], sizeof Tc); st_vote += (double)bestv / (double)nc; }
        else { memcpy(Tc, I4, sizeof Tc); st_nocam++; }
    }
    /* Tabelle je Op-Index (nur Welt-Draws gueltig) */
    need = nops * RP_ROWS * 4u;
    if (rp_cpucap < need) {
        float *p = (float *)realloc(rp_cpu, need * sizeof(float));
        if (!p) { st_fail++; return 0; }
        rp_cpu = p;
        rp_cpucap = need;
    }
    memset(rp_cpu, 0, need * sizeof(float));
    for (i = 0; i < n; i++) {
        const sb_rp_op_t *o = &ops[i];
        float *row;
        if (o->idx >= nops) { continue; }
        row = rp_cpu + (size_t)o->idx * RP_ROWS * 4u;
        if (g_cfg.reproj_opt & 64) { rp_put(row, (o->stat || o->flat) ? I4 : Tc, o->vp, 0.0f); st_cam++; continue; } /* DIAG: nur Kamera (ohne Objektbewegung) */
        if (o->stat && !o->flat && (g_cfg.reproj_opt & 512) && rp_rel_err(Tc, I4) > 0.02) {
            /* DIAG: unveraendert (gleiche Matrix + Daten) trotz Kamerabewegung: im Sichtraum gezeichnet. Weit reichend
             * (Bezugs-w > 8) -> Gelaende/Umgebung, Oberflaeche ruht in der Welt -> Kamerabewegung; nah -> Cockpit/Arm (I) */
            double wmax = 0.0, MB[16];
            int k, q;
            rp_f2d(MB, o->mvp);
            for (k = 0; k < 4; k++) {
                double pb[4], cb[4];
                for (q = 0; q < 4; q++) { pb[q] = o->pref[k][q]; }
                rp_vec(cb, pb, MB);
                if (fabs(cb[3]) > wmax) { wmax = fabs(cb[3]); }
            }
            if (wmax > 8.0) { rp_put(row, Tc, o->vp, 6.0f); st_cam++; continue; }
        }
        if (o->stat || (o->flat && !o->lerp)) { rp_put(row, I4, o->vp, 3.0f); st_ident++; continue; }
        if (o->pre && !(g_cfg.reproj_opt & 1)) { rp_put(row, Tc, o->vp, 4.0f); st_cam++; continue; } /* vorab transformiert: ruhende Welt (reproj_opt Bit 0: eigene Matrix) */
        if (o->lerp && o->same && rp_tmat(T, o->mvp, o->amvp)) {
            if ((g_cfg.reproj_opt & 256) && rp_rel_err(T, I4) < 0.02 && rp_rel_err(Tc, I4) > 0.02) {
                /* DIAG: bildfest (T ~ I) bei Kamerabewegung, aber weit reichend (Bezugs-w > 8) -> der Kamera folgendes
                 * Gelaendegitter (Oberflaeche ruht in der Welt) statt am Cockpit befestigt -> Kamerabewegung */
                double wmax = 0.0, MB[16];
                int k, q;
                rp_f2d(MB, o->mvp);
                for (k = 0; k < 4; k++) {
                    double pb[4], cb[4];
                    for (q = 0; q < 4; q++) { pb[q] = o->pref[k][q]; }
                    rp_vec(cb, pb, MB);
                    if (fabs(cb[3]) > wmax) { wmax = fabs(cb[3]); }
                }
                if (wmax > 8.0) { rp_put(row, Tc, o->vp, 5.0f); st_cam++; continue; }
            }
            if (g_cfg.reproj_maxobj > 0.0f) {
                /* Plausibilitaet: Bildbewegung der Bezugs-Vertices unter T gegen die Kamerabewegung. Mehr als
                 * reproj_maxobj (NDC je Tick) Unterschied = falsche Instanz (gleiches Modell, andere Matrix, z. B.
                 * Bodenkachel, W91) statt echter Objektbewegung -> Kamerabewegung. */
                double MB[16], dmax = 0.0;
                int k, q;
                rp_f2d(MB, o->mvp);
                for (k = 0; k < 4; k++) {
                    double pb[4], cb[4], ct[4], cc[4];
                    for (q = 0; q < 4; q++) { pb[q] = o->pref[k][q]; }
                    rp_vec(cb, pb, MB);
                    if (cb[3] <= 1e-6) { continue; }
                    rp_vec(ct, cb, T);
                    rp_vec(cc, cb, Tc);
                    if (ct[3] <= 1e-6 || cc[3] <= 1e-6) { dmax = 1e9; break; }
                    {
                        double dx = ct[0] / ct[3] - cc[0] / cc[3], dy = ct[1] / ct[3] - cc[1] / cc[3];
                        double d = sqrt(dx * dx + dy * dy);
                        if (d > dmax) { dmax = d; }
                    }
                }
                if (dmax > (double)g_cfg.reproj_maxobj) { rp_put(row, Tc, o->vp, 7.0f); st_cam++; st_inst++; continue; }
            }
            rp_put(row, T, o->vp, 1.0f); st_mat++; continue;
        }
        if (o->lerp && (g_cfg.reproj_opt & 8)) { rp_put(row, Tc, o->vp, 2.0f); st_vtx++; continue; } /* DIAG: Vertex-Lerp nur Kamera */
        if (o->lerp) { /* Vertex-Lerp: Kamera + mittlere Rest-Verschiebung der Bezugs-Vertices (NDC) */
            double MB[16], MA[16], dx = 0.0, dy = 0.0;
            int k, m = 0;
            rp_f2d(MB, o->mvp);
            rp_f2d(MA, o->amvp);
            for (k = 0; k < 4; k++) {
                double pb[4], pa[4], cb[4], ca[4], cc[4];
                int q;
                for (q = 0; q < 4; q++) { pb[q] = o->pref[k][q]; pa[q] = o->apref[k][q]; }
                rp_vec(cb, pb, MB);
                rp_vec(ca, pa, MA);
                rp_vec(cc, cb, Tc);
                if (cb[3] <= 1e-6 || ca[3] <= 1e-6 || cc[3] <= 1e-6) { continue; }
                dx += ca[0] / ca[3] - cc[0] / cc[3];
                dy += ca[1] / ca[3] - cc[1] / cc[3];
                m++;
            }
            memcpy(T, Tc, sizeof T);
            if (m > 0) {
                dx /= m; dy /= m;
                if (fabs(dx) < 0.5 && fabs(dy) < 0.5) {
                    int r;
                    /* T = Tc * S(d): x' = x + w*dx, y' = y + w*dy */
                    for (r = 0; r < 4; r++) { T[r * 4 + 0] += T[r * 4 + 3] * dx; T[r * 4 + 1] += T[r * 4 + 3] * dy; }
                    if (fabs(dx) > st_dmax) { st_dmax = fabs(dx); }
                    if (fabs(dy) > st_dmax) { st_dmax = fabs(dy); }
                }
            }
            rp_put(row, T, o->vp, 2.0f);
            st_vtx++;
            continue;
        }
        rp_put(row, Tc, o->vp, 0.0f); /* ohne Partner: ruht in der Welt */
        st_cam++;
    }
    if (!L->tab || L->tabcap < nops) {
        D3D11_BUFFER_DESC bd;
        D3D11_SHADER_RESOURCE_VIEW_DESC sv;
        unsigned cap = nops < 4096u ? 4096u : nops + nops / 4u;
        if (cap > 65536u) { cap = 65536u; }
        rp_rel((IUnknown **)&L->tab_srv);
        rp_rel((IUnknown **)&L->tab);
        L->tabcap = 0;
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = cap * RP_ROWS * 16u;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &L->tab))) { L->tab = NULL; st_fail++; return 0; }
        memset(&sv, 0, sizeof sv);
        sv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        sv.Buffer.FirstElement = 0;
        sv.Buffer.NumElements = cap * RP_ROWS;
        if (FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)L->tab, &sv, &L->tab_srv))) { st_fail++; return 0; }
        L->tabcap = cap;
    }
    {
        D3D11_BOX box;
        memset(&box, 0, sizeof box);
        box.left = 0; box.right = need * (UINT)sizeof(float); box.top = 0; box.bottom = 1; box.front = 0; box.back = 1;
        ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)L->tab, 0, &box, rp_cpu, 0, 0);
    }
    {
        int q;
        for (q = 0; q < 16; q++) { L->tcam[q] = (float)Tc[q]; }
        /* Ersatz-Viewport fuer Pixel ohne Draw (ID 0): groesster Welt-Draw */
        L->vpc[0] = 0.0f; L->vpc[1] = 0.0f; L->vpc[2] = (float)cd.Width; L->vpc[3] = (float)cd.Height; L->vpc[4] = 0.0f; L->vpc[5] = 1.0f;
        if (nc > 0) { memcpy(L->vpc, ops[cand[0]].vp, sizeof L->vpc); }
    }
    if (g_cfg.reproj_diag >= 1 && ((g_cfg.reproj_dframe > 0 && sb_pf_m0() && g_present_no >= sb_pf_m0() + (unsigned)g_cfg.reproj_dframe && g_present_no < sb_pf_m0() + (unsigned)g_cfg.reproj_dframe + 6u) ||
                                  (g_cfg.reproj_dframe <= 0 && rp_rel_err(Tc, I4) > 0.01 && (++rp_dmove % 13u) == 0u))) { /* DIAG: bei Kamerabewegung 8 groesste Welt-Draws */
        static unsigned nd;
        int top[16], nt = 0, w2, z;
        for (i = 0; i < n; i++) {
            for (w2 = 0; w2 < 16; w2++) {
                if (w2 >= nt || ops[i].nv > ops[top[w2]].nv) {
                    if (nt < 16) { nt++; }
                    for (z = nt - 1; z > w2; z--) { top[z] = top[z - 1]; }
                    top[w2] = (int)i;
                    break;
                }
            }
        }
        if (nd < 600u) {
            HLOG_I("REPROJ dt f=%u ser=%u Kamera %d/%d einig Tc=[%.4f %.4f %.4f %.4f|%.4f %.4f %.4f %.4f|%.4f %.4f %.4f %.4f|%.4f %.4f %.4f %.4f]",
                   g_present_no, ser, bestv, nc, Tc[0], Tc[1], Tc[2], Tc[3], Tc[4], Tc[5], Tc[6], Tc[7], Tc[8], Tc[9], Tc[10], Tc[11], Tc[12], Tc[13], Tc[14], Tc[15]);
        }
        for (w2 = 0; w2 < nt && nd < 600u; w2++, nd++) {
            const sb_rp_op_t *o = &ops[top[w2]];
            const float *row = rp_cpu + (size_t)o->idx * RP_ROWS * 4u;
            double Tr[16], e;
            double pb[4], cb[4], ct[4], MB[16];
            int q;
            for (q = 0; q < 16; q++) { Tr[q] = row[q]; }
            e = rp_rel_err(Tr, Tc);
            rp_f2d(MB, o->mvp);
            for (q = 0; q < 4; q++) { pb[q] = o->pref[0][q]; }
            rp_vec(cb, pb, MB);
            rp_vec(ct, cb, Tr);
            HLOG_I("REPROJ d ser=%u op=%u nv=%u art=%.0f lerp=%d same=%d stat=%d flat=%d pre=%d abw_kamera=%.4f ref0 B=(%.3f,%.3f,w%.3f) A~(%.3f,%.3f)",
                   ser, o->idx, o->nv, (double)row[23], o->lerp, o->same, o->stat, o->flat, o->pre, e,
                   cb[3] != 0.0 ? cb[0] / cb[3] : 0.0, cb[3] != 0.0 ? cb[1] / cb[3] : 0.0, cb[3],
                   ct[3] != 0.0 ? ct[0] / ct[3] : 0.0, ct[3] != 0.0 ? ct[1] / ct[3] : 0.0);
        }
    }
    L->nops = nops;
    L->ser = ser;
    L->ok = 1;
    if (g_cfg.reproj_diag >= 1) {
        static unsigned nlog;
        if (nlog++ < 6u) {
            HLOG_I("REPROJ tick ser=%u li=%d Welt %ux%u fmt=%u Tiefe fmt=%u Draws %u (ops %u) Kamera: %d/%d einig "
                   "Tc=[%.4f %.4f %.4f %.4f|%.4f %.4f %.4f %.4f|%.4f %.4f %.4f %.4f|%.4f %.4f %.4f %.4f]",
                   ser, li, cd.Width, cd.Height, (unsigned)cd.Format, (unsigned)L->dfmt, n, nops, bestv, nc,
                   Tc[0], Tc[1], Tc[2], Tc[3], Tc[4], Tc[5], Tc[6], Tc[7], Tc[8], Tc[9], Tc[10], Tc[11], Tc[12], Tc[13], Tc[14], Tc[15]);
        }
    }
    return 1;
}

int sb_rp_ready(int li, unsigned ser)
{
    return li >= 0 && li < RP_LISTS && rp_l[li].ok && rp_l[li].ser == ser && ser != 0u;
}

void sb_rp_warp(ID3D11DeviceContext *c, int li, ID3D11RenderTargetView *dst, ID3D11Texture2D *dtex, float t)
{
    rp_list_t *L;
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_VIEWPORT vp;
    D3D11_TEXTURE2D_DESC td;
    ID3D11ShaderResourceView *vs[4], *nul[4] = { NULL, NULL, NULL, NULL };
    ID3D11Buffer *nb = NULL;
    if (li < 0 || li >= RP_LISTS || !rp_l[li].ok || !dst || !dtex) { return; }
    L = &rp_l[li];
    ID3D11Texture2D_GetDesc(dtex, &td);
    if (td.Width != L->w || td.Height != L->h || td.Format != L->cfmt) { return; }
    if (!rp_zt || rp_zw != L->w || rp_zh != L->h) { return; }
    if (g_cfg.reproj_opt & 4) { t = 0.0f; } /* DIAG: Vorhersage Tick N-1 (Pruefung der Bewegung gegen das echte Tick-Bild) */
    st_warps++;
    ID3D11DeviceContext_CopyResource(c, (ID3D11Resource *)dtex, (ID3D11Resource *)L->col); /* Grundierung (Bildrand) */
    ID3D11DeviceContext_ClearDepthStencilView(c, rp_zdsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
    if (FAILED(ID3D11DeviceContext_Map(c, (ID3D11Resource *)rp_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { return; }
    {
        float *f = (float *)m.pData;
        f[0] = (float)L->w; f[1] = (float)L->h; f[2] = t;
        f[3] = (g_cfg.reproj_tear > 0.0f ? g_cfg.reproj_tear : 2.0f) * (float)L->w / 512.0f;
        f[4] = (float)L->nops; f[5] = (float)g_cfg.reproj_diag; f[6] = 0.0f; f[7] = 0.0f;
        memcpy(f + 8, L->tcam, 16 * sizeof(float));
        f[24] = L->vpc[0]; f[25] = L->vpc[1]; f[26] = L->vpc[2] > 0.0f ? L->vpc[2] : 1.0f; f[27] = L->vpc[3] > 0.0f ? L->vpc[3] : 1.0f;
        f[28] = L->vpc[4]; f[29] = L->vpc[5]; f[30] = 0.0f; f[31] = 0.0f;
        ID3D11DeviceContext_Unmap(c, (ID3D11Resource *)rp_cb, 0);
    }
    ID3D11DeviceContext_OMSetRenderTargets(c, 1, &dst, rp_zdsv);
    ID3D11DeviceContext_OMSetBlendState(c, NULL, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(c, rp_ds, 0);
    ID3D11DeviceContext_RSSetState(c, rp_rs);
    memset(&vp, 0, sizeof vp);
    vp.Width = (float)L->w; vp.Height = (float)L->h; vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(c, 1, &vp);
    ID3D11DeviceContext_IASetInputLayout(c, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(c, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(c, rp_vs, NULL, 0);
    ID3D11DeviceContext_GSSetShader(c, rp_gs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(c, rp_ps, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(c, 0, 1, &rp_cb);
    ID3D11DeviceContext_GSSetConstantBuffers(c, 0, 1, &rp_cb);
    ID3D11DeviceContext_PSSetConstantBuffers(c, 0, 1, &rp_cb);
    vs[0] = NULL; vs[1] = L->dep_srv; vs[2] = L->id_srv; vs[3] = L->tab_srv;
    ID3D11DeviceContext_VSSetShaderResources(c, 0, 4, vs);
    ID3D11DeviceContext_PSSetShaderResources(c, 0, 1, &L->col_srv);
    ID3D11DeviceContext_PSSetSamplers(c, 0, 1, &rp_smp);
    ID3D11DeviceContext_Draw(c, (L->w - 1u) * (L->h - 1u) * 6u, 0);
    ID3D11DeviceContext_GSSetShader(c, NULL, NULL, 0);
    ID3D11DeviceContext_GSSetConstantBuffers(c, 0, 1, &nb);
    ID3D11DeviceContext_VSSetShaderResources(c, 0, 4, nul);
    ID3D11DeviceContext_PSSetShaderResources(c, 0, 1, nul);
}

void sb_rp_stat_log(void)
{
    if (!g_cfg.reproj_diag || !st_ticks) { return; }
    HLOG_I("REPROJ stat: ticks=%u fehl=%u warps=%u draws matrix=%u vertex=%u kamera=%u fest=%u ohne_kamera=%u "
           "kamera_einig=%.2f rest_max=%.3f instanz_verworfen=%u",
           st_ticks, st_fail, st_warps, st_mat, st_vtx, st_cam, st_ident, st_nocam,
           st_ticks > st_nocam ? st_vote / (double)(st_ticks - st_nocam) : 0.0, st_dmax, st_inst);
    st_ticks = st_fail = st_warps = st_mat = st_vtx = st_cam = st_ident = st_nocam = st_inst = 0;
    st_vote = st_dmax = 0.0;
}
