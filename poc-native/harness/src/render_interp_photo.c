/* render_interp_photo.c - FPS-INTERP Bilder/VR/Pause/Foto-Wiedergabe und Weltreprojektion-Vorbereitung (W295).
 * W463 RCORE: aus render_interp.c ausgegliedert (reines Verschieben); gemeinsamer Zustand in
 * render_interp_int.h, Modulkarte docs/RENDER_ARCH.md. */
#include "render_interp_int.h"
static sb_interp_mvp_hook_t ip_pl_hook; /* W191-Schnittstelle: MVP der BB-Draws (Cockpit-Projektion) */

/* Bildfolge (interp_shots) fuer diesen Tick? Speichert das Spiel-Bild. */
void ip_shots_tick(ip_list_t *B)
{
    char nm[64];
    ip_shot_ser = 0;
    if (g_cfg.interp_shots <= 0 || ip_pairs_total < (unsigned)g_cfg.interp_shots) { return; }
    if (!ip_shot_left && ip_pairs_total >= ip_shot_next) {
        if (ip_shot_seq >= (unsigned)g_cfg.interp_shot_max) { return; }
        ip_shot_left = (unsigned)(g_cfg.interp_shot_ticks > 0 ? g_cfg.interp_shot_ticks : 6);
        ip_shot_seq++;
        ip_shot_next = g_cfg.interp_shot_every > 0 ? ip_pairs_total + (unsigned)g_cfg.interp_shot_every
                                                   : 0xFFFFFFFFu;
    }
    if (!ip_shot_left) { return; }
    ip_shot_left--;
    ip_shot_ser = B->serial;
    _snprintf_s(nm, sizeof nm, _TRUNCATE, "ip_%06u_g", B->frame);
    sb_render_shot_name(nm, (ID3D11Resource *)g_ish[B->bbref].bak[ip_list_index(B)]);
}

/* W134 VR-PROTO: Augen-Wiedergabe eines Paars (unter ctx_lock, Context-Thread
 * oder VR-Thread). pA/pB = NULL -> Paar zum Zeitpunkt due (Echtzeit-Takt wie
 * ip_poll) bzw. im Testtakt das juengste Paar mit t aus der Zeit seit dem Swap. */
/* DIAG(VR-PROTO,W134): exaktes Mono-Bild des Ticks B (Vergleich Einzelauge, IPD 0, Kopf still) */
void sb_interp_vr_mono_shot(void *pB, const char *name)
{
    ip_list_t *B = (ip_list_t *)pB;
    if (!B || B->bbref < 0) { return; }
    sb_render_shot_name(name, (ID3D11Resource *)g_ish[B->bbref].bak[ip_list_index(B)]);
}

int sb_interp_vr_available(void)
{
    return ip_init_done && !ip_init_fail && ip_nhist >= 1;
}

int sb_interp_vr_replay(sb_vr_pass_t *p, void *pA, void *pB, float t, long long due, float *tout)
{
    ip_list_t *A = (ip_list_t *)pA, *B = (ip_list_t *)pB;
    ID3D11Buffer *vcb2 = NULL;
    int hold = 0;
    if (!ip_init_done || ip_init_fail || ip_nhist < 1 || !p || !p->rtv) { return 0; }
    if (!B) {
        LONGLONG now = ip_now();
        if (ip_live) {
            ip_pair_at(due ? (LONGLONG)due : now, &A, &B, &t, &hold);
        } else {
            B = &g_ipl[ip_hist[ip_nhist - 1]];
            A = ip_nhist >= 2 ? &g_ipl[ip_hist[ip_nhist - 2]] : NULL;
            t = 1.0f;
            if (A && B->tswap > A->tswap && now > B->tswap) {
                double f = (double)(now - B->tswap) / (double)(B->tswap - A->tswap);
                t = f >= 1.0 ? 1.0f : (float)f;
            }
        }
    }
    if (!B || B->bbref < 0 || !B->ready) { return 0; }
    if (A && !A->ready) { A = NULL; }
    p->hold = hold; /* W183 VR-FIX3: Diagnose Paarwahl */
    p->bser = B->serial;
    p->aser = A ? A->serial : 0u;
    ID3D11DeviceContext_VSGetConstantBuffers(g_ctx, 2, 1, &vcb2); /* VR-VS nutzt b2 */
    ip_vrp = p;
    {
        int gpm = sb_gp_begin(1); /* DIAG(GPU-COST,W146): GPU je VR-Bild (beide Augen) */
        ip_replay_ex(B, ip_list_index(B), A, A ? ip_list_index(A) : -1, t, 0, ip_cx_imm());
        if (gpm) { sb_gp_end(); }
    }
    ip_vrp = NULL;
    ID3D11DeviceContext_VSSetConstantBuffers(g_ctx, 2, 1, &vcb2);
    if (vcb2) { ID3D11Buffer_Release(vcb2); }
    if (tout) { *tout = (A && t < 1.0f) ? t : 1.0f; }
    return 1;
}

/* ---------- W193 PAUSE-LIVE: Standbild der Pause mit den aktuellen Einstellungen ----------
 * Der juengste Tick liegt vollstaendig in der Aufzeichnung (Draws, Clears, Zustaende,
 * Shading-Konstanten, Schatten-Startstand). Die exakte Wiedergabe (t = 1) zeichnet ihn neu;
 * was erst bei der Wiedergabe bzw. im Blit gelesen wird, wirkt damit sofort. Zusaetzlich
 * weichen nur in dieser Wiedergabe ab (ip_pl_*):
 *  - Shading-Konstanten: sfx0..sfx3 (Nebel, Kontrast, Gamma, Schaerfe, Bloom, Belichtung,
 *    Tonemapping, Licht Welt/Cockpit, Fokus, FXAA Welt-RT, weiche Schatten) je Shading-Draw aus den
 *    Eingaben der Aufzeichnung neu (shfx_rq_consts_rep); Schatten aus -> Schattenstufe = 1
 *    (tsc.z = 0, wie ohne Schattenpuffer). Das Fenster wird einmal je Liste zurueckgelesen.
 *  - shading aus -> Shading-Draws mit IM-PS-Ersatz (wie VR vr_shading=0).
 *  - monitor_fx aus bzw. Staerke 0 -> Korn-Wash/Stoerzeilen ausgelassen.
 *  - MVP-Haken der Backbuffer-Draws (W191 Cockpit-Projektion).
 * Mit unveraenderten Einstellungen ist das Ergebnis die exakte Wiedergabe (= Spiel-Bild,
 * Abgleich sb_interp_pause_cmp). */
static float *ip_pl_corig, *ip_pl_cwork; /* CPU-Kopie des Konstanten-Fensters (Original / neu) */
static unsigned ip_pl_cn, ip_pl_ccap;    /* belegte Konstanten / Kapazitaet (float4) */
static unsigned ip_pl_cser;              /* Liste, fuer die ip_pl_corig gilt */
static unsigned ip_pl_cbcap;             /* ip_pl_cb: Konstanten */

void sb_interp_set_pause_mvp_hook(sb_interp_mvp_hook_t fn)
{
    ip_pl_hook = fn;
}

/* Konstanten-Fenster der Liste B zurueckholen (einmal je Liste) und neu berechnen.
 * 1 = Kopie gebunden, 0 = keine Shading-Konstanten, -1 = Fehler (Aufzeichnungswerte). */
static int ip_pl_consts(ip_list_t *B, sb_pl_info_t *inf)
{
    D3D11_BUFFER_DESC bd;
    D3D11_MAPPED_SUBRESOURCE m;
    unsigned i, k, n;
    ip_pl_cbsrc = NULL;
    if (!B->rqc || !B->rqcpos) { return 0; }
    n = B->rqcpos;
    if (ip_pl_cser != B->serial || !ip_pl_corig || ip_pl_cn != n) {
        ID3D11Buffer *stg = NULL;
        if (ip_pl_ccap < n) {
            free(ip_pl_corig);
            free(ip_pl_cwork);
            ip_pl_corig = (float *)malloc((size_t)n * 16u);
            ip_pl_cwork = (float *)malloc((size_t)n * 16u);
            ip_pl_ccap = (ip_pl_corig && ip_pl_cwork) ? n : 0u;
            if (!ip_pl_ccap) { return -1; }
        }
        ID3D11Buffer_GetDesc(B->rqc, &bd);
        bd.Usage = D3D11_USAGE_STAGING;
        bd.BindFlags = 0;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        bd.MiscFlags = 0;
        bd.StructureByteStride = 0;
        if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &stg))) { return -1; }
        ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)stg, (ID3D11Resource *)B->rqc);
        if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)stg, 0, D3D11_MAP_READ, 0, &m))) {
            ID3D11Buffer_Release(stg);
            return -1;
        }
        memcpy(ip_pl_corig, m.pData, (size_t)n * 16u);
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)stg, 0);
        ID3D11Buffer_Release(stg);
        ip_pl_cser = B->serial;
        ip_pl_cn = n;
    }
    memcpy(ip_pl_cwork, ip_pl_corig, (size_t)n * 16u);
    for (i = 0; i < B->nops; i++) {
        const ip_op_t *o = &B->ops[i];
        const rq_rec_t *r = &o->rqr;
        float *pb, sx[16];
        if (o->kind != IP_DRAW || !o->rq || r->cbuf != B->rqc || r->pnum < 28u || r->pfirst + 28u > n) { continue; }
        pb = ip_pl_cwork + (size_t)r->pfirst * 4u;
        inf->ncb++;
        shfx_rq_consts_rep(sx, r->sfx_xp, r->sfx_tgt, (r->sfx_fl & 1u) != 0, (r->sfx_fl & 2u) != 0,
                           (r->sfx_fl & 4u) != 0, r->sfx_nst, (r->sfx_fl >> 3) & 3u); /* W507: Bloom-Klasse */
        if (memcmp(&pb[24u * 4u], sx, sizeof sx) != 0) {
            memcpy(&pb[24u * 4u], sx, sizeof sx);
            inf->nsfx++;
        }
        if (!g_cfg.shade_shadow) { /* Schattenpuffer (tsc[k].w != 0) aus: Stufe = 1 */
            for (k = 1; k < 4u; k++) {
                float *ts = &pb[(19u + k) * 4u];
                if (ts[3] != 0.0f && ts[2] != 0.0f) {
                    ts[2] = 0.0f;
                    ts[3] = 0.0f;
                    inf->nshadow++;
                }
            }
        }
    }
    if (!ip_pl_cb || ip_pl_cbcap < B->rqccap) {
        if (ip_pl_cb) { ID3D11Buffer_Release(ip_pl_cb); ip_pl_cb = NULL; ip_pl_cbcap = 0; }
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = B->rqccap * 16u;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &ip_pl_cb))) { ip_pl_cb = NULL; return -1; }
        ip_pl_cbcap = B->rqccap;
    }
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)ip_pl_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { return -1; }
    memcpy(m.pData, ip_pl_cwork, (size_t)n * 16u);
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)ip_pl_cb, 0);
    ip_pl_cbsrc = B->rqc;
    return 1;
}

/* W209: no extra VB work during normal play or an unchanged pause. Read the
 * recorded VB once per dirty camera replay, restore sparse originals, then
 * transform vertices and matrices together. The recording remains immutable. */
static int ip_pl_cpersp(ip_list_t *B)
{
    D3D11_BUFFER_DESC bd;
    D3D11_MAPPED_SUBRESOURCE map;
    D3D11_SUBRESOURCE_DATA init;
    D3D11_BOX box;
    ID3D11Buffer *stage = NULL;
    im_vertex_t *v = NULL;
    ip_cp_t *p;
    unsigned i, bytes = B->nverts * (unsigned)sizeof *v;
    int ok = 0;
    if (!sb_cp_pause_begin(B->cp_camera) || !ip_pl_hook) { return 1; }
    if (!bytes) { return 0; }
    v = (im_vertex_t *)malloc(bytes);
    ip_pl_mvp = (float (*)[16])malloc((size_t)B->nops * sizeof *ip_pl_mvp);
    if (!v || !ip_pl_mvp) { goto done; }
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = bytes; bd.Usage = D3D11_USAGE_STAGING; bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &stage))) { goto done; }
    memset(&box, 0, sizeof box);
    box.right = bytes; box.bottom = box.back = 1;
    ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)stage, 0, 0, 0, 0,
                                            (ID3D11Resource *)B->vb, 0, &box);
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)stage, 0, D3D11_MAP_READ, 0, &map))) { goto done; }
    memcpy(v, map.pData, bytes);
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)stage, 0);
    for (p = B->cp_original; p; p = p->next) {
        if (p->start > B->nverts || p->n > B->nverts - p->start) { goto done; }
        for (i = 0; i < p->n; i++) { memcpy(v[p->start + i].pos, p->pos[i], sizeof p->pos[i]); }
    }
    for (i = 0; i < B->nops; i++) {
        ip_op_t *o = &B->ops[i];
        memcpy(ip_pl_mvp[i], o->mvp, sizeof o->mvp);
        if (o->kind == IP_DRAW && o->rtv == g_rtv) {
            if (o->vstart > B->nverts || o->nsv > B->nverts - o->vstart) { goto done; }
            if (ip_pl_hook(ip_pl_mvp[i], o->cls, v[o->vstart].pos,
                           sizeof *v / sizeof(float), o->nsv)) { ip_pl_nhook++; } /* W264: Vertices im Tick-VB */
        }
    }
    bd.Usage = D3D11_USAGE_IMMUTABLE; bd.CPUAccessFlags = 0; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    memset(&init, 0, sizeof init); init.pSysMem = v;
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, &init, &ip_pl_vb))) { goto done; }
    ok = 1;
done:
    if (stage) { ID3D11Buffer_Release(stage); }
    free(v);
    if (!ok) { free(ip_pl_mvp); ip_pl_mvp = NULL; }
    return ok;
}

static ID3D11Resource *ip_pl_render(sb_pl_info_t *inf, const sb_ph_rep_t *ph);
ID3D11Resource *sb_interp_pause_render(sb_pl_info_t *inf)
{
    return ip_pl_render(inf, NULL);
}

/* W195 PHOTO-MODE: wie sb_interp_pause_render, zusaetzlich mit den Foto-Abweichungen ph (Umlenkung in eine
 * Kachel, freie Kamera). Rueckgabe Schatten-BB (Cockpit-Bild) bzw. NULL. */
ID3D11Resource *sb_interp_photo_render(const sb_ph_rep_t *ph, sb_pl_info_t *inf, unsigned *ncam, unsigned *nred)
{
    ID3D11Resource *r;
    ip_ph_ncam = ip_ph_nred = ip_ph_nclr = 0;
    ip_ph_nhud = 0;
    ip_ph_nself = ip_ph_ncmfx = ip_ph_ncovl = 0;
    ip_ph_nextra = 0;
    ip_ph_self_L = NULL; /* W445: Merker eigener VT je Wiedergabe neu (Liste/Reichweite koennen wechseln) */
    r = ip_pl_render(inf, ph);
    ip_ph_self_L = NULL;
    if (ph && ph->cine) { /* W428 */
        static unsigned nlog;
        if (nlog++ < 12u || (g_cfg.photo_cine_diag & 1)) { /* W445: mit DIAG jede Wiedergabe */
            HLOG_I("PHOTO: Cinematic - ausgelassen: %u Draws des eigenen VT (Welt-RT e%d), %u Monitor-Effekt, %u Einblendungen "
                   "(Monitor-RT e%d / Backbuffer); W445: davon %u Luecke, Reichweite %d", ip_ph_nself, ph->wld, ip_ph_ncmfx,
                   ip_ph_ncovl, ph->mon, ip_ph_nextra, g_cfg.photo_cine_self);
        }
    }
    if (ph && ph->hud_off && (ip_ph_nhud || ph->mon < 0)) { /* W290 */
        static unsigned nlog;
        if (nlog++ < 4u) { HLOG_I("PHOTO: HUD aus - %u HUD-Draws im Monitor-RT e%d ausgelassen", ip_ph_nhud, ph->mon); }
    }
    if (ncam) { *ncam = ip_ph_ncam; }
    if (nred) { *nred = ip_ph_nred + ip_ph_nclr; }
    return r;
}

static ID3D11Resource *ip_pl_render(sb_pl_info_t *inf, const sb_ph_rep_t *ph)
{
    ip_list_t *A, *B;
    sb_pl_info_t dummy;
    unsigned i;
    if (!inf) { inf = &dummy; }
    memset(inf, 0, sizeof *inf);
    if (!ip_init_done || ip_init_fail || ip_nhist < 1 || !g_dev || !g_ctx) { return NULL; }
    B = &g_ipl[ip_hist[ip_nhist - 1]];
    A = ip_nhist >= 2 ? &g_ipl[ip_hist[ip_nhist - 2]] : NULL;
    if (!B->ready || B->bbref < 0) { return NULL; }
    if (A && !A->ready) { A = NULL; }
    ip_rt_quiesce(); /* W126: Render-Thread liest Listen/Schatten */
    inf->ser = B->serial;
    inf->frame = B->frame;
    inf->nops = B->nops;
    for (i = 0; i < B->nops; i++) {
        if (B->ops[i].kind == IP_DRAW && B->ops[i].rq) { inf->nrq++; }
    }
    inf->cb_state = ip_pl_consts(B, inf);
    if (inf->cb_state < 0) { ip_pl_cbsrc = NULL; }
    ip_pl_noshade = !g_cfg.shading;
    ip_pl_mfx = 0u;
    if (!g_cfg.monitor_fx || (g_cfg.monitor_grain <= 0 && g_cfg.monitor_glitch <= 0)) { ip_pl_mfx |= 1u; }
    if (!g_cfg.monitor_fx || g_cfg.monitor_glitch <= 0) { ip_pl_mfx |= 2u; }
    ip_pl_nhook = ip_pl_nmfx = ip_pl_nnoshade = 0;
    if (!ip_pl_cpersp(B)) { HLOG_W("CPERSP pause: rebuild failed, keeping snapshot"); return NULL; }
    ip_pl_tex = sb_tex_pause_nmap() > 0;
    ip_pl_on = 1;
    if (ph) { /* W195 */
        int j;
        ip_ph = *ph;
        ip_ph.on = 1;
        if (ip_ph.nsc > 8) { ip_ph.nsc = 8; }
        for (j = 0; j < ip_ph.nsc; j++) { ip_ph_shsrv[j] = ip_ph.sc_e[j] >= 0 ? g_ish[ip_ph.sc_e[j]].srv : NULL; }
    }
    ip_replay_ex(B, ip_list_index(B), A, A ? ip_list_index(A) : -1, 1.0f, 0, ip_cx_imm());
    memset(&ip_ph, 0, sizeof ip_ph);
    if (ip_pl_vb) { ID3D11Buffer_Release(ip_pl_vb); ip_pl_vb = NULL; }
    free(ip_pl_mvp); ip_pl_mvp = NULL;
    ip_pl_on = 0;
    ip_pl_tex = 0;
    ip_pl_cbsrc = NULL;
    inf->nhook = ip_pl_nhook;
    inf->nmfx = ip_pl_nmfx;
    inf->noshade = ip_pl_nnoshade;
    return (ID3D11Resource *)g_ish[B->bbref].sh;
}

int sb_interp_pause_cmp(ID3D11Resource *img, unsigned *ndiff, unsigned *maxd)
{
    D3D11_MAPPED_SUBRESOURCE m0, m1;
    ip_list_t *B;
    unsigned x, y, nd = 0, mx = 0;
    int bi;
    if (!ip_init_done || ip_nhist < 1 || !img) { return 0; }
    B = &g_ipl[ip_hist[ip_nhist - 1]];
    bi = ip_list_index(B);
    if (B->bbref < 0 || g_ish[B->bbref].bakser[bi] != B->serial) { return 0; }
    if (!ip_stage(0, img) || !ip_stage(1, (ID3D11Resource *)g_ish[B->bbref].bak[bi])) { return 0; }
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)ip_vstg[0], 0, D3D11_MAP_READ, 0, &m0))) { return 0; }
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)ip_vstg[1], 0, D3D11_MAP_READ, 0, &m1))) {
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)ip_vstg[0], 0);
        return 0;
    }
    for (y = 0; y < g_bb_h; y++) {
        const unsigned char *a = (const unsigned char *)m0.pData + (size_t)y * m0.RowPitch;
        const unsigned char *b = (const unsigned char *)m1.pData + (size_t)y * m1.RowPitch;
        if (memcmp(a, b, (size_t)g_bb_w * 4u) == 0) { continue; }
        for (x = 0; x < g_bb_w; x++) {
            unsigned c, d = 0;
            for (c = 0; c < 3u; c++) {
                unsigned dc = a[x * 4u + c] > b[x * 4u + c] ? a[x * 4u + c] - b[x * 4u + c] : b[x * 4u + c] - a[x * 4u + c];
                if (dc > d) { d = dc; }
            }
            if (d) {
                nd++;
                if (d > mx) { mx = d; }
            }
        }
    }
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)ip_vstg[1], 0);
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)ip_vstg[0], 0);
    if (ndiff) { *ndiff = nd; }
    if (maxd) { *maxd = mx; }
    return 1;
}

/* ---------- W195 PHOTO-MODE: Zensus des juengsten Ticks und Matrix-Hilfen ---------- */
int ip_m4inv(float out[16], const float m[16])
{
    double a[4][8];
    int i, j, k;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) { a[i][j] = m[i * 4 + j]; a[i][j + 4] = (i == j) ? 1.0 : 0.0; }
    }
    for (i = 0; i < 4; i++) {
        int p = i;
        double d;
        for (k = i + 1; k < 4; k++) { if (fabs(a[k][i]) > fabs(a[p][i])) { p = k; } }
        if (fabs(a[p][i]) < 1e-12) { return 0; }
        if (p != i) { for (j = 0; j < 8; j++) { double t = a[i][j]; a[i][j] = a[p][j]; a[p][j] = t; } }
        d = a[i][i];
        for (j = 0; j < 8; j++) { a[i][j] /= d; }
        for (k = 0; k < 4; k++) {
            if (k != i && a[k][i] != 0.0) {
                double f = a[k][i];
                for (j = 0; j < 8; j++) { a[k][j] -= f * a[i][j]; }
            }
        }
    }
    for (i = 0; i < 4; i++) { for (j = 0; j < 4; j++) { out[i * 4 + j] = (float)a[i][j + 4]; } }
    return 1;
}

static int ip_ph_ent_rtv(ID3D11RenderTargetView *v)
{
    int k;
    if (!v) { return -1; }
    for (k = 0; k < IP_SH_MAX; k++) { if (g_ish[k].rtv == v) { return k; } }
    return -1;
}

static int ip_ph_ent_dsv(ID3D11DepthStencilView *v)
{
    int k;
    if (!v) { return -1; }
    for (k = 0; k < IP_SH_MAX; k++) { if (g_ish[k].dsv == v) { return k; } }
    return -1;
}

/* W195: Vertices (Position, UV, NDC nach MVP) eines Draws der Liste B ins Log (Beleg, welcher Ausschnitt
 * des Welt-RT auf dem Hauptmonitor landet). Nur aus dem Foto-Zensus. */
static void ip_ph_dump_draw(ip_list_t *B, const ip_op_t *o, const char *what, float *bbx)
{
    D3D11_BUFFER_DESC bd;
    D3D11_BOX bx;
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11Buffer *stg = NULL;
    unsigned n = bbx ? (o->nsv < 65536u ? o->nsv : 65536u) : (o->nsv < 6u ? o->nsv : 6u), q; /* Grenzen ueber alle Vertices (W264: im Tick-VB) */
    char line[900];
    size_t pos = 0;
    if (!B->vb || !n) { return; }
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = n * sizeof(im_vertex_t);
    bd.Usage = D3D11_USAGE_STAGING;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &stg))) { return; }
    bx.left = o->vstart * sizeof(im_vertex_t); bx.right = bx.left + n * sizeof(im_vertex_t);
    bx.top = 0; bx.bottom = 1; bx.front = 0; bx.back = 1;
    ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)stg, 0, 0, 0, 0, (ID3D11Resource *)B->vb, 0, &bx);
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)stg, 0, D3D11_MAP_READ, 0, &m))) {
        const im_vertex_t *v = (const im_vertex_t *)m.pData;
        if (bbx) { bbx[0] = bbx[2] = bbx[4] = bbx[6] = 1e30f; bbx[1] = bbx[3] = bbx[5] = bbx[7] = -1e30f; }
        for (q = 0; q < n; q++) {
            float c[4];
            ip_clip(v[q].pos, o->mvp, c);
            if (bbx && c[3] != 0.0f) { /* NDC-x/y- und UV-Grenzen */
                float nx = c[0] / c[3], ny = c[1] / c[3];
                if (nx < bbx[0]) { bbx[0] = nx; } if (nx > bbx[1]) { bbx[1] = nx; }
                if (ny < bbx[2]) { bbx[2] = ny; } if (ny > bbx[3]) { bbx[3] = ny; }
                if (v[q].uv[0] < bbx[4]) { bbx[4] = v[q].uv[0]; } if (v[q].uv[0] > bbx[5]) { bbx[5] = v[q].uv[0]; }
                if (v[q].uv[1] < bbx[6]) { bbx[6] = v[q].uv[1]; } if (v[q].uv[1] > bbx[7]) { bbx[7] = v[q].uv[1]; }
            }
            if (q < 6u) pos += (size_t)_snprintf_s(line + pos, sizeof line - pos, _TRUNCATE, " v%u ndc=(%.3f,%.3f) uv=(%.4f,%.4f)", q,
                                       c[3] != 0.0f ? c[0] / c[3] : 0.0f, c[3] != 0.0f ? c[1] / c[3] : 0.0f, v[q].uv[0],
                                       v[q].uv[1]);
        }
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)stg, 0);
        if (bbx) {
            pos += (size_t)_snprintf_s(line + pos, sizeof line - pos, _TRUNCATE, " | alle: ndc x %.3f..%.3f y %.3f..%.3f uv %.4f..%.4f x %.4f..%.4f",
                                       bbx[0], bbx[1], bbx[2], bbx[3], bbx[4], bbx[5], bbx[6], bbx[7]);
        }
        HLOG_I("PHOTO: %s n=%u vp=%.0fx%.0f@%.0f,%.0f vpg=%u,%u %ux%u tex=0x%X%s", what, o->nv, o->vp.Width, o->vp.Height,
               o->vp.TopLeftX, o->vp.TopLeftY, o->vpg[0], o->vpg[1], o->vpg[2], o->vpg[3], o->has_tex, line);
    }
    ID3D11Buffer_Release(stg);
}

/* DIAG(W249) PHOTO-PIXEL: Raster-Lage der Eckpunkte je Welt-Draw (photo_diag=1, nur Foto-Zensus).
 * Position im logischen Welt-RT (Pixel, wie das Spiel sie sieht): Clip -> NDC -> Viewport -> / RT-Skala.
 * "auf Raster" = x und y je hoechstens 1/64 Pixel neben einer ganzen Zahl. Klasse: VS-Programm (P1/P2/sonst,
 * vsh_program), Draw mit Pixel-Matrix (RHW-Pfad) oder 3D. */
static int ip_ph_ent_srv(ID3D11ShaderResourceView *v)
{
    int k;
    if (!v) { return -1; }
    for (k = 0; k < IP_SH_MAX; k++) { if (g_ish[k].srv == v) { return k; } }
    return -1;
}

static void ip_ph_quant(ip_list_t *B, int best, unsigned w_tw, unsigned w_lw)
{
    unsigned i, nlog = 0, tot[4][3], rd[IP_SH_MAX];
    int k;
    float rsc = (w_lw && w_tw) ? (float)w_tw / (float)w_lw : 1.0f;
    static const char *const kn[4] = { "3D", "RHW", "P1", "P2" };
    memset(tot, 0, sizeof tot);
    memset(rd, 0, sizeof rd);
    if (!B->vb) { return; }
    for (i = 0; i < B->nops; i++) { /* welche Schatten-Ziele (RTs) lesen Welt-Draws als Textur (Stufe 0..3)? */
        const ip_op_t *o = &B->ops[i];
        int st;
        if (o->kind != IP_DRAW || o->sh_rtv != g_ish[best].rtv) { continue; }
        k = ip_ph_ent_srv(o->sh_srv);
        if (k >= 0) { rd[k]++; }
        for (st = 1; st < 4 && o->rq; st++) { k = ip_ph_ent_srv(o->rq_shsrv[st]); if (k >= 0) { rd[k]++; } }
    }
    for (k = 0; k < IP_SH_MAX; k++) {
        if (rd[k] && g_ish[k].sh) {
            D3D11_TEXTURE2D_DESC td;
            unsigned j, nw = 0;
            ID3D11Texture2D_GetDesc(g_ish[k].sh, &td);
            for (j = 0; j < B->nops; j++) { if (B->ops[j].kind == IP_DRAW && B->ops[j].sh_rtv == g_ish[k].rtv) { nw++; } }
            HLOG_I("PHOTO diag: Welt-Draws lesen Ziel e%d %ux%u (%u Draws, im Tick %u Draws in dieses Ziel)", k, td.Width, td.Height,
                   rd[k], nw);
        }
    }
    for (i = 0; i < B->nops; i++) {
        const ip_op_t *o = &B->ops[i];
        const vsh_prog_t *vp;
        D3D11_BUFFER_DESC bd;
        D3D11_BOX bx;
        D3D11_MAPPED_SUBRESOURCE m;
        ID3D11Buffer *stg = NULL;
        unsigned n, q, ngrid = 0, nhalf = 0, nok = 0, kc;
        int pix;
        float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f, r0[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        if (o->kind != IP_DRAW || o->sh_rtv != g_ish[best].rtv || !o->nv) { continue; }
        n = o->nsv < 65536u ? o->nsv : 65536u; /* W264: Vertices im Tick-VB */
        vp = o->pvs ? vsh_program(o->pvs) : NULL;
        pix = o->mvp[3] == 0.0f && o->mvp[7] == 0.0f && o->mvp[11] == 0.0f && o->mvp[15] == 1.0f;
        kc = (vp && vp->kind == 1) ? 2u : (vp && vp->kind == 2) ? 3u : pix ? 1u : 0u;
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = n * sizeof(im_vertex_t);
        bd.Usage = D3D11_USAGE_STAGING;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &stg))) { continue; }
        bx.left = o->vstart * sizeof(im_vertex_t); bx.right = bx.left + n * sizeof(im_vertex_t);
        bx.top = 0; bx.bottom = 1; bx.front = 0; bx.back = 1;
        ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)stg, 0, 0, 0, 0, (ID3D11Resource *)B->vb, 0, &bx);
        if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)stg, 0, D3D11_MAP_READ, 0, &m))) {
            const im_vertex_t *v = (const im_vertex_t *)m.pData;
            memcpy(r0, v[0].pos, sizeof r0);
            for (q = 0; q < n; q++) {
                float c[4], X, Y, fx, fy;
                ip_clip(v[q].pos, o->mvp, c);
                if (c[3] <= 1e-6f) { continue; }
                X = (o->vp.TopLeftX + (c[0] / c[3] + 1.0f) * 0.5f * o->vp.Width) / rsc;
                Y = (o->vp.TopLeftY + (1.0f - c[1] / c[3]) * 0.5f * o->vp.Height) / rsc;
                fx = fabsf(X - floorf(X + 0.5f));
                fy = fabsf(Y - floorf(Y + 0.5f));
                nok++;
                if (fx <= 1.0f / 64.0f && fy <= 1.0f / 64.0f) { ngrid++; }
                else if (fabsf(fx - 0.5f) <= 1.0f / 64.0f && fabsf(fy - 0.5f) <= 1.0f / 64.0f) { nhalf++; }
                if (X < x0) { x0 = X; } if (X > x1) { x1 = X; }
                if (Y < y0) { y0 = Y; } if (Y > y1) { y1 = Y; }
            }
            ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)stg, 0);
        }
        ID3D11Buffer_Release(stg);
        tot[kc][0]++;
        tot[kc][1] += nok;
        tot[kc][2] += ngrid;
        if (nlog < 600u) {
            D3D11_DEPTH_STENCIL_DESC dd;
            D3D11_BLEND_DESC bd2;
            D3D11_RASTERIZER_DESC rd;
            memset(&dd, 0, sizeof dd);
            memset(&bd2, 0, sizeof bd2);
            memset(&rd, 0, sizeof rd);
            if (o->ds) { ID3D11DepthStencilState_GetDesc(o->ds, &dd); }
            if (o->bs) { ID3D11BlendState_GetDesc(o->bs, &bd2); }
            if (o->rs) { ID3D11RasterizerState_GetDesc(o->rs, &rd); }
            nlog++;
            HLOG_I("PHOTO diag: Welt-Draw #%u %s vs=0x%X tex=0x%X srv=e%d n=%u rq=%d auf Raster %u/%u (halb %u) x %.1f..%.1f y %.1f..%.1f "
                   "v0=(%.4f,%.4f,%.4f,%.4f) dsv=e%d z=%d/%d/%d st=%d bl=%d/%d/%d wm=%X bias=%d/%.3f",
                   i, kn[kc], o->pvs, o->pxp, ip_ph_ent_srv(o->sh_srv), o->nv, o->rq, ngrid, nok, nhalf, (double)x0, (double)x1, (double)y0, (double)y1,
                   (double)r0[0], (double)r0[1], (double)r0[2], (double)r0[3], ip_ph_ent_dsv(o->sh_dsv), (int)dd.DepthEnable,
                   (int)dd.DepthWriteMask, (int)dd.DepthFunc, (int)dd.StencilEnable, (int)bd2.RenderTarget[0].BlendEnable,
                   (int)bd2.RenderTarget[0].SrcBlend, (int)bd2.RenderTarget[0].DestBlend,
                   (unsigned)bd2.RenderTarget[0].RenderTargetWriteMask, (int)rd.DepthBias, (double)rd.SlopeScaledDepthBias);
        }
    }
    for (i = 0; i < 4u; i++) {
        if (tot[i][0]) {
            HLOG_I("PHOTO diag: Klasse %s: %u Draws, %u Eckpunkte, davon auf dem Pixelraster des Welt-RT %u (%.1f %%)", kn[i],
                   tot[i][0], tot[i][1], tot[i][2], tot[i][1] ? 100.0 * tot[i][2] / tot[i][1] : 0.0);
        }
    }
}

/* DIAG(W428) PHOTO-CINEMATIC: je Welt-Draw Lage des Modell-Ursprungs in der Sicht der Spielkamera
 * (Zeile 3 der mvp / Projektion: x = m12/a, y = m13/b, z = m15), Bezugs-NDC und -w (photo_diag Bit 4). */
static void ip_ph_selfdiag(ip_list_t *B, int best)
{
    unsigned i, n = 0;
    const ip_list_t *A = NULL; /* W445 DIAG: Vortick (Partner o->match) */
    if (ip_nhist >= 2 && g_ipl[ip_hist[ip_nhist - 2]].serial == B->prevser && B->pair_ok) { A = &g_ipl[ip_hist[ip_nhist - 2]]; }
    for (i = 0; i < B->nops && n < 400u; i++) {
        const ip_op_t *o = &B->ops[i];
        const vsh_prog_t *vp;
        float a, b, wmin = 1e30f, x0 = 9.0f, x1 = -9.0f, y0 = 9.0f, y1 = -9.0f, dm = -1.0f;
        int q;
        if (o->kind != IP_DRAW || o->sh_rtv != g_ish[best].rtv) { continue; }
        vp = o->pvs ? vsh_program(o->pvs) : NULL;
        a = o->prj[0] > 1e-4f ? o->prj[0] : 1.0f;
        b = o->prj[1] > 1e-4f ? o->prj[1] : 1.0f;
        for (q = 0; q < 4; q++) {
            if (o->rn[q][2] < wmin) { wmin = o->rn[q][2]; }
            if (o->rn[q][0] < x0) { x0 = o->rn[q][0]; } if (o->rn[q][0] > x1) { x1 = o->rn[q][0]; }
            if (o->rn[q][1] < y0) { y0 = o->rn[q][1]; } if (o->rn[q][1] > y1) { y1 = o->rn[q][1]; }
        }
        if (A && o->match >= 0 && (unsigned)o->match < A->nops) { /* W445: Ursprung in Kamera-Sicht gegen den Vortick */
            const ip_op_t *p = &A->ops[o->match];
            float pa = p->prj[0] > 1e-4f ? p->prj[0] : 1.0f, pb = p->prj[1] > 1e-4f ? p->prj[1] : 1.0f;
            float dx = o->mvp[12] / a - p->mvp[12] / pa, dy = o->mvp[13] / b - p->mvp[13] / pb, dz = o->mvp[15] - p->mvp[15];
            dm = sqrtf(dx * dx + dy * dy + dz * dz);
        }
        n++;
        HLOG_I("PHOTO cine: i=%u cls=%u vsk=%d nv=%u tex=%x rq=%d pxp=%08x pps=%08x nolerp=%d fxk=%u org=%.2f,%.2f,%.2f "
               "prj=%.3f,%.3f,%.4f,%.4f w=%.2f ndc=%.2f..%.2f,%.2f..%.2f r0=%.3f,%.3f,%.3f r1=%.3f,%.3f,%.3f near=%.2f dm=%.3f",
               i, (unsigned)o->cls, vp ? (int)vp->kind : -1, o->nv, o->has_tex, o->rq, o->pxp, o->pps, o->nolerp, o->fxk,
               (double)(o->mvp[12] / a), (double)(o->mvp[13] / b), (double)o->mvp[15], (double)o->prj[0], (double)o->prj[1],
               (double)o->prj[2], (double)o->prj[3], (double)wmin, (double)x0, (double)x1, (double)y0, (double)y1,
               (double)(o->mvp[0] / a), (double)(o->mvp[1] / b), (double)o->mvp[3], (double)(o->mvp[4] / a),
               (double)(o->mvp[5] / b), (double)o->mvp[7], (double)ip_ph_near_ref(o), (double)dm);
    }
}

int sb_interp_photo_census(sb_ph_census_t *c, int log)
{
    ip_list_t *B;
    unsigned cnt[IP_SH_MAX], cnt3[IP_SH_MAX], dcnt[IP_SH_MAX], bdcnt[IP_SH_MAX], i;
    int k, best = -1, bd = -1, wd = -1;
    D3D11_TEXTURE2D_DESC td;
    memset(c, 0, sizeof *c);
    c->bb = c->bbd = c->wld = c->wldd = -1;
    if (!ip_init_done || ip_init_fail || ip_nhist < 1) { return 0; }
    B = &g_ipl[ip_hist[ip_nhist - 1]];
    if (!B->ready || B->bbref < 0) { return 0; }
    memset(cnt, 0, sizeof cnt);
    memset(cnt3, 0, sizeof cnt3);
    memset(dcnt, 0, sizeof dcnt);
    memset(bdcnt, 0, sizeof bdcnt);
    c->ser = B->serial;
    c->frame = B->frame;
    c->nops = B->nops;
    c->bb = B->bbref;
    for (i = 0; i < B->nops; i++) {
        const ip_op_t *o = &B->ops[i];
        int e;
        if (o->kind != IP_DRAW) { continue; }
        e = ip_ph_ent_rtv(o->sh_rtv);
        if (e < 0) { continue; }
        cnt[e]++;
        if (o->cls) { cnt3[e]++; }
        if (e == B->bbref) {
            int d = ip_ph_ent_dsv(o->sh_dsv);
            c->n_bb++;
            if (o->cls) { c->n_bb3d++; } else { c->n_bb2d++; }
            if (d >= 0) { bdcnt[d]++; }
        }
    }
    for (k = 0; k < IP_SH_MAX; k++) {
        if (k != B->bbref && cnt[k]) { c->n_rts++; }
        if (k != B->bbref && cnt3[k] >= 8u && (best < 0 || cnt3[k] > cnt3[best])) { best = k; }
        if (bdcnt[k] && (bd < 0 || bdcnt[k] > bdcnt[bd])) { bd = k; }
    }
    c->bbd = bd;
    c->wld = best;
    c->mon = -1;
    if (best >= 0) { /* W290: Monitor-RT = groesstes Ziel (ohne BB), das das Welt-RT liest (wie der Beleg unten) */
        unsigned mw = 0;
        for (i = 0; i < B->nops; i++) {
            const ip_op_t *o = &B->ops[i];
            int e;
            if (o->kind != IP_DRAW || !o->sh_srv || o->sh_srv != g_ish[best].srv || o->sh_rtv == g_ish[best].rtv) { continue; }
            e = ip_ph_ent_rtv(o->sh_rtv);
            if (e >= 0 && e != B->bbref && g_ish[e].sh) {
                ID3D11Texture2D_GetDesc(g_ish[e].sh, &td);
                if (td.Width > mw) { mw = td.Width; c->mon = e; }
            }
        }
    }
    if (g_ish[B->bbref].sh) {
        ID3D11Texture2D_GetDesc(g_ish[B->bbref].sh, &td);
        c->bb_w = td.Width;
        c->bb_h = td.Height;
        c->bb_tex = g_ish[B->bbref].sh;
    }
    if (best >= 0) {
        int gotvp = 0, gotprj = 0;
        for (i = 0; i < B->nops; i++) {
            const ip_op_t *o = &B->ops[i];
            int d;
            if (o->kind != IP_DRAW || o->sh_rtv != g_ish[best].rtv) { continue; }
            c->n_w++;
            if (o->rq) { c->n_wrq++; }
            if (o->cls) { c->n_w3d++; } else {
                c->n_w2d++;
                if (o->rn[0][2] > 1.001f || o->rn[0][2] < 0.999f || o->rn[3][2] > 1.001f || o->rn[3][2] < 0.999f) { c->n_w2dw++; }
            }
            d = ip_ph_ent_dsv(o->sh_dsv);
            if (d >= 0) { dcnt[d]++; }
            if (!gotvp && o->vpg[2] && o->vpg[3] && o->rtl[0] && o->rtl[1]) {
                c->g_x = o->vpg[0]; c->g_y = o->vpg[1]; c->g_w = o->vpg[2]; c->g_h = o->vpg[3];
                c->w_lw = o->rtl[0]; c->w_lh = o->rtl[1];
                gotvp = 1;
            }
            if (!gotprj && o->cls && o->prj[0] > 0.05f && o->prj[1] > 0.05f) {
                memcpy(c->prj, o->prj, sizeof c->prj);
                gotprj = 1;
            }
        }
        for (k = 0; k < IP_SH_MAX; k++) { if (dcnt[k] && (wd < 0 || dcnt[k] > dcnt[wd])) { wd = k; } }
        c->wldd = wd;
        if (g_ish[best].sh) {
            ID3D11Texture2D_GetDesc(g_ish[best].sh, &td);
            c->w_tw = td.Width;
            c->w_th = td.Height;
            c->w_tex = g_ish[best].sh;
        }
        if (!gotvp && c->w_tw) { c->g_w = c->w_lw = c->w_tw; c->g_h = c->w_lh = c->w_th; }
        if (log && (g_cfg.photo_diag & ~16)) { ip_ph_quant(B, best, c->w_tw, c->w_lw); } /* DIAG(W249) */
        if (log && (g_cfg.photo_diag & 16)) { ip_ph_selfdiag(B, best); } /* DIAG(W428) */
    }
    for (k = 0; k < IP_SH_MAX && c->n_rt < 16u; k++) { /* RT-Ziele (ohne BB) mit Tiefe, Groesse, Draws */
        if (k == B->bbref || !cnt[k] || !g_ish[k].sh) { continue; }
        c->rt[c->n_rt].e = k;
        c->rt[c->n_rt].d = -1;
        c->rt[c->n_rt].nd = cnt[k];
        c->rt[c->n_rt].tex = g_ish[k].sh;
        ID3D11Texture2D_GetDesc(g_ish[k].sh, &td);
        c->rt[c->n_rt].w = td.Width;
        c->rt[c->n_rt].h = td.Height;
        for (i = 0; i < B->nops; i++) {
            const ip_op_t *o = &B->ops[i];
            if (o->kind == IP_DRAW && o->sh_rtv == g_ish[k].rtv && o->sh_dsv) {
                c->rt[c->n_rt].d = ip_ph_ent_dsv(o->sh_dsv);
                break;
            }
        }
        c->n_rt++;
    }
    c->ok = best >= 0 && c->w_tw > 0 && c->bb_w > 0;
    if (log) {
        char top[256];
        size_t pos = 0;
        unsigned used[IP_SH_MAX];
        int r;
        memset(used, 0, sizeof used);
        top[0] = 0;
        for (r = 0; r < 6; r++) { /* groesste RT-Ziele (Draws) fuer den Beleg */
            int m = -1;
            for (k = 0; k < IP_SH_MAX; k++) {
                if (k != B->bbref && cnt[k] && !used[k] && (m < 0 || cnt[k] > cnt[m])) { m = k; }
            }
            if (m < 0) { break; }
            used[m] = 1;
            memset(&td, 0, sizeof td);
            if (g_ish[m].sh) { ID3D11Texture2D_GetDesc(g_ish[m].sh, &td); }
            pos += (size_t)_snprintf_s(top + pos, sizeof top - pos, _TRUNCATE, "%se%d %ux%u %u/%u", r ? ", " : "", m,
                                       td.Width, td.Height, cnt[m], cnt3[m]);
        }
        if (best >= 0) { /* Beleg: Komposition Welt-RT -> Monitor-RT -> BB (Ausschnitt/UV) */
            int ncomp = 0, mon = -1, nbb = 0;
            unsigned monw = 0;
            for (i = 0; i < B->nops && ncomp < 3; i++) {
                const ip_op_t *o = &B->ops[i];
                if (o->kind == IP_DRAW && o->sh_srv && o->sh_srv == g_ish[best].srv && o->sh_rtv != g_ish[best].rtv) {
                    int e = ip_ph_ent_rtv(o->sh_rtv);
                    if (e >= 0 && g_ish[e].sh) { /* Monitor-RT = groesstes Ziel, das das Welt-RT liest */
                        ID3D11Texture2D_GetDesc(g_ish[e].sh, &td);
                        if (td.Width > monw) { monw = td.Width; mon = e; }
                    }
                    ip_ph_dump_draw(B, o, e == B->bbref ? "Welt-RT -> BB" : "Welt-RT -> Monitor-RT", NULL);
                    ncomp++;
                }
            }
            for (i = 0; mon >= 0 && mon != B->bbref && i < B->nops && nbb < 2; i++) {
                const ip_op_t *o = &B->ops[i];
                if (o->kind == IP_DRAW && o->sh_srv && o->sh_srv == g_ish[mon].srv && o->sh_rtv == g_ish[B->bbref].rtv) {
                    float bx[8];
                    ip_ph_dump_draw(B, o, "Monitor-RT -> BB", bx);
                    nbb++;
                    /* Anzeige-Seitenverhaeltnis des Kamerabilds: Pixel auf dem BB je Anteil des Kamerabilds
                     * (UV-Breite = Anteil der Monitor-RT-Breite; UV-Hoehe / Viewport-Anteil des Welt-RT). */
                    if (nbb == 1 && bx[1] > bx[0] && bx[3] > bx[2] && bx[5] > bx[4] && bx[7] > bx[6] && c->w_lh && c->g_h) {
                        float px = (bx[1] - bx[0]) * 0.5f * (float)c->bb_w, py = (bx[3] - bx[2]) * 0.5f * (float)c->bb_h;
                        float fu = bx[5] - bx[4], fv = (bx[7] - bx[6]) / ((float)c->g_h / (float)c->w_lh);
                        float as = (px / fu) / (py / fv);
                        if (as > 0.9f && as < 2.5f) { c->disp_asp = as; }
                    }
                }
            }
        }
        HLOG_I("PHOTO: Zensus Tick f=%u ser=%u ops=%u | BB e%d %ux%u Draws %u (3D %u, 2D %u) Tiefe e%d | Welt-RT e%d %ux%u "
               "logisch %ux%u Spiel-Viewport %u,%u %ux%u Draws %u (3D %u, 2D %u davon mit Clip-w %u, Shading %u) Tiefe e%d | "
               "Projektion a=%.4f b=%.4f A=%.5f B=%.5f (b/a=%.4f) | Monitor zeigt %.4f:1 | RT-Ziele %u: %s",
               c->frame, c->ser, c->nops, c->bb, c->bb_w, c->bb_h, c->n_bb, c->n_bb3d, c->n_bb2d, c->bbd, c->wld, c->w_tw,
               c->w_th, c->w_lw, c->w_lh, c->g_x, c->g_y, c->g_w, c->g_h, c->n_w, c->n_w3d, c->n_w2d, c->n_w2dw, c->n_wrq,
               c->wldd, (double)c->prj[0], (double)c->prj[1], (double)c->prj[2], (double)c->prj[3],
               c->prj[0] > 0.0f ? (double)(c->prj[1] / c->prj[0]) : 0.0, (double)c->disp_asp, c->n_rts, top);
    }
    return c->ok;
}

/* W295 WORLD-REPROJ: Tick B (Liste bi) fuer die Reprojektion vorbereiten (Spiel-Thread, ctx_lock, Immediate):
 * Welt-RT wie der Foto-Zensus (meiste 3D-Draws, >= 8), Draw-ID-Wiedergabe nur der Welt-Ops (t = 1), Bewegungsdaten
 * je Welt-Draw an render_reproj.c. Die naechste Wiedergabe setzt alle Schatten per Startkopie zurueck. */
static sb_rp_op_t *ip_rp_ops;
static unsigned ip_rp_cap;
void ip_rp_tick(ip_list_t *A, int ai, ip_list_t *B, int bi)
{
    unsigned cnt3[IP_SH_MAX], dcnt[IP_SH_MAX], i, n = 0, last = 0, nshare = 0;
    int k, we = -1, wd = -1;
    ID3D11RenderTargetView *rw;
    ID3D11DepthStencilView *rd;
    ip_rp_ser[bi] = 0;
    ip_rp_we[bi] = ip_rp_wd[bi] = -1;
    if (B->bbref < 0 || !ip_id_ps || !ip_dbg_cb) { return; }
    memset(cnt3, 0, sizeof cnt3);
    memset(dcnt, 0, sizeof dcnt);
    for (i = 0; i < B->nops; i++) {
        const ip_op_t *o = &B->ops[i];
        if (o->kind == IP_DRAW && o->cls && (k = ip_ph_ent_rtv(o->sh_rtv)) >= 0 && k != B->bbref) { cnt3[k]++; }
    }
    { /* Hauptmonitor-Welt = flaechengroesstes RT mit >= 8 3D-Draws (Frontsicht/Sub-Monitore sind kleiner) */
        unsigned ba = 0;
        static int lastwe = -2;
        for (k = 0; k < IP_SH_MAX; k++) {
            D3D11_TEXTURE2D_DESC td;
            unsigned ar;
            if (cnt3[k] < 8u || !g_ish[k].sh) { continue; }
            ID3D11Texture2D_GetDesc(g_ish[k].sh, &td);
            ar = td.Width * td.Height;
            if (we < 0 || ar > ba || (ar == ba && cnt3[k] > cnt3[we])) { we = k; ba = ar; }
        }
        if (we != lastwe && g_cfg.reproj_diag >= 1) {
            static unsigned nl;
            if (nl++ < 40u) { HLOG_I("REPROJ welt f=%u ser=%u: Welt-RT e%d (Flaeche %u, 3D-Draws %u), vorher e%d", B->frame, B->serial, we, ba, we >= 0 ? cnt3[we] : 0u, lastwe); }
            lastwe = we;
        }
    }
    if (we < 0 || !g_ish[we].sh || !g_ish[we].bak[bi] || g_ish[we].bakser[bi] != B->serial) { st_rp_skip[0]++; return; }
    rw = g_ish[we].rtv;
    for (i = 0; i < B->nops; i++) {
        const ip_op_t *o = &B->ops[i];
        if (o->kind == IP_DRAW && o->sh_rtv == rw && (k = ip_ph_ent_dsv(o->sh_dsv)) >= 0) { dcnt[k]++; }
    }
    for (k = 0; k < IP_SH_MAX; k++) { if (dcnt[k] && (wd < 0 || dcnt[k] > dcnt[wd])) { wd = k; } }
    if (wd < 0 || !g_ish[wd].bak[bi] || g_ish[wd].bakser[bi] != B->serial) { st_rp_skip[1]++; return; }
    rd = g_ish[wd].dsv;
    if (ip_rp_cap < B->nops) {
        sb_rp_op_t *p = (sb_rp_op_t *)realloc(ip_rp_ops, B->nops * sizeof *p);
        if (!p) { return; }
        ip_rp_ops = p;
        ip_rp_cap = B->nops;
    }
    { /* Welt-Durchgang = Welt-Ops bis zur ersten Op, die das Welt-RT liest (Komposition ins Monitor-RT). Spaetere
       * Welt-Ops (z. B. weitere Kamera derselben RT-Ressource) laufen wie bisher. */
        unsigned first_rd = 0xFFFFFFFFu, after = 0;
        ID3D11ShaderResourceView *ws = g_ish[we].srv;
        last = 0xFFFFFFFFu;
        for (i = 0; i < B->nops; i++) {
            const ip_op_t *o = &B->ops[i];
            int w = IP_RP_ISW(o, rw, rd);
            int rdw = o->kind == IP_DRAW && ws && (o->sh_srv == ws || (o->rq && (o->rq_shsrv[1] == ws || o->rq_shsrv[2] == ws || o->rq_shsrv[3] == ws)));
            if (rdw && !w && first_rd == 0xFFFFFFFFu) { first_rd = i; }
            if (w) { if (first_rd == 0xFFFFFFFFu) { last = i; } else { after++; } }
            else if (first_rd == 0xFFFFFFFFu && o->sh_dsv == rd && o->kind != IP_CLR_RTV) { nshare++; } /* Welt-Tiefe fremd */
        }
        if (last == 0xFFFFFFFFu) { st_rp_skip[2]++; return; }
        if (g_cfg.reproj_diag >= 1) {
            static unsigned nl2;
            if (nl2++ < 6u) {
                HLOG_I("REPROJ zensus ser=%u Welt e%d Tiefe e%d: Welt-Durchgang bis Op %u, Komposition Op %u, spaetere Welt-Ops %u, "
                       "Tiefe fremd benutzt %u, ops %u", B->serial, we, wd, last, first_rd, after, nshare, B->nops);
            }
        }
    }
    for (i = 0; i <= last; i++) {
        const ip_op_t *o = &B->ops[i];
        sb_rp_op_t *r;
        if (o->kind != IP_DRAW || !IP_RP_ISW(o, rw, rd)) { continue; }
        r = &ip_rp_ops[n++];
        memset(r, 0, sizeof *r);
        r->idx = i;
        r->lerp = B->pair_ok && o->flag && o->match >= 0 && (unsigned)o->match < A->nops;
        r->same = r->lerp && (o->rzb || A->ops[o->match].rawhash == o->rawhash);
        r->stat = o->why == IP_W_STATIC;
        r->flat = !o->cls && o->rn[0][2] > 0.999f && o->rn[0][2] < 1.001f && o->rn[3][2] > 0.999f && o->rn[3][2] < 1.001f;
        r->pre = o->cls && o->rn[0][2] > 0.999f && o->rn[0][2] < 1.001f && o->rn[3][2] > 0.999f && o->rn[3][2] < 1.001f;
        r->nv = o->nv;
        memcpy(r->mvp, o->mvp, sizeof r->mvp);
        memcpy(r->pref, o->pref, sizeof r->pref);
        if (r->lerp) {
            const ip_op_t *a = &A->ops[o->match];
            memcpy(r->amvp, IP_AMVP(o, a), sizeof r->amvp); /* W317 rzm */
            memcpy(r->apref, o->rzb ? o->pref : a->pref, sizeof r->apref); /* rzb: Tick-N-Vertices mit Matrix N-1 */
        }
        r->vp[0] = o->vp.TopLeftX; r->vp[1] = o->vp.TopLeftY; r->vp[2] = o->vp.Width; r->vp[3] = o->vp.Height;
        r->vp[4] = o->vp.MinDepth; r->vp[5] = o->vp.MaxDepth;
    }
    /* exakte Teil-Wiedergabe (t = 1) bis zur letzten Welt-Op: Farbe + Tiefe des Welt-Durchgangs; danach dieselbe mit
     * dem Draw-ID-Shader. Die naechste Wiedergabe setzt alle Schatten per Startkopie zurueck. */
    ip_rp_stop = last;
    ip_rp_idrtv = rw;
    ip_rp_prep = 1;
    ip_replay_ex(B, bi, A, ai, 1.0f, 0, ip_cx_imm());
    ip_rp_prep = 0;
    if (!g_ish[wd].sh || !sb_rp_begin(bi, g_ish[we].sh, g_ish[wd].sh)) { ip_rp_idrtv = NULL; st_rp_skip[3]++; return; }
    ip_rp_prep = 2;
    ip_replay_ex(B, bi, A, ai, 1.0f, 2, ip_cx_imm());
    ip_rp_prep = 0;
    ip_rp_idrtv = NULL;
    if (!sb_rp_tick(bi, B->serial, ip_rp_ops, n, B->nops, g_ish[we].sh)) { return; }
    ip_rp_we[bi] = we;
    ip_rp_wd[bi] = wd;
    ip_rp_last[bi] = last;
    ip_rp_ser[bi] = B->serial;
}
