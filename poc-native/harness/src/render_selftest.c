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
#include "loc_aim_law.h" /* W376 LOC-LOCAL-AIM */
#include "loc_smooth_math.h" /* W385 LOC-SMOOTH */
#include "render_pace.h" /* W393 LOC-PACING */
#include "loc_hud_math.h" /* W394 LOC-HUD-VIEW */
#include "loc_ret_mark.h" /* W444 LOC-RETICLE-MARK */
#include "loc_aimassist.h" /* W442 LOC-AIMASSIST */
#include "render_loclook.h" /* W358 LOC-LOOK */
#include "render_line_src.h" /* W352 ISSUE-EMP line_scale */
#include "render_interp_shref.h" /* W361 LOC-INTERP-CRASH */
#include "render_interp_eye.h" /* W448 LOC-CHAFF-LOOK */
#include "render_locmenu.h" /* W377 LOC-MENU-FPS */
#include "render_rq_exp.h" /* W383 LOC-GH-DATA (#62) */
#include "sb_tex_budget.h" /* W383 LOC-GH-DATA (#60) */
#include "sb_ovl_loclock.h" /* W416 LOC-ESC-MENU */
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

/* ---------- W376 LOC-LOCAL-AIM: Gesetz Eingabe -> Sicht/Arm und Kamera-Korrektur (loc_aim_law.h) ----------
 * (a) Sicht Tick fuer Tick gegen den Messlauf W376_A0 (Spielerblock, Sichthebel voll rechts = +127): Soll/Glaettung,
 * Anschlag -pi/2, Totzone. (b) Arm: Hoechstschritt, Annaeherung, Ruhe bei Hebel 0. (c) Kamera-Korrektur: VP * C ==
 * Kamera mit gedrehter Sicht (Gier, Nick, beides; Punkte im Bild), Null-Drehung = Einheit. */
static void la_camera(double VP[16], const double e[3], double yaw, double pitch)
{
    double f[3], r[3], u[3], V[16], P[16], fov = 0.8, zn = 0.5, zf = 4000.0, py, a, b;
    int i;
    f[0] = sin(yaw) * cos(pitch); f[1] = sin(pitch); f[2] = cos(yaw) * cos(pitch);
    r[0] = cos(yaw); r[1] = 0.0; r[2] = -sin(yaw);
    u[0] = f[1] * r[2] - f[2] * r[1]; u[1] = f[2] * r[0] - f[0] * r[2]; u[2] = f[0] * r[1] - f[1] * r[0];
    memset(V, 0, sizeof V);
    for (i = 0; i < 3; i++) { V[i * 4 + 0] = r[i]; V[i * 4 + 1] = u[i]; V[i * 4 + 2] = f[i]; }
    V[12] = -(e[0] * r[0] + e[1] * r[1] + e[2] * r[2]);
    V[13] = -(e[0] * u[0] + e[1] * u[1] + e[2] * u[2]);
    V[14] = -(e[0] * f[0] + e[1] * f[1] + e[2] * f[2]);
    V[15] = 1.0;
    py = 1.0 / tan(fov * 0.5); a = zf / (zf - zn); b = -zn * a;
    memset(P, 0, sizeof P);
    P[0] = py * 0.75; P[5] = py; P[10] = a; P[11] = 1.0; P[14] = b;
    la_m4mul_d(VP, V, P);
}
static double la_img_err(const double VPa[16], const double C[16], const double VPb[16])
{
    static const double pts[5][3] = {{10, 2, 300}, {-80, -5, 200}, {40, 30, 900}, {0, -2, 25}, {300, 0, 200}};
    double M[16], err = 0.0;
    int k, c;
    la_m4mul_d(M, VPa, C);
    for (k = 0; k < 5; k++) {
        double ca[4] = {0, 0, 0, 0}, cb[4] = {0, 0, 0, 0}, p[4] = {pts[k][0], pts[k][1], pts[k][2], 1.0};
        for (c = 0; c < 4; c++) {
            int q;
            for (q = 0; q < 4; q++) { ca[c] += p[q] * M[q * 4 + c]; cb[c] += p[q] * VPb[q * 4 + c]; }
        }
        if (cb[3] <= 1e-6) { continue; }
        for (c = 0; c < 3; c++) { double d = fabs(ca[c] / ca[3] - cb[c] / cb[3]); if (d > err) { err = d; } }
    }
    return err;
}
static void test_locaim(void)
{
    int fail = 0, n = 0, k;
    la_state_t s;
    uint8_t in[14];
    double VPa[16], VPb[16], C[16], e[3] = {120.0, 14.0, -350.0}, up[3] = {0.0, 1.0, 0.0};
    /* (a) Sicht gegen W376_A0: Slot 1849 -> 1850 -> 1851 (Sichthebel +127), 1870 -> 1871 (Anschlag), 1878 -> 1879 */
    memset(&s, 0, sizeof s);
    memset(in, 0, sizeof in);
    s.fa0 = 0x4400u; s.f98 = 0x80u; s.arate = 0.0523598790;
    s.yt = -0.0692727268; s.ys = -0.0277090911;
    in[12] = 127;
    n++; if (!la_can(&s) || !la_step(&s, in) || fabs(s.yt - -0.138545454) > 2e-6 || fabs(s.ys - -0.072043635) > 2e-6) { fail++; }
    n++; if (!la_step(&s, in) || fabs(s.yt - -0.20781818) > 2e-6 || fabs(s.ys - -0.126353458) > 2e-6) { fail++; }
    s.yt = -1.52400041; s.ys = -1.42009258;
    n++; if (!la_step(&s, in) || fabs(s.yt - -1.57079637) > 2e-6 || fabs(s.ys - -1.4803741) > 2e-6) { fail++; }
    s.ys = -1.5682652;
    n++; if (!la_step(&s, in) || fabs(s.ys - -1.56927764) > 2e-6) { fail++; }
    n++; if (!la_step(&s, in) || fabs(s.ys - -1.56927764) > 2e-6) { fail++; } /* Totzone: bleibt stehen */
    /* Totzone 25,6 (FUN_0001B0A0): Ruhewert -1 und 25 bewegen nicht, 26 schon */
    {
        la_state_t t0 = s;
        memset(in, 0, sizeof in);
        t0.pt = 0.5411475; t0.ps = 0.5393885; /* W376_A6: Rest-Delta 0,0009 rad ohne Totzone */
        in[13] = 0xFF;
        la_step(&t0, in); la_step(&t0, in);
        n++; if (fabs(t0.ps - 0.5393885) > 1e-9 || fabs(t0.pt - 0.5411475) > 1e-9) { fail++; }
        in[13] = 25; la_step(&t0, in);
        n++; if (fabs(t0.pt - 0.5411475) > 1e-9) { fail++; }
        in[13] = 26; la_step(&t0, in);
        n++; if (!(t0.pt < 0.5411475 - 1e-3)) { fail++; }
        memset(in, 0, sizeof in);
        in[12] = 127;
    }
    in[2] = 1; /* Stick-Klick: keine Vorhersage */
    n++; if (la_step(&s, in) != 0) { fail++; }
    in[2] = 0;
    s.f94 = 0x20u;
    n++; if (la_can(&s)) { fail++; }
    s.f94 = 0u;
    /* (b) Arm: Hebel -128 (voll) -> Soll +2,094; je Tick hoechstens arate; Hebel 0 -> zurueck; Nick-Hoechstschritt */
    memset(in, 0, sizeof in);
    in[10] = 0x80;
    s.ax = 0.0;
    for (k = 0; k < 3; k++) { la_step(&s, in); }
    n++; if (fabs(s.ax - 3.0 * 0.0523598790) > 1e-6) { fail++; }
    for (k = 0; k < 60; k++) { la_step(&s, in); }
    /* Glaettung k = |d| * 0,2387: nahe am Soll quadratisch langsamer (nie darueber) */
    n++; if (!(s.ax > 2.0944 - 0.1 && s.ax <= 2.0944)) { fail++; }
    {
        double ax0 = s.ax;
        in[10] = 0; in[11] = 100;
        la_step(&s, in);
        n++; if (fabs(s.ay - 0.05235987901687622) > 1e-6 || !(s.ax < ax0 && s.ax >= ax0 - 0.0523598790 - 1e-9)) { fail++; }
    }
    /* (c) Kamera-Korrektur */
    la_camera(VPa, e, 0.4, 0.0); la_camera(VPb, e, 0.4 + 0.12, 0.0);
    n++; if (!la_cam_corr(C, VPa, up, 0.12, 0.0) || la_img_err(VPa, C, VPb) > 1e-6) { fail++; }
    la_camera(VPa, e, -1.1, 0.2); la_camera(VPb, e, -1.1, 0.2 - 0.07);
    n++; if (!la_cam_corr(C, VPa, up, 0.0, -0.07) || la_img_err(VPa, C, VPb) > 1e-6) { fail++; }
    la_camera(VPa, e, 2.5, 0.0); la_camera(VPb, e, 2.5 - 0.2, 0.05);
    n++; if (!la_cam_corr(C, VPa, up, -0.2, 0.05) || la_img_err(VPa, C, VPb) > 1e-6) { fail++; }
    la_camera(VPa, e, 0.7, 0.1);
    n++; if (!la_cam_corr(C, VPa, up, 0.0, 0.0) || la_img_err(VPa, C, VPa) > 1e-9) { fail++; }
    /* rechtshaendige Welt wie LoC (x gespiegelt): "rechts"/"hoch" bleiben Bild-rechts/-oben */
    {
        double S[16], A2[16], B2[16];
        memset(S, 0, sizeof S); S[0] = -1.0; S[5] = 1.0; S[10] = 1.0; S[15] = 1.0;
        la_camera(VPa, e, 0.3, 0.0); la_camera(VPb, e, 0.3 + 0.15, 0.0);
        la_m4mul_d(A2, S, VPa); la_m4mul_d(B2, S, VPb);
        n++; if (!la_cam_corr(C, A2, up, 0.15, 0.0) || la_img_err(A2, C, B2) > 1e-6) { fail++; }
        la_camera(VPa, e, 0.3, 0.1); la_camera(VPb, e, 0.3, 0.1 + 0.06);
        la_m4mul_d(A2, S, VPa); la_m4mul_d(B2, S, VPb);
        n++; if (!la_cam_corr(C, A2, up, 0.0, 0.06) || la_img_err(A2, C, B2) > 1e-6) { fail++; }
    }
    printf("LOCAIM (W376): %d Faelle (Sicht gegen Messlauf, Anschlag, Totzone, Sonderfaelle, Arm, Kamera Gier/Nick/beides/null/rechtshaendig): %d Fehler\n", n, fail);
    g_fail += fail;
}

/* ---------- W442 LOC-AIMASSIST: Zielhilfe im LoC-Modell (loc_aimassist.h) ----------
 * Quantisierung/Totzone, Rand, Wirkung nur auf neutrale Sicht-Achsen, Sonderpfade, geschlossene Schleife mit dem
 * LoC-Gesetz (la_step) ohne und mit Eingabeverzoegerung D (Basis = Simulation + ausstehende eigene Saetze wie loc_aim.c),
 * Nick mit/ohne Umkehr, Stufe 2, Determinismus. */
static void laa_init(la_state_t *s)
{
    memset(s, 0, sizeof *s);
    s->arate = 0.03;
    s->fa0 = 0x4000u;
}
static void laa_mkin(uint8_t in[14], int hx, int hy)
{
    memset(in, 0, 14);
    in[10] = (uint8_t)(int8_t)hx;
    in[11] = (uint8_t)(int8_t)hy;
    in[12] = 0;
    in[13] = 0xFF; /* Sicht-Nick Ruhewert -1 (~0) */
}
/* Schleife: D Ticks Verzoegerung; base = Simulation + ausstehende eigene Saetze (wie loc_aim.c im Rollback).
 * Rueckgabe: Endzustand; *flips = Richtungswechsel der sichtbaren Sicht (Pendeln im Bild), *over = groesster Stand
 * des Fadenkreuzes auf der Gegenseite (Sicht laeuft am Arm vorbei), s_laa_exc = Ueberschreitung des Rands, out_in =
 * letzter Satz. */
static double s_laa_exc; /* groesste Ueberschreitung des Rands (sichtbar) ab Tick 20 */
static la_state_t laa_loop(const laa_cfg_t *c, int D, int ticks, int hx, int hy, int inv_sy, int *flips, double *over,
                           uint8_t out_in[14])
{
    la_state_t sim, b;
    uint8_t q[16][14], in[14];
    int t, i, n = 0, last = 0;
    laa_init(&sim);
    sim.inv_sy = inv_sy;
    *flips = 0;
    *over = 0.0;
    s_laa_exc = -1.0;
    for (t = 0; t < ticks; t++) {
        b = sim;
        for (i = 0; i < n; i++) la_step(&b, q[i]);
        laa_mkin(in, hx, hy);
        laa_apply(&b, c, in);
        memcpy(q[n++], in, 14);
        if (n > D) {
            double ys0 = sim.ys, dys;
            la_step(&sim, q[0]);
            memmove(q[0], q[1], (size_t)(n - 1) * 14);
            n--;
            dys = sim.ys - ys0; /* sichtbare Sicht: Richtungswechsel = Pendeln im Bild */
            if (fabs(dys) > 1e-5) {
                int sg = dys > 0.0 ? 1 : -1;
                if (last && sg != last) (*flips)++;
                last = sg;
            }
        }
        {
            double rel = sim.ax - sim.ys; /* sichtbar; hx > 0: Arm links (negativ), Rand rel = -edge; Gegenseite = rel > 0 */
            double o = hx > 0 ? rel : -rel;
            if (o > *over) *over = o;
            if (t >= 20 && fabs(rel) - c->edge_x > s_laa_exc) s_laa_exc = fabs(rel) - c->edge_x;
        }
    }
    memcpy(out_in, in, 14);
    return sim;
}
static void test_locaa(void)
{
    int n = 0, fail = 0, flips, m;
    double over, k0 = 60.0 * 0.0181818176060915 * 0.0005000000237487257, e, tq;
    la_state_t s, r;
    laa_cfg_t c;
    uint8_t in[14], in2[14];
    /* (a) Quantisierung, Totzone */
    n++; if (laa_q(0.4) != 0 || laa_q(12.0) != 0 || laa_q(13.0) != 26 || laa_q(-13.0) != -26 || laa_q(30.2) != 30 ||
             laa_q(-30.6) != -31 || laa_q(500.0) != 127 || laa_q(-500.0) != -127 || laa_q(25.0) != 26) { fail++; }
    n++; if (!laa_neutral(0) || !laa_neutral(-1) || !laa_neutral(25) || !laa_neutral(-25) || laa_neutral(26) ||
             laa_neutral(-26) || laa_neutral(127)) { fail++; }
    /* (b) Rand */
    e = laa_edge(80, 1.7320508075688772);
    tq = 13.5 * k0 + 0.0025; /* Quantum des Spiels: halber kleinster Soll-Schritt + Totzone der Sicht-Glaettung (0,05 Grad / 0,4) */
    n++; if (fabs(e - atan(0.8 / 1.7320508075688772)) > 1e-12 || fabs(laa_edge(100, 0.0) - atan(1.0 / 1.7320508075688772)) > 1e-12) { fail++; }
    memset(&c, 0, sizeof c);
    c.mode = 1; c.edge_x = c.edge_y = e;
    /* (c) innerhalb des Rands keine Wirkung; Stufe 0/3 keine Wirkung; eigene Sicht gewinnt */
    laa_init(&s);
    laa_mkin(in, 10, -5);
    memcpy(in2, in, 14);
    n++; if (laa_apply(&s, &c, in) != 0 || memcmp(in, in2, 14) != 0) { fail++; }
    s.ax = -1.0; /* Arm weit links: x muss nachfuehren */
    laa_mkin(in, 61, 0);
    n++; m = laa_apply(&s, &c, in); if (!(m & 1) || (int8_t)in[12] <= 0) { fail++; } /* yt' = yt - k*Hebel -> Hebel > 0 dreht nach links (negativ) */
    laa_mkin(in, 61, 0); in[12] = (uint8_t)(int8_t)-90;
    n++; if ((laa_apply(&s, &c, in) & 1) || (int8_t)in[12] != -90) { fail++; }
    { laa_cfg_t c0 = c; c0.mode = 0; laa_mkin(in, 61, 0); memcpy(in2, in, 14);
      n++; if (laa_apply(&s, &c0, in) != 0 || memcmp(in, in2, 14) != 0) { fail++; }
      c0.mode = 3; n++; if (laa_apply(&s, &c0, in) != 0 || memcmp(in, in2, 14) != 0) { fail++; } }
    /* (d) Sonderpfade: Stick-Klick, Arm folgt der Sicht (+0x98 Bit 4), keine Vorhersage moeglich */
    laa_mkin(in, 61, 0); in[2] = 1; memcpy(in2, in, 14);
    n++; if (laa_apply(&s, &c, in) != 0 || memcmp(in, in2, 14) != 0) { fail++; }
    r = s; r.f98 |= 4u; laa_mkin(in, 61, 0);
    n++; if (laa_apply(&r, &c, in) != 0) { fail++; }
    r = s; r.fa0 = 0; laa_mkin(in, 61, 0);
    n++; if (laa_apply(&r, &c, in) != 0) { fail++; }
    r = s; r.f94 |= 0x20u; laa_mkin(in, 61, 0);
    n++; if (laa_apply(&r, &c, in) != 0) { fail++; }
    /* (e) Schleife Stufe 1, Hebel 40 (Arm-Soll -0,65 rad), D = 0/1/2/3 mit Vorhersage: sichtbares Fadenkreuz am Rand
     * gehalten (ab Tick 20 hoechstens 0,004 rad darueber), kein Pendeln, nie am Arm vorbei; in Ruhe am bzw. knapp
     * innerhalb des Rands (Nachlauf der Spiel-Glaettung) */
    {
        int D;
        for (D = 0; D <= 3; D++) {
            r = laa_loop(&c, D, 200, 40, 0, 0, &flips, &over, in);
            n++; if (flips != 0 || over > 1e-9 || s_laa_exc > tq) { fail++; printf("  LOCAA e D=%d flips=%d over=%.4f exc=%.4f\n", D, flips, over, s_laa_exc); }
            n++; if (-(r.ax - r.ys) > e + tq || -(r.ax - r.ys) < e - 0.06) { fail++; printf("  LOCAA e D=%d rel=%.4f rand=%.4f\n", D, r.ax - r.ys, -e); }
        }
        /* Hebel voll (Arm-Soll -2,08 rad): Sicht bis zum Anschlag -pi/2 */
        r = laa_loop(&c, 2, 400, 127, 0, 0, &flips, &over, in);
        n++; if (fabs(r.yt + LAA_YAW_MAX) > 13.5 * k0 + 1e-6 || flips != 0) { fail++; printf("  LOCAA Anschlag yt=%.4f flips=%d\n", r.yt, flips); }
        /* andere Richtung */
        r = laa_loop(&c, 2, 200, -40, 0, 0, &flips, &over, in);
        n++; if (flips != 0 || over > 1e-9 || s_laa_exc > tq || (r.ax - r.ys) > e + tq || (r.ax - r.ys) < e - 0.06) { fail++; printf("  LOCAA e- rel=%.4f exc=%.4f\n", r.ax - r.ys, s_laa_exc); }
    }
    /* (f) Stufe 2 (Maus-Achse x): sichtbare Sicht folgt dem Arm ganz (Fadenkreuz in der Mitte); Achse ohne Bit wie Stufe 1 */
    {
        laa_cfg_t c2 = c;
        c2.mode = 2; c2.lock_axes = 1u;
        r = laa_loop(&c2, 2, 200, 40, 0, 0, &flips, &over, in);
        n++; if (fabs(r.ax - r.ys) > tq || flips != 0) { fail++; printf("  LOCAA f rel=%.4f flips=%d\n", r.ax - r.ys, flips); }
        c2.lock_axes = 2u;
        r = laa_loop(&c2, 2, 200, 40, 0, 0, &flips, &over, in);
        n++; if (-(r.ax - r.ys) > e + tq || -(r.ax - r.ys) < e - 0.06) { fail++; }
    }
    /* (g) Nick: Fadenkreuz-Nick = Arm_y + Sicht-Nick; mit und ohne Umkehr (+0x53C) gleiches Ergebnis */
    {
        laa_cfg_t cy = c;
        la_state_t r1, r2;
        cy.edge_y = 0.1;
        r1 = laa_loop(&cy, 2, 200, 0, 100, 0, &flips, &over, in);
        r2 = laa_loop(&cy, 2, 200, 0, 100, 1, &flips, &over, in2);
        n++; if ((r1.ay + r1.ps) > 0.1 + tq || (r1.ay + r1.ps) < 0.1 - 0.06) { fail++; printf("  LOCAA g ay=%.4f ps=%.4f\n", r1.ay, r1.ps); }
        n++; if (fabs(r1.pt - r2.pt) > 1e-9 || fabs(r1.ps - r2.ps) > 1e-9) { fail++; printf("  LOCAA g inv pt=%.4f/%.4f\n", r1.pt, r2.pt); }
        laa_init(&r1); r1.ay = 0.6; r2 = r1; r2.inv_sy = 1;
        laa_mkin(in, 0, 100); laa_mkin(in2, 0, 100);
        n++; if (laa_apply(&r1, &cy, in) != 2 || laa_apply(&r2, &cy, in2) != 2 || (int8_t)in[13] != 127 || (int8_t)in2[13] != -127) { fail++; }
    }
    /* (i) Determinismus: gleiche Folge zweimal -> gleiche Saetze und gleicher Endzustand */
    {
        la_state_t a1 = laa_loop(&c, 2, 150, 70, -60, 0, &flips, &over, in);
        la_state_t a2 = laa_loop(&c, 2, 150, 70, -60, 0, &flips, &over, in2);
        n++; if (memcmp(in, in2, 14) != 0 || a1.yt != a2.yt || a1.pt != a2.pt || a1.ax != a2.ax) { fail++; }
    }
    printf("LOCAA (W442): %d Faelle (Totzone, Rand, neutral/eigene Sicht, Sonderpfade, Schleife D 0..3, Anschlag, Stufe 2, Nick/Umkehr, Determinismus): %d Fehler\n", n, fail);
    g_fail += fail;
}

/* ---------- W435 LOC-SKY-LAG-REG: Sicht-Korrektur C (la_cam_corr) auf den Himmel ----------
 * Himmel = Draw in Weltkoordinaten mit eigener Tiefe: Spalten x, y, w der mvp = Kamera VP, Spalte z anders (W412).
 * Fuer Aspect 4:3, 16:9 und 2,39 (Hor+, Spalte x skaliert), links- und rechtshaendige Welt und mehrere Gier/Nick-
 * Korrekturen: Zeile z von C ist (0, 0, 1, 0); mvp_Himmel * C zeigt jeden Himmelspunkt (x, y) genau dort, wo die
 * gedrehte Kamera ihn zeigt (wie die Welt, VP * C), und w ist gleich. Gegenprobe: ohne C weicht er ab. */
static void test_sky435(void)
{
    static const double asp[3] = {4.0 / 3.0, 16.0 / 9.0, 3440.0 / 1440.0};
    static const double rot[4][2] = {{0.12, 0.0}, {0.0, -0.07}, {-0.2, 0.05}, {0.031, 0.012}};
    static const double pts[5][3] = {{10, 2, 300}, {-80, -5, 200}, {40, 30, 900}, {0, 40, 25}, {300, 0, 200}};
    const double e[3] = {12.0, 3.0, -40.0}, up[3] = {0.0, 1.0, 0.0};
    double VP[16], VPb[16], Sk[16], C[16], M[16], Mw[16], S[16], T[16];
    int n = 0, fail = 0, a, r, h, k, c, q, neg = 0;
    memset(S, 0, sizeof S); S[0] = -1.0; S[5] = 1.0; S[10] = 1.0; S[15] = 1.0;
    for (a = 0; a < 3; a++) {
        for (h = 0; h < 2; h++) {
            for (r = 0; r < 4; r++) {
                double err = 0.0, err0 = 0.0, kx = (4.0 / 3.0) / asp[a];
                la_camera(VP, e, 0.4 + 0.3 * r, 0.05 * r);
                la_camera(VPb, e, 0.4 + 0.3 * r + rot[r][0], 0.05 * r + rot[r][1]);
                for (q = 0; q < 4; q++) { VP[q * 4] *= kx; VPb[q * 4] *= kx; } /* Hor+: Spalte x */
                if (h) { la_m4mul_d(T, S, VP); memcpy(VP, T, sizeof T); la_m4mul_d(T, S, VPb); memcpy(VPb, T, sizeof T); }
                memcpy(Sk, VP, sizeof Sk);
                for (q = 0; q < 4; q++) { Sk[q * 4 + 2] = 0.99999 * VP[q * 4 + 3]; } /* Tiefe auf die Ferne */
                n++;
                if (!la_cam_corr(C, VP, up, rot[r][0], rot[r][1])) { fail++; continue; }
                n++; if (fabs(C[8]) + fabs(C[9]) + fabs(C[11]) > 1e-9 || fabs(C[10] - 1.0) > 1e-9) { fail++; }
                la_m4mul_d(M, Sk, C);
                la_m4mul_d(Mw, VP, C);
                for (k = 0; k < 5; k++) {
                    double cs[4] = {0, 0, 0, 0}, cw[4] = {0, 0, 0, 0}, cb[4] = {0, 0, 0, 0}, c0[4] = {0, 0, 0, 0};
                    double p[4] = {pts[k][0] * 20.0, pts[k][1] * 20.0, pts[k][2] * 20.0, 1.0};
                    for (c = 0; c < 4; c++) {
                        for (q = 0; q < 4; q++) {
                            cs[c] += p[q] * M[q * 4 + c]; cw[c] += p[q] * Mw[q * 4 + c];
                            cb[c] += p[q] * VPb[q * 4 + c]; c0[c] += p[q] * Sk[q * 4 + c];
                        }
                    }
                    if (cb[3] <= 1e-6 || cs[3] <= 1e-6) { continue; }
                    for (c = 0; c < 2; c++) {
                        double d = fabs(cs[c] / cs[3] - cb[c] / cb[3]), dw = fabs(cs[c] / cs[3] - cw[c] / cw[3]);
                        double d0 = fabs(c0[c] / c0[3] - cb[c] / cb[3]);
                        if (d > err) { err = d; } if (dw > err) { err = dw; } if (d0 > err0) { err0 = d0; }
                    }
                    if (fabs(cs[3] - cw[3]) > 1e-6 * (1.0 + fabs(cw[3]))) { err = 1.0; }
                }
                n++; if (err > 1e-6) { fail++; }
                n++; if (err0 < 1e-3) { fail++; neg++; } /* Gegenprobe: ohne Korrektur sichtbar daneben */
            }
        }
    }
    printf("SKY435 (W435): %d Faelle (Himmel mit Sicht-Korrektur, Aspect 4:3/16:9/2,39, links/rechtshaendig, Gier/Nick, Gegenprobe): %d Fehler%s\n",
           n, fail, neg ? " (Gegenprobe zu klein)" : "");
    g_fail += fail;
}

/* ---------- W385 LOC-SMOOTH: weiche Rollback-Korrektur (loc_smooth_math.h) ----------
 * (a) Abklingen: Gewichte 1, 2/3, 1/3, 0 (150 ms = 3 Listen), danach zurueckgesetzt; zweite Korrektur waehrend des
 * Abklingens = Rest + neue (Anzeige stetig); Gier-Wrap. (b) Objekt: korrigierte Lage -> vorhergesagte, Vorwaertsrichtung
 * dreht um dGier, Null-Versatz = Einheit. (c) Kamera: K * VP(korrigiert) zeigt jeden Punkt wie VP(vorhergesagt), Auge
 * aus VP^-1. (d) beides zusammen (anderer VT gesehen von der vorhergesagten eigenen Kamera). */
static void ls_img(double out[3], const double p[3], const double M[16])
{
    double c[4];
    int k;
    for (k = 0; k < 4; k++) c[k] = p[0] * M[k] + p[1] * M[4 + k] + p[2] * M[8 + k] + M[12 + k];
    out[0] = c[0] / c[3];
    out[1] = c[1] / c[3];
    out[2] = c[3];
}
static double ls_d3(const double a[3], const double b[3])
{
    return fabs(a[0] - b[0]) + fabs(a[1] - b[1]) + fabs(a[2] - b[2]);
}
static void test_locsmooth(void)
{
    int n = 0, fail = 0, k, i;
    ls_track_t t;
    double o[4], e1[4] = {30.0, 2.0, -12.0, 0.05}, e2[4] = {-6.0, 0.0, 4.0, -0.02};
    static const double w3[4] = {1.0, 2.0 / 3.0, 1.0 / 3.0, 0.0};
    /* (a) */
    memset(&t, 0, sizeof t);
    n++; if (ls_track_step(&t, o) != 0 || o[0] != 0.0) { fail++; }
    ls_track_add(&t, e1, 3);
    for (k = 0; k < 4; k++) {
        int nz = ls_track_step(&t, o);
        n++; if (nz != (k < 3) || fabs(o[0] - e1[0] * w3[k]) > 1e-12 || fabs(o[3] - e1[3] * w3[k]) > 1e-12) { fail++; }
    }
    n++; if (t.n != 0 || t.k != 0 || t.off0[0] != 0.0) { fail++; } /* zurueckgesetzt */
    ls_track_add(&t, e1, 3);
    (void)ls_track_step(&t, o);                 /* Liste 1: e1 */
    (void)ls_track_step(&t, o);                 /* Liste 2: 2/3 e1 */
    ls_track_add(&t, e2, 3);                    /* neue Korrektur vor Liste 3 */
    (void)ls_track_step(&t, o);
    for (i = 0; i < 4; i++) { n++; if (fabs(o[i] - (e1[i] * 2.0 / 3.0 + e2[i])) > 1e-12) { fail++; } }
    (void)ls_track_step(&t, o);
    n++; if (fabs(o[2] - (e1[2] * 2.0 / 3.0 + e2[2]) * 2.0 / 3.0) > 1e-12) { fail++; }
    ls_track_add(&t, e1, 3); /* mitten im Abklingen: Rest der zuletzt gezeigten Liste + e1 */
    (void)ls_track_step(&t, o);
    n++; if (fabs(o[0] - ((e1[0] * 2.0 / 3.0 + e2[0]) * 2.0 / 3.0 + e1[0])) > 1e-12) { fail++; }
    ls_track_reset(&t);
    {
        double ey[4] = {0.0, 0.0, 0.0, 3.0};
        ls_track_add(&t, ey, 2);
        (void)ls_track_step(&t, o); /* Rest 3.0 */
        ls_track_add(&t, ey, 2);    /* 6.0 -> gewickelt */
        n++; if (!(t.off0[3] > -LS_PI && t.off0[3] <= LS_PI) || fabs(t.off0[3] - (6.0 - 2.0 * LS_PI)) > 1e-12) { fail++; }
    }
    n++; if (ls_weight(0, 2) != 1.0 || ls_weight(1, 2) != 0.5 || ls_weight(2, 2) != 0.0 || ls_weight(0, 0) != 0.0) { fail++; }
    /* (b) Objekt */
    {
        double p[3] = {1200.0, 40.0, -800.0}, X[16], q[3], r[3], f[3], a = 0.7, L = 50.0;
        double off[4] = {35.0, -3.0, 18.0, 0.12}, z[4] = {0, 0, 0, 0};
        ls_obj_xform(X, p, off);
        ls_xform_pt(q, p, X);
        r[0] = p[0] + off[0]; r[1] = p[1] + off[1]; r[2] = p[2] + off[2];
        n++; if (ls_d3(q, r) > 1e-9) { fail++; }
        f[0] = p[0] + sin(a) * L; f[1] = p[1]; f[2] = p[2] + cos(a) * L; /* Vorwaerts bei Gier a */
        ls_xform_pt(q, f, X);
        r[0] = p[0] + off[0] + sin(a + off[3]) * L; r[1] = p[1] + off[1]; r[2] = p[2] + off[2] + cos(a + off[3]) * L;
        n++; if (ls_d3(q, r) > 1e-9) { fail++; }
        ls_obj_xform(X, p, z);
        for (k = 0; k < 16; k++) { n++; if (fabs(X[k] - ((k % 5) == 0 ? 1.0 : 0.0)) > 1e-12) { fail++; } }
    }
    /* (c) Kamera, (d) zusammen */
    {
        double e[3] = {500.0, 120.0, 300.0}, offc[4] = {-22.0, 1.5, 14.0, -0.09}, ep[3], VPc[16], VPp[16], VPi[16], K[16],
               M[16], eye[3], ia[3], ib[3], z[4] = {0, 0, 0, 0}, X[16], MX[16];
        static const double pts[6][3] = {{520, 120, 900}, {300, 60, 700}, {900, 300, 1500}, {480, 100, 340},
                                         {1500, 0, 600}, {-200, 50, 1200}};
        double po[3] = {700.0, 110.0, 820.0}, offo[4] = {40.0, 0.0, -25.0, 0.2};
        double a = 0.25;
        ep[0] = e[0] + offc[0]; ep[1] = e[1] + offc[1]; ep[2] = e[2] + offc[2];
        la_camera(VPc, e, a, 0.05);
        la_camera(VPp, ep, a + offc[3], 0.05);
        n++; if (!la_m4inv_d(VPi, VPc) || !ls_eye_of(eye, VPi) || ls_d3(eye, e) > 1e-6) { fail++; }
        ls_cam_xform(K, eye, offc);
        ls_m4_mul(M, K, VPc);
        for (k = 0; k < 6; k++) {
            ls_img(ia, pts[k], M);
            ls_img(ib, pts[k], VPp);
            n++; if (ls_d3(ia, ib) > 1e-6 * (1.0 + fabs(ib[2]))) { fail++; }
        }
        ls_obj_xform(X, po, offo); /* anderer VT: seine Draws (Weltursprung po) erst X, dann K */
        ls_m4_mul(MX, X, K);
        ls_m4_mul(MX, MX, VPc);
        for (k = 0; k < 6; k++) {
            double q[3];
            ls_xform_pt(q, pts[k], X);
            ls_img(ia, pts[k], MX);
            ls_img(ib, q, VPp);
            n++; if (ls_d3(ia, ib) > 1e-6 * (1.0 + fabs(ib[2]))) { fail++; }
        }
        ls_cam_xform(K, eye, z);
        for (k = 0; k < 16; k++) { n++; if (fabs(K[k] - ((k % 5) == 0 ? 1.0 : 0.0)) > 1e-9) { fail++; } }
    }
    printf("LOCSMOOTH (W385): %d Faelle (Abklingen 1/2/3-Listen, Rest + neue Korrektur, Gier-Wrap, Objekt Lage/Drehung/null, "
           "Kamera Auge/Bild/null, Objekt von vorhergesagter Kamera): %d Fehler\n", n, fail);
    g_fail += fail;
}

/* ---------- W387 LOC-SMOOTH-OPP: andere VTs verzoegert zeigen (loc_smooth_math.h) ----------
 * (a) Historie: T - d mit Bruchteil linear, Gier-Wrap, fehlender/fremder Tick -> verkuerzt bzw. 0. (b) Versatz
 * Lage(T - d) - Lage(T), Objekt-Matrix bringt den Mittelpunkt dorthin. (c) Bedarf einer Korrektur (0..3) aus Ringen
 * vorher/nachher, Gier zaehlt mit Hebel. (d) Auto-Regelung: aus = 0, fest = 1 (Rampe), auto ohne Korrekturen 0, nach
 * 2 sichtbaren 1, nach 3 mit Bedarf 2 -> 2 (bzw. Obergrenze 1), Absenken erst nach Fenster + Ruhezeit, nie schneller als
 * 1/20 Tick je Tick, Zufallsfolge beschraenkt. */
static void lso_put(ls_hist_t *h, unsigned t, unsigned obj, double x, double yaw)
{
    double p[4];
    p[0] = x; p[1] = 5.0; p[2] = -2.0 * x; p[3] = yaw;
    ls_hist_put(h, t, obj, p);
}
/* W390 LOC-OWN-PREDICT: Korrektur zwischen zwei Vorlaeufen, Anzeige-Versatz, stetige Anzeige bei Korrektur. */
static void test_locownpred(void)
{
    int n = 0, fail = 0, k, i, ok[4] = {1, 1, 1, 1};
    unsigned gt[4] = {102, 103, 104, 105};
    float p[4][4], pp[4] = {1000.f, 5.f, 2000.f, 3.10f}, pred[4];
    double c[4], sim[4], res[4] = {0, 0, 0, 0}, o[4];
    for (k = 0; k < 4; k++) {
        p[k][0] = 900.f + (float)k * 30.f; p[k][1] = 5.f; p[k][2] = 2000.f; p[k][3] = -3.10f;
    }
    /* (a) Korrektur am gemeinsamen Tick, Gier ueber +-pi */
    n++; if (!ls_own_corr(105, pp, 4, gt, (const float(*)[4])p, ok, c) || fabs(c[0] - (1000.0 - 990.0)) > 1e-4 ||
             fabs(c[3] - ls_wrap(3.10 - (-3.10))) > 1e-5 || fabs(c[3]) > 0.1) { fail++; }
    n++; if (ls_own_corr(106, pp, 4, gt, (const float(*)[4])p, ok, c)) { fail++; } /* nicht enthalten */
    ok[3] = 0;
    n++; if (ls_own_corr(105, pp, 4, gt, (const float(*)[4])p, ok, c)) { fail++; } /* Lage ungueltig */
    ok[3] = 1;
    /* (b) Versatz und Grenzen */
    pred[0] = 1500.f; pred[1] = 0.f; pred[2] = -200.f; pred[3] = 0.3f;
    sim[0] = 1000.0; sim[1] = 0.0; sim[2] = 0.0; sim[3] = 0.1;
    n++; if (!ls_own_offset(pred, sim, res, 20000.0, o) || fabs(o[0] - 500.0) > 1e-4 || fabs(o[2] + 200.0) > 1e-4 || fabs(o[3] - 0.2) > 1e-6) { fail++; }
    res[0] = -100.0; res[3] = 0.05;
    n++; if (!ls_own_offset(pred, sim, res, 20000.0, o) || fabs(o[0] - 400.0) > 1e-4 || fabs(o[3] - 0.25) > 1e-6) { fail++; }
    res[0] = 0.0; res[3] = 0.0;
    pred[0] = 30000.f;
    n++; if (ls_own_offset(pred, sim, res, 20000.0, o) || o[0] != 0.0 || o[3] != 0.0) { fail++; } /* Teleport */
    pred[0] = 1500.f; pred[3] = 2.0f;
    n++; if (ls_own_offset(pred, sim, res, 20000.0, o)) { fail++; } /* Gier-Sprung */
    /* (c) gleichfoermige Fahrt (v je Tick), Vorlauf 2 Ticks; bei Tick 10 korrigiert ein neuer Vorlauf die Vorhersage um
     * delta. Angezeigt = Vorhersage + abklingende Korrektur: Schritt je Tick weicht hoechstens delta/3 von v ab. */
    {
        ls_track_t t;
        double v = 120.0, delta = -90.0, prevd = 0.0, worst = 0.0, shift = 0.0, last_final = 0.0;
        unsigned lastgt = 0;
        memset(&t, 0, sizeof t);
        for (k = 0; k < 30; k++) {
            unsigned g[3];
            float q[3][4];
            int qo[3] = {1, 1, 1};
            double r4[4], s4[4], d;
            if (k == 10) shift = delta; /* ab hier sagt jeder Vorlauf delta weniger voraus */
            for (i = 0; i < 3; i++) {
                g[i] = (unsigned)(k + i + 1);
                q[i][0] = (float)(v * (double)(k + i + 1) + shift); q[i][1] = 0.f; q[i][2] = 0.f; q[i][3] = 0.f;
            }
            if (lastgt) {
                float lp[4];
                lp[0] = (float)last_final; lp[1] = lp[2] = lp[3] = 0.f;
                if (ls_own_corr(lastgt, lp, 3, g, (const float(*)[4])q, qo, c) && (fabs(c[0]) > 1e-6)) ls_track_add(&t, c, 3);
            }
            (void)ls_track_step(&t, r4);
            s4[0] = v * (double)(k + 1); s4[1] = s4[2] = s4[3] = 0.0; /* simuliert (Tick k + 1) */
            if (!ls_own_offset(q[2], s4, r4, 20000.0, o)) { fail++; break; }
            d = s4[0] + o[0];
            if (k > 0 && fabs((d - prevd) - v) > worst) worst = fabs((d - prevd) - v);
            prevd = d;
            lastgt = g[2];
            last_final = q[2][0];
        }
        n++; if (worst > fabs(delta) / 3.0 + 1e-3 || worst < fabs(delta) / 3.0 - 1e-3) { fail++; }
    }
    { /* (d) Drehung um den VT-Mittelpunkt: das vorhergesagte Auge erscheint nach K am echten Auge, Blickrichtung um dG */
        double e[3] = {100.0, 2500.0, 1800.0}, cc[3] = {100.0, 0.0, 0.0}, off4[4] = {300.0, 0.0, 900.0, 0.2}, oc[4], K[16];
        double ep[3], m[16], r[16], d[3], t[3], u[3];
        ls_cam_off_pivot(off4, e, cc, oc);
        ls_m4_yaw(r, off4[3]);
        for (i = 0; i < 3; i++) d[i] = e[i] - cc[i];
        ls_xform_pt(u, d, r);
        for (i = 0; i < 3; i++) ep[i] = cc[i] + off4[i] + u[i]; /* vorhergesagtes Auge */
        ls_cam_xform(K, e, oc);
        ls_xform_pt(t, ep, K);
        n++; if (fabs(t[0] - e[0]) + fabs(t[1] - e[1]) + fabs(t[2] - e[2]) > 1e-6) { fail++; }
        oc[3] = 0.0; off4[3] = 0.0;
        ls_cam_off_pivot(off4, e, cc, oc);
        n++; if (fabs(oc[0] - 300.0) + fabs(oc[2] - 900.0) > 1e-9) { fail++; } /* ohne Drehung = reiner Versatz */
        (void)m;
    }
    { /* (e) Vorsprung 3 -> 2 Ticks bei gleichfoermiger Fahrt: mit Ausgleich laeuft die Anzeige ohne Stillstand weiter */
        float a[4] = {1000.f, 0.f, 500.f, 0.3f}, b[4] = {880.f, 0.f, 500.f, 0.28f};
        double c[4];
        ls_own_leadchg(a, b, -1, c);
        n++; if (fabs(c[0] - 120.0) > 1e-4 || fabs(c[2]) > 1e-9 || fabs(c[3] - 0.02) > 1e-6) { fail++; }
        ls_own_leadchg(a, b, 1, c);
        n++; if (fabs(c[0] + 120.0) > 1e-4) { fail++; }
    }
    printf("LOCOWNPRED (W390): %d Faelle (Korrektur am gemeinsamen Tick, Gier-Wrap, Versatz, Teleport/Gier-Grenze, stetige Anzeige): %d Fehler\n", n, fail);
    g_fail += fail;
}

/* ---------- W393 LOC-PACING: Bildtakt (render_pace.h) ---------- */
typedef struct { LONG a, b[2000], c; } pace_tst_t; /* alle Felder = Folgenummer: zerrissen, wenn nicht gleich */
static volatile LONG pace_tst_seq, pace_tst_stop;
static pace_tst_t pace_tst_slots[PACE_PUB_SLOTS];
static DWORD WINAPI pace_tst_writer(LPVOID p)
{
    LONG i, k;
    static pace_tst_t v;
    (void)p;
    for (i = 1; !pace_tst_stop; i++) {
        v.a = v.c = i;
        for (k = 0; k < 2000; k++) { v.b[k] = i; }
        pace_pub_write(&pace_tst_seq, pace_tst_slots, sizeof v, &v);
        if ((i & 1) == 0) { /* Paare dicht hintereinander (Live-Tick + Vorlauf), dann kurz Ruhe */
            for (k = 0; k < 2000; k++) { YieldProcessor(); }
        }
    }
    return 0;
}

static void test_pacing(void)
{
    int n = 0, fail = 0, i;
    LONGLONG f = 10000000; /* 10 MHz wie QPC */
    { /* (a) Bilduhr: L springt +30 ms und nach 1,6 s -30 ms. Alt (L direkt): gezeigte Zeit laeuft zurueck bzw. springt.
       *     Neu (pace_lat_step 200/20 Promille): gezeigte Zeit steigt immer, Schritt je Bild 80..102 % des Bildabstands. */
        LONGLONG dt = f / 120, due = 0, lat = 0, want, v, vlast = 0;
        int back_old = 0, bad_new = 0, conv = 0;
        LONGLONG lold = 0;
        for (i = 0; i < 1200; i++) {
            want = (i >= 100 && i < 292) ? f * 82 / 1000 : f * 52 / 1000;
            due += dt;
            if (i > 0 && (due - want) < (due - dt - lold)) { back_old++; } /* alt: v = due - want */
            lold = want;
            lat = pace_lat_step(lat, want, i ? dt : 0, 200, 20, f / 10);
            v = due - lat;
            if (i > 0) {
                LONGLONG st = v - vlast;
                if (st <= 0 || st < dt * 79 / 100 || st > dt * 103 / 100) { bad_new++; }
            }
            vlast = v;
            if (i == 1199 && lat == f * 52 / 1000) { conv = 1; }
        }
        n++; if (back_old < 1) { fail++; printf("  PACING (a) alt ohne Ruecklauf?\n"); }
        n++; if (bad_new) { fail++; printf("  PACING (a) neu %d unstetige Schritte\n", bad_new); }
        n++; if (!conv) { fail++; printf("  PACING (a) L nicht eingeschwungen\n"); }
    }
    { /* (b) Snap, Start, Grenzen */
        n++; if (pace_lat_step(0, 500, 10, 200, 20, 0) != 500) { fail++; }
        n++; if (pace_lat_step(1000, 3000, 1000, 200, 20, 1500) != 3000) { fail++; } /* > snap: sofort */
        n++; if (pace_lat_step(1000, 2000, 1000, 200, 20, 1500) != 1200) { fail++; } /* +20 % */
        n++; if (pace_lat_step(1000, 500, 1000, 200, 20, 0) != 980) { fail++; }       /* -2 % */
        n++; if (pace_lat_step(1000, 1010, 1000, 200, 20, 0) != 1010) { fail++; }     /* innerhalb: Ziel */
    }
    { /* (c) Kennzahlen Bildabstand */
        float v[100];
        pace_ft_t r;
        for (i = 0; i < 100; i++) { v[i] = 8.33f; }
        v[10] = 9.5f; v[20] = 16.7f; v[30] = 7.0f;
        r = pace_ft_sum(v, 100, 8.333);
        n++; if (r.out1 != 3 || fabs(r.p50 - 8.33) > 1e-3 || fabs(r.mx - 16.7) > 1e-3 || fabs(r.p99 - 9.5) > 1e-3) { fail++; printf("  PACING (c) out1=%d p50=%.3f p99=%.3f max=%.3f\n", r.out1, r.p50, r.p99, r.mx); }
    }
    { /* (d) gezeigte Spielzeit: doppelt (0), Sprung, Median */
        float g[60];
        pace_g_t r;
        for (i = 0; i < 60; i++) { g[i] = 1.0f / 6.0f; }
        g[5] = 0.0f; g[6] = 2.0f / 6.0f; g[40] = 0.5f;
        r = pace_g_sum(g, 60);
        n++; if (r.same != 1 || r.jump != 2 || fabs(r.med - 1.0 / 6.0) > 1e-6 || r.dev99 < 0.99) { fail++; printf("  PACING (d) same=%d jump=%d med=%.4f dev99=%.3f\n", r.same, r.jump, r.med, r.dev99); }
    }
    { /* (e) VBlank-Raster: Mitte zwischen zwei VBlanks (k VBlanks je Bild) bzw. gleichmaessig (m Bilder je VBlank) */
        LONGLONG st = 0, of = 0;
        n++; if (pace_vb_next(1020, 1000, 100, 50) != 1050 || pace_vb_next(1050, 1000, 100, 50) != 1150) { fail++; }
        n++; if (pace_vb_next(700, 1000, 100, 50) != 750 || pace_vb_next(650, 1000, 100, 50) != 750) { fail++; } /* vor vb */
        n++; if (!pace_vb_grid(60.0, f / 60, f, &st, &of) || st != f / 60 || of != f / 120) { fail++; }       /* 60 auf 60 Hz */
        n++; if (!pace_vb_grid(60.0, f / 120, f, &st, &of) || st != f / 60 || of != f / 240) { fail++; }      /* k = 2 */
        n++; if (!pace_vb_grid(120.0, f / 60, f, &st, &of) || st != f / 120 || of != f / 240) { fail++; }     /* m = 2 */
        n++; if (pace_vb_grid(144.0, f / 120, f, &st, &of) || pace_vb_grid(100.0, f / 60, f, &st, &of)) { fail++; } /* krumm */
        { /* Rasterpunkte nie naeher als ein Viertel Periode am VBlank, auch bei 119,9 Hz und 120 Bildern */
            LONGLONG per = f * 1000 / 119900, vb = 12345, t = 20000, last = 0;
            int bad = 0;
            if (!pace_vb_grid(120.0, per, f, &st, &of)) { bad++; }
            for (i = 0; i < 500 && !bad; i++) {
                LONGLONG s2 = pace_vb_next(t + f / 120 - st / 2, vb, st, of), ph = (s2 - vb) % per;
                if (last && s2 - last != st) { bad++; }
                if (ph < per / 4 || ph > per * 3 / 4) { bad++; }
                last = t = s2;
            }
            n++; if (bad) { fail++; printf("  PACING (e) %d Fehler im Raster\n", bad); }
        }
    }
    { /* (i) Tick-Zeitbasis: ein Tick ein Feld (16,7 ms) spaet, der naechste holt auf -> Abstaende 50 ms +-4 %, kein
       *     Neuansatz. Spiel verliert dauerhaft 70 ms bzw. ein Feld (Phase verschoben) -> Neuansatz sofort bzw. beim
       *     zweiten Tick, danach wieder Erwartung. Takt 1 % langsamer -> folgt ohne Neuansatz (Abstand beschraenkt). */
        LONGLONG P = f / 20, fld = f / 60, prev = 0, raw, r, worst = 0, le = 0, sh;
        int sn, snaps = 0, k2, at = -1;
        for (k2 = 1; k2 <= 40; k2++) {
            raw = (LONGLONG)k2 * P + (k2 == 10 ? fld : 0);
            r = pace_ts_step(prev, raw, P, &sn, &le);
            snaps += sn;
            if (k2 > 1) { LONGLONG d = r - prev - P; if (d < 0) { d = -d; } if (d > worst) { worst = d; } }
            prev = r;
        }
        n++; if (snaps || worst > P / 25) { fail++; printf("  PACING (i) Feld spaet: snaps=%d worst=%lld\n", snaps, worst); }
        for (sh = 0; sh < 2; sh++) { /* 0: 70 ms verloren, 1: ein Feld verloren (ab Tick 10 dauerhaft) */
            LONGLONG lost = sh ? fld : f * 70 / 1000;
            prev = 0; le = 0; snaps = 0; at = -1;
            for (k2 = 1; k2 <= 40; k2++) {
                raw = (LONGLONG)k2 * P + (k2 >= 10 ? lost : 0);
                prev = pace_ts_step(prev, raw, P, &sn, &le);
                if (sn) { snaps++; if (at < 0) { at = k2; } }
            }
            n++; if (snaps != 1 || at != (sh ? 11 : 10) || prev != 40 * P + lost) { fail++; printf("  PACING (i) verloren %lld: snaps=%d bei %d\n", lost, snaps, at); }
        }
        r = pace_ts_step(prev, prev + P + f / 5, P, &sn, &le);
        n++; if (!sn || r != prev + P + f / 5) { fail++; }
        prev = 0; worst = 0; le = 0; snaps = 0;
        for (k2 = 1; k2 <= 400; k2++) { /* Takt 1 % langsamer */
            raw = (LONGLONG)k2 * (P + P / 100);
            prev = pace_ts_step(prev, raw, P, &sn, &le);
            snaps += sn;
            if (k2 > 100) { LONGLONG d = prev - raw; if (d < 0) { d = -d; } if (d > worst) { worst = d; } }
        }
        n++; if (snaps || worst > P / 10) { fail++; printf("  PACING (i) Drift: snaps=%d Abstand %lld\n", snaps, worst); }
    }
    { /* (j) Elastisches Halten: Ticks alle 50 ms (Ankunft = Zustandszeit + 40 ms), Tick 20 kommt 30 ms spaet. Bild
       *     120 Hz, Latenz-Ziel 45 ms. Alt (L fest): nach dem Halten springt die gezeigte Zeit um ~30 ms nach vorn.
       *     Neu: gezeigte Zeit bleibt beim Halten stehen und laeuft danach mit <= 102 % weiter (kein Sprung). */
        LONGLONG P = f / 20, dt = f / 120, tgt = f * 45 / 1000, due, v, vlo = -1, vln = -1, Ln = 0, Lnd = 0;
        double jo = 0.0, jn = 0.0;
        int frames;
        for (frames = 1; frames < 600; frames++) {
            LONGLONG newest, arr;
            int k3;
            due = (LONGLONG)frames * dt + f;
            newest = -1;
            for (k3 = 0; k3 < 100; k3++) { /* juengster angekommener Tick zur Zeit due */
                arr = (LONGLONG)k3 * P + f * 40 / 1000 + (k3 == 20 ? f * 30 / 1000 : 0);
                if (arr <= due) { newest = (LONGLONG)k3 * P; }
            }
            if (newest < 0) { continue; }
            v = due - tgt; if (v > newest) { v = newest; }                       /* alt: Halten, danach Sprung */
            if (vlo >= 0) { double st = (double)(v - vlo) / (double)dt; if (st > jo) { jo = st; } }
            vlo = v;
            Ln = Ln > 0 ? pace_lat_step(Ln, tgt, Lnd ? due - Lnd : 0, 50, 20, f / 10) : tgt; /* neu */
            if (due - Ln > newest) { Ln = pace_lat_hold(Ln, due - newest, tgt, f / 10); }
            Lnd = due;
            v = due - Ln;
            if (vln >= 0) { double st = (double)(v - vln) / (double)dt; if (st > jn) { jn = st; } }
            vln = v;
        }
        n++; if (jo < 2.0) { fail++; printf("  PACING (j) alt ohne Sprung? %.2f\n", jo); }
        n++; if (jn > 1.021) { fail++; printf("  PACING (j) neu Schritt %.3f Bildabstaende\n", jn); }
    }
    { /* (f) Phase im Tick */
        n++; if (pace_phase_bin(125, 100, 50, 10) != 5 || pace_phase_bin(95, 100, 50, 10) != 9) { fail++; }
    }
    { /* (g) Doppelpuffer: Schreiber veroeffentlicht ohne Pause (auch zwei Freigaben dicht hintereinander), Leser liest
       *     200 000 Mal - nie ein zerrissener Stand, Folgenummer nie rueckwaerts, nie gesperrt. */
        HANDLE th;
        int torn = 0, back = 0, okn = 0;
        /* 8 KB je Stand: Lesen dauert lange genug, dass zwei Freigaben hineinfallen (mit 2 Plaetzen zerrissen) */
        LONG last = 0;
        static pace_tst_t o;
        pace_tst_seq = 0; pace_tst_stop = 0;
        memset(pace_tst_slots, 0, sizeof pace_tst_slots);
        th = CreateThread(NULL, 0, pace_tst_writer, NULL, 0, NULL);
        { DWORD w0 = GetTickCount(); while (!pace_tst_seq && GetTickCount() - w0 < 2000u) { Sleep(0); } } /* Schreiber laeuft */
        for (i = 0; i < 100000; i++) {
            int k, bad = 0;
            if (!pace_pub_read(&pace_tst_seq, pace_tst_slots, sizeof o, &o)) { continue; }
            okn++;
            for (k = 0; k < 2000; k++) { if (o.b[k] != o.a) { bad = 1; } }
            if (o.c != o.a) { bad = 1; }
            if (bad) { torn++; }
            if (o.a < last) { back++; }
            last = o.a;
        }
        InterlockedExchange(&pace_tst_stop, 1);
        if (th) { WaitForSingleObject(th, 5000); CloseHandle(th); }
        n++; if (torn || back || okn < 1000) { fail++; printf("  PACING (g) zerrissen=%d rueckwaerts=%d gelesen=%d\n", torn, back, okn); }
    }
    { /* (h) Ablauf fest nachgestellt: Leser hat q gesehen, der Schreiber gibt q+1 frei und schreibt gerade q+2 (halb).
       *     Der Leser kopiert jetzt und prueft: Freigabe q+1 erlaubt, sein Platz q darf nicht der halb geschriebene sein
       *     (mit zwei Plaetzen waere er es: zerrissener Stand). */
        static pace_tst_t sl[PACE_PUB_SLOTS], w, o2;
        volatile LONG sq = 0;
        LONG q, k2;
        int bad = 0;
        memset(sl, 0, sizeof sl);
        for (k2 = 1; k2 <= 5; k2++) { w.a = w.c = k2; for (i = 0; i < 2000; i++) { w.b[i] = k2; } pace_pub_write(&sq, sl, sizeof w, &w); }
        q = sq;                                                              /* Leser: Folgenummer gelesen */
        w.a = w.c = q + 1; for (i = 0; i < 2000; i++) { w.b[i] = q + 1; }
        pace_pub_write(&sq, sl, sizeof w, &w);                               /* Schreiber: q+1 frei */
        { pace_tst_t *half = &sl[(q + 2) % PACE_PUB_SLOTS];                  /* Schreiber: q+2 halb geschrieben */
          half->a = q + 2; for (i = 0; i < 1000; i++) { half->b[i] = q + 2; } }
        memcpy(&o2, &sl[q % PACE_PUB_SLOTS], sizeof o2);                     /* Leser kopiert seinen Platz */
        if ((LONG)(sq - q) <= 1) {                                           /* Pruefung wuerde bestehen */
            for (i = 0; i < 2000; i++) { if (o2.b[i] != o2.a) { bad = 1; } }
            if (o2.c != o2.a || o2.a != q) { bad = 1; }
        }
        n++; if (bad) { fail++; printf("  PACING (h) zerrissener Stand trotz bestandener Pruefung\n"); }
    }
    printf("PACING (W393): %d Faelle (stetige Bilduhr, Kennzahlen, VBlank-Raster, Phase, Doppelpuffer 3 Plaetze): %d Fehler\n", n, fail);
    g_fail += fail;
}

static void test_locsmoothopp(void)
{
    int n = 0, fail = 0, i, r;
    unsigned t;
    ls_hist_t h, pre, post;
    ls_auto_t a;
    double o[4], off[4], X[16], c[3], q[3];
    /* (a) */
    ls_hist_reset(&h);
    for (t = 100; t <= 105; t++) lso_put(&h, t, 7u, 10.0 * t, 0.1 * (t - 100));
    r = ls_hist_at(&h, 105, 7u, 0.0, o); n++; if (r != 2 || o[0] != 1050.0) { fail++; }
    r = ls_hist_at(&h, 105, 7u, 1.0, o); n++; if (r != 2 || o[0] != 1040.0 || fabs(o[2] + 2080.0) > 1e-9) { fail++; }
    r = ls_hist_at(&h, 105, 7u, 0.5, o); n++; if (r != 2 || fabs(o[0] - 1045.0) > 1e-9 || fabs(o[3] - 0.45) > 1e-9) { fail++; }
    r = ls_hist_at(&h, 105, 7u, 1.25, o); n++; if (r != 2 || fabs(o[0] - 1037.5) > 1e-9) { fail++; }
    r = ls_hist_at(&h, 105, 8u, 0.0, o); n++; if (r != 0) { fail++; } /* anderes Objekt (Neustart) */
    r = ls_hist_at(&h, 106, 7u, 0.0, o); n++; if (r != 0) { fail++; } /* Tick fehlt */
    lso_put(&h, 104, 7u, 1040.0, 3.1);
    lso_put(&h, 105, 7u, 1050.0, -3.1);
    r = ls_hist_at(&h, 105, 7u, 0.5, o); n++; if (r != 2 || fabs(fabs(o[3]) - LS_PI) > 1e-9) { fail++; } /* Wrap */
    ls_hist_reset(&h);
    lso_put(&h, 105, 7u, 1050.0, 0.0);
    lso_put(&h, 104, 7u, 1040.0, 0.0);
    lso_put(&h, 102, 7u, 1020.0, 0.0); /* 103 fehlt */
    r = ls_hist_at(&h, 105, 7u, 1.5, o); n++; if (r != 1 || o[0] != 1040.0) { fail++; }
    r = ls_hist_at(&h, 105, 7u, 2.5, o); n++; if (r != 1 || o[0] != 1040.0) { fail++; }
    lso_put(&h, 97, 7u, 970.0, 0.0); /* ueberschreibt Platz 105 & 7 = 1 -> Tick 105 fehlt */
    r = ls_hist_at(&h, 105, 7u, 0.0, o); n++; if (r != 0) { fail++; }
    /* (b) */
    ls_hist_reset(&h);
    for (t = 200; t <= 204; t++) lso_put(&h, t, 3u, 100.0 * t, 0.01 * t);
    r = ls_delay_off(&h, 204, 3u, 0.0, off); n++; if (r != 2 || off[0] != 0.0 || off[3] != 0.0) { fail++; }
    r = ls_delay_off(&h, 204, 3u, 0.6, off);
    n++; if (r != 2 || fabs(off[0] + 60.0) > 1e-9 || fabs(off[2] - 120.0) > 1e-9 || fabs(off[3] + 0.006) > 1e-9) { fail++; }
    {
        double p[3] = {20400.0, 5.0, -40800.0}, want[3] = {20340.0, 5.0, -40680.0};
        ls_obj_xform(X, p, off);
        ls_xform_pt(q, p, X);
        n++; if (ls_d3(q, want) > 1e-6) { fail++; }
        c[0] = p[0] + 1000.0; c[1] = p[1]; c[2] = p[2]; /* Punkt 1 m daneben dreht um dGier mit */
        ls_xform_pt(q, c, X);
        n++; if (fabs(sqrt((q[0] - want[0]) * (q[0] - want[0]) + (q[2] - want[2]) * (q[2] - want[2])) - 1000.0) > 1e-6) { fail++; }
    }
    r = ls_delay_off(&h, 204, 9u, 1.0, off); n++; if (r != 0 || off[0] != 0.0) { fail++; }
    /* (c) */
    ls_hist_reset(&pre);
    for (t = 300; t <= 305; t++) lso_put(&pre, t, 1u, 10.0 * t, 0.0);
    post = pre;
    n++; if (ls_need(&pre, &post, 305, 1u, 20.0, 2) != 0) { fail++; }
    lso_put(&post, 305, 1u, 3050.0 + 50.0, 0.0);
    n++; if (ls_need(&pre, &post, 305, 1u, 20.0, 2) != 1) { fail++; }
    n++; if (ls_need(&pre, &post, 305, 1u, 200.0, 2) != 0) { fail++; } /* weiter weg: Schwelle hoeher */
    lso_put(&post, 304, 1u, 3040.0 + 30.0, 0.0);
    n++; if (ls_need(&pre, &post, 305, 1u, 20.0, 2) != 2) { fail++; }
    lso_put(&post, 303, 1u, 3030.0, 0.02); /* Gier 0,02 rad * 2000 = 40 */
    n++; if (ls_need(&pre, &post, 305, 1u, 20.0, 2) != 3) { fail++; }
    lso_put(&post, 303, 1u, 3030.0 + 5.0, 0.0);
    n++; if (ls_need(&pre, &post, 305, 1u, 20.0, 2) != 2) { fail++; }
    /* (d) aus / fest */
    ls_auto_reset(&a);
    ls_auto_event(&a, 2); ls_auto_event(&a, 2); ls_auto_event(&a, 2);
    for (i = 0; i < 50; i++) { n++; if (ls_auto_step(&a, 0, 2) != 0.0) { fail++; break; } }
    ls_auto_reset(&a);
    for (i = 1; i <= 40; i++) {
        double d = ls_auto_step(&a, 2, 2);
        if (i == 10) { n++; if (fabs(d - 0.5) > 1e-9) { fail++; } }
        if (i >= 20) { n++; if (fabs(d - 1.0) > 1e-9) { fail++; break; } }
    }
    /* auto: ohne Korrekturen 0; eine sichtbare reicht nicht; zwei -> 1 */
    ls_auto_reset(&a);
    for (i = 0; i < 1000; i++) ls_auto_step(&a, 1, 2);
    n++; if (a.d != 0.0 || a.target != 0) { fail++; }
    ls_auto_event(&a, 0); ls_auto_event(&a, 1);
    for (i = 0; i < 100; i++) ls_auto_step(&a, 1, 2);
    n++; if (a.d != 0.0 || a.target != 0) { fail++; }
    ls_auto_event(&a, 1);
    ls_auto_step(&a, 1, 2);
    n++; if (a.target != 1 || fabs(a.d - LS_AUP) > 1e-12) { fail++; }
    for (i = 0; i < 19; i++) ls_auto_step(&a, 1, 2);
    n++; if (fabs(a.d - 1.0) > 1e-9) { fail++; }
    /* Absenken: Ziel 1 bis Fenster + Ruhezeit nach der letzten Korrektur, dann Rampe ab */
    {
        unsigned last = a.ev_t[(a.ev_n - 1u) % LS_AEV];
        int ok480 = 0, ok520 = 0, ok570 = 0;
        double prev = a.d, dmax = 0.0;
        while (a.now < last + 600u) {
            double d = ls_auto_step(&a, 1, 2);
            if (fabs(d - prev) > dmax) dmax = fabs(d - prev);
            prev = d;
            if (a.now == last + 480u) ok480 = a.target == 1 && fabs(a.d - 1.0) < 1e-9;
            if (a.now == last + 520u) ok520 = a.target == 0 && a.d < 1.0;
            if (a.now == last + 570u) ok570 = a.d == 0.0;
        }
        n++; if (!ok480) { fail++; }
        n++; if (!ok520) { fail++; }
        n++; if (!ok570) { fail++; }
        n++; if (dmax > LS_ADN + 1e-12) { fail++; }
    }
    /* starker Jitter: 3 x Bedarf 2 -> 2 (Obergrenze 1 -> 1) */
    ls_auto_reset(&a);
    ls_auto_event(&a, 2); ls_auto_event(&a, 3); ls_auto_event(&a, 2);
    for (i = 0; i < 60; i++) ls_auto_step(&a, 1, 2);
    n++; if (a.target != 2 || fabs(a.d - 2.0) > 1e-9) { fail++; }
    ls_auto_reset(&a);
    ls_auto_event(&a, 2); ls_auto_event(&a, 3); ls_auto_event(&a, 2);
    for (i = 0; i < 60; i++) ls_auto_step(&a, 1, 1);
    n++; if (a.target != 1 || fabs(a.d - 1.0) > 1e-9) { fail++; }
    /* 2 -> 1 nach Fenster + Ruhe, 1 -> 0 nach weiterer Ruhe */
    ls_auto_reset(&a);
    ls_auto_event(&a, 2); ls_auto_event(&a, 2); ls_auto_event(&a, 2);
    for (i = 0; i < 520; i++) ls_auto_step(&a, 1, 2);
    n++; if (a.target != 1) { fail++; }
    for (i = 0; i < 260; i++) ls_auto_step(&a, 1, 2);
    n++; if (a.target != 0 || a.d != 0.0) { fail++; }
    /* Zufallsfolge: beschraenkt, nie sprunghaft */
    {
        unsigned s = 12345u;
        double prev = 0.0, worst = 0.0, lo = 0.0, hi = 0.0;
        ls_auto_reset(&a);
        for (i = 0; i < 20000; i++) {
            double d;
            s = s * 1103515245u + 12345u;
            if (((s >> 16) & 63u) == 0u) ls_auto_event(&a, (int)((s >> 8) & 3u));
            d = ls_auto_step(&a, 1, 2);
            if (fabs(d - prev) > worst) worst = fabs(d - prev);
            if (d < lo) lo = d;
            if (d > hi) hi = d;
            prev = d;
        }
        n++; if (worst > LS_AUP + 1e-12 || lo < 0.0 || hi > 2.0 || hi <= 0.0) { fail++; }
    }
    printf("LOCSMOOTHOPP (W387): %d Faelle (Historie T-d, Wrap, Luecken, Versatz/Objekt, Bedarf, Auto aus/fest/auto, Hysterese, Jitter, Zufallsfolge): %d Fehler\n", n, fail);
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

/* ---------- W383 LOC-GH-DATA (#62): Strom-2-Expansion bleibt im reservierten Platz ----------
 * Nachbau des Absturzes: Dreiecksstreifen mit 30000 Vertices, Platz fuer 65536 Eintraege (IM_VB_MAX) zu je 64 Bytes.
 * Die alte Schleife (Grenze IM_VB_MAX*3) haette 89994 Eintraege geschrieben (Tester: Eintrag 70828 lag hinter dem
 * Mapping). Prueft Kappung, Waechterbereich, Reihenfolge je Primitiv und die unveraenderte Expansion ohne Kappung. */
#define W383_VBMAX 65536u
#define W383_ST 64u
static unsigned w383_src(const unsigned char *X, unsigned k) { unsigned v; memcpy(&v, X + (size_t)k * W383_ST, 4); return v; }
static int w383_guard(const unsigned char *X, unsigned cap)
{
    unsigned i;
    for (i = 0; i < 4096u; i++) { if (X[(size_t)cap * W383_ST + i] != 0xCDu) { return 0; } }
    return 1;
}
static void test_rqexp(void)
{
    int fail = 0, n = 0;
    unsigned i, e, t, cap = W383_VBMAX;
    size_t xb = (size_t)cap * W383_ST + 4096u;
    unsigned char *X = (unsigned char *)malloc(xb), *P = (unsigned char *)malloc((size_t)80000u * W383_ST);
    if (!X || !P) { printf("RQEXP (W383): kein Speicher\n"); g_fail++; free(X); free(P); return; }
    for (i = 0; i < 80000u; i++) { memset(P + (size_t)i * W383_ST, 0x11, W383_ST); memcpy(P + (size_t)i * W383_ST, &i, 4); }
    /* 1. Streifen 30000 (Absturzfall) */
    memset(X, 0xCD, xb);
    e = rq_exp_fill(X, P, W383_ST, 6u, 30000u, cap, W383_VBMAX);
    n++; if (e != 65535u || !w383_guard(X, cap)) { fail++; printf("  RQEXP Streifen: e=%u Waechter=%d\n", e, w383_guard(X, cap)); }
    n++; for (t = 0; t < e / 3u; t += 997u) { if (w383_src(X, 3u * t) != t || w383_src(X, 3u * t + 1u) != t + 1u || w383_src(X, 3u * t + 2u) != t + 2u) { fail++; break; } }
    /* 2. Facher 30000 */
    memset(X, 0xCD, xb);
    e = rq_exp_fill(X, P, W383_ST, 7u, 30000u, cap, W383_VBMAX);
    n++; if (e != 65535u || !w383_guard(X, cap) || w383_src(X, 3u * 20000u) != 0u || w383_src(X, 3u * 20000u + 2u) != 20002u) { fail++; }
    /* 3. Viereckliste 60000 und 4. Viereckstreifen 30000 (je 90000 ungekappt) */
    memset(X, 0xCD, xb);
    e = rq_exp_fill(X, P, W383_ST, 8u, 60000u, cap, W383_VBMAX);
    n++; if (e != 65532u || !w383_guard(X, cap) || w383_src(X, 6u * 10921u + 5u) != 4u * 10921u + 3u) { fail++; }
    memset(X, 0xCD, xb);
    e = rq_exp_fill(X, P, W383_ST, 9u, 30000u, cap, W383_VBMAX);
    n++; if (e != 65532u || !w383_guard(X, cap) || w383_src(X, 6u * 10921u + 5u) != 2u * 10921u + 3u) { fail++; }
    /* 5. Liste 70000 > Platz */
    memset(X, 0xCD, xb);
    e = rq_exp_fill(X, P, W383_ST, 5u, 70000u, cap, W383_VBMAX);
    n++; if (e != cap || !w383_guard(X, cap) || w383_src(X, cap - 1u) != cap - 1u) { fail++; }
    /* 6. Linienschleife: voll ohne Schluss, klein mit Schluss */
    memset(X, 0xCD, xb);
    e = rq_exp_fill(X, P, W383_ST, 3u, W383_VBMAX, cap, W383_VBMAX);
    n++; if (e != cap || !w383_guard(X, cap)) { fail++; }
    e = rq_exp_fill(X, P, W383_ST, 3u, 10u, cap, W383_VBMAX);
    n++; if (e != 11u || w383_src(X, 10u) != 0u || w383_src(X, 9u) != 9u) { fail++; }
    /* 7. ohne Kappung wie bisher (Platz = exakte Zahl): Streifen 10 -> 24, Waechter direkt dahinter */
    memset(X, 0xCD, xb);
    e = rq_exp_fill(X, P, W383_ST, 6u, 10u, 24u, W383_VBMAX);
    n++; if (e != 24u || !w383_guard(X, 24u) || w383_src(X, 23u) != 9u || w383_src(X, 21u) != 7u) { fail++; }
    /* 8. stride 0 (Programm ohne v-Register): nichts geschrieben */
    memset(X, 0xCD, xb);
    e = rq_exp_fill(X, P, 0u, 6u, 30000u, cap, W383_VBMAX);
    n++; if (e != 65535u || X[0] != 0xCDu) { fail++; }
    printf("RQEXP (W383): %d Faelle (Strom-2-Expansion gekappt auf reservierten Platz, Waechter, Reihenfolge): %d Fehler\n", n, fail);
    g_fail += fail;
    free(X);
    free(P);
}

/* ---------- W383 LOC-GH-DATA (#60): nie ladbare Texturen fressen nicht das Upload-Budget ----------
 * Modell von sb_tex_get/tex_upload mit der echten Zulassung (tex_budget_admit): 17 Bump-Eintraege (Format nicht
 * ladbar, je 180 Draws im Bild, Gelaende vor dem Cockpit; Tester: n=180 je Eintrag und Bild) und 15 Cockpit-
 * Eintraege, deren Inhalt zu Beginn von Runde 2 neu ist (Refresh je 16 Gets, je 4 Draws im Bild). Start mit
 * gedrosseltem Missionsbeginn (alle Bump-Eintraege "an der Drossel gescheitert", W307 thr). */
typedef struct { int fmt_ok, thr, fresh; unsigned gets; } w383_te_t;
static void w383_tex_get(w383_te_t *e, int fmt_first, unsigned *count, unsigned *attempts)
{
    unsigned pf = 0;
    int attempt;
    if (!e->fmt_ok) { attempt = e->thr || ((++e->gets & 15u) == 0u); } /* ohne SRV: W307-Retry bzw. 16. Get */
    else { attempt = !e->fresh && ((++e->gets & 15u) == 0u); }       /* Refresh-Takt (Hash neu) */
    if (!attempt) { return; }
    (*attempts)++;
    if (tex_budget_admit(e->fmt_ok, fmt_first, count, 12u, 0, &pf)) { e->fresh = 1; }
    if (!e->fmt_ok || !e->fresh) { e->thr = (pf == 2u); }
}
static unsigned w383_sim(int fmt_first, unsigned frames, unsigned *bump_slots)
{
    w383_te_t t[32];
    unsigned f, k, d, count, att, done_at = 0, used = 0;
    for (k = 0; k < 32u; k++) { t[k].fmt_ok = k >= 17u; t[k].thr = k < 17u; t[k].fresh = 0; t[k].gets = k * 5u; }
    for (f = 1; f <= frames; f++) {
        unsigned c0;
        count = 0; att = 0;
        for (d = 0; d < 180u; d++) { for (k = 0; k < 17u; k++) { w383_tex_get(&t[k], fmt_first, &count, &att); } }
        c0 = count; used += c0;
        for (d = 0; d < 4u; d++) { for (k = 17u; k < 32u; k++) { w383_tex_get(&t[k], fmt_first, &count, &att); } }
        if (!done_at) {
            unsigned ok = 0;
            for (k = 17u; k < 32u; k++) { ok += (unsigned)t[k].fresh; }
            if (ok == 15u) { done_at = f; }
        }
    }
    *bump_slots = used;
    return done_at;
}
static void test_texbudget(void)
{
    int fail = 0, n = 0;
    unsigned pf = 0, c = 0, slots_new = 0, slots_old = 0, fnew, fold;
    /* Zulassung einzeln */
    n++; if (tex_budget_admit(0, 1, &c, 12u, 0, &pf) || pf != 4u || c != 0u) { fail++; }   /* neu: kein Platz */
    n++; if (tex_budget_admit(0, 0, &c, 12u, 0, &pf) || pf != 4u || c != 1u) { fail++; }   /* alt: Platz belegt */
    c = 12u;
    n++; if (tex_budget_admit(1, 1, &c, 12u, 0, &pf) || pf != 2u || c != 12u) { fail++; }  /* voll: Drossel */
    n++; if (!tex_budget_admit(1, 1, &c, 12u, 1, &pf) || pf != 0u || c != 13u) { fail++; } /* Pause: frei */
    c = 0;
    n++; if (!tex_budget_admit(1, 0, &c, 12u, 0, &pf) || pf != 0u || c != 1u) { fail++; }  /* ladbar: wie alt */
    /* Modell Runde 2 */
    fnew = w383_sim(1, 600u, &slots_new);
    fold = w383_sim(0, 600u, &slots_old);
    n++; if (!fnew || fnew > 10u) { fail++; }        /* neu: Cockpit nach wenigen Bildern frisch */
    n++; if (slots_new != 0u) { fail++; }            /* neu: Bump-Eintraege belegen keinen Platz */
    n++; if (fold != 0u || slots_old < 600u * 12u) { fail++; } /* Gegenprobe alt: Budget jedes Bild weg, Cockpit nie */
    printf("TEXBUDGET (W383): %d Faelle (Drossel-Zulassung, Modell Runde 2: Cockpit frisch nach %u Bildern, alt %s, "
           "Bump-Plaetze neu %u / alt %u in 600 Bildern): %d Fehler\n", n, fnew, fold ? "frisch" : "nie", slots_new,
           slots_old, fail);
    g_fail += fail;
}

/* ---------- W377 LOC-MENU-FPS: Wiederholbild-Takt der LoC-Menues, Textebenen-Ring ---------- */
/* Simuliert 2 s Menue mit 30-Hz-Spiel-Presents (Zeit in us) und dem Takt-Thread; zaehlt Presents je Spielbild,
 * den kleinsten Abstand eines Wiederholbilds vor dem naechsten Spiel-Present und den groessten Abstand. */
static void lm_sim(int rate, int fps_limit, long long game_per, long long late_every, long long late_us, int gpu_busy_us,
                   int *rep, double *min_gap_before_game, double *max_gap)
{
    long long dt = 1000000 / lm_rate(rate, fps_limit), t, next_game = 0, last = 0, dl = 0, prev_pres = 0;
    long long ev[2] = { 0, 0 }; /* Fertigzeit der letzten beiden Presents (Ring wie render_locmenu.c) */
    int evk = 0, ng = 0;
    lm_clock_t c;
    memset(&c, 0, sizeof c);
    *rep = 0; *min_gap_before_game = 1e9; *max_gap = 0;
    for (t = 1000; t < 2001000; t += 50) { /* 50-us-Raster */
        if (t >= next_game) { /* Spiel-Present (nie verzoegert) */
            if (ng >= 3 && prev_pres && (double)(t - prev_pres) > *max_gap) { *max_gap = (double)(t - prev_pres); } /* ab Spielbild 3 (Periode bekannt) */
            if (ng >= 3 && last && last != c.game && (double)(t - last) < *min_gap_before_game) { *min_gap_before_game = (double)(t - last); }
            lm_clock_game(&c, t, dt, 10000, 125000);
            last = prev_pres = t; ev[evk] = t + gpu_busy_us; evk ^= 1; ng++;
            next_game = t + game_per + ((late_every && ng % late_every == 0) ? late_us : 0);
            dl = 0;
            continue;
        }
        if (t >= dl) {
            int a = lm_clock_decide(&c, t, dt, t >= ev[evk], &dl); /* vorletztes Present fertig? */
            if (a == LM_REPEAT) {
                if (ng >= 3 && prev_pres && (double)(t - prev_pres) > *max_gap) { *max_gap = (double)(t - prev_pres); }
                last = prev_pres = t; ev[evk] = t + gpu_busy_us; evk ^= 1; (*rep)++;
                lm_clock_done(&c, t);
            }
        }
    }
}

static void test_locmenu(void)
{
    int fail = 0, n = 0, rep;
    double mg, mx;
    /* Bildrate / Menue-Limit */
    n++; if (lm_rate(120, 120) != 120 || lm_rate(144, 120) != 120 || lm_rate(144, 0) != 144 || lm_rate(0, 0) != 60 ||
             lm_rate(120, 60) != 60) { fail++; }
    /* 30-Hz-Menue bei 120: 3 Wiederholbilder je Spielbild (~90/s), keins in der halben Periode vor dem Spiel-Present */
    lm_sim(120, 0, 33333, 0, 0, 500, &rep, &mg, &mx);
    n++; if (rep < 170 || rep + 60 > 242 || mg < 4100 || mx > 9000) { fail++; printf("LOCMENU 120: rep=%d gap_vor_spiel=%.0f max=%.0f\n", rep, mg, mx); }
    /* 144 Hz: im Mittel 3,8 je Spielbild, zusammen hoechstens 144/s */
    lm_sim(144, 0, 33333, 0, 0, 500, &rep, &mg, &mx);
    n++; if (rep < 215 || rep + 60 > 290 || mg < 3400 || mx > 9000) { fail++; printf("LOCMENU 144: rep=%d gap_vor_spiel=%.0f max=%.0f\n", rep, mg, mx); }
    /* Menue-Limit 60: 1 je Spielbild */
    lm_sim(144, 60, 33333, 0, 0, 500, &rep, &mg, &mx);
    n++; if (rep < 55 || rep + 60 > 122 || mx > 17500) { fail++; printf("LOCMENU Limit 60: rep=%d max=%.0f\n", rep, mx); }
    /* Bildrate <= Spieltakt: keine Wiederholbilder */
    lm_sim(30, 0, 33333, 0, 0, 500, &rep, &mg, &mx);
    n++; if (rep != 0) { fail++; printf("LOCMENU 30: rep=%d\n", rep); }
    /* jedes 5. Spielbild 40 ms zu spaet: Wiederholbilder laufen weiter (kein Loch > 2 Bildperioden) */
    lm_sim(120, 0, 33333, 5, 40000, 500, &rep, &mg, &mx);
    n++; if (mx > 16700 || rep < 180) { fail++; printf("LOCMENU spaet: rep=%d max=%.0f\n", rep, mx); }
    /* GPU langsam (je Present 20 ms): hoechstens ein Present in Arbeit -> deutlich weniger Wiederholbilder */
    lm_sim(120, 0, 33333, 0, 0, 20000, &rep, &mg, &mx);
    n++; if (rep > 130) { fail++; printf("LOCMENU GPU langsam: rep=%d\n", rep); }
    /* Hash: ganzer Inhalt, jedes Bit an jeder Stelle zaehlt, Vertauschen zaehlt, nie 0 */
    {
        static unsigned long long buf[640 * 480 / 2];
        unsigned long long h0, h1, h2, h3;
        unsigned pos, bad = 0;
        memset(buf, 0, sizeof buf);
        h0 = lm_hash64(buf, 640 * 480 / 4);
        for (pos = 0; pos < 640u * 480u / 2u; pos += 997u) {
            unsigned bit = (pos * 7u) & 63u;
            buf[pos] ^= 1ull << bit;
            if (lm_hash64(buf, 640 * 480 / 4) == h0) { bad++; }
            buf[pos] ^= 1ull << bit;
        }
        buf[10] = 0x1111111111111111ull; buf[11] = 0x2222222222222222ull;
        h1 = lm_hash64(buf, 640 * 480 / 4);
        buf[10] = 0x2222222222222222ull; buf[11] = 0x1111111111111111ull;
        h2 = lm_hash64(buf, 640 * 480 / 4);
        h3 = lm_hash64(buf, 640 * 480 / 4);
        /* verschobener Text: dasselbe Muster an anderer Stelle (Vielfache von 16/64 Byte, eine Bildzeile = 2560 Byte)
         * und zwei vertauschte Muster muessen andere Hashes ergeben (W377: Summe ohne Positionsschluessel war
         * vertauschungsinvariant -> falscher Textebenen-Zustand in der Lobby) */
        {
            static const unsigned offs[] = { 0u, 16u, 64u, 256u, 2560u, 2576u, 25600u, 640u * 4u * 100u, 1228800u - 128u };
            unsigned long long hs[9];
            unsigned a, c, w;
            unsigned char *bb = (unsigned char *)buf;
            for (a = 0; a < 9; a++) {
                memset(buf, 0, sizeof buf);
                for (w = 0; w < 64u; w++) { bb[offs[a] + w] = (unsigned char)(w * 37u + 11u); }
                hs[a] = lm_hash64(buf, 640 * 480 / 4);
                for (c = 0; c < a; c++) { if (hs[c] == hs[a]) { bad++; } }
            }
            memset(buf, 0, sizeof buf);
            for (w = 0; w < 64u; w++) { bb[2560u + w] = (unsigned char)(w + 1u); bb[25600u + w] = (unsigned char)(200u - w); }
            h0 = lm_hash64(buf, 640 * 480 / 4);
            memset(buf, 0, sizeof buf);
            for (w = 0; w < 64u; w++) { bb[25600u + w] = (unsigned char)(w + 1u); bb[2560u + w] = (unsigned char)(200u - w); }
            if (lm_hash64(buf, 640 * 480 / 4) == h0) { bad++; }
            memset(buf, 0, sizeof buf);
            h0 = lm_hash64(buf, 640 * 480 / 4);
        }
        n++; if (!h0 || !h1 || h1 == h2 || h2 != h3 || bad) { fail++; printf("LOCMENU Hash: bad=%u\n", bad); }
    }
    /* Ring: Treffer nach Kopf + Groesse + Hash + Device, Referenzen, Verdraengen */
    {
        lm_txr_t r[2];
        unsigned tick = 0;
        D3D11_TEXTURE2D_DESC td;
        ID3D11Texture2D *tx[3] = { NULL, NULL, NULL };
        ID3D11ShaderResourceView *sv[3] = { NULL, NULL, NULL };
        int k, ok = 1;
        memset(r, 0, sizeof r);
        memset(&td, 0, sizeof td);
        td.Width = td.Height = 4; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        for (k = 0; k < 3 && ok; k++) {
            ok = SUCCEEDED(ID3D11Device_CreateTexture2D(g_d, &td, NULL, &tx[k])) &&
                 SUCCEEDED(ID3D11Device_CreateShaderResourceView(g_d, (ID3D11Resource *)tx[k], NULL, &sv[k]));
        }
        if (!ok) { printf("LOCMENU: Anlegen fehlgeschlagen\n"); g_fail++; return; }
        lm_txr_put(r, 2, &tick, 0x7264E4u, 640, 480, 11ull, 0u, g_d, sv[0]);
        lm_txr_put(r, 2, &tick, 0x7264E4u, 640, 480, 22ull, 0u, g_d, sv[1]);
        lm_txr_put(r, 2, &tick, 0x7264E4u, 640, 480, 22ull, 0u, g_d, sv[1]); /* doppelt: keine zweite Referenz */
        n++; if (lm_txr_find(r, 2, &tick, 0x7264E4u, 640, 480, 11ull, g_d) < 0 ||
                 lm_txr_find(r, 2, &tick, 0x7264E4u, 640, 480, 33ull, g_d) >= 0 ||
                 lm_txr_find(r, 2, &tick, 0x667D00Cu, 640, 480, 11ull, g_d) >= 0 ||
                 lm_txr_find(r, 2, &tick, 0x7264E4u, 512, 480, 11ull, g_d) >= 0 ||
                 lm_txr_find(r, 2, &tick, 0x7264E4u, 640, 480, 11ull, NULL) >= 0) { fail++; }
        /* 11 zuletzt benutzt -> 22 wird verdraengt */
        lm_txr_put(r, 2, &tick, 0x7264E4u, 640, 480, 33ull, 0u, g_d, sv[2]);
        n++; if (lm_txr_find(r, 2, &tick, 0x7264E4u, 640, 480, 22ull, g_d) >= 0 ||
                 lm_txr_find(r, 2, &tick, 0x7264E4u, 640, 480, 11ull, g_d) < 0 ||
                 lm_txr_find(r, 2, &tick, 0x7264E4u, 640, 480, 33ull, g_d) < 0) { fail++; }
        /* Referenzen: sv[1] nur noch eigene (1), sv[0]/sv[2] eigene + Ring (2) */
        {
            ULONG c0, c1, c2;
            ID3D11ShaderResourceView_AddRef(sv[0]); c0 = ID3D11ShaderResourceView_Release(sv[0]);
            ID3D11ShaderResourceView_AddRef(sv[1]); c1 = ID3D11ShaderResourceView_Release(sv[1]);
            ID3D11ShaderResourceView_AddRef(sv[2]); c2 = ID3D11ShaderResourceView_Release(sv[2]);
            n++; if (c0 != 2u || c1 != 1u || c2 != 2u) { fail++; printf("LOCMENU Ring-Referenzen %lu/%lu/%lu\n", c0, c1, c2); }
        }
        for (k = 0; k < 2; k++) { if (r[k].srv) { ID3D11ShaderResourceView_Release(r[k].srv); } }
        for (k = 0; k < 3; k++) { ID3D11ShaderResourceView_Release(sv[k]); ID3D11Texture2D_Release(tx[k]); }
    }
    printf("LOCMENU (W377): %d Faelle (Bildrate/Menue-Limit, Wiederholbild-Takt 120/144/60/30 Hz, spaete Spielbilder, "
           "langsame GPU, Inhalts-Hash, Textebenen-Ring): %d Fehler\n", n, fail);
    g_fail += fail;
}

/* ---------- W416 LOC-ESC-MENU: ESC-Zeilen ohne Wirkung in LoC (sb_ovl_loclock.h) ----------
 * LoC-Profil -> gesperrt mit Grund (gleiche Gruende wie Launcher kLocLock), SB-Profil -> nichts gesperrt. */
static void test_ovlloclock(void)
{
    static const char *const lock[][2] = {
        { "monitor_fx", "ovl.ll_monfx" }, { "monitor_grain", "ovl.ll_monfx" }, { "cockpit_hide", "ovl.ll_cockpit" },
        { "monitor_view", "ovl.ll_cockpit" }, { "cockpit_persp", "ovl.ll_cockpit" }, { "hud_label_avoid", "ovl.ll_cockpit" },
        { "aim_turn", "ovl.ll_assist" }, { "replay_label", "ovl.ll_replay" },
        { "sb_allied_squads", "ovl.ll_assist" }, /* W424; W440: aim_assist/auto_gear frei mit Hinweis (unten) */
        { "watermark_media", "ovl.ll_replay" }, { "replay_fx_live", "ovl.ll_replay" }, { "remaster", "ovl.ll_packs" },
        { "hd_textures", "ovl.ll_packs" }, { "vr_enable", "ovl.ll_vr" }, { "vr_preset", "ovl.ll_vr" },
        { "save_lock", "ovl.ll_savelock" }, { "cockpit_glow", "ovl.ll_post" }, { "cockpit_light", "ovl.ll_post" },
        { "color_sat", "ovl.ll_post" }, { "color_gain", "ovl.ll_post" }, { "ssao", "ovl.ll_post" },
    };
    static const char *const freek[] = { "interp_fps", "window_mode", "rt_scale", "shading", "world_fog", "light_world",
                                         "light_cockpit", "bloom_strength", "shade_shadow", "aa_mode", "world_sharpen",
                                         "fx_soft", "fx_light_cockpit", "mouse_mode", "mouse_sens", "stick_aim",
                                         "sbc_mouse_off", "menu_pause", "volume_master", "volume_voice",
                                         "auto_gear" /* W441 */, "", NULL };
    static const char *const edge[] = { "edge_mode", "edge_fade_width", "edge_fade_strength" };
    int fail = 0, n = 0, k, w;
    const char *r;
    for (k = 0; k < (int)(sizeof lock / sizeof lock[0]); k++) {
        for (w = 0; w < 2; w++) {
            r = sb_ovl_loc_lock(lock[k][0], 1, w);
            n++; if (!r || strcmp(r, lock[k][1]) != 0) { printf("OVLLOCK FAIL LoC %s (16:9=%d): %s\n", lock[k][0], w, r ? r : "frei"); fail++; }
            n++; if (sb_ovl_loc_lock(lock[k][0], 0, w)) { printf("OVLLOCK FAIL SB %s gesperrt\n", lock[k][0]); fail++; }
        }
        n++; if (strcmp(sb_ovl_loc_lock_tag(sb_ovl_loc_lock(lock[k][0], 1, 0)), "ovl.ll_tag") != 0) { fail++; }
    }
    for (k = 0; k < (int)(sizeof freek / sizeof freek[0]); k++) {
        for (w = 0; w < 2; w++) {
            n++; if (sb_ovl_loc_lock(freek[k], 1, w) || sb_ovl_loc_lock(freek[k], 0, w)) { printf("OVLLOCK FAIL frei %s\n", freek[k] ? freek[k] : "(null)"); fail++; }
        }
    }
    for (k = 0; k < 3; k++) { /* Raender: nur LoC mit 16:9 fest (Launcher pergame_box_169) */
        n++; if (sb_ovl_loc_lock(edge[k], 1, 0) || sb_ovl_loc_lock(edge[k], 0, 1)) { fail++; }
        r = sb_ovl_loc_lock(edge[k], 1, 1);
        n++; if (!r || strcmp(r, "ovl.ll_edge") != 0 || strcmp(sb_ovl_loc_lock_tag(r), "ovl.ll_tag169") != 0) { fail++; }
    }
    n++; if (sb_ovl_loc_lock_tag(NULL) != NULL) { fail++; }
    { /* W440 LOC-LOBBY-MODES: Zielhilfe/Automatik in LoC bedienbar, mit Hinweis "nur Arcade"; SB ohne Hinweis */
        static const char *const ak[] = { "aim_assist", "auto_gear" };
        for (k = 0; k < 2; k++) {
            for (w = 0; w < 2; w++) {
                n++; if (sb_ovl_loc_lock(ak[k], 1, w) || sb_ovl_loc_lock(ak[k], 0, w)) { printf("OVLLOCK FAIL W440 %s gesperrt\n", ak[k]); fail++; }
            }
            r = sb_ovl_loc_hint(ak[k], 1);
            n++; if (!r || strcmp(r, "ovl.ll_arcade") != 0) { printf("OVLLOCK FAIL W440 %s ohne Hinweis\n", ak[k]); fail++; }
            n++; if (sb_ovl_loc_hint(ak[k], 0)) { printf("OVLLOCK FAIL W440 %s Hinweis in SB\n", ak[k]); fail++; }
        }
        n++; if (sb_ovl_loc_hint("slide_assist", 1) || sb_ovl_loc_hint("toggle_assist", 1) || sb_ovl_loc_hint(NULL, 1)) { fail++; }
    }
    printf("OVLLOCK (W416): %d Faelle (LoC gesperrt mit Grund, SB frei, wirksame Zeilen frei, Raender nur bei 16:9): %d Fehler\n",
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
    /* W363 loc_swznorm: swizzled normiert (LoC), linear/RT unveraendert, SB unveraendert */
    n++; if (rq_loc_texel_scale(1, 1, 0, 0, 1, 0, 0, 11.25f)) { fail++; }   /* LoC: Korn-Textur 128^2, UV 11 -> normiert */
    n++; if (!rq_loc_texel_scale(0, 1, 0, 0, 1, 0, 0, 11.25f)) { fail++; }  /* SB: alte Regel (Golden) */
    n++; if (!rq_loc_texel_scale(1, 0, 0, 0, 1, 0, 0, 11.25f)) { fail++; }  /* LoC, Schalter aus: alt */
    n++; if (!rq_loc_texel_scale(1, 1, 0, 1, 0, 0, 0, 640.0f)) { fail++; }  /* LoC linear (LIN_*): Texel */
    n++; if (!rq_loc_texel_scale(1, 1, 1, 0, 1, 1, 0, 512.0f)) { fail++; }  /* RT mit Texel-UV (loc_rtuv) */
    n++; if (rq_loc_texel_scale(1, 1, 1, 0, 1, 0, 0, 1.0f)) { fail++; }     /* RT normiert */
    n++; if (rq_loc_texel_scale(0, 0, 0, 0, 0, 0, 0, 11.25f)) { fail++; }   /* 3D-Draw swizzled: nie */
    n++; if (rq_loc_texel_scale(0, 0, 0, 0, 1, 0, 1, 11.25f)) { fail++; }   /* SB-Monitor-Wash (W121) */
    /* W368 LOC-COCKPIT: Linear-Atlas der Cockpit-Anzeigen, oT0 vom Programm gerechnet -> Texel (NV2A) */
    n++; if (!rq_loc_lin_progscale(1, 1, 0, 1, 0, 0, 1)) { fail++; }  /* LoC, linear, VS-Programm rechnet oT0 */
    n++; if (rq_loc_lin_progscale(0, 1, 0, 1, 0, 0, 1)) { fail++; }   /* SB: nie (Golden) */
    n++; if (rq_loc_lin_progscale(1, 0, 0, 1, 0, 0, 1)) { fail++; }   /* Schalter aus: alt */
    n++; if (rq_loc_lin_progscale(1, 1, 0, 0, 0, 0, 1)) { fail++; }   /* swizzled: immer normiert */
    n++; if (rq_loc_lin_progscale(1, 1, 1, 1, 0, 0, 1)) { fail++; }   /* RT: eigene Regel (loc_rtuv) */
    n++; if (rq_loc_lin_progscale(1, 1, 0, 1, 1, 0, 1)) { fail++; }   /* vortransformiert: tmax-Regel */
    n++; if (rq_loc_lin_progscale(1, 1, 0, 1, 0, 1, 1)) { fail++; }   /* kind 1 (P1): CPU normiert schon */
    n++; if (rq_loc_lin_progscale(1, 1, 0, 1, 0, 0, 0)) { fail++; }   /* mov oT0, vN: tmax-Regel */
    /* Kette wie render_shade.c: Atlas 512x256, tmax 0 (Programm-UV) -> alte Regel nein, neue ja */
    n++; if (rq_loc_texel_scale(1, 1, 0, 1, 0, 0, 0, 0.0f) || !rq_loc_lin_progscale(1, 1, 0, 1, 0, 0, 1)) { fail++; }
    printf("LOCLOOK (W358/W363/W368): %d Faelle (oD0-Quelle, LOD-Bias-Kompensation, Mip-Filter, swizzled normiert, Linear mit Programm-UV): %d Fehler\n", n, fail);
    g_fail += fail;
}

/* ---------- W448 LOC-CHAFF-LOOK: Netze hinter der Kamera im Zwischenbild exakt (render_interp_eye.h) ---------- */
static void eye448_refs(float p[4][4], float z0, float z1, float z2, float z3)
{
    float z[4];
    int q;
    z[0] = z0; z[1] = z1; z[2] = z2; z[3] = z3;
    for (q = 0; q < 4; q++) { p[q][0] = 10.0f * (float)q; p[q][1] = 5.0f; p[q][2] = z[q]; p[q][3] = 1.0f; }
}
static void test_eye448(void)
{
    /* Kamera blickt entlang +z: Clip-w = z (Spalte 3 = (0,0,1,0)), dazu ein Versatz je Tick (Huelse fliegt). */
    static const float M[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 1,  0, 0, 0, 0 };
    float pa[4][4], pb[4][4];
    int fail = 0, n = 0;
    /* (1) alle Bezuege in beiden Ticks hinter der Kamera, Netz n = 66 -> exakt */
    eye448_refs(pa, -556.0f, -540.0f, -530.0f, -521.0f); eye448_refs(pb, -314.0f, -290.0f, -260.0f, -246.0f);
    n++; if (!ip_eye_blind((const float (*)[4])pa, M, 66u, (const float (*)[4])pb, M, 66u)) { fail++; }
    /* (2) ein Bezug im Tick N vor der Kamera (Huelse schneidet die Augenebene) -> ip_plausible entscheidet wie bisher */
    eye448_refs(pb, -50.0f, -20.0f, 5.0f, 15.0f);
    n++; if (ip_eye_blind((const float (*)[4])pa, M, 66u, (const float (*)[4])pb, M, 66u)) { fail++; }
    /* (3) beide Ticks vor der Kamera -> unberuehrt */
    eye448_refs(pa, 517.0f, 520.0f, 523.0f, 537.0f); eye448_refs(pb, 817.0f, 790.0f, 770.0f, 757.0f);
    n++; if (ip_eye_blind((const float (*)[4])pa, M, 66u, (const float (*)[4])pb, M, 66u)) { fail++; }
    /* (4) Partikel-Quad (6 Vertices nach Expansion) hinter der Kamera -> unberuehrt (Vertex-Lerp wie bisher) */
    eye448_refs(pa, -542.0f, -542.0f, -542.0f, -542.0f); eye448_refs(pb, -509.0f, -509.0f, -509.0f, -509.0f);
    n++; if (ip_eye_blind((const float (*)[4])pa, M, 6u, (const float (*)[4])pb, M, 6u)) { fail++; }
    n++; if (ip_eye_blind((const float (*)[4])pa, M, 8u, (const float (*)[4])pb, M, 8u)) { fail++; }
    n++; if (!ip_eye_blind((const float (*)[4])pa, M, 9u, (const float (*)[4])pb, M, 9u)) { fail++; }
    /* (5) andere Vertexzahl je Tick -> unberuehrt */
    n++; if (ip_eye_blind((const float (*)[4])pa, M, 66u, (const float (*)[4])pb, M, 60u)) { fail++; }
    /* (6) w genau 0 (Augenebene) zaehlt nicht als "hinter" */
    eye448_refs(pb, -10.0f, 0.0f, -10.0f, -10.0f);
    n++; if (ip_eye_blind((const float (*)[4])pa, M, 66u, (const float (*)[4])pb, M, 66u)) { fail++; }
    /* (7) Matrix-Wirkung: dieselben Punkte, Kamera gedreht (w = -z) -> vor der Kamera -> unberuehrt */
    { static const float Mr[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, -1, -1,  0, 0, 0, 0 };
      eye448_refs(pa, -556.0f, -540.0f, -530.0f, -521.0f); eye448_refs(pb, -314.0f, -290.0f, -260.0f, -246.0f);
      n++; if (ip_eye_blind((const float (*)[4])pa, Mr, 66u, (const float (*)[4])pb, Mr, 66u)) { fail++; } }
    printf("EYE448 (W448): %d Faelle (Netz hinter der Kamera exakt, Augenebene, vorn, Partikel <= 8 Vertices, Vertexzahl, Matrix): %d Fehler\n", n, fail);
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
    { /* FIX(SB-HUD-SMEAR): Bezug ganzes RT statt Spiel-Viewport. Rechnung + Band einer senkrechten HUD-Strich-Linie
       * in einem Teil-Viewport (Kompassstreifen): RT 16x16 logisch / 64x64 D3D11, Spiel-Viewport 4x2. */
        static const struct { unsigned rw, rh, pw, ph; int on; const char *n; } k[] = {
            {512, 512, 1024, 1024, 1, "Hauptmonitor rt_scale 2"},
            {256, 256, 1024, 1024, 1, "Sub-Monitor rt_scale 4"},
            {512, 512, 512, 512, 0, "rt_scale 1 bitgleich"},
            {256, 256, 320, 320, 0, "Faktor 1,25"},
            {512, 512, 1024, 512, 0, "nur eine Achse skaliert"},
            {0, 512, 1024, 1024, 0, "RT 0"},
        };
        unsigned i, w, h;
        int on, nf = 0, nv = -1, nvold = -1;
        for (i = 0; i < sizeof k / sizeof k[0]; i++) {
            w = h = 0;
            on = sb_line_rt_dims(k[i].rw, k[i].rh, k[i].pw, k[i].ph, &w, &h);
            if (on != k[i].on || (on && (w != k[i].rw || h != k[i].rh))) {
                printf("LSRT FAIL %s: an=%d %ux%u\n", k[i].n, on, w, h);
                nf++;
            }
        }
        w = h = 0;
        if (sb_line_rt_dims(16, 16, 64, 64, &w, &h)) {
            char src[2048];
            ID3DBlob *bv = compile(kLsVsIm, "vs_4_0"), *bp = compile(kLsPsIm, "ps_4_0"), *bg, *bo;
            ID3D11VertexShader *vs = NULL; ID3D11PixelShader *ps = NULL; ID3D11GeometryShader *gs = NULL, *go = NULL;
            ID3D11InputLayout *lay = NULL;
            sb_line_gs_source(src, sizeof src, w, h, 0);
            bg = compile(src, "gs_4_0");
            sb_line_gs_source(src, sizeof src, 4, 2, 0); /* alt (W352): Spiel-Viewport 4x2 */
            bo = compile(src, "gs_4_0");
            if (bv && bp && bg && bo) {
                ID3D11Device_CreateVertexShader(g_d, bv->lpVtbl->GetBufferPointer(bv), bv->lpVtbl->GetBufferSize(bv), NULL, &vs);
                ID3D11Device_CreatePixelShader(g_d, bp->lpVtbl->GetBufferPointer(bp), bp->lpVtbl->GetBufferSize(bp), NULL, &ps);
                ID3D11Device_CreateGeometryShader(g_d, bg->lpVtbl->GetBufferPointer(bg), bg->lpVtbl->GetBufferSize(bg), NULL, &gs);
                ID3D11Device_CreateGeometryShader(g_d, bo->lpVtbl->GetBufferPointer(bo), bo->lpVtbl->GetBufferSize(bo), NULL, &go);
                {
                    D3D11_INPUT_ELEMENT_DESC ie2[2] = {
                        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 } };
                    ID3D11Device_CreateInputLayout(g_d, ie2, 2, bv->lpVtbl->GetBufferPointer(bv), bv->lpVtbl->GetBufferSize(bv), &lay);
                }
                if (vs && ps && gs && go && lay) {
                    nv = ls_count(vs, ps, gs, lay, rtv, rt, stg, 8.5f, 2.0f, 8.5f, 14.0f, 1.0f);    /* 1 x 12 Xbox-Pixel */
                    nvold = ls_count(vs, ps, go, lay, rtv, rt, stg, 8.5f, 2.0f, 8.5f, 14.0f, 1.0f); /* alt: 16/4 = 4 Xbox-Pixel breit */
                }
            }
            if (vs) { vs->lpVtbl->Release(vs); } if (ps) { ps->lpVtbl->Release(ps); }
            if (gs) { gs->lpVtbl->Release(gs); } if (go) { go->lpVtbl->Release(go); } if (lay) { lay->lpVtbl->Release(lay); }
            if (bv) { bv->lpVtbl->Release(bv); } if (bp) { bp->lpVtbl->Release(bp); }
            if (bg) { bg->lpVtbl->Release(bg); } if (bo) { bo->lpVtbl->Release(bo); }
        }
        /* Soll: 4 x 48 = 192 (1 Xbox-Pixel breit); alter Bezug 4x2 -> 16 px breit (~ 768) */
        if (!(nv >= 184 && nv <= 200 && nvold > 400)) { nf++; }
        printf("LSRT %s: %u Faelle, senkrechte Linie im Teil-Viewport band=%d (Soll ~192) alt=%d (~768, zu breit)\n",
               nf ? "FAIL" : "ok", (unsigned)(sizeof k / sizeof k[0]), nv, nvold);
        g_fail += nf;
    }
    rtv->lpVtbl->Release(rtv); rt->lpVtbl->Release(rt); stg->lpVtbl->Release(stg);
    { /* W381 (#59) loc_line_bb: logische Groesse fuer Backbuffer-Linien (reine Rechnung; GS wie oben) */
        static const struct { float vw, vh; unsigned bw, bh, fh; float asp; int on; unsigned w, h; const char *n; } k[] = {
            {3440, 1440, 3440, 1440, 480, 4.0f / 3.0f, 1, 1147, 480, "3440x1440 Hor+"},
            {1920, 1440, 3440, 1440, 480, 4.0f / 3.0f, 1, 640, 480, "4:3-Box in 3440x1440"},
            {3840, 2160, 3840, 2160, 480, 4.0f / 3.0f, 1, 853, 480, "3840x2160 Hor+"},
            {640, 480, 640, 480, 480, 4.0f / 3.0f, 0, 0, 0, "640x480 (Testlaeufe) bitgleich"},
            {800, 600, 800, 600, 480, 4.0f / 3.0f, 0, 0, 0, "800x600 Faktor 1,25"},
            {960, 720, 960, 720, 480, 4.0f / 3.0f, 1, 640, 480, "960x720 Faktor 1,5"},
            {1000, 1500, 1000, 1500, 480, 4.0f / 3.0f, 1, 640, 960, "hohes Fenster (Breite begrenzt)"},
            {64, 64, 64, 64, 16, 1.0f, 1, 16, 16, "64x64/16 = GS-Pruefung oben"},
            {640, 480, 640, 480, 0, 4.0f / 3.0f, 0, 0, 0, "Framebuffer 0"},
        };
        unsigned i, w, h;
        int on, nf = 0;
        printf("== 7b. LoC-Backbuffer-Linien (W381 loc_line_bb)\n");
        for (i = 0; i < sizeof k / sizeof k[0]; i++) {
            w = h = 0;
            on = sb_line_bb_dims(k[i].vw, k[i].vh, k[i].bw, k[i].bh, k[i].fh, k[i].asp, &w, &h);
            if (on != k[i].on || (on && (w != k[i].w || h != k[i].h))) {
                printf("LSBB FAIL %s: an=%d %ux%u (Soll %d %ux%u)\n", k[i].n, on, w, h, k[i].on, k[i].w, k[i].h);
                nf++;
            }
        }
        printf("LSBB %s: %u Faelle\n", nf ? "FAIL" : "ok", (unsigned)(sizeof k / sizeof k[0]));
        g_fail += nf;
    }
}

/* ---------- W394 LOC-HUD-VIEW / LOC-VIEW-PERFRAME (loc_hud_math.h) ---------- */
static void test_lochud(void)
{
    int n = 0, fail = 0, v, k, a, b;
    /* (a) Umrechnung Zielhebel Block -> Satz (belegt C2/C3): Totzone 0x7F/0x80, sonst Stufen */
    n++; if (lh_conv_aim(0x7F) != 0 || lh_conv_aim(0x80) != 0) { fail++; }
    n++; if (lh_conv_aim(0xFF) != 127 || lh_conv_aim(0x00) != -127 || lh_conv_aim(0x81) != 1 || lh_conv_aim(0x7E) != -1) { fail++; }
    { uint8_t f4[4]; lh_axes_to_frame(123, 123, 0, 0, f4); n++; if ((int8_t)f4[0] != -4 || (int8_t)f4[1] != 4 || (int8_t)f4[3] != -1) { fail++; } }
    /* (b) Vorschau-Satz = Satz, den der Poll mit den neuen Achsen liefern wuerde (alle Achsen, Grenzen eingeschlossen) */
    for (a = 0; a < 256; a += 3)
        for (b = 0; b < 256; b += 5) {
            uint8_t base[14], out[14], exp4[4], fin[4];
            int have[4] = {1, 1, 1, 1};
            int sx0 = (a % 255) - 127, sx1 = (b % 255) - 127;
            memset(base, 0x5A, sizeof base);
            fin[0] = (uint8_t)a; fin[1] = (uint8_t)b; fin[2] = (uint8_t)(int8_t)sx0; fin[3] = (uint8_t)(int8_t)sx1;
            lh_axes_to_frame(a, b, sx0, sx1, base + 10);
            lh_frame(base, fin, b, a, sx1, sx0, have, out);
            lh_axes_to_frame(b, a, sx1, sx0, exp4);
            n++; if (memcmp(out + 10, exp4, 4) != 0 || memcmp(out, base, 10) != 0) { fail++; }
        }
    { /* ohne have: Satz unveraendert */
        uint8_t base[14], out[14], fin[4] = {0x90, 0x70, 5, 250};
        int have[4] = {0, 0, 0, 0};
        for (k = 0; k < 14; k++) base[k] = (uint8_t)(k * 17);
        lh_frame(base, fin, 0xFF, 0x00, 127, -127, have, out);
        n++; if (memcmp(base, out, 14) != 0) { fail++; }
    }
    /* (c) Gewicht und Mischung: w 0 -> Basis, 1 -> voller Schritt, Umbruch bei +-pi */
    n++; if (lh_weight(0.0, 49.0) != 0.0 || lh_weight(49.0, 49.0) != 1.0 || lh_weight(80.0, 49.0) != 1.0 || fabs(lh_weight(24.5, 49.0) - 0.5) > 1e-12) { fail++; }
    n++; if (fabs(lh_weight(25.0, 0.0) - 0.5) > 1e-12) { fail++; } /* ohne Schaetzung 50 ms */
    {
        la_state_t p, s1;
        double o[4];
        memset(&p, 0, sizeof p); memset(&s1, 0, sizeof s1);
        p.ys = 3.10; s1.ys = -3.10; p.ax = 0.2; s1.ax = 0.3; p.ps = -0.1; s1.ps = 0.1; p.ay = 0.0; s1.ay = -0.04;
        lh_blend(&p, &s1, 0.0, o); n++; if (fabs(o[0] - 3.10) > 1e-12 || fabs(o[2] - 0.2) > 1e-12) { fail++; }
        lh_blend(&p, &s1, 1.0, o); n++; if (fabs(o[0] + 3.10) > 1e-9 || fabs(o[2] - 0.3) > 1e-12 || fabs(o[3] + 0.04) > 1e-12) { fail++; }
        lh_blend(&p, &s1, 0.5, o); n++; if (fabs(fabs(o[0]) - LA_PI) > 1e-9 || fabs(o[1]) > 1e-12) { fail++; } /* ueber +-pi, nicht durch 0 */
    }
    /* (d) Stetigkeit am Uebergang: naechste Vorhersage = voller Schritt (gleicher Satz) -> Anzeige springt nicht */
    {
        uint32_t r = 7u;
        for (v = 0; v < 2000; v++) {
            la_state_t p, s1, p2;
            uint8_t in[14];
            double o1[4], o2[4];
            memset(&p, 0, sizeof p);
            r = r * 1664525u + 1013904223u;
            p.fa0 = 0x4000u; p.zoom = (double)(r % 56u); p.arate = 0.05;
            p.ys = ((int)(r >> 8) % 1000) * 0.001; p.yt = p.ys + 0.1; p.ax = 0.1; p.ay = -0.05;
            memset(in, 0, sizeof in);
            r = r * 1664525u + 1013904223u;
            in[10] = (uint8_t)(r >> 8); in[11] = (uint8_t)(r >> 16); in[12] = (uint8_t)(r >> 20); in[13] = (uint8_t)(r >> 24);
            s1 = p;
            if (!la_step(&s1, in)) { continue; }
            p2 = s1; /* Simulation hat den Satz uebernommen */
            lh_blend(&p, &s1, 1.0, o1);   /* gehalten bis zur Vorhersage */
            lh_blend(&p2, &p2, 0.0, o2);  /* neue Basis, w = 0 */
            n++; if (fabs(la_wrap(o1[0] - o2[0])) + fabs(la_wrap(o1[1] - o2[1])) + fabs(o1[2] - o2[2]) + fabs(o1[3] - o2[3]) > 1e-12) { fail++; }
        }
    }
    /* (e) Marken-Verschiebung: ohne Korrektur c/w - a, hinter der Kamera keine, Grenze +-0,5 */
    {
        double c[4] = {0.3, -0.2, 0.5, 2.0}, dx, dy;
        float C[16];
        n++; if (!lh_mark_shift(c, NULL, 0.1, -0.05, &dx, &dy) || fabs(dx - 0.05) > 1e-12 || fabs(dy + 0.05) > 1e-12) { fail++; }
        c[3] = -1.0; n++; if (lh_mark_shift(c, NULL, 0.0, 0.0, &dx, &dy)) { fail++; }
        c[3] = 0.5; c[0] = 2.0; n++; if (!lh_mark_shift(c, NULL, 0.0, 0.0, &dx, &dy) || dx != 0.5) { fail++; }
        /* mit Kamera-Korrektur (W376 la_cam_corr): Marke folgt einem Weltpunkt unter der korrigierten Kamera */
        {
            static const double up[3] = {0.0, 1.0, 0.0};
            double VP[16], Cd[16], q[3] = {200.0, 50.0, 3000.0}, cq[4], bq[4];
            memset(VP, 0, sizeof VP);
            VP[0] = 1.3; VP[5] = 1.73; VP[10] = 1.0; VP[11] = 1.0; VP[14] = -10.0; /* Blick +z, w = z */
            if (la_cam_corr(Cd, VP, up, 0.05, -0.02)) {
                for (k = 0; k < 16; k++) { C[k] = (float)Cd[k]; }
                for (k = 0; k < 4; k++) { cq[k] = q[0] * VP[k] + q[1] * VP[4 + k] + q[2] * VP[8 + k] + VP[12 + k]; }
                for (k = 0; k < 4; k++) { bq[k] = cq[0] * Cd[k] + cq[1] * Cd[4 + k] + cq[2] * Cd[8 + k] + cq[3] * Cd[12 + k]; }
                n++; if (!lh_mark_shift(cq, C, cq[0] / cq[3], cq[1] / cq[3], &dx, &dy) ||
                         fabs(dx - (bq[0] / bq[3] - cq[0] / cq[3])) > 1e-5 || fabs(dy - (bq[1] / bq[3] - cq[1] / cq[3])) > 1e-5 ||
                         fabs(dx) < 1e-3) { fail++; }
            } else { n++; fail++; }
        }
    }
    printf("LOCHUD (W394): %d Faelle (Umrechnung Block/Satz, Vorschau-Satz, Gewicht/Mischung, Stetigkeit am Uebergang, Marken-Verschiebung): %d Fehler\n", n, fail);
    g_fail += fail;
}

/* ---------- W439 LOC-RETICLE-JUMP: Fadenkreuz im Zwischenbild stetig (gelerpte und exakte Draws am Soll) ----------
 * Simulierte Mitte springt je Tick um bis zu 0,3 NDC (schnelles Zielen), Soll = Sicht je Bild. Gezeichnet: gelerpter Draw
 * lerp(a, b, t) + r, exakter Draw b + Verschiebung. Mit lh_ret_exact_shift liegen beide am Soll (Fehler < 1e-6), mit der
 * alten Verschiebung r lag der exakte Draw um (1 - t) * (b - a) daneben (Gegenprobe). */
static void test_ret439(void)
{
    int n = 0, fail = 0, i, k, old_bad = 0;
    uint32_t rnd = 439u;
    for (i = 0; i < 400; i++) {
        float a[2], b[2];
        double e[2], s[2], r[2], t, gl[2], gx[2], go[2];
        rnd = rnd * 1664525u + 1013904223u; a[0] = (float)(((rnd >> 8) % 2000u) / 1000.0 - 1.0);
        rnd = rnd * 1664525u + 1013904223u; a[1] = (float)(((rnd >> 8) % 2000u) / 1000.0 - 1.0);
        rnd = rnd * 1664525u + 1013904223u; b[0] = a[0] + (float)(((rnd >> 8) % 600u) / 1000.0 - 0.3);
        rnd = rnd * 1664525u + 1013904223u; b[1] = a[1] + (float)(((rnd >> 8) % 600u) / 1000.0 - 0.3);
        for (k = 1; k < 8; k++) {
            t = k / 8.0;
            rnd = rnd * 1664525u + 1013904223u;
            s[0] = b[0] + ((rnd >> 8) % 200u) / 1000.0 - 0.1;
            s[1] = b[1] - ((rnd >> 16) % 200u) / 1000.0 + 0.1;
            r[0] = s[0] - (a[0] + (b[0] - a[0]) * t);
            r[1] = s[1] - (a[1] + (b[1] - a[1]) * t);
            lh_ret_exact_shift(r[0], r[1], a, b, t, 1, e);
            gl[0] = a[0] + (b[0] - a[0]) * t + r[0]; gl[1] = a[1] + (b[1] - a[1]) * t + r[1]; /* gelerpt */
            gx[0] = b[0] + e[0]; gx[1] = b[1] + e[1];                                     /* exakt, neu */
            go[0] = b[0] + r[0]; go[1] = b[1] + r[1];                                     /* exakt, alt */
            n++;
            if (fabs(gl[0] - s[0]) > 1e-6 || fabs(gl[1] - s[1]) > 1e-6 || fabs(gx[0] - s[0]) > 1e-6 ||
                fabs(gx[1] - s[1]) > 1e-6) { fail++; }
            if (fabs(go[0] - s[0]) > 0.02 || fabs(go[1] - s[1]) > 0.02) { old_bad++; }
        }
        lh_ret_exact_shift(0.1, -0.2, a, b, 0.5, 0, e); /* ohne Zwischenbild: unveraendert r */
        n++; if (e[0] != 0.1 || e[1] != -0.2) { fail++; }
    }
    n++; if (old_bad < 1000) { fail++; } /* Gegenprobe: alte Verschiebung lag fuer exakte Draws daneben */
    printf("RET439 (W439): %d Faelle (Fadenkreuz gelerpt/exakt am Soll, ohne Zwischenbild unveraendert, Gegenprobe alt %d daneben): %d Fehler\n",
           n, old_bad, fail);
    g_fail += fail;
}

/* ---------- W444 LOC-RETICLE-MARK: Fadenkreuz-Teile robust erkennen (loc_ret_mark.h) ----------
 * Synthetische Listen wie im Lauf A0 gemessen: Ring (40 Vertices) folgt der Formel-Mitte, Kreuz (8, 4 x 4, 24) bewegt sich
 * eigenstaendig (bis 0,34 NDC daneben); dazu fremde Draws: gleiche Gruppe 8 Vertices andere Form (mal im Kasten, mal mit
 * falschem Zuordnungs-Partner), gleiche Gruppe 4 Vertices gleicher Form ruhend daneben, andere Gruppe gleicher Ring.
 * Soll: ab der zweiten Liste immer 7 von 7, nie ein fremder; Einblend-Animation (Formwechsel) und Gedaechtnis-Fall;
 * Gegenprobe alte Pruefung (nur Start-Regel) verliert Teile. */
typedef struct { unsigned nv; float ox, oy; float s[3][2]; int cross; } rm_tpl_t;
static const rm_tpl_t rm_tpl[7] = {
    {40, -0.123f, 0.064f, {{0.123f, -0.158f}, {0.246f, -0.045f}, {0.029f, 0.059f}}, 0},
    {8, 0.0f, 0.078f, {{-0.039f, -0.078f}, {0.0f, -0.117f}, {0.078f, -0.078f}}, 1},
    {4, 0.086f, 0.0f, {{0.035f, 0.020f}, {0.035f, -0.020f}, {0.0f, 0.0f}}, 1},
    {4, -0.086f, 0.0f, {{-0.035f, -0.020f}, {-0.035f, 0.020f}, {0.0f, 0.0f}}, 1},
    {24, -0.059f, 0.039f, {{0.117f, 0.020f}, {0.117f, -0.098f}, {0.059f, -0.042f}}, 1},
    {4, -0.086f, 0.0f, {{-0.035f, -0.020f}, {-0.035f, 0.020f}, {0.0f, 0.0f}}, 1},
    {4, 0.086f, 0.0f, {{0.035f, 0.020f}, {0.035f, -0.020f}, {0.0f, 0.0f}}, 1},
};
static void rm_part(lrm_part_t *p, unsigned nv, unsigned grp, float x, float y, const float s[3][2])
{
    int k;
    p->nv = nv; p->grp = grp; p->p[0][0] = x; p->p[0][1] = y;
    for (k = 0; k < 3; k++) { p->p[k + 1][0] = x + s[k][0]; p->p[k + 1][1] = y + s[k][1]; }
}
static void test_retmark444(void)
{
    enum { G = 0xdb10b36fu, T = 1200 };
    static const float fs[3][2] = {{-0.028f, -0.083f}, {-0.017f, -0.024f}, {-0.017f, -0.024f}}; /* fremd 8 (gemessen) */
    int n = 0, fail = 0, old_lost = 0, fremd_marked = 0, lost_new = 0, tick, mode, nform = 0, npair = 0, nseed = 0;
    uint32_t rnd = 444u;
    for (mode = 0; mode < 3; mode++) { /* 0 normal, 1 Einblend-Animation der 4er-Teile, 2 Vorliste ohne Fadenkreuz (Gedaechtnis) */
        lrm_part_t prev[LRM_MAXP];
        lrm_mem_t mem[LRM_MAXP];
        int np = 0, nmem = 0, prev_src[LRM_MAXP];
        float rc[2] = {0.0f, 0.0f}, rca[2] = {0.0f, 0.0f}, cx = 0.0f, cy = 0.0f, vx = 0.0f, vy = 0.0f, fx = 0.3f, fy = 0.02f;
        float last[11][2];
        int have_last = 0, q0;
        for (q0 = 0; q0 < 11; q0++) { last[q0][0] = last[q0][1] = 9.0f; }
        for (tick = 0; tick < T; tick++) {
            lrm_cand_t c[LRM_MAXC];
            lrm_part_t mk[LRM_MAXP];
            unsigned char why[LRM_MAXC];
            int src[LRM_MAXC], nc = 0, ord[11], i, j, moved, nmk = 0, nfr = 0, nmk_old = 0, nm = 0, gap;
            float anim = (mode == 1 && tick >= 300 && tick < 306) ? (float)(306 - tick) / 6.0f : 0.0f;
            gap = mode == 2 && tick % 25 == 0; /* Vorliste ohne Fadenkreuz: nur Gedaechtnis */
            rca[0] = rc[0]; rca[1] = rc[1];
            rnd = rnd * 1664525u + 1013904223u;
            if (tick > 2) { /* Formel-Mitte: Zielen bis 0,3 NDC je Tick (Phasen), Kreuz eigenstaendig bis 0,34 daneben */
                int ph = (tick / 40) % 4;
                float st = ph == 0 ? 0.0f : (ph == 1 ? 0.05f : (ph == 2 ? 0.3f : 0.02f));
                rc[0] += st * ((float)((rnd >> 8) % 2001u) / 1000.0f - 1.0f);
                rc[1] += 0.5f * st * ((float)((rnd >> 18) % 2001u) / 1000.0f - 1.0f);
                if (rc[0] > 0.6f) { rc[0] = 0.6f; } if (rc[0] < -0.6f) { rc[0] = -0.6f; }
                if (rc[1] > 0.5f) { rc[1] = 0.5f; } if (rc[1] < -0.5f) { rc[1] = -0.5f; }
                vx += 0.01f * ((float)((rnd >> 4) % 201u) / 100.0f - 1.0f) - 0.05f * cx;
                vy += 0.004f * ((float)((rnd >> 12) % 201u) / 100.0f - 1.0f) - 0.05f * cy;
                if (vx > 0.06f) { vx = 0.06f; } if (vx < -0.06f) { vx = -0.06f; }
                cx += vx; cy += vy;
                if (cx > 0.34f) { cx = 0.34f; } if (cx < -0.34f) { cx = -0.34f; }
                if (cy > 0.15f) { cy = 0.15f; } if (cy < -0.15f) { cy = -0.15f; }
                if (ph == 0) { cx *= 0.5f; cy *= 0.5f; vx = vy = 0.0f; } /* Ruhe: Kreuz kehrt zur Mitte zurueck */
            }
            moved = fabs(rc[0] - rca[0]) + fabs(rc[1] - rca[1]) > 0.01;
            for (i = 0; i < 11; i++) { ord[i] = i; }
            for (i = 10; i > 0; i--) { /* Draw-Reihenfolge mischen (fremde auch zwischen Ring und Kreuz) */
                int t2;
                rnd = rnd * 1664525u + 1013904223u;
                j = (int)((rnd >> 8) % (unsigned)(i + 1)); t2 = ord[i]; ord[i] = ord[j]; ord[j] = t2;
            }
            if (gap) { np = 0; }
            np = lrm_prev_add_mem(prev, prev_src, np, mem, nmem, (unsigned)tick);
            for (i = 0; i < 11; i++) {
                int q = ord[i];
                lrm_cand_t *e = &c[nc];
                if (q >= 7 && tick < 5) { continue; } /* fremde erst nach dem Start (Start = alte Regel, wie bisher) */
                float x, y, s[3][2], ex, ey;
                e->prev = -1;
                if (q < 7) {
                    const rm_tpl_t *t = &rm_tpl[q];
                    memcpy(s, t->s, sizeof s);
                    if (anim > 0.0f && t->nv == 4u) { s[0][0] *= 1.0f - anim; s[1][1] += 0.03f * anim; } /* Formwechsel */
                    x = rc[0] + t->ox + (t->cross ? cx : 0.0f);
                    y = rc[1] + t->oy + (t->cross ? cy : 0.01f * (float)(tick % 7));
                    if (anim > 0.0f && t->nv == 4u) { x -= 0.05f * anim * (t->ox > 0 ? 1.0f : -1.0f); }
                    rm_part(&e->d, t->nv, G, x, y, s);
                } else if (q == 7) { /* fremd: gleiche Gruppe, 8 Vertices, andere Form, ruhend; mal im Kasten */
                    if (tick % 50 < 10) { fx = rc[0] + 0.05f; fy = rc[1] + 0.03f; }
                    rm_part(&e->d, 8u, G, fx, fy, fs);
                } else if (q == 8) { /* fremd: gleiche Gruppe, Form wie ein 4er-Teil, ruhend im Fenster */
                    rm_part(&e->d, 4u, G, 0.35f, -0.3f, rm_tpl[2].s);
                } else if (q == 9) { /* fremd: andere Gruppe, Ring-Form, in der Mitte */
                    rm_part(&e->d, 40u, G ^ 1u, rc[0] - 0.1f, rc[1] + 0.06f, rm_tpl[0].s);
                } else { /* fremd: gleiche Gruppe 24 Vertices, andere Form, ausserhalb des Fensters */
                    rm_part(&e->d, 24u, G, rc[0] + 0.9f, rc[1], fs);
                }
                src[nc] = q;
                /* Start-Regel wie render_interp: Kasten +-0,16/0,12; bewegt: wie die Formel +-0,01 (Partner = gleicher Draw) */
                ex = e->d.p[0][0] - rc[0]; ey = e->d.p[0][1] - rc[1];
                e->seed = (unsigned char)(fabs(ex) <= 0.16f && fabs(ey) <= 0.12f);
                if (e->seed && moved && have_last) {
                    float mx = (e->d.p[0][0] - last[q][0]) - (rc[0] - rca[0]);
                    float my = (e->d.p[0][1] - last[q][1]) - (rc[1] - rca[1]);
                    if (fabs(mx) > 0.01f || fabs(my) > 0.01f) { e->seed = 0; }
                }
                for (j = 0; j < np; j++) { /* simulierte Zuordnung: gleiches Teil; fremde 8 alle 3 Ticks mit dem Kreuz-8er gepaart */
                    if (prev_src[j] >= 0 && (prev_src[j] == q || (q == 7 && tick % 3 == 0 && prev_src[j] == 1))) { e->prev = j; break; }
                }
                last[q][0] = e->d.p[0][0]; last[q][1] = e->d.p[0][1];
                nc++;
            }
            have_last = 1;
            lrm_mark(prev, np, c, nc, rc, why);
            np = 0;
            for (i = 0; i < nc; i++) {
                if (src[i] < 7 && c[i].seed) { nmk_old++; }
                if (!why[i]) { continue; }
                if (src[i] < 7) { nmk++; } else { nfr++; }
                if (why[i] == LRM_FORM) { nform++; } else if (why[i] == LRM_PAIR) { npair++; } else { nseed++; }
                if (np < LRM_MAXP) { prev[np] = c[i].d; prev_src[np] = src[i]; np++; }
                if (nm < LRM_MAXP) { mk[nm++] = c[i].d; }
            }
            nmem = lrm_mem_update(mem, nmem, mk, nm, (unsigned)tick);
            if (tick >= 1) {
                n++;
                if (nmk != 7) { lost_new++; fail++; }
                if (nfr) { fremd_marked += nfr; fail++; }
                if (nmk_old < 7) { old_lost++; }
            }
        }
    }
    { /* Einzelfaelle: Form-Gleichheit, Gruppen-/Vertexzahl-Schutz; ohne Vorlage nur Start-Regel */
        lrm_part_t a, b;
        lrm_cand_t c2[3];
        unsigned char w2[3];
        float rc0[2] = {0.0f, 0.0f};
        rm_part(&a, 8u, G, 0.1f, 0.2f, rm_tpl[1].s);
        rm_part(&b, 8u, G, -0.3f, 0.4f, rm_tpl[1].s);
        n++; if (!lrm_same_shape(&a, &b)) { fail++; }
        b.grp ^= 2u; n++; if (lrm_same_shape(&a, &b)) { fail++; }
        b.grp = G; b.nv = 4u; n++; if (lrm_same_shape(&a, &b)) { fail++; }
        rm_part(&b, 8u, G, 0.1f, 0.2f, fs); n++; if (lrm_same_shape(&a, &b)) { fail++; }
        rm_part(&c2[0].d, 8u, G, 0.0f, 0.08f, rm_tpl[1].s); c2[0].seed = 1; c2[0].prev = -1;
        rm_part(&c2[1].d, 4u, G, 0.3f, 0.0f, rm_tpl[2].s); c2[1].seed = 0; c2[1].prev = -1;
        rm_part(&c2[2].d, 40u, G, -0.12f, 0.06f, rm_tpl[0].s); c2[2].seed = 1; c2[2].prev = -1;
        n++; if (lrm_mark(NULL, 0, c2, 3, rc0, w2) != 2 || w2[0] != LRM_SEED || w2[1] || w2[2] != LRM_SEED) { fail++; }
        c2[2].d.grp = G ^ 4u; /* mit Vorlage: Start-Kandidat anderer Gruppe = fremd */
        n++; if (lrm_mark(&a, 1, c2, 3, rc0, w2) != 1 || w2[0] != LRM_FORM || w2[2]) { fail++; }
    }
    n++; if (old_lost < 100) { fail++; } /* Gegenprobe: alte Pruefung verliert beim Drehen Teile */
    n++; if (!npair || !nseed || !nform) { fail++; } /* alle drei Regeln kamen vor */
    printf("RETMARK444 (W444): %d Faelle (7 von 7 ab der 2. Liste, Einblend-Animation, Gedaechtnis-Fall, fremde Draws nie markiert; "
           "Teile Form %d Partner %d Start %d; neu fehlend %d, fremd markiert %d; Gegenprobe alt: %d Listen mit fehlenden Teilen): %d Fehler\n",
           n, nform, npair, nseed, lost_new, fremd_marked, old_lost, fail);
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
    test_texzero();
    test_bbcam();
    test_camfit();
    test_locaim();
    test_locaa(); /* W442 */
    test_sky435(); /* W435 */
    test_locsmooth(); /* W385 */
    test_locsmoothopp(); /* W387 */
    test_locownpred(); /* W390 */
    test_pacing(); /* W393 */
    test_lochud(); /* W394 */
    test_ret439(); /* W439 */
    test_retmark444(); /* W444 */
    test_loclook();
    test_eye448(); /* W448 */
    test_linescale();
    test_shref();
    test_rqexp();
    test_texbudget();
    test_locmenu();
    test_ovlloclock(); /* W416 */
    printf("== ERGEBNIS: %d Fehler\n", g_fail);
    return g_fail ? 1 : 0;
}
