/* launcher_layout.c - W281 LAUNCHER-SIZE: Layout-Varianten 0/a/b (Entwurf).
 * Reine Rechnungen ohne Fenster: Groessen-Tabellen, Einpassen in die
 * Arbeitsflaeche, Abbildung des 960x640-Entwurfsrasters auf die Flaeche von
 * Variante b, Spielen-Seite von b, Selbsttest. Quelltext ASCII. */
#include "launcher_layout.h"

#include <stdio.h>
#include <string.h>

/* --- Tabellen (zentral, s. Kopf) ------------------------------------ */
/* a: Geometrie/Schrift je UI-Groesse. Normal: Fenster 1152x768 bei 100 %,
 * Schrift +12 %, Abstaende +20 % (Geometrie waechst staerker als die
 * Schrift: mehr Innenabstand und Zeilenhoehe). Kompakt = heute. */
static const int kGeoA[LLAY_SZ_N] = { 100, 120, 140 };
static const int kFontA[LLAY_SZ_N] = { 100, 112, 128 };
/* b: Flaeche 1280x800, Schrift leicht groesser, Geometrie ~100 %. */
static const int kGeoB[LLAY_SZ_N] = { 92, 100, 110 };
static const int kFontB[LLAY_SZ_N] = { 96, 106, 118 };
#define B_TW 1280
#define B_TH 800
#define B_NAV 236
#define B_MIN_W 1040
#define B_MIN_H 660

int llay_parse(const char *v)
{
    if (!v || !*v) {
        return -1;
    }
    if (strcmp(v, "0") == 0 || strcmp(v, "heute") == 0 || strcmp(v, "today") == 0) {
        return LLAY_0;
    }
    if ((v[0] == 'a' || v[0] == 'A') && !v[1]) {
        return LLAY_A;
    }
    if ((v[0] == 'b' || v[0] == 'B') && !v[1]) {
        return LLAY_B;
    }
    return -1;
}

const char *llay_name(int id)
{
    return id == LLAY_A ? "a" : id == LLAY_B ? "b" : "0";
}

int llay_size_parse(const char *v)
{
    if (!v || !*v) {
        return -1;
    }
    if (strcmp(v, "compact") == 0 || strcmp(v, "0") == 0) {
        return LLAY_SZ_COMPACT;
    }
    if (strcmp(v, "normal") == 0 || strcmp(v, "1") == 0) {
        return LLAY_SZ_NORMAL;
    }
    if (strcmp(v, "large") == 0 || strcmp(v, "2") == 0) {
        return LLAY_SZ_LARGE;
    }
    return -1;
}

const char *llay_size_name(int size)
{
    return size == LLAY_SZ_COMPACT ? "compact" : size == LLAY_SZ_LARGE ? "large" : "normal";
}

void llay_init(llay_t *L, int id, int size)
{
    memset(L, 0, sizeof *L);
    if (id < LLAY_0 || id >= LLAY_N) {
        id = LLAY_0;
    }
    if (size < 0 || size >= LLAY_SZ_N) {
        size = LLAY_SZ_NORMAL;
    }
    L->id = id;
    L->size = size;
    L->tw = LLAY_DW;
    L->th = LLAY_DH;
    L->nav = LLAY_NAV;
    L->geo_pct = 100;
    L->font_pct = 100;
    if (id == LLAY_A) {
        L->geo_pct = kGeoA[size];
        L->font_pct = kFontA[size];
    } else if (id == LLAY_B) {
        L->geo_pct = kGeoB[size];
        L->font_pct = kFontB[size];
        L->tw = B_TW;
        L->th = B_TH;
        L->nav = B_NAV;
        L->resizable = 1;
        L->min_w = B_MIN_W;
        L->min_h = B_MIN_H;
    }
}

static int rdiv(int a, int b) /* gerundet, a,b > 0 (wie MulDiv) */
{
    return (a + b / 2) / b;
}

static int geo_dpi(const llay_t *L, int mon_dpi)
{
    return rdiv(mon_dpi * L->geo_pct, 100);
}

void llay_window_px(const llay_t *L, int mon_dpi, int *w, int *h)
{
    int d = geo_dpi(L, mon_dpi);
    *w = rdiv(L->tw * d, 96);
    *h = rdiv(L->th * d, 96);
}

int llay_fit(llay_t *L, int mon_dpi, int work_w, int work_h)
{
    int w, h, shrunk = 0, g0 = L->geo_pct, f0 = L->font_pct;
    if (L->id == LLAY_0 || mon_dpi < 48 || work_w <= 0 || work_h <= 0) {
        return 0;
    }
    if (L->resizable) { /* b: erst die Flaeche bis zur Mindestgroesse */
        int d = geo_dpi(L, mon_dpi);
        int aw = work_w * 96 / d, ah = work_h * 96 / d;
        if (L->tw > aw) {
            L->tw = aw < L->min_w ? L->min_w : aw;
            shrunk = 1;
        }
        if (L->th > ah) {
            L->th = ah < L->min_h ? L->min_h : ah;
            shrunk = 1;
        }
    }
    llay_window_px(L, mon_dpi, &w, &h);
    while ((w > work_w || h > work_h) && L->geo_pct > LLAY_MIN_PCT) {
        L->geo_pct--;
        shrunk = 1;
        llay_window_px(L, mon_dpi, &w, &h);
    }
    if (L->geo_pct != g0) { /* Schrift im selben Verhaeltnis */
        L->font_pct = rdiv(f0 * L->geo_pct, g0);
        if (L->font_pct < LLAY_MIN_PCT) {
            L->font_pct = LLAY_MIN_PCT;
        }
    }
    return shrunk;
}

void llay_resize_px(llay_t *L, int mon_dpi, int w_px, int h_px)
{
    int d = geo_dpi(L, mon_dpi);
    if (!L->resizable || d <= 0) {
        return;
    }
    L->tw = w_px * 96 / d;
    L->th = h_px * 96 / d;
    if (L->tw < L->min_w) {
        L->tw = L->min_w;
    }
    if (L->th < L->min_h) {
        L->th = L->min_h;
    }
}

/* --- Abbildung -------------------------------------------------------- */
int llay_mapx(const llay_t *L, int x, int y)
{
    if (L->id != LLAY_B) {
        return x;
    }
    if (y < LLAY_TITLE) { /* Titelband: rechte Haelfte am rechten Rand */
        return x >= LLAY_DW / 2 ? L->tw - (LLAY_DW - x) : x;
    }
    if (y >= LLAY_DH - LLAY_BOT) { /* Fussband */
        if (x >= LLAY_DW / 2) {
            return L->tw - (LLAY_DW - x);
        }
        return x < LLAY_NAV ? x * L->nav / LLAY_NAV : L->nav + (x - LLAY_NAV);
    }
    if (x <= LLAY_NAV) {
        return x * L->nav / LLAY_NAV;
    }
    return L->nav + (x - LLAY_NAV) * (L->tw - L->nav) / (LLAY_DW - LLAY_NAV);
}

int llay_mapy(const llay_t *L, int y)
{
    int ch = LLAY_DH - LLAY_TITLE - LLAY_BOT;
    if (L->id != LLAY_B) {
        return y;
    }
    if (y <= LLAY_TITLE) {
        return y;
    }
    if (y >= LLAY_DH - LLAY_BOT) {
        return L->th - (LLAY_DH - y);
    }
    return LLAY_TITLE + (y - LLAY_TITLE) * (L->th - LLAY_TITLE - LLAY_BOT) / ch;
}

void llay_map_rect(const llay_t *L, int x, int y, int w, int h, int sw, int sh,
                   int out[4])
{
    int x0 = llay_mapx(L, x, y), y0 = llay_mapy(L, y);
    int x1 = sw ? llay_mapx(L, x + w, y) : x0 + w;
    int y1 = sh ? llay_mapy(L, y + h) : y0 + h;
    if (sw && x1 < x0 + w && w > 0 && (y < LLAY_TITLE || y >= LLAY_DH - LLAY_BOT)) {
        x1 = x0 + w; /* Bandgrenze: nie schmaler als entworfen */
    }
    out[0] = x0;
    out[1] = y0;
    out[2] = x1 - x0;
    out[3] = y1 - y0;
}

void llay_map_paint(const llay_t *L, int x, int y, int w, int h, int out[4])
{
    /* Punkte/Symbole (<= 16) bleiben, Banner und einzeilige Texte behalten
     * die Hoehe (< 48), Karten/Vorschauen/Absaetze strecken mit. */
    int small = w <= 16 && h <= 16;
    llay_map_rect(L, x, y, w, h, !small, !small && h >= 48, out);
}

/* --- b: Spielen-Seite (Entwurfsraster) ------------------------------- */
#define B_CX0 (LLAY_NAV + 28)
#define B_CW (LLAY_DW - B_CX0 - 28)
#define B_CY0 (LLAY_TITLE + 16) /* W397 */
#define B_GRID_Y (B_CY0 + 230)
#define B_GAP 12
#define B_CARD_W ((B_CW - 2 * B_GAP) / 3)
#define B_CARD_H ((LLAY_DH - LLAY_BOT - B_GRID_Y - B_GAP - 2) / 2)

static void put(int out[4], int x, int y, int w, int h)
{
    out[0] = x;
    out[1] = y;
    out[2] = w;
    out[3] = h;
}

void llay_b_card(int i, int out[4])
{
    int col = i % 3, row = i / 3;
    put(out, B_CX0 + col * (B_CARD_W + B_GAP), B_GRID_Y + row * (B_CARD_H + B_GAP),
        B_CARD_W, B_CARD_H);
}

void llay_b_card_go(int i, int out[4])
{
    int c[4];
    llay_b_card(i, c);
    put(out, c[0] + c[2] - 18 - 104, c[1] + c[3] - 14 - 30, 104, 30);
}

void llay_b_start_area(int out[4])
{
    put(out, B_CX0, B_CY0 + 76, B_CW, 138);
}

void llay_b_start_big(int out[4])
{
    put(out, B_CX0 + 18, B_CY0 + 90, 330, 58);
}

void llay_b_start_vr(int out[4])
{
    put(out, B_CX0 + 18, B_CY0 + 154, 330, 36);
}

void llay_b_start_hint(int out[4])
{
    put(out, B_CX0 + 18, B_CY0 + 194, 330, 18);
}

int llay_overlap(const int a[4], const int b[4])
{
    return a[0] < b[0] + b[2] && b[0] < a[0] + a[2] && a[1] < b[1] + b[3] &&
           b[1] < a[1] + a[3] && a[2] > 0 && a[3] > 0 && b[2] > 0 && b[3] > 0;
}

/* --- Selbsttest -------------------------------------------------------- */
void llay_selftest(llay_rep_fn rep)
{
    char d[256];
    int ok, i, j;
    /* Parser */
    ok = llay_parse("0") == LLAY_0 && llay_parse("a") == LLAY_A && llay_parse("B") == LLAY_B &&
         llay_parse("c") == -1 && llay_parse("") == -1 && llay_parse(NULL) == -1 &&
         llay_size_parse("compact") == LLAY_SZ_COMPACT && llay_size_parse("large") == LLAY_SZ_LARGE &&
         llay_size_parse("x") == -1 && strcmp(llay_name(LLAY_B), "b") == 0;
    rep("W281 layout: Parser 0|a|b, compact|normal|large", ok, "");
    /* Layout 0: unveraendert (960x640, 100 %, Identitaet, kein Einpassen) */
    {
        llay_t L;
        int r[4], w, h;
        llay_init(&L, LLAY_0, LLAY_SZ_LARGE);
        llay_map_rect(&L, 300, 200, 120, 30, 1, 1, r);
        llay_window_px(&L, 96, &w, &h);
        ok = L.geo_pct == 100 && L.font_pct == 100 && w == LLAY_DW && h == LLAY_DH && r[0] == 300 &&
             r[1] == 200 && r[2] == 120 && r[3] == 30 && llay_fit(&L, 144, 1366, 728) == 0 &&
             L.geo_pct == 100;
        rep("W281 layout: 0 bleibt 960x656 ohne Abbildung/Einpassen", ok, "");
    }
    /* Einpassen: kleine Arbeitsflaechen je Variante und Groesse */
    {
        static const int kWork[][3] = {
            { 1366, 728, 96 },   /* 1366x768, 100 %, Taskleiste */
            { 1920, 1032, 120 }, /* 1920x1080, 125 % */
            { 1920, 1032, 144 }, /* 1920x1080, 150 % */
            { 1280, 680, 96 },   /* 1280x720 */
            { 1366, 728, 120 },  /* Laptop 125 % */
            { 2560, 1400, 96 },  /* gross: nichts verkleinern */
            { 1024, 600, 96 },   /* sehr klein: Mindest-Skalierung */
        };
        int bad = 0;
        d[0] = '\0';
        for (i = 0; i < (int)(sizeof kWork / sizeof kWork[0]); i++) {
            int id;
            for (id = LLAY_A; id <= LLAY_B; id++) {
                int sz;
                for (sz = 0; sz < LLAY_SZ_N; sz++) {
                    llay_t L;
                    int w, h;
                    llay_init(&L, id, sz);
                    llay_fit(&L, kWork[i][2], kWork[i][0], kWork[i][1]);
                    llay_window_px(&L, kWork[i][2], &w, &h);
                    if ((w > kWork[i][0] || h > kWork[i][1]) && L.geo_pct > LLAY_MIN_PCT) {
                        bad++;
                        _snprintf_s(d, sizeof d, _TRUNCATE, "%s/%s %dx%d@%d -> %dx%d",
                                    llay_name(id), llay_size_name(sz), kWork[i][0],
                                    kWork[i][1], kWork[i][2], w, h);
                    }
                    if (L.font_pct < LLAY_MIN_PCT || L.tw < LLAY_DW || L.th < LLAY_DH) {
                        bad++;
                    }
                }
            }
        }
        {
            llay_t L;
            int w, h, r1, r2, r3;
            llay_init(&L, LLAY_A, LLAY_SZ_NORMAL); /* 1152x768 -> 1366x728: verkleinert */
            r1 = llay_fit(&L, 96, 1366, 728);
            llay_window_px(&L, 96, &w, &h);
            r1 = r1 == 1 && h <= 728 && L.geo_pct < 120 && L.font_pct < 112;
            llay_init(&L, LLAY_A, LLAY_SZ_NORMAL); /* gross: unveraendert */
            r2 = llay_fit(&L, 96, 2560, 1400) == 0 && L.geo_pct == 120;
            llay_init(&L, LLAY_B, LLAY_SZ_NORMAL); /* b: erst Flaeche, Skalierung bleibt */
            llay_fit(&L, 96, 1366, 728);
            r3 = L.geo_pct == 100 && L.tw == 1280 && L.th == 728;
            if (!(r1 && r2 && r3)) {
                bad++;
                _snprintf_s(d, sizeof d, _TRUNCATE, "a1366=%d a2560=%d b1366=%d", r1, r2, r3);
            }
        }
        rep("W281 layout: Fenster passt auf kleine Arbeitsflaechen (a/b, 3 Groessen, 7 Flaechen)",
            bad == 0, d);
    }
    /* Abbildung b: Raender, Anker, Monotonie */
    {
        llay_t L;
        int r[4], prev = -1, mono = 1;
        llay_init(&L, LLAY_B, LLAY_SZ_NORMAL);
        for (i = 0; i <= LLAY_DW; i += 8) {
            int x = llay_mapx(&L, i, 300);
            if (x < prev) {
                mono = 0;
            }
            prev = x;
        }
        llay_map_rect(&L, LLAY_DW - 46, 0, 46, 40, 1, 0, r); /* Schliessen-Knopf */
        ok = mono && r[0] + r[2] == L.tw && r[2] == 46 &&
             llay_mapx(&L, LLAY_DW, 300) == L.tw &&
             llay_mapy(&L, LLAY_DH - LLAY_BOT) == L.th - LLAY_BOT && llay_mapy(&L, 10) == 10 &&
             llay_mapx(&L, LLAY_NAV, 300) == L.nav;
        llay_map_paint(&L, 300, 300, 10, 10, r); /* Punkt behaelt Groesse */
        ok = ok && r[2] == 10 && r[3] == 10;
        llay_map_paint(&L, 300, 300, 200, 40, r); /* Banner: Hoehe bleibt */
        ok = ok && r[3] == 40 && r[2] > 200;
        llay_map_paint(&L, 300, 100, 300, 200, r); /* Karte streckt */
        ok = ok && r[3] > 200 && r[2] > 300;
        rep("W281 layout: Abbildung b (monoton, Titel/Fuss am Rand, Punkte/Banner/Karten)", ok, "");
    }
    /* b-Spielen: Karten/Knoepfe ohne Ueberlappung, innerhalb des Inhalts */
    {
        int c[LLAY_BCARD_N][4], g[LLAY_BCARD_N][4], sa[4], sb[4], sv[4], sh[4];
        int bad = 0;
        llay_b_start_area(sa);
        llay_b_start_big(sb);
        llay_b_start_vr(sv);
        llay_b_start_hint(sh);
        for (i = 0; i < LLAY_BCARD_N; i++) {
            llay_b_card(i, c[i]);
            llay_b_card_go(i, g[i]);
            if (llay_overlap(c[i], sa) || c[i][1] + c[i][3] > LLAY_DH - LLAY_BOT ||
                c[i][0] + c[i][2] > LLAY_DW - 28) {
                bad++;
            }
            if (!(g[i][0] >= c[i][0] && g[i][1] >= c[i][1] &&
                  g[i][0] + g[i][2] <= c[i][0] + c[i][2] && g[i][1] + g[i][3] <= c[i][1] + c[i][3])) {
                bad++;
            }
            for (j = 0; j < i; j++) {
                if (llay_overlap(c[i], c[j])) {
                    bad++;
                }
            }
        }
        if (llay_overlap(sb, sv) || llay_overlap(sv, sh) || llay_overlap(sb, sh) ||
            sh[1] + sh[3] > sa[1] + sa[3]) {
            bad++;
        }
        _snprintf_s(d, sizeof d, _TRUNCATE, "%d Konflikte", bad);
        rep("W281 layout: b-Spielen Karten/Knoepfe ohne Ueberlappung", bad == 0, d);
    }
}
