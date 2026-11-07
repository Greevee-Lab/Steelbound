/* render_bloom.c - DIAG(SHADE-BLOOM,W112): Bloom-Kette des Hauptmonitors
 * (Filter 0xAB800D0/0xAB801E0 -> 64/20/6-RTs -> Komposition 0xAB80950)
 * Draw fuer Draw belegen.
 *
 * [render] bloom_diag=1 (nur Kampagnen-Mission, Modus 4) bzw. 2 (jede
 * Mission inkl. Titel-Demo): in Diag-Bildern (Screenshot-Bilder ab
 * bloom_frame, mit shade_ab=1 auch das Partnerbild +1; hoechstens
 * bloom_max Bilder, Abstand >= bloom_every) fuer jeden Draw, der ein RT
 * liest, in ein kleines RT (<= 200 px) schreibt oder einen PS mit >= 3
 * Combiner-Stufen hat:
 *  - Zeile BLOOM draw (PS/VS/Stufen/Ziel/Viewport/Blend/Shading-Pfad),
 *  - BLOOM st je belegter Stufe (xptr, RT-Slot, Groesse, Adressmodus),
 *  - BLOOM c (wirksame PS-Konstanten C0/C1 je Stufe, FC0/FC1),
 *  - BLOOM v je Quell-Vertex (bis 8): Position (CPU-Pfad), oT0..oT3
 *    (CPU-VS-Interpreter, Shading-Pfad) bzw. uv (alter Pfad),
 *  - Rohdumps (BGRA8, Zeilen dicht) shots/bl_<p>_<i>_t<k>.bin (Eingaenge),
 *    _pre.bin / _post.bin (Ziel vor/nach dem Draw); gleiche Inhalte werden
 *    nur einmal geschrieben (BLOOM dump ... = <Datei>).
 * Auswertung: scripts/bloom_check.py (CPU-Nachrechnung der Combiner). */
#include "render_int.h"
#include <d3d11_1.h>
#include "render_qa.h"
#include "sb_tex.h"
#include <stdio.h>

void sb_render_blend_state(unsigned *on, unsigned *src, unsigned *dst);
int rq_diag_vertex(unsigned i, float pos[4], float ot[4][4]); /* render_shade.c */
const float *rq_last_pblk(void);                               /* render_shade.c */
unsigned rq_last_pstages(void);
int rtt_cur_slot(void);                                        /* render_rtt.c */

float g_im_diag_mvp[16];      /* DIAG(SHADOWBUF,W127): MVP des letzten Draws (render_im.c, nur shadow_diag) */
static unsigned g_bl_p;       /* aktuelles Diag-Bild (Present-Nr.), 0 = keins */
static unsigned g_bl_nfr;     /* Diag-Bilder bisher */
static unsigned g_bl_lastp;   /* letztes Diag-Bild */
static unsigned g_bl_i;       /* Draw-Index im Diag-Bild */
static unsigned g_bl_ndump;   /* Dateien im Diag-Bild */
static unsigned long long g_bl_bytes;
static int g_bl_cur;          /* aktueller Draw wird protokolliert */
static int g_bl_tslot;        /* Ziel-Slot des aktuellen Draws */
#define BL_HMAX 256u
static unsigned g_bl_hash[BL_HMAX];
static char g_bl_hname[BL_HMAX][48];
static unsigned g_bl_nh;

/* DIAG(SHADE-M2,W117) bloom_diag: Zusammenfassung ALLER Draws des Diag-Bilds je
 * (Ziel, PS, VS, Shading ja/nein, Grund) -> Zeilen BLOOM wsum beim Present:
 * welcher Pass (512er Hauptmonitor-Welt, 256er FRONT VIEW) zeichnet was in
 * welchem Pfad (g_rq_why: Index wie RQSTAT-skip, 9 bind, 10 nicht versucht,
 * 11 keine Roh-Variante, 12 Roh-Upload, 100 Shading). */
extern unsigned g_rq_why;
typedef struct {
    int tslot;
    unsigned ps, vs, sh, why, tm, i0, i1, n, verts, xp[4], srvm;
    float c[4][4]; /* C0 st0, C1 st0, C0 st1, FC0 (nur Shading) */
} bl_wsum_t;
#define BL_WMAX 160u
static bl_wsum_t g_bl_w[BL_WMAX];
static unsigned g_bl_wn, g_bl_wdrop;
static unsigned g_bl_seen3[64], g_bl_n3;
/* W117: Backbuffer-Bereich des Hauptmonitors nach jedem BB-Draw im Diag-Bild
 * (Mittel RGB, Bereich x 36,5..58,5 %, y 31..57 % des BB) -> Zeile BLOOM bbreg:
 * welcher Draw hellt den Monitor auf. */
static int g_bl_bbm;
static unsigned g_bl_bbi;
static ID3D11Texture2D *g_bl_bbstg;
static unsigned g_bl_bbw, g_bl_bbh;
extern ID3D11RenderTargetView *g_rtv;
static void bl_bbreg(unsigned i, const char *when)
{
    ID3D11Resource *res = NULL;
    D3D11_TEXTURE2D_DESC td;
    D3D11_MAPPED_SUBRESOURCE map;
    D3D11_BOX box;
    unsigned x, y, w, h;
    double sr = 0, sg = 0, sb = 0;
    unsigned nwhite = 0;
    if (!g_rtv) { return; }
    ID3D11RenderTargetView_GetResource(g_rtv, &res);
    if (!res) { return; }
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)res, &td);
    box.left = td.Width * 365u / 1000u; box.right = td.Width * 585u / 1000u;
    box.top = td.Height * 310u / 1000u; box.bottom = td.Height * 570u / 1000u;
    box.front = 0; box.back = 1;
    w = box.right - box.left; h = box.bottom - box.top;
    if (!g_bl_bbstg || g_bl_bbw != w || g_bl_bbh != h) {
        D3D11_TEXTURE2D_DESC sd = td;
        if (g_bl_bbstg) { ID3D11Texture2D_Release(g_bl_bbstg); g_bl_bbstg = NULL; }
        sd.Width = w; sd.Height = h; sd.MipLevels = 1; sd.ArraySize = 1; sd.SampleDesc.Count = 1; sd.SampleDesc.Quality = 0;
        sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &sd, NULL, &g_bl_bbstg))) { g_bl_bbstg = NULL; }
        g_bl_bbw = w; g_bl_bbh = h;
    }
    if (g_bl_bbstg) {
        sb_ctx_enter();
        ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)g_bl_bbstg, 0, 0, 0, 0, res, 0, &box);
        if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_bl_bbstg, 0, D3D11_MAP_READ, 0, &map))) {
            int bgra = td.Format == DXGI_FORMAT_B8G8R8A8_UNORM || td.Format == DXGI_FORMAT_B8G8R8X8_UNORM;
            for (y = 0; y < h; y++) {
                const unsigned char *r = (const unsigned char *)map.pData + (size_t)y * map.RowPitch;
                for (x = 0; x < w; x++) {
                    unsigned c0 = r[x * 4], c1 = r[x * 4 + 1], c2 = r[x * 4 + 2];
                    sr += bgra ? c2 : c0; sg += c1; sb += bgra ? c0 : c2;
                    if (c0 >= 242 && c1 >= 242 && c2 >= 242) { nwhite++; }
                }
            }
            ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_bl_bbstg, 0);
            HLOG_W("BLOOM bbreg p=%u i=%u %s mean=(%.3f,%.3f,%.3f) weiss=%.1f%% box=%u,%u,%u,%u ps=0x%X vs=0x%X sh=%d",
                   g_bl_p, i, when, sr / (255.0 * w * h), sg / (255.0 * w * h), sb / (255.0 * w * h),
                   100.0 * nwhite / (double)(w * h), box.left, box.top, box.right, box.bottom,
                   g_im_pshader, g_im_shader, g_rq_active);
        }
        sb_ctx_leave();
    }
    ID3D11Resource_Release(res);
} /* W117: >=3-Stufen-Weltdraws je PS/Ziel einmal */

static unsigned bl_u32(unsigned a)
{
    unsigned v = 0;
    __try { v = *(volatile unsigned *)(uintptr_t)a; }
    __except (EXCEPTION_EXECUTE_HANDLER) { v = 0; }
    return v;
}

/* Wird das gerade gezeichnete Bild (g_present_no + 1) protokolliert? */
static int bl_frame_on(void)
{
    unsigned p = g_present_no + 1u, sf = (unsigned)g_cfg.screenshot_frame, ev = (unsigned)g_cfg.screenshot_every;
    unsigned r;
    if (g_bl_p == p) { return 1; }
    if (!sf || !ev || p < sf || p < (unsigned)g_cfg.bloom_frame) { return 0; }
    r = (p - sf) % ev;
    /* Partnerbild (shade_ab) nur zum protokollierten Basisbild, zaehlt nicht */
    if (!(r == 0u || (r == 1u && g_cfg.shade_ab && g_bl_lastp && g_bl_lastp + 1u == p))) { return 0; }
    if (r == 0u && g_bl_nfr >= (unsigned)g_cfg.bloom_max) { return 0; }
    if (r == 0u && g_bl_lastp && g_cfg.bloom_every > 0 && p - g_bl_lastp < (unsigned)g_cfg.bloom_every) { return 0; }
    if (bl_u32(M3D_SCENE_ADDR) != 0x2Cu || bl_u32(M3D_MLOOP_ADDR) != 1u) { return 0; }
    if (g_cfg.bloom_diag == 1 && bl_u32(0x2EEA34u) != 4u) { return 0; }
    g_bl_p = p; g_bl_lastp = p;
    if (r == 0u) { g_bl_nfr++; }
    g_bl_i = 0; g_bl_ndump = 0; g_bl_bytes = 0; g_bl_nh = 0; g_bl_wn = 0; g_bl_wdrop = 0; g_bl_n3 = 0;
    HLOG_W("BLOOM frame p=%u (#%u) shading=%d ab=%d rt_scale=%d", p, g_bl_nfr, g_cfg.shading,
           g_cfg.shade_ab ? (g_cfg.shading && rq_ab_on()) : -1, g_cfg.rt_scale);
    return 1;
}

static int bl_slot_of(unsigned xp)
{
    unsigned data = 0;
    int i;
    if (!xp) { return -1; }
    data = bl_u32(xp + 4u);
    if (!data) { return -1; }
    for (i = 0; i < g_rtt_n; i++) {
        if (g_rtt[i].surf && g_rtt[i].data == data && !g_rtt[i].isdepth) { return i; }
    }
    return -1;
}

/* Textur roh (BGRA8) schreiben; Inhalt je Diag-Bild nur einmal. */
static void bl_dump_res(ID3D11Resource *res, const char *tag, char *outname, size_t on)
{
    D3D11_TEXTURE2D_DESC td, sd;
    ID3D11Texture2D *stage = NULL;
    D3D11_MAPPED_SUBRESOURCE map;
    unsigned y, h = 2166136261u;
    unsigned char *buf;
    char path[600];
    FILE *fp = NULL;
    outname[0] = 0;
    if (!res || g_bl_ndump >= 200u || g_bl_bytes > (256ull << 20)) { strcpy_s(outname, on, "-"); return; }
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)res, &td);
    /* DIAG(SHADOWBUF,W127) shadow_diag: Schattenpuffer (R24G8_TYPELESS) roh als
     * uint32 je Texel (Bits 0..23 Tiefe D24, 24..31 Stencil) */
    if (td.Format != DXGI_FORMAT_B8G8R8A8_UNORM && td.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
        td.Format != DXGI_FORMAT_B8G8R8X8_UNORM && !(g_cfg.shadow_diag && td.Format == DXGI_FORMAT_R24G8_TYPELESS)) {
        _snprintf_s(outname, on, _TRUNCATE, "fmt%u", (unsigned)td.Format);
        return;
    }
    sd = td;
    sd.MipLevels = 1; sd.ArraySize = 1; sd.SampleDesc.Count = 1; sd.SampleDesc.Quality = 0;
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &sd, NULL, &stage)) || !stage) { strcpy_s(outname, on, "nostage"); return; }
    buf = (unsigned char *)malloc((size_t)td.Width * td.Height * 4u);
    sb_ctx_enter();
    ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)stage, 0, 0, 0, 0, res, 0, NULL);
    if (buf && SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)stage, 0, D3D11_MAP_READ, 0, &map))) {
        for (y = 0; y < td.Height; y++) {
            memcpy(buf + (size_t)y * td.Width * 4u, (unsigned char *)map.pData + (size_t)y * map.RowPitch, td.Width * 4u);
        }
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)stage, 0);
        {
            size_t k, n = (size_t)td.Width * td.Height * 4u;
            for (k = 0; k < n; k += 4) { h = (h ^ *(unsigned *)(buf + k)) * 16777619u; }
            h ^= td.Width * 7919u + td.Height;
        }
    } else if (buf) { free(buf); buf = NULL; }
    sb_ctx_leave();
    ID3D11Texture2D_Release(stage);
    if (!buf) { strcpy_s(outname, on, "nomap"); return; }
    for (y = 0; y < g_bl_nh; y++) {
        if (g_bl_hash[y] == h) { _snprintf_s(outname, on, _TRUNCATE, "=%s", g_bl_hname[y]); free(buf); return; }
    }
    _snprintf_s(outname, on, _TRUNCATE, "bl_%06u_%04u_%s.bin", g_bl_p, g_bl_i, tag);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s/shots/%s", hcfg_out_dir(), outname);
    if (fopen_s(&fp, path, "wb") == 0 && fp) {
        fwrite(buf, 1, (size_t)td.Width * td.Height * 4u, fp);
        fclose(fp);
        g_bl_ndump++;
        g_bl_bytes += (unsigned long long)td.Width * td.Height * 4u;
    }
    if (g_bl_nh < BL_HMAX) {
        g_bl_hash[g_bl_nh] = h;
        strcpy_s(g_bl_hname[g_bl_nh], sizeof g_bl_hname[0], outname);
        g_bl_nh++;
    }
    {   /* Groesse anhaengen (Python liest sie aus der Zeile) */
        char tmp[64];
        _snprintf_s(tmp, sizeof tmp, _TRUNCATE, "%s:%ux%u", outname, td.Width, td.Height);
        strcpy_s(outname, on, tmp);
    }
    free(buf);
}

static void bl_dump_srv(ID3D11ShaderResourceView *srv, const char *tag, char *outname, size_t on)
{
    ID3D11Resource *res = NULL;
    outname[0] = 0;
    if (!srv) { strcpy_s(outname, on, "-"); return; }
    ID3D11ShaderResourceView_GetResource(srv, &res);
    if (!res) { strcpy_s(outname, on, "-"); return; }
    bl_dump_res(res, tag, outname, on);
    ID3D11Resource_Release(res);
}

static unsigned bl_nstages(unsigned ps, unsigned *tm)
{
    unsigned pdef = ps ? bl_u32((ps & ~1u) + 8u) : 0u;
    if (tm) { *tm = 0; }
    if (pdef < 0x10000u || pdef >= 0x80000000u) { return 0; }
    if (tm) { *tm = bl_u32(pdef + 0xD8u); }
    return bl_u32(pdef + 0xD4u) & 0xFFu;
}

/* Vor dem Draw (Bindungen stehen). srv0 = Stufe 0 des alten Pfads. */
void bloom_diag_pre(const im_vertex_t *out, unsigned nout, unsigned nsrc, int rhw, unsigned prim,
                    ID3D11ShaderResourceView *srv0)
{
    unsigned xp[4], nn[4], k, tm = 0, nst, on = 0, bs = 0, bd = 0;
    int slot[4], tslot, rel = 0;
    char nm[96];
    g_bl_cur = 0;
    if (!bl_frame_on()) { return; }
    __try {
        sb_tex_stages(xp, nn);
        nst = bl_nstages(g_im_pshader, &tm);
        tslot = rtt_active() ? g_rtt_bound : -1;
        for (k = 0; k < 4u; k++) {
            slot[k] = (((tm >> (k * 5u)) & 0x1Fu) || k == 0u) ? bl_slot_of(xp[k]) : -1;
            if (slot[k] >= 0) { rel = 1; }
        }
        if (tslot >= 0 && g_rtt[tslot].w <= 128u && tm != 0u) { rel = 1; } /* kleine RTs (ohne Zellenkarte, tm=0) */
        if (tslot >= 0 && g_rtt[tslot].w <= 200u && (rs_cmask == 0x1000000u || tm != 0u)) { rel = 1; } /* 200er-Unschaerfe + Alpha-Masken */
        if (nst >= 3u) {
            /* W117: Welt-Draws mit >= 3 Stufen ohne RT-Eingang (z. B. 0xAB81A50,
             * 659 Draws je Pass) nur einmal je PS + Ziel und Bild, sonst ist
             * das Dump-Budget vor der Bloom-Kette verbraucht. */
            if (!rel && tslot >= 0 && g_rtt[tslot].w >= 256u) {
                unsigned q, key = g_im_pshader ^ ((unsigned)tslot << 24);
                for (q = 0; q < g_bl_n3; q++) { if (g_bl_seen3[q] == key) { break; } }
                if (q == g_bl_n3 && g_bl_n3 < 64u) { g_bl_seen3[g_bl_n3++] = key; rel = 1; }
            } else { rel = 1; }
        }
        g_bl_bbm = 0;
        if (tslot < 0 && rs_cmask != 0u) { g_bl_bbm = 1; g_bl_bbi = g_bl_i; } /* W117: BB-Monitorbereich nach dem Draw */
        if (rs_cmask != 0u) { /* DIAG(SHADE-M2,W117): Zusammenfassung je Pass */
            unsigned q, sh = g_rq_active ? 1u : 0u, why = g_rq_why;
            bl_wsum_t *w = NULL;
            for (q = 0; q < g_bl_wn; q++) {
                bl_wsum_t *e = &g_bl_w[q];
                if (e->tslot == tslot && e->ps == g_im_pshader && e->vs == g_im_shader && e->sh == sh && e->why == why) { w = e; break; }
            }
            if (!w && g_bl_wn < BL_WMAX) {
                w = &g_bl_w[g_bl_wn++];
                memset(w, 0, sizeof *w);
                w->tslot = tslot; w->ps = g_im_pshader; w->vs = g_im_shader; w->sh = sh; w->why = why; w->tm = tm;
                w->i0 = g_bl_i;
                for (q = 0; q < 4u; q++) {
                    w->xp[q] = xp[q];
                    if (sh ? (g_rq_rec.srv[q] != NULL) : (q == 0u && srv0 != NULL)) { w->srvm |= 1u << q; }
                }
                if (sh) {
                    const float *pb = rq_last_pblk();
                    memcpy(w->c[0], &pb[0], 16); memcpy(w->c[1], &pb[8u * 4u], 16);
                    memcpy(w->c[2], &pb[4], 16); memcpy(w->c[3], &pb[64], 16);
                }
            } else if (!w) { g_bl_wdrop++; }
            if (w) { w->n++; w->verts += nout; w->i1 = g_bl_i; }
        }
        if (g_cfg.shadow_diag > 0 && g_rq_active && !rel) {
            /* DIAG(SHADOWBUF,W127): Draws mit Schattenpuffer-Stufe (tsc[k].w != 0,
             * Tiefen-SRV) protokollieren - je Diag-Bild hoechstens shadow_diag;
             * Auswertung scripts/shadow_check.py (CPU-Vergleich an Vertex-Pixeln). */
            static unsigned sd_p, sd_n;
            const float *pb = rq_last_pblk();
            if (sd_p != g_bl_p) { sd_p = g_bl_p; sd_n = 0; }
            for (k = 1; k < 4u; k++) {
                if (g_rq_rec.srv[k] && pb[(19u + k) * 4u + 3u] != 0.0f && sd_n < (unsigned)g_cfg.shadow_diag) {
                    sd_n++; rel = 1;
                    HLOG_W("BLOOM shadow p=%u i=%u k=%u tsc=(%.8f,%.8f,%.0f,%.8f) func=0x%X", g_bl_p, g_bl_i, k,
                           pb[(19u + k) * 4u], pb[(19u + k) * 4u + 1u], pb[(19u + k) * 4u + 2u], pb[(19u + k) * 4u + 3u],
                           (*(const unsigned *)&pb[23u * 4u + 2u] >> 8) & 7u);
                    {   const float *m = g_im_diag_mvp; /* Zeilenvektor: clip = pos * M (wie g_im_vs) */
                        HLOG_W("BLOOM smvp p=%u i=%u m=(%g,%g,%g,%g,%g,%g,%g,%g,%g,%g,%g,%g,%g,%g,%g,%g)", g_bl_p, g_bl_i,
                               m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]); }
                    break;
                }
            }
        }
        if (!rel || rs_cmask == 0u) { g_bl_i++; return; }
        g_bl_cur = 1; g_bl_tslot = tslot;
        sb_render_blend_state(&on, &bs, &bd);
        HLOG_W("BLOOM draw p=%u i=%u ps=0x%X nst=%u tm=0x%X vs=0x%X sh=%d prim=%u n=%u nsrc=%u rhw=%d tgt=%d %ux%u/%ux%u vp=%ux%u+%u+%u bl=%u/%u/%u cm=0x%X",
               g_bl_p, g_bl_i, g_im_pshader, nst, tm, g_im_shader, g_rq_active, prim, nout, nsrc, rhw, tslot,
               tslot >= 0 ? g_rtt[tslot].w : g_bb_w, tslot >= 0 ? g_rtt[tslot].h : g_bb_h,
               tslot >= 0 ? g_rtt[tslot].pw : g_bb_w, tslot >= 0 ? g_rtt[tslot].ph : g_bb_h,
               g_vp_game.w, g_vp_game.h, g_vp_game.x, g_vp_game.y, on, bs, bd, rs_cmask);
        for (k = 0; k < 4u; k++) {
            ID3D11ShaderResourceView *s = g_rq_active ? g_rq_rec.srv[k] : (k == 0u ? srv0 : NULL);
            unsigned au = bl_u32(0x21A8B0u + (k * 32u + 0u) * 4u), av = bl_u32(0x21A8B0u + (k * 32u + 1u) * 4u);
            unsigned mag = bl_u32(0x21A8B0u + (k * 32u + 3u) * 4u), mn = bl_u32(0x21A8B0u + (k * 32u + 4u) * 4u);
            if (!xp[k] && !s) { continue; }
            if (k > 0u && !((tm >> (k * 5u)) & 0x1Fu) && !s) { continue; }
            {
                char tg[8];
                _snprintf_s(tg, sizeof tg, _TRUNCATE, "t%u", k);
                bl_dump_srv(s, tg, nm, sizeof nm);
            }
            HLOG_W("BLOOM st p=%u i=%u k=%u xp=0x%X rt=%d %ux%u/%ux%u fmt=0x%08X addr=%u/%u filt=%u/%u srv=%d file=%s",
                   g_bl_p, g_bl_i, k, xp[k], slot[k], slot[k] >= 0 ? g_rtt[slot[k]].w : 0u,
                   slot[k] >= 0 ? g_rtt[slot[k]].h : 0u, slot[k] >= 0 ? g_rtt[slot[k]].pw : 0u,
                   slot[k] >= 0 ? g_rtt[slot[k]].ph : 0u, xp[k] ? bl_u32(xp[k] + 12u) : 0u, au, av, mag, mn,
                   s != NULL, nm);
        }
        if (g_rq_active) {
            const float *pb = rq_last_pblk();
            unsigned q;
            for (q = 0; q < 8u && q < rq_last_pstages() + 1u; q++) {
                HLOG_W("BLOOM c p=%u i=%u st=%u C0=(%.4f,%.4f,%.4f,%.4f) C1=(%.4f,%.4f,%.4f,%.4f)", g_bl_p, g_bl_i, q,
                       pb[q * 4u], pb[q * 4u + 1], pb[q * 4u + 2], pb[q * 4u + 3],
                       pb[(8u + q) * 4u], pb[(8u + q) * 4u + 1], pb[(8u + q) * 4u + 2], pb[(8u + q) * 4u + 3]);
            }
            HLOG_W("BLOOM c p=%u i=%u FC0=(%.4f,%.4f,%.4f,%.4f) FC1=(%.4f,%.4f,%.4f,%.4f) tsc0=(%.6f,%.6f,%.0f) tsc1=(%.6f,%.6f,%.0f) tsc2=(%.6f,%.6f,%.0f) tsc3=(%.6f,%.6f,%.0f)",
                   g_bl_p, g_bl_i, pb[64], pb[65], pb[66], pb[67], pb[68], pb[69], pb[70], pb[71],
                   pb[76], pb[77], pb[78], pb[80], pb[81], pb[82], pb[84], pb[85], pb[86], pb[88], pb[89], pb[90]);
        } else {
            HLOG_W("BLOOM c p=%u i=%u psc=0x%X c0=0x%08X c1=0x%08X (alter Pfad)", g_bl_p, g_bl_i,
                   g_rq_psconst_set, g_rq_psconst[0], g_rq_psconst[1]);
        }
        for (k = 0; k < nout && k < 6u; k++) {
            HLOG_W("BLOOM o p=%u i=%u j=%u pos=(%.3f,%.3f,%.4f,%.4f) uv=(%.4f,%.4f) col=(%.3f,%.3f,%.3f,%.3f)",
                   g_bl_p, g_bl_i, k, out[k].pos[0], out[k].pos[1], out[k].pos[2], out[k].pos[3],
                   out[k].uv[0], out[k].uv[1], out[k].col[0], out[k].col[1], out[k].col[2], out[k].col[3]);
        }
        if (g_rq_active) {
            for (k = 0; k < nsrc && k < 8u; k++) {
                float pos[4], ot[4][4];
                if (!rq_diag_vertex(k, pos, ot)) { break; }
                HLOG_W("BLOOM v p=%u i=%u j=%u v0=(%.3f,%.3f,%.4f,%.4f) oT0=(%.4f,%.4f,%.4f,%.4f) oT1=(%.4f,%.4f,%.4f,%.4f) oT2=(%.4f,%.4f,%.4f,%.4f) oT3=(%.4f,%.4f,%.4f,%.4f)",
                       g_bl_p, g_bl_i, k, pos[0], pos[1], pos[2], pos[3],
                       ot[0][0], ot[0][1], ot[0][2], ot[0][3], ot[1][0], ot[1][1], ot[1][2], ot[1][3],
                       ot[2][0], ot[2][1], ot[2][2], ot[2][3], ot[3][0], ot[3][1], ot[3][2], ot[3][3]);
                {   /* DIAG(SHADE-M2,W117): Vertexfarben (Final-Combiner V0/V1, Nebel) */
                    extern float g_rq_diag_od[3][4];
                    HLOG_W("BLOOM vd p=%u i=%u j=%u oD0=(%.4f,%.4f,%.4f,%.4f) oD1=(%.4f,%.4f,%.4f,%.4f) oFog=%.4f",
                           g_bl_p, g_bl_i, k, g_rq_diag_od[0][0], g_rq_diag_od[0][1], g_rq_diag_od[0][2], g_rq_diag_od[0][3],
                           g_rq_diag_od[1][0], g_rq_diag_od[1][1], g_rq_diag_od[1][2], g_rq_diag_od[1][3], g_rq_diag_od[2][0]);
                }
            }
        }
        if (tslot >= 0 && g_rtt[tslot].tex) {
            bl_dump_res((ID3D11Resource *)g_rtt[tslot].tex, "pre", nm, sizeof nm);
            HLOG_W("BLOOM pre p=%u i=%u tgt=%d file=%s", g_bl_p, g_bl_i, tslot, nm);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_bl_cur = 0;
        HLOG_W("BLOOM-EX pre p=%u i=%u", g_bl_p, g_bl_i);
    }
}

void bloom_diag_post(void)
{
    char nm[96];
    if (g_bl_bbm) { /* DIAG(SHADE-M2,W117) */
        g_bl_bbm = 0;
        __try { bl_bbreg(g_bl_bbi, "nach"); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (!g_bl_cur) { return; }
    g_bl_cur = 0;
    __try {
        if (g_bl_tslot >= 0 && g_bl_tslot < g_rtt_n && g_rtt[g_bl_tslot].tex) {
            bl_dump_res((ID3D11Resource *)g_rtt[g_bl_tslot].tex, "post", nm, sizeof nm);
            HLOG_W("BLOOM post p=%u i=%u tgt=%d file=%s", g_bl_p, g_bl_i, g_bl_tslot, nm);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        HLOG_W("BLOOM-EX post p=%u i=%u", g_bl_p, g_bl_i);
    }
    g_bl_i++;
}

/* DIAG(SHADE-M2,W117): SetPixelShaderConstant im Diag-Bild (vor Draw i),
 * Rohwerte vor Klemmung/Quantisierung, Aufrufer = Ruecksprungadresse. */
void bloom_diag_psc(unsigned reg, unsigned pdata, unsigned count, unsigned ra)
{
    static unsigned nlog;
    float f[4] = { 0, 0, 0, 0 };
    if (!g_bl_p || g_bl_p != g_present_no + 1u || nlog > 20000u) { return; }
    nlog++;
    __try { memcpy(f, (const void *)(uintptr_t)pdata, 16); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    HLOG_W("BLOOM psc p=%u i=%u reg=%u n=%u v=(%.4f,%.4f,%.4f,%.4f) ps=0x%X tgt=%d ra=0x%X", g_bl_p, g_bl_i, reg, count,
           f[0], f[1], f[2], f[3], g_im_pshader, rtt_active() ? g_rtt_bound : -1, ra);
}

/* Clear in einem Diag-Bild (Clear-Farbe/-Alpha der RTs). */
void bloom_diag_clear(unsigned flags, unsigned color)
{
    if (!g_cfg.bloom_diag || g_bl_p != g_present_no + 1u) { return; }
    HLOG_W("BLOOM clear p=%u i=%u sess=%d bound=%d flags=0x%X color=0x%08X", g_bl_p, g_bl_i,
           rtt_cur_slot(), rtt_active() ? g_rtt_bound : -1, flags, color);
}

/* Present: Diag-Bild abschliessen. */
void bloom_diag_present(unsigned p)
{
    if (g_bl_p && g_bl_p == p) {
        unsigned q;
        for (q = 0; q < g_bl_wn; q++) { /* DIAG(SHADE-M2,W117) */
            const bl_wsum_t *w = &g_bl_w[q];
            int t = w->tslot;
            HLOG_W("BLOOM wsum p=%u tgt=%d %ux%u ps=0x%X vs=0x%X sh=%u why=%u tm=0x%X ir=%u..%u draws=%u verts=%u xp=%X/%X/%X/%X srv=0x%X C0=(%.3f,%.3f,%.3f,%.3f) C1=(%.3f,%.3f,%.3f,%.3f) C0s1=(%.3f,%.3f,%.3f,%.3f) FC0=(%.3f,%.3f,%.3f,%.3f)",
                   p, t, t >= 0 ? g_rtt[t].w : g_bb_w, t >= 0 ? g_rtt[t].h : g_bb_h, w->ps, w->vs, w->sh, w->why, w->tm,
                   w->i0, w->i1, w->n, w->verts, w->xp[0], w->xp[1], w->xp[2], w->xp[3], w->srvm,
                   w->c[0][0], w->c[0][1], w->c[0][2], w->c[0][3], w->c[1][0], w->c[1][1], w->c[1][2], w->c[1][3],
                   w->c[2][0], w->c[2][1], w->c[2][2], w->c[2][3], w->c[3][0], w->c[3][1], w->c[3][2], w->c[3][3]);
        }
        if (g_bl_wdrop) { HLOG_W("BLOOM wsum p=%u verworfen=%u", p, g_bl_wdrop); }
        __try { bl_bbreg(g_bl_i, "present"); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        g_bl_wn = 0; g_bl_wdrop = 0;
        HLOG_W("BLOOM end p=%u draws=%u dumps=%u bytes=%llu", p, g_bl_i, g_bl_ndump, g_bl_bytes);
        g_bl_p = 0;
    }
}
