/* render_fx.c - FX-ORIG (W90): Effekt-Draws (Mündungsfeuer, Rauch, Funken,
 * Explosionen) und die NV2A-RenderStates, die der Renderer bisher nicht
 * spiegelte (Alpha-Test, Tiefenfunktion, Blend-Gleichung/-Farbe, Punkt-
 * Zustand). Spiegel immer (billig), Diagnose hinter fx_diag (Standard 0).
 *
 * NV097-Methoden (Simple-Pfad, ECX = Count<<18 | Methode, W33/XBLEND):
 * 0x300 ALPHA_TEST_ENABLE, 0x304 BLEND_ENABLE, 0x30C DEPTH_TEST_ENABLE,
 * 0x318 POINT_PARAMS_ENABLE, 0x31C POINT_SMOOTH_ENABLE (Point-Sprite),
 * 0x338 POLY_OFFSET_FILL_ENABLE, 0x33C ALPHA_FUNC (GL 0x200..0x207),
 * 0x340 ALPHA_REF (0..255), 0x344/0x348 BLEND_FUNC_S/D, 0x34C BLEND_COLOR,
 * 0x350 BLEND_EQUATION (GL 0x8006 ADD, 0x800A SUB, 0x800B REVSUB, 0x8007 MIN,
 * 0x8008 MAX), 0x354 DEPTH_FUNC (GL), 0x358 COLOR_MASK, 0x35C DEPTH_MASK,
 * 0x388/0x38C POLYGON_OFFSET_SCALE/BIAS, 0x43C POINT_SIZE. */
#include "render_int.h"
#include "sb_profile.h" /* W229 SBA() */
#include "render_qa.h"
#include "sb_state.h" /* W232: sb_in_mission */
void fx_ps_diag(unsigned *c0, unsigned *def0, unsigned *src);

float g_fx_raw[4][4], g_fx_mat[4][4]; /* DIAG: Rohpositionen/VSH-Matrix des aktuellen Draws (render_im.c) */
unsigned g_fx_raw_n, g_fx_slotmask; int g_fx_vb;
float g_fx_s2[4], g_fx_s3[4], g_fx_s9[4];
int g_fx_vsh_em, g_fx_vsh_kind;
unsigned g_fx_nv[0x800]; /* Spiegel NV-Methode m -> g_fx_nv[m >> 2] (0..0x1FFC) */
static unsigned g_fx_nv_set[0x800 / 32];

void fx_nv_note(unsigned method, unsigned value)
{
    unsigned m = method & 0x1FFCu, k = m >> 2;
    int first = !(g_fx_nv_set[k >> 5] & (1u << (k & 31u)));
    unsigned old = g_fx_nv[k];
    g_fx_nv[k] = value;
    g_fx_nv_set[k >> 5] |= 1u << (k & 31u);
    if (g_cfg.fx_diag && (first || old != value)) { /* DIAG(FX-ORIG,W90) */
        static unsigned seen[512], nseen, nlog;
        unsigned key = (m << 16) ^ (value * 2654435761u), i;
        for (i = 0; i < nseen; i++) { if (seen[i] == key) { break; } }
        if (i >= nseen && nseen < 512u) {
            seen[nseen++] = key;
            if (nlog < 400u) {
                nlog++;
                HLOG_W("FXNV f=%u m=0x%03X v=0x%X (neu)", g_present_no, m, value);
            }
        }
    }
}

int fx_nv_has(unsigned m) { unsigned k = (m & 0x1FFCu) >> 2; return (g_fx_nv_set[k >> 5] >> (k & 31u)) & 1u; }
unsigned fx_nv(unsigned m, unsigned def) { return fx_nv_has(m) ? g_fx_nv[(m & 0x1FFCu) >> 2] : def; }

/* ---------- DIAG(FX-ORIG,W90): Effekt-Zensus je Zustands-Schlüssel ---------- */
#define FX_KEYS 256u
typedef struct {
    unsigned key, vs, ps, xp, prim, rhw, tgt;
    unsigned bon, bs, bd, beq, ate, af, ar, zf, dm, zen, cm;
    unsigned n, draws, pix, pixq, last_log_draws;
    float amin, amax, cmax;
} fx_ent_t;
static fx_ent_t g_fx[FX_KEYS];
static unsigned g_fx_n;
static ID3D11Query *g_fx_q[8];
static int g_fx_qk[8], g_fx_qbusy[8];
static int g_fx_qcur = -1;

static int fx_in_mission(void)
{
    unsigned s = 0, m = 0;
    __try {
        s = *(volatile unsigned *)(uintptr_t)M3D_SCENE_ADDR;
        m = *(volatile unsigned *)(uintptr_t)M3D_MLOOP_ADDR;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return s == 0x2Cu && m == 1u;
}

static void fx_poll(void)
{
    int i;
    for (i = 0; i < 8; i++) {
        UINT64 v = 0;
        if (!g_fx_qbusy[i] || !g_fx_q[i]) { continue; }
        if (ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)g_fx_q[i], &v, sizeof v,
                                        D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) {
            fx_ent_t *e = &g_fx[g_fx_qk[i]];
            e->pix += (unsigned)v; e->pixq++;
            g_fx_qbusy[i] = 0;
        }
    }
}

/* Vor dem Draw (Zustand final, out[] = expandierte Vertices). Rückgabe 1 =
 * Occlusion-Query offen (fx_draw_post schließt sie). */
int fx_draw_pre(const im_vertex_t *out, unsigned nout, int rhw, unsigned prim, const float *mvp)
{
    float bx0 = 1e30f, bx1 = -1e30f, by0 = 1e30f, by1 = -1e30f; unsigned nbeh = 0;
    unsigned bon = 0, bs = 0, bd = 0, key, i, ate, af, ar, zf, beq, tgt;
    float amin = 1e30f, amax = -1e30f, cmax = 0.0f;
    fx_ent_t *e = NULL;
    int all = (g_cfg.fx_diag >= 2);
    if (!g_cfg.fx_diag || !fx_in_mission()) { return 0; }
    /* DIAG(W300 M12-FRAME): Linien/Punkte je Spiel-Caller (IM: Ruecksprung aus
     * D3DDevice_End, VB: DrawVertices-Caller) - erster Draw und Draw 10/100/1000/
     * 10000 je Caller+Prim mit Farbe, Ziel und NDC-Ausdehnung (fx_diag=1; 3 = alle Primitive). */
    if (prim <= 4u || g_cfg.fx_diag >= 3) { /* 3 = alle Primitive */
        extern unsigned g_im_end_caller, dd_caller;
        static unsigned lc_key[512], lc_n[512], lc_cnt;
        unsigned c = g_submit_vb ? dd_caller : g_im_end_caller, lk = c ^ (prim << 28) ^ ((unsigned)g_submit_vb << 27), li;
        for (li = 0; li < lc_cnt; li++) { if (lc_key[li] == lk) { break; } }
        if (li == lc_cnt && lc_cnt < 512u) { lc_key[lc_cnt++] = lk; }
        if (li < 512u) {
            unsigned q = ++lc_n[li];
            if (q == 1u || q == 10u || q == 100u || q == 1000u || q == 10000u) {
                float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f; unsigned j, beh = 0;
                for (j = 0; j < nout; j++) {
                    const float *pp = out[j].pos;
                    float X = pp[0]*mvp[0]+pp[1]*mvp[4]+pp[2]*mvp[8]+pp[3]*mvp[12];
                    float Y = pp[0]*mvp[1]+pp[1]*mvp[5]+pp[2]*mvp[9]+pp[3]*mvp[13];
                    float W = pp[0]*mvp[3]+pp[1]*mvp[7]+pp[2]*mvp[11]+pp[3]*mvp[15];
                    if (W > 1e-6f) { X /= W; Y /= W; if (X < x0) { x0 = X; } if (X > x1) { x1 = X; } if (Y < y0) { y0 = Y; } if (Y > y1) { y1 = Y; } }
                    else { beh++; }
                }
                if (x1 < x0) { x0 = x1 = y0 = y1 = 0.0f; }
                HLOG_W("FXLINE f=%u caller=0x%X %s p=%u n=%u #%u rhw=%d vs=0x%X xp=0x%X tgt=%d col0=(%.2f,%.2f,%.2f,%.2f) ndc=[%.2f,%.2f]x[%.2f,%.2f] beh=%u",
                       g_present_no, c, g_submit_vb ? "VB" : "IM", prim, nout, q, rhw, g_im_shader, sb_tex_xptr(),
                       rtt_active() ? g_rtt_bound : -1, nout ? out[0].col[0] : 0.0f, nout ? out[0].col[1] : 0.0f,
                       nout ? out[0].col[2] : 0.0f, nout ? out[0].col[3] : 0.0f, x0, x1, y0, y1, beh);
            }
        }
    }
    fx_poll();
    sb_render_blend_state(&bon, &bs, &bd);
    ate = fx_nv(0x300u, 0); af = fx_nv(0x33Cu, 0x207u); ar = fx_nv(0x340u, 0);
    zf = fx_nv(0x354u, 0x203u); beq = fx_nv(0x350u, 0x8006u);
    /* Effekt-Kandidat: geblendet ohne Tiefenschreiben, additiv, Alpha-Test
     * oder Punkte/Linien; fx_diag=2 = alle Draws. */
    if (!all && !((bon && (bd == 2u || bs == 2u || !rs_dmask)) || ate || prim <= 4u)) { return 0; }
    tgt = rtt_active() ? (unsigned)g_rtt_bound : 0xFFu;
    key = g_im_shader * 16777619u ^ g_im_pshader * 40503u /* Textur nicht im Schluessel (animierte Effekte) */
        ^ (prim << 24) ^ (bs << 4) ^ (bd << 8) ^ (bon << 12) ^ (ate << 13) ^ (rs_dmask << 14)
        ^ (tgt << 16) ^ (af * 7u) ^ (zf * 13u) ^ ((unsigned)rhw << 15) ^ (beq * 31u);
    for (i = 0; i < g_fx_n; i++) { if (g_fx[i].key == key) { e = &g_fx[i]; break; } }
    for (i = 0; i < nout; i++) {
        float a = out[i].col[3], c = out[i].col[0];
        if (out[i].col[1] > c) { c = out[i].col[1]; }
        if (out[i].col[2] > c) { c = out[i].col[2]; }
        if (a < amin) { amin = a; }
        if (a > amax) { amax = a; }
        if (c > cmax) { cmax = c; }
        { const float *p = out[i].pos;
          float X = p[0]*mvp[0]+p[1]*mvp[4]+p[2]*mvp[8]+p[3]*mvp[12];
          float Y = p[0]*mvp[1]+p[1]*mvp[5]+p[2]*mvp[9]+p[3]*mvp[13];
          float W = p[0]*mvp[3]+p[1]*mvp[7]+p[2]*mvp[11]+p[3]*mvp[15];
          if (W > 1e-6f) { X /= W; Y /= W;
              if (X < bx0) { bx0 = X; } if (X > bx1) { bx1 = X; }
              if (Y < by0) { by0 = Y; } if (Y > by1) { by1 = Y; } }
          else { nbeh++; } }
    }
    if (bx1 < bx0) { bx0 = bx1 = by0 = by1 = 0.0f; }
    if (!e) {
        unsigned tf = 0; int tl = 0;
        if (g_fx_n >= FX_KEYS) { return 0; }
        e = &g_fx[g_fx_n++];
        memset(e, 0, sizeof *e);
        e->key = key; e->vs = g_im_shader; e->ps = g_im_pshader; e->xp = sb_tex_xptr();
        e->prim = prim; e->rhw = (unsigned)rhw; e->tgt = tgt;
        e->bon = bon; e->bs = bs; e->bd = bd; e->beq = beq; e->ate = ate; e->af = af; e->ar = ar;
        e->zf = zf; e->dm = rs_dmask; e->zen = m3d_zenable; e->cm = rs_cmask;
        e->amin = amin; e->amax = amax; e->cmax = cmax; e->n = nout;
        sb_tex_fmt(&tf, &tl);
        HLOG_W("FXKEY f=%u k=%u vs=0x%X ps=0x%X xp=0x%X tfmt=0x%X p=%u n=%u rhw=%d tgt=%u bl=%u/%u/%u eq=0x%X bc=0x%X at=%u/0x%X/%u zf=0x%X zen=%u dm=%u cm=0x%X col a=[%.2f,%.2f] rgbmax=%.2f pt=%u/%u/%u ps26=%u po=%u/%u ndc=[%.3f,%.3f]x[%.3f,%.3f] beh=%u",
               g_present_no, (unsigned)(e - g_fx), e->vs, e->ps, e->xp, tf, prim, nout, rhw, tgt,
               bon, bs, bd, beq, fx_nv(0x34Cu, 0), ate, af, ar, zf, m3d_zenable, rs_dmask, rs_cmask,
               amin, amax, cmax, fx_nv(0x318u, 0), fx_nv(0x31Cu, 0), fx_nv(0x43Cu, 0),
               *(volatile unsigned *)(uintptr_t)(SBA(0x21AC20u) + 26u * 4u),
               fx_nv(0x338u, 0), fx_nv(0x38Cu, 0), bx0, bx1, by0, by1, nbeh);
        { unsigned pc0, pd0, psrc; float fc[4]; fx_ps_diag(&pc0, &pd0, &psrc);
          HLOG_W("FXPS k=%u ps=0x%X c0=0x%08X def=0x%08X src=%u fold=%d", (unsigned)(e - g_fx), g_im_pshader, pc0, pd0, psrc, fx_ps_add_c0(fc)); }
        HLOG_W("FXREG k=%u %s slots=0x%04X s2=(%.3f,%.3f,%.3f,%.3f) s3=(%.3f,%.3f,%.3f,%.3f) s9=(%.3f,%.3f,%.3f,%.3f) uv0=(%.3f,%.3f) col0=(%.2f,%.2f,%.2f,%.2f)",
               (unsigned)(e - g_fx), g_fx_vb ? "VB" : "IM", g_fx_slotmask,
               g_fx_s2[0], g_fx_s2[1], g_fx_s2[2], g_fx_s2[3], g_fx_s3[0], g_fx_s3[1], g_fx_s3[2], g_fx_s3[3],
               g_fx_s9[0], g_fx_s9[1], g_fx_s9[2], g_fx_s9[3], out[0].uv[0], out[0].uv[1],
               out[0].col[0], out[0].col[1], out[0].col[2], out[0].col[3]);
        HLOG_W("FXRAW k=%u em=%d kind=%d n=%u v0=(%.3f,%.3f,%.3f,%.3f) v1=(%.3f,%.3f,%.3f,%.3f) v2=(%.3f,%.3f,%.3f,%.3f) v3=(%.3f,%.3f,%.3f,%.3f) m0=(%.4f,%.4f,%.4f,%.4f) m1=(%.4f,%.4f,%.4f,%.4f) m3=(%.4f,%.4f,%.4f,%.4f) o0=(%.2f,%.2f,%.4f,%.3f) o1=(%.2f,%.2f,%.4f,%.3f)",
               (unsigned)(e - g_fx), g_fx_vsh_em, g_fx_vsh_kind, g_fx_raw_n,
               g_fx_raw[0][0], g_fx_raw[0][1], g_fx_raw[0][2], g_fx_raw[0][3],
               g_fx_raw[1][0], g_fx_raw[1][1], g_fx_raw[1][2], g_fx_raw[1][3],
               g_fx_raw[2][0], g_fx_raw[2][1], g_fx_raw[2][2], g_fx_raw[2][3],
               g_fx_raw[3][0], g_fx_raw[3][1], g_fx_raw[3][2], g_fx_raw[3][3],
               g_fx_mat[0][0], g_fx_mat[0][1], g_fx_mat[0][2], g_fx_mat[0][3],
               g_fx_mat[1][0], g_fx_mat[1][1], g_fx_mat[1][2], g_fx_mat[1][3],
               g_fx_mat[3][0], g_fx_mat[3][1], g_fx_mat[3][2], g_fx_mat[3][3],
               out[0].pos[0], out[0].pos[1], out[0].pos[2], out[0].pos[3],
               nout > 1 ? out[1].pos[0] : 0.0f, nout > 1 ? out[1].pos[1] : 0.0f,
               nout > 1 ? out[1].pos[2] : 0.0f, nout > 1 ? out[1].pos[3] : 0.0f);
    }
    e->draws++;
    /* DIAG fx_shots=N: Bild in Frames mit Effekt-PS (V0+C0)*T0 (max. N, >= 15 Frames Abstand). */
    if (g_cfg.fx_shots > 0) {
        static unsigned nshot, lastf;
        float fc[4];
        int gate_ok = 1; /* DIAG(FX-HOLE,W106): mit m3d_census_gate=2 nur Kampagnen-Mission (W232: sb_in_mission) + VT online */
        if (g_cfg.m3d_census_gate >= 2) {
            __try { gate_ok = sb_in_mission()
                              && *(volatile unsigned *)(uintptr_t)M3D_VTONLINE_ADDR == 1u; }
            __except (EXCEPTION_EXECUTE_HANDLER) { gate_ok = 0; }
        }
        if (gate_ok && nshot < (unsigned)g_cfg.fx_shots && g_present_no - lastf >= (unsigned)(g_cfg.fx_shot_gap > 0 ? g_cfg.fx_shot_gap : 15) && fx_ps_add_c0(fc)) {
            nshot++; lastf = g_present_no; g_shot_census = 1;
            if (nshot <= (unsigned)g_cfg.fx_census) { g_fx_census_req = 1; } /* DIAG(FX-HOLE,W106) */
            HLOG_W("FXSHOT #%u f=%u k=%u c0=(%.2f,%.2f,%.2f,%.2f)", nshot, g_present_no, (unsigned)(e - g_fx), fc[0], fc[1], fc[2], fc[3]);
        }
    }
    if (amin < e->amin) { e->amin = amin; }
    if (amax > e->amax) { e->amax = amax; }
    if (cmax > e->cmax) { e->cmax = cmax; }
    /* Occlusion-Query: wie viele Fragmente bestehen Tiefe/Stencil? */
    if (e->pixq < 64u) {
        int q;
        for (q = 0; q < 8; q++) { if (!g_fx_qbusy[q]) { break; } }
        if (q < 8) {
            if (!g_fx_q[q]) {
                D3D11_QUERY_DESC qd; qd.Query = D3D11_QUERY_OCCLUSION; qd.MiscFlags = 0;
                if (FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &g_fx_q[q]))) { g_fx_q[q] = NULL; }
            }
            if (g_fx_q[q]) {
                ID3D11DeviceContext_Begin(g_ctx, (ID3D11Asynchronous *)g_fx_q[q]);
                g_fx_qk[q] = (int)(e - g_fx); g_fx_qbusy[q] = 1; g_fx_qcur = q;
                return 1;
            }
        }
    }
    return 0;
}

void fx_draw_post(int q)
{
    if (q && g_fx_qcur >= 0) {
        ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)g_fx_q[g_fx_qcur]);
        g_fx_qcur = -1;
    }
}

/* Je Present: alle 1500 Frames Summenzeile je Schlüssel mit neuen Draws. */
void fx_present(void)
{
    static unsigned last;
    unsigned i;
    if (!g_cfg.fx_diag) { return; }
    fx_poll();
    if (g_present_no - last < 1500u) { return; }
    last = g_present_no;
    for (i = 0; i < g_fx_n; i++) {
        fx_ent_t *e = &g_fx[i];
        if (e->draws == e->last_log_draws) { continue; }
        e->last_log_draws = e->draws;
        HLOG_W("FXSUM f=%u k=%u draws=%u pixq=%u pix/q=%u a=[%.2f,%.2f] rgbmax=%.2f",
               g_present_no, i, e->draws, e->pixq, e->pixq ? e->pix / e->pixq : 0u,
               e->amin, e->amax, e->cmax);
    }
}

/* ---------- FIX(FX-ORIG,W90): Combiner-Summe V0 + C0 im Standardpfad ----------
 * Der IM-Pixelshader (shading=0) rechnet Farbe = Vertexfarbe x Textur. Die
 * Effekt-Programme (VS ..E131: Mündungsfeuer, Rauch, Funkenspur) nutzen aber
 * den Pixelshader (Def-Muster, Lauf W90_k3 / W77-Testdaten AB802F0):
 *   Stufe 0: R0 = V0*1 + C0*1 (RGB und Alpha)   Stufe 1: R0 = R0 * T0
 *   Final:   out = R0, Alpha = R0.a
 * C0 ist die Laufzeit-Konstante des Spiels (Effekt-Intensität/-Deckkraft).
 * Ohne C0 bleiben die Effekte so dunkel/durchsichtig wie ihre Vertexfarbe
 * (Rauch: Vertex-Alpha 0..0,04 -> unsichtbar). Da V0 linear interpoliert wird,
 * ist (V0 + C0) je Vertex exakt dasselbe wie je Pixel (bis auf die Klemme
 * [0,1], die je Vertex angewandt wird). Nur dieses Muster, Shading-Pfad
 * unberührt (rechnet die Combiner selbst). */
static int fx_ps_def(const unsigned **pd)
{
    unsigned pdef = 0;
    if (!g_im_pshader) { return 0; }
    __try {
        pdef = *(unsigned *)(uintptr_t)((g_im_pshader & ~1u) + 8u);
        if (pdef < 0x10000u || pdef >= SB_HOST_LIM()) { return 0; } /* W315 host_hi_addr */
        *pd = (const unsigned *)(uintptr_t)pdef;
        (void)(*pd)[59];
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 1;
}

/* C0 der Stufe 0 wie im Shading-Pfad aufgelöst: Def-Wert, SetPixelShaderConstant
 * (C0-Mapping), SetRenderState(PSCONSTANT0_0) seit SetPixelShader. */
static unsigned fx_ps_c0_stage0(const unsigned *d)
{
    unsigned c0 = d[10], r0 = d[57] & 0xFu;
    /* RENDER-QA3 (W100): SetPixelShaderConstant-Werte (shade_psconst) nur im
     * Geltungsbereich des Shading-Pfads (Mission, shade_scope=0); Menues/Titel
     * bleiben wie ohne shade_psconst (Golden). */
    if (r0 < 8u && (g_rq_psconst_set & (1u << r0)) && (g_cfg.shade_scope || rq_in_mission())) { c0 = g_rq_psconst[r0]; }
    if (g_rq_rs_set & (1ull << 10u)) { c0 = g_rq_rs[10]; }
    return c0;
}

int fx_ps_add_c0(float c0[4])
{
    const unsigned *d = NULL;
    unsigned v;
    if (!fx_ps_def(&d)) { return 0; }
    __try {
        if ((d[53] & 0xFFu) != 2u) { return 0; }
        if (d[34] != 0xC420C120u || d[0] != 0xD430D130u) { return 0; } /* St.0: V0*1 + C0*1 */
        if (d[45] != 0x00000C00u || d[26] != 0x00000C00u) { return 0; } /* -> R0 (Summe) */
        if (d[35] != 0xCCC80000u || d[1] != 0xDCD81010u) { return 0; } /* St.1: R0*T0 */
        if (d[46] != 0x000000C0u || d[27] != 0x000000C0u) { return 0; }
        if (d[8] != 0x200C0000u || d[9] != 0x00001C80u) { return 0; }  /* Final = R0 */
        v = fx_ps_c0_stage0(d);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    c0[0] = (float)((v >> 16) & 0xFFu) / 255.0f;
    c0[1] = (float)((v >> 8) & 0xFFu) / 255.0f;
    c0[2] = (float)(v & 0xFFu) / 255.0f;
    c0[3] = (float)(v >> 24) / 255.0f;
    return 1;
}

/* FIX(LOAD-VT-COLOR,W257): Pixelshader-Muster "Farbe = V0 * C0" (1 Stufe:
 * RGB R0 = V0 * C0, Alpha R0.a = V0.a * C0.a, Final = R0, ohne Textur). Das
 * Spiel zeichnet so die Drahtgitter-VTs (Ladebild FUN_00012140, Nachschub-/
 * VT-Menue FUN_000D5C40): Vertexfarbe weiss (VB bzw. SetVertexDataColor
 * 0xFFFFFFFF), die VT-Farbe kommt allein aus SetPixelShaderConstant(c0).
 * Der Standardpfad (ohne Shading bzw. ausserhalb der Mission) rechnete nur
 * V0 -> weiss. Liefert C0 nur, wenn das Spiel die Konstante seit
 * SetPixelShader explizit gesetzt hat (SetPixelShaderConstant oder
 * PSCONSTANT0_0), sonst 0 = alter Pfad (minimaler Wirkungsbereich). */
int fx_ps_mul_c0(float c0[4])
{
    extern unsigned g_psc_raw[8], g_psc_raw_set;
    const unsigned *d = NULL;
    unsigned v = 0, r0, have = 0;
    if (!fx_ps_def(&d)) { return 0; }
    __try {
        if ((d[53] & 0xFFu) != 1u) { return 0; }
        if (d[34] != 0xC4C10000u || d[0] != 0xD4D11010u) { return 0; } /* St.0: V0*C0 (+0*0) */
        if (d[45] != 0x000000C0u || d[26] != 0x000000C0u) { return 0; } /* AB -> R0 */
        if (d[8] != 0x200C0000u || d[9] != 0x00001C80u) { return 0; }  /* Final = R0 */
        r0 = d[57] & 0xFu;
        if (r0 < 8u && (g_psc_raw_set & (1u << r0))) { v = g_psc_raw[r0]; have = 1; }
        if (g_rq_rs_set & (1ull << 10u)) { v = g_rq_rs[10]; have = 1; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    if (!have) { return 0; }
    c0[0] = (float)((v >> 16) & 0xFFu) / 255.0f;
    c0[1] = (float)((v >> 8) & 0xFFu) / 255.0f;
    c0[2] = (float)(v & 0xFFu) / 255.0f;
    c0[3] = (float)(v >> 24) / 255.0f;
    return 1;
}

/* DIAG: Konstanten-Stand des aktuellen PS fuer FXKEY. */
void fx_ps_diag(unsigned *c0, unsigned *def0, unsigned *src)
{
    const unsigned *d = NULL;
    *c0 = *def0 = *src = 0;
    if (!fx_ps_def(&d)) { return; }
    __try {
        unsigned r0 = d[57] & 0xFu;
        *def0 = d[10];
        *c0 = fx_ps_c0_stage0(d);
        *src = ((r0 < 8u && (g_rq_psconst_set & (1u << r0))) ? 1u : 0u) | ((g_rq_rs_set & (1ull << 10u)) ? 2u : 0u);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

/* ---------- W313 P2-ZTEST ----------
 * P2-Draws (VS-Programm mit Clip/Tail, render_im.c vsh_em == 2) laufen in
 * Spiel-RTs als Bildschirm-Pixel (rhw = 1, z = z/w aus dem Programm). Die NV2A
 * testet auch solche Vertices gegen den Z-Puffer, wenn das Spiel ZENABLE setzt
 * und das RT eine Tiefenflaeche hat (SetRenderTarget(rt, z)); ohne Tiefenflaeche
 * kein Test. Bisher schaltete rs_apply_ds fuer rhw immer ab -> Feuer/Partikel
 * vor Gebaeuden. Der Schattenpuffer (isdepth) ist in rs_apply_ds schon frei.
 * Draws im Sichtbarkeitstest (Sonne/Flares W304, Objekt-Vorpass FUN_00025790
 * mit Farbmaske 0) bleiben bei vis_ztest (W304-Entscheidung unberuehrt). */
int p2z_scene_mission(void) { return fx_in_mission(); } /* DIAG(W313): shot_mission=2-Anker (Szene 0x2C, Missionsschleife, auch Titel-Demo) */

int p2z_ztest(void)
{
    extern int sb_visq_in_test(void);
    return m3d_zenable && g_rtt_zsurf && !g_rtt_dummy && !sb_visq_in_test() && !g_sba_loc; /* LoC: nicht geprueft */
}

/* DIAG(P2-ZTEST,W313, p2z_diag=1): Zensus aller Draws in Spiel-RTs der
 * Kampagnen-Mission je Zustand: VS/PS/Textur (erste)/Ziel, Tiefenflaeche,
 * ZENABLE (Spiegel 0x20CF40 und NV 0x30C), ZFUNC, Tiefenmaske, Blend,
 * Alpha-Test, P2/rhw, Tiefentest angewendet; je Schluessel z-Bereich (NDC
 * nach mvp) und Pixel nach Tiefentest (Occlusion-Query: erste 32, dann jeder 64. Draw). */
#define P2Z_KEYS 384u
typedef struct {
    unsigned key, vs, ps, xp, prim, rhw, em, tgt, rw, rh, zs, zen, zen30c, zf, dm, cm, bon, bs, bd, ate, zt, po;
    unsigned draws, verts, pix, pixq, last;
    float zmin, zmax;
} p2z_ent_t;
static p2z_ent_t g_p2z[P2Z_KEYS];
static unsigned g_p2z_n;
static ID3D11Query *g_p2z_q[16];
static int g_p2z_qk[16], g_p2z_qbusy[16], g_p2z_qcur = -1;

static void p2z_poll(void)
{
    int i;
    for (i = 0; i < 16; i++) {
        UINT64 v = 0;
        if (!g_p2z_qbusy[i] || !g_p2z_q[i]) { continue; }
        if (ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)g_p2z_q[i], &v, sizeof v,
                                        D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) {
            g_p2z[g_p2z_qk[i]].pix += (unsigned)v; g_p2z[g_p2z_qk[i]].pixq++;
            g_p2z_qbusy[i] = 0;
        }
    }
}

void p2z_note(const im_vertex_t *out, unsigned nout, int rhw, int em, unsigned prim, int zt, const float *mvp)
{
    unsigned bon = 0, bs = 0, bd = 0, key, i, tgt, rw = 0, rh = 0, zs, ate, zf, z30c, po;
    float zmin = 1e30f, zmax = -1e30f;
    p2z_ent_t *e = NULL;
    g_p2z_qcur = -1;
    if (!rtt_active() || !fx_in_mission()) { return; }
    p2z_poll();
    sb_render_blend_state(&bon, &bs, &bd);
    tgt = (unsigned)g_rtt_bound; rtt_cur_dims(&rw, &rh);
    zs = g_rtt_zsurf ? 1u : 0u; ate = fx_nv(0x300u, 0); zf = fx_nv(0x354u, 0x203u); z30c = fx_nv(0x30Cu, 0xFFu);
    po = fx_nv(0x338u, 0);
    key = g_im_shader * 16777619u ^ g_im_pshader * 40503u ^ (prim << 24) ^ (bs << 4) ^ (bd << 8) ^ (bon << 12)
        ^ (ate << 13) ^ (rs_dmask << 14) ^ (tgt << 16) ^ (zf * 13u) ^ ((unsigned)rhw << 15) ^ ((unsigned)em << 28)
        ^ (m3d_zenable << 30) ^ (zs << 31) ^ ((unsigned)zt << 27) ^ (z30c * 977u) ^ (po << 26) ^ (rs_cmask * 2246822519u);
    for (i = 0; i < g_p2z_n; i++) { if (g_p2z[i].key == key) { e = &g_p2z[i]; break; } }
    for (i = 0; i < nout; i++) {
        const float *p = out[i].pos;
        float Z = p[0]*mvp[2]+p[1]*mvp[6]+p[2]*mvp[10]+p[3]*mvp[14];
        float W = p[0]*mvp[3]+p[1]*mvp[7]+p[2]*mvp[11]+p[3]*mvp[15];
        if (W > 1e-6f) { Z /= W; if (Z < zmin) { zmin = Z; } if (Z > zmax) { zmax = Z; } }
    }
    if (!e) {
        if (g_p2z_n >= P2Z_KEYS) { return; }
        e = &g_p2z[g_p2z_n++];
        memset(e, 0, sizeof *e);
        e->key = key; e->vs = g_im_shader; e->ps = g_im_pshader; e->xp = sb_tex_xptr(); e->prim = prim;
        e->rhw = (unsigned)rhw; e->em = (unsigned)em; e->tgt = tgt; e->rw = rw; e->rh = rh; e->zs = zs;
        e->zen = m3d_zenable; e->zen30c = z30c; e->zf = zf; e->dm = rs_dmask; e->cm = rs_cmask; e->bon = bon; e->bs = bs; e->bd = bd;
        e->ate = ate; e->zt = (unsigned)zt; e->po = po; e->zmin = zmin; e->zmax = zmax;
        HLOG_W("P2Z key f=%u k=%u vs=0x%X ps=0x%X xp=0x%X p=%u n=%u rhw=%d em=%d tgt=%u %ux%u zs=%u zen=%u nv30c=0x%X zf=0x%X dm=%u cm=0x%X bl=%u/%u/%u at=%u po=%u zt=%d z=[%.5f,%.5f]",
               g_present_no, (unsigned)(e - g_p2z), e->vs, e->ps, e->xp, prim, nout, rhw, em, tgt, rw, rh, zs,
               m3d_zenable, z30c, zf, rs_dmask, rs_cmask, bon, bs, bd, ate, po, zt, zmin, zmax);
    }
    e->draws++; e->verts += nout;
    if (zmin < e->zmin) { e->zmin = zmin; }
    if (zmax > e->zmax) { e->zmax = zmax; }
    if (e->pixq < 32u || (e->draws & 63u) == 0u) { /* erste 32, danach jeder 64. Draw */
        int q;
        for (q = 0; q < 16; q++) { if (!g_p2z_qbusy[q]) { break; } }
        if (q < 16) {
            if (!g_p2z_q[q]) {
                D3D11_QUERY_DESC qd; qd.Query = D3D11_QUERY_OCCLUSION; qd.MiscFlags = 0;
                if (FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &g_p2z_q[q]))) { g_p2z_q[q] = NULL; }
            }
            if (g_p2z_q[q]) {
                ID3D11DeviceContext_Begin(g_ctx, (ID3D11Asynchronous *)g_p2z_q[q]);
                g_p2z_qk[q] = (int)(e - g_p2z); g_p2z_qbusy[q] = 1; g_p2z_qcur = q;
            }
        }
    }
}

void p2z_post(void)
{
    if (g_p2z_qcur >= 0) {
        ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)g_p2z_q[g_p2z_qcur]);
        g_p2z_qcur = -1;
    }
}

/* Je 1500 Bilder Summenzeile je Schluessel mit neuen Draws. */
void p2z_present(void)
{
    static unsigned last;
    unsigned i;
    if (!g_cfg.p2z_diag) { return; }
    p2z_poll();
    if (g_present_no - last < 1500u) { return; }
    last = g_present_no;
    for (i = 0; i < g_p2z_n; i++) {
        p2z_ent_t *e = &g_p2z[i];
        if (e->draws == e->last) { continue; }
        e->last = e->draws;
        HLOG_W("P2Z sum f=%u k=%u draws=%u verts=%u pixq=%u pix/q=%u z=[%.5f,%.5f]",
               g_present_no, i, e->draws, e->verts, e->pixq, e->pixq ? e->pix / e->pixq : 0u, e->zmin, e->zmax);
    }
}
