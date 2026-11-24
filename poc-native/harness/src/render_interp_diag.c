/* render_interp_diag.c - FPS-INTERP Diagnose (DIAG-Schalter): Pruefsummen/Referenzwache, Draw-Analyse, Naht/Keil, GPU-Zeit, Statistikzeilen.
 * W463 RCORE: aus render_interp.c ausgegliedert (reines Verschieben); gemeinsamer Zustand in
 * render_interp_int.h, Modulkarte docs/RENDER_ARCH.md. */
#include "render_interp_int.h"

/* ---------- DIAG(W273 INTERP-RACE) interp_rhash ----------
 * Bit 1: je Zwischenbild-Wiedergabe (0<t<1, beide Contexte) Pruefsummen der CPU-Eingangsdaten
 *        (ohne Zeigerwerte): hB = Draws von B, hM = Zuordnung, hA = Partner aus A, hS = Puffer-
 *        wahl/Startkopien. Vergleich zwischen zwei Laeufen je (Tick-Bild-Nr, t).
 * Bit 2: GPU-Inhalt der Tick-Puffer per Ruecklesen (Staging): am Swap von L gemerkt und vor den
 *        Zwischenbildern von (A, B) erneut gelesen -> Abweichung = Puffer nach dem Swap ueberschrieben.
 * Nur Diagnose, Bild unveraendert (Ruecklesen nur im Spiel-Thread, synchron). */
static unsigned ip_rh_fnv(unsigned h, const void *p, size_t n)
{
    const unsigned char *b = (const unsigned char *)p;
    size_t i;
    for (i = 0; i < n; i++) { h = (h ^ b[i]) * 16777619u; }
    return h;
}
static ID3D11Buffer *ip_rh_stg;
static unsigned ip_rh_stgcap;
static unsigned st_rh_chk, st_rh_bad[6];
static unsigned ip_rh_buf(ID3D11Buffer *b, unsigned bytes)
{
    D3D11_BOX bx;
    D3D11_MAPPED_SUBRESOURCE m;
    unsigned h = 2166136261u, i, n;
    const unsigned *w;
    if (!b || !bytes) { return 0u; }
    if (!ip_rh_stg || ip_rh_stgcap < bytes) {
        D3D11_BUFFER_DESC bd;
        if (ip_rh_stg) { ID3D11Buffer_Release(ip_rh_stg); ip_rh_stg = NULL; }
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = bytes + bytes / 2u;
        bd.Usage = D3D11_USAGE_STAGING;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &ip_rh_stg))) { ip_rh_stg = NULL; ip_rh_stgcap = 0; return 1u; }
        ip_rh_stgcap = bd.ByteWidth;
    }
    memset(&bx, 0, sizeof bx);
    bx.right = bytes; bx.bottom = 1; bx.back = 1;
    ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)ip_rh_stg, 0, 0, 0, 0, (ID3D11Resource *)b, 0, &bx);
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)ip_rh_stg, 0, D3D11_MAP_READ, 0, &m))) { return 2u; }
    w = (const unsigned *)m.pData;
    n = bytes / 4u;
    for (i = 0; i < n; i++) { h = (h ^ w[i]) * 16777619u; }
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)ip_rh_stg, 0);
    return h | 16u; /* nie 0..15 (Fehlercodes) */
}
void ip_rh_take(const ip_list_t *L, unsigned out[6])
{
    out[0] = ip_rh_buf(L->vb, L->nverts * (unsigned)sizeof(im_vertex_t));
    out[1] = (L->gpu_ok & 1u) ? ip_rh_buf(L->vbg, L->nverts * (unsigned)sizeof(im_vertex_t)) : 0u;
    out[2] = ip_rh_buf(L->rqx, L->rqxpos > L->rqxcap ? L->rqxcap : L->rqxpos);
    out[3] = (L->gpu_ok & 2u) ? ip_rh_buf(L->rqxg, L->rqxpos > L->rqxcap ? L->rqxcap : L->rqxpos) : 0u;
    out[4] = ip_rh_buf(L->rqc, L->rqcpos * 16u);
    out[5] = L->wb_ok ? ip_rh_buf(L->wb, L->nverts * (unsigned)sizeof(float)) : 0u;
}
void ip_rh_check(const ip_list_t *L, const char *who, unsigned fr, int k)
{
    static const char *const nm[6] = { "vb", "vbg", "rqx", "rqxg", "rqc", "wb" };
    unsigned now[6];
    int q;
    if (!L || !L->ready) { return; }
    ip_rh_take(L, now);
    st_rh_chk++;
    for (q = 0; q < 6; q++) {
        if (now[q] != L->rh[q]) {
            st_rh_bad[q]++;
            if (st_rh_bad[q] <= 40u) {
                HLOG_W("INTERP rhash: Puffer %s von %s (Tick f=%u ser=%u) geaendert seit Swap: %08X -> %08X (Bild f=%u k=%d)",
                       nm[q], who, L->frame, L->serial, L->rh[q], now[q], fr, k);
            }
        }
    }
    if ((st_rh_chk % 2000u) == 0u) {
        HLOG_I("INTERP rhash: %u Pruefungen, geaendert vb=%u vbg=%u rqx=%u rqxg=%u rqc=%u wb=%u", st_rh_chk,
               st_rh_bad[0], st_rh_bad[1], st_rh_bad[2], st_rh_bad[3], st_rh_bad[4], st_rh_bad[5]);
    }
}
void ip_rh_inputs(const ip_list_t *B, int bi, const ip_list_t *A, int ai, float t, const ip_cx_t *x,
                         int vbgA, int vbgB, int ring)
{
    unsigned hB = 2166136261u, hM = 2166136261u, hA = 2166136261u, hS = 2166136261u, i, nl = 0;
    int j;
    for (i = 0; i < B->nops; i++) {
        const ip_op_t *o = &B->ops[i];
        unsigned v[10];
        v[0] = o->kind; v[1] = o->topo; v[2] = o->vstart; v[3] = o->nv; v[4] = o->ib1; v[5] = o->nsv;
        v[6] = o->has_tex; v[7] = o->outhash; v[8] = o->rq; v[9] = o->nolerp;
        hB = ip_rh_fnv(hB, v, sizeof v);
        hB = ip_rh_fnv(hB, o->mvp, sizeof o->mvp);
        if (o->kind != IP_DRAW) { continue; }
        v[0] = o->flag; v[1] = o->why; v[2] = (unsigned)o->match; v[3] = (unsigned)o->rzb;
        hM = ip_rh_fnv(hM, v, (o->rzb ? 4 : 3) * sizeof v[0]); /* W284: rzb nur wenn gesetzt (Pruefsumme sonst wie bisher) */
        if (o->flag && o->match >= 0 && (unsigned)o->match < A->nops) {
            const ip_op_t *a = &A->ops[o->match];
            v[0] = a->vstart; v[1] = a->nv; v[2] = a->ib1; v[3] = a->nsv; v[4] = a->outhash;
            hA = ip_rh_fnv(hA, v, 5 * sizeof v[0]);
            hA = ip_rh_fnv(hA, a->mvp, sizeof a->mvp);
            nl++;
        }
    }
    {
        unsigned v[8];
        v[0] = (unsigned)vbgA; v[1] = (unsigned)vbgB; v[2] = (unsigned)ring; v[3] = A->gpu_ok; v[4] = B->gpu_ok;
        v[5] = (unsigned)A->wb_ok | ((unsigned)B->wb_ok << 1); v[6] = B->nverts; v[7] = A->nverts;
        hS = ip_rh_fnv(hS, v, sizeof v);
    }
    for (j = 0; j < B->nrefs; j++) {
        const ip_sh_t *e = &g_ish[B->refs[j]];
        unsigned v[2];
        v[0] = (unsigned)B->refs[j];
        v[1] = (ai >= 0 && e->bakser[ai] == A->serial) ? 1u : (e->bakser[bi] == B->serial ? 2u : 0u);
        hS = ip_rh_fnv(hS, v, sizeof v);
    }
    HLOG_I("INTERP rhash f=%u t=%.3f ctx=%d ops=%u lerp=%u hB=%08X hM=%08X hA=%08X hS=%08X", B->frame, (double)t,
           x->deferred, B->nops, nl, hB, hM, hA, hS);
}

/* W295 WORLD-REPROJ (interp_mode=reproj): je Liste Welt-Schatten-Eintrag, Tiefe, letzte Welt-Op; waehrend der
 * Vorbereitung am Swap: Teil-Wiedergaben bis zur letzten Welt-Op (ip_rp_prep, ip_rp_stop). */
int ip_rp_we[IP_LISTS] = { -1, -1, -1, -1 }, ip_rp_wd[IP_LISTS] = { -1, -1, -1, -1 };
unsigned ip_rp_last[IP_LISTS], ip_rp_ser[IP_LISTS];
unsigned st_rp_skip[4]; /* W295 DIAG: Ticks ohne Reprojektion (kein Welt-RT, keine Tiefe, kein Durchgang, Ressourcen) */
ID3D11RenderTargetView *ip_rp_idrtv; /* Welt-Schatten waehrend der Vorbereitung (ID-Durchgang: auf 0 loeschen) */
int ip_rp_prep;      /* Vorbereitung am Swap: 1 = exakte Teil-Wiedergabe (Farbe/Tiefe), 2 = Draw-ID */
unsigned ip_rp_stop; /* ... nur Ops 0..ip_rp_stop (bis zur letzten Welt-Op vor der Komposition) */

/* W361 LOC-INTERP-CRASH: Sind die Schatten-Eintraege der Liste noch die beim Sichern? (Generation je Eintrag.)
 * Nach Entwurf nie verletzt (Verdraengung nur ohne lebende Sicherung, vorher ip_rt_quiesce); falls doch, bleiben
 * die Views dank refv gueltig - hier nur zaehlen und melden (Bild unveraendert). */
int ip_list_refs_check(const ip_list_t *L, int li, const char *wo)
{
    int j;
    for (j = 0; j < L->nrefs && j < IP_REFS_MAX; j++) {
        int e = L->refs[j];
        if (e < 0 || e >= IP_SH_MAX || !ip_shref_same(&L->refo[j], g_ish[e].gen)) {
            LONG n = InterlockedIncrement(&st_sh_stale_tot);
            InterlockedIncrement(&st_sh_stale);
            if (n <= 8) {
                HLOG_W("INTERP W361: Liste %d (%s, Tick f=%u ser=%u) nutzt ersetzten Schatten-Eintrag %d (Generation %u -> %u) "
                       "- Views der Liste gehalten, kein Absturz", li, wo, L->frame, L->serial, e, L->refo[j].gen,
                       (e >= 0 && e < IP_SH_MAX) ? g_ish[e].gen : 0u);
            }
            return 0;
        }
    }
    return 1;
}

/* W361: oeffentlicher Referenzzaehler einer View (AddRef/Release-Rueckgabe). */
static ULONG ip_view_refs(ID3D11View *v)
{
    ID3D11View_AddRef(v);
    return ID3D11View_Release(v);
}

/* W361 LOC-INTERP-CRASH: Zaehlerwache der Schatten-Views. Soll = 1 (Eintrag) + je Liste, die den Eintrag mit
 * eigener Referenz haelt (refo), 1. Weniger = fremde Ueberfreigabe (Minidumps P0/A5a: SRV mit Zaehler -4/-3
 * zerstoert, RTV/DSV desselben Eintrags heil) -> melden und die Differenz selbst halten (Leck statt Absturz;
 * Abgabe beim Freigeben des Eintrags). Am Swap unter ctx_lock (fix=1) und vor jedem Render-Thread-Auftrag (fix=0,
 * nur Eintraege der Auftragslisten). */
volatile LONG st_rc_bad, st_rc_fix, st_rc_dead;
ID3D11View *volatile ip_dead_v[IP_DEAD_MAX]; /* tot gefundene Views (Render-Thread), Quarantaene in ip_show */
volatile LONG ip_dead_n;
int ip_sh_rc_watch(int fix, const ip_list_t *A, const ip_list_t *B, const char *wo)
{
    int e, k, j, q, dead = 0;
    for (e = 0; e < IP_SH_MAX; e++) {
        ip_sh_t *s = &g_ish[e];
        ID3D11View *vw[3];
        if (!s->res) { continue; }
        if ((A || B) && !fix) { /* nur Eintraege der Auftragslisten */
            int use = 0;
            for (j = 0; B && j < B->nrefs && !use; j++) { use = (B->refs[j] == e); }
            for (j = 0; A && j < A->nrefs && !use; j++) { use = (A->refs[j] == e); }
            if (!use) { continue; }
        }
        vw[0] = (ID3D11View *)s->rtv; vw[1] = (ID3D11View *)s->dsv; vw[2] = (ID3D11View *)s->srv;
        for (q = 0; q < 3; q++) {
            ULONG soll = 1u + s->pin[q], ist;
            if (!vw[q]) { continue; }
            for (k = 0; k < IP_LISTS; k++) {
                const ip_list_t *L = &g_ipl[k];
                if (!fix && L != A && L != B) { continue; } /* Render-Thread: nur gepinnte Listen (Untergrenze) */
                for (j = 0; j < L->nrefs && j < IP_REFS_MAX; j++) {
                    if (L->refs[j] == e && L->refo[j].v[q] == vw[q]) { soll++; }
                }
            }
            ist = ip_view_refs(vw[q]);
            if (fix ? (ist == soll) : (ist >= soll)) { continue; }
            if (ist >= 0x80000000u) { /* d3d11-Zerstoerungsmarke (Zaehler + 0xC0000001): nicht mehr binden */
                LONG n = InterlockedIncrement(&ip_dead_n);
                if (n <= IP_DEAD_MAX) { ip_dead_v[n - 1] = vw[q]; }
                InterlockedIncrement(&st_rc_dead);
                dead = 1;
            }
            if (InterlockedIncrement(&st_rc_bad) <= 24) {
                HLOG_W("INTERP W361 Zaehler (%s): Schatten %d %s %p soll %lu ist %lu (Tick f=%u)%s", wo, e,
                       q == 0 ? "rtv" : (q == 1 ? "dsv" : "srv"), (void *)vw[q], soll, ist, g_present_no,
                       ist >= 0x80000000u ? " -> zerstoert, Bild entfaellt, Quarantaene" :
                       (ist < soll && ist >= 1u ? " -> Differenz gehalten" : ""));
            }
            if (ist < soll && ist >= 1u) { /* lebt noch: Differenz selbst halten (auch im Render-Thread, atomar) */
                ULONG d = soll - ist;
                InterlockedExchangeAdd((volatile LONG *)&s->pin[q], (LONG)d);
                while (d--) { ID3D11View_AddRef(vw[q]); }
                InterlockedIncrement(&st_rc_fix);
            }
        }
    }
    return dead;
}


/* ---------- DIAG (interp_diag>=3): Draw-Analyse eines Bildfolgen-Ticks ---------- */

static const char *kIP_WHY[IP_W_N] = { "-", "static", "lerp", "rawlerp", "nomatch",
                                       "rawchg", "reject", "bb2d", "rt2dlerp", "rt2dex" };

/* Pixel-Huellrechteck der Vertices [vs, vs+n) einer Liste (mvp, Viewport vp).
 * Rueckgabe: Zahl Vertices mit w <= 0. */
static unsigned ip_bbox(float (*P)[4], unsigned vs, unsigned n, const float m[16],
                        const D3D11_VIEWPORT *vp, float bb[4])
{
    unsigned i, behind = 0;
    bb[0] = bb[1] = 1e30f;
    bb[2] = bb[3] = -1e30f;
    for (i = 0; i < n; i++) {
        float c[4], x, y;
        ip_clip(P[vs + i], m, c);
        if (c[3] <= 1e-6f) { behind++; continue; }
        x = vp->TopLeftX + (c[0] / c[3] * 0.5f + 0.5f) * vp->Width;
        y = vp->TopLeftY + (0.5f - c[1] / c[3] * 0.5f) * vp->Height;
        if (x < bb[0]) { bb[0] = x; }
        if (y < bb[1]) { bb[1] = y; }
        if (x > bb[2]) { bb[2] = x; }
        if (y > bb[3]) { bb[3] = y; }
    }
    return behind;
}

static int ip_sh_of_rtv(ID3D11RenderTargetView *v)
{
    int e;
    if (!v) { return -1; }
    for (e = 0; e < IP_SH_MAX; e++) {
        if (g_ish[e].rtv == v) { return e; }
    }
    return -1;
}

void ip_diag_tick(ip_list_t *A, int ai, ip_list_t *B, int bi)
{
    static unsigned ndone;
    unsigned i, cnt[IP_W_N];
    float (*PA)[4] = ip_cpos[ai], (*PB)[4] = ip_cpos[bi];
    if (!PA || !PB || (ndone >= 48u && !g_cfg.interp_ddfile[0])) { return; } /* W284: mit Tick-Liste ohne Grenze */
    ndone++;
    memset(cnt, 0, sizeof cnt);
    for (i = 0; i < B->nops; i++) {
        const ip_op_t *o = &B->ops[i];
        float bb[4], ba[4];
        unsigned beh, behA = 0;
        char extra[200];
        if (o->kind != IP_DRAW) { continue; }
        cnt[o->why < IP_W_N ? o->why : 0]++;
        if (o->why == IP_W_BB2D) { continue; }
        beh = ip_bbox(PB, o->vstart, o->nv, o->mvp, &o->vp, bb);
        extra[0] = 0;
        ba[0] = ba[1] = ba[2] = ba[3] = 0;
        if (o->match >= 0 && (unsigned)o->match < A->nops) {
            const ip_op_t *a = &A->ops[o->match];
            behA = ip_bbox(PA, a->vstart, a->nv, a->mvp, &a->vp, ba);
            if (a->nv == o->nv && (o->why == IP_W_RAWCHG || o->why == IP_W_RAWLERP ||
                                   o->why == IP_W_RT2DLERP || o->why == IP_W_RT2DEX ||
                                   o->why == IP_W_REJECT || o->why == IP_W_LERP)) {
                /* Vertex i <-> i: Eingangs-Differenz (Mittel, max. Abweichung) und
                 * Bild-Sprung je Vertex (max / Anteil > 8 px). */
                double dm[3] = { 0, 0, 0 }, dev = 0, jmax = 0;
                unsigned k, nbig = 0, nj = 0;
                for (k = 0; k < o->nv; k++) {
                    int c;
                    for (c = 0; c < 3; c++) { dm[c] += PB[o->vstart + k][c] - PA[a->vstart + k][c]; }
                }
                for (k = 0; k < 3; k++) { dm[k] /= (double)o->nv; }
                for (k = 0; k < o->nv; k++) {
                    float c0[4], c1[4];
                    int c;
                    for (c = 0; c < 3; c++) {
                        double d = PB[o->vstart + k][c] - PA[a->vstart + k][c] - dm[c];
                        if (d < 0) { d = -d; }
                        if (d > dev) { dev = d; }
                    }
                    ip_clip(PA[a->vstart + k], a->mvp, c0);
                    ip_clip(PB[o->vstart + k], o->mvp, c1);
                    if (c0[3] > 1e-6f && c1[3] > 1e-6f) {
                        double dx = (c1[0] / c1[3] - c0[0] / c0[3]) * 0.5 * o->vp.Width;
                        double dy = (c1[1] / c1[3] - c0[1] / c0[3]) * 0.5 * o->vp.Height;
                        double j = sqrt(dx * dx + dy * dy);
                        if (j > jmax) { jmax = j; }
                        if (j > 8.0) { nbig++; }
                        nj++;
                    }
                }
                _snprintf_s(extra, sizeof extra, _TRUNCATE,
                            " dmean=%.3f,%.3f,%.3f dev=%.3f jmax=%.1f big=%u/%u",
                            dm[0], dm[1], dm[2], dev, jmax, nbig, nj);
            }
        } else if (o->why == IP_W_NOMATCH) {
            /* Kandidat gleicher Signatur ohne Vertexzahl im Vortick */
            unsigned j;
            for (j = 0; j < A->nops; j++) {
                const ip_op_t *a = &A->ops[j];
                if (a->kind == IP_DRAW && a->sig0 == o->sig0 && !a->taken) {
                    behA = ip_bbox(PA, a->vstart, a->nv, a->mvp, &a->vp, ba);
                    _snprintf_s(extra, sizeof extra, _TRUNCATE, " cand=%u n0=%u raw0=%s", j, a->nv,
                                a->rawhash == o->rawhash ? "gleich" : "neu");
                    break;
                }
            }
            if (!extra[0]) {
                /* gleiches Ziel + Vertexzahl + Eingangsdaten: welcher Signatur-Teil wechselt? */
                for (j = 0; j < A->nops; j++) {
                    const ip_op_t *a = &A->ops[j];
                    if (a->kind == IP_DRAW && a->rtv == o->rtv && a->nv == o->nv &&
                        a->rawhash == o->rawhash) {
                        behA = ip_bbox(PA, a->vstart, a->nv, a->mvp, &a->vp, ba);
                        _snprintf_s(extra, sizeof extra, _TRUNCATE,
                                    " same=%u tk=%u vs=%X/%X xp=%X/%X ps=%X/%X tex=%u/%u topo=%u/%u cls=%u/%u",
                                    j, a->taken, a->pvs, o->pvs, a->pxp, o->pxp, a->pps, o->pps,
                                    a->has_tex, o->has_tex, a->topo, o->topo, a->cls, o->cls);
                        break;
                    }
                }
            }
        }
        HLOG_I("INTERP dd f=%u i=%u w=%s e=%d vp=%.0fx%.0f cls=%u n=%u m=%d bb=%.0f,%.0f,%.0f,%.0f beh=%u bbA=%.0f,%.0f,%.0f,%.0f behA=%u mvpeq=%d%s",
               B->frame, i, kIP_WHY[o->why < IP_W_N ? o->why : 0], ip_sh_of_rtv(o->sh_rtv),
               o->vp.Width, o->vp.Height, o->cls, o->nv, o->match, bb[0], bb[1], bb[2], bb[3], beh,
               ba[0], ba[1], ba[2], ba[3], behA,
               (o->match >= 0 && (unsigned)o->match < A->nops &&
                memcmp(A->ops[o->match].mvp, o->mvp, sizeof o->mvp) == 0) ? 1 : 0, extra);
        if (ip_dd_on()) { /* DIAG(W251 INTERP-GROUND): Zustand + Huelle des Lerps bei t=0,25 */
            float tb[4] = { 1e30f, 1e30f, -1e30f, -1e30f };
            unsigned tbeh = 0;
            const ip_op_t *a = (o->match >= 0 && (unsigned)o->match < A->nops) ? &A->ops[o->match] : NULL;
            if (a && a->nv == o->nv && o->flag) {
                unsigned k;
                for (k = 0; k < o->nv; k++) {
                    float c0[4], c1[4], c[4], px, py;
                    int q;
                    ip_clip(PA[a->vstart + k], a->mvp, c0);
                    ip_clip(PB[o->vstart + k], o->mvp, c1);
                    for (q = 0; q < 4; q++) { c[q] = 0.75f * c0[q] + 0.25f * c1[q]; }
                    if (c[3] <= 1e-6f) { tbeh++; continue; }
                    px = o->vp.TopLeftX + (c[0] / c[3] * 0.5f + 0.5f) * o->vp.Width;
                    py = o->vp.TopLeftY + (0.5f - c[1] / c[3] * 0.5f) * o->vp.Height;
                    if (px < tb[0]) { tb[0] = px; }
                    if (py < tb[1]) { tb[1] = py; }
                    if (px > tb[2]) { tb[2] = px; }
                    if (py > tb[3]) { tb[3] = py; }
                }
            }
            HLOG_I("INTERP dd2 f=%u i=%u flag=%d rq=%d srv=%p asrv=%p pxp=%X/%X ht=%X nol=%d bs=%p ds=%p topo=%u vs=%u/%u tbb=%.0f,%.0f,%.0f,%.0f tbeh=%u pr=%.3f,%.3f,%.3f,%.3f",
                   B->frame, i, o->flag, o->rq, (void *)o->sh_srv, a ? (void *)a->sh_srv : NULL, a ? a->pxp : 0u, o->pxp,
                   o->has_tex, o->nolerp, (void *)o->bs, (void *)o->ds, o->topo, o->vstart, a ? a->vstart : 0u,
                   tb[0], tb[1], tb[2], tb[3], tbeh, o->prj[0], o->prj[1], o->prj[2], o->prj[3]);
        }
    }
    HLOG_I("INTERP ddsum f=%u static=%u lerp=%u rawlerp=%u nomatch=%u rawchg=%u reject=%u bb2d=%u rt2dlerp=%u rt2dex=%u",
           B->frame, cnt[1], cnt[2], cnt[3], cnt[4], cnt[5], cnt[6], cnt[7], cnt[8], cnt[9]);
    for (i = 0; i < IP_SH_MAX; i++) {
        if (g_ish[i].res) {
            D3D11_TEXTURE2D_DESC dd;
            ID3D11Texture2D_GetDesc(g_ish[i].sh, &dd);
            HLOG_I("INTERP ddsh e=%u %ux%u bind=0x%X%s", i, dd.Width, dd.Height, dd.BindFlags,
                   (int)i == B->bbref ? " (BB)" : "");
        }
    }
}
static sd_ent_t *sd_e;
static unsigned sd_cap;
static unsigned sd_cls_n[IP_W_N * IP_W_N + 1], sd_cls_bad[IP_W_N * IP_W_N + 1];
static float sd_cls_max[IP_W_N * IP_W_N + 1];
static unsigned sd_ticks, sd_lines, sd_groups, sd_bad05, sd_bad2, sd_badA;
static unsigned sd_mx[2][7]; /* W429 DIAG: exakte Seite gemischter Spalte [3D|P2][Vertex-Regel, interp_pass, Ablage, nomatch, rawchg, reject, sonst] */
static unsigned sd_rj_draws[2], sd_rj_verts[2], sd_rj_bad[2], sd_rjl; /* [0] 3D-Matrix, [1] P2 (vorab transformiert) */
static float sd_rj_max[2], sd_rj_dy[2];
static double sd_rj_sum[2];
static double sd_maxs;
/* DIAG(W410 LOC-GFX-JITTER): falsche Vertex-Partner ("Keile"). Fuer jeden gelerpten Welt-Draw (flag) wird je
 * Vertex k die Vortick-Lage c0_k (wie in der Wiedergabe: Vortick-Vertex bzw. Tick-N-Vertex bei rzb, Matrix N-1)
 * mit der Kamerabewegung D = M_A^-1 * M_B des Ziels (rz_cam, beste von bis zu 8) nach Tick N gebracht und mit
 * c1_k verglichen (Pixel). Ruhende Welt mit richtigem Partner: Fehler ~0. Ein falscher Partner (anderer Weltpunkt
 * je Vertex) zieht im Zwischenbild Dreiecke von der alten zur neuen Lage = Keil. Gezaehlt: Draws mit einem Vertex
 * > 8 px UND > 4x Median des Draws (starr bewegte Objekte wie VTs haben gleichmaessigen Fehler). Nur Diagnose. */
static unsigned kd_ticks, kd_draws[IP_W_N], kd_bad[IP_W_N], kd_badrzb, kd_lines;
static float kd_max[IP_W_N];
static float *kd_e;
static unsigned kd_ecap;
static int kd_fcmp(const void *x, const void *y)
{
    float a = *(const float *)x, b = *(const float *)y;
    return a < b ? -1 : (a > b ? 1 : 0);
}
static void ip_wedge_diag(ip_list_t *A, float (*PA)[4], ip_list_t *B, float (*PB)[4])
{
    double D[8][16];
    unsigned i, k;
    rz_cam_pre(A, B);
    for (i = 0; i < B->nops; i++) {
        const ip_op_t *o = &B->ops[i];
        const ip_op_t *a = (o->match >= 0 && (unsigned)o->match < A->nops) ? &A->ops[o->match] : NULL;
        int nD, x, bestx = -1;
        float best = 1e30f, bmed = 0.0f;
        unsigned bkmax = 0, bnbad = 0, bnv = 0, w = o->why < IP_W_N ? o->why : 0;
        if (o->kind != IP_DRAW || !o->cls || !o->flag || !a || (!o->rzb && a->nv != o->nv) || !o->nv) { continue; }
        nD = rz_cam(A, B, o, D);
        if (!nD) { continue; }
        if (kd_ecap < o->nv * 2u) {
            float *ne = (float *)realloc(kd_e, (size_t)o->nv * 2u * sizeof *kd_e);
            if (!ne) { return; }
            kd_e = ne; kd_ecap = o->nv * 2u;
        }
        for (x = 0; x < nD; x++) {
            unsigned nv = 0, kmax = 0, nbad = 0;
            float emax = 0.0f, med;
            for (k = 0; k < o->nv; k++) {
                float c0[4], c1[4], p[4], px1, py1, px0, py0, dx, dy, e;
                int j;
                ip_clip(o->rzb ? PB[o->vstart + k] : PA[a->vstart + k], IP_AMVP(o, a), c0);
                ip_clip(PB[o->vstart + k], o->mvp, c1);
                if (c0[3] <= 1e-6f || c1[3] <= 1e-6f) { continue; }
                px1 = (c1[0] / c1[3] * 0.5f + 0.5f) * o->vp.Width; py1 = (0.5f - c1[1] / c1[3] * 0.5f) * o->vp.Height;
                if (px1 < -0.5f * o->vp.Width || px1 > 1.5f * o->vp.Width || py1 < -0.5f * o->vp.Height || py1 > 1.5f * o->vp.Height) { continue; }
                for (j = 0; j < 4; j++) {
                    p[j] = (float)(c0[0] * D[x][0 * 4 + j] + c0[1] * D[x][1 * 4 + j] + c0[2] * D[x][2 * 4 + j] + c0[3] * D[x][3 * 4 + j]);
                }
                if (p[3] <= 1e-6f) { e = 1e6f; }
                else {
                    px0 = (p[0] / p[3] * 0.5f + 0.5f) * o->vp.Width; py0 = (0.5f - p[1] / p[3] * 0.5f) * o->vp.Height;
                    dx = px1 - px0; dy = py1 - py0;
                    e = sqrtf(dx * dx + dy * dy);
                }
                kd_e[nv++] = e;
                if (e > emax) { emax = e; kmax = k; }
            }
            if (!nv) { break; }
            memcpy(kd_e + nv, kd_e, nv * sizeof *kd_e);
            qsort(kd_e + nv, nv, sizeof *kd_e, kd_fcmp);
            med = kd_e[nv + nv / 2u];
            for (k = 0; k < nv; k++) { if (kd_e[k] > 8.0f && kd_e[k] > 4.0f * med) { nbad++; } }
            if (emax < best) { best = emax; bestx = x; bmed = med; bkmax = kmax; bnbad = nbad; bnv = nv; }
        }
        if (bestx < 0) { continue; }
        kd_draws[w]++;
        if (bnbad) {
            kd_bad[w]++;
            if (o->rzb) { kd_badrzb++; }
            if (best > kd_max[w]) { kd_max[w] = best; }
            if (kd_lines < (unsigned)g_cfg.seam_diag) {
                kd_lines++;
                HLOG_I("KEIL f=%u i=%u n=%u/%u %s rzb=%u m=%d raw=%s max=%.1fpx (Vertex %u) median=%.2fpx keil_vertices=%u/%u",
                       B->frame, i, o->nv, a->nv, kIP_WHY[w], o->rzb, o->match, a->rawhash == o->rawhash ? "gleich" : "neu",
                       (double)best, bkmax, (double)bmed, bnbad, bnv);
            }
        }
    }
    if (++kd_ticks >= 100u) {
        HLOG_I("KEILSUM ticks=%u lerp=%u/%u(max %.1f) rawlerp=%u/%u(max %.1f) rt2dlerp=%u/%u davon_rzb=%u (keil/gepruefte Draws)",
               kd_ticks, kd_bad[IP_W_LERP], kd_draws[IP_W_LERP], (double)kd_max[IP_W_LERP], kd_bad[IP_W_RAWLERP],
               kd_draws[IP_W_RAWLERP], (double)kd_max[IP_W_RAWLERP], kd_bad[IP_W_RT2DLERP], kd_draws[IP_W_RT2DLERP], kd_badrzb);
        kd_ticks = 0; kd_badrzb = 0;
        memset(kd_draws, 0, sizeof kd_draws); memset(kd_bad, 0, sizeof kd_bad); memset(kd_max, 0, sizeof kd_max);
    }
}

static int sd_cmp(const void *x, const void *y)
{
    const sd_ent_t *a = (const sd_ent_t *)x, *b = (const sd_ent_t *)y;
    if (a->key != b->key) { return a->key < b->key ? -1 : 1; }
    return a->op < b->op ? -1 : (a->op > b->op ? 1 : 0);
}
void ip_seam_diag(ip_list_t *A, int ai, ip_list_t *B, int bi)
{
    const float T = 0.25f;
    float (*PA)[4] = ip_cpos[ai], (*PB)[4] = ip_cpos[bi];
    ID3D11RenderTargetView *rts[16];
    unsigned nrt = 0, ne = 0, i, k, g0, gtick = 0, gbad05 = 0, gbad2 = 0, gbadA = 0;
    double tmax = 0.0;
    struct { float s, dy; unsigned i0, i1, w0, w1; float x, y; float e[4]; unsigned k0, k1; } worst[3]; /* W429: e = echte w N-1/N beider Ecken */
    memset(worst, 0, sizeof worst);
    if (!PA || !PB || !A || !B || !(g_sba_loc ? loc_render_interp_mission() : sb_in_mission()) ||
        (g_cfg.pf_ring > 0 && !sb_pf_active())) { return; } /* mit pf_ring nur in den Bildfolgen; W410: auch LoC-Mission */
    ip_wedge_diag(A, PA, B, PB);
    for (i = 0; i < B->nops; i++) {
        const ip_op_t *o = &B->ops[i];
        const ip_op_t *a = (o->match >= 0 && (unsigned)o->match < A->nops) ? &A->ops[o->match] : NULL;
        unsigned r;
        if (o->kind != IP_DRAW || !o->cls || o->why == IP_W_BB2D || o->why == IP_W_STATIC || !o->nv) { continue; } /* unbewegt (Cockpit) bewegt sich nicht gegen Nachbarn */
        for (r = 0; r < nrt && rts[r] != o->rtv; r++) {}
        if (r == nrt) { if (nrt >= 16u) { continue; } rts[nrt++] = o->rtv; }
        if (ne + o->nv > sd_cap) {
            unsigned nc = (ne + o->nv) * 2u;
            sd_ent_t *ne2 = (sd_ent_t *)realloc(sd_e, (size_t)nc * sizeof *sd_e);
            if (!ne2) { return; }
            sd_e = ne2; sd_cap = nc;
        }
        for (k = 0; k < o->nv; k++) {
            float c1[4], c[4], c0[4], px, py;
            int q, lerp = 0;
            long long qx, qy;
            float sew0 = 0.0f, sew1 = 0.0f; /* W429 DIAG */
            ip_clip(PB[o->vstart + k], o->mvp, c1);
            if (c1[3] <= 1e-6f) { continue; }
            px = o->vp.TopLeftX + (c1[0] / c1[3] * 0.5f + 0.5f) * o->vp.Width;
            py = o->vp.TopLeftY + (0.5f - c1[1] / c1[3] * 0.5f) * o->vp.Height;
            if (px < o->vp.TopLeftX || px > o->vp.TopLeftX + o->vp.Width || py < o->vp.TopLeftY || py > o->vp.TopLeftY + o->vp.Height) { continue; } /* nur sichtbare Ecken */
            memcpy(c, c1, sizeof c);
            c0[0] = c0[1] = c0[2] = 0.0f; c0[3] = -1.0f;
            if (o->flag && a && (o->rzb || k < a->nv)) {
                /* W429: wie der Zwischenbild-VS - echte w (interp_hlerp-Strom), homogener Lerp (Modus >= 2),
                 * W224-Regel mit echter w, Bildsprung-Regel nur in den W237-Modi 1/2 */
                float hw0 = 1.0f, hw1 = 1.0f, ew0, ew1, h0[4], h1[4];
                const int hl = sb_ip_hlerp();
                int pre = 0;
                ip_clip(o->rzb ? PB[o->vstart + k] : PA[a->vstart + k], IP_AMVP(o, a), c0);
                if (hl && A->wb_ok && B->wb_ok && A->ipw && B->ipw && o->vstart + k < B->ipwcap &&
                    (o->rzb || a->vstart + k < A->ipwcap)) {
                    hw1 = B->ipw[o->vstart + k];
                    hw0 = o->rzb ? hw1 : A->ipw[a->vstart + k];
                }
                ew0 = c0[3] * hw0; ew1 = c1[3] * hw1;
                sew0 = ew0; sew1 = ew1;
                memcpy(h0, c0, sizeof h0); memcpy(h1, c1, sizeof h1);
                if (hl >= 2 && c0[3] == 1.0f && c1[3] == 1.0f) {
                    pre = 1;
                    for (q = 0; q < 4; q++) { h0[q] *= hw0; h1[q] *= hw1; }
                }
                if (!(g_cfg.interp_eyefix && ew0 * ew1 <= 0.0f)) {
                    int jmp = 0;
                    if (g_cfg.interp_eyefix && hl && hl != 3 && ew0 > 0.0f && ew1 > 0.0f) {
                        float nx0 = c0[0] / c0[3], ny0 = c0[1] / c0[3], nx1 = c1[0] / c1[3], ny1 = c1[1] / c1[3];
                        jmp = fabsf(nx1 - nx0) > 0.5f * (1.0f + fmaxf(fabsf(nx0), fabsf(nx1))) ||
                              fabsf(ny1 - ny0) > 0.5f * (1.0f + fmaxf(fabsf(ny0), fabsf(ny1)));
                    }
                    if (!jmp) {
                        for (q = 0; q < 4; q++) { c[q] = (1.0f - T) * h0[q] + T * h1[q]; }
                        if (pre && c[3] != 0.0f) { c[0] /= c[3]; c[1] /= c[3]; c[2] /= c[3]; c[3] = 1.0f; }
                        lerp = 1;
                    }
                }
            }
            if (c[3] <= 1e-6f) { continue; }
            qx = (long long)floor((double)px * 32.0);
            qy = (long long)floor((double)py * 32.0);
            sd_e[ne].key = ((unsigned long long)r << 48) | ((unsigned long long)(qx & 0xFFFFFF) << 24) | (unsigned long long)(qy & 0xFFFFFF);
            sd_e[ne].op = i;
            sd_e[ne].nx = px; sd_e[ne].ny = py;
            sd_e[ne].tx = o->vp.TopLeftX + (c[0] / c[3] * 0.5f + 0.5f) * o->vp.Width;
            sd_e[ne].ty = o->vp.TopLeftY + (0.5f - c[1] / c[3] * 0.5f) * o->vp.Height;
            if (lerp && !o->rzb && c0[3] > 1e-6f) {
                sd_e[ne].ax = o->vp.TopLeftX + (c0[0] / c0[3] * 0.5f + 0.5f) * o->vp.Width;
                sd_e[ne].ay = o->vp.TopLeftY + (0.5f - c0[1] / c0[3] * 0.5f) * o->vp.Height;
            } else {
                sd_e[ne].ax = sd_e[ne].ay = 1e30f;
            }
            sd_e[ne].why = (unsigned char)(o->why < IP_W_N ? o->why : 0);
            sd_e[ne].flag = (unsigned char)(lerp ? 1 : 0);
            sd_e[ne].rzb = (unsigned char)(o->rzb != 0);
            sd_e[ne].rt = (unsigned char)r;
            sd_e[ne].ew0 = sew0; sd_e[ne].ew1 = sew1; sd_e[ne].k = k;
            ne++;
        }
    }
    /* Versatz exakt gezeichneter Draws gegenueber einer konsistenten Zwischenlage: "reject" = gleiche
     * Eingangsdaten (Vertex i <-> i ist derselbe Weltpunkt), nur ein Bezugs-Vertex sprang zu weit -> der
     * ganze Draw steht im Zwischenbild in Tick-N-Lage, gelerpte Nachbarn um (1-t)*(pN - pA) daneben. */
    for (i = 0; i < B->nops; i++) {
        const ip_op_t *o = &B->ops[i];
        const ip_op_t *a = (o->match >= 0 && (unsigned)o->match < A->nops) ? &A->ops[o->match] : NULL;
        float dmax = 0.0f, dymax = 0.0f;
        unsigned non = 0, p2;
        if (o->kind != IP_DRAW || !o->cls || o->flag || o->why != IP_W_REJECT || !a || a->nv != o->nv) { continue; }
        p2 = ip_mvp_affine(o->mvp) ? 1u : 0u;
        for (k = 0; k < o->nv; k++) {
            float c0[4], c1[4], x0, y0, x1, y1, dx, dy, d;
            ip_clip(PA[a->vstart + k], a->mvp, c0);
            ip_clip(PB[o->vstart + k], o->mvp, c1);
            if (c0[3] <= 1e-6f || c1[3] <= 1e-6f) { continue; }
            x0 = (c0[0] / c0[3] * 0.5f + 0.5f) * o->vp.Width; y0 = (0.5f - c0[1] / c0[3] * 0.5f) * o->vp.Height;
            x1 = (c1[0] / c1[3] * 0.5f + 0.5f) * o->vp.Width; y1 = (0.5f - c1[1] / c1[3] * 0.5f) * o->vp.Height;
            if (x1 < 0.0f || x1 > o->vp.Width || y1 < 0.0f || y1 > o->vp.Height) { continue; } /* nur sichtbare Ecken */
            dx = (1.0f - T) * (x1 - x0); dy = (1.0f - T) * (y1 - y0);
            d = sqrtf(dx * dx + dy * dy);
            if (d > dmax) { dmax = d; }
            if (fabsf(dy) > dymax) { dymax = fabsf(dy); }
            non++;
        }
        if (!non) { continue; }
        sd_rj_draws[p2]++; sd_rj_verts[p2] += non;
        if (dmax > 0.5f) { sd_rj_bad[p2]++; }
        if (dmax > sd_rj_max[p2]) { sd_rj_max[p2] = dmax; }
        if (dymax > sd_rj_dy[p2]) { sd_rj_dy[p2] = dymax; }
        sd_rj_sum[p2] += dmax;
        if (sd_rjl < (unsigned)g_cfg.seam_diag && dmax > 0.5f && (g_cfg.pf_ring <= 0 || sb_pf_active())) {
            sd_rjl++;
            HLOG_I("SEAMRJ f=%u i=%u n=%u p2=%u sichtbar=%u versatz_max=%.2fpx dy_max=%.2fpx (t=%.2f, exakt statt gelerpt)",
                   B->frame, i, o->nv, p2, non, (double)dmax, (double)dymax, (double)T);
        }
    }
    if (!ne) { return; }
    qsort(sd_e, ne, sizeof *sd_e, sd_cmp);
    for (g0 = 0; g0 < ne;) {
        unsigned g1 = g0 + 1, x, y, multi = 0, allA = 1;
        float best = 0.0f, bdy = 0.0f, bestA = 0.0f;
        unsigned bx = g0, by = g0;
        while (g1 < ne && sd_e[g1].key == sd_e[g0].key) { if (sd_e[g1].op != sd_e[g0].op) { multi = 1; } g1++; }
        if (!multi) { g0 = g1; continue; }
        for (x = g0; x < g1; x++) {
            if (sd_e[x].ax > 1e29f) { allA = 0; }
            for (y = x + 1; y < g1; y++) {
                float dx, dy, s;
                if (sd_e[y].op == sd_e[x].op) { continue; }
                dx = sd_e[y].tx - sd_e[x].tx; dy = sd_e[y].ty - sd_e[x].ty;
                s = sqrtf(dx * dx + dy * dy);
                if (s > best) { best = s; bdy = dy < 0 ? -dy : dy; bx = x; by = y; }
                if (sd_e[x].ax < 1e29f && sd_e[y].ax < 1e29f) {
                    float ex = sd_e[y].ax - sd_e[x].ax, ey = sd_e[y].ay - sd_e[x].ay, sa = sqrtf(ex * ex + ey * ey);
                    if (sa > bestA) { bestA = sa; }
                }
            }
        }
        gtick++;
        if (allA && bestA > 0.5f) { gbadA++; }
        {
            unsigned w0 = sd_e[bx].why, w1 = sd_e[by].why, cl;
            if (w0 > w1) { unsigned tt = w0; w0 = w1; w1 = tt; }
            cl = w0 * IP_W_N + w1;
            if (sd_e[bx].flag != sd_e[by].flag) { cl = IP_W_N * IP_W_N; } /* gemischt exakt/gelerpt */
            sd_cls_n[cl]++;
            if (best > 0.5f) {
                sd_cls_bad[cl]++;
                gbad05++;
                if (cl == IP_W_N * IP_W_N) { /* W429 DIAG: warum ist die exakte Ecke exakt? */
                    const sd_ent_t *ee = sd_e[bx].flag ? &sd_e[by] : &sd_e[bx];
                    const ip_op_t *eo = &B->ops[ee->op];
                    unsigned c = eo->flag ? 0u : eo->exr == 1 ? 1u : eo->exr == 2 ? 2u : eo->why == IP_W_NOMATCH ? 3u :
                                 eo->why == IP_W_RAWCHG ? 4u : eo->why == IP_W_REJECT ? 5u : 6u;
                    sd_mx[ip_mvp_affine(eo->mvp) ? 1 : 0][c]++;
                }
                if (best > 2.0f) { gbad2++; }
                if (best > sd_cls_max[cl]) { sd_cls_max[cl] = best; }
                for (x = 0; x < 3; x++) {
                    if (best > worst[x].s) {
                        memmove(&worst[x + 1], &worst[x], (2u - x) * sizeof worst[0]);
                        worst[x].s = best; worst[x].dy = bdy;
                        worst[x].i0 = sd_e[bx].op; worst[x].i1 = sd_e[by].op;
                        worst[x].w0 = sd_e[bx].why | ((unsigned)sd_e[bx].flag << 8) | ((unsigned)sd_e[bx].rzb << 9);
                        worst[x].w1 = sd_e[by].why | ((unsigned)sd_e[by].flag << 8) | ((unsigned)sd_e[by].rzb << 9);
                        worst[x].x = sd_e[bx].nx; worst[x].y = sd_e[bx].ny;
                        worst[x].e[0] = sd_e[bx].ew0; worst[x].e[1] = sd_e[bx].ew1; worst[x].k0 = sd_e[bx].k;
                        worst[x].e[2] = sd_e[by].ew0; worst[x].e[3] = sd_e[by].ew1; worst[x].k1 = sd_e[by].k;
                        break;
                    }
                }
            }
        }
        if (best > tmax) { tmax = best; }
        g0 = g1;
    }
    sd_ticks++; sd_groups += gtick; sd_bad05 += gbad05; sd_bad2 += gbad2; sd_badA += gbadA;
    if (tmax > sd_maxs) { sd_maxs = tmax; }
    if (sd_lines < (unsigned)g_cfg.seam_diag && (gbad05 || gbadA) && (g_cfg.pf_ring <= 0 || sb_pf_active())) {
        sd_lines++;
        HLOG_I("SEAM f=%u t=%.2f gruppen=%u spalt>0.5px=%u spalt>2px=%u max=%.2fpx vortick_streu>0.5px=%u",
               B->frame, (double)T, gtick, gbad05, gbad2, tmax, gbadA);
        for (k = 0; k < 3 && worst[k].s > 0.0f; k++) {
            const ip_op_t *o0 = &B->ops[worst[k].i0], *o1 = &B->ops[worst[k].i1];
            HLOG_I("SEAMW f=%u #%u s=%.2fpx dy=%.2f bei=%.1f,%.1f A: i=%u n=%u %s flag=%u rzb=%u m=%d | B: i=%u n=%u %s flag=%u rzb=%u m=%d",
                   B->frame, k, (double)worst[k].s, (double)worst[k].dy, (double)worst[k].x, (double)worst[k].y,
                   worst[k].i0, o0->nv, kIP_WHY[worst[k].w0 & 0xFF], (worst[k].w0 >> 8) & 1u, (worst[k].w0 >> 9) & 1u, o0->match,
                   worst[k].i1, o1->nv, kIP_WHY[worst[k].w1 & 0xFF], (worst[k].w1 >> 8) & 1u, (worst[k].w1 >> 9) & 1u, o1->match);
            if (o0->exr == 1 && o0->exj >= 0 && (unsigned)o0->exj < B->nops) { /* W429 DIAG: ausloesender exakter Durchgang */
                const ip_op_t *e = &B->ops[o0->exj];
                HLOG_I("SEAMP f=%u i=%u -> j=%d %s m=%d n=%u p2w=%u gleiche_lage=%d", B->frame, worst[k].i0, o0->exj,
                       kIP_WHY[e->why < IP_W_N ? e->why : 0], e->match, e->nv, e->p2w,
                       memcmp(e->pref, o0->pref, sizeof e->pref) == 0);
            }
            HLOG_I("SEAMV f=%u #%u A: i=%u k=%u p2=%u ex=%u ib=%u/%u w=%.4g->%.4g | B: i=%u k=%u p2=%u ex=%u ib=%u/%u w=%.4g->%.4g", B->frame, k, /* W429 DIAG */
                   worst[k].i0, worst[k].k0, (unsigned)ip_mvp_affine(o0->mvp), o0->exr, o0->ib1,
                   (o0->match >= 0 && (unsigned)o0->match < A->nops) ? A->ops[o0->match].ib1 : 99u, (double)worst[k].e[0], (double)worst[k].e[1],
                   worst[k].i1, worst[k].k1, (unsigned)ip_mvp_affine(o1->mvp), o1->exr, o1->ib1,
                   (o1->match >= 0 && (unsigned)o1->match < A->nops) ? A->ops[o1->match].ib1 : 99u, (double)worst[k].e[2], (double)worst[k].e[3]);
        }
    }
    if (sd_ticks >= 200u) {
        char buf[900];
        int p = 0;
        unsigned c2;
        buf[0] = 0;
        for (c2 = 0; c2 <= IP_W_N * IP_W_N && p < (int)sizeof buf - 64; c2++) {
            if (!sd_cls_n[c2]) { continue; }
            if (c2 == IP_W_N * IP_W_N) {
                p += _snprintf_s(buf + p, sizeof buf - (size_t)p, _TRUNCATE, " exakt/gelerpt=%u/%u(max %.1f)", sd_cls_bad[c2], sd_cls_n[c2], (double)sd_cls_max[c2]);
            } else {
                p += _snprintf_s(buf + p, sizeof buf - (size_t)p, _TRUNCATE, " %s/%s=%u/%u(max %.1f)", kIP_WHY[c2 / IP_W_N], kIP_WHY[c2 % IP_W_N],
                                 sd_cls_bad[c2], sd_cls_n[c2], (double)sd_cls_max[c2]);
            }
        }
        HLOG_I("SEAMSUM ticks=%u gruppen=%u spalt>0.5px=%u spalt>2px=%u max=%.1fpx vortick_streu>0.5px=%u klassen(spalt/gruppen):%s",
               sd_ticks, sd_groups, sd_bad05, sd_bad2, sd_maxs, sd_badA, buf);
        HLOG_I("SEAMRJSUM ticks=%u p2: draws=%u ecken=%u versatz>0.5px=%u max=%.1fpx mittel=%.2fpx dy_max=%.1fpx | 3d: draws=%u ecken=%u versatz>0.5px=%u max=%.1fpx mittel=%.2fpx dy_max=%.1fpx",
               sd_ticks, sd_rj_draws[1], sd_rj_verts[1], sd_rj_bad[1], (double)sd_rj_max[1],
               sd_rj_draws[1] ? sd_rj_sum[1] / sd_rj_draws[1] : 0.0, (double)sd_rj_dy[1],
               sd_rj_draws[0], sd_rj_verts[0], sd_rj_bad[0], (double)sd_rj_max[0],
               sd_rj_draws[0] ? sd_rj_sum[0] / sd_rj_draws[0] : 0.0, (double)sd_rj_dy[0]);
        HLOG_I("SEAMX ticks=%u exakte_seite 3d: vertex=%u pass=%u ablage=%u nomatch=%u rawchg=%u reject=%u sonst=%u | p2: vertex=%u pass=%u ablage=%u nomatch=%u rawchg=%u reject=%u sonst=%u",
               sd_ticks, sd_mx[0][0], sd_mx[0][1], sd_mx[0][2], sd_mx[0][3], sd_mx[0][4], sd_mx[0][5], sd_mx[0][6],
               sd_mx[1][0], sd_mx[1][1], sd_mx[1][2], sd_mx[1][3], sd_mx[1][4], sd_mx[1][5], sd_mx[1][6]); /* W429 DIAG */
        memset(sd_mx, 0, sizeof sd_mx);
        memset(sd_rj_draws, 0, sizeof sd_rj_draws); memset(sd_rj_verts, 0, sizeof sd_rj_verts); memset(sd_rj_bad, 0, sizeof sd_rj_bad);
        memset(sd_rj_max, 0, sizeof sd_rj_max); memset(sd_rj_dy, 0, sizeof sd_rj_dy); memset(sd_rj_sum, 0, sizeof sd_rj_sum);
        sd_ticks = sd_groups = sd_bad05 = sd_bad2 = sd_badA = 0;
        sd_maxs = 0.0;
        memset(sd_cls_n, 0, sizeof sd_cls_n); memset(sd_cls_bad, 0, sizeof sd_cls_bad); memset(sd_cls_max, 0, sizeof sd_cls_max);
    }
}
static gd_slot_t gd_s[GD_N];
static unsigned gd_cnt, gd_lost;
static int gd_fail;
static FILE *gd_f;
static double gd_ms[8], gd_dr[8], gd_vb[8], gd_mx[8];
static unsigned gd_n[8];
static LONGLONG gd_t0;
static const char *const gd_vn[8] = { "normal", "vp1x1", "v3", "nocb", "nodraw", "ring", "gpuvb", "ring+gpuvb" };

static int gd_vi(unsigned var)
{
    int k = 0;
    while (var && k < 7) { var >>= 1; k++; }
    return k;
}

static void gd_log(int force)
{
    LONGLONG now = ip_now();
    char b[1400];
    int o = 0, k;
    if (!gd_t0) { gd_t0 = now; return; }
    if (!force && ip_ms(gd_t0, now) < 10000.0) { return; }
    for (k = 0; k < 8 && o >= 0; k++) {
        if (!gd_n[k]) { continue; }
        o += _snprintf_s(b + o, sizeof b - (size_t)o, _TRUNCATE, " | %s n=%u gpu=%.3fms(max %.2f) draws=%.0f vb=%.1fMB",
                         gd_vn[k], gd_n[k], gd_ms[k] / gd_n[k], gd_mx[k], gd_dr[k] / gd_n[k], gd_vb[k] / gd_n[k]);
    }
    if (o > 0) {
        HLOG_I("IPGPU %.1fs:%s | verloren=%u vb_kopien=%u (%.1f MB/Kopie, fehl %u, GPU %.3fms(max %.2f) je Kopie, n=%u) neuanlagen=%u",
               ip_ms(gd_t0, now) / 1000.0, b, gd_lost, st_vbg_copy, st_vbg_copy ? st_vbg_mb / st_vbg_copy : 0.0, st_vbg_fail,
               gc_n ? gc_ms / gc_n : 0.0, gc_mx, gc_n, st_vbg_async);
    }
    gc_ms = gc_mx = 0.0; gc_n = 0;
    memset(gd_ms, 0, sizeof gd_ms); memset(gd_dr, 0, sizeof gd_dr); memset(gd_vb, 0, sizeof gd_vb);
    memset(gd_mx, 0, sizeof gd_mx); memset(gd_n, 0, sizeof gd_n);
    gd_lost = 0; st_vbg_copy = 0; st_vbg_mb = 0.0; st_vbg_fail = 0;
    gd_t0 = now;
}

static void gd_poll(void)
{
    int k;
    for (k = 0; k < GD_N; k++) {
        gd_slot_t *g = &gd_s[k];
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        UINT64 t0, t1;
        double ms;
        int vi;
        if (g->st != 2) { continue; }
        if (ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)g->dj, &dj, sizeof dj,
                                        D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) { continue; }
        g->st = 0;
        if (dj.Disjoint || !dj.Frequency ||
            ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)g->t0, &t0, sizeof t0, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)g->t1, &t1, sizeof t1, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            t1 < t0) {
            gd_lost++;
            continue;
        }
        ms = (double)(t1 - t0) * 1000.0 / (double)dj.Frequency;
        vi = gd_vi(g->rc.var);
        gd_ms[vi] += ms; gd_n[vi]++;
        gd_dr[vi] += g->rc.ndraw;
        gd_vb[vi] += (double)g->rc.vbytes / 1048576.0;
        if (ms > gd_mx[vi]) { gd_mx[vi] = ms; }
        if (!gd_f) {
            char dir[600], path[700];
            DWORD n = GetEnvironmentVariableA("SB_OUT_DIR", dir, sizeof dir);
            if (!n || n >= sizeof dir) { strcpy_s(dir, sizeof dir, "."); }
            _snprintf_s(path, sizeof path, _TRUNCATE, "%s/ipgpu.csv", dir);
            gd_f = _fsopen(path, "w", _SH_DENYNO); /* mitlesbar waehrend des Laufs */
            if (!gd_f) { gd_fail = 1; HLOG_W("IPGPU: %s nicht schreibbar", path); }
            else {
                fprintf(gd_f, "ser,t,var,ms,nops,ndraw,nlerp,nrq,nclr,nverts,vb_mb,rqx_kb,nmap,ncb,slots,nrtv,nsrv,ring,vbg,qpc_s\n");
                HLOG_I("IPGPU: Messung je Wiedergabe nach %s (ip_gdiag=%d, ip_ablate=%d, ip_cb_ring=%d, ip_vb_gpu=%d)", path,
                       g_cfg.ip_gdiag, g_cfg.ip_ablate, g_cfg.ip_cb_ring, g_cfg.ip_vb_gpu);
            }
        }
        if (gd_f) {
            fprintf(gd_f, "%u,%.3f,%u,%.4f,%u,%u,%u,%u,%u,%u,%.2f,%u,%u,%u,%u,%u,%u,%u,%u,%.4f\n", g->ser, g->t, g->rc.var, ms,
                    g->rc.nops, g->rc.ndraw, g->rc.nlerp, g->rc.nrq, g->rc.nclr, g->rc.nverts,
                    (double)g->rc.vbytes / 1048576.0, g->rc.rqxb / 1024u, g->rc.nmap, g->rc.ncb, g->rc.slots,
                    g->rc.nrtv, g->rc.nsrv, g->rc.ring, g->rc.vbg, (double)ip_now() / (double)ip_qpf.QuadPart);
        }
    }
    gd_log(0);
}

int gd_begin(void)
{
    int k;
    D3D11_QUERY_DESC qd;
    if (g_cfg.ip_gdiag <= 0 || gd_fail) { return -1; }
    gd_poll();
    if ((++gd_cnt % (unsigned)g_cfg.ip_gdiag) != 0u) { return -1; }
    for (k = 0; k < GD_N; k++) { if (gd_s[k].st == 0) { break; } }
    if (k >= GD_N) { gd_lost++; return -1; }
    if (!gd_s[k].dj) {
        memset(&qd, 0, sizeof qd);
        qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        if (FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &gd_s[k].dj))) { gd_fail = 1; return -1; }
        qd.Query = D3D11_QUERY_TIMESTAMP;
        if (FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &gd_s[k].t0)) ||
            FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &gd_s[k].t1))) { gd_fail = 1; return -1; }
    }
    ID3D11DeviceContext_Begin(g_ctx, (ID3D11Asynchronous *)gd_s[k].dj);
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)gd_s[k].t0);
    return k;
}

void gd_end(int k, const ip_rc_t *rc, unsigned ser, float t)
{
    gd_slot_t *g = &gd_s[k];
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)g->t1);
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)g->dj);
    g->rc = *rc;
    g->ser = ser;
    g->t = t;
    g->st = 2;
}

/* DIAG(W251 INTERP-GROUND): Draw-ID-Bild des Bildes (A, B, t) - Ziele mit den meisten
 * RT-Draws (Welt) und BB als BMP (ipid_<f>_<t>_e<k>), danach das gezeigte Bild wiederherstellen. */
static void ip_tex_ppm(const char *name, ID3D11Texture2D *tex) /* DIAG(W251): Schatten-Ziel roh als PPM (Bytes 0..2 je Pixel) */
{
    D3D11_TEXTURE2D_DESC dd;
    ID3D11Texture2D *stg = NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    char path[600];
    FILE *f;
    unsigned y, x;
    ID3D11Texture2D_GetDesc(tex, &dd);
    dd.MipLevels = 1; dd.ArraySize = 1; dd.SampleDesc.Count = 1; dd.SampleDesc.Quality = 0;
    dd.Usage = D3D11_USAGE_STAGING; dd.BindFlags = 0; dd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; dd.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &dd, NULL, &stg))) { return; }
    ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)stg, 0, 0, 0, 0, (ID3D11Resource *)tex, 0, NULL);
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)stg, 0, D3D11_MAP_READ, 0, &m))) {
        sprintf_s(path, sizeof path, "%s/shots/%s_f%u.ppm", hcfg_out_dir(), name, (unsigned)dd.Format);
        if (fopen_s(&f, path, "wb") == 0 && f) {
            fprintf(f, "P6\n%u %u\n255\n", dd.Width, dd.Height);
            for (y = 0; y < dd.Height; y++) {
                const unsigned char *r = (const unsigned char *)m.pData + (size_t)y * m.RowPitch;
                for (x = 0; x < dd.Width; x++) { fwrite(r + 4u * x, 1, 3, f); }
            }
            fclose(f);
        }
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)stg, 0);
    }
    ID3D11Texture2D_Release(stg);
}

void ip_id_dump(ip_list_t *A, int ai, ip_list_t *B, int bi, float t)
{
    unsigned cnt[IP_SH_MAX], i;
    int e, top[2] = { -1, -1 }, jpg = g_cfg.screenshot_jpg, k;
    char nm[64];
    memset(cnt, 0, sizeof cnt);
    for (i = 0; i < B->nops; i++) {
        const ip_op_t *o = &B->ops[i];
        if (o->kind == IP_DRAW && o->rtv != g_rtv && (e = ip_sh_of_rtv(o->sh_rtv)) >= 0) { cnt[e]++; }
    }
    for (e = 0; e < IP_SH_MAX; e++) {
        if (!cnt[e]) { continue; }
        if (top[0] < 0 || cnt[e] > cnt[top[0]]) { top[1] = top[0]; top[0] = e; }
        else if (top[1] < 0 || cnt[e] > cnt[top[1]]) { top[1] = e; }
    }
    ip_replay_ex(B, bi, A, ai, t, 2, ip_cx_imm());
    g_cfg.screenshot_jpg = 0;
    for (k = 0; k < 2; k++) {
        if (top[k] < 0 || !g_ish[top[k]].sh) { continue; }
        _snprintf_s(nm, sizeof nm, _TRUNCATE, "ipid_%06u_%03u_e%d", B->frame, (unsigned)(t * 1000.0f + 0.5f), top[k]);
        ip_tex_ppm(nm, g_ish[top[k]].sh);
    }
    _snprintf_s(nm, sizeof nm, _TRUNCATE, "ipid_%06u_%03u_bb", B->frame, (unsigned)(t * 1000.0f + 0.5f));
    sb_render_shot_name(nm, (ID3D11Resource *)g_ish[B->bbref].sh);
    g_cfg.screenshot_jpg = jpg;
    HLOG_I("INTERP ddid f=%u t=%.3f e=%d(%u) e2=%d(%u) bb=%d", B->frame, (double)t, top[0], top[0] >= 0 ? cnt[top[0]] : 0u,
           top[1], top[1] >= 0 ? cnt[top[1]] : 0u, B->bbref);
    if (t < 1.0f && A) { ip_replay(B, bi, A, ai, t); } /* gezeigtes Bild wiederherstellen */
    else { ip_replay(B, bi, A, ai, 1.0f); }            /* Tick-Bild (nur Schatten, gezeigt wird bak) */
    for (k = 0; k < 2; k++) {
        if (top[k] < 0 || !g_ish[top[k]].sh) { continue; }
        _snprintf_s(nm, sizeof nm, _TRUNCATE, "iprt_%06u_%03u_e%d", B->frame, (unsigned)(t * 1000.0f + 0.5f), top[k]);
        ip_tex_ppm(nm, g_ish[top[k]].sh);
    }
}

/* Bild zwischen A und B zeigen: t >= 1 (oder ohne A) -> gesichertes Spiel-Bild
 * von B; sonst Wiedergabe in die Schatten. tag = Name fuer Bildfolgen. */
/* W323 LOC-INTERP: Zaehler der gezeigten Bilder im LoC-Profil (Zeile LOCINTERP alle 10 s, pd_stat_log). */
unsigned li_mid, li_exact, li_same, li_ticks, li_dev2, li_kept;
double li_dev_max; /* Abweichung des Zustandsabstands vom Spieltakt (ms): Anker-Guete */
unsigned li_last_ser;
float li_last_t = -1.0f;
void li_note(const ip_list_t *A, const ip_list_t *B, float t)
{
    float tt = (t >= 1.0f || !A) ? 1.0f : t;
    if (tt >= 1.0f) { li_exact++; } else { li_mid++; }
    if (B->serial == li_last_ser && tt == li_last_t) { li_same++; } /* Wiederhol-Present: dasselbe Bild erneut */
    li_last_ser = B->serial;
    li_last_t = tt;
}

/* DIAG(W349 LOC-FRAMES-2) loc_interp_dbg Bit 4: nach Zwischenbild-Wiedergaben in den Bildfolgen (pf_ring) die kleinen
 * Render-Ziel-Schatten (<= 256 px, Glow-Kette) roh als PPM ablegen (12 Bilder): folgen sie t oder bleiben sie stehen? */
void li_sh_dump(const ip_list_t *B, float t)
{
    static unsigned n;
    int j;
    char nm[80];
    int big = (g_cfg.loc_interp_dbg & 8) != 0; /* Bit 8: nur Ziele 257..512 px (Welt-RT), 24 Bilder */
    if (n >= (big ? 24u : 12u)) { return; }
    n++;
    for (j = 0; j < B->nrefs; j++) {
        int e = B->refs[j];
        D3D11_TEXTURE2D_DESC dd;
        if (!g_ish[e].sh) { continue; }
        ID3D11Texture2D_GetDesc(g_ish[e].sh, &dd);
        if ((big ? (dd.Width <= 256u || dd.Width > 512u) : dd.Width > 256u) || !(dd.BindFlags & D3D11_BIND_RENDER_TARGET) ||
            dd.SampleDesc.Count != 1) { continue; }
        _snprintf_s(nm, sizeof nm, _TRUNCATE, "lish_%06u_%03u_e%02d_%ux%u", B->frame, (unsigned)(t * 1000.0f + 0.5f), e, dd.Width, dd.Height);
        ip_tex_ppm(nm, g_ish[e].sh);
    }
}

void ip_stat_log(void)
{
    if (g_cfg.interp_mode == 1) { /* W295 WORLD-REPROJ */
        if (g_cfg.reproj_diag >= 1) { HLOG_I("REPROJ ohne: welt=%u tiefe=%u durchgang=%u ressourcen=%u", st_rp_skip[0], st_rp_skip[1], st_rp_skip[2], st_rp_skip[3]); }
        memset(st_rp_skip, 0, sizeof st_rp_skip);
        sb_rp_stat_log();
    }
    LONGLONG now = ip_now();
    double secs = ip_ms(st_t0, now) / 1000.0;
    if (secs <= 0.0) { secs = 1e-3; }
    HLOG_I("INTERP stat: ticks=%u pairs=%u presents=%u rate=%.1f/s ticks/s=%.1f K=%d lerp=%u rawlerp=%u rt2d=%u static=%u nomatch=%u rawchg=%u reject=%u 2d=%u cut=%u exact=%u hold=%u skip=%u late=%u nopair=%u ovf=%u rec=%.2fms cap=%.2fms replay=%.2fms(max %.2f) present=%.2fms gpu=%.2fms(max %.2f, n=%u) r=%.1fms(max %.1f) L=%.1fms verify=%u/%u",
           st_ticks, st_pairs, st_presents, (double)st_presents / secs,
           (double)st_ticks / secs, ip_K, st_lerp, st_rawlerp, st_rt2d, st_static, st_nomatch, st_rawchg, st_reject,
           st_2d, st_cut, st_exact, st_hold, st_skip, st_late, st_nopair, st_ovf,
           st_ticks ? st_rec_ms / st_ticks : 0.0,
           st_ticks ? (double)st_cap_q * 1000.0 / (double)ip_qpf.QuadPart / st_ticks : 0.0,
           st_rep_n ? st_rep_ms / st_rep_n : 0.0, st_rep_max,
           st_presents ? st_pres_ms / st_presents : 0.0,
           st_gpu_n ? st_gpu_ms / st_gpu_n : 0.0, st_gpu_max, st_gpu_n,
           st_r_n ? st_r_ms / st_r_n : 0.0, st_r_max, st_lat_n ? st_lat_ms / st_lat_n : 0.0,
           st_verify - st_verify_bad, st_verify);
    HLOG_I("INTERP stat2: geo=%u (Geometrie-Partner, in lerp enthalten)", st_geo);
    if (!g_sba_loc) { /* W347 GHOST: RT-Selbstkopien (gesehen / als Op aufgezeichnet / mit Schatten / wiedergegeben) */
        HLOG_I("INTERP selfcopy: gesehen=%ld ops=%ld schatten=%ld wiedergabe=%ld (interp_selfcopy=%d)",
               li_cp_seen, li_cp_rec, li_cp_ok, li_cp_rep, g_cfg.interp_selfcopy);
        li_cp_seen = li_cp_rec = li_cp_ok = li_cp_rep = 0;
        HLOG_I("INTERP bbcam: gepaart=%u ohne_partner=%u ungeeignet=%u raster=%u zeit=%.2fms (interp_bbcam=%d, Billboards per Kamera-Schwerpunkt)",
               st_bb_ok, st_bb_far, st_bb_no, st_bb_build, st_bb_ms, g_cfg.interp_bbcam);
        st_bb_ok = st_bb_far = st_bb_no = st_bb_build = 0;
        st_bb_ms = 0.0;
    }
    if (sb_ip_hlerp()) { /* FIX(W237) */
        HLOG_I("INTERP hlerp: ticks=%u fehl=%u draws=%u ersatz=%u (homogener Zwischenbild-Lerp, w-Puffer je Tick)",
               st_hw_up, st_hw_fail, st_hw_draws, st_hw_one);
    }
    if (g_cfg.interp_guard_diag) { /* DIAG(W265 LINE-GUARD) */
        ip_gd_poll();
        HLOG_I("INTERP guard: modus=%d maske=%d bilder=%u dreiecke=%llu treffer=%llu (%.4f%%) bilder_mit_treffer=%u max_je_bild=%u ohne_abfrage=%u",
               g_cfg.interp_guard, g_cfg.interp_guard_diag, st_gd_frames, st_gd_tris, st_gd_hit,
               st_gd_tris ? 100.0 * (double)st_gd_hit / (double)st_gd_tris : 0.0, st_gd_fhit, st_gd_max, st_gd_lost);
    }
    if (g_cfg.interp_zcons) { /* FIX(W251 INTERP-GROUND) */
        HLOG_I("INTERP zcons: modus=%d relerp=%u ohne_partner=%u kamera_abw=%u ziele_ok=%u ticks_exakt=%u draws_exakt=%u (Tiefen-Konsistenz)",
               g_cfg.interp_zcons, st_zc_rel, st_zc_nocand, st_zc_cam, st_zc_tgt, st_zc_ticks, st_zc_draws);
    }
    if (g_cfg.interp_rawzc) { /* FIX(W284 INTERP-RAWCHG) */
        HLOG_I("INTERP rawzc: relerp_welt=%u relerp_versatz=%u ohne_versatz=%u kamera_abw=%u ohne_kamera=%u sonst=%u zeit=%.3fms(max %.3f, n=%u) (verschobene Eingangsdaten)",
               st_rz_relw, st_rz_rel, st_rz_noofs, st_rz_cam, st_rz_nocam, st_rz_other,
               st_rz_n ? st_rz_ms / st_rz_n : 0.0, st_rz_max, st_rz_n);
        st_rz_ms = st_rz_max = 0.0; st_rz_n = 0;
        if (g_cfg.interp_rawzc >= 2) {
            HLOG_I("INTERP rawzc nomatch: relerp_welt=%u ohne_kandidat=%u kamera_abw=%u", st_rz_nm, st_rz_nmno, st_rz_nmcam);
            st_rz_nm = st_rz_nmno = st_rz_nmcam = 0;
        }
        st_rz_relw = st_rz_rel = st_rz_noofs = st_rz_cam = st_rz_nocam = st_rz_other = 0;
    }
    if (g_cfg.interp_pair || g_cfg.interp_pair_diag) { /* W317 INTERP-PAIR */
        HLOG_I("INTERP pair2: modus=%d ticks=%u menge=%u partner=%u vertex=%u ruhend=%u alt_frei=%u kamera=%u folge=%u geaendert=%u zeit=%.3fms(max %.3f; kamera+A %.3f, kandidaten %.3f, zuordnung %.3f)",
               g_cfg.interp_pair, st_pr_ticks, st_pr_set, st_pr_pair, st_pr_raw, st_pr_still, st_pr_keep, st_pr_cam, st_pr_follow, st_pr_chg,
               st_pr_ticks ? st_pr_ms / st_pr_ticks : 0.0, st_pr_max, st_pr_ticks ? st_pr_ms1 / st_pr_ticks : 0.0,
               st_pr_ticks ? st_pr_ms2 / st_pr_ticks : 0.0, st_pr_ticks ? st_pr_ms3 / st_pr_ticks : 0.0);
        if (g_cfg.interp_pair_diag) {
            int c;
            for (c = 0; c < PR_NC; c++) {
                const unsigned *z = st_pz[c];
                HLOG_I("INTERP pairz: klasse=%s ok=%u falsch=%u verpasst=%u neu=%u bewegt=%u exakt=%u statisch=%u",
                       kPR_CLS[c], z[0], z[1], z[2], z[3], z[4], z[5], z[6]);
            }
            memset(st_pz, 0, sizeof st_pz);
        }
        st_pr_ticks = st_pr_set = st_pr_pair = st_pr_raw = st_pr_still = st_pr_keep = st_pr_cam = st_pr_follow = st_pr_chg = 0;
        st_pr_ms = st_pr_max = st_pr_ms1 = st_pr_ms2 = st_pr_ms3 = 0.0;
    }
    if (g_cfg.interp_streak) { /* FIX(W294 LINE-STREAK) */
        HLOG_I("INTERP streak: modus=%d rawlerp_versatz=%u davon_welt=%u kamera_abw=%u rawchg_ohne_tiefe=%u",
               g_cfg.interp_streak, st_sk_rlofs, st_sk_rl, st_sk_rlcam, st_sk_nod);
        st_sk_rlofs = st_sk_rl = st_sk_rlcam = st_sk_nod = 0;
    }
    if (g_cfg.interp_p2hom && !g_sba_loc) { /* FIX(W429) */
        HLOG_I("INTERP p2hom: hlerp=%d reject_gelerpt_3d=%u reject_gelerpt_p2=%u (gleiche Eingangsdaten, Bezugs-Vertices im Bild springen wenig)"
               " exakt_daten_neu=%u exakt_ohne_w=%u exakt_kein_bezug=%u exakt_sprung=%u",
               sb_ip_hlerp(), st_p2hom[0], st_p2hom[1], st_rr[0], st_rr[1], st_rr[2], st_rr[3]);
        HLOG_I("INTERP p2cam: reject_3d=%u reject_p2=%u nomatch_3d=%u nomatch_p2=%u kamera_abw=%u fehler_ok=%u/%u/%u/%u"
               " fehler_abw=%u/%u/%u/%u/%u ohne_kamera=%u (Welt-Ruhe-Pruefung, Grenze 0,05 bzw. nomatch 0,02)",
               st_p2c[0], st_p2c[1], st_p2c[2], st_p2c[3], st_p2c[4], st_p2ok[0], st_p2ok[1], st_p2ok[2], st_p2ok[3],
               st_p2e[0], st_p2e[1], st_p2e[2], st_p2e[3], st_p2e[4], st_p2e[5]);
        st_p2hom[0] = st_p2hom[1] = 0;
        memset(st_p2c, 0, sizeof st_p2c); memset(st_p2e, 0, sizeof st_p2e); memset(st_p2ok, 0, sizeof st_p2ok);
        memset(st_rr, 0, sizeof st_rr);
    }
    if (g_cfg.interp_p2rej) { /* OPTION(W406) */
        HLOG_I("INTERP p2rej: gelerpt=%u (P2-Draws mit gleichen Eingangsdaten, sonst reject)", st_p2rej);
        st_p2rej = 0;
    }
    if (g_cfg.interp_eyediag > 0) { /* DIAG(W224) */
        HLOG_I("INTERP eyesum: draws=%u verts=%u (w-Wechsel in zwischengerechneten Draws, eyefix=%d)",
               st_eyex_draws, st_eyex_verts, g_cfg.interp_eyefix);
        st_eyex_draws = st_eyex_verts = 0;
        HLOG_I("INTERP p2sum: draws=%u verts=%u erkannt=%u sprung=%u ohne_wwechsel=%u welt: static=%u lerp=%u rawlerp=%u nomatch=%u rawchg=%u reject=%u",
               st_p2x_draws, st_p2x_verts, st_p2x_hit, st_jmp_verts, st_jmp_fp, st_wwhy[IP_W_STATIC], st_wwhy[IP_W_LERP],
               st_wwhy[IP_W_RAWLERP], st_wwhy[IP_W_NOMATCH], st_wwhy[IP_W_RAWCHG], st_wwhy[IP_W_REJECT]); /* DIAG(W237) */
        HLOG_I("INTERP jumpsum: vorn lerp=%u rawlerp=%u rt2d=%u (Draws %u/%u/%u) hinten lerp=%u rawlerp=%u rt2d=%u",
               st_jf[IP_W_LERP], st_jf[IP_W_RAWLERP], st_jf[IP_W_RT2DLERP], st_jfd[IP_W_LERP], st_jfd[IP_W_RAWLERP],
               st_jfd[IP_W_RT2DLERP], st_jb[IP_W_LERP], st_jb[IP_W_RAWLERP], st_jb[IP_W_RT2DLERP]); /* DIAG(W237) */
        memset(st_jf, 0, sizeof st_jf); memset(st_jb, 0, sizeof st_jb); memset(st_jfd, 0, sizeof st_jfd);
        st_p2x_draws = st_p2x_verts = st_p2x_hit = st_jmp_verts = st_jmp_fp = 0;
        memset(st_wwhy, 0, sizeof st_wwhy);
    }
    HLOG_I("INTERP inst: multi=%u wrong=%u amb=%u pass=%u (interp_inst=%d interp_pass=%d)", st_imulti,
           st_iwrong, st_iamb, st_ipass, g_cfg.interp_inst, g_cfg.interp_pass); /* POPIN (W91) */
    if (g_cfg.interp_pass && g_cfg.interp_pass_exact > 0) { /* W273 */
        HLOG_I("INTERP pass_exact: Fehltreffer der 16-Bit-Maske verworfen=%u", st_ipass_false);
        st_ipass_false = 0;
    }
    if (ip_rt_ok) { /* W126 RENDER-THREAD */
        HLOG_I("INTERP rt: auftraege=%u vorbereitet=%u nachgefordert=%u gewartet=%u verworfen=%u fehl=%u aufzeichnung=%.2fms(max %.2f, Render-Thread) spiel: holen=%.2fms ausfuehren=%.2fms je Bild",
               st_rt_n, st_rt_hit, st_rt_miss, st_rt_wait, st_rt_stale, st_rt_fail,
               st_rt_n ? st_rt_ms / st_rt_n : 0.0, st_rt_max,
               (st_rt_hit + st_rt_miss) ? st_rt_wait_ms / (st_rt_hit + st_rt_miss) : 0.0,
               st_rt_exec ? st_rt_exec_ms / st_rt_exec : 0.0);
        st_rt_n = st_rt_hit = st_rt_miss = st_rt_wait = st_rt_stale = st_rt_fail = st_rt_exec = 0;
        st_rt_ms = st_rt_max = st_rt_wait_ms = st_rt_exec_ms = 0.0;
    }
    st_geo = 0;
    st_imulti = st_iwrong = st_iamb = st_ipass = 0;
    st_t0 = now;
    st_presents = st_ticks = st_pairs = 0;
    st_lerp = st_static = st_nomatch = st_rawchg = st_reject = st_2d = st_rawlerp = st_rt2d = 0;
    st_cut = st_exact = st_hold = st_skip = st_late = st_nopair = st_ovf = 0;
    st_rec_ms = st_rep_ms = st_rep_max = st_pres_ms = 0.0;
    st_cap_q = 0;
    st_rep_n = 0;
    st_gpu_ms = st_gpu_max = 0.0;
    st_gpu_n = 0;
    st_r_ms = st_r_max = st_lat_ms = 0.0;
    st_r_n = st_lat_n = 0;
}

void ip_hist_clear(void)
{
    int i;
    ip_rt_quiesce(); /* W126 RENDER-THREAD */
    for (i = 0; i < ip_nhist; i++) { ip_list_reset(&g_ipl[ip_hist[i]]); }
    ip_nhist = 0;
    ip_live = 0;
    ip_nneed = 0;
}
