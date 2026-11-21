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
/* FIX(SB-HUD-SMEAR): logische Bezugsgroesse des Bands fuer eine Linie in ein Spiel-RT (logisch rw x rh, D3D11
 * pw x ph). Der Client bindet fuer jedes RT den D3D11-Viewport ueber das ganze RT (rtt_bind) und bildet einen
 * Teil-Viewport des Spiels in die Positionen ab (rt_vp_matrix bzw. Bildschirm-Pixel bei RHW/VSH-P1). NDC ueberspannt
 * also immer das ganze RT. W352 nahm den Spiel-Viewport (g_vp_game): Bei Teil-Viewports wie dem Kompassstreifen
 * (192x24 im 512er Hauptmonitor) wurde das Band 512/192 = 2,7 bzw. 512/24 = 21 Xbox-Pixel breit (gruene/orange
 * Kloetze unten im Hauptmonitor), beim Welt-Viewport 512x422 senkrecht 21 % zu breit. 0 = kein Band (Faktor < 1,5:
 * bitgleich wie bisher). */
static int sb_line_rt_dims(unsigned rw, unsigned rh, unsigned pw, unsigned ph, unsigned *w, unsigned *h)
{
    if (!rw || !rh || pw * 2u < rw * 3u || ph * 2u < rh * 3u) { return 0; }
    *w = rw;
    *h = rh;
    return 1;
}
/* W381 (GitHub #59, loc_line_bb): Linien im Backbuffer. LoC zeichnet die Welt und ihre Effekte (Funken-/Explosions-
 * Linien) direkt in den Backbuffer, nicht in ein Spiel-RT. Der Backbuffer ist um s = Bildhoehe / Framebuffer-Hoehe
 * groesser als das Xbox-Bild (Hor+ und 4:3-Box halten die Hoehe; bei hohem Fenster begrenzt die Breite), D3D11-Linien
 * bleiben 1 physisches Pixel breit (bei 1440 Zeilen nur 1/3 der Deckung). Liefert die logische Groesse, mit der der
 * Band-GS (sb_line_gs_source) bezogen auf den physischen Viewport vpw x vph genau s physische Pixel (= 1 Xbox-Pixel)
 * breit zeichnet: halbe Breite im NDC = s / vp. 0 = kein Band (s < 1,5: bitgleich wie bisher). */
static int sb_line_bb_dims(float vpw, float vph, unsigned bb_w, unsigned bb_h, unsigned fb_h, float disp_aspect,
                           unsigned *w, unsigned *h)
{
    double s1, s2, s;
    if (!fb_h || !bb_w || !bb_h || !(disp_aspect > 0.0f) || !(vpw >= 1.0f) || !(vph >= 1.0f)) { return 0; }
    s1 = (double)bb_h / (double)fb_h;
    s2 = (double)bb_w / ((double)fb_h * (double)disp_aspect);
    s = s1 < s2 ? s1 : s2;
    if (s < 1.5 - 1e-4) { return 0; } /* Toleranz: Seitenverhaeltnis als float */
    *w = (unsigned)((double)vpw / s + 0.5);
    *h = (unsigned)((double)vph / s + 0.5);
    return *w && *h;
}
#endif
