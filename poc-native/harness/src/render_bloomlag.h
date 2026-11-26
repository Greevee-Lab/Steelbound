/* render_bloomlag.h - W507 LOC-BLOOM-LAG (GitHub #107): Zeitverhalten des spieleigenen LoC-Bloom - reine
 * Regeln (ohne D3D, ohne Speicherzugriff), Selbsttest render_selftest Fall BLOOMLAG.
 *
 * Befund (Ghidra/Capstone, LoC-VAs, nur gelesen): FUN_00034270 (Hauptbild, Ziel 14x14) und FUN_00034680
 * (Lock-On-Kamera, 9x9) zeichnen je Spielbild (20 Hz) zuerst ein schwarzes Quad mit Alpha-Blend
 * SRCALPHA/INVSRCALPHA, Alpha = [0x307044] bzw. [0x3071A8] (Mission 0x14 = 20/255), danach additiv
 * (ONE/ONE) das Mittel von 4 gedrehten Abtastungen der 256^2-Glow-Textur:
 *     B(n) = B(n-1) * (1 - a) + G(n)
 * Ein laufender Mittelwert mit Zeitkonstante 1/a = 12,8 Bilder = 0,64 s; nach einem Blickwechsel vom Boden
 * zum Himmel steht der braune Bodenschein noch ~1 s im Bloom (nach 1 s 20 %, nach 2 s 4 %). Die
 * Komposition FUN_00034B00 addiert B auf das Hauptbild. Das ist Original-Verhalten (Xbox: gleicher Code,
 * gleicher 20-Hz-Takt, 8-Bit-Ziel); Enhanced/Vivid verstaerken B (bloom_strength 130/150 ueber der Schwelle
 * 20 % -> 1,6- bzw. 1,9-fach) und machen den Nachlauf deutlich sichtbar.
 *
 * Fix (nur Enhanced-Bloom, d. h. bloom_strength != 100 oder bloom_threshold > 0, Original unberuehrt):
 * k Abklingschritte je Bild in einem Draw:
 *     a' = 1 - (1 - a)^k          (Abkling-Quad: Ausgabe-Alpha im PS)
 *     c  = a' / a                 (additiver Draw: Ausgabe-RGB * c im PS)
 * Gleichgewicht G / a' * c = G / a bleibt exakt (gleiche Helligkeit des Bloom, gleiche Saettigung: wo
 * c*G > 1 klemmt, war G/a schon > 1/a' >= 1 und das Original ebenfalls gesaettigt), die Zeitkonstante
 * sinkt auf 1/(k*a) Bilder. Reine Darstellung: nur Bloom-Ziele, kein Spielzustand, kein Sync. */
#ifndef RENDER_BLOOMLAG_H
#define RENDER_BLOOMLAG_H

#include <math.h>

#define BLOOMLAG_DECAY 1u  /* Abkling-Quad (SRCALPHA/INVSRCALPHA) in ein LoC-Bloom-Ziel */
#define BLOOMLAG_ADD 2u    /* additiver Draw (ONE/ONE) in ein LoC-Bloom-Ziel */
#define BLOOMLAG_KMAX 16

/* Klasse eines Draws: LoC, Ziel <= 32^2 (Bloom 14^2 / Kamera 9^2), Blend an; on/src/dst = PC-D3DBLEND
 * (2 ONE, 5 SRCALPHA, 6 INVSRCALPHA) wie sb_render_blend_state. */
static unsigned bloomlag_class(int loc, unsigned w, unsigned h, unsigned on, unsigned src, unsigned dst)
{
    if (!loc || !w || !h || w > 32u || h > 32u || !on) { return 0u; }
    if (src == 5u && dst == 6u) { return BLOOMLAG_DECAY; }
    if (src == 2u && dst == 2u) { return BLOOMLAG_ADD; }
    return 0u;
}

/* Wirksamer Faktor k: 0/1 = aus; nur mit Enhanced-Bloom (Original-Bloom bleibt bitgleich). */
static int bloomlag_k(int loc, int fast, int bloom_pct, int bloom_thr)
{
    if (!loc || fast <= 1 || (bloom_pct == 100 && bloom_thr <= 0)) { return 1; }
    return fast > BLOOMLAG_KMAX ? BLOOMLAG_KMAX : fast;
}

/* a8 = Abklingwert des Spiels (Alpha 1..255). 0 (kein Abklingen) bzw. ungueltig -> 0.0 = Original lassen. */
static float bloomlag_a(unsigned a8)
{
    return (a8 >= 1u && a8 <= 255u) ? (float)a8 / 255.0f : 0.0f;
}

static float bloomlag_alpha(float a, int k)
{
    return 1.0f - powf(1.0f - a, (float)k);
}

static float bloomlag_comp(float a, int k)
{
    return a > 0.0f ? bloomlag_alpha(a, k) / a : 1.0f;
}

#endif
