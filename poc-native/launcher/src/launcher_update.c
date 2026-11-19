/* launcher_update.c - W250 UPDATE-CHECK (s. launcher_update.h).
 * Nur pruefen: kein Download, kein Selbst-Update, opt-in. Quelltext ASCII. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "launcher_update.h"
#include "launcher_app.h"
#include "launcher_u8.h"
#include "sb_version.h" /* W301 BETA-VERSION: Phase + Anzeigetext (Selbsttest) */

#define printf cli_printf

#ifndef SB_VERSION_STR
#define SB_VERSION_STR ""
#endif

const char *lupd_version(void)
{
    return SB_VERSION_STR;
}

/* ------------------------------------------------------------------ */
/* Versionsvergleich (SemVer-Vorrang)                                  */
/* ------------------------------------------------------------------ */
typedef struct {
    unsigned long core[3];
    char pre[64]; /* "" = Release-Fassung */
} ver_t;

static int is_ident_ch(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           c == '-' || c == '.';
}

static int ver_parse(const char *s, ver_t *v)
{
    int i;
    size_t n = 0;
    memset(v, 0, sizeof *v);
    if (!s) {
        return 0;
    }
    if (*s == 'v' || *s == 'V') {
        s++;
    }
    for (i = 0; i < 3; i++) {
        int d = 0;
        unsigned long x = 0;
        while (*s >= '0' && *s <= '9') {
            if (++d > 9) {
                return 0;
            }
            x = x * 10 + (unsigned long)(*s - '0');
            s++;
        }
        if (d == 0) {
            return 0;
        }
        v->core[i] = x;
        if (i < 2) {
            if (*s != '.') {
                return 0;
            }
            s++;
        }
    }
    if (*s == '-') {
        s++;
        while (*s && *s != '+') {
            if (!is_ident_ch(*s) || n + 1 >= sizeof v->pre) {
                return 0;
            }
            v->pre[n++] = *s++;
        }
        v->pre[n] = '\0';
        /* leere Bezeichner ("alpha..1", "-", "alpha.") sind ungueltig */
        if (n == 0 || v->pre[0] == '.' || v->pre[n - 1] == '.' || strstr(v->pre, "..")) {
            return 0;
        }
    }
    if (*s == '+') {
        s++;
        if (!*s) {
            return 0;
        }
        while (*s) {
            if (!is_ident_ch(*s)) {
                return 0;
            }
            s++;
        }
    }
    return *s == '\0';
}

static int all_digits(const char *s, size_t n)
{
    size_t i;
    if (n == 0) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return 0;
        }
    }
    return 1;
}

/* Prerelease-Bezeichner vergleichen (SemVer 11.4). */
static int pre_cmp(const char *a, const char *b)
{
    while (*a && *b) {
        size_t na = strcspn(a, "."), nb = strcspn(b, ".");
        int da = all_digits(a, na), db = all_digits(b, nb), c = 0;
        if (da && db) {
            /* numerisch: ohne fuehrende Nullen zuerst nach Laenge */
            while (na > 1 && *a == '0') { a++; na--; }
            while (nb > 1 && *b == '0') { b++; nb--; }
            c = na != nb ? (na < nb ? -1 : 1) : strncmp(a, b, na);
        } else if (da != db) {
            c = da ? -1 : 1; /* Zahl < Text */
        } else {
            size_t m = na < nb ? na : nb;
            c = strncmp(a, b, m);
            if (c == 0 && na != nb) {
                c = na < nb ? -1 : 1;
            }
        }
        if (c) {
            return c < 0 ? -1 : 1;
        }
        a += na;
        b += nb;
        if (*a == '.') { a++; }
        if (*b == '.') { b++; }
    }
    return *a ? 1 : *b ? -1 : 0; /* mehr Bezeichner = hoeher */
}

int lupd_ver_cmp(const char *a, const char *b, int *cmp)
{
    ver_t va, vb;
    int i, c = 0;
    if (!ver_parse(a, &va) || !ver_parse(b, &vb)) {
        return 0;
    }
    for (i = 0; i < 3 && c == 0; i++) {
        if (va.core[i] != vb.core[i]) {
            c = va.core[i] < vb.core[i] ? -1 : 1;
        }
    }
    if (c == 0) {
        if (!va.pre[0] || !vb.pre[0]) {
            c = (!va.pre[0] && !vb.pre[0]) ? 0 : (!va.pre[0] ? 1 : -1);
        } else {
            c = pre_cmp(va.pre, vb.pre);
        }
    }
    if (cmp) {
        *cmp = c;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* JSON (nur das Noetige: Array von Objekten, Schluessel der 1. Ebene) */
/* ------------------------------------------------------------------ */
typedef struct {
    const char *p, *e;
    int bad;
} js_t;

static void js_ws(js_t *j)
{
    while (j->p < j->e && (*j->p == ' ' || *j->p == '\t' || *j->p == '\r' || *j->p == '\n')) {
        j->p++;
    }
}

/* String lesen (j->p auf '"'). out darf NULL sein. Nicht-ASCII und
 * Steuerzeichen werden zu '?' (scheitern spaeter an Versions-/URL-Pruefung). */
static int js_str(js_t *j, char *out, size_t cap)
{
    size_t n = 0;
    if (j->p >= j->e || *j->p != '"') {
        j->bad = 1;
        return 0;
    }
    j->p++;
    while (j->p < j->e && *j->p != '"') {
        char c = *j->p++;
        if ((unsigned char)c < 0x20) {
            j->bad = 1;
            return 0;
        }
        if (c == '\\') {
            if (j->p >= j->e) {
                j->bad = 1;
                return 0;
            }
            c = *j->p++;
            switch (c) {
            case '"': case '\\': case '/': break;
            case 'b': case 'f': case 'n': case 'r': case 't': c = '?'; break;
            case 'u': {
                int k;
                unsigned v = 0;
                for (k = 0; k < 4; k++) {
                    char h = j->p < j->e ? *j->p++ : 0;
                    v <<= 4;
                    if (h >= '0' && h <= '9') v |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') v |= (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') v |= (unsigned)(h - 'A' + 10);
                    else { j->bad = 1; return 0; }
                }
                c = (v >= 0x20 && v < 0x7f) ? (char)v : '?';
                break;
            }
            default:
                j->bad = 1;
                return 0;
            }
        } else if ((unsigned char)c >= 0x80) {
            c = '?';
        }
        if (out && n + 1 < cap) {
            out[n++] = c;
        } else if (out) {
            n = cap; /* zu lang: als unlesbar markieren */
        }
    }
    if (j->p >= j->e) {
        j->bad = 1;
        return 0;
    }
    j->p++;
    if (out && cap) {
        if (n >= cap) {
            out[0] = '\0'; /* abgeschnitten = verwerfen */
        } else {
            out[n] = '\0';
        }
    }
    return 1;
}

/* Beliebigen Wert ueberspringen (verschachtelt, iterativ). */
static void js_skip(js_t *j)
{
    int depth = 0;
    do {
        js_ws(j);
        if (j->p >= j->e) {
            j->bad = 1;
            return;
        }
        if (*j->p == '"') {
            if (!js_str(j, NULL, 0)) {
                return;
            }
        } else if (*j->p == '{' || *j->p == '[') {
            if (++depth > 64) {
                j->bad = 1;
                return;
            }
            j->p++;
        } else if (*j->p == '}' || *j->p == ']') {
            if (depth == 0) {
                j->bad = 1;
                return;
            }
            depth--;
            j->p++;
        } else if (*j->p == ',' || *j->p == ':') {
            if (depth == 0) {
                j->bad = 1;
                return;
            }
            j->p++;
        } else {
            /* Zahl / true / false / null */
            const char *s = j->p;
            while (j->p < j->e && *j->p && strchr("+-.0123456789eEtrufalsn", *j->p)) {
                j->p++;
            }
            if (j->p == s) {
                j->bad = 1;
                return;
            }
        }
    } while (depth > 0 && !j->bad);
}

/* Literal true/false lesen (sonst ueberspringen, Ergebnis 0). */
static int js_bool(js_t *j)
{
    js_ws(j);
    if (j->e - j->p >= 4 && strncmp(j->p, "true", 4) == 0) {
        j->p += 4;
        return 1;
    }
    js_skip(j);
    return 0;
}

int lupd_pick(const char *json, size_t len, lupd_rel_t *best)
{
    js_t j;
    int n = 0;
    memset(best, 0, sizeof *best);
    if (!json) {
        return -1;
    }
    j.p = json;
    j.e = json + len;
    j.bad = 0;
    js_ws(&j);
    if (j.p >= j.e || *j.p != '[') {
        return -1;
    }
    j.p++;
    js_ws(&j);
    if (j.p < j.e && *j.p == ']') {
        return 0;
    }
    while (!j.bad) {
        lupd_rel_t r;
        int draft = 0, cmp = 0;
        memset(&r, 0, sizeof r);
        js_ws(&j);
        if (j.p >= j.e || *j.p != '{') {
            return -1;
        }
        j.p++;
        js_ws(&j);
        if (j.p < j.e && *j.p == '}') {
            j.p++;
        } else {
            while (!j.bad) {
                char key[32];
                js_ws(&j);
                if (!js_str(&j, key, sizeof key)) {
                    return -1;
                }
                js_ws(&j);
                if (j.p >= j.e || *j.p != ':') {
                    return -1;
                }
                j.p++;
                js_ws(&j);
                if (strcmp(key, "tag_name") == 0 && j.p < j.e && *j.p == '"') {
                    js_str(&j, r.tag, sizeof r.tag);
                } else if (strcmp(key, "html_url") == 0 && j.p < j.e && *j.p == '"') {
                    js_str(&j, r.url, sizeof r.url);
                } else if (strcmp(key, "draft") == 0) {
                    draft = js_bool(&j);
                } else if (strcmp(key, "prerelease") == 0) {
                    r.prerelease = js_bool(&j);
                } else {
                    js_skip(&j);
                }
                js_ws(&j);
                if (j.p < j.e && *j.p == ',') {
                    j.p++;
                    continue;
                }
                if (j.p < j.e && *j.p == '}') {
                    j.p++;
                    break;
                }
                return -1;
            }
        }
        if (j.bad) {
            return -1;
        }
        /* Entwuerfe zaehlen nie; unlesbare Tags werden uebergangen. */
        if (!draft && r.tag[0] && lupd_ver_cmp(r.tag, r.tag, &cmp)) {
            if (n == 0 || (lupd_ver_cmp(r.tag, best->tag, &cmp) && cmp > 0)) {
                *best = r;
            }
            n++;
        }
        js_ws(&j);
        if (j.p < j.e && *j.p == ',') {
            j.p++;
            continue;
        }
        if (j.p < j.e && *j.p == ']') {
            return n;
        }
        return -1;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* URL-Pruefung                                                        */
/* ------------------------------------------------------------------ */
int lupd_url_ok(const char *url)
{
    size_t lp = strlen(LUPD_PAGE_PREFIX), n;
    const char *s;
    if (!url || strncmp(url, LUPD_PAGE_PREFIX, lp) != 0) {
        return 0;
    }
    n = strlen(url);
    if (n == lp || n > 300 || strstr(url, "..")) {
        return 0;
    }
    for (s = url + lp; *s; s++) {
        char c = *s;
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              c == '.' || c == '_' || c == '~' || c == '/' || c == '%' || c == '+' || c == '-')) {
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Auswertung + Abruf                                                  */
/* ------------------------------------------------------------------ */
void lupd_eval(const char *cur, const char *json, size_t len, lupd_res_t *r)
{
    int n, cmp = 0;
    n = lupd_pick(json, len, &r->rel);
    if (n <= 0 || !lupd_ver_cmp(r->rel.tag, cur, &cmp)) {
        r->result = LUPD_R_BAD;
        return;
    }
    r->result = cmp > 0 ? LUPD_R_NEWER : LUPD_R_CURRENT;
}

void lupd_fetch(const char *cur, lupd_res_t *r)
{
    HINTERNET ses = NULL, con = NULL, req = NULL;
    wchar_t ua[96], hdrs[4096];
    char *body = NULL;
    size_t len = 0;
    DWORD status = 0, sz = sizeof status, opt;
    int cmp;
    memset(r, 0, sizeof *r);
    if (!cur || !cur[0] || !lupd_ver_cmp(cur, cur, &cmp)) {
        r->result = LUPD_R_NOVER;
        return;
    }
    r->result = LUPD_R_NET;
    _snwprintf_s(ua, 96, _TRUNCATE, L"Steelbound-Launcher/%S", cur);
    /* System-Proxy (WinHTTP/WPAD), keine Anmeldung, keine Cookies. */
    ses = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                      WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) {
        ses = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                          WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!ses) {
        return;
    }
    WinHttpSetTimeouts(ses, 3000, 3000, 3000, 4000);
    /* W332 SEC: nur TLS 1.2/1.3 (Zertifikatspruefung bleibt Standard: kein
     * SECURITY_FLAG_IGNORE_*). Ohne TLS-1.3-Unterstuetzung (aeltere Windows)
     * nur 1.2. */
    opt = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    if (!WinHttpSetOption(ses, WINHTTP_OPTION_SECURE_PROTOCOLS, &opt, sizeof opt)) {
        opt = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(ses, WINHTTP_OPTION_SECURE_PROTOCOLS, &opt, sizeof opt);
    }
    con = WinHttpConnect(ses, L"" LUPD_API_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (con) {
        req = WinHttpOpenRequest(con, L"GET", L"" LUPD_API_PATH, NULL, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    }
    if (req) {
        opt = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION | WINHTTP_DISABLE_KEEP_ALIVE;
        WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &opt, sizeof opt);
        opt = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH; /* nie Windows-Anmeldedaten senden */
        WinHttpSetOption(req, WINHTTP_OPTION_AUTOLOGON_POLICY, &opt, sizeof opt);
        /* W332 SEC: fester Host - keine Weiterleitung (auch nicht https->https). */
        opt = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &opt, sizeof opt);
    }
    if (req && WinHttpSendRequest(req, L"Accept: application/vnd.github+json\r\n", (DWORD)-1L,
                                  WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(req, NULL)) {
        DWORD hs = sizeof hdrs;
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF | WINHTTP_QUERY_FLAG_REQUEST_HEADERS,
                                WINHTTP_HEADER_NAME_BY_INDEX, hdrs, &hs, WINHTTP_NO_HEADER_INDEX)) {
            WideCharToMultiByte(CP_UTF8, 0, hdrs, -1, r->req_headers, (int)sizeof r->req_headers,
                                NULL, NULL);
        }
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
        r->http_status = (int)status;
        if (status != 200) {
            r->result = LUPD_R_HTTP;
        } else {
            body = (char *)malloc(LUPD_MAX_BODY);
            r->result = LUPD_R_BAD;
            if (body) {
                for (;;) {
                    DWORD avail = 0, got = 0;
                    if (!WinHttpQueryDataAvailable(req, &avail)) {
                        r->result = LUPD_R_NET;
                        len = 0;
                        break;
                    }
                    if (avail == 0) {
                        break;
                    }
                    if (len + avail > LUPD_MAX_BODY) { /* zu gross: verwerfen */
                        len = 0;
                        break;
                    }
                    if (!WinHttpReadData(req, body + len, avail, &got)) {
                        r->result = LUPD_R_NET;
                        len = 0;
                        break;
                    }
                    len += got;
                }
                r->bytes = len;
                if (len) {
                    lupd_eval(cur, body, len, r);
                }
                free(body);
            }
        }
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
}

/* ------------------------------------------------------------------ */
/* Einstellung in user.cfg                                             */
/* ------------------------------------------------------------------ */
/* Zeile "update_check=N" (fuehrende Leerzeichen erlaubt)? -> Zeiger auf N */
static const char *cfg_key_line(const char *line)
{
    size_t kl = strlen(LUPD_CFG_KEY);
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    if (strncmp(line, LUPD_CFG_KEY, kl) != 0) {
        return NULL;
    }
    line += kl;
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    return *line == '=' ? line + 1 : NULL;
}

int lupd_cfg_get(const char *path)
{
    FILE *f = NULL;
    char line[512];
    int v = -1;
    if (!path || u8_fopen_s(&f, path, "r") != 0 || !f) {
        return -1;
    }
    while (fgets(line, sizeof line, f)) {
        const char *val = cfg_key_line(line);
        if (val) {
            v = atoi(val) ? 1 : 0; /* letzter gewinnt (wie hcfg) */
        }
    }
    fclose(f);
    return v;
}

int lupd_cfg_set(const char *path, int on)
{
    FILE *f = NULL;
    char line[1024], *out;
    size_t n = 0, cap = 1024 * 1024;
    int done = 0;
    out = (char *)malloc(cap);
    if (!out) {
        return -1;
    }
    if (u8_fopen_s(&f, path, "r") == 0 && f) {
        while (fgets(line, sizeof line, f)) {
            size_t l;
            if (cfg_key_line(line)) {
                if (done) {
                    continue; /* doppelte Zeilen entfernen */
                }
                _snprintf_s(line, sizeof line, _TRUNCATE, "%s=%d\n", LUPD_CFG_KEY, on ? 1 : 0);
                done = 1;
            }
            l = strlen(line);
            if (n + l + 128 >= cap) {
                fclose(f);
                free(out);
                return -1; /* user.cfg ist real < 4 KB */
            }
            memcpy(out + n, line, l);
            n += l;
        }
        fclose(f);
        if (n && out[n - 1] != '\n') {
            out[n++] = '\n';
        }
    }
    if (!done) {
        n += (size_t)_snprintf_s(out + n, cap - n, _TRUNCATE,
                                 "# launcher: check for new versions at startup (1 = yes, 0 = no)\n"
                                 "%s=%d\n", LUPD_CFG_KEY, on ? 1 : 0);
    }
    if (u8_fopen_s(&f, path, "w") != 0 || !f) {
        free(out);
        return -1;
    }
    if (fwrite(out, 1, n, f) != n) {
        fclose(f);
        free(out);
        return -1;
    }
    fclose(f);
    free(out);
    return 0;
}

int lupd_should_ask(const char *version, int cfg_state)
{
    return version && version[0] && cfg_state < 0;
}

int lupd_auto_check(const char *version, int cfg_state)
{
    return version && version[0] && cfg_state == 1;
}

/* ------------------------------------------------------------------ */
/* Selbsttest (ohne Netz)                                              */
/* ------------------------------------------------------------------ */
void lupd_selftest(int *pass, int *fail)
{
    char why[256] = "";
    int i, c;
    /* a) Versionsvergleich */
    static const struct { const char *a, *b; int want; } kCmp[] = {
        { "0.1.0-alpha.10", "0.1.0-alpha.9", 1 },
        { "0.1.0-alpha.9", "0.1.0-alpha.10", -1 },
        { "0.1.0-alpha.7", "0.1.0-alpha.7", 0 },
        { "v0.1.0-alpha.7", "0.1.0-alpha.7", 0 },
        { "0.1.0-alpha.6", "0.1.0-alpha.7", -1 },
        { "0.1.0-beta.1", "0.1.0-alpha.12", 1 },
        { "0.1.0", "0.1.0-beta.3", 1 },
        { "0.1.0-rc.1", "0.1.0", -1 },
        { "0.2.0-alpha.1", "0.1.9", 1 },
        { "0.1.10", "0.1.9", 1 },
        { "1.0.0", "0.99.99", 1 },
        { "0.1.0-alpha", "0.1.0-alpha.1", -1 },
        { "0.1.0-alpha.1", "0.1.0-alpha.beta", -1 },
        { "0.1.0+build.5", "0.1.0", 0 },
        { "0.1.0-alpha.010", "0.1.0-alpha.9", 1 },
        /* W301 BETA-VERSION: Kette alpha.9 < 0.2.0 < 0.2.1 < 0.3.0 < 1.0.0 */
        { "0.1.0-alpha.9", "0.2.0", -1 },
        { "0.2.0", "0.1.0-alpha.9", 1 },
        { "0.2.0", "0.2.1", -1 },
        { "0.2.1", "0.3.0", -1 },
        { "0.3.0", "1.0.0", -1 },
        { "v0.2.0", "0.2.0", 0 },
        { "0.2.0-rc.1", "0.2.0", -1 },
        { "0.2.10", "0.2.9", 1 },
    };
    static const char *const kBadVer[] = {
        "", "dev", "0.1", "0.1.0.1", "0.1.0-", "0.1.0-alpha..1", "0.1.0 alpha",
        "x0.1.0", "0.1.0-al?ha", "1234567890.0.0",
    };
    for (i = 0; i < (int)(sizeof kCmp / sizeof kCmp[0]) && !why[0]; i++) {
        if (!lupd_ver_cmp(kCmp[i].a, kCmp[i].b, &c) || c != kCmp[i].want) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "%s vs %s", kCmp[i].a, kCmp[i].b);
        }
    }
    for (i = 0; i < (int)(sizeof kBadVer / sizeof kBadVer[0]) && !why[0]; i++) {
        if (lupd_ver_cmp(kBadVer[i], "0.1.0", &c)) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "ungueltig gelesen: '%s'", kBadVer[i]);
        }
    }
    if (!why[0]) {
        printf("PASS W250 update: Versionsvergleich (alpha.10 > alpha.9, gleich, aelter, beta > alpha, Release > Prerelease, ungueltige Formen; W301: 0.1.0-alpha.9 < 0.2.0 < 0.2.1 < 0.3.0 < 1.0.0)\n");
        (*pass)++;
    } else {
        printf("FAIL W250 update Versionsvergleich: %s\n", why);
        (*fail)++;
    }

    /* b) JSON-Auswahl: Entwurf ignorieren, Prerelease zaehlt, nur 1. Ebene */
    why[0] = '\0';
    {
        static const char kJs[] =
            "[\n"
            " {\"url\":\"https://api.github.com/x\",\"html_url\":\"https://github.com/Greevee-Lab/Steelbound/releases/tag/v0.1.0-alpha.11\","
            "  \"tag_name\":\"v0.1.0-alpha.11\",\"draft\":true,\"prerelease\":true,"
            "  \"author\":{\"login\":\"x\",\"html_url\":\"https://github.com/x\"},\"assets\":[]},\n"
            " {\"author\":{\"html_url\":\"https://github.com/someone\",\"tag_name\":\"v9.9.9\"},"
            "  \"assets\":[{\"browser_download_url\":\"https://example.invalid/x.zip\",\"tag_name\":\"9.0.0\"}],"
            "  \"tag_name\":\"v0.1.0-alpha.10\",\"html_url\":\"https://github.com/Greevee-Lab/Steelbound/releases/tag/v0.1.0-alpha.10\","
            "  \"draft\":false,\"prerelease\":true,\"body\":\"Fixes \\\"x\\\" \\u00e4 [1] {2}\",\"id\":123,\"x\":null},\n"
            " {\"tag_name\":\"v0.1.0-alpha.9\",\"html_url\":\"https://github.com/Greevee-Lab/Steelbound/releases/tag/v0.1.0-alpha.9\","
            "  \"draft\":false,\"prerelease\":false},\n"
            " {\"tag_name\":\"nightly\",\"draft\":false}\n"
            "]";
        static const char kOnlyDraft[] =
            "[{\"tag_name\":\"v0.2.0\",\"draft\":true,\"html_url\":\"https://github.com/Greevee-Lab/Steelbound/releases/tag/v0.2.0\"}]";
        static const char *const kBadJs[] = { "", "{}", "[{\"tag_name\":\"v1.0.0\"", "[1,2]",
                                              "[{\"tag_name\":v1}]", "<html>rate limit</html>" };
        lupd_rel_t r;
        lupd_res_t res;
        int n = lupd_pick(kJs, sizeof kJs - 1, &r);
        if (n != 2 || strcmp(r.tag, "v0.1.0-alpha.10") != 0 || !r.prerelease ||
            strcmp(r.url, "https://github.com/Greevee-Lab/Steelbound/releases/tag/v0.1.0-alpha.10") != 0) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "Auswahl n=%d tag=%s", n, r.tag);
        }
        if (!why[0] && lupd_pick(kOnlyDraft, sizeof kOnlyDraft - 1, &r) != 0) {
            strcpy_s(why, sizeof why, "nur Entwurf -> gezaehlt");
        }
        if (!why[0] && lupd_pick("[]", 2, &r) != 0) {
            strcpy_s(why, sizeof why, "leere Liste");
        }
        for (i = 0; i < (int)(sizeof kBadJs / sizeof kBadJs[0]) && !why[0]; i++) {
            if (lupd_pick(kBadJs[i], strlen(kBadJs[i]), &r) != -1) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "kaputtes JSON akzeptiert: %s", kBadJs[i]);
            }
        }
        /* Bewertung gegen die eigene Version */
        if (!why[0]) {
            memset(&res, 0, sizeof res);
            lupd_eval("0.1.0-alpha.9", kJs, sizeof kJs - 1, &res);
            if (res.result != LUPD_R_NEWER) strcpy_s(why, sizeof why, "alpha.9 -> nicht NEWER");
        }
        if (!why[0]) {
            memset(&res, 0, sizeof res);
            lupd_eval("0.1.0-alpha.10", kJs, sizeof kJs - 1, &res);
            if (res.result != LUPD_R_CURRENT) strcpy_s(why, sizeof why, "alpha.10 -> nicht CURRENT");
        }
        if (!why[0]) {
            memset(&res, 0, sizeof res);
            lupd_eval("0.1.0-alpha.12", kJs, sizeof kJs - 1, &res);
            if (res.result != LUPD_R_CURRENT) strcpy_s(why, sizeof why, "neuer als Releases -> nicht CURRENT");
        }
        if (!why[0]) {
            memset(&res, 0, sizeof res);
            lupd_eval("0.1.0-alpha.9", kOnlyDraft, sizeof kOnlyDraft - 1, &res);
            if (res.result != LUPD_R_BAD) strcpy_s(why, sizeof why, "nur Entwurf -> nicht BAD");
        }
        if (!why[0]) {
            lupd_fetch("", &res); /* ohne Release-Version: kein Netz */
            if (res.result != LUPD_R_NOVER) strcpy_s(why, sizeof why, "ohne Version -> nicht NOVER");
        }
    }
    if (!why[0]) {
        printf("PASS W250 update: JSON-Auswahl (Entwurf ignoriert, Prerelease zaehlt, verschachtelte Schluessel ignoriert, kaputtes JSON abgelehnt, NEWER/CURRENT, ohne Version kein Abruf)\n");
        (*pass)++;
    } else {
        printf("FAIL W250 update JSON-Auswahl: %s\n", why);
        (*fail)++;
    }

    /* b2) W301 BETA-VERSION: normale Releases (prerelease=false) und alte
     * Alpha-Prereleases gemischt - die hoechste Version gewinnt, egal in
     * welcher Reihenfolge GitHub sie liefert und ob sie Prerelease ist. */
    why[0] = '\0';
    {
        static const char kMix[] =
            "[{\"tag_name\":\"v0.2.0\",\"html_url\":\"https://github.com/Greevee-Lab/Steelbound/releases/tag/v0.2.0\","
            "\"draft\":false,\"prerelease\":false},"
            "{\"tag_name\":\"v0.3.0\",\"html_url\":\"https://github.com/Greevee-Lab/Steelbound/releases/tag/v0.3.0\","
            "\"draft\":true,\"prerelease\":false},"
            "{\"tag_name\":\"v0.2.1\",\"html_url\":\"https://github.com/Greevee-Lab/Steelbound/releases/tag/v0.2.1\","
            "\"draft\":false,\"prerelease\":false},"
            "{\"tag_name\":\"v0.1.0-alpha.10\",\"html_url\":\"https://github.com/Greevee-Lab/Steelbound/releases/tag/v0.1.0-alpha.10\","
            "\"draft\":false,\"prerelease\":true},"
            "{\"tag_name\":\"v0.1.0-alpha.9\",\"html_url\":\"https://github.com/Greevee-Lab/Steelbound/releases/tag/v0.1.0-alpha.9\","
            "\"draft\":false,\"prerelease\":true}]";
        static const char kPreNewer[] =
            "[{\"tag_name\":\"v0.2.0\",\"html_url\":\"https://github.com/Greevee-Lab/Steelbound/releases/tag/v0.2.0\","
            "\"draft\":false,\"prerelease\":false},"
            "{\"tag_name\":\"v0.3.0-rc.1\",\"html_url\":\"https://github.com/Greevee-Lab/Steelbound/releases/tag/v0.3.0-rc.1\","
            "\"draft\":false,\"prerelease\":true}]";
        static const struct { const char *v, *stage, *label; } kStage[] = {
            { "0.2.0", "BETA", "Steelbound 0.2.0 \xC2\xB7 BETA" },
            { "0.3.4", "BETA", "Steelbound 0.3.4 \xC2\xB7 BETA" },
            { "0.1.0-alpha.9", "ALPHA", "Steelbound 0.1.0-alpha.9 \xC2\xB7 ALPHA" },
            { "0.2.0-rc.1", "RC", "Steelbound 0.2.0-rc.1 \xC2\xB7 RC" },
            { "1.0.0", "", "Steelbound 1.0.0" },
            { "1.2.0-beta.1", "BETA", "Steelbound 1.2.0-beta.1 \xC2\xB7 BETA" },
            { "0.2.0+build-7", "BETA", "Steelbound 0.2.0+build-7 \xC2\xB7 BETA" },
            { "", "", "Steelbound dev" },
        };
        lupd_rel_t r;
        lupd_res_t res;
        char lb[96];
        int n = lupd_pick(kMix, sizeof kMix - 1, &r);
        if (n != 4 || strcmp(r.tag, "v0.2.1") != 0 || r.prerelease ||
            strcmp(r.url, "https://github.com/Greevee-Lab/Steelbound/releases/tag/v0.2.1") != 0) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "gemischt: n=%d tag=%s", n, r.tag);
        }
        if (!why[0]) {
            memset(&res, 0, sizeof res);
            lupd_eval("0.1.0-alpha.9", kMix, sizeof kMix - 1, &res);
            if (res.result != LUPD_R_NEWER || strcmp(res.rel.tag, "v0.2.1") != 0)
                strcpy_s(why, sizeof why, "alpha.9 -> nicht NEWER 0.2.1");
        }
        if (!why[0]) {
            memset(&res, 0, sizeof res);
            lupd_eval("0.2.0", kMix, sizeof kMix - 1, &res);
            if (res.result != LUPD_R_NEWER) strcpy_s(why, sizeof why, "0.2.0 -> nicht NEWER");
        }
        if (!why[0]) {
            memset(&res, 0, sizeof res);
            lupd_eval("0.2.1", kMix, sizeof kMix - 1, &res);
            if (res.result != LUPD_R_CURRENT) strcpy_s(why, sizeof why, "0.2.1 -> nicht CURRENT (Entwurf 0.3.0 gezaehlt?)");
        }
        if (!why[0]) {
            memset(&res, 0, sizeof res);
            lupd_eval("0.2.0", kPreNewer, sizeof kPreNewer - 1, &res);
            if (res.result != LUPD_R_NEWER || strcmp(res.rel.tag, "v0.3.0-rc.1") != 0 || !res.rel.prerelease)
                strcpy_s(why, sizeof why, "0.2.0 vs 0.3.0-rc.1 -> nicht NEWER");
        }
        if (!why[0]) {
            memset(&res, 0, sizeof res);
            lupd_eval("1.0.0", kPreNewer, sizeof kPreNewer - 1, &res);
            if (res.result != LUPD_R_CURRENT) strcpy_s(why, sizeof why, "1.0.0 -> nicht CURRENT");
        }
        for (i = 0; i < (int)(sizeof kStage / sizeof kStage[0]) && !why[0]; i++) {
            sb_version_label(lb, sizeof lb, kStage[i].v);
            if (strcmp(sb_release_stage(kStage[i].v), kStage[i].stage) != 0 || strcmp(lb, kStage[i].label) != 0) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "Phase/Text '%s' -> '%s' / '%s'", kStage[i].v,
                            sb_release_stage(kStage[i].v), lb);
            }
        }
    }
    if (!why[0]) {
        printf("PASS W301 update: Beta-Versionen (normale Releases + alte Alpha-Prereleases gemischt -> hoechste, Entwurf nie, rc als Prerelease; Phase/Wasserzeichen-Text 0.2.0 = BETA, alpha = ALPHA, 1.0.0 ohne, dev)\n");
        (*pass)++;
    } else {
        printf("FAIL W301 update Beta-Versionen: %s\n", why);
        (*fail)++;
    }

    /* c) URL-Pruefung: fremde URL -> nichts oeffnen */
    why[0] = '\0';
    {
        static const char *const kOk[] = {
            "https://github.com/Greevee-Lab/Steelbound/releases/tag/v0.1.0-alpha.8",
            "https://github.com/Greevee-Lab/Steelbound/releases/tag/0.1.0-alpha.10",
        };
        static const char *const kNo[] = {
            "https://github.com/someone/Steelbound/releases/tag/v1",
            "https://github.com/Greevee-Lab/Steelbound/releases/",
            "http://github.com/Greevee-Lab/Steelbound/releases/tag/v1",
            "https://github.com.evil.example/Greevee-Lab/Steelbound/releases/tag/v1",
            "https://github.com/Greevee-Lab/Steelbound/releases/../../evil",
            "https://github.com/Greevee-Lab/Steelbound/releases/tag/v1?x=1",
            "https://github.com/Greevee-Lab/Steelbound/releases/tag/v1 \"&calc",
            "https://github.com/Greevee-Lab/Steelbound/releases/tag/v1@evil.example",
            "https://github.com/Greevee-Lab/Steelbound/releases/tag/v1#x",
            "https://github.com/greevee-lab/steelbound/releases/tag/v1",
            "file:///C:/Windows/system32/calc.exe",
            "",
        };
        for (i = 0; i < (int)(sizeof kOk / sizeof kOk[0]) && !why[0]; i++) {
            if (!lupd_url_ok(kOk[i])) _snprintf_s(why, sizeof why, _TRUNCATE, "abgelehnt: %s", kOk[i]);
        }
        for (i = 0; i < (int)(sizeof kNo / sizeof kNo[0]) && !why[0]; i++) {
            if (lupd_url_ok(kNo[i])) _snprintf_s(why, sizeof why, _TRUNCATE, "akzeptiert: %s", kNo[i]);
        }
        if (!why[0] && lupd_url_ok(NULL)) strcpy_s(why, sizeof why, "NULL akzeptiert");
    }
    if (!why[0]) {
        printf("PASS W250 update: URL-Pruefung (nur %s..., fremde/verbogene URLs -> nichts oeffnen)\n", LUPD_PAGE_PREFIX);
        (*pass)++;
    } else {
        printf("FAIL W250 update URL-Pruefung: %s\n", why);
        (*fail)++;
    }

    /* d) Erstabfrage-Zustand (unbeantwortet / ja / nein) in user.cfg */
    why[0] = '\0';
    {
        char p[1024], buf[2048];
        FILE *f = NULL;
        size_t n = 0;
        u8_GetTempPath(p, sizeof p);
        strcat_s(p, sizeof p, "w250_update_cfg_test.cfg");
        u8_DeleteFile(p);
        if (lupd_cfg_get(p) != -1) strcpy_s(why, sizeof why, "fehlende Datei != -1");
        if (!why[0] && (!lupd_should_ask("0.1.0-alpha.7", -1) || lupd_should_ask("0.1.0-alpha.7", 0) ||
                        lupd_should_ask("0.1.0-alpha.7", 1) || lupd_should_ask("", -1)))
            strcpy_s(why, sizeof why, "should_ask");
        if (!why[0] && (lupd_auto_check("0.1.0-alpha.7", -1) || lupd_auto_check("0.1.0-alpha.7", 0) ||
                        !lupd_auto_check("0.1.0-alpha.7", 1) || lupd_auto_check("", 1)))
            strcpy_s(why, sizeof why, "auto_check");
        if (!why[0] && u8_fopen_s(&f, p, "w") == 0 && f) {
            fputs("[display]\nwidth=1280\n# Kommentar\n", f);
            fclose(f);
        }
        if (!why[0] && lupd_cfg_get(p) != -1) strcpy_s(why, sizeof why, "ohne Schluessel != -1");
        if (!why[0] && (lupd_cfg_set(p, 1) != 0 || lupd_cfg_get(p) != 1)) strcpy_s(why, sizeof why, "ja");
        if (!why[0] && (lupd_cfg_set(p, 0) != 0 || lupd_cfg_get(p) != 0)) strcpy_s(why, sizeof why, "nein");
        if (!why[0] && u8_fopen_s(&f, p, "r") == 0 && f) {
            n = fread(buf, 1, sizeof buf - 1, f);
            fclose(f);
        }
        buf[n] = '\0';
        if (!why[0] && (!strstr(buf, "width=1280\n") || !strstr(buf, "# Kommentar\n") ||
                        strstr(buf, "update_check=1") || !strstr(buf, "update_check=0\n") ||
                        strstr(strstr(buf, "update_check=") + 1, "update_check=")))
            _snprintf_s(why, sizeof why, _TRUNCATE, "Inhalt: %s", buf);
        u8_DeleteFile(p);
        if (!why[0] && (lupd_cfg_set(p, 0) != 0 || lupd_cfg_get(p) != 0))
            strcpy_s(why, sizeof why, "neue Datei");
        u8_DeleteFile(p);
    }
    if (!why[0]) {
        printf("PASS W250 update: Erstabfrage-Zustand (unbeantwortet = fragen, ja = Pruefung beim Start, nein = still; user.cfg fremde Zeilen bleiben)\n");
        (*pass)++;
    } else {
        printf("FAIL W250 update Erstabfrage-Zustand: %s\n", why);
        (*fail)++;
    }
}
