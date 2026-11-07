/* render_mfx.c - MONITOR-FX (W105): Monitor-Effekte des Originals.
 *
 * Das Spiel zeichnet die Röhrenmonitor-Optik selbst (Disassembly, BERICHT W105):
 *
 * 1. Korn/Scanlines ("Wash"): FUN_00016d70 (Hauptmonitor, RT S40 512²),
 *    FUN_000174d0 (RT41 256²), FUN_00017710 (Radar 128²) zeichnen je ein
 *    P1-Quad (Begin(6), 4 Vertices, VS [0x3B8C8C]) mit der 128²-Rauschtextur
 *    [0x3B7B24] (swizzled 0x06 → NV2A normiert, WRAP). UV-Fenster
 *    [0x354EC4] = 6,97 (Init 0x2CDB7), Ursprung u = rand()·0,005 bzw. ·0,01
 *    (springt je Bild), v rollt um [0x356EA0]·0,5 bzw. ·1 (0,01/0,02 je Bild).
 *    Farbe 0,922 grau, Alpha:
 *      512²: [0x352150] (0,2)                        · (2·Nacht+1)
 *      256²: [0x35641C] + Spitze [0x356DBC] (0,8 bei Kanalwechsel, −20 %/Bild) · (2·Nacht+1)
 *      128²: [0x356F54] + Spitze [0x35182C]
 *    (Nacht = [0x351638], NIGHT SCOPE; Faktor nur bei [0x3B8CF4] < 2.)
 *    Unser Renderer hielt die UVs (> 1,5 bei RHW) für Pixel-UVs und teilte
 *    durch 128 → Fenster 0,054 → glatter Schleier statt Korn (W50 „Haze“).
 *
 * 2. Signalstörung (Zeilen-Zittern): FUN_00016d70 zeichnet vor dem Wash
 *    Begin(2) LINELIST mit N Zeilen (rollendes Band ab [0x31159C]) aus der
 *    Hauptmonitor-Textur [0x352144] (= S40 selbst) in S40, je Zeile um
 *    ±rand·Amp·0,001 px verschoben. N = clamp(50·[0x352BD8] − 55, 0, 512)
 *    bzw. 512 bei Treffer-Spitze [0x354F30] > 0,1 (−20 %/Bild). [0x352BD8]
 *    = Störpegel (Treffer FUN_0002f250, Ereignisse in FUN_00037290, Kappe 5).
 *    Das Spiel sampelt dabei sein eigenes RT; unser Renderer schaltete bei
 *    Selbst-Sampling auf den Backbuffer um (SIDESMEAR) → Zeilen landeten
 *    außerhalb des Monitors. Neu: Kopie des RT als Quelle (rtt_selfcopy).
 *
 * Schalter ([render]): monitor_fx 0 = aus (kein Korn, keine Störung),
 * 1 = Original (Standard), 2 = alter Pfad (vor W105, A/B);
 * monitor_grain / monitor_glitch 0..100 % (Standard 100 = Original).
 * mfx_diag=1: DIAG-Zeilen MFX; mfx_ab=1: A/B-Bildpaare (wie shade_ab). */
#include "render_int.h"
#include "sb_tex.h"
#include "render_qa.h"
int rq_ab_on(void); /* render_shade.c */
void rq_dump_srv(ID3D11ShaderResourceView *srv, unsigned xptr); /* render_combiner.c */

#define MFX_WASH_TEX   0x3B7B24u
#define MFX_MON_TEX    0x352144u
#define MFX_A512       0x352150u
#define MFX_A256       0x35641Cu
#define MFX_S256       0x356DBCu
#define MFX_A128       0x356F54u
#define MFX_S128       0x35182Cu
#define MFX_NIGHT      0x351638u
#define MFX_LEVEL      0x352BD8u
#define MFX_HIT        0x354F30u

int g_mfx_kind;    /* aktueller Draw: MFX_NONE/WASH/LINES */
int g_mfx_nonorm;  /* 1 = UV-Pixel-Normierung für diesen Draw aus */
extern int g_rtt_selfcopy; /* render_rtt.c */
static unsigned g_mfx_n[3], g_mfx_skip[3];
/* DIAG(W105) mfx_shots: zustandsgesteuerte Screenshots (Wash-/Zeilen-Bilder);
 * mit mfx_ab A/B-Paar: Bild p+1 alter Pfad, p+2 neu. */
static int g_mfx_fr[3];        /* dieses Bild hatte Wash/Zeilen */
static int g_mfx_abstate;      /* 0 normal, 1 = naechstes Bild alt + Shot, 2 = naechstes Bild neu + Shot */
static unsigned g_mfx_washfr, g_mfx_linefr, g_mfx_nshot, g_mfx_seq;

static int mfx_rd(unsigned a, unsigned *v)
{
    __try { *v = *(volatile unsigned *)(uintptr_t)a; }
    __except (EXCEPTION_EXECUTE_HANDLER) { *v = 0; return 0; }
    return 1;
}
static float mfx_rf(unsigned a)
{
    unsigned v = 0; float f;
    mfx_rd(a, &v);
    memcpy(&f, &v, 4);
    return (f == f && f > -1e6f && f < 1e6f) ? f : 0.0f;
}
static int mfx_mission(void)
{
    unsigned s = 0, m = 0;
    mfx_rd(M3D_SCENE_ADDR, &s);
    mfx_rd(M3D_MLOOP_ADDR, &m);
    return s == 0x2Cu && m == 1u;
}
static float mfx_pct(int v)
{
    if (v <= 0) { return 0.0f; }
    if (v >= 100) { return 1.0f; }
    return (float)v / 100.0f;
}

/* Klassifiziert einen IM-Draw (vor allen Umrechnungen). buf = g_im_buf
 * (Register 0 Position, 1 Farbe, 2 UV – belegt per Disassembly). */
int mfx_classify(unsigned prim, unsigned n, int is_vb)
{
    unsigned xp, wt = 0, mt = 0;
    g_mfx_kind = MFX_NONE;
    g_mfx_nonorm = 0;
    if (is_vb || n < 2) { return MFX_NONE; }
    /* DIAG(W105) mfx_ab=1: Screenshot-Bild alter Pfad, Partnerbild (+1) neu */
    if (g_cfg.monitor_fx == 2) { return MFX_NONE; }
    if (g_cfg.mfx_ab && g_cfg.mfx_shots > 0) { if (g_mfx_abstate == 1 && g_cfg.shade_ab != 3) { return MFX_NONE; } } /* W121 shade_ab=3: MFX in beiden Bildern */
    else if (g_cfg.mfx_ab && !rq_ab_on()) { return MFX_NONE; }
    xp = sb_tex_xptr();
    if (!xp) { return MFX_NONE; }
    mfx_rd(MFX_WASH_TEX, &wt);
    mfx_rd(MFX_MON_TEX, &mt);
    if (xp != wt && xp != mt) { return MFX_NONE; }
    if (!mfx_mission()) { return MFX_NONE; }
    if (xp == wt && prim == 6u && n == 4u) { g_mfx_kind = MFX_WASH; }
    else if (xp == mt && prim == 2u && (n & 1u) == 0u) { g_mfx_kind = MFX_LINES; }
    return g_mfx_kind;
}

/* Wendet Stärke/Korrektur an. buf = g_im_buf, *n/*prim dürfen sich ändern
 * (Zeilen → Dreiecks-Quads). Rückgabe 0 = Draw auslassen. */
int mfx_apply(float (*buf)[16][4], unsigned *n, unsigned *prim, unsigned nmax)
{
    float g = mfx_pct(g_cfg.monitor_grain), gl = mfx_pct(g_cfg.monitor_glitch);
    unsigned i;
    if (g_mfx_kind == MFX_NONE) { return 1; }
    g_mfx_n[g_mfx_kind]++;
    g_mfx_fr[g_mfx_kind] = 1;
    if (g_mfx_kind == MFX_WASH) {
        float xmax = 0.0f, a0 = buf[0][1][3], base = 1.0f, spike = 0.0f, k;
        for (i = 0; i < *n; i++) { if (buf[i][0][0] > xmax) { xmax = buf[i][0][0]; } }
        if (xmax > 384.0f) { base = mfx_rf(MFX_A512); spike = 0.0f; }
        else if (xmax > 192.0f) { base = mfx_rf(MFX_A256); spike = mfx_rf(MFX_S256); }
        else { base = mfx_rf(MFX_A128); spike = mfx_rf(MFX_S128); }
        if (base < 0.0f) { base = 0.0f; }
        if (spike < 0.0f) { spike = 0.0f; }
        k = (base + spike > 1e-6f) ? (base * g + spike * gl) / (base + spike) : g;
        if (!g_cfg.monitor_fx || k <= 0.0f) { g_mfx_skip[MFX_WASH]++; return 0; }
        if (g_cfg.mfx_diag >= 2) { k = 1000.0f; } /* DIAG: Wash deckend (wo landet er?) */
        for (i = 0; i < *n; i++) {
            float a = buf[i][1][3] * k;
            buf[i][1][3] = a > 1.0f ? 1.0f : a;
        }
        g_mfx_nonorm = 1; /* swizzled: NV2A-Koordinaten sind normiert (WRAP) */
        if (g_cfg.mfx_diag && g_mfx_n[MFX_WASH] == 1u) { /* DIAG: Rauschtextur einmal als BMP */
            ID3D11ShaderResourceView *ws = sb_tex_get();
            unsigned tw = 0, th = 0, tf = 0; int tl = 0;
            sb_tex_dims(&tw, &th); sb_tex_fmt(&tf, &tl);
            HLOG_I("MFX washtex xp=0x%X %ux%u fmt=0x%X lin=%d srv=%u", sb_tex_xptr(), tw, th, tf, tl, ws ? 1u : 0u);
            if (ws) { rq_dump_srv(ws, sb_tex_xptr()); }
        }
        if (g_cfg.mfx_diag && (g_mfx_n[MFX_WASH] <= 12u || (g_mfx_n[MFX_WASH] % 3000u) == 0u)) {
            HLOG_I("MFX wash #%u f=%u q=%.0f a=%.3f k=%.3f base=%.3f spike=%.3f uv=(%.2f,%.2f)-(%.2f,%.2f) rt=%d",
                   g_mfx_n[MFX_WASH], g_present_no, xmax, a0, k, base, spike,
                   buf[0][2][0], buf[0][2][1], buf[*n - 1][2][0], buf[*n - 1][2][1], g_rtt_bound);
        }
        return 1;
    }
    /* MFX_LINES: x' = u − gl·(u − x) (u = Quell-Spalte in Pixeln), dann
     * jede Zeile als 1 Pixel hohes Quad (deckt auch skalierte RTs, W88)
     * mit v auf Zeilenmitte. Quelle = RT-Kopie (rtt_selfcopy). */
    {
        unsigned nl = *n / 2u, nn;
        if (!g_cfg.monitor_fx || gl <= 0.0f) { g_mfx_skip[MFX_LINES]++; return 0; }
        if (nl * 6u > nmax) { nl = nmax / 6u; }
        if (g_cfg.mfx_diag && (g_mfx_n[MFX_LINES] <= 12u || (g_mfx_n[MFX_LINES] % 300u) == 0u || g_cfg.mfx_diag >= 3)) {
            HLOG_I("MFX lines #%u f=%u n=%u y0=%.0f dx0=%.1f lvl=%.3f hit=%.3f rt=%d",
                   g_mfx_n[MFX_LINES], g_present_no, nl, buf[0][0][1],
                   buf[0][2][0] - buf[0][0][0], mfx_rf(MFX_LEVEL), mfx_rf(MFX_HIT), g_rtt_bound);
        }
        for (i = nl; i-- > 0;) {
            float l0[16][4], l1[16][4];
            float y, v, x0, x1, u0, u1;
            unsigned o = i * 6u, q;
            memcpy(l0, buf[2u * i], sizeof l0);
            memcpy(l1, buf[2u * i + 1u], sizeof l1);
            u0 = l0[2][0]; u1 = l1[2][0];
            x0 = u0 - gl * (u0 - l0[0][0]);
            x1 = u1 - gl * (u1 - l1[0][0]);
            y = l0[0][1];
            v = l0[2][1] + 0.5f;
            for (q = 0; q < 6u; q++) {
                /* Quad-Ecken: 0 (x0,y) 1 (x1,y) 2 (x0,y+1) | 1 (x1,y) 3 (x1,y+1) 2 (x0,y+1) */
                static const unsigned char cx[6] = { 0, 1, 0, 1, 1, 0 };
                static const unsigned char cy[6] = { 0, 0, 1, 0, 1, 1 };
                float (*d)[4] = buf[o + q];
                memcpy(d, cx[q] ? l1 : l0, sizeof l0);
                d[0][0] = cx[q] ? x1 : x0;
                d[0][1] = y + (float)cy[q];
                d[2][0] = cx[q] ? u1 : u0;
                d[2][1] = v;
            }
        }
        nn = nl * 6u;
        *n = nn;
        *prim = 5u; /* TRIANGLELIST */
        g_rtt_selfcopy = 1;
        return 1;
    }
}

/* DIAG(SHADE-FX,W121) shade_ab=3: Phase des mfx_ab-Paares (1 = Bild p+1 "alt", 2 = p+2) */
int mfx_ab_phase(void)
{
    return g_mfx_abstate;
}

void mfx_end(void)
{
    g_rtt_selfcopy = 0;
    g_mfx_kind = MFX_NONE;
    g_mfx_nonorm = 0;
}

/* DIAG: Screenshot-Ausloeser (mfx_shots, je Present aufgerufen). */
static void mfx_shot_tick(void)
{
    int wash = g_mfx_fr[MFX_WASH], lines = g_mfx_fr[MFX_LINES], trig = 0;
    g_mfx_fr[MFX_WASH] = g_mfx_fr[MFX_LINES] = 0;
    if (g_mfx_abstate == 1) { g_shot_census = 1; g_mfx_abstate = 2; HLOG_I("MFX shot f=%u alt", g_present_no); return; }
    if (g_mfx_abstate == 2) { g_shot_census = 1; g_mfx_abstate = 0; g_mfx_nshot++; HLOG_I("MFX shot f=%u neu", g_present_no); return; }
    if (g_mfx_seq) { g_mfx_seq--; g_shot_census = 1; HLOG_I("MFX shot f=%u folge", g_present_no); return; }
    if (g_cfg.shade_ab == 3) { /* DIAG(W121): Paare nur in der Kampagnen-Mission (Modus 4), Titel-Demo zaehlt nicht */
        unsigned mode = 0;
        mfx_rd(0x2EEA34u, &mode);
        if (mode != 4u) { return; }
    }
    if (lines) { g_mfx_linefr++; if (g_mfx_linefr == 2u || g_mfx_linefr == 40u || g_mfx_linefr == 120u || g_mfx_linefr == 400u) { trig = 1; } }
    if (wash && !trig) { g_mfx_washfr++; if ((g_mfx_washfr % 400u) == 30u && g_mfx_nshot < (unsigned)g_cfg.mfx_shots) { trig = 1; } }
    if (!trig) { return; }
    if (!mfx_mission()) { return; }
    if (g_cfg.mfx_ab) { g_mfx_abstate = 1; }
    else { g_shot_census = 1; g_mfx_seq = 2; g_mfx_nshot++; HLOG_I("MFX shot f=%u %s", g_present_no, lines ? "zeilen" : "wash"); }
}

/* DIAG: Störpegel/Spitzen 1×/300 Missionsbilder (mfx_diag=1). */
void mfx_present(void)
{
    static unsigned last;
    if (g_cfg.mfx_shots > 0) { mfx_shot_tick(); }
    if (!g_cfg.mfx_diag || g_present_no - last < 300u || !mfx_mission()) { return; }
    last = g_present_no;
    {
        unsigned night = 0, mode = 0;
        mfx_rd(MFX_NIGHT, &night);
        mfx_rd(0x3B8CF4u, &mode);
        HLOG_I("MFX stat f=%u lvl=%.3f hit=%.3f a512=%.3f a256=%.3f s256=%.3f a128=%.3f s128=%.3f night=%u mode=%u wash=%u/%u lines=%u/%u",
               g_present_no, mfx_rf(MFX_LEVEL), mfx_rf(MFX_HIT), mfx_rf(MFX_A512), mfx_rf(MFX_A256),
               mfx_rf(MFX_S256), mfx_rf(MFX_A128), mfx_rf(MFX_S128), night, mode,
               g_mfx_n[MFX_WASH], g_mfx_skip[MFX_WASH], g_mfx_n[MFX_LINES], g_mfx_skip[MFX_LINES]);
    }
}
