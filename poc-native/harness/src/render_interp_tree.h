/* render_interp_tree.h - FIX(W506 ISSUE-105) loc_tree_fix: Baeume in LoC-Zwischenbildern (reine Mathematik, ohne D3D).
 *
 * Ein ruhender Weltpunkt, den Tick N an clip_B zeigt, lag im Vortick an clip_B * Dw (Dw = D^-1 = Kamera der Welt,
 * Tick N -> Vortick, W415). Ein Zwischenbild-Partner (Vortick-Lage clip_A der Bezugs-Vertices) gehoert zum selben ruhenden
 * Objekt, wenn clip_A ~ clip_B * Dw (bis auf die Eigendrehung eines zur Kamera gedrehten Billboards). Zeilenvektoren wie
 * im Spiel (clip = p * M). Genutzt von render_interp_match.c (w506_tree_fix) und render_selftest.c ("TREE506"). */
#ifndef RENDER_INTERP_TREE_H
#define RENDER_INTERP_TREE_H
#include <math.h>

#define IPT_TOL0 0.003   /* NDC: Grundtoleranz Partner passt zur Kamera der Welt */
#define IPT_TOLX 0.05    /* Anteil der NDC-Ausdehnung (Eigendrehung grosser naher Billboards) */
#define IPT_REST 0.01    /* NDC: anderer Vortick-Draw passt als ruhendes Objekt */

static void ipt_clip(const float p[4], const float m[16], double c[4])
{
    int j;
    for (j = 0; j < 4; j++) {
        c[j] = (double)p[0] * m[0 * 4 + j] + (double)p[1] * m[1 * 4 + j] + (double)p[2] * m[2 * 4 + j] + (double)p[3] * m[3 * 4 + j];
    }
}

/* Tick-N-Bezugs-Vertices pb*mb, wie die Kamera der Welt sie im Vortick zeigt (cw = clip_B * Dw); ext = NDC-Ausdehnung der
 * sichtbaren Vertices in Tick N. Rueckgabe: Anzahl sichtbarer Vertices (w > 0) in Tick N. */
static int ipt_world(const float pb[4][4], const float mb[16], const double Dw[16], double cw[4][4], double *ext)
{
    double mn[2] = { 1e30, 1e30 }, mx[2] = { -1e30, -1e30 };
    int q, k, nvis = 0;
    for (q = 0; q < 4; q++) {
        double cb[4];
        ipt_clip(pb[q], mb, cb);
        for (k = 0; k < 4; k++) { cw[q][k] = cb[0] * Dw[0 * 4 + k] + cb[1] * Dw[1 * 4 + k] + cb[2] * Dw[2 * 4 + k] + cb[3] * Dw[3 * 4 + k]; }
        if (cb[3] > 1e-6) {
            nvis++;
            for (k = 0; k < 2; k++) {
                double v = cb[k] / cb[3];
                if (v < mn[k]) { mn[k] = v; }
                if (v > mx[k]) { mx[k] = v; }
            }
        }
    }
    *ext = nvis ? fmax(mx[0] - mn[0], mx[1] - mn[1]) : 0.0;
    return nvis;
}

/* groesste NDC-Abweichung (x/y) der Vortick-Lage pa*ma gegen cw; Vertices hinter der Kamera nicht bewertet; 1e9 = keiner */
static double ipt_dev(const float pa[4][4], const float ma[16], const double cw[4][4])
{
    double mx = 0.0;
    int q, nvis = 0;
    for (q = 0; q < 4; q++) {
        double ca[4];
        if (cw[q][3] <= 1e-6) { continue; }
        ipt_clip(pa[q], ma, ca);
        if (ca[3] <= 1e-6) { continue; }
        nvis++;
        mx = fmax(mx, fmax(fabs(ca[0] / ca[3] - cw[q][0] / cw[q][3]), fabs(ca[1] / ca[3] - cw[q][1] / cw[q][3])));
    }
    return nvis ? mx : 1e9;
}

/* Toleranz "Partner passt zur Kamera der Welt" */
static double ipt_tol(double ext) { return IPT_TOL0 + IPT_TOLX * ext; }

/* Entscheidung fuer einen gelerpten Draw: 1 = falscher Partner (weicht ab, ein anderer Vortick-Draw passt als ruhendes
 * Objekt deutlich besser) -> mit der Kamera der Welt bewegen; 0 = Partner behalten (passt, oder bewegtes Objekt) */
static int ipt_wrong_partner(double dev, double tol, double best_other)
{
    return dev > tol && best_other <= IPT_REST && best_other < 0.5 * dev;
}
#endif
