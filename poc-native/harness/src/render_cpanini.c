/* render_cpanini.c - W191 UW-PERSP: Panini-Projektion der Cockpit-Ansicht als
 * Nachbearbeitung (Grafik-Option [render] cockpit_panini, Standard 0 = aus).
 *
 * Das Spiel zeichnet geradlinig (rektilinear). Die Panini-Abbildung (Sharpless
 * 2010) projiziert die Blickrichtungen erst auf einen Zylinder und den
 * Zylinder dann von einem Punkt im Abstand d hinter der Achse auf den
 * Schirm: senkrechte Linien bleiben senkrecht und gerade, die Bildmitte bleibt
 * formtreu (gleiche Skala in x und y), der Rand wird waagerecht verdichtet.
 * Der Massstab S ist so gewaehlt, dass die Bildraender fest bleiben (kein
 * Inhalt geht seitlich verloren, keine schwarzen Ecken); dafuer waechst die
 * Mitte um S = T(d + cos t)/((d + 1) sin t), t = halber Horizontalwinkel,
 * T = tan t: 16:9 und d = 1 -> 1,12, 21:9 -> 1,20, 32:9 -> 1,39. Oben und
 * unten in der Bildmitte wird um denselben Faktor beschnitten (Panini haelt
 * die Mitte formtreu - ein Bild, das die Mitte vergroessert UND den ganzen
 * Rahmen formtreu zeigt, gibt es nicht: Schwarz-Lemma).
 *
 * Umsetzung: Kopie des gezeigten Bildes, ein Vollbild-Dreieck mit der
 * inversen Abbildung (Schirm -> Quellbild), lineares Sampling. Laeuft an den
 * Stellen der Bildraender-Maske (vor der Maske, vor ESC-Menue/F2): ohne
 * FPS-INTERP auf den Spiel-BB vor dem Screenshot, mit FPS-INTERP im Blit
 * auf jedes gezeigte Bild (auch das Standbild der Pause -> live im ESC-Menue).
 * Alle Inhalte der Cockpit-Ansicht (Mesh, Monitore, HUD, 2D) laufen durch
 * dieselbe Abbildung und bleiben deckungsgleich. Nur Cockpit-Ansicht, nie VR.
 *
 * W235 COCKPIT-WIDTH: optional danach eine waagerechte Streckung um die
 * Bildmitte (Faktor W = 1 + cockpit_width %, p1.y = 1/W); laeuft auch mit
 * d = 0 (Panini-Abbildung dann exakt die Identitaet, S = 1). Inhalt jenseits
 * |x| > 1/W faellt seitlich heraus (Hor+-Rand), keine schwarzen Spalten.
 */
#include "render_int.h"
#include <math.h>
#include "render_post.h" /* W463 RCORE: sb_post_fs_t */

static const char kPN_VS[] =
    "struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
    "O main(uint id : SV_VertexID) { O o; float2 t = float2((id << 1) & 2, id & 2);\n"
    "  o.uv = t; o.p = float4(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, 0.0, 1.0); return o; }\n";
/* p0: T (tan halber Horizontalwinkel), b (vertikale Brennweite), d, S; p1.x: senkrechte Verschiebung (NDC),
 * p1.y: 1 / Breitenfaktor (W235, 1 = aus) */
static const char kPN_PS[] =
    "cbuffer P : register(b0) { float4 p0; float4 p1; };\n"
    "Texture2D t0 : register(t0); SamplerState s0 : register(s0);\n"
    "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
    "  float T = p0.x, b = p0.y, d = p0.z, S = p0.w;\n"
    "  float xs = (uv.x * 2.0 - 1.0) * p1.y * T, ys = ((1.0 - uv.y * 2.0) - p1.x) / b;\n"
    "  float k = xs / (S * (d + 1.0)), k2 = k * k;\n"
    "  float c = (-k2 * d + sqrt(max(1.0 + k2 * (1.0 - d * d), 0.0))) / (1.0 + k2);\n"
    "  float tx = k * (d + c) / c;\n"
    "  float ty = ys * (d + c) / (S * (d + 1.0)) / c;\n"
    "  float2 suv = float2(tx / T * 0.5 + 0.5, 0.5 - ty * b * 0.5);\n"
    "  if (any(suv < 0.0) || any(suv > 1.0)) return float4(0.0, 0.0, 0.0, 1.0);\n"
    "  return float4(t0.SampleLevel(s0, suv, 0).rgb, 1.0); }\n";

/* W463 RCORE: Vollbild-Kopierpass gemeinsam mit Vaporwave (render_post.c) */
static sb_post_fs_t pn = { kPN_VS, kPN_PS, 32, "CPANINI: Ressourcen gescheitert -> cockpit_panini wirkungslos" };

/* Mitte-Vergroesserung S der Panini-Abbildung bei d (Raender fest). */
float sb_cp_panini_scale(float d)
{
    float b = g_proj_game.m[5], T, th;
    if (!(d > 0.0f) || !g_bb_w || !g_bb_h) { return 1.0f; }
    if (!(b > 0.3f && b < 20.0f)) { b = 2.41421356f; }
    T = ((float)g_bb_w / (float)g_bb_h) / b;
    th = atanf(T);
    return T * (d + cosf(th)) / ((d + 1.0f) * sinf(th));
}

/* Panini-Abbildung (d > 0) und/oder W235-Breite (sb_cp_width() > 1) auf rtv/res
 * (Groesse g_bb_w x g_bb_h). Unter ctx_lock. */
void sb_cp_panini_apply(ID3D11RenderTargetView *rtv, ID3D11Resource *res, float d)
{
    float b = g_proj_game.m[5], T, th, S, cb[8], wf = sb_cp_width();
    D3D11_MAPPED_SUBRESOURCE map;
    if (!(d > 0.0f)) { d = 0.0f; }
    if ((d == 0.0f && !(wf > 1.0f)) || !rtv || !res || !g_ctx || !g_bb_w || !g_bb_h) { return; }
    if (!(wf >= 1.0f)) { wf = 1.0f; }
    if (!sb_post_fs_init(&pn) || !sb_post_fs_ensure_copy(&pn, res)) { return; }
    if (!(b > 0.3f && b < 20.0f)) { b = 2.41421356f; }
    T = ((float)g_bb_w / (float)g_bb_h) / b;
    th = atanf(T);
    S = d > 0.0f ? T * (d + cosf(th)) / ((d + 1.0f) * sinf(th)) : 1.0f; /* d = 0: Identitaet (nur Breite) */
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)pn.tex, res);
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)pn.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) { return; }
    cb[0] = T;
    cb[1] = b;
    cb[2] = d;
    cb[3] = S;
    cb[4] = d > 0.0f ? sb_cp_panini_offset() : 0.0f;
    cb[5] = 1.0f / wf; /* W235: Breite (1 = aus, bitgleich zum W191-Pass) */
    cb[6] = cb[7] = 0.0f;
    memcpy(map.pData, cb, sizeof cb);
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)pn.cb, 0);
    sb_post_fs_draw(&pn, rtv, (float)g_bb_w, (float)g_bb_h);
    {
        static int lk = -1, lwk = -1;
        static unsigned lw;
        int k = (int)(d * 1000.0f + 0.5f), wk = (int)(wf * 100.0f + 0.5f);
        static unsigned nlog;
        if ((k != lk || wk != lwk || lw != g_bb_w) && (!g_cfg.cockpit_persp_cycle[0] || nlog < 8u)) {
            nlog++;
            lk = k;
            lwk = wk;
            lw = g_bb_w;
            if (d > 0.0f) {
                HLOG_I("CPANINI d=%.3f bb=%ux%u halber Horizontalwinkel=%.1f Grad -> Mitte x%.3f (Raender fest) senkrecht %+.3f rahmen=%d f=%u",
                       d, g_bb_w, g_bb_h, th * 57.2958f, S, cb[4], g_cfg.cockpit_panini_frame, g_present_no);
            }
            if (wf > 1.0f) { /* W235 */
                HLOG_I("CWIDTH breite x%.2f bb=%ux%u panini d=%.3f: Inhalt |x| > %.3f (%.2f halbe 4:3-Breiten) faellt seitlich heraus f=%u",
                       wf, g_bb_w, g_bb_h, d, 1.0f / wf, (float)g_bb_w / (float)g_bb_h * 0.75f / wf, g_present_no);
            }
        }
    }
}
