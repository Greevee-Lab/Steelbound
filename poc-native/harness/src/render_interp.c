/* render_interp.c - FPS-INTERP (W69): Zwischenbilder ueber den 30-Hz-Spieltakt.
 *
 * Die Mission tickt mit 30 Hz (1 Update + 1 Swap + wait(2 VBlanks), FPS120.md).
 * Mit [render] interp_fps>0 zeichnet dieses Modul in der Mission jeden Tick
 * (Swap bis Swap) auf D3D11-Ebene auf und gibt ihn als Zwischenbilder aus:
 *  - Aufzeichnung: jeder Draw des IM-Pfads (sb_im_submit) und der Aspect-
 *    Balken mit seinen fertigen Vertexdaten (liegen im Tick-VB, den der
 *    Spiel-Draw selbst beschreibt), MVP + has_tex und dem Pipeline-Zustand
 *    per Get* (RTV/DSV, Viewport, Rasterizer/Scissor, Blend, DepthStencil +
 *    Ref, Textur t0); Clears mit Ziel-View und Werten. Shader/Layout/Sampler
 *    sind im IM-Pfad konstant. Keine Spiel-Speicher-Zugriffe bei Wiedergabe.
 *  - Schatten-Ziele: das Spiel zeichnet (bei interp_fps>0) in einen Offscreen-
 *    Backbuffer (d3d11_render.c). Jede vom Tick benutzte Render-Ressource
 *    (BB, BB-Tiefe, RTs als Ziel oder Textur) bekommt einen Schatten; am Swap
 *    wird der Spiel-Stand jeder Ressource je Liste gesichert. Wiedergaben
 *    zeichnen nur in Schatten (Anfangsstand = Ende des Vorticks), nie in
 *    Spiel-Ziele -> der Takt darf mitten im naechsten Spiel-Tick praesentieren.
 *  - Exakte Wiedergabe (t = 1) pixelgleich zum Spiel-Bild (interp_verify);
 *    das gezeigte Tick-Bild selbst ist die Sicherung des Spiel-BB.
 *  - Zwischenbild (0 < t < 1): Welt-Draws (3D-Pfad oder VSH-P2) des Ticks N
 *    werden ihrem Partner im Tick N-1 zugeordnet (Signatur aus Ziel, VS,
 *    Textur, PS, Primitiv, Vertexzahl + Hash der Eingangs-Positionen) und
 *    clip-linear interpoliert: pos = lerp(p0*M0, p1*M1, t) im Interp-VS (zwei
 *    Streams, zwei Matrizen) - Kamera, bewegte Objekte, CPU-transformierte
 *    RT-Paesse. 2D/RHW (HUD), unveraenderte und nicht zuordenbare Draws exakt
 *    aus Tick N.
 *  - Takt Echtzeit (vblank_hz>0): fester 1/interp_fps-Takt (QPC). Ein Bild zur
 *    Zeit tau zeigt den Spielzustand zur Zeit tau - L (L ~ 1 Tick + Jitter);
 *    Zustandszeit eines Ticks = Fensteranfang (Feldwechsel) + mittlere Rechen-
 *    zeit. Gezeigt wird aus den Spiel-Hooks (Draws/Clears des naechsten Ticks,
 *    Feld-Poll, VBlank-Warten) - alle auf dem Context-Thread.
 *    Testtakt (vblank_hz=0): K = interp_fps/30 Bilder (t = k/K) sofort am Swap.
 * interp_fps=0: g_ip_recording bleibt 0, sb_interp_swap kehrt sofort mit 0
 * zurueck -> exakt der bisherige Pfad. */
#include "render_int.h"
#include <d3d11_1.h>
#include "render_qa.h"
#include "sb_perfhud.h" /* W122 PERF-HUD */
#include "render_fx_rem.h" /* W133 REM-FX */
#include "vr.h" /* W134 VR-PROTO */
#include "render_cwiden.h" /* W166 COCKPIT-WIDEN */
#include <math.h>

#define IP_LISTS 4
#define IP_HIST 3
#define IP_DRAW 1
#define IP_CLR_RTV 2
#define IP_CLR_DSV 3
#define IP_VERTS_INIT (1024u * 1024u)      /* Tick-VB 40 MB (Mission ~0,7-1,2 Mio Vertices/Tick) */
#define IP_VERTS_MAX (4u * 1024u * 1024u) /* Deckel je Tick (160 MB) -> sonst Tick ohne Interp */
#define IP_OPS_MAX 65536u
#define IP_SH_MAX 64
#define IP_REFS_MAX 64
/* Zuordnungs-Ergebnis je Draw (ip_op_t.why) */
#define IP_W_NONE 0
#define IP_W_STATIC 1   /* Welt, unveraendert -> exakt */
#define IP_W_LERP 2     /* Welt, Matrix-Interpolation (gleiche Eingangsdaten) */
#define IP_W_RAWLERP 3  /* Welt, neue Eingangsdaten, Vertex i <-> i */
#define IP_W_NOMATCH 4  /* Welt, kein Partner */
#define IP_W_RAWCHG 5   /* Welt, neue Eingangsdaten ueber der Grenze -> exakt */
#define IP_W_REJECT 6   /* Welt, unplausibler Sprung -> exakt */
#define IP_W_BB2D 7     /* 2D im Backbuffer (HUD) -> exakt */
#define IP_W_RT2DLERP 8 /* 2D in RT, Vertex-Zuordnung */
#define IP_W_RT2DEX 9   /* 2D in RT, exakt (kein Partner/unveraendert/unplausibel) */
#define IP_W_N 10

typedef struct {
    unsigned char kind, cls, flag, taken; /* flag: 0 exakt, 1 lerp */
    unsigned char why;  /* Zuordnungs-Ergebnis IP_W_* (Diagnose/Statistik) */
    unsigned sig0;      /* Signatur ohne Vertexzahl (Diagnose: Partner mit anderem n) */
    unsigned gkey;      /* Geometrie-Schluessel: Ziel, Klasse, Vertexzahl, Eingangs-Hash (ohne Shader/Textur) */
    unsigned pvs, pxp, pps; /* DIAG: Signatur-Bestandteile (VS, Textur, PS) */
    unsigned topo;
    ID3D11RenderTargetView *rtv;
    ID3D11DepthStencilView *dsv;
    ID3D11BlendState *bs;
    float bf[4];
    UINT smask;
    ID3D11DepthStencilState *ds;
    UINT sref;
    ID3D11RasterizerState *rs;
    D3D11_VIEWPORT vp;
    D3D11_RECT sc;
    ID3D11ShaderResourceView *srv;
    /* Schatten-Views (nicht besitzend; Eintraege leben laenger als Listen) */
    ID3D11RenderTargetView *sh_rtv;
    ID3D11DepthStencilView *sh_dsv;
    ID3D11ShaderResourceView *sh_srv;
    float mvp[16];
    unsigned has_tex;
    unsigned vstart, nv;
    float pref[4][4]; /* 4 Bezugs-Vertices (erster, 1/3, 2/3, letzter): Plausibilitaet */
    float rn[4][3];   /* POPIN (W91): deren NDC x/y + w (einmal je Draw, fuer ip_cost) */
    unsigned sig, rawhash, outhash;
    int match;
    float col[4]; /* Clear */
    UINT dsflags;
    float z;
    unsigned stencil;
    int rq;                 /* RENDER-QA (W77): Shading-Draw */
    rq_rec_t rqr;           /* dessen Zustand (srv[1..3] mit AddRef) */
    ID3D11ShaderResourceView *rq_shsrv[4]; /* Schatten-SRVs der Stufen 1..3 */
    ID3D11SamplerState *ta_smp; /* FIX(TEXADDR,W84): s0 des alten Draws (NULL = Init-CLAMP; Cache, nicht besitzend) */
    int nolerp;             /* MONITOR-FX (W105): Korn-Wash/Stoerzeilen nie interpolieren (Zufall je Tick) */
    unsigned fxk;           /* W133 REM-FX: 1 = Effekt-Draw des Spiels, 2 = eigener Zusatz-Draw (Glut/Licht) */
    float fxp[8];           /* W133 REM-FX: Parameter (b2) bei has_tex & FXR_MASK */
} ip_op_t;

typedef struct {
    ip_op_t *ops;
    unsigned nops, capops;
    unsigned nverts;  /* belegte Vertices im Tick-VB */
    ID3D11Buffer *vb; /* Tick-VB: das Spiel schreibt seine Draws direkt hinein */
    unsigned vbcap, want;
    unsigned frame;   /* Spiel-Present-Nr am Swap */
    unsigned serial;  /* fortlaufend je fertigem Tick */
    unsigned prevser; /* Vortick, gegen den zugeordnet wurde (0 = keiner) */
    int pair_ok;      /* Zuordnung brauchbar (kein Schnitt) */
    int overflow;
    int ready;        /* fertig + gesichert, Wiedergabe moeglich */
    int snap_ok;      /* alle Ressourcen gesichert */
    int refs[IP_REFS_MAX], nrefs; /* benutzte Schatten-Eintraege */
    int bbref;        /* Eintrag des Spiel-BB */
    LONGLONG tstate;  /* Zustandszeit (QPC) */
    LONGLONG tswap;   /* Swap-Zeit (QPC) */
    /* RENDER-QA (W77): Tick-Puffer fuer Stream 2 (Bytes) und Konstanten (float4) */
    ID3D11Buffer *rqx, *rqc;
    ID3D11ShaderResourceView *rqxsrv; /* SHADE-PERF (W80): rqx als Buffer<uint> */
    unsigned rqxcap, rqxpos, rqxwant, rqccap, rqcpos, rqcwant;
    unsigned nodisc;  /* PERF(TICK-PERF,W125): Bit 1/2/4 = vb/rqx/rqc frei (GPU-Fence) -> erstes Map NO_OVERWRITE */
    ID3D11SamplerState *smp0; /* PERF(RENDER-THREAD,W126): s0 des Spiels beim Aufnahme-Start (AddRef) - Anfangszustand der Deferred-Wiedergabe */
} ip_list_t;

typedef struct {
    ID3D11Resource *res; /* Spiel-Ressource (AddRef: Adresse bleibt eindeutig) */
    ID3D11Texture2D *sh;
    ID3D11RenderTargetView *rtv;
    ID3D11DepthStencilView *dsv;
    ID3D11ShaderResourceView *srv;
    ID3D11Texture2D *bak[IP_LISTS];
    unsigned bakser[IP_LISTS];
    unsigned used;
} ip_sh_t;

int g_ip_recording; /* 1 = Draws/Clears dieses Ticks werden aufgezeichnet */
unsigned g_ip_skip_tot; /* W122 PERF-HUD: verfallene Zwischenbilder gesamt */
unsigned g_ip_sigparts[3]; /* DIAG: VS/Textur/PS des naechsten rec_draw (render_im.c) */

/* DIAG (interp_diag>=3): CPU-Kopie der Positionen je Liste (Ausreisser-Analyse) */
static float (*ip_cpos[IP_LISTS])[4];
static unsigned ip_ccap[IP_LISTS];

static ip_list_t g_ipl[IP_LISTS];
static ip_sh_t g_ish[IP_SH_MAX];
static int ip_rec = -1;
static int ip_hist[IP_HIST], ip_nhist;
static unsigned ip_serial;
static int ip_init_done, ip_init_fail;
static ID3D11VertexShader *ip_vs2;
static ID3D11InputLayout *ip_lay2;
static ID3D11Buffer *ip_cb0, *ip_cb1;
static ID3D11PixelShader *ip_dbg_ps; /* DIAG: Klassifikationsbild (interp_diag>=3) */
static ID3D11Buffer *ip_dbg_cb;
static LARGE_INTEGER ip_qpf;
static int ip_K = 1;

/* Takt (Echtzeit) */
static int ip_live;           /* 1 = Interp-Takt zeigt die Bilder */
static int ip_in_poll;        /* Rekursionsschutz */
static LONGLONG ip_tnext;     /* naechstes Present (QPC) */
static LONGLONG ip_dt;        /* 1/interp_fps (QPC) */
static LONGLONG ip_lat;       /* Latenz L (QPC) */
static LONGLONG ip_need[32];  /* Swap_N - Zustandszeit_N-1 */
static unsigned ip_nneed;
static LONGLONG ip_vb_qpc;    /* letzter Feldwechsel im Feld-Poll (QPC) */
static double ip_rbar;        /* mittlere Rechenzeit r (QPC-Einheiten) */
static LONGLONG ip_tlast_present;
static HANDLE pd_wake; /* W138 PERF-FIX: Takt-Thread wecken (Interp-Takt startet) */

static unsigned ip_shot_seq, ip_shot_left, ip_shot_next, ip_pairs_total;
static unsigned ip_shot_ser; /* Liste, deren Bilder gespeichert werden (0 = keine) */

/* Statistik */
static unsigned st_ticks, st_pairs, st_presents, st_lerp, st_static, st_nomatch,
    st_rawchg, st_reject, st_2d, st_cut, st_skip, st_hold, st_late, st_nopair, st_ovf,
    st_verify, st_verify_bad, st_exact, st_rawlerp, st_rt2d, st_geo;
/* POPIN (W91): Instanz-Zuordnung (gleiche Signatur + gleiche Eingangsdaten,
 * mehrere Kandidaten = mehrfach gezeichnetes Modell, z. B. Haeuser) */
static unsigned st_imulti, st_iwrong, st_iamb, st_ipass;
static double st_rec_ms, st_rep_ms, st_rep_max, st_pres_ms, st_r_ms, st_r_max, st_lat_ms;
static unsigned st_r_n, st_rep_n, st_lat_n;
static LONGLONG st_cap_q, st_t0;
static double st_gpu_ms, st_gpu_max;
static unsigned st_gpu_n;

static void ip_poll(void);
static void ip_rt_quiesce(void); /* W126 RENDER-THREAD */
static void ip_ev_mark(void); /* W125 TICK-PERF */
static int ip_in_rec; /* W122 PERF-HUD: ip_poll aus rec_draw/rec_clear (Zeit steckt in cap) */
static void ip_clip(const float p[4], const float m[16], float c[4]);

static double ip_ms(LONGLONG a, LONGLONG b)
{
    return (double)(b - a) * 1000.0 / (double)ip_qpf.QuadPart;
}

static LONGLONG ip_now(void)
{
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return q.QuadPart;
}

/* Hash ueber die Position (erste 4 Worte) von hoechstens 64 gleichmaessig
 * verteilten Vertices + n. Stichprobe genuegt: "gleich" heisst nur "exakt
 * aus Tick N zeichnen" bzw. "Vertex i auf Vertex i lerpen" - beides bleibt
 * bei einem unentdeckten Unterschied wohldefiniert. */
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
    if (o->rq) {
        int k;
        for (k = 1; k < 4; k++) { if (o->rqr.srv[k]) { ID3D11ShaderResourceView_Release(o->rqr.srv[k]); } }
    }
}

static void ip_list_reset(ip_list_t *L)
{
    unsigned i;
    for (i = 0; i < L->nops; i++) {
        ip_op_release(&L->ops[i]);
    }
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
    o = &L->ops[L->nops++];
    memset(o, 0, sizeof *o);
    o->match = -1;
    return o;
}

/* PERF(TICK-PERF,W125) interp_bsets=N: Reserve-Saetze der Tick-Puffer.
 * Ursache (W125_b2/b3, TICKBLK ipmap 5-18 ms/Tick): die Aufnahme nimmt die
 * gerade aus der Historie gefallene Liste; deren Tick-Puffer (40-MB-VB,
 * 64-MB-Stream-2, Konstanten) liest die GPU noch fuer eingereihte
 * Zwischenbilder (Paar A/B); das erste DISCARD-Map je Tick wartet dann bzw.
 * legt neuen Speicher an (W125_n1: auch bei freiem Satz noch 3,5 ms/Tick ->
 * freier Satz wird ab 0 per NO_OVERWRITE beschrieben, interp_bsets_nod). Neu: beim
 * Aufnahme-Start gibt die Liste ihren Satz mit einer Event-Query (= nach allen
 * bisher eingereihten GPU-Befehlen) in die Reserve und nimmt einen Satz, den
 * die GPU fertig gelesen hat (sonst den aeltesten). Inhalt und Lage der Daten
 * je Draw sind unveraendert (gleiche Offsets), nur der Speicher wechselt. */
#define IP_BS_MAX 4
typedef struct {
    ID3D11Buffer *vb, *rqx, *rqc;
    ID3D11ShaderResourceView *rqxsrv;
    unsigned vbcap, rqxcap, rqccap;
    ID3D11Query *ev;  /* Fence: GPU fertig mit allen Befehlen vor der Abgabe */
    unsigned ser;     /* Abgabe-Nr (aeltester = kleinste) */
    int used;
} ip_bset_t;
static ip_bset_t ip_bs[IP_BS_MAX];
static unsigned ip_bs_ser;
unsigned g_ip_bs_idle, g_ip_bs_busy, g_ip_bs_new; /* DIAG(TICK-PERF,W125): TICKBLK */
static int g_ip_bs_last_idle;

static void ip_bset_rotate(ip_list_t *L)
{
    int n = g_cfg.interp_bsets, i, best = -1, empty = -1;
    unsigned bser = 0xFFFFFFFFu;
    ip_bset_t t;
    if (n <= 0) { return; }
    if (n > IP_BS_MAX) { n = IP_BS_MAX; }
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
static int ip_vb_ensure(ip_list_t *L)
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

/* RENDER-QA (W77): Tick-Puffer fuer Stream 2 / Konstanten-Fenster des
 * Shading-Pfads (nur shading=1). 1 = vorhanden. */
#define IP_RQX_INIT (64u << 20)
#define IP_RQX_MAX (256u << 20)
#define IP_RQC_INIT (256u * 1024u)
#define IP_RQC_MAX (2048u * 1024u)
static int ip_rq_ensure(ip_list_t *L)
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
    }
    n = g_cfg.interp_bsets > IP_BS_MAX ? IP_BS_MAX : g_cfg.interp_bsets;
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
    CW_HLSL_CB0 /* W166 COCKPIT-WIDEN */
    "cbuffer IP : register(b1) { row_major float4x4 mvp0; float ip_t; };\n"
    "struct VSIN { float4 pos : POSITION0; float4 col : COLOR0;\n"
    "              float2 uv : TEXCOORD0; float4 pos0 : POSITION1; };\n"
    "struct VSOUT { float4 pos : SV_Position; float4 col : COLOR0;\n"
    "               float2 uv : TEXCOORD0; };\n"
    "VSOUT main(VSIN i) { VSOUT o;\n"
    "  float4 c0 = mul(i.pos0, mvp0), c1 = mul(i.pos, mvp);\n"
    "  o.pos = c0 + ip_t * (c1 - c0);\n" /* gleiche Komponenten bleiben bitgenau (W73) */
    CW_HLSL_APPLY /* W166: nach dem Lerp (Funktion der Endposition) */
    "  o.col = i.col; o.uv = i.uv; return o; }\n";

static int ip_init(void)
{
    ID3DBlob *vsb = NULL;
    D3D11_INPUT_ELEMENT_DESC el[4];
    D3D11_BUFFER_DESC bd;
    HRESULT hr;
    int i;
    if (ip_init_done) { return !ip_init_fail; }
    QueryPerformanceFrequency(&ip_qpf);
    if (!g_dev || !g_im_ready) { return 0; }
    ip_init_done = 1;
    ip_init_fail = 1;
    for (i = 0; i < IP_LISTS; i++) { g_ipl[i].bbref = -1; }
    if (!sb_im_compile(kIP_VS, "vs_4_0", &vsb)) { return 0; }
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
    hr = ID3D11Device_CreateInputLayout(g_dev, el, 4, vsb->lpVtbl->GetBufferPointer(vsb),
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
    if (g_cfg.interp_diag >= 3) {
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
    }
    ip_init_fail = 0;
    st_t0 = ip_now();
    HLOG_I("INTERP: bereit (interp_fps=%d, K=%d, vblank_hz=%d, vblank_qpc=%d, Takt=%s)",
           g_cfg.interp_fps, g_cfg.interp_fps / 30 < 1 ? 1 : g_cfg.interp_fps / 30,
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
                             unsigned sig, unsigned rawhash, unsigned vstart, LONGLONG q0);

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
    ip_rec_draw_core((const float (*)[4])pref, sb_interp_hash(v, sizeof *v, n), n, topo, mvp,
                     has_tex, cls, sig, rawhash, vstart, q0);
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
    if (ip_live) { ip_in_rec = 1; ip_poll(); ip_in_rec = 0; } /* W122: in cap */
    if (ip_rec < 0 || !n) { return; }
    L = &g_ipl[ip_rec];
    if (L->overflow) { return; }
    q0 = ip_now();
    if (vstart == 0xFFFFFFFFu) { L->overflow = 1; return; } /* = Fehlschlag von vb_write */
    for (q = 0; q < 4u; q++) {
        memcpy(pref[q], src[sb_im_exp_src(prim, nsrc, (n - 1u) * q / 3u)].pos, sizeof pref[q]);
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
                     vstart, q0);
}

static void ip_rec_draw_core(const float pref[4][4], unsigned outhash, unsigned n, unsigned topo,
                             const float mvp[16], unsigned has_tex, int cls,
                             unsigned sig, unsigned rawhash, unsigned vstart, LONGLONG q0)
{
    ip_list_t *L = &g_ipl[ip_rec];
    ip_op_t *o;
    o = ip_new_op(L);
    if (!o) { return; }
    o->kind = IP_DRAW;
    o->cls = (unsigned char)(cls ? 1 : 0);
    o->topo = topo;
    memcpy(o->mvp, mvp, sizeof o->mvp);
    o->has_tex = has_tex;
    o->vstart = vstart;
    o->nv = n;
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
    o->nolerp = (g_mfx_kind != MFX_NONE); /* MONITOR-FX (W105) */
    o->fxk = g_fxr_reck; /* W133 REM-FX */
    if (has_tex & FXR_MASK) { memcpy(o->fxp, g_fxr_recp, sizeof o->fxp); }
    g_fxr_reck = 0;
    if (g_rq_active) { /* RENDER-QA (W77): Shading-Zustand mitschreiben */
        int k;
        o->rq = 1;
        o->rqr = g_rq_rec;
        for (k = 1; k < 4; k++) { if (o->rqr.srv[k]) { ID3D11ShaderResourceView_AddRef(o->rqr.srv[k]); } }
    }
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

/* ---------- Zuordnung Tick N-1 -> N ---------- */

static void ip_clip(const float p[4], const float m[16], float c[4])
{
    int j;
    for (j = 0; j < 4; j++) {
        c[j] = p[0] * m[0 * 4 + j] + p[1] * m[1 * 4 + j] + p[2] * m[2 * 4 + j] + p[3] * m[3 * 4 + j];
    }
}

/* Plausibilitaet einer Zuordnung a (Tick N-1) -> o (Tick N): jeder der 4
 * Bezugs-Vertices, der in beiden Ticks vor der Kamera liegt (w > 0), darf
 * hoechstens interp_maxjump (NDC) springen; ein Vorzeichenwechsel von w ist
 * unplausibel; mind. ein Bezug muss vor der Kamera liegen, ausser alle
 * liegen in beiden Ticks dahinter (unsichtbar). 1 = lerpen. */
static int ip_plausible(const ip_op_t *a, const ip_op_t *o, float maxjump)
{
    int q, front = 0, behind = 0;
    for (q = 0; q < 4; q++) {
        float c0[4], c1[4];
        ip_clip(a->pref[q], a->mvp, c0);
        ip_clip(o->pref[q], o->mvp, c1);
        if (c0[3] > 1e-6f && c1[3] > 1e-6f) {
            float dx = c1[0] / c1[3] - c0[0] / c0[3], dy = c1[1] / c1[3] - c0[1] / c0[3];
            if (dx < 0) { dx = -dx; }
            if (dy < 0) { dy = -dy; }
            if (dx > maxjump || dy > maxjump) { return 0; }
            front++;
        } else if (c0[3] < -1e-6f && c1[3] < -1e-6f) {
            behind++;
        } else {
            return 0; /* w-Wechsel oder w ~ 0 */
        }
    }
    return front > 0 || behind == 4;
}

/* Partnersuche ueber den ganzen Vortick (W73): Draws von A je Signatur-Eimer
 * in aufsteigender Reihenfolge verkettet. Frueher Fenster von 256 Draws ab dem
 * letzten Treffer - ein im Vortick fehlender/geaenderter Block > 256 Draws
 * (z. B. ein Monitor-Pass, der nur jeden 2. Tick neu gebaut wird) liess alle
 * folgenden Draws des Ticks ohne Partner (W73_d1: ~600 nomatch je 2. Tick). */
#define IP_HBITS 12
#define IP_HB(k) (((k) ^ ((k) >> IP_HBITS) ^ ((k) >> 24)) & ((1u << IP_HBITS) - 1u))
static int ip_hhead[1u << IP_HBITS], ip_ghead[1u << IP_HBITS];
static int *ip_hnext, *ip_gnext;
static unsigned ip_hcap;

static int ip_hash_build(ip_list_t *A)
{
    unsigned j;
    if (ip_hcap < A->nops) {
        int *nn = (int *)realloc(ip_hnext, A->nops * sizeof *nn);
        if (!nn) { return 0; }
        ip_hnext = nn;
        nn = (int *)realloc(ip_gnext, A->nops * sizeof *nn);
        if (!nn) { return 0; }
        ip_gnext = nn;
        ip_hcap = A->nops;
    }
    memset(ip_hhead, 0xFF, sizeof ip_hhead);
    memset(ip_ghead, 0xFF, sizeof ip_ghead);
    for (j = A->nops; j-- > 0;) {
        const ip_op_t *a = &A->ops[j];
        unsigned h;
        if (a->kind != IP_DRAW) { continue; }
        h = IP_HB(a->sig);
        ip_hnext[j] = ip_hhead[h];
        ip_hhead[h] = (int)j;
        h = IP_HB(a->gkey);
        ip_gnext[j] = ip_ghead[h];
        ip_ghead[h] = (int)j;
    }
    return 1;
}

/* Freier Partner gleicher Signatur, raw: 1 = gleicher Eingangs-Hash, 0 =
 * anderer, -1 = egal. Bevorzugt der naechste ab jp (Reihenfolge), sonst der
 * naechstliegende davor. -1 = keiner. */
static int ip_pick(ip_list_t *A, const ip_op_t *o, unsigned jp, int raw)
{
    unsigned h = IP_HB(o->sig);
    int j, before = -1;
    for (j = ip_hhead[h]; j >= 0; j = ip_hnext[j]) {
        const ip_op_t *a = &A->ops[j];
        if (a->taken || a->sig != o->sig) { continue; }
        if (raw == 1 && a->rawhash != o->rawhash) { continue; }
        if (raw == 0 && a->rawhash == o->rawhash) { continue; }
        if ((unsigned)j >= jp) {
            if (before < 0 || (unsigned)j - jp <= jp - (unsigned)before) { return j; }
            return before;
        }
        before = j;
    }
    return before;
}

/* Geometrie-Partner (W73): gleiche Eingangs-Positionen, gleiches Ziel und
 * gleiche Vertexzahl, aber anderer Shader/andere Textur (W73_d3: Sub-Monitor-
 * Paesse wechseln zwischen Ticks Material/Shader, ~760 Draws ohne Partner).
 * Freie Partner bevorzugt, sonst darf ein schon vergebener Draw des Vorticks
 * ein zweites Mal als Positionsquelle dienen (Farben/UV kommen aus Tick N). */
#define IP_COST_INF 1e9f
static float ip_cost(const ip_op_t *a, const ip_op_t *o); /* POPIN (W91) */
static int ip_pick_geo(ip_list_t *A, const ip_op_t *o, unsigned jp)
{
    int j, best = -1, besttk = -1;
    unsigned bd = 0xFFFFFFFFu, bdt = 0xFFFFFFFFu;
    for (j = ip_ghead[IP_HB(o->gkey)]; j >= 0; j = ip_gnext[j]) {
        const ip_op_t *a = &A->ops[j];
        unsigned d;
        if (a->gkey != o->gkey || a->rtv != o->rtv || a->nv != o->nv || a->rawhash != o->rawhash) {
            continue;
        }
        d = (unsigned)j >= jp ? (unsigned)j - jp : jp - (unsigned)j;
        if (g_cfg.interp_inst) { /* POPIN (W91): naechste Instanz statt Reihenfolge */
            float c = ip_cost(a, o);
            d = c >= IP_COST_INF ? 0xFFFFFFFEu : (unsigned)(c * 1.0e6f);
        }
        if (!a->taken) { if (d < bd) { bd = d; best = j; } }
        else if (d < bdt) { bdt = d; besttk = j; }
    }
    return best >= 0 ? best : besttk;
}

/* Praktisch unbewegt (W73): alle 4 Bezugs-Vertices springen < 1e-4 NDC
 * (~0,13 px bei 2560). Solche Draws (Cockpit-Teile mit bitweise leicht
 * anderer Matrix) exakt aus Tick N zeichnen - sonst weichen Zwischenbilder
 * durch Rundung um 1 Farbstufe vom stehenden Bild ab (W73_d7 A4). */
static int ip_still(const ip_op_t *a, const ip_op_t *o)
{
    int q;
    for (q = 0; q < 4; q++) {
        float c0[4], c1[4];
        ip_clip(a->pref[q], a->mvp, c0);
        ip_clip(o->pref[q], o->mvp, c1);
        if (c0[3] <= 1e-6f || c1[3] <= 1e-6f) { return 0; }
        if (fabsf(c1[0] / c1[3] - c0[0] / c0[3]) > 1e-4f || fabsf(c1[1] / c1[3] - c0[1] / c0[3]) > 1e-4f ||
            fabsf(c1[2] / c1[3] - c0[2] / c0[3]) > 1e-6f) {
            return 0;
        }
    }
    return 1;
}

/* ---------- POPIN (W91): Instanz-Zuordnung ----------
 * Befund W91: ein Modell, das mehrfach im Tick gezeichnet wird (Haeuser,
 * Cockpit-Teile: gleiche VS/Textur/PS/Vertexzahl UND gleiche Objektraum-
 * Vertices, nur andere Matrix), hat mehrere gleichwertige Partner. ip_pick
 * waehlt den naechsten freien in Draw-Reihenfolge - kommt eine Instanz ins
 * Bild (Culling) oder faellt eine heraus, verschiebt sich die Reihenfolge und
 * der Draw wird mit einer ANDEREN Instanz des Vorticks interpoliert: das Haus
 * fliegt in den Zwischenbildern von der Stelle des Nachbarn heran (Aufpoppen
 * fuer 1-3 Bilder je Tick), die richtige Instanz bleibt ohne Partner.
 * interp_inst=1: Partner = kleinster Bildsprung der 4 Bezugs-Vertices, nur
 * wenn auch umgekehrt kein anderer offener Draw des Ticks naeher an diesem
 * Partner liegt (gegenseitig naechster) und die Wahl eindeutig ist; sonst
 * exakt aus Tick N (30 Hz, aber nie falsch). */
static int ip_bhead[1u << IP_HBITS];
static int *ip_bnext;
static unsigned ip_bcap;

/* Bildsprung a -> o: groesster NDC-Sprung der 4 Bezugs-Vertices (wie
 * ip_plausible, aus den beim Aufzeichnen berechneten NDC). */
static float ip_cost(const ip_op_t *a, const ip_op_t *o)
{
    int q, front = 0;
    float m = 0.0f;
    for (q = 0; q < 4; q++) {
        float w0 = a->rn[q][2], w1 = o->rn[q][2];
        if (w0 > 1e-6f && w1 > 1e-6f) {
            float dx = fabsf(o->rn[q][0] - a->rn[q][0]), dy = fabsf(o->rn[q][1] - a->rn[q][1]);
            if (dx > m) { m = dx; }
            if (dy > m) { m = dy; }
            front++;
        } else if (!(w0 < -1e-6f && w1 < -1e-6f)) {
            return IP_COST_INF; /* w-Wechsel */
        }
    }
    return front ? m : 0.0f;
}

static int ip_bhash_build(ip_list_t *B)
{
    unsigned j;
    if (ip_bcap < B->nops) {
        int *nn = (int *)realloc(ip_bnext, B->nops * sizeof *nn);
        if (!nn) { return 0; }
        ip_bnext = nn;
        ip_bcap = B->nops;
    }
    memset(ip_bhead, 0xFF, sizeof ip_bhead);
    for (j = B->nops; j-- > 0;) {
        unsigned h;
        if (B->ops[j].kind != IP_DRAW) { continue; }
        h = IP_HB(B->ops[j].sig);
        ip_bnext[j] = ip_bhead[h];
        ip_bhead[h] = (int)j;
    }
    return 1;
}

/* Kleinster-Sprung-Kandidat in A (frei, gleiche Signatur + Eingangs-Hash).
 * *n = Zahl der Kandidaten, *c1/*c2 = bester/zweitbester Sprung. */
static int ip_inst_best(ip_list_t *A, const ip_op_t *o, int *n, float *c1, float *c2, int *second)
{
    int j, best = -1;
    *n = 0;
    *c1 = *c2 = IP_COST_INF;
    *second = -1;
    for (j = ip_hhead[IP_HB(o->sig)]; j >= 0; j = ip_hnext[j]) {
        const ip_op_t *a = &A->ops[j];
        float c;
        if (a->taken || a->sig != o->sig || a->rawhash != o->rawhash) { continue; }
        (*n)++;
        c = ip_cost(a, o);
        if (c < *c1) { *c2 = *c1; *second = best; *c1 = c; best = j; }
        else if (c < *c2) { *c2 = c; *second = j; }
    }
    return best;
}

/* Gibt es in B einen anderen offenen Draw gleicher Art, der naeher an a liegt? */
static int ip_inst_closer(ip_list_t *B, unsigned oi, const ip_op_t *a, float c)
{
    int k;
    const ip_op_t *o = &B->ops[oi];
    for (k = ip_bhead[IP_HB(o->sig)]; k >= 0; k = ip_bnext[k]) {
        const ip_op_t *o2 = &B->ops[k];
        if ((unsigned)k == oi || o2->sig != o->sig || o2->rawhash != o->rawhash || o2->match >= 0) { continue; }
        if (o2->kind != IP_DRAW || o2->why != IP_W_NONE) { continue; } /* schon entschieden */
        if (ip_cost(a, o2) < c) { return 1; }
    }
    return 0;
}

/* interp_inst=1: Partner fuer o (Index oi). -1 = keiner (neu im Bild bzw. nicht
 * eindeutig -> exakt). *amb = 1, wenn es Kandidaten gab, aber keiner eindeutig. */
static int ip_pick_inst(ip_list_t *A, ip_list_t *B, unsigned oi, int *amb)
{
    const ip_op_t *o = &B->ops[oi];
    int n, best, second;
    float c1, c2;
    *amb = 0;
    best = ip_inst_best(A, o, &n, &c1, &c2, &second);
    if (best < 0) { return -1; }
    if (n > 1) { st_imulti++; }
    if (c1 >= IP_COST_INF) { *amb = 1; return -1; }
    /* eindeutig: Zweitbester deutlich weiter weg (oder beide praktisch gleich nah) */
    if (second >= 0 && c2 < 2.0f * c1 && c2 > 0.02f) { *amb = 1; return -1; }
    if (ip_inst_closer(B, oi, &A->ops[best], c1)) { *amb = 1; return -1; }
    return best;
}

/* DIAG(POPIN,W91): Pixel-Huellrechteck der 4 Bezugs-Vertices im Ziel-Viewport
 * (grob, nur Lagevergleich). 0 = nicht vor der Kamera. */
static int ip_refbox(const ip_op_t *o, int bb[4])
{
    int q, n = 0;
    bb[0] = bb[1] = 1 << 30;
    bb[2] = bb[3] = -(1 << 30);
    for (q = 0; q < 4; q++) {
        float c[4];
        int x, y;
        ip_clip(o->pref[q], o->mvp, c);
        if (c[3] <= 1e-6f) { continue; }
        x = (int)(o->vp.TopLeftX + (c[0] / c[3] + 1.0f) * 0.5f * o->vp.Width);
        y = (int)(o->vp.TopLeftY + (1.0f - c[1] / c[3]) * 0.5f * o->vp.Height);
        if (x < bb[0]) { bb[0] = x; }
        if (y < bb[1]) { bb[1] = y; }
        if (x > bb[2]) { bb[2] = x; }
        if (y > bb[3]) { bb[3] = y; }
        n++;
    }
    return n;
}

static void ip_iwrong_log(const ip_list_t *A, const ip_list_t *B, unsigned i, int found, int best)
{
    static unsigned nlog;
    unsigned md = 0;
    int b0[4], b1[4], b2[4];
    const ip_op_t *o = &B->ops[i];
    __try { md = *(volatile unsigned *)(uintptr_t)0x2EEA34u; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (md != 4u || nlog >= 4000u) { return; } /* nur Mission (Modus 4), gedeckelt */
    if (g_cfg.pf_ring > 0 && !sb_pf_active()) { return; } /* mit pf_ring nur waehrend der Bildfolgen */
    nlog++;
    ip_refbox(o, b0);
    ip_refbox(&A->ops[found], b1);
    ip_refbox(&A->ops[best], b2);
    HLOG_I("INTERP iwrong f=%u i=%u n=%u %s sig=%08X raw=%08X partner=%d sprung=%.3f naechste=%d sprung=%.3f "
           "box=%d,%d,%d,%d partnerbox=%d,%d,%d,%d naechstebox=%d,%d,%d,%d",
           B->frame, i, o->nv, o->rtv == g_rtv ? "BB" : "RT", o->sig, o->rawhash, found,
           (double)ip_cost(&A->ops[found], o), best, (double)ip_cost(&A->ops[best], o),
           b0[0], b0[1], b0[2], b0[3], b1[0], b1[1], b1[2], b1[3], b2[0], b2[1], b2[2], b2[3]);
}

/* POPIN (W91): 16-Bit-Schluessel Instanz-Durchgang = gkey + Matrix (FNV). */
static unsigned ip_mvpkey(const ip_op_t *o)
{
    const unsigned char *b = (const unsigned char *)o->mvp;
    unsigned h = 2166136261u ^ o->gkey, k;
    for (k = 0; k < sizeof o->mvp; k++) { h = (h ^ b[k]) * 16777619u; }
    return (h ^ (h >> 16)) & 0xFFFFu;
}

static int ip_match(ip_list_t *A, ip_list_t *B, int log)
{
    unsigned i, jp = 0, ndraw = 0, nclr = 0, nworld = 0, nmatch = 0, nlerp = 0,
             nstatic = 0, nnomatch = 0, nrawchg = 0, nreject = 0, n2d = 0, inorder = 0,
             nrawlerp = 0, nrt2d = 0, ngeo = 0;
    unsigned bbdraws = 0;
    for (i = 0; i < A->nops; i++) { A->ops[i].taken = 0; }
    if (!ip_hash_build(A)) { return 0; }
    if (g_cfg.interp_inst) { /* POPIN (W91): ip_inst_closer prueft noch offene Draws von B */
        for (i = 0; i < B->nops; i++) { B->ops[i].match = -1; B->ops[i].why = IP_W_NONE; }
        if (!ip_bhash_build(B)) { return 0; }
    }
    for (i = 0; i < B->nops; i++) {
        ip_op_t *o = &B->ops[i];
        int found = -1, sigonly = -1, iwbest = -1;
        o->match = -1;
        o->flag = 0;
        o->why = IP_W_NONE;
        if (o->kind != IP_DRAW) { nclr++; continue; }
        ndraw++;
        if (o->rtv == g_rtv) { bbdraws++; }
        if (o->nolerp) { n2d++; o->why = IP_W_RT2DEX; continue; } /* MONITOR-FX (W105): Korn/Stoerung wechselt im Spieltakt, exakt aus Tick N */
        if (!o->cls && o->rtv == g_rtv) { n2d++; o->why = IP_W_BB2D; continue; } /* HUD/2D im BB: immer exakt */
        if (!o->cls) {
            /* 2D/RHW-Draw in ein RT (vom Spiel vorprojizierte Welt- oder
             * Monitor-Inhalte): nur Vertex-Zuordnung mit strenger Grenze;
             * unveraendert -> exakt. */
            int s2;
            n2d++;
            o->why = IP_W_RT2DEX;
            s2 = ip_pick(A, o, jp, 1);
            if (s2 < 0) { s2 = ip_pick(A, o, jp, -1); }
            if (s2 >= 0) {
                ip_op_t *a = &A->ops[s2];
                a->taken = 1;
                jp = (unsigned)s2 + 1u;
                o->match = s2;
                if (!(a->outhash == o->outhash && memcmp(a->mvp, o->mvp, sizeof o->mvp) == 0) &&
                    ip_plausible(a, o, g_cfg.interp_maxjump * 0.3f)) {
                    o->flag = 1;
                    o->why = IP_W_RT2DLERP;
                    nrt2d++;
                }
            }
            continue;
        }
        nworld++;
        if (g_cfg.interp_inst) {
            int amb = 0;
            found = ip_pick_inst(A, B, i, &amb);
            if (amb) { /* mehrere gleichwertige Instanzen, keine eindeutig -> exakt */
                st_iamb++;
                nnomatch++;
                o->why = IP_W_NOMATCH;
                continue;
            }
        } else {
            found = ip_pick(A, o, jp, 1);
            if (found >= 0 && g_cfg.interp_diag >= 1) { /* DIAG(POPIN,W91): Reihenfolge-Wahl vs. naechste Instanz */
                int n, second, best;
                float c1, c2, cf = ip_cost(&A->ops[found], o);
                best = ip_inst_best(A, o, &n, &c1, &c2, &second);
                if (n > 1) { st_imulti++; }
                if (best >= 0 && best != found && cf > 2.0f * c1 + 0.02f) { st_iwrong++; iwbest = best; }
            }
        }
        if (found < 0) { sigonly = ip_pick(A, o, jp, 0); }
        if (found < 0 && sigonly >= 0) {
            /* Gleiche Signatur, neue Eingangs-Positionen (z. B. kamera-relativ
             * neu gebackenes Gelaende, CPU-animiert): Vertex i <-> Vertex i,
             * nur bei kleinem Sprung aller 4 Bezugs-Vertices (strenger als
             * bei Matrix-Draws, schuetzt vor vertauschten Partikeln). */
            ip_op_t *a = &A->ops[sigonly];
            if (ip_plausible(a, o, g_cfg.interp_maxjump * ((o->fxk && g_cfg.fx_interp) ? 1.0f : 0.3f))) { /* W133 fx_interp */
                a->taken = 1;
                jp = (unsigned)sigonly + 1u;
                o->match = sigonly;
                o->flag = 1;
                o->why = IP_W_RAWLERP;
                nmatch++;
                nlerp++;
                nrawlerp++;
                continue;
            }
        }
        if (found < 0) {
            int g = ip_pick_geo(A, o, jp);
            if (g >= 0 && ip_plausible(&A->ops[g], o, g_cfg.interp_maxjump * ((o->fxk && g_cfg.fx_interp) ? 1.0f : 0.3f))) { /* W133 fx_interp */
                A->ops[g].taken = 1;
                o->match = g;
                nmatch++;
                if (memcmp(A->ops[g].mvp, o->mvp, sizeof o->mvp) == 0) {
                    nstatic++;
                    o->why = IP_W_STATIC;
                } else {
                    o->flag = 1;
                    o->why = IP_W_LERP;
                    nlerp++;
                    ngeo++;
                }
                continue;
            }
            if (sigonly >= 0) { nrawchg++; o->why = IP_W_RAWCHG; o->match = sigonly; }
            else { nnomatch++; o->why = IP_W_NOMATCH; }
            continue;
        }
        {
            ip_op_t *a = &A->ops[found];
            if ((unsigned)found == jp) { inorder++; }
            a->taken = 1;
            jp = (unsigned)found + 1u;
            o->match = found;
            nmatch++;
            if (a->outhash == o->outhash && memcmp(a->mvp, o->mvp, sizeof o->mvp) == 0) {
                nstatic++;
                o->why = IP_W_STATIC;
                continue;
            }
            if (ip_still(a, o)) { nstatic++; o->why = IP_W_STATIC; }
            else if (ip_plausible(a, o, g_cfg.interp_maxjump)) { o->flag = 1; nlerp++; o->why = IP_W_LERP; }
            else { nreject++; o->why = IP_W_REJECT; }
            if (iwbest >= 0 && o->why == IP_W_LERP) { ip_iwrong_log(A, B, i, found, iwbest); } /* DIAG(POPIN,W91) */
        }
    }
    if (g_cfg.interp_pass) {
        /* POPIN (W91): Mehrpass-Konsistenz. Durchgaenge derselben Instanz
         * (gleiches Ziel, gleiche Geometrie = gkey, gleiche Matrix) muessen
         * gleich behandelt werden: ist einer exakt (ohne Partner, neue
         * Eingangsdaten, unplausibel), wird ein gelerpter zweiter Durchgang
         * gegen ihn verschoben gezeichnet -> Z-Fighting-Streifen/Aufblitzen
         * in Zwischenbildern. Dann alle Durchgaenge der Instanz exakt. */
        static unsigned char ex[1u << 13]; /* 65536 Bit */
        unsigned npass = 0;
        memset(ex, 0, sizeof ex);
        for (i = 0; i < B->nops; i++) {
            const ip_op_t *o = &B->ops[i];
            unsigned k;
            if (o->kind != IP_DRAW || !o->cls) { continue; }
            if (o->why != IP_W_NOMATCH && o->why != IP_W_RAWCHG && o->why != IP_W_REJECT) { continue; }
            k = ip_mvpkey(o);
            ex[k >> 3] |= (unsigned char)(1u << (k & 7u));
        }
        for (i = 0; i < B->nops; i++) {
            ip_op_t *o = &B->ops[i];
            unsigned k;
            if (o->kind != IP_DRAW || !o->cls || !o->flag) { continue; }
            k = ip_mvpkey(o);
            if (ex[k >> 3] & (1u << (k & 7u))) { o->flag = 0; npass++; }
        }
        st_ipass += npass;
        if (npass <= nlerp) { nlerp -= npass; }
    }
    st_lerp += nlerp; st_static += nstatic; st_nomatch += nnomatch;
    st_rawchg += nrawchg; st_reject += nreject; st_2d += n2d; st_rawlerp += nrawlerp;
    st_rt2d += nrt2d;
    st_geo += ngeo;
    if (log) {
        HLOG_I("INTERP pair f=%u ops=%u draws=%u clears=%u bb=%u 2d=%u world=%u matched=%u inorder=%u lerp=%u (rawlerp=%u rt2d=%u) static=%u nomatch=%u rawchg=%u reject=%u verts=%u/%u",
               B->frame, B->nops, ndraw, nclr, bbdraws, n2d, nworld, nmatch, inorder,
               nlerp, nrawlerp, nrt2d, nstatic, nnomatch, nrawchg, nreject, A->nverts, B->nverts);
    }
    /* Schnitt (Kamerasprung): viele unplausible Spruenge -> Paar exakt. */
    if (nreject > 4u && nreject * 4u > nlerp + nreject) {
        st_cut++;
        for (i = 0; i < B->nops; i++) { B->ops[i].flag = 0; }
        if (log || st_cut <= 8u) {
            HLOG_I("INTERP cut f=%u reject=%u lerp=%u (Paar exakt)", B->frame, nreject, nlerp);
        }
        return 0;
    }
    return 1;
}

/* Diagnose: Draw-Liste eines Ticks (interp_diag=2, erste volle Ticks). */
static void ip_dump(ip_list_t *B)
{
    unsigned i;
    for (i = 0; i < B->nops; i++) {
        const ip_op_t *o = &B->ops[i];
        if (o->kind != IP_DRAW) {
            HLOG_I("INTERP op f=%u i=%u %s rtv=%p dsv=%p", B->frame, i,
                   o->kind == IP_CLR_RTV ? "clr-rt" : "clr-ds", (void *)o->rtv, (void *)o->dsv);
            continue;
        }
        HLOG_I("INTERP op f=%u i=%u draw rtv=%p%s cls=%u n=%u topo=%u tex=%u sig=%08X raw=%08X out=%08X m=%d fl=%u mvp0=%.4f,%.4f,%.4f,%.4f",
               B->frame, i, (void *)o->rtv, o->rtv == g_rtv ? "(BB)" : "", o->cls, o->nv, o->topo,
               o->has_tex, o->sig, o->rawhash, o->outhash, o->match, o->flag,
               o->mvp[0], o->mvp[5], o->mvp[12], o->mvp[13]);
    }
}

/* ---------- Schatten-Ziele + Sicherung je Liste ---------- */

static void ip_sh_free(ip_sh_t *e)
{
    int i;
    if (e->res) { ID3D11Resource_Release(e->res); }
    if (e->rtv) { ID3D11RenderTargetView_Release(e->rtv); }
    if (e->dsv) { ID3D11DepthStencilView_Release(e->dsv); }
    if (e->srv) { ID3D11ShaderResourceView_Release(e->srv); }
    if (e->sh) { ID3D11Texture2D_Release(e->sh); }
    for (i = 0; i < IP_LISTS; i++) {
        if (e->bak[i]) { ID3D11Texture2D_Release(e->bak[i]); }
    }
    memset(e, 0, sizeof *e);
}

static int ip_sh_find(ID3D11Resource *res)
{
    int i, free_i = -1;
    D3D11_TEXTURE2D_DESC dd;
    ip_sh_t *e;
    HRESULT hr = S_OK;
    for (i = 0; i < IP_SH_MAX; i++) {
        if (g_ish[i].res == res) { return i; }
        if (!g_ish[i].res && free_i < 0) { free_i = i; }
    }
    if (free_i < 0) {
        /* Eintrag ohne gueltige Sicherung einer lebenden Liste verdraengen */
        unsigned best = 0xFFFFFFFFu;
        for (i = 0; i < IP_SH_MAX; i++) {
            int live = 0, k;
            for (k = 0; k < IP_LISTS; k++) {
                if (g_ipl[k].serial && g_ish[i].bakser[k] == g_ipl[k].serial) { live = 1; }
            }
            if (!live && g_ish[i].used < best) { best = g_ish[i].used; free_i = i; }
        }
        if (free_i < 0) { return -1; }
        ip_rt_quiesce(); /* W126 RENDER-THREAD */
        ip_sh_free(&g_ish[free_i]);
    }
    e = &g_ish[free_i];
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)res, &dd);
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &dd, NULL, &e->sh)) || !e->sh) {
        memset(e, 0, sizeof *e);
        return -1;
    }
    if (dd.BindFlags & D3D11_BIND_RENDER_TARGET) {
        hr = ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)e->sh, NULL, &e->rtv);
    }
    {   /* FIX(SHADOWBUF,W127): typlose Tiefentextur (Schattenpuffer, W100:
         * R24G8_TYPELESS mit DSV + SRV) braucht explizite View-Formate - mit
         * NULL-Beschreibung schlug die Schattenkopie fehl, jede Tick-Liste mit
         * Tiefenpass war snap_ok=0 und wurde verworfen (W100_oni2: pairs=0).
         * Typisierte Ressourcen wie bisher (NULL). */
        DXGI_FORMAT vs = DXGI_FORMAT_UNKNOWN, vd = DXGI_FORMAT_UNKNOWN;
        if (dd.Format == DXGI_FORMAT_R24G8_TYPELESS) { vs = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; vd = DXGI_FORMAT_D24_UNORM_S8_UINT; }
        else if (dd.Format == DXGI_FORMAT_R32_TYPELESS) { vs = DXGI_FORMAT_R32_FLOAT; vd = DXGI_FORMAT_D32_FLOAT; }
        else if (dd.Format == DXGI_FORMAT_R16_TYPELESS) { vs = DXGI_FORMAT_R16_UNORM; vd = DXGI_FORMAT_D16_UNORM; }
        if (SUCCEEDED(hr) && (dd.BindFlags & D3D11_BIND_SHADER_RESOURCE)) {
            D3D11_SHADER_RESOURCE_VIEW_DESC sv;
            memset(&sv, 0, sizeof sv);
            sv.Format = vs;
            sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sv.Texture2D.MipLevels = dd.MipLevels ? dd.MipLevels : 1u;
            hr = ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)e->sh,
                                                       vs != DXGI_FORMAT_UNKNOWN ? &sv : NULL, &e->srv);
        }
        if (SUCCEEDED(hr) && (dd.BindFlags & D3D11_BIND_DEPTH_STENCIL)) {
            D3D11_DEPTH_STENCIL_VIEW_DESC dv;
            memset(&dv, 0, sizeof dv);
            dv.Format = vd;
            dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            hr = ID3D11Device_CreateDepthStencilView(g_dev, (ID3D11Resource *)e->sh,
                                                     vd != DXGI_FORMAT_UNKNOWN ? &dv : NULL, &e->dsv);
        }
    }
    for (i = 0; SUCCEEDED(hr) && i < IP_LISTS; i++) {
        hr = ID3D11Device_CreateTexture2D(g_dev, &dd, NULL, &e->bak[i]);
    }
    if (FAILED(hr)) {
        static unsigned nfail; /* W127: Fehlschlag sichtbar (vorher stumm -> Liste verworfen) */
        if (nfail++ < 8u) {
            HLOG_W("INTERP Schatten fehlgeschlagen: %ux%u fmt=%u bind=0x%X hr=0x%08X", dd.Width, dd.Height,
                   (unsigned)dd.Format, dd.BindFlags, (unsigned)hr);
        }
        ip_sh_free(e);
        return -1;
    }
    e->res = res;
    ID3D11Resource_AddRef(res);
    if (g_cfg.interp_diag >= 1) {
        static unsigned nlog;
        if (nlog++ < 48u) {
            HLOG_I("INTERP Schatten #%d: %ux%u fmt=%u bind=0x%X", free_i, dd.Width, dd.Height,
                   (unsigned)dd.Format, dd.BindFlags);
        }
    }
    return free_i;
}

/* Ressource der View fuer Liste L (Index li) sichern (einmal je Liste),
 * Eintrag liefern. need_rt: Textur -> nur Render-Ziele haben Schatten. */
static int ip_sh_ref(ip_list_t *L, int li, ID3D11View *v, int need_rt)
{
    ID3D11Resource *res = NULL;
    int e, j;
    if (!v) { return -1; }
    ID3D11View_GetResource(v, &res);
    if (!res) { L->snap_ok = 0; return -1; }
    if (need_rt) {
        D3D11_TEXTURE2D_DESC dd;
        D3D11_RESOURCE_DIMENSION dim;
        ID3D11Resource_GetType(res, &dim);
        if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) { ID3D11Resource_Release(res); return -1; }
        ID3D11Texture2D_GetDesc((ID3D11Texture2D *)res, &dd);
        /* FIX(SHADOWBUF,W127): Schattenpuffer (Tiefen-RT als Textur der Stufe 1)
         * bekommt ebenfalls einen Schatten - die Wiedergabe sampelt dann den in
         * der Wiedergabe gezeichneten Tiefenpass statt des Spielstands. */
        if (!(dd.BindFlags & D3D11_BIND_RENDER_TARGET) &&
            !(g_cfg.shade_shadow && (dd.BindFlags & D3D11_BIND_DEPTH_STENCIL))) { ID3D11Resource_Release(res); return -1; }
    }
    e = ip_sh_find(res);
    if (e < 0) { ID3D11Resource_Release(res); L->snap_ok = 0; return -1; }
    for (j = 0; j < L->nrefs && L->refs[j] != e; j++) {}
    if (j == L->nrefs) {
        if (L->nrefs >= IP_REFS_MAX) { ID3D11Resource_Release(res); L->snap_ok = 0; return -1; }
        L->refs[L->nrefs++] = e;
        ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)g_ish[e].bak[li], res);
        g_ish[e].bakser[li] = L->serial;
        g_ish[e].used = L->serial;
    }
    ID3D11Resource_Release(res);
    return e;
}

/* Am Swap: alle Ressourcen der Liste sichern, Schatten-Views eintragen. */
static void ip_list_snap(ip_list_t *L, int li)
{
    unsigned i;
    ID3D11View *lv[3] = { NULL, NULL, NULL };
    int le[3] = { -1, -1, -1 };
    L->snap_ok = 1;
    L->nrefs = 0;
    L->bbref = ip_sh_ref(L, li, (ID3D11View *)g_rtv, 0);
    for (i = 0; i < L->nops; i++) {
        ip_op_t *o = &L->ops[i];
        ID3D11View *vv[3];
        int q;
        vv[0] = (ID3D11View *)o->rtv;
        vv[1] = (ID3D11View *)o->dsv;
        vv[2] = (ID3D11View *)o->srv;
        o->sh_rtv = NULL;
        o->sh_dsv = NULL;
        o->sh_srv = o->srv;
        for (q = 0; q < 3; q++) {
            int e;
            if (!vv[q]) { continue; }
            if (vv[q] == lv[q]) { e = le[q]; } /* gleiche View wie zuvor (lebt in der Liste) */
            else { e = ip_sh_ref(L, li, vv[q], q == 2); lv[q] = vv[q]; le[q] = e; }
            if (q == 0) { o->sh_rtv = (e >= 0) ? g_ish[e].rtv : NULL; }
            else if (q == 1) { o->sh_dsv = (e >= 0) ? g_ish[e].dsv : NULL; }
            else if (e >= 0) { o->sh_srv = g_ish[e].srv; }
        }
        if (o->rq) { /* RENDER-QA: Stufen 1..3 (RT-Texturen bekommen Schatten) */
            int k2;
            for (k2 = 1; k2 < 4; k2++) {
                int e2;
                o->rq_shsrv[k2] = o->rqr.srv[k2];
                if (!o->rqr.srv[k2]) { continue; }
                e2 = ip_sh_ref(L, li, (ID3D11View *)o->rqr.srv[k2], 1);
                if (e2 >= 0) { o->rq_shsrv[k2] = g_ish[e2].srv; }
            }
        }
        if ((o->rtv && !o->sh_rtv) || (o->dsv && !o->sh_dsv)) { L->snap_ok = 0; }
    }
    if (L->bbref < 0) { L->snap_ok = 0; }
}

/* PERF(RENDER-THREAD,W126): Ziel-Context einer Wiedergabe. Immediate (Spiel-
 * Thread, bisheriger Pfad) oder Deferred (Render-Thread, eigene Konstanten-
 * puffer; Zustand beginnt leer, wird nicht gesichert/wiederhergestellt). */
typedef struct {
    ID3D11DeviceContext *c;
    ID3D11DeviceContext1 *c1;
    ID3D11Buffer *cb0, *cb1;
    int deferred;
} ip_cx_t;

/* Schatten vor der Wiedergabe von B auf den Tick-Anfang setzen: Stand vom
 * Ende des Vorticks A (falls dort gesichert), sonst Ende von B (in A nicht
 * benutzt -> seit davor unveraendert bzw. wird in B neu gezeichnet). */
static const unsigned char *ip_sh_noskip; /* W146: Eintraege ohne Startkopie (NULL = alle kopieren) */
static void ip_sh_start(ip_list_t *B, int bi, ip_list_t *A, int ai, const ip_cx_t *x)
{
    int j;
    for (j = 0; j < B->nrefs; j++) {
        ip_sh_t *e = &g_ish[B->refs[j]];
        ID3D11Texture2D *src = NULL;
        if (ip_sh_noskip && ip_sh_noskip[B->refs[j]]) { continue; } /* W146 GPU-COST: erst Clear bzw. ruhig */
        if (A && ai >= 0 && e->bakser[ai] == A->serial) { src = e->bak[ai]; }
        else if (e->bakser[bi] == B->serial) { src = e->bak[bi]; }
        if (src) {
            ID3D11DeviceContext_CopyResource(x->c, (ID3D11Resource *)e->sh, (ID3D11Resource *)src);
        }
    }
}

/* ---------- Wiedergabe (in Schatten-Ziele) ---------- */

typedef struct {
    ID3D11RenderTargetView *rtv;
    ID3D11DepthStencilView *dsv;
    ID3D11BlendState *bs;
    float bf[4];
    UINT smask;
    ID3D11DepthStencilState *ds;
    UINT sref;
    ID3D11RasterizerState *rs;
    D3D11_VIEWPORT vp;
    UINT nvp;
    D3D11_RECT sc;
    UINT nsc;
    ID3D11ShaderResourceView *srv;
    ID3D11Buffer *vb[2];
    UINT vbs[2], vbo[2];
    ID3D11InputLayout *lay;
    D3D11_PRIMITIVE_TOPOLOGY topo;
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11Buffer *vcb[2], *pcb;
    ID3D11SamplerState *smp[4];          /* RENDER-QA: Shading-Wiedergabe setzt s0..s3 */
    ID3D11ShaderResourceView *srvx[3];   /* t1..t3 */
    ID3D11Buffer *vb2; UINT vbs2, vbo2;  /* RENDER-QA: Stream 2 */
    ID3D11Buffer *vcb3, *pcb4;           /* RENDER-QA: Konstanten-Fenster b3/b4 */
    ID3D11ShaderResourceView *vsrv0;     /* SHADE-PERF (W80): Roh-Eingaenge VS-t0 */
    UINT vf3, vn3, pf4, pn4;
} ip_state_t;

static void ip_state_save(ip_state_t *s)
{
    memset(s, 0, sizeof *s);
    ID3D11DeviceContext_OMGetRenderTargets(g_ctx, 1, &s->rtv, &s->dsv);
    ID3D11DeviceContext_OMGetBlendState(g_ctx, &s->bs, s->bf, &s->smask);
    ID3D11DeviceContext_OMGetDepthStencilState(g_ctx, &s->ds, &s->sref);
    ID3D11DeviceContext_RSGetState(g_ctx, &s->rs);
    s->nvp = 1;
    ID3D11DeviceContext_RSGetViewports(g_ctx, &s->nvp, &s->vp);
    s->nsc = 1;
    ID3D11DeviceContext_RSGetScissorRects(g_ctx, &s->nsc, &s->sc);
    ID3D11DeviceContext_PSGetShaderResources(g_ctx, 0, 1, &s->srv);
    ID3D11DeviceContext_IAGetVertexBuffers(g_ctx, 0, 2, s->vb, s->vbs, s->vbo);
    ID3D11DeviceContext_IAGetInputLayout(g_ctx, &s->lay);
    ID3D11DeviceContext_IAGetPrimitiveTopology(g_ctx, &s->topo);
    ID3D11DeviceContext_VSGetShader(g_ctx, &s->vs, NULL, NULL);
    ID3D11DeviceContext_PSGetShader(g_ctx, &s->ps, NULL, NULL);
    ID3D11DeviceContext_VSGetConstantBuffers(g_ctx, 0, 2, s->vcb);
    ID3D11DeviceContext_PSGetConstantBuffers(g_ctx, 0, 1, &s->pcb);
    ID3D11DeviceContext_PSGetSamplers(g_ctx, 0, 4, s->smp);
    ID3D11DeviceContext_PSGetShaderResources(g_ctx, 1, 3, s->srvx);
    /* RENDER-QA (W77): Wiedergaben laufen im Echtzeit-Takt auch zwischen
     * rq_bind und dem Draw eines Shading-Draws (ip_poll in rec_draw) ->
     * Stream 2 und die Konstanten-Fenster ebenfalls sichern. */
    if (g_cfg.shading) { ID3D11DeviceContext_IAGetVertexBuffers(g_ctx, 2, 1, &s->vb2, &s->vbs2, &s->vbo2); }
    if (g_cfg.shading) { ID3D11DeviceContext_VSGetShaderResources(g_ctx, 0, 1, &s->vsrv0); }
    if (g_rq_ctx1) {
        ID3D11DeviceContext1_VSGetConstantBuffers1(g_rq_ctx1, 3, 1, &s->vcb3, &s->vf3, &s->vn3);
        ID3D11DeviceContext1_PSGetConstantBuffers1(g_rq_ctx1, 4, 1, &s->pcb4, &s->pf4, &s->pn4);
    }
}

static void ip_state_restore(ip_state_t *s)
{
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &s->rtv, s->dsv);
    ID3D11DeviceContext_OMSetBlendState(g_ctx, s->bs, s->bf, s->smask);
    ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, s->ds, s->sref);
    ID3D11DeviceContext_RSSetState(g_ctx, s->rs);
    if (s->nvp) { ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &s->vp); }
    if (s->nsc) { ID3D11DeviceContext_RSSetScissorRects(g_ctx, 1, &s->sc); }
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, &s->srv);
    ID3D11DeviceContext_IASetVertexBuffers(g_ctx, 0, 2, s->vb, s->vbs, s->vbo);
    ID3D11DeviceContext_IASetInputLayout(g_ctx, s->lay);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, s->topo);
    ID3D11DeviceContext_VSSetShader(g_ctx, s->vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_ctx, s->ps, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(g_ctx, 0, 2, s->vcb);
    ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 0, 1, &s->pcb);
    ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 4, s->smp);
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 1, 3, s->srvx);
    if (g_cfg.shading) { ID3D11DeviceContext_IASetVertexBuffers(g_ctx, 2, 1, &s->vb2, &s->vbs2, &s->vbo2); }
    if (s->vb2) { ID3D11Buffer_Release(s->vb2); }
    if (g_cfg.shading) { ID3D11DeviceContext_VSSetShaderResources(g_ctx, 0, 1, &s->vsrv0); }
    if (s->vsrv0) { ID3D11ShaderResourceView_Release(s->vsrv0); }
    if (g_rq_ctx1) {
        if (s->vcb3) {
            ID3D11DeviceContext1_VSSetConstantBuffers1(g_rq_ctx1, 3, 1, &s->vcb3, &s->vf3, &s->vn3);
            ID3D11Buffer_Release(s->vcb3);
        }
        if (s->pcb4) {
            ID3D11DeviceContext1_PSSetConstantBuffers1(g_rq_ctx1, 4, 1, &s->pcb4, &s->pf4, &s->pn4);
            ID3D11Buffer_Release(s->pcb4);
        }
    }
    {
        int k;
        for (k = 0; k < 4; k++) { if (s->smp[k]) { ID3D11SamplerState_Release(s->smp[k]); } }
        for (k = 0; k < 3; k++) { if (s->srvx[k]) { ID3D11ShaderResourceView_Release(s->srvx[k]); } }
    }
    if (s->rtv) { ID3D11RenderTargetView_Release(s->rtv); }
    if (s->dsv) { ID3D11DepthStencilView_Release(s->dsv); }
    if (s->bs) { ID3D11BlendState_Release(s->bs); }
    if (s->ds) { ID3D11DepthStencilState_Release(s->ds); }
    if (s->rs) { ID3D11RasterizerState_Release(s->rs); }
    if (s->srv) { ID3D11ShaderResourceView_Release(s->srv); }
    if (s->vb[0]) { ID3D11Buffer_Release(s->vb[0]); }
    if (s->vb[1]) { ID3D11Buffer_Release(s->vb[1]); }
    if (s->lay) { ID3D11InputLayout_Release(s->lay); }
    if (s->vs) { ID3D11VertexShader_Release(s->vs); }
    if (s->ps) { ID3D11PixelShader_Release(s->ps); }
    if (s->vcb[0]) { ID3D11Buffer_Release(s->vcb[0]); }
    if (s->vcb[1]) { ID3D11Buffer_Release(s->vcb[1]); }
    if (s->pcb) { ID3D11Buffer_Release(s->pcb); }
}

static void ip_cb_write(const ip_cx_t *x, ID3D11Buffer *cb, const float m[16], unsigned u, float f, int isf)
{
    D3D11_MAPPED_SUBRESOURCE map;
    HRESULT hrm;
    WD_CALL("ip_cb_map", NULL, hrm = ID3D11DeviceContext_Map(x->c, (ID3D11Resource *)cb, 0,
                                       D3D11_MAP_WRITE_DISCARD, 0, &map)); /* DIAG(PERF-WARM,W139) */
    if (FAILED(hrm)) {
        return;
    }
    memcpy(map.pData, m, 64);
    if (isf) { *(float *)((unsigned char *)map.pData + 64) = f; }
    else { *(unsigned *)((unsigned char *)map.pData + 64) = u; }
    memcpy((unsigned char *)map.pData + 68, g_cw_par, sizeof g_cw_par); /* W166 COCKPIT-WIDEN */
    ID3D11DeviceContext_Unmap(x->c, (ID3D11Resource *)cb, 0);
}

/* Tick B in die Schatten wiedergeben (Anfangsstand aus A bzw. B);
 * 0<t<1 und gueltiges Paar -> Welt-Draws mit flag=1 lerpen. */
static const float kIP_DBG_COL[IP_W_N][3] = {
    { 1, 1, 1 }, { 0.5f, 0.5f, 0.5f }, { 0, 1, 0 }, { 0, 1, 1 }, { 1, 0, 0 },
    { 1, 1, 0 }, { 1, 0, 1 }, { 1, 1, 1 }, { 0, 0, 1 }, { 1, 0.5f, 0 } };

/* dbg = 1 (interp_diag>=3): Welt-/RT-Draws in der Farbe ihres Zuordnungs-
 * Ergebnisses (kIP_DBG_COL), 2D im BB normal (Monitore zeigen die RTs). */
static void ip_replay_ex(ip_list_t *B, int bi, ip_list_t *A, int ai, float t, int dbg, const ip_cx_t *x);
static const ip_cx_t *ip_cx_imm(void);
static void ip_replay(ip_list_t *B, int bi, ip_list_t *A, int ai, float t)
{
    ip_replay_ex(B, bi, A, ai, t, 0, ip_cx_imm());
}

/* DIAG(TICK-PERF,W125) perf_gsplit=N: jede N-te Zwischenbild-Wiedergabe mit
 * Timestamps an jedem Zielwechsel; synchron ausgewertet (Diagnose, kostet
 * einen GPU-Sync je Messung). Zeile GPUSPLIT: Summe je Zielgroesse. */
#define IP_GS_MAX 512
static ID3D11Query *ip_gs_dj, *ip_gs_q[IP_GS_MAX];
static ID3D11RenderTargetView *ip_gs_rtv[IP_GS_MAX];
static unsigned ip_gs_draws[IP_GS_MAX], ip_gs_rq[IP_GS_MAX];
static int ip_gs_n = -1; /* -1 = keine Messung in dieser Wiedergabe */
static unsigned ip_gs_cnt;

static void ip_gs_mark(ID3D11RenderTargetView *rtv)
{
    if (ip_gs_n < 0 || ip_gs_n >= IP_GS_MAX) { return; }
    if (!ip_gs_q[ip_gs_n]) {
        D3D11_QUERY_DESC qd;
        memset(&qd, 0, sizeof qd);
        qd.Query = D3D11_QUERY_TIMESTAMP;
        if (FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &ip_gs_q[ip_gs_n]))) { ip_gs_n = -1; return; }
    }
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)ip_gs_q[ip_gs_n]);
    ip_gs_rtv[ip_gs_n] = rtv;
    ip_gs_draws[ip_gs_n] = 0;
    ip_gs_rq[ip_gs_n] = 0;
    ip_gs_n++;
}

static int ip_gs_begin(float t)
{
    D3D11_QUERY_DESC qd;
    ip_gs_n = -1;
    if (g_cfg.perf_gsplit <= 0 || t >= 1.0f || !rq_in_mission()) { return 0; }
    if ((++ip_gs_cnt % (unsigned)g_cfg.perf_gsplit) != 0u) { return 0; }
    if (!ip_gs_dj) {
        memset(&qd, 0, sizeof qd);
        qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        if (FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &ip_gs_dj))) { ip_gs_dj = NULL; return 0; }
    }
    ID3D11DeviceContext_Begin(g_ctx, (ID3D11Asynchronous *)ip_gs_dj);
    ip_gs_n = 0;
    ip_gs_mark(NULL); /* Segment 0 = Schatten-Startkopien */
    return 1;
}

static void ip_gs_end(unsigned nops)
{
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
    UINT64 ts[IP_GS_MAX];
    int i, n, k, nk = 0;
    struct { unsigned w, h, segs, draws, rq; double ms; } ag[24];
    double tot, copy;
    char line[1400];
    int o = 0;
    DWORD t0;
    if (ip_gs_n < 0) { return; }
    ip_gs_mark((ID3D11RenderTargetView *)1); /* Ende */
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)ip_gs_dj);
    n = ip_gs_n;
    ip_gs_n = -1;
    t0 = GetTickCount();
    while (ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)ip_gs_dj, &dj, sizeof dj, 0) == S_FALSE) {
        if (GetTickCount() - t0 > 500u) { return; }
        YieldProcessor();
    }
    if (dj.Disjoint || !dj.Frequency || n < 2) { return; }
    for (i = 0; i < n; i++) {
        if (ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)ip_gs_q[i], &ts[i], sizeof ts[i], 0) != S_OK) { return; }
    }
    memset(ag, 0, sizeof ag);
    tot = (double)(ts[n - 1] - ts[0]) * 1000.0 / (double)dj.Frequency;
    copy = (double)(ts[1] - ts[0]) * 1000.0 / (double)dj.Frequency;
    for (i = 1; i < n - 1; i++) {
        unsigned w = 0, h = 0;
        double ms = (double)(ts[i + 1] - ts[i]) * 1000.0 / (double)dj.Frequency;
        if ((uintptr_t)ip_gs_rtv[i] > 16u) { /* 1/2 = Marken */
            ID3D11Resource *r = NULL;
            D3D11_TEXTURE2D_DESC td;
            ID3D11View_GetResource((ID3D11View *)ip_gs_rtv[i], &r);
            if (r) {
                ID3D11Texture2D_GetDesc((ID3D11Texture2D *)r, &td);
                w = td.Width; h = td.Height;
                ID3D11Resource_Release(r);
            }
        }
        for (k = 0; k < nk && (ag[k].w != w || ag[k].h != h); k++) {}
        if (k == nk) { if (nk >= 24) { continue; } ag[k].w = w; ag[k].h = h; nk++; }
        ag[k].ms += ms; ag[k].segs++; ag[k].draws += ip_gs_draws[i]; ag[k].rq += ip_gs_rq[i];
    }
    for (k = 0; k < nk && o >= 0; k++) {
        o += _snprintf_s(line + o, sizeof line - (size_t)o, _TRUNCATE, " | %ux%u ms=%.2f segs=%u draws=%u rq=%u",
                         ag[k].w, ag[k].h, ag[k].ms, ag[k].segs, ag[k].draws, ag[k].rq);
    }
    HLOG_I("GPUSPLIT f=%u ops=%u total=%.2fms startkopien=%.2fms segs=%d%s", g_present_no, nops, tot, copy, n - 2, line);
}

/* ---------- W146 GPU-COST: dynamische Render-Skalierung (rt_scale_dyn) ----------
 * GPU-Zeit je Zwischenbild (Wiedergabe + Blit + Nachbearbeitung, asynchrone
 * Timestamps, kein Sync) in 250-ms-Fenstern. Ueber rt_scale_dyn_hi fuer 1 s ->
 * Faktor der Welt-/Monitor-RTs eine Stufe runter (nicht unter rt_scale_dyn_min),
 * unter rt_scale_dyn_lo fuer 8 s -> eine Stufe hoch (nicht ueber den auto-/
 * rt_scale-Faktor). Mindestens 2 s zwischen Wechseln; folgt einem Hochschalten
 * binnen 15 s wieder ein Runterschalten, ruht das Hochschalten 60 s (kein
 * Pendeln). Umgesetzt im Spiel-Thread am Swap-Anfang (sb_rt_dyn_apply). Das
 * Seitenverhaeltnis bleibt (logische RT-Groesse und Abbildung unveraendert). */
#define DY_N 6
static ID3D11Query *dy_dj[DY_N], *dy_t0[DY_N], *dy_t1[DY_N];
static int dy_st[DY_N], dy_open = -1, dy_last_dir, dy_fail;
static double dy_acc, dy_last_avg;
static unsigned dy_n, dy_switches;
static LONGLONG dy_win_q, dy_hi_since, dy_lo_since, dy_last_switch, dy_up_block;
static volatile LONG g_rt_dyn_want;
static unsigned dy_up_veto, dy_wait_n; /* W165: Hochschalten per Prognose verworfen / Wechsel wartet auf Saetze */
static int dy_pref_done;               /* W165: Satz eine Stufe tiefer vorab bestellt */

static void dy_poll(void)
{
    int k;
    for (k = 0; k < DY_N; k++) {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        UINT64 a, b;
        if (dy_st[k] != 2) { continue; }
        if (ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)dy_dj[k], &dj, sizeof dj,
                                        D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) { continue; }
        dy_st[k] = 0;
        if (dj.Disjoint || !dj.Frequency) { continue; }
        if (ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)dy_t0[k], &a, sizeof a, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)dy_t1[k], &b, sizeof b, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            b < a) { continue; }
        dy_acc += (double)(b - a) * 1000.0 / (double)dj.Frequency;
        dy_n++;
    }
}

static void dy_begin(void)
{
    int k;
    if (g_cfg.rt_scale_dyn <= 0 || dy_fail || dy_open >= 0 || !rq_in_mission()) { return; }
    dy_poll();
    for (k = 0; k < DY_N && dy_st[k] != 0; k++) {}
    if (k >= DY_N) { return; }
    if (!dy_dj[k]) {
        D3D11_QUERY_DESC qd;
        memset(&qd, 0, sizeof qd);
        qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        if (FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &dy_dj[k]))) { dy_dj[k] = NULL; dy_fail = 1; return; }
        qd.Query = D3D11_QUERY_TIMESTAMP;
        if (FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &dy_t0[k])) ||
            FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &dy_t1[k]))) { dy_fail = 1; return; }
    }
    ID3D11DeviceContext_Begin(g_ctx, (ID3D11Asynchronous *)dy_dj[k]);
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)dy_t0[k]);
    dy_st[k] = 1;
    dy_open = k;
}

static void dy_ctrl(void)
{
    LONGLONG now = ip_now(), f = ip_qpf.QuadPart;
    unsigned cur, base, mn;
    double avg, hi, lo;
    LONG want = 0;
    if (!dy_win_q) { dy_win_q = now; }
    if (now - dy_win_q < f / 4) { return; }
    dy_win_q = now;
    if (!dy_n) { return; }
    avg = dy_acc / (double)dy_n;
    dy_acc = 0.0;
    dy_n = 0;
    dy_last_avg = avg;
    cur = sb_rtt_dyn_factor();
    base = sb_rtt_base_factor();
    mn = (unsigned)(g_cfg.rt_scale_dyn_min > 0 ? g_cfg.rt_scale_dyn_min : 1);
    if (mn > base) { mn = base; }
    hi = (double)g_cfg.rt_scale_dyn_hi / 10.0;
    lo = (double)g_cfg.rt_scale_dyn_lo / 10.0;
    if (avg > hi) { if (!dy_hi_since) { dy_hi_since = now; } } else { dy_hi_since = 0; }
    if (avg < lo) { if (!dy_lo_since) { dy_lo_since = now; } } else { dy_lo_since = 0; }
    if (dy_last_switch && now - dy_last_switch < 2 * f) { return; }
    if (dy_hi_since && now - dy_hi_since >= f && cur > mn) { want = (LONG)cur - 1; }
    else if (dy_lo_since && now - dy_lo_since >= 8 * f && cur < base && now >= dy_up_block) {
        /* W165 rt_scale_dyn_keep: nur hoch, wenn auch die obere Stufe nach Prognose (Pixel ~ n^2,
         * obere Schranke: alles skaliert) unter der Runter-Schwelle bleibt -> kein Pendeln */
        double pr = avg * (double)((cur + 1u) * (cur + 1u)) / (double)(cur * cur);
        if (g_cfg.rt_scale_dyn_keep <= 0 || pr < hi) { want = (LONG)cur + 1; }
        else { dy_up_veto++; }
    }
    if (want) { InterlockedExchange(&g_rt_dyn_want, want); }
}

static void dy_end(void)
{
    if (dy_open < 0) { return; }
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)dy_t1[dy_open]);
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)dy_dj[dy_open]);
    dy_st[dy_open] = 2;
    dy_open = -1;
    dy_ctrl();
}

void sb_rt_dyn_apply(void)
{
    LONG w;
    unsigned cur;
    int r;
    LONGLONG now;
    if (g_cfg.rt_scale_dyn <= 0) { return; }
    if (g_cfg.rt_scale_dyn_keep > 0 && rq_in_mission()) {
        /* W165: Saetze eine Stufe unter dem aktuellen Faktor vorab im Hilfs-Thread anlegen
         * (einmal je Stufe; neue RTs werden beim Wechsel nachbestellt) */
        unsigned c0 = sb_rtt_dyn_factor(), mn0 = (unsigned)(g_cfg.rt_scale_dyn_min > 0 ? g_cfg.rt_scale_dyn_min : 1);
        if (c0 > mn0 && dy_pref_done != (int)c0) {
            sb_ctx_enter();
            sb_rtt_keep_prefetch(c0 - 1u);
            sb_ctx_leave();
            dy_pref_done = (int)c0;
        }
    }
    w = InterlockedExchange(&g_rt_dyn_want, 0);
    if (!w) { return; }
    cur = sb_rtt_dyn_factor();
    if ((unsigned)w == cur) { return; }
    sb_ctx_enter();
    r = sb_rtt_rescale((unsigned)w);
    sb_ctx_leave();
    if (r == -2) { /* W165 keep: Saetze entstehen noch im Hilfs-Thread -> naechster Swap */
        if (dy_wait_n++ == 0) { HLOG_I("RT-DYN: Faktor %u -> %ld wartet auf vorab angelegte RTs (W165 keep)", cur, w); }
        InterlockedCompareExchange(&g_rt_dyn_want, w, 0);
        return;
    }
    if (r < 0) { InterlockedCompareExchange(&g_rt_dyn_want, w, 0); return; } /* RT gebunden: naechster Swap */
    now = ip_now();
    if ((unsigned)w < cur && dy_last_dir > 0 && now - dy_last_switch < 15 * ip_qpf.QuadPart) {
        dy_up_block = now + 60 * ip_qpf.QuadPart; /* gependelt: 60 s nicht hochschalten */
    }
    dy_last_dir = (unsigned)w > cur ? 1 : -1;
    dy_last_switch = now;
    dy_hi_since = dy_lo_since = 0;
    dy_switches++;
    HLOG_I("RT-DYN: Faktor %u -> %ld (%d RTs neu angelegt, GPU %.2f ms je Zwischenbild, Wechsel #%u, f=%u)",
           cur, w, r, dy_last_avg, dy_switches, g_present_no);
    if (g_cfg.rt_scale_dyn_keep > 0) { /* W165: neue Zeile (obige bleibt fuer Skripte gleich) */
        unsigned mk, jb, sw;
        double jms;
        sb_rtt_keep_stat(&mk, &jb, &sw, &jms);
        HLOG_I("RT-DYN keep: getauscht ohne Neuanlage, Saetze vorab=%u (Auftraege %u, letzter %.1f ms im Hilfs-Thread), "
               "gewartet=%u Swaps, hoch verworfen (Prognose)=%u", mk, jb, jms, dy_wait_n, dy_up_veto);
        dy_wait_n = 0;
    }
}

/* ---------- W146 GPU-COST: Zwischenbild-Cache (interp_cockpit_cache) ----------
 * Ein Zwischenbild zeichnet die komplette Aufzeichnung des Ticks neu. Zwei
 * Teile davon sind fuer alle t eines Paars (A,B) gleich:
 *  a) Ziele (Schatten-Eintraege), deren schreibende Ops in B alle exakt sind
 *     (Clears, Draws mit flag = 0 = exakt aus B, kein REM-FX) und nur feste
 *     Texturen oder selbst solche "ruhigen" Ziele lesen - typisch Sub-Monitore,
 *     Karte, Komposition. Ihr Endstand ist der gesicherte Tick-Stand bak[B]
 *     (Start = Ende von A = Start von B im Spiel). Bedingung: kein Lesen des
 *     Ziels vor seinem letzten Schreiben in B. -> Ops ueberspringen, bak[B] in
 *     den Schatten kopieren (Modus "ruhig", Bit 1 von interp_cockpit_cache).
 *  b) Der Anfang der Backbuffer-Ops (Clear, Cockpit-Gehaeuse ...), solange jede
 *     BB-Op exakt ist und nur feste Texturen/ruhige Ziele liest (Bit 2). Im
 *     ersten Zwischenbild des Paars gezeichnet und gesichert (Schatten-BB +
 *     BB-Tiefe), in den weiteren zurueckkopiert, Ops uebersprungen. Wackelt
 *     das Cockpit (flag = 1), endet der Anfang dort (kein 30-Hz-Wackeln).
 * Tick-Bilder unberuehrt (bak/Spiel-BB), Verify (t = 1), VR, Render-Thread und
 * Diagnosebild ohne Cache. interp_cockpit_cache: 0 aus, 1 = a, 2 = b, 3 = a+b. */
#define CC_MAXOPS 16384
static ID3D11Texture2D *cc_col, *cc_dep;
static unsigned cc_ser_a, cc_ser_b, cc_scan_ser;
static int cc_valid, cc_pfx = -1, cc_dep_e = -1, cc_why, cc_bbe = -1;
static unsigned cc_npfx, cc_nbb, cc_nskip, cc_nquiet;
static ID3D11DepthStencilView *cc_bdsv;
static unsigned char cc_skip[CC_MAXOPS];   /* 1 = Op schreibt nur ruhige Ziele */
static signed char cc_ent[CC_MAXOPS][3];   /* Eintrag rtv/dsv/srv (-1) */
static unsigned char cc_dirty[IP_SH_MAX], cc_wr[IP_SH_MAX];
static unsigned char cc_nosc[IP_SH_MAX];   /* 1 = keine Startkopie noetig (erste Beruehrung = Clear bzw. ruhig) */
static unsigned cc_nnosc, cc_st_nosc;
static unsigned cc_st_built, cc_st_used, cc_st_off, cc_st_why[8], cc_st_pfx, cc_st_bb, cc_st_n, cc_st_skip, cc_st_q,
    cc_st_ops, cc_st_rep;

static int cc_ent_of(const ip_list_t *B, const void *v, int q)
{
    int j;
    if (!v) { return -1; }
    for (j = 0; j < B->nrefs; j++) {
        const ip_sh_t *e = &g_ish[B->refs[j]];
        if ((q == 0 && (const void *)e->rtv == v) || (q == 1 && (const void *)e->dsv == v) ||
            (q == 2 && (const void *)e->srv == v)) { return B->refs[j]; }
    }
    return -1;
}

static int cc_op_exact(const ip_op_t *o)
{
    if (o->kind != IP_DRAW) { return 1; }
    return !o->flag && !(o->has_tex & FXR_MASK);
}

/* liest die Op ein unruhiges Ziel (Schatten-SRV, auch Stufen 1..3)? */
static int cc_reads_dirty(const ip_list_t *B, unsigned i)
{
    const ip_op_t *o = &B->ops[i];
    int k, e;
    if (o->kind != IP_DRAW) { return 0; }
    if (cc_ent[i][2] >= 0 && cc_dirty[cc_ent[i][2]]) { return 1; }
    if (o->rq) {
        for (k = 1; k < 4; k++) {
            if (o->rq_shsrv[k] && o->rq_shsrv[k] != o->rqr.srv[k]) {
                e = cc_ent_of(B, o->rq_shsrv[k], 2);
                if (e < 0 || cc_dirty[e]) { return 1; }
            }
        }
    }
    return 0;
}

static void cc_scan(ip_list_t *B, int bi)
{
    unsigned i, it, k;
    unsigned lastw[IP_SH_MAX];
    int changed;
    cc_scan_ser = B->serial;
    cc_pfx = 0; cc_why = 0; cc_npfx = cc_nbb = cc_nskip = cc_nquiet = 0;
    cc_bdsv = NULL; cc_dep_e = -1; cc_bbe = B->bbref;
    memset(cc_dirty, 0, sizeof cc_dirty);
    memset(cc_wr, 0, sizeof cc_wr);
    memset(lastw, 0, sizeof lastw);
    if (B->nops > CC_MAXOPS) { cc_why = 6; cc_pfx = 0; memset(cc_skip, 0, sizeof cc_skip); return; }
    for (i = 0; i < B->nops; i++) {
        const ip_op_t *o = &B->ops[i];
        cc_ent[i][0] = (signed char)cc_ent_of(B, o->sh_rtv, 0);
        cc_ent[i][1] = (signed char)cc_ent_of(B, o->sh_dsv, 1);
        cc_ent[i][2] = (signed char)((o->kind == IP_DRAW && o->sh_srv != o->srv) ? cc_ent_of(B, o->sh_srv, 2) : -1);
        if (o->kind == IP_DRAW && o->sh_srv != o->srv && cc_ent[i][2] < 0) { cc_ent[i][2] = -2; } /* unbekannt */
        for (k = 0; k < 2; k++) {
            int e = cc_ent[i][k];
            if (e >= 0) { cc_wr[e] = 1; lastw[e] = i + 1u; if (!cc_op_exact(o)) { cc_dirty[e] = 1; } }
        }
        if (!cc_bdsv && o->rtv == g_rtv && o->dsv) { cc_bdsv = o->dsv; cc_dep_e = cc_ent[i][1]; }
    }
    /* gelesene Ziele: Lesen vor dem letzten Schreiben -> Endstand passt nicht */
    for (i = 0; i < B->nops; i++) {
        int r = cc_ent[i][2];
        if (r >= 0 && cc_wr[r] && i < lastw[r]) { cc_dirty[r] = 1; }
    }
    if (cc_bbe >= 0) { cc_dirty[cc_bbe] = 1; } /* BB nie ganz ueberspringen (Anfangs-Cache b) */
    if (cc_dep_e >= 0) { cc_dirty[cc_dep_e] = 1; }
    for (it = 0; it < 16u; it++) { /* Ausbreitung: liest unruhig / gemeinsames Ziel+Tiefe */
        changed = 0;
        for (i = 0; i < B->nops; i++) {
            const ip_op_t *o = &B->ops[i];
            int e0 = cc_ent[i][0], e1 = cc_ent[i][1];
            int dyn = (cc_ent[i][2] == -2) || cc_reads_dirty(B, i) || (e0 >= 0 && cc_dirty[e0]) || (e1 >= 0 && cc_dirty[e1]);
            if (o->rtv && e0 < 0) { dyn = 1; }
            if (o->dsv && e1 < 0) { dyn = 1; }
            if (dyn) {
                if (e0 >= 0 && !cc_dirty[e0]) { cc_dirty[e0] = 1; changed = 1; }
                if (e1 >= 0 && !cc_dirty[e1]) { cc_dirty[e1] = 1; changed = 1; }
            }
        }
        if (!changed) { break; }
    }
    for (k = 0; k < IP_SH_MAX; k++) {
        if (cc_wr[k] && !cc_dirty[k]) {
            if (g_ish[k].bakser[bi] == B->serial && g_ish[k].bak[bi]) { cc_nquiet++; }
            else { cc_dirty[k] = 1; } /* ohne Sicherung kein Endstand */
        }
    }
    for (i = 0; i < B->nops; i++) {
        int e0 = cc_ent[i][0], e1 = cc_ent[i][1];
        cc_skip[i] = (unsigned char)((e0 >= 0 || e1 >= 0) && (e0 < 0 || !cc_dirty[e0]) && (e1 < 0 || !cc_dirty[e1]));
        if (cc_skip[i]) { cc_nskip++; }
    }
    /* c) Startkopie unnoetig: erste Beruehrung des Eintrags (Schreiben/Lesen) ist ein
     *    Clear, der ihn ganz ueberschreibt (D3D11-Clear = ganze View; Tiefe nur mit
     *    Tiefe + Stencil), oder der Eintrag ist ruhig (bekommt bak[B]). */
    {
        unsigned char seen[IP_SH_MAX];
        memset(seen, 0, sizeof seen);
        memset(cc_nosc, 0, sizeof cc_nosc);
        cc_nnosc = 0;
        for (i = 0; i < B->nops; i++) {
            const ip_op_t *o = &B->ops[i];
            int e0 = cc_ent[i][0], e1 = cc_ent[i][1], e2 = cc_ent[i][2];
            if (o->kind == IP_CLR_RTV && e0 >= 0 && !seen[e0]) { seen[e0] = 1; cc_nosc[e0] = 1; continue; }
            if (o->kind == IP_CLR_DSV && e1 >= 0 && !seen[e1]) { seen[e1] = 1; cc_nosc[e1] = (o->dsflags & 3u) == 3u; continue; }
            if (e0 >= 0) { seen[e0] = 1; }
            if (e1 >= 0) { seen[e1] = 1; }
            if (e2 >= 0) { seen[e2] = 1; }
            if (o->kind == IP_DRAW && o->rq) {
                for (k = 1; k < 4; k++) {
                    if (o->rq_shsrv[k] && o->rq_shsrv[k] != o->rqr.srv[k]) {
                        int e = cc_ent_of(B, o->rq_shsrv[k], 2);
                        if (e >= 0) { seen[e] = 1; }
                    }
                }
            }
        }
        for (k = 0; k < IP_SH_MAX; k++) {
            if (cc_wr[k] && !cc_dirty[k] && (g_cfg.interp_cockpit_cache & 1)) { cc_nosc[k] = 1; }
            if (cc_nosc[k]) { cc_nnosc++; }
        }
    }
    /* b) BB-Anfang */
    for (i = 0; i < B->nops; i++) {
        const ip_op_t *o = &B->ops[i];
        int bb = (o->rtv == g_rtv) || (cc_bdsv && o->dsv == cc_bdsv);
        int r = cc_ent[i][2];
        if (o->kind == IP_DRAW && (r == cc_bbe || (cc_dep_e >= 0 && r == cc_dep_e))) { cc_why = 1; break; }
        if (!bb) { continue; }
        if (o->kind != IP_DRAW) { cc_pfx = (int)i + 1; continue; }
        if (o->rtv != g_rtv) { cc_why = 2; break; }
        if (o->flag) { cc_why = 3; break; }
        if (o->has_tex & FXR_MASK) { cc_why = 4; break; }
        if (r == -2 || cc_reads_dirty(B, i)) { cc_why = 5; break; }
        cc_pfx = (int)i + 1;
        cc_npfx++;
    }
    for (i = 0; i < B->nops; i++) {
        if (B->ops[i].kind == IP_DRAW && B->ops[i].rtv == g_rtv) { cc_nbb++; }
    }
}

static int cc_mk(ID3D11Texture2D **dst, ID3D11Texture2D *like)
{
    D3D11_TEXTURE2D_DESC d, h;
    ID3D11Texture2D_GetDesc(like, &d);
    if (*dst) {
        ID3D11Texture2D_GetDesc(*dst, &h);
        if (h.Width == d.Width && h.Height == d.Height && h.Format == d.Format && h.SampleDesc.Count == d.SampleDesc.Count) {
            return 1;
        }
        ID3D11Texture2D_Release(*dst);
        *dst = NULL;
    }
    d.MiscFlags = 0;
    d.CPUAccessFlags = 0;
    d.Usage = D3D11_USAGE_DEFAULT;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &d, NULL, dst))) { *dst = NULL; return 0; }
    return 1;
}

/* Stand nach dem BB-Anfang sichern (Modus 1) */
static void cc_store(ip_list_t *A, ip_list_t *B, const ip_cx_t *x)
{
    ID3D11Texture2D *bc = g_ish[B->bbref].sh, *bd = cc_dep_e >= 0 ? g_ish[cc_dep_e].sh : NULL;
    if (!bc || !cc_mk(&cc_col, bc) || (bd && !cc_mk(&cc_dep, bd))) { cc_valid = 0; return; }
    ID3D11DeviceContext_CopyResource(x->c, (ID3D11Resource *)cc_col, (ID3D11Resource *)bc);
    if (bd) { ID3D11DeviceContext_CopyResource(x->c, (ID3D11Resource *)cc_dep, (ID3D11Resource *)bd); }
    cc_valid = 1;
    cc_ser_a = A->serial;
    cc_ser_b = B->serial;
    cc_st_built++;
}

/* Vor ip_sh_start: Scan (einmal je Tick) und Startkopie-Maske. 1 = Cache aktiv. */
static int cc_prep(ip_list_t *B, int bi, int lerp_ok)
{
    int mode = g_cfg.interp_cockpit_cache;
    ip_sh_noskip = NULL;
    if (mode <= 0 || !lerp_ok || B->bbref < 0) { return 0; }
    if (cc_scan_ser != B->serial) {
        cc_scan(B, bi);
        cc_st_n++;
        cc_st_why[cc_why & 7]++;
        cc_st_pfx += cc_npfx;
        cc_st_bb += cc_nbb;
        cc_st_skip += cc_nskip;
        cc_st_q += cc_nquiet;
        cc_st_ops += B->nops;
        cc_st_nosc += cc_nnosc;
    }
    if (mode & 4) { ip_sh_noskip = cc_nosc; } /* Bit 3: Startkopien sparen */
    return 1;
}

/* Rueckgabe: Bit 0..1 = BB-Anfang (0 aus, 1 aufbauen, 2 nutzen), Bit 4 = ruhige Ziele ueberspringen */
static int cc_begin(ip_list_t *B, int bi, ip_list_t *A, int lerp_ok, const ip_cx_t *x)
{
    int m = 0, k;
    int mode = g_cfg.interp_cockpit_cache;
    ip_sh_noskip = NULL;
    if (mode <= 0 || !lerp_ok || B->bbref < 0 || cc_scan_ser != B->serial) { return 0; }
    cc_st_rep++;
    if ((mode & 1) && cc_nskip) { /* a) ruhige Ziele: Tick-Endstand */
        for (k = 0; k < IP_SH_MAX; k++) {
            if (cc_wr[k] && !cc_dirty[k]) {
                ID3D11DeviceContext_CopyResource(x->c, (ID3D11Resource *)g_ish[k].sh, (ID3D11Resource *)g_ish[k].bak[bi]);
            }
        }
        m |= 16;
    }
    if ((mode & 2) && cc_pfx > 0 &&
        cc_npfx >= (unsigned)(g_cfg.interp_cockpit_cache_min > 0 ? g_cfg.interp_cockpit_cache_min : 1)) {
        if (cc_valid && cc_ser_a == A->serial && cc_ser_b == B->serial) {
            ID3D11DeviceContext_CopyResource(x->c, (ID3D11Resource *)g_ish[B->bbref].sh, (ID3D11Resource *)cc_col);
            if (cc_dep_e >= 0 && cc_dep) {
                ID3D11DeviceContext_CopyResource(x->c, (ID3D11Resource *)g_ish[cc_dep_e].sh, (ID3D11Resource *)cc_dep);
            }
            cc_st_used++;
            m |= 2;
        } else {
            cc_valid = 0;
            m |= 1;
        }
    } else if (mode & 2) {
        cc_st_off++;
    }
    if ((cc_st_rep % 1200u) == 1199u) {
        HLOG_I("INTERP ccache: bilder=%u ticks=%u ops/tick=%.0f ohne-startkopie/tick=%.1f ruhig: ziele/tick=%.1f ops/tick=%.0f | bb-anfang: aufgebaut=%u "
               "genutzt=%u aus=%u bb-draws/tick=%.0f davon im Anfang=%.0f | ende: alle=%u liest-bb=%u fremd-ziel=%u lerp=%u "
               "fx=%u liest-unruhig=%u gross=%u",
               cc_st_rep, cc_st_n, cc_st_n ? (double)cc_st_ops / cc_st_n : 0.0, cc_st_n ? (double)cc_st_nosc / cc_st_n : 0.0,
               cc_st_n ? (double)cc_st_q / cc_st_n : 0.0,
               cc_st_n ? (double)cc_st_skip / cc_st_n : 0.0, cc_st_built, cc_st_used, cc_st_off,
               cc_st_n ? (double)cc_st_bb / cc_st_n : 0.0, cc_st_n ? (double)cc_st_pfx / cc_st_n : 0.0, cc_st_why[0],
               cc_st_why[1], cc_st_why[2], cc_st_why[3], cc_st_why[4], cc_st_why[5], cc_st_why[6]);
    }
    return m;
}

/* ---------- W134 VR-PROTO: Augen-Durchgang ----------
 * Mit ip_vrp != NULL zeichnet ip_replay_ex jeden Draw/Clear, der im Spiel in
 * den Backbuffer ging (o->rtv == g_rtv), je Auge ins Augenziel statt in den
 * BB-Schatten. RT-Paesse laufen unveraendert (Welt im Monitor, Sub-Monitore).
 * VR-VS je Draw-Klasse: Spiel-Clip c -> Sitz-Sicht v (RH) -> Augen-Clip E:
 *  k2.z = 1  3D-Pfad (Objektraum, w = 1): v = (c.x/a', c.y/b, -c.w)
 *  k2.z = 0  2D/RHW (Pixel -> NDC):        v = (n.x*D/a', n.y*D/b, -D)
 *  k2.z = 2  P2 = Cockpit (fast orthografisch, w ~ 1e5): Relief um D aus z/w
 * Tiefe: z_Auge = (c.z/c.w) * w_Auge -> Tiefentest wie im Original (die
 * Klassen haben im Spiel nicht vergleichbare Tiefen, W134_e2). Shading-Draws
 * des 3D-Pfads behalten ihren VS: lineare Augen-Matrix (mvp*S + e44)*E, Tiefe
 * aus der Augenprojektion mit Near/Far der Spielkamera (= Original bei
 * geradem Kopf). Shading-Draws P2/2D im BB: IM-PS-Ersatz. */
static sb_vr_pass_t *ip_vrp;
static ID3D11VertexShader *ip_vr_vs1, *ip_vr_vs2;
static ID3D11Buffer *ip_vr_cb;
static int ip_vr_init_done;
static const char *kIP_VR_VS =
    "cbuffer CB : register(b0) { row_major float4x4 mvp; uint has_tex; };\n"
    "cbuffer IP : register(b1) { row_major float4x4 mvp0; float ip_t; };\n"
    "cbuffer VR : register(b2) { row_major float4x4 E; float4 k; float4 k2; };\n"
    "struct VSIN { float4 pos : POSITION0; float4 col : COLOR0;\n"
    "              float2 uv : TEXCOORD0;\n"
    "#ifdef VR_LERP\n"
    "              float4 pos0 : POSITION1;\n"
    "#endif\n"
    "};\n"
    "struct VSOUT { float4 pos : SV_Position; float4 col : COLOR0;\n"
    "               float2 uv : TEXCOORD0; };\n"
    "VSOUT main(VSIN i) { VSOUT o;\n"
    "  float4 c = mul(i.pos, mvp);\n"
    "#ifdef VR_LERP\n"
    "  float4 c0 = mul(i.pos0, mvp0); c = c0 + ip_t * (c - c0);\n"
    "#endif\n"
    "  float2 n = c.xy / c.w; float zn = c.z / c.w; float3 v; float md = 0.0;\n"
    "  if (k2.z > 1.5) { float d = clamp(k.z + k.w * (zn - k2.x), k.z - k2.y, k.z + k2.y);\n"
    "    if (k2.w > 1.5 && zn > 0.2 && zn < 0.999) { d = min(10.0 / (100.0 - 99.9 * zn), 90.0); md = 1.0; }\n"
    "    v = float3(n.x * d * k.x, n.y * d * k.y, -d); }\n"
    "  else if (k2.z > 0.5) { v = float3(c.x * k.x, c.y * k.y, -c.w); }\n"
    "  else { v = float3(n.x * k.z * k.x, n.y * k.z * k.y, -k.z); }\n"
    "  o.pos = mul(float4(v, 1.0), E);\n"
    "  if (md > 0.5) { o.pos.z -= 2e-5 * o.pos.w; }\n"
    "  else if (k2.w < 0.5 || k2.w > 1.5) { o.pos.z = zn * o.pos.w; }\n"
    "  o.col = i.col; o.uv = i.uv; return o; }\n";

static int ip_vr_init(void)
{
    ID3DBlob *b = NULL;
    D3D11_BUFFER_DESC bd;
    char src[2048];
    if (ip_vr_init_done) { return ip_vr_vs1 && ip_vr_vs2 && ip_vr_cb; }
    ip_vr_init_done = 1;
    if (sb_im_compile(kIP_VR_VS, "vs_4_0", &b)) {
        ID3D11Device_CreateVertexShader(g_dev, b->lpVtbl->GetBufferPointer(b), b->lpVtbl->GetBufferSize(b), NULL, &ip_vr_vs1);
        b->lpVtbl->Release(b);
        b = NULL;
    }
    _snprintf_s(src, sizeof src, _TRUNCATE, "#define VR_LERP 1\n%s", kIP_VR_VS);
    if (sb_im_compile(src, "vs_4_0", &b)) {
        ID3D11Device_CreateVertexShader(g_dev, b->lpVtbl->GetBufferPointer(b), b->lpVtbl->GetBufferSize(b), NULL, &ip_vr_vs2);
        b->lpVtbl->Release(b);
    }
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = 96;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &ip_vr_cb))) { ip_vr_cb = NULL; }
    HLOG_I("VR: Wiedergabe-Shader %s", (ip_vr_vs1 && ip_vr_vs2 && ip_vr_cb) ? "bereit" : "FEHLER");
    return ip_vr_vs1 && ip_vr_vs2 && ip_vr_cb;
}

/* P2 im BB: render_im.c setzt mvp = diag(vk, 1, 1, 1) (Clip liegt per CPU vor). */
static int ip_vr_isp2(const float m[16])
{
    return m[1] == 0.0f && m[2] == 0.0f && m[3] == 0.0f && m[4] == 0.0f && m[5] == 1.0f && m[6] == 0.0f &&
           m[7] == 0.0f && m[8] == 0.0f && m[9] == 0.0f && m[10] == 1.0f && m[11] == 0.0f && m[12] == 0.0f &&
           m[13] == 0.0f && m[14] == 0.0f && m[15] == 1.0f;
}

static void ip_m4mul(float r[16], const float a[16], const float b[16])
{
    int i, j;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            r[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] +
                           a[i * 4 + 2] * b[2 * 4 + j] + a[i * 4 + 3] * b[3 * 4 + j];
        }
    }
}

/* 3D-Pfad (Objektraum, w = 1): Spiel-mvp -> Augen-mvp, linear:
 * v = (X/a', Y/b, -W, pos.w) = pos * (mvp * S + e44), danach * E. */
static void ip_vr_mat3d(float out[16], const float mvp[16], const sb_vr_pass_t *p, int e)
{
    float S[16], T[16];
    memset(S, 0, sizeof S);
    S[0] = p->ia; S[5] = p->ib; S[14] = -1.0f;
    ip_m4mul(T, mvp, S);
    T[15] += 1.0f;
    ip_m4mul(out, T, p->E[e]);
}

static void ip_vr_eye_target(const ip_cx_t *x, const sb_vr_pass_t *p, int e, const ip_op_t *o)
{
    D3D11_VIEWPORT v;
    D3D11_RECT r;
    v.TopLeftX = p->vp[e][0]; v.TopLeftY = p->vp[e][1];
    v.Width = p->vp[e][2]; v.Height = p->vp[e][3];
    v.MinDepth = o->vp.MinDepth; v.MaxDepth = o->vp.MaxDepth; /* Tiefenbereich des Spiels (Schichtung) */
    r.left = (LONG)p->vp[e][0]; r.top = (LONG)p->vp[e][1];
    r.right = (LONG)(p->vp[e][0] + p->vp[e][2]); r.bottom = (LONG)(p->vp[e][1] + p->vp[e][3]);
    ID3D11DeviceContext_OMSetRenderTargets(x->c, 1, (ID3D11RenderTargetView *const *)&p->rtv,
                                           o->dsv ? (ID3D11DepthStencilView *)p->dsv : NULL);
    ID3D11DeviceContext_RSSetViewports(x->c, 1, &v);
    ID3D11DeviceContext_RSSetScissorRects(x->c, 1, &r);
}

static void ip_vr_cb_write(const ip_cx_t *x, const sb_vr_pass_t *p, int e, int kind)
{
    D3D11_MAPPED_SUBRESOURCE map;
    float *f;
    if (FAILED(ID3D11DeviceContext_Map(x->c, (ID3D11Resource *)ip_vr_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) { return; }
    f = (float *)map.pData;
    /* W164: k2.w = 1 (vr_world_depth, 3D-Pfad = Cockpit-Mesh): Tiefe affin aus der
     * Augenprojektion statt Original-z/w x w_Auge (das extrapoliert bei Dreiecken, die
     * die Augen-Ebene schneiden -> Flaechen mit Tiefe ~0 verdecken alles);
     * k2.w = 2 (vr_inst_depth, P2): Vertices mit perspektivischem z/w (> 0,2) auf die
     * metrische Tiefe w = n f / (f - z/w (f - n)) (Instrumente bleiben auf ihrer
     * Tafel), Tiefe ebenfalls affin (+ 2e-5 zum Auge: Decals mit LESS_EQUAL) */
    memcpy(f, p->E[e], 64);
    f[16] = p->ia; f[17] = p->ib; f[18] = p->d2d; f[19] = p->rl;
    f[20] = p->rz0; f[21] = p->rmax; f[22] = (float)kind;
    f[23] = (kind == 1 && p->wfix) ? 1.0f : ((kind == 2 && p->ifix) ? 2.0f : 0.0f);
    ID3D11DeviceContext_Unmap(x->c, (ID3D11Resource *)ip_vr_cb, 0);
}

/* Nicht-Shading-Draw je Auge. VS/Layout/Streams des bisherigen Pfads sind
 * gebunden (mode 0 = exakt, 1 = Lerp); P2 bindet den VR-VS und setzt mode = -1. */
static void ip_vr_draw(const ip_cx_t *x, sb_vr_pass_t *p, const ip_op_t *o, const ip_op_t *a, int kind,
                       float t, int lerp, int *mode)
{
    int e;
    ID3D11Buffer *cbs[3];
    if (g_cfg.vr_diag_skip & (1 << kind)) { return; } /* DIAG(VR-FIX2,W164): Draw-Klasse weglassen */
    if (!ip_vr_init()) { return; }
    ID3D11DeviceContext_VSSetShader(x->c, lerp ? ip_vr_vs2 : ip_vr_vs1, NULL, 0);
    cbs[0] = x->cb0; cbs[1] = x->cb1; cbs[2] = ip_vr_cb;
    ID3D11DeviceContext_VSSetConstantBuffers(x->c, 0, 3, cbs);
    if (lerp) { ip_cb_write(x, x->cb1, a->mvp, 0, t, 1); }
    *mode = -1;
    for (e = 0; e < p->neyes; e++) {
        ip_vr_eye_target(x, p, e, o);
        ip_vr_cb_write(x, p, e, kind);
        if (lerp) { ID3D11DeviceContext_Draw(x->c, o->nv, 0); }
        else { ID3D11DeviceContext_Draw(x->c, o->nv, o->vstart); }
    }
}

/* ---------- W183 VR-FIX3: Aussenwelt im Spieltakt (vr_world_tick=1) ----------
 * Je VR-Bild muss nur das Kopfabhaengige neu: die Draws, die im Spiel in den
 * Backbuffer gingen (Cockpit-Mesh, Instrumente, Monitor-Komposition, HUD) -
 * je Auge mit der aktuellen Pose. Die RT-Paesse (Aussenwelt im Hauptmonitor,
 * Sub-Monitore, Karte; ~75 % der Draws) haengen nicht vom Kopf ab: statt sie
 * je VR-Bild (90 Hz) neu zu zeichnen, bekommen ihre Schatten den gesicherten
 * Tick-Endstand bak[B] (Spieltakt 30 Hz, wie das Original). Sicher nur, wenn
 * kein BB-Draw ein RT liest, bevor B es zum letzten Mal schreibt, und jedes
 * geschriebene RT eine Sicherung von B hat - sonst volle Wiedergabe. Einmal je
 * Tick (Seriennummer) geprueft. */
static unsigned char vt_skip[CC_MAXOPS], vt_wr[IP_SH_MAX], vt_nosc[IP_SH_MAX];
static unsigned vt_ser, vt_nskip;
static int vt_ok;

static int vt_reads_late(const ip_list_t *B, const ip_op_t *o, unsigned i, const unsigned *lastw)
{
    int k, e;
    if (o->sh_srv && o->sh_srv != o->srv) {
        e = cc_ent_of(B, o->sh_srv, 2);
        if (e < 0) { return 1; }
        if (vt_wr[e] && i < lastw[e]) { return 1; }
    }
    if (o->rq) {
        for (k = 1; k < 4; k++) {
            if (o->rq_shsrv[k] && o->rq_shsrv[k] != o->rqr.srv[k]) {
                e = cc_ent_of(B, o->rq_shsrv[k], 2);
                if (e < 0 || (vt_wr[e] && i < lastw[e])) { return 1; }
            }
        }
    }
    return 0;
}

static void vt_scan(const ip_list_t *B, int bi)
{
    unsigned i, lastw[IP_SH_MAX];
    int k, bbread = 0, bbdep = -1;
    vt_ser = B->serial;
    vt_ok = 1;
    vt_nskip = 0;
    memset(vt_wr, 0, sizeof vt_wr);
    memset(vt_nosc, 0, sizeof vt_nosc);
    memset(lastw, 0, sizeof lastw);
    if (B->nops > CC_MAXOPS) { vt_ok = 0; return; }
    for (i = 0; i < B->nops; i++) {
        const ip_op_t *o = &B->ops[i];
        int bb = (o->kind == IP_DRAW && o->rtv == g_rtv) || (o->kind == IP_CLR_RTV && o->rtv == g_rtv) ||
                 (o->kind == IP_CLR_DSV && o->dsv == g_dsv);
        vt_skip[i] = (unsigned char)!bb;
        if (bb) {
            if (o->kind == IP_DRAW && o->sh_dsv && bbdep < 0) { bbdep = cc_ent_of(B, o->sh_dsv, 1); }
            continue;
        }
        vt_nskip++;
        if (o->kind != IP_CLR_DSV) {
            int e0 = cc_ent_of(B, o->sh_rtv, 0);
            if (o->sh_rtv && e0 < 0) { vt_ok = 0; return; }
            if (e0 >= 0) { vt_wr[e0] = 1; lastw[e0] = i + 1u; }
        }
        if (o->kind != IP_CLR_RTV) {
            int e1 = cc_ent_of(B, o->sh_dsv, 1);
            if (o->sh_dsv && e1 < 0) { vt_ok = 0; return; }
            if (e1 >= 0) { vt_wr[e1] = 1; lastw[e1] = i + 1u; }
        }
    }
    for (i = 0; i < B->nops; i++) {
        const ip_op_t *o = &B->ops[i];
        int r;
        if (vt_skip[i] || o->kind != IP_DRAW) { continue; }
        if (vt_reads_late(B, o, i, lastw)) { vt_ok = 0; return; }
        r = (o->sh_srv && o->sh_srv != o->srv) ? cc_ent_of(B, o->sh_srv, 2) : -1;
        if (r >= 0 && (r == B->bbref || r == bbdep)) { bbread = 1; }
    }
    for (k = 0; k < IP_SH_MAX; k++) {
        if (!vt_wr[k]) { continue; }
        if (g_ish[k].bakser[bi] != B->serial || !g_ish[k].bak[bi]) { vt_ok = 0; return; }
        vt_nosc[k] = 1; /* bekommt bak[B] statt der Startkopie bak[A] */
    }
    /* BB-Schatten: im Augenbild ungenutzt (Augenziel), ausser ein BB-Draw liest ihn. Die BB-Tiefe
     * behaelt ihre Startkopie (Effekt-Draws kopieren sie, W133). */
    if (!bbread && B->bbref >= 0 && B->bbref < IP_SH_MAX && !vt_wr[B->bbref]) { vt_nosc[B->bbref] = 1; }
}

static void ip_replay_ex(ip_list_t *B, int bi, ip_list_t *A, int ai, float t, int dbg, const ip_cx_t *x)
{
    int cps = -1; /* DIAG: 1 = Diagnose-PS gebunden */
    unsigned cwhy = 0xFFFFFFFFu;
    ip_state_t sv;
    unsigned i;
    UINT stride = sizeof(im_vertex_t), zero = 0;
    ID3D11RenderTargetView *crtv = (ID3D11RenderTargetView *)1;
    ID3D11DepthStencilView *cdsv = (ID3D11DepthStencilView *)1;
    ID3D11BlendState *cbs = (ID3D11BlendState *)1;
    ID3D11DepthStencilState *cds = (ID3D11DepthStencilState *)1;
    UINT csref = 0xFFFFFFFFu;
    ID3D11RasterizerState *crs = (ID3D11RasterizerState *)1;
    ID3D11ShaderResourceView *csrv = (ID3D11ShaderResourceView *)1;
    D3D11_VIEWPORT cvp;
    D3D11_RECT csc;
    unsigned ctopo = 0xFFFFFFFFu;
    int mode = -1; /* 0 exakt-VS, 1 interp-VS */
    int rq_dirty = 0; /* RENDER-QA: letzter Draw war ein Shading-Draw */
    ID3D11SamplerState *csmp = (ID3D11SamplerState *)1; /* FIX(TEXADDR,W84): gebundener s0 (1 = unbekannt) */
    int lerp_ok = (A != NULL && t > 0.0f && t < 1.0f && A->ready && B->pair_ok &&
                   B->prevser == A->serial);
    fxr_rep_t fxs; /* W133 REM-FX: Effekt-PS/Tiefen-Kopie der Wiedergabe */
    sb_vr_pass_t *vrp = (ip_vrp && !x->deferred && !dbg) ? ip_vrp : NULL; /* W134 VR-PROTO */
    int vrbb = 0, vrkind = -1; /* W134 VR-PROTO */
    int ccm = 0; /* W146 GPU-COST: Cockpit-Cache 0 aus, 1 aufbauen, 2 nutzen */
    int vtk = 0; /* W183 VR-FIX3: 1 = RT-Ops uebersprungen (Tick-Endstand) */
    fxr_rep_begin(&fxs);
    memset(&cvp, 0xFF, sizeof cvp);
    memset(&csc, 0xFF, sizeof csc);
    if (!x->deferred) {
        ip_state_save(&sv);
        if (!dbg) { ip_gs_begin(t); } /* DIAG(TICK-PERF,W125) */
    } else {
        /* PERF(RENDER-THREAD,W126): Deferred-Context beginnt leer. Einziger
         * geerbter Zustand der Wiedergabe ist s0 (tex_addr=0, Draw ohne
         * eigenen Sampler) -> s0 des Spiels beim Aufnahme-Start von B. */
        ID3D11DeviceContext_PSSetSamplers(x->c, 0, 1, &B->smp0);
    }
    if (!x->deferred && !vrp && !dbg) { cc_prep(B, bi, lerp_ok); } /* W146 GPU-COST: Scan + Startkopie-Maske */
    if (vrp && vrp->wtick) { /* W183 VR-FIX3: Aussenwelt im Spieltakt (Tick-Endstand), nur BB-Ops je Auge */
        static int vt_have;
        if (!vt_have || vt_ser != B->serial) { vt_scan(B, bi); vt_have = 1; }
        vtk = vt_ok;
        vrp->wtick_used = vtk;
        vrp->nskip = vtk ? vt_nskip : 0u;
        if (vtk) { ip_sh_noskip = vt_nosc; }
    }
    ip_sh_start(B, bi, A, ai, x);
    ip_sh_noskip = NULL;
    if (vtk) {
        int k;
        for (k = 0; k < IP_SH_MAX; k++) {
            if (vt_wr[k]) { ID3D11DeviceContext_CopyResource(x->c, (ID3D11Resource *)g_ish[k].sh, (ID3D11Resource *)g_ish[k].bak[bi]); }
        }
    }
    if (!x->deferred && !vrp && !dbg) { ccm = cc_begin(B, bi, A, lerp_ok, x); } /* W146 GPU-COST */
    if (ip_gs_n >= 0) { ip_gs_mark((ID3D11RenderTargetView *)2); } /* DIAG(TICK-PERF,W125): Kopien fertig */
    ID3D11DeviceContext_PSSetShader(x->c, g_im_ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(x->c, 0, 1, &x->cb0);
    for (i = 0; i < B->nops; i++) {
        ip_op_t *o = &B->ops[i];
        int lerp;
        if ((ccm & 16) && cc_skip[i]) { continue; } /* W146 GPU-COST: ruhiges Ziel (Tick-Endstand kopiert) */
        if (vtk && vt_skip[i]) { continue; } /* W183 VR-FIX3: RT-Op, Ziel hat den Tick-Endstand */
        if ((ccm & 3) && i < (unsigned)cc_pfx) { /* W146 GPU-COST: BB-Anfang aus dem Cockpit-Cache */
            if ((ccm & 3) == 2 && (o->rtv == g_rtv || (cc_bdsv && o->dsv == cc_bdsv))) { continue; }
        } else if ((ccm & 3) == 1 && i == (unsigned)cc_pfx) {
            cc_store(A, B, x);
            ccm &= ~3;
        }
        if (g_gp_open && !x->deferred) { /* DIAG(GPU-COST,W146) perf_gpass: Kategorie je Op */
            int gc;
            if (o->kind == IP_DRAW && (o->has_tex & FXR_MASK)) { gc = GP_FX; }
            else if (o->kind == IP_DRAW && o->nolerp) { gc = GP_MFX; }
            else { gc = sb_gp_rt_class(o->sh_rtv, o->rtv == g_rtv, o->kind == IP_DRAW && o->rq); }
            sb_gp_mark(gc);
        }
        if (ip_gs_n > 0 && o->sh_rtv && o->sh_rtv != ip_gs_rtv[ip_gs_n - 1]) { ip_gs_mark(o->sh_rtv); } /* DIAG(TICK-PERF,W125) */
        if (ip_gs_n > 0 && o->kind == IP_DRAW) { ip_gs_draws[ip_gs_n - 1]++; if (o->rq) { ip_gs_rq[ip_gs_n - 1]++; } }
        if (o->kind == IP_CLR_RTV) {
            if (vrp && o->rtv == g_rtv) { /* W134 VR-PROTO: BB-Clear -> Augenziel */
                ID3D11DeviceContext_ClearRenderTargetView(x->c, vrp->rtv, o->col);
                vrp->nclr++;
                continue;
            }
            if (o->sh_rtv) { WD_CALL("ip_clear_rtv", NULL, ID3D11DeviceContext_ClearRenderTargetView(x->c, o->sh_rtv, o->col)); }
            continue;
        }
        if (o->kind == IP_CLR_DSV) {
            if (vrp && o->dsv == g_dsv) { /* W134 VR-PROTO: BB-Tiefe -> Augentiefe */
                if (vrp->dsv) {
                    ID3D11DeviceContext_ClearDepthStencilView(x->c, vrp->dsv, o->dsflags, o->z, (UINT8)o->stencil);
                }
                continue;
            }
            if (o->sh_dsv) {
                WD_CALL("ip_clear_dsv", NULL, ID3D11DeviceContext_ClearDepthStencilView(x->c, o->sh_dsv, o->dsflags, o->z,
                                                          (UINT8)o->stencil));
            }
            continue;
        }
        if (o->sh_rtv != crtv || o->sh_dsv != cdsv) {
            ID3D11DeviceContext_OMSetRenderTargets(x->c, 1, &o->sh_rtv, o->sh_dsv);
            crtv = o->sh_rtv;
            cdsv = o->sh_dsv;
        }
        if (o->bs != cbs) {
            /* Blendfaktor/Maske setzt der IM-Pfad konstant (NULL/0xFFFFFFFF). */
            ID3D11DeviceContext_OMSetBlendState(x->c, o->bs, o->bf, o->smask);
            cbs = o->bs;
        }
        if (o->ds != cds || o->sref != csref) {
            ID3D11DeviceContext_OMSetDepthStencilState(x->c, o->ds, o->sref);
            cds = o->ds;
            csref = o->sref;
        }
        if (o->rs != crs) {
            ID3D11DeviceContext_RSSetState(x->c, o->rs);
            crs = o->rs;
        }
        if (memcmp(&o->vp, &cvp, sizeof cvp) != 0) {
            ID3D11DeviceContext_RSSetViewports(x->c, 1, &o->vp);
            cvp = o->vp;
        }
        if (memcmp(&o->sc, &csc, sizeof csc) != 0) {
            ID3D11DeviceContext_RSSetScissorRects(x->c, 1, &o->sc);
            csc = o->sc;
        }
        if (o->sh_srv != csrv) {
            ID3D11DeviceContext_PSSetShaderResources(x->c, 0, 1, &o->sh_srv);
            csrv = o->sh_srv;
        }
        if (o->topo != ctopo) {
            ID3D11DeviceContext_IASetPrimitiveTopology(x->c, (D3D11_PRIMITIVE_TOPOLOGY)o->topo);
            ctopo = o->topo;
        }
        ip_cb_write(x, x->cb0, o->mvp, o->has_tex, 0.0f, 0);
        if (dbg && ip_dbg_ps && ip_dbg_cb) {
            /* gefaerbt nur Draws, die NICHT interpoliert/unveraendert sind */
            int want = (o->why != IP_W_BB2D && o->why != IP_W_LERP && o->why != IP_W_STATIC &&
                        o->why != IP_W_RT2DLERP) ? 1 : 0;
            if (want != cps) {
                ID3D11DeviceContext_PSSetShader(x->c, want ? ip_dbg_ps : g_im_ps, NULL, 0);
                if (want) { ID3D11DeviceContext_PSSetConstantBuffers(x->c, 1, 1, &ip_dbg_cb); }
                cps = want;
            }
            if (want && o->why != cwhy) {
                D3D11_MAPPED_SUBRESOURCE dm;
                if (SUCCEEDED(ID3D11DeviceContext_Map(x->c, (ID3D11Resource *)ip_dbg_cb, 0,
                                                      D3D11_MAP_WRITE_DISCARD, 0, &dm))) {
                    float *f = (float *)dm.pData;
                    unsigned w = o->why < IP_W_N ? o->why : 0u;
                    f[0] = kIP_DBG_COL[w][0]; f[1] = kIP_DBG_COL[w][1]; f[2] = kIP_DBG_COL[w][2]; f[3] = 1;
                    ID3D11DeviceContext_Unmap(x->c, (ID3D11Resource *)ip_dbg_cb, 0);
                }
                cwhy = o->why;
            }
        }
        lerp = lerp_ok && o->flag && o->match >= 0 && (unsigned)o->match < A->nops;
        vrbb = vrp && o->rtv == g_rtv; /* W134 VR-PROTO: Draw in den Backbuffer -> je Auge */
        vrkind = !vrbb ? -1 : (!o->cls ? 0 : (ip_vr_isp2(o->mvp) ? 2 : 1));
        if (vrbb) {
            if (vrkind == 0) { vrp->n2d++; } else if (vrkind == 1) { vrp->n3d++; } else { vrp->np2++; }
            if (vrkind == 2) {
                int q;
                for (q = 0; q < 4; q++) {
                    if (o->rn[q][2] > 0.0f) {
                        float zn = o->pref[q][2] / o->pref[q][3];
                        vrp->p2w_sum += o->rn[q][2];
                        vrp->p2w_n++;
                        vrp->p2z_sum += zn;
                        if (vrp->p2w_n == 1u || zn < vrp->p2z_min) { vrp->p2z_min = zn; }
                        if (vrp->p2w_n == 1u || zn > vrp->p2z_max) { vrp->p2z_max = zn; }
                        if (vrp->wantz && vrp->zs_n < 1024u) { vrp->zs[vrp->zs_n++] = zn; }
                    }
                }
            }
            if (vrkind == 1) { int q; for (q = 0; q < 4; q++) { if (o->rn[q][2] > 0.0f && (vrp->w3_min <= 0.0f || o->rn[q][2] < vrp->w3_min)) { vrp->w3_min = o->rn[q][2]; }
                                                       if (o->rn[q][2] > 0.0f && vrp->wantz && vrp->ws_n < 512u) { vrp->ws[vrp->ws_n++] = o->rn[q][2]; } } } /* W164 */
            if (o->rq) { vrp->nrq++; if (vrkind != 1 || !x->c1 || vrp->noshade) { vrp->nrqfb++; } }
            if (vrp->dump > 0) { /* DIAG(VR-PROTO,W134): Eingangsdaten der BB-Draws */
                vrp->dump--;
                HLOG_I("VR op i=%u kind=%d n=%u rq=%d tex=0x%X lerp=%d vpz=[%.3f,%.3f] ds=%p mvp=[%.3f %.3f %.3f %.3f|%.3f %.3f %.3f %.3f|%.3f %.3f %.3f %.3f|%.3f %.3f %.3f %.3f] "
                       "p0=(%.3f,%.3f,%.4f,%.3f) p1=(%.3f,%.3f,%.4f,%.3f) p3=(%.3f,%.3f,%.4f,%.3f)",
                       i, vrkind, o->nv, o->rq, o->has_tex, lerp, o->vp.MinDepth, o->vp.MaxDepth, (void *)o->ds,
                       o->mvp[0], o->mvp[1], o->mvp[2], o->mvp[3], o->mvp[4], o->mvp[5], o->mvp[6], o->mvp[7],
                       o->mvp[8], o->mvp[9], o->mvp[10], o->mvp[11], o->mvp[12], o->mvp[13], o->mvp[14], o->mvp[15],
                       o->pref[0][0], o->pref[0][1], o->pref[0][2], o->pref[0][3], o->pref[1][0], o->pref[1][1], o->pref[1][2], o->pref[1][3],
                       o->pref[3][0], o->pref[3][1], o->pref[3][2], o->pref[3][3]);
                { /* DIAG(VR-FIX2,W164): Tiefenzustand des Draws (Relief-Richtung, Decals) */
                    D3D11_DEPTH_STENCIL_DESC dd;
                    memset(&dd, 0, sizeof dd);
                    if (o->ds) { ID3D11DepthStencilState_GetDesc(o->ds, &dd); }
                    HLOG_I("VR op2 i=%u kind=%d tiefe=%d schreiben=%d func=%d dsv=%d zn=[%.5f %.5f %.5f %.5f] ndc0=(%.3f,%.3f) ndc3=(%.3f,%.3f) w=[%.3f %.3f %.3f %.3f]",
                           i, vrkind, (int)dd.DepthEnable, (int)dd.DepthWriteMask, (int)dd.DepthFunc, o->dsv ? 1 : 0,
                           o->pref[0][3] != 0.0f ? o->pref[0][2] / o->pref[0][3] : 0.0f, o->pref[1][3] != 0.0f ? o->pref[1][2] / o->pref[1][3] : 0.0f,
                           o->pref[2][3] != 0.0f ? o->pref[2][2] / o->pref[2][3] : 0.0f, o->pref[3][3] != 0.0f ? o->pref[3][2] / o->pref[3][3] : 0.0f,
                           o->rn[0][0], o->rn[0][1], o->rn[3][0], o->rn[3][1], o->rn[0][2], o->rn[1][2], o->rn[2][2], o->rn[3][2]);
                }
            }
        }
        if (o->rq && x->c1 && !(vrbb && (vrkind != 1 || vrp->noshade))) { /* W183: vr_shading=0 -> IM-PS-Ersatz */
            /* RENDER-QA (W77): Shading-Draw mit seinem Zustand (Stream 0 per
             * Versatz, Stream 2, Konstanten-Fenster, Stufen 0..3, Sampler). */
            const rq_rec_t *r = &o->rqr;
            ID3D11ShaderResourceView *sv[4];
            UINT st2 = stride, o0 = o->vstart * stride;
            sv[0] = o->sh_srv; sv[1] = o->rq_shsrv[1]; sv[2] = o->rq_shsrv[2]; sv[3] = o->rq_shsrv[3];
            ID3D11DeviceContext_PSSetShader(x->c, r->ps, NULL, 0);
            ID3D11DeviceContext1_VSSetConstantBuffers1(x->c1, 3, 1, (ID3D11Buffer *const *)&r->cbuf, &r->vfirst, &r->vnum);
            ID3D11DeviceContext1_PSSetConstantBuffers1(x->c1, 4, 1, (ID3D11Buffer *const *)&r->cbuf, &r->pfirst, &r->pnum);
            ID3D11DeviceContext_PSSetShaderResources(x->c, 0, 4, sv);
            ID3D11DeviceContext_PSSetSamplers(x->c, 0, 4, (ID3D11SamplerState *const *)r->smp);
            if (r->xsrv) { ID3D11DeviceContext_VSSetShaderResources(x->c, 0, 1, (ID3D11ShaderResourceView *const *)&r->xsrv); } /* SHADE-PERF (W80) */
            else { ID3D11DeviceContext_IASetVertexBuffers(x->c, 2, 1, (ID3D11Buffer *const *)&r->xbuf, &r->xstride, &r->xoff); }
            if (lerp) {
                const ip_op_t *a = &A->ops[o->match];
                ID3D11Buffer *cbs2[2];
                UINT o1 = a->vstart * stride;
                ip_cb_write(x, x->cb1, a->mvp, 0, t, 1);
                ID3D11DeviceContext_IASetInputLayout(x->c, r->layl);
                ID3D11DeviceContext_VSSetShader(x->c, r->vsl, NULL, 0);
                cbs2[0] = x->cb0; cbs2[1] = x->cb1;
                ID3D11DeviceContext_VSSetConstantBuffers(x->c, 0, 2, cbs2);
                ID3D11DeviceContext_IASetVertexBuffers(x->c, 0, 1, &B->vb, &st2, &o0);
                ID3D11DeviceContext_IASetVertexBuffers(x->c, 1, 1, &A->vb, &st2, &o1);
            } else {
                ID3D11DeviceContext_IASetInputLayout(x->c, r->lay);
                ID3D11DeviceContext_VSSetShader(x->c, r->vs, NULL, 0);
                ID3D11DeviceContext_VSSetConstantBuffers(x->c, 0, 1, &x->cb0);
                ID3D11DeviceContext_IASetVertexBuffers(x->c, 0, 1, &B->vb, &st2, &o0);
            }
            if (vrbb) { /* W134 VR-PROTO: Shading-Draw 3D-Pfad je Auge (lineare Augen-Matrix in b0/b1) */
                int e;
                for (e = 0; e < vrp->neyes && !(g_cfg.vr_diag_skip & 2); e++) { /* DIAG(W164): vr_diag_skip Bit 1 = 3D */
                    float m[16];
                    ip_vr_eye_target(x, vrp, e, o);
                    ip_vr_mat3d(m, o->mvp, vrp, e);
                    ip_cb_write(x, x->cb0, m, o->has_tex, 0.0f, 0);
                    if (lerp) { ip_vr_mat3d(m, A->ops[o->match].mvp, vrp, e); ip_cb_write(x, x->cb1, m, 0, t, 1); }
                    ID3D11DeviceContext_Draw(x->c, o->nv, 0);
                }
                crtv = (ID3D11RenderTargetView *)1; cdsv = (ID3D11DepthStencilView *)1;
                memset(&cvp, 0xFF, sizeof cvp); memset(&csc, 0xFF, sizeof csc);
            } else {
                WD_CALL("ip_draw_rq", x->c, ID3D11DeviceContext_Draw(x->c, o->nv, 0)); /* DIAG(PERF-WARM,W139) */
            }
            mode = -1;      /* naechster alter Draw bindet VS/Layout/Streams neu */
            rq_dirty = 1;   /* ... und PS/Sampler/Stufen */
            fxs.bound = 0; fxs.dirty = 1; /* W133: PS ersetzt, Tiefe evtl. geschrieben */
            csmp = (ID3D11SamplerState *)1;
            csrv = (ID3D11ShaderResourceView *)1;
            continue;
        }
        if (rq_dirty) {
            ID3D11ShaderResourceView *nul[3] = { NULL, NULL, NULL };
            ID3D11SamplerState *ds = rq_default_sampler();
            ID3D11DeviceContext_PSSetShader(x->c, (dbg && cps == 1) ? ip_dbg_ps : g_im_ps, NULL, 0);
            ID3D11DeviceContext_PSSetConstantBuffers(x->c, 0, 1, &x->cb0);
            ID3D11DeviceContext_PSSetShaderResources(x->c, 1, 3, nul);
            if (ds) { ID3D11DeviceContext_PSSetSamplers(x->c, 0, 1, &ds); }
            ID3D11DeviceContext_PSSetShaderResources(x->c, 0, 1, &o->sh_srv);
            csrv = o->sh_srv;
            rq_dirty = 0;
            csmp = (ID3D11SamplerState *)1;
        }
        if (!dbg && ((o->has_tex & FXR_MASK) || fxs.bound)) { /* W133 REM-FX: Effekt-PS + Tiefen-Kopie des Schatten-Ziels */
            fxr_rep_op(x->c, &fxs, o->has_tex, o->fxp, o->sh_dsv, x->deferred);
        } else if (o->kind == IP_DRAW) { fxs.dirty = 1; }
        if (g_cfg.tex_addr || o->ta_smp || /* FIX(TEXADDR,W84): s0 wie beim Spiel-Draw; MONITOR-FX (W105): Wash-WRAP auch bei tex_addr=0 */
            (csmp != (ID3D11SamplerState *)1 && csmp != sb_ta_sampler(0, 0))) {
            ID3D11SamplerState *ws = o->ta_smp ? o->ta_smp : sb_ta_sampler(0, 0);
            if (ws && ws != csmp) { ID3D11DeviceContext_PSSetSamplers(x->c, 0, 1, &ws); csmp = ws; }
        }
        if (lerp) {
            const ip_op_t *a = &A->ops[o->match];
            ID3D11Buffer *vbs[2];
            UINT strides[2], offs[2];
            ip_cb_write(x, x->cb1, a->mvp, 0, t, 1);
            if (mode != 1) {
                ID3D11Buffer *cbs2[2];
                ID3D11DeviceContext_IASetInputLayout(x->c, ip_lay2);
                ID3D11DeviceContext_VSSetShader(x->c, ip_vs2, NULL, 0);
                cbs2[0] = x->cb0;
                cbs2[1] = x->cb1;
                ID3D11DeviceContext_VSSetConstantBuffers(x->c, 0, 2, cbs2);
                mode = 1;
            }
            vbs[0] = B->vb; vbs[1] = A->vb;
            strides[0] = stride; strides[1] = stride;
            offs[0] = o->vstart * stride; offs[1] = a->vstart * stride;
            ID3D11DeviceContext_IASetVertexBuffers(x->c, 0, 2, vbs, strides, offs);
            if (vrbb) { /* W134 VR-PROTO */
                ip_vr_draw(x, vrp, o, a, vrkind, t, 1, &mode);
                crtv = (ID3D11RenderTargetView *)1; cdsv = (ID3D11DepthStencilView *)1;
                memset(&cvp, 0xFF, sizeof cvp); memset(&csc, 0xFF, sizeof csc);
            } else {
                WD_CALL("ip_draw_lerp", x->c, ID3D11DeviceContext_Draw(x->c, o->nv, 0));
            }
        } else {
            if (mode != 0) {
                ID3D11Buffer *nb = NULL;
                ID3D11DeviceContext_IASetInputLayout(x->c, g_im_layout);
                ID3D11DeviceContext_VSSetShader(x->c, g_im_vs, NULL, 0);
                ID3D11DeviceContext_VSSetConstantBuffers(x->c, 0, 1, &x->cb0);
                ID3D11DeviceContext_IASetVertexBuffers(x->c, 0, 1, &B->vb, &stride, &zero);
                ID3D11DeviceContext_IASetVertexBuffers(x->c, 1, 1, &nb, &stride, &zero);
                mode = 0;
            }
            if (vrbb) { /* W134 VR-PROTO */
                ip_vr_draw(x, vrp, o, NULL, vrkind, t, 0, &mode);
                crtv = (ID3D11RenderTargetView *)1; cdsv = (ID3D11DepthStencilView *)1;
                memset(&cvp, 0xFF, sizeof cvp); memset(&csc, 0xFF, sizeof csc);
            } else {
                WD_CALL("ip_draw", x->c, ID3D11DeviceContext_Draw(x->c, o->nv, o->vstart));
            }
        }
    }
    if ((ccm & 3) == 1) { cc_store(A, B, x); } /* W146: BB-Anfang = alle BB-Ops */
    if (cps >= 0) {
        ID3D11Buffer *nb = NULL;
        ID3D11DeviceContext_PSSetConstantBuffers(x->c, 1, 1, &nb);
    }
    fxr_rep_end(x->c, &fxs); /* W133 REM-FX */
    if (!x->deferred) {
        ip_state_restore(&sv);
        ip_gs_end(B->nops); /* DIAG(TICK-PERF,W125) */
    }
}

/* ---------- DIAG (interp_diag>=3): Draw-Analyse eines Bildfolgen-Ticks ---------- */

static const char *kIP_WHY[IP_W_N] = { "-", "static", "lerp", "rawlerp", "nomatch",
                                       "rawchg", "reject", "bb2d", "rt2dlerp", "rt2dex" };

/* Pixel-Huellrechteck der Vertices [vs, vs+n) einer Liste (mvp, Viewport vp).
 * Rueckgabe: Zahl Vertices mit w <= 0. */
static unsigned ip_bbox(float (*P)[4], unsigned vs, unsigned n, const float m[16],
                        const D3D11_VIEWPORT *vp, float bb[4])
{
    unsigned i, behind = 0;
    bb[0] = bb[1] = 1e30f;
    bb[2] = bb[3] = -1e30f;
    for (i = 0; i < n; i++) {
        float c[4], x, y;
        ip_clip(P[vs + i], m, c);
        if (c[3] <= 1e-6f) { behind++; continue; }
        x = vp->TopLeftX + (c[0] / c[3] * 0.5f + 0.5f) * vp->Width;
        y = vp->TopLeftY + (0.5f - c[1] / c[3] * 0.5f) * vp->Height;
        if (x < bb[0]) { bb[0] = x; }
        if (y < bb[1]) { bb[1] = y; }
        if (x > bb[2]) { bb[2] = x; }
        if (y > bb[3]) { bb[3] = y; }
    }
    return behind;
}

static int ip_sh_of_rtv(ID3D11RenderTargetView *v)
{
    int e;
    if (!v) { return -1; }
    for (e = 0; e < IP_SH_MAX; e++) {
        if (g_ish[e].rtv == v) { return e; }
    }
    return -1;
}

static void ip_diag_tick(ip_list_t *A, int ai, ip_list_t *B, int bi)
{
    static unsigned ndone;
    unsigned i, cnt[IP_W_N];
    float (*PA)[4] = ip_cpos[ai], (*PB)[4] = ip_cpos[bi];
    if (!PA || !PB || ndone >= 48u) { return; }
    ndone++;
    memset(cnt, 0, sizeof cnt);
    for (i = 0; i < B->nops; i++) {
        const ip_op_t *o = &B->ops[i];
        float bb[4], ba[4];
        unsigned beh, behA = 0;
        char extra[200];
        if (o->kind != IP_DRAW) { continue; }
        cnt[o->why < IP_W_N ? o->why : 0]++;
        if (o->why == IP_W_BB2D) { continue; }
        beh = ip_bbox(PB, o->vstart, o->nv, o->mvp, &o->vp, bb);
        extra[0] = 0;
        ba[0] = ba[1] = ba[2] = ba[3] = 0;
        if (o->match >= 0 && (unsigned)o->match < A->nops) {
            const ip_op_t *a = &A->ops[o->match];
            behA = ip_bbox(PA, a->vstart, a->nv, a->mvp, &a->vp, ba);
            if (a->nv == o->nv && (o->why == IP_W_RAWCHG || o->why == IP_W_RAWLERP ||
                                   o->why == IP_W_RT2DLERP || o->why == IP_W_RT2DEX ||
                                   o->why == IP_W_REJECT || o->why == IP_W_LERP)) {
                /* Vertex i <-> i: Eingangs-Differenz (Mittel, max. Abweichung) und
                 * Bild-Sprung je Vertex (max / Anteil > 8 px). */
                double dm[3] = { 0, 0, 0 }, dev = 0, jmax = 0;
                unsigned k, nbig = 0, nj = 0;
                for (k = 0; k < o->nv; k++) {
                    int c;
                    for (c = 0; c < 3; c++) { dm[c] += PB[o->vstart + k][c] - PA[a->vstart + k][c]; }
                }
                for (k = 0; k < 3; k++) { dm[k] /= (double)o->nv; }
                for (k = 0; k < o->nv; k++) {
                    float c0[4], c1[4];
                    int c;
                    for (c = 0; c < 3; c++) {
                        double d = PB[o->vstart + k][c] - PA[a->vstart + k][c] - dm[c];
                        if (d < 0) { d = -d; }
                        if (d > dev) { dev = d; }
                    }
                    ip_clip(PA[a->vstart + k], a->mvp, c0);
                    ip_clip(PB[o->vstart + k], o->mvp, c1);
                    if (c0[3] > 1e-6f && c1[3] > 1e-6f) {
                        double dx = (c1[0] / c1[3] - c0[0] / c0[3]) * 0.5 * o->vp.Width;
                        double dy = (c1[1] / c1[3] - c0[1] / c0[3]) * 0.5 * o->vp.Height;
                        double j = sqrt(dx * dx + dy * dy);
                        if (j > jmax) { jmax = j; }
                        if (j > 8.0) { nbig++; }
                        nj++;
                    }
                }
                _snprintf_s(extra, sizeof extra, _TRUNCATE,
                            " dmean=%.3f,%.3f,%.3f dev=%.3f jmax=%.1f big=%u/%u",
                            dm[0], dm[1], dm[2], dev, jmax, nbig, nj);
            }
        } else if (o->why == IP_W_NOMATCH) {
            /* Kandidat gleicher Signatur ohne Vertexzahl im Vortick */
            unsigned j;
            for (j = 0; j < A->nops; j++) {
                const ip_op_t *a = &A->ops[j];
                if (a->kind == IP_DRAW && a->sig0 == o->sig0 && !a->taken) {
                    behA = ip_bbox(PA, a->vstart, a->nv, a->mvp, &a->vp, ba);
                    _snprintf_s(extra, sizeof extra, _TRUNCATE, " cand=%u n0=%u raw0=%s", j, a->nv,
                                a->rawhash == o->rawhash ? "gleich" : "neu");
                    break;
                }
            }
            if (!extra[0]) {
                /* gleiches Ziel + Vertexzahl + Eingangsdaten: welcher Signatur-Teil wechselt? */
                for (j = 0; j < A->nops; j++) {
                    const ip_op_t *a = &A->ops[j];
                    if (a->kind == IP_DRAW && a->rtv == o->rtv && a->nv == o->nv &&
                        a->rawhash == o->rawhash) {
                        behA = ip_bbox(PA, a->vstart, a->nv, a->mvp, &a->vp, ba);
                        _snprintf_s(extra, sizeof extra, _TRUNCATE,
                                    " same=%u tk=%u vs=%X/%X xp=%X/%X ps=%X/%X tex=%u/%u topo=%u/%u cls=%u/%u",
                                    j, a->taken, a->pvs, o->pvs, a->pxp, o->pxp, a->pps, o->pps,
                                    a->has_tex, o->has_tex, a->topo, o->topo, a->cls, o->cls);
                        break;
                    }
                }
            }
        }
        HLOG_I("INTERP dd f=%u i=%u w=%s e=%d vp=%.0fx%.0f cls=%u n=%u m=%d bb=%.0f,%.0f,%.0f,%.0f beh=%u bbA=%.0f,%.0f,%.0f,%.0f behA=%u mvpeq=%d%s",
               B->frame, i, kIP_WHY[o->why < IP_W_N ? o->why : 0], ip_sh_of_rtv(o->sh_rtv),
               o->vp.Width, o->vp.Height, o->cls, o->nv, o->match, bb[0], bb[1], bb[2], bb[3], beh,
               ba[0], ba[1], ba[2], ba[3], behA,
               (o->match >= 0 && (unsigned)o->match < A->nops &&
                memcmp(A->ops[o->match].mvp, o->mvp, sizeof o->mvp) == 0) ? 1 : 0, extra);
    }
    HLOG_I("INTERP ddsum f=%u static=%u lerp=%u rawlerp=%u nomatch=%u rawchg=%u reject=%u bb2d=%u rt2dlerp=%u rt2dex=%u",
           B->frame, cnt[1], cnt[2], cnt[3], cnt[4], cnt[5], cnt[6], cnt[7], cnt[8], cnt[9]);
    for (i = 0; i < IP_SH_MAX; i++) {
        if (g_ish[i].res) {
            D3D11_TEXTURE2D_DESC dd;
            ID3D11Texture2D_GetDesc(g_ish[i].sh, &dd);
            HLOG_I("INTERP ddsh e=%u %ux%u bind=0x%X%s", i, dd.Width, dd.Height, dd.BindFlags,
                   (int)i == B->bbref ? " (BB)" : "");
        }
    }
}

/* ---------- Verifikation (interp_verify) ---------- */

static ID3D11Texture2D *ip_vstg[2];

static int ip_stage(int k, ID3D11Resource *src)
{
    if (!ip_vstg[k]) {
        D3D11_TEXTURE2D_DESC dd;
        ID3D11Texture2D_GetDesc((ID3D11Texture2D *)src, &dd);
        dd.Usage = D3D11_USAGE_STAGING;
        dd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        dd.BindFlags = 0;
        dd.MiscFlags = 0;
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &dd, NULL, &ip_vstg[k]))) {
            ip_vstg[k] = NULL;
            return 0;
        }
    }
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)ip_vstg[k], src);
    return 1;
}

unsigned sb_interp_pairs(void)
{
    return ip_pairs_total;
}

/* W81 WINMODE: vor ResizeBuffers alles freigeben, was Backbuffer-Ressourcen
 * (oder ihre Views) haelt: Aufzeichnungs-/Historienlisten (o->rtv/dsv/srv),
 * Schatten-Eintraege (AddRef auf Spiel-BB/RTs, Kopien in BB-Groesse) und
 * die Verify-Staging-Texturen. Der Takt stoppt; die naechsten zwei Ticks
 * bauen die Historie in der neuen Groesse neu auf. Aufruf unter ctx_lock. */
void sb_interp_reset(const char *why)
{
    int i, nsh = 0;
    if (!ip_init_done) { return; }
    ip_rt_quiesce(); /* W126 RENDER-THREAD: Render-Thread liest Listen/Schatten */
    for (i = 0; i < IP_LISTS; i++) { ip_list_reset(&g_ipl[i]); }
    ip_nhist = 0;
    ip_nneed = 0;
    ip_live = 0;
    ip_rec = -1;
    g_ip_recording = 0;
    ip_shot_ser = 0;
    for (i = 0; i < IP_SH_MAX; i++) {
        if (g_ish[i].res) { nsh++; }
        ip_sh_free(&g_ish[i]);
    }
    for (i = 0; i < 2; i++) {
        if (ip_vstg[i]) { ID3D11Texture2D_Release(ip_vstg[i]); ip_vstg[i] = NULL; }
    }
    HLOG_I("INTERP: Reset (%s): Listen, %d Schatten, Staging freigegeben", why ? why : "", nsh);
}

/* Schatten-BB (exakte Wiedergabe) gegen gesicherten Spiel-BB vergleichen. */
static void ip_verify_check(ip_list_t *B, int bi)
{
    D3D11_MAPPED_SUBRESOURCE m0, m1;
    unsigned x, y, ndiff = 0, maxd = 0, x0 = 0xFFFFFFFFu, y0 = 0xFFFFFFFFu, x1 = 0, y1 = 0;
    ip_sh_t *e;
    if (B->bbref < 0) { return; }
    e = &g_ish[B->bbref];
    if (!ip_stage(0, (ID3D11Resource *)e->sh) || !ip_stage(1, (ID3D11Resource *)e->bak[bi])) { return; }
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)ip_vstg[0], 0, D3D11_MAP_READ, 0, &m0))) {
        return;
    }
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)ip_vstg[1], 0, D3D11_MAP_READ, 0, &m1))) {
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)ip_vstg[0], 0);
        return;
    }
    for (y = 0; y < g_bb_h; y++) {
        const unsigned char *a = (const unsigned char *)m0.pData + (size_t)y * m0.RowPitch;
        const unsigned char *b = (const unsigned char *)m1.pData + (size_t)y * m1.RowPitch;
        if (memcmp(a, b, (size_t)g_bb_w * 4u) == 0) { continue; }
        for (x = 0; x < g_bb_w; x++) {
            unsigned c, d = 0;
            for (c = 0; c < 3u; c++) {
                unsigned dc = a[x * 4u + c] > b[x * 4u + c] ? a[x * 4u + c] - b[x * 4u + c]
                                                             : b[x * 4u + c] - a[x * 4u + c];
                if (dc > d) { d = dc; }
            }
            if (d) {
                ndiff++;
                if (d > maxd) { maxd = d; }
                if (x < x0) { x0 = x; }
                if (y < y0) { y0 = y; }
                if (x > x1) { x1 = x; }
                if (y > y1) { y1 = y; }
            }
        }
    }
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)ip_vstg[1], 0);
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)ip_vstg[0], 0);
    st_verify++;
    if (ndiff) { st_verify_bad++; }
    HLOG_I("INTERP verify f=%u: diff=%u px max=%u box=%u,%u-%u,%u (exakte Wiedergabe vs. Spiel-Bild)",
           B->frame, ndiff, maxd, ndiff ? x0 : 0, ndiff ? y0 : 0, x1, y1);
}

/* ---------- GPU-Zeit einer Wiedergabe (interp_diag, Timestamp-Queries) ---------- */
static ID3D11Query *ip_qd, *ip_qt[2];
static int ip_q_pend;

static int ip_gpu_begin(void)
{
    D3D11_QUERY_DESC qd;
    if (g_cfg.interp_diag < 1 || ip_q_pend || (st_presents % 37u) != 0u) { return 0; }
    if (!ip_qd) {
        memset(&qd, 0, sizeof qd);
        qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        if (FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &ip_qd))) { ip_qd = NULL; return 0; }
        qd.Query = D3D11_QUERY_TIMESTAMP;
        if (FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &ip_qt[0])) ||
            FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &ip_qt[1]))) { return 0; }
    }
    ID3D11DeviceContext_Begin(g_ctx, (ID3D11Asynchronous *)ip_qd);
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)ip_qt[0]);
    return 1;
}

static void ip_gpu_end(void)
{
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)ip_qt[1]);
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)ip_qd);
    ip_q_pend = 1;
}

static void ip_gpu_poll(void)
{
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
    UINT64 t0, t1;
    if (!ip_q_pend) { return; }
    if (ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)ip_qd, &dj, sizeof dj,
                                    D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) { return; }
    ip_q_pend = 0;
    if (dj.Disjoint || !dj.Frequency) { return; }
    if (ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)ip_qt[0], &t0, sizeof t0, 0) != S_OK ||
        ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)ip_qt[1], &t1, sizeof t1, 0) != S_OK) {
        return;
    }
    {
        double ms = (double)(t1 - t0) * 1000.0 / (double)dj.Frequency;
        st_gpu_ms += ms;
        st_gpu_n++;
        if (ms > st_gpu_max) { st_gpu_max = ms; }
    }
}

/* ---------- Bilder zeigen ---------- */

static int ip_list_index(const ip_list_t *L)
{
    return (int)(L - g_ipl);
}

/* ---------- PERF(RENDER-THREAD,W126): Wiedergabe auf eigenem Thread ----------
 * render_thread=1: Ein Host-Thread (kein Xbox-Thread, haelt keine Host-Sperre)
 * zeichnet die Zwischenbild-Wiedergaben in einen D3D11-Deferred-Context auf
 * (ip_replay_ex mit eigenem Context und eigenen Konstantenpuffern). Der
 * Spiel-Thread fuehrt bei Faelligkeit nur ExecuteCommandList aus (Blit,
 * Present, Effekte wie bisher). Ein Auftrags-Platz: Nach jedem gezeigten Bild
 * und jedem Swap stellt der Spiel-Thread das naechste faellige Paar (A, B, t)
 * vor (ip_rt_prefetch). Passt es bei Faelligkeit nicht (Swap dazwischen,
 * anderes t), wird es verworfen und ueber denselben Pfad synchron erzeugt
 * (gleiche Bilder in jedem Fall). Die Listen eines offenen Auftrags sind
 * gepinnt: Historien-Abwurf, Reset und Schatten-Verdraengung warten, bis der
 * Render-Thread fertig ist (ip_rt_drop/ip_rt_quiesce). Den Immediate-Context
 * benutzt weiterhin nur der Spiel-Thread. */
#define IP_RT_FREE 0
#define IP_RT_REQ 1
#define IP_RT_DONE 2
typedef struct {
    volatile LONG st;
    int ai, bi;
    unsigned aser, bser;
    float t;
    ID3D11CommandList *cl;
    LONGLONG q0, q1; /* Aufzeichnung Start/Ende (QPC, Render-Thread) */
} ip_rtjob_t;
static ip_rtjob_t ip_rtj;
static HANDLE ip_rt_th, ip_rt_ev;
static DWORD ip_rt_tid;
static ip_cx_t ip_rt_cx;
static int ip_rt_ok, ip_rt_tried;
static unsigned st_rt_hit, st_rt_miss, st_rt_wait, st_rt_stale, st_rt_n, st_rt_fail, st_rt_exec;
static double st_rt_ms, st_rt_max, st_rt_wait_ms, st_rt_exec_ms;
static ip_cx_t ip_cx_imm_v;
static ip_list_t *ip_rt_nA, *ip_rt_nB; /* Testtakt: Folgebild (A, B, t) fuer ip_rt_get */
static float ip_rt_nt;

static const ip_cx_t *ip_cx_imm(void)
{
    ip_cx_imm_v.c = g_ctx;
    ip_cx_imm_v.c1 = g_rq_ctx1;
    ip_cx_imm_v.cb0 = ip_cb0;
    ip_cx_imm_v.cb1 = ip_cb1;
    ip_cx_imm_v.deferred = 0;
    return &ip_cx_imm_v;
}

static DWORD WINAPI ip_rt_main(LPVOID p)
{
    (void)p;
    for (;;) {
        WaitForSingleObject(ip_rt_ev, INFINITE);
        if (ip_rtj.st == IP_RT_REQ) {
            ip_rtjob_t *j = &ip_rtj;
            ID3D11CommandList *cl = NULL;
            j->q0 = ip_now();
            ip_replay_ex(&g_ipl[j->bi], j->bi, j->ai >= 0 ? &g_ipl[j->ai] : NULL, j->ai, j->t, 0, &ip_rt_cx);
            if (FAILED(ID3D11DeviceContext_FinishCommandList(ip_rt_cx.c, FALSE, &cl))) { cl = NULL; }
            j->cl = cl;
            j->q1 = ip_now();
            InterlockedExchange(&j->st, IP_RT_DONE);
        }
    }
}

static int ip_rt_start(void)
{
    D3D11_BUFFER_DESC bd;
    D3D11_FEATURE_DATA_THREADING th;
    ID3D11DeviceContext *dc = NULL;
    if (ip_rt_tried) { return ip_rt_ok; }
    ip_rt_tried = 1;
    if (!g_dev || !ip_cb0) { return 0; }
    if (FAILED(ID3D11Device_CreateDeferredContext(g_dev, 0, &dc)) || !dc) {
        HLOG_W("INTERP rt: CreateDeferredContext fehlgeschlagen -> Wiedergabe im Spiel-Thread");
        return 0;
    }
    memset(&ip_rt_cx, 0, sizeof ip_rt_cx);
    ip_rt_cx.c = dc;
    if (g_rq_ctx1 && FAILED(ID3D11DeviceContext_QueryInterface(dc, &IID_ID3D11DeviceContext1, (void **)&ip_rt_cx.c1))) {
        ip_rt_cx.c1 = NULL;
        HLOG_W("INTERP rt: Deferred-Context ohne ID3D11DeviceContext1 -> Wiedergabe im Spiel-Thread");
        return 0;
    }
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = 80;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &ip_rt_cx.cb0)) ||
        FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &ip_rt_cx.cb1))) {
        HLOG_W("INTERP rt: Konstantenpuffer fehlgeschlagen -> Wiedergabe im Spiel-Thread");
        return 0;
    }
    ip_rt_cx.deferred = 1;
    (void)sb_ta_sampler(0, 0); /* Cache vorwaermen: der Render-Thread liest ihn nur */
    ip_rt_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!ip_rt_ev) { return 0; }
    ip_rt_th = CreateThread(NULL, 0, ip_rt_main, NULL, 0, &ip_rt_tid);
    if (!ip_rt_th) { HLOG_W("INTERP rt: CreateThread fehlgeschlagen (%lu)", GetLastError()); return 0; }
    if (g_cfg.render_thread_core >= 0 && g_cfg.render_thread_core < 32) {
        SetThreadAffinityMask(ip_rt_th, (DWORD_PTR)1 << g_cfg.render_thread_core);
    }
    memset(&th, 0, sizeof th);
    ID3D11Device_CheckFeatureSupport(g_dev, D3D11_FEATURE_THREADING, &th, sizeof th);
    ip_rt_ok = 1;
    HLOG_I("INTERP rt: Render-Thread T%04lx bereit (Deferred-Context, Kern %d, DriverCommandLists=%d)",
           ip_rt_tid, g_cfg.render_thread_core, th.DriverCommandLists ? 1 : 0);
    return 1;
}

/* Render-Thread fuer diesen Lauf aktiv? Diagnosen, die den Immediate-Context
 * waehrend der Wiedergabe brauchen (perf_gsplit, interp_diag>=3), behalten den
 * alten Pfad. */
static int ip_rt_active(void)
{
    if (g_cfg.render_thread <= 0 || g_cfg.interp_diag >= 3 || g_cfg.perf_gsplit > 0) { return 0; }
    return ip_rt_start();
}

static void ip_rt_wait(void)
{
    while (ip_rtj.st == IP_RT_REQ) {
        int k;
        for (k = 0; k < 64 && ip_rtj.st == IP_RT_REQ; k++) { YieldProcessor(); }
        if (ip_rtj.st == IP_RT_REQ) { SwitchToThread(); }
    }
}

/* Fertigen Auftrag abholen (Statistik), Platz freigeben; Command-List an den Aufrufer. */
static ID3D11CommandList *ip_rt_collect(void)
{
    ID3D11CommandList *cl = ip_rtj.cl;
    double ms = ip_ms(ip_rtj.q0, ip_rtj.q1);
    ip_rtj.cl = NULL;
    st_rt_n++;
    st_rt_ms += ms;
    if (ms > st_rt_max) { st_rt_max = ms; }
    if (!cl) { st_rt_fail++; }
    InterlockedExchange(&ip_rtj.st, IP_RT_FREE);
    return cl;
}

/* Offenen Auftrag abwarten und verwerfen (vor Reset/Verdraengung). */
static void ip_rt_quiesce(void)
{
    ID3D11CommandList *cl;
    if (ip_rtj.st == IP_RT_FREE) { return; }
    ip_rt_wait();
    st_rt_stale++;
    cl = ip_rt_collect();
    if (cl) { ID3D11CommandList_Release(cl); }
}

/* Liste li verlaesst die Historie: offenen Auftrag mit li verwerfen. */
static void ip_rt_drop(int li)
{
    if (ip_rtj.st != IP_RT_FREE && (ip_rtj.ai == li || ip_rtj.bi == li)) { ip_rt_quiesce(); }
}

static int ip_rt_match(int ai, int bi, unsigned aser, unsigned bser, float t)
{
    return ip_rtj.ai == ai && ip_rtj.bi == bi && ip_rtj.aser == aser && ip_rtj.bser == bser &&
           memcmp(&ip_rtj.t, &t, sizeof t) == 0;
}

static void ip_rt_post(int ai, int bi, unsigned aser, unsigned bser, float t)
{
    ip_rtj.ai = ai;
    ip_rtj.bi = bi;
    ip_rtj.aser = aser;
    ip_rtj.bser = bser;
    ip_rtj.t = t;
    ip_rtj.cl = NULL;
    InterlockedExchange(&ip_rtj.st, IP_RT_REQ);
    SetEvent(ip_rt_ev);
}

/* Command-List der Wiedergabe (A, B, t): vorbereiteter Auftrag (ggf. abwarten)
 * oder neu ueber den Render-Thread (synchron). NULL = Fehlschlag. */
static ID3D11CommandList *ip_rt_get(ip_list_t *A, ip_list_t *B, float t)
{
    int ai = A ? ip_list_index(A) : -1, bi = ip_list_index(B);
    unsigned aser = A ? A->serial : 0u, bser = B->serial;
    LONGLONG q0 = ip_now();
    if (ip_rtj.st != IP_RT_FREE && ip_rt_match(ai, bi, aser, bser, t)) {
        if (ip_rtj.st == IP_RT_REQ) { st_rt_wait++; ip_rt_wait(); }
        st_rt_hit++;
    } else {
        ip_rt_quiesce();
        ip_rt_post(ai, bi, aser, bser, t);
        ip_rt_wait();
        st_rt_miss++;
    }
    st_rt_wait_ms += ip_ms(q0, ip_now());
    {
        ID3D11CommandList *cl = ip_rt_collect();
        if (ip_rt_nA) { /* Testtakt: naechstes Bild sofort vorbestellen (Aufzeichnung waehrend Ausfuehrung/Present) */
            ip_rt_post(ip_list_index(ip_rt_nA), ip_list_index(ip_rt_nB), ip_rt_nA->serial, ip_rt_nB->serial, ip_rt_nt);
            ip_rt_nA = NULL;
        }
        return cl;
    }
}

static void ip_pair_at(LONGLONG due, ip_list_t **pA, ip_list_t **pB, float *pt, int *phold);

/* Naechstes faelliges Zwischenbild vorbereiten (nach Bild bzw. Swap). */
static void ip_rt_prefetch(void)
{
    ip_list_t *A, *B;
    float t;
    int hold, ai, bi;
    if (!ip_rt_ok || g_cfg.render_thread <= 0 || !ip_live || ip_nhist < 1) { return; }
    ip_pair_at(ip_tnext, &A, &B, &t, &hold);
    if (!A || hold || t >= 1.0f) { return; }
    ai = ip_list_index(A);
    bi = ip_list_index(B);
    if (ip_rtj.st != IP_RT_FREE) {
        if (ip_rt_match(ai, bi, A->serial, B->serial, t)) { return; }
        if (ip_rtj.st == IP_RT_REQ) { return; } /* laeuft noch: nicht abbrechbar */
        ip_rt_quiesce();
    }
    ip_rt_post(ai, bi, A->serial, B->serial, t);
}

/* Command-List im Spiel-Thread ausfuehren (Immediate-Zustand bleibt). */
static void ip_rt_exec(ID3D11CommandList *cl)
{
    LONGLONG q0 = ip_now();
    WD_CALL("ip_exec_cl", NULL, ID3D11DeviceContext_ExecuteCommandList(g_ctx, cl, TRUE));
    ID3D11CommandList_Release(cl);
    st_rt_exec++;
    st_rt_exec_ms += ip_ms(q0, ip_now());
}

/* Bild zwischen A und B zeigen: t >= 1 (oder ohne A) -> gesichertes Spiel-Bild
 * von B; sonst Wiedergabe in die Schatten. tag = Name fuer Bildfolgen. */
static void ip_show(ip_list_t *A, ip_list_t *B, float t, const char *tag)
{
    int bi = ip_list_index(B), ai = A ? ip_list_index(A) : -1;
    LONGLONG t0 = ip_now(), t1, tph = t0; /* tph: W122 PERF-HUD */
    ID3D11Resource *img;
    double ms;
    int gpm = 0; /* DIAG(GPU-COST,W146) perf_gpass: Messung dieses Bildes */
    if (B->bbref < 0) { return; }
    if (g_cfg.vr_enable && sb_vr_preview_active() && sb_vr_preview_show(A, B, t)) {
        /* W134 VR-PROTO: Desktop-Vorschau (beide Augen bzw. ein Auge aus derselben Aufzeichnung) */
        t1 = ip_now();
        ms = ip_ms(t0, t1);
        st_rep_ms += ms; st_rep_n++;
        if (ms > st_rep_max) { st_rep_max = ms; }
        if (t >= 1.0f || !A) { st_exact++; }
        t0 = ip_now();
        if (GetCurrentThreadId() == g_pd_tid) { g_phud_src = 1; } /* W138 perf_trace */
        sb_render_present_raw();
        g_phud_src = 0;
        ip_ev_mark();
        t1 = ip_now();
        st_pres_ms += ip_ms(t0, t1);
        if (ip_tlast_present && t1 - ip_tlast_present > ip_dt * 3 / 2) { st_late++; }
        ip_tlast_present = t1;
        st_presents++;
        (void)bi; (void)ai; (void)tph;
        return;
    }
    if (t >= 1.0f || !A || (g_cfg.vr_enable && sb_vr_window_mirrors())) {
        /* W183 VR-FIX3: zeigt das Fenster den VR-Spiegel, entfaellt die Mono-Wiedergabe (sb_render_blit
         * kopiert dann nicht, sondern zeigt das Augenbild) */
        img = (ID3D11Resource *)g_ish[B->bbref].bak[bi];
        st_exact++;
    } else {
        int gq;
        ID3D11CommandList *cl = ip_rt_active() ? ip_rt_get(A, B, t) : NULL; /* W126 RENDER-THREAD */
        if (!cl) { gpm = sb_gp_begin(0); } /* DIAG(GPU-COST,W146) */
        dy_begin(); /* W146 rt_scale_dyn */
        gq = ip_gpu_begin();
        if (cl) { ip_rt_exec(cl); }
        else { ip_replay(B, bi, A, ai, t); }
        if (gq) { ip_gpu_end(); }
        img = (ID3D11Resource *)g_ish[B->bbref].sh;
        t1 = ip_now();
        ms = ip_ms(t0, t1);
        st_rep_ms += ms;
        st_rep_n++;
        if (ms > st_rep_max) { st_rep_max = ms; }
    }
    if (tag && ip_shot_ser && B->serial == ip_shot_ser) {
        char nm[64];
        _snprintf_s(nm, sizeof nm, _TRUNCATE, "ip_%06u_%s", B->frame, tag);
        sb_render_shot_name(nm, img);
        if (g_cfg.interp_diag >= 3 && A && t < 1.0f && ip_dbg_ps) {
            /* DIAG: Klassifikationsbild (Farbe je Zuordnungs-Ergebnis) */
            ip_replay_ex(B, bi, A, ai, t, 1, ip_cx_imm());
            _snprintf_s(nm, sizeof nm, _TRUNCATE, "ipc_%06u_%s", B->frame, tag);
            sb_render_shot_name(nm, img);
            ip_replay(B, bi, A, ai, t); /* gezeigtes Bild wiederherstellen */
        }
    }
    sb_render_blit(img);
    if (gpm) { sb_gp_end(); } /* DIAG(GPU-COST,W146) */
    dy_end(); /* W146 rt_scale_dyn */
    sb_pf_note((t >= 1.0f || !A) ? 1.0f : t, B->frame); /* DIAG(POPIN,W91): pf_ring */
    t0 = ip_now();
    if (GetCurrentThreadId() == g_pd_tid) { g_phud_src = 1; } /* W138 perf_trace */
    sb_render_present_raw();
    g_phud_src = 0;
    ip_ev_mark(); /* PERF(TICK-PERF,W125) interp_gpu_q */
    t1 = ip_now();
    st_pres_ms += ip_ms(t0, t1);
    if (ip_tlast_present && t1 - ip_tlast_present > ip_dt * 3 / 2) { st_late++; }
    ip_tlast_present = t1;
    st_presents++;
    if (g_phud_meas && GetCurrentThreadId() != g_pd_tid) { sb_phud_interp_q(ip_now() - tph, ip_in_rec ? 1 : (g_phud_in_wait ? 2 : 0)); } /* W122 PERF-HUD: Interp-CPU (Wiedergabe + Present); W138: nicht aus dem Takt-Thread */
}

/* PERF(TICK-PERF,W125) interp_gpu_q=N: Event-Query nach jedem Present; ein
 * Zwischenbild (Wiedergabe t<1) wird nur eingereiht, wenn die GPU das N-letzte
 * gezeigte Bild fertig hat. Ursache (W125_b2/b3): bei GPU-Last stapeln sich
 * Wiedergaben vor den synchronen Stellen des Spiel-Ticks (Sichtbarkeitstest
 * GetVisibilityTestResult wartet blockierend wie das Original, visq 7-29 ms/
 * Tick) -> Tick-Last > 33 ms, Spiel < 30 Ticks/s. Mit der Grenze verfaellt
 * dann ein Zwischenbild statt dass der Tick wartet. */
#define IP_EV_N 8
static ID3D11Query *ip_ev[IP_EV_N];
static unsigned ip_ev_n;
static double ip_r_last_ms; /* Rechenzeit des letzten Ticks (Feldwechsel -> Swap) */
unsigned g_ip_gskip; /* DIAG(TICK-PERF,W125): wegen GPU-Rueckstau verfallene Zwischenbilder */
unsigned g_ip_gtight; /* DIAG(TICK-PERF,W125): Pruefungen mit verschaerfter Grenze */

static void ip_ev_mark(void)
{
    ID3D11Query **q;
    if (g_cfg.interp_gpu_q <= 0) { return; }
    q = &ip_ev[ip_ev_n % IP_EV_N];
    if (!*q) {
        D3D11_QUERY_DESC qd;
        memset(&qd, 0, sizeof qd);
        qd.Query = D3D11_QUERY_EVENT;
        if (FAILED(ID3D11Device_CreateQuery(g_dev, &qd, q))) { *q = NULL; return; }
    }
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)*q);
    ip_ev_n++;
}

static int ip_gpu_behind(void)
{
    unsigned f = (unsigned)g_cfg.interp_gpu_q;
    BOOL done = FALSE;
    HRESULT hr;
    ID3D11Query *q;
    if (g_cfg.interp_gpu_q <= 0) { return 0; }
    /* interp_gpu_tight=T: war der letzte Tick laenger als T ms, gilt Grenze 1
     * (nur Bilder einreihen, wenn die GPU alles Gezeigte fertig hat). */
    if (f > 1u && g_cfg.interp_gpu_tight > 0 && ip_r_last_ms > (double)g_cfg.interp_gpu_tight) { f = 1u; g_ip_gtight++; }
    if (ip_ev_n < f) { return 0; }
    if (f > IP_EV_N - 1u) { f = IP_EV_N - 1u; }
    q = ip_ev[(ip_ev_n - f) % IP_EV_N];
    if (!q) { return 0; }
    hr = ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)q, &done, sizeof done,
                                     D3D11_ASYNC_GETDATA_DONOTFLUSH);
    return (hr == S_FALSE) ? 1 : 0;
}

/* Paar fuer die Faelligkeit due: tstate(A) <= due - L < tstate(B) (Historie:
 * aelteste zuerst); hold = juengster Tick exakt halten (naechster fehlt). */
static void ip_pair_at(LONGLONG due, ip_list_t **pA, ip_list_t **pB, float *pt, int *phold)
{
    LONGLONG v = due - ip_lat;
    ip_list_t *A = NULL, *B;
    float t = 1.0f;
    int i;
    *phold = 0;
    B = &g_ipl[ip_hist[ip_nhist - 1]];
    if (v >= B->tstate) {
        *phold = 1; /* naechster Tick noch nicht da: juengsten exakt halten */
    } else {
        for (i = ip_nhist - 1; i >= 1; i--) {
            ip_list_t *a = &g_ipl[ip_hist[i - 1]], *b = &g_ipl[ip_hist[i]];
            if (v >= a->tstate) {
                A = a;
                B = b;
                t = (float)((double)(v - a->tstate) / (double)(b->tstate - a->tstate));
                break;
            }
        }
        if (!A) { B = &g_ipl[ip_hist[0]]; t = 1.0f; }
    }
    *pA = A;
    *pB = B;
    *pt = t;
}

/* Echtzeit-Takt: faelliges Bild zeigen (Zustand zur Zeit tau - L). */
static int pd_owns_clock(void); /* W138 PERF-FIX */
static void ip_poll(void)
{
    LONGLONG now;
    ip_list_t *A = NULL, *B = NULL;
    float t = 1.0f;
    int hold;
    char tag[16];
    if (!ip_live || ip_in_poll || ip_nhist < 1) { return; }
    if (pd_owns_clock()) { return; } /* W138 present_decouple: Bilder zeigt nur der Takt-Thread */
    now = ip_now();
    if (now < ip_tnext) { return; }
    ip_in_poll = 1;
    ip_pair_at(ip_tnext, &A, &B, &t, &hold);
    if (hold) { st_hold++; }
    _snprintf_s(tag, sizeof tag, _TRUNCATE, "t%03u", (unsigned)(t * 1000.0f + 0.5f));
    sb_ctx_enter();
    if (A && t < 1.0f && ip_gpu_behind()) { /* PERF(TICK-PERF,W125): GPU hinterher -> Zwischenbild verfaellt */
        sb_ctx_leave();
        g_ip_gskip++; st_skip++; g_ip_skip_tot++;
        ip_tnext += ip_dt;
        now = ip_now();
        while (ip_tnext <= now) { ip_tnext += ip_dt; st_skip++; g_ip_skip_tot++; }
        ip_rt_prefetch(); /* W126 RENDER-THREAD */
        ip_in_poll = 0;
        return;
    }
    ip_show(A, B, t, tag);
    sb_ctx_leave();
    ip_tnext += ip_dt;
    now = ip_now();
    while (ip_tnext <= now) { ip_tnext += ip_dt; st_skip++; g_ip_skip_tot++; }
    ip_rt_prefetch(); /* W126 RENDER-THREAD: naechstes Bild vorbereiten */
    ip_in_poll = 0;
}


/* ---------- PERF(PERF-FIX,W138): Present-Takt-Thread (present_decouple=1) ----------
 * Bisher zeigte nur der Spiel-Thread Bilder: Zwischenbilder aus den Hooks
 * (Feld-Poll, rec_draw/rec_clear, Swap), Menues/Filme/Ladebilder am Present
 * des Spiels. Steckt der Spiel-Thread in Spielcode ohne Hook (Spiellogik
 * ~11 ms jeden 2. Tick, Ladecode 113-120 ms), stand die Ausgabe (W137 §3).
 * Der Takt-Thread (Host-Thread, kein Xbox-Thread, wird nie suspendiert,
 * haelt ausser ctx keine Sperre) wartet mit hochaufloesendem Timer auf die
 * naechste Faelligkeit und zeigt dann unter ctx_lock (Vortritt g_pd_want):
 *   - Interp-Takt aktiv (Mission, Film): das faellige Bild wie bisher ip_poll
 *     (gleiche Paarwahl ip_pair_at, gleiche Wiedergabe, gleicher Blit); der
 *     Spiel-Thread zeigt selbst keine Zwischenbilder mehr.
 *   - sonst: liegt das letzte echte Present laenger als 1/interp_fps + 0,5 ms
 *     zurueck, den Swapchain-BB erneut (Bild unveraendert, nur keine
 *     eingefrorene Ausgabe). Das Spiel-Bild selbst bleibt unberuehrt.
 * Nur Echtzeit (vblank_hz>0) mit Offscreen-BB (interp_fps>0), nicht mit VR.
 * Testtakt (Golden, interp_verify im Testtakt): Thread ohne Wirkung. */
static HANDLE pd_th, pd_timer;
static DWORD pd_tid_l;
static volatile LONG pd_started; /* 0 = nie, 1 = laeuft, 2 = Start fehlgeschlagen */
static LONGLONG pd_deadline;
static unsigned pd_n_ip, pd_n_hold, pd_n_late, pd_n_lockw, pd_n_iter, pd_n_gw, pd_n_gw_ok;
static double pd_lockw_ms, pd_lockw_max, pd_late_max, pd_wake_err_max, pd_gw_ms;
static LONGLONG pd_stat_q;

int sb_pd_on(void)
{
    return pd_started == 1 && g_cfg.present_decouple > 0 && g_cfg.vblank_hz > 0 && !g_cfg.vr_enable &&
           sb_render_offscreen();
}

static int pd_owns_clock(void)
{
    return pd_started == 1 && GetCurrentThreadId() != g_pd_tid && sb_pd_on();
}

static double pd_hold_ms, pd_hold_max; /* DIAG(PERF-TICK,W168) ctx_wait_diag */
static unsigned pd_hold_n05;
static void pd_stat_log(LONGLONG now)
{
    double secs = pd_stat_q ? ip_ms(pd_stat_q, now) / 1000.0 : 0.0;
    if (pd_stat_q && secs < 10.0) { return; }
    if (pd_stat_q) {
        HLOG_I("PRESENT-TAKT: %.1fs zwischenbilder=%u wiederholt=%u spaet(>1ms)=%u max_spaet=%.2fms "
               "sperre_gewartet=%u (%.3fms mittel, max %.2fms) weckfehler_max=%.2fms runden=%u live=%d "
               "gpu_gewartet=%u (danach gezeigt %u, %.2fms mittel)",
               secs, pd_n_ip, pd_n_hold, pd_n_late, pd_late_max, pd_n_lockw,
               pd_n_lockw ? pd_lockw_ms / pd_n_lockw : 0.0, pd_lockw_max, pd_wake_err_max, pd_n_iter, ip_live,
               pd_n_gw, pd_n_gw_ok, pd_n_gw ? pd_gw_ms / pd_n_gw : 0.0);
    }
    if (pd_stat_q && g_cfg.ctx_wait_diag) { /* DIAG(PERF-TICK,W168) */
        extern volatile LONGLONG g_ctxw_q, g_ctxw_tick_max;
        extern volatile LONG g_ctxw_n;
        HLOG_I("CTXWAIT %.1fs: spiel_wartet=%.1fms (n=%ld, max je Tick %.2fms) takt_haelt=%.3fms mittel (max %.2fms, >0.5ms: %u) runden=%u",
               secs, (double)g_ctxw_q * 1000.0 / (double)ip_qpf.QuadPart, (long)g_ctxw_n,
               (double)g_ctxw_tick_max * 1000.0 / (double)ip_qpf.QuadPart,
               pd_n_iter ? pd_hold_ms / pd_n_iter : 0.0, pd_hold_max, pd_hold_n05, pd_n_iter);
        g_ctxw_q = 0; g_ctxw_n = 0; g_ctxw_tick_max = 0;
        pd_hold_ms = pd_hold_max = 0.0; pd_hold_n05 = 0;
    }
    pd_stat_q = now;
    pd_n_ip = pd_n_hold = pd_n_late = pd_n_lockw = pd_n_iter = pd_n_gw = pd_n_gw_ok = 0;
    pd_lockw_ms = pd_lockw_max = pd_late_max = pd_wake_err_max = pd_gw_ms = 0.0;
}

/* Waere das faellige Bild wegen GPU-Rueckstau (interp_gpu_q) verfallen? (unter ctx_lock) */
static int pd_gpu_would_skip(void)
{
    ip_list_t *A, *B;
    float t;
    int hold;
    if (!ip_live || ip_nhist < 1) { return 0; }
    ip_pair_at(ip_tnext, &A, &B, &t, &hold);
    return A && t < 1.0f && ip_gpu_behind();
}

/* ctx_lock mit Vortritt nehmen (Takt-Thread). */
#include "hlock.h" /* W168 PERF-TICK pd_gw_nopri */
extern const hlock_t *sb_ctx_hlock(void);
static void pd_lock(void)
{
    InterlockedExchange(&g_pd_want, 1);
    sb_ctx_enter();
    InterlockedExchange(&g_pd_want, 0);
}

/* Ein Schritt unter ctx_lock; setzt pd_deadline. */
static void pd_step(void)
{
    LONGLONG now = ip_now();
    LONGLONG dt = ip_dt > 0 ? ip_dt : ip_qpf.QuadPart / 120;
    if (!sb_pd_on()) { pd_deadline = now + dt; return; }
    if (ip_live && ip_nhist >= 1) {
        if (now >= ip_tnext) {
            unsigned np = st_presents;
            double late;
            /* GPU hinterher (interp_gpu_q): bis 1/2 Bildperiode ohne Sperre auf die GPU
             * warten, statt das Bild sofort verfallen zu lassen (Luecke 2 x 8,3 ms).
             * Der Spiel-Thread laeuft dabei weiter. Danach entscheidet ip_poll wie bisher. */
            if (pd_gpu_would_skip()) {
                int wp = g_cfg.pd_gpu_wait > 0 ? (g_cfg.pd_gpu_wait > 90 ? 90 : g_cfg.pd_gpu_wait) : 50; /* W165 GPU-COST2 */
                LONGLONG lim = ip_tnext + ip_dt * wp / 100, g0 = ip_now();
                pd_n_gw++;
                while (ip_now() < lim) {
                    LONGLONG w0;
                    sb_ctx_leave();
                    w0 = ip_now();
                    while (ip_now() - w0 < ip_qpf.QuadPart / 4000) { YieldProcessor(); } /* 0,25 ms */
                    /* PERF(PERF-TICK,W168) pd_gw_nopri=1: im GPU-Warten die Sperre ohne Vortritt nehmen - der
                     * Spiel-Thread musste sonst alle 0,25 ms weichen (CTXWAIT: bis 5800 Wartevorgaenge je 10 s) */
                    if (g_cfg.pd_gw_nopri) {
                        /* frei werdende Sperre abwarten (bis 0,5 ms), dann ohne Vortritt nehmen; sonst wie bisher */
                        const hlock_t *hl = sb_ctx_hlock();
                        LONGLONG s0 = ip_now();
                        int got = 0;
                        while (ip_now() - s0 < ip_qpf.QuadPart / 2000) {
                            if (!hlock_busy(hl)) { sb_ctx_enter(); got = 1; break; }
                            YieldProcessor();
                        }
                        if (!got) { pd_lock(); }
                    } else { pd_lock(); }
                    if (!pd_gpu_would_skip()) { pd_n_gw_ok++; break; }
                }
                pd_gw_ms += ip_ms(g0, ip_now());
                now = ip_now();
            }
            late = ip_ms(ip_tnext, now);
            ip_poll();
            if (st_presents != np) {
                pd_n_ip++;
                if (late > 1.0) { pd_n_late++; }
                if (late > pd_late_max) { pd_late_max = late; }
            }
        }
        pd_deadline = ip_tnext;
        return;
    }
    {
        LONGLONG hp = dt + ip_qpf.QuadPart / 2000; /* 1/interp_fps + 0,5 ms */
        if (g_pd_last_q && now - g_pd_last_q >= hp) {
            if (g_cfg.warm_gpu_soft & 4) { /* W165: erst zeigen, dann Last -> sie hat die ganze Bildperiode */
                if (sb_render_present_hold()) { pd_n_hold++; }
                sb_warm_gpu_step(ip_now());
            } else {
                sb_warm_gpu_step(now); /* W139 PERF-WARM warm_gpu: GPU-Takt in der Ladephase halten */
                if (sb_render_present_hold()) { pd_n_hold++; }
            }
            pd_deadline = ip_now() + dt;
        } else {
            pd_deadline = g_pd_last_q ? g_pd_last_q + hp : now + dt;
        }
    }
}

static DWORD WINAPI pd_main(LPVOID p)
{
    HANDLE hs[2];
    (void)p;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    hs[0] = pd_timer;
    hs[1] = pd_wake;
    pd_deadline = ip_now();
    for (;;) {
        LONGLONG now = ip_now(), early = ip_qpf.QuadPart / 2500; /* 0,4 ms vorher wach, Rest spinnen */
        LONGLONG q0;
        double w;
        if (pd_deadline - now > early) {
            LARGE_INTEGER due; /* relativ, 100-ns-Einheiten */
            due.QuadPart = -(LONGLONG)((double)(pd_deadline - early - now) * 1e7 / (double)ip_qpf.QuadPart);
            if (due.QuadPart > -1) { due.QuadPart = -1; }
            if (SetWaitableTimer(pd_timer, &due, 0, NULL, NULL, FALSE)) {
                WaitForMultipleObjects(2, hs, FALSE, 100);
            } else {
                Sleep(1);
            }
        }
        if (WaitForSingleObject(pd_wake, 0) == WAIT_OBJECT_0) { pd_deadline = ip_now(); }
        while ((now = ip_now()) < pd_deadline) { YieldProcessor(); } /* <= 0,4 ms; SwitchToThread gaebe den Kern ganze Zeitscheiben ab */
        w = ip_ms(pd_deadline, now);
        if (w > pd_wake_err_max) { pd_wake_err_max = w; }
        if (!sb_pd_on()) { /* Testtakt / ohne Offscreen-BB / VR: ruhen, keine Sperre nehmen */
            pd_deadline = now + (ip_dt > 0 ? ip_dt : ip_qpf.QuadPart / 120);
            continue;
        }
        q0 = ip_now();
        pd_lock();
        w = ip_ms(q0, ip_now());
        if (w > 0.05) {
            pd_n_lockw++;
            pd_lockw_ms += w;
            if (w > pd_lockw_max) { pd_lockw_max = w; }
        }
        pd_n_iter++;
        q0 = ip_now();
        pd_step();
        sb_ctx_leave();
        if (g_cfg.ctx_wait_diag) { /* DIAG(PERF-TICK,W168): Haltezeit des Takt-Threads */
            double hm = ip_ms(q0, ip_now());
            pd_hold_ms += hm;
            if (hm > pd_hold_max) { pd_hold_max = hm; }
            if (hm > 0.5) { pd_hold_n05++; }
        }
        pd_stat_log(ip_now());
    }
}

void sb_pd_start(void)
{
    if (pd_started || g_cfg.present_decouple <= 0) { return; }
    if (g_cfg.vr_enable) { /* W183 VR-FIX3: im VR-Betrieb ruht der Takt ohnehin (sb_pd_on) -> Thread gar nicht starten */
        if (InterlockedCompareExchange(&pd_started, 2, 0) == 0) {
            HLOG_I("PRESENT-TAKT: aus im VR-Betrieb (Fenster folgt dem Interp-Takt, VR-Thread taktet die Brille)");
        }
        return;
    }
    if (InterlockedCompareExchange(&pd_started, 2, 0) != 0) { return; }
    if (!ip_qpf.QuadPart) { QueryPerformanceFrequency(&ip_qpf); }
    if (ip_dt <= 0) { ip_dt = ip_qpf.QuadPart / (g_cfg.interp_fps > 0 ? g_cfg.interp_fps : 120); }
    /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (0x2, Windows 10 1803+); sonst normal */
    pd_timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002u, TIMER_ALL_ACCESS);
    if (!pd_timer) { pd_timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS); }
    pd_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!pd_timer || !pd_wake) {
        HLOG_W("PRESENT-TAKT: Timer/Event fehlgeschlagen (%lu) -> aus", GetLastError());
        return; /* pd_started bleibt 2: sb_pd_on = 0 */
    }
    pd_th = CreateThread(NULL, 0, pd_main, NULL, CREATE_SUSPENDED, &pd_tid_l);
    if (!pd_th) {
        HLOG_W("PRESENT-TAKT: CreateThread fehlgeschlagen (%lu) -> aus", GetLastError());
        return;
    }
    g_pd_tid = pd_tid_l;
    if (g_cfg.present_core >= 0 && g_cfg.present_core < 32) {
        SetThreadAffinityMask(pd_th, (DWORD_PTR)1 << g_cfg.present_core);
    }
    HLOG_I("PRESENT-TAKT: Thread T%04lx gestartet (Kern %d, Takt %d Hz, Echtzeit=%d, Offscreen=%d)", pd_tid_l,
           g_cfg.present_core, g_cfg.interp_fps, g_cfg.vblank_hz > 0, sb_render_offscreen());
    InterlockedExchange(&pd_started, 1);
    ResumeThread(pd_th);
}

int sb_interp_pending(void)
{
    return ip_live;
}

void sb_interp_field(unsigned counter)
{
    static unsigned last;
    if (counter != last) {
        last = counter;
        ip_vb_qpc = ip_now(); /* Feldwechsel: Spiel-Warten endet hier */
    }
    if (ip_live) { ip_poll(); }
}

void sb_interp_idle(unsigned max_ms)
{
    LONGLONG now;
    unsigned ms;
    ip_poll();
    if (!ip_live || pd_owns_clock()) { /* W138: Takt-Thread zeigt die Bilder -> nur warten */
        Sleep(max_ms > 1u ? max_ms - 1u : 0u);
        return;
    }
    now = ip_now();
    ms = (ip_tnext > now) ? (unsigned)((ip_tnext - now) * 1000 / ip_qpf.QuadPart) : 0u;
    if (max_ms > 1u && ms > max_ms - 1u) { ms = max_ms - 1u; }
    Sleep(ms);
}

static int ip_mission(void)
{
    unsigned scene = 0, mloop = 0;
    __try {
        scene = *(volatile unsigned *)(uintptr_t)M3D_SCENE_ADDR;
        mloop = *(volatile unsigned *)(uintptr_t)M3D_MLOOP_ADDR;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return scene == 0x2Cu && mloop == 1u;
}

static void ip_stat_log(void)
{
    LONGLONG now = ip_now();
    double secs = ip_ms(st_t0, now) / 1000.0;
    if (secs <= 0.0) { secs = 1e-3; }
    HLOG_I("INTERP stat: ticks=%u pairs=%u presents=%u rate=%.1f/s ticks/s=%.1f K=%d lerp=%u rawlerp=%u rt2d=%u static=%u nomatch=%u rawchg=%u reject=%u 2d=%u cut=%u exact=%u hold=%u skip=%u late=%u nopair=%u ovf=%u rec=%.2fms cap=%.2fms replay=%.2fms(max %.2f) present=%.2fms gpu=%.2fms(max %.2f, n=%u) r=%.1fms(max %.1f) L=%.1fms verify=%u/%u",
           st_ticks, st_pairs, st_presents, (double)st_presents / secs,
           (double)st_ticks / secs, ip_K, st_lerp, st_rawlerp, st_rt2d, st_static, st_nomatch, st_rawchg, st_reject,
           st_2d, st_cut, st_exact, st_hold, st_skip, st_late, st_nopair, st_ovf,
           st_ticks ? st_rec_ms / st_ticks : 0.0,
           st_ticks ? (double)st_cap_q * 1000.0 / (double)ip_qpf.QuadPart / st_ticks : 0.0,
           st_rep_n ? st_rep_ms / st_rep_n : 0.0, st_rep_max,
           st_presents ? st_pres_ms / st_presents : 0.0,
           st_gpu_n ? st_gpu_ms / st_gpu_n : 0.0, st_gpu_max, st_gpu_n,
           st_r_n ? st_r_ms / st_r_n : 0.0, st_r_max, st_lat_n ? st_lat_ms / st_lat_n : 0.0,
           st_verify - st_verify_bad, st_verify);
    HLOG_I("INTERP stat2: geo=%u (Geometrie-Partner, in lerp enthalten)", st_geo);
    HLOG_I("INTERP inst: multi=%u wrong=%u amb=%u pass=%u (interp_inst=%d interp_pass=%d)", st_imulti,
           st_iwrong, st_iamb, st_ipass, g_cfg.interp_inst, g_cfg.interp_pass); /* POPIN (W91) */
    if (ip_rt_ok) { /* W126 RENDER-THREAD */
        HLOG_I("INTERP rt: auftraege=%u vorbereitet=%u nachgefordert=%u gewartet=%u verworfen=%u fehl=%u aufzeichnung=%.2fms(max %.2f, Render-Thread) spiel: holen=%.2fms ausfuehren=%.2fms je Bild",
               st_rt_n, st_rt_hit, st_rt_miss, st_rt_wait, st_rt_stale, st_rt_fail,
               st_rt_n ? st_rt_ms / st_rt_n : 0.0, st_rt_max,
               (st_rt_hit + st_rt_miss) ? st_rt_wait_ms / (st_rt_hit + st_rt_miss) : 0.0,
               st_rt_exec ? st_rt_exec_ms / st_rt_exec : 0.0);
        st_rt_n = st_rt_hit = st_rt_miss = st_rt_wait = st_rt_stale = st_rt_fail = st_rt_exec = 0;
        st_rt_ms = st_rt_max = st_rt_wait_ms = st_rt_exec_ms = 0.0;
    }
    st_geo = 0;
    st_imulti = st_iwrong = st_iamb = st_ipass = 0;
    st_t0 = now;
    st_presents = st_ticks = st_pairs = 0;
    st_lerp = st_static = st_nomatch = st_rawchg = st_reject = st_2d = st_rawlerp = st_rt2d = 0;
    st_cut = st_exact = st_hold = st_skip = st_late = st_nopair = st_ovf = 0;
    st_rec_ms = st_rep_ms = st_rep_max = st_pres_ms = 0.0;
    st_cap_q = 0;
    st_rep_n = 0;
    st_gpu_ms = st_gpu_max = 0.0;
    st_gpu_n = 0;
    st_r_ms = st_r_max = st_lat_ms = 0.0;
    st_r_n = st_lat_n = 0;
}

static void ip_hist_clear(void)
{
    int i;
    ip_rt_quiesce(); /* W126 RENDER-THREAD */
    for (i = 0; i < ip_nhist; i++) { ip_list_reset(&g_ipl[ip_hist[i]]); }
    ip_nhist = 0;
    ip_live = 0;
    ip_nneed = 0;
}

/* Bildfolge (interp_shots) fuer diesen Tick? Speichert das Spiel-Bild. */
static void ip_shots_tick(ip_list_t *B)
{
    char nm[64];
    ip_shot_ser = 0;
    if (g_cfg.interp_shots <= 0 || ip_pairs_total < (unsigned)g_cfg.interp_shots) { return; }
    if (!ip_shot_left && ip_pairs_total >= ip_shot_next) {
        if (ip_shot_seq >= (unsigned)g_cfg.interp_shot_max) { return; }
        ip_shot_left = (unsigned)(g_cfg.interp_shot_ticks > 0 ? g_cfg.interp_shot_ticks : 6);
        ip_shot_seq++;
        ip_shot_next = g_cfg.interp_shot_every > 0 ? ip_pairs_total + (unsigned)g_cfg.interp_shot_every
                                                   : 0xFFFFFFFFu;
    }
    if (!ip_shot_left) { return; }
    ip_shot_left--;
    ip_shot_ser = B->serial;
    _snprintf_s(nm, sizeof nm, _TRUNCATE, "ip_%06u_g", B->frame);
    sb_render_shot_name(nm, (ID3D11Resource *)g_ish[B->bbref].bak[ip_list_index(B)]);
}

/* W134 VR-PROTO: Augen-Wiedergabe eines Paars (unter ctx_lock, Context-Thread
 * oder VR-Thread). pA/pB = NULL -> Paar zum Zeitpunkt due (Echtzeit-Takt wie
 * ip_poll) bzw. im Testtakt das juengste Paar mit t aus der Zeit seit dem Swap. */
/* DIAG(VR-PROTO,W134): exaktes Mono-Bild des Ticks B (Vergleich Einzelauge, IPD 0, Kopf still) */
void sb_interp_vr_mono_shot(void *pB, const char *name)
{
    ip_list_t *B = (ip_list_t *)pB;
    if (!B || B->bbref < 0) { return; }
    sb_render_shot_name(name, (ID3D11Resource *)g_ish[B->bbref].bak[ip_list_index(B)]);
}

int sb_interp_vr_available(void)
{
    return ip_init_done && !ip_init_fail && ip_nhist >= 1;
}

int sb_interp_vr_replay(sb_vr_pass_t *p, void *pA, void *pB, float t, long long due, float *tout)
{
    ip_list_t *A = (ip_list_t *)pA, *B = (ip_list_t *)pB;
    ID3D11Buffer *vcb2 = NULL;
    int hold = 0;
    if (!ip_init_done || ip_init_fail || ip_nhist < 1 || !p || !p->rtv) { return 0; }
    if (!B) {
        LONGLONG now = ip_now();
        if (ip_live) {
            ip_pair_at(due ? (LONGLONG)due : now, &A, &B, &t, &hold);
        } else {
            B = &g_ipl[ip_hist[ip_nhist - 1]];
            A = ip_nhist >= 2 ? &g_ipl[ip_hist[ip_nhist - 2]] : NULL;
            t = 1.0f;
            if (A && B->tswap > A->tswap && now > B->tswap) {
                double f = (double)(now - B->tswap) / (double)(B->tswap - A->tswap);
                t = f >= 1.0 ? 1.0f : (float)f;
            }
        }
    }
    if (!B || B->bbref < 0 || !B->ready) { return 0; }
    if (A && !A->ready) { A = NULL; }
    p->hold = hold; /* W183 VR-FIX3: Diagnose Paarwahl */
    p->bser = B->serial;
    p->aser = A ? A->serial : 0u;
    ID3D11DeviceContext_VSGetConstantBuffers(g_ctx, 2, 1, &vcb2); /* VR-VS nutzt b2 */
    ip_vrp = p;
    {
        int gpm = sb_gp_begin(1); /* DIAG(GPU-COST,W146): GPU je VR-Bild (beide Augen) */
        ip_replay_ex(B, ip_list_index(B), A, A ? ip_list_index(A) : -1, t, 0, ip_cx_imm());
        if (gpm) { sb_gp_end(); }
    }
    ip_vrp = NULL;
    ID3D11DeviceContext_VSSetConstantBuffers(g_ctx, 2, 1, &vcb2);
    if (vcb2) { ID3D11Buffer_Release(vcb2); }
    if (tout) { *tout = (A && t < 1.0f) ? t : 1.0f; }
    return 1;
}

/* Swap N (sb_render_present, vor dem Present). 1 = Present uebernommen. */
int sb_interp_swap(void)
{
    int mission, took = 0, k, li;
    static unsigned npair_log, ndump;
    LONGLONG t0, now;
    ip_list_t *L = NULL, *A = NULL;
    if (g_cfg.interp_fps <= 0) { return 0; }
    if (!ip_init()) { return 0; }
    t0 = ip_now();
    sb_ctx_enter();
    ip_gpu_poll();
    mission = ip_mission();
    ip_K = g_cfg.interp_fps / 30;
    if (ip_K < 1) { ip_K = 1; }
    if (ip_K > 8) { ip_K = 8; }
    ip_dt = ip_qpf.QuadPart / g_cfg.interp_fps;
    /* 1. Aufnahme abschliessen und sichern. */
    if (ip_rec >= 0) {
        li = ip_rec;
        L = &g_ipl[li];
        ip_rec = -1;
        st_ticks++;
        L->frame = g_present_no;
        L->serial = ++ip_serial;
        L->tswap = t0;
        if (!mission || L->overflow || !L->nops) {
            if (L->overflow) { st_ovf++; }
            ip_list_reset(L);
            ip_hist_clear();
            L = NULL;
        } else {
            ip_list_snap(L, li);
            if (!L->snap_ok) {
                ip_list_reset(L);
                ip_hist_clear();
                L = NULL;
            }
        }
    }
    if (L) {
        L->ready = 1;
        A = ip_nhist ? &g_ipl[ip_hist[ip_nhist - 1]] : NULL;
        /* 2. Zuordnung gegen den Vortick. */
        if (A) {
            int log = (g_cfg.interp_diag >= 1) &&
                      ((npair_log < 20u && L->nops >= 500u) || (ip_pairs_total % 300u) == 0u);
            if (log) { npair_log++; }
            if (g_cfg.swap_match_unlock) {
                /* PERF(PERF-TICK,W168) swap_match_unlock=1: Zuordnung ist reine CPU-Arbeit (keine Context-Aufrufe)
                 * auf L (noch nicht in der Historie) und A->ops[].taken (liest die Wiedergabe nicht) -> ctx_lock
                 * waehrenddessen abgeben, damit der Present-Takt nicht 1-3 ms auf den Swap wartet. Ohne Takt-Thread
                 * (Testtakt) wirkungslos. */
                int cr = sb_ctx_release_all();
                L->pair_ok = ip_match(A, L, log);
                sb_ctx_reacquire(cr);
            } else
            L->pair_ok = ip_match(A, L, log);
            L->prevser = A->serial;
            if (g_cfg.fxr_diag) { /* DIAG(REM-FX,W133): Zuordnung der Effekt-Draws */
                unsigned q;
                for (q = 0; q < L->nops; q++) {
                    const ip_op_t *fo = &L->ops[q];
                    if (fo->kind == IP_DRAW && fo->fxk) { fxr_interp_note(fo->fxk, fo->why, fo->flag && L->pair_ok); }
                }
            }
            if (g_cfg.interp_diag >= 2 && ndump < 2u && L->nops >= 500u) {
                ndump++;
                ip_dump(L);
            }
            st_pairs++;
            ip_pairs_total++;
        } else {
            st_nopair++;
        }
        /* 3. Zustandszeit: Fensteranfang (Feldwechsel) + mittlere Rechenzeit r
         *    (regelmaessiger als die Swap-Zeit selbst); Testtakt: Swap-Zeit. */
        L->tstate = t0;
        if (g_cfg.vblank_hz > 0 && ip_vb_qpc && t0 - ip_vb_qpc < ip_qpf.QuadPart / 10) {
            double r = (double)(t0 - ip_vb_qpc);
            double rms = r * 1000.0 / (double)ip_qpf.QuadPart;
            ip_rbar = ip_rbar > 0.0 ? ip_rbar * 0.9 + r * 0.1 : r;
            L->tstate = ip_vb_qpc + (LONGLONG)ip_rbar;
            st_r_ms += rms;
            if (rms > st_r_max) { st_r_max = rms; }
            st_r_n++;
            ip_r_last_ms = rms; /* PERF(TICK-PERF,W125): interp_gpu_tight */
        }
        if (A && L->tstate <= A->tstate) { L->tstate = A->tstate + 1; }
        /* 4. Latenz L = max(Swap_N - Zustand_N-1) der letzten 32 Ticks + 1 ms. */
        if (A) {
            unsigned w;
            LONGLONG mx = 0;
            ip_need[ip_nneed++ % 32u] = t0 - A->tstate;
            for (w = 0; w < 32u && w < ip_nneed; w++) {
                if (ip_need[w] > mx) { mx = ip_need[w]; }
            }
            ip_lat = mx + ip_qpf.QuadPart / 1000;
            if (ip_lat > ip_qpf.QuadPart / 12) { ip_lat = ip_qpf.QuadPart / 12; } /* <= 83 ms */
            st_lat_ms += (double)ip_lat * 1000.0 / (double)ip_qpf.QuadPart;
            st_lat_n++;
        }
        /* 5. Historie: juengste 3 Listen behalten. */
        if (ip_nhist == IP_HIST) {
            ip_rt_drop(ip_hist[0]); /* W126 RENDER-THREAD: Auftrag mit dieser Liste abwarten/verwerfen */
            ip_list_reset(&g_ipl[ip_hist[0]]);
            memmove(ip_hist, ip_hist + 1, (IP_HIST - 1) * sizeof ip_hist[0]);
            ip_nhist--;
        }
        ip_hist[ip_nhist++] = li;
        ip_shots_tick(L);
        if (A && g_cfg.interp_diag >= 3 && ip_shot_ser == L->serial) {
            ip_diag_tick(A, ip_list_index(A), L, li);
        }
        if (A && g_cfg.interp_verify > 0 && (ip_pairs_total % (unsigned)g_cfg.interp_verify) == 1u) {
            /* W126 RENDER-THREAD: exakte Wiedergabe ueber denselben Pfad wie die
             * Zwischenbilder (Command-List des Render-Threads) pruefen. */
            ID3D11CommandList *vcl = ip_rt_active() ? ip_rt_get(A, L, 1.0f) : NULL;
            if (vcl) { ip_rt_exec(vcl); }
            else { ip_replay(L, li, A, ip_list_index(A), 1.0f); }
            ip_verify_check(L, li);
        }
    }
    /* 6. Naechste Aufnahme (freie Liste). */
    if (mission) {
        for (k = 0; k < IP_LISTS; k++) {
            int inh = 0, j;
            for (j = 0; j < ip_nhist; j++) {
                if (ip_hist[j] == k) { inh = 1; }
            }
            if (!inh) { break; }
        }
        ip_list_reset(&g_ipl[k]);
        ip_bset_rotate(&g_ipl[k]); /* PERF(TICK-PERF,W125) interp_bsets */
        ip_rec = (ip_vb_ensure(&g_ipl[k]) && ip_rq_ensure(&g_ipl[k])) ? k : -1;
        if (ip_rec >= 0 && g_cfg.render_thread > 0) { /* W126: s0 am Tick-Anfang (Deferred-Wiedergabe) */
            ID3D11DeviceContext_PSGetSamplers(g_ctx, 0, 1, &g_ipl[k].smp0);
        }
    } else {
        ip_hist_clear();
        ip_rec = -1;
    }
    g_ip_recording = (ip_rec >= 0);
    now = ip_now();
    st_rec_ms += ip_ms(t0, now);
    if (g_phud_meas) { sb_phud_interp_q(now - t0, 0); } /* W122 PERF-HUD: Aufzeichnung/Zuordnung */
    /* 7. Zeigen. Testtakt: K Bilder (t = k/K) des Paars sofort am Swap.
     *    Echtzeit: Takt starten bzw. weiterlaufen lassen (zeigt auch das
     *    Spiel-Bild dieses Ticks, um L verzoegert). */
    if (L && ip_K > 1) {
        if (g_cfg.vblank_hz <= 0) {
            if (A) {
                char tag[8];
                for (k = 1; k <= ip_K; k++) {
                    _snprintf_s(tag, sizeof tag, _TRUNCATE, "%d", k);
                    if (ip_rt_ok && k + 1 < ip_K) { ip_rt_nA = A; ip_rt_nB = L; ip_rt_nt = (float)(k + 1) / (float)ip_K; } /* W126 */
                    ip_show(A, L, k >= ip_K ? 1.0f : (float)k / (float)ip_K, tag);
                    ip_rt_nA = NULL;
                }
                took = 1;
            }
        } else if (ip_nhist >= 2) {
            if (!ip_live) {
                ip_live = 1;
                ip_tnext = now;
                ip_tlast_present = 0;
                if (pd_wake) { SetEvent(pd_wake); } /* W138: Takt-Thread sofort auf den Interp-Takt */
            }
            took = 1;
        }
    }
    if (!took) { ip_live = 0; }
    if (ip_live) { ip_rt_prefetch(); } /* W126 RENDER-THREAD: Historie neu -> naechstes Bild vorbereiten */
    sb_ctx_leave();
    if (ip_live) { ip_poll(); }
    if (g_cfg.interp_diag >= 1 && st_ticks >= 300u) { sb_ctx_enter(); ip_stat_log(); sb_ctx_leave(); } /* W138: Zaehler auch vom Takt-Thread */
    return took;
}
