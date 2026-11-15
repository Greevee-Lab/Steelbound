/* loc_net_core.c - W286 LOC-SYSLINK: Netz-Kern (siehe loc_net_core.h).
 * Sicherheit: bindet nur an die eigene (Loopback- bzw. private) Adresse, sendet nur an
 * Loopback/private Ziele, verwirft Pakete/Verbindungen fremder Absender. */
#define _CRT_RAND_S
#include "loc_net_core.h"
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LN_GAME_PORT_LO 1000u
#define LN_GAME_PORT_HI 1999u
#define LN_CLAIM_OFF 200u /* Spielport 1200 nutzt LoC nicht */
#define LN_IPPROTO_VDP 0xFE
#define LN_SIO_UDP_CONNRESET 0x9800000Cu /* _WSAIOW(IOC_VENDOR, 12) */

static void ln_logf(const locnet *n, const char *fmt, ...)
{
    char b[512];
    va_list ap;
    if (!n || !n->log) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    n->log(b);
}

const char *locnet_ip_str(uint32_t ip_net, char buf[16])
{
    const uint8_t *b = (const uint8_t *)&ip_net;
    snprintf(buf, 16, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return buf;
}

int locnet_ip_allowed(uint32_t ip_net)
{
    uint32_t h = ntohl(ip_net);
    if ((h >> 24) == 127u) {
        return 1;
    }
    if ((h >> 24) == 10u) {
        return 1;
    }
    if ((h & 0xFFF00000u) == 0xAC100000u) { /* 172.16/12 */
        return 1;
    }
    if ((h & 0xFFFF0000u) == 0xC0A80000u) { /* 192.168/16 */
        return 1;
    }
    if ((h & 0xFFFF0000u) == 0xA9FE0000u) { /* 169.254/16 */
        return 1;
    }
    return 0;
}

static int ln_parse_ip(const char *s, uint32_t *out)
{
    struct in_addr a;
    if (inet_pton(AF_INET, s, &a) != 1) {
        return 0;
    }
    *out = a.s_addr;
    return 1;
}

static uint16_t ln_base_of(const locnet *n, uint32_t ip_net)
{
    int i;
    for (i = 0; i < n->npeers; i++) {
        if (n->peers[i].ip == ip_net) {
            return n->peers[i].base;
        }
    }
    return n->base;
}

uint16_t locnet_port_out(const locnet *n, uint32_t ip_net, uint16_t gp)
{
    if (gp >= LN_GAME_PORT_LO && gp <= LN_GAME_PORT_HI) {
        return (uint16_t)(ln_base_of(n, ip_net) + (gp - LN_GAME_PORT_LO));
    }
    return gp;
}

uint16_t locnet_port_in(const locnet *n, uint32_t ip_net, uint16_t hp)
{
    uint16_t b = ln_base_of(n, ip_net);
    if (hp >= b && hp <= b + (LN_GAME_PORT_HI - LN_GAME_PORT_LO)) {
        return (uint16_t)(LN_GAME_PORT_LO + (hp - b));
    }
    return hp;
}

static SOCKET ln_claim(uint32_t ip_net, uint16_t port)
{
    struct sockaddr_in a;
    BOOL on = TRUE;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        return s;
    }
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&on, sizeof on);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = ip_net;
    a.sin_port = htons(port);
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

static void ln_add_peer(locnet *n, uint32_t ip, uint16_t base)
{
    char b[16];
    int i;
    if (ip == n->self_ip && base == n->base) {
        return;
    }
    if (!locnet_ip_allowed(ip) || ip == INADDR_BROADCAST) {
        ln_logf(n, "LOCNET: Mitspieler %s abgelehnt (nur Loopback/privat erlaubt)", locnet_ip_str(ip, b));
        return;
    }
    for (i = 0; i < n->npeers; i++) {
        if (n->peers[i].ip == ip) {
            return;
        }
    }
    if (n->npeers < LOCNET_MAX_PEERS) {
        n->peers[n->npeers].ip = ip;
        n->peers[n->npeers].base = base;
        n->npeers++;
    }
}

int locnet_init(locnet *n, const char *addr, int base, const char *peers, int rtt_ms, locnet_log_fn log)
{
    WSADATA wd;
    char ipb[16];
    int i;
    memset(n, 0, sizeof *n);
    n->claim = INVALID_SOCKET;
    n->log = log;
    n->rtt_ms = (rtt_ms > 0 && rtt_ms < 5000) ? rtt_ms : 10;
    if (base < 1000 || base > 64000) {
        ln_logf(n, "LOCNET: loc_net_port=%d ausserhalb 1000..64000 - Basis 1000", base);
        base = 1000;
    }
    n->base = (uint16_t)base;
    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) {
        ln_logf(n, "LOCNET: Host-WSAStartup fehlgeschlagen (%d) - Netz aus", WSAGetLastError());
        return 0;
    }
    if (!addr || !addr[0] || _stricmp(addr, "auto") == 0) {
        n->auto_addr = 1;
        for (i = 2; i <= 9 && n->claim == INVALID_SOCKET; i++) {
            uint32_t ip = htonl(0x7F000000u | (uint32_t)i);
            n->claim = ln_claim(ip, (uint16_t)(n->base + LN_CLAIM_OFF));
            if (n->claim != INVALID_SOCKET) {
                n->self_ip = ip;
            }
        }
        if (n->claim == INVALID_SOCKET) {
            ln_logf(n, "LOCNET: keine freie Adresse in 127.0.0.2-127.0.0.9 (Basis %u) - Netz aus", n->base);
            return 0;
        }
    } else {
        if (!ln_parse_ip(addr, &n->self_ip) || !locnet_ip_allowed(n->self_ip)) {
            ln_logf(n, "LOCNET: loc_net_addr=%s ungueltig (nur Loopback/privat) - Netz aus", addr);
            return 0;
        }
        n->claim = ln_claim(n->self_ip, (uint16_t)(n->base + LN_CLAIM_OFF));
        if (n->claim == INVALID_SOCKET) {
            ln_logf(n, "LOCNET: %s:%u belegt (zweite Instanz mit gleicher Adresse?) - Netz aus", addr,
                    n->base + LN_CLAIM_OFF);
            return 0;
        }
    }
    if (!peers || !peers[0] || _stricmp(peers, "auto") == 0) {
        for (i = 2; i <= 9; i++) {
            ln_add_peer(n, htonl(0x7F000000u | (uint32_t)i), n->base);
        }
    } else {
        char tmp[512], *ctx = NULL, *tok;
        strncpy_s(tmp, sizeof tmp, peers, _TRUNCATE);
        for (tok = strtok_s(tmp, ",; ", &ctx); tok; tok = strtok_s(NULL, ",; ", &ctx)) {
            char *c = strchr(tok, ':');
            uint32_t ip;
            int pb = n->base;
            if (c) {
                *c = 0;
                pb = atoi(c + 1);
                if (pb < 1000 || pb > 64000) {
                    ln_logf(n, "LOCNET: Mitspieler %s: Basis %d ungueltig - uebergangen", tok, pb);
                    continue;
                }
            }
            if (!ln_parse_ip(tok, &ip)) {
                ln_logf(n, "LOCNET: Mitspieler \"%s\" ist keine IPv4 - uebergangen", tok);
                continue;
            }
            ln_add_peer(n, ip, (uint16_t)pb);
        }
    }
    n->ready = 1;
    ln_logf(n, "LOCNET: bereit, eigene Adresse %s%s, Portbasis %u (TCP %u, UDP %u/%u, VDP %u), %d Mitspieler-Adressen",
            locnet_ip_str(n->self_ip, ipb), n->auto_addr ? " (auto)" : "", n->base, n->base, n->base + 201,
            n->base + 202, n->base + 203, n->npeers);
    return 1;
}

void locnet_shutdown(locnet *n)
{
    if (n->claim != INVALID_SOCKET) {
        closesocket(n->claim);
        n->claim = INVALID_SOCKET;
    }
    if (n->ready) {
        n->ready = 0;
        WSACleanup();
    }
}

/* Erste Sichtung je (Richtung, Port-Art, Rahmentyp u16) protokollieren - nur Typ und
 * Laenge, nie Inhalte (Rahmen tragen Pilot-/Eingabedaten). */
static void ln_seen(locnet *n, SOCKET s, int out, const char *buf, int len)
{
    static volatile LONG tab[128];
    static volatile LONG cnt;
    struct sockaddr_in me;
    int l = sizeof me, typ, i, sock_type = 0, tl = sizeof sock_type;
    LONG key;
    if (!buf || len < 2) {
        return;
    }
    typ = (uint8_t)buf[0] | ((uint8_t)buf[1] << 8);
    getsockopt(s, SOL_SOCKET, SO_TYPE, (char *)&sock_type, &tl);
    me.sin_port = 0;
    getsockname(s, (struct sockaddr *)&me, &l);
    key = (LONG)((out << 30) | ((sock_type & 3) << 28) | ((ntohs(me.sin_port) & 0xFFF) << 16) | (typ & 0xFFFF));
    if (sock_type == SOCK_STREAM) {
        key &= ~(0xFFF << 16); /* TCP: Port der Gegenstelle egal */
    }
    for (i = 0; i < cnt && i < 128; i++) {
        if (tab[i] == key) {
            return;
        }
    }
    i = InterlockedIncrement(&cnt) - 1;
    if (i < 128) {
        tab[i] = key;
        ln_logf(n, "LOCNETMSG: %s %s Typ 0x%X Laenge %d (erste Sichtung)", out ? "gesendet" : "empfangen",
                sock_type == SOCK_STREAM ? "TCP" : "UDP", typ, len);
    }
}

/* ---------- Sockets ---------- */
/* Ungebundene Sockets (TCP-connect, UDP-sendto) an die eigene Adresse binden, sonst
 * waehlt Windows 127.0.0.1 als Absender und der Mitspieler sieht eine falsche Identitaet. */
static void ln_bind_self(locnet *n, SOCKET s)
{
    struct sockaddr_in me;
    int l = sizeof me;
    if (getsockname(s, (struct sockaddr *)&me, &l) == 0 && me.sin_family == AF_INET &&
        me.sin_addr.s_addr != INADDR_ANY) {
        return;
    }
    memset(&me, 0, sizeof me);
    me.sin_family = AF_INET;
    me.sin_addr.s_addr = n->self_ip;
    bind(s, (struct sockaddr *)&me, sizeof me);
}

SOCKET locnet_socket(locnet *n, int af, int type, int proto)
{
    SOCKET s;
    if (proto == LN_IPPROTO_VDP) { /* VDP (Sprache, Port 1203): zwischen zwei HLE-Clients reines UDP */
        proto = IPPROTO_UDP;
    }
    s = socket(af, type, proto);
    if (s != INVALID_SOCKET) {
        InterlockedIncrement(&n->st.sockets);
        if (type == SOCK_DGRAM) {
            /* Windows: ICMP "Port unerreichbar" (Unicast an Mitspieler ohne Spiel) liesse das
             * naechste recvfrom mit WSAECONNRESET scheitern - die Xbox kennt das nicht. */
            BOOL off = FALSE;
            DWORD got = 0;
            WSAIoctl(s, LN_SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &got, NULL, NULL);
        }
    }
    return s;
}

void locnet_note_stream(locnet *n, SOCKET s, int out, const char *buf, int len)
{
    ln_seen(n, s, out, buf, len);
}

int locnet_closesocket(locnet *n, SOCKET s)
{
    (void)n;
    return closesocket(s);
}

int locnet_setsockopt(locnet *n, SOCKET s, int level, int opt, const char *val, int len)
{
    int r, e;
    if (level == SOL_SOCKET && opt == SO_BROADCAST) {
        return 0; /* kein echter Broadcast: Unicast an die Mitspielerliste */
    }
    r = setsockopt(s, level, opt, val, len);
    if (r != 0) {
        e = WSAGetLastError();
        ln_logf(n, "LOCNET: setsockopt(level 0x%X, opt 0x%X) -> Fehler %d (ignoriert)", level, opt, e);
        r = 0; /* Xbox-eigene Optionen gibt es auf dem Host nicht */
    }
    return r;
}

int locnet_bind(locnet *n, SOCKET s, const struct sockaddr *a, int len)
{
    struct sockaddr_in in;
    char ipb[16];
    int r, e;
    if (!a || len < (int)sizeof in || a->sa_family != AF_INET) {
        return bind(s, a, len);
    }
    memcpy(&in, a, sizeof in);
    if (in.sin_addr.s_addr != INADDR_ANY && in.sin_addr.s_addr != n->self_ip) {
        ln_logf(n, "LOCNET: bind auf fremde Adresse %s -> eigene Adresse", locnet_ip_str(in.sin_addr.s_addr, ipb));
    }
    in.sin_addr.s_addr = n->self_ip; /* nie INADDR_ANY: keine offenen Ports nach aussen */
    in.sin_port = htons(locnet_port_out(n, n->self_ip, ntohs(in.sin_port)));
    r = bind(s, (struct sockaddr *)&in, sizeof in);
    e = r ? WSAGetLastError() : 0;
    ln_logf(n, "LOCNET: bind Spielport %u -> %s:%u%s", ntohs(((const struct sockaddr_in *)a)->sin_port),
            locnet_ip_str(n->self_ip, ipb), ntohs(in.sin_port), r ? " FEHLER" : "");
    if (r) {
        WSASetLastError(e);
    }
    return r;
}

int locnet_connect(locnet *n, SOCKET s, const struct sockaddr *a, int len)
{
    struct sockaddr_in in;
    char ipb[16];
    int r, e;
    if (!a || len < (int)sizeof in || a->sa_family != AF_INET) {
        return connect(s, a, len);
    }
    memcpy(&in, a, sizeof in);
    if (!locnet_ip_allowed(in.sin_addr.s_addr)) {
        ln_logf(n, "LOCNET: connect zu %s verweigert (nicht lokal)", locnet_ip_str(in.sin_addr.s_addr, ipb));
        WSASetLastError(WSAEHOSTUNREACH);
        return SOCKET_ERROR;
    }
    in.sin_port = htons(locnet_port_out(n, in.sin_addr.s_addr, ntohs(in.sin_port)));
    ln_bind_self(n, s);
    InterlockedIncrement(&n->st.connects);
    r = connect(s, (struct sockaddr *)&in, sizeof in);
    e = r ? WSAGetLastError() : 0;
    ln_logf(n, "LOCNET: connect %s:%u (Spielport %u) -> %d (%d)", locnet_ip_str(in.sin_addr.s_addr, ipb),
            ntohs(in.sin_port), ntohs(((const struct sockaddr_in *)a)->sin_port), r, e);
    if (r) {
        WSASetLastError(e);
    }
    return r;
}

SOCKET locnet_accept(locnet *n, SOCKET s, struct sockaddr *a, int *len)
{
    struct sockaddr_in in;
    int l = sizeof in;
    char ipb[16];
    SOCKET c = accept(s, (struct sockaddr *)&in, &l);
    if (c == INVALID_SOCKET) {
        return c;
    }
    if (!locnet_ip_allowed(in.sin_addr.s_addr)) {
        ln_logf(n, "LOCNET: Verbindung von %s verworfen (nicht lokal)", locnet_ip_str(in.sin_addr.s_addr, ipb));
        closesocket(c);
        InterlockedIncrement(&n->st.drop);
        WSASetLastError(WSAEWOULDBLOCK);
        return INVALID_SOCKET;
    }
    in.sin_port = htons(locnet_port_in(n, in.sin_addr.s_addr, ntohs(in.sin_port)));
    if (a && len && *len >= (int)sizeof in) {
        memcpy(a, &in, sizeof in);
        *len = sizeof in;
    }
    InterlockedIncrement(&n->st.accepts);
    ln_logf(n, "LOCNET: accept von %s", locnet_ip_str(in.sin_addr.s_addr, ipb));
    return c;
}

int locnet_sendto(locnet *n, SOCKET s, const char *buf, int len, int flags, const struct sockaddr *to, int tolen)
{
    struct sockaddr_in in;
    uint16_t gp;
    int i, r;
    if (!to || tolen < (int)sizeof in || to->sa_family != AF_INET) {
        r = sendto(s, buf, len, flags, to, tolen); /* verbundener Socket (TCP 1000) */
        if (r > 0) {
            InterlockedIncrement(&n->st.tx);
            ln_seen(n, s, 1, buf, r);
        }
        return r;
    }
    memcpy(&in, to, sizeof in);
    gp = ntohs(in.sin_port);
    ln_bind_self(n, s);
    if (in.sin_addr.s_addr == INADDR_BROADCAST) {
        /* System-Link-Suche/Host-Info: Unicast an jeden Mitspieler */
        for (i = 0; i < n->npeers; i++) {
            in.sin_addr.s_addr = n->peers[i].ip;
            in.sin_port = htons((uint16_t)(gp >= LN_GAME_PORT_LO && gp <= LN_GAME_PORT_HI
                                               ? n->peers[i].base + (gp - LN_GAME_PORT_LO)
                                               : gp));
            sendto(s, buf, len, flags, (struct sockaddr *)&in, sizeof in);
        }
        InterlockedIncrement(&n->st.bc);
        ln_seen(n, s, 1, buf, len);
        return len; /* UDP: ein Broadcast gilt immer als gesendet */
    }
    if (!locnet_ip_allowed(in.sin_addr.s_addr)) {
        InterlockedIncrement(&n->st.drop);
        return len; /* still verwerfen (nie ins Internet) */
    }
    in.sin_port = htons(locnet_port_out(n, in.sin_addr.s_addr, gp));
    r = sendto(s, buf, len, flags, (struct sockaddr *)&in, sizeof in);
    if (r > 0) {
        InterlockedIncrement(&n->st.tx);
        ln_seen(n, s, 1, buf, r);
    }
    return r;
}

int locnet_recvfrom(locnet *n, SOCKET s, char *buf, int len, int flags, struct sockaddr *from, int *fromlen)
{
    int tries;
    for (tries = 0; tries < 16; tries++) {
        struct sockaddr_in in;
        int l = sizeof in;
        int r = recvfrom(s, buf, len, flags, (struct sockaddr *)&in, &l);
        if (r == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAECONNRESET) {
                continue; /* Rest-Meldung einer ICMP-Antwort (UDP), wie Xbox ignorieren */
            }
            return r;
        }
        if (l >= (int)sizeof in && in.sin_family == AF_INET) {
            if (!locnet_ip_allowed(in.sin_addr.s_addr)) {
                InterlockedIncrement(&n->st.drop);
                continue; /* fremder Absender */
            }
            in.sin_port = htons(locnet_port_in(n, in.sin_addr.s_addr, ntohs(in.sin_port)));
        }
        if (from && fromlen) {
            int c = *fromlen < l ? *fromlen : l;
            if (c > 0) {
                memcpy(from, &in, (size_t)c);
            }
            *fromlen = l;
        }
        if (r > 0) {
            InterlockedIncrement(&n->st.rx);
            ln_seen(n, s, 0, buf, r);
        }
        return r;
    }
    WSASetLastError(WSAEWOULDBLOCK);
    return SOCKET_ERROR;
}

/* ---------- XNet ---------- */
void locnet_xnaddr(const locnet *n, uint32_t ip_net, uint8_t out[LOCNET_XNADDR_SIZE])
{
    const uint8_t *b = (const uint8_t *)&ip_net;
    (void)n;
    memset(out, 0, LOCNET_XNADDR_SIZE);
    memcpy(out + 0, &ip_net, 4); /* ina */
    /* inaOnline = 0, wPortOnline = 0 (kein Live) */
    out[10] = 0x00; /* abEnet: lokal verwaltete, aus der Adresse abgeleitete MAC */
    out[11] = 0x50;
    out[12] = 0xF2;
    out[13] = b[1];
    out[14] = b[2];
    out[15] = b[3];
}

uint32_t locnet_xnaddr_ip(const uint8_t xna[LOCNET_XNADDR_SIZE])
{
    uint32_t ip;
    memcpy(&ip, xna, 4);
    return ip;
}

void locnet_random(uint8_t *p, unsigned len)
{
    unsigned i;
    for (i = 0; i < len; i++) {
        unsigned v = 0;
        rand_s(&v);
        p[i] = (uint8_t)v;
    }
}

void locnet_create_key(uint8_t kid[8], uint8_t key[16])
{
    locnet_random(kid, 8);
    locnet_random(key, 16);
    kid[0] &= 0x0F; /* XNKID-Typ System Link (oberes Nibble 0) */
}

/* XNQOS: u32 cxnqos, u32 cxnqosPending, XNQOSINFO[cxnqos] (je 24 B):
 * u8 bFlags, u8 res, u16 cProbesXmit, u16 cProbesRecv, u16 cbData, u32 pbData,
 * u16 wRttMinInMsecs, u16 wRttMedInMsecs, u32 dwUpBitsPerSec, u32 dwDnBitsPerSec. */
void *locnet_qos_alloc(const locnet *n, unsigned cnt, unsigned probes)
{
    uint8_t *q;
    unsigned i;
    if (cnt == 0) {
        cnt = 1;
    }
    if (cnt > 64) {
        cnt = 64;
    }
    q = (uint8_t *)calloc(1, 8 + 24 * cnt);
    if (!q) {
        return NULL;
    }
    ((uint32_t *)q)[0] = cnt;
    ((uint32_t *)q)[1] = 0;
    for (i = 0; i < cnt; i++) {
        uint8_t *e = q + 8 + 24 * i;
        e[0] = 0x03; /* COMPLETE | TARGET_CONTACTED */
        *(uint16_t *)(e + 2) = (uint16_t)probes;
        *(uint16_t *)(e + 4) = (uint16_t)probes;
        *(uint16_t *)(e + 12) = (uint16_t)n->rtt_ms;
        *(uint16_t *)(e + 14) = (uint16_t)n->rtt_ms;
        *(uint32_t *)(e + 16) = 10000000u;
        *(uint32_t *)(e + 20) = 10000000u;
    }
    return q;
}

void locnet_qos_free(void *q)
{
    free(q);
}
