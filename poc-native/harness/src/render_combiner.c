/* render_combiner.c - RENDER-QA (W77): Pixel-Pipeline nach NV2A-Semantik.
 * Teil 1 (dieses Stueck): Zensus-DIAG [render] rq_diag=1 -
 *  - RQPS: Pixelshader-Definition (D3DPIXELSHADERDEF, 0xF0 Byte) je Handle,
 *  - RQVS: Vertex-Deklaration (16 Slots stream/off/fmt) je VS-Handle,
 *  - RQFOG: Nebel-Zustand (Deferred-RenderStates 0x21AC20.., FogColor) bei
 *    Aenderung, RQTSS: Textur-Stufen-Zustand (0x21A8B0) bei Aenderung,
 *  - RQCOMBO: Draw-Zaehlung je (Szene, VS, PS, Ziel, RHW, Stufen-Belegung),
 *    Tabelle alle rq_every Frames.
 * Teil 2: Shading-Pfad [render] shading=1 (s. Abschnitt "Shading-Pfad"). */
#include "render_int.h"
#include <d3d11_1.h>
#include "render_qa.h"
#include "render_vsh.h"
#include "render_psgen.h"

/* XDK-D3D8 (analyse/functions_report.txt): Deferred-RenderStates ab
 * D3DRS_FogEnable = 0x21AC20 (FOGENABLE, FOGTABLEMODE, FOGSTART, FOGEND,
 * FOGDENSITY, RANGEFOGENABLE, WRAP0..3, ...), Deferred-TextureStates
 * 0x21A8B0 (4 Stufen x 32 DWORDs: ADDRESSU/V/W, MAG/MIN/MIPFILTER, ...,
 * COLOROP=12 ... TEXCOORDINDEX=28). */
#define RQ_DRS_BASE 0x21AC20u
#define RQ_DTS_BASE 0x21A8B0u

unsigned g_rq_fogcolor;      /* SetRenderState_FogColor (ARGB) */
unsigned g_rq_fogcolor_set;  /* 1 = vom Spiel gesetzt */
unsigned g_rq_psconst[8];    /* SetPixelShaderConstant: c0..c7 als ARGB (NV2A 8 Bit) */
unsigned g_rq_psconst_set;   /* Bitmaske gesetzter Register */
unsigned g_rq_psconst_calls;
unsigned g_rq_pbc_calls[64], g_rq_pbc_last[64]; /* SHADE-PERF (W80): Simple-Pfad-Combinerkonstanten */
unsigned g_rq_rs[64];             /* SetRenderStateNotInline: PS-States 0..56 (Spiegel) */
unsigned long long g_rq_rs_set;   /* Bit n = State n seit SetPixelShader gesetzt */
unsigned g_rq_rs_calls;

static unsigned rq_u32(unsigned a)
{
    unsigned v = 0;
    __try { v = *(volatile unsigned *)(uintptr_t)a; }
    __except (EXCEPTION_EXECUTE_HANDLER) { v = 0; }
    return v;
}

/* ---------- Zensus ---------- */
#define RQ_COMBO_MAX 256
typedef struct {
    unsigned scene, vs, vshash, ps, tgt, rhw, st, cnt, verts;
} rq_combo_t;
static rq_combo_t g_rqc[RQ_COMBO_MAX];
static unsigned g_rqc_n, g_rqc_drop;
static unsigned g_rq_seen_ps[128], g_rq_nps;
static unsigned g_rq_seen_vs[128], g_rq_nvs;
static unsigned g_rq_fog_last[6], g_rq_fog_have, g_rq_fogc_last;
static unsigned g_rq_tss_last[4][8];

static void rq_dump_ps(unsigned ps)
{
    unsigned i;
    for (i = 0; i < g_rq_nps; i++) {
        if (g_rq_seen_ps[i] == ps) { return; }
    }
    if (g_rq_nps >= 128u) { return; }
    g_rq_seen_ps[g_rq_nps++] = ps;
    __try {
        unsigned base = ps & ~1u;
        unsigned pdef = *(unsigned *)(uintptr_t)(base + 8u);
        if (pdef >= 0x10000u && pdef < 0x80000000u) {
            const unsigned *d = (const unsigned *)(uintptr_t)pdef;
            unsigned r;
            for (r = 0; r < 60u; r += 12u) {
                HLOG_W("RQPS ps=0x%X def=0x%X [%02u]: %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X",
                       ps, pdef, r, d[r], d[r + 1], d[r + 2], d[r + 3], d[r + 4], d[r + 5],
                       d[r + 6], d[r + 7], d[r + 8], d[r + 9], d[r + 10], d[r + 11]);
            }
        } else {
            HLOG_W("RQPS ps=0x%X: kein pDef (0x%X)", ps, pdef);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        HLOG_W("RQPS ps=0x%X: nicht lesbar", ps);
    }
}

static void rq_dump_vs(unsigned vs)
{
    unsigned i;
    for (i = 0; i < g_rq_nvs; i++) {
        if (g_rq_seen_vs[i] == vs) { return; }
    }
    if (g_rq_nvs >= 128u) { return; }
    g_rq_seen_vs[g_rq_nvs++] = vs;
    if (vs < 0x10000u) { HLOG_W("RQVS vs=0x%X: FVF", vs); return; }
    __try {
        unsigned base = vs & ~1u, s;
        char buf[512];
        int off = 0;
        buf[0] = 0;
        for (s = 0; s < 16u; s++) {
            const unsigned *sl = (const unsigned *)(uintptr_t)(base + 0x14u + s * 16u);
            if ((sl[2] & 0xFFu) == 0x02u) { continue; }
            off += _snprintf_s(buf + off, sizeof buf - (size_t)off, _TRUNCATE, " v%u=s%u@%u/%02X",
                               s, sl[0], sl[1], sl[2] & 0xFFu);
            if (off < 0) { break; }
        }
        HLOG_W("RQVS vs=0x%X hash=%08X:%s", vs, vsh_program(vs) ? vsh_program(vs)->hash : 0u, buf);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        HLOG_W("RQVS vs=0x%X: nicht lesbar", vs);
    }
}

static void rq_state_check(void)
{
    unsigned i, s, v[6], changed = 0;
    for (i = 0; i < 6u; i++) {
        v[i] = rq_u32(RQ_DRS_BASE + i * 4u);
        if (!g_rq_fog_have || v[i] != g_rq_fog_last[i]) { changed = 1; }
    }
    if (g_rq_fogcolor != g_rq_fogc_last) { changed = 1; }
    if (changed) {
        union { unsigned u; float f; } st, en, de;
        static unsigned nfog;
        st.u = v[2]; en.u = v[3]; de.u = v[4];
        if (nfog < 200u) {
            nfog++;
            HLOG_W("RQFOG f=%u en=%u tbl=%u start=%.3f end=%.3f dens=%.5f range=%u col=0x%08X(set=%u) scene=0x%X",
                   g_present_no, v[0], v[1], st.f, en.f, de.f, v[5], g_rq_fogcolor,
                   g_rq_fogcolor_set, rq_u32(M3D_SCENE_ADDR));
        }
        memcpy(g_rq_fog_last, v, sizeof v);
        g_rq_fogc_last = g_rq_fogcolor;
        g_rq_fog_have = 1;
    }
    for (s = 0; s < 4u; s++) {
        unsigned t[8];
        /* ADDRESSU, ADDRESSV, ADDRESSW, MAG, MIN, MIP, COLORKEYOP(9), TEXCOORDINDEX(28) */
        static const unsigned idx[8] = { 0, 1, 2, 3, 4, 5, 9, 28 };
        int ch = 0;
        for (i = 0; i < 8u; i++) {
            t[i] = rq_u32(RQ_DTS_BASE + (s * 32u + idx[i]) * 4u);
            if (t[i] != g_rq_tss_last[s][i]) { ch = 1; }
        }
        if (ch) {
            static unsigned ntss;
            if (ntss < 300u) {
                ntss++;
                HLOG_W("RQTSS f=%u st=%u addr=%u/%u/%u filt=%u/%u/%u ckey=%u tci=0x%X",
                       g_present_no, s, t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7]);
            }
            memcpy(g_rq_tss_last[s], t, sizeof t);
        }
    }
}

void rq_census_draw(int rhw, unsigned nverts)
{
    unsigned scene, st, xp[4], n[4], i, h = 0;
    unsigned tgt = rtt_active() ? (unsigned)(g_rtt_bound + 1) : 0u;
    const vsh_prog_t *vp;
    if (!g_cfg.rq_diag) { return; }
    rq_state_check();
    if (g_im_pshader) { rq_dump_ps(g_im_pshader); }
    if (g_im_shader) { rq_dump_vs(g_im_shader); }
    scene = rq_u32(M3D_SCENE_ADDR);
    sb_tex_stages(xp, n);
    st = 0;
    for (i = 0; i < 4u; i++) { if (xp[i]) { st |= 1u << i; } }
    vp = g_im_shader >= 0x10000u ? vsh_program(g_im_shader) : NULL;
    if (vp) { h = vp->hash; }
    for (i = 0; i < g_rqc_n; i++) {
        rq_combo_t *c = &g_rqc[i];
        if (c->scene == scene && c->vshash == h && c->vs == g_im_shader && c->ps == g_im_pshader &&
            c->tgt == tgt && c->rhw == (unsigned)rhw && c->st == st) {
            c->cnt++; c->verts += nverts;
            return;
        }
    }
    if (g_rqc_n >= RQ_COMBO_MAX) { g_rqc_drop++; return; }
    g_rqc[g_rqc_n].scene = scene; g_rqc[g_rqc_n].vs = g_im_shader; g_rqc[g_rqc_n].vshash = h;
    g_rqc[g_rqc_n].ps = g_im_pshader; g_rqc[g_rqc_n].tgt = tgt; g_rqc[g_rqc_n].rhw = (unsigned)rhw;
    g_rqc[g_rqc_n].st = st; g_rqc[g_rqc_n].cnt = 1; g_rqc[g_rqc_n].verts = nverts;
    g_rqc_n++;
}

static void rq_psct_log(unsigned f); /* DIAG(PSCONST,W84) */
void rq_census_present(unsigned f)
{
    unsigned i, every;
    if (!g_cfg.rq_diag) { return; }
    every = g_cfg.rq_every > 0 ? (unsigned)g_cfg.rq_every : 3000u;
    if ((f % every) == 0u) { rq_stat_log(f); }
    if ((f % every) != 0u || g_rqc_n == 0) { return; }
    HLOG_W("RQCOMBO f=%u: %u Kombinationen (verworfen %u), pscalls=%u psset=0x%X",
           f, g_rqc_n, g_rqc_drop, g_rq_psconst_calls, g_rq_psconst_set);
    rq_psct_log(f); /* DIAG(PSCONST,W84) */
    for (i = 0; i < g_rqc_n; i++) {
        const rq_combo_t *c = &g_rqc[i];
        HLOG_W("RQCOMBO f=%u scene=0x%X vs=0x%X h=%08X ps=0x%X tgt=%u rhw=%u st=0x%X draws=%u verts=%u",
               f, c->scene, c->vs, c->vshash, c->ps, c->tgt, c->rhw, c->st, c->cnt, c->verts);
    }
    g_rqc_n = 0; g_rqc_drop = 0;
}

/* ---------- Hooks (nur bei rq_diag/shading installiert) ---------- */
/* D3DDevice_SetPixelShaderConstant (0x210220, stdcall Register, pData, Count):
 * Original klemmt jede Komponente auf [0,1], quantisiert auf 8 Bit (x*255+0.5)
 * und speichert ARGB (A = w); verteilt danach per PSC0/PSC1/FinalConst-
 * Mapping der aktiven PS-Definition auf die Combiner-Konstanten. Hier: nur
 * den Registerwert merken (Verteilung beim Draw nach derselben Regel). */
static unsigned rq_q8(float x)
{
    if (!(x > 0.0f)) { return 0u; }
    if (x >= 1.0f) { return 255u; }
    return (unsigned)(x * 255.0f + 0.5f);
}

/* DIAG(PSCONST,W84, rq_diag): je (PS, Register) Aufrufe + letzter Wert, Zeile RQPSCT */
typedef struct { unsigned ps, reg, n, last; } rq_psct_t;
static rq_psct_t g_rq_psct[32];
static unsigned g_rq_npsct;
static void rq_psct_note(unsigned reg, unsigned pdata)
{
    unsigned i, v = 0;
    __try {
        const float *f = (const float *)(uintptr_t)pdata;
        v = (rq_q8(f[3]) << 24) | (rq_q8(f[0]) << 16) | (rq_q8(f[1]) << 8) | rq_q8(f[2]);
    } __except (EXCEPTION_EXECUTE_HANDLER) { v = 0xDEADu; }
    for (i = 0; i < g_rq_npsct; i++) {
        if (g_rq_psct[i].ps == g_im_pshader && g_rq_psct[i].reg == reg) { g_rq_psct[i].n++; g_rq_psct[i].last = v; return; }
    }
    if (g_rq_npsct < 32u) {
        g_rq_psct[g_rq_npsct].ps = g_im_pshader; g_rq_psct[g_rq_npsct].reg = reg;
        g_rq_psct[g_rq_npsct].n = 1; g_rq_psct[g_rq_npsct].last = v; g_rq_npsct++;
    }
}
static void rq_psct_log(unsigned f)
{
    unsigned i;
    for (i = 0; i < g_rq_npsct; i++) {
        HLOG_W("RQPSCT f=%u ps=0x%X reg=%u n=%u last=0x%08X", f, g_rq_psct[i].ps, g_rq_psct[i].reg, g_rq_psct[i].n, g_rq_psct[i].last);
    }
    g_rq_npsct = 0;
}

unsigned g_psc_ra; /* DIAG(SHADE-M2,W117): Aufrufer (hook_D3D_PSConst) */
unsigned __stdcall hook_D3DDevice_SetPixelShaderConstant(unsigned reg, unsigned pdata, unsigned count)
{
    unsigned k;
    g_rq_psconst_calls++;
    if (g_cfg.rq_diag) { for (k = 0; k < count && k < 8u; k++) { rq_psct_note(reg + k, pdata + k * 16u); } }
    if (g_cfg.bloom_diag) { /* DIAG(SHADE-M2,W117): Aufrufe im Diag-Bild mit Draw-Index + Aufrufer */
        extern void bloom_diag_psc(unsigned reg, unsigned pdata, unsigned count, unsigned ra);
        bloom_diag_psc(reg, pdata, count, g_psc_ra);
    }
    if (!(g_cfg.shading && g_cfg.shade_psconst)) { /* FIX(PSCONST,W84): nur Diag-Zaehler */
        if (g_cfg.rq_diag && g_rq_psconst_calls <= 64u) {
            HLOG_W("RQPSC #%u f=%u reg=%u n=%u (nur gezaehlt) ps=0x%X", g_rq_psconst_calls, g_present_no, reg, count, g_im_pshader);
        }
        return 0;
    }
    __try {
        for (k = 0; k < count && reg + k < 8u; k++) {
            const float *f = (const float *)(uintptr_t)(pdata + k * 16u);
            g_rq_psconst[reg + k] = (rq_q8(f[3]) << 24) | (rq_q8(f[0]) << 16) |
                                    (rq_q8(f[1]) << 8) | rq_q8(f[2]);
            g_rq_psconst_set |= 1u << (reg + k);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (g_cfg.rq_diag && g_rq_psconst_calls <= 64u) {
        HLOG_W("RQPSC #%u f=%u reg=%u n=%u -> 0x%08X ps=0x%X", g_rq_psconst_calls, g_present_no,
               reg, count, reg < 8u ? g_rq_psconst[reg] : 0u, g_im_pshader);
    }
    return 0;
}

/* ---------- DIAG (rq_diag>=2): Draws ausserhalb der Mission ----------
 * Fuer die Ausbilder-Zwischenszene nach der Mission (Szene 0x30, Textbox ohne
 * Text): je 2000. Frame alle Draws mit Textur-/Farb-/Lagedaten (RQDLG) und
 * einmal je Textur-xptr die D3D11-Textur als BMP (Farbe + Alpha) nach
 * shots/rqtex_<xptr>_{rgb,a}.bmp (nur 8-Bit-RGBA-Formate). */
static void rq_bmp(const char *path, const unsigned char *px, unsigned w, unsigned h, unsigned pitch,
                   int bgra, int alpha)
{
    FILE *f;
    unsigned y, x, row = (w * 3u + 3u) & ~3u;
    unsigned char hdr[54], *line;
    if (fopen_s(&f, path, "wb") || !f) { return; }
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    *(unsigned *)(hdr + 2) = 54u + row * h; *(unsigned *)(hdr + 10) = 54u; *(unsigned *)(hdr + 14) = 40u;
    *(int *)(hdr + 18) = (int)w; *(int *)(hdr + 22) = (int)h; *(unsigned short *)(hdr + 26) = 1;
    *(unsigned short *)(hdr + 28) = 24; *(unsigned *)(hdr + 34) = row * h;
    fwrite(hdr, 1, 54, f);
    line = (unsigned char *)calloc(1, row);
    for (y = 0; y < h && line; y++) {
        const unsigned char *s = px + (size_t)(h - 1u - y) * pitch;
        for (x = 0; x < w; x++) {
            unsigned char r = bgra ? s[x * 4 + 2] : s[x * 4], g = s[x * 4 + 1], b = bgra ? s[x * 4] : s[x * 4 + 2];
            if (alpha) { r = g = b = s[x * 4 + 3]; }
            line[x * 3] = b; line[x * 3 + 1] = g; line[x * 3 + 2] = r;
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
}

static const char *g_rq_dump_name; /* W98: optionaler Dateiname-Zusatz */
void rq_dump_srv_named(ID3D11ShaderResourceView *srv, unsigned xptr, const char *name)
{
    g_rq_dump_name = name;
    rq_dump_srv(srv, xptr);
    g_rq_dump_name = NULL;
}
void rq_dump_srv(ID3D11ShaderResourceView *srv, unsigned xptr) /* W98: auch fuer sbar_diag */
{
    ID3D11Resource *res = NULL;
    ID3D11Texture2D *stg = NULL;
    D3D11_TEXTURE2D_DESC dd;
    D3D11_MAPPED_SUBRESOURCE m;
    char path[MAX_PATH];
    int bgra;
    if (!srv) { return; }
    ID3D11ShaderResourceView_GetResource(srv, &res);
    if (!res) { return; }
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)res, &dd);
    bgra = (dd.Format == DXGI_FORMAT_B8G8R8A8_UNORM || dd.Format == DXGI_FORMAT_B8G8R8X8_UNORM);
    HLOG_W("RQTEX xptr=0x%X %ux%u fmt=%u mips=%u", xptr, dd.Width, dd.Height, (unsigned)dd.Format, dd.MipLevels);
    if (dd.Format == DXGI_FORMAT_BC1_UNORM || dd.Format == DXGI_FORMAT_BC2_UNORM || dd.Format == DXGI_FORMAT_BC3_UNORM) {
        /* DIAG(TEXADDR,W84): BCn-Farbblock per CPU entpacken (nur RGB, Mip 0) */
        unsigned bsz = (dd.Format == DXGI_FORMAT_BC1_UNORM) ? 8u : 16u, w = dd.Width, h = dd.Height;
        dd.MipLevels = 1; dd.ArraySize = 1; dd.Usage = D3D11_USAGE_STAGING; dd.BindFlags = 0;
        dd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; dd.MiscFlags = 0; dd.SampleDesc.Count = 1; dd.SampleDesc.Quality = 0;
        if (w >= 4u && h >= 4u && SUCCEEDED(ID3D11Device_CreateTexture2D(g_dev, &dd, NULL, &stg)) && stg) {
            unsigned char *px = (unsigned char *)calloc((size_t)w * h, 4u);
            ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)stg, 0, 0, 0, 0, res, 0, NULL);
            if (px && SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)stg, 0, D3D11_MAP_READ, 0, &m))) {
                unsigned bx, by, k;
                for (by = 0; by < h / 4u; by++) {
                    for (bx = 0; bx < w / 4u; bx++) {
                        const unsigned char *b = (const unsigned char *)m.pData + (size_t)by * m.RowPitch + bx * bsz + (bsz - 8u);
                        unsigned c0 = b[0] | (b[1] << 8), c1 = b[2] | (b[3] << 8), bits = b[4] | (b[5] << 8) | (b[6] << 16) | ((unsigned)b[7] << 24);
                        unsigned char pal[4][3];
                        pal[0][0] = (unsigned char)(((c0 >> 11) & 31u) * 255u / 31u); pal[0][1] = (unsigned char)(((c0 >> 5) & 63u) * 255u / 63u); pal[0][2] = (unsigned char)((c0 & 31u) * 255u / 31u);
                        pal[1][0] = (unsigned char)(((c1 >> 11) & 31u) * 255u / 31u); pal[1][1] = (unsigned char)(((c1 >> 5) & 63u) * 255u / 63u); pal[1][2] = (unsigned char)((c1 & 31u) * 255u / 31u);
                        for (k = 0; k < 3u; k++) {
                            if (c0 > c1 || bsz == 16u) {
                                pal[2][k] = (unsigned char)((2u * pal[0][k] + pal[1][k]) / 3u);
                                pal[3][k] = (unsigned char)((pal[0][k] + 2u * pal[1][k]) / 3u);
                            } else {
                                pal[2][k] = (unsigned char)((pal[0][k] + pal[1][k]) / 2u); pal[3][k] = 0;
                            }
                        }
                        for (k = 0; k < 16u; k++) {
                            unsigned idx = (bits >> (k * 2u)) & 3u, x = bx * 4u + (k & 3u), y = by * 4u + (k >> 2);
                            unsigned char *o = px + ((size_t)y * w + x) * 4u;
                            o[0] = pal[idx][0]; o[1] = pal[idx][1]; o[2] = pal[idx][2]; o[3] = 255;
                        }
                    }
                }
                ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)stg, 0);
                _snprintf_s(path, sizeof path, _TRUNCATE, "%s/shots/rqtex_%08X_rgb.bmp", hcfg_out_dir(), xptr);
                rq_bmp(path, px, w, h, w * 4u, 0, 0);
            }
            free(px);
            ID3D11Texture2D_Release(stg);
        }
        ID3D11Resource_Release(res);
        return;
    }
    if (!bgra && dd.Format != DXGI_FORMAT_R8G8B8A8_UNORM) { ID3D11Resource_Release(res); return; }
    dd.MipLevels = 1; dd.ArraySize = 1; dd.Usage = D3D11_USAGE_STAGING; dd.BindFlags = 0;
    dd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; dd.MiscFlags = 0; dd.SampleDesc.Count = 1; dd.SampleDesc.Quality = 0;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(g_dev, &dd, NULL, &stg)) && stg) {
        ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)stg, 0, 0, 0, 0, res, 0, NULL);
        if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)stg, 0, D3D11_MAP_READ, 0, &m))) {
            _snprintf_s(path, sizeof path, _TRUNCATE, "%s/shots/rqtex_%08X%s_rgb.bmp", hcfg_out_dir(), xptr, g_rq_dump_name ? g_rq_dump_name : "");
            rq_bmp(path, (const unsigned char *)m.pData, dd.Width, dd.Height, m.RowPitch, bgra, 0);
            _snprintf_s(path, sizeof path, _TRUNCATE, "%s/shots/rqtex_%08X%s_a.bmp", hcfg_out_dir(), xptr, g_rq_dump_name ? g_rq_dump_name : "");
            rq_bmp(path, (const unsigned char *)m.pData, dd.Width, dd.Height, m.RowPitch, bgra, 1);
            ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)stg, 0);
        }
        ID3D11Texture2D_Release(stg);
    }
    ID3D11Resource_Release(res);
}

static unsigned g_rq_dlg_lastent;
void rq_dlg_draw(const im_vertex_t *v, unsigned n, int rhw, ID3D11ShaderResourceView *srv, unsigned prim)
{
    unsigned scene, i, fmt = 0, size = 0, c0 = 0;
    float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f, u0 = 1e30f, u1 = -1e30f, w0 = 1e30f, w1 = -1e30f;
    static unsigned seenx[64], nseen, lastf, nf;
    if (g_cfg.rq_diag < 2 || !n) { return; }
    scene = rq_u32(M3D_SCENE_ADDR);
    if (scene == 0x2Cu || (g_present_no % 2000u) != 0u) { return; }
    if (g_present_no != lastf) { lastf = g_present_no; if (++nf > 40u) { return; } }
    for (i = 0; i < n; i++) {
        if (v[i].pos[0] < x0) { x0 = v[i].pos[0]; } if (v[i].pos[0] > x1) { x1 = v[i].pos[0]; }
        if (v[i].pos[1] < y0) { y0 = v[i].pos[1]; } if (v[i].pos[1] > y1) { y1 = v[i].pos[1]; }
        if (v[i].uv[0] < u0) { u0 = v[i].uv[0]; } if (v[i].uv[0] > u1) { u1 = v[i].uv[0]; }
        if (v[i].uv[1] < w0) { w0 = v[i].uv[1]; } if (v[i].uv[1] > w1) { w1 = v[i].uv[1]; }
    }
    {
        unsigned xp = sb_tex_xptr();
        if (xp) { c0 = rq_u32(xp); fmt = rq_u32(xp + 12u); size = rq_u32(xp + 16u); }
        if (g_present_no != g_rq_dlg_lastent) {
            unsigned cnt = rq_u32(0x4C19C0u) & 0xFFFFu, k2;
            char tb[200];
            int o2 = 0;
            g_rq_dlg_lastent = g_present_no;
            tb[0] = 0;
            for (k2 = 0; k2 < cnt && k2 < 24u; k2++) {
                o2 += _snprintf_s(tb + o2, sizeof tb - (size_t)o2, _TRUNCATE, "%02X/%02X ",
                                  rq_u32(0x4B99C0u + k2 * 16u + 8u) >> 16 & 0xFFu,
                                  rq_u32(0x4B99C0u + k2 * 16u + 8u) >> 24 & 0xFFu);
                if (o2 < 0) { break; }
            }
            HLOG_W("RQDLGQ f=%u cnt=%u sel[0x340F00]=%u eintraege(+A/+B)=%s", g_present_no, cnt,
                   rq_u32(0x340F00u) & 0xFFFFu, tb);
        }
        HLOG_W("RQDLG f=%u cnt=%u scene=0x%X prim=%u n=%u rhw=%d vs=0x%X ps=0x%X xp=0x%X c0=0x%X fmt=0x%X size=0x%X srv=%d why=%u x=[%.1f,%.1f] y=[%.1f,%.1f] uv=[%.3f,%.3f]x[%.3f,%.3f] col=(%.2f,%.2f,%.2f,%.2f) cm=0x%X",
               g_present_no, rq_u32(0x4C19C0u) & 0xFFFFu, scene, prim, n, rhw, g_im_shader, g_im_pshader, xp, c0, fmt, size, srv != NULL,
               sb_tex_why(), x0, x1, y0, y1, u0, u1, w0, w1, v[0].col[0], v[0].col[1], v[0].col[2], v[0].col[3], rs_cmask);
        if (srv && xp) {
            for (i = 0; i < nseen && seenx[i] != xp; i++) {}
            if (i == nseen && nseen < 64u) { seenx[nseen++] = xp; rq_dump_srv(srv, xp); }
        }
    }
}

/* ---------- FIX(DLGTEXT, W77): CPU-Text in Render-Target-Texturen ----------
 * Die Ausbilder-Zwischenszene nach der Mission (Szene 0x30) zeigt Textbox +
 * Weiter-Pfeil, aber keinen Text. Befund (W77_s2, rq_diag=2): der Text liegt
 * in einer Vollbild-Ebene (Textur 0x4C19C8, 1024x1024 linear, Quad 640x480
 * mit UV 0..0.625 x 0..0.469, Farbe a=0.75), die in der Szene von keinem
 * Draw beschrieben wird. Geschrieben wird sie per CPU: FUN_00194D25 (XFONT-
 * artiges TextOut der XAPI-Bibliothek, einziger Aufrufer 0x4F121 in der
 * Kompositions-Funktion FUN_0004EF50) holt D3DSurface_GetDesc (0x211060),
 * sperrt die Surface mit D3DSurface_LockRect (0x211080, einziger Aufrufer
 * 0x194D42) und rastert den Text per FUN_00194C56 direkt in den Speicher.
 * 0x4C19C8 ist aber zugleich ein Render-Target (RTT-Eintrag) -> gesampelt
 * wird die D3D11-RT-Textur, die die CPU-Schriebe nie sieht. Fix
 * ([render] dlg_text_upload=1): nach jedem TextOut den gesperrten Speicher
 * in die passende RT-Textur hochladen (32 Bit). Die Surface beschreibt nur
 * den genutzten Ausschnitt (640x480, Pitch der 1024er-Textur), die RT-Textur
 * ist 1024x1024 (gleiche Data-Adresse) -> Upload in das Teilrechteck. */
static unsigned g_dlg_surf, g_dlg_n, g_dlg_up;
/* DIAG(DLGTEXT,W85, dlg_diag=1): QPC-Kosten je Abschnitt (0 Rastern, 1 Clear, 2 Upload) */
static LONGLONG g_dlg_tick[4], g_dlg_cnt[4];
static unsigned g_dlg_gpu_clr; /* FIX(DLGTEXT3): GPU-Clear (PC-Flag TARGET) einer Textebene -> Delta-Upload-Stand ungueltig */
static LONGLONG dlg_qpc(void) { LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart; }
static void dlg_perf_add(int k, LONGLONG t0)
{
    g_dlg_tick[k] += dlg_qpc() - t0; g_dlg_cnt[k]++;
    if (k == 2 && (g_dlg_cnt[2] % 1000) == 0) {
        LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
        HLOG_I("DLGPERF f=%u n=%lld/%lld/%lld us je Aufruf raster=%.1f clear=%.1f upload=%.1f gpu=%lld x %.1f", g_present_no,
               g_dlg_cnt[0], g_dlg_cnt[1], g_dlg_cnt[2],
               g_dlg_cnt[0] ? g_dlg_tick[0] * 1e6 / (double)fq.QuadPart / (double)g_dlg_cnt[0] : 0.0,
               g_dlg_cnt[1] ? g_dlg_tick[1] * 1e6 / (double)fq.QuadPart / (double)g_dlg_cnt[1] : 0.0,
               g_dlg_tick[2] * 1e6 / (double)fq.QuadPart / (double)g_dlg_cnt[2], g_dlg_cnt[3],
               g_dlg_cnt[3] ? g_dlg_tick[3] * 1e6 / (double)fq.QuadPart / (double)g_dlg_cnt[3] : 0.0);
        g_dlg_tick[0] = g_dlg_tick[1] = g_dlg_tick[2] = g_dlg_tick[3] = 0;
        g_dlg_cnt[0] = g_dlg_cnt[1] = g_dlg_cnt[2] = g_dlg_cnt[3] = 0;
    }
}

void rq_text_lock_note(unsigned surf) { g_dlg_surf = surf; }

/* TextOut wird je Bild bis zu ~56x aufgerufen (ein Aufruf je Textzeile) ->
 * nur vormerken, hochgeladen wird einmal vor dem naechsten Draw/Present
 * (rq_text_flush). */
static unsigned g_dlg_pend, g_dlg_pbits, g_dlg_ppitch, g_dlg_pw, g_dlg_ph, g_dlg_pfmt, g_dlg_psurf;
static void rq_text_upload_now(unsigned bits, unsigned pitch, unsigned w, unsigned h, unsigned fmt);
void rq_text_upload(unsigned bits, unsigned pitch, unsigned w, unsigned h, unsigned fmt)
{
    g_dlg_pend = 1; g_dlg_pbits = bits; g_dlg_ppitch = pitch; g_dlg_pw = w; g_dlg_ph = h;
    g_dlg_pfmt = fmt; g_dlg_psurf = g_dlg_surf;
}
void rq_text_flush(void)
{
    unsigned keep;
    if (!g_dlg_pend) { return; }
    g_dlg_pend = 0;
    keep = g_dlg_surf;
    g_dlg_surf = g_dlg_psurf;
    if (g_cfg.dlg_diag) {
        LONGLONG t0 = dlg_qpc();
        rq_text_upload_now(g_dlg_pbits, g_dlg_ppitch, g_dlg_pw, g_dlg_ph, g_dlg_pfmt);
        dlg_perf_add(2, t0);
    } else {
        rq_text_upload_now(g_dlg_pbits, g_dlg_ppitch, g_dlg_pw, g_dlg_ph, g_dlg_pfmt);
    }
    g_dlg_surf = keep;
}
/* FIX(DLGTEXT3,W85, dlg_text_raster=1): Schattenkopie der Textebene in
 * normalem Speicher. Der Xbox-Speicher der Surface ist teuer zu LESEN
 * (W85_c_perf: 1,9 MB memcpy ~10 ms je Bild, Schreiben/Clear 0,3 ms ->
 * nicht gecachter/WC-Speicher). Der Clear (rq_text_on_clear) fuellt den
 * Schatten direkt mit der Farbe, TextOut meldet die beschriebenen Zeilen
 * (dlg_rows_touch); gelesen werden nur diese Zeilen. */
static unsigned char *g_dlg_tmp;
static size_t g_dlg_tmpn;
#define DLG_SH_ROWS 4096
static unsigned g_sh_ok, g_sh_bits, g_sh_pitch, g_sh_rows, g_sh_color;
static unsigned char g_sh_dirty[DLG_SH_ROWS]; /* Zeile von TextOut beschrieben, noch nicht gelesen */
static unsigned char g_sh_clr[DLG_SH_ROWS];   /* Zeile = g_sh_color in Xbox-Speicher und Schatten */
static void dlg_rows_touch(int lo, int hi)
{
    int y;
    if (lo < 0) { lo = 0; }
    if (hi > DLG_SH_ROWS) { hi = DLG_SH_ROWS; }
    for (y = lo; y < hi; y++) { g_sh_dirty[y] = 1; g_sh_clr[y] = 0; }
}
static int dlg_tmp_grow(size_t need)
{
    if (need <= g_dlg_tmpn) { return 1; }
    free(g_dlg_tmp);
    g_dlg_tmp = (unsigned char *)malloc(need);
    g_dlg_tmpn = g_dlg_tmp ? need : 0;
    g_sh_ok = 0;
    return g_dlg_tmp != NULL;
}
static void rq_text_upload_now(unsigned bits, unsigned pitch, unsigned w, unsigned h, unsigned fmt)
{
    unsigned data = 0, i;
    unsigned char *tmp;
    size_t need;
    int ok = 1;
    if (!g_dev || !g_dlg_surf || !bits || !w || !h || w > 4096u || h > 4096u || pitch < w * 4u) {
        static unsigned nbad;
        if (nbad++ < 8u) {
            HLOG_I("DLGTEXT verworfen: surf=0x%X bits=0x%X pitch=%u %ux%u fmt=0x%X", g_dlg_surf, bits, pitch, w, h, fmt);
        }
        return;
    }
    data = rq_u32(g_dlg_surf + 4u);
    if (!data) { return; }
    for (i = 0; i < (unsigned)g_rtt_n; i++) {
        if (g_rtt[i].surf && g_rtt[i].data == data && !g_rtt[i].isdepth && g_rtt[i].tex && g_rtt[i].w >= w && g_rtt[i].h >= h) { break; }
    }
    if (i >= (unsigned)g_rtt_n) {
        if (g_dlg_n++ < 4u) {
            HLOG_I("DLGTEXT #%u: surf=0x%X data=0x%X %ux%u fmt=0x%X - kein RT (CPU-Textur, alter Weg)",
                   g_dlg_n, g_dlg_surf, data, w, h, fmt);
        }
        return;
    }
    need = (size_t)pitch * h;
    if (!dlg_tmp_grow(need)) { return; }
    tmp = g_dlg_tmp;
    if (g_cfg.dlg_text_raster && g_sh_ok && g_sh_bits == bits && g_sh_pitch == pitch && g_sh_rows >= h && h <= DLG_SH_ROWS) {
        unsigned y;
        __try {
            for (y = 0; y < h; y++) {
                if (g_sh_dirty[y]) { memcpy(tmp + (size_t)y * pitch, (const void *)(uintptr_t)(bits + y * pitch), (size_t)w * 4u); }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    } else {
        __try { memcpy(tmp, (const void *)(uintptr_t)bits, need); }
        __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
        g_sh_ok = ok && g_cfg.dlg_text_raster && h <= DLG_SH_ROWS; g_sh_bits = bits; g_sh_pitch = pitch; g_sh_rows = h;
        memset(g_sh_clr, 0, sizeof g_sh_clr);
        if (g_cfg.dlg_diag) {
            MEMORY_BASIC_INFORMATION mi;
            static unsigned nq;
            if (nq++ < 4u && VirtualQuery((const void *)(uintptr_t)bits, &mi, sizeof mi)) {
                HLOG_I("DLGTEXT3 Vollkopie bits=0x%X %ux%u: Protect=0x%X (WC=0x400/NOCACHE=0x200) Type=0x%X", bits, w, h,
                       (unsigned)mi.Protect, (unsigned)mi.Type);
            }
        }
    }
    memset(g_sh_dirty, 0, sizeof g_sh_dirty);
    if (!ok) { g_sh_ok = 0; return; }
    if (g_cfg.dlg_diag && g_cfg.dlg_text_raster) { /* DIAG(DLGTEXT3): Schatten gegen Vollkopie pruefen */
        static unsigned nv, nvlog;
        if ((nv++ % 500u) == 0u) {
            unsigned char *chk = (unsigned char *)malloc(need);
            unsigned y, bad = 0, first = 0xFFFFFFFFu;
            if (chk) {
                __try { memcpy(chk, (const void *)(uintptr_t)bits, need); } __except (EXCEPTION_EXECUTE_HANDLER) { bad = 0xFFFFFFFFu; }
                for (y = 0; bad != 0xFFFFFFFFu && y < h; y++) {
                    if (memcmp(chk + (size_t)y * pitch, tmp + (size_t)y * pitch, (size_t)w * 4u) != 0) { if (first == 0xFFFFFFFFu) { first = y; } bad++; }
                }
                free(chk);
                if (nvlog++ < 40u) { HLOG_I("DLGTEXT3 verify f=%u %ux%u: %u Zeilen abweichend (erste %d)", g_present_no, w, h, bad, (int)first); }
            }
        }
    }
    if (!rtt_unscale((int)i) || !g_rtt[i].tex) { return; } /* W88 RENDER-RES: CPU-Upload in Xbox-Pixeln -> RT 1:1 */
    {
        /* FIX(DLGTEXT3,W85, mit dlg_text_raster=1): nur geaenderte Zeilen
         * hochladen. Die Textebene wird nur per CPU beschrieben (W77: kein
         * Draw in diese RT), ihr D3D11-Inhalt ist also der zuletzt
         * hochgeladene Stand. Gleiche RT-Textur/Groesse und gleiche Bytes ->
         * kein UpdateSubresource (Vollbild-Upload kostete ~9 ms je Bild,
         * W85_b_perf). */
        static unsigned char *last;
        static size_t lastn;
        static void *ltex;
        static unsigned lw, lh, lpitch;
        unsigned y0 = 0, y1 = h;
        int same = 0;
        if (g_cfg.dlg_text_raster) {
            if (g_dlg_gpu_clr) { g_dlg_gpu_clr = 0; ltex = NULL; }
            if (last && lastn >= need && ltex == (void *)g_rtt[i].tex && lw == w && lh == h && lpitch == pitch) {
                while (y0 < h && memcmp(tmp + (size_t)y0 * pitch, last + (size_t)y0 * pitch, (size_t)w * 4u) == 0) { y0++; }
                if (y0 >= h) { same = 1; }
                else {
                    while (y1 > y0 && memcmp(tmp + (size_t)(y1 - 1u) * pitch, last + (size_t)(y1 - 1u) * pitch, (size_t)w * 4u) == 0) { y1--; }
                }
            }
            if (same) { return; }
            if (lastn < need) { free(last); last = (unsigned char *)malloc(need); lastn = last ? need : 0; }
            if (last) {
                memcpy(last + (size_t)y0 * pitch, tmp + (size_t)y0 * pitch, (size_t)(y1 - y0) * pitch);
                ltex = (void *)g_rtt[i].tex; lw = w; lh = h; lpitch = pitch;
            }
        }
        {
            LONGLONG t0 = g_cfg.dlg_diag ? dlg_qpc() : 0;
            D3D11_BOX bx;
            sb_ctx_enter();
            bx.left = 0; bx.top = y0; bx.front = 0; bx.right = w; bx.bottom = y1; bx.back = 1;
            ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)g_rtt[i].tex, 0, &bx,
                                                  tmp + (size_t)y0 * pitch, pitch, 0);
            sb_ctx_leave();
            if (g_cfg.dlg_diag) { g_dlg_tick[3] += dlg_qpc() - t0; g_dlg_cnt[3]++; }
        }
    }
    if (g_dlg_up++ < 24u || (g_dlg_up % 500u) == 0u) {
        unsigned nz = 0, nrgb = 0, y, x, first = 0;
        for (y = 0; y < h; y++) {
            const unsigned *row = (const unsigned *)(tmp + (size_t)y * pitch);
            for (x = 0; x < w; x++) {
                if (row[x] & 0xFF000000u) { nz++; }
                if (row[x] & 0x00FFFFFFu) { if (!nrgb) { first = row[x]; } nrgb++; }
            }
        }
        HLOG_I("DLGTEXT up #%u: surf=0x%X data=0x%X %ux%u pitch=%u fmt=0x%X -> RT%u (f=%u, %u Pixel mit Alpha>0, %u mit RGB>0, erster 0x%08X, Szene 0x%X)",
               g_dlg_up, g_dlg_surf, data, w, h, pitch, fmt, i, g_present_no, nz, nrgb, first, rq_u32(M3D_SCENE_ADDR));
    }
}

typedef unsigned(__stdcall *rq_fn4_t)(unsigned, unsigned, unsigned, unsigned);
typedef unsigned(__stdcall *rq_fn10_t)(unsigned, unsigned, unsigned, unsigned, unsigned,
                                       unsigned, unsigned, unsigned, unsigned, unsigned);
/* Aufrufstelle 0x194D42: D3DSurface_LockRect(surf, pLocked, pRect, flags) */
int sb_tex_lockbits(unsigned xptr, unsigned *pitch, unsigned *bits, unsigned flags);
unsigned __stdcall hook_txt_lockrect(unsigned surf, unsigned plr, unsigned prect, unsigned flags)
{
    unsigned r, pitch = 0, bits = 0, fixed = 0;
    static unsigned nlog;
    rq_text_lock_note(surf);
    r = ((rq_fn4_t)0x211080u)(surf, plr, prect, flags);
    __try { pitch = *(unsigned *)(uintptr_t)plr; bits = *(unsigned *)(uintptr_t)(plr + 4u); }
    __except (EXCEPTION_EXECUTE_HANDLER) { pitch = bits = 0; }
    /* Original-Lock liefert fuer Render-Target-Surfaces (Fake-Device) keinen
     * gueltigen Zeiger -> TextOut rasterte nichts. Dann Bits/Pitch aus dem
     * Surface-Kopf wie hook_D3DTexture_LockRect (sb_tex_lockbits). */
    if (!bits || IsBadWritePtr((void *)(uintptr_t)bits, 4)) {
        unsigned p2 = 0, b2 = 0;
        if (sb_tex_lockbits(surf, &p2, &b2, flags) && b2) {
            __try { *(unsigned *)(uintptr_t)plr = p2; *(unsigned *)(uintptr_t)(plr + 4u) = b2; fixed = 1; }
            __except (EXCEPTION_EXECUTE_HANDLER) { fixed = 0; }
        }
        if (nlog++ < 8u) {
            HLOG_I("DLGTEXT lock: surf=0x%X data=0x%X orig bits=0x%X pitch=%u -> %s 0x%X/%u", surf,
                   rq_u32(surf + 4u), bits, pitch, fixed ? "Kopf" : "kein Ersatz", b2, p2);
        }
    }
    return r;
}
/* FIX(DLGTEXT2,W97): Textebene vor dem Rastern loeschen wie die Xbox.
 * FUN_0004EF50 bindet die Textebene (SetRenderTarget, Aufrufer 0x4EF78),
 * loescht sie mit D3DDevice_Clear(0, NULL, 0xF0 = D3DCLEAR_TARGET, 0) und
 * rastert dann je Warteschlangen-Eintrag (0x4B99C0, ein Zeichen je Eintrag)
 * per TextOut per CPU in denselben Speicher. Auf der Xbox schreibt der
 * GPU-Clear den gemeinsamen Speicher; hier erreicht er ihn nie -> der
 * 640x480-Ausschnitt behielt Altdaten (wiederverwendeter Texturspeicher,
 * W97_h_dlg: Bildmuell statt Text) bzw. die Glyphen alter Bilder. Darum:
 * Clear mit Ziel-Bits (Xbox 0xF0) auf einer Surface, die TextOut schon
 * gesperrt hat -> Xbox-Speicher mit der Clear-Farbe fuellen und den
 * Upload vormerken (auch ohne TextOut wird die Ebene so wieder leer).
 * Ist die Ebene seit dem letzten Clear unveraendert leer (kein TextOut,
 * gleiche Farbe), entfaellt Fuellen und Upload. Schalter [render]
 * dlg_text_clear (Standard 1, wirkt nur mit dlg_text_upload=1). */
#define RQ_TXT_MAX 4
static unsigned g_txt_surf[RQ_TXT_MAX], g_txt_clean[RQ_TXT_MAX], g_txt_color[RQ_TXT_MAX], g_txt_n;
static int rq_txt_slot(unsigned surf, int add)
{
    unsigned k;
    for (k = 0; k < g_txt_n; k++) { if (g_txt_surf[k] == surf) { return (int)k; } }
    if (!add) { return -1; }
    k = (g_txt_n < RQ_TXT_MAX) ? g_txt_n++ : (g_txt_n - 1u); /* voll: letzten Eintrag ersetzen */
    g_txt_surf[k] = surf; g_txt_clean[k] = 0; g_txt_color[k] = 0;
    HLOG_I("DLGTEXT2 Textebene #%u: surf=0x%X data=0x%X", k, surf, rq_u32(surf + 4u));
    return (int)k;
}
void rq_text_on_clear(unsigned surf, unsigned flags, unsigned color)
{
    unsigned data, fmt, size, w = 0, h = 0, pitch = 0, bits = 0, y, x;
    int k, ok = 1;
    static unsigned nlog;
    if ((flags & XB_CLEAR_TARGET) && rq_txt_slot(surf, 0) >= 0) { g_dlg_gpu_clr = 1; } /* FIX(DLGTEXT3,W85) */
    if (!g_cfg.dlg_text_upload || !g_cfg.dlg_text_clear || !surf || !(flags & 0xF0u)) { return; }
    k = rq_txt_slot(surf, 0);
    if (k < 0) { return; }
    if (g_txt_clean[k] && g_txt_color[k] == color) { return; }
    data = rq_u32(surf + 4u); fmt = rq_u32(surf + 12u); size = rq_u32(surf + 16u);
    LONGLONG t0 = g_cfg.dlg_diag ? dlg_qpc() : 0;
    if (!data || !rtt_dims(fmt, size, &w, &h) || !sb_tex_lockbits(surf, &pitch, &bits, 0) || !bits ||
        pitch < w * 4u || w > 2048u || h > 2048u) {
        if (nlog++ < 8u) { HLOG_I("DLGTEXT2 clear verworfen: surf=0x%X data=0x%X %ux%u bits=0x%X pitch=%u", surf, data, w, h, bits, pitch); }
        return;
    }
    if (g_cfg.dlg_text_raster && h <= DLG_SH_ROWS && dlg_tmp_grow((size_t)pitch * h)) {
        /* FIX(DLGTEXT3,W85): Xbox-Speicher (Write-Combine, Lesen teuer) und
         * Schatten nur in Zeilen fuellen, die nicht schon die Clear-Farbe
         * tragen (nach dem ersten Clear: nur die Textzeilen). */
        int same = g_sh_ok && g_sh_bits == bits && g_sh_pitch == pitch && g_sh_rows >= h && g_sh_color == color;
        __try {
            for (y = 0; y < h; y++) {
                unsigned *row, *srow;
                if (same && g_sh_clr[y] && !g_sh_dirty[y]) { continue; }
                row = (unsigned *)(uintptr_t)(bits + y * pitch);
                srow = (unsigned *)(g_dlg_tmp + (size_t)y * pitch);
                if (!color) { memset(row, 0, (size_t)w * 4u); memset(srow, 0, (size_t)w * 4u); }
                else { for (x = 0; x < w; x++) { row[x] = color; srow[x] = color; } }
                g_sh_clr[y] = 1; g_sh_dirty[y] = 0;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
        if (!ok) { g_sh_ok = 0; return; }
        if (!same) { memset(g_sh_dirty, 0, sizeof g_sh_dirty); }
        g_sh_ok = 1; g_sh_bits = bits; g_sh_pitch = pitch; g_sh_rows = h; g_sh_color = color;
    } else {
        __try {
            for (y = 0; y < h; y++) {
                unsigned *row = (unsigned *)(uintptr_t)(bits + y * pitch);
                if (!color) { memset(row, 0, (size_t)w * 4u); }
                else { for (x = 0; x < w; x++) { row[x] = color; } }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
        if (!ok) { return; }
        g_sh_ok = 0;
    }
    g_dlg_surf = surf;
    rq_text_upload(bits, pitch, w, h, (fmt >> 8) & 0xFFu);
    g_txt_clean[k] = 1; g_txt_color[k] = color;
    if (g_cfg.dlg_diag) { dlg_perf_add(1, t0); }
    if (nlog++ < 8u) {
        HLOG_I("DLGTEXT2 clear #%u: surf=0x%X data=0x%X bits=0x%X %ux%u pitch=%u color=0x%08X flags=0x%X f=%u",
               nlog, surf, data, bits, w, h, pitch, color, flags, g_present_no);
    }
}

/* Aufrufstelle 0x194D65: FUN_00194C56(font, bits, pitch, w, h, fmt, a, b, c, d), stdcall */
/* DIAG(DLGTEXT,W85): Glyphen-Suche des Fonts wie der Rasterer 0x195E1C
 * (0x194B21: ecx=Font, dx=Zeichen, Stack &Glyphe, &Groesse; ret 8). */
static int dlg_glyph(unsigned font, unsigned chr, unsigned *pg, unsigned *ps)
{
    int r = -1;
    __try {
        __asm {
            push ps
            push pg
            mov ecx, font
            mov edx, chr
            mov eax, 0x194B21
            call eax
            mov r, eax
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { r = -2; }
    return r;
}
static unsigned dlg_count(unsigned bits, unsigned pitch, unsigned w, unsigned h, int x0, int y0)
{
    unsigned n = 0;
    int x, y;
    __try {
        for (y = y0 < 0 ? 0 : y0; y < y0 + 64 && y < (int)h; y++) {
            const unsigned *row = (const unsigned *)(uintptr_t)(bits + (unsigned)y * pitch);
            for (x = x0 < 0 ? 0 : x0; x < x0 + 64 && x < (int)w; x++) { if (row[x]) { n++; } }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { n = 0xFFFFFFFFu; }
    return n;
}
static void dlg_diag_call(unsigned font, unsigned bits, unsigned pitch, unsigned w, unsigned h, unsigned fmt,
                          unsigned str, unsigned len, unsigned x, unsigned y, unsigned r, unsigned before)
{
    static unsigned ncall, nlog;
    unsigned scene = rq_u32(M3D_SCENE_ADDR), ch = 0, g = 0, gs = 0, after, f[20], i;
    unsigned char gb[8] = {0};
    int gr;
    ncall++;
    if (!(nlog < 80u || (ncall % 4000u) == 1u)) { return; }
    nlog++;
    __try { ch = *(const unsigned short *)(uintptr_t)str; } __except (EXCEPTION_EXECUTE_HANDLER) { ch = 0xFFFFFFFFu; }
    for (i = 0; i < 20u; i++) { f[i] = rq_u32(font + i * 4u); }
    gr = dlg_glyph(font, ch & 0xFFFFu, &g, &gs);
    if (gr >= 0 && g) { __try { memcpy(gb, (const void *)(uintptr_t)g, 8); } __except (EXCEPTION_EXECUTE_HANDLER) { gb[0] = 0xEE; } }
    after = dlg_count(bits, pitch, w, h, (int)x - 8, (int)y - 8);
    HLOG_I("DLGDIAG #%u f=%u sc=0x%X font=0x%X bits=0x%X pitch=%u %ux%u fmt=0x%X ch=0x%04X len=%d x=%d y=%d "
           "ret=0x%X px %u->%u | col=0x%08X bk=0x%08X mode=%u spc=%d align=0x%X clip=%u [%d,%d,%d,%d] "
           "hdr=%u,%u,%u,%u,%u,%u +60=0x%X +68=0x%X +8c=0x%X +90=0x%X +94=0x%X +98=0x%X +9c=0x%X | glyph r=0x%X g=0x%X sz=%u b=%02X %02X %02X %02X %02X data=%02X%02X%02X",
           ncall, g_present_no, scene, font, bits, pitch, w, h, fmt, ch, (int)len, (int)x, (int)y,
           r, before, after, f[10], f[11], f[12], (int)f[8], f[9], f[13], (int)f[14], (int)f[15], (int)f[16], (int)f[17],
           f[1], f[2], f[3], f[4], f[5], f[6], rq_u32(font + 0x60u), rq_u32(font + 0x68u), rq_u32(font + 0x8Cu),
           rq_u32(font + 0x90u), rq_u32(font + 0x94u), rq_u32(font + 0x98u), rq_u32(font + 0x9Cu),
           (unsigned)gr, g, gs, gb[0], gb[1], gb[2], gb[3], gb[4], gb[5], gb[6], gb[7]);
}
unsigned __stdcall hook_txt_render(unsigned font, unsigned bits, unsigned pitch, unsigned w, unsigned h,
                                   unsigned fmt, unsigned a, unsigned b, unsigned c, unsigned d)
{
    unsigned r, before = 0;
    if (g_cfg.dlg_text_raster && (!bits || !pitch || !h || IsBadWritePtr((void *)(uintptr_t)bits, (size_t)pitch * h))) {
        static unsigned nskip; /* FIX(DLGTEXT3,W85): rep stosd 0x196033 ist aktiv -> nie in ungueltigen Speicher rastern */
        if (nskip++ < 8u) { HLOG_I("DLGTEXT3 TextOut uebersprungen: bits=0x%X pitch=%u %ux%u", bits, pitch, w, h); }
        return 0;
    }
    if (g_cfg.dlg_text_raster && rq_txt_slot(g_dlg_surf, 0) < 0 && rq_txt_slot(g_dlg_surf, 1) >= 0) {
        /* FIX(DLGTEXT3,W85): erste Benutzung einer Textebene - der Spiel-Clear
         * dieses Bildes lief vor der Registrierung ins Leere (Altdaten waren
         * ein Bild lang sichtbar, W85_e_ckp up #1). Einmal loeschen wie die
         * Kompositions-Funktion (D3DCLEAR_TARGET 0xF0, Farbe 0). */
        rq_text_on_clear(g_dlg_surf, 0xF0u, 0u);
    }
    if (g_cfg.dlg_diag) { before = dlg_count(bits, pitch, w, h, (int)c - 8, (int)d - 8); }
    {
        LONGLONG t0 = g_cfg.dlg_diag ? dlg_qpc() : 0;
        r = ((rq_fn10_t)0x194C56u)(font, bits, pitch, w, h, fmt, a, b, c, d);
        if (g_cfg.dlg_diag) { dlg_perf_add(0, t0); }
    }
    if (g_cfg.dlg_diag) { dlg_diag_call(font, bits, pitch, w, h, fmt, a, b, c, d, r, before); }
    { int k = rq_txt_slot(g_dlg_surf, 1); if (k >= 0) { g_txt_clean[k] = 0; } } /* FIX(DLGTEXT2,W97) */
    { /* FIX(DLGTEXT3,W85): beschriebene Zeilen (grosszuegig: Zeichenzelle +/- Ober-/Unterlaenge, Ausrichtung 0x18) */
        int f4 = (int)rq_u32(font + 4u), f8 = (int)rq_u32(font + 8u);
        if (f4 < 0 || f4 > 512 || f8 < 0 || f8 > 512) { dlg_rows_touch(0, 0x7FFFFFFF); }
        else { dlg_rows_touch((int)d - f4 - f8 - 4, (int)d + f4 + f8 + 4); }
    }
    if (g_cfg.rq_diag >= 2) { /* DIAG(DLGTEXT2,W97): TextOut-Argumente, Font-Felder, Rueckgabe, Pixel um (x,y) */
        static unsigned nd;
        if (nd < 24u || (r && nd < 64u)) {
            unsigned fo[16] = { 0 }, ch = 0, nz = 0, yy, xx, i2;
            __try { for (i2 = 0; i2 < 16u; i2++) { fo[i2] = *(unsigned *)(uintptr_t)(font + i2 * 4u); } ch = *(unsigned short *)(uintptr_t)a; } __except (EXCEPTION_EXECUTE_HANDLER) {}
            __try {
                for (yy = (d < 480u ? d : 0); yy < (d < 448u ? d + 32u : 480u); yy++) {
                    for (xx = (c < 640u ? c : 0); xx < (c < 608u ? c + 32u : 640u); xx++) {
                        if (*(unsigned *)(uintptr_t)(bits + yy * pitch + xx * 4u)) { nz++; }
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) { nz = 0xFFFFFFFFu; }
            nd++;
            if (nd <= 4u) { /* Glyph-Kopf ueber den Font-Glyph-Lookup 0x194B21 (ecx=Font, edx=Zeichen, 2 Ausgabezeiger) */
                typedef int(__fastcall * rq_glyph_fn)(unsigned, unsigned, unsigned *, unsigned *);
                unsigned gp = 0, gx = 0, fx[24] = { 0 }, gb[6] = { 0 }; int gr = -1;
                __try { for (i2 = 0; i2 < 24u; i2++) { fx[i2] = *(unsigned *)(uintptr_t)(font + 0x40u + i2 * 4u); } } __except (EXCEPTION_EXECUTE_HANDLER) {}
                __try { gr = ((rq_glyph_fn)0x194B21u)(font, ch, &gp, &gx); } __except (EXCEPTION_EXECUTE_HANDLER) { gr = -2; }
                __try { if (gp) { for (i2 = 0; i2 < 6u; i2++) { gb[i2] = *(unsigned *)(uintptr_t)(gp + i2 * 4u); } } } __except (EXCEPTION_EXECUTE_HANDLER) {}
                HLOG_W("DLGTXT2 glyph ch=0x%X r=%d glyph=0x%X extra=0x%X bytes=%08X %08X %08X %08X %08X %08X font[40..9C]=%08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X",
                       ch, gr, gp, gx, gb[0], gb[1], gb[2], gb[3], gb[4], gb[5], fx[0], fx[1], fx[2], fx[3], fx[4], fx[5], fx[6], fx[7], fx[8], fx[9], fx[10], fx[11],
                       fx[12], fx[13], fx[14], fx[15], fx[16], fx[17], fx[18], fx[19], fx[20], fx[21], fx[22], fx[23]);
            }
            HLOG_W("DLGTXT2 #%u: r=0x%X font=0x%X bits=0x%X pitch=%u %ux%u fmt=0x%X ch=0x%X len=%d x=%d y=%d nz32=%u "
                   "font[0..3C]=%08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X f=%u",
                   nd, r, font, bits, pitch, w, h, fmt, ch, (int)b, (int)c, (int)d, nz,
                   fo[0], fo[1], fo[2], fo[3], fo[4], fo[5], fo[6], fo[7], fo[8], fo[9], fo[10], fo[11], fo[12], fo[13], fo[14], fo[15],
                   g_present_no);
        }
    }
    rq_text_upload(bits, pitch, w, h, fmt);
    return r;
}

/* DIAG(DLGTEXT, rq_diag>=2): welche Aufrufstelle der Text-Komposition
 * FUN_0004EF50 laeuft in welcher Szene (je Stelle/Szene einmal, max. 64).
 * Die Wrapper rufen das Original unveraendert auf. */
static void rq_comp_note(unsigned site)
{
    static unsigned seen[64][2], n, calls, maxcnt;
    unsigned scene = rq_u32(M3D_SCENE_ADDR), k, c = rq_u32(0x4C19C0u) & 0xFFFFu;
    if (scene == 0x30u) {
        calls++;
        if (c > maxcnt) { maxcnt = c; }
        if ((calls % 2000u) == 1u) {
            HLOG_W("RQCOMPN site=0x%X scene=0x30 aufrufe=%u cnt=%u maxcnt=%u f=%u", site, calls, c, maxcnt, g_present_no);
        }
    }
    for (k = 0; k < n; k++) { if (seen[k][0] == site && seen[k][1] == scene) { return; } }
    if (n < 64u) {
        seen[n][0] = site; seen[n][1] = scene; n++;
        HLOG_W("RQCOMP site=0x%X scene=0x%X f=%u cnt=%u", site, scene, g_present_no, rq_u32(0x4C19C0u) & 0xFFFFu);
    }
}
#define RQ_COMP_WRAP(site) void hook_comp_##site(void) { rq_comp_note(0x##site##u); ((void (*)(void))0x4EF50u)(); }
RQ_COMP_WRAP(13536)
RQ_COMP_WRAP(1383B)
RQ_COMP_WRAP(CD715)
RQ_COMP_WRAP(E1ECE)
RQ_COMP_WRAP(100226)
RQ_COMP_WRAP(102649)
RQ_COMP_WRAP(1050AB)
