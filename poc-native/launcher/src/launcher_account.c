/* launcher_account.c - W328 STEELNET-ACCOUNT (s. launcher_account.h): reiner Teil.
 * Vertrag: net/steelnet/PROTOKOLL.md, Abschnitt "W328: Pilotenkonten".
 * W537 LAUNCHER-SLIM: Netz (HTTPS, Challenge-Response, Discord-Loopback), Schreiben der Schluesseldatei und Zufall
 * liegen in launcher_account_net.c (Hilfsprogramm sb_account.exe bzw. Entwickler-/Test-Launcher). Diese Datei braucht
 * kein Netz und keine DPAPI: Krypto (HMAC), Eingabepruefung, Dienst-Konfiguration, Pfad/Loeschen der
 * Schluesseldatei, JSON, Kind-Umgebung. Lesen und Schreiben der Schluesseldatei (DPAPI) liegen seit W537 ebenfalls
 * im Netzteil; der ausgelieferte Launcher bekommt oeffentliche Felder, Wiederherstellungscode und Spielschluessel
 * vom Hilfsprogramm.
 * Quelltext ASCII. Geheimnisse (K, Passwort, OAuth-Code) werden nie
 * ausgegeben oder geloggt; Puffer werden nach Gebrauch geloescht. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "launcher_account.h"
#include "launcher_account_cfg.h"
#include "launcher_app.h"
#include "launcher_u8.h"
#include "sb_sha256.h"
#include "launcher_account_int.h" /* W537: mit launcher_account_net.c geteilt */
#ifndef LACC_HELPER
#include "launcher_games.h"
#include "sb_userdata.h"
#endif

#define printf cli_printf

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

int hexval(char c)
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

/* ====================================================================== Eingaben */
int name_chars(const char *s, size_t lo, size_t hi)
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

/* W487 LIVE-GAMERTAG: Regel wie steelcore - 1-15 Zeichen [A-Za-z0-9 ], erstes Zeichen ein Buchstabe, kein
 * Leerzeichen am Ende, keine zwei Leerzeichen hintereinander. */
int lacc_valid_gamertag(const char *s)
{
    size_t n, i;
    if (!s) return 0;
    n = strlen(s);
    if (n < 1 || n > 15) return 0;
    if (!((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z'))) return 0;
    if (s[n - 1] == ' ') return 0;
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ')) return 0;
        if (c == ' ' && i + 1 < n && s[i + 1] == ' ') return 0;
    }
    return 1;
}

int lacc_gt_status_known(const char *status)
{
    static const char *const k[] = { "ok", "taken", "similar", "reserved", "blocked", "invalid_length",
                                     "invalid_characters", "change_limit", "unchanged", "off", "unavailable" };
    int i;
    if (!status) return 0;
    for (i = 0; i < (int)(sizeof k / sizeof k[0]); i++) {
        if (strcmp(status, k[i]) == 0) return 1;
    }
    return 0;
}

/* ====================================================================== Konfiguration */
int lacc_is_loopback(const char *host)
{
    return host && (strcmp(host, "127.0.0.1") == 0 || _stricmp(host, "localhost") == 0 ||
                    strcmp(host, "::1") == 0);
}

int norm_pin(const char *in, char out[65])
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
    norm_pin(SB_STEELNET_SPKI1, s->pin1); /* W334: SPKI-Pins (steelnet_pins.h) */
    norm_pin(SB_STEELNET_SPKI2, s->pin2);
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
                if (u8_GetEnv("SB_STEELNET_SPKI", pin, sizeof pin) > 0 || u8_GetEnv("SB_STEELNET_PIN", pin, sizeof pin) > 0) {
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

void lacc_store_path(const char *dir, char *out, size_t cap)
{
    size_t n = strlen(dir);
    _snprintf_s(out, cap, _TRUNCATE, "%s%s%s", dir, (n && (dir[n - 1] == '\\' || dir[n - 1] == '/')) ? "" : "\\",
                LACC_FILE);
}

#ifndef LACC_HELPER /* Hilfsprogramm: Ordner kommt vom Launcher (Auftrag dir=) */
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
#endif

int lacc_store_forget(const char *dir)
{
    char path[1200];
    lacc_store_path(dir, path, sizeof path);
    if (u8_GetFileAttributes(path) == INVALID_FILE_ATTRIBUTES) return 0;
    return u8_DeleteFile(path) ? 0 : -1;
}

/* ====================================================================== JSON */
const char *js_find(const char *json, const char *key)
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

size_t lacc_env_len(const wchar_t *env)
{
    const wchar_t *p = env;
    if (!env) return 0;
    while (*p) p += wcslen(p) + 1; /* Eintraege bis zum leeren Eintrag */
    return (size_t)(p - env) + 1;  /* + abschliessende Null (bei leerem Block die einzige) */
}

void lacc_env_wipe(wchar_t *env)
{
    if (env) SecureZeroMemory(env, lacc_env_len(env) * sizeof(wchar_t));
}

void lacc_env_free(wchar_t *env)
{
    lacc_env_wipe(env);
    free(env);
}

