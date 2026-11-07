/* sb_pack.c - .sbpack-Paketlader v1 (W140, PACK_FORMAT.md).
 *
 * Start (sb_pack_init): packs.json + Manifeste (+ entries.json) lesen,
 * ZIP-Verzeichnisse einlesen, Index Hash -> Variante bauen, Shadowing
 * (normal vor fallback, Reihenfolge) einmal aufloesen, Stufen-Obergrenzen
 * (category_level, overrides) je Eintrag vormerken.
 * Spielpfad (sb_pack_find/sb_pack_read): nur Hash-Lookup + Datei/Offset
 * lesen. Kein JSON, keine Pruefsummen, keine Verzeichnissuche.
 * Aufruf nur aus sb_remaster.c unter g_rcs (keine eigene Sperre).
 */
#include "sb_pack.h"
#include "hlog.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef unsigned long long u64;

/* ------------------------------------------------------------------ */
/* JSON (kleines DOM, nur Startpfad)                                   */
/* ------------------------------------------------------------------ */
typedef struct jnode jnode_t;
struct jnode {
    int kind; /* 0=null 1=false 2=true 3=Zahl 4=Text 5=Objekt 6=Feld */
    double num;
    char *s;               /* Text (kind 4) */
    char **keys;           /* Objekt */
    jnode_t **vals;
    int n, cap;
    jnode_t **items;       /* Feld */
};

static void jfree(jnode_t *j)
{
    int i;
    if (!j) { return; }
    free(j->s);
    for (i = 0; i < j->n; i++) {
        free(j->keys ? j->keys[i] : NULL);
        jfree(j->vals ? j->vals[i] : NULL);
        jfree(j->items ? j->items[i] : NULL);
    }
    free(j->keys);
    free(j->vals);
    free(j->items);
    free(j);
}

typedef struct { const char *p, *end; int depth, err; } jrd_t;

static void jws(jrd_t *r)
{
    while (r->p < r->end && (*r->p == ' ' || *r->p == '\t' ||
                             *r->p == '\r' || *r->p == '\n')) { r->p++; }
}

static jnode_t *jnew(int kind)
{
    jnode_t *j = (jnode_t *)calloc(1, sizeof *j);
    if (j) { j->kind = kind; }
    return j;
}

static int jpush(jnode_t *j, char *k, jnode_t *v)
{
    if (j->n >= j->cap) {
        int nc = j->cap ? j->cap * 2 : 8, i;
        char **nk;
        jnode_t **nv, **ni;
        if (j->kind == 5) {
            nk = (char **)realloc(j->keys, sizeof *nk * (size_t)nc);
            nv = (jnode_t **)realloc(j->vals, sizeof *nv * (size_t)nc);
            if (!nk || !nv) { return -1; }
            j->keys = nk;
            j->vals = nv;
        } else {
            ni = (jnode_t **)realloc(j->items, sizeof *ni * (size_t)nc);
            if (!ni) { return -1; }
            j->items = ni;
        }
        for (i = j->cap; i < nc; i++) {
            if (j->kind == 5) { j->keys[i] = NULL; j->vals[i] = NULL; }
            else { j->items[i] = NULL; }
        }
        j->cap = nc;
    }
    if (j->kind == 5) {
        j->keys[j->n] = k;
        j->vals[j->n] = v;
    } else {
        j->items[j->n] = v;
    }
    j->n++;
    return 0;
}

/* Text ab " lesen (Escapes aufgeloest, ohne \\u-Surrogates: \\uXXXX wird
 * UTF-8). Rueckgabe malloc'd oder NULL. */
static char *jstr(jrd_t *r)
{
    size_t cap = 32, n = 0;
    char *o = (char *)malloc(cap);
    if (!o || r->p >= r->end || *r->p != '"') { free(o); return NULL; }
    r->p++;
    while (r->p < r->end && *r->p != '"') {
        unsigned c = (unsigned char)*r->p++;
        char tmp[4];
        int tn = 0;
        if (c == '\\' && r->p < r->end) {
            char e = *r->p++;
            if (e == 'u' && r->p + 4 <= r->end) {
                unsigned v = 0, i;
                for (i = 0; i < 4; i++) {
                    char h = r->p[i];
                    v <<= 4;
                    if (h >= '0' && h <= '9') { v |= (unsigned)(h - '0'); }
                    else if (h >= 'a' && h <= 'f') { v |= (unsigned)(h - 'a' + 10); }
                    else if (h >= 'A' && h <= 'F') { v |= (unsigned)(h - 'A' + 10); }
                    else { free(o); return NULL; }
                }
                r->p += 4;
                if (v < 0x80) { tmp[0] = (char)v; tn = 1; }
                else if (v < 0x800) {
                    tmp[0] = (char)(0xC0 | (v >> 6));
                    tmp[1] = (char)(0x80 | (v & 63));
                    tn = 2;
                } else {
                    tmp[0] = (char)(0xE0 | (v >> 12));
                    tmp[1] = (char)(0x80 | ((v >> 6) & 63));
                    tmp[2] = (char)(0x80 | (v & 63));
                    tn = 3;
                }
            } else if (e == 'n') { tmp[0] = '\n'; tn = 1; }
            else if (e == 't') { tmp[0] = '\t'; tn = 1; }
            else if (e == 'r') { tmp[0] = '\r'; tn = 1; }
            else if (e == 'b') { tmp[0] = '\b'; tn = 1; }
            else if (e == 'f') { tmp[0] = '\f'; tn = 1; }
            else { tmp[0] = e; tn = 1; }
        } else if (c < 0x20) {
            free(o);
            return NULL;
        } else {
            tmp[0] = (char)c;
            tn = 1;
        }
        while (n + (size_t)tn + 1 > cap) {
            char *q;
            cap *= 2;
            q = (char *)realloc(o, cap);
            if (!q) { free(o); return NULL; }
            o = q;
        }
        memcpy(o + n, tmp, (size_t)tn);
        n += (size_t)tn;
    }
    if (r->p >= r->end) { free(o); return NULL; }
    r->p++;
    o[n] = '\0';
    return o;
}

static jnode_t *jval(jrd_t *r);

static jnode_t *jobj(jrd_t *r)
{
    jnode_t *j = jnew(5);
    r->p++; /* { */
    jws(r);
    if (!j) { return NULL; }
    if (r->p < r->end && *r->p == '}') { r->p++; return j; }
    for (;;) {
        char *k;
        jnode_t *v;
        jws(r);
        if (r->p >= r->end || *r->p != '"') { jfree(j); return NULL; }
        k = jstr(r);
        if (!k) { jfree(j); return NULL; }
        jws(r);
        if (r->p >= r->end || *r->p != ':') { free(k); jfree(j); return NULL; }
        r->p++;
        v = jval(r);
        if (!v) { free(k); jfree(j); return NULL; }
        if (jpush(j, k, v) != 0) { free(k); jfree(j); jfree(v); return NULL; }
        jws(r);
        if (r->p < r->end && *r->p == ',') { r->p++; continue; }
        if (r->p < r->end && *r->p == '}') { r->p++; return j; }
        jfree(j);
        return NULL;
    }
}

static jnode_t *jarr(jrd_t *r)
{
    jnode_t *j = jnew(6);
    r->p++; /* [ */
    jws(r);
    if (!j) { return NULL; }
    if (r->p < r->end && *r->p == ']') { r->p++; return j; }
    for (;;) {
        jnode_t *v = jval(r);
        if (!v || jpush(j, NULL, v) != 0) { jfree(j); jfree(v); return NULL; }
        jws(r);
        if (r->p < r->end && *r->p == ',') { r->p++; continue; }
        if (r->p < r->end && *r->p == ']') { r->p++; return j; }
        jfree(j);
        return NULL;
    }
}

static jnode_t *jval(jrd_t *r)
{
    jnode_t *j;
    jws(r);
    if (r->p >= r->end) { return NULL; }
    if (++r->depth > 32) { r->depth--; return NULL; }
    j = NULL;
    if (*r->p == '{') { j = jobj(r); }
    else if (*r->p == '[') { j = jarr(r); }
    else if (*r->p == '"') {
        char *s = jstr(r);
        j = s ? jnew(4) : NULL;
        if (j) { j->s = s; } else { free(s); }
    } else if ((*r->p >= '0' && *r->p <= '9') || *r->p == '-') {
        char *e;
        j = jnew(3);
        if (j) { j->num = strtod(r->p, &e); r->p = e < r->end ? e : r->end; }
    } else if (r->end - r->p >= 4 && memcmp(r->p, "true", 4) == 0) {
        j = jnew(2);
        r->p += 4;
    } else if (r->end - r->p >= 5 && memcmp(r->p, "false", 5) == 0) {
        j = jnew(1);
        r->p += 5;
    } else if (r->end - r->p >= 4 && memcmp(r->p, "null", 4) == 0) {
        j = jnew(0);
        r->p += 4;
    }
    r->depth--;
    return j;
}

static jnode_t *jparse(const char *d, size_t n)
{
    jrd_t r;
    jnode_t *j;
    r.p = d;
    r.end = d + n;
    r.depth = 0;
    r.err = 0;
    j = jval(&r);
    if (!j) { return NULL; }
    jws(&r);
    if (r.p != r.end) { jfree(j); return NULL; }
    return j;
}

static const jnode_t *jget(const jnode_t *j, const char *k)
{
    int i;
    if (!j || j->kind != 5) { return NULL; }
    for (i = 0; i < j->n; i++) {
        if (strcmp(j->keys[i], k) == 0) { return j->vals[i]; }
    }
    return NULL;
}

static const char *jstrv(const jnode_t *j, const char *k)
{
    const jnode_t *v = jget(j, k);
    return (v && v->kind == 4) ? v->s : NULL;
}

static int jintv(const jnode_t *j, const char *k, int dflt)
{
    const jnode_t *v = jget(j, k);
    return (v && v->kind == 3) ? (int)v->num : dflt;
}

/* ------------------------------------------------------------------ */
/* Inflate (nur Startpfad: JSON aus Datei-Paketen)                     */
/* ------------------------------------------------------------------ */
typedef struct {
    const unsigned char *p, *end;
    unsigned bitbuf;
    int bitn;
} bits_t;

static unsigned bneed(bits_t *b, int n)
{
    while (b->bitn < n) {
        unsigned c = 0;
        if (b->p < b->end) { c = *b->p++; }
        b->bitbuf |= c << b->bitn;
        b->bitn += 8;
    }
    return b->bitbuf;
}

static unsigned btake(bits_t *b, int n)
{
    unsigned v = bneed(b, n) & ((n == 32) ? 0xFFFFFFFFu : ((1u << n) - 1u));
    b->bitbuf >>= n;
    b->bitn -= n;
    return v;
}

typedef struct { short fast[512]; unsigned short *extra; int n; } huff_t;
/* Kanonischer Huffman aus Laengen: fast-Tabelle (9 Bit) + Rest linear. */
static int hbuild(huff_t *h, const unsigned char *len, int n)
{
    unsigned cnt[16] = { 0 }, code = 0;
    int i, sym = 0;
    int *tmp;
    for (i = 0; i < n; i++) { if (len[i] < 16) { cnt[len[i]]++; } }
    cnt[0] = 0;
    for (i = 0; i < 512; i++) { h->fast[i] = -1; }
    tmp = (int *)malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
    if (!tmp) { return -1; }
    for (i = 1; i <= 15; i++) { /* Symbole je Laenge, aufsteigend */
        int k;
        for (k = 0; k < n; k++) {
            if (len[k] == (unsigned)i) { tmp[sym++] = k; }
        }
    }
    sym = 0;
    code = 0;
    for (i = 1; i <= 15; i++) {
        unsigned j;
        for (j = 0; j < cnt[i]; j++) {
            unsigned c = code++, rev = 0, k;
            int s = tmp[sym++];
            for (k = 0; k < (unsigned)i; k++) { /* bitumgekehrt (LSB-zuerst) */
                rev = (rev << 1) | ((c >> k) & 1u);
            }
            if (i <= 9) {
                unsigned f;
                for (f = 0; f < (1u << (9 - i)); f++) {
                    h->fast[rev | (f << i)] = (short)((s << 4) | i);
                }
            }
            /* Laengen > 9 Bit: langsamer Pfad in hsym. */
        }
        code <<= 1;
    }
    free(tmp);
    h->extra = NULL;
    h->n = n;
    return 0;
}

/* Ein Symbol lesen (fast 9 Bit, sonst bitweise mit Zaehltabelle). */
static int hsym(bits_t *b, huff_t *h, const unsigned char *len, int n)
{
    unsigned v = bneed(b, 9) & 511u;
    short f = h->fast[v];
    if (f >= 0) {
        btake(b, f & 15);
        return f >> 4;
    }
    /* Langsam: Code bitweise aufbauen und gegen alle Laengen pruefen. */
    {
        unsigned code = 0, first = 0, idx = 0, i;
        int bl;
        unsigned cnt[16] = { 0 }, base[16];
        int sym = -1;
        for (i = 0; i < (unsigned)n; i++) { if (len[i] < 16) { cnt[len[i]]++; } }
        base[1] = 0;
        for (bl = 1; bl < 15; bl++) { base[bl + 1] = (base[bl] + cnt[bl]) << 1; }
        for (bl = 1; bl <= 15; bl++) {
            code |= btake(b, 1) << (bl - 1);
            /* Bit-Reihenfolge: MSB-zuerst vergleichen -> umdrehen */
            {
                unsigned rev = 0;
                int k;
                for (k = 0; k < bl; k++) { rev = (rev << 1) | ((code >> k) & 1u); }
                if (rev >= base[bl] && rev < base[bl] + cnt[bl]) {
                    unsigned ord = rev - base[bl];
                    for (i = 0; i < (unsigned)n; i++) {
                        if (len[i] == (unsigned)bl && ord-- == 0) {
                            sym = (int)i;
                            break;
                        }
                    }
                    return sym;
                }
            }
            (void)first;
            (void)idx;
        }
        return -1;
    }
}

/* Bloecke entpacken. Rueckgabe: Bytezahl oder (size_t)-1. */
static size_t inflate_raw(const unsigned char *in, size_t nin,
                          unsigned char **out, size_t maxout)
{
    static const unsigned short lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
    static const unsigned char lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
        4, 4, 4, 4, 5, 5, 5, 5, 0 };
    static const unsigned short dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
    static const unsigned char dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
        9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
    static const unsigned char dord[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    bits_t b;
    size_t cap = 65536, n = 0;
    unsigned char *o;
    int fin = 0;
    b.p = in;
    b.end = in + nin;
    b.bitbuf = 0;
    b.bitn = 0;
    o = (unsigned char *)malloc(cap);
    if (!o) { return (size_t)-1; }
    while (!fin) {
        huff_t lh, dh;
        unsigned char llen[288], dlen[32];
        int nl, nd, s;
        fin = (int)btake(&b, 1);
        switch (btake(&b, 2)) {
        case 0: { /* stored */
            unsigned len, inv;
            b.bitbuf = 0;
            b.bitn = 0;
            if (b.end - b.p < 4) { free(o); return (size_t)-1; }
            len = b.p[0] | (b.p[1] << 8);
            inv = b.p[2] | (b.p[3] << 8);
            b.p += 4;
            if ((len ^ inv) != 0xFFFFu || (size_t)(b.end - b.p) < len) { free(o); return (size_t)-1; }
            while (n + len > cap) {
                unsigned char *q;
                if (cap >= maxout) { free(o); return (size_t)-1; }
                cap *= 2;
                q = (unsigned char *)realloc(o, cap);
                if (!q) { free(o); return (size_t)-1; }
                o = q;
            }
            memcpy(o + n, b.p, len);
            n += len;
            b.p += len;
            continue;
        }
        case 1: /* fixed */
            for (s = 0; s < 144; s++) { llen[s] = 8; }
            for (; s < 256; s++) { llen[s] = 9; }
            for (; s < 280; s++) { llen[s] = 7; }
            for (; s < 288; s++) { llen[s] = 8; }
            for (s = 0; s < 32; s++) { dlen[s] = 5; }
            nl = 288;
            nd = 32;
            break;
        case 2: { /* dynamic */
            unsigned char clen[19];
            int ncl, i = 0;
            nl = (int)btake(&b, 5) + 257;
            nd = (int)btake(&b, 5) + 1;
            ncl = (int)btake(&b, 4) + 4;
            if (nl > 288 || nd > 32) { free(o); return (size_t)-1; }
            memset(clen, 0, sizeof clen);
            for (s = 0; s < ncl; s++) { clen[dord[s]] = (unsigned char)btake(&b, 3); }
            {
                huff_t ch;
                if (hbuild(&ch, clen, 19) != 0) { free(o); return (size_t)-1; }
                while (i < nl + nd) {
                    int v = hsym(&b, &ch, clen, 19);
                    if (v < 0) { free(ch.extra); free(o); return (size_t)-1; }
                    if (v < 16) {
                        if (i < nl) { llen[i] = (unsigned char)v; }
                        else { dlen[i - nl] = (unsigned char)v; }
                        i++;
                    } else if (v == 16) {
                        int rep = (int)btake(&b, 2) + 3, pv;
                        if (i == 0) { free(ch.extra); free(o); return (size_t)-1; }
                        pv = i - 1 < nl ? llen[i - 1] : dlen[i - 1 - nl];
                        while (rep-- > 0 && i < nl + nd) {
                            if (i < nl) { llen[i] = (unsigned char)pv; }
                            else { dlen[i - nl] = (unsigned char)pv; }
                            i++;
                        }
                    } else {
                        int rep = (v == 17) ? (int)btake(&b, 3) + 3 : (int)btake(&b, 7) + 11;
                        while (rep-- > 0 && i < nl + nd) {
                            if (i < nl) { llen[i] = 0; }
                            else { dlen[i - nl] = 0; }
                            i++;
                        }
                    }
                }
                free(ch.extra);
            }
            break;
        }
        default:
            free(o);
            return (size_t)-1;
        }
        if (hbuild(&lh, llen, nl) != 0 || hbuild(&dh, dlen, nd) != 0) {
            free(lh.extra);
            free(dh.extra);
            free(o);
            return (size_t)-1;
        }
        for (;;) {
            int v = hsym(&b, &lh, llen, nl);
            if (v < 0) { free(lh.extra); free(dh.extra); free(o); return (size_t)-1; }
            if (v < 256) {
                if (n + 1 > cap) {
                    unsigned char *q;
                    if (cap >= maxout) { free(lh.extra); free(dh.extra); free(o); return (size_t)-1; }
                    cap *= 2;
                    q = (unsigned char *)realloc(o, cap);
                    if (!q) { free(lh.extra); free(dh.extra); free(o); return (size_t)-1; }
                    o = q;
                }
                o[n++] = (unsigned char)v;
            } else if (v == 256) {
                break;
            } else if (v < 286) {
                unsigned len = lbase[v - 257], dist;
                int dv;
                if (lext[v - 257]) { len += btake(&b, lext[v - 257]); }
                dv = hsym(&b, &dh, dlen, nd);
                if (dv < 0 || dv >= 30) { free(lh.extra); free(dh.extra); free(o); return (size_t)-1; }
                dist = dbase[dv];
                if (dext[dv]) { dist += btake(&b, dext[dv]); }
                if (dist == 0 || dist > n || n + len > maxout) {
                    free(lh.extra);
                    free(dh.extra);
                    free(o);
                    return (size_t)-1;
                }
                while (n + len > cap) {
                    unsigned char *q;
                    cap *= 2;
                    q = (unsigned char *)realloc(o, cap);
                    if (!q) { free(lh.extra); free(dh.extra); free(o); return (size_t)-1; }
                    o = q;
                }
                while (len-- > 0) { o[n] = o[n - dist]; n++; }
            } else {
                free(lh.extra);
                free(dh.extra);
                free(o);
                return (size_t)-1;
            }
        }
        free(lh.extra);
        free(dh.extra);
    }
    *out = o;
    return n;
}

/* ------------------------------------------------------------------ */
/* ZIP-Verzeichnis (nur Startpfad)                                     */
/* ------------------------------------------------------------------ */
static unsigned u16le(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned u32le(const unsigned char *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24);
}

typedef struct {
    char *name;
    int method;          /* 0 = stored, 8 = deflate, <0 = unbrauchbar */
    unsigned long long lho;  /* Offset des lokalen Kopfs */
    unsigned long long off;  /* Datenoffset (stored: direkt lesbar) */
    unsigned long long csize, usize;
} zipent_t;

static unsigned long long u64le(const unsigned char *p)
{
    return (unsigned long long)u32le(p) | ((unsigned long long)u32le(p + 4) << 32);
}

/* Zentrales Verzeichnis einlesen. Rueckgabe: Eintraege oder NULL. */
static zipent_t *zip_dir(const char *path, int *nout, HANDLE *hdir)
{
    HANDLE h;
    LARGE_INTEGER fs;
    unsigned char *tail = NULL;
    DWORD rd = 0;
    long i, found = -1;
    unsigned long long cnum = 0, coff = 0;
    unsigned char *cd = NULL;
    zipent_t *ents = NULL;
    int n = 0, cap = 0;
    *nout = 0;
    *hdir = INVALID_HANDLE_VALUE;
    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { return NULL; }
    if (!GetFileSizeEx(h, &fs) || fs.QuadPart < 22) {
        CloseHandle(h);
        return NULL;
    }
    {
        DWORD tailn = fs.QuadPart > 70000 ? 70000 : (DWORD)fs.QuadPart;
        LARGE_INTEGER off;
        tail = (unsigned char *)malloc(tailn);
        if (!tail) { CloseHandle(h); return NULL; }
        off.QuadPart = fs.QuadPart - tailn;
        if (!SetFilePointerEx(h, off, NULL, FILE_BEGIN) ||
            !ReadFile(h, tail, tailn, &rd, NULL) || rd != tailn) {
            free(tail);
            CloseHandle(h);
            return NULL;
        }
        for (i = (long)tailn - 22; i >= 0; i--) {
            if (tail[i] == 0x50 && tail[i + 1] == 0x4B &&
                tail[i + 2] == 0x05 && tail[i + 3] == 0x06) {
                unsigned clen = u16le(tail + i + 20);
                if ((long)i + 22 + (long)clen == (long)tailn) { found = i; break; }
            }
        }
        if (found < 0) { free(tail); CloseHandle(h); return NULL; }
        cnum = u16le(tail + found + 10);
        coff = u32le(tail + found + 16);
        /* ZIP64: Kennungs-Locator direkt vor dem EOCD. */
        if ((cnum == 0xFFFFu || coff == 0xFFFFFFFFu) && found >= 20 &&
            tail[found - 20] == 0x50 && tail[found - 19] == 0x4B &&
            tail[found - 18] == 0x06 && tail[found - 17] == 0x07) {
            unsigned long long e64 = u64le(tail + found - 12);
            unsigned char h64[56];
            LARGE_INTEGER lo;
            DWORD r = 0;
            lo.QuadPart = (LONGLONG)e64;
            if (SetFilePointerEx(h, lo, NULL, FILE_BEGIN) &&
                ReadFile(h, h64, 56, &r, NULL) && r == 56 &&
                h64[0] == 0x50 && h64[1] == 0x4B && h64[2] == 0x06 && h64[3] == 0x06) {
                cnum = u64le(h64 + 32);
                coff = u64le(h64 + 48);
            } else {
                free(tail);
                CloseHandle(h);
                return NULL;
            }
        }
        free(tail);
        tail = NULL;
        if (!cnum || coff >= (unsigned long long)fs.QuadPart) {
            CloseHandle(h);
            return NULL;
        }
    }
    { /* Verzeichnis lesen (grob begrenzt) */
        LARGE_INTEGER off;
        unsigned long long left = (unsigned long long)fs.QuadPart - coff, got = 0;
        unsigned char *p;
        if (left > (64u << 20)) { CloseHandle(h); return NULL; }
        cd = (unsigned char *)malloc((size_t)left ? (size_t)left : 1);
        if (!cd) { CloseHandle(h); return NULL; }
        off.QuadPart = (LONGLONG)coff;
        if (!SetFilePointerEx(h, off, NULL, FILE_BEGIN)) { free(cd); CloseHandle(h); return NULL; }
        while (got < left) {
            DWORD r = 0, want = (DWORD)(left - got);
            if (!ReadFile(h, cd + got, want, &r, NULL) || !r) {
                free(cd);
                CloseHandle(h);
                return NULL;
            }
            got += r;
        }
        p = cd;
        while ((unsigned long long)n < cnum && n < 1000000 &&
               (size_t)(p - cd) + 46 <= (size_t)left) {
            unsigned fnl, efl, fcl, method, flags;
            unsigned long long lho, cs, us;
            if (p[0] != 0x50 || p[1] != 0x4B || p[2] != 0x01 || p[3] != 0x02) { break; }
            flags = u16le(p + 8);
            method = u16le(p + 10);
            fnl = u16le(p + 28);
            efl = u16le(p + 30);
            fcl = u16le(p + 32);
            cs = u32le(p + 20);
            us = u32le(p + 24);
            lho = u32le(p + 42);
            if ((size_t)(p - cd) + 46 + fnl + efl + fcl > (size_t)left) { break; }
            if ((cs == 0xFFFFFFFFu || us == 0xFFFFFFFFu || lho == 0xFFFFFFFFu) && efl >= 4) {
                /* ZIP64-Extrafeld 0x0001: usize, csize, lho je 8 Byte. */
                const unsigned char *x = p + 46 + fnl, *xe = x + efl;
                while (x + 4 <= xe) {
                    unsigned tag = u16le(x), sz = u16le(x + 2);
                    const unsigned char *d;
                    if (x + 4 + sz > xe) { break; }
                    d = x + 4;
                    if (tag == 0x0001) {
                        if (us == 0xFFFFFFFFu && sz >= 8) { us = u64le(d); d += 8; sz -= 8; }
                        if (cs == 0xFFFFFFFFu && sz >= 8) { cs = u64le(d); d += 8; sz -= 8; }
                        if (lho == 0xFFFFFFFFu && sz >= 8) { lho = u64le(d); }
                        break;
                    }
                    x += 4 + sz;
                }
            }
            if (n >= cap) {
                int nc = cap ? cap * 2 : 256;
                zipent_t *q = (zipent_t *)realloc(ents, sizeof *ents * (size_t)nc);
                if (!q) { break; }
                ents = q;
                cap = nc;
            }
            ents[n].name = (char *)malloc(fnl + 1);
            if (!ents[n].name) { break; }
            memcpy(ents[n].name, p + 46, fnl);
            ents[n].name[fnl] = '\0';
            ents[n].method = (flags & 1) ? -1 : (int)method;
            ents[n].csize = cs;
            ents[n].usize = us;
            ents[n].lho = lho;
            ents[n].off = 0;
            /* Datenoffset aus dem lokalen Kopf (stored: direkt lesbar). */
            if (ents[n].method == 0) {
                unsigned char lh[30];
                LARGE_INTEGER lo;
                DWORD r = 0;
                lo.QuadPart = (LONGLONG)lho;
                if (SetFilePointerEx(h, lo, NULL, FILE_BEGIN) &&
                    ReadFile(h, lh, 30, &r, NULL) && r == 30 &&
                    lh[0] == 0x50 && lh[1] == 0x4B && lh[2] == 0x03 && lh[3] == 0x04) {
                    ents[n].off = lho + 30u + u16le(lh + 26) + u16le(lh + 28);
                } else {
                    ents[n].method = -2; /* Kopf unlesbar */
                }
            }
            n++;
            p += 46 + fnl + efl + fcl;
        }
        free(cd);
    }
    *nout = n;
    *hdir = h; /* bleibt offen: ReadFile auf Offset im Spielpfad */
    return ents;
}

/* Eintragsbytes lesen (stored direkt, deflate per Inflate). Startpfad. */
static int zip_get(HANDLE h, const zipent_t *e, unsigned char **out, size_t *nout)
{
    unsigned char lh[30], *cbuf = NULL, *o = NULL;
    DWORD r = 0;
    unsigned long long got = 0;
    LARGE_INTEGER lo;
    unsigned long long data;
    *out = NULL;
    *nout = 0;
    if ((e->method != 0 && e->method != 8)) { return 0; }
    if (e->csize > (64u << 20) || e->usize > (64u << 20)) { return 0; }
    if (e->method == 0 && e->off) {
        data = e->off;
    } else {
        lo.QuadPart = (LONGLONG)e->lho;
        if (!SetFilePointerEx(h, lo, NULL, FILE_BEGIN) ||
            !ReadFile(h, lh, 30, &r, NULL) || r != 30 ||
            lh[0] != 0x50 || lh[1] != 0x4B || lh[2] != 0x03 || lh[3] != 0x04) {
            return 0;
        }
        data = e->lho + 30u + u16le(lh + 26) + u16le(lh + 28);
    }
    lo.QuadPart = (LONGLONG)data;
    if (!SetFilePointerEx(h, lo, NULL, FILE_BEGIN)) { return 0; }
    cbuf = (unsigned char *)malloc((size_t)e->csize + 1);
    if (!cbuf) { return 0; }
    cbuf[e->csize] = '\0'; /* JSON (strtod) braucht NUL */
    while (got < e->csize) {
        DWORD want = (DWORD)(e->csize - got);
        if (!ReadFile(h, cbuf + got, want, &r, NULL) || !r) {
            free(cbuf);
            return 0;
        }
        got += r;
    }
    if (e->method == 0) {
        *out = cbuf;
        *nout = (size_t)e->csize;
        return 1;
    }
    {
        size_t n = inflate_raw(cbuf, (size_t)e->csize, &o, (size_t)e->usize + 16);
        free(cbuf);
        if (n == (size_t)-1 || n != (size_t)e->usize) { free(o); return 0; }
        *out = o;
        *nout = n;
        return 1;
    }
}

/* ------------------------------------------------------------------ */
/* Pakete + Index                                                      */
/* ------------------------------------------------------------------ */
#define PK_MAX 32
#define IDX_HN 16384

typedef struct {
    u64 hash;
    int pri;        /* 0 = normal, 1 = fallback */
    char *cat;      /* Kategorie (malloc) */
    char *f2, *f4;  /* Variant-Dateien im Paket (malloc, NULL = fehlt) */
} pkent_t;

typedef struct {
    char id[128], name[128], version[32], src[600];
    int is_zip;
    char root[600]; /* Ordner-Paket: Wurzel; Datei-Paket: Pfad */
    HANDLE h;       /* ZIP-Handle (offen) */
    zipent_t *zent;
    int nzent;
    pkent_t *ents;
    int nents;
    int ndrop;      /* verworfene Varianten (Datei fehlt, Pfad, Methode) */
} pack_t;

typedef struct {
    int used;
    u64 hash;
    int pack;       /* gewaehltes Paket */
    int cap;        /* min(category_level, overrides), ohne globale Stufe */
    int has2, has4;
    char *path2, *path4; /* Ordner: voller Pfad; ZIP: Anzeige "id!datei" */
    unsigned long long off2, size2, off4, size4;
} idxent_t;

static pack_t g_pk[PK_MAX];
static int g_npk;
static idxent_t *g_idx;
static int *g_idxh;
static int g_nidx, g_avail;
static int g_inited;

typedef struct { char *name; int cap; int hit; } catcap_t;
static catcap_t *g_catcap;
static int g_ncatcap;
typedef struct { u64 hash; int cap; int hit; } over_t;
static over_t *g_over;
static int g_nover;

static int hex16(const char *s, u64 *out)
{
    int i;
    u64 h = 0;
    if (!s) { return 0; }
    for (i = 0; i < 16; i++) {
        char c = s[i];
        int x;
        if (!c) { return 0; }
        if (c >= '0' && c <= '9') { x = c - '0'; }
        else if (c >= 'a' && c <= 'f') { x = c - 'a' + 10; }
        else if (c >= 'A' && c <= 'F') { x = c - 'A' + 10; }
        else { return 0; }
        h = (h << 4) | (u64)x;
    }
    if (s[16]) { return 0; }
    *out = h;
    return 1;
}

/* Pfad im Paket pruefen (kein .., absolut, Laufwerk). 1 = ok. */
static int pk_path_ok(const char *s)
{
    size_t i, n;
    if (!s || !s[0] || strlen(s) > 200) { return 0; }
    if (s[0] == '/' || s[0] == '\\') { return 0; }
    if (strlen(s) > 1 && s[1] == ':') { return 0; }
    if (strstr(s, "..")) { return 0; }
    n = strlen(s);
    for (i = 0; i < n; i++) {
        if (s[i] == '\\') { return 0; }
    }
    return 1;
}

static int read_small(const char *path, unsigned char **out, size_t *nout,
                      size_t maxb)
{
    FILE *f = NULL;
    long sz;
    *out = NULL;
    *nout = 0;
    if (fopen_s(&f, path, "rb") != 0 || !f) { return 0; }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || (size_t)sz > maxb) { fclose(f); return 0; }
    *out = (unsigned char *)malloc((size_t)sz + 1);
    if (!*out) { fclose(f); return 0; }
    *nout = fread(*out, 1, (size_t)sz, f);
    fclose(f);
    if (*nout != (size_t)sz) { free(*out); *out = NULL; *nout = 0; return 0; }
    (*out)[sz] = '\0';
    return 1;
}

static const zipent_t *zip_find(const pack_t *p, const char *name)
{
    int i;
    for (i = 0; i < p->nzent; i++) {
        if (strcmp(p->zent[i].name, name) == 0) { return &p->zent[i]; }
    }
    return NULL;
}

static int lvl_of(const char *s)
{
    if (!s) { return 4; }
    if (strcmp(s, "orig") == 0) { return 0; }
    if (strcmp(s, "x2") == 0) { return 2; }
    return 4;
}

/* Manifest-Eintraege eines Pakets lesen. 1 = ok (ggf. 0 Eintraege). */
static int pack_entries(pack_t *p)
{
    unsigned char *mj = NULL;
    size_t mn = 0;
    jnode_t *m = NULL;
    const jnode_t *arr = NULL;
    jnode_t *ef = NULL;
    int i, cap = 0;
    char mp[700];
    if (p->is_zip) {
        const zipent_t *e = zip_find(p, "manifest.json");
        if (!e || !zip_get(p->h, e, &mj, &mn)) { return 0; }
    } else {
        _snprintf_s(mp, sizeof mp, _TRUNCATE, "%s/manifest.json", p->root);
        if (!read_small(mp, &mj, &mn, 16u << 20)) { return 0; }
    }
    m = jparse((const char *)mj, mn);
    free(mj);
    if (!m) { return 0; }
    {
        const char *fmt = jstrv(m, "format");
        const char *ha = jstrv(m, "hash_algo");
        const char *id = jstrv(m, "id");
        const char *nm = jstrv(m, "name");
        const char *vs = jstrv(m, "version");
        if (!fmt || strcmp(fmt, "sbpack") != 0 || jintv(m, "format_version", 0) != 1 ||
            !ha || strcmp(ha, "sbt1-fnv64") != 0 || !id || !id[0]) {
            jfree(m);
            jfree(ef);
            return 0;
        }
        strncpy_s(p->id, sizeof p->id, id, _TRUNCATE);
        strncpy_s(p->name, sizeof p->name, nm ? nm : id, _TRUNCATE);
        strncpy_s(p->version, sizeof p->version, vs ? vs : "?", _TRUNCATE);
    }
    arr = jget(m, "entries");
    if (!arr) {
        const char *efn = jstrv(m, "entries_file");
        unsigned char *ej = NULL;
        size_t en = 0;
        if (!efn || !pk_path_ok(efn)) { jfree(m); return 0; }
        if (p->is_zip) {
            const zipent_t *e = zip_find(p, efn);
            if (!e || !zip_get(p->h, e, &ej, &en)) { jfree(m); return 0; }
        } else {
            _snprintf_s(mp, sizeof mp, _TRUNCATE, "%s/%s", p->root, efn);
            if (!read_small(mp, &ej, &en, 16u << 20)) { jfree(m); return 0; }
        }
        ef = jparse((const char *)ej, en);
        free(ej);
        if (!ef) { jfree(m); return 0; }
        arr = jget(ef, "entries");
    }
    if (!arr || arr->kind != 6) { jfree(m); jfree(ef); return 0; }
    for (i = 0; i < arr->n; i++) {
        const jnode_t *e = arr->items[i], *var, *v2, *v4;
        u64 hv;
        const char *hs, *pri, *cat;
        pkent_t *d;
        if (!e || e->kind != 5) { continue; }
        hs = jstrv(e, "hash");
        if (!hex16(hs, &hv)) { continue; }
        var = jget(e, "variants");
        if (!var || var->kind != 5) { continue; }
        /* Duplikat im Paket: erstes gewinnt (validate meldet rot). */
        {
            int k, dup = 0;
            for (k = 0; k < p->nents; k++) {
                if (p->ents[k].hash == hv) { dup = 1; break; }
            }
            if (dup) { continue; }
        }
        if (p->nents >= cap) {
            int nc = cap ? cap * 2 : 512;
            pkent_t *q = (pkent_t *)realloc(p->ents, sizeof *q * (size_t)nc);
            if (!q) { break; }
            memset(q + cap, 0, sizeof *q * (size_t)(nc - cap));
            p->ents = q;
            cap = nc;
        }
        d = &p->ents[p->nents++];
        d->hash = hv;
        pri = jstrv(e, "priority");
        d->pri = (pri && strcmp(pri, "fallback") == 0) ? 1 : 0;
        cat = jstrv(e, "category");
        if (cat && cat[0]) {
            d->cat = (char *)malloc(strlen(cat) + 1);
            if (d->cat) { strcpy_s(d->cat, strlen(cat) + 1, cat); }
        }
        v2 = jget(var, "x2");
        v4 = jget(var, "x4");
        if (v2 && v2->kind == 5) {
            const char *fn = jstrv(v2, "file");
            if (fn && pk_path_ok(fn)) {
                d->f2 = (char *)malloc(strlen(fn) + 1);
                if (d->f2) { strcpy_s(d->f2, strlen(fn) + 1, fn); }
            } else {
                p->ndrop++;
            }
        }
        if (v4 && v4->kind == 5) {
            const char *fn = jstrv(v4, "file");
            if (fn && pk_path_ok(fn)) {
                d->f4 = (char *)malloc(strlen(fn) + 1);
                if (d->f4) { strcpy_s(d->f4, strlen(fn) + 1, fn); }
            } else {
                p->ndrop++;
            }
        }
    }
    jfree(m);
    jfree(ef);
    return 1;
}

/* Variant-Dateien pruefen (Startpfad): Ordner = vorhanden, ZIP = stored. */
static void pack_check_files(pack_t *p)
{
    int i;
    for (i = 0; i < p->nents; i++) {
        pkent_t *d = &p->ents[i];
        char **slots[2] = { &d->f2, &d->f4 };
        int s;
        for (s = 0; s < 2; s++) {
            char *fn = *slots[s];
            if (!fn) { continue; }
            if (p->is_zip) {
                const zipent_t *e = zip_find(p, fn);
                if (!e || e->method != 0) {
                    free(fn);
                    *slots[s] = NULL;
                    p->ndrop++;
                }
            } else {
                char fp[1200];
                _snprintf_s(fp, sizeof fp, _TRUNCATE, "%s/%s", p->root, fn);
                if (GetFileAttributesA(fp) == INVALID_FILE_ATTRIBUTES) {
                    free(fn);
                    *slots[s] = NULL;
                    p->ndrop++;
                }
            }
        }
    }
}

static int idx_slot(u64 hv, int add)
{
    unsigned s = (unsigned)(hv ^ (hv >> 29)) & (IDX_HN - 1u);
    unsigned steps = 0;
    while (g_idxh[s] >= 0) {
        if (g_idx[g_idxh[s]].hash == hv) { return g_idxh[s]; }
        s = (s + 1u) & (IDX_HN - 1u);
        if (++steps >= IDX_HN) { return -1; }
    }
    if (!add || g_nidx >= IDX_HN) { return -1; }
    memset(&g_idx[g_nidx], 0, sizeof g_idx[g_nidx]);
    g_idx[g_nidx].hash = hv;
    g_idxh[s] = g_nidx;
    return g_nidx++;
}

static int cat_cap(const char *cat)
{
    int i;
    if (!cat) { return 4; }
    for (i = 0; i < g_ncatcap; i++) {
        if (strcmp(g_catcap[i].name, cat) == 0) { g_catcap[i].hit = 1; return g_catcap[i].cap; }
    }
    return 4;
}

static int over_cap(u64 hv)
{
    int i;
    for (i = 0; i < g_nover; i++) {
        if (g_over[i].hash == hv) { g_over[i].hit = 1; return g_over[i].cap; }
    }
    return 4;
}

/* ------------------------------------------------------------------ */
/* Effect-Pakete (W151, PACK_FORMAT.md Abschnitt 9)                     */
/* ------------------------------------------------------------------ */
/* Positivliste der Effekt-Parameter (params.json-Schluessel = cfg-Keys).
 * Einzige Quelle im Client; sbpack.py spiegelt sie zur Pruefung (V13/V14),
 * sb_packscan.c nutzt sie fuer Pruefen + Anzeige. */
const sb_fxparam_t kSbFxParams[SB_FXN + 1] = {
    { "fx_soft", 0, 200 },
    { "fx_embers", 0, 200 },
    { "fx_light", 0, 200 },
    { "fx_light_cockpit", 0, 200 },
    { "fx_interp", 0, 1 },
    { "bloom_threshold", 0, 90 },
    { "color_sat", 0, 200 },
    { "color_temp", -100, 100 },
    { "color_lift", -50, 50 },
    { "color_gamma", 50, 200 },
    { "color_gain", 50, 200 },
    { NULL, 0, 0 }
};

static int fx_idx(const char *k)
{
    int i;
    for (i = 0; i < SB_FXN; i++) {
        if (strcmp(k, kSbFxParams[i].key) == 0) { return i; }
    }
    return -1;
}

/* Datei aus Ordner- oder ZIP-Paket lesen (Startpfad). 1 = ok. */
static int fx_read(pack_t *p, const char *name, unsigned char **out,
                   size_t *nout)
{
    char mp[700];
    if (p->is_zip) {
        const zipent_t *e = zip_find(p, name);
        if (!e || !zip_get(p->h, e, out, nout)) { return 0; }
        return 1;
    }
    _snprintf_s(mp, sizeof mp, _TRUNCATE, "%s/%s", p->root, name);
    return read_small(mp, out, nout, 16u << 20);
}

static void fx_close(pack_t *p)
{
    int k;
    if (p->h != INVALID_HANDLE_VALUE) { CloseHandle(p->h); p->h = INVALID_HANDLE_VALUE; }
    for (k = 0; k < p->nzent; k++) { free(p->zent[k].name); }
    free(p->zent);
    p->zent = NULL;
    p->nzent = 0;
}

int sb_pack_fxparams(const char *packs_json, int *out, int *hit,
                     char (*src)[128])
{
    unsigned char *pj = NULL;
    size_t pn = 0;
    jnode_t *j = NULL;
    const jnode_t *order;
    char dir[600], *s;
    int i, nwarn = 0;
    if (!out || !hit) { return 0; }
    for (i = 0; i < SB_FXN; i++) {
        hit[i] = 0;
        out[i] = 0;
        if (src) { src[i][0] = '\0'; }
    }
    if (!packs_json || !packs_json[0]) { return 1; }
    if (!read_small(packs_json, &pj, &pn, 1u << 20)) { return 1; }
    j = jparse((const char *)pj, pn);
    free(pj);
    if (!j) { return 1; }
    strncpy_s(dir, sizeof dir, packs_json, _TRUNCATE);
    for (s = dir; *s; s++) { if (*s == '\\') { *s = '/'; } }
    s = strrchr(dir, '/');
    if (s) { *s = '\0'; } else { strcpy_s(dir, sizeof dir, "."); }
    order = jget(j, "order");
    if (order && order->kind == 6) {
        for (i = 0; i < order->n; i++) {
            const jnode_t *o = order->items[i];
            const jnode_t *en, *pp;
            const char *fn, *ty;
            char full[700], id[128];
            DWORD a;
            pack_t p;
            unsigned char *mj = NULL, *qj = NULL;
            size_t mn = 0, qn = 0;
            jnode_t *m = NULL, *q = NULL;
            int k;
            if (!o || o->kind != 5) { continue; }
            en = jget(o, "enabled");
            if (en && en->kind == 1) { continue; }
            fn = jstrv(o, "file");
            if (!fn || !fn[0]) { continue; }
            memset(&p, 0, sizeof p);
            p.h = INVALID_HANDLE_VALUE;
            if ((strlen(fn) > 1 && fn[1] == ':') || fn[0] == '/' || fn[0] == '\\') {
                strncpy_s(full, sizeof full, fn, _TRUNCATE);
            } else {
                _snprintf_s(full, sizeof full, _TRUNCATE, "%s/%s", dir, fn);
            }
            a = GetFileAttributesA(full);
            if (a == INVALID_FILE_ATTRIBUTES) { continue; }
            if (a & FILE_ATTRIBUTE_DIRECTORY) {
                p.is_zip = 0;
                strncpy_s(p.root, sizeof p.root, full, _TRUNCATE);
            } else {
                int nz = 0;
                HANDLE h = INVALID_HANDLE_VALUE;
                zipent_t *ze = zip_dir(full, &nz, &h);
                if (!ze || !nz) {
                    if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); }
                    free(ze);
                    continue;
                }
                p.is_zip = 1;
                strncpy_s(p.root, sizeof p.root, full, _TRUNCATE);
                p.h = h;
                p.zent = ze;
                p.nzent = nz;
            }
            if (!fx_read(&p, "manifest.json", &mj, &mn)) { fx_close(&p); continue; }
            m = jparse((const char *)mj, mn);
            free(mj);
            ty = m ? jstrv(m, "type") : NULL;
            {
                const char *mid = m ? jstrv(m, "id") : NULL;
                strncpy_s(id, sizeof id, mid && mid[0] ? mid : fn, _TRUNCATE);
            }
            if (!m || !ty || strcmp(ty, "effect") != 0) { jfree(m); fx_close(&p); continue; }
            jfree(m);
            m = NULL;
            if (!fx_read(&p, "params.json", &qj, &qn)) { fx_close(&p); continue; }
            q = jparse((const char *)qj, qn);
            free(qj);
            pp = q ? jget(q, "params") : NULL;
            if (!q || !pp || pp->kind != 5) {
                HLOG_W("PACK-FX: %s: params.json unlesbar (ignoriert)", id);
                jfree(q);
                fx_close(&p);
                continue;
            }
            for (k = 0; k < pp->n; k++) {
                int fi = fx_idx(pp->keys[k] ? pp->keys[k] : "");
                const jnode_t *v;
                int iv;
                if (fi < 0) {
                    if (nwarn < 8) {
                        HLOG_W("PACK-FX: %s: unbekannter Schluessel \"%s\" ignoriert",
                               id, pp->keys[k] ? pp->keys[k] : "?");
                    }
                    nwarn++;
                    continue;
                }
                if (hit[fi]) { continue; } /* hoeheres Paket gewann */
                v = pp->vals[k];
                if (!v || v->kind != 3) {
                    if (nwarn < 8) {
                        HLOG_W("PACK-FX: %s: %s kein Zahlenwert (ignoriert)", id,
                               kSbFxParams[fi].key);
                    }
                    nwarn++;
                    continue;
                }
                iv = (int)v->num; /* Rumpf wie sbpack (Wertebereich s. PACK_FORMAT) */
                if (iv < kSbFxParams[fi].min || iv > kSbFxParams[fi].max) {
                    int cl = iv < kSbFxParams[fi].min ? kSbFxParams[fi].min : kSbFxParams[fi].max;
                    HLOG_W("PACK-FX: %s: %s=%d -> %d geklemmt", id,
                           kSbFxParams[fi].key, iv, cl);
                    iv = cl;
                }
                out[fi] = iv;
                hit[fi] = 1;
                if (src) { strncpy_s(src[fi], 128, id, _TRUNCATE); }
            }
            jfree(q);
            fx_close(&p);
        }
    }
    jfree(j);
    if (nwarn > 8) {
        HLOG_W("PACK-FX: ... und %d weitere Warnungen (Deckel)", nwarn - 8);
    }
    return 1;
}

int sb_pack_init(const char *packs_json, const char *rdir)
{
    unsigned char *pj = NULL;
    size_t pn = 0;
    jnode_t *j = NULL;
    const jnode_t *order, *cc, *ov;
    char dir[600], *s;
    int i, norder = 0, nactive = 0, n2 = 0, n4 = 0;
    const char *lvl;
    if (g_inited) { return 1; }
    g_inited = 1;
    g_idx = (idxent_t *)calloc(IDX_HN, sizeof *g_idx);
    g_idxh = (int *)malloc(sizeof(int) * IDX_HN);
    if (!g_idx || !g_idxh) { free(g_idx); free(g_idxh); g_idx = NULL; g_idxh = NULL; return 0; }
    for (i = 0; i < IDX_HN; i++) { g_idxh[i] = -1; }
    if (!packs_json || !packs_json[0]) {
        HLOG_I("PACK: pack_list leer -> keine Pakete (Original)");
        return 1;
    }
    if (!read_small(packs_json, &pj, &pn, 1u << 20)) {
        HLOG_W("PACK: %s nicht lesbar -> keine Pakete (Original)", packs_json);
        return 1;
    }
    j = jparse((const char *)pj, pn);
    free(pj);
    if (!j) {
        HLOG_W("PACK: %s kein JSON -> keine Pakete (Original)", packs_json);
        return 1;
    }
    strncpy_s(dir, sizeof dir, packs_json, _TRUNCATE);
    for (s = dir; *s; s++) { if (*s == '\\') { *s = '/'; } }
    s = strrchr(dir, '/');
    if (s) { *s = '\0'; } else { strcpy_s(dir, sizeof dir, "."); }
    (void)rdir;
    order = jget(j, "order");
    lvl = jstrv(j, "level");
    HLOG_I("PACK liste: %s level=%s (Hinweis: Stufe kommt aus hd_scale)", packs_json, lvl ? lvl : "?");
    cc = jget(j, "category_level");
    if (cc && cc->kind == 5) {
        for (i = 0; i < cc->n; i++) {
            catcap_t *q;
            if (!cc->keys[i] || !cc->vals[i] || cc->vals[i]->kind != 4) { continue; }
            q = (catcap_t *)realloc(g_catcap, sizeof *q * (size_t)(g_ncatcap + 1));
            if (!q) { break; }
            g_catcap = q;
            g_catcap[g_ncatcap].name = cc->keys[i];
            cc->keys[i] = NULL; /* Besitz uebernommen */
            g_catcap[g_ncatcap].cap = lvl_of(cc->vals[i]->s);
            g_catcap[g_ncatcap].hit = 0;
            g_ncatcap++;
        }
    }
    ov = jget(j, "overrides");
    if (ov && ov->kind == 5) {
        for (i = 0; i < ov->n; i++) {
            over_t *q;
            u64 hv;
            if (!ov->keys[i] || !ov->vals[i] || ov->vals[i]->kind != 4) { continue; }
            if (!hex16(ov->keys[i], &hv)) { continue; }
            q = (over_t *)realloc(g_over, sizeof *q * (size_t)(g_nover + 1));
            if (!q) { break; }
            g_over = q;
            g_over[g_nover].hash = hv;
            g_over[g_nover].cap = lvl_of(ov->vals[i]->s);
            g_over[g_nover].hit = 0;
            g_nover++;
        }
    }
    if (order && order->kind == 6) {
        for (i = 0; i < order->n && g_npk < PK_MAX; i++) {
            const jnode_t *o = order->items[i];
            const jnode_t *en;
            const char *fn, *id;
            pack_t *p;
            char full[700];
            DWORD a;
            if (!o || o->kind != 5) { continue; }
            en = jget(o, "enabled");
            if (en && en->kind == 1) { continue; }
            fn = jstrv(o, "file");
            id = jstrv(o, "id");
            if (!fn || !fn[0]) { continue; }
            norder++;
            p = &g_pk[g_npk];
            memset(p, 0, sizeof *p);
            p->h = INVALID_HANDLE_VALUE;
            strncpy_s(p->src, sizeof p->src, fn, _TRUNCATE);
            if ((strlen(fn) > 1 && fn[1] == ':') || fn[0] == '/' || fn[0] == '\\') {
                strncpy_s(full, sizeof full, fn, _TRUNCATE);
            } else {
                _snprintf_s(full, sizeof full, _TRUNCATE, "%s/%s", dir, fn);
            }
            a = GetFileAttributesA(full);
            if (a == INVALID_FILE_ATTRIBUTES) {
                HLOG_W("PACK: %s (%s) fehlt -> uebersprungen", fn, id ? id : "?");
                continue;
            }
            if (a & FILE_ATTRIBUTE_DIRECTORY) {
                unsigned char *mj = NULL;
                size_t mn = 0;
                char mp[700];
                p->is_zip = 0;
                strncpy_s(p->root, sizeof p->root, full, _TRUNCATE);
                _snprintf_s(mp, sizeof mp, _TRUNCATE, "%s/manifest.json", full);
                if (!read_small(mp, &mj, &mn, 16u << 20)) {
                    HLOG_W("PACK: %s ohne manifest.json -> uebersprungen", fn);
                    continue;
                }
                free(mj);
            } else {
                int nz = 0;
                HANDLE h = INVALID_HANDLE_VALUE;
                zipent_t *ze = zip_dir(full, &nz, &h);
                if (!ze || !nz) {
                    HLOG_W("PACK: %s kein lesbares ZIP -> uebersprungen", fn);
                    if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); }
                    free(ze);
                    continue;
                }
                p->is_zip = 1;
                strncpy_s(p->root, sizeof p->root, full, _TRUNCATE);
                p->h = h;
                p->zent = ze;
                p->nzent = nz;
            }
            if (!pack_entries(p) || !p->id[0]) {
                HLOG_W("PACK: %s Manifest unlesbar -> uebersprungen", fn);
                if (p->h != INVALID_HANDLE_VALUE) { CloseHandle(p->h); }
                {
                    int k;
                    for (k = 0; k < p->nzent; k++) { free(p->zent[k].name); }
                    free(p->zent);
                    p->zent = NULL;
                }
                continue;
            }
            pack_check_files(p);
            g_npk++;
        }
    }
    jfree(j);
    /* Aufloesung je Hash: erstes normal in Reihenfolge, sonst erstes fallback. */
    for (i = 0; i < g_npk; i++) {
        int k;
        for (k = 0; k < g_pk[i].nents; k++) {
            pkent_t *d = &g_pk[i].ents[k];
            int s;
            if (!d->f2 && !d->f4) { continue; }
            s = idx_slot(d->hash, 0);
            if (s >= 0) {
                /* Bereits ein normaler Eintrag? Dann bleibt er. */
                continue;
            }
            if (d->pri == 0) {
                s = idx_slot(d->hash, 1);
                if (s >= 0) {
                    g_idx[s].pack = i;
                    g_idx[s].used = 1;
                }
            }
        }
    }
    for (i = 0; i < g_npk; i++) {
        int k;
        for (k = 0; k < g_pk[i].nents; k++) {
            pkent_t *d = &g_pk[i].ents[k];
            int s;
            if (!d->f2 && !d->f4) { continue; }
            s = idx_slot(d->hash, 0);
            if (s >= 0) { continue; }
            if (d->pri == 1) {
                s = idx_slot(d->hash, 1);
                if (s >= 0) {
                    g_idx[s].pack = i;
                    g_idx[s].used = 1;
                }
            }
        }
    }
    /* Fundorte + Kappen vormerken (Spielpfad: nur lesen). */
    for (i = 0; i < g_nidx; i++) {
        idxent_t *x = &g_idx[i];
        pack_t *p = &g_pk[x->pack];
        pkent_t *d = NULL;
        int k, c1 = 4, c2 = 4;
        for (k = 0; k < p->nents; k++) {
            if (p->ents[k].hash == x->hash) { d = &p->ents[k]; break; }
        }
        if (!d) { x->used = 0; continue; }
        c1 = cat_cap(d->cat);
        c2 = over_cap(x->hash);
        x->cap = c1 < c2 ? c1 : c2;
        if (d->f2) {
            x->has2 = 1;
            n2++;
            if (p->is_zip) {
                const zipent_t *e = zip_find(p, d->f2);
                if (e && e->method == 0) {
                    x->off2 = e->off;
                    x->size2 = e->csize;
                } else {
                    x->has2 = 0;
                    n2--;
                }
                x->path2 = (char *)malloc(strlen(p->id) + strlen(d->f2) + 2);
                if (x->path2) { sprintf_s(x->path2, strlen(p->id) + strlen(d->f2) + 2, "%s!%s", p->id, d->f2); }
            } else {
                size_t L = strlen(p->root) + strlen(d->f2) + 2;
                x->path2 = (char *)malloc(L);
                if (x->path2) { sprintf_s(x->path2, L, "%s/%s", p->root, d->f2); }
            }
        }
        if (d->f4) {
            x->has4 = 1;
            n4++;
            if (p->is_zip) {
                const zipent_t *e = zip_find(p, d->f4);
                if (e && e->method == 0) {
                    x->off4 = e->off;
                    x->size4 = e->csize;
                } else {
                    x->has4 = 0;
                    n4--;
                }
                x->path4 = (char *)malloc(strlen(p->id) + strlen(d->f4) + 2);
                if (x->path4) { sprintf_s(x->path4, strlen(p->id) + strlen(d->f4) + 2, "%s!%s", p->id, d->f4); }
            } else {
                size_t L = strlen(p->root) + strlen(d->f4) + 2;
                x->path4 = (char *)malloc(L);
                if (x->path4) { sprintf_s(x->path4, L, "%s/%s", p->root, d->f4); }
            }
        }
    }
    if (g_ncatcap || g_nover) {
        int uh = 0, uc = 0;
        for (i = 0; i < g_ncatcap; i++) { if (!g_catcap[i].hit) { uc++; } }
        for (i = 0; i < g_nover; i++) { if (!g_over[i].hit) { uh++; } }
        HLOG_I("PACK grenzen: kategorien=%d unbekannt=%d overrides=%d unbekannt=%d", g_ncatcap, uc, g_nover, uh);
    }
    g_avail = (n2 ? 1 : 0) | (n4 ? 2 : 0);
    /* Je Paket eine Zeile, dann Summe. */
    for (i = 0; i < g_npk; i++) {
        int k, shadow = 0;
        for (k = 0; k < g_pk[i].nents; k++) {
            int s = idx_slot(g_pk[i].ents[k].hash, 0);
            if (s < 0 || g_idx[s].pack != i) { shadow++; }
        }
        HLOG_I("PACK paket: %s %s \"%s\" eintraege=%d verdeckt=%d verworfen=%d %s",
               g_pk[i].id, g_pk[i].version, g_pk[i].name, g_pk[i].nents,
               shadow, g_pk[i].ndrop, g_pk[i].is_zip ? "(Datei)" : "(Ordner)");
        nactive++;
    }
    HLOG_I("PACK summe: liste=%d aktiv=%d hashs=%d x2=%d x4=%d", norder, nactive,
           g_nidx, n2, n4);
    return 1;
}

int sb_pack_active(void) { return g_inited && g_nidx > 0; }
int sb_pack_avail(void) { return g_avail; }

int sb_pack_find(u64 hv, int level, sb_pack_loc_t *loc)
{
    int s, eff, slot = 0;
    idxent_t *x;
    if (!g_inited || !loc || !g_idx || !g_idxh) { return 0; }
    s = idx_slot(hv, 0);
    if (s < 0 || !g_idx[s].used) { return 0; }
    x = &g_idx[s];
    eff = level < x->cap ? level : x->cap;
    if (eff >= 4 && x->has4) { slot = 4; }
    else if (eff >= 2 && x->has2) { slot = 2; }
    else if (eff >= 4 && x->has2) { slot = 2; }
    else { return 0; }
    memset(loc, 0, sizeof *loc);
    loc->slot = slot;
    loc->zip = g_pk[x->pack].is_zip;
    if (slot == 4) {
        strncpy_s(loc->path, sizeof loc->path, x->path4 ? x->path4 : "?", _TRUNCATE);
        loc->off = x->off4;
        loc->size = x->size4;
    } else {
        strncpy_s(loc->path, sizeof loc->path, x->path2 ? x->path2 : "?", _TRUNCATE);
        loc->off = x->off2;
        loc->size = x->size2;
    }
    return 1;
}

int sb_pack_read(const sb_pack_loc_t *loc, unsigned char **out, unsigned long *nout)
{
    *out = NULL;
    *nout = 0;
    if (!loc || !loc->path[0]) { return 0; }
    if (!loc->zip) {
        size_t n = 0;
        unsigned char *d = NULL;
        if (!read_small(loc->path, &d, &n, 256u << 20)) { return 0; }
        *out = d;
        *nout = (unsigned long)n;
        return 1;
    }
    {
        /* ZIP-Handle zum Offset finden: Anzeige "id!datei" -> Paket per id. */
        char id[128];
        const char *bang = strchr(loc->path, '!');
        size_t L;
        int i;
        HANDLE h = INVALID_HANDLE_VALUE;
        unsigned char *d;
        DWORD got = 0, r = 0;
        LARGE_INTEGER lo;
        if (!bang || (L = (size_t)(bang - loc->path)) >= sizeof id) { return 0; }
        memcpy(id, loc->path, L);
        id[L] = '\0';
        for (i = 0; i < g_npk; i++) {
            if (strcmp(g_pk[i].id, id) == 0) { h = g_pk[i].h; break; }
        }
        if (h == INVALID_HANDLE_VALUE || !loc->size || loc->size > (256u << 20)) { return 0; }
        d = (unsigned char *)malloc((size_t)loc->size);
        if (!d) { return 0; }
        lo.QuadPart = (LONGLONG)loc->off;
        if (!SetFilePointerEx(h, lo, NULL, FILE_BEGIN)) { free(d); return 0; }
        while (got < loc->size) {
            DWORD want = (DWORD)(loc->size - got);
            if (!ReadFile(h, d + got, want, &r, NULL) || !r) {
                free(d);
                return 0;
            }
            got += r;
        }
        *out = d;
        *nout = (unsigned long)loc->size;
        return 1;
    }
}

void sb_pack_shutdown(void)
{
    int i, k;
    if (!g_inited) { return; }
    g_inited = 0;
    for (i = 0; i < g_npk; i++) {
        if (g_pk[i].h != INVALID_HANDLE_VALUE) { CloseHandle(g_pk[i].h); }
        for (k = 0; k < g_pk[i].nzent; k++) { free(g_pk[i].zent[k].name); }
        free(g_pk[i].zent);
        for (k = 0; k < g_pk[i].nents; k++) {
            free(g_pk[i].ents[k].cat);
            free(g_pk[i].ents[k].f2);
            free(g_pk[i].ents[k].f4);
        }
        free(g_pk[i].ents);
    }
    g_npk = 0;
    for (i = 0; i < g_nidx; i++) {
        free(g_idx[i].path2);
        free(g_idx[i].path4);
    }
    free(g_idx);
    free(g_idxh);
    g_idx = NULL;
    g_idxh = NULL;
    g_nidx = 0;
    for (i = 0; i < g_ncatcap; i++) { free(g_catcap[i].name); }
    free(g_catcap);
    g_catcap = NULL;
    g_ncatcap = 0;
    free(g_over);
    g_over = NULL;
    g_nover = 0;
}
