/* loc_net_selftest.c - W286 LOC-SYSLINK: Netz-Kern mit zwei Instanzen ueber Loopback.
 * Spielt den System-Link-Ablauf von LoC nach (LOC_RE1 §6): Suche per Broadcast auf
 * UDP 1201, Host-Info mit XNADDR zurueck, XNADDR -> IN_ADDR, Beitritt ueber UDP 1202,
 * TCP 1000 listen/connect/accept mit Rahmen bis 0x100C, VDP 1203, Sicherheitsregeln.
 * Nutzt eigene Adressen 127.0.0.21/.22 und Portbasis 30000 (stoert keine Spiellaeufe). */
#include "loc_net_core.h"
#include "loc_netsim.h"
#include "loc_tun.h"
#include <ws2tcpip.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
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

/* ---------- W322 LOC-NETLAT: Verzoegerungsstufe ---------- */
static double now_ms(void)
{
    LARGE_INTEGER q, f;
    QueryPerformanceCounter(&q);
    QueryPerformanceFrequency(&f);
    return (double)q.QuadPart * 1000.0 / (double)f.QuadPart;
}

static void frame(char *b, int seq)
{
    memset(b, 0, 26);
    b[0] = 0x34;
    memcpy(b + 2, &seq, 4);
}

/* n Rahmen a 26 B empfangen (Polling wie im Spiel), Ankunftszeit und Folgenummer je Rahmen */
static int recv_frames(SOCKET s, int n, double *arr, int *seq, int timeout_ms, int *eof)
{
    char b[26 * 64];
    int have = 0, fr = 0;
    double t0 = now_ms();
    if (eof) *eof = 0;
    while (fr < n && now_ms() - t0 < timeout_ms) {
        fd_set r;
        struct timeval tv = {0, 0};
        FD_ZERO(&r);
        FD_SET(s, &r);
        if (select(0, &r, NULL, NULL, &tv) > 0) {
            int k = recv(s, b + have, (int)sizeof b - have, 0);
            double t = now_ms();
            if (k <= 0) {
                if (eof && k == 0) *eof = 1;
                break;
            }
            have += k;
            while (have >= 26 && fr < n) {
                arr[fr] = t;
                memcpy(&seq[fr], b + 2, 4);
                fr++;
                memmove(b, b + 26, (size_t)(have - 26));
                have -= 26;
            }
        } else {
            Sleep(0);
        }
    }
    return fr;
}

static locnetsim *sim_on(locnet *n, const char *spec)
{
    locnetsim_cfg c;
    char err[96];
    if (n->sim) {
        locnetsim_stop(n->sim);
        n->sim = NULL;
    }
    if (!locnetsim_parse(spec, &c, err, (int)sizeof err)) {
        printf("FEHLER: loc_netsim=%s (%s)\n", spec, err);
        return NULL;
    }
    n->sim = locnetsim_start(&c, lg);
    return n->sim;
}

/* Profiltabelle gegen poc-native/net/netsim/profiles.json (falls auffindbar) */
static double json_num(const char *obj, const char *end, const char *key, double dflt)
{
    char k[40];
    const char *p;
    _snprintf_s(k, sizeof k, _TRUNCATE, "\"%s\"", key);
    p = strstr(obj, k);
    if (!p || p > end) return dflt;
    p = strchr(p + strlen(k), ':');
    return p ? atof(p + 1) : dflt;
}

static void netsim_profiles_vs_json(void)
{
    static const char *paths[] = {"../net/netsim/profiles.json", "../../net/netsim/profiles.json",
                                  "../../../net/netsim/profiles.json", "poc-native/net/netsim/profiles.json"};
    static char txt[16384];
    FILE *f = NULL;
    size_t len;
    int i, bad = 0;
    for (i = 0; i < 4 && !f; i++) fopen_s(&f, paths[i], "rb");
    if (!f) {
        printf("Hinweis: profiles.json nicht gefunden - Profilabgleich uebersprungen\n");
        return;
    }
    len = fread(txt, 1, sizeof txt - 1, f);
    fclose(f);
    txt[len] = 0;
    for (i = 0; i < locnetsim_profile_count(); i++) {
        const char *nm = locnetsim_profile_name(i), *p, *prof = strstr(txt, "\"profiles\""), *e;
        char k[48];
        locnetsim_cfg c;
        memset(&c, 0, sizeof c);
        locnetsim_profile(nm, &c);
        _snprintf_s(k, sizeof k, _TRUNCATE, "\"%s\":", nm);
        p = prof ? strstr(prof, k) : NULL;
        e = p ? strchr(p, '}') : NULL;
        if (p && e && strstr(p, "\"ge\"") && strstr(p, "\"ge\"") < e) e = strchr(e + 1, '}'); /* ge-Unterobjekt */
        if (!p || !e || fabs(json_num(p, e, "rtt_ms", -1) - c.rtt_ms) > 1e-6 ||
            fabs(json_num(p, e, "jitter_ms", 0) - c.jitter_ms) > 1e-6 ||
            fabs(json_num(p, e, "loss_pct", 0) - c.loss_pct) > 1e-6 ||
            fabs(json_num(p, e, "p_gb", 0) - c.ge_p_gb) > 1e-9) {
            printf("FEHLER: Profil %s weicht von profiles.json ab\n", nm);
            bad++;
        }
    }
    CHECK(bad == 0, "eingebaute Profile = profiles.json (rtt/jitter/loss/ge)");
}

static void netsim_tests(void)
{
    locnet A, B;
    locnetsim_cfg c;
    char err[96], b[200];
    SOCKET blis, acon, bacc = INVALID_SOCKET;
    double tsend[32], arr[32], mn = 1e9, mx = 0, t0;
    int seq[32], i, k = 0, ok, eof = 0;

    /* Schluessel */
    CHECK(locnetsim_parse("", &c, err, 96) == 0, "loc_netsim leer = aus");
    CHECK(locnetsim_parse("dsl", &c, err, 96) == 1 && c.rtt_ms == 30 && c.jitter_ms == 2 && c.nagle == 1,
          "loc_netsim=dsl -> eu-eu (30 ms, Jitter 2), Nagle an");
    CHECK(locnetsim_parse("dsl", &c, err, 96) == 1 && c.nagle_set == 0, "nagle ohne Angabe folgt loc_net_nodelay");
    CHECK(locnetsim_parse("eu-jp,nagle:0", &c, err, 96) == 1 && c.rtt_ms == 250 && c.nagle == 0 && c.nagle_set,
          "Profil + Einzelwert (eu-jp, nagle:0)");
    CHECK(locnetsim_parse("nagle:0,wlan", &c, err, 96) == 1 && c.nagle == 0 && c.ge_p_gb > 0 && c.reorder == 1,
          "Einzelwert vor Profil bleibt erhalten (wlan = mobile-bad)");
    CHECK(locnetsim_parse("rtt:300,jitter:20,loss:1,bw:500", &c, err, 96) == 1 && c.rtt_ms == 300 &&
              c.jitter_ms == 20 && c.loss_pct == 1 && c.bw_kbps == 500,
          "Einzelwerte rtt/jitter/loss/bw");
    CHECK(locnetsim_parse("rtt:100,ramp:60>800/120>1600", &c, err, 96) == 1 && c.ramp_n == 2 && c.ramp_s[1] == 120 &&
              c.ramp_rtt[1] == 1600,
          "Stufen (ramp)");
    CHECK(locnetsim_parse("gibtsnicht", &c, err, 96) == 0 && err[0], "unbekanntes Profil abgelehnt");
    CHECK(locnetsim_parse("rtt:-5", &c, err, 96) == 0, "negativer Wert abgelehnt");
    netsim_profiles_vs_json();

    /* Verbindung A -> B (A verzoegert seine ausgehenden Daten) */
    CHECK(locnet_init(&A, "127.0.0.31", 32000, "127.0.0.32", 10, lg) == 1 &&
              locnet_init(&B, "127.0.0.32", 32000, "127.0.0.31", 10, lg) == 1,
          "Netzsim: Instanzen A/B");
    {
        struct sockaddr_in l = sa(INADDR_ANY, 1000), to = sa(B.self_ip, 1000);
        u_long nb = 1;
        blis = locnet_socket(&B, AF_INET, SOCK_STREAM, IPPROTO_TCP);
        locnet_bind(&B, blis, (struct sockaddr *)&l, sizeof l);
        listen(blis, 5);
        ioctlsocket(blis, (long)0x8004667E, &nb);
        A.nodelay = 1; /* loc_net_nodelay=1 (Standard) */
        B.nodelay = 1;
        acon = locnet_socket(&A, AF_INET, SOCK_STREAM, IPPROTO_TCP);
        locnet_connect(&A, acon, (struct sockaddr *)&to, sizeof to);
        for (i = 0; i < 200 && bacc == INVALID_SOCKET; i++) {
            bacc = locnet_accept(&B, blis, NULL, NULL);
            if (bacc == INVALID_SOCKET) Sleep(5);
        }
    }
    CHECK(bacc != INVALID_SOCKET, "Netzsim: TCP verbunden");
    {
        BOOL nd1 = FALSE, nd2 = FALSE;
        int l1 = sizeof nd1, l2 = sizeof nd2;
        getsockopt(acon, IPPROTO_TCP, TCP_NODELAY, (char *)&nd1, &l1);
        getsockopt(bacc, IPPROTO_TCP, TCP_NODELAY, (char *)&nd2, &l2);
        CHECK(nd1 && nd2, "loc_net_nodelay: TCP_NODELAY auf connect- und accept-Socket");
    }

    /* 1) Ablaufzeit + Reihenfolge trotz Jitter: RTT 200 (je Richtung 100), Jitter 30 */
    sim_on(&A, "rtt:200,jitter:30,seed:7,nagle:0");
    for (i = 0; i < 20; i++) {
        frame(b, i);
        tsend[i] = now_ms();
        k = locnet_send(&A, acon, b, 26, 0); /* Folge ohne Pause, Empfang danach */
    }
    CHECK(k == 26, "Netzsim: send nimmt den Rahmen sofort an");
    k = recv_frames(bacc, 20, arr, seq, 3000, NULL);
    ok = (k == 20);
    for (i = 0; i < k; i++) {
        double d = arr[i] - tsend[i];
        if (seq[i] != i) ok = 0;
        if (d < mn) mn = d;
        if (d > mx) mx = d;
    }
    CHECK(ok, "Netzsim TCP: alle 20 Rahmen vollstaendig und in Reihenfolge (Jitter sortiert nicht um)");
    CHECK(mn >= 99.0, "Netzsim TCP: kein Rahmen vor Ablauf der halben RTT (100 ms) gemeldet");
    CHECK(mx < 260.0 && mx - mn > 5.0, "Netzsim TCP: Jitter wirkt (Streuung > 5 ms), obere Grenze plausibel");
    printf("Netzsim TCP: Verweildauer %.1f..%.1f ms (Soll >= 100)\n", mn, mx);

    /* 2) Nagle-Modell: zweiter kleiner Rahmen wartet auf das ACK des ersten (RTT 100) */
    sim_on(&A, "rtt:100");
    t0 = now_ms();
    frame(b, 100);
    locnet_send(&A, acon, b, 26, 0);
    Sleep(10);
    frame(b, 101);
    locnet_send(&A, acon, b, 26, 0);
    k = recv_frames(bacc, 2, arr, seq, 2000, NULL);
    CHECK(k == 2 && seq[0] == 100 && seq[1] == 101 && arr[0] - t0 >= 49.0 && arr[0] - t0 < 90.0 &&
              arr[1] - t0 >= 148.0,
          "Netzsim Nagle: Rahmen 1 nach 50 ms, Rahmen 2 erst nach ACK (>= 150 ms)");
    CHECK(locnetsim_get_stats(A.sim)->nagle_held >= 1, "Netzsim Nagle: Zaehler");
    printf("Netzsim Nagle: %.1f / %.1f ms\n", arr[0] - t0, k == 2 ? arr[1] - t0 : -1.0);

    /* 3) TCP-Verlust = Neuuebertragung nach RTO, Strom bleibt geordnet und vollstaendig */
    sim_on(&A, "rtt:20,loss:30,seed:3,nagle:0");
    for (i = 0; i < 30; i++) {
        frame(b, 200 + i);
        tsend[i] = now_ms();
        locnet_send(&A, acon, b, 26, 0);
    }
    k = recv_frames(bacc, 30, arr, seq, 8000, NULL);
    ok = (k == 30);
    mx = 0;
    for (i = 0; i < k; i++) {
        if (seq[i] != 200 + i) ok = 0;
        if (arr[i] - tsend[i] > mx) mx = arr[i] - tsend[i];
    }
    CHECK(ok && locnetsim_get_stats(A.sim)->tcp_retx > 0 && mx >= 200.0,
          "Netzsim TCP-Verlust: vollstaendig + geordnet, Neuuebertragung nach RTO >= 200 ms");

    /* 4) Bandbreite: 104 B bei 8 kbit/s = (104+40)*8/8 = 144 ms */
    sim_on(&A, "rtt:0,bw:8,nagle:0");
    t0 = now_ms();
    frame(b, 300);
    frame(b + 26, 301);
    frame(b + 52, 302);
    frame(b + 78, 303);
    locnet_send(&A, acon, b, 104, 0);
    k = recv_frames(bacc, 4, arr, seq, 2000, NULL);
    CHECK(k == 4 && arr[3] - t0 >= 143.0, "Netzsim Bandbreite: Serialisierung (>= 144 ms fuer 104+40 B bei 8 kbit/s)");

    /* 5) UDP: Verzoegerung und Verlust */
    {
        SOCKET ua = game_udp(&A, 1202, IPPROTO_UDP), ub = game_udp(&B, 1202, IPPROTO_UDP);
        struct sockaddr_in to = sa(B.self_ip, 1202), fr;
        int got = 0;
        LONG lost;
        sim_on(&A, "rtt:160");
        t0 = now_ms();
        locnet_sendto(&A, ua, "\x03\x00", 2, 0, (struct sockaddr *)&to, sizeof to);
        while (now_ms() - t0 < 1000 && !got) {
            int fl = sizeof fr;
            if (locnet_recvfrom(&B, ub, b, sizeof b, 0, (struct sockaddr *)&fr, &fl) == 2) got = 1;
            else Sleep(0);
        }
        CHECK(got && now_ms() - t0 >= 79.0, "Netzsim UDP: Datagramm nach >= 80 ms (halbe RTT)");
        sim_on(&A, "loss:50,rtt:0,seed:1");
        for (i = 0; i < 40; i++) {
            locnet_sendto(&A, ua, "\x03\x00", 2, 0, (struct sockaddr *)&to, sizeof to);
        }
        Sleep(100);
        got = 0;
        for (i = 0; i < 100; i++) {
            int fl = sizeof fr;
            if (locnet_recvfrom(&B, ub, b, sizeof b, 0, (struct sockaddr *)&fr, &fl) == 2) got++;
        }
        lost = locnetsim_get_stats(A.sim)->udp_lost;
        CHECK(lost > 5 && got == 40 - lost, "Netzsim UDP-Verlust: verlorene Datagramme fehlen, Rest kommt an");
        sim_on(&A, "loss:50,uloss:0,rtt:0,seed:1");
        for (i = 0; i < 20; i++) {
            locnet_sendto(&A, ua, " ", 2, 0, (struct sockaddr *)&to, sizeof to);
        }
        Sleep(100);
        got = 0;
        for (i = 0; i < 100; i++) {
            int fl = sizeof fr;
            if (locnet_recvfrom(&B, ub, b, sizeof b, 0, (struct sockaddr *)&fr, &fl) == 2) got++;
        }
        CHECK(got == 20 && locnetsim_get_stats(A.sim)->udp_lost == 0, "Netzsim uloss:0: UDP ohne Verlust trotz loss");
        closesocket(ua);
        closesocket(ub);
    }

    /* 6) closesocket mit offenen Sendungen: erst zustellen, dann geordnetes Ende */
    sim_on(&A, "rtt:200,nagle:0");
    t0 = now_ms();
    frame(b, 400);
    locnet_send(&A, acon, b, 26, 0);
    CHECK(locnet_closesocket(&A, acon) == 0, "Netzsim: closesocket kehrt sofort zurueck");
    k = recv_frames(bacc, 1, arr, seq, 2000, NULL);
    CHECK(k == 1 && seq[0] == 400 && arr[0] - t0 >= 99.0, "Netzsim: offener Rahmen kommt nach dem Schliessen noch an");
    k = recv_frames(bacc, 1, arr, seq, 2000, &eof);
    CHECK(k == 0 && eof, "Netzsim: danach Verbindungsende (recv 0)");

    closesocket(bacc);
    closesocket(blis);
    locnet_shutdown(&A); /* stoppt die Stufe */
    locnet_shutdown(&B);
}


/* ---------- W324 LOC-NETPLAY: eigener UDP-Transport ---------- */
typedef struct tun_pair {
    locnet A, B;
    int ok;
} tun_pair;

static char s_auth_a[100], s_auth_b[100]; /* W328: --auth-a/--auth-b "account:exp:G-hex" */

/* Zwei Instanzen mit Transport (feste Mitspieler oder Treffpunkt). simspec: Stufe auf beiden (NULL = aus). */
static int tun_pair_start(tun_pair *P, int base, const char *simspec, const char *lobby, int nodirect, int red)
{
    loctun_cfg ca, cb;
    char ipa[16], ipb2[16];
    int i;
    memset(P, 0, sizeof *P);
    _snprintf_s(ipa, sizeof ipa, _TRUNCATE, "127.0.0.%d", 31 + (base / 100) % 40);
    _snprintf_s(ipb2, sizeof ipb2, _TRUNCATE, "127.0.0.%d", 71 + (base / 100) % 40);
    if (!locnet_init(&P->A, lobby ? "random" : ipa, base, "none", 10, lg)) return 0;
    if (!locnet_init(&P->B, lobby ? "random" : ipb2, base, "none", 10, lg)) return 0;
    if (simspec) {
        sim_on(&P->A, simspec);
        sim_on(&P->B, simspec);
    }
    loctun_cfg_default(&ca);
    loctun_cfg_default(&cb);
    ca.proxy_base = cb.proxy_base = (uint16_t)(base + 3000);
    ca.red_fixed = cb.red_fixed = red;
    ca.nodirect = cb.nodirect = nodirect;
    if (lobby) {
        char room[33];
        _snprintf_s(room, sizeof room, _TRUNCATE, "selftest-%lu", GetCurrentProcessId() % 100000u);
        ca.allow_public = cb.allow_public = 1; /* wie loc_net.c: mit Treffpunkt auch oeffentliche Adressen */
        strcpy_s(ca.lobby, sizeof ca.lobby, lobby);
        strcpy_s(cb.lobby, sizeof cb.lobby, lobby);
        strcpy_s(ca.room, sizeof ca.room, room);
        strcpy_s(cb.room, sizeof cb.room, room);
        strcpy_s(ca.role, sizeof ca.role, "host");
        strcpy_s(cb.role, sizeof cb.role, "join");
        strcpy_s(ca.pilot, sizeof ca.pilot, "TestA");
        strcpy_s(cb.pilot, sizeof cb.pilot, "TestB");
        strcpy_s(ca.auth, sizeof ca.auth, s_auth_a); /* W328: Konten (leer = Gast) */
        strcpy_s(cb.auth, sizeof cb.auth, s_auth_b);
    } else {
        ca.port = (uint16_t)(base + 900);
        cb.port = (uint16_t)(base + 901);
        _snprintf_s(ca.peers, sizeof ca.peers, _TRUNCATE, "127.0.0.1:%d", base + 901);
        _snprintf_s(cb.peers, sizeof cb.peers, _TRUNCATE, "127.0.0.1:%d", base + 900);
    }
    P->A.tun = loctun_start(&ca, &P->A, P->A.sim, lg);
    if (lobby) Sleep(500); /* Host legt die Lobby an */
    P->B.tun = loctun_start(&cb, &P->B, P->B.sim, lg);
    if (!P->A.tun || !P->B.tun) return 0;
    for (i = 0; i < (lobby ? 1500 : 500); i++) {
        if (loctun_links_up(P->A.tun) == 1 && loctun_links_up(P->B.tun) == 1 && P->A.npeers == 1 &&
            P->B.npeers == 1) {
            P->ok = 1;
            return 1;
        }
        Sleep(10);
    }
    return 0;
}

static void tun_pair_stop(tun_pair *P)
{
    locnet_shutdown(&P->A);
    locnet_shutdown(&P->B);
}

/* TCP 1000 ueber den Transport: B lauscht, A verbindet zur vip von B. */
static int tun_tcp(tun_pair *P, SOCKET *acon, SOCKET *bacc, SOCKET *blis)
{
    struct sockaddr_in l = sa(INADDR_ANY, 1000), to = sa(P->B.self_ip, 1000);
    u_long nb = 1;
    int i;
    *blis = locnet_socket(&P->B, AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (locnet_bind(&P->B, *blis, (struct sockaddr *)&l, sizeof l) != 0 || listen(*blis, 5) != 0) return 0;
    ioctlsocket(*blis, (long)0x8004667E, &nb);
    *acon = locnet_socket(&P->A, AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (locnet_connect(&P->A, *acon, (struct sockaddr *)&to, sizeof to) != 0) return 0;
    *bacc = INVALID_SOCKET;
    for (i = 0; i < 300 && *bacc == INVALID_SOCKET; i++) {
        struct sockaddr_in r;
        int rl = sizeof r;
        *bacc = locnet_accept(&P->B, *blis, (struct sockaddr *)&r, &rl);
        if (*bacc != INVALID_SOCKET) {
            CHECK(r.sin_addr.s_addr == P->A.self_ip, "Transport: accept meldet die virtuelle Adresse von A");
        } else {
            Sleep(10);
        }
    }
    return *bacc != INVALID_SOCKET;
}

/* n Lockstep-Rahmen (26 B) im Abstand gap_ms von s nach r: geordnet, vollstaendig, groesster Abstand.
 * Der Empfang laeuft gleichzeitig in einem eigenen Thread (Ankunftszeiten wie im Spiel). */
typedef struct tf_rx {
    SOCKET r;
    int n, got;
    double arr[400];
    int seq[400];
} tf_rx;

static DWORD WINAPI tf_rx_thread(LPVOID p)
{
    tf_rx *x = (tf_rx *)p;
    x->got = recv_frames(x->r, x->n, x->arr, x->seq, 15000, NULL);
    return 0;
}

static int s_tf_over; /* Abstaende > 2*gap+30 ms (Haenger im Kleinen) */
static int tun_frames(SOCKET s, SOCKET r, int n, int gap_ms, double *maxgap, int *inorder)
{
    static tf_rx x;
    HANDLE th;
    int i;
    char b[26];
    double t0;
    if (n > 400) n = 400;
    memset(&x, 0, sizeof x);
    x.r = r;
    x.n = n;
    th = CreateThread(NULL, 0, tf_rx_thread, &x, 0, NULL);
    t0 = now_ms();
    for (i = 0; i < n; i++) {
        frame(b, i);
        send(s, b, 26, 0);
        while (now_ms() - t0 < (double)(i + 1) * gap_ms) Sleep(0); /* Sleep(1) waere ohne Fenster 15,6 ms */
    }
    s_tf_over = 0;
    WaitForSingleObject(th, 20000);
    CloseHandle(th);
    *inorder = 1;
    *maxgap = 0;
    for (i = 0; i < x.got; i++) {
        if (x.seq[i] != i) *inorder = 0;
        if (i > 0 && x.arr[i] - x.arr[i - 1] > *maxgap) *maxgap = x.arr[i] - x.arr[i - 1];
        if (i > 0 && x.arr[i] - x.arr[i - 1] > 2.0 * gap_ms + 30.0) {
            s_tf_over++;
            if (s_verbose) printf("  Abstand vor Rahmen %d: %.1f ms\n", i, x.arr[i] - x.arr[i - 1]);
        }
    }
    return x.got;
}

static void tun_tests(void)
{
    tun_pair P;
    SOCKET a1201, b1201, acon, bacc, blis;
    static char buf[0x1100], big[0x100C];
    struct sockaddr_in from;
    int k, i, path = 0, red = 0, inorder;
    uint32_t vip = 0;
    double srtt = 0, maxgap;
    char relay[24];
    int red_over = 0;

    /* Schluessel/Regeln ohne Netz */
    {
        locnet X;
        loctun *t;
        loctun_cfg c;
        CHECK(locnet_init(&X, "random", 34000, "none", 10, lg) == 1, "Transport: loc_net_addr=random startet");
        CHECK((ntohl(X.self_ip) >> 22) == (0x7F400000u >> 22) && X.npeers == 0,
              "Transport: Zufallsadresse in 127.64/10, keine Mitspielerliste (none)");
        loctun_cfg_default(&c);
        strcpy_s(c.peers, sizeof c.peers, "8.8.8.8:5000");
        t = loctun_start(&c, &X, NULL, lg);
        CHECK(t != NULL, "Transport startet");
        Sleep(300);
        CHECK(t && loctun_links_up(t) == 0 && !loctun_link_info(t, 0, NULL, NULL, NULL, NULL, NULL, 0),
              "Transport: oeffentlicher Mitspieler ohne Treffpunkt abgelehnt");
        CHECK(t && loctun_dgram(t, ip4("127.0.0.99"), 1201, 1201, "x", 1) == -1,
              "Transport: Datagramm an unbekannte vip verworfen");
        X.tun = t;
        locnet_shutdown(&X);
    }

    /* 1) ohne Stoerung: Suche, Antwort, TCP-Rahmen bis 0x100C */
    CHECK(tun_pair_start(&P, 36000, NULL, NULL, 0, 0), "Transport: Verbindung A<->B steht (Durchstich + HELLO)");
    if (!P.ok) {
        tun_pair_stop(&P);
        return;
    }
    CHECK(loctun_link_info(P.A.tun, 0, &path, &srtt, &red, &vip, relay, sizeof relay) && path == 1 &&
              vip == P.B.self_ip,
          "Transport: A sieht B unter dessen Adresse, Weg direkt");
    CHECK(P.A.npeers == 1 && P.A.peers[0].ip == P.B.self_ip && P.A.peers[0].base == 39000,
          "Transport: Mitspielerliste = vip mit Stellvertreter-Basis");
    a1201 = game_udp(&P.A, 1201, IPPROTO_UDP);
    b1201 = game_udp(&P.B, 1201, IPPROTO_UDP);
    {
        struct sockaddr_in bc = sa(INADDR_BROADCAST, 1201);
        memset(buf, 0, 0x14);
        buf[0] = 1;
        k = locnet_sendto(&P.A, a1201, buf, 0x14, 0, (struct sockaddr *)&bc, sizeof bc);
        CHECK(k == 0x14, "Transport: Broadcast-Suche angenommen");
    }
    k = poll_recvfrom(&P.B, b1201, buf, sizeof buf, &from);
    CHECK(k == 0x14 && buf[0] == 1 && from.sin_addr.s_addr == P.A.self_ip && ntohs(from.sin_port) == 1201,
          "Transport: B empfaengt die Suche von A:1201 (aus Spielsicht)");
    memset(buf, 0, 100);
    buf[0] = 2;
    k = locnet_sendto(&P.B, b1201, buf, 100, 0, (struct sockaddr *)&from, sizeof from);
    k = poll_recvfrom(&P.A, a1201, buf, sizeof buf, &from);
    CHECK(k == 100 && buf[0] == 2 && from.sin_addr.s_addr == P.B.self_ip && ntohs(from.sin_port) == 1201,
          "Transport: Antwort an die Absenderadresse kommt bei A an");
    k = poll_recvfrom(&P.A, a1201, buf, sizeof buf, &from);
    CHECK(k < 0, "Transport: Datagramm genau einmal zugestellt (Kopien entdoppelt)");
    CHECK(tun_tcp(&P, &acon, &bacc, &blis), "Transport: TCP 1000 ueber den Stellvertreter verbunden");
    {
        SOCKET t = locnet_socket(&P.A, AF_INET, SOCK_STREAM, IPPROTO_TCP);
        struct sockaddr_in o = sa(ip4("127.0.0.99"), 1000);
        CHECK(locnet_connect(&P.A, t, (struct sockaddr *)&o, sizeof o) == SOCKET_ERROR,
              "Transport: connect an Nicht-Mitspieler verweigert (kein Weg am Transport vorbei)");
        closesocket(t);
    }
    for (i = 0; i < (int)sizeof big; i++) big[i] = (char)(i * 13 + 1);
    k = send(acon, big, sizeof big, 0);
    k = recv_all(bacc, buf, (int)sizeof big);
    CHECK(k == (int)sizeof big && memcmp(buf, big, sizeof big) == 0, "Transport: 0x100C-Rahmen A->B vollstaendig");
    k = send(bacc, big, sizeof big, 0);
    k = recv_all(acon, buf, (int)sizeof big);
    CHECK(k == (int)sizeof big && memcmp(buf, big, sizeof big) == 0, "Transport: 0x100C-Rahmen B->A vollstaendig");
    k = tun_frames(acon, bacc, 100, 5, &maxgap, &inorder);
    CHECK(k == 100 && inorder, "Transport: 100 Lockstep-Rahmen geordnet und vollstaendig");
    printf("Transport ohne Stoerung: RTT %.2f ms, groesster Rahmenabstand %.1f ms (Soll 5)\n", srtt, maxgap);
    {
        uint8_t *q;
        Sleep(300);
        q = (uint8_t *)locnet_qos_alloc(&P.A, 1, 8);
        CHECK(q && *(uint16_t *)(q + 0x14) >= 100 && *(uint16_t *)(q + 0x14) < 200,
              "Transport: QoS-RTT = gemessene RTT + Reserve 100 ms");
        locnet_qos_free(q);
    }
    closesocket(acon);
    k = recv_all(bacc, buf, 1);
    CHECK(k == 0, "Transport: Schliessen kommt beim Partner an (FIN)");
    closesocket(bacc); closesocket(blis); closesocket(a1201); closesocket(b1201);
    tun_pair_stop(&P);

    /* 2) Verlust 10 % + Jitter + Umordnung: Reihenfolge, Vollstaendigkeit, Redundanz */
    CHECK(tun_pair_start(&P, 37000, "rtt:40,jitter:8,loss:10,reorder:1,seed:7", NULL, 0, 0),
          "Transport (10 % Verlust): Verbindung steht");
    if (P.ok && tun_tcp(&P, &acon, &bacc, &blis)) {
        const loctun_stats *sa_ = loctun_get_stats(P.A.tun), *sb_ = loctun_get_stats(P.B.tun);
        double t0 = now_ms();
        k = tun_frames(acon, bacc, 300, 10, &maxgap, &inorder);
        CHECK(k == 300 && inorder, "Transport (10 % Verlust, Umordnung): 300 Rahmen geordnet und vollstaendig");
        printf("Transport 10 %% Verlust: %d Rahmen in %.0f ms, groesster Abstand %.1f ms, > 50 ms: %d; A: Kopien %ld RTO %ld; "
               "B: gerettet %ld doppelt %ld\n",
               k, now_ms() - t0, maxgap, s_tf_over, sa_->msg_copy, sa_->msg_rto, sb_->msg_saved, sb_->msg_dup);
        CHECK(sb_->msg_saved > 0, "Transport: verlorene Rahmen durch Redundanz-Kopien gerettet");
        CHECK(maxgap < 100.0 && s_tf_over <= 2,
              "Transport: kein Haenger >= 100 ms bei 10 % Verlust, hoechstens 2 Abstaende > 50 ms (TCP-RTO >= 200 ms)");
        red_over = s_tf_over;
        loctun_link_info(P.A.tun, 0, &path, &srtt, &red, &vip, relay, sizeof relay);
        CHECK(red >= 3, "Transport: Redundanz N steigt mit gemeldetem Verlust (adaptiv)");
        printf("Transport 10 %% Verlust: N=%d, RTT %.1f ms\n", red, srtt);
        for (i = 0; i < (int)sizeof big; i++) big[i] = (char)(i * 3 + 5);
        send(acon, big, sizeof big, 0);
        k = recv_all(bacc, buf, (int)sizeof big);
        CHECK(k == (int)sizeof big && memcmp(buf, big, sizeof big) == 0,
              "Transport (10 % Verlust): 0x100C-Rahmen segmentiert und vollstaendig");
        a1201 = game_udp(&P.A, 1202, IPPROTO_UDP);
        b1201 = game_udp(&P.B, 1202, IPPROTO_UDP);
        {
            struct sockaddr_in to = sa(P.B.self_ip, 1202);
            int got = 0, dup = 0;
            unsigned char seen[60];
            memset(seen, 0, sizeof seen);
            for (i = 0; i < 50; i++) {
                memset(buf, 0, 64);
                buf[0] = (char)i;
                locnet_sendto(&P.A, a1201, buf, 64, 0, (struct sockaddr *)&to, sizeof to);
                Sleep(5);
            }
            for (i = 0; i < 100; i++) {
                k = poll_recvfrom(&P.B, b1201, buf, sizeof buf, &from);
                if (k < 0) break;
                if (seen[(unsigned char)buf[0] % 60]++) dup++;
                got++;
            }
            printf("Transport 10 %% Verlust: Datagramme %d/50 angekommen, doppelt %d\n", got, dup);
            CHECK(got >= 47 && dup == 0, "Transport: Datagramme mit Kopien >= 47/50 bei 10 % Verlust, keine Doppelten");
        }
        closesocket(acon); closesocket(bacc); closesocket(blis); closesocket(a1201); closesocket(b1201);
    } else {
        CHECK(0, "Transport (10 % Verlust): TCP verbunden");
    }
    tun_pair_stop(&P);

    /* 3) feste Redundanz N=1 (keine Kopien) als Gegenprobe: dann nur RTO */
    CHECK(tun_pair_start(&P, 38000, "rtt:40,loss:10,seed:3", NULL, 0, 1), "Transport N=1: Verbindung steht");
    if (P.ok && tun_tcp(&P, &acon, &bacc, &blis)) {
        const loctun_stats *sa_ = loctun_get_stats(P.A.tun);
        double mg1;
        k = tun_frames(acon, bacc, 200, 10, &mg1, &inorder);
        CHECK(k == 200 && inorder && sa_->msg_copy == 0 && sa_->msg_rto > 0,
              "Transport N=1: vollstaendig nur ueber Wiederholung (RTO), keine Kopien");
        printf("Transport N=1 (Gegenprobe): groesster Abstand %.1f ms, > 50 ms: %d, RTO %ld\n", mg1, s_tf_over,
               sa_->msg_rto);
        CHECK(s_tf_over > red_over, "Transport: ohne Kopien mehr Stockungen als mit Redundanz (Gegenprobe)");
        closesocket(acon); closesocket(bacc); closesocket(blis);
    }
    tun_pair_stop(&P);
}

/* ---------- W329 STEELNET-SECREVIEW: boeswilliger Mitspieler (handgebaute Transportpakete) ---------- */
typedef struct fpeer {
    SOCKET s;                 /* eigener UDP-Socket des "Mitspielers" */
    struct sockaddr_in b;     /* Transport-Adresse von B (aus dem Durchstich) */
    int have_b;
    uint32_t vip;             /* gemeldete virtuelle Adresse */
    uint32_t seq, acked, pseq;
    uint32_t fin_mask;        /* S_FIN von B fuer Strom-ID < 32 */
} fpeer;

static void fp_put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void fp_put32(uint8_t *p, uint32_t v) { fp_put16(p, (uint16_t)v); fp_put16(p + 2, (uint16_t)(v >> 16)); }
static uint32_t fp_get32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static void fp_hdr(uint8_t *p, int type)
{
    memcpy(p, "SBT1", 4); /* Kennung */
    p[4] = (uint8_t)type;
    p[5] = 0;
    memcpy(p + 6, "SBT1", 4); /* Sitzung = erste 4 Byte der TEST-Nonce "SBT1TEST" */
}

static void fp_hello(fpeer *f)
{
    uint8_t p[18];
    fp_hdr(p, 1);
    fp_put32(p + 10, f->vip);
    p[14] = 1;
    p[15] = 1;
    fp_put16(p + 16, 0);
    if (f->have_b) sendto(f->s, (const char *)p, sizeof p, 0, (struct sockaddr *)&f->b, sizeof f->b);
}

/* Ein T_DATA mit einem Eintrag (K_STREAM: sub/stream/data mit Folgenummer seq; K_DGRAM: src/dst/data). */
static void fp_data(fpeer *f, int kind, uint32_t seq, int sub, uint16_t stream, uint16_t dst, const uint8_t *d, int n)
{
    uint8_t p[1200];
    int off = 31;
    memset(p, 0, 31);
    fp_hdr(p, 3);
    fp_put32(p + 10, ++f->pseq);
    p[30] = 1;
    if (kind == 1) {
        p[off] = 1;
        fp_put32(p + off + 1, seq);
        p[off + 5] = (uint8_t)sub;
        fp_put16(p + off + 6, stream);
        p[off + 8] = 0;
        fp_put16(p + off + 9, (uint16_t)n);
        if (n) memcpy(p + off + 11, d, (size_t)n);
        off += 11 + n;
    } else {
        p[off] = 2;
        fp_put16(p + off + 1, (uint16_t)f->pseq);
        fp_put16(p + off + 3, 1202);
        fp_put16(p + off + 5, dst);
        p[off + 7] = 0;
        fp_put16(p + off + 8, (uint16_t)n);
        memcpy(p + off + 10, d, (size_t)n);
        off += 10 + n;
    }
    sendto(f->s, (const char *)p, off, 0, (struct sockaddr *)&f->b, sizeof f->b);
}

/* Pakete von B verarbeiten (Durchstich beantworten, Bestaetigung + FIN mitlesen), ms Millisekunden. */
static void fp_pump(fpeer *f, int ms)
{
    double t0 = now_ms();
    do {
        uint8_t b[2048];
        struct sockaddr_in from;
        int fl = sizeof from, k;
        while ((k = recvfrom(f->s, (char *)b, sizeof b, 0, (struct sockaddr *)&from, &fl)) > 0) {
            if (k == 13 && memcmp(b, "SBP1", 4) == 0 && b[4] == 1) { /* PROBE -> ACK */
                b[4] = 2;
                sendto(f->s, (const char *)b, 13, 0, (struct sockaddr *)&from, sizeof from);
                f->b = from;
                f->have_b = 1;
            } else if (k >= 31 && memcmp(b, "SBT1", 4) == 0 && b[4] == 3) {
                uint32_t ack = fp_get32(b + 14);
                int n = b[30], off = 31, i;
                if ((int32_t)(ack - f->acked) > 0) f->acked = ack;
                for (i = 0; i < n && off < k; i++) {
                    if (b[off] == 1 && off + 11 <= k) {
                        int ml = b[off + 9] | (b[off + 10] << 8);
                        int st = b[off + 6] | (b[off + 7] << 8);
                        if (b[off + 5] == 3 && st < 32) f->fin_mask |= 1u << st;
                        off += 11 + ml;
                    } else if (b[off] == 2 && off + 10 <= k) {
                        off += 10 + (b[off + 8] | (b[off + 9] << 8));
                    } else {
                        break;
                    }
                }
            }
            fl = sizeof from;
        }
        if (ms > 0) Sleep(1);
    } while (now_ms() - t0 < ms);
}

/* Zuverlaessig (Go-Back-N ueber die Bestaetigung von B) eine Strom-Nachricht senden. */
static int fp_stream(fpeer *f, int sub, uint16_t stream, const uint8_t *d, int n)
{
    uint32_t s = f->seq++;
    double t0 = now_ms();
    fp_data(f, 1, s, sub, stream, 0, d, n);
    fp_pump(f, 0);
    while ((int32_t)(f->acked - (s + 1)) < 0) { /* auf Bestaetigung warten, sonst wiederholen */
        fp_pump(f, 5);
        if ((int32_t)(f->acked - (s + 1)) < 0) fp_data(f, 1, s, sub, stream, 0, d, n);
        if (now_ms() - t0 > 3000) return 0;
    }
    return 1;
}

static int fp_tcp_pending(SOCKET l, int ms)
{
    fd_set r;
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    FD_ZERO(&r);
    FD_SET(l, &r);
    return select(0, &r, NULL, NULL, &tv) > 0;
}

static void tun_guard_tests(void)
{
    locnet B;
    loctun_cfg c;
    fpeer f;
    struct sockaddr_in a;
    u_long nb = 1;
    char buf[2048];
    SOCKET vic_u, vic_t, g1202, glis, gacc = INVALID_SOCKET;
    int i, k, ok;
    memset(&f, 0, sizeof f);
    if (!locnet_init(&B, "127.0.0.45", 44000, "none", 10, lg)) {
        CHECK(0, "W329: Instanz B startet");
        return;
    }
    f.s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    a = sa(htonl(INADDR_LOOPBACK), 44950);
    bind(f.s, (struct sockaddr *)&a, sizeof a);
    ioctlsocket(f.s, (long)FIONBIO, &nb);
    loctun_cfg_default(&c);
    c.port = 44900;
    c.proxy_base = 45000; /* Stellvertreter fuer die vip des "Mitspielers" */
    strcpy_s(c.peers, sizeof c.peers, "127.0.0.1:44950");
    B.tun = loctun_start(&c, &B, NULL, lg);
    if (!B.tun) {
        CHECK(0, "W329: Transport B startet");
        closesocket(f.s);
        locnet_shutdown(&B);
        return;
    }
    for (i = 0; i < 200 && !f.have_b; i++) fp_pump(&f, 10);
    CHECK(f.have_b, "W329: Durchstich zum Test-Mitspieler");

    /* 1) virtuelle Adresse: 0.0.0.0 / 127.0.0.1 / eigene LAN-Adresse nie -> Stellvertreter nicht offen fuers Netz */
    f.vip = 0;
    for (i = 0; i < 20; i++) { fp_hello(&f); fp_pump(&f, 10); }
    CHECK(loctun_links_up(B.tun) == 0, "W329: HELLO mit vip 0.0.0.0 abgelehnt (kein Stellvertreter auf allen Schnittstellen)");
    f.vip = ip4("127.0.0.1");
    for (i = 0; i < 20; i++) { fp_hello(&f); fp_pump(&f, 10); }
    CHECK(loctun_links_up(B.tun) == 0, "W329: HELLO mit vip 127.0.0.1 abgelehnt");
    f.vip = ip4("192.168.77.5");
    for (i = 0; i < 20; i++) { fp_hello(&f); fp_pump(&f, 10); }
    CHECK(loctun_links_up(B.tun) == 0, "W329: HELLO mit nicht-Loopback-vip abgelehnt");
    f.vip = ip4("127.0.0.46");
    for (i = 0; i < 100 && loctun_links_up(B.tun) == 0; i++) { fp_hello(&f); fp_pump(&f, 10); }
    CHECK(loctun_links_up(B.tun) == 1, "W329: HELLO mit Loopback-vip 127.0.0.46 -> Verbindung steht");
    {
        uint32_t keep = f.vip, vip = 0;
        f.vip = ip4("127.0.0.47");
        for (i = 0; i < 10; i++) { fp_hello(&f); fp_pump(&f, 10); }
        loctun_link_info(B.tun, 0, NULL, NULL, NULL, &vip, NULL, 0);
        CHECK(vip == keep, "W329: vip nach dem Verbindungsaufbau nicht mehr aenderbar");
        f.vip = keep;
    }

    /* 2) UDP: nur an Ports, die das Spiel gebunden hat - nie an fremde lokale Dienste */
    vic_u = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    a = sa(B.self_ip, 44777);
    bind(vic_u, (struct sockaddr *)&a, sizeof a);
    ioctlsocket(vic_u, (long)FIONBIO, &nb);
    g1202 = game_udp(&B, 1202, IPPROTO_UDP);
    memset(buf, 0x5A, 64);
    fp_data(&f, 2, 0, 0, 0, 44777, (const uint8_t *)buf, 64);
    fp_data(&f, 2, 0, 0, 0, 1999, (const uint8_t *)buf, 64); /* Spielbereich, aber nicht gebunden */
    fp_data(&f, 2, 0, 0, 0, 1202, (const uint8_t *)buf, 64);
    fp_pump(&f, 200);
    k = recv(vic_u, buf, sizeof buf, 0);
    CHECK(k <= 0, "W329: Datagramm des Mitspielers an fremden lokalen UDP-Port verworfen");
    {
        struct sockaddr_in from;
        k = poll_recvfrom(&B, g1202, buf, sizeof buf, &from);
        CHECK(k == 64, "W329: Datagramm an gebundenen Spielport 1202 kommt an (Gegenprobe)");
    }

    /* 3) TCP: OPEN nur an Spiel-TCP 1000 - nie Tunnel zu beliebigen lokalen Diensten */
    vic_t = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    a = sa(B.self_ip, 44778);
    bind(vic_t, (struct sockaddr *)&a, sizeof a);
    listen(vic_t, 4);
    {
        uint8_t od[4];
        fp_put16(od, 44778);
        fp_put16(od + 2, 0);
        ok = fp_stream(&f, 1, 2, od, 4);
        fp_pump(&f, 300);
        CHECK(ok && !fp_tcp_pending(vic_t, 200), "W329: OPEN an fremden TCP-Port 44778 baut keine Verbindung auf");
        CHECK(f.fin_mask & (1u << 2), "W329: abgelehnter OPEN wird beim Mitspieler geschlossen (FIN)");
    }
    glis = locnet_socket(&B, AF_INET, SOCK_STREAM, IPPROTO_TCP);
    a = sa(INADDR_ANY, 1000);
    locnet_bind(&B, glis, (struct sockaddr *)&a, sizeof a);
    listen(glis, 4);
    {
        uint8_t od[4];
        fp_put16(od, 1000);
        fp_put16(od + 2, 0);
        ok = fp_stream(&f, 1, 4, od, 4);
        if (fp_tcp_pending(glis, 1000)) gacc = accept(glis, NULL, NULL);
        CHECK(ok && gacc != INVALID_SOCKET, "W329: OPEN an Spiel-TCP 1000 verbunden (Gegenprobe)");
    }

    /* 4) Puffer zum Spiel begrenzt: Spiel liest nicht, Mitspieler sendet weiter -> Strom zu, kein Speicherlauf */
    if (gacc != INVALID_SOCKET) {
        static uint8_t chunk[1000];
        int sent = 0;
        memset(chunk, 0x33, sizeof chunk);
        for (i = 0; i < 12000 && !(f.fin_mask & (1u << 4)); i++) { /* bis 12 MB */
            if (!fp_stream(&f, 2, 4, chunk, sizeof chunk)) break;
            sent++;
        }
        fp_pump(&f, 300);
        printf("W329: Puffergrenze nach %d KB ohne Lesen des Spiels\n", sent);
        CHECK(f.fin_mask & (1u << 4), "W329: Puffer zum Spiel voll -> Strom geschlossen (FIN), kein unbegrenzter Speicher");
        closesocket(gacc);
    }
    closesocket(glis);
    closesocket(vic_t);
    closesocket(vic_u);
    closesocket(g1202);
    closesocket(f.s);
    locnet_shutdown(&B);
}

/* --lobby host:port [--nodirect] [--sim SPEC]: zwei Instanzen ueber einen laufenden steelnet-Treffpunkt. */
static int tun_lobby_test(const char *lobby, int nodirect, const char *simspec)
{
    tun_pair P;
    SOCKET acon, bacc, blis;
    int k, path = 0, red = 0, inorder;
    double srtt = 0, maxgap;
    uint32_t vip;
    char relay[24];
    CHECK(tun_pair_start(&P, 40000, simspec, lobby, nodirect, 0), "Treffpunkt: beide in der Lobby, Verbindung steht");
    if (s_auth_a[0] || s_auth_b[0]) { /* W328: beide mit Konto angemeldet (kein stiller Rueckfall auf Gast) */
        CHECK(P.A.tun && loctun_auth_state(P.A.tun) == (s_auth_a[0] ? 2 : 0), "Treffpunkt: Konto A per Challenge-Response");
        CHECK(P.B.tun && loctun_auth_state(P.B.tun) == (s_auth_b[0] ? 2 : 0), "Treffpunkt: Konto B per Challenge-Response");
    }
    if (P.ok) {
        Sleep(1500); /* Wegwahl des Treffpunkts abwarten */
        loctun_link_info(P.A.tun, 0, &path, &srtt, &red, &vip, relay, sizeof relay);
        printf("Treffpunkt: Weg %s%s, RTT %.1f ms, N=%d\n", path == 1 ? "direkt" : (path == 2 ? "Relay " : "keiner"),
               path == 2 ? relay : "", srtt, red);
        CHECK(nodirect ? path == 2 : path == 1, nodirect ? "Treffpunkt: Weg ueber Relay" : "Treffpunkt: Weg direkt");
        CHECK(tun_tcp(&P, &acon, &bacc, &blis), "Treffpunkt: TCP 1000 verbunden");
        k = tun_frames(acon, bacc, 200, 10, &maxgap, &inorder);
        CHECK(k == 200 && inorder, "Treffpunkt: 200 Rahmen geordnet und vollstaendig");
        printf("Treffpunkt: groesster Rahmenabstand %.1f ms\n", maxgap);
        loctun_log_stats(P.A.tun, 0);
        closesocket(acon); closesocket(bacc); closesocket(blis);
    }
    tun_pair_stop(&P);
    printf("loc_net_selftest --lobby: %d/%d OK\n", s_ok, s_n);
    return s_ok == s_n ? 0 : 1;
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
    for (i = 1; i < argc; i++) { /* W324: --lobby host:port [--nodirect] [--sim SPEC] */
        if (strcmp(argv[i], "-v") == 0) s_verbose = 1;
    }
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--lobby") == 0 && i + 1 < argc) {
            int nd = 0, j;
            const char *sp = NULL;
            for (j = 1; j < argc; j++) {
                if (strcmp(argv[j], "--nodirect") == 0) nd = 1;
                if (strcmp(argv[j], "--sim") == 0 && j + 1 < argc) sp = argv[j + 1];
                if (strcmp(argv[j], "--auth-a") == 0 && j + 1 < argc) strncpy_s(s_auth_a, sizeof s_auth_a, argv[j + 1], _TRUNCATE);
                if (strcmp(argv[j], "--auth-b") == 0 && j + 1 < argc) strncpy_s(s_auth_b, sizeof s_auth_b, argv[j + 1], _TRUNCATE);
            }
            return tun_lobby_test(argv[i + 1], nd, sp);
        }
    }

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
    { /* W324: Typ-Merkliste zerlegt den TCP-Strom (zusammengelegte und geteilte Rahmen, LOC_RE1 §6.4) */
        static char fb[0x1A0 + 4 + 26];
#define SEEN(t) ((A.st.typ_seen[0][(t) >> 5] >> ((t) & 31)) & 1)
        memset(fb, 0x0F, sizeof fb); /* Inhalt sieht aus wie Typ 0x0F */
        fb[0] = 0x0E; fb[1] = 0;            /* 0x0E: 0x1A0 B */
        fb[0x1A0] = 0x0F; fb[0x1A1] = 0;    /* 0x0F: 4 B */
        fb[0x1A4] = 0x34; fb[0x1A5] = 0;    /* 0x34: 26 B */
        memset((void *)A.st.typ_seen, 0, sizeof A.st.typ_seen);
        locnet_note_stream(&A, acon, 0, fb, 100);
        CHECK(SEEN(0x0E) && !SEEN(0x0F), "Strom: Inhalt eines geteilten Rahmens zaehlt nicht als Typ");
        locnet_note_stream(&A, acon, 0, fb + 100, (int)sizeof fb - 100);
        CHECK(SEEN(0x0F) && SEEN(0x34), "Strom: zusammengelegte Rahmen einzeln erkannt (0x0F hinter 0x0E)");
#undef SEEN
    }

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

    netsim_tests(); /* W322 */
    if (loctun_available()) {
        tun_tests(); /* W324 */
        tun_guard_tests(); /* W329 */
    } else {
        printf("Hinweis: eigener Transport nur in der privaten Fassung - Transport-Pruefungen uebersprungen\n");
    }

    printf("loc_net_selftest: %d/%d OK\n", s_ok, s_n);
    return s_ok == s_n ? 0 : 1;
}
