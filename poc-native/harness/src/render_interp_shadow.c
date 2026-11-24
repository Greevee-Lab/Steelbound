/* render_interp_shadow.c - FPS-INTERP Schatten-Ziele, Pipeline-Zustand sichern/herstellen, Konstanten-Ring, Geometrie-Ausschnitt.
 * W463 RCORE: aus render_interp.c ausgegliedert (reines Verschieben); gemeinsamer Zustand in
 * render_interp_int.h, Modulkarte docs/RENDER_ARCH.md. */
#include "render_interp_int.h"
static unsigned ip_sh_gen; /* W361: Generationszaehler der Schatten-Eintraege */

void ip_sh_free(ip_sh_t *e)
{
    int i;
    if (e->res) { ID3D11Resource_Release(e->res); }
    { /* W361: Zusatz-Referenzen der Zaehlerwache mit abgeben */
        ID3D11View *vw[3];
        int q;
        vw[0] = (ID3D11View *)e->rtv; vw[1] = (ID3D11View *)e->dsv; vw[2] = (ID3D11View *)e->srv;
        for (q = 0; q < 3; q++) { while (vw[q] && e->pin[q]) { ID3D11View_Release(vw[q]); e->pin[q]--; } }
    }
    if (e->rtv) { ID3D11RenderTargetView_Release(e->rtv); }
    if (e->dsv) { ID3D11DepthStencilView_Release(e->dsv); }
    if (e->srv) { ID3D11ShaderResourceView_Release(e->srv); }
    if (e->sh) { ID3D11Texture2D_Release(e->sh); }
    for (i = 0; i < IP_LISTS; i++) {
        if (e->bak[i]) { ID3D11Texture2D_Release(e->bak[i]); }
    }
    memset(e, 0, sizeof *e);
}

static int ip_sh_find(ID3D11Resource *res)
{
    int i, free_i = -1;
    D3D11_TEXTURE2D_DESC dd;
    ip_sh_t *e;
    HRESULT hr = S_OK;
    for (i = 0; i < IP_SH_MAX; i++) {
        if (g_ish[i].res == res) { return i; }
        if (!g_ish[i].res && free_i < 0) { free_i = i; }
    }
    if (free_i < 0) {
        /* Eintrag ohne gueltige Sicherung einer lebenden Liste verdraengen */
        unsigned best = 0xFFFFFFFFu;
        for (i = 0; i < IP_SH_MAX; i++) {
            int live = 0, k;
            for (k = 0; k < IP_LISTS; k++) {
                if (g_ipl[k].serial && g_ish[i].bakser[k] == g_ipl[k].serial) { live = 1; }
            }
            if (!live && g_ish[i].used < best) { best = g_ish[i].used; free_i = i; }
        }
        if (free_i < 0) { return -1; }
        ip_rt_quiesce(); /* W126 RENDER-THREAD */
        ip_sh_free(&g_ish[free_i]);
    }
    e = &g_ish[free_i];
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)res, &dd);
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &dd, NULL, &e->sh)) || !e->sh) {
        memset(e, 0, sizeof *e);
        return -1;
    }
    if (dd.BindFlags & D3D11_BIND_RENDER_TARGET) {
        hr = ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)e->sh, NULL, &e->rtv);
    }
    {   /* FIX(SHADOWBUF,W127): typlose Tiefentextur (Schattenpuffer, W100:
         * R24G8_TYPELESS mit DSV + SRV) braucht explizite View-Formate - mit
         * NULL-Beschreibung schlug die Schattenkopie fehl, jede Tick-Liste mit
         * Tiefenpass war snap_ok=0 und wurde verworfen (W100_oni2: pairs=0).
         * Typisierte Ressourcen wie bisher (NULL). */
        DXGI_FORMAT vs = DXGI_FORMAT_UNKNOWN, vd = DXGI_FORMAT_UNKNOWN;
        if (dd.Format == DXGI_FORMAT_R24G8_TYPELESS) { vs = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; vd = DXGI_FORMAT_D24_UNORM_S8_UINT; }
        else if (dd.Format == DXGI_FORMAT_R32_TYPELESS) { vs = DXGI_FORMAT_R32_FLOAT; vd = DXGI_FORMAT_D32_FLOAT; }
        else if (dd.Format == DXGI_FORMAT_R16_TYPELESS) { vs = DXGI_FORMAT_R16_UNORM; vd = DXGI_FORMAT_D16_UNORM; }
        if (SUCCEEDED(hr) && (dd.BindFlags & D3D11_BIND_SHADER_RESOURCE)) {
            D3D11_SHADER_RESOURCE_VIEW_DESC sv;
            memset(&sv, 0, sizeof sv);
            sv.Format = vs;
            sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sv.Texture2D.MipLevels = dd.MipLevels ? dd.MipLevels : 1u;
            hr = ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)e->sh,
                                                       vs != DXGI_FORMAT_UNKNOWN ? &sv : NULL, &e->srv);
        }
        if (SUCCEEDED(hr) && (dd.BindFlags & D3D11_BIND_DEPTH_STENCIL)) {
            D3D11_DEPTH_STENCIL_VIEW_DESC dv;
            memset(&dv, 0, sizeof dv);
            dv.Format = vd;
            dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            hr = ID3D11Device_CreateDepthStencilView(g_dev, (ID3D11Resource *)e->sh,
                                                     vd != DXGI_FORMAT_UNKNOWN ? &dv : NULL, &e->dsv);
        }
    }
    for (i = 0; SUCCEEDED(hr) && i < IP_LISTS; i++) {
        hr = ID3D11Device_CreateTexture2D(g_dev, &dd, NULL, &e->bak[i]);
    }
    if (FAILED(hr)) {
        static unsigned nfail; /* W127: Fehlschlag sichtbar (vorher stumm -> Liste verworfen) */
        if (nfail++ < 8u) {
            HLOG_W("INTERP Schatten fehlgeschlagen: %ux%u fmt=%u bind=0x%X hr=0x%08X", dd.Width, dd.Height,
                   (unsigned)dd.Format, dd.BindFlags, (unsigned)hr);
        }
        ip_sh_free(e);
        return -1;
    }
    e->res = res;
    ID3D11Resource_AddRef(res);
    if (++ip_sh_gen == 0u) { ip_sh_gen = 1u; } /* W361 */
    e->gen = ip_sh_gen;
    if (g_cfg.interp_diag >= 1) {
        static unsigned nlog;
        if (nlog++ < 48u) {
            HLOG_I("INTERP Schatten #%d: %ux%u fmt=%u bind=0x%X", free_i, dd.Width, dd.Height,
                   (unsigned)dd.Format, dd.BindFlags);
        }
    }
    return free_i;
}

/* Ressource der View fuer Liste L (Index li) sichern (einmal je Liste),
 * Eintrag liefern. need_rt: Textur -> nur Render-Ziele haben Schatten. */
static int ip_sh_ref(ip_list_t *L, int li, ID3D11View *v, int need_rt)
{
    ID3D11Resource *res = NULL;
    int e, j;
    if (!v) { return -1; }
    ID3D11View_GetResource(v, &res);
    if (!res) { L->snap_ok = 0; return -1; }
    if (need_rt) {
        D3D11_TEXTURE2D_DESC dd;
        D3D11_RESOURCE_DIMENSION dim;
        ID3D11Resource_GetType(res, &dim);
        if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) { ID3D11Resource_Release(res); return -1; }
        ID3D11Texture2D_GetDesc((ID3D11Texture2D *)res, &dd);
        /* FIX(SHADOWBUF,W127): Schattenpuffer (Tiefen-RT als Textur der Stufe 1)
         * bekommt ebenfalls einen Schatten - die Wiedergabe sampelt dann den in
         * der Wiedergabe gezeichneten Tiefenpass statt des Spielstands. */
        if (!(dd.BindFlags & D3D11_BIND_RENDER_TARGET) &&
            !(g_cfg.shade_shadow && (dd.BindFlags & D3D11_BIND_DEPTH_STENCIL))) { ID3D11Resource_Release(res); return -1; }
    }
    e = ip_sh_find(res);
    if (e < 0) { ID3D11Resource_Release(res); L->snap_ok = 0; return -1; }
    for (j = 0; j < L->nrefs && L->refs[j] != e; j++) {}
    if (j == L->nrefs) {
        if (L->nrefs >= IP_REFS_MAX) { ID3D11Resource_Release(res); L->snap_ok = 0; return -1; }
        /* W361: Views selbst halten, Generation merken (Abgabe in ip_list_unref) */
        ip_shref_take(&L->refo[L->nrefs], (ID3D11View *)g_ish[e].rtv, (ID3D11View *)g_ish[e].dsv,
                      (ID3D11View *)g_ish[e].srv, g_ish[e].gen);
        L->refs[L->nrefs++] = e;
        if (g_cfg.imlock_diag) { /* DIAG(LOC-IM-LOCK) */
            LONGLONG cq0 = ip_now();
            ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)g_ish[e].bak[li], res);
            isw_cp_q += ip_now() - cq0; isw_cp_n++;
        } else
        ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)g_ish[e].bak[li], res);
        g_ish[e].bakser[li] = L->serial;
        g_ish[e].used = L->serial;
    }
    ID3D11Resource_Release(res);
    return e;
}

/* DIAG(W340 LOC-FRAMES): Texturen ohne Schatten, die Draws sampeln (bis 24 verschiedene, je Ressource einmal) */
static void li_srv_note(const ip_op_t *o, unsigned i)
{
    static void *seen[24];
    static unsigned nseen;
    ID3D11Resource *res = NULL;
    D3D11_TEXTURE2D_DESC dd;
    D3D11_RESOURCE_DIMENSION dim;
    unsigned k;
    if (nseen >= 24u || !o->srv) { return; }
    ID3D11View_GetResource((ID3D11View *)o->srv, &res);
    if (!res) { return; }
    for (k = 0; k < nseen && seen[k] != (void *)res; k++) {}
    if (k == nseen) {
        ID3D11Resource_GetType(res, &dim);
        memset(&dd, 0, sizeof dd);
        if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) { ID3D11Texture2D_GetDesc((ID3D11Texture2D *)res, &dd); }
        seen[nseen++] = (void *)res;
        HLOG_I("LOCINTERP srv ohne Schatten #%u: op=%u %ux%u fmt=%u bind=0x%X ziel=%s cls=%u n=%u f=%u", nseen, i, dd.Width,
               dd.Height, (unsigned)dd.Format, dd.BindFlags, o->rtv == g_rtv ? "BB" : "RT", o->cls, o->nv, g_present_no);
    }
    ID3D11Resource_Release(res);
}

/* Am Swap: alle Ressourcen der Liste sichern, Schatten-Views eintragen. */
/* LOC-IM-LOCK im_lock_split=1: Vorab-Einordnung der Textur-Quellen fuer ip_list_snap OHNE ctx_lock (Spiel-Thread am
 * Anfang von sb_interp_swap). Messung (imlock_diag, Zeile IMLOCK swap): ip_list_snap hielt die Sperre je Swap 1-2 ms
 * (bis 10 ms), fast nur fuer GetResource/GetType/GetDesc/Release je Draw-Textur, um festzustellen, dass eine
 * gewoehnliche Textur keinen Schatten braucht (ip_sh_ref need_rt=1 -> -1 ohne Seiteneffekt). Diese Abfragen betreffen
 * nur Device-Objekte (frei-threaded), keinen Context; die Aufnahmeliste L gehoert dem Spiel-Thread (nicht in der
 * Historie, der Present-Takt liest sie nicht), sb_interp_reset laeuft nur auf diesem Thread. Ergebnis je Op: Bit 0 =
 * srv ohne Schatten, Bit k = rq-Stufe k ohne Schatten; ip_list_snap ueberspringt dann genau diese ip_sh_ref-Aufrufe
 * (gleiches Ergebnis, gleiche Liste). */
static unsigned char ip_snap_cls[IP_OPS_MAX];
static const ip_list_t *ip_snap_cls_L;
static unsigned ip_snap_cls_n;
static int ip_view_no_shadow(ID3D11View *v) /* 1 = ip_sh_ref(..., need_rt=1) liefert sicher -1 ohne Seiteneffekt */
{
    ID3D11Resource *res = NULL;
    D3D11_RESOURCE_DIMENSION dim;
    D3D11_TEXTURE2D_DESC dd;
    int r;
    ID3D11View_GetResource(v, &res);
    if (!res) { return 0; } /* ip_sh_ref entscheidet (snap_ok = 0) */
    ID3D11Resource_GetType(res, &dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) { r = 1; }
    else {
        ID3D11Texture2D_GetDesc((ID3D11Texture2D *)res, &dd);
        r = !(dd.BindFlags & D3D11_BIND_RENDER_TARGET) && !(g_cfg.shade_shadow && (dd.BindFlags & D3D11_BIND_DEPTH_STENCIL));
    }
    ID3D11Resource_Release(res);
    return r;
}
void ip_snap_classify(const ip_list_t *L)
{
    unsigned i, n = L->nops;
    ID3D11View *lv = NULL;
    int lr = 0;
    ip_snap_cls_L = NULL;
    if (n > IP_OPS_MAX) { return; }
    for (i = 0; i < n; i++) {
        const ip_op_t *o = &L->ops[i];
        unsigned char c = 0;
        if (o->kind != IP_COPY) {
            if (o->srv) {
                if ((ID3D11View *)o->srv != lv) { lv = (ID3D11View *)o->srv; lr = ip_view_no_shadow(lv); }
                if (lr) { c |= 1u; }
            }
            if (o->rq) {
                int k2;
                for (k2 = 1; k2 < 4; k2++) {
                    if (o->rqr.srv[k2] && ip_view_no_shadow((ID3D11View *)o->rqr.srv[k2])) { c |= (unsigned char)(1u << k2); }
                }
            }
        }
        ip_snap_cls[i] = c;
    }
    ip_snap_cls_n = n;
    ip_snap_cls_L = L;
}

void ip_list_snap(ip_list_t *L, int li)
{
    unsigned i;
    ID3D11View *lv[3] = { NULL, NULL, NULL };
    int le[3] = { -1, -1, -1 };
    const int cls = (ip_snap_cls_L == L && ip_snap_cls_n == L->nops); /* LOC-IM-LOCK: Vorab-Einordnung gilt */
    LONGLONG uq0 = g_cfg.imlock_diag ? ip_now() : 0;
    ip_snap_cls_L = NULL;
    L->snap_ok = 1;
    ip_list_unref(L); /* W361 */
    if (uq0) { isw_un_q += ip_now() - uq0; }
    L->nrefs = 0;
    L->bbref = ip_sh_ref(L, li, (ID3D11View *)g_rtv, 0);
    for (i = 0; i < L->nops; i++) {
        ip_op_t *o = &L->ops[i];
        ID3D11View *vv[3];
        int q;
        /* LOC-IM-LOCK im_lock_split=1: alle 64 Ops dem Present-Takt die Sperre ueberlassen, wenn er wartet (sb_ctx_yield:
         * nur aeusserste Ebene). Sicher wie die Zuordnung ohne Sperre (swap_match_unlock): L ist noch nicht in der Historie,
         * Schreibzugriffe gehen nur auf Eintraege/Sicherungen von L (bak[li]); der Takt-Thread stellt den Pipeline-Zustand
         * nach seiner Wiedergabe wieder her. Messung: Snap 1-2 ms (bis 10 ms) je Swap am Stueck. */
        if (g_cfg.im_lock_split > 0 && (i & 63u) == 63u) { sb_ctx_yield(); }
        if (o->kind == IP_COPY) { /* W340: Quelle = Schatten des BB, Ziel = Schatten der Textur (braucht RT-Bindung) */
            o->cp_es = ip_sh_ref(L, li, (ID3D11View *)o->cps, 0);
            o->cp_ed = ip_sh_ref(L, li, (ID3D11View *)o->cpd, 1);
            if (o->cp_es >= 0 && o->cp_ed >= 0) { InterlockedIncrement(&li_cp_ok); }
            continue;
        }
        vv[0] = (ID3D11View *)o->rtv;
        vv[1] = (ID3D11View *)o->dsv;
        vv[2] = (ID3D11View *)o->srv;
        o->sh_rtv = NULL;
        o->sh_dsv = NULL;
        o->sh_srv = o->srv;
        for (q = 0; q < 3; q++) {
            int e;
            if (!vv[q]) { continue; }
            if (vv[q] == lv[q]) { e = le[q]; } /* gleiche View wie zuvor (lebt in der Liste) */
            else if (q == 2 && cls && (ip_snap_cls[i] & 1u)) { e = -1; lv[q] = vv[q]; le[q] = e; } /* LOC-IM-LOCK: kein RT */
            else if (g_cfg.imlock_diag) { LONGLONG gq0 = ip_now(); e = ip_sh_ref(L, li, vv[q], q == 2); isw_gr_q += ip_now() - gq0; isw_gr_n++; lv[q] = vv[q]; le[q] = e; } /* DIAG */
            else { e = ip_sh_ref(L, li, vv[q], q == 2); lv[q] = vv[q]; le[q] = e; }
            if (q == 0) { o->sh_rtv = (e >= 0) ? g_ish[e].rtv : NULL; }
            else if (q == 1) { o->sh_dsv = (e >= 0) ? g_ish[e].dsv : NULL; }
            else if (e >= 0) { o->sh_srv = g_ish[e].srv; }
            else if (g_sba_loc && g_cfg.interp_diag >= 1) { li_srv_note(o, i); } /* DIAG(W340) */
        }
        if (o->rq) { /* RENDER-QA: Stufen 1..3 (RT-Texturen bekommen Schatten) */
            int k2;
            for (k2 = 1; k2 < 4; k2++) {
                int e2;
                o->rq_shsrv[k2] = o->rqr.srv[k2];
                if (!o->rqr.srv[k2]) { continue; }
                if (cls && (ip_snap_cls[i] & (1u << k2))) { continue; } /* LOC-IM-LOCK: kein RT */
                e2 = ip_sh_ref(L, li, (ID3D11View *)o->rqr.srv[k2], 1);
                if (e2 >= 0) { o->rq_shsrv[k2] = g_ish[e2].srv; }
            }
        }
        if ((o->rtv && !o->sh_rtv) || (o->dsv && !o->sh_dsv)) { L->snap_ok = 0; }
    }
    if (L->bbref < 0) { L->snap_ok = 0; }
}

/* Schatten vor der Wiedergabe von B auf den Tick-Anfang setzen: Stand vom
 * Ende des Vorticks A (falls dort gesichert), sonst Ende von B (in A nicht
 * benutzt -> seit davor unveraendert bzw. wird in B neu gezeichnet). */
const unsigned char *ip_sh_noskip; /* W146: Eintraege ohne Startkopie (NULL = alle kopieren) */
void ip_sh_start(ip_list_t *B, int bi, ip_list_t *A, int ai, const ip_cx_t *x)
{
    int j;
    for (j = 0; j < B->nrefs; j++) {
        ip_sh_t *e = &g_ish[B->refs[j]];
        ID3D11Texture2D *src = NULL;
        if (ip_sh_noskip && ip_sh_noskip[B->refs[j]]) { continue; } /* W146 GPU-COST: erst Clear bzw. ruhig */
        if (A && ai >= 0 && e->bakser[ai] == A->serial) { src = e->bak[ai]; }
        else if (e->bakser[bi] == B->serial) { src = e->bak[bi]; }
        if (src) {
            ID3D11DeviceContext_CopyResource(x->c, (ID3D11Resource *)e->sh, (ID3D11Resource *)src);
        }
    }
}

void ip_state_save(ip_state_t *s)
{
    memset(s, 0, sizeof *s);
    ID3D11DeviceContext_OMGetRenderTargets(g_ctx, 1, &s->rtv, &s->dsv);
    ID3D11DeviceContext_OMGetBlendState(g_ctx, &s->bs, s->bf, &s->smask);
    ID3D11DeviceContext_OMGetDepthStencilState(g_ctx, &s->ds, &s->sref);
    ID3D11DeviceContext_RSGetState(g_ctx, &s->rs);
    s->nvp = 1;
    ID3D11DeviceContext_RSGetViewports(g_ctx, &s->nvp, &s->vp);
    s->nsc = 1;
    ID3D11DeviceContext_RSGetScissorRects(g_ctx, &s->nsc, &s->sc);
    ID3D11DeviceContext_PSGetShaderResources(g_ctx, 0, 1, &s->srv);
    ID3D11DeviceContext_IAGetVertexBuffers(g_ctx, 0, 2, s->vb, s->vbs, s->vbo);
    ID3D11DeviceContext_IAGetInputLayout(g_ctx, &s->lay);
    ID3D11DeviceContext_IAGetPrimitiveTopology(g_ctx, &s->topo);
    ID3D11DeviceContext_VSGetShader(g_ctx, &s->vs, NULL, NULL);
    ID3D11DeviceContext_PSGetShader(g_ctx, &s->ps, NULL, NULL);
    ID3D11DeviceContext_VSGetConstantBuffers(g_ctx, 0, 2, s->vcb);
    ID3D11DeviceContext_PSGetConstantBuffers(g_ctx, 0, 1, &s->pcb);
    ID3D11DeviceContext_PSGetSamplers(g_ctx, 0, 4, s->smp);
    ID3D11DeviceContext_PSGetShaderResources(g_ctx, 1, 3, s->srvx);
    /* RENDER-QA (W77): Wiedergaben laufen im Echtzeit-Takt auch zwischen
     * rq_bind und dem Draw eines Shading-Draws (ip_poll in rec_draw) ->
     * Stream 2 und die Konstanten-Fenster ebenfalls sichern. */
    if (g_cfg.shading) { ID3D11DeviceContext_IAGetVertexBuffers(g_ctx, 2, 1, &s->vb2, &s->vbs2, &s->vbo2); }
    if (g_cfg.shading) { ID3D11DeviceContext_VSGetShaderResources(g_ctx, 0, 1, &s->vsrv0); }
    if (g_rq_ctx1) {
        ID3D11DeviceContext1_VSGetConstantBuffers1(g_rq_ctx1, 3, 1, &s->vcb3, &s->vf3, &s->vn3);
        ID3D11DeviceContext1_PSGetConstantBuffers1(g_rq_ctx1, 4, 1, &s->pcb4, &s->pf4, &s->pn4);
    }
}

void ip_state_restore(ip_state_t *s)
{
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &s->rtv, s->dsv);
    ID3D11DeviceContext_OMSetBlendState(g_ctx, s->bs, s->bf, s->smask);
    ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, s->ds, s->sref);
    ID3D11DeviceContext_RSSetState(g_ctx, s->rs);
    if (s->nvp) { ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &s->vp); }
    if (s->nsc) { ID3D11DeviceContext_RSSetScissorRects(g_ctx, 1, &s->sc); }
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, &s->srv);
    ID3D11DeviceContext_IASetVertexBuffers(g_ctx, 0, 2, s->vb, s->vbs, s->vbo);
    ID3D11DeviceContext_IASetInputLayout(g_ctx, s->lay);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, s->topo);
    ID3D11DeviceContext_VSSetShader(g_ctx, s->vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_ctx, s->ps, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(g_ctx, 0, 2, s->vcb);
    ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 0, 1, &s->pcb);
    ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 4, s->smp);
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 1, 3, s->srvx);
    if (g_cfg.shading) { ID3D11DeviceContext_IASetVertexBuffers(g_ctx, 2, 1, &s->vb2, &s->vbs2, &s->vbo2); }
    if (s->vb2) { ID3D11Buffer_Release(s->vb2); }
    if (g_cfg.shading) { ID3D11DeviceContext_VSSetShaderResources(g_ctx, 0, 1, &s->vsrv0); }
    if (s->vsrv0) { ID3D11ShaderResourceView_Release(s->vsrv0); }
    if (g_rq_ctx1) {
        if (s->vcb3) {
            ID3D11DeviceContext1_VSSetConstantBuffers1(g_rq_ctx1, 3, 1, &s->vcb3, &s->vf3, &s->vn3);
            ID3D11Buffer_Release(s->vcb3);
        }
        if (s->pcb4) {
            ID3D11DeviceContext1_PSSetConstantBuffers1(g_rq_ctx1, 4, 1, &s->pcb4, &s->pf4, &s->pn4);
            ID3D11Buffer_Release(s->pcb4);
        }
    }
    {
        int k;
        for (k = 0; k < 4; k++) { if (s->smp[k]) { ID3D11SamplerState_Release(s->smp[k]); } }
        for (k = 0; k < 3; k++) { if (s->srvx[k]) { ID3D11ShaderResourceView_Release(s->srvx[k]); } }
    }
    if (s->rtv) { ID3D11RenderTargetView_Release(s->rtv); }
    if (s->dsv) { ID3D11DepthStencilView_Release(s->dsv); }
    if (s->bs) { ID3D11BlendState_Release(s->bs); }
    if (s->ds) { ID3D11DepthStencilState_Release(s->ds); }
    if (s->rs) { ID3D11RasterizerState_Release(s->rs); }
    if (s->srv) { ID3D11ShaderResourceView_Release(s->srv); }
    if (s->vb[0]) { ID3D11Buffer_Release(s->vb[0]); }
    if (s->vb[1]) { ID3D11Buffer_Release(s->vb[1]); }
    if (s->lay) { ID3D11InputLayout_Release(s->lay); }
    if (s->vs) { ID3D11VertexShader_Release(s->vs); }
    if (s->ps) { ID3D11PixelShader_Release(s->ps); }
    if (s->vcb[0]) { ID3D11Buffer_Release(s->vcb[0]); }
    if (s->vcb[1]) { ID3D11Buffer_Release(s->vcb[1]); }
    if (s->pcb) { ID3D11Buffer_Release(s->pcb); }
}

void ip_cb_write(const ip_cx_t *x, ID3D11Buffer *cb, const float m[16], unsigned u, float f, int isf)
{
    D3D11_MAPPED_SUBRESOURCE map;
    HRESULT hrm;
    WD_CALL("ip_cb_map", NULL, hrm = ID3D11DeviceContext_Map(x->c, (ID3D11Resource *)cb, 0,
                                       D3D11_MAP_WRITE_DISCARD, 0, &map)); /* DIAG(PERF-WARM,W139) */
    if (FAILED(hrm)) {
        return;
    }
    memcpy(map.pData, m, 64);
    if (isf) { *(float *)((unsigned char *)map.pData + 64) = f; }
    else { *(unsigned *)((unsigned char *)map.pData + 64) = u; }
    ID3D11DeviceContext_Unmap(x->c, (ID3D11Resource *)cb, 0);
}
ip_ring_t ip_ring[2]; /* 0 = Immediate, 1 = Deferred (Render-Thread) */
ip_rc_t ip_rc_ctx[2];

int ip_ring_fill(const ip_cx_t *x, const ip_list_t *B, const ip_list_t *A, int lerp_ok, float t, ip_rc_t *rc,
                        int laOn, float laM[LA_NM][16])
{
    ip_ring_t *R = &ip_ring[x->deferred ? 1 : 0];
    unsigned i, need = 0, n = 0, last0 = 0xFFFFFFFFu, last1 = 0xFFFFFFFFu;
    unsigned char r[80], p0[80], p1[80];
    D3D11_MAPPED_SUBRESOURCE m;
    unsigned char *d;
    HRESULT hr;
    if (!x->c1 || !B->nops) { return 0; }
    for (i = 0; i < B->nops; i++) {
        const ip_op_t *o = &B->ops[i];
        if (o->kind != IP_DRAW) { continue; }
        need += (lerp_ok && o->flag && o->match >= 0 && (unsigned)o->match < A->nops) ? 2u : 1u;
    }
    if (!need) { return 0; }
    if (R->ofscap < 2u * B->nops) {
        unsigned nc = 2u * B->nops + 1024u;
        unsigned *no = (unsigned *)realloc(R->ofs, (size_t)nc * sizeof *no);
        if (!no) { return 0; }
        R->ofs = no;
        R->ofscap = nc;
    }
    if (!R->buf || R->cap < need) {
        D3D11_BUFFER_DESC bd;
        unsigned nc = need + need / 4u;
        if (nc < 4096u) { nc = 4096u; }
        if (R->buf) { ID3D11Buffer_Release(R->buf); R->buf = NULL; R->cap = 0; }
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = nc * IP_RING_SLOT;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &R->buf))) { R->buf = NULL; return 0; }
        R->cap = nc;
        HLOG_I("INTERP cb_ring: Ring-Puffer %s %u Plaetze (%u KB)", x->deferred ? "Render-Thread" : "Spiel-Thread", nc,
               nc * IP_RING_SLOT / 1024u);
    }
    WD_CALL("ip_ring_map", NULL, hr = ID3D11DeviceContext_Map(x->c, (ID3D11Resource *)R->buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m));
    if (FAILED(hr)) { return 0; }
    d = (unsigned char *)m.pData;
    for (i = 0; i < B->nops; i++) {
        const ip_op_t *o = &B->ops[i];
        R->ofs[2u * i] = R->ofs[2u * i + 1u] = 0xFFFFFFFFu;
        if (o->kind != IP_DRAW) { continue; }
        if (LA_ON(laOn, o->law)) { float lm[16]; ip_la_mul(lm, o->mvp, laM[o->law - 1]); memcpy(r, lm, 64); } /* W376, W394 */
        else { memcpy(r, o->mvp, 64); }
        memcpy(r + 64, &o->has_tex, 4);
        memset(r + 68, 0, 12); /* Padding (frueher W166-Parameter, entfernt in W181) */
        if (last0 == 0xFFFFFFFFu || memcmp(r, p0, 80) != 0) {
            memcpy(d + (size_t)n * IP_RING_SLOT, r, 80);
            memcpy(p0, r, 80);
            last0 = n++;
        }
        R->ofs[2u * i] = last0;
        if (lerp_ok && o->flag && o->match >= 0 && (unsigned)o->match < A->nops) {
            if (LA_ON(laOn, o->law)) { float lm[16]; ip_la_mul(lm, IP_AMVP(o, &A->ops[o->match]), laM[o->law - 1]); memcpy(r, lm, 64); } /* W376, W394 */
            else { memcpy(r, IP_AMVP(o, &A->ops[o->match]), 64); } /* W317 rzm */
            memcpy(r + 64, &t, 4);
            if (last1 == 0xFFFFFFFFu || memcmp(r, p1, 80) != 0) {
                memcpy(d + (size_t)n * IP_RING_SLOT, r, 80);
                memcpy(p1, r, 80);
                last1 = n++;
            }
            R->ofs[2u * i + 1u] = last1;
        }
    }
    ID3D11DeviceContext_Unmap(x->c, (ID3D11Resource *)R->buf, 0);
    rc->slots = n;
    rc->nmap++;
    return 1;
}

/* Versatz binden (nur bei Wechsel). which: 0 = b0 (VS+PS), 1 = b1 (VS). */
void ip_ring_bind(const ip_cx_t *x, unsigned slot, int which, unsigned cur[3], ip_rc_t *rc)
{
    ID3D11Buffer *b = ip_ring[x->deferred ? 1 : 0].buf;
    UINT f = slot * (IP_RING_SLOT / 16u), nc = IP_RING_SLOT / 16u;
    if (slot == 0xFFFFFFFFu) { return; }
    if (which == 0) {
        if (cur[0] != slot) { ID3D11DeviceContext1_VSSetConstantBuffers1(x->c1, 0, 1, &b, &f, &nc); cur[0] = slot; rc->ncb++; }
        if (cur[2] != slot) { ID3D11DeviceContext1_PSSetConstantBuffers1(x->c1, 0, 1, &b, &f, &nc); cur[2] = slot; rc->ncb++; }
    } else if (cur[1] != slot) {
        ID3D11DeviceContext1_VSSetConstantBuffers1(x->c1, 1, 1, &b, &f, &nc);
        cur[1] = slot;
        rc->ncb++;
    }
}

/* DIAG(PERF-M2,W174) ip_ablate: Variante dieser Wiedergabe (0 = normal). Im
 * Wechsel je Tick und t, damit alle Varianten dieselbe Szene/Last sehen. */
unsigned ip_var_pick(const ip_list_t *B, float t)
{
    unsigned ab = (unsigned)g_cfg.ip_ablate & 127u, vars[8], nv = 0, k;
    if (!ab || t <= 0.0f || t >= 1.0f) { return 0u; }
    vars[nv++] = 0u;
    for (k = 1u; k <= 64u; k <<= 1) { if (ab & k) { vars[nv++] = k; } }
    k = (unsigned)(t * 8.0f + 0.5f) >> 1;
    return vars[(B->serial + k) % nv];
}

/* dbg = 1 (interp_diag>=3): Welt-/RT-Draws in der Farbe ihres Zuordnungs-
 * Ergebnisses (kIP_DBG_COL), 2D im BB normal (Monitore zeigen die RTs). */
void ip_replay(ip_list_t *B, int bi, ip_list_t *A, int ai, float t)
{
    ip_replay_ex(B, bi, A, ai, t, 0, ip_cx_imm());
}
static ID3D11Query *ip_gs_dj, *ip_gs_q[IP_GS_MAX];
ID3D11RenderTargetView *ip_gs_rtv[IP_GS_MAX];
unsigned ip_gs_draws[IP_GS_MAX], ip_gs_rq[IP_GS_MAX];
int ip_gs_n = -1; /* -1 = keine Messung in dieser Wiedergabe */
static unsigned ip_gs_cnt;

void ip_gs_mark(ID3D11RenderTargetView *rtv)
{
    if (ip_gs_n < 0 || ip_gs_n >= IP_GS_MAX) { return; }
    if (!ip_gs_q[ip_gs_n]) {
        D3D11_QUERY_DESC qd;
        memset(&qd, 0, sizeof qd);
        qd.Query = D3D11_QUERY_TIMESTAMP;
        if (FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &ip_gs_q[ip_gs_n]))) { ip_gs_n = -1; return; }
    }
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)ip_gs_q[ip_gs_n]);
    ip_gs_rtv[ip_gs_n] = rtv;
    ip_gs_draws[ip_gs_n] = 0;
    ip_gs_rq[ip_gs_n] = 0;
    ip_gs_n++;
}

int ip_gs_begin(float t)
{
    D3D11_QUERY_DESC qd;
    ip_gs_n = -1;
    if (g_cfg.perf_gsplit <= 0 || t >= 1.0f || !rq_in_mission()) { return 0; }
    if ((++ip_gs_cnt % (unsigned)g_cfg.perf_gsplit) != 0u) { return 0; }
    if (!ip_gs_dj) {
        memset(&qd, 0, sizeof qd);
        qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        if (FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &ip_gs_dj))) { ip_gs_dj = NULL; return 0; }
    }
    ID3D11DeviceContext_Begin(g_ctx, (ID3D11Asynchronous *)ip_gs_dj);
    ip_gs_n = 0;
    ip_gs_mark(NULL); /* Segment 0 = Schatten-Startkopien */
    return 1;
}

void ip_gs_end(unsigned nops)
{
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
    UINT64 ts[IP_GS_MAX];
    int i, n, k, nk = 0;
    struct { unsigned w, h, segs, draws, rq; double ms; } ag[24];
    double tot, copy;
    char line[1400];
    int o = 0;
    DWORD t0;
    if (ip_gs_n < 0) { return; }
    ip_gs_mark((ID3D11RenderTargetView *)1); /* Ende */
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)ip_gs_dj);
    n = ip_gs_n;
    ip_gs_n = -1;
    t0 = GetTickCount();
    while (ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)ip_gs_dj, &dj, sizeof dj, 0) == S_FALSE) {
        if (GetTickCount() - t0 > 500u) { return; }
        YieldProcessor();
    }
    if (dj.Disjoint || !dj.Frequency || n < 2) { return; }
    for (i = 0; i < n; i++) {
        if (ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)ip_gs_q[i], &ts[i], sizeof ts[i], 0) != S_OK) { return; }
    }
    memset(ag, 0, sizeof ag);
    tot = (double)(ts[n - 1] - ts[0]) * 1000.0 / (double)dj.Frequency;
    copy = (double)(ts[1] - ts[0]) * 1000.0 / (double)dj.Frequency;
    for (i = 1; i < n - 1; i++) {
        unsigned w = 0, h = 0;
        double ms = (double)(ts[i + 1] - ts[i]) * 1000.0 / (double)dj.Frequency;
        if ((uintptr_t)ip_gs_rtv[i] > 16u) { /* 1/2 = Marken */
            ID3D11Resource *r = NULL;
            D3D11_TEXTURE2D_DESC td;
            ID3D11View_GetResource((ID3D11View *)ip_gs_rtv[i], &r);
            if (r) {
                ID3D11Texture2D_GetDesc((ID3D11Texture2D *)r, &td);
                w = td.Width; h = td.Height;
                ID3D11Resource_Release(r);
            }
        }
        for (k = 0; k < nk && (ag[k].w != w || ag[k].h != h); k++) {}
        if (k == nk) { if (nk >= 24) { continue; } ag[k].w = w; ag[k].h = h; nk++; }
        ag[k].ms += ms; ag[k].segs++; ag[k].draws += ip_gs_draws[i]; ag[k].rq += ip_gs_rq[i];
    }
    for (k = 0; k < nk && o >= 0; k++) {
        o += _snprintf_s(line + o, sizeof line - (size_t)o, _TRUNCATE, " | %ux%u ms=%.2f segs=%u draws=%u rq=%u",
                         ag[k].w, ag[k].h, ag[k].ms, ag[k].segs, ag[k].draws, ag[k].rq);
    }
    HLOG_I("GPUSPLIT f=%u ops=%u total=%.2fms startkopien=%.2fms segs=%d%s", g_present_no, nops, tot, copy, n - 2, line);
}
