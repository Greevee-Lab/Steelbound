/* render_shader.c - D3D11-Renderer: IM-Pipeline (HLSL-VS/PS, Kompilieren,
 * Layout/VB/States: sb_im_init) und VSH2D-Analyse (NV2A-Vertex-Shader-
 * Dekoder, oPos-Muster, Konstanten-Spiegel). W68 SPLIT: 1:1 aus
 * d3d11_render.c verschoben. */
#include "render_int.h"

/* ---------- M2b-Geometriepfad (IM -> D3D11) ---------- */
/* Fixed-Function-Ersatz: Spiel nutzt Vertex-Shader-PROGRAMME (kein FVF).
 * Heuristik pro Draw: Pixel-Koordinaten (RHW/UI) vs. Welt (3D). RHW wird
 * per Pixel->NDC-Matrix abgebildet, 3D per World*View*Proj (Hor+). */
static const char *kIM_VS =
    "row_major float4x4 mvp : register(b0);\n"
    "struct VSIN { float4 pos : POSITION; float4 col : COLOR0;\n"
    "              float2 uv : TEXCOORD0; };\n"
    "struct VSOUT { float4 pos : SV_Position; float4 col : COLOR0;\n"
    "               float2 uv : TEXCOORD0; };\n"
    "VSOUT main(VSIN i) { VSOUT o; o.pos = mul(i.pos, mvp);\n"
    "  o.col = i.col; o.uv = i.uv; return o; }\n";
/* IM-Pixelshader (einmalig beim Init): Vertexfarbe, mal Textur. */
static char g_ps_src[2048];
static void sb_im_ps_source(void)
{
    _snprintf_s(g_ps_src, sizeof g_ps_src, _TRUNCATE,
        "cbuffer CB : register(b0) { row_major float4x4 mvp; uint has_tex; };\n"
        "Texture2D tex : register(t0);\n"
        "SamplerState samp : register(s0);\n"
        "struct PSIN { float4 pos : SV_Position; float4 col : COLOR0;\n"
        "              float2 uv : TEXCOORD0; };\n"
        "float4 main(PSIN i) : SV_Target {\n"
        "  float4 c = i.col;\n"
        "  if ((has_tex & 1u) != 0) { float4 t = tex.Sample(samp, i.uv); c *= t;\n"
        /* FIX(STARTSEQ-BARS,W98): Xbox D3DTSS_ALPHAKILL Stufe 0 (has_tex
         * Bit 24): Fragment verwerfen, wenn das Texel-Alpha 0 ist (NV2A). */
        "    if ((has_tex & 0x1000000u) != 0u && t.a < (0.5 / 255.0)) { discard; } }\n"
        /* FIX(FX-ORIG,W90): NV2A-Alpha-Test (has_tex Bits 8-11 = GL-Funktion
         * - 0x1FF, 0 = aus; Bits 16-23 = Ref). Vergleich in 8 Bit wie NV2A. */
        "  uint af = (has_tex >> 8) & 15u;\n"
        "  if (af != 0u) { uint a8 = (uint)(saturate(c.a) * 255.0 + 0.5); uint r8 = (has_tex >> 16) & 255u;\n"
        "    bool ok = (af == 2u) ? (a8 < r8) : (af == 3u) ? (a8 == r8) : (af == 4u) ? (a8 <= r8) :\n"
        "              (af == 5u) ? (a8 > r8) : (af == 6u) ? (a8 != r8) : (af == 7u) ? (a8 >= r8) : (af == 8u);\n"
        "    if (!ok) { discard; } }\n"
        "  return c;\n"
        "}\n");
}

ID3D11VertexShader *g_im_vs;
ID3D11PixelShader *g_im_ps;
ID3D11InputLayout *g_im_layout;
ID3D11Buffer *g_im_vb, *g_im_cb;
ID3D11DepthStencilState *g_im_ds_off, *g_im_ds_on;
unsigned g_im_submits;
int g_im_ready;
/* g_im_col_slot: letzter Color-Slot pro Draw (-1 = keiner), s.o. */

/* ID3DBlob hat keine COBJMACROS -> direkt ueber vtbl (IUnknown-ABI). */
#define BLOB_PTR(b) ((b)->lpVtbl->GetBufferPointer(b))
#define BLOB_SIZE(b) ((b)->lpVtbl->GetBufferSize(b))
#define BLOB_REL(b) ((b)->lpVtbl->Release(b))

typedef HRESULT(WINAPI *fn_d3dcompile)(LPCVOID, SIZE_T, LPCSTR,
                                       const D3D_SHADER_MACRO *,
                                       ID3DInclude *, LPCSTR, LPCSTR, UINT,
                                       UINT, ID3DBlob **, ID3DBlob **);

int sb_im_compile(const char *src, const char *target, ID3DBlob **out)
{
    HMODULE dc;
    fn_d3dcompile comp;
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr;
    dc = LoadLibraryA("d3dcompiler_47.dll");
    if (!dc) {
        dc = LoadLibraryA("d3dcompiler_43.dll");
    }
    if (!dc) {
        HLOG_E("render: d3dcompiler-DLL fehlt (kein IM-Shader)");
        return 0;
    }
    comp = (fn_d3dcompile)GetProcAddress(dc, "D3DCompile");
    if (!comp) {
        HLOG_E("render: D3DCompile fehlt");
        return 0;
    }
    hr = comp(src, strlen(src), NULL, NULL, NULL, "main", target,
              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) {
        HLOG_E("render: Shader-Fehler %s: %s", target,
               err ? (const char *)BLOB_PTR(err) : "?");
        if (err) {
            BLOB_REL(err);
        }
        return 0;
    }
    if (err) {
        BLOB_REL(err);
    }
    *out = code;
    return 1;
}

void sb_im_init(void)
{
    ID3DBlob *vsb = NULL, *psb = NULL;
    D3D11_BUFFER_DESC bd;
    D3D11_INPUT_ELEMENT_DESC layout[3];
    D3D11_RASTERIZER_DESC rd;
    D3D11_VIEWPORT dvp;
    ID3D11RasterizerState *rs = NULL;
    HRESULT hr;
    if (g_im_ready || !g_dev) {
        return;
    }
    sb_im_ps_source();
    if (!sb_im_compile(kIM_VS, "vs_4_0", &vsb) ||
        !sb_im_compile(g_ps_src, "ps_4_0", &psb)) {
        ExitProcess(7);
    }
    hr = ID3D11Device_CreateVertexShader(g_dev,
                                         BLOB_PTR(vsb),
                                         BLOB_SIZE(vsb), NULL,
                                         &g_im_vs);
    if (FAILED(hr)) {
        HLOG_E("render: VS fehlgeschlagen");
        ExitProcess(7);
    }
    layout[0].SemanticName = "POSITION";
    layout[0].SemanticIndex = 0;
    layout[0].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    layout[0].InputSlot = 0;
    layout[0].AlignedByteOffset = 0;
    layout[0].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
    layout[0].InstanceDataStepRate = 0;
    layout[1].SemanticName = "COLOR";
    layout[1].SemanticIndex = 0;
    layout[1].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    layout[1].InputSlot = 0;
    layout[1].AlignedByteOffset = 16;
    layout[1].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
    layout[1].InstanceDataStepRate = 0;
    layout[2].SemanticName = "TEXCOORD";
    layout[2].SemanticIndex = 0;
    layout[2].Format = DXGI_FORMAT_R32G32_FLOAT;
    layout[2].InputSlot = 0;
    layout[2].AlignedByteOffset = 32;
    layout[2].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
    layout[2].InstanceDataStepRate = 0;
    hr = ID3D11Device_CreateInputLayout(g_dev, layout, 3,
                                        BLOB_PTR(vsb),
                                        BLOB_SIZE(vsb),
                                        &g_im_layout);
    BLOB_REL(vsb);
    if (FAILED(hr)) {
        HLOG_E("render: Input-Layout fehlgeschlagen");
        ExitProcess(7);
    }
    hr = ID3D11Device_CreatePixelShader(g_dev,
                                        BLOB_PTR(psb),
                                        BLOB_SIZE(psb), NULL,
                                        &g_im_ps);
    BLOB_REL(psb);
    if (FAILED(hr)) {
        HLOG_E("render: PS fehlgeschlagen");
        ExitProcess(7);
    }
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = sizeof(im_vertex_t) * IM_VB_MAX;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &g_im_vb);
    if (FAILED(hr)) {
        HLOG_E("render: IM-VB fehlgeschlagen");
        ExitProcess(7);
    }
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = 80;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &g_im_cb);
    if (FAILED(hr)) {
        HLOG_E("render: IM-CB fehlgeschlagen");
        ExitProcess(7);
    }
    /* Cull aus (kein Winding-Raten bei expandierten Fans/Strips). */
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    hr = ID3D11Device_CreateRasterizerState(g_dev, &rd, &rs);
    if (FAILED(hr)) {
        HLOG_E("render: RS fehlgeschlagen");
        ExitProcess(7);
    }
    ID3D11DeviceContext_RSSetState(g_ctx, rs);
    g_rs_norm = rs; /* SIDESMEAR: behalten (Restore nach Clip-Draws) */
    /* SIDESMEAR: gleiche RS + Scissor (Selbst-Sampling-Clip auf Box). */
    {
        D3D11_RASTERIZER_DESC rd2;
        memset(&rd2, 0, sizeof rd2);
        rd2.FillMode = D3D11_FILL_SOLID;
        rd2.CullMode = D3D11_CULL_NONE;
        rd2.DepthClipEnable = TRUE;
        rd2.ScissorEnable = TRUE;
        if (FAILED(ID3D11Device_CreateRasterizerState(g_dev, &rd2, &g_rs_scis))) {
            g_rs_scis = NULL; /* ohne Clip-RS = alter Pfad */
        }
    }
    /* Alpha-Blending (Fade-Overlays etc.; opake Draws unberuehrt). */
    {
        D3D11_BLEND_DESC bd;
        ID3D11BlendState *bs = NULL;
        memset(&bd, 0, sizeof bd);
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask =
            D3D11_COLOR_WRITE_ENABLE_ALL;
        hr = ID3D11Device_CreateBlendState(g_dev, &bd, &bs);
        if (FAILED(hr)) {
            HLOG_E("render: Blend-State fehlgeschlagen");
            ExitProcess(7);
        }
        ID3D11DeviceContext_OMSetBlendState(g_ctx, bs, NULL,
                                            0xFFFFFFFFu);
        ID3D11BlendState_Release(bs);
    }
    {
        D3D11_DEPTH_STENCIL_DESC dd;
        memset(&dd, 0, sizeof dd);
        dd.DepthEnable = FALSE;
        dd.StencilEnable = FALSE;
        hr = ID3D11Device_CreateDepthStencilState(g_dev, &dd,
                                                  &g_im_ds_off);
        if (FAILED(hr)) {
            HLOG_E("render: DS-off fehlgeschlagen");
            ExitProcess(7);
        }
        dd.DepthEnable = TRUE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dd.DepthFunc = D3D11_COMPARISON_LESS;
        hr = ID3D11Device_CreateDepthStencilState(g_dev, &dd,
                                                  &g_im_ds_on);
        if (FAILED(hr)) {
            HLOG_E("render: DS-on fehlgeschlagen");
            ExitProcess(7);
        }
    }
    /* Vollbild-Viewport (M2b-einfach; Split/Letterbox spaeter). */
    dvp.TopLeftX = 0.0f;
    dvp.TopLeftY = 0.0f;
    dvp.Width = (float)g_bb_w;
    dvp.Height = (float)g_bb_h;
    dvp.MinDepth = 0.0f;
    dvp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &dvp);
    {
        D3D11_SAMPLER_DESC sd;
        ID3D11SamplerState *ss = NULL;
        memset(&sd, 0, sizeof sd);
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxAnisotropy = 1;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MinLOD = 0;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        hr = ID3D11Device_CreateSamplerState(g_dev, &sd, &ss);
        if (FAILED(hr)) {
            HLOG_E("render: Sampler fehlgeschlagen");
            ExitProcess(7);
        }
        ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 1, &ss);
        ID3D11SamplerState_Release(ss);
    }
    sb_tex_init(g_dev);
    g_im_ready = 1;
    HLOG_I("render: IM-Pipeline bereit (VS/PS/VB %u Verts)", IM_VB_MAX);
}

/* ---------- VSH2D (W38): oPos per Vertex-Shader-Analyse ---------- */
/* Das Spiel nutzt ausnahmslos eigene Vertex-Shader-PROGRAMME (NV2A-VSH-
 * Mikrocode: je Instruktion 4 DWORDs, Region ab Struct+0x114 als
 * [Chunk-Header][bis zu 8 Slots] wiederholt, Header 0x00NN0B00 mit
 * NN = 16 * Slots im Chunk; progsize = Slot-Anzahl).
 * Alle 23 Programme eines Missions-Laufs nutzen zwei oPos-Muster (Beleg:
 * VSH-Dump + Offline-Dekoder, Bit-Layout nach der Public-Domain-Referenz
 * nv2a_vsh_cpu + Khronos NV_vertex_program1_1):
 * P1 (Passthrough, z.B. Progsize 2/3/5/6): oPos = v0 (Rohwerte = Pixel).
 * P2 (z.B. Progsize 7-10): oPos.xyzw = DP4(v0, cE..cE+3) + Viewport-Tail
 *   (MUL oPos.xyz, R12, c-38; RCC R1.x, R12.w; MAD oPos.xyz, R12, R1.x, c-37).
 * R12 liest oPos zurueck (NV2A-Semantik), c-38/-37 (Encoding 58/59) liefert
 * der Treiber aus dem Viewport (Skala/Offset), alle anderen Konstanten
 * stehen im CPU-Spiegel 0x21BCA8 + Encoding*16 (das 8-Bit-Encoding
 * adressiert den Spiegel direkt; bewiesen: DP4-c2 = 0x62 = Spiegel 98 =
 * WVP aus m3d_vs_mvp; Spiegel 58/59 sind null, der Treiber liefert sie
 * pro Draw aus dem Viewport (Treiberverhalten, eigene Analyse).
 * vsh_emul=1: Draws mit P1/P2-Programmen bekommen exakte Positionen (statt
 * RHW-Fehlklassifikation bzw. Vollbild-statt-Viewport-Abbildung). Unbekannte
 * Muster (u.a. grosse Lighting-Programme, auf BB via m3d_vs_mvp korrekt)
 * laufen weiter ueber den alten Pfad (DIAG VSHEMU bei vsh_dump=1). */
typedef struct {
    unsigned mac, ilu;      /* Opcodes (MAC 0-13, ILU 0-7) */
    unsigned craw;          /* 8-Bit-Konstanten-Encoding = Spiegel-Index */
    unsigned vin;           /* v-Register (A/B/C teilen sich V) */
    unsigned amux, ar, aneg, aswz;
    unsigned bmux, br, bneg, bswz;
    unsigned cmux, cr, cneg, cswz;
    unsigned macmask, outr, ilumask, omask, orb, oaddr, omux, a0x;
} vsh_dec_t;

/* Ein Slot dekodieren (t = 4 DWORDs, t[0] ungenutzt). Swizzle-Bytes:
 * (x<<6)|(y<<4)|(z<<2)|w; 0x1B = Identitaet, 0xAA = zzzz, 0x00 = xxxx,
 * 0xFF = wwww. Masken: Bit3=X, Bit2=Y, Bit1=Z, Bit0=W. */
static void vsh_decode_slot(const unsigned *t, vsh_dec_t *d)
{
    unsigned w1 = t[1], w2 = t[2], w3 = t[3];
    d->mac = (w1 >> 21) & 0xFu; d->ilu = (w1 >> 25) & 7u;
    d->craw = (w1 >> 13) & 0xFFu; d->vin = (w1 >> 9) & 0xFu;
    d->aneg = (w1 >> 8) & 1u; d->aswz = w1 & 0xFFu;
    d->ar = (w2 >> 28) & 0xFu; d->amux = (w2 >> 26) & 3u;
    d->bneg = (w2 >> 25) & 1u; d->bswz = (w2 >> 17) & 0xFFu;
    d->br = (w2 >> 13) & 0xFu; d->bmux = (w2 >> 11) & 3u;
    d->cneg = (w2 >> 10) & 1u; d->cswz = (w2 >> 2) & 0xFFu;
    d->cr = ((w2 & 3u) << 2) | ((w3 >> 30) & 3u); d->cmux = (w3 >> 28) & 3u;
    d->macmask = (w3 >> 24) & 0xFu; d->outr = (w3 >> 20) & 0xFu;
    d->ilumask = (w3 >> 16) & 0xFu; d->omask = (w3 >> 12) & 0xFu;
    d->orb = (w3 >> 11) & 1u; d->oaddr = (w3 >> 3) & 0xFFu;
    d->omux = (w3 >> 2) & 1u; d->a0x = (w3 >> 1) & 1u;
}

static vsh_prog_t g_vsh_cache[64];
static unsigned g_vsh_ncache;

/* Schreibt Slot Temp-Register t.Komponente? (gepaarte ILU schreibt R1). */
static int vsh_writes_temp(const vsh_dec_t *d, unsigned t, unsigned compbit)
{
    int paired = (d->mac != 0 && d->ilu != 0);
    if (d->mac != 0 && d->mac != 13 /*ARL*/) {
        if ((d->macmask & compbit) && d->outr == t) { return 1; }
    }
    if (d->ilu != 0) {
        unsigned dest = paired ? 1u : d->outr;
        if ((d->ilumask & compbit) && dest == t) { return 1; }
    }
    return 0;
}

/* Eingangs-Helfer (which: 0=A, 1=B, 2=C). */
static int vsh_in_v0(const vsh_dec_t *d, int which)
{
    unsigned mux, neg, swz;
    if (which == 0) { mux = d->amux; neg = d->aneg; swz = d->aswz; }
    else if (which == 1) { mux = d->bmux; neg = d->bneg; swz = d->bswz; }
    else { mux = d->cmux; neg = d->cneg; swz = d->cswz; }
    return mux == 2 && d->vin == 0 && neg == 0 && swz == 0x1Bu;
}
static int vsh_in_const(const vsh_dec_t *d, int which, unsigned swzneed)
{
    unsigned mux, neg, swz;
    if (which == 0) { mux = d->amux; neg = d->aneg; swz = d->aswz; }
    else if (which == 1) { mux = d->bmux; neg = d->bneg; swz = d->bswz; }
    else { mux = d->cmux; neg = d->cneg; swz = d->cswz; }
    return mux == 3 && neg == 0 && swz == swzneed;
}
static int vsh_in_temp(const vsh_dec_t *d, int which, unsigned r, unsigned swzneed)
{
    unsigned mux, rr, neg, swz;
    if (which == 0) { mux = d->amux; rr = d->ar; neg = d->aneg; swz = d->aswz; }
    else if (which == 1) { mux = d->bmux; rr = d->br; neg = d->bneg; swz = d->bswz; }
    else { mux = d->cmux; rr = d->cr; neg = d->cneg; swz = d->cswz; }
    return mux == 1 && rr == r && neg == 0 && swz == swzneed;
}
static int vsh_out_opos(const vsh_dec_t *d)
{
    return d->omask != 0 && d->orb == 1 && (d->oaddr & 0xF) == 0;
}

/* Programm aus dem Spiel-Speicher lesen + klassifizieren. Gibt 1 zurueck
 * (p->kind 0/1/2), 0 bei unlesbar/ungueltig (alter Pfad). */
static int vsh_classify(unsigned handle, vsh_prog_t *p)
{
    unsigned base, nslots, nwords, si, ri, k, h;
    unsigned slots[136 * 4];
    vsh_dec_t dec[136];
    int s, npos, ok;
    memset(p, 0, sizeof *p);
    p->ot0 = -1; /* W90 */
    p->ot0i = -1; /* W84 */
    p->handle = handle;
    if (handle < 0x10000u) { return 0; }
    base = handle & ~1u;
    __try {
        unsigned *hd = (unsigned *)(uintptr_t)base;
        unsigned *rg;
        nslots = hd[2]; nwords = hd[3];
        if (nslots < 1u || nslots > 136u || nwords < 1u || nwords > 1024u) { return 0; }
        rg = (unsigned *)(uintptr_t)(base + 0x114u);
        si = 0; ri = 0;
        while (si < nslots && ri < nwords) {
            unsigned cn;
            cn = (rg[ri] >> 16) & 0xFFu;
            ri++;
            cn = (cn >= 16u && cn <= 128u && (cn % 16u) == 0) ? (cn / 16u) : 8u;
            if (cn > nslots - si) { cn = nslots - si; }
            for (k = 0; k < cn && ri + 4u <= nwords; k++) {
                slots[si * 4u + 0] = rg[ri];
                slots[si * 4u + 1] = rg[ri + 1u];
                slots[si * 4u + 2] = rg[ri + 2u];
                slots[si * 4u + 3] = rg[ri + 3u];
                ri += 4u; si++;
            }
            if (k < cn) { break; }
        }
        if (si != nslots) { return 0; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    h = 2166136261u;
    for (si = 0; si < nslots * 4u; si++) {
        unsigned w = slots[si], b;
        for (b = 0; b < 4u; b++) {
            h ^= (w >> (b * 8u)) & 0xFFu;
            h *= 16777619u;
        }
    }
    p->hash = h; p->nslots = nslots;
    p->chk0 = nslots; p->chk1 = slots[1];
    for (s = 0; s < (int)nslots; s++) {
        vsh_decode_slot(&slots[(unsigned)s * 4u], &dec[s]);
    }
    /* W50 WASH (vsh_color): oD0-Quelle bestimmen (letzter MOV vN -> oD0).
     * oD0: volle Maske, orb=1, omux=0, oaddr=0x03 (belegt: P1/P2-Comp-
     * Programme, VSH-Dump W50_vsh); Quelle muss planes v-Register sein
     * (amux=2, kein Neg, Identitaets-Swizzle), sonst -1 (alter Pfad). */
    p->od0 = -1;
    for (s = 0; s < (int)nslots; s++) {
        if (dec[s].mac == 1 && dec[s].ilu == 0 && dec[s].omask == 0xFu &&
            dec[s].orb == 1 && dec[s].omux == 0 && dec[s].oaddr == 0x03u &&
            dec[s].amux == 2 && dec[s].aneg == 0 && dec[s].aswz == 0x1Bu) {
            p->od0 = (int)dec[s].vin;
        }
    }
    /* W90 FX-ORIG (fx_im_uv): oT0-Quelle wie oD0 (MOV vN -> oT0, oaddr 0x09,
     * volle Maske oder .xy). Effekt-Programme (..E131/E771/F421): oT0 = v3. */
    p->ot0 = -1;
    for (s = 0; s < (int)nslots; s++) {
        if (dec[s].mac == 1 && dec[s].ilu == 0 && (dec[s].omask == 0xFu || dec[s].omask == 0xCu) &&
            dec[s].orb == 1 && dec[s].omux == 0 && dec[s].oaddr == 0x09u &&
            dec[s].amux == 2 && dec[s].aneg == 0 && dec[s].aswz == 0x1Bu) {
            p->ot0 = (int)dec[s].vin;
        }
    }
    /* W84 TERRAIN-UV (im_uv_ot0): oT0 per ILU-MOV (omux=1, Quelle C = vN). */
    p->ot0i = -1;
    for (s = 0; s < (int)nslots; s++) {
        if (dec[s].ilu == 1 && (dec[s].omask == 0xFu || dec[s].omask == 0xCu) &&
            dec[s].orb == 1 && dec[s].omux == 1 && dec[s].oaddr == 0x09u &&
            dec[s].cmux == 2 && dec[s].cneg == 0 && dec[s].cswz == 0x1Bu) {
            p->ot0i = (int)dec[s].vin;
        }
    }
    for (s = 0; s < (int)nslots; s++) {
        if (dec[s].a0x) { return 1; } /* relativ -> alter Pfad */
    }
    /* P1: genau ein oPos-Schrieb: MOV v0 -> oPos.xyzw. */
    npos = 0; ok = 1;
    for (s = 0; s < (int)nslots; s++) {
        if (!vsh_out_opos(&dec[s])) { continue; }
        npos++;
        if (dec[s].omux != 0 || dec[s].omask != 0xF || dec[s].mac != 1) { ok = 0; break; }
        if (!vsh_in_v0(&dec[s], 0)) { ok = 0; break; }
    }
    if (ok && npos == 1) { p->kind = 1; return 1; }
    /* P2: oPos.x/y/z/w aus DP4(v0, cE..) (+Tail MUL/MAD als letzte oPos). */
    {
        int opos[136], npos2 = 0, t1, t2, ex = -1, ey = -1, ew = -1, zx = -1;
        int dp4z[12], dp4zp[12];
        int q, s2;
        for (q = 0; q < 12; q++) { dp4z[q] = -1; dp4zp[q] = -1; }
        for (s2 = 0; s2 < (int)nslots; s2++) {
            if (vsh_out_opos(&dec[s2])) { opos[npos2++] = s2; }
        }
        if (npos2 < 6) { return 1; }
        t1 = opos[npos2 - 2]; t2 = opos[npos2 - 1];
        /* Tail T1: MUL R12,c-38 -> oPos.xyz + RCC R12.w -> R1.x. */
        {
            vsh_dec_t *d = &dec[t1];
            if (d->mac != 2 || d->ilu != 3 || d->omux != 0 || d->omask != 0xE) { return 1; }
            if (!vsh_in_temp(d, 0, 12, 0x1B)) { return 1; }
            if (!vsh_in_const(d, 1, 0x1B) || d->craw != 58) { return 1; }
            if (d->ilumask != 8 || !vsh_in_temp(d, 2, 12, 0xFF)) { return 1; }
        }
        /* Tail T2: MAD R12,R1.x,c-37 -> oPos.xyz (letzter oPos-Schrieb). */
        {
            vsh_dec_t *d = &dec[t2];
            if (d->mac != 4 || d->ilu != 0 || d->omux != 0 || d->omask != 0xE) { return 1; }
            if (!vsh_in_temp(d, 0, 12, 0x1B)) { return 1; }
            if (!vsh_in_temp(d, 1, 1, 0x00)) { return 1; }
            if (!vsh_in_const(d, 2, 0x1B) || d->craw != 59) { return 1; }
        }
        for (q = t1 + 1; q < t2; q++) {
            if (vsh_writes_temp(&dec[q], 1, 8)) { return 1; }
        }
        /* R12 nur in T1 (A, C) und T2 (A) gelesen (zugewiesene Eingaenge). */
        for (q = 0; q < (int)nslots; q++) {
            vsh_dec_t *d = &dec[q];
            int useA = 0, useB = 0, useC = 0;
            if (d->mac == 1 || d->mac == 13) { useA = 1; }
            else if (d->mac == 2 || (d->mac >= 5 && d->mac <= 12)) { useA = 1; useB = 1; }
            else if (d->mac == 3) { useA = 1; useC = 1; }
            else if (d->mac == 4) { useA = 1; useB = 1; useC = 1; }
            if (d->ilu != 0) { useC = 1; }
            if (useA && d->amux == 1 && d->ar == 12 && q != t1 && q != t2) { return 1; }
            if (useB && d->bmux == 1 && d->br == 12) { return 1; }
            if (useC && d->cmux == 1 && d->cr == 12 && q != t1) { return 1; }
        }
        /* DP4-Temp-Tracking: R#.z = DP4(v0, cK).z (fuer oPos.z via Temp). */
        for (q = 0; q < (int)nslots; q++) {
            vsh_dec_t *d = &dec[q];
            if (d->mac == 7 && vsh_in_v0(d, 0) && vsh_in_const(d, 1, 0x1B)) {
                if ((d->macmask & 2) && d->outr < 12) {
                    if (dp4z[d->outr] != -1) { dp4z[d->outr] = -2; }
                    else { dp4z[d->outr] = (int)d->craw; dp4zp[d->outr] = q; }
                }
            }
        }
        for (q = 0; q < 12; q++) {
            if (dp4z[q] >= 0) {
                int w;
                for (w = 0; w < (int)nslots; w++) {
                    if (w != dp4zp[q] && vsh_writes_temp(&dec[w], (unsigned)q, 2)) { dp4z[q] = -2; break; }
                }
            }
        }
        /* Fruehe oPos-Schriebe: genau x,y,z,w (Masken 8/4/2/1). */
        for (k = 0; k < (unsigned)(npos2 - 2); k++) {
            vsh_dec_t *d = &dec[opos[k]];
            if (d->omux != 0) { return 1; }
            if (d->omask == 8) {
                if (d->mac != 7 || !vsh_in_v0(d, 0) || !vsh_in_const(d, 1, 0x1B)) { return 1; }
                if (ex != -1) { return 1; }
                ex = (int)d->craw;
            } else if (d->omask == 4) {
                if (d->mac != 7 || !vsh_in_v0(d, 0) || !vsh_in_const(d, 1, 0x1B)) { return 1; }
                if (ey != -1) { return 1; }
                ey = (int)d->craw;
            } else if (d->omask == 1) {
                if (d->mac != 7 || !vsh_in_v0(d, 0) || !vsh_in_const(d, 1, 0x1B)) { return 1; }
                if (ew != -1) { return 1; }
                ew = (int)d->craw;
            } else if (d->omask == 2) {
                if (zx != -1) { return 1; }
                zx = opos[k];
            } else { return 1; }
        }
        if (ex < 0 || ey != ex + 1 || ew != ex + 3 || zx < 0) { return 1; }
        if (ex + 3 > 191 || (ex <= 59 && ex + 3 >= 58)) { return 1; }
        /* oPos.z: direkt (DP4) oder via Temp (MOV/ADD R#.zzzz). */
        {
            vsh_dec_t *d = &dec[zx];
            if (d->mac == 7 && vsh_in_v0(d, 0) && vsh_in_const(d, 1, 0x1B) && (int)d->craw == ex + 2) {
                /* direkt, nichts zu tun */
            } else if (d->mac == 1 && d->amux == 1 && d->aneg == 0 && d->aswz == 0xAA) {
                int tt = (int)d->ar;
                if (tt < 0 || tt > 11 || dp4z[tt] != ex + 2 || dp4zp[tt] < 0 || dp4zp[tt] >= zx) { return 1; }
            } else if (d->mac == 3 && d->amux == 1 && d->aneg == 0 && d->aswz == 0xAA &&
                       vsh_in_const(d, 2, 0xAA)) {
                int tt = (int)d->ar;
                unsigned kk = d->craw;
                if (tt < 0 || tt > 11 || dp4z[tt] != ex + 2 || dp4zp[tt] < 0 || dp4zp[tt] >= zx) { return 1; }
                if (kk > 191 || kk == 58 || kk == 59) { return 1; }
                p->zadd = 1; p->zadde = kk;
            } else { return 1; }
        }
        p->kind = 2; p->rowe = (unsigned)ex;
        return 1;
    }
}

/* Programm je Handle einmal klassifizieren (Cache + Rekreations-Wache). */
const vsh_prog_t *vsh_program(unsigned handle)
{
    unsigned i;
    for (i = 0; i < g_vsh_ncache; i++) {
        if (g_vsh_cache[i].handle == handle) {
            int same = 0;
            __try {
                unsigned base = handle & ~1u;
                unsigned *hd = (unsigned *)(uintptr_t)base;
                unsigned *rg = (unsigned *)(uintptr_t)(base + 0x114u);
                if (hd[2] == g_vsh_cache[i].chk0 && rg[2] == g_vsh_cache[i].chk1) { same = 1; }
            } __except (EXCEPTION_EXECUTE_HANDLER) { same = 0; }
            if (same) { return &g_vsh_cache[i]; }
            {
                vsh_prog_t np;
                if (vsh_classify(handle, &np) && np.nslots) {
                    g_vsh_cache[i] = np;
                    return &g_vsh_cache[i];
                }
                return NULL;
            }
        }
    }
    if (g_vsh_ncache < 64u) {
        vsh_prog_t *np = &g_vsh_cache[g_vsh_ncache];
        if (vsh_classify(handle, np) && np->nslots) {
            if (g_cfg.vsh_dump) {
                HLOG_W("VSHEMU vs=0x%X hash=%08X kind=%d nslots=%u E=%u od0=%d%s",
                       handle, np->hash, np->kind, np->nslots, np->rowe, np->od0,
                       np->zadd ? " zadd" : "");
                HLOG_W("VSHOT0 vs=0x%X ot0=%d ot0i=%d", handle, np->ot0, np->ot0i); /* W90/W84 */
            }
            g_vsh_ncache++;
            return np;
        }
        return NULL;
    }
    {
        static vsh_prog_t vsh_tmp; /* nur bei >64 Programmen (praktisch nie) */
        if (vsh_classify(handle, &vsh_tmp) && vsh_tmp.nslots) { return &vsh_tmp; }
        return NULL;
    }
}

/* P2-Matrixzeilen aus dem CPU-Spiegel lesen (SEH). 1 = ok. */
int vsh_read_mat(const vsh_prog_t *vp, float m[4][4], float zadd[4])
{
    int ok = 0;
    __try {
        unsigned r;
        for (r = 0; r < 4u; r++) {
            const float *c = (const float *)(uintptr_t)(0x21BCA8u + (vp->rowe + r) * 16u);
            m[r][0] = c[0]; m[r][1] = c[1]; m[r][2] = c[2]; m[r][3] = c[3];
        }
        if (vp->zadd) {
            const float *c = (const float *)(uintptr_t)(0x21BCA8u + vp->zadde * 16u);
            zadd[0] = c[0]; zadd[1] = c[1]; zadd[2] = c[2]; zadd[3] = c[3];
        } else {
            zadd[0] = zadd[1] = zadd[2] = zadd[3] = 0.0f;
        }
        ok = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    return ok;
}

/* RCC: reziprok, geclampt (NV2A; vgl. Public-Domain-Referenz nv2a_vsh_cpu). */
float vsh_rcc(float x)
{
    float r;
    union { unsigned u; float f; } mx, mn;
    mx.u = 0x5F800000u; mn.u = 0xDF800000u;
    if (x == 1.0f) { return 1.0f; }
    r = 1.0f / x;
    if (r > 0.0f) {
        if (r < 5.42101e-020f) { r = 5.42101e-020f; }
        else if (r > mx.f) { r = mx.f; }
    } else {
        if (r < mn.f) { r = mn.f; }
        else if (r > -5.42101e-020f) { r = -5.42101e-020f; }
    }
    return r;
}
