/* loc_net_selftest.c - W286 LOC-SYSLINK: Netz-Kern mit zwei Instanzen ueber Loopback.
 * Spielt den System-Link-Ablauf von LoC nach (LOC_RE1 §6): Suche per Broadcast auf
 * UDP 1201, Host-Info mit XNADDR zurueck, XNADDR -> IN_ADDR, Beitritt ueber UDP 1202,
 * TCP 1000 listen/connect/accept mit Rahmen bis 0x100C, VDP 1203, Sicherheitsregeln.
 * Nutzt eigene Adressen 127.0.0.21/.22 und Portbasis 30000 (stoert keine Spiellaeufe). */
#include "loc_net_core.h"
#include <ws2tcpip.h>
#include <stdio.h>
#include <string.h>

static int s_ok, s_n;
#define CHECK(c, msg)                                                   \
    do {                                                                \
        s_n++;                                                          \
        if (c) {                                                        \
            s_ok++;                                                     \
        } else {                                                        \
            printf("FEHLER: %s (Zeile %d)\n", msg, __LINE__);           \
        }                                                               \
    } while (0)

static int s_verbose;
static void lg(const char *m)
{
    if (s_verbose) {
        printf("  [log] %s\n", m);
    }
}

static uint32_t ip4(const char *s)
{
    struct in_addr a;
    inet_pton(AF_INET, s, &a);
    return a.s_addr;
}

static struct sockaddr_in sa(uint32_t ip, uint16_t port)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = ip;
    a.sin_port = htons(port);
    return a;
}

/* wie LoC (FUN_0004E5E0/0004E6A0): UDP, bind INADDR_ANY:port, Broadcast, nicht blockierend */
static SOCKET game_udp(locnet *n, uint16_t port, int proto)
{
    struct sockaddr_in a = sa(INADDR_ANY, port);
    int one = 1;
    u_long nb = 1;
    SOCKET s = locnet_socket(n, AF_INET, SOCK_DGRAM, proto);
    if (s == INVALID_SOCKET) {
        return s;
    }
    if (locnet_bind(n, s, (struct sockaddr *)&a, sizeof a) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    locnet_setsockopt(n, s, SOL_SOCKET, SO_BROADCAST, (const char *)&one, 4);
    ioctlsocket(s, (long)0x8004667E, &nb);
    return s;
}

/* Polling wie im Spiel (select mit Timeout 0), hoechstens ~2 s */
static int poll_recvfrom(locnet *n, SOCKET s, char *buf, int len, struct sockaddr_in *from)
{
    int t;
    for (t = 0; t < 200; t++) {
        fd_set r;
        struct timeval tv = {0, 0};
        FD_ZERO(&r);
        FD_SET(s, &r);
        if (select(0, &r, NULL, NULL, &tv) > 0 && __WSAFDIsSet(s, &r)) {
            int fl = sizeof *from;
            int k = locnet_recvfrom(n, s, buf, len, 0, (struct sockaddr *)from, &fl);
            if (k > 0) {
                return k;
            }
        }
        Sleep(10);
    }
    return -1;
}

static int recv_all(SOCKET s, char *buf, int len)
{
    int got = 0, t;
    for (t = 0; t < 400 && got < len; t++) {
        fd_set r;
        struct timeval tv = {0, 0};
        FD_ZERO(&r);
        FD_SET(s, &r);
        if (select(0, &r, NULL, NULL, &tv) > 0) {
            int k = recv(s, buf + got, len - got, 0);
            if (k <= 0) {
                break;
            }
            got += k;
        } else {
            Sleep(5);
        }
    }
    return got;
}

int main(int argc, char **argv)
{
    locnet A, B, C, X1, X2;
    char ipb[16];
    char buf[0x1100], big[0x100C];
    uint8_t xna[LOCNET_XNADDR_SIZE];
    struct sockaddr_in from;
    SOCKET a1201, b1201, a1202, b1202, bvdp, blis, acon, bacc;
    int k, i;
    s_verbose = (argc > 1 && strcmp(argv[1], "-v") == 0);

    /* --- Init + Sicherheitsregeln --- */
    CHECK(locnet_init(&A, "127.0.0.21", 30000, "127.0.0.22:30000, 8.8.8.8, 127.0.0.23:35000, 1.2.3", 10, lg) == 1,
          "Instanz A startet");
    CHECK(A.npeers == 2, "oeffentliche Adresse 8.8.8.8 und Unsinn in loc_net_peers abgelehnt");
    CHECK(locnet_init(&B, "127.0.0.22", 30000, "127.0.0.21", 10, lg) == 1, "Instanz B startet");
    CHECK(locnet_init(&C, "127.0.0.21", 30000, "auto", 10, lg) == 0, "zweite Instanz mit gleicher Adresse abgelehnt");
    CHECK(locnet_init(&C, "8.8.8.8", 30000, "auto", 10, lg) == 0, "oeffentliche eigene Adresse abgelehnt");
    CHECK(locnet_ip_allowed(ip4("192.168.1.5")) && locnet_ip_allowed(ip4("10.1.2.3")) &&
              locnet_ip_allowed(ip4("172.20.0.1")) && !locnet_ip_allowed(ip4("172.32.0.1")) &&
              !locnet_ip_allowed(ip4("8.8.4.4")) && !locnet_ip_allowed(INADDR_BROADCAST),
          "Adressfilter Loopback/privat");

    /* --- Portabbildung --- */
    CHECK(locnet_port_out(&A, ip4("127.0.0.23"), 1202) == 35202, "Port 1202 -> Basis des Mitspielers");
    CHECK(locnet_port_in(&A, ip4("127.0.0.23"), 35202) == 1202, "Rueckabbildung 35202 -> 1202");
    CHECK(locnet_port_out(&A, ip4("127.0.0.22"), 1000) == 30000, "TCP 1000 -> 30000");
    CHECK(locnet_port_out(&A, ip4("127.0.0.22"), 80) == 80, "Port ausserhalb 1000-1999 unveraendert");

    /* --- System-Link-Suche (UDP 1201, Broadcast) --- */
    a1201 = game_udp(&A, 1201, IPPROTO_UDP);
    b1201 = game_udp(&B, 1201, IPPROTO_UDP);
    CHECK(a1201 != INVALID_SOCKET && b1201 != INVALID_SOCKET, "UDP 1201 an beiden Instanzen gebunden");
    {
        struct sockaddr_in me;
        int l = sizeof me;
        getsockname(a1201, (struct sockaddr *)&me, &l);
        CHECK(me.sin_addr.s_addr == ip4("127.0.0.21") && ntohs(me.sin_port) == 30201,
              "bind INADDR_ANY:1201 -> 127.0.0.21:30201 (nie alle Schnittstellen)");
    }
    {
        struct sockaddr_in bc = sa(INADDR_BROADCAST, 1201);
        memset(buf, 0, 0x14);
        buf[0] = 1; /* Typ 1: Suche */
        k = locnet_sendto(&A, a1201, buf, 0x14, 0, (struct sockaddr *)&bc, sizeof bc);
        CHECK(k == 0x14, "Broadcast-Suche gesendet");
    }
    k = poll_recvfrom(&B, b1201, buf, sizeof buf, &from);
    CHECK(k == 0x14 && buf[0] == 1, "B empfaengt die Suche");
    CHECK(from.sin_addr.s_addr == ip4("127.0.0.21") && ntohs(from.sin_port) == 1201,
          "Absender aus Spielsicht 127.0.0.21:1201");

    /* --- Host-Info (Typ 2, 100 B) mit XNADDR + XNKID/XNKEY --- */
    {
        struct sockaddr_in bc = sa(INADDR_BROADCAST, 1201);
        uint8_t kid[8], key[16];
        memset(buf, 0, 100);
        buf[0] = 2;
        locnet_create_key(kid, key);
        CHECK((kid[0] & 0xF0) == 0, "XNKID ist System-Link-Typ");
        memcpy(buf + 0x0C, kid, 8);
        memcpy(buf + 0x14, key, 16);
        locnet_xnaddr(&B, B.self_ip, (uint8_t *)buf + 0x24);
        k = locnet_sendto(&B, b1201, buf, 100, 0, (struct sockaddr *)&bc, sizeof bc);
        CHECK(k == 100, "Host-Info gesendet");
    }
    k = poll_recvfrom(&A, a1201, buf, sizeof buf, &from);
    CHECK(k == 100 && buf[0] == 2, "A empfaengt die Host-Info");
    memcpy(xna, buf + 0x24, sizeof xna);
    CHECK(locnet_xnaddr_ip(xna) == ip4("127.0.0.22"), "XNADDR -> IN_ADDR = Adresse von B");
    {
        uint8_t x2[LOCNET_XNADDR_SIZE];
        locnet_xnaddr(&A, ip4("127.0.0.22"), x2);
        CHECK(memcmp(x2, xna, sizeof xna) == 0, "XNetInAddrToXnAddr ist an beiden Enden gleich");
    }

    /* --- Beitritt (UDP 1202, Typ 3, 0x2C4 B) an die Host-Adresse --- */
    a1202 = game_udp(&A, 1202, IPPROTO_UDP);
    b1202 = game_udp(&B, 1202, IPPROTO_UDP);
    {
        struct sockaddr_in to = sa(locnet_xnaddr_ip(xna), 1202);
        memset(buf, 0x5A, 0x2C4);
        buf[0] = 3;
        buf[1] = 0;
        k = locnet_sendto(&A, a1202, buf, 0x2C4, 0, (struct sockaddr *)&to, sizeof to);
        CHECK(k == 0x2C4, "Beitrittsanfrage gesendet");
    }
    k = poll_recvfrom(&B, b1202, buf, sizeof buf, &from);
    CHECK(k == 0x2C4 && buf[0] == 3, "Host empfaengt die Beitrittsanfrage");
    CHECK(from.sin_addr.s_addr == A.self_ip && ntohs(from.sin_port) == 1202, "Antwortadresse 127.0.0.21:1202");
    k = locnet_sendto(&B, b1202, "\x06\x00", 2, 0, (struct sockaddr *)&from, sizeof from);
    k = poll_recvfrom(&A, a1202, buf, sizeof buf, &from);
    CHECK(k == 2 && buf[0] == 6, "Antwort des Hosts kommt an");

    /* --- VDP 1203 (Protokoll 0xFE) --- */
    bvdp = game_udp(&B, 1203, 0xFE);
    CHECK(bvdp != INVALID_SOCKET, "VDP-Socket (0xFE) als UDP 1203");

    /* --- TCP 1000 (listen/connect/accept, wie FUN_0004E7C0) --- */
    {
        struct sockaddr_in l = sa(INADDR_ANY, 1000), to = sa(B.self_ip, 1000);
        int one = 1;
        u_long nb = 1;
        blis = locnet_socket(&B, AF_INET, SOCK_STREAM, IPPROTO_TCP);
        locnet_setsockopt(&B, blis, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, 4);
        CHECK(locnet_bind(&B, blis, (struct sockaddr *)&l, sizeof l) == 0, "TCP 1000 gebunden");
        CHECK(listen(blis, 5) == 0, "listen(5)");
        ioctlsocket(blis, (long)0x8004667E, &nb);
        acon = locnet_socket(&A, AF_INET, SOCK_STREAM, IPPROTO_TCP);
        CHECK(locnet_connect(&A, acon, (struct sockaddr *)&to, sizeof to) == 0, "connect zum Host");
    }
    bacc = INVALID_SOCKET;
    for (i = 0; i < 200 && bacc == INVALID_SOCKET; i++) {
        struct sockaddr_in r;
        int rl = sizeof r;
        bacc = locnet_accept(&B, blis, (struct sockaddr *)&r, &rl);
        if (bacc != INVALID_SOCKET) {
            CHECK(r.sin_addr.s_addr == A.self_ip, "accept meldet die Adresse von A");
        } else {
            Sleep(10);
        }
    }
    CHECK(bacc != INVALID_SOCKET, "Host nimmt die Verbindung an");
    memset(buf, 0, 26);
    buf[0] = 0x34; /* 0x34: 26 B je Tick und Peer */
    k = locnet_sendto(&A, acon, buf, 26, 0, NULL, 0);
    CHECK(k == 26, "0x34-Rahmen ueber sendto(NULL) auf TCP");
    k = recv_all(bacc, buf, 26);
    CHECK(k == 26 && buf[0] == 0x34, "0x34-Rahmen vollstaendig");
    for (i = 0; i < (int)sizeof big; i++) {
        big[i] = (char)(i * 7);
    }
    k = send(bacc, big, sizeof big, 0);
    CHECK(k == (int)sizeof big, "0x100C-Rahmen gesendet");
    k = recv_all(acon, buf, (int)sizeof big);
    CHECK(k == (int)sizeof big && memcmp(buf, big, sizeof big) == 0, "0x100C-Rahmen vollstaendig und gleich");

    /* --- nie ins Internet --- */
    {
        struct sockaddr_in pub = sa(ip4("8.8.8.8"), 1202);
        LONG d0 = A.st.drop;
        SOCKET t;
        k = locnet_sendto(&A, a1202, "x", 1, 0, (struct sockaddr *)&pub, sizeof pub);
        CHECK(k == 1 && A.st.drop == d0 + 1, "UDP an oeffentliche Adresse still verworfen");
        t = locnet_socket(&A, AF_INET, SOCK_STREAM, IPPROTO_TCP);
        pub = sa(ip4("8.8.8.8"), 1000);
        CHECK(locnet_connect(&A, t, (struct sockaddr *)&pub, sizeof pub) == SOCKET_ERROR &&
                  WSAGetLastError() == WSAEHOSTUNREACH,
              "connect an oeffentliche Adresse verweigert");
        closesocket(t);
    }

    /* --- QoS-Block (Aufbau wie FUN_0011F1C0 liest) --- */
    {
        uint8_t *q = (uint8_t *)locnet_qos_alloc(&A, 1, 8);
        CHECK(q && ((uint32_t *)q)[0] == 1 && (q[8] & 1) && !(q[8] & 4) && *(uint16_t *)(q + 0x14) == 10 &&
                  *(uint16_t *)(q + 0x16) == 10 && *(uint32_t *)(q + 0x18) >= 205200u,
              "XNQOS: COMPLETE, RTT 10 ms, Bandbreite >= 205200 bit/s");
        locnet_qos_free(q);
    }

    closesocket(a1201); closesocket(b1201); closesocket(a1202); closesocket(b1202); closesocket(bvdp);
    closesocket(acon); closesocket(bacc); closesocket(blis);
    locnet_shutdown(&A);
    locnet_shutdown(&B);

    /* --- auto: zwei Instanzen auf einem Rechner bekommen verschiedene Adressen --- */
    CHECK(locnet_init(&X1, "auto", 31000, "auto", 10, lg) == 1, "auto-Instanz 1");
    CHECK(locnet_init(&X2, "auto", 31000, "auto", 10, lg) == 1, "auto-Instanz 2");
    CHECK(X1.self_ip != X2.self_ip, "auto: verschiedene Loopback-Adressen");
    CHECK(X1.npeers == 7 && X2.npeers == 7, "auto: 7 Mitspieler-Adressen (127.0.0.2-9 ohne sich)");
    printf("auto-Adressen: %s", locnet_ip_str(X1.self_ip, ipb));
    printf(" / %s\n", locnet_ip_str(X2.self_ip, ipb));
    locnet_shutdown(&X1);
    locnet_shutdown(&X2);

    printf("loc_net_selftest: %d/%d OK\n", s_ok, s_n);
    return s_ok == s_n ? 0 : 1;
}
