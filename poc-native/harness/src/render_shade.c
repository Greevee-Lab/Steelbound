/* render_shade.c - RENDER-QA (W77): Shading-Pfad [render] shading=1.
 * Bisher zeichnete der IM-Pfad jeden Draw als Vertexfarbe x Textur Stufe 0
 * (g_im_vs/g_im_ps). Mit shading=1 laufen Draws (Mission, Farbschreiben an,
 * Vertex-Programm + Pixelshader-Definition lesbar und unterstuetzt) ueber
 * generierte Shader:
 *  - VS: das NV2A-Vertex-Programm des Spiels als HLSL (render_vshgen.c) -
 *    liefert oD0/oD1/oFog/oT0..3 je Vertex auf der GPU (Licht, Nebelabstand,
 *    Texgen); die Position kommt unveraendert aus dem bisherigen CPU-Pfad.
 *  - PS: die Register-Combiner des Pixelshaders (render_psgen.c) mit bis zu
 *    vier Texturstufen, Combiner-Konstanten und Nebel im Final-Combiner.
 *  - Nebelfaktor nach NV2A-FOG_PARAMS, wie das XDK-D3D sie aus FOGTABLEMODE/
 *    START/END/DENSITY berechnet (Lazy-Fog 0x213340, s. render_vshgen.c).
 *  - Textur-Adressierung je Stufe aus den Deferred-TextureStates (0x21A8B0).
 * Datenwege: Stream 2 = gepackte v-Register (Ring bzw. Tick-Puffer der
 * 120-FPS-Interpolation), Konstanten als Fenster in einem grossen
 * Konstantenpuffer (D3D11.1 *SetConstantBuffers1) - die Interpolation
 * zeichnet einen Tick so ohne erneutes Hochladen nach (render_interp.c).
 * shading=0: kein Aufruf veraendert den bisherigen Pfad (rq_begin -> 0). */
#include "render_int.h"
#include "sb_userdata.h" /* W150 USER-DATA */
#include <d3d11_1.h>
#include "render_qa.h"
#include "render_vsh.h"
#include "render_psgen.h"

#define RQ_VS_MAX 96
#define RQ_PS_MAX 96
#define RQ_XRING_BYTES (32u << 20)   /* Stream-2-Ring ausserhalb der Aufnahme */
#define RQ_CRING_CONST (256u * 1024u) /* Konstanten-Ring (4 MB) */
#define RQ_VMIRROR 0x21BCA8u          /* VS-Konstanten-Spiegel (c-96..95) */

typedef struct {
    unsigned handle, hash, nslots, chk1;
    int ok;
    rqv_prog_t prog;
    unsigned nv;
    unsigned char vreg[16];
    unsigned char cidx[RQV_NCONST];
    int tsrc[4];       /* v-Register, aus dem oTk direkt (MOV/ADD) kommt, sonst -1 */
    unsigned nc;
    ID3D11VertexShader *vs, *vsl;
    ID3D11InputLayout *lay, *layl;
    int kind;          /* vsh_program()->kind (0 Licht/unbekannt, 1 P1, 2 P2) */
    /* SHADE-PERF (W80): Roh-Variante fuer VB-Draws (Decl-Formate auf der GPU) */
    unsigned rawsig;   /* Decl-Signatur der Roh-Variante (0 = keine) */
    int rawok;         /* 1 = Roh-Variante bereit, -1 = nicht moeglich (fuer rawsig) */
    int pkok;          /* gepackte Variante: 1 bereit, -1 nicht moeglich, 0 noch nicht versucht */
    rqv_raw_t raw;
    ID3D11VertexShader *vsr, *vsrl;
    ID3D11InputLayout *layr, *layrl;
} rq_vsent_t;

typedef struct {
    unsigned handle;
    unsigned d[60];
    int ok;
    rqp_info_t info;
    ID3D11PixelShader *ps;
} rq_psent_t;

static rq_vsent_t g_rqvs[RQ_VS_MAX];
static unsigned g_rqvs_n;
static rq_psent_t g_rqps[RQ_PS_MAX];
static unsigned g_rqps_n;

ID3D11DeviceContext1 *g_rq_ctx1;
int g_rq_active;
rq_rec_t g_rq_rec;
static int g_rq_inited, g_rq_dead;
extern unsigned g_rq_shadow_draws, g_rq_shadow_func; /* RENDER-QA3 (W100) */
static ID3D11Buffer *g_rq_xring, *g_rq_cring;
static ID3D11ShaderResourceView *g_rq_xsrv; /* SHADE-PERF (W80): Ring als Buffer<uint> (t0) */
static int g_rq_xsrv_ok = -1;               /* MapNoOverwriteOnDynamicBufferSRV */
/* SHADE-PERF (W80): Quelle der Roh-Eingaenge des aktuellen VB-Draws */
typedef struct {
    const unsigned char *vdata;
    const unsigned short *idx;
    const float (*cur)[4];
    unsigned stride, base, start, n;
    int on;     /* 1 = gesetzt (rq_vb_raw_set), gilt bis rq_vb_raw_clear */
    int use;    /* 1 = aktueller Shading-Draw nutzt die Roh-Variante */
} rq_rawsrc_t;
static rq_rawsrc_t g_rq_src;
static unsigned g_rq_rawx[4]; /* rq_raw (Basis, Stride, Prim, n) */
static unsigned g_rq_rawi;    /* rq_raw2.x (Index-Basis in DWORDs) */
static unsigned g_rq_xpos, g_rq_cpos;
static ID3D11SamplerState *g_rq_smp[6][6], *g_rq_defsmp;
static float *g_rq_pack, *g_rq_xexp;   /* je IM_VB_MAX * 16 float4 */
static float (*g_rq_vbvin)[16][4];    /* VB-Eingaenge (alle v-Register) */
static unsigned g_rq_n, g_rq_kind;
static float g_rq_tmax[4]; /* max |uv| der oTk-Quelle (Texel- oder UV-Koordinaten?) */
static rq_vsent_t *g_rq_vse;
static rq_psent_t *g_rq_pse;
unsigned g_rq_stat_draws, g_rq_stat_skip[8];
/* DIAG(SHADE-M2,W117): Grund des letzten Draws (Index wie g_rq_stat_skip,
 * 10 = nicht versucht/A-B-Bild, 9 = rq_bind fehlgeschlagen, 100 = Shading) */
unsigned g_rq_why;

int rq_in_mission(void);
static unsigned g_rq_pdchk, g_rq_pddiff, g_rq_pdlast[4]; /* DIAG(W80): PSDef-Live-Vergleich */
/* DIAG(SHADE-PERF,W80): rq_perf=1 - Abschnittszeiten (QPC) je Missions-Bild */
long long g_rqp_q[RQP_N];
unsigned g_rqp_draws, g_rqp_shaded, g_rqp_vin, g_rqp_vout, g_rqp_compiles, g_rqp_creuse;
unsigned long long g_rqp_xbytes, g_rqp_cbytes;
long long rq_qpc(void)
{
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return q.QuadPart;
}
static void rq_perf_reset(void)
{
    memset(g_rqp_q, 0, sizeof g_rqp_q);
    g_rqp_draws = g_rqp_shaded = g_rqp_vin = g_rqp_vout = g_rqp_compiles = g_rqp_creuse = 0;
    g_rqp_xbytes = g_rqp_cbytes = 0;
    memset(g_imp_q, 0, sizeof g_imp_q); /* DIAG(IM-PERF,W99) */
    g_imp_fastd = g_imp_vbv = g_imp_imv = 0;
}
/* Je Spiel-Present: Bildzeit (Present-zu-Present) aufsummieren, alle 300
 * Missions-Bilder RQPERF (ms je Bild). Ausserhalb der Mission: verwerfen. */
void rq_perf_present(unsigned f)
{
    static long long last, frame_q, pres_q0;
    static unsigned nfr;
    static LARGE_INTEGER qpf;
    long long now = rq_qpc();
    if (!qpf.QuadPart) { QueryPerformanceFrequency(&qpf); }
    if (!rq_in_mission()) { last = 0; nfr = 0; frame_q = 0; rq_perf_reset(); return; }
    if (last) { frame_q += now - last; nfr++; }
    else { rq_perf_reset(); pres_q0 = now; }
    last = now;
    if (nfr >= 300u) {
        double k = 1000.0 / (double)qpf.QuadPart / (double)nfr;
        double secs = (double)(now - pres_q0) / (double)qpf.QuadPart;
        HLOG_I("RQPERF f=%u n=%u fps=%.2f frame=%.2fms vbloop=%.2f submit=%.2f begin=%.2f exp=%.2f bind=%.2f draw=%.2f comp=%.2f cap=%.2f prep=%.2f present=%.2f ms/Bild draws=%u shaded=%u vin=%u vout=%u x=%.2fMB c=%.1fKB creuse=%u je Bild compiles=%u shading=%d",
               f, nfr, secs > 0.0 ? (double)nfr / secs : 0.0, (double)frame_q * k,
               (double)g_rqp_q[RQP_VBLOOP] * k, (double)g_rqp_q[RQP_SUBMIT] * k, (double)g_rqp_q[RQP_BEGIN] * k,
               (double)g_rqp_q[RQP_EXP] * k, (double)g_rqp_q[RQP_BIND] * k, (double)g_rqp_q[RQP_DRAW] * k,
               (double)g_rqp_q[RQP_COMPILE] * k, (double)g_rqp_q[RQP_CAP] * k,
               (double)g_rqp_q[RQP_PREP] * k, (double)g_rqp_q[RQP_PRESENT] * k,
               g_rqp_draws / nfr, g_rqp_shaded / nfr, g_rqp_vin / nfr, g_rqp_vout / nfr,
               (double)g_rqp_xbytes / (1024.0 * 1024.0) / nfr, (double)g_rqp_cbytes / 1024.0 / nfr, g_rqp_creuse / nfr,
               g_rqp_compiles, g_cfg.shading);
        HLOG_I("IMPERF f=%u pre=%.2f up=%.2f cb=%.2f st=%.2f ms/Bild fast=%u vbv=%u imv=%u je Bild im_fast=%d interp=%d p0=%.2f p1=%.2f p2=%.2f",
               f, (double)(g_imp_q[IMP_PRE] + g_imp_q[IMP_P0] + g_imp_q[IMP_P1] + g_imp_q[IMP_P2]) * k,
               (double)g_imp_q[IMP_UP] * k, (double)g_imp_q[IMP_CB] * k,
               (double)g_imp_q[IMP_ST] * k, g_imp_fastd / nfr, g_imp_vbv / nfr, g_imp_imv / nfr,
               g_cfg.im_fast, g_cfg.interp_fps, (double)g_imp_q[IMP_P0] * k, (double)g_imp_q[IMP_P1] * k,
               (double)g_imp_q[IMP_P2] * k); /* DIAG(IM-PERF,W99) */
        {   /* SHADE-PERF (W80): Combiner-Konstanten ueber den Simple-Pfad */
            extern unsigned g_rq_pbc_calls[64], g_rq_pbc_last[64];
            unsigned tot = 0, i;
            for (i = 0; i < 64u; i++) { tot += g_rq_pbc_calls[i]; }
            HLOG_I("RQPBC f=%u calls=%u F0[0..3]=%u/%u/%u/%u F1[0..1]=%u/%u FC=%u/%u last F0_0=%08X F0_1=%08X F1_0=%08X FC0=%08X FC1=%08X pbconst=%d",
                   f, tot, g_rq_pbc_calls[10], g_rq_pbc_calls[11], g_rq_pbc_calls[12], g_rq_pbc_calls[13],
                   g_rq_pbc_calls[18], g_rq_pbc_calls[19], g_rq_pbc_calls[43], g_rq_pbc_calls[44],
                   g_rq_pbc_last[10], g_rq_pbc_last[11], g_rq_pbc_last[18], g_rq_pbc_last[43], g_rq_pbc_last[44],
                   g_cfg.shade_pbconst);
            memset(g_rq_pbc_calls, 0, sizeof g_rq_pbc_calls);
        }
        if (g_cfg.shade_pbconst >= 2) { /* DIAG: Push-Buffer-Pool nach Combiner-Faktor-Methoden durchsuchen */
            extern unsigned char g_d3d_pool[8 * 1024 * 1024];
            const unsigned *w = (const unsigned *)g_d3d_pool;
            unsigned i, nf0 = 0, nf1 = 0, nfc = 0, v0 = 0, v1 = 0, vc = 0, nhdr = 0;
            for (i = 0; i + 1u < (8u << 20) / 4u; i++) {
                unsigned x = w[i], m = x & 0x1FFFu, cnt = (x >> 18) & 0x7FFu;
                if ((x & 0xE0000003u) || ((x >> 13) & 7u) || cnt < 1u || cnt > 16u) { continue; }
                if (m >= 0xA60u && m <= 0xA7Cu) { nf0++; v0 = w[i + 1]; }
                else if (m >= 0xA80u && m <= 0xA9Cu) { nf1++; v1 = w[i + 1]; }
                else if (m == 0x1E20u || m == 0x1E24u) { nfc++; vc = w[i + 1]; }
                else if (m == 0x17FCu || m == 0x1E60u) { nhdr++; }
            }
            HLOG_I("RQPBSCAN f=%u F0=%u (letzt %08X) F1=%u (%08X) FC=%u (%08X) ctl=%u", f, nf0, v0, nf1, v1, nfc, vc, nhdr);
            HLOG_I("RQPDLIVE f=%u draws=%u abweichend=%u letzt ps=0x%X idx=%u live=%08X eintrag=%08X", f, g_rq_pdchk, g_rq_pddiff,
                   g_rq_pdlast[0], g_rq_pdlast[1], g_rq_pdlast[2], g_rq_pdlast[3]);
            g_rq_pdchk = g_rq_pddiff = 0;
        }
        rq_perf_reset();
        frame_q = 0; nfr = 0; pres_q0 = now;
    }
}

static void *rq_big(size_t bytes)
{
    return VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}

float (*rq_vb_vin(void))[16][4]
{
    if (!g_rq_vbvin) { g_rq_vbvin = (float (*)[16][4])rq_big((size_t)IM_VB_MAX * 16u * 16u); }
    return g_rq_vbvin;
}

/* SHADE-PERF (W80): Dynamische Puffer als SRV mit NO_OVERWRITE (D3D11.1-Option). */
int rq_xsrv_supported(void)
{
    if (g_rq_xsrv_ok < 0) {
        D3D11_FEATURE_DATA_D3D11_OPTIONS fo;
        memset(&fo, 0, sizeof fo);
        g_rq_xsrv_ok = (g_dev && SUCCEEDED(ID3D11Device_CheckFeatureSupport(g_dev, D3D11_FEATURE_D3D11_OPTIONS, &fo, sizeof fo)) &&
                        fo.MapNoOverwriteOnDynamicBufferSRV) ? 1 : 0;
    }
    return g_rq_xsrv_ok;
}
ID3D11ShaderResourceView *rq_mksrv(ID3D11Buffer *b, unsigned bytes)
{
    D3D11_SHADER_RESOURCE_VIEW_DESC vd;
    ID3D11ShaderResourceView *v = NULL;
    memset(&vd, 0, sizeof vd);
    vd.Format = DXGI_FORMAT_R32_UINT;
    vd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    vd.Buffer.FirstElement = 0;
    vd.Buffer.NumElements = bytes / 4u;
    if (FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)b, &vd, &v))) { v = NULL; }
    return v;
}

static int rq_init(void)
{
    D3D11_BUFFER_DESC bd;
    D3D11_FEATURE_DATA_D3D11_OPTIONS fo;
    D3D11_SAMPLER_DESC sd;
    if (g_rq_inited) { return !g_rq_dead; }
    g_rq_inited = 1;
    g_rq_dead = 1;
    if (!g_dev || !g_ctx) { return 0; }
    memset(&fo, 0, sizeof fo);
    if (FAILED(ID3D11Device_CheckFeatureSupport(g_dev, D3D11_FEATURE_D3D11_OPTIONS, &fo, sizeof fo)) ||
        !fo.ConstantBufferOffsetting) {
        HLOG_W("RENDER-QA: Konstantenpuffer-Fenster (D3D11.1) fehlen -> shading aus");
        return 0;
    }
    if (FAILED(ID3D11DeviceContext_QueryInterface(g_ctx, &IID_ID3D11DeviceContext1, (void **)&g_rq_ctx1)) ||
        !g_rq_ctx1) {
        HLOG_W("RENDER-QA: ID3D11DeviceContext1 fehlt -> shading aus");
        return 0;
    }
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = RQ_XRING_BYTES;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER | (rq_xsrv_supported() ? D3D11_BIND_SHADER_RESOURCE : 0u);
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &g_rq_xring))) { return 0; }
    if (rq_xsrv_supported()) { g_rq_xsrv = rq_mksrv(g_rq_xring, RQ_XRING_BYTES); }
    bd.ByteWidth = RQ_CRING_CONST * 16u;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &g_rq_cring))) { return 0; }
    g_rq_pack = (float *)rq_big((size_t)IM_VB_MAX * 16u * 16u);
    g_rq_xexp = (float *)rq_big((size_t)IM_VB_MAX * 3u * 16u * 16u);
    if (!g_rq_pack || !g_rq_xexp) { return 0; }
    memset(&sd, 0, sizeof sd); /* = Init-Sampler des IM-Pfads (render_shader.c) */
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxAnisotropy = 1;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(ID3D11Device_CreateSamplerState(g_dev, &sd, &g_rq_defsmp))) { return 0; }
    g_rq_dead = 0;
    HLOG_I("RENDER-QA: Shading-Pfad bereit (shade_mask=0x%X, shade_scope=%d)", g_cfg.shade_mask, g_cfg.shade_scope);
    HLOG_I("SHADE-PERF: VB-Roh-Eingaenge %s (shade_gpuvb=%d, SRV-NoOverwrite=%d)",
           (g_cfg.shade_gpuvb && g_rq_xsrv) ? "GPU" : "CPU", g_cfg.shade_gpuvb, rq_xsrv_supported());
    return 1;
}

/* D3DTADDRESS (1 WRAP, 2 MIRROR, 3 CLAMP, 4 BORDER, 5 CLAMPTOEDGE; 0 = nie
 * gesetzt -> CLAMP wie der bisherige Pfad) -> Sampler (linear wie bisher). */
static ID3D11SamplerState *rq_sampler(unsigned au, unsigned av)
{
    static const D3D11_TEXTURE_ADDRESS_MODE am[6] = {
        D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_MIRROR,
        D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_BORDER, D3D11_TEXTURE_ADDRESS_CLAMP };
    if (au > 5u) { au = 0; }
    if (av > 5u) { av = 0; }
    if (!g_rq_smp[au][av]) {
        D3D11_SAMPLER_DESC sd;
        memset(&sd, 0, sizeof sd);
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = am[au]; sd.AddressV = am[av]; sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxAnisotropy = 1;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(ID3D11Device_CreateSamplerState(g_dev, &sd, &g_rq_smp[au][av]))) {
            g_rq_smp[au][av] = NULL;
            return g_rq_defsmp;
        }
    }
    return g_rq_smp[au][av];
}

static unsigned rq_u32(unsigned a)
{
    unsigned v = 0;
    __try { v = *(volatile unsigned *)(uintptr_t)a; }
    __except (EXCEPTION_EXECUTE_HANDLER) { v = 0; }
    return v;
}

/* Slots wie vsh_classify (render_shader.c) aus dem Spiel-Speicher. */
static int rq_read_slots(unsigned handle, unsigned *slots, unsigned *pn)
{
    unsigned base = handle & ~1u, nslots, nwords, si = 0, ri = 0, k;
    __try {
        unsigned *hd = (unsigned *)(uintptr_t)base;
        unsigned *rg = (unsigned *)(uintptr_t)(base + 0x114u);
        nslots = hd[2]; nwords = hd[3];
        if (nslots < 1u || nslots > 136u || nwords < 1u || nwords > 1024u) { return 0; }
        while (si < nslots && ri < nwords) {
            unsigned cn = (rg[ri] >> 16) & 0xFFu;
            ri++;
            cn = (cn >= 16u && cn <= 128u && (cn % 16u) == 0) ? (cn / 16u) : 8u;
            if (cn > nslots - si) { cn = nslots - si; }
            for (k = 0; k < cn && ri + 4u <= nwords; k++) {
                slots[si * 4u + 0] = rg[ri]; slots[si * 4u + 1] = rg[ri + 1u];
                slots[si * 4u + 2] = rg[ri + 2u]; slots[si * 4u + 3] = rg[ri + 3u];
                ri += 4u; si++;
            }
            if (k < cn) { break; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    if (si != nslots) { return 0; }
    *pn = nslots;
    return 1;
}

/* SHADE-PERF (W80): D3DCompile-Ergebnisse je Quelltext-Hash im Client-Ordner
 * (<exe>\shadercache\rq_<hash>.cso, shade_cache=1). Erster Lauf uebersetzt
 * und schreibt (Temp-Datei + Umbenennen, parallele Instanzen sicher), danach
 * nur Laden: kein Kompilier-Stocken beim ersten Auftreten eines Programms. */
typedef HRESULT(WINAPI *rq_fn_createblob)(SIZE_T, ID3DBlob **);
static int rq_cache_path(const char *src, const char *target, char *path, size_t pn)
{
    static char dir[MAX_PATH];
    static int have = -1;
    unsigned long long h = 14695981039346656037ull;
    const char *q;
    size_t len = 0;
    if (have < 0) {
        char *sl;
        have = 0;
        if (sb_userdata_path(SB_UD_SHADERCACHE, dir, sizeof dir) == 1) {
            have = 1; /* W150 USER-DATA: Profil/portabel */
        } else if (GetModuleFileNameA(NULL, dir, sizeof dir) && (sl = strrchr(dir, '\\')) != NULL) {
            *sl = 0;
            if (strlen(dir) + 16u < sizeof dir) {
                strcat_s(dir, sizeof dir, "\\shadercache");
                CreateDirectoryA(dir, NULL);
                have = 1;
            }
        }
    }
    if (!have) { return 0; }
    for (q = target; *q; q++) { h = (h ^ (unsigned char)*q) * 1099511628211ull; }
    for (q = src; *q; q++) { h = (h ^ (unsigned char)*q) * 1099511628211ull; len++; }
    _snprintf_s(path, pn, _TRUNCATE, "%s\\rq_%016llx_%u.cso", dir, h, (unsigned)len);
    return 1;
}

/* DIAG(PERF-BENCH,W137): Shader-Erstellung je Teil (QPC), Zeile SHADE-PERF Zeit */
long long g_shz_comp, g_shz_all, g_shz_gen;
unsigned g_shz_n;
static int rq_compile_x(const char *src, const char *target, ID3DBlob **out);
static int rq_compile(const char *src, const char *target, ID3DBlob **out)
{
    long long q = rq_qpc();
    int r = rq_compile_x(src, target, out);
    g_shz_comp += rq_qpc() - q;
    return r;
}
static int rq_compile_x(const char *src, const char *target, ID3DBlob **out)
{
    char path[MAX_PATH + 64];
    static rq_fn_createblob cb;
    static int ncache_hit, ncache_new;
    if (g_cfg.shade_cache && rq_cache_path(src, target, path, sizeof path)) {
        HANDLE f;
        if (!cb) {
            HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
            if (dc) { cb = (rq_fn_createblob)GetProcAddress(dc, "D3DCreateBlob"); }
        }
        /* W139 PERF-WARM (warm_shaders): Blob aus dem Vorrat des Hilfs-Threads statt von der Platte */
        f = sb_warm_blob(path, cb, out) ? NULL : CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD sz, rd = 0;
            ID3DBlob *b = NULL;
            int ok = 0;
            if (!f) { ok = 1; b = *out; }
            else {
                sz = GetFileSize(f, NULL);
                if (cb && sz > 0 && sz < (8u << 20) && SUCCEEDED(cb(sz, &b)) && b) {
                    ok = ReadFile(f, b->lpVtbl->GetBufferPointer(b), sz, &rd, NULL) && rd == sz;
                }
                CloseHandle(f);
            }
            if (ok) {
                *out = b;
                ncache_hit++;
                if ((ncache_hit % 32) == 0) { /* DIAG(PERF-BENCH,W137): Zeitaufteilung auch bei warmem Cache */
                    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
                    HLOG_I("SHADE-PERF Zeit: n=%u gesamt=%.1fms davon uebersetzen/laden=%.1fms quelltext=%.1fms (Rest = D3D-Erstellung + Layouts), Cache %d geladen",
                           g_shz_n, (double)g_shz_all * 1000.0 / (double)f.QuadPart, (double)g_shz_comp * 1000.0 / (double)f.QuadPart,
                           (double)g_shz_gen * 1000.0 / (double)f.QuadPart, ncache_hit);
                    sb_warm_stat_log(); /* W139 */
                }
                return 1;
            }
            if (b) { b->lpVtbl->Release(b); }
        }
        if (!sb_im_compile(src, target, out)) { return 0; }
        {
            char tmp[MAX_PATH + 80];
            HANDLE w;
            _snprintf_s(tmp, sizeof tmp, _TRUNCATE, "%s.%lu.tmp", path, (unsigned long)GetCurrentProcessId());
            w = CreateFileA(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
            if (w != INVALID_HANDLE_VALUE) {
                DWORD wr = 0, sz = (DWORD)(*out)->lpVtbl->GetBufferSize(*out);
                BOOL okw = WriteFile(w, (*out)->lpVtbl->GetBufferPointer(*out), sz, &wr, NULL) && wr == sz;
                CloseHandle(w);
                if (!okw || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) { DeleteFileA(tmp); }
                else { ncache_new++; }
            }
        }
        if (((ncache_new + ncache_hit) % 16) == 1) {
            HLOG_I("SHADE-PERF: Shader-Cache %d geladen, %d neu (%s)", ncache_hit, ncache_new, path);
            { LARGE_INTEGER f; QueryPerformanceFrequency(&f); /* DIAG(PERF-BENCH,W137) */
              HLOG_I("SHADE-PERF Zeit: n=%u gesamt=%.1fms davon uebersetzen/laden=%.1fms quelltext=%.1fms (Rest = D3D-Erstellung + Layouts)",
                     g_shz_n, (double)g_shz_all * 1000.0 / (double)f.QuadPart, (double)g_shz_comp * 1000.0 / (double)f.QuadPart,
                     (double)g_shz_gen * 1000.0 / (double)f.QuadPart); }
        }
        return 1;
    }
    return sb_im_compile(src, target, out);
}

static int rq_mklayout(const rq_vsent_t *e, int lerp, ID3DBlob *b, ID3D11InputLayout **out, int raw)
{
    D3D11_INPUT_ELEMENT_DESC el[4 + 16];
    unsigned ne = 0, j, nv = raw ? 0u : e->nv;
    memset(el, 0, sizeof el);
    el[ne].SemanticName = "POSITION"; el[ne].Format = DXGI_FORMAT_R32G32B32A32_FLOAT; el[ne].AlignedByteOffset = 0; ne++;
    el[ne].SemanticName = "COLOR"; el[ne].Format = DXGI_FORMAT_R32G32B32A32_FLOAT; el[ne].AlignedByteOffset = 16; ne++;
    el[ne].SemanticName = "TEXCOORD"; el[ne].Format = DXGI_FORMAT_R32G32_FLOAT; el[ne].AlignedByteOffset = 32; ne++;
    if (lerp) {
        el[ne].SemanticName = "POSITION"; el[ne].SemanticIndex = 1; el[ne].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        el[ne].InputSlot = 1; ne++;
    }
    for (j = 0; j < nv; j++) {
        el[ne].SemanticName = "ATTR"; el[ne].SemanticIndex = j; el[ne].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        el[ne].InputSlot = 2; el[ne].AlignedByteOffset = j * 16u; ne++;
    }
    return SUCCEEDED(ID3D11Device_CreateInputLayout(g_dev, el, ne, b->lpVtbl->GetBufferPointer(b),
                                                    b->lpVtbl->GetBufferSize(b), out));
}

static void rq_raw_release(rq_vsent_t *e)
{
    if (e->vsr) { ID3D11VertexShader_Release(e->vsr); }
    if (e->vsrl) { ID3D11VertexShader_Release(e->vsrl); }
    if (e->layr) { ID3D11InputLayout_Release(e->layr); }
    if (e->layrl) { ID3D11InputLayout_Release(e->layrl); }
    e->vsr = e->vsrl = NULL;
    e->layr = e->layrl = NULL;
    e->rawok = 0;
    e->rawsig = 0;
}

/* SHADE-PERF (W80): Roh-Variante fuer die Decl dieses Handles (Stream 0,
 * Stride des Draws). 1 = bereit. */
static int rq_raw_build(rq_vsent_t *e, unsigned handle, const unsigned *st, const unsigned *of,
                        const unsigned *fm, unsigned sig);
static int rq_raw_variant(rq_vsent_t *e, unsigned handle)
{
    unsigned st[16], of[16], fm[16], s, sig = 2166136261u;
    __try {
        unsigned base = handle & ~1u;
        for (s = 0; s < 16u; s++) {
            const unsigned *sl = (const unsigned *)(uintptr_t)(base + 0x14u + s * 16u);
            st[s] = sl[0]; of[s] = sl[1]; fm[s] = sl[2] & 0xFFu;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    for (s = 0; s < 16u; s++) {
        if (!(e->prog.vinmask & (1u << s))) { continue; }
        sig = (sig ^ s) * 16777619u; sig = (sig ^ st[s]) * 16777619u;
        sig = (sig ^ of[s]) * 16777619u; sig = (sig ^ fm[s]) * 16777619u;
    }
    sig |= 1u;
    if (e->rawsig == sig) { return e->rawok > 0; }
    /* W139 PERF-WARM: Die Roh-Variante entsteht beim ersten VB-Draw mitten im Submit
     * (rq_vb_raw_set) - bisher unter ctx_lock: kalter Cache 30-75 ms D3DCompile je
     * Programm, warm Laden + Erstellen (W139_c0m1: Sperr-Regionen 11-63 ms ab
     * sb_render_vb_draw). Wie rq_vs_get_slow (W138) ohne ctx_lock (nur mit Takt-Thread). */
    {
        int cr = sb_ctx_release_all(), r = rq_raw_build(e, handle, st, of, fm, sig);
        sb_ctx_reacquire(cr);
        return r;
    }
}
static int rq_raw_build(rq_vsent_t *e, unsigned handle, const unsigned *st, const unsigned *of,
                        const unsigned *fm, unsigned sig)
{
    static char src[96 * 1024];
    unsigned char cof[RQV_NCONST];
    unsigned nc = 0, s;
    ID3DBlob *b = NULL, *bl = NULL;
    long long q0;
    rq_raw_release(e);
    e->rawsig = sig;
    e->rawok = -1;
    if (!rqv_raw_build(st, of, fm, e->prog.vinmask, &e->raw)) { return 0; }
    q0 = rq_qpc(); /* W137: immer (einmal je Variante) */
    g_shz_n++;
    if (!rqv_hlsl_ex(&e->prog, 0, &e->raw, src, sizeof src, cof, &nc) || nc != e->nc || !rq_compile(src, "vs_4_0", &b)) {
        HLOG_W("SHADE-PERF: VS 0x%X Roh-Variante nicht uebersetzbar -> CPU-Packen", handle);
        return 0;
    }
    if (!rqv_hlsl_ex(&e->prog, 1, &e->raw, src, sizeof src, cof, &nc) || !rq_compile(src, "vs_4_0", &bl)) {
        b->lpVtbl->Release(b);
        return 0;
    }
    if (FAILED(sb_warm_create_vs(b->lpVtbl->GetBufferPointer(b), b->lpVtbl->GetBufferSize(b), &e->vsr)) ||
        FAILED(sb_warm_create_vs(bl->lpVtbl->GetBufferPointer(bl), bl->lpVtbl->GetBufferSize(bl), &e->vsrl)) ||
        !rq_mklayout(e, 0, b, &e->layr, 1) || !rq_mklayout(e, 1, bl, &e->layrl, 1)) {
        b->lpVtbl->Release(b); bl->lpVtbl->Release(bl);
        rq_raw_release(e);
        e->rawsig = sig; e->rawok = -1;
        return 0;
    }
    b->lpVtbl->Release(b); bl->lpVtbl->Release(bl);
    g_shz_all += rq_qpc() - q0; /* DIAG(PERF-BENCH,W137) */
    if (g_cfg.rq_perf) { g_rqp_q[RQP_COMPILE] += rq_qpc() - q0; g_rqp_compiles++; }
    e->rawok = 1;
    {
        char fb[160];
        int o = 0;
        for (s = 0; s < 16u && o >= 0 && o < 140; s++) {
            if (e->prog.vinmask & (1u << s)) {
                o += _snprintf_s(fb + o, sizeof fb - (size_t)o, _TRUNCATE, "v%u=%02X@%u ", s, e->raw.fmt[s], e->raw.off[s]);
            }
        }
        HLOG_I("SHADE-PERF: VS 0x%X Roh-Variante need=%u inl=%u %s", handle, e->raw.need, e->raw.ninl, fb);
    }
    return 1;
}

/* Gepackte Variante (Stream 2 = v-Register je Vertex, W77-Weg). 1 = bereit. */
static int rq_packed_build(rq_vsent_t *e);
static int rq_packed_variant(rq_vsent_t *e)
{
    int cr, r;
    if (e->pkok) { return e->pkok > 0; }
    /* W139 PERF-WARM: erste gepackte Variante (IM-Draw, rq_prepare) ohne ctx_lock wie die Roh-Variante */
    cr = sb_ctx_release_all();
    r = rq_packed_build(e);
    sb_ctx_reacquire(cr);
    return r;
}
static int rq_packed_build(rq_vsent_t *e)
{
    static char src[96 * 1024];
    unsigned char cof[RQV_NCONST];
    unsigned nc = 0;
    ID3DBlob *b = NULL, *bl = NULL;
    long long q0;
    e->pkok = -1;
    q0 = rq_qpc(); /* W137: immer (einmal je Programm) */
    g_shz_n++;
    if (!rqv_hlsl(&e->prog, 0, src, sizeof src, cof, &nc) || !rq_compile(src, "vs_4_0", &b)) { return 0; }
    if (!rqv_hlsl(&e->prog, g_cfg.interp_eyefix ? 2 : 1, src, sizeof src, cof, &nc) || /* W224 */
        !rq_compile(src, "vs_4_0", &bl)) {
        b->lpVtbl->Release(b);
        return 0;
    }
    if (FAILED(sb_warm_create_vs(b->lpVtbl->GetBufferPointer(b), b->lpVtbl->GetBufferSize(b), &e->vs)) ||
        FAILED(sb_warm_create_vs(bl->lpVtbl->GetBufferPointer(bl), bl->lpVtbl->GetBufferSize(bl), &e->vsl)) ||
        !rq_mklayout(e, 0, b, &e->lay, 0) || !rq_mklayout(e, 1, bl, &e->layl, 0)) {
        b->lpVtbl->Release(b); bl->lpVtbl->Release(bl);
        return 0;
    }
    b->lpVtbl->Release(b); bl->lpVtbl->Release(bl);
    g_shz_all += rq_qpc() - q0; /* DIAG(PERF-BENCH,W137) */
    if (g_cfg.rq_perf) { g_rqp_q[RQP_COMPILE] += rq_qpc() - q0; g_rqp_compiles++; }
    e->pkok = 1;
    return 1;
}

static rq_vsent_t *rq_vs_get_slow0(unsigned handle);
static rq_vsent_t *rq_vs_get_slow(unsigned handle)
{
    long long q0 = g_cfg.rq_perf ? rq_qpc() : 0;
    unsigned nc = g_rqvs_n;
    int cr = sb_ctx_release_all(); /* W138 present_decouple: Laden/Erstellen ohne ctx_lock (Device-Aufrufe) */
    rq_vsent_t *e = rq_vs_get_slow0(handle);
    sb_ctx_reacquire(cr);
    if (g_cfg.rq_perf && g_rqvs_n != nc) { g_rqp_q[RQP_COMPILE] += rq_qpc() - q0; g_rqp_compiles++; }
    return e;
}
/* Schnellweg: gleicher Handle wie zuletzt im selben Bild -> letzter Eintrag
 * (je Bild einmal voll geprueft: vsh_program-Wache + Hash). */
/* SHADE-PERF (W80): je Handle (direkt abgebildet, 64 Plaetze) statt nur des
 * letzten Handles - Draws wechseln die Programme oft. */
static rq_vsent_t *rq_vs_get(unsigned handle)
{
    static struct { unsigned h, f; rq_vsent_t *e; } c[64];
    unsigned s = (handle ^ (handle >> 7)) & 63u;
    if (c[s].h == handle && c[s].f == g_present_no) { return c[s].e; }
    c[s].e = rq_vs_get_slow(handle);
    c[s].h = handle; c[s].f = g_present_no;
    return c[s].e;
}
static rq_vsent_t *rq_vs_get_slow0(unsigned handle)
{
    const vsh_prog_t *vp = vsh_program(handle);
    rq_vsent_t *e = NULL;
    unsigned i, slots[136 * 4], n = 0, k;
    static char src[96 * 1024];
    ID3DBlob *b = NULL, *bl = NULL;
    if (!vp) { return NULL; }
    for (i = 0; i < g_rqvs_n; i++) {
        if (g_rqvs[i].handle == handle) {
            if (g_rqvs[i].hash == vp->hash) { return g_rqvs[i].ok ? &g_rqvs[i] : NULL; }
            e = &g_rqvs[i];
            break;
        }
    }
    if (!e) {
        if (g_rqvs_n >= RQ_VS_MAX) { return NULL; }
        e = &g_rqvs[g_rqvs_n++];
    } else {
        if (e->vs) { ID3D11VertexShader_Release(e->vs); }
        if (e->vsl) { ID3D11VertexShader_Release(e->vsl); }
        if (e->lay) { ID3D11InputLayout_Release(e->lay); }
        if (e->layl) { ID3D11InputLayout_Release(e->layl); }
        rq_raw_release(e);
    }
    memset(e, 0, sizeof *e);
    e->handle = handle;
    e->hash = vp->hash;
    if (!rq_read_slots(handle, slots, &n) || !rqv_decode(slots, n, &e->prog)) {
        HLOG_W("RENDER-QA: VS 0x%X nicht dekodierbar -> alter Pfad", handle);
        return NULL;
    }
    for (k = 0; k < 16u; k++) {
        if (e->prog.vinmask & (1u << k)) { e->vreg[e->nv++] = (unsigned char)k; }
    }
    {
        int q;
        for (k = 0; k < 4u; k++) { e->tsrc[k] = -1; }
        for (q = 0; q < e->prog.nops; q++) {
            const rqv_op_t *o = &e->prog.ops[q];
            unsigned a = o->oaddr & 0xFu;
            if (o->omask && o->orb && a >= 9u && a <= 12u) {
                e->tsrc[a - 9u] = (!o->omux && (o->mac == 1 || o->mac == 3) && o->in[0].mux == 2) ? (int)o->vin : -1;
            }
        }
    }
    if (!rqv_hlsl(&e->prog, 0, src, sizeof src, e->cidx, &e->nc)) {
        HLOG_W("RENDER-QA: VS 0x%X Uebersetzung fehlgeschlagen", handle);
        return NULL;
    }
    /* SHADE-PERF (W80): shade_gpuvb=1 -> gepackte Variante (IM-Draws) erst bei
     * Bedarf (rq_packed_variant); VB-Draws nutzen die Roh-Variante. */
    if (!g_cfg.shade_gpuvb && !rq_packed_variant(e)) {
        HLOG_W("RENDER-QA: VS 0x%X D3D-Objekte fehlgeschlagen", handle);
        return NULL;
    }
    e->ok = 1;
    e->kind = vp->kind;
    HLOG_I("RENDER-QA: VS 0x%X hash=%08X slots=%u vin=0x%X out=0x%X konst=%u kind=%d", handle, e->hash, n,
           e->prog.vinmask, e->prog.outmask, e->nc, vp->kind);
    return e;
}

static rq_psent_t *rq_ps_get_slow0(unsigned handle);
static rq_psent_t *rq_ps_get_slow(unsigned handle)
{
    long long q0 = g_cfg.rq_perf ? rq_qpc() : 0;
    unsigned nc = g_rqps_n;
    int cr = sb_ctx_release_all(); /* W138 present_decouple: Laden/Erstellen ohne ctx_lock (Device-Aufrufe) */
    rq_psent_t *e = rq_ps_get_slow0(handle);
    sb_ctx_reacquire(cr);
    if (g_cfg.rq_perf && g_rqps_n != nc) { g_rqp_q[RQP_COMPILE] += rq_qpc() - q0; g_rqp_compiles++; }
    return e;
}
static rq_psent_t *rq_ps_get(unsigned handle)
{
    static struct { unsigned h, f; rq_psent_t *e; } c[64];
    unsigned s = (handle ^ (handle >> 7)) & 63u;
    if (c[s].h == handle && c[s].f == g_present_no) { return c[s].e; }
    c[s].e = rq_ps_get_slow(handle);
    c[s].h = handle; c[s].f = g_present_no;
    return c[s].e;
}
static rq_psent_t *rq_ps_get_slow0(unsigned handle)
{
    unsigned d[60], i, pdef = 0;
    rq_psent_t *e = NULL;
    static char src[96 * 1024];
    ID3DBlob *b = NULL;
    __try {
        pdef = *(unsigned *)(uintptr_t)((handle & ~1u) + 8u);
        if (pdef < 0x10000u || pdef >= 0x80000000u) { return NULL; }
        memcpy(d, (const void *)(uintptr_t)pdef, sizeof d);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
    for (i = 0; i < g_rqps_n; i++) {
        if (g_rqps[i].handle == handle) {
            if (!memcmp(g_rqps[i].d, d, sizeof d)) { return g_rqps[i].ok ? &g_rqps[i] : NULL; }
            e = &g_rqps[i];
            break;
        }
    }
    if (!e) {
        if (g_rqps_n >= RQ_PS_MAX) { return NULL; }
        e = &g_rqps[g_rqps_n++];
    } else if (e->ps) {
        ID3D11PixelShader_Release(e->ps);
    }
    memset(e, 0, sizeof *e);
    e->handle = handle;
    memcpy(e->d, d, sizeof d);
    { long long qg = rq_qpc(); int okg; g_shz_n++; /* DIAG(PERF-BENCH,W137) */
      okg = rqp_hlsl(d, 0xF, src, sizeof src, &e->info);
      g_shz_gen += rq_qpc() - qg;
      if (okg && rq_compile(src, "ps_4_0", &b)) {
          if (SUCCEEDED(sb_warm_create_ps(b->lpVtbl->GetBufferPointer(b), b->lpVtbl->GetBufferSize(b), &e->ps))) {
              e->ok = 2; /* gleich unten auf 1 */
          }
          b->lpVtbl->Release(b);
          b = NULL;
      }
      g_shz_all += rq_qpc() - qg;
      if (!okg) {
          HLOG_W("RENDER-QA: PS 0x%X nicht unterstuetzt (Stufe %d) -> alter Pfad", handle, e->info.unsupported);
          return NULL;
      }
      if (e->ok != 2) { return NULL; } /* Uebersetzen/Erstellen fehlgeschlagen (wie bisher) */
    }
    (void)b;
    e->ok = 1;
    HLOG_I("RENDER-QA: PS 0x%X stufen=%u texmodes=%u/%u/%u/%u final=%s", handle, e->info.nstages,
           e->info.texmode[0], e->info.texmode[1], e->info.texmode[2], e->info.texmode[3],
           e->info.final_default ? "R0" : "ABCD");
    return e;
}

/* DIAG(RENDER-QA3,W100) shade_ab=1: Shading nur im Bild direkt nach jedem
 * Screenshot-Bild (Present-Nr. sf + k*every + 1, wird zusaetzlich
 * geschossen) -> Bildpaare alt/neu mit 1 Tick Abstand aus EINEM Lauf.
 * Waehrend Bild p gezeichnet wird, steht g_present_no auf p - 1. */
int rq_ab_on(void)
{
    unsigned p = g_present_no + 1u, sf = (unsigned)g_cfg.screenshot_frame, ev = (unsigned)g_cfg.screenshot_every;
    if (!sf || !ev || p < sf) { return 0; }
    return ((p - sf) % ev) == 1u;
}

/* DIAG(SHADE-BLOOM,W112) shade_ab=2: Shading in allen Bildern AUSSER dem
 * Screenshot-Bild (Present-Nr. sf + k*every) -> Partnerbild +1 zeigt den
 * eingeschwungenen Shading-Zustand (Textur-Cache, zeitlich gemischte
 * Bloom-RTs), das Screenshot-Bild den alten Pfad. 1 = W100 (nur +1). */
int rq_ab_base(void)
{
    unsigned p = g_present_no + 1u, sf = (unsigned)g_cfg.screenshot_frame, ev = (unsigned)g_cfg.screenshot_every;
    if (!sf || !ev || p < sf) { return 0; }
    return ((p - sf) % ev) == 0u;
}
int mfx_ab_phase(void); /* render_mfx.c */
static int rq_ab_skip(void)
{
    /* DIAG(SHADE-FX,W121) shade_ab=3 (+ mfx_ab=1, mfx_shots): Paare an Korn-/
     * Stoerzeilen-Bildern - Bild p+1 alter Pfad, p+2 Shading, sonst Shading. */
    if (g_cfg.shade_ab == 3) { return mfx_ab_phase() == 1; }
    return g_cfg.shade_ab == 2 ? rq_ab_base() : !rq_ab_on();
}

int rq_in_mission(void)
{
    return rq_u32(M3D_SCENE_ADDR) == 0x2Cu && rq_u32(M3D_MLOOP_ADDR) == 1u;
}

/* HLE-Luecke (shade_cfix=1): ein Combiner-Faktor ist per PSC0/PSC1-Mapping auf
 * ein Konstantenregister gelegt, das Spiel liefert bei uns aber keinen Wert
 * (weder SetPixelShaderConstant noch SetRenderState seit SetPixelShader;
 * der Weg im Original ist offen), der Def-Wert ist 0 und die Konstante wird
 * als Faktor benutzt (Himmel 0xAB7FFC0: R0 = C0 * Wolken, sonst schwarz bzw.
 * mit 1 weiss/deckend). Dann zeichnet dieser Draw wie bisher (alter Pfad).
 * Additiv benutzte Konstanten (Cockpit V0 + C0) bleiben 0. */
static int rq_const_unknown(const rq_psent_t *pe)
{
    const unsigned *d = pe->d;
    int uniq0 = (d[53] & 0x100u) != 0, uniq1 = (d[53] & 0x1000u) != 0;
    unsigned k, q;
    for (k = 0; k < 8u; k++) {
        unsigned m0 = 0, m1 = 0;
        unsigned r0 = (d[57] >> (k * 4u)) & 0xFu, r1 = (d[58] >> (k * 4u)) & 0xFu;
        for (q = 0; q < 8u; q++) {
            if (uniq0 ? q == k : k == 0u) { m0 |= pe->info.cmul[q] & 1u; }
            if (uniq1 ? q == k : k == 0u) { m1 |= pe->info.cmul[q] & 2u; }
        }
        if (m0 && r0 < 8u && d[10 + k] == 0u && !(g_rq_psconst_set & (1u << r0)) &&
            !(g_rq_rs_set & (1ull << (10u + k)))) { return 1; }
        if (m1 && r1 < 8u && d[18 + k] == 0u && !(g_rq_psconst_set & (1u << r1)) &&
            !(g_rq_rs_set & (1ull << (18u + k)))) { return 1; }
    }
    return 0;
}

/* Vor allen Positions-Umrechnungen in sb_im_submit: Eignung + Eingaenge
 * packen (vin = g_im_buf bzw. VB-Dekodierung, je Vertex 16 v-Register). */
int rq_begin(const float (*vin)[16][4], unsigned n, int vb)
{
    unsigned i, j, nv;
    int raw = vb && g_rq_src.on;

    g_rq_active = 0;
    g_rq_src.use = 0;
    g_rq_why = 10u; /* DIAG(SHADE-M2,W117) */
    if (!g_cfg.shading || g_rq_dead || !n || (!vin && !raw)) { return 0; }
    if (g_cfg.shade_ab && rq_ab_skip()) { return 0; } /* DIAG(W100/W112): A/B-Bildpaare */
    if (!rq_init()) { return 0; }
    if (g_cfg.shade_scope == 0 && !rq_in_mission()) { g_rq_stat_skip[0]++; g_rq_why = 0u; return 0; }
    if (rs_cmask == 0u) { g_rq_stat_skip[1]++; g_rq_why = 1u; return 0; } /* reiner Tiefenpass */
    if (g_im_shader < 0x10000u || !g_im_pshader) { g_rq_stat_skip[2]++; g_rq_why = 2u; return 0; }
    g_rq_vse = rq_vs_get(g_im_shader);
    if (!g_rq_vse) { g_rq_stat_skip[3]++; g_rq_why = 3u; return 0; }
    /* shade_mask Bit 16 aus (Default): 2D-Kompositionen (P1-Programme,
     * oPos = v0: HUD, Karte, Filter, Monitor-Komposition) wie bisher - ihre
     * Laufzeit-Konstanten fehlen uns (Karte sonst weiss). */
    if (g_rq_vse->kind == 1 && !(g_cfg.shade_mask & 16)) { g_rq_stat_skip[7]++; g_rq_why = 7u; return 0; }
    g_rq_pse = rq_ps_get(g_im_pshader);
    if (!g_rq_pse) { g_rq_stat_skip[4]++; g_rq_why = 4u; return 0; }
    if (g_cfg.shade_cfix && rq_const_unknown(g_rq_pse)) { g_rq_stat_skip[6]++; g_rq_why = 6u; return 0; }
    /* shade_mask Bit 8 aus: lange Combiner-Ketten (>= 5 Stufen: Monitor-
     * Komposition 0xAB80950, Filter 0xAB801E0) wie bisher zeichnen. */
    if (!(g_cfg.shade_mask & 8) && g_rq_pse->info.nstages >= 5u) { g_rq_stat_skip[7]++; g_rq_why = 7u; return 0; }
    if (n > IM_VB_MAX) { n = IM_VB_MAX; }
    nv = g_rq_vse->nv;
    if (raw) { /* SHADE-PERF (W80): GPU dekodiert, Texel-Skala bei Bedarf in rq_bind */
        if (!nv || g_rq_vse->rawok <= 0) { g_rq_why = 11u; return 0; }
        g_rq_src.use = 1;
        if (g_rq_src.n > n) { g_rq_src.n = n; }
        for (j = 0; j < 4u; j++) { g_rq_tmax[j] = -1.0f; }
        g_rq_n = n;
        g_rq_kind = (unsigned)g_rq_vse->kind;
        g_rq_active = 1;
        g_rq_why = 100u;
        return 1;
    }
    if (!rq_packed_variant(g_rq_vse)) { g_rq_stat_skip[3]++; g_rq_why = 3u; return 0; }
    for (i = 0; i < n; i++) {
        float *dst = g_rq_pack + (size_t)i * nv * 4u;
        for (j = 0; j < nv; j++) { memcpy(dst + j * 4u, vin[i][g_rq_vse->vreg[j]], 16); }
    }
    for (j = 0; j < 4u; j++) {
        int r = g_rq_vse->tsrc[j];
        float mx = 0.0f;
        if (r >= 0) {
            for (i = 0; i < n; i++) {
                float a = vin[i][r][0] < 0 ? -vin[i][r][0] : vin[i][r][0];
                float b = vin[i][r][1] < 0 ? -vin[i][r][1] : vin[i][r][1];
                if (a > mx) { mx = a; }
                if (b > mx) { mx = b; }
            }
        }
        g_rq_tmax[j] = mx;
    }
    g_rq_n = n;
    g_rq_kind = (unsigned)g_rq_vse->kind;
    g_rq_active = 1;
    g_rq_why = 100u;
    return 1;
}

/* Nach der Primitiv-Expansion in sb_im_submit: dieselbe Abbildung auf die
 * gepackten Eingaenge anwenden (Spiegel der switch-Faelle dort) und Stream 2
 * hochladen. */
/* Anzahl der expandierten Vertices (gleiche Schleifen wie sb_im_submit). */
static unsigned rq_expand_count(unsigned prim, unsigned n)
{
    unsigned e = 0, i;
    switch (prim) {
    case 3: e = n; if (n > 0 && n + 1 <= IM_VB_MAX) { e++; } break;
    case 6: case 7: case 10: for (i = 2; i < n && e + 3 <= IM_VB_MAX * 3u; i++) { e += 3; } break;
    case 8: for (i = 0; i + 3 < n && e + 6 <= IM_VB_MAX * 3u; i += 4) { e += 6; } break;
    case 9: for (i = 0; i + 3 < n && e + 6 <= IM_VB_MAX * 3u; i += 2) { e += 6; } break;
    default: e = n; break;
    }
    return e > IM_VB_MAX ? IM_VB_MAX : e;
}

/* Stream-2-/Rohdaten-Platz: Tick-Puffer (Aufnahme) bzw. Ring. NULL = kein Platz. */
static unsigned char *rq_xmap(unsigned bytes, ID3D11Buffer **buf, UINT *off, ID3D11ShaderResourceView **srv)
{
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_MAP mt = D3D11_MAP_WRITE_NO_OVERWRITE;
    unsigned char *p;
    if (g_ip_recording) {
        p = (unsigned char *)sb_interp_rq_xmap(bytes, buf, off, srv);
        if (p) { return p; }
    }
    if (bytes > RQ_XRING_BYTES) { return NULL; }
    if (g_rq_xpos + bytes > RQ_XRING_BYTES || g_rq_xpos == 0) { g_rq_xpos = 0; mt = D3D11_MAP_WRITE_DISCARD; }
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_rq_xring, 0, mt, 0, &m))) { return NULL; }
    *buf = g_rq_xring;
    *off = g_rq_xpos;
    *srv = g_rq_xsrv;
    p = (unsigned char *)m.pData + g_rq_xpos;
    g_rq_xpos += (bytes + 255u) & ~255u;
    return p;
}

/* SHADE-PERF (W80): Rohdaten des VB-Draws hochladen (ein Map): nicht
 * indiziert = zusammenhaengender Bereich; indiziert = Bereich [min..max] +
 * Indexliste (uint32, relativ), bei duennem Bereich je Index gesammelt. */
static int rq_raw_upload(unsigned prim, unsigned n)
{
    const rq_rawsrc_t *r = &g_rq_src;
    unsigned st = r->stride, i, lo = 0, hi = 0, rawb, idxb = 0, mode = 0; /* 0 Bereich, 1 Bereich+Index, 2 gesammelt */
    unsigned char *p;
    ID3D11Buffer *buf = NULL;
    ID3D11ShaderResourceView *srv = NULL;
    UINT off = 0;
    int ok = 1;
    if (!n) { return 0; }
    if (r->idx) {
        __try {
            lo = 0xFFFFFFFFu; hi = 0;
            for (i = 0; i < n; i++) {
                unsigned v = (unsigned)r->idx[i] + r->base;
                if (v < lo) { lo = v; }
                if (v > hi) { hi = v; }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
        mode = ((hi - lo + 1u) <= 2u * n + 256u) ? 1u : 2u;
    } else {
        lo = r->start;
    }
    rawb = (mode == 1u ? (hi - lo + 1u) : n) * st;
    rawb = (rawb + 15u) & ~15u;
    if (mode == 1u) { idxb = n * 4u; }
    p = rq_xmap(rawb + idxb, &buf, &off, &srv);
    if (!p) { return 0; }
    if (!srv) { ok = 0; }
    else {
        __try {
            if (mode == 2u) {
                for (i = 0; i < n; i++) { memcpy(p + (size_t)i * st, r->vdata + (size_t)((unsigned)r->idx[i] + r->base) * st, st); }
            } else {
                memcpy(p, r->vdata + (size_t)lo * st, (size_t)(mode == 1u ? (hi - lo + 1u) : n) * st);
            }
            if (mode == 1u) {
                unsigned *ix = (unsigned *)(p + rawb);
                for (i = 0; i < n; i++) { ix[i] = (unsigned)r->idx[i] + r->base - lo; }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    }
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)buf, 0);
    if (!ok) { return 0; }
    g_rqp_xbytes += rawb + idxb; /* DIAG(SHADE-PERF,W80) */
    g_rq_rec.xbuf = buf;
    g_rq_rec.xoff = off;
    g_rq_rec.xstride = 16u;
    g_rq_rec.xsrv = srv;
    g_rq_rawx[0] = off; g_rq_rawx[1] = st; g_rq_rawx[2] = prim; g_rq_rawx[3] = n;
    g_rq_rawi = (mode == 1u) ? (off + rawb) / 4u : 0xFFFFFFFFu;
    return 1;
}

int rq_expand_upload(unsigned prim, unsigned n, unsigned nout)
{
    unsigned nv, stride, e = 0, i, bytes;
    const float *P = g_rq_pack;
    float *X;
    ID3D11Buffer *xb = NULL;
    ID3D11ShaderResourceView *xs = NULL;
    UINT xo = 0;
    if (!g_rq_active) { return 0; }
    nv = g_rq_vse->nv;
    stride = nv * 16u;
    if (n > g_rq_n) { n = g_rq_n; }
    if (rq_expand_count(prim, n) != nout) { g_rq_active = 0; g_rq_stat_skip[5]++; g_rq_why = 5u; return 0; } /* Abbildung weicht ab -> alter Pfad */
    if (g_rq_src.use) {
        if (!rq_raw_upload(prim, n)) { g_rq_active = 0; g_rq_why = 12u; return 0; }
        return 1;
    }
    g_rq_rec.xsrv = NULL;
    if (!g_cfg.shade_gpuvb) { X = g_rq_xexp; } /* W77-Weg: erst expandieren, dann kopieren */
    else { /* SHADE-PERF (W80): direkt in den Puffer expandieren */
        X = (float *)rq_xmap(stride ? nout * stride : 16u, &xb, &xo, &xs);
        if (!X) { g_rq_active = 0; return 0; }
    }
#define RQ_CP(src) do { if (nv) { memcpy(X + (size_t)e * nv * 4u, P + (size_t)(src) * nv * 4u, stride); } e++; } while (0)
    switch (prim) {
    case 3: /* LINELOOP */
        for (i = 0; i < n; i++) { RQ_CP(i); }
        if (n > 0 && n + 1 <= IM_VB_MAX) { RQ_CP(0); }
        break;
    case 6: case 7: case 10:
        for (i = 2; i < n && e + 3 <= IM_VB_MAX * 3u; i++) {
            if (prim == 6) { RQ_CP(i - 2); RQ_CP(i - 1); RQ_CP(i); }
            else { RQ_CP(0); RQ_CP(i - 1); RQ_CP(i); }
        }
        break;
    case 8:
        for (i = 0; i + 3 < n && e + 6 <= IM_VB_MAX * 3u; i += 4) {
            RQ_CP(i); RQ_CP(i + 1); RQ_CP(i + 2); RQ_CP(i); RQ_CP(i + 2); RQ_CP(i + 3);
        }
        break;
    case 9:
        for (i = 0; i + 3 < n && e + 6 <= IM_VB_MAX * 3u; i += 2) {
            RQ_CP(i); RQ_CP(i + 1); RQ_CP(i + 2); RQ_CP(i); RQ_CP(i + 2); RQ_CP(i + 3);
        }
        break;
    default: /* 1, 2, 4, 5, unbekannt: direkt */
        for (i = 0; i < n; i++) { RQ_CP(i); }
        break;
    }
#undef RQ_CP
    if (e > IM_VB_MAX) { e = IM_VB_MAX; }
    g_rq_rec.xstride = stride ? stride : 16u;
    bytes = nout * stride;
    if (!stride) { /* Programm liest keine v-Register: Dummy-Stream */
        bytes = 16u;
        memset(X, 0, 16);
    }
    if (xb) {
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)xb, 0);
        g_rqp_xbytes += bytes; /* DIAG(SHADE-PERF,W80) */
        g_rq_rec.xbuf = xb;
        g_rq_rec.xoff = xo;
        return 1;
    }
    g_rqp_xbytes += bytes; /* DIAG(SHADE-PERF,W80) */
    if (g_ip_recording && sb_interp_rq_xwrite(X, bytes, &g_rq_rec.xbuf, &g_rq_rec.xoff)) {
        return 1;
    }
    {
        D3D11_MAPPED_SUBRESOURCE m;
        D3D11_MAP mt = D3D11_MAP_WRITE_NO_OVERWRITE;
        if (bytes > RQ_XRING_BYTES) { g_rq_active = 0; return 0; }
        if (g_rq_xpos + bytes > RQ_XRING_BYTES || g_rq_xpos == 0) { g_rq_xpos = 0; mt = D3D11_MAP_WRITE_DISCARD; }
        if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_rq_xring, 0, mt, 0, &m))) {
            g_rq_active = 0;
            return 0;
        }
        memcpy((unsigned char *)m.pData + g_rq_xpos, X, bytes);
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_rq_xring, 0);
        g_rq_rec.xbuf = g_rq_xring;
        g_rq_rec.xoff = g_rq_xpos;
        g_rq_xpos += (bytes + 255u) & ~255u;
    }
    return 1;
}

static void rq_argb(float *o, unsigned c)
{
    o[0] = (float)((c >> 16) & 0xFFu) / 255.0f;
    o[1] = (float)((c >> 8) & 0xFFu) / 255.0f;
    o[2] = (float)(c & 0xFFu) / 255.0f;
    o[3] = (float)((c >> 24) & 0xFFu) / 255.0f;
}

/* Konstanten-Fenster schreiben: VS-Block (nc+2) und PS-Block (24), je auf
 * 16 Konstanten gerundet (D3D11.1-Fenster). */
/* Konstanten-Block in Tick-Puffer bzw. Ring schreiben (cbuf/vfirst). */
static int rq_consts_put(const float *tmp, unsigned tot)
{
    g_rqp_cbytes += (unsigned long long)tot * 16u; /* DIAG(SHADE-PERF,W80) */
    if (g_ip_recording && sb_interp_rq_cwrite(tmp, tot, &g_rq_rec.cbuf, &g_rq_rec.vfirst)) { return 1; }
    {
        D3D11_MAPPED_SUBRESOURCE m;
        D3D11_MAP mt = D3D11_MAP_WRITE_NO_OVERWRITE;
        if (g_rq_cpos + tot > RQ_CRING_CONST || g_rq_cpos == 0) { g_rq_cpos = 0; mt = D3D11_MAP_WRITE_DISCARD; }
        if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_rq_cring, 0, mt, 0, &m))) { return 0; }
        memcpy((unsigned char *)m.pData + (size_t)g_rq_cpos * 16u, tmp, (size_t)tot * 16u);
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_rq_cring, 0);
        g_rq_rec.cbuf = g_rq_cring;
        g_rq_rec.vfirst = g_rq_cpos;
        g_rq_cpos += tot;
    }
    return 1;
}

static int rq_consts(const float *vblk, unsigned vn, const float *pblk, unsigned pn)
{
    unsigned vr = (vn + 15u) & ~15u, pr = (pn + 15u) & ~15u, tot = vr + pr;
    static float tmp[(RQV_NCONST + 4 + 16 + 15 + 32) * 4];
    memset(tmp, 0, (size_t)tot * 16u);
    memcpy(tmp, vblk, (size_t)vn * 16u);
    memcpy(tmp + vr * 4u, pblk, (size_t)pn * 16u);
    /* SHADE-PERF (W80): gleiches Fenster wie der vorige Shading-Draw (gleicher
     * Puffer, seitdem nicht neu begonnen) -> kein erneuter Upload. */
    if (g_cfg.shade_gpuvb) {
        static float last[(RQV_NCONST + 4 + 16 + 15 + 32) * 4];
        static unsigned last_tot;
        static ID3D11Buffer *last_buf;
        static UINT last_first;
        ID3D11Buffer *cb = NULL;
        UINT pos = 0;
        if (g_ip_recording) { cb = sb_interp_rq_cpos(&pos); }
        if (!cb) { cb = g_rq_cring; pos = g_rq_cpos; }
        if (tot == last_tot && cb == last_buf && last_first + last_tot <= pos &&
            !memcmp(tmp, last, (size_t)tot * 16u)) {
            g_rqp_creuse++;
            g_rq_rec.cbuf = last_buf;
            g_rq_rec.vfirst = last_first;
            g_rq_rec.vnum = vr;
            g_rq_rec.pfirst = g_rq_rec.vfirst + vr;
            g_rq_rec.pnum = pr;
            return 1;
        }
        if (!rq_consts_put(tmp, tot)) { return 0; }
        memcpy(last, tmp, (size_t)tot * 16u);
        last_tot = tot; last_buf = g_rq_rec.cbuf; last_first = g_rq_rec.vfirst;
        g_rq_rec.vnum = vr;
        g_rq_rec.pfirst = g_rq_rec.vfirst + vr;
        g_rq_rec.pnum = pr;
        return 1;
    }
    g_rqp_cbytes += (unsigned long long)tot * 16u; /* DIAG(SHADE-PERF,W80) */
    if (g_ip_recording && sb_interp_rq_cwrite(tmp, tot, &g_rq_rec.cbuf, &g_rq_rec.vfirst)) {
        /* Tick-Puffer */
    } else {
        D3D11_MAPPED_SUBRESOURCE m;
        D3D11_MAP mt = D3D11_MAP_WRITE_NO_OVERWRITE;
        if (g_rq_cpos + tot > RQ_CRING_CONST || g_rq_cpos == 0) { g_rq_cpos = 0; mt = D3D11_MAP_WRITE_DISCARD; }
        if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_rq_cring, 0, mt, 0, &m))) { return 0; }
        memcpy((unsigned char *)m.pData + (size_t)g_rq_cpos * 16u, tmp, (size_t)tot * 16u);
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_rq_cring, 0);
        g_rq_rec.cbuf = g_rq_cring;
        g_rq_rec.vfirst = g_rq_cpos;
        g_rq_cpos += tot;
    }
    g_rq_rec.vnum = vr;
    g_rq_rec.pfirst = g_rq_rec.vfirst + vr;
    g_rq_rec.pnum = pr;
    return 1;
}

/* Texel-Adressierung nur fuer Render-Targets (sicher Rechteck-Texturen; das
 * Spiel adressiert sie mit Texel-Koordinaten, z. B. Monitorflaechen SHORT2
 * roh, Filter-Paesse) sowie fuer lineare Texturen, deren oTk-Quelle (MOV/ADD
 * aus einem v-Register) |uv| > 1.5 hat, bei Pixel-Draws (RHW) auch fuer
 * andere Texturen (beides wie die UV-Normierung des bisherigen Pfads). Lineare
 * Texturen mit NORMSHORT2-UVs (Cockpit-Atlas, Bereich [-1,1]) bleiben
 * normiert. */
extern unsigned g_tex_stage_why;
static int rq_stage_is_rt(unsigned k)
{
    unsigned xp[4], nn[4], w = 0, h = 0;
    sb_tex_stages(xp, nn);
    return xp[k] && sb_rtt_lookup(xp[k], NULL, &w, &h);
}

/* Vor dem Draw: Konstanten, Texturen, Sampler bestimmen und binden.
 * srv0 = Stufe-0-SRV des bisherigen Pfads (NULL bei fehlender Textur bzw.
 * PS-Stufe-0 NONE). */
/* SHADE-PERF (W80): max |uv| der oTk-Quelle aus den Rohdaten (nur bei Bedarf). */
static float rq_raw_tmax(unsigned k)
{
    const rq_rawsrc_t *r = &g_rq_src;
    int reg = g_rq_vse->tsrc[k];
    unsigned i;
    float mx = 0.0f, o[4];
    if (reg < 0) { return 0.0f; }
    __try {
        for (i = 0; i < r->n; i++) {
            unsigned vi = r->idx ? (unsigned)r->idx[i] + r->base : r->start + i;
            unsigned f = g_rq_vse->raw.fmt[reg];
            float a, b;
            rqv_vfetch(f ? f : 0x02u, r->vdata + (size_t)vi * r->stride + g_rq_vse->raw.off[reg], r->cur[reg], o);
            a = o[0] < 0 ? -o[0] : o[0];
            b = o[1] < 0 ? -o[1] : o[1];
            if (a > mx) { mx = a; }
            if (b > mx) { mx = b; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0.0f; }
    return mx;
}

/* Vertex i der Roh-Quelle in vin[16][4] (DIAG RQREF). */
static void rq_raw_vertex(unsigned i, float vin[16][4])
{
    const rq_rawsrc_t *r = &g_rq_src;
    unsigned s;
    __try {
        unsigned vi = r->idx ? (unsigned)r->idx[i] + r->base : r->start + i;
        for (s = 0; s < 16u; s++) {
            unsigned f = g_rq_vse->raw.fmt[s];
            if (!(g_rq_vse->prog.vinmask & (1u << s))) { continue; }
            rqv_vfetch(f ? f : 0x02u, r->vdata + (size_t)vi * r->stride + g_rq_vse->raw.off[s], r->cur[s], vin[s]);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

/* DIAG(SHADE-BLOOM,W112): wirksame PS-Konstanten des letzten rq_bind und
 * VS-Ausgaben eines Quell-Vertex (CPU-Interpreter) fuer render_bloom.c. */
static float g_rq_lastpb[24 * 4];
static unsigned g_rq_lastnst;
const float *rq_last_pblk(void) { return g_rq_lastpb; }
unsigned rq_last_pstages(void) { return g_rq_lastnst; }
float g_rq_diag_od[3][4]; /* DIAG(SHADE-M2,W117): oD0, oD1, oFog des letzten rq_diag_vertex */
int rq_diag_vertex(unsigned i, float pos[4], float ot[4][4])
{
    static float vin[16][4], cc[RQV_NCONST][4];
    rqv_out_t ro;
    unsigned j;
    if (!g_rq_active || !g_rq_vse) { return 0; }
    memset(vin, 0, sizeof vin);
    if (g_rq_src.use) {
        if (i >= g_rq_src.n) { return 0; }
        rq_raw_vertex(i, vin);
    } else {
        if (i >= g_rq_n) { return 0; }
        for (j = 0; j < g_rq_vse->nv; j++) { memcpy(vin[g_rq_vse->vreg[j]], g_rq_pack + ((size_t)i * g_rq_vse->nv + j) * 4u, 16); }
    }
    __try { memcpy(cc, (const void *)(uintptr_t)RQ_VMIRROR, sizeof cc); }
    __except (EXCEPTION_EXECUTE_HANDLER) { memset(cc, 0, sizeof cc); }
    rqv_run(&g_rq_vse->prog, (const float (*)[4])vin, cc, &ro);
    memcpy(pos, ro.o[RQV_OPOS], 16);
    for (j = 0; j < 4u; j++) { memcpy(ot[j], ro.o[RQV_OT0 + j], 16); }
    memcpy(g_rq_diag_od[0], ro.o[RQV_OD0], 16); /* DIAG(SHADE-M2,W117): oD0/oD1/oFog */
    memcpy(g_rq_diag_od[1], ro.o[RQV_OD1], 16);
    memcpy(g_rq_diag_od[2], ro.o[RQV_OFOG], 16);
    return 1;
}

/* W194: begrenzte, rein lesende Lichtspur. Nur explizite Screenshot-Termine,
 * maximal screenshot_max Bilder, 4096 Draws/Bild und sechs Quell-Vertices.
 * Konstanten/Eingaenge erlauben Nachrechnung ohne Spielspeicher-Dump.
 * rqv_run erhaelt stets eine Kopie (Programme duerfen Konstanten schreiben). */
static void rq_flash_diag(const float *pb)
{
    static unsigned last, draw;
    unsigned p = g_present_no + 1u, sf = (unsigned)g_cfg.screenshot_frame;
    unsigned ev = (unsigned)g_cfg.screenshot_every, n, samples, k, j, xp[4], nn[4];
    float vin[16][4], cc[RQV_NCONST][4];
    rqv_out_t ro;
    if (g_cfg.flash_diag <= 0 || g_cfg.screenshot_max <= 0 || !sf || !ev || p < sf ||
        (p - sf) % ev || (p - sf) / ev >= (unsigned)g_cfg.screenshot_max ||
        rq_u32(0x2EEA34u) != 4u) { return; }
    if (last != p) { last = p; draw = 0; }
    if (draw >= 4096u) { return; }
    n = g_rq_src.use ? g_rq_src.n : g_rq_n;
    if (!n) { return; }
    sb_tex_stages(xp, nn);
    HLOG_W("FLASH draw f=%u d=%u vs=0x%X ps=0x%X tgt=%d tex=0x%X n=%u raw=%d hash=0x%X",
           p, draw, g_im_shader, g_im_pshader, rtt_active() ? g_rtt_bound : -1, xp[0], n, g_rq_src.use, g_rq_vse->hash);
    HLOG_W("FLASH ambient f=%u d=%u c0=(%.9g,%.9g,%.9g,%.9g)",
           p, draw, pb[0], pb[1], pb[2], pb[3]);
    /* XDK 0x213CC0 / 0x217E3A: Sekundaerfarbe bei SPECULARENABLE oder
     * PS-Bedarf (Device+8, Bit 6); kein Eingriff in den Hardware-Zustand. */
    HLOG_W("FLASH spec f=%u d=%u rs=%u psneed=%u", p, draw,
           rq_u32(0x21AC4Cu), (rq_u32(rq_u32(0x21A8A0u) + 8u) >> 6) & 1u);
    __try { memcpy(cc, (const void *)(uintptr_t)RQ_VMIRROR, sizeof cc); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    for (j = 0; j < g_rq_vse->nc; j++) {
        unsigned c = g_rq_vse->cidx[j];
        HLOG_W("FLASH c f=%u d=%u c=%d v=(%.9g,%.9g,%.9g,%.9g)",
               p, draw, (int)c - 96, cc[c][0], cc[c][1], cc[c][2], cc[c][3]);
    }
    samples = (unsigned)g_cfg.flash_diag;
    if (samples > 6u) { samples = 6u; }
    if (samples > n) { samples = n; }
    for (k = 0; k < samples; k++) {
        unsigned vi = samples > 1u ? k * (n - 1u) / (samples - 1u) : 0u;
        memset(vin, 0, sizeof vin);
        if (g_rq_src.use) { rq_raw_vertex(vi, vin); }
        else {
            for (j = 0; j < g_rq_vse->nv; j++) {
                memcpy(vin[g_rq_vse->vreg[j]], g_rq_pack + ((size_t)vi * g_rq_vse->nv + j) * 4u, 16);
            }
        }
        for (j = 0; j < 16u; j++) {
            if (!(g_rq_vse->prog.vinmask & (1u << j))) { continue; }
            HLOG_W("FLASH v f=%u d=%u i=%u r=%u v=(%.9g,%.9g,%.9g,%.9g)",
                   p, draw, vi, j, vin[j][0], vin[j][1], vin[j][2], vin[j][3]);
        }
        __try { memcpy(cc, (const void *)(uintptr_t)RQ_VMIRROR, sizeof cc); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return; }
        rqv_run(&g_rq_vse->prog, (const float (*)[4])vin, cc, &ro);
        HLOG_W("FLASH o f=%u d=%u i=%u pos=(%.9g,%.9g,%.9g,%.9g) d0=(%.9g,%.9g,%.9g,%.9g) d1=(%.9g,%.9g,%.9g,%.9g)",
               p, draw, vi, ro.o[0][0], ro.o[0][1], ro.o[0][2], ro.o[0][3],
               ro.o[3][0], ro.o[3][1], ro.o[3][2], ro.o[3][3],
               ro.o[4][0], ro.o[4][1], ro.o[4][2], ro.o[4][3]);
    }
    draw++;
}

int rq_bind(ID3D11ShaderResourceView *srv0, int rhw)
{
    float vblk[(RQV_NCONST + 4 + 16) * 4], pblk[28 * 4]; /* W121: +sfx0..sfx3 (Fenster bleibt 32) */
    const unsigned *d;
    unsigned k, mask = (unsigned)g_cfg.shade_mask, fen, ftb, vn;
    float *tsc;
    if (!g_rq_active) { return 0; }
    d = g_rq_pse->d;
    if (g_cfg.shade_pbconst >= 2) { /* DIAG(W80): PSDef-Konstanten im Speicher je Draw vs. Eintrag */
        unsigned pdef = rq_u32((g_im_pshader & ~1u) + 8u), q;
        g_rq_pdchk++;
        if (pdef >= 0x10000u && pdef < 0x80000000u) {
            for (q = 10u; q < 45u; q++) {
                unsigned lv;
                if (q >= 26u && q < 43u) { continue; }
                lv = rq_u32(pdef + q * 4u);
                if (lv != d[q]) { g_rq_pddiff++; g_rq_pdlast[0] = g_im_pshader; g_rq_pdlast[1] = q; g_rq_pdlast[2] = lv; g_rq_pdlast[3] = d[q]; }
            }
        }
    }
    /* VS-Konstanten aus dem Spiegel (umnummeriert) */
    __try {
        for (k = 0; k < g_rq_vse->nc; k++) {
            memcpy(&vblk[k * 4u], (const void *)(uintptr_t)(RQ_VMIRROR + (unsigned)g_rq_vse->cidx[k] * 16u), 16);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_rq_active = 0; g_rq_why = 9u; return 0; }
    /* Nebel: XDK-Lazy-Fog (0x213340) */
    {
        float *fg = &vblk[g_rq_vse->nc * 4u];
        union { unsigned u; float f; } st, en, de;
        fen = rq_u32(0x21AC20u); ftb = rq_u32(0x21AC24u);
        st.u = rq_u32(0x21AC28u); en.u = rq_u32(0x21AC2Cu); de.u = rq_u32(0x21AC30u);
        fg[0] = 0.0f; fg[1] = 1.0f; fg[2] = 1.0f; fg[3] = 0.0f;
        if (fen && (mask & 1u)) {
            if (ftb == 0u) { fg[0] = 1.0f; fg[1] = 1.0f; fg[2] = 1.0f; }
            else if (ftb == 3u) {
                float kk = (en.f != st.f) ? 1.0f / (en.f - st.f) : 8192.0f;
                fg[0] = 1.0f; fg[1] = en.f * kk + 1.0f; fg[2] = -kk;
            } else if (ftb == 1u) { fg[0] = 2.0f; fg[1] = 1.5f; fg[2] = de.f * -0.0901680738f; }
            else if (ftb == 2u) { fg[0] = 3.0f; fg[1] = 1.5f; fg[2] = de.f * -0.2123300284f; }
        }
        fg[4] = (g_rq_kind == 0u && (mask & 2u)) ? 1.0f : 0.0f; /* oD0 aus dem Programm */
        fg[5] = 1.0f;                                             /* oT0 aus dem Programm */
        fg[6] = (mask & 2u) ? 1.0f : 0.0f;                        /* oD1 */
        fg[7] = 0.0f;
    }
    vn = g_rq_vse->nc + 2u;
    if (g_rq_src.use) { /* SHADE-PERF (W80): rq_raw, rq_raw2, rq_inl */
        unsigned *ru = (unsigned *)&vblk[vn * 4u], s;
        ru[0] = g_rq_rawx[0]; ru[1] = g_rq_rawx[1]; ru[2] = g_rq_rawx[2]; ru[3] = g_rq_rawx[3];
        ru[4] = g_rq_rawi; ru[5] = 0; ru[6] = 0; ru[7] = 0;
        vn += 2u;
        for (s = 0; s < 16u; s++) {
            if ((g_rq_vse->prog.vinmask & (1u << s)) && g_rq_vse->raw.fmt[s] == 0) {
                memcpy(&vblk[(vn + g_rq_vse->raw.inl[s]) * 4u], g_rq_src.cur[s], 16);
            }
        }
        if (g_rq_vse->raw.ninl) { vn += g_rq_vse->raw.ninl; }
        else { memset(&vblk[vn * 4u], 0, 16); vn++; }
    }
    /* PS-Konstanten (C0/C1 je Stufe, Final, Nebelfarbe, Texturskalen) */
    memset(pblk, 0, sizeof pblk);
    for (k = 0; k < 8u; k++) {
        unsigned c0 = d[10 + k], c1 = d[18 + k];
        int uniq0k = (d[53] & 0x100u) != 0, uniq1k = (d[53] & 0x1000u) != 0;
        unsigned r0 = (d[57] >> (k * 4u)) & 0xFu, r1 = (d[58] >> (k * 4u)) & 0xFu;
        if (r0 < 8u && (g_rq_psconst_set & (1u << r0))) { c0 = g_rq_psconst[r0]; }
        if (r1 < 8u && (g_rq_psconst_set & (1u << r1))) { c1 = g_rq_psconst[r1]; }
        /* SetRenderState(D3DRS_PSCONSTANT0_k / 1_k) seit SetPixelShader */
        if (g_rq_rs_set & (1ull << (10u + k))) { c0 = g_rq_rs[10u + k]; }
        if (g_rq_rs_set & (1ull << (18u + k))) { c1 = g_rq_rs[18u + k]; }
        rq_argb(&pblk[k * 4u], c0);
        rq_argb(&pblk[(8u + k) * 4u], c1);
    }
    {
        unsigned f0 = d[43], f1 = d[44], m0 = d[59] & 0xFu, m1 = (d[59] >> 4) & 0xFu;
        if (m0 < 8u && (g_rq_psconst_set & (1u << m0))) { f0 = g_rq_psconst[m0]; }
        if (m1 < 8u && (g_rq_psconst_set & (1u << m1))) { f1 = g_rq_psconst[m1]; }
        if (g_rq_rs_set & (1ull << 43u)) { f0 = g_rq_rs[43]; }
        if (g_rq_rs_set & (1ull << 44u)) { f1 = g_rq_rs[44]; }
        rq_argb(&pblk[16u * 4u], f0);
        rq_argb(&pblk[17u * 4u], f1);
        rq_argb(&pblk[18u * 4u], g_rq_fogcolor);
    }
    tsc = &pblk[19u * 4u];
    for (k = 0; k < 4u; k++) {
        unsigned tm = g_rq_pse->info.texmode[k], w = 0, h = 0;
        int lin = 0;
        ID3D11ShaderResourceView *s = NULL;
        tsc[k * 4u + 0] = 1.0f; tsc[k * 4u + 1] = 1.0f; tsc[k * 4u + 2] = 0.0f; tsc[k * 4u + 3] = 0.0f;
        g_rq_rec.srv[k] = NULL;
        g_rq_rec.smp[k] = g_rq_defsmp;
        if (tm != 1u && tm != 2u) { continue; }
        if (k == 0) {
            s = srv0;
            if (s) { sb_tex_dims(&w, &h); { unsigned f = 0; sb_tex_fmt(&f, &lin); } }
        } else if (mask & 4u) {
            /* DIAG(SHADOWBUF,W127) shadow_ab=1: im Screenshot-Bild ohne Schattenvergleich
             * (T1 = 1 wie shade_shadow=0, Tiefenpass laeuft weiter), Partnerbild +1 mit. */
            if (g_cfg.shade_shadow && !((g_cfg.shadow_ab || g_cfg.shfx_ab == 5) && rq_ab_base())) { /* W131: Preset-Tripel A = Original */ /* RENDER-QA3 (W100): Schattenpuffer? */
                unsigned sxp[4], snn[4];
                float zs = 0.0f;
                sb_tex_stages(sxp, snn);
                s = sb_rtt_depth_srv(sxp[k], &w, &h, &zs);
                if (s && w && h) {
                    g_rq_rec.srv[k] = s;
                    tsc[k * 4u + 0] = 1.0f / (float)w; /* Linear-Tiefe: Texel-Koordinaten */
                    tsc[k * 4u + 1] = 1.0f / (float)h;
                    tsc[k * 4u + 2] = 1.0f;
                    tsc[k * 4u + 3] = (zs > 0.0f) ? 1.0f / zs : 1.0f;
                    g_rq_shadow_draws++;
                    continue;
                }
                s = NULL; w = h = 0;
            }
            s = sb_tex_get_stage(k, &w, &h, &lin);
        }
        if (!s) { continue; }
        g_rq_rec.srv[k] = s;
        tsc[k * 4u + 2] = 1.0f;
        /* Linear-Textur: NV2A adressiert mit Texel-Koordinaten. Stufe 0 mit
         * i.uv (P1, kind 1) ist bereits normiert (bisheriger CPU-Pfad). */
        if (w && h) {
            int isrt = rq_stage_is_rt(k); /* SHADE-PERF (W80): einmal je Stufe */
            if (!isrt && (lin || rhw) && g_rq_tmax[k] < 0.0f) { g_rq_tmax[k] = rq_raw_tmax(k); }
            /* FIX(SHADE-FX,W121): Korn-Wash der Monitore (render_mfx, swizzled,
             * WRAP) adressiert normiert - wie g_mfx_nonorm im alten Pfad; die
             * Texel-Skala 1/128 machte aus dem Korn einen glatten Schleier. */
            int mfx_norm = (k == 0u && g_mfx_nonorm && g_cfg.shade_mfx && !isrt);
            if (isrt || (!mfx_norm && g_rq_tmax[k] > 1.5f && (lin || rhw))) {
                tsc[k * 4u + 0] = 1.0f / (float)w;
                tsc[k * 4u + 1] = 1.0f / (float)h;
            }
        }
        g_rq_rec.smp[k] = rq_sampler(rq_u32(0x21A8B0u + (k * 32u + 0u) * 4u),
                                     rq_u32(0x21A8B0u + (k * 32u + 1u) * 4u));
    }
    {   /* RENDER-QA3 (W100): pflags = Alpha-Test (x Funktion, y Ref), ALPHAKILL-
         * Maske der Stufen 0..3 (z), DIAG-Term (w); als uint (asuint im PS).
         * Gleiche Schalter/Quellen wie der alte Pfad: fx_alphatest (W90, NV-
         * Spiegel 0x300/0x33C/0x340), fx_alphakill (W98, Deferred-TSS
         * 0x21A8B0 + (k*32 + 11)*4 Bit 2, RT-Texturen ausgenommen). */
        unsigned *pf = (unsigned *)&pblk[23u * 4u];
        pf[0] = pf[1] = pf[2] = pf[3] = 0u;
        if (g_cfg.fx_alphatest && fx_nv(0x300u, 0u)) {
            unsigned af = fx_nv(0x33Cu, 0x207u) & 0xFFFFu;
            if (af >= 0x200u && af <= 0x206u) { pf[0] = af - 0x1FFu; pf[1] = fx_nv(0x340u, 0u) & 0xFFu; }
        }
        if (g_cfg.fx_alphakill && (g_cfg.fx_alphakill >= 2 || rq_in_mission())) {
            for (k = 0; k < 4u; k++) {
                if (g_rq_rec.srv[k] && (rq_u32(0x21A8B0u + (k * 32u + 11u) * 4u) & 4u) && !rq_stage_is_rt(k)) {
                    pf[2] |= 1u << k;
                }
            }
        }
        if (g_cfg.shade_shadow) {
            /* D3DRS_SHADOWFUNC (hook_D3D_SetRenderState_ShadowFunc, D3DCMP -
             * 0x200 = NV 0..7: NEVER LESS EQUAL LEQUAL GREATER NOTEQUAL GEQUAL
             * ALWAYS). NV2A-Semantik "Tiefe FUNC r" (Spiel: GREATER = beleuchtet,
             * wenn die gespeicherte Tiefe hinter r liegt); rq_cmp rechnet
             * "r FUNC' Tiefe" -> Operanden tauschen. shade_shadow=2: ungetauscht (DIAG). */
            extern unsigned g_rq_shadowfunc;
            static const unsigned sw[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };
            unsigned sf = g_rq_shadowfunc != 0xFFFFFFFFu ? g_rq_shadowfunc : 0x204u;
            if (sf >= 0x200u) { sf -= 0x200u; }
            sf &= 7u;
            if (g_cfg.shade_shadow != 2) { sf = sw[sf]; }
            pf[2] |= sf << 8;
            g_rq_shadow_func = g_rq_shadowfunc;
        }
        if (g_cfg.shade_dbg && g_rq_kind != 1u && rtt_active()) { /* DIAG(W100): nur 3D-Draws in Render-Targets (Welt-RTs) */
            pf[3] = g_cfg.shade_dbg == 99 ? (g_present_no / 1500u) % 10u : (unsigned)g_cfg.shade_dbg;
        }
    }
    {   /* SHADE-FX (W121): Optionen Aussenwelt (sfx0/sfx1, alles 0 = Original) */
        int srvk[4], tgt = rtt_active() ? g_rtt_bound : -1;
        for (k = 0; k < 4u; k++) { srvk[k] = g_rq_rec.srv[k] != NULL && tsc[k * 4u + 3] == 0.0f; }
        if (tgt >= 0 && g_rq_kind == 0u && !rhw) { shfx_note_3d(tgt); }
        /* Aussenwelt = Nebel an (FOGENABLE) UND Nebel im Spiel-PS; der Cockpit-PS hat
         * ebenfalls einen Nebel-Final-Combiner, zeichnet aber mit FOGENABLE=0 (W121_h4). */
        int is3d = g_rq_kind == 0u && !rhw, fogps = fen != 0u && g_rq_pse->info.uses_fog;
        int s0rt = srvk[0] && rq_stage_is_rt(0);
        shfx_rq_consts(&pblk[24u * 4u], srvk, tgt, is3d, fogps, s0rt, g_rq_pse->info.nstages);
        if (g_ip_recording) { /* W193 PAUSE-LIVE: Eingaben fuer die Neuberechnung in der Pause */
            unsigned sxp[4], snn[4];
            sb_tex_stages(sxp, snn);
            for (k = 0; k < 4u; k++) { g_rq_rec.sfx_xp[k] = srvk[k] ? sxp[k] : 0u; }
            g_rq_rec.sfx_tgt = (signed char)tgt;
            g_rq_rec.sfx_fl = (unsigned char)((is3d ? 1 : 0) | (fogps ? 2 : 0) | (s0rt ? 4 : 0));
            g_rq_rec.sfx_nst = (unsigned char)g_rq_pse->info.nstages;
        }
    }
    if (g_cfg.bloom_diag) { memcpy(g_rq_lastpb, pblk, sizeof g_rq_lastpb); g_rq_lastnst = g_rq_pse->info.nstages; } /* DIAG(SHADE-BLOOM,W112) */
    if (!rq_consts(vblk, vn, pblk, 28u)) { g_rq_active = 0; g_rq_why = 9u; return 0; }
    if (g_rq_src.use) {
        g_rq_rec.vs = g_rq_vse->vsr; g_rq_rec.vsl = g_rq_vse->vsrl;
        g_rq_rec.lay = g_rq_vse->layr; g_rq_rec.layl = g_rq_vse->layrl;
    } else {
        g_rq_rec.vs = g_rq_vse->vs; g_rq_rec.vsl = g_rq_vse->vsl;
        g_rq_rec.lay = g_rq_vse->lay; g_rq_rec.layl = g_rq_vse->layl;
    }
    g_rq_rec.ps = g_rq_pse->ps;
    /* Binden */
    ID3D11DeviceContext_IASetInputLayout(g_ctx, g_rq_rec.lay);
    if (g_rq_rec.xsrv) { ID3D11DeviceContext_VSSetShaderResources(g_ctx, 0, 1, &g_rq_rec.xsrv); }
    else { ID3D11DeviceContext_IASetVertexBuffers(g_ctx, 2, 1, &g_rq_rec.xbuf, &g_rq_rec.xstride, &g_rq_rec.xoff); }
    ID3D11DeviceContext_VSSetShader(g_ctx, g_rq_rec.vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_ctx, g_rq_rec.ps, NULL, 0);
    ID3D11DeviceContext1_VSSetConstantBuffers1(g_rq_ctx1, 3, 1, &g_rq_rec.cbuf, &g_rq_rec.vfirst, &g_rq_rec.vnum);
    ID3D11DeviceContext1_PSSetConstantBuffers1(g_rq_ctx1, 4, 1, &g_rq_rec.cbuf, &g_rq_rec.pfirst, &g_rq_rec.pnum);
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 4, g_rq_rec.srv);
    ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 4, g_rq_rec.smp);
    g_rq_stat_draws++;
    if (g_cfg.flash_diag > 0) { rq_flash_diag(pblk); }
    if (g_cfg.rq_diag) {
        static unsigned seen[64][2], nseen;
        unsigned q;
        unsigned key = g_im_pshader ^ ((unsigned)(rtt_active() ? g_rtt_bound + 1 : 0) << 24);
        for (q = 0; q < nseen; q++) { if (seen[q][0] == g_im_shader && seen[q][1] == key) { break; } }
        if (q == nseen && nseen < 64u) {
            unsigned xp[4], nn[4];
            seen[nseen][0] = g_im_shader; seen[nseen][1] = key; nseen++;
            sb_tex_stages(xp, nn);
            {   /* DIAG: D3D__RenderState 0x21AAB0.. (PS-States + Simple-States) nicht-null */
                char rb[600];
                int ro = 0;
                unsigned ri;
                rb[0] = 0;
                for (ri = 0; ri < 92u && ro >= 0 && ro < 560; ri++) {
                    unsigned rv = rq_u32(0x21AAB0u + ri * 4u);
                    if (rv) { ro += _snprintf_s(rb + ro, sizeof rb - (size_t)ro, _TRUNCATE, "%u=%X ", ri, rv); }
                }
                HLOG_W("RQRS f=%u ps=0x%X %s", g_present_no, g_im_pshader, rb);
            }
            if (xp[1] && !g_rq_rec.srv[1] && (g_rq_pse->info.texmode[1] == 1u || g_rq_pse->info.texmode[1] == 2u)) {
                unsigned w1 = 0, h1 = 0;
                int l1 = 0;
                sb_tex_get_stage(1, &w1, &h1, &l1);
                HLOG_W("RQSTAGE1 xp=0x%X c0=0x%X data=0x%X fmt=0x%X size=0x%X why=%u",
                       xp[1], rq_u32(xp[1]), rq_u32(xp[1] + 4u), rq_u32(xp[1] + 12u), rq_u32(xp[1] + 16u), g_tex_stage_why);
            }
            HLOG_W("RQSTAGE f=%u vs=0x%X ps=0x%X C0s0=(%.2f,%.2f,%.2f,%.2f) C0s1=(%.2f,%.2f,%.2f,%.2f) psc=%u/0x%X RS[10..13]=%08X %08X %08X %08X RS[18]=%08X RS[43]=%08X dev4ac=%08X",
                   g_present_no, g_im_shader, g_im_pshader, pblk[0], pblk[1], pblk[2], pblk[3],
                   pblk[4], pblk[5], pblk[6], pblk[7], g_rq_psconst_calls, g_rq_psconst_set,
                   rq_u32(0x21AAB0u + 40u), rq_u32(0x21AAB0u + 44u), rq_u32(0x21AAB0u + 48u), rq_u32(0x21AAB0u + 52u),
                   rq_u32(0x21AAB0u + 72u), rq_u32(0x21AAB0u + 172u), rq_u32(rq_u32(0x21A8A0u) + 0x4ACu));
            HLOG_W("RQSTAGE f=%u vs=0x%X ps=0x%X tgt=%d rhw=%d kind=%u raw=%d tmax0=%.1f xp=0x%X/0x%X/0x%X/0x%X srv=%d%d%d%d tsc0=%.5f fog=%u/%u",
                   g_present_no, g_im_shader, g_im_pshader, rtt_active() ? g_rtt_bound + 1 : 0, rhw, g_rq_kind, g_rq_src.use, g_rq_tmax[0], xp[0], xp[1], xp[2], xp[3],
                   g_rq_rec.srv[0] != NULL, g_rq_rec.srv[1] != NULL, g_rq_rec.srv[2] != NULL,
                   g_rq_rec.srv[3] != NULL, tsc[0], fen, ftb);
            /* CPU-Referenz fuer Vertex 0 (gleiches Programm, gleiche Konstanten) */
            {
                static float vin[16][4], cc[RQV_NCONST][4];
                rqv_out_t ro;
                unsigned j;
                memset(vin, 0, sizeof vin);
                if (g_rq_src.use) { rq_raw_vertex(0, vin); }
                else { for (j = 0; j < g_rq_vse->nv; j++) { memcpy(vin[g_rq_vse->vreg[j]], g_rq_pack + j * 4u, 16); } }
                __try { memcpy(cc, (const void *)(uintptr_t)RQ_VMIRROR, sizeof cc); }
                __except (EXCEPTION_EXECUTE_HANDLER) { memset(cc, 0, sizeof cc); }
                rqv_run(&g_rq_vse->prog, (const float (*)[4])vin, cc, &ro);
                HLOG_W("RQREF vs=0x%X v0=(%.3f,%.3f,%.3f,%.3f) v1=(%.3f,%.3f,%.3f) v2=(%.3f,%.3f,%.3f,%.3f) v3=(%.4f,%.4f) oD0=(%.3f,%.3f,%.3f,%.3f) oD1=(%.3f,%.3f,%.3f,%.3f) fog=%.3f oT0=(%.4f,%.4f,%.4f,%.4f) oT1=(%.3f,%.3f,%.3f,%.3f)",
                       g_im_shader, vin[0][0], vin[0][1], vin[0][2], vin[0][3], vin[1][0], vin[1][1], vin[1][2],
                       vin[2][0], vin[2][1], vin[2][2], vin[2][3], vin[3][0], vin[3][1],
                       ro.o[RQV_OD0][0], ro.o[RQV_OD0][1], ro.o[RQV_OD0][2], ro.o[RQV_OD0][3],
                       ro.o[RQV_OD1][0], ro.o[RQV_OD1][1], ro.o[RQV_OD1][2], ro.o[RQV_OD1][3], ro.o[RQV_OFOG][0],
                       ro.o[RQV_OT0][0], ro.o[RQV_OT0][1], ro.o[RQV_OT0][2], ro.o[RQV_OT0][3],
                       ro.o[RQV_OT1][0], ro.o[RQV_OT1][1], ro.o[RQV_OT1][2], ro.o[RQV_OT1][3]);
                HLOG_W("RQREF vs=0x%X c0=(%.3f,%.3f,%.3f,%.3f) c1=(%.3f,%.3f,%.3f,%.3f) c10=(%.2f,%.2f,%.2f,%.2f) c12=(%.4f,%.4f,%.4f,%.4f) c17=(%.2f,%.2f,%.2f) c27=(%.3f,%.3f,%.3f,%.3f) c40=(%.3f,%.3f,%.3f,%.3f) c42=(%.3f,%.3f,%.3f,%.3f)",
                       g_im_shader, cc[96][0], cc[96][1], cc[96][2], cc[96][3], cc[97][0], cc[97][1], cc[97][2], cc[97][3],
                       cc[106][0], cc[106][1], cc[106][2], cc[106][3], cc[108][0], cc[108][1], cc[108][2], cc[108][3],
                       cc[113][0], cc[113][1], cc[113][2], cc[123][0], cc[123][1], cc[123][2], cc[123][3],
                       cc[136][0], cc[136][1], cc[136][2], cc[136][3], cc[138][0], cc[138][1], cc[138][2], cc[138][3]);
            }
        }
    }
    return 1;
}

/* Draw mit Stream-0-Versatz (statt StartVertex, damit Stream 2 frei liegen
 * kann) und Rueckbau auf den bisherigen Zustand (Sampler s0, t1..t3, b3/b4
 * stoeren den alten PS nicht). */
void rq_draw(ID3D11Buffer *vb0, unsigned nout, unsigned vb_start)
{
    UINT st = sizeof(im_vertex_t), off = vb_start * (UINT)sizeof(im_vertex_t), z = 0;
    ID3D11ShaderResourceView *nul[3] = { NULL, NULL, NULL };
    ID3D11Buffer *nb = NULL;
    ID3D11DeviceContext_IASetVertexBuffers(g_ctx, 0, 1, &vb0, &st, &off);
    WD_CALL("draw_rq", g_ctx, ID3D11DeviceContext_Draw(g_ctx, nout, 0)); /* DIAG(PERF-WARM,W139) */
    if (g_cfg.shade_gpuvb) {
        /* SHADE-PERF (W80): nur zuruecksetzen, was der naechste alte Draw nicht
         * selbst setzt (Stream 0/Layout/VS/PS/t0 setzt jeder Draw neu; Stream 2
         * liest g_im_layout nicht; Sampler s0 und t1..t3 nur falls belegt). */
        if (g_rq_rec.srv[1] || g_rq_rec.srv[2] || g_rq_rec.srv[3]) {
            ID3D11DeviceContext_PSSetShaderResources(g_ctx, 1, 3, nul);
        }
        if (g_rq_rec.smp[0] != g_rq_defsmp) { ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 1, &g_rq_defsmp); }
        g_rq_active = 0;
        g_rq_src.use = 0;
        return;
    }
    ID3D11DeviceContext_IASetVertexBuffers(g_ctx, 0, 1, &vb0, &st, &z);
    ID3D11DeviceContext_IASetVertexBuffers(g_ctx, 2, 1, &nb, &st, &z);
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 1, 3, nul);
    ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 1, &g_rq_defsmp);
    g_rq_active = 0;
    g_rq_src.use = 0;
}

/* SHADE-PERF (W80): VB-Draw, vor der Vertex-Schleife (nach rq_vb_prepare):
 * Roh-Eingaenge setzen, wenn die Decl auf der GPU dekodierbar ist. 1 = ja
 * (dann keine CPU-Dekodierung der v-Register). */
int rq_vb_raw_set(const unsigned char *vdata, unsigned stride, const unsigned short *idx,
                  unsigned base, unsigned start, unsigned n, const float (*cur)[4])
{
    rq_vsent_t *e;
    g_rq_src.on = 0;
    if (!g_cfg.shade_gpuvb || !g_rq_xsrv || !vdata || !n || stride == 0) { return 0; }
    e = rq_vs_get(g_im_shader);
    if (!e || !e->nv || !rq_raw_variant(e, g_im_shader) || e->raw.need > stride) { return 0; }
    g_rq_src.vdata = vdata; g_rq_src.stride = stride; g_rq_src.idx = idx;
    g_rq_src.base = base; g_rq_src.start = start; g_rq_src.n = n; g_rq_src.cur = cur;
    g_rq_src.on = 1;
    return 1;
}

void rq_vb_raw_clear(void)
{
    g_rq_src.on = 0;
}

ID3D11SamplerState *rq_default_sampler(void) { return g_rq_defsmp; }

unsigned g_rq_shadow_draws, g_rq_shadow_func; /* RENDER-QA3 (W100) */
void rq_stat_log(unsigned f)
{
    if (g_cfg.shading && g_cfg.shade_shadow) {
        static unsigned last;
        if (g_rq_shadow_draws != last) {
            HLOG_W("RQSHADOW f=%u draws=%u func=0x%X", f, g_rq_shadow_draws, g_rq_shadow_func);
            last = g_rq_shadow_draws;
        }
    }
    if (!g_cfg.shading || !g_cfg.rq_diag) { return; }
    HLOG_W("RQSTAT f=%u draws=%u skip(szene=%u tiefe=%u ohnePS=%u vs=%u ps=%u exp=%u konst=%u 2d/kette=%u) vs=%u ps=%u psrs=%u",
           f, g_rq_stat_draws, g_rq_stat_skip[0], g_rq_stat_skip[1], g_rq_stat_skip[2], g_rq_stat_skip[3],
           g_rq_stat_skip[4], g_rq_stat_skip[5], g_rq_stat_skip[6], g_rq_stat_skip[7], g_rqvs_n, g_rqps_n, g_rq_rs_calls);
}

/* VB-Pfad: alle v-Register eines Vertex aus der Decl dekodieren (Xbox
 * X_D3DVSDT; fehlende Komponenten 0, w = 1; nicht belegte Register = Inline-
 * Werte cur, NV2A-Semantik). */
/* VB-Pfad, vor der Vertex-Schleife: braucht dieser Draw den Shading-Pfad?
 * Rueckgabe = v-Register-Maske des Programms (0 = nein -> keine Dekodierung;
 * spart die Arbeit fuer Tiefenpaesse/Menues). */
unsigned rq_vb_prepare(void)
{
    rq_vsent_t *e;
    if (!g_cfg.shading || g_rq_dead || !rq_init()) { return 0; }
    if (g_cfg.shade_ab && rq_ab_skip()) { return 0; } /* DIAG(W100/W112) */
    if (g_cfg.shade_scope == 0 && !rq_in_mission()) { return 0; }
    if (rs_cmask == 0u || g_im_shader < 0x10000u || !g_im_pshader) { return 0; }
    e = rq_vs_get(g_im_shader);
    if (!e || (e->kind == 1 && !(g_cfg.shade_mask & 16))) { return 0; }
    return e->prog.vinmask ? e->prog.vinmask : 1u;
}

static int rq_vb_decode_in(unsigned handle, const unsigned char *v, const float (*cur)[4], float out[16][4], unsigned mask);
int rq_vb_decode(unsigned handle, const unsigned char *v, const float (*cur)[4], float out[16][4], unsigned mask)
{
    int r = 0;
    __try { r = rq_vb_decode_in(handle, v, cur, out, mask); }
    __except (EXCEPTION_EXECUTE_HANDLER) { r = 0; }
    return r;
}
static int rq_vb_decode_in(unsigned handle, const unsigned char *v, const float (*cur)[4], float out[16][4], unsigned mask)
{
    static unsigned dh;
    static unsigned doff[16], dfmt[16], dok;
    unsigned s;
    if (handle != dh) {
        dh = handle; dok = 0;
        if (handle < 0x10000u) { return 0; }
        __try {
            unsigned base = handle & ~1u;
            for (s = 0; s < 16u; s++) {
                const unsigned *sl = (const unsigned *)(uintptr_t)(base + 0x14u + s * 16u);
                unsigned fmt = sl[2] & 0xFFu;
                doff[s] = sl[1];
                dfmt[s] = (sl[0] == 0u) ? fmt : 0x02u; /* nur Stream 0 */
            }
            dok = 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) { dok = 0; }
    }
    if (!dok) { return 0; }
    for (s = 0; s < 16u; s++) {
        if (!(mask & (1u << s))) { continue; }
        rqv_vfetch(dfmt[s], v + doff[s], cur[s], out[s]);
    }
    return 1;
}
