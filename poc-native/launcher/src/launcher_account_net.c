/* launcher_account_net.c - W537 LAUNCHER-SLIM: Netz- und Schreibteil des steelnet-Kontos (s. launcher_account.h).
 *
 * Seit W537 liegt alles, was mit dem Konto-Dienst spricht, in dieser Datei:
 *   - HTTPS zur Konto-API (WinHTTP, SPKI-Pin vor dem Rumpf), GET /v1/info (Kopf-Chip), Lobbyliste,
 *   - alle Aktionen mit Challenge-Response (Nonce + HMAC-Nachweis), Anlegen/Anmelden/Uebertragen/Loeschen,
 *   - Discord-OAuth mit dem lokalen Loopback-Empfaenger (127.0.0.1, nur waehrend des Ablaufs offen),
 *   - Schreiben der DPAPI-Schluesseldatei (lacc_store_save), Zufall (BCryptGenRandom).
 * Gebaut wird sie in das kleine Hilfsprogramm sb_account.exe (LACC_HELPER, s. sb_account_main.c) und - fuer die
 * Konto-Kommandozeile --steelnet und den Selbsttest - in Entwickler- bzw. Test-Launcher. Der ausgelieferte
 * Launcher enthaelt sie nicht: er startet sb_account.exe je Auftrag (launcher_acchelp.c) und liest nur noch die
 * Schluesseldatei (Spielschluessel G fuer den Client, Wiederherstellungscode).
 * Vertrag: net/steelnet/PROTOKOLL.md, Abschnitt "W328: Pilotenkonten". Quelltext ASCII. Geheimnisse (K, Passwort,
 * OAuth-Code) werden nie ausgegeben oder geloggt; Puffer werden nach Gebrauch geloescht. */
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
#include "launcher_u8.h"
#include "sb_sha256.h"
#include "launcher_account_int.h"
#ifndef LACC_HELPER
#include "launcher_acchelp.h" /* W537: Konto-CLI wahlweise ueber sb_account.exe */
#endif
#include "sb_version.h" /* W421: sb_netver_strict (min_client_version aus /v1/info) */
#include "launcher_camp.h" /* W491: --steelnet campaign-probe */

#define printf cli_printf

#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 0x00002000
#endif

static const char kEntropy[] = LACC_DPAPI_ENTROPY;

static void wipe(void *p, size_t n)
{
    SecureZeroMemory(p, n);
}

lacc_url_fn g_lacc_open_url; /* W537: s. launcher_account.h */

/* ====================================================================== Zufall, PKCE */
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

/* ====================================================================== Speicher (DPAPI) lesen + schreiben */
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
    /* 2) Pinning (W334, SECURITY.md §9): SHA-256 ueber das DER-kodierte SubjectPublicKeyInfo, sonst Abbruch
     *    vor dem Rumpf. Ein neues Zertifikat mit demselben Schluessel bricht nichts; Schluesselwechsel ueber SPKI2. */
    n = sizeof cert;
    if (!WinHttpQueryOption(req, WINHTTP_OPTION_SERVER_CERT_CONTEXT, &cert, &n) || !cert) {
        r->pin_fail = 1;
        strcpy_s(r->error, sizeof r->error, "pin");
        goto done;
    }
    {
        BYTE *der = NULL;
        DWORD dl = 0;
        int enc = CryptEncodeObjectEx(X509_ASN_ENCODING, X509_PUBLIC_KEY_INFO, &cert->pCertInfo->SubjectPublicKeyInfo,
                                      CRYPT_ENCODE_ALLOC_FLAG, NULL, &der, &dl) && der;
        CertFreeCertificateContext(cert);
        if (!enc) {
            r->pin_fail = 1;
            strcpy_s(r->error, sizeof r->error, "pin");
            goto done;
        }
        sb_sha256(der, dl, sha);
        LocalFree(der);
    }
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

/* Live-Zahlen aus GET /v1/info: "online":{"players":n,"lobbies":n,"games":n,...}. Gesucht wird nur innerhalb
 * dieses Objekts (gleichnamige Schluessel anderswo zaehlen nicht); alle drei Werte muessen da und plausibel
 * (0..1000000) sein, sonst has_counts 0 (aelterer Server oder kaputte Antwort -> Anzeige wie vorher). */
static void probe_counts(const char *body, lacc_probe_t *o)
{
    char obj[512];
    const char *p = js_find(body, "online"), *q;
    long long v[3];
    static const char *const kKeys[3] = { "players", "lobbies", "games" };
    int i, depth = 0, instr = 0;
    o->has_counts = 0;
    o->players = o->lobbies = o->games = 0;
    if (!p || *p != '{') return;
    for (q = p; *q; q++) { /* passende } (Zeichenketten uebersprungen) */
        if (instr) {
            if (*q == '\\' && q[1]) q++;
            else if (*q == '"') instr = 0;
        } else if (*q == '"') {
            instr = 1;
        } else if (*q == '{') {
            depth++;
        } else if (*q == '}' && --depth == 0) {
            break;
        }
    }
    if (*q != '}' || (size_t)(q - p) + 2 > sizeof obj) return;
    memcpy(obj, p, (size_t)(q - p) + 1);
    obj[q - p + 1] = '\0';
    for (i = 0; i < 3; i++) {
        if (!lacc_js_int(obj, kKeys[i], &v[i]) || v[i] < 0 || v[i] > 1000000) return;
    }
    o->players = (int)v[0];
    o->lobbies = (int)v[1];
    o->games = (int)v[2];
    o->has_counts = 1;
}

/* W421 LOC-VERSION-GATE: "min_client_version" aus GET /v1/info, nur strikt X.Y.Z (je 1-4 Ziffern, <= 14 Zeichen).
 * lacc_js_str kuerzt still auf den Puffer - 64 Byte reichen, damit eine zu lange Angabe an der Laengengrenze
 * scheitert statt gekuerzt gueltig zu werden. Fehlt/leer/kaputt -> "" (keine Sperre). */
static void probe_minver(const char *body, lacc_probe_t *o)
{
    char v[64];
    o->min_ver[0] = '\0';
    if (lacc_js_str(body, "min_client_version", v, sizeof v) && sb_netver_strict(v)) {
        strcpy_s(o->min_ver, sizeof o->min_ver, v);
    }
}

/* W470: optionale Zahl aus dem Objekt "online" (-1 = fehlt/unplausibel). */
static int probe_opt(const char *body, const char *key)
{
    const char *p = js_find(body, "online");
    char obj[512], pat[40];
    const char *q;
    long long v;
    if (!p || *p != '{') return -1;
    q = strchr(p, '}'); /* "online" enthaelt nur Zahlen (keine Zeichenketten, keine Unterobjekte) */
    if (!q || (size_t)(q - p) + 2 > sizeof obj) return -1;
    memcpy(obj, p, (size_t)(q - p) + 1);
    obj[q - p + 1] = '\0';
    _snprintf_s(pat, sizeof pat, _TRUNCATE, "%s", key);
    if (!lacc_js_int(obj, pat, &v) || v < 0 || v > 1000000) return -1;
    return (int)v;
}

/* W470 LAUNCHER-HUB: Antwort der Aktion lobbies lesen (W472: angemeldet). Ungueltige Eintraege fallen weg (Zahlen ausserhalb 0..64,
 * mehr Spieler als Plaetze, fehlender Host); Namen nur druckbares ASCII. 0 = Liste gelesen (auch leer).
 * W485 LIVE-QOS: Gruppe "live" (Xbox-Live-Sessions, W482) getrennt nach lv/nlive/live_more; fehlt sie (aelterer
 * Server), bleibt sie leer. p zeigt auf '['; Rueckgabe: Zeiger hinter ']' bzw. NULL (kaputt). */
static const char *lob_array(const char *p, lacc_lobby_t *arr, int cap, int *n, int *more)
{
    const char *q;
    *n = 0;
    *more = 0;
    if (!p || *p != '[') return NULL;
    p++;
    for (;;) {
        char obj[600], host[64], mode[16], st[16], kind[16];
        long long pl = -1, mx = -1;
        int depth = 0, instr = 0;
        size_t k;
        while (*p == ' ' || *p == ',' || *p == '\r' || *p == '\n' || *p == '\t') p++;
        if (*p == ']') return p + 1;
        if (*p != '{') return NULL;
        for (q = p; *q; q++) { /* passende } (Zeichenketten uebersprungen) */
            if (instr) {
                if (*q == '\\' && q[1]) q++;
                else if (*q == '"') instr = 0;
            } else if (*q == '"') {
                instr = 1;
            } else if (*q == '{') {
                depth++;
            } else if (*q == '}' && --depth == 0) {
                break;
            }
        }
        if (*q != '}') return NULL;
        if ((size_t)(q - p) + 2 <= sizeof obj && *n < cap) {
            memcpy(obj, p, (size_t)(q - p) + 1);
            obj[q - p + 1] = '\0';
            host[0] = mode[0] = st[0] = kind[0] = '\0';
            lacc_js_str(obj, "host", host, sizeof host);
            lacc_js_str(obj, "play_mode", mode, sizeof mode);
            lacc_js_str(obj, "state", st, sizeof st);
            lacc_js_str(obj, "kind", kind, sizeof kind);
            lacc_js_int(obj, "players", &pl);
            lacc_js_int(obj, "max_players", &mx);
            if (host[0] && pl >= 0 && pl <= 64 && mx >= 1 && mx <= 64 && pl <= mx) {
                lacc_lobby_t *e = &arr[(*n)++];
                for (k = 0; host[k] && k + 1 < sizeof e->host; k++) {
                    unsigned char c = (unsigned char)host[k];
                    e->host[k] = (char)(c >= 0x20 && c < 0x7F ? c : '?');
                }
                e->host[k] = '\0';
                e->arcade = strcmp(mode, "arcade") == 0;
                e->started = strcmp(st, "open") != 0;
                e->chat = strcmp(kind, "chat") == 0;
                e->players = (int)pl;
                e->max_players = (int)mx;
            } else if (*n >= cap) {
                (*more)++;
            }
        } else {
            (*more)++;
        }
        p = q + 1;
    }
}

int lacc_lobbies_parse(const char *body, lacc_lobbies_t *o)
{
    const char *p = body ? js_find(body, "lobbies") : NULL, *e;
    long long v;
    int more = 0;
    o->n = 0;
    o->more = 0;
    o->nlive = 0;
    o->live_more = 0;
    e = lob_array(p, o->l, LACC_LOB_MAX, &o->n, &more);
    if (!e) {
        o->n = 0;
        return 1;
    }
    o->more = more;
    if (lacc_js_int(e, "more", &v) && v > 0 && v < 100000) {
        o->more += (int)v;
    }
    /* W485: Gruppe "live" = Schluessel "live" mit Liste (die Eintraege selbst tragen "live":true) */
    for (p = e; (p = js_find(p, "live")) != NULL && *p != '[';) {
    }
    if (p) {
        const char *le = lob_array(p, o->lv, LACC_LIVE_MAX, &o->nlive, &more);
        if (!le) {
            o->nlive = 0; /* kaputte Live-Gruppe: System-Link-Liste bleibt gueltig */
        } else {
            o->live_more = more;
            if (lacc_js_int(le, "live_more", &v) && v > 0 && v < 100000) o->live_more += (int)v;
        }
    }
    return 0;
}

/* ====================================================================== Aktionen */
int lacc_probe(lacc_probe_t *o)
{
    lacc_srv_t s;
    lacc_resp_t r;
    ULONGLONG t0;
    memset(o, 0, sizeof *o);
    if (!lacc_server(&s)) {
        strcpy_s(o->error, sizeof o->error, "off");
        return 1;
    }
    t0 = GetTickCount64();
    if (lacc_http(&s, "GET", "/v1/info", NULL, &r) == 0 && r.ok) {
        o->ok = 1;
        lacc_js_str(r.body, "version", o->version, sizeof o->version);
        probe_counts(r.body, o);
        probe_minver(r.body, o); /* W421 */
        o->games_today = probe_opt(r.body, "games_today"); /* W470 */
        o->peak_7d = probe_opt(r.body, "peak_7d");
    } else {
        strcpy_s(o->error, sizeof o->error, r.error[0] ? r.error : "net");
    }
    o->ms = (int)(GetTickCount64() - t0);
    SecureZeroMemory(&r, sizeof r);
    return o->ok ? 0 : 1;
}

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

/* W472 LOBBIES-AUTH: Lobbyliste nur mit eigenem Konto (angemeldete Aktion "lobbies", Challenge + Nachweis wie
 * status). Ohne Konto kein Netz ("no_account"). Fehler: API-Code (auth_failed, rate_limited + retry_after,
 * not_found = Server ohne Aktion) oder net/pin/off/bad. Kein Log, keine Adresse, Antwortpuffer wird geloescht. */
int lacc_lobbies(const lacc_store_t *st, lacc_lobbies_t *o)
{
    lacc_srv_t s;
    lacc_resp_t r;
    char err[48] = "";
    ULONGLONG t0;
    memset(o, 0, sizeof *o);
    memset(&r, 0, sizeof r);
    if (!lacc_server(&s)) {
        strcpy_s(o->error, sizeof o->error, "off");
        return 1;
    }
    if (!st || !st->have) {
        strcpy_s(o->error, sizeof o->error, "no_account");
        return 1;
    }
    t0 = GetTickCount64();
    if (post_auth(&s, st, "lobbies", "", &r, err, sizeof err) == 0 && lacc_lobbies_parse(r.body, o) == 0) {
        o->ok = 1;
    } else {
        o->n = o->more = 0;
        strcpy_s(o->error, sizeof o->error, err[0] ? err : "bad");
    }
    o->http = r.http;
    if (!o->ok && r.retry_after > 0) {
        o->retry_after = r.retry_after > 3600 ? 3600 : (int)r.retry_after;
    }
    o->ms = (int)(GetTickCount64() - t0);
    SecureZeroMemory(&r, sizeof r);
    return o->ok ? 0 : 1;
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

/* W487: Gamertag aus der Antwort nur, wenn er die Regel erfuellt (Anzeige/Vergleich), sonst "" */
static void gt_take(const char *body, const char *key, char *out, size_t cap)
{
    char t[64];
    out[0] = '\0';
    if (lacc_js_str(body, key, t, sizeof t) && lacc_valid_gamertag(t)) strcpy_s(out, cap, t);
}

/* W487: Statuscode (nur [a-z_], hoechstens 23 Zeichen), sonst "" */
static void gt_code(const char *body, const char *key, char *out, size_t cap)
{
    char t[64];
    const char *q;
    out[0] = '\0';
    if (!lacc_js_str(body, key, t, sizeof t) || !t[0] || strlen(t) >= cap) return;
    for (q = t; *q; q++) {
        if (!((*q >= 'a' && *q <= 'z') || *q == '_')) return;
    }
    strcpy_s(out, cap, t);
}

void lacc_create_body(const char *name, const char *gamertag, char *out, size_t cap)
{
    char e[64], g[64];
    lacc_js_esc(name ? name : "", e, sizeof e);
    if (gamertag && gamertag[0]) {
        lacc_js_esc(gamertag, g, sizeof g);
        _snprintf_s(out, cap, _TRUNCATE, "{\"name\":\"%s\",\"gamertag\":\"%s\"}", e, g);
    } else {
        _snprintf_s(out, cap, _TRUNCATE, "{\"name\":\"%s\"}", e);
    }
}

void lacc_gtset_parse(const char *body, lacc_gtset_t *o)
{
    long long v = 0;
    memset(o, 0, sizeof *o);
    if (!body) return;
    gt_take(body, "gamertag", o->gamertag, sizeof o->gamertag);
    if (lacc_js_int(body, "next_change_at", &v) && v > 0) o->next_at = v;
    gt_code(body, "status", o->status, sizeof o->status);
    if (!o->status[0]) gt_code(body, "gamertag_error", o->status, sizeof o->status);
}

int lacc_op_create_gt(const lacc_srv_t *s, const char *dir, const char *name, const char *gamertag, lacc_store_t *st,
                      lacc_gtset_t *gt, char *err, size_t ecap)
{
    lacc_resp_t r;
    lacc_gtset_t g;
    char body[192];
    int rc;
    memset(&g, 0, sizeof g);
    if (gt) memset(gt, 0, sizeof *gt);
    if (!lacc_valid_name(name)) return fail(err, ecap, "invalid_name");
    if (gamertag && gamertag[0] && !lacc_valid_gamertag(gamertag)) return fail(err, ecap, "invalid_gamertag");
    lacc_create_body(name, gamertag, body, sizeof body);
    if (post(s, "create", body, &r, err, ecap) != 0) {
        if (strcmp(err, "gamertag_refused") == 0 && gt) lacc_gtset_parse(r.body, gt);
        return 1;
    }
    lacc_gtset_parse(r.body, &g); /* vor take_key (loescht den Antwortpuffer) */
    rc = take_key(s, dir, &r, st, err, ecap);
    if (rc == 0 && gt) *gt = g;
    return rc;
}

int lacc_op_create(const lacc_srv_t *s, const char *dir, const char *name, lacc_store_t *st,
                   char *err, size_t ecap)
{
    return lacc_op_create_gt(s, dir, name, NULL, st, NULL, err, ecap);
}

void lacc_status_parse(const char *body, lacc_status_t *o)
{
    long long v = 0;
    memset(o, 0, sizeof *o);
    o->gt_days = 7;
    if (!body) return;
    lacc_js_bool(body, "discord", &o->discord);
    lacc_js_bool(body, "login", &o->login);
    lacc_js_str(body, "login_name", o->login_name, sizeof o->login_name);
    if (lacc_js_int(body, "created", &v)) o->created = v;
    if (lacc_js_int(body, "gen", &v)) o->gen = (int)v;
    gt_take(body, "gamertag", o->gamertag, sizeof o->gamertag);
    if (lacc_js_int(body, "gamertag_next_change", &v) && v > 0) o->gt_next = v;
    if (lacc_js_int(body, "gamertag_change_days", &v) && v > 0 && v <= 365) o->gt_days = (int)v;
    gt_code(body, "gamertag_state", o->gt_state, sizeof o->gt_state);
}

int lacc_op_status(const lacc_srv_t *s, const lacc_store_t *st, lacc_status_t *o, char *err, size_t ecap)
{
    lacc_resp_t r;
    memset(o, 0, sizeof *o);
    if (post_auth(s, st, "status", "", &r, err, ecap) != 0) return 1;
    lacc_status_parse(r.body, o);
    return 0;
}

void lacc_gtcheck_body(const char *name, const char *suggest, char *out, size_t cap)
{
    char e1[64], e2[64];
    int hn = name && name[0], hs = suggest && suggest[0];
    lacc_js_esc(hn ? name : "", e1, sizeof e1);
    lacc_js_esc(hs ? suggest : "", e2, sizeof e2);
    if (hn && hs) _snprintf_s(out, cap, _TRUNCATE, "{\"gamertag\":\"%s\",\"suggest\":\"%s\"}", e1, e2);
    else if (hn) _snprintf_s(out, cap, _TRUNCATE, "{\"gamertag\":\"%s\"}", e1);
    else _snprintf_s(out, cap, _TRUNCATE, "{\"suggest\":\"%s\"}", e2);
}

int lacc_gtcheck_parse(const char *body, lacc_gtcheck_t *o)
{
    char t[64];
    memset(o, 0, sizeof *o);
    if (!body) return 1;
    if (lacc_js_str(body, "gamertag", t, sizeof t)) {
        o->has_name = 1;
        if (lacc_valid_gamertag(t)) strcpy_s(o->gamertag, sizeof o->gamertag, t);
        if (!lacc_js_bool(body, "valid", &o->valid) || !lacc_js_bool(body, "available", &o->available)) return 1;
        gt_code(body, "status", o->status, sizeof o->status);
        if (!o->status[0]) return 1;
        if (o->available && (!o->valid || !o->gamertag[0])) o->available = 0; /* nie "frei" ohne gueltigen Namen */
    }
    gt_take(body, "suggestion", o->suggestion, sizeof o->suggestion);
    return 0;
}

int lacc_op_gamertag_check(const lacc_srv_t *s, const char *name, const char *suggest, lacc_gtcheck_t *o,
                           char *err, size_t ecap)
{
    lacc_resp_t r;
    char body[192];
    memset(o, 0, sizeof *o);
    if ((!name || !name[0]) && (!suggest || !suggest[0])) return fail(err, ecap, "bad_request");
    if (name && strlen(name) > 40) return fail(err, ecap, "invalid_gamertag");
    if (suggest && strlen(suggest) > 40) return fail(err, ecap, "bad_request");
    lacc_gtcheck_body(name, suggest, body, sizeof body);
    if (post(s, "gamertag_check", body, &r, err, ecap) != 0) return 1;
    if (lacc_gtcheck_parse(r.body, o) != 0) return fail(err, ecap, "bad");
    return 0;
}

int lacc_op_gamertag_set(const lacc_srv_t *s, const lacc_store_t *st, const char *name, lacc_gtset_t *o,
                         char *err, size_t ecap)
{
    lacc_resp_t r;
    char e[64], extra[128];
    lacc_gtset_t g;
    if (o) memset(o, 0, sizeof *o);
    if (!lacc_valid_gamertag(name)) return fail(err, ecap, "invalid_gamertag");
    lacc_js_esc(name, e, sizeof e);
    _snprintf_s(extra, sizeof extra, _TRUNCATE, ",\"gamertag\":\"%s\"", e);
    if (post_auth(s, st, "gamertag_set", extra, &r, err, ecap) != 0) {
        if (strcmp(err, "gamertag_refused") == 0 && o) lacc_gtset_parse(r.body, o);
        return 1;
    }
    lacc_gtset_parse(r.body, &g);
    if (!g.gamertag[0]) return fail(err, ecap, "bad");
    if (o) *o = g;
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

/* W534 AVPRE: Test-Ueberschreibungen des Discord-Ablaufs (SB_DISCORD_AUTHORIZE, SB_TEST_NO_BROWSER) nur in
 * Entwickler- und Test-Builds (SB_TEST_HOOKS ohne SB_VERSION, Test-Launcher SB_LAUNCHER_SELFTEST); genutzt von
 * e2e_accounts.py ueber --steelnet (seit W532 ebenfalls nur dort). Im Release-Launcher gilt immer discord.com. */
#if (defined(SB_TEST_HOOKS) && SB_TEST_HOOKS) || (defined(SB_LAUNCHER_SELFTEST) && SB_LAUNCHER_SELFTEST)
#define LACC_TEST_ENV 1
#else
#define LACC_TEST_ENV 0
#endif

/* Erlaubte Autorisierungs-Basis: fest (discord.com) oder Test-Ueberschreibung nur Loopback */
static void authorize_base(char *out, size_t cap)
{
#if LACC_TEST_ENV
    char v[256];
#endif
    strcpy_s(out, cap, SB_DISCORD_AUTHORIZE_URL);
#if LACC_TEST_ENV
    if (u8_GetEnv("SB_DISCORD_AUTHORIZE", v, sizeof v) > 0 &&
        (strncmp(v, "http://127.0.0.1:", 17) == 0 || strncmp(v, "http://localhost:", 17) == 0) &&
        !strpbrk(v, "?#\"' ")) {
        strcpy_s(out, cap, v);
    }
#endif
}

#if LACC_TEST_ENV
static int test_no_browser(void)
{
    char v[8];
    return u8_GetEnv("SB_TEST_NO_BROWSER", v, sizeof v) > 0 && v[0] == '1';
}
#endif

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
    if (g_lacc_open_url) { /* W537: Hilfsprogramm - der Launcher oeffnet den Browser (Vordergrund wie bisher) */
        if (g_lacc_open_url(url) != 0) {
            lacc_loop_close(sock);
            return fail(err, ecap, "browser");
        }
    } else
#ifdef LACC_HELPER
    {
        lacc_loop_close(sock); /* Hilfsprogramm oeffnet nie selbst einen Browser */
        return fail(err, ecap, "browser");
    }
#else
#if LACC_TEST_ENV
    if (test_no_browser()) {
        printf("[discord-url] %s\n", url);
    } else
#endif
    {
        wchar_t wu[1400];
        u8_to_w(url, wu, 1400);
        if ((INT_PTR)ShellExecuteW(NULL, L"open", wu, NULL, NULL, SW_SHOWNORMAL) <= 32) {
            lacc_loop_close(sock);
            return fail(err, ecap, "browser");
        }
    }
#endif /* !LACC_HELPER */
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

/* W332: Auswertung einer empfangenen Anfrage (ohne Socket; isoliert fuzzbar, tests/fuzz/proj_launcher).
 * req = NUL-terminiert (got Bytes). path = Arbeitspuffer (>= 2048). Ergebnis:
 * LOOP_P_404 = nicht "GET /discord[?...]" (z. B. favicon), LOOP_P_FOREIGN = falscher/fehlender state (fremde
 * Anfrage, weiter warten), LOOP_P_DENIED = error=... mit richtigem state, LOOP_P_CODE = richtiger state, code
 * gesetzt (leer = ungueltig). */
enum { LOOP_P_404 = 0, LOOP_P_FOREIGN, LOOP_P_DENIED, LOOP_P_CODE };
static int loop_parse(const char *req, int got, const char *state, char *path, size_t pcap, char *code, size_t ccap)
{
    char v[600];
    const char *q;
    code[0] = '\0';
    path[0] = '\0';
    if (got < 5 || pcap < 2048 || strncmp(req, "GET ", 4) != 0 ||
        sscanf_s(req + 4, "%2047s", path, (unsigned)pcap) != 1 || strncmp(path, "/discord", 8) != 0 ||
        (path[8] != '?' && path[8] != '\0')) {
        return LOOP_P_404;
    }
    q = path[8] == '?' ? path + 9 : "";
    /* W329: erst state pruefen. Falscher/fehlender state = fremde Anfrage (z. B. eine Webseite, die
     * http://127.0.0.1:47090/discord?... aufruft): ablehnen und WEITER warten - sie darf die echte
     * Anmeldung weder ersetzen noch abbrechen. Auch "error" zaehlt nur mit richtigem state. */
    q_get(q, "state", v, sizeof v);
    if (!state || !state[0] || strlen(v) != strlen(state) || strcmp(v, state) != 0) {
        wipe(v, sizeof v);
        return LOOP_P_FOREIGN;
    }
    q_get(q, "error", v, sizeof v);
    if (v[0]) {
        return LOOP_P_DENIED;
    }
    q_get(q, "code", code, ccap);
    return LOOP_P_CODE;
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
        char req[4096], path[2048];
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
        n = loop_parse(req, got, state, path, sizeof path, code, ccap);
        if (n == LOOP_P_404) {
            static const char k404[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            send(c, k404, (int)sizeof k404 - 1, 0);
            closesocket(c);
            continue; /* z. B. favicon: weiter warten */
        }
        if (n == LOOP_P_FOREIGN) {
            loop_reply(c, 0);
            closesocket(c);
            wipe(req, sizeof req);
            wipe(path, sizeof path);
            continue;
        }
        if (n == LOOP_P_DENIED) {
            loop_reply(c, 0);
            closesocket(c);
            return fail(err, ecap, "denied");
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

#ifndef LACC_HELPER /* CLI + Selbsttest nur im Launcher (Entwickler-/Test-Build), nie im Hilfsprogramm */
/* ====================================================================== CLI */
static int cli_out_store(const char *tag, const lacc_store_t *st)
{
    printf("ok %saccount=%lu name=%s gen=%d\n", tag, (unsigned long)st->account, st->name, st->gen);
    return 0;
}

/* W334 (L6): Passwort aus stdin (eine Zeile, ohne Zeilenende). 1 = gelesen und nicht leer. */
static int cli_read_password(char *pw, size_t cap)
{
    size_t l;
    if (!fgets(pw, (int)cap, stdin)) {
        pw[0] = '\0';
        return 0;
    }
    l = strlen(pw);
    while (l > 0 && (pw[l - 1] == '\n' || pw[l - 1] == '\r')) pw[--l] = '\0';
    return l > 0;
}

/* W537 LAUNCHER-SLIM: SB_ACC_VIA_HELPER=1 -> die Aufrufe der Konto-CLI gehen wie im ausgelieferten Launcher ueber
 * sb_account.exe (lach_run, gleicher Weg wie die Oberflaeche). Fuer steelnet/tests/e2e_accounts.py --via-helper:
 * dieselbe Ausgabe, damit derselbe Ablauf beide Wege prueft. Signaturen wie die lacc_*-Funktionen. */
static int g_cli_via;
static char g_cli_dir[1200];

static int via(const char *op, const char *a, const char *b, const char *c, lacc_store_t *st, lach_res_t *res,
               int timeout_ms, char *err, size_t ecap)
{
    int rc = lach_run(op, g_cli_dir, a, b, c, 0, timeout_ms, NULL, res, err, ecap);
    if (st) {
        lacc_store_t ns;
        if (lacc_store_load(g_cli_dir, &ns) >= 0) *st = ns;
        wipe(&ns, sizeof ns);
    }
    return rc;
}

static int via_probe(lacc_probe_t *o)
{
    lach_res_t r;
    char e[48];
    int rc = lach_run("info", NULL, NULL, NULL, NULL, 0, 30000, NULL, &r, e, sizeof e);
    *o = r.probe;
    if (rc && !o->error[0]) strcpy_s(o->error, sizeof o->error, e);
    return rc;
}

static int via_lobbies(const lacc_store_t *st, lacc_lobbies_t *o)
{
    lach_res_t r;
    char e[48];
    int rc;
    memset(o, 0, sizeof *o);
    if (!st || !st->have) {
        strcpy_s(o->error, sizeof o->error, "no_account");
        return 1;
    }
    rc = via("lobbies", NULL, NULL, NULL, NULL, &r, 30000, e, sizeof e);
    *o = r.lob;
    if (rc && !o->error[0]) strcpy_s(o->error, sizeof o->error, e);
    return rc;
}

static int via_check(const lacc_srv_t *s, const char *name, int *av, char *err, size_t ecap)
{
    lach_res_t r;
    int rc = via("check", name, NULL, NULL, NULL, &r, 60000, err, ecap);
    (void)s;
    *av = r.avail;
    return rc;
}

static int via_create(const lacc_srv_t *s, const char *dir, const char *name, const char *gt, lacc_store_t *st,
                      lacc_gtset_t *g, char *err, size_t ecap)
{
    lach_res_t r;
    int rc = via("create", name, NULL, gt, st, &r, 60000, err, ecap);
    (void)s;
    (void)dir;
    if (g) *g = r.gts;
    return rc;
}

static int via_gtcheck(const lacc_srv_t *s, const char *name, const char *sg, lacc_gtcheck_t *o, char *err,
                       size_t ecap)
{
    lach_res_t r;
    int rc = via("gtcheck", name, NULL, sg, NULL, &r, 60000, err, ecap);
    (void)s;
    *o = r.gtc;
    return rc;
}

static int via_gtset(const lacc_srv_t *s, const lacc_store_t *st, const char *name, lacc_gtset_t *g, char *err,
                     size_t ecap)
{
    lach_res_t r;
    int rc = via("gtset", name, NULL, NULL, NULL, &r, 60000, err, ecap);
    (void)s;
    (void)st;
    if (g) *g = r.gts;
    return rc;
}

static int via_status(const lacc_srv_t *s, const lacc_store_t *st, lacc_status_t *o, char *err, size_t ecap)
{
    lach_res_t r;
    int rc = via("status", NULL, NULL, NULL, NULL, &r, 60000, err, ecap);
    (void)s;
    (void)st;
    *o = r.status;
    return rc;
}

static int via_discord(const lacc_srv_t *s, const char *dir, int lm, lacc_store_t *st, int timeout_ms,
                       volatile long *cancel, char *err, size_t ecap)
{
    lach_res_t r;
    (void)s;
    (void)dir;
    (void)cancel;
    return via(lm ? "discord-login" : "discord-link", NULL, NULL, NULL, st, &r, timeout_ms + 5000, err, ecap);
}

static int via_login_set(const lacc_srv_t *s, const lacc_store_t *st, const char *login, const char *pw, char *err,
                         size_t ecap)
{
    lach_res_t r;
    (void)s;
    (void)st;
    return via("login-set", login, pw, NULL, NULL, &r, 60000, err, ecap);
}

static int via_login(const lacc_srv_t *s, const char *dir, const char *login, const char *pw, lacc_store_t *st,
                     char *err, size_t ecap)
{
    lach_res_t r;
    (void)s;
    (void)dir;
    return via("login", login, pw, NULL, st, &r, 60000, err, ecap);
}

static int via_code(const char *op, const char *code, lacc_store_t *st, char *err, size_t ecap)
{
    lach_res_t r;
    return via(op, code, NULL, NULL, st, &r, 60000, err, ecap);
}

static int via_transfer(const lacc_srv_t *s, const char *dir, const char *code, lacc_store_t *st, char *err, size_t ecap)
{
    (void)s;
    (void)dir;
    return via_code("transfer", code, st, err, ecap);
}

static int via_recovery(const lacc_srv_t *s, const char *dir, const char *code, lacc_store_t *st, char *err, size_t ecap)
{
    (void)s;
    (void)dir;
    return via_code("recovery", code, st, err, ecap);
}

static int via_delete(const lacc_srv_t *s, const char *dir, lacc_store_t *st, char *err, size_t ecap)
{
    (void)s;
    (void)dir;
    return via_code("delete", NULL, st, err, ecap);
}

/* Innerhalb von lacc_cli: Aufrufe umlenken (Makro-Name im eigenen Ersatz wird nicht erneut ersetzt) */
#define lacc_probe(o) (g_cli_via ? via_probe(o) : lacc_probe(o))
#define lacc_lobbies(st, o) (g_cli_via ? via_lobbies(st, o) : lacc_lobbies(st, o))
#define lacc_op_check(...) (g_cli_via ? via_check(__VA_ARGS__) : lacc_op_check(__VA_ARGS__))
#define lacc_op_create_gt(...) (g_cli_via ? via_create(__VA_ARGS__) : lacc_op_create_gt(__VA_ARGS__))
#define lacc_op_gamertag_check(...) (g_cli_via ? via_gtcheck(__VA_ARGS__) : lacc_op_gamertag_check(__VA_ARGS__))
#define lacc_op_gamertag_set(...) (g_cli_via ? via_gtset(__VA_ARGS__) : lacc_op_gamertag_set(__VA_ARGS__))
#define lacc_op_status(...) (g_cli_via ? via_status(__VA_ARGS__) : lacc_op_status(__VA_ARGS__))
#define lacc_op_discord(...) (g_cli_via ? via_discord(__VA_ARGS__) : lacc_op_discord(__VA_ARGS__))
#define lacc_op_login_set(...) (g_cli_via ? via_login_set(__VA_ARGS__) : lacc_op_login_set(__VA_ARGS__))
#define lacc_op_login(...) (g_cli_via ? via_login(__VA_ARGS__) : lacc_op_login(__VA_ARGS__))
#define lacc_op_transfer(...) (g_cli_via ? via_transfer(__VA_ARGS__) : lacc_op_transfer(__VA_ARGS__))
#define lacc_op_recovery(...) (g_cli_via ? via_recovery(__VA_ARGS__) : lacc_op_recovery(__VA_ARGS__))
#define lacc_op_delete(...) (g_cli_via ? via_delete(__VA_ARGS__) : lacc_op_delete(__VA_ARGS__))

int lacc_cli(const char *client_dir, int argc, char **argv)
{
    lacc_srv_t s;
    lacc_store_t st;
    char dir[1200], err[64] = "";
    const char *act = argc > 0 ? argv[0] : "";
    const char *a1 = argc > 1 ? argv[1] : "", *a2 = argc > 2 ? argv[2] : "";
    int rc = 1, ld;
    if (strcmp(act, "campaign-probe") == 0) { /* W491: Kampagnenzugang wie der Launcher (TLS + Pin, nur Handshake;
                                                * ohne Kontodatei, keine Adresse in der Ausgabe) */
        char ce[16];
        int test = strcmp(a1, "test") == 0;
        rc = lcamp_probe_tls(test, 5000, ce, sizeof ce); /* W534: TLS + Pin nur hier (Entwickler-CLI) */
        if (rc == 0) printf("ok campaign=%s\n", test ? "test" : "season");
        else printf("error %s\n", ce[0] ? ce : "net");
        return rc == 0 ? 0 : 1;
    }
    lacc_server(&s);
    lacc_default_dir(client_dir, dir, sizeof dir);
    ld = lacc_store_load(dir, &st);
    {
        char v[8];
        g_cli_via = u8_GetEnv("SB_ACC_VIA_HELPER", v, sizeof v) > 0 && v[0] == '1'; /* W537 */
        strcpy_s(g_cli_dir, sizeof g_cli_dir, dir);
    }
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
#ifdef SB_TEST_HOOKS
    /* W334 (SECURITY.md §9, L6): gibt den Spielschluessel G aus - nur in Testbauten, nie im Release */
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
#endif
    if (!s.pin1[0] && !s.pin2[0]) {
        printf("error off\n");
        return 1;
    }
    if (strcmp(act, "probe") == 0) { /* W403: dieselbe Pruefung wie der Kopf-Chip (eine Anfrage, keine Adresse) */
        lacc_probe_t pr;
        rc = lacc_probe(&pr);
        if (rc == 0) {
            printf("ok ms=%d version=%s", pr.ms, pr.version[0] ? pr.version : "-");
            if (pr.has_counts) { /* Live-Zahlen (nur neuere Server) */
                printf(" players=%d lobbies=%d games=%d", pr.players, pr.lobbies, pr.games);
            }
            if (pr.min_ver[0]) { /* W421: Mindestversion (nur neuere Server mit gesetzter Mindestversion) */
                printf(" min_client_version=%s", pr.min_ver);
            }
            printf("\n");
        } else {
            printf("error %s ms=%d\n", pr.error, pr.ms);
        }
        return rc;
    }
    if (strcmp(act, "lobbies") == 0) { /* W470/W472: dieselbe Abfrage wie steelnet > Info (angemeldet, keine Adresse) */
        lacc_lobbies_t lb;
        int i, sim = 0, arc = 0, run = 0;
        rc = lacc_lobbies(ld == 1 ? &st : NULL, &lb); /* W472: nur mit eigenem Konto */
        if (rc == 0) {
            for (i = 0; i < lb.n; i++) {
                run += lb.l[i].started;
                if (lb.l[i].arcade) arc++;
                else sim++;
            }
            printf("ok ms=%d lobbies=%d more=%d sim=%d arcade=%d started=%d\n", lb.ms, lb.n, lb.more, sim, arc, run);
            for (i = 0; i < lb.n; i++) {
                printf("lobby host=%s mode=%s players=%d/%d state=%s\n", lb.l[i].host, lb.l[i].arcade ? "arcade" : "sim",
                       lb.l[i].players, lb.l[i].max_players, lb.l[i].started ? "started" : "open");
            }
        } else {
            printf("error %s http=%d ms=%d", lb.error, lb.http, lb.ms);
            if (lb.retry_after > 0) printf(" retry_after=%d", lb.retry_after);
            printf("\n");
        }
        return rc;
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
    } else if (strcmp(act, "create") == 0) { /* W487: create NAME [GAMERTAG] */
        lacc_gtset_t g;
        rc = lacc_op_create_gt(&s, dir, a1, a2, &st, &g, err, sizeof err);
        if (rc == 0) {
            printf("ok account=%lu name=%s gen=%d gamertag=%s%s%s\n", (unsigned long)st.account, st.name, st.gen,
                   g.gamertag, g.status[0] ? " gamertag_error=" : "", g.status);
        } else if (strcmp(err, "gamertag_refused") == 0) {
            printf("error gamertag_refused status=%s\n", g.status[0] ? g.status : "-");
            wipe(&st, sizeof st);
            return 1;
        }
    } else if (strcmp(act, "gamertag-check") == 0) { /* W487: gamertag-check NAME | gamertag-check --suggest BASE */
        lacc_gtcheck_t o;
        int sg = strcmp(a1, "--suggest") == 0;
        rc = lacc_op_gamertag_check(&s, sg ? NULL : a1, sg ? a2 : NULL, &o, err, sizeof err);
        if (rc == 0) {
            if (o.has_name) {
                printf("ok gamertag=%s valid=%d available=%d status=%s suggestion=%s\n", o.gamertag, o.valid,
                       o.available, o.status, o.suggestion);
            } else {
                printf("ok suggestion=%s\n", o.suggestion);
            }
        }
    } else if (strcmp(act, "gamertag-set") == 0) { /* W487: gamertag-set NAME (angemeldet) */
        lacc_gtset_t g;
        memset(&g, 0, sizeof g);
        rc = ld == 1 ? lacc_op_gamertag_set(&s, &st, a1, &g, err, sizeof err) : fail(err, sizeof err, "no_account");
        if (rc == 0) {
            printf("ok gamertag=%s next_change_at=%lld\n", g.gamertag, g.next_at);
        } else if (strcmp(err, "gamertag_refused") == 0) {
            printf("error gamertag_refused status=%s next_change_at=%lld\n", g.status[0] ? g.status : "-", g.next_at);
            wipe(&st, sizeof st);
            return 1;
        }
    } else if (strcmp(act, "status") == 0) {
        lacc_status_t o;
        rc = ld == 1 ? lacc_op_status(&s, &st, &o, err, sizeof err) : fail(err, sizeof err, "no_account");
        if (rc == 0) {
            printf("ok account=%lu name=%s gen=%d discord=%d login=%d login_name=%s gamertag=%s gamertag_next_change=%lld "
                   "gamertag_change_days=%d gamertag_state=%s\n",
                   (unsigned long)st.account, st.name, o.gen, o.discord, o.login, o.login_name, o.gamertag, o.gt_next,
                   o.gt_days, o.gt_state[0] ? o.gt_state : "-");
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
    } else if (strcmp(act, "login-set") == 0 || strcmp(act, "login") == 0) {
        /* W334 (L6): Passwort nie aus der Kommandozeile (Prozessliste) - nur ueber stdin, eine Zeile */
        char pw[200];
        if (a2 && a2[0]) {
            printf("error password_on_cmdline (Passwort ueber stdin uebergeben)\n");
            return 2;
        }
        if (!cli_read_password(pw, sizeof pw)) {
            rc = fail(err, sizeof err, "no_password");
        } else if (strcmp(act, "login-set") == 0) {
            rc = ld == 1 ? lacc_op_login_set(&s, &st, a1, pw, err, sizeof err) : fail(err, sizeof err, "no_account");
            if (rc == 0) printf("ok login=1 login_name=%s\n", a1);
        } else {
            rc = lacc_op_login(&s, dir, a1, pw, &st, err, sizeof err);
            if (rc == 0) cli_out_store("", &st);
        }
        wipe(pw, sizeof pw);
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
        printf("error usage (create NAME [GAMERTAG] | check NAME | gamertag-check NAME | gamertag-check --suggest BASE | "
               "gamertag-set NAME | status | local | info | discord-link | discord-login | "
               "login-set LOGIN | login LOGIN (Passwort ueber stdin) | transfer CODE | recovery-show | "
               "recovery CODE | logout-all | delete | forget | campaign-probe [test])\n");
        return 2;
    }
    if (rc != 0) printf("error %s\n", err[0] ? err : "bad");
    wipe(&st, sizeof st);
    return rc == 0 ? 0 : 1;
}

#undef lacc_probe
#undef lacc_lobbies
#undef lacc_op_check
#undef lacc_op_create_gt
#undef lacc_op_gamertag_check
#undef lacc_op_gamertag_set
#undef lacc_op_status
#undef lacc_op_discord
#undef lacc_op_login_set
#undef lacc_op_login
#undef lacc_op_transfer
#undef lacc_op_recovery
#undef lacc_op_delete

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
        if (!why[0] && !had_api && lacc_server(&s) != (SB_STEELNET_SPKI1[0] || SB_STEELNET_SPKI2[0]))
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

    /* 6b: W344 Loeschen des ganzen Blocks (vorher nur der erste Eintrag bis zum ersten Nullzeichen) */
    {
        static const wchar_t kBlk[] = L"A=1\0SB_LOC_AUTH=42:1:ab\0Z=9\0"; /* + implizite Null = doppelte Null */
        wchar_t b[64];
        size_t i, n = sizeof kBlk / sizeof kBlk[0], nz = 0;
        memcpy(b, kBlk, sizeof kBlk);
        b[n] = L'x'; /* Waechter hinter dem Block bleibt */
        if (lacc_env_len(b) != n || lacc_env_len(NULL) != 0 || lacc_env_len(L"") != 1)
            _snprintf_s(why, sizeof why, _TRUNCATE, "Laenge %u statt %u", (unsigned)lacc_env_len(b), (unsigned)n);
        lacc_env_wipe(b);
        for (i = 0; i < n; i++) nz += b[i] != 0;
        if (!why[0] && (nz || b[n] != L'x')) _snprintf_s(why, sizeof why, _TRUNCATE, "%u Zeichen uebrig", (unsigned)nz);
        lacc_env_free(NULL);
    }
    ST_RESULT("W344 account: child env block wiped up to the double NUL (all entries, incl. SB_LOC_AUTH)");

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

    /* 7b: Live-Zahlen aus /v1/info ("online"-Objekt), alte Server ohne Objekt, Schluessel ausserhalb, Unsinn */
    {
        static const struct {
            const char *body;
            int has, pl, lb, gm;
        } kCase[] = {
            { "{\"ok\":true,\"version\":\"0.2.0\"}", 0, 0, 0, 0 }, /* alter Server */
            { "{\"ok\":true,\"online\":{\"players\":5,\"lobbies\":2,\"games\":1,\"games_today\":14,\"peak_7d\":9}}", 1, 5, 2, 1 },
            { "{\"ok\":true,\"online\": { \"games\" : 0 , \"players\":0,\"lobbies\":0 } ,\"version\":\"x\"}", 1, 0, 0, 0 },
            { "{\"ok\":true,\"players\":7,\"online\":{\"lobbies\":1,\"games\":1}}", 0, 0, 0, 0 }, /* players ausserhalb */
            { "{\"ok\":true,\"online\":{\"players\":-1,\"lobbies\":0,\"games\":0}}", 0, 0, 0, 0 },
            { "{\"ok\":true,\"online\":{\"players\":\"5\",\"lobbies\":0,\"games\":0}}", 0, 0, 0, 0 },
            { "{\"ok\":true,\"online\":true}", 0, 0, 0, 0 },
            { "{\"ok\":true,\"online\":{\"players\":3,\"lobbies\":1,\"games\":0", 0, 0, 0, 0 }, /* abgeschnitten */
            { "{\"ok\":true,\"online\":{\"m\":\"}\",\"players\":4,\"lobbies\":1,\"games\":2}}", 1, 4, 1, 2 }, /* } im Text */
        };
        int k;
        for (k = 0; k < (int)(sizeof kCase / sizeof kCase[0]) && !why[0]; k++) {
            lacc_probe_t pr;
            memset(&pr, 0x5A, sizeof pr);
            probe_counts(kCase[k].body, &pr);
            if (pr.has_counts != kCase[k].has || pr.players != kCase[k].pl || pr.lobbies != kCase[k].lb ||
                pr.games != kCase[k].gm)
                _snprintf_s(why, sizeof why, _TRUNCATE, "Fall %d: has %d %d/%d/%d", k, pr.has_counts, pr.players,
                            pr.lobbies, pr.games);
        }
    }
    ST_RESULT("SNP-COUNTS account: live counts from /v1/info (online object only, old server without it, bad values ignored)");

    /* 7c: W421 LOC-VERSION-GATE: min_client_version aus /v1/info strikt (X.Y.Z), sonst leer; Vergleich sb_netver_* */
    {
        static char longv[200];
        static const struct {
            const char *body, *want;
        } kMv[] = {
            { "{\"ok\":true,\"version\":\"0.2.0\"}", "" }, /* alter Server ohne Feld */
            { "{\"ok\":true,\"min_client_version\":\"0.3.2\"}", "0.3.2" },
            { "{\"ok\":true,\"min_client_version\" : \"10.20.3000\" ,\"x\":1}", "10.20.3000" },
            { "{\"ok\":true,\"min_client_version\":\"\"}", "" },       /* leer = keine Mindestversion */
            { "{\"ok\":true,\"min_client_version\":\"1.2\"}", "" },
            { "{\"ok\":true,\"min_client_version\":\"abc\"}", "" },
            { "{\"ok\":true,\"min_client_version\":\"1.2.3.4\"}", "" },
            { "{\"ok\":true,\"min_client_version\":\"0.3.2-beta\"}", "" },
            { "{\"ok\":true,\"min_client_version\":\" 0.3.2\"}", "" },
            { "{\"ok\":true,\"min_client_version\":\"12345.0.0\"}", "" },
            { "{\"ok\":true,\"min_client_version\":\"1..2\"}", "" },
            { "{\"ok\":true,\"min_client_version\":\".1.2\"}", "" },
            { "{\"ok\":true,\"min_client_version\":0.3}", "" },          /* Zahl statt Text */
            { "{\"ok\":true,\"min_client_version\":\"0.3.2", "" },        /* abgeschnitten */
            { "{\"ok\":true,\"min_client_version\":\"0.3.\\u0032\"}", "0.3.2" }, /* Escape wie jeder JSON-Text */
        };
        static const struct {
            const char *own, *min;
            int out;
        } kCmp[] = {
            { "0.3.1", "0.3.2", 1 }, { "0.3.2", "0.3.2", 0 }, { "0.3.10", "0.3.2", 0 }, { "0.3.2", "0.3.10", 1 },
            { "0.4.0", "0.3.9", 0 }, { "0.9.9", "1.0.0", 1 }, { "0.3.2", "", 0 }, { "0.3.2", "1.2", 0 },
            { "0.3.2", "abc", 0 }, { "", "9.9.9", 0 }, { "dev", "9.9.9", 0 },
        };
        int k;
        for (k = 0; k < (int)(sizeof kMv / sizeof kMv[0]) && !why[0]; k++) {
            lacc_probe_t pr;
            memset(&pr, 0x5A, sizeof pr);
            probe_minver(kMv[k].body, &pr);
            if (strcmp(pr.min_ver, kMv[k].want) != 0)
                _snprintf_s(why, sizeof why, _TRUNCATE, "W421 Feld %d: '%.15s'", k, pr.min_ver);
        }
        if (!why[0]) { /* sehr lang: 1.1.1.1... bzw. 0000...1.2.3 - Laengengrenze, nie gekuerzt gueltig */
            lacc_probe_t pr;
            char body[300];
            memset(longv, '1', sizeof longv - 1);
            longv[1] = longv[3] = '.';
            longv[sizeof longv - 1] = '\0';
            _snprintf_s(body, sizeof body, _TRUNCATE, "{\"ok\":true,\"min_client_version\":\"%s\"}", longv);
            probe_minver(body, &pr);
            if (pr.min_ver[0]) _snprintf_s(why, sizeof why, _TRUNCATE, "W421 lang: '%.15s'", pr.min_ver);
        }
        for (k = 0; k < (int)(sizeof kCmp / sizeof kCmp[0]) && !why[0]; k++) {
            if (sb_netver_outdated(kCmp[k].own, kCmp[k].min) != kCmp[k].out)
                _snprintf_s(why, sizeof why, _TRUNCATE, "W421 Vergleich %s < %s", kCmp[k].own, kCmp[k].min);
        }
    }
    ST_RESULT("W421 account: min_client_version from /v1/info strict X.Y.Z (missing/empty/1.2/abc/long ignored), compare");

    /* 8: W332 Discord-Empfaenger ohne Socket (loop_parse, gefuzzt in tests/fuzz/proj_launcher discord_loop) */
    {
        static const char S[] = "0123456789abcdef0123456789abcdef";
        static char big[4096];
        char path[2048], code[512], rq[256];
        const char *r1 = "GET /favicon.ico HTTP/1.1\r\n\r\n", *r2 = "GET /discordX?state=x HTTP/1.1\r\n\r\n";
        if (loop_parse(r1, (int)strlen(r1), S, path, sizeof path, code, sizeof code) != LOOP_P_404 ||
            loop_parse(r2, (int)strlen(r2), S, path, sizeof path, code, sizeof code) != LOOP_P_404 ||
            loop_parse("GET ", 4, S, path, sizeof path, code, sizeof code) != LOOP_P_404)
            strcpy_s(why, sizeof why, "fremder Pfad nicht 404");
        _snprintf_s(rq, sizeof rq, _TRUNCATE, "GET /discord?code=abc&state=%.31s0 HTTP/1.1\r\n\r\n", S);
        if (!why[0] && (loop_parse(rq, (int)strlen(rq), S, path, sizeof path, code, sizeof code) != LOOP_P_FOREIGN ||
                        code[0] || loop_parse(rq, (int)strlen(rq), NULL, path, sizeof path, code, sizeof code) !=
                                       LOOP_P_FOREIGN))
            strcpy_s(why, sizeof why, "falscher/fehlender state angenommen");
        _snprintf_s(rq, sizeof rq, _TRUNCATE, "GET /discord?error=access_denied&state=%s HTTP/1.1\r\n\r\n", S);
        if (!why[0] && loop_parse(rq, (int)strlen(rq), S, path, sizeof path, code, sizeof code) != LOOP_P_DENIED)
            strcpy_s(why, sizeof why, "error= nicht erkannt");
        _snprintf_s(rq, sizeof rq, _TRUNCATE, "GET /discord?state=%s&code=a%%2Bb+c%%zz HTTP/1.1\r\n\r\n", S);
        if (!why[0] && (loop_parse(rq, (int)strlen(rq), S, path, sizeof path, code, sizeof code) != LOOP_P_CODE ||
                        strcmp(code, "a+b c%zz") != 0))
            strcpy_s(why, sizeof why, "code-Dekodierung");
        memset(big, 'A', sizeof big - 1); /* volle 4095 Byte (recv-Grenze), Pfad laenger als path */
        memcpy(big, "GET /discord?state=", 19);
        big[sizeof big - 1] = '\0';
        if (!why[0] && loop_parse(big, (int)sizeof big - 1, S, path, sizeof path, code, sizeof code) != LOOP_P_FOREIGN)
            strcpy_s(why, sizeof why, "uebergrosse Anfrage");
    }
    ST_RESULT("W332 account: Discord loopback parser without socket (foreign paths 404, wrong/missing state, error=, %-decoding, 4 KB request)");

    /* W487 LIVE-GAMERTAG: lokale Regel (Grenzfaelle), Rumpf create/gamertag_check (Maskierung), Parser status/
     * gamertag_check/gamertag_set inkl. gamertag_refused/change_limit, kein Netz bei ungueltigem Gamertag */
    {
        static const char *const kOk[] = { "A", "Ace Pilot", "abcdefghijklmno", "Z9 9 9", "a1" };
        static const char *const kBad[] = { "", "1Ace", " Ace", "Ace ", "Ace  Pilot", "abcdefghijklmnop", "Ace_Pilot",
                                            "Ace-1", "\xC3\xA4" "bc", "Ace\tX", "Ace;x" };
        char b[256], e2[64];
        lacc_status_t so;
        lacc_gtcheck_t gc;
        lacc_gtset_t gs;
        lacc_srv_t off;
        for (i = 0; i < (int)(sizeof kOk / sizeof kOk[0]) && !why[0]; i++)
            if (!lacc_valid_gamertag(kOk[i])) _snprintf_s(why, sizeof why, _TRUNCATE, "gueltig abgelehnt: '%s'", kOk[i]);
        for (i = 0; i < (int)(sizeof kBad / sizeof kBad[0]) && !why[0]; i++)
            if (lacc_valid_gamertag(kBad[i])) _snprintf_s(why, sizeof why, _TRUNCATE, "ungueltig angenommen: '%s'", kBad[i]);
        if (!why[0] && lacc_valid_gamertag(NULL)) strcpy_s(why, sizeof why, "NULL angenommen");
        lacc_create_body("Ace Pilot", "Ace Pilot", b, sizeof b);
        if (!why[0] && strcmp(b, "{\"name\":\"Ace Pilot\",\"gamertag\":\"Ace Pilot\"}") != 0)
            _snprintf_s(why, sizeof why, _TRUNCATE, "create-Rumpf '%s'", b);
        lacc_create_body("Ace", "a\"b\\c", b, sizeof b); /* Maskierung (Rumpf bleibt gueltiges JSON) */
        if (!why[0] && strcmp(b, "{\"name\":\"Ace\",\"gamertag\":\"a\\\"b\\\\c\"}") != 0)
            _snprintf_s(why, sizeof why, _TRUNCATE, "create-Rumpf maskiert '%s'", b);
        if (!why[0] && (!lacc_js_str(b, "gamertag", e2, sizeof e2) || strcmp(e2, "a\"b\\c") != 0))
            strcpy_s(why, sizeof why, "create-Rumpf Rundlauf");
        lacc_create_body("Ace", "", b, sizeof b);
        if (!why[0] && strcmp(b, "{\"name\":\"Ace\"}") != 0) _snprintf_s(why, sizeof why, _TRUNCATE, "create ohne Gamertag '%s'", b);
        lacc_gtcheck_body("Ace", NULL, b, sizeof b);
        if (!why[0] && strcmp(b, "{\"gamertag\":\"Ace\"}") != 0) _snprintf_s(why, sizeof why, _TRUNCATE, "check-Rumpf '%s'", b);
        lacc_gtcheck_body(NULL, "Ace Pilot", b, sizeof b);
        if (!why[0] && strcmp(b, "{\"suggest\":\"Ace Pilot\"}") != 0) _snprintf_s(why, sizeof why, _TRUNCATE, "suggest-Rumpf '%s'", b);
        lacc_gtcheck_body("Ace", "Ace", b, sizeof b);
        if (!why[0] && strcmp(b, "{\"gamertag\":\"Ace\",\"suggest\":\"Ace\"}") != 0) strcpy_s(why, sizeof why, "check+suggest-Rumpf");
        /* status: neuer Server, aelterer Server (ohne Felder), kaputter Gamertag */
        lacc_status_parse("{\"ok\":true,\"discord\":true,\"login\":false,\"gen\":2,\"gamertag\":\"Ace Pilot\","
                          "\"gamertag_next_change\":1764000000,\"gamertag_change_days\":7,\"gamertag_state\":\"ok\"}", &so);
        if (!why[0] && (!so.discord || so.gen != 2 || strcmp(so.gamertag, "Ace Pilot") != 0 || so.gt_next != 1764000000LL ||
                        so.gt_days != 7 || strcmp(so.gt_state, "ok") != 0))
            strcpy_s(why, sizeof why, "status (neu)");
        lacc_status_parse("{\"ok\":true,\"discord\":false,\"login\":true,\"login_name\":\"x\"}", &so);
        if (!why[0] && (so.gamertag[0] || so.gt_next || so.gt_days != 7 || so.gt_state[0] || !so.login))
            strcpy_s(why, sizeof why, "status (alter Server)");
        lacc_status_parse("{\"ok\":true,\"gamertag\":\"1;rm\",\"gamertag_state\":\"Off!\",\"gamertag_change_days\":-3}", &so);
        if (!why[0] && (so.gamertag[0] || so.gt_state[0] || so.gt_days != 7)) strcpy_s(why, sizeof why, "status (kaputte Werte)");
        lacc_status_parse("{\"ok\":true,\"gamertag\":\"\",\"gamertag_next_change\":0,\"gamertag_state\":\"unavailable\"}", &so);
        if (!why[0] && (so.gamertag[0] || so.gt_next || strcmp(so.gt_state, "unavailable") != 0))
            strcpy_s(why, sizeof why, "status (ohne Gamertag)");
        /* gamertag_check */
        if (!why[0] && (lacc_gtcheck_parse("{\"ok\":true,\"gamertag\":\"Ace\",\"valid\":true,\"available\":false,"
                                           "\"status\":\"taken\",\"suggestion\":\"\"}", &gc) != 0 ||
                        !gc.has_name || gc.available || !gc.valid || strcmp(gc.status, "taken") != 0 || gc.suggestion[0]))
            strcpy_s(why, sizeof why, "check taken");
        if (!why[0] && (lacc_gtcheck_parse("{\"ok\":true,\"suggestion\":\"Ace Pilot2\"}", &gc) != 0 || gc.has_name ||
                        strcmp(gc.suggestion, "Ace Pilot2") != 0))
            strcpy_s(why, sizeof why, "check suggest");
        if (!why[0] && (lacc_gtcheck_parse("{\"ok\":true,\"gamertag\":\"Ace\",\"valid\":true,\"available\":true,"
                                           "\"status\":\"ok\"}", &gc) != 0 || !gc.available))
            strcpy_s(why, sizeof why, "check frei");
        if (!why[0] && lacc_gtcheck_parse("{\"ok\":true,\"gamertag\":\"Ace\",\"valid\":true}", &gc) == 0)
            strcpy_s(why, sizeof why, "check ohne available angenommen");
        if (!why[0] && (lacc_gtcheck_parse("{\"ok\":true,\"gamertag\":\"1x\",\"valid\":true,\"available\":true,"
                                           "\"status\":\"ok\"}", &gc) != 0 || gc.available))
            strcpy_s(why, sizeof why, "check: ungueltiger Name als frei");
        if (!why[0] && (lacc_gtcheck_parse("{\"ok\":true,\"gamertag\":\"Ace\",\"valid\":false,\"available\":false,"
                                           "\"status\":\"invalid_characters\",\"suggestion\":\"Bad Name!\"}", &gc) != 0 ||
                        strcmp(gc.status, "invalid_characters") != 0 || gc.suggestion[0]))
            strcpy_s(why, sizeof why, "check invalid/kaputter Vorschlag");
        /* gamertag_set: Erfolg, gamertag_refused (taken, change_limit), create mit gamertag_error */
        lacc_gtset_parse("{\"ok\":true,\"gamertag\":\"Ace Pilot\",\"next_change_at\":1764600000}", &gs);
        if (!why[0] && (strcmp(gs.gamertag, "Ace Pilot") != 0 || gs.next_at != 1764600000LL || gs.status[0]))
            strcpy_s(why, sizeof why, "set ok");
        lacc_gtset_parse("{\"ok\":false,\"error\":\"gamertag_refused\",\"status\":\"change_limit\",\"next_change_at\":1764600000}", &gs);
        if (!why[0] && (strcmp(gs.status, "change_limit") != 0 || gs.next_at != 1764600000LL || gs.gamertag[0]))
            strcpy_s(why, sizeof why, "set change_limit");
        lacc_gtset_parse("{\"ok\":false,\"error\":\"gamertag_refused\",\"status\":\"similar\"}", &gs);
        if (!why[0] && (strcmp(gs.status, "similar") != 0 || gs.next_at)) strcpy_s(why, sizeof why, "set similar");
        lacc_gtset_parse("{\"ok\":true,\"account\":5,\"gamertag\":\"\",\"gamertag_error\":\"unavailable\"}", &gs);
        if (!why[0] && (gs.gamertag[0] || strcmp(gs.status, "unavailable") != 0)) strcpy_s(why, sizeof why, "create gamertag_error");
        if (!why[0] && (!lacc_gt_status_known("change_limit") || !lacc_gt_status_known("similar") || lacc_gt_status_known("x")))
            strcpy_s(why, sizeof why, "Statuscodes");
        /* ungueltiger Gamertag: lokal abgelehnt, ohne Netz (Dienst "aus" wuerde sonst "off" melden) */
        memset(&off, 0, sizeof off);
        if (!why[0] && (lacc_op_gamertag_set(&off, NULL, "1bad", &gs, e2, sizeof e2) == 0 || strcmp(e2, "invalid_gamertag") != 0))
            _snprintf_s(why, sizeof why, _TRUNCATE, "set lokal: '%s'", e2);
        if (!why[0] && (lacc_op_create_gt(&off, "", "Ace", "x ", NULL, &gs, e2, sizeof e2) == 0 ||
                        strcmp(e2, "invalid_gamertag") != 0))
            _snprintf_s(why, sizeof why, _TRUNCATE, "create lokal: '%s'", e2);
        if (!why[0] && (lacc_op_gamertag_check(&off, "Ace", NULL, &gc, e2, sizeof e2) == 0 || strcmp(e2, "off") != 0))
            _snprintf_s(why, sizeof why, _TRUNCATE, "check ohne Dienst: '%s'", e2);
    }
    ST_RESULT("W487 account: gamertag rule (edge cases), create/gamertag_check bodies with JSON escaping, status/gamertag_check/gamertag_set parsers incl. gamertag_refused/change_limit, local refusal without network");
#undef ST_RESULT
    wipe(K, sizeof K);
    wipe(G, sizeof G);
}
#endif /* !LACC_HELPER */
