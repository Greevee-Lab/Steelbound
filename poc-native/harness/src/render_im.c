/* render_im.c - D3D11-Renderer: Draw-Aufzeichnung und Submit. Immediate-Mode
 * (Begin/VertexData/End), VB-Pfad (Stream -> IM-Submit, VB-Decls), Mathe-
 * Helfer, PS-Stufe-0/RT-Viewport/Selbst-Sampling und sb_im_submit (IM ->
 * D3D11-Draw). Schnitt fuer FPS-INTERP: hier entsteht jeder Draw, Present/
 * Backbuffer bleiben in d3d11_render.c. W68 SPLIT: 1:1 aus d3d11_render.c. */
#include "render_int.h"
#include "render_fx_rem.h" /* W133 REM-FX */
#include "sb_perfhud.h" /* W125 TICK-PERF */
#include "sb_remaster.h" /* W103 REM-FRAME */
#include "render_qa.h"

/* ---------- Immediate-Mode (Xbox Begin/End, M2b-Geometrie) ----------
 * Semantik (Cxbx XbVertexBuffer.cpp): Register -1 (VERTEX) -> Slot 0
 * (POSITION); jeder Write setzt Slot[index]; POSITION-Write committet
 * den Vertex (Attribute persistieren). End zeichnet alle Vertices. */
#define IM_SLOTS 16
#define IM_MAX_VERTS 65536
unsigned g_im_draws, g_im_vdata, g_im_prim_hist[16];
unsigned g_im_tris, g_im_rhw, g_im_3d;
static int g_im_col_slot;
static int g_im_col_written; /* FIX(MONITORVIEW,W36): 1 = Draw rief SetVertexDataColor (sonst stale Slot-Inhalt) */
unsigned g_im_texd, g_im_notex;
static float g_im_cur[IM_SLOTS][4];
static float g_im_buf[IM_MAX_VERTS][IM_SLOTS][4];
static unsigned g_im_nverts, g_im_prim;
static int g_rq_vb_ok; /* RENDER-QA (W77): VB-Eingaenge fuer den Shading-Pfad dekodiert */
static unsigned g_rq_vb_mask;
/* PERF(IM-PERF,W99, im_fast=1): VB-Draw kompakt dekodiert (pos/col/uv wie die
 * Slots 0/3/9 von g_im_buf, die der alte Weg je Vertex mit 256 B memset +
 * Dekodierung fuellte und in sb_im_submit wieder umkopierte). */
static im_vertex_t g_imf[IM_MAX_VERTS];
static int g_im_fast;
long long g_imp_q[IMP_N]; /* DIAG(IM-PERF,W99): Teilabschnitte (rq_perf) */
unsigned g_imp_fastd, g_imp_vbv, g_imp_imv;

void sb_render_im_begin(unsigned prim)
{
    g_im_draws++;
    if (prim < 16) {
        g_im_prim_hist[prim]++;
    }
    g_im_prim = prim;
    g_im_nverts = 0;
    g_im_col_slot = -1;
    g_im_col_written = 0;
}

void sb_render_im_vdata(unsigned reg, float a, float b, float c, float d)
{
    unsigned idx = (reg == 0xFFFFFFFFu) ? 0 : reg;
    g_im_vdata++;
    if (idx >= IM_SLOTS) {
        return;
    }
    g_im_cur[idx][0] = a;
    g_im_cur[idx][1] = b;
    g_im_cur[idx][2] = c;
    g_im_cur[idx][3] = d;
    if (idx == 0) {
        /* POSITION-Write committet den Vertex. */
        if (g_im_nverts < IM_MAX_VERTS) {
            memcpy(g_im_buf[g_im_nverts], g_im_cur, sizeof g_im_cur);
            g_im_nverts++;
        }
    }
}

static int g_im_col_last = -1; /* W215 rbc: zuletzt per SetVertexDataColor gesetztes Register */

void sb_render_im_color(unsigned reg, unsigned color)
{
    unsigned idx = (reg == 0xFFFFFFFFu) ? 0 : reg;
    if (idx < IM_SLOTS) {
        /* Xbox-Farbe ARGB -> float4 RGBA. */
        g_im_cur[idx][0] = (float)((color >> 16) & 0xFF) / 255.0f;
        g_im_cur[idx][1] = (float)((color >> 8) & 0xFF) / 255.0f;
        g_im_cur[idx][2] = (float)(color & 0xFF) / 255.0f;
        g_im_cur[idx][3] = (float)((color >> 24) & 0xFF) / 255.0f;
        g_im_col_slot = (int)idx;
        g_im_col_written = 1;
        g_im_col_last = (int)idx; /* W215: Farbregister bleibt ueber Begin hinaus */
    }
    g_im_vdata++;
}


void sb_render_im_end_nolock(void)
{
    g_submit_vb = 0; /* DIAG(MOVIE): IM-Pfad */
    if (g_cfg.rtt_rb_sync == 2) { /* W215: Zellenkarte zusaetzlich auf der CPU rastern */
        sb_rtt_rbc_im(g_im_prim, &g_im_buf[0][0][0], (unsigned)g_im_nverts, IM_SLOTS, g_im_col_last);
    }
    sb_im_submit();
    /* FIX(EDGE,W64): End() schliesst den Batch (Xbox/cxbx: nach End ist kein
     * Vertex mehr offen). Ohne Reset blieb g_im_nverts stehen; der naechste
     * VB-Draw hielt das fuer eine offene IM-Batch und zeichnete sie mit SEINEM
     * State erneut ("Interleave-Flush", sb_render_vb_draw). Mission: Wash-Quad
     * FUN_000174d0 (0..256 px, fuer RT41) wurde mit Face-VS AB7D8B1 + S40 auf
     * den Backbuffer wiederholt -> Riesen-Quad oben rechts (Hor+-Rand hell). */
    if (g_cfg.im_end_reset) { g_im_nverts = 0; }
}

/* A6 ctx_lock: IM-Submit unter Context-Lock (Rumpf in _nolock). */
void sb_render_im_end(void)
{
    sb_ctx_enter();
    sb_render_im_end_nolock();
    sb_ctx_leave();
}

/* Aktiver Vertex-Shader/FVF (Pfad-Wahl beim Submit). */
unsigned g_im_shader;
static int g_im_force3d; /* VB: FLOAT3/4-pos -> 3D erzwingen */

void sb_render_im_shader(unsigned handle)
{
    g_im_shader = handle;
}

/* ---------- M2c-VB-Pfad (Stream -> IM-Submit) ---------- */
/* VB-Decl aus aktivem VS (Slots an (h&~1)+0x14, je 16 B: stream/off/fmt;
 * Xbox X_VERTEXSHADERINPUT). Rollen: pos = Slot0 (FLOAT2/3/4); col =
 * erstes D3DCOLOR; uv = erstes 2-komponentiges Feld ausser pos. Nur
 * Stream 0 (Spiel nutzt nur diesen). Konvertiert nach g_im_buf
 * (Slot0=pos, Slot3=col, Slot9=uv) und ruft den erprobten IM-Submit. */
static unsigned char g_ta_raw[3][32]; static unsigned g_ta_stride, g_ta_nraw; /* DIAG(TEXADDR,W84) */
typedef struct { unsigned off, fmt; int ok; } vb_field_t;
typedef struct {
    unsigned handle;
    vb_field_t pos, col, uv;
    int is_rhw;
} vb_decl_t;
/* FIX(COCKPIT,W60): 16 -> 64. Mission 1 belegt bereits 16 Decls; der 17. VS
 * (z. B. 0xAB700F1, W60_c1) bekam NULL -> 2070 VB-Draws still verworfen. */
#define VB_MAX_DECLS 64u
static vb_decl_t g_vb_decls[VB_MAX_DECLS];
static unsigned g_vb_ndecl;

static const vb_decl_t *sb_vb_decl(unsigned handle)
{
    unsigned i;
    for (i = 0; i < g_vb_ndecl; i++) {
        if (g_vb_decls[i].handle == handle) {
            return &g_vb_decls[i];
        }
    }
    if (g_vb_ndecl >= VB_MAX_DECLS || !handle) {
        return NULL;
    }
    /* M2c: FVF-Handles (klein, Bit0=0): kein Struct, direkt dekodieren.
     * D3D-Feldreihenfolge: RHW/XYZ, Normal, PSize, Diffuse, Specular, TexN.
     * 0x144 = XYZRHW|DIFFUSE|TEX1 -> pos=FLOAT4@0, col@16, uv=F2@20 (28 B). */
    if ((handle & 1u) == 0 && handle < 0x10000u && handle != 0) {
        vb_decl_t *d = &g_vb_decls[g_vb_ndecl];
        unsigned off = 0, texn, k;
        memset(d, 0, sizeof *d);
        d->handle = handle;
        if (handle & 0x004u) {
            d->pos.off = 0; d->pos.fmt = 0x42u; d->pos.ok = 1;
            d->is_rhw = 1;
            off = 16;
        } else if (handle & 0x002u) {
            d->pos.off = 0; d->pos.fmt = 0x32u; d->pos.ok = 1;
            off = 12;
            if ((handle & 0x00Eu) == 0x006u) { off += 4; }
            else if ((handle & 0x00Eu) == 0x008u) { off += 8; }
            else if ((handle & 0x00Eu) == 0x00Au) { off += 12; }
            else if ((handle & 0x00Eu) == 0x00Cu) { off += 16; }
        }
        if (handle & 0x010u) { off += 12; } /* Normal */
        if (handle & 0x020u) { off += 4; } /* PSize */
        if (handle & 0x040u) {
            d->col.off = off; d->col.fmt = 0x40u; d->col.ok = 1;
            off += 4;
        }
        if (handle & 0x080u) { off += 4; } /* Specular */
        texn = (handle & 0xF00u) >> 8;
        for (k = 0; k < texn && k < 4; k++) {
            unsigned sz = (handle >> (16 + k * 2)) & 3u;
            if (k == 0 && !d->uv.ok) {
                d->uv.off = off;
                d->uv.fmt = (sz == 3u) ? 0x22u : (sz == 1u) ? 0x32u : (sz == 2u) ? 0x42u : 0x22u;
                d->uv.ok = 1;
            }
            off += (sz == 3u) ? 4u : (sz == 1u) ? 12u : (sz == 2u) ? 16u : 8u;
        }
        HLOG_I("VB decl #%u: FVF=0x%X pos=%d@%u/%02X col=%d@%u uv=%d@%u/%02X rhw=%d",
               g_vb_ndecl + 1, handle, d->pos.ok, d->pos.off, d->pos.fmt,
               d->col.ok, d->col.off, d->uv.ok, d->uv.off, d->uv.fmt,
               d->is_rhw);
        g_vb_ndecl++;
        return d;
    }
    {
        vb_decl_t *d = &g_vb_decls[g_vb_ndecl];
        unsigned base = handle & ~1u;
        memset(d, 0, sizeof *d);
        d->handle = handle;
        __try {
            for (i = 0; i < 16; i++) {
                unsigned *sl = (unsigned *)(uintptr_t)(base + 0x14u + i * 16u);
                unsigned stream = sl[0], off = sl[1], fmt = sl[2] & 0xFFu;
                if (stream != 0 || fmt == 0x02u) {
                    continue; /* nur Stream 0; 0x02 = NONE */
                }
                if (i == 0 && (fmt == 0x22u || fmt == 0x32u || fmt == 0x42u)) {
                    d->pos.off = off; d->pos.fmt = fmt; d->pos.ok = 1;
                } else if (fmt == 0x40u && !d->col.ok) {
                    d->col.off = off; d->col.fmt = fmt; d->col.ok = 1;
                } else if (!d->uv.ok && (fmt == 0x22u || fmt == 0x21u ||
                                         fmt == 0x25u || fmt == 0x24u ||
                                         fmt == 0x72u) &&
                           !(i == 0 && d->pos.ok)) {
                    d->uv.off = off; d->uv.fmt = fmt; d->uv.ok = 1;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            memset(d, 0, sizeof *d);
            d->handle = handle;
        }
        HLOG_I("VB decl #%u: vs=0x%X pos=%d@%u/%02X col=%d@%u uv=%d@%u/%02X",
               g_vb_ndecl + 1, handle, d->pos.ok, d->pos.off, d->pos.fmt,
               d->col.ok, d->col.off, d->uv.ok, d->uv.off, d->uv.fmt);
        g_vb_ndecl++;
        return d;
    }
}

static void sb_vb_pos(float out[4], const unsigned char *v,
                      const vb_field_t *f)
{
    const float *fl = (const float *)(v + f->off);
    switch (f->fmt) {
    case 0x22u:
        out[0] = fl[0]; out[1] = fl[1]; out[2] = 0.0f; out[3] = 1.0f;
        break;
    case 0x32u:
        out[0] = fl[0]; out[1] = fl[1]; out[2] = fl[2]; out[3] = 1.0f;
        break;
    default: /* 0x42u */
        out[0] = fl[0]; out[1] = fl[1]; out[2] = fl[2]; out[3] = fl[3];
        break;
    }
}

static void sb_vb_col(float out[4], const unsigned char *v,
                      const vb_field_t *f)
{
    unsigned argb = *(const unsigned *)(v + f->off);
    out[3] = (float)(argb >> 24) / 255.0f;
    out[0] = (float)((argb >> 16) & 0xFFu) / 255.0f;
    out[1] = (float)((argb >> 8) & 0xFFu) / 255.0f;
    out[2] = (float)(argb & 0xFFu) / 255.0f;
}

static void sb_vb_uv(float out[2], const unsigned char *v,
                     const vb_field_t *f)
{
    switch (f->fmt) {
    case 0x25u: /* SHORT2 */
        /* FIX(COCKPIT,W60): SHORT2 ist roh (NV2A/cxbx; nur NORMSHORT2 wird
         * normiert). Einziger Nutzer ist die Monitor-Face-Decl (VS AB7D8B1,
         * oT0 = v3 unveraendert): Texel-Koords in LINEAR-Texturen (RT S40/
         * RT32/RT39, Cockpit-Anzeigen 0x28FC000C/0x2901000C, Format 0x12
         * LIN_A8R8G8B8), z. B. Hauptmonitor 0..512 x 0..422. Die Normierung
         * /(w,h) macht der lin_tex_rect-Pfad in sb_im_submit. Alt (/32767)
         * ergab UV 0..0.016 -> jede Face sampelte eine Ecke (Einfarbflaeche,
         * "Mitte leer", W38-R1/W58). */
        if (g_cfg.short2_raw) {
            const short *s = (const short *)(v + f->off);
            out[0] = (float)s[0];
            out[1] = (float)s[1];
            break;
        }
        /* fall through: alt wie NORMSHORT2 */
    case 0x21u: { /* NORMSHORT2 */
        const short *s = (const short *)(v + f->off);
        out[0] = (float)s[0] / 32767.0f;
        out[1] = (float)s[1] / 32767.0f;
        break;
    }
    case 0x24u: /* PBYTE2 */
        out[0] = (float)v[f->off] / 255.0f;
        out[1] = (float)v[f->off + 1] / 255.0f;
        break;
    default: /* FLOAT2 / FLOAT2H: erste 2 Floats */
        out[0] = ((const float *)(v + f->off))[0];
        out[1] = ((const float *)(v + f->off))[1];
        break;
    }
}

static void sb_render_vb_draw_body(unsigned prim, const unsigned char *vdata,
                                   unsigned stride, const unsigned short *idx,
                                   unsigned idxcount, unsigned base_vtx, unsigned start,
                                   unsigned count);
void sb_render_vb_draw(unsigned prim, const unsigned char *vdata,
                       unsigned stride, const unsigned short *idx,
                       unsigned idxcount, unsigned base_vtx, unsigned start,
                       unsigned count)
{
    int z0 = g_ph_zone; /* DIAG(PERF-TICK,W168) */
    g_ph_zone = 1;
    sb_render_vb_draw_body(prim, vdata, stride, idx, idxcount, base_vtx, start, count);
    g_ph_zone = z0;
}
static void sb_render_vb_draw_body(unsigned prim, const unsigned char *vdata,
                                   unsigned stride, const unsigned short *idx,
                                   unsigned idxcount, unsigned base_vtx, unsigned start,
                                   unsigned count)
{
    const vb_decl_t *d;
    unsigned n, i;
    static unsigned nwarn;
    long long rqp_q0 = 0; /* DIAG(SHADE-PERF,W80) */
    if (!vdata || stride == 0 || stride > 256) {
        sb_render_drawdrop(0, g_im_shader, idx != NULL ? idxcount : count, 0);
        return;
    }
    d = sb_vb_decl(g_im_shader);
    if (!d || !d->pos.ok) {
        sb_render_drawdrop(1, g_im_shader, idx != NULL ? idxcount : count, 0);
        if (nwarn < 3) {
            nwarn++;
            HLOG_W("VB draw skipped: vs=0x%X (kein pos)", g_im_shader);
        }
        return;
    }
    n = (idx != NULL) ? idxcount : count;
    if (n == 0) {
        sb_render_drawdrop(2, g_im_shader, 0, 0);
        return;
    }
    if (n > IM_MAX_VERTS) {
        if (nwarn < 6) {
            nwarn++;
            HLOG_W("VB draw gekappt: n=%u stride=%u", n, stride);
        }
        n = IM_MAX_VERTS;
    }
    /* Interleave-Schutz: offene IM-Batch zuerst flushen. */
    if (g_im_nverts > 0) {
        static unsigned nflush;
        if (nflush < 3) {
            nflush++;
            HLOG_I("VB draw: IM-Batch (%u) vorgeflusht", g_im_nverts);
        }
        g_submit_vb = 0; /* DIAG(MOVIE): IM-Rest */
        sb_ctx_enter(); /* A6 ctx_lock (Interleave-Flush) */
        sb_im_submit();
        sb_ctx_leave();
    }
    g_rq_vb_ok = 0;
    if (g_cfg.shading) { /* RENDER-QA (W77): nur benoetigte v-Register, nur geeignete Draws */
        long long q0 = RQP_ON ? rq_qpc() : 0;
        g_rq_vb_mask = rq_vb_prepare();
        /* SHADE-PERF (W80): Roh-Eingaenge -> GPU dekodiert, keine CPU-Dekodierung */
        if (g_rq_vb_mask && rq_vb_raw_set(vdata, stride, idx, base_vtx, start, n, (const float (*)[4])g_im_cur)) {
            g_rq_vb_mask = 0;
        }
        g_rq_vb_ok = (g_rq_vb_mask && rq_vb_vin() != NULL) ? 1 : 0;
        if (RQP_ON) { g_rqp_q[RQP_PREP] += rq_qpc() - q0; }
    }
    if (RQP_ON) { rqp_q0 = rq_qpc(); g_imp_vbv += n; } /* DIAG(SHADE-PERF,W80) */
    /* PERF(IM-PERF,W99): Diagnosen, die g_im_buf/out lesen, nehmen den alten Weg */
    g_im_fast = (g_cfg.im_fast && !g_cfg.rq_diag && !g_cfg.fx_diag && !m3d_on && !g_cfg.sbar_diag &&
                 g_cfg.interp_diag < 3) ? 1 : 0;
    if (g_im_fast) {
        __try {
            for (i = 0; i < n; i++) {
                unsigned vi = (idx != NULL) ? (unsigned)idx[i] + base_vtx : start + i;
                const unsigned char *v = vdata + (size_t)vi * stride;
                im_vertex_t *o = &g_imf[i];
                if (g_rq_vb_ok && !rq_vb_decode(g_im_shader, v, (const float (*)[4])g_im_cur, rq_vb_vin()[i], g_rq_vb_mask)) {
                    g_rq_vb_ok = 0;
                }
                sb_vb_pos(o->pos, v, &d->pos);
                if (d->col.ok) { sb_vb_col(o->col, v, &d->col); }
                else { o->col[0] = o->col[1] = o->col[2] = o->col[3] = 1.0f; }
                if (d->uv.ok) { sb_vb_uv(o->uv, v, &d->uv); }
                else { o->uv[0] = o->uv[1] = 0.0f; }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_im_fast = 0;
            sb_render_drawdrop(3, g_im_shader, n, 0);
            if (g_cfg.shading) { rq_vb_raw_clear(); } /* SHADE-PERF (W80) */
            if (nwarn < 9) {
                nwarn++;
                HLOG_W("VB draw fault: n=%u stride=%u", n, stride);
            }
            return;
        }
        if (RQP_ON) { g_imp_fastd++; }
    }
    if (!g_im_fast || g_cfg.im_fast == 2) { /* DIAG(IM-PERF,W99): im_fast=2 -> zusaetzlich alter Weg (Selbstpruefung) */
    __try {
        for (i = 0; i < n; i++) {
            unsigned vi = (idx != NULL) ? (unsigned)idx[i] + base_vtx
                                        : start + i;
            const unsigned char *v = vdata + (size_t)vi * stride;
            if (g_rq_vb_ok && !rq_vb_decode(g_im_shader, v, (const float (*)[4])g_im_cur, rq_vb_vin()[i], g_rq_vb_mask)) {
                g_rq_vb_ok = 0; /* RENDER-QA: alle v-Register fuer den Shading-Pfad */
            }
            memset(g_im_buf[i], 0, sizeof g_im_buf[i]);
            sb_vb_pos(g_im_buf[i][0], v, &d->pos);
            if (d->col.ok) {
                sb_vb_col(g_im_buf[i][3], v, &d->col);
            } else {
                g_im_buf[i][3][0] = g_im_buf[i][3][1] =
                    g_im_buf[i][3][2] = g_im_buf[i][3][3] = 1.0f;
            }
            if (d->uv.ok) {
                sb_vb_uv(g_im_buf[i][9], v, &d->uv);
                g_im_buf[i][9][2] = 0.0f;
                g_im_buf[i][9][3] = 1.0f;
            } else {
                g_im_buf[i][9][3] = 1.0f;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_im_fast = 0; /* PERF(IM-PERF,W99) */
        sb_render_drawdrop(3, g_im_shader, n, 0);
        if (g_cfg.shading) { rq_vb_raw_clear(); } /* SHADE-PERF (W80) */
        if (nwarn < 9) {
            nwarn++;
            HLOG_W("VB draw fault: n=%u stride=%u", n, stride);
        }
        return;
    }
    }
    if (RQP_ON) { g_rqp_q[RQP_VBLOOP] += rq_qpc() - rqp_q0; } /* DIAG(SHADE-PERF,W80) */
    if (g_im_fast) { /* PERF(IM-PERF,W99): Log-Stichprobe wie unten, aus g_imf */
        static unsigned nvdraw2;
        if (nvdraw2 < 12 && n > 0) {
            struct ID3D11ShaderResourceView *s = sb_tex_get();
            nvdraw2++;
            HLOG_I("VB draw #%u: prim=%u n=%u vs=0x%X stride=? tex=%d xptr=0x%X",
                   nvdraw2, prim, n, g_im_shader, s ? 1 : 0, sb_tex_xptr());
            HLOG_I("  pos0=(%.1f,%.1f,%.1f,%.1f) posN=(%.1f,%.1f,%.1f,%.1f)",
                   g_imf[0].pos[0], g_imf[0].pos[1], g_imf[0].pos[2], g_imf[0].pos[3],
                   g_imf[n - 1].pos[0], g_imf[n - 1].pos[1], g_imf[n - 1].pos[2], g_imf[n - 1].pos[3]);
            HLOG_I("  uv0=(%.3f,%.3f) uvN=(%.3f,%.3f) col0=(%.2f,%.2f,%.2f,%.2f)",
                   g_imf[0].uv[0], g_imf[0].uv[1], g_imf[n - 1].uv[0], g_imf[n - 1].uv[1],
                   g_imf[0].col[0], g_imf[0].col[1], g_imf[0].col[2], g_imf[0].col[3]);
        }
    } else
    {
        static unsigned nvdraw;
        if (nvdraw < 12 && n > 0) {
            struct ID3D11ShaderResourceView *s = sb_tex_get();
            nvdraw++;
            HLOG_I("VB draw #%u: prim=%u n=%u vs=0x%X stride=? tex=%d xptr=0x%X",
                   nvdraw, prim, n, g_im_shader, s ? 1 : 0, sb_tex_xptr());
            HLOG_I("  pos0=(%.1f,%.1f,%.1f,%.1f) posN=(%.1f,%.1f,%.1f,%.1f)",
                   g_im_buf[0][0][0], g_im_buf[0][0][1],
                   g_im_buf[0][0][2], g_im_buf[0][0][3],
                   g_im_buf[n - 1][0][0], g_im_buf[n - 1][0][1],
                   g_im_buf[n - 1][0][2], g_im_buf[n - 1][0][3]);
            HLOG_I("  uv0=(%.3f,%.3f) uvN=(%.3f,%.3f) col0=(%.2f,%.2f,%.2f,%.2f)",
                   g_im_buf[0][9][0], g_im_buf[0][9][1],
                   g_im_buf[n - 1][9][0], g_im_buf[n - 1][9][1],
                   g_im_buf[0][3][0], g_im_buf[0][3][1],
                   g_im_buf[0][3][2], g_im_buf[0][3][3]);
        }
    }
    if (g_cfg.rq_diag >= 4) { /* DIAG(TEXADDR,W84): Rohbytes der ersten 3 Vertices + Stride */
        unsigned q3;
        g_ta_stride = stride; g_ta_nraw = 0;
        __try {
            for (q3 = 0; q3 < 3u && q3 < n; q3++) {
                unsigned vi3 = (idx != NULL) ? (unsigned)idx[q3] + base_vtx : start + q3;
                memcpy(g_ta_raw[q3], vdata + (size_t)vi3 * stride, stride < 32u ? stride : 32u);
                g_ta_nraw++;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { g_ta_nraw = 0; }
    }
    g_im_draws++;
    if (prim < 16) {
        g_im_prim_hist[prim]++;
    }
    g_im_nverts = n;
    g_im_prim = prim;
    sb_ctx_enter(); /* A6 ctx_lock (VB-Submit; keine Returns bis leave) */
    g_im_col_slot = 3;
    g_im_col_written = 1; /* VB-Farben sind explizit (Decl/Weiss), nie stale */
    g_im_force3d = (!d->is_rhw && (d->pos.fmt == 0x32u || d->pos.fmt == 0x42u)) ? 1 : 0;
    g_submit_vb = 1; /* DIAG(MOVIE): VB-Pfad */
    sb_im_submit();
    g_im_nverts = 0;
    g_im_force3d = 0;
    g_im_fast = 0; /* PERF(IM-PERF,W99) */
    if (g_cfg.shading) { rq_vb_raw_clear(); } /* SHADE-PERF (W80) */
    sb_ctx_leave();
}

/* Float-Bitbild (NaN/Inf als %08X; Min/Max wuerden sie
 * verstecken, da jeder Vergleich mit NaN falsch ist). */
static unsigned sb_fbits(float f)
{
    unsigned u;
    memcpy(&u, &f, sizeof u);
    return u;
}

/* Row-major 4x4-Multiplikation (D3DMATRIX-Konvention): C = A*B. */
static void sb_mat4_mul(float out[16], const float a[16], const float b[16])
{
    int i, j, k;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            float s = 0.0f;
            for (k = 0; k < 4; k++) {
                s += a[i * 4 + k] * b[k * 4 + j];
            }
            out[i * 4 + j] = s;
        }
    }
}

/* FIX(WORLDVIEW,W65): 1 = aktueller Pixel-Shader liest Textur-Stufe 0 nicht
 * (D3DPIXELSHADERDEF.PSTextureModes, Bits 0-4 = Stufe 0, 0 = NONE). PS-Handle
 * = Struct mit Zeiger auf die Definition an +8 (TEX3D-Dump: raw[2]),
 * PSTextureModes an Def+0xD8. Ohne PS (Fixed Function), unlesbar oder
 * Schalter aus -> 0 (alter Pfad: Stage-0-Textur wird gesampelt). */
static int ps_stage0_none(void)
{
    int none = 0;
    if (!g_cfg.ps_texmode || !g_im_pshader) { return 0; }
    __try {
        unsigned base = g_im_pshader & ~1u;
        unsigned pdef = *(unsigned *)(uintptr_t)(base + 8u);
        if (pdef >= 0x10000u && pdef < 0x80000000u) {
            unsigned tm = *(unsigned *)(uintptr_t)(pdef + 0xD8u);
            none = ((tm & 0x1Fu) == 0u) ? 1 : 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { none = 0; }
    return none;
}

/* FIX(WORLDVIEW,W65): Nachmatrix Clip -> RT-NDC fuer Clip-Raum-Draws in ein
 * RT mit Teil-Viewport (row-major, out = pos x M). Xbox-Tail: px = x/w *
 * vw/2 + vw/2 + vx, py = -y/w * vh/2 + vh/2 + vy; D3D11 bildet NDC aufs ganze
 * RT (rw x rh) ab -> x' = x*vw/rw + w*((vw+2vx)/rw - 1),
 * y' = y*vh/rh + w*(1 - (vh+2vy)/rh), z/w unveraendert (wie bisher: z/w in
 * [0,1], P2-Pfad identisch). 0 = kein RT / voller Viewport (Identitaet). */
static int rt_vp_matrix(float m[16])
{
    unsigned rw = 0, rh = 0;
    float vx, vy, vw, vh;
    if (!rtt_active()) { return 0; }
    rtt_cur_dims(&rw, &rh);
    if (!rw || !rh || !g_vp_game.w || !g_vp_game.h) { return 0; }
    if (g_vp_game.x == 0 && g_vp_game.y == 0 && g_vp_game.w == rw && g_vp_game.h == rh) { return 0; }
    vx = (float)g_vp_game.x; vy = (float)g_vp_game.y;
    vw = (float)g_vp_game.w; vh = (float)g_vp_game.h;
    memset(m, 0, 16 * sizeof(float));
    m[0] = vw / (float)rw;
    m[12] = (vw + 2.0f * vx) / (float)rw - 1.0f;
    m[5] = vh / (float)rh;
    m[13] = 1.0f - (vh + 2.0f * vy) / (float)rh;
    m[10] = 1.0f;
    m[15] = 1.0f;
    return 1;
}

/* SIDESMEAR (W45): Selbst-Sampling-Test: bildet die Unbind-Entscheidung aus
 * sb_rtt_lookup exakt ab (erster Data-Match + Dims + rtt/srv + bound==i).
 * Trifft zu, bindet der erste Textur-Zugriff im Draw auf BB um, waehrend das
 * MVP schon fuers RT gerechnet ist -> RT-gemappter Draw landet Vollbild auf
 * BB (Seiten-Schmier statt Pillarbox, s. BERICHT W45). Aktion
 * (rtt_selfclip=1): Draw per Scissor auf die Aspect-Box einclippen -> Seiten
 * schwarz, Mitte/S40/Draw-Folge exakt wie bisher (der folgende Wash-Draw
 * entweicht weiter wie bisher, sonst deckt er die Welt in S40 zu). */
static unsigned g_rtt_self_n;
static int rtt_self_sample(void)
{
    unsigned xp, data = 0, c0 = 0, tfmt = 0, tsize = 0, tw = 0, th = 0;
    int i;
    xp = sb_tex_xptr();
    if (!xp) { return 0; }
    __try {
        data = *(unsigned *)(uintptr_t)(xp + 4u);
        c0 = *(unsigned *)(uintptr_t)xp;
        tfmt = *(unsigned *)(uintptr_t)(xp + 12u);
        tsize = *(unsigned *)(uintptr_t)(xp + 16u);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    if (!data) { return 0; }
    for (i = 0; i < g_rtt_n; i++) {
        if (g_rtt[i].surf && g_rtt[i].data == data && !g_rtt[i].isdepth) { break; }
    }
    if (i >= g_rtt_n) { return 0; }
    if (c0 == 0xC0002u) { tw = 640; th = 480; }
    else if (!rtt_dims(tfmt, tsize, &tw, &th)) { return 0; }
    if (!tw || tw != g_rtt[i].w || th != g_rtt[i].h) { return 0; }
    if (!g_cfg.rtt || !g_rtt[i].srv) { return 0; }
    return (g_rtt_bound == i) ? 1 : 0;
}

/* SIDESMEAR: Clip an/aus (nur um den Draw, stellt exakt wieder her). */
static void sb_selfclip_begin(void)
{
    unsigned bx, by, bw, bh;
    D3D11_RECT rc;
    if (!g_rs_scis) { return; }
    sb_aspect_box(&bx, &by, &bw, &bh);
    sb_ctx_enter();
    ID3D11DeviceContext_RSSetState(g_ctx, g_rs_scis);
    rc.left = (LONG)bx; rc.top = (LONG)by;
    rc.right = (LONG)(bx + bw); rc.bottom = (LONG)(by + bh);
    ID3D11DeviceContext_RSSetScissorRects(g_ctx, 1, &rc);
    sb_ctx_leave();
}
static void sb_selfclip_end(void)
{
    if (!g_rs_scis || !g_rs_norm) { return; }
    sb_ctx_enter();
    ID3D11DeviceContext_RSSetState(g_ctx, g_rs_norm);
    sb_ctx_leave();
}

/* FIX(TEXADDR,W84): Adressmodus der Stufe 0 im alten Pfad. Bisher war der
 * Sampler s0 immer CLAMP (Init in render_shader.c). Der Missionsboden nutzt
 * NORMSHORT2-UVs in [-1,1] (oT0 = v3, kein Texgen) mit Adressmodus WRAP/
 * MIRROR (D3DTSS_ADDRESSU/V, Deferred-TextureStates 0x21A8B0) - CLAMP zog
 * die Randtexel ueber alle Flaechen mit UV < 0 bzw. > 1 ("Marmor"-Muster,
 * Nutzer-Playtest 3.11.). tex_addr=1: Werte des Spiels (0 = nie gesetzt ->
 * CLAMP), tex_addr=2: 0 -> WRAP (D3D-Grundzustand). Nur in der Mission. */
ID3D11SamplerState *g_ta_smp;           /* Sampler des laufenden alten Draws (NULL = Init-CLAMP) */
static ID3D11SamplerState *g_ta_cache[6][6], *g_ta_def;
static ID3D11SamplerState *g_ta_bound = (ID3D11SamplerState *)1; /* 1 = unbekannt */
unsigned g_ta_hist[6][6];               /* DIAG: Draws je (au, av) */
static int g_ta_oob;                    /* DIAG: UV ausserhalb [0,1] (erste 64 Vertices) */
static int g_ta_uvslot;                 /* DIAG: UV-Slot des Draws */
ID3D11SamplerState *sb_ta_sampler(unsigned au, unsigned av)
{
    static const D3D11_TEXTURE_ADDRESS_MODE am[6] = {
        D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_MIRROR,
        D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_BORDER, D3D11_TEXTURE_ADDRESS_CLAMP };
    D3D11_SAMPLER_DESC sd;
    if (au > 5u) { au = 0; }
    if (av > 5u) { av = 0; }
    if (au == 0 && av == 0) {
        if (!g_ta_def) { au = 0; } else { return g_ta_def; }
    } else if (g_ta_cache[au][av]) {
        return g_ta_cache[au][av];
    }
    memset(&sd, 0, sizeof sd); /* = Init-Sampler des IM-Pfads (render_shader.c) */
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = am[au]; sd.AddressV = am[av]; sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxAnisotropy = 1;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (au == 0 && av == 0) {
        if (FAILED(ID3D11Device_CreateSamplerState(g_dev, &sd, &g_ta_def))) { g_ta_def = NULL; }
        return g_ta_def;
    }
    if (FAILED(ID3D11Device_CreateSamplerState(g_dev, &sd, &g_ta_cache[au][av]))) {
        g_ta_cache[au][av] = NULL;
    }
    return g_ta_cache[au][av];
}
static unsigned sb_ta_u32(unsigned a)
{
    unsigned v = 0;
    __try { v = *(volatile unsigned *)(uintptr_t)a; }
    __except (EXCEPTION_EXECUTE_HANDLER) { v = 0; }
    return v;
}
/* Waehlt/bindet s0 fuer den alten Draw. Shading-Draws binden selbst
 * (rq_bind/rq_draw) -> danach ist der gebundene Sampler unbekannt. */
static int g_ta_mfx; /* MONITOR-FX (W105): Sampler fuer Wash umgestellt -> naechster Draw stellt zurueck */
static void sb_ta_apply(int has_tex)
{
    ID3D11SamplerState *want = NULL;
    g_ta_smp = NULL;
    if (g_rq_active) { g_ta_bound = (ID3D11SamplerState *)1; return; }
    if (g_mfx_kind == MFX_WASH && has_tex) {
        /* MONITOR-FX (W105): Wash-UVs 0..330 brauchen den Adressmodus des
         * Spiels (FUN_00016d70/174d0/17710 setzen ADDRESSU/V = 1 WRAP vor
         * dem Draw, danach 3 CLAMP) - auch bei tex_addr=0. */
        unsigned au = sb_ta_u32(0x21A8B0u), av = sb_ta_u32(0x21A8B4u);
        if (au > 5u) { au = 0; }
        if (av > 5u) { av = 0; }
        if (au || av) { want = sb_ta_sampler(au, av); }
        g_ta_mfx = 1;
    } else if (g_cfg.tex_addr && has_tex && rq_in_mission()) {
        unsigned au = sb_ta_u32(0x21A8B0u), av = sb_ta_u32(0x21A8B4u);
        if (au > 5u) { au = 0; }
        if (av > 5u) { av = 0; }
        g_ta_hist[au][av]++;
        if (g_cfg.tex_addr >= 2) { if (!au) { au = 1; } if (!av) { av = 1; } }
        if (au || av) { want = sb_ta_sampler(au, av); }
    }
    g_ta_smp = want;
    if (!want) { want = sb_ta_sampler(0, 0); }
    if (want && want != g_ta_bound) {
        ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 1, &want);
        g_ta_bound = want;
    }
}

/* DIAG(TEXADDR,W84, rq_diag>=1): je (VS, Textur, ADDRESSU/V) einmal UV-Bereich
 * des alten Draws + Textur-Dimensionen (max. 80 Zeilen), dazu alle 3000
 * Bilder die Verteilung der Adressmodi (TEXADDRH). */
void rq_dump_srv(ID3D11ShaderResourceView *srv, unsigned xptr); /* render_combiner.c */
static void sb_ta_diag(const im_vertex_t *out, unsigned nout, int rhw)
{
    static unsigned keys[80], nk, lastf, epoch, ndump;
    unsigned au = sb_ta_u32(0x21A8B0u), av = sb_ta_u32(0x21A8B4u);
    unsigned key = (g_im_shader * 16777619u) ^ (sb_tex_xptr() * 2654435761u) ^ (au << 4) ^ av ^ ((unsigned)rhw << 9);
    unsigned i, w = 0, h = 0;
    float mnu = 1e30f, mxu = -1e30f, mnv = 1e30f, mxv = -1e30f;
    if (g_present_no / 3000u != lastf) {
        unsigned a, b;
        char buf[256]; int p = 0;
        lastf = g_present_no / 3000u;
        for (a = 0; a < 6; a++) { for (b = 0; b < 6; b++) {
            if (g_ta_hist[a][b] && p < 220) { p += sprintf_s(buf + p, sizeof buf - p, " %u/%u=%u", a, b, g_ta_hist[a][b]); }
        } }
        if (p) { HLOG_W("TEXADDRH f=%u tex_addr=%d:%s", g_present_no, g_cfg.tex_addr, buf); }
    }
    if (g_present_no / 20000u != epoch) { epoch = g_present_no / 20000u; nk = 0; ndump = 0; } /* je 20000 Bilder neu */
    { unsigned q2; for (q2 = 0; q2 < nout && q2 < 64u; q2++) { if (out[q2].uv[0] < -0.01f || out[q2].uv[0] > 1.01f || out[q2].uv[1] < -0.01f || out[q2].uv[1] > 1.01f) { break; } }
      g_ta_oob = (q2 < nout && q2 < 64u) ? 1 : 0; }
    if (g_cfg.rq_diag >= 3 && !rhw && rtt_active() && (g_cfg.rq_diag >= 4 || nout >= 300u || g_ta_oob || au == 1u || au == 2u || av == 1u || av == 2u)) { /* Welt im RT: grosse Draws / WRAP / UV ausserhalb (rq_diag>=4: alle), je Textur einmal */
        static unsigned wx[160], nwx, wep;
        unsigned xp = sb_tex_xptr(), q;
        if (g_present_no / 20000u != wep) { wep = g_present_no / 20000u; nwx = 0; }
        for (q = 0; q < nwx; q++) { if (wx[q] == xp) { break; } }
        if (q == nwx && nwx < 160u) {
            ID3D11ShaderResourceView *sv = sb_tex_get();
            float a0 = 1e30f, a1 = -1e30f, b0 = 1e30f, b1 = -1e30f;
            wx[nwx++] = xp;
            for (q = 0; q < nout; q++) {
                if (out[q].uv[0] < a0) { a0 = out[q].uv[0]; } if (out[q].uv[0] > a1) { a1 = out[q].uv[0]; }
                if (out[q].uv[1] < b0) { b0 = out[q].uv[1]; } if (out[q].uv[1] > b1) { b1 = out[q].uv[1]; }
            }
            sb_tex_dims(&w, &h);
            HLOG_W("TEXADDRW f=%u vs=0x%X ps=0x%X xp=0x%X %ux%u fmt=0x%08X n=%u uv=[%.3f,%.3f]x[%.3f,%.3f] addr=%u/%u col0=(%.2f,%.2f,%.2f,%.2f)",
                   g_present_no, g_im_shader, g_im_pshader, xp, w, h, sb_ta_u32(xp + 0xCu), nout, a0, a1, b0, b1, au, av, out[0].col[0], out[0].col[1], out[0].col[2], out[0].col[3]);
            if (sv) { rq_dump_srv(sv, xp); }
        }
        if (g_cfg.rq_diag >= 4 && w >= 1024u && h >= 1024u) { /* Boden-Kandidat: Vertices */
            static unsigned nv4, ep4;
            if (g_present_no / 20000u != ep4) { ep4 = g_present_no / 20000u; nv4 = 0; }
            if (nv4 < 12u) {
                char rb[200]; int ro = 0; unsigned k4, j4;
                nv4++;
                sb_tex_dims(&w, &h);
                for (k4 = 0; k4 < g_ta_nraw && k4 < 2u; k4++) {
                    for (j4 = 0; j4 < g_ta_stride && j4 < 32u && ro < 180; j4++) { ro += sprintf_s(rb + ro, sizeof rb - ro, "%02X", g_ta_raw[k4][j4]); }
                    if (ro < 190) { rb[ro++] = '|'; rb[ro] = 0; }
                }
                HLOG_W("TEXADDRG f=%u xp=0x%X n=%u vb=%d uvslot=%d stride=%u raw=%s", g_present_no, xp, nout, g_submit_vb, g_ta_uvslot, g_ta_stride, ro ? rb : "-");
                for (k4 = 1; k4 < (unsigned)IM_SLOTS && k4 < 16u; k4++) {
                    const float *r4 = g_im_buf[0][k4];
                    if (r4[0] != 0.0f || r4[1] != 0.0f || r4[2] != 0.0f || r4[3] != 0.0f) {
                        HLOG_W("TEXADDRG   slot%u v0=(%.5f,%.5f,%.5f,%.5f) v1=(%.5f,%.5f,%.5f,%.5f)", k4, r4[0], r4[1], r4[2], r4[3],
                               g_im_buf[1][k4][0], g_im_buf[1][k4][1], g_im_buf[1][k4][2], g_im_buf[1][k4][3]);
                    }
                }
                for (k4 = 0; k4 < nout && k4 < 6u; k4++) {
                    HLOG_W("TEXADDRG   v%u pos=(%.2f,%.2f,%.2f) uv=(%.5f,%.5f)", k4, out[k4].pos[0], out[k4].pos[1], out[k4].pos[2], out[k4].uv[0], out[k4].uv[1]);
                }
            }
        }
    }
    for (i = 0; i < nk; i++) { if (keys[i] == key) { return; } }
    if (nk >= 80u) { return; }
    keys[nk++] = key;
    if (g_cfg.rq_diag >= 3 && !rhw && nout >= 6u && ndump < 12u) { /* Welt-Draw: Textur + erste Vertices */
        ID3D11ShaderResourceView *sv = sb_tex_get();
        ndump++;
        HLOG_W("TEXADDRV f=%u xp=0x%X v0..5 uv=(%.4f,%.4f)(%.4f,%.4f)(%.4f,%.4f)(%.4f,%.4f)(%.4f,%.4f)(%.4f,%.4f) pos0=(%.1f,%.1f,%.1f) pos1=(%.1f,%.1f,%.1f) pos2=(%.1f,%.1f,%.1f)",
               g_present_no, sb_tex_xptr(), out[0].uv[0], out[0].uv[1], out[1].uv[0], out[1].uv[1], out[2].uv[0], out[2].uv[1],
               out[3].uv[0], out[3].uv[1], out[4].uv[0], out[4].uv[1], out[5].uv[0], out[5].uv[1],
               out[0].pos[0], out[0].pos[1], out[0].pos[2], out[1].pos[0], out[1].pos[1], out[1].pos[2],
               out[2].pos[0], out[2].pos[1], out[2].pos[2]);
        if (sv) { rq_dump_srv(sv, sb_tex_xptr()); }
    }
    for (i = 0; i < nout; i++) {
        if (out[i].uv[0] < mnu) { mnu = out[i].uv[0]; } if (out[i].uv[0] > mxu) { mxu = out[i].uv[0]; }
        if (out[i].uv[1] < mnv) { mnv = out[i].uv[1]; } if (out[i].uv[1] > mxv) { mxv = out[i].uv[1]; }
    }
    sb_tex_dims(&w, &h);
    HLOG_W("TEXADDR f=%u vs=0x%X xp=0x%X %ux%u ps=0x%X rhw=%d addr=%u/%u filt=%u/%u n=%u uv=[%.3f,%.3f]x[%.3f,%.3f] tgt=%s",
           g_present_no, g_im_shader, sb_tex_xptr(), w, h, g_im_pshader, rhw, au, av,
           sb_ta_u32(0x21A8B0u + 3u * 4u), sb_ta_u32(0x21A8B0u + 4u * 4u), nout, mnu, mxu, mnv, mxv,
           rtt_active() ? "RT" : "BB");
}

static void sb_im_submit_body(void);

/* DIAG(IM-PERF,W99, im_fast=2): Selbstpruefung des schnellen Wegs gegen den
 * alten im selben Lauf. a) Dekodierung + Umwandlung: g_imf[i] gegen die
 * Umwandlung der g_im_buf-Slots (Farb-Slot 3, Weiss-Fallback, UV-Slot nach
 * der alten allzero/Slot-2-Regel); b) Expansion: alte exp-Schleifen (mit
 * Kappung) gegen im_fast_expand; c) Interp-Stichproben (Hash ueber die
 * expandierten Vertices gegen die Quell-Abbildung). Zeile IMCHK. */
static unsigned g_ichk_draws, g_ichk_dec, g_ichk_exp, g_ichk_cnt, g_ichk_hash, g_ichk_slot;
static im_vertex_t g_ichk_a[IM_VB_MAX * 3], g_ichk_b[IM_VB_MAX];
static void im_fast_check_decode(unsigned n)
{
    unsigned i, uvs = 9, bad = 0;
    int allzero = 1, any2 = 0;
    for (i = 0; i < n; i++) { if (g_im_buf[i][9][0] != 0.0f || g_im_buf[i][9][1] != 0.0f) { allzero = 0; break; } }
    if (allzero) {
        for (i = 0; i < n; i++) { if (g_im_buf[i][2][0] != 0.0f || g_im_buf[i][2][1] != 0.0f) { any2 = 1; break; } }
        if (any2) { uvs = 2; g_ichk_slot++; }
    }
    for (i = 0; i < n && i < IM_VB_MAX; i++) {
        im_vertex_t r;
        const float *c = g_im_buf[i][g_im_col_slot >= 0 ? g_im_col_slot : 3];
        int zero_fb = (c[3] == 0.0f && c[0] == 0.0f && c[1] == 0.0f && c[2] == 0.0f);
        float cc[4];
        memcpy(r.pos, g_im_buf[i][0], 16);
        if (g_cfg.im_zero_explicit && g_im_col_written) { zero_fb = 0; }
        memcpy(cc, g_imf[i].col, 16);
        if (!g_cfg.im_zero_explicit && cc[3] == 0.0f && cc[0] == 0.0f && cc[1] == 0.0f && cc[2] == 0.0f) {
            cc[0] = cc[1] = cc[2] = cc[3] = 1.0f; /* = Fast-Fallback in sb_im_submit_body */
        }
        if (zero_fb) { r.col[0] = r.col[1] = r.col[2] = r.col[3] = 1.0f; } else { memcpy(r.col, c, 16); }
        r.uv[0] = g_im_buf[i][uvs][0]; r.uv[1] = g_im_buf[i][uvs][1];
        if (memcmp(r.pos, g_imf[i].pos, 16) || memcmp(r.col, cc, 16) || memcmp(r.uv, g_imf[i].uv, 8)) { bad++; }
    }
    if (bad) {
        static unsigned nlog;
        g_ichk_dec++;
        if (nlog < 8) { nlog++; HLOG_W("IMCHK dec f=%u vs=0x%X n=%u bad=%u uvslot=%u", g_present_no, g_im_shader, n, bad, uvs); }
    }
}
static unsigned im_ref_expand(im_vertex_t *ex, im_vertex_t *src, unsigned prim, unsigned n)
{
    unsigned e = 0, i;
    switch (prim) { /* Kopie der alten Schleifen in sb_im_submit_body */
    case 3: if (n > 0 && n + 1 <= IM_VB_MAX) { memcpy(ex, src, n * sizeof *ex); ex[n] = src[0]; e = n + 1; } else { memcpy(ex, src, n * sizeof *ex); e = n; } break;
    case 6: case 7: case 10:
        for (i = 2; i < n && e + 3 <= IM_VB_MAX * 3u; i++) {
            if (prim == 6) { ex[e++] = src[i - 2]; ex[e++] = src[i - 1]; ex[e++] = src[i]; }
            else { ex[e++] = src[0]; ex[e++] = src[i - 1]; ex[e++] = src[i]; }
        }
        break;
    case 8: for (i = 0; i + 3 < n && e + 6 <= IM_VB_MAX * 3u; i += 4) { ex[e++] = src[i]; ex[e++] = src[i + 1]; ex[e++] = src[i + 2]; ex[e++] = src[i]; ex[e++] = src[i + 2]; ex[e++] = src[i + 3]; } break;
    case 9: for (i = 0; i + 3 < n && e + 6 <= IM_VB_MAX * 3u; i += 2) { ex[e++] = src[i]; ex[e++] = src[i + 1]; ex[e++] = src[i + 2]; ex[e++] = src[i]; ex[e++] = src[i + 2]; ex[e++] = src[i + 3]; } break;
    default: memcpy(ex, src, n * sizeof *ex); e = n; break;
    }
    return e > IM_VB_MAX ? IM_VB_MAX : e;
}
static void im_fast_expand(im_vertex_t *dst, const im_vertex_t *src, unsigned prim,
                           unsigned n, unsigned nout);
static void im_fast_check_expand(const im_vertex_t *src, unsigned prim, unsigned n, unsigned nout)
{
    unsigned e = im_ref_expand(g_ichk_a, (im_vertex_t *)src, prim, n), q, h = 2166136261u, i, step;
    g_ichk_draws++;
    if (e != nout) { g_ichk_cnt++; return; }
    im_fast_expand(g_ichk_b, src, prim, n, nout);
    if (memcmp(g_ichk_a, g_ichk_b, (size_t)nout * sizeof *g_ichk_b)) {
        static unsigned nlog;
        g_ichk_exp++;
        if (nlog < 8) { nlog++; HLOG_W("IMCHK exp f=%u prim=%u n=%u nout=%u", g_present_no, prim, n, nout); }
    }
    step = nout > 64u ? nout / 64u : 1u;
    for (i = 0; i < nout; i += step) {
        const unsigned *w = (const unsigned *)src[sb_im_exp_src(prim, n, i)].pos;
        h = (h ^ w[0]) * 16777619u; h = (h ^ w[1]) * 16777619u; h = (h ^ w[2]) * 16777619u; h = (h ^ w[3]) * 16777619u;
    }
    for (q = 0; q < 4u; q++) {
        if (memcmp(g_ichk_a[(nout - 1u) * q / 3u].pos, src[sb_im_exp_src(prim, n, (nout - 1u) * q / 3u)].pos, 16)) { h ^= 1u; }
    }
    if ((h ^ nout) != sb_interp_hash(g_ichk_a, sizeof g_ichk_a[0], nout)) { g_ichk_hash++; }
}
void im_fast_check_present(unsigned f)
{
    if (g_cfg.im_fast != 2 || (f % 300u) != 0u || !g_ichk_draws) { return; }
    HLOG_I("IMCHK f=%u draws=%u dec_diff=%u exp_diff=%u count_diff=%u hash_diff=%u uvslot2=%u",
           f, g_ichk_draws, g_ichk_dec, g_ichk_exp, g_ichk_cnt, g_ichk_hash, g_ichk_slot);
}

/* PERF(IM-PERF,W99): die ersten nout Vertices der Primitiv-Expansion von src
 * (n Quell-Vertices) direkt nach dst (gemappter Puffer, nur schreiben) -
 * dieselbe Reihenfolge wie die exp-Schleifen in sb_im_submit_body. */
static void im_fast_expand(im_vertex_t *dst, const im_vertex_t *src, unsigned prim,
                           unsigned n, unsigned nout)
{
    unsigned e = 0, i;
    switch (prim) {
    case 6:
        for (i = 2; i < n && e + 3u <= nout; i++) {
            dst[e++] = src[i - 2]; dst[e++] = src[i - 1]; dst[e++] = src[i];
        }
        break;
    case 7: case 10:
        for (i = 2; i < n && e + 3u <= nout; i++) {
            dst[e++] = src[0]; dst[e++] = src[i - 1]; dst[e++] = src[i];
        }
        break;
    case 8:
        for (i = 0; i + 3 < n && e + 6u <= nout; i += 4) {
            dst[e++] = src[i]; dst[e++] = src[i + 1]; dst[e++] = src[i + 2];
            dst[e++] = src[i]; dst[e++] = src[i + 2]; dst[e++] = src[i + 3];
        }
        break;
    case 9:
        for (i = 0; i + 3 < n && e + 6u <= nout; i += 2) {
            dst[e++] = src[i]; dst[e++] = src[i + 1]; dst[e++] = src[i + 2];
            dst[e++] = src[i]; dst[e++] = src[i + 2]; dst[e++] = src[i + 3];
        }
        break;
    case 3:
        memcpy(dst, src, (size_t)(nout < n ? nout : n) * sizeof *dst);
        if (nout > n) { dst[n] = src[0]; }
        return;
    default:
        memcpy(dst, src, (size_t)nout * sizeof *dst);
        return;
    }
    for (; e < nout; e++) { dst[e] = src[sb_im_exp_src(prim, n, e)]; } /* Rest bei Kappung */
}

/* DIAG(STARTSEQ-BARS,W98): sbar_probe=x1:y1:x2:y2 -> im Zensus-Frame nach
 * jedem Draw ins BB zwei Pixel lesen (1x1-Staging, synchron, nur Diagnose)
 * und bei Aenderung loggen: welcher Draw faerbt die Status-Check-Balken? */
void sbar_probe_after(int rhw, unsigned nout)
{
    static ID3D11Texture2D *stg;
    static unsigned last[2], lastf;
    ID3D11Resource *res = NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    int k;
    if (rtt_active() || !g_rtv) { return; }
    ID3D11RenderTargetView_GetResource(g_rtv, &res);
    if (!res) { return; }
    if (!stg) {
        D3D11_TEXTURE2D_DESC dd;
        ID3D11Texture2D_GetDesc((ID3D11Texture2D *)res, &dd);
        dd.Width = 1; dd.Height = 1; dd.MipLevels = 1; dd.ArraySize = 1; dd.Usage = D3D11_USAGE_STAGING;
        dd.BindFlags = 0; dd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; dd.MiscFlags = 0;
        dd.SampleDesc.Count = 1; dd.SampleDesc.Quality = 0;
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &dd, NULL, &stg))) { stg = NULL; }
    }
    if (lastf != g_present_no) { lastf = g_present_no; last[0] = last[1] = 0xDEADBEEFu; }
    for (k = 0; k < 2 && stg; k++) {
        D3D11_BOX b;
        b.left = (UINT)g_cfg.sbar_probe[k * 2]; b.top = (UINT)g_cfg.sbar_probe[k * 2 + 1];
        b.right = b.left + 1u; b.bottom = b.top + 1u; b.front = 0; b.back = 1;
        ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)stg, 0, 0, 0, 0, res, 0, &b);
        if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)stg, 0, D3D11_MAP_READ, 0, &m))) {
            unsigned px = *(const unsigned *)m.pData;
            ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)stg, 0);
            if (px != last[k]) {
                unsigned cbon = 0, cbs = 0, cbd = 0;
                sb_render_blend_state(&cbon, &cbs, &cbd);
                HLOG_W("SBAR px f=%u i=%u k=%d (%d,%d) 0x%08X -> 0x%08X xp=0x%X vs=0x%X ps=0x%X rhw=%d n=%u bl=%u/%u/%u lastclear=0x%08X",
                       g_present_no, m3d_ndraw ? m3d_ndraw - 1u : 0u, k, g_cfg.sbar_probe[k * 2], g_cfg.sbar_probe[k * 2 + 1],
                       last[k], px, sb_tex_xptr(), g_im_shader, g_im_pshader, rhw, nout, cbon, cbs, cbd,
                       g_last_clear);
                last[k] = px;
            }
        }
    }
    ID3D11Resource_Release(res);
}
/* DIAG(SHADE-PERF,W80): rq_perf=1 misst die ganze Uebergabe je Draw. */
void sb_im_submit(void)
{
    long long q0;
    int z0 = g_ph_zone; /* DIAG(PERF-TICK,W168) */
    g_ph_zone = 1;
    if (!RQP_ON) { sb_im_submit_body(); mfx_end(); g_ph_zone = z0; return; }
    q0 = rq_qpc();
    sb_im_submit_body();
    mfx_end();
    g_rqp_q[RQP_SUBMIT] += rq_qpc() - q0;
    g_ph_zone = z0;
}
static void sb_im_submit_body(void)
{
    D3D11_MAPPED_SUBRESOURCE map;
    unsigned vb_start = 0;
    static im_vertex_t out[IM_VB_MAX];
    unsigned n = g_im_nverts, nout = 0, i;
    unsigned topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    int uvslot = 9;
    float mvp[16], tmp[16];
    int rhw = 1;
    unsigned sbon = 0, sbs = 5, sbd = 6; /* W2B-FIX Blend-Save */
    int vsh_em = 0, vsh_p2bb = 0; /* VSH2D: 0 = alter Pfad, 1 = P1, 2 = P2 */
    int self_clip = 0; /* SIDESMEAR: 1 = diesen Draw auf Aspect-Box clippen */
    const vsh_prog_t *vsh_p = NULL;
    float vsh_mat[4][4]; float vsh_zadd[4];
    unsigned stride = sizeof(im_vertex_t), offset = 0;
    unsigned ip_raw = 0, ip_tex = 0; /* FPS-INTERP (W69): Eingangs-Hash, has_tex */
    ID3D11Buffer *ip_vb = NULL;      /* FPS-INTERP (W69): Tick-VB statt IM-Ring */
    ID3D11ShaderResourceView *rq_srv0 = NULL; /* RENDER-QA (W77) */
    HRESULT hr;
    /* PERF(IM-PERF,W99): fast = VB-Draw kompakt in g_imf (Positionen dort,
     * Farbe/UV schon wie nach der Umwandlung unten); sonst g_im_buf-Slots. */
    const int fast = g_im_fast;
    float *const pb = fast ? g_imf[0].pos : g_im_buf[0][0];
    const size_t pst = fast ? sizeof(im_vertex_t) / sizeof(float) : (size_t)IM_SLOTS * 4u;
    im_vertex_t *ov = fast ? g_imf : out; /* Vertices vor der Expansion */
    long long imq = 0; /* DIAG(IM-PERF,W99) */
    int fxr_on = 0, fxr_cls = 0; /* W133 REM-FX */
#define IMPOS(i) (pb + (size_t)(i) * pst)
#define IMP_MARK(k) do { if (RQP_FULL) { long long t_ = rq_qpc(); g_imp_q[k] += t_ - imq; imq = t_; } } while (0)
    if (!g_dev || n == 0) {
        return;
    }
    if (RQP_FULL) { imq = rq_qpc(); if (!g_submit_vb) { g_imp_imv += n; } }
    if (fast && g_cfg.im_fast == 2) { im_fast_check_decode(n); } /* DIAG(IM-PERF,W99) */
    if (g_cfg.sbar_diag || g_cfg.sbar_skip_xp) { /* DIAG(STARTSEQ-BARS,W98) */
        unsigned scn = 0, mlp = 0, sx = sb_tex_xptr();
        __try { scn = *(volatile unsigned *)(uintptr_t)M3D_SCENE_ADDR; mlp = *(volatile unsigned *)(uintptr_t)M3D_MLOOP_ADDR; }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (scn == 0x2Cu && mlp == 1u) {
            if (g_cfg.sbar_diag && sx) { /* jede neue Stage-0-Textur einmal als BMP */
                static unsigned sb_seen[256], sb_n;
                unsigned sq;
                for (sq = 0; sq < sb_n && sb_seen[sq] != sx; sq++) {}
                if (sq == sb_n && sb_n < 256u) {
                    ID3D11ShaderResourceView *ssrv = sb_tex_get();
                    sb_seen[sb_n++] = sx;
                    HLOG_W("SBAR tex f=%u xp=0x%X vs=0x%X ps=0x%X n=%u srv=%u", g_present_no, sx, g_im_shader, g_im_pshader, n, ssrv ? 1u : 0u);
                    if (ssrv) { rq_dump_srv(ssrv, sx); }
                }
            }
            if (g_cfg.sbar_skip_xp && sx == g_cfg.sbar_skip_xp) { return; }
        }
    }
    /* MONITOR-FX (W105): Korn-Wash / Stoer-Zeilen der Monitore erkennen und
     * auf Original-Semantik bringen (render_mfx.c); 0 = Draw auslassen. */
    if (mfx_classify(g_im_prim, n, (g_submit_vb || fast) ? 1 : 0) != MFX_NONE) {
        if (!mfx_apply(g_im_buf, &n, &g_im_prim, IM_MAX_VERTS < IM_VB_MAX ? IM_MAX_VERTS : IM_VB_MAX)) { return; }
    }
    if (g_ip_recording) { ip_raw = fast ? sb_interp_hash(g_imf, sizeof g_imf[0], n) : sb_interp_hash(g_im_buf, sizeof g_im_buf[0], n); }
    /* SIDESMEAR (W45): Selbst-Sampling -> vor Draw auf Box clippen (s.o.).
     * Erkennung immer loggen (gedeckelt); Clip nur bei Schalter + Pillarbox
     * (aspect_mode=0 streckt gewollt Vollbild). */
    if (g_mfx_kind != MFX_LINES && rtt_self_sample()) {
        g_rtt_self_n++;
        self_clip = (g_cfg.rtt_selfclip && g_cfg.aspect_mode && !g_cfg.rtt_selfcopy) ? 1 : 0; /* W106: mit rtt_selfcopy bleibt der Draw im RT */
        if (g_rtt_self_n <= 12u || (g_rtt_self_n % 1000u) == 0u) {
            HLOG_W("RTT self #%u: f=%u xp=0x%X slot=%d %ux%u prim=%u n=%u%s",
                   g_rtt_self_n, g_present_no, sb_tex_xptr(), g_rtt_bound,
                   g_rtt[g_rtt_bound].w, g_rtt[g_rtt_bound].h, g_im_prim, n,
                   g_cfg.rtt_selfcopy ? " (Kopie, Ziel bleibt)" : self_clip ? " (clip)" : " (alt: Vollbild)");
        }
    }
    /* RTT-Zaehler steht nach Draw (s.u., nur ausgefuehrte Draws). */
    g_fix_blend = 0;
    g_rtt_selfread = -1; /* W106 rtt_selfcopy: Lese-Markierung je Draw */
    if (!g_im_ready) {
        sb_im_init();
        if (!g_im_ready) {
            return;
        }
    }
    /* RENDER-QA (W77): Shading-Pfad - Eingaenge vor allen Positions-
     * Umrechnungen packen (shading=0: g_rq_active bleibt 0). */
    g_rq_active = 0;
    if (g_cfg.dlg_text_upload) { rq_text_flush(); } /* FIX(DLGTEXT): CPU-Text einmal hochladen */
    if (g_cfg.shading) {
        long long q0 = RQP_FULL ? rq_qpc() : 0;
        rq_begin(g_submit_vb ? (g_rq_vb_ok ? (const float (*)[16][4])rq_vb_vin() : NULL)
                             : (const float (*)[16][4])g_im_buf, n, g_submit_vb);
        if (RQP_FULL) { g_rqp_q[RQP_BEGIN] += rq_qpc() - q0; }
    }
    IMP_MARK(IMP_P0); /* DIAG(IM-PERF,W99): Hash, Selbst-Sampling, Shading-Begin */
    /* Pfad-Heuristik: alle Positionen in Pixel-Range + w==1 -> RHW.
     * PERF(IM-PERF,W99): fast + force3d -> Ergebnis wird direkt danach auf
     * 0 gesetzt, Schleife ueberspringen (3D-Objektkoordinaten liegen oft
     * ganz im Pixel-Bereich -> lief bisher ueber alle Vertices). */
    if (fast && g_im_force3d) { rhw = 0; } else
    for (i = 0; i < n; i++) {
        const float *pp = IMPOS(i);
        float x = pp[0], y = pp[1];
        float z = pp[2], w = pp[3];
        if (x < -64.0f || x > 704.0f || y < -64.0f || y > 544.0f ||
            z < -1.0f || z > 2.0f || w < 0.999f || w > 1.001f) {
            rhw = 0;
            break;
        }
    }
    if (g_im_force3d) { rhw = 0; } /* VB: FLOAT3/4 -> 3D */
    /* VSH2D (W38): P1/P2-Programme emulieren (exakte oPos statt Heuristik).
     * Matrixfehler -> alter Pfad (kein degenerierter Draw). */
    if (g_cfg.vsh_emul && g_im_shader) {
        vsh_p = vsh_program(g_im_shader);
        if (vsh_p && vsh_p->kind == 1) { vsh_em = 1; }
        else if (vsh_p && vsh_p->kind == 2 && vsh_read_mat(vsh_p, vsh_mat, vsh_zadd)) { vsh_em = 2; }
    }
    if (g_cfg.fx_diag) { /* DIAG(FX-ORIG,W90): Rohpositionen + VSH-Matrix fuer FXKEY */
        unsigned q;
        g_fx_raw_n = n; g_fx_vsh_em = vsh_em; g_fx_vsh_kind = vsh_p ? vsh_p->kind : -1;
        for (q = 0; q < 4u; q++) { memcpy(g_fx_raw[q], g_im_buf[q < n ? q : n - 1][0], 16); }
        g_fx_slotmask = 0; g_fx_vb = g_submit_vb;
        for (q = 0; q < 16u; q++) {
            if (g_im_buf[0][q][0] != 0.0f || g_im_buf[0][q][1] != 0.0f || g_im_buf[0][q][2] != 0.0f || g_im_buf[0][q][3] != 0.0f) { g_fx_slotmask |= 1u << q; }
        }
        memcpy(g_fx_s3, g_im_buf[0][3], 16); memcpy(g_fx_s9, g_im_buf[0][9], 16); memcpy(g_fx_s2, g_im_buf[0][2], 16);
        if (vsh_em == 2) { memcpy(g_fx_mat, vsh_mat, sizeof g_fx_mat); } else { memset(g_fx_mat, 0, sizeof g_fx_mat); }
    }
    /* W2B-FIX (lin_rhw_range): linearer VB-Quad/Strip in Pixel-Range ist
     * ein UI-Quad (Movie!), kein 3D: RHW statt degeneriertem 3D-MVP.
     * Z/W-Test greift hier nicht (Spiel normalisiert UI-Z/W anders).
     * Prefetch fuellt Cache VOR Check+Norm (heilt Early-Miss). */
    if (!vsh_em && g_cfg.lin_rhw_range && !rhw && g_submit_vb &&
        (g_im_prim == 4 || g_im_prim == 6)) {
        unsigned q; int inrange = 1;
        for (q = 0; inrange && q < n; q++) {
            float x = IMPOS(q)[0], y = IMPOS(q)[1];
            if (x < -64.0f || x > 704.0f || y < -64.0f || y > 544.0f) {
                inrange = 0;
            }
        }
        if (inrange) {
            unsigned ffmt = 0; int ftlin = 0;
            static unsigned nfix;
            sb_tex_get(); /* Prefetch: Cache fuellen */
            sb_tex_fmt(&ffmt, &ftlin);
            if (ftlin) {
                unsigned r;
                rhw = 1;
                g_fix_blend = 1;
                /* W2B-FIX: RHW braucht w=1 (Offset!), Spiel liefert w=0. */
                for (r = 0; r < n; r++) {
                    IMPOS(r)[2] = 0.0f;
                    IMPOS(r)[3] = 1.0f;
                }
                if (nfix < 3) {
                    nfix++;
                    HLOG_W("TEX lin-rhw-range #%u: xptr=0x%X f=%u (VB->RHW)",
                           nfix, sb_tex_xptr(), g_present_no);
                }
            }
        }
    }
    /* W133 REM-FX: Effekt-Draws erkennen (Rohpositionen, VS-Matrix); alle
     * Optionen aus -> fxr_active() = 0, nichts geaendert. */
    fxr_on = fxr_active();
    if (fxr_on) { fxr_cls = fxr_pre(vsh_em, (const float (*)[4])vsh_mat, vsh_zadd, pb, pst, n, rhw, g_im_prim); }
    /* VSH2D: Positionen emulieren (P1 = Pixel, P2 = Clip/Tail). */
    if (vsh_em == 1) {
        for (i = 0; i < n; i++) {
            IMPOS(i)[3] = 1.0f;
        }
        rhw = 1;
    } else if (vsh_em == 2) {
        if (rtt_active()) {
            /* RT: Xbox-exakte Screenspace-Pixel (Tail mit Spiel-Viewport). */
            float vpw = (float)(g_vp_game.w ? g_vp_game.w : 640);
            float vph = (float)(g_vp_game.h ? g_vp_game.h : 480);
            float scx = vpw * 0.5f, scy = -vph * 0.5f;
            float scz = g_vp_game.maxz - g_vp_game.minz;
            float ofx = vpw * 0.5f + (float)g_vp_game.x;
            float ofy = vph * 0.5f + (float)g_vp_game.y;
            float ofz = g_vp_game.minz;
            int eye_clip = 0;
            unsigned flash_blend, flash_src, flash_dst;
            sb_render_blend_state(&flash_blend, &flash_src, &flash_dst);
            /* W194 M3-FLASH: Additive P2-Effekte koennen die Augenebene schneiden.
             * Vorzeitiges /w mit anschliessendem w=1 faltet deren Dreiecke
             * auf die sichtbare Seite (additive Blitze brennen dann aus).
             * Fuer diesen Draw homogene Pixelkoordinaten bis zum Rasterizer
             * erhalten. Andere Blendpfade und vollstaendig vordere Draws
             * bleiben bitgleich (keine allgemeine P2-Umstellung). */
            /* W224 LINE-FLICKER: Dieselbe Faltung trifft auch nicht-additive
             * P2-Draws (lange Linie quer ueber den Hauptmonitor in M4).
             * eye_clip_all=1 (Option, Standard 0): fuer jeden Blendmodus. Die
             * flackernde Linie war ein Zwischenbild-Fehler (interp_eyefix). */
            int eye_any = g_cfg.eye_clip_all ||
                          (flash_blend && flash_src == 2u && flash_dst == 2u);
            for (i = 0; (eye_any || g_cfg.eyeclip_diag > 0) && i < n; i++) {
                const float *pp = IMPOS(i);
                float cw = pp[0] * vsh_mat[3][0] + pp[1] * vsh_mat[3][1] +
                           pp[2] * vsh_mat[3][2] + pp[3] * vsh_mat[3][3];
                if (cw <= 0.0f) { eye_clip = eye_any ? 1 : 2; break; }
            }
            if (eye_clip && g_cfg.eyeclip_diag > 0) { /* DIAG(W224) */
                static int ecd_n;
                if (ecd_n < g_cfg.eyeclip_diag) {
                    float wmin = 1e30f, wmax = -1e30f;
                    unsigned nneg = 0, q;
                    for (q = 0; q < n; q++) {
                        const float *pp = IMPOS(q);
                        float cw = pp[0] * vsh_mat[3][0] + pp[1] * vsh_mat[3][1] +
                                   pp[2] * vsh_mat[3][2] + pp[3] * vsh_mat[3][3];
                        if (cw < wmin) { wmin = cw; }
                        if (cw > wmax) { wmax = cw; }
                        if (cw <= 0.0f) { nneg++; }
                    }
                    ecd_n++;
                    HLOG_W("EYECLIP f=%u fix=%d blend=%u src=%u dst=%u prim=%u n=%u neg=%u w=%.3f..%.3f vs=0x%X",
                           g_present_no, eye_clip == 1, flash_blend, flash_src, flash_dst,
                           g_im_prim, n, nneg, wmin, wmax, g_im_shader);
                }
                if (eye_clip == 2) { eye_clip = 0; }
            }
            for (i = 0; i < n; i++) {
                float *pp = IMPOS(i);
                float x = pp[0], y = pp[1];
                float z = pp[2], w = pp[3];
                float cx = x * vsh_mat[0][0] + y * vsh_mat[0][1] + z * vsh_mat[0][2] + w * vsh_mat[0][3];
                float cy = x * vsh_mat[1][0] + y * vsh_mat[1][1] + z * vsh_mat[1][2] + w * vsh_mat[1][3];
                float cz = x * vsh_mat[2][0] + y * vsh_mat[2][1] + z * vsh_mat[2][2] + w * vsh_mat[2][3] + vsh_zadd[2];
                float cw = x * vsh_mat[3][0] + y * vsh_mat[3][1] + z * vsh_mat[3][2] + w * vsh_mat[3][3];
                if (eye_clip) {
                    pp[0] = cx * scx + ofx * cw;
                    pp[1] = cy * scy + ofy * cw;
                    pp[2] = (scz != 0.0f) ? cz : 0.0f;
                    pp[3] = cw;
                } else {
                    float rw = vsh_rcc(cw);
                    pp[0] = cx * scx * rw + ofx;
                    pp[1] = cy * scy * rw + ofy;
                    pp[2] = (scz != 0.0f) ? (cz * rw) : 0.0f; /* inverser Viewport: (pz-ofz)/scz */
                    pp[3] = 1.0f;
                }
            }
            rhw = 1;
        } else {
            /* BB: Clip + Hor+ weiter unten (exakt wie m3d_vs_mvp). */
            for (i = 0; i < n; i++) {
                float *pp = IMPOS(i);
                float x = pp[0], y = pp[1];
                float z = pp[2], w = pp[3];
                pp[0] = x * vsh_mat[0][0] + y * vsh_mat[0][1] + z * vsh_mat[0][2] + w * vsh_mat[0][3];
                pp[1] = x * vsh_mat[1][0] + y * vsh_mat[1][1] + z * vsh_mat[1][2] + w * vsh_mat[1][3];
                pp[2] = x * vsh_mat[2][0] + y * vsh_mat[2][1] + z * vsh_mat[2][2] + w * vsh_mat[2][3] + vsh_zadd[2];
                pp[3] = x * vsh_mat[3][0] + y * vsh_mat[3][1] + z * vsh_mat[3][2] + w * vsh_mat[3][3];
            }
            rhw = 0;
            vsh_p2bb = 1;
        }
    }
    if (rhw) {
        /* Pixel -> NDC (Spiel-Viewport als Referenz). ASPECT (W18): in
         * die zentrierte Spiel-Aspect-Box statt Vollbild (nie strecken). */
        float vw = (float)(g_vp_game.w ? g_vp_game.w : 640);
        /* RTT: Offscreen-Textur 1:1 (Aspect-Box steht weiter unten still). */
        if (rtt_active()) {
            unsigned rtdw = 0, rtdh = 0;
            rtt_cur_dims(&rtdw, &rtdh);
            if (rtdw) { vw = (float)rtdw; }
        }
        float vh = (float)(g_vp_game.h ? g_vp_game.h : 480);
        if (rtt_active()) { unsigned rtdw2 = 0, rtdh2 = 0; rtt_cur_dims(&rtdw2, &rtdh2); if (rtdh2) { vh = (float)rtdh2; } }
        float nL = -1.0f, nR = 1.0f, nT = 1.0f, nB = -1.0f;
        if (g_cfg.aspect_mode && !rtt_active()) {
            /* Feste 640x480-Referenz (RHW = Framebuffer-Pixel, s.o.). */
            vw = 640.0f; vh = 480.0f;
            unsigned bx, by, bw, bh;
            sb_aspect_box(&bx, &by, &bw, &bh);
            nL = (float)bx / (float)g_bb_w * 2.0f - 1.0f;
            nR = (float)(bx + bw) / (float)g_bb_w * 2.0f - 1.0f;
            nT = 1.0f - (float)by / (float)g_bb_h * 2.0f;
            nB = 1.0f - (float)(by + bh) / (float)g_bb_h * 2.0f;
        }
        memset(mvp, 0, sizeof mvp);
        mvp[0] = (nR - nL) / vw;
        mvp[5] = (nB - nT) / vh;
        mvp[10] = 1.0f;
        mvp[12] = nL;
        mvp[13] = nT;
        mvp[15] = 1.0f;
        g_im_rhw++;
    } else {
        /* 3D: World[0]*View*Proj(Hor+, Auge 0). Das Spiel setzt World nie
         * per SetTransform (steckt in den VS-Konstanten). */
        sb_mat4_mul(tmp, g_world[0].m, g_view.m);
        /* MISSION3D (W25): m3d_vs_mvp=1 -> MVP aus VS-Konstanten c98-c101
         * (pro-Draw-WorldViewProj des Spiels, column-major, homogen
         * skaliert). Hor+: x-Spalte mit k (4:3-Basis, VS-Proj enthaelt
         * 4:3-Aspect 1.8107); ins RT 1:1 ohne Hor+ (W23-Regel). */
        if (g_cfg.m3d_vs_mvp && m3d_vs_mvp_read(tmp)) {
            if (!rtt_active()) {
                float oa = (float)g_bb_w / (float)(g_bb_h ? g_bb_h : 1);
                float vk = (4.0f / 3.0f) / (oa > 0.0f ? oa : 1.0f);
                tmp[0] *= vk; tmp[4] *= vk; tmp[8] *= vk; tmp[12] *= vk;
            }
            memcpy(mvp, tmp, sizeof mvp);
        } else {
        /* RTT: originalgetreu ohne Hor+ (Spiel komponiert fuers Target). */
        if (rtt_active()) { sb_mat4_mul(mvp, tmp, g_proj_game.m); }
        else { sb_mat4_mul(mvp, tmp, g_rig[0].proj.m); }
        }
        if (vsh_p2bb) {
            /* VSH2D: Clip liegt vor (DP4), nur Hor+ (x-Spalte, 4:3-Basis). */
            float oa = (float)g_bb_w / (float)(g_bb_h ? g_bb_h : 1);
            float vk = (4.0f / 3.0f) / (oa > 0.0f ? oa : 1.0f);
            memset(mvp, 0, sizeof mvp);
            mvp[0] = vk; mvp[5] = 1.0f; mvp[10] = 1.0f; mvp[15] = 1.0f;
        } else if (g_cfg.rt_vp_clip && rtt_active()) {
            /* FIX(WORLDVIEW,W65): Clip-Raum ins RT ueber den SPIEL-Viewport
             * abbilden (Xbox: Viewport-Tail c-38/c-37), nicht ueber das
             * ganze RT (D3D11-Viewport = RT-Groesse, s. sb_rtt_on_set_target).
             * Hauptmonitor-Welt: RT31 512x512 mit Viewport 512x422 – P2-Draws
             * (Pixel per Tail) lagen richtig, die Farbpaesse der Licht-
             * Programme (alter Pfad) um 512/422 vertikal gestreckt (Census
             * W65_c1: gleiche Geometrie P2 ndcy [-2.17,-0.69] vs. alt
             * [-2.84,-1.05]; A/B W65_ab0: Welt im Monitor um ~20 % nach unten
             * verschoben). Voller Viewport -> Identitaet (keine Aenderung). */
            float rv[16];
            if (rt_vp_matrix(rv)) {
                memcpy(tmp, mvp, sizeof tmp);
                sb_mat4_mul(mvp, tmp, rv);
            }
        }
        g_im_3d++;
    }
    /* W191 UW-PERSP: Cockpit-Perspektive (nur BB, Cockpit-Ansicht; Standard
     * aus -> sb_cp_frame() = 0 ohne Speicherzugriff, MVP/Vertices unveraendert). */
    if (!rtt_active() && sb_cp_frame()) {
        sb_cp_draw(mvp, vsh_p2bb ? 1 : (rhw ? 2 : 0), rhw ? NULL : pb, pst, n, g_im_prim);
    }
    if (g_cfg.shadow_diag) { extern float g_im_diag_mvp[16]; memcpy(g_im_diag_mvp, mvp, sizeof g_im_diag_mvp); } /* DIAG(SHADOWBUF,W127) */
    IMP_MARK(IMP_P1); /* DIAG(IM-PERF,W99): RHW-Heuristik, VSH-Emulation, MVP */
    /* UV-Slot: 9 (TEXCOORD0), Fallback 2 (Shader mappt frei).
     * PERF(IM-PERF,W99): VB-Draws fuellen Slot 2 nie (memset) -> immer 9. */
    if (!fast) {
        int allzero = 1;
        for (i = 0; i < n; i++) {
            if (g_im_buf[i][9][0] != 0.0f ||
                g_im_buf[i][9][1] != 0.0f) {
                allzero = 0;
                break;
            }
        }
        if (allzero) {
            int any2 = 0;
            for (i = 0; i < n; i++) {
                if (g_im_buf[i][2][0] != 0.0f ||
                    g_im_buf[i][2][1] != 0.0f) {
                    any2 = 1;
                    break;
                }
            }
            if (any2) {
                uvslot = 2;
            }
        }
    }
    /* FIX(FX-ORIG,W90): IM-Draw mit Transform-Programm (P2): UV aus dem
     * oT0-Quellregister des Programms (Effekte: MOV oT0, v3) statt fest
     * Slot 9 / Fallback 2 (dort steht die Farbe v2 -> Textur an falscher
     * Stelle gesampelt, Effekte als Einfarb-Kleckse). */
    if (g_cfg.fx_im_uv && !g_submit_vb && g_im_shader) {
        const vsh_prog_t *vt = vsh_program(g_im_shader);
        if (vt && vt->kind == 2 && vt->ot0 > 0 && vt->ot0 < IM_SLOTS) { uvslot = vt->ot0; }
    }
    /* FIX(TERRAIN-UV,W84): IM-Draws der Licht-Programme (Boden-Raster der
     * Mission, oT0 = v3 per ILU-MOV) nahmen Slot 9/Fallback 2 = Vertexfarbe v2
     * als UV (Werte k/255 -> Bodentextur entlang der Farbverlaeufe gestreckt,
     * "Marmor/Wellen", Nutzer-Playtest 3.11.). Jetzt das oT0-Quellregister. */
    if (g_cfg.im_uv_ot0 && !g_submit_vb && g_im_shader >= 0x10000u) {
        const vsh_prog_t *vt = vsh_program(g_im_shader);
        int src = vt ? (vt->ot0 > 0 ? vt->ot0 : vt->ot0i) : -1;
        if (src > 0 && src < IM_SLOTS) { uvslot = src; }
    }
    g_ta_uvslot = uvslot; /* DIAG(TEXADDR,W84) */
    /* Verts konvertieren (pos=Slot0, col=Color-Slot/weiss, uv=Slot).
     * PERF(IM-PERF,W99): fast -> g_imf ist schon umgewandelt (VB: Slot 3,
     * Farbe explizit, uv Slot 9); nur der Weiss-Fallback fuer (0,0,0,0) bei
     * im_zero_explicit=0 (mon_color_default greift bei VB nie: written=1). */
    if (fast) {
        if (!g_cfg.im_zero_explicit) {
            for (i = 0; i < n && i < IM_VB_MAX; i++) {
                if (ov[i].col[3] == 0.0f && ov[i].col[0] == 0.0f && ov[i].col[1] == 0.0f && ov[i].col[2] == 0.0f) {
                    ov[i].col[0] = ov[i].col[1] = ov[i].col[2] = ov[i].col[3] = 1.0f;
                }
            }
        }
    } else
    for (i = 0; i < n && i < IM_VB_MAX; i++) {
        int c, zero_fb;
        out[i].pos[0] = g_im_buf[i][0][0];
        out[i].pos[1] = g_im_buf[i][0][1];
        out[i].pos[2] = g_im_buf[i][0][2];
        out[i].pos[3] = g_im_buf[i][0][3];
        c = (g_im_col_slot >= 0) ? g_im_col_slot : 3;
        /* W50 WASH (vsh_color): IM-Draw ohne Color-Call nimmt Diffuse aus
         * dem oD0-Quellregister seines VSH-Programms (P1/P2-Comps: v1,
         * belegt per VSH-Dump + IM-Reg-Trace) statt stale Slot 3 (VB-Slot).
         * Explizite Color-Calls und VB-Draws unveraendert (alter Pfad). */
        if (g_im_col_slot < 0 && !g_submit_vb && g_cfg.vsh_color && g_im_shader) {
            const vsh_prog_t *vc = vsh_program(g_im_shader);
            if (vc && vc->od0 >= 0 && vc->od0 < IM_SLOTS) { c = vc->od0; }
        }
        if (c >= IM_SLOTS) {
            c = 3;
        }
        /* Leerer Slot (nie geschrieben) -> weiss (alle 0 + a=0 -> a=1?).
         * FIX(TITLEBG2,W54): mit Color-Call (written) ist (0,0,0,0) eine
         * explizite transparente Farbe (Titel-Overlay, Vollbild, geblendet),
         * kein leerer Slot: kein Weiss-Fallback (Schalter im_zero_explicit).
         * Ohne Color-Call (stale/leer, od0-Pfad) unveraendert. */
        zero_fb = (g_im_buf[i][c][3] == 0.0f && g_im_buf[i][c][0] == 0.0f &&
            g_im_buf[i][c][1] == 0.0f && g_im_buf[i][c][2] == 0.0f);
        if (g_cfg.im_zero_explicit && g_im_col_written) { zero_fb = 0; }
        if (zero_fb || (g_cfg.mon_color_default && !g_im_col_written && (g_im_buf[i][c][0] > 1.0f || g_im_buf[i][c][1] > 1.0f || g_im_buf[i][c][2] > 1.0f))) {
            out[i].col[0] = out[i].col[1] = out[i].col[2] = 1.0f;
            out[i].col[3] = 1.0f;
        } else {
            /* Sonst: Slot-Inhalt uebernehmen. */
            out[i].col[0] = g_im_buf[i][c][0];
            out[i].col[1] = g_im_buf[i][c][1];
            out[i].col[2] = g_im_buf[i][c][2];
            out[i].col[3] = g_im_buf[i][c][3];
        }
        out[i].uv[0] = g_im_buf[i][uvslot][0];
        out[i].uv[1] = g_im_buf[i][uvslot][1];
    }
    n = (n > IM_VB_MAX) ? IM_VB_MAX : n;
    /* FIX(FX-ORIG,W90): Effekt-PS R0 = (V0 + C0) * T0 im Standardpfad (C0 je
     * Vertex addieren, geklemmt wie der Combiner); Shading-Pfad rechnet selbst. */
    if (g_cfg.fx_ps_c0 && !g_rq_active && g_im_pshader) {
        float fc[4];
        if (fx_ps_add_c0(fc)) {
            unsigned q, k2;
            for (q = 0; q < n; q++) {
                for (k2 = 0; k2 < 4u; k2++) {
                    float v = ov[q].col[k2] + fc[k2];
                    ov[q].col[k2] = (v > 1.0f) ? 1.0f : (v < 0.0f ? 0.0f : v);
                }
            }
        }
    }
    /* M2c: Pixel-UVs (RHW/UI) normalisieren. Manche Shader schreiben
     * Texcoords in Pixeln (0..w,0..h, z.B. Lade-BG 0..640); der Xbox-Shader
     * skaliert per Konstante (macht unser Fixed-Function-Ersatz nicht).
     * Nur RHW + max|uv|>1.5 (3D-Tiling bleibt roh).
     * A2 (lin_tex_rect): NV2A adressiert LINEARtexturen (texture
     * rectangle) mit Texel-Koords, auch als 3D-Draw (Bink-Quad:
     * UV -0.5..511.5 bei rhw=0, ehem. MOVIE-Zensus). Dann
     * /(tw,th) auch ohne RHW; DXT-3D-Tiling bleibt roh. */
    { int do_norm = rhw;
      if (!do_norm && g_cfg.lin_tex_rect) {
          unsigned tf = 0; int tlin = 0;
          sb_tex_fmt(&tf, &tlin);
          do_norm = tlin;
      }
      if (g_mfx_nonorm) { do_norm = 0; } /* MONITOR-FX (W105): Wash-UVs sind normiert (swizzled, WRAP) */
      if (do_norm) {
        unsigned tw = 0, th = 0;
        float umax = 0.0f;
        sb_tex_dims(&tw, &th);
        if ((tw == 0 || th == 0) && g_cfg.lin_tex_rect) {
            /* Erster Draw: Cache noch leer -> Upload vorziehen,
             * sonst fehlen die Dims und die Norm entfaellt. */
            sb_tex_get();
            sb_tex_dims(&tw, &th);
        }
        if ((tw == 0 || th == 0) && g_cfg.lin_rhw_range) {
            /* W2B-FIX: Header-Fallback (kein Cache-Timing mehr). */
            unsigned hw = 0, hh = 0;
            if (sb_tex_hdr_dims(sb_tex_xptr(), &hw, &hh) && hw && hh) {
                tw = hw; th = hh;
            }
        }
        if (tw > 0 && th > 0) {
            for (i = 0; i < n; i++) {
                float a = (ov[i].uv[0] < 0) ? -ov[i].uv[0] : ov[i].uv[0];
                float b = (ov[i].uv[1] < 0) ? -ov[i].uv[1] : ov[i].uv[1];
                if (a > umax) { umax = a; }
                if (b > umax) { umax = b; }
            }
            if (umax > 1.5f) {
                static unsigned nnorm;
                for (i = 0; i < n; i++) {
                    ov[i].uv[0] /= (float)tw;
                    ov[i].uv[1] /= (float)th;
                }
                if (nnorm < 3) {
                    nnorm++;
                    HLOG_I("UV norm #%u: /%ux%u (umax=%.1f prim=%u n=%u)",
                           nnorm, tw, th, umax, g_im_prim, n);
                }
            }
        }
      }
    }
    IMP_MARK(IMP_P2); /* DIAG(IM-PERF,W99): UV-Slot, Umwandlung, C0, UV-Norm */
    /* Primitiv-Expansion (Xbox -> D3D11). Cull ist aus (Winding egal). */
    {
        static im_vertex_t exp[IM_VB_MAX * 3];
        unsigned e = 0;
        switch (g_im_prim) {
        case 1: /* POINTLIST */
            topo = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
            e = n;
            break;
        case 2: /* LINELIST */
            topo = D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
            e = n;
            break;
        case 3: /* LINELOOP -> STRIP + Schluss */
            topo = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
            if (n > 0 && n + 1 <= IM_VB_MAX) {
                if (!fast) { out[n] = out[0]; } /* PERF(IM-PERF,W99): fast schreibt in im_fast_expand */
                e = n + 1;
            } else {
                e = n;
            }
            break;
        case 4: /* LINESTRIP */
            topo = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
            e = n;
            break;
        case 6: /* TRIANGLESTRIP -> LIST */
        case 7: /* TRIANGLEFAN -> LIST */
        case 10: /* POLYGON -> wie FAN */
            if (fast) { e = (n > 2u) ? (n - 2u) * 3u : 0u; break; } /* PERF(IM-PERF,W99): nur zaehlen */
            for (i = 2; i < n && e + 3 <= sizeof exp / sizeof exp[0]; i++) {
                if (g_im_prim == 6) {
                    exp[e++] = out[i - 2];
                    exp[e++] = out[i - 1];
                    exp[e++] = out[i];
                } else {
                    exp[e++] = out[0];
                    exp[e++] = out[i - 1];
                    exp[e++] = out[i];
                }
            }
            memcpy(out, exp, e * sizeof out[0]);
            break;
        case 8: /* QUADLIST -> LIST */
            if (fast) { e = (n / 4u) * 6u; break; } /* PERF(IM-PERF,W99) */
            for (i = 0; i + 3 < n &&
                 e + 6 <= sizeof exp / sizeof exp[0];
                 i += 4) {
                exp[e++] = out[i];
                exp[e++] = out[i + 1];
                exp[e++] = out[i + 2];
                exp[e++] = out[i];
                exp[e++] = out[i + 2];
                exp[e++] = out[i + 3];
            }
            memcpy(out, exp, e * sizeof out[0]);
            break;
        case 9: /* QUADSTRIP -> LIST */
            if (fast) { e = (n >= 4u) ? ((n - 4u) / 2u + 1u) * 6u : 0u; break; } /* PERF(IM-PERF,W99) */
            for (i = 0; i + 3 < n &&
                 e + 6 <= sizeof exp / sizeof exp[0];
                 i += 2) {
                exp[e++] = out[i];
                exp[e++] = out[i + 1];
                exp[e++] = out[i + 2];
                exp[e++] = out[i];
                exp[e++] = out[i + 2];
                exp[e++] = out[i + 3];
            }
            memcpy(out, exp, e * sizeof out[0]);
            break;
        default: /* 5 TRIANGLELIST + unbekannt: direkt */
            e = n;
            break;
        }
        /* Clamp (out fasst IM_VB_MAX; Draws sind klein, greift nie). */
        if (e > IM_VB_MAX) {
            e = IM_VB_MAX;
        }
        nout = e;
    }
    if (nout == 0) {
        g_rq_active = 0;
        return;
    }
    if (g_rq_active) { /* RENDER-QA: Stream 2 */
        long long q0 = RQP_FULL ? rq_qpc() : 0;
        rq_expand_upload(g_im_prim, n, nout);
        if (RQP_FULL) { g_rqp_q[RQP_EXP] += rq_qpc() - q0; }
    }
    if (RQP_FULL) { g_rqp_draws++; g_rqp_vin += n; g_rqp_vout += nout; if (g_rq_active) { g_rqp_shaded++; } }
    /* Upload + Draw. IMRING (31.10.): VB als Ring mit NO_OVERWRITE, DISCARD
     * nur beim Umlauf. Vorher DISCARD des ganzen 3-MB-VB pro Draw -> der
     * Treiber muss pro Draw umbenennen; bei Draw-Stuermen (bootFCS1: ~82k
     * Draws/Frame) blockierte Map bis 116 ms -> 1 Frame/35 s. */
    IMP_MARK(IMP_PRE); /* DIAG(IM-PERF,W99) */
    if (fast && g_cfg.im_fast == 2) { im_fast_check_expand(ov, g_im_prim, n, nout); } /* DIAG(IM-PERF,W99) */
    if (fast) { /* PERF(IM-PERF,W99): Expansion direkt in Tick-VB bzw. IM-Ring */
        im_vertex_t *dst = NULL;
        if (g_ip_recording) { dst = sb_interp_vb_begin(nout, &ip_vb, &vb_start); }
        if (dst) {
            im_fast_expand(dst, ov, g_im_prim, n, nout);
            sb_interp_vb_end(nout);
        } else {
            ip_vb = NULL;
            if (g_cfg.im_ring && g_im_vb_pos + nout <= IM_VB_MAX) {
                hr = ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_im_vb, 0,
                                             g_im_vb_pos ? D3D11_MAP_WRITE_NO_OVERWRITE
                                                         : D3D11_MAP_WRITE_DISCARD,
                                             0, &map);
            } else {
                g_im_vb_pos = 0;
                hr = ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_im_vb, 0,
                                             D3D11_MAP_WRITE_DISCARD, 0, &map);
            }
            if (FAILED(hr)) {
                g_rq_active = 0;
                return;
            }
            vb_start = g_im_vb_pos;
            im_fast_expand((im_vertex_t *)map.pData + vb_start, ov, g_im_prim, n, nout);
            ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_im_vb, 0);
            g_im_vb_pos = g_cfg.im_ring ? vb_start + nout : 0;
        }
    } else
    if (g_ip_recording && sb_interp_vb_write(out, nout, &ip_vb, &vb_start)) {
        /* FPS-INTERP (W69): Vertices liegen im Tick-VB (Wiedergabe ohne Kopie). */
    } else {
    ip_vb = NULL;
    if (g_cfg.im_ring && g_im_vb_pos + nout <= IM_VB_MAX) {
        hr = ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_im_vb, 0,
                                     g_im_vb_pos ? D3D11_MAP_WRITE_NO_OVERWRITE
                                                 : D3D11_MAP_WRITE_DISCARD,
                                     0, &map);
    } else {
        g_im_vb_pos = 0;
        hr = ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_im_vb, 0,
                                     D3D11_MAP_WRITE_DISCARD, 0, &map);
    }
    if (FAILED(hr)) {
        g_rq_active = 0;
        return;
    }
    vb_start = g_im_vb_pos;
    memcpy((im_vertex_t *)map.pData + vb_start, out, nout * sizeof out[0]);
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_im_vb, 0);
    g_im_vb_pos = g_cfg.im_ring ? vb_start + nout : 0;
    }
    IMP_MARK(IMP_UP); /* DIAG(IM-PERF,W99) */
    /* DIAG(DRAWSTORM): normal ~170 Draws/Frame in der Mission; >20000 ohne
     * Present = Sturm. Caller + Draw-Form loggen (Ursache offen). */
    if (++g_im_frame_draws == 20000u) {
        static unsigned nstorm;
        extern unsigned g_im_end_caller;
        if (nstorm < 4) {
            nstorm++;
            HLOG_W("DRAWSTORM #%u: 20000 Draws seit Present #%u, caller=0x%X "
                   "prim=%u n=%u rhw=%d tex=0x%X",
                   nstorm, g_present_no, g_im_end_caller, g_im_prim, nout,
                   rhw, sb_tex_xptr());
        }
    }
    {
        struct ID3D11ShaderResourceView *srv = sb_tex_get();
        unsigned has_tex = (srv != NULL) ? 1u : 0u;
        /* FIX(WORLDVIEW,W65): Pixel-Shader ohne Textur-Stufe 0
         * (PSTextureModes Stufe 0 = NONE) sampelt auf der Xbox nichts –
         * die noch gebundene Stage-0-Textur ist ein Rest des vorigen Draws.
         * Beleg: Zellenkarte (RT 128x128, PS 0xAB80400 = reine Vertexfarbe,
         * texmodes=0) wurde mit einem wechselnden Fremd-Texel multipliziert
         * -> Sichtzellen falsch -> Gelaende/Objekte nur im 1. Frame nach
         * START gezeichnet (W65_d3/rb0, BERICHT W65). */
        if (has_tex && ps_stage0_none()) {
            srv = NULL;
            has_tex = 0u;
        }
        if (g_cfg.bloom_diag && !srv && sb_tex_xptr() && rtt_active() && rq_in_mission() && !ps_stage0_none()) {
            /* DIAG(SHADE-BLOOM,W112): Stufe 0 belegt, aber keine SRV (Shading: T0 = 1) */
            static unsigned nx[64], nn;
            unsigned q, xp0 = sb_tex_xptr(), sxp[4], snn[4], h[5] = { 0, 0, 0, 0, 0 };
            for (q = 0; q < nn && nx[q] != xp0; q++) {}
            if (q == nn && nn < 64u) {
                nx[nn++] = xp0;
                sb_tex_stages(sxp, snn);
                __try { for (q = 0; q < 5u; q++) { h[q] = *(unsigned *)(uintptr_t)(xp0 + q * 4u); } }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
                HLOG_W("BLOOM notex f=%u xp=0x%X hdr=[%08X %08X %08X %08X %08X] why=%u sh=%d vs=0x%X ps=0x%X vb=%d prim=%u n=%u rhw=%d tgt=%d st=0x%X/0x%X/0x%X/0x%X",
                       g_present_no, xp0, h[0], h[1], h[2], h[3], h[4], sb_tex_why(), g_rq_active, g_im_shader,
                       g_im_pshader, g_submit_vb, g_im_prim, n, rhw, g_rtt_bound, sxp[0], sxp[1], sxp[2], sxp[3]);
            }
        }
        g_sub_xptr = sb_tex_xptr(); g_sub_tex = has_tex;
        rq_srv0 = srv; /* RENDER-QA: Stufe 0 wie bisher bestimmt */
        if (g_cfg.tex_addr || g_mfx_kind == MFX_WASH || g_ta_mfx) { /* FIX(TEXADDR,W84) / MONITOR-FX (W105) */
            if (g_mfx_kind != MFX_WASH) { g_ta_mfx = 0; }
            sb_ta_apply((int)has_tex);
        }
        if (g_cfg.rq_diag && has_tex && rq_in_mission()) { sb_ta_diag(out, nout, rhw); } /* DIAG(TEXADDR,W84) */
        ip_tex = has_tex;
        if (has_tex) {
            g_im_texd++;
        } else {
            g_im_notex++;
        }
        /* SHADE-PERF (W80): Shading-Draws binden t0..t3 in rq_bind selbst */
        if (!(g_rq_active && g_cfg.shade_gpuvb)) { ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, &srv); }
        { static LARGE_INTEGER qpf;
          LARGE_INTEGER qc0, qc1;
          static unsigned nslow;
          if (!qpf.QuadPart) { QueryPerformanceFrequency(&qpf); }
          QueryPerformanceCounter(&qc0);
        hr = ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_im_cb, 0,
                                     D3D11_MAP_WRITE_DISCARD, 0, &map);
          QueryPerformanceCounter(&qc1);
          if ((qc1.QuadPart - qc0.QuadPart) * 1000 > qpf.QuadPart * 50 &&
              nslow < 8) {
              nslow++;
              HLOG_W("IM-SLOW #%u: MapCB %lldms", nslow,
                     (qc1.QuadPart - qc0.QuadPart) * 1000 / qpf.QuadPart);
          }
        }
        if (FAILED(hr)) {
            g_rq_active = 0;
            return;
        }
        memcpy(map.pData, mvp, sizeof mvp);
        /* FIX(FX-ORIG,W90): Alpha-Test im has_tex-Wort (Bit 0 bleibt has_tex,
         * die Interp-Wiedergabe uebernimmt das Wort unveraendert). */
        if (g_cfg.fx_alphatest && fx_nv(0x300u, 0u)) {
            unsigned af = fx_nv(0x33Cu, 0x207u) & 0xFFFFu;
            if (af >= 0x200u && af <= 0x206u) {
                has_tex |= ((af - 0x1FFu) << 8) | ((fx_nv(0x340u, 0u) & 0xFFu) << 16);
                ip_tex = has_tex;
            }
        }
        /* FIX(STARTSEQ-BARS,W98): Xbox D3DTSS_ALPHAKILL der Stufe 0
         * (Deferred-TextureState 0x21A8B0 + 11*4, D3DTALPHAKILL_ENABLE = 4).
         * Das Spiel setzt es je Missions-Frame (FUN_00045F60) und kodiert
         * z. B. die Fuellung der Status-Check-Balken nur im Texel-Alpha
         * (FUN_00092710). RT-Texturen ausgenommen (bei uns immer BGRA,
         * X8-RTs haetten sonst Alpha 0 statt 1). */
        if (g_cfg.fx_alphakill && (has_tex & 1u) && !sb_tex_rtt_hit()) {
            unsigned ak = 0, scn = 0, mlp = 0;
            __try {
                ak = *(volatile unsigned *)(uintptr_t)0x21A8DCu;
                scn = *(volatile unsigned *)(uintptr_t)M3D_SCENE_ADDR;
                mlp = *(volatile unsigned *)(uintptr_t)M3D_MLOOP_ADDR;
            } __except (EXCEPTION_EXECUTE_HANDLER) { ak = 0; }
            /* 1 = nur Mission (Szene 0x2C, mloop=1; Menues/Filme bleiben
             * bitgenau), 2 = ueberall (der Zustand bleibt nach einer Mission
             * stehen). */
            if (g_cfg.fx_alphakill < 2 && !(scn == 0x2Cu && mlp == 1u)) { ak = 0; }
            if (ak & 4u) { has_tex |= 0x1000000u; ip_tex = has_tex; }
        }
        if (fxr_cls) { unsigned fb = fxr_draw_bits(); if (fb) { has_tex |= fb; ip_tex = has_tex; } } /* W133 REM-FX: weicher Partikel */
        *(unsigned *)((unsigned char *)map.pData + 64) = has_tex;
        { LARGE_INTEGER qu0, qu1;
          LARGE_INTEGER qpf2;
          QueryPerformanceFrequency(&qpf2);
          QueryPerformanceCounter(&qu0);
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_im_cb, 0);
          QueryPerformanceCounter(&qu1);
          if ((qu1.QuadPart - qu0.QuadPart) * 1000 > qpf2.QuadPart * 50) {
              HLOG_W("IM-SLOW: UnmapCB %lldms (nout=%u)",
                     (qu1.QuadPart - qu0.QuadPart) * 1000 / qpf2.QuadPart,
                     nout);
          }
        }
    }
    IMP_MARK(IMP_CB); /* DIAG(IM-PERF,W99): Textur + Konstanten */

    if (g_fix_blend) { /* W2B-FIX: scoped Blend-Off (Bink-Alpha=0). */
        sb_render_blend_state(&sbon, &sbs, &sbd);
        sb_render_blend(27, 0);
    }
    /* COLORMASK (W33): Blend-Maske + DepthStencil aus Spiel-States;
     * rs_*=0 nimmt exakt den alten Pfad (RHW->off, sonst on). */
    rs_apply_states(rhw);
    g_im_submits++;
    /* SHADE-PERF (W80): Shading-Draws (shade_gpuvb=1) setzen Layout/Stream 0/
     * VS/PS/t0 in rq_bind/rq_draw selbst -> hier nicht doppelt (bei
     * Fehlschlag von rq_bind unten nachgeholt). */
    if (!(g_rq_active && g_cfg.shade_gpuvb)) {
        ID3D11DeviceContext_IASetInputLayout(g_ctx, g_im_layout);
        ID3D11DeviceContext_IASetVertexBuffers(g_ctx, 0, 1, ip_vb ? &ip_vb : &g_im_vb, &stride,
                                               &offset);
    }
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, topo);
    if (!(g_rq_active && g_cfg.shade_gpuvb)) { ID3D11DeviceContext_VSSetShader(g_ctx, g_im_vs, NULL, 0); }
    ID3D11DeviceContext_VSSetConstantBuffers(g_ctx, 0, 1, &g_im_cb);
    if (!(g_rq_active && g_cfg.shade_gpuvb)) { ID3D11DeviceContext_PSSetShader(g_ctx, g_im_ps, NULL, 0); }
    ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 0, 1, &g_im_cb);
    if (g_rq_active) { /* RENDER-QA: generierte VS/PS + Stufen 0..3 */
        long long q0 = RQP_FULL ? rq_qpc() : 0;
        int lean = g_cfg.shade_gpuvb;
        if (!rq_bind(rq_srv0, rhw) && lean) { /* alter Pfad: Zustand nachholen */
            ID3D11DeviceContext_IASetInputLayout(g_ctx, g_im_layout);
            ID3D11DeviceContext_IASetVertexBuffers(g_ctx, 0, 1, ip_vb ? &ip_vb : &g_im_vb, &stride, &offset);
            ID3D11DeviceContext_VSSetShader(g_ctx, g_im_vs, NULL, 0);
            ID3D11DeviceContext_PSSetShader(g_ctx, g_im_ps, NULL, 0);
            ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, &rq_srv0);
        }
        if (!g_rq_active && (g_cfg.tex_addr || g_mfx_kind == MFX_WASH)) { sb_ta_apply(rq_srv0 != NULL); } /* FIX(TEXADDR,W84): Fallback alter Pfad */
        if (RQP_FULL) { g_rqp_q[RQP_BIND] += rq_qpc() - q0; }
    }
    if (g_cfg.sbar_diag > 1 && nout == (unsigned)g_cfg.sbar_diag && sb_tex_xptr()) {
        /* DIAG(STARTSEQ-BARS,W98): Balken-Textur alle 150 Frames zwangsweise
         * neu laden + dumpen (beschreibt das Spiel die Texel zur Laufzeit?). */
        static unsigned sb_lastf, sb_pend;
        unsigned sx = sb_tex_xptr();
        if (sb_pend && g_present_no != sb_lastf) {
            char nm[32];
            ID3D11ShaderResourceView *ssrv = sb_tex_get(); /* dirty -> Neu-Upload */
            sb_pend = 0;
            _snprintf_s(nm, sizeof nm, _TRUNCATE, "_f%u", g_present_no);
            HLOG_W("SBAR texdump f=%u xp=0x%X srv=%u", g_present_no, sx, ssrv ? 1u : 0u);
            if (ssrv) { rq_dump_srv_named(ssrv, sx, nm); }
            sb_lastf = g_present_no;
        } else if (!sb_pend && g_present_no - sb_lastf >= 150u) {
            sb_tex_mark_dirty(sx);
            sb_pend = 1; sb_lastf = g_present_no;
        }
    }
    /* MISSION3D-Zensus (W25, DIAG): NDC-Bereich + Zustand je Draw,
     * Pixel per Occlusion-Query (Ergebnis in M3D-pix-Zeilen). */
    { int m3d_qs = 0;
    if (m3d_on && m3d_nskip < (unsigned)g_cfg.m3d_census_skip) { m3d_nskip++; m3d_ndraw++; } /* DIAG(W98) */
    else if (m3d_on && m3d_ndraw < M3D_MAXDRAWS + (unsigned)g_cfg.m3d_census_skip) {
        float mnx = 1e30f, mxx = -1e30f, mny = 1e30f, mxy = -1e30f;
        float mnz = 1e30f, mxz = -1e30f;
        float mpx2 = 1e30f, mpxx2 = -1e30f, mpy2 = 1e30f, mpyx2 = -1e30f;
        float mnu2 = 1e30f, mxu2 = -1e30f, mnv2 = 1e30f, mxv2 = -1e30f;
        unsigned wpos = 0, wneg = 0, wzero = 0, inside = 0, kk2;
        unsigned cbon2, cbs2, cbd2;
        char tgt2[40];
        struct ID3D11ShaderResourceView *csrv2;
        unsigned t3_fmt = 0; int t3_lin = 0;
        unsigned t3_xp[4], t3_n[4];
        for (kk2 = 0; kk2 < nout; kk2++) {
            float X2 = out[kk2].pos[0]*mvp[0]+out[kk2].pos[1]*mvp[4]+out[kk2].pos[2]*mvp[8]+out[kk2].pos[3]*mvp[12];
            float Y2 = out[kk2].pos[0]*mvp[1]+out[kk2].pos[1]*mvp[5]+out[kk2].pos[2]*mvp[9]+out[kk2].pos[3]*mvp[13];
            float Z2 = out[kk2].pos[0]*mvp[2]+out[kk2].pos[1]*mvp[6]+out[kk2].pos[2]*mvp[10]+out[kk2].pos[3]*mvp[14];
            float W2 = out[kk2].pos[0]*mvp[3]+out[kk2].pos[1]*mvp[7]+out[kk2].pos[2]*mvp[11]+out[kk2].pos[3]*mvp[15];
            if (out[kk2].pos[0] < mpx2) { mpx2 = out[kk2].pos[0]; }
            if (out[kk2].pos[0] > mpxx2) { mpxx2 = out[kk2].pos[0]; }
            if (out[kk2].pos[1] < mpy2) { mpy2 = out[kk2].pos[1]; }
            if (out[kk2].pos[1] > mpyx2) { mpyx2 = out[kk2].pos[1]; }
            if (out[kk2].uv[0] < mnu2) { mnu2 = out[kk2].uv[0]; }
            if (out[kk2].uv[0] > mxu2) { mxu2 = out[kk2].uv[0]; }
            if (out[kk2].uv[1] < mnv2) { mnv2 = out[kk2].uv[1]; }
            if (out[kk2].uv[1] > mxv2) { mxv2 = out[kk2].uv[1]; }
            if (W2 > 1e-9f) {
                float nx2 = X2 / W2, ny2 = Y2 / W2, nz2 = Z2 / W2;
                wpos++;
                if (nx2 < mnx) { mnx = nx2; } if (nx2 > mxx) { mxx = nx2; }
                if (ny2 < mny) { mny = ny2; } if (ny2 > mxy) { mxy = ny2; }
                if (nz2 < mnz) { mnz = nz2; } if (nz2 > mxz) { mxz = nz2; }
                if (nx2 >= -1.0f && nx2 <= 1.0f && ny2 >= -1.0f && ny2 <= 1.0f && nz2 >= 0.0f && nz2 <= 1.0f) { inside++; }
            } else if (W2 < -1e-9f) { wneg++; } else { wzero++; }
        }
        if (!wpos) { mnx = mxx = mny = mxy = mnz = mxz = 0.0f; }
        csrv2 = sb_tex_get();
        if (g_cfg.sbar_diag > 1 && nout == (unsigned)g_cfg.sbar_diag) { /* DIAG(STARTSEQ-BARS,W98): Vertex-Dump */
            extern unsigned g_rq_psconst[8], g_rq_psconst_set;
            unsigned vq;
            HLOG_W("SBAR vdump f=%u i=%u xp=0x%X vs=0x%X ps=0x%X n=%u psc0=0x%08X psc1=0x%08X set=0x%X",
                   g_present_no, m3d_ndraw, sb_tex_xptr(), g_im_shader, g_im_pshader, nout,
                   g_rq_psconst[0], g_rq_psconst[1], g_rq_psconst_set);
            for (vq = 0; vq < nout; vq++) {
                HLOG_W("SBAR v f=%u i=%u k=%u pos=%.4f,%.4f,%.4f,%.4f col=%.3f,%.3f,%.3f,%.3f uv=%.4f,%.4f",
                       g_present_no, m3d_ndraw, vq, out[vq].pos[0], out[vq].pos[1], out[vq].pos[2], out[vq].pos[3],
                       out[vq].col[0], out[vq].col[1], out[vq].col[2], out[vq].col[3], out[vq].uv[0], out[vq].uv[1]);
            }
        }
        sb_tex_fmt(&t3_fmt, &t3_lin);
        sb_tex_stages(t3_xp, t3_n);
        sb_render_blend_state(&cbon2, &cbs2, &cbd2);
        if (rtt_active()) {
            unsigned rw2 = 0, rh2 = 0;
            rtt_cur_dims(&rw2, &rh2);
            sprintf_s(tgt2, sizeof tgt2, "RT%d-%ux%u", g_rtt_bound, rw2, rh2);
        } else {
            sprintf_s(tgt2, sizeof tgt2, "BB-%ux%u", g_bb_w, g_bb_h);
        }
        HLOG_W("M3D draw f=%u i=%u xp=0x%X %s p=%u n=%u rhw=%d vs=0x%X x=[%.1f,%.1f] y=[%.1f,%.1f] ndcx=[%.2f,%.2f] ndcy=[%.2f,%.2f] ndcz=[%.2f,%.2f] w=%u/%u/%u in=%u col=(%.2f,%.2f,%.2f,%.2f) uv=[%.2f,%.2f]x[%.2f,%.2f] tex=%u bl=%u/%u/%u tgt=%s vp=%ux%u ds=%s zen=%u xfm=0x%X why=%u rtt=%u s1=0x%X s2=0x%X s3=0x%X ps=0x%X cm=0x%X dm=%u se=%u sf=0x%X sx=0x%X st=%u",
               g_present_no, m3d_ndraw, sb_tex_xptr(), g_submit_vb ? "VB" : "IM", g_im_prim, nout, rhw, g_im_shader,
               mpx2, mpxx2, mpy2, mpyx2, mnx, mxx, mny, mxy, mnz, mxz, wpos, wneg, wzero, inside,
               out[0].col[0], out[0].col[1], out[0].col[2], out[0].col[3],
               mnu2, mxu2, mnv2, mxv2, csrv2 ? 1u : 0u, cbon2, cbs2, cbd2, tgt2,
               g_vp_game.w, g_vp_game.h, rhw ? "off" : "on", m3d_zenable,
               t3_fmt, sb_tex_why(), sb_tex_rtt_hit(), t3_xp[1], t3_xp[2], t3_xp[3], g_im_pshader, rs_cmask, rs_dmask, rs_st_enable, rs_st_func, g_sub_xptr, g_sub_tex);
        /* DIAG(FRONT-HANGAR,W101): Spiel-Tiefenfunktion + Polygon-Offset je Draw */
        HLOG_W("M3D zst f=%u i=%u zf=0x%X zen=0x%X poe=0x%X/0x%X/0x%X pof=0x%X pob=0x%X",
               g_present_no, m3d_ndraw, fx_nv(0x354u, 0u), fx_nv(0x30Cu, 0u),
               fx_nv(0x338u, 0u), fx_nv(0x334u, 0u), fx_nv(0x330u, 0u),
               fx_nv(0x384u, 0u), fx_nv(0x388u, 0u));
        /* VSH-Dump (vsh_dump): Stage0-Datenzeiger je Draw (Alias-Flips
         * des Comp-Xptr sichtbar machen): nur Frames #1+#2. */
        if (g_cfg.vsh_dump && m3d_nframes <= 2u) {
            __try {
                unsigned xxp = sb_tex_xptr();
                unsigned xdata = xxp ? *(unsigned *)(uintptr_t)(xxp + 4u) : 0u;
                HLOG_W("M3D xpdata f=%u i=%u xp=0x%X data=0x%X rtt=%u",
                       g_present_no, m3d_ndraw, xxp, xdata, sb_tex_rtt_hit());
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        /* DIAG(TEX3D,W31): VS-/PS-Structs je Handle einmal (Combiner-Pfad:
         * sampelt die Weiss-PS eine Textur? Lighting-VS?). SEH-geschuetzt. */
        { static unsigned seen_vs3[16], seen_ps3[16];
          unsigned q3, fnd3;
          for (q3 = 0, fnd3 = 0; q3 < 16u; q3++) {
              if (seen_vs3[q3] == g_im_shader) { fnd3 = 1; break; }
              if (!seen_vs3[q3]) { break; }
          }
          if (!fnd3 && q3 < 16u && g_im_shader) {
              unsigned base3 = g_im_shader & ~1u;
              seen_vs3[q3] = g_im_shader;
              __try {
                  unsigned *hd3 = (unsigned *)(uintptr_t)base3;
                  int s3;
                  HLOG_W("TEX3D vs=0x%X: ref=%u flags=0x%X progsize=%u dwords=%u",
                         g_im_shader, hd3[0], hd3[1], hd3[2], hd3[3]);
                  for (s3 = 0; s3 < 8; s3++) {
                      unsigned *sl3 = (unsigned *)(uintptr_t)(base3 + 0x14u + (unsigned)s3 * 16u);
                      HLOG_W("TEX3D vs=0x%X slot[%d]: stream=%u off=%u fmt=0x%02X",
                             g_im_shader, s3, sl3[0], sl3[1], sl3[2] & 0xFFu);
                  }
              } __except (EXCEPTION_EXECUTE_HANDLER) {
                  HLOG_W("TEX3D vs=0x%X: Struct nicht lesbar", g_im_shader);
              }
          }
          for (q3 = 0, fnd3 = 0; q3 < 16u; q3++) {
              if (seen_ps3[q3] == g_im_pshader) { fnd3 = 1; break; }
              if (!seen_ps3[q3]) { break; }
          }
          if (!fnd3 && q3 < 16u && g_im_pshader) {
              unsigned base3 = g_im_pshader & ~1u;
              seen_ps3[q3] = g_im_pshader;
              __try {
                  unsigned *hd3 = (unsigned *)(uintptr_t)base3;
                  unsigned pdef = hd3[2];
                  HLOG_W("TEX3D ps=0x%X: raw=[%08X %08X %08X %08X %08X %08X %08X %08X]",
                         g_im_pshader, hd3[0], hd3[1], hd3[2], hd3[3],
                         hd3[4], hd3[5], hd3[6], hd3[7]);
                  if (pdef >= 0x10000u && pdef < 0x80000000u) {
                      unsigned *pd = (unsigned *)(uintptr_t)pdef;
                      HLOG_W("TEX3D ps=0x%X def=0x%X: combiner=%u texmodes=0x%08X rgbin0=0x%08X finalABCD=0x%08X inputTex=0x%X",
                             g_im_pshader, pdef, pd[0xD4u / 4u], pd[0xD8u / 4u],
                             pd[0x88u / 4u], pd[0x20u / 4u], pd[0xE0u / 4u]);
                  } else {
                      HLOG_W("TEX3D ps=0x%X: kein pPSDef (direkt?)", g_im_pshader);
                  }
              } __except (EXCEPTION_EXECUTE_HANDLER) {
                  HLOG_W("TEX3D ps=0x%X: Struct nicht lesbar", g_im_pshader);
              }
          }
        }
        /* MISSION3D: Pro-3D-Draw Konstanten-Fenster c88-c107 (nur Frames
         * #1+#2): Wo steht die WorldViewProj-Matrix pro Objekt? */
        if ((!rhw || g_cfg.vsh_dump) && m3d_nframes <= 2u) {
            __try {
                unsigned vr2;
                for (vr2 = 88u; vr2 < 108u; vr2 += 4u) {
                    const float *w0 = (const float *)(uintptr_t)(0x21BCA8u + vr2 * 16u);
                    const float *w1 = (const float *)(uintptr_t)(0x21BCA8u + (vr2 + 1u) * 16u);
                    const float *w2 = (const float *)(uintptr_t)(0x21BCA8u + (vr2 + 2u) * 16u);
                    const float *w3 = (const float *)(uintptr_t)(0x21BCA8u + (vr2 + 3u) * 16u);
                    HLOG_W("M3D vcw f=%u i=%u r=%u: %.4f %.4f %.4f %.4f ; %.4f %.4f %.4f %.4f ; %.4f %.4f %.4f %.4f ; %.4f %.4f %.4f %.4f",
                           g_present_no, m3d_ndraw, vr2,
                           w0[0], w0[1], w0[2], w0[3], w1[0], w1[1], w1[2], w1[3],
                           w2[0], w2[1], w2[2], w2[3], w3[0], w3[1], w3[2], w3[3]);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        /* VSH-Dump (vsh_dump): Viewport-/Material-Konstanten je Draw
         * (c-38/-37 = Spiegel 58/59, c42 = Spiegel 138): nur Frames #1+#2. */
        if (g_cfg.vsh_dump && m3d_nframes <= 2u) {
            __try {
                const float *vm38 = (const float *)(uintptr_t)(0x21BCA8u + 58u * 16u);
                const float *vm37 = (const float *)(uintptr_t)(0x21BCA8u + 59u * 16u);
                const float *vc42 = (const float *)(uintptr_t)(0x21BCA8u + 138u * 16u);
                HLOG_W("M3D vsh f=%u i=%u vs=0x%X: m38=[%.4f %.4f %.4f %.4f] m37=[%.4f %.4f %.4f %.4f] c42=[%.4f %.4f %.4f %.4f]",
                       g_present_no, m3d_ndraw, g_im_shader,
                       vm38[0], vm38[1], vm38[2], vm38[3], vm37[0], vm37[1], vm37[2], vm37[3],
                       vc42[0], vc42[1], vc42[2], vc42[3]);
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        if (m3d_nq < M3D_MAXQ) {
            if (!m3d_q[m3d_nq]) {
                D3D11_QUERY_DESC qd2;
                qd2.Query = D3D11_QUERY_OCCLUSION; qd2.MiscFlags = 0;
                if (FAILED(ID3D11Device_CreateQuery(g_dev, &qd2, &m3d_q[m3d_nq]))) { m3d_q[m3d_nq] = NULL; }
            }
            if (m3d_q[m3d_nq]) {
                ID3D11DeviceContext_Begin(g_ctx, (ID3D11Asynchronous *)m3d_q[m3d_nq]);
                m3d_qdraw[m3d_nq] = m3d_ndraw;
                m3d_nq++;
                m3d_qs = 1;
            }
        }
        m3d_ndraw++;
    }
    if (g_cfg.rq_diag) { rq_census_draw(rhw, nout); rq_dlg_draw(out, nout, rhw, rq_srv0, g_im_prim); } /* DIAG(RENDER-QA,W77) */
    /* SIDESMEAR: Selbst-Sampling-Draw auf Aspect-Box clippen. */
    if (self_clip) { sb_selfclip_begin(); }
    IMP_MARK(IMP_ST); /* DIAG(IM-PERF,W99): Zustaende + Diagnosen */
    if (g_ip_recording) { /* FPS-INTERP (W69): Draw samt Zustand aufzeichnen */
        long long q0 = RQP_ON ? rq_qpc() : 0;
        g_phud_in_cap = 1; /* W125: Zwischenbilder hier stecken in cap */
        g_ip_sigparts[0] = g_im_shader; /* DIAG(FPS-INTERP-2) */
        g_ip_sigparts[1] = sb_tex_xptr();
        g_ip_sigparts[2] = g_im_pshader;
        if (fast) { /* PERF(IM-PERF,W99): Stichproben aus den Quell-Vertices */
            sb_interp_rec_draw_src(ov, g_im_prim, n, nout, topo, mvp, ip_tex, (!rhw || vsh_em == 2) ? 1 : 0,
                                   (g_im_shader * 16777619u) ^ (sb_tex_xptr() * 2654435761u) ^
                                       (g_im_pshader * 40503u) ^ g_im_prim,
                                   ip_raw, ip_vb ? vb_start : 0xFFFFFFFFu);
        } else
        sb_interp_rec_draw(out, nout, topo, mvp, ip_tex, (!rhw || vsh_em == 2) ? 1 : 0,
                           (g_im_shader * 16777619u) ^ (sb_tex_xptr() * 2654435761u) ^
                               (g_im_pshader * 40503u) ^ g_im_prim,
                           ip_raw, ip_vb ? vb_start : 0xFFFFFFFFu);
        g_phud_in_cap = 0;
        if (RQP_ON) { g_rqp_q[RQP_CAP] += rq_qpc() - q0; }
    }
    { long long q0 = RQP_ON ? rq_qpc() : 0;
    int fxq = g_cfg.fx_diag ? fx_draw_pre(out, nout, rhw, g_im_prim, mvp) : 0; /* DIAG(FX-ORIG,W90) */
    if (g_cfg.bloom_diag) { bloom_diag_pre(out, nout, n, rhw, g_im_prim, rq_srv0); } /* DIAG(SHADE-BLOOM,W112) */
    { extern int g_visq_on; if (g_visq_on) { extern void sb_visq_draw_note(void); sb_visq_draw_note(); } } /* FIX(SHADE-M2,W117) vis_query: Draws im Sichtbarkeitstest */
    if (fxr_cls) { fxr_bind(); } else if (fxr_on) { fxr_note_other(); } /* W133 REM-FX: Tiefen-Kopie, Effekt-PS */
    if (g_rq_active) { rq_draw(ip_vb ? ip_vb : g_im_vb, nout, vb_start); } /* RENDER-QA */
    else { WD_CALL("draw_im", g_ctx, ID3D11DeviceContext_Draw(g_ctx, nout, vb_start)); }
    if (fxr_cls) { fxr_after(fast ? ov : out, fast ? n : nout); } /* W133 REM-FX: Glut/Licht, Energie */
    if (g_cfg.bloom_diag) { bloom_diag_post(); } /* DIAG(SHADE-BLOOM,W112) */
    fx_draw_post(fxq); /* DIAG(FX-ORIG,W90) */
    if (RQP_ON) { g_rqp_q[RQP_DRAW] += rq_qpc() - q0; } } /* DIAG(SHADE-PERF,W80): auch alter Pfad */
    if (g_cfg.sbar_probe[0] && m3d_on) { sbar_probe_after(rhw, nout); } /* DIAG(STARTSEQ-BARS,W98) */
    if (self_clip) { sb_selfclip_end(); }
    if (g_ta_mfx && !g_cfg.tex_addr && !g_rq_active) { /* MONITOR-FX (W105): Wash-Sampler sofort zuruecksetzen (Init-CLAMP) */
        ID3D11SamplerState *d0 = sb_ta_sampler(0, 0);
        if (d0) { ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 1, &d0); g_ta_bound = d0; }
        g_ta_smp = NULL;
        g_ta_mfx = 0;
    }
    if (m3d_qs) { ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)m3d_q[m3d_nq - 1]); }
    /* DIAG(WORLDVIEW,W65): wv_diag=2 -> grosses RT (>= 512 px)
     * nach jedem Farb-Draw (cm != 0, kein Tiefenvorpass) im Zensus-Frame
     * dumpen (Aufbau der Welt im RT31 Draw fuer Draw; Kappe wie MONDUMP). */
    if (g_cfg.wv_diag >= 2 && m3d_on && rtt_active() && rs_cmask != 0u &&
        g_rtt[g_rtt_bound].w >= 512u) {
        static unsigned wvf, wvk; /* eigener Zaehler: m3d_ndraw steht ab 400 still */
        if (wvf != g_present_no) { wvf = g_present_no; wvk = 0; }
        wvk++;
        HLOG_W("WVDUMP f=%u k=%u i=%u slot=%d vs=0x%X xp=0x%X n=%u",
               g_present_no, wvk, m3d_ndraw, g_rtt_bound, g_im_shader, sb_tex_xptr(), nout);
        mon_dump_slot(g_rtt_bound, g_present_no, 500u + wvk, 0);
    }
    }
    rtt_note_draw(g_im_prim, nout, rhw, sb_tex_xptr());
    if (g_cfg.tex_dump) { sb_rem_note_draw(rhw, sb_tex_xptr()); } /* DIAG(REM-FRAME,W103) */
    if (g_fix_blend) { /* W2B-FIX: Blend-Restore (kein Leck in UI/HUD). */
        sb_render_blend(27, (int)sbon);
        sb_render_blend(19, (int)sbs);
        sb_render_blend(20, (int)sbd);
        g_fix_blend = 0;
    }
    if (topo == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST) {
        g_im_tris += nout / 3;
    }
}
