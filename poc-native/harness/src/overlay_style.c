/* overlay_style.c - W461 OVERLAY-CORE: gemeinsamer Look und Zeichenweg der Overlays (Beschreibung in overlay_style.h). */
#include "render_int.h"
#include "overlay_style.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

const ovs_frame_t OVS_FRAME_MENU = { 8.0f, 20.0f, 6.0f, 2.0f };
const ovs_frame_t OVS_FRAME_HUD = { 7.0f, 15.0f, 5.0f, 1.0f };

/* ------------------------------------------------------------------ Skalierung, Spielbox, Ecken */
int ovs_px(float f, float units)
{
    int v = (int)(units * f + 0.5f);
    return v < 1 ? 1 : v;
}

float ovs_scale_ui(unsigned box_h)
{
    float f = (float)box_h / OVS_REF_UI;
    return f < 1.0f ? 1.0f : f;
}

void ovs_fit_box(float aspect, unsigned w, unsigned h, unsigned *bx, unsigned *by, unsigned *bw, unsigned *bh)
{
    float ba = (float)w / (float)(h ? h : 1);
    if (aspect >= ba) {
        *bw = w;
        *bh = (unsigned)((float)w / aspect + 0.5f);
        if (*bh < 1u) *bh = 1u;
        if (*bh > h) *bh = h;
        *bx = 0;
        *by = (h - *bh) / 2;
    } else {
        *bh = h;
        *bw = (unsigned)((float)h * aspect + 0.5f);
        if (*bw < 1u) *bw = 1u;
        if (*bw > w) *bw = w;
        *bx = (w - *bw) / 2;
        *by = 0;
    }
}

void ovs_loc_box(unsigned *bx, unsigned *by, unsigned *bw, unsigned *bh)
{
    ovs_fit_box(g_loc_wide ? 16.0f / 9.0f : 4.0f / 3.0f, g_bb_w, g_bb_h, bx, by, bw, bh);
}

void ovs_anchor(ovs_corner c, unsigned w, unsigned h, unsigned margin, unsigned *x, unsigned *y)
{
    *x = (c == OVS_TOP_LEFT || c == OVS_BOTTOM_LEFT) ? margin : g_bb_w - w - margin;
    *y = (c == OVS_TOP_RIGHT || c == OVS_TOP_LEFT) ? margin : g_bb_h - h - margin;
}

/* ------------------------------------------------------------------ Belegungsplan */
static unsigned s_lay_frame = 0xFFFFFFFFu;
static unsigned s_lay_r[OVS_SLOT_N][4];
static unsigned char s_lay_on[OVS_SLOT_N];

static void lay_sync(void)
{
    if (s_lay_frame != g_present_no) {
        s_lay_frame = g_present_no;
        memset(s_lay_on, 0, sizeof s_lay_on);
    }
}

static int lay_overlap(const unsigned *r, unsigned x, unsigned y, unsigned w, unsigned h)
{
    return x < r[0] + r[2] && r[0] < x + w && y < r[1] + r[3] && r[1] < y + h;
}

void ovs_layout_claim(ovs_slot id, unsigned x, unsigned y, unsigned w, unsigned h)
{
    if ((unsigned)id >= OVS_SLOT_N) return;
    lay_sync();
    s_lay_r[id][0] = x;
    s_lay_r[id][1] = y;
    s_lay_r[id][2] = w;
    s_lay_r[id][3] = h;
    s_lay_on[id] = 1;
}

int ovs_layout_get(ovs_slot id, unsigned r[4])
{
    if ((unsigned)id >= OVS_SLOT_N) return 0;
    lay_sync();
    if (!s_lay_on[id]) return 0;
    memcpy(r, s_lay_r[id], sizeof s_lay_r[id]);
    return 1;
}

int ovs_layout_hits(ovs_slot self, unsigned x, unsigned y, unsigned w, unsigned h)
{
    int i;
    lay_sync();
    for (i = 0; i < OVS_SLOT_N; i++)
        if (i != (int)self && s_lay_on[i] && lay_overlap(s_lay_r[i], x, y, w, h)) return 1;
    return 0;
}

int ovs_layout_place(ovs_slot id, ovs_corner c, unsigned w, unsigned h, unsigned margin, unsigned gap, unsigned *x,
                     unsigned *y)
{
    int top = (c == OVS_TOP_RIGHT || c == OVS_TOP_LEFT), guard = 0;
    unsigned px, py;
    if ((unsigned)id >= OVS_SLOT_N || w + 2u * margin > g_bb_w || h + 2u * margin > g_bb_h) return 0;
    lay_sync();
    s_lay_on[id] = 0;
    ovs_anchor(c, w, h, margin, &px, &py);
    /* von der Ecke weg schieben, bis nichts mehr im Weg ist (hoechstens einmal je Slot) */
    while (guard++ <= OVS_SLOT_N) {
        int i, hit = -1;
        for (i = 0; i < OVS_SLOT_N; i++)
            if (s_lay_on[i] && lay_overlap(s_lay_r[i], px, py, w, h)) { hit = i; break; }
        if (hit < 0) {
            ovs_layout_claim(id, px, py, w, h);
            *x = px;
            *y = py;
            return 1;
        }
        if (top) {
            py = s_lay_r[hit][1] + s_lay_r[hit][3] + gap;
            if (py + h + margin > g_bb_h) return 0;
        } else {
            if (s_lay_r[hit][1] < h + gap + margin) return 0;
            py = s_lay_r[hit][1] - h - gap;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ Font-Cache */
typedef struct {
    const wchar_t *name;
    BYTE pitch;
} ovs_face_def;
static const ovs_face_def kFace[OVS_FACE_N] = {
    { L"Consolas", FIXED_PITCH | FF_MODERN },
    { L"Bahnschrift SemiCondensed", VARIABLE_PITCH | FF_SWISS },
    { L"Bahnschrift", VARIABLE_PITCH | FF_SWISS },
    { L"Segoe UI", VARIABLE_PITCH | FF_SWISS },
    { L"Segoe Fluent Icons", DEFAULT_PITCH },
};

#define OVS_FONTS 48
typedef struct {
    HFONT f;
    int face, weight, px, refs;
} ovs_font_ent;
static ovs_font_ent s_fonts[OVS_FONTS];
static SRWLOCK s_font_lk = SRWLOCK_INIT;

const wchar_t *ovs_face_name(ovs_face face)
{
    return (unsigned)face < OVS_FACE_N ? kFace[face].name : L"";
}

static HFONT font_create(int face, int weight, int px)
{
    return CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       ANTIALIASED_QUALITY, kFace[face].pitch, kFace[face].name);
}

/* Zaehler der Schrift f senken (unter Sperre); 0 -> loeschen */
static void font_release_locked(HFONT f)
{
    int i;
    for (i = 0; i < OVS_FONTS; i++) {
        if (s_fonts[i].f == f) {
            if (--s_fonts[i].refs <= 0) {
                DeleteObject(f);
                memset(&s_fonts[i], 0, sizeof s_fonts[i]);
            }
            return;
        }
    }
}

HFONT ovs_font_get(ovs_font_t *s, ovs_face face, int weight, int px)
{
    HFONT nf = NULL, old;
    int i, free_i = -1, own = 0;
    if (!s || (unsigned)face >= OVS_FACE_N || px < 1) return NULL;
    if (s->f && s->face == (int)face && s->weight == weight && s->px == px) return s->f;
    AcquireSRWLockExclusive(&s_font_lk);
    for (i = 0; i < OVS_FONTS; i++) {
        if (s_fonts[i].f && s_fonts[i].face == (int)face && s_fonts[i].weight == weight && s_fonts[i].px == px) {
            s_fonts[i].refs++;
            nf = s_fonts[i].f;
            break;
        }
        if (!s_fonts[i].f && free_i < 0) free_i = i;
    }
    if (!nf) {
        nf = font_create((int)face, weight, px);
        if (nf && free_i >= 0) {
            s_fonts[free_i].f = nf;
            s_fonts[free_i].face = (int)face;
            s_fonts[free_i].weight = weight;
            s_fonts[free_i].px = px;
            s_fonts[free_i].refs = 1;
        } else if (nf) {
            own = 1; /* Cache voll: Schrift gehoert dem Halter allein */
        }
    }
    old = s->f;
    if (old) {
        if (s->own) DeleteObject(old);
        else font_release_locked(old);
    }
    ReleaseSRWLockExclusive(&s_font_lk);
    s->f = nf;
    s->face = (int)face;
    s->weight = weight;
    s->px = nf ? px : 0;
    s->own = own;
    return nf;
}

void ovs_font_put(ovs_font_t *s)
{
    if (!s || !s->f) return;
    AcquireSRWLockExclusive(&s_font_lk);
    if (s->own) DeleteObject(s->f);
    else font_release_locked(s->f);
    ReleaseSRWLockExclusive(&s_font_lk);
    memset(s, 0, sizeof *s);
}

/* ------------------------------------------------------------------ Zeichenflaeche */
static void canvas_font_off(ovs_canvas_t *c)
{
    if (c->dc && c->font_old) {
        SelectObject(c->dc, c->font_old);
        c->font_old = NULL;
    }
}

HDC ovs_canvas_dc(ovs_canvas_t *c)
{
    if (!c->dc) c->dc = CreateCompatibleDC(NULL);
    return c->dc;
}

int ovs_canvas_size(ovs_canvas_t *c, unsigned w, unsigned h)
{
    BITMAPINFO bi;
    if (c->dc && c->bmp && w == c->w && h == c->h) return 1;
    if (!c->dc) {
        c->dc = CreateCompatibleDC(NULL);
        if (!c->dc) return 0;
    }
    if (c->bmp) {
        SelectObject(c->dc, c->bmp_old);
        DeleteObject(c->bmp);
        c->bmp = NULL;
        c->bits = NULL;
    }
    canvas_font_off(c);
    c->font_px = 0;
    c->w = c->h = 0;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = (LONG)w;
    bi.bmiHeader.biHeight = -(LONG)h; /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    c->bmp = CreateDIBSection(c->dc, &bi, DIB_RGB_COLORS, (void **)&c->bits, NULL, 0);
    if (!c->bmp || !c->bits) {
        if (c->bmp) DeleteObject(c->bmp);
        c->bmp = NULL;
        c->bits = NULL;
        return 0;
    }
    c->bmp_old = (HBITMAP)SelectObject(c->dc, c->bmp);
    SetBkMode(c->dc, TRANSPARENT);
    c->w = w;
    c->h = h;
    return 1;
}

int ovs_canvas_font(ovs_canvas_t *c, ovs_face face, int weight, int px)
{
    HFONT f;
    if (!c->dc) return 0;
    if (c->font_old && c->font.f && c->font.face == (int)face && c->font.weight == weight && c->font.px == px) return 1;
    canvas_font_off(c); /* alte Schrift abwaehlen, bevor der Cache sie freigeben darf */
    f = ovs_font_get(&c->font, face, weight, px);
    c->font_px = px;
    if (!f) return 0;
    c->font_old = (HFONT)SelectObject(c->dc, f);
    return 1;
}

void ovs_canvas_free(ovs_canvas_t *c)
{
    float f = c->f;
    if (c->dc) {
        canvas_font_off(c);
        if (c->bmp) {
            SelectObject(c->dc, c->bmp_old);
            DeleteObject(c->bmp);
        }
        DeleteDC(c->dc);
    }
    ovs_font_put(&c->font);
    memset(c, 0, sizeof *c);
    c->f = f;
}

/* ------------------------------------------------------------------ Textur */
void ovs_tex_free(ovs_tex_t *t)
{
    if (t->srv) ID3D11ShaderResourceView_Release(t->srv);
    if (t->tex) ID3D11Texture2D_Release(t->tex);
    memset(t, 0, sizeof *t);
}

int ovs_tex_ensure(ovs_tex_t *t, unsigned w, unsigned h)
{
    D3D11_TEXTURE2D_DESC td;
    if (t->tex && t->w == w && t->h == h) return 1;
    ovs_tex_free(t);
    if (!g_dev || !w || !h) return 0;
    memset(&td, 0, sizeof td);
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &t->tex)) ||
        FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)t->tex, NULL, &t->srv))) {
        ovs_tex_free(t);
        return 0;
    }
    t->w = w;
    t->h = h;
    return 2;
}

int ovs_tex_write(ovs_tex_t *t, const unsigned *px)
{
    D3D11_MAPPED_SUBRESOURCE map;
    unsigned y;
    if (!t->tex || !px || !g_ctx) return 0;
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)t->tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return 0;
    for (y = 0; y < t->h; y++)
        memcpy((unsigned char *)map.pData + (size_t)y * map.RowPitch, px + (size_t)y * t->w, t->w * 4u);
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)t->tex, 0);
    return 1;
}

int ovs_tex_upload(ovs_tex_t *t, const unsigned *px, unsigned w, unsigned h)
{
    return ovs_tex_ensure(t, w, h) && ovs_tex_write(t, px);
}

/* ------------------------------------------------------------------ Zeichenhelfer */
void ovs_fill(HDC dc, int x0, int y0, int x1, int y1, COLORREF c)
{
    RECT r;
    HBRUSH b = CreateSolidBrush(c);
    r.left = x0;
    r.top = y0;
    r.right = x1;
    r.bottom = y1;
    FillRect(dc, &r, b);
    DeleteObject(b);
}

void ovs_text(HDC dc, int x, int y, const wchar_t *s, COLORREF c, int right)
{
    SetTextColor(dc, c);
    SetTextAlign(dc, (right ? TA_RIGHT : TA_LEFT) | TA_TOP);
    TextOutW(dc, x, y, s, (int)wcslen(s));
}

void ovs_text_shadow(HDC dc, int x, int y, const wchar_t *s, COLORREF c, COLORREF shadow, int off, int right)
{
    if (off > 0) ovs_text(dc, x + off, y + off, s, shadow, right);
    ovs_text(dc, x, y, s, c, right);
}

void ovs_panel(HDC dc, float f, int W, int H, const ovs_frame_t *fr)
{
    int b = ovs_px(f, OVS_BORDER), cl = ovs_px(f, fr->bracket);
    ovs_fill(dc, 0, 0, W, H, OVS_C_BG);
    ovs_fill(dc, 0, 0, W, b, OVS_C_LINE);
    ovs_fill(dc, 0, H - b, W, H, OVS_C_LINE);
    ovs_fill(dc, 0, 0, b, H, OVS_C_LINE);
    ovs_fill(dc, W - b, 0, W, H, OVS_C_LINE);
    ovs_fill(dc, 0, 0, cl, 2 * b + 1, OVS_C_TXT);
    ovs_fill(dc, 0, 0, 2 * b + 1, cl, OVS_C_TXT);
    ovs_fill(dc, W - cl, H - 2 * b - 1, W, H, OVS_C_TXT);
    ovs_fill(dc, W - 2 * b - 1, H - cl, W, H, OVS_C_TXT);
}

int ovs_head(HDC dc, float f, int W, int font_px, const ovs_frame_t *fr, const wchar_t *title, const wchar_t *tag)
{
    int b = ovs_px(f, OVS_BORDER), head = ovs_px(f, fr->head), pad = ovs_px(f, fr->pad), in = ovs_px(f, fr->inset);
    int i2 = ovs_px(f, 2.0f), ty;
    ovs_fill(dc, b + i2, b + i2, W - b - i2, head, OVS_C_HEAD);
    ty = b + i2 + (head - b - i2 - font_px) / 2 - ovs_px(f, 0.5f);
    if (title && title[0]) ovs_text(dc, pad + in, ty, title, OVS_C_BG, 0);
    if (tag && tag[0]) ovs_text(dc, W - pad - in, ty, tag, OVS_C_BG, 1);
    return head;
}

int ovs_frame(HDC dc, float f, int W, int H, int font_px, const ovs_frame_t *fr, const wchar_t *title,
              const wchar_t *tag)
{
    ovs_panel(dc, f, W, H, fr);
    return ovs_head(dc, f, W, font_px, fr, title, tag);
}

void ovs_row(HDC dc, int pad, int W, int y, const wchar_t *left, COLORREF cl, const wchar_t *right, COLORREF cr)
{
    if (left && left[0]) ovs_text(dc, pad, y, left, cl, 0);
    if (right && right[0]) ovs_text(dc, W - pad, y, right, cr, 1);
}

void ovs_dot(HDC dc, int cx, int cy, int d, COLORREF c)
{
    int r0 = d / 2;
    if (d < 1) d = 1;
    ovs_fill(dc, cx - r0, cy - r0, cx - r0 + d, cy - r0 + d, c);
}

/* ------------------------------------------------------------------ Umrechnung */
void ovs_finish(unsigned *bits, unsigned w, unsigned h, float f)
{
    unsigned n = w * h, px;
    (void)h;
    for (px = 0; px < n; px++) {
        unsigned c = bits[px], r = (c >> 16) & 255u, g = (c >> 8) & 255u, bl = c & 255u;
        float a = OVS_ALPHA, k = (((px / w) & 1u) && f >= 2.0f) ? OVS_SCANLINE : 1.0f;
        r = (unsigned)((float)r * a * k + 0.5f);
        g = (unsigned)((float)g * a * k + 0.5f);
        bl = (unsigned)((float)bl * a * k + 0.5f);
        bits[px] = ((unsigned)(a * 255.0f + 0.5f) << 24) | (r << 16) | (g << 8) | bl;
    }
}

void ovs_pm_span(unsigned *dst, const unsigned *src, int n, unsigned col, unsigned bg, float bga)
{
    float cr = (float)((col >> 16) & 255u), cg = (float)((col >> 8) & 255u), cb = (float)(col & 255u);
    float br = (float)((bg >> 16) & 255u) * bga, bgc = (float)((bg >> 8) & 255u) * bga, bb = (float)(bg & 255u) * bga;
    int x;
    for (x = 0; x < n; x++) {
        unsigned px = src[x];
        unsigned m = (px >> 16) & 255u, g2 = (px >> 8) & 255u, b2 = px & 255u;
        float c, oa;
        if (g2 > m) m = g2;
        if (b2 > m) m = b2;
        c = (float)m / 255.0f;
        oa = c + bga * (1.0f - c);
        dst[x] = ((unsigned)(oa * 255.0f + 0.5f) << 24) | ((unsigned)(cr * c + br * (1.0f - c) + 0.5f) << 16) |
                 ((unsigned)(cg * c + bgc * (1.0f - c) + 0.5f) << 8) | (unsigned)(cb * c + bb * (1.0f - c) + 0.5f);
    }
}

int ovs_buf(unsigned **buf, unsigned *cap, unsigned n)
{
    unsigned *p;
    if (*buf && *cap >= n) return 1;
    p = (unsigned *)malloc((size_t)n * 4u);
    if (!p) return 0;
    free(*buf);
    *buf = p;
    *cap = n;
    return 1;
}

unsigned ovs_pm_px(unsigned rgb, float a)
{
    if (a < 0.0f) a = 0.0f;
    if (a > 1.0f) a = 1.0f;
    return ((unsigned)(a * 255.0f + 0.5f) << 24) | ((unsigned)((float)((rgb >> 16) & 255u) * a + 0.5f) << 16) |
           ((unsigned)((float)((rgb >> 8) & 255u) * a + 0.5f) << 8) | (unsigned)((float)(rgb & 255u) * a + 0.5f);
}

/* ------------------------------------------------------------------ LoC-Panel (W467 OVERLAY-UNIFY) */
int ovs_lp_row_top(float f, int head, int i, int *bot)
{
    float t, b;
    if (head && i == 0) {
        t = OVS_LP_PAD - 1.0f;
        b = OVS_LP_PAD + OVS_LP_HEAD - 2.0f;
    } else {
        t = OVS_LP_PAD + (head ? OVS_LP_HEAD + OVS_LP_LH * (float)(i - 1) : OVS_LP_LH * (float)i);
        b = t + OVS_LP_LH;
    }
    if (bot) *bot = (int)(b * f + 0.5f);
    return (int)(t * f + 0.5f);
}

unsigned ovs_lp_height(float f, int head, int nrow)
{
    float u;
    if (nrow < 1) nrow = 1;
    u = 2.0f * OVS_LP_PAD + (head ? OVS_LP_HEAD + OVS_LP_LH * (float)(nrow - 1) : OVS_LP_LH * (float)nrow);
    return (unsigned)ovs_px(f, u);
}

void ovs_lp_finish(unsigned *pm, const unsigned *bits, unsigned w, unsigned h, float f, int head, const unsigned *col,
                   int nrow)
{
    unsigned x, y, lw, bar, rule, ry, rx0, rx1;
    int ri = 0, bot = 0;
    if (!pm || !bits || !w || !h || nrow < 1) return;
    lw = (unsigned)ovs_px(f, OVS_LP_BAR);
    if (lw > w) lw = w;
    bar = ovs_pm_px(OVS_RGB_ACCENT, OVS_LP_BAR_ALPHA);
    ovs_lp_row_top(f, head, 0, &bot);
    for (y = 0; y < h; y++) {
        while (ri + 1 < nrow && (int)y >= bot) { /* naechste Zeile, sobald die Unterkante der aktuellen erreicht ist */
            ri++;
            ovs_lp_row_top(f, head, ri, &bot);
        }
        for (x = 0; x < lw; x++) pm[y * w + x] = bar;
        ovs_pm_span(pm + y * w + lw, bits + y * w + lw, (int)(w - lw), col[ri], OVS_RGB_PANEL, OVS_LP_ALPHA);
    }
    if (!head) return;
    /* duenne Trennlinie unter der Kopfzeile (Akzent, gedimmt) */
    ry = (unsigned)ovs_px(f, OVS_LP_PAD + OVS_LP_HEAD - 1.0f);
    rx0 = lw + (unsigned)ovs_px(f, OVS_LP_INSET);
    rx1 = w > (unsigned)ovs_px(f, OVS_LP_PAD) ? w - (unsigned)ovs_px(f, OVS_LP_PAD) : 0;
    rule = ovs_pm_px(OVS_RGB_ACCENT, 0.5f);
    rule = (rule & 0x00FFFFFFu) | 0xB0000000u; /* Farbe halbe Staerke, Deckung 0,69 (wie W460) */
    if (ry < h)
        for (x = rx0; x < rx1; x++) pm[ry * w + x] = rule;
}
