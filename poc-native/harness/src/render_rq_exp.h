/* render_rq_exp.h - W383 LOC-GH-DATA (#62): Strom-2-Expansion der Shading-Rohdaten (RENDER-QA, W77/W80)
 * mit harter Kappung auf den reservierten Platz.
 *
 * Befund (Tester-Log #62, LoC Mission 03, Render-Thread): Schreibzugriff hinter das Ende des gemappten
 * Strom-2-Puffers in rq_expand_upload (memcpy, Eintrag 70828 bei 65536 reservierten). Ursache: Die Schleifen
 * expandierten Dreiecks-/Viereck-Streifen bis IM_VB_MAX*3 Eintraege, rq_expand_count und der Aufrufer kappen
 * die Zahl aber auf IM_VB_MAX. Ein Streifen ab ~21848 Vertices bekam daher nur Platz fuer IM_VB_MAX Eintraege
 * und beschrieb bis zum Doppelten dahinter (Tick-Puffer bzw. Ring, am Ende der Abbildung: Absturz).
 *
 * rq_exp_fill expandiert hoechstens cap Eintraege und nur ganze Primitive (wie im_fast_expand in
 * render_im.c), die ersten Eintraege sind identisch zur ungekappten Expansion. Ohne Kappung (alle SB-Draws)
 * aendert sich nichts.
 *
 * Nur Header (render_selftest prueft Kappung und Reihenfolge). */
#ifndef RENDER_RQ_EXP_H
#define RENDER_RQ_EXP_H

#include <string.h>

/* X: Ziel (cap * stride Bytes), P: gepackte Quell-Vertices (n * stride Bytes), stride 0 = nichts kopieren.
 * Liefert die Zahl geschriebener Eintraege (<= cap). Linien-Schleife (3): n Eintraege + Schluss auf den ersten,
 * falls n + 1 <= lmax (wie rq_expand_count/render_im mit lmax = IM_VB_MAX). */
static __inline unsigned rq_exp_fill(unsigned char *X, const unsigned char *P, unsigned stride, unsigned prim,
                                     unsigned n, unsigned cap, unsigned lmax)
{
    unsigned e = 0, i;
#define RQ_EXP_CP(src) do { if (stride) { memcpy(X + (size_t)e * stride, P + (size_t)(src) * stride, stride); } e++; } while (0)
    switch (prim) {
    case 3: /* LINELOOP */
        for (i = 0; i < n && e < cap; i++) { RQ_EXP_CP(i); }
        if (n > 0 && n + 1 <= lmax && e < cap) { RQ_EXP_CP(0); }
        break;
    case 6: case 7: case 10: /* TRISTRIP / TRIFAN / POLYGON -> Liste */
        for (i = 2; i < n && e + 3u <= cap; i++) {
            if (prim == 6) { RQ_EXP_CP(i - 2); RQ_EXP_CP(i - 1); RQ_EXP_CP(i); }
            else { RQ_EXP_CP(0); RQ_EXP_CP(i - 1); RQ_EXP_CP(i); }
        }
        break;
    case 8: /* QUADLIST -> Liste */
        for (i = 0; i + 3 < n && e + 6u <= cap; i += 4) {
            RQ_EXP_CP(i); RQ_EXP_CP(i + 1); RQ_EXP_CP(i + 2); RQ_EXP_CP(i); RQ_EXP_CP(i + 2); RQ_EXP_CP(i + 3);
        }
        break;
    case 9: /* QUADSTRIP -> Liste */
        for (i = 0; i + 3 < n && e + 6u <= cap; i += 2) {
            RQ_EXP_CP(i); RQ_EXP_CP(i + 1); RQ_EXP_CP(i + 2); RQ_EXP_CP(i); RQ_EXP_CP(i + 2); RQ_EXP_CP(i + 3);
        }
        break;
    default: /* 1, 2, 4, 5, unbekannt: direkt */
        for (i = 0; i < n && e < cap; i++) { RQ_EXP_CP(i); }
        break;
    }
#undef RQ_EXP_CP
    return e;
}

#endif
