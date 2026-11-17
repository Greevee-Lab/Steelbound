/* launcher_account.c - W328 STEELNET-ACCOUNT (s. launcher_account.h).
 * Vertrag: net/steelnet/PROTOKOLL.md, Abschnitt "W328: Pilotenkonten".
 * Quelltext ASCII. Geheimnisse (K, Passwort, OAuth-Code) werden nie
 * ausgegeben oder geloggt; Puffer werden nach Gebrauch geloescht. */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "launcher_account.h"
#include "launcher_account_cfg.h"
#include "launcher_app.h"
#include "launcher_games.h"
#include "launcher_u8.h"
#include "sb_sha256.h"
#include "sb_userdata.h"

#define printf cli_printf

#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 0x00002000
#endif

static void wipe(void *p, size_t n)
{
    SecureZeroMemory(p, n);
}

/* ====================================================================== Krypto */
void lacc_hmac(const uint8_t *key, size_t klen, const void *msg, size_t n, uint8_t out[32])
{
    uint8_t k0[64], pad[64], inner[32];
    sb_sha256_t c;
    int i;
    memset(k0, 0, sizeof k0);
    if (klen > 64) {
        sb_sha256(key, klen, k0);
    } else if (klen) {
        memcpy(k0, key, klen);
    }
    for (i = 0; i < 64; i++) pad[i] = (uint8_t)(k0[i] ^ 0x36);
    sb_sha256_init(&c);
    sb_sha256_update(&c, pad, 64);
    sb_sha256_update(&c, msg, n);
    sb_sha256_final(&c, inner);
    for (i = 0; i < 64; i++) pad[i] = (uint8_t)(k0[i] ^ 0x5c);
    sb_sha256_init(&c);
    sb_sha256_update(&c, pad, 64);
    sb_sha256_update(&c, inner, 32);
    sb_sha256_final(&c, out);
    wipe(k0, sizeof k0);
    wipe(pad, sizeof pad);
    wipe(inner, sizeof inner);
    wipe(&c, sizeof c);
}

void lacc_hex(const uint8_t *b, size_t n, char *out)
{
    static const char h[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < n; i++) {
        out[2 * i] = h[b[i] >> 4];
        out[2 * i + 1] = h[b[i] & 15];
    }
    out[2 * n] = '\0';
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int lacc_unhex(const char *s, uint8_t *out, size_t n)
{
    size_t i;
    if (!s || strlen(s) != 2 * n) return 0;
    for (i = 0; i < n; i++) {
        int a = hexval(s[2 * i]), b = hexval(s[2 * i + 1]);
        if (a < 0 || b < 0) return 0;
        out[i] = (uint8_t)(a * 16 + b);
    }
    return 1;
}

void lacc_game_key(const uint8_t K[32], uint32_t account, long long exp, uint8_t G[32])
{
    char m[96];
    int n = _snprintf_s(m, sizeof m, _TRUNCATE, "steelnet-game-v1|%lu|%lld", (unsigned long)account, exp);
    lacc_hmac(K, 32, m, (size_t)n, G);
}

void lacc_api_proof(const uint8_t K[32], const char *action, const char *nonce, uint32_t account,
                    char hex[65])
{
    char m[256];
    uint8_t d[32];
    int n = _snprintf_s(m, sizeof m, _TRUNCATE, "steelnet-api-v1|%s|%s|%lu", action, nonce,
                        (unsigned long)account);
    lacc_hmac(K, 32, m, (size_t)n, d);
    lacc_hex(d, 32, hex);
    wipe(d, sizeof d);
}

void lacc_auth_proof(const uint8_t G[32], const char *nonce, uint32_t client_id, char hex[65])
{
    char m[160];
    uint8_t d[32];
    int n = _snprintf_s(m, sizeof m, _TRUNCATE, "steelnet-auth-v1|%s|%lu", nonce, (unsigned long)client_id);
    lacc_hmac(G, 32, m, (size_t)n, d);
    lacc_hex(d, 32, hex);
}

static const char kB32[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

void lacc_b32_enc(const uint8_t *in, size_t n, char *out)
{
    size_t i, o = 0;
    unsigned buf = 0;
    int bits = 0;
    for (i = 0; i < n; i++) {
        buf = (buf << 8) | in[i];
        bits += 8;
        while (bits >= 5) {
            out[o++] = kB32[(buf >> (bits - 5)) & 31];
            bits -= 5;
        }
    }
    if (bits > 0) out[o++] = kB32[(buf << (5 - bits)) & 31];
    out[o] = '\0';
}

int lacc_b32_dec(const char *in, uint8_t *out, size_t cap)
{
    unsigned buf = 0;
    int bits = 0;
    size_t o = 0;
    for (; *in; in++) {
        char c = *in;
        int v;
        if (c == '-' || c == ' ' || c == '=') continue;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= '2' && c <= '7') v = c - '2' + 26;
        else return -1;
        buf = (buf << 5) | (unsigned)v;
        bits += 5;
        if (bits >= 8) {
            if (o >= cap) return -1;
            out[o++] = (uint8_t)(buf >> (bits - 8));
            bits -= 8;
        }
    }
    return (int)o;
}

void lacc_recovery_code(uint32_t account, const uint8_t K[32], char *out, size_t cap)
{
    uint8_t raw[20];
    char b[40];
    size_t i, o = 0;
    raw[0] = (uint8_t)(account >> 24);
    raw[1] = (uint8_t)(account >> 16);
    raw[2] = (uint8_t)(account >> 8);
    raw[3] = (uint8_t)account;
    memcpy(raw + 4, K, 16);
    lacc_b32_enc(raw, 20, b); /* 32 Zeichen */
    for (i = 0; b[i] && o + 2 < cap; i++) {
        if (i && i % 4 == 0) out[o++] = '-';
        out[o++] = b[i];
    }
    out[o] = '\0';
    wipe(raw, sizeof raw);
    wipe(b, sizeof b);
}

int lacc_recovery_parse(const char *code, uint32_t *account, uint8_t k16[16])
{
    uint8_t raw[24];
    int n = lacc_b32_dec(code, raw, sizeof raw);
    if (n != 20) return 0;
    *account = ((uint32_t)raw[0] << 24) | ((uint32_t)raw[1] << 16) | ((uint32_t)raw[2] << 8) | raw[3];
    memcpy(k16, raw + 4, 16);
    wipe(raw, sizeof raw);
    return 1;
}

void lacc_game_auth(const lacc_store_t *st, long long now, char *out, size_t cap)
{
    uint8_t G[32];
    char gh[65];
    long long exp = now + LACC_GAME_TTL;
    lacc_game_key(st->key, st->account, exp, G);
    lacc_hex(G, 32, gh);
    _snprintf_s(out, cap, _TRUNCATE, "%lu:%lld:%s", (unsigned long)st->account, exp, gh);
    wipe(G, sizeof G);
    wipe(gh, sizeof gh);
}

int lacc_random(void *buf, size_t n)
{
    return BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

static void b64url(const uint8_t *in, size_t n, char *out)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t i, o = 0;
    for (i = 0; i + 2 < n; i += 3) {
        unsigned v = ((unsigned)in[i] << 16) | ((unsigned)in[i + 1] << 8) | in[i + 2];
        out[o++] = t[(v >> 18) & 63];
        out[o++] = t[(v >> 12) & 63];
        out[o++] = t[(v >> 6) & 63];
        out[o++] = t[v & 63];
    }
    if (n - i == 1) {
        unsigned v = (unsigned)in[i] << 16;
        out[o++] = t[(v >> 18) & 63];
        out[o++] = t[(v >> 12) & 63];
    } else if (n - i == 2) {
        unsigned v = ((unsigned)in[i] << 16) | ((unsigned)in[i + 1] << 8);
        out[o++] = t[(v >> 18) & 63];
        out[o++] = t[(v >> 12) & 63];
        out[o++] = t[(v >> 6) & 63];
    }
    out[o] = '\0';
}

/* PKCE S256: challenge = b64url(SHA-256(verifier)) */
static void pkce_challenge(const char *verifier, char out[48])
{
    uint8_t d[32];
    sb_sha256(verifier, strlen(verifier), d);
    b64url(d, 32, out);
}

/* ====================================================================== Eingaben */
static int name_chars(const char *s, size_t lo, size_t hi)
{
    size_t n;
    if (!s) return 0;
    n = strlen(s);
    if (n < lo || n > hi) return 0;
    for (; *s; s++) {
        char c = *s;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' ||
              c == '_' || c == '-')) {
            return 0;
        }
    }
    return 1;
}

int lacc_valid_name(const char *s)
{
    if (!name_chars(s, 1, 16)) return 0;
    if (s[0] == ' ' || s[strlen(s) - 1] == ' ') return 0; /* keine Rand-Leerzeichen (wie der Dienst) */
    if (_strnicmp(s, "guest-", 6) == 0 || _strnicmp(s, "gast-", 5) == 0) return 0;
    return 1;
}

int lacc_valid_login(const char *s)
{
    return name_chars(s, 3, 32);
}

int lacc_valid_password(const char *s)
{
    size_t n = s ? strlen(s) : 0;
    return n >= 10 && n <= 256;
}

/* ====================================================================== Konfiguration */
int lacc_is_loopback(const char *host)
{
    return host && (strcmp(host, "127.0.0.1") == 0 || _stricmp(host, "localhost") == 0 ||
                    strcmp(host, "::1") == 0);
}

static int norm_pin(const char *in, char out[65])
{
    int o = 0;
    out[0] = '\0';
    if (!in) return 0;
    for (; *in && o < 64; in++) {
        if (*in == ':' || *in == ' ') continue;
        if (hexval(*in) < 0) return 0;
        out[o++] = (char)(*in >= 'A' && *in <= 'F' ? *in + 32 : *in);
    }
    out[o] = '\0';
    if (*in || (o != 0 && o != 64)) {
        out[0] = '\0';
        return 0;
    }
    return o == 64;
}

int lacc_server(lacc_srv_t *s)
{
    char api[200], pin[200];
    memset(s, 0, sizeof *s);
    strcpy_s(s->host, sizeof s->host, SB_STEELNET_HOST);
    s->port = SB_STEELNET_API_PORT;
    s->lobby_port = SB_STEELNET_PORT;
    norm_pin(SB_STEELNET_PIN1, s->pin1);
    norm_pin(SB_STEELNET_PIN2, s->pin2);
    if (u8_GetEnv("SB_STEELNET_API", api, sizeof api) > 0) {
        char *c = strrchr(api, ':');
        if (c) {
            int port = atoi(c + 1);
            *c = '\0';
            if (lacc_is_loopback(api) && port > 0 && port < 65536) {
                strcpy_s(s->host, sizeof s->host, api);
                s->port = port;
                s->test = 1;
                s->pin1[0] = s->pin2[0] = '\0';
                if (u8_GetEnv("SB_STEELNET_PIN", pin, sizeof pin) > 0) {
                    norm_pin(pin, s->pin1);
                }
                if (u8_GetEnv("SB_STEELNET_LOBBY_PORT", pin, sizeof pin) > 0 && atoi(pin) > 0) {
                    s->lobby_port = atoi(pin);
                }
            }
        }
    }
    return s->pin1[0] != '\0' || s->pin2[0] != '\0';
}

int lacc_pin_match(const lacc_srv_t *s, const uint8_t sha[32])
{
    char h[65];
    lacc_hex(sha, 32, h);
    if (s->pin1[0] && strlen(s->pin1) == 64 && strcmp(h, s->pin1) == 0) return 1;
    if (s->pin2[0] && strlen(s->pin2) == 64 && strcmp(h, s->pin2) == 0) return 1;
    return 0;
}

/* ====================================================================== Speicher (DPAPI) */
static const char kEntropy[] = "steelbound-steelnet-account-v1";

void lacc_store_path(const char *dir, char *out, size_t cap)
{
    size_t n = strlen(dir);
    _snprintf_s(out, cap, _TRUNCATE, "%s%s%s", dir, (n && (dir[n - 1] == '\\' || dir[n - 1] == '/')) ? "" : "\\",
                LACC_FILE);
}

void lacc_default_dir(const char *client_dir, char *out, size_t cap)
{
    char acp[1024], base[1100];
    if (u8_GetEnv("SB_ACCOUNT_DIR", out, cap) > 0) {
        return;
    }
    if (sb_userdata_path_peek(SB_UD_ROOT, acp, sizeof acp) == 1) {
        u8_from_acp(acp, base, sizeof base);
    } else {
        strcpy_s(base, sizeof base, client_dir ? client_dir : ".");
    }
    lgame_loc_dir(base, out, cap);
}

static void mkdirs(const char *dir)
{
    char p[1200];
    size_t i;
    strcpy_s(p, sizeof p, dir);
    for (i = 3; p[i]; i++) {
        if (p[i] == '\\' || p[i] == '/') {
            char c = p[i];
            p[i] = '\0';
            u8_CreateDirectory(p);
            p[i] = c;
        }
    }
    u8_CreateDirectory(p);
}

int lacc_store_save(const char *dir, const lacc_store_t *st)
{
    char plain[512], kh[65], path[1200], tmp[1220];
    DATA_BLOB in, ent, out;
    FILE *f = NULL;
    int n, rc = -1;
    wchar_t wp[1200], wt[1220];
    lacc_hex(st->key, 32, kh);
    n = _snprintf_s(plain, sizeof plain, _TRUNCATE, "v=1\naccount=%lu\ngen=%d\nname=%s\nhost=%s\nkey=%s\n",
                    (unsigned long)st->account, st->gen, st->name, st->host, kh);
    wipe(kh, sizeof kh);
    if (n <= 0) return -1;
    in.pbData = (BYTE *)plain;
    in.cbData = (DWORD)n;
    ent.pbData = (BYTE *)kEntropy;
    ent.cbData = (DWORD)(sizeof kEntropy - 1);
    memset(&out, 0, sizeof out);
    if (!CryptProtectData(&in, L"Steelbound pilot", &ent, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        wipe(plain, sizeof plain);
        return -1;
    }
    wipe(plain, sizeof plain);
    mkdirs(dir);
    lacc_store_path(dir, path, sizeof path);
    _snprintf_s(tmp, sizeof tmp, _TRUNCATE, "%s.tmp", path);
    if (u8_fopen_s(&f, tmp, "wb") == 0 && f) {
        if (fwrite("SBA1", 1, 4, f) == 4 && fwrite(out.pbData, 1, out.cbData, f) == out.cbData) rc = 0;
        if (fclose(f) != 0) rc = -1;
        u8_to_w(path, wp, 1200);
        u8_to_w(tmp, wt, 1220);
        if (rc == 0 && !MoveFileExW(wt, wp, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) rc = -1;
        if (rc != 0) u8_DeleteFile(tmp);
    }
    LocalFree(out.pbData);
    return rc;
}

static int kv_get(const char *txt, const char *key, char *out, size_t cap)
{
    size_t kl = strlen(key);
    const char *p = txt;
    while (p && *p) {
        if (strncmp(p, key, kl) == 0 && p[kl] == '=') {
            const char *e = strchr(p, '\n');
            size_t n = e ? (size_t)(e - p - kl - 1) : strlen(p + kl + 1);
            if (n >= cap) return 0;
            memcpy(out, p + kl + 1, n);
            out[n] = '\0';
            return 1;
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
    return 0;
}

int lacc_store_load(const char *dir, lacc_store_t *st)
{
    char path[1200], txt[600], v[200];
    uint8_t buf[4096];
    size_t n;
    FILE *f = NULL;
    DATA_BLOB in, ent, out;
    int ok = 0;
    memset(st, 0, sizeof *st);
    lacc_store_path(dir, path, sizeof path);
    if (u8_GetFileAttributes(path) == INVALID_FILE_ATTRIBUTES) return 0;
    if (u8_fopen_s(&f, path, "rb") != 0 || !f) return -1;
    n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (n < 8 || memcmp(buf, "SBA1", 4) != 0) return -1;
    in.pbData = buf + 4;
    in.cbData = (DWORD)(n - 4);
    ent.pbData = (BYTE *)kEntropy;
    ent.cbData = (DWORD)(sizeof kEntropy - 1);
    memset(&out, 0, sizeof out);
    if (!CryptUnprotectData(&in, NULL, &ent, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &out)) return -1;
    if (out.cbData < sizeof txt) {
        memcpy(txt, out.pbData, out.cbData);
        txt[out.cbData] = '\0';
        ok = 1;
    }
    wipe(out.pbData, out.cbData);
    LocalFree(out.pbData);
    if (!ok) return -1;
    ok = kv_get(txt, "v", v, sizeof v) && strcmp(v, "1") == 0;
    if (ok && kv_get(txt, "account", v, sizeof v)) st->account = (uint32_t)strtoul(v, NULL, 10);
    else ok = 0;
    if (ok && kv_get(txt, "gen", v, sizeof v)) st->gen = atoi(v);
    else ok = 0;
    if (ok && !(kv_get(txt, "name", st->name, sizeof st->name) && name_chars(st->name, 1, 16))) ok = 0;
    if (ok && !kv_get(txt, "host", st->host, sizeof st->host)) ok = 0;
    if (ok && !(kv_get(txt, "key", v, sizeof v) && lacc_unhex(v, st->key, 32))) ok = 0;
    wipe(txt, sizeof txt);
    wipe(v, sizeof v);
    if (!ok || st->account == 0) {
        memset(st, 0, sizeof *st);
        return -1;
    }
    st->have = 1;
    return 1;
}

int lacc_store_forget(const char *dir)
{
    char path[1200];
    lacc_store_path(dir, path, sizeof path);
    if (u8_GetFileAttributes(path) == INVALID_FILE_ATTRIBUTES) return 0;
    return u8_DeleteFile(path) ? 0 : -1;
}

/* ====================================================================== JSON */
static const char *js_find(const char *json, const char *key)
{
    char pat[64];
    const char *p;
    _snprintf_s(pat, sizeof pat, _TRUNCATE, "\"%s\"", key);
    p = json;
    while ((p = strstr(p, pat)) != NULL) {
        const char *q = p + strlen(pat);
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
        if (*q == ':') {
            q++;
            while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
            return q;
        }
        p = q;
    }
    return NULL;
}

int lacc_js_str(const char *json, const char *key, char *out, size_t cap)
{
    const char *p = js_find(json, key);
    size_t o = 0;
    if (!p || *p != '"' || cap == 0) return 0;
    p++;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\') {
            if (!*p) break; /* W329: Backslash am Ende - nicht hinter die 0 lesen */
            c = *p++;
            if (c == 'n') c = '\n';
            else if (c == 't') c = '\t';
            else if (c == 'u') { /* nur ASCII noetig */
                int v = 0, i;
                for (i = 0; i < 4 && hexval(*p) >= 0; i++) v = v * 16 + hexval(*p++);
                c = (char)(v > 0 && v < 128 ? v : '?');
            }
        }
        if (o + 1 < cap) out[o++] = c;
    }
    out[o] = '\0';
    return *p == '"';
}

int lacc_js_int(const char *json, const char *key, long long *v)
{
    const char *p = js_find(json, key);
    char *e;
    if (!p || !((*p >= '0' && *p <= '9') || *p == '-')) return 0;
    *v = _strtoi64(p, &e, 10);
    return e != p;
}

int lacc_js_bool(const char *json, const char *key, int *v)
{
    const char *p = js_find(json, key);
    if (!p) return 0;
    if (strncmp(p, "true", 4) == 0) { *v = 1; return 1; }
    if (strncmp(p, "false", 5) == 0) { *v = 0; return 1; }
    return 0;
}

void lacc_js_esc(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (; *in && o + 7 < cap; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 0x20) {
            o += (size_t)_snprintf_s(out + o, cap - o, _TRUNCATE, "\\u%04x", c);
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

/* ====================================================================== HTTPS */
int lacc_http(const lacc_srv_t *s, const char *method, const char *path, const char *body,
              lacc_resp_t *r)
{
    HINTERNET ses = NULL, con = NULL, req = NULL;
    wchar_t wh[128], wm[8], wpath[128], ua[64];
    DWORD len = body ? (DWORD)strlen(body) : 0, opt, n;
    PCCERT_CONTEXT cert = NULL;
    uint8_t sha[32];
    int rc = 1;
    memset(r, 0, sizeof *r);
    strcpy_s(r->error, sizeof r->error, "net");
    if (!s->pin1[0] && !s->pin2[0]) {
        strcpy_s(r->error, sizeof r->error, "off");
        return 1;
    }
    u8_to_w(s->host, wh, 128);
    u8_to_w(method, wm, 8);
    u8_to_w(path, wpath, 128);
    _snwprintf_s(ua, 64, _TRUNCATE, L"Steelbound-Launcher");
    ses = WinHttpOpen(ua, lacc_is_loopback(s->host) ? WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) {
        ses = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!ses) return 1;
    opt = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    if (!WinHttpSetOption(ses, WINHTTP_OPTION_SECURE_PROTOCOLS, &opt, sizeof opt)) {
        opt = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(ses, WINHTTP_OPTION_SECURE_PROTOCOLS, &opt, sizeof opt);
    }
    WinHttpSetTimeouts(ses, 5000, 5000, 10000, 15000);
    con = WinHttpConnect(ses, wh, (INTERNET_PORT)s->port, 0);
    if (con) {
        req = WinHttpOpenRequest(con, wm, wpath, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 WINHTTP_FLAG_SECURE);
    }
    if (!req) goto done;
    /* Selbstsigniert: CA/Name/Datum ignorieren - NUR weil danach der feste
     * Fingerabdruck geprueft wird (vor dem Senden des Rumpfs). */
    opt = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
          SECURITY_FLAG_IGNORE_CERT_DATE_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
    WinHttpSetOption(req, WINHTTP_OPTION_SECURITY_FLAGS, &opt, sizeof opt);
    opt = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION | WINHTTP_DISABLE_REDIRECTS;
    WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &opt, sizeof opt);
    opt = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    WinHttpSetOption(req, WINHTTP_OPTION_AUTOLOGON_POLICY, &opt, sizeof opt);
    /* 1) Verbindung + TLS + Kopfzeilen (ohne Rumpf; Kopfzeilen enthalten nichts Geheimes) */
    if (!WinHttpSendRequest(req, L"Content-Type: application/json\r\nConnection: close\r\n", (DWORD)-1L,
                            WINHTTP_NO_REQUEST_DATA, 0, len, 0)) {
        goto done;
    }
    /* 2) Pinning: SHA-256 ueber das DER-Zertifikat, sonst Abbruch vor dem Rumpf */
    n = sizeof cert;
    if (!WinHttpQueryOption(req, WINHTTP_OPTION_SERVER_CERT_CONTEXT, &cert, &n) || !cert) {
        r->pin_fail = 1;
        strcpy_s(r->error, sizeof r->error, "pin");
        goto done;
    }
    sb_sha256(cert->pbCertEncoded, cert->cbCertEncoded, sha);
    CertFreeCertificateContext(cert);
    if (!lacc_pin_match(s, sha)) {
        r->pin_fail = 1;
        strcpy_s(r->error, sizeof r->error, "pin");
        goto done;
    }
    /* 3) Rumpf */
    if (len) {
        DWORD wr = 0;
        if (!WinHttpWriteData(req, body, len, &wr) || wr != len) goto done;
    }
    if (!WinHttpReceiveResponse(req, NULL)) goto done;
    n = sizeof opt;
    opt = 0;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &opt, &n, WINHTTP_NO_HEADER_INDEX);
    r->http = (int)opt;
    {
        size_t got = 0;
        for (;;) {
            DWORD avail = 0, rd = 0;
            if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
            if (got + avail >= sizeof r->body) {
                got = sizeof r->body; /* zu gross */
                break;
            }
            if (!WinHttpReadData(req, r->body + got, avail, &rd) || rd == 0) break;
            got += rd;
        }
        if (got >= sizeof r->body) {
            r->body[0] = '\0';
            strcpy_s(r->error, sizeof r->error, "bad");
            goto done;
        }
        r->body[got] = '\0';
    }
    if (!lacc_js_bool(r->body, "ok", &r->ok)) {
        strcpy_s(r->error, sizeof r->error, "bad");
        r->ok = 0;
    } else if (r->ok) {
        r->error[0] = '\0';
    } else if (!lacc_js_str(r->body, "error", r->error, sizeof r->error) || !r->error[0]) {
        strcpy_s(r->error, sizeof r->error, "bad");
    }
    lacc_js_int(r->body, "retry_after", &r->retry_after);
    rc = 0;
done:
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return rc;
}

/* ====================================================================== Aktionen */
static int fail(char *err, size_t ecap, const char *code)
{
    strcpy_s(err, ecap, code && *code ? code : "bad");
    return 1;
}

/* POST ohne Auth; 0 = ok:true */
static int post(const lacc_srv_t *s, const char *action, const char *body, lacc_resp_t *r, char *err, size_t ecap)
{
    char path[64];
    _snprintf_s(path, sizeof path, _TRUNCATE, "/v1/%s", action);
    if (lacc_http(s, "POST", path, body, r) != 0 || !r->ok) {
        return fail(err, ecap, r->error);
    }
    err[0] = '\0';
    return 0;
}

/* Authentisierte Aktion: Challenge holen, Beweis mit K, extra = weitere JSON-Felder ("" oder ",\"a\":..") */
static int post_auth(const lacc_srv_t *s, const lacc_store_t *st, const char *action, const char *extra,
                     lacc_resp_t *r, char *err, size_t ecap)
{
    char nonce[80], proof[65], body[2048];
    int rc;
    if (!st->have) return fail(err, ecap, "no_account");
    if (post(s, "challenge", "{}", r, err, ecap) != 0) return 1;
    if (!lacc_js_str(r->body, "nonce", nonce, sizeof nonce) || !nonce[0]) return fail(err, ecap, "bad");
    { /* W329: Nonce geht unmaskiert in den JSON-Rumpf -> nur Hex zulassen */
        const char *q;
        for (q = nonce; *q; q++) {
            if (hexval(*q) < 0) return fail(err, ecap, "bad");
        }
    }
    lacc_api_proof(st->key, action, nonce, st->account, proof);
    _snprintf_s(body, sizeof body, _TRUNCATE, "{\"account\":%lu,\"nonce\":\"%s\",\"proof\":\"%s\"%s}",
                (unsigned long)st->account, nonce, proof, extra ? extra : "");
    rc = post(s, action, body, r, err, ecap);
    wipe(body, sizeof body);
    wipe(proof, sizeof proof);
    return rc;
}

/* Antwort mit account/name/key/gen -> st (+ speichern) */
static int take_key(const lacc_srv_t *s, const char *dir, lacc_resp_t *r, lacc_store_t *st, char *err, size_t ecap)
{
    lacc_store_t n;
    long long a = 0, g = 0;
    char kh[80];
    memset(&n, 0, sizeof n);
    if (!lacc_js_int(r->body, "account", &a) || a <= 0 || a > 0xFFFFFFFFLL ||
        !lacc_js_int(r->body, "gen", &g) || !lacc_js_str(r->body, "name", n.name, sizeof n.name) ||
        !name_chars(n.name, 1, 16) || /* W329: Name landet in der Kontodatei (key=value\n) und in --set (;) */
        !lacc_js_str(r->body, "key", kh, sizeof kh) || !lacc_unhex(kh, n.key, 32)) {
        wipe(kh, sizeof kh);
        wipe(r->body, sizeof r->body);
        return fail(err, ecap, "bad");
    }
    wipe(kh, sizeof kh);
    wipe(r->body, sizeof r->body);
    n.have = 1;
    n.account = (uint32_t)a;
    n.gen = (int)g;
    _snprintf_s(n.host, sizeof n.host, _TRUNCATE, "%s:%d", s->host, s->port);
    if (lacc_store_save(dir, &n) != 0) {
        wipe(&n, sizeof n);
        return fail(err, ecap, "store");
    }
    *st = n;
    wipe(&n, sizeof n);
    return 0;
}

int lacc_op_check(const lacc_srv_t *s, const char *name, int *available, char *err, size_t ecap)
{
    lacc_resp_t r;
    char e[64], body[128];
    *available = 0;
    if (!lacc_valid_name(name)) return fail(err, ecap, "invalid_name");
    lacc_js_esc(name, e, sizeof e);
    _snprintf_s(body, sizeof body, _TRUNCATE, "{\"name\":\"%s\"}", e);
    if (post(s, "check", body, &r, err, ecap) != 0) return 1;
    if (!lacc_js_bool(r.body, "available", available)) return fail(err, ecap, "bad");
    return 0;
}

int lacc_op_create(const lacc_srv_t *s, const char *dir, const char *name, lacc_store_t *st,
                   char *err, size_t ecap)
{
    lacc_resp_t r;
    char e[64], body[128];
    if (!lacc_valid_name(name)) return fail(err, ecap, "invalid_name");
    lacc_js_esc(name, e, sizeof e);
    _snprintf_s(body, sizeof body, _TRUNCATE, "{\"name\":\"%s\"}", e);
    if (post(s, "create", body, &r, err, ecap) != 0) return 1;
    return take_key(s, dir, &r, st, err, ecap);
}

int lacc_op_status(const lacc_srv_t *s, const lacc_store_t *st, lacc_status_t *o, char *err, size_t ecap)
{
    lacc_resp_t r;
    long long v = 0;
    memset(o, 0, sizeof *o);
    if (post_auth(s, st, "status", "", &r, err, ecap) != 0) return 1;
    lacc_js_bool(r.body, "discord", &o->discord);
    lacc_js_bool(r.body, "login", &o->login);
    lacc_js_str(r.body, "login_name", o->login_name, sizeof o->login_name);
    if (lacc_js_int(r.body, "created", &v)) o->created = v;
    if (lacc_js_int(r.body, "gen", &v)) o->gen = (int)v;
    return 0;
}

int lacc_op_login_set(const lacc_srv_t *s, const lacc_store_t *st, const char *login,
                      const char *password, char *err, size_t ecap)
{
    lacc_resp_t r;
    char el[80], ep[1600], extra[1800];
    int rc;
    if (!lacc_valid_login(login)) return fail(err, ecap, "invalid_login");
    if (!lacc_valid_password(password)) return fail(err, ecap, "weak_password");
    lacc_js_esc(login, el, sizeof el);
    lacc_js_esc(password, ep, sizeof ep);
    _snprintf_s(extra, sizeof extra, _TRUNCATE, ",\"login\":\"%s\",\"password\":\"%s\"", el, ep);
    rc = post_auth(s, st, "login_set", extra, &r, err, ecap);
    wipe(ep, sizeof ep);
    wipe(extra, sizeof extra);
    return rc;
}

int lacc_op_login(const lacc_srv_t *s, const char *dir, const char *login, const char *password,
                  lacc_store_t *st, char *err, size_t ecap)
{
    lacc_resp_t r;
    char el[80], ep[1600], body[1800];
    int rc;
    if (!lacc_valid_login(login) || !password || !*password) return fail(err, ecap, "login_failed");
    lacc_js_esc(login, el, sizeof el);
    lacc_js_esc(password, ep, sizeof ep);
    _snprintf_s(body, sizeof body, _TRUNCATE, "{\"login\":\"%s\",\"password\":\"%s\"}", el, ep);
    rc = post(s, "login", body, &r, err, ecap);
    wipe(ep, sizeof ep);
    wipe(body, sizeof body);
    if (rc != 0) return 1;
    return take_key(s, dir, &r, st, err, ecap);
}

static int redeem(const lacc_srv_t *s, const char *dir, const char *action, const char *code, lacc_store_t *st,
                  char *err, size_t ecap)
{
    lacc_resp_t r;
    char c[80], body[128];
    size_t i, o = 0;
    for (i = 0; code && code[i] && o + 1 < sizeof c; i++) { /* nur A-Z a-z 0-9 - (tolerant, Server normalisiert) */
        char ch = code[i];
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-') c[o++] = ch;
    }
    c[o] = '\0';
    if (o < 8) return fail(err, ecap, "code_invalid");
    _snprintf_s(body, sizeof body, _TRUNCATE, "{\"code\":\"%s\"}", c);
    if (post(s, action, body, &r, err, ecap) != 0) return 1;
    return take_key(s, dir, &r, st, err, ecap);
}

int lacc_op_transfer(const lacc_srv_t *s, const char *dir, const char *code, lacc_store_t *st,
                     char *err, size_t ecap)
{
    return redeem(s, dir, "transfer_redeem", code, st, err, ecap);
}

int lacc_op_recovery(const lacc_srv_t *s, const char *dir, const char *code, lacc_store_t *st,
                     char *err, size_t ecap)
{
    return redeem(s, dir, "recovery_redeem", code, st, err, ecap);
}

int lacc_op_logout_all(const lacc_srv_t *s, const char *dir, lacc_store_t *st, char *err, size_t ecap)
{
    lacc_resp_t r;
    lacc_store_t n = *st;
    long long g = 0;
    char kh[80];
    if (post_auth(s, st, "logout_all", "", &r, err, ecap) != 0) return 1;
    if (!lacc_js_int(r.body, "gen", &g) || !lacc_js_str(r.body, "key", kh, sizeof kh) || !lacc_unhex(kh, n.key, 32)) {
        wipe(kh, sizeof kh);
        wipe(r.body, sizeof r.body);
        wipe(&n, sizeof n);
        return fail(err, ecap, "bad");
    }
    wipe(kh, sizeof kh);
    wipe(r.body, sizeof r.body);
    n.gen = (int)g;
    if (lacc_store_save(dir, &n) != 0) {
        wipe(&n, sizeof n);
        return fail(err, ecap, "store");
    }
    *st = n;
    wipe(&n, sizeof n);
    return 0;
}

int lacc_op_delete(const lacc_srv_t *s, const char *dir, lacc_store_t *st, char *err, size_t ecap)
{
    lacc_resp_t r;
    if (post_auth(s, st, "delete", "", &r, err, ecap) != 0) return 1;
    lacc_store_forget(dir);
    wipe(st, sizeof *st);
    return 0;
}

/* ---------------------------------------------------------------- Discord */
static void url_enc(const char *in, char *out, size_t cap)
{
    static const char h[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *in && o + 4 < cap; in++) {
        unsigned char c = (unsigned char)*in;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = h[c >> 4];
            out[o++] = h[c & 15];
        }
    }
    out[o] = '\0';
}

/* "http://127.0.0.1:<port>/discord" -> port (sonst 0) */
static int redirect_port(const char *uri)
{
    const char *p = "http://127.0.0.1:";
    char *e;
    long v;
    if (strncmp(uri, p, strlen(p)) != 0) return 0;
    v = strtol(uri + strlen(p), &e, 10);
    if (v <= 0 || v > 65535 || strcmp(e, "/discord") != 0) return 0;
    return (int)v;
}

/* Erlaubte Autorisierungs-Basis: fest (discord.com) oder Test-Ueberschreibung nur Loopback */
static void authorize_base(char *out, size_t cap)
{
    char v[256];
    strcpy_s(out, cap, SB_DISCORD_AUTHORIZE_URL);
    if (u8_GetEnv("SB_DISCORD_AUTHORIZE", v, sizeof v) > 0 &&
        (strncmp(v, "http://127.0.0.1:", 17) == 0 || strncmp(v, "http://localhost:", 17) == 0) &&
        !strpbrk(v, "?#\"' ")) {
        strcpy_s(out, cap, v);
    }
}

static int test_no_browser(void)
{
    char v[8];
    return u8_GetEnv("SB_TEST_NO_BROWSER", v, sizeof v) > 0 && v[0] == '1';
}

int lacc_op_discord(const lacc_srv_t *s, const char *dir, int login_mode, lacc_store_t *st,
                    int timeout_ms, volatile long *cancel, char *err, size_t ecap)
{
    lacc_resp_t r;
    char client_id[64] = "", redirect[128] = "", base[256], url[1400], e_red[200], verifier[64], challenge[48];
    char state[40], code[512], extra[1400];
    uint8_t rnd[32];
    int on = 0, port, rc;
    long long sock;
    if (!login_mode && !st->have) return fail(err, ecap, "no_account");
    if (lacc_http(s, "GET", "/v1/info", NULL, &r) != 0 || !r.ok) return fail(err, ecap, r.error);
    lacc_js_bool(r.body, "discord", &on);
    lacc_js_str(r.body, "discord_client_id", client_id, sizeof client_id);
    lacc_js_str(r.body, "discord_redirect_uri", redirect, sizeof redirect);
    if (SB_DISCORD_CLIENT_ID[0]) strcpy_s(client_id, sizeof client_id, SB_DISCORD_CLIENT_ID);
    if (!on || !client_id[0]) return fail(err, ecap, "discord_off");
    port = redirect_port(redirect);
    if (!port) {
        port = SB_DISCORD_REDIRECT_PORT;
        _snprintf_s(redirect, sizeof redirect, _TRUNCATE, "http://127.0.0.1:%d/discord", port);
    }
    sock = lacc_loop_open(port, NULL);
    if (sock < 0) return fail(err, ecap, "port_busy");
    if (!lacc_random(rnd, 32)) {
        lacc_loop_close(sock);
        return fail(err, ecap, "bad");
    }
    b64url(rnd, 32, verifier); /* 43 Zeichen */
    pkce_challenge(verifier, challenge);
    if (!lacc_random(rnd, 16)) {
        lacc_loop_close(sock);
        return fail(err, ecap, "bad");
    }
    lacc_hex(rnd, 16, state);
    authorize_base(base, sizeof base);
    url_enc(redirect, e_red, sizeof e_red);
    {
        char e_id[200];
        url_enc(client_id, e_id, sizeof e_id);
        _snprintf_s(url, sizeof url, _TRUNCATE,
                    "%s?response_type=code&client_id=%s&scope=identify&redirect_uri=%s&state=%s"
                    "&code_challenge=%s&code_challenge_method=S256",
                    base, e_id, e_red, state, challenge);
    }
    if (test_no_browser()) {
        printf("[discord-url] %s\n", url);
    } else {
        wchar_t wu[1400];
        u8_to_w(url, wu, 1400);
        if ((INT_PTR)ShellExecuteW(NULL, L"open", wu, NULL, NULL, SW_SHOWNORMAL) <= 32) {
            lacc_loop_close(sock);
            return fail(err, ecap, "browser");
        }
    }
    rc = lacc_loop_wait(sock, state, timeout_ms, cancel, code, sizeof code, err, ecap);
    lacc_loop_close(sock);
    if (rc != 0) {
        wipe(verifier, sizeof verifier);
        return 1;
    }
    {
        char ec[600];
        lacc_js_esc(code, ec, sizeof ec);
        _snprintf_s(extra, sizeof extra, _TRUNCATE, "%s\"code\":\"%s\",\"verifier\":\"%s\",\"redirect_uri\":\"%s\"",
                    login_mode ? "" : ",", ec, verifier, redirect);
        wipe(ec, sizeof ec);
    }
    wipe(code, sizeof code);
    wipe(verifier, sizeof verifier);
    if (login_mode) {
        char body[1500];
        _snprintf_s(body, sizeof body, _TRUNCATE, "{%s}", extra);
        rc = post(s, "discord_login", body, &r, err, ecap);
        wipe(body, sizeof body);
        wipe(extra, sizeof extra);
        if (rc != 0) return 1;
        return take_key(s, dir, &r, st, err, ecap);
    }
    rc = post_auth(s, st, "discord_link", extra, &r, err, ecap);
    wipe(extra, sizeof extra);
    return rc;
}

/* ---------------------------------------------------------------- Loopback-Empfaenger */
static int g_wsa;

long long lacc_loop_open(int port, int *actual)
{
    SOCKET s;
    struct sockaddr_in a;
    int al = sizeof a;
    BOOL ex = TRUE;
    if (!g_wsa) {
        WSADATA wd;
        if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) return -1;
        g_wsa = 1;
    }
    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return -1;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&ex, sizeof ex);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* nur 127.0.0.1 */
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0 || listen(s, 4) != 0) {
        closesocket(s);
        return -1;
    }
    if (actual) {
        getsockname(s, (struct sockaddr *)&a, &al);
        *actual = ntohs(a.sin_port);
    }
    return (long long)s;
}

void lacc_loop_close(long long sock)
{
    if (sock >= 0) closesocket((SOCKET)sock);
}

static void q_get(const char *q, const char *key, char *out, size_t cap)
{
    size_t kl = strlen(key), o = 0;
    out[0] = '\0';
    while (q && *q) {
        if (strncmp(q, key, kl) == 0 && q[kl] == '=') {
            const char *p = q + kl + 1;
            while (*p && *p != '&' && *p != ' ' && o + 1 < cap) {
                if (*p == '%' && hexval(p[1]) >= 0 && hexval(p[2]) >= 0) {
                    out[o++] = (char)(hexval(p[1]) * 16 + hexval(p[2]));
                    p += 3;
                } else {
                    out[o++] = *p == '+' ? ' ' : *p;
                    p++;
                }
            }
            out[o] = '\0';
            return;
        }
        q = strchr(q, '&');
        if (q) q++;
    }
}

static void loop_reply(SOCKET c, int ok)
{
    char resp[1200];
    const char *body = ok ? "<!doctype html><meta charset=\"utf-8\"><title>Steelbound</title>"
                            "<body style=\"background:#111;color:#eee;font:16px sans-serif;padding:40px\">"
                            "<h2>Steelbound</h2><p>Fertig &ndash; du kannst das Fenster schlie&szlig;en.</p>"
                            "<p>Done &ndash; you can close this window.</p></body>"
                          : "<!doctype html><meta charset=\"utf-8\"><title>Steelbound</title>"
                            "<body style=\"background:#111;color:#eee;font:16px sans-serif;padding:40px\">"
                            "<h2>Steelbound</h2><p>Anmeldung abgebrochen oder ung&uuml;ltig. Bitte im Launcher neu "
                            "versuchen.</p><p>Sign-in cancelled or invalid. Please try again in the launcher.</p></body>";
    int n = _snprintf_s(resp, sizeof resp, _TRUNCATE,
                        "HTTP/1.1 %s\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %u\r\n"
                        "Cache-Control: no-store\r\nReferrer-Policy: no-referrer\r\n"
                        "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'\r\n"
                        "Connection: close\r\n\r\n%s",
                        ok ? "200 OK" : "400 Bad Request", (unsigned)strlen(body), body);
    if (n > 0) send(c, resp, n, 0);
}

int lacc_loop_wait(long long sock, const char *state, int timeout_ms, volatile long *cancel,
                   char *code, size_t ccap, char *err, size_t ecap)
{
    ULONGLONG end = GetTickCount64() + (ULONGLONG)(timeout_ms > 0 ? timeout_ms : 180000);
    code[0] = '\0';
    for (;;) {
        fd_set rs;
        struct timeval tv = { 0, 250000 };
        SOCKET c;
        char req[4096], path[2048], v[600];
        int got = 0, n;
        DWORD to = 3000;
        if (cancel && *cancel) return fail(err, ecap, "cancel");
        if (GetTickCount64() >= end) return fail(err, ecap, "timeout");
        FD_ZERO(&rs);
        FD_SET((SOCKET)sock, &rs);
        if (select(0, &rs, NULL, NULL, &tv) <= 0) continue;
        c = accept((SOCKET)sock, NULL, NULL);
        if (c == INVALID_SOCKET) continue;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof to);
        {
            /* W329: Gesamtfrist je Verbindung (sonst haelt ein tropfender Absender den Empfaenger stundenlang) */
            ULONGLONG cend = GetTickCount64() + 3000;
            while (got < (int)sizeof req - 1 && GetTickCount64() < cend &&
                   (n = recv(c, req + got, (int)sizeof req - 1 - got, 0)) > 0) {
                got += n;
                req[got] = '\0';
                if (strstr(req, "\r\n\r\n")) break;
            }
        }
        req[got] = '\0';
        if (got < 5 || strncmp(req, "GET ", 4) != 0 || sscanf_s(req + 4, "%2047s", path, (unsigned)sizeof path) != 1 ||
            strncmp(path, "/discord", 8) != 0 || (path[8] != '?' && path[8] != '\0')) {
            static const char k404[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            send(c, k404, (int)sizeof k404 - 1, 0);
            closesocket(c);
            continue; /* z. B. favicon: weiter warten */
        }
        {
            const char *q = path[8] == '?' ? path + 9 : "";
            /* W329: erst state pruefen. Falscher/fehlender state = fremde Anfrage (z. B. eine Webseite, die
             * http://127.0.0.1:47090/discord?... aufruft): ablehnen und WEITER warten - sie darf die echte
             * Anmeldung weder ersetzen noch abbrechen. Auch "error" zaehlt nur mit richtigem state. */
            q_get(q, "state", v, sizeof v);
            if (!state || !state[0] || strlen(v) != strlen(state) || strcmp(v, state) != 0) {
                loop_reply(c, 0);
                closesocket(c);
                wipe(req, sizeof req);
                wipe(path, sizeof path);
                continue;
            }
            q_get(q, "error", v, sizeof v);
            if (v[0]) {
                loop_reply(c, 0);
                closesocket(c);
                return fail(err, ecap, "denied");
            }
            q_get(q, "code", code, ccap);
        }
        wipe(req, sizeof req);
        wipe(path, sizeof path);
        loop_reply(c, code[0] != 0);
        shutdown(c, SD_SEND);
        closesocket(c);
        if (!code[0]) return fail(err, ecap, "bad");
        err[0] = '\0';
        return 0;
    }
}

/* ====================================================================== Kindprozess-Umgebung */
static int env_cmp(const void *a, const void *b)
{
    const wchar_t *x = *(const wchar_t *const *)a, *y = *(const wchar_t *const *)b;
    /* Namen vergleichen (bis '=', fuehrendes '=' der Laufwerkseintraege zaehlt mit) */
    for (;; x++, y++) {
        wchar_t cx = (*x == L'=' && x != *(const wchar_t *const *)a) ? 0 : towupper(*x);
        wchar_t cy = (*y == L'=' && y != *(const wchar_t *const *)b) ? 0 : towupper(*y);
        if (cx != cy) return cx < cy ? -1 : 1;
        if (!cx) return 0;
    }
}

wchar_t *lacc_env_block(const char *name, const char *value)
{
    wchar_t *env = GetEnvironmentStringsW(), *p, **v = NULL, *out = NULL, wn[64], *entry;
    size_t cnt = 0, total = 0, nl, i, o = 0;
    int k;
    if (!env) return NULL;
    u8_to_w(name, wn, 64);
    nl = wcslen(wn);
    for (p = env; *p; p += wcslen(p) + 1) cnt++;
    v = (wchar_t **)calloc(cnt + 1, sizeof *v);
    k = (int)(strlen(name) + strlen(value) + 2);
    entry = (wchar_t *)calloc((size_t)k * 2, sizeof(wchar_t));
    if (!v || !entry) {
        free(v);
        free(entry);
        FreeEnvironmentStringsW(env);
        return NULL;
    }
    {
        int wl = u8_to_w(name, entry, k * 2);
        (void)wl;
        wcscat_s(entry, (size_t)k * 2, L"=");
        u8_to_w(value, entry + wcslen(entry), (int)((size_t)k * 2 - wcslen(entry)));
    }
    cnt = 0;
    for (p = env; *p; p += wcslen(p) + 1) {
        if (_wcsnicmp(p, wn, nl) == 0 && p[nl] == L'=') continue; /* alter Wert raus */
        v[cnt++] = p;
    }
    v[cnt++] = entry;
    qsort(v, cnt, sizeof *v, env_cmp);
    for (i = 0; i < cnt; i++) total += wcslen(v[i]) + 1;
    out = (wchar_t *)malloc((total + 1) * sizeof(wchar_t));
    if (out) {
        for (i = 0; i < cnt; i++) {
            size_t l = wcslen(v[i]) + 1;
            memcpy(out + o, v[i], l * sizeof(wchar_t));
            o += l;
        }
        out[o] = 0;
    }
    SecureZeroMemory(entry, (size_t)k * 2 * sizeof(wchar_t));
    free(entry);
    free(v);
    FreeEnvironmentStringsW(env);
    return out;
}

/* ====================================================================== CLI */
static int cli_out_store(const char *tag, const lacc_store_t *st)
{
    printf("ok %saccount=%lu name=%s gen=%d\n", tag, (unsigned long)st->account, st->name, st->gen);
    return 0;
}

int lacc_cli(const char *client_dir, int argc, char **argv)
{
    lacc_srv_t s;
    lacc_store_t st;
    char dir[1200], err[64] = "";
    const char *act = argc > 0 ? argv[0] : "";
    const char *a1 = argc > 1 ? argv[1] : "", *a2 = argc > 2 ? argv[2] : "";
    int rc = 1, ld;
    lacc_server(&s);
    lacc_default_dir(client_dir, dir, sizeof dir);
    ld = lacc_store_load(dir, &st);
    if (strcmp(act, "local") == 0) {
        if (ld == 1) {
            printf("ok have=1 account=%lu name=%s gen=%d host=%s\n", (unsigned long)st.account, st.name, st.gen,
                   st.host);
        } else {
            printf("ok have=0%s\n", ld < 0 ? " unreadable=1" : "");
        }
        return 0;
    }
    if (strcmp(act, "forget") == 0) {
        rc = lacc_store_forget(dir);
        printf(rc == 0 ? "ok forgotten=1\n" : "error store\n");
        return rc == 0 ? 0 : 1;
    }
    if (strcmp(act, "recovery-show") == 0) {
        char c[48];
        if (ld != 1) {
            printf("error no_account\n");
            return 1;
        }
        lacc_recovery_code(st.account, st.key, c, sizeof c);
        printf("ok code=%s\n", c);
        wipe(c, sizeof c);
        return 0;
    }
    if (strcmp(act, "game-auth") == 0) {
        char a[160];
        if (ld != 1) {
            printf("error no_account\n");
            return 1;
        }
        lacc_game_auth(&st, (long long)time(NULL), a, sizeof a);
        printf("ok auth=%s pilot=%s\n", a, st.name);
        wipe(a, sizeof a);
        return 0;
    }
    if (!s.pin1[0] && !s.pin2[0]) {
        printf("error off\n");
        return 1;
    }
    if (strcmp(act, "info") == 0) {
        lacc_resp_t r;
        if (lacc_http(&s, "GET", "/v1/info", NULL, &r) == 0 && r.ok) {
            int d = 0;
            lacc_js_bool(r.body, "discord", &d);
            printf("ok discord=%d\n", d);
            return 0;
        }
        printf("error %s\n", r.error);
        return 1;
    }
    if (strcmp(act, "check") == 0) {
        int av = 0;
        rc = lacc_op_check(&s, a1, &av, err, sizeof err);
        if (rc == 0) printf("ok available=%d\n", av);
    } else if (strcmp(act, "create") == 0) {
        rc = lacc_op_create(&s, dir, a1, &st, err, sizeof err);
        if (rc == 0) cli_out_store("", &st);
    } else if (strcmp(act, "status") == 0) {
        lacc_status_t o;
        rc = ld == 1 ? lacc_op_status(&s, &st, &o, err, sizeof err) : fail(err, sizeof err, "no_account");
        if (rc == 0) {
            printf("ok account=%lu name=%s gen=%d discord=%d login=%d login_name=%s\n", (unsigned long)st.account,
                   st.name, o.gen, o.discord, o.login, o.login_name);
        }
    } else if (strcmp(act, "discord-link") == 0 || strcmp(act, "discord-login") == 0) {
        int lm = strcmp(act, "discord-login") == 0;
        if (!lm && ld != 1) {
            rc = fail(err, sizeof err, "no_account");
        } else {
            rc = lacc_op_discord(&s, dir, lm, &st, 60000, NULL, err, sizeof err);
        }
        if (rc == 0) {
            if (lm) cli_out_store("", &st);
            else printf("ok discord=1\n");
        }
    } else if (strcmp(act, "login-set") == 0) {
        rc = ld == 1 ? lacc_op_login_set(&s, &st, a1, a2, err, sizeof err) : fail(err, sizeof err, "no_account");
        if (rc == 0) printf("ok login=1 login_name=%s\n", a1);
    } else if (strcmp(act, "login") == 0) {
        rc = lacc_op_login(&s, dir, a1, a2, &st, err, sizeof err);
        if (rc == 0) cli_out_store("", &st);
    } else if (strcmp(act, "transfer") == 0) {
        rc = lacc_op_transfer(&s, dir, a1, &st, err, sizeof err);
        if (rc == 0) cli_out_store("", &st);
    } else if (strcmp(act, "recovery") == 0) {
        rc = lacc_op_recovery(&s, dir, a1, &st, err, sizeof err);
        if (rc == 0) cli_out_store("", &st);
    } else if (strcmp(act, "logout-all") == 0) {
        rc = ld == 1 ? lacc_op_logout_all(&s, dir, &st, err, sizeof err) : fail(err, sizeof err, "no_account");
        if (rc == 0) printf("ok gen=%d\n", st.gen);
    } else if (strcmp(act, "delete") == 0) {
        rc = ld == 1 ? lacc_op_delete(&s, dir, &st, err, sizeof err) : fail(err, sizeof err, "no_account");
        if (rc == 0) printf("ok deleted=1\n");
    } else {
        printf("error usage (create NAME | check NAME | status | local | info | discord-link | discord-login | "
               "login-set LOGIN PASS | login LOGIN PASS | transfer CODE | recovery-show | recovery CODE | "
               "logout-all | delete | game-auth | forget)\n");
        return 2;
    }
    if (rc != 0) printf("error %s\n", err[0] ? err : "bad");
    wipe(&st, sizeof st);
    return rc == 0 ? 0 : 1;
}

/* ====================================================================== Selbsttest */
typedef struct {
    int port;
    char req[600], req2[600]; /* req2: zweite Verbindung danach ("" = keine) */
} loop_cli_t;

static void loop_send1(int port, const char *req)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a;
    char buf[2048];
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (s != INVALID_SOCKET && connect(s, (struct sockaddr *)&a, sizeof a) == 0) {
        send(s, req, (int)strlen(req), 0);
        recv(s, buf, sizeof buf, 0);
    }
    if (s != INVALID_SOCKET) closesocket(s);
}

static DWORD WINAPI loop_client(LPVOID p)
{
    loop_cli_t *lc = (loop_cli_t *)p;
    loop_send1(lc->port, lc->req);
    if (lc->req2[0]) loop_send1(lc->port, lc->req2);
    return 0;
}

static int loop_case2(const char *req, const char *req2, const char *state, int timeout_ms, char *code, size_t ccap,
                      char *err, size_t ecap)
{
    loop_cli_t lc;
    HANDLE th;
    long long s = lacc_loop_open(0, &lc.port);
    int rc;
    if (s < 0) return fail(err, ecap, "open");
    strcpy_s(lc.req, sizeof lc.req, req);
    strcpy_s(lc.req2, sizeof lc.req2, req2 ? req2 : "");
    th = CreateThread(NULL, 0, loop_client, &lc, 0, NULL);
    rc = lacc_loop_wait(s, state, timeout_ms, NULL, code, ccap, err, ecap);
    if (th) {
        WaitForSingleObject(th, 5000);
        CloseHandle(th);
    }
    lacc_loop_close(s);
    return rc;
}

static int loop_case(const char *req, const char *state, char *code, size_t ccap, char *err, size_t ecap)
{
    return loop_case2(req, NULL, state, 5000, code, ccap, err, ecap);
}

void lacc_selftest(int *pass, int *fail_n)
{
    char why[300] = "", h[80], c[64];
    uint8_t K[32], G[32], d[32], k16[16];
    uint32_t acc = 0;
    int i;
#define ST_RESULT(name)                                       \
    do {                                                      \
        if (why[0]) {                                         \
            printf("FAIL %s: %s\n", name, why);               \
            (*fail_n)++;                                      \
        } else {                                              \
            printf("PASS %s\n", name);                        \
            (*pass)++;                                        \
        }                                                     \
        why[0] = '\0';                                        \
    } while (0)

    /* 1: Testvektoren (RFC 4231 + eigene, mit Python hmac/hashlib/base64 nachgerechnet) */
    lacc_hmac((const uint8_t *)"Jefe", 4, "what do ya want for nothing?", 28, d);
    lacc_hex(d, 32, h);
    if (strcmp(h, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843") != 0)
        strcpy_s(why, sizeof why, "HMAC RFC4231 #2");
    {
        uint8_t big[131];
        static const char m[] = "Test Using Larger Than Block-Size Key - Hash Key First";
        memset(big, 0xaa, sizeof big);
        lacc_hmac(big, sizeof big, m, sizeof m - 1, d);
        lacc_hex(d, 32, h);
        if (!why[0] && strcmp(h, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54") != 0)
            strcpy_s(why, sizeof why, "HMAC RFC4231 #6 (langer Schluessel)");
    }
    for (i = 0; i < 32; i++) K[i] = (uint8_t)i;
    lacc_game_key(K, 42, 1700000000LL, G);
    lacc_hex(G, 32, h);
    if (!why[0] && strcmp(h, "ac52b91d5d8374dd9c6087ca976814c887165cd602650d2ab55ba7c525995153") != 0)
        strcpy_s(why, sizeof why, "Spielschluessel G");
    lacc_api_proof(K, "status", "00112233445566778899aabbccddeeff", 42, h);
    if (!why[0] && strcmp(h, "b27dd17e05e63585666d98ab7ce4fed1278520b67dc5b386d3fd01e0d02e5aee") != 0)
        strcpy_s(why, sizeof why, "API-Beweis");
    lacc_auth_proof(G, "00112233445566778899aabbccddeeff", 7, h);
    if (!why[0] && strcmp(h, "460be8bb42f5853fef1c72cbcb50f15d6b64ff8db2c8988f68aa54ce69c09791") != 0)
        strcpy_s(why, sizeof why, "Spiel-Beweis (auth)");
    lacc_recovery_code(42, K, c, sizeof c);
    if (!why[0] && strcmp(c, "AAAA-AKQA-AEBA-GBAF-AYDQ-QCIK-BMGA-2DQP") != 0)
        _snprintf_s(why, sizeof why, _TRUNCATE, "Wiederherstellungscode %s", c);
    pkce_challenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk", h);
    if (!why[0] && strcmp(h, "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM") != 0)
        strcpy_s(why, sizeof why, "PKCE S256 (RFC 7636 Anhang B)");
    {
        lacc_store_t st;
        char a[160], gh[65];
        memset(&st, 0, sizeof st);
        st.have = 1;
        st.account = 42;
        memcpy(st.key, K, 32);
        lacc_game_auth(&st, 1700000000LL - LACC_GAME_TTL, a, sizeof a);
        lacc_hex(G, 32, gh);
        _snprintf_s(h, sizeof h, _TRUNCATE, "42:1700000000:%s", gh);
        if (!why[0] && strcmp(a, h) != 0) strcpy_s(why, sizeof why, "SB_LOC_AUTH-Format");
    }
    ST_RESULT("W328 account: HMAC-SHA256 (RFC 4231), game key G, API/auth proofs, recovery code, PKCE S256 (vectors from Python)");

    /* 2: Base32-Rundweg + tolerantes Parsen, Namen/Login/Passwort */
    {
        uint8_t raw[23], back[32];
        char e[64];
        int n, len;
        for (len = 0; len <= 23 && !why[0]; len++) {
            for (i = 0; i < len; i++) raw[i] = (uint8_t)(i * 37 + len);
            lacc_b32_enc(raw, (size_t)len, e);
            n = lacc_b32_dec(e, back, sizeof back);
            if (n != len || memcmp(raw, back, (size_t)len) != 0)
                _snprintf_s(why, sizeof why, _TRUNCATE, "Rundweg Laenge %d", len);
        }
        if (!why[0] && (!lacc_recovery_parse(" aaaa-akqa-AEBA gbaf-aydq-qcik-bmga-2dqp ", &acc, k16) || acc != 42 ||
                        memcmp(k16, K, 16) != 0))
            strcpy_s(why, sizeof why, "tolerantes Parsen (klein, Leerzeichen, Striche)");
        if (!why[0] && (lacc_recovery_parse("AAAA-AKQA-AEB1", &acc, k16) || lacc_b32_dec("AB!C", back, 32) >= 0))
            strcpy_s(why, sizeof why, "ungueltiger Code angenommen");
        if (!why[0] && (!lacc_valid_name("Ace_1") || !lacc_valid_name("x y-z") || lacc_valid_name("") ||
                        lacc_valid_name("abcdefghijklmnopq") || lacc_valid_name("Guest-Ace") ||
                        lacc_valid_name("gast-x") || lacc_valid_name(" Ace") || lacc_valid_name("Ace ") || lacc_valid_name("A\xC3\xA4")))
            strcpy_s(why, sizeof why, "Pilotnamen-Regel");
        if (!why[0] && (!lacc_valid_login("abc") || lacc_valid_login("ab") || lacc_valid_login("a@b.c") ||
                        !lacc_valid_password("0123456789") || lacc_valid_password("012345678")))
            strcpy_s(why, sizeof why, "Login/Passwort-Regel");
    }
    ST_RESULT("W328 account: Base32 roundtrip 0..23 bytes, tolerant recovery parsing, name/login/password rules");

    /* 3: Pin-Vergleich (richtig/falsch/leer, zwei Pins, Normalisierung), Test-Ueberschreibung nur Loopback */
    {
        lacc_srv_t s;
        uint8_t sha[32];
        char keep_api[200] = "", keep_pin[200] = "";
        int had_api = u8_GetEnv("SB_STEELNET_API", keep_api, sizeof keep_api) > 0;
        int had_pin = u8_GetEnv("SB_STEELNET_PIN", keep_pin, sizeof keep_pin) > 0;
        for (i = 0; i < 32; i++) sha[i] = (uint8_t)(0xA0 + i);
        memset(&s, 0, sizeof s);
        if (lacc_pin_match(&s, sha)) strcpy_s(why, sizeof why, "leerer Pin passt");
        lacc_hex(sha, 32, s.pin2);
        if (!why[0] && !lacc_pin_match(&s, sha)) strcpy_s(why, sizeof why, "Pin2 passt nicht");
        sha[31] ^= 1;
        if (!why[0] && lacc_pin_match(&s, sha)) strcpy_s(why, sizeof why, "falscher Pin passt");
        if (!why[0] && (!norm_pin("A0:A1:A2:A3:A4:A5:A6:A7:A8:A9:AA:AB:AC:AD:AE:AF:B0:B1:B2:B3:B4:B5:B6:B7:B8:B9:BA:BB:BC:BD:BE:BF", h) ||
                        strcmp(h, "a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebf") != 0 ||
                        norm_pin("abc", h) || norm_pin("zz", h)))
            strcpy_s(why, sizeof why, "Pin-Normalisierung");
        SetEnvironmentVariableA("SB_STEELNET_API", "10.0.0.5:47001"); /* kein Loopback: ignoriert */
        SetEnvironmentVariableA("SB_STEELNET_PIN", "a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebf");
        lacc_server(&s);
        if (!why[0] && (s.test || strcmp(s.host, SB_STEELNET_HOST) != 0 || s.port != SB_STEELNET_API_PORT))
            strcpy_s(why, sizeof why, "Ueberschreibung mit fremder Adresse angenommen");
        SetEnvironmentVariableA("SB_STEELNET_API", "127.0.0.1:47123");
        if (!why[0] && (!lacc_server(&s) || !s.test || s.port != 47123 || strcmp(s.pin1, keep_pin[0] ? s.pin1 : s.pin1) != 0))
            strcpy_s(why, sizeof why, "Loopback-Ueberschreibung");
        SetEnvironmentVariableA("SB_STEELNET_API", had_api ? keep_api : NULL);
        SetEnvironmentVariableA("SB_STEELNET_PIN", had_pin ? keep_pin : NULL);
        if (!why[0] && !had_api && lacc_server(&s) != (SB_STEELNET_PIN1[0] || SB_STEELNET_PIN2[0]))
            strcpy_s(why, sizeof why, "Standard-Konfiguration (leerer Pin = aus)");
        {
            lacc_resp_t r;
            memset(&s, 0, sizeof s);
            strcpy_s(s.host, sizeof s.host, "127.0.0.1");
            s.port = 9;
            if (!why[0] && (lacc_http(&s, "GET", "/v1/info", NULL, &r) == 0 || strcmp(r.error, "off") != 0))
                strcpy_s(why, sizeof why, "ohne Pin kein Netzzugriff");
        }
    }
    ST_RESULT("W328 account: certificate pin compare (match/mismatch/empty, two pins, normalising), override loopback only, no pin = off");

    /* 4: DPAPI-Rundweg im Temp-Profilordner; Datei ohne Klartext; kaputt = unlesbar; vergessen */
    {
        char tmp[1024], dir[1100], path[1200], kh[65];
        lacc_store_t a, b;
        uint8_t buf[4096];
        size_t n = 0;
        FILE *f = NULL;
        u8_GetTempPath(tmp, sizeof tmp);
        _snprintf_s(dir, sizeof dir, _TRUNCATE, "%sw328_acc\\profil", tmp);
        lacc_store_forget(dir);
        memset(&a, 0, sizeof a);
        a.have = 1;
        a.account = 4242;
        a.gen = 3;
        strcpy_s(a.name, sizeof a.name, "Test Pilot");
        strcpy_s(a.host, sizeof a.host, "127.0.0.1:47001");
        for (i = 0; i < 32; i++) a.key[i] = (uint8_t)(0x5A ^ i);
        if (lacc_store_load(dir, &b) != 0) strcpy_s(why, sizeof why, "leerer Ordner != kein Konto");
        if (!why[0] && lacc_store_save(dir, &a) != 0) strcpy_s(why, sizeof why, "speichern");
        if (!why[0] && (lacc_store_load(dir, &b) != 1 || b.account != 4242 || b.gen != 3 ||
                        strcmp(b.name, "Test Pilot") != 0 || memcmp(b.key, a.key, 32) != 0 ||
                        strcmp(b.host, a.host) != 0))
            strcpy_s(why, sizeof why, "Rundweg");
        lacc_store_path(dir, path, sizeof path);
        if (!why[0] && u8_fopen_s(&f, path, "rb") == 0 && f) {
            n = fread(buf, 1, sizeof buf - 1, f);
            fclose(f);
            buf[n] = 0;
            lacc_hex(a.key, 32, kh);
            for (i = 0; i + 4 <= (int)n && !why[0]; i++) {
                if (memcmp(buf + i, "Test", 4) == 0 || memcmp(buf + i, kh, 4) == 0 || memcmp(buf + i, "key=", 4) == 0)
                    strcpy_s(why, sizeof why, "Klartext in der Datei");
            }
        }
        if (!why[0] && n > 40) { /* ein Byte kippen -> unlesbar (kein Absturz, kein Konto) */
            buf[n / 2] ^= 0x55;
            if (u8_fopen_s(&f, path, "wb") == 0 && f) {
                fwrite(buf, 1, n, f);
                fclose(f);
            }
            if (lacc_store_load(dir, &b) != -1 || b.have) strcpy_s(why, sizeof why, "kaputte Datei angenommen");
        }
        if (!why[0] && (lacc_store_forget(dir) != 0 || lacc_store_load(dir, &b) != 0))
            strcpy_s(why, sizeof why, "vergessen");
        _snprintf_s(path, sizeof path, _TRUNCATE, "%sw328_acc", tmp);
        {
            wchar_t w1[1200];
            u8_to_w(dir, w1, 1200);
            RemoveDirectoryW(w1);
            u8_to_w(path, w1, 1200);
            RemoveDirectoryW(w1);
        }
    }
    ST_RESULT("W328 account: DPAPI key store roundtrip in temp profile, no plaintext on disk, tampered file rejected, forget");

    /* 5: Loopback-Empfaenger (richtiger state, falscher state, Fehler, fremder Pfad wird uebergangen) */
    {
        char code[256], err[32];
        if (loop_case("GET /favicon.ico HTTP/1.1\r\nHost: x\r\n\r\n", "s1", code, sizeof code, err, sizeof err) == 0 ||
            strcmp(err, "timeout") != 0)
            _snprintf_s(why, sizeof why, _TRUNCATE, "fremder Pfad: %s", err);
        if (!why[0] && (loop_case("GET /discord?code=abc%2D123&state=0123abcd HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n",
                                  "0123abcd", code, sizeof code, err, sizeof err) != 0 ||
                        strcmp(code, "abc-123") != 0))
            _snprintf_s(why, sizeof why, _TRUNCATE, "gueltige Rueckleitung: %s", err);
        /* W329: fremde Anfrage (falscher/fehlender state, auch mit error=) wird abgelehnt, bricht aber nicht ab */
        if (!why[0] && (loop_case2("GET /discord?code=abc&state=evil HTTP/1.1\r\n\r\n", NULL, "0123abcd", 1500, code,
                                   sizeof code, err, sizeof err) == 0 ||
                        strcmp(err, "timeout") != 0))
            strcpy_s(why, sizeof why, "falscher state angenommen");
        if (!why[0] && (loop_case2("GET /discord?code=abc HTTP/1.1\r\n\r\n", NULL, "0123abcd", 1500, code, sizeof code,
                                   err, sizeof err) == 0 ||
                        strcmp(err, "timeout") != 0))
            strcpy_s(why, sizeof why, "fehlender state angenommen");
        if (!why[0] && (loop_case2("GET /discord?code=EVIL&state=guess HTTP/1.1\r\n\r\n",
                                   "GET /discord?code=good&state=0123abcd HTTP/1.1\r\n\r\n", "0123abcd", 5000, code,
                                   sizeof code, err, sizeof err) != 0 ||
                        strcmp(code, "good") != 0))
            _snprintf_s(why, sizeof why, _TRUNCATE, "CSRF: fremde Rueckleitung ersetzt/bricht echte ab (%s)", err);
        if (!why[0] && (loop_case2("GET /discord?error=access_denied HTTP/1.1\r\n\r\n",
                                   "GET /discord?code=good&state=0123abcd HTTP/1.1\r\n\r\n", "0123abcd", 5000, code,
                                   sizeof code, err, sizeof err) != 0 ||
                        strcmp(code, "good") != 0))
            _snprintf_s(why, sizeof why, _TRUNCATE, "fremdes error= ohne state bricht ab (%s)", err);
        if (!why[0]) {
            char big[600];
            memset(big, 'A', sizeof big - 1);
            memcpy(big, "GET /discord?code=", 18);
            big[sizeof big - 1] = '\0';
            if (loop_case2(big, "GET /discord?code=ok2&state=0123abcd HTTP/1.1\r\n\r\n", "0123abcd", 8000, code,
                           sizeof code, err, sizeof err) != 0 ||
                strcmp(code, "ok2") != 0)
                _snprintf_s(why, sizeof why, _TRUNCATE, "lange Anfrage ohne Zeilenende (%s)", err);
        }
        if (!why[0] && (loop_case("GET /discord?error=access_denied&state=0123abcd HTTP/1.1\r\n\r\n", "0123abcd", code,
                                  sizeof code, err, sizeof err) == 0 ||
                        strcmp(err, "denied") != 0))
            strcpy_s(why, sizeof why, "Abbruch nicht erkannt");
        if (!why[0] && (redirect_port("http://127.0.0.1:47090/discord") != 47090 ||
                        redirect_port("http://evil.example:47090/discord") != 0 ||
                        redirect_port("http://127.0.0.1:47090/discord?x") != 0))
            strcpy_s(why, sizeof why, "Rueckleitungs-URI");
        {
            char b[256], keep[256] = "";
            int had = u8_GetEnv("SB_DISCORD_AUTHORIZE", keep, sizeof keep) > 0;
            SetEnvironmentVariableA("SB_DISCORD_AUTHORIZE", "https://evil.example/authorize");
            authorize_base(b, sizeof b);
            if (!why[0] && strcmp(b, SB_DISCORD_AUTHORIZE_URL) != 0) strcpy_s(why, sizeof why, "fremde Autorisierungs-URL");
            SetEnvironmentVariableA("SB_DISCORD_AUTHORIZE", "http://127.0.0.1:5555/oauth2/authorize");
            authorize_base(b, sizeof b);
            if (!why[0] && strcmp(b, "http://127.0.0.1:5555/oauth2/authorize") != 0)
                strcpy_s(why, sizeof why, "Loopback-Autorisierung (Test)");
            SetEnvironmentVariableA("SB_DISCORD_AUTHORIZE", had ? keep : NULL);
        }
    }
    ST_RESULT("W328 account: Discord loopback redirect (code ok, wrong/missing state rejected without aborting (W329 CSRF), denied, other paths skipped, slow/long request), fixed authorize URL");

    /* 6: Kindprozess-Umgebung: genau ein SB_LOC_AUTH, eigener Prozess unveraendert, kein K */
    {
        wchar_t *blk, *p;
        char val[160], kh[65], u8[2048], own[8];
        int nauth = 0, ntot = 0, nparent = 0;
        lacc_store_t st;
        wchar_t *penv = GetEnvironmentStringsW();
        for (p = penv; p && *p; p += wcslen(p) + 1) {
            if (_wcsnicmp(p, L"SB_LOC_AUTH=", 12) != 0) nparent++;
        }
        if (penv) FreeEnvironmentStringsW(penv);
        memset(&st, 0, sizeof st);
        st.have = 1;
        st.account = 42;
        memcpy(st.key, K, 32);
        lacc_game_auth(&st, 1700000000LL - LACC_GAME_TTL, val, sizeof val);
        lacc_hex(K, 32, kh);
        blk = lacc_env_block(LACC_ENV_AUTH, val);
        if (!blk) strcpy_s(why, sizeof why, "kein Block");
        for (p = blk; p && *p && !why[0]; p += wcslen(p) + 1) {
            w_to_u8(p, u8, sizeof u8);
            ntot++;
            if (strncmp(u8, "SB_LOC_AUTH=", 12) == 0) {
                nauth++;
                if (strcmp(u8 + 12, val) != 0) strcpy_s(why, sizeof why, "Wert falsch");
            }
            if (strstr(u8, kh) || strstr(u8, "steelnet_account")) strcpy_s(why, sizeof why, "Geheimnis im Block");
        }
        free(blk);
        if (!why[0] && (nauth != 1 || ntot != nparent + 1))
            _snprintf_s(why, sizeof why, _TRUNCATE, "SB_LOC_AUTH %d-mal, %d Eintraege (Eltern %d)", nauth, ntot, nparent);
        if (!why[0] && GetEnvironmentVariableA(LACC_ENV_AUTH, own, sizeof own) > 0)
            strcpy_s(why, sizeof why, "eigene Umgebung veraendert");
        if (!why[0] && (strncmp(val, "42:1700000000:", 14) != 0 || strstr(val, kh)))
            strcpy_s(why, sizeof why, "Uebergabe enthaelt K");
    }
    ST_RESULT("W328 account: child env block has exactly one SB_LOC_AUTH (account:exp:G), never K, launcher env unchanged");

    /* 7: W329 Haertung: JSON-Leser am Pufferende, Pilotname mit Steuerzeichen in der Kontodatei abgelehnt */
    {
        char o[16], tmp[1024], dir[1100];
        lacc_store_t a, b;
        static const char trail[] = "{\"error\":\"ab\\"; /* endet mit Backslash */
        if (lacc_js_str(trail, "error", o, sizeof o) != 0 || strcmp(o, "ab") != 0)
            strcpy_s(why, sizeof why, "JSON: Backslash am Ende");
        if (!why[0] && (!lacc_js_str("{\"n\":\"a\\u0000b\"}", "n", o, sizeof o) || strcmp(o, "a?b") != 0))
            strcpy_s(why, sizeof why, "JSON: \\u0000 kuerzt den Wert");
        u8_GetTempPath(tmp, sizeof tmp);
        _snprintf_s(dir, sizeof dir, _TRUNCATE, "%sw329_acc", tmp);
        memset(&a, 0, sizeof a);
        a.have = 1;
        a.account = 7;
        strcpy_s(a.name, sizeof a.name, "Ace;loc_x=1");
        strcpy_s(a.host, sizeof a.host, "127.0.0.1:1");
        if (!why[0] && (lacc_store_save(dir, &a) != 0 || lacc_store_load(dir, &b) != -1 || b.have))
            strcpy_s(why, sizeof why, "Kontodatei mit unzulaessigem Pilotnamen angenommen");
        lacc_store_forget(dir);
        {
            wchar_t w1[1200];
            u8_to_w(dir, w1, 1200);
            RemoveDirectoryW(w1);
        }
    }
    ST_RESULT("W329 account hardening: JSON reader stops at buffer end, \\u0000 not truncating, pilot name with ';'/control chars rejected from key file");
#undef ST_RESULT
    wipe(K, sizeof K);
    wipe(G, sizeof G);
}
