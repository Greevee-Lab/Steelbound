/* render_interp_bb.h - FIX(W347 GHOST) interp_bbcam: Billboard-Paarung per Kamera (reine Mathematik).
 *
 * Baeume sind Quads (n=6/12), die das Spiel je Tick neu zur Kamera ausgerichtet in den Vertex-Puffer schreibt
 * (Matrix gleich, Eingangsdaten neu). Die Zwischenbilder fanden fuer sie keinen Partner (Welt- und Versatz-Test
 * scheitern, Reihenfolge-Partner = anderer Baum) und zeichneten sie exakt in Tick-N-Lage: beim Drehen stehen die
 * Baeume im Spieltakt, der Rest gleitet -> Doppelkanten (Steelbound#45).
 *
 * Ein Billboard ruht in der Welt um seinen Mittelpunkt. Fuer den richtigen Vortick-Partner gilt mit der
 * Kamerabewegung D = M_A^-1 * M_B des Ziels (Zeilenvektoren, Clip-Raum): Schwerpunkt_A * M_A * D ~ Schwerpunkt_B *
 * M_B; die Ecken weichen nur um die Eigendrehung ab. Genutzt von render_interp.c und render_selftest.c. */
#ifndef RENDER_INTERP_BB_H
#define RENDER_INTERP_BB_H

#include <math.h>

#define IPBB_EC 0.01 /* Schwerpunkt: groesste NDC-Abweichung (x/y) zur Kamera-Vorhersage */
#define IPBB_EV 0.05 /* Ecken: groesste NDC-Abweichung (Eigendrehung des Billboards) */

/* out = r * D (Zeilenvektor mal 4x4, zeilenweise) */
static void ipbb_row(const double r[4], const double D[16], double out[4])
{
    int j;
    for (j = 0; j < 4; j++) { out[j] = r[0] * D[j] + r[1] * D[4 + j] + r[2] * D[8 + j] + r[3] * D[12 + j]; }
}

static void ipbb_clip(const float p[4], const float m[16], double c[4])
{
    int j;
    for (j = 0; j < 4; j++) {
        c[j] = (double)p[0] * m[j] + (double)p[1] * m[4 + j] + (double)p[2] * m[8 + j] + (double)p[3] * m[12 + j];
    }
}

/* groesste NDC-Abweichung x/y zweier Clip-Punkte (vor der Kamera), sonst sehr gross */
static double ipbb_ndc(const double p[4], const double q[4])
{
    double dx, dy;
    if (p[3] <= 1e-6 || q[3] <= 1e-6) { return 1e30; }
    dx = fabs(p[0] / p[3] - q[0] / q[3]);
    dy = fabs(p[1] / p[3] - q[1] / q[3]);
    return dx > dy ? dx : dy;
}

/* Clip-Lagen der 4 Bezugs-Vertices und ihr Schwerpunkt (im Clip-Raum linear = Clip des Objektraum-Schwerpunkts) */
static void ipbb_prep(const float p[4][4], const float m[16], double v[4][4], double c[4])
{
    int q, j;
    c[0] = c[1] = c[2] = c[3] = 0.0;
    for (q = 0; q < 4; q++) {
        ipbb_clip(p[q], m, v[q]);
        for (j = 0; j < 4; j++) { c[j] += 0.25 * v[q][j]; }
    }
}

/* Fehler eines vorbereiteten Vortick-Kandidaten (va, ca) fuer den Draw (vb, cb) unter Kamera D: *ec = Schwerpunkt,
 * *ev = groesste Ecke (nur berechnet, wenn der Schwerpunkt passt; sonst sehr gross). */
static void ipbb_err2(const double va[4][4], const double ca[4], const double vb[4][4], const double cb[4],
                      const double D[16], double *ec, double *ev)
{
    double p[4], e;
    int q;
    ipbb_row(ca, D, p);
    *ec = ipbb_ndc(p, cb);
    *ev = 1e30;
    if (*ec > IPBB_EC) { return; }
    *ev = 0.0;
    for (q = 0; q < 4; q++) {
        ipbb_row(va[q], D, p);
        e = ipbb_ndc(p, vb[q]);
        if (e > *ev) { *ev = e; }
    }
}

/* wie ipbb_err2 aus Rohdaten (4 Bezugs-Vertices pa/pb, Matrizen ma/mb) */
static void ipbb_err(const float pa[4][4], const float ma[16], const float pb[4][4], const float mb[16],
                     const double D[16], double *ec, double *ev)
{
    double va[4][4], ca[4], vb[4][4], cb[4];
    ipbb_prep(pa, ma, va, ca);
    ipbb_prep(pb, mb, vb, cb);
    ipbb_err2((const double (*)[4])va, ca, (const double (*)[4])vb, cb, D, ec, ev);
}

/* Kandidat passt: Schwerpunkt ruht und keine Ecke weicht weit ab */
static int ipbb_ok(double ec, double ev) { return ec <= IPBB_EC && ev <= IPBB_EV; }

#endif
