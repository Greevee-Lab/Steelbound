/* render_line_src.h - FIX(ISSUE-EMP,W352, line_scale): HLSL des Linien-Band-
 * Geometry-Shaders (render_line.c) - eigener Kopf, damit render_selftest ihn
 * ohne Renderer-Zustand uebersetzen und auf WARP pruefen kann.
 *
 * Eingang: zwei Vertices einer Linie (LINELIST/LINESTRIP) mit der Ausgabe des
 * IM-VS (pos, col, uv) bzw. des Shading-VS (zusaetzlich spec, t0..t3, fog).
 * Ausgabe: Band aus 2 Dreiecken, 1 Xbox-Pixel breit (halbe Breite LS_HX/LS_HY
 * im NDC des Spiel-Viewports, = 1 / logische Viewport-Groesse), Attribute
 * je Endpunkt unveraendert. Richtung ohne Division durch w (gleiche
 * projektive Gerade auch bei w <= 0), Versatz mal w (feste Bildbreite).
 * Linie ohne Laenge: nichts (wie eine D3D-Linie der Laenge 0). */
#ifndef RENDER_LINE_SRC_H
#define RENDER_LINE_SRC_H
#include <stdio.h>

static const char kLINE_GS[] =
    "#ifdef LS_SHADE\n"
    "#define GSX float4 spec : COLOR1; float4 t0 : TEXCOORD1; float4 t1 : TEXCOORD2; float4 t2 : TEXCOORD3; float4 t3 : TEXCOORD4; float fog : TEXCOORD5;\n"
    "#else\n"
    "#define GSX\n"
    "#endif\n"
    "struct GSV { float4 pos : SV_Position; float4 col : COLOR0; float2 uv : TEXCOORD0; GSX };\n"
    "[maxvertexcount(4)]\n"
    "void main(line GSV i[2], inout TriangleStream<GSV> s) {\n"
    "  float2 d = (i[1].pos.xy * i[0].pos.w - i[0].pos.xy * i[1].pos.w) / float2(LS_HX, LS_HY);\n"
    "  float l = length(d);\n"
    "  if (!(l > 0.0)) return;\n"
    "  float2 o = float2(-d.y, d.x) / l * float2(LS_HX, LS_HY);\n"
    "  GSV v;\n"
    "  v = i[0]; v.pos.xy = i[0].pos.xy - o * i[0].pos.w; s.Append(v);\n"
    "  v = i[0]; v.pos.xy = i[0].pos.xy + o * i[0].pos.w; s.Append(v);\n"
    "  v = i[1]; v.pos.xy = i[1].pos.xy - o * i[1].pos.w; s.Append(v);\n"
    "  v = i[1]; v.pos.xy = i[1].pos.xy + o * i[1].pos.w; s.Append(v);\n"
    "}\n";

/* Quelltext fuer logische Viewport-Groesse w x h (Xbox-Pixel), shade = Shading-Ausgabe. */
static void sb_line_gs_source(char *buf, size_t cap, unsigned w, unsigned h, int shade)
{
    _snprintf_s(buf, cap, _TRUNCATE, "%s#define LS_HX %.9g\n#define LS_HY %.9g\n%s",
                shade ? "#define LS_SHADE 1\n" : "", 1.0 / (double)w, 1.0 / (double)h, kLINE_GS);
}
#endif
