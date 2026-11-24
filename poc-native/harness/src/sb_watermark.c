/* sb_watermark.c - W219 WATERMARK: temporaeres Versions-Wasserzeichen (W301: BETA).
 * Beschreibung s. sb_watermark.h. Quelltext ASCII (Mittelpunkt per Zeichencode 0x00B7). */
#include "render_int.h"
#include "sb_overlay.h"
#include "overlay_style.h" /* W461 OVERLAY-CORE */
#include "sb_photo.h"
#include "sb_watermark.h"
#include "sb_version.h" /* W301: Phase ALPHA/BETA */
#include "render_vexp.h" /* W290: watermark_media */
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifndef SB_VERSION_STR
#define SB_VERSION_STR ""
#endif

#define WM_ALPHA   OVS_WM_ALPHA  /* W467: Werte aus overlay_style */
#define WM_SHADOW  OVS_WM_SHADOW
#define WM_R ((OVS_RGB_WM >> 16) & 255u)
#define WM_G ((OVS_RGB_WM >> 8) & 255u)
#define WM_B (OVS_RGB_WM & 255u)

static ovs_canvas_t wm_cv;      /* W461: GDI-DIB (Abdeckung), Schrift und Textur aus overlay_style */
#define wm_dc (wm_cv.dc)
#define wm_bits (wm_cv.bits)
static ovs_font_t wm_fn;
static unsigned *wm_pm;         /* fertiges vormultipliziertes BGRA */
static unsigned wm_w, wm_h;     /* Groesse des fertigen Bildes */
static float wm_f;              /* Faktor des fertigen Bildes */
static ovs_tex_t wm_tx;
/* PO 13.11.: Version fuer andere Overlays (W222 Willkommens-Hinweis); "" = Entwickler-Build */
const char *sb_version_str(void) { return SB_VERSION_STR; }

static int wm_dirty_tex = 1;
static sb_ovl_save_t wm_save;
static wchar_t wm_txt[96];

static void wm_text_init(void)
{
    char lb[128];
    if (wm_txt[0]) { return; }
    /* W301 BETA-VERSION: gemeinsamer Text (sb_version.h, Launcher-Selbsttest prueft ihn):
     * 0.x ohne Suffix = ". BETA", 0.1.0-alpha.N = ". ALPHA", ab 1.0.0 ohne, sonst "Steelbound dev" */
    sb_version_label(lb, sizeof lb, SB_VERSION_STR);
    if (!MultiByteToWideChar(CP_UTF8, 0, lb, -1, wm_txt, 96)) {
        wcscpy_s(wm_txt, 96, L"Steelbound dev");
    }
}

int sb_wm_on(void)
{
    /* W290: watermark_media=0 -> nicht im Video-Export (Export-Fenster ist unsichtbar, nur das Video zaehlt) */
    return g_cfg.watermark && !g_cfg.vr_enable && !sb_photo_active() && !(!g_cfg.watermark_media && sb_vexp_on());
}

/* 4:3-Box fuer ein Bild w x h (gleiche Formel wie sb_aspect_box, ohne Log) */
#define S ovs_px

/* Text bei Faktor f in wm_pm rendern (nur bei Groessenwechsel). 1 = ok. */
static int wm_render(float f)
{
    HFONT font, fold;
    SIZE sz;
    int fpx, pad, off, n;
    unsigned w, h, i, *shadow;
    if (wm_pm && f == wm_f) { return 1; }
    wm_text_init();
    if (!ovs_canvas_dc(&wm_cv)) { return 0; }
    fpx = S(f, 8.0f);
    font = ovs_font_get(&wm_fn, OVS_FACE_MONO, FW_NORMAL, fpx);
    if (!font) { return 0; }
    fold = (HFONT)SelectObject(wm_dc, font);
    n = (int)wcslen(wm_txt);
    if (!GetTextExtentPoint32W(wm_dc, wm_txt, n, &sz)) { sz.cx = fpx * n / 2; sz.cy = fpx; }
    pad = S(f, 1.0f);
    off = S(f, 0.5f);
    w = (unsigned)(sz.cx + 2 * pad + off);
    h = (unsigned)(sz.cy + 2 * pad + off);
    shadow = (unsigned *)malloc((size_t)w * h * 4u);
    if (!ovs_canvas_size(&wm_cv, w, h) || !shadow) {
        free(shadow);
        SelectObject(wm_dc, fold);
        return 0;
    }
    SetBkMode(wm_dc, TRANSPARENT);
    SetTextColor(wm_dc, RGB(255, 255, 255));
    SetTextAlign(wm_dc, TA_LEFT | TA_TOP);
    /* Pass 1: Schatten (versetzt), Pass 2: Schrift; Abdeckung = Gruenkanal */
    memset(wm_bits, 0, (size_t)w * h * 4u);
    TextOutW(wm_dc, pad + off, pad + off, wm_txt, n);
    GdiFlush();
    memcpy(shadow, wm_bits, (size_t)w * h * 4u);
    memset(wm_bits, 0, (size_t)w * h * 4u);
    TextOutW(wm_dc, pad, pad, wm_txt, n);
    GdiFlush();
    free(wm_pm);
    wm_pm = (unsigned *)malloc((size_t)w * h * 4u);
    if (!wm_pm) { free(shadow); SelectObject(wm_dc, fold); return 0; }
    for (i = 0; i < w * h; i++) {
        float ct = (float)((wm_bits[i] >> 8) & 255u) / 255.0f, cs = (float)((shadow[i] >> 8) & 255u) / 255.0f;
        float at = ct * WM_ALPHA, as = cs * WM_SHADOW, a = at + as * (1.0f - at);
        unsigned r = (unsigned)((float)WM_R * at + 0.5f), g = (unsigned)((float)WM_G * at + 0.5f),
                 b = (unsigned)((float)WM_B * at + 0.5f), aa = (unsigned)(a * 255.0f + 0.5f);
        wm_pm[i] = (aa << 24) | (r << 16) | (g << 8) | b;
    }
    free(shadow);
    SelectObject(wm_dc, fold);
    HLOG_I("WATERMARK: Version \"%s\" %ux%u (Faktor %.2f, Schrift %d px)", SB_VERSION_STR[0] ? SB_VERSION_STR : "dev",
           w, h, (double)f, fpx);
    wm_w = w;
    wm_h = h;
    wm_f = f;
    wm_dirty_tex = 1;
    return 1;
}

/* Lage im Bild w x h: unten rechts in der 4:3-Box, Rand 6 Einheiten. 0 = passt nicht. */
static int wm_place(unsigned w, unsigned h, unsigned *x0, unsigned *y0)
{
    unsigned bx, by, bw, bh, m;
    float f;
    if (g_sba_loc) { ovs_loc_box(&bx, &by, &bw, &bh); } /* W467: LoC 16:9 -> Spielbox statt 4:3 */
    else { ovs_fit_box(640.0f / 480.0f, w, h, &bx, &by, &bw, &bh); }
    f = ovs_scale_ui(bh);
    if (!wm_render(f)) { return 0; }
    m = (unsigned)S(f, 6.0f);
    if (wm_w + 2u * m > bw || wm_h + 2u * m > bh) { return 0; }
    *x0 = bx + bw - wm_w - m;
    *y0 = by + bh - wm_h - m;
    return 1;
}

static int wm_upload(void)
{
    int r = ovs_tex_ensure(&wm_tx, wm_w, wm_h);
    if (!r) {
        HLOG_W("WATERMARK: Textur %ux%u gescheitert", wm_w, wm_h);
        return 0;
    }
    if (r == 2) { wm_dirty_tex = 1; }
    if (!wm_dirty_tex) { return 1; }
    if (!ovs_tex_write(&wm_tx, wm_pm)) { return 0; }
    wm_dirty_tex = 0;
    return 1;
}

void sb_wm_draw(ID3D11RenderTargetView *rtv, int save)
{
    unsigned x0, y0;
    if (!sb_wm_on() || !rtv || !g_ctx || !g_dev) { return; }
    if (!wm_place(g_bb_w, g_bb_h, &x0, &y0)) { return; }
    if (!wm_upload()) { return; }
    ovs_layout_claim(OVS_SLOT_WATERMARK, x0, y0, wm_w, wm_h);
    if (save) { sb_ovl_save_take(&wm_save, rtv, x0, y0, wm_w, wm_h); }
    sb_ovl_quad(rtv, wm_tx.srv, x0, y0, wm_w, wm_h);
}

void sb_wm_restore(void)
{
    sb_ovl_save_put(&wm_save);
}

int sb_wm_blend_bgr(unsigned char *bgr, unsigned w, unsigned h)
{
    unsigned x0, y0, x, y;
    if (!sb_wm_on() || !bgr) { return 0; }
    if (!wm_place(w, h, &x0, &y0)) { return 0; }
    for (y = 0; y < wm_h; y++) {
        unsigned char *d = bgr + ((size_t)(y0 + y) * w + x0) * 3u;
        const unsigned *s = wm_pm + (size_t)y * wm_w;
        for (x = 0; x < wm_w; x++, d += 3) {
            unsigned c = s[x], ia = 255u - (c >> 24);
            d[0] = (unsigned char)((c & 255u) + (d[0] * ia + 127u) / 255u);
            d[1] = (unsigned char)(((c >> 8) & 255u) + (d[1] * ia + 127u) / 255u);
            d[2] = (unsigned char)(((c >> 16) & 255u) + (d[2] * ia + 127u) / 255u);
        }
    }
    return 1;
}
