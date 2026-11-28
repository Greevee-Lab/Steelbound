/* launcher_camp_net.c - W537 LAUNCHER-SLIM: Erreichbarkeits-Probe des Steelbound-Kampagnenzugangs (aus
 * launcher_camp.c, W491/W534). lcamp_probe = reine TCP-Verbindung (sofort geschlossen, keine Daten), lcamp_probe_tls =
 * TCP + TLS-Handshake + Pin (nur Entwickler-CLI --steelnet campaign-probe). Gebaut ins Hilfsprogramm sb_account.exe
 * und in Entwickler-/Test-Launcher, nicht in den ausgelieferten Launcher (der startet sb_account.exe, Auftrag camp). */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "launcher_camp.h"
#include "launcher_u8.h"
#ifndef LACC_HELPER
#include "sb_tls.h"
#endif

#pragma comment(lib, "ws2_32.lib")

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

/* W534 AVPRE: Erreichbarkeit fuer die Launcher-Oberflaeche = nur TCP-Verbindung zum Zugang, sofort schliessen
 * (kein TLS, keine Daten). Der Pin wird beim echten Verbinden im Spielprozess geprueft (Kampagnenbruecke, sb_tls in
 * sb_emu.dll); ohne passenden Pin sendet das Spiel nichts. Damit braucht der ausgelieferte Launcher kein SChannel
 * (Secur32) mehr. lcamp_probe_tls (TLS + Pin) bleibt fuer `--steelnet campaign-probe` (nur Entwickler-Builds). */
int lcamp_probe(int test_campaign, int timeout_ms, char *err, size_t ecap)
{
    lcamp_target_t t;
    WSADATA wd;
    SOCKET s;
    int to = 0;
    if (err && ecap) err[0] = '\0';
    if (!lcamp_target(test_campaign, &t)) return fail(err, ecap, "off");
    if (timeout_ms < 500) timeout_ms = 500;
    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) return fail(err, ecap, "net");
    s = tcp_connect(&t, GetTickCount64() + (ULONGLONG)timeout_ms, &to);
    if (s == INVALID_SOCKET) {
        fail(err, ecap, to ? "timeout" : "net");
        WSACleanup();
        return 1;
    }
    shutdown(s, SD_BOTH);
    closesocket(s);
    WSACleanup();
    return 0;
}

#ifndef LACC_HELPER /* TLS-Probe (Entwickler-CLI) und Selbsttest nur im Launcher-Build */
int lcamp_probe_tls(int test_campaign, int timeout_ms, char *err, size_t ecap)
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

/* W534 AVPRE Selbsttest: lcamp_probe ist reines TCP. Loopback-Listener -> 0; nach dem Schliessen -> 1 "net"
 * (bzw. "timeout"); ohne Pin -> "off". Setzt/loescht SB_CAMPAIGN_ADDR/SB_CAMPAIGN_SPKI selbst. why leer = bestanden. */
void lcamp_selftest_tcp(char *why, size_t cap)
{
    WSADATA wd;
    SOCKET l;
    struct sockaddr_in a;
    int al = sizeof a, rc;
    char v[64], err[16];
    why[0] = '\0';
    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) {
        strcpy_s(why, cap, "WSAStartup");
        return;
    }
    l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (l == INVALID_SOCKET || bind(l, (struct sockaddr *)&a, sizeof a) != 0 || listen(l, 4) != 0 ||
        getsockname(l, (struct sockaddr *)&a, &al) != 0) {
        strcpy_s(why, cap, "Loopback-Listener");
        if (l != INVALID_SOCKET) closesocket(l);
        WSACleanup();
        return;
    }
    _snprintf_s(v, sizeof v, _TRUNCATE, "127.0.0.1:%u", (unsigned)ntohs(a.sin_port));
    SetEnvironmentVariableA("SB_CAMPAIGN_ADDR", v);
    SetEnvironmentVariableA("SB_CAMPAIGN_SPKI", NULL);
    rc = lcamp_probe(0, 2000, err, sizeof err);
    if (rc != 1 || strcmp(err, "off") != 0) _snprintf_s(why, cap, _TRUNCATE, "ohne Pin rc %d err %s (erwartet off)", rc, err);
    SetEnvironmentVariableA("SB_CAMPAIGN_SPKI", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    if (!why[0]) {
        rc = lcamp_probe(0, 2000, err, sizeof err);
        if (rc != 0) _snprintf_s(why, cap, _TRUNCATE, "Listener offen: rc %d err %s (erwartet 0)", rc, err);
    }
    closesocket(l);
    if (!why[0]) {
        rc = lcamp_probe(0, 1500, err, sizeof err);
        if (rc != 1 || (strcmp(err, "net") != 0 && strcmp(err, "timeout") != 0))
            _snprintf_s(why, cap, _TRUNCATE, "Listener zu: rc %d err %s (erwartet net)", rc, err);
    }
    SetEnvironmentVariableA("SB_CAMPAIGN_ADDR", NULL);
    SetEnvironmentVariableA("SB_CAMPAIGN_SPKI", NULL);
    WSACleanup();
}
#endif /* !LACC_HELPER */
