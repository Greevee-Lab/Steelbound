/* launcher_camp.c - W491 CAMP-CLIENT (Launcher): Kampagnenstatus und Erreichbarkeit des Steelbound-Zugangs
 * (siehe launcher_camp.h). */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "launcher_camp.h"
#include "launcher_account.h"     /* lacc_is_loopback */
#include "launcher_account_cfg.h" /* steelnet_pins.h: SB_STEELNET_HOST, SB_CAMPAIGN_SPKI1/2 */
#include "launcher_u8.h"
#include "loc_camp.h"             /* LOCCAMP_PORT_SEASON / LOCCAMP_PORT_TEST (eine Quelle mit dem Client) */
#include "sb_tls.h"

#pragma comment(lib, "ws2_32.lib")

/* ACC_LIVE_* aus launcher_gui_int.h (dort per Pruefung zur Uebersetzungszeit gegen diese Werte abgesichert) */
#define A_OK 0
#define A_OFF 1
#define A_NOACC 2
#define A_UNK 3
#define A_NOGT 4
#define A_GTOFF 5

int lcamp_state(int acc, int outdated, int reach)
{
    switch (acc) {
    case A_OK: break;
    case A_OFF: return LCAMP_OFF;
    case A_NOACC: return LCAMP_NOACC;
    case A_UNK: return LCAMP_UNK;
    case A_GTOFF: return LCAMP_GTOFF;
    case A_NOGT: return LCAMP_NOGT;
    default: return LCAMP_UNK;
    }
    if (outdated) return LCAMP_OLD;
    if (reach == LCAMP_R_OK) return LCAMP_OK;
    return reach == LCAMP_R_FAIL ? LCAMP_UNREACH : LCAMP_CHECK;
}

static int hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* 64 Hex (Doppelpunkte/Leerzeichen erlaubt) -> klein, sonst leer */
static void norm_pin(const char *in, char out[65])
{
    int o = 0;
    out[0] = '\0';
    if (!in) return;
    for (; *in && o < 64; in++) {
        if (*in == ':' || *in == ' ') continue;
        if (hexv(*in) < 0) return;
        out[o++] = (char)(*in >= 'A' && *in <= 'F' ? *in + 32 : *in);
    }
    out[o] = '\0';
    if (*in || o != 64) out[0] = '\0';
}

int lcamp_target(int test_campaign, lcamp_target_t *t)
{
    char v[200];
    memset(t, 0, sizeof *t);
    strcpy_s(t->host, sizeof t->host, SB_STEELNET_HOST);
    t->port = test_campaign ? (int)LOCCAMP_PORT_TEST : (int)LOCCAMP_PORT_SEASON;
    norm_pin(SB_CAMPAIGN_SPKI1, t->pin1);
    norm_pin(SB_CAMPAIGN_SPKI2, t->pin2);
    if (u8_GetEnv("SB_CAMPAIGN_ADDR", v, sizeof v) > 0) { /* Test: nur Loopback, eigener Pin */
        char *c = strrchr(v, ':');
        if (c) {
            int port = atoi(c + 1);
            *c = '\0';
            if (lacc_is_loopback(v) && port > 0 && port < 65536) {
                strcpy_s(t->host, sizeof t->host, v);
                t->port = port;
                t->test = 1;
                t->pin1[0] = t->pin2[0] = '\0';
                if (u8_GetEnv("SB_CAMPAIGN_SPKI", v, sizeof v) > 0) norm_pin(v, t->pin1);
            }
        }
    }
    return t->pin1[0] != '\0' || t->pin2[0] != '\0';
}

static int fail(char *err, size_t ecap, const char *code)
{
    if (err && ecap) strcpy_s(err, ecap, code);
    return 1;
}

/* Restzeit in ms bis zur Frist (>= 0) */
static int left_ms(ULONGLONG end)
{
    ULONGLONG now = GetTickCount64();
    return now >= end ? 0 : (int)(end - now);
}

/* Nicht blockierender Verbindungsaufbau mit Frist. INVALID_SOCKET bei Fehler/Zeitablauf. */
static SOCKET tcp_connect(const lcamp_target_t *t, ULONGLONG end, int *timed_out)
{
    struct addrinfo hints, *res = NULL, *ai;
    char port[16];
    SOCKET s = INVALID_SOCKET;
    *timed_out = 0;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    _snprintf_s(port, sizeof port, _TRUNCATE, "%d", t->port);
    if (getaddrinfo(t->host, port, &hints, &res) != 0 || !res) return INVALID_SOCKET;
    for (ai = res; ai && s == INVALID_SOCKET; ai = ai->ai_next) {
        u_long nb = 1;
        int r;
        SOCKET c = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (c == INVALID_SOCKET) continue;
        ioctlsocket(c, FIONBIO, &nb);
        r = connect(c, ai->ai_addr, (int)ai->ai_addrlen);
        if (r != 0 && WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set wf, ef;
            struct timeval tv;
            int ms = left_ms(end);
            FD_ZERO(&wf);
            FD_ZERO(&ef);
            FD_SET(c, &wf);
            FD_SET(c, &ef);
            tv.tv_sec = ms / 1000;
            tv.tv_usec = (ms % 1000) * 1000;
            r = select(0, NULL, &wf, &ef, &tv);
            if (r == 0) *timed_out = 1;
            if (r > 0 && FD_ISSET(c, &wf)) {
                int so = 0, sl = sizeof so;
                r = getsockopt(c, SOL_SOCKET, SO_ERROR, (char *)&so, &sl) == 0 && so == 0 ? 0 : -1;
            } else {
                r = -1;
            }
        }
        if (r == 0) s = c;
        else closesocket(c);
        if (left_ms(end) == 0) break;
    }
    freeaddrinfo(res);
    return s;
}

int lcamp_probe(int test_campaign, int timeout_ms, char *err, size_t ecap)
{
    lcamp_target_t t;
    uint8_t pins[2][32];
    WSADATA wd;
    SOCKET s;
    sbtls *tls = NULL;
    ULONGLONG end;
    int rc = 1, to = 0, r;
    if (err && ecap) err[0] = '\0';
    if (!lcamp_target(test_campaign, &t)) return fail(err, ecap, "off");
    memset(pins, 0, sizeof pins);
    if (t.pin1[0]) sbtls_parse_pin(t.pin1, pins[0]);
    if (t.pin2[0]) sbtls_parse_pin(t.pin2, pins[1]);
    if (timeout_ms < 500) timeout_ms = 500;
    end = GetTickCount64() + (ULONGLONG)timeout_ms;
    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) return fail(err, ecap, "net");
    s = tcp_connect(&t, end, &to);
    if (s == INVALID_SOCKET) {
        fail(err, ecap, to ? "timeout" : "net");
        WSACleanup();
        return 1;
    }
    if (sbtls_client_new(&tls, s, t.host, (const uint8_t(*)[32])pins, 2) != SBTLS_OK) {
        fail(err, ecap, "tls");
    } else {
        for (;;) {
            r = sbtls_handshake(tls);
            if (r == SBTLS_OK) { /* Pin + Suite bestanden; keine Anwendungsdaten */
                rc = 0;
                break;
            }
            if (r != SBTLS_WANT_IO) {
                fail(err, ecap, sbtls_pin_failed(tls) ? "pin" : "tls");
                break;
            }
            if (left_ms(end) == 0) {
                fail(err, ecap, "timeout");
                break;
            }
            { /* auf Daten warten (Senden puffert sb_tls; kurze Scheiben, damit auch offenes Senden weiterlaeuft) */
                fd_set rf;
                struct timeval tv;
                int ms = left_ms(end);
                if (ms > 50) ms = 50;
                FD_ZERO(&rf);
                FD_SET(s, &rf);
                tv.tv_sec = 0;
                tv.tv_usec = ms * 1000;
                select(0, &rf, NULL, NULL, &tv);
            }
        }
    }
    if (tls) sbtls_free(tls);
    shutdown(s, SD_BOTH);
    closesocket(s);
    WSACleanup();
    if (rc == 0 && err && ecap) err[0] = '\0';
    return rc;
}
