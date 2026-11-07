/* render_cwiden.h - W166 COCKPIT-WIDEN: HLSL-Bausteine (rein, ohne D3D).
 *
 * Cockpit-Draws (Backbuffer, 3D, nur Cockpit-Ansicht) tragen im has_tex-Wort
 * das Bit CW_HAS_BIT. Der Vertex-Shader verschiebt dann jeden Vertex, dessen
 * Bildschirm-x |x/w| jenseits der Naht cw_a liegt, um cw_d (NDC) nach aussen
 * (mit cw_b > 0 linear ueber das Band [cw_a, cw_a + cw_b] eingeblendet).
 * Teile ganz ausserhalb der Naht werden starr verschoben (keine Verzerrung),
 * Teile innerhalb bleiben unveraendert; nur Dreiecke, die die Naht kreuzen,
 * werden gedehnt (die Naht liegt dort, wo keine Instrumente sind).
 * Ohne Bit (Standard, cockpit_widen=0) nimmt der Shader den Zweig nicht:
 * o.pos bleibt exakt das bisherige Ergebnis.
 * Die drei Parameter liegen im b0-Puffer hinter has_tex (Byte 68/72/76,
 * vorher ungenutztes Padding des 80-Byte-Puffers). */
#ifndef RENDER_CWIDEN_H
#define RENDER_CWIDEN_H

#define CW_HAS_BIT 0x20000000u

/* b0-Deklaration fuer Vertex-Shader (Layout wie bisher + 3 Floats im Padding) */
#define CW_HLSL_CB0 \
    "cbuffer CB : register(b0) { row_major float4x4 mvp; uint has_tex; float cw_a; float cw_b; float cw_d; };\n"

/* nach der Berechnung von o.pos einfuegen */
#define CW_HLSL_APPLY \
    "  if ((has_tex & 0x20000000u) != 0u && o.pos.w > 0.0) {\n" \
    "    float cwu = abs(o.pos.x) / o.pos.w;\n" \
    "    float cwt = cw_b > 0.0 ? saturate((cwu - cw_a) / cw_b) : (cwu > cw_a ? 1.0 : 0.0);\n" \
    "    o.pos.x += (o.pos.x < 0.0 ? -cw_d : cw_d) * o.pos.w * cwt; }\n"

#endif
