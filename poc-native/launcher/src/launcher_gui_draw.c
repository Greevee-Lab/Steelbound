/* launcher_gui_draw.c - Zeichnen/Theme: Kleinkram, Zeichenflaeche, Schriften, Hintergrundbild, launcher_ui.cfg, Texte, Steuerelemente anlegen, Bild zusammensetzen, eigene Knoepfe.
 * W462 LAUNCHER-SPLIT: aus launcher_gui.c verschoben (unveraendert), gemeinsamer Zustand
 * in launcher_gui_int.h. */
#include "launcher_gui_int.h"
/* Kleinkram                                                          */
/* ------------------------------------------------------------------ */
void ui_settext(HWND h, const char *u8)
{
    wchar_t *w = u8_wdup(u8);
    SetWindowTextW(h, w ? w : L"");
    free(w);
}

void ui_gettext(HWND h, char *out, size_t cap)
{
    wchar_t w[2048];
    GetWindowTextW(h, w, 2048);
    w_to_u8(w, out, (int)cap);
}

void trim_u8(char *s)
{
    char *p = s, *e;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (p != s) {
        memmove(s, p, strlen(p) + 1);
    }
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t')) {
        e--;
    }
    *e = '\0';
}

uictl_t *ui_find(int id)
{
    int i;
    for (i = 0; i < g_ui_n; i++) {
        if (g_ui[i].id == id && g_ui[i].parent == g_hwnd) {
            return &g_ui[i];
        }
    }
    return NULL;
}

uictl_t *ui_by_hwnd(HWND h)
{
    int i;
    for (i = 0; i < g_ui_n; i++) {
        if (g_ui[i].h == h) {
            return &g_ui[i];
        }
    }
    return NULL;
}

HWND ctl(int id)
{
    uictl_t *u = ui_find(id);
    return u ? u->h : NULL;
}

int ui_checked(int id)
{
    uictl_t *u = ui_find(id);
    return u ? u->checked : 0;
}

void ui_set_checked(int id, int on)
{
    uictl_t *u = ui_find(id);
    if (u) {
        u->checked = on ? 1 : 0;
        InvalidateRect(u->h, NULL, FALSE);
    }
}

/* Nachrichtenfenster nur im Normalbetrieb (Test-/Shot-Modi: Text auf
 * die Konsole, Antwort = Standard, nie ein Fenster). */
int ui_msgbox(HWND owner, const char *text, const char *title,
                     UINT flags)
{
    if (g_o.shot != 0) {
        printf("[%s] %s\n", title, text);
        return (flags & MB_YESNO) ? IDNO : IDOK;
    }
    {
        wchar_t *wt = u8_wdup(text), *wc = u8_wdup(title);
        int r = MessageBoxW(owner, wt, wc, flags);
        free(wt);
        free(wc);
        return r;
    }
}

/* ------------------------------------------------------------------ */
/* Zeichenflaeche (32-bit-DIB, eigenes Alpha-Mischen mit Kantenglaettung) */
/* ------------------------------------------------------------------ */

canvas_t g_base, g_bb; /* Hintergrund-Schicht / fertiges Bild */

void cv_free(canvas_t *c)
{
    if (c->dc) {
        SelectObject(c->dc, c->old);
        DeleteDC(c->dc);
    }
    if (c->bmp) {
        DeleteObject(c->bmp);
    }
    memset(c, 0, sizeof *c);
}

int cv_make(canvas_t *c, int w, int h)
{
    BITMAPINFO bi;
    void *bits = NULL;
    if (c->dc && c->w == w && c->h == h) {
        return 1;
    }
    cv_free(c);
    if (w <= 0 || h <= 0) {
        return 0;
    }
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    c->dc = CreateCompatibleDC(NULL);
    c->bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!c->dc || !c->bmp || !bits) {
        cv_free(c);
        return 0;
    }
    c->px = (uint32_t *)bits;
    c->old = SelectObject(c->dc, c->bmp);
    c->w = w;
    c->h = h;
    return 1;
}

static float rr_cov(float px, float py, float x, float y, float w, float h,
                    float r)
{
    float hw = w * 0.5f, hh = h * 0.5f, qx, qy, ox, oy, d, m;
    if (w <= 0 || h <= 0) {
        return 0.f;
    }
    if (r > hw) {
        r = hw;
    }
    if (r > hh) {
        r = hh;
    }
    qx = fabsf(px - (x + hw)) - (hw - r);
    qy = fabsf(py - (y + hh)) - (hh - r);
    ox = qx > 0 ? qx : 0;
    oy = qy > 0 ? qy : 0;
    m = qx > qy ? qx : qy;
    d = sqrtf(ox * ox + oy * oy) + (m < 0 ? m : 0) - r;
    d = 0.5f - d;
    return d < 0 ? 0.f : d > 1 ? 1.f : d;
}

static void px_blend(uint32_t *p, int r, int g, int b, int a256)
{
    uint32_t d = *p;
    int dr = (int)((d >> 16) & 255), dg = (int)((d >> 8) & 255),
        db = (int)(d & 255);
    dr += ((r - dr) * a256) >> 8;
    dg += ((g - dg) * a256) >> 8;
    db += ((b - db) * a256) >> 8;
    *p = ((uint32_t)dr << 16) | ((uint32_t)dg << 8) | (uint32_t)db;
}

/* Gefuelltes (abgerundetes) Rechteck, alpha 0..255, Pixel-Koordinaten. */
void cv_fill(canvas_t *c, int x, int y, int w, int h, COLORREF col,
                    int alpha, int rad)
{
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > c->w ? c->w : x + w, y1 = y + h > c->h ? c->h : y + h;
    int R = GetRValue(col), G = GetGValue(col), B = GetBValue(col);
    int xx, yy;
    if (!c->px || x0 >= x1 || y0 >= y1) {
        return;
    }
    GdiFlush();
    for (yy = y0; yy < y1; yy++) {
        uint32_t *row = c->px + (size_t)yy * (size_t)c->w;
        int inner_y = rad <= 0 || (yy >= y + rad && yy < y + h - rad);
        for (xx = x0; xx < x1; xx++) {
            int a;
            if (inner_y || (xx >= x + rad && xx < x + w - rad)) {
                a = alpha * 256 / 255;
            } else {
                float cv = rr_cov((float)xx + 0.5f, (float)yy + 0.5f,
                                  (float)x, (float)y, (float)w, (float)h,
                                  (float)rad);
                a = (int)(alpha * cv * 256.f / 255.f);
            }
            if (a > 0) {
                px_blend(row + xx, R, G, B, a);
            }
        }
    }
}

/* Rahmen der Dicke t (abgerundet). */
void cv_ring(canvas_t *c, int x, int y, int w, int h, COLORREF col,
                    int alpha, int rad, int t)
{
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > c->w ? c->w : x + w, y1 = y + h > c->h ? c->h : y + h;
    int R = GetRValue(col), G = GetGValue(col), B = GetBValue(col);
    int xx, yy;
    float ir = (float)(rad - t);
    if (!c->px || x0 >= x1 || y0 >= y1) {
        return;
    }
    if (ir < 0) {
        ir = 0;
    }
    GdiFlush();
    for (yy = y0; yy < y1; yy++) {
        uint32_t *row = c->px + (size_t)yy * (size_t)c->w;
        for (xx = x0; xx < x1; xx++) {
            float co, ci, cv;
            int a;
            /* Innenbereich weit weg von Kanten: ueberspringen */
            if (xx >= x + rad + t && xx < x + w - rad - t && yy >= y + t + 1 &&
                yy < y + h - t - 1) {
                continue;
            }
            if (yy >= y + rad + t && yy < y + h - rad - t && xx >= x + t + 1 &&
                xx < x + w - t - 1) {
                continue;
            }
            co = rr_cov((float)xx + 0.5f, (float)yy + 0.5f, (float)x,
                        (float)y, (float)w, (float)h, (float)rad);
            ci = rr_cov((float)xx + 0.5f, (float)yy + 0.5f, (float)(x + t),
                        (float)(y + t), (float)(w - 2 * t),
                        (float)(h - 2 * t), ir);
            cv = co - ci;
            if (cv <= 0) {
                continue;
            }
            a = (int)(alpha * cv * 256.f / 255.f);
            if (a > 0) {
                px_blend(row + xx, R, G, B, a);
            }
        }
    }
}

void cv_text(canvas_t *c, HFONT f, const char *u8, int x, int y,
                    int w, int h, COLORREF col, UINT fmt)
{
    RECT r;
    wchar_t *wt;
    if (!c->dc || !u8 || !*u8) {
        return;
    }
    wt = u8_wdup(u8);
    if (!wt) {
        return;
    }
    r.left = x;
    r.top = y;
    r.right = x + w;
    r.bottom = y + h;
    SelectObject(c->dc, f);
    SetBkMode(c->dc, TRANSPARENT);
    SetTextColor(c->dc, col);
    DrawTextW(c->dc, wt, -1, &r, fmt | DT_NOPREFIX);
    free(wt);
}

void cv_textw(canvas_t *c, HFONT f, const wchar_t *wt, int n, int x,
                     int y, int w, int h, COLORREF col, UINT fmt)
{
    RECT r;
    r.left = x;
    r.top = y;
    r.right = x + w;
    r.bottom = y + h;
    SelectObject(c->dc, f);
    SetBkMode(c->dc, TRANSPARENT);
    SetTextColor(c->dc, col);
    DrawTextW(c->dc, wt, n, &r, fmt | DT_NOPREFIX);
}

/* Logische Wrapper auf das fertige Bild g_bb. */
/* W281: Layout b bildet das Entwurfsraster ab (launcher_layout.c). */
int lay_b(void)
{
    return g_lay.id == LLAY_B;
}
int lay_tw(void)
{
    return lay_b() ? g_lay.tw : WIN_W;
}
int lay_th(void)
{
    return lay_b() ? g_lay.th : WIN_H;
}
static int lay_nav(void)
{
    return lay_b() ? g_lay.nav : NAV_W;
}
/* Pixel-Rechteck eines Entwurfs-Rechtecks (sw/sh: Breite/Hoehe strecken). */
void lay_px(int x, int y, int w, int h, int sw, int sh, RECT *r)
{
    int o[4];
    if (lay_b()) {
        llay_map_rect(&g_lay, x, y, w, h, sw, sh, o);
        x = o[0];
        y = o[1];
        w = o[2];
        h = o[3];
    }
    r->left = SX(x);
    r->top = SX(y);
    r->right = SX(x + w);
    r->bottom = SX(y + h);
}
/* Gezeichnete Flaeche/Text (Streckregel llay_map_paint). */
void lay_paint_px(int x, int y, int w, int h, RECT *r)
{
    int o[4];
    llay_map_paint(&g_lay, x, y, w, h, o);
    r->left = SX(o[0]);
    r->top = SX(o[1]);
    r->right = SX(o[0] + o[2]);
    r->bottom = SX(o[1] + o[3]);
}

void L_fill(int x, int y, int w, int h, COLORREF c, int a, int r)
{
    if (lay_b()) {
        RECT q;
        lay_paint_px(x, y, w, h, &q);
        cv_fill(&g_bb, q.left, q.top, q.right - q.left, q.bottom - q.top, c, a, SX(r));
        return;
    }
    cv_fill(&g_bb, SX(x), SX(y), SX(x + w) - SX(x), SX(y + h) - SX(y), c, a,
            SX(r));
}

void L_ring(int x, int y, int w, int h, COLORREF c, int a, int r)
{
    int t = SX(1) < 1 ? 1 : SX(1);
    if (lay_b()) {
        RECT q;
        lay_paint_px(x, y, w, h, &q);
        cv_ring(&g_bb, q.left, q.top, q.right - q.left, q.bottom - q.top, c, a, SX(r), t);
        return;
    }
    cv_ring(&g_bb, SX(x), SX(y), SX(x + w) - SX(x), SX(y + h) - SX(y), c, a,
            SX(r), t);
}

void L_text(int font, const char *u8, int x, int y, int w, int h,
                   COLORREF c, UINT fmt)
{
    if (lay_b()) {
        RECT q;
        lay_paint_px(x, y, w, h, &q);
        if ((fmt & DT_WORDBREAK) && h >= 28 && u8 && *u8 && g_bb.dc) {
            /* W281: Absatz waechst mit der Flaeche (wie Karten) und darf in den
             * leeren Entwurfsabstand darunter (16) laufen - weniger Abschneiden. */
            wchar_t *wt = u8_wdup(u8);
            RECT m = { 0, 0, q.right - q.left, 0 };
            int cap = SX(llay_mapy(&g_lay, y + h + 16)) - q.top;
            int st = SX(llay_mapy(&g_lay, y + h)) - q.top;
            if (wt) {
                SelectObject(g_bb.dc, g_f[font]);
                DrawTextW(g_bb.dc, wt, -1, &m, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
                free(wt);
                q.bottom = q.top + (st > m.bottom ? st : (m.bottom < cap ? m.bottom : cap));
            }
        }
        cv_text(&g_bb, g_f[font], u8, q.left, q.top, q.right - q.left, q.bottom - q.top, c,
                fmt);
        return;
    }
    cv_text(&g_bb, g_f[font], u8, SX(x), SX(y), SX(x + w) - SX(x),
            SX(y + h) - SX(y), c, fmt);
}

/* ------------------------------------------------------------------ */
/* Schriften                                                          */
/* ------------------------------------------------------------------ */
static HFONT mkfont(const wchar_t *const *faces, int pt10, int weight,
                    wchar_t *used, int used_cap)
{
    HDC dc = GetDC(NULL);
    HFONT f = NULL;
    int i;
    for (i = 0; faces[i]; i++) {
        wchar_t got[64];
        f = CreateFontW(-MulDiv(pt10, g_dpi_font, 720), 0, 0, 0, weight, FALSE, /* W281 */
                        FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH | FF_DONTCARE, faces[i]);
        if (!f) {
            continue;
        }
        if (!faces[i + 1]) {
            break; /* letzte Wahl: nehmen */
        }
        {
            HGDIOBJ o = SelectObject(dc, f);
            got[0] = 0;
            GetTextFaceW(dc, 64, got);
            SelectObject(dc, o);
        }
        if (_wcsicmp(got, faces[i]) == 0) {
            break;
        }
        DeleteObject(f);
        f = NULL;
    }
    if (f && used) {
        HGDIOBJ o = SelectObject(dc, f);
        GetTextFaceW(dc, used_cap, used);
        SelectObject(dc, o);
    }
    ReleaseDC(NULL, dc);
    if (!f) {
        f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    }
    return f;
}

void fonts_free(void)
{
    int i;
    for (i = 0; i < F_N; i++) {
        if (g_f[i]) {
            DeleteObject(g_f[i]);
            g_f[i] = NULL;
        }
    }
}

void fonts_make(void)
{
    static const wchar_t *const text[] = { L"Segoe UI Variable Text",
                                           L"Segoe UI", NULL };
    static const wchar_t *const semi[] = {
        L"Segoe UI Variable Text Semibold", L"Segoe UI Semibold",
        L"Segoe UI", NULL
    };
    static const wchar_t *const disp[] = {
        L"Segoe UI Variable Display Semib", L"Segoe UI Variable Display",
        L"Segoe UI Semibold", L"Segoe UI", NULL
    };
    static const wchar_t *const bold[] = { L"Segoe UI Variable Display",
                                           L"Segoe UI", NULL };
    static const wchar_t *const icon[] = { L"Segoe Fluent Icons",
                                           L"Segoe MDL2 Assets", NULL };
    static const wchar_t *const mono[] = { L"Cascadia Mono", L"Consolas",
                                           NULL };
    wchar_t used[64];
    fonts_free();
    g_f[F_BODY] = mkfont(text, 100, FW_NORMAL, g_face_body, 64);
    g_f[F_SMALL] = mkfont(text, 90, FW_NORMAL, NULL, 0);
    g_f[F_TINY] = mkfont(text, 82, FW_NORMAL, NULL, 0);
    g_f[F_SEMI] = mkfont(semi, 100, FW_SEMIBOLD, NULL, 0);
    g_f[F_CARD] = mkfont(disp, 110, FW_SEMIBOLD, NULL, 0);
    g_f[F_BRAND] = mkfont(disp, 105, FW_SEMIBOLD, NULL, 0);
    g_f[F_HERO] = mkfont(bold, 300, FW_BOLD, NULL, 0);
    g_f[F_BIG] = mkfont(disp, 140, FW_SEMIBOLD, NULL, 0);
    g_f[F_GAME] = mkfont(disp, 150, FW_SEMIBOLD, NULL, 0); /* W398 */
    g_f[F_MONO] = mkfont(mono, 90, FW_NORMAL, NULL, 0);
    used[0] = 0;
    g_f[F_ICON] = mkfont(icon, 110, FW_NORMAL, used, 64);
    g_f[F_ICONSM] = mkfont(icon, 75, FW_NORMAL, NULL, 0);
    g_has_icons = _wcsicmp(used, L"Segoe Fluent Icons") == 0 ||
                  _wcsicmp(used, L"Segoe MDL2 Assets") == 0;
    g_icon_fluent = _wcsicmp(used, L"Segoe Fluent Icons") == 0;
}

/* ------------------------------------------------------------------ */
/* Hintergrundbild (Datei res/launcher_bg.png neben der Exe, per WIC)  */
/* ------------------------------------------------------------------ */
static const GUID kCLSID_WICFactory = {
    0xcacaf262, 0x9370, 0x4615, { 0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a }
};
static const GUID kIID_WICFactory = {
    0xec5ec8a9, 0xc395, 0x4314, { 0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70 }
};
static const GUID kFmt32BGR = {
    0x6fddc324, 0x4e03, 0x4bfe, { 0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0e }
};
static const GUID kPngContainer = {
    0x1b7cfaf4, 0x713f, 0x473c, { 0xbb, 0xcd, 0x61, 0x37, 0x42, 0x5f, 0xae, 0xaf }
};

static IWICImagingFactory *g_wic;
uint32_t *g_img;  /* Quelle 32bpp BGR */
int g_img_w, g_img_h;
uint32_t *g_bgs;  /* skaliert + ausgeblichen fuer g_bgs_w x g_bgs_h */
static int g_bgs_w, g_bgs_h;

static int wic_init(void)
{
    if (g_wic) {
        return 1;
    }
    if (FAILED(CoCreateInstance(&kCLSID_WICFactory, NULL,
                                CLSCTX_INPROC_SERVER, &kIID_WICFactory,
                                (void **)&g_wic))) {
        g_wic = NULL;
        return 0;
    }
    return 1;
}

/* W535 AVVT: Datei ganz einlesen (bis LAUNCHER_BG_MAX). -> malloc-Puffer oder NULL. */
static BYTE *bg_read_file(const wchar_t *path, DWORD *n_out)
{
    HANDLE f;
    LARGE_INTEGER sz;
    BYTE *buf = NULL;
    DWORD got = 0;
    *n_out = 0;
    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        return NULL;
    }
    if (GetFileSizeEx(f, &sz) && sz.QuadPart > 0 && sz.QuadPart <= (LONGLONG)LAUNCHER_BG_MAX) {
        buf = (BYTE *)malloc((size_t)sz.QuadPart);
        if (buf && (!ReadFile(f, buf, (DWORD)sz.QuadPart, &got, NULL) || got != (DWORD)sz.QuadPart)) {
            free(buf);
            buf = NULL;
        }
    }
    CloseHandle(f);
    if (buf) {
        *n_out = got;
    }
    return buf;
}

/* W535 AVVT: Pfad des Hintergrundbilds (Exe-Ordner + LAUNCHER_BG_REL). 1 = ok. */
int bg_default_path(wchar_t *out, size_t cap)
{
    wchar_t *sep;
    DWORD n = GetModuleFileNameW(NULL, out, (DWORD)cap);
    if (n == 0 || n >= cap) {
        return 0;
    }
    sep = wcsrchr(out, L'\\');
    if (!sep) {
        return 0;
    }
    sep[1] = 0;
    return wcscat_s(out, cap, LAUNCHER_BG_REL) == 0;
}

/* PNG-Datei dekodieren (W535: statt RCDATA-Ressource). 1 = ok, 0 = fehlt/unlesbar -> einfarbiger Verlauf.
 * path NULL = Standardpfad neben der Exe. Setzt bei Erfolg g_img/g_img_w/g_img_h. */
int bg_load_file(const wchar_t *path)
{
    wchar_t def[MAX_PATH + 32];
    BYTE *p;
    DWORD n = 0;
    IWICStream *st = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *fr = NULL;
    IWICFormatConverter *cvt = NULL;
    UINT w = 0, h = 0;
    int ok = 0;
    if (!wic_init()) {
        return 0;
    }
    if (!path) {
        if (!bg_default_path(def, sizeof(def) / sizeof(def[0]))) {
            return 0;
        }
        path = def;
    }
    p = bg_read_file(path, &n);
    if (!p) {
        return 0;
    }
    if (SUCCEEDED(IWICImagingFactory_CreateStream(g_wic, &st)) &&
        SUCCEEDED(IWICStream_InitializeFromMemory(st, p, n)) &&
        SUCCEEDED(IWICImagingFactory_CreateDecoderFromStream(
            g_wic, (IStream *)st, NULL, WICDecodeMetadataCacheOnDemand,
            &dec)) &&
        SUCCEEDED(IWICBitmapDecoder_GetFrame(dec, 0, &fr)) &&
        SUCCEEDED(IWICImagingFactory_CreateFormatConverter(g_wic, &cvt)) &&
        SUCCEEDED(IWICFormatConverter_Initialize(
            cvt, (IWICBitmapSource *)fr, &kFmt32BGR, WICBitmapDitherTypeNone,
            NULL, 0.0, WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(IWICFormatConverter_GetSize(cvt, &w, &h)) && w > 0 &&
        h > 0 && w <= 8192 && h <= 8192) {
        uint32_t *img = (uint32_t *)malloc((size_t)w * h * 4);
        if (img &&
            SUCCEEDED(IWICFormatConverter_CopyPixels(cvt, NULL, w * 4,
                                                     w * h * 4,
                                                     (BYTE *)img))) {
            free(g_img); /* erneutes Laden (Selbsttest) ersetzt das alte Bild */
            g_img = img;
            g_img_w = (int)w;
            g_img_h = (int)h;
            g_bgs_w = g_bgs_h = 0; /* Skalier-Cache neu bauen */
            ok = 1;
        } else {
            free(img);
        }
    }
    if (cvt) {
        IWICFormatConverter_Release(cvt);
    }
    if (fr) {
        IWICBitmapFrameDecode_Release(fr);
    }
    if (dec) {
        IWICBitmapDecoder_Release(dec);
    }
    if (st) {
        IWICStream_Release(st);
    }
    free(p); /* WIC-Stream ist freigegeben, Pixel liegen kopiert in g_img */
    return ok;
}

/* Hintergrundbild einmal laden (Standardpfad). 1 = ok. */
int bg_load(void)
{
    static int tried;
    if (g_img) {
        return 1;
    }
    if (tried) {
        return 0; /* fehlende Datei nicht bei jedem Neuaufbau erneut suchen */
    }
    tried = 1;
    return bg_load_file(NULL);
}

/* Zuschnitt "fuellen ohne Strecken": groesster Ausschnitt der Quelle mit
 * dem Seitenverhaeltnis des Ziels (Mitte, vertikal leicht oben). */
void bg_cover_rect(int sw, int sh, int dw, int dh, WICRect *out)
{
    double sa = (double)sw / sh, da = (double)dw / dh;
    if (da > sa) { /* Ziel breiter: Hoehe beschneiden */
        int ch = (int)(sw / da + 0.5);
        out->X = 0;
        out->Width = sw;
        out->Height = ch > sh ? sh : ch;
        out->Y = (int)((sh - out->Height) * 0.45);
    } else {       /* Ziel schmaler: Breite beschneiden */
        int cw = (int)(sh * da + 0.5);
        out->Y = 0;
        out->Height = sh;
        out->Width = cw > sw ? sw : cw;
        out->X = (sw - out->Width) / 2;
    }
}

/* Skaliertes, ausgeblichenes Bild fuer dw x dh bauen (Cache). */
int bg_scaled(int dw, int dh)
{
    IWICBitmap *src = NULL;
    IWICBitmapClipper *clip = NULL;
    IWICBitmapScaler *sc = NULL;
    WICRect cr;
    int ok = 0;
    if (g_bgs && g_bgs_w == dw && g_bgs_h == dh) {
        return 1;
    }
    if (!g_img || !wic_init() || dw <= 0 || dh <= 0) {
        return 0;
    }
    free(g_bgs);
    g_bgs = (uint32_t *)malloc((size_t)dw * dh * 4);
    if (!g_bgs) {
        return 0;
    }
    bg_cover_rect(g_img_w, g_img_h, dw, dh, &cr);
    if (SUCCEEDED(IWICImagingFactory_CreateBitmapFromMemory(
            g_wic, (UINT)g_img_w, (UINT)g_img_h, &kFmt32BGR,
            (UINT)g_img_w * 4, (UINT)(g_img_w * g_img_h * 4), (BYTE *)g_img,
            &src)) &&
        SUCCEEDED(IWICImagingFactory_CreateBitmapClipper(g_wic, &clip)) &&
        SUCCEEDED(IWICBitmapClipper_Initialize(clip, (IWICBitmapSource *)src,
                                               &cr)) &&
        SUCCEEDED(IWICImagingFactory_CreateBitmapScaler(g_wic, &sc)) &&
        SUCCEEDED(IWICBitmapScaler_Initialize(sc, (IWICBitmapSource *)clip,
                                              (UINT)dw, (UINT)dh,
                                              WICBitmapInterpolationModeFant)) &&
        SUCCEEDED(IWICBitmapScaler_CopyPixels(sc, NULL, (UINT)dw * 4,
                                              (UINT)(dw * dh * 4),
                                              (BYTE *)g_bgs))) {
        /* Ausbleichen/abdunkeln: Graustufe -> dunkle Palette, links
         * (Navigation) und unten staerker, leichter Kaltton. */
        int x, y;
        for (y = 0; y < dh; y++) {
            double fy = (double)y / dh;
            double vy = 1.0 - 0.30 * fy * fy;
            for (x = 0; x < dw; x++) {
                uint32_t p = g_bgs[(size_t)y * dw + x];
                int g = (int)(((p >> 16) & 255) * 30 + ((p >> 8) & 255) * 59 +
                              (p & 255) * 11) / 100;
                double fx = (double)x / dw;
                double k = (0.20 + 0.24 * fx) * vy;
                int r = 11 + (int)(g * k * 0.96);
                int gg = 13 + (int)(g * k * 1.00);
                int b = 17 + (int)(g * k * 1.08);
                if (r > 255) r = 255;
                if (gg > 255) gg = 255;
                if (b > 255) b = 255;
                g_bgs[(size_t)y * dw + x] =
                    ((uint32_t)r << 16) | ((uint32_t)gg << 8) | (uint32_t)b;
            }
        }
        g_bgs_w = dw;
        g_bgs_h = dh;
        ok = 1;
    }
    if (sc) {
        IWICBitmapScaler_Release(sc);
    }
    if (clip) {
        IWICBitmapClipper_Release(clip);
    }
    if (src) {
        IWICBitmap_Release(src);
    }
    if (!ok) {
        free(g_bgs);
        g_bgs = NULL;
    }
    return ok;
}

/* 32bpp-Pixel als PNG speichern (WIC). 0 = ok. */
int png_save(const uint32_t *px, int w, int h, const char *path)
{
    IWICStream *st = NULL;
    IWICBitmapEncoder *enc = NULL;
    IWICBitmapFrameEncode *fr = NULL;
    IPropertyBag2 *props = NULL;
    IWICBitmap *bmp = NULL;
    wchar_t wp[1024];
    int rc = 1;
    if (!wic_init()) {
        return 1;
    }
    u8_to_w(path, wp, 1024);
    if (SUCCEEDED(IWICImagingFactory_CreateBitmapFromMemory(
            g_wic, (UINT)w, (UINT)h, &kFmt32BGR, (UINT)w * 4,
            (UINT)(w * h * 4), (BYTE *)px, &bmp)) &&
        SUCCEEDED(IWICImagingFactory_CreateStream(g_wic, &st)) &&
        SUCCEEDED(IWICStream_InitializeFromFilename(st, wp, GENERIC_WRITE)) &&
        SUCCEEDED(IWICImagingFactory_CreateEncoder(g_wic, &kPngContainer,
                                                   NULL, &enc)) &&
        SUCCEEDED(IWICBitmapEncoder_Initialize(enc, (IStream *)st,
                                               WICBitmapEncoderNoCache)) &&
        SUCCEEDED(IWICBitmapEncoder_CreateNewFrame(enc, &fr, &props)) &&
        SUCCEEDED(IWICBitmapFrameEncode_Initialize(fr, props)) &&
        SUCCEEDED(IWICBitmapFrameEncode_SetSize(fr, (UINT)w, (UINT)h)) &&
        SUCCEEDED(IWICBitmapFrameEncode_WriteSource(
            fr, (IWICBitmapSource *)bmp, NULL)) &&
        SUCCEEDED(IWICBitmapFrameEncode_Commit(fr)) &&
        SUCCEEDED(IWICBitmapEncoder_Commit(enc))) {
        rc = 0;
    }
    if (props) {
        IPropertyBag2_Release(props);
    }
    if (fr) {
        IWICBitmapFrameEncode_Release(fr);
    }
    if (enc) {
        IWICBitmapEncoder_Release(enc);
    }
    if (st) {
        IWICStream_Release(st);
    }
    if (bmp) {
        IWICBitmap_Release(bmp);
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* Launcher-eigene Einstellung (Hintergrundbild): launcher_ui.cfg      */
/* ------------------------------------------------------------------ */
void ui_cfg_path(const char *client_dir, char *out, size_t cap)
{
    _snprintf_s(out, cap, _TRUNCATE, "%s/launcher_ui.cfg", client_dir);
}

/* key=0/1 aus der UI-cfg (dflt, wenn Datei/Schluessel fehlt). */
int ui_cfg_load_key(const char *client_dir, const char *key, int dflt)
{
    char p[1200], line[256];
    FILE *f = NULL;
    size_t kl = strlen(key);
    int v = dflt;
    ui_cfg_path(client_dir, p, sizeof p);
    if (u8_fopen_s(&f, p, "r") == 0 && f) {
        while (fgets(line, sizeof line, f)) {
            if (strncmp(line, key, kl) == 0 && line[kl] == '=') {
                v = atoi(line + kl + 1) ? 1 : 0;
            }
        }
        fclose(f);
    }
    return v;
}

/* W281 LAUNCHER-SIZE: Text-Wert aus der UI-cfg (leer, wenn er fehlt). */
void ui_cfg_load_str(const char *client_dir, const char *key, char *out, size_t cap)
{
    char p[1200], line[256];
    FILE *f = NULL;
    size_t kl = strlen(key);
    out[0] = '\0';
    ui_cfg_path(client_dir, p, sizeof p);
    if (u8_fopen_s(&f, p, "r") == 0 && f) {
        while (fgets(line, sizeof line, f)) {
            if (strncmp(line, key, kl) == 0 && line[kl] == '=') {
                char *v = line + kl + 1, *e = v + strlen(v);
                while (e > v && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) {
                    *--e = '\0';
                }
                strncpy_s(out, cap, v, _TRUNCATE);
            }
        }
        fclose(f);
    }
}

/* W281: gemerkte Layout-Wahl (launcher_ui.cfg launcher_layout / ui_size);
 * -1 = nie gewaehlt (dann schreibt ui_cfg_save_bg nichts dazu, Layout 0
 * bleibt byte-gleich). */
int g_lay_cfg_id = -1, g_lay_cfg_size = -1;

/* W301 BETA-VERSION: Seite "Test tools" - neuer Schluessel test_tools, alte
 * launcher_ui.cfg mit alpha_tools gelten weiter (Rueckfall), Standard 1. */
int ui_cfg_load_tools(const char *client_dir)
{
    int v = ui_cfg_load_key(client_dir, "test_tools", -1);
    return v >= 0 ? v : ui_cfg_load_key(client_dir, "alpha_tools", 1);
}

int ui_cfg_load_bg(const char *client_dir)
{
    return ui_cfg_load_key(client_dir, "bg_image", 1); /* Standard: an */
}

int ui_cfg_save_bg(const char *client_dir, int bg)
{
    char p[1200];
    FILE *f = NULL;
    ui_cfg_path(client_dir, p, sizeof p);
    if (u8_fopen_s(&f, p, "w") != 0 || !f) {
        return 1;
    }
    /* W171: + diag_open (Erweitert > Diagnose aufgeklappt) */
    fprintf(f, "# Launcher-Oberflaeche (W82), nicht vom Client gelesen\n"
               "bg_image=%d\n"
               "diag_open=%d\n"
               "test_tools=%d\n" /* W272/W301: Seite Test tools (0 = aus; alte cfg: alpha_tools) */
               "show_advanced=%d\n", /* W291: Feinregler auf Anzeige/Grafik */
            bg ? 1 : 0, g_diag_open ? 1 : 0, g_alpha_tools ? 1 : 0, g_show_adv ? 1 : 0);
    if (g_loctp_mode_cfg >= 0) { /* W351: Testpartner-Ablauf */
        fprintf(f, "loctp_mode=%d\n", g_loctp_mode_cfg);
    }
    if (g_lay_cfg_id >= 0) { /* W281: Layout-Vorschau 0|a|b + UI-Groesse */
        fprintf(f, "launcher_layout=%s\nui_size=%s\n", llay_name(g_lay_cfg_id),
                llay_size_name(g_lay_cfg_size >= 0 ? g_lay_cfg_size : LLAY_SZ_NORMAL));
    }
    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Texte                                                              */
/* ------------------------------------------------------------------ */


/* W107 EDGE-MODE: Verlaufsbreite (% der 4:3-Breite) und Abdunklung (%).
 * W170 LAUNCHER-P1/B19: Standard 10 % ist ein Listeneintrag (kein Roheintrag). */

/* ------------------------------------------------------------------ */
/* Steuerelemente anlegen / anordnen                                   */
/* ------------------------------------------------------------------ */
static LRESULT CALLBACK hover_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp,
                                   UINT_PTR sid, DWORD_PTR ref)
{
    uictl_t *u;
    (void)sid;
    (void)ref;
    switch (msg) {
    case WM_MOUSEMOVE:
        u = ui_by_hwnd(h);
        if (u && !u->hover) {
            TRACKMOUSEEVENT t;
            u->hover = 1;
            memset(&t, 0, sizeof t);
            t.cbSize = sizeof t;
            t.dwFlags = TME_LEAVE;
            t.hwndTrack = h;
            TrackMouseEvent(&t);
            InvalidateRect(h, NULL, FALSE);
        }
        break;
    case WM_MOUSELEAVE:
        u = ui_by_hwnd(h);
        if (u) {
            u->hover = 0;
            InvalidateRect(h, NULL, FALSE);
        }
        break;
    case WM_SETCURSOR: /* W398: Handcursor auf dem inaktiven Spielnamen (Wortmarken-Tab) */
        u = ui_by_hwnd(h);
        if (u && gametab_id(u->id) && !gametab_active(u) && LOWORD(lp) == HTCLIENT) { /* W473: auch in Optionen/steelnet */
            SetCursor(LoadCursor(NULL, IDC_HAND));
            return TRUE;
        }
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_NCDESTROY:
        RemoveWindowSubclass(h, hover_proc, 1);
        break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

void ui_place(uictl_t *u);

HWND mk(HWND parent, int kind, int id, int page, int x, int y, int w,
               int h, const char *text, DWORD extra)
{
    uictl_t *u;
    const wchar_t *cls = L"BUTTON";
    DWORD st = WS_CHILD | extra;
    HWND hw;
    wchar_t *wt;
    if (g_ui_n >= UI_MAX) {
        static int warned;
        if (!warned++) {
            fprintf(stderr, "launcher: UI_MAX (%d) erreicht - Steuerelement %d fehlt\n", UI_MAX, id); /* W373 */
        }
        return NULL;
    }
    switch (kind) {
    case K_EDIT:
        cls = L"EDIT";
        st |= ES_AUTOHSCROLL | WS_TABSTOP;
        break;
    case K_MEMO:
        cls = L"EDIT";
        st |= ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL |
              WS_TABSTOP;
        break;
    case K_COMBO:
        cls = WC_COMBOBOXW;
        st |= CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP;
        break;
    case K_COMBOEDIT:
        cls = WC_COMBOBOXW;
        st |= CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL | WS_TABSTOP;
        break;
    case K_STATIC:
        cls = L"STATIC";
        st |= SS_LEFT | SS_NOPREFIX;
        break;
    case K_ICON: /* W287: Symbol; SS_NOTIFY = Maus fuer den Tooltip, kein Tabstopp */
        cls = L"STATIC";
        st |= SS_OWNERDRAW | SS_NOTIFY | SS_NOPREFIX;
        break;
    case K_LIST:
        cls = L"LISTBOX";
        st |= LBS_NOTIFY | WS_VSCROLL | WS_HSCROLL | WS_BORDER | WS_TABSTOP | /* W148: +HScroll */
              LBS_NOINTEGRALHEIGHT;
        break;
    case K_TRACK:
        cls = TRACKBAR_CLASSW;
        st |= TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP;
        break;
    case K_PKLIST: /* W172: Zeilen zeichnet packs_draw_row, Daten = Index */
        cls = L"LISTBOX";
        st |= LBS_NOTIFY | LBS_OWNERDRAWFIXED | WS_VSCROLL | WS_TABSTOP |
              LBS_NOINTEGRALHEIGHT;
        break;
    case K_TBMIN:
    case K_TBCLOSE:
        st |= BS_OWNERDRAW;
        break;
    default:
        st |= BS_OWNERDRAW | WS_TABSTOP;
        break;
    }
    wt = u8_wdup(text ? text : "");
    hw = CreateWindowExW(0, cls, wt, st, 0, 0, 10, 10, parent,
                         (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    free(wt);
    if (!hw) {
        return NULL;
    }
    u = &g_ui[g_ui_n++];
    memset(u, 0, sizeof *u);
    u->h = hw;
    u->parent = parent;
    u->id = id;
    u->page = page;
    u->kind = kind;
    u->x = x;
    u->y = y;
    u->w = w;
    u->h_ = h;
    if (kind < K_EDIT) {
        SetWindowSubclass(hw, hover_proc, 1, 0);
    }
    if (kind == K_COMBO || kind == K_COMBOEDIT) {
        SetWindowTheme(hw, L"DarkMode_CFD", NULL);
        {
            COMBOBOXINFO ci;
            memset(&ci, 0, sizeof ci);
            ci.cbSize = sizeof ci;
            if (GetComboBoxInfo(hw, &ci) && ci.hwndList) {
                SetWindowTheme(ci.hwndList, L"DarkMode_Explorer", NULL);
            }
        }
    } else if (kind == K_MEMO || kind == K_LIST || kind == K_PKLIST) {
        SetWindowTheme(hw, L"DarkMode_Explorer", NULL);
    }
    ui_place(u);
    return hw;
}

/* W281 LAUNCHER-SIZE: Layout b ordnet die Spielen-Seite neu (Start-Bereich,
 * Statuskarten). Entwurfs-Rechteck je Element, 0 = keine Vorgabe. */
static int lay_b_override(int id, int o[4])
{
    if (!lay_b()) {
        return 0;
    }
    if (id == ID_START_BIG) {
        llay_b_start_big(o);
        return 1;
    }
    if (id == ID_START_VR) {
        llay_b_start_vr(o);
        return 1;
    }
    if (id >= ID_SUM_GO0 && id < ID_SUM_GO0 + SUM_GO_N) {
        llay_b_card_go(id - ID_SUM_GO0, o);
        return 1;
    }
    return 0;
}

/* W281: Pixel-Rechteck eines Elements (Layout 0/a wie bisher SX). */
void ui_px_rect(const uictl_t *u, RECT *r)
{
    int o[4], sh;
    if (!lay_b() || u->parent != g_hwnd) {
        r->left = SX(u->x);
        r->top = SX(u->y);
        r->right = SX(u->x + u->w);
        r->bottom = SX(u->y + u->h_);
        return;
    }
    if (!lay_b_override(u->id, o)) {
        o[0] = u->x;
        o[1] = u->y;
        o[2] = u->w;
        o[3] = u->h_;
    }
    /* Listen/Memos strecken mit, Knoepfe/Kombis/Felder behalten die Hoehe */
    sh = u->kind == K_MEMO || u->kind == K_LIST || u->kind == K_PKLIST ||
         (u->kind == K_STATIC && o[3] >= 48);
    lay_px(o[0], o[1], o[2], o[3], 1, sh, r);
}

/* Pixel-Rechteck eines Elements (Rahmen; Edits liegen innen). */
void ui_place(uictl_t *u)
{
    RECT pr;
    int x, y, w, h;
    ui_px_rect(u, &pr);
    x = pr.left;
    y = pr.top;
    w = pr.right - pr.left;
    h = pr.bottom - pr.top;
    HFONT f = g_f[F_BODY];
    if (u->kind == K_MEMO) {
        f = g_f[F_SMALL];
    }
    if (u->kind == K_STATIC && u->parent != g_hwnd) {
        f = g_f[F_SMALL];
    }
    SendMessageW(u->h, WM_SETFONT, (WPARAM)f, FALSE);
    if (u->kind == K_EDIT) {
        /* Text vertikal mittig: Edit innen, Rahmen zeichnet compose(). */
        TEXTMETRICW tm;
        HDC dc = GetDC(u->h);
        HGDIOBJ o = SelectObject(dc, f);
        int th;
        GetTextMetricsW(dc, &tm);
        SelectObject(dc, o);
        ReleaseDC(u->h, dc);
        th = tm.tmHeight + 2;
        SetWindowPos(u->h, NULL, x + SX(9), y + (h - th) / 2, w - SX(18), th,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    } else if (u->kind == K_MEMO) {
        SetWindowPos(u->h, NULL, x + SX(8), y + SX(6), w - SX(10), h - SX(12),
                     SWP_NOZORDER | SWP_NOACTIVATE);
    } else if (u->kind == K_COMBO || u->kind == K_COMBOEDIT) {
        SetWindowPos(u->h, NULL, x, y, w, SX(320),
                     SWP_NOZORDER | SWP_NOACTIVATE);
        SendMessageW(u->h, CB_SETITEMHEIGHT, (WPARAM)-1, h - SX(6));
        SendMessageW(u->h, CB_SETITEMHEIGHT, 0, SX(24));
    } else if (u->kind == K_PKLIST) { /* W172: Zeilenhoehe folgt der DPI */
        SetWindowPos(u->h, NULL, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        SendMessageW(u->h, LB_SETITEMHEIGHT, 0, SX(PK_ROW_H));
    } else {
        SetWindowPos(u->h, NULL, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

/* W291 ESC-REWORK: Feinregler (nur mit "Erweiterte Einstellungen zeigen"),
 * gleiche Einteilung wie die Feinregler unter "Erweitert" im ESC-Menue.
 * W296 PRESET-ORIG: Verlaufsbreite/Abdunklung (ID_EDGE_W/S) gehoeren wieder zur
 * Breitbild-Zeile und stehen sichtbar, sobald "Fade out" gewaehlt ist
 * (edge_fade_shown), mit "Erweitert" wie bisher immer. */
static int lay_adv_ctl(int id)
{
    return id == ID_GFX_SOFT || id == ID_REM_LABEL || id == ID_CPERSP || id == ID_CPANINI || id == ID_CWIDTH;
}

/* W296: Verlaufszeile der Karte Breitbild sichtbar? (Fade out = Index 2 = edge_mode 2) */
static int edge_fade_shown(void)
{
    HWND c = ctl(ID_EDGE);
    return g_show_adv || (c && ComboBox_GetCurSel(c) == 2);
}

/* W296: Lage der Vorschau auf der Karte Breitbild (Entwurfseinheiten ab CY0):
 * mit Erweitert unter Cockpit-Kamera/Panini/Breite, sonst unter der
 * Verlaufszeile (+ Regelzeile R11/R12) bzw. direkt unter der Rand-Auswahl. */
int disp_preview_y(void)
{
    return g_show_adv ? 308 : edge_fade_shown() ? 194 : 150;
}

/* W291: Kartenlage der Seite Grafik (rechte Spalte rueckt ohne Feinregler zusammen) */
typedef struct { int cm_y, cm_h, wl_y, wl_h, wl_rr, fx_y, fx_h; } lay_gfx_t;
static void lay_gfx(lay_gfx_t *l)
{
    l->cm_y = CY0;
    l->cm_h = g_show_adv ? 160 : 124;          /* Monitor-Effekt, Cockpit, [Zieletikett] */
    l->wl_y = l->cm_y + l->cm_h + 12;
    l->wl_rr = g_show_adv ? 152 : 116;          /* Begruendungszeile unter den Zeilen */
    l->wl_h = l->wl_rr + 24;                    /* Weltbeleuchtung, Schatten, [Weiche Schatten] */
    l->fx_y = l->wl_y + l->wl_h + 12;
    l->fx_h = 132;                              /* Effekte, Kantenglaettung, Begruendung */
}


static void ui_move_xy(int id, int x, int y)
{
    uictl_t *u = ui_find(id);
    RECT q;
    if (!u || (u->x == x && u->y == y)) {
        return;
    }
    u->x = x;
    u->y = y;
    ui_px_rect(u, &q);
    SetWindowPos(u->h, NULL, q.left, q.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* W465 LOC-ENHANCED: Preset-Optionsfelder ohne Luecken - nur die im aktiven Spiel sichtbaren (SB: Original,
 * Performance, Klare Sicht; LoC: Original, Enhanced, Vivid W478) und Benutzerdefiniert, Zeilenabstand 50 (Feld 48 hoch).
 * Zuruecksetzen steht rechts in der Zeile Benutzerdefiniert. */
#define GP_ROW_H 50
void gfx_preset_layout(void)
{
    int i, row = 0;
    for (i = 0; i <= LGP_CUSTOM; i++) {
        if (i < LGP_CUSTOM && !lcfg_gfx_visible(i)) {
            continue;
        }
        ui_move_xy(ID_GP0 + i, CX0 + 18, CY0 + 42 + row * GP_ROW_H);
        if (i == LGP_CUSTOM) {
            ui_move_xy(ID_GP_RESET, CX0 + COLW - 18 - 84, CY0 + 42 + row * GP_ROW_H + 4);
        }
        row++;
    }
}

/* W291: Steuerelemente der Grafik-Seite an die aktuelle Kartenlage setzen */
void lay_gfx_apply(void)
{
    lay_gfx_t l;
    lay_gfx(&l);
    gfx_preset_layout(); /* W465 */
    ui_move_xy(ID_MONFX, COL2 + 120, l.cm_y + 44);
    ui_move_xy(ID_CHIDE, COL2 + 120, l.cm_y + 80); /* W319: Auswahl statt Schalter */
    ui_move_xy(ID_REM_LABEL, COL2 + 18, l.cm_y + 116);
    ui_move_xy(ID_XEXP_HUD, COL2 + COLW - 18 - XI_W, l.cm_y + 12);
    ui_move_xy(ID_GFX_LIGHT, COL2 + 120, l.wl_y + 44);
    ui_move_xy(ID_GFX_SHADOW, COL2 + 120, l.wl_y + 80);
    ui_move_xy(ID_GFX_SOFT, COL2 + 120, l.wl_y + 116);
    ui_move_xy(ID_XEXP_WORLD, COL2 + COLW - 18 - XI_W, l.wl_y + 12);
    ui_move_xy(ID_GFX_FX, COL2 + 120, l.fx_y + 44);
    ui_move_xy(ID_AAMODE, COL2 + 120, l.fx_y + 80);
    ui_move_xy(ID_XEXP_IMAGE, COL2 + COLW - 18 - XI_W, l.fx_y + 12);
}

int ui_visible_on(const uictl_t *u, int page)
{
    if (!u) { /* W201: edition-specific controls need not exist. */
        return 0;
    }
    { /* W496 NET-TELEMETRY: Dialog "anonyme Netzwerkstatistik" offen = nur seine Knoepfe */
        int nv = nrq_visible(u->id);
        if (nv >= 0) {
            return nv;
        }
    }
    { /* W470 LAUNCHER-HUB: Spiel-Navigation nur im Spielbereich, Reiter je Bereich */
        int hv = hub_visible(u->id, page);
        if (hv >= 0) {
            return hv;
        }
    }
    { /* W397 LAUNCHER-STRUCT: Zahnrad, Launcher-Einstellungen, Erstabfrage, Multiplayer-Spruenge */
        int sv = lset_visible(u->id, page);
        if (sv >= 0) {
            return sv;
        }
    }
    { /* W278 ORIG-FLAG: Knoepfe am Hinweis (beide Bereiche) */
        int ov = orig_visible(u->id, page);
        if (ov >= 0) {
            return ov;
        }
    }
    { /* W276: Bereich Line of Contact (LoC-Seite statt SB-Spielen, ohne Packs/Erweitert) */
        int lv = loctab_visible(u, page);
        if (lv >= 0) {
            return lv;
        }
    }
    /* W173 LAUNCHER-P4: "Zuruecksetzen" nur bei Benutzerdefiniert. Die
     * Texturen-Auswahl bleibt immer sichtbar (ohne Pack: Hinweis R30). */
    if (u->id == ID_GP_RESET && !ui_checked(ID_GP0 + LGP_CUSTOM)) {
        return 0;
    }
    if (u->id >= ID_GP0 && u->id < ID_GP0 + LGP_CUSTOM && !lcfg_gfx_visible(u->id - ID_GP0)) {
        return 0; /* W465 LOC-ENHANCED: Preset im aktiven Spiel nicht waehlbar */
    }
    /* W172/P5: Texturstufe nur noch auf der Seite Packs (Texturdetail);
     * die alte Kombi bleibt unsichtbar als Traeger fuer hd_scale. */
    if (u->id == ID_REM_SCALE) {
        return 0;
    }
    /* W291 ESC-REWORK: Schalter nur auf Anzeige und Grafik; Feinregler nur mit "Erweitert" */
    if (u->id == ID_SHOWADV) {
        return page == P_DISPLAY || page == P_GFX;
    }
    if (!g_show_adv && lay_adv_ctl(u->id)) {
        return 0;
    }
    if ((u->id == ID_EDGE_W || u->id == ID_EDGE_S) && !edge_fade_shown()) { /* W296 PRESET-ORIG */
        return 0;
    }
    /* W172/P6: Spielen zeigt entweder die Einrichtungskarte (Disc-Abbild)
     * oder die Bereitschaft mit Sprunglinks. */
    if (u->id == ID_IMAGE || u->id == ID_IMAGE_B || u->id == ID_IMAGE_C ||
        u->id == ID_PLAY_FOLDER) {
        if (!play_setup_shown()) {
            return 0;
        }
    }
    if (u->id >= ID_SUM_GO0 && u->id < ID_SUM_GO0 + SUM_GO_N && play_setup_shown()) {
        return 0;
    }
    /* W172/P5: Liste + Detailknoepfe nur mit Packs, Pack Creator nur im
     * leeren Zustand und nur, wenn die Anwendung neben dem Launcher liegt. */
    if ((u->id == ID_PK_LIST || u->id == ID_PK_UP || u->id == ID_PK_DOWN ||
         u->id == ID_PK_CHECKALL || u->id == ID_PK_TOGGLE ||
         u->id == ID_PK_CHECK || u->id == ID_PK_DEL) && packs_count() == 0) {
        return 0;
    }
    if (u->id == ID_PK_CREATOR && (SB_EDITION_COMMUNITY || packs_count() != 0 || g_creator != 1)) {
        return 0;
    }
    /* W170 LAUNCHER-P1/B4+B7: Schalter ohne Wirkung sind keine Bedienung.
     * Die Elemente bleiben angelegt, damit Speichern/Laden/Voreinstellungen
     * unveraendert laufen (W171: auch keine Infozeile mehr, Konzept 0.8). */
    if (u->id == ID_ASPECT) {
        return 0;
    }
    if (!dg_ctl_visible(u->id)) { /* W279: Absturz-Karte (Knoepfe nur mit Karte, Update-Karte weicht) */
        return 0;
    }
    /* W250: Erstabfrage nur unbeantwortet, Hinweis nur bei neuerer Version,
     * Einstellung nur mit Release-Version (sonst Hinweistext). */
    if ((u->id == ID_UPD_YES || u->id == ID_UPD_NO) && !g_upd_ask) {
        return 0;
    }
    if (u->id == ID_UPD_OPEN && (g_upd_ask || g_upd_res.result != LUPD_R_NEWER)) {
        return 0;
    }
    if ((u->id == ID_UPD_AUTO || u->id == ID_UPD_NOW) && !g_upd_ver[0]) {
        return 0;
    }
    /* W171: Live-Ton liegt in der eingeklappten Diagnose (Erweitert) */
    if (u->id == ID_LIVE && !g_diag_open) {
        return 0;
    }
    /* W171: Rueckgaengig nur, solange ein automatisches Umschalten offen ist */
    if (u->id == ID_UNDO && !g_undo_on) {
        return 0;
    }
    if ((u->id == ID_LAYOUT || u->id == ID_LAYOUT_LBL) && g_lay.id == LLAY_0) { /* W281: Layout 0 bleibt wie heute */
        return 0;
    }
    return u->page == PG_ALL || u->page == page ||
           (u->page == PG_NOTPLAY && page != P_PLAY);
}

/* Tabstopps der Tastenuebersicht (Dialog-Einheiten aus der Schrift). */
void keys_tabstops(void)
{
    HWND e = ctl(ID_KEYS);
    TEXTMETRICW tm;
    HDC dc;
    HGDIOBJ o;
    int du;
    if (!e) {
        return;
    }
    dc = GetDC(e);
    o = SelectObject(dc, g_f[F_SMALL]);
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, o);
    ReleaseDC(e, dc);
    du = MulDiv(SX(100), 4, tm.tmAveCharWidth ? tm.tmAveCharWidth : 7); /* W227: 112 -> 100, zweite Spalte passt */
    SendMessageW(e, EM_SETTABSTOPS, 1, (LPARAM)&du);
}

void ui_relayout(void)
{
    int i;
    for (i = 0; i < g_ui_n; i++) {
        if (g_ui[i].parent == g_hwnd) {
            ui_place(&g_ui[i]);
        }
    }
    keys_tabstops();
    tips_place(); /* W172 */
}

/* ------------------------------------------------------------------ */
/* Bild zusammensetzen                                                 */
/* ------------------------------------------------------------------ */
void base_build(void)
{
    int W = SX(lay_tw()), H = SX(lay_th()), x, y, NW = SX(lay_nav()); /* W281 */
    if (!cv_make(&g_base, W, H)) {
        return;
    }
    GdiFlush();
    if (g_bg_on && bg_load() && bg_scaled(W, H)) {
        memcpy(g_base.px, g_bgs, (size_t)W * H * 4);
    } else {
        for (y = 0; y < H; y++) {
            double fy = (double)y / H;
            int r = 17 - (int)(6 * fy), g = 19 - (int)(6 * fy),
                b = 24 - (int)(7 * fy);
            uint32_t c = ((uint32_t)r << 16) | ((uint32_t)g << 8) |
                         (uint32_t)b;
            for (x = 0; x < W; x++) {
                g_base.px[(size_t)y * W + x] = c;
            }
        }
    }
    /* Titelleiste, Navigation, Fussleiste: halbtransparente Flaechen. */
    cv_fill(&g_base, 0, 0, W, SX(TITLE_H), RGB(4, 5, 7), 150, 0);
    cv_fill(&g_base, 0, SX(TITLE_H), NW, H - SX(TITLE_H),
            RGB(6, 7, 10), 165, 0);
    cv_fill(&g_base, NW - 1, SX(TITLE_H), 1, H - SX(TITLE_H),
            RGB(255, 255, 255), 16, 0);
    cv_fill(&g_base, 0, SX(TITLE_H) - 1, W, 1, RGB(255, 255, 255), 16, 0);
    cv_fill(&g_base, NW, H - SX(BOT_H), W - NW, SX(BOT_H),
            RGB(4, 5, 7), 120, 0);
    cv_fill(&g_base, NW, H - SX(BOT_H), W - NW, 1,
            RGB(255, 255, 255), 14, 0);
    /* W398 LAUNCHER-GAMESWITCH: Das Logo links entfaellt, die Spielnamen selbst sind der Umschalter (Tabs
     * ID_GAME_*). Darueber klein und gedimmt die Projektzeile. Plaketten (Edition/Beta) stehen rechts neben der
     * Statusanzeige und werden mit ihr in hdr_paint_status gezeichnet. */
    hdr_paint_sub(); /* W401: Projektzeile + Edition/Beta */
    /* Fuss der Navigation (W281: in a/b steht dort die Layout-Vorschau) */
    if (g_lay.id == LLAY_0) {
        cv_text(&g_base, g_f[F_TINY], LT("gui.foot_hint"),
                SX(20), H - SX(58), SX(NAV_W - 28), SX(44), C_FAINT,
                DT_LEFT | DT_WORDBREAK);
    }
}

void card(int x, int y, int w, int h, const char *title)
{
    L_fill(x, y, w, h, C_PANEL, 214, 10);
    L_ring(x, y, w, h, RGB(255, 255, 255), 20, 10);
    if (title && *title) {
        L_text(F_CARD, title, x + 18, y + 12, w - 36, 24, C_TEXT,
               DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    }
}

void label(int font, const char *t, int x, int y, int w, int h,
                  COLORREF c)
{
    L_text(font, t, x, y, w, h, c, DT_LEFT | DT_WORDBREAK);
}

void label1(int font, const char *t, int x, int y, int w, int h,
                   COLORREF c)
{
    L_text(font, t, x, y, w, h, c,
           DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
}

void dot(int x, int y, COLORREF c)
{
    L_fill(x, y, 10, 10, c, 255, 5);
}

/* Spiel-Box: 4:3, maximal in w x h (Pillar-/Letterbox). */
int pergame_box_169(void); /* W402: LoC mit Bildformat 16:9 -> Box 16:9 statt 4:3 */
void aspect_box_for(unsigned w, unsigned h, unsigned *bw, unsigned *bh)
{
    unsigned ax = 4, ay = 3;
    if (w == 0 || h == 0) {
        *bw = 0;
        *bh = 0;
        return;
    }
    if (pergame_box_169()) {
        ax = 16;
        ay = 9;
    }
    *bw = h * ax / ay;
    *bh = h;
    if (*bw > w) {
        *bw = w;
        *bh = w * ay / ax;
    }
}

static void paint_preview(int x, int y, int w, int h)
{
    L_fill(x, y, w, h, RGB(0, 0, 0), 235, 8);
    if (g_prev_w > 0 && g_prev_h > 0) {
        RECT pr; /* W281: Flaeche wie L_fill (Layout b abgebildet) */
        int ow, oh;
        long pw = (long)g_prev_w, ph = (long)g_prev_h;
        int ox, oy;
        unsigned bw, bh;
        if (lay_b()) {
            lay_paint_px(x, y, w, h, &pr);
        } else {
            pr.left = SX(x);
            pr.top = SX(y);
            pr.right = SX(x) + SX(w);
            pr.bottom = SX(y) + SX(h);
        }
        ow = (pr.right - pr.left) - SX(20);
        oh = (pr.bottom - pr.top) - SX(20);
        if ((long)ow * ph > (long)oh * pw) {
            ow = (int)((long)oh * pw / ph);
        } else {
            oh = (int)((long)ow * ph / pw);
        }
        ox = pr.left + ((pr.right - pr.left) - ow) / 2;
        oy = pr.top + ((pr.bottom - pr.top) - oh) / 2;
        cv_fill(&g_bb, ox, oy, ow, oh, RGB(10, 10, 10), 255, 0);
        aspect_box_for((unsigned)ow, (unsigned)oh, &bw, &bh);
        { /* W107: Hor+-Rand der Mission je nach Bildraender-Einstellung */
            int ix = ox + (ow - (int)bw) / 2, iy = oy + (oh - (int)bh) / 2, side;
            int pill = (int)bw < ow;
            int len = pill ? ix - ox : iy - oy;
            int fw = (int)((long)g_prev_ew * (long)(pill ? (int)bw : (int)bh) / 100);
            for (side = 0; side < 2 && len > 0 && g_prev_edge != 1; side++) {
                int d; /* harter Schnitt: Rand bleibt schwarz */
                for (d = 0; d < len; d++) {
                    double f = 1.0, u;
                    int r, g, b;
                    if (g_prev_edge == 2) {
                        u = fw > 0 ? ((double)d + 0.5) / (double)fw : 1.0;
                        if (u > 1.0) { u = 1.0; }
                        u = u * u * (3.0 - 2.0 * u);
                        f = 1.0 - (double)g_prev_es / 100.0 * u;
                    }
                    r = 10 + (int)((58 - 10) * f);
                    g = 10 + (int)((72 - 10) * f);
                    b = 10 + (int)((88 - 10) * f);
                    if (pill) {
                        cv_fill(&g_bb, side ? ix + (int)bw + d : ix - 1 - d, oy, 1, oh,
                                RGB(r, g, b), 255, 0);
                    } else {
                        cv_fill(&g_bb, ox, side ? iy + (int)bh + d : iy - 1 - d, ow, 1,
                                RGB(r, g, b), 255, 0);
                    }
                }
            }
        }
        cv_ring(&g_bb, ox, oy, ow, oh, RGB(255, 255, 255), 40, 0, 1);
        {
            int ix = ox + (ow - (int)bw) / 2, iy = oy + (oh - (int)bh) / 2;
            int sh = (int)bh < 70 || (int)bw < SX(200); /* W171: kurz, wenn der Text nicht passt */
            cv_fill(&g_bb, ix, iy, (int)bw, (int)bh, RGB(44, 92, 128), 255, 0);
            cv_text(&g_bb, g_f[sh ? F_TINY : F_SEMI], pergame_box_169() ? (sh ? "16:9" : LT("gui.preview_169")) /* W402 */
                                                         : sh ? "4:3" : LT("gui.preview_43"), ix, /* W148 */
                    iy, (int)bw, (int)bh, C_TEXT,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    } else {
        L_text(F_BODY, LT("gui.preview_nores"), x, y, w, h, C_DIM,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

/* W172/P6: Spiel, Grafik, Anzeige, Packs, Steuerung (Konzept 4.1) */
sumrow_t g_sum[SUM_N];
const int kSumPage[SUM_N] = { P_ADV, P_GFX, P_DISPLAY, P_PACKS, P_CTRL };
/* W159: VR-Hinweis unter dem VR-Knopf + Runtime da (update_vr_button). */
char g_vr_hint[256];
int g_vr_present;

/* W171: Begruendungszeile einer Regel (Muster A/B/D/E), nur wenn sie greift.
 * Rueckgabe = belegte Hoehe (0 = nichts gezeichnet). */
int paint_reason(int rule, int x, int y, int w, int h)
{
    const lr_res_t *r;
    COLORREF c;
    int tx = x;
    if (rule < LR_FIRST || rule > LR_LAST || !g_rr[rule].active) {
        return 0;
    }
    r = &g_rr[rule];
    c = r->level == LR_LVL_ERR    ? C_ERR
        : r->level == LR_LVL_WARN ? C_WARN
        : r->level == LR_LVL_LOCK ? C_ACCENT_HI
                                  : C_DIM;
    if (r->level == LR_LVL_LOCK && g_has_icons) { /* Muster D: Schloss */
        wchar_t ic = 0xE72E;
        RECT q; /* W281 */
        lay_px(x, y, 14, 16, 0, 0, &q);
        cv_textw(&g_bb, g_f[F_ICONSM], &ic, 1, q.left, q.top, SX(14), SX(16), c,
                 DT_LEFT | DT_TOP | DT_SINGLELINE);
        tx += 16;
    }
    L_text(F_TINY, r->text, tx, y, w - (tx - x), h, c,
           h > 18 ? (DT_LEFT | DT_WORDBREAK)
                  : (DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS));
    return h;
}

/* Erster greifender Grund einer Liste (Startsperre: R4/R3 vor R5). */
int first_rule(const int *rules, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (g_rr[rules[i]].active) {
            return rules[i];
        }
    }
    return 0;
}

int launch_blocked(void)
{
    return g_rr[3].disable || g_rr[4].disable;
}

/* W220 PLAY-LAYOUT: Textzeilen der Einrichtungskarte (eine Quelle fuer
 * Zeichnen und Selbsttest 62). text == NULL: Zeile leer. */

char g_img_path[1024]; /* W220: Pfad, zu dem g_img_msg gehoert */
/* W220: Pruef-Fixture (Selbsttest/Shots ohne echtes Abbild): gilt nur fuer
 * genau diesen Pfad, sonst echte Pruefung. */
char g_imgfix_path[1024];
int g_imgfix_rc;
char g_imgfix_msg[600];
int g_imgfix_ed; /* W231: Fassung der Fixture (LCFG_ED_*) */

int img_check(const char *path, char *msg, size_t cap)
{
    if (g_imgfix_path[0] && path && strcmp(path, g_imgfix_path) == 0) {
        strcpy_s(msg, cap, g_imgfix_msg);
        return g_imgfix_rc;
    }
    return lcfg_image_check(path, msg, cap);
}

void imgfix_set(const char *path, int rc, const char *msg)
{
    strcpy_s(g_imgfix_path, sizeof g_imgfix_path, path ? path : "");
    g_imgfix_rc = rc;
    strcpy_s(g_imgfix_msg, sizeof g_imgfix_msg, msg ? msg : "");
}

void play_setup_rows(psrow_t *rw)
{
    char cur[1024];
    memset(rw, 0, sizeof(psrow_t) * PSR_N);
    rw[PSR_TXT].text = LT("gui.play_setup_txt");
    rw[PSR_TXT].font = F_SMALL;
    rw[PSR_TXT].x = CX0 + 18; rw[PSR_TXT].y = CY0 + 250;
    rw[PSR_TXT].w = CW - 36; rw[PSR_TXT].h = 20;
    rw[PSR_TXT].single = 1; rw[PSR_TXT].col = C_DIM;
    /* Ergebniszeile unter dem Pfadfeld: Pruefergebnis (nur fuer den
     * aktuellen Pfad), sonst Erststart-Schritte (W217), sonst Fehler (R3) */
    rw[PSR_MSG].font = F_SMALL;
    rw[PSR_MSG].x = CX0 + 18; rw[PSR_MSG].y = CY0 + 318;
    rw[PSR_MSG].w = CW - 36; rw[PSR_MSG].h = 36;
    cur[0] = '\0';
    if (ctl(ID_IMAGE)) {
        ui_gettext(ctl(ID_IMAGE), cur, sizeof cur);
    }
    if (g_img_msg[0] && cur[0] && strcmp(cur, g_img_path) == 0) {
        rw[PSR_MSG].text = g_img_msg;
        rw[PSR_MSG].col = g_img_ok ? C_OK : C_ERR;
    } else if (!cur[0] && !g_cur.game_image[0] && !g_cur.media_root[0] &&
               !g_cur.xbe_path[0]) {
        rw[PSR_MSG].text = LT("gui.play_firstrun"); /* W217 FIRST-RUN */
        rw[PSR_MSG].col = C_TEXT;
    } else if (g_rr[3].active) {
        rw[PSR_MSG].text = g_rr[3].text;
        rw[PSR_MSG].reason = 1;
    } else if (!cur[0] && g_cur.media_root[0]) {
        rw[PSR_MSG].text = LT("gui.play_folder_used");
        rw[PSR_MSG].col = C_DIM;
    }
#if SB_EDITION_COMMUNITY
    rw[PSR_DISC].text = LT("gui.play_own_disc");
#endif
    rw[PSR_DISC].font = F_TINY;
    rw[PSR_DISC].x = CX0 + 18; rw[PSR_DISC].y = CY0 + 360;
    rw[PSR_DISC].w = CW - 36; rw[PSR_DISC].h = 30;
    rw[PSR_DISC].col = C_FAINT;
    rw[PSR_FOLDERQ].text = LT("gui.play_folder_q");
    rw[PSR_FOLDERQ].font = F_SMALL;
    rw[PSR_FOLDERQ].x = CX0 + 18; rw[PSR_FOLDERQ].y = CY0 + 400;
    rw[PSR_FOLDERQ].w = CW - 36 - 240; rw[PSR_FOLDERQ].h = 24; /* W227: Knopf 230 */
    rw[PSR_FOLDERQ].single = 1; rw[PSR_FOLDERQ].col = C_DIM;
}

/* W250 UPDATE-CHECK: Karte rechts neben den Startknoepfen (frei auf jeder
 * Spielen-Ansicht): Erstabfrage (bis beantwortet) oder Hinweis auf eine
 * neuere Version. Knoepfe ID_UPD_YES/NO/OPEN (build_controls). */
void upd_tag_disp(const char *tag, char *out, size_t cap)
{
    strcpy_s(out, cap, (tag[0] == 'v' || tag[0] == 'V') ? tag + 1 : tag);
}

static void upd_paint_play(void)
{
    char t[256], v[64];
    if (dg_paint_play()) { /* W279: "The last session crashed" hat Vorrang */
        return;
    }
    if (g_upd_ask) { /* W397: Erstabfrage "Online & Updates" (drei Stufen) statt Update-Frage */
        netq_paint(UPD_X, UPD_Y, UPD_W);
    } else if (g_upd_res.result == LUPD_R_NEWER) {
        card(UPD_X, UPD_Y, UPD_W, 96, LT("gui.upd_new_title"));
        upd_tag_disp(g_upd_res.rel.tag, v, sizeof v);
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.upd_new"), v);
        label1(F_SMALL, t, UPD_X + 18, UPD_Y + 34, UPD_W - 36, 20, C_ACCENT_HI);
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.upd_cur"), g_upd_ver);
        label1(F_TINY, t, UPD_X + 18, UPD_Y + 64, UPD_W - 36 - 168, 24, C_DIM);
    }
}

/* W281 LAUNCHER-SIZE: Spielen in Layout b - Start-Bereich (Knoepfe links,
 * Kurzstand rechts) und sechs Statuskarten (Spiel, Grafik, Anzeige, Packs,
 * Steuerung, Primaerfeuer); Einrichtungskarte wie heute. Rechtecke im
 * Entwurfsraster aus launcher_layout.c (llay_b_*). */
static void paint_play_b(void)
{
    int i, r[4], sa[4], hn[4];
    static const int kBig[] = { 4, 3 }, kVr[] = { 5 };
    SetTextCharacterExtra(g_bb.dc, SX(3));
    L_text(F_HERO, "STEEL BATTALION", CX0 - 2, CY0 - 2, CW, 50, C_TEXT,
           DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    SetTextCharacterExtra(g_bb.dc, 0);
    label1(F_BODY, LT("gui.start_sub"), CX0, CY0 + 50, CW, 22, C_DIM);
    llay_b_start_area(sa);
    card(sa[0], sa[1], sa[2], sa[3], NULL);
    llay_b_start_hint(hn);
    {
        int rr = first_rule(kBig, 2);
        if (!rr) {
            rr = first_rule(kVr, 1);
        }
        if (rr) {
            paint_reason(rr, hn[0], hn[1], hn[2], hn[3]);
        } else {
            label1(F_TINY, g_vr_hint, hn[0], hn[1], hn[2], hn[3], C_DIM);
        }
    }
    if (!g_upd_ask && g_upd_res.result != LUPD_R_NEWER) { /* Platz der Update-Karte */
        int x = sa[0] + 372, w = sa[0] + sa[2] - 18 - x;
        dot(x, sa[1] + 27, g_sum[0].col);
        label1(F_CARD, LT(launch_blocked() ? "gui.play_setup" : "gui.sum_title"), x + 18,
               sa[1] + 18, w - 18, 26, C_TEXT);
        label(F_SMALL, g_sum[0].value, x, sa[1] + 52, w, 40, C_DIM);
        label(F_TINY, LT("gui.foot_hint"), x, sa[1] + 98, w, 34, C_FAINT);
    }
    upd_paint_play();
    if (play_setup_shown()) { /* wie paint_play (Einrichtungskarte) */
        psrow_t rw[PSR_N];
        int k;
        play_setup_rows(rw);
        card(CX0, CY0 + 216, CW, 232, LT("gui.play_setup"));
        for (k = 0; k < PSR_N; k++) {
            if (!rw[k].text) {
                continue;
            }
            if (rw[k].reason) {
                paint_reason(3, rw[k].x, rw[k].y, rw[k].w, rw[k].h);
            } else if (rw[k].single) {
                label1(rw[k].font, rw[k].text, rw[k].x, rw[k].y, rw[k].w, rw[k].h,
                       rw[k].col);
            } else {
                label(rw[k].font, rw[k].text, rw[k].x, rw[k].y, rw[k].w, rw[k].h,
                      rw[k].col);
            }
        }
        return;
    }
    for (i = 0; i < SUM_N && i < LLAY_BCARD_N - 1; i++) {
        llay_b_card(i, r);
        card(r[0], r[1], r[2], r[3], NULL);
        dot(r[0] + 18, r[1] + 20, g_sum[i].col);
        label1(F_SEMI, g_sum[i].label, r[0] + 36, r[1] + 13, r[2] - 54, 24, C_TEXT);
        label(F_SMALL, g_sum[i].value, r[0] + 18, r[1] + 44, r[2] - 36, 44, C_DIM);
    }
    llay_b_card(LLAY_BCARD_N - 1, r);
    if (!g_rr[26].active) { /* Primaerfeuer (nur Tastatur-Standardbelegung, R26) */
        L_fill(r[0], r[1], r[2], r[3], C_ACCENT, 46, 10);
        L_ring(r[0], r[1], r[2], r[3], C_ACCENT, 140, 10);
        label1(F_SEMI, LT("gui.primfeuer"), r[0] + 18, r[1] + 13, r[2] - 36, 24, C_ACCENT_HI);
        label(F_BODY, LT("gui.primfeuer_hint"), r[0] + 18, r[1] + 44, r[2] - 36, 72, C_TEXT);
    } else {
        card(r[0], r[1], r[2], r[3], NULL);
        label(F_SMALL, g_sum[4].value, r[0] + 18, r[1] + 44, r[2] - 36, 44, C_DIM);
    }
}

static void paint_play(void)
{
    int i, y;
    static const int kBig[] = { 4, 3 }, kVr[] = { 5 };
    if (lay_b()) { /* W281 */
        paint_play_b();
        return;
    }
    SetTextCharacterExtra(g_bb.dc, SX(3));
    L_text(F_HERO, "STEEL BATTALION", CX0 - 2, CY0 - 2, CW, 50, C_TEXT,
           DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    SetTextCharacterExtra(g_bb.dc, 0);
    label1(F_BODY, LT("gui.start_sub"), CX0, CY0 + 50, CW, 22, C_DIM);
    /* Nutzer 7.11.: keine Schnellhilfe neben dem Startknopf (schlichter);
     * Tastenhilfe steht auf der Seite Controls. */
    /* Grund unter den Startknoepfen (Muster E/A), sonst VR-Hinweis */
    {
        int r = first_rule(kBig, 2);
        if (!r) {
            r = first_rule(kVr, 1);
        }
        if (r) {
            paint_reason(r, CX0, CY0 + 190, 330, 18);
        } else {
            label1(F_TINY, g_vr_hint, CX0, CY0 + 190, 330, 18, C_DIM);
        }
    }
    if (play_setup_shown()) {
        /* W172/P6: Einrichtungskarte (Spieldaten fehlen/ungueltig oder
         * "Aendern" an der Spiel-Zeile): Disc-Abbild waehlen + pruefen,
         * Ordner-Modus nur als Verweis nach Erweitert (Konzept 4.1). */
        psrow_t rw[PSR_N];
        int k;
        play_setup_rows(rw);
        card(CX0, CY0 + 216, CW, 232, LT("gui.play_setup"));
        for (k = 0; k < PSR_N; k++) {
            if (!rw[k].text) {
                continue;
            }
            if (rw[k].reason) { /* konkreter Fehler aus dem Regelwerk (R3) */
                paint_reason(3, rw[k].x, rw[k].y, rw[k].w, rw[k].h);
            } else if (rw[k].single) {
                label1(rw[k].font, rw[k].text, rw[k].x, rw[k].y, rw[k].w, rw[k].h,
                       rw[k].col);
            } else {
                label(rw[k].font, rw[k].text, rw[k].x, rw[k].y, rw[k].w, rw[k].h,
                      rw[k].col);
            }
        }
    } else {
        /* W172/P6: Bereitschaft mit Sprunglinks ("Aendern" = Seite der
         * Ursache; Punkt gruen/gelb/rot). */
        card(CX0, CY0 + 216, CW, 232, LT("gui.sum_title"));
        for (i = 0, y = CY0 + 256; i < SUM_N; i++, y += 36) {
            dot(CX0 + 20, y + 6, g_sum[i].col);
            label1(F_SMALL, g_sum[i].label, CX0 + 40, y, 100, 22, C_DIM);
            label1(F_SMALL, g_sum[i].value, CX0 + 142, y, CW - 142 - 18 - 112, 22,
                   C_TEXT);
        }
    }
    upd_paint_play(); /* W250 */
    if (g_orig_mask && !play_setup_shown()) { /* W278: Hinweis statt Primaerfeuer (neue Installation = Original, ohne Hinweis) */
        orig_paint_play(0);
    } else
    /* Primaerfeuer deutlich (nur Tastatur-Standardbelegung, R26) */
    if (!g_rr[26].active) {
        L_fill(CX0, CY0 + 456, CW, 40, C_ACCENT, 46, 10);
        L_ring(CX0, CY0 + 456, CW, 40, C_ACCENT, 140, 10);
        label1(F_SEMI, LT("gui.primfeuer"), CX0 + 18, CY0 + 465, 130, 22,
               C_ACCENT_HI);
        label1(F_BODY, LT("gui.primfeuer_hint"), CX0 + 140, CY0 + 465, CW - 156,
               22, C_TEXT);
    }
}

void combo_add(HWND cb, const char *u8);
/* W227: Eintraege mit Beschriftung ("Res. 80 %"), auch beim Sprachwechsel */
void vrres_fill(HWND c)
{
    int i, sel = ComboBox_GetCurSel(c);
    ComboBox_ResetContent(c);
    for (i = 0; i < KVRRES_N; i++) {
        char t[32];
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.vr_res_fmt"), kVrRes[i]);
        combo_add(c, t);
    }
    if (sel >= 0) {
        ComboBox_SetCurSel(c, sel);
    }
}

int vrres_index(int pct)
{
    int k, best = 0;
    for (k = 0; k < KVRRES_N; k++) {
        if (abs(kVrRes[k] - pct) < abs(kVrRes[best] - pct)) { best = k; }
    }
    return best;
}

/* Nutzer 14.11.: auf der Anzeige-Seite nur Warnungen/Fehler/Sperren zeigen,
 * reine Erklaerzeilen (Stufe Info) entfallen. */
static int paint_reason_alert(int rule, int x, int y, int w, int h)
{
    if (rule < LR_FIRST || rule > LR_LAST || !g_rr[rule].active || g_rr[rule].level == LR_LVL_INFO) {
        return 0;
    }
    return paint_reason(rule, x, y, w, h);
}

/* W171: Seite Anzeige (Bildschirm, Bildrate, VR-Vorschau, Bildformat,
 * Render-Skalierung). */
static void paint_display(void)
{
    /* Nutzer 14.11.: Erklaerzeilen weg (nur Warnungen/Sperren). W291 ESC-REWORK:
     * links eine Karte "Anzeige" (Bildschirm, Bildrate, Render-Skalierung) und
     * VR, rechts "Breitbild"; Verlauf und Cockpit-Ansicht nur mit "Erweitert"
     * (wie das ESC-Menue). */
    int adv = g_show_adv, py = disp_preview_y(); /* W296: Verlauf bei "Fade out" auch ohne Erweitert */
    card(CX0, CY0, COLW, 300, LT("gui.grp_display"));
    label1(F_SMALL, LT("gui.gfx_winmode"), CX0 + 18, CY0 + 44, 100, 30, C_DIM);
    label1(F_SMALL, LT("gui.disp_hz"), CX0 + 18, CY0 + 80, 100, 30, C_DIM);
    label1(F_SMALL, LT("gui.gfx_res"), CX0 + 18, CY0 + 116, 100, 30, C_DIM);
    label1(F_SMALL, LT("gui.gfx_wh"), CX0 + 18, CY0 + 152, 100, 30, C_DIM);
    label1(F_BODY, U8_TIMES, CX0 + 215, CY0 + 152, 14, 30, C_DIM);
    label1(F_SMALL, LT("gui.gfx_ipfps"), CX0 + 18, CY0 + 188, 100, 30, C_DIM);
    label1(F_SMALL, LT("gui.rs_label"), CX0 + 18, CY0 + 224, 100, 30, C_DIM);
    if (!paint_reason_alert(10, CX0 + 18, CY0 + 258, COLW - 36, 30)) {
        paint_reason_alert(9, CX0 + 18, CY0 + 258, COLW - 36, 30);
    }

    card(CX0, CY0 + 312, COLW, 118, LT("gui.disp_vr"));
    label1(F_SMALL, LT("gui.gfx_vr"), CX0 + 18, CY0 + 356, 100, 30, C_DIM);
    label1(F_SMALL, LT("gui.vr_perf"), CX0 + 18, CY0 + 390, 100, 30, C_DIM); /* W183 */

    card(COL2, CY0, COLW, 488, LT("gui.disp_wide"));
    /* W278/W287: Breitbild/Cockpit-Ansicht experimentell = Symbol ID_XEXP_DISP */
    /* R13: festes Bildformat (S3.10) als Info, kein Schalter */
    label(F_SMALL, g_game == LGAME_LOC ? LT(pergame_box_169() ? "gui.loc_disp_wide_169" : "gui.loc_disp_wide_43")
                                       : g_rr[13].text, COL2 + 18, CY0 + 42, COLW - 36, 52, C_DIM); /* W402 */
    label1(F_SMALL, LT("gui.gfx_edge"), COL2 + 18, CY0 + 100, 100, 30, C_DIM);
    if (adv || edge_fade_shown()) { /* W296 PRESET-ORIG: Verlaufszeile gehoert zu "Fade out" */
        label1(F_SMALL, LT("gui.gfx_edgefade"), COL2 + 18, CY0 + 136, 100, 30, C_DIM);
        if (!paint_reason(11, COL2 + 120, CY0 + 168, COLW - 138, 16)) {
            paint_reason(12, COL2 + 120, CY0 + 168, COLW - 138, 16);
        }
    }
    if (adv) {
        /* W191 UW-PERSP: Cockpit-Perspektive; R34 nur als Sperre (VR) */
        label1(F_SMALL, LT("gui.cpersp"), COL2 + 18, CY0 + 186, 100, 30, C_DIM);
        label1(F_SMALL, LT("gui.cpanini"), COL2 + 18, CY0 + 220, 100, 30, C_DIM); /* W227 */
        label1(F_SMALL, LT("gui.cwidth"), COL2 + 18, CY0 + 254, 100, 30, C_DIM); /* W235 */
        paint_reason_alert(34, COL2 + 18, CY0 + 286, COLW - 36, 16);
    }
    paint_preview(COL2 + 18, CY0 + py, COLW - 36, 104);
    label(F_SMALL, g_aspect_info, COL2 + 18, CY0 + py + 110, COLW - 36, 64, C_TEXT);
}

/* W171: Seite Grafik (Voreinstellungen, Bild, Remaster, HUD). */
/* W173: Abweichungs-Punkt (Konzept 4.3 "*") vor der Beschriftung. */
static void gfx_label(const char *key, int x, int y, int mark)
{
    if (mark) {
        L_fill(x + 6, y + 12, 6, 6, C_ACCENT_HI, 255, 3);
    }
    label1(F_SMALL, LT(key), x + 18, y, 100, 30, C_DIM);
}

static void paint_gfx(void)
{
    lgp_mask_t lock = gfx_lock_now(), dm = 0;
    int p = lcfg_gfx_preset(&g_gfx, lock);
    int fxbits = 0, k;
    lay_gfx_t l;
    lay_gfx(&l);
    if (p == LGP_CUSTOM) {
        lcfg_gfx_diff(&g_gfx, lcfg_gfx_base(&g_gfx, lock), lock, &dm);
    }
    for (k = 0; k < LFX_N; k++) {
        if (dm & ((lgp_mask_t)1 << (LGK_FX0 + k))) {
            fxbits = 1;
        }
    }
    /* W291 ESC-REWORK: Karten mit den Gruppennamen des ESC-Menues in dessen
     * Reihenfolge: links Voreinstellung + Texturen, rechts Cockpit & Monitor,
     * Welt & Licht, Effekte. Feinregler nur mit "Erweiterte Einstellungen". */
    card(CX0, CY0, COLW, 304, LT("gui.gfx_preset_card"));
    if (g_rr[20].active) { /* Muster D an der Voreinstellung */
        L_text(F_TINY, g_rr[20].text2, CX0 + 18, CY0 + 284, COLW - 36, 18, C_ACCENT_HI,
               DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    card(CX0, LAY_TEX_Y, COLW, 124, LT("gui.gfx_tex"));
    gfx_label("gui.gfx_tex", CX0, LAY_TEX_Y + 44, (dm & ((lgp_mask_t)1 << LGK_HD)) != 0);
    { /* Texturen-Zeile (Konzept 4.3): Quelle und Detail, Sprung zu Packs */
        char t[240];
        if (!lcfg_gfx_value(&g_gfx, LGK_HD)) {
            strncpy_s(t, sizeof t, LT("gui.gfx_tex_orig"), _TRUNCATE);
        } else if (g_rr[30].active) {
            strcpy_s(t, sizeof t, g_rr[30].text);
        } else {
            _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.gfx_tex_on"), packs_level_word(),
                        g_tex_src);
        }
        label(F_TINY, t, CX0 + 18, LAY_TEX_Y + 80, COLW - 36 - 104, 30, C_DIM);
    }
    label(F_SMALL, LT("gui.gfx_esc_hint"), CX0 + 4, LAY_TEX_Y + 130, COLW - 8, 20, C_FAINT);

    card(COL2, l.cm_y, COLW, l.cm_h, LT("gui.grp_cockpit"));
    /* W287: Symbol ID_XEXP_HUD */
    gfx_label("gui.gfx_monitor", COL2, l.cm_y + 44, (dm & ((lgp_mask_t)1 << LGK_MFX)) != 0); /* W252 */
    gfx_label("gui.gfx_cview", COL2, l.cm_y + 80, ctl(ID_CHIDE) && ComboBox_GetCurSel(ctl(ID_CHIDE)) > 0); /* W319 */

    card(COL2, l.wl_y, COLW, l.wl_h, LT("gui.grp_world"));
    /* W287: Symbol ID_XEXP_WORLD */
    gfx_label("gui.gfx_light", COL2, l.wl_y + 44, (dm & ((lgp_mask_t)1 << LSHX_SHADING)) != 0);
    gfx_label("gui.gfx_shadow", COL2, l.wl_y + 80, (dm & ((lgp_mask_t)1 << LSHX_SHADOW)) != 0);
    if (g_show_adv) {
        gfx_label("gui.gfx_soft", COL2, l.wl_y + 116, (dm & ((lgp_mask_t)1 << LSHX_SOFT)) != 0);
    }
    if (!paint_reason(15, COL2 + 18, l.wl_y + l.wl_rr, COLW - 36, 16) && g_show_adv) {
        paint_reason(16, COL2 + 18, l.wl_y + l.wl_rr, COLW - 36, 16);
    }

    card(COL2, l.fx_y, COLW, l.fx_h, LT("gui.gfx_fx"));
    /* W287: Symbol ID_XEXP_IMAGE */
    gfx_label("gui.gfx_fx", COL2, l.fx_y + 44, fxbits);
    gfx_label("gui.gfx_aa", COL2, l.fx_y + 80, (dm & ((lgp_mask_t)1 << LSHX_AA)) != 0);
    paint_reason(20, COL2 + 18, l.fx_y + 112, COLW - 36, 16);
}

/* W195 PHOTO-MODE: Standard-Fotoordner (wie der Client: <Nutzerdaten>\Screenshots, legacy <Client>\out\Screenshots) */
void extras_default_dir(char *out, size_t cap)
{
    if (sb_userdata_path_peek(SB_UD_SCREENSHOTS, out, cap) == 1) {
        return;
    }
    _snprintf_s(out, cap, _TRUNCATE, "%s\\out\\Screenshots", g_client_disp[0] ? g_client_disp : (g_client_dir[0] ? g_client_dir : "."));
}

/* W195 R33: Fotoordner nutzbar? Leer = Standard (ok); sonst absolut (X:\\ bzw. \\\\server) und vorhanden
 * oder der uebergeordnete Ordner vorhanden (der Client legt die letzte Ebene an). */
int extras_dir_ok(const char *d)
{
    char p[600];
    size_t n;
    if (!d || !d[0]) {
        return 1;
    }
    if (!((((d[0] | 32) >= 'a' && (d[0] | 32) <= 'z') && d[1] == ':' && (d[2] == '\\' || d[2] == '/')) ||
          (d[0] == '\\' && d[1] == '\\'))) {
        return 0;
    }
    if (is_dir_u8(d)) {
        return 1;
    }
    strcpy_s(p, sizeof p, d);
    n = strlen(p);
    while (n > 0 && (p[n - 1] == '\\' || p[n - 1] == '/')) {
        p[--n] = 0;
    }
    while (n > 0 && p[n - 1] != '\\' && p[n - 1] != '/') {
        p[--n] = 0;
    }
    return n > 0 ? is_dir_u8(p) : 0;
}

/* W195: wirksamer Fotoordner (Feld oder Standard) */
void extras_dir(char *out, size_t cap)
{
    char cur[600];
    ui_gettext(ctl(ID_PH_DIR), cur, sizeof cur);
    trim_u8(cur);
    if (cur[0]) {
        strcpy_s(out, cap, cur);
    } else {
        extras_default_dir(out, cap);
    }
}

static void paint_audio(void)
{
    /* W290: Ton und Spieltempo nebeneinander, darunter die groessere Karte "Fotos & Screenshots" */
    /* W303 AUDIO-VOLUME: Karte "Ton & Lautstaerke" (158 hoch): Schalter + Ausgabe, darunter 2x2 Regler */
    card(CX0, CY0, COLW, 140 + EXY, LT("gui.audio_card"));
    /* W356: rechts neben "Ton an" der Schalter "Kopfhoerer" (statt des Hinweises zum Ausgabegeraet) */
    {
        int k;
        for (k = 0; k < 4; k++) { /* Gesamt | Musik / Effekte | Sprache/Funk */
            label1(F_SMALL, LT(kVolKey[k]), CX0 + 18 + (k & 1) * 152, CY0 + 82 + (k >> 1) * 36, k & 1 ? 74 : 66, 30,
                   C_DIM);
        }
    }
    /* W234 PAL50: Karte "Spieltempo" (Schalter + kurze Erklaerung) */
    card(COL2, CY0, COLW, 140 + EXY, LT("gui.pal50_card"));
    label(F_TINY, LT(g_game == LGAME_LOC ? "gui.loc_lock_tick" : "gui.pal50_txt"), COL2 + 18, CY0 + 86, COLW - 36, 48, /* W402 */
          g_game == LGAME_LOC ? C_WARN : C_DIM);
    /* W195 PHOTO-MODE / W290 QUICK-SHOT: Karte "Fotos & Screenshots" (eine Konfiguration fuer Fotomodus und
     * Schnell-Screenshot: Ordner, Ansicht, Groesse, Format, Bildformat der Aussenwelt; Replays & Video-Export) */
    card(CX0, CY0 + 152 + EXY, CW, 344 - EXY, LT("gui.extras_card"));
    label1(F_TINY, LT("gui.extras_dir"), CX0 + 18, CY0 + 190 + EXY, CW - 36, 16, C_DIM);
    label1(F_SMALL, LT("gui.extras_view"), CX0 + 18, CY0 + 250 + EXY, 110, 30, C_DIM);
    label1(F_SMALL, LT("gui.extras_res"), CX0 + 372, CY0 + 250 + EXY, 66, 30, C_DIM);
    label1(F_SMALL, LT("gui.extras_fmt"), CX0 + 18, CY0 + PHY_FMT + EXY, 110, 30, C_DIM);
    label1(F_SMALL, LT("gui.extras_asp"), CX0 + 250, CY0 + PHY_FMT + EXY, 188, 30, C_DIM);
    /* W445: Cinematic-Reichweite eigene VT-Teile (Liste) mit kurzem Hinweis rechts daneben */
    label1(F_SMALL, LT("gui.extras_cself"), CX0 + 18, CY0 + PHY_CS + EXY, 226, 30, C_DIM);
    label(F_TINY, LT("gui.extras_cself_txt"), CX0 + 362, CY0 + PHY_CS + EXY - 2, CW - 18 - 362, 34, C_DIM);
    label1(F_TINY, LT("gui.extras_rp"), CX0 + 18, CY0 + PHY_RP + EXY, CW - 36, 16, C_DIM);
    {
        char d[600], t[900];
        extras_default_dir(d, sizeof d);
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.extras_txt"), d);
        if (!paint_reason(33, CX0 + 18, CY0 + PHY_TXT + EXY, CW - 36, PHY_TXTH)) { /* W195 R33: Ordner nicht nutzbar */
            label(F_TINY, t, CX0 + 18, CY0 + PHY_TXT + EXY, CW - 36, PHY_TXTH, C_DIM);
        }
    }
}

static void paint_ctrl(void)
{
    card(CX0, CY0, COLW, 248, LT("gui.ctl_device"));
    label(F_SMALL, g_sbc_txt, CX0 + 18, CY0 + 80, COLW - 36, 34, C_TEXT);
    paint_reason(25, CX0 + 18, CY0 + 116, COLW - 36, 30);
    paint_reason(23, CX0 + 18, CY0 + 180, COLW - 36, 16);

    card(CX0, CY0 + 260, COLW, 120, LT("gui.ctl_profile"));

    card(CX0, CY0 + 392, COLW, 104, LT("gui.ctl_pad"));
    /* W217: Zustand und Detail untereinander (lagen uebereinander) */
    label(F_BODY, g_pad_txt, CX0 + 18, CY0 + 430, COLW - 36, 22, C_TEXT);
    label(F_SMALL, g_padlive_txt, CX0 + 18, CY0 + 454, COLW - 36, 36, C_DIM);

    card(COL2, CY0, COLW, 496, LT("gui.ctl_keymap"));
    L_fill(COL2 + 18, CY0 + 46, COLW - 36, 50, C_ACCENT, 46, 8);
    L_ring(COL2 + 18, CY0 + 46, COLW - 36, 50, C_ACCENT, 150, 8);
    label1(F_TINY, LT("gui.ctl_prim"), COL2 + 32, CY0 + 50, COLW - 60, 18,
           C_ACCENT_HI);
    label1(F_SEMI, LT("gui.ctl_prim_keys"), COL2 + 32, CY0 + 68, COLW - 60, 24,
           C_TEXT);
    label1(F_SMALL, LT("gui.ctl_mouse"), COL2 + 18, CY0 + 108, 56, 30, C_DIM);
    label1(F_SMALL, LT("gui.ctl_sens"), COL2 + COLW - 18 - 54 - 48, CY0 + 108,
           48, 30, C_DIM);
    if (!paint_reason(24, COL2 + 18, CY0 + 142, COLW - 36, 16) && g_game == LGAME_LOC) { /* W440: Hilfen nur Arcade */
        label1(F_TINY, LT("gui.loc_arcade_only"), COL2 + 18, CY0 + 142, COLW - 36, 16, C_DIM);
    }
    /* W271 STICK-RELATIVE: Stick-Zielen wie die Maus-Zeile */
    label1(F_SMALL, LT("gui.ctl_stick"), COL2 + 18, CY0 + 262, 56, 30, C_DIM); /* W424: 228 -> 262 */
    label1(F_SMALL, LT("gui.ctl_sens"), COL2 + COLW - 18 - 54 - 48, CY0 + 262,
           48, 30, C_DIM);
}

/* Erweitert: Diagnose-Zeile (Bezeichnung klein, Wert darunter). */
static void diag_row(const char *lbl, const char *val, COLORREF c, int y)
{
    label1(F_TINY, lbl, COL2 + 18, y, COLW - 36, 14, C_DIM);
    label1(F_SMALL, val, COL2 + 18, y + 14, COLW - 36, 18, c); /* W250: Zeilen 32 statt 36 */
}


/* W172/P7: Datenort im Klartext (gleiche Quelle wie --userdata:
 * sb_userdata_mode + sb_userdata_path_peek). mode = Sprachschluessel. */
void adv_data_loc(char *path, size_t cap, const char **mode)
{
    char p[1024];
    sb_ud_mode_t m = sb_userdata_mode();
    *mode = m == SB_UD_PROFILE    ? "gui.ud_profile"
            : m == SB_UD_PORTABLE ? "gui.ud_portable"
                                  : "gui.ud_legacy";
    if (sb_userdata_path_peek(SB_UD_ROOT, p, sizeof p) == 1) {
        u8_from_acp(p, path, cap);
    } else {
        strcpy_s(path, cap, g_client_disp);
    }
}

/* W172/P7: Ordner der Spielstaende wie der Client (hcfg.c: "HDD" im
 * Profil-/Portabel-Modus = <Datenordner>\hdd, sonst relativ zum Client). */
void adv_hdd_path(char *out, size_t cap)
{
    /* W231 US-LAUNCHER: US-Spielstaende getrennt in <hdd>\us (gleiche
     * Title-ID 43430002); Anzeige, Oeffnen, Import/Export und Zuruecksetzen
     * gelten fuer die Fassung des eingestellten Spiels. */
    char base[1100];
    app_hdd_base(g_client_disp, g_cur.hdd_root, base, sizeof base);
    lcfg_hdd_for_edition(base, g_edition, out, cap);
}

/* W231 US-LAUNCHER: Import-Hinweis zur Fassung. Spielstaende von EU und US
 * tragen dieselbe Title-ID und lassen sich nicht unterscheiden; nur ein
 * Launcher-Exportname ("-us-") verraet die Herkunft. Rein, testbar. */
void saves_edition_hint(const char *src_u8, int edition, char *out, size_t cap)
{
    const char *b = src_u8 ? src_u8 + strlen(src_u8) : "";
    int us_name, eu_name;
    while (src_u8 && b > src_u8 && b[-1] != '\\' && b[-1] != '/') {
        b--;
    }
    us_name = _strnicmp(b, "Steelbound-saves-us-", 20) == 0;
    eu_name = !us_name && _strnicmp(b, "Steelbound-saves-", 17) == 0 &&
              b[17] >= '0' && b[17] <= '9';
    if ((us_name && edition != LCFG_ED_US) || (eu_name && edition == LCFG_ED_US)) {
        _snprintf_s(out, cap, _TRUNCATE, LT("gui.si_ed_mismatch"),
                    LT(us_name ? "gui.ed_us" : "gui.ed_eu"),
                    LT(edition == LCFG_ED_US ? "gui.ed_us" : "gui.ed_eu"));
    } else {
        _snprintf_s(out, cap, _TRUNCATE, LT("gui.si_ed_hint"),
                    LT(edition == LCFG_ED_US ? "gui.ed_us" : "gui.ed_eu"));
    }
}

/* W187 KIA-LOAD: laeuft ein Spiel-Client (sb_ldr.exe)? */
int g_save_test_busy = -1; /* only set by the offscreen selftest */
int client_running(void)
{
    PROCESSENTRY32W pe;
    int found = 0;
    if (g_save_test_busy >= 0) return g_save_test_busy;
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE) {
        return 0;
    }
    pe.dwSize = sizeof pe;
    if (Process32FirstW(s, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"sb_ldr.exe") == 0) {
                found = 1;
            }
        } while (!found && Process32NextW(s, &pe));
    }
    CloseHandle(s);
    return found;
}

/* W187 KIA-LOAD: Spielstaende (TDATA/UDATA unter hdd) mit Rueckfrage in
 * <hdd>\_backup\saves-<zeit>\ sichern und leer neu anlegen. Rueckgabe wie
 * sb_saves_reset (0 ok, 1 nichts zu tun, -1 Fehler), 2 = abgelehnt/gesperrt. */
int adv_saves_reset_at(const char *hdd_u8, int check_running)
{
    char q[2000], err[300], bk8[1200];
    wchar_t w[1100], bk[800];
    int rc;
    if (!is_dir_u8(hdd_u8)) {
        set_status_k(LT("gui.saves_reset_none"), 0);
        return 1;
    }
    if (check_running && client_running()) {
        set_status_k(LT("gui.saves_reset_running"), 2);
        return 2;
    }
    _snprintf_s(q, sizeof q, _TRUNCATE, LT("gui.saves_reset_q"), hdd_u8);
    if (!ui_confirm(q, LT("gui.saves_reset_title"))) {
        return 2;
    }
    u8_to_w(hdd_u8, w, 1100);
    rc = sb_saves_reset(w, bk, 800, err, sizeof err);
    if (rc == 0) {
        w_to_u8(bk, bk8, sizeof bk8);
        _snprintf_s(q, sizeof q, _TRUNCATE, LT("gui.saves_reset_done"), bk8);
        set_status_k(q, 1);
    } else if (rc == 1) {
        set_status_k(LT("gui.saves_reset_none"), 0);
    } else {
        _snprintf_s(q, sizeof q, _TRUNCATE, LT("gui.saves_reset_fail"), err);
        set_status_k(q, 2);
    }
    return rc;
}


/* W199: native dialogs only; same effective HDD path and running-client gate as reset. */
void ui_to_settings(launcher_settings_t *s);
void adv_saves_transfer(int exporting)
{
    wchar_t file[1024] = L"", hdd[1100], title[160];
    char path[1024], msg[4096];
    const char *error = NULL;
    ls_counts counts;
    ls_plan *plan;
    int mode = 1, rc;
    OPENFILENAMEW ofn;
    SYSTEMTIME tm;
    if (client_running())
    {
        set_status_k(LT("gui.saves_reset_running"), 2);
        return;
    }
    ui_to_settings(&g_cur);
    adv_hdd_path(path, sizeof path);
    u8_to_w(path, hdd, 1100);
    u8_to_w(LT(exporting ? "gui.si_export" : "gui.si_import"), title, 160);
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFile = file;
    ofn.nMaxFile = 1024;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST;
    if (exporting)
    {
        GetLocalTime(&tm);
        /* W231: US-Export traegt "-us-" im Namen (Daten selbst sind gleich) */
        _snwprintf_s(file, 1024, _TRUNCATE, L"Steelbound-saves-%s%04u%02u%02u-%02u%02u%02u.zip",
                     g_edition == LCFG_ED_US ? L"us-" : L"",
                     tm.wYear, tm.wMonth, tm.wDay, tm.wHour, tm.wMinute, tm.wSecond);
        ofn.lpstrFilter = L"ZIP\0*.zip\0\0";
        ofn.lpstrDefExt = L"zip";
        if (!GetSaveFileNameW(&ofn))
            return;
        if (client_running())
        {
            set_status_k(LT("gui.saves_reset_running"), 2);
            return;
        }
        rc = ls_export(hdd, file, &error);
        set_status_k(LT(rc == 1 ? "gui.si_exported" : error), rc == 1 ? 1 : 2);
        if (rc < 0) ui_msgbox(g_hwnd, LT(error), LT("gui.si_export"), MB_OK | MB_ICONERROR);
        return;
    }
    else
    {
        HMENU menu = CreatePopupMenu();
        RECT rect;
        wchar_t a[160], b[160];
        int choice;
        u8_to_w(LT("gui.si_folder"), a, 160);
        u8_to_w(LT("gui.si_file"), b, 160);
        AppendMenuW(menu, MF_STRING, 1, a);
        AppendMenuW(menu, MF_STRING, 2, b);
        GetWindowRect(ctl(ID_SAVES_IMPORT), &rect);
        choice = TrackPopupMenu(menu, TPM_RETURNCMD, rect.left, rect.bottom, 0, g_hwnd, NULL);
        DestroyMenu(menu);
        if (choice == 1)
        {
            BROWSEINFOW bi;
            LPITEMIDLIST pidl;
            memset(&bi, 0, sizeof bi);
            bi.hwndOwner = g_hwnd;
            bi.lpszTitle = title;
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            pidl = SHBrowseForFolderW(&bi);
            if (!pidl)
                return;
            rc = SHGetPathFromIDListW(pidl, file);
            CoTaskMemFree(pidl);
            if (!rc)
                return;
        }
        else if (choice == 2)
        {
            ofn.lpstrFilter = L"ZIP / SAV\0*.zip;*.sav\0\0";
            ofn.Flags |= OFN_FILEMUSTEXIST;
            if (!GetOpenFileNameW(&ofn))
                return;
        }
        else
            return;
    }
    plan = ls_open(file, hdd, &counts, &error);
    if (!plan)
    {
        set_status_k(LT(error), 2);
        ui_msgbox(g_hwnd, LT(error), LT("gui.si_import"), MB_OK | MB_ICONERROR);
        return;
    }
    _snprintf_s(msg, sizeof msg, _TRUNCATE, LT("gui.si_preview"), counts.saves, counts.pilots,
                counts.replays, counts.conflicts);
    { /* W231: Fassung ist in den Daten nicht erkennbar -> Hinweis (+ Name) */
        char f8[1100], hint[600];
        w_to_u8(file, f8, sizeof f8);
        saves_edition_hint(f8, g_edition, hint, sizeof hint);
        strcat_s(msg, sizeof msg, "\n\n");
        strcat_s(msg, sizeof msg, hint);
    }
    if (counts.conflicts)
    {
        int answer;
        wchar_t names[2200];
        char names8[2200];
        ls_conflict_names(plan, names, 2200);
        w_to_u8(names, names8, sizeof names8);
        strcat_s(msg, sizeof msg, "\n\n");
        strcat_s(msg, sizeof msg, names8);
        strcat_s(msg, sizeof msg, "\n");
        strcat_s(msg, sizeof msg, LT("gui.si_conflict"));
        answer = ui_msgbox(g_hwnd, msg, LT("gui.si_import"), MB_YESNOCANCEL | MB_ICONQUESTION);
        mode = answer == IDYES ? 1 : answer == IDNO ? 2 : 0;
    }
    else
        mode = ui_confirm(msg, LT("gui.si_import")) ? 1 : 0;
    if (mode && client_running())
    {
        ls_close(plan);
        set_status_k(LT("gui.saves_reset_running"), 2);
        return;
    }
    rc = ls_import(plan, mode, &error);
    ls_close(plan);
    if (rc == 1)
    {
        _snprintf_s(msg, sizeof msg, _TRUNCATE, LT("gui.si_done"), counts.saves, counts.pilots,
                    counts.replays);
        set_status_k(msg, 1);
        ui_msgbox(g_hwnd, msg, LT("gui.si_import"), MB_OK | MB_ICONINFORMATION);
    }
    else if (rc < 0) {
        set_status_k(LT(error), 2);
        ui_msgbox(g_hwnd, LT(error), LT("gui.si_import"), MB_OK | MB_ICONERROR);
    }
}

/* Erweitert, rechte Spalte oben: Feinabstimmung (VSync, FPS-Grenze) und
 * Launcher (Hintergrund, Update-Pruefung). Gilt fuer beide Spiele
 * (W293: auch im Bereich LoC sichtbar). */
void paint_adv_shared(void)
{
    card(COL2, CY0, COLW, 148, LT("gui.adv_finetune")); /* W468: 124 -> 148 (Zeile Bildtakt) */
    label1(F_SMALL, LT("gui.adv_vsync"), COL2 + 18, CY0 + 40, 150, 30, C_DIM);
    label1(F_SMALL, LT("gui.adv_fpslimit"), COL2 + 18, CY0 + 74, 150, 30, C_DIM);
    label1(F_SMALL, LT("gui.adv_pacing"), COL2 + 18, CY0 + 108, 150, 30, C_DIM); /* W468 */
    /* W397: Karte "Launcher" (Hintergrund, Versionspruefung) liegt jetzt auf den Launcher-Einstellungen */
}

void paint_loc_adv(void); /* W293: Bereich LoC (Block "W276 LOCTAB") */

static void paint_adv(void)
{
    char t[1400];
    if (g_game == LGAME_LOC) { /* W293 LOC-LAYOUT: LoC-Karten links, gemeinsame rechts */
        paint_loc_adv();
        return;
    }
    /* Spieldateien (Ordner-Modus, Konzept 4.7) */
    card(CX0, CY0, COLW, 184, LT("gui.adv_files"));
    label1(F_TINY, LT("gui.adv_files_txt"), CX0 + 18, CY0 + 38, COLW - 36, 16, C_DIM);
    label1(F_TINY, LT("gui.adv_folder"), CX0 + 18, CY0 + 58, COLW - 36, 16, C_DIM);
    label1(F_TINY, LT("gui.adv_xbe"), CX0 + 18, CY0 + 108, COLW - 36, 16, C_DIM);
    if (!paint_reason(1, CX0 + 18, CY0 + 160, COLW - 36, 16)) {
        if (g_cur.media_root[0] && !g_data_ok) { /* Ordner gesetzt, aber ungueltig */
            label1(F_TINY, g_data_err, CX0 + 18, CY0 + 160, COLW - 36, 16, C_ERR);
        } else if (g_cur.media_root[0]) {
            label1(F_TINY, LT(g_rr[2].active ? "gui.adv_folder_ok" : "gui.adv_folder_ok2"),
                   CX0 + 18, CY0 + 160, COLW - 36, 16, C_OK);
        }
    }

    /* Spielstaende + Datenort */
    card(CX0, CY0 + 196, COLW, 146, LT("gui.adv_saves"));
    label1(F_SMALL, LT("gui.adv_hdd"), CX0 + 18, CY0 + 236, 84, 30, C_DIM);
    {
        char path[1024];
        const char *mk2;
        adv_data_loc(path, sizeof path, &mk2);
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.adv_data_fmt"), LT(mk2));
        if (g_edition == LCFG_ED_US) { /* W231: eigener US-Saveordner sichtbar */
            strncpy_s(t, sizeof t, LT("gui.adv_hdd_us"), _TRUNCATE);
            adv_hdd_path(path, sizeof path);
        }
        label1(F_TINY, t, CX0 + 18, CY0 + 270, COLW - 36 - 118, 15, C_DIM);
        L_text(F_TINY, path, CX0 + 18, CY0 + 285, COLW - 36 - 118, 15, C_TEXT,
               DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_PATH_ELLIPSIS);
    }

    /* Gespeicherte Einstellungen (Glossar: "Saved setup") */
    card(CX0, CY0 + 354, COLW, 142, LT("gui.adv_setups"));
    if (!paint_reason(28, CX0 + 18, CY0 + 479, COLW - 36, 12)) {
        label1(F_TINY, g_prof_info, CX0 + 18, CY0 + 479, COLW - 36, 12, C_DIM);
    }

    paint_adv_shared();

    card(COL2, ADV_DIAG_Y, COLW, CY0 + 496 - ADV_DIAG_Y, LT("gui.adv_diag"));
    /* W190 SAVE-LOCK: Schalter "Spielstaende schuetzen (Testhilfe)" oben in der
     * Karte (immer sichtbar), Erklaerung darunter (eingeklappt). */
    if (!g_diag_open) {
        label(F_TINY, LT("gui.save_lock_txt"), COL2 + 18, ADV_DIAG_Y + 72, COLW - 36, 30, C_DIM);
        label(F_TINY, LT("gui.adv_diag_txt"), COL2 + 18, ADV_DIAG_Y + 112,
              COLW - 36, 32, C_FAINT);
        return;
    }
    /* W250: Zeilen 32 px (Karte 32 px niedriger) */
    diag_row(LT("gui.path_client_dir"), g_client_disp, C_TEXT, ADV_DIAG_Y + ADV_DIAG_DY + 36);
    diag_row(LT("gui.path_exe"),
             g_has_ldr ? LT("gui.path_found") : LT("gui.path_missing"),
             g_has_ldr ? C_OK : C_ERR, ADV_DIAG_Y + ADV_DIAG_DY + 68);
    if (sb_userdata_mode() != SB_UD_LEGACY) { /* W150 USER-DATA */
        lcfg_user_cfg_path(g_client_dir, t, sizeof t);
    } else {
        _snprintf_s(t, sizeof t, _TRUNCATE, "%s\\user.cfg", g_client_disp);
    }
    diag_row(LT("gui.path_usercfg"), t, C_TEXT, ADV_DIAG_Y + ADV_DIAG_DY + 100);
    diag_row(LT("gui.sum_settings"), g_profile_line, C_TEXT, ADV_DIAG_Y + ADV_DIAG_DY + 132);
    paint_reason(27, COL2 + 18, ADV_DIAG_Y + ADV_DIAG_DY + 192, COLW - 36, 12);
}

void paint_packs(void); /* W148: unten definiert (Packs-Engine) */
static void compose(void)
{
    int W = SX(lay_tw()), H = SX(lay_th()), i; /* W281 */
    if (!g_base.px || g_base.w != W || g_base.h != H) {
        base_build();
    }
    if (!cv_make(&g_bb, W, H) || !g_base.px) {
        return;
    }
    GdiFlush();
    memcpy(g_bb.px, g_base.px, (size_t)W * H * 4);
    g_bb_setup = g_page == P_PLAY ? play_setup_shown() : -1; /* W220 */
    switch (g_page) {
    case P_PLAY: if (g_game == LGAME_LOC) { paint_loc_play(); } else { paint_play(); } break; /* W276 */
    case P_DISPLAY: paint_display(); break;
    case P_GFX: paint_gfx(); break;
    case P_PACKS: if (g_game == LGAME_LOC) { paint_lnet(); } else { paint_packs(); } break; /* W392: LoC Netzwerk */
    case P_CTRL: paint_ctrl(); break;
    case P_AUDIO: paint_audio(); break;
    case P_ADV: paint_adv(); break;
    case P_ALPHA: paint_alpha(); break; /* W272 */
    case P_LSET: paint_lset(); break; /* W397 */
    case P_NETI: paint_neti(); break; /* W470: steelnet > Info */
    }
    hub_paint_nav(); /* W470: Bereichstitel ueber den Reitern (Optionen/steelnet) */
    hdr_paint_status(); /* W397: Kopf rechts (steelnet / neue Version) */
    nrq_paint(); /* W496: Dialog ueber allem (Seite abgedunkelt) */
    /* Eingabefelder: Rahmen (Fokus = Akzent) */
    for (i = 0; i < g_ui_n; i++) {
        uictl_t *u = &g_ui[i];
        if (u->parent != g_hwnd || !ui_visible_on(u, g_page)) {
            continue;
        }
        if (u->kind == K_EDIT || u->kind == K_MEMO) {
            int en = IsWindowEnabled(u->h);
            if (lay_b()) { /* W281: Rahmen = Element-Rechteck */
                RECT q;
                int t = SX(1) < 1 ? 1 : SX(1);
                ui_px_rect(u, &q);
                cv_fill(&g_bb, q.left, q.top, q.right - q.left, q.bottom - q.top, C_INPUT,
                        255, SX(6));
                cv_ring(&g_bb, q.left, q.top, q.right - q.left, q.bottom - q.top,
                        g_focus_id == u->id ? C_ACCENT : C_INPUT_BRD, en ? 255 : 120, SX(6), t);
                continue;
            }
            L_fill(u->x, u->y, u->w, u->h_, C_INPUT, 255, 6);
            L_ring(u->x, u->y, u->w, u->h_,
                   g_focus_id == u->id ? C_ACCENT : C_INPUT_BRD,
                   en ? 255 : 120, 6);
        }
    }
    /* W171: Startsperre (Muster E) in der Fussleiste der Unterseiten:
     * Grund unter der Statuszeile, Statuszeile darueber. */
    {
        int lw = WIN_W - NAV_W - 28 - 330 - (g_undo_on ? 122 : 0);
        int blk = g_page != P_PLAY && g_game != LGAME_LOC && launch_blocked(); /* W276: SB-Sperre nicht im Bereich LoC */
        if (blk) {
            static const int kBlk[] = { 4, 3 };
            paint_reason(first_rule(kBlk, 2), NAV_W + 28, WIN_H - 22, lw, 16);
        } else if (g_page != P_PLAY && g_game == LGAME_LOC && !loc_launch_ok()) {
            char m[700]; /* LoC-Sperre (Disc/steelnet): Grund wie auf der Spielen-Seite */
            loc_gate_msg(m, sizeof m);
            L_text(F_TINY, m, NAV_W + 28, WIN_H - 22, lw, 16, C_WARN,
                   DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            blk = 1;
        }
    if (g_status[0]) {
        wchar_t *wt = u8_wdup(g_status);
        if (wt) {
            RECT r;
            int bw = SX(lw + lay_tw() - WIN_W), th; /* W281: b breiter */
            int area = blk ? SX(BOT_H) - SX(22) : SX(BOT_H);
            r.left = 0;
            r.top = 0;
            r.right = bw;
            r.bottom = 0;
            SelectObject(g_bb.dc, g_f[F_SMALL]);
            DrawTextW(g_bb.dc, wt, -1, &r,
                      DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
            th = r.bottom - r.top;
            if (th > area - SX(8)) {
                th = area - SX(8);
            }
            cv_textw(&g_bb, g_f[F_SMALL], wt, -1, SX(lay_nav() + 28),
                     SX(lay_th() - BOT_H) + (area - th) / 2, bw, th,
                     g_status_kind == 2   ? C_ERR
                     : g_status_kind == 1 ? C_OK
                                          : C_DIM,
                     DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
            free(wt);
        }
    }
    }
}

void redraw_all(void)
{
    compose();
    if (g_hwnd) {
        RedrawWindow(g_hwnd, NULL, NULL,
                     RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }
}

/* Nur Hintergrund neu (Text-Aenderung ohne Elemente darueber). */
void redraw_rect(int x, int y, int w, int h)
{
    RECT r;
    compose();
    lay_px(x, y, w, h, 1, 1, &r); /* W281 */
    if (g_hwnd) {
        InvalidateRect(g_hwnd, &r, FALSE);
    }
}

void set_status_k(const char *msg, int kind)
{
    strcpy_s(g_status, sizeof g_status, msg ? msg : "");
    g_status_kind = kind;
    if (g_undo_on) { /* W171: neue Meldung beendet das Rueckgaengig-Angebot */
        HWND u;
        g_undo_on = 0;
        u = ctl(ID_UNDO);
        if (u) {
            ShowWindow(u, SW_HIDE);
        }
    }
    redraw_rect(NAV_W, WIN_H - BOT_H, WIN_W - NAV_W, BOT_H);
}

/* W171 Muster C: Meldung zum automatischen Umschalten + "Rueckgaengig"
 * (Knopf in der Fussleiste, Strg+Z). before = Stand vor dem Umschalten. */
void set_status_undo(const char *msg, const launcher_settings_t *before)
{
    HWND u;
    set_status_k(msg, 0);
    g_undo_s = *before;
    g_undo_on = 1;
    u = ctl(ID_UNDO);
    if (u) {
        ShowWindow(u, SW_SHOWNA);
    }
    redraw_rect(NAV_W, WIN_H - BOT_H, WIN_W - NAV_W, BOT_H);
}

void set_status(const char *msg)
{
    set_status_k(msg, 0);
}

/* ------------------------------------------------------------------ */
/* Eigene Knoepfe zeichnen                                             */
/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */
/* W287 EXP-ICON: Experimentell-Symbol (Kolben, Akzentfarbe) und Warn-  */
/* Symbol (Dreieck, gelb). Ohne passende Symbolschrift: kurze Plakette  */
/* "EXP" bzw. "!" im Ring. Die Bedeutung haengt nie nur an der Farbe:   */
/* Form + Tooltip + zugaenglicher Name (Fenstertext).                   */
/* ------------------------------------------------------------------ */
static int xicon_glyph_ok(int warn)
{
    return warn ? g_has_icons : g_icon_fluent;
}

static void xicon_paint(canvas_t *c, int x, int y, int w, int h, int warn, int big)
{
    COLORREF col = warn ? C_WARN : C_ACCENT_HI;
    if (xicon_glyph_ok(warn)) {
        wchar_t ic = warn ? 0xE7BA : 0xF196;
        cv_textw(c, g_f[big ? F_ICON : F_ICONSM], &ic, 1, x, y, w, h, col,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    if (warn) {
        int d = (w < h ? w : h) - SX(4);
        cv_ring(c, x + (w - d) / 2, y + (h - d) / 2, d, d, col, 255, d / 2, SX(2) < 2 ? 2 : SX(2));
        cv_textw(c, g_f[F_SEMI], L"!", 1, x, y, w, h, col, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else {
        wchar_t t[16];
        int ph = SX(16) < h ? SX(16) : h;
        u8_to_w(LT("gui.exp_short"), t, 16);
        cv_fill(c, x, y + (h - ph) / 2, w, ph, col, 40, SX(3));
        cv_ring(c, x, y + (h - ph) / 2, w, ph, col, 150, SX(3), 1);
        cv_textw(c, g_f[F_TINY], t, -1, x, y, w, h, col, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

/* Gruppen, die auf einer Seite eingestellt werden (Warn-Symbol in der
 * Navigation). Effect Packs: Packs; Breitbild/Cockpit-Ansicht: Anzeige;
 * Rest (Licht, Nachbearbeitung, Effekte, Monitor, HD, Cockpit, HUD): Grafik. */
unsigned xwarn_page_mask(int page)
{
    unsigned wide = 1u << SB_OFG_WIDE, packs = 1u << SB_OFG_PACKS;
    switch (page) {
    case P_DISPLAY: return g_orig_mask & wide;
    case P_GFX: return g_orig_mask & ~(wide | packs);
    case P_PACKS: return g_game == LGAME_LOC ? 0 : (g_orig_mask & packs);
    default: return 0;
    }
}

/* Preset (LGP_*) experimentell? (Tabelle sb_origflag.h ueber lorig;
 * die Presets sind fest, darum einmal gerechnet) */
int xexp_preset(int p)
{
    static int done[2];
    static unsigned m[2][LGP_CUSTOM];
    int g = lcfg_gfx_loc(); /* W465: je Spiel (LoC: Enhanced-Werte sind kein Experimental) */
    if (!done[g]) {
        int i;
        for (i = 0; i < LGP_CUSTOM; i++) {
            m[g][i] = lorig_preset_mask(i);
        }
        done[g] = 1;
    }
    return p >= 0 && p < LGP_CUSTOM && m[g][p] != 0;
}

/* " (experimental)": steht im Fenstertext der experimentellen Presets
 * (zugaenglicher Name), gezeichnet wird statt dessen das Symbol. */
void xexp_sfx(char *out, size_t cap)
{
    _snprintf_s(out, cap, _TRUNCATE, " (%s)", LT("gui.exp_word"));
}

/* Sichtbare Laenge des Titels eines Preset-Optionsfelds (ohne Suffix). */
int xexp_title_len(int id, const wchar_t *txt, int n1)
{
    char s8[80];
    wchar_t sfx[80];
    int sl;
    if (id < ID_GP0 || id >= ID_GP0 + LGP_CUSTOM || !xexp_preset(id - ID_GP0)) {
        return n1;
    }
    xexp_sfx(s8, sizeof s8);
    u8_to_w(s8, sfx, 80);
    sl = (int)wcslen(sfx);
    if (sl > 0 && n1 >= sl && wcsncmp(txt + n1 - sl, sfx, sl) == 0) {
        return n1 - sl;
    }
    return n1;
}

/* W398 LAUNCHER-GAMESWITCH (Block vor hdr_paint_status) */
void gametab_paint(canvas_t *c, const uictl_t *u, int w, int h, const wchar_t *txt, int n, int focus);
int acc_danger_id(int id); /* W418: Konto loeschen (W502: Aufklapper-Textlinks entfielen, alles echte Knoepfe) */
void draw_item(const DRAWITEMSTRUCT *d)
{
    uictl_t *u = ui_by_hwnd(d->hwndItem);
    canvas_t c;
    int w = d->rcItem.right - d->rcItem.left, h = d->rcItem.bottom - d->rcItem.top;
    int pressed = (d->itemState & ODS_SELECTED) != 0;
    int focus = (d->itemState & ODS_FOCUS) && !(d->itemState & ODS_NOFOCUSRECT);
    int dis = (d->itemState & ODS_DISABLED) != 0;
    wchar_t txt[256];
    int n;
    if (d->CtlType == ODT_LISTBOX) { /* W172: Packs-Liste */
        packs_draw_row(d);
        return;
    }
    memset(&c, 0, sizeof c);
    if (!u || !cv_make(&c, w, h)) {
        return;
    }
    /* Hintergrund: aus dem fertigen Bild (Hauptfenster) oder einfarbig */
    GdiFlush();
    if (u->parent == g_hwnd && g_bb.px) {
        POINT p = { 0, 0 };
        int y;
        MapWindowPoints(d->hwndItem, g_hwnd, &p, 1);
        for (y = 0; y < h; y++) {
            int sy = p.y + y;
            if (sy < 0 || sy >= g_bb.h || p.x < 0 || p.x + w > g_bb.w) {
                cv_fill(&c, 0, y, w, 1, C_BASE, 255, 0);
                continue;
            }
            memcpy(c.px + (size_t)y * w, g_bb.px + (size_t)sy * g_bb.w + p.x,
                   (size_t)w * 4);
        }
    } else {
        cv_fill(&c, 0, 0, w, h, C_DIALOG, 255, 0);
    }
    n = GetWindowTextW(d->hwndItem, txt, 256);
    switch (u->kind) {
    case K_NAV: {
        int hp = hub_tab_page(u->id); /* W470: Reiter der Bereiche Optionen/steelnet (sonst -1) */
        int sel = hp >= 0 ? hp == g_page : u->id - ID_NAV0 == g_page;
        if (u->id == ID_NAVSUB_MATCH || u->id == ID_NAVSUB_ACC) { /* W399: Unterpunkt von Multiplayer */
            int ssel = (u->id == ID_NAVSUB_ACC) == (acc_sub_active() != 0);
            int gx = SX(g_has_icons ? 25 : 12); /* Fuehrungslinie unter dem Symbol des Elternpunkts */
            cv_fill(&c, gx, 0, SX(1), h, RGB(255, 255, 255), 40, 0);
            if (ssel) {
                cv_fill(&c, gx + SX(14), 0, w - gx - SX(14), h, RGB(255, 255, 255), 14, SX(5));
                cv_fill(&c, gx - SX(1), SX(7), SX(3), h - SX(14), C_ACCENT, 255, SX(1));
            } else if (u->hover) {
                cv_fill(&c, gx + SX(14), 0, w - gx - SX(14), h, RGB(255, 255, 255), 10, SX(5));
            }
            cv_textw(&c, g_f[F_SMALL], txt, n, gx + SX(24), 0, w - gx - SX(30), h,
                     ssel ? C_TEXT : C_DIM, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            break;
        }
        if (sel) {
            cv_fill(&c, 0, 0, w, h, RGB(255, 255, 255), 22, SX(6));
            cv_fill(&c, 0, SX(9), SX(3), h - SX(18), C_ACCENT, 255, SX(1));
        } else if (u->hover) {
            cv_fill(&c, 0, 0, w, h, RGB(255, 255, 255), 12, SX(6));
        }
        if (g_has_icons) {
            wchar_t ic = hp >= 0 ? hub_tab_icon(u->id) /* W470 */
                         : g_game == LGAME_LOC && u->id - ID_NAV0 == P_PACKS ? 0xE774 : kPageIcon[u->id - ID_NAV0]; /* W392 */
            cv_textw(&c, g_f[F_ICON], &ic, 1, SX(14), 0, SX(24), h,
                     sel ? C_ACCENT_HI : C_DIM,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        { /* W287: zweite Zeile = zugaenglicher Hinweis (nicht gezeichnet), Warn-Symbol rechts */
            wchar_t *nl = wcschr(txt, L'\n');
            int warn = hp < 0 && xwarn_page_mask(u->id - ID_NAV0) != 0;
            if (nl) {
                n = (int)(nl - txt);
            }
            cv_textw(&c, sel ? g_f[F_SEMI] : g_f[F_BODY], txt, n,
                     SX(g_has_icons ? 46 : 18), 0, w - SX(g_has_icons ? 46 : 18) - (warn ? SX(40) : 0), h, /* W403 */
                     sel ? C_TEXT : C_DIM, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            if (warn) {
                xicon_paint(&c, w - SX(14) - SX(24), 0, SX(24), h, 1, 0); /* W403: Rand rechts 14 = Symbol links */
            }
        }
        break;
    }
    case K_ICON: /* W287 */
        xicon_paint(&c, 0, 0, w, h, u->id == ID_XWARN_SB || u->id == ID_XWARN_LOC, 1);
        break;
    case K_TBMIN:
    case K_TBCLOSE: {
        if (u->hover) {
            cv_fill(&c, 0, 0, w, h,
                    u->kind == K_TBCLOSE ? RGB(196, 43, 28) : RGB(255, 255, 255),
                    u->kind == K_TBCLOSE ? 255 : 26, 0);
        }
        if (g_has_icons) {
            wchar_t ic = u->kind == K_TBCLOSE ? 0xE8BB : 0xE921;
            cv_textw(&c, g_f[F_ICONSM], &ic, 1, 0, 0, w, h, C_TEXT,
                     DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            cv_textw(&c, g_f[F_BODY], u->kind == K_TBCLOSE ? L"x" : L"_", 1,
                     0, 0, w, h, C_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        break;
    }
    case K_TOGGLE: {
        int tw = SX(38), th = SX(20), ty = (h - th) / 2;
        COLORREF track = u->checked ? C_ACCENT : RGB(70, 77, 90);
        if (u->hover && !dis) {
            track = u->checked ? C_ACCENT_HI : RGB(84, 92, 106);
        }
        cv_fill(&c, 0, ty, tw, th, track, dis ? 110 : 255, th / 2);
        {
            int kd = th - SX(6);
            int kx = u->checked ? tw - SX(3) - kd : SX(3);
            cv_fill(&c, kx, ty + SX(3), kd, kd,
                    u->checked ? C_ON_ACCENT : RGB(230, 233, 238),
                    dis ? 140 : 255, kd / 2);
        }
        if (focus) {
            cv_ring(&c, 0, ty - SX(2), tw + SX(4), th + SX(4), C_ACCENT_HI,
                    200, th / 2 + SX(2), 1);
        }
        cv_textw(&c, g_f[F_BODY], txt, n, tw + SX(12), 0, w - tw - SX(12), h,
                 dis ? C_FAINT : C_TEXT,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        break;
    }
    case K_RADIO: { /* W173: Kreis + Titel, darunter Hilfe (gedimmt) */
        int d0 = SX(18), rx = SX(2), ry = SX(3), tx = d0 + SX(14), n1 = n, nt, xi = 0;
        wchar_t *nl = wcschr(txt, L'\n');
        COLORREF ring = u->checked ? C_ACCENT : RGB(120, 128, 140);
        if (nl) {
            n1 = (int)(nl - txt);
        }
        if (u->hover && !dis) {
            ring = u->checked ? C_ACCENT_HI : C_DIM;
        }
        cv_ring(&c, rx, ry, d0, d0, ring, dis ? 110 : 255, d0 / 2, SX(2) < 2 ? 2 : SX(2));
        if (u->checked) {
            int di = d0 - SX(10);
            cv_fill(&c, rx + SX(5), ry + SX(5), di, di, C_ACCENT, dis ? 140 : 255, di / 2);
        }
        if (focus) {
            cv_ring(&c, 0, ry - SX(2), d0 + SX(4), d0 + SX(4), C_ACCENT_HI, 200,
                    d0 / 2 + SX(2), 1);
        }
        nt = xexp_title_len(u->id, txt, n1);
        if (u->id >= ID_GP0 && u->id < ID_GP0 + LGP_CUSTOM && xexp_preset(u->id - ID_GP0)) {
            /* W287: "(experimental)" bleibt im Fenstertext (zugaenglicher Name),
             * gezeichnet wird statt dessen das Symbol hinter dem Namen */
            SIZE sz;
            SelectObject(c.dc, g_f[u->checked ? F_SEMI : F_BODY]);
            GetTextExtentPoint32W(c.dc, txt, nt, &sz);
            xi = sz.cx;
            if (xi > w - tx - SX(36)) {
                xi = w - tx - SX(36);
            }
        }
        cv_textw(&c, g_f[u->checked ? F_SEMI : F_BODY], txt, nt, tx, 0, xi ? xi + SX(2) : w - tx, SX(24),
                 dis ? C_FAINT : C_TEXT,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (xi) {
            xicon_paint(&c, tx + xi + SX(6), 0, xicon_glyph_ok(0) ? SX(22) : SX(30), SX(24), 0, 1);
        }
        if (nl) {
            cv_textw(&c, g_f[F_TINY], nl + 1, n - n1 - 1, tx, SX(21), w - tx, h - SX(21),
                     C_DIM, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
        }
        break;
    }
    default: { /* K_BTN, K_ACCENT, K_BIG, K_SEG (W172: gewaehlt = Akzent) */
        int acc;
        COLORREF bg, fg;
        int rad;
        if (gametab_id(u->id)) { /* W398: Wortmarken-Tab statt Knopf (kein Hintergrund) */
            gametab_paint(&c, u, w, h, txt, n, focus);
            break;
        }
        acc = u->kind == K_ACCENT || u->kind == K_BIG || (u->kind == K_SEG && u->checked);
        bg = acc ? C_ACCENT : C_BTN;
        fg = acc ? C_ON_ACCENT : C_TEXT;
        if (acc_danger_id(u->id) && !dis) { /* W418: Konto loeschen rot (selten, ganz unten) */
            fg = C_DANGER;
        }
        rad = SX(u->kind == K_BIG ? 10 : 6);
        if (!dis && u->hover) {
            bg = acc ? C_ACCENT_HI : C_BTN_HI;
        }
        if (!dis && pressed) {
            bg = acc ? C_ACCENT_LO : RGB(36, 40, 49);
        }
        if (dis) {
            bg = RGB(40, 44, 52);
            fg = C_FAINT;
        }
        cv_fill(&c, 0, 0, w, h, bg, u->parent == g_hwnd ? 240 : 255, rad);
        if (!acc) {
            if (acc_danger_id(u->id) && !dis) {
                cv_ring(&c, 0, 0, w, h, C_DANGER, 110, rad, 1); /* W418 */
            } else {
                cv_ring(&c, 0, 0, w, h, RGB(255, 255, 255), 22, rad, 1);
            }
        }
        if (focus) {
            cv_ring(&c, 0, 0, w, h, acc ? RGB(255, 236, 200) : C_ACCENT, 255,
                    rad, SX(2) < 2 ? 2 : SX(2));
        }
        if (u->kind == K_BIG) {
            SetTextCharacterExtra(c.dc, SX(2));
        }
        if (u->id == ID_LSET_OPEN) { /* W397: Zahnrad (Fenstertext = zugaenglicher Name) */
            wchar_t ic = g_has_icons ? 0xE713 : L'*';
            cv_textw(&c, g_has_icons ? g_f[F_ICON] : g_f[F_SEMI], &ic, 1, 0, 0, w, h, fg,
                     DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            break;
        }
        if (u->id == ID_START_MENU) { /* W171: Pfeil "v" (Launch-Menue) */
            wchar_t ic = g_has_icons ? 0xE70D : L'v';
            cv_textw(&c, g_has_icons ? g_f[F_ICONSM] : g_f[F_SEMI], &ic, 1, 0,
                     0, w, h, fg, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SetTextCharacterExtra(c.dc, 0);
            break;
        }
        cv_textw(&c, u->kind == K_BIG ? g_f[F_BIG] : g_f[F_SEMI], txt, n, 0,
                 0, w, h, fg, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SetTextCharacterExtra(c.dc, 0);
        break;
    }
    }
    GdiFlush();
    BitBlt(d->hDC, d->rcItem.left, d->rcItem.top, w, h, c.dc, 0, 0, SRCCOPY);
    cv_free(&c);
}

/* Farben der System-Elemente (Edits, Listen, Combos) */
HBRUSH g_br_input, g_br_dialog;

LRESULT ctlcolor(HDC dc, HWND child, int is_static_bg)
{
    uictl_t *u = ui_by_hwnd(child);
    if (!g_br_input) {
        g_br_input = CreateSolidBrush(C_INPUT);
    }
    if (!g_br_dialog) {
        g_br_dialog = CreateSolidBrush(C_DIALOG);
    }
    if (u && u->kind == K_STATIC) {
        SetTextColor(dc, C_TEXT);
        SetBkColor(dc, C_DIALOG);
        return (LRESULT)g_br_dialog;
    }
    (void)is_static_bg;
    SetTextColor(dc, (u && !IsWindowEnabled(child)) ? C_FAINT : C_TEXT);
    SetBkColor(dc, C_INPUT);
    return (LRESULT)g_br_input;
}

/* ------------------------------------------------------------------ */
