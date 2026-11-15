/* loc_render.c - W285 LOC-RENDER: Renderer-Anpassungen nur fuer das LoC-Profil.
 * Alles hier wirkt nur bei g_sba_loc (Line of Contact, [expert] loc_boot=1). */
#include "render_int.h"
#include "loc_render.h"
#include "hooks_int.h"
#include "hcfg.h"
#include "sb_profile.h"
#include "hlog.h"

unsigned g_loc_draws;
static unsigned s_prev_draws;

extern int g_rqp_bump_as_2d; /* render_psgen.c */

void loc_render_frame(unsigned f)
{
    g_rqp_bump_as_2d = g_sba_loc && g_cfg.loc_bumpenv; /* W285: BUMPENVMAP-Naeherung */
    if (g_cfg.loc_census_from > 0 && (f % 10u) == 0u) { /* DIAG: Draws je Bild (Szenenwahl) */
        HLOG_I("LOCDRAWS f=%u n=%u", f, g_loc_draws);
    }
    s_prev_draws = g_loc_draws;
    g_loc_draws = 0;
}

/* DIAG(LOC-RENDER): Zensus-Ausloeser im LoC-Profil. Die SB-Szenenadressen lesen
 * dort die Null-Seite; der Zeitpunkt der Titel-Demo schwankt je Lauf. Daher: ab
 * Bild loc_census_from und nur, wenn das Vorbild mindestens loc_census_draws
 * Draws hatte (3D-Szene statt Titel/Karte). */
int loc_render_census_go(unsigned f)
{
    if (!g_sba_loc || g_cfg.loc_census_from <= 0 || f < (unsigned)g_cfg.loc_census_from) { return 0; }
    return s_prev_draws >= (unsigned)g_cfg.loc_census_draws;
}

/* ---------------------------------------------------------------------------
 * Backbuffer als Textur (LoC-Glow/Bloom-Kette).
 *
 * Beleg (Zensus W285G2, Demo-Spielszene): Nach der 3D-Welt im Backbuffer setzt
 * LoC die von GetBackBuffer2 gelieferte Surface (bei uns g_fake_surface, Kopf
 * leer) als Textur und zeichnet
 *   1. ein Vollbild-Quad ins 256x256-RT (Verkleinerung der Szene, Texel-UV 0..1440 x 0..480),
 *   2. Weichzeichner/Helligkeit in 256/14/9-RTs,
 *   3. das Kompositions-Quad in den Backbuffer (Blend aus, Stufe 0 = Backbuffer,
 *      Stufe 1 = Glow-RT), Bildschirmkoordinaten 0..1440 x 0..480.
 * Ohne Textur wurde Stufe 0 weiss -> weisse Flaeche ueber der ganzen 4:3-Box.
 * 1440 = 2 x 720: LoC legt das Geraet mit MultiSampleType 0x41121 an (FUN_0001DCD0,
 * D3DMULTISAMPLE_2_SAMPLES_SUPERSAMPLE_HORIZONTAL_LINEAR) -> Xbox-Backbuffer
 * 1440x480. Wir zeichnen in 720er-Einheiten in die 4:3-Box; die Kopie der Box
 * vertritt den 1440x480-Puffer (Texel-UV / 1440 = normiert).
 * ------------------------------------------------------------------------- */
static ID3D11Texture2D *s_bbc_tex, *s_bbc_res;
static ID3D11ShaderResourceView *s_bbc_srv;
static unsigned s_bbc_w, s_bbc_h, s_bbc_rw, s_bbc_rh, s_bbc_frame = 0xFFFFFFFFu, s_bbc_draw = 0xFFFFFFFFu;
static DXGI_FORMAT s_bbc_fmt;
unsigned g_loc_bbtex_draws; /* Zaehler (Log) */

static void bbc_release(void)
{
    if (s_bbc_srv) { ID3D11ShaderResourceView_Release(s_bbc_srv); s_bbc_srv = NULL; }
    if (s_bbc_tex) { ID3D11Texture2D_Release(s_bbc_tex); s_bbc_tex = NULL; }
    if (s_bbc_res) { ID3D11Texture2D_Release(s_bbc_res); s_bbc_res = NULL; }
    s_bbc_w = s_bbc_h = s_bbc_rw = s_bbc_rh = 0;
}

/* DIAG(loc_psdump): Alpha-Statistik der Backbuffer-Kopie (Glow-Maske der LoC-Komposition). */
static void tex_alpha_diag(ID3D11Texture2D *src, const char *what)
{
    D3D11_TEXTURE2D_DESC sd;
    ID3D11Texture2D *st = NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    unsigned x, y, hist[8] = { 0 }, n = 0;
    unsigned long long sum = 0;
    if (!src) { return; }
    ID3D11Texture2D_GetDesc(src, &sd);
    if (sd.SampleDesc.Count != 1 || sd.ArraySize != 1) { return; }
    sd.MipLevels = 1;
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &sd, NULL, &st)) || !st) { return; }
    ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)st, 0, 0, 0, 0, (ID3D11Resource *)src, 0, NULL);
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
        for (y = 0; y < sd.Height; y += 8u) {
            const unsigned char *row = (const unsigned char *)m.pData + (size_t)y * m.RowPitch;
            for (x = 0; x < sd.Width; x += 8u) { unsigned a = row[x * 4u + 3u]; sum += a; hist[a >> 5]++; n++; }
        }
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)st, 0);
        HLOG_I("LOCRENDER DIAG: %s-Alpha f=%u fmt=%u mittel=%.1f hist8=[%u %u %u %u %u %u %u %u]", what, g_present_no, (unsigned)sd.Format,
               n ? (double)sum / n : 0.0, hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6], hist[7]);
    }
    ID3D11Texture2D_Release(st);
}

static ID3D11ShaderResourceView *bbc_copy(void)
{
    ID3D11Resource *res = NULL;
    ID3D11Texture2D *bb = NULL;
    D3D11_TEXTURE2D_DESC bd, dd;
    D3D11_BOX box;
    unsigned bx = 0, by = 0, bw = 0, bh = 0;
    static unsigned nlog;
    if (!g_dev || !g_ctx || !g_rtv) { return NULL; }
    if (s_bbc_srv && s_bbc_frame == g_present_no && s_bbc_draw == g_loc_draws) { return s_bbc_srv; }
    sb_aspect_box(&bx, &by, &bw, &bh);
    if (!bw || !bh) { return NULL; }
    sb_ctx_enter();
    ID3D11RenderTargetView_GetResource(g_rtv, &res);
    if (!res || FAILED(ID3D11Resource_QueryInterface(res, &IID_ID3D11Texture2D, (void **)&bb)) || !bb) {
        if (res) { ID3D11Resource_Release(res); }
        sb_ctx_leave();
        return NULL;
    }
    ID3D11Resource_Release(res);
    ID3D11Texture2D_GetDesc(bb, &bd);
    if (bx + bw > bd.Width) { bw = bd.Width > bx ? bd.Width - bx : 0; }
    if (by + bh > bd.Height) { bh = bd.Height > by ? bd.Height - by : 0; }
    if (!bw || !bh) { ID3D11Texture2D_Release(bb); sb_ctx_leave(); return NULL; }
    if (s_bbc_tex && (s_bbc_w != bw || s_bbc_h != bh || s_bbc_fmt != bd.Format)) { bbc_release(); }
    if (!s_bbc_tex) {
        memset(&dd, 0, sizeof dd);
        dd.Width = bw; dd.Height = bh; dd.MipLevels = 1; dd.ArraySize = 1;
        dd.Format = bd.Format; dd.SampleDesc.Count = 1;
        dd.Usage = D3D11_USAGE_DEFAULT; dd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &dd, NULL, &s_bbc_tex)) ||
            FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)s_bbc_tex, NULL, &s_bbc_srv))) {
            bbc_release();
            ID3D11Texture2D_Release(bb);
            sb_ctx_leave();
            return NULL;
        }
        s_bbc_w = bw; s_bbc_h = bh; s_bbc_fmt = bd.Format;
    }
    box.left = bx; box.top = by; box.right = bx + bw; box.bottom = by + bh; box.front = 0; box.back = 1;
    if (bd.SampleDesc.Count > 1) { /* MSAA-Backbuffer: erst aufloesen */
        if (s_bbc_res && (s_bbc_rw != bd.Width || s_bbc_rh != bd.Height)) {
            ID3D11Texture2D_Release(s_bbc_res); s_bbc_res = NULL;
        }
        if (!s_bbc_res) {
            dd = bd; dd.SampleDesc.Count = 1; dd.SampleDesc.Quality = 0; dd.MipLevels = 1;
            dd.Usage = D3D11_USAGE_DEFAULT; dd.BindFlags = D3D11_BIND_SHADER_RESOURCE; dd.CPUAccessFlags = 0; dd.MiscFlags = 0;
            if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &dd, NULL, &s_bbc_res))) { s_bbc_res = NULL; }
            s_bbc_rw = bd.Width; s_bbc_rh = bd.Height;
        }
        if (s_bbc_res) {
            ID3D11DeviceContext_ResolveSubresource(g_ctx, (ID3D11Resource *)s_bbc_res, 0, (ID3D11Resource *)bb, 0, bd.Format);
            ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)s_bbc_tex, 0, 0, 0, 0, (ID3D11Resource *)s_bbc_res, 0, &box);
        }
    } else {
        ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)s_bbc_tex, 0, 0, 0, 0, (ID3D11Resource *)bb, 0, &box);
    }
    ID3D11Texture2D_Release(bb);
    sb_ctx_leave();
    s_bbc_frame = g_present_no; s_bbc_draw = g_loc_draws;
    g_loc_bbtex_draws++;
    if (g_cfg.loc_psdump && g_loc_bbtex_draws <= 400u && (g_loc_bbtex_draws % 50u) <= 2u) { /* DIAG */
        unsigned xp[4], nn[4], w1 = 0, h1 = 0;
        ID3D11ShaderResourceView *s1 = NULL;
        ID3D11Resource *r1 = NULL;
        tex_alpha_diag(s_bbc_tex, "BB");
        sb_tex_stages(xp, nn);
        if (xp[1] && sb_rtt_lookup(xp[1], &s1, &w1, &h1) && s1) {
            ID3D11ShaderResourceView_GetResource(s1, &r1);
            if (r1) { tex_alpha_diag((ID3D11Texture2D *)r1, "Stufe1-RT"); ID3D11Resource_Release(r1); }
        }
    }
    if (nlog < 3u) {
        nlog++;
        HLOG_I("LOCRENDER: Backbuffer als Textur #%u: Box %ux%u@%u,%u (BB %ux%u, %u Samples) -> Texel %ux%u f=%u",
               nlog, bw, bh, bx, by, bd.Width, bd.Height, bd.SampleDesc.Count, g_game_fb_w * 2u, g_game_fb_h, g_present_no);
    }
    return s_bbc_srv;
}

int loc_render_is_bbtex(unsigned xptr)
{
    return g_sba_loc && g_cfg.loc_bbtex && xptr && xptr == (unsigned)(uintptr_t)g_fake_surface;
}

int loc_render_bb_lookup(unsigned xptr, ID3D11ShaderResourceView **srv, unsigned *w, unsigned *h)
{
    if (!loc_render_is_bbtex(xptr)) { return 0; }
    if (w) { *w = g_game_fb_w * 2u; } /* Xbox-Backbuffer 2x horizontal (Supersampling) */
    if (h) { *h = g_game_fb_h; }
    if (srv) { *srv = bbc_copy(); }
    return 1;
}

void loc_render_dump_ps(unsigned handle, const unsigned *d, unsigned nd, const char *src)
{
    char path[600];
    FILE *f = NULL;
    unsigned i;
    static unsigned n;
    if (n >= 64u) { return; }
    n++;
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s/locps_%08X.txt", hcfg_out_dir(), handle);
    if (fopen_s(&f, path, "w") || !f) { return; }
    fprintf(f, "PS 0x%08X (f=%u)\n", handle, g_present_no);
    for (i = 0; i < nd; i++) { fprintf(f, "d[%u]=%08X%s", i, d[i], (i % 6u) == 5u ? "\n" : " "); }
    fprintf(f, "\n---\n%s\n", src ? src : "(nicht unterstuetzt)");
    fclose(f);
}
