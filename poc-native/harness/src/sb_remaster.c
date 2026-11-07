/* sb_remaster.c - REMASTER-Geruest (W103 REM-FRAME).
 *
 * 1. Inhalts-Hash je Textur (64 Bit, FNV-1a): Kopf "SBT1" + Xbox-Formatcode
 *    + Breite + Hoehe + Palettenlaenge (0, das Spiel nutzt keine P8) +
 *    Mip-Stufe 0 NACH dem Deswizzle (0x06) bzw. dicht gepackt (DXT-Bloecke,
 *    lineare Zeilen ohne Pitch-Polster). Unabhaengig von xptr/Data-Adresse,
 *    gleich ueber Laeufe und Missionen.
 * 2. tex_dump=1 (DIAG): jede neue Textur einmal als RGBA-PNG nach
 *    <remaster_dir>/dump/<hash>.png + Eintrag in dump/index.json (Format s.
 *    BERICHT_M2 W103). Bestehendes index.json wird beim Start eingelesen
 *    (Zaehler laufen ueber Laeufe weiter, je Hash genau ein Eintrag).
 * 3. remaster=1 + hd_textures=1: beim Upload gleicher Hash ->
 *    <remaster_dir>/textures/x<hd_scale>/<hash>.dds (sonst textures/<hash>.dds)
 *    laden und statt des Originals binden. Fehlt die Datei -> Original.
 *    W114: Freigaben je Variante aus <remaster_dir>/selection.json (Board):
 *    Stufe 4: 4x nicht OK -> x2 (wenn 2x nicht "nicht OK"), sonst Original;
 *    Stufe 2: 2x nicht OK -> Original. Unbewertet = Auto-Pack. hd_scale=0
 *    (alt, W111 "Auswahl") = Stufe 4 mit Freigaben.
 *    hd_manual=1 (W113): textures/manual/<hash>.dds (Text-Rekonstruktion,
 *    remaster_text.py; bei Stufe 2 zuerst manual/x2/) hat Vorrang vor x2/x4
 *    und vor den Freigaben - ausser 2x und 4x sind beide "nicht OK".
 *    Gleiches Seitenverhaeltnis Pflicht (sonst abgelehnt). Mips aus der
 *    Datei, sonst erzeugt (GenerateMips; BC1-3 dafuer entpackt). Per CPU
 *    beschriebene Texturen (Schreib-LockRect: Film, Atlas W98, Textebene)
 *    werden nie ersetzt. Cache mit Grenze (hd_cache_mb), VRAM-Zeile im Log.
 * RT-Texturen laufen nie durch tex_upload (sb_rtt_lookup vorher) und sind
 * damit ausgenommen.
 */
#define COBJMACROS
#include "sb_remaster.h"
#include "sb_pack.h" /* W140 */
#include "hlog.h"
#include "hcfg.h"
#include "render_int.h"
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

typedef unsigned long long u64;

static ID3D11Device *g_rdev;
static int g_dump_on, g_hd_on, g_inited;
static int g_rcs_ok; /* W118: g_rcs initialisiert (Init oder erstes Live-Einschalten) */
static char g_rdir[512];
static CRITICAL_SECTION g_rcs;

/* ------------------------------------------------------------------ */
/* Hash                                                                */
/* ------------------------------------------------------------------ */
#define FNV64_INIT 0xcbf29ce484222325ull
#define FNV64_PRIME 0x100000001b3ull
static u64 fnv64(u64 h, const void *p, size_t n)
{
    const unsigned char *b = (const unsigned char *)p;
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= b[i];
        h *= FNV64_PRIME;
    }
    return h;
}

static unsigned fmt_code(unsigned fmt) { return (fmt & 0xFF00u) >> 8; }
static unsigned lin_bpp(unsigned code) { return code == 0x1Cu ? 2u : 4u; }

/* Bytes je Zeile (dicht) und Zeilenzahl der Mip-0-Daten. */
static void mip0_rows(int kind, unsigned code, unsigned w, unsigned h,
                      unsigned *rowbytes, unsigned *rows)
{
    if (kind == 1) {
        unsigned blk = (code == 0x0Cu) ? 8u : 16u;
        *rowbytes = (w / 4u) * blk; /* wie tex_upload */
        *rows = h / 4u;
    } else if (kind == 3) {
        *rowbytes = w * 4u;
        *rows = h;
    } else {
        *rowbytes = w * lin_bpp(code);
        *rows = h;
    }
}

static u64 tex_content_hash(int kind, unsigned code, unsigned w, unsigned h,
                            const void *pix, unsigned pitch, int *ok)
{
    unsigned hdr[5], rb, rows, y;
    u64 hv = FNV64_INIT;
    *ok = 0;
    hdr[0] = 0x31544253u; /* "SBT1" */
    hdr[1] = code;
    hdr[2] = w;
    hdr[3] = h;
    hdr[4] = 0; /* Palettenlaenge (P8 nicht im Spiel) */
    hv = fnv64(hv, hdr, sizeof hdr);
    mip0_rows(kind, code, w, h, &rb, &rows);
    if (!pix || !rb || !rows) {
        return 0;
    }
    __try {
        for (y = 0; y < rows; y++) {
            hv = fnv64(hv, (const unsigned char *)pix + (size_t)y * pitch, rb);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    *ok = 1;
    return hv ? hv : 1ull;
}

/* ------------------------------------------------------------------ */
/* BCn / Format -> RGBA8                                               */
/* ------------------------------------------------------------------ */
static void c565(unsigned c, unsigned char o[4])
{
    unsigned r = (c >> 11) & 31u, g = (c >> 5) & 63u, b = c & 31u;
    o[0] = (unsigned char)((r << 3) | (r >> 2));
    o[1] = (unsigned char)((g << 2) | (g >> 4));
    o[2] = (unsigned char)((b << 3) | (b >> 2));
    o[3] = 255;
}
/* Farbblock (8 B) -> 16 RGBA. bc1 = 1: 3-Farb-Modus mit Transparenz. */
static void bc_color(const unsigned char *b, unsigned char out[16][4], int bc1)
{
    unsigned c0 = b[0] | (b[1] << 8), c1 = b[2] | (b[3] << 8), i, k;
    unsigned idx = b[4] | (b[5] << 8) | (b[6] << 16) | ((unsigned)b[7] << 24);
    unsigned char pal[4][4];
    c565(c0, pal[0]);
    c565(c1, pal[1]);
    if (!bc1 || c0 > c1) {
        for (k = 0; k < 3; k++) {
            pal[2][k] = (unsigned char)((2u * pal[0][k] + pal[1][k] + 1u) / 3u);
            pal[3][k] = (unsigned char)((pal[0][k] + 2u * pal[1][k] + 1u) / 3u);
        }
        pal[2][3] = pal[3][3] = 255;
    } else {
        for (k = 0; k < 3; k++) {
            pal[2][k] = (unsigned char)((pal[0][k] + pal[1][k]) / 2u);
            pal[3][k] = 0;
        }
        pal[2][3] = 255;
        pal[3][3] = 0;
    }
    for (i = 0; i < 16; i++) {
        memcpy(out[i], pal[(idx >> (2 * i)) & 3u], 4);
    }
}
static void bc3_alpha(const unsigned char *b, unsigned char out[16][4])
{
    unsigned a[8], i;
    u64 bits = 0;
    a[0] = b[0];
    a[1] = b[1];
    if (a[0] > a[1]) {
        for (i = 1; i < 7; i++) { a[i + 1] = ((7u - i) * a[0] + i * a[1] + 3u) / 7u; }
    } else {
        for (i = 1; i < 5; i++) { a[i + 1] = ((5u - i) * a[0] + i * a[1] + 2u) / 5u; }
        a[6] = 0;
        a[7] = 255;
    }
    for (i = 0; i < 6; i++) { bits |= (u64)b[2 + i] << (8 * i); }
    for (i = 0; i < 16; i++) { out[i][3] = (unsigned char)a[(bits >> (3 * i)) & 7u]; }
}
/* bc: 1/2/3. src = dichte Bloecke (bw*bh), dst = w*h*4 RGBA. */
static void bc_decode(int bc, const unsigned char *src, unsigned w, unsigned h,
                      unsigned char *dst)
{
    unsigned bw = (w + 3u) / 4u, bh = (h + 3u) / 4u, bx, by, i, bsz = (bc == 1) ? 8u : 16u;
    unsigned char px[16][4];
    for (by = 0; by < bh; by++) {
        for (bx = 0; bx < bw; bx++) {
            const unsigned char *b = src + ((size_t)by * bw + bx) * bsz;
            if (bc == 1) {
                bc_color(b, px, 1);
            } else {
                bc_color(b + 8, px, 0);
                if (bc == 2) {
                    for (i = 0; i < 16; i++) {
                        unsigned v = (b[i / 2] >> ((i & 1u) * 4u)) & 15u;
                        px[i][3] = (unsigned char)(v * 17u);
                    }
                } else {
                    bc3_alpha(b, px);
                }
            }
            for (i = 0; i < 16; i++) {
                unsigned x = bx * 4u + (i & 3u), y = by * 4u + (i >> 2);
                if (x < w && y < h) { memcpy(dst + ((size_t)y * w + x) * 4u, px[i], 4); }
            }
        }
    }
}

/* Mip 0 (Spielspeicher, dicht/pitch) -> RGBA8 (so wie der Renderer sampelt). */
static int decode_rgba(int kind, unsigned code, unsigned w, unsigned h,
                       const unsigned char *pix, unsigned pitch, unsigned char *dst)
{
    unsigned x, y;
    __try {
        if (kind == 1) {
            unsigned bw = w / 4u, bh = h / 4u, blk = (code == 0x0Cu) ? 8u : 16u;
            unsigned char *tight;
            if (!bw || !bh) { return 0; }
            tight = (unsigned char *)malloc((size_t)bw * bh * blk);
            if (!tight) { return 0; }
            for (y = 0; y < bh; y++) { memcpy(tight + (size_t)y * bw * blk, pix + (size_t)y * pitch, bw * blk); }
            bc_decode(code == 0x0Cu ? 1 : code == 0x0Eu ? 2 : 3, tight, w, h, dst);
            free(tight);
            return 1;
        }
        for (y = 0; y < h; y++) {
            const unsigned char *s = pix + (size_t)y * pitch;
            unsigned char *d = dst + (size_t)y * w * 4u;
            for (x = 0; x < w; x++) {
                if (code == 0x1Cu) { /* DXGI B5G5R5A1 */
                    unsigned v = s[2 * x] | (s[2 * x + 1] << 8);
                    unsigned r = (v >> 10) & 31u, g = (v >> 5) & 31u, b = v & 31u;
                    d[4 * x + 0] = (unsigned char)((r << 3) | (r >> 2));
                    d[4 * x + 1] = (unsigned char)((g << 3) | (g >> 2));
                    d[4 * x + 2] = (unsigned char)((b << 3) | (b >> 2));
                    d[4 * x + 3] = (v & 0x8000u) ? 255 : 0;
                } else { /* BGRA (0x06/0x12) bzw. BGRX (0x1E) */
                    d[4 * x + 0] = s[4 * x + 2];
                    d[4 * x + 1] = s[4 * x + 1];
                    d[4 * x + 2] = s[4 * x + 0];
                    d[4 * x + 3] = (code == 0x1Eu) ? 255 : s[4 * x + 3];
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* PNG (zlib fixed Huffman + LZ77, ohne externe Bibliothek)            */
/* ------------------------------------------------------------------ */
typedef struct { unsigned char *p; size_t n, cap; unsigned bitbuf, bitn; int err; } obuf_t;
static void ob_byte(obuf_t *o, unsigned char c)
{
    if (o->err) { return; }
    if (o->n == o->cap) {
        size_t nc = o->cap ? o->cap * 2u : 65536u;
        unsigned char *q = (unsigned char *)realloc(o->p, nc);
        if (!q) { o->err = 1; return; }
        o->p = q;
        o->cap = nc;
    }
    o->p[o->n++] = c;
}
static void ob_bits(obuf_t *o, unsigned v, unsigned n)
{
    o->bitbuf |= v << o->bitn;
    o->bitn += n;
    while (o->bitn >= 8u) {
        ob_byte(o, (unsigned char)(o->bitbuf & 0xFFu));
        o->bitbuf >>= 8;
        o->bitn -= 8u;
    }
}
static unsigned rev_bits(unsigned v, unsigned n)
{
    unsigned r = 0, i;
    for (i = 0; i < n; i++) { r = (r << 1) | ((v >> i) & 1u); }
    return r;
}
static void huff_sym(obuf_t *o, unsigned s)
{
    if (s < 144u) { ob_bits(o, rev_bits(0x30u + s, 8), 8); }
    else if (s < 256u) { ob_bits(o, rev_bits(0x190u + s - 144u, 9), 9); }
    else if (s < 280u) { ob_bits(o, rev_bits(s - 256u, 7), 7); }
    else { ob_bits(o, rev_bits(0xC0u + s - 280u, 8), 8); }
}
static const unsigned short k_lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const unsigned char k_lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const unsigned short k_dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const unsigned char k_dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
#define LZ_HBITS 15
#define LZ_WIN 32768u
static void deflate_fixed(obuf_t *o, const unsigned char *d, size_t n)
{
    int *head = (int *)malloc(sizeof(int) << LZ_HBITS);
    int *prev = (int *)malloc(sizeof(int) * LZ_WIN);
    size_t i = 0;
    if (!head || !prev) { free(head); free(prev); o->err = 1; return; }
    memset(head, 0xFF, sizeof(int) << LZ_HBITS);
    ob_bits(o, 1, 1); /* BFINAL */
    ob_bits(o, 1, 2); /* fixed Huffman */
    while (i < n) {
        unsigned best = 0, bdist = 0;
        if (i + 3u <= n) {
            unsigned hsh = ((d[i] << 16) ^ (d[i + 1] << 8) ^ d[i + 2]) * 2654435761u >> (32 - LZ_HBITS);
            int cand = head[hsh], chain = 32;
            size_t maxl = n - i < 258u ? n - i : 258u;
            while (cand >= 0 && i - (size_t)cand <= LZ_WIN - 1u && chain-- > 0) {
                const unsigned char *a = d + cand, *b = d + i;
                unsigned l = 0;
                while (l < maxl && a[l] == b[l]) { l++; }
                if (l > best) { best = l; bdist = (unsigned)(i - (size_t)cand); if (l == maxl) { break; } }
                cand = prev[cand & (LZ_WIN - 1u)];
            }
            prev[i & (LZ_WIN - 1u)] = head[hsh];
            head[hsh] = (int)i;
        }
        if (best >= 3u) {
            unsigned li = 0, di = 0, k;
            while (li < 28u && k_lbase[li + 1] <= best) { li++; }
            huff_sym(o, 257u + li);
            if (k_lext[li]) { ob_bits(o, best - k_lbase[li], k_lext[li]); }
            while (di < 29u && k_dbase[di + 1] <= bdist) { di++; }
            ob_bits(o, rev_bits(di, 5), 5);
            if (k_dext[di]) { ob_bits(o, bdist - k_dbase[di], k_dext[di]); }
            for (k = 1; k < best; k++) { /* Hash-Kette fuer uebersprungene Positionen */
                size_t j = i + k;
                if (j + 3u <= n) {
                    unsigned hj = ((d[j] << 16) ^ (d[j + 1] << 8) ^ d[j + 2]) * 2654435761u >> (32 - LZ_HBITS);
                    prev[j & (LZ_WIN - 1u)] = head[hj];
                    head[hj] = (int)j;
                }
            }
            i += best;
        } else {
            huff_sym(o, d[i]);
            i++;
        }
    }
    huff_sym(o, 256);
    if (o->bitn) { ob_bits(o, 0, 8u - o->bitn); }
    free(head);
    free(prev);
}
static unsigned crc_tab[256];
static unsigned crc32_upd(unsigned c, const unsigned char *p, size_t n)
{
    size_t i;
    if (!crc_tab[1]) {
        unsigned k, j;
        for (k = 0; k < 256u; k++) {
            unsigned v = k;
            for (j = 0; j < 8u; j++) { v = (v & 1u) ? 0xEDB88320u ^ (v >> 1) : v >> 1; }
            crc_tab[k] = v;
        }
    }
    for (i = 0; i < n; i++) { c = crc_tab[(c ^ p[i]) & 0xFFu] ^ (c >> 8); }
    return c;
}
static void be32(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v;
}
static void png_chunk(FILE *f, const char *type, const unsigned char *d, size_t n)
{
    unsigned char b[4];
    unsigned c;
    be32(b, (unsigned)n);
    fwrite(b, 1, 4, f);
    fwrite(type, 1, 4, f);
    if (n) { fwrite(d, 1, n, f); }
    c = crc32_upd(0xFFFFFFFFu, (const unsigned char *)type, 4);
    c = crc32_upd(c, d, n) ^ 0xFFFFFFFFu;
    be32(b, c);
    fwrite(b, 1, 4, f);
}
static int paeth(int a, int b, int c)
{
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return (pa <= pb && pa <= pc) ? a : (pb <= pc) ? b : c;
}
/* RGBA8 -> PNG (Farbtyp 6). 0 = ok. */
static int png_write_rgba(const char *path, const unsigned char *rgba, unsigned w, unsigned h)
{
    size_t stride = (size_t)w * 4u, rawn = (stride + 1u) * h;
    unsigned char *raw = (unsigned char *)malloc(rawn), *cand = (unsigned char *)malloc(stride * 5u);
    obuf_t o;
    unsigned y, a1 = 1, a2 = 0;
    size_t i;
    FILE *f = NULL;
    unsigned char ih[13];
    if (!raw || !cand) { free(raw); free(cand); return 1; }
    for (y = 0; y < h; y++) { /* Filter je Zeile: kleinste Summe |Wert| (Standard-Heuristik) */
        const unsigned char *cur = rgba + y * stride, *up = y ? cur - stride : NULL;
        unsigned best = 0, ft;
        unsigned long long bsum = ~0ull;
        for (ft = 0; ft < 5u; ft++) {
            unsigned char *c = cand + ft * stride;
            unsigned long long sum = 0;
            for (i = 0; i < stride; i++) {
                int a = i >= 4u ? cur[i - 4] : 0, b = up ? up[i] : 0, cc = (i >= 4u && up) ? up[i - 4] : 0, v;
                switch (ft) {
                case 0: v = cur[i]; break;
                case 1: v = cur[i] - a; break;
                case 2: v = cur[i] - b; break;
                case 3: v = cur[i] - ((a + b) >> 1); break;
                default: v = cur[i] - paeth(a, b, cc); break;
                }
                c[i] = (unsigned char)v;
                sum += (unsigned)abs((signed char)c[i]);
            }
            if (sum < bsum) { bsum = sum; best = ft; }
        }
        raw[y * (stride + 1u)] = (unsigned char)best;
        memcpy(raw + y * (stride + 1u) + 1u, cand + best * stride, stride);
    }
    free(cand);
    memset(&o, 0, sizeof o);
    ob_byte(&o, 0x78);
    ob_byte(&o, 0x01);
    deflate_fixed(&o, raw, rawn);
    for (i = 0; i < rawn; i++) { a1 = (a1 + raw[i]) % 65521u; a2 = (a2 + a1) % 65521u; }
    free(raw);
    {
        unsigned ad = (a2 << 16) | a1;
        ob_byte(&o, (unsigned char)(ad >> 24)); ob_byte(&o, (unsigned char)(ad >> 16));
        ob_byte(&o, (unsigned char)(ad >> 8)); ob_byte(&o, (unsigned char)ad);
    }
    if (o.err || fopen_s(&f, path, "wb") != 0 || !f) { free(o.p); return 1; }
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    be32(ih, w);
    be32(ih + 4, h);
    ih[8] = 8; ih[9] = 6; ih[10] = 0; ih[11] = 0; ih[12] = 0;
    png_chunk(f, "IHDR", ih, 13);
    png_chunk(f, "IDAT", o.p, o.n);
    png_chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(o.p);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Kontext (Szene/Modus/Mission) + Quelle (NtReadFile)                */
/* ------------------------------------------------------------------ */
static unsigned rd32(unsigned a)
{
    unsigned v = 0xFFFFFFFFu;
    __try { v = *(volatile unsigned *)(uintptr_t)a; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return v;
}
static unsigned rd16(unsigned a)
{
    unsigned v = 0xFFFFu;
    __try { v = *(volatile unsigned short *)(uintptr_t)a; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return v;
}
#define RD_RING 4096
typedef struct { unsigned buf, len; char path[96]; } rdrec_t;
static rdrec_t g_rd[RD_RING];
static volatile LONG g_rd_n;
static SRWLOCK g_rd_lock = SRWLOCK_INIT;
void sb_rem_note_read(unsigned buf, unsigned len, const char *path)
{
    rdrec_t *r;
    const char *m;
    if (!g_dump_on || !buf || len < 64u || !path) { return; }
    /* Spieldaten liegen je nach Pfad unter Media/ oder im ZCACHE (Z:) -
     * gespeichert werden die letzten beiden Pfadteile (Ordner/Datei). */
    {
        const char *a = strrchr(path, '/'), *b = strrchr(path, '\\');
        const char *last = (a > b) ? a : b, *q;
        m = path;
        if (last) {
            for (q = last - 1; q > path; q--) {
                if (*q == '/' || *q == '\\') { m = q + 1; break; }
            }
        }
    }
    if (strstr(path, "TDATA") || strstr(path, "UDATA")) { return; } /* Spielstaende */
    AcquireSRWLockExclusive(&g_rd_lock);
    r = &g_rd[g_rd_n % RD_RING];
    r->buf = buf;
    r->len = len;
    strncpy_s(r->path, sizeof r->path, m, _TRUNCATE);
    { char *s; for (s = r->path; *s; s++) { if (*s == '\\') { *s = '/'; } } }
    g_rd_n++;
    ReleaseSRWLockExclusive(&g_rd_lock);
}
static void src_lookup(unsigned addr, char *out, size_t cap)
{
    LONG n, i, lo;
    out[0] = '\0';
    AcquireSRWLockShared(&g_rd_lock);
    n = g_rd_n;
    lo = n > RD_RING ? n - RD_RING : 0;
    for (i = n - 1; i >= lo; i--) {
        const rdrec_t *r = &g_rd[i % RD_RING];
        if (addr >= r->buf && addr - r->buf < r->len) {
            strncpy_s(out, cap, r->path, _TRUNCATE);
            break;
        }
    }
    if (!out[0]) { /* DIAG: naechster Lese-Puffer (gedeckelt) */
        static unsigned nmiss;
        LONG bi = -1;
        unsigned bd = 0xFFFFFFFFu;
        for (i = n - 1; i >= lo; i--) {
            const rdrec_t *r = &g_rd[i % RD_RING];
            unsigned d = addr >= r->buf ? addr - r->buf : r->buf - addr;
            if (d < bd) { bd = d; bi = i; }
        }
        if (nmiss < 8u) {
            nmiss++;
            if (bi >= 0) {
                HLOG_I("REMASTER src-miss #%u: data=0x%X naechster Read buf=0x%X len=0x%X %s (Reads=%ld)", nmiss, addr,
                       g_rd[bi % RD_RING].buf, g_rd[bi % RD_RING].len, g_rd[bi % RD_RING].path, (long)n);
            } else {
                HLOG_I("REMASTER src-miss #%u: data=0x%X keine Reads", nmiss, addr);
            }
        }
    }
    ReleaseSRWLockShared(&g_rd_lock);
}

/* ------------------------------------------------------------------ */
/* Dump-Index                                                          */
/* ------------------------------------------------------------------ */
#define DE_MAX 32768
#define DE_HN 65536
typedef struct {
    u64 hash;
    unsigned w, h, code, mips;
    int alpha;          /* 0 = keins (alle 255), 1 = 1 Bit (0/255), 2 = voll */
    int dark;           /* Anteil fast schwarzer Texel in % (Luma < 24, nur sichtbare), -1 = unbekannt */
    unsigned scene, mode, mno, mloop, frame; /* erster Kontext */
    unsigned uses, uploads, runs, d2, d3;   /* Zaehler (ueber Laeufe) */
    int cpu, png, seen_run, dirty_d;
    char src[96];
} dent_t;
static dent_t *g_de;
static int g_nde;
static int *g_deh; /* Hash-Index (-1 = leer) */
static int g_idx_dirty, g_nde_new, g_npng, g_npng_fail, g_nskip_cpu;
static unsigned g_idx_last_flush;

static int de_find(u64 hv, int add)
{
    unsigned s = (unsigned)(hv ^ (hv >> 32)) & (DE_HN - 1u);
    while (g_deh[s] >= 0) {
        if (g_de[g_deh[s]].hash == hv) { return g_deh[s]; }
        s = (s + 1u) & (DE_HN - 1u);
    }
    if (!add || g_nde >= DE_MAX) { return -1; }
    memset(&g_de[g_nde], 0, sizeof g_de[g_nde]);
    g_de[g_nde].hash = hv;
    g_deh[s] = g_nde;
    return g_nde++;
}

/* xptr -> Hash/Eintrag, CPU-Flag (offene Adressierung, Schluessel bleibt) */
#define XM_N 32768
typedef struct { unsigned xptr; int de; int cpu; int cpu_dumped; u64 hash; } xmap_t;
static xmap_t *g_xm;
static xmap_t *xm_get(unsigned xptr, int add)
{
    unsigned s = ((xptr >> 4) * 2654435761u) & (XM_N - 1u), n = 0;
    if (!g_xm || !xptr) { return NULL; }
    while (g_xm[s].xptr && g_xm[s].xptr != xptr) {
        s = (s + 1u) & (XM_N - 1u);
        if (++n >= XM_N) { return NULL; }
    }
    if (!g_xm[s].xptr) {
        if (!add) { return NULL; }
        g_xm[s].xptr = xptr;
        g_xm[s].de = -1;
    }
    return &g_xm[s];
}

static const char *fmt_name(unsigned code)
{
    switch (code) {
    case 0x0C: return "DXT1";
    case 0x0E: return "DXT3";
    case 0x0F: return "DXT5";
    case 0x06: return "A8R8G8B8";
    case 0x12: return "LIN_A8R8G8B8";
    case 0x1E: return "LIN_X8R8G8B8";
    case 0x1C: return "LIN_X1R5G5B5";
    default: return "?";
    }
}

/* Kategorie-Vorschlag (Heuristik, s. BERICHT W103). */
static const char *de_category(const dent_t *e, const char **why)
{
    char s[96]; /* Quelle klein geschrieben (Media/ und ZCACHE unterscheiden sich in der Schreibung) */
    size_t k;
    int mission = (e->scene == 0x2Cu && e->mloop == 1u);
    for (k = 0; k + 1 < sizeof s && e->src[k]; k++) { s[k] = (char)tolower((unsigned char)e->src[k]); }
    s[k] = 0;
    if (e->cpu) { *why = "cpu"; return "cpu"; }
    if (strstr(s, "fonts/")) { *why = "src"; return "font_hud"; }
    if (strstr(s, "cockpit/")) { *why = "src"; return "cockpit"; }
    if (strstr(s, "advance")) { *why = "src"; return (e->alpha && e->d2 > e->d3) ? "font_hud" : "menu"; }
    if (strstr(s, "title/")) { *why = "src"; return "menu"; }
    if (strstr(s, "dvdmovie") || strstr(s, "texmovie")) { *why = "src"; return "cockpit"; } /* Monitor-Bildfolgen (keine Bink-Filme) */
    if (strstr(s, "effdata/")) { *why = "src"; return "effects"; }
    if (strstr(s, "model.bin")) { *why = "src"; return "vehicles"; }
    if (strstr(s, "texobj")) { *why = "src"; return "buildings"; }
    if (strstr(s, "bin/tex") && !strstr(s, "texmovie")) { *why = "src"; return "ground"; }
    if (!mission && e->d3 == 0u) { *why = "scene"; return "menu"; }
    if (e->dark >= 70 && e->alpha == 0 && e->w >= 256u) { *why = "dark"; return "cockpit"; } /* Monitor-/OS-Textbilder */
    if (e->d2 > e->d3) {
        *why = "draw2d";
        if (e->alpha && (e->w <= 256u || e->h <= 256u)) { return "font_hud"; }
        return mission ? "cockpit" : "menu";
    }
    if (e->d3) {
        *why = "draw3d";
        if (e->alpha == 2 && e->w <= 128u && e->h <= 128u) { return "effects"; }
        if (e->alpha == 0 && e->w == e->h && e->code == 0x0Cu && e->dark >= 0 && e->dark < 30) { return "ground"; }
        return "world";
    }
    *why = "none";
    return "unknown";
}

static void idx_path(char *out, size_t cap, const char *name)
{
    _snprintf_s(out, cap, _TRUNCATE, "%s/dump/%s", g_rdir, name);
}

static int json_u(const char *l, const char *key, unsigned *v)
{
    char k[40];
    const char *p;
    _snprintf_s(k, sizeof k, _TRUNCATE, "\"%s\":", key);
    p = strstr(l, k);
    if (!p) { return 0; }
    p += strlen(k);
    while (*p == ' ') { p++; }
    if (*p == '"') { p++; }
    if (strncmp(p, "true", 4) == 0) { *v = 1; return 1; }
    if (strncmp(p, "false", 5) == 0) { *v = 0; return 1; }
    *v = (unsigned)strtoul(p, NULL, 0);
    return 1;
}
static void json_s(const char *l, const char *key, char *out, size_t cap)
{
    char k[40];
    const char *p, *e;
    out[0] = '\0';
    _snprintf_s(k, sizeof k, _TRUNCATE, "\"%s\":\"", key);
    p = strstr(l, k);
    if (!p) { return; }
    p += strlen(k);
    e = strchr(p, '"');
    if (!e) { return; }
    _snprintf_s(out, cap, _TRUNCATE, "%.*s", (int)(e - p), p);
}

/* index.json einlesen (eine Textur je Zeile, wie idx_save schreibt). */
static void idx_load(void)
{
    char p[600], line[1024];
    FILE *f = NULL;
    int n = 0;
    idx_path(p, sizeof p, "index.json");
    if (fopen_s(&f, p, "r") != 0 || !f) { return; }
    while (fgets(line, sizeof line, f)) {
        char hs[24];
        const char *al;
        u64 hv;
        int i;
        unsigned v;
        json_s(line, "hash", hs, sizeof hs);
        if (strlen(hs) != 16u) { continue; }
        hv = _strtoui64(hs, NULL, 16);
        i = de_find(hv, 1);
        if (i < 0) { break; }
        {
            dent_t *e = &g_de[i];
            json_u(line, "w", &e->w);
            json_u(line, "h", &e->h);
            json_u(line, "code", &e->code);
            json_u(line, "mips", &e->mips);
            al = strstr(line, "\"alpha_kind\":\"");
            e->alpha = al ? (strncmp(al + 14, "full", 4) == 0 ? 2 : strncmp(al + 14, "1bit", 4) == 0 ? 1 : 0) : 0;
            if (json_u(line, "first_scene", &v)) { e->scene = v; }
            json_u(line, "first_mode", &e->mode);
            json_u(line, "first_mno", &e->mno);
            json_u(line, "first_mloop", &e->mloop);
            json_u(line, "first_frame", &e->frame);
            json_u(line, "uses", &e->uses);
            json_u(line, "uploads", &e->uploads);
            json_u(line, "runs", &e->runs);
            json_u(line, "draw2d", &e->d2);
            json_u(line, "draw3d", &e->d3);
            e->dark = -1;
            if (json_u(line, "dark", &v)) { e->dark = (int)v; }
            if (json_u(line, "cpu", &v)) { e->cpu = (int)v; }
            json_s(line, "src", e->src, sizeof e->src);
            e->png = 1;
            n++;
        }
    }
    fclose(f);
    HLOG_I("REMASTER dump: index.json geladen (%d Eintraege) %s", n, p);
}

static void idx_save(const char *why)
{
    char p[600], pt[600];
    FILE *f = NULL;
    int i, ncat[10] = { 0 };
    static const char *cats[10] = { "font_hud", "cockpit", "ground", "buildings", "vehicles", "effects", "menu", "world", "cpu", "unknown" };
    idx_path(p, sizeof p, "index.json");
    idx_path(pt, sizeof pt, "index.json.tmp");
    if (fopen_s(&f, pt, "w") != 0 || !f) { return; }
    fprintf(f, "{\n\"format\":\"sb-remaster-dump\",\"version\":1,\n"
               "\"hash_algo\":\"fnv1a64(SBT1|code|w|h|palette_len=0|mip0 dicht, nach Deswizzle)\",\n"
               "\"textures\":[\n");
    for (i = 0; i < g_nde; i++) {
        const dent_t *e = &g_de[i];
        const char *why2 = "", *cat = de_category(e, &why2);
        int k;
        for (k = 0; k < 10; k++) { if (strcmp(cats[k], cat) == 0) { ncat[k]++; break; } }
        fprintf(f, "{\"hash\":\"%016llx\",\"png\":\"%016llx.png\",\"file\":\"%016llx.png\",\"w\":%u,\"h\":%u,"
                   "\"width\":%u,\"height\":%u,\"fmt\":\"%s\",\"format\":\"%s\",\"code\":%u,"
                   "\"mips\":%u,\"alpha\":%s,\"alpha_kind\":\"%s\",\"first_scene\":\"0x%X\",\"first_mode\":%u,"
                   "\"first_mno\":%u,\"first_mloop\":%u,\"first_frame\":%u,\"uses\":%u,\"uploads\":%u,\"runs\":%u,"
                   "\"draw2d\":%u,\"draw3d\":%u,\"dark\":%d,\"cpu\":%s,\"src\":\"%s\",\"category\":\"%s\",\"cat_reason\":\"%s\"}%s\n",
                e->hash, e->hash, e->hash, e->w, e->h, e->w, e->h, fmt_name(e->code), fmt_name(e->code), e->code, e->mips,
                e->alpha ? "true" : "false", e->alpha == 2 ? "full" : e->alpha == 1 ? "1bit" : "none",
                e->scene, e->mode, e->mno, e->mloop, e->frame, e->uses, e->uploads, e->runs,
                e->d2, e->d3, e->dark, e->cpu ? "true" : "false", e->src, cat, why2,
                (i + 1 < g_nde) ? "," : "");
    }
    fprintf(f, "]\n}\n");
    fclose(f);
    if (!MoveFileExA(pt, p, MOVEFILE_REPLACE_EXISTING)) { return; }
    g_idx_dirty = 0;
    HLOG_I("REMASTER dump: index.json geschrieben (%s): eintraege=%d neu=%d png=%d png_fehler=%d cpu_skip=%d "
           "kat font_hud=%d cockpit=%d ground=%d buildings=%d vehicles=%d effects=%d menu=%d world=%d cpu=%d unknown=%d",
           why, g_nde, g_nde_new, g_npng, g_npng_fail, g_nskip_cpu,
           ncat[0], ncat[1], ncat[2], ncat[3], ncat[4], ncat[5], ncat[6], ncat[7], ncat[8], ncat[9]);
}

static int dark_pct(const unsigned char *rgba, unsigned n)
{
    unsigned i, vis = 0, dk = 0;
    for (i = 0; i < n; i++) {
        const unsigned char *q = rgba + 4u * i;
        if (q[3] < 128u) { continue; }
        vis++;
        if ((q[0] * 54u + q[1] * 183u + q[2] * 19u) >> 8 < 24u) { dk++; }
    }
    return vis ? (int)(dk * 100u / vis) : 0;
}
static int alpha_kind(const unsigned char *rgba, unsigned n)
{
    unsigned i;
    int k = 0;
    for (i = 0; i < n; i++) {
        unsigned a = rgba[4 * i + 3];
        if (a == 255u) { continue; }
        if (a != 0u) { return 2; }
        k = 1;
    }
    return k;
}

static void dump_texture(unsigned xptr, unsigned fmt, int kind, unsigned w, unsigned h,
                         const void *pix, unsigned pitch, unsigned nlev, u64 hv, xmap_t *xm)
{
    unsigned code = fmt_code(fmt);
    int i = de_find(hv, 1), isnew;
    dent_t *e;
    if (i < 0) { return; }
    e = &g_de[i];
    isnew = (e->w == 0u);
    if (isnew) {
        char src[96];
        e->w = w; e->h = h; e->code = code; e->mips = nlev;
        e->scene = rd32(0x6B96DCu); e->mode = rd32(0x2EEA34u);
        e->mno = rd16(0x3B89A0u); e->mloop = rd32(0x356F58u);
        e->frame = g_present_no;
        e->src[0] = '\0';
        g_nde_new++;
    }
    if (!e->src[0] && kind != 3) { /* Quelle (auch nachtraeglich fuer alte Eintraege); 0x06: pix = Kopie */
        char src[96];
        __try { src_lookup((unsigned)(uintptr_t)pix, src, sizeof src); } __except (EXCEPTION_EXECUTE_HANDLER) { src[0] = '\0'; }
        if (src[0]) { strncpy_s(e->src, sizeof e->src, src, _TRUNCATE); }
    }
    if (!e->seen_run) { e->seen_run = 1; e->runs++; }
    e->uploads++;
    if (xm) {
        xm->de = i;
        if (xm->cpu) { e->cpu = 1; }
    }
    g_idx_dirty = 1;
    if (!e->png) {
        char p[600], hs[24];
        unsigned char *rgba = (unsigned char *)malloc((size_t)w * h * 4u);
        _snprintf_s(hs, sizeof hs, _TRUNCATE, "%016llx.png", hv);
        idx_path(p, sizeof p, hs);
        e->png = 1;
        if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) {
            if (rgba && decode_rgba(kind, code, w, h, (const unsigned char *)pix, pitch, rgba)) {
                e->alpha = alpha_kind(rgba, w * h);
                e->dark = dark_pct(rgba, w * h);
            }
        } else if (rgba && decode_rgba(kind, code, w, h, (const unsigned char *)pix, pitch, rgba)) {
            e->alpha = alpha_kind(rgba, w * h);
            e->dark = dark_pct(rgba, w * h);
            if (png_write_rgba(p, rgba, w, h) == 0) {
                g_npng++;
                if (g_npng <= 16 || (g_npng % 256) == 0) {
                    HLOG_I("REMASTER dump #%d: %016llx %ux%u %s alpha=%d xptr=0x%X f=%u src=%s", g_npng, hv, w, h,
                           fmt_name(code), e->alpha, xptr, g_present_no, e->src[0] ? e->src : "-");
                }
            } else {
                g_npng_fail++;
            }
        } else {
            g_npng_fail++;
        }
        free(rgba);
    }
}

/* ------------------------------------------------------------------ */
/* Ersatz-Lader (DDS)                                                  */
/* ------------------------------------------------------------------ */
#define HD_MAX 8192
#define HD_HN 16384
typedef struct {
    u64 hash;
    int state; /* 1 = geladen, 2 = keine Datei, 3 = abgelehnt */
    ID3D11ShaderResourceView *srv;
    unsigned bytes, lastuse, w, h, mips;
} hdent_t;
static hdent_t *g_hd;
static int g_nhd;
static int *g_hdh;
static unsigned long long g_hd_bytes;
static unsigned g_hd_tick, g_hd_hits, g_hd_miss, g_hd_rej, g_hd_cpu, g_hd_load, g_hd_evict, g_hd_last_log;
static int g_hd_logdirty;

static int hd_find(u64 hv, int add)
{
    unsigned s = (unsigned)(hv ^ (hv >> 29)) & (HD_HN - 1u);
    while (g_hdh[s] >= 0) {
        if (g_hd[g_hdh[s]].hash == hv) { return g_hdh[s]; }
        s = (s + 1u) & (HD_HN - 1u);
    }
    if (!add || g_nhd >= HD_MAX) { return -1; }
    memset(&g_hd[g_nhd], 0, sizeof g_hd[g_nhd]);
    g_hd[g_nhd].hash = hv;
    g_hdh[s] = g_nhd;
    return g_nhd++;
}

static unsigned long long hd_limit(void)
{
    int mb = g_cfg.hd_cache_mb > 0 ? g_cfg.hd_cache_mb : 1024;
    return (unsigned long long)mb << 20;
}
/* Cache-Grenze: am laengsten unbenutzte geladene Eintraege freigeben
 * (gebundene SRVs halten eigene Referenzen im Textur-Cache). */
static void hd_trim(unsigned long long need)
{
    while (g_hd_bytes + need > hd_limit()) {
        int i, best = -1;
        unsigned age = 0;
        for (i = 0; i < g_nhd; i++) {
            if (g_hd[i].state == 1 && g_hd[i].srv && g_hd_tick - g_hd[i].lastuse >= age) {
                age = g_hd_tick - g_hd[i].lastuse;
                best = i;
            }
        }
        if (best < 0) { return; }
        ID3D11ShaderResourceView_Release(g_hd[best].srv);
        g_hd[best].srv = NULL;
        g_hd[best].state = 0; /* bei Bedarf neu laden */
        g_hd_bytes -= g_hd[best].bytes;
        g_hd_evict++;
    }
}

static int read_file(const char *p, unsigned char **out, size_t *n)
{
    FILE *f = NULL;
    long sz;
    *out = NULL;
    *n = 0;
    if (fopen_s(&f, p, "rb") != 0 || !f) { return 0; }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 128 || sz > (256L << 20)) { fclose(f); return 0; }
    *out = (unsigned char *)malloc((size_t)sz);
    if (!*out) { fclose(f); return 0; }
    *n = fread(*out, 1, (size_t)sz, f);
    fclose(f);
    return *n == (size_t)sz;
}
static unsigned u32le(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24); }

/* Bytes einer Stufe (bc: Blockgroesse, 0 = 32 bpp unkomprimiert). */
static size_t lvl_bytes(unsigned bc, unsigned w, unsigned h, unsigned *pitch)
{
    if (bc) {
        unsigned bw = (w + 3u) / 4u, bh = (h + 3u) / 4u;
        *pitch = bw * bc;
        return (size_t)bw * bh * bc;
    }
    *pitch = w * 4u;
    return (size_t)w * h * 4u;
}

/* DDS -> SRV. Liefert NULL bei Fehler (Grund im Log). */
static ID3D11ShaderResourceView *dds_load_mem(unsigned char *d, size_t n, const char *disp,
                                              unsigned ow, unsigned oh, unsigned *outw, unsigned *outh,
                                              unsigned *outmips, unsigned *outbytes, int *reject);
static ID3D11ShaderResourceView *dds_load(const char *path, unsigned ow, unsigned oh,
                                          unsigned *outw, unsigned *outh, unsigned *outmips,
                                          unsigned *outbytes, int *reject)
{
    unsigned char *d = NULL;
    size_t n = 0;
    if (!read_file(path, &d, &n)) { free(d); return NULL; }
    return dds_load_mem(d, n, path, ow, oh, outw, outh, outmips, outbytes, reject);
}
/* DDS-Bytes -> SRV (d gehoert danach uns, wird immer freigegeben). */
static ID3D11ShaderResourceView *dds_load_mem(unsigned char *d, size_t n, const char *disp,
                                              unsigned ow, unsigned oh, unsigned *outw, unsigned *outh,
                                              unsigned *outmips, unsigned *outbytes, int *reject)
{
    size_t off = 128, total = 0;
    unsigned w, h, mips, fourcc, pfflags, bits, rmask, bc = 0, lv, maxlev, t;
    DXGI_FORMAT df = DXGI_FORMAT_UNKNOWN;
    int gen = 0;
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA sr[16];
    ID3D11Texture2D *tex = NULL;
    ID3D11ShaderResourceView *srv = NULL;
    unsigned char *conv = NULL;
    static unsigned nlog;
    *reject = 0;
    if (!d || !n) { free(d); return NULL; }
    if (u32le(d) != 0x20534444u || u32le(d + 4) != 124u) { *reject = 1; goto bad; }
    h = u32le(d + 12);
    w = u32le(d + 16);
    mips = (u32le(d + 8) & 0x20000u) ? u32le(d + 28) : 1u;
    if (!mips) { mips = 1; }
    pfflags = u32le(d + 80);
    fourcc = u32le(d + 84);
    bits = u32le(d + 88);
    rmask = u32le(d + 92);
    if (pfflags & 4u) {
        if (fourcc == 0x31545844u) { df = DXGI_FORMAT_BC1_UNORM; bc = 8; }
        else if (fourcc == 0x33545844u || fourcc == 0x32545844u) { df = DXGI_FORMAT_BC2_UNORM; bc = 16; }
        else if (fourcc == 0x35545844u || fourcc == 0x34545844u) { df = DXGI_FORMAT_BC3_UNORM; bc = 16; }
        else if (fourcc == 0x30315844u) { /* "DX10" */
            unsigned f10;
            if (n < 148u) { *reject = 1; goto bad; }
            f10 = u32le(d + 128);
            off = 148;
            switch (f10) {
            case 71: case 72: df = DXGI_FORMAT_BC1_UNORM; bc = 8; break;
            case 74: case 75: df = DXGI_FORMAT_BC2_UNORM; bc = 16; break;
            case 77: case 78: df = DXGI_FORMAT_BC3_UNORM; bc = 16; break;
            case 98: case 99: df = DXGI_FORMAT_BC7_UNORM; bc = 16; break;
            case 28: case 29: df = DXGI_FORMAT_R8G8B8A8_UNORM; break;
            case 87: case 91: df = DXGI_FORMAT_B8G8R8A8_UNORM; break;
            default: break;
            }
        }
    } else if ((pfflags & 0x40u) && bits == 32u) { /* DDPF_RGB */
        df = (rmask == 0x00FF0000u) ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    if (df == DXGI_FORMAT_UNKNOWN || !w || !h || w > 16384u || h > 16384u) { *reject = 1; goto bad; }
    /* Seitenverhaeltnis + mind. Originalgroesse (nur Aufloesung groesser). */
    if ((unsigned long long)w * oh != (unsigned long long)h * ow || w < ow) {
        if (nlog < 16u) { nlog++; HLOG_W("REMASTER hd: %s abgelehnt: %ux%u passt nicht zu %ux%u (Seitenverhaeltnis/kleiner)", disp, w, h, ow, oh); }
        *reject = 2; /* Grund schon geloggt */
        goto bad;
    }
    maxlev = 1;
    for (t = (w > h) ? w : h; t > 1u; t >>= 1) { maxlev++; }
    if (mips > maxlev) { mips = maxlev; }
    if (mips > 16u) { mips = 16u; }
    for (lv = 0; lv < mips; lv++) {
        unsigned lw = (w >> lv) ? (w >> lv) : 1u, lh = (h >> lv) ? (h >> lv) : 1u, pitch;
        size_t lb = lvl_bytes(bc, lw, lh, &pitch);
        if (off + total + lb > n) { break; }
        sr[lv].pSysMem = d + off + total;
        sr[lv].SysMemPitch = pitch;
        sr[lv].SysMemSlicePitch = (UINT)lb;
        total += lb;
    }
    if (lv == 0u) { *reject = 1; goto bad; }
    mips = lv;
    memset(&td, 0, sizeof td);
    td.Width = w;
    td.Height = h;
    td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.Format = df;
    td.MipLevels = mips;
    if (mips == 1u && maxlev > 1u && df != DXGI_FORMAT_BC7_UNORM) {
        /* keine Mips in der Datei -> erzeugen (BC1-3 dafuer nach RGBA8) */
        gen = 1;
        if (bc) {
            conv = (unsigned char *)malloc((size_t)w * h * 4u);
            if (!conv) { *reject = 1; goto bad; }
            bc_decode(df == DXGI_FORMAT_BC1_UNORM ? 1 : df == DXGI_FORMAT_BC2_UNORM ? 2 : 3, d + off, w, h, conv);
            td.Format = df = DXGI_FORMAT_R8G8B8A8_UNORM;
            bc = 0;
        }
        td.MipLevels = 0;
        td.BindFlags |= D3D11_BIND_RENDER_TARGET;
        td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    }
    if (gen) {
        if (FAILED(ID3D11Device_CreateTexture2D(g_rdev, &td, NULL, &tex)) || !tex) { *reject = 1; goto bad; }
    } else if (FAILED(ID3D11Device_CreateTexture2D(g_rdev, &td, sr, &tex)) || !tex) {
        *reject = 1;
        goto bad;
    }
    if (FAILED(ID3D11Device_CreateShaderResourceView(g_rdev, (ID3D11Resource *)tex, NULL, &srv)) || !srv) {
        ID3D11Texture2D_Release(tex);
        *reject = 1;
        goto bad;
    }
    if (gen) {
        D3D11_TEXTURE2D_DESC got;
        sb_ctx_enter();
        ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)tex, 0, NULL,
                                              conv ? (const void *)conv : sr[0].pSysMem,
                                              conv ? w * 4u : sr[0].SysMemPitch, 0);
        ID3D11DeviceContext_GenerateMips(g_ctx, srv);
        sb_ctx_leave();
        ID3D11Texture2D_GetDesc(tex, &got);
        mips = got.MipLevels;
        total = 0;
        for (lv = 0; lv < mips; lv++) {
            unsigned pitch, lw = (w >> lv) ? (w >> lv) : 1u, lh = (h >> lv) ? (h >> lv) : 1u;
            total += lvl_bytes(0, lw, lh, &pitch);
        }
    }
    ID3D11Texture2D_Release(tex);
    free(conv);
    free(d);
    *outw = w;
    *outh = h;
    *outmips = mips;
    *outbytes = (unsigned)total;
    return srv;
bad:
    if (*reject == 1 && nlog < 16u) { nlog++; HLOG_W("REMASTER hd: %s nicht ladbar (Format/Groesse/Datei)", disp); }
    free(conv);
    free(d);
    return NULL;
}

/* W114: Freigaben je Variante aus <remaster_dir>/selection.json (Format v2,
 * remaster_select.py / Board): "approvals": {"<16 hex>": {"x2": "ok"|"nok",
 * "x4": "ok"|"nok"}, ...}. Fehlt eine Variante = unbewertet (Auto-Pack).
 * Gilt bei hd_scale 2 und 4 (alt 0 = W111-Auswahl -> 4 mit Freigaben):
 *   Stufe 4: 4x nicht "nok" -> x4, sonst 2x nicht "nok" -> x2, sonst Original.
 *   Stufe 2: 2x "nok" -> Original, sonst x2.
 * Fehlende, alte (v1 "choices") oder kaputte Datei = Auto-Pack, nie Abbruch. */
typedef struct {
    u64 hash;
    unsigned char a2, a4; /* 0 = offen, 1 = ok, 2 = nicht ok */
} selent_t;
static selent_t *g_sel;
static int g_nsel, g_sel_cap;
static unsigned g_sel_orig, g_sel_x2, g_sel_log;

static int sel_cmp(const void *a, const void *b)
{
    u64 x = ((const selent_t *)a)->hash, y = ((const selent_t *)b)->hash;
    return x < y ? -1 : (x > y ? 1 : 0);
}
static const char *sel_ws(const char *q)
{
    while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') { q++; }
    return q;
}
/* JSON-Zeichenkette ab q ('"') nach out (gekuerzt, NUL), -> Zeiger hinter das
 * schliessende '"' oder NULL (Dateiende). Escapes werden uebersprungen. */
static const char *sel_str(const char *q, char *out, int cap)
{
    int n = 0;
    if (*q != '"') { return NULL; }
    for (q++; *q && *q != '"'; q++) {
        if (*q == '\\') {
            q++;
            if (!*q) { return NULL; }
        }
        if (n < cap - 1) { out[n++] = *q; }
    }
    out[n] = '\0';
    return *q == '"' ? q + 1 : NULL;
}
static int sel_add(u64 h, int a2, int a4)
{
    if (g_nsel >= g_sel_cap) {
        int nc = g_sel_cap ? g_sel_cap * 2 : 1024;
        selent_t *n2 = (selent_t *)realloc(g_sel, sizeof *g_sel * (size_t)nc);
        if (!n2) { return -1; }
        g_sel = n2;
        g_sel_cap = nc;
    }
    g_sel[g_nsel].hash = h;
    g_sel[g_nsel].a2 = (unsigned char)a2;
    g_sel[g_nsel].a4 = (unsigned char)a4;
    g_nsel++;
    return 0;
}
/* "approvals"-Objekt lesen; 0 = ok, sonst Position des Fehlers (>0) */
static long sel_parse_v2(const char *d, const char *q)
{
    q = sel_ws(q + 11);
    if (*q != ':') { return (long)(q - d) + 1; }
    q = sel_ws(q + 1);
    if (*q != '{') { return (long)(q - d) + 1; }
    q = sel_ws(q + 1);
    if (*q == '}') { return 0; }
    for (;;) {
        char key[24], k[8], v[8];
        u64 h = 0;
        int i, valid, a2 = 0, a4 = 0;
        if ((q = sel_str(q, key, sizeof key)) == NULL) { return 1; }
        valid = strlen(key) == 16;
        for (i = 0; valid && i < 16; i++) {
            char c = key[i];
            int x = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
            if (x < 0) { valid = 0; }
            h = (h << 4) | (u64)(x & 15);
        }
        q = sel_ws(q);
        if (*q != ':') { return (long)(q - d) + 1; }
        q = sel_ws(q + 1);
        if (*q != '{') { return (long)(q - d) + 1; }
        q = sel_ws(q + 1);
        while (*q != '}') {
            int st;
            if ((q = sel_str(q, k, sizeof k)) == NULL) { return 1; }
            q = sel_ws(q);
            if (*q != ':') { return (long)(q - d) + 1; }
            q = sel_ws(q + 1);
            if ((q = sel_str(q, v, sizeof v)) == NULL) { return 1; }
            st = strcmp(v, "ok") == 0 ? 1 : (strcmp(v, "nok") == 0 ? 2 : 0);
            if (strcmp(k, "x2") == 0) { a2 = st; }
            else if (strcmp(k, "x4") == 0) { a4 = st; }
            q = sel_ws(q);
            if (*q == ',') { q = sel_ws(q + 1); continue; }
            if (*q != '}') { return (long)(q - d) + 1; }
        }
        q = sel_ws(q + 1);
        if (valid && (a2 || a4) && sel_add(h, a2, a4) != 0) { return 1; }
        if (*q == ',') { q = sel_ws(q + 1); continue; }
        if (*q == '}') { return 0; }
        return (long)(q - d) + 1;
    }
}
static void sel_load(void)
{
    char p[600], *d = NULL;
    const char *q;
    FILE *f = NULL;
    long sz = 0, err;
    int i, c[5] = { 0, 0, 0, 0, 0 }, stufe = g_cfg.hd_scale == 2 ? 2 : 4;
    if (g_cfg.hd_scale == 0) {
        HLOG_I("REMASTER sel: hd_scale=0 (alt, W111 Auswahl) -> Stufe 4 mit Freigaben");
    }
    _snprintf_s(p, sizeof p, _TRUNCATE, "%s/selection.json", g_rdir);
    if (fopen_s(&f, p, "rb") == 0 && f) {
        fseek(f, 0, SEEK_END);
        sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0 && sz < (64L << 20) && (d = (char *)malloc((size_t)sz + 1)) != NULL) {
            sz = (long)fread(d, 1, (size_t)sz, f);
            d[sz] = '\0';
        }
        fclose(f);
    }
    if (!d) {
        HLOG_I("REMASTER sel: %s fehlt -> Auto-Pack (stufe=%d, keine Freigaben)", p, stufe);
        return;
    }
    q = strstr(d, "\"approvals\"");
    if (!q) {
        HLOG_W("REMASTER sel: %s %s -> ignoriert, Auto-Pack (stufe=%d)", p,
               strstr(d, "\"choices\"") ? "altes Format (v1, W111)" : "ohne approvals", stufe);
        free(d);
        return;
    }
    err = sel_parse_v2(d, q);
    free(d);
    if (err) {
        HLOG_W("REMASTER sel: %s kaputt (bei Zeichen %ld) -> ignoriert, Auto-Pack (stufe=%d)", p, err - 1, stufe);
        g_nsel = 0;
        return;
    }
    if (g_nsel > 1) { qsort(g_sel, (size_t)g_nsel, sizeof *g_sel, sel_cmp); }
    for (i = 0; i < g_nsel; i++) {
        /* W132: nur bewertete Varianten zaehlen (0 = unbewertet; vorher
         * landeten fehlende x4-Wertungen in c[2] = x2_nok, W130). */
        if (g_sel[i].a2) c[g_sel[i].a2]++;     /* 1 ok, 2 nok */
        if (g_sel[i].a4) c[2 + g_sel[i].a4]++; /* 3 ok, 4 nok */
    }
    HLOG_I("REMASTER sel: %s v2 eintraege=%d x2_ok=%d x2_nok=%d x4_ok=%d x4_nok=%d stufe=%d", p, g_nsel, c[1], c[2],
           c[3], c[4], stufe);
}
static const selent_t *sel_get(u64 h)
{
    selent_t key;
    key.hash = h;
    return g_nsel ? (const selent_t *)bsearch(&key, g_sel, (size_t)g_nsel, sizeof *g_sel, sel_cmp) : NULL;
}
/* Paket-Stufe je Textur nach den Freigaben (4, 2 oder 0 = Original) */
static int sel_stufe(u64 hv)
{
    int sc = g_cfg.hd_scale == 2 ? 2 : 4, want;
    const selent_t *r = sel_get(hv);
    static const char *const an[3] = { "offen", "ok", "nok" };
    if (!r) { return sc; }
    want = sc;
    if (sc == 4 && r->a4 == 2) { sc = r->a2 == 2 ? 0 : 2; }
    else if (sc == 2 && r->a2 == 2) { sc = 0; }
    if (sc != want) {
        if (sc == 2) { g_sel_x2++; }
        if (g_sel_log < 32u) {
            g_sel_log++;
            HLOG_I("REMASTER sel: %016llx stufe %d -> %s (2x=%s 4x=%s)", hv, want, sc ? "x2" : "original", an[r->a2],
                   an[r->a4]);
        }
    }
    return sc;
}

/* W113 REM-TEXT: manuelle Ersatz-Texturen <remaster_dir>/textures/manual/ */
static int g_man_dir; /* Ordner vorhanden (sonst keine Datei-Tests) */
static unsigned g_man_load, g_man_orig;

static int man_count(const char *sub)
{
    char p[700];
    WIN32_FIND_DATAA fd;
    HANDLE fh;
    int n = 0;
    _snprintf_s(p, sizeof p, _TRUNCATE, "%s/textures/manual%s/*.dds", g_rdir, sub);
    fh = FindFirstFileA(p, &fd);
    if (fh == INVALID_HANDLE_VALUE) { return 0; }
    do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { n++; } } while (FindNextFileA(fh, &fd));
    FindClose(fh);
    return n;
}

static void man_init(void)
{
    char p[700];
    DWORD a;
    int n4, n2;
    g_man_dir = 0;
    if (!g_cfg.hd_manual) { return; }
    _snprintf_s(p, sizeof p, _TRUNCATE, "%s/textures/manual", g_rdir);
    a = GetFileAttributesA(p);
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) { return; }
    g_man_dir = 1;
    n4 = man_count("");
    n2 = man_count("/x2");
    HLOG_I("REMASTER manual: %s dateien=%d x2=%d (Vorrang vor x2/x4/Freigaben, ausser 2x+4x nicht OK)", p, n4, n2);
}

/* Pfad der manuellen Ersatz-Textur (Stufe 2: zuerst manual/x2/), 0 = keine */
static int man_path(u64 hv, int sc, char *p, size_t cap)
{
    if (!g_man_dir) { return 0; }
    if (sc == 2) {
        _snprintf_s(p, cap, _TRUNCATE, "%s/textures/manual/x2/%016llx.dds", g_rdir, hv);
        if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) { return 1; }
    }
    _snprintf_s(p, cap, _TRUNCATE, "%s/textures/manual/%016llx.dds", g_rdir, hv);
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
}

static ID3D11ShaderResourceView *hd_lookup(u64 hv, unsigned ow, unsigned oh)
{
    int i = hd_find(hv, 1);
    hdent_t *e;
    if (i < 0) { return NULL; }
    e = &g_hd[i];
    g_hd_tick++;
    if (e->state == 2 || e->state == 3) { return NULL; }
    if (e->state == 0) {
        char p[700];
        unsigned w = 0, h = 0, mips = 0, bytes = 0;
        int rej = 0, sc = sel_stufe(hv), man = 0; /* W114: Stufe 4/2 nach Freigaben, 0 = Original */
        const selent_t *r = sel_get(hv);
        ID3D11ShaderResourceView *srv = NULL;
        if (g_cfg.pack_loader) { /* W140: Ersatz aus .sbpack (Stufe aus hd_scale) */
            sb_pack_loc_t loc;
            unsigned char *bd = NULL;
            unsigned long bn = 0;
            sc = g_cfg.hd_scale == 2 ? 2 : 4;
            if (!sb_pack_active() || !sb_pack_find(hv, sc, &loc) ||
                !sb_pack_read(&loc, &bd, &bn)) {
                free(bd);
                e->state = 2;
                g_hd_miss++;
                g_hd_logdirty = 1;
                return NULL;
            }
            srv = dds_load_mem(bd, (size_t)bn, loc.path, ow, oh, &w, &h, &mips, &bytes, &rej);
        } else {
        { /* W113: manual vor x2/x4. Steuer 6.11. (Nutzer zu 127b0809): auch wenn beide KI-Varianten "nicht OK" sind -
           eine Text-Rekonstruktion bleibt, die Ablehnung gilt den KI-Bildern */
            man = man_path(hv, sc ? sc : (g_cfg.hd_scale == 2 ? 2 : 4), p, sizeof p);
        }
        if (sc == 0 && !man) { /* Freigaben: Variante(n) nicht OK -> kein Ersatz */
            if (r && man_path(hv, 4, p, sizeof p)) { g_man_orig++; }
            e->state = 2;
            g_sel_orig++;
            return NULL;
        }
        if (!man) {
            _snprintf_s(p, sizeof p, _TRUNCATE, "%s/textures/x%d/%016llx.dds", g_rdir, sc, hv);
            if (GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) {
                _snprintf_s(p, sizeof p, _TRUNCATE, "%s/textures/%016llx.dds", g_rdir, hv);
            }
        }
        if (GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) {
            e->state = 2;
            g_hd_miss++;
            g_hd_logdirty = 1;
            return NULL;
        }
        srv = dds_load(p, ow, oh, &w, &h, &mips, &bytes, &rej);
        } /* else pack_loader */
        if (!srv) {
            e->state = 3;
            g_hd_rej++;
            g_hd_logdirty = 1;
            return NULL;
        }
        hd_trim(bytes);
        if (g_hd_bytes + bytes > hd_limit()) {
            ID3D11ShaderResourceView_Release(srv);
            e->state = 3;
            g_hd_rej++;
            HLOG_W("REMASTER hd: %016llx (%u KB) passt nicht in hd_cache_mb", hv, bytes >> 10);
            return NULL;
        }
        e->srv = srv;
        e->state = 1;
        e->w = w; e->h = h; e->mips = mips; e->bytes = bytes;
        g_hd_bytes += bytes;
        g_hd_load++;
        g_hd_logdirty = 1;
        if (man) {
            g_man_load++;
            if (g_man_load <= 64u) {
                HLOG_I("REMASTER manual #%u: %016llx %ux%u -> %ux%u mips=%u %u KB (vram=%.1f MB) %s", g_man_load, hv,
                       ow, oh, w, h, mips, bytes >> 10, (double)g_hd_bytes / 1048576.0,
                       strstr(p, "/manual/x2/") ? "manual/x2" : "manual");
            }
        } else if (g_hd_load <= 32u || (g_hd_load % 256u) == 0) {
            HLOG_I("REMASTER hd #%u: %016llx %ux%u -> %ux%u mips=%u %u KB (vram=%.1f MB)", g_hd_load, hv, ow, oh,
                   w, h, mips, bytes >> 10, (double)g_hd_bytes / 1048576.0);
        }
    }
    e->lastuse = g_hd_tick;
    g_hd_hits++;
    ID3D11ShaderResourceView_AddRef(e->srv);
    return e->srv;
}

/* ------------------------------------------------------------------ */
/* Einstiege                                                           */
/* ------------------------------------------------------------------ */
static void rem_resolve_dir(void)
{
    if (g_cfg.remaster_dir[0]) {
        strcpy_s(g_rdir, sizeof g_rdir, g_cfg.remaster_dir);
    } else {
        /* auto: erster Vorfahr des Programmordners mit poc-native/ -> <der>/remaster */
        char exe[512], probe[600], *s;
        DWORD n = GetModuleFileNameA(NULL, exe, sizeof exe);
        strcpy_s(g_rdir, sizeof g_rdir, "remaster");
        if (n && n < sizeof exe) {
            for (s = exe; *s; s++) { if (*s == '\\') { *s = '/'; } }
            while ((s = strrchr(exe, '/')) != NULL) {
                *s = '\0';
                _snprintf_s(probe, sizeof probe, _TRUNCATE, "%s/poc-native", exe);
                if (GetFileAttributesA(probe) != INVALID_FILE_ATTRIBUTES) {
                    _snprintf_s(g_rdir, sizeof g_rdir, _TRUNCATE, "%s/remaster", exe);
                    break;
                }
            }
        }
    }
    { size_t l = strlen(g_rdir); while (l && (g_rdir[l - 1] == '/' || g_rdir[l - 1] == '\\')) { g_rdir[--l] = '\0'; } }
}

/* W151 EFFECT-PACK: Effekt-Parameter aus aktiven Effect-Packs lesen und auf
 * die cfg-Werte legen (Paket gewinnt ueber cfg, PACK_FORMAT.md Abschnitt 9.3).
 * Startpfad, unabhaengig von remaster/hd (die fx_-Optionen wirken auch solo).
 * Ohne Effect-Pack: keine Aenderung (bitgleich). */
static unsigned g_rem_fx_lock; /* W173: Bit i = kSbFxParams[i] von einem Effect Pack gesetzt */

int sb_rem_fx_locked(const char *key)
{
    int i;
    if (!g_rem_fx_lock || !key) { return 0; }
    for (i = 0; i < SB_FXN; i++) {
        if ((g_rem_fx_lock & (1u << i)) && strcmp(kSbFxParams[i].key, key) == 0) { return 1; }
    }
    return 0;
}

void sb_rem_fx_start(void)
{
    char pl[1100];
    int out[SB_FXN], hit[SB_FXN], i, n = 0;
    static char src[SB_FXN][128]; /* static: 1,4 KB, Stack schonen */
    if (!g_cfg.pack_loader || !g_cfg.pack_fx) { return; }
    if (!g_rdir[0]) { rem_resolve_dir(); }
    if (!g_cfg.pack_list[0]) { return; }
    if ((strlen(g_cfg.pack_list) > 1 && g_cfg.pack_list[1] == ':') ||
        g_cfg.pack_list[0] == '/' || g_cfg.pack_list[0] == '\\') {
        strcpy_s(pl, sizeof pl, g_cfg.pack_list);
    } else {
        _snprintf_s(pl, sizeof pl, _TRUNCATE, "%s/%s", g_rdir, g_cfg.pack_list);
    }
    if (GetFileAttributesA(pl) == INVALID_FILE_ATTRIBUTES) { return; }
    if (!sb_pack_fxparams(pl, out, hit, src)) { return; }
    for (i = 0; i < SB_FXN; i++) {
        const char *k;
        if (!hit[i]) { continue; }
        k = kSbFxParams[i].key;
        if (strcmp(k, "fx_soft") == 0) { g_cfg.fx_soft = out[i]; }
        else if (strcmp(k, "fx_embers") == 0) { g_cfg.fx_embers = out[i]; }
        else if (strcmp(k, "fx_light") == 0) { g_cfg.fx_light = out[i]; }
        else if (strcmp(k, "fx_light_cockpit") == 0) { g_cfg.fx_light_cockpit = out[i]; }
        else if (strcmp(k, "fx_interp") == 0) { g_cfg.fx_interp = out[i]; }
        else if (strcmp(k, "bloom_threshold") == 0) { g_cfg.bloom_threshold = out[i]; }
        else if (strcmp(k, "color_sat") == 0) { g_cfg.color_sat = out[i]; }
        else if (strcmp(k, "color_temp") == 0) { g_cfg.color_temp = out[i]; }
        else if (strcmp(k, "color_lift") == 0) { g_cfg.color_lift = out[i]; }
        else if (strcmp(k, "color_gamma") == 0) { g_cfg.color_gamma = out[i]; }
        else if (strcmp(k, "color_gain") == 0) { g_cfg.color_gain = out[i]; }
        else { continue; }
        g_rem_fx_lock |= 1u << i; /* W173: Effect-Pack-Ausnahme (Preset-Erkennung) */
        n++;
    }
    if (n > 0) {
        /* Eine Zeile je beitragendem Paket (Quelle je Parameter). */
        char done[SB_FXN][128];
        int ndone = 0, p;
        for (p = 0; p < SB_FXN; p++) {
            char line[900], *w;
            size_t rest;
            int q, first;
            if (!hit[p]) { continue; }
            for (q = 0; q < ndone; q++) {
                if (strcmp(done[q], src[p]) == 0) { break; }
            }
            if (q < ndone) { continue; }
            strcpy_s(done[ndone], sizeof done[ndone], src[p]);
            ndone++;
            w = line;
            rest = sizeof line;
            {
                int L = _snprintf_s(w, rest, _TRUNCATE, "PACK-FX: %s: ", src[p]);
                if (L < 0) { continue; }
                w += (size_t)L;
                rest -= (size_t)L;
            }
            first = 1;
            for (q = 0; q < SB_FXN; q++) {
                if (hit[q] && strcmp(src[q], src[p]) == 0) {
                    int L = _snprintf_s(w, rest, _TRUNCATE, "%s%s=%d",
                                        first ? "" : ", ", kSbFxParams[q].key, out[q]);
                    if (L < 0) { break; }
                    w += (size_t)L;
                    rest -= (size_t)L;
                    first = 0;
                }
            }
            HLOG_I("%s", line);
        }
        HLOG_I("PACK-FX: %d Parameter aus Effect-Packs (Paket gewinnt ueber cfg)", n);
    }
}

/* W140: Paket-Lader starten (pack_list relativ zu g_rdir). W148: fehlende Datei = keine Pakete (Original). */
static void rem_pack_start(void)
{
    char pl[1100];
    if (!g_cfg.pack_list[0]) { pl[0] = '\0'; }
    else if ((strlen(g_cfg.pack_list) > 1 && g_cfg.pack_list[1] == ':') ||
             g_cfg.pack_list[0] == '/' || g_cfg.pack_list[0] == '\\') {
        strcpy_s(pl, sizeof pl, g_cfg.pack_list);
    } else {
        _snprintf_s(pl, sizeof pl, _TRUNCATE, "%s/%s", g_rdir, g_cfg.pack_list);
    }
    if (pl[0] && GetFileAttributesA(pl) == INVALID_FILE_ATTRIBUTES) { /* W148: Standard ohne Pakete = Original */
        HLOG_I("REMASTER pack: %s fehlt -> keine Pakete (Original)", pl);
        pl[0] = '\0';
    }
    sb_pack_init(pl, g_rdir);
}

void sb_rem_init(ID3D11Device *dev)
{
    int i;
    g_rdev = dev;
    if (g_inited) { return; }
    g_inited = 1;
    g_dump_on = g_cfg.tex_dump ? 1 : 0;
    g_hd_on = (g_cfg.remaster && g_cfg.hd_textures) ? 1 : 0;
    HLOG_I("REMASTER cfg: remaster=%d hd_textures=%d hd_scale=%d fx_enhanced=%d ultrawide=%d postfx=%d tex_dump=%d hd_cache_mb=%d",
           g_cfg.remaster, g_cfg.hd_textures, g_cfg.hd_scale, g_cfg.fx_enhanced, g_cfg.rem_ultrawide,
           g_cfg.postfx, g_cfg.tex_dump, g_cfg.hd_cache_mb);
    if (!g_dump_on && !g_hd_on) { return; }
    InitializeCriticalSection(&g_rcs);
    g_rcs_ok = 1;
    rem_resolve_dir();
    g_xm = (xmap_t *)calloc(XM_N, sizeof *g_xm);
    if (g_dump_on) {
        char p[600];
        CreateDirectoryA(g_rdir, NULL);
        _snprintf_s(p, sizeof p, _TRUNCATE, "%s/dump", g_rdir);
        CreateDirectoryA(p, NULL);
        g_de = (dent_t *)calloc(DE_MAX, sizeof *g_de);
        g_deh = (int *)malloc(sizeof(int) * DE_HN);
        if (!g_de || !g_deh) { g_dump_on = 0; }
        else {
            for (i = 0; i < DE_HN; i++) { g_deh[i] = -1; }
            idx_load();
        }
    }
    if (g_hd_on) {
        g_hd = (hdent_t *)calloc(HD_MAX, sizeof *g_hd);
        g_hdh = (int *)malloc(sizeof(int) * HD_HN);
        if (!g_hd || !g_hdh) { g_hd_on = 0; }
        else { for (i = 0; i < HD_HN; i++) { g_hdh[i] = -1; } }
        if (g_hd_on && g_cfg.pack_loader) { rem_pack_start(); } /* W140: Pakete statt Ordner+Freigaben */
        if (g_hd_on && !g_cfg.pack_loader) { sel_load(); } /* W114: Freigaben gelten bei Stufe 2 und 4 */
        if (g_hd_on && !g_cfg.pack_loader) { man_init(); } /* W113: manuelle Ersatz-Texturen */
    }
    HLOG_I("REMASTER: dir=%s dump=%d hd=%d (textures/x%d, Grenze %d MB)", g_rdir, g_dump_on, g_hd_on,
           g_cfg.hd_scale, g_cfg.hd_cache_mb);
}

int sb_rem_active(void) { return g_dump_on || g_hd_on; }

/* W118 REM-OVERLAY: vorhandene Paket-Stufen (1 = textures/x2, 2 = textures/x4). */
int sb_rem_live_avail(void)
{
    if (g_cfg.pack_loader) { return sb_pack_avail(); } /* W140 */
    char p[700];
    DWORD a;
    int m = 0, sc;
    if (!g_rdir[0]) { rem_resolve_dir(); }
    for (sc = 2; sc <= 4; sc += 2) {
        _snprintf_s(p, sizeof p, _TRUNCATE, "%s/textures/x%d", g_rdir, sc);
        a = GetFileAttributesA(p);
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) { m |= (sc == 2) ? 1 : 2; }
    }
    return m;
}

/* W118 REM-OVERLAY: HD-Ersatz live an/aus bzw. Stufe 2/4 (Render-Thread,
 * zwischen den Bildern). Legt beim ersten Einschalten die Ersatz-Strukturen
 * an (wie sb_rem_init), leert bei Stufenwechsel den HD-Cache (Eintraege
 * merken den Zustand je Stufe). Der Aufrufer laedt danach den Textur-Cache
 * neu (sb_tex_relive_all). Rueckgabe 1 = ok, -1 = kein Paket (nichts
 * geaendert). */
int sb_rem_live_set(int on, int scale)
{
    int i;
    scale = scale == 2 ? 2 : 4;
    if (on && !(sb_rem_live_avail() & (scale == 2 ? 1 : 2))) { return -1; }
    if (on && !g_hd) {
        if (!g_rcs_ok) { InitializeCriticalSection(&g_rcs); g_rcs_ok = 1; }
        if (!g_rdir[0]) { rem_resolve_dir(); }
        if (!g_xm) { g_xm = (xmap_t *)calloc(XM_N, sizeof *g_xm); }
        g_hd = (hdent_t *)calloc(HD_MAX, sizeof *g_hd);
        g_hdh = (int *)malloc(sizeof(int) * HD_HN);
        if (!g_hd || !g_hdh || !g_xm) {
            free(g_hd); g_hd = NULL;
            free(g_hdh); g_hdh = NULL;
            return -1;
        }
        for (i = 0; i < HD_HN; i++) { g_hdh[i] = -1; }
        g_cfg.hd_scale = scale;
        if (g_cfg.pack_loader) { rem_pack_start(); } /* W140 */
        else { sel_load(); man_init(); }
        HLOG_I("REMASTER live: Ersatz-Lader angelegt (dir=%s)", g_rdir);
    }
    EnterCriticalSection(&g_rcs);
    if (g_hd && (g_cfg.hd_scale == 2 ? 2 : 4) != scale) {
        for (i = 0; i < g_nhd; i++) {
            if (g_hd[i].srv) { ID3D11ShaderResourceView_Release(g_hd[i].srv); }
        }
        memset(g_hd, 0, sizeof(hdent_t) * HD_MAX);
        g_nhd = 0;
        for (i = 0; i < HD_HN; i++) { g_hdh[i] = -1; }
        g_hd_bytes = 0;
    }
    g_cfg.hd_scale = scale;
    if (on) { g_cfg.remaster = 1; }
    g_cfg.hd_textures = on ? 1 : 0;
    g_hd_on = (g_cfg.remaster && g_cfg.hd_textures && g_hd) ? 1 : 0;
    LeaveCriticalSection(&g_rcs);
    HLOG_I("REMASTER live: hd=%d x%d (vram=%.1f MB, f=%u)", g_hd_on, scale, (double)g_hd_bytes / 1048576.0,
           g_present_no);
    return 1;
}

ID3D11ShaderResourceView *sb_rem_upload(unsigned xptr, unsigned fmt, int kind, unsigned w, unsigned h,
                                        const void *pix, unsigned pitch, unsigned nlev,
                                        ID3D11ShaderResourceView *orig)
{
    u64 hv;
    int ok = 0;
    xmap_t *xm;
    ID3D11ShaderResourceView *rep = NULL;
    unsigned code = fmt_code(fmt);
    if ((!g_dump_on && !g_hd_on) || !orig || !w || !h) { return orig; }
    /* PERF(FILM-FPS,W123) hd_cpu_nohash: per CPU beschriebene Texturen (Bink-
     * Film: jedes Filmbild ein Upload) werden nie ersetzt und - ausser beim
     * ersten Dump-Stand - nie gedumpt; der Inhalts-Hash (byteweise FNV-64 ueber
     * den ganzen Mip-0) waere unbenutzt. Gleiche Zaehler/Zustaende wie unten. */
    if (g_cfg.hd_cpu_nohash) {
        int skip = 0;
        EnterCriticalSection(&g_rcs);
        xm = xm_get(xptr, 0);
        if (xm && xm->cpu && (!g_dump_on || kind == 2 || xm->cpu_dumped)) {
            skip = 1;
            xm->hash = 0;
            xm->de = -1;
            if (g_dump_on) { g_nskip_cpu++; }
            if (g_hd_on) { g_hd_cpu++; }
        }
        LeaveCriticalSection(&g_rcs);
        if (skip) { return orig; }
    }
    hv = tex_content_hash(kind, code, w, h, pix, pitch, &ok);
    if (!ok) { return orig; }
    EnterCriticalSection(&g_rcs);
    xm = xm_get(xptr, 1);
    if (xm) { xm->hash = hv; xm->de = -1; }
    if (g_dump_on) {
        if (xm && xm->cpu && kind == 2) {
            g_nskip_cpu++; /* Film/CPU-Textebene: nie dumpen */
        } else if (xm && xm->cpu && xm->cpu_dumped) {
            g_nskip_cpu++; /* per CPU beschrieben: nur erster Stand je xptr */
        } else {
            if (xm && xm->cpu) { xm->cpu_dumped = 1; }
            dump_texture(xptr, fmt, kind, w, h, pix, pitch, nlev, hv, xm);
        }
    }
    if (g_hd_on) {
        if (xm && xm->cpu) {
            g_hd_cpu++;
        } else {
            rep = hd_lookup(hv, w, h);
        }
    }
    LeaveCriticalSection(&g_rcs);
    if (rep) {
        ID3D11ShaderResourceView_Release(orig);
        return rep;
    }
    return orig;
}

void sb_rem_note_cpu(unsigned xptr)
{
    xmap_t *xm;
    if (!g_dump_on && !g_hd_on) { return; }
    EnterCriticalSection(&g_rcs);
    xm = xm_get(xptr, 1);
    if (xm && !xm->cpu) {
        static unsigned ncpu;
        xm->cpu = 1;
        if (g_dump_on && xm->de >= 0) { g_de[xm->de].cpu = 1; g_idx_dirty = 1; }
        if (++ncpu <= 32u) { HLOG_I("REMASTER cpu #%u: xptr=0x%X (Schreib-Lock -> nie ersetzen)", ncpu, xptr); }
    }
    LeaveCriticalSection(&g_rcs);
}

void sb_rem_forget(unsigned xptr)
{
    xmap_t *xm;
    if (!g_dump_on && !g_hd_on) { return; }
    EnterCriticalSection(&g_rcs);
    xm = xm_get(xptr, 0);
    if (xm) { xm->cpu = 0; xm->cpu_dumped = 0; xm->de = -1; xm->hash = 0; }
    LeaveCriticalSection(&g_rcs);
}

void sb_rem_note_draw(int rhw, unsigned xptr)
{
    xmap_t *xm;
    if (!g_dump_on || !xptr) { return; }
    EnterCriticalSection(&g_rcs);
    xm = xm_get(xptr, 0);
    if (xm && xm->de >= 0) {
        dent_t *e = &g_de[xm->de];
        e->uses++;
        if (rhw) { e->d2++; } else { e->d3++; }
    }
    LeaveCriticalSection(&g_rcs);
}

void sb_rem_present(unsigned frame)
{
    if (!g_dump_on && !g_hd_on) { return; }
    EnterCriticalSection(&g_rcs);
    if (g_dump_on && frame - g_idx_last_flush >= 600u) {
        g_idx_last_flush = frame;
        if (g_idx_dirty) { idx_save("periodisch"); }
    }
    if (g_hd_on && g_hd_logdirty && frame - g_hd_last_log >= 300u) {
        g_hd_last_log = frame;
        g_hd_logdirty = 0;
        HLOG_I("REMASTER hd stat: f=%u geladen=%u vram=%.1f MB treffer=%u fehlend=%u abgelehnt=%u cpu=%u verdraengt=%u",
               frame, g_hd_load, (double)g_hd_bytes / 1048576.0, g_hd_hits, g_hd_miss, g_hd_rej, g_hd_cpu, g_hd_evict);
    }
    LeaveCriticalSection(&g_rcs);
}

void sb_rem_shutdown(void)
{
    if (!g_dump_on && !g_hd_on) { return; }
    /* TryEnter: der Render-Thread koennte den Schnitt halten (Exit-Pfad) */
    if (!TryEnterCriticalSection(&g_rcs)) { Sleep(50); if (!TryEnterCriticalSection(&g_rcs)) { return; } }
    if (g_dump_on) { idx_save("ende"); }
    if (g_hd_on) {
        HLOG_I("REMASTER hd stat: ende geladen=%u vram=%.1f MB treffer=%u fehlend=%u abgelehnt=%u cpu=%u verdraengt=%u",
               g_hd_load, (double)g_hd_bytes / 1048576.0, g_hd_hits, g_hd_miss, g_hd_rej, g_hd_cpu, g_hd_evict);
        HLOG_I("REMASTER sel stat: ende eintraege=%d wahl_original=%u x4_zu_x2=%u stufe=%d", g_nsel, g_sel_orig,
               g_sel_x2, g_cfg.hd_scale == 2 ? 2 : 4);
        if (g_man_dir) {
            HLOG_I("REMASTER manual stat: ende geladen=%u wahl_original=%u", g_man_load, g_man_orig);
        }
    }
    sb_pack_shutdown(); /* W140: ohne Pakete ein No-Op */
    LeaveCriticalSection(&g_rcs);
}
