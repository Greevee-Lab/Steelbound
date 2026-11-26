/* render_interp_replay.c - FPS-INTERP Wiedergabe: Ruhe-Cache (cc_*), VR-Augen, Draw-Ausfuehrung, Foto-Abweichungen, LoC-Sicht-Vorbereitung, ip_replay_ex.
 * W463 RCORE: aus render_interp.c ausgegliedert (reines Verschieben); gemeinsamer Zustand in
 * render_interp_int.h, Modulkarte docs/RENDER_ARCH.md. */
#include "render_interp_int.h"

/* Tick B in die Schatten wiedergeben (Anfangsstand aus A bzw. B);
 * 0<t<1 und gueltiges Paar -> Welt-Draws mit flag=1 lerpen. */
static const float kIP_DBG_COL[IP_W_N][3] = {
    { 1, 1, 1 }, { 0.5f, 0.5f, 0.5f }, { 0, 1, 0 }, { 0, 1, 1 }, { 1, 0, 0 },
    { 1, 1, 0 }, { 1, 0, 1 }, { 1, 1, 1 }, { 0, 0, 1 }, { 1, 0.5f, 0 } };
static ID3D11Texture2D *cc_col, *cc_dep;
static unsigned cc_ser_a, cc_ser_b, cc_scan_ser;
static int cc_valid, cc_pfx = -1, cc_dep_e = -1, cc_why, cc_bbe = -1;
static unsigned cc_npfx, cc_nbb, cc_nskip, cc_nquiet;
static ID3D11DepthStencilView *cc_bdsv;
static unsigned char cc_skip[CC_MAXOPS];   /* 1 = Op schreibt nur ruhige Ziele */
static signed char cc_ent[CC_MAXOPS][3];   /* Eintrag rtv/dsv/srv (-1) */
static unsigned char cc_dirty[IP_SH_MAX], cc_wr[IP_SH_MAX];
static unsigned char cc_nosc[IP_SH_MAX];   /* 1 = keine Startkopie noetig (erste Beruehrung = Clear bzw. ruhig) */
static unsigned cc_nnosc, cc_st_nosc;
static unsigned cc_st_built, cc_st_used, cc_st_off, cc_st_why[8], cc_st_pfx, cc_st_bb, cc_st_n, cc_st_skip, cc_st_q,
    cc_st_ops, cc_st_rep;

static int cc_ent_of(const ip_list_t *B, const void *v, int q)
{
    int j;
    if (!v) { return -1; }
    for (j = 0; j < B->nrefs; j++) {
        const ip_sh_t *e = &g_ish[B->refs[j]];
        if ((q == 0 && (const void *)e->rtv == v) || (q == 1 && (const void *)e->dsv == v) ||
            (q == 2 && (const void *)e->srv == v)) { return B->refs[j]; }
    }
    return -1;
}

static int cc_op_exact(const ip_op_t *o)
{
    if (o->kind != IP_DRAW) { return 1; }
    return !o->flag && !(o->has_tex & FXR_MASK);
}

/* liest die Op ein unruhiges Ziel (Schatten-SRV, auch Stufen 1..3)? */
static int cc_reads_dirty(const ip_list_t *B, unsigned i)
{
    const ip_op_t *o = &B->ops[i];
    int k, e;
    if (o->kind != IP_DRAW) { return 0; }
    if (cc_ent[i][2] >= 0 && cc_dirty[cc_ent[i][2]]) { return 1; }
    if (o->rq) {
        for (k = 1; k < 4; k++) {
            if (o->rq_shsrv[k] && o->rq_shsrv[k] != o->rqr.srv[k]) {
                e = cc_ent_of(B, o->rq_shsrv[k], 2);
                if (e < 0 || cc_dirty[e]) { return 1; }
            }
        }
    }
    return 0;
}

static void cc_scan(ip_list_t *B, int bi)
{
    unsigned i, it, k;
    unsigned lastw[IP_SH_MAX];
    int changed;
    cc_scan_ser = B->serial;
    cc_pfx = 0; cc_why = 0; cc_npfx = cc_nbb = cc_nskip = cc_nquiet = 0;
    cc_bdsv = NULL; cc_dep_e = -1; cc_bbe = B->bbref;
    memset(cc_dirty, 0, sizeof cc_dirty);
    memset(cc_wr, 0, sizeof cc_wr);
    memset(lastw, 0, sizeof lastw);
    if (B->nops > CC_MAXOPS) { cc_why = 6; cc_pfx = 0; memset(cc_skip, 0, sizeof cc_skip); return; }
    for (i = 0; i < B->nops; i++) {
        const ip_op_t *o = &B->ops[i];
        cc_ent[i][0] = (signed char)cc_ent_of(B, o->sh_rtv, 0);
        cc_ent[i][1] = (signed char)cc_ent_of(B, o->sh_dsv, 1);
        cc_ent[i][2] = (signed char)((o->kind == IP_DRAW && o->sh_srv != o->srv) ? cc_ent_of(B, o->sh_srv, 2) : -1);
        if (o->kind == IP_DRAW && o->sh_srv != o->srv && cc_ent[i][2] < 0) { cc_ent[i][2] = -2; } /* unbekannt */
        for (k = 0; k < 2; k++) {
            int e = cc_ent[i][k];
            if (e >= 0) { cc_wr[e] = 1; lastw[e] = i + 1u; if (!cc_op_exact(o)) { cc_dirty[e] = 1; } }
        }
        if (!cc_bdsv && o->rtv == g_rtv && o->dsv) { cc_bdsv = o->dsv; cc_dep_e = cc_ent[i][1]; }
    }
    /* gelesene Ziele: Lesen vor dem letzten Schreiben -> Endstand passt nicht */
    for (i = 0; i < B->nops; i++) {
        int r = cc_ent[i][2];
        if (r >= 0 && cc_wr[r] && i < lastw[r]) { cc_dirty[r] = 1; }
    }
    if (cc_bbe >= 0) { cc_dirty[cc_bbe] = 1; } /* BB nie ganz ueberspringen (Anfangs-Cache b) */
    if (cc_dep_e >= 0) { cc_dirty[cc_dep_e] = 1; }
    for (it = 0; it < 16u; it++) { /* Ausbreitung: liest unruhig / gemeinsames Ziel+Tiefe */
        changed = 0;
        for (i = 0; i < B->nops; i++) {
            const ip_op_t *o = &B->ops[i];
            int e0 = cc_ent[i][0], e1 = cc_ent[i][1];
            int dyn = (cc_ent[i][2] == -2) || cc_reads_dirty(B, i) || (e0 >= 0 && cc_dirty[e0]) || (e1 >= 0 && cc_dirty[e1]);
            if (o->rtv && e0 < 0) { dyn = 1; }
            if (o->dsv && e1 < 0) { dyn = 1; }
            if (dyn) {
                if (e0 >= 0 && !cc_dirty[e0]) { cc_dirty[e0] = 1; changed = 1; }
                if (e1 >= 0 && !cc_dirty[e1]) { cc_dirty[e1] = 1; changed = 1; }
            }
        }
        if (!changed) { break; }
    }
    for (k = 0; k < IP_SH_MAX; k++) {
        if (cc_wr[k] && !cc_dirty[k]) {
            if (g_ish[k].bakser[bi] == B->serial && g_ish[k].bak[bi]) { cc_nquiet++; }
            else { cc_dirty[k] = 1; } /* ohne Sicherung kein Endstand */
        }
    }
    for (i = 0; i < B->nops; i++) {
        int e0 = cc_ent[i][0], e1 = cc_ent[i][1];
        cc_skip[i] = (unsigned char)((e0 >= 0 || e1 >= 0) && (e0 < 0 || !cc_dirty[e0]) && (e1 < 0 || !cc_dirty[e1]));
        if (cc_skip[i]) { cc_nskip++; }
    }
    /* c) Startkopie unnoetig: erste Beruehrung des Eintrags (Schreiben/Lesen) ist ein
     *    Clear, der ihn ganz ueberschreibt (D3D11-Clear = ganze View; Tiefe nur mit
     *    Tiefe + Stencil), oder der Eintrag ist ruhig (bekommt bak[B]). */
    {
        unsigned char seen[IP_SH_MAX];
        memset(seen, 0, sizeof seen);
        memset(cc_nosc, 0, sizeof cc_nosc);
        cc_nnosc = 0;
        for (i = 0; i < B->nops; i++) {
            const ip_op_t *o = &B->ops[i];
            int e0 = cc_ent[i][0], e1 = cc_ent[i][1], e2 = cc_ent[i][2];
            if (o->kind == IP_CLR_RTV && e0 >= 0 && !seen[e0]) { seen[e0] = 1; cc_nosc[e0] = 1; continue; }
            if (o->kind == IP_CLR_DSV && e1 >= 0 && !seen[e1]) { seen[e1] = 1; cc_nosc[e1] = (o->dsflags & 3u) == 3u; continue; }
            if (e0 >= 0) { seen[e0] = 1; }
            if (e1 >= 0) { seen[e1] = 1; }
            if (e2 >= 0) { seen[e2] = 1; }
            if (o->kind == IP_DRAW && o->rq) {
                for (k = 1; k < 4; k++) {
                    if (o->rq_shsrv[k] && o->rq_shsrv[k] != o->rqr.srv[k]) {
                        int e = cc_ent_of(B, o->rq_shsrv[k], 2);
                        if (e >= 0) { seen[e] = 1; }
                    }
                }
            }
        }
        for (k = 0; k < IP_SH_MAX; k++) {
            if (cc_wr[k] && !cc_dirty[k] && (g_cfg.interp_cockpit_cache & 1)) { cc_nosc[k] = 1; }
            if (cc_nosc[k]) { cc_nnosc++; }
        }
    }
    /* b) BB-Anfang */
    for (i = 0; i < B->nops; i++) {
        const ip_op_t *o = &B->ops[i];
        int bb = (o->rtv == g_rtv) || (cc_bdsv && o->dsv == cc_bdsv);
        int r = cc_ent[i][2];
        if (o->kind == IP_DRAW && (r == cc_bbe || (cc_dep_e >= 0 && r == cc_dep_e))) { cc_why = 1; break; }
        if (!bb) { continue; }
        if (o->kind != IP_DRAW) { cc_pfx = (int)i + 1; continue; }
        if (o->rtv != g_rtv) { cc_why = 2; break; }
        if (o->flag) { cc_why = 3; break; }
        if (o->has_tex & FXR_MASK) { cc_why = 4; break; }
        if (r == -2 || cc_reads_dirty(B, i)) { cc_why = 5; break; }
        cc_pfx = (int)i + 1;
        cc_npfx++;
    }
    for (i = 0; i < B->nops; i++) {
        if (B->ops[i].kind == IP_DRAW && B->ops[i].rtv == g_rtv) { cc_nbb++; }
    }
}

static int cc_mk(ID3D11Texture2D **dst, ID3D11Texture2D *like)
{
    D3D11_TEXTURE2D_DESC d, h;
    ID3D11Texture2D_GetDesc(like, &d);
    if (*dst) {
        ID3D11Texture2D_GetDesc(*dst, &h);
        if (h.Width == d.Width && h.Height == d.Height && h.Format == d.Format && h.SampleDesc.Count == d.SampleDesc.Count) {
            return 1;
        }
        ID3D11Texture2D_Release(*dst);
        *dst = NULL;
    }
    d.MiscFlags = 0;
    d.CPUAccessFlags = 0;
    d.Usage = D3D11_USAGE_DEFAULT;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &d, NULL, dst))) { *dst = NULL; return 0; }
    return 1;
}

/* Stand nach dem BB-Anfang sichern (Modus 1) */
static void cc_store(ip_list_t *A, ip_list_t *B, const ip_cx_t *x)
{
    ID3D11Texture2D *bc = g_ish[B->bbref].sh, *bd = cc_dep_e >= 0 ? g_ish[cc_dep_e].sh : NULL;
    if (!bc || !cc_mk(&cc_col, bc) || (bd && !cc_mk(&cc_dep, bd))) { cc_valid = 0; return; }
    ID3D11DeviceContext_CopyResource(x->c, (ID3D11Resource *)cc_col, (ID3D11Resource *)bc);
    if (bd) { ID3D11DeviceContext_CopyResource(x->c, (ID3D11Resource *)cc_dep, (ID3D11Resource *)bd); }
    cc_valid = 1;
    cc_ser_a = A->serial;
    cc_ser_b = B->serial;
    cc_st_built++;
}

/* Vor ip_sh_start: Scan (einmal je Tick) und Startkopie-Maske. 1 = Cache aktiv. */
static int cc_prep(ip_list_t *B, int bi, int lerp_ok)
{
    int mode = g_cfg.interp_cockpit_cache;
    ip_sh_noskip = NULL;
    if (mode <= 0 || !lerp_ok || B->bbref < 0) { return 0; }
    if (B->ncopy) { return 0; } /* W340: Kopier-Ops kennt die Ruhe-/Anfangs-Analyse nicht -> ohne Cache */
    if (cc_scan_ser != B->serial) {
        cc_scan(B, bi);
        cc_st_n++;
        cc_st_why[cc_why & 7]++;
        cc_st_pfx += cc_npfx;
        cc_st_bb += cc_nbb;
        cc_st_skip += cc_nskip;
        cc_st_q += cc_nquiet;
        cc_st_ops += B->nops;
        cc_st_nosc += cc_nnosc;
    }
    if (mode & 4) { ip_sh_noskip = cc_nosc; } /* Bit 3: Startkopien sparen */
    return 1;
}

/* Rueckgabe: Bit 0..1 = BB-Anfang (0 aus, 1 aufbauen, 2 nutzen), Bit 4 = ruhige Ziele ueberspringen */
static int cc_begin(ip_list_t *B, int bi, ip_list_t *A, int lerp_ok, const ip_cx_t *x)
{
    int m = 0, k;
    int mode = g_cfg.interp_cockpit_cache;
    ip_sh_noskip = NULL;
    if (mode <= 0 || !lerp_ok || B->bbref < 0 || cc_scan_ser != B->serial || B->ncopy) { return 0; }
    cc_st_rep++;
    if ((mode & 1) && cc_nskip) { /* a) ruhige Ziele: Tick-Endstand */
        for (k = 0; k < IP_SH_MAX; k++) {
            if (cc_wr[k] && !cc_dirty[k]) {
                ID3D11DeviceContext_CopyResource(x->c, (ID3D11Resource *)g_ish[k].sh, (ID3D11Resource *)g_ish[k].bak[bi]);
            }
        }
        m |= 16;
    }
    if ((mode & 2) && cc_pfx > 0 &&
        cc_npfx >= (unsigned)(g_cfg.interp_cockpit_cache_min > 0 ? g_cfg.interp_cockpit_cache_min : 1)) {
        if (cc_valid && cc_ser_a == A->serial && cc_ser_b == B->serial) {
            ID3D11DeviceContext_CopyResource(x->c, (ID3D11Resource *)g_ish[B->bbref].sh, (ID3D11Resource *)cc_col);
            if (cc_dep_e >= 0 && cc_dep) {
                ID3D11DeviceContext_CopyResource(x->c, (ID3D11Resource *)g_ish[cc_dep_e].sh, (ID3D11Resource *)cc_dep);
            }
            cc_st_used++;
            m |= 2;
        } else {
            cc_valid = 0;
            m |= 1;
        }
    } else if (mode & 2) {
        cc_st_off++;
    }
    if ((cc_st_rep % 1200u) == 1199u) {
        HLOG_I("INTERP ccache: bilder=%u ticks=%u ops/tick=%.0f ohne-startkopie/tick=%.1f ruhig: ziele/tick=%.1f ops/tick=%.0f | bb-anfang: aufgebaut=%u "
               "genutzt=%u aus=%u bb-draws/tick=%.0f davon im Anfang=%.0f | ende: alle=%u liest-bb=%u fremd-ziel=%u lerp=%u "
               "fx=%u liest-unruhig=%u gross=%u",
               cc_st_rep, cc_st_n, cc_st_n ? (double)cc_st_ops / cc_st_n : 0.0, cc_st_n ? (double)cc_st_nosc / cc_st_n : 0.0,
               cc_st_n ? (double)cc_st_q / cc_st_n : 0.0,
               cc_st_n ? (double)cc_st_skip / cc_st_n : 0.0, cc_st_built, cc_st_used, cc_st_off,
               cc_st_n ? (double)cc_st_bb / cc_st_n : 0.0, cc_st_n ? (double)cc_st_pfx / cc_st_n : 0.0, cc_st_why[0],
               cc_st_why[1], cc_st_why[2], cc_st_why[3], cc_st_why[4], cc_st_why[5], cc_st_why[6]);
    }
    return m;
}

/* ---------- W134 VR-PROTO: Augen-Durchgang ----------
 * Mit ip_vrp != NULL zeichnet ip_replay_ex jeden Draw/Clear, der im Spiel in
 * den Backbuffer ging (o->rtv == g_rtv), je Auge ins Augenziel statt in den
 * BB-Schatten. RT-Paesse laufen unveraendert (Welt im Monitor, Sub-Monitore).
 * VR-VS je Draw-Klasse: Spiel-Clip c -> Sitz-Sicht v (RH) -> Augen-Clip E:
 *  k2.z = 1  3D-Pfad (Objektraum, w = 1): v = (c.x/a', c.y/b, -c.w)
 *  k2.z = 0  2D/RHW (Pixel -> NDC):        v = (n.x*D/a', n.y*D/b, -D)
 *  k2.z = 2  P2 = Cockpit (fast orthografisch, w ~ 1e5): Relief um D aus z/w
 * Tiefe: z_Auge = (c.z/c.w) * w_Auge -> Tiefentest wie im Original (die
 * Klassen haben im Spiel nicht vergleichbare Tiefen, W134_e2). Shading-Draws
 * des 3D-Pfads behalten ihren VS: lineare Augen-Matrix (mvp*S + e44)*E, Tiefe
 * aus der Augenprojektion mit Near/Far der Spielkamera (= Original bei
 * geradem Kopf). Shading-Draws P2/2D im BB: IM-PS-Ersatz. */
sb_vr_pass_t *ip_vrp;
static ID3D11VertexShader *ip_vr_vs1, *ip_vr_vs2;
static ID3D11Buffer *ip_vr_cb;
static int ip_vr_init_done;
static const char *kIP_VR_VS =
    "cbuffer CB : register(b0) { row_major float4x4 mvp; uint has_tex; };\n"
    "cbuffer IP : register(b1) { row_major float4x4 mvp0; float ip_t; };\n"
    "cbuffer VR : register(b2) { row_major float4x4 E; float4 k; float4 k2; };\n"
    "struct VSIN { float4 pos : POSITION0; float4 col : COLOR0;\n"
    "              float2 uv : TEXCOORD0;\n"
    "#ifdef VR_LERP\n"
    "              float4 pos0 : POSITION1;\n"
    "#endif\n"
    "};\n"
    "struct VSOUT { float4 pos : SV_Position; float4 col : COLOR0;\n"
    "               float2 uv : TEXCOORD0;\n"
    "#if defined(VR_LERP) && defined(IP_GUARD)\n" /* FIX(W265 LINE-GUARD): Lagen N/N-1 im Augenraum fuer den GS */
    "               float4 g1 : IPGA; float4 g0 : IPGB; float2 gw : IPGW;\n"
    "#endif\n"
    "};\n"
    "float4 vrpos(float4 c) {\n" /* Spiel-Clip -> Augen-Clip (je Draw-Klasse, s. oben) */
    "  float2 n = c.xy / c.w; float zn = c.z / c.w; float3 v; float md = 0.0; float4 p;\n"
    "  if (k2.z > 1.5) { float d = clamp(k.z + k.w * (zn - k2.x), k.z - k2.y, k.z + k2.y);\n"
    "    if (k2.w > 1.5 && zn > 0.2 && zn < 0.999) { d = min(10.0 / (100.0 - 99.9 * zn), 90.0); md = 1.0; }\n"
    "    v = float3(n.x * d * k.x, n.y * d * k.y, -d); }\n"
    "  else if (k2.z > 0.5) { v = float3(c.x * k.x, c.y * k.y, -c.w); }\n"
    "  else { v = float3(n.x * k.z * k.x, n.y * k.z * k.y, -k.z); }\n"
    "  p = mul(float4(v, 1.0), E);\n"
    "  if (md > 0.5) { p.z -= 2e-5 * p.w; }\n"
    "  else if (k2.w < 0.5 || k2.w > 1.5) { p.z = zn * p.w; }\n"
    "  return p; }\n"
    "VSOUT main(VSIN i) { VSOUT o;\n"
    "  float4 c = mul(i.pos, mvp);\n"
    "#ifdef VR_LERP\n"
    "  float4 c0 = mul(i.pos0, mvp0); float4 cn = c; c = c0 + ip_t * (c - c0);\n"
    "#ifdef IP_EYE\n" /* FIX(W224) wie kIP_VS */
    "  if (c0.w * cn.w <= 0.0) { c = cn; }\n"
    "#endif\n"
    "#ifdef IP_GUARD\n"
    "  o.g0 = vrpos(c0); o.g1 = vrpos(cn); o.gw = float2(c0.w, cn.w);\n"
    "#endif\n"
    "#endif\n"
    "  o.pos = vrpos(c);\n"
    "  o.col = i.col; o.uv = i.uv; return o; }\n";

static int ip_vr_init(void)
{
    ID3DBlob *b = NULL;
    D3D11_BUFFER_DESC bd;
    char src[2048];
    if (ip_vr_init_done) { return ip_vr_vs1 && ip_vr_vs2 && ip_vr_cb; }
    ip_vr_init_done = 1;
    if (sb_im_compile(kIP_VR_VS, "vs_4_0", &b)) {
        ID3D11Device_CreateVertexShader(g_dev, b->lpVtbl->GetBufferPointer(b), b->lpVtbl->GetBufferSize(b), NULL, &ip_vr_vs1);
        b->lpVtbl->Release(b);
        b = NULL;
    }
    _snprintf_s(src, sizeof src, _TRUNCATE, "#define VR_LERP 1\n%s%s%s",
                g_cfg.interp_eyefix ? "#define IP_EYE 1\n" : "", /* W224 */
                (g_cfg.interp_guard || g_cfg.interp_guard_diag) ? "#define IP_GUARD 1\n" : "", kIP_VR_VS); /* W265 */
    if (sb_im_compile(src, "vs_4_0", &b)) {
        ID3D11Device_CreateVertexShader(g_dev, b->lpVtbl->GetBufferPointer(b), b->lpVtbl->GetBufferSize(b), NULL, &ip_vr_vs2);
        b->lpVtbl->Release(b);
    }
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = 96;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &ip_vr_cb))) { ip_vr_cb = NULL; }
    HLOG_I("VR: Wiedergabe-Shader %s", (ip_vr_vs1 && ip_vr_vs2 && ip_vr_cb) ? "bereit" : "FEHLER");
    return ip_vr_vs1 && ip_vr_vs2 && ip_vr_cb;
}

/* P2 im BB: render_im.c setzt mvp = diag(vk, 1, 1, 1) (Clip liegt per CPU vor). */
/* FIX(W265 LINE-GUARD): braucht dieser gelerpte Dreiecks-Draw den Waechter? Nicht noetig ist er nur
 * fuer einen Matrix-Lerp gleicher Objektraum-Vertices mit echter Perspektive (IP_W_LERP, Matrizen
 * verschieden, w haengt von der Lage ab): dort ist der Clip-Lerp exakt die Transformation mit der
 * Zwischenmatrix M_t (ein Dreieck bleibt ein Dreieck, die Rasterung clippt an der Nahebene). Alles
 * andere - neue Eingangsdaten (Vertex i <-> i), gleiche Matrix mit neuem Strom, vorab durch w
 * geteilte P2-/2D-Stroeme (w-Spalte 0, Lerp im Bildraum) - bekommt den GS. interp_guard Bit 4: alle. */
static int ip_gd_need(const ip_op_t *o, const ip_op_t *a)
{
    if (o->topo != 4u && o->topo != 5u) { return 0; } /* GS nur fuer Dreiecks-Liste/Strip */
    if (g_cfg.interp_guard & 4) { return 1; }
    if (o->why != IP_W_LERP || !a) { return 1; }
    if (!memcmp(a->mvp, o->mvp, sizeof o->mvp)) { return 1; }
    return o->mvp[3] == 0.0f && o->mvp[7] == 0.0f && o->mvp[11] == 0.0f;
}

static int ip_vr_isp2(const float m[16])
{
    return m[1] == 0.0f && m[2] == 0.0f && m[3] == 0.0f && m[4] == 0.0f && m[5] == 1.0f && m[6] == 0.0f &&
           m[7] == 0.0f && m[8] == 0.0f && m[9] == 0.0f && m[10] == 1.0f && m[11] == 0.0f && m[12] == 0.0f &&
           m[13] == 0.0f && m[14] == 0.0f && m[15] == 1.0f;
}

void ip_m4mul(float r[16], const float a[16], const float b[16])
{
    int i, j;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            r[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] +
                           a[i * 4 + 2] * b[2 * 4 + j] + a[i * 4 + 3] * b[3 * 4 + j];
        }
    }
}

/* 3D-Pfad (Objektraum, w = 1): Spiel-mvp -> Augen-mvp, linear:
 * v = (X/a', Y/b, -W, pos.w) = pos * (mvp * S + e44), danach * E. */
static void ip_vr_mat3d(float out[16], const float mvp[16], const sb_vr_pass_t *p, int e)
{
    float S[16], T[16];
    memset(S, 0, sizeof S);
    S[0] = p->ia; S[5] = p->ib; S[14] = -1.0f;
    ip_m4mul(T, mvp, S);
    T[15] += 1.0f;
    ip_m4mul(out, T, p->E[e]);
}

static void ip_vr_eye_target(const ip_cx_t *x, const sb_vr_pass_t *p, int e, const ip_op_t *o)
{
    D3D11_VIEWPORT v;
    D3D11_RECT r;
    v.TopLeftX = p->vp[e][0]; v.TopLeftY = p->vp[e][1];
    v.Width = p->vp[e][2]; v.Height = p->vp[e][3];
    v.MinDepth = o->vp.MinDepth; v.MaxDepth = o->vp.MaxDepth; /* Tiefenbereich des Spiels (Schichtung) */
    r.left = (LONG)p->vp[e][0]; r.top = (LONG)p->vp[e][1];
    r.right = (LONG)(p->vp[e][0] + p->vp[e][2]); r.bottom = (LONG)(p->vp[e][1] + p->vp[e][3]);
    ID3D11DeviceContext_OMSetRenderTargets(x->c, 1, (ID3D11RenderTargetView *const *)&p->rtv,
                                           o->dsv ? (ID3D11DepthStencilView *)p->dsv : NULL);
    ID3D11DeviceContext_RSSetViewports(x->c, 1, &v);
    ID3D11DeviceContext_RSSetScissorRects(x->c, 1, &r);
}

static void ip_vr_cb_write(const ip_cx_t *x, const sb_vr_pass_t *p, int e, int kind)
{
    D3D11_MAPPED_SUBRESOURCE map;
    float *f;
    if (FAILED(ID3D11DeviceContext_Map(x->c, (ID3D11Resource *)ip_vr_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) { return; }
    f = (float *)map.pData;
    /* W164: k2.w = 1 (vr_world_depth, 3D-Pfad = Cockpit-Mesh): Tiefe affin aus der
     * Augenprojektion statt Original-z/w x w_Auge (das extrapoliert bei Dreiecken, die
     * die Augen-Ebene schneiden -> Flaechen mit Tiefe ~0 verdecken alles);
     * k2.w = 2 (vr_inst_depth, P2): Vertices mit perspektivischem z/w (> 0,2) auf die
     * metrische Tiefe w = n f / (f - z/w (f - n)) (Instrumente bleiben auf ihrer
     * Tafel), Tiefe ebenfalls affin (+ 2e-5 zum Auge: Decals mit LESS_EQUAL) */
    memcpy(f, p->E[e], 64);
    f[16] = p->ia; f[17] = p->ib; f[18] = p->d2d; f[19] = p->rl;
    f[20] = p->rz0; f[21] = p->rmax; f[22] = (float)kind;
    f[23] = (kind == 1 && p->wfix) ? 1.0f : ((kind == 2 && p->ifix) ? 2.0f : 0.0f);
    ID3D11DeviceContext_Unmap(x->c, (ID3D11Resource *)ip_vr_cb, 0);
}

/* Nicht-Shading-Draw je Auge. VS/Layout/Streams des bisherigen Pfads sind
 * gebunden (mode 0 = exakt, 1 = Lerp); P2 bindet den VR-VS und setzt mode = -1. */
/* W264 DRAW-OPT2: Draw eines aufgezeichneten Ops - indiziert (o->ib1) ueber den
 * festen Strip/Fan/Quad-Indexpuffer, sonst als Liste; base = erster Vertex
 * (0, wenn die Stroeme per Versatz gebunden sind). */
static void ip_draw_op(ID3D11DeviceContext *c, const ip_op_t *o, unsigned n, unsigned base)
{
    /* FIX(ISSUE-EMP,W352) line_scale: Linie als Band wie im Tick-Bild (GS je Draw an/aus; Linien-Draws
     * haben nie den W265-Waechter, mit interp_guard traegt der Lerp-VS Zusatzausgaben -> dann alt). */
    if (o->lgs && !g_cfg.interp_guard) {
        ID3D11DeviceContext_GSSetShader(c, o->lgs, NULL, 0);
        if (o->ib1) { sb_im_ib_bind(c); ID3D11DeviceContext_DrawIndexed(c, n, o->ib1 - 1u, (INT)base); }
        else { ID3D11DeviceContext_Draw(c, n, base); }
        ID3D11DeviceContext_GSSetShader(c, NULL, NULL, 0);
        return;
    }
    if (o->ib1) {
        sb_im_ib_bind(c);
        ID3D11DeviceContext_DrawIndexed(c, n, o->ib1 - 1u, (INT)base);
    } else {
        ID3D11DeviceContext_Draw(c, n, base);
    }
}

static void ip_vr_draw(const ip_cx_t *x, sb_vr_pass_t *p, const ip_op_t *o, const ip_op_t *a, int kind,
                       float t, int lerp, int *mode)
{
    int e;
    ID3D11Buffer *cbs[3];
    if (g_cfg.vr_diag_skip & (1 << kind)) { return; } /* DIAG(VR-FIX2,W164): Draw-Klasse weglassen */
    if (!ip_vr_init()) { return; }
    ID3D11DeviceContext_VSSetShader(x->c, lerp ? ip_vr_vs2 : ip_vr_vs1, NULL, 0);
    cbs[0] = x->cb0; cbs[1] = x->cb1; cbs[2] = ip_vr_cb;
    ID3D11DeviceContext_VSSetConstantBuffers(x->c, 0, 3, cbs);
    if (lerp) { ip_cb_write(x, x->cb1, IP_AMVP(o, a), 0, t, 1); }
    *mode = -1;
    for (e = 0; e < p->neyes; e++) {
        ip_vr_eye_target(x, p, e, o);
        ip_vr_cb_write(x, p, e, kind);
        if (lerp) { ip_draw_op(x->c, o, o->nv, 0); }
        else { ip_draw_op(x->c, o, o->nv, o->vstart); }
    }
}

/* ---------- W183 VR-FIX3: Aussenwelt im Spieltakt (vr_world_tick=1) ----------
 * Je VR-Bild muss nur das Kopfabhaengige neu: die Draws, die im Spiel in den
 * Backbuffer gingen (Cockpit-Mesh, Instrumente, Monitor-Komposition, HUD) -
 * je Auge mit der aktuellen Pose. Die RT-Paesse (Aussenwelt im Hauptmonitor,
 * Sub-Monitore, Karte; ~75 % der Draws) haengen nicht vom Kopf ab: statt sie
 * je VR-Bild (90 Hz) neu zu zeichnen, bekommen ihre Schatten den gesicherten
 * Tick-Endstand bak[B] (Spieltakt 30 Hz, wie das Original). Sicher nur, wenn
 * kein BB-Draw ein RT liest, bevor B es zum letzten Mal schreibt, und jedes
 * geschriebene RT eine Sicherung von B hat - sonst volle Wiedergabe. Einmal je
 * Tick (Seriennummer) geprueft. */
static unsigned char vt_skip[CC_MAXOPS], vt_wr[IP_SH_MAX], vt_nosc[IP_SH_MAX];
static unsigned vt_ser, vt_nskip;
static int vt_ok;

static int vt_reads_late(const ip_list_t *B, const ip_op_t *o, unsigned i, const unsigned *lastw)
{
    int k, e;
    if (o->sh_srv && o->sh_srv != o->srv) {
        e = cc_ent_of(B, o->sh_srv, 2);
        if (e < 0) { return 1; }
        if (vt_wr[e] && i < lastw[e]) { return 1; }
    }
    if (o->rq) {
        for (k = 1; k < 4; k++) {
            if (o->rq_shsrv[k] && o->rq_shsrv[k] != o->rqr.srv[k]) {
                e = cc_ent_of(B, o->rq_shsrv[k], 2);
                if (e < 0 || (vt_wr[e] && i < lastw[e])) { return 1; }
            }
        }
    }
    return 0;
}

static void vt_scan(const ip_list_t *B, int bi)
{
    unsigned i, lastw[IP_SH_MAX];
    int k, bbread = 0, bbdep = -1;
    vt_ser = B->serial;
    vt_ok = 1;
    vt_nskip = 0;
    memset(vt_wr, 0, sizeof vt_wr);
    memset(vt_nosc, 0, sizeof vt_nosc);
    memset(lastw, 0, sizeof lastw);
    if (B->nops > CC_MAXOPS || B->ncopy) { vt_ok = 0; return; } /* W340: Kopier-Ops nicht in der VR-Analyse */
    for (i = 0; i < B->nops; i++) {
        const ip_op_t *o = &B->ops[i];
        int bb = (o->kind == IP_DRAW && o->rtv == g_rtv) || (o->kind == IP_CLR_RTV && o->rtv == g_rtv) ||
                 (o->kind == IP_CLR_DSV && o->dsv == g_dsv);
        vt_skip[i] = (unsigned char)!bb;
        if (bb) {
            if (o->kind == IP_DRAW && o->sh_dsv && bbdep < 0) { bbdep = cc_ent_of(B, o->sh_dsv, 1); }
            continue;
        }
        vt_nskip++;
        if (o->kind != IP_CLR_DSV) {
            int e0 = cc_ent_of(B, o->sh_rtv, 0);
            if (o->sh_rtv && e0 < 0) { vt_ok = 0; return; }
            if (e0 >= 0) { vt_wr[e0] = 1; lastw[e0] = i + 1u; }
        }
        if (o->kind != IP_CLR_RTV) {
            int e1 = cc_ent_of(B, o->sh_dsv, 1);
            if (o->sh_dsv && e1 < 0) { vt_ok = 0; return; }
            if (e1 >= 0) { vt_wr[e1] = 1; lastw[e1] = i + 1u; }
        }
    }
    for (i = 0; i < B->nops; i++) {
        const ip_op_t *o = &B->ops[i];
        int r;
        if (vt_skip[i] || o->kind != IP_DRAW) { continue; }
        if (vt_reads_late(B, o, i, lastw)) { vt_ok = 0; return; }
        r = (o->sh_srv && o->sh_srv != o->srv) ? cc_ent_of(B, o->sh_srv, 2) : -1;
        if (r >= 0 && (r == B->bbref || r == bbdep)) { bbread = 1; }
    }
    for (k = 0; k < IP_SH_MAX; k++) {
        if (!vt_wr[k]) { continue; }
        if (g_ish[k].bakser[bi] != B->serial || !g_ish[k].bak[bi]) { vt_ok = 0; return; }
        vt_nosc[k] = 1; /* bekommt bak[B] statt der Startkopie bak[A] */
    }
    /* BB-Schatten: im Augenbild ungenutzt (Augenziel), ausser ein BB-Draw liest ihn. Die BB-Tiefe
     * behaelt ihre Startkopie (Effekt-Draws kopieren sie, W133). */
    if (!bbread && B->bbref >= 0 && B->bbref < IP_SH_MAX && !vt_wr[B->bbref]) { vt_nosc[B->bbref] = 1; }
}

/* ---------- W193 PAUSE-LIVE: Wiedergabe-Abweichungen der Pause-Neuzeichnung ----------
 * Nur waehrend sb_interp_pause_render (Spiel-Thread in der Pause, ctx_lock) gesetzt; alle
 * anderen Wiedergaben (Zwischenbilder, Verify, VR) sehen ip_pl_on = 0 -> unveraendert. */
int ip_pl_on;                   /* 1 = laufende Wiedergabe ist eine Pause-Neuzeichnung */
int ip_pl_noshade;              /* Shading-Draws mit IM-PS-Ersatz (shading jetzt aus, wie VR vr_shading=0) */
unsigned ip_pl_mfx;             /* Bit 0: Korn-Wash auslassen, Bit 1: Stoerzeilen auslassen (monitor_fx) */
ID3D11Buffer *ip_pl_cbsrc;      /* Konstanten-Fenster der Liste (Aufzeichnung) ... */
ID3D11Buffer *ip_pl_cb;         /* ... und die neu berechnete Kopie (sfx0..sfx3, Schattenstufen) */
ID3D11Buffer *ip_pl_vb;         /* W209: pause-only rebuilt vertex buffer */
float (*ip_pl_mvp)[16];         /* matrices paired with that buffer */
unsigned ip_pl_nhook, ip_pl_nmfx, ip_pl_nnoshade; /* Zaehler der letzten Neuzeichnung */
int ip_pl_tex;                  /* 1 = in der Pause neu geladene Texturen tauschen (sb_tex_pause_map) */
static ID3D11ShaderResourceView *ip_ph_srv(ID3D11ShaderResourceView *s); /* W195: skalierte Zwischenziele */

/* ---------- W195 PHOTO-MODE: Abweichungen der Foto-Wiedergabe (zusaetzlich zu ip_pl_*) ----------
 * Nur waehrend sb_interp_photo_render (Pause, ctx_lock): ip_ph.on = 1. Sonst 0 -> alle Pfade unveraendert.
 *  - Umlenkung: Draws/Clears in das Ziel tgt (Farbe) bzw. tgtd (Tiefe) gehen in die Foto-Kachel
 *    (rtv/dsv), Viewport und Scissor werden affin abgebildet (x' = x*kx + ox, y' = y*ky + oy).
 *  - Freie Kamera: Welt-Draws (Ziel wld, Clip-w echt) bekommen MVP * M (Clip -> Sicht der
 *    Spielkamera -> verschobene/gedrehte Sicht -> Clip), M je Draw aus seinem Spiel-Viewport. */
sb_ph_rep_t ip_ph;
unsigned ip_ph_ncam, ip_ph_nred, ip_ph_nclr;
unsigned ip_ph_nhud; /* W290: ausgelassene HUD-Draws (hud_off) */

/* W290 QUICK-SHOT: HUD-Draw des Hauptmonitors (nur Foto-Wiedergabe mit hud_off): 2D-Draw in das Monitor-RT, das
 * nicht das Welt-RT kopiert und kein Korn/Stoerzeilen-Draw (W105, o->nolerp) ist. */
static int ip_ph_is_hud(const ip_op_t *o)
{
    if (!ip_ph.hud_off || ip_ph.mon < 0 || o->kind != IP_DRAW || o->cls || o->nolerp) { return 0; }
    if (o->sh_rtv != g_ish[ip_ph.mon].rtv) { return 0; }
    if (ip_ph.wld >= 0 && o->sh_srv && o->sh_srv == g_ish[ip_ph.wld].srv) { return 0; }
    return 1;
}
/* W428 PHOTO-CINEMATIC: Draw des eigenen VT (nur Foto-Wiedergabe mit cine): 3D-Draw in das Welt-RT, dessen
 * Modell-Ursprung naeher als photo_cine_self/10 Einheiten an der Spielkamera liegt. Lage aus der mvp und der
 * Spiel-Projektion des Draws (Zeilenvektoren: Ursprung -> Clip = Zeile 3; x = m12/a, y = m13/b, Tiefe = w = m15).
 * Draws mit Pixel-Matrix (P2/RHW, auf der CPU transformiert) tragen keine Sicht-Lage und bleiben. */
/* W445 PHOTO-CINE-SELF: kleinster Abstand zur Spielkamera aus Modell-Ursprung und den 4 Bezugs-Vertices (pref: erster,
 * 1/3, 2/3, letzter) in Kamera-Sicht (x = c0/a, y = c1/b, Tiefe = w). 1e30 = keine Sicht-Projektion. */
float ip_ph_near_ref(const ip_op_t *o)
{
    float a = o->prj[0], b = o->prj[1], x, y, z, d2, best;
    int q;
    if (a <= 1e-4f || b <= 1e-4f) { return 1e30f; }
    x = o->mvp[12] / a;
    y = o->mvp[13] / b;
    z = o->mvp[15];
    best = x * x + y * y + z * z;
    for (q = 0; q < 4; q++) {
        float c[4];
        ip_clip(o->pref[q], o->mvp, c);
        x = c[0] / a;
        y = c[1] / b;
        z = c[3];
        d2 = x * x + y * y + z * z;
        if (d2 < best) { best = d2; }
    }
    return sqrtf(best);
}
unsigned ip_ph_nself; /* ausgelassene Draws des eigenen VT (cine) */
unsigned ip_ph_nextra; /* W445: davon Luecke (Draw mitten in der Folge eigener Teile) */
static int ip_ph_is_self_dist(const ip_op_t *o)
{
    const vsh_prog_t *vp;
    float a, b, x, y, z, r;
    if (!ip_ph.cine || ip_ph.wld < 0 || o->kind != IP_DRAW || !o->cls || o->sh_rtv != g_ish[ip_ph.wld].rtv) { return 0; }
    vp = o->pvs ? vsh_program(o->pvs) : NULL;
    if (vp && vp->kind == 2) { return (g_cfg.photo_cine_diag & 2) ? 1 : 0; } /* DIAG(W428): P2 testweise aus */
    if (o->mvp[3] == 0.0f && o->mvp[7] == 0.0f && o->mvp[11] == 0.0f && o->mvp[15] == 1.0f) { return 0; } /* Pixel-Matrix */
    a = o->prj[0];
    b = o->prj[1];
    r = (float)g_cfg.photo_cine_self * 0.1f;
    if (a <= 1e-4f || b <= 1e-4f || r <= 0.0f) { return 0; }
    x = o->mvp[12] / a;
    y = o->mvp[13] / b;
    z = o->mvp[15];
    if (x * x + y * y + z * z > r * r) { return 0; }
    static unsigned s_ndg;
    if ((g_cfg.photo_cine_diag & 1) && s_ndg < 200u) {
        s_ndg++;
        HLOG_I("PHOTO cine: eigener VT nv=%u tex=%x Ursprung %.2f,%.2f,%.2f (Abstand %.2f)", o->nv, o->has_tex, (double)x,
               (double)y, (double)z, sqrt((double)(x * x + y * y + z * z)));
    }
    return 1;
}
/* W445 PHOTO-CINE-SELF: Merker je Op, einmal je Foto-Wiedergabe (ip_ph_self_prep): 1 = eigener Teil (Abstandsregel),
 * 3 = Luecke (3D-Draw mitten in der Folge eigener Teile). Gepruefte und verworfene Merkmale: BERICHT_M2 W445 (P2-Kopien
 * ohne Bildwirkung, der P2-Draw direkt vor den eigenen Teilen ist der Himmel, naechster Vertex trifft Gelaende/Strassen). */
static unsigned char ip_ph_self_f[IP_OPS_MAX];
const ip_list_t *ip_ph_self_L; /* NULL = neu bestimmen (je sb_interp_photo_render) */
static int ip_ph_world3d(const ip_op_t *o)
{
    return o->kind == IP_DRAW && o->cls && o->sh_rtv == g_ish[ip_ph.wld].rtv;
}
/* W445: Sichtlage-Abstand des Ursprungs (wie ip_ph_is_self_dist, ohne Schwelle); 1e30 = keine Sicht-Matrix/P2 */
static float ip_ph_org_dist(const ip_op_t *o)
{
    const vsh_prog_t *vp = o->pvs ? vsh_program(o->pvs) : NULL;
    float a = o->prj[0], b = o->prj[1], x, y, z;
    if ((vp && vp->kind == 2) || a <= 1e-4f || b <= 1e-4f) { return 1e30f; }
    if (o->mvp[3] == 0.0f && o->mvp[7] == 0.0f && o->mvp[11] == 0.0f && o->mvp[15] == 1.0f) { return 1e30f; }
    x = o->mvp[12] / a;
    y = o->mvp[13] / b;
    z = o->mvp[15];
    return sqrtf(x * x + y * y + z * z);
}
static void ip_ph_self_prep(const ip_list_t *B)
{
    unsigned i, j, n = B->nops < IP_OPS_MAX ? B->nops : IP_OPS_MAX, nown = 0, nsw = 0;
    float r2 = (float)g_cfg.photo_cine_self * 0.2f; /* Lueckenregel: hoechstens doppelte Reichweite */
    ip_ph_self_L = B;
    memset(ip_ph_self_f, 0, n);
    for (i = 0; i < n; i++) {
        if (ip_ph_is_self_dist(&B->ops[i])) { ip_ph_self_f[i] = 1; nown++; }
    }
    if (!nown) { return; }
    /* Luecke: 3D-Draw (Sicht-Matrix) zwischen zwei eigenen Teilen der Zeichenfolge (naechster 3D-Draw davor und danach
     * im Welt-RT eigen) und naeher als 2x Reichweite - z. B. das Dreieck (Sprite) ~6,2-6,8 vor dem Auge zwischen Rohr
     * und Arm, das mit der Kamera mitfaehrt (Zensus W445). Fremde Objekte stehen nicht mitten in der VT-Folge. */
    for (j = 0; j < n; j++) {
        unsigned p, q;
        if (ip_ph_self_f[j] || !ip_ph_world3d(&B->ops[j]) || ip_ph_org_dist(&B->ops[j]) > r2) { continue; }
        for (p = j; p-- > 0;) { if (ip_ph_world3d(&B->ops[p])) { break; } }
        for (q = j + 1; q < n; q++) { if (ip_ph_world3d(&B->ops[q])) { break; } }
        if (p < j && q < n && ip_ph_self_f[p] == 1 && ip_ph_self_f[q] == 1) { ip_ph_self_f[j] = 3; nsw++; }
    }
    if (g_cfg.photo_cine_diag & 1) { /* DIAG(W445): je Wiedergabe-Liste Merker (begrenzt) */
        static unsigned s_nsum;
        if (s_nsum < 400u) {
            char t[1024];
            int k = 0;
            s_nsum++;
            t[0] = 0;
            for (j = 0; j < n && k < (int)sizeof t - 24; j++) {
                if (ip_ph_self_f[j]) { k += _snprintf_s(t + k, sizeof t - (size_t)k, _TRUNCATE, " %u:%u/%u", j, ip_ph_self_f[j], B->ops[j].nv); }
            }
            HLOG_I("PHOTO cine: Merker ser=%u eigen=%u Luecke=%u |%s", B->serial, nown, nsw, t);
        }
    }
}
static int ip_ph_is_self(const ip_list_t *B, unsigned i)
{
    if (!ip_ph.cine || ip_ph.wld < 0 || i >= IP_OPS_MAX) { return 0; }
    if (ip_ph_self_L != B) { ip_ph_self_prep(B); }
    if (ip_ph_self_f[i] > 1) { ip_ph_nextra++; }
    return ip_ph_self_f[i] != 0;
}
/* W428: Einblendung (Cinematic): Draw ins Monitor-RT, der nicht das Welt-RT kopiert (HUD-Glyphen, Marken, Radar,
 * FRONT VIEW, Texte), bzw. Draw in den Backbuffer, der weder das Monitor-RT noch das Welt-RT zeigt (Einblendungen
 * und Cockpit-Reste der Vollbild-Ansicht). Ohne Monitor-RT bzw. Welt-RT im Zensus: nichts auslassen. */
unsigned ip_ph_ncmfx, ip_ph_ncovl;
static int ip_ph_cine_ovl(const ip_op_t *o, int bb)
{
    ID3D11ShaderResourceView *ws = ip_ph.wld >= 0 ? g_ish[ip_ph.wld].srv : NULL;
    ID3D11ShaderResourceView *ms = ip_ph.mon >= 0 ? g_ish[ip_ph.mon].srv : NULL;
    if (!ws) { return 0; }
    if (ms && o->sh_rtv == g_ish[ip_ph.mon].rtv) { return o->sh_srv != ws; }
    if (bb) { return o->sh_srv != ws && (!ms || o->sh_srv != ms); }
    return 0;
}
ID3D11ShaderResourceView *ip_ph_shsrv[8]; /* Schatten-SRV der skalierten Eintraege (gesetzt in ip_pl_render) */

static ID3D11ShaderResourceView *ip_ph_srv(ID3D11ShaderResourceView *s)
{
    int j;
    if (!ip_ph.on || !ip_ph.nsc || !s) { return s; }
    for (j = 0; j < ip_ph.nsc; j++) { if (s == ip_ph_shsrv[j]) { return ip_ph.sc_srv[j]; } }
    return s;
}

static int ip_ph_sc_rtv(ID3D11RenderTargetView *v)
{
    int j;
    for (j = 0; v && j < ip_ph.nsc; j++) { if (ip_ph.sc_e[j] >= 0 && v == g_ish[ip_ph.sc_e[j]].rtv) { return j; } }
    return -1;
}

static int ip_ph_sc_dsv(ID3D11DepthStencilView *v)
{
    int j;
    for (j = 0; v && j < ip_ph.nsc; j++) { if (ip_ph.sc_d[j] >= 0 && v == g_ish[ip_ph.sc_d[j]].dsv) { return j; } }
    return -1;
}


/* Clip-Matrix (Zeilenvektor, c' = c * M) eines Welt-Draws: Viewport-Abbildung rv (Spiel-Viewport im RT)
 * zurueck, Projektion zurueck, Kamera-Delta, Projektion, rv. 0 = nicht anwendbar. */
static int ip_ph_cam_matrix(const ip_op_t *o, float M[16])
{
    float P[16], Pi[16], RV[16], RVi[16], T[16], U[16];
    /* eine Spielkamera fuer alle Welt-Draws: Projektion aus dem Zensus (a aus dem Anzeige-Seitenverhaeltnis,
     * s. render_photo.c ph_rep_base); o->prj ist g_proj_game zur Zeit des Draws und nicht fuer jeden Draw die
     * Projektion seines Vertex-Programms (W195-Befund: a = b im Zensus, Monitor zeigt 4:3) */
    float a = ip_ph.def_prj[0], b = ip_ph.def_prj[1], A = ip_ph.def_prj[2], Bz = ip_ph.def_prj[3];
    (void)o->prj;
    memset(P, 0, sizeof P);
    P[0] = a; P[5] = b; P[10] = A; P[11] = -1.0f; P[14] = Bz;
    if (!ip_m4inv(Pi, P)) { return 0; }
    memset(RV, 0, sizeof RV);
    RV[0] = 1.0f; RV[5] = 1.0f; RV[10] = 1.0f; RV[15] = 1.0f;
    if (o->rtl[0] && o->rtl[1] && o->vpg[2] && o->vpg[3] &&
        !(o->vpg[0] == 0 && o->vpg[1] == 0 && o->vpg[2] == o->rtl[0] && o->vpg[3] == o->rtl[1])) {
        float rw = (float)o->rtl[0], rh = (float)o->rtl[1];
        RV[0] = (float)o->vpg[2] / rw;
        RV[12] = ((float)o->vpg[2] + 2.0f * (float)o->vpg[0]) / rw - 1.0f;
        RV[5] = (float)o->vpg[3] / rh;
        RV[13] = 1.0f - ((float)o->vpg[3] + 2.0f * (float)o->vpg[1]) / rh;
    }
    if (!ip_m4inv(RVi, RV)) { return 0; }
    /* Zeilenvektor: c_f' = c_f * RVi * Pi * D * P * RV */
    ip_m4mul(T, RVi, Pi);
    ip_m4mul(U, T, ip_ph.cam_d);
    ip_m4mul(T, U, P);
    ip_m4mul(M, T, RV);
    return 1;
}

static int ip_ph_is_world(const ip_op_t *o)
{
    if (ip_ph.wld < 0 || o->kind != IP_DRAW) { return 0; }
    if (o->sh_rtv != g_ish[ip_ph.wld].rtv) { return 0; }
    if (o->cls) { return 1; }
    /* 2D/RHW im Welt-RT: nur mit echtem Clip-w (vorab transformierte Welt, nicht Bildschirm-Quads) */
    return o->rn[0][2] > 1.001f || o->rn[0][2] < 0.999f || o->rn[3][2] > 1.001f || o->rn[3][2] < 0.999f;
}
/* W361: tot gemeldete Views unter Quarantaene stellen (nie wieder binden oder freigeben - Release auf ein
 * zerstoertes Objekt waere selbst ein Fehler), Historie verwerfen. Unter ctx_lock, Render-Thread ruhig. */
void ip_dead_purge(void)
{
    LONG n = ip_dead_n, i;
    int e, k, j, q;
    if (n > IP_DEAD_MAX) { n = IP_DEAD_MAX; }
    ip_rt_quiesce();
    for (i = 0; i < n; i++) {
        ID3D11View *d = ip_dead_v[i];
        if (!d) { continue; }
        for (k = 0; k < IP_LISTS; k++) {
            ip_list_t *L = &g_ipl[k];
            for (j = 0; j < L->nrefs && j < IP_REFS_MAX; j++) {
                for (q = 0; q < 3; q++) { if (L->refo[j].v[q] == d) { L->refo[j].v[q] = NULL; } }
            }
        }
        for (e = 0; e < IP_SH_MAX; e++) {
            ip_sh_t *s = &g_ish[e];
            int hit = 0;
            if ((ID3D11View *)s->rtv == d) { s->rtv = NULL; s->pin[0] = 0; hit = 1; }
            if ((ID3D11View *)s->dsv == d) { s->dsv = NULL; s->pin[1] = 0; hit = 1; }
            if ((ID3D11View *)s->srv == d) { s->srv = NULL; s->pin[2] = 0; hit = 1; }
            if (hit && s->res) { ip_sh_free(s); } /* Rest frei, Eintrag wird beim naechsten Sichern neu angelegt */
        }
        ip_dead_v[i] = NULL;
    }
    ip_dead_n = 0;
    ip_hist_clear(); /* Ops der Listen zeigen auf die tote View */
    HLOG_W("INTERP W361: %ld zerstoerte Schatten-View(s) in Quarantaene, Historie verworfen (Zwischenbilder setzen kurz aus)", n);
}

/* ---------- W376 LOC-LOCAL-AIM: eigene Sicht sofort im Bild ----------
 * Je Wiedergabe (Tick- und Zwischenbild) das Vorhersage-Delta der Sicht lerp(A, B, t) als Kameradrehung C auf die
 * Welt-Draws (o->law, gesetzt am Swap) anwenden: mvp' = mvp * C, ebenso die Matrix N-1. Spielzustand unberuehrt.
 * Vorzeichen (Messlauf W376_A2): Sicht-Gier des Spiels faellt beim Blick nach rechts, Nick steigt beim Blick nach oben. */
static const double LA_TURN_K = -1.0, LA_NICK_K = 1.0, LA_MAXANG = 0.6;
volatile LONG li_la_rep, li_la_on; /* Wiedergaben mit Liste gueltig / mit Korrektur (LOCAIM-Zeile) */
volatile LONG li_la_lists, li_la_draws, li_la_rets; /* Listen mit Vorhersage, Welt-/Fadenkreuz-Draws darin */
void ip_la_mul(float out[16], const float m[16], const float *C)
{
    int i, j;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            out[i * 4 + j] = m[i * 4] * C[j] + m[i * 4 + 1] * C[4 + j] + m[i * 4 + 2] * C[8 + j] + m[i * 4 + 3] * C[12 + j];
}
/* DIAG(W376) loc_aim_diag Bit 32: je Wiedergabe Zeit, Tick, t, Drehung (angewandt bzw. mit Schalter aus: berechnet) */
static CRITICAL_SECTION li_la_cs;
static volatile LONG li_la_cs_init;
static void li_la_note(const ip_list_t *B, float t, double dy, double dp, int on, double rx, double ry)
{
    static FILE *f;
    struct { LONGLONG q; unsigned ser, seq; float t, dy, dp; int on; float rx, ry; } r;
    LARGE_INTEGER q;
    if (InterlockedCompareExchange(&li_la_cs_init, 1, 0) == 0) { InitializeCriticalSection(&li_la_cs); InterlockedExchange(&li_la_cs_init, 2); }
    while (li_la_cs_init != 2) { Sleep(0); }
    QueryPerformanceCounter(&q);
    r.q = q.QuadPart; r.ser = B->serial; r.seq = B->la_seq; r.t = t; r.dy = (float)dy; r.dp = (float)dp; r.on = on;
    r.rx = (float)rx; r.ry = (float)ry;
    EnterCriticalSection(&li_la_cs);
    if (!f) { f = (FILE *)loc_aim_diag_file("locaim_rep.bin"); }
    if (f) { fwrite(&r, sizeof r, 1, f); fflush(f); }
    LeaveCriticalSection(&li_la_cs);
}
/* W394 LOC-VIEW-PERFRAME: angezeigte Sicht/Arm (absolut, rad) fuer diese Wiedergabe. Basis = juengste Vorhersage (D eigene
 * Saetze), dazu ein Zusatz-Schritt mit dem Satz, den der naechste Poll jetzt liefern wuerde (Maus-Akku nur gelesen),
 * gewichtet mit der Phase bis zu diesem Poll (w 0 -> 1). Zwischen Poll und naechster Vorhersage (der Satz ist schon
 * abgeholt, aber noch nicht in der Vorhersage) gilt der letzte volle Zusatz-Schritt. Rueckgabe 0 = nichts (alter Weg),
 * 1 = Basis ohne Zusatz-Schritt, 2 = mit Zusatz-Schritt, 3 = gehalten. */
static SRWLOCK li_vpf_lk = SRWLOCK_INIT;
static la_state_t li_vpf_full;
static uint32_t li_vpf_full_seq;
volatile LONG li_vpf_n[4]; /* LOCVPF-Zeile: Wiedergaben je Rueckgabe */
/* W505 ISSUE-106: Gleichmaessigkeit der gezeigten Sicht-Gier je Bild (Zeile LOCVPF-GLATT, nur Zaehler) */
static SRWLOCK li_sm_lk = SRWLOCK_INIT;
static lh_smooth_t li_sm;
void li_vpf_smooth_take(unsigned out[3], double *rel_mean)
{
    AcquireSRWLockExclusive(&li_sm_lk);
    out[0] = li_sm.turn; out[1] = li_sm.stall; out[2] = li_sm.jump;
    *rel_mean = li_sm.turn ? li_sm.rel_sum / li_sm.turn : 0.0;
    li_sm.turn = li_sm.stall = li_sm.jump = 0; li_sm.rel_sum = 0.0;
    ReleaseSRWLockExclusive(&li_sm_lk);
}
/* Zeitbasis = Zustandszeit des Bildes (lerp der Listen-Zeiten mit t): laeuft mit den Faelligkeiten der Bilder (stetige
 * Bilduhr W393), nicht mit dem Zeitpunkt der Wiedergabe (der Render-Thread bereitet Bilder vorab vor). */
static void li_vpf_smooth_note(const ip_list_t *A, const ip_list_t *B, int lerp_ok, float t, double yaw)
{
    double ts = (double)B->tstate;
    if (lerp_ok && A) { ts = (double)A->tstate + (double)t * (double)(B->tstate - A->tstate); }
    AcquireSRWLockExclusive(&li_sm_lk);
    lh_smooth_add(&li_sm, ts * 1000.0 / (double)ip_qpf.QuadPart, yaw);
    ReleaseSRWLockExclusive(&li_sm_lk);
}
static int li_vpf_view(double v[4], LONGLONG fw)
{
    loc_aim_pub_t P;
    uint8_t F[14];
    uint32_t q = 0;
    la_state_t s1;
    LARGE_INTEGER now;
    int r, mode;
    double w;
    if (!loc_aim_get(&P) || !P.have_pst) { InterlockedIncrement(&li_vpf_n[0]); return 0; }
    r = loc_vpf_preview(P.last_in, F, &q);
    if (r) (void)loc_aimassist_preview(&P.pst, P.last_in, F); /* W442: naechster Poll mit Zielhilfe (sonst Sicht-Ruckeln) */
    QueryPerformanceCounter(&now);
    AcquireSRWLockExclusive(&li_vpf_lk);
    if (r == 2 && (q == P.poll || q == P.poll + 1u)) {
        /* W505 ISSUE-106: Eingabeprofil - der wiederholte Satz haengt nicht am Maus-Akku, gilt also auch zwischen Poll und
         * naechster Vorhersage. Rampe ueber den ganzen Abstand zweier Vorhersagen (statt bis zum Poll und dann halten):
         * sonst stand je Tick ein Bild still und das naechste sprang (Bildfolge J2: 5/5/5/5/9/1 px). */
        s1 = P.pst;
        if (!la_step(&s1, F)) { s1 = P.pst; }
        /* Zeit = Bildzeit fw (Zustandszeit + Latenz), nicht der Zeitpunkt der Wiedergabe: ein Bild wird teils zweimal
         * wiedergegeben (vorab und nach einer neuen Liste), dann mit schon neuer Vorhersage. Linear auch etwas ueber 0..1
         * hinaus: beim Wechsel der Vorhersage gilt w_neu = w_alt - 1 (stetig), spaete Ticks drehen weiter statt zu halten. */
        w = lh_rep_weight((double)(fw - P.tq) * 1000.0 / (double)ip_qpf.QuadPart, P.tick_ms);
        lh_blend(&P.pst, &s1, w, v);
        li_vpf_full = s1;
        li_vpf_full_seq = P.seq;
        mode = 2;
    } else if (r == 1 && q == P.poll) {
        s1 = P.pst;
        if (!la_step(&s1, F)) { s1 = P.pst; }
        w = lh_weight((double)(now.QuadPart - P.tq) * 1000.0 / (double)ip_qpf.QuadPart, P.wspan_ms);
        lh_blend(&P.pst, &s1, w, v);
        li_vpf_full = s1;
        li_vpf_full_seq = P.seq;
        mode = 2;
    } else if (r == 1 && q == P.poll + 1u && li_vpf_full_seq == P.seq) {
        v[0] = li_vpf_full.ys; v[1] = li_vpf_full.ps; v[2] = li_vpf_full.ax; v[3] = li_vpf_full.ay;
        mode = 3;
    } else {
        v[0] = P.pst.ys; v[1] = P.pst.ps; v[2] = P.pst.ax; v[3] = P.pst.ay;
        mode = 1;
    }
    ReleaseSRWLockExclusive(&li_vpf_lk);
    InterlockedIncrement(&li_vpf_n[mode]);
    return mode;
}
/* DIAG(W394) loc_hud_diag Bit 4: je Wiedergabe angezeigte Sicht-Gier und Fadenkreuz-Mitte (nur Testfolge) */
static void li_vpf_note(const ip_list_t *B, float t, int mode, double yaw, double rcx, double rcy, double syaw, double sx)
{
    static FILE *f;
    static SRWLOCK lk = SRWLOCK_INIT;
    struct { LONGLONG q; unsigned ser; float t; int mode; float yaw, rcx, rcy, syaw, sx; } r;
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    r.q = q.QuadPart; r.ser = B->serial; r.t = t; r.mode = mode; r.yaw = (float)yaw; r.rcx = (float)rcx; r.rcy = (float)rcy;
    r.syaw = (float)syaw; r.sx = (float)sx;
    AcquireSRWLockExclusive(&lk);
    if (!f) { f = (FILE *)loc_aim_diag_file("locvpf_rep.bin"); }
    if (f) { fwrite(&r, sizeof r, 1, f); }
    ReleaseSRWLockExclusive(&lk);
}
volatile LONG lr_rep, lr_ret, lr_lost, lr_exact, lr_nola, lr_jumps, lr_jumps_all;
volatile LONG lr_retx; /* W439: Fadenkreuz-Draws mit Verschiebung gegen B (exakt gezeichnet) */
volatile LONG lr_rjmv; /* W439: Fadenkreuz-Kandidaten, die die Bewegungs-Pruefung verwarf */
/* W444: Listen mit Fadenkreuz, Teile je Regel (Form/Partner/Start), verworfene fremde Start-Kandidaten, Listen mit um das Gedaechtnis ergaenzter Vorlage */
volatile LONG lrm_st_lists, lrm_st_form, lrm_st_pair, lrm_st_seed, lrm_st_fremd, lrm_st_mem;
double lr_err_max;
static int lr_hist_n;
static double lr_hist[3][2];
static LONGLONG lr_hist_q[3];
/* F11: Sprung-Zaehler (nur Zaehler) */
unsigned loc_ret_jumps(void) { return (unsigned)lr_jumps_all; }
static void li_ret_note(const ip_list_t *B, const ip_list_t *A, int lerp_ok, float t, int on, const li_ret_t *rr,
                        float M[][16])
{
    static FILE *f;
    static SRWLOCK lk = SRWLOCK_INIT;
    struct {
        LONGLONG q; unsigned ser, aser; float t; int lerp_ok, stage, mode, on, retB, retA, nret, nlerp, nlost;
        float tx, ty, gx, gy, bx, by, vy, vp, rx, ry, rcBx, rcBy, rn0x, rn0y; unsigned rb, rjB, nrA;
        int fi, fm; float dx, dy; /* erster Fadenkreuz-Draw: Op-Index, Partner in A, gezeichnete Lage (Bezugs-Vertex) */
        int npl; float epl;       /* Teile ohne Markierung in B, deren Partner in A Fadenkreuz war; groesste Abweichung vom Soll */
        float gx2, gy2, epl2, emk2; /* W444: Mitte mit Teil-Versatz je Teil (gelerpt zwischen A und B), groesste Abweichung je Teil
                                   * vom eigenen Soll (Lage ohne Vorhersage + Verschiebung) fuer unmarkierte bzw. markierte Teile */
    } r;
    LARGE_INTEGER q;
    double gx = 0.0, gy = 0.0, gx2 = 0.0, gy2 = 0.0;
    int n = 0, nl = 0, nlost = 0;
    unsigned i;
    if (!g_sba_loc) { return; }
    memset(&r, 0, sizeof r);
    r.rn0x = r.rn0y = 9.0f;
    InterlockedIncrement(&lr_rep);
    if (rr->stage == 2 && B->la_ret) {
        for (i = 0; i < B->nops; i++) {
            const ip_op_t *o = &B->ops[i];
            double px, py;
            int lp;
            if (o->kind != IP_DRAW || !LA_IS_RET(o->law)) { continue; }
            lp = lerp_ok && o->flag && o->match >= 0 && A && (unsigned)o->match < A->nops;
            px = o->rn[0][0]; py = o->rn[0][1];
            if (lp) { const ip_op_t *a = &A->ops[o->match]; px = a->rn[0][0] + (px - a->rn[0][0]) * (double)t; py = a->rn[0][1] + (py - a->rn[0][1]) * (double)t; nl++; }
            if (LA_ON(on, o->law)) { px += M[o->law - 1][12]; py += M[o->law - 1][13]; } /* angewandte Verschiebung */
            gx += px - ((double)o->rn[0][0] - B->la_rc[0]);
            gy += py - ((double)o->rn[0][1] - B->la_rc[1]);
            { /* W444: Soll je Teil = eigener Versatz zur simulierten Mitte (gelerpt wie der Draw) + Soll-Mitte */
                double ox = (double)o->rn[0][0] - B->la_rc[0], oy = (double)o->rn[0][1] - B->la_rc[1], e2;
                if (lp && A->la_ok) {
                    const ip_op_t *a = &A->ops[o->match];
                    ox = ((double)a->rn[0][0] - A->la_rc[0]) + (ox - ((double)a->rn[0][0] - A->la_rc[0])) * (double)t;
                    oy = ((double)a->rn[0][1] - A->la_rc[1]) + (oy - ((double)a->rn[0][1] - A->la_rc[1])) * (double)t;
                }
                gx2 += px - ox; gy2 += py - oy;
                e2 = sqrt((px - ox - rr->tx) * (px - ox - rr->tx) + (py - oy - rr->ty) * (py - oy - rr->ty));
                if (e2 > r.emk2) { r.emk2 = (float)e2; }
            }
            if (!n) { r.rn0x = o->rn[0][0]; r.rn0y = o->rn[0][1]; r.fi = (int)i; r.fm = o->match; r.dx = (float)px; r.dy = (float)py; }
            n++;
        }
        if (A && A->la_ret && lerp_ok) { /* Teile, die B nicht markierte (ohne Verschiebung gezeichnet) */
            for (i = 0; i < B->nops; i++) {
                const ip_op_t *o = &B->ops[i];
                const ip_op_t *a;
                double px, py, e;
                if (o->kind != IP_DRAW || LA_IS_RET(o->law) || o->match < 0 || (unsigned)o->match >= A->nops) { continue; }
                a = &A->ops[o->match];
                if (!LA_IS_RET(a->law)) { continue; }
                px = o->rn[0][0]; py = o->rn[0][1];
                if (o->flag) { px = a->rn[0][0] + (px - a->rn[0][0]) * (double)t; py = a->rn[0][1] + (py - a->rn[0][1]) * (double)t; }
                px -= (double)a->rn[0][0] - A->la_rc[0];
                py -= (double)a->rn[0][1] - A->la_rc[1];
                e = sqrt((px - rr->tx) * (px - rr->tx) + (py - rr->ty) * (py - rr->ty));
                r.npl++;
                if (e > r.epl) { r.epl = (float)e; }
                /* W444: fehlende Verschiebung = Abweichung vom eigenen Soll (gelerpt rx, exakt Soll - Mitte B) */
                e = o->flag ? sqrt(rr->rx * rr->rx + rr->ry * rr->ry)
                            : sqrt((rr->tx - B->la_rc[0]) * (rr->tx - B->la_rc[0]) + (rr->ty - B->la_rc[1]) * (rr->ty - B->la_rc[1]));
                if (e > r.epl2) { r.epl2 = (float)e; }
            }
        }
    } else if (A && A->la_ret && lerp_ok) {
        /* Fadenkreuz in A markiert, in B nicht (bzw. B ohne Vorhersage): Partner der A-Fadenkreuz-Draws ohne Verschiebung */
        for (i = 0; i < B->nops; i++) {
            const ip_op_t *o = &B->ops[i];
            const ip_op_t *a;
            double px, py;
            if (o->kind != IP_DRAW || o->match < 0 || (unsigned)o->match >= A->nops) { continue; }
            a = &A->ops[o->match];
            if (!LA_IS_RET(a->law)) { continue; }
            px = o->rn[0][0]; py = o->rn[0][1];
            if (o->flag) { px = a->rn[0][0] + (px - a->rn[0][0]) * (double)t; py = a->rn[0][1] + (py - a->rn[0][1]) * (double)t; nl++; }
            gx += px - ((double)a->rn[0][0] - A->la_rc[0]);
            gy += py - ((double)a->rn[0][1] - A->la_rc[1]);
            nlost++;
            n++;
        }
    }
    if (n) { gx /= n; gy /= n; gx2 /= n; gy2 /= n; }
    if (rr->stage == 1) { InterlockedIncrement(&lr_nola); }
    if (n && !nlost) { InterlockedIncrement(&lr_ret); }
    if (nlost) { InterlockedIncrement(&lr_lost); }
    if (n && nl < n) { InterlockedIncrement(&lr_exact); }
    QueryPerformanceCounter(&q);
    { /* Sprung: gezeichnete Mitte weicht vom Mittel der Nachbarn ab und kehrt zurueck (Nachbar-Abstand klein) */
        AcquireSRWLockExclusive(&lk);
        if (n) {
            double e;
            lr_hist[0][0] = lr_hist[1][0]; lr_hist[0][1] = lr_hist[1][1]; lr_hist_q[0] = lr_hist_q[1];
            lr_hist[1][0] = lr_hist[2][0]; lr_hist[1][1] = lr_hist[2][1]; lr_hist_q[1] = lr_hist_q[2];
            lr_hist[2][0] = gx; lr_hist[2][1] = gy; lr_hist_q[2] = q.QuadPart;
            if (lr_hist_n < 3) { lr_hist_n++; }
            if (lr_hist_n == 3 && (double)(lr_hist_q[2] - lr_hist_q[0]) * 1000.0 / (double)ip_qpf.QuadPart < 120.0) {
                double mx = 0.5 * (lr_hist[0][0] + lr_hist[2][0]), my = 0.5 * (lr_hist[0][1] + lr_hist[2][1]);
                double ex = lr_hist[1][0] - mx, ey = lr_hist[1][1] - my;
                double nx = lr_hist[2][0] - lr_hist[0][0], ny = lr_hist[2][1] - lr_hist[0][1];
                e = sqrt(ex * ex + ey * ey);
                if (e > lr_err_max) { lr_err_max = e; }
                if (e > 0.02 && e > 2.0 * sqrt(nx * nx + ny * ny)) { InterlockedIncrement(&lr_jumps); InterlockedIncrement(&lr_jumps_all); }
            }
        } else {
            lr_hist_n = 0;
        }
        ReleaseSRWLockExclusive(&lk);
    }
    if (!(g_cfg.loc_ret_diag & 1) || (!g_cfg.loc_replay_tick[0] && !g_cfg.loc_steps[0])) { return; }
    r.q = q.QuadPart; r.ser = B->serial; r.aser = A ? A->serial : 0u; r.t = t; r.lerp_ok = lerp_ok; r.stage = rr->stage;
    r.mode = rr->mode; r.on = on; r.retB = B->la_ret; r.retA = A ? A->la_ret : 0; r.nret = n; r.nlerp = nl; r.nlost = nlost;
    r.tx = (float)rr->tx; r.ty = (float)rr->ty; r.gx = n ? (float)gx : 9.0f; r.gy = n ? (float)gy : 9.0f;
    r.gx2 = n ? (float)gx2 : 9.0f; r.gy2 = n ? (float)gy2 : 9.0f;
    r.bx = (float)rr->bx; r.by = (float)rr->by; r.vy = (float)rr->vy; r.vp = (float)rr->vp; r.rx = (float)rr->rx; r.ry = (float)rr->ry;
    r.rcBx = B->la_rc[0]; r.rcBy = B->la_rc[1];
    r.rjB = B->la_rjmv; r.nrA = A ? A->la_nret : 0u;
    { loc_rb_hud h; r.rb = loc_rb_hud_read(&h) ? h.rb_total : 0u; }
    AcquireSRWLockExclusive(&lk);
    if (!f) { f = (FILE *)loc_aim_diag_file("locret_rep.bin"); }
    if (f) { fwrite(&r, sizeof r, 1, f); }
    ReleaseSRWLockExclusive(&lk);
}

/* W394 LOC-HUD-VIEW: Marken je VT (o->law 3 + j) um die Abweichung der angezeigten von der simulierten Bildlage
 * verschieben: b = c * C (C = Sicht-Korrektur dieser Wiedergabe, sonst Einheit), Verschiebung b/w - a (Clip-Translation).
 * Rueckgabe: Bits 2 + j. */
volatile LONG li_hm_rep, li_hm_on; /* LOCHUD-Zeile */
volatile LONG li_hm_lists, li_hm_draws, li_hm_cmp, li_hm_near; /* LOCHUD-Zeile (near: Gegner < 100 m im Bild) */
double li_hm_res_max, li_hm_res_sum; unsigned li_hm_res_n;
volatile LONG li_hm_rad; /* Listen mit Radar-Draws */
volatile LONG li_hm_cand, li_hm_rej_mv, li_hm_rej_st; /* Kandidaten nahe a_j, verworfen (Bewegung, ruhend) */
double li_rad_k; int li_rad_n; /* Radar: Drehung der Karte je rad eigener Gier (+-1 nach 6 gleichen Messungen) */ /* Rest: Verschiebung aus B gegen lerp(A, B, t) */
double li_cmp_k = 0.477; /* NDC je rad Gier (Eichung C3; laufend nachgefuehrt) */

double li_hm_px_max, li_hm_px_sum; unsigned li_hm_px_n;
static int ip_hm_prep(const ip_list_t *B, const ip_list_t *A, float t, const float *C, float M[LA_NM][16])
{
    int j, on = 0;
    InterlockedIncrement(&li_hm_rep);
    for (j = 0; j < LS_MAXV; j++) {
        double dx, dy, ax, ay, r;
        if (!(B->hm_vt & (1u << j))) { continue; }
        if (!lh_mark_shift(B->hm_c[j], C, B->hm_a[j][0], B->hm_a[j][1], &dx, &dy)) { continue; }
        if (A && A->hm_ok && A->hm_c[j][3] > 0.0 && lh_mark_shift(A->hm_c[j], C, A->hm_a[j][0], A->hm_a[j][1], &ax, &ay)) {
            /* Statistik: angewandt (aus B) gegen lerp(A, B, t) */
            r = (1.0 - t) * sqrt((ax - dx) * (ax - dx) * 102400.0 + (ay - dy) * (ay - dy) * 57600.0);
            if (r > li_hm_res_max) { li_hm_res_max = r; } li_hm_res_sum += r; li_hm_res_n++;
        }
        if (fabs(dx) + fabs(dy) < 1e-5) { continue; }
        memset(M[2 + j], 0, 64);
        M[2 + j][0] = M[2 + j][5] = M[2 + j][10] = M[2 + j][15] = 1.0f;
        M[2 + j][12] = (float)dx;
        M[2 + j][13] = (float)dy;
        on |= 1 << (2 + j);
        { double px = sqrt(dx * 320.0 * dx * 320.0 + dy * 240.0 * dy * 240.0); /* Statistik (ungeschuetzt, nur Anzeige) */
          if (px > li_hm_px_max) { li_hm_px_max = px; } li_hm_px_sum += px; li_hm_px_n++; }
    }
    if (on) { InterlockedIncrement(&li_hm_on); }
    return on;
}

static int ip_la_prep0(const ip_list_t *B, const ip_list_t *A, int lerp_ok, float t, int dbg, int vr, float M[LA_NM][16],
                       li_ret_t *rr);
/* Rueckgabe Bitmaske: 1 = Kamera-Korrektur M[0] (o->law 1), 2 = Fadenkreuz-Verschiebung M[1] (o->law 2),
 * 4 << j = Marken des VT j (W394). */
static int ip_la_prep(const ip_list_t *B, const ip_list_t *A, int lerp_ok, float t, int dbg, int vr, float M[LA_NM][16])
{
    li_ret_t rr;
    int on;
    memset(&rr, 0, sizeof rr);
    on = ip_la_prep0(B, A, lerp_ok, t, dbg, vr, M, &rr);
    if (rr.stage) { li_ret_note(B, A, lerp_ok, t, on, &rr, M); } /* W439: Fadenkreuz-Zeitreihe/Sprung-Zaehler */
    if (g_sba_loc && (g_cfg.loc_wlag_diag & 4) && !dbg && !vr && B->la_ok) { /* DIAG(W435): Himmel in Wiedergaben mit Sicht-Korrektur */
        InterlockedIncrement(&sk_rep);
        if (on & 1) {
            InterlockedIncrement(&sk_rep_on);
            InterlockedExchangeAdd(&sk_rep_sky, (LONG)B->la_nsk);
            InterlockedExchangeAdd(&sk_rep_skyc, (LONG)B->la_nskl);
            InterlockedExchangeAdd(&sk_rep_na, (LONG)B->la_nna);
            sk_ang_sum += sk_last_ang; /* nur Anzeige-Statistik */
            if (sk_last_ang > sk_ang_max) { sk_ang_max = sk_last_ang; }
        }
    }
    if (g_sba_loc && g_cfg.loc_hud_predict && !dbg && !vr && !ip_ph.on && !ip_pl_on && g_cfg.interp_mode != 1) {
        if (B->hm_ok && B->hm_vt) { on |= ip_hm_prep(B, lerp_ok ? A : NULL, t, (on & 1) ? M[0] : NULL, M); }
        if (B->hm_ncmp && fabs(B->hm_dyaw) > 1e-6) { /* Kompassband: eigene Gier angezeigt - simuliert (W390) */
            float *m = M[LA_LAW_CMP - 1];
            memset(m, 0, 64);
            m[0] = m[5] = m[10] = m[15] = 1.0f;
            m[12] = (float)la_clamp(li_cmp_k * B->hm_dyaw, -0.5, 0.5);
            on |= 1 << (LA_LAW_CMP - 1);
        }
        if (B->hm_nrad && li_rad_n >= 6 && fabs(B->hm_dyaw) > 1e-6) { /* Radar: Karte um die Bildmitte drehen */
            float *m = M[LA_LAW_RAD - 1];
            double ph = la_clamp(li_rad_k * B->hm_dyaw, -0.5, 0.5), cs = cos(ph), sn = sin(ph);
            memset(m, 0, 64);
            m[0] = (float)cs; m[1] = (float)sn; m[4] = (float)-sn; m[5] = (float)cs; m[10] = m[15] = 1.0f;
            on |= 1 << (LA_LAW_RAD - 1);
        }
    }
    return on;
}
static int ip_la_prep0(const ip_list_t *B, const ip_list_t *A, int lerp_ok, float t, int dbg, int vr, float M[LA_NM][16],
                       li_ret_t *rr)
{
    static const double up[3] = {0.0, 1.0, 0.0};
    double dy, dp, Cd[16], rx, ry;
    int i, apply, on = 0;
    if (!g_sba_loc || dbg || vr || ip_ph.on || ip_pl_on || g_cfg.interp_mode == 1 || !B->la_ok) {
        if (g_sba_loc && !dbg && !vr && !ip_ph.on && !ip_pl_on && g_cfg.interp_mode != 1 && g_cfg.loc_local_aim) { rr->stage = 1; }
        return 0;
    }
    apply = g_cfg.loc_local_aim != 0;
    dy = B->la_d[0]; dp = B->la_d[1];
    if (lerp_ok && A && A->la_ok) {
        dy = A->la_d[0] + (B->la_d[0] - A->la_d[0]) * (double)t;
        dp = A->la_d[1] + (B->la_d[1] - A->la_d[1]) * (double)t;
    }
    dy = la_clamp(dy * LA_TURN_K, -LA_MAXANG, LA_MAXANG);
    dp = la_clamp(dp * LA_NICK_K, -LA_MAXANG, LA_MAXANG);
    rx = ry = 0.0;
    if (B->la_ret) {
        rx = B->la_rd[0]; ry = B->la_rd[1];
        if (lerp_ok && A && A->la_ok) {
            rx = A->la_rd[0] + (B->la_rd[0] - A->la_rd[0]) * (double)t;
            ry = A->la_rd[1] + (B->la_rd[1] - A->la_rd[1]) * (double)t;
        }
    }
    if (apply && (g_cfg.loc_view_perframe || (g_cfg.loc_hud_diag & 4))) { /* W394 LOC-VIEW-PERFRAME */
        double v[4], sy = B->la_s[0], sp = B->la_s[1], bx = B->la_rc[0], by = B->la_rc[1];
        double c1 = sqrt(B->la_vp[1] * B->la_vp[1] + B->la_vp[5] * B->la_vp[5] + B->la_vp[9] * B->la_vp[9]);
        double c3 = sqrt(B->la_vp[3] * B->la_vp[3] + B->la_vp[7] * B->la_vp[7] + B->la_vp[11] * B->la_vp[11]);
        double py = c3 > 0.0 ? c1 / c3 : 0.0;
        int mode = 0;
        if (lerp_ok && A && A->la_ok) { /* so zeichnet die Zwischenbild-Wiedergabe den simulierten Stand */
            sy = la_wrap(A->la_s[0] + la_wrap(B->la_s[0] - A->la_s[0]) * (double)t);
            sp = la_wrap(A->la_s[1] + la_wrap(B->la_s[1] - A->la_s[1]) * (double)t);
            bx = A->la_rc[0] + (B->la_rc[0] - A->la_rc[0]) * (double)t;
            by = A->la_rc[1] + (B->la_rc[1] - A->la_rc[1]) * (double)t;
        }
        LONGLONG fw = (lerp_ok && A ? A->tstate + (LONGLONG)((double)t * (double)(B->tstate - A->tstate)) : B->tstate) + ip_lat; /* W505: Bildzeit */
        if (g_cfg.loc_view_perframe && !loc_vpf_test_off() && (mode = li_vpf_view(v, fw)) != 0) {
            dy = la_clamp(la_wrap(v[0] - sy) * LA_TURN_K, -LA_MAXANG, LA_MAXANG);
            dp = la_clamp(la_wrap(v[1] - sp) * LA_NICK_K, -LA_MAXANG, LA_MAXANG);
            if (B->la_ret) {
                rx = -py * tan(la_clamp(v[2] - v[0], -1.4, 1.4)) - bx;
                ry = -py * tan(la_clamp(v[3] + v[1], -1.4, 1.4)) - by;
            }
        }
        if ((g_cfg.loc_hud_diag & 4) && (g_cfg.loc_replay_tick[0] || g_cfg.loc_steps[0])) {
            li_vpf_note(B, t, mode, sy + dy / LA_TURN_K, bx + rx, by + ry, sy, bx);
        }
        rr->stage = 2; rr->mode = mode; rr->tx = bx + rx; rr->ty = by + ry; rr->bx = bx; rr->by = by; /* W439 */
        rr->vy = sy + dy / LA_TURN_K; rr->vp = sp + dp / LA_NICK_K; rr->rx = rx; rr->ry = ry;
        li_vpf_smooth_note(A, B, lerp_ok, t, rr->vy); /* W505: gezeigte Sicht-Gier dieses Bildes */
    }
    if (g_cfg.loc_aim_diag & 32) { li_la_note(B, t, dy, dp, apply, rx, ry); }
    sk_last_ang = sqrt(dy * dy + dp * dp); /* DIAG(W435): Winkel der Sicht-Korrektur dieser Wiedergabe (rad) */
    if (!apply) { return 0; }
    InterlockedIncrement(&li_la_rep);
    if (fabs(dy) + fabs(dp) >= 1e-6 && la_cam_corr(Cd, B->la_vp, up, dy, dp)) {
        for (i = 0; i < 16; i++) { M[0][i] = (float)Cd[i]; }
        on |= 1;
    }
    if (B->la_ret) { /* Fadenkreuz: Clip-Verschiebung x += dx * w, y += dy * w (Zeilenvektor: Zeile 3) */
        if (fabs(rx) + fabs(ry) >= 1e-5) {
            memset(M[1], 0, 64);
            M[1][0] = M[1][5] = M[1][10] = M[1][15] = 1.0f;
            M[1][12] = (float)la_clamp(rx, -1.0, 1.0); M[1][13] = (float)la_clamp(ry, -1.0, 1.0);
            on |= 2;
        }
        if (g_cfg.loc_ret_fix) {
            /* W439 LOC-RETICLE-JUMP: Die Verschiebung rx = Soll - lerp(A, B, t) der simulierten Mitte gilt nur fuer
             * gelerpte Fadenkreuz-Draws. Draws, die im Zwischenbild exakt aus Tick N gezeichnet werden (Sprung zu gross
             * fuer die strenge 2D-Zuordnung, o->law LA_LAW_RETX), liegen bei der Mitte von B -> Soll - B. Vorher lag das
             * Fadenkreuz dort bei jedem Tick um (1 - t) * Tickschritt neben dem Soll (Ruck beim schnellen Zielen). */
            double e2[2], ex, ey;
            lh_ret_exact_shift(rx, ry, A ? A->la_rc : B->la_rc, B->la_rc, (double)t, lerp_ok && A && A->la_ok, e2);
            ex = e2[0]; ey = e2[1];
            if (fabs(ex) + fabs(ey) >= 1e-5) {
                float *m = M[LA_LAW_RETX - 1];
                memset(m, 0, 64);
                m[0] = m[5] = m[10] = m[15] = 1.0f;
                m[12] = (float)la_clamp(ex, -1.0, 1.0); m[13] = (float)la_clamp(ey, -1.0, 1.0);
                on |= 1 << (LA_LAW_RETX - 1);
            }
        }
    }
    if (on) { InterlockedIncrement(&li_la_on); }
    return on;
}

void ip_replay_ex(ip_list_t *B, int bi, ip_list_t *A, int ai, float t, int dbg, const ip_cx_t *x)
{
    int cps = -1; /* DIAG: 1 = Diagnose-PS gebunden */
    unsigned cwhy = 0xFFFFFFFFu;
    ip_state_t sv;
    unsigned i;
    UINT stride = sizeof(im_vertex_t), zero = 0;
    ID3D11RenderTargetView *crtv = (ID3D11RenderTargetView *)1;
    ID3D11DepthStencilView *cdsv = (ID3D11DepthStencilView *)1;
    ID3D11BlendState *cbs = (ID3D11BlendState *)1;
    ID3D11DepthStencilState *cds = (ID3D11DepthStencilState *)1;
    UINT csref = 0xFFFFFFFFu;
    ID3D11RasterizerState *crs = (ID3D11RasterizerState *)1;
    ID3D11ShaderResourceView *csrv = (ID3D11ShaderResourceView *)1;
    D3D11_VIEWPORT cvp;
    D3D11_RECT csc;
    unsigned ctopo = 0xFFFFFFFFu;
    int mode = -1; /* 0 exakt-VS, 1 interp-VS */
    int rq_dirty = 0; /* RENDER-QA: letzter Draw war ein Shading-Draw */
    ID3D11SamplerState *csmp = (ID3D11SamplerState *)1; /* FIX(TEXADDR,W84): gebundener s0 (1 = unbekannt) */
    int lerp_ok = (A != NULL && t > 0.0f && t < 1.0f && A->ready && B->pair_ok &&
                   B->prevser == A->serial);
    fxr_rep_t fxs; /* W133 REM-FX: Effekt-PS/Tiefen-Kopie der Wiedergabe */
    sb_vr_pass_t *vrp = (ip_vrp && !x->deferred && !dbg) ? ip_vrp : NULL; /* W134 VR-PROTO */
    int vrbb = 0, vrkind = -1; /* W134 VR-PROTO */
    int ccm = 0; /* W146 GPU-COST: Cockpit-Cache 0 aus, 1 aufbauen, 2 nutzen */
    int vtk = 0; /* W183 VR-FIX3: 1 = RT-Ops uebersprungen (Tick-Endstand) */
    ip_rc_t rc; /* DIAG(PERF-M2,W174) ip_gdiag */
    unsigned var = 0u, rcur[3] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu }; /* W174: Variante, gebundene Ring-Plaetze VS b0/b1, PS b0 */
    int ring = 0, vbgB = 0, vbgA = 0; /* PERF(PERF-M2,W174) */
    unsigned *rofs = NULL;
    ID3D11Buffer *bvb, *avb = NULL; /* W174: Tick-VB von B/A (GPU-Kopie oder DYNAMIC) */
    ID3D11GeometryShader *cgd = NULL; /* FIX(W265 LINE-GUARD): gebundener Waechter-GS */
    int gdon, gdq = -1;
    ID3D11RenderTargetView *rpw = NULL; /* W295 WORLD-REPROJ: Welt-Schatten (Welt-Ops bis rplast durch den Warp ersetzen) */
    ID3D11DepthStencilView *rpd = NULL;
    unsigned rplast = 0xFFFFFFFFu;
    float la_M[LA_NM][16], la_tmp[16]; /* W376 LOC-LOCAL-AIM, W394 */
    int la_on = ip_la_prep(B, A, lerp_ok, t, dbg, vrp != NULL, la_M);
#define IP_LAM(o, m) (LA_ON(la_on, (o)->law) ? (ip_la_mul(la_tmp, (m), la_M[(o)->law - 1]), (const float *)la_tmp) : (const float *)(m))
    (void)ip_list_refs_check(B, bi, "B"); /* W361 */
    if (A) { (void)ip_list_refs_check(A, ai, "A"); }
    fxr_rep_begin(&fxs);
    memset(&rc, 0, sizeof rc);
    if (!dbg && !vrp) {
        unsigned ab = (unsigned)g_cfg.ip_ablate;
        var = ip_var_pick(B, t);
        ring = (ab & 80u) ? (var == 16u || var == 64u) : (g_cfg.ip_cb_ring > 0);
        vbgB = (ab & 96u) ? (var == 32u || var == 64u) : (g_cfg.ip_vb_gpu > 0);
        if (ip_pl_on) { ring = 0; var = 0u; } /* W193: Konstanten je Draw (MVP-Haken), keine Diagnose-Varianten */
    }
    vbgA = vbgB && A && (A->gpu_ok & 1u);
    vbgB = vbgB && (B->gpu_ok & 1u);
    bvb = (ip_pl_on && ip_pl_vb) ? ip_pl_vb : ip_vbsel(B, vbgB);
    if (A) { avb = ip_vbsel(A, vbgA); }
    if ((g_cfg.interp_rhash & 1) && lerp_ok && !dbg) { ip_rh_inputs(B, bi, A, ai, t, x, vbgA, vbgB, ring); } /* DIAG(W273) */
    rc.var = var;
    rc.nops = B->nops;
    rc.nverts = B->nverts;
    rc.rqxb = B->rqxpos;
    rc.vbg = (unsigned)vbgB;
    memset(&cvp, 0xFF, sizeof cvp);
    memset(&csc, 0xFF, sizeof csc);
    gdon = lerp_ok && (ip_gd_im || ip_gd_sh); /* W265: Waechter nur in Zwischenbildern (t < 1) */
    if (gdon && g_cfg.interp_guard_diag && !dbg) { /* DIAG(W265): Pipeline-Statistik dieser Wiedergabe (auch Deferred) */
        unsigned q = ip_gd_qi % IP_GD_Q;
        if (!x->deferred) { ip_gd_poll(); }
        if (ip_gd_q[q] && !ip_gd_qbusy[q]) {
            gdq = (int)q;
            ID3D11DeviceContext_Begin(x->c, (ID3D11Asynchronous *)ip_gd_q[gdq]);
        } else { st_gd_lost++; }
        ip_gd_qi++;
    }
    if (!x->deferred) {
        ip_state_save(&sv);
        if (!dbg) { ip_gs_begin(t); } /* DIAG(TICK-PERF,W125) */
    } else {
        /* PERF(RENDER-THREAD,W126): Deferred-Context beginnt leer. Einziger
         * geerbter Zustand der Wiedergabe ist s0 (tex_addr=0, Draw ohne
         * eigenen Sampler) -> s0 des Spiels beim Aufnahme-Start von B. */
        ID3D11DeviceContext_PSSetSamplers(x->c, 0, 1, &B->smp0);
    }
    if (lerp_ok && !dbg && !vrp && !ip_pl_on && !ip_ph.on && g_cfg.interp_mode == 1 && ip_rp_ser[bi] == B->serial &&
             ip_rp_we[bi] >= 0 && sb_rp_ready(bi, B->serial)) {
        rpw = g_ish[ip_rp_we[bi]].rtv;
        rpd = ip_rp_wd[bi] >= 0 ? g_ish[ip_rp_wd[bi]].dsv : NULL;
        rplast = ip_rp_last[bi];
    }
    if (!x->deferred && !vrp && !dbg && !ip_rp_prep) { cc_prep(B, bi, lerp_ok); } /* W146 GPU-COST: Scan + Startkopie-Maske */
    if (vrp && vrp->wtick) { /* W183 VR-FIX3: Aussenwelt im Spieltakt (Tick-Endstand), nur BB-Ops je Auge */
        static int vt_have;
        if (!vt_have || vt_ser != B->serial) { vt_scan(B, bi); vt_have = 1; }
        vtk = vt_ok;
        vrp->wtick_used = vtk;
        vrp->nskip = vtk ? vt_nskip : 0u;
        if (vtk) { ip_sh_noskip = vt_nosc; }
    }
    ip_sh_start(B, bi, A, ai, x);
    ip_sh_noskip = NULL;
    if (ip_rp_prep == 2 && ip_rp_idrtv) { static const float k0[4] = { 0, 0, 0, 0 }; ID3D11DeviceContext_ClearRenderTargetView(x->c, ip_rp_idrtv, k0); } /* W295: ID 0 */
    if (ip_ph.on && ip_ph.init && ip_ph.tgt >= 0) { ip_ph.init(ip_ph.ctx, g_ish[ip_ph.tgt].sh); } /* W195: Kachel vorbelegen */
    if (vtk) {
        int k;
        for (k = 0; k < IP_SH_MAX; k++) {
            if (vt_wr[k]) { ID3D11DeviceContext_CopyResource(x->c, (ID3D11Resource *)g_ish[k].sh, (ID3D11Resource *)g_ish[k].bak[bi]); }
        }
    }
    if (!x->deferred && !vrp && !dbg && !ip_rp_prep) { ccm = cc_begin(B, bi, A, lerp_ok, x); } /* W146 GPU-COST */
    if (ip_gs_n >= 0) { ip_gs_mark((ID3D11RenderTargetView *)2); } /* DIAG(TICK-PERF,W125): Kopien fertig */
    if (ring) { /* PERF(PERF-M2,W174) ip_cb_ring: alle Draw-Konstanten in einem Map */
        ring = ip_ring_fill(x, B, A, lerp_ok, t, &rc, la_on, la_M);
        rofs = ip_ring[x->deferred ? 1 : 0].ofs;
    }
    rc.ring = (unsigned)ring;
    ID3D11DeviceContext_PSSetShader(x->c, g_im_ps, NULL, 0);
    if (!ring) { ID3D11DeviceContext_PSSetConstantBuffers(x->c, 0, 1, &x->cb0); }
    for (i = 0; i < B->nops; i++) {
        ip_op_t *o = &B->ops[i];
        int lerp;
        int phred = 0; /* W195: Viewport/Scissor dieser Op schon umgelenkt gesetzt */
        if ((ccm & 16) && cc_skip[i]) { continue; } /* W146 GPU-COST: ruhiges Ziel (Tick-Endstand kopiert) */
        if (ip_rp_prep && i > ip_rp_stop) { break; } /* W295: Vorbereitung nur bis zur letzten Welt-Op vor der Komposition */
        if (rpw && i <= rplast) { /* W295 WORLD-REPROJ: Welt-Ops bis zur Komposition durch die Reprojektion ersetzen */
            if (IP_RP_ISW(o, rpw, rpd)) {
                if (i == rplast) {
                    sb_rp_warp(x->c, bi, rpw, g_ish[ip_rp_we[bi]].sh, t);
                    crtv = (ID3D11RenderTargetView *)1; cdsv = (ID3D11DepthStencilView *)1; cbs = (ID3D11BlendState *)1;
                    cds = (ID3D11DepthStencilState *)1; csref = 0xFFFFFFFFu; crs = (ID3D11RasterizerState *)1;
                    csrv = (ID3D11ShaderResourceView *)1; csmp = (ID3D11SamplerState *)1; ctopo = 0xFFFFFFFFu;
                    memset(&cvp, 0xFF, sizeof cvp); memset(&csc, 0xFF, sizeof csc);
                    mode = -1; rq_dirty = 1; fxs.dirty = 1; cgd = NULL;
                    rcur[0] = rcur[1] = rcur[2] = 0xFFFFFFFFu;
                    if (!ring) { ID3D11DeviceContext_PSSetConstantBuffers(x->c, 0, 1, &x->cb0); }
                }
                continue;
            }
        }
        if (ip_pl_on && o->nolerp && (ip_pl_mfx & (o->nolerp == MFX_WASH ? 1u : 2u))) { ip_pl_nmfx++; continue; } /* W193: monitor_fx aus */
        if (ip_ph.on && ip_ph.hud_off && ip_ph_is_hud(o)) { ip_ph_nhud++; continue; } /* W290: HUD aus (nur Foto) */
        if (ip_ph.on && ip_ph.cine && o->kind == IP_DRAW) { /* W428 PHOTO-CINEMATIC (nur Foto-Wiedergabe) */
            if (ip_ph_is_self(B, i)) { ip_ph_nself++; continue; } /* eigener VT im Welt-RT (W445: mit Luecke) */
            if (o->nolerp) { ip_ph_ncmfx++; continue; } /* Monitor-Effekt (Korn-Wash/Stoerzeilen) */
            if (ip_ph_cine_ovl(o, B->bbref >= 0 && o->sh_rtv == g_ish[B->bbref].rtv)) { ip_ph_ncovl++; continue; } /* Einblendungen (Radar, FRONT VIEW, Marken, Texte) */
        }
        if (vtk && vt_skip[i]) { continue; } /* W183 VR-FIX3: RT-Op, Ziel hat den Tick-Endstand */
        if ((g_cfg.loc_interp_dbg & 1) && g_sba_loc && lerp_ok && !dbg && o->kind == IP_DRAW && o->cls && !o->flag) { continue; } /* DIAG(W349) */
        if ((ccm & 3) && i < (unsigned)cc_pfx) { /* W146 GPU-COST: BB-Anfang aus dem Cockpit-Cache */
            if ((ccm & 3) == 2 && (o->rtv == g_rtv || (cc_bdsv && o->dsv == cc_bdsv))) { continue; }
        } else if ((ccm & 3) == 1 && i == (unsigned)cc_pfx) {
            cc_store(A, B, x);
            ccm &= ~3;
        }
        if (g_gp_open && !x->deferred) { /* DIAG(GPU-COST,W146) perf_gpass: Kategorie je Op */
            int gc;
            if (o->kind == IP_DRAW && (o->has_tex & FXR_MASK)) { gc = GP_FX; }
            else if (o->kind == IP_DRAW && o->nolerp) { gc = GP_MFX; }
            else { gc = sb_gp_rt_class(o->sh_rtv, o->rtv == g_rtv, o->kind == IP_DRAW && o->rq); }
            sb_gp_mark(gc);
        }
        if (ip_gs_n > 0 && o->sh_rtv && o->sh_rtv != ip_gs_rtv[ip_gs_n - 1]) { ip_gs_mark(o->sh_rtv); } /* DIAG(TICK-PERF,W125) */
        if (ip_gs_n > 0 && o->kind == IP_DRAW) { ip_gs_draws[ip_gs_n - 1]++; if (o->rq) { ip_gs_rq[ip_gs_n - 1]++; } }
        if (o->kind == IP_COPY) { /* W340 LOC-FRAMES: BB-Schatten (Stand dieser Wiedergabe) -> Schatten der Textur */
            if (o->cp_es >= 0 && o->cp_ed >= 0 && g_ish[o->cp_es].sh && g_ish[o->cp_ed].sh) {
                ID3D11Resource *dr = (ID3D11Resource *)g_ish[o->cp_ed].sh, *sr = (ID3D11Resource *)g_ish[o->cp_es].sh;
                if (vrp) { /* W134 VR: BB-Draws gingen ins Augenziel -> Textur bleibt Tick-Stand */ }
                else {
                    if (o->cpbox.back) { ID3D11DeviceContext_CopySubresourceRegion(x->c, dr, 0, 0, 0, 0, sr, 0, &o->cpbox); }
                    else { ID3D11DeviceContext_CopyResource(x->c, dr, sr); }
                    InterlockedIncrement(&li_cp_rep);
                }
            }
            continue;
        }
        if (o->kind == IP_CLR_RTV) {
            if (ip_ph.on && ip_ph.tgt >= 0 && o->sh_rtv && o->sh_rtv == g_ish[ip_ph.tgt].rtv) { /* W195: Foto-Kachel */
                D3D11_RECT cr;
                cr.left = ip_ph.cl[0]; cr.top = ip_ph.cl[1]; cr.right = ip_ph.cl[2]; cr.bottom = ip_ph.cl[3];
                if (x->c1 && cr.right > cr.left && cr.bottom > cr.top) {
                    ID3D11DeviceContext1_ClearView(x->c1, (ID3D11View *)ip_ph.rtv, o->col, &cr, 1);
                } else {
                    ID3D11DeviceContext_ClearRenderTargetView(x->c, ip_ph.rtv, o->col);
                }
                ip_ph_nclr++;
                continue;
            }
            if (ip_ph.on && ip_ph.nsc && ip_ph_sc_rtv(o->sh_rtv) >= 0) { /* W195: skaliertes Zwischenziel */
                ID3D11DeviceContext_ClearRenderTargetView(x->c, ip_ph.sc_rtv[ip_ph_sc_rtv(o->sh_rtv)], o->col);
                ip_ph_nclr++;
                continue;
            }
            if (vrp && o->rtv == g_rtv) { /* W134 VR-PROTO: BB-Clear -> Augenziel */
                ID3D11DeviceContext_ClearRenderTargetView(x->c, vrp->rtv, o->col);
                vrp->nclr++;
                continue;
            }
            if (o->sh_rtv) {
                static const float kz[4] = { 0, 0, 0, 0 }; /* DIAG(W251) dbg=2: ID 0 = geloescht */
                WD_CALL("ip_clear_rtv", NULL, ID3D11DeviceContext_ClearRenderTargetView(x->c, o->sh_rtv, dbg == 2 ? kz : o->col));
            }
            rc.nclr++;
            continue;
        }
        if (o->kind == IP_CLR_DSV) {
            if (ip_ph.on && ip_ph.tgtd >= 0 && ip_ph.dsv && o->sh_dsv && o->sh_dsv == g_ish[ip_ph.tgtd].dsv) { /* W195 */
                ID3D11DeviceContext_ClearDepthStencilView(x->c, ip_ph.dsv, o->dsflags, o->z, (UINT8)o->stencil);
                ip_ph_nclr++;
                continue;
            }
            if (ip_ph.on && ip_ph.nsc && ip_ph_sc_dsv(o->sh_dsv) >= 0) { /* W195 */
                int j = ip_ph_sc_dsv(o->sh_dsv);
                if (ip_ph.sc_dsv[j]) {
                    ID3D11DeviceContext_ClearDepthStencilView(x->c, ip_ph.sc_dsv[j], o->dsflags, o->z, (UINT8)o->stencil);
                }
                ip_ph_nclr++;
                continue;
            }
            if (vrp && o->dsv == g_dsv) { /* W134 VR-PROTO: BB-Tiefe -> Augentiefe */
                if (vrp->dsv) {
                    ID3D11DeviceContext_ClearDepthStencilView(x->c, vrp->dsv, o->dsflags, o->z, (UINT8)o->stencil);
                }
                continue;
            }
            if (o->sh_dsv) {
                WD_CALL("ip_clear_dsv", NULL, ID3D11DeviceContext_ClearDepthStencilView(x->c, o->sh_dsv, o->dsflags, o->z,
                                                          (UINT8)o->stencil));
            }
            continue;
        }
        {
            ID3D11RenderTargetView *wr = o->sh_rtv; /* W195: Foto-Umlenkung (sonst die Op-Werte, unveraendert) */
            ID3D11DepthStencilView *wd = o->sh_dsv;
            if (ip_ph.on) {
                int red = 0;
                if (ip_ph.tgt >= 0 && wr && wr == g_ish[ip_ph.tgt].rtv) { wr = ip_ph.rtv; red = 1; }
                if (ip_ph.tgtd >= 0 && ip_ph.dsv && wd && wd == g_ish[ip_ph.tgtd].dsv && (red || !o->sh_rtv)) { wd = ip_ph.dsv; red = 1; }
                else if (red) { wd = NULL; } /* Umlenkung ohne passende Tiefe: keine (Groessen muessen passen) */
                if (red) {
                    D3D11_VIEWPORT v = o->vp;
                    D3D11_RECT s;
                    LONG l, t, r, bt;
                    v.TopLeftX = o->vp.TopLeftX * ip_ph.kx + ip_ph.ox;
                    v.TopLeftY = o->vp.TopLeftY * ip_ph.ky + ip_ph.oy;
                    v.Width = o->vp.Width * ip_ph.kx;
                    v.Height = o->vp.Height * ip_ph.ky;
                    l = (LONG)floorf((float)o->sc.left * ip_ph.kx + ip_ph.ox);
                    t = (LONG)floorf((float)o->sc.top * ip_ph.ky + ip_ph.oy);
                    r = (LONG)ceilf((float)o->sc.right * ip_ph.kx + ip_ph.ox);
                    bt = (LONG)ceilf((float)o->sc.bottom * ip_ph.ky + ip_ph.oy);
                    s.left = l < ip_ph.cl[0] ? ip_ph.cl[0] : (l > ip_ph.cl[2] ? ip_ph.cl[2] : l);
                    s.top = t < ip_ph.cl[1] ? ip_ph.cl[1] : (t > ip_ph.cl[3] ? ip_ph.cl[3] : t);
                    s.right = r < s.left ? s.left : (r > ip_ph.cl[2] ? ip_ph.cl[2] : r);
                    s.bottom = bt < s.top ? s.top : (bt > ip_ph.cl[3] ? ip_ph.cl[3] : bt);
                    ID3D11DeviceContext_RSSetViewports(x->c, 1, &v);
                    ID3D11DeviceContext_RSSetScissorRects(x->c, 1, &s);
                    memset(&cvp, 0xFF, sizeof cvp); /* naechster nicht umgelenkter Draw setzt neu */
                    memset(&csc, 0xFF, sizeof csc);
                    ip_ph_nred++;
                    phred = 1;
                } else if (ip_ph.nsc && (ip_ph_sc_rtv(wr) >= 0 || (!wr && ip_ph_sc_dsv(wd) >= 0))) {
                    int j = wr ? ip_ph_sc_rtv(wr) : ip_ph_sc_dsv(wd), jd = ip_ph_sc_dsv(wd);
                    float k = ip_ph.sc_k[j];
                    D3D11_VIEWPORT v = o->vp;
                    D3D11_RECT sr;
                    if (wr) { wr = ip_ph.sc_rtv[j]; }
                    wd = (jd >= 0 && (jd == j || !o->sh_rtv)) ? ip_ph.sc_dsv[jd] : NULL;
                    v.TopLeftX *= k; v.TopLeftY *= k; v.Width *= k; v.Height *= k;
                    sr.left = (LONG)floorf((float)o->sc.left * k); sr.top = (LONG)floorf((float)o->sc.top * k);
                    sr.right = (LONG)ceilf((float)o->sc.right * k); sr.bottom = (LONG)ceilf((float)o->sc.bottom * k);
                    if (sr.left < 0) { sr.left = 0; }
                    if (sr.top < 0) { sr.top = 0; }
                    if (sr.right > ip_ph.sc_w[j]) { sr.right = ip_ph.sc_w[j]; }
                    if (sr.bottom > ip_ph.sc_h[j]) { sr.bottom = ip_ph.sc_h[j]; }
                    if (sr.right < sr.left) { sr.right = sr.left; }
                    if (sr.bottom < sr.top) { sr.bottom = sr.top; }
                    ID3D11DeviceContext_RSSetViewports(x->c, 1, &v);
                    ID3D11DeviceContext_RSSetScissorRects(x->c, 1, &sr);
                    memset(&cvp, 0xFF, sizeof cvp);
                    memset(&csc, 0xFF, sizeof csc);
                    ip_ph_nred++;
                    phred = 1;
                }
            }
            if (wr != crtv || wd != cdsv) {
                ID3D11DeviceContext_OMSetRenderTargets(x->c, 1, &wr, wd);
                crtv = wr;
                cdsv = wd;
                rc.nrtv++;
            }
        }
        if (o->bs != cbs) {
            /* Blendfaktor/Maske setzt der IM-Pfad konstant (NULL/0xFFFFFFFF). */
            ID3D11DeviceContext_OMSetBlendState(x->c, o->bs, o->bf, o->smask);
            cbs = o->bs;
        }
        if (o->ds != cds || o->sref != csref) {
            ID3D11DeviceContext_OMSetDepthStencilState(x->c, o->ds, o->sref);
            cds = o->ds;
            csref = o->sref;
        }
        if (o->rs != crs) {
            ID3D11DeviceContext_RSSetState(x->c, o->rs);
            crs = o->rs;
        }
        if (!phred && memcmp(&o->vp, &cvp, sizeof cvp) != 0) {
            if (var == 1u) { /* DIAG(PERF-M2,W174) ip_ablate 1: Viewport 1x1 (keine Pixelarbeit) */
                D3D11_VIEWPORT v1 = o->vp;
                v1.Width = 1.0f; v1.Height = 1.0f;
                ID3D11DeviceContext_RSSetViewports(x->c, 1, &v1);
            } else {
                ID3D11DeviceContext_RSSetViewports(x->c, 1, &o->vp);
            }
            cvp = o->vp;
        }
        if (!phred && memcmp(&o->sc, &csc, sizeof csc) != 0) {
            ID3D11DeviceContext_RSSetScissorRects(x->c, 1, &o->sc);
            csc = o->sc;
        }
        if (IP_PL_SRV(o->sh_srv) != csrv) { /* W193: Textur-Tausch nur in der Pause-Neuzeichnung */
            ID3D11ShaderResourceView *ss = IP_PL_SRV(o->sh_srv);
            ID3D11DeviceContext_PSSetShaderResources(x->c, 0, 1, &ss);
            csrv = ss;
            rc.nsrv++;
        }
        if (o->topo != ctopo) {
            ID3D11DeviceContext_IASetPrimitiveTopology(x->c, (D3D11_PRIMITIVE_TOPOLOGY)o->topo);
            ctopo = o->topo;
        }
        if (ring) { ip_ring_bind(x, rofs[2u * i], 0, rcur, &rc); } /* PERF(PERF-M2,W174) */
        else if (ip_ph.on && (ip_ph.cam || (ip_ph.wcx > 0.0f && ip_ph.wcx != 1.0f)) && ip_ph_is_world(o)) {
            /* W195 PHOTO-MODE: freie Kamera bzw. Weitwinkel (Welt-RT) */
            float M[16], pm[16];
            if (ip_ph.cam && ip_ph_cam_matrix(o, M)) { ip_m4mul(pm, o->mvp, M); ip_ph_ncam++; }
            else { memcpy(pm, o->mvp, sizeof pm); }
            if (ip_ph.wcx > 0.0f && ip_ph.wcx != 1.0f) { /* Clip x * wcx (Zeilenvektor: Spalte 0 skalieren) */
                pm[0] *= ip_ph.wcx; pm[4] *= ip_ph.wcx; pm[8] *= ip_ph.wcx; pm[12] *= ip_ph.wcx;
            }
            ip_cb_write(x, x->cb0, pm, o->has_tex, 0.0f, 0);
            rc.nmap++;
        }
        else if (ip_pl_on && ip_pl_mvp && o->rtv == g_rtv) {
            ip_cb_write(x, x->cb0, ip_pl_mvp[i], o->has_tex, 0.0f, 0);
            rc.nmap++;
        }
        else if (var != 4u) { ip_cb_write(x, x->cb0, IP_LAM(o, o->mvp), o->has_tex, 0.0f, 0); rc.nmap++; } /* W376 */
        if (dbg == 2 && ip_id_ps && ip_dbg_cb) {
            /* DIAG(W251 INTERP-GROUND): Draw-ID-Bild - R/G = Op-Index+1, B = 255 gelerpt / 64 exakt;
             * Mischen aus (ausser reinen Tiefen-/Stencil-Draws ohne Farbschreiben) */
            D3D11_MAPPED_SUBRESOURCE dm;
            D3D11_BLEND_DESC bdd;
            int il = lerp_ok && o->flag && o->match >= 0 && A && (unsigned)o->match < A->nops;
            unsigned id = i + 1u;
            memset(&bdd, 0, sizeof bdd);
            bdd.RenderTarget[0].RenderTargetWriteMask = 15;
            if (o->bs) { ID3D11BlendState_GetDesc(o->bs, &bdd); }
            if (bdd.RenderTarget[0].RenderTargetWriteMask) {
                ID3D11DeviceContext_OMSetBlendState(x->c, NULL, NULL, 0xFFFFFFFFu);
                cbs = (ID3D11BlendState *)1;
            }
            if (cps != 2) {
                ID3D11DeviceContext_PSSetShader(x->c, ip_id_ps, NULL, 0);
                ID3D11DeviceContext_PSSetConstantBuffers(x->c, 1, 1, &ip_dbg_cb);
                cps = 2;
            }
            if (SUCCEEDED(ID3D11DeviceContext_Map(x->c, (ID3D11Resource *)ip_dbg_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &dm))) {
                float *f = (float *)dm.pData;
                f[0] = (float)(id & 255u) / 255.0f; f[1] = (float)((id >> 8) & 255u) / 255.0f;
                f[2] = il ? 1.0f : 64.0f / 255.0f; f[3] = 1;
                ID3D11DeviceContext_Unmap(x->c, (ID3D11Resource *)ip_dbg_cb, 0);
            }
        } else if (dbg && ip_dbg_ps && ip_dbg_cb) {
            /* gefaerbt nur Draws, die NICHT interpoliert/unveraendert sind */
            int want = (o->why != IP_W_BB2D && o->why != IP_W_LERP && o->why != IP_W_STATIC &&
                        o->why != IP_W_RT2DLERP) ? 1 : 0;
            if (want != cps) {
                ID3D11DeviceContext_PSSetShader(x->c, want ? ip_dbg_ps : g_im_ps, NULL, 0);
                if (want) { ID3D11DeviceContext_PSSetConstantBuffers(x->c, 1, 1, &ip_dbg_cb); }
                cps = want;
            }
            if (want && o->why != cwhy) {
                D3D11_MAPPED_SUBRESOURCE dm;
                if (SUCCEEDED(ID3D11DeviceContext_Map(x->c, (ID3D11Resource *)ip_dbg_cb, 0,
                                                      D3D11_MAP_WRITE_DISCARD, 0, &dm))) {
                    float *f = (float *)dm.pData;
                    unsigned w = o->why < IP_W_N ? o->why : 0u;
                    f[0] = kIP_DBG_COL[w][0]; f[1] = kIP_DBG_COL[w][1]; f[2] = kIP_DBG_COL[w][2]; f[3] = 1;
                    ID3D11DeviceContext_Unmap(x->c, (ID3D11Resource *)ip_dbg_cb, 0);
                }
                cwhy = o->why;
            }
        }
        lerp = lerp_ok && o->flag && o->match >= 0 && (unsigned)o->match < A->nops;
        rc.ndraw++; /* DIAG(PERF-M2,W174) */
        if (lerp) { rc.nlerp++; }
        rc.vbytes += (unsigned long long)o->nv * sizeof(im_vertex_t) * (lerp ? 2u : 1u);
        if (o->rq) { rc.nrq++; }
        vrbb = vrp && o->rtv == g_rtv; /* W134 VR-PROTO: Draw in den Backbuffer -> je Auge */
        vrkind = !vrbb ? -1 : (!o->cls ? 0 : (ip_vr_isp2(o->mvp) ? 2 : 1));
        if (vrbb) {
            if (vrkind == 0) { vrp->n2d++; } else if (vrkind == 1) { vrp->n3d++; } else { vrp->np2++; }
            if (vrkind == 2) {
                int q;
                for (q = 0; q < 4; q++) {
                    if (o->rn[q][2] > 0.0f) {
                        float zn = o->pref[q][2] / o->pref[q][3];
                        vrp->p2w_sum += o->rn[q][2];
                        vrp->p2w_n++;
                        vrp->p2z_sum += zn;
                        if (vrp->p2w_n == 1u || zn < vrp->p2z_min) { vrp->p2z_min = zn; }
                        if (vrp->p2w_n == 1u || zn > vrp->p2z_max) { vrp->p2z_max = zn; }
                        if (vrp->wantz && vrp->zs_n < 1024u) { vrp->zs[vrp->zs_n++] = zn; }
                    }
                }
            }
            if (vrkind == 1) { int q; for (q = 0; q < 4; q++) { if (o->rn[q][2] > 0.0f && (vrp->w3_min <= 0.0f || o->rn[q][2] < vrp->w3_min)) { vrp->w3_min = o->rn[q][2]; }
                                                       if (o->rn[q][2] > 0.0f && vrp->wantz && vrp->ws_n < 512u) { vrp->ws[vrp->ws_n++] = o->rn[q][2]; } } } /* W164 */
            if (o->rq) { vrp->nrq++; if (vrkind != 1 || !x->c1 || vrp->noshade) { vrp->nrqfb++; } }
            if (vrp->dump > 0) { /* DIAG(VR-PROTO,W134): Eingangsdaten der BB-Draws */
                vrp->dump--;
                HLOG_I("VR op i=%u kind=%d n=%u rq=%d tex=0x%X lerp=%d vpz=[%.3f,%.3f] ds=%p mvp=[%.3f %.3f %.3f %.3f|%.3f %.3f %.3f %.3f|%.3f %.3f %.3f %.3f|%.3f %.3f %.3f %.3f] "
                       "p0=(%.3f,%.3f,%.4f,%.3f) p1=(%.3f,%.3f,%.4f,%.3f) p3=(%.3f,%.3f,%.4f,%.3f)",
                       i, vrkind, o->nv, o->rq, o->has_tex, lerp, o->vp.MinDepth, o->vp.MaxDepth, (void *)o->ds,
                       o->mvp[0], o->mvp[1], o->mvp[2], o->mvp[3], o->mvp[4], o->mvp[5], o->mvp[6], o->mvp[7],
                       o->mvp[8], o->mvp[9], o->mvp[10], o->mvp[11], o->mvp[12], o->mvp[13], o->mvp[14], o->mvp[15],
                       o->pref[0][0], o->pref[0][1], o->pref[0][2], o->pref[0][3], o->pref[1][0], o->pref[1][1], o->pref[1][2], o->pref[1][3],
                       o->pref[3][0], o->pref[3][1], o->pref[3][2], o->pref[3][3]);
                { /* DIAG(VR-FIX2,W164): Tiefenzustand des Draws (Relief-Richtung, Decals) */
                    D3D11_DEPTH_STENCIL_DESC dd;
                    memset(&dd, 0, sizeof dd);
                    if (o->ds) { ID3D11DepthStencilState_GetDesc(o->ds, &dd); }
                    HLOG_I("VR op2 i=%u kind=%d tiefe=%d schreiben=%d func=%d dsv=%d zn=[%.5f %.5f %.5f %.5f] ndc0=(%.3f,%.3f) ndc3=(%.3f,%.3f) w=[%.3f %.3f %.3f %.3f]",
                           i, vrkind, (int)dd.DepthEnable, (int)dd.DepthWriteMask, (int)dd.DepthFunc, o->dsv ? 1 : 0,
                           o->pref[0][3] != 0.0f ? o->pref[0][2] / o->pref[0][3] : 0.0f, o->pref[1][3] != 0.0f ? o->pref[1][2] / o->pref[1][3] : 0.0f,
                           o->pref[2][3] != 0.0f ? o->pref[2][2] / o->pref[2][3] : 0.0f, o->pref[3][3] != 0.0f ? o->pref[3][2] / o->pref[3][3] : 0.0f,
                           o->rn[0][0], o->rn[0][1], o->rn[3][0], o->rn[3][1], o->rn[0][2], o->rn[1][2], o->rn[2][2], o->rn[3][2]);
                }
            }
        }
        if (o->rq && ip_pl_on && ip_pl_noshade) { ip_pl_nnoshade++; } /* W193: shading aus -> IM-PS-Ersatz */
        if (o->rq && x->c1 && !(ip_pl_on && ip_pl_noshade) && !(vrbb && (vrkind != 1 || vrp->noshade))) { /* W183: vr_shading=0 -> IM-PS-Ersatz */
            /* RENDER-QA (W77): Shading-Draw mit seinem Zustand (Stream 0 per
             * Versatz, Stream 2, Konstanten-Fenster, Stufen 0..3, Sampler). */
            const rq_rec_t *r = &o->rqr;
            ID3D11ShaderResourceView *sv[4];
            ID3D11Buffer *rcb = (ip_pl_on && ip_pl_cb && r->cbuf == ip_pl_cbsrc) ? ip_pl_cb : r->cbuf; /* W193 */
            UINT st2 = stride, o0 = o->vstart * stride;
            sv[0] = IP_PL_SRV(o->sh_srv); sv[1] = IP_PL_SRV(o->rq_shsrv[1]); sv[2] = IP_PL_SRV(o->rq_shsrv[2]); sv[3] = IP_PL_SRV(o->rq_shsrv[3]);
            ID3D11DeviceContext_PSSetShader(x->c, (dbg == 2 && ip_id_ps) ? ip_id_ps : r->ps, NULL, 0); /* DIAG(W251) */
            ID3D11DeviceContext1_VSSetConstantBuffers1(x->c1, 3, 1, &rcb, &r->vfirst, &r->vnum);
            ID3D11DeviceContext1_PSSetConstantBuffers1(x->c1, 4, 1, &rcb, &r->pfirst, &r->pnum);
            ID3D11DeviceContext_PSSetShaderResources(x->c, 0, 4, sv);
            ID3D11DeviceContext_PSSetSamplers(x->c, 0, 4, (ID3D11SamplerState *const *)r->smp);
            { /* PERF(PERF-M2,W174) ip_vb_gpu Bit 2: Stream 2/Roh-Eingaenge aus der GPU-Kopie (gleicher Versatz) */
                ID3D11ShaderResourceView *xs = (ID3D11ShaderResourceView *)r->xsrv;
                ID3D11Buffer *xb = (ID3D11Buffer *)r->xbuf;
                if (vbgB && (B->gpu_ok & 2u)) {
                    if (xs && xs == B->rqxsrv && B->rqxgsrv) { xs = B->rqxgsrv; }
                    if (xb && xb == B->rqx && B->rqxg) { xb = B->rqxg; }
                }
                if (xs) { ID3D11DeviceContext_VSSetShaderResources(x->c, 0, 1, &xs); } /* SHADE-PERF (W80) */
                else { ID3D11DeviceContext_IASetVertexBuffers(x->c, 2, 1, &xb, &r->xstride, &r->xoff); }
            }
            if (lerp) {
                const ip_op_t *a = &A->ops[o->match];
                ID3D11Buffer *cbs2[2];
                UINT o1 = (o->rzb ? o->vstart : a->vstart) * stride; /* W284 rzb: Strom N-1 = Tick-N-Vertices */
                if (ring) { ip_ring_bind(x, rofs[2u * i + 1u], 1, rcur, &rc); }
                else if (var != 4u) { ip_cb_write(x, x->cb1, IP_LAM(o, IP_AMVP(o, a)), 0, t, 1); rc.nmap++; } /* W376 */
                ID3D11DeviceContext_IASetInputLayout(x->c, r->layl);
                ID3D11DeviceContext_VSSetShader(x->c, r->vsl, NULL, 0);
                cbs2[0] = x->cb0; cbs2[1] = x->cb1;
                if (!ring) { ID3D11DeviceContext_VSSetConstantBuffers(x->c, 0, 2, cbs2); }
                ID3D11DeviceContext_IASetVertexBuffers(x->c, 0, 1, &bvb, &st2, &o0);
                ID3D11DeviceContext_IASetVertexBuffers(x->c, 1, 1, o->rzb ? &bvb : &avb, &st2, &o1);
                ip_hw_bind(x->c, B, o->rzb ? B : A, o->vstart, o->rzb ? o->vstart : a->vstart); /* FIX(W237) interp_hlerp */
                if (gdon) { ip_gd_bind(x->c, &cgd, ip_gd_need(o, a) ? ip_gd_sh : NULL); } /* FIX(W265) */
            } else {
                if (cgd) { ip_gd_bind(x->c, &cgd, NULL); } /* W265 */
                ID3D11DeviceContext_IASetInputLayout(x->c, r->lay);
                ID3D11DeviceContext_VSSetShader(x->c, r->vs, NULL, 0);
                if (!ring) { ID3D11DeviceContext_VSSetConstantBuffers(x->c, 0, 1, &x->cb0); }
                ID3D11DeviceContext_IASetVertexBuffers(x->c, 0, 1, &bvb, &st2, &o0);
            }
            if (vrbb) { /* W134 VR-PROTO: Shading-Draw 3D-Pfad je Auge (lineare Augen-Matrix in b0/b1) */
                int e;
                for (e = 0; e < vrp->neyes && !(g_cfg.vr_diag_skip & 2); e++) { /* DIAG(W164): vr_diag_skip Bit 1 = 3D */
                    float m[16];
                    ip_vr_eye_target(x, vrp, e, o);
                    ip_vr_mat3d(m, o->mvp, vrp, e);
                    ip_cb_write(x, x->cb0, m, o->has_tex, 0.0f, 0);
                    if (lerp) { ip_vr_mat3d(m, A->ops[o->match].mvp, vrp, e); ip_cb_write(x, x->cb1, m, 0, t, 1); }
                    ip_draw_op(x->c, o, o->nv, 0);
                }
                crtv = (ID3D11RenderTargetView *)1; cdsv = (ID3D11DepthStencilView *)1;
                memset(&cvp, 0xFF, sizeof cvp); memset(&csc, 0xFF, sizeof csc);
            } else {
                if (var != 8u) { WD_CALL("ip_draw_rq", x->c, ip_draw_op(x->c, o, var == 2u && o->nv > 3u ? 3u : o->nv, 0)); } /* DIAG(PERF-WARM,W139); W174 ip_ablate */
            }
            mode = -1;      /* naechster alter Draw bindet VS/Layout/Streams neu */
            rq_dirty = 1;   /* ... und PS/Sampler/Stufen */
            fxs.bound = 0; fxs.dirty = 1; /* W133: PS ersetzt, Tiefe evtl. geschrieben */
            csmp = (ID3D11SamplerState *)1;
            csrv = (ID3D11ShaderResourceView *)1;
            continue;
        }
        if (rq_dirty) {
            ID3D11ShaderResourceView *nul[3] = { NULL, NULL, NULL };
            ID3D11SamplerState *ds = rq_default_sampler();
            ID3D11DeviceContext_PSSetShader(x->c, (dbg && cps == 2) ? ip_id_ps : ((dbg && cps == 1) ? ip_dbg_ps : g_im_ps), NULL, 0);
            if (!ring) { ID3D11DeviceContext_PSSetConstantBuffers(x->c, 0, 1, &x->cb0); }
            ID3D11DeviceContext_PSSetShaderResources(x->c, 1, 3, nul);
            if (ds) { ID3D11DeviceContext_PSSetSamplers(x->c, 0, 1, &ds); }
            {
                ID3D11ShaderResourceView *ss = IP_PL_SRV(o->sh_srv); /* W193 */
                ID3D11DeviceContext_PSSetShaderResources(x->c, 0, 1, &ss);
                csrv = ss;
            }
            rq_dirty = 0;
            csmp = (ID3D11SamplerState *)1;
        }
        if (!dbg && ((o->has_tex & FXR_MASK) || fxs.bound)) { /* W133 REM-FX: Effekt-PS + Tiefen-Kopie des Schatten-Ziels */
            fxr_rep_op(x->c, &fxs, o->has_tex, o->fxp, o->sh_dsv, x->deferred);
        } else if (o->kind == IP_DRAW) { fxs.dirty = 1; }
        if (g_cfg.tex_addr || o->ta_smp || /* FIX(TEXADDR,W84): s0 wie beim Spiel-Draw; MONITOR-FX (W105): Wash-WRAP auch bei tex_addr=0 */
            (csmp != (ID3D11SamplerState *)1 && csmp != sb_ta_sampler(0, 0))) {
            ID3D11SamplerState *ws = o->ta_smp ? o->ta_smp : sb_ta_sampler(0, 0);
            if (ws && ws != csmp) { ID3D11DeviceContext_PSSetSamplers(x->c, 0, 1, &ws); csmp = ws; }
        }
        if (lerp) {
            const ip_op_t *a = &A->ops[o->match];
            ID3D11Buffer *vbs[2];
            UINT strides[2], offs[2];
            if (ring) { ip_ring_bind(x, rofs[2u * i + 1u], 1, rcur, &rc); } /* PERF(PERF-M2,W174) */
            else if (var != 4u) { ip_cb_write(x, x->cb1, IP_LAM(o, IP_AMVP(o, a)), 0, t, 1); rc.nmap++; } /* W376 */
            if (mode != 1) {
                ID3D11Buffer *cbs2[2];
                ID3D11DeviceContext_IASetInputLayout(x->c, ip_lay2);
                ID3D11DeviceContext_VSSetShader(x->c, ip_vs2, NULL, 0);
                cbs2[0] = x->cb0;
                cbs2[1] = x->cb1;
                if (!ring) { ID3D11DeviceContext_VSSetConstantBuffers(x->c, 0, 2, cbs2); }
                mode = 1;
            }
            vbs[0] = bvb; vbs[1] = o->rzb ? bvb : avb; /* W284 rzb */
            strides[0] = stride; strides[1] = stride;
            offs[0] = o->vstart * stride; offs[1] = (o->rzb ? o->vstart : a->vstart) * stride;
            ID3D11DeviceContext_IASetVertexBuffers(x->c, 0, 2, vbs, strides, offs);
            ip_hw_bind(x->c, B, o->rzb ? B : A, o->vstart, o->rzb ? o->vstart : a->vstart); /* FIX(W237) interp_hlerp */
            if (gdon) { ip_gd_bind(x->c, &cgd, ip_gd_need(o, a) ? ip_gd_im : NULL); } /* FIX(W265); VR: Lagen im Augenraum */
            if (vrbb) { /* W134 VR-PROTO */
                ip_vr_draw(x, vrp, o, a, vrkind, t, 1, &mode);
                crtv = (ID3D11RenderTargetView *)1; cdsv = (ID3D11DepthStencilView *)1;
                memset(&cvp, 0xFF, sizeof cvp); memset(&csc, 0xFF, sizeof csc);
            } else {
                if (var != 8u) { WD_CALL("ip_draw_lerp", x->c, ip_draw_op(x->c, o, var == 2u && o->nv > 3u ? 3u : o->nv, 0)); } /* W174 ip_ablate */
            }
        } else {
            if (cgd) { ip_gd_bind(x->c, &cgd, NULL); } /* W265 */
            if (mode != 0) {
                ID3D11Buffer *nb = NULL;
                ID3D11DeviceContext_IASetInputLayout(x->c, g_im_layout);
                ID3D11DeviceContext_VSSetShader(x->c, g_im_vs, NULL, 0);
                if (!ring) { ID3D11DeviceContext_VSSetConstantBuffers(x->c, 0, 1, &x->cb0); }
                ID3D11DeviceContext_IASetVertexBuffers(x->c, 0, 1, &bvb, &stride, &zero);
                ID3D11DeviceContext_IASetVertexBuffers(x->c, 1, 1, &nb, &stride, &zero);
                mode = 0;
            }
            if (vrbb) { /* W134 VR-PROTO */
                ip_vr_draw(x, vrp, o, NULL, vrkind, t, 0, &mode);
                crtv = (ID3D11RenderTargetView *)1; cdsv = (ID3D11DepthStencilView *)1;
                memset(&cvp, 0xFF, sizeof cvp); memset(&csc, 0xFF, sizeof csc);
            } else {
                if (var != 8u) { WD_CALL("ip_draw", x->c, ip_draw_op(x->c, o, var == 2u && o->nv > 3u ? 3u : o->nv, o->vstart)); } /* W174 ip_ablate */
            }
        }
    }
    if ((ccm & 3) == 1) { cc_store(A, B, x); } /* W146: BB-Anfang = alle BB-Ops */
    if (cgd) { ip_gd_bind(x->c, &cgd, NULL); } /* W265: Spiel-Draws nie mit GS */
    if (gdq >= 0) { ID3D11DeviceContext_End(x->c, (ID3D11Asynchronous *)ip_gd_q[gdq]); ip_gd_qbusy[gdq] = (int)ip_gd_qi; } /* DIAG(W265) */
    if (cps >= 0) {
        ID3D11Buffer *nb = NULL;
        ID3D11DeviceContext_PSSetConstantBuffers(x->c, 1, 1, &nb);
    }
    fxr_rep_end(x->c, &fxs); /* W133 REM-FX */
    ip_rc_ctx[x->deferred ? 1 : 0] = rc; /* DIAG(PERF-M2,W174) */
    if (!x->deferred) {
        ip_state_restore(&sv);
        ip_gs_end(B->nops); /* DIAG(TICK-PERF,W125) */
    }
}
