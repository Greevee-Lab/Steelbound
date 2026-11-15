/* sb_diagcore.c - W279 DIAG-CRASH: gemeinsame Diagnose-Bausteine (Client +
 * Launcher). Beschreibung s. sb_diagcore.h. Die Bereinigung stammt aus
 * sb_feedback.c (W188) und ist dort unveraendert weiter in Gebrauch. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>
#include "sb_diagcore.h"
#include "sb_sha256.h"

/* ------------------------------------------------------------------ */
/* Bereinigen (W188-Regeln)                                            */
/* ------------------------------------------------------------------ */
static int dc_ieq(char a, char b)
{
    if (a == '/') { a = '\\'; }
    if (b == '/') { b = '\\'; }
    if (a >= 'A' && a <= 'Z') { a = (char)(a + 32); }
    if (b >= 'A' && b <= 'Z') { b = (char)(b + 32); }
    return a == b;
}

static int dc_alnum(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

/* Alle Vorkommen von needle (ohne Gross/Klein, / == \) durch rep ersetzen.
 * word=1: nur ohne Buchstabe/Ziffer davor/danach. */
static int dc_replace(char *s, size_t cap, const char *needle, const char *rep, int word)
{
    size_t nl = needle ? strlen(needle) : 0, rl = strlen(rep), i = 0, sl = strlen(s);
    int n = 0;
    if (nl < 2) { return 0; }
    while (i + nl <= sl) {
        size_t k;
        for (k = 0; k < nl && dc_ieq(s[i + k], needle[k]); k++) {}
        if (k == nl && (!word || ((i == 0 || !dc_alnum(s[i - 1])) && !dc_alnum(s[i + nl])))) {
            if (sl - nl + rl + 1 > cap) { break; }
            memmove(s + i + rl, s + i + nl, sl - i - nl + 1);
            memcpy(s + i, rep, rl);
            sl = sl - nl + rl;
            i += rl;
            n++;
        } else {
            i++;
        }
    }
    return n;
}

static int dc_count(const char *s, const char *needle)
{
    size_t nl = needle ? strlen(needle) : 0, i, k;
    int n = 0;
    if (nl < 3) { return 0; }
    for (i = 0; s[i]; i++) {
        for (k = 0; k < nl && s[i + k] && dc_ieq(s[i + k], needle[k]); k++) {}
        if (k == nl) { n++; }
    }
    return n;
}

static int dc_pathch(char c)
{
    return c && c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != '"' && c != '\'' && c != '<' && c != '>' &&
           c != '|' && c != ',' && c != ';' && c != '(' && c != ')' && c != '[' && c != ']';
}

/* Absolute Pfade (X:\..., X:/..., \\host\...) -> <pfad>\<datei> bzw. <pfad>. */
static int dc_abs_paths(char *s, size_t cap)
{
    size_t i = 0, sl = strlen(s);
    int n = 0;
    while (s[i]) {
        size_t st = i, en, b;
        int abs = 0;
        if (((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z')) && s[i + 1] == ':' &&
            (s[i + 2] == '\\' || s[i + 2] == '/') && (i == 0 || !dc_alnum(s[i - 1]))) {
            abs = 1;
        } else if (s[i] == '\\' && s[i + 1] == '\\' && dc_alnum(s[i + 2]) && (i == 0 || s[i - 1] != '\\')) {
            abs = 1;
        }
        if (!abs) { i++; continue; }
        if (st > 0 && (s[st - 1] == '\'' || s[st - 1] == '"')) { /* in Anfuehrung: bis zum Ende (Leerzeichen im Pfad) */
            char q = s[st - 1];
            for (en = st + 2; s[en] && s[en] != q && s[en] != '\n' && s[en] != '\r'; en++) {}
        } else {
            for (en = st + 2; dc_pathch(s[en]); en++) {}
        }
        /* Dateiname = letzter Teil mit Punkt (Endung); Ordner -> nur <pfad> */
        for (b = en; b > st && s[b - 1] != '\\' && s[b - 1] != '/'; b--) {}
        {
            char rep[96];
            size_t bl = en - b, rl;
            if (bl > 0 && bl < 48 && memchr(s + b, '.', bl) && b > st + 3) {
                _snprintf_s(rep, sizeof rep, _TRUNCATE, "<pfad>\\%.*s", (int)bl, s + b);
            } else {
                strcpy_s(rep, sizeof rep, "<pfad>");
            }
            rl = strlen(rep);
            if (sl - (en - st) + rl + 1 > cap) { break; }
            memmove(s + st + rl, s + en, sl - en + 1);
            memcpy(s + st, rep, rl);
            sl = sl - (en - st) + rl;
            i = st + rl;
            n++;
        }
    }
    return n;
}

static void dc_env(const char *name, char *o, size_t cap, UINT cp)
{
    wchar_t wn[64], value[600];
    DWORD r;
    o[0] = '\0';
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, wn, 64)) { return; }
    r = GetEnvironmentVariableW(wn, value, 600);
    if (r && r < 600) {
        if (cp == CP_ACP) {
            DWORD i;
            for (i = 0; i < r && value[i] < 128; ++i) {}
            if (i == r || GetACP() == CP_UTF8) { return; } /* schon im UTF-8-Durchgang bereinigt */
        }
        WideCharToMultiByte(cp, 0, value, -1, o, (int)cap, NULL, NULL);
    }
}

/* Wurzel (UTF-8) in Kodierung cp; leer, wenn gleich dem UTF-8-Durchgang. */
static void dc_root_cp(const char *u8, char *o, size_t cap, UINT cp)
{
    wchar_t w[1100];
    o[0] = '\0';
    if (cp == CP_UTF8) {
        strcpy_s(o, cap, u8);
        return;
    }
    if (GetACP() == CP_UTF8 || !MultiByteToWideChar(CP_UTF8, 0, u8, -1, w, 1100)) { return; }
    WideCharToMultiByte(CP_ACP, 0, w, -1, o, (int)cap, NULL, NULL);
    if (strcmp(o, u8) == 0) { o[0] = '\0'; }
}

int sb_diag_sanitize(char *s, size_t cap, const sb_diag_root_t *roots, int nroots)
{
    static const char *const envp[][2] = {
        { "LOCALAPPDATA", "%LOCALAPPDATA%" }, { "APPDATA", "%APPDATA%" }, { "TEMP", "%TEMP%" },
        { "TMP", "%TEMP%" }, { "OneDrive", "%OneDrive%" }, { "USERPROFILE", "%USERPROFILE%" },
        { "PUBLIC", "%PUBLIC%" },
    };
    char v[1100];
    size_t i;
    unsigned pass;
    unsigned passes;
    int n = 0, r;
    if (!s || !cap) { return 0; }
    /* UTF-8-Eingabe und alte ANSI-Logzeilen beide bereinigen. */
    passes = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0) ? 1u : 2u;
    for (pass = 0; pass < passes; ++pass) {
        UINT cp = pass ? CP_ACP : CP_UTF8;
        for (r = 0; roots && r < nroots; r++) { /* W279: Datenordner/Client-Ordner zuerst */
            if (!roots[r].path || !roots[r].tag) { continue; }
            dc_root_cp(roots[r].path, v, sizeof v, cp);
            {
                size_t l = strlen(v);
                while (l > 3 && (v[l - 1] == '\\' || v[l - 1] == '/')) { v[--l] = '\0'; }
            }
            if (strlen(v) >= 4) { n += dc_replace(s, cap, v, roots[r].tag, 0); }
        }
        for (i = 0; i < sizeof envp / sizeof envp[0]; i++) {
            dc_env(envp[i][0], v, sizeof v, cp);
            if (strlen(v) >= 4) { n += dc_replace(s, cap, v, envp[i][1], 0); }
        }
        dc_env("USERNAME", v, sizeof v, cp);
        if (strlen(v) >= 2) { n += dc_replace(s, cap, v, "<user>", strlen(v) < 4); }
        dc_env("COMPUTERNAME", v, sizeof v, cp);
        if (strlen(v) >= 3) { n += dc_replace(s, cap, v, "<pc>", strlen(v) < 5); }
        dc_env("USERDOMAIN", v, sizeof v, cp);
        if (strlen(v) >= 3) { n += dc_replace(s, cap, v, "<pc>", strlen(v) < 5); }
    }
    n += dc_abs_paths(s, cap);
    return n;
}

int sb_diag_clean_hits(const char *s)
{
    char v[600];
    int n = 0;
    unsigned pass;
    unsigned passes;
    if (!s) { return 0; }
    passes = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0) ? 1u : 2u;
    for (pass = 0; pass < passes; ++pass) {
        UINT cp = pass ? CP_ACP : CP_UTF8;
        dc_env("USERNAME", v, sizeof v, cp);
        if (strlen(v) >= 4) { n += dc_count(s, v); }
        dc_env("USERPROFILE", v, sizeof v, cp);
        if (strlen(v) >= 4) { n += dc_count(s, v); }
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Eingabe-Zeilen entfernen (Nutzervorgabe 15.11.)                      */
/* ------------------------------------------------------------------ */
int sb_diag_is_input_line(const char *l)
{
    static const char *const tags[] = { "INPUT", "INDIAG", "AIMSTEP", "XIN", "MOUSE", "SBC", "JOY", "HOTAS",
                                        "KEY", "TAP", "PAD", "BIND", "INREC" };
    const char *m = l;
    size_t i;
    int br = 0;
    /* Kopf "[t] [LEVEL] [Txxxx] " ueberspringen (bis zu drei Klammern) */
    while (*m == '[' && br < 3) {
        const char *e = strchr(m, ']');
        if (!e) { break; }
        m = e + 1;
        while (*m == ' ') { m++; }
        br++;
    }
    for (i = 0; i < sizeof tags / sizeof tags[0]; i++) {
        size_t n = strlen(tags[i]);
        if (_strnicmp(m, tags[i], n) == 0 && !dc_alnum(m[n]) && m[n] != '_') { return 1; }
    }
    return 0;
}

int sb_diag_drop_input_lines(char *s)
{
    char *r = s, *w = s;
    int n = 0;
    if (!s) { return 0; }
    while (*r) {
        char *e = strchr(r, 10);
        size_t l = e ? (size_t)(e - r) + 1 : strlen(r);
        if (sb_diag_is_input_line(r)) {
            n++;
        } else {
            if (w != r) { memmove(w, r, l); }
            w += l;
        }
        r += l;
    }
    *w = 0;
    return n;
}

/* ------------------------------------------------------------------ */
/* Pack-Pruefsumme                                                     */
/* ------------------------------------------------------------------ */
static unsigned dc_u32(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

int sb_diag_pack_fp(const char *path, char *hex, size_t cap)
{
    DWORD a;
    unsigned char dg[32];
    char h[65];
    if (!path || !path[0] || !hex || cap < 65) { return 0; }
    hex[0] = '\0';
    a = GetFileAttributesA(path);
    if (a == INVALID_FILE_ATTRIBUTES) { return 0; }
    if (a & FILE_ATTRIBUTE_DIRECTORY) {
        char m[1200];
        _snprintf_s(m, sizeof m, _TRUNCATE, "%s\\manifest.json", path);
        if (sb_sha256_file(m, dg) != 0) { return 0; }
        sb_sha256_hex(dg, h);
        strcpy_s(hex, cap, h);
        return 2;
    }
    {
        HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        LARGE_INTEGER sz, off;
        unsigned char *tail = NULL, *cd = NULL;
        DWORD want, got = 0, i;
        int ok = 0;
        sb_sha256_t c;
        if (f == INVALID_HANDLE_VALUE) { return 0; }
        if (!GetFileSizeEx(f, &sz) || sz.QuadPart < 22) { CloseHandle(f); return 0; }
        want = (DWORD)(sz.QuadPart > 65557 ? 65557 : sz.QuadPart);
        tail = (unsigned char *)malloc(want);
        off.QuadPart = sz.QuadPart - want;
        if (tail && SetFilePointerEx(f, off, NULL, FILE_BEGIN) && ReadFile(f, tail, want, &got, NULL) &&
            got == want) {
            sb_sha256_init(&c);
            sb_sha256_update(&c, &sz.QuadPart, 8);
            for (i = want - 22 + 1; i-- > 0;) {
                if (dc_u32(tail + i) == 0x06054b50u) {
                    unsigned cds = dc_u32(tail + i + 12), cdo = dc_u32(tail + i + 16);
                    if (cdo != 0xFFFFFFFFu && (unsigned long long)cdo + cds <= (unsigned long long)sz.QuadPart &&
                        cds <= 64u * 1024 * 1024 && (cd = (unsigned char *)malloc(cds ? cds : 1)) != NULL) {
                        DWORD g2 = 0;
                        off.QuadPart = cdo;
                        if (SetFilePointerEx(f, off, NULL, FILE_BEGIN) && ReadFile(f, cd, cds, &g2, NULL) && g2 == cds) {
                            sb_sha256_update(&c, cd, cds);
                            ok = 1;
                        }
                    }
                    break;
                }
            }
            if (!ok) { /* kein/Zip64-Verzeichnis: Groesse + letzte 64 KB */
                sb_sha256_update(&c, tail, want);
                ok = 1;
            }
            sb_sha256_final(&c, dg);
            sb_sha256_hex(dg, h);
            strcpy_s(hex, cap, h);
        }
        free(cd);
        free(tail);
        CloseHandle(f);
        return ok ? 1 : 0;
    }
}

/* ------------------------------------------------------------------ */
/* cfg-Abweichungen                                                     */
/* ------------------------------------------------------------------ */
#define DC_KV_MAX 900
typedef struct {
    char k[64];
    char v[264];
    const char *src;
} dc_kv_t;

static void dc_trim(char **b, char **e)
{
    while (*b < *e && (**b == ' ' || **b == '\t')) { (*b)++; }
    while (*e > *b && ((*e)[-1] == ' ' || (*e)[-1] == '\t' || (*e)[-1] == '\r' || (*e)[-1] == '\n')) { (*e)--; }
}

static int dc_kv_find(dc_kv_t *t, int n, const char *k)
{
    int i;
    for (i = 0; i < n; i++) {
        if (strcmp(t[i].k, k) == 0) { return i; }
    }
    return -1;
}

/* key=value setzen (spaetere Quelle gewinnt). */
static int dc_kv_put(dc_kv_t *t, int n, const char *kb, size_t kl, const char *vb, size_t vl, const char *src)
{
    char k[64];
    int i;
    if (kl == 0 || kl >= sizeof k) { return n; }
    memcpy(k, kb, kl);
    k[kl] = '\0';
    i = dc_kv_find(t, n, k);
    if (i < 0) {
        if (n >= DC_KV_MAX) { return n; }
        i = n++;
        strcpy_s(t[i].k, sizeof t[i].k, k);
    }
    if (vl >= sizeof t[i].v) { vl = sizeof t[i].v - 1; }
    memcpy(t[i].v, vb, vl);
    t[i].v[vl] = '\0';
    t[i].src = src;
    return n;
}

static int dc_kv_file(dc_kv_t *t, int n, const char *path, const char *src, int *ok)
{
    FILE *f = NULL;
    char line[4096];
    wchar_t wp[1100];
    *ok = 0;
    if (!path || !path[0] || !MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 1100) || _wfopen_s(&f, wp, L"r") != 0 ||
        !f) {
        return n;
    }
    *ok = 1;
    while (fgets(line, sizeof line, f)) {
        char *k = line + strspn(line, " \t"), *eq, *ke, *v, *ve;
        if (*k == '#' || *k == ';' || *k == '[' || *k == '\n' || *k == '\0') { continue; }
        eq = strchr(k, '=');
        if (!eq) { continue; }
        ke = eq;
        v = eq + 1;
        ve = v + strlen(v);
        dc_trim(&k, &ke);
        dc_trim(&v, &ve);
        n = dc_kv_put(t, n, k, (size_t)(ke - k), v, (size_t)(ve - v), src);
    }
    fclose(f);
    return n;
}

int sb_diag_cfg_diff(const char *base_cfg, const char *user_cfg, const char *set_chain, char *out, size_t cap)
{
    dc_kv_t *base = (dc_kv_t *)calloc(DC_KV_MAX, sizeof *base);
    dc_kv_t *ov = (dc_kv_t *)calloc(DC_KV_MAX, sizeof *ov);
    int nb = 0, no = 0, i, lines = 0, bok = 0, uok = 0;
    size_t used = 0;
    if (!out || !cap) { free(base); free(ov); return 0; }
    out[0] = '\0';
    if (!base || !ov) { free(base); free(ov); return 0; }
    nb = dc_kv_file(base, nb, base_cfg, "sb_native.cfg", &bok);
    no = dc_kv_file(ov, no, user_cfg, "user.cfg", &uok);
    if (set_chain && set_chain[0]) {
        const char *p = set_chain;
        while (*p) {
            const char *e = strchr(p, ';'), *eq;
            char *kb, *ke, *vb, *ve;
            char tmp[600];
            size_t l;
            if (!e) { e = p + strlen(p); }
            l = (size_t)(e - p);
            if (l >= sizeof tmp) { l = sizeof tmp - 1; }
            memcpy(tmp, p, l);
            tmp[l] = '\0';
            eq = strchr(tmp, '=');
            if (eq) {
                kb = tmp;
                ke = (char *)eq;
                vb = (char *)eq + 1;
                ve = tmp + l;
                dc_trim(&kb, &ke);
                dc_trim(&vb, &ve);
                no = dc_kv_put(ov, no, kb, (size_t)(ke - kb), vb, (size_t)(ve - vb), "start");
            }
            p = *e ? e + 1 : e;
        }
    }
    for (i = 0; i < no; i++) {
        int j = dc_kv_find(base, nb, ov[i].k);
        int hide;
        if (j >= 0 && _stricmp(base[j].v, ov[i].v) == 0) { continue; }
        /* Nutzervorgabe 15.11.: Eingabe-Folgen (Replay-/Schritt-Ketten) nie im Wortlaut */
        hide = strstr(ov[i].k, "replay") || strstr(ov[i].k, "_steps") || strstr(ov[i].k, "_nav_") ||
               strstr(ov[i].k, "inrec");
        _snprintf_s(out + used, cap - used, _TRUNCATE, "%s=%s  (%s%s)\r\n", ov[i].k,
                    hide ? (ov[i].v[0] ? "<set, value omitted>" : "") : ov[i].v, ov[i].src,
                    j < 0 ? ", not in sb_native.cfg" : "");
        used = strlen(out);
        lines++;
        if (used + 8 >= cap) { break; }
    }
    free(base);
    free(ov);
    return bok ? lines : -1;
}

/* ------------------------------------------------------------------ */
/* Absturzbericht-Ordner                                               */
/* ------------------------------------------------------------------ */
int sb_diag_crash_name_ok(const char *n)
{
    size_t l, i;
    const char *k;
    if (!n) { return 0; }
    l = strlen(n);
    if (l < 20 || l >= 64) { return 0; }
    for (i = 0; i < 15; i++) {
        if (i == 8) {
            if (n[i] != '-') { return 0; }
        } else if (n[i] < '0' || n[i] > '9') {
            return 0;
        }
    }
    k = strrchr(n, '-');
    if (!k || (strcmp(k, "-crash") != 0 && strcmp(k, "-hang") != 0)) { return 0; }
    for (i = 15; n + i < k; i++) { /* optional -nnn */
        if (!(n[i] == '-' || (n[i] >= '0' && n[i] <= '9'))) { return 0; }
    }
    return 1;
}

static int dc_wexists(const wchar_t *dir, const wchar_t *name)
{
    wchar_t p[1200];
    _snwprintf_s(p, 1200, _TRUNCATE, L"%s\\%s", dir, name);
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

static int dc_cmp_desc(const void *a, const void *b)
{
    return -strcmp(((const sb_crash_ent_t *)a)->name, ((const sb_crash_ent_t *)b)->name);
}

int sb_diag_crash_list(const char *crash_dir, sb_crash_ent_t *out, int cap)
{
    wchar_t wd[1100], pat[1150];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int n = 0;
    if (!crash_dir || !crash_dir[0] || !out || cap <= 0) { return 0; }
    if (!MultiByteToWideChar(CP_UTF8, 0, crash_dir, -1, wd, 1100)) { return 0; }
    _snwprintf_s(pat, 1150, _TRUNCATE, L"%s\\*", wd);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) { return 0; }
    do {
        char nm[64];
        wchar_t sub[1200], fp[1260];
        sb_crash_ent_t *e;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            continue;
        }
        if (!WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, nm, sizeof nm, NULL, NULL) || !sb_diag_crash_name_ok(nm)) {
            continue;
        }
        if (n >= cap) { /* voll: nur ersetzen, wenn neuer als der aelteste */
            int i, old = 0;
            for (i = 1; i < cap; i++) {
                if (strcmp(out[i].name, out[old].name) < 0) { old = i; }
            }
            if (strcmp(nm, out[old].name) <= 0) { continue; }
            e = &out[old];
        } else {
            e = &out[n++];
        }
        memset(e, 0, sizeof *e);
        strcpy_s(e->name, sizeof e->name, nm);
        e->kind = strstr(nm, "-hang") ? SB_CRASH_KIND_HANG : SB_CRASH_KIND_CRASH;
        _snwprintf_s(sub, 1200, _TRUNCATE, L"%s\\%s", wd, fd.cFileName);
        e->recovered = dc_wexists(sub, L"recovered.txt");
        {
            FILE *f = NULL;
            _snwprintf_s(fp, 1260, _TRUNCATE, L"%s\\status.txt", sub);
            if (_wfopen_s(&f, fp, L"rb") == 0 && f) {
                char st[32] = "";
                size_t g = fread(st, 1, sizeof st - 1, f);
                st[g] = '\0';
                fclose(f);
                e->fatal = strncmp(st, "fatal", 5) == 0;
            }
        }
        {
            WIN32_FIND_DATAW f2;
            HANDLE h2;
            _snwprintf_s(fp, 1260, _TRUNCATE, L"%s\\*", sub);
            h2 = FindFirstFileW(fp, &f2);
            if (h2 != INVALID_HANDLE_VALUE) {
                do {
                    size_t l = wcslen(f2.cFileName);
                    if (f2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { continue; }
                    e->bytes += ((unsigned long long)f2.nFileSizeHigh << 32) | f2.nFileSizeLow;
                    if (l > 4 && _wcsicmp(f2.cFileName + l - 4, L".dmp") == 0) { e->has_dump = 1; }
                } while (FindNextFileW(h2, &f2));
                FindClose(h2);
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    qsort(out, (size_t)n, sizeof *out, dc_cmp_desc);
    return n;
}

int sb_diag_crash_counts(const sb_crash_ent_t *e)
{
    return e && (e->fatal || !e->recovered);
}

/* ------------------------------------------------------------------ */
/* Minidump bereinigen                                                 */
/* ------------------------------------------------------------------ */
/* MINIDUMP_STRING (u32 Laenge in Bytes + UTF-16) -> nur Dateiname. */
static int dc_md_name(unsigned char *d, size_t n, unsigned rva)
{
    unsigned len, i, base = 0, cnt;
    wchar_t *w;
    if ((size_t)rva + 4 > n) { return 0; }
    len = dc_u32(d + rva);
    if (len < 2 || (size_t)rva + 4 + len > n) { return 0; }
    w = (wchar_t *)(d + rva + 4);
    cnt = len / 2;
    for (i = 0; i < cnt; i++) {
        if (w[i] == L'\\' || w[i] == L'/') { base = i + 1; }
    }
    if (base == 0) { return 0; }
    memmove(w, w + base, (cnt - base) * sizeof(wchar_t));
    memset(w + (cnt - base), 0, base * sizeof(wchar_t));
    len = (cnt - base) * 2;
    d[rva] = (unsigned char)len;
    d[rva + 1] = (unsigned char)(len >> 8);
    d[rva + 2] = 0;
    d[rva + 3] = 0;
    return 1;
}

/* CodeView RSDS: PDB-Pfad -> nur Dateiname (Rest mit 0 auffuellen). */
static int dc_md_cv(unsigned char *d, size_t n, unsigned size, unsigned rva)
{
    char *p, *s, *b;
    size_t l, room;
    if (size < 25 || (size_t)rva + size > n || dc_u32(d + rva) != 0x53445352u) { return 0; } /* 'RSDS' */
    p = (char *)(d + rva + 24);
    room = size - 24;
    l = strnlen(p, room);
    if (l >= room) { return 0; }
    b = p;
    for (s = p; *s; s++) {
        if (*s == '\\' || *s == '/') { b = s + 1; }
    }
    if (b == p) { return 0; }
    memmove(p, b, strlen(b) + 1);
    memset(p + strlen(p), 0, room - strlen(p));
    return 1;
}

/* Benutzer-/Rechnername (ASCII ohne Gross/Klein, UTF-16LE) -> '_' gleicher Laenge. */
static int dc_md_word(unsigned char *d, size_t n, const wchar_t *w)
{
    size_t wl = wcslen(w), i, k;
    int hits = 0;
    char a[256];
    int al;
    if (wl < 3 || wl > 120) { return 0; }
    al = wl >= 4 ? WideCharToMultiByte(CP_ACP, 0, w, (int)wl, a, sizeof a - 1, NULL, NULL) : 0; /* ASCII erst ab 4 Zeichen */
    for (i = 0; al > 0 && i + (size_t)al <= n; i++) {
        for (k = 0; k < (size_t)al && dc_ieq((char)d[i + k], a[k]); k++) {}
        if (k == (size_t)al) {
            memset(d + i, '_', (size_t)al);
            hits++;
            i += (size_t)al - 1;
        }
    }
    for (i = 0; i + wl * 2 <= n; i++) {
        for (k = 0; k < wl; k++) {
            wchar_t c = (wchar_t)(d[i + 2 * k] | (d[i + 2 * k + 1] << 8));
            if (towlower(c) != towlower(w[k])) { break; }
        }
        if (k == wl) {
            for (k = 0; k < wl; k++) {
                d[i + 2 * k] = '_';
                d[i + 2 * k + 1] = 0;
            }
            hits++;
            i += wl * 2 - 1;
        }
    }
    return hits;
}

int sb_diag_dump_scrub(const char *dump_u8)
{
    wchar_t wp[1100];
    HANDLE f;
    LARGE_INTEGER sz;
    unsigned char *d = NULL;
    DWORD got = 0, wr = 0;
    unsigned ns, dir, i;
    int hits = 0;
    if (!dump_u8 || !MultiByteToWideChar(CP_UTF8, 0, dump_u8, -1, wp, 1100)) { return -1; }
    f = CreateFileW(wp, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) { return -1; }
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart < 32 || sz.QuadPart > 512ll * 1024 * 1024 ||
        (d = (unsigned char *)malloc((size_t)sz.QuadPart)) == NULL ||
        !ReadFile(f, d, (DWORD)sz.QuadPart, &got, NULL) || got != (DWORD)sz.QuadPart || dc_u32(d) != 0x504D444Du) {
        free(d);
        CloseHandle(f);
        return -1;
    }
    ns = dc_u32(d + 8);
    dir = dc_u32(d + 12);
    for (i = 0; i < ns && (size_t)dir + (size_t)(i + 1) * 12 <= got; i++) {
        const unsigned char *de = d + dir + i * 12;
        unsigned type = dc_u32(de), size = dc_u32(de + 4), rva = dc_u32(de + 8);
        if ((size_t)rva + size > got) { continue; }
        if (type == 4 && size >= 4) { /* ModuleListStream */
            unsigned nm = dc_u32(d + rva), m;
            for (m = 0; m < nm && 4 + (size_t)(m + 1) * 108 <= size; m++) {
                unsigned char *me = d + rva + 4 + m * 108;
                hits += dc_md_name(d, got, dc_u32(me + 20));
                hits += dc_md_cv(d, got, dc_u32(me + 76), dc_u32(me + 80));
            }
        } else if (type == 14 && size >= 12) { /* UnloadedModuleListStream */
            unsigned hs = dc_u32(d + rva), es = dc_u32(d + rva + 4), ne = dc_u32(d + rva + 8), m;
            for (m = 0; m < ne && es >= 24 && (size_t)hs + (size_t)(m + 1) * es <= size; m++) {
                hits += dc_md_name(d, got, dc_u32(d + rva + hs + m * es + 20));
            }
        }
    }
    {
        static const wchar_t *const ev[] = { L"USERNAME", L"COMPUTERNAME", L"USERDOMAIN" };
        for (i = 0; i < 3; i++) {
            wchar_t v[260];
            DWORD r = GetEnvironmentVariableW(ev[i], v, 260);
            if (r && r < 260) { hits += dc_md_word(d, got, v); }
        }
    }
    {
        LARGE_INTEGER z;
        z.QuadPart = 0;
        if (!SetFilePointerEx(f, z, NULL, FILE_BEGIN) || !WriteFile(f, d, got, &wr, NULL) || wr != got) {
            hits = -1;
        }
    }
    free(d);
    CloseHandle(f);
    return hits;
}
