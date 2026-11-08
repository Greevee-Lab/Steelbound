/* sb_packscan.c - W148 PACK-LAUNCHER: Scan/Pruefen/packs.json.
 *
 * Bindet sb_pack.c ein und nutzt dessen JSON-/ZIP-/Inflate-Parser
 * (kein zweiter Parser). Wird in beiden Builds STATT sb_pack.c
 * uebersetzt (CMake); sb_pack.c steht in keinem Build mehr direkt.
 * Aufrufer: Client (nur Typen) und Launcher-Seite "Packs" (Scan,
 * Vollpruefung V1-V13, Verdeckungszahlen, packs.json lesen/schreiben).
 * Kein Spielzustand, keine HLOG-Pflicht (Launcher: hlog_stub.c).
 */
#include "sb_pack.c"
#include "sb_sha256.h"

/* ------------------------------------------------------------------ */
/* Pfade                                                               */
/* ------------------------------------------------------------------ */
static int sbpk_is_abs(const char *p)
{
    return p && ((strlen(p) > 1 && p[1] == ':') || p[0] == '/' ||
                 p[0] == '\\');
}

static void sbpk_join(char *out, size_t cap, const char *dir, const char *fn)
{
    if (sbpk_is_abs(fn)) {
        strncpy_s(out, cap, fn, _TRUNCATE);
    } else {
        _snprintf_s(out, cap, _TRUNCATE, "%s/%s", dir, fn);
    }
}

static void sbpk_dirname(const char *path, char *out, size_t cap)
{
    char *s;
    strncpy_s(out, cap, path, _TRUNCATE);
    for (s = out; *s; s++) {
        if (*s == '\\') {
            *s = '/';
        }
    }
    s = strrchr(out, '/');
    if (s) {
        *s = '\0';
    } else {
        strcpy_s(out, cap, ".");
    }
}

/* ------------------------------------------------------------------ */
/* Validatoren (wie tools/pack/sbpack.py)                              */
/* ------------------------------------------------------------------ */
static int sbpk_ishex(int c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

static int sbpk_sha64(const char *s)
{
    int i;
    if (!s) {
        return 0;
    }
    for (i = 0; i < 64; i++) {
        char c = s[i];
        if (!c) {
            return 0;
        }
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return 0;
        }
    }
    return s[64] == '\0';
}

static int sbpk_hash16_lo(const char *s)
{
    int i;
    if (!s) {
        return 0;
    }
    for (i = 0; i < 16; i++) {
        char c = s[i];
        if (!c) {
            return 0;
        }
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return 0;
        }
    }
    return s[16] == '\0';
}

static int sbpk_alnumlo(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

/* ^[a-z0-9]+(\.[a-z0-9-]+)+$ */
static int sbpk_id_ok(const char *s)
{
    int dots = 0;
    if (!s || !sbpk_alnumlo(s[0])) {
        return 0;
    }
    while (sbpk_alnumlo(*s)) {
        s++;
    }
    while (*s == '.') {
        dots++;
        s++;
        if (*s != '-' && !sbpk_alnumlo(*s)) {
            return 0;
        }
        while (*s == '-' || sbpk_alnumlo(*s)) {
            s++;
        }
    }
    return dots > 0 && *s == '\0';
}

/* ^[0-9]+\.[0-9]+\.[0-9]+(-[0-9a-zA-Z.-]+)?$ */
static int sbpk_semver(const char *s)
{
    int part = 0;
    if (!s || !s[0]) {
        return 0;
    }
    for (;;) {
        if (*s < '0' || *s > '9') {
            return 0;
        }
        while (*s >= '0' && *s <= '9') {
            s++;
        }
        part++;
        if (part < 3) {
            if (*s != '.') {
                return 0;
            }
            s++;
        } else {
            break;
        }
    }
    if (*s == '\0') {
        return 1;
    }
    if (*s != '-') {
        return 0;
    }
    s++;
    if (!*s) {
        return 0;
    }
    while (*s) {
        char c = *s++;
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') || c == '.' || c == '-')) {
            return 0;
        }
    }
    return 1;
}

static int sbpk_cat_known(const char *s)
{
    static const char *const k[] = { "font_hud", "cockpit", "ground",
        "buildings", "vehicles", "effects", "effect", "menu", "world",
        "unknown" };
    unsigned i;
    for (i = 0; i < sizeof k / sizeof k[0]; i++) {
        if (strcmp(s, k[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static int sbpk_kind_known(const char *s)
{
    return strcmp(s, "ai") == 0 || strcmp(s, "text") == 0 ||
           strcmp(s, "manual") == 0 || strcmp(s, "mixed") == 0;
}

/* Pfad-Regel wie sbpack.check_path: NULL = ok, sonst Code. */
static const char *sbpk_check_path(const char *n)
{
    const char *s;
    if (!n || strlen(n) > 200) {
        return "path_too_long";
    }
    if (n[0] == '/' || strchr(n, '\\') ||
        (strlen(n) > 1 && n[1] == ':')) {
        return "bad_path";
    }
    for (s = n;;) {
        const char *e = strchr(s, '/');
        size_t L = e ? (size_t)(e - s) : strlen(s);
        if (L == 2 && s[0] == '.' && s[1] == '.') {
            return "bad_path";
        }
        if (!e) {
            break;
        }
        s = e + 1;
    }
    return NULL;
}

/* Positivliste wie sbpack.suffix_ok (kleingeschrieben verglichen). */
static int sbpk_suffix_ok(const char *n)
{
    char b[220];
    size_t L, i;
    if (!n) {
        return 0;
    }
    strncpy_s(b, sizeof b, n, _TRUNCATE);
    L = strlen(b);
    for (i = 0; i < L; i++) {
        if (b[i] >= 'A' && b[i] <= 'Z') {
            b[i] = (char)(b[i] - 'A' + 'a');
        }
    }
    if (strcmp(b, "manifest.json") == 0 || strcmp(b, "entries.json") == 0) {
        return 1;
    }
    if (L > 4 && strcmp(b + L - 4, ".png") == 0) {
        return strncmp(b, "preview/", 8) == 0;
    }
    if (L > 5 && strcmp(b + L - 5, ".json") == 0) {
        return 1;
    }
    if (L > 4 && (strcmp(b + L - 4, ".dds") == 0 ||
                  strcmp(b + L - 4, ".txt") == 0 ||
                  strcmp(b + L - 3, ".md") == 0)) {
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* DDS-Kopf (wie sbpack.dds_info)                                      */
/* ------------------------------------------------------------------ */
typedef struct { int w, h, mips; char fmt[8]; } sbpk_dds_t;

static int sbpk_dds(const unsigned char *d, size_t n, sbpk_dds_t *o)
{
    unsigned pff, bits;
    if (n < 128 || memcmp(d, "DDS ", 4) != 0 || u32le(d + 4) != 124) {
        return 0;
    }
    o->h = (int)u32le(d + 12);
    o->w = (int)u32le(d + 16);
    o->mips = (u32le(d + 8) & 0x20000u) ? (int)u32le(d + 28) : 1;
    if (!o->mips) {
        o->mips = 1;
    }
    pff = u32le(d + 80);
    bits = u32le(d + 88);
    o->fmt[0] = '\0';
    if (pff & 4u) {
        if (memcmp(d + 84, "DXT1", 4) == 0) {
            strcpy_s(o->fmt, sizeof o->fmt, "BC1");
        } else if (memcmp(d + 84, "DXT3", 4) == 0) {
            strcpy_s(o->fmt, sizeof o->fmt, "BC3");
        } else if (memcmp(d + 84, "DXT2", 4) == 0) {
            strcpy_s(o->fmt, sizeof o->fmt, "BC2");
        } else if (memcmp(d + 84, "DXT5", 4) == 0 ||
                   memcmp(d + 84, "DXT4", 4) == 0) {
            strcpy_s(o->fmt, sizeof o->fmt, "BC3");
        } else if (memcmp(d + 84, "DX10", 4) == 0) {
            unsigned f10;
            if (n < 148) {
                return 0;
            }
            f10 = u32le(d + 128);
            if (f10 == 71 || f10 == 72) {
                strcpy_s(o->fmt, sizeof o->fmt, "BC1");
            } else if (f10 == 74 || f10 == 75) {
                strcpy_s(o->fmt, sizeof o->fmt, "BC2");
            } else if (f10 == 77 || f10 == 78) {
                strcpy_s(o->fmt, sizeof o->fmt, "BC3");
            } else if (f10 == 98 || f10 == 99) {
                strcpy_s(o->fmt, sizeof o->fmt, "BC7");
            } else if (f10 == 28 || f10 == 29) {
                strcpy_s(o->fmt, sizeof o->fmt, "RGBA8");
            } else {
                return 0;
            }
        } else {
            return 0;
        }
    } else if ((pff & 0x40u) && bits == 32) {
        strcpy_s(o->fmt, sizeof o->fmt, "RGBA8");
    }
    if (!o->fmt[0] || !o->w || !o->h || o->w > 16384 || o->h > 16384) {
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Befunde                                                             */
/* ------------------------------------------------------------------ */
typedef struct {
    int red; /* 1 = rot, 0 = gelb */
    char rule[8], reason[40], detail[220];
} sbpk_find_t;

typedef struct { sbpk_find_t *v; int n, cap; } sbpk_finds_t;

static void sbpk_add(sbpk_finds_t *f, int red, const char *rule,
                     const char *reason, const char *detail)
{
    sbpk_find_t *d;
    if (f->n >= 128) {
        return;
    }
    if (f->n >= f->cap) {
        int nc = f->cap ? f->cap * 2 : 32;
        sbpk_find_t *q = (sbpk_find_t *)realloc(f->v, sizeof *q * (size_t)nc);
        if (!q) {
            return;
        }
        f->v = q;
        f->cap = nc;
    }
    d = &f->v[f->n++];
    d->red = red;
    strncpy_s(d->rule, sizeof d->rule, rule, _TRUNCATE);
    strncpy_s(d->reason, sizeof d->reason, reason, _TRUNCATE);
    strncpy_s(d->detail, sizeof d->detail, detail ? detail : "", _TRUNCATE);
}

static void sbpk_addf(sbpk_finds_t *f, int red, const char *rule,
                      const char *reason, const char *d1, const char *d2)
{
    char b[220];
    if (d2 && d2[0]) {
        _snprintf_s(b, sizeof b, _TRUNCATE, "%s %s", d1 ? d1 : "", d2);
    } else {
        strncpy_s(b, sizeof b, d1 ? d1 : "", _TRUNCATE);
    }
    sbpk_add(f, red, rule, reason, b);
}

/* ------------------------------------------------------------------ */
/* Paket-Zugriff (Ordner oder ZIP, nur Pruefpfad)                       */
/* ------------------------------------------------------------------ */
typedef struct {
    char root[700];
    int is_zip;
    HANDLE h;
    zipent_t *zent;
    int nzent;
} sbpk_pk_t;

static int sbpk_open(const char *path, sbpk_pk_t *p)
{
    DWORD a;
    memset(p, 0, sizeof *p);
    p->h = INVALID_HANDLE_VALUE;
    strncpy_s(p->root, sizeof p->root, path, _TRUNCATE);
    a = GetFileAttributesA(path);
    if (a == INVALID_FILE_ATTRIBUTES) {
        return 0;
    }
    if (a & FILE_ATTRIBUTE_DIRECTORY) {
        p->is_zip = 0;
        return 1;
    }
    p->zent = zip_dir(path, &p->nzent, &p->h);
    if (!p->zent || !p->nzent) {
        if (p->h != INVALID_HANDLE_VALUE) {
            CloseHandle(p->h);
            p->h = INVALID_HANDLE_VALUE;
        }
        free(p->zent);
        p->zent = NULL;
        return 0;
    }
    p->is_zip = 1;
    return 1;
}

static void sbpk_close(sbpk_pk_t *p)
{
    int i;
    if (p->h != INVALID_HANDLE_VALUE) {
        CloseHandle(p->h);
    }
    for (i = 0; i < p->nzent; i++) {
        free(p->zent[i].name);
    }
    free(p->zent);
    memset(p, 0, sizeof *p);
    p->h = INVALID_HANDLE_VALUE;
}

static const zipent_t *sbpk_zfind(sbpk_pk_t *p, const char *name)
{
    int i;
    for (i = 0; i < p->nzent; i++) {
        if (strcmp(p->zent[i].name, name) == 0) {
            return &p->zent[i];
        }
    }
    return NULL;
}

static int sbpk_has(sbpk_pk_t *p, const char *name)
{
    if (p->is_zip) {
        return sbpk_zfind(p, name) != NULL;
    } else {
        char fp[1400];
        _snprintf_s(fp, sizeof fp, _TRUNCATE, "%s/%s", p->root, name);
        return GetFileAttributesA(fp) != INVALID_FILE_ATTRIBUTES;
    }
}

/* -3 = fehlt, sonst Methode (Ordner: 0). */
static int sbpk_method(sbpk_pk_t *p, const char *name)
{
    if (!p->is_zip) {
        return 0;
    } else {
        const zipent_t *e = sbpk_zfind(p, name);
        return e ? e->method : -3;
    }
}

/* ZIP-Eintrag lesen (stored bis 256 MB wie der Spielpfad; deflated klein). */
static int sbpk_zread(sbpk_pk_t *p, const zipent_t *e, unsigned char **out,
                      size_t *nout)
{
    if (e->method == 0) {
        unsigned char *d;
        DWORD got = 0, r = 0;
        LARGE_INTEGER lo;
        if (!e->csize || e->csize > (256u << 20)) {
            return 0;
        }
        d = (unsigned char *)malloc((size_t)e->csize);
        if (!d) {
            return 0;
        }
        lo.QuadPart = (LONGLONG)e->off;
        if (!SetFilePointerEx(p->h, lo, NULL, FILE_BEGIN)) {
            free(d);
            return 0;
        }
        while (got < e->csize) {
            DWORD want = (DWORD)(e->csize - got);
            if (!ReadFile(p->h, d + got, want, &r, NULL) || !r) {
                free(d);
                return 0;
            }
            got += r;
        }
        *out = d;
        *nout = (size_t)e->csize;
        return 1;
    }
    return zip_get(p->h, e, out, nout);
}

static int sbpk_read(sbpk_pk_t *p, const char *name, unsigned char **out,
                     size_t *nout)
{
    if (p->is_zip) {
        const zipent_t *e = sbpk_zfind(p, name);
        if (!e) {
            return 0;
        }
        return sbpk_zread(p, e, out, nout);
    } else {
        char fp[1400];
        _snprintf_s(fp, sizeof fp, _TRUNCATE, "%s/%s", p->root, name);
        return read_small(fp, out, nout, 256u << 20);
    }
}

/* Alle Dateinamen einsammeln (Ordner: rekursiv, ZIP: ohne Ordner-Eintraege).
 * Rueckgabe: Anzahl (malloc, Aufrufer gibt frei: Namen + Feld). */
static char **sbpk_names(sbpk_pk_t *p, int *nout)
{
    char **out = NULL;
    int n = 0, cap = 0;
    *nout = 0;
    if (p->is_zip) {
        int i;
        for (i = 0; i < p->nzent; i++) {
            size_t L = strlen(p->zent[i].name);
            char *q;
            if (!L || p->zent[i].name[L - 1] == '/') {
                continue;
            }
            if (n >= cap) {
                int nc = cap ? cap * 2 : 256;
                char **r = (char **)realloc(out, sizeof *r * (size_t)nc);
                if (!r) {
                    break;
                }
                out = r;
                cap = nc;
            }
            q = (char *)malloc(L + 1);
            if (!q) {
                break;
            }
            memcpy(q, p->zent[i].name, L + 1);
            out[n++] = q;
        }
    } else {
        /* Tiefensuche per Stapel offener Verzeichnisse (max. 64). */
        char stack[64][700];
        int sp = 1;
        strncpy_s(stack[0], sizeof stack[0], "", _TRUNCATE);
        while (sp > 0) {
            char cur[700], pat[1450];
            WIN32_FIND_DATAA fd;
            HANDLE fh;
            strcpy_s(cur, sizeof cur, stack[--sp]);
            _snprintf_s(pat, sizeof pat, _TRUNCATE, "%s/%s*", p->root, cur);
            fh = FindFirstFileA(pat, &fd);
            if (fh == INVALID_HANDLE_VALUE) {
                continue;
            }
            do {
                if (strcmp(fd.cFileName, ".") == 0 ||
                    strcmp(fd.cFileName, "..") == 0) {
                    continue;
                }
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    if (sp < 64) {
                        _snprintf_s(stack[sp], sizeof stack[0],
                                    _TRUNCATE, "%s%s/", cur, fd.cFileName);
                        sp++;
                    }
                } else {
                    char rel[700];
                    char *q;
                    _snprintf_s(rel, sizeof rel, _TRUNCATE, "%s%s", cur,
                                fd.cFileName);
                    if (n >= cap) {
                        int nc = cap ? cap * 2 : 256;
                        char **r = (char **)realloc(out,
                            sizeof *r * (size_t)nc);
                        if (!r) {
                            break;
                        }
                        out = r;
                        cap = nc;
                    }
                    q = (char *)malloc(strlen(rel) + 1);
                    if (!q) {
                        break;
                    }
                    strcpy_s(q, strlen(rel) + 1, rel);
                    out[n++] = q;
                    if (n >= 65536) {
                        break;
                    }
                }
            } while (FindNextFileA(fh, &fd));
            FindClose(fh);
            if (n >= 65536) {
                break;
            }
        }
    }
    *nout = n;
    return out;
}

static void sbpk_names_free(char **v, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        free(v[i]);
    }
    free(v);
}

/* JSON-Zahl lesen (nur kind 3 gilt). */
static int sbpk_jint(const jnode_t *j, const char *k, int *ok)
{
    const jnode_t *v = jget(j, k);
    if (ok) {
        *ok = (v && v->kind == 3);
    }
    return (v && v->kind == 3) ? (int)v->num : 0;
}

/* Kleinbuchstaben-Kopie (fuer xbe-Vergleich). */
static void sbpk_lower(const char *s, char *out, size_t cap)
{
    size_t i;
    for (i = 0; i + 1 < cap && s[i]; i++) {
        char c = s[i];
        out[i] = (c >= 'A' && c <= 'F') ? (char)(c - 'A' + 'a') : c;
    }
    out[i] = '\0';
}

static int sbpk_endswith_lo(const char *n, const char *suf)
{
    size_t L = strlen(n), S = strlen(suf), i;
    if (L < S) {
        return 0;
    }
    for (i = 0; i < S; i++) {
        char a = n[L - S + i], b = suf[i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (a != b) {
            return 0;
        }
    }
    return 1;
}

/* Manifest lesen + parsen. rueckgabe: Knoten (Aufrufer: jfree) oder NULL
 * (Befund bereits eingetragen). */
static jnode_t *sbpk_manifest(sbpk_pk_t *p, sbpk_finds_t *f, jnode_t **ef_out)
{
    unsigned char *mj = NULL;
    size_t mn = 0;
    jnode_t *m;
    *ef_out = NULL;
    if (!sbpk_has(p, "manifest.json")) {
        sbpk_add(f, 1, "V1", "missing_manifest", "");
        return NULL;
    }
    if (!sbpk_read(p, "manifest.json", &mj, &mn)) {
        sbpk_add(f, 1, "V1", "missing_manifest", "");
        return NULL;
    }
    m = jparse((const char *)mj, mn);
    free(mj);
    if (!m || m->kind != 5) {
        sbpk_add(f, 1, "V1", "manifest_not_json", "");
        jfree(m);
        return NULL;
    }
    return m;
}

/* Vollpruefung wie sbpack.validate_pack (Reihenfolge = dort). */
static void sbpk_validate(sbpk_pk_t *p, const char *xbe, sbpk_finds_t *f)
{
    char **names = NULL;
    int nnames = 0, i;
    jnode_t *m = NULL, *ef = NULL;
    const jnode_t *arr = NULL;
    const jnode_t *g;
    static const char *const need[] = { "id", "name", "version", "type",
        "author", "license", "game", "hash_algo" };
    if (p->is_zip) {
        for (i = 0; i < p->nzent; i++) {
            if (p->zent[i].method == -1) {
                sbpk_add(f, 1, "V3", "zip_encrypted", p->zent[i].name);
            }
        }
    }
    names = sbpk_names(p, &nnames);
    if (names) {
        for (i = 0; i < nnames; i++) {
            const char *r = sbpk_check_path(names[i]);
            if (r) {
                sbpk_add(f, 1, "V3", r, names[i]);
            }
            if (!sbpk_suffix_ok(names[i])) {
                sbpk_add(f, 1, "V3", "deny_suffix", names[i]);
            }
        }
    }
    m = sbpk_manifest(p, f, &ef);
    if (!m) {
        sbpk_names_free(names, nnames);
        return;
    }
    /* V1 Schema. */
    {
        const char *fmt = jstrv(m, "format");
        char b[64];
        if (!fmt || strcmp(fmt, "sbpack") != 0) {
            _snprintf_s(b, sizeof b, _TRUNCATE, "'%s'", fmt ? fmt : "none");
            sbpk_add(f, 1, "V1", "bad_format", b);
        }
        if (jintv(m, "format_version", 0) != 1) {
            _snprintf_s(b, sizeof b, _TRUNCATE, "%d",
                        jintv(m, "format_version", 0));
            sbpk_add(f, 1, "V1", "bad_format_version", b);
        }
        for (i = 0; i < 8; i++) {
            if (!jget(m, need[i])) {
                sbpk_add(f, 1, "V1", "missing_field", need[i]);
            }
        }
        {
            const char *id = jstrv(m, "id");
            if (jget(m, "id") && !sbpk_id_ok(id ? id : "")) {
                _snprintf_s(b, sizeof b, _TRUNCATE, "'%s'", id ? id : "?");
                sbpk_add(f, 1, "V1", "bad_id", b);
            }
        }
        {
            const char *vs = jstrv(m, "version");
            if (jget(m, "version") && !sbpk_semver(vs ? vs : "")) {
                _snprintf_s(b, sizeof b, _TRUNCATE, "'%s'", vs ? vs : "?");
                sbpk_add(f, 1, "V1", "bad_version", b);
            }
        }
        {
            const char *ty = jstrv(m, "type");
            if (!ty || (strcmp(ty, "texture") != 0 &&
                        strcmp(ty, "effect") != 0)) {
                _snprintf_s(b, sizeof b, _TRUNCATE, "'%s'", ty ? ty : "none");
                sbpk_add(f, 1, "V1", "bad_type", b);
            }
        }
        {
            const char *ha = jstrv(m, "hash_algo");
            if (!ha || strcmp(ha, "sbt1-fnv64") != 0) {
                _snprintf_s(b, sizeof b, _TRUNCATE, "'%s'", ha ? ha : "none");
                sbpk_add(f, 1, "V1", "bad_hash_algo", b);
            }
        }
    }
    g = jget(m, "game");
    {
        const jnode_t *xl = (g && g->kind == 5) ? jget(g, "xbe_sha256") : NULL;
        int ok = 0;
        if (xl && xl->kind == 6 && xl->n > 0) {
            int k;
            ok = 1;
            for (k = 0; k < xl->n; k++) {
                const jnode_t *x = xl->items[k];
                if (!x || x->kind != 4 || !sbpk_sha64(x->s)) {
                    ok = 0;
                    sbpk_add(f, 1, "V1", "bad_game", "xbe_sha256 hex");
                    break;
                }
            }
        } else {
            sbpk_add(f, 1, "V1", "bad_game", "xbe_sha256");
        }
        (void)ok;
        /* V2 Spielversion (nur wenn bekannt). */
        if (xbe && xbe[0] && xl && xl->kind == 6) {
            int k, hit = 0;
            char lo[70];
            for (k = 0; k < xl->n; k++) {
                const jnode_t *x = xl->items[k];
                if (!x || x->kind != 4) {
                    continue;
                }
                sbpk_lower(x->s, lo, sizeof lo);
                if (strcmp(xbe, lo) == 0) {
                    hit = 1;
                    break;
                }
            }
            if (!hit) {
                char xb[20];
                strncpy_s(xb, sizeof xb, xbe, 16);
                xb[16] = '\0';
                sbpk_add(f, 1, "V2", "game_mismatch", xb);
            }
        }
    }
    /* Eintraege (inline oder entries_file). */
    {
        const jnode_t *en = jget(m, "entries");
        const char *efn = jstrv(m, "entries_file");
        int have_en = en != NULL, have_ef = jget(m, "entries_file") != NULL;
        if (have_en && have_ef) {
            sbpk_add(f, 1, "V1", "entries_and_file", "");
        } else if (have_ef) {
            unsigned char *ej = NULL;
            size_t eln = 0;
            if (!efn || sbpk_check_path(efn) || !sbpk_has(p, efn)) {
                sbpk_add(f, 1, "V1", "entries_file_missing", "");
            } else if (!sbpk_read(p, efn, &ej, &eln)) {
                sbpk_add(f, 1, "V1", "entries_file_missing", "");
            } else {
                ef = jparse((const char *)ej, eln);
                free(ej);
                arr = ef ? jget(ef, "entries") : NULL;
                if (!ef || !arr || arr->kind != 6) {
                    sbpk_add(f, 1, "V1", "entries_file_not_json", "");
                    arr = NULL;
                }
            }
        } else if (!en || en->kind != 6) {
            sbpk_add(f, 1, "V1", "entries_not_list", "");
        } else {
            arr = en;
        }
    }
    if (arr && arr->kind == 6) {
        char (*seen)[17] = NULL;
        int nseen = 0;
        if (arr->n > 0) {
            seen = (char(*)[17])malloc(sizeof *seen * (size_t)arr->n);
        }
        for (i = 0; i < arr->n; i++) {
            const jnode_t *e = arr->items[i];
            const char *hh, *cat, *pri, *kind;
            const jnode_t *var, *orig;
            int ow = 0, oh = 0, have_orig = 0, k;
            char tag[48];
            int dup = 0;
            _snprintf_s(tag, sizeof tag, _TRUNCATE, "entries[%d]", i);
            if (!e || e->kind != 5) {
                sbpk_add(f, 1, "V1", "bad_entry", tag);
                continue;
            }
            hh = jstrv(e, "hash");
            if (!hh || !sbpk_hash16_lo(hh)) {
                char b[80];
                _snprintf_s(b, sizeof b, _TRUNCATE, "%s '%s'", tag,
                            hh ? hh : "?");
                sbpk_add(f, 1, "V1", "bad_hash", b);
                continue;
            }
            for (k = 0; k < nseen; k++) {
                if (strcmp(seen[k], hh) == 0) {
                    dup = 1;
                    break;
                }
            }
            if (dup) {
                sbpk_add(f, 1, "V10", "dup_hash", hh);
            } else if (seen) {
                strcpy_s(seen[nseen++], 17, hh);
            }
            cat = jstrv(e, "category");
            if (cat && cat[0] && !sbpk_cat_known(cat)) {
                char b[120];
                _snprintf_s(b, sizeof b, _TRUNCATE, "%s %s", hh, cat);
                sbpk_add(f, 0, "V12", "unknown_category", b);
            }
            if (jget(e, "kind")) {
                kind = jstrv(e, "kind");
                if (!kind || !sbpk_kind_known(kind)) {
                    char b[120];
                    _snprintf_s(b, sizeof b, _TRUNCATE, "%s '%s'", hh,
                                kind ? kind : "?");
                    sbpk_add(f, 1, "V1", "bad_kind", b);
                }
            }
            pri = jstrv(e, "priority");
            if (!pri) {
                pri = "normal";
            }
            if (strcmp(pri, "normal") != 0 &&
                strcmp(pri, "fallback") != 0) {
                char b[120];
                _snprintf_s(b, sizeof b, _TRUNCATE, "%s '%s'", hh, pri);
                sbpk_add(f, 1, "V1", "bad_priority", b);
            }
            var = jget(e, "variants");
            if (!var || var->kind != 5 || !var->n) {
                sbpk_add(f, 1, "V1", "no_variants", hh);
                continue;
            }
            for (k = 0; k < var->n; k++) {
                if (strcmp(var->keys[k], "x2") != 0 &&
                    strcmp(var->keys[k], "x4") != 0) {
                    char b[120];
                    _snprintf_s(b, sizeof b, _TRUNCATE, "%s %s", hh,
                                var->keys[k]);
                    sbpk_add(f, 1, "V1", "bad_slot", b);
                }
            }
            orig = jget(e, "orig");
            if (orig && orig->kind == 5) {
                int okw = 0, okh = 0;
                ow = sbpk_jint(orig, "w", &okw);
                oh = sbpk_jint(orig, "h", &okh);
                have_orig = okw && okh && ow > 0 && oh > 0;
            }
            {
                static const char *const slots[2] = { "x2", "x4" };
                for (k = 0; k < 2; k++) {
                    const jnode_t *v = jget(var, slots[k]);
                    const char *fn, *vf, *vsha;
                    int mw = 0, mh = 0, mmips = 0, okw = 0, okh = 0, okm = 0;
                    char vtag[64];
                    static const char *const vfneed[6] = { "file", "w", "h",
                        "fmt", "mips", "sha256" };
                    int q;
                    unsigned char *data = NULL;
                    size_t dlen = 0;
                    uint8_t dg[32];
                    char dhex[65];
                    sbpk_dds_t di;
                    if (!v) {
                        continue;
                    }
                    _snprintf_s(vtag, sizeof vtag, _TRUNCATE, "%s %s", hh,
                                slots[k]);
                    if (v->kind != 5) {
                        sbpk_add(f, 1, "V1", "bad_variant", vtag);
                        continue;
                    }
                    for (q = 0; q < 6; q++) {
                        if (!jget(v, vfneed[q])) {
                            char b[160];
                            _snprintf_s(b, sizeof b, _TRUNCATE, "%s %s",
                                        vtag, vfneed[q]);
                            sbpk_add(f, 1, "V1", "missing_variant_field", b);
                        }
                    }
                    fn = jstrv(v, "file");
                    if (!fn || sbpk_check_path(fn)) {
                        sbpk_add(f, 1, "V3", "bad_variant_path", vtag);
                        continue;
                    }
                    if (!sbpk_has(p, fn)) {
                        char b[300];
                        _snprintf_s(b, sizeof b, _TRUNCATE, "%s %s", vtag,
                                    fn);
                        sbpk_add(f, 1, "V4", "file_missing", b);
                        continue;
                    }
                    if (p->is_zip && sbpk_endswith_lo(fn, ".dds") &&
                        sbpk_method(p, fn) != 0) {
                        sbpk_add(f, 1, "V11", "dds_not_stored", vtag);
                    }
                    if (!sbpk_read(p, fn, &data, &dlen)) {
                        sbpk_add(f, 1, "V4", "file_unreadable", vtag);
                        continue;
                    }
                    sb_sha256(data, dlen, dg);
                    sb_sha256_hex(dg, dhex);
                    vsha = jstrv(v, "sha256");
                    if (!vsha || strcmp(dhex, vsha) != 0) {
                        sbpk_add(f, 1, "V9", "sha_mismatch", vtag);
                    }
                    if (!sbpk_endswith_lo(fn, ".dds")) {
                        free(data);
                        continue;
                    }
                    if (!sbpk_dds(data, dlen, &di)) {
                        char b[160];
                        _snprintf_s(b, sizeof b, _TRUNCATE, "%s bad header",
                                    vtag);
                        sbpk_add(f, 1, "V5", "dds_bad_header", b);
                        free(data);
                        continue;
                    }
                    free(data);
                    data = NULL;
                    vf = jstrv(v, "fmt");
                    mw = sbpk_jint(v, "w", &okw);
                    mh = sbpk_jint(v, "h", &okh);
                    mmips = sbpk_jint(v, "mips", &okm);
                    if (!vf || strcmp(vf, di.fmt) != 0) {
                        char b[160];
                        _snprintf_s(b, sizeof b, _TRUNCATE,
                                    "%s manifest=%s dds=%s", vtag,
                                    vf ? vf : "?", di.fmt);
                        sbpk_add(f, 1, "V5", "fmt_mismatch", b);
                    }
                    if (!okw || !okh || mw != di.w || mh != di.h) {
                        char b[160];
                        _snprintf_s(b, sizeof b, _TRUNCATE,
                                    "%s manifest=%dx%d dds=%dx%d", vtag,
                                    okw ? mw : -1, okh ? mh : -1, di.w, di.h);
                        sbpk_add(f, 1, "V5", "size_mismatch", b);
                    }
                    if (!okm || mmips != di.mips) {
                        char b[160];
                        _snprintf_s(b, sizeof b, _TRUNCATE,
                                    "%s manifest=%d dds=%d", vtag,
                                    okm ? mmips : -1, di.mips);
                        sbpk_add(f, 0, "V8", "mips_mismatch", b);
                    }
                    if (have_orig) {
                        if (di.w * oh != di.h * ow || di.w < ow) {
                            char b[160];
                            _snprintf_s(b, sizeof b, _TRUNCATE,
                                        "%s dds=%dx%d orig=%dx%d", vtag,
                                        di.w, di.h, ow, oh);
                            sbpk_add(f, 1, "V6", "aspect_mismatch", b);
                        } else {
                            int fc = strcmp(slots[k], "x2") == 0 ? 2 : 4;
                            if (di.w != ow * fc || di.h != oh * fc) {
                                char b[160];
                                _snprintf_s(b, sizeof b, _TRUNCATE,
                                            "%s dds=%dx%d orig=%dx%d",
                                            vtag, di.w, di.h, ow, oh);
                                sbpk_add(f, 0, "V7", "size_not_factor", b);
                            }
                        }
                    }
                }
            }
        }
        free(seen);
    }
    /* Effect-Parameter (PACK_FORMAT.md Abschnitt 9, Positivliste kSbFxParams). */
    {
        const char *ty = jstrv(m, "type");
        if (ty && strcmp(ty, "texture") == 0 && sbpk_has(p, "params.json")) {
            sbpk_add(f, 0, "V13", "params_ignored",
                     "params.json in texture pack");
        }
        if (ty && strcmp(ty, "effect") == 0 && sbpk_has(p, "params.json")) {
            unsigned char *pj = NULL;
            size_t pn = 0;
            jnode_t *pp = NULL;
            const jnode_t *ps = NULL;
            int k;
            if (sbpk_read(p, "params.json", &pj, &pn)) {
                pp = jparse((const char *)pj, pn);
                free(pj);
            }
            if (!pp) {
                sbpk_add(f, 1, "V1", "params_not_json", "");
            } else {
                ps = jget(pp, "params");
                if (!ps || ps->kind != 5) {
                    sbpk_add(f, 1, "V1", "params_bad_schema", "");
                } else {
                    for (k = 0; k < ps->n; k++) {
                        const char *key = ps->keys[k] ? ps->keys[k] : "";
                        const jnode_t *v = ps->vals[k];
                        int fi = fx_idx(key);
                        int iv;
                        char b[160];
                        if (fi < 0) {
                            sbpk_add(f, 0, "V13", "unknown_param", key);
                            continue;
                        }
                        if (!v || v->kind != 3) {
                            _snprintf_s(b, sizeof b, _TRUNCATE, "%s", key);
                            sbpk_add(f, 0, "V14", "params_bad_value", b);
                            continue;
                        }
                        iv = (int)v->num;
                        if ((double)iv != v->num) {
                            _snprintf_s(b, sizeof b, _TRUNCATE, "%s %g", key, v->num);
                            sbpk_add(f, 0, "V14", "params_not_int", b);
                        }
                        if (iv < kSbFxParams[fi].min || iv > kSbFxParams[fi].max) {
                            _snprintf_s(b, sizeof b, _TRUNCATE, "%s %d ausserhalb %d..%d",
                                        key, iv, kSbFxParams[fi].min, kSbFxParams[fi].max);
                            sbpk_add(f, 0, "V14", "param_range", b);
                        }
                    }
                }
            }
            jfree(pp);
        }
    }
    sbpk_names_free(names, nnames);
    jfree(m);
    jfree(ef);
}

/* Schnellpruefung fuer die Liste (nur Struktur: Manifest, Schema,
 * Eintraege, Datei-Anwesenheit; keine Bytes, kein SHA). */
static int sbpk_quick(sbpk_pk_t *p, char *reason, size_t rcap)
{
    unsigned char *mj = NULL;
    size_t mn = 0;
    jnode_t *m = NULL, *ef = NULL;
    const jnode_t *arr = NULL;
    int i, status = 2;
    static const char *const need[] = { "id", "name", "version", "type",
        "author", "license", "game", "hash_algo" };
    if (reason && rcap) {
        reason[0] = '\0';
    }
    if (!sbpk_has(p, "manifest.json") ||
        !sbpk_read(p, "manifest.json", &mj, &mn)) {
        if (reason && rcap) {
            strncpy_s(reason, rcap, "missing_manifest", _TRUNCATE);
        }
        return 0;
    }
    m = jparse((const char *)mj, mn);
    free(mj);
    if (!m || m->kind != 5) {
        jfree(m);
        if (reason && rcap) {
            strncpy_s(reason, rcap, "manifest_not_json", _TRUNCATE);
        }
        return 0;
    }
    {
        const char *fmt = jstrv(m, "format");
        const char *rcode = NULL;
        if (!fmt || strcmp(fmt, "sbpack") != 0) {
            rcode = "bad_format";
        } else if (jintv(m, "format_version", 0) != 1) {
            rcode = "bad_format_version";
        } else {
            for (i = 0; i < 8; i++) {
                if (!jget(m, need[i])) {
                    rcode = "missing_field";
                    break;
                }
            }
            if (!rcode) {
                const char *id = jstrv(m, "id");
                const char *vs = jstrv(m, "version");
                const char *ty = jstrv(m, "type");
                const char *ha = jstrv(m, "hash_algo");
                if (!sbpk_id_ok(id ? id : "")) {
                    rcode = "bad_id";
                } else if (!sbpk_semver(vs ? vs : "")) {
                    rcode = "bad_version";
                } else if (!ty || (strcmp(ty, "texture") != 0 &&
                                   strcmp(ty, "effect") != 0)) {
                    rcode = "bad_type";
                } else if (!ha || strcmp(ha, "sbt1-fnv64") != 0) {
                    rcode = "bad_hash_algo";
                }
            }
        }
        if (rcode) {
            if (reason && rcap) {
                strncpy_s(reason, rcap, rcode, _TRUNCATE);
            }
            jfree(m);
            return 0;
        }
    }
    {
        const jnode_t *en = jget(m, "entries");
        const char *efn = jstrv(m, "entries_file");
        int have_en = en != NULL, have_ef = jget(m, "entries_file") != NULL;
        if (have_en && have_ef) {
            if (reason && rcap) {
                strncpy_s(reason, rcap, "entries_and_file", _TRUNCATE);
            }
            jfree(m);
            return 0;
        } else if (have_ef) {
            unsigned char *ej = NULL;
            size_t eln = 0;
            if (!efn || sbpk_check_path(efn) || !sbpk_has(p, efn) ||
                !sbpk_read(p, efn, &ej, &eln)) {
                free(ej);
                if (reason && rcap) {
                    strncpy_s(reason, rcap, "entries_file_missing",
                              _TRUNCATE);
                }
                jfree(m);
                return 0;
            }
            ef = jparse((const char *)ej, eln);
            free(ej);
            arr = ef ? jget(ef, "entries") : NULL;
            if (!arr || arr->kind != 6) {
                if (reason && rcap) {
                    strncpy_s(reason, rcap, "entries_file_not_json",
                              _TRUNCATE);
                }
                jfree(m);
                jfree(ef);
                return 0;
            }
        } else if (!en || en->kind != 6) {
            if (reason && rcap) {
                strncpy_s(reason, rcap, "entries_not_list", _TRUNCATE);
            }
            jfree(m);
            return 0;
        } else {
            arr = en;
        }
    }
    for (i = 0; i < arr->n; i++) {
        const jnode_t *e = arr->items[i];
        const char *hh, *cat;
        const jnode_t *var;
        static const char *const slots[2] = { "x2", "x4" };
        int k;
        if (!e || e->kind != 5) {
            status = 0;
            if (reason && rcap) {
                strncpy_s(reason, rcap, "bad_entry", _TRUNCATE);
            }
            break;
        }
        hh = jstrv(e, "hash");
        if (!hh || !sbpk_hash16_lo(hh)) {
            status = 0;
            if (reason && rcap) {
                strncpy_s(reason, rcap, "bad_hash", _TRUNCATE);
            }
            break;
        }
        cat = jstrv(e, "category");
        if (cat && cat[0] && !sbpk_cat_known(cat) && status == 2) {
            status = 1;
            if (reason && rcap) {
                strncpy_s(reason, rcap, "unknown_category", _TRUNCATE);
            }
        }
        var = jget(e, "variants");
        if (!var || var->kind != 5 || !var->n) {
            status = 0;
            if (reason && rcap) {
                strncpy_s(reason, rcap, "no_variants", _TRUNCATE);
            }
            break;
        }
        for (k = 0; k < 2; k++) {
            const jnode_t *v = jget(var, slots[k]);
            const char *fn;
            if (!v) {
                continue;
            }
            if (v->kind != 5) {
                status = 0;
                if (reason && rcap) {
                    strncpy_s(reason, rcap, "bad_variant", _TRUNCATE);
                }
                break;
            }
            fn = jstrv(v, "file");
            if (!fn || sbpk_check_path(fn)) {
                status = 0;
                if (reason && rcap) {
                    strncpy_s(reason, rcap, "bad_variant_path", _TRUNCATE);
                }
                break;
            }
            if (!sbpk_has(p, fn)) {
                status = 0;
                if (reason && rcap) {
                    strncpy_s(reason, rcap, "file_missing", _TRUNCATE);
                }
                break;
            }
            if (p->is_zip && sbpk_endswith_lo(fn, ".dds") &&
                sbpk_method(p, fn) != 0) {
                status = 0;
                if (reason && rcap) {
                    strncpy_s(reason, rcap, "dds_not_stored", _TRUNCATE);
                }
                break;
            }
        }
        if (status == 0) {
            break;
        }
    }
    jfree(m);
    jfree(ef);
    return status;
}

int sb_pack_check(const char *pkg_path, const char *xbe_sha256,
                  char *reason, unsigned rcap, char *detail, unsigned dcap)
{
    sbpk_pk_t p;
    sbpk_finds_t f;
    int i, status = 2;
    char xlo[70];
    if (reason && rcap) {
        reason[0] = '\0';
    }
    if (detail && dcap) {
        detail[0] = '\0';
    }
    memset(&f, 0, sizeof f);
    if (!sbpk_open(pkg_path, &p)) {
        sbpk_add(&f, 1, "V4", "pkg_unreadable", pkg_path);
    } else {
        if (xbe_sha256 && xbe_sha256[0]) {
            sbpk_lower(xbe_sha256, xlo, sizeof xlo);
            sbpk_validate(&p, xlo, &f);
        } else {
            sbpk_validate(&p, NULL, &f);
        }
        sbpk_close(&p);
    }
    for (i = 0; i < f.n; i++) {
        if (f.v[i].red) {
            status = 0;
            if (reason && rcap) {
                strncpy_s(reason, rcap, f.v[i].reason, _TRUNCATE);
            }
            if (detail && dcap) {
                char b[300];
                _snprintf_s(b, sizeof b, _TRUNCATE, "%s %s", f.v[i].rule,
                            f.v[i].detail);
                strncpy_s(detail, dcap, b, _TRUNCATE);
            }
            break;
        }
    }
    if (status == 2) {
        for (i = 0; i < f.n; i++) {
            if (!f.v[i].red) {
                status = 1;
                if (reason && rcap) {
                    strncpy_s(reason, rcap, f.v[i].reason, _TRUNCATE);
                }
                if (detail && dcap) {
                    char b[300];
                    _snprintf_s(b, sizeof b, _TRUNCATE, "%s %s",
                                f.v[i].rule, f.v[i].detail);
                    strncpy_s(detail, dcap, b, _TRUNCATE);
                }
                break;
            }
        }
    }
    free(f.v);
    return status;
}

/* packs.json einlesen: order-Text + Verzeichnis. */
static jnode_t *sbpk_read_pj(const char *packs_json, char *dir, size_t dcap)
{
    unsigned char *pj = NULL;
    size_t pn = 0;
    jnode_t *j;
    if (!read_small(packs_json, &pj, &pn, 1u << 20)) {
        return NULL;
    }
    j = jparse((const char *)pj, pn);
    free(pj);
    if (!j || j->kind != 5) {
        jfree(j);
        return NULL;
    }
    sbpk_dirname(packs_json, dir, dcap);
    return j;
}

/* Manifest-Meta eines Pakets lesen (fuer Scan). */
static void sbpk_meta(const char *full, sb_pack_scan_t *o)
{
    sbpk_pk_t p;
    unsigned char *mj = NULL;
    size_t mn = 0;
    jnode_t *m = NULL, *ef = NULL;
    const jnode_t *arr;
    o->nentries = -1;
    o->status = -1;
    o->reason[0] = '\0';
    o->nparams = -1;
    o->paramsum[0] = '\0';
    if (!sbpk_open(full, &p)) {
        o->is_zip = -1;
        o->status = 0;
        strcpy_s(o->reason, sizeof o->reason, "pkg_unreadable");
        return;
    }
    o->is_zip = p.is_zip;
    o->status = sbpk_quick(&p, o->reason, sizeof o->reason);
    if (!sbpk_has(&p, "manifest.json") ||
        !sbpk_read(&p, "manifest.json", &mj, &mn)) {
        sbpk_close(&p);
        return;
    }
    m = jparse((const char *)mj, mn);
    free(mj);
    if (!m || m->kind != 5) {
        jfree(m);
        sbpk_close(&p);
        return;
    }
    {
        const char *id = jstrv(m, "id"), *nm = jstrv(m, "name");
        const char *vs = jstrv(m, "version"), *au = jstrv(m, "author");
        const char *li = jstrv(m, "license"), *ty = jstrv(m, "type");
        if (id && id[0]) {
            strncpy_s(o->id, sizeof o->id, id, _TRUNCATE);
        }
        strncpy_s(o->name, sizeof o->name, nm ? nm : o->id, _TRUNCATE);
        strncpy_s(o->version, sizeof o->version, vs ? vs : "?", _TRUNCATE);
        strncpy_s(o->author, sizeof o->author, au ? au : "?", _TRUNCATE);
        strncpy_s(o->license, sizeof o->license, li ? li : "?", _TRUNCATE);
        strncpy_s(o->type, sizeof o->type, ty ? ty : "?", _TRUNCATE);
    }
    arr = jget(m, "entries");
    if (!arr) {
        const char *efn = jstrv(m, "entries_file");
        unsigned char *ej = NULL;
        size_t eln = 0;
        if (efn && !sbpk_check_path(efn) && sbpk_has(&p, efn) &&
            sbpk_read(&p, efn, &ej, &eln)) {
            ef = jparse((const char *)ej, eln);
            free(ej);
            arr = ef ? jget(ef, "entries") : NULL;
        }
    }
    o->nentries = (arr && arr->kind == 6) ? arr->n : -1;
    /* W151: Effect-Pack-Parameter zaehlen + Kurzfassung (bekannte Keys). */
    {
        const char *ty = jstrv(m, "type");
        if (ty && strcmp(ty, "effect") == 0 && sbpk_has(&p, "params.json")) {
            unsigned char *qj = NULL;
            size_t qn = 0;
            jnode_t *q = NULL;
            const jnode_t *ps;
            int fi, n = 0;
            char *w = o->paramsum;
            size_t rest = sizeof o->paramsum;
            if (sbpk_read(&p, "params.json", &qj, &qn)) {
                q = jparse((const char *)qj, qn);
                free(qj);
            }
            ps = q ? jget(q, "params") : NULL;
            o->nparams = 0;
            if (ps && ps->kind == 5) {
                for (fi = 0; fi < SB_FXN; fi++) {
                    const jnode_t *v = jget(ps, kSbFxParams[fi].key);
                    int L;
                    if (!v || v->kind != 3) { continue; }
                    o->nparams = ++n;
                    L = _snprintf_s(w, rest, _TRUNCATE, "%s%s=%d",
                                    w == o->paramsum ? "" : ", ",
                                    kSbFxParams[fi].key, (int)v->num);
                    if (L < 0) { break; }
                    w += (size_t)L;
                    rest -= (size_t)L;
                }
            }
            jfree(q);
        }
    }
    jfree(m);
    jfree(ef);
    sbpk_close(&p);
}

int sb_pack_scan(const char *packs_json, sb_pack_scan_t *out, int cap)
{
    char dir[600];
    jnode_t *j = sbpk_read_pj(packs_json, dir, sizeof dir);
    const jnode_t *order;
    int i, n = 0;
    if (!j) {
        return -1;
    }
    order = jget(j, "order");
    if (order && order->kind == 6) {
        for (i = 0; i < order->n; i++) {
            const jnode_t *o = order->items[i];
            const jnode_t *en;
            const char *fn, *id;
            char full[700];
            if (!o || o->kind != 5) {
                continue;
            }
            fn = jstrv(o, "file");
            id = jstrv(o, "id");
            if (!fn || !fn[0]) {
                continue;
            }
            if (n < cap && out) {
                sb_pack_scan_t *d = &out[n];
                memset(d, 0, sizeof *d);
                strncpy_s(d->src, sizeof d->src, fn, _TRUNCATE);
                strncpy_s(d->id, sizeof d->id, id ? id : "?", _TRUNCATE);
                en = jget(o, "enabled");
                d->enabled = !(en && en->kind == 1);
                sbpk_join(full, sizeof full, dir, fn);
                sbpk_meta(full, d);
                if (!d->name[0]) {
                    strncpy_s(d->name, sizeof d->name, d->id, _TRUNCATE);
                }
                if (!d->version[0]) {
                    strcpy_s(d->version, sizeof d->version, "?");
                }
                if (!d->author[0]) {
                    strcpy_s(d->author, sizeof d->author, "?");
                }
                if (!d->license[0]) {
                    strcpy_s(d->license, sizeof d->license, "?");
                }
                if (!d->type[0]) {
                    strcpy_s(d->type, sizeof d->type, "?");
                }
            }
            n++;
        }
    }
    jfree(j);
    return n;
}

/* Ein geladenes Paket fuer die Verdeckungsrechnung (Auszug aus pack_t). */
typedef struct {
    u64 *hash;
    char *pri;   /* 0 = normal, 1 = fallback */
    char *file;  /* 1 = mind. eine Variante mit Datei */
    char *cat;   /* W172: Kategorie-Index (kSbPackCat, nur Anzeige) */
    char *var;   /* W172: Bit 1 = Variante x2, Bit 2 = x4 (nach Dateipruefung) */
    int n;
} sbpk_shadow_pk_t;

/* W172 LAUNCHER-P567: Kategorien in Anzeige-Reihenfolge (Launcher-Seite
 * Packs, Abdeckung je Kategorie). "effect" zaehlt wie "effects", fehlende
 * oder unbekannte Kategorie = "unknown". */
const char *const kSbPackCat[SB_PACK_NCAT] = { "font_hud", "cockpit",
    "ground", "buildings", "vehicles", "effects", "menu", "world", "unknown" };

static int sbpk_cat_index(const char *c)
{
    int i;
    if (!c || !c[0]) {
        return SB_PACK_NCAT - 1;
    }
    if (strcmp(c, "effect") == 0) {
        return 5;
    }
    for (i = 0; i < SB_PACK_NCAT; i++) {
        if (strcmp(c, kSbPackCat[i]) == 0) {
            return i;
        }
    }
    return SB_PACK_NCAT - 1;
}

static void sbpk_shadow_free(sbpk_shadow_pk_t *v, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        free(v[i].hash);
        free(v[i].pri);
        free(v[i].file);
        free(v[i].cat);
        free(v[i].var);
    }
    free(v);
}

/* Paket wie der Lader einlesen (pack_entries + pack_check_files). */
static int sbpk_shadow_load(const char *full, sbpk_shadow_pk_t *o)
{
    pack_t p;
    int i;
    memset(o, 0, sizeof *o);
    memset(&p, 0, sizeof p);
    p.h = INVALID_HANDLE_VALUE;
    {
        DWORD a = GetFileAttributesA(full);
        if (a == INVALID_FILE_ATTRIBUTES) {
            return 0;
        }
        if (a & FILE_ATTRIBUTE_DIRECTORY) {
            p.is_zip = 0;
            strncpy_s(p.root, sizeof p.root, full, _TRUNCATE);
        } else {
            int nz = 0;
            HANDLE h = INVALID_HANDLE_VALUE;
            zipent_t *ze = zip_dir(full, &nz, &h);
            if (!ze || !nz) {
                if (h != INVALID_HANDLE_VALUE) {
                    CloseHandle(h);
                }
                free(ze);
                return 0;
            }
            p.is_zip = 1;
            strncpy_s(p.root, sizeof p.root, full, _TRUNCATE);
            p.h = h;
            p.zent = ze;
            p.nzent = nz;
        }
    }
    if (!pack_entries(&p) || !p.id[0]) {
        if (p.h != INVALID_HANDLE_VALUE) {
            CloseHandle(p.h);
        }
        for (i = 0; i < p.nzent; i++) {
            free(p.zent[i].name);
        }
        free(p.zent);
        for (i = 0; i < p.nents; i++) {
            free(p.ents[i].cat);
            free(p.ents[i].f2);
            free(p.ents[i].f4);
        }
        free(p.ents);
        return 0;
    }
    pack_check_files(&p);
    if (p.nents > 0) {
        o->hash = (u64 *)malloc(sizeof *o->hash * (size_t)p.nents);
        o->pri = (char *)malloc((size_t)p.nents);
        o->file = (char *)malloc((size_t)p.nents);
        o->cat = (char *)malloc((size_t)p.nents);
        o->var = (char *)malloc((size_t)p.nents);
        if (!o->hash || !o->pri || !o->file || !o->cat || !o->var) {
            free(o->hash);
            free(o->pri);
            free(o->file);
            free(o->cat);
            free(o->var);
            memset(o, 0, sizeof *o);
        } else {
            for (i = 0; i < p.nents; i++) {
                o->hash[i] = p.ents[i].hash;
                o->pri[i] = (char)p.ents[i].pri;
                o->file[i] = (p.ents[i].f2 || p.ents[i].f4) ? 1 : 0;
                o->cat[i] = (char)sbpk_cat_index(p.ents[i].cat);
                o->var[i] = (char)((p.ents[i].f2 ? 1 : 0) | (p.ents[i].f4 ? 2 : 0));
            }
            o->n = p.nents;
        }
    }
    if (p.h != INVALID_HANDLE_VALUE) {
        CloseHandle(p.h);
    }
    for (i = 0; i < p.nzent; i++) {
        free(p.zent[i].name);
    }
    free(p.zent);
    for (i = 0; i < p.nents; i++) {
        free(p.ents[i].cat);
        free(p.ents[i].f2);
        free(p.ents[i].f4);
    }
    free(p.ents);
    return 1;
}

/* Laden + Aufloesung wie sb_pack_init (gemeinsam fuer sb_pack_shadow und
 * W172 sb_pack_coverage): erst normal in Reihenfolge, dann fallback;
 * Gewinner je Hash = erstes Paket. */
typedef struct {
    sbpk_shadow_pk_t *pkgs;
    char *loaded;
    u64 *chosen;
    int *choice;
    int nchoice;
    int n;
} sbpk_res_t;

static void sbpk_res_free(sbpk_res_t *r)
{
    free(r->chosen);
    free(r->choice);
    sbpk_shadow_free(r->pkgs, r->n);
    free(r->loaded);
    memset(r, 0, sizeof *r);
}

/* Rueckgabe: Order-Eintraege, -1 = packs.json unlesbar/kein Speicher. */
static int sbpk_resolve(const char *packs_json, sbpk_res_t *r)
{
    char dir[600];
    jnode_t *j = sbpk_read_pj(packs_json, dir, sizeof dir);
    const jnode_t *order;
    int n = 0, i, k, pass;
    memset(r, 0, sizeof *r);
    if (!j) {
        return -1;
    }
    order = jget(j, "order");
    if (order && order->kind == 6) {
        /* Gueltige Order-Eintraege zaehlen. */
        for (i = 0; i < order->n; i++) {
            const jnode_t *o = order->items[i];
            const char *fn = (o && o->kind == 5) ? jstrv(o, "file") : NULL;
            if (fn && fn[0]) {
                n++;
            }
        }
    }
    if (n > 0) {
        r->pkgs = (sbpk_shadow_pk_t *)calloc((size_t)n, sizeof *r->pkgs);
        r->loaded = (char *)calloc((size_t)n, 1);
    }
    if (n > 0 && (!r->pkgs || !r->loaded)) {
        free(r->pkgs);
        free(r->loaded);
        memset(r, 0, sizeof *r);
        jfree(j);
        return -1;
    }
    r->n = n;
    /* Laden (deaktivierte/fehlende bleiben ungeladen). */
    if (order && order->kind == 6) {
        int w = 0;
        for (i = 0; i < order->n && w < n; i++) {
            const jnode_t *o = order->items[i];
            const jnode_t *en;
            const char *fn = (o && o->kind == 5) ? jstrv(o, "file") : NULL;
            char full[700];
            if (!fn || !fn[0]) {
                continue;
            }
            en = jget(o, "enabled");
            if (en && en->kind == 1) {
                w++;
                continue;
            }
            sbpk_join(full, sizeof full, dir, fn);
            if (sbpk_shadow_load(full, &r->pkgs[w])) {
                r->loaded[w] = 1;
            }
            w++;
        }
    }
    jfree(j);
    {
        int total = 0;
        for (i = 0; i < n; i++) {
            total += r->pkgs[i].n;
        }
        if (total > 0) {
            r->chosen = (u64 *)malloc(sizeof *r->chosen * (size_t)total);
            r->choice = (int *)malloc(sizeof *r->choice * (size_t)total);
            if (!r->chosen || !r->choice) { /* Speicher knapp: alles verdeckt */
                free(r->chosen);
                free(r->choice);
                r->chosen = NULL;
                r->choice = NULL;
            }
        }
    }
    for (pass = 0; pass < 2 && r->chosen && r->choice; pass++) {
        for (i = 0; i < n; i++) {
            if (!r->loaded[i]) {
                continue;
            }
            for (k = 0; k < r->pkgs[i].n; k++) {
                int q, have = 0;
                if (!r->pkgs[i].file[k] || r->pkgs[i].pri[k] != pass) {
                    continue;
                }
                for (q = 0; q < r->nchoice; q++) {
                    if (r->chosen[q] == r->pkgs[i].hash[k]) {
                        have = 1;
                        break;
                    }
                }
                if (!have) {
                    r->chosen[r->nchoice] = r->pkgs[i].hash[k];
                    r->choice[r->nchoice] = i;
                    r->nchoice++;
                }
            }
        }
    }
    return n;
}

/* Gewinner-Paket eines Hash (-1 = keiner). */
static int sbpk_winner(const sbpk_res_t *r, u64 h)
{
    int q;
    for (q = 0; q < r->nchoice; q++) {
        if (r->chosen[q] == h) {
            return r->choice[q];
        }
    }
    return -1;
}

int sb_pack_shadow(const char *packs_json, int *nentries, int *shadowed,
                   int cap)
{
    sbpk_res_t r;
    int i, k, w, n = sbpk_resolve(packs_json, &r);
    if (n < 0) {
        return -1;
    }
    /* Zaehlen (wie die PACK-paket-Zeile: Eintrag ohne Wahl = verdeckt). */
    w = cap < n ? cap : n;
    for (i = 0; i < w; i++) {
        if (!r.loaded[i]) {
            if (nentries) {
                nentries[i] = -1;
            }
            if (shadowed) {
                shadowed[i] = -1;
            }
            continue;
        }
        if (nentries) {
            nentries[i] = r.pkgs[i].n;
        }
        if (shadowed) {
            int sh = 0;
            for (k = 0; k < r.pkgs[i].n; k++) {
                if (sbpk_winner(&r, r.pkgs[i].hash[k]) != i) {
                    sh++;
                }
            }
            shadowed[i] = sh;
        }
    }
    sbpk_res_free(&r);
    return n;
}

int sb_pack_coverage(const char *packs_json, sb_pack_cov_t *cov, int *covby,
                     int cap)
{
    sbpk_res_t r;
    int i, k, c, w, n = sbpk_resolve(packs_json, &r);
    if (n < 0) {
        return -1;
    }
    w = cap < n ? cap : n;
    if (covby && cap > 0) {
        memset(covby, 0, sizeof *covby * (size_t)cap * (size_t)cap);
    }
    for (i = 0; i < w; i++) {
        if (cov) {
            for (c = 0; c < SB_PACK_NCAT; c++) {
                cov[i].cat[c] = r.loaded[i] ? 0 : -1;
                cov[i].cat2[c] = 0;
                cov[i].cat4[c] = 0;
            }
        }
        if (!r.loaded[i]) {
            continue;
        }
        for (k = 0; k < r.pkgs[i].n; k++) {
            int ci = (unsigned char)r.pkgs[i].cat[k] % SB_PACK_NCAT;
            int win;
            if (cov) {
                cov[i].cat[ci]++;
                if (r.pkgs[i].var[k] & 1) {
                    cov[i].cat2[ci]++;
                }
                if (r.pkgs[i].var[k] & 2) {
                    cov[i].cat4[ci]++;
                }
            }
            win = sbpk_winner(&r, r.pkgs[i].hash[k]);
            if (covby && win >= 0 && win != i && win < w) {
                covby[i * cap + win]++;
            }
        }
    }
    sbpk_res_free(&r);
    return n;
}

int sb_pack_read_cfg(const char *packs_json, sb_pack_order_t *ord, int ocap,
                     char *level, unsigned lcap, sb_pack_cap_t *caps, int ccap,
                     sb_pack_over_t *overs, int vcap)
{
    char dir[600];
    jnode_t *j = sbpk_read_pj(packs_json, dir, sizeof dir);
    const jnode_t *order, *cc, *ov;
    const char *lv;
    int i, n = 0;
    (void)dir;
    if (level && lcap) {
        strcpy_s(level, lcap, "x4");
    }
    if (!j) {
        return -1;
    }
    lv = jstrv(j, "level");
    if (level && lcap && lv && (strcmp(lv, "x2") == 0 ||
                                strcmp(lv, "x4") == 0 ||
                                strcmp(lv, "orig") == 0)) {
        strcpy_s(level, lcap, lv);
    }
    order = jget(j, "order");
    if (order && order->kind == 6) {
        for (i = 0; i < order->n; i++) {
            const jnode_t *o = order->items[i];
            const jnode_t *en;
            const char *fn, *id;
            if (!o || o->kind != 5) {
                continue;
            }
            fn = jstrv(o, "file");
            id = jstrv(o, "id");
            if (!fn || !fn[0]) {
                continue;
            }
            if (n < ocap && ord) {
                strncpy_s(ord[n].file, sizeof ord[n].file, fn, _TRUNCATE);
                strncpy_s(ord[n].id, sizeof ord[n].id, id ? id : "?",
                          _TRUNCATE);
                en = jget(o, "enabled");
                ord[n].enabled = !(en && en->kind == 1);
            }
            n++;
        }
    }
    cc = jget(j, "category_level");
    if (cc && cc->kind == 5 && caps && ccap > 0) {
        int w = 0;
        for (i = 0; i < cc->n && w < ccap; i++) {
            if (!cc->keys[i] || !cc->vals[i] || cc->vals[i]->kind != 4) {
                continue;
            }
            strncpy_s(caps[w].name, sizeof caps[w].name, cc->keys[i],
                      _TRUNCATE);
            caps[w].cap = lvl_of(cc->vals[i]->s);
            w++;
        }
    }
    ov = jget(j, "overrides");
    if (ov && ov->kind == 5 && overs && vcap > 0) {
        int w = 0;
        for (i = 0; i < ov->n && w < vcap; i++) {
            u64 hv;
            if (!ov->keys[i] || !ov->vals[i] || ov->vals[i]->kind != 4) {
                continue;
            }
            if (!hex16(ov->keys[i], &hv)) {
                continue;
            }
            strncpy_s(overs[w].hash, sizeof overs[w].hash, ov->keys[i],
                      _TRUNCATE);
            overs[w].cap = lvl_of(ov->vals[i]->s);
            w++;
        }
    }
    jfree(j);
    return n;
}

/* JSON-Text maskiert anhaengen (UTF-8 bleibt, Steuerzeichen als \u00XX). */
static void sbpk_jesc(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            fputc('\\', f);
            fputc(c, f);
        } else if (c == '\n') {
            fputs("\\n", f);
        } else if (c == '\r') {
            fputs("\\r", f);
        } else if (c == '\t') {
            fputs("\\t", f);
        } else if (c < 0x20) {
            fprintf(f, "\\u%04x", c);
        } else {
            fputc(c, f);
        }
    }
    fputc('"', f);
}

static const char *sbpk_lvlstr(int cap)
{
    if (cap <= 0) {
        return "orig";
    }
    if (cap <= 2) {
        return "x2";
    }
    return "x4";
}

int sb_pack_write_cfg(const char *packs_json, const sb_pack_order_t *ord,
                      int n, const char *level, const sb_pack_cap_t *caps,
                      int nc, const sb_pack_over_t *overs, int nv)
{
    FILE *f = NULL;
    int i;
    if (fopen_s(&f, packs_json, "wb") != 0 || !f) {
        return 0;
    }
    fputs("{ \"version\": 1,\n  \"order\": [", f);
    for (i = 0; i < n; i++) {
        fputs(i ? ",\n    { " : "\n    { ", f);
        fputs("\"id\": ", f);
        sbpk_jesc(f, ord[i].id);
        fputs(", \"file\": ", f);
        sbpk_jesc(f, ord[i].file);
        fprintf(f, ", \"enabled\": %s }", ord[i].enabled ? "true" : "false");
    }
    fputs(n ? "\n  ],\n" : " ],\n", f);
    fprintf(f, "  \"level\": \"%s\",\n",
            (level && (strcmp(level, "x2") == 0 || strcmp(level, "x4") == 0 ||
                       strcmp(level, "orig") == 0)) ? level : "x4");
    fputs("  \"category_level\": {", f);
    for (i = 0; i < nc; i++) {
        fprintf(f, "%s ", i ? "," : "");
        sbpk_jesc(f, caps[i].name);
        fprintf(f, ": \"%s\"", sbpk_lvlstr(caps[i].cap));
    }
    fputs(" },\n  \"overrides\": {", f);
    for (i = 0; i < nv; i++) {
        fprintf(f, "%s ", i ? "," : "");
        sbpk_jesc(f, overs[i].hash);
        fprintf(f, ": \"%s\"", sbpk_lvlstr(overs[i].cap));
    }
    fputs(" } }\n", f);
    fclose(f);
    return 1;
}
/* W199: bounded reuse of the pack inflater; caller validates ZIP and CRC. */
int sb_save_inflate(const unsigned char *src, size_t n, unsigned char **dst, size_t expected)
{
    size_t got = inflate_raw(src, n, dst, expected + 16);
    if (got == (size_t)-1 || got != expected) { free(*dst); *dst = NULL; return 0; }
    return 1;
}
