/* render_line.c - FIX(ISSUE-EMP,W352, line_scale): Linien in skalierten Spiel-RTs.
 *
 * Die NV2A rastert Linien-Primitive (Begin LINELIST/LINESTRIP, z. B. Funken-
 * und Fallen-Linien FUN_0001D3E0, Stoerzeilen und HUD-Gitter im Monitor)
 * 1 Pixel breit im Xbox-RT (Breite nie gesetzt, D3DRS_LINEWIDTH = 1).
 * Mit Render-Skalierung (rt_scale/auto, W88) ist das D3D11-RT N-mal so gross,
 * D3D11-Linien bleiben aber 1 physisches Pixel breit. Nach dem Verkleinern
 * auf den Monitor tragen sie nur noch ~1/N der Original-Deckung (W352_k4:
 * 1,8-2,4 Pixel je Xbox-Pixel Laenge statt 4x4) - feine Linien wie der
 * EMP-Bogen verschwinden.
 *
 * Abhilfe: Ein Geometry Shader zieht jede Linie im Bildraum zu einem Band
 * von 1 Xbox-Pixel Breite (= N physische Pixel) auf. Breite im NDC des
 * Spiel-Viewports: halbe Breite = 1 / Viewport-Groesse (logisch). Nur fuer
 * Linien in Spiel-RTs mit Faktor >= 1,5; mit rt_scale=1 (Xbox-Groesse,
 * Testlaeufe) und im Backbuffer bleibt alles bitgleich. Zwei Varianten fuer
 * die beiden VS-Ausgaben (IM-Pfad bzw. Shading-Pfad), je Viewport-Groesse
 * einmal uebersetzt (Konstanten im Shader, kein Konstantenpuffer -> auch auf
 * dem Wiedergabe-Kontext der Zwischenbilder ohne Zusatzzustand nutzbar). */
#include "render_int.h"
#include "render_line_src.h" /* kLINE_GS, sb_line_gs_source (auch render_selftest) */

#define LS_MAX 16
typedef struct { unsigned w, h; int shade; ID3D11GeometryShader *gs; } ls_ent_t;
static ls_ent_t g_ls[LS_MAX];
static unsigned g_ls_n;
unsigned g_ls_draws; /* Zaehler (verbreiterte Tick-Draws) */

static ID3D11GeometryShader *ls_get(unsigned w, unsigned h, int shade)
{
    unsigned i;
    char src[2048];
    ID3DBlob *b = NULL;
    ID3D11GeometryShader *gs = NULL;
    for (i = 0; i < g_ls_n; i++) {
        if (g_ls[i].w == w && g_ls[i].h == h && g_ls[i].shade == shade) { return g_ls[i].gs; }
    }
    if (g_ls_n >= LS_MAX || !g_dev) { return NULL; }
    sb_line_gs_source(src, sizeof src, w, h, shade);
    if (sb_im_compile(src, "gs_4_0", &b)) {
        if (FAILED(ID3D11Device_CreateGeometryShader(g_dev, b->lpVtbl->GetBufferPointer(b),
                                                     b->lpVtbl->GetBufferSize(b), NULL, &gs))) { gs = NULL; }
        b->lpVtbl->Release(b);
    }
    g_ls[g_ls_n].w = w; g_ls[g_ls_n].h = h; g_ls[g_ls_n].shade = shade; g_ls[g_ls_n].gs = gs;
    g_ls_n++;
    HLOG_I("LINE-SCALE: GS %ux%u %s %s", w, h, shade ? "Shading" : "IM", gs ? "bereit" : "FEHLER (Linien bleiben 1 px)");
    return gs;
}

/* Tick-Draw: GS fuer diesen Draw (NULL = keiner). Linien-Topologie in ein
 * Spiel-RT mit Render-Skalierung >= 1,5. */
ID3D11GeometryShader *sb_line_gs_for_draw(unsigned topo, int shade)
{
    const rtt_entry_t *r;
    if (!g_cfg.line_scale) { return NULL; }
    if (topo != D3D11_PRIMITIVE_TOPOLOGY_LINELIST && topo != D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP) { return NULL; }
    if (!rtt_active() || g_rtt_bound < 0) { return NULL; }
    r = &g_rtt[g_rtt_bound];
    if (!r->w || !r->h || r->pw * 2u < r->w * 3u || r->ph * 2u < r->h * 3u) { return NULL; }
    if (!g_vp_game.w || !g_vp_game.h) { return NULL; }
    return ls_get(g_vp_game.w, g_vp_game.h, shade ? 1 : 0);
}
