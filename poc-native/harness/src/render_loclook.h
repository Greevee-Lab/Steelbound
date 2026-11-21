#pragma once
/* render_loclook.h - W358 LOC-LOOK: reine Rechenregeln der LoC-Bildkorrekturen (ohne D3D/Spielspeicher),
 * genutzt von render_shade.c und geprueft von render_selftest (Fall LOCLOOK).
 *
 * 1. loc_vsod0: P2-Vertexprogramme (kind 2: DP4-oPos + Rest) der LoC-Modelle rechnen das Vertex-Licht
 *    selbst (NORMSHORT3-Normale, 50..82 Befehle); oD0 muss dann aus dem Programm kommen.
 * 2. loc_texfilt: MIPMAPLODBIAS des Spiels gilt auf der NV2A gegen das Xbox-Raster (Backbuffer 480 Zeilen
 *    bzw. Xbox-RT-Groesse). Bei Render-Skalierung s waehlt die GPU die Mip-Stufe schon um log2(s) feiner;
 *    gleiche Texeldichte je Spielpixel wie das Original = Bias + log2(s), nie unschaerfer als ohne Bias.
 *    Ergebnis in 1/16-Schritten (Sampler-Cache-Schluessel). */
#include <math.h>

/* 1 = oD0 aus dem Programm (sonst Eingangsfarbe des alten Pfads). kind 0 wie bisher immer. */
static __inline int rq_loc_od0_from_prog(int kind, int loc, int vsod0, unsigned shade_mask)
{
    if (!(shade_mask & 2u)) { return 0; }
    return kind == 0 || (kind == 2 && loc && vsod0);
}

/* LOD-Bias in 1/16 (mode 1/3 = kompensiert, 2 = roh). game_bias = Spielwert (float aus TSS 6). */
static __inline int rq_loc_bias16(float game_bias, float scale, int mode)
{
    float b = (game_bias == game_bias && game_bias > -16.0f && game_bias < 16.0f) ? game_bias : 0.0f;
    float g = b;
    if (mode == 1 || mode == 3) {
        if (scale > 1.0f) { b += (float)(log((double)scale) * 1.4426950408889634); }
        if (b > 0.0f && g <= 0.0f) { b = 0.0f; }
    }
    return (int)(b * 16.0f + (b < 0.0f ? -0.5f : 0.5f));
}

/* Mip-Filter fuer den Sampler: mode 1 = immer linear (2), sonst Spielwert (0 NONE, 1 POINT, 2 LINEAR). */
static __inline unsigned rq_loc_mipfilter(unsigned game_mip, int mode)
{
    if (mode == 1) { return 2u; }
    return game_mip > 2u ? 2u : game_mip;
}

/* W363 LOC-ORIGINAL (loc_swznorm): Texel-Koordinaten (Skala 1/w, 1/h) adressiert die NV2A nur bei linearen
 * Formaten (LIN_*); swizzled Texturen sind immer normiert, auch bei vortransformierten Draws. Alte Regel
 * (SB, loc_swznorm=0): Texel-Skala auch fuer swizzled, wenn vortransformiert und UV > 1,5.
 * Rueckgabe 1 = Texel-Skala anwenden. rtscale = RT-Regel (loc_rtuv), mfx_norm = SB-Monitor-Wash (W121). */
static __inline int rq_loc_texel_scale(int loc, int swznorm, int isrt, int lin, int rhw, int rtscale, int mfx_norm, float tmax)
{
    int swz = loc && swznorm && !lin && !isrt;
    return rtscale || (!mfx_norm && !swz && tmax > 1.5f && (lin || rhw));
}

/* W368 LOC-COCKPIT (loc_lintexel): Rechnet das Vertexprogramm oTk selbst (kein reines "mov oTk, vN",
 * tsrc < 0), ist der UV-Bereich auf der CPU unbekannt (tmax = 0) und die Regel oben greift nie. Die NV2A
 * adressiert lineare Formate aber immer mit Texel-Koordinaten. LoC zeichnet die Cockpit-Anzeigen
 * (Waffen, Munition, Schalterlampen, Instrumentenfelder) aus dem CPU-gerasterten Linear-Atlas 512x256 mit
 * einem solchen Programm (oT0 = Texel-UV); normiert gesampelt klemmte CLAMP alles auf das Eckentexel ->
 * flache lila Flaechen. Rueckgabe 1 = Texel-Skala zusaetzlich anwenden. kind 1 (P1) normiert die CPU schon. */
static __inline int rq_loc_lin_progscale(int loc, int lintexel, int isrt, int lin, int rhw, int kind, int prog_uv)
{
    return loc && lintexel && lin && !isrt && !rhw && kind != 1 && prog_uv;
}
