#pragma once
/* render_interp_eye.h - W448 LOC-CHAFF-LOOK (GitHub #83): reine Regel ohne D3D/Spielspeicher, genutzt von
 * render_interp.c (ip_pair) und geprueft von render_selftest (Fall EYE448).
 *
 * Befund: Beim Chaff wirft der VT Huelsen (gleiches Netz, n = 66, je Instanz eine Matrix) dicht an der Kamera vorbei.
 * ip_plausible liess den Matrix-Lerp eines solchen Draws zu, wenn alle 4 Bezugs-Vertices in beiden Ticks hinter der
 * Kamera lagen ("unsichtbar"). Die Bezuege sind aber nur 4 von 66 Vertices; die Huelse schneidet die Augenebene bzw.
 * die Instanz-Zuordnung wechselt zwischen zwei Huelsen. Im Zwischenbild entstanden grosse, harte graue/weisse
 * Vierecke ueber der Sicht (nur mit Zwischenbildern, nie im Tick-Bild).
 * Regel: Netz (> 8 Vertices) mit allen Bezuegen in beiden Ticks hinter der Kamera -> exakt aus Tick N. Liegt es
 * wirklich ganz hinter der Kamera, ist exakt genauso unsichtbar wie gelerpt (kein Bildunterschied); sonst ist exakt
 * der sichere Weg (wie bei "reject"). Partikel-Quads (<= 8 Vertices) bleiben unberuehrt. */

static __inline void ip_eye_clip(const float p[4], const float m[16], float c[4])
{
    int j;
    for (j = 0; j < 4; j++) {
        c[j] = p[0] * m[0 * 4 + j] + p[1] * m[1 * 4 + j] + p[2] * m[2 * 4 + j] + p[3] * m[3 * 4 + j];
    }
}

/* 1 = exakt zeichnen (alle 4 Bezuege in beiden Ticks hinter der Kamera, Netz mit gleicher Vertexzahl > 8). */
static __inline int ip_eye_blind(const float pa[4][4], const float ma[16], unsigned na,
                                 const float pb[4][4], const float mb[16], unsigned nb)
{
    int q;
    if (nb <= 8u || na != nb) { return 0; }
    for (q = 0; q < 4; q++) {
        float c0[4], c1[4];
        ip_eye_clip(pa[q], ma, c0);
        ip_eye_clip(pb[q], mb, c1);
        if (!(c0[3] < -1e-6f && c1[3] < -1e-6f)) { return 0; }
    }
    return 1;
}
