/* render_visqz.h - FIX(SB-SUN-WHITEOUT,W477): Flare-Testpunkt hinter der hinteren Ebene.
 *
 * Die Licht-Flare-Tests des Spiels (FUN_0001B900/0001BA10, Indizes 0x3EFE..0x3FFD, render_visq.c)
 * zeichnen EINEN vortransformierten Punkt (P2, rhw, w = 1) an der Bildposition der Sonne mit dem
 * Tiefentest des Spiels (ZFUNC LEQUAL). Steht die Sonne hinter der Kamera, liefert die Projektion
 * eine gespiegelte Bildposition mit z > 1 (gemessen M10: 1,0013). Auf der NV2A besteht ein Pixel
 * mit z > 1 einen LESS/LEQUAL/EQUAL-Test gegen den Tiefenpuffer (Werte <= 1) nie -> Ergebnis 0.
 * D3D11 schneidet ihn mit DepthClipEnable weg (NVIDIA: 0 von 30 501 Tests sichtbar), manche
 * AMD-Treiber klemmen z aber auf 1,0 -> besteht gegen den geleerten Himmel (1,0) -> Flare
 * "sichtbar", das Spiel zeichnet sein additives Sonnen-Flare an die gespiegelte Stelle und der
 * Hauptmonitor ueberstrahlt weiss (Tester RX 570: 722 von 27 001 sichtbar).
 * Rein (ohne D3D), damit render_selftest es pruefen kann. */
#ifndef RENDER_VISQZ_H
#define RENDER_VISQZ_H

/* 1 = Punkt-Draw im Sichtbarkeitstest kann auf der NV2A keinen Pixel liefern (alle z > 1 bei
 * ZFUNC LESS 0x201 / EQUAL 0x202 / LEQUAL 0x203, GL-Werte des NV2A-Registers 0x354), sonst 0.
 * NaN zaehlt nicht als "dahinter" (bleibt wie bisher). */
static __inline int sb_visqz_behind_far(const float *z, unsigned stride, unsigned n, unsigned zfunc)
{
    unsigned i;
    zfunc &= 0xFFFFu;
    if (!z || n == 0u || (zfunc != 0x201u && zfunc != 0x202u && zfunc != 0x203u)) { return 0; }
    for (i = 0; i < n; i++) {
        if (!(z[(unsigned long)i * stride] > 1.0f)) { return 0; }
    }
    return 1;
}

#endif
