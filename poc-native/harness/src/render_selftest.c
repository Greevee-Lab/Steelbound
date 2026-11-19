/* render_selftest.c - RENDER-QA (W77): Einzeltests fuer VSH-Interpreter,
 * VSH->HLSL-Uebersetzung und Combiner->HLSL-Uebersetzung.
 *  1. CPU-Interpreter (rqv_selftest): konstruierte Slots je Befehl/Sonderfall.
 *  2. GPU-VS: alle 27 Spiel-Programme (Mikrocode aus W77_cen1) uebersetzen,
 *     auf einem WARP-Device fuer Zufallseingaben ausfuehren und oD0/oD1/oFog/
 *     oT0..3 gegen den CPU-Interpreter pruefen.
 *  3. Combiner: alle 18 Spiel-PS kompilieren; konstruierte Faelle (Modulate,
 *     Licht-PS mit Nebel/Spekular, Dot/Mux/Ausgabeabbildung/Final-Summe)
 *     gegen von Hand nach NV2A-Formeln gerechnete Werte.
 * Aufruf: render_selftest.exe [default.xbe]  (Exit 0 = alle Tests ok,
 * 1 = FAIL, 2 = Umgebung: d3dcompiler/WARP/XBE fehlt).
 * W152 COMM-SELFTEST: privat kommen die Vektoren aus render_selftest_data.h
 * (privat, nicht im Export); community laedt sie zur Laufzeit aus der eigenen
 * default.xbe (Datei-Offsets + FNV-1a32 aus render_selftest_xbe.h, erzeugt von
 * scripts/selftest_vectors.py). Ohne XBE: SKIP (Exit 2) mit Hinweis. */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "render_vsh.h"
#include "render_psgen.h"
#include "render_cpersp_math.h"
#include "sb_edition.h"
#include "sb_texhash.h" /* W288 PACK-ZERO-TEX */
#include "render_interp_bb.h" /* W347 GHOST */
#include "render_interp_camfit.h" /* W349 LOC-FRAMES-2 */
#include "render_loclook.h" /* W358 LOC-LOOK */
#include "render_line_src.h" /* W352 ISSUE-EMP line_scale */
#include "render_interp_shref.h" /* W361 LOC-INTERP-CRASH */
#if SB_EDITION_COMMUNITY
#include "render_selftest_xbe.h"
/* Gleiche Formen wie in render_selftest_data.h, aber mit Lade-Puffern
 * statt eingebetteter Spielbytes (st_load_xbe fuellt sie aus der XBE). */
typedef struct { unsigned handle, n; unsigned *w; } st_vs_t;
typedef struct { unsigned handle; unsigned d[60]; } st_ps_t;
static st_vs_t st_vs[ST_XBE_NVS];
static unsigned st_vs_buf[ST_XBE_VS_WORDS];
static st_ps_t st_ps[ST_XBE_NPS];

static unsigned st_fnv1a(const unsigned char *p, size_t n)
{
    unsigned h = 0x811C9DC5u, i;
    for (i = 0; i < n; i++) { h ^= p[i]; h *= 0x01000193u; }
    return h;
}

/* XBE-Pfad: argv[1] > SB_SELFTEST_XBE > default.xbe neben der Exe.
 * Liefert 1 + Pfad in buf, sonst 0 (tried nennt den Kandidaten). */
static int st_find_xbe(int argc, char **argv, char *buf, size_t cap,
                       char *tried, size_t tcap)
{
    char exe[MAX_PATH], *bs;
    const char *env;
    FILE *f;
    tried[0] = '\0';
    if (argc > 1 && argv[1][0]) {
        _snprintf_s(tried, tcap, _TRUNCATE, "%s", argv[1]);
        f = NULL; fopen_s(&f, argv[1], "rb");
        if (f) { fclose(f); strcpy_s(buf, cap, argv[1]); return 1; }
        return 0;
    }
    env = NULL; _dupenv_s((char **)&env, NULL, "SB_SELFTEST_XBE");
    if (env && env[0]) {
        _snprintf_s(tried, tcap, _TRUNCATE, "%s", env);
        f = NULL; fopen_s(&f, env, "rb");
        if (f) { fclose(f); strcpy_s(buf, cap, env); free((void *)env); return 1; }
        free((void *)env);
        return 0;
    }
    free((void *)env);
    exe[0] = '\0';
    GetModuleFileNameA(NULL, exe, sizeof exe);
    bs = strrchr(exe, '\\');
    if (bs) {
        size_t nd = (size_t)(bs - exe) + 1;
        if (nd + 12 < cap) { memcpy(buf, exe, nd); strcpy_s(buf + nd, cap - nd, "default.xbe"); }
        else { strcpy_s(buf, cap, "default.xbe"); }
    } else {
        strcpy_s(buf, cap, "default.xbe");
    }
    _snprintf_s(tried, tcap, _TRUNCATE, "%s", buf);
    f = NULL; fopen_s(&f, buf, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}

/* Vektoren aus der XBE-Datei lesen + je Vektor per FNV-1a32 pruefen.
 * 1 = ok (dieselben Pruefungen wie privat laufen danach), 0 = FAIL. */
static int st_load_xbe(const char *path)
{
    unsigned char *img = NULL;
    long size;
    size_t i, used = 0;
    FILE *f = NULL;
    if (fopen_s(&f, path, "rb") || !f) { printf("XBE nicht lesbar: %s\n", path); return 0; }
    fseek(f, 0, SEEK_END); size = ftell(f); fseek(f, 0, SEEK_SET);
    if (size < 4) { fclose(f); printf("XBE zu klein: %s\n", path); return 0; }
    img = (unsigned char *)malloc((size_t)size);
    if (!img || fread(img, 1, (size_t)size, f) != (size_t)size) {
        fclose(f); free(img);
        printf("XBE nicht lesbar: %s\n", path); return 0;
    }
    fclose(f);
    if (memcmp(img, "XBEH", 4) != 0) {
        printf("FAIL: keine XBE (Magie fehlt): %s\n", path);
        free(img); return 0;
    }
    for (i = 0; i < ST_XBE_NVS; i++) {
        unsigned off = st_xbe_vs[i].off, nw = st_xbe_vs[i].nwords;
        if ((double)off + (double)nw * 4.0 > (double)size ||
            st_fnv1a(img + off, (size_t)nw * 4u) != st_xbe_vs[i].fnv) {
            printf("FAIL: VSH-Vektor 0x%X (off 0x%X, %u Woerter) ungueltig - "
                   "falsche XBE-Version? (%s)\n", st_xbe_vs[i].handle, off, nw, path);
            free(img); return 0;
        }
        st_vs[i].handle = st_xbe_vs[i].handle;
        st_vs[i].n = nw / 4u;
        st_vs[i].w = st_vs_buf + used;
        memcpy(st_vs_buf + used, img + off, (size_t)nw * 4u);
        used += nw;
    }
    for (i = 0; i < ST_XBE_NPS; i++) {
        unsigned off = st_xbe_ps[i].off, nw = st_xbe_ps[i].nwords;
        if (nw != 60 || (double)off + (double)nw * 4.0 > (double)size ||
            st_fnv1a(img + off, (size_t)nw * 4u) != st_xbe_ps[i].fnv) {
            printf("FAIL: PS-Vektor 0x%X (off 0x%X) ungueltig - "
                   "falsche XBE-Version? (%s)\n", st_xbe_ps[i].handle, off, path);
            free(img); return 0;
        }
        st_ps[i].handle = st_xbe_ps[i].handle;
        memcpy(st_ps[i].d, img + off, sizeof st_ps[i].d);
    }
    free(img);
    printf("Vektoren aus XBE: %s (%u VS + %u PS, Pruefsummen ok)\n",
           path, (unsigned)ST_XBE_NVS, (unsigned)ST_XBE_NPS);
    return 1;
}
#else
#include "render_selftest_data.h"
#endif

#pragma comment(lib, "d3d11.lib")

static ID3D11Device *g_d;
static ID3D11DeviceContext *g_c;
static int g_fail;

static void st_log(const char *s) { printf("%s\n", s); }

typedef HRESULT(WINAPI *fn_comp)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *,
                                 LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);
static fn_comp g_comp;

static ID3DBlob *compile(const char *src, const char *target)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = g_comp(src, strlen(src), NULL, NULL, NULL, "main", target,
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) {
        printf("COMPILE-FEHLER %s: %s\n", target, err ? (const char *)err->lpVtbl->GetBufferPointer(err) : "?");
        if (err) { err->lpVtbl->Release(err); }
        return NULL;
    }
    if (err) { err->lpVtbl->Release(err); }
    return code;
}

static unsigned g_rng = 12345u;
static float frand(float lo, float hi)
{
    g_rng = g_rng * 1664525u + 1013904223u;
    return lo + (hi - lo) * (float)((g_rng >> 8) & 0xFFFFu) / 65535.0f;
}

static ID3D11Buffer *mkbuf(UINT bind, const void *data, UINT size)
{
    D3D11_BUFFER_DESC bd;
    D3D11_SUBRESOURCE_DATA sd;
    ID3D11Buffer *b = NULL;
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = (size + 15u) & ~15u;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = bind;
    sd.pSysMem = data; sd.SysMemPitch = 0; sd.SysMemSlicePitch = 0;
    if (FAILED(ID3D11Device_CreateBuffer(g_d, &bd, data ? &sd : NULL, &b))) { return NULL; }
    return b;
}

/* 1x1-Ziele (float4) + Staging */
#define NRT 7
static ID3D11Texture2D *g_rt[NRT], *g_stg;
static ID3D11RenderTargetView *g_rtv[NRT];

static int mk_targets(void)
{
    D3D11_TEXTURE2D_DESC td;
    int i;
    memset(&td, 0, sizeof td);
    td.Width = 1; td.Height = 1; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    for (i = 0; i < NRT; i++) {
        if (FAILED(ID3D11Device_CreateTexture2D(g_d, &td, NULL, &g_rt[i]))) { return 0; }
        if (FAILED(ID3D11Device_CreateRenderTargetView(g_d, (ID3D11Resource *)g_rt[i], NULL, &g_rtv[i]))) { return 0; }
    }
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateTexture2D(g_d, &td, NULL, &g_stg))) { return 0; }
    return 1;
}

static void readrt(int i, float out[4])
{
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11DeviceContext_CopyResource(g_c, (ID3D11Resource *)g_stg, (ID3D11Resource *)g_rt[i]);
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_c, (ID3D11Resource *)g_stg, 0, D3D11_MAP_READ, 0, &m))) {
        memcpy(out, m.pData, 16);
        ID3D11DeviceContext_Unmap(g_c, (ID3D11Resource *)g_stg, 0);
    }
}

static void setvp(void)
{
    D3D11_VIEWPORT vp;
    vp.TopLeftX = 0; vp.TopLeftY = 0; vp.Width = 1; vp.Height = 1; vp.MinDepth = 0; vp.MaxDepth = 1;
    ID3D11DeviceContext_RSSetViewports(g_c, 1, &vp);
}

static float clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

static int near4(const float *a, const float *b, float tol)
{
    int k;
    for (k = 0; k < 4; k++) {
        float d = fabsf(a[k] - b[k]), s = fabsf(b[k]) > 1.0f ? fabsf(b[k]) : 1.0f;
        if (!(d <= tol * s)) { return 0; }
    }
    return 1;
}

/* ---------- 2: GPU-VS gegen CPU-Interpreter ---------- */
static const char *kVsOutPS =
    "struct PSIN { float4 pos : SV_Position; float4 col : COLOR0; float2 uv : TEXCOORD0;\n"
    "  float4 spec : COLOR1; float4 t0 : TEXCOORD1; float4 t1 : TEXCOORD2;\n"
    "  float4 t2 : TEXCOORD3; float4 t3 : TEXCOORD4; float fog : TEXCOORD5; };\n"
    "struct PO { float4 a : SV_Target0; float4 b : SV_Target1; float4 c : SV_Target2; float4 d : SV_Target3;\n"
    "  float4 e : SV_Target4; float4 f : SV_Target5; float4 g : SV_Target6; };\n"
    "PO main(PSIN i) { PO o; o.a = i.col; o.b = i.spec; o.c = i.t0; o.d = i.t1;\n"
    "  o.e = float4(i.fog, 0, 0, 0); o.f = i.t2; o.g = i.t3; return o; }\n";

static void test_vs_gpu(void)
{
    ID3DBlob *psb = compile(kVsOutPS, "ps_4_0");
    ID3D11PixelShader *ps = NULL;
    unsigned t, ok = 0, bad = 0;
    char line[256];
    if (!psb) { g_fail++; return; }
    ID3D11Device_CreatePixelShader(g_d, psb->lpVtbl->GetBufferPointer(psb), psb->lpVtbl->GetBufferSize(psb), NULL, &ps);
    psb->lpVtbl->Release(psb);
    for (t = 0; t < sizeof st_vs / sizeof st_vs[0]; t++) {
        rqv_prog_t p;
        static char src[65536];
        unsigned char cof[RQV_NCONST];
        unsigned nc = 0, k, nv = 0, rep;
        ID3DBlob *vsb;
        ID3D11VertexShader *vs = NULL;
        ID3D11InputLayout *lay = NULL;
        D3D11_INPUT_ELEMENT_DESC el[3 + 16];
        int prog_ok = 1;
        if (!rqv_decode(st_vs[t].w, st_vs[t].n, &p)) {
            snprintf(line, sizeof line, "VSGPU vs=0x%X dekodieren FAIL", st_vs[t].handle); st_log(line);
            g_fail++; continue;
        }
        if (!rqv_hlsl(&p, 0, src, sizeof src, cof, &nc)) { st_log("VSGPU HLSL zu lang"); g_fail++; continue; }
        vsb = compile(src, "vs_4_0");
        if (!vsb) { g_fail++; continue; }
        ID3D11Device_CreateVertexShader(g_d, vsb->lpVtbl->GetBufferPointer(vsb), vsb->lpVtbl->GetBufferSize(vsb), NULL, &vs);
        memset(el, 0, sizeof el);
        el[0].SemanticName = "POSITION"; el[0].Format = DXGI_FORMAT_R32G32B32A32_FLOAT; el[0].AlignedByteOffset = 0;
        el[1].SemanticName = "COLOR"; el[1].Format = DXGI_FORMAT_R32G32B32A32_FLOAT; el[1].AlignedByteOffset = 16;
        el[2].SemanticName = "TEXCOORD"; el[2].Format = DXGI_FORMAT_R32G32_FLOAT; el[2].AlignedByteOffset = 32;
        for (k = 0; k < 16u; k++) {
            if (p.vinmask & (1u << k)) {
                D3D11_INPUT_ELEMENT_DESC *e = &el[3 + nv];
                e->SemanticName = "ATTR"; e->SemanticIndex = nv; e->Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
                e->InputSlot = 2; e->AlignedByteOffset = nv * 16u; nv++;
            }
        }
        if (FAILED(ID3D11Device_CreateInputLayout(g_d, el, 3 + nv, vsb->lpVtbl->GetBufferPointer(vsb),
                                                  vsb->lpVtbl->GetBufferSize(vsb), &lay))) {
            st_log("VSGPU Layout FAIL"); g_fail++; vsb->lpVtbl->Release(vsb); continue;
        }
        vsb->lpVtbl->Release(vsb);
        for (rep = 0; rep < 8u; rep++) {
            float vin[16][4], c[192][4], cc[192][4], s0[10] = { 0, 0, 0.5f, 1, 0.3f, 0.6f, 0.9f, 1, 0.25f, 0.75f };
            float vcb[(192 + 2) * 4], mvp[20];
            float xs[16 * 4], got[NRT][4];
            rqv_out_t o;
            ID3D11Buffer *b0, *b3, *vb0, *vb2;
            UINT st0 = 40, st2 = nv ? nv * 16u : 16u, off = 0;
            unsigned q;
            for (k = 0; k < 16u; k++) {
                vin[k][0] = frand(-1, 1); vin[k][1] = frand(-1, 1); vin[k][2] = frand(-1, 1); vin[k][3] = frand(-1, 1);
            }
            vin[0][3] = 1.0f;
            for (k = 0; k < 192u; k++) {
                c[k][0] = frand(-1, 1); c[k][1] = frand(-1, 1); c[k][2] = frand(0.1f, 2); c[k][3] = frand(0, 1);
            }
            memcpy(cc, c, sizeof c);
            rqv_run(&p, (const float (*)[4])vin, cc, &o);
            memset(vcb, 0, sizeof vcb);
            for (k = 0; k < nc; k++) { memcpy(&vcb[k * 4], c[cof[k]], 16); }
            vcb[nc * 4 + 0] = 1.0f; vcb[nc * 4 + 1] = 1.0f; vcb[nc * 4 + 2] = 1.0f; /* Nebel: Tabelle NONE */
            vcb[nc * 4 + 4] = 1.0f; vcb[nc * 4 + 5] = 1.0f; vcb[nc * 4 + 6] = 1.0f; /* oD0/oT0/oD1 */
            memset(mvp, 0, sizeof mvp);
            mvp[0] = mvp[5] = mvp[10] = mvp[15] = 1.0f;
            nv = 0;
            for (k = 0; k < 16u; k++) { if (p.vinmask & (1u << k)) { memcpy(&xs[nv * 4], vin[k], 16); nv++; } }
            if (!nv) { memset(xs, 0, 16); }
            b0 = mkbuf(D3D11_BIND_CONSTANT_BUFFER, mvp, sizeof mvp);
            b3 = mkbuf(D3D11_BIND_CONSTANT_BUFFER, vcb, (nc + 2u) * 16u);
            vb0 = mkbuf(D3D11_BIND_VERTEX_BUFFER, s0, sizeof s0);
            vb2 = mkbuf(D3D11_BIND_VERTEX_BUFFER, xs, nv ? nv * 16u : 16u);
            ID3D11DeviceContext_IASetInputLayout(g_c, lay);
            ID3D11DeviceContext_IASetVertexBuffers(g_c, 0, 1, &vb0, &st0, &off);
            ID3D11DeviceContext_IASetVertexBuffers(g_c, 2, 1, &vb2, &st2, &off);
            ID3D11DeviceContext_IASetPrimitiveTopology(g_c, D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
            ID3D11DeviceContext_VSSetShader(g_c, vs, NULL, 0);
            ID3D11DeviceContext_VSSetConstantBuffers(g_c, 0, 1, &b0);
            ID3D11DeviceContext_VSSetConstantBuffers(g_c, 3, 1, &b3);
            ID3D11DeviceContext_PSSetShader(g_c, ps, NULL, 0);
            ID3D11DeviceContext_OMSetRenderTargets(g_c, NRT, g_rtv, NULL);
            setvp();
            {
                float z[4] = { -9, -9, -9, -9 };
                for (q = 0; q < NRT; q++) { ID3D11DeviceContext_ClearRenderTargetView(g_c, g_rtv[q], z); }
            }
            ID3D11DeviceContext_Draw(g_c, 1, 0);
            for (q = 0; q < NRT; q++) { readrt((int)q, got[q]); }
            {
                float wcol[4], wspec[4], wfog[4];
                for (q = 0; q < 4; q++) {
                    wcol[q] = clamp01(o.o[RQV_OD0][q]);
                    wspec[q] = clamp01(o.o[RQV_OD1][q]);
                }
                wfog[0] = clamp01(o.o[RQV_OFOG][0]); wfog[1] = wfog[2] = wfog[3] = 0;
                if (!near4(got[0], wcol, 2e-4f) || !near4(got[1], wspec, 2e-4f) ||
                    !near4(got[2], o.o[RQV_OT0], 2e-4f) || !near4(got[3], o.o[RQV_OT1], 2e-4f) ||
                    !near4(got[4], wfog, 2e-4f) || !near4(got[5], o.o[RQV_OT2], 2e-4f) ||
                    !near4(got[6], o.o[RQV_OT3], 2e-4f)) {
                    prog_ok = 0;
                    snprintf(line, sizeof line, "VSGPU vs=0x%X rep=%u FAIL oD0 gpu=(%g,%g,%g,%g) cpu=(%g,%g,%g,%g) oT1 gpu=(%g,%g,%g,%g) cpu=(%g,%g,%g,%g) fog %g/%g",
                             st_vs[t].handle, rep, got[0][0], got[0][1], got[0][2], got[0][3],
                             wcol[0], wcol[1], wcol[2], wcol[3], got[3][0], got[3][1], got[3][2], got[3][3],
                             o.o[RQV_OT1][0], o.o[RQV_OT1][1], o.o[RQV_OT1][2], o.o[RQV_OT1][3], got[4][0], wfog[0]);
                    st_log(line);
                }
            }
            ID3D11Buffer_Release(b0); ID3D11Buffer_Release(b3); ID3D11Buffer_Release(vb0); ID3D11Buffer_Release(vb2);
        }
        snprintf(line, sizeof line, "VSGPU vs=0x%X slots=%u vin=0x%X out=0x%X konst=%u %s",
                 st_vs[t].handle, st_vs[t].n, p.vinmask, p.outmask, nc, prog_ok ? "ok" : "FAIL");
        st_log(line);
        if (prog_ok) { ok++; } else { bad++; g_fail++; }
        ID3D11InputLayout_Release(lay);
        ID3D11VertexShader_Release(vs);
    }
    snprintf(line, sizeof line, "VSGPU: %u ok, %u FAIL", ok, bad);
    st_log(line);
    ID3D11PixelShader_Release(ps);
}


/* ---------- 2b: SHADE-PERF (W80) Roh-Variante: Decl-Formate + Expansion auf der GPU ---------- */
static const unsigned kRawFmts[] = { 0x42, 0x32, 0x22, 0x12, 0x72, 0x40, 0x41, 0x31, 0x21, 0x11,
                                     0x45, 0x35, 0x25, 0x15, 0x44, 0x34, 0x24, 0x14, 0x16, 0x02 };
/* CPU-Spiegel der Expansion (sb_im_submit): Ausgabe-Vertex id -> Quell-Index */
static unsigned raw_src(unsigned prim, unsigned id, unsigned n)
{
    unsigned t = id / 3u, k = id % 3u, q = id / 6u, j = id % 6u;
    static const unsigned m6[6] = { 0, 1, 2, 0, 2, 3 };
    switch (prim) {
    case 6: return t + k;
    case 7: case 10: return k == 0 ? 0 : t + k;
    case 8: return 4u * q + m6[j];
    case 9: return 2u * q + m6[j];
    case 3: return id >= n ? 0 : id;
    default: return id;
    }
}

static void test_vs_raw(void)
{
    ID3DBlob *psb = compile(kVsOutPS, "ps_4_0");
    ID3D11PixelShader *ps = NULL;
    unsigned t, ok = 0, bad = 0, ncase = 0;
    char line[320];
    if (!psb) { g_fail++; return; }
    ID3D11Device_CreatePixelShader(g_d, psb->lpVtbl->GetBufferPointer(psb), psb->lpVtbl->GetBufferSize(psb), NULL, &ps);
    psb->lpVtbl->Release(psb);
    for (t = 0; t < sizeof st_vs / sizeof st_vs[0]; t++) {
        rqv_prog_t p;
        rqv_raw_t raw;
        static char src[65536];
        unsigned char cof[RQV_NCONST];
        unsigned nc = 0, k, st[16], of[16], fm[16], o = 0, stride, nf = 0, rep;
        ID3DBlob *vsb;
        ID3D11VertexShader *vs = NULL;
        ID3D11InputLayout *lay = NULL;
        D3D11_INPUT_ELEMENT_DESC el[3];
        int prog_ok = 1;
        static unsigned char vdata[4096];
        float cur[16][4];
        if (!rqv_decode(st_vs[t].w, st_vs[t].n, &p) || !p.vinmask) { continue; }
        for (k = 0; k < 16u; k++) {
            st[k] = 0; of[k] = 0; fm[k] = 0x02;
            cur[k][0] = frand(-1, 1); cur[k][1] = frand(-1, 1); cur[k][2] = frand(-1, 1); cur[k][3] = frand(-1, 1);
            if (p.vinmask & (1u << k)) {
                fm[k] = kRawFmts[(t + nf++) % (sizeof kRawFmts / sizeof kRawFmts[0])];
                of[k] = o;
                o += rqv_fmt_size(fm[k]) + (k & 1u); /* auch unausgerichtete Versaetze */
            }
        }
        stride = o + 3u;
        if (!rqv_raw_build(st, of, fm, p.vinmask, &raw) || raw.need > stride) { st_log("VSRAW build FAIL"); g_fail++; continue; }
        if (!rqv_hlsl_ex(&p, 0, &raw, src, sizeof src, cof, &nc)) { st_log("VSRAW HLSL zu lang"); g_fail++; continue; }
        vsb = compile(src, "vs_4_0");
        if (!vsb) { g_fail++; continue; }
        ID3D11Device_CreateVertexShader(g_d, vsb->lpVtbl->GetBufferPointer(vsb), vsb->lpVtbl->GetBufferSize(vsb), NULL, &vs);
        memset(el, 0, sizeof el);
        el[0].SemanticName = "POSITION"; el[0].Format = DXGI_FORMAT_R32G32B32A32_FLOAT; el[0].AlignedByteOffset = 0;
        el[1].SemanticName = "COLOR"; el[1].Format = DXGI_FORMAT_R32G32B32A32_FLOAT; el[1].AlignedByteOffset = 16;
        el[2].SemanticName = "TEXCOORD"; el[2].Format = DXGI_FORMAT_R32G32_FLOAT; el[2].AlignedByteOffset = 32;
        if (FAILED(ID3D11Device_CreateInputLayout(g_d, el, 3, vsb->lpVtbl->GetBufferPointer(vsb),
                                                  vsb->lpVtbl->GetBufferSize(vsb), &lay))) {
            st_log("VSRAW Layout FAIL"); g_fail++; vsb->lpVtbl->Release(vsb); continue;
        }
        vsb->lpVtbl->Release(vsb);
        /* 8 Quell-Vertices: Float-Formate mit Zufallszahlen, sonst Zufallsbytes */
        for (k = 0; k < sizeof vdata; k++) { vdata[k] = (unsigned char)(frand(0, 255.99f)); }
        for (rep = 0; rep < 8u; rep++) {
            unsigned r2;
            for (r2 = 0; r2 < 16u; r2++) {
                unsigned f = fm[r2], q, nn = 0;
                if (!(p.vinmask & (1u << r2))) { continue; }
                if (f == 0x12 || f == 0x22 || f == 0x32 || f == 0x42) { nn = f >> 4; }
                if (f == 0x72) { nn = 3; }
                for (q = 0; q < nn; q++) { float v = frand(-1, 1); memcpy(vdata + 8 + rep * stride + of[r2] + 4 * q, &v, 4); }
            }
        }
        for (rep = 0; rep < 20u; rep++) {
            static const unsigned prims[6] = { 5, 6, 7, 8, 9, 3 };
            static const unsigned ibprims[4] = { 6, 7, 8, 9 };
            /* W264 DRAW-OPT2: rep 12..19 = DrawIndexed ueber das Strip/Fan/Quad-Muster (Index k = Quell-Vertex
             * des expandierten Vertex k), rq_raw.z = 5 -> SV_VertexID = Index; ab rep 16 mit Indexliste */
            unsigned ibm = (rep >= 12u);
            unsigned prim = ibm ? ibprims[rep % 4u] : prims[rep % 6u], n = 8, indexed = ibm ? (rep >= 16u) : (rep >= 6u), id, si, vi, q;
            unsigned idx[16];
            float vin[16][4], c[192][4], cc[192][4], vcb[(192 + 4 + 16) * 4], mvp[20], got[NRT][4];
            float s0[64 * 10];
            unsigned *vu;
            rqv_out_t out;
            ID3D11Buffer *b0, *b3, *vb0, *rb;
            ID3D11ShaderResourceView *srv = NULL;
            D3D11_SHADER_RESOURCE_VIEW_DESC vd;
            UINT st0 = 40, off = 0;
            static unsigned char rbuf[8192];
            if (prim == 8u) { n = 8; } /* 2 Quads */
            id = (rep * 7u + 5u) % (prim == 5u ? n : (prim == 3u ? n + 1u : 6u));
            for (q = 0; q < 16u; q++) { idx[q] = (q * 5u + 3u) % 8u; }
            si = raw_src(prim, id, n);
            vi = indexed ? idx[si] : si;
            memset(vin, 0, sizeof vin);
            for (q = 0; q < 16u; q++) {
                if (p.vinmask & (1u << q)) { rqv_vfetch(fm[q], vdata + 8 + vi * stride + of[q], cur[q], vin[q]); }
            }
            for (q = 0; q < 192u; q++) {
                c[q][0] = frand(-1, 1); c[q][1] = frand(-1, 1); c[q][2] = frand(0.1f, 2); c[q][3] = frand(0, 1);
            }
            memcpy(cc, c, sizeof c);
            rqv_run(&p, (const float (*)[4])vin, cc, &out);
            memset(vcb, 0, sizeof vcb);
            for (q = 0; q < nc; q++) { memcpy(&vcb[q * 4], c[cof[q]], 16); }
            vcb[nc * 4 + 0] = 1.0f; vcb[nc * 4 + 1] = 1.0f; vcb[nc * 4 + 2] = 1.0f;
            vcb[nc * 4 + 4] = 1.0f; vcb[nc * 4 + 5] = 1.0f; vcb[nc * 4 + 6] = 1.0f;
            vu = (unsigned *)&vcb[(nc + 2) * 4];
            vu[0] = 8; vu[1] = stride; vu[2] = ibm ? 5u : prim; vu[3] = n;
            vu[4] = indexed ? 1024u : 0xFFFFFFFFu;
            for (q = 0; q < 16u; q++) {
                if ((p.vinmask & (1u << q)) && raw.fmt[q] == 0) { memcpy(&vcb[(nc + 4 + raw.inl[q]) * 4], cur[q], 16); }
            }
            memset(mvp, 0, sizeof mvp);
            mvp[0] = mvp[5] = mvp[10] = mvp[15] = 1.0f;
            for (q = 0; q < 64u; q++) {
                float z[10] = { 0, 0, 0.5f, 1, 0.3f, 0.6f, 0.9f, 1, 0.25f, 0.75f };
                memcpy(&s0[q * 10], z, sizeof z);
            }
            memcpy(rbuf, vdata, sizeof vdata);
            memcpy(rbuf + 4096, idx, sizeof idx);
            b0 = mkbuf(D3D11_BIND_CONSTANT_BUFFER, mvp, sizeof mvp);
            b3 = mkbuf(D3D11_BIND_CONSTANT_BUFFER, vcb, (nc + 4u + (raw.ninl ? raw.ninl : 1u)) * 16u);
            vb0 = mkbuf(D3D11_BIND_VERTEX_BUFFER, s0, sizeof s0);
            rb = mkbuf(D3D11_BIND_SHADER_RESOURCE, rbuf, sizeof rbuf);
            memset(&vd, 0, sizeof vd);
            vd.Format = DXGI_FORMAT_R32_UINT; vd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            vd.Buffer.FirstElement = 0; vd.Buffer.NumElements = sizeof rbuf / 4u;
            ID3D11Device_CreateShaderResourceView(g_d, (ID3D11Resource *)rb, &vd, &srv);
            ID3D11DeviceContext_IASetInputLayout(g_c, lay);
            ID3D11DeviceContext_IASetVertexBuffers(g_c, 0, 1, &vb0, &st0, &off);
            ID3D11DeviceContext_IASetPrimitiveTopology(g_c, D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
            ID3D11DeviceContext_VSSetShader(g_c, vs, NULL, 0);
            ID3D11DeviceContext_VSSetConstantBuffers(g_c, 0, 1, &b0);
            ID3D11DeviceContext_VSSetConstantBuffers(g_c, 3, 1, &b3);
            ID3D11DeviceContext_VSSetShaderResources(g_c, 0, 1, &srv);
            ID3D11DeviceContext_PSSetShader(g_c, ps, NULL, 0);
            ID3D11DeviceContext_OMSetRenderTargets(g_c, NRT, g_rtv, NULL);
            setvp();
            {
                float z[4] = { -9, -9, -9, -9 };
                for (q = 0; q < NRT; q++) { ID3D11DeviceContext_ClearRenderTargetView(g_c, g_rtv[q], z); }
            }
            if (ibm) { /* W264: letzter Punkt = Index id = Quell-Vertex si */
                unsigned ibd[16];
                ID3D11Buffer *ib;
                for (q = 0; q < 16u; q++) { ibd[q] = raw_src(prim, q, n); }
                ib = mkbuf(D3D11_BIND_INDEX_BUFFER, ibd, sizeof ibd);
                ID3D11DeviceContext_IASetIndexBuffer(g_c, ib, DXGI_FORMAT_R32_UINT, 0);
                ID3D11DeviceContext_DrawIndexed(g_c, id + 1u, 0, 0);
                ID3D11DeviceContext_IASetIndexBuffer(g_c, NULL, DXGI_FORMAT_R32_UINT, 0);
                ID3D11Buffer_Release(ib);
            } else
            ID3D11DeviceContext_Draw(g_c, id + 1u, 0); /* letzter Punkt (vid = id) gewinnt */
            for (q = 0; q < NRT; q++) { readrt((int)q, got[q]); }
            {
                float wcol[4], wspec[4], wfog[4];
                for (q = 0; q < 4; q++) { wcol[q] = clamp01(out.o[RQV_OD0][q]); wspec[q] = clamp01(out.o[RQV_OD1][q]); }
                wfog[0] = clamp01(out.o[RQV_OFOG][0]); wfog[1] = wfog[2] = wfog[3] = 0;
                if (!near4(got[0], wcol, 2e-4f) || !near4(got[1], wspec, 2e-4f) ||
                    !near4(got[2], out.o[RQV_OT0], 2e-4f) || !near4(got[3], out.o[RQV_OT1], 2e-4f) ||
                    !near4(got[4], wfog, 2e-4f) || !near4(got[5], out.o[RQV_OT2], 2e-4f) ||
                    !near4(got[6], out.o[RQV_OT3], 2e-4f)) {
                    prog_ok = 0;
                    snprintf(line, sizeof line, "VSRAW%s vs=0x%X prim=%u id=%u idx=%u FAIL oD0 gpu=(%g,%g,%g,%g) cpu=(%g,%g,%g,%g) oT0 gpu=(%g,%g,%g,%g) cpu=(%g,%g,%g,%g)",
                             ibm ? "-IB" : "", st_vs[t].handle, prim, id, indexed, got[0][0], got[0][1], got[0][2], got[0][3],
                             wcol[0], wcol[1], wcol[2], wcol[3], got[2][0], got[2][1], got[2][2], got[2][3],
                             out.o[RQV_OT0][0], out.o[RQV_OT0][1], out.o[RQV_OT0][2], out.o[RQV_OT0][3]);
                    st_log(line);
                }
            }
            ncase++;
            ID3D11ShaderResourceView_Release(srv);
            ID3D11Buffer_Release(b0); ID3D11Buffer_Release(b3); ID3D11Buffer_Release(vb0); ID3D11Buffer_Release(rb);
        }
        {
            char fb[200];
            int fo = 0;
            for (k = 0; k < 16u && fo >= 0 && fo < 180; k++) {
                if (p.vinmask & (1u << k)) { fo += snprintf(fb + fo, sizeof fb - (size_t)fo, "v%u=%02X@%u ", k, raw.fmt[k], raw.off[k]); }
            }
            snprintf(line, sizeof line, "VSRAW vs=0x%X stride=%u %s%s", st_vs[t].handle, stride, fb, prog_ok ? "ok" : "FAIL");
        }
        st_log(line);
        if (prog_ok) { ok++; } else { bad++; g_fail++; }
        ID3D11InputLayout_Release(lay);
        ID3D11VertexShader_Release(vs);
    }
    snprintf(line, sizeof line, "VSRAW: %u ok, %u FAIL (%u Faelle: Formate, Expansion LIST/STRIP/FAN/QUADS/QUADSTRIP/LINELOOP, mit/ohne Index, W264 DrawIndexed-Muster)", ok, bad, ncase);
    st_log(line);
    ID3D11PixelShader_Release(ps);
}

/* ---------- 3: Combiner ---------- */
static const char *kPsVS =
    "cbuffer K : register(b5) { float4 kcol, kspec, kt0, kt1, kt2, kt3, kfog; };\n"
    "struct VSOUT { float4 pos : SV_Position; float4 col : COLOR0; float2 uv : TEXCOORD0;\n"
    "  float4 spec : COLOR1; float4 t0 : TEXCOORD1; float4 t1 : TEXCOORD2;\n"
    "  float4 t2 : TEXCOORD3; float4 t3 : TEXCOORD4; float fog : TEXCOORD5; };\n"
    "VSOUT main(float4 p : POSITION0) { VSOUT o; o.pos = p; o.col = kcol; o.uv = kt0.xy; o.spec = kspec;\n"
    "  o.t0 = kt0; o.t1 = kt1; o.t2 = kt2; o.t3 = kt3; o.fog = kfog.x; return o; }\n";

static ID3D11ShaderResourceView *mktex(const float c[4])
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA sd;
    ID3D11Texture2D *t = NULL;
    ID3D11ShaderResourceView *v = NULL;
    memset(&td, 0, sizeof td);
    td.Width = 1; td.Height = 1; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    sd.pSysMem = c; sd.SysMemPitch = 16; sd.SysMemSlicePitch = 0;
    ID3D11Device_CreateTexture2D(g_d, &td, &sd, &t);
    if (t) { ID3D11Device_CreateShaderResourceView(g_d, (ID3D11Resource *)t, NULL, &v); ID3D11Texture2D_Release(t); }
    return v;
}

typedef struct {
    const char *name;
    unsigned d[60];
    float col[4], spec[4], tex[4][4], fog;
    float pc0[8][4], pc1[8][4], fc0[4], fc1[4], fogc[4];
    float want[4];
} pstest_t;

static void run_ps(const pstest_t *t)
{
    static char src[65536];
    rqp_info_t info;
    ID3DBlob *vsb, *psb;
    ID3D11VertexShader *vs = NULL;
    ID3D11PixelShader *ps = NULL;
    ID3D11InputLayout *lay = NULL;
    D3D11_INPUT_ELEMENT_DESC el;
    float kb[7 * 4], pcb[(8 + 8 + 3 + 4 + 1 + 4) * 4] /* W121: + sfx0..sfx3 (0 = Original) */, got[4];
    float pos[4] = { 0, 0, 0.5f, 1 };
    ID3D11Buffer *bk, *bp, *vb;
    ID3D11ShaderResourceView *srv[4];
    ID3D11SamplerState *smp = NULL;
    D3D11_SAMPLER_DESC sd;
    UINT st = 16, off = 0;
    unsigned k;
    char line[300];
    if (!rqp_hlsl(t->d, 0xF, src, sizeof src, &info)) {
        snprintf(line, sizeof line, "PS  %-30s FAIL (Uebersetzung, unsupported=%d)", t->name, info.unsupported);
        st_log(line); g_fail++; return;
    }
    vsb = compile(kPsVS, "vs_4_0");
    psb = compile(src, "ps_4_0");
    if (!vsb || !psb) { g_fail++; printf("%s\n", src); return; }
    ID3D11Device_CreateVertexShader(g_d, vsb->lpVtbl->GetBufferPointer(vsb), vsb->lpVtbl->GetBufferSize(vsb), NULL, &vs);
    ID3D11Device_CreatePixelShader(g_d, psb->lpVtbl->GetBufferPointer(psb), psb->lpVtbl->GetBufferSize(psb), NULL, &ps);
    memset(&el, 0, sizeof el);
    el.SemanticName = "POSITION"; el.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    ID3D11Device_CreateInputLayout(g_d, &el, 1, vsb->lpVtbl->GetBufferPointer(vsb), vsb->lpVtbl->GetBufferSize(vsb), &lay);
    vsb->lpVtbl->Release(vsb); psb->lpVtbl->Release(psb);
    memcpy(&kb[0], t->col, 16); memcpy(&kb[4], t->spec, 16);
    for (k = 0; k < 4; k++) { kb[8 + k * 4 + 0] = 0.5f; kb[8 + k * 4 + 1] = 0.5f; kb[8 + k * 4 + 2] = 0; kb[8 + k * 4 + 3] = 1; }
    kb[24] = t->fog; kb[25] = kb[26] = kb[27] = 0;
    memset(pcb, 0, sizeof pcb);
    memcpy(&pcb[0], t->pc0, sizeof t->pc0); memcpy(&pcb[32], t->pc1, sizeof t->pc1);
    memcpy(&pcb[64], t->fc0, 16); memcpy(&pcb[68], t->fc1, 16); memcpy(&pcb[72], t->fogc, 16);
    for (k = 0; k < 4; k++) { pcb[76 + k * 4] = 1; pcb[76 + k * 4 + 1] = 1; pcb[76 + k * 4 + 2] = 1; }
    bk = mkbuf(D3D11_BIND_CONSTANT_BUFFER, kb, sizeof kb);
    bp = mkbuf(D3D11_BIND_CONSTANT_BUFFER, pcb, sizeof pcb);
    vb = mkbuf(D3D11_BIND_VERTEX_BUFFER, pos, sizeof pos);
    for (k = 0; k < 4; k++) { srv[k] = mktex(t->tex[k]); }
    memset(&sd, 0, sizeof sd);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11Device_CreateSamplerState(g_d, &sd, &smp);
    {
        ID3D11SamplerState *sm4[4] = { smp, smp, smp, smp };
        ID3D11DeviceContext_PSSetSamplers(g_c, 0, 4, sm4);
    }
    ID3D11DeviceContext_IASetInputLayout(g_c, lay);
    ID3D11DeviceContext_IASetVertexBuffers(g_c, 0, 1, &vb, &st, &off);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_c, D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    ID3D11DeviceContext_VSSetShader(g_c, vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(g_c, 5, 1, &bk);
    ID3D11DeviceContext_PSSetShader(g_c, ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(g_c, 4, 1, &bp);
    ID3D11DeviceContext_PSSetShaderResources(g_c, 0, 4, srv);
    ID3D11DeviceContext_OMSetRenderTargets(g_c, 1, &g_rtv[0], NULL);
    setvp();
    {
        float z[4] = { -9, -9, -9, -9 };
        ID3D11DeviceContext_ClearRenderTargetView(g_c, g_rtv[0], z);
    }
    ID3D11DeviceContext_Draw(g_c, 1, 0);
    readrt(0, got);
    {
        int ok = near4(got, t->want, 1e-4f);
        snprintf(line, sizeof line, "PS  %-30s %s got=(%.4f,%.4f,%.4f,%.4f) want=(%.4f,%.4f,%.4f,%.4f)",
                 t->name, ok ? "ok  " : "FAIL", got[0], got[1], got[2], got[3],
                 t->want[0], t->want[1], t->want[2], t->want[3]);
        st_log(line);
        if (!ok) { g_fail++; }
    }
    for (k = 0; k < 4; k++) { if (srv[k]) { ID3D11ShaderResourceView_Release(srv[k]); } }
    ID3D11SamplerState_Release(smp);
    ID3D11Buffer_Release(bk); ID3D11Buffer_Release(bp); ID3D11Buffer_Release(vb);
    ID3D11InputLayout_Release(lay); ID3D11VertexShader_Release(vs); ID3D11PixelShader_Release(ps);
}

static const unsigned *find_ps(unsigned h)
{
    unsigned i;
    for (i = 0; i < sizeof st_ps / sizeof st_ps[0]; i++) { if (st_ps[i].handle == h) { return st_ps[i].d; } }
    return NULL;
}

static void test_ps(void)
{
    static pstest_t t;
    unsigned i, nok = 0;
    char line[200];
    static char src[65536];
    /* alle Spiel-PS: Uebersetzung + Kompilieren */
    for (i = 0; i < sizeof st_ps / sizeof st_ps[0]; i++) {
        rqp_info_t info;
        ID3DBlob *b;
        int ok = rqp_hlsl(st_ps[i].d, 0xF, src, sizeof src, &info);
        b = ok ? compile(src, "ps_4_0") : NULL;
        snprintf(line, sizeof line, "PSC ps=0x%X stufen=%u texmodes=%u/%u/%u/%u final=%s nebel=%d %s",
                 st_ps[i].handle, info.nstages, info.texmode[0], info.texmode[1], info.texmode[2],
                 info.texmode[3], info.final_default ? "R0" : "ABCD", info.uses_fog, b ? "ok" : "FAIL");
        st_log(line);
        if (b) { b->lpVtbl->Release(b); nok++; } else { g_fail++; }
    }
    /* 3a: Modulate (PS 0xAB80510): V0*T0 */
    memset(&t, 0, sizeof t);
    t.name = "Modulate AB80510";
    memcpy(t.d, find_ps(0xAB80510), sizeof t.d);
    t.col[0] = 0.5f; t.col[1] = 0.25f; t.col[2] = 1.0f; t.col[3] = 0.8f;
    t.tex[0][0] = 0.5f; t.tex[0][1] = 1.0f; t.tex[0][2] = 0.2f; t.tex[0][3] = 0.5f;
    t.want[0] = 0.25f; t.want[1] = 0.25f; t.want[2] = 0.2f; t.want[3] = 0.4f;
    run_ps(&t);
    /* 3b: Licht-PS AB7F850: R1 = V0*T1 + C0; R0 = 2*(R1*T0); out = FOG.a*R0 +
     * (1-FOG.a)*FOG.rgb + V1, a = FOG.a */
    memset(&t, 0, sizeof t);
    t.name = "Licht AB7F850 (Nebel+V1)";
    memcpy(t.d, find_ps(0xAB7F850), sizeof t.d);
    t.col[0] = 0.6f; t.col[1] = 0.4f; t.col[2] = 0.2f; t.col[3] = 1.0f;
    t.tex[0][0] = 0.5f; t.tex[0][1] = 0.5f; t.tex[0][2] = 1.0f; t.tex[0][3] = 1.0f;
    t.tex[1][0] = 0.5f; t.tex[1][1] = 1.0f; t.tex[1][2] = 0.25f; t.tex[1][3] = 0.5f;
    t.pc0[0][0] = t.pc0[0][1] = t.pc0[0][2] = 0.1f; t.pc0[0][3] = 0.0f;
    t.fog = 0.75f; t.fogc[0] = 0.2f; t.fogc[1] = 0.3f; t.fogc[2] = 0.4f;
    t.spec[0] = 0.05f; t.spec[1] = 0.1f;
    t.want[0] = 0.4f; t.want[1] = 0.55f; t.want[2] = 0.325f; t.want[3] = 0.75f;
    run_ps(&t);
    /* 3c: Cockpit-PS AB81B60: R1 = V0 + C0; R0 = R1*T0; Nebel + V1 */
    memset(&t, 0, sizeof t);
    t.name = "Cockpit AB81B60";
    memcpy(t.d, find_ps(0xAB81B60), sizeof t.d);
    t.col[0] = 0.3f; t.col[1] = 0.5f; t.col[2] = 0.7f; t.col[3] = 0.9f;
    t.pc0[0][0] = 0.1f; t.pc0[0][1] = 0.2f; t.pc0[0][2] = 0.3f; t.pc0[0][3] = 0.1f;
    t.tex[0][0] = 1.0f; t.tex[0][1] = 0.5f; t.tex[0][2] = 0.5f; t.tex[0][3] = 0.5f;
    t.fog = 1.0f; t.spec[2] = 0.2f;
    t.want[0] = 0.4f; t.want[1] = 0.35f; t.want[2] = 0.7f; t.want[3] = 0.5f;
    run_ps(&t);
    /* 3d: konstruiert: Stufe 0 Dot (expand) + SHR1 -> R1, Alpha-Summe -> R0.a;
     * Stufe 1 MUX (R0.a >= 0.5 -> CD = C1); Final 1*R0 + R1, a = R0.a */
    memset(&t, 0, sizeof t);
    t.name = "Dot/Mux/SHR1/Final-Summe";
    t.d[0] = 0xD830D430u; t.d[26] = 0x00000C00u;
    t.d[34] = 0x48410000u; t.d[45] = 0x000320D0u;
    t.d[35] = 0xC420C220u; t.d[46] = 0x00004C00u;
    t.d[8] = 0x200C000Du; t.d[9] = 0x00001C00u;
    t.d[53] = 0x00011102u; t.d[54] = 1u;
    t.pc0[0][0] = 1.0f; t.pc0[0][1] = 0.5f; t.pc0[0][2] = 1.0f;
    t.pc1[1][0] = 0.1f; t.pc1[1][1] = 0.2f; t.pc1[1][2] = 0.3f;
    t.tex[0][0] = 1.0f; t.tex[0][1] = 0.5f; t.tex[0][2] = 0.75f; t.tex[0][3] = 0.6f;
    t.col[0] = 0.9f; t.col[1] = 0.8f; t.col[2] = 0.7f; t.col[3] = 0.3f;
    t.want[0] = 0.85f; t.want[1] = 0.95f; t.want[2] = 1.0f; t.want[3] = 0.9f;
    run_ps(&t);
    /* 3e: wie 3d, aber R0.a = 0.1 + 0.3 < 0.5 -> MUX nimmt AB = V0 */
    t.name = "Mux AB-Zweig";
    t.tex[0][3] = 0.1f;
    t.want[0] = 1.0f; t.want[1] = 1.0f; t.want[2] = 1.0f; t.want[3] = 0.4f;
    t.d[8] = 0x200C0000u; /* Final ohne R1: out = R0 = V0 */
    t.want[0] = 0.9f; t.want[1] = 0.8f; t.want[2] = 0.7f;
    run_ps(&t);
    /* 3f: Final mit EF_PROD/V1R0_SUM + Komplement: A=1, B=PROD(E=V0,F=T0),
     * C=0, D=SUM(1-V1 + R0, geklemmt) ; R0 = (0,0,0,T0.a) (keine Stufe schreibt)
     * -> out = V0*T0 + sat(1-V1 + 0) */
    memset(&t, 0, sizeof t);
    t.name = "Final PROD/SUM/Komplement";
    t.d[53] = 0x00010101u; t.d[54] = 1u;
    t.d[8] = 0x200F000Eu; t.d[9] = 0x0408DCC0u; /* E=V0 F=T0 G=R0.a, CLAMP+COMPL_V1 */
    t.col[0] = 0.5f; t.col[1] = 0.5f; t.col[2] = 0.5f; t.col[3] = 1.0f;
    t.tex[0][0] = 0.2f; t.tex[0][1] = 0.4f; t.tex[0][2] = 0.6f; t.tex[0][3] = 0.7f;
    t.spec[0] = 0.9f; t.spec[1] = 0.5f; t.spec[2] = 0.0f;
    t.want[0] = 0.1f + 0.1f; t.want[1] = 0.2f + 0.5f; t.want[2] = 1.0f; t.want[3] = 0.7f;
    run_ps(&t);
    snprintf(line, sizeof line, "PSC: %u von %u Spiel-PS kompiliert", nok, (unsigned)(sizeof st_ps / sizeof st_ps[0]));
    st_log(line);
}

/* W209: independent geometry oracle: a decal at the same metric depth
 * must remain aligned with its mesh, and pass LESS_EQUAL with the live bias.
 * Reprojecting a previously recorded mesh must match direct projection. */
static void test_cpersp(void)
{
    const float al = 100.0f / 99.9f, be = -0.1f * al;
    const float distances[] = { 0.0f, -1.5f, -3.0f };
    unsigned old, now, j;
    int fail = 0;
    for (old = 0; old < 3; old++) {
        for (now = 0; now < 3; now++) {
            float base[16] = {0}, direct[16], replay[16];
            float d0 = distances[old], d1 = distances[now];
            float k0 = (0.75f - d0) / 0.75f, k1 = (0.75f - d1) / 0.75f;
            float p[4] = {0.3f, -0.2f, al * 0.7f + be, 0.7f};
            float flat[4] = {0.3f, 0.2f, 0.034f, 1.0f}, copy[4];
            float w = 0.7f - d1, x, y, z;
            base[0] = base[5] = 1.0f; base[10] = al;
            base[11] = 1.0f; base[14] = be;
            memcpy(direct, base, sizeof base); memcpy(replay, base, sizeof base);
            cp_mvp_redo(direct, 0.0f, 1.0f, d1, k1, NULL, NULL);
            cp_mvp_redo(replay, 0.0f, 1.0f, d0, k0, NULL, NULL);
            cp_mvp_redo(replay, d0, k0, d1, k1, NULL, NULL);
            for (j = 0; j < 16; j++) { if (fabsf(direct[j] - replay[j]) > 2e-5f) { fail++; } }
            cp_p2_apply(p, 4, 1, d1, k1, al, be, 0.1f);
            x = 0.3f * direct[0] / w;
            y = -0.2f * direct[5] / w;
            z = (0.7f * direct[10] + direct[14]) / w;
            if (fabsf(p[0] / p[3] - x) > 1e-6f || fabsf(p[1] / p[3] - y) > 1e-6f ||
                fabsf(z - p[2] / p[3] - (d1 ? 2e-5f : 0.0f)) > 1e-6f) { fail++; }
            memcpy(copy, flat, sizeof flat);
            cp_p2_apply(flat, 4, 1, d1, k1, al, be, 0.1f);
            if (memcmp(copy, flat, sizeof flat)) { fail++; }
            /* The old near plane remains at z=0 even after moving back. */
            if (fabsf(0.1f * direct[10] + direct[14]) > 2e-6f) { fail++; }
        }
    }
    printf("CPERSP: 9 camera pairs, alignment/depth/near/flat: %d Fehler\n", fail);
    g_fail += fail;
}

/* W288 PACK-ZERO-TEX: Null-Texturen bekommen keinen HD-Ersatz.
 * Prueft den Client-Code aus sb_texhash.h (Hash, Null-Pruefung, Tor):
 * (a) die beiden W262-Pack-Hashes sind genau genullte LIN_A8R8G8B8 256x256/64x64,
 * (b) Nachbildung von sb_rem_upload mit einem Schein-Pack, das den Null-Hash und
 *     den Hash derselben Textur mit Daten enthaelt: Null -> kein Ersatz, mit
 *     Daten -> Ersatz, hd_zero_skip=0 -> alter Weg (Null wird ersetzt),
 * (c) Pitch-Polster zaehlt nicht, ein einzelnes Byte am Ende zaehlt,
 *     DXT1/ent-swizzelt ebenso. */
static int tz_replaced(int zero_skip, int kind, unsigned code, unsigned w, unsigned h,
                       const unsigned char *pix, unsigned pitch,
                       const unsigned long long *pack, int npack)
{
    int ok = 0, i;
    unsigned long long hv = tex_content_hash(kind, code, w, h, pix, pitch, &ok);
    int zero = zero_skip ? tex_mip0_zero(kind, code, w, h, pix, pitch) : 0;
    if (!ok || !tex_hd_gate(zero_skip, 0, zero)) { return 0; }
    for (i = 0; i < npack; i++) { if (pack[i] == hv) { return 1; } }
    return 0;
}

static void test_texzero(void)
{
    static unsigned char buf[256 * 4 * 256 + 64 * 256];
    unsigned long long pack[4];
    int ok = 0, fail = 0, n = 0;
    unsigned pitch;
    /* (a) W262-Fund: Hash genullten Speichers */
    memset(buf, 0, sizeof buf);
    n++; if (tex_content_hash(2, 0x12u, 256, 256, buf, 1024, &ok) != 0x3c2dbd7a7c6c2a55ull || !ok) { fail++; }
    n++; if (tex_content_hash(2, 0x12u, 64, 64, buf, 256, &ok) != 0x338471d56d184e05ull || !ok) { fail++; }
    n++; if (!tex_mip0_zero(2, 0x12u, 256, 256, buf, 1024)) { fail++; }
    /* (b) Schein-Pack: W262-Null-Eintraege + dieselbe 256x256 mit Daten */
    pack[0] = 0x3c2dbd7a7c6c2a55ull;
    pack[1] = 0x338471d56d184e05ull;
    buf[1024 * 100 + 37] = 0x80;
    pack[2] = tex_content_hash(2, 0x12u, 256, 256, buf, 1024, &ok);
    pack[3] = 0;
    memset(buf, 0, sizeof buf);
    n++; if (tz_replaced(1, 2, 0x12u, 256, 256, buf, 1024, pack, 3)) { fail++; }      /* Null: kein Ersatz */
    n++; if (tz_replaced(1, 2, 0x12u, 64, 64, buf, 256, pack, 3)) { fail++; }
    n++; if (!tz_replaced(0, 2, 0x12u, 256, 256, buf, 1024, pack, 3)) { fail++; }     /* alt: Null ersetzt */
    buf[1024 * 100 + 37] = 0x80;                                                       /* Daten kommen an */
    n++; if (!tz_replaced(1, 2, 0x12u, 256, 256, buf, 1024, pack, 3)) { fail++; }     /* neuer Hash: Ersatz */
    n++; if (tex_mip0_zero(2, 0x12u, 256, 256, buf, 1024)) { fail++; }
    /* (c) Pitch-Polster ausserhalb des gehashten Bereichs */
    memset(buf, 0, sizeof buf);
    pitch = 64 * 4 + 64;
    {
        unsigned y;
        for (y = 0; y < 64; y++) { memset(buf + (size_t)y * pitch + 64 * 4, 0xAB, 64); }
    }
    n++; if (!tex_mip0_zero(2, 0x12u, 64, 64, buf, pitch)) { fail++; }
    n++; if (tex_content_hash(2, 0x12u, 64, 64, buf, pitch, &ok) != 0x338471d56d184e05ull) { fail++; }
    buf[63 * pitch + 64 * 4 - 1] = 1; /* letztes gehashtes Byte */
    n++; if (tex_mip0_zero(2, 0x12u, 64, 64, buf, pitch)) { fail++; }
    /* DXT1 64x64 (16 Zeilen a 128 B) und ent-swizzelt 32 Bit 16x16 */
    memset(buf, 0, sizeof buf);
    n++; if (!tex_mip0_zero(1, 0x0Cu, 64, 64, buf, 128)) { fail++; }
    n++; if (tex_content_hash(1, 0x0Cu, 64, 64, buf, 128, &ok) != 0x8268e1d073b9b31bull) { fail++; }
    n++; if (!tex_mip0_zero(3, 0x06u, 16, 16, buf, 64)) { fail++; }
    buf[15 * 128 + 127] = 0x10;
    n++; if (tex_mip0_zero(1, 0x0Cu, 64, 64, buf, 128)) { fail++; }
    /* Tor: per CPU beschrieben nie, leerer Bereich nie "null" */
    n++; if (tex_hd_gate(1, 1, 0) || tex_hd_gate(0, 1, 0) || !tex_hd_gate(1, 0, 0) || !tex_hd_gate(0, 0, 1)) { fail++; }
    n++; if (tex_mip0_zero(2, 0x12u, 0, 0, buf, 0) || tex_mip0_zero(2, 0x12u, 4, 4, NULL, 16)) { fail++; }
    printf("TEXZERO (W288): %d Faelle (Null-Hash W262, Schein-Pack, Polster, DXT1/swz, Tor): %d Fehler\n", n, fail);
    g_fail += fail;
}

/* ---- W347 GHOST: Billboard-Paarung per Kamera (render_interp_bb.h) ----
 * Baeume als je Tick zur Kamera ausgerichtete Quads (Sichtraum-Vertices, Matrix = Projektion, wie im Spiel:
 * Matrix gleich, Eingangsdaten neu). Kamera dreht 3 Grad je Tick (schneller Schwenk) und faehrt 0,3 m vor;
 * Tick N in umgekehrter Reihenfolge. Soll: jeder Baum findet genau sich selbst, auch neben einem 2 m entfernten
 * Nachbarn; ein bewegter Baum (5 m versetzt) findet keinen; stehende Kamera: Fehler ~0. */
static void bb_mul(double r[16], const double a[16], const double b[16])
{
    int i, j, k;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            double s = 0.0;
            for (k = 0; k < 4; k++) { s += a[i * 4 + k] * b[k * 4 + j]; }
            r[i * 4 + j] = s;
        }
    }
}
static int bb_inv(double out[16], const double m[16])
{
    double a[4][8];
    int i, j, k;
    for (i = 0; i < 4; i++) { for (j = 0; j < 4; j++) { a[i][j] = m[i * 4 + j]; a[i][j + 4] = (i == j); } }
    for (i = 0; i < 4; i++) {
        int pv = i;
        double d;
        for (k = i + 1; k < 4; k++) { if (fabs(a[k][i]) > fabs(a[pv][i])) { pv = k; } }
        if (fabs(a[pv][i]) < 1e-12) { return 0; }
        for (j = 0; j < 8; j++) { double t = a[i][j]; a[i][j] = a[pv][j]; a[pv][j] = t; }
        d = a[i][i];
        for (j = 0; j < 8; j++) { a[i][j] /= d; }
        for (k = 0; k < 4; k++) { if (k != i) { double f = a[k][i]; for (j = 0; j < 8; j++) { a[k][j] -= f * a[i][j]; } } }
    }
    for (i = 0; i < 4; i++) { for (j = 0; j < 4; j++) { out[i * 4 + j] = a[i][j + 4]; } }
    return 1;
}
/* Sicht-Matrix (Zeilenvektoren): Welt -> Sicht fuer Kamera an (cx,0,cz) mit Gierwinkel deg */
static void bb_view(double v[16], double cx, double cz, double deg)
{
    double t = deg * 3.14159265358979 / 180.0, c = cos(t), s = sin(t);
    double T[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 }, R[16] = { 0 };
    T[12] = -cx; T[14] = -cz;
    /* Drehung um y um -t: x' = c*x - s*z, z' = s*x + c*z (Zeilenvektor p*R) */
    R[0] = c; R[2] = s; R[5] = 1.0; R[8] = -s; R[10] = c; R[15] = 1.0;
    bb_mul(v, T, R);
}
/* Baum-Quad im Sichtraum: 4 Bezugs-Vertices (Liste n=6: Index 0, 2, 4, 5) */
static void bb_tree(float pr[4][4], const double V[16], double x, double z)
{
    static const double ex[4][2] = { { -3, -4 }, { 3, 4 }, { 3, 4 }, { 3, -4 } };
    double w[4] = { x, 4.0, z, 1.0 }, c[4];
    int q, j;
    for (j = 0; j < 4; j++) { c[j] = w[0] * V[j] + w[1] * V[4 + j] + w[2] * V[8 + j] + w[3] * V[12 + j]; }
    for (q = 0; q < 4; q++) {
        pr[q][0] = (float)(c[0] + ex[q][0]); pr[q][1] = (float)(c[1] + ex[q][1]); pr[q][2] = (float)c[2]; pr[q][3] = 1.0f;
    }
}
static int bb_pick(float pa[][4][4], int na, const float P[16], const float pb[4][4], const double D[16], double *ecb)
{
    int k, best = -1;
    double eb = 1e30;
    for (k = 0; k < na; k++) {
        double ec, ev;
        ipbb_err((const float (*)[4])pa[k], P, pb, P, D, &ec, &ev);
        if (ipbb_ok(ec, ev) && ec < eb) { eb = ec; best = k; }
    }
    if (ecb) { *ecb = eb; }
    return best;
}
static void test_bbcam(void)
{
    static const double trees[7][2] = { { -20, 60 }, { -18, 61 }, { 0, 80 }, { 15, 50 }, { 30, 120 }, { -40, 90 }, { 4, 15 } };
    enum { NT = 7 };
    float P[16] = { 0 }, pa[NT][4][4], pb[4][4];
    double VA[16], VB[16], PA[16], PB[16], Pd[16] = { 0 }, IA[16], D[16], ec;
    int n = 0, fail = 0, k, r;
    double n0 = 1.0, f0 = 1000.0, ys = 1.0 / tan(30.0 * 3.14159265358979 / 180.0);
    Pd[0] = ys; Pd[5] = ys; Pd[10] = f0 / (f0 - n0); Pd[11] = 1.0; Pd[14] = -n0 * f0 / (f0 - n0);
    for (k = 0; k < 16; k++) { P[k] = (float)Pd[k]; }
    /* (a) Schwenk 3 Grad + 0,3 m Fahrt, Tick N in umgekehrter Reihenfolge */
    bb_view(VA, 0.0, 0.0, 0.0);
    bb_view(VB, 0.0, 0.3, 3.0);
    bb_mul(PA, VA, Pd); bb_mul(PB, VB, Pd);
    if (!bb_inv(IA, PA)) { fail++; }
    bb_mul(D, IA, PB); /* D = M_A^-1 * M_B der Welt-Geometrie (Kamera des Ziels) */
    for (k = 0; k < NT; k++) { bb_tree(pa[k], VA, trees[k][0], trees[k][1]); }
    for (k = NT - 1; k >= 0; k--) {
        bb_tree(pb, VB, trees[k][0], trees[k][1]);
        r = bb_pick(pa, NT, P, (const float (*)[4])pb, D, &ec);
        n++; if (r != k) { fail++; printf("BBCAM Schwenk: Baum %d -> %d (ec=%.5f)\n", k, r, ec); }
    }
    /* (b) bewegter Baum (5 m versetzt): kein Partner */
    bb_tree(pb, VB, trees[0][0] - 5.0, trees[0][1]);
    r = bb_pick(pa, NT, P, (const float (*)[4])pb, D, &ec);
    n++; if (r >= 0) { fail++; printf("BBCAM bewegt: Partner %d (ec=%.5f)\n", r, ec); }
    /* (c) Reihenfolge-Partner (anderer Baum) wird abgelehnt: Baum 2 gegen Kandidat 3 allein */
    bb_tree(pb, VB, trees[2][0], trees[2][1]);
    r = bb_pick(&pa[3], 1, P, (const float (*)[4])pb, D, &ec);
    n++; if (r >= 0) { fail++; printf("BBCAM fremd: angenommen (ec=%.5f)\n", ec); }
    /* (d) stehende Kamera: D = I, Fehler ~0 */
    for (k = 0; k < 16; k++) { D[k] = (k % 5) == 0 ? 1.0 : 0.0; }
    for (k = 0; k < NT; k++) {
        r = bb_pick(pa, NT, P, (const float (*)[4])pa[k], D, &ec);
        n++; if (r != k || ec > 1e-6) { fail++; printf("BBCAM still: Baum %d -> %d (ec=%.6f)\n", k, r, ec); }
    }
    /* (e) sehr schneller Schwenk 8 Grad je Tick (240 Grad/s): naher Baum (15 m) bleibt eindeutig */
    bb_view(VB, 0.0, 0.0, 8.0);
    bb_mul(PB, VB, Pd);
    bb_mul(D, IA, PB);
    bb_tree(pb, VB, trees[6][0], trees[6][1]);
    r = bb_pick(pa, NT, P, (const float (*)[4])pb, D, &ec);
    n++; if (r != 6) { fail++; printf("BBCAM schnell: Baum 6 -> %d (ec=%.5f)\n", r, ec); }
    printf("BBCAM (W347): %d Faelle (Schwenk/Reihenfolge, bewegt, fremd, still, schnell): %d Fehler\n", n, fail);
    g_fail += fail;
}

/* W349 LOC-FRAMES-2: Kamera-Pruefung der Zwischenbild-Zuordnung (render_interp_camfit.h). Kamera dreht sich um
 * die Hochachse (Gierwinkel), Projektion 90 Grad, Zeilenvektoren wie im Spiel (clip = p * M). */
static void cf_mat(float m[16], double yaw, double tx)
{
    double c = cos(yaw), s = sin(yaw), f = 1.0, zn = 1.0, zf = 1000.0;
    double V[16] = { c, 0, -s, 0,  0, 1, 0, 0,  s, 0, c, 0,  -tx * c, 0, tx * s, 1 };
    double P[16] = { f, 0, 0, 0,  0, f, 0, 0,  0, 0, zf / (zf - zn), 1,  0, 0, -zn * zf / (zf - zn), 0 };
    int r, k, j;
    for (r = 0; r < 4; r++) {
        for (k = 0; k < 4; k++) {
            double a = 0.0;
            for (j = 0; j < 4; j++) { a += V[r * 4 + j] * P[j * 4 + k]; }
            m[r * 4 + k] = (float)a;
        }
    }
}
static int cf_inv(double out[16], const float m[16]) /* Gauss-Jordan wie zc_dmat */
{
    double a[4][8];
    int i, j, k;
    for (i = 0; i < 4; i++) { for (j = 0; j < 4; j++) { a[i][j] = m[i * 4 + j]; a[i][j + 4] = (i == j); } }
    for (i = 0; i < 4; i++) {
        int pv = i;
        double d;
        for (k = i + 1; k < 4; k++) { if (fabs(a[k][i]) > fabs(a[pv][i])) { pv = k; } }
        if (fabs(a[pv][i]) < 1e-12) { return 0; }
        for (j = 0; j < 8; j++) { double t = a[i][j]; a[i][j] = a[pv][j]; a[pv][j] = t; }
        d = a[i][i];
        for (j = 0; j < 8; j++) { a[i][j] /= d; }
        for (k = 0; k < 4; k++) { if (k != i) { double f = a[k][i]; for (j = 0; j < 8; j++) { a[k][j] -= f * a[i][j]; } } }
    }
    for (i = 0; i < 4; i++) { for (j = 0; j < 4; j++) { out[i * 4 + j] = a[i][j + 4]; } }
    return 1;
}
static void test_camfit(void)
{
    float MA[16], MB[16], pa[4][4], pb[4][4], pc[4][4];
    double Ai[16], D[1][16];
    int fail = 0, n = 0, q, r, k;
    cf_mat(MA, 0.0, 0.0);
    cf_mat(MB, 0.15, 0.3); /* 20-Hz-Tick bei schneller Drehung: ~8,6 Grad + Versatz */
    cf_inv(Ai, MA);
    for (r = 0; r < 4; r++) {
        for (k = 0; k < 4; k++) {
            double a = 0.0;
            for (q = 0; q < 4; q++) { a += Ai[r * 4 + q] * MB[q * 4 + k]; }
            D[0][r * 4 + k] = a;
        }
    }
    /* (a) ruhender Gelaendestreifen: gleiche Vertices, Matrix A/B -> ~0 */
    for (q = 0; q < 4; q++) { pa[q][0] = -20.0f + 13.0f * q; pa[q][1] = -3.0f; pa[q][2] = 40.0f + 5.0f * q; pa[q][3] = 1.0f; }
    n++; if (!(ipcf_camfit(pa, MA, pa, MB, D, 1) < 1e-4)) { fail++; }
    /* (b) Billboard (Wolken-Sprite, 6 Vertices -> 4 Bezugs-Vertices) neu zur Kamera gedreht: klein (< 0,25) */
    for (q = 0; q < 4; q++) {
        double ang = 0.15 * 0.5, cx = 30.0, cz = 300.0, ox = (q & 1) ? 12.0 : -12.0, oy = (q & 2) ? 8.0 : -8.0;
        pa[q][0] = (float)(cx + ox); pa[q][1] = (float)(40.0 + oy); pa[q][2] = (float)cz; pa[q][3] = 1.0f;
        pb[q][0] = (float)(cx + ox * cos(ang)); pb[q][1] = (float)(40.0 + oy); pb[q][2] = (float)(cz + ox * sin(ang)); pb[q][3] = 1.0f;
    }
    n++; if (!(ipcf_camfit(pa, MA, pb, MB, D, 1) < 0.25)) { fail++; }
    /* (c) falscher Partner (anderer Sprite 60 Einheiten daneben): gross */
    for (q = 0; q < 4; q++) { pc[q][0] = pa[q][0] + 60.0f; pc[q][1] = pa[q][1]; pc[q][2] = pa[q][2]; pc[q][3] = 1.0f; }
    n++; if (!(ipcf_camfit(pc, MA, pb, MB, D, 1) > 0.25)) { fail++; }
    /* (d) Streifen hinter der Kamera (w < 0), ruhend: klein; (e) w-Seitenwechsel: unbrauchbar */
    for (q = 0; q < 4; q++) { pa[q][0] = -5.0f + 3.0f * q; pa[q][1] = -2.0f; pa[q][2] = -30.0f - 4.0f * q; pa[q][3] = 1.0f; }
    n++; if (!(ipcf_camfit(pa, MA, pa, MB, D, 1) < 1e-4)) { fail++; }
    pb[0][0] = 0.0f; pb[0][1] = 0.0f; pb[0][2] = 10.0f; pb[0][3] = 1.0f;
    for (q = 1; q < 4; q++) { memcpy(pb[q], pa[q], sizeof pb[q]); }
    n++; if (!(ipcf_camfit(pa, MA, pb, MB, D, 1) >= 1e9)) { fail++; }
    /* (f) Matrix-Lerp-Test (Tick-N-Vertices mit M_A): ruhendes Gitter knapp ueber der Matrix-Schwelle (Rundung) */
    {
        float MB2[16];
        memcpy(MB2, MB, sizeof MB2);
        MB2[12] *= 1.003f; MB2[14] *= 1.003f;
        for (q = 0; q < 4; q++) { pa[q][0] = -200.0f + 130.0f * q; pa[q][1] = -3.0f; pa[q][2] = 400.0f + 50.0f * q; pa[q][3] = 1.0f; }
        n++; if (!(ipcf_camfit(pa, MA, pa, MB2, D, 1) < 0.05)) { fail++; }
    }
    printf("CAMFIT (W349): %d Faelle (ruhend, Billboard, Fehlpartner, hinter der Kamera, w-Wechsel, Rundung): %d Fehler\n", n, fail);
    g_fail += fail;
}

/* ---------- W361 LOC-INTERP-CRASH: Liste haelt Schatten-Views ueber die Lebensdauer des Eintrags hinaus ----------
 * Nachbau des Absturzpfads: Eintrag gibt Textur/Views frei, die Liste bindet die SRV danach per Deferred-Context
 * (dort lag der Absturz: AllocateDependentCLS<CResource> mit Ressource NULL). */
static void test_shref(void)
{
    int fail = 0, n = 0;
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D *tex = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11ShaderResourceView *srv = NULL;
    ID3D11DeviceContext *dc = NULL;
    ip_shref_t r;
    memset(&td, 0, sizeof td);
    td.Width = td.Height = 16;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(g_d, &td, NULL, &tex)) ||
        FAILED(ID3D11Device_CreateRenderTargetView(g_d, (ID3D11Resource *)tex, NULL, &rtv)) ||
        FAILED(ID3D11Device_CreateShaderResourceView(g_d, (ID3D11Resource *)tex, NULL, &srv)) ||
        FAILED(ID3D11Device_CreateDeferredContext(g_d, 0, &dc))) {
        printf("SHREF (W361): Anlegen fehlgeschlagen\n");
        g_fail++;
        return;
    }
    memset(&r, 0, sizeof r);
    ip_shref_take(&r, (ID3D11View *)rtv, NULL, (ID3D11View *)srv, 5u);
    n++; if (!ip_shref_same(&r, 5u) || ip_shref_same(&r, 6u) || ip_shref_same(&r, 0u)) { fail++; } /* Generation */
    n++; if (r.v[0] != (ID3D11View *)rtv || r.v[1] || r.v[2] != (ID3D11View *)srv) { fail++; }
    /* Eintrag wird verdraengt: seine Referenzen gehen weg, die der Liste bleiben */
    ID3D11Texture2D_Release(tex);
    ID3D11RenderTargetView_Release(rtv);
    ID3D11ShaderResourceView_Release(srv);
    n++; { ULONG c1 = ID3D11ShaderResourceView_AddRef(srv), c0 = ID3D11ShaderResourceView_Release(srv);
           if (c1 != 2u || c0 != 1u) { fail++; } } /* nur noch die Liste */
    n++; { ID3D11Resource *res = NULL; /* View haelt die Textur am Leben */
           ID3D11View_GetResource(r.v[2], &res);
           if (!res) { fail++; } else { ID3D11Resource_Release(res); } }
    n++; { ID3D11ShaderResourceView *sv = srv; /* Absturzpfad: Deferred-Context bindet die SRV, Liste ausfuehren */
           ID3D11CommandList *cl = NULL;
           ID3D11DeviceContext_PSSetShaderResources(dc, 0, 1, &sv);
           if (FAILED(ID3D11DeviceContext_FinishCommandList(dc, FALSE, &cl)) || !cl) { fail++; }
           else { ID3D11DeviceContext_ExecuteCommandList(g_c, cl, FALSE); ID3D11CommandList_Release(cl); } }
    ip_shref_drop(&r);
    n++; if (r.v[0] || r.v[1] || r.v[2] || r.gen || ip_shref_same(&r, 5u)) { fail++; }
    ip_shref_drop(&r); /* zweimal unschaedlich */
    n++; if (r.v[0] || r.v[2]) { fail++; }
    ID3D11DeviceContext_ClearState(g_c);
    ID3D11DeviceContext_Release(dc);
    printf("SHREF (W361): %d Faelle (Listen-Referenzen auf Schatten-Views, Generation, Deferred-Bindung nach Freigabe): %d Fehler\n",
           n, fail);
    g_fail += fail;
}

/* ---------- W358 LOC-LOOK: Vertex-Licht der P2-Programme, LOD-Bias-Kompensation, Mip-Filter ---------- */
static void test_loclook(void)
{
    int fail = 0, n = 0;
    /* oD0-Quelle: kind 0 immer, kind 2 nur LoC + Schalter, kind 1 nie, shade_mask Bit 1 Pflicht */
    n++; if (!rq_loc_od0_from_prog(0, 0, 0, 31u)) { fail++; }
    n++; if (rq_loc_od0_from_prog(2, 0, 1, 31u)) { fail++; }     /* SB: unveraendert (Eingangsfarbe) */
    n++; if (rq_loc_od0_from_prog(2, 1, 0, 31u)) { fail++; }     /* LoC, Schalter aus */
    n++; if (!rq_loc_od0_from_prog(2, 1, 1, 31u)) { fail++; }    /* LoC, Schalter an */
    n++; if (rq_loc_od0_from_prog(1, 1, 1, 31u)) { fail++; }     /* P1 (2D) nie */
    n++; if (rq_loc_od0_from_prog(0, 1, 1, 0u)) { fail++; }      /* shade_mask ohne Bit 1 */
    /* Bias: Spiel -2,0 bei 480 Zeilen (s = 1) bleibt -2,0; bei 1440 Zeilen (s = 3) -2 + log2(3) = -0,415 */
    n++; if (rq_loc_bias16(-2.0f, 1.0f, 1) != -32) { fail++; }
    n++; if (rq_loc_bias16(-2.0f, 3.0f, 1) != -7) { fail++; }    /* -0,415 * 16 = -6,64 -> -7 */
    n++; if (rq_loc_bias16(-2.0f, 3.0f, 2) != -32) { fail++; }   /* roh */
    n++; if (rq_loc_bias16(-0.1f, 1.5f, 1) != 0) { fail++; }     /* nie unschaerfer als ohne Bias */
    n++; if (rq_loc_bias16(0.5f, 2.0f, 1) != 24) { fail++; }     /* positiver Spiel-Bias + 1 */
    { union { unsigned u; float f; } nan; nan.u = 0x7FC00000u;
      n++; if (rq_loc_bias16(nan.f, 1.0f, 1) != 0) { fail++; } } /* ungueltig -> 0 */
    n++; if (rq_loc_bias16(-1.0f, 0.5f, 1) != -16) { fail++; }   /* Skala < 1: keine Kompensation */
    /* Mip-Filter: Stufe 1 immer linear, volle Emulation = Spielwert */
    n++; if (rq_loc_mipfilter(1u, 1) != 2u || rq_loc_mipfilter(0u, 1) != 2u) { fail++; }
    n++; if (rq_loc_mipfilter(0u, 3) != 0u || rq_loc_mipfilter(1u, 3) != 1u || rq_loc_mipfilter(7u, 3) != 2u) { fail++; }
    printf("LOCLOOK (W358): %d Faelle (oD0-Quelle, LOD-Bias-Kompensation, Mip-Filter): %d Fehler\n", n, fail);
    g_fail += fail;
}

/* ---------- W352 ISSUE-EMP: Linien-Band-GS (line_scale) ----------
 * Spiel-Viewport 16x16 (logisch) auf 64x64 (Faktor 4): Linie mit und ohne
 * GS rastern, gezaehlte Pixel gegen Laenge x Breite (1 Xbox-Pixel = 4 px). */
static const char *kLsVsIm =
    "struct VI { float4 pos : POSITION; float4 col : COLOR0; };\n"
    "struct VO { float4 pos : SV_Position; float4 col : COLOR0; float2 uv : TEXCOORD0; };\n"
    "VO main(VI i) { VO o; o.pos = i.pos; o.col = i.col; o.uv = float2(0, 0); return o; }\n";
static const char *kLsPsIm =
    "float4 main(float4 p : SV_Position, float4 c : COLOR0, float2 uv : TEXCOORD0) : SV_Target { return c; }\n";
static const char *kLsVsSh =
    "struct VI { float4 pos : POSITION; float4 col : COLOR0; };\n"
    "struct VO { float4 pos : SV_Position; float4 col : COLOR0; float2 uv : TEXCOORD0;\n"
    "  float4 spec : COLOR1; float4 t0 : TEXCOORD1; float4 t1 : TEXCOORD2;\n"
    "  float4 t2 : TEXCOORD3; float4 t3 : TEXCOORD4; float fog : TEXCOORD5; };\n"
    "VO main(VI i) { VO o = (VO)0; o.pos = i.pos; o.col = i.col; o.fog = 1.0; return o; }\n";
static const char *kLsPsSh =
    "struct PSIN { float4 pos : SV_Position; float4 col : COLOR0; float2 uv : TEXCOORD0;\n"
    "  float4 spec : COLOR1; float4 t0 : TEXCOORD1; float4 t1 : TEXCOORD2;\n"
    "  float4 t2 : TEXCOORD3; float4 t3 : TEXCOORD4; float fog : TEXCOORD5; };\n"
    "float4 main(PSIN i) : SV_Target { return i.col * i.fog; }\n";

/* Linie (logische Pixel x0,y0 -> x1,y1, Clip-w = w) zeichnen, leuchtende Pixel zaehlen */
static int ls_count(ID3D11VertexShader *vs, ID3D11PixelShader *ps, ID3D11GeometryShader *gs, ID3D11InputLayout *lay,
                    ID3D11RenderTargetView *rtv, ID3D11Texture2D *rt, ID3D11Texture2D *stg,
                    float x0, float y0, float x1, float y1, float w)
{
    float v[2][8];
    ID3D11Buffer *vb;
    UINT st = 32, of = 0;
    float clr[4] = { 0, 0, 0, 0 };
    D3D11_VIEWPORT vp;
    D3D11_MAPPED_SUBRESOURCE m;
    int n = 0, i;
    float px[2][2] = { { x0, y0 }, { x1, y1 } };
    for (i = 0; i < 2; i++) {
        v[i][0] = (px[i][0] / 16.0f * 2.0f - 1.0f) * w; v[i][1] = (1.0f - px[i][1] / 16.0f * 2.0f) * w;
        v[i][2] = 0.5f * w; v[i][3] = w;
        v[i][4] = 1; v[i][5] = 1; v[i][6] = 1; v[i][7] = 1;
    }
    vb = mkbuf(D3D11_BIND_VERTEX_BUFFER, v, sizeof v);
    if (!vb) { return -1; }
    vp.TopLeftX = 0; vp.TopLeftY = 0; vp.Width = 64; vp.Height = 64; vp.MinDepth = 0; vp.MaxDepth = 1;
    ID3D11DeviceContext_ClearRenderTargetView(g_c, rtv, clr);
    ID3D11DeviceContext_OMSetRenderTargets(g_c, 1, &rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(g_c, 1, &vp);
    ID3D11DeviceContext_IASetInputLayout(g_c, lay);
    ID3D11DeviceContext_IASetVertexBuffers(g_c, 0, 1, &vb, &st, &of);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_c, D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    ID3D11DeviceContext_VSSetShader(g_c, vs, NULL, 0);
    ID3D11DeviceContext_GSSetShader(g_c, gs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_c, ps, NULL, 0);
    ID3D11DeviceContext_Draw(g_c, 2, 0);
    ID3D11DeviceContext_GSSetShader(g_c, NULL, NULL, 0);
    ID3D11DeviceContext_CopyResource(g_c, (ID3D11Resource *)stg, (ID3D11Resource *)rt);
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_c, (ID3D11Resource *)stg, 0, D3D11_MAP_READ, 0, &m))) {
        int y, x;
        for (y = 0; y < 64; y++) {
            const unsigned *row = (const unsigned *)((const unsigned char *)m.pData + (size_t)y * m.RowPitch);
            for (x = 0; x < 64; x++) { if (row[x] & 0xFFu) { n++; } }
        }
        ID3D11DeviceContext_Unmap(g_c, (ID3D11Resource *)stg, 0);
    }
    vb->lpVtbl->Release(vb);
    return n;
}

static void test_linescale(void)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_INPUT_ELEMENT_DESC ie[2] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 } };
    ID3D11Texture2D *rt = NULL, *stg = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    int sh;
    printf("== 7. Linien-Band-GS (W352 line_scale, Faktor 4)\n");
    memset(&td, 0, sizeof td);
    td.Width = 64; td.Height = 64; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(ID3D11Device_CreateTexture2D(g_d, &td, NULL, &rt)) ||
        FAILED(ID3D11Device_CreateRenderTargetView(g_d, (ID3D11Resource *)rt, NULL, &rtv))) { printf("LS FAIL Ziel\n"); g_fail++; return; }
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateTexture2D(g_d, &td, NULL, &stg))) { printf("LS FAIL Staging\n"); g_fail++; return; }
    for (sh = 0; sh < 2; sh++) {
        char src[2048];
        ID3DBlob *bv = compile(sh ? kLsVsSh : kLsVsIm, "vs_4_0"), *bp = compile(sh ? kLsPsSh : kLsPsIm, "ps_4_0"), *bg;
        ID3D11VertexShader *vs = NULL; ID3D11PixelShader *ps = NULL; ID3D11GeometryShader *gs = NULL; ID3D11InputLayout *lay = NULL;
        int nl, nh, nh2, nd, nz, ok;
        sb_line_gs_source(src, sizeof src, 16, 16, sh);
        bg = compile(src, "gs_4_0");
        if (!bv || !bp || !bg) { printf("LS FAIL %s: Uebersetzung\n", sh ? "Shading" : "IM"); g_fail++; continue; }
        ID3D11Device_CreateVertexShader(g_d, bv->lpVtbl->GetBufferPointer(bv), bv->lpVtbl->GetBufferSize(bv), NULL, &vs);
        ID3D11Device_CreatePixelShader(g_d, bp->lpVtbl->GetBufferPointer(bp), bp->lpVtbl->GetBufferSize(bp), NULL, &ps);
        ID3D11Device_CreateGeometryShader(g_d, bg->lpVtbl->GetBufferPointer(bg), bg->lpVtbl->GetBufferSize(bg), NULL, &gs);
        ID3D11Device_CreateInputLayout(g_d, ie, 2, bv->lpVtbl->GetBufferPointer(bv), bv->lpVtbl->GetBufferSize(bv), &lay);
        if (!vs || !ps || !gs || !lay) { printf("LS FAIL %s: Objekte\n", sh ? "Shading" : "IM"); g_fail++; continue; }
        nl = ls_count(vs, ps, NULL, lay, rtv, rt, stg, 2.0f, 8.5f, 14.0f, 8.5f, 1.0f);  /* alt: 1 px */
        nh = ls_count(vs, ps, gs, lay, rtv, rt, stg, 2.0f, 8.5f, 14.0f, 8.5f, 1.0f);    /* 12 x 1 Xbox-Pixel */
        nh2 = ls_count(vs, ps, gs, lay, rtv, rt, stg, 2.0f, 8.5f, 14.0f, 8.5f, 2.5f);   /* gleiche Linie, w = 2,5 */
        nd = ls_count(vs, ps, gs, lay, rtv, rt, stg, 2.0f, 2.0f, 13.0f, 13.0f, 1.0f);   /* Diagonale 15,56 */
        nz = ls_count(vs, ps, gs, lay, rtv, rt, stg, 5.0f, 5.0f, 5.0f, 5.0f, 1.0f);     /* Laenge 0 */
        /* Soll: 12*4 x 4 = 192; Diagonale (8,8)-(52,52) x 4 px: 220 Pixelmitten (Flaeche 249); ohne GS ~48; Laenge 0 -> 0 */
        ok = nl >= 44 && nl <= 50 && nh >= 184 && nh <= 200 && nh2 == nh && nd >= 210 && nd <= 230 && nz == 0;
        printf("LS %s %s: ohne=%d band=%d band_w2.5=%d diag=%d null=%d (Soll ~48/192/=band/~220/0)\n",
               sh ? "Shading" : "IM", ok ? "ok" : "FAIL", nl, nh, nh2, nd, nz);
        if (!ok) { g_fail++; }
        vs->lpVtbl->Release(vs); ps->lpVtbl->Release(ps); gs->lpVtbl->Release(gs); lay->lpVtbl->Release(lay);
        bv->lpVtbl->Release(bv); bp->lpVtbl->Release(bp); bg->lpVtbl->Release(bg);
    }
    rtv->lpVtbl->Release(rtv); rt->lpVtbl->Release(rt); stg->lpVtbl->Release(stg);
}

int main(int argc, char **argv)
{
    HMODULE dc;
    D3D_FEATURE_LEVEL fl;
    int f1;
#if SB_EDITION_COMMUNITY
    {
        char xpath[MAX_PATH], tried[MAX_PATH];
        if (!st_find_xbe(argc, argv, xpath, sizeof xpath, tried, sizeof tried)) {
            printf("SKIP: keine default.xbe gefunden (versucht: %s).\n"
                   "Der Community-Selbsttest liest seine Vektoren aus der eigenen XBE:\n"
                   "Pfad als Argument uebergeben oder SB_SELFTEST_XBE setzen.\n",
                   tried[0] ? tried : "-");
            return 2;
        }
        if (!st_load_xbe(xpath)) { return 1; }
    }
#else
    (void)argc; (void)argv;
#endif
    printf("== 1. VSH-CPU-Interpreter\n");
    f1 = rqv_selftest(st_log);
    g_fail += f1;
    dc = LoadLibraryA("d3dcompiler_47.dll");
    if (!dc) { printf("d3dcompiler_47 fehlt\n"); return 2; }
    g_comp = (fn_comp)GetProcAddress(dc, "D3DCompile");
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION,
                                 &g_d, &fl, &g_c))) {
        printf("WARP-Device fehlt\n");
        return 2;
    }
    if (!mk_targets()) { printf("Ziele fehlen\n"); return 2; }
    printf("== 2. VSH -> HLSL (GPU) gegen CPU-Interpreter\n");
    test_vs_gpu();
    printf("== 2b. VSH -> HLSL Roh-Eingaenge (SHADE-PERF, W80)\n");
    test_vs_raw();
    printf("== 3. Register-Combiner -> HLSL\n");
    test_ps();
    test_cpersp();
    test_texzero();
    test_bbcam();
    test_camfit();
    test_loclook();
    test_linescale();
    test_shref();
    printf("== ERGEBNIS: %d Fehler\n", g_fail);
    return g_fail ? 1 : 0;
}
