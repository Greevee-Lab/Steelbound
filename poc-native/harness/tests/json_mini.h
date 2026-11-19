/* json_mini.h - W334: kleiner JSON-Leser nur fuer Testvektoren (loc_net_selftest --vectors).
 * Ganzer Text im Speicher; Werte werden als Zeiger in den Text zurueckgegeben. Keine Netzdaten. */
#pragma once
#include <stdlib.h>
#include <string.h>

static const char *jm_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

/* Ende des Wertes ab p (oder NULL). */
static const char *jm_skip(const char *p)
{
    p = jm_ws(p);
    if (*p == '"') {
        for (p++; *p; p++) {
            if (*p == '\\' && p[1]) { p++; continue; }
            if (*p == '"') return p + 1;
        }
        return NULL;
    }
    if (*p == '{' || *p == '[') {
        char close = *p == '{' ? '}' : ']';
        p = jm_ws(p + 1);
        if (*p == close) return p + 1;
        for (;;) {
            if (close == '}') {
                p = jm_skip(p);
                if (!p) return NULL;
                p = jm_ws(p);
                if (*p != ':') return NULL;
                p++;
            }
            p = jm_skip(p);
            if (!p) return NULL;
            p = jm_ws(p);
            if (*p == ',') { p = jm_ws(p + 1); continue; }
            if (*p == close) return p + 1;
            return NULL;
        }
    }
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\r' && *p != '\n') p++;
    return p;
}

/* Wert zu key im Objekt obj. */
static const char *jm_get(const char *obj, const char *key)
{
    const char *p = jm_ws(obj);
    size_t kl = strlen(key);
    if (!p || *p != '{') return NULL;
    p = jm_ws(p + 1);
    while (*p == '"') {
        const char *ks = p + 1, *ke = jm_skip(p), *v;
        if (!ke) return NULL;
        p = jm_ws(ke);
        if (*p != ':') return NULL;
        v = jm_ws(p + 1);
        if ((size_t)(ke - 1 - ks) == kl && memcmp(ks, key, kl) == 0) return v;
        p = jm_skip(v);
        if (!p) return NULL;
        p = jm_ws(p);
        if (*p == ',') p = jm_ws(p + 1);
    }
    return NULL;
}

/* i-tes Element eines Arrays (oder NULL). */
static const char *jm_idx(const char *arr, int i)
{
    const char *p = jm_ws(arr);
    if (!p || *p != '[') return NULL;
    p = jm_ws(p + 1);
    if (*p == ']') return NULL;
    while (i-- > 0) {
        p = jm_skip(p);
        if (!p) return NULL;
        p = jm_ws(p);
        if (*p != ',') return NULL;
        p = jm_ws(p + 1);
    }
    return p;
}

static int jm_len(const char *arr)
{
    int n = 0;
    while (jm_idx(arr, n)) n++;
    return n;
}

/* Zeichenkette (ohne Escapes) in out kopieren. */
static int jm_str(const char *v, char *out, int cap)
{
    int o = 0;
    v = v ? jm_ws(v) : NULL;
    if (!v || *v != '"') return 0;
    for (v++; *v && *v != '"' && o < cap - 1; v++) out[o++] = *v;
    out[o] = 0;
    return 1;
}

static double jm_num(const char *v)
{
    return v ? atof(jm_ws(v)) : 0.0;
}

static int jm_true(const char *v)
{
    return v && strncmp(jm_ws(v), "true", 4) == 0;
}

/* Hex-Zeichenkette -> Bytes. Rueckgabe Anzahl Bytes oder -1. */
static int jm_hex(const char *v, unsigned char *out, int cap)
{
    static char tmp[16384];
    int i, n;
    if (!jm_str(v, tmp, sizeof tmp)) return -1;
    n = (int)strlen(tmp);
    if (n % 2 || n / 2 > cap) return -1;
    for (i = 0; i < n / 2; i++) {
        unsigned x;
        if (sscanf_s(tmp + 2 * i, "%2x", &x) != 1) return -1;
        out[i] = (unsigned char)x;
    }
    return n / 2;
}

static char *jm_load(const char *path)
{
    FILE *f;
    long n;
    char *b;
    if (fopen_s(&f, path, "rb") != 0 || !f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (char *)malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        b = NULL;
    }
    if (b) b[n] = 0;
    fclose(f);
    return b;
}
