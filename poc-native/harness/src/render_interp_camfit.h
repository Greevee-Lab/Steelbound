/* render_interp_camfit.h - W349 LOC-FRAMES-2: Kamera-Pruefung der Zwischenbild-Zuordnung (rein, ohne D3D).
 *
 * Fuer einen in der Welt ruhenden Punkt p gilt clip_B = clip_A * D mit D = M_A^-1 * M_B einer Kamera-Referenz
 * des Ziels (Zeilenvektoren). Neu gebaute Billboards/Himmelsgitter (neue Eingangsdaten, gleiche Vertex-Reihenfolge)
 * und Gelaendestreifen knapp ueber der Matrix-Schwelle liegen so nahe an der Vorhersage. Fehler je Bezugs-Vertex
 * in x/y durch |w| (vor der Kamera = NDC; dahinter gleich gerechnet, das w-Vorzeichen und |w| (25 %) muessen passen).
 * Rueckgabe: groesster Fehler relativ zur NDC-Ausdehnung der sichtbaren Bezugs-Vertices von B (mind. 0,05 NDC);
 * >= 1e9 = unbrauchbar. Genutzt von render_interp.c (li_camfit*) und render_selftest.c ("CAMFIT"). */
#ifndef RENDER_INTERP_CAMFIT_H
#define RENDER_INTERP_CAMFIT_H
#include <math.h>

static void ipcf_clip(const float p[4], const float m[16], double c[4])
{
    int j;
    for (j = 0; j < 4; j++) {
        c[j] = (double)p[0] * m[0 * 4 + j] + (double)p[1] * m[1 * 4 + j] + (double)p[2] * m[2 * 4 + j] + (double)p[3] * m[3 * 4 + j];
    }
}

/* pa/ma: 4 Bezugs-Vertices und Matrix des Vorticks (bzw. Tick-N-Vertices mit M_A fuer den Matrix-Lerp-Test),
 * pb/mb: Tick N. D: nD Kamera-Referenzen (je 16 double). */
static double ipcf_camfit(const float pa[4][4], const float ma[16], const float pb[4][4], const float mb[16],
                          const double (*D)[16], int nD)
{
    double best = 1e9, c0[4][4], c1[4][3], mn[2] = { 1e30, 1e30 }, mx[2] = { -1e30, -1e30 }, ext;
    int q, k, x, nvis = 0;
    for (q = 0; q < 4; q++) {
        double b0[4];
        ipcf_clip(pa[q], ma, c0[q]);
        ipcf_clip(pb[q], mb, b0);
        if (fabs(b0[3]) <= 1e-6) { return 1e9; }
        c1[q][0] = b0[0]; c1[q][1] = b0[1]; c1[q][2] = b0[3];
        if (b0[3] > 0.0) {
            nvis++;
            for (k = 0; k < 2; k++) {
                double v = b0[k] / b0[3];
                if (v < mn[k]) { mn[k] = v; }
                if (v > mx[k]) { mx[k] = v; }
            }
        }
    }
    ext = nvis ? (mx[0] - mn[0] > mx[1] - mn[1] ? mx[0] - mn[0] : mx[1] - mn[1]) : 1.0;
    if (ext < 0.05) { ext = 0.05; } /* kleine/entfernte Objekte: Fehler gegen mind. 0,05 NDC */
    for (x = 0; x < nD; x++) {
        double er = 0.0;
        for (q = 0; q < 4 && er < best; q++) {
            double p[4], aw = fabs(c1[q][2]);
            for (k = 0; k < 4; k++) {
                p[k] = c0[q][0] * D[x][0 * 4 + k] + c0[q][1] * D[x][1 * 4 + k] + c0[q][2] * D[x][2 * 4 + k] + c0[q][3] * D[x][3 * 4 + k];
            }
            if ((p[3] > 0.0) != (c1[q][2] > 0.0) || fabs(p[3] - c1[q][2]) > 0.25 * aw + 1e-3) { er = 1e9; break; }
            for (k = 0; k < 2; k++) {
                double e1 = fabs(p[k] - c1[q][k]) / aw / ext;
                if (e1 > er) { er = e1; }
            }
        }
        if (er < best) { best = er; }
    }
    return best;
}
#endif
