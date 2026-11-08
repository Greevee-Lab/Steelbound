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
        for (rep = 0; rep < 12u; rep++) {
            static const unsigned prims[6] = { 5, 6, 7, 8, 9, 3 };
            unsigned prim = prims[rep % 6u], n = 8, indexed = (rep >= 6u), id, si, vi, q;
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
            vu[0] = 8; vu[1] = stride; vu[2] = prim; vu[3] = n;
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
                    snprintf(line, sizeof line, "VSRAW vs=0x%X prim=%u id=%u idx=%u FAIL oD0 gpu=(%g,%g,%g,%g) cpu=(%g,%g,%g,%g) oT0 gpu=(%g,%g,%g,%g) cpu=(%g,%g,%g,%g)",
                             st_vs[t].handle, prim, id, indexed, got[0][0], got[0][1], got[0][2], got[0][3],
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
    snprintf(line, sizeof line, "VSRAW: %u ok, %u FAIL (%u Faelle: Formate, Expansion LIST/STRIP/FAN/QUADS/QUADSTRIP/LINELOOP, mit/ohne Index)", ok, bad, ncase);
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
    printf("== ERGEBNIS: %d Fehler\n", g_fail);
    return g_fail ? 1 : 0;
}
