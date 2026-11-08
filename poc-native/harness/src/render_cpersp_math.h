/* W209: shared live/replay camera math; CPU-only regression tests. */
#ifndef SB_RENDER_CPERSP_MATH_H
#define SB_RENDER_CPERSP_MATH_H

/* 3D-Pfad: Objektraum-Positionen mit w = 1 -> Zeile 3 der MVP ist die
 * Verschiebung. Spalte W = Abstand (metrisch), Spalte Z = al*W + be (aus der
 * MVP selbst bestimmt, kleinste Quadrate ueber die Zeilen 0..2). Vorschub d:
 * W' = W - d (mvp[15] -= d), Z' = al*W' + be (mvp[14] -= al*d), Sichtfeld
 * k*zoom auf die Spalten X/Y. Rueckgabe 0 = keine Perspektiv-MVP (unveraendert
 * bis auf Zoom). */
/* Tiefe nach dem Kamera-Weg: z/w = al + be / w (al = f/(f-n), be = -n*al).
 * Kamera zurueck (d < 0): Near-Ebene auf n - d = Original-Near um das alte Auge
 * -> genau die Geometrie, die das Original sah (nichts hinter dem alten Auge:
 * Sitz, Rueckwand, Kopfstuetze bleiben weg), bessere Tiefenpraezision.
 * Kamera vor (d > 0): Projektion unveraendert. al0/be0 = Original. */
static void cp_depth_new(float al0, float be0, float d, float *al1, float *be1)
{
    float n, f, n1;
    if (!(d < 0.0f) || !(al0 > 1.0f) || !(be0 < 0.0f)) { *al1 = al0; *be1 = be0; return; }
    n = -be0 / al0;
    f = n * al0 / (al0 - 1.0f);
    n1 = n - d;
    if (!(f > n1 * 1.01f)) { *al1 = al0; *be1 = be0; return; }
    *al1 = f / (f - n1);
    *be1 = -n1 * *al1;
}

/* MVP eines 3D-Draws (Objektraum w = 1) vom Kamera-Weg d0/Faktor kz0 auf d1/kz1
 * umrechnen (Aufnahme: d0 = 0, kz0 = 1; Pause-Haken: Differenz). al0/be0 =
 * Original-Tiefe (aus der MVP bestimmt, falls NULL-Zeiger nicht gesetzt). */
static int cp_mvp_redo(float m[16], float d0, float kz0, float d1, float kz1, float *al_out, float *be_out)
{
    float ww = m[3] * m[3] + m[7] * m[7] + m[11] * m[11], al, be, al0, be0, aln, ben, fx;
    int i;
    if (!(ww > 1e-12f)) { return 0; }
    al = (m[2] * m[3] + m[6] * m[7] + m[10] * m[11]) / ww; /* Tiefe der vorliegenden MVP */
    be = m[14] - al * m[15];
    /* Original-Tiefe zurueckrechnen (d0 < 0 hatte die Near-Ebene verschoben) */
    al0 = al; be0 = be;
    if (d0 < 0.0f && al > 1.0f && be < 0.0f) {
        float n1 = -be / al, f = n1 * al / (al - 1.0f), n = n1 + d0;
        if (n > 1e-4f && f > n * 1.01f) { al0 = f / (f - n); be0 = -n * al0; }
    }
    fx = kz1 / kz0;
    for (i = 0; i < 4; i++) { m[i * 4 + 0] *= fx; m[i * 4 + 1] *= fx; }
    m[15] += d0 - d1;                       /* W' = W - d */
    cp_depth_new(al0, be0, d1, &aln, &ben);  /* Z' = al' W' + be' */
    m[2] = aln * m[3]; m[6] = aln * m[7]; m[10] = aln * m[11]; m[14] = aln * m[15] + ben;
    if (al_out) { *al_out = al0; }
    if (be_out) { *be_out = be0; }
    return 1;
}

static unsigned cp_p2_apply(float *pos, size_t st, unsigned n,
                           float d, float k, float al, float be, float near0)
{
    unsigned i, moved = 0;
    float al1, be1;
    if (d == 0.0f) { return 0; }
    cp_depth_new(al, be, d, &al1, &be1);
    for (i = 0; i < n; i++) {
        float *p = pos + (size_t)i * st;
        float w = p[3], zn, wm, w1, s;
        if (!(w > 0.0f)) { continue; }
        zn = p[2] / w;
        if (!(zn > 0.2f && zn < 0.999f)) { continue; }
        wm = be / (zn - al);
        if (!(wm > 0.0f)) { continue; }
        w1 = wm - d;
        if (w1 < near0 * 1.05f) { w1 = near0 * 1.05f; }
        s = k * wm / w1;
        p[0] *= s;
        p[1] *= s;
        p[2] = (al1 + be1 / w1 - 2e-5f) * w;
        moved++;
    }
    return moved;
}

#endif
