/* render_cwiden.c - W166 COCKPIT-WIDEN: Cockpit im Breitbild verbreitern,
 * ohne zu strecken (Grafik-Option, Standard aus).
 *
 * Befund (BERICHT_M2 W166): Das Spiel kennt keinen Breitbild-/720p-Pfad
 * (FUN_0001A540 erzeugt das Device fest mit 640x480; aus XGetVideoFlags wird
 * nur 480p (Bit 3) und PAL60 (Bit 6) gelesen). Im Hor+-Bild endet das
 * Cockpit-Modell seitlich (16:9 knapp, 21:9 deutlich vor dem Bildrand).
 *
 * Verfahren: Nur in der Cockpit-Ansicht (Entscheidung des Spiels in
 * FUN_000459B0 @0x45E3D: Faces + Schale werden gezeichnet) bekommen die
 * 3D-Draws in den Backbuffer (= Cockpit-Modell; Welt/Monitore laufen ueber
 * Render-Targets, 2D/HUD ueber die 4:3-Box) das has_tex-Bit CW_HAS_BIT.
 * Der Vertex-Shader schiebt dann jeden Vertex jenseits der Naht
 * |x/w| > cw_a um cw_d nach aussen (render_cwiden.h). Seitenteile werden
 * dadurch starr verschoben (Instrumente unverzerrt), die Mitte (Hauptmonitor,
 * HUD, Sub-Monitore, 4:3-Inhalt innerhalb der Naht) bleibt unveraendert. Nur
 * Dreiecke, die die Naht kreuzen, werden gedehnt.
 *
 * Masse in "4:3-Einheiten" (1.0 = Kante der 4:3-Box, in NDC = vk):
 *   Naht  = cockpit_widen_seam / 100
 *   Band  = cockpit_widen_band / 100 (0 = harte Stufe an der Naht)
 *   Kante = cockpit_widen_edge / 100 (Ende des Cockpit-Modells im Hor+-Bild)
 *   Verschiebung D = Bildrand - Kante (>= 0), hoechstens cockpit_widen_max/100.
 * Bei 4:3-Ausgabe ist D = 0 (nichts zu tun).
 */
#include "render_int.h"
#include "render_cwiden.h"

float g_cw_par[3];            /* cw_a, cw_b, cw_d (NDC); alles 0 = aus */
static unsigned cw_f = 0xFFFFFFFFu;
static int cw_on;             /* Bild gilt als Cockpit-Ansicht mit D > 0 */
static int cw_view;           /* letzte Ansichts-Entscheidung (Log) */
static float cw_dpx;          /* Verschiebung in BB-Pixeln (Bildraender) */
static int cw_log_k[6];

static unsigned cw_rd32(unsigned a)
{
    unsigned v = 0xFFFFFFFFu;
    __try { v = *(volatile unsigned *)(uintptr_t)a; }
    __except (EXCEPTION_EXECUTE_HANDLER) { v = 0xFFFFFFFEu; }
    return v;
}

static unsigned cw_rd16(unsigned a)
{
    unsigned v = 0xFFFFu;
    __try { v = *(volatile unsigned short *)(uintptr_t)a; }
    __except (EXCEPTION_EXECUTE_HANDLER) { v = 0xFFFEu; }
    return v;
}

/* 1 = das Spiel zeichnet in diesem Bild das Cockpit (FUN_000459B0 @0x45E3D:
 * word[0x346FB0] != 0x80 und (word[0x3516DC] == 0 oder dword[0x3520DC] != 0),
 * nur in der Mission (Szene 0x2C, mloop 1). */
static int cw_cockpit_view(void)
{
    if (cw_rd32(M3D_SCENE_ADDR) != 0x2Cu || cw_rd32(M3D_MLOOP_ADDR) != 1u) { return 0; }
    if (cw_rd16(0x346FB0u) == 0x80u) { return 0; }
    return cw_rd16(0x3516DCu) == 0u || (cw_rd32(0x3520DCu) != 0u && cw_rd32(0x3520DCu) < 0xFFFFFFFEu);
}

static float cw_pct(int v, int lo, int hi)
{
    if (v < lo) { v = lo; }
    if (v > hi) { v = hi; }
    return (float)v / 100.0f;
}

static void cw_eval(void)
{
    float oa, vk, seam, band, edge, dmax, d;
    int view = 0;
    cw_on = 0;
    cw_dpx = 0.0f;
    g_cw_par[0] = g_cw_par[1] = g_cw_par[2] = 0.0f;
    if (!g_cfg.cockpit_widen || g_cfg.vr_enable || !g_bb_w || !g_bb_h) { return; }
    oa = (float)g_bb_w / (float)g_bb_h;
    vk = (4.0f / 3.0f) / oa;              /* halbe 4:3-Boxbreite in NDC */
    seam = cw_pct(g_cfg.cockpit_widen_seam, 0, 200);
    band = cw_pct(g_cfg.cockpit_widen_band, 0, 200);
    edge = cw_pct(g_cfg.cockpit_widen_edge, 100, 400);
    dmax = cw_pct(g_cfg.cockpit_widen_max, 0, 400);
    d = 1.0f / vk - edge;                 /* Bildrand - Modellkante (4:3-Einheiten) */
    if (d > dmax) { d = dmax; }
    if (d <= 0.0f) { d = 0.0f; }
    view = cw_cockpit_view();
    if (g_cfg.cockpit_widen_alt && (g_present_no & 1u) == 0u) { view = 0; } /* DIAG: A/B in Folgebildern */
    cw_view = view;
    if (!view || d <= 0.0f) { return; }
    cw_on = 1;
    g_cw_par[0] = seam * vk;
    g_cw_par[1] = band * vk;
    g_cw_par[2] = d * vk;
    cw_dpx = d * vk * 0.5f * (float)g_bb_w;
}

int sb_cw_draw(void)
{
    if (cw_f != g_present_no) {
        cw_f = g_present_no;
        cw_eval();
        sb_cw_log_frame(g_present_no);
    }
    return cw_on;
}

void sb_cw_edge_box(unsigned *bx, unsigned *bw)
{
    unsigned px, x0, w;
    if (!g_cfg.cockpit_widen || !cw_on || cw_dpx <= 0.0f || cw_f + 2u < g_present_no) { return; } /* nur frischer Zustand */
    px = (unsigned)(cw_dpx + 0.5f);
    x0 = *bx > px ? *bx - px : 0u;
    w = *bw + (*bx - x0) * 2u;
    if (x0 + w > g_bb_w) { w = g_bb_w - x0; }
    *bx = x0;
    *bw = w;
}

void sb_cw_log_frame(unsigned f)
{
    int k[6];
    k[0] = cw_on; k[1] = cw_view; k[2] = (int)g_bb_w; k[3] = (int)g_bb_h;
    k[4] = (int)(g_cw_par[2] * 10000.0f); k[5] = g_cfg.cockpit_widen;
    if (g_cfg.cockpit_widen_alt) { return; } /* A/B-Wechsel je Bild: kein Log-Sturm */
    if (memcmp(k, cw_log_k, sizeof k) == 0) { return; }
    memcpy(cw_log_k, k, sizeof k);
    HLOG_I("CWIDEN on=%d ansicht=%d bb=%ux%u naht=%.3f band=%.3f d=%.3f (NDC) dpx=%.0f modus=%d f=%u",
           cw_on, cw_view, g_bb_w, g_bb_h, g_cw_par[0], g_cw_par[1], g_cw_par[2], cw_dpx,
           g_cfg.cockpit_widen, f);
}
