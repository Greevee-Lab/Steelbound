/* loc_camp_bridge.c - W491 CAMP-CLIENT: Kampagnen-Bruecke (siehe loc_camp_bridge.h, docs/loc/LOC_CAMPAIGN.md).
 *
 * Ablauf je Verbindung (Spiel oder Bereitschaftspruefung):
 *   Ticket vom Transport (loctun_camp_ticket) -> TCP zum Zugang -> TLS mit SPKI-Pin (sb_tls) -> Vorspann "SBGW" ->
 *   Antwort (6 B): 0 = ok -> Original-Protokoll durchreichen; 1 = abgewiesen -> einmal neues Ticket, dann aufgeben;
 *   2 = beschaeftigt; 3 = Version nicht unterstuetzt.
 * Server -> Spiel nur ganze Rahmen [u16 len][u16 cmd][payload] (len <= 0x1400, sonst Trennung), Spiel -> Server
 * unveraendert. Keine Inhalte, Tickets oder Adressen im Log, nur Zustaende und Zahlen. */
#include "loc_camp_bridge.h"
#include "loc_tun.h"
#include "sb_tls.h"
#include "steelnet_pins.h"

#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#define CB_MAX_CONN 6
#define CB_EXPECT 8
#define CB_TICKET_WAIT_MS 20000u   /* Spielverbindung wartet hoechstens so lange auf ein Ticket */
#define CB_CONNECT_MS 10000u       /* TCP + TLS + Vorspann */
#define CB_IDLE_MS 60000u          /* wie das Zeitlimit des Zugangs je Verbindung */
#define CB_REPROBE_MS 60000u       /* nicht verfuegbar: so oft neu pruefen */
#define CB_DN_CAP (LOCCAMP_FRAME_MAX + 4 + 16384)
#define CB_OUT_CAP (2 * (LOCCAMP_FRAME_MAX + 4) + 16384)

enum { CS_FREE = 0, CS_TICKET, CS_CONNECT, CS_TLS, CS_PRE, CS_RELAY };

typedef struct cconn {
    int st, probe, retried;
    SOCKET g;   /* Spielseite (angenommener Loopback-Socket), INVALID bei Pruefung */
    SOCKET s;   /* Zugang */
    sbtls *tls;
    DWORD t0, t_st, t_act;
    uint8_t rep[LOCCAMP_GW_REPLY];
    int replen;
    uint8_t dn[CB_DN_CAP];
    int dnlen;
    uint8_t out[CB_OUT_CAP];
    int outlen, outoff;
    int g_closed; /* Spiel hat geschlossen */
    int s_closed; /* Zugang hat geschlossen: Rest zustellen, dann Spielseite schliessen */
    long up, down;
} cconn;

static struct {
    CRITICAL_SECTION cs;
    int cs_init;
    HANDLE th;
    volatile LONG stop, state, running;
    SOCKET ls;
    uint16_t lport; /* Host-Reihenfolge */
    struct loctun *tun;
    loccamp_cfg c;
    uint8_t pins[2][32];
    int npins;
    struct sockaddr_in gw;
    int gw_ok;
    DWORD probe_at;
    int probe_logged;
    uint16_t expect[CB_EXPECT]; /* lokale Ports (Netz-Reihenfolge) umgelenkter Spiel-Sockets */
    DWORD expect_at[CB_EXPECT];
    cconn *cn[CB_MAX_CONN];
    volatile LONG n_conn, n_ok, n_refused, n_failed;
    loccamp_log_fn log;
} B;

static void cb_logf(const char *fmt, ...)
{
    char b[300];
    va_list ap;
    if (!B.log) return;
    va_start(ap, fmt);
    _vsnprintf_s(b, sizeof b, _TRUNCATE, fmt, ap);
    va_end(ap);
    B.log(b);
}

static void cb_set_state(int st, const char *why)
{
    LONG old = InterlockedExchange(&B.state, st);
    if (old != st) cb_logf("LOCCAMP: Kampagne %s (%s)", loccamp_state_name(st), why);
}

/* ---------- Ziel und Pins ---------- */
static int cb_resolve(void)
{
    struct addrinfo hints, *res = NULL;
    char ps[8];
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    _snprintf_s(ps, sizeof ps, _TRUNCATE, "%u", (unsigned)B.c.port);
    if (getaddrinfo(B.c.host, ps, &hints, &res) != 0 || !res) return 0;
    memcpy(&B.gw, res->ai_addr, sizeof B.gw);
    freeaddrinfo(res);
    {
        uint32_t ip = ntohl(B.gw.sin_addr.s_addr);
        int lo = (ip >> 24) == 127;
        char ev[80];
        DWORD gl;
        B.npins = 0;
        memset(B.pins, 0, sizeof B.pins);
        if (sbtls_parse_pin(SB_CAMPAIGN_SPKI1, B.pins[B.npins])) B.npins++;
        if (sbtls_parse_pin(SB_CAMPAIGN_SPKI2, B.pins[B.npins])) B.npins++;
        if (lo) { /* lokaler Test-Zugang: eigener Pin nur mit Loopback-Ziel (wie SB_STEELNET_SPKI) */
            if (B.npins < 2 && B.c.spki[0] && sbtls_parse_pin(B.c.spki, B.pins[B.npins])) B.npins++;
            gl = GetEnvironmentVariableA("SB_CAMPAIGN_SPKI", ev, sizeof ev);
            if (gl > 0 && gl < sizeof ev && B.npins < 2 && sbtls_parse_pin(ev, B.pins[B.npins])) B.npins++;
        } else if (ip == 0 || (ip >> 24) == 10 || (ip >> 20) == 0xAC1 || (ip >> 16) == 0xC0A8 ||
                   (ip >> 16) == 0xA9FE || ip >= 0xE0000000u) {
            return 0; /* privat/link-local/Multicast: nie (DNS-Umlenkung ins LAN) */
        }
    }
    return B.npins > 0;
}

/* ---------- Verbindungen ---------- */
static void cb_close(cconn *c, const char *why, int failed)
{
    if (!c) return;
    if (c->tls) sbtls_free(c->tls);
    c->tls = NULL;
    if (c->s != INVALID_SOCKET) closesocket(c->s);
    c->s = INVALID_SOCKET;
    if (c->g != INVALID_SOCKET) {
        shutdown(c->g, SD_SEND);
        closesocket(c->g);
    }
    c->g = INVALID_SOCKET;
    if (failed) InterlockedIncrement(&B.n_failed);
    if (c->probe && B.state == LOCCAMP_ST_PENDING) cb_set_state(LOCCAMP_ST_DOWN, why ? why : "Pruefung abgebrochen");
    if (why && (failed || c->probe))
        cb_logf("LOCCAMP: %s beendet (%s, %ld B hin, %ld B zurueck, %lu ms)", c->probe ? "Pruefung" : "Verbindung",
                why, c->up, c->down, (unsigned long)(GetTickCount() - c->t0));
    SecureZeroMemory(c, sizeof *c);
    c->g = c->s = INVALID_SOCKET;
    c->st = CS_FREE;
}

static cconn *cb_new(SOCKET g, int probe)
{
    int i;
    for (i = 0; i < CB_MAX_CONN; i++) {
        cconn *c = B.cn[i];
        if (c->st == CS_FREE) {
            SecureZeroMemory(c, sizeof *c);
            c->g = g;
            c->s = INVALID_SOCKET;
            c->probe = probe;
            c->st = CS_TICKET;
            c->t0 = c->t_st = c->t_act = GetTickCount();
            return c;
        }
    }
    return NULL;
}

static void cb_goto(cconn *c, int st)
{
    c->st = st;
    c->t_st = GetTickCount();
}

static void cb_start_connect(cconn *c)
{
    u_long nb = 1;
    int r;
    if (!B.gw_ok && !(B.gw_ok = cb_resolve())) {
        cb_close(c, "Zugang nicht aufloesbar bzw. kein Pin", 1);
        return;
    }
    c->s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (c->s == INVALID_SOCKET) {
        cb_close(c, "socket", 1);
        return;
    }
    ioctlsocket(c->s, FIONBIO, &nb);
    {
        BOOL on = TRUE;
        setsockopt(c->s, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof on);
    }
    r = connect(c->s, (const struct sockaddr *)&B.gw, sizeof B.gw);
    if (r == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
        B.gw_ok = 0; /* beim naechsten Mal neu aufloesen */
        cb_close(c, "connect", 1);
        return;
    }
    cb_goto(c, CS_CONNECT);
}

static void cb_send_pre(cconn *c)
{
    uint8_t tk[LOCCAMP_TICKET_MAX], pre[LOCCAMP_PRE_MAX];
    long long exp = 0;
    int n = loctun_camp_ticket(B.tun, tk, sizeof tk, &exp, NULL, 0), pl;
    pl = n > 0 ? loccamp_preamble(tk, n, pre, sizeof pre) : -1;
    SecureZeroMemory(tk, sizeof tk);
    if (pl <= 0 || sbtls_write(c->tls, pre, pl) != SBTLS_OK) {
        SecureZeroMemory(pre, sizeof pre);
        cb_close(c, n > 0 ? "tls-senden" : "Ticket abgelaufen", 1);
        return;
    }
    SecureZeroMemory(pre, sizeof pre);
    c->replen = 0;
    cb_goto(c, CS_PRE);
}

/* Antwort des Zugangs auswerten (genau 6 Byte; danach folgende Bytes = Original-Protokoll ab 0x1010). */
static void cb_on_reply(cconn *c)
{
    int st = loccamp_gw_status(c->rep, c->replen);
    if (st == LOCCAMP_GW_OK) {
        InterlockedIncrement(&B.n_ok);
        if (c->probe) {
            cb_set_state(LOCCAMP_ST_READY, "Ticket angenommen, Zugang erreichbar");
            cb_close(c, NULL, 0);
            return;
        }
        cb_goto(c, CS_RELAY);
        return;
    }
    if (st == LOCCAMP_GW_REFUSED) {
        InterlockedIncrement(&B.n_refused);
        if (!c->retried) { /* LOC_CAMPAIGN "Fuer den Client": bei 1 einmal ein neues Ticket, dann aufgeben */
            if (c->tls) sbtls_free(c->tls);
            c->tls = NULL;
            closesocket(c->s);
            c->s = INVALID_SOCKET;
            c->retried = 1;
            c->dnlen = c->replen = 0;
            loctun_camp_want(B.tun, B.c.campaign, 1);
            cb_logf("LOCCAMP: Zugang lehnt Ticket ab - neues Ticket angefordert");
            cb_goto(c, CS_TICKET);
            return;
        }
        if (c->probe) cb_set_state(LOCCAMP_ST_DOWN, "Ticket abgewiesen");
        cb_close(c, "Ticket abgewiesen", 1);
        return;
    }
    if (st == LOCCAMP_GW_BUSY) {
        if (c->probe) { /* Ratenbremse je Identitaet: Ticket war gueltig */
            cb_set_state(LOCCAMP_ST_READY, "Zugang erreichbar (beschaeftigt)");
            cb_close(c, NULL, 0);
            return;
        }
        cb_close(c, "Zugang beschaeftigt", 1);
        return;
    }
    if (c->probe) cb_set_state(LOCCAMP_ST_DOWN, st == LOCCAMP_GW_UNSUPPORTED ? "Version vom Zugang nicht unterstuetzt"
                                                                             : "Antwort des Zugangs ungueltig");
    cb_close(c, st == LOCCAMP_GW_UNSUPPORTED ? "Version nicht unterstuetzt (Client aktualisieren)" : "Antwort ungueltig",
             1);
}

/* Bytes vom Zugang: erst die 6-B-Antwort, danach ganze Rahmen fuer das Spiel. 0 = weiter, -1 = geschlossen. */
static int cb_pump_down(cconn *c)
{
    for (;;) {
        int r, room = (int)sizeof c->dn - c->dnlen;
        if (c->st == CS_RELAY && c->outlen - c->outoff > (int)sizeof c->out / 2) return 0; /* Spiel liest nicht */
        if (room <= 0) {
            cb_close(c, "Puffer voll", 1);
            return -1;
        }
        r = sbtls_read(c->tls, c->dn + c->dnlen, room);
        if (r == SBTLS_WANT_IO) return 0;
        if (r == SBTLS_CLOSED) {
            c->s_closed = 1;
            return 0;
        }
        if (r < 0) {
            cb_close(c, "tls-lesen", 1);
            return -1;
        }
        c->dnlen += r;
        c->t_act = GetTickCount();
        if (c->st == CS_PRE) {
            int take = LOCCAMP_GW_REPLY - c->replen;
            if (take > c->dnlen) take = c->dnlen;
            memcpy(c->rep + c->replen, c->dn, (size_t)take);
            c->replen += take;
            memmove(c->dn, c->dn + take, (size_t)(c->dnlen - take));
            c->dnlen -= take;
            if (c->replen < LOCCAMP_GW_REPLY) continue;
            cb_on_reply(c);
            if (c->st != CS_RELAY) return c->st == CS_FREE ? -1 : 0;
        }
        while (c->st == CS_RELAY && c->dnlen > 0) {
            int f = loccamp_frame(c->dn, c->dnlen);
            if (f < 0) {
                cb_close(c, "Rahmen zu gross", 1);
                return -1;
            }
            if (f == 0) break;
            if (c->outoff > 0 && c->outlen + f > (int)sizeof c->out) {
                memmove(c->out, c->out + c->outoff, (size_t)(c->outlen - c->outoff));
                c->outlen -= c->outoff;
                c->outoff = 0;
            }
            if (c->outlen + f > (int)sizeof c->out) return 0; /* spaeter weiter */
            memcpy(c->out + c->outlen, c->dn, (size_t)f);
            c->outlen += f;
            memmove(c->dn, c->dn + f, (size_t)(c->dnlen - f));
            c->dnlen -= f;
            c->down += f;
        }
    }
}

static void cb_game_out(cconn *c)
{
    while (c->outoff < c->outlen && c->g != INVALID_SOCKET) {
        int r = send(c->g, (const char *)c->out + c->outoff, c->outlen - c->outoff, 0);
        if (r <= 0) {
            if (WSAGetLastError() != WSAEWOULDBLOCK) c->g_closed = 1;
            break;
        }
        c->outoff += r;
    }
    if (c->outoff >= c->outlen) c->outoff = c->outlen = 0;
}

static void cb_game_in(cconn *c)
{
    char b[4096];
    for (;;) {
        int r = recv(c->g, b, sizeof b, 0);
        if (r == 0) {
            c->g_closed = 1;
            break;
        }
        if (r < 0) {
            if (WSAGetLastError() != WSAEWOULDBLOCK) c->g_closed = 1;
            break;
        }
        if (sbtls_write(c->tls, b, r) != SBTLS_OK) {
            c->g_closed = 1;
            break;
        }
        c->up += r;
        c->t_act = GetTickCount();
    }
    SecureZeroMemory(b, sizeof b);
}

/* Millisekunden seit t, vorzeichenbehaftet mit frischem Tick: Zeitstempel werden auch waehrend eines Schritts
 * gesetzt (Daten empfangen), eine Differenz zu einem vorher gelesenen 'now' liefe sonst unter null ueber. */
static LONG cb_elapsed(DWORD t)
{
    return (LONG)(GetTickCount() - t);
}

static void cb_step(cconn *c)
{
    if (c->st == CS_FREE) return;
    if (c->st == CS_TICKET) {
        char err[24];
        int n = loctun_camp_ticket(B.tun, NULL, LOCCAMP_TICKET_MAX, NULL, err, sizeof err);
        if (n > 0) {
            cb_start_connect(c);
        } else if (n < 0) {
            if (c->probe) cb_set_state(LOCCAMP_ST_DOWN, err[0] ? err : "kein Ticket");
            cb_close(c, err[0] ? err : "kein Ticket", 1);
        } else if (cb_elapsed(c->t_st) > (LONG)CB_TICKET_WAIT_MS) {
            if (c->probe) cb_set_state(LOCCAMP_ST_DOWN, "kein Ticket (Zeit)");
            cb_close(c, "kein Ticket (Zeit)", 1);
        }
        return;
    }
    if (c->st != CS_RELAY && cb_elapsed(c->t_st) > (LONG)CB_CONNECT_MS) {
        if (c->probe) cb_set_state(LOCCAMP_ST_DOWN, "Zugang antwortet nicht");
        cb_close(c, "Zeitlimit Aufbau", 1);
        return;
    }
    if (c->st == CS_TLS) {
        int r = sbtls_handshake(c->tls);
        if (r == SBTLS_OK) {
            cb_send_pre(c);
        } else if (r != SBTLS_WANT_IO) {
            int pin = sbtls_pin_failed(c->tls);
            if (c->probe) cb_set_state(LOCCAMP_ST_DOWN, pin ? "Server-Pruefung fehlgeschlagen (Pin)" : "TLS");
            cb_close(c, pin ? "Pin falsch" : "TLS", 1);
            return;
        }
    }
    if (c->st == CS_PRE || c->st == CS_RELAY) {
        if (sbtls_flush(c->tls) == SBTLS_FAIL) {
            cb_close(c, "tls-senden", 1);
            return;
        }
        if (cb_pump_down(c) < 0 || c->st == CS_FREE) return;
    }
    if (c->st == CS_RELAY) {
        if (!c->g_closed) cb_game_in(c);
        if (sbtls_flush(c->tls) == SBTLS_FAIL) {
            cb_close(c, "tls-senden", 1);
            return;
        }
        cb_game_out(c);
        if (c->g_closed) {
            cb_close(c, "Spiel hat geschlossen", 0);
            return;
        }
        if (c->s_closed && c->outoff >= c->outlen && c->dnlen == 0) {
            cb_close(c, "Server hat geschlossen", 0);
            return;
        }
        if (cb_elapsed(c->t_act) > (LONG)CB_IDLE_MS) cb_close(c, "Leerlauf", 1);
    } else if (c->st == CS_PRE && c->s_closed) {
        if (c->probe) cb_set_state(LOCCAMP_ST_DOWN, "Zugang schliesst vor Antwort");
        cb_close(c, "Zugang schliesst vor Antwort", 1);
    }
}

/* ---------- Annahme ---------- */
static int cb_expected(uint16_t port_net)
{
    int i, ok = 0;
    DWORD now = GetTickCount();
    EnterCriticalSection(&B.cs);
    for (i = 0; i < CB_EXPECT; i++) {
        if (B.expect[i] == port_net && now - B.expect_at[i] < 10000u) {
            B.expect[i] = 0;
            ok = 1;
            break;
        }
    }
    LeaveCriticalSection(&B.cs);
    return ok;
}

static void cb_accept(void)
{
    for (;;) {
        struct sockaddr_in a;
        int l = sizeof a;
        u_long nb = 1;
        cconn *c;
        SOCKET g = accept(B.ls, (struct sockaddr *)&a, &l);
        if (g == INVALID_SOCKET) return;
        if (a.sin_family != AF_INET || (ntohl(a.sin_addr.s_addr) >> 24) != 127 || !cb_expected(a.sin_port)) {
            closesocket(g); /* nur umgelenkte Spiel-Sockets dieses Prozesses */
            cb_logf("LOCCAMP: fremde Verbindung zur Bruecke verworfen");
            continue;
        }
        ioctlsocket(g, FIONBIO, &nb);
        {
            BOOL on = TRUE;
            setsockopt(g, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof on);
        }
        c = cb_new(g, 0);
        if (!c) {
            closesocket(g);
            cb_logf("LOCCAMP: zu viele Kampagnen-Verbindungen gleichzeitig");
            continue;
        }
        InterlockedIncrement(&B.n_conn);
    }
}

static void cb_probe_tick(void)
{
    int i, busy = 0;
    DWORD now = GetTickCount();
    if (B.state == LOCCAMP_ST_READY) return;
    for (i = 0; i < CB_MAX_CONN; i++)
        if (B.cn[i]->st != CS_FREE && B.cn[i]->probe) busy = 1;
    if (busy || (B.probe_at && now - B.probe_at < (B.state == LOCCAMP_ST_DOWN ? CB_REPROBE_MS : 0u))) return;
    if (B.state == LOCCAMP_ST_DOWN) loctun_camp_want(B.tun, B.c.campaign, 0); /* Ablehnung vergessen, neu fragen */
    B.probe_at = now ? now : 1;
    if (!cb_new(INVALID_SOCKET, 1)) B.probe_at = now;
}

static DWORD WINAPI cb_thread(LPVOID arg)
{
    (void)arg;
    while (!B.stop) {
        fd_set rd, wr;
        struct timeval tv = {0, 20000};
        int i, nfd = 0;
        FD_ZERO(&rd);
        FD_ZERO(&wr);
        FD_SET(B.ls, &rd);
        nfd++;
        for (i = 0; i < CB_MAX_CONN; i++) {
            cconn *c = B.cn[i];
            if (c->st == CS_FREE) continue;
            if (c->s != INVALID_SOCKET && nfd < FD_SETSIZE - 2) {
                if (c->st == CS_CONNECT) FD_SET(c->s, &wr);
                else FD_SET(c->s, &rd);
                nfd++;
            }
            if (c->g != INVALID_SOCKET && c->st == CS_RELAY && nfd < FD_SETSIZE - 2) {
                FD_SET(c->g, &rd);
                nfd++;
            }
        }
        select(0, &rd, &wr, NULL, &tv);
        if (FD_ISSET(B.ls, &rd)) cb_accept();
        for (i = 0; i < CB_MAX_CONN; i++) {
            cconn *c = B.cn[i];
            if (c->st == CS_CONNECT && c->s != INVALID_SOCKET) {
                fd_set ex;
                struct timeval z = {0, 0};
                FD_ZERO(&ex);
                FD_SET(c->s, &ex);
                if (FD_ISSET(c->s, &wr)) {
                    if (sbtls_client_new(&c->tls, c->s, B.c.host, (const uint8_t(*)[32])B.pins, B.npins) != SBTLS_OK) {
                        cb_close(c, "tls-start", 1);
                        continue;
                    }
                    cb_goto(c, CS_TLS);
                } else if (select(0, NULL, NULL, &ex, &z) > 0) {
                    B.gw_ok = 0;
                    if (c->probe) cb_set_state(LOCCAMP_ST_DOWN, "Zugang nicht erreichbar");
                    cb_close(c, "Zugang nicht erreichbar", 1);
                    continue;
                }
            }
            cb_step(c);
        }
        cb_probe_tick();
    }
    return 0;
}

/* ---------- API ---------- */
int loccamp_bridge_start(struct loctun *t, const loccamp_cfg *c, loccamp_log_fn log)
{
    struct sockaddr_in a;
    int l = sizeof a, i;
    u_long nb = 1;
    if (B.running || !t || !c || !c->campaign[0]) return 0;
    if (!B.cs_init) {
        InitializeCriticalSection(&B.cs);
        B.cs_init = 1;
    }
    B.log = log;
    B.tun = t;
    B.c = *c;
    if (!B.c.host[0]) strcpy_s(B.c.host, sizeof B.c.host, SB_STEELNET_HOST);
    if (!B.c.port) loccamp_campaign_id(B.c.campaign, &B.c.port);
    for (i = 0; i < CB_MAX_CONN; i++) {
        B.cn[i] = (cconn *)calloc(1, sizeof(cconn));
        if (!B.cn[i]) return 0;
        B.cn[i]->g = B.cn[i]->s = INVALID_SOCKET;
    }
    B.ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (B.ls == INVALID_SOCKET) return 0;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* nur Loopback, nie nach aussen */
    {
        BOOL ex = TRUE;
        setsockopt(B.ls, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&ex, sizeof ex);
    }
    if (bind(B.ls, (struct sockaddr *)&a, sizeof a) != 0 || listen(B.ls, 8) != 0 ||
        getsockname(B.ls, (struct sockaddr *)&a, &l) != 0) {
        closesocket(B.ls);
        B.ls = INVALID_SOCKET;
        return 0;
    }
    ioctlsocket(B.ls, FIONBIO, &nb);
    B.lport = ntohs(a.sin_port);
    B.stop = 0;
    B.state = LOCCAMP_ST_PENDING;
    loctun_camp_want(t, B.c.campaign, 0);
    B.th = CreateThread(NULL, 0, cb_thread, NULL, 0, NULL);
    if (!B.th) {
        closesocket(B.ls);
        B.ls = INVALID_SOCKET;
        B.state = LOCCAMP_ST_OFF;
        return 0;
    }
    B.running = 1;
    cb_logf("LOCCAMP: Bruecke an (Kampagne %s, Zugang Port %u, Dienst 0x43430009 -> virtuelle Adresse)",
            B.c.campaign, (unsigned)B.c.port);
    return 1;
}

void loccamp_bridge_stop(void)
{
    int i;
    if (!B.running) return;
    B.stop = 1;
    WaitForSingleObject(B.th, 2000);
    CloseHandle(B.th);
    B.th = NULL;
    for (i = 0; i < CB_MAX_CONN; i++) {
        if (B.cn[i]) {
            if (B.cn[i]->st != CS_FREE) cb_close(B.cn[i], NULL, 0);
            free(B.cn[i]);
            B.cn[i] = NULL;
        }
    }
    if (B.ls != INVALID_SOCKET) closesocket(B.ls);
    B.ls = INVALID_SOCKET;
    cb_logf("LOCCAMP: Bruecke aus (Verbindungen %ld, angenommen %ld, abgewiesen %ld, Fehler %ld)", B.n_conn, B.n_ok,
            B.n_refused, B.n_failed);
    B.running = 0;
    B.state = LOCCAMP_ST_OFF;
}

int loccamp_bridge_state(void)
{
    return B.running ? (int)B.state : LOCCAMP_ST_OFF;
}

int loccamp_bridge_redirect(SOCKET s, struct sockaddr_in *dst)
{
    struct sockaddr_in a;
    int l = sizeof a, i, slot = 0;
    DWORD now = GetTickCount(), oldest = 0;
    if (!dst || !loccamp_is_vip(dst->sin_addr.s_addr, dst->sin_port)) return 0;
    if (!B.running || B.state != LOCCAMP_ST_READY) return -1;
    if (getsockname(s, (struct sockaddr *)&a, &l) != 0 || a.sin_port == 0) {
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) return -1;
        l = sizeof a;
        if (getsockname(s, (struct sockaddr *)&a, &l) != 0) return -1;
    }
    EnterCriticalSection(&B.cs);
    for (i = 0; i < CB_EXPECT; i++) { /* freier bzw. aeltester Platz */
        if (!B.expect[i]) {
            slot = i;
            break;
        }
        if (now - B.expect_at[i] > oldest) {
            oldest = now - B.expect_at[i];
            slot = i;
        }
    }
    B.expect[slot] = a.sin_port;
    B.expect_at[slot] = now;
    LeaveCriticalSection(&B.cs);
    dst->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dst->sin_port = htons(B.lport);
    return 1;
}

void loccamp_bridge_counts(long *conns, long *ok, long *refused, long *failed)
{
    if (conns) *conns = B.n_conn;
    if (ok) *ok = B.n_ok;
    if (refused) *refused = B.n_refused;
    if (failed) *failed = B.n_failed;
}
