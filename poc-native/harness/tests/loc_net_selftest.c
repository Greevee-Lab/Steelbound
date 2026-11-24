/* loc_net_selftest.c - W286 LOC-SYSLINK: Netz-Kern mit zwei Instanzen ueber Loopback.
 * Spielt den System-Link-Ablauf von LoC nach (LOC_RE1 §6): Suche per Broadcast auf
 * UDP 1201, Host-Info mit XNADDR zurueck, XNADDR -> IN_ADDR, Beitritt ueber UDP 1202,
 * TCP 1000 listen/connect/accept mit Rahmen bis 0x100C, VDP 1203, Sicherheitsregeln.
 * Nutzt eigene Adressen 127.0.0.21/.22 und Portbasis 30000 (stoert keine Spiellaeufe). */
#include "loc_net_core.h"
#include "loc_netsim.h"
#include "loc_tun.h"
#include "loc_route.h" /* W395 */
#include "sb_e2e.h"
#include "loc_guard.h"
#include "sb_tls.h"
#include <ws2tcpip.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json_mini.h"
#include "sb_pausenet.h" /* W357 LOC-PAUSE-NET */
#include "loc_rule.h"     /* W420 LOC-CLIPDUMP */
#include "loc_chat.h"     /* W437 LOC-CHAT */
#include "loc_assist.h"   /* W440 LOC-LOBBY-MODES */
#include "loc_lobbylist.h" /* W443 LOC-LOBBY-LIST */
#include "loc_lobbywidget.h" /* W460 LOC-LOBBY-WIDGET */
#include "loc_lan.h"       /* W454 LOC-LAN-MODE */
#include "loc_rbn.h"       /* W475 LOC-RB-PREAGREE (Vektoren SBRP) */
#include "sb_sha256.h"

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
static volatile LONG s_lg_selfvip; /* W332: Logzeilen "dieselbe virtuelle Adresse" (Log-Flut durch Mitspieler) */
static volatile LONG s_lg_n; /* W332: alle Logzeilen (Log-Obergrenze) */
static void lg(const char *m)
{
    InterlockedIncrement(&s_lg_n);
    if (strstr(m, "dieselbe virtuelle Adresse")) {
        InterlockedIncrement(&s_lg_selfvip);
    }
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
/* W339: Test-Zustandspruefung - lehnt 0x38 ab und merkt sich den Absender */
static uint32_t s_w339_from;
static int w339_reject_38(uint16_t gp, const uint8_t *d, int n, uint32_t from_ip)
{
    if (gp == 1202 && n >= 2 && d[0] == 0x38 && d[1] == 0) {
        s_w339_from = from_ip;
        return 0;
    }
    return 1;
}

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
    b[0] = 0x34; /* W334: gueltiger Lockstep-Rahmen (n = 1 -> 26 B), sonst verwirft der Gastfilter */
    b[4] = 1;
    memcpy(b + 6, &seq, 4);
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
                memcpy(&seq[fr], b + 6, 4);
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
        locnet_sendto(&A, ua, "\x05\x00\x00\x00", 4, 0, (struct sockaddr *)&to, sizeof to);
        while (now_ms() - t0 < 1000 && !got) {
            int fl = sizeof fr;
            if (locnet_recvfrom(&B, ub, b, sizeof b, 0, (struct sockaddr *)&fr, &fl) == 4) got = 1;
            else Sleep(0);
        }
        CHECK(got && now_ms() - t0 >= 79.0, "Netzsim UDP: Datagramm nach >= 80 ms (halbe RTT)");
        sim_on(&A, "loss:50,rtt:0,seed:1");
        for (i = 0; i < 40; i++) {
            locnet_sendto(&A, ua, "\x05\x00\x00\x00", 4, 0, (struct sockaddr *)&to, sizeof to);
        }
        Sleep(100);
        got = 0;
        for (i = 0; i < 100; i++) {
            int fl = sizeof fr;
            if (locnet_recvfrom(&B, ub, b, sizeof b, 0, (struct sockaddr *)&fr, &fl) == 4) got++;
        }
        lost = locnetsim_get_stats(A.sim)->udp_lost;
        CHECK(lost > 5 && got == 40 - lost, "Netzsim UDP-Verlust: verlorene Datagramme fehlen, Rest kommt an");
        sim_on(&A, "loss:50,uloss:0,rtt:0,seed:1");
        for (i = 0; i < 20; i++) {
            locnet_sendto(&A, ua, "\x05\x00\x00\x00", 4, 0, (struct sockaddr *)&to, sizeof to);
        }
        Sleep(100);
        got = 0;
        for (i = 0; i < 100; i++) {
            int fl = sizeof fr;
            if (locnet_recvfrom(&B, ub, b, sizeof b, 0, (struct sockaddr *)&fr, &fl) == 4) got++;
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
static char s_spki[80];                     /* W334: --spki <hex64> = Pin des (Loopback-)Testtreffpunkts */
static char s_rsim_a[256], s_rsim_b[256];   /* W395: --rsim/--rsim-a/--rsim-b (loc_route_sim je Seite) */
static int s_open_hold = -1;                /* W409: >= 0 = open_hold_ms beider Seiten in tun_pair_start */
static int s_refuse_b;                      /* W409 TEST: test_refuse_ms nur bei B (connect wie unter Linux sofort abgelehnt) */
static int s_ver_check;                     /* W420 TEST: Versionspruefung im HELLO ohne Treffpunkt */
static char s_ver_a[16], s_ver_b[16];       /* W420 TEST: Versionen je Seite ("-" = ohne Feld) */

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
    if (s_open_hold >= 0) ca.open_hold_ms = cb.open_hold_ms = s_open_hold; /* W409 */
    cb.test_refuse_ms = s_refuse_b;
    ca.ver_check = cb.ver_check = s_ver_check; /* W420 */
    strcpy_s(ca.ver_test, sizeof ca.ver_test, s_ver_a);
    strcpy_s(cb.ver_test, sizeof cb.ver_test, s_ver_b);
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
        strcpy_s(ca.spki, sizeof ca.spki, s_spki); /* W334: TLS-Pin des Testtreffpunkts */
        strcpy_s(cb.spki, sizeof cb.spki, s_spki);
        strcpy_s(ca.route_sim, sizeof ca.route_sim, s_rsim_a); /* W395 TEST: Wege mit eigener Verzoegerung */
        strcpy_s(cb.route_sim, sizeof cb.route_sim, s_rsim_b);
    } else {
        ca.port = (uint16_t)(base + 900);
        cb.port = (uint16_t)(base + 901);
        _snprintf_s(ca.peers, sizeof ca.peers, _TRUNCATE, "127.0.0.1:%d", base + 901);
        _snprintf_s(cb.peers, sizeof cb.peers, _TRUNCATE, "127.0.0.1:%d", base + 900);
    }
    P->A.tun = loctun_start(&ca, &P->A, P->A.sim, lg);
    if (lobby && P->A.tun) {
        /* W334: Host legt eine Lobby mit Raumcode an (Standard); B tritt mit dem Code bei */
        for (i = 0; i < 1000 && !loctun_lobby_code(P->A.tun, cb.code, sizeof cb.code); i++) Sleep(10);
        printf("Treffpunkt: Raumcode %s\n", cb.code[0] ? "erhalten" : "FEHLT");
    }
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
    CHECK(locnet_addr_known(&P.A, P.B.self_ip) && locnet_addr_known(&P.A, P.A.self_ip) &&
              !locnet_addr_known(&P.A, ip4("127.0.0.99")) && !locnet_addr_known(&P.A, ip4("192.168.1.7")),
          "W334: XNet-HLE gibt nur eigene/verbundene Adressen heraus (fremde -> Fehler)");
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
    big[0] = 0x22; /* W334: gueltiger Kopf (0x100C) */
    big[1] = 0;
    big[4] = 0; big[5] = 0x10; big[6] = 0; big[7] = 0; /* W339: Blocklaenge 0x1000 (Inhaltsregel Stufe 2) */
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
        big[0] = 0x22; /* W334 */
        big[4] = 0; big[5] = 0x10; big[6] = 0; big[7] = 0; /* W339: Blocklaenge 0x1000 */
        big[1] = 0;
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
                memset(buf, 0, 0x2C4);
                buf[0] = 3; /* W334: gueltiger Rahmen (Beitritt, 0x2C4 B), Kennung in Byte 4 */
                buf[4] = (char)i;
                locnet_sendto(&P.A, a1201, buf, 0x2C4, 0, (struct sockaddr *)&to, sizeof to);
                Sleep(5);
            }
            for (i = 0; i < 100; i++) {
                k = poll_recvfrom(&P.B, b1201, buf, sizeof buf, &from);
                if (k < 0) break;
                if (seen[(unsigned char)buf[4] % 60]++) dup++;
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

/* ---------- W329/W334: boeswilliger Mitspieler (handgebaute Transportpakete, seit W334 als SBT2) ----------
 * Der Test-Mitspieler kennt den Testschluessel des Wegs ohne Treffpunkt (SECURITY.md §4.7) und kann daher
 * authentische Pakete mit beliebigem Inhalt bauen - genau die Lage eines eingeladenen, boeswilligen Mitspielers. */
typedef struct fpeer {
    SOCKET s;                 /* eigener UDP-Socket des "Mitspielers" */
    struct sockaddr_in b;     /* Transport-Adresse von B (aus dem Durchstich) */
    int have_b;
    uint32_t vip;             /* gemeldete virtuelle Adresse */
    uint32_t seq, acked, pseq;
    uint32_t fin_mask;        /* S_FIN von B fuer Strom-ID < 32 */
    sbe2e_link e2e;           /* Schluessel (Seite "hi": Port 44950 > 44900) */
    uint8_t last[1200];       /* zuletzt gesendetes Paket (Wiederholungstest) */
    int last_len;
    int probes_seen;          /* empfangene, authentische PROBE von B */
    int rt_pong, rt_pong_bad, small_pong; /* W395: Echos auf Wegproben (8 B, gleich), abweichend, 4-B-PONG */
    uint8_t rt_last[8];       /* W395: zuletzt gesendete Wegprobe */
} fpeer;

static void fp_put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void fp_put32(uint8_t *p, uint32_t v) { fp_put16(p, (uint16_t)v); fp_put16(p + 2, (uint16_t)(v >> 16)); }
static uint32_t fp_get32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

/* Klartext [Typ][Rumpf] versiegeln und an B senden. */
static void fp_send(fpeer *f, const uint8_t *pt, int n)
{
    int k = sbe2e_seal(&f->e2e, pt, n, f->last, sizeof f->last);
    if (k <= 0 || !f->have_b) return;
    f->last_len = k;
    sendto(f->s, (const char *)f->last, k, 0, (struct sockaddr *)&f->b, sizeof f->b);
}

static void fp_hello(fpeer *f)
{
    uint8_t p[9];
    p[0] = 1; /* T_HELLO */
    fp_put32(p + 1, f->vip);
    p[5] = 1;
    p[6] = 2;
    fp_put16(p + 7, 0);
    fp_send(f, p, sizeof p);
}

/* Ein T_DATA mit einem Eintrag (K_STREAM: sub/stream/data mit Folgenummer seq; K_DGRAM: src/dst/data). */
static void fp_data(fpeer *f, int kind, uint32_t seq, int sub, uint16_t stream, uint16_t dst, const uint8_t *d, int n)
{
    uint8_t p[1168];
    int off = 22;
    memset(p, 0, 22);
    p[0] = 3; /* T_DATA */
    fp_put32(p + 1, ++f->pseq);
    p[21] = 1;
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
    fp_send(f, p, off);
}

/* Pakete von B verarbeiten (Durchstich beantworten, Bestaetigung + FIN mitlesen), ms Millisekunden. */
static void fp_pump(fpeer *f, int ms)
{
    double t0 = now_ms();
    do {
        uint8_t b[2048], pt[1200];
        struct sockaddr_in from;
        int fl = sizeof from, k, n;
        while ((k = recvfrom(f->s, (char *)b, sizeof b, 0, (struct sockaddr *)&from, &fl)) > 0) {
            fl = sizeof from;
            n = sbe2e_open(&f->e2e, b, k, pt, sizeof pt);
            if (n < 1) continue;
            if (pt[0] == 7 && n == 9) { /* PROBE -> PROBE_ACK an die Absenderadresse */
                uint8_t q[9];
                f->b = from;
                f->have_b = 1;
                f->probes_seen++;
                q[0] = 8;
                memcpy(q + 1, pt + 1, 8);
                fp_send(f, q, 9);
            } else if (pt[0] == 5) { /* W395: T_PONG */
                if (n == 9 && memcmp(pt + 1, f->rt_last, 8) == 0) f->rt_pong++;
                else if (n == 9) f->rt_pong_bad++;
                else if (n == 5) f->small_pong++;
            } else if (pt[0] == 3 && n >= 22) {
                uint32_t ack = fp_get32(pt + 5);
                int cnt = pt[21], off = 22, i;
                if ((int32_t)(ack - f->acked) > 0) f->acked = ack;
                for (i = 0; i < cnt && off < n; i++) {
                    if (pt[off] == 1 && off + 11 <= n) {
                        int ml = pt[off + 9] | (pt[off + 10] << 8);
                        int st = pt[off + 6] | (pt[off + 7] << 8);
                        if (pt[off + 5] == 3 && st < 32) f->fin_mask |= 1u << st;
                        off += 11 + ml;
                    } else if (pt[off] == 2 && off + 10 <= n) {
                        off += 10 + (pt[off + 8] | (pt[off + 9] << 8));
                    } else {
                        break;
                    }
                }
            }
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

/* Instanz B mit Transport und festem Test-Mitspieler f (Port 44950); vip = 0: keine Verbindung aufbauen. */
static int gt_start(locnet *B, fpeer *f, uint32_t vip)
{
    loctun_cfg c;
    struct sockaddr_in a;
    u_long nb = 1;
    int i;
    memset(f, 0, sizeof *f);
    if (!locnet_init(B, "127.0.0.45", 44000, "none", 10, lg)) return 0;
    f->s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    a = sa(htonl(INADDR_LOOPBACK), 44950);
    bind(f->s, (struct sockaddr *)&a, sizeof a);
    ioctlsocket(f->s, (long)FIONBIO, &nb);
    if (sbe2e_link_test(0, &f->e2e) != 0) return 0;
    loctun_cfg_default(&c);
    c.port = 44900;
    c.proxy_base = 45000; /* Stellvertreter fuer die vip des "Mitspielers" */
    strcpy_s(c.peers, sizeof c.peers, "127.0.0.1:44950");
    B->tun = loctun_start(&c, B, NULL, lg);
    if (!B->tun) return 0;
    for (i = 0; i < 200 && !f->have_b; i++) fp_pump(f, 10);
    if (!f->have_b) return 0;
    if (!vip) return 1;
    f->vip = vip;
    for (i = 0; i < 100 && loctun_links_up(B->tun) == 0; i++) {
        fp_hello(f);
        fp_pump(f, 10);
    }
    return loctun_links_up(B->tun) == 1;
}

static void gt_stop(locnet *B, fpeer *f)
{
    if (f->s != INVALID_SOCKET && f->s) closesocket(f->s);
    sbe2e_link_free(&f->e2e);
    locnet_shutdown(B);
}

/* Spiel lauscht auf TCP 1000, Mitspieler oeffnet einen Strom dorthin. INVALID_SOCKET bei Fehler. */
static SOCKET gt_open1000(locnet *B, fpeer *f, SOCKET *glis, uint16_t stream)
{
    struct sockaddr_in a = sa(INADDR_ANY, 1000);
    uint8_t od[4];
    SOCKET g = INVALID_SOCKET;
    *glis = locnet_socket(B, AF_INET, SOCK_STREAM, IPPROTO_TCP);
    locnet_bind(B, *glis, (struct sockaddr *)&a, sizeof a);
    listen(*glis, 4);
    fp_put16(od, 1000);
    fp_put16(od + 2, 0);
    if (fp_stream(f, 1, stream, od, 4) && fp_tcp_pending(*glis, 1000)) g = accept(*glis, NULL, NULL);
    return g;
}

/* W334: boeswillige Rahmen (SECURITY.md §12 B): kein Byte an den Gast, Partner getrennt. */
static void gt_bad_frame(const char *name, const uint8_t *fr, int n, int udp)
{
    locnet B;
    fpeer f;
    SOCKET glis = INVALID_SOCKET, g = INVALID_SOCKET, gu = INVALID_SOCKET;
    char msg[160], buf[64];
    int got = 0, i;
    LONG gd;
    if (!gt_start(&B, &f, ip4("127.0.0.46"))) {
        _snprintf_s(msg, sizeof msg, _TRUNCATE, "W334: %s - Verbindung zum Test-Mitspieler", name);
        CHECK(0, msg);
        gt_stop(&B, &f);
        return;
    }
    gd = loctun_get_stats(B.tun)->guard;
    if (udp) {
        struct sockaddr_in from;
        gu = game_udp(&B, 1202, IPPROTO_UDP);
        fp_data(&f, 2, 0, 0, 0, 1202, fr, n);
        fp_pump(&f, 300);
        got = poll_recvfrom(&B, gu, buf, sizeof buf, &from) > 0;
    } else {
        g = gt_open1000(&B, &f, &glis, 2);
        if (g != INVALID_SOCKET) {
            fp_stream(&f, 2, 2, fr, n);
            fp_pump(&f, 300);
            if (fp_tcp_pending(g, 300)) got = recv(g, buf, sizeof buf, 0) > 0;
        } else {
            got = -1;
        }
    }
    for (i = 0; i < 50 && loctun_links_up(B.tun) != 0; i++) Sleep(10);
    _snprintf_s(msg, sizeof msg, _TRUNCATE, "W334: %s - kein Byte an den Gast, Mitspieler getrennt", name);
    CHECK(got == 0 && loctun_links_up(B.tun) == 0 && loctun_get_stats(B.tun)->guard > gd, msg);
    if (g != INVALID_SOCKET) closesocket(g);
    if (glis != INVALID_SOCKET) closesocket(glis);
    if (gu != INVALID_SOCKET) closesocket(gu);
    gt_stop(&B, &f);
}

static void tun_guard_tests(void)
{
    locnet B;
    fpeer f;
    struct sockaddr_in a;
    u_long nb = 1;
    char buf[2048];
    SOCKET vic_u, vic_t, g1202, glis, gacc = INVALID_SOCKET;
    int i, k, ok;
    if (!gt_start(&B, &f, 0)) {
        CHECK(0, "W329: Durchstich zum Test-Mitspieler (SBT2-PROBE)");
        gt_stop(&B, &f);
        return;
    }
    CHECK(f.have_b && f.probes_seen > 0, "W329: Durchstich zum Test-Mitspieler (SBT2-PROBE)");

    /* W334: Altformate SBT1/SBP1 werden ohne Ausnahme verworfen (kein Klartext-Rueckfall) */
    {
        uint8_t old[18];
        LONG eb = loctun_get_stats(B.tun)->e2e_bad;
        memcpy(old, "SBT1", 4);
        old[4] = 1;
        old[5] = 0;
        memcpy(old + 6, "SBT1", 4);
        fp_put32(old + 10, ip4("127.0.0.46"));
        old[14] = 1;
        old[15] = 1;
        fp_put16(old + 16, 0);
        for (i = 0; i < 20; i++) {
            sendto(f.s, (const char *)old, sizeof old, 0, (struct sockaddr *)&f.b, sizeof f.b);
            fp_pump(&f, 10);
        }
        memcpy(old, "SBP1", 4); /* alter PROBE */
        old[4] = 1;
        memcpy(old + 5, "SBT1TEST", 8);
        sendto(f.s, (const char *)old, 13, 0, (struct sockaddr *)&f.b, sizeof f.b);
        fp_pump(&f, 50);
        CHECK(loctun_links_up(B.tun) == 0 && loctun_get_stats(B.tun)->e2e_bad >= eb + 21,
              "W334: SBT1-HELLO und SBP1-PROBE verworfen, keine Verbindung");
    }
    /* W334: fremde sid / fremder Schluessel / veraendertes Tag / Wiederholung -> verworfen */
    {
        sbe2e_link other;
        uint8_t prk[32], pkt[64], p9[9];
        LONG eb = loctun_get_stats(B.tun)->e2e_bad;
        int n;
        memset(prk, 0x5C, sizeof prk);
        sbe2e_link_from_prk(prk, 0, &other);
        p9[0] = 1;
        fp_put32(p9 + 1, ip4("127.0.0.46"));
        p9[5] = 1;
        p9[6] = 2;
        fp_put16(p9 + 7, 0);
        n = sbe2e_seal(&other, p9, 9, pkt, sizeof pkt);
        sendto(f.s, (const char *)pkt, n, 0, (struct sockaddr *)&f.b, sizeof f.b); /* fremde sid */
        other.sid = f.e2e.sid;
        n = sbe2e_seal(&other, p9, 9, pkt, sizeof pkt);
        sendto(f.s, (const char *)pkt, n, 0, (struct sockaddr *)&f.b, sizeof f.b); /* richtige sid, falscher Schluessel */
        n = sbe2e_seal(&f.e2e, p9, 9, pkt, sizeof pkt);
        pkt[n - 1] ^= 0x01;
        sendto(f.s, (const char *)pkt, n, 0, (struct sockaddr *)&f.b, sizeof f.b); /* Tag veraendert */
        fp_pump(&f, 100);
        CHECK(loctun_links_up(B.tun) == 0 && loctun_get_stats(B.tun)->e2e_bad >= eb + 3,
              "W334: fremde sid, fremder Schluessel und veraendertes Tag verworfen");
        sbe2e_link_free(&other);
    }

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
    /* W332: vip = eigene Adresse wird abgelehnt - aber nur EINE Logzeile, nicht eine je HELLO (Log-Flut) */
    f.vip = B.self_ip;
    InterlockedExchange(&s_lg_selfvip, 0);
    for (i = 0; i < 30; i++) { fp_hello(&f); fp_pump(&f, 5); }
    CHECK(loctun_links_up(B.tun) == 0 && s_lg_selfvip == 1,
          "W332: HELLO mit eigener vip abgelehnt, nur eine Logzeile (keine Log-Flut je Paket)");
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
    /* W334: Wiederholung eines authentischen Pakets wird verworfen (Fenster) */
    {
        LONG eb = loctun_get_stats(B.tun)->e2e_bad;
        fp_hello(&f);
        sendto(f.s, (const char *)f.last, f.last_len, 0, (struct sockaddr *)&f.b, sizeof f.b);
        fp_pump(&f, 50);
        CHECK(loctun_get_stats(B.tun)->e2e_bad == eb + 1, "W334: Wiederholung eines gueltigen Pakets verworfen");
    }

    /* 2) UDP: nur an Ports, die das Spiel gebunden hat - nie an fremde lokale Dienste */
    vic_u = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    a = sa(B.self_ip, 44777);
    bind(vic_u, (struct sockaddr *)&a, sizeof a);
    ioctlsocket(vic_u, (long)FIONBIO, &nb);
    g1202 = game_udp(&B, 1202, IPPROTO_UDP);
    memset(buf, 0, 0x2C4);
    buf[0] = 3; /* gueltiger Rahmen (Beitritt) - nur der Zielport ist falsch */
    fp_data(&f, 2, 0, 0, 0, 44777, (const uint8_t *)buf, 0x2C4);
    fp_data(&f, 2, 0, 0, 0, 1999, (const uint8_t *)buf, 0x2C4); /* Spielbereich, aber nicht gebunden */
    fp_data(&f, 2, 0, 0, 0, 1202, (const uint8_t *)buf, 0x2C4);
    fp_pump(&f, 200);
    k = recv(vic_u, buf, sizeof buf, 0);
    CHECK(k <= 0, "W329: Datagramm des Mitspielers an fremden lokalen UDP-Port verworfen");
    {
        struct sockaddr_in from;
        k = poll_recvfrom(&B, g1202, buf, sizeof buf, &from);
        CHECK(k == 0x2C4, "W329: Datagramm an gebundenen Spielport 1202 kommt an (Gegenprobe)");
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
    /* W332: Log-Obergrenze - 650 Stroeme an Spiel-TCP 1000 (OPEN, Spiel nimmt an und schliesst, FIN) erzeugen je
     * eine Zeile; hoechstens 200 je 10-s-Fenster duerfen ins Log (sonst fuellt der Mitspieler die Platte). Ein
     * Zeitraum < 10 s beruehrt hoechstens zwei Fenster -> <= 400 Zeilen, ohne Grenze 650. */
    {
        uint8_t od[4];
        LONG n0 = s_lg_n;
        int sent = 0;
        double t0, el;
        SOCKET tl1000 = locnet_socket(&B, AF_INET, SOCK_STREAM, IPPROTO_TCP);
        a = sa(INADDR_ANY, 1000);
        locnet_bind(&B, tl1000, (struct sockaddr *)&a, sizeof a);
        listen(tl1000, 64);
        ioctlsocket(tl1000, (long)FIONBIO, &nb);
        fp_put16(od, 1000);
        fp_put16(od + 2, 0);
        n0 = s_lg_n;
        t0 = now_ms();
        for (i = 0; i < 650; i += 25) { /* je 25: OPEN, Spiel nimmt an und schliesst, FIN (Stroeme wieder frei) */
            int j, got = 0;
            double tw = now_ms();
            for (j = i; j < i + 25 && j < 650; j++) sent += fp_stream(&f, 1, (uint16_t)(1000 + 2 * j), od, 4);
            while (got < j - i && now_ms() - tw < 2000) {
                SOCKET g = accept(tl1000, NULL, NULL);
                if (g != INVALID_SOCKET) {
                    closesocket(g);
                    got++;
                } else {
                    fp_pump(&f, 1);
                }
            }
            for (j = i; j < i + 25 && j < 650; j++) fp_stream(&f, 3, (uint16_t)(1000 + 2 * j), NULL, 0);
        }
        el = now_ms() - t0;
        fp_pump(&f, 300);
        closesocket(tl1000);
        printf("W332: %d Stroeme in %.0f ms -> %ld Logzeilen\n", sent, el, s_lg_n - n0);
        CHECK(sent == 650 && s_lg_n - n0 >= 150 && s_lg_n - n0 <= 200 * (2 + (int)(el / 10000.0)) + 1,
              "W332: Log-Flut durch Mitspieler auf 200 Zeilen je 10-s-Fenster begrenzt");
        /* < 15 s: ohne Grenze kaemen mehr als 400 Zeilen -> Unterdrueckung pruefbar; sonst (Rechner ausgelastet) Hinweis */
        CHECK(el >= 15000.0 || s_lg_n - n0 < sent, "W332: Log-Flut - Zeilen ueber der Obergrenze unterdrueckt");
        if (el >= 15000.0) printf("Hinweis: Strom-Flut dauerte %.0f ms - Unterdrueckung nicht pruefbar\n", el);
    }
    gacc = gt_open1000(&B, &f, &glis, 4);
    CHECK(gacc != INVALID_SOCKET, "W329: OPEN an Spiel-TCP 1000 verbunden (Gegenprobe)");

    /* W334: gueltiger Lockstep-Rahmen kommt durch den Filter (Gegenprobe zu den Verstoessen unten) */
    if (gacc != INVALID_SOCKET) {
        uint8_t fr[26];
        memset(fr, 0, sizeof fr);
        fr[0] = 0x34;
        fr[4] = 1;
        fp_stream(&f, 2, 4, fr, 13); /* geteilt: Kopf in zwei Nachrichten */
        fp_stream(&f, 2, 4, fr + 13, 13);
        k = fp_tcp_pending(gacc, 1000) ? recv(gacc, buf, sizeof buf, 0) : 0;
        if (k > 0 && k < 26 && fp_tcp_pending(gacc, 500)) k += recv(gacc, buf + k, (int)sizeof buf - k, 0);
        CHECK(k == 26 && (uint8_t)buf[0] == 0x34, "W334: gueltiger 0x34-Rahmen (n = 1, geteilt) erreicht das Spiel ganz");
    }

    /* 4) Puffer zum Spiel begrenzt: Spiel liest nicht, Mitspieler sendet weiter -> Strom zu, kein Speicherlauf */
    if (gacc != INVALID_SOCKET) {
        static uint8_t chunk[1000];
        int sent = 0;
        uint32_t pos = 0;
        for (i = 0; i < 12000 && !(f.fin_mask & (1u << 4)); i++) { /* bis 12 MB, gueltige 0x22-Rahmen (0x100C B) */
            int j;
            for (j = 0; j < (int)sizeof chunk; j++, pos++) {
                uint32_t o = pos % 0x100C;
                chunk[j] = (uint8_t)(o == 0 ? 0x22 : (o == 1 ? 0 : (o == 5 ? 0x10 : (o >= 4 && o <= 7 ? 0 : 0x33)))); /* W339: Laenge 0x1000 */
            }
            if (!fp_stream(&f, 2, 4, chunk, sizeof chunk)) break;
            sent++;
        }
        fp_pump(&f, 300);
        printf("W329: Puffergrenze nach %d KB ohne Lesen des Spiels\n", sent);
        CHECK(f.fin_mask & (1u << 4), "W329: Puffer zum Spiel voll -> Strom geschlossen (FIN), kein unbegrenzter Speicher");
        closesocket(gacc);
    }
    /* W395 LOC-ROUTE: Wegproben eines boeswilligen Mitspielers (T_PING mit Kennung "RT", SECURITY.md §26) */
    {
        uint8_t q[16];
        loctun_peer_hud h;
        memset(q, 0, sizeof q);
        q[0] = 4; /* T_PING */
        q[1] = 'R'; q[2] = 'T'; q[3] = 0; q[4] = 0; q[5] = 0x34; q[6] = 0x12;
        memcpy(f.rt_last, q + 1, 8);
        f.rt_pong = f.rt_pong_bad = f.small_pong = 0;
        fp_send(&f, q, 9);
        fp_pump(&f, 150);
        CHECK(f.rt_pong == 1 && f.rt_pong_bad == 0, "W395: Wegprobe auf dem Ankunftsweg gleich gross beantwortet (Echo)");
        f.rt_pong = 0;
        fp_send(&f, q, 8);   /* 7 Byte Rumpf: alte PING-Regel (4-Byte-PONG, kleiner als die Anfrage) */
        fp_send(&f, q, 10);  /* 9 Byte Rumpf: ebenso */
        q[2] = 'X';
        fp_send(&f, q, 9);   /* falsche Kennung: alte PING-Regel */
        fp_pump(&f, 150);
        CHECK(f.rt_pong == 0 && f.rt_pong_bad == 0 && f.small_pong == 3,
              "W395: Wegprobe mit falscher Laenge/Kennung -> nur die alte 4-Byte-Antwort, kein Echo");
        q[2] = 'T';
        f.rt_pong = f.small_pong = 0;
        for (i = 0; i < 400; i++) {
            q[5] = (uint8_t)i;
            memcpy(f.rt_last, q + 1, 8);
            fp_send(&f, q, 9);
            if (i % 50 == 0) fp_pump(&f, 0);
        }
        fp_pump(&f, 200);
        printf("W395: 400 Wegproben in Folge -> %d Echos\n", f.rt_pong + f.rt_pong_bad);
        CHECK(f.rt_pong + f.rt_pong_bad <= 130, "W395: Echos auf Wegproben gebremst (100/s je Verbindung)");
        for (i = 0; i < 200; i++) { /* gefaelschte Echos (Slot/Folge beliebig) und Unsinn */
            q[0] = 5;
            q[3] = (uint8_t)(i * 37);
            q[4] = (uint8_t)i;
            q[5] = (uint8_t)(i * 11);
            fp_send(&f, q, 9 + (i % 3) - 1);
        }
        q[0] = 1; /* HELLO mit allen Flag-Bits */
        fp_put32(q + 1, f.vip);
        q[5] = 1; q[6] = 2; q[7] = 0xFF; q[8] = 0xFF;
        fp_send(&f, q, 9);
        fp_pump(&f, 200);
        memset(&h, 0, sizeof h);
        CHECK(loctun_links_up(B.tun) == 1 && loctun_peer_hud_get(B.tun, 0, &h) && h.up && !h.rt_on && h.nswitch == 0,
              "W395: gefaelschte Echos/Flags ohne Wirkung (ohne Treffpunkt keine eigene Wegwahl), Verbindung steht");
    }
    closesocket(glis);
    closesocket(vic_t);
    closesocket(vic_u);
    closesocket(g1202);
    gt_stop(&B, &f);

    /* W334: Gastfilter gegen boeswillige Rahmen (SECURITY.md §6, §12 B) - je Fall eine frische Verbindung */
    {
        uint8_t fr[32];
        memset(fr, 0, sizeof fr);
        fr[0] = 0x34;
        fr[4] = 0xFF; /* n = -1: Spielzerleger rechnet negative Laenge (rep movsd mit riesigem Zaehler) */
        gt_bad_frame("TCP 0x34 mit n = -1", fr, 26, 0);
        fr[4] = 2;    /* n = 2: Handler ueberschreibt den 16-Byte-Schlitz (Stufe 2) */
        gt_bad_frame("TCP 0x34 mit n = 2", fr, 26, 0);
        memset(fr, 0, sizeof fr);
        fr[0] = 0x09; /* Laenge 0 in der Spieltabelle (Zerleger tritt auf der Stelle) */
        gt_bad_frame("TCP Typ 0x09", fr, 4, 0);
        memset(fr, 0, sizeof fr);
        fr[0] = 0x09; /* UDP 0x09: Stufe 1 gesperrt */
        gt_bad_frame("UDP 0x09 an 1202", fr, 4, 1);
        fr[0] = 0x06; /* UDP 0x06 mit falscher Laenge */
        gt_bad_frame("UDP 0x06 mit 4 statt 0x308 Byte", fr, 4, 1);
    }
}

/* ---------- W334 LOC-SEC-CLIENT: Krypto, Fenster, Gastfilter gegen net/tools/sbt2_vectors.json ---------- */
static int hexeq(const char *v, const uint8_t *p, int n)
{
    uint8_t b[2048];
    int k = jm_hex(v, b, sizeof b);
    return k == n && memcmp(b, p, (size_t)n) == 0;
}

static void sec_unit_tests(void)
{
    char why[96];
    int t, ok;
    CHECK(sbe2e_selftest(why, sizeof why) == 0, "W334: Start-Selbsttest (X25519 RFC 7748, HKDF TC1, GCM TC16, SBT2-Paket)");
    for (t = 1; t <= 4; t++) {
        char msg[120];
        ok = sbe2e_selftest_ex(why, sizeof why, t) != 0 && why[0];
        _snprintf_s(msg, sizeof msg, _TRUNCATE, "W334: Selbsttest erkennt verfaelschten Wert %d (%s)", t, why);
        CHECK(ok, msg);
    }
    { /* Schluesselpaar, beide Seiten leiten dieselben Schluessel ab, Paket hin und zurueck */
        sbe2e_kp ka, kb;
        sbe2e_side sa_, sb_;
        sbe2e_link la, lb;
        uint8_t th[32], pkt[80], pt[80], msg[] = {3, 9, 8, 7, 6};
        int n;
        memset(&sa_, 0, sizeof sa_);
        memset(&sb_, 0, sizeof sb_);
        ok = sbe2e_keypair(&ka, sa_.pub) == 0 && sbe2e_keypair(&kb, sb_.pub) == 0;
        sa_.cid = 5;
        sb_.cid = 6;
        strcpy_s(sa_.pilot, sizeof sa_.pilot, "A");
        strcpy_s(sb_.pilot, sizeof sb_.pilot, "Guest-B");
        ok = ok && sbe2e_transcript(th, 1, (const uint8_t *)"0123456789abcdef", &sa_, &sb_) == 0;
        ok = ok && sbe2e_derive(&ka, sb_.pub, th, 5, 6, &la) == 0 && sbe2e_derive(&kb, sa_.pub, th, 6, 5, &lb) == 0;
        ok = ok && la.sid == lb.sid;
        n = ok ? sbe2e_seal(&la, msg, sizeof msg, pkt, sizeof pkt) : -1;
        ok = ok && sbe2e_open(&lb, pkt, n, pt, sizeof pt) == (int)sizeof msg && memcmp(pt, msg, sizeof msg) == 0;
        n = ok ? sbe2e_seal(&lb, msg, sizeof msg, pkt, sizeof pkt) : -1;
        ok = ok && sbe2e_open(&la, pkt, n, pt, sizeof pt) == (int)sizeof msg;
        CHECK(ok, "W334: fluechtige X25519-Paare (CNG), gleiche Paarschluessel auf beiden Seiten, SBT2 hin und zurueck");
        /* anderer Pilotname im Transkript -> andere Schluessel, keine Verbindung (§4.3) */
        strcpy_s(sb_.pilot, sizeof sb_.pilot, "Guest-X");
        sbe2e_link_free(&lb);
        ok = sbe2e_transcript(th, 1, (const uint8_t *)"0123456789abcdef", &sa_, &sb_) == 0 &&
             sbe2e_derive(&kb, sa_.pub, th, 6, 5, &lb) == 0;
        n = sbe2e_seal(&la, msg, sizeof msg, pkt, sizeof pkt);
        CHECK(ok && sbe2e_open(&lb, pkt, n, pt, sizeof pt) < 0, "W334: abweichende Identitaet im Transkript -> Paket nicht oeffenbar");
        CHECK(sbe2e_derive(&ka, sb_.pub, th, 5, 5, &lb) != 0, "W334: gleiche client_id -> Ableitung abgelehnt");
        sbe2e_link_free(&la);
        sbe2e_link_free(&lb);
        sbe2e_kp_free(&ka);
        sbe2e_kp_free(&kb);
    }
    { /* Paketgrenze 2^32 (kein Schluesselwechsel im Betrieb) */
        sbe2e_link l;
        uint8_t pkt[64], m[] = {4, 1, 2, 3, 4};
        sbe2e_link_test(1, &l);
        l.tx_ctr = SBE2E_CTR_MAX - 1;
        ok = sbe2e_seal(&l, m, sizeof m, pkt, sizeof pkt) > 0 && sbe2e_exhausted(&l) &&
             sbe2e_seal(&l, m, sizeof m, pkt, sizeof pkt) < 0;
        CHECK(ok, "W334: ctr 2^32 ist das letzte Paket, danach verweigert seal (Verbindung beenden)");
        ok = sbe2e_seal(&l, (const uint8_t *)"\x02\x00", 2, pkt, sizeof pkt) < 0;
        sbe2e_link_free(&l);
        sbe2e_link_test(1, &l);
        CHECK(ok && sbe2e_seal(&l, (const uint8_t *)"\x09\x00", 2, pkt, sizeof pkt) < 0,
              "W334: unbekannter innerer Typ wird nicht versiegelt");
        sbe2e_link_free(&l);
    }
    { /* Gastfilter ohne Puffer (HLE-Hooks) */
        lguard_walk w;
        uint8_t s[64];
        int why2 = 0;
        memset(&w, 0, sizeof w);
        memset(s, 0, sizeof s);
        s[0] = 0x34;
        s[4] = 1; /* 26 B */
        s[26] = 0x37; /* 4 B */
        ok = lguard_walk_tcp(&w, s, 4, 1, &why2) == 0; /* 0x34-Kopf ohne n: zurueckhalten */
        ok = ok && lguard_walk_tcp(&w, s, 30, 1, &why2) == 30 && w.rem == 0;
        ok = ok && lguard_walk_tcp(&w, s, 1, 1, &why2) == 0; /* 1 Byte Typ: zurueckhalten */
        s[0] = 0x34;
        s[4] = 0x80;
        ok = ok && lguard_walk_tcp(&w, s, 26, 1, &why2) < 0 && why2 == LGUARD_V_NEG;
        s[4] = 3;
        ok = ok && lguard_walk_tcp(&w, s, 26, 1, &why2) < 0 && why2 == LGUARD_V_CONTENT;
        s[0] = 0x15;
        ok = ok && lguard_walk_tcp(&w, s, 4, 1, &why2) < 0 && why2 == LGUARD_V_TYPE;
        CHECK(ok, "W334: Gastfilter-Laeufer (HLE recv): Koepfe erst entschieden freigeben, 0x34 n>=0x80 / n>1, Typ 0x15 = Verstoss");
    }
}

static SOCKET s_late_sock;
static DWORD WINAPI late_rest(LPVOID p)
{
    static const char rest[23] = {0, 1};
    (void)p;
    Sleep(100);
    send(s_late_sock, rest, 23, 0);
    return 0;
}

/* HLE-recv des Spiels: jeder Weg (auch ohne Transport) geht durch den Filter. */
static void hle_recv_tests(void)
{
    locnet A;
    SOCKET l, c, s;
    struct sockaddr_in a;
    int al = sizeof a, k;
    char b[64];
    u_long nb = 1;
    if (!locnet_init(&A, "127.0.0.25", 46000, "none", 10, lg)) {
        CHECK(0, "W334: HLE-recv-Test startet");
        return;
    }
    l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    a = sa(ip4("127.0.0.25"), 46990);
    bind(l, (struct sockaddr *)&a, sizeof a);
    listen(l, 2);
    c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    connect(c, (struct sockaddr *)&a, sizeof a);
    s = accept(l, (struct sockaddr *)&a, &al);
    ioctlsocket(s, (long)FIONBIO, &nb);
    send(c, "\x34\x00\x00", 3, 0); /* Kopf unvollstaendig: Typ 0x34, n fehlt noch */
    Sleep(30);
    {
        HANDLE th;
        s_late_sock = c;
        th = CreateThread(NULL, 0, late_rest, NULL, 0, NULL); /* Rest nach 100 ms */
        int k0, e0, polls = 0;
        double t0 = now_ms();
        k0 = locnet_recv(&A, s, b, sizeof b, 0); /* Kopf unentschieden: sofort "noch keine Daten", nie blockieren */
        e0 = WSAGetLastError();
        CHECK(k0 == SOCKET_ERROR && e0 == WSAEWOULDBLOCK && now_ms() - t0 < 50.0,
              "W334: HLE recv blockiert nie bei unentschiedenem Kopf (WSAEWOULDBLOCK, Spiel-Thread frei)");
        do { /* das Spiel fragt wieder (nicht blockierender Socket) */
            Sleep(5);
            k = locnet_recv(&A, s, b, sizeof b, 0);
            polls++;
        } while (k == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK && now_ms() - t0 < 1900.0);
        WaitForSingleObject(th, 2000);
        CloseHandle(th);
        if (k != 26) printf("HLE recv Kopf: k=%d err=%d polls=%d nach %.0f ms\n", k, WSAGetLastError(), polls, now_ms() - t0);
        CHECK(k == 26 && b[0] == 0x34 && b[4] == 1 && polls >= 1,
              "W334: HLE recv haelt einen unentschiedenen Kopf zurueck und liefert dann den ganzen Rahmen");
    }
    send(c, "\x37\x00\x00\x00\x34\x00\x00\x00\xFF", 9, 0); /* 0x37 gueltig, dann 0x34 mit n = -1 */
    Sleep(30);
    k = locnet_recv(&A, s, b, sizeof b, 0); /* fail closed: auch der gueltige Rahmen davor geht nicht mehr hinaus */
    CHECK(k == SOCKET_ERROR && WSAGetLastError() == WSAECONNRESET && A.st.guard > 0 && b[4] == 0,
          "W334: HLE recv: 0x34 mit negativer Anzahl -> Verbindung getrennt, kein Byte zum Spiel");
    k = locnet_recv(&A, s, b, sizeof b, 0);
    CHECK(k == SOCKET_ERROR && WSAGetLastError() == WSAECONNRESET, "W334: HLE recv: getrennter Strom bleibt getrennt");
    closesocket(c);
    locnet_closesocket(&A, s); /* wie das Spiel ueber den Hook: Filterzustand des Handles vergessen */
    /* Das Spiel liest seinen TCP-Strom auch ueber recvfrom (W334-Lauf L1): gleicher TCP-Filter, kein UDP-Filter */
    c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    a = sa(ip4("127.0.0.25"), 46990); /* accept hat a mit der Gegenadresse ueberschrieben */
    connect(c, (struct sockaddr *)&a, sizeof a);
    al = sizeof a;
    s = accept(l, (struct sockaddr *)&a, &al);
    ioctlsocket(s, (long)FIONBIO, &nb);
    send(c, "\x37\x00\x00\x00", 4, 0); /* gueltiger Rahmen 0x37 (4 Byte) */
    Sleep(30);
    {
        struct sockaddr_in fr;
        int fl = sizeof fr;
        memset(&fr, 0x5A, sizeof fr);
        memset(b, 0, sizeof b);
        k = locnet_recvfrom(&A, s, b, sizeof b, 0, (struct sockaddr *)&fr, &fl);
        CHECK(k == 4 && b[0] == 0x37, "W334: HLE recvfrom auf dem TCP-Strom liefert gueltige Rahmen (TCP-Filter)");
    }
    {
        static const char bad[26] = {0x34, 0, 0, 0, 2}; /* 0x34 mit n = 2 (Stufe 2) */
        send(c, bad, sizeof bad, 0);
    }
    Sleep(30);
    memset(b, 0, sizeof b);
    k = locnet_recvfrom(&A, s, b, sizeof b, 0, NULL, NULL);
    CHECK(k == SOCKET_ERROR && WSAGetLastError() == WSAECONNRESET && b[0] == 0,
          "W334: HLE recvfrom auf dem TCP-Strom: 0x34 mit n = 2 -> Verbindung getrennt, kein Byte zum Spiel");
    closesocket(c);
    locnet_closesocket(&A, s); /* wie das Spiel ueber den Hook: Filterzustand des Handles vergessen */
    /* Kopf bleibt > 2 s unentschieden (Mitspieler schickt den Rest nie) -> getrennt, ohne dass recv je wartet */
    c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    a = sa(ip4("127.0.0.25"), 46990);
    connect(c, (struct sockaddr *)&a, sizeof a);
    al = sizeof a;
    s = accept(l, (struct sockaddr *)&a, &al);
    ioctlsocket(s, (long)FIONBIO, &nb);
    send(c, "\x34\x00\x00\x00", 4, 0); /* 0x34 ohne Anzahl-Byte */
    {
        double t0 = now_ms(), worst = 0;
        int e = 0;
        do {
            double t1;
            Sleep(20);
            t1 = now_ms();
            k = locnet_recv(&A, s, b, sizeof b, 0);
            e = WSAGetLastError();
            if (now_ms() - t1 > worst) worst = now_ms() - t1;
        } while (k == SOCKET_ERROR && e == WSAEWOULDBLOCK && now_ms() - t0 < 3000.0);
        printf("HLE recv unentschieden: getrennt nach %.0f ms, laengster recv-Aufruf %.1f ms\n", now_ms() - t0, worst);
        CHECK(k == SOCKET_ERROR && e == WSAECONNRESET && now_ms() - t0 >= 1900.0 && worst < 200.0,
              "W334: HLE recv: Kopf > 2 s unentschieden -> Verbindung getrennt (kein Warten im Spiel-Thread)");
    }
    closesocket(c);
    locnet_closesocket(&A, s); /* wie das Spiel ueber den Hook: Filterzustand des Handles vergessen */
    closesocket(l);
    locnet_shutdown(&A);
}

/* W350 LOC-REJOIN: Abgang eines Mitspielers. Das Spiel wertet recv 0 nicht als Fehler und haelt den Platz (Geist);
 * das Netz-HLE meldet das Stromende als WSAECONNRESET (wie XNet), dauerhaft fuer diesen Socket. Wiederbeitritt von
 * derselben Adresse = neue Verbindung mit frischem Zustand, genau ein lebender Strom. */
static SOCKET w350_accept(locnet *A, SOCKET l, int ms)
{
    int i;
    for (i = 0; i < ms / 5; i++) {
        struct sockaddr_in r;
        int rl = sizeof r;
        SOCKET s = locnet_accept(A, l, (struct sockaddr *)&r, &rl);
        if (s != INVALID_SOCKET) return s;
        Sleep(5);
    }
    return INVALID_SOCKET;
}
/* liest, bis ein Ergebnis != WSAEWOULDBLOCK kommt (hoechstens ms); *err = WSAGetLastError() */
static int w350_recv(locnet *A, SOCKET s, char *b, int len, int ms, int *err)
{
    double t0 = now_ms();
    int k;
    do {
        k = locnet_recv(A, s, b, len, 0);
        *err = k == SOCKET_ERROR ? WSAGetLastError() : 0;
        if (!(k == SOCKET_ERROR && *err == WSAEWOULDBLOCK)) return k;
        Sleep(5);
    } while (now_ms() - t0 < ms);
    return k;
}
static SOCKET w350_join(locnet *A, locnet *C, SOCKET l, const struct sockaddr_in *to, SOCKET *c)
{
    u_long nb = 1;
    SOCKET s;
    *c = locnet_socket(C, AF_INET, SOCK_STREAM, IPPROTO_TCP);
    locnet_connect(C, *c, (const struct sockaddr *)to, sizeof *to);
    s = w350_accept(A, l, 2000);
    if (s != INVALID_SOCKET) ioctlsocket(s, (long)FIONBIO, &nb);
    return s;
}
static void w350_tests(void)
{
    static const char ka[4] = {0x37, 0, 0, 0}; /* Keepalive 0x37 (4 Byte) */
    locnet A, C;
    SOCKET l, c, s, prev = INVALID_SOCKET;
    struct sockaddr_in a, to;
    int k, k2, e, e2, i, ok_leave = 1, ok_new = 1, ok_once = 1, joins = 0;
    char b[64];
    u_long nb = 1;
    if (!locnet_init(&A, "127.0.0.26", 46100, "none", 10, lg) || !locnet_init(&C, "127.0.0.27", 46100, "none", 10, lg)) {
        CHECK(0, "W350: Abgangs-Test startet");
        return;
    }
    l = locnet_socket(&A, AF_INET, SOCK_STREAM, IPPROTO_TCP);
    a = sa(INADDR_ANY, 1000);
    locnet_bind(&A, l, (struct sockaddr *)&a, sizeof a);
    listen(l, 5);
    ioctlsocket(l, (long)FIONBIO, &nb);
    to = sa(A.self_ip, 1000);
    /* 5x beitreten, Rahmen tauschen, geordnet verlassen (closesocket = FIN) */
    for (i = 0; i < 5; i++) {
        long leave0 = A.st.leave;
        s = w350_join(&A, &C, l, &to, &c);
        if (s == INVALID_SOCKET) {
            ok_new = 0;
            closesocket(c);
            break;
        }
        joins++;
        k = locnet_recv(&A, s, b, sizeof b, 0); /* frische Verbindung: nichts da, kein Reset (auch bei gleichem Handle) */
        e = WSAGetLastError();
        if (!(k == SOCKET_ERROR && e == WSAEWOULDBLOCK)) ok_new = 0;
        send(c, ka, 4, 0);
        k = w350_recv(&A, s, b, sizeof b, 1000, &e);
        if (k != 4 || b[0] != 0x37) ok_new = 0;
        locnet_closesocket(&C, c); /* Abgang (Spiel ueber den Hook) */
        k = w350_recv(&A, s, b, sizeof b, 1000, &e);
        if (!(k == SOCKET_ERROR && e == WSAECONNRESET)) ok_leave = 0;
        k = locnet_recv(&A, s, b, sizeof b, 0);
        e = WSAGetLastError();
        if (!(k == SOCKET_ERROR && e == WSAECONNRESET)) ok_leave = 0; /* bleibt getrennt */
        if (A.st.leave != leave0 + 1) ok_once = 0;                    /* ein Abgang je Verbindung */
        if (prev != INVALID_SOCKET) locnet_closesocket(&A, prev);
        prev = s; /* das Spiel schliesst den alten Platz erst spaeter: der neue Strom darf davon nichts erben */
    }
    if (prev != INVALID_SOCKET) locnet_closesocket(&A, prev);
    CHECK(joins == 5 && ok_new, "W350: 5x Wiederbeitritt derselben Adresse - je ein frischer Strom, Rahmen kommen an");
    CHECK(ok_leave, "W350: geordneter Abgang (FIN) -> Spiel sieht WSAECONNRESET, auch beim naechsten recv");
    CHECK(ok_once && A.st.leave == 5, "W350: Abgangszaehler = 5 (einmal je Verbindung)");
    /* harter Abbruch (RST, z. B. beendeter Prozess) */
    s = w350_join(&A, &C, l, &to, &c);
    if (s != INVALID_SOCKET) {
        struct linger lg0 = {1, 0};
        setsockopt(c, SOL_SOCKET, SO_LINGER, (const char *)&lg0, sizeof lg0);
        closesocket(c);
        k = w350_recv(&A, s, b, sizeof b, 1000, &e);
        CHECK(k == SOCKET_ERROR && e == WSAECONNRESET, "W350: harter Abbruch (RST) -> WSAECONNRESET");
        locnet_closesocket(&A, s);
    } else {
        CHECK(0, "W350: harter Abbruch (Verbindung)");
        closesocket(c);
    }
    /* letzter Rahmen, dann Abgang: erst der Rahmen, dann WSAECONNRESET (nichts geht verloren) */
    s = w350_join(&A, &C, l, &to, &c);
    if (s != INVALID_SOCKET) {
        send(c, ka, 4, 0);
        locnet_closesocket(&C, c);
        Sleep(30);
        k = w350_recv(&A, s, b, sizeof b, 1000, &e);
        k2 = w350_recv(&A, s, b + 8, sizeof b - 8, 1000, &e2);
        CHECK(k == 4 && b[0] == 0x37 && k2 == SOCKET_ERROR && e2 == WSAECONNRESET,
              "W350: letzter Rahmen vor dem Abgang kommt noch an, danach WSAECONNRESET");
        locnet_closesocket(&A, s);
    } else {
        CHECK(0, "W350: Rahmen vor Abgang (Verbindung)");
        closesocket(c);
    }
    /* unvollstaendiger Rahmen, dann Abgang: der Kopf (Typ 0x37) ist entschieden, die Teilbytes darf der Filter
     * weitergeben (Strom, das Spiel puffert); danach muss der Abgang kommen, nie recv 0 */
    s = w350_join(&A, &C, l, &to, &c);
    if (s != INVALID_SOCKET) {
        send(c, ka, 2, 0);
        locnet_closesocket(&C, c);
        Sleep(30);
        k = w350_recv(&A, s, b, sizeof b, 3000, &e);
        if (k > 0) k = w350_recv(&A, s, b, sizeof b, 3000, &e);
        CHECK(k == SOCKET_ERROR && e == WSAECONNRESET, "W350: halber Rahmen + Abgang -> danach WSAECONNRESET");
        locnet_closesocket(&A, s);
    } else {
        CHECK(0, "W350: halber Rahmen (Verbindung)");
        closesocket(c);
    }
    /* Schalter loc_net_leave_reset=0 (fin_pass): altes Verhalten recv 0 */
    A.fin_pass = 1;
    s = w350_join(&A, &C, l, &to, &c);
    if (s != INVALID_SOCKET) {
        locnet_closesocket(&C, c);
        k = w350_recv(&A, s, b, sizeof b, 1000, &e);
        CHECK(k == 0, "W350: loc_net_leave_reset=0 reicht recv 0 durch (Vergleichsweg)");
        locnet_closesocket(&A, s);
    } else {
        CHECK(0, "W350: Vergleichsweg (Verbindung)");
        closesocket(c);
    }
    A.fin_pass = 0;
    closesocket(l);
    locnet_shutdown(&C);
    locnet_shutdown(&A);
}

static void vector_tests(const char *path, int required)
{
    char *js = jm_load(path), name[96];
    const char *st, *sb, *in, *v, *arr;
    static uint8_t c[8192], d[8192], out[8192];
    uint8_t a[64], b[64], tag[16], th[32], prk[32];
    int i, n, k, ok;
    if (!js) {
        printf("Hinweis: Testvektoren %s nicht lesbar - Vektor-Pruefungen uebersprungen\n", path);
        /* Standardlauf: die Vektoren liegen unter net/tools (privat, nicht im Community-Export); der Start-KAT
         * (sbe2e_selftest) laeuft trotzdem. Mit --vectors PFAD ist die Datei Pflicht. */
        if (required) CHECK(0, "W334: sbt2_vectors.json lesbar");
        return;
    }
    st = jm_get(js, "standard");
    /* X25519 RFC 7748 §5.2 und §6.1 */
    v = jm_get(st, "x25519_rfc7748_5_2");
    jm_hex(jm_get(v, "scalar"), a, 32);
    jm_hex(jm_get(v, "u"), b, 32);
    k = sbe2e_x25519(a, b, out);
    printf("X25519 RFC 7748 §5.2: %s\n", k == 0 ? (hexeq(jm_get(v, "out"), out, 32) ? "gleich" : "ABWEICHUNG") : "von CNG abgelehnt");
    /* W334 (B): der u-Wert von §5.2 liegt nicht in der Untergruppe primer Ordnung (l*P != O, nachgerechnet);
     * CNG prueft das beim Import und lehnt ab. Echte oeffentliche Schluessel (Skalar * Basispunkt) liegen immer
     * darin - die Abweichung ist nur strenger (fail closed, SECURITY.md §11 Aenderung W334). */
    CHECK((k == 0 && hexeq(jm_get(v, "out"), out, 32)) || k < 0,
          "Vektor: X25519 RFC 7748 §5.2 (gleich oder als Punkt ausserhalb der Untergruppe abgelehnt)");
    v = jm_get(st, "x25519_rfc7748_6_1");
    {
        sbe2e_kp kp;
        uint8_t pub[32];
        jm_hex(jm_get(v, "alice_priv"), a, 32);
        jm_hex(jm_get(v, "bob_pub"), b, 32);
        ok = sbe2e_x25519(a, b, out) == 0 && hexeq(jm_get(v, "shared"), out, 32);
        ok = ok && sbe2e_kp_import(&kp, a, pub) == 0 && hexeq(jm_get(v, "alice_pub"), pub, 32);
        sbe2e_kp_free(&kp);
        jm_hex(jm_get(v, "bob_priv"), a, 32);
        jm_hex(jm_get(v, "alice_pub"), b, 32);
        ok = ok && sbe2e_x25519(a, b, out) == 0 && hexeq(jm_get(v, "shared"), out, 32);
        ok = ok && sbe2e_kp_import(&kp, a, pub) == 0 && hexeq(jm_get(v, "bob_pub"), pub, 32);
        sbe2e_kp_free(&kp);
        CHECK(ok, "Vektor: X25519 RFC 7748 §6.1 (beide Seiten, oeffentliche Schluessel)");
    }
    /* HKDF RFC 5869 TC1 */
    v = jm_get(st, "hkdf_rfc5869_tc1");
    {
        int il = jm_hex(jm_get(v, "ikm"), a, 64), sl = jm_hex(jm_get(v, "salt"), b, 64), nl = jm_hex(jm_get(v, "info"), c, 64);
        int len = (int)jm_num(jm_get(v, "len"));
        sbe2e_hkdf_extract(b, sl, a, il, prk);
        ok = hexeq(jm_get(v, "prk"), prk, 32) && sbe2e_hkdf_expand(prk, c, nl, out, len) == 0 && hexeq(jm_get(v, "okm"), out, len);
        CHECK(ok, "Vektor: HKDF-SHA256 RFC 5869 TC1");
    }
    /* AES-256-GCM TC13/14/16 */
    {
        static const char *tcs[3] = {"aes256gcm_tc13", "aes256gcm_tc14", "aes256gcm_tc16"};
        for (i = 0; i < 3; i++) {
            uint8_t key[32], iv[12], aad[64];
            int pl, al;
            v = jm_get(st, tcs[i]);
            jm_hex(jm_get(v, "key"), key, 32);
            jm_hex(jm_get(v, "iv"), iv, 12);
            pl = jm_hex(jm_get(v, "pt"), c, sizeof c);
            al = jm_hex(jm_get(v, "aad"), aad, sizeof aad);
            ok = sbe2e_gcm(1, key, iv, aad, al, c, pl, out, tag) == 0 && hexeq(jm_get(v, "ct"), out, pl) &&
                 hexeq(jm_get(v, "tag"), tag, 16);
            ok = ok && sbe2e_gcm(0, key, iv, aad, al, out, pl, d, tag) == 0 && memcmp(d, c, (size_t)pl) == 0;
            tag[0] ^= 1;
            ok = ok && sbe2e_gcm(0, key, iv, aad, al, out, pl, d, tag) != 0;
            _snprintf_s(name, sizeof name, _TRUNCATE, "Vektor: AES-256-GCM %s (versiegeln, oeffnen, falscher Tag)", tcs[i] + 10);
            CHECK(ok, name);
        }
    }
    /* SBT2: Ableitung */
    sb = jm_get(js, "sbt2");
    in = jm_get(sb, "inputs");
    {
        sbe2e_side sa_, sb_;
        uint8_t apriv[32], bpriv[32], pn[16], pub[32], shared[32];
        sbe2e_kp ka, kb;
        sbe2e_link la, lb;
        const char *A_ = jm_get(in, "a"), *B_ = jm_get(in, "b");
        memset(&sa_, 0, sizeof sa_);
        memset(&sb_, 0, sizeof sb_);
        sa_.cid = (uint32_t)jm_num(jm_get(A_, "cid"));
        sa_.account = (uint32_t)jm_num(jm_get(A_, "account"));
        jm_str(jm_get(A_, "pilot"), sa_.pilot, sizeof sa_.pilot);
        jm_hex(jm_get(A_, "pub"), sa_.pub, 32);
        jm_hex(jm_get(A_, "priv"), apriv, 32);
        sb_.cid = (uint32_t)jm_num(jm_get(B_, "cid"));
        sb_.account = (uint32_t)jm_num(jm_get(B_, "account"));
        jm_str(jm_get(B_, "pilot"), sb_.pilot, sizeof sb_.pilot);
        jm_hex(jm_get(B_, "pub"), sb_.pub, 32);
        jm_hex(jm_get(B_, "priv"), bpriv, 32);
        jm_hex(jm_get(in, "pair_nonce"), pn, 16);
        ok = sbe2e_transcript(th, (uint32_t)jm_num(jm_get(in, "lobby_id")), pn, &sa_, &sb_) == 0 &&
             hexeq(jm_get(sb, "transcript_hash"), th, 32);
        CHECK(ok, "Vektor: SBT2 Transkript-Hash");
        ok = sbe2e_x25519(apriv, sb_.pub, shared) == 0 && hexeq(jm_get(sb, "shared"), shared, 32) &&
             sbe2e_x25519(bpriv, sa_.pub, out) == 0 && memcmp(out, shared, 32) == 0;
        sbe2e_hkdf_extract(th, 32, shared, 32, prk);
        ok = ok && hexeq(jm_get(sb, "prk"), prk, 32);
        sbe2e_hkdf_expand(prk, "sbt2 k lo>hi", 12, out, 32);
        ok = ok && hexeq(jm_get(sb, "k_lohi"), out, 32);
        sbe2e_hkdf_expand(prk, "sbt2 k hi>lo", 12, out, 32);
        ok = ok && hexeq(jm_get(sb, "k_hilo"), out, 32);
        CHECK(ok, "Vektor: SBT2 Z, PRK, k_lohi, k_hilo");
        ok = sbe2e_kp_import(&ka, apriv, pub) == 0 && memcmp(pub, sa_.pub, 32) == 0 && sbe2e_kp_import(&kb, bpriv, pub) == 0 &&
             sbe2e_derive(&ka, sb_.pub, th, sa_.cid, sb_.cid, &la) == 0 &&
             sbe2e_derive(&kb, sa_.pub, th, sb_.cid, sa_.cid, &lb) == 0 &&
             la.sid == (uint32_t)jm_num(jm_get(sb, "sid")) && lb.sid == la.sid;
        CHECK(ok, "Vektor: SBT2 sid und Ableitung beider Seiten (sbe2e_derive)");
        /* Punkte kleiner Ordnung -> Abbruch */
        arr = jm_get(sb, "low_order_points");
        k = 0;
        for (i = 0; i < jm_len(arr); i++) {
            sbe2e_link lx;
            jm_hex(jm_idx(arr, i), b, 32);
            if (sbe2e_derive(&ka, b, th, sa_.cid, sb_.cid, &lx) != 0 && !lx.valid) k++;
            sbe2e_link_free(&lx);
        }
        CHECK(k == jm_len(arr) && k == 3, "Vektor: 3 Punkte kleiner Ordnung -> Ableitung abgelehnt");
        /* Pakete: gueltige bitgenau versiegeln + oeffnen, ungueltige verwerfen */
        arr = jm_get(sb, "packets");
        for (i = 0; i < jm_len(arr); i++) {
            const char *p = jm_idx(arr, i);
            char kn[16];
            int valid = jm_true(jm_get(p, "valid")), pl;
            int lohi;
            sbe2e_link rx, tx;
            jm_str(jm_get(p, "name"), name, sizeof name);
            jm_str(jm_get(p, "key"), kn, sizeof kn);
            lohi = strcmp(kn, "k_lohi") == 0;
            /* Empfaenger: k_lohi -> Seite hi (a, cid 9); Sender: lo (b) */
            sbe2e_link_from_prk(prk, lohi ? 0 : 1, &rx);
            sbe2e_link_from_prk(prk, lohi ? 1 : 0, &tx);
            pl = jm_hex(jm_get(p, "packet"), c, sizeof c);
            n = sbe2e_open(&rx, c, pl, d, sizeof d);
            if (valid) {
                uint64_t ctr = (uint64_t)jm_num(jm_get(p, "ctr"));
                int tl = jm_hex(jm_get(p, "pt"), a, sizeof a);
                ok = n == tl && memcmp(d, a, (size_t)tl) == 0;
                tx.tx_ctr = ctr - 1;
                k = sbe2e_seal(&tx, a, tl, out, sizeof out);
                ok = ok && k == pl && memcmp(out, c, (size_t)pl) == 0;
            } else {
                ok = n < 0;
            }
            sbe2e_link_free(&rx);
            sbe2e_link_free(&tx);
            {
                char msg[160];
                _snprintf_s(msg, sizeof msg, _TRUNCATE, "Vektor: SBT2-Paket \"%s\" %s", name,
                            valid ? "bitgenau versiegelt und geoeffnet" : "verworfen");
                CHECK(ok, msg);
            }
        }
        sbe2e_link_free(&la);
        sbe2e_link_free(&lb);
        sbe2e_kp_free(&ka);
        sbe2e_kp_free(&kb);
    }
    /* Wiederholungsfenster */
    {
        sbe2e_link w;
        memset(&w, 0, sizeof w);
        arr = jm_get(js, "replay");
        ok = jm_len(arr) > 10;
        for (i = 0; i < jm_len(arr); i++) {
            const char *e = jm_idx(arr, i);
            uint64_t ctr = (uint64_t)jm_num(jm_get(e, "ctr"));
            int acc = sbe2e_win_check(&w, ctr) && jm_true(jm_get(e, "tag_ok"));
            if (acc) sbe2e_win_commit(&w, ctr);
            if (acc != jm_true(jm_get(e, "accept"))) {
                ok = 0;
                printf("  Fenster: Schritt %d ctr %llu falsch\n", i, (unsigned long long)ctr);
            }
        }
        CHECK(ok, "Vektor: Wiederholungsfenster (Duplikat, verspaetet, zu alt, falscher Tag aendert nichts, ctr 0, 2^32+1)");
    }
    /* Gastfilter: Spieltabelle, TCP, UDP */
    {
        const char *f = jm_get(js, "filter"), *gl = jm_get(f, "game_len");
        int typ, bad = 0;
        for (typ = 0; typ < 0x40; typ++) {
            char key[8];
            uint8_t fr[8] = {0};
            const char *e;
            if (typ == 0x34) continue;
            _snprintf_s(key, sizeof key, _TRUNCATE, "0x%02X", typ);
            e = jm_get(gl, key);
            fr[0] = (uint8_t)typ;
            if (lguard_frame_len(fr, 8) != (e ? (int)jm_num(e) : 0)) bad++;
        }
        CHECK(bad == 0, "Vektor: Laengentabelle des Spiels (FUN_0004EFE0) identisch");
        arr = jm_get(f, "tcp");
        for (i = 0; i < jm_len(arr); i++) {
            const char *e = jm_idx(arr, i), *ch = jm_get(e, "chunks"), *ou = jm_get(e, "out");
            int okexp = jm_true(jm_get(e, "ok")), j, fail = 0, viol = 0;
            lguard_tcp *g = (lguard_tcp *)malloc(sizeof *g);
            lguard_tcp_init(g);
            jm_str(jm_get(e, "name"), name, sizeof name);
            for (j = 0; j < jm_len(ch); j++) {
                int cl = jm_hex(jm_idx(ch, j), c, sizeof c), ol = jm_hex(jm_idx(ou, j), d, sizeof d);
                k = lguard_tcp_feed(g, c, cl, out, sizeof out);
                if (k < 0) {
                    viol = 1;
                    break;
                }
                if (k != ol || memcmp(out, d, (size_t)k) != 0) fail = 1;
            }
            free(g);
            {
                char msg[160];
                _snprintf_s(msg, sizeof msg, _TRUNCATE, "Vektor: TCP-Filter \"%s\" %s", name, okexp ? "freigegeben" : "Verstoss");
                CHECK(okexp ? (!viol && !fail) : viol, msg);
            }
            {
                /* W339: derselbe Strom ueber den HLE-Hook-Pfad (lguard_walk_tcp ohne Puffer, wie recv mit MSG_PEEK),
                 * einmal mit grossem Spielpuffer, einmal mit 7-Byte-Puffer (Rahmen werden mittendrin entnommen) */
                static uint8_t all[16384];
                int tot = 0, pass;
                for (j = 0; j < jm_len(ch); j++) tot += jm_hex(jm_idx(ch, j), all + tot, (int)sizeof all - tot);
                for (pass = 0; pass < 2; pass++) {
                    lguard_walk w = {0};
                    int pos = 0, wv = 0, why = 0;
                    char msg[160];
                    for (;;) {
                        int kk = lguard_walk_tcp(&w, all + pos, tot - pos, 0, &why), take;
                        if (kk < 0) {
                            wv = 1;
                            break;
                        }
                        if (kk == 0) break;
                        take = pass ? (kk < 7 ? kk : 7) : kk;
                        lguard_walk_advance(&w, all + pos, tot - pos, take);
                        pos += take;
                    }
                    _snprintf_s(msg, sizeof msg, _TRUNCATE, "Vektor: Hook-Pfad%s \"%s\" %s", pass ? " (7-B-Puffer)" : "", name,
                                okexp ? "freigegeben" : "Verstoss");
                    CHECK(okexp ? (!wv && pos == tot) : wv, msg);
                }
            }
        }
        arr = jm_get(f, "udp");
        for (i = 0; i < jm_len(arr); i++) {
            const char *e = jm_idx(arr, i);
            int dl = jm_hex(jm_get(e, "dgram"), c, sizeof c);
            char msg[160];
            jm_str(jm_get(e, "name"), name, sizeof name);
            _snprintf_s(msg, sizeof msg, _TRUNCATE, "Vektor: UDP-Filter \"%s\"", name);
            CHECK(lguard_udp_ok((uint16_t)jm_num(jm_get(e, "port")), c, dl) == jm_true(jm_get(e, "ok")), msg);
        }
    }
    /* W475 LOC-RB-PREAGREE: Paket SBRP (Format, Pruefsumme) gegen die Python-Referenz */
    arr = jm_get(js, "rbn_pre");
    if (required) CHECK(arr && jm_len(arr) >= 30, "W475: Vektoren rbn_pre vorhanden");
    for (i = 0; arr && i < jm_len(arr); i++) {
        const char *e = jm_idx(arr, i), *dv = jm_get(e, "digest");
        int pl = jm_hex(jm_get(e, "pkt"), c, sizeof c), okexp = jm_true(jm_get(e, "ok"));
        char msg[160];
        jm_str(jm_get(e, "name"), name, sizeof name);
        _snprintf_s(msg, sizeof msg, _TRUNCATE, "Vektor: SBRP \"%s\" %s", name, okexp ? "gueltig" : "verworfen");
        CHECK(rbn_pre_check(c, pl) == okexp, msg);
        if (dv && okexp && pl >= RBN_PRE_HEAD) {
            uint32_t part[RBN_PRE_MAXPART], want = 0, k2;
            int np = c[7];
            char dh[16];
            jm_str(dv, dh, sizeof dh);
            want = (uint32_t)strtoul(dh, NULL, 16);
            for (k2 = 0; k2 < (uint32_t)np && k2 < RBN_PRE_MAXPART; k2++)
                part[k2] = (uint32_t)c[32 + 4 * k2] | ((uint32_t)c[33 + 4 * k2] << 8) | ((uint32_t)c[34 + 4 * k2] << 16) |
                           ((uint32_t)c[35 + 4 * k2] << 24);
            _snprintf_s(msg, sizeof msg, _TRUNCATE, "Vektor: SBRP-Pruefsumme \"%s\"", name);
            CHECK(rbn_pre_digest((uint32_t)c[12] | ((uint32_t)c[13] << 8) | ((uint32_t)c[14] << 16) | ((uint32_t)c[15] << 24),
                                 (uint32_t)c[16] | ((uint32_t)c[17] << 8) | ((uint32_t)c[18] << 16) | ((uint32_t)c[19] << 24),
                                 c[24], c[25], c[26],
                                 (uint32_t)c[28] | ((uint32_t)c[29] << 8) | ((uint32_t)c[30] << 16) | ((uint32_t)c[31] << 24),
                                 part, np) == want, msg);
        }
    }
    free(js);
}


/* W334: TLS-Pruefung gegen einen laufenden (Loopback-)Treffpunkt: Handshake mit Pin(s), bei Erfolg eine Zeile senden.
 * --expect fail: Handshake MUSS scheitern und es darf kein Anwendungsbyte gesendet werden (Mitschnitt am Server). */
static int tls_check(int argc, char **argv, const char *hp)
{
    char host[64], pins[200] = "", expect[8] = "ok", *ctx = NULL, *tok;
    uint8_t pin[2][32];
    int np = 0, i, r = SBTLS_WANT_IO, wrote = 0;
    uint16_t port;
    struct sockaddr_in a;
    SOCKET s;
    sbtls *t = NULL;
    u_long nb = 1;
    double t0;
    locnet ln; /* startet Winsock ueber den Netz-Kern */
    if (!locnet_init(&ln, "127.0.0.26", 47200, "none", 10, lg)) return 2;
    memset(pin, 0, sizeof pin);
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--spki") == 0 && i + 1 < argc) strncpy_s(pins, sizeof pins, argv[i + 1], _TRUNCATE);
        if (strcmp(argv[i], "--expect") == 0 && i + 1 < argc) strncpy_s(expect, sizeof expect, argv[i + 1], _TRUNCATE);
    }
    for (tok = strtok_s(pins, ",", &ctx); tok && np < 2; tok = strtok_s(NULL, ",", &ctx)) {
        if (strcmp(tok, "-") == 0) { np++; continue; } /* leerer Pin */
        if (sbtls_parse_pin(tok, pin[np])) np++;
    }
    {
        const char *cl = strrchr(hp, ':');
        if (!cl || (size_t)(cl - hp) >= sizeof host) return 2;
        memcpy(host, hp, (size_t)(cl - hp));
        host[cl - hp] = 0;
        port = (uint16_t)atoi(cl + 1);
    }
    a = sa(ip4(host), port);
    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (connect(s, (struct sockaddr *)&a, sizeof a) != 0) {
        printf("tls_check: connect fehlgeschlagen\n");
        return 2;
    }
    ioctlsocket(s, (long)FIONBIO, &nb);
    if (sbtls_client_new(&t, s, "relay.steelbound.dev", (const uint8_t(*)[32])pin, np > 0 ? np : 1) == SBTLS_OK) {
        t0 = now_ms();
        while ((r = sbtls_handshake(t)) == SBTLS_WANT_IO && now_ms() - t0 < 10000) Sleep(5);
    } else {
        r = SBTLS_FAIL;
    }
    if (r == SBTLS_OK) {
        char line[] = "{\"t\":\"ping\"}\n", rb[256];
        int k = 0;
        wrote = sbtls_write(t, line, (int)strlen(line)) == SBTLS_OK;
        t0 = now_ms();
        while (now_ms() - t0 < 3000 && (k = sbtls_read(t, rb, sizeof rb - 1)) == SBTLS_WANT_IO) Sleep(5);
        printf("tls_check: verbunden (%s), Antwort %d Byte\n", sbtls_version(t), k > 0 ? k : 0);
    } else {
        /* Versuch, trotzdem zu schreiben: muss abgelehnt werden (kein Anwendungsbyte vor bestandener Pruefung) */
        wrote = t && sbtls_write(t, "x", 1) == SBTLS_OK;
        printf("tls_check: abgelehnt (%s)%s\n", sbtls_error(t), t && sbtls_pin_failed(t) ? " [Pin]" : "");
    }
    sbtls_free(t);
    closesocket(s);
    locnet_shutdown(&ln);
    if (strcmp(expect, "fail") == 0) return (r != SBTLS_OK && !wrote) ? 0 : 1;
    return (r == SBTLS_OK && wrote) ? 0 : 1;
}

/* ---------- W335 NET-AUDIT: Regressionstests der Client-Befunde N1-N3 ---------- */
/* W350: Abgang ueber den eigenen Transport: Strom-FIN (S_FIN) bzw. Ende des Mitspielers (BYE) -> der Stellvertreter
 * schliesst den Strom zum Spiel; das Spiel sieht WSAECONNRESET. Wiederbeitritt dazwischen erbt nichts. */
static void w350_tun_tests(void)
{
    static const char ka[4] = {0x37, 0, 0, 0};
    tun_pair P;
    SOCKET acon, bacc, blis;
    char b[64];
    int k, e, i;
    u_long nb = 1;
    if (!tun_pair_start(&P, 42000, NULL, NULL, 0, 0)) {
        CHECK(0, "W350 Transport: Verbindung A<->B steht");
        tun_pair_stop(&P);
        return;
    }
    if (!tun_tcp(&P, &acon, &bacc, &blis)) {
        CHECK(0, "W350 Transport: TCP ueber den Transport");
        tun_pair_stop(&P);
        return;
    }
    ioctlsocket(bacc, (long)FIONBIO, &nb);
    locnet_send(&P.A, acon, ka, 4, 0);
    k = w350_recv(&P.B, bacc, b, sizeof b, 2000, &e);
    CHECK(k == 4 && b[0] == 0x37, "W350 Transport: Rahmen ueber den Strom");
    locnet_closesocket(&P.A, acon); /* Abgang: S_FIN durch den Transport */
    k = w350_recv(&P.B, bacc, b, sizeof b, 3000, &e);
    CHECK(k == SOCKET_ERROR && e == WSAECONNRESET, "W350 Transport: Strom-FIN des Mitspielers -> WSAECONNRESET beim Host");
    locnet_closesocket(&P.B, bacc);
    /* Wiederbeitritt, dann Ende des Mitspielers (BYE, Verbindung weg) */
    {
        struct sockaddr_in to = sa(P.B.self_ip, 1000);
        acon = locnet_socket(&P.A, AF_INET, SOCK_STREAM, IPPROTO_TCP);
        locnet_connect(&P.A, acon, (struct sockaddr *)&to, sizeof to);
    }
    bacc = INVALID_SOCKET;
    for (i = 0; i < 300 && bacc == INVALID_SOCKET; i++) {
        struct sockaddr_in r;
        int rl = sizeof r;
        bacc = locnet_accept(&P.B, blis, (struct sockaddr *)&r, &rl);
        if (bacc == INVALID_SOCKET) Sleep(10);
    }
    CHECK(bacc != INVALID_SOCKET, "W350 Transport: Wiederbeitritt (neuer Strom angenommen)");
    if (bacc != INVALID_SOCKET) {
        ioctlsocket(bacc, (long)FIONBIO, &nb);
        locnet_send(&P.A, acon, ka, 4, 0);
        k = w350_recv(&P.B, bacc, b, sizeof b, 2000, &e);
        CHECK(k == 4 && b[0] == 0x37, "W350 Transport: neuer Strom erbt keinen Abgang (Rahmen kommt an)");
        locnet_shutdown(&P.A); /* Mitspieler hart weg (Prozessende: kein BYE, kein FIN) */
        k = w350_recv(&P.B, bacc, b, sizeof b, 1000, &e);
        CHECK(k == SOCKET_ERROR && (e == WSAEWOULDBLOCK || e == WSAECONNRESET),
              "W350 Transport: Mitspieler hart weg -> Strom still oder Reset, nie recv 0 (Original: 300 stille Polls entfernen)");
        locnet_closesocket(&P.B, bacc);
    } else {
        locnet_shutdown(&P.A);
    }
    closesocket(blis);
    locnet_shutdown(&P.B);
}

/* W409 LOC-MESH-KICK: Strom-OPEN zu einem Spiel, das TCP 1000 (noch) nicht lauscht. Das Original wiederholt einen
 * abgelehnten connect; beim Mitspieler steht die Verbindung zum Stellvertreter schon - ein sofortiges S_FIN wurde ihm
 * als Verbindungsreset gemeldet (W350) und sein Spiel entfernte uns (Rauswurf beim Beitritt zu 3+ Spielern). Jetzt
 * haelt der Empfaenger das OPEN bis open_hold_ms und verbindet neu. Fall 1: B lauscht erst nach ~800 ms (gehalten,
 * Rahmen kommen in beiden Richtungen an, kein Reset bei A). Fall 2: wie unter Linux sofort abgelehnt (TEST
 * test_refuse_ms, Windows wiederholt das SYN auf Loopback sonst ~2 s), mit Halten. Fall 3: open_hold=0 (altes
 * Verhalten) -> sofort Reset. Fall 4: abgelehnt ueber das Halten hinaus -> Reset nach etwa open_hold_ms (begrenzt). */
/* expect: 1 = gehalten und angenommen, 0 = sofort Reset, 2 = Reset nach dem Halten */
static void w409_case(int base, int hold, int refuse_ms, int listen_after_ms, int expect, const char *what)
{
    static const char ka[4] = {0x37, 0, 0, 0};
    tun_pair P;
    SOCKET acon, bacc = INVALID_SOCKET, blis = INVALID_SOCKET;
    struct sockaddr_in to, l = sa(INADDR_ANY, 1000);
    char b[64], msg[200];
    int k, e = 0;
    u_long nb = 1;
    double t0, t_reset = -1.0;
    s_open_hold = hold;
    s_refuse_b = refuse_ms;
    if (!tun_pair_start(&P, base, NULL, NULL, 0, 0)) {
        _snprintf_s(msg, sizeof msg, _TRUNCATE, "W409 %s: Verbindung A<->B steht", what);
        CHECK(0, msg);
        tun_pair_stop(&P);
        s_open_hold = -1;
        s_refuse_b = 0;
        return;
    }
    if (listen_after_ms == 0) { /* B lauscht schon (nur der TEST-Schalter lehnt ab) */
        blis = locnet_socket(&P.B, AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (locnet_bind(&P.B, blis, (struct sockaddr *)&l, sizeof l) == 0 && listen(blis, 5) == 0)
            ioctlsocket(blis, (long)0x8004667E, &nb);
    }
    to = sa(P.B.self_ip, 1000);
    acon = locnet_socket(&P.A, AF_INET, SOCK_STREAM, IPPROTO_TCP);
    k = locnet_connect(&P.A, acon, (struct sockaddr *)&to, sizeof to);
    ioctlsocket(acon, (long)FIONBIO, &nb);
    t0 = now_ms();
    locnet_send(&P.A, acon, ka, 4, 0); /* Spiel sendet sofort nach dem connect (Rahmen wartet beim Empfaenger) */
    while (now_ms() - t0 < (listen_after_ms > 0 ? listen_after_ms : listen_after_ms == 0 ? refuse_ms + 300 : hold + 1500)) {
        k = locnet_recv(&P.A, acon, b, sizeof b, 0);
        e = k == SOCKET_ERROR ? WSAGetLastError() : 0;
        if (!(k == SOCKET_ERROR && e == WSAEWOULDBLOCK)) {
            t_reset = now_ms() - t0;
            break;
        }
        Sleep(5);
    }
    if (listen_after_ms > 0 && t_reset < 0) {
        blis = locnet_socket(&P.B, AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (locnet_bind(&P.B, blis, (struct sockaddr *)&l, sizeof l) == 0 && listen(blis, 5) == 0)
            ioctlsocket(blis, (long)0x8004667E, &nb);
    }
    if (listen_after_ms >= 0 && t_reset < 0 && blis != INVALID_SOCKET) bacc = w350_accept(&P.B, blis, 2500);
    if (expect == 1) {
        _snprintf_s(msg, sizeof msg, _TRUNCATE, "W409 %s: kein Reset bei A, solange B noch nicht lauscht (OPEN gehalten)", what);
        CHECK(t_reset < 0, msg);
        _snprintf_s(msg, sizeof msg, _TRUNCATE, "W409 %s: B nimmt den gehaltenen Strom an, sobald es lauscht", what);
        CHECK(bacc != INVALID_SOCKET, msg);
        if (bacc != INVALID_SOCKET) {
            ioctlsocket(bacc, (long)FIONBIO, &nb);
            k = w350_recv(&P.B, bacc, b, sizeof b, 2000, &e);
            _snprintf_s(msg, sizeof msg, _TRUNCATE, "W409 %s: Rahmen von vor dem Annehmen kommt bei B an", what);
            CHECK(k == 4 && b[0] == 0x37, msg);
            locnet_send(&P.B, bacc, ka, 4, 0);
            k = w350_recv(&P.A, acon, b, sizeof b, 2000, &e);
            _snprintf_s(msg, sizeof msg, _TRUNCATE, "W409 %s: Antwort B -> A ueber denselben Strom", what);
            CHECK(k == 4 && b[0] == 0x37, msg);
            closesocket(bacc);
        }
    } else if (expect == 0) {
        _snprintf_s(msg, sizeof msg, _TRUNCATE, "W409 %s: ohne Halten sofort Reset bei A (altes Verhalten)", what);
        CHECK(t_reset >= 0 && t_reset < 600 && e == WSAECONNRESET, msg);
    } else {
        _snprintf_s(msg, sizeof msg, _TRUNCATE, "W409 %s: Reset bei A erst nach dem Halten (%.0f ms, Grenze %d ms)", what,
                    t_reset, hold);
        CHECK(t_reset >= hold - 300 && t_reset < hold + 1500 && e == WSAECONNRESET, msg);
    }
    if (blis != INVALID_SOCKET) closesocket(blis);
    locnet_closesocket(&P.A, acon);
    tun_pair_stop(&P);
    s_open_hold = -1;
    s_refuse_b = 0;
}

static void w409_tun_tests(void)
{
    w409_case(43000, 3000, 0, 800, 1, "spaet lauschen");
    w409_case(43300, 3000, 1000, 0, 1, "sofort abgelehnt, gehalten");
    w409_case(43600, 0, 1000, 0, 0, "open_hold=0");
    w409_case(43900, 600, 5000, 0, 2, "abgelehnt ueber das Halten hinaus");
}

static void w335_tests(void)
{
    locnet B;
    fpeer f;
    loctun_cfg c;
    struct sockaddr_in a;
    u_long nb = 1;
    int i;
    /* N1: Testweg (loc_transport_peers, Schluessel oeffentlich bekannt) nie zusammen mit einem Treffpunkt.
     * Vorher: B stach zum Test-Mitspieler durch und nahm dessen HELLO an. */
    memset(&f, 0, sizeof f);
    if (!locnet_init(&B, "127.0.0.45", 44000, "none", 10, lg)) {
        CHECK(0, "W335 N1: Instanz");
        return;
    }
    f.s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    a = sa(htonl(INADDR_LOOPBACK), 44950);
    bind(f.s, (struct sockaddr *)&a, sizeof a);
    ioctlsocket(f.s, (long)FIONBIO, &nb);
    sbe2e_link_test(0, &f.e2e);
    loctun_cfg_default(&c);
    c.port = 44900;
    c.proxy_base = 45000;
    c.allow_public = 1; /* wie loc_net.c mit loc_lobby */
    strcpy_s(c.peers, sizeof c.peers, "127.0.0.1:44950");
    strcpy_s(c.lobby, sizeof c.lobby, "127.0.0.1:44999"); /* kein Treffpunkt erreichbar - genuegt */
    B.tun = loctun_start(&c, &B, NULL, lg);
    CHECK(B.tun != NULL, "W335 N1: Transport startet mit Treffpunkt + loc_transport_peers");
    if (B.tun) {
        fp_pump(&f, 600);
        f.b = sa(htonl(INADDR_LOOPBACK), 44900);
        f.have_b = 1;
        f.vip = ip4("127.0.0.46");
        for (i = 0; i < 20; i++) {
            fp_hello(&f);
            fp_pump(&f, 10);
        }
        CHECK(f.probes_seen == 0 && loctun_links_up(B.tun) == 0,
              "W335 N1: Testweg mit Treffpunkt nicht aktiv (kein Durchstich, HELLO mit Testschluessel verworfen)");
    }
    gt_stop(&B, &f);

    /* N3: UDP ueber recv wie recvfrom - abgeschnittene Datagramme nie zum Spiel, abgelehnte auch bei MSG_PEEK
     * verbraucht. Vorher: WSAEMSGSIZE mit ungeprueftem Anfang im Puffer; ein abgelehntes gepeektes Datagramm
     * blieb vorn in der Warteschlange. */
    if (locnet_init(&B, "127.0.0.47", 46000, "none", 10, lg)) {
        SOCKET g = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP), tx = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        struct sockaddr_in ga = sa(ip4("127.0.0.47"), 46201); /* Spielport 1201 */
        uint8_t big[200], ok1[0x14], bad[0x14], buf[256];
        int r, e, zero = 1;
        bind(g, (struct sockaddr *)&ga, sizeof ga);
        ioctlsocket(g, (long)FIONBIO, &nb);
        memset(big, 0x41, sizeof big);
        big[0] = 1;
        big[1] = 0;
        memset(ok1, 0, sizeof ok1);
        ok1[0] = 1;
        memset(bad, 0, sizeof bad);
        bad[0] = 9; /* Typ 0x09: nicht in der Positivliste */
        sendto(tx, (const char *)big, sizeof big, 0, (struct sockaddr *)&ga, sizeof ga);
        Sleep(20);
        memset(buf, 0, sizeof buf);
        r = locnet_recv(&B, g, (char *)buf, 0x14, 0);
        e = WSAGetLastError();
        for (i = 0; i < 0x14; i++) zero &= buf[i] == 0;
        CHECK(r == SOCKET_ERROR && e == WSAEWOULDBLOCK && zero, "W335 N3: abgeschnittenes UDP-Datagramm (recv) nie zum Spiel");
        sendto(tx, (const char *)bad, sizeof bad, 0, (struct sockaddr *)&ga, sizeof ga);
        sendto(tx, (const char *)ok1, sizeof ok1, 0, (struct sockaddr *)&ga, sizeof ga);
        Sleep(20);
        r = locnet_recv(&B, g, (char *)buf, sizeof buf, MSG_PEEK);
        CHECK(r == 0x14 && buf[0] == 1, "W335 N3: abgelehntes Datagramm bei MSG_PEEK verbraucht, gueltiges folgt");
        r = locnet_recv(&B, g, (char *)buf, sizeof buf, 0);
        CHECK(r == 0x14, "W335 N3: gueltiges Datagramm (recv) unveraendert");
        closesocket(g);
        closesocket(tx);

        /* N2: Zustandstabelle des HLE-TCP-Filters voll -> fail closed (vorher: Zustand eines fremden Sockets
         * verdraengt, Rahmengrenze vergessen). */
        {
            SOCKET ss[100];
            int n = 0, reset_at = -1;
            for (i = 0; i < 100; i++) {
                ss[i] = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                n = i + 1;
                r = locnet_recv(&B, ss[i], (char *)buf, 16, 0);
                if (r == SOCKET_ERROR && WSAGetLastError() == WSAECONNRESET) {
                    reset_at = i;
                    break;
                }
            }
            CHECK(reset_at >= 0 && reset_at <= 64, "W335 N2: volle Filtertabelle trennt den neuen Socket (fail closed)");
            for (i = 0; i < n; i++) locnet_closesocket(&B, ss[i]);
        }
        locnet_shutdown(&B);
    } else {
        CHECK(0, "W335 N3: Instanz");
    }
}

/* ---------- W332 SEC-HARDEN: Treffpunkt-Attrappe (TCP-Zeilen auf Loopback), Funde aus dem Fuzzing ---------- */
static SOCKET fl_accept(SOCKET lis, int ms)
{
    return fp_tcp_pending(lis, ms) ? accept(lis, NULL, NULL) : INVALID_SOCKET;
}

/* Datagramme am Beobachter (UDP-Port des Treffpunkts) zaehlen, die mit "SBN1" beginnen. */
static int fl_udp_count(SOCKET u, int ms)
{
    double t0 = now_ms();
    int n = 0;
    do {
        char b[256];
        int k;
        while ((k = recv(u, b, sizeof b, 0)) > 0) {
            if (k >= 5 && memcmp(b, "SBN1", 4) == 0) n++;
        }
        if (ms > 0) Sleep(5);
    } while (now_ms() - t0 < ms);
    return n;
}

static void fl_send(SOCKET c, const char *s)
{
    send(c, s, (int)strlen(s), 0);
}

/* W332 (nach W334 auf Protokoll v2 umgestellt): Treffpunkt-Attrappe im Klartext. Der Client spricht den
 * Steuerkanal nur noch per TLS mit SPKI-Pin (W334). Gepruefte Sicherheitsgrenze:
 *  a) ohne Pin: gar keine Verbindung zum Treffpunkt (kein ungepruefter Kanal);
 *  b) mit Pin gegen einen Klartext-Dienst: nur ein TLS-ClientHello, nie ein Klartext-hello; ein Klartext-
 *     welcome der Attrappe wird nicht uebernommen (Client trennt, kein UDP-BIND, kein lobby_list).
 * Die alte Klartext-Pruefung "Sitzung erst nach gueltigem welcome" deckt jetzt der welcome-Handler von W334 ab
 * (alle Felder geprueft, bevor client_id/udp_key uebernommen werden). */
static int fl_raw(SOCKET c, char *buf, int cap, int ms)
{
    double t0 = now_ms();
    int o = 0;
    while (o < cap && now_ms() - t0 < ms) {
        int k;
        if (!fp_tcp_pending(c, 50)) {
            if (o > 0) break;
            continue;
        }
        k = recv(c, buf + o, cap - o, 0);
        if (k <= 0) return o > 0 ? o : -2;
        o += k;
    }
    return o;
}

static int fl_has(const char *b, int n, const char *needle)
{
    int i, l = (int)strlen(needle);
    for (i = 0; i + l <= n; i++)
        if (memcmp(b + i, needle, (size_t)l) == 0) return 1;
    return 0;
}

static void tun_lobby_guard_tests(void)
{
    locnet B;
    loctun_cfg c;
    struct sockaddr_in a;
    u_long nb = 1;
    char buf[4096];
    SOCKET lis, uobs, con;
    int k, pass, n0 = s_n, ok0 = s_ok;
    SetEnvironmentVariableA("SB_STEELNET_SPKI", NULL); /* nur die Konfiguration dieses Tests zaehlt */
    lis = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    a = sa(htonl(INADDR_LOOPBACK), 46970);
    bind(lis, (struct sockaddr *)&a, sizeof a);
    listen(lis, 4);
    uobs = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    a = sa(htonl(INADDR_LOOPBACK), 46971);
    bind(uobs, (struct sockaddr *)&a, sizeof a);
    ioctlsocket(uobs, (long)FIONBIO, &nb);
    for (pass = 0; pass < 2; pass++) {
        if (!locnet_init(&B, pass ? "127.0.0.49" : "127.0.0.48", (uint16_t)(46000 + 100 * pass), "none", 10, lg)) {
            CHECK(0, "W332: Instanz B startet");
            break;
        }
        loctun_cfg_default(&c);
        c.port = (uint16_t)(46900 + 10 * pass);
        c.proxy_base = (uint16_t)(46500 + 100 * pass);
        strcpy_s(c.lobby, sizeof c.lobby, "127.0.0.1:46970");
        if (pass) /* irgendein Pin: die Attrappe kann ihn nie erfuellen */
            strcpy_s(c.spki, sizeof c.spki, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
        B.tun = loctun_start(&c, &B, NULL, lg);
        if (!B.tun) {
            CHECK(0, "W332: Transport B mit Treffpunkt-Attrappe startet");
            locnet_shutdown(&B);
            break;
        }
        fl_udp_count(uobs, 0);
        con = fl_accept(lis, pass ? 4000 : 2500);
        if (!pass) {
            CHECK(con == INVALID_SOCKET, "W332: ohne Server-Pin keine Verbindung zum Treffpunkt");
            if (con != INVALID_SOCKET) closesocket(con);
        } else {
            CHECK(con != INVALID_SOCKET, "W332: mit Pin baut der Client die Verbindung auf");
            if (con != INVALID_SOCKET) {
                k = fl_raw(con, buf, sizeof buf, 3000);
                CHECK(k > 5 && (unsigned char)buf[0] == 0x16 && (unsigned char)buf[1] == 0x03,
                      "W332: erstes Paket ist ein TLS-ClientHello");
                CHECK(k > 0 && !fl_has(buf, k, "\"t\":\"hello\"") && !fl_has(buf, k, "\"pilot\""),
                      "W332: kein Klartext-hello an einen Dienst ohne TLS");
                fl_send(con, "{\"t\":\"welcome\",\"v\":2,\"client_id\":42,\"udp_key\":"
                             "\"00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\","
                             "\"udp_port\":46971,\"pilot\":\"Guest-X\"}\n");
                k = fl_raw(con, buf, sizeof buf, 3000);
                CHECK(k == -2 || (k >= 0 && !fl_has(buf, k, "lobby_list")),
                      "W332: Klartext-welcome nicht uebernommen (kein lobby_list, Client trennt)");
                CHECK(fl_udp_count(uobs, 1000) == 0, "W332: Klartext-welcome -> kein UDP-BIND");
                closesocket(con);
            }
        }
        locnet_shutdown(&B);
    }
    closesocket(lis);
    closesocket(uobs);
    printf("W332: Treffpunkt-Attrappe (TLS-Pflicht): %d/%d OK\n", s_ok - ok0, s_n - n0);
}

/* ---------- W373 LOC-PUBLIC-LOBBY: oeffentliche Lobbys ohne Netz (Steuerzeilen eingespeist, LOC_BOOT §48) ---------- */
static uint32_t pub_rand(uint32_t *s)
{
    *s = *s * 1103515245u + 12345u;
    return *s >> 8;
}

/* Liste offener Lobbys an die Instanz: eine Zeile, Eintraege als JSON-Text. fields = Spielfelder (NULL = gueltig). */
#define PUB_F(ma, mb, ms, ca, cb) "\"fa\":0,\"fb\":1,\"ma\":" ma ",\"mb\":" mb ",\"tm\":0,\"ms\":" ms ",\"cf\":0,\"op\":0,\"ca\":" ca ",\"cb\":" cb
static void pub_entry(char *b, size_t cap, int id, const char *pilot, uint32_t vip_host, const char *fields, const char *extra)
{
    size_t l = strlen(b);
    _snprintf_s(b + l, cap - l, _TRUNCATE,
                "%s{\"id\":%d,\"name\":\"%s\",\"game\":\"loc\",\"mode\":\"syslink\",\"map\":\"m02\",\"players\":1,"
                "\"max_players\":10,\"host_pilot\":\"%s\",\"rules\":{\"vip\":%u,%s%s},\"state\":\"open\"}",
                b[l - 1] == '[' ? "" : ",", id, pilot, pilot, vip_host, fields ? fields : PUB_F("5", "5", "2", "1", "0"),
                extra ? extra : "");
}

static int pub_recv_all(locnet *n, SOCKET s, uint8_t nonce[8], uint32_t *vips, int maxv, int *bad)
{
    char buf[256];
    struct sockaddr_in from;
    int k, got = 0;
    *bad = 0;
    while ((k = poll_recvfrom(n, s, buf, sizeof buf, &from)) > 0) {
        if (k != 100 || buf[0] != 2 || memcmp(buf + 4, nonce, 8) != 0 || !lguard_udp_ok(1201, (uint8_t *)buf, k) ||
            locnet_xnaddr_ip((uint8_t *)buf + 0x24) != from.sin_addr.s_addr || ntohs(from.sin_port) != 1201 ||
            memchr(buf + 0x50, 0, 16) == NULL)
            (*bad)++;
        if (got < maxv) vips[got] = from.sin_addr.s_addr;
        got++;
        if (got > 60) break;
    }
    return got;
}

static void pub_search(locnet *n, SOCKET g1201, uint8_t nonce[8], uint32_t *seed)
{
    struct sockaddr_in bc = sa(INADDR_BROADCAST, 1201);
    char buf[0x14];
    int i;
    memset(buf, 0, sizeof buf);
    buf[0] = 1;
    for (i = 0; i < 8; i++) nonce[i] = (uint8_t)pub_rand(seed);
    memcpy(buf + 4, nonce, 8);
    memcpy(buf + 0xC, "Ver 1.0", 8);
    locnet_sendto(n, g1201, buf, 0x14, 0, (struct sockaddr *)&bc, sizeof bc);
}

static void pub_tests(void)
{
    locnet G, H;
    loctun_cfg c;
    char out[8192], line[12000], buf[0x2C4 + 16];
    SOCKET g1201, g1202;
    struct sockaddr_in from;
    uint8_t nonce[8];
    uint32_t seed = 373, vips[64];
    uint32_t vA = ip4("127.88.1.2"), vB = ip4("127.88.3.4"), vG = ip4("127.88.5.6");
    int k, i, bad, n0 = s_n, ok0 = s_ok;
    SetEnvironmentVariableA("SB_STEELNET_SPKI", NULL); /* kein Pin -> der Transport verbindet nie (nur Einspeisung) */
    /* Raumcode geht immer privat, auch mit vis=public */
    if (locnet_init(&H, "127.0.0.58", 47300, "none", 10, lg)) {
        loctun_cfg_default(&c);
        c.port = 47390;
        c.proxy_base = 47700;
        c.allow_public = 1;
        strcpy_s(c.lobby, sizeof c.lobby, "127.0.0.1:44998");
        strcpy_s(c.vis, sizeof c.vis, "public");
        strcpy_s(c.code, sizeof c.code, "7K3M-Q9TX");
        H.tun = loctun_start(&c, &H, NULL, lg);
        CHECK(H.tun && loctun_test_line(H.tun, 9, NULL, out, sizeof out) == 0, "W373: mit Raumcode kein oeffentlicher Modus");
        locnet_shutdown(&H);
    }
    if (!locnet_init(&G, "127.0.0.57", 47100, "none", 10, lg)) {
        CHECK(0, "W373: Instanz startet");
        return;
    }
    loctun_cfg_default(&c);
    c.port = 47190;
    c.proxy_base = 47500;
    c.allow_public = 1;
    strcpy_s(c.lobby, sizeof c.lobby, "127.0.0.1:44998");
    strcpy_s(c.vis, sizeof c.vis, "public");
    strcpy_s(c.pilot, sizeof c.pilot, "Seeker");
    G.tun = loctun_start(&c, &G, NULL, lg);
    CHECK(G.tun && loctun_test_line(G.tun, 7, NULL, out, sizeof out) == 1, "W373: oeffentlicher Modus (vis=public, kein Code)");
    if (!G.tun) {
        locnet_shutdown(&G);
        return;
    }
    CHECK(!strstr(out, "lobby_create") && !strstr(out, "lobby_list"), "W373: ohne Match und ohne Suche keine Lobby, keine Liste");
    g1201 = game_udp(&G, 1201, IPPROTO_UDP);
    g1202 = game_udp(&G, 1202, IPPROTO_UDP);
    /* Liste: zwei gueltige Hosts, ein Name mit Steuerzeichen/zu lang (bereinigt), dazu ungueltige/volle Eintraege */
    strcpy_s(line, sizeof line, "{\"t\":\"lobby_list\",\"lobbies\":[");
    pub_entry(line, sizeof line, 11, "Alpha", ntohl(vA), NULL, NULL);
    pub_entry(line, sizeof line, 12, "Guest-Bravo", ntohl(vB), NULL, ",\"rt_main\":40");
    pub_entry(line, sizeof line, 13, "Zz\\nLongEvilNameXYZ", ntohl(vG), NULL, NULL);       /* Steuerzeichen, zu lang */
    pub_entry(line, sizeof line, 14, "NotLoop", ntohl(ip4("10.1.2.3")), NULL, NULL);       /* vip nicht 127/8 */
    pub_entry(line, sizeof line, 15, "SameVip", ntohl(vA), NULL, NULL);                    /* vip wie Alpha */
    pub_entry(line, sizeof line, 16, "Self", ntohl(G.self_ip), NULL, NULL);                /* eigene Adresse */
    pub_entry(line, sizeof line, 17, "Full", ntohl(ip4("127.88.7.8")), PUB_F("5", "5", "2", "5", "5"), NULL); /* voll */
    pub_entry(line, sizeof line, 18, "Mis18", ntohl(ip4("127.88.9.1")), PUB_F("5", "5", "24", "1", "0"), NULL); /* 0x18 */
    pub_entry(line, sizeof line, 19, "Seats", ntohl(ip4("127.88.9.2")), PUB_F("6", "5", "2", "1", "0"), NULL);  /* 6+5 */
    pub_entry(line, sizeof line, 20, "Frac", ntohl(ip4("127.88.9.3")), PUB_F("5", "5", "2.5", "1", "0"), NULL); /* 2,5 */
    pub_entry(line, sizeof line, 22, "Neg", ntohl(ip4("127.88.9.4")), PUB_F("5", "-1", "2", "1", "0"), NULL);   /* < 0 */
    strcat_s(line, sizeof line, ",{\"id\":21,\"host_pilot\":\"Started\",\"state\":\"started\",\"rules\":{}}]}");
    loctun_test_line(G.tun, 0, line, out, sizeof out);
    CHECK(loctun_pub_games(G.tun) == 3, "W373: Liste -> 3 offene Spiele (ungueltige, volle, gestartete, Adresskonflikte nicht)");
    { /* W460 LOC-LOBBY-WIDGET: Liste fuer das Widget (gleiche Geister, aelteste zuerst, Obergrenze, nur Zahlen/Namen) */
        loctun_pub_entry pe[5];
        int tot = -1, n, okc = 1;
        n = loctun_pub_list(G.tun, pe, 5, &tot);
        CHECK(n == 3 && tot == 3 && pe[0].lid == 11 && pe[1].lid == 12 && pe[2].lid == 13,
              "W460: Widget-Liste 3 offene Lobbys, aelteste (kleinste Id) zuerst");
        CHECK(n == 3 && strcmp(pe[0].pilot, "Alpha") == 0 && strcmp(pe[1].pilot, "Guest-Bravo") == 0 && pe[0].cur == 1 &&
                  pe[0].max == 10 && pe[0].mode == LAS_SIM && pe[0].est < 0,
              "W460: Eintrag Pilot, Spieler 1/10, Modus Sim, Ping unbekannt");
        for (i = 0; n == 3 && pe[2].pilot[i]; i++) okc &= (unsigned char)pe[2].pilot[i] >= 0x20;
        CHECK(n == 3 && okc, "W460: Pilotname ohne Steuerzeichen");
        n = loctun_pub_list(G.tun, pe, 2, &tot);
        CHECK(n == 2 && tot == 3 && pe[0].lid == 11 && pe[1].lid == 12, "W460: Obergrenze cap, total = alle offenen");
    }
    CHECK(locnet_addr_known(&G, vA) && locnet_addr_known(&G, vB) && !locnet_addr_known(&G, ip4("127.88.7.8")),
          "W373: XNetXnAddrToInAddr kennt nur gelistete Geister");
    /* Suche des Spiels -> Host-Info je Spiel (Nonce-Echo, XNADDR = Absender, Gastfilter), Liste wird frisch geholt */
    pub_search(&G, g1201, nonce, &seed);
    loctun_test_line(G.tun, 0, NULL, out, sizeof out);
    CHECK(strstr(out, "\"t\":\"lobby_list\"") && strstr(out, "\"open\":true"), "W373: Suche holt die Liste (open:true)");
    loctun_pub_watch(G.tun, 1); /* W460: Widget sichtbar, Liste eben geholt -> keine weitere Abfrage vor Ablauf von 12 s */
    loctun_test_line(G.tun, 0, NULL, out, sizeof out);
    CHECK(!strstr(out, "lobby_list"), "W460: Widget sichtbar - keine zweite Abfrage binnen 12 s (Rate-Limit)");
    loctun_pub_watch(G.tun, 0);
    k = pub_recv_all(&G, g1201, nonce, vips, 64, &bad);
    CHECK(k == 3 && bad == 0, "W373: Suche -> 3 Host-Infos (Typ 2, 100 B, Nonce-Echo, XNADDR = Absender, Gastfilter ok)");
    {
        int fA = 0, fB = 0, fG = 0;
        for (i = 0; i < k && i < 64; i++) {
            fA |= vips[i] == vA;
            fB |= vips[i] == vB;
            fG |= vips[i] == vG;
        }
        CHECK(fA && fB && fG, "W373: Host-Infos von Alpha, Bravo und dem bereinigten Namen");
    }
    /* Feldinhalt einer Host-Info (erneute Suche, nur Alpha pruefen) */
    pub_search(&G, g1201, nonce, &seed);
    loctun_test_line(G.tun, 0, NULL, out, sizeof out);
    {
        int seen = 0, ok = 1, okb = 0;
        while ((k = poll_recvfrom(&G, g1201, buf, sizeof buf, &from)) > 0) {
            if (from.sin_addr.s_addr == vB) okb = strcmp(buf + 0x50, "~Bravo") == 0; /* Gast: "~Name" */
            if (from.sin_addr.s_addr == vA) {
                seen = 1;
                ok &= buf[0x48] == 0 && buf[0x49] == 1 && buf[0x4A] == 5 && buf[0x4B] == 5 && buf[0x4D] == 2 &&
                     buf[0x60] == 1 && buf[0x61] == 0 && strcmp(buf + 0x50, "Alpha") == 0 && (buf[0x0C] & 0xF0) == 0;
            }
            if (from.sin_addr.s_addr == vG) {
                ok &= strlen(buf + 0x50) <= 15;
                for (i = 0x50; i < 0x60 && buf[i]; i++) ok &= buf[i] >= 0x20 && buf[i] <= 0x7E;
            }
        }
        CHECK(seen && ok && okb, "W373: Host-Info-Felder aus rules (Lager, Sitze, Mission, Zaehler, Name druckbar <= 15, Gast ~Name)");
    }
    /* Beitritt: Typ 3 an den Geist -> lobby_join mit lobby_id; Treffpunkt meldet voll -> Typ 7 (6 B) vom Geist */
    {
        struct sockaddr_in to = sa(vA, 1202);
        memset(buf, 0, sizeof buf);
        buf[0] = 3;
        k = locnet_sendto(&G, g1202, buf, 0x2C4, 0, (struct sockaddr *)&to, sizeof to);
        loctun_test_line(G.tun, 0, NULL, out, sizeof out);
        CHECK(k == 0x2C4 && strstr(out, "{\"t\":\"lobby_join\",\"lobby_id\":11}"), "W373: Beitritt am Geist -> lobby_join lobby_id 11");
        CHECK(poll_recvfrom(&G, g1202, buf, sizeof buf, &from) < 0, "W373: waehrend des Beitritts keine Antwort an das Spiel (Client wartet)");
        loctun_test_line(G.tun, 0, "{\"t\":\"error\",\"code\":\"lobby_full\",\"msg\":\"lobby full\"}", out, sizeof out);
        k = poll_recvfrom(&G, g1202, buf, sizeof buf, &from);
        CHECK(k == 6 && buf[0] == 7 && buf[4] == 0 && from.sin_addr.s_addr == vA, "W373: voll -> Ablehnung Typ 7 (6 B) vom Host-Geist");
        CHECK(loctun_pub_games(G.tun) == 2, "W373: abgelehnte Lobby verschwindet aus der Liste");
    }
    /* Neue Liste ohne Bravo (gestartet) -> verschwindet */
    strcpy_s(line, sizeof line, "{\"t\":\"lobby_list\",\"lobbies\":[");
    pub_entry(line, sizeof line, 13, "Zz", ntohl(vG), NULL, NULL);
    strcat_s(line, sizeof line, "]}");
    loctun_test_line(G.tun, 0, line, out, sizeof out);
    CHECK(loctun_pub_games(G.tun) == 1 && !locnet_addr_known(&G, vB), "W373: gestartete/volle Lobby verschwindet (nicht mehr gelistet)");
    /* Host-Seite: Match angeboten (Spielzustand 0x22) -> oeffentliche Lobby mit rules, Aenderung -> lobby_update,
     * Lockstep -> lobby_start */
    InterlockedExchange(&G.pub_w[0], (LONG)(0u | (1u << 8) | (5u << 16) | (5u << 24)));
    InterlockedExchange(&G.pub_w[1], (LONG)(0u | (2u << 8)));
    InterlockedExchange(&G.pub_w[2], (LONG)(0x22u | (1u << 8) | 0x1000000u));
    for (i = 0; i < 30 && !strstr(out, "lobby_create"); i++) {
        loctun_test_line(G.tun, 0, NULL, out, sizeof out);
        if (!strstr(out, "lobby_create")) Sleep(100);
    }
    {
        char want[64];
        _snprintf_s(want, sizeof want, _TRUNCATE, "\"vip\":%u,", ntohl(G.self_ip));
        CHECK(strstr(out, "\"t\":\"lobby_create\"") && strstr(out, "\"visibility\":\"public\"") && strstr(out, want) &&
              strstr(out, "\"ms\":2") && strstr(out, "\"ma\":5") && strstr(out, "\"ca\":1") && strstr(out, "\"max_players\":10"),
              "W373: Match angeboten -> oeffentliche Lobby (rules: vip, Sitze, Mission, Zaehler)");
        CHECK(!strstr(out, "127.") && !strstr(out, "\"code\""), "W373: Lobby-Anlage ohne Adresse im Klartext, ohne Raumcode");
    }
    loctun_test_line(G.tun, 0, "{\"t\":\"lobby_state\",\"lobby\":{\"id\":99,\"members\":[{\"client_id\":7,\"pilot\":\"Seeker\","
                               "\"controller\":\"pad\",\"host\":true}],\"visibility\":\"public\"}}", out, sizeof out);
    InterlockedExchange(&G.pub_w[2], (LONG)(0x22u | (2u << 8) | 0x1000000u));
    loctun_test_line(G.tun, 0, NULL, out, sizeof out);
    Sleep(1700);
    loctun_test_line(G.tun, 0, NULL, out, sizeof out);
    CHECK(strstr(out, "\"t\":\"lobby_update\"") && strstr(out, "\"ca\":2"), "W373: Spielerzahl geaendert -> lobby_update");
    InterlockedExchange(&G.st.last34_ms, (LONG)GetTickCount());
    loctun_test_line(G.tun, 0, NULL, out, sizeof out);
    CHECK(strstr(out, "\"t\":\"lobby_start\"") != NULL, "W373: Missionsbeginn (0x34) -> lobby_start (nicht mehr gelistet)");
    InterlockedExchange(&G.st.last34_ms, 0);
    InterlockedExchange(&G.pub_w[2], 0);
    /* Fuzz: veraenderte Listen-, Zustands- und Fehlerzeilen - danach nur gueltige Host-Infos, hoechstens 24 Spiele */
    loctun_test_line(G.tun, 0, "{\"t\":\"lobby_closed\",\"lobby_id\":99}", out, sizeof out);
    {
        static const char *const tmpl[] = {
            "{\"t\":\"lobby_list\",\"lobbies\":[{\"id\":31,\"host_pilot\":\"Fz\",\"state\":\"open\",\"rules\":{\"vip\":2136474114,"
            "\"fa\":0,\"fb\":1,\"ma\":5,\"mb\":5,\"tm\":0,\"ms\":2,\"cf\":0,\"op\":0,\"ca\":1,\"cb\":0,\"rt_main\":30}}],\"more\":true}",
            "{\"t\":\"lobby_state\",\"lobby\":{\"id\":5,\"members\":[{\"client_id\":3,\"host\":true},{\"client_id\":7}]}}",
            "{\"t\":\"error\",\"code\":\"lobby_started\"}",
            "{\"t\":\"lobby_list\",\"lobbies\":[]}",
        };
        int it, mx = 0;
        for (it = 0; it < 3000; it++) {
            const char *tp = tmpl[pub_rand(&seed) % 4];
            int l = (int)strlen(tp), m, nm = 1 + (int)(pub_rand(&seed) % 4);
            strcpy_s(line, sizeof line, tp);
            for (m = 0; m < nm; m++) {
                int pos = (int)(pub_rand(&seed) % (unsigned)l);
                switch (pub_rand(&seed) % 4) {
                case 0: line[pos] = (char)(pub_rand(&seed) & 0xFF); if (!line[pos]) line[pos] = '9'; break;
                case 1: line[pos] = "0123456789-.e\"{}[],:"[pub_rand(&seed) % 21]; break;
                case 2: line[pos] = 0; l = pos > 0 ? pos : 1; break;
                default: line[pos] = '9'; break;
                }
            }
            loctun_test_line(G.tun, 0, line, out, sizeof out);
            k = loctun_pub_games(G.tun);
            if (k > mx) mx = k;
        }
        /* viele gueltige Eintraege: Obergrenze */
        strcpy_s(line, sizeof line, "{\"t\":\"lobby_list\",\"lobbies\":[");
        for (i = 0; i < 40; i++) {
            char nmx[16];
            _snprintf_s(nmx, sizeof nmx, _TRUNCATE, "H%d", i);
            pub_entry(line, sizeof line, 100 + i, nmx, ntohl(ip4("127.90.0.10")) + (uint32_t)(i << 8), NULL, NULL);
        }
        strcat_s(line, sizeof line, "]}");
        loctun_test_line(G.tun, 0, line, out, sizeof out);
        k = loctun_pub_games(G.tun);
        if (k > mx) mx = k;
        pub_search(&G, g1201, nonce, &seed);
        loctun_test_line(G.tun, 0, NULL, out, sizeof out);
        k = pub_recv_all(&G, g1201, nonce, vips, 64, &bad);
        CHECK(mx <= 24 && k == loctun_pub_games(G.tun) && k == 24 && bad == 0,
              "W373: Fuzz (3000 Zeilen) ohne Absturz, hoechstens 24 Spiele, alle Host-Infos gueltig");
    }
    closesocket(g1201);
    closesocket(g1202);
    locnet_shutdown(&G);
    printf("W373: oeffentliche Lobbys (eingespeist): %d/%d OK\n", s_ok - ok0, s_n - n0);
}

/* --lobby host:port [--nodirect] [--sim SPEC]: zwei Instanzen ueber einen laufenden steelnet-Treffpunkt. */
/* ---------- W395 LOC-ROUTE: Wegwahl mit mehreren Relays und Wegen eigener Verzoegerung (loc_route_sim) ----------
 * --route host:port --spki X [--rsim SPEC] [--rsim-a SPEC] [--rsim-b SPEC] [--secs N] [--expect-first NAME]
 *         [--expect-final NAME] [--max-switch N]
 * Zwei Clients ueber den (lokalen) Treffpunkt; A schickt N Sekunden lang Lockstep-Rahmen (26 B, alle 33 ms) an B.
 * Ausgabe: Zeitleiste des Weges von A und B, erste Wahl, Endweg, Wechsel/Ausfaelle, groesster Rahmenabstand. */
#define RT_MAXF 4000
typedef struct rt_rx {
    SOCKET r;
    int n, got, to_ms;
    double *arr;
    int *seq;
} rt_rx;

static DWORD WINAPI rt_rx_thread(LPVOID p)
{
    rt_rx *x = (rt_rx *)p;
    x->got = recv_frames(x->r, x->n, x->arr, x->seq, x->to_ms, NULL);
    return 0;
}

static void rt_hud(loctun *t, char *route, int cap, int *nsw, int *nfail, int *decided, int *rt_on, char *alts, int acap)
{
    loctun_peer_hud h;
    int i, o = 0;
    memset(&h, 0, sizeof h);
    route[0] = 0;
    if (alts) alts[0] = 0;
    if (!t || !loctun_peer_hud_get(t, 0, &h)) return;
    strncpy_s(route, (size_t)cap, h.route, _TRUNCATE);
    *nsw = h.nswitch;
    *nfail = h.nfail;
    *decided = h.decided;
    *rt_on = h.rt_on;
    for (i = 0; alts && i < h.nalt && o < acap - 40; i++) {
        o += _snprintf_s(alts + o, (size_t)(acap - o), _TRUNCATE, "%s%s=%.0f%s%s", i ? " " : "", h.alt[i].name,
                         h.alt[i].rtt, h.alt[i].state == 0 ? "(aus)" : h.alt[i].state == 1 ? "(?)" : "",
                         h.alt[i].active ? "*" : "");
    }
}

static int tun_route_test(const char *lobby, int secs, const char *exp_first, const char *exp_final, int max_switch)
{
    tun_pair P;
    SOCKET acon, bacc, blis;
    static rt_rx x;
    static double arr[RT_MAXF];
    static int seq[RT_MAXF];
    char first[24] = "", ra[24] = "", rb[24] = "", la[24] = "", lb2[24] = "", alts[200];
    int nswa = 0, nfa = 0, deca = 0, ona = 0, nswb = 0, nfb = 0, decb = 0, onb = 0, i, n, over = 0, inorder = 1;
    double t0, maxgap = 0, gap_at = 0, decided_at = -1;
    HANDLE th;
    char b[26];
    CHECK(tun_pair_start(&P, 41000, NULL, lobby, 0, 0), "W395: beide in der Lobby, Verbindung steht");
    if (!P.ok) {
        tun_pair_stop(&P);
        printf("loc_net_selftest --route: %d/%d OK\n", s_ok, s_n);
        return 1;
    }
    CHECK(tun_tcp(&P, &acon, &bacc, &blis), "W395: TCP 1000 verbunden");
    n = secs * 30;
    if (n > RT_MAXF) n = RT_MAXF;
    memset(&x, 0, sizeof x);
    x.r = bacc;
    x.n = n;
    x.arr = arr;
    x.seq = seq;
    x.to_ms = secs * 1000 + 15000;
    th = CreateThread(NULL, 0, rt_rx_thread, &x, 0, NULL);
    t0 = now_ms();
    for (i = 0; i < n; i++) {
        frame(b, i);
        send(acon, b, 26, 0);
        if (i % 3 == 0) { /* Zeitleiste alle 100 ms */
            rt_hud(P.A.tun, ra, sizeof ra, &nswa, &nfa, &deca, &ona, alts, sizeof alts);
            rt_hud(P.B.tun, rb, sizeof rb, &nswb, &nfb, &decb, &onb, NULL, 0);
            if (deca && decided_at < 0) {
                decided_at = (now_ms() - t0) / 1000.0;
                strcpy_s(first, sizeof first, ra);
            }
            if (strcmp(ra, la) != 0 || strcmp(rb, lb2) != 0) {
                printf("W395 t=%5.1f s: A %s, B %s (Wahl %s, A misst: %s)\n", (now_ms() - t0) / 1000.0, ra[0] ? ra : "-",
                       rb[0] ? rb : "-", ona ? (deca ? "fertig" : "misst") : "Treffpunkt", alts);
                strcpy_s(la, sizeof la, ra);
                strcpy_s(lb2, sizeof lb2, rb);
            }
            if (i % 300 == 0) printf("W395 t=%5.1f s: A misst %s\n", (now_ms() - t0) / 1000.0, alts);
        }
        while (now_ms() - t0 < (double)(i + 1) * 33.0) Sleep(0);
    }
    WaitForSingleObject(th, 20000);
    CloseHandle(th);
    for (i = 0; i < x.got; i++) {
        double g = i > 0 ? x.arr[i] - x.arr[i - 1] : 0;
        if (x.seq[i] != i) inorder = 0;
        if (g > maxgap) {
            maxgap = g;
            gap_at = (x.arr[i] - t0) / 1000.0;
        }
        if (g > 2.0 * 33 + 30.0) over++;
    }
    rt_hud(P.A.tun, ra, sizeof ra, &nswa, &nfa, &deca, &ona, alts, sizeof alts);
    rt_hud(P.B.tun, rb, sizeof rb, &nswb, &nfb, &decb, &onb, NULL, 0);
    printf("W395ROUTE: erst=%s (t=%.1f s) final=%s/%s wechsel=%d/%d ausfall=%d/%d rahmen=%d/%d geordnet=%d "
           "maxgap=%.0f ms (t=%.1f s) haenger=%d wege: %s\n",
           first[0] ? first : "-", decided_at, ra[0] ? ra : "-", rb[0] ? rb : "-", nswa, nswb, nfa, nfb, x.got, n,
           inorder, maxgap, gap_at, over, alts);
    CHECK(ona && onb, "W395: beide waehlen den Weg selbst (HELLO-Flag)");
    CHECK(x.got == n && inorder, "W395: alle Rahmen geordnet und vollstaendig (kein Desync im Strom)");
    if (exp_first) CHECK(strcmp(first, exp_first) == 0, "W395: erste Wahl wie erwartet");
    if (exp_final) CHECK(strcmp(ra, exp_final) == 0 && strcmp(rb, exp_final) == 0, "W395: Endweg wie erwartet (A und B)");
    if (max_switch >= 0) CHECK(nswa <= max_switch && nswb <= max_switch, "W395: hoechstens erwartete Zahl an Wechseln");
    loctun_log_stats(P.A.tun, secs);
    closesocket(acon); closesocket(bacc); closesocket(blis);
    tun_pair_stop(&P);
    printf("loc_net_selftest --route: %d/%d OK\n", s_ok, s_n);
    return s_ok == s_n ? 0 : 1;
}

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

/* ---------- W395 LOC-ROUTE: neue Felder der route-Zeile (link_id/ticket bei "direct", relays) gegen Unsinn ---------- */
static void route_feed_tests(void)
{
    locnet G;
    loctun_cfg c;
    static char out[16384], line[4096];
    static const char *base =
        "{\"t\":\"route\",\"mode\":\"direct\",\"max_ms\":10,\"relays\":[\"eu\",\"us\"],\"links\":[{\"peer\":77,\"via\":\"direct\","
        "\"ms\":10,\"link_id\":12345,\"ticket\":\"00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\"},"
        "{\"peer\":77,\"via\":\"relay\",\"relay\":\"us\",\"host\":\"127.0.0.1\",\"port\":46982,\"ms\":20,\"link_id\":12345,"
        "\"ticket\":\"00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\"}]}";
    static const char *bad[] = {
        "{\"t\":\"route\",\"links\":[{\"peer\":77,\"via\":\"direct\",\"link_id\":-1,\"ticket\":\"00\"}]}",
        "{\"t\":\"route\",\"links\":[{\"peer\":77,\"via\":\"direct\",\"link_id\":1e300,\"ticket\":\"0011\"}]}",
        "{\"t\":\"route\",\"links\":[{\"peer\":77,\"via\":\"direct\",\"link_id\":\"x\",\"ticket\":12}]}",
        "{\"t\":\"route\",\"links\":[{\"peer\":77,\"via\":\"direct\",\"link_id\":5,\"ticket\":\"0011223344556677889900112233445566778899001122334455667788990\"}]}",
        "{\"t\":\"route\",\"links\":[{\"peer\":77,\"via\":\"direct\",\"link_id\":5,\"ticket\":\"zz112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\"}]}",
        "{\"t\":\"route\",\"links\":[{\"peer\":77,\"via\":\"direct\",\"link_id\":5,\"ticket\":\"00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff00\"}]}",
        "{\"t\":\"route\",\"relays\":5,\"links\":{}}",
        "{\"t\":\"route\",\"relays\":[1,2,{},[],null,true],\"links\":[]}",
        "{\"t\":\"route\",\"relays\":[\"eu\",\"eu\",\"eu\",\"eu\",\"eu\",\"eu\",\"eu\",\"eu\",\"eu\",\"eu\",\"eu\",\"eu\",\"eu\",\"eu\",\"eu\",\"eu\",\"eu\"]}",
        "{\"t\":\"route\",\"relays\":[\"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\"]}",
        "{\"t\":\"route\",\"relays\":[\"\\\"\\\\\"],\"links\":[{\"peer\":4294967296,\"via\":\"direct\"}]}",
        "{\"t\":\"route\",\"relays\":[]}",
        "{\"t\":\"route\",\"relays\":[\"eu\"",
    };
    uint32_t rng = 395;
    int i, k, n0 = s_n, ok0 = s_ok, len;
    SetEnvironmentVariableA("SB_STEELNET_SPKI", NULL); /* kein Pin -> der Transport verbindet nie (nur Einspeisung) */
    if (!locnet_init(&G, "127.0.0.59", 47800, "none", 10, lg)) {
        CHECK(0, "W395: Instanz startet");
        return;
    }
    loctun_cfg_default(&c);
    c.port = 47890;
    c.proxy_base = 47950;
    c.allow_public = 1;
    strcpy_s(c.lobby, sizeof c.lobby, "127.0.0.1:44997");
    G.tun = loctun_start(&c, &G, NULL, lg);
    if (!G.tun) {
        CHECK(0, "W395: Transport mit Einspeisung startet");
        locnet_shutdown(&G);
        return;
    }
    loctun_test_line(G.tun, 5,
                     "{\"t\":\"welcome\",\"v\":2,\"client_id\":5,\"udp_key\":\"00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\","
                     "\"udp_port\":44996,\"pilot\":\"Fz\",\"relays\":[{\"name\":\"eu\",\"host\":\"127.0.0.1\",\"port\":46981},"
                     "{\"name\":\"us\",\"host\":\"127.0.0.1\",\"port\":46982}]}", out, sizeof out);
    loctun_test_line(G.tun, 5,
                     "{\"t\":\"peer_info\",\"peer\":77,\"pair_nonce\":\"00112233445566778899aabbccddeeff\",\"e2e\":{\"suite\":1,"
                     "\"pub\":\"9f1e2d3c4b5a69788796a5b4c3d2e1f00f1e2d3c4b5a69788796a5b4c3d2e1f0\"},\"account\":0,\"lobby_id\":3,"
                     "\"pilot\":\"Bad\",\"local_eps\":[]}", out, sizeof out);
    CHECK(loctun_test_line(G.tun, 5, base, out, sizeof out) >= 0, "W395: gueltige route-Zeile mit link_id/ticket bei direct");
    for (i = 0; i < (int)(sizeof bad / sizeof bad[0]); i++) loctun_test_line(G.tun, 5, bad[i], out, sizeof out);
    len = (int)strlen(base);
    for (k = 1; k < len; k++) { /* jede Kuerzung */
        memcpy(line, base, (size_t)k);
        line[k] = 0;
        loctun_test_line(G.tun, 5, line, out, sizeof out);
    }
    for (i = 0; i < 3000; i++) { /* zufaellige Byte-Aenderungen (fest gesaet) */
        int j, m;
        strcpy_s(line, sizeof line, base);
        for (m = 0; m < 1 + i % 4; m++) {
            rng = rng * 1103515245u + 12345u;
            j = (int)((rng >> 8) % (uint32_t)len);
            rng = rng * 1103515245u + 12345u;
            line[j] = (char)("\"{}[],:0123456789-e.x\\ "[(rng >> 8) % 23]);
        }
        loctun_test_line(G.tun, 5, line, out, sizeof out);
    }
    CHECK(loctun_test_line(G.tun, 5, base, out, sizeof out) >= 0, "W395: 3000 veraenderte und alle gekuerzten route-Zeilen ohne Absturz");
    locnet_shutdown(&G);
    printf("W395 route-Zeilen: %d/%d OK\n", s_ok - ok0, s_n - n0);
}

/* ---------- W395 LOC-ROUTE: Wahl-Logik (loc_route.c) ohne Netz, simulierte Wege ---------- */
static struct { int used, slot; unsigned short seq; double due; } s_rp[4096];
static unsigned s_rp_n;
typedef struct ru_env {
    double rtt[LOCROUTE_MAXP];
    int dead[LOCROUTE_MAXP];
    int loss_every[LOCROUTE_MAXP]; /* jede k-te Probe verloren (0 = keine) */
    int sent[LOCROUTE_MAXP];
} ru_env;

static void ru_reset(void)
{
    memset(s_rp, 0, sizeof s_rp);
    s_rp_n = 0;
}

/* Zeit in 10-ms-Schritten bis until; zaehlt Wechsel (besser/Ausfall) und merkt die Zeit des ersten Ausfallwechsels. */
static void ru_run(locroute *r, ru_env *e, double *now, double until, int ready, int *sw, double *fail_at, int *dual_seen)
{
    int s, why, k;
    while (*now < until) {
        for (s = 0; s < LOCROUTE_MAXP; s++) {
            unsigned short q;
            if (!locroute_probe_due(r, s, *now, &q)) continue;
            e->sent[s]++;
            if (e->dead[s] || (e->loss_every[s] && e->sent[s] % e->loss_every[s] == 0)) continue;
            k = (int)(s_rp_n++ % 4096);
            s_rp[k].used = 1;
            s_rp[k].slot = s;
            s_rp[k].seq = q;
            s_rp[k].due = *now + e->rtt[s];
        }
        for (k = 0; k < 4096; k++) {
            if (s_rp[k].used && s_rp[k].due <= *now) {
                s_rp[k].used = 0;
                if (!e->dead[s_rp[k].slot]) locroute_probe_ack(r, s_rp[k].slot, s_rp[k].seq, *now);
            }
        }
        locroute_step(r, *now, ready, &why);
        if (sw && (why == 2 || why == 3)) (*sw)++;
        if (fail_at && why == 3 && *fail_at < 0) *fail_at = *now;
        if (dual_seen && locroute_dual(r, *now) >= 0) *dual_seen = 1;
        *now += 10.0;
    }
}

static void route_unit_tests(void)
{
    locroute r;
    ru_env e;
    double now, t_cut, fail_at;
    int sw, dual, n0 = s_n, k0 = s_ok;
    unsigned short q;
    /* 1) Messfenster: vorlaeufig direkt, danach das schnellere Relay (60 statt 100 ms) */
    ru_reset();
    memset(&e, 0, sizeof e);
    e.rtt[0] = 100; e.rtt[1] = 60; e.rtt[2] = 150;
    locroute_init(&r);
    now = 1000;
    locroute_set_avail(&r, 0, 1, now);
    locroute_set_avail(&r, 1, 1, now);
    locroute_set_avail(&r, 2, 1, now);
    ru_run(&r, &e, &now, 3400, 1, NULL, NULL, NULL);
    CHECK(!r.decided && r.active == 0, "W395: im Messfenster vorlaeufig direkt");
    ru_run(&r, &e, &now, 4000, 1, NULL, NULL, NULL);
    CHECK(r.decided && r.active == 1 && r.nswitch == 0, "W395: nach dem Messfenster das schnellste Relay (erste Wahl kein Wechsel)");
    CHECK(fabs(r.p[0].srtt - 100) < 15 && fabs(r.p[1].srtt - 60) < 15 && r.p[1].n >= LOCROUTE_MIN_SAMPLES,
          "W395: RTT je Weg gemessen");
    /* 2) Gleichstand: direkt bleibt (97 vs 100 ms) */
    ru_reset();
    memset(&e, 0, sizeof e);
    e.rtt[0] = 100; e.rtt[1] = 97;
    locroute_init(&r);
    now = 1000;
    locroute_set_avail(&r, 0, 1, now);
    locroute_set_avail(&r, 1, 1, now);
    ru_run(&r, &e, &now, 5000, 1, NULL, NULL, NULL);
    CHECK(r.decided && r.active == 0, "W395: Gleichstand -> direkt bevorzugt");
    /* 3) Treffpunkt noch ohne Wege (ready=0): erst nach LOCROUTE_MEASURE_MAX */
    ru_reset();
    locroute_init(&r);
    now = 1000;
    locroute_set_avail(&r, 0, 1, now);
    locroute_set_avail(&r, 1, 1, now);
    ru_run(&r, &e, &now, 1000 + LOCROUTE_MEASURE_MAX - 100, 0, NULL, NULL, NULL);
    CHECK(!r.decided, "W395: ohne Wegliste des Treffpunkts noch keine Wahl");
    ru_run(&r, &e, &now, 1000 + LOCROUTE_MEASURE_MAX + 200, 0, NULL, NULL, NULL);
    CHECK(r.decided, "W395: spaetestens nach dem Messfenster-Maximum gewaehlt");
    /* 4) Hysterese: 20 ms besser -> bleibt; 40 ms besser -> Wechsel nach >= 3 s */
    ru_reset();
    memset(&e, 0, sizeof e);
    e.rtt[0] = 100; e.rtt[1] = 100;
    locroute_init(&r);
    now = 1000;
    locroute_set_avail(&r, 0, 1, now);
    locroute_set_avail(&r, 1, 1, now);
    sw = 0;
    ru_run(&r, &e, &now, 5000, 1, &sw, NULL, NULL);
    CHECK(r.active == 0, "W395: gleich schnell -> direkt");
    e.rtt[1] = 80;
    ru_run(&r, &e, &now, 25000, 1, &sw, NULL, NULL);
    CHECK(r.active == 0 && sw == 0, "W395: 20 ms Vorsprung reicht nicht (Hysterese 25 ms)");
    e.rtt[1] = 60;
    t_cut = now;
    ru_run(&r, &e, &now, t_cut + 2900, 1, &sw, NULL, NULL);
    CHECK(r.active == 0, "W395: kein Wechsel vor 3 s Vorsprung");
    ru_run(&r, &e, &now, t_cut + 6000, 1, &sw, NULL, NULL);
    CHECK(r.active == 1 && sw == 1 && r.nfail == 0, "W395: dauerhaft 40 ms besser -> genau ein Wechsel");
    CHECK(locroute_dual(&r, r.last_switch + 500) == 0 && locroute_dual(&r, r.last_switch + LOCROUTE_DUAL_MS + 10) < 0,
          "W395: nach dem Wechsel 1 s doppelt ueber den alten Weg");
    /* 5) Ausfall des aktiven Weges: Verdacht (doppelt senden), dann Wechsel < 1 s */
    sw = 0;
    fail_at = -1;
    dual = 0;
    ru_run(&r, &e, &now, now + 12000, 1, &sw, NULL, NULL); /* Doppelsenden vorbei, Mindestabstand vorbei */
    r.kick = 0;
    e.dead[1] = 1;
    t_cut = now;
    ru_run(&r, &e, &now, t_cut + 400, 1, &sw, &fail_at, &dual);
    CHECK(dual && r.active == 1 && r.kick, "W395: Verdacht -> doppelt ueber den naechsten Weg, Offenes neu senden");
    ru_run(&r, &e, &now, t_cut + 2000, 1, &sw, &fail_at, NULL);
    CHECK(r.active == 0 && r.nfail == 1 && fail_at > 0 && fail_at - t_cut < 1000.0,
          "W395: Ausfall -> sofort auf den naechsten Weg (< 1 s)");
    printf("W395: Ausfall erkannt nach %.0f ms (Verdacht vorher)\n", fail_at - t_cut);
    /* 6) Wiederkehr: Weg antwortet wieder -> nach Hysterese und Mindestabstand zurueck */
    e.dead[1] = 0;
    ru_run(&r, &e, &now, now + 20000, 1, &sw, NULL, NULL);
    CHECK(r.active == 1 && r.nswitch >= 3, "W395: wiederkehrender schnellerer Weg wird wieder gewaehlt");
    /* 7) Verlust-Strafe: direkt 80 ms mit 10 % Verlust gegen Relay 100 ms ohne Verlust */
    ru_reset();
    memset(&e, 0, sizeof e);
    e.rtt[0] = 80; e.rtt[1] = 100; e.loss_every[0] = 10;
    locroute_init(&r);
    now = 1000;
    locroute_set_avail(&r, 0, 1, now);
    locroute_set_avail(&r, 1, 1, now);
    ru_run(&r, &e, &now, 6000, 1, NULL, NULL, NULL);
    CHECK(r.active == 1 && locroute_loss(&r.p[0], now) > 0.05, "W395: 10 % Verlust wiegt 20 ms Vorsprung auf");
    /* 8) Antworten pruefen: unbekannt, doppelt, nach Neustart des Weges */
    locroute_init(&r);
    locroute_set_avail(&r, 0, 1, 1000);
    CHECK(locroute_probe_due(&r, 0, 1000, &q) && q == 0, "W395: erste Probe faellig");
    CHECK(!locroute_probe_ack(&r, 0, 7, 1050), "W395: Antwort auf nie gesendete Probe ignoriert");
    CHECK(locroute_probe_ack(&r, 0, 0, 1050) && !locroute_probe_ack(&r, 0, 0, 1060), "W395: doppelte Antwort ignoriert");
    CHECK(!locroute_probe_ack(&r, 3, 0, 1050) && !locroute_probe_ack(&r, -1, 0, 1050) &&
              !locroute_probe_ack(&r, LOCROUTE_MAXP, 0, 1050), "W395: fremder/ungueltiger Slot ignoriert");
    CHECK(locroute_probe_due(&r, 0, 1100, &q) && q == 1, "W395: zweite Probe");
    locroute_set_avail(&r, 0, 0, 1110);
    locroute_set_avail(&r, 0, 1, 1120);
    CHECK(!locroute_probe_ack(&r, 0, 1, 1150), "W395: Antwort aus der Zeit vor dem Neustart des Weges ignoriert");
    CHECK(locroute_score(&r.p[0], 1150) < 0, "W395: unter 8 Antworten keine Bewertung");
    CHECK(!locroute_probe_due(&r, 5, 1150, &q) && !locroute_probe_due(&r, LOCROUTE_MAXP, 1150, &q),
          "W395: nicht nutzbarer/ungueltiger Weg ohne Probe");
    printf("W395 loc_route: %d/%d OK\n", s_ok - k0, s_n - n0);
}

/* W357 LOC-PAUSE-NET (sb_pausenet.h): Netzspiel-Erkennung und Regel "Overlays halten den Takt nicht an". */
static void pause_net_tests(void)
{
    sb_pn_live_t s;
    int w, n0 = s_n, k0 = s_ok;
    memset(&s, 0, sizeof s);
    /* Netz aus / erster Zaehlerstand ist nur Bezug */
    CHECK(sb_pn_live_step(&s, 1000, 50, 0, 0, 1, 0) == 0, "W357: Netz aus -> kein Mitspieler");
    CHECK(sb_pn_live_step(&s, 1000, 50, 0, 0, 1, 1) == 0, "W357: erster rx-Stand ist kein Lebenszeichen");
    CHECK(sb_pn_live_step(&s, 1500, 50, 0, 0, 1, 1) == 0, "W357: rx unveraendert -> allein");
    CHECK(sb_pn_live_step(&s, 1800, 52, 0, 0, 0, 1) == 0, "W357: Empfang ohne Spiel-Strom (allein) -> kein Mitspieler");
    CHECK(sb_pn_live_step(&s, 2000, 51, 0, 0, 1, 1) == 1, "W357: Empfang -> verbunden");
    CHECK(sb_pn_live_step(&s, 2000 + SB_PN_HOLD_MS - 1, 51, 0, 0, 1, 1) == 1, "W357: innerhalb der Haltezeit verbunden");
    CHECK(sb_pn_live_step(&s, 2000 + SB_PN_HOLD_MS + 1, 51, 0, 0, 1, 1) == 0, "W357: nach der Haltezeit allein");
    CHECK(sb_pn_live_step(&s, 9000, 51, 1, 0, 1, 1) == 1, "W357: Transport-Verbindung -> verbunden");
    CHECK(sb_pn_live_step(&s, 9000, 51, 0, 8500 | 1, 1, 1) == 1, "W357: eigener Lockstep-Rahmen -> verbunden");
    CHECK(sb_pn_live_step(&s, 0xFFFFFF00u, 51, 0, (long)0xFFFFFE00u, 1, 1) == 1, "W357: GetTickCount-Ueberlauf");
    CHECK(sb_pn_live_step(&s, 100, 51, 0, (long)0xFFFFFF00u, 1, 1) == 1, "W357: Lockstep ueber den Ueberlauf");
    CHECK(sb_pn_live_step(&s, 20000, 51, 0, 20001, 1, 1) == 1, "W357: Lockstep-Stempel |1 (1 ms voraus)");
    CHECK(sb_pn_live_step(&s, 100, 51, 0, 0, 1, 0) == 0 && !s.seen, "W357: Netz aus setzt zurueck");
    /* Schalter */
    CHECK(sb_pn_net(0, 1, 1) == 0, "W357: pause_net=0 -> immer wie bisher");
    CHECK(sb_pn_net(1, 1, 1) == 1, "W357: pause_net=1, LoC + Mitspieler -> Netzspiel");
    CHECK(sb_pn_net(1, 0, 1) == 0, "W357: pause_net=1, SB -> kein Netzspiel");
    CHECK(sb_pn_net(1, 1, 0) == 0, "W357: pause_net=1, LoC allein -> kein Netzspiel");
    CHECK(sb_pn_net(2, 0, 0) == 1, "W357: pause_net=2 (TEST) -> Netzspiel");
    /* Regel ohne Netz: unveraendert (Pause wie bisher, ESC nach menu_pause) */
    CHECK(sb_pn_policy(SB_PN_ESC, 0, 1) == SB_PN_PAUSE, "W357: allein ESC pausiert");
    CHECK(sb_pn_policy(SB_PN_ESC, 0, 0) == SB_PN_RUN, "W357: allein ESC mit menu_pause=0 laeuft");
    for (w = SB_PN_PAUSEKEY; w < SB_PN_N; w++) CHECK(sb_pn_policy(w, 0, 1) == SB_PN_PAUSE, "W357: allein pausiert alles");
    /* Regel im Netzspiel */
    CHECK(sb_pn_policy(SB_PN_ESC, 1, 1) == SB_PN_RUN, "W357: Netz ESC laeuft weiter");
    CHECK(sb_pn_policy(SB_PN_FEEDBACK, 1, 1) == SB_PN_RUN, "W357: Netz F11 laeuft weiter");
    CHECK(sb_pn_policy(SB_PN_WELCOME, 1, 1) == SB_PN_RUN, "W357: Netz Alpha-Hinweis laeuft weiter");
    CHECK(sb_pn_policy(SB_PN_PAUSEKEY, 1, 1) == SB_PN_BLOCK, "W357: Netz Pause-Taste gesperrt");
    CHECK(sb_pn_policy(SB_PN_PHOTO, 1, 1) == SB_PN_BLOCK, "W357: Netz Fotomodus gesperrt");
    CHECK(sb_pn_policy(SB_PN_QSHOT, 1, 1) == SB_PN_ASYNC, "W357: Netz Schnell-Screenshot asynchron");
    for (w = 0; w < SB_PN_N; w++) CHECK(sb_pn_policy(w, 1, 0) != SB_PN_PAUSE, "W357: Netz pausiert nie");
    printf("W357 pause_net: %d/%d OK\n", s_ok - k0, s_n - n0);
}


/* ---------- W420 LOC-CLIPDUMP: Match-Regel (SBRU, loc_rule.c) und Client-Version im Tunnel-HELLO ---------- */
/* Modell der Spielstelle (LoC-XBE, Dekompilat): Schuss FUN_000CD7B0 (Einzelschuss: Pause = WEPDAT+0x2A), Tick
 * FUN_000CA680 (Pause - 1), Nachladen FUN_000CF3C0 (Magazin voll, Magazine - 1, dann FUN_000CF390 = Pause 0; mit Regel
 * entfaellt nur dieser Aufruf). Liefert die Schuesse in n Ticks bei gehaltenem Abzug; Nachladen alle spam Ticks, 0 = erst bei
 * leerem Magazin (normales Spiel), -1 = nie. */
typedef struct { int cd, ammo, mags; } w420_vt;
static int w420_model(int rule, int ticks, int interval, int magsize, int mags, int spam, unsigned *hash)
{
    w420_vt v = {0, magsize, mags};
    int t, shots = 0;
    unsigned h = 2166136261u;
    for (t = 0; t < ticks; t++) {
        if (v.mags > 0 && ((spam > 0 && t % spam == spam - 1) || (spam == 0 && v.ammo == 0))) { /* FUN_000CF3C0 */
            v.ammo = magsize;
            v.mags--;
            if (!(rule & LRU_BIT_CLIPDUMP)) v.cd = 0; /* FUN_000CF390 (Original) */
        }
        if (v.cd <= 0) { /* FUN_000CD7B0 */
            if (v.ammo < 1) v.cd = interval; /* leer: Klick, Pause */
            else {
                v.ammo--;
                v.cd = interval;
                shots++;
            }
        }
        if (v.cd > -1) v.cd--; /* FUN_000CA680 */
        h = (h ^ (unsigned)(v.cd * 131 + v.ammo * 7 + v.mags)) * 16777619u;
    }
    if (hash) *hash = h;
    return shots;
}

static void w420_rule_tests(void)
{
    uint8_t b[LRU_PKT_LEN], c[LRU_PKT_LEN];
    lru_pkt k;
    lru_t r;
    const lru_peer *P;
    int src, i, bad;
    unsigned h0, h1, h2;
    const uint32_t HOST = 0x0A00007Fu, J2 = 0x0B00007Fu, J3 = 0x0C00007Fu;
    /* Paket: bauen, streng pruefen */
    CHECK(lru_build(b, sizeof b, 0x1234u, -1, LRU_BIT_CLIPDUMP, 0, LRU_BIT_CLIPDUMP) == LRU_PKT_LEN, "W420: SBRU bauen");
    CHECK(lru_parse(b, LRU_PKT_LEN, &k) && k.pref == 1 && k.applied == 0 && k.in_mission == 0 && k.pidx == -1 &&
              k.ruleset == LRU_RULESET && k.nonce == 0x1234u,
          "W420: SBRU gueltig, angewandte Regel nur in der Mission");
    CHECK(lru_build(b, sizeof b, 0, 0, 1, 0, 0) == 0, "W420: Kennung 0 wird nicht gebaut");
    lru_build(b, sizeof b, 0x1234u, 3, 1, 1, 1);
    CHECK(lru_parse(b, LRU_PKT_LEN, &k) && k.pidx == 3 && k.applied == 1, "W420: SBRU in der Mission");
    for (i = 0, bad = 0; i < 11; i++) { /* je eine Verfaelschung -> verworfen */
        int len = LRU_PKT_LEN;
        memcpy(c, b, sizeof c);
        switch (i) {
        case 0: len = LRU_PKT_LEN - 1; break;
        case 1: len = LRU_PKT_LEN + 1; break;
        case 2: c[0] = 'X'; break;
        case 3: c[4] = 2; break;               /* Paketversion */
        case 4: c[5] = 0; break;               /* Regelsatz 0 */
        case 5: c[6] = 10; break;              /* Spielerindex */
        case 6: c[7] = 2; break;               /* unbekanntes Flag */
        case 7: memset(c + 8, 0, 4); break;    /* Kennung 0 */
        case 8: c[12] = 2; break;              /* unbekannte Regel im eigenen Regelsatz */
        case 9: c[15] = 1; break;              /* Reserve */
        case 10: c[7] = 0; break;              /* angewandte Regel ausserhalb der Mission */
        }
        bad += !lru_parse(c, len, NULL);
    }
    CHECK(bad == 11, "W420: SBRU-Verfaelschungen (Laenge, Kennung, Version, Index, Flags, Bits, Reserve) verworfen");
    memcpy(c, b, sizeof c);
    c[5] = LRU_RULESET + 1;
    c[12] = 3;
    CHECK(lru_parse(c, LRU_PKT_LEN, &k) && k.pref == 3, "W420: neuerer Regelsatz mit unbekannter Regel lesbar (Ablehnung)");
    /* Empfang, Bindung, Entscheidung */
    lru_init(&r, 0x1111u);
    lru_build(b, sizeof b, 0x1111u, -1, 1, 0, 0);
    CHECK(lru_rx(&r, 1000, HOST, b, LRU_PKT_LEN) == 0 && r.rx_self == 1, "W420: eigenes SBRU ignoriert");
    CHECK(lru_decide(&r, LRU_ROLE_OFFLINE, 0, 1, &src) == 1 && src == LRU_SRC_LOCAL, "W420: offline = eigene Einstellung");
    CHECK(lru_decide(&r, LRU_ROLE_HOST, 0, 0, &src) == 0 && src == LRU_SRC_HOST_SELF, "W420: Host = eigene Einstellung");
    CHECK(lru_decide(&r, LRU_ROLE_JOIN, HOST, 1, &src) == 0 && src == LRU_SRC_HOST_OLD,
          "W420: Host ohne Regel-Feld (aelter) -> Original, auch bei eigener Einstellung an");
    lru_build(b, sizeof b, 0x2222u, -1, LRU_BIT_CLIPDUMP, 0, 0); /* Host: Regel an */
    CHECK(lru_rx(&r, 1000, HOST, b, LRU_PKT_LEN) == 1, "W420: SBRU des Hosts uebernommen");
    CHECK(lru_decide(&r, LRU_ROLE_JOIN, HOST, 0, &src) == 1 && src == LRU_SRC_HOST,
          "W420: Beitretender uebernimmt die Regel des Hosts (eigene Einstellung aus)");
    lru_build(b, sizeof b, 0x2223u, -1, 0, 0, 0);
    CHECK(lru_rx(&r, 1100, HOST, b, LRU_PKT_LEN) == 1 && lru_decide(&r, LRU_ROLE_JOIN, HOST, 1, &src) == 0 &&
              lru_find(&r, HOST)->first_ms == 1100,
          "W420: Neustart des Hosts (neue Kennung) mit Regel aus -> Original");
    lru_build(b, sizeof b, 0x2223u, -1, 0, 0, 0);
    CHECK(lru_rx(&r, 1200, J2, b, LRU_PKT_LEN) == -1, "W420: fremde Adresse mit frischer Kennung des Hosts verworfen");
    memcpy(c, b, sizeof c);
    c[5] = LRU_RULESET + 1;
    c[12] = 3;
    c[8] = 0x77;
    CHECK(lru_rx(&r, 1300, HOST, c, LRU_PKT_LEN) == 1 && lru_decide(&r, LRU_ROLE_JOIN, HOST, 1, &src) == 0 &&
              src == LRU_SRC_HOST_NEWER,
          "W420: Host mit neuerem Regelsatz und unbekannter Regel -> ablehnen (bitte aktualisieren)");
    /* Zulassen beim Host */
    CHECK(lru_admit(&r, J3, 0, 50000, 1000) == 1, "W420: Regel aus (Original) -> aelterer Client zugelassen");
    CHECK(lru_admit(&r, J3, 1, 1000 + LRU_ADMIT_MS - 1, 1000) == 1, "W420: Regel an -> Frist fuer das erste SBRU");
    CHECK(lru_admit(&r, J3, 1, 1000 + LRU_ADMIT_MS, 1000) == 0, "W420: Regel an, kein SBRU nach der Frist -> getrennt");
    lru_build(b, sizeof b, 0x3333u, -1, 0, 0, 0);
    lru_rx(&r, 2000, J3, b, LRU_PKT_LEN);
    CHECK(lru_admit(&r, J3, 1, 60000, 1000) == 1, "W420: Regel an, Mitspieler kennt das Regel-Feld -> zugelassen");
    /* Gegenprobe in der Mission */
    lru_build(b, sizeof b, 0x3333u, 1, 0, 1, 0);
    lru_rx(&r, 3000, J3, b, LRU_PKT_LEN);
    P = lru_find(&r, J3);
    CHECK(lru_mismatch(P, 1, 3100) == 1 && lru_mismatch(P, 0, 3100) == 0, "W420: abweichende angewandte Regel erkannt");
    CHECK(lru_mismatch(P, 1, 3000 + LRU_FRESH_MS + 1) == 0, "W420: veraltetes SBRU zaehlt nicht");
    lru_build(b, sizeof b, 0x3333u, -1, 0, 0, 0);
    lru_rx(&r, 3200, J3, b, LRU_PKT_LEN);
    CHECK(lru_mismatch(lru_find(&r, J3), 1, 3300) == 0, "W420: ausserhalb der Mission keine Gegenprobe");
    for (i = 0, bad = 0; i < 3000; i++) { /* Zufallspakete: nie uebernommen, Tabelle bleibt heil */
        int j, len = 1 + (int)(((unsigned)i * 2654435761u) % 24u);
        uint8_t z[24];
        for (j = 0; j < 24; j++) z[j] = (uint8_t)(((unsigned)i * 31u + (unsigned)j * 17u) * 2654435761u >> 24);
        if (i % 3 == 0 && len >= 4) memcpy(z, "SBRU", 4);
        bad += lru_rx(&r, 4000, J2, z, len) == 1;
    }
    CHECK(bad == 0, "W420: 3000 Zufallspakete verworfen");
    { /* Missionsklammer: Luecken der Hinweise aendern nichts, neue Runde gibt frei (W375-Ablauf) */
        lru_phase ph = {0, 0, 0};
        int ok = 1, k2;
        ok &= !lru_phase_lobby(&ph, 100, 0) && !ph.frozen;           /* Lobby, Host bietet an */
        ok &= lru_phase_mission(&ph, 1, 100, 0) && ph.frozen;         /* Mission 1 beginnt */
        for (k2 = 0; k2 < 20; k2++) {                                  /* Luecken (Rollback, Nachbesprechung) */
            ok &= !lru_phase_lobby(&ph, 100, 0);
            ok &= !lru_phase_mission(&ph, k2 & 1, 100, 0) && ph.frozen;
        }
        ok &= lru_phase_lobby(&ph, 900, 0) && !ph.frozen;              /* Runde 2: Host bietet neu an */
        ok &= lru_phase_mission(&ph, 1, 900, 0) && ph.frozen;
        ok &= !lru_phase_lobby(&ph, 900, 0) && ph.frozen;
        CHECK(ok, "W420: Missionsklammer Host - fest ueber Luecken, frei erst mit neuem Match-Angebot");
        memset(&ph, 0, sizeof ph);
        ok = lru_phase_mission(&ph, 1, 0, 5000) && !lru_phase_lobby(&ph, 0, 5000) && ph.frozen; /* Beitretender */
        ok &= !lru_phase_mission(&ph, 0, 0, 5000) && ph.frozen;        /* Missionsende ohne Lobby-Handlung */
        ok &= lru_phase_lobby(&ph, 0, 9000) && !ph.frozen;             /* neue Beitrittsanfrage */
        ok &= !lru_phase_mission(&ph, 0, 0, 9000) && !ph.frozen;
        CHECK(ok, "W420: Missionsklammer Beitretender - frei erst mit neuer Beitrittsanfrage");
    }
    /* Spielstelle als Modell: Regel aus = Original (Pause 0 nach Nachladen), Regel an = nie schneller als Dauerfeuer */
    {
        int s_off_n = w420_model(0, 100, 60, 3, 3, 0, NULL), s_off = w420_model(0, 100, 60, 3, 3, 4, &h1);
        int s_norm = w420_model(1, 100, 60, 3, 3, 0, NULL), s_on = w420_model(1, 100, 60, 3, 3, 4, &h2);
        int s_on2 = w420_model(1, 100, 60, 3, 3, 4, &h0);
        unsigned h3, h4;
        CHECK(s_off > s_off_n && s_off > s_on, "W420: Modell Regel aus - Clip dumping schneller als Dauerfeuer (Original)");
        CHECK(s_on <= s_norm && s_on == s_on2 && h0 == h2,
              "W420: Modell Regel an - Spam nicht schneller als Dauerfeuer, deterministisch");
        CHECK(w420_model(1, 600, 60, 3, 3, -1, &h3) == w420_model(0, 600, 60, 3, 3, -1, &h4) && h3 == h4,
              "W420: Modell ohne Nachladen - Regel an bitgleich zum Original");
        printf("W420 Modell (100 Ticks, Intervall 60, Magazin 3, 3 Reserve): Dauerfeuer aus %d / an %d, Spam aus %d / an %d\n",
               s_off_n, s_norm, s_off, s_on);
        (void)h1;
        (void)h2;
    }
}

/* Client-Version im Tunnel-HELLO (TEST ver_check: wie mit Treffpunkt, ohne Netz nach aussen) */
static void w420_ver_case(int base, const char *va, const char *vb, int expect_up, const char *what)
{
    tun_pair P;
    char need[16], peer[16], msg[200];
    int up, i, sa_, sb_;
    s_ver_check = 1;
    strcpy_s(s_ver_a, sizeof s_ver_a, va);
    strcpy_s(s_ver_b, sizeof s_ver_b, vb);
    up = tun_pair_start(&P, base, NULL, NULL, 0, 0);
    if (!up) for (i = 0; i < 100; i++) Sleep(10); /* HELLO-Runden abwarten */
    sa_ = P.A.tun ? loctun_version_state(P.A.tun, need, sizeof need, peer, sizeof peer) : -1;
    _snprintf_s(msg, sizeof msg, _TRUNCATE, "W420 %s: Verbindung %s", what, expect_up ? "steht" : "abgelehnt");
    CHECK(up == expect_up, msg);
    if (!expect_up) {
        _snprintf_s(msg, sizeof msg, _TRUNCATE, "W420 %s: A meldet Mitspieler-Version '%s' (neuere %s)", what, peer, need);
        CHECK(sa_ == 2 && strcmp(peer, strcmp(vb, "-") == 0 ? "" : vb) == 0, msg);
        if (strcmp(vb, "-") != 0) {
            sb_ = loctun_version_state(P.B.tun, need, sizeof need, peer, sizeof peer);
            _snprintf_s(msg, sizeof msg, _TRUNCATE, "W420 %s: B lehnt ebenfalls ab (Mitspieler '%s')", what, peer);
            CHECK(sb_ == 2 && strcmp(peer, va) == 0, msg);
        }
    } else {
        CHECK(sa_ == 0, "W420: gleiche Version - keine Ablehnung");
    }
    tun_pair_stop(&P);
    s_ver_check = 0;
    s_ver_a[0] = s_ver_b[0] = 0;
}
/* ---------- W437 LOC-CHAT: Chatkanal SBCH ueber den eigenen Transport (Spielport 1262, nie an das Spiel) ---------- */
static volatile LONG s_w437_n, s_w437_len, s_w437_ok;
static void w437_cb(uint32_t vip, const char *pilot, const uint8_t *d, int len)
{
    lch_pkt p;
    (void)vip;
    (void)pilot; /* ohne Treffpunkt leer; Name kommt sonst aus peer_info */
    InterlockedIncrement(&s_w437_n);
    InterlockedExchange(&s_w437_len, len);
    if (lch_parse(d, len, &p) && p.len == 5 && memcmp(p.text, "Hallo", 5) == 0) InterlockedIncrement(&s_w437_ok);
}
static void w437_wait(volatile LONG *v, LONG want, int ms)
{
    int i;
    for (i = 0; i < ms / 10 && *v < want; i++) Sleep(10);
}
static void w437_chat_tun_tests(void)
{
    tun_pair P;
    uint8_t pkt[LCH_PKT_MAX];
    int up, len, i;
    LONG bad0, n1;
    up = tun_pair_start(&P, 43400, NULL, NULL, 0, 0);
    CHECK(up, "W437: Tunnelpaar fuer den Chatkanal steht");
    if (up) {
        loctun_set_chat(P.B.tun, (uint16_t)LCH_PORT_GP, w437_cb);
        len = lch_build(pkt, sizeof pkt, 0x1234u, 1u, (const uint8_t *)"Hallo", 5);
        CHECK(loctun_dgram(P.A.tun, INADDR_BROADCAST, (uint16_t)LCH_PORT_GP, (uint16_t)LCH_PORT_GP, (const char *)pkt,
                           len) == len, "W437: SBCH an alle verbundenen Mitspieler angenommen");
        w437_wait(&s_w437_n, 1, 2000);
        Sleep(300); /* Kopien der Redundanz abwarten */
        CHECK(s_w437_n == 1 && s_w437_ok == 1 && s_w437_len == len,
              "W437: SBCH kommt beim Mitspieler genau einmal und unveraendert an (Kopien entdoppelt)");
        for (i = 0; i < 20; i++) { /* Flut: Transport-Bremse 4/s, Burst 8 */
            len = lch_build(pkt, sizeof pkt, 0x1234u, (uint32_t)(i + 2), (const uint8_t *)"Hallo", 5);
            loctun_dgram(P.A.tun, INADDR_BROADCAST, (uint16_t)LCH_PORT_GP, (uint16_t)LCH_PORT_GP, (const char *)pkt, len);
        }
        Sleep(800);
        n1 = s_w437_n - 1;
        {
            char m[160];
            _snprintf_s(m, sizeof m, _TRUNCATE, "W437: Flut von 20 Chatpaketen gebremst (%ld zugestellt, erwartet 7..12)", n1);
            CHECK(n1 >= 7 && n1 <= 12, m);
        }
        bad0 = loctun_get_stats(P.A.tun)->bad;
        len = lch_build(pkt, sizeof pkt, 0x99u, 1u, (const uint8_t *)"Hallo", 5);
        loctun_dgram(P.B.tun, INADDR_BROADCAST, (uint16_t)LCH_PORT_GP, (uint16_t)LCH_PORT_GP, (const char *)pkt, len);
        Sleep(400);
        CHECK(loctun_get_stats(P.A.tun)->bad > bad0 && loctun_get_stats(P.A.tun)->guard == 0,
              "W437: ohne Chat-Empfaenger verworfen (Port nicht gebunden, nie an das Spiel, kein Gastfilter-Verstoss)");
        loctun_set_chat(P.B.tun, (uint16_t)LCH_PORT_GP, NULL);
    }
    tun_pair_stop(&P);
}

/* ---------- W440 LOC-LOBBY-MODES: Spielmodus der Lobby (Protokoll im Transport, Durchsetzungsregel) ---------- */
static void w440_las_tests(void)
{
    int src = -1, m, loc, net, srv, lob, tst;
    /* Codec: nur genau "sim"/"arcade" (wie der Dienst) */
    CHECK(las_parse("sim") == LAS_SIM && las_parse("arcade") == LAS_ARCADE, "W440: Modus sim/arcade erkannt");
    CHECK(!las_parse("") && !las_parse("SIM") && !las_parse("Arcade") && !las_parse("arcade ") && !las_parse("free") &&
              !las_parse(NULL) && !las_parse("arcadex"),
          "W440: andere Werte ungueltig (Gross/Klein, Leerzeichen, leer, NULL)");
    /* SB: nie veraendert */
    m = las_effective(0, 1, 1, LAS_SIM, LAS_SIM, &src);
    CHECK(m == LAS_FREE && src == LAS_SRC_SB && las_value(m, 2) == 2 && las_value(m, 1) == 1, "W440: Steel Battalion unveraendert");
    /* LoC ohne Netz: eigene Einstellung */
    m = las_effective(1, 0, 0, 0, 0, &src);
    CHECK(m == LAS_FREE && src == LAS_SRC_OFFLINE && las_value(m, 1) == 1, "W440: LoC ohne Netz = eigene Einstellung");
    /* Lobby sim/arcade */
    m = las_effective(1, 1, 1, LAS_SIM, 0, &src);
    CHECK(m == LAS_SIM && src == LAS_SRC_LOBBY && las_value(m, 2) == 0 && las_value(m, 1) == 0, "W440: Sim-Lobby schaltet Hilfen ab");
    m = las_effective(1, 1, 1, LAS_ARCADE, 0, &src);
    CHECK(m == LAS_ARCADE && src == LAS_SRC_LOBBY && las_value(m, 2) == 2 && las_value(m, 0) == 0 && las_value(m, 1) == 1,
          "W440: Arcade-Lobby laesst die eigene Einstellung (nichts erzwungen)");
    /* ohne Lobby-Angabe: Sim; Testschluessel nur ohne Treffpunkt und nie neben einer Lobby */
    m = las_effective(1, 1, 1, 0, LAS_ARCADE, &src);
    CHECK(m == LAS_SIM && src == LAS_SRC_NOLOBBY, "W440: Treffpunkt ohne Lobby = Sim, Testschluessel wirkungslos");
    m = las_effective(1, 1, 0, 0, 0, &src);
    CHECK(m == LAS_SIM && src == LAS_SRC_NOLOBBY, "W440: System Link ohne Treffpunkt = Sim");
    m = las_effective(1, 1, 0, 0, LAS_ARCADE, &src);
    CHECK(m == LAS_ARCADE && src == LAS_SRC_TEST, "W440: Testschluessel ohne Treffpunkt");
    m = las_effective(1, 1, 1, LAS_SIM, LAS_ARCADE, &src);
    CHECK(m == LAS_SIM && src == LAS_SRC_LOBBY, "W440: Lobby-Modus geht vor dem Testschluessel");
    /* vollstaendige Tabelle: in Sim nie eine Hilfe, sonst immer genau die eigene Einstellung */
    {
        int bad = 0, cfgv;
        for (loc = 0; loc < 2; loc++)
            for (net = 0; net < 2; net++)
                for (srv = 0; srv < 2; srv++)
                    for (lob = -1; lob < 4; lob++)
                        for (tst = -1; tst < 4; tst++)
                            for (cfgv = 0; cfgv < 3; cfgv++) {
                                m = las_effective(loc, net, srv, lob, tst, NULL);
                                if (m != LAS_FREE && m != LAS_SIM && m != LAS_ARCADE) bad++;
                                if (las_value(m, cfgv) != (m == LAS_SIM ? 0 : cfgv)) bad++;
                                if (!loc && m != LAS_FREE) bad++;
                                if (loc && net && m == LAS_FREE) bad++;              /* LoC online nie "frei" */
                                if (loc && net && m == LAS_ARCADE && lob != LAS_ARCADE && !(lob != LAS_SIM && !srv && tst == LAS_ARCADE)) bad++;
                            }
        CHECK(bad == 0, "W440: Modus-Tabelle (600 Faelle: Sim = 0, sonst eigene Einstellung, Arcade nur aus Lobby/Test)");
    }
    CHECK(strcmp(las_name(LAS_SIM), "sim") == 0 && strcmp(las_name(LAS_ARCADE), "arcade") == 0, "W440: Namen fuer Log/Overlay");
}

static void w440_mode_tun_tests(void)
{
    locnet G;
    loctun_cfg c;
    char out[8192];
    int i, pass;
    SetEnvironmentVariableA("SB_STEELNET_SPKI", NULL); /* kein Pin -> nur Einspeisung, keine Verbindung */
    for (pass = 0; pass < 2; pass++) { /* 0 = Standard (sim), 1 = Host waehlt arcade */
        if (!locnet_init(&G, "127.0.0.59", 47800 + pass * 40, "none", 10, lg)) {
            CHECK(0, "W440: Instanz startet");
            return;
        }
        loctun_cfg_default(&c);
        c.port = (uint16_t)(47890 + pass);
        c.proxy_base = (uint16_t)(48100 + pass * 200);
        c.allow_public = 1;
        strcpy_s(c.lobby, sizeof c.lobby, "127.0.0.1:44998");
        strcpy_s(c.vis, sizeof c.vis, "public");
        strcpy_s(c.pilot, sizeof c.pilot, "Host");
        if (pass) strcpy_s(c.play_mode, sizeof c.play_mode, "arcade");
        G.tun = loctun_start(&c, &G, NULL, lg);
        CHECK(G.tun && loctun_test_line(G.tun, 7, NULL, out, sizeof out) == 1, "W440: oeffentlicher Modus");
        if (!G.tun) {
            locnet_shutdown(&G);
            return;
        }
        CHECK(loctun_play_mode(G.tun) == 0, "W440: vor der Lobby kein Modus");
        InterlockedExchange(&G.pub_w[0], (LONG)(0u | (1u << 8) | (5u << 16) | (5u << 24)));
        InterlockedExchange(&G.pub_w[1], (LONG)(0u | (2u << 8)));
        InterlockedExchange(&G.pub_w[2], (LONG)(0x22u | (1u << 8) | 0x1000000u));
        for (i = 0; i < 30 && !strstr(out, "lobby_create"); i++) {
            loctun_test_line(G.tun, 0, NULL, out, sizeof out);
            if (!strstr(out, "lobby_create")) Sleep(100);
        }
        CHECK(strstr(out, "\"t\":\"lobby_create\"") &&
                  strstr(out, pass ? "\"play_mode\":\"arcade\"}" : "\"play_mode\":\"sim\"}") && strstr(out, "\"rules\":{"),
              pass ? "W440: Host arcade -> lobby_create mit play_mode arcade" : "W440: Standard -> lobby_create mit play_mode sim");
        /* lobby_state: Modus der Lobby uebernehmen (der Dienst sendet ihn jedem Mitglied) */
        loctun_test_line(G.tun, 0, "{\"t\":\"lobby_state\",\"lobby\":{\"id\":99,\"play_mode\":\"arcade\",\"members\":[{\"client_id\":7,"
                                   "\"pilot\":\"Host\",\"controller\":\"pad\",\"host\":true}],\"visibility\":\"public\"}}", out, sizeof out);
        CHECK(loctun_play_mode(G.tun) == LAS_ARCADE, "W440: lobby_state play_mode arcade -> arcade");
        loctun_test_line(G.tun, 0, "{\"t\":\"lobby_state\",\"lobby\":{\"id\":99,\"play_mode\":\"sim\",\"members\":[]}}", out, sizeof out);
        CHECK(loctun_play_mode(G.tun) == LAS_SIM, "W440: lobby_state play_mode sim -> sim");
        loctun_test_line(G.tun, 0, "{\"t\":\"lobby_state\",\"lobby\":{\"id\":99,\"play_mode\":\"arcade\",\"members\":[]}}", out, sizeof out);
        loctun_test_line(G.tun, 0, "{\"t\":\"lobby_state\",\"lobby\":{\"id\":99,\"members\":[]}}", out, sizeof out);
        CHECK(loctun_play_mode(G.tun) == LAS_SIM, "W440: Feld fehlt (aelterer Dienst) -> sim");
        {
            static const char *const bad[] = { "\"ARCADE\"", "\"arcade \"", "1", "true", "null", "[\"arcade\"]", "{\"m\":1}",
                                               "\"\"", "\"arcadearcadearcadearcade\"" };
            int k, ok = 1;
            char line[400];
            for (k = 0; k < (int)(sizeof bad / sizeof bad[0]); k++) {
                loctun_test_line(G.tun, 0, "{\"t\":\"lobby_state\",\"lobby\":{\"id\":99,\"play_mode\":\"arcade\",\"members\":[]}}", out,
                                 sizeof out);
                _snprintf_s(line, sizeof line, _TRUNCATE, "{\"t\":\"lobby_state\",\"lobby\":{\"id\":99,\"play_mode\":%s,\"members\":[]}}",
                            bad[k]);
                loctun_test_line(G.tun, 0, line, out, sizeof out);
                if (loctun_play_mode(G.tun) != LAS_SIM) ok = 0;
            }
            CHECK(ok, "W440: ungueltiger play_mode (9 Formen) -> sim (fail closed)");
        }
        loctun_test_line(G.tun, 0, "{\"t\":\"lobby_state\",\"lobby\":{\"id\":99,\"play_mode\":\"arcade\",\"members\":[]}}", out, sizeof out);
        loctun_test_line(G.tun, 0, "{\"t\":\"lobby_closed\",\"lobby_id\":99}", out, sizeof out);
        CHECK(loctun_play_mode(G.tun) == 0, "W440: Lobby geschlossen -> kein Modus (wirksam Sim)");
        InterlockedExchange(&G.pub_w[2], 0);
        locnet_shutdown(&G);
    }
}

/* ---------- W443 LOC-LOBBY-LIST: Modus/Ping je Zeile der Suchliste (Zuordnung ueber die XNKID) ---------- */
static void w443_entry(uint8_t e[LLV_ENTRY], int lid)
{
    uint8_t d[100];
    memset(d, 0, sizeof d); /* Aufbau wie pub_hostinfo (loc_tun.c), das Spiel kopiert ab +0xC 0x58 B */
    d[0] = 2;
    d[0x0C] = 0;
    memcpy(d + 0x0D, "SBP", 3);
    d[0x10] = (uint8_t)lid; d[0x11] = (uint8_t)(lid >> 8); d[0x12] = (uint8_t)(lid >> 16); d[0x13] = (uint8_t)(lid >> 24);
    memcpy(d + 0x14, "SBPUBLIC", 8);
    memcpy(d + 0x1C, d + 0x10, 4);
    memcpy(e, d + 0x0C, LLV_ENTRY);
}

static void w443_llv_tests(void)
{
    static uint8_t list[LLV_MAX_ENTRIES * LLV_ENTRY];
    uint8_t e[LLV_ENTRY];
    llv_view v;
    char pt[16];
    int i, y0, y1, ok = 1;
    w443_entry(e, 31);
    /* W460 LOC-LOBBY-WIDGET: Seitenerkennung (Szene 8, Text-Id der ersten Zeile, Zahl der Zeilen) */
    CHECK(lw_screen_from(8, LW_TID_MAIN, 7) == 1 && lw_screen_from(8, LW_TID_MATCH, 2) == 2,
          "W460: MAIN MENU (0x1B4, 7 Zeilen) und MatchMaking (0x3A0, 2 Zeilen) erkannt");
    CHECK(lw_screen_from(8, 0x1B5, 25) == 0 && lw_screen_from(8, 0x432, 1) == 0 && lw_screen_from(5, LW_TID_MAIN, 7) == 0 &&
              lw_screen_from(8, LW_TID_MAIN, 2) == 0 && lw_screen_from(8, LW_TID_MATCH, 7) == 0 && lw_screen_from(12, LW_TID_MATCH, 2) == 0 &&
              lw_screen_from(-1, -1, -1) == 0,
          "W460: Missionswahl, Suchergebnis/Cancel-Dialog, andere Szenen und Mischwerte -> kein Widget");
    CHECK(llv_entry_lid(e) == 31, "W443: Listeneintrag eines Geists -> Lobby-Id aus der XNKID");
    w443_entry(e, 2147483647);
    CHECK(llv_entry_lid(e) == 2147483647, "W443: groesste Lobby-Id");
    {
        static const int off[] = { 0, 1, 3, 8, 15, 0x10, 0x14 }; /* Typ, Kennung, Schluessel, Id-Kopie, Rest */
        for (i = 0; i < (int)(sizeof off / sizeof off[0]); i++) {
            w443_entry(e, 31);
            e[off[i]] ^= 0x41;
            if (llv_entry_lid(e) != 0) ok = 0;
        }
        w443_entry(e, 0);
        if (llv_entry_lid(e) != 0) ok = 0;
        w443_entry(e, -5);
        if (llv_entry_lid(e) != 0) ok = 0;
        memset(e, 0, sizeof e); /* echter Host: zufaellige XNKID/XNKEY */
        for (i = 0; i < 0x18; i++) e[i] = (uint8_t)(i * 37 + 11);
        if (llv_entry_lid(e) != 0) ok = 0;
        if (llv_entry_lid(NULL) != 0) ok = 0;
        CHECK(ok, "W443: fremde/verfaelschte Eintraege (echter Host, Id 0/negativ, Kennung/Schluessel/Id-Kopie) -> keine Lobby");
    }
    for (i = 0; i < LLV_MAX_ENTRIES; i++) w443_entry(list + (size_t)i * LLV_ENTRY, 100 + i);
    memset(list + 4 * LLV_ENTRY, 0x5A, LLV_ENTRY); /* Eintrag 4 = echter Host */
    CHECK(llv_view_from(2, 1, 10, 3, 0, 1, list, &v) && v.nvis == 3 && v.lid[0] == 100 && v.lid[2] == 102 && v.sel_lid == 101,
          "W443: Ansicht 3 Eintraege, gewaehlt 1");
    CHECK(llv_view_from(2, 1, 10, 13, 2, 4, list, &v) && v.nvis == 10 && v.lid[0] == 102 && v.lid[2] == 0 &&
              v.lid[9] == 111 && v.sel_lid == 0,
          "W443: 13 Eintraege ab Zeile 2 -> 10 sichtbar, echter Host ohne Lobby");
    CHECK(llv_view_from(2, 1, 1, 49, 44, 48, list, &v) && v.nvis == 5 && v.lid[4] == 148 && v.sel_lid == 148,
          "W443: 49 Eintraege (Obergrenze), letzte Seite");
    ok = !llv_view_from(0, 1, 10, 3, 0, 0, list, &v) && !llv_view_from(2, 3, 10, 3, 0, 0, list, &v) &&
         !llv_view_from(2, 1, 0, 3, 0, 0, list, &v) && !llv_view_from(2, 1, 10, 0, 0, 0, list, &v) &&
         !llv_view_from(2, 1, 10, 50, 0, 0, list, &v) && !llv_view_from(2, 1, 10, 3, 3, 0, list, &v) &&
         !llv_view_from(2, 1, 10, 3, -1, 0, list, &v) && !llv_view_from(2, 1, 10, 3, 0, 3, list, &v) &&
         !llv_view_from(2, 1, 10, 3, 0, -1, list, &v) && !llv_view_from(2, 1, 10, 3, 0, 0, NULL, &v) && !v.active;
    CHECK(ok, "W443: keine Anzeige ausserhalb der System-Link-Suchliste bzw. bei unplausiblen Werten (10 Faelle)");
    CHECK(strcmp(llv_mode_label(1), "SIM") == 0 && strcmp(llv_mode_label(2), "ARCADE") == 0 && !llv_mode_label(0) &&
              !llv_mode_label(3),
          "W443: Marken SIM/ARCADE, sonst keine");
    llv_ping_text(120, pt, sizeof pt);
    ok = strcmp(pt, "~120 ms") == 0;
    llv_ping_text(-1, pt, sizeof pt);
    ok &= strcmp(pt, "--") == 0;
    llv_ping_text(5000, pt, sizeof pt);
    ok &= strcmp(pt, "~999 ms") == 0;
    llv_row_y(0, &y0, &y1);
    ok &= y0 == 0x57 && y1 == 0x6E;
    llv_row_y(9, &y0, &y1);
    ok &= y0 == 0x57 + 9 * 28 && y1 == 0x6E + 9 * 28;
    CHECK(ok, "W443: Ping-Text und Zeilenlage (FUN_000FADD0: 0x57 + 28 k)");
}

static void w443_tun_entry(char *b, size_t cap, int id, const char *pilot, uint32_t vip_host, const char *pm)
{
    size_t l = strlen(b);
    _snprintf_s(b + l, cap - l, _TRUNCATE,
                "%s{\"id\":%d,\"name\":\"%s\",\"game\":\"loc\",\"mode\":\"syslink\",\"map\":\"m02\",\"players\":1,"
                "\"max_players\":10,\"host_pilot\":\"%s\",\"rules\":{\"vip\":%u,%s},\"state\":\"open\"%s%s}",
                b[l - 1] == '[' ? "" : ",", id, pilot, pilot, vip_host, PUB_F("5", "5", "2", "1", "0"),
                pm ? ",\"play_mode\":" : "", pm ? pm : "");
}

static void w443_tun_tests(void)
{
    locnet G;
    loctun_cfg c;
    char out[8192], line[8000], buf[256];
    static uint8_t list[LLV_MAX_ENTRIES * LLV_ENTRY];
    struct sockaddr_in from;
    SOCKET g1201;
    uint8_t nonce[8];
    uint32_t seed = 443, vip[4] = { ip4("127.88.21.1"), ip4("127.88.21.2"), ip4("127.88.21.3"), ip4("127.88.21.4") };
    int lids[6] = { 31, 32, 33, 34, 99, 0 }, md[6], es[6], k, n = 0, ok;
    llv_view v;
    SetEnvironmentVariableA("SB_STEELNET_SPKI", NULL); /* kein Pin -> nur Einspeisung, keine Verbindung */
    if (!locnet_init(&G, "127.0.0.61", 48600, "none", 10, lg)) {
        CHECK(0, "W443: Instanz startet");
        return;
    }
    loctun_cfg_default(&c);
    c.port = 48690;
    c.proxy_base = 48800;
    c.allow_public = 1;
    strcpy_s(c.lobby, sizeof c.lobby, "127.0.0.1:44998");
    strcpy_s(c.vis, sizeof c.vis, "public");
    strcpy_s(c.pilot, sizeof c.pilot, "Seeker");
    G.tun = loctun_start(&c, &G, NULL, lg);
    CHECK(G.tun && loctun_test_line(G.tun, 7, NULL, out, sizeof out) == 1, "W443: oeffentlicher Modus");
    if (!G.tun) {
        locnet_shutdown(&G);
        return;
    }
    g1201 = game_udp(&G, 1201, IPPROTO_UDP);
    strcpy_s(line, sizeof line, "{\"t\":\"lobby_list\",\"lobbies\":[");
    w443_tun_entry(line, sizeof line, 31, "Arc", ntohl(vip[0]), "\"arcade\"");
    w443_tun_entry(line, sizeof line, 32, "Sim", ntohl(vip[1]), "\"sim\"");
    w443_tun_entry(line, sizeof line, 33, "Old", ntohl(vip[2]), NULL);       /* aelterer Dienst ohne Feld */
    w443_tun_entry(line, sizeof line, 34, "Odd", ntohl(vip[3]), "\"ARCADE\""); /* ungueltig */
    strcat_s(line, sizeof line, "]}");
    loctun_test_line(G.tun, 0, line, out, sizeof out);
    CHECK(loctun_pub_games(G.tun) == 4, "W443: Liste -> 4 offene Spiele");
    k = loctun_pub_lookup(G.tun, lids, 6, md, es);
    CHECK(k == 4 && md[0] == LAS_ARCADE && md[1] == LAS_SIM && md[2] == LAS_SIM && md[3] == LAS_SIM && md[4] == 0 &&
              md[5] == 0 && es[0] == -1 && es[4] == -1,
          "W443: Modus je Lobby (arcade, sim, Feld fehlt = sim, ungueltig = sim; unbekannt/0 = keine; ohne Relay kein Ping)");
    CHECK(loctun_pub_lookup(G.tun, NULL, 3, md, es) == 0 && loctun_pub_lookup(G.tun, lids, 0, md, es) == 0,
          "W443: Abfrage ohne Ids");
    /* Kette wie im Spiel: Suche -> Host-Infos -> Liste (ab +0xC, je 0x58 B) -> Lobby-Id je Zeile -> Modus */
    pub_search(&G, g1201, nonce, &seed);
    loctun_test_line(G.tun, 0, NULL, out, sizeof out);
    ok = 1;
    while ((k = poll_recvfrom(&G, g1201, buf, sizeof buf, &from)) > 0 && n < LLV_MAX_ENTRIES) {
        if (k != 100 || buf[0] != 2) { ok = 0; continue; }
        memcpy(list + (size_t)n * LLV_ENTRY, buf + 0x0C, LLV_ENTRY);
        n++;
    }
    CHECK(ok && n == 4, "W443: Suche -> 4 Host-Infos fuer die Spielliste");
    ok = n == 4 && llv_view_from(2, 1, 10, n, 0, n - 1, list, &v) && v.nvis == 4;
    if (ok) {
        int seen = 0;
        ok = loctun_pub_lookup(G.tun, v.lid, v.nvis, md, es) == 4;
        for (k = 0; k < v.nvis && ok; k++) {
            int want = v.lid[k] == 31 ? LAS_ARCADE : (v.lid[k] >= 32 && v.lid[k] <= 34) ? LAS_SIM : -1;
            ok = md[k] == want;
            if (ok) seen |= 1 << (v.lid[k] - 31);
        }
        ok = ok && seen == 15;
    }
    CHECK(ok, "W443: jede Zeile der Spielliste findet ihre Lobby und deren Modus (Zuordnung ueber die XNKID)");
    /* Lobby verschwindet (gestartet) -> Zeile der eingefrorenen Liste ohne Modus */
    strcpy_s(line, sizeof line, "{\"t\":\"lobby_list\",\"lobbies\":[");
    w443_tun_entry(line, sizeof line, 32, "Sim", ntohl(vip[1]), "\"sim\"");
    strcat_s(line, sizeof line, "]}");
    loctun_test_line(G.tun, 0, line, out, sizeof out);
    k = loctun_pub_lookup(G.tun, lids, 6, md, es);
    CHECK(k == 1 && md[0] == 0 && md[1] == LAS_SIM, "W443: verschwundene Lobby -> keine Marke mehr");
    closesocket(g1201);
    locnet_shutdown(&G);
}

/* ---------- W446 STEELNET-CHAT: steelnet-Chat ueber den Steuerkanal (Einspeisung, ohne Netz) ---------- */
static volatile LONG s_w446_n[3];
static char s_w446_from[24], s_w446_txt[600];
static int s_w446_len;
static void w446_cb(int kind, const char *from, const uint8_t *text, int len)
{
    if (kind < 0 || kind > 2) return;
    InterlockedIncrement(&s_w446_n[kind]);
    strcpy_s(s_w446_from, sizeof s_w446_from, from ? from : "");
    s_w446_len = len < (int)sizeof s_w446_txt - 1 ? len : (int)sizeof s_w446_txt - 1;
    memcpy(s_w446_txt, text, (size_t)s_w446_len);
    s_w446_txt[s_w446_len] = 0;
}

static void w446_snchat_tun_tests(void)
{
    static const char welcome[] = "{\"t\":\"welcome\",\"v\":2,\"client_id\":7,\"udp_key\":\"000000000000000000000000000000000000"
                                  "0000000000000000000000000000\",\"udp_port\":47000,\"pilot\":\"Host\",\"guest\":true,"
                                  "\"relays\":[],\"chat\":true}";
    locnet G;
    loctun_cfg c;
    char out[8192];
    int pass;
    SetEnvironmentVariableA("SB_STEELNET_SPKI", NULL);
    for (pass = 0; pass < 2; pass++) { /* 0 = Ablauf, 1 = TEST join_delay_ms (Lobby erst spaeter) */
        if (!locnet_init(&G, "127.0.0.61", 47900 + pass * 40, "none", 10, lg)) {
            CHECK(0, "W446: Instanz startet");
            return;
        }
        loctun_cfg_default(&c);
        c.port = (uint16_t)(47990 + pass);
        c.proxy_base = (uint16_t)(48500 + pass * 200);
        c.allow_public = 1;
        strcpy_s(c.lobby, sizeof c.lobby, "127.0.0.1:44997");
        strcpy_s(c.pilot, sizeof c.pilot, "Host");
        if (pass) c.join_delay_ms = 600000;
        G.tun = loctun_start(&c, &G, NULL, lg);
        if (!G.tun) {
            CHECK(0, "W446: Transport startet");
            locnet_shutdown(&G);
            return;
        }
        loctun_set_snchat(G.tun, w446_cb, pass ? 0 : 1); /* pass 1: Schalter erst nach dem welcome */
        loctun_test_line(G.tun, 7, NULL, out, sizeof out);
        CHECK(loctun_snchat_state(G.tun) == 0 && !strstr(out, "chat_sub"), "W446: vor dem welcome kein Abonnement");
        loctun_test_line(G.tun, 0, welcome, out, sizeof out);
        if (pass) {
            CHECK(!strstr(out, "lobby_create") && !strstr(out, "chat_sub"),
                  "W446: ohne Schalter kein chat_sub (auch kein Abbestellen); TEST join_delay: Lobby noch nicht");
            loctun_set_snchat(G.tun, w446_cb, 1);
            loctun_test_line(G.tun, 0, NULL, out, sizeof out);
            CHECK(!strstr(out, "lobby_create") && strstr(out, "{\"t\":\"chat_sub\",\"on\":true,\"hist\":true}"),
                  "W446 TEST join_delay: steelnet ja (mit Verlauf), Lobby noch nicht");
            locnet_shutdown(&G);
            break;
        }
        CHECK(loctun_snchat_state(G.tun) == 1 && strstr(out, "{\"t\":\"chat_sub\",\"on\":true,\"hist\":true}") &&
                  strstr(out, "lobby_create"), "W446: welcome chat:true -> Abonnement mit Verlauf (Lobby wie bisher)");
        CHECK(loctun_snchat_send(G.tun, "{\"t\":\"chat\",\"text\":\"x\"}", 24) == 0, "W446: Senden erst abonniert");
        loctun_test_line(G.tun, 0, "{\"t\":\"chat_sub\",\"on\":true}", out, sizeof out);
        CHECK(loctun_snchat_state(G.tun) == 2, "W446: chat_sub on -> abonniert");
        loctun_test_line(G.tun, 0, "{\"t\":\"chat\",\"id\":5,\"from\":\"Alpha\",\"text\":\"hi \\u00e4 \\ud83d\\ude00\",\"ts\":1}", out,
                         sizeof out);
        CHECK(s_w446_n[0] == 1 && strcmp(s_w446_from, "Alpha") == 0 && strcmp(s_w446_txt, "hi \xC3\xA4 \xF0\x9F\x98\x80") == 0,
              "W446: Nachricht an den Chat (Name, UTF-8 aus \\u und Surrogatpaar)");
        loctun_test_line(G.tun, 0, "{\"t\":\"chat\",\"id\":3,\"from\":\"Bravo\",\"text\":\"alt\",\"ts\":1,\"hist\":1}", out, sizeof out);
        CHECK(s_w446_n[1] == 1 && strcmp(s_w446_from, "Bravo") == 0, "W446: Verlaufszeile als Verlauf");
        {
            static const char *const bad[] = {
                "{\"t\":\"chat\",\"id\":6,\"text\":\"x\"}",                                      /* ohne Absender */
                "{\"t\":\"chat\",\"id\":6,\"from\":\"\",\"text\":\"x\"}",                         /* leerer Absender */
                "{\"t\":\"chat\",\"id\":6,\"from\":\"ABCDEFGHIJKLMNOPQRS\",\"text\":\"x\"}",      /* zu lang */
                "{\"t\":\"chat\",\"id\":6,\"from\":7,\"text\":\"x\"}",                            /* keine Zeichenkette */
                "{\"t\":\"chat\",\"id\":6,\"from\":\"A\",\"text\":5}",                            /* Text keine Zeichenkette */
                "{\"t\":\"chat\",\"id\":6,\"from\":\"A\",\"text\":\"a\\qb\"}",                    /* kaputtes Escape */
                "{\"t\":\"chat\",\"id\":6,\"from\":\"A\",\"text\":\"\"}",                         /* leer */
                "{\"t\":\"chat\",\"id\":6,\"from\":\"A\\u0001\",\"text\":\"x\"}" };               /* Escape im Namen */
            int k;
            LONG before = s_w446_n[0];
            for (k = 0; k < (int)(sizeof bad / sizeof bad[0]); k++) loctun_test_line(G.tun, 0, bad[k], out, sizeof out);
            CHECK(s_w446_n[0] == before, "W446: 8 ungueltige chat-Zeilen verworfen");
        }
        loctun_test_line(G.tun, 0, "{\"t\":\"chat_status\",\"code\":\"rate\"}", out, sizeof out);
        CHECK(s_w446_n[2] == 1 && strcmp(s_w446_txt, "rate") == 0, "W446: Statuscode an den Chat");
        loctun_test_line(G.tun, 0, "{\"t\":\"chat_status\",\"code\":\"ok\",\"id\":9}", out, sizeof out);
        loctun_test_line(G.tun, 0, "{\"t\":\"chat_status\",\"code\":\"R4te!\"}", out, sizeof out);
        CHECK(s_w446_n[2] == 1, "W446: ok und unbekannte Form ohne Meldung");
        CHECK(loctun_snchat_send(G.tun, "{\"t\":\"chat\",\"text\":\"hallo\"}", 28) == 1, "W446: Zeile eingereiht");
        loctun_test_line(G.tun, 0, NULL, out, sizeof out);
        CHECK(strstr(out, "{\"t\":\"chat\",\"text\":\"hallo\"}\n") != NULL, "W446: eingereihte Zeile ueber den Steuerkanal gesendet");
        loctun_set_snchat(G.tun, w446_cb, 0);
        loctun_test_line(G.tun, 0, NULL, out, sizeof out);
        CHECK(strstr(out, "{\"t\":\"chat_sub\",\"on\":false,\"hist\":false}") != NULL, "W446: Schalter aus -> abbestellt");
        loctun_test_line(G.tun, 0, "{\"t\":\"chat_sub\",\"on\":false}", out, sizeof out);
        loctun_test_line(G.tun, 0, "{\"t\":\"chat\",\"id\":7,\"from\":\"Alpha\",\"text\":\"spaet\",\"ts\":1}", out, sizeof out);
        CHECK(loctun_snchat_state(G.tun) == 1 && s_w446_n[0] == 1, "W446: nicht abonniert -> nichts angenommen");
        loctun_set_snchat(G.tun, w446_cb, 1);
        loctun_test_line(G.tun, 0, NULL, out, sizeof out);
        CHECK(strstr(out, "{\"t\":\"chat_sub\",\"on\":true,\"hist\":false}") != NULL,
              "W446: erneut abonniert ohne zweiten Verlauf (keine Doppel)");
        loctun_test_line(G.tun, 0, "{\"t\":\"welcome\",\"v\":2,\"client_id\":7,\"udp_key\":\"0000000000000000000000000000000000000000"
                                   "000000000000000000000000\",\"udp_port\":47000,\"pilot\":\"Host\",\"relays\":[]}", out, sizeof out);
        CHECK(loctun_snchat_state(G.tun) == 0 && !strstr(out, "chat_sub"), "W446: aelterer Dienst ohne chat-Feld -> kein Tab");
        loctun_set_snchat(G.tun, NULL, 0);
        locnet_shutdown(&G);
    }
}

static void w420_ver_tests(void)
{
    w420_ver_case(43000, "0.3.2", "0.3.2", 1, "gleiche Version");
    w420_ver_case(43100, "0.3.2", "0.3.1", 0, "aeltere Version");
    w420_ver_case(43200, "0.3.2", "0.3.10", 0, "neuere Version");
    w420_ver_case(43300, "0.3.2", "-", 0, "aelterer Client ohne Versionsfeld");
}

/* ---------- W454 LOC-LAN-MODE: LAN ohne steelnet (reiner Kern + Transport ueber Loopback) ---------- */
static void w454_hex(const uint8_t *p, int n, char *out)
{
    static const char H[] = "0123456789abcdef";
    int i;
    for (i = 0; i < n; i++) {
        out[2 * i] = H[p[i] >> 4];
        out[2 * i + 1] = H[p[i] & 15];
    }
    out[2 * n] = 0;
}

static void w454_beacon_fixed(llan_beacon *o)
{
    int i;
    memset(o, 0, sizeof *o);
    o->tport = 47555;
    o->cid = 0x12345678u;
    strcpy_s(o->room, sizeof o->room, "LAN");
    o->room_id = llan_room_id("LAN");
    for (i = 0; i < 16; i++) o->nonce[i] = (uint8_t)i;
    for (i = 0; i < 32; i++) o->pub[i] = (uint8_t)(64 + i);
    strcpy_s(o->pilot, sizeof o->pilot, "Alice");
    strcpy_s(o->ver, sizeof o->ver, "0.4.1");
    o->mode = 1;
}

/* Vektoren unabhaengig mit Python hashlib berechnet (pbkdf2_hmac, sha256, hmac) - BERICHT_M2 W454 */
static void w454_lan_unit_tests(void)
{
    char room[32], pil[32], hx[80];
    uint8_t psk[32], pn[16], th[32], key[32], b[LLAN_BEACON_LEN + 4], d[32];
    uint8_t nlo[16], nhi[16];
    llan_beacon o, q;
    int i, ok;
    CHECK(llan_room_norm("", room, sizeof room) && strcmp(room, "LAN") == 0, "W454: Raumname leer = LAN");
    CHECK(llan_room_norm("  Party  ", room, sizeof room) && strcmp(room, "Party") == 0, "W454: Raumname ohne Randleerzeichen");
    CHECK(!llan_room_norm("12345678901234567", room, sizeof room) && !llan_room_norm("R\xc3\xa4um", room, sizeof room) &&
              !llan_room_norm("a\tb", room, sizeof room),
          "W454: Raumname zu lang / nicht ASCII / Steuerzeichen abgelehnt");
    llan_pilot_norm("J\xc3\xbcrgen", pil, sizeof pil);
    CHECK(strcmp(pil, "J?rgen") == 0, "W454: Pilotname UTF-8 -> ein '?' je Zeichen");
    llan_pilot_norm("", pil, sizeof pil);
    CHECK(strcmp(pil, "Pilot") == 0, "W454: Pilotname leer = Pilot");
    llan_pilot_norm("ABCDEFGHIJKLMNOPQRS", pil, sizeof pil);
    CHECK(strlen(pil) == 16, "W454: Pilotname auf 16 Zeichen gekuerzt");
    CHECK(llan_room_id("LAN") == 0x1efa18b4u && llan_room_id("Party") == 0x62a20445u, "W454: Raumkennung (Vektor)");
    llan_psk("LAN", "geheim", 0, psk);
    w454_hex(psk, 32, hx);
    CHECK(strcmp(hx, "898c8a6e172f1e9be38139ec23a9bae2f7b7828e1478bf646d2536f50b71d656") == 0,
          "W454: Raumschluessel PBKDF2-HMAC-SHA256 20000 (Vektor, Passwort)");
    llan_psk("LAN", "", 0, psk);
    w454_hex(psk, 32, hx);
    CHECK(strcmp(hx, "8d6221e9cdd995a97569321fbad3906b5191320053d21a15117b35a449e6e553") == 0,
          "W454: Raumschluessel ohne Passwort (Vektor, oeffentlich bekannt)");
    llan_psk("Party", "pw", 3, psk);
    w454_hex(psk, 32, hx);
    CHECK(strcmp(hx, "3640831340daae6135df699567c110de0eee30a6fc3e432dc0d40f8535a4549d") == 0,
          "W454: Raumschluessel Raum als Salz (Vektor, 3 Runden)");
    for (i = 0; i < 16; i++) {
        nlo[i] = (uint8_t)i;
        nhi[i] = (uint8_t)(16 + i);
    }
    llan_pair_nonce(nlo, nhi, pn);
    w454_hex(pn, 16, hx);
    CHECK(strcmp(hx, "8e36c42208e422f1f2863381533ebbe1") == 0, "W454: pair_nonce aus beiden Zufallswerten (Vektor)");
    memset(th, 0x11, 32);
    memset(key, 0x22, 32);
    llan_th_mix(th, key);
    w454_hex(th, 32, hx);
    CHECK(strcmp(hx, "978bde21bf424836c6df1fdab109215951cdfbbb4ddc0552094a75796b8fa76c") == 0,
          "W454: Transkript an den Raumschluessel gebunden (Vektor)");
    /* Paket: Bau gegen Vektor, Rundweg */
    w454_beacon_fixed(&o);
    CHECK(llan_beacon_build(b, sizeof b, &o) == LLAN_BEACON_LEN, "W454: SBL1 gebaut (116 Byte)");
    sb_sha256(b, LLAN_BEACON_LEN, d);
    w454_hex(d, 32, hx);
    CHECK(strcmp(hx, "4b162cbd5de409301b3e62f7dfe426fcac20a2d27f6faae025fc524fb6222dc8") == 0, "W454: SBL1 Byte fuer Byte (Vektor)");
    memset(&q, 0, sizeof q);
    CHECK(llan_beacon_parse(b, LLAN_BEACON_LEN, &q) == 1 && q.cid == o.cid && q.tport == o.tport &&
              strcmp(q.room, "LAN") == 0 && strcmp(q.pilot, "Alice") == 0 && strcmp(q.ver, "0.4.1") == 0 && q.mode == 1 &&
              memcmp(q.pub, o.pub, 32) == 0 && memcmp(q.nonce, o.nonce, 16) == 0,
          "W454: SBL1 Rundweg (alle Felder)");
    /* Bau verweigert ungueltige Felder */
    q = o; q.cid = 0; ok = llan_beacon_build(b, sizeof b, &q) == 0;
    q = o; q.tport = 0; ok &= llan_beacon_build(b, sizeof b, &q) == 0;
    q = o; q.room_id ^= 1; ok &= llan_beacon_build(b, sizeof b, &q) == 0;
    q = o; strcpy_s(q.ver, sizeof q.ver, "0.4/1"); ok &= llan_beacon_build(b, sizeof b, &q) == 0;
    q = o; q.mode = 3; ok &= llan_beacon_build(b, sizeof b, &q) == 0;
    CHECK(ok, "W454: SBL1-Bau verweigert ungueltige Felder");
    /* Pruefung: jede Verletzung verworfen */
    {
        static const struct { int off; uint8_t val; const char *why; } kMut[] = {
            { 0, 'X', "Magic" }, { 4, 2, "Protokoll" }, { 5, 1, "Reserve 5" }, { 115, 1, "Reserve 115" },
            { 64, 0, "Raum leer" }, { 64, 17, "Raum zu lang" }, { 65, 0x7F, "Raum Steuerzeichen" }, { 70, 'x', "Raum-Fuellung" },
            { 81, 0, "Pilot leer" }, { 81, 17, "Pilot zu lang" }, { 82, 0x19, "Pilot Steuerzeichen" }, { 90, 'x', "Pilot-Fuellung" },
            { 98, 0, "Version leer" }, { 99, '/', "Version Zeichen" }, { 110, 'x', "Version-Fuellung" }, { 113, '1', "Version 16 Zeichen" },
            { 114, 0, "Modus 0" }, { 114, 3, "Modus 3" }, { 12, 0, "Raumkennung passt nicht" },
        };
        uint8_t m[LLAN_BEACON_LEN + 4];
        int k, all = 1;
        llan_beacon_build(b, sizeof b, &o);
        for (k = 0; k < (int)(sizeof kMut / sizeof kMut[0]); k++) {
            memcpy(m, b, LLAN_BEACON_LEN);
            m[kMut[k].off] = kMut[k].val;
            if (llan_beacon_parse(m, LLAN_BEACON_LEN, &q)) {
                all = 0;
                printf("  W454: Mutation %s angenommen\n", kMut[k].why);
            }
        }
        memcpy(m, b, LLAN_BEACON_LEN);
        m[6] = m[7] = 0; /* tport 0 */
        all &= !llan_beacon_parse(m, LLAN_BEACON_LEN, &q);
        memcpy(m, b, LLAN_BEACON_LEN);
        memset(m + 8, 0, 4); /* cid 0 */
        all &= !llan_beacon_parse(m, LLAN_BEACON_LEN, &q);
        memcpy(m, b, LLAN_BEACON_LEN);
        memset(m + 32, 0, 32); /* Schluessel 0 */
        all &= !llan_beacon_parse(m, LLAN_BEACON_LEN, &q);
        all &= !llan_beacon_parse(b, LLAN_BEACON_LEN - 1, &q) && !llan_beacon_parse(b, LLAN_BEACON_LEN + 1, &q) &&
               !llan_beacon_parse(b, 0, &q) && !llan_beacon_parse(NULL, LLAN_BEACON_LEN, &q);
        CHECK(all, "W454: SBL1-Pruefung verwirft jede Feldverletzung (Laenge, Magic, Reserve, Namen, Fuellung, Version, Modus, Kennung)");
    }
    /* Fuzz: zufaellige Mutationen und Zufallspuffer - nie Absturz, angenommen nur kanonische Pakete */
    {
        uint32_t rng = 0x454u;
        uint8_t m[160], r2[LLAN_BEACON_LEN + 4];
        int k, acc = 0, noncanon = 0;
        llan_beacon_build(b, sizeof b, &o);
        for (k = 0; k < 200000; k++) {
            int n = LLAN_BEACON_LEN, j, flips;
            rng = rng * 1103515245u + 12345u;
            if ((k & 7) == 0) { /* Zufallspuffer beliebiger Laenge */
                n = (int)((rng >> 8) % 160u);
                for (j = 0; j < n; j++) {
                    rng = rng * 1103515245u + 12345u;
                    m[j] = (uint8_t)(rng >> 16);
                }
                if (n >= 5 && (k & 15) == 0) {
                    memcpy(m, "SBL1", 4);
                    m[4] = 1;
                }
            } else {
                memcpy(m, b, LLAN_BEACON_LEN);
                flips = 1 + (int)((rng >> 12) % 4u);
                for (j = 0; j < flips; j++) {
                    rng = rng * 1103515245u + 12345u;
                    m[(rng >> 8) % LLAN_BEACON_LEN] ^= (uint8_t)(1u << ((rng >> 4) & 7u));
                }
            }
            if (llan_beacon_parse(m, n, &q)) {
                acc++;
                if (llan_beacon_build(r2, sizeof r2, &q) != LLAN_BEACON_LEN || memcmp(r2, m, LLAN_BEACON_LEN) != 0) noncanon++;
            }
        }
        {
            char msg[160];
            _snprintf_s(msg, sizeof msg, _TRUNCATE,
                        "W454: Fuzz 200000 SBL1-Mutationen ohne Absturz, %d angenommen, alle kanonisch (%d abweichend)", acc,
                        noncanon);
            CHECK(noncanon == 0, msg);
        }
    }
    /* Schluessel: beide Seiten gleich mit gleichem Passwort, ungleich mit anderem (wie loc_tun_lan.c/tl_link_pair) */
    {
        static const uint8_t ka[32] = { 0x77, 0x07, 0x6d, 0x0a, 0x73, 0x18, 0xa5, 0x7d, 0x3c, 0x16, 0xc1, 0x72, 0x51, 0xb2,
                                        0x66, 0x45, 0xdf, 0x4c, 0x2f, 0x87, 0xeb, 0xc0, 0x99, 0x2a, 0xb1, 0x77, 0xfb, 0xa5,
                                        0x1d, 0xb9, 0x2c, 0x2a };
        static const uint8_t kb[32] = { 0x5d, 0xab, 0x08, 0x7e, 0x62, 0x4a, 0x8a, 0x4b, 0x79, 0xe1, 0x7f, 0x8b, 0x83, 0x80,
                                        0x0e, 0xe6, 0x6f, 0x3b, 0xb1, 0x29, 0x26, 0x18, 0xb6, 0xfd, 0x1c, 0x2f, 0x8b, 0x27,
                                        0xff, 0x88, 0xe0, 0xeb };
        sbe2e_kp A, B;
        sbe2e_side sa_, sb2;
        sbe2e_link LA, LB, LC;
        uint8_t tha[32], thb[32], thc[32], pskA[32], pskC[32], pkt[128], pt[64];
        int n1, r1, r2;
        memset(&sa_, 0, sizeof sa_);
        memset(&sb2, 0, sizeof sb2);
        memset(&LA, 0, sizeof LA);
        memset(&LB, 0, sizeof LB);
        memset(&LC, 0, sizeof LC);
        sbe2e_kp_import(&A, ka, sa_.pub);
        sbe2e_kp_import(&B, kb, sb2.pub);
        sa_.cid = 100;
        sb2.cid = 200;
        strcpy_s(sa_.pilot, sizeof sa_.pilot, "Alice");
        strcpy_s(sb2.pilot, sizeof sb2.pilot, "Bob");
        llan_psk("LAN", "geheim", 3, pskA);
        llan_psk("LAN", "falsch", 3, pskC);
        sbe2e_transcript(tha, llan_room_id("LAN"), pn, &sa_, &sb2);
        memcpy(thb, tha, 32);
        memcpy(thc, tha, 32);
        llan_th_mix(tha, pskA);
        llan_th_mix(thb, pskA);
        llan_th_mix(thc, pskC);
        r1 = sbe2e_derive(&A, sb2.pub, tha, 100, 200, &LA);
        r2 = sbe2e_derive(&B, sa_.pub, thb, 200, 100, &LB);
        sbe2e_derive(&B, sa_.pub, thc, 200, 100, &LC);
        CHECK(r1 == 0 && r2 == 0 && LA.sid == LB.sid && LA.sid != LC.sid, "W454: gleiche Schluessel nur mit gleichem Raumpasswort");
        memset(pt, 0, sizeof pt);
        pt[0] = 4; /* PING */
        n1 = sbe2e_seal(&LA, pt, 5, pkt, sizeof pkt);
        CHECK(n1 > 0 && sbe2e_open(&LB, pkt, n1, pt, sizeof pt) == 5, "W454: SBT2 mit LAN-Schluessel: Gegenseite oeffnet");
        n1 = sbe2e_seal(&LA, pt, 5, pkt, sizeof pkt);
        CHECK(n1 > 0 && sbe2e_open(&LC, pkt, n1, pt, sizeof pt) < 0, "W454: anderes Raumpasswort: Paket wird verworfen");
        sbe2e_link_free(&LA);
        sbe2e_link_free(&LB);
        sbe2e_link_free(&LC);
        sbe2e_kp_free(&A);
        sbe2e_kp_free(&B);
    }
}

/* Transport im LAN-Modus ueber Loopback (TEST-Ziele statt Broadcast): Instanz i mit Entdeckungs-Port base+10+i. */
typedef struct w454_inst {
    locnet N;
} w454_inst;

static int w454_start(w454_inst *I, int idx, int base, int n, const char *room, const char *pass, const char *ver,
                      const char *pilot)
{
    loctun_cfg c;
    int k;
    size_t l;
    memset(I, 0, sizeof *I);
    if (!locnet_init(&I->N, "random", base + idx * 10, "none", 10, lg)) return 0;
    loctun_cfg_default(&c);
    c.proxy_base = (uint16_t)(base + 3000 + idx * 100);
    c.port = (uint16_t)(base + 900 + idx);
    c.lan = 1;
    c.lan_port = (uint16_t)(base + 10 + idx);
    strcpy_s(c.lan_room, sizeof c.lan_room, room);
    strcpy_s(c.lan_pass, sizeof c.lan_pass, pass);
    strcpy_s(c.pilot, sizeof c.pilot, pilot);
    if (ver) strcpy_s(c.ver_test, sizeof c.ver_test, ver);
    c.lan_targets[0] = 0;
    for (k = 0; k < n; k++) {
        if (k == idx) continue;
        l = strlen(c.lan_targets);
        _snprintf_s(c.lan_targets + l, sizeof c.lan_targets - l, _TRUNCATE, "%s127.0.0.1:%d", l ? "," : "", base + 10 + k);
    }
    I->N.tun = loctun_start(&c, &I->N, NULL, lg);
    return I->N.tun != NULL;
}

static int w454_wait_links(w454_inst *I, int n, int want, int ms)
{
    int t, k, all;
    for (t = 0; t < ms; t += 20) {
        all = 1;
        for (k = 0; k < n; k++) all &= loctun_links_up(I[k].N.tun) == want && I[k].N.npeers == want;
        if (all) return 1;
        Sleep(20);
    }
    return 0;
}

static void w454_stop(w454_inst *I, int n)
{
    int k;
    for (k = 0; k < n; k++) locnet_shutdown(&I[k].N);
}

static volatile LONG s_w454_chat;
static char s_w454_from[24];
static void w454_chat_cb(uint32_t vip, const char *pilot, const uint8_t *d, int len)
{
    (void)vip; (void)d; (void)len;
    strncpy_s(s_w454_from, sizeof s_w454_from, pilot ? pilot : "", _TRUNCATE);
    InterlockedIncrement(&s_w454_chat);
}

static void w454_lan_tun_tests(void)
{
    w454_inst I[3];
    int ok, peers = 0, up = 0, k;
    char need[16], pv[16], nm[24];
    /* drei Clients im selben Raum mit Passwort: jeder findet jeden (Masche), Pilotnamen aus der Entdeckung */
    ok = w454_start(&I[0], 0, 45400, 3, "Party", "geheim", NULL, "Alice") &&
         w454_start(&I[1], 1, 45400, 3, "Party", "geheim", NULL, "Bob") &&
         w454_start(&I[2], 2, 45400, 3, "Party", "geheim", NULL, "Carol");
    CHECK(ok, "W454: drei Transporte im LAN-Modus gestartet (ohne Treffpunkt)");
    ok = ok && w454_wait_links(I, 3, 2, 8000);
    CHECK(ok, "W454: LAN-Entdeckung + Durchstich: drei Clients verbinden sich untereinander (je 2 Verbindungen, SBT2)");
    if (ok) {
        CHECK(loctun_lan_state(I[0].N.tun, &peers, &up) == 1 && peers == 2 && up == 2 && loctun_lobby_state(I[0].N.tun) == 3 &&
                  loctun_sec_state(I[0].N.tun) == 0,
              "W454: LAN-Zustand (2 Mitspieler, 2 verbunden, Raum = Lobby fuer den Chat, sec ok)");
        nm[0] = 0;
        CHECK(loctun_my_pilot(I[1].N.tun, nm, sizeof nm) && strcmp(nm, "Bob") == 0, "W454: eigener Pilotname im LAN");
        nm[0] = 0;
        CHECK(loctun_peer_pilot(I[0].N.tun, I[2].N.self_ip, nm, sizeof nm) && strcmp(nm, "Carol") == 0,
              "W454: Pilotname des Mitspielers aus der Entdeckung (an die Schluessel gebunden)");
        { /* Spiel-Suche (UDP 1201 Broadcast) ueber den Transport an alle */
            SOCKET a = locnet_socket(&I[0].N, AF_INET, SOCK_DGRAM, IPPROTO_UDP), b = locnet_socket(&I[1].N, AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            struct sockaddr_in la = sa(INADDR_ANY, 1201), to = sa(INADDR_BROADCAST, 1201), fr;
            BOOL on = TRUE;
            char q[0x14], r[64];
            int fl = sizeof fr, got = -1, t;
            u_long nb = 1;
            memset(q, 0, sizeof q);
            q[0] = 1;
            locnet_bind(&I[0].N, a, (struct sockaddr *)&la, sizeof la);
            locnet_bind(&I[1].N, b, (struct sockaddr *)&la, sizeof la);
            setsockopt(a, SOL_SOCKET, SO_BROADCAST, (const char *)&on, sizeof on);
            ioctlsocket(b, (long)0x8004667E, &nb);
            locnet_sendto(&I[0].N, a, q, sizeof q, 0, (struct sockaddr *)&to, sizeof to);
            for (t = 0; t < 100 && got < 0; t++) {
                fl = sizeof fr;
                got = locnet_recvfrom(&I[1].N, b, r, sizeof r, 0, (struct sockaddr *)&fr, &fl);
                if (got < 0) Sleep(20);
            }
            CHECK(got == 0x14 && r[0] == 1 && fr.sin_addr.s_addr == I[0].N.self_ip,
                  "W454: System-Link-Suche (Broadcast 1201) erreicht den LAN-Mitspieler mit virtueller Absenderadresse");
            locnet_closesocket(&I[0].N, a);
            locnet_closesocket(&I[1].N, b);
        }
        { /* Lobby-Chat P2P im LAN */
            uint8_t pkt[LCH_PKT_MAX];
            int len = lch_build(pkt, sizeof pkt, 0x454u, 1u, (const uint8_t *)"Hallo LAN", 9);
            s_w454_chat = 0;
            loctun_set_chat(I[2].N.tun, (uint16_t)LCH_PORT_GP, w454_chat_cb);
            loctun_dgram(I[0].N.tun, INADDR_BROADCAST, (uint16_t)LCH_PORT_GP, (uint16_t)LCH_PORT_GP, (const char *)pkt, len);
            for (k = 0; k < 100 && !s_w454_chat; k++) Sleep(20);
            Sleep(200);
            CHECK(s_w454_chat == 1 && strcmp(s_w454_from, "Alice") == 0,
                  "W454: Lobby-Chat im LAN genau einmal beim Mitspieler, Absendername aus der Sitzung");
            loctun_set_chat(I[2].N.tun, (uint16_t)LCH_PORT_GP, NULL);
        }
    }
    w454_stop(I, 3);
    /* falsches Passwort: Entdeckung ja, Verbindung nie (andere Schluessel) */
    ok = w454_start(&I[0], 0, 45600, 2, "Party", "geheim", NULL, "Alice") &&
         w454_start(&I[1], 1, 45600, 2, "Party", "falsch", NULL, "Mallory");
    CHECK(ok, "W454: zwei Transporte mit verschiedenen Passwoertern gestartet");
    if (ok) {
        Sleep(3000);
        loctun_lan_state(I[0].N.tun, &peers, &up);
        CHECK(loctun_links_up(I[0].N.tun) == 0 && loctun_links_up(I[1].N.tun) == 0 && peers == 1 && up == 0 &&
                  loctun_get_stats(I[0].N.tun)->e2e_bad + loctun_get_stats(I[1].N.tun)->e2e_bad > 0,
              "W454: anderes Raumpasswort: gefunden, aber keine Verbindung (Pakete scheitern an der Authentisierung)");
        Sleep(13500); /* LAN_PAIR_MS 15 s: Verbindungsplatz frei, Kennung uebergangen */
        loctun_lan_state(I[0].N.tun, &peers, &up);
        CHECK(peers == 0 && up == 0 && loctun_links_up(I[0].N.tun) == 0,
              "W454: Paar ohne Verbindung nach 15 s freigegeben (kein dauerhaft belegter Platz durch falsches Passwort)");
    }
    w454_stop(I, 2);
    /* andere Version: vor dem Schluesseltausch abgelehnt, Meldung 3 mit der neueren Version */
    ok = w454_start(&I[0], 0, 45700, 2, "LAN", "", "0.4.1", "Alice") && w454_start(&I[1], 1, 45700, 2, "LAN", "", "0.4.2", "Bob");
    CHECK(ok, "W454: zwei Transporte mit verschiedenen Versionen gestartet");
    if (ok) {
        Sleep(2500);
        CHECK(loctun_links_up(I[0].N.tun) == 0 && loctun_version_state(I[0].N.tun, need, sizeof need, pv, sizeof pv) == 3 &&
                  strcmp(need, "0.4.2") == 0 && strcmp(pv, "0.4.2") == 0,
              "W454: andere Client-Version im LAN: keine Verbindung, Meldung (3) mit der neueren Version");
    }
    w454_stop(I, 2);
    /* anderer Raum: still uebergangen (keine Verbindung, keine Versionsmeldung) */
    ok = w454_start(&I[0], 0, 45800, 2, "Rot", "", NULL, "Alice") && w454_start(&I[1], 1, 45800, 2, "Blau", "", NULL, "Bob");
    if (ok) {
        Sleep(2500);
        loctun_lan_state(I[0].N.tun, &peers, &up);
        CHECK(loctun_links_up(I[0].N.tun) == 0 && peers == 0 && loctun_version_state(I[0].N.tun, NULL, 0, NULL, 0) == 0,
              "W454: anderer Raum: still uebergangen (keine Verbindung)");
    } else {
        CHECK(0, "W454: Transporte fuer den Raumtest gestartet");
    }
    w454_stop(I, 2);
    /* ohne Passwort (Standard-Raum LAN) verbinden sich zwei Clients */
    ok = w454_start(&I[0], 0, 45900, 2, "", "", NULL, "Alice") && w454_start(&I[1], 1, 45900, 2, "LAN", "", NULL, "Bob");
    CHECK(ok && w454_wait_links(I, 2, 1, 6000), "W454: Standard-Raum ohne Passwort: zwei Clients verbunden");
    w454_stop(I, 2);
    /* LAN nie zusammen mit dem festen Testweg bzw. Treffpunkt */
    {
        locnet X;
        loctun_cfg c;
        if (locnet_init(&X, "random", 46000, "none", 10, lg)) {
            loctun_cfg_default(&c);
            c.lan = 1;
            strcpy_s(c.peers, sizeof c.peers, "127.0.0.1:46901");
            X.tun = loctun_start(&c, &X, NULL, lg);
            CHECK(X.tun && loctun_lan_state(X.tun, NULL, NULL) == 0, "W454: loc_lan neben dem festen Testweg abgelehnt (LAN aus)");
            locnet_shutdown(&X);
        }
    }
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
    for (i = 1; i < argc; i++) { /* W334: --vectors <pfad>: Krypto/Filter gegen sbt2_vectors.json */
        if (strcmp(argv[i], "--vectors") == 0 && i + 1 < argc) {
            sec_unit_tests();
            w454_lan_unit_tests(); /* W454: LAN-Kern (Vektoren, Fuzz) */
            vector_tests(argv[i + 1], 1);
            printf("loc_net_selftest --vectors: %d/%d OK\n", s_ok, s_n);
            return s_ok == s_n ? 0 : 1;
        }
    }
    for (i = 1; i < argc; i++) { /* W334: --tls-check host:port --spki HEX[,HEX2] --expect ok|fail */
        if (strcmp(argv[i], "--tls-check") == 0 && i + 1 < argc) return tls_check(argc, argv, argv[i + 1]);
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
                if (strcmp(argv[j], "--spki") == 0 && j + 1 < argc) strncpy_s(s_spki, sizeof s_spki, argv[j + 1], _TRUNCATE);
                if (strcmp(argv[j], "--rsim") == 0 && j + 1 < argc) { /* W395 */
                    strncpy_s(s_rsim_a, sizeof s_rsim_a, argv[j + 1], _TRUNCATE);
                    strncpy_s(s_rsim_b, sizeof s_rsim_b, argv[j + 1], _TRUNCATE);
                }
                if (strcmp(argv[j], "--rsim-a") == 0 && j + 1 < argc) strncpy_s(s_rsim_a, sizeof s_rsim_a, argv[j + 1], _TRUNCATE);
                if (strcmp(argv[j], "--rsim-b") == 0 && j + 1 < argc) strncpy_s(s_rsim_b, sizeof s_rsim_b, argv[j + 1], _TRUNCATE);
            }
            return tun_lobby_test(argv[i + 1], nd, sp);
        }
        if (strcmp(argv[i], "--route") == 0 && i + 1 < argc) { /* W395 LOC-ROUTE */
            int j, secs = 30, maxsw = -1;
            const char *ef = NULL, *el = NULL;
            for (j = 1; j < argc - 1; j++) {
                if (strcmp(argv[j], "--spki") == 0) strncpy_s(s_spki, sizeof s_spki, argv[j + 1], _TRUNCATE);
                if (strcmp(argv[j], "--rsim") == 0) {
                    strncpy_s(s_rsim_a, sizeof s_rsim_a, argv[j + 1], _TRUNCATE);
                    strncpy_s(s_rsim_b, sizeof s_rsim_b, argv[j + 1], _TRUNCATE);
                }
                if (strcmp(argv[j], "--rsim-a") == 0) strncpy_s(s_rsim_a, sizeof s_rsim_a, argv[j + 1], _TRUNCATE);
                if (strcmp(argv[j], "--rsim-b") == 0) strncpy_s(s_rsim_b, sizeof s_rsim_b, argv[j + 1], _TRUNCATE);
                if (strcmp(argv[j], "--secs") == 0) secs = atoi(argv[j + 1]);
                if (strcmp(argv[j], "--expect-first") == 0) ef = argv[j + 1];
                if (strcmp(argv[j], "--expect-final") == 0) el = argv[j + 1];
                if (strcmp(argv[j], "--max-switch") == 0) maxsw = atoi(argv[j + 1]);
            }
            if (secs < 5) secs = 5;
            return tun_route_test(argv[i + 1], secs, ef, el, maxsw);
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
        /* W339: Inhaltsregeln Stufe 2 - Gamertag mit NUL, Portrait im Bereich, Pilotname mit NUL */
        buf[0x1F] = 0;
        buf[0x20 + 5] = 0;
        buf[0x20 + 0x26] = 0;
        buf[0x20 + 0x27] = 0;
        buf[0x20 + 0x28 + 22] = 0;
        k = locnet_sendto(&A, a1202, buf, 0x2C4, 0, (struct sockaddr *)&to, sizeof to);
        CHECK(k == 0x2C4, "Beitrittsanfrage gesendet");
    }
    k = poll_recvfrom(&B, b1202, buf, sizeof buf, &from);
    CHECK(k == 0x2C4 && buf[0] == 3, "Host empfaengt die Beitrittsanfrage");
    CHECK(from.sin_addr.s_addr == A.self_ip && ntohs(from.sin_port) == 1202, "Antwortadresse 127.0.0.21:1202");
    memset(buf, 0, 0x308);
    buf[0] = 6; /* W334: Typ 6 (angenommen) hat 0x308 B - kuerzer verwirft der Gastfilter */
    k = locnet_sendto(&B, b1202, buf, 0x308, 0, (struct sockaddr *)&from, sizeof from);
    k = poll_recvfrom(&A, a1202, buf, sizeof buf, &from);
    CHECK(k == 0x308 && buf[0] == 6, "Antwort des Hosts kommt an");
    {
        LONG g0 = A.st.guard;
        struct sockaddr_in toA = sa(A.self_ip, 1202);
        k = locnet_sendto(&B, b1202, "\x06\x00", 2, 0, (struct sockaddr *)&toA, sizeof toA);
        k = poll_recvfrom(&A, a1202, buf, sizeof buf, &from);
        CHECK(k < 0 && A.st.guard > g0, "W334: Gastfilter in recvfrom - Typ 6 mit falscher Laenge erreicht das Spiel nie");
    }
    {
        /* W339: Zustandspruefung fuer UDP (0x38 nur von einem Absender mit Platz in der Spielertabelle, loc_net.c) */
        LONG g0 = A.st.guard;
        struct sockaddr_in toA = sa(A.self_ip, 1202);
        char f38[8] = {0x38, 0, 0, 0, 1, 0, 0, 0};
        locnet_set_udp_state_check(w339_reject_38);
        k = locnet_sendto(&B, b1202, f38, 8, 0, (struct sockaddr *)&toA, sizeof toA);
        k = poll_recvfrom(&A, a1202, buf, sizeof buf, &from);
        CHECK(k < 0 && A.st.guard > g0 && s_w339_from == B.self_ip,
              "W339: 0x38 von unbekanntem Absender verworfen (Zustandspruefung sieht die Absenderadresse)");
        locnet_set_udp_state_check(NULL);
        k = locnet_sendto(&B, b1202, f38, 8, 0, (struct sockaddr *)&toA, sizeof toA);
        k = poll_recvfrom(&A, a1202, buf, sizeof buf, &from);
        CHECK(k == 8 && buf[0] == 0x38, "W339: 0x38 ohne Zustandspruefung zugestellt (Gegenprobe)");
    }

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
    buf[4] = 1;
    k = locnet_sendto(&A, acon, buf, 26, 0, NULL, 0);
    CHECK(k == 26, "0x34-Rahmen ueber sendto(NULL) auf TCP");
    k = recv_all(bacc, buf, 26);
    CHECK(k == 26 && buf[0] == 0x34, "0x34-Rahmen vollstaendig");
    for (i = 0; i < (int)sizeof big; i++) {
        big[i] = (char)(i * 7);
    }
    big[0] = 0x22; /* W334: Typ 0x22 = 0x100C B */
    big[4] = 0; big[5] = 0x10; big[6] = 0; big[7] = 0; /* W339: Blocklaenge 0x1000 */
    big[1] = 0;
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
    sec_unit_tests(); /* W334 */
    hle_recv_tests(); /* W334 */
    w350_tests();     /* W350 LOC-REJOIN */
    {
        /* W334: Vektoren am Standardort (Exe in harness/build/Release) */
        char vp[MAX_PATH], *sl;
        GetModuleFileNameA(NULL, vp, sizeof vp);
        sl = strrchr(vp, '\\');
        if (sl) *sl = 0;
        strcat_s(vp, sizeof vp, "\\..\\..\\..\\net\\tools\\sbt2_vectors.json");
        vector_tests(vp, 0);
    }
    if (loctun_available()) {
        tun_tests(); /* W324 */
        tun_guard_tests(); /* W329 */
        w335_tests();      /* W335 NET-AUDIT */
        w350_tun_tests();  /* W350 LOC-REJOIN */
        w409_tun_tests();  /* W409 LOC-MESH-KICK */
        tun_lobby_guard_tests(); /* W332 */
        pub_tests();             /* W373 LOC-PUBLIC-LOBBY */
        route_feed_tests();      /* W395 LOC-ROUTE */
        w420_ver_tests();        /* W420 LOC-CLIPDUMP: Client-Version im HELLO */
        w437_chat_tun_tests();   /* W437 LOC-CHAT: Chatkanal */
        w440_mode_tun_tests();   /* W440 LOC-LOBBY-MODES: play_mode im Transport */
        w443_tun_tests();        /* W443 LOC-LOBBY-LIST: Modus je Zeile der Suchliste */
        w446_snchat_tun_tests(); /* W446 STEELNET-CHAT: steelnet-Chat im Steuerkanal */
        w454_lan_tun_tests();    /* W454 LOC-LAN-MODE: LAN ohne steelnet */
    } else {
        printf("Hinweis: eigener Transport nur in der privaten Fassung - Transport-Pruefungen uebersprungen\n");
    }

    route_unit_tests(); /* W395 */
    w420_rule_tests(); /* W420 LOC-CLIPDUMP: Match-Regel */
    w440_las_tests();  /* W440 LOC-LOBBY-MODES: Durchsetzungsregel */
    w443_llv_tests();  /* W443 LOC-LOBBY-LIST: Zuordnung Zeile <-> Lobby */
    w454_lan_unit_tests(); /* W454 LOC-LAN-MODE: LAN-Kern */
    pause_net_tests(); /* W357 */
    printf("loc_net_selftest: %d/%d OK\n", s_ok, s_n);
    return s_ok == s_n ? 0 : 1;
}
