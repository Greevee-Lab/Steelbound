/* sb_watermark.c - W219 WATERMARK: temporaeres Alpha-Wasserzeichen.
 * Beschreibung s. sb_watermark.h. Quelltext ASCII (Mittelpunkt per Zeichencode 0x00B7). */
#include "render_int.h"
#include "sb_overlay.h"
#include "sb_photo.h"
#include "sb_watermark.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifndef SB_VERSION_STR
#define SB_VERSION_STR ""
#endif

#define WM_ALPHA   0.60f /* Deckung der Schrift */
#define WM_SHADOW  0.45f /* Deckung des Schattens (dunkel, 1 Einheit versetzt) */
#define WM_R 225
#define WM_G 240
#define WM_B 225

static HDC wm_dc;
static HBITMAP wm_bmp, wm_bmp_old;
static unsigned *wm_bits;       /* GDI-DIB (Abdeckung) */
static unsigned *wm_pm;         /* fertiges vormultipliziertes BGRA */
static unsigned wm_w, wm_h;     /* Groesse des fertigen Bildes */
static float wm_f;              /* Faktor des fertigen Bildes */
static ID3D11Texture2D *wm_tex;
static ID3D11ShaderResourceView *wm_srv;
static unsigned wm_tw, wm_th;
/* PO 13.11.: Version fuer andere Overlays (W222 Willkommens-Hinweis); "" = Entwickler-Build */
const char *sb_version_str(void) { return SB_VERSION_STR; }

static int wm_dirty_tex = 1;
static sb_ovl_save_t wm_save;
static wchar_t wm_txt[96];

static void wm_text_init(void)
{
    const char *v = SB_VERSION_STR;
    wchar_t wv[48];
    if (wm_txt[0]) { return; }
    if (!v[0] || !MultiByteToWideChar(CP_UTF8, 0, v, -1, wv, 48)) {
        wcscpy_s(wm_txt, 96, L"Steelbound dev");
    } else if (strstr(v, "alpha")) {
        _snwprintf_s(wm_txt, 96, _TRUNCATE, L"Steelbound %s %c ALPHA", wv, (wchar_t)0x00B7);
    } else if (strstr(v, "beta")) {
        _snwprintf_s(wm_txt, 96, _TRUNCATE, L"Steelbound %s %c BETA", wv, (wchar_t)0x00B7);
    } else {
        _snwprintf_s(wm_txt, 96, _TRUNCATE, L"Steelbound %s", wv);
    }
}

int sb_wm_on(void)
{
    return g_cfg.watermark && !g_cfg.vr_enable && !sb_photo_active();
}

/* 4:3-Box fuer ein Bild w x h (gleiche Formel wie sb_aspect_box, ohne Log) */
static void wm_box(unsigned w, unsigned h, unsigned *bx, unsigned *by, unsigned *bw, unsigned *bh)
{
    float ga = 640.0f / 480.0f, ba = (float)w / (float)(h ? h : 1);
    if (ga >= ba) {
        *bw = w;
        *bh = (unsigned)((float)w / ga + 0.5f);
        if (*bh < 1) { *bh = 1; }
        if (*bh > h) { *bh = h; }
        *bx = 0;
        *by = (h - *bh) / 2;
    } else {
        *bh = h;
        *bw = (unsigned)((float)h * ga + 0.5f);
        if (*bw < 1) { *bw = 1; }
        if (*bw > w) { *bw = w; }
        *bx = (w - *bw) / 2;
        *by = 0;
    }
}

static int S(float f, float x)
{
    int v = (int)(x * f + 0.5f);
    return v < 1 ? 1 : v;
}

/* Text bei Faktor f in wm_pm rendern (nur bei Groessenwechsel). 1 = ok. */
static int wm_render(float f)
{
    BITMAPINFO bi;
    HFONT font, fold;
    SIZE sz;
    int fpx, pad, off, n;
    unsigned w, h, i, *shadow;
    if (wm_pm && f == wm_f) { return 1; }
    wm_text_init();
    if (!wm_dc) {
        wm_dc = CreateCompatibleDC(NULL);
        if (!wm_dc) { return 0; }
    }
    fpx = S(f, 8.0f);
    font = CreateFontW(-fpx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                       CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    if (!font) { return 0; }
    fold = (HFONT)SelectObject(wm_dc, font);
    n = (int)wcslen(wm_txt);
    if (!GetTextExtentPoint32W(wm_dc, wm_txt, n, &sz)) { sz.cx = fpx * n / 2; sz.cy = fpx; }
    pad = S(f, 1.0f);
    off = S(f, 0.5f);
    w = (unsigned)(sz.cx + 2 * pad + off);
    h = (unsigned)(sz.cy + 2 * pad + off);
    if (wm_bmp) { SelectObject(wm_dc, wm_bmp_old); DeleteObject(wm_bmp); wm_bmp = NULL; wm_bits = NULL; }
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = (LONG)w;
    bi.bmiHeader.biHeight = -(LONG)h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    wm_bmp = CreateDIBSection(wm_dc, &bi, DIB_RGB_COLORS, (void **)&wm_bits, NULL, 0);
    shadow = (unsigned *)malloc((size_t)w * h * 4u);
    if (!wm_bmp || !wm_bits || !shadow) {
        free(shadow);
        SelectObject(wm_dc, fold);
        DeleteObject(font);
        return 0;
    }
    wm_bmp_old = (HBITMAP)SelectObject(wm_dc, wm_bmp);
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
    if (!wm_pm) { free(shadow); SelectObject(wm_dc, fold); DeleteObject(font); return 0; }
    for (i = 0; i < w * h; i++) {
        float ct = (float)((wm_bits[i] >> 8) & 255u) / 255.0f, cs = (float)((shadow[i] >> 8) & 255u) / 255.0f;
        float at = ct * WM_ALPHA, as = cs * WM_SHADOW, a = at + as * (1.0f - at);
        unsigned r = (unsigned)((float)WM_R * at + 0.5f), g = (unsigned)((float)WM_G * at + 0.5f),
                 b = (unsigned)((float)WM_B * at + 0.5f), aa = (unsigned)(a * 255.0f + 0.5f);
        wm_pm[i] = (aa << 24) | (r << 16) | (g << 8) | b;
    }
    free(shadow);
    SelectObject(wm_dc, fold);
    DeleteObject(font);
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
    wm_box(w, h, &bx, &by, &bw, &bh);
    f = (float)bh / 540.0f;
    if (f < 1.0f) { f = 1.0f; }
    if (!wm_render(f)) { return 0; }
    m = (unsigned)S(f, 6.0f);
    if (wm_w + 2u * m > bw || wm_h + 2u * m > bh) { return 0; }
    *x0 = bx + bw - wm_w - m;
    *y0 = by + bh - wm_h - m;
    return 1;
}

static int wm_upload(void)
{
    D3D11_MAPPED_SUBRESOURCE map;
    unsigned y;
    if (!wm_tex || wm_tw != wm_w || wm_th != wm_h) {
        D3D11_TEXTURE2D_DESC td;
        if (wm_srv) { ID3D11ShaderResourceView_Release(wm_srv); wm_srv = NULL; }
        if (wm_tex) { ID3D11Texture2D_Release(wm_tex); wm_tex = NULL; }
        memset(&td, 0, sizeof td);
        td.Width = wm_w;
        td.Height = wm_h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &wm_tex)) ||
            FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)wm_tex, NULL, &wm_srv))) {
            HLOG_W("WATERMARK: Textur %ux%u gescheitert", wm_w, wm_h);
            return 0;
        }
        wm_tw = wm_w;
        wm_th = wm_h;
        wm_dirty_tex = 1;
    }
    if (!wm_dirty_tex) { return 1; }
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)wm_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        return 0;
    }
    for (y = 0; y < wm_h; y++) {
        memcpy((unsigned char *)map.pData + (size_t)y * map.RowPitch, wm_pm + (size_t)y * wm_w, wm_w * 4u);
    }
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)wm_tex, 0);
    wm_dirty_tex = 0;
    return 1;
}

void sb_wm_draw(ID3D11RenderTargetView *rtv, int save)
{
    unsigned x0, y0;
    if (!sb_wm_on() || !rtv || !g_ctx || !g_dev) { return; }
    if (!wm_place(g_bb_w, g_bb_h, &x0, &y0)) { return; }
    if (!wm_upload()) { return; }
    if (save) { sb_ovl_save_take(&wm_save, rtv, x0, y0, wm_w, wm_h); }
    sb_ovl_quad(rtv, wm_srv, x0, y0, wm_w, wm_h);
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
