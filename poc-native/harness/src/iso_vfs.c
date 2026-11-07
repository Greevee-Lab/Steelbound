/* iso_vfs.c - W150 ISO-DIRECT: XDVDFS-Abbild nur lesend (siehe iso_vfs.h).
 *
 * Format nach oeffentlicher Beschreibung (xboxdevwiki "XDVDFS"), eigener
 * Code. Enthaelt keine Spielbytes und keine Netz-APIs. */
#include "iso_vfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <winioctl.h>

#define XS 2048u                              /* Sektorgroesse */
#define XVD_SECTOR 32u                        /* Volume-Deskriptor */
static const char k_magic[20] = { 'M','I','C','R','O','S','O','F','T','*',
                                  'X','B','O','X','*','M','E','D','I','A' };
/* Partitionsbeginn: XISO, Redump-Vollabbild (nach der Videopartition). */
static const uint64_t k_bases[] = { 0ull, 0x18300000ull };

typedef struct {
    uint64_t off;       /* absoluter Byte-Offset der Daten im Abbild */
    uint32_t size;
    int parent;
    int first_child;    /* sortierte Kinder (in-order des Baums) */
    int next_sibling;
    uint8_t attr;
    uint32_t name_ofs;  /* Original-Name im Pool */
    uint32_t lpath_ofs; /* voller Pfad klein, '\\'-getrennt, im Pool */
} xnode_t;

struct iso_vfs {
    HANDLE h;
    uint64_t base;
    uint64_t file_size;
    uint32_t vt_lo, vt_hi;
    xnode_t *n;
    unsigned nn, ncap, ndirs;
    char *pool;
    uint32_t npool, poolcap;
    int *hash;          /* Knoten-Index+1, 0 = leer */
    unsigned hcap;
};

static void seterr(char *err, size_t cap, const char *fmt, const char *a)
{
    if (err && cap) {
        _snprintf_s(err, cap, _TRUNCATE, fmt, a ? a : "");
    }
}

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

static int raw_read(HANDLE h, uint64_t off, void *buf, uint32_t len, uint32_t *got)
{
    OVERLAPPED ov;
    DWORD g = 0;
    memset(&ov, 0, sizeof ov);
    ov.Offset = (DWORD)(off & 0xFFFFFFFFu);
    ov.OffsetHigh = (DWORD)(off >> 32);
    if (!ReadFile(h, buf, len, &g, &ov)) {
        DWORD e = GetLastError();
        *got = 0;
        return e == ERROR_HANDLE_EOF ? 0 : (int)e;
    }
    *got = g;
    return 0;
}

static uint32_t pool_add(iso_vfs_t *v, const char *s, size_t n)
{
    uint32_t o;
    if (v->npool + n + 1 > v->poolcap) {
        uint32_t nc = v->poolcap ? v->poolcap * 2 : 65536;
        char *np;
        while (nc < v->npool + n + 1) nc *= 2;
        np = (char *)realloc(v->pool, nc);
        if (!np) return 0xFFFFFFFFu;
        v->pool = np;
        v->poolcap = nc;
    }
    o = v->npool;
    memcpy(v->pool + o, s, n);
    v->pool[o + n] = '\0';
    v->npool += (uint32_t)n + 1;
    return o;
}

static int node_add(iso_vfs_t *v)
{
    if (v->nn == v->ncap) {
        unsigned nc = v->ncap ? v->ncap * 2 : 256;
        xnode_t *np = (xnode_t *)realloc(v->n, nc * sizeof *np);
        if (!np) return -1;
        v->n = np;
        v->ncap = nc;
    }
    memset(&v->n[v->nn], 0, sizeof v->n[0]);
    v->n[v->nn].parent = -1;
    v->n[v->nn].first_child = -1;
    v->n[v->nn].next_sibling = -1;
    return (int)v->nn++;
}

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static uint32_t fnv(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) { h ^= (uint8_t)*s; h *= 16777619u; }
    return h;
}

/* Ein Verzeichnis (Tabelle) einlesen; Kinder in-order an 'dir' haengen. */
static int parse_dir(iso_vfs_t *v, int dir, char *err, size_t errcap)
{
    xnode_t *d = &v->n[dir];
    uint32_t size = d->size, got = 0, sp = 0, cur;
    uint8_t *t;
    uint8_t *seen;
    uint32_t *stk;
    int last = -1, rc = 0;
    int pushed_left;
    uint32_t stkcap;
    char parent_lpath[1024];

    if (size == 0) return 0; /* leeres Verzeichnis */
    if (size > 16u * 1024 * 1024) {
        seterr(err, errcap, "Verzeichnistabelle zu gross (%s)", "");
        return -1;
    }
    strcpy_s(parent_lpath, sizeof parent_lpath, v->pool + d->lpath_ofs);
    t = (uint8_t *)malloc(size);
    seen = (uint8_t *)calloc(size / 4 + 1, 1);
    stkcap = (size / 4 + 2) * 3;
    stk = (uint32_t *)malloc(stkcap * sizeof *stk);
    if (!t || !seen || !stk) { free(t); free(seen); free(stk); return -1; }
    if (raw_read(v->h, d->off, t, size, &got) != 0 || got != size) {
        seterr(err, errcap, "Verzeichnistabelle nicht lesbar (%s)", "");
        free(t); free(seen); free(stk);
        return -1;
    }
    /* In-order ohne Rekursion: Stapel mit (Offset<<1 | links_erledigt). */
    stk[sp++] = 0;
    while (sp) {
        uint32_t ent = stk[--sp];
        cur = ent >> 1;
        pushed_left = (int)(ent & 1u);
        if (cur + 14 > size) continue;
        if (sp + 3 > stkcap) { rc = -1; break; }
        if (!pushed_left) {
            uint16_t l = rd16(t + cur);
            if (rd16(t + cur) == 0xFFFFu && rd16(t + cur + 2) == 0xFFFFu) continue; /* Fuellung */
            if (seen[cur / 4]) continue; /* Zyklus-Schutz */
            seen[cur / 4] = 1;
            stk[sp++] = (cur << 1) | 1u;
            if (l != 0 && l != 0xFFFFu) stk[sp++] = ((uint32_t)l * 4u) << 1;
            continue;
        }
        {
            uint16_t r = rd16(t + cur + 2);
            uint32_t sec = rd32(t + cur + 4), fsz = rd32(t + cur + 8);
            uint8_t attr = t[cur + 12], nl = t[cur + 13];
            char nm[256], lp[1300];
            int k, i;
            if (cur + 14u + nl > size || nl == 0) { rc = -1; break; }
            memcpy(nm, t + cur + 14, nl);
            nm[nl] = '\0';
            k = node_add(v);
            if (k < 0) { rc = -1; break; }
            d = &v->n[dir]; /* realloc */
            v->n[k].off = v->base + (uint64_t)sec * XS;
            v->n[k].size = fsz;
            v->n[k].attr = attr;
            v->n[k].parent = dir;
            v->n[k].name_ofs = pool_add(v, nm, nl);
            if (parent_lpath[0]) {
                _snprintf_s(lp, sizeof lp, _TRUNCATE, "%s\\%s", parent_lpath, nm);
            } else {
                strcpy_s(lp, sizeof lp, nm);
            }
            for (i = 0; lp[i]; i++) lp[i] = lc(lp[i]);
            v->n[k].lpath_ofs = pool_add(v, lp, strlen(lp));
            if (v->n[k].name_ofs == 0xFFFFFFFFu || v->n[k].lpath_ofs == 0xFFFFFFFFu) { rc = -1; break; }
            if (last < 0) d->first_child = k; else v->n[last].next_sibling = k;
            last = k;
            if (attr & ISO_VFS_ATTR_DIR) v->ndirs++;
            if (r != 0 && r != 0xFFFFu) stk[sp++] = ((uint32_t)r * 4u) << 1;
        }
    }
    free(t); free(seen); free(stk);
    if (rc != 0) seterr(err, errcap, "Verzeichniseintrag defekt (%s)", "");
    return rc;
}

static int build_hash(iso_vfs_t *v)
{
    unsigned i;
    v->hcap = 64;
    while (v->hcap < v->nn * 4) v->hcap *= 2;
    v->hash = (int *)calloc(v->hcap, sizeof *v->hash);
    if (!v->hash) return -1;
    for (i = 0; i < v->nn; i++) {
        uint32_t h = fnv(v->pool + v->n[i].lpath_ofs) & (v->hcap - 1);
        while (v->hash[h]) {
            /* doppelter Pfad: erster gewinnt (wie der Baum) */
            if (strcmp(v->pool + v->n[v->hash[h] - 1].lpath_ofs, v->pool + v->n[i].lpath_ofs) == 0) break;
            h = (h + 1) & (v->hcap - 1);
        }
        if (!v->hash[h]) v->hash[h] = (int)i + 1;
    }
    return 0;
}

iso_vfs_t *iso_vfs_open(const char *path, char *err, size_t errcap)
{
    iso_vfs_t *v;
    uint8_t vd[XS];
    unsigned b;
    LARGE_INTEGER fs;
    unsigned qi;
    int root;

    if (err && errcap) err[0] = '\0';
    if (!path || !path[0]) { seterr(err, errcap, "kein Abbild angegeben%s", ""); return NULL; }
    v = (iso_vfs_t *)calloc(1, sizeof *v);
    if (!v) return NULL;
    v->h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (v->h == INVALID_HANDLE_VALUE) {
        char m[64];
        _snprintf_s(m, sizeof m, _TRUNCATE, "%lu", GetLastError());
        seterr(err, errcap, "Abbild nicht lesbar (Fehler %s)", m);
        free(v);
        return NULL;
    }
    if (GetFileSizeEx(v->h, &fs)) v->file_size = (uint64_t)fs.QuadPart;
    for (b = 0; b < sizeof k_bases / sizeof k_bases[0]; b++) {
        uint32_t got = 0;
        uint64_t o = k_bases[b] + (uint64_t)XVD_SECTOR * XS;
        if (o + XS > v->file_size) continue;
        if (raw_read(v->h, o, vd, XS, &got) == 0 && got == XS &&
            memcmp(vd, k_magic, 20) == 0 && memcmp(vd + 0x7EC, k_magic, 20) == 0) {
            v->base = k_bases[b];
            break;
        }
    }
    if (b == sizeof k_bases / sizeof k_bases[0]) {
        seterr(err, errcap, "kein Xbox-Abbild (XDVDFS-Kennung fehlt)%s", "");
        iso_vfs_close(v);
        return NULL;
    }
    v->vt_lo = rd32(vd + 0x1C);
    v->vt_hi = rd32(vd + 0x20);
    root = node_add(v);
    if (root < 0) { iso_vfs_close(v); return NULL; }
    v->n[root].off = v->base + (uint64_t)rd32(vd + 0x14) * XS;
    v->n[root].size = rd32(vd + 0x18);
    v->n[root].attr = ISO_VFS_ATTR_DIR;
    v->n[root].name_ofs = pool_add(v, "", 0);
    v->n[root].lpath_ofs = pool_add(v, "", 0);
    /* Breitensuche ueber Verzeichnisse (Knotenliste waechst mit). */
    for (qi = 0; qi < v->nn; qi++) {
        if (!(v->n[qi].attr & ISO_VFS_ATTR_DIR)) continue;
        if (v->n[qi].off + v->n[qi].size > v->file_size) {
            seterr(err, errcap, "Verzeichnis ausserhalb des Abbilds (abgeschnitten?)%s", "");
            iso_vfs_close(v);
            return NULL;
        }
        if (parse_dir(v, (int)qi, err, errcap) != 0) {
            iso_vfs_close(v);
            return NULL;
        }
        if (v->nn > 200000) {
            seterr(err, errcap, "zu viele Eintraege%s", "");
            iso_vfs_close(v);
            return NULL;
        }
    }
    if (build_hash(v) != 0) { iso_vfs_close(v); return NULL; }
    return v;
}

void iso_vfs_close(iso_vfs_t *v)
{
    if (!v) return;
    if (v->h && v->h != INVALID_HANDLE_VALUE) CloseHandle(v->h);
    free(v->n);
    free(v->pool);
    free(v->hash);
    free(v);
}

uint64_t iso_vfs_part_base(const iso_vfs_t *v) { return v ? v->base : 0; }
unsigned iso_vfs_count(const iso_vfs_t *v) { return v ? v->nn : 0; }
unsigned iso_vfs_dir_count(const iso_vfs_t *v) { return v ? v->ndirs : 0; }

void iso_vfs_volume_time(const iso_vfs_t *v, uint32_t *lo, uint32_t *hi)
{
    *lo = v ? v->vt_lo : 0;
    *hi = v ? v->vt_hi : 0;
}

/* rel normalisieren: Trenner vereinheitlichen, leere/"."-Teile weg, ".."
 * zurueck, klein. */
static void norm_path(const char *rel, char *out, size_t cap)
{
    size_t o = 0;
    const char *p = rel ? rel : "";
    out[0] = '\0';
    while (*p) {
        const char *s;
        size_t n;
        while (*p == '\\' || *p == '/') p++;
        s = p;
        while (*p && *p != '\\' && *p != '/') p++;
        n = (size_t)(p - s);
        if (n == 0 || (n == 1 && s[0] == '.')) continue;
        if (n == 2 && s[0] == '.' && s[1] == '.') {
            while (o > 0 && out[o - 1] != '\\') o--;
            if (o > 0) o--;
            out[o] = '\0';
            continue;
        }
        if (o + n + 2 >= cap) break;
        if (o) out[o++] = '\\';
        while (n--) out[o++] = lc(*s++);
        out[o] = '\0';
    }
}

static int find_norm(const iso_vfs_t *v, const char *np)
{
    uint32_t h;
    if (!v->hash) return -1;
    h = fnv(np) & (v->hcap - 1);
    while (v->hash[h]) {
        int i = v->hash[h] - 1;
        if (strcmp(v->pool + v->n[i].lpath_ofs, np) == 0) return i;
        h = (h + 1) & (v->hcap - 1);
    }
    return -1;
}

int iso_vfs_lookup(const iso_vfs_t *v, const char *rel, int *parent_ok)
{
    char np[1024];
    int i;
    if (parent_ok) *parent_ok = 0;
    if (!v) return -1;
    norm_path(rel, np, sizeof np);
    i = find_norm(v, np);
    if (parent_ok) {
        if (i >= 0) {
            *parent_ok = 1;
        } else {
            char *s = strrchr(np, '\\');
            int pi;
            if (!s) {
                pi = 0; /* Wurzel */
            } else {
                *s = '\0';
                pi = find_norm(v, np);
            }
            *parent_ok = (pi >= 0 && (v->n[pi].attr & ISO_VFS_ATTR_DIR)) ? 1 : 0;
        }
    }
    return i;
}

int iso_vfs_stat(const iso_vfs_t *v, int node, iso_vfs_stat_t *st)
{
    if (!v || node < 0 || (unsigned)node >= v->nn) return -1;
    st->size = v->n[node].size;
    st->attr = v->n[node].attr;
    st->is_dir = (v->n[node].attr & ISO_VFS_ATTR_DIR) != 0;
    return 0;
}

const char *iso_vfs_name(const iso_vfs_t *v, int node)
{
    if (!v || node < 0 || (unsigned)node >= v->nn) return "";
    return v->pool + v->n[node].name_ofs;
}

int iso_vfs_child(const iso_vfs_t *v, int dir, unsigned k)
{
    int c;
    if (!v || dir < 0 || (unsigned)dir >= v->nn) return -1;
    for (c = v->n[dir].first_child; c >= 0 && k; k--) c = v->n[c].next_sibling;
    return c;
}

unsigned long iso_vfs_read(const iso_vfs_t *v, int node, uint64_t pos,
                           void *buf, uint32_t len, uint32_t *got)
{
    const xnode_t *x;
    uint32_t n;
    *got = 0;
    if (!v || node < 0 || (unsigned)node >= v->nn) return ERROR_INVALID_HANDLE;
    x = &v->n[node];
    if (x->attr & ISO_VFS_ATTR_DIR) return ERROR_ACCESS_DENIED;
    if (pos >= x->size || len == 0) return 0;
    n = (uint64_t)len < x->size - pos ? len : (uint32_t)(x->size - pos);
    return (unsigned long)raw_read(v->h, x->off + pos, buf, n, got);
}

int iso_vfs_read_all(const iso_vfs_t *v, const char *rel, uint8_t **out, uint32_t *len)
{
    int i = iso_vfs_lookup(v, rel, NULL);
    uint8_t *b;
    uint32_t got = 0;
    *out = NULL;
    *len = 0;
    if (i < 0 || (v->n[i].attr & ISO_VFS_ATTR_DIR)) return -1;
    b = (uint8_t *)malloc(v->n[i].size ? v->n[i].size : 1);
    if (!b) return -1;
    if (iso_vfs_read(v, i, 0, b, v->n[i].size, &got) != 0 || got != v->n[i].size) {
        free(b);
        return -1;
    }
    *out = b;
    *len = got;
    return 0;
}

/* ---------------- Selbsttest (ohne Spieldaten) ---------------- */
static void put_ent(uint8_t *t, uint32_t o, uint16_t l, uint16_t r, uint32_t sec,
                    uint32_t sz, uint8_t attr, const char *nm)
{
    uint8_t nl = (uint8_t)strlen(nm);
    memcpy(t + o, &l, 2);
    memcpy(t + o + 2, &r, 2);
    memcpy(t + o + 4, &sec, 4);
    memcpy(t + o + 8, &sz, 4);
    t[o + 12] = attr;
    t[o + 13] = nl;
    memcpy(t + o + 14, nm, nl);
}

static int write_at(HANDLE h, uint64_t off, const void *p, uint32_t n)
{
    OVERLAPPED ov;
    DWORD w = 0;
    memset(&ov, 0, sizeof ov);
    ov.Offset = (DWORD)off;
    ov.OffsetHigh = (DWORD)(off >> 32);
    return WriteFile(h, p, n, &w, &ov) && w == n ? 0 : -1;
}

/* Abbild: Wurzel (Sektor 40) = { "Media"(dir, Sektor 41), "default.xbe"
 * (Sektor 50, 5000 B) } als Baum: Wurzel-Eintrag "default.xbe", links
 * "Media". Media (Sektor 41) = { "A.TXT"(Sektor 60, 3 B), "Leer"(dir, 0 B) }. */
static int st_build(const char *path, uint64_t base)
{
    HANDLE h;
    uint8_t s[XS];
    uint32_t i;
    DWORD br;
    int rc = 0;
    h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_TEMPORARY, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    if (base) DeviceIoControl(h, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &br, NULL);
    memset(s, 0, sizeof s);
    memcpy(s, k_magic, 20);
    i = 40; memcpy(s + 0x14, &i, 4);
    i = XS; memcpy(s + 0x18, &i, 4);
    i = 0x11223344u; memcpy(s + 0x1C, &i, 4);
    i = 0x01C00000u; memcpy(s + 0x20, &i, 4);
    memcpy(s + 0x7EC, k_magic, 20);
    rc |= write_at(h, base + 32u * XS, s, XS);
    memset(s, 0xFF, sizeof s);
    put_ent(s, 0, 8, 0, 50, 5000, 0x20, "default.xbe"); /* links -> Offset 32 */
    put_ent(s, 32, 0, 0, 41, XS, 0x10, "Media");
    rc |= write_at(h, base + 40u * XS, s, XS);
    memset(s, 0xFF, sizeof s);
    put_ent(s, 0, 0, 8, 60, 3, 0x20, "A.TXT"); /* rechts -> Offset 32 */
    put_ent(s, 32, 0, 0, 0, 0, 0x10, "Leer");
    rc |= write_at(h, base + 41u * XS, s, XS);
    for (i = 0; i < sizeof s; i++) s[i] = (uint8_t)(i * 7u + 1u);
    rc |= write_at(h, base + 50u * XS, s, XS);
    rc |= write_at(h, base + 51u * XS, s, XS);
    rc |= write_at(h, base + 52u * XS, s, XS);
    rc |= write_at(h, base + 60u * XS, "abc", 3);
    rc |= write_at(h, base + 61u * XS, s, XS); /* Abbild endet hinter A.TXT */
    CloseHandle(h);
    return rc;
}

int iso_vfs_make_test_image(const char *path, const void *xbe, uint32_t n)
{
    HANDLE h;
    uint8_t s[XS];
    uint32_t i;
    int rc = 0;
    h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    memset(s, 0, sizeof s);
    memcpy(s, k_magic, 20);
    i = 40; memcpy(s + 0x14, &i, 4);
    i = XS; memcpy(s + 0x18, &i, 4);
    memcpy(s + 0x7EC, k_magic, 20);
    rc |= write_at(h, 32u * XS, s, XS);
    memset(s, 0xFF, sizeof s);
    put_ent(s, 0, 0, 0, 50, n, 0x20, "default.xbe");
    rc |= write_at(h, 40u * XS, s, XS);
    rc |= write_at(h, 50u * XS, xbe, n);
    memset(s, 0, sizeof s);
    rc |= write_at(h, 50u * XS + ((n + XS - 1) / XS) * XS, s, XS);
    CloseHandle(h);
    return rc;
}

int iso_vfs_selftest(char *msg, size_t cap)
{
    char dir[MAX_PATH], path[MAX_PATH + 32], err[256];
    unsigned b;
    int fail = 0;
    if (!GetTempPathA(sizeof dir, dir)) return 1;
    _snprintf_s(path, sizeof path, _TRUNCATE, "%ssb_isovfs_selftest_%lu.iso", dir,
                GetCurrentProcessId());
    for (b = 0; b < 2 && !fail; b++) {
        iso_vfs_t *v;
        iso_vfs_stat_t st;
        uint8_t buf[6000];
        uint32_t got = 0;
        int i, po = 0;
        if (st_build(path, k_bases[b]) != 0) { fail = 10; break; }
        v = iso_vfs_open(path, err, sizeof err);
        if (!v) { fail = 11; break; }
        if (iso_vfs_part_base(v) != k_bases[b]) fail = 12;
        else if (iso_vfs_count(v) != 5) fail = 13;
        else if ((i = iso_vfs_lookup(v, "DEFAULT.XBE", NULL)) < 0) fail = 14;
        else if (iso_vfs_stat(v, i, &st) || st.size != 5000 || st.is_dir) fail = 15;
        else if (iso_vfs_read(v, i, 0, buf, sizeof buf, &got) || got != 5000 ||
                 buf[1] != 8 || buf[2048] != 1) fail = 16;
        else if (iso_vfs_read(v, i, 5000, buf, 10, &got) || got != 0) fail = 17;
        else if ((i = iso_vfs_lookup(v, "/media//a.txt", NULL)) < 0) fail = 18;
        else if (iso_vfs_read(v, i, 1, buf, 10, &got) || got != 2 || buf[0] != 'b') fail = 19;
        else if (iso_vfs_lookup(v, "Media\\fehlt.bin", &po) >= 0 || !po) fail = 20;
        else if (iso_vfs_lookup(v, "Fehlt\\x.bin", &po) >= 0 || po) fail = 21;
        else if ((i = iso_vfs_lookup(v, "media", NULL)) < 0 ||
                 strcmp(iso_vfs_name(v, iso_vfs_child(v, i, 0)), "A.TXT") != 0 ||
                 strcmp(iso_vfs_name(v, iso_vfs_child(v, i, 1)), "Leer") != 0 ||
                 iso_vfs_child(v, i, 2) != -1) fail = 22;
        else if (iso_vfs_lookup(v, "", NULL) != 0) fail = 23;
        else if ((i = iso_vfs_lookup(v, "media\\leer", NULL)) < 0 ||
                 iso_vfs_child(v, i, 0) != -1) fail = 24;
        else if (iso_vfs_read(v, iso_vfs_lookup(v, "media", NULL), 0, buf, 4, &got) == 0) fail = 25;
        iso_vfs_close(v);
    }
    if (!fail) {
        /* falsches Abbild: Kennung fehlt -> klare Meldung, kein Absturz */
        HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            uint8_t z[XS * 40];
            DWORD w;
            memset(z, 0x5A, sizeof z);
            WriteFile(h, z, sizeof z, &w, NULL);
            CloseHandle(h);
            if (iso_vfs_open(path, err, sizeof err) != NULL || !strstr(err, "XDVDFS")) fail = 30;
        } else {
            fail = 31;
        }
    }
    DeleteFileA(path);
    if (msg && cap) {
        _snprintf_s(msg, cap, _TRUNCATE, fail ? "iso_vfs Selbsttest FEHLER %d" : "iso_vfs Selbsttest ok (XISO + Redump-Offset, Suche, Lesen, EOF, falsches Abbild)", fail);
    }
    return fail;
}
