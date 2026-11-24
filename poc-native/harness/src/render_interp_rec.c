/* render_interp_rec.c - FPS-INTERP Aufzeichnung: Listen/Ops, Tick-Puffer (VB, Stream 2, Konstanten, GPU-Kopien), Draw-/Kopie-Aufzeichnung.
 * W463 RCORE: aus render_interp.c ausgegliedert (reines Verschieben); gemeinsamer Zustand in
 * render_interp_int.h, Modulkarte docs/RENDER_ARCH.md. */
#include "render_interp_int.h"
static unsigned ip_ccap[IP_LISTS];
float *ip_cw[IP_LISTS];
float (*ip_cj[IP_LISTS])[2];
unsigned char *ip_cp2[IP_LISTS]; /* 1 = w aus g_ip_tw (P2 im RT) */
unsigned ip_cwcap[IP_LISTS];
unsigned st_eyex_draws, st_eyex_verts, st_eyex_ln;
/* W237: Vertex-Sprung-Statistik in zwischengerechneten Draws */
unsigned st_p2x_draws, st_p2x_verts, st_p2x_hit, st_jmp_verts, st_jmp_fp, st_p2x_ln;
unsigned st_p2rej; /* W406 interp_p2rej: P2-Draws statt reject gelerpt */
unsigned st_p2hom[2]; /* W429 interp_p2hom: reject mit gleichen Eingangsdaten gelerpt [0] 3D, [1] P2 */
unsigned st_jf[IP_W_N], st_jb[IP_W_N], st_jfd[IP_W_N]; /* W237: Sprung vor (beide w>0) / hinter der Kamera je why; Draws mit Sprung vorn */
unsigned st_wwhy[IP_W_N];
static void ip_cw_rec(int li, unsigned vbcap, unsigned vstart, unsigned n, const float mvp[16],
                      const im_vertex_t *v, const im_vertex_t *src, unsigned prim, unsigned nsrc,
                      const float *tw, unsigned twn, unsigned twp)
{
    unsigned k;
    if (ip_cwcap[li] < vbcap) {
        free(ip_cw[li]); free(ip_cj[li]); free(ip_cp2[li]);
        ip_cw[li] = (float *)malloc((size_t)vbcap * sizeof(float));
        ip_cj[li] = (float (*)[2])malloc((size_t)vbcap * sizeof ip_cj[li][0]);
        ip_cp2[li] = (unsigned char *)malloc((size_t)vbcap);
        ip_cwcap[li] = (ip_cw[li] && ip_cj[li] && ip_cp2[li]) ? vbcap : 0u;
    }
    if (!ip_cwcap[li] || vstart + n > ip_cwcap[li]) { return; }
    for (k = 0; k < n; k++) {
        const float *p = v ? v[k].pos : src[sb_im_exp_src(prim, nsrc, k)].pos;
        float cx = p[0] * mvp[0] + p[1] * mvp[4] + p[2] * mvp[8] + p[3] * mvp[12];
        float cy = p[0] * mvp[1] + p[1] * mvp[5] + p[2] * mvp[9] + p[3] * mvp[13];
        float cw = p[0] * mvp[3] + p[1] * mvp[7] + p[2] * mvp[11] + p[3] * mvp[15];
        ip_cj[li][vstart + k][0] = cw != 0.0f ? cx / cw : 0.0f;
        ip_cj[li][vstart + k][1] = cw != 0.0f ? cy / cw : 0.0f;
        ip_cp2[li][vstart + k] = 0;
        if (tw) {
            unsigned s = sb_im_exp_src(twp, twn, k);
            if (s < twn) { cw = tw[s]; ip_cp2[li][vstart + k] = 1; }
        }
        ip_cw[li][vstart + k] = cw;
    }
}

ip_list_t g_ipl[IP_LISTS];
static ID3D11Buffer *ip_hw_one; /* FIX(W237): w = 1 (Stride 0) */

/* FIX(W237 LINE-FLICKER2) interp_hlerp: w der n Tick-VB-Vertices ab vstart merken. P2-Draws im RT
 * (render_im.c teilt dort schon durch w, Strom w = 1) liefern die echte Clip-w ihrer Quell-Vertices
 * (tw, Expansion wie im Tick-VB); alle anderen Draws tragen w schon im Strom -> 1.0. */
static void ip_w_rec(ip_list_t *L, unsigned vstart, unsigned n, const float *tw, unsigned twn, unsigned twp)
{
    unsigned k;
    float *d;
    if (L->ipw_bad) { return; }
    if (L->ipwcap < L->vbcap) {
        float *nw = (float *)realloc(L->ipw, (size_t)L->vbcap * sizeof(float));
        if (!nw) { L->ipw_bad = 1; return; }
        L->ipw = nw;
        L->ipwcap = L->vbcap;
    }
    if (vstart + n > L->ipwcap) { L->ipw_bad = 1; return; }
    d = L->ipw + vstart;
    if (!tw) {
        for (k = 0; k < n; k++) { d[k] = 1.0f; }
    } else if (twp != 3u && twp != 6u && twp != 7u && twp != 8u && twp != 9u && twp != 10u && n <= twn) {
        memcpy(d, tw, (size_t)n * sizeof(float)); /* Liste: Vertex k = Quelle k */
    } else {
        for (k = 0; k < n; k++) {
            unsigned q = sb_im_exp_src(twp, twn, k);
            d[k] = q < twn ? tw[q] : 1.0f;
        }
    }
}

/* Am Swap: w des Ticks in den GPU-Puffer (ein UpdateSubresource, 4 Byte je Vertex). */
unsigned st_hw_up, st_hw_fail;
void ip_w_upload(ip_list_t *L)
{
    D3D11_BOX b;
    L->wb_ok = 0;
    if (!sb_ip_hlerp() || L->ipw_bad || !L->ipw || !L->nverts || L->nverts > L->ipwcap) {
        if (sb_ip_hlerp() && L->nverts) { st_hw_fail++; }
        return;
    }
    if (!L->wb || L->wbcap < L->nverts) {
        D3D11_BUFFER_DESC bd;
        unsigned cap = L->vbcap > L->nverts ? L->vbcap : L->nverts;
        if (L->wb) { ID3D11Buffer_Release(L->wb); L->wb = NULL; L->wbcap = 0; }
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = cap * (UINT)sizeof(float);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &L->wb))) { L->wb = NULL; st_hw_fail++; return; }
        L->wbcap = cap;
    }
    memset(&b, 0, sizeof b);
    b.right = L->nverts * (UINT)sizeof(float);
    b.bottom = 1;
    b.back = 1;
    ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)L->wb, 0, &b, L->ipw, 0, 0);
    L->wb_ok = 1;
    st_hw_up++;
}

unsigned st_hw_draws, st_hw_one;
void ip_hw_bind(ID3D11DeviceContext *c, const ip_list_t *B, const ip_list_t *A, unsigned bstart, unsigned astart)
{
    ID3D11Buffer *vb[2];
    UINT st[2], of[2];
    if (!sb_ip_hlerp() || !ip_hw_one) { return; }
    if (B->wb_ok && A->wb_ok && B->wb && A->wb) {
        vb[0] = B->wb; vb[1] = A->wb;
        st[0] = st[1] = (UINT)sizeof(float);
        of[0] = bstart * (UINT)sizeof(float); of[1] = astart * (UINT)sizeof(float);
        st_hw_draws++;
    } else {
        vb[0] = vb[1] = ip_hw_one;
        st[0] = st[1] = 0;
        of[0] = of[1] = 0;
        st_hw_one++;
    }
    ID3D11DeviceContext_IASetVertexBuffers(c, 3, 2, vb, st, of);
}
ip_sh_t g_ish[IP_SH_MAX];
int ip_rec = -1;
int ip_hist[IP_HIST], ip_nhist;
int ip_init_done, ip_init_fail;
ID3D11VertexShader *ip_vs2;
ID3D11InputLayout *ip_lay2;

/* FIX(W237) interp_hlerp: Strom 3/4 = Clip-w von Tick N (B, ab o->vstart) und N-1 (A, ab a->vstart);
 * ohne w-Puffer eines der beiden Ticks w = 1 fuer beide (alter Bildraum-Lerp). */
ID3D11Buffer *ip_cb0, *ip_cb1;
ID3D11PixelShader *ip_dbg_ps; /* DIAG: Klassifikationsbild (interp_diag>=3) */
ID3D11PixelShader *ip_id_ps;  /* DIAG(W251 INTERP-GROUND): Draw-ID-Bild (nur SV_Position, Farbe aus b1) */
unsigned ip_dd_lo, ip_dd_ser; /* DIAG(W251): Listen B (Serien lo..ser), deren Bilder ID-Bilder bekommen (interp_ddrel) */
/* DIAG(W284 INTERP-RAWCHG) interp_ddfile: Tick-Liste (relativ zum Missionsbeginn) statt eines Bereichs */
static unsigned *ip_ddf_t, ip_ddf_n;
static int ip_ddf_state; /* 0 = nicht geladen, 1 = geladen, -1 = aus */
int ip_dd_on(void) { return g_cfg.interp_ddrel > 0 || g_cfg.interp_ddfile[0] != 0; }
int ip_ddf_has(unsigned rel)
{
    unsigned k;
    if (ip_ddf_state == 0) {
        FILE *f = NULL;
        unsigned cap = 0;
        ip_ddf_state = -1;
        if (fopen_s(&f, g_cfg.interp_ddfile, "r") == 0 && f) {
            int c, have = 0;
            unsigned v = 0;
            for (;;) {
                c = fgetc(f);
                if (c >= '0' && c <= '9') { v = v * 10u + (unsigned)(c - '0'); have = 1; continue; }
                if (have) {
                    if (ip_ddf_n >= cap) {
                        unsigned nc = cap ? cap * 2u : 256u;
                        unsigned *nn = (unsigned *)realloc(ip_ddf_t, nc * sizeof *nn);
                        if (!nn) { break; }
                        ip_ddf_t = nn; cap = nc;
                    }
                    ip_ddf_t[ip_ddf_n++] = v;
                }
                v = 0; have = 0;
                if (c == EOF) { break; }
            }
            fclose(f);
            ip_ddf_state = ip_ddf_n ? 1 : -1;
        }
        HLOG_I("INTERP ddfile: %u Ticks geladen", ip_ddf_n);
    }
    if (ip_ddf_state <= 0) { return 0; }
    for (k = 0; k < ip_ddf_n; k++) { if (ip_ddf_t[k] == rel) { return 1; } }
    return 0;
}
ID3D11Buffer *ip_dbg_cb;
LARGE_INTEGER ip_qpf;
int ip_K = 1;

/* Takt (Echtzeit) */
int ip_live;           /* 1 = Interp-Takt zeigt die Bilder */

unsigned sb_interp_hash(const void *base, unsigned stride, unsigned n)
{
    const unsigned char *b = (const unsigned char *)base;
    unsigned h = 2166136261u, i, step = n > 64u ? n / 64u : 1u;
    for (i = 0; i < n; i += step) {
        const unsigned *w = (const unsigned *)(b + (size_t)i * stride);
        h = (h ^ w[0]) * 16777619u;
        h = (h ^ w[1]) * 16777619u;
        h = (h ^ w[2]) * 16777619u;
        h = (h ^ w[3]) * 16777619u;
    }
    return h ^ n;
}

static void ip_op_release(ip_op_t *o)
{
    if (o->rtv) { ID3D11RenderTargetView_Release(o->rtv); }
    if (o->dsv) { ID3D11DepthStencilView_Release(o->dsv); }
    if (o->bs) { ID3D11BlendState_Release(o->bs); }
    if (o->ds) { ID3D11DepthStencilState_Release(o->ds); }
    if (o->rs) { ID3D11RasterizerState_Release(o->rs); }
    if (o->srv) { ID3D11ShaderResourceView_Release(o->srv); }
    if (o->cps) { ID3D11RenderTargetView_Release(o->cps); } /* W340 */
    if (o->cpd) { ID3D11ShaderResourceView_Release(o->cpd); } /* W340 */
    if (o->rq) {
        int k;
        for (k = 1; k < 4; k++) { if (o->rqr.srv[k]) { ID3D11ShaderResourceView_Release(o->rqr.srv[k]); } }
    }
}

/* W361: eigene Referenzen der Liste auf Schatten-Views abgeben (vor nrefs = 0). */
void ip_list_unref(ip_list_t *L)
{
    int j;
    for (j = 0; j < L->nrefs && j < IP_REFS_MAX; j++) { ip_shref_drop(&L->refo[j]); }
}

void ip_list_reset(ip_list_t *L)
{
    unsigned i;
    ip_list_unref(L); /* W361 */
    while (L->cp_original) {
        ip_cp_t *p = L->cp_original;
        L->cp_original = p->next;
        free(p);
    }
    for (i = 0; i < L->nops; i++) {
        ip_op_release(&L->ops[i]);
    }
    L->cp_camera[0] = 0.0f; L->cp_camera[1] = L->cp_camera[2] = 1.0f;
    L->nops = 0;
    L->nverts = 0;
    L->rqxpos = 0;
    L->rqcpos = 0;
    L->overflow = 0;
    L->ready = 0;
    L->snap_ok = 0;
    L->frame = 0;
    L->serial = 0;
    L->prevser = 0;
    L->pair_ok = 0;
    L->nrefs = 0;
    L->bbref = -1;
    L->nodisc = 0;
    L->gpu_ok = 0; /* W174 */
    L->ipw_bad = 0; L->wb_ok = 0; /* W237 */
    L->ddsel = 0; /* W284 */
    L->tvb0 = 0; L->ncopy = 0; /* W340 */
    L->la_ok = 0; L->la_seq = 0; /* W376 */
    if (L->smp0) { ID3D11SamplerState_Release(L->smp0); L->smp0 = NULL; } /* W126 */
}

static ip_op_t *ip_new_op(ip_list_t *L)
{
    ip_op_t *o;
    if (L->nops >= L->capops) {
        unsigned nc = L->capops ? L->capops * 2u : 2048u;
        ip_op_t *no;
        if (nc > IP_OPS_MAX) { L->overflow = 1; return NULL; }
        no = (ip_op_t *)realloc(L->ops, nc * sizeof *no);
        if (!no) { L->overflow = 1; return NULL; }
        L->ops = no;
        L->capops = nc;
    }
    if (!L->nops && g_sba_loc && g_cfg.loc_interp_anchor) {
        /* W340 LOC-FRAMES: Tick-Anfang merken. Der Spiel-Render-Thread beginnt ~0,2 ms nach dem Feldwechsel, der den
         * Tick freigibt; am Swap (Renderdauer bis ~18 ms bei 3440x1440) ist oft schon das naechste Feld da -> die
         * Zustandszeit sprang um 16,7 ms (zustand_abw), die Zwischenbilder liefen ungleichmaessig. */
        L->tvb0 = ip_vb_get();
    }
    o = &L->ops[L->nops++];
    memset(o, 0, sizeof *o);
    o->match = -1;
    return o;
}
static ip_bset_t ip_bs[IP_BS_MAX];
static unsigned ip_bs_ser;
unsigned g_ip_bs_idle, g_ip_bs_busy, g_ip_bs_new; /* DIAG(TICK-PERF,W125): TICKBLK */
static int g_ip_bs_last_idle;

/* PERF(PERF-SPIKE,W318) interp_bsets_imm=N: ohne Render-Thread (render_thread=0,
 * AMD-Weg W302 ohne treibereigene Command Lists) spielt der Takt-Thread die
 * Zwischenbilder direkt auf dem Immediate-Context ab; bei GPU-Last liest die GPU
 * dann laenger an den Tick-Puffern, beide Reserve-Saetze sind belegt und das
 * DISCARD-Map der 40-64-MB-Puffer blockiert den Spiel-Thread (W318_g50r0:
 * ipmap 9-12 ms/Tick, 25,9 Ticks/s, 16,8 s Stillstand im Kampf M1). Dort gilt
 * mindestens N Saetze (Standard 4 = IP_BS_MAX). interp_bsets=0 bleibt "aus". */
static int ip_bsets_n(void)
{
    int n = g_cfg.interp_bsets;
    if (n > 0 && g_cfg.render_thread <= 0 && g_cfg.interp_bsets_imm > n) { n = g_cfg.interp_bsets_imm; }
    return n > IP_BS_MAX ? IP_BS_MAX : n;
}

void ip_bset_rotate(ip_list_t *L)
{
    int n = ip_bsets_n(), i, best = -1, empty = -1; /* W318: interp_bsets_imm */
    unsigned bser = 0xFFFFFFFFu;
    ip_bset_t t;
    if (n <= 0) { return; }
    if (!L->vb && !L->rqx && !L->rqc) { return; } /* erster Lauf: Liste legt neu an */
    for (i = 0; i < n; i++) {
        BOOL done = FALSE;
        if (!ip_bs[i].used) { if (empty < 0) { empty = i; } continue; }
        if (ip_bs[i].ev &&
            ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)ip_bs[i].ev, &done, sizeof done,
                                        D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && done) {
            if (ip_bs[i].ser < bser) { bser = ip_bs[i].ser; best = i; }
        }
    }
    g_ip_bs_last_idle = 0;
    if (best >= 0) { g_ip_bs_idle++; g_ip_bs_last_idle = (g_cfg.interp_bsets_nod != 0); }
    else if (empty >= 0) { best = empty; g_ip_bs_new++; } /* leerer Platz: Liste legt neue Puffer an */
    else { /* alle noch in Arbeit: aeltesten nehmen (wartet ggf. wie bisher) */
        for (i = 0; i < n; i++) { if (ip_bs[i].ser < bser) { bser = ip_bs[i].ser; best = i; } }
        g_ip_bs_busy++;
    }
    if (best < 0) { return; }
    t = ip_bs[best];
    ip_bs[best].vb = L->vb; ip_bs[best].vbcap = L->vbcap;
    ip_bs[best].rqx = L->rqx; ip_bs[best].rqxsrv = L->rqxsrv; ip_bs[best].rqxcap = L->rqxcap;
    ip_bs[best].rqc = L->rqc; ip_bs[best].rqccap = L->rqccap;
    if (!ip_bs[best].ev) {
        D3D11_QUERY_DESC qd;
        memset(&qd, 0, sizeof qd);
        qd.Query = D3D11_QUERY_EVENT;
        if (FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &ip_bs[best].ev))) { ip_bs[best].ev = NULL; }
    }
    if (ip_bs[best].ev) { ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)ip_bs[best].ev); }
    ip_bs[best].ser = ++ip_bs_ser;
    ip_bs[best].used = 1;
    if (t.used) {
        L->vb = t.vb; L->vbcap = t.vbcap;
        L->rqx = t.rqx; L->rqxsrv = t.rqxsrv; L->rqxcap = t.rqxcap;
        L->rqc = t.rqc; L->rqccap = t.rqccap;
        /* Fence fertig: die GPU liest den Satz nicht mehr -> erstes Map ab 0
         * per NO_OVERWRITE (DISCARD legt bei 40-64-MB-Puffern neuen Speicher an,
         * W125_n1: ipmap 3,5 ms/Tick trotz freiem Satz). Belegt -> DISCARD. */
        if (g_ip_bs_last_idle) { L->nodisc = (L->vb ? 1u : 0u) | (L->rqx ? 2u : 0u) | (L->rqc ? 4u : 0u); }
    } else {
        L->vb = NULL; L->vbcap = 0;
        L->rqx = NULL; L->rqxsrv = NULL; L->rqxcap = 0;
        L->rqc = NULL; L->rqccap = 0;
    }
}

/* DIAG(TICK-PERF,W125): Map der Tick-Puffer; DISCARD (erster Zugriff je Tick)
 * mit Zeitmesser TB_IPMAP (perf_blk). */
static HRESULT ip_map(ID3D11Buffer *b, D3D11_MAP t, D3D11_MAPPED_SUBRESOURCE *m)
{
    HRESULT hr;
    long long q = (t == D3D11_MAP_WRITE_DISCARD) ? TB_BEGIN() : 0;
    WD_CALL(t == D3D11_MAP_WRITE_DISCARD ? "ip_map_disc" : "ip_map", NULL,
            hr = ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)b, 0, t, 0, m)); /* DIAG(PERF-WARM,W139) */
    if (t == D3D11_MAP_WRITE_DISCARD) { TB_END(TB_IPMAP, q); }
    return hr;
}

/* PERF(PERF-WARM,W139) warm_vb=1: Tick-Puffer (VB 40-60 MB, Stream 2 64 MB,
 * Konstanten 4 MB je Liste) mit Anfangsdaten (Nullen) anlegen und ohne
 * ctx_lock. Befund W139_c0m1/W139_b0m1 (warm_diag): Das erste
 * Map(WRITE_DISCARD) eines neu angelegten Tick-Puffers dauert 8-36 ms unter
 * ctx_lock (der Treiber legt den Speicher erst dort an) - beim Missionsstart
 * und Retry je Liste einmal -> Present-Takt steht. Mit Anfangsdaten legt der
 * Treiber den Speicher schon beim Anlegen an (Device-Aufruf, frei-threaded).
 * Inhalt ohne Wirkung: jede Aufnahme schreibt ihren Bereich vor dem Lesen
 * (erstes Map je Tick ist DISCARD). Die Nullen kommen aus einem kurzlebigen
 * VirtualAlloc-Block (Nullseiten des Systems, danach freigegeben). */
static HRESULT ip_mkbuf(const D3D11_BUFFER_DESC *bd, ID3D11Buffer **out)
{
    HRESULT hr;
    int cr;
    void *z = NULL;
    D3D11_SUBRESOURCE_DATA sd;
    if (g_cfg.warm_vb <= 0) { return ID3D11Device_CreateBuffer(g_dev, bd, NULL, out); }
    cr = sb_ctx_release_all();
    z = VirtualAlloc(NULL, bd->ByteWidth, MEM_COMMIT | MEM_RESERVE, PAGE_READONLY);
    memset(&sd, 0, sizeof sd);
    sd.pSysMem = z;
    hr = ID3D11Device_CreateBuffer(g_dev, bd, z ? &sd : NULL, out);
    if (z) { VirtualFree(z, 0, MEM_RELEASE); }
    sb_ctx_reacquire(cr);
    return hr;
}

/* Tick-VB anlegen/vergroessern (beim Start einer Aufnahme). */
int ip_vb_ensure(ip_list_t *L)
{
    D3D11_BUFFER_DESC bd;
    if (!L->want) { /* PERF(PERF-BENCH,W137): interp_vb_kv = Startgroesse in 1024 Vertices (0 = alt 1024K) */
        L->want = g_cfg.interp_vb_kv > 0 ? (unsigned)g_cfg.interp_vb_kv * 1024u : IP_VERTS_INIT;
        if (L->want > IP_VERTS_MAX) { L->want = IP_VERTS_MAX; }
    }
    if (L->vb && L->vbcap >= L->want) { return 1; }
    L->nodisc &= ~1u; /* W125: neuer Puffer -> erstes Map DISCARD */
    if (L->vb) { ID3D11Buffer_Release(L->vb); L->vb = NULL; L->vbcap = 0; }
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = L->want * (unsigned)sizeof(im_vertex_t);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    { int cr = g_cfg.warm_vb > 0 ? 0 : sb_ctx_release_all(); /* W138: 40-60-MB-Puffer ohne ctx_lock anlegen (Device; L ist die neue Aufnahme, nicht in der Historie) */
    HRESULT hr = ip_mkbuf(&bd, &L->vb); /* W139 warm_vb: mit Anfangsdaten, gibt selbst ab */
    sb_ctx_reacquire(cr);
    if (FAILED(hr)) {
        L->vb = NULL;
        return 0;
    } }
    L->vbcap = L->want;
    HLOG_I("INTERP: Tick-VB %u Vertices (%u MB)", L->vbcap,
           (unsigned)((unsigned long long)L->vbcap * sizeof(im_vertex_t) >> 20));
    return 1;
}
int ip_rq_ensure(ip_list_t *L)
{
    D3D11_BUFFER_DESC bd;
    if (!g_cfg.shading) { return 1; }
    if (!L->rqxwant) { L->rqxwant = IP_RQX_INIT; }
    if (!L->rqcwant) { L->rqcwant = IP_RQC_INIT; }
    if (!L->rqx || L->rqxcap < L->rqxwant) {
        L->nodisc &= ~2u; /* W125 */
        if (L->rqxsrv) { ID3D11ShaderResourceView_Release(L->rqxsrv); L->rqxsrv = NULL; }
        if (L->rqx) { ID3D11Buffer_Release(L->rqx); L->rqx = NULL; L->rqxcap = 0; }
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = L->rqxwant;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        /* SHADE-PERF (W80): auch als Buffer<uint> (Roh-Eingaenge, VS-t0) */
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER | (rq_xsrv_supported() ? D3D11_BIND_SHADER_RESOURCE : 0u);
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ip_mkbuf(&bd, &L->rqx))) { L->rqx = NULL; return 0; } /* W139 warm_vb */
        L->rqxcap = L->rqxwant;
        if (rq_xsrv_supported()) { L->rqxsrv = rq_mksrv(L->rqx, L->rqxcap); }
    }
    if (!L->rqc || L->rqccap < L->rqcwant) {
        L->nodisc &= ~4u; /* W125 */
        if (L->rqc) { ID3D11Buffer_Release(L->rqc); L->rqc = NULL; L->rqccap = 0; }
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = L->rqcwant * 16u;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ip_mkbuf(&bd, &L->rqc))) { L->rqc = NULL; return 0; } /* W139 warm_vb */
        L->rqccap = L->rqcwant;
    }
    return 1;
}

/* PERF(PERF-M2,W174) ip_vb_gpu: Die Tick-Puffer sind DYNAMIC (CPU schreibt,
 * Treiber legt sie i. d. R. in Systemspeicher). Jede Zwischenbild-Wiedergabe
 * liest den ganzen Tick (lerp-Draws zwei Stroeme) erneut ueber PCIe. Am Swap
 * eine GPU-Kopie in einen DEFAULT-Puffer der Liste (einmal je Tick); die
 * Wiedergabe liest daraus. Inhalt und Offsets gleich -> gleiche Bilder.
 * Bits: 1 VB, 2 Stream 2/Roh-Eingaenge (rqx), 4 Konstanten-Fenster (rqc). */
unsigned st_vbg_copy, st_vbg_fail;
double st_vbg_mb;
static ID3D11Query *gc_dj[GC_N], *gc_t0[GC_N], *gc_t1[GC_N];
static int gc_st[GC_N], gc_fail;
double gc_ms, gc_mx;
unsigned gc_n;
static void gc_poll(void)
{
    int k;
    for (k = 0; k < GC_N; k++) {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        UINT64 t0, t1;
        if (gc_st[k] != 2) { continue; }
        if (ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)gc_dj[k], &dj, sizeof dj,
                                        D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) { continue; }
        gc_st[k] = 0;
        if (dj.Disjoint || !dj.Frequency ||
            ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)gc_t0[k], &t0, sizeof t0, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)gc_t1[k], &t1, sizeof t1, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            t1 < t0) { continue; }
        {
            double ms = (double)(t1 - t0) * 1000.0 / (double)dj.Frequency;
            gc_ms += ms; gc_n++;
            if (ms > gc_mx) { gc_mx = ms; }
        }
    }
}
static int gc_begin(void)
{
    int k;
    if (g_cfg.ip_gdiag <= 0 || gc_fail) { return -1; }
    gc_poll();
    for (k = 0; k < GC_N; k++) { if (gc_st[k] == 0) { break; } }
    if (k >= GC_N) { return -1; }
    if (!gc_dj[k]) {
        D3D11_QUERY_DESC qd;
        memset(&qd, 0, sizeof qd);
        qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        if (FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &gc_dj[k]))) { gc_fail = 1; return -1; }
        qd.Query = D3D11_QUERY_TIMESTAMP;
        if (FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &gc_t0[k])) ||
            FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &gc_t1[k]))) { gc_fail = 1; return -1; }
    }
    ID3D11DeviceContext_Begin(g_ctx, (ID3D11Asynchronous *)gc_dj[k]);
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)gc_t0[k]);
    return k;
}
static void gc_end(int k)
{
    if (k < 0) { return; }
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)gc_t1[k]);
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)gc_dj[k]);
    gc_st[k] = 2;
}
static ID3D11Buffer *ip_mk_default(unsigned bytes, UINT bind)
{
    D3D11_BUFFER_DESC bd;
    ID3D11Buffer *b = NULL;
    int cr = sb_ctx_release_all(); /* Device-Aufruf ohne ctx_lock (40-64 MB) */
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = bytes;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = bind;
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &b))) { b = NULL; }
    sb_ctx_reacquire(cr);
    return b;
}

/* Neuanlage (Tick-VB gewachsen, z. B. M2 60 -> 90 MB): Ein DEFAULT-Puffer
 * dieser Groesse kostet beim Anlegen 65-75 ms (W174_r1) - am Swap haette das den
 * Tick aufgehalten. Daher im Hilfs-Thread (Device-Aufruf, frei-threaded);
 * bis zur Abholung liest die Wiedergabe dieser Liste wie bisher den DYNAMIC-
 * Puffer (gleiche Daten). Beim ersten Present (sb_interp_prealloc) synchron. */
unsigned st_vbg_async;
static DWORD WINAPI ip_ga_work(LPVOID p)
{
    ip_list_t *L = (ip_list_t *)p;
    if (L->ga_vbcap) { L->ga_vbg = ip_mk_default(L->ga_vbcap * (unsigned)sizeof(im_vertex_t), D3D11_BIND_VERTEX_BUFFER); }
    if (L->ga_rqxcap) {
        L->ga_rqxg = ip_mk_default(L->ga_rqxcap, D3D11_BIND_VERTEX_BUFFER | (rq_xsrv_supported() ? D3D11_BIND_SHADER_RESOURCE : 0u));
        if (L->ga_rqxg && rq_xsrv_supported()) { L->ga_srv = rq_mksrv(L->ga_rqxg, L->ga_rqxcap); }
    }
    InterlockedExchange(&L->ga_st, 2);
    return 0;
}

static void ip_ga_adopt(ip_list_t *L)
{
    if (L->ga_vbg) {
        if (L->vbg) { ID3D11Buffer_Release(L->vbg); }
        L->vbg = L->ga_vbg; L->vbgcap = L->ga_vbcap;
        HLOG_I("INTERP vb_gpu: GPU-VB Liste %d %u Vertices (%u MB, Hilfs-Thread)", (int)(L - g_ipl), L->vbgcap,
               (unsigned)(((unsigned long long)L->vbgcap * sizeof(im_vertex_t)) >> 20));
    } else if (L->ga_vbcap) { st_vbg_fail++; }
    if (L->ga_rqxg && (L->ga_srv || !rq_xsrv_supported())) {
        if (L->rqxgsrv) { ID3D11ShaderResourceView_Release(L->rqxgsrv); }
        if (L->rqxg) { ID3D11Buffer_Release(L->rqxg); }
        L->rqxg = L->ga_rqxg; L->rqxgsrv = L->ga_srv; L->rqxgcap = L->ga_rqxcap;
    } else {
        if (L->ga_srv) { ID3D11ShaderResourceView_Release(L->ga_srv); }
        if (L->ga_rqxg) { ID3D11Buffer_Release(L->ga_rqxg); }
        if (L->ga_rqxcap) { st_vbg_fail++; }
    }
    L->ga_vbg = NULL; L->ga_rqxg = NULL; L->ga_srv = NULL;
    L->ga_vbcap = L->ga_rqxcap = 0;
    InterlockedExchange(&L->ga_st, 0);
}

static void ip_gpu_alloc(ip_list_t *L, int async)
{
    unsigned want = (unsigned)g_cfg.ip_vb_gpu;
    int needvb, needrq;
    if (!want || !g_dev) { return; }
    if (L->ga_st == 2) { ip_ga_adopt(L); }
    needvb = (want & 1u) && L->vb && (!L->vbg || L->vbgcap < L->vbcap);
    needrq = (want & 2u) && L->rqx && (!L->rqxg || L->rqxgcap < L->rqxcap);
    if (!needvb && !needrq) { return; }
    if (async) {
        if (L->ga_st != 0) { return; } /* laeuft schon */
        L->ga_vbcap = needvb ? L->vbcap : 0u;
        L->ga_rqxcap = needrq ? L->rqxcap : 0u;
        L->ga_vbg = NULL; L->ga_rqxg = NULL; L->ga_srv = NULL;
        InterlockedExchange(&L->ga_st, 1);
        if (!QueueUserWorkItem(ip_ga_work, L, WT_EXECUTELONGFUNCTION)) {
            L->ga_vbcap = L->ga_rqxcap = 0;
            InterlockedExchange(&L->ga_st, 0);
            st_vbg_fail++;
            return;
        }
        st_vbg_async++;
        return;
    }
    if (needvb) {
        if (L->vbg) { ID3D11Buffer_Release(L->vbg); L->vbg = NULL; L->vbgcap = 0; }
        L->vbg = ip_mk_default(L->vbcap * (unsigned)sizeof(im_vertex_t), D3D11_BIND_VERTEX_BUFFER);
        if (L->vbg) {
            L->vbgcap = L->vbcap;
            HLOG_I("INTERP vb_gpu: GPU-VB Liste %d %u Vertices (%u MB)", (int)(L - g_ipl), L->vbgcap,
                   (unsigned)(((unsigned long long)L->vbgcap * sizeof(im_vertex_t)) >> 20));
        } else { st_vbg_fail++; }
    }
    if (needrq) {
        if (L->rqxgsrv) { ID3D11ShaderResourceView_Release(L->rqxgsrv); L->rqxgsrv = NULL; }
        if (L->rqxg) { ID3D11Buffer_Release(L->rqxg); L->rqxg = NULL; L->rqxgcap = 0; }
        L->rqxg = ip_mk_default(L->rqxcap, D3D11_BIND_VERTEX_BUFFER |
                                               (rq_xsrv_supported() ? D3D11_BIND_SHADER_RESOURCE : 0u));
        if (L->rqxg) {
            L->rqxgcap = L->rqxcap;
            if (rq_xsrv_supported()) { L->rqxgsrv = rq_mksrv(L->rqxg, L->rqxgcap); }
        } else { st_vbg_fail++; }
    }
}

void ip_gpu_copy(ip_list_t *L)
{
    D3D11_BOX bx;
    unsigned want = (unsigned)g_cfg.ip_vb_gpu;
    int gk = -1;
    L->gpu_ok = 0;
    if (!want || !g_ctx) { return; }
    ip_gpu_alloc(L, 1); /* fehlende/zu kleine Kopien im Hilfs-Thread anlegen (sonst schon in sb_interp_prealloc) */
    memset(&bx, 0, sizeof bx);
    bx.bottom = 1; bx.back = 1;
    if ((want & 1u) && L->vb && L->nverts) {
        if (L->vbg && L->vbgcap >= L->nverts) { /* alte Kopie reicht, solange der Tick hineinpasst */
            gk = gc_begin(); /* DIAG(PERF-M2,W174) */
            bx.left = 0; bx.right = L->nverts * (unsigned)sizeof(im_vertex_t);
            ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)L->vbg, 0, 0, 0, 0,
                                                      (ID3D11Resource *)L->vb, 0, &bx);
            L->gpu_ok |= 1u;
            st_vbg_copy++;
            st_vbg_mb += (double)bx.right / 1048576.0;
        }
    }
    if ((want & 2u) && L->rqx && L->rqxpos) {
        if (L->rqxg && L->rqxgcap >= L->rqxpos && (L->rqxgsrv || !L->rqxsrv)) {
            bx.left = 0; bx.right = L->rqxpos > L->rqxcap ? L->rqxcap : L->rqxpos;
            ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)L->rqxg, 0, 0, 0, 0,
                                                      (ID3D11Resource *)L->rqx, 0, &bx);
            L->gpu_ok |= 2u;
            st_vbg_mb += (double)bx.right / 1048576.0;
        }
    }
    gc_end(gk);
}

/* Puffer der Wiedergabe: GPU-Kopie, wenn vorhanden und gewuenscht. */
ID3D11Buffer *ip_vbsel(const ip_list_t *L, int g)
{
    return (g && (L->gpu_ok & 1u)) ? L->vbg : L->vb;
}

/* PERF(PERF-WARM,W139) warm_vbpre=1: alle Tick-Puffersaetze (IP_LISTS Listen +
 * interp_bsets Reserve-Saetze, je VB 40-60 MB + Stream 2 64 MB + Konstanten
 * 4 MB) schon beim ersten Present mit Offscreen-BB (Bootbild, unsichtbar und
 * ungemessen) anlegen und je einmal per Map(DISCARD)/Unmap beruehren.
 * Befund W139_w7m1: Beim ersten Missionsstart legte die Aufnahme sechs Saetze
 * an; waehrend der Anlage (Device-Aufruf ohne ctx_lock) blockierte der Treiber
 * auch die Map-Aufrufe des Present-Takt-Threads (ip_cb_map 28-36 ms) und das
 * erste Map(DISCARD) je neuem Puffer dauerte 8-36 ms. Beim Retry existieren die
 * Puffer schon (dort gruen). Inhalt ohne Wirkung (jede Aufnahme schreibt ihren
 * Bereich vor dem Lesen); Reserve-Saetze erhalten ihren Fence wie in
 * ip_bset_rotate (gleich fertig -> NO_OVERWRITE wie ein freier Satz). */
static void ip_touch(ID3D11Buffer *b)
{
    D3D11_MAPPED_SUBRESOURCE m;
    if (b && SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)b, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)b, 0);
    }
}

void sb_interp_prealloc(void)
{
    static int done;
    static ip_list_t T;
    int i, k, n, nl = 0, ns = 0;
    LARGE_INTEGER f, q0, q1;
    if (done || g_cfg.warm_vbpre <= 0 || g_cfg.interp_fps <= 0 || !g_dev || !g_ctx) { return; }
    done = 1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&q0);
    sb_ctx_enter();
    for (i = 0; i < IP_LISTS; i++) {
        ip_list_t *L = &g_ipl[i];
        if (!ip_vb_ensure(L) || !ip_rq_ensure(L)) { break; }
        ip_touch(L->vb); ip_touch(L->rqx); ip_touch(L->rqc);
        nl++;
        ip_gpu_alloc(L, 0); /* PERF(PERF-M2,W174) ip_vb_gpu: GPU-Kopien gleich mit anlegen */
    }
    n = ip_bsets_n(); /* W318: interp_bsets_imm */
    for (k = 0; k < n && nl == IP_LISTS; k++) {
        if (ip_bs[k].used) { continue; }
        memset(&T, 0, sizeof T);
        if (!ip_vb_ensure(&T) || !ip_rq_ensure(&T)) { break; }
        ip_touch(T.vb); ip_touch(T.rqx); ip_touch(T.rqc);
        ip_bs[k].vb = T.vb; ip_bs[k].vbcap = T.vbcap;
        ip_bs[k].rqx = T.rqx; ip_bs[k].rqxsrv = T.rqxsrv; ip_bs[k].rqxcap = T.rqxcap;
        ip_bs[k].rqc = T.rqc; ip_bs[k].rqccap = T.rqccap;
        if (!ip_bs[k].ev) {
            D3D11_QUERY_DESC qd;
            memset(&qd, 0, sizeof qd);
            qd.Query = D3D11_QUERY_EVENT;
            if (FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &ip_bs[k].ev))) { ip_bs[k].ev = NULL; }
        }
        if (ip_bs[k].ev) { ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)ip_bs[k].ev); }
        ip_bs[k].ser = ++ip_bs_ser;
        ip_bs[k].used = 1;
        ns++;
    }
    sb_ctx_leave();
    QueryPerformanceCounter(&q1);
    HLOG_I("INTERP: Tick-Puffer vorab angelegt (W139 warm_vbpre): %d Listen + %d Reserve-Saetze in %.1f ms", nl, ns,
           (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)f.QuadPart);
}

/* SHADE-PERF (W80): Platz im Tick-Puffer direkt beschreiben (ein Map, der
 * Aufrufer schreibt und ruft Unmap auf *buf). NULL = kein Platz. */
void *sb_interp_rq_xmap(unsigned bytes, ID3D11Buffer **buf, UINT *off, ID3D11ShaderResourceView **srv)
{
    ip_list_t *L;
    D3D11_MAPPED_SUBRESOURCE m;
    unsigned pos;
    if (ip_rec < 0 || !bytes) { return NULL; }
    L = &g_ipl[ip_rec];
    if (L->overflow || !L->rqx) { return NULL; }
    pos = L->rqxpos;
    if (pos + bytes > L->rqxcap) {
        L->overflow = 1;
        if (L->rqxwant < IP_RQX_MAX) { L->rqxwant += L->rqxwant / 2u; }
        return NULL;
    }
    if (FAILED(ip_map(L->rqx, (pos || (L->nodisc & 2u)) ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD, &m))) {
        L->overflow = 1;
        return NULL;
    }
    *buf = L->rqx;
    *off = pos;
    *srv = L->rqxsrv;
    L->rqxpos = pos + ((bytes + 255u) & ~255u);
    return (unsigned char *)m.pData + pos;
}

int sb_interp_rq_xwrite(const void *data, unsigned bytes, ID3D11Buffer **buf, UINT *off)
{
    ip_list_t *L;
    D3D11_MAPPED_SUBRESOURCE m;
    unsigned pos;
    if (ip_rec < 0 || !bytes) { return 0; }
    L = &g_ipl[ip_rec];
    if (L->overflow || !L->rqx) { return 0; }
    pos = L->rqxpos;
    if (pos + bytes > L->rqxcap) {
        L->overflow = 1;
        if (L->rqxwant < IP_RQX_MAX) { L->rqxwant += L->rqxwant / 2u; }
        return 0;
    }
    if (FAILED(ip_map(L->rqx, (pos || (L->nodisc & 2u)) ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD, &m))) {
        L->overflow = 1;
        return 0;
    }
    memcpy((unsigned char *)m.pData + pos, data, bytes);
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)L->rqx, 0);
    *buf = L->rqx;
    *off = pos;
    L->rqxpos = pos + ((bytes + 255u) & ~255u);
    return 1;
}

/* SHADE-PERF (W80): aktueller Konstanten-Tick-Puffer + Schreibposition
 * (NULL = keine Aufnahme bzw. Ueberlauf). */
ID3D11Buffer *sb_interp_rq_cpos(UINT *pos)
{
    ip_list_t *L;
    if (ip_rec < 0) { return NULL; }
    L = &g_ipl[ip_rec];
    if (L->overflow || !L->rqc) { return NULL; }
    *pos = L->rqcpos;
    return L->rqc;
}

int sb_interp_rq_cwrite(const void *data, unsigned nconst, ID3D11Buffer **buf, UINT *first)
{
    ip_list_t *L;
    D3D11_MAPPED_SUBRESOURCE m;
    unsigned pos;
    if (ip_rec < 0 || !nconst) { return 0; }
    L = &g_ipl[ip_rec];
    if (L->overflow || !L->rqc) { return 0; }
    pos = L->rqcpos;
    if (pos + nconst > L->rqccap) {
        L->overflow = 1;
        if (L->rqcwant < IP_RQC_MAX) { L->rqcwant += L->rqcwant / 2u; }
        return 0;
    }
    if (FAILED(ip_map(L->rqc, (pos || (L->nodisc & 4u)) ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD, &m))) {
        L->overflow = 1;
        return 0;
    }
    memcpy((unsigned char *)m.pData + (size_t)pos * 16u, data, (size_t)nconst * 16u);
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)L->rqc, 0);
    *buf = L->rqc;
    *first = pos;
    L->rqcpos = pos + nconst;
    return 1;
}

/* Draw-Vertices in den Tick-VB der laufenden Aufnahme schreiben (statt in den
 * IM-Ring): der Spiel-Draw liest von dort, die Wiedergabe ebenso -> keine
 * Kopie. 1 = geschrieben (*vb/*start gesetzt), 0 = alter Ring-Pfad. */
/* PERF(IM-PERF,W99): Tick-VB-Bereich fuer n Vertices mappen (Aufrufer
 * schreibt direkt, z. B. die Primitiv-Expansion), danach sb_interp_vb_end.
 * NULL = alter Ring-Pfad (Ueberlauf wie in sb_interp_vb_write markiert). */
im_vertex_t *sb_interp_vb_begin(unsigned n, ID3D11Buffer **vb, unsigned *start)
{
    ip_list_t *L;
    D3D11_MAPPED_SUBRESOURCE m;
    if (ip_rec < 0 || !n) { return NULL; }
    L = &g_ipl[ip_rec];
    if (L->overflow || !L->vb) { return NULL; }
    if (L->nverts + n > L->vbcap) {
        L->overflow = 1;
        if (L->want < IP_VERTS_MAX) { L->want = L->want + L->want / 2u; }
        return NULL;
    }
    if (FAILED(ip_map(L->vb, (L->nverts || (L->nodisc & 1u)) ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD, &m))) {
        L->overflow = 1;
        return NULL;
    }
    *vb = L->vb;
    *start = L->nverts;
    return (im_vertex_t *)m.pData + L->nverts;
}
void sb_interp_vb_end(unsigned n)
{
    ip_list_t *L = &g_ipl[ip_rec];
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)L->vb, 0);
    L->nverts += n;
}

int sb_interp_vb_write(const im_vertex_t *v, unsigned n, ID3D11Buffer **vb, unsigned *start)
{
    ip_list_t *L;
    D3D11_MAPPED_SUBRESOURCE m;
    if (ip_rec < 0 || !n) { return 0; }
    L = &g_ipl[ip_rec];
    if (L->overflow || !L->vb) { return 0; }
    if (L->nverts + n > L->vbcap) {
        L->overflow = 1;
        if (L->want < IP_VERTS_MAX) { L->want = L->want + L->want / 2u; }
        return 0;
    }
    if (FAILED(ip_map(L->vb, (L->nverts || (L->nodisc & 1u)) ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD, &m))) {
        L->overflow = 1;
        return 0;
    }
    memcpy((im_vertex_t *)m.pData + L->nverts, v, (size_t)n * sizeof *v);
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)L->vb, 0);
    if (g_cfg.interp_diag >= 3) {
        int li = ip_rec;
        unsigned k;
        if (ip_ccap[li] < L->vbcap) {
            free(ip_cpos[li]);
            ip_cpos[li] = (float (*)[4])malloc((size_t)L->vbcap * sizeof ip_cpos[li][0]);
            ip_ccap[li] = ip_cpos[li] ? L->vbcap : 0u;
        }
        if (ip_cpos[li]) {
            for (k = 0; k < n; k++) { memcpy(ip_cpos[li][L->nverts + k], v[k].pos, 16); }
        }
    }
    *vb = L->vb;
    *start = L->nverts;
    L->nverts += n;
    return 1;
}

/* Interp-VS (Stream 0 = Tick N, Stream 1 = Tick N-1), b0 wie IM-Pfad. */
static const char *kIP_VS =
    "cbuffer CB : register(b0) { row_major float4x4 mvp; uint has_tex; };\n"
    "cbuffer IP : register(b1) { row_major float4x4 mvp0; float ip_t; };\n"
    "struct VSIN { float4 pos : POSITION0; float4 col : COLOR0;\n"
    "              float2 uv : TEXCOORD0; float4 pos0 : POSITION1;\n"
    "#ifdef IP_HW\n" /* FIX(W237): echte Clip-w Tick N / N-1 (Strom 3/4) */
    "              float hw1 : IPW0; float hw0 : IPW1;\n"
    "#endif\n"
    "};\n"
    "struct VSOUT { float4 pos : SV_Position; float4 col : COLOR0;\n"
    "               float2 uv : TEXCOORD0;\n"
    "#ifdef IP_GUARD\n" /* FIX(W265 LINE-GUARD): Lagen fuer den Dreieck-Waechter (GS) */
    "               float4 g1 : IPGA; float4 g0 : IPGB; float2 gw : IPGW;\n"
    "#endif\n"
    "};\n"
    "VSOUT main(VSIN i) { VSOUT o;\n"
    "  float4 c0 = mul(i.pos0, mvp0), c1 = mul(i.pos, mvp);\n"
    "#ifdef IP_GUARD\n"
    "  o.g0 = c0; o.g1 = c1;\n" /* Strom-Lagen N-1/N (g1 = exakt die Tick-N-Ausgabe) */
    "#endif\n"
    "  float ew0 = c0.w, ew1 = c1.w;\n" /* w fuer die W224-Pruefung */
    "#ifdef IP_HW\n" /* FIX(W237 LINE-FLICKER2): P2 im RT (Strom w = 1): echte Clip-w aus Strom 3/4 */
    "  ew0 *= i.hw0; ew1 *= i.hw1;\n"
    "#ifdef IP_HWH\n" /* interp_hlerp=2: homogener Lerp im Clip-Raum */
    "  bool pre = (c0.w == 1.0 && c1.w == 1.0);\n" /* Strom schon durch w geteilt (wie das exakte Bild) */
    "  c0 *= i.hw0; c1 *= i.hw1;\n"
    "#endif\n"
    "#endif\n"
    "  o.pos = c0 + ip_t * (c1 - c0);\n" /* gleiche Komponenten bleiben bitgenau (W73) */
    "#ifdef IP_EYE\n" /* FIX(W224 LINE-FLICKER): w-Wechsel -> Tick N (kein Lauf durch w=0) */
    "  if (ew0 * ew1 <= 0.0) { o.pos = c1; }\n"
    "#ifdef IP_JUMP\n" /* FIX(W237): unplausibler Bildsprung vor der Kamera (Nahebene, Fehlpaarung) -> Tick N (nicht im W429-Modus 3) */
    "  else if (ew0 > 0.0 && ew1 > 0.0) {\n"
    "    float2 n0 = c0.xy / c0.w, n1 = c1.xy / c1.w;\n"
    "    if (any(abs(n1 - n0) > IP_JUMP * (1.0 + max(abs(n0), abs(n1))))) { o.pos = c1; } }\n"
    "#endif\n"
    "#endif\n"
    "#ifdef IP_HWH\n" /* W237: wieder durch w teilen -> gleiche Rasterung (w = 1, affine Attribute) wie das Tick-Bild */
    "  if (pre) { o.pos = float4(o.pos.xyz / o.pos.w, 1.0); }\n"
    "#endif\n"
    "#ifdef IP_GUARD\n"
    "  o.gw = float2(ew0, ew1);\n"
    "#endif\n"
    "  o.col = i.col; o.uv = i.uv; return o; }\n";

/* FIX(W265 LINE-GUARD): Dreieck-Waechter fuer gelerpte Dreiecke im Zwischenbild (Geometry Shader).
 * Der Lerp-VS (IM bzw. Shading, IP_GUARD/Flag 16) liefert je Vertex zusaetzlich die Strom-Lagen
 * g0 (Tick N-1) und g1 (Tick N, bitgleich zur exakten Ausgabe) sowie die (echten) w beider Ticks.
 * Je Dreieck: Regel 1 = ein Vertex wechselt zwischen N-1 und N das w-Vorzeichen; liegen alle
 * Vertices in beiden Ticks vor der Kamera, Regel 2 = Bildflaeche bei t > k_A * max(Flaeche N-1,
 * Flaeche N) + A_min, Regel 4 = Umklappen (N-1 und N gleiche Orientierung, t umgekehrt),
 * Regel 8 = eine Kante bei t > k_E * max(Laenge N-1, Laenge N) + L_min (NDC). Trifft eine Regel
 * aus GMASK, wird das GANZE Dreieck in Tick-N-Lage gezeichnet (nicht verworfen: keine Loecher,
 * Sichtbarkeit/Umlauf wie im Tick-Bild). Flaechenverhaeltnisse sind unabhaengig vom
 * Seitenverhaeltnis. GDIAG: Treffer der Regelmaske als zusaetzliches, entartetes Dreieck ausserhalb
 * des Bildes (Pipeline-Statistik GSPrimitives - GSInvocations = Treffer, Bild unveraendert). */
static const char *kIP_GS =
    "#ifdef GS_SHADE\n"
    "#define GSX float4 spec : COLOR1; float4 t0 : TEXCOORD1; float4 t1 : TEXCOORD2; float4 t2 : TEXCOORD3; float4 t3 : TEXCOORD4; float fog : TEXCOORD5;\n"
    "#define GSCP(o, v) o.spec = v.spec; o.t0 = v.t0; o.t1 = v.t1; o.t2 = v.t2; o.t3 = v.t3; o.fog = v.fog;\n"
    "#else\n"
    "#define GSX\n"
    "#define GSCP(o, v)\n"
    "#endif\n"
    "struct GSI { float4 pos : SV_Position; float4 col : COLOR0; float2 uv : TEXCOORD0; GSX\n"
    "             float4 g1 : IPGA; float4 g0 : IPGB; float2 gw : IPGW; };\n"
    "struct GSO { float4 pos : SV_Position; float4 col : COLOR0; float2 uv : TEXCOORD0; GSX };\n"
    "float gar(float2 a, float2 b, float2 c) { return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); }\n"
    "uint grules(GSI i[3]) {\n"
    "  uint r = 0u; int k;\n"
    "  [unroll] for (k = 0; k < 3; k++) { if (i[k].gw.x * i[k].gw.y <= 0.0) r |= 1u; }\n"
    "  float wm = min(min(min(i[0].g0.w, i[1].g0.w), i[2].g0.w), min(min(i[0].g1.w, i[1].g1.w), i[2].g1.w));\n"
    "  float wt = min(min(i[0].pos.w, i[1].pos.w), i[2].pos.w);\n"
    "  if (wm <= GW_EPS || wt <= GW_EPS) return r;\n"
    "  float2 p0[3], p1[3], pt[3];\n"
    "  [unroll] for (k = 0; k < 3; k++) { p0[k] = i[k].g0.xy / i[k].g0.w; p1[k] = i[k].g1.xy / i[k].g1.w;\n"
    "    pt[k] = i[k].pos.xy / i[k].pos.w; }\n"
    "  float a0 = gar(p0[0], p0[1], p0[2]), a1 = gar(p1[0], p1[1], p1[2]), at = gar(pt[0], pt[1], pt[2]);\n"
    "  if (abs(at) > GK_A * max(abs(a0), abs(a1)) + GA_MIN) r |= 2u;\n"
    "  if (a0 * a1 > 0.0 && at * a0 < 0.0 && abs(at) > GA_MIN) r |= 4u;\n"
    "  [unroll] for (k = 0; k < 3; k++) { int j = k == 2 ? 0 : k + 1;\n"
    "    float lt = length(pt[j] - pt[k]), lm = max(length(p0[j] - p0[k]), length(p1[j] - p1[k]));\n"
    "    if (lt > GK_E * lm + GL_MIN) r |= 8u; }\n"
    "  return r; }\n"
    "[maxvertexcount(GMAXV)]\n"
    "void main(triangle GSI i[3], inout TriangleStream<GSO> s) {\n"
    "  uint r = grules(i);\n"
    "  bool g = (r & GMASK) != 0u;\n"
    "  [unroll] for (int k = 0; k < 3; k++) { GSO o; o.pos = g ? i[k].g1 : i[k].pos; o.col = i[k].col; o.uv = i[k].uv;\n"
    "    GSCP(o, i[k]) s.Append(o); }\n"
    "#if GDIAG\n"
    "  if ((r & GDIAG) != 0u) { s.RestartStrip(); GSO z = (GSO)0; z.pos = float4(4.0, 4.0, 0.5, 1.0);\n"
    "    s.Append(z); s.Append(z); s.Append(z); }\n"
    "#endif\n"
    "}\n";
ID3D11GeometryShader *ip_gd_im, *ip_gd_sh; /* W265: IM- bzw. Shading-Ausgabe */
ID3D11Query *ip_gd_q[IP_GD_Q];
volatile int ip_gd_qbusy[IP_GD_Q]; /* 0 frei, sonst ip_gd_qi beim End (Verfall nach 4*IP_GD_Q Wiedergaben) */
volatile unsigned ip_gd_qi;
unsigned long long st_gd_tris, st_gd_hit;
unsigned st_gd_frames, st_gd_fhit, st_gd_max, st_gd_lost;

static ID3D11GeometryShader *ip_gd_make(int shade)
{
    char src[6144];
    ID3DBlob *b = NULL;
    ID3D11GeometryShader *gs = NULL;
    int ge = g_cfg.interp_guard, dm = g_cfg.interp_guard_diag & 15;
    unsigned gmask = ((ge & 1) ? 14u : 0u) | ((ge & 2) ? 1u : 0u);
    _snprintf_s(src, sizeof src, _TRUNCATE,
                "%s#define GW_EPS 1e-5\n#define GA_MIN 0.002\n#define GL_MIN 0.03\n#define GK_A %d.0\n#define GK_E %.2f\n"
                "#define GMASK %uu\n#define GDIAG %du\n#define GMAXV %d\n%s",
                shade ? "#define GS_SHADE 1\n" : "", g_cfg.interp_guard_ka > 0 ? g_cfg.interp_guard_ka : 4,
                (g_cfg.interp_guard_ke > 0 ? g_cfg.interp_guard_ke : 20) / 10.0, gmask, dm, dm ? 6 : 3, kIP_GS);
    if (!sb_im_compile(src, "gs_4_0", &b)) { HLOG_W("INTERP guard: GS (%s) nicht uebersetzbar", shade ? "Shading" : "IM"); return NULL; }
    if (FAILED(ID3D11Device_CreateGeometryShader(g_dev, b->lpVtbl->GetBufferPointer(b), b->lpVtbl->GetBufferSize(b), NULL, &gs))) { gs = NULL; }
    b->lpVtbl->Release(b);
    return gs;
}

/* W265: GS nur fuer gelerpte Dreiecks-Draws (Liste/Strip) binden; *cur = gebundener GS (Cache). */
void ip_gd_bind(ID3D11DeviceContext *c, ID3D11GeometryShader **cur, ID3D11GeometryShader *want)
{
    if (*cur != want) { ID3D11DeviceContext_GSSetShader(c, want, NULL, 0); *cur = want; }
}

void ip_gd_poll(void)
{
    unsigned k;
    for (k = 0; k < IP_GD_Q; k++) {
        D3D11_QUERY_DATA_PIPELINE_STATISTICS ps;
        if (!ip_gd_qbusy[k]) { continue; }
        if (ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)ip_gd_q[k], &ps, sizeof ps,
                                        D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
            if (ip_gd_qi - (unsigned)ip_gd_qbusy[k] > 4u * IP_GD_Q) { ip_gd_qbusy[k] = 0; st_gd_lost++; } /* nie ausgefuehrt */
            continue;
        }
        ip_gd_qbusy[k] = 0;
        {
            unsigned long long hit = ps.GSPrimitives > ps.GSInvocations ? ps.GSPrimitives - ps.GSInvocations : 0ull;
            st_gd_tris += ps.GSInvocations;
            st_gd_hit += hit;
            st_gd_frames++;
            if (hit) { st_gd_fhit++; }
            if (hit > st_gd_max) { st_gd_max = (unsigned)hit; }
        }
    }
}

int ip_init(void)
{
    ID3DBlob *vsb = NULL;
    D3D11_INPUT_ELEMENT_DESC el[6];
    D3D11_BUFFER_DESC bd;
    HRESULT hr;
    int i;
    if (ip_init_done) { return !ip_init_fail; }
    QueryPerformanceFrequency(&ip_qpf);
    if (!g_dev || !g_im_ready) { return 0; }
    ip_init_done = 1;
    ip_init_fail = 1;
    for (i = 0; i < IP_LISTS; i++) { g_ipl[i].bbref = -1; }
    {
        /* FIX(W224 LINE-FLICKER): interp_eyefix=1 -> Variante mit IP_EYE */
        char ipsrc[4096];
        const int hl = sb_ip_hlerp(); /* W429: Modus 3 = homogen ohne Bildsprung-Regel */
        _snprintf_s(ipsrc, sizeof ipsrc, _TRUNCATE, "%s%s%s%s%s",
                    (g_cfg.interp_guard || g_cfg.interp_guard_diag) ? "#define IP_GUARD 1\n" : "", /* W265 */
                    g_cfg.interp_eyefix ? "#define IP_EYE 1\n" : "",
                    hl == 3 ? "#define IP_HW 1\n" : (hl ? "#define IP_HW 1\n#define IP_JUMP 0.5\n" : ""),
                    hl >= 2 ? "#define IP_HWH 1\n" : "", kIP_VS); /* W237 */
        if (!sb_im_compile(ipsrc, "vs_4_0", &vsb)) { return 0; }
    }
    hr = ID3D11Device_CreateVertexShader(g_dev, vsb->lpVtbl->GetBufferPointer(vsb),
                                         vsb->lpVtbl->GetBufferSize(vsb), NULL, &ip_vs2);
    if (FAILED(hr)) { vsb->lpVtbl->Release(vsb); return 0; }
    memset(el, 0, sizeof el);
    el[0].SemanticName = "POSITION"; el[0].SemanticIndex = 0;
    el[0].Format = DXGI_FORMAT_R32G32B32A32_FLOAT; el[0].InputSlot = 0; el[0].AlignedByteOffset = 0;
    el[1].SemanticName = "COLOR"; el[1].SemanticIndex = 0;
    el[1].Format = DXGI_FORMAT_R32G32B32A32_FLOAT; el[1].InputSlot = 0; el[1].AlignedByteOffset = 16;
    el[2].SemanticName = "TEXCOORD"; el[2].SemanticIndex = 0;
    el[2].Format = DXGI_FORMAT_R32G32_FLOAT; el[2].InputSlot = 0; el[2].AlignedByteOffset = 32;
    el[3].SemanticName = "POSITION"; el[3].SemanticIndex = 1;
    el[3].Format = DXGI_FORMAT_R32G32B32A32_FLOAT; el[3].InputSlot = 1; el[3].AlignedByteOffset = 0;
    el[4].SemanticName = "IPW"; el[4].SemanticIndex = 0; /* FIX(W237): Clip-w Tick N (Strom 3) */
    el[4].Format = DXGI_FORMAT_R32_FLOAT; el[4].InputSlot = 3; el[4].AlignedByteOffset = 0;
    el[5].SemanticName = "IPW"; el[5].SemanticIndex = 1; /* Tick N-1 (Strom 4) */
    el[5].Format = DXGI_FORMAT_R32_FLOAT; el[5].InputSlot = 4; el[5].AlignedByteOffset = 0;
    hr = ID3D11Device_CreateInputLayout(g_dev, el, sb_ip_hlerp() ? 6 : 4, vsb->lpVtbl->GetBufferPointer(vsb),
                                        vsb->lpVtbl->GetBufferSize(vsb), &ip_lay2);
    vsb->lpVtbl->Release(vsb);
    if (FAILED(hr)) { return 0; }
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = 80;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &ip_cb0))) { return 0; }
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &ip_cb1))) { return 0; }
    if (sb_ip_hlerp()) { /* FIX(W237): Ersatz-Strom w = 1 (Stride 0) fuer Ticks ohne w-Puffer */
        static const float ones[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        D3D11_SUBRESOURCE_DATA sd;
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = 16;
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        memset(&sd, 0, sizeof sd);
        sd.pSysMem = ones;
        if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, &sd, &ip_hw_one))) { return 0; }
        bd.ByteWidth = 80;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    }
    if (g_cfg.interp_diag >= 3 || sb_rp_want()) { /* W295: Draw-ID-Shader auch fuer interp_mode=reproj */
        static const char *kIP_DBG_PS =
            "cbuffer CB : register(b0) { row_major float4x4 mvp; uint has_tex; };\n"
            "cbuffer D : register(b1) { float4 dcol; };\n"
            "Texture2D tex : register(t0);\n"
            "SamplerState samp : register(s0);\n"
            "struct PSIN { float4 pos : SV_Position; float4 col : COLOR0;\n"
            "              float2 uv : TEXCOORD0; };\n"
            "float4 main(PSIN i) : SV_Target {\n"
            "  float a = i.col.a; if ((has_tex & 1u) != 0) { a *= tex.Sample(samp, i.uv).a; }\n"
            "  return float4(dcol.rgb, a); }\n";
        ID3DBlob *psb = NULL;
        if (sb_im_compile(kIP_DBG_PS, "ps_4_0", &psb)) {
            ID3D11Device_CreatePixelShader(g_dev, psb->lpVtbl->GetBufferPointer(psb),
                                           psb->lpVtbl->GetBufferSize(psb), NULL, &ip_dbg_ps);
            psb->lpVtbl->Release(psb);
        }
        bd.ByteWidth = 16;
        if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &ip_dbg_cb))) { ip_dbg_cb = NULL; }
        if (ip_dd_on() || sb_rp_want()) { /* DIAG(W251): passt zu jedem VS (liest nur SV_Position) */
            static const char *kIP_ID_PS =
                "cbuffer D : register(b1) { float4 dcol; };\n"
                "float4 main(float4 p : SV_Position) : SV_Target { return float4(dcol.rgb, 1.0); }\n";
            if (sb_im_compile(kIP_ID_PS, "ps_4_0", &psb)) {
                ID3D11Device_CreatePixelShader(g_dev, psb->lpVtbl->GetBufferPointer(psb),
                                               psb->lpVtbl->GetBufferSize(psb), NULL, &ip_id_ps);
                psb->lpVtbl->Release(psb);
            }
        }
    }
    if (g_cfg.interp_guard || g_cfg.interp_guard_diag) { /* FIX(W265 LINE-GUARD) */
        ip_gd_im = ip_gd_make(0);
        ip_gd_sh = ip_gd_make(1);
        if (g_cfg.interp_guard_diag) {
            D3D11_QUERY_DESC qd;
            int k;
            memset(&qd, 0, sizeof qd);
            qd.Query = D3D11_QUERY_PIPELINE_STATISTICS;
            for (k = 0; k < IP_GD_Q; k++) {
                if (FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &ip_gd_q[k]))) { ip_gd_q[k] = NULL; }
            }
        }
        HLOG_I("INTERP guard: modus=%d k_A=%d k_E=%d diag=%d GS IM=%s Shading=%s", g_cfg.interp_guard,
               g_cfg.interp_guard_ka, g_cfg.interp_guard_ke, g_cfg.interp_guard_diag,
               ip_gd_im ? "ok" : "FEHLT", ip_gd_sh ? "ok" : "FEHLT");
    }
    ip_init_fail = 0;
    st_t0 = ip_now();
    HLOG_I("INTERP: bereit (interp_fps=%d, K=%d, vblank_hz=%d, vblank_qpc=%d, Takt=%s)",
           g_cfg.interp_fps, g_cfg.interp_fps / ip_tick_hz() < 1 ? 1 : g_cfg.interp_fps / ip_tick_hz(),
           g_cfg.vblank_hz, g_cfg.vblank_qpc,
           g_cfg.vblank_hz > 0 ? "Echtzeit" : "Testtakt (K Bilder am Swap)");
    return 1;
}

/* ---------- Aufzeichnung ---------- */

static void ip_snap(ip_op_t *o)
{
    UINT n;
    ID3D11DeviceContext_OMGetRenderTargets(g_ctx, 1, &o->rtv, &o->dsv);
    ID3D11DeviceContext_OMGetBlendState(g_ctx, &o->bs, o->bf, &o->smask);
    ID3D11DeviceContext_OMGetDepthStencilState(g_ctx, &o->ds, &o->sref);
    ID3D11DeviceContext_RSGetState(g_ctx, &o->rs);
    n = 1;
    ID3D11DeviceContext_RSGetViewports(g_ctx, &n, &o->vp);
    n = 1;
    ID3D11DeviceContext_RSGetScissorRects(g_ctx, &n, &o->sc);
    ID3D11DeviceContext_PSGetShaderResources(g_ctx, 0, 1, &o->srv);
}

/* PERF(IM-PERF,W99): Quell-Vertex des expandierten Vertex k (Spiegel der
 * Expansion in sb_im_submit: STRIP/FAN/POLYGON/QUADS/QUADSTRIP/LINELOOP). */
unsigned sb_im_exp_src(unsigned prim, unsigned n, unsigned k)
{
    static const unsigned char q6[6] = { 0, 1, 2, 0, 2, 3 };
    switch (prim) {
    case 3: return (k < n) ? k : 0u;
    case 6: return k / 3u + k % 3u;
    case 7: case 10: return (k % 3u) ? k / 3u + k % 3u : 0u;
    case 8: return (k / 6u) * 4u + q6[k % 6u];
    case 9: return (k / 6u) * 2u + q6[k % 6u];
    default: return k;
    }
}

static void ip_rec_draw_core(const float pref[4][4], unsigned outhash, unsigned n, unsigned topo,
                             const float mvp[16], unsigned has_tex, int cls,
                             unsigned sig, unsigned rawhash, unsigned vstart, LONGLONG q0,
                             unsigned ib1, unsigned nsv);
static float s_prefw[4]; /* FIX(W429): echte w der Bezugs-Vertices des naechsten Draws (P2 im RT) */
static int s_prefw_ok;

/* FIX(W429 M3-ROAD-FIX): wirksamer interp_hlerp-Modus. interp_hlerp gesetzt -> dieser (1/2 wie W237); sonst mit
 * interp_p2hom (nur SB) Modus 3 = homogener Lerp der P2-Draws im RT mit echter Clip-w, W224-Regel mit echter w,
 * OHNE die W237-Bildsprung-Regel - also dieselbe Regel je Vertex wie beim 3D-Pass derselben Geometrie. */
int sb_ip_hlerp(void)
{
    if (g_cfg.interp_hlerp) { return g_cfg.interp_hlerp; }
    return (g_cfg.interp_p2hom && !g_sba_loc) ? 3 : 0;
}

void sb_interp_rec_draw(const im_vertex_t *v, unsigned n, unsigned topo,
                        const float mvp[16], unsigned has_tex, int cls,
                        unsigned sig, unsigned rawhash, unsigned vstart)
{
    ip_list_t *L;
    LONGLONG q0;
    float pref[4][4];
    unsigned q;
    if (ip_live) { ip_in_rec = 1; ip_poll(); ip_in_rec = 0; } /* Interp-Takt: faelliges Bild (Schatten-Ziele); W122: in cap */
    if (ip_rec < 0 || !n) { return; }
    L = &g_ipl[ip_rec];
    if (L->overflow) { return; }
    q0 = ip_now();
    if (vstart == 0xFFFFFFFFu) { /* Aufrufer nutzt den IM-Ring (Balken): hier kopieren */
        ID3D11Buffer *vb;
        if (!sb_interp_vb_write(v, n, &vb, &vstart)) { L->overflow = 1; return; }
    }
    for (q = 0; q < 4u; q++) { memcpy(pref[q], v[(n - 1u) * q / 3u].pos, sizeof pref[q]); }
    {   /* W237: echte Clip-w (P2 im RT) fuer interp_hlerp / Diagnose */
        const float *tw = g_ip_tw;
        unsigned twn = g_ip_tw_n, twp = g_ip_tw_prim;
        g_ip_tw = NULL;
        if (sb_ip_hlerp()) { ip_w_rec(L, vstart, n, tw, twn, twp); }
        if (tw && sb_ip_hlerp()) { /* FIX(W429): echte w der Bezugs-Vertices (Abbildung wie ip_w_rec) */
            for (q = 0; q < 4u; q++) {
                unsigned k = (n - 1u) * q / 3u;
                unsigned s = (twp != 3u && twp != 6u && twp != 7u && twp != 8u && twp != 9u && twp != 10u && n <= twn) ? k : sb_im_exp_src(twp, twn, k);
                s_prefw[q] = s < twn ? tw[s] : 1.0f;
            }
            s_prefw_ok = 1;
        }
        if (g_cfg.interp_eyediag > 0) { ip_cw_rec(ip_rec, L->vbcap, vstart, n, mvp, v, NULL, 0, 0, tw, twn, twp); } /* DIAG(W224) */
    }
    ip_rec_draw_core((const float (*)[4])pref, sb_interp_hash(v, sizeof *v, n), n, topo, mvp,
                     has_tex, cls, sig, rawhash, vstart, q0, 0u, n);
}

/* Save originals before the active camera modifies them. The next VB upload
 * starts at L->nverts. Expansion matches render_im.c (including its cap).
 * No call/copy/allocation at all in the camera-0 draw path. On failure the
 * tick is invalidated, so replay never mixes old P2 with a new mesh camera. */
void sb_interp_rec_cpersp(const float camera[3], const float *pos, size_t st, unsigned prim, unsigned nsrc)
{
    ip_list_t *L;
    ip_cp_t *p;
    unsigned i, n;
    if (ip_rec < 0 || !nsrc) { return; }
    L = &g_ipl[ip_rec];
    if (L->overflow) { return; }
    memcpy(L->cp_camera, camera, sizeof L->cp_camera);
    if (!pos) { return; }
    if (nsrc > IM_VB_MAX) { nsrc = IM_VB_MAX; }
    n = nsrc;
    switch (prim) {
    case 3: if (n < IM_VB_MAX) { n++; } break;
    case 6: case 7: case 10: n = nsrc > 2u ? (nsrc - 2u) * 3u : 0u; break;
    case 8: n = (nsrc / 4u) * 6u; break;
    case 9: n = nsrc >= 4u ? ((nsrc - 4u) / 2u + 1u) * 6u : 0u; break;
    default: break;
    }
    if (n > IM_VB_MAX) { n = IM_VB_MAX; }
    if (!n) { return; }
    p = (ip_cp_t *)malloc(sizeof *p + (size_t)(n - 1u) * sizeof p->pos[0]);
    if (!p) { L->overflow = 1; return; }
    p->start = L->nverts; p->n = n;
    for (i = 0; i < n; i++) { memcpy(p->pos[i], pos + st * sb_im_exp_src(prim, nsrc, i), sizeof p->pos[i]); }
    p->next = L->cp_original;
    L->cp_original = p;
}

/* PERF(IM-PERF,W99): wie sb_interp_rec_draw, die expandierten Vertices liegen
 * aber nur im Tick-VB (direkt geschrieben): Stichproben (pref, Hash) ueber die
 * Quell-Vertices src[sb_im_exp_src(k)] - bitgleich zu den expandierten. */
void sb_interp_rec_draw_src(const im_vertex_t *src, unsigned prim, unsigned nsrc, unsigned n,
                            unsigned topo, const float mvp[16], unsigned has_tex, int cls,
                            unsigned sig, unsigned rawhash, unsigned vstart)
{
    ip_list_t *L;
    LONGLONG q0;
    float pref[4][4];
    unsigned q, h = 2166136261u, i, step;
    const unsigned ib1 = g_ip_ib1, nsv = ib1 ? nsrc : n; /* W264 DRAW-OPT2: indiziert -> nsrc Quell-Vertices im Tick-VB */
    g_ip_ib1 = 0;
    if (ip_live) { ip_in_rec = 1; ip_poll(); ip_in_rec = 0; } /* W122: in cap */
    if (ip_rec < 0 || !n) { return; }
    L = &g_ipl[ip_rec];
    if (L->overflow) { return; }
    q0 = ip_now();
    if (vstart == 0xFFFFFFFFu) { L->overflow = 1; return; } /* = Fehlschlag von vb_write */
    for (q = 0; q < 4u; q++) {
        memcpy(pref[q], src[sb_im_exp_src(prim, nsrc, (n - 1u) * q / 3u)].pos, sizeof pref[q]);
    }
    {   /* W237: echte Clip-w (P2 im RT) fuer interp_hlerp / Diagnose */
        const float *tw = g_ip_tw;
        unsigned twn = g_ip_tw_n, twp = g_ip_tw_prim;
        g_ip_tw = NULL;
        if (sb_ip_hlerp()) { ip_w_rec(L, vstart, nsv, tw, twn, ib1 ? 5u : twp); } /* W264: indiziert w je Quell-Vertex */
        if (tw && sb_ip_hlerp()) { /* FIX(W429): echte w der Bezugs-Vertices (tw je Quell-Vertex) */
            for (q = 0; q < 4u; q++) {
                unsigned s = sb_im_exp_src(prim, nsrc, (n - 1u) * q / 3u);
                s_prefw[q] = s < twn ? tw[s] : 1.0f;
            }
            s_prefw_ok = 1;
        }
        if (g_cfg.interp_eyediag > 0) { ip_cw_rec(ip_rec, L->vbcap, vstart, n, mvp, NULL, src, prim, nsrc, tw, twn, twp); } /* DIAG(W224) */
    }
    step = n > 64u ? n / 64u : 1u; /* Stichprobe exakt wie sb_interp_hash */
    for (i = 0; i < n; i += step) {
        const unsigned *w = (const unsigned *)src[sb_im_exp_src(prim, nsrc, i)].pos;
        h = (h ^ w[0]) * 16777619u;
        h = (h ^ w[1]) * 16777619u;
        h = (h ^ w[2]) * 16777619u;
        h = (h ^ w[3]) * 16777619u;
    }
    ip_rec_draw_core((const float (*)[4])pref, h ^ n, n, topo, mvp, has_tex, cls, sig, rawhash,
                     vstart, q0, ib1, nsv);
}

static void ip_rec_draw_core(const float pref[4][4], unsigned outhash, unsigned n, unsigned topo,
                             const float mvp[16], unsigned has_tex, int cls,
                             unsigned sig, unsigned rawhash, unsigned vstart, LONGLONG q0,
                             unsigned ib1, unsigned nsv)
{
    ip_list_t *L = &g_ipl[ip_rec];
    ip_op_t *o;
    const int pw = s_prefw_ok; /* W429 */
    s_prefw_ok = 0;
    o = ip_new_op(L);
    if (!o) { return; }
    o->kind = IP_DRAW;
    if (pw) { memcpy(o->prefw, s_prefw, sizeof o->prefw); o->p2w = 1; } /* W429 */
    o->cls = (unsigned char)(cls ? 1 : 0);
    o->topo = topo;
    memcpy(o->mvp, mvp, sizeof o->mvp);
    o->has_tex = has_tex;
    o->vstart = vstart;
    o->nv = n;
    o->ib1 = ib1; o->nsv = nsv; /* W264 DRAW-OPT2 */
    o->lgs = g_line_gs_rec; /* FIX(ISSUE-EMP,W352) */
    {
        unsigned q;
        for (q = 0; q < 4u; q++) {
            float c[4];
            memcpy(o->pref[q], pref[q], sizeof o->pref[q]);
            ip_clip(o->pref[q], mvp, c); /* POPIN (W91): NDC fuer ip_cost */
            o->rn[q][2] = c[3];
            o->rn[q][0] = c[3] > 1e-6f ? c[0] / c[3] : 0.0f;
            o->rn[q][1] = c[3] > 1e-6f ? c[1] / c[3] : 0.0f;
        }
    }
    o->outhash = outhash;
    o->rawhash = rawhash;
    o->sig = sig;
    o->pvs = g_ip_sigparts[0];
    o->pxp = g_ip_sigparts[1];
    o->pps = g_ip_sigparts[2];
    g_ip_sigparts[0] = g_ip_sigparts[1] = g_ip_sigparts[2] = 0;
    o->ta_smp = g_ta_smp; /* FIX(TEXADDR,W84) */
    o->nolerp = g_mfx_kind; /* MONITOR-FX (W105); W193: Art (MFX_WASH/MFX_LINES) fuer die Pause-Neuzeichnung */
    o->fxk = g_fxr_reck; /* W133 REM-FX */
    if (has_tex & FXR_MASK) { memcpy(o->fxp, g_fxr_recp, sizeof o->fxp); }
    g_fxr_reck = 0;
    if (g_rq_active) { /* RENDER-QA (W77): Shading-Zustand mitschreiben */
        int k;
        o->rq = 1;
        o->rqr = g_rq_rec;
        for (k = 1; k < 4; k++) { if (o->rqr.srv[k]) { ID3D11ShaderResourceView_AddRef(o->rqr.srv[k]); } }
    }
    if (rtt_active()) { /* W195 PHOTO-MODE: Spiel-Viewport/RT-Groesse fuer die freie Kamera der Foto-Wiedergabe */
        unsigned rw = 0, rh = 0;
        rtt_cur_dims(&rw, &rh);
        o->vpg[0] = (unsigned short)g_vp_game.x; o->vpg[1] = (unsigned short)g_vp_game.y;
        o->vpg[2] = (unsigned short)g_vp_game.w; o->vpg[3] = (unsigned short)g_vp_game.h;
        o->rtl[0] = (unsigned short)rw; o->rtl[1] = (unsigned short)rh;
    }
    o->prj[0] = g_proj_game.m[0]; o->prj[1] = g_proj_game.m[5]; /* W195 */
    o->prj[2] = g_proj_game.m[10]; o->prj[3] = g_proj_game.m[14];
    ip_snap(o);
    o->sig = (o->sig ^ (unsigned)(uintptr_t)o->rtv) * 16777619u;
    o->sig0 = (o->sig ^ topo ^ (has_tex << 8) ^ ((unsigned)o->cls << 9)) * 16777619u;
    o->sig = (o->sig ^ n) * 16777619u;
    o->gkey = ((((rawhash * 16777619u) ^ (unsigned)(uintptr_t)o->rtv) * 16777619u ^ n) * 16777619u) ^
              ((unsigned)o->cls << 31);
    o->sig = (o->sig ^ topo ^ (has_tex << 8) ^ ((unsigned)o->cls << 9)) * 16777619u;
    st_cap_q += ip_now() - q0;
}

void sb_interp_rec_clear(ID3D11RenderTargetView *rtv, const float c[4],
                         ID3D11DepthStencilView *dsv, unsigned dsflags,
                         float z, unsigned stencil)
{
    ip_list_t *L;
    ip_op_t *o;
    if (ip_live) { ip_in_rec = 1; ip_poll(); ip_in_rec = 0; } /* W122: in cap */
    if (ip_rec < 0) { return; }
    L = &g_ipl[ip_rec];
    if (L->overflow) { return; }
    o = ip_new_op(L);
    if (!o) { return; }
    if (rtv) {
        o->kind = IP_CLR_RTV;
        o->rtv = rtv;
        ID3D11RenderTargetView_AddRef(rtv);
        memcpy(o->col, c, sizeof o->col);
    } else if (dsv) {
        o->kind = IP_CLR_DSV;
        o->dsv = dsv;
        ID3D11DepthStencilView_AddRef(dsv);
        o->dsflags = dsflags;
        o->z = z;
        o->stencil = stencil;
    } else {
        L->nops--;
    }
}

/* W340 LOC-FRAMES: LoC zeichnet die Welt in den Backbuffer, kopiert den 4:3-Ausschnitt in eine Textur
 * (loc_render.c bbc_copy, "Backbuffer als Textur") und zeichnet daraus Glow-Verkleinerung und das
 * Kompositions-Quad zurueck in den Backbuffer. Die Kopie war keine Op: Zwischenbilder lerpten die Welt in den
 * BB-Schatten, das Kompositions-Quad sampelte aber die Spiel-Kopie (Stand Tick N) und deckte die gelerpte Welt
 * zu -> Welt nur 20 Bilder/s, Cockpit 120. Jetzt Op: Wiedergabe kopiert aus dem BB-Schatten in den Schatten der
 * Textur, die sampelnden Draws bekommen dessen SRV (ip_list_snap). */
void sb_interp_rec_copy(ID3D11RenderTargetView *src, ID3D11ShaderResourceView *dst, const D3D11_BOX *box)
{
    ip_list_t *L;
    ip_op_t *o;
    if (!src || !dst || ip_rec < 0) { return; }
    InterlockedIncrement(&li_cp_seen); /* W347 DIAG: Kopien waehrend der Aufzeichnung (auch mit Schalter aus) */
    /* FIX(W347 GHOST): SB-Selbstkopien (W106-Ring) ebenso, eigener Schalter */
    if (!(g_sba_loc ? g_cfg.loc_interp_copy : g_cfg.interp_selfcopy)) { return; }
    L = &g_ipl[ip_rec];
    if (L->overflow) { return; }
    o = ip_new_op(L);
    if (!o) { return; }
    o->kind = IP_COPY;
    o->cps = src;
    ID3D11RenderTargetView_AddRef(src);
    o->cpd = dst;
    ID3D11ShaderResourceView_AddRef(dst);
    if (box) { o->cpbox = *box; } else { o->cpbox.back = 0; } /* back = 0: ganze Ressource (CopyResource) */
    o->cp_es = o->cp_ed = -1;
    L->ncopy++;
    InterlockedIncrement(&li_cp_rec);
}
