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
 * D3D11-Viewports (= ganzes RT, SB-HUD-SMEAR): halbe Breite = 1 / logische
 * RT-Groesse. Nur fuer
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
unsigned g_ls_bb_draws; /* W381 (#59): davon LoC-Backbuffer-Linien (loc_line_bb) */

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
    if (!rtt_active() || g_rtt_bound < 0) { /* Backbuffer */
        D3D11_VIEWPORT vp;
        UINT nvp = 1;
        unsigned lw = 0, lh = 0;
        if (!g_sba_loc || !g_cfg.loc_line_bb || !g_ctx) { return NULL; } /* W381 (#59): nur LoC (SB bitgleich) */
        ID3D11DeviceContext_RSGetViewports(g_ctx, &nvp, &vp);
        if (!nvp || !sb_line_bb_dims(vp.Width, vp.Height, g_bb_w, g_bb_h, g_game_fb_h, g_game_disp_aspect, &lw, &lh)) {
            return NULL;
        }
        g_ls_bb_draws++;
        if (g_ls_bb_draws <= 2u || (g_ls_bb_draws & (g_ls_bb_draws - 1u)) == 0u && g_ls_bb_draws <= 65536u) {
            HLOG_I("LINE-SCALE: LoC-Backbuffer-Linie #%u (Viewport %.0fx%.0f -> logisch %ux%u, Bild %u) f=%u", g_ls_bb_draws,
                   vp.Width, vp.Height, lw, lh, g_game_fb_h, g_present_no);
        }
        return ls_get(lw, lh, shade ? 1 : 0);
    }
    r = &g_rtt[g_rtt_bound];
    { /* FIX(SB-HUD-SMEAR): Bezug = ganzes RT (D3D11-Viewport), nicht der Spiel-Viewport (render_line_src.h) */
        unsigned lw = 0, lh = 0;
        if (!sb_line_rt_dims(r->w, r->h, r->pw, r->ph, &lw, &lh)) { return NULL; }
        return ls_get(lw, lh, shade ? 1 : 0);
    }
}
