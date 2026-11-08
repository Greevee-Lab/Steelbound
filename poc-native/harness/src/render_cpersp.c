/* render_cpersp.c - W191 UW-PERSP: Cockpit-Perspektive (nur Desktop,
 * Grafik-Option, Standard aus = Original bitgleich).
 *
 * Dolly-Zoom der Cockpit-Ansicht: Kamera um d entlang der Blickachse, Sichtfeld
 * k = (D - d) / D. Alles in der Ebene D bleibt exakt an seinem Platz, Teile
 * davor/dahinter skalieren mit s(w) = k * w / (w - d) (w = Abstand). Gleiche
 * Skala in x und y je Tiefe: echte Perspektive, nie gestreckt. Tiefen (W191,
 * Skalenfit je Bildbereich): Radar 0,80 m, Frontview 0,71 m, Instrumente
 * ~0,7 m, Pfosten-Displays ~0,76 m, sichtbare Seitenkante ~0,5 m.
 *  - cockpit_persp_dist < 0 (Standard -250 mm): Kamera VOR, weiteres
 *    Sichtfeld, Ebene D = 0,75 m (zwischen Radar und Frontview): Radar,
 *    Frontview, Hauptmonitor und HUD bleiben (+-3 %), die nahen Seitenpanels
 *    wandern nach aussen und fuellen den Breitbildrand (Kante x1,25 bei 100 %).
 *  - cockpit_persp_dist > 0: Kamera ZURUECK, engeres Sichtfeld (Experiment):
 *    nahe Teile schrumpfen, die Monitoroeffnung bleibt praktisch gleich
 *    (Pfosten liegen in der Ebene des vertikalen Rahmens) - kein "mehr Monitor".
 * Ein breiteres Sichtfeld allein zeigt nichts Neues: das Mesh endet im Hor+-Bild
 * bei ~1,27 halben 4:3-Breiten (W166).
 *
 * Draw-Klassen im Backbuffer der Cockpit-Ansicht (W134/W164):
 *  - 3D-Pfad (Cockpit-Mesh, per-Draw-WVP, Objektraum w = 1, Clip-w metrisch):
 *    MVP angepasst (mvp[15] -= d, mvp[14] -= al*d, Spalten X/Y * k) - kein
 *    Shader geaendert, Interp-Wiedergabe nimmt die aufgezeichnete MVP.
 *  - VSH-P2 mit z/w in (0,2; 0,999) (Instrumente, Monitor-HUD; Tiefe
 *    LESS_EQUAL gegen das Mesh): je Vertex auf der CPU, metrische Tiefe aus
 *    z/w (w = be / (z/w - al), al/be aus der Mesh-MVP), gleiche Abbildung wie
 *    das Mesh, Tiefe 2e-5 zum Auge (wie VR W164).
 *  - uebrige P2 (z/w ~0,034, fast orthografisch) und 2D/RHW: unveraendert
 *    (liegen in der Ebene D; nur der optionale Zoom).
 * Welt/Monitore/Schatten laufen ueber Render-Targets und bleiben unberuehrt
 * (Zielen, Fadenkreuz und Lock-On rechnet das Spiel in der Welt-Kamera).
 *
 * Gate: nur Cockpit-Ansicht in der Mission (Spiel-Entscheidung FUN_000459B0
 * @0x45E3D, wie W166), nie VR (vr_enable), nie in ein Render-Target.
 * Standard cockpit_persp=0 und cockpit_zoom=100: sb_cp_frame() kehrt vor jedem
 * Speicherzugriff mit 0 zurueck, kein Draw wird angefasst (bitgleich).
 */
#include "render_int.h"
#include <math.h>
#include "render_cpersp_math.h"

#define CP_CYC_MAX 8

static unsigned cp_f = 0xFFFFFFFFu;
static int cp_on;              /* dieses Bild: Option aktiv (Cockpit-Ansicht) */
static int cp_view;            /* letzte Ansichts-Entscheidung */
static float cp_d, cp_D, cp_k, cp_zoom; /* m, m, Sichtfeld-Faktor, Zoom */
static float cp_al, cp_be, cp_n;        /* Tiefe der Spielprojektion: z/w = al + be / w, Near */
static float cp_Mz[16];
static float cp_al3, cp_be3;           /* Tiefe aus der Mesh-MVP: z/w = al3 + be3 / w */
static unsigned cp_have3;               /* Bild (+1) der letzten Mesh-MVP */
static int cp_pct_now, cp_zoom_now, cp_dmm_now;
/* Kanten (aus dem groessten 3D-Draw, Cockpit-Schale) */
static unsigned cp_ex_f;                /* Bild der letzten Messung (+1, 0 = keine) */
static int cp_ex_key[4] = { -1, -1, -1, -1 };
static unsigned cp_ex_n;
static int cp_log_k[7];
static int cp_cyc_n = -1, cp_cyc_p[CP_CYC_MAX], cp_cyc_z[CP_CYC_MAX], cp_cyc_pn[CP_CYC_MAX];
static int cp_pn_now;                   /* Panini-Staerke dieses Bildes (%), 0 = aus */
static unsigned cp_cls[4];              /* DIAG: Draws je Klasse im Bild (3d, p2-decal, p2-flach, 2d) */
static unsigned cp_p2v;                 /* DIAG: P2-Vertices verschoben */
static LONGLONG cp_q_frame;             /* CPU-Zeit der Option im laufenden Bild (QPC-Ticks) */
static double cp_cost_ema;              /* geglaettet, ms je Bild (F2-Overlay) */

static unsigned cp_rd32(unsigned a)
{
    unsigned v = 0xFFFFFFFFu;
    __try { v = *(volatile unsigned *)(uintptr_t)a; }
    __except (EXCEPTION_EXECUTE_HANDLER) { v = 0xFFFFFFFEu; }
    return v;
}

static unsigned cp_rd16(unsigned a)
{
    unsigned v = 0xFFFFu;
    __try { v = *(volatile unsigned short *)(uintptr_t)a; }
    __except (EXCEPTION_EXECUTE_HANDLER) { v = 0xFFFEu; }
    return v;
}

/* 1 = das Spiel zeichnet in diesem Bild das Cockpit (FUN_000459B0 @0x45E3D:
 * word[0x346FB0] != 0x80 und (word[0x3516DC] == 0 oder dword[0x3520DC] != 0)),
 * nur in der Mission (Szene 0x2C, mloop 1). Gleiche Entscheidung wie W166. */
static int cp_cockpit_view(void)
{
    unsigned b;
    if (cp_rd32(M3D_SCENE_ADDR) != 0x2Cu || cp_rd32(M3D_MLOOP_ADDR) != 1u) { return 0; }
    if (cp_rd16(0x346FB0u) == 0x80u) { return 0; }
    b = cp_rd32(0x3520DCu);
    return cp_rd16(0x3516DCu) == 0u || (b != 0u && b < 0xFFFFFFFEu);
}

static void cp_cycle_parse(void)
{
    const char *s = g_cfg.cockpit_persp_cycle;
    cp_cyc_n = 0;
    while (*s && cp_cyc_n < CP_CYC_MAX) {
        int p = 0, z = 100, pn = 0;
        char *e = NULL;
        p = (int)strtol(s, &e, 10);
        if (e == s) { break; }
        s = e;
        if (*s == '/') { s++; z = (int)strtol(s, &e, 10); if (e == s) { z = 100; } s = e; }
        if (*s == '/') { s++; pn = (int)strtol(s, &e, 10); if (e == s) { pn = 0; } s = e; }
        cp_cyc_p[cp_cyc_n] = p;
        cp_cyc_z[cp_cyc_n] = z;
        cp_cyc_pn[cp_cyc_n] = pn;
        cp_cyc_n++;
        while (*s == ',' || *s == ' ') { s++; }
    }
}

static int cp_clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Tiefe der Spielprojektion aus g_proj_game: z/w = al + be / w (w = Abstand).
 * Fallback Near/Far 0,1/100 (wie VR W134: g_proj_game 0,1/100). */
static void cp_depth_params(void)
{
    float s = g_proj_game.m[11], m10 = g_proj_game.m[10], m14 = g_proj_game.m[14];
    if ((s > 0.99f && s < 1.01f) || (s < -0.99f && s > -1.01f)) {
        float al = m10 * (s > 0.0f ? 1.0f : -1.0f), be = m14;
        float n = (al != 0.0f) ? -be / al : 0.0f; /* z/w = 0 bei w = Near */
        if (be < 0.0f && al > 0.9f && al < 1.2f && n > 0.01f && n < 1.0f) {
            cp_al = al; cp_be = be; cp_n = n;
            return;
        }
    }
    cp_n = 0.1f;
    cp_al = 100.0f / (100.0f - 0.1f);
    cp_be = -0.1f * cp_al;
}

static void cp_build(void)
{
    memset(cp_Mz, 0, sizeof cp_Mz);
    cp_Mz[0] = cp_Mz[5] = cp_zoom;
    cp_Mz[10] = cp_Mz[15] = 1.0f;
}

static int cp_mvp3(float m[16], float *al_out, float *be_out)
{
    return cp_mvp_redo(m, 0.0f, 1.0f, cp_d, cp_k * cp_zoom, al_out, be_out);
}

static void cp_log_state(unsigned f)
{
    int k[7];
    if (cp_cyc_n > 0) { return; } /* DIAG-Wechsel je Bild: kein Log-Sturm */
    k[0] = cp_on; k[1] = cp_view; k[2] = (int)g_bb_w; k[3] = (int)g_bb_h;
    k[4] = cp_pct_now * 1000 + cp_dmm_now; k[5] = cp_zoom_now; k[6] = g_cfg.cockpit_persp_ref;
    if (memcmp(k, cp_log_k, sizeof k) == 0) { return; }
    memcpy(cp_log_k, k, sizeof k);
    HLOG_I("CPERSP on=%d ansicht=%d bb=%ux%u staerke=%d%% d=%.3f ref=%.3f k=%.4f zoom=%.2f near=%.3f f=%u",
           cp_on, cp_view, g_bb_w, g_bb_h, cp_pct_now, cp_d, cp_D, cp_k, cp_zoom, cp_n, f);
}

static void cp_eval(void)
{
    int pct = g_cfg.cockpit_persp, zm = g_cfg.cockpit_zoom, pn = g_cfg.cockpit_panini;
    cp_on = 0;
    if (g_cfg.cockpit_persp_cycle[0]) {
        if (cp_cyc_n < 0) { cp_cycle_parse(); }
        if (cp_cyc_n > 0) {
            unsigned i = g_present_no % (unsigned)cp_cyc_n;
            pct = cp_cyc_p[i];
            zm = cp_cyc_z[i];
            pn = cp_cyc_pn[i];
        }
    }
    cp_pn_now = cp_clampi(pn, 0, 100);
    pct = cp_clampi(pct, 0, 100);
    zm = cp_clampi(zm <= 0 ? 100 : zm, 100, 200);
    cp_pct_now = pct;
    cp_zoom_now = zm;
    cp_view = cp_cockpit_view();
    if (g_cfg.vr_enable || !g_bb_w || !g_bb_h || !cp_view || (pct == 0 && zm == 100)) {
        cp_d = 0.0f; cp_k = 1.0f; cp_zoom = 1.0f;
        return;
    }
    cp_depth_params();
    cp_D = (float)cp_clampi(g_cfg.cockpit_persp_ref, 200, 5000) / 1000.0f;
    /* Kamera-Weg: cockpit_persp_dist > 0 = zurueck (d < 0), < 0 = vor (d > 0) */
    cp_d = -(float)pct / 100.0f * (float)cp_clampi(g_cfg.cockpit_persp_dist, -400, 3000) / 1000.0f;
    if (cp_d > cp_D * 0.5f) { cp_d = cp_D * 0.5f; }
    cp_dmm_now = (int)(cp_d * 1000.0f + 0.5f);
    cp_k = (cp_D - cp_d) / cp_D;
    cp_zoom = (float)zm / 100.0f;
    cp_build();
    cp_on = 1;
}

/* W209: replay uses original P2 positions, never inverse-projects biased depth.
 * Mesh depth is recovered in draw order; the same near plane and P2 mapping
 * as in the live draw are used. All scratch work happens in pause only. */
static float cp_rec_d, cp_rec_kz = 1.0f, cp_rec_zoom = 1.0f;
static float cp_pause_d, cp_pause_k, cp_pause_z;
static float cp_pause_al, cp_pause_be, cp_pause_n;
static float cp_shown_d, cp_shown_kz = 1.0f, cp_shown_z = 1.0f;

int sb_cp_pause_begin(const float camera[3])
{
    float D;
    int pct = cp_clampi(g_cfg.cockpit_persp, 0, 100);
    cp_view = cp_cockpit_view();
    if (g_cfg.vr_enable || !cp_view) { return 0; }
    D = (float)cp_clampi(g_cfg.cockpit_persp_ref, 200, 5000) / 1000.0f;
    cp_pause_d = -(float)pct / 100.0f * (float)cp_clampi(g_cfg.cockpit_persp_dist, -400, 3000) / 1000.0f;
    if (cp_pause_d > D * 0.5f) { cp_pause_d = D * 0.5f; }
    cp_pause_k = (D - cp_pause_d) / D;
    cp_pause_z = (float)cp_clampi(g_cfg.cockpit_zoom <= 0 ? 100 : g_cfg.cockpit_zoom, 100, 200) / 100.0f;
    cp_rec_d = camera[0]; cp_rec_kz = camera[1]; cp_rec_zoom = camera[2];
    if (!g_cfg.cockpit_persp_pause) {
        cp_pause_d = cp_rec_d;
        cp_pause_k = cp_rec_kz / cp_rec_zoom;
    }
    cp_shown_d = cp_pause_d; cp_shown_kz = cp_pause_k * cp_pause_z; cp_shown_z = cp_pause_z;
    cp_depth_params();
    cp_pause_al = cp_al; cp_pause_be = cp_be; cp_pause_n = cp_n;
    return cp_pause_d != cp_rec_d || cp_shown_kz != cp_rec_kz || cp_pause_z != cp_rec_zoom;
}

static int cp_pause_hook(float m[16], int cls, float *pos, size_t st, unsigned n)
{
    float ww = m[3] * m[3] + m[7] * m[7] + m[11] * m[11];
    int i;
    if (cls == 1 && ww > 1e-12f) {
        float al, be;
        int ret = cp_mvp_redo(m, cp_rec_d, cp_rec_kz, cp_pause_d,
                              cp_pause_k * cp_pause_z, &al, &be);
        if (ret && n >= 10000u) { cp_pause_al = al; cp_pause_be = be; }
        return ret;
    }
    /* P2 has only diagonal Hor+/zoom; RHW has viewport translation and cls 0.
     * Original positions have been restored before entering this hook. */
    if (cls == 1 && pos && m[10] == 1.0f && m[15] == 1.0f &&
        m[12] == 0.0f && m[13] == 0.0f && m[14] == 0.0f) {
        cp_p2_apply(pos, st, n, cp_pause_d, cp_pause_k, cp_pause_al, cp_pause_be, cp_pause_n);
    }
    for (i = 0; i < 4; i++) {
        m[i * 4] *= cp_pause_z / cp_rec_zoom;
        m[i * 4 + 1] *= cp_pause_z / cp_rec_zoom;
    }
    return 1;
}

int sb_cp_frame(void)
{
    static int hooked;
    if (!hooked) { hooked = 1; sb_interp_set_pause_mvp_hook(cp_pause_hook); }
    if (!g_cfg.cockpit_persp && g_cfg.cockpit_zoom == 100 && !g_cfg.cockpit_panini && !g_cfg.cockpit_persp_cycle[0]) {
        cp_view = 0; /* Panini aus */
        return 0;
    }
    if (cp_f != g_present_no) {
        cp_f = g_present_no;
        if (g_cfg.cockpit_persp_diag >= 2 && cp_on && (g_present_no % 300u) == 0u) {
            HLOG_I("CPERSP klassen (Bild %u): 3d=%u p2-decal=%u p2-flach=%u 2d=%u p2-vertices verschoben=%u cpu=%.3fms/Bild (geglaettet)",
                   g_present_no - 1u, cp_cls[0], cp_cls[1], cp_cls[2], cp_cls[3], cp_p2v, cp_cost_ema);
        }
        memset(cp_cls, 0, sizeof cp_cls);
        cp_p2v = 0;
        if (cp_on) {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            cp_cost_ema = cp_cost_ema * 0.95 + 0.05 * ((double)cp_q_frame * 1000.0 / (double)f.QuadPart);
        }
        cp_q_frame = 0;
        cp_eval();
        cp_log_state(g_present_no);
        cp_shown_d = cp_d; cp_shown_kz = cp_k * cp_zoom; cp_shown_z = cp_zoom;
    }
    return cp_on;
}

static void m4mul(float r[16], const float a[16], const float b[16])
{
    float t[16];
    int i, j;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            t[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
        }
    }
    memcpy(r, t, sizeof t);
}

static int cp_fcmp(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* Kanten des Cockpit-Meshs (groesster 3D-Draw): einmal je Einstellungs-/
 * Aufloesungswechsel und alle 600 Bilder. pos = Objektraum-Positionen (Stride
 * st Floats), mvp0 = Original-MVP. Die Vertex-Extreme liegen weit ausserhalb
 * (nahe Waende, geclippt); massgeblich ist die SICHTBARE Kante bei ~1,27
 * halben 4:3-Breiten (W166): Median-w der Vertices mit |x| in [1,15; 1,35]
 * (4:3-Einheiten, |y| < 1) -> Kantenfaktor s = k*zoom*w/(w - d). */
static float cp_edge_f = 1.0f;
static void cp_measure(const float *pos, size_t st, unsigned n, const float mvp0[16])
{
    static float we[8192], wm[4096];
    float oa = (float)g_bb_w / (float)(g_bb_h ? g_bb_h : 1), vk = (4.0f / 3.0f) / (oa > 0.0f ? oa : 1.0f);
    float wmin = 1e9f, kz = cp_k * cp_zoom, wedge = 0.0f, wmon = 0.0f, f = 1.0f;
    unsigned i, ne = 0, nm = 0, nw1 = 0, nnear = 0;
    for (i = 0; i < n; i++) {
        const float *p = pos + (size_t)i * st;
        float X = p[0] * mvp0[0] + p[1] * mvp0[4] + p[2] * mvp0[8] + p[3] * mvp0[12];
        float Y = p[0] * mvp0[1] + p[1] * mvp0[5] + p[2] * mvp0[9] + p[3] * mvp0[13];
        float W = p[0] * mvp0[3] + p[1] * mvp0[7] + p[2] * mvp0[11] + p[3] * mvp0[15];
        float x, y;
        if (p[3] < 0.999f || p[3] > 1.001f) { nw1++; }
        if (!(W > 0.05f)) { continue; }
        if (W < wmin) { wmin = W; }
        if (W - cp_d < cp_n) { nnear++; }
        x = X / W / vk; y = Y / W;
        if (fabsf(y) < 1.0f && fabsf(x) > 1.15f && fabsf(x) < 1.35f && ne < 8192u) { we[ne++] = W; }
        if (fabsf(x) < 0.3f && y > 0.0f && y < 0.6f && nm < 4096u) { wm[nm++] = W; }
    }
    if (ne) { qsort(we, ne, sizeof we[0], cp_fcmp); wedge = we[ne / 2u]; }
    if (nm) { qsort(wm, nm, sizeof wm[0], cp_fcmp); wmon = wm[nm / 2u]; }
    if (wedge > cp_d + cp_n) { f = kz * wedge / (wedge - cp_d); } else if (ne) { f = kz * 4.0f; } else { f = cp_zoom; }
    cp_edge_f = f;
    cp_ex_f = g_present_no + 1u;
    cp_ex_n++;
    if (cp_cyc_n > 0 && cp_ex_n > 2u * (unsigned)cp_cyc_n && (g_cfg.cockpit_persp_diag < 2 || (cp_ex_n % 600u) != 0u)) { return; }
    HLOG_I("CPERSP mesh: n=%u bb=%ux%u staerke=%d%% zoom=%d%% d=%.3f k=%.4f: sichtbare Kante (|x| 1,15..1,35) w Median=%.3f (%u) -> faktor %.3f "
           "= Kante bei %.2f halben 4:3-Breiten (Bildrand %.2f); w Monitorbereich Median=%.3f (%u), naechstes w=%.3f, hinter Near=%u, w!=1: %u, tiefe al=%.4f be=%.5f f=%u",
           n, g_bb_w, g_bb_h, cp_pct_now, cp_zoom_now, cp_d, cp_k, wedge, ne, f, 1.27f * f, 1.0f / vk, wmon, nm, wmin, nnear, nw1,
           cp_al3, cp_be3, g_present_no);
}

static void cp_draw_body(float mvp[16], int cls, float *pos, size_t st, unsigned n);
void sb_cp_draw(float mvp[16], int cls, float *pos, size_t st, unsigned n, unsigned prim)
{
    LARGE_INTEGER q0, q1;
    QueryPerformanceCounter(&q0);
    if (g_ip_recording) {
        float camera[3] = { cp_d, cp_k * cp_zoom, cp_zoom };
        sb_interp_rec_cpersp(camera, cls == 1 && cp_d != 0.0f ? pos : NULL, st, prim, n);
    }
    cp_draw_body(mvp, cls, pos, st, n);
    QueryPerformanceCounter(&q1);
    cp_q_frame += q1.QuadPart - q0.QuadPart;
}

/* F2-Overlay: Staerke in % (0 aus) und CPU-Kosten (ms je Bild, geglaettet) */
int sb_cp_status(double *ms)
{
    if (ms) { *ms = cp_cost_ema; }
    return (g_cfg.cockpit_persp && !g_cfg.vr_enable) ? g_cfg.cockpit_persp : 0;
}

static void cp_draw_body(float mvp[16], int cls, float *pos, size_t st, unsigned n)
{
    float t[16];
    if (cls == 0) { /* 3D-Pfad (Cockpit-Mesh) */
        int key[4];
        float al, be;
        cp_cls[0]++;
        memcpy(t, mvp, sizeof t);
        if (!cp_mvp3(mvp, &al, &be)) {
            if (cp_zoom != 1.0f) { m4mul(mvp, t, cp_Mz); }
            return;
        }
        if (n >= 10000u) { cp_al3 = al; cp_be3 = be; cp_have3 = g_present_no + 1u; } /* Tiefe des Meshs fuer die Decals */
        key[0] = cp_dmm_now; key[1] = cp_zoom_now; key[2] = (int)g_bb_w; key[3] = (int)g_bb_h;
        if (n >= 10000u && pos &&
            (memcmp(key, cp_ex_key, sizeof key) != 0 || g_present_no >= cp_ex_f + 600u)) {
            memcpy(cp_ex_key, key, sizeof key);
            cp_measure(pos, st, n, t);
        }
        return;
    }
    if (cls == 1 && pos && cp_d != 0.0f) { /* VSH-P2 (Clip-Raum auf der CPU) */
        unsigned moved = 0;
        float al = cp_al, be = cp_be;
        if (cp_have3 && cp_have3 + 2u >= g_present_no && cp_be3 < 0.0f && cp_al3 > 0.5f) { al = cp_al3; be = cp_be3; }
        moved = cp_p2_apply(pos, st, n, cp_d, cp_k, al, be, cp_n);
        cp_cls[moved ? 1 : 2]++;
        cp_p2v += moved;
    } else {
        cp_cls[cls == 1 ? 2 : 3]++;
    }
    if (cp_zoom != 1.0f) {
        memcpy(t, mvp, sizeof t);
        m4mul(mvp, t, cp_Mz);
    }
}

/* Bildraender (W107): Box horizontal/vertikal um den Faktor erweitern, um den
 * die Cockpit-Kante nach aussen gewandert ist (Verlauf beginnt dort, wo er im
 * Original relativ zur Kante begann). Nur mit frischer Messung. */
void sb_cp_edge_box(unsigned *bx, unsigned *by, unsigned *bw, unsigned *bh)
{
    float fx, fy, cx, cy, w, h;
    if (!cp_on || !cp_ex_f || cp_f + 2u < g_present_no || g_present_no + 1u > cp_ex_f + 1200u) { return; }
    fx = cp_edge_f;
    fy = cp_zoom;
    if (fx < 1.0f) { fx = 1.0f; }
    if (fy < 1.0f) { fy = 1.0f; }
    if (fx == 1.0f && fy == 1.0f) { return; }
    cx = (float)*bx + (float)*bw * 0.5f;
    cy = (float)*by + (float)*bh * 0.5f;
    w = (float)*bw * fx;
    h = (float)*bh * fy;
    {
        float x0 = cx - w * 0.5f, y0 = cy - h * 0.5f, x1 = cx + w * 0.5f, y1 = cy + h * 0.5f;
        if (x0 < 0.0f) { x0 = 0.0f; }
        if (y0 < 0.0f) { y0 = 0.0f; }
        if (x1 > (float)g_bb_w) { x1 = (float)g_bb_w; }
        if (y1 > (float)g_bb_h) { y1 = (float)g_bb_h; }
        *bx = (unsigned)(x0 + 0.5f);
        *by = (unsigned)(y0 + 0.5f);
        *bw = (unsigned)(x1 + 0.5f) - *bx;
        *bh = (unsigned)(y1 + 0.5f) - *by;
    }
}

/* DIAG(UW-PERSP,W191) cockpit_persp_shots: Bildfolgen aus Folgebildern (mit
 * cockpit_persp_cycle wechselt die Einstellung je Bild -> gleiche Szene).
 * Aufruf im Present vor dem Screenshot (unter ctx_lock, ohne FPS-INTERP). */
void sb_cp_present(unsigned n)
{
    static unsigned cvf, cvf_demo, left, idx, nlist, list[8];
    static int parsed, demo[8];
    unsigned md;
    if (!g_cfg.cockpit_persp_shots[0]) { return; }
    if (!parsed) {
        const char *s = g_cfg.cockpit_persp_shots;
        parsed = 1;
        while (*s && nlist < 8u) {
            char *e = NULL;
            long v = strtol(s, &e, 10);
            if (e == s) { break; }
            demo[nlist] = v < 0;
            list[nlist++] = (unsigned)(v < 0 ? -v : v);
            s = e;
            while (*s == ',' || *s == ' ') { s++; }
        }
    }
    if (left) {
        char nm[64];
        left--;
        _snprintf_s(nm, sizeof nm, _TRUNCATE, "cpersp_%06u_p%03d_z%03d_n%03d", n, cp_on ? cp_pct_now : 0, cp_on ? cp_zoom_now : 100,
                    sb_cp_panini_d() > 0.0f ? cp_pn_now : 0);
        sb_render_shot_name(nm, NULL);
        if (g_cfg.edge_ab) { sb_edge_ab_shots(nm, NULL); } /* + _e1/_e2: Bildraender mit der verschobenen Kante */
        HLOG_I("CPERSP shot %s: on=%d ansicht=%d staerke=%d%% zoom=%d%% d=%.3f k=%.4f kantenfaktor=%.3f", nm, cp_on, cp_view,
               cp_pct_now, cp_zoom_now, cp_d, cp_k, cp_edge_f);
        return;
    }
    if (idx >= nlist || cp_f + 1u != n || !cp_view) { return; } /* Draws dieses Bildes liefen mit g_present_no = n - 1 */
    md = cp_rd32(0x2EEA34u);
    if (md == 4u) { cvf++; }
    cvf_demo++;
    if ((demo[idx] ? cvf_demo : cvf) >= list[idx]) {
        idx++;
        left = 2u * (unsigned)(cp_cyc_n > 0 ? cp_cyc_n : 1);
    }
}

/* Panini-d dieses Bildes (0 = aus): Cockpit-Ansicht, nicht VR. Ohne Zyklus
 * gilt der aktuelle cfg-Wert (live auch im Standbild der Pause).
 * Rahmen halten (cockpit_panini_frame=1, Standard): Radar-Oberkante und
 * Frontview-Unterkante (M1-Cockpit: 3,7 % / 93 % der Bildhoehe, Tiefe 0,80 /
 * 0,71 m) muessen nach Kamera-Weg und Panini voll im Bild bleiben. Panini
 * vergroessert die Bildmitte formtreu um S(d) = (d + cos t)/(cos t (d + 1));
 * der Rahmen erlaubt S <= 2 / (y_radar - y_frontview) -> 100 % = groesstes d
 * ohne Anschnitt (16:9 ~1,0, 21:9 ~0,4, 32:9 ~0,2), dazu eine senkrechte
 * Verschiebung o, die den Rahmen mittig haelt. */
#define CP_FR_TOP 0.037f  /* Radar-Oberkante (Anteil der Bildhoehe von oben, Original) */
#define CP_FR_BOT 0.955f  /* Frontview-Unterkante inkl. Gehaeuse (Bildschirm 0,93) */
#define CP_W_RADAR 0.80f
#define CP_W_FRONT 0.71f
static float cp_pn_o;     /* senkrechte Verschiebung (NDC) des laufenden Panini-Bildes */
static float cp_pn_lim;   /* d bei 100 % (Rahmengrenze) */
float sb_cp_panini_d(void)
{
    int pn = g_cfg.cockpit_persp_cycle[0] ? cp_pn_now : g_cfg.cockpit_panini;
    float dmax, b, T, c, S, yr, yf, sr = 1.0f, sf = 1.0f, Smax, dlim, d;
    cp_pn_o = 0.0f;
    if (!cp_view || g_cfg.vr_enable || pn <= 0 || !g_bb_w || !g_bb_h) { return 0.0f; }
    dmax = (float)cp_clampi(g_cfg.cockpit_panini_d, 0, 400) / 100.0f;
    if (!g_cfg.cockpit_panini_frame) { return (float)cp_clampi(pn, 0, 100) / 100.0f * dmax; }
    b = g_proj_game.m[5];
    if (!(b > 0.3f && b < 20.0f)) { b = 2.41421356f; }
    T = ((float)g_bb_w / (float)g_bb_h) / b;
    c = cosf(atanf(T));
    if (cp_shown_d != 0.0f || cp_shown_z != 1.0f) { /* camera of the displayed replay */
        sr = cp_shown_kz * CP_W_RADAR / (CP_W_RADAR - cp_shown_d);
        sf = cp_shown_kz * CP_W_FRONT / (CP_W_FRONT - cp_shown_d);
    }
    yr = (1.0f - 2.0f * CP_FR_TOP) * sr;
    yf = (1.0f - 2.0f * CP_FR_BOT) * sf;
    Smax = (yr - yf) > 1e-3f ? 2.0f / (yr - yf) : 1.0f;
    if (Smax <= 1.0f) { return 0.0f; }
    if (Smax * c >= 1.0f) { dlim = dmax; }
    else { dlim = c * (Smax - 1.0f) / (1.0f - Smax * c); if (dlim > dmax) { dlim = dmax; } }
    cp_pn_lim = dlim;
    d = (float)cp_clampi(pn, 0, 100) / 100.0f * dlim;
    if (!(d > 0.0f)) { return 0.0f; }
    S = (d + c) / (c * (d + 1.0f));
    if (S * yr > 1.0f) { cp_pn_o -= S * yr - 1.0f; }
    if (-1.0f - S * yf > 0.0f) { cp_pn_o += -1.0f - S * yf; }
    return d;
}

float sb_cp_panini_offset(void)
{
    return cp_pn_o;
}
