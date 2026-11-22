/* loc_net_core.c - W286 LOC-SYSLINK: Netz-Kern (siehe loc_net_core.h).
 * Sicherheit: bindet nur an die eigene (Loopback- bzw. private) Adresse, sendet nur an
 * Loopback/private Ziele, verwirft Pakete/Verbindungen fremder Absender. */
#define _CRT_RAND_S
#include "loc_net_core.h"
#include "loc_netsim.h"
#include "loc_tun.h"
#include "loc_guard.h"
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LN_GAME_PORT_LO 1000u
#define LN_GAME_PORT_HI 1999u
#define LN_CLAIM_OFF 200u /* Spielport 1200 nutzt LoC nicht */
#define LN_IPPROTO_VDP 0xFE
#define LN_SIO_UDP_CONNRESET 0x9800000Cu /* _WSAIOW(IOC_VENDOR, 12) */

/* W339 SEC-A3 (SECURITY.md §6.6 Punkt 3): 1, sobald Bytes eines Mitspielers einen Puffer des Spiels erreicht haben
 * (Gastfilter bestanden). Bleibt bis zum Prozessende gesetzt (Rahmeninhalte liegen danach im Gastspeicher). Der
 * VEH "Absturz = Ende" (emu_main.c) beendet ab dann jede Ausnahme im Gastcode statt sie dem SEH des Spiels zu geben. */
volatile LONG g_locnet_session;
static void ln_logf(const locnet *n, const char *fmt, ...);
static void ln_session_mark(const locnet *n)
{
    if (!g_locnet_session && InterlockedExchange(&g_locnet_session, 1) == 0) {
        ln_logf(n, "LOCSEC: Netzsitzung - Absturz = Ende aktiv (Gastcode-Ausnahmen beenden die Sitzung)");
    }
}

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
    if (n->tun && n->tun_base && ip_net != n->self_ip) {
        return n->tun_base; /* W373: Geister oeffentlicher Lobbys (vip:Stellvertreter) wie Mitspieler */
    }
    return n->base;
}

/* W329: gebundene Spiel-UDP-Ports merken (Socket an self_ip, Typ DGRAM). on = 0: vergessen. */
static void ln_udp_mark(locnet *n, SOCKET s, int on)
{
    struct sockaddr_in me;
    int l = sizeof me, st = 0, tl = sizeof st, e = WSAGetLastError();
    uint16_t p;
    if (getsockopt(s, SOL_SOCKET, SO_TYPE, (char *)&st, &tl) == 0 && st == SOCK_DGRAM &&
        getsockname(s, (struct sockaddr *)&me, &l) == 0 && me.sin_family == AF_INET &&
        me.sin_addr.s_addr == n->self_ip && me.sin_port != 0) {
        p = ntohs(me.sin_port);
        if (on) {
            InterlockedOr(&n->udp_bound[p >> 5], (LONG)(1u << (p & 31)));
        } else {
            InterlockedAnd(&n->udp_bound[p >> 5], (LONG)~(1u << (p & 31)));
        }
    }
    WSASetLastError(e);
}

int locnet_udp_bound(const locnet *n, uint16_t host_port)
{
    return (n->udp_bound[host_port >> 5] >> (host_port & 31)) & 1;
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

void locnet_peer_add(locnet *n, uint32_t ip, uint16_t base)
{
    int i;
    for (i = 0; i < n->npeers; i++) {
        if (n->peers[i].ip == ip) {
            n->peers[i].base = base;
            return;
        }
    }
    if (n->npeers < LOCNET_MAX_PEERS && locnet_ip_allowed(ip)) {
        n->peers[n->npeers].ip = ip;
        n->peers[n->npeers].base = base;
        MemoryBarrier();
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
    if (addr && _stricmp(addr, "random") == 0) { /* W324: eindeutig auch zwischen Rechnern (Transport) */
        for (i = 0; i < 32 && n->claim == INVALID_SOCKET; i++) {
            unsigned r = 0;
            uint32_t ip;
            rand_s(&r);
            ip = htonl(0x7F400000u | (r & 0x003FFFFFu));
            if ((ntohl(ip) & 0xFFu) == 0u || (ntohl(ip) & 0xFFu) == 255u) {
                continue;
            }
            n->claim = ln_claim(ip, (uint16_t)(n->base + LN_CLAIM_OFF));
            if (n->claim != INVALID_SOCKET) {
                n->self_ip = ip;
            }
        }
        n->auto_addr = 1;
        if (n->claim == INVALID_SOCKET) {
            ln_logf(n, "LOCNET: keine freie Zufallsadresse in 127.64/10 - Netz aus");
            return 0;
        }
    } else if (!addr || !addr[0] || _stricmp(addr, "auto") == 0) {
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
    if (peers && _stricmp(peers, "none") == 0) {
        /* W324: Mitspieler kommen vom Transport (locnet_peer_add) */
    } else if (!peers || !peers[0] || _stricmp(peers, "auto") == 0) {
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
    if (n->tun) { /* W324: Transport vor der Stufe beenden (sendet ueber sie) */
        loctun_stop(n->tun);
        n->tun = NULL;
    }
    if (n->sim) { /* W322: Rest zustellen, Thread beenden */
        locnetsim_stop(n->sim);
        n->sim = NULL;
    }
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
static void ln_seen_one(locnet *n, int out, int typ, int len, int sock_type, uint16_t lport)
{
    static volatile LONG tab[128];
    static volatile LONG cnt;
    int i;
    LONG key;
    if (typ < 256) { /* W297: Typ-Merkliste fuer Testfolgen (loc_steps rx:/tx:) */
        InterlockedOr(&n->st.typ_seen[out ? 1 : 0][typ >> 5], (LONG)(1u << (typ & 31)));
        if (out && typ == 0x34) {
            InterlockedExchange(&n->st.last34_ms, (LONG)(GetTickCount() | 1u));
            if (n->sim) {
                locnetsim_mark_start(n->sim, GetTickCount()); /* W322: Lockstep-Start (nur erster) fuer Stufen */
            }
        }
    }
    key = (LONG)((out << 30) | ((sock_type & 3) << 28) | ((lport & 0xFFF) << 16) | (typ & 0xFFFF));
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

/* W324: TCP 1000 ist ein Bytestrom aus Rahmen [u16 Typ] ohne Laengenfeld, Laenge fest je Typ (LOC_RE1 §6.4,
 * Tabelle FUN_0004EFE0). Ein recv kann mehrere Rahmen oder Teile davon liefern (Netz, eigener Transport);
 * fuer die Typ-Merkliste der Testfolgen wird der Strom daher je Socket und Richtung zerlegt. 0 = unbekannt. */
static int ln_tcp_frame_len(int typ)
{
    switch (typ) {
    case 0x0A: return 100;
    case 0x0B: case 0x0D: return 0x308;
    case 0x0C: return 0x34;
    case 0x0E: case 0x30: return 0x1A0;
    case 0x0F: case 0x10: case 0x11: case 0x12: return 4;
    case 0x13: return 0x16C;
    case 0x14: return 0x10;
    case 0x1B: return 0x4F8;
    case 0x20: case 0x2E: return 0x1008;
    case 0x22: return 0x100C;
    case 0x27: return 0x2A8;
    case 0x2C: case 0x2D: return 0x10;
    case 0x31: case 0x32: case 0x37: return 4;
    case 0x34: return 26; /* 12 + 14 n, n = 1 */
    case 0x35: case 0x36: return 8;
    default: return 0;
    }
}

typedef struct ln_walk {
    SOCKET s;
    uint32_t rem; /* Restbytes des laufenden Rahmens */
    int half;     /* 1 = erstes Typbyte liegt in lo */
    uint8_t lo;
} ln_walk;
static ln_walk s_walk[2][32];
static volatile LONG s_walk_lock;
/* W334: Gastfilter-Zustand je Spiel-TCP-Socket (SECURITY.md §6, letzte Linie in den HLE-Hooks) */
typedef struct ln_gw {
    SOCKET s;
    lguard_walk w;
    int dead; /* Verstoss: Verbindung getrennt, recv meldet WSAECONNRESET */
    ULONGLONG undecided_since; /* seit wann ein Rahmenkopf unentschieden ansteht (0 = keiner) */
} ln_gw;
#define LN_GW_MAX 64 /* W335: 32 -> 64 (Ueberlauf jetzt fail closed, ln_gw_get) */
static ln_gw s_gw[LN_GW_MAX];

static void ln_walk_lock(void)
{
    while (InterlockedExchange(&s_walk_lock, 1)) {
        YieldProcessor();
    }
}

/* Zustand eines Sockets vergessen (neue Verbindung, Schliessen). */
static void ln_walk_reset(SOCKET s)
{
    int d, i;
    ln_walk_lock();
    for (d = 0; d < 2; d++) {
        for (i = 0; i < 32; i++) {
            if (s_walk[d][i].s == s) {
                memset(&s_walk[d][i], 0, sizeof s_walk[d][i]);
            }
        }
    }
    for (i = 0; i < LN_GW_MAX; i++) {
        if (s_gw[i].s == s) memset(&s_gw[i], 0, sizeof s_gw[i]);
    }
    InterlockedExchange(&s_walk_lock, 0);
}

static void ln_seen(locnet *n, SOCKET s, int out, const char *buf, int len)
{
    struct sockaddr_in me;
    int l = sizeof me, sock_type = 0, tl = sizeof sock_type, pos = 0, i, fr = -1;
    ln_walk *w = NULL;
    if (!buf || len < 1) {
        return;
    }
    getsockopt(s, SOL_SOCKET, SO_TYPE, (char *)&sock_type, &tl);
    me.sin_port = 0;
    getsockname(s, (struct sockaddr *)&me, &l);
    if (sock_type != SOCK_STREAM) { /* UDP: ein Rahmen je Datagramm */
        if (len >= 2) {
            ln_seen_one(n, out, (uint8_t)buf[0] | ((uint8_t)buf[1] << 8), len, sock_type, ntohs(me.sin_port));
        }
        return;
    }
    ln_walk_lock();
    for (i = 0; i < 32; i++) {
        if (s_walk[out ? 1 : 0][i].s == s) {
            w = &s_walk[out ? 1 : 0][i];
            break;
        }
        if (fr < 0 && s_walk[out ? 1 : 0][i].s == 0) {
            fr = i;
        }
    }
    if (!w) {
        w = &s_walk[out ? 1 : 0][fr >= 0 ? fr : (int)((uintptr_t)s % 32)];
        memset(w, 0, sizeof *w);
        w->s = s;
    }
    while (pos < len) {
        int typ, fl;
        if (w->rem > 0) {
            int k = (int)w->rem < len - pos ? (int)w->rem : len - pos;
            w->rem -= (uint32_t)k;
            pos += k;
            continue;
        }
        if (w->half) {
            typ = w->lo | ((uint8_t)buf[pos] << 8);
            w->half = 0;
            pos += 1;
        } else if (len - pos >= 2) {
            typ = (uint8_t)buf[pos] | ((uint8_t)buf[pos + 1] << 8);
            pos += 2;
        } else {
            w->lo = (uint8_t)buf[pos];
            w->half = 1;
            break;
        }
        fl = ln_tcp_frame_len(typ);
        ln_seen_one(n, out, typ, fl ? fl : len - pos + 2, sock_type, ntohs(me.sin_port));
        if (!fl) { /* unbekannter Typ: Rest dieses Puffers ueberspringen, naechster beginnt neu (wie vor W324) */
            w->rem = 0;
            break;
        }
        w->rem = (uint32_t)(fl - 2);
    }
    InterlockedExchange(&s_walk_lock, 0);
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
    if (bind(s, (struct sockaddr *)&me, sizeof me) == 0) {
        ln_udp_mark(n, s, 1); /* W329 */
    }
}

/* W322: Die Spielrahmen (0x34: 26 B je Tick) sind klein. Mit Nagle haelt der Host-TCP-Stapel jeden
 * weiteren kleinen Rahmen zurueck, bis das vorige Segment bestaetigt ist (bis zu einer RTT). Auf
 * Loopback/LAN unmerklich, ueber eine Strecke mit Laufzeit (z. B. VPN mit privaten Adressen) addiert
 * es sich zur Eingabeverzoegerung (W322 §20). Die Spiel-Logik bleibt unberuehrt. */
static void ln_nodelay(locnet *n, SOCKET s)
{
    int st = 0, tl = sizeof st, e = WSAGetLastError();
    BOOL on = TRUE;
    if (n->nodelay && getsockopt(s, SOL_SOCKET, SO_TYPE, (char *)&st, &tl) == 0 && st == SOCK_STREAM) {
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof on);
    }
    WSASetLastError(e);
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
    InterlockedCompareExchange(&n->tcp_listen, 0, (LONG)s); /* W409 */
    ln_walk_reset(s); /* W324 */
    ln_udp_mark(n, s, 0); /* W329 */
    if (n->sim && locnetsim_close(n->sim, s)) {
        return 0; /* W322: offene Sendungen noch zustellen, dann schliesst die Stufe */
    }
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
    if (r == 0) {
        ln_udp_mark(n, s, 1); /* W329 */
    }
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
    if (n->tun && in.sin_addr.s_addr != n->self_ip && !loctun_is_peer(n->tun, in.sin_addr.s_addr)) {
        /* W324: mit Transport nur zu verbundenen Mitspielern (sonst ginge es am Transport vorbei) */
        ln_logf(n, "LOCNET: connect zu %s verweigert (kein verbundener Mitspieler)", locnet_ip_str(in.sin_addr.s_addr, ipb));
        WSASetLastError(WSAEHOSTUNREACH);
        return SOCKET_ERROR;
    }
    in.sin_port = htons(locnet_port_out(n, in.sin_addr.s_addr, ntohs(in.sin_port)));
    ln_bind_self(n, s);
    ln_nodelay(n, s);
    InterlockedIncrement(&n->st.connects);
    ln_walk_reset(s); /* W324: neuer Strom */
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
    if (n->tun && in.sin_addr.s_addr != n->self_ip && !loctun_is_peer(n->tun, in.sin_addr.s_addr)) {
        /* W334 (SECURITY.md §6.5): mit Transport nur Verbindungen der Stellvertreter verbundener Mitspieler */
        ln_logf(n, "LOCNET: Verbindung von %s verworfen (kein verbundener Mitspieler)", locnet_ip_str(in.sin_addr.s_addr, ipb));
        closesocket(c);
        InterlockedIncrement(&n->st.drop);
        WSASetLastError(WSAEWOULDBLOCK);
        return INVALID_SOCKET;
    }
    in.sin_port = htons(locnet_port_in(n, in.sin_addr.s_addr, ntohs(in.sin_port)));
    ln_nodelay(n, c);
    ln_walk_reset(c); /* W324: neuer Strom */
    if (a && len && *len >= (int)sizeof in) {
        memcpy(a, &in, sizeof in);
        *len = sizeof in;
    }
    InterlockedIncrement(&n->st.accepts);
    ln_logf(n, "LOCNET: accept von %s", locnet_ip_str(in.sin_addr.s_addr, ipb));
    return c;
}

/* W322: Senden direkt oder ueber die Verzoegerungsstufe (loc_netsim, Standard aus). */
static int ln_tx(locnet *n, SOCKET s, const char *buf, int len, int flags, const struct sockaddr_in *to)
{
    if (n->sim && !n->tun) { /* W324: mit Transport wirkt die Stufe auf dessen Pakete, nicht auf Loopback */
        int st = 0, tl = sizeof st;
        getsockopt(s, SOL_SOCKET, SO_TYPE, (char *)&st, &tl);
        return locnetsim_send(n->sim, s, st == SOCK_STREAM, buf, len, flags, to);
    }
    return to ? sendto(s, buf, len, flags, (const struct sockaddr *)to, sizeof *to) : send(s, buf, len, flags);
}

int locnet_send(locnet *n, SOCKET s, const char *buf, int len, int flags)
{
    int r = ln_tx(n, s, buf, len, flags, NULL);
    if (r > 0) {
        int e = WSAGetLastError();
        InterlockedIncrement(&n->st.tx);
        ln_seen(n, s, 1, buf, r);
        WSASetLastError(e);
    }
    return r;
}

int locnet_sendto(locnet *n, SOCKET s, const char *buf, int len, int flags, const struct sockaddr *to, int tolen)
{
    struct sockaddr_in in;
    uint16_t gp;
    int i, r;
    if (!to || tolen < (int)sizeof in || to->sa_family != AF_INET) {
        r = n->sim ? ln_tx(n, s, buf, len, flags, NULL)
                   : sendto(s, buf, len, flags, to, tolen); /* verbundener Socket (TCP 1000) */
        if (r > 0) {
            InterlockedIncrement(&n->st.tx);
            ln_seen(n, s, 1, buf, r);
        }
        return r;
    }
    memcpy(&in, to, sizeof in);
    gp = ntohs(in.sin_port);
    ln_bind_self(n, s);
    if (gp == 1202 && len >= 2 && (((unsigned)(uint8_t)buf[0]) | ((unsigned)(uint8_t)buf[1] << 8)) == 3 &&
        in.sin_addr.s_addr != INADDR_BROADCAST && in.sin_addr.s_addr != n->self_ip) {
        /* W420: Beitrittsanfrage an den Host (nur Adresse und Zeit merken) */
        InterlockedExchange(&n->join_ip, (LONG)in.sin_addr.s_addr);
        InterlockedExchange(&n->join_ms, (LONG)(GetTickCount() | 1u));
    }
    if (n->tun && in.sin_addr.s_addr != n->self_ip) {
        /* W324: Spiel-UDP ueber den eigenen Transport (Broadcast = an alle verbundenen Mitspieler) */
        struct sockaddr_in me;
        int l = sizeof me;
        uint16_t sgp = 0;
        if (getsockname(s, (struct sockaddr *)&me, &l) == 0) {
            sgp = locnet_port_in(n, n->self_ip, ntohs(me.sin_port));
        }
        r = loctun_dgram(n->tun, in.sin_addr.s_addr, sgp, gp, buf, len);
        if (r > 0) {
            InterlockedIncrement(in.sin_addr.s_addr == INADDR_BROADCAST ? &n->st.bc : &n->st.tx);
            ln_seen(n, s, 1, buf, len);
        } else {
            InterlockedIncrement(&n->st.drop);
        }
        return len; /* UDP: gilt als gesendet */
    }
    if (in.sin_addr.s_addr == INADDR_BROADCAST) {
        /* System-Link-Suche/Host-Info: Unicast an jeden Mitspieler */
        for (i = 0; i < n->npeers; i++) {
            in.sin_addr.s_addr = n->peers[i].ip;
            in.sin_port = htons((uint16_t)(gp >= LN_GAME_PORT_LO && gp <= LN_GAME_PORT_HI
                                               ? n->peers[i].base + (gp - LN_GAME_PORT_LO)
                                               : gp));
            ln_tx(n, s, buf, len, flags, &in);
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
    r = ln_tx(n, s, buf, len, flags, &in);
    if (r > 0) {
        InterlockedIncrement(&n->st.tx);
        ln_seen(n, s, 1, buf, r);
    }
    return r;
}

/* ---------- W334 LOC-SEC-CLIENT: Gastfilter in den HLE-Hooks (SECURITY.md §6) ----------
 * Letzte Linie fuer jeden Weg zum Spiel (eigener Transport, Original-TCP/UDP, Loopback): das Spiel bekommt ueber
 * recv/recvfrom nur Bytes, die der Gastfilter freigibt. Im Transport (loc_tun) prueft derselbe Filter vorher und
 * trennt den Mitspieler; hier wird zusaetzlich alles abgefangen, was an ihm vorbei kaeme. */
static void ln_guard_log(locnet *n, int tcp, int why)
{
    static volatile LONG logged;
    if (InterlockedIncrement(&logged) <= 8) {
        ln_logf(n, "LOCGUARD: Spiel-%s Verstoss %s - %s", tcp ? "TCP" : "UDP", lguard_reason(why),
                tcp ? "Verbindung getrennt" : "verworfen");
    }
}

/* W339 SEC-A3: Zustandspruefung fuer UDP-Rahmen, die vom Spielzustand abhaengt (sb_emu setzt sie, loc_net.c). */
static locnet_udp_state_fn s_udp_state;
void locnet_set_udp_state_check(locnet_udp_state_fn f)
{
    s_udp_state = f;
}

/* 1 = Datagramm darf zum Spiel. Spielport aus dem eigenen gebundenen Port. from_ip = Absender, wie ihn das Spiel
 * sieht (Netzwerk-Byte-Reihenfolge, 0 = unbekannt). */
static int ln_udp_guard(locnet *n, SOCKET s, char *buf, int r, uint32_t from_ip)
{
    struct sockaddr_in me;
    int l = sizeof me, why = 0, e = WSAGetLastError();
    uint16_t gp = 0;
    if (getsockname(s, (struct sockaddr *)&me, &l) == 0 && me.sin_family == AF_INET) {
        gp = locnet_port_in(n, n->self_ip, ntohs(me.sin_port));
    }
    WSASetLastError(e);
    if (lguard_udp_check(gp, (const uint8_t *)buf, r, &why)) {
        static volatile LONG st_logged;
        if (!s_udp_state || s_udp_state(gp, (const uint8_t *)buf, r, from_ip)) return 1;
        /* W339: 0x38 von einem Absender ohne Platz in der Spielertabelle (Rahmen selbst gueltig). W409: eigene Zeile -
         * das ist der Normalfall kurz nach einem Abgang/Rauswurf (der Entfernte sendet noch einige 0x38), kein
         * defekter Rahmen; verworfen bleibt er (das Spiel schriebe sonst nach Platz 0xFF). */
        memset(buf, 0, (size_t)r);
        InterlockedIncrement(&n->st.guard);
        if (InterlockedIncrement(&st_logged) <= 4)
            ln_logf(n, "LOCGUARD: Spiel-UDP 0x38 von Absender ohne Platz in der Spielertabelle - verworfen (z. B. nach Abgang)");
        return 0;
    }
    memset(buf, 0, (size_t)r);
    InterlockedIncrement(&n->st.guard);
    ln_guard_log(n, 0, why);
    return 0;
}

/* W335: abgelehntes Datagramm verwerfen. Bei MSG_PEEK liegt es noch in der Warteschlange -> einmal ohne PEEK
 * entnehmen. Puffer des Spiels danach leeren (keine ungeprueften Bytes im Gastspeicher). */
static void ln_dg_discard(SOCKET s, char *buf, int len, int flags)
{
    int e = WSAGetLastError();
    if (flags & MSG_PEEK) recv(s, buf, len, flags & ~MSG_PEEK);
    if (len > 0) memset(buf, 0, (size_t)len);
    WSASetLastError(e);
}

static ln_gw *ln_gw_get(SOCKET s)
{
    static ln_gw over;
    int i, fr = -1;
    for (i = 0; i < LN_GW_MAX; i++) {
        if (s_gw[i].s == s) return &s_gw[i];
        if (fr < 0 && s_gw[i].s == 0) fr = i;
    }
    if (fr < 0) {
        /* W335 (NET-AUDIT N2): Tabelle voll. Frueher wurde der Zustand eines fremden, laufenden Sockets verdraengt
         * (Rahmengrenze vergessen -> Filter und Spiel zerlegen den Strom verschieden). Fail closed: dieser Socket
         * gilt als getrennt. */
        memset(&over, 0, sizeof over);
        over.s = s;
        over.dead = 1;
        return &over;
    }
    memset(&s_gw[fr], 0, sizeof s_gw[fr]);
    s_gw[fr].s = s;
    return &s_gw[fr];
}

static int ln_tcp_kill(locnet *n, SOCKET s, int why)
{
    shutdown(s, SD_BOTH);
    ln_walk_lock();
    ln_gw_get(s)->dead = 1;
    InterlockedExchange(&s_walk_lock, 0);
    InterlockedIncrement(&n->st.guard);
    ln_guard_log(n, 1, why);
    WSASetLastError(WSAECONNRESET);
    return SOCKET_ERROR;
}

/* W350 LOC-REJOIN: Der Mitspieler hat seinen Strom beendet (recv 0 = FIN; das Spiel ruft nie shutdown, ein halb
 * geschlossener Strom kommt im Original nicht vor). Das Spiel wertet recv 0 nicht als Fehler, setzt aber den
 * Stillezaehler zurueck, weil select "lesbar" meldet (FUN_00053B20) - der Platz bliebe fuer immer belegt
 * (Geistereintrag nach Verlassen/Wiederbeitritt). Auf der Konsole loest der Abgang (XNetUnregisterInAddr bzw. Ende der
 * Sicherheitszuordnung) einen Verbindungsreset aus; WSAECONNRESET entfernt den Spieler (FUN_00051FE0 -> FUN_000511E0(5),
 * LOC_RE1 §6.1). Daher hier genau das melden, auch bei jedem weiteren Aufruf (dead). Keine Inhalte im Log. */
static int ln_tcp_leave(locnet *n, SOCKET s)
{
    struct sockaddr_in pa;
    int pl = sizeof pa;
    char ipb[16];
    ln_walk_lock();
    ln_gw_get(s)->dead = 1;
    InterlockedExchange(&s_walk_lock, 0);
    InterlockedIncrement(&n->st.leave);
    memset(&pa, 0, sizeof pa);
    if (getpeername(s, (struct sockaddr *)&pa, &pl) != 0) pa.sin_addr.s_addr = 0;
    ln_logf(n, "LOCNET: Strom von %s beendet - dem Spiel als Verbindungsreset gemeldet (Abgang)",
            locnet_ip_str(pa.sin_addr.s_addr, ipb));
    WSASetLastError(WSAECONNRESET);
    return SOCKET_ERROR;
}

void locnet_guard_kill(locnet *n, SOCKET s, int why)
{
    int e = WSAGetLastError();
    ln_tcp_kill(n, s, why);
    WSASetLastError(e);
}

int locnet_recv(locnet *n, SOCKET s, char *buf, int len, int flags)
{
    int st = 0, tl = sizeof st;
    lguard_walk w;
    ln_gw *g;
    if (getsockopt(s, SOL_SOCKET, SO_TYPE, (char *)&st, &tl) == 0 && st == SOCK_DGRAM) {
        /* W335 (NET-AUDIT N3): UDP ueber recv wie ueber recvfrom absichern - Absender nur Loopback/privat
         * (unverbundener Socket), abgeschnittene Datagramme (WSAEMSGSIZE, Puffer enthaelt ungeprueften Anfang) nie
         * zum Spiel, abgelehnte Datagramme auch bei MSG_PEEK verbrauchen (sonst blockiert eines die Warteschlange). */
        int tries;
        for (tries = 0; tries < 16; tries++) {
            struct sockaddr_in in;
            int l = sizeof in, r = recvfrom(s, buf, len, flags, (struct sockaddr *)&in, &l);
            if (r == SOCKET_ERROR) {
                if (WSAGetLastError() != WSAEMSGSIZE) return r;
                ln_dg_discard(s, buf, len, flags);
                InterlockedIncrement(&n->st.guard);
                continue;
            }
            if (l >= (int)sizeof in && in.sin_family == AF_INET && !locnet_ip_allowed(in.sin_addr.s_addr)) {
                ln_dg_discard(s, buf, len, flags);
                InterlockedIncrement(&n->st.drop);
                continue;
            }
            if (r > 0 && !ln_udp_guard(n, s, buf, r, l >= (int)sizeof in && in.sin_family == AF_INET ? in.sin_addr.s_addr : 0)) {
                ln_dg_discard(s, buf, len, flags);
                continue;
            }
            if (r > 0) ln_session_mark(n);
            return r;
        }
        WSASetLastError(WSAEWOULDBLOCK);
        return SOCKET_ERROR;
    }
    ln_walk_lock();
    g = ln_gw_get(s);
    w = g->w;
    if (g->dead) {
        InterlockedExchange(&s_walk_lock, 0);
        WSASetLastError(WSAECONNRESET);
        return SOCKET_ERROR;
    }
    InterlockedExchange(&s_walk_lock, 0);
    for (;;) {
        uint8_t *big = NULL; /* W339: groesserer Blick (bis LGUARD_HOLD_MAX + 16), nur wenn der Spielpuffer zu klein ist */
        const uint8_t *view = (const uint8_t *)buf;
        int r = recv(s, buf, len, MSG_PEEK), ok, why = 0, vr;
        if (r <= 0) {
            if (r == 0) {
                if (n->fin_pass || len <= 0) return recv(s, buf, len, flags); /* W350 aus: Ende wie bisher melden */
                return ln_tcp_leave(n, s); /* W350: Abgang wie auf der Konsole (Verbindungsreset) */
            }
            return r;
        }
        vr = r;
        ok = lguard_walk_tcp(&w, view, vr, 0, &why);
        if (ok == 0 && r >= len) {
            /* Puffer des Spiels zu klein fuer die Entscheidung (fast voller Strompuffer, bzw. W339: Rahmen mit
             * Inhaltsregel bis 0x100C B muss ganz sichtbar sein): separat ansehen. Kein Stapelpuffer (Gast-Stapel). */
            big = (uint8_t *)malloc(LGUARD_HOLD_MAX + 16);
            if (big) {
                int hr = recv(s, (char *)big, LGUARD_HOLD_MAX + 16, MSG_PEEK);
                if (hr > r) {
                    view = big;
                    vr = hr;
                    ok = lguard_walk_tcp(&w, view, vr, 0, &why);
                }
            }
        }
        if (ok < 0) {
            free(big);
            memset(buf, 0, (size_t)r);
            return ln_tcp_kill(n, s, why);
        }
        if (ok == 0) {
            /* Rahmenkopf noch nicht entscheidbar (1 Byte bzw. 0x34 mit < 5 Byte; W339: Rahmen mit Inhaltsregel noch
             * nicht ganz da): NICHT im Spiel-Thread warten
             * (W334-Lauf CW: das Warten machte aus dem nicht blockierenden recv ein blockierendes -> Haenger bis 1 s).
             * Wie "noch keine Daten" melden; das Spiel fragt wieder. Bleibt der Kopf > 2 s unentschieden, ist der
             * Mitspieler defekt oder boeswillig -> trennen wie bei einem Verstoss. */
            ULONGLONG now = GetTickCount64(), since;
            free(big);
            memset(buf, 0, (size_t)r);
            ln_walk_lock();
            g = ln_gw_get(s);
            if (!g->undecided_since) g->undecided_since = now;
            since = g->undecided_since;
            InterlockedExchange(&s_walk_lock, 0);
            if (now - since >= 2000) return ln_tcp_kill(n, s, LGUARD_V_STATE);
            InterlockedIncrement(&n->st.hold);
            WSASetLastError(WSAEWOULDBLOCK);
            return SOCKET_ERROR;
        }
        if (ok > len) ok = len;
        if (ok < r) memset(buf + ok, 0, (size_t)(r - ok));
        if (ok > 0) ln_session_mark(n);
        if (flags & MSG_PEEK) {
            free(big);
            return ok;
        }
        r = recv(s, buf, ok, flags & ~MSG_PEEK); /* genau die geprueften Bytes entnehmen (identisch zum Blick) */
        if (r > 0) {
            lguard_walk_advance(&w, view, vr, r);
            ln_walk_lock();
            g = ln_gw_get(s);
            g->w = w;
            g->undecided_since = 0;
            InterlockedExchange(&s_walk_lock, 0);
        }
        free(big);
        return r;
    }
}

int locnet_recvfrom(locnet *n, SOCKET s, char *buf, int len, int flags, struct sockaddr *from, int *fromlen)
{
    int tries, st = 0, tl = sizeof st;
    /* W334: das Spiel liest seinen TCP-Strom (Spielport 1000) auch ueber recvfrom. Stream-Sockets gehen durch den
     * TCP-Gastfilter (Rahmenzerleger) wie bei recv; from/fromlen bleiben unberuehrt (Winsock ignoriert sie bei
     * verbindungsorientierten Sockets). Vorher lief der Strom durch den UDP-Filter und wurde ganz verworfen. */
    if (getsockopt(s, SOL_SOCKET, SO_TYPE, (char *)&st, &tl) == 0 && st == SOCK_STREAM) {
        int r = locnet_recv(n, s, buf, len, flags);
        if (r > 0 && !(flags & MSG_PEEK)) { /* wie ln_recv: Zaehler und Typ-Merkliste (loc_steps rx:) */
            int e = WSAGetLastError();
            InterlockedIncrement(&n->st.rx);
            ln_seen(n, s, 0, buf, r);
            WSASetLastError(e);
        }
        return r;
    }
    for (tries = 0; tries < 16; tries++) {
        struct sockaddr_in in;
        int l = sizeof in;
        int r = recvfrom(s, buf, len, flags, (struct sockaddr *)&in, &l);
        if (r == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAECONNRESET) {
                continue; /* Rest-Meldung einer ICMP-Antwort (UDP), wie Xbox ignorieren */
            }
            if (WSAGetLastError() == WSAEMSGSIZE) { /* W334: abgeschnittenes Datagramm nie zum Spiel */
                if (len > 0) memset(buf, 0, (size_t)len);
                InterlockedIncrement(&n->st.guard);
                continue;
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
        if (r > 0 && !ln_udp_guard(n, s, buf, r, l >= (int)sizeof in && in.sin_family == AF_INET ? in.sin_addr.s_addr : 0)) {
            continue; /* W334: Gastfilter UDP (Positivliste) - nie zum Spiel */
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
            ln_session_mark(n);
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

/* W334 (SECURITY.md §6.5): 1 = Adresse ist die eigene oder die eines verbundenen Mitspielers. */
int locnet_addr_known(locnet *n, uint32_t ip_net)
{
    int i;
    if (ip_net == 0) return 0;
    if (ip_net == n->self_ip) return 1;
    if (n->tun) return loctun_is_peer(n->tun, ip_net);
    for (i = 0; i < n->npeers; i++) {
        if (n->peers[i].ip == ip_net) return 1;
    }
    return 0;
}
