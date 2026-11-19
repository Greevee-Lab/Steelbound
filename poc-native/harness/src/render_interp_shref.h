/* render_interp_shref.h - W361 LOC-INTERP-CRASH: eigene Referenzen einer Zwischenbild-Liste auf die
 * Schatten-Views (rtv/dsv/srv) eines Schatten-Eintrags plus dessen Generation beim Sichern.
 *
 * Befund (Minidump W358 P0, 120 Hz): Der Render-Thread der Zwischenbilder band per Deferred-Context eine
 * bereits zerstoerte Schatten-SRV (d3d11 CShaderResourceView::CLS::FinalConstruct ->
 * AllocateDependentCLS<CResource> liest die Ressource = NULL). Die Ops tragen die Schatten-Views als rohe
 * Zeiger; gehoerten sie nur dem Eintrag, reichte eine einzige Luecke in dessen Lebensdauer (Verdraengung,
 * fremde Freigabe) fuer einen Absturz. Mit diesen Referenzen bleiben die Views gueltige Objekte, solange die
 * Liste lebt; die Generation macht einen ersetzten Eintrag sichtbar (Diagnose, Bild unveraendert).
 *
 * Nur Header (render_selftest prueft die Semantik mit einem WARP-Device). */
#ifndef RENDER_INTERP_SHREF_H
#define RENDER_INTERP_SHREF_H

#include <d3d11.h>

typedef struct {
    ID3D11View *v[3]; /* rtv, dsv, srv (je AddRef, NULL = keiner) */
    unsigned gen;     /* Generation des Eintrags beim Sichern (0 = keiner) */
} ip_shref_t;

/* Views des Eintrags selbst halten und die Generation merken. */
static __inline void ip_shref_take(ip_shref_t *r, ID3D11View *rtv, ID3D11View *dsv, ID3D11View *srv, unsigned gen)
{
    int q;
    r->v[0] = rtv;
    r->v[1] = dsv;
    r->v[2] = srv;
    for (q = 0; q < 3; q++) {
        if (r->v[q]) { ID3D11View_AddRef(r->v[q]); }
    }
    r->gen = gen;
}

/* Eigene Referenzen abgeben (mehrfacher Aufruf unschaedlich). */
static __inline void ip_shref_drop(ip_shref_t *r)
{
    int q;
    for (q = 0; q < 3; q++) {
        if (r->v[q]) { ID3D11View_Release(r->v[q]); r->v[q] = NULL; }
    }
    r->gen = 0u;
}

/* 1 = Eintrag ist noch derselbe wie beim Sichern (cur_gen = aktuelle Generation des Eintrags). */
static __inline int ip_shref_same(const ip_shref_t *r, unsigned cur_gen)
{
    return r->gen != 0u && r->gen == cur_gen;
}

#endif
