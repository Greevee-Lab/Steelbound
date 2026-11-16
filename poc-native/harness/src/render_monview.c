/* render_monview.c - W319 MONITOR-VIEW: Monitoransicht (experimentell,
 * [render] monitor_view, Standard 0 = Original bitgleich).
 *
 * Befund (BERICHT_M2 W319 §1): Cockpit und flache Vollbild-Ansicht des Spiels
 * (FUN_000297C0, Kniff IGNITION+START bzw. cockpit_hide) zeichnen dasselbe
 * Monitor-RT (Welt-RT -> Komposition PS ..0950 mit Helligkeitskurve, Bloom,
 * Fokusmaske, Nachtsicht -> HUD, Korn, Stoerzeilen). Verschieden ist nur der
 * letzte Schritt in den Backbuffer:
 *  - Cockpit: Monitor-Netz (gewoelbt, ~1755 Vertices) mit PS (V0+C0)*T0,
 *    V0 = Cockpit-Licht je Vertex, C0 = Grundlicht -> Farbe/Glanz des Glases,
 *    Ausschnitt eines breiten Streifens des RT.
 *  - Flach: 640x480-Quad, C0 = 0, V0 = 1 -> RT unveraendert, 4:3-Rahmen.
 *
 * Die Monitoransicht nimmt deshalb den Monitor-Draw des Spiels selbst: In der
 * Cockpit-Ansicht (nach der VT-Verriegelung, ohne Treffer-Cockpit) werden die
 * Monitor-Netze im Clip-Raum gleichmaessig vergroessert und mittig gelegt
 * (gleiche Skala in x und y = nie gestreckt, schwarze Balken), alle anderen
 * Draws in den Backbuffer (Schale, Instrumente, Lampen, Glas) entfallen.
 * Licht, Farbe, Korn, Stoerung, Nachtsicht und HUD bleiben so exakt wie im
 * Cockpit. monitor_view=2 zeigt zusaetzlich Radar (oben rechts) und FRONT VIEW
 * (unten links) als kleine Felder - wie die Vollbild-Ansicht des Spiels.
 *
 * Erkennung: Stufe-0-Textur = Monitor-RT-Objekt des Spiels (EU 0x352B5C
 * Hauptmonitor, 0x35234C FRONT VIEW, 0x352BE8 Radar; SBA fuer US). Weggelassen
 * wird erst, wenn der Hauptmonitor im Bild davor gefunden wurde (sonst bliebe
 * ein schwarzes Bild): Faellt die Erkennung aus, bleibt das Cockpit sichtbar.
 *
 * Die Aenderung geschieht vor der Aufzeichnung (render_im.c), wirkt also
 * gleich in 30 FPS, Zwischenbildern (FPS-INTERP), Foto und Schnell-Screenshot.
 * Nicht in VR, Replay/Titel-Demo/Video-Export (dort gilt die Kamera der
 * Wiedergabe), LoC und vor der Verriegelung (Startsequenz braucht das Cockpit).
 */
#include "render_int.h"
#include "sb_profile.h" /* SBA() */
#include <math.h>

#define MV_MAIN 0
#define MV_FRONT 1
#define MV_RADAR 2

static unsigned mv_f = 0xFFFFFFFFu;
static int mv_live;          /* dieses Bild: Option wirksam (Cockpit-Ansicht, verriegelt) */
static unsigned mv_seen_f;   /* Bild (+1), in dem der Hauptmonitor zuletzt gezeichnet wurde */
static int mv_skip_ok;       /* dieses Bild: Hauptmonitor kuerzlich gesehen -> andere Draws weglassen */
static int mv_state_log = -1;
static unsigned mv_n_keep, mv_n_skip[3], mv_n_mon[3];
static float mv_main_rect[4]; /* Hauptmonitor im Bild nach der Abbildung (NDC x0,y0,x1,y1) */
static unsigned mv_main_rect_f;

static unsigned mv_rd32(unsigned a)
{
    unsigned v = 0xFFFFFFFFu;
    __try { v = *(volatile unsigned *)(uintptr_t)a; }
    __except (EXCEPTION_EXECUTE_HANDLER) { v = 0xFFFFFFFEu; }
    return v;
}

static unsigned mv_rd16(unsigned a)
{
    unsigned v = 0xFFFFu;
    __try { v = *(volatile unsigned short *)(uintptr_t)a; }
    __except (EXCEPTION_EXECUTE_HANDLER) { v = 0xFFFEu; }
    return v;
}

/* Live-Mission in der Cockpit-Ansicht des Spiels (FUN_000459B0 @0x45E3D: Cockpit,
 * wenn word[0x346FB0] != 0x80 und word[0x3516DC] == 0), VT verriegelt
 * [0x53831C] == 1, kein Treffer-/Ausfall-Cockpit [0x3520DC] == 0, keine
 * Titel-Demo/Replay [0x3B8CDC] != 1, keine Wiedergabe [0x3B8CC0] != 1. */
static int mv_eval(void)
{
    if (g_cfg.monitor_view <= 0 || g_cfg.vr_enable || g_sba_loc) { return 0; }
    if (mv_rd32(M3D_SCENE_ADDR) != 0x2Cu || mv_rd32(M3D_MLOOP_ADDR) != 1u) { return 0; }
    if (mv_rd32(SBA(0x3B8CDCu)) == 1u || mv_rd32(SBA(0x3B8CC0u)) == 1u) { return 0; }
    if (mv_rd16(SBA(0x346FB0u)) == 0x80u || mv_rd16(SBA(0x3516DCu)) != 0u) { return 0; }
    if (mv_rd32(SBA(0x3520DCu)) != 0u || mv_rd32(SBA(0x53831Cu)) != 1u) { return 0; }
    return 1;
}

int sb_mv_frame(void)
{
    if (g_cfg.monitor_view <= 0 && mv_state_log <= 0) { return 0; } /* Standard: kein Speicherzugriff */
    if (mv_f != g_present_no) {
        mv_f = g_present_no;
        if (g_cfg.monitor_view_diag && mv_live && (g_present_no % 600u) == 0u) {
            HLOG_I("MONVIEW zaehler (Bild %u): monitor haupt=%u front=%u radar=%u, weggelassen 3d=%u p2=%u 2d=%u, behalten 2d=%u",
                   g_present_no, mv_n_mon[0], mv_n_mon[1], mv_n_mon[2], mv_n_skip[0], mv_n_skip[1], mv_n_skip[2], mv_n_keep);
        }
        memset(mv_n_mon, 0, sizeof mv_n_mon);
        memset(mv_n_skip, 0, sizeof mv_n_skip);
        mv_n_keep = 0;
        mv_live = mv_eval();
        mv_skip_ok = mv_live && mv_seen_f && mv_seen_f + 3u >= g_present_no;
        if (mv_live != mv_state_log) {
            if (mv_state_log >= 0 || mv_live) {
                HLOG_I("MONVIEW: %s (monitor_view=%d, f=%u)", mv_live ? "Monitoransicht aktiv" : "Cockpit/andere Ansicht",
                       g_cfg.monitor_view, g_present_no);
            }
            mv_state_log = mv_live;
        }
    }
    return mv_live;
}

/* 1 = Monitoransicht in diesem Bild sichtbar (fuer Bildraender/Panini): nur mit
 * gefundenem Hauptmonitor. */
int sb_mv_on(void)
{
    return g_cfg.monitor_view > 0 && mv_live && mv_skip_ok;
}

static int mv_kind(unsigned xp)
{
    if (!xp) { return -1; }
    if (xp == SBA(0x352B5Cu)) { return MV_MAIN; }
    if (xp == SBA(0x35234Cu)) { return MV_FRONT; }
    if (xp == SBA(0x352BE8u)) { return MV_RADAR; }
    return -1;
}

/* NDC-Rahmen der sichtbaren Vertices (w > 0) */
static int mv_bbox(const float mvp[16], const float *pos, size_t st, unsigned n, float b[4])
{
    unsigned i, k = 0;
    b[0] = b[1] = 1e9f; b[2] = b[3] = -1e9f;
    if (!pos) { return 0; }
    for (i = 0; i < n; i++) {
        const float *p = pos + (size_t)i * st;
        float X = p[0] * mvp[0] + p[1] * mvp[4] + p[2] * mvp[8] + p[3] * mvp[12];
        float Y = p[0] * mvp[1] + p[1] * mvp[5] + p[2] * mvp[9] + p[3] * mvp[13];
        float W = p[0] * mvp[3] + p[1] * mvp[7] + p[2] * mvp[11] + p[3] * mvp[15];
        float x, y;
        if (!(W > 1e-4f)) { continue; }
        x = X / W; y = Y / W;
        if (!(x > -50.0f && x < 50.0f && y > -50.0f && y < 50.0f)) { continue; }
        if (x < b[0]) { b[0] = x; }
        if (y < b[1]) { b[1] = y; }
        if (x > b[2]) { b[2] = x; }
        if (y > b[3]) { b[3] = y; }
        k++;
    }
    return k >= 3u && b[2] > b[0] && b[3] > b[1];
}

/* Clip-Raum-Abbildung: Rahmen b auf das Ziel t (beides NDC) mit EINER Skala in
 * Pixeln (x und y gleich), zentriert in t. mvp' = mvp * Z (Zeilenvektoren). */
static void mv_fit(float mvp[16], const float b[4], const float t[4], float out[4])
{
    float bw = (float)(g_bb_w ? g_bb_w : 1), bh = (float)(g_bb_h ? g_bb_h : 1);
    float wpx = (b[2] - b[0]) * 0.5f * bw, hpx = (b[3] - b[1]) * 0.5f * bh;
    float twp = (t[2] - t[0]) * 0.5f * bw, thp = (t[3] - t[1]) * 0.5f * bh;
    float k = twp / wpx, ky = thp / hpx, cx, cy, tx, ty;
    int i;
    if (ky < k) { k = ky; }
    cx = (b[0] + b[2]) * 0.5f; cy = (b[1] + b[3]) * 0.5f;
    tx = (t[0] + t[2]) * 0.5f; ty = (t[1] + t[3]) * 0.5f;
    /* x' = k*x + (tx - k*cx)*w  (in NDC ist die Pixel-Skala je Achse konstant) */
    for (i = 0; i < 4; i++) {
        float w = mvp[i * 4 + 3];
        mvp[i * 4] = k * mvp[i * 4] + (tx - k * cx) * w;
        mvp[i * 4 + 1] = k * mvp[i * 4 + 1] + (ty - k * cy) * w;
    }
    if (out) {
        out[0] = tx - k * (cx - b[0]); out[2] = tx + k * (b[2] - cx);
        out[1] = ty - k * (cy - b[1]); out[3] = ty + k * (b[3] - cy);
    }
}

static void mv_diag_skip(int cls, unsigned xp, unsigned n, const float mvp[16], const float *pos, size_t st)
{
    static unsigned nlog, seen[48], nseen;
    unsigned key = xp * 2654435761u ^ (unsigned)cls ^ (n << 8), i;
    float b[4] = { 0, 0, 0, 0 };
    if (!g_cfg.monitor_view_diag || nlog >= 48u) { return; }
    for (i = 0; i < nseen; i++) { if (seen[i] == key) { return; } }
    if (nseen < 48u) { seen[nseen++] = key; }
    nlog++;
    mv_bbox(mvp, pos, st, n, b);
    HLOG_I("MONVIEW weg #%u f=%u cls=%d xp=0x%X vs=0x%X ps=0x%X n=%u ndc=[%.2f,%.2f]x[%.2f,%.2f]",
           nlog, g_present_no, cls, xp, g_im_shader, g_im_pshader, n, b[0], b[2], b[1], b[3]);
}

/* Je Draw in den Backbuffer bei aktiver Monitoransicht. Rueckgabe 1 = Draw
 * auslassen, 0 = zeichnen (mvp ggf. angepasst). cls wie sb_cp_draw. */
int sb_mv_draw(float mvp[16], int cls, const float *pos, size_t st, unsigned n, unsigned xp)
{
    int kind = mv_kind(xp);
    float b[4];
    if (kind == MV_MAIN && cls != 2 && mv_bbox(mvp, pos, st, n, b)) {
        static const float full[4] = { -1.0f, -1.0f, 1.0f, 1.0f };
        static unsigned nlog;
        mv_seen_f = g_present_no + 1u;
        mv_n_mon[0]++;
        if (!mv_skip_ok) { return 0; } /* erstes Bild: noch Cockpit (keine Luecke ohne Monitor) */
        mv_fit(mvp, b, full, mv_main_rect);
        mv_main_rect_f = g_present_no + 1u;
        if (g_cfg.monitor_view_diag && (nlog < 3u || (g_present_no % 1800u) == 0u)) {
            nlog++;
            HLOG_I("MONVIEW haupt f=%u n=%u ndc=[%.3f,%.3f]x[%.3f,%.3f] -> [%.3f,%.3f]x[%.3f,%.3f] bb=%ux%u", g_present_no, n,
                   b[0], b[2], b[1], b[3], mv_main_rect[0], mv_main_rect[2], mv_main_rect[1], mv_main_rect[3], g_bb_w, g_bb_h);
        }
        return 0;
    }
    if (!mv_skip_ok) { return 0; }
    if ((kind == MV_FRONT || kind == MV_RADAR) && cls != 2 && g_cfg.monitor_view >= 2 && mv_main_rect_f &&
        mv_main_rect_f + 3u >= g_present_no + 1u && mv_bbox(mvp, pos, st, n, b)) {
        /* kleine Felder im Bild des Hauptmonitors: Radar oben rechts, FRONT VIEW unten links
         * (Lage wie in der Vollbild-Ansicht des Spiels), je 30 % der Bildhoehe */
        const float *r = mv_main_rect;
        float hs = (r[3] - r[1]) * 0.30f, ws = hs * (float)g_bb_h / (float)(g_bb_w ? g_bb_w : 1) * 1.40f;
        float mx = (r[2] - r[0]) * 0.02f, my = (r[3] - r[1]) * 0.03f, t[4];
        if (kind == MV_RADAR) { t[2] = r[2] - mx; t[0] = t[2] - ws; t[3] = r[3] - my; t[1] = t[3] - hs; }
        else { t[0] = r[0] + mx; t[2] = t[0] + ws; t[1] = r[1] + my; t[3] = t[1] + hs; }
        mv_n_mon[kind]++;
        mv_fit(mvp, b, t, NULL);
        return 0;
    }
    if (cls == 2) { mv_n_keep++; return 0; } /* 2D (RHW) in den BB: Blenden/Ueberblendungen des Spiels bleiben */
    mv_n_skip[cls == 1 ? 1 : 0]++;
    mv_diag_skip(cls, xp, n, mvp, pos, st);
    return 1;
}
