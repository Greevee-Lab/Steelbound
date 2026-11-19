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
