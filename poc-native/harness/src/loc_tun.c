/* loc_tun.c - W324 LOC-NETPLAY: UDP-Transport mit Redundanz + steelnet-Anbindung (siehe loc_tun.h).
 *
 * Aufbau (ein Thread, alle Sockets mit EINEM Netz-Ereignis, dazu ein Ereignis fuer Spiel-UDP):
 *   Spiel --TCP--> Stellvertreter vip:proxy_base (Loopback) --+
 *   Spiel --UDP--> loctun_dgram (aus locnet_sendto)          --+--> Verbindung je Mitspieler --UDP--> direkt/Relay
 *   Spiel <-TCP--- Verbindung des Stellvertreters (bind vip)  <-+
 *   Spiel <-UDP--- Stellvertreter-UDP (bind vip:proxy_base+x) <-+
 * Paket "SBT1" (Little Endian): Kopf 10 B (Magic, Typ, Flags, Sitzung), DATA zusaetzlich Paketnummer,
 * kumulative Bestaetigung, Zeitstempel + Echo (RTT), gemessener Eingangsverlust, Eintraege:
 *   Strom-Nachricht: Art 1, seq u32, sub u8 (1 OPEN, 2 DATA, 3 FIN), Strom u16, Kopie u8, Laenge u16, Daten
 *   Datagramm:       Art 2, id u16, Quellport u16, Zielport u16, Kopie u8, Laenge u16, Daten
 * Jede Nachricht geht in bis zu N Paketen hinaus (Abstand >= gap_ms), danach nur noch nach RTO.
 *
 * W334 LOC-SEC-CLIENT (Protokoll v2, net/SECURITY.md): Steuerkanal TLS mit SPKI-Pin (sb_tls), jedes
 * Transportpaket als SBT2 (AES-256-GCM, Schluessel je Paar aus X25519 + Transkript, sb_e2e) - auch Durchstich
 * (PROBE/PROBE_ACK) und Relay-Nutzlast; die alten Klartextformate SBT1/SBP1 werden ohne Ausnahme verworfen.
 * Der Klartext eines SBT2-Pakets ist [u8 Typ][Rumpf wie v1 nach dem alten 10-Byte-Kopf] (Little Endian).
 * Vor jeder Uebergabe ans Spiel: Gastfilter (loc_guard) - Verstoss = Mitspieler getrennt. */
#define _CRT_RAND_S
#define FD_SETSIZE 256
#include "loc_tun.h"
#include "loc_net_core.h"
#include "loc_netsim.h"
#include "loc_route.h"
#include "sb_sha256.h"
#include "sb_e2e.h"
#include "sb_tls.h"
#include "loc_guard.h"
#include "steelnet_pins.h"
#include "sb_version.h" /* W421: sb_netver_cmp (gemeinsam mit dem Launcher) */
#include <ws2tcpip.h>
#include <mmsystem.h>
#include <math.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "winmm.lib")

#define TUN_MAX_LINKS 12
#define TUN_MAX_STREAMS 48
#define TUN_SQ 2048
#define TUN_RQ 2048
#define TUN_DQ 64
#define TUN_PKT 1200 /* = steelnet MAX_RELAY_PAYLOAD */
#define TUN_MSG 1000
#define TUN_MAX_RELAYS 8
#define TUN_MAX_CAND 9
#define TUN_FLOW_BYTES (256 * 1024)
#define TUN_SEEN 256

#define T_HELLO 1
#define T_DATA 3
#define T_PING 4
#define T_PONG 5
#define T_BYE 6
#define T_PROBE 7     /* W334: Durchstich als SBT2 (u64 probe_id) */
#define T_PROBE_ACK 8 /* W334: Antwort an die Absenderadresse des PROBE */
/* W395: Wegprobe = T_PING mit 8 Byte Rumpf ['R']['T'][u8 Slot][u8 0][u16 seq][u16 0], Echo als T_PONG gleicher Groesse
 * ueber denselben Weg (keine neuen inneren Typen: sb_e2e laesst nur die bekannten zu; alte Gegenstellen antworten mit
 * 4 Byte und werden ignoriert, ihre k-Pruefung (k < 5) verwirft unsere Probe). */
#define RP_LEN 8
#define HELLO_F_ROUTE 1 /* W395: HELLO-Flag "waehlt den Weg selbst, beantwortet Wegproben" */
#define HELLO_F_VER 2   /* W420: HELLO traegt die Client-Version (16 Byte nach den Flags, NUL-gefuellt) */
#define HELLO_VLEN 16
#define HELLO_LEN (HDR + 8 + HELLO_VLEN)
#ifndef SB_NET_VERSION
#define SB_NET_VERSION "dev"
#endif
#define TUN_ROUTE_SIMS 4 /* W395 TEST loc_route_sim: Wege mit eigener Verzoegerung/Ausfall */
#define K_STREAM 1
#define K_DGRAM 2
#define S_OPEN 1
#define S_DATA 2
#define S_FIN 3
#define HDR 1 /* W334: Klartext-Kopf im SBT2-Paket = nur der innere Typ (v1: 10 Byte "SBT1"+Typ+Flags+Sitzung) */
#define DHDR (HDR + 21)
#define TUN_PT SBE2E_MAX_PT /* 1168 Byte Klartext je SBT2-Paket (1200 mit Kopf und Tag = Relay-Nutzlast) */

/* steelnet UDP (PROTOKOLL.md, v2: SECURITY.md §5) */
#define SN_BIND_ACK 0x02
#define SN_PING 0x04
#define SN_PONG 0x05
#define SN_TBIND 0x06
#define SN_TBIND_ACK 0x07
#define SN_TBIND_COOKIE 0x0A
#define SN_TBIND_CONFIRM 0x0B
#define SN_RELAY 0x10
#define SN_RELAY_FROM 0x11
#define SN_BIND2 0x21
#define SN_KEEPALIVE2 0x23
#define TICKET_LEN 32
#define TUN_DEC_RATE 2000.0 /* Entschluesselungsversuche je Verbindung und Sekunde (SECURITY.md §4.5) */
#define TUN_ACK_RATE 20.0   /* PROBE_ACK je Verbindung und Sekunde (§4.6) */
#define TUN_RP_RATE 100.0   /* W395: Wegproben-Antworten je Verbindung und Sekunde (9 Wege x 10/s Proben) */
#define TUN_DG_RATE 60.0    /* Datagramme zum Spiel je Mitspieler und Port (§6.4), Burst 120 */
#define TUN_MAX_GHOST 24    /* W373: oeffentliche Lobbys je Suche (Spiel nimmt bis 49 Hosts) */

typedef struct tmsg {
    uint32_t seq;
    uint16_t stream, len;
    uint8_t sub, tx;
    double last, arr;
    uint8_t data[1];
} tmsg;

typedef struct tdg {
    int used;
    uint16_t id, src, dst, len;
    uint8_t tx;
    double last;
    uint8_t data[TUN_PKT];
} tdg;

typedef struct tdpx {
    uint16_t gp;    /* Quellport beim Mitspieler (Spielport oder roh) */
    uint16_t local; /* tatsaechlicher lokaler Port des Stellvertreters */
    SOCKET s;
} tdpx;

typedef struct trelay {
    char name[24], host[96];
    uint16_t port;
    struct sockaddr_in addr;
    int resolved, pings, nrtt;
    double ping_at[5], rtt[5], ms;
    uint8_t nonce[8];
} trelay;

typedef struct tlink {
    int used, up, got_hello, peer_got, down_logged;
    int ver_bad;    /* W420: Mitspieler mit anderer bzw. ohne Client-Version (steelnet) - abgelehnt */
    int bad_logged; /* W329: Bit 1 Port, 2 vip, 4 Puffer, 8 eigene vip (W332) - je Grund nur einmal protokollieren */
    uint32_t cid, vip;
    char pilot[24];
    /* W334: Ende-zu-Ende-Schutz (SBT2) und Gastfilter */
    sbe2e_link e2e;
    uint8_t pair_nonce[16], peer_pub[32];
    uint32_t account;
    double info_at;       /* W353: Zeitpunkt des letzten peer_info (Fehlergrund "path", wenn kein Weg aufgeht) */
    int blocked;          /* Verstoss: getrennt bis zu einem neuen peer_info (neues pair_nonce) */
    int drop_pending;     /* Paketgrenze erreicht: Verbindung abbauen */
    uint64_t probe_id;    /* offene Durchstich-Kennung (CSPRNG) */
    lguard_bucket b_dec, b_ack, b_dg[2];
    lguard_bucket b_app; /* W338: R2-Eingabekanal */
    lguard_bucket b_app2; /* W420: Regelkanal (eigene Bremse, stoert den R2-Kanal nicht) */
    lguard_bucket b_rp;  /* W395: Antworten auf Wegproben */
    struct sockaddr_in cand[TUN_MAX_CAND];
    int ncand;
    double punch_start, punch_last;
    int punching, direct_ok, direct_failed, dpings, ndrtt;
    struct sockaddr_in direct;
    double dping_at[5], drtt[5], direct_rtt, dping_next;
    struct sockaddr_in seen_from; /* Absender eines gueltigen PROBE (Weg zu uns offen) */
    int seen_ok;
    int relay_idx, want, path, reported, rep_dirty, rep_link; /* W395: relay_idx = aktives Relay */
    uint32_t relay_link;
    uint8_t ticket[TICKET_LEN];
    /* W395 LOC-ROUTE: dieselbe link_id auf allen Relays gebunden, Wahl im Client (loc_route) */
    int want_ri;          /* vom Treffpunkt gewaehltes Relay (alte Logik), -1 */
    int have_ticket, peer_route, rt_on, route_seen;
    struct { int ok, have_cookie, tries; uint8_t cookie[16]; double bind_last; } rb[TUN_MAX_RELAYS];
    locroute rt;
    double rt_last_ok[LOCROUTE_MAXP]; /* fuer die Wiederbindung eines stillen Relays */
    double hello_last, route_ms;
    /* zuverlaessiger Kanal */
    uint32_t snd_next, snd_una, rcv_next;
    tmsg *sq[TUN_SQ];
    tmsg *rq[TUN_RQ];
    int sq_bytes;
    tdg dq[TUN_DQ];
    uint16_t dg_next;
    uint16_t seen[TUN_SEEN];
    int nseen, seen_pos;
    uint32_t pkt_seq, rx_hi, rx_cnt, rx_hi0, rx_cnt0;
    double loss_t;
    int loss_in, loss_peer, red;
    uint32_t echo_ts;
    double echo_at, srtt, rttvar, rtt_min;
    int rtt_n;
    double last_rx, last_tx, ack_due;
    int ack_pending;
    SOCKET lsn;
    tdpx dpx[8];
    int ndpx;
    uint16_t stream_ctr;
    LONG s_tx, s_rx, s_copy, s_rto, s_saved, s_dup, s_new, s_dgtx, s_dgrx;
} tlink;

/* W373 LOC-PUBLIC-LOBBY: oeffentliche Lobby eines anderen Hosts, lokal vertreten (Host-Info, Beitritt). */
typedef struct tghost {
    int used, lid, est;  /* Lobby-Id, Ping-Schaetzung ms (-1 = unbekannt) */
    uint32_t vip;        /* vip des Hosts (aus rules, geprueft) */
    char name[16];       /* Anzeigename <= 15 Zeichen */
    uint8_t sf[8], cf[2];/* Sitze/Lager/Mission/Optionen, Zaehler je Seite (geprueft) */
    SOCKET s[2];         /* vip:Stellvertreter+201 / +202 (erst beim Senden) */
    unsigned ans;        /* Suche (pub_seq), fuer die schon geantwortet wurde */
    double seen;
} tghost;

typedef struct tstream {
    int used, link, connecting, rd_closed, fin_rx, fin_sent, wr_shut;
    /* W409 LOC-MESH-KICK: peer = Strom vom Mitspieler geoeffnet (wir verbinden zum eigenen Spiel); held = eigenes Spiel
     * nahm TCP 1000 (noch) nicht an, Strom gehalten bis retry_at; tries = Versuche; rx_any/tx_any = schon Bytes vom
     * Mitspieler bzw. vom eigenen Spiel; t_open = Zeitpunkt des OPEN (Diagnose "sofort geschlossen") */
    int peer, held, tries, rx_any, tx_any;
    double t_open, retry_at;
    uint16_t id;
    SOCKET s;
    uint8_t *ob;
    int olen, ocap;
    lguard_tcp *g; /* W334: Rahmenfilter fuer die Bytes des Mitspielers zum Spiel */
    struct { int rem, hl, fofs, mut, rule; unsigned typ; double pb; uint8_t h[5]; } fz; /* W335: Sende-Fuzzer (TEST); W339: rule */
} tstream;

enum { LB_OFF, LB_CONNECTING, LB_HELLO, LB_LOBBY, LB_TLS };
/* W334: Sicherheitszustand fuer Log/Launcher (loctun_sec_state) */
enum { SEC_OK = 0, SEC_SELFTEST = 1, SEC_PIN = 2, SEC_NOPIN = 3, SEC_UPGRADE = 4, SEC_VERSION = 5 /* W420 */ };

struct loctun {
    loctun_cfg c;
    /* W328 STEELNET-ACCOUNT: Konto fuer die Anmeldung am Treffpunkt (Spielschluessel G, nie der Geraeteschluessel) */
    uint32_t acct;
    long long acct_exp;
    uint8_t gkey[32];
    volatile LONG auth_state; /* loctun_auth_state */
    locnet *n;
    locnetsim *sim;
    loctun_log_fn log;
    CRITICAL_SECTION cs;
    HANDLE ev_net, ev_app, th, timer;
    volatile LONG stop;
    SOCKET us; /* Transport-UDP */
    uint16_t uport;
    uint32_t bind_ip;
    double qpf_ms, t0;
    tlink links[TUN_MAX_LINKS];
    tstream streams[TUN_MAX_STREAMS];
    loctun_stats st;
    volatile uint16_t app_gp;      /* W338: Kanal-Spielport (0 = aus) */
    loctun_app_fn volatile app_fn; /* W338: Empfaenger im Client */
    volatile uint16_t app2_gp;      /* W420: zweiter Kanal (Match-Regel, loc_rule.c), 0 = aus */
    loctun_app_fn volatile app2_fn;
    /* Treffpunkt */
    int lb_state, lb_lobby_id, lb_joined, lb_listed;
    SOCKET lb_s;
    struct sockaddr_in lb_addr;
    char lb_host[96];
    uint16_t lb_port;
    char lb_in[65536]; /* W329: eine Steuerzeile (Lobbyliste mit rules) kann mehrere KB lang sein */
    int lb_inlen;
    double lb_next_try, lb_ping_last, lb_bind_last, lb_list_last, lb_ka_last;
    uint32_t my_cid;
    int bound, ref;
    /* W334: Protokoll v2 */
    sbtls *tls;
    uint8_t pins[2][32];
    int npins;
    sbe2e_kp kp;        /* fluechtiges X25519-Paar dieser Steuerverbindung */
    uint8_t my_pub[32];
    uint8_t udp_key[32];/* BIND2/KEEPALIVE2-MAC (aus welcome) */
    uint64_t bctr;
    char my_pilot[17];  /* wirksamer Name aus welcome (Transkript) */
    char lobby_code[16];/* Raumcode der eigenen Lobby (Anzeige im Launcher) */
    char need_ver[16];  /* W420: vom Treffpunkt verlangte bzw. neuere Version eines Mitspielers (nur [0-9A-Za-z.+-]) */
    char peer_ver[16];  /* W420: Version des abgelehnten Mitspielers ("" = Feld fehlt, aelterer Client) */
    volatile LONG ver_state; /* W420: 0 nichts, 1 Treffpunkt lehnt ab, 2 Mitspieler mit anderer Version abgelehnt */
    char lb_err[12];    /* W353: letzter Fehlergrund fuer den Launcher (err=, leer = keiner) */
    int lb_host_left;   /* W366: Host hat die Lobby geschlossen -> err=closed statt code beim erneuten Beitritt */
    volatile LONG netcut; /* W366 TEST (netcut_file): Netz unterbrochen - UDP ein/aus verwerfen */
    double netcut_chk;
    volatile LONG sec;  /* SEC_* */
    int lb_code_tries;
    struct sockaddr_in srv_udp;
    trelay relays[TUN_MAX_RELAYS];
    int nrelays, relays_done;
    double relays_start;
    char route_mode[16];
    double route_max;
    double status_last;
    /* W335 NET-AUDIT: Sende-Fuzzer (TEST, nur Testweg ohne Treffpunkt, Umgebung SB_LOC_FUZZ_TX=<Startwert>) */
    uint32_t fz_rng;
    int fz_on;
    int fz_rule; /* W339 TEST: SB_LOC_FUZZ_RULE - Regel-Felder gezielt verletzen (1 = zusaetzlich, 2 = nur diese) */
    LONG fz_frames, fz_bytes, fz_dg;
    DWORD log_t0; /* W332: Log-Obergrenze je 10 s (tl_logf) */
    unsigned log_n, log_drop;
    /* W373 LOC-PUBLIC-LOBBY */
    int pub;                    /* oeffentlicher Modus (loc_lobby_vis=public, kein Raumcode) */
    int pub_host, pub_started, pub_creating, pub_nmem;
    uint32_t pub_host_cid, pub_host_vip, pub_join_vip;
    uint8_t pub_sig[10];        /* zuletzt veroeffentlichte Spielfelder */
    double pub_upd_at, pub_chg_at, pub_off_at, pub_create_at, pub_list_at, pub_t1_at, pub_join_at, pub_idle_at;
    volatile LONG pub_t1_new;   /* Suche des Spiels noch unbeantwortet */
    unsigned pub_seq;
    uint8_t pub_nonce[8];
    tghost gh[TUN_MAX_GHOST];
    int pub_join, pub_joining, pub_join_bad, pub_ngames, pub_bad_logged;
    LONG pub_answered;
    uint8_t pub_t3[0x2C4];      /* Beitrittsanfrage des eigenen Spiels (bis die Verbindung steht) */
    int pub_t3_len;
    uint16_t pub_t3_src;
    /* W395 TEST loc_route_sim: je Weg eigene Verzoegerungsstufe bzw. Ausfallfenster */
    struct { char name[24]; locnetsim *sim; double cut_a, cut_b; int cut_logged; } rsim[TUN_ROUTE_SIMS];
    int nrsim;
    unsigned relay_usable;      /* W395: Bit i = Relay i darf gewaehlt werden (route "relays"; ohne Feld alle) */
    int relay_usable_set;
    char *test_cap;             /* W373 TEST (loctun_test_line): gesendete Steuerzeilen mitschneiden */
    int test_cap_len, test_cap_max;
};
/* W373: oeffentliche Lobbys (Funktionen unten bei lb_enter) */
static void pub_on_link_up(loctun *t, tlink *L);
static void pub_reject(loctun *t, int gi, const char *why, double now);
static void pub_ghosts_clear(loctun *t);

/* ====================================================================== Hilfen */
static void tl_logf(loctun *t, const char *fmt, ...)
{
    char b[600];
    va_list ap;
    DWORD now;
    if (!t->log) return;
    /* W332: Viele Zeilen haengen an Paketen von Mitspielern bzw. Zeilen des Treffpunkts. Ohne Obergrenze kann eine
     * Gegenstelle das Log (und die Platte) in Paketrate fuellen: hoechstens 200 Zeilen je 10 s, Rest gezaehlt. */
    now = GetTickCount();
    if (now - t->log_t0 >= 10000u) {
        if (t->log_drop) {
            _snprintf_s(b, sizeof b, _TRUNCATE, "LOCTUN: %u Logzeilen unterdrueckt (hoechstens 200 je 10 s)", t->log_drop);
            t->log(b);
        }
        t->log_t0 = now;
        t->log_n = 0;
        t->log_drop = 0;
    }
    if (t->log_n >= 200u) {
        t->log_drop++;
        return;
    }
    t->log_n++;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    t->log(b);
}

static double tl_now(const loctun *t)
{
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return (double)q.QuadPart * t->qpf_ms - t->t0;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)get16(p) | ((uint32_t)get16(p + 2) << 16); }
static void put32be(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t get32be(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* W328: HMAC-SHA256 (RFC 2104) ueber sb_sha256; Schluessel <= 64 Byte. */
static void tl_hmac(const uint8_t *key, size_t klen, const char *msg, size_t mlen, uint8_t out[32])
{
    uint8_t k[64], pad[64], inner[32];
    sb_sha256_t c;
    int i;
    memset(k, 0, sizeof k);
    memcpy(k, key, klen > 64 ? 64 : klen);
    for (i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    sb_sha256_init(&c);
    sb_sha256_update(&c, pad, 64);
    sb_sha256_update(&c, msg, mlen);
    sb_sha256_final(&c, inner);
    for (i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    sb_sha256_init(&c);
    sb_sha256_update(&c, pad, 64);
    sb_sha256_update(&c, inner, 32);
    sb_sha256_final(&c, out);
    SecureZeroMemory(k, sizeof k);
    SecureZeroMemory(pad, sizeof pad);
}

static int unhex(const char *s, uint8_t *out, int n)
{
    int i;
    if ((int)strlen(s) != 2 * n) return 0;
    for (i = 0; i < n; i++) {
        int a = hexval(s[2 * i]), b = hexval(s[2 * i + 1]);
        if (a < 0 || b < 0) return 0;
        out[i] = (uint8_t)(a * 16 + b);
    }
    return 1;
}

static void tohex(const uint8_t *p, int n, char *out)
{
    static const char hx[] = "0123456789abcdef";
    int i;
    for (i = 0; i < n; i++) {
        out[2 * i] = hx[p[i] >> 4];
        out[2 * i + 1] = hx[p[i] & 15];
    }
    out[2 * n] = 0;
}

static int sa_eq(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

static int tl_dest_ok(loctun *t, uint32_t ip)
{
    if (ip == 0 || ip == INADDR_BROADCAST) return 0;
    return locnet_ip_allowed(ip) || t->c.allow_public;
}

static int tl_resolve(const char *host, uint16_t port, struct sockaddr_in *out)
{
    struct addrinfo hints, *res = NULL;
    char ps[8];
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    _snprintf_s(ps, sizeof ps, _TRUNCATE, "%u", port);
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) return 0;
    memcpy(out, res->ai_addr, sizeof *out);
    freeaddrinfo(res);
    return 1;
}

static int tl_split_hostport(const char *s, char *host, int hl, uint16_t *port)
{
    const char *c = strrchr(s, ':');
    int p;
    if (!c || c == s) return 0;
    p = atoi(c + 1);
    if (p <= 0 || p > 65535) return 0;
    if ((int)(c - s) >= hl) return 0;
    memcpy(host, s, (size_t)(c - s));
    host[c - s] = 0;
    *port = (uint16_t)p;
    return 1;
}

/* W395 TEST loc_route_sim: Eintrag fuer den Weg zu/von einer Adresse (Relayname, sonst "direct"), -1 = keiner. */
static int tl_rsim_find(loctun *t, const struct sockaddr_in *a)
{
    const char *name = "direct";
    int i;
    for (i = 0; i < t->nrelays; i++) {
        if (t->relays[i].resolved && sa_eq(a, &t->relays[i].addr)) {
            name = t->relays[i].name;
            break;
        }
    }
    if (i == t->nrelays && sa_eq(a, &t->srv_udp)) return -1; /* Treffpunkt-UDP ohne gleichnamiges Relay */
    for (i = 0; i < t->nrsim; i++) {
        if (strcmp(t->rsim[i].name, name) == 0) return i;
    }
    return -1;
}

/* 1 = Weg k ist gerade im Ausfallfenster (beide Richtungen verwerfen). */
static int tl_rsim_cut(loctun *t, int k)
{
    double s;
    if (k < 0 || t->rsim[k].cut_a < 0) return 0;
    s = (tl_now(t) - 1000.0) / 1000.0; /* t0 liegt 1 s vor dem Start */
    if (s >= t->rsim[k].cut_a && (t->rsim[k].cut_b < 0 || s < t->rsim[k].cut_b)) {
        if (!(t->rsim[k].cut_logged & 1)) {
            t->rsim[k].cut_logged |= 1;
            tl_logf(t, "LOCROUTE: TEST Weg %s ausgefallen (t=%.1f s)", t->rsim[k].name, s);
        }
        return 1;
    }
    if ((t->rsim[k].cut_logged & 3) == 1) {
        t->rsim[k].cut_logged |= 2;
        tl_logf(t, "LOCROUTE: TEST Weg %s wieder da (t=%.1f s)", t->rsim[k].name, s);
    }
    return 0;
}

/* loc_route_sim zerlegen: "name:SPEC/name:SPEC", SPEC = loc_netsim-Teile plus cut:A-B. */
static void tl_rsim_setup(loctun *t)
{
    char buf[256], *ctx = NULL, *part;
    strcpy_s(buf, sizeof buf, t->c.route_sim);
    for (part = strtok_s(buf, "/", &ctx); part && t->nrsim < TUN_ROUTE_SIMS; part = strtok_s(NULL, "/", &ctx)) {
        char *c = strchr(part, ':'), spec[200] = "", err[96] = "", *q, *ctx2 = NULL;
        locnetsim_cfg sc;
        int k = t->nrsim;
        if (!c) continue;
        *c++ = 0;
        memset(&t->rsim[k], 0, sizeof t->rsim[k]);
        strncpy_s(t->rsim[k].name, sizeof t->rsim[k].name, part, _TRUNCATE);
        t->rsim[k].cut_a = t->rsim[k].cut_b = -1;
        for (q = strtok_s(c, ",", &ctx2); q; q = strtok_s(NULL, ",", &ctx2)) {
            if (_strnicmp(q, "cut:", 4) == 0) {
                const char *dash = strchr(q + 4, '-');
                t->rsim[k].cut_a = atof(q + 4);
                t->rsim[k].cut_b = (dash && dash[1]) ? atof(dash + 1) : -1;
            } else {
                if (spec[0]) strcat_s(spec, sizeof spec, ",");
                strncat_s(spec, sizeof spec, q, _TRUNCATE);
            }
        }
        if (spec[0]) {
            if (locnetsim_parse(spec, &sc, err, sizeof err)) {
                t->rsim[k].sim = locnetsim_start(&sc, NULL);
                if (t->rsim[k].sim) locnetsim_mark_start(t->rsim[k].sim, GetTickCount()); /* ramp ab Transportstart */
            } else {
                tl_logf(t, "LOCROUTE: TEST loc_route_sim %s ungueltig (%s)", t->rsim[k].name, err);
            }
        }
        tl_logf(t, "LOCROUTE: TEST Weg %s: %s%s, Ausfall %.0f-%.0f s", t->rsim[k].name, spec[0] ? spec : "ohne Verzoegerung",
                t->rsim[k].sim ? "" : (spec[0] ? " (aus)" : ""), t->rsim[k].cut_a, t->rsim[k].cut_b);
        t->nrsim++;
    }
}

/* Rohes Senden ueber den Transport-Socket (ueber die Verzoegerungsstufe, falls aktiv). */
static void tl_sendto(loctun *t, const struct sockaddr_in *to, const uint8_t *buf, int len)
{
    if (!tl_dest_ok(t, to->sin_addr.s_addr)) {
        InterlockedIncrement(&t->st.bad);
        return;
    }
    if (t->netcut) return; /* W366 TEST: Netz weg */
    if (t->nrsim) { /* W395 TEST: Weg mit eigener Verzoegerung bzw. im Ausfallfenster */
        int k = tl_rsim_find(t, to);
        if (k >= 0 && tl_rsim_cut(t, k)) return;
        if (k >= 0 && t->rsim[k].sim) {
            locnetsim_send(t->rsim[k].sim, t->us, 0, (const char *)buf, len, 0, to);
            return;
        }
    }
    if (t->sim) {
        locnetsim_send(t->sim, t->us, 0, (const char *)buf, len, 0, to);
    } else {
        sendto(t->us, (const char *)buf, len, 0, (const struct sockaddr *)to, sizeof *to);
    }
}

static void tl_hdr(uint8_t *p, int type)
{
    p[0] = (uint8_t)type; /* W334: innerer Typ; Magic/Sitzung stehen im SBT2-Kopf */
}

static void tl_link_drop(loctun *t, tlink *L, const char *why);

/* W334: Klartext versiegeln (SBT2). 0 = kein Paket (keine Schluessel, Grenze erreicht). */
static int tl_seal(loctun *t, tlink *L, const uint8_t *pt, int len, uint8_t *out)
{
    int n;
    if (!L->e2e.valid || L->blocked) return 0;
    n = sbe2e_seal(&L->e2e, pt, len, out, TUN_PKT);
    if (n <= 0) {
        InterlockedIncrement(&t->st.bad);
        return 0;
    }
    return n;
}

/* Nach dem Senden: harte ctr-Grenze 2^32 erreicht -> Verbindung beenden (kein Schluesselwechsel, §4.5). */
static void tl_after_seal(loctun *t, tlink *L)
{
    (void)t;
    if (sbe2e_exhausted(&L->e2e)) L->drop_pending = 1; /* Abbau im naechsten Zeitgeber-Durchlauf */
}

/* Versiegeltes Paket an eine Adresse (Durchstich, Antworten auf dem Ankunftsweg). */
static void tl_send_to(loctun *t, tlink *L, const struct sockaddr_in *to, const uint8_t *pt, int len)
{
    uint8_t pkt[TUN_PKT];
    int n = tl_seal(t, L, pt, len, pkt);
    if (n <= 0) return;
    tl_sendto(t, to, pkt, n);
    tl_after_seal(t, L);
}

/* W395: Paket ueber einen bestimmten Weg-Slot (0 = direkt, 1+i = Relay i; SBT2 unveraendert als Relay-Nutzlast). */
static void tl_send_slot(loctun *t, tlink *L, int slot, const uint8_t *pt, int len)
{
    uint8_t b[TUN_PKT + 16];
    int n;
    if (slot == 0) {
        if (!L->direct_ok) return;
    } else if (slot < 1 || slot > t->nrelays || !t->relays[slot - 1].resolved || L->relay_link == 0) {
        return;
    }
    n = tl_seal(t, L, pt, len, b + 9);
    if (n <= 0) return;
    if (slot == 0) {
        tl_sendto(t, &L->direct, b + 9, n);
    } else {
        memcpy(b, "SBN1", 4);
        b[4] = SN_RELAY;
        put32be(b + 5, L->relay_link);
        tl_sendto(t, &t->relays[slot - 1].addr, b, n + 9);
    }
    L->last_tx = tl_now(t);
    InterlockedIncrement(&L->s_tx);
    InterlockedIncrement(&t->st.pkt_tx);
    tl_after_seal(t, L);
}

/* Paket an den Mitspieler ueber den gewaehlten Weg (path 1 direkt, 2 Relay relay_idx). W395: auf dem aktiven Weg
 * waehrend eines Wechsels bzw. bei verdaechtigem Weg zusaetzlich ueber den zweiten Weg (der Empfaenger entdoppelt). */
static void tl_send_link(loctun *t, tlink *L, int path, const uint8_t *pt, int len)
{
    int slot;
    if (path == 1) slot = 0;
    else if (path == 2 && L->relay_idx >= 0) slot = 1 + L->relay_idx;
    else return;
    tl_send_slot(t, L, slot, pt, len);
    if (L->rt_on && path == L->path) {
        int d = locroute_dual(&L->rt, tl_now(t));
        if (d >= 0 && d != slot) tl_send_slot(t, L, d, pt, len);
    }
}

static tlink *tl_link_by_cid(loctun *t, uint32_t cid, int create)
{
    int i, fr = -1;
    for (i = 0; i < TUN_MAX_LINKS; i++) {
        if (t->links[i].used && t->links[i].cid == cid) return &t->links[i];
        if (!t->links[i].used && fr < 0) fr = i;
    }
    if (!create || fr < 0) return NULL;
    memset(&t->links[fr], 0, sizeof t->links[fr]);
    t->links[fr].used = 1;
    t->links[fr].cid = cid;
    t->links[fr].relay_idx = -1;
    t->links[fr].want_ri = -1;
    locroute_init(&t->links[fr].rt); /* W395 */
    t->links[fr].lsn = INVALID_SOCKET;
    t->links[fr].srtt = 0;
    t->links[fr].red = t->c.red_fixed > 0 ? t->c.red_fixed : 3;
    return &t->links[fr];
}

static tlink *tl_link_by_vip(loctun *t, uint32_t vip)
{
    int i;
    for (i = 0; i < TUN_MAX_LINKS; i++) {
        if (t->links[i].used && t->links[i].up && t->links[i].vip == vip) return &t->links[i];
    }
    return NULL;
}

static double tl_rto(const tlink *L)
{
    double r;
    if (L->rtt_n == 0) return 200.0;
    r = L->srtt + 4.0 * L->rttvar;
    if (r < 30.0) r = 30.0;
    if (r > 1000.0) r = 1000.0;
    return r;
}

static void tl_rtt_sample(tlink *L, double s)
{
    if (s < 0) s = 0;
    if (s > 30000) return;
    if (L->rtt_n == 0) {
        L->srtt = s;
        L->rttvar = s / 2;
        L->rtt_min = s;
    } else {
        L->rttvar = 0.75 * L->rttvar + 0.25 * fabs(L->srtt - s);
        L->srtt = 0.875 * L->srtt + 0.125 * s;
        if (s < L->rtt_min) L->rtt_min = s;
    }
    L->rtt_n++;
}

static int tl_red(loctun *t, const tlink *L)
{
    int n;
    if (t->c.red_fixed > 0) return t->c.red_fixed;
    /* Kopien kosten bei den kleinen Lockstep-Rahmen kaum etwas (0x34: 26 B) und gehen nur hinaus, solange
     * die Nachricht unbestaetigt ist (LAN: Bestaetigung vor der ersten Kopie). */
    if (L->loss_peer < 5) n = 3;
    else if (L->loss_peer < 20) n = 4;
    else if (L->loss_peer < 50) n = 6;
    else n = 8;
    if (n > t->c.red_max) n = t->c.red_max;
    if (n < 1) n = 1;
    return n;
}

/* ====================================================================== Stroeme (Spiel-TCP) */
static void tl_sock_events(loctun *t, SOCKET s, long ev)
{
    WSAEventSelect(s, t->ev_net, ev);
}

static void tl_nodelay(SOCKET s)
{
    BOOL on = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof on);
}

static int tl_queue_msg(loctun *t, tlink *L, int sub, uint16_t stream, const uint8_t *data, int len)
{
    tmsg *m;
    if (L->snd_next - L->snd_una >= TUN_SQ) return 0;
    m = (tmsg *)malloc(sizeof *m + (size_t)(len > 0 ? len : 1));
    if (!m) return 0;
    memset(m, 0, sizeof *m);
    m->seq = L->snd_next;
    m->sub = (uint8_t)sub;
    m->stream = stream;
    m->len = (uint16_t)len;
    if (len > 0) memcpy(m->data, data, (size_t)len);
    L->sq[L->snd_next % TUN_SQ] = m;
    L->snd_next++;
    L->sq_bytes += len;
    InterlockedIncrement(&L->s_new);
    InterlockedIncrement(&t->st.msg_new);
    (void)t;
    return 1;
}

static tstream *tl_stream_new(loctun *t, int link, uint16_t id, SOCKET s)
{
    int i;
    for (i = 0; i < TUN_MAX_STREAMS; i++) {
        if (!t->streams[i].used) {
            tstream *st = &t->streams[i];
            lguard_tcp *g = (lguard_tcp *)malloc(sizeof *g); /* W334: jeder Strom zum Spiel durch den Rahmenfilter */
            if (!g) return NULL;
            lguard_tcp_init(g);
            memset(st, 0, sizeof *st);
            st->g = g;
            st->used = 1;
            st->link = link;
            st->id = id;
            st->s = s;
            InterlockedIncrement(&t->st.streams);
            return st;
        }
    }
    return NULL;
}

static tstream *tl_stream_find(loctun *t, int link, uint16_t id)
{
    int i;
    for (i = 0; i < TUN_MAX_STREAMS; i++) {
        if (t->streams[i].used && t->streams[i].link == link && t->streams[i].id == id) return &t->streams[i];
    }
    return NULL;
}

static void tl_stream_free(loctun *t, tstream *st)
{
    if (st->s != INVALID_SOCKET) {
        WSAEventSelect(st->s, NULL, 0);
        closesocket(st->s);
    }
    free(st->ob);
    if (st->g) {
        SecureZeroMemory(st->g, sizeof *st->g); /* unzugestellte Bytes des Mitspielers */
        free(st->g);
    }
    memset(st, 0, sizeof *st);
    st->s = INVALID_SOCKET;
    (void)t;
}

static void tl_stream_flush(loctun *t, tstream *st)
{
    while (st->olen > 0 && !st->connecting && st->s != INVALID_SOCKET) {
        int r = send(st->s, (const char *)st->ob, st->olen, 0);
        if (r > 0) {
            memmove(st->ob, st->ob + r, (size_t)(st->olen - r));
            st->olen -= r;
            continue;
        }
        if (WSAGetLastError() == WSAEWOULDBLOCK) return;
        {
            /* W409: Verbindung zum Spiel weg - einmal je Mitspieler sichtbar machen und dem Mitspieler das Ende melden
             * (vorher blieb sein Strom halb offen: kein S_FIN, Eintrag nie frei) */
            tlink *L = &t->links[st->link];
            int e = WSAGetLastError();
            if (!(L->bad_logged & 256)) {
                L->bad_logged |= 256;
                tl_logf(t, "LOCTUN: Strom %u (c%u): Senden an das eigene Spiel gescheitert (Fehler %d) - Strom beendet",
                        st->id, L->cid, e);
            }
            if (!st->fin_sent) {
                tl_queue_msg(t, L, S_FIN, st->id, NULL, 0);
                st->fin_sent = 1;
            }
            st->wr_shut = 1;
        }
        st->olen = 0; /* Verbindung zum Spiel weg */
        st->rd_closed = 1;
        break;
    }
    if (st->fin_rx && st->olen == 0 && !st->wr_shut && !st->connecting && st->s != INVALID_SOCKET) {
        shutdown(st->s, SD_SEND);
        st->wr_shut = 1;
    }
    if (st->wr_shut && st->rd_closed) {
        tl_stream_free(t, st);
    }
}

/* W329: Obergrenze des Puffers zum eigenen Spiel. Liest das Spiel nicht, darf ein Mitspieler den Speicher
 * nicht unbegrenzt fuellen (Spielrahmen sind <= 0x100C Byte; 1 MiB ist reichlich). 0 = voll/kein Speicher. */
#define TUN_OB_MAX (1024 * 1024)
static int tl_stream_append(tstream *st, const uint8_t *d, int len)
{
    if (len < 0 || st->olen + len > TUN_OB_MAX) return 0;
    if (st->olen + len > st->ocap) {
        int nc = st->ocap ? st->ocap * 2 : 8192;
        uint8_t *nb;
        while (nc < st->olen + len) nc *= 2;
        nb = (uint8_t *)realloc(st->ob, (size_t)nc);
        if (!nb) return 0;
        st->ob = nb;
        st->ocap = nc;
    }
    memcpy(st->ob + st->olen, d, (size_t)len);
    st->olen += len;
    return 1;
}

/* W334: Verbindung zu einem Mitspieler abbauen (Stroeme zu, Stellvertreter weg, Schluessel geloescht). Der Eintrag
 * bleibt als "gesperrt" stehen, bis ein neues peer_info (neues pair_nonce) kommt bzw. die Lobby ihn entfernt. */
static void tl_link_drop(loctun *t, tlink *L, const char *why)
{
    int i, li = (int)(L - t->links);
    uint32_t s;
    if (!L->used) return;
    if (L->e2e.valid && !L->blocked && L->path) {
        uint8_t bye[1];
        bye[0] = T_BYE;
        tl_send_link(t, L, L->path, bye, 1); /* BYE (authentisch), dann Schluessel weg */
    }
    for (i = 0; i < TUN_MAX_STREAMS; i++) {
        if (t->streams[i].used && t->streams[i].link == li) tl_stream_free(t, &t->streams[i]);
    }
    if (L->lsn != INVALID_SOCKET) {
        WSAEventSelect(L->lsn, NULL, 0);
        closesocket(L->lsn);
        L->lsn = INVALID_SOCKET;
    }
    for (i = 0; i < L->ndpx; i++) closesocket(L->dpx[i].s);
    L->ndpx = 0;
    for (s = 0; s < TUN_SQ; s++) {
        free(L->sq[s]);
        L->sq[s] = NULL;
    }
    for (s = 0; s < TUN_RQ; s++) {
        free(L->rq[s]);
        L->rq[s] = NULL;
    }
    sbe2e_link_free(&L->e2e);
    L->up = 0;
    L->path = 0;
    L->want = 0;
    L->punching = 0;
    L->direct_ok = 0;
    L->seen_ok = 0;
    memset(L->rb, 0, sizeof L->rb); /* W395 */
    L->rt_on = 0;
    locroute_init(&L->rt);
    L->blocked = 1;
    L->drop_pending = 0;
    tl_logf(t, "LOCTUN: Verbindung zu c%u beendet (%s)", L->cid, why);
}

/* W334: Gastfilter-Verstoss (SECURITY.md §6.1): einmal je Grund protokollieren (ohne Inhalte), trennen. */
static void tl_guard_kill(loctun *t, tlink *L, int why)
{
    InterlockedIncrement(&t->st.guard);
    if (!L->used || L->blocked) return;
    tl_logf(t, "LOCGUARD: c%u Verstoss %s - Verbindung getrennt", L->cid, lguard_reason(why));
    tl_link_drop(t, L, "Gastfilter");
}

/* W409 LOC-MESH-KICK: Strom vom Mitspieler aufgeben - S_FIN an ihn, Eintrag frei. */
static void tl_stream_give_up(loctun *t, tstream *st)
{
    tlink *L = &t->links[st->link];
    if (!st->fin_sent) {
        tl_queue_msg(t, L, S_FIN, st->id, NULL, 0);
        st->fin_sent = 1;
    }
    tl_stream_free(t, st);
}

/* W409: Verbindung zum eigenen Spiel (neu) aufbauen, Absender = vip des Mitspielers. 1 = laeuft bzw. steht,
 * 0 = abgelehnt (*err, z. B. WSAECONNREFUSED: Spiel lauscht nicht), -1 = kein Socket (*err). */
static int tl_stream_connect(loctun *t, tstream *st, int *err)
{
    tlink *L = &t->links[st->link];
    struct sockaddr_in a;
    char ipb[16];
    u_long nb = 1;
    SOCKET s;
    *err = 0;
    if (t->c.test_refuse_ms > 0 && tl_now(t) - st->t_open < (double)t->c.test_refuse_ms) {
        *err = WSAECONNREFUSED; /* W409 TEST: wie ein Spiel, das TCP 1000 (noch) nicht lauscht */
        return 0;
    }
    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        *err = WSAGetLastError();
        return -1;
    }
    tl_nodelay(s);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = L->vip;
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0 && !(L->bad_logged & 8192)) {
        L->bad_logged |= 8192; /* W409: einmal je Mitspieler (Text unveraendert) */
        tl_logf(t, "LOCTUN: Strom %u: bind %s fehlgeschlagen (%d)", st->id, locnet_ip_str(L->vip, ipb), WSAGetLastError());
    }
    ioctlsocket(s, FIONBIO, &nb);
    tl_sock_events(t, s, FD_READ | FD_WRITE | FD_CONNECT | FD_CLOSE);
    a.sin_addr.s_addr = t->n->self_ip;
    a.sin_port = htons(locnet_port_out(t->n, t->n->self_ip, 1000));
    st->s = s;
    st->connecting = 1;
    if (connect(s, (struct sockaddr *)&a, sizeof a) == 0) {
        st->connecting = 0;
    } else if (WSAGetLastError() != WSAEWOULDBLOCK) {
        *err = WSAGetLastError();
        if (!(L->bad_logged & 32)) {
            L->bad_logged |= 32; /* W409: einmal je Mitspieler (Text unveraendert) */
            tl_logf(t, "LOCTUN: Strom %u: connect an das eigene Spiel fehlgeschlagen (%d)", st->id, *err);
        }
        WSAEventSelect(s, NULL, 0);
        closesocket(s);
        st->s = INVALID_SOCKET;
        st->connecting = 0;
        return 0;
    }
    return 1;
}

/* W409: Zustand des eigenen Spiels fuer die Diagnosezeilen (keine Adressen): lauscht es auf TCP 1000, Lobby-Zustand
 * G+0x15 (vom Spiel-Thread alle 8 Bilder gemeldet, W373; "?" = unbekannt). */
static const char *tl_game_info(loctun *t, char *b, size_t cap)
{
    LONG w = t->n->pub_w[2];
    if (w & 0x1000000)
        _snprintf_s(b, cap, _TRUNCATE, "Spiel lauscht %s, Lobby-Zustand 0x%02X", t->n->tcp_listen ? "ja" : "nein",
                    (unsigned)(w & 0xFF));
    else
        _snprintf_s(b, cap, _TRUNCATE, "Spiel lauscht %s, Lobby-Zustand ?", t->n->tcp_listen ? "ja" : "nein");
    return b;
}

/* W409: Das eigene Spiel nahm den Strom des Mitspielers nicht an (connect abgelehnt: lauscht nicht auf TCP 1000).
 * Das Original wiederholt einen abgelehnten connect (FUN_0004FD80: select meldet Ausnahme -> Socket zu,
 * FUN_000562C0 verbindet neu); beim Mitspieler steht die Verbindung zum Stellvertreter aber schon, ein S_FIN wuerde
 * ihm als Verbindungsreset gemeldet (W350) und sein Spiel entfernt uns. Daher den Strom kurz halten und neu
 * verbinden (alle 200 ms, hoechstens open_hold_ms ab OPEN), danach S_FIN wie bisher. Puffer bleibt begrenzt
 * (TUN_OB_MAX, Gastfilter je Rahmen), Strom-Tabelle ebenso (TUN_MAX_STREAMS). */
static void tl_stream_refused(loctun *t, tstream *st, int err)
{
    tlink *L = &t->links[st->link];
    double now = tl_now(t), age = now - st->t_open;
    char gi[64];
    if (st->s != INVALID_SOCKET) {
        WSAEventSelect(st->s, NULL, 0);
        closesocket(st->s);
        st->s = INVALID_SOCKET;
    }
    st->connecting = 0;
    if (st->peer && !st->tx_any && !st->fin_rx && !L->blocked && L->up && age + 200.0 <= (double)t->c.open_hold_ms) {
        st->held = 1;
        st->tries++;
        st->retry_at = now + 200.0;
        if (!(L->bad_logged & 64)) {
            L->bad_logged |= 64;
            tl_logf(t, "LOCTUN: Strom %u von c%u: eigenes Spiel nimmt TCP 1000 nicht an (Fehler %d, %s) - Strom gehalten, "
                       "neuer Versuch bis %d ms",
                    st->id, L->cid, err, tl_game_info(t, gi, sizeof gi), t->c.open_hold_ms);
        }
        return;
    }
    if (!(L->bad_logged & 4096)) {
        L->bad_logged |= 4096;
        tl_logf(t, "LOCTUN: Strom %u von c%u: eigenes Spiel nahm TCP 1000 nicht an (Fehler %d, %d Versuche, %.0f ms, %s) - "
                   "Strom geschlossen (S_FIN)",
                st->id, L->cid, err, st->tries + 1, age, tl_game_info(t, gi, sizeof gi));
    }
    tl_stream_give_up(t, st);
}

/* OPEN vom Mitspieler: Verbindung zum eigenen Spiel aufbauen, Absender = vip des Mitspielers. */
static void tl_stream_open_remote(loctun *t, int li, uint16_t id, uint16_t dst_gp)
{
    tlink *L = &t->links[li];
    tstream *st;
    int r, err;
    if (tl_stream_find(t, li, id)) return;
    if (dst_gp != 1000) {
        /* W329: Der Mitspieler bestimmt den Zielport. Das Spiel nutzt nur TCP 1000 (tl_accept); jeder andere Port
         * wuerde eine TCP-Verbindung zu beliebigen lokalen Diensten (0.0.0.0 ist ueber die 127.x-Adresse
         * erreichbar) tunneln. Ablehnen und den Strom beim Mitspieler schliessen. */
        InterlockedIncrement(&t->st.bad);
        if (!(L->bad_logged & 1)) {
            L->bad_logged |= 1;
            tl_logf(t, "LOCTUN: Strom %u von c%u an Port %u abgelehnt (nur Spiel-TCP 1000)", id, L->cid, dst_gp);
        }
        tl_queue_msg(t, L, S_FIN, id, NULL, 0);
        return;
    }
    st = tl_stream_new(t, li, id, INVALID_SOCKET);
    if (!st) {
        /* W409: vorher still verworfen (der Strom des Mitspielers blieb offen) - jetzt S_FIN und einmal sichtbar */
        InterlockedIncrement(&t->st.bad);
        if (!(L->bad_logged & 16)) {
            L->bad_logged |= 16;
            tl_logf(t, "LOCTUN: Strom %u von c%u abgelehnt (Strom-Tabelle voll, %d) - S_FIN", id, L->cid, TUN_MAX_STREAMS);
        }
        tl_queue_msg(t, L, S_FIN, id, NULL, 0);
        return;
    }
    st->peer = 1;
    st->t_open = tl_now(t);
    tl_logf(t, "LOCTUN: Strom %u von c%u geoeffnet (Spielport %u)", id, L->cid, dst_gp);
    r = tl_stream_connect(t, st, &err);
    if (r < 0) {
        if (!(L->bad_logged & 16)) {
            L->bad_logged |= 16;
            tl_logf(t, "LOCTUN: Strom %u von c%u abgelehnt (kein Socket, Fehler %d) - S_FIN", id, L->cid, err);
        }
        tl_stream_give_up(t, st);
    } else if (r == 0) {
        tl_stream_refused(t, st, err);
    }
}

static void tl_deliver_msg(loctun *t, int li, tmsg *m)
{
    tstream *st;
    if (t->links[li].blocked) return;
    if (m->sub == S_OPEN) {
        tl_stream_open_remote(t, li, m->stream, m->len >= 2 ? get16(m->data) : 1000);
        return;
    }
    st = tl_stream_find(t, li, m->stream);
    if (!st) {
        /* W409: Nachricht fuer einen Strom, den es hier nicht (mehr) gibt - einmal je Mitspieler sichtbar */
        tlink *L = &t->links[li];
        if (!(L->bad_logged & 1024)) {
            L->bad_logged |= 1024;
            tl_logf(t, "LOCTUN: Strom %u (c%u): %s fuer unbekannten Strom verworfen", m->stream, L->cid,
                    m->sub == S_FIN ? "Ende" : "Daten");
        }
        return;
    }
    if (m->sub == S_DATA) {
        st->rx_any = 1;
        /* W334: Gastfilter TCP (SECURITY.md §6.2) - nur ganze, gueltige Spielrahmen; Verstoss = Mitspieler trennen */
        static uint8_t gout[LGUARD_TCP_MAX + TUN_MSG];
        int gn = st->g ? lguard_tcp_feed(st->g, m->data, m->len, gout, (int)sizeof gout) : -1;
        if (gn < 0) {
            tl_guard_kill(t, &t->links[li], st->g ? st->g->bad : LGUARD_V_STATE);
            return;
        }
        if (gn > 0 && !tl_stream_append(st, gout, gn)) {
            tlink *L = &t->links[li];
            InterlockedIncrement(&t->st.bad);
            if (!(L->bad_logged & 4)) {
                L->bad_logged |= 4;
                tl_logf(t, "LOCTUN: Strom %u von c%u: Puffer zum Spiel voll (%d Byte) - Strom geschlossen", m->stream,
                        L->cid, st->olen);
            }
            if (!st->fin_sent) tl_queue_msg(t, L, S_FIN, st->id, NULL, 0);
            tl_stream_free(t, st);
            return;
        }
    } else if (m->sub == S_FIN) {
        st->fin_rx = 1;
        if (!st->peer && !st->rx_any) {
            /* W409: eigener Strom (Spiel verband sich zum Mitspieler), Gegenseite schloss ihn vor jedem Byte - das
             * Spiel sieht gleich einen Verbindungsreset (W350). Grund steht im Log der Gegenseite (LOCTUN: Strom ...). */
            tlink *L = &t->links[li];
            if (!(L->bad_logged & 2048)) {
                L->bad_logged |= 2048;
                tl_logf(t, "LOCTUN: Strom %u zu c%u: Gegenseite schloss ihn sofort (nach %.0f ms, vor Daten)", st->id,
                        L->cid, tl_now(t) - st->t_open);
            }
        }
    }
    tl_stream_flush(t, st);
}

/* ====================================================================== Datagramme (Spiel-UDP) */
static SOCKET tl_dpx(loctun *t, tlink *L, uint16_t src_gp)
{
    struct sockaddr_in a;
    int i, l = sizeof a;
    BOOL off = FALSE;
    DWORD got = 0;
    SOCKET s;
    for (i = 0; i < L->ndpx; i++) {
        if (L->dpx[i].gp == src_gp) return L->dpx[i].s;
    }
    if (L->ndpx >= 8) return INVALID_SOCKET;
    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return s;
    WSAIoctl(s, 0x9800000Cu, &off, sizeof off, NULL, 0, &got, NULL, NULL);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = L->vip;
    a.sin_port = (src_gp >= 1000 && src_gp <= 1999) ? htons((uint16_t)(t->c.proxy_base + (src_gp - 1000))) : 0;
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) {
        char ipb[16];
        tl_logf(t, "LOCTUN: Stellvertreter-UDP %s:%u belegt (%d)", locnet_ip_str(L->vip, ipb), ntohs(a.sin_port),
                WSAGetLastError());
        closesocket(s);
        return INVALID_SOCKET;
    }
    getsockname(s, (struct sockaddr *)&a, &l);
    L->dpx[L->ndpx].gp = src_gp;
    L->dpx[L->ndpx].local = ntohs(a.sin_port);
    L->dpx[L->ndpx].s = s;
    L->ndpx++;
    return s;
}

static int tl_seen_dg(tlink *L, uint16_t id)
{
    int i;
    for (i = 0; i < L->nseen; i++) {
        if (L->seen[i] == id) return 1;
    }
    L->seen[L->seen_pos] = id;
    L->seen_pos = (L->seen_pos + 1) % TUN_SEEN;
    if (L->nseen < TUN_SEEN) L->nseen++;
    return 0;
}

static void tl_deliver_dg(loctun *t, tlink *L, uint16_t src, uint16_t dst, const uint8_t *d, int len)
{
    struct sockaddr_in a;
    uint16_t hp = (dst >= 1000 && dst <= 1999) ? locnet_port_out(t->n, t->n->self_ip, dst) : dst;
    SOCKET s;
    if (L->blocked) return;
    if (t->app_fn && t->app_gp && dst == t->app_gp && src == t->app_gp) { /* W338: R2-Kanal, nie an den Gast */
        loctun_app_fn fn = t->app_fn;
        if (!lguard_bucket_take(&L->b_app, tl_now(t), TUN_DG_RATE, 2.0 * TUN_DG_RATE)) {
            InterlockedIncrement(&t->st.dg_drop);
            return;
        }
        if (fn) fn(L->vip, d, len);
        return;
    }
    if (t->app2_fn && t->app2_gp && dst == t->app2_gp && src == t->app2_gp) { /* W420: Regelkanal, nie an den Gast */
        loctun_app_fn fn = t->app2_fn;
        if (!lguard_bucket_take(&L->b_app2, tl_now(t), 20.0, 40.0)) { /* 5/s erwartet */
            InterlockedIncrement(&t->st.dg_drop);
            return;
        }
        if (fn) fn(L->vip, d, len);
        return;
    }
    /* W329: Zielport bestimmt der Mitspieler. Nur Ports, die das eigene Spiel gebunden hat - sonst koennte er
     * beliebige lokale UDP-Dienste (alles auf 0.0.0.0 ist auch ueber die 127.x-Adresse erreichbar) ansprechen. */
    if (hp == 0 || !locnet_udp_bound(t->n, hp)) {
        InterlockedIncrement(&t->st.bad);
        return;
    }
    /* W334: Ratenbremse je Mitspieler und Port (60/s, Burst 120, darueber verwerfen - kein Verstoss, §6.4), dann
     * UDP-Positivliste (§6.3): Verstoss = Mitspieler trennen. */
    if (!lguard_bucket_take(&L->b_dg[dst == 1201 ? 0 : 1], tl_now(t), TUN_DG_RATE, 2.0 * TUN_DG_RATE)) {
        InterlockedIncrement(&t->st.dg_drop);
        return;
    }
    {
        int why = 0;
        if (!lguard_udp_check(dst, d, len, &why)) {
            tl_guard_kill(t, L, why);
            return;
        }
    }
    s = tl_dpx(t, L, src);
    if (s == INVALID_SOCKET) return;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = t->n->self_ip;
    a.sin_port = htons(hp);
    sendto(s, (const char *)d, len, 0, (struct sockaddr *)&a, sizeof a);
}

/* ====================================================================== Pakete bauen/senden */
static int tl_build_send(loctun *t, tlink *L, double now, int force)
{
    uint8_t p[TUN_PT];
    int off = DHDR, nitems = 0, i, red = L->red, path = L->path;
    double rto = tl_rto(L), gap = (double)t->c.gap_ms;
    uint32_t s;
    if (!L->up || path == 0) return 0;
    /* Strom-Nachrichten, aelteste zuerst */
    for (s = L->snd_una; s != L->snd_next; s++) {
        tmsg *m = L->sq[s % TUN_SQ];
        int need;
        if (!m) continue;
        if (!(m->tx == 0 || (m->tx < red && now - m->last >= gap) || now - m->last >= rto)) continue;
        need = 11 + m->len;
        if (off + need > TUN_PT) {
            if (nitems == 0) continue;
            break;
        }
        p[off] = K_STREAM;
        put32(p + off + 1, m->seq);
        p[off + 5] = m->sub;
        put16(p + off + 6, m->stream);
        p[off + 8] = m->tx > 254 ? 254 : m->tx;
        put16(p + off + 9, m->len);
        memcpy(p + off + 11, m->data, m->len);
        off += need;
        nitems++;
        if (m->tx == 0) {
        } else if (m->tx < red && now - m->last < rto) {
            InterlockedIncrement(&L->s_copy);
            InterlockedIncrement(&t->st.msg_copy);
        } else {
            InterlockedIncrement(&L->s_rto);
            InterlockedIncrement(&t->st.msg_rto);
        }
        if (m->tx < 255) m->tx++;
        m->last = now;
    }
    /* Datagramme */
    for (i = 0; i < TUN_DQ; i++) {
        tdg *d = &L->dq[i];
        int need;
        if (!d->used) continue;
        if (!(d->tx == 0 || now - d->last >= gap)) continue;
        need = 10 + d->len;
        if (off + need > TUN_PT) continue;
        p[off] = K_DGRAM;
        put16(p + off + 1, d->id);
        put16(p + off + 3, d->src);
        put16(p + off + 5, d->dst);
        p[off + 7] = d->tx;
        put16(p + off + 8, d->len);
        memcpy(p + off + 10, d->data, d->len);
        off += need;
        nitems++;
        d->tx++;
        d->last = now;
        if (d->tx >= (red > 2 ? red : 2)) d->used = 0;
        if (nitems >= 250) break;
    }
    if (nitems == 0 && !force && !(L->ack_pending && now >= L->ack_due)) return 0;
    tl_hdr(p, T_DATA);
    put32(p + HDR, ++L->pkt_seq);
    put32(p + HDR + 4, L->rcv_next);
    put32(p + HDR + 8, (uint32_t)(now < 1 ? 1 : now));
    put32(p + HDR + 12, L->echo_ts);
    put16(p + HDR + 16, (uint16_t)(L->echo_ts ? (now - L->echo_at > 60000 ? 60000 : now - L->echo_at) : 0));
    put16(p + HDR + 18, (uint16_t)L->loss_in);
    p[HDR + 20] = (uint8_t)nitems;
    tl_send_link(t, L, path, p, off);
    L->ack_pending = 0;
    return nitems > 0 ? 2 : 1;
}

/* W420: Version nur aus [0-9A-Za-z.+-] (hoechstens 15 Zeichen), sonst leer */
static void tl_ver_clean(const char *in, int n, char *out, int cap)
{
    int i, o = 0;
    out[0] = 0;
    for (i = 0; i < n && in[i]; i++) {
        char c = in[i];
        if (!(isalnum((unsigned char)c) || c == '.' || c == '+' || c == '-') || o + 1 >= cap || o >= 15) {
            out[0] = 0;
            return;
        }
        out[o++] = c;
    }
    out[o] = 0;
}
/* W420: numerischer Vergleich "0.3.10" > "0.3.2" (Nicht-Ziffern beenden den Teil); > 0 = a neuer.
 * W421: gemeinsame Hilfsfunktion mit dem Launcher (sb_version.h, LoC-Startsperre aus /v1/info) */
static int tl_ver_cmp(const char *a, const char *b)
{
    return sb_netver_cmp(a, b);
}
/* W420: eigene Version (TEST: ver_test) und ob die Pruefung gilt (steelnet bzw. TEST ver_check) */
static const char *tl_myver(const loctun *t) { return t->c.ver_test[0] ? t->c.ver_test : SB_NET_VERSION; }
static int tl_ver_required(const loctun *t) { return t->c.lobby[0] || t->c.ver_check; }
static const char *tl_sec_name(const loctun *t)
{
    return t->sec == SEC_OK ? "ok" : (t->sec == SEC_PIN ? "pin_failed" : (t->sec == SEC_NOPIN ? "no_pin" :
           (t->sec == SEC_UPGRADE ? "upgrade_required" : (t->sec == SEC_VERSION ? "version_required" :
           "selftest_failed"))));
}

static void tl_hello_fill(loctun *t, tlink *L, uint8_t *p)
{
    tl_hdr(p, T_HELLO);
    put32(p + HDR, t->n->self_ip);
    p[HDR + 4] = (uint8_t)L->got_hello;
    p[HDR + 5] = 2; /* Version (W334: SBT2) */
    memset(p + HDR + 8, 0, HELLO_VLEN);
    if (strcmp(tl_myver(t), "-") == 0) { /* TEST: wie ein aelterer Client (Feld fehlt, Flag aus) */
        put16(p + HDR + 6, (uint16_t)(t->c.route_auto ? HELLO_F_ROUTE : 0));
    } else {
        size_t vl = strlen(tl_myver(t));
        put16(p + HDR + 6, (uint16_t)((t->c.route_auto ? HELLO_F_ROUTE : 0) | HELLO_F_VER)); /* W395: Flags, W420 */
        memcpy(p + HDR + 8, tl_myver(t), vl < HELLO_VLEN ? vl : HELLO_VLEN - 1);
    }
}

static void tl_hello(loctun *t, tlink *L, double now)
{
    uint8_t p[HELLO_LEN];
    tl_hello_fill(t, L, p);
    tl_send_link(t, L, L->path, p, sizeof p);
    L->hello_last = now;
}

/* HELLO auf einem bestimmten Weg: slot >= 1 = Relay slot-1 (W395), sonst direkt an "to". */
static void tl_hello_on(loctun *t, tlink *L, int slot, const struct sockaddr_in *to, double now)
{
    uint8_t p[HELLO_LEN];
    tl_hello_fill(t, L, p);
    if (slot >= 1) {
        tl_send_slot(t, L, slot, p, sizeof p);
    } else {
        tl_send_to(t, L, to, p, sizeof p);
    }
    L->hello_last = now;
}

/* ====================================================================== Empfang */
static void tl_link_up(loctun *t, tlink *L)
{
    struct sockaddr_in a;
    char ipb[16], ipc[16];
    u_long nb = 1;
    if (L->up || L->ver_bad) return; /* W420 */
    if (L->vip == t->n->self_ip) {
        if (!(L->bad_logged & 8)) { /* W332: je HELLO eine Zeile = Mitspieler fuellt das Log unbegrenzt */
            L->bad_logged |= 8;
            tl_logf(t, "LOCTUN: c%u hat dieselbe virtuelle Adresse %s - Verbindung abgelehnt (loc_net_addr=random)",
                    L->cid, locnet_ip_str(L->vip, ipb));
        }
        return;
    }
    L->up = 1;
    L->last_rx = tl_now(t);
    if (t->pub) pub_on_link_up(t, L); /* W373: Geister-Sockets vor den Stellvertretern schliessen */
    /* Stellvertreter fuer TCP 1000 */
    L->lsn = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (L->lsn != INVALID_SOCKET) {
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = L->vip;
        a.sin_port = htons(t->c.proxy_base);
        if (bind(L->lsn, (struct sockaddr *)&a, sizeof a) != 0 || listen(L->lsn, 8) != 0) {
            tl_logf(t, "LOCTUN: Stellvertreter %s:%u nicht moeglich (%d)", locnet_ip_str(L->vip, ipb), t->c.proxy_base,
                    WSAGetLastError());
            closesocket(L->lsn);
            L->lsn = INVALID_SOCKET;
        } else {
            ioctlsocket(L->lsn, FIONBIO, &nb);
            tl_sock_events(t, L->lsn, FD_ACCEPT);
        }
    }
    locnet_peer_add(t->n, L->vip, t->c.proxy_base);
    tl_logf(t, "LOCTUN: Verbindung zu c%u steht: virtuelle Adresse %s (eigene %s), Weg %s, RTT %.1f ms", L->cid,
            locnet_ip_str(L->vip, ipb), locnet_ip_str(t->n->self_ip, ipc),
            L->path == 1 ? "direkt" : (L->path == 2 ? "Relay" : "keiner"),
            L->rtt_n ? L->srtt : L->direct_rtt);
}

static void tl_rx_data(loctun *t, int li, const uint8_t *p, int len, double now)
{
    tlink *L = &t->links[li];
    uint32_t pseq, ack, ts, echo;
    int hold, nitems, off = DHDR, i, newdata = 0, fresh;
    if (len < DHDR) return;
    pseq = get32(p + HDR);
    ack = get32(p + HDR + 4);
    ts = get32(p + HDR + 8);
    echo = get32(p + HDR + 12);
    hold = get16(p + HDR + 16);
    L->loss_peer = get16(p + HDR + 18);
    nitems = p[HDR + 20];
    L->peer_got = 1;
    /* Verlust (Eingang) */
    L->rx_cnt++;
    fresh = (int32_t)(pseq - L->rx_hi) > 0 || L->rx_hi == 0; /* W395: Kopie ueber den zweiten Weg bzw. Nachzuegler */
    if ((int32_t)(pseq - L->rx_hi) > 0) L->rx_hi = pseq;
    if (now - L->loss_t >= 1000.0) {
        uint32_t exp = L->rx_hi - L->rx_hi0, got = L->rx_cnt - L->rx_cnt0;
        if (L->loss_t > 0 && exp > 0) {
            int pm = got >= exp ? 0 : (int)((exp - got) * 1000 / exp);
            L->loss_in = (L->loss_in * 7 + pm * 3 + 5) / 10;
            if (pm > L->loss_in) L->loss_in = pm; /* schnell hoch, langsam runter */
        }
        L->rx_hi0 = L->rx_hi;
        L->rx_cnt0 = L->rx_cnt;
        L->loss_t = now;
    }
    /* RTT (W395: nur aus dem neuesten Paket - eine spaetere Kopie desselben Pakets verfaelschte die Messung) */
    if (ts && fresh) {
        L->echo_ts = ts;
        L->echo_at = now;
    }
    if (echo && fresh) {
        tl_rtt_sample(L, now - (double)echo - (double)hold);
    }
    /* Bestaetigung */
    while ((int32_t)(ack - L->snd_una) > 0 && L->snd_una != L->snd_next) {
        tmsg *m = L->sq[L->snd_una % TUN_SQ];
        if (m) {
            L->sq_bytes -= m->len;
            free(m);
            L->sq[L->snd_una % TUN_SQ] = NULL;
        }
        L->snd_una++;
    }
    L->red = tl_red(t, L);
    /* Eintraege */
    for (i = 0; i < nitems; i++) {
        if (off + 1 > len) break;
        if (p[off] == K_STREAM) {
            uint32_t seq;
            uint16_t ml;
            uint8_t cp;
            if (off + 11 > len) break;
            seq = get32(p + off + 1);
            cp = p[off + 8];
            ml = get16(p + off + 9);
            if (off + 11 + ml > len || ml > TUN_MSG) break;
            newdata = 1;
            if ((int32_t)(seq - L->rcv_next) < 0 || (int32_t)(seq - L->rcv_next) >= TUN_RQ ||
                L->rq[seq % TUN_RQ]) {
                InterlockedIncrement(&L->s_dup);
                InterlockedIncrement(&t->st.msg_dup);
            } else {
                tmsg *m = (tmsg *)malloc(sizeof *m + (ml ? ml : 1));
                if (m) {
                    memset(m, 0, sizeof *m);
                    m->seq = seq;
                    m->sub = p[off + 5];
                    m->stream = get16(p + off + 6);
                    m->len = ml;
                    m->arr = now;
                    memcpy(m->data, p + off + 11, ml);
                    L->rq[seq % TUN_RQ] = m;
                    InterlockedIncrement(&t->st.msg_rx);
                    if (cp > 0) {
                        InterlockedIncrement(&L->s_saved);
                        InterlockedIncrement(&t->st.msg_saved);
                    }
                }
            }
            off += 11 + ml;
        } else if (p[off] == K_DGRAM) {
            uint16_t id, src, dst, dl;
            if (off + 10 > len) break;
            id = get16(p + off + 1);
            src = get16(p + off + 3);
            dst = get16(p + off + 5);
            dl = get16(p + off + 8);
            if (off + 10 + dl > len) break;
            newdata = 1;
            if (tl_seen_dg(L, id)) {
                InterlockedIncrement(&t->st.dg_dup);
            } else {
                InterlockedIncrement(&L->s_dgrx);
                InterlockedIncrement(&t->st.dg_rx);
                tl_deliver_dg(t, L, src, dst, p + off + 10, dl);
            }
            off += 10 + dl;
        } else {
            break;
        }
    }
    /* geordnet zustellen (mit optionaler Haltezeit = Jitter-Puffer) */
    for (;;) {
        tmsg *m = L->rq[L->rcv_next % TUN_RQ];
        if (!m || m->seq != L->rcv_next) break;
        if (t->c.hold_ms > 0 && now - m->arr < (double)t->c.hold_ms) break;
        L->rq[L->rcv_next % TUN_RQ] = NULL;
        L->rcv_next++;
        tl_deliver_msg(t, li, m);
        free(m);
    }
    if (newdata && !L->ack_pending) {
        L->ack_pending = 1;
        L->ack_due = now + 2.0;
    }
}

/* W329: virtuelle Adresse aus dem HELLO des Mitspielers. Auf ihr bindet der Stellvertreter (TCP proxy_base,
 * UDP) - sie muss daher eine eigene Loopback-Adresse sein: 127/8 ausser 127.0.0.1 und .0/.255,
 * nicht die eines anderen Mitspielers (sonst Verkehr fuer ihn umgeleitet), nach dem Verbindungsaufbau fest.
 * Ohne Pruefung: 0.0.0.0 bzw. die eigene LAN-Adresse = Stellvertreter-Port offen fuers Netz. */
static int tl_vip_ok(loctun *t, const tlink *L, uint32_t vip)
{
    uint32_t h = ntohl(vip);
    int i;
    if ((h >> 24) != 127u || h == 0x7F000001u || (h & 0xFFu) == 0u || (h & 0xFFu) == 255u) return 0;
    /* vip == eigene Adresse: tl_link_up lehnt mit eigener Meldung ab (loc_net_addr=random) */
    if (L->up && vip != L->vip) return 0;
    for (i = 0; i < TUN_MAX_LINKS; i++) {
        const tlink *o = &t->links[i];
        if (o != L && o->used && o->vip == vip) return 0;
    }
    return 1;
}

/* W334: Empfang eines SBT2-Pakets (SECURITY.md §4.5, Reihenfolge genau so):
 * 1. Form (Laenge, Magic, sid bekannt, ctr-Bereich) ohne Krypto, 2. Ratenbremse vor der Krypto, 3.-5. Fenster
 * pruefen, GCM oeffnen, erst dann Fenster setzen (sbe2e_open), 6. authentische Pakete von jeder Adresse - der
 * Sendeweg wechselt nur per PROBE/PROBE_ACK oder route, 7. Transportlogik, Gastfilter vor dem Spiel. */
/* via_relay_link/via_ri: Verbindung und Relay einer Relay-Nutzlast (-1 = direkt empfangen). */
static void tl_rx_sbt(loctun *t, const uint8_t *pkt, int plen, const struct sockaddr_in *from, int via_relay_link,
                      int via_ri, double now)
{
    uint8_t p[TUN_PKT];
    uint32_t sid;
    int li, len, slot;
    tlink *L = NULL;
    if (!sbe2e_peek_sid(pkt, plen, &sid)) {
        InterlockedIncrement(&t->st.e2e_bad);
        return;
    }
    for (li = 0; li < TUN_MAX_LINKS; li++) {
        tlink *c = &t->links[li];
        if (!c->used || c->blocked || !c->e2e.valid || c->e2e.sid != sid) continue;
        if (via_relay_link >= 0 && li != via_relay_link) continue;
        L = c;
        break;
    }
    if (!L) {
        InterlockedIncrement(&t->st.e2e_bad); /* unbekannte sid: verwerfen ohne Rechenaufwand */
        return;
    }
    if (L->cid == 0 && !locnet_ip_allowed(from->sin_addr.s_addr)) {
        /* W335 (NET-AUDIT N1): Testverbindung (loc_transport_peers, Schluessel oeffentlich bekannt) nur von
         * Loopback/privat - sonst koennte jeder, der den UDP-Port erreicht, als "Mitspieler" Rahmen einspeisen */
        InterlockedIncrement(&t->st.e2e_bad);
        return;
    }
    if (!lguard_bucket_take(&L->b_dec, now, TUN_DEC_RATE, TUN_DEC_RATE)) {
        InterlockedIncrement(&t->st.e2e_bad);
        return;
    }
    len = sbe2e_open(&L->e2e, pkt, plen, p, sizeof p);
    if (len < 1) {
        InterlockedIncrement(&t->st.e2e_bad);
        return;
    }
    L->last_rx = now;
    InterlockedIncrement(&L->s_rx);
    InterlockedIncrement(&t->st.pkt_rx);
    /* W395: Ankunftsweg (Relay i -> 1+i; direkt nur von einer per Durchstich bestaetigten Adresse) */
    if (via_relay_link >= 0) slot = via_ri >= 0 ? 1 + via_ri : -1;
    else slot = ((L->direct_ok && sa_eq(&L->direct, from)) || (L->seen_ok && sa_eq(&L->seen_from, from))) ? 0 : -1;
    switch (p[0]) {
    case T_HELLO:
        if (len >= HDR + 6) {
            uint32_t vip = get32(p + HDR);
            if (!tl_vip_ok(t, L, vip)) {
                char ipb[16];
                InterlockedIncrement(&t->st.bad);
                if (!(L->bad_logged & 2)) {
                    L->bad_logged |= 2;
                    tl_logf(t, "LOCTUN: c%u meldet unzulaessige virtuelle Adresse %s - ignoriert", L->cid,
                            locnet_ip_str(vip, ipb));
                }
                break;
            }
            if (tl_ver_required(t)) { /* W420: steelnet nur mit gleicher Client-Version (fehlt = aelterer Client) */
                char pv[16] = "";
                int has = len >= HELLO_LEN && (get16(p + HDR + 6) & HELLO_F_VER);
                if (has) tl_ver_clean((const char *)p + HDR + 8, HELLO_VLEN, pv, (int)sizeof pv);
                if (!has || strcmp(pv, tl_myver(t)) != 0) {
                    if (!L->ver_bad) {
                        L->ver_bad = 1;
                        strcpy_s(t->peer_ver, sizeof t->peer_ver, pv);
                        strcpy_s(t->need_ver, sizeof t->need_ver,
                                 (pv[0] && tl_ver_cmp(pv, tl_myver(t)) > 0) ? pv : tl_myver(t));
                        InterlockedExchange(&t->ver_state, 2);
                        tl_logf(t, "LOCVER: c%u nutzt Client-Version %s, eigene %s - Verbindung abgelehnt (steelnet nur mit "
                                "gleicher Version)", L->cid, pv[0] ? pv : "(fehlt, aelterer Client)", tl_myver(t));
                        if (slot >= 0) tl_hello_on(t, L, slot, from, now); /* damit ein neuer Client es auch sieht */
                        tl_link_drop(t, L, "andere Client-Version");
                    }
                    break;
                }
            }
            L->vip = vip;
            if (p[HDR + 4]) L->peer_got = 1;
            if (len >= HDR + 8 && (get16(p + HDR + 6) & HELLO_F_ROUTE)) L->peer_route = 1; /* W395 */
            if (!L->got_hello) {
                L->got_hello = 1;
                /* Antwort auf dem Ankunftsweg - direkt nur an eine per Durchstich bestaetigte Adresse */
                if (slot >= 0) tl_hello_on(t, L, slot, from, now);
            }
            /* "steht" erst mit einem Weg zum Partner (Durchstich bestaetigt bzw. Relay); sonst holt der Zeitgeber
             * das nach, sobald der Weg da ist */
            if (L->path || via_relay_link >= 0) tl_link_up(t, L);
        }
        break;
    case T_DATA:
        if (L->up) tl_rx_data(t, li, p, len, now);
        break;
    case T_PING:
        if (len == HDR + RP_LEN && p[HDR] == 'R' && p[HDR + 1] == 'T') {
            /* W395: Wegprobe - gleich gross auf dem Ankunftsweg zurueck (keine Verstaerkung, nur authentisch) */
            uint8_t q[HDR + RP_LEN];
            if (slot < 0 || !lguard_bucket_take(&L->b_rp, now, TUN_RP_RATE, TUN_RP_RATE)) break;
            tl_hdr(q, T_PONG);
            memcpy(q + HDR, p + HDR, RP_LEN);
            if (slot >= 1) tl_send_slot(t, L, slot, q, sizeof q);
            else tl_send_to(t, L, from, q, sizeof q);
            break;
        }
        if (len >= HDR + 4) {
            uint8_t q[HDR + 4];
            tl_hdr(q, T_PONG);
            memcpy(q + HDR, p + HDR, 4);
            if (slot >= 1) {
                tl_send_slot(t, L, slot, q, sizeof q);
            } else if (slot == 0) {
                tl_send_to(t, L, from, q, sizeof q);
            }
        }
        break;
    case T_PONG:
        if (len == HDR + RP_LEN && p[HDR] == 'R' && p[HDR + 1] == 'T') {
            /* W395: nur auf dem Weg, ueber den die Probe ging (Slot im Echo = Ankunftsweg) */
            if (slot >= 0 && p[HDR + 2] == (uint8_t)slot && p[HDR + 3] == 0 &&
                locroute_probe_ack(&L->rt, slot, get16(p + HDR + 4), now)) {
                L->rt_last_ok[slot] = now;
            }
            break;
        }
        if (len >= HDR + 4) {
            uint32_t k = get32(p + HDR);
            if (k < 5 && L->dping_at[k] > 0 && slot == 0) {
                double smp = now - L->dping_at[k];
                if (L->ndrtt < 5) L->drtt[L->ndrtt++] = smp;
                L->dping_at[k] = 0;
                tl_rtt_sample(L, smp);
            }
        }
        break;
    case T_PROBE: /* Durchstich: authentisch -> Weg vom Partner zu uns offen; ACK gleich gross an die Absenderadresse */
        if (len == HDR + 8 && via_relay_link < 0 && !t->c.nodirect && tl_dest_ok(t, from->sin_addr.s_addr) &&
            lguard_bucket_take(&L->b_ack, now, TUN_ACK_RATE, TUN_ACK_RATE)) {
            uint8_t q[HDR + 8];
            L->seen_from = *from;
            L->seen_ok = 1;
            tl_hdr(q, T_PROBE_ACK);
            memcpy(q + HDR, p + HDR, 8);
            tl_send_to(t, L, from, q, sizeof q);
        }
        break;
    case T_PROBE_ACK: /* nur mit der offenen probe_id: direkter Weg zu dieser Adresse bestaetigt */
        if (len == HDR + 8 && via_relay_link < 0 && !t->c.nodirect && L->probe_id != 0 && !L->direct_ok &&
            memcmp(p + HDR, &L->probe_id, 8) == 0 && tl_dest_ok(t, from->sin_addr.s_addr)) {
            char ipb[16];
            L->direct = *from;
            L->direct_ok = 1;
            L->direct_failed = 0;
            L->punching = 0;
            /* W339 (W335 I3): oeffentliche Adresse des Mitspielers nie ins Log (Logs gehen in Fehlerberichte);
             * Loopback/privat/link-local bleiben lesbar (Fehlersuche im LAN). Zeilenanfang unveraendert. */
            tl_logf(t, "LOCTUN: direkter Weg zu c%u offen (%s:%u)", L->cid,
                    locnet_ip_allowed(from->sin_addr.s_addr) ? locnet_ip_str(from->sin_addr.s_addr, ipb) : "oeffentlich",
                    ntohs(from->sin_port));
        }
        break;
    case T_BYE:
        if (L->up || L->path) tl_link_drop(t, L, "Partner hat beendet");
        break;
    default:
        break;
    }
    SecureZeroMemory(p, (size_t)len);
}

/* ====================================================================== Treffpunkt (steelnet) */
/* --- Mini-JSON (nur fuer die Antworten unseres eigenen Dienstes, mit Grenzpruefung) --- */
static const char *js_ws(const char *p, const char *e)
{
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    return p;
}

static const char *js_skip(const char *p, const char *e, int depth)
{
    p = js_ws(p, e);
    if (p >= e || depth > 16) return NULL;
    if (*p == '"') {
        for (p++; p < e; p++) {
            if (*p == '\\') { p++; continue; }
            if (*p == '"') return p + 1;
        }
        return NULL;
    }
    if (*p == '{' || *p == '[') {
        char close = *p == '{' ? '}' : ']';
        p = js_ws(p + 1, e);
        if (p < e && *p == close) return p + 1;
        for (;;) {
            if (close == '}') {
                p = js_skip(p, e, depth + 1); /* Schluessel */
                if (!p) return NULL;
                p = js_ws(p, e);
                if (p >= e || *p != ':') return NULL;
                p++;
            }
            p = js_skip(p, e, depth + 1);
            if (!p) return NULL;
            p = js_ws(p, e);
            if (p >= e) return NULL;
            if (*p == ',') { p = js_ws(p + 1, e); continue; }
            if (*p == close) return p + 1;
            return NULL;
        }
    }
    while (p < e && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\r' && *p != '\n') p++;
    return p;
}

/* Wert zu key im Objekt ab obj ('{'); *ve = Ende des Wertes. */
static const char *js_get(const char *obj, const char *e, const char *key, const char **ve)
{
    const char *p = js_ws(obj, e);
    size_t kl = strlen(key);
    if (p >= e || *p != '{') return NULL;
    p = js_ws(p + 1, e);
    while (p < e && *p == '"') {
        const char *ks = p + 1, *kend = js_skip(p, e, 0), *v, *vend;
        if (!kend) return NULL;
        p = js_ws(kend, e);
        if (p >= e || *p != ':') return NULL;
        v = js_ws(p + 1, e);
        vend = js_skip(v, e, 0);
        if (!vend) return NULL;
        if ((size_t)(kend - 1 - ks) == kl && memcmp(ks, key, kl) == 0) {
            if (ve) *ve = vend;
            return v;
        }
        p = js_ws(vend, e);
        if (p < e && *p == ',') p = js_ws(p + 1, e);
    }
    return NULL;
}

static int js_num(const char *v, const char *ve, double *out)
{
    char b[48];
    size_t l;
    if (!v || v >= ve) return 0;
    l = (size_t)(ve - v);
    if (l >= sizeof b || *v == '"' || *v == 'n' || *v == '{' || *v == '[') return 0;
    memcpy(b, v, l);
    b[l] = 0;
    *out = atof(b);
    return 1;
}

static int js_str(const char *v, const char *ve, char *out, int cap)
{
    int o = 0;
    if (!v || v >= ve || *v != '"') return 0;
    for (v++; v < ve - 1 && o < cap - 1; v++) {
        if (*v == '\\' && v + 1 < ve - 1) {
            v++;
            out[o++] = *v == 'n' ? '\n' : *v;
            continue;
        }
        out[o++] = *v;
    }
    out[o] = 0;
    return 1;
}

static const char *js_arr_first(const char *v, const char *ve)
{
    const char *p;
    if (!v || v >= ve || *v != '[') return NULL;
    p = js_ws(v + 1, ve);
    return (p < ve && *p != ']') ? p : NULL;
}

static const char *js_arr_next(const char *el, const char *ve, const char **elend)
{
    const char *p = js_skip(el, ve, 0);
    if (elend) *elend = p;
    if (!p) return NULL;
    p = js_ws(p, ve);
    if (p < ve && *p == ',') return js_ws(p + 1, ve);
    return NULL;
}

/* [ip, port] */
static int js_ep(const char *v, const char *ve, struct sockaddr_in *out)
{
    const char *a = js_arr_first(v, ve), *ae, *b, *be;
    char ip[48];
    double port;
    struct in_addr in;
    if (!a) return 0;
    b = js_arr_next(a, ve, &ae);
    if (!b || !js_str(a, ae, ip, sizeof ip)) return 0;
    js_arr_next(b, ve, &be);
    if (!be || !js_num(b, be, &port) || port < 1 || port > 65535) return 0;
    if (inet_pton(AF_INET, ip, &in) != 1) return 0;
    memset(out, 0, sizeof *out);
    out->sin_family = AF_INET;
    out->sin_addr = in;
    out->sin_port = htons((uint16_t)port);
    return 1;
}

static void lb_close(loctun *t, const char *why, double now);

/* W334: eine JSON-Zeile ueber TLS (nie vor bestandener Pin-Pruefung; sbtls_write lehnt das selbst ab). */
static void lb_send(loctun *t, const char *json)
{
    char b[4200];
    int l = _snprintf_s(b, sizeof b, _TRUNCATE, "%s\n", json);
    if (l > 0 && t->test_cap && t->test_cap_len + l < t->test_cap_max) { /* W373 TEST */
        memcpy(t->test_cap + t->test_cap_len, b, (size_t)l);
        t->test_cap_len += l;
        t->test_cap[t->test_cap_len] = 0;
    }
    if (l > 0 && t->tls && t->lb_s != INVALID_SOCKET && (t->lb_state == LB_HELLO || t->lb_state == LB_LOBBY)) {
        if (sbtls_write(t->tls, b, l) != SBTLS_OK) lb_close(t, "tls-senden", tl_now(t));
    }
    SecureZeroMemory(b, sizeof b);
}
/* Trennung vom Treffpunkt: Schluessel dieser Steuerverbindung loeschen; alle Lobby-Verbindungen enden (neue
 * Steuerverbindung = neue client_id, neues E, neue Paarschluessel - SECURITY.md §4.2 Punkte 4/5). */
static void lb_close(loctun *t, const char *why, double now)
{
    int i;
    if (t->tls) {
        sbtls_free(t->tls);
        t->tls = NULL;
    }
    if (t->lb_s != INVALID_SOCKET) {
        WSAEventSelect(t->lb_s, NULL, 0);
        closesocket(t->lb_s);
        t->lb_s = INVALID_SOCKET;
    }
    if (t->lb_state != LB_OFF) tl_logf(t, "LOCLOBBY: Treffpunkt getrennt (%s) - neuer Versuch in 5 s", why);
    /* W353: Grund fuer den Launcher - Aufbau gescheitert (Netz/Firewall/Dienst aus) bzw. Verbindung verloren */
    if (t->lb_state == LB_CONNECTING || t->lb_state == LB_TLS) {
        strcpy_s(t->lb_err, sizeof t->lb_err, "connect");
    } else if (t->lb_state != LB_OFF && strcmp(why, "upgrade_required") != 0 && strcmp(why, "e2e_required") != 0) {
        strcpy_s(t->lb_err, sizeof t->lb_err, "lost");
    }
    for (i = 0; i < TUN_MAX_LINKS; i++) {
        tlink *L = &t->links[i];
        if (!L->used || L->cid == 0) continue;
        tl_link_drop(t, L, "Treffpunkt getrennt");
        L->used = 0;
    }
    sbe2e_kp_free(&t->kp);
    SecureZeroMemory(t->my_pub, sizeof t->my_pub);
    SecureZeroMemory(t->udp_key, sizeof t->udp_key);
    t->lobby_code[0] = 0;
    if (t->pub) { /* W373: neue Steuerverbindung = neue Lobby; offene Beitritte scheitern */
        if (t->pub_joining) pub_reject(t, t->pub_join, "Treffpunkt getrennt", now);
        pub_ghosts_clear(t);
        t->pub_host = t->pub_started = t->pub_creating = 0;
        t->pub_host_vip = t->pub_host_cid = 0;
    }
    t->my_cid = 0;
    t->bctr = 0;
    t->lb_state = LB_OFF;
    t->lb_joined = 0;
    t->lb_listed = 0;
    t->bound = 0;
    t->lb_next_try = now + 5000.0;
}

/* W329: Konfigurationswerte (Raum, Pilotname) JSON-sicher in einen String einsetzen. */
static void tl_js_esc(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (; in && *in && o + 7 < cap; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 0x20 || c == 0x7F) {
            o += (size_t)_snprintf_s(out + o, cap - o, _TRUNCATE, "\\u%04x", c);
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = 0;
}

/* W329: Lobbyliste nur fuer den eigenen Raum (Server filtert mit "name"; alte Server ignorieren das Feld).
 * Ungefiltert kann die Liste aller Lobbys eine Steuerzeile sprengen - dann faende der Client seinen Raum nie. */
static void lb_list(loctun *t)
{
    char b[300], r[200];
    tl_js_esc(t->c.room, r, sizeof r);
    _snprintf_s(b, sizeof b, _TRUNCATE, "{\"t\":\"lobby_list\",\"game\":\"loc\",\"name\":\"%s\"}", r);
    lb_send(t, b);
}

/* W334: Pins fuer den Treffpunkt (SECURITY.md §3.2/§3.6). 0 = keiner -> keine Verbindung. */
static int lb_pins(loctun *t, const struct sockaddr_in *a)
{
    char ev[80];
    DWORD gl;
    int lo = (ntohl(a->sin_addr.s_addr) >> 24) == 127;
    t->npins = 0;
    memset(t->pins, 0, sizeof t->pins);
    if (_stricmp(t->lb_host, SB_STEELNET_HOST) == 0) {
        if (sbtls_parse_pin(SB_STEELNET_SPKI1, t->pins[t->npins])) t->npins++;
        if (sbtls_parse_pin(SB_STEELNET_SPKI2, t->pins[t->npins])) t->npins++;
    }
    if (t->npins < 2 && t->c.spki[0] && sbtls_parse_pin(t->c.spki, t->pins[t->npins])) t->npins++; /* loc_lobby_spki */
    gl = GetEnvironmentVariableA("SB_STEELNET_SPKI", ev, sizeof ev);
    if (lo && gl > 0 && gl < sizeof ev && t->npins < 2 && sbtls_parse_pin(ev, t->pins[t->npins])) t->npins++; /* TEST */
    return t->npins;
}

static void lb_connect(loctun *t, double now)
{
    struct sockaddr_in a;
    u_long nb = 1;
    if (!tl_resolve(t->lb_host, t->lb_port, &a)) {
        tl_logf(t, "LOCLOBBY: Treffpunkt nicht aufloesbar - neuer Versuch in 5 s");
        strcpy_s(t->lb_err, sizeof t->lb_err, "dns"); /* W353: kein Netz bzw. Name unbekannt */
        t->lb_next_try = now + 5000.0;
        return;
    }
    if (!tl_dest_ok(t, a.sin_addr.s_addr)) {
        tl_logf(t, "LOCLOBBY: Treffpunkt-Adresse nicht erlaubt");
        t->lb_next_try = now + 60000.0;
        return;
    }
    if (lb_pins(t, &a) == 0) {
        /* ohne Pin kein ungepruefter Kanal - auch nicht fuer private Server (SECURITY.md §3.6) */
        if (InterlockedExchange(&t->sec, SEC_NOPIN) != SEC_NOPIN)
            tl_logf(t, "LOCSEC: kein Server-Pin fuer den Treffpunkt (Dienst nicht eingerichtet bzw. loc_lobby_spki fehlt) - "
                       "Online-Spiel aus");
        t->lb_next_try = now + 60000.0;
        return;
    }
    t->lb_addr = a;
    t->lb_s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (t->lb_s == INVALID_SOCKET) return;
    tl_nodelay(t->lb_s);
    ioctlsocket(t->lb_s, FIONBIO, &nb);
    tl_sock_events(t, t->lb_s, FD_READ | FD_WRITE | FD_CONNECT | FD_CLOSE);
    t->lb_inlen = 0;
    t->lb_state = LB_CONNECTING;
    t->lb_next_try = now + 5000.0; /* Zeitlimit fuer den Aufbau */
    if (connect(t->lb_s, (struct sockaddr *)&a, sizeof a) != 0 && WSAGetLastError() != WSAEWOULDBLOCK) {
        lb_close(t, "connect", now);
    }
}

static void lb_hello(loctun *t)
{
    char b[640], ip[16], pe[120], pub[65];
    struct sockaddr_in me;
    int l = sizeof me;
    tl_js_esc(t->c.pilot, pe, sizeof pe); /* W329 */
    getsockname(t->us, (struct sockaddr *)&me, &l);
    if (me.sin_addr.s_addr == INADDR_ANY) {
        /* eigene LAN-Adresse fuer den Durchstich im selben Netz: die zum Treffpunkt benutzte */
        struct sockaddr_in la;
        int ll = sizeof la;
        if (getsockname(t->lb_s, (struct sockaddr *)&la, &ll) == 0) me.sin_addr = la.sin_addr;
    }
    tohex(t->my_pub, 32, pub);
    _snprintf_s(b, sizeof b, _TRUNCATE,
                "{\"t\":\"hello\",\"v\":2,\"client\":\"steelbound-loc\",\"pilot\":\"%s\",\"controller\":\"kbm\","
                "\"local_eps\":[[\"%s\",%u]],\"e2e\":{\"suite\":1,\"pub\":\"%s\"},\"cver\":\"%s\"}",
                pe, locnet_ip_str(me.sin_addr.s_addr, ip), t->uport, pub, tl_myver(t)); /* W420: cver */
    if (t->acct) { /* W328: Konto -> auth_challenge statt welcome */
        size_t bl = strlen(b);
        _snprintf_s(b + bl - 1, sizeof b - bl + 1, _TRUNCATE, ",\"account\":%u,\"exp\":%lld}", t->acct, t->acct_exp);
    }
    t->lb_state = LB_HELLO;
    lb_send(t, b);
}

/* W334: BIND2/KEEPALIVE2 (SECURITY.md §5.1): "SBN1" | Typ | client_id u32 | bctr u64 | HMAC(udp_key, 0..16)[:16] */
static void lb_bind(loctun *t, double now)
{
    uint8_t p[33], mac[32];
    memcpy(p, "SBN1", 4);
    p[4] = t->bound ? SN_KEEPALIVE2 : SN_BIND2;
    put32be(p + 5, t->my_cid);
    t->bctr++;
    put32be(p + 9, (uint32_t)(t->bctr >> 32));
    put32be(p + 13, (uint32_t)t->bctr);
    sbe2e_hmac(t->udp_key, 32, p, 17, mac);
    memcpy(p + 17, mac, 16);
    SecureZeroMemory(mac, sizeof mac);
    sendto(t->us, (const char *)p, 33, 0, (struct sockaddr *)&t->srv_udp, sizeof t->srv_udp); /* nie verzoegert */
    t->lb_bind_last = now;
}

static trelay *lb_relay(loctun *t, const char *name, int create)
{
    int i;
    for (i = 0; i < t->nrelays; i++) {
        if (strcmp(t->relays[i].name, name) == 0) return &t->relays[i];
    }
    if (!create || t->nrelays >= TUN_MAX_RELAYS) return NULL;
    memset(&t->relays[t->nrelays], 0, sizeof t->relays[0]);
    strncpy_s(t->relays[t->nrelays].name, sizeof t->relays[0].name, name, _TRUNCATE);
    return &t->relays[t->nrelays++];
}

static void lb_relay_set(loctun *t, const char *name, const char *host, uint16_t port)
{
    trelay *r = lb_relay(t, name, 1);
    if (!r) return;
    strncpy_s(r->host, sizeof r->host, host && host[0] ? host : t->lb_host, _TRUNCATE);
    r->port = port;
    r->resolved = tl_resolve(r->host, r->port, &r->addr) && tl_dest_ok(t, r->addr.sin_addr.s_addr);
    r->pings = 0;
    r->nrtt = 0;
    r->ms = -1;
    if (!r->resolved) tl_logf(t, "LOCLOBBY: Relay %s nicht erreichbar/aufloesbar", r->name);
}

static void lb_report(loctun *t)
{
    char b[2048];
    int o, i, first = 1;
    o = _snprintf_s(b, sizeof b, _TRUNCATE, "{\"t\":\"route_report\",\"relays\":[");
    for (i = 0; i < t->nrelays && o > 0; i++) {
        if (t->relays[i].ms < 0) continue;
        o += _snprintf_s(b + o, sizeof b - o, _TRUNCATE, "%s[\"%s\",%.1f]", first ? "" : ",", t->relays[i].name,
                         t->relays[i].ms);
        first = 0;
    }
    o += _snprintf_s(b + o, sizeof b - o, _TRUNCATE, "],\"peers\":[");
    first = 1;
    for (i = 0; i < TUN_MAX_LINKS && o > 0; i++) {
        tlink *L = &t->links[i];
        if (!L->used || L->cid == 0) continue;
        if (L->direct_ok && L->direct_rtt > 0) {
            o += _snprintf_s(b + o, sizeof b - o, _TRUNCATE, "%s[%u,%.1f]", first ? "" : ",", L->cid, L->direct_rtt);
        } else if (L->direct_failed) {
            o += _snprintf_s(b + o, sizeof b - o, _TRUNCATE, "%s[%u,null]", first ? "" : ",", L->cid);
        } else {
            continue;
        }
        first = 0;
        L->reported = 1;
    }
    if (o > 0) _snprintf_s(b + o, sizeof b - o, _TRUNCATE, "]}");
    lb_send(t, b);
}

static void lb_on_route(loctun *t, const char *m, const char *e)
{
    const char *v, *ve, *el, *ee;
    double d;
    char mode[16] = "";
    v = js_get(m, e, "mode", &ve);
    js_str(v, ve, mode, sizeof mode);
    v = js_get(m, e, "max_ms", &ve);
    if (js_num(v, ve, &d)) t->route_max = d;
    strcpy_s(t->route_mode, sizeof t->route_mode, mode);
    v = js_get(m, e, "relays", &ve); /* W395: waehlbare Relays (fehlt beim alten Treffpunkt = alle) */
    if (v) {
        unsigned mask = 0;
        for (el = js_arr_first(v, ve); el; el = js_arr_next(el, ve, NULL)) {
            char rn[24] = "";
            trelay *r;
            ee = js_skip(el, ve, 0);
            if (!ee || !js_str(el, ee, rn, sizeof rn)) continue;
            r = lb_relay(t, rn, 0);
            if (r) mask |= 1u << (unsigned)(r - t->relays);
        }
        t->relay_usable = mask;
        t->relay_usable_set = 1;
    }
    v = js_get(m, e, "links", &ve);
    for (el = js_arr_first(v, ve); el; el = js_arr_next(el, ve, NULL)) {
        const char *x, *xe;
        char via[12] = "", rname[24] = "", host[96] = "", tk[80] = "";
        double peer = 0, port = 0, lid = 0, ms = -1;
        tlink *L;
        ee = js_skip(el, ve, 0);
        if (!ee) break;
        x = js_get(el, ee, "peer", &xe);
        js_num(x, xe, &peer);
        x = js_get(el, ee, "via", &xe);
        js_str(x, xe, via, sizeof via);
        x = js_get(el, ee, "ms", &xe);
        js_num(x, xe, &ms);
        if (!(peer > 0 && peer <= 4294967295.0)) continue; /* W329: Bereich vor (uint32_t) */
        L = tl_link_by_cid(t, (uint32_t)peer, 0);
        if (!L) continue;
        L->route_ms = ms;
        L->route_seen = 1;
        /* W395: link_id + Ticket kommen (neuer Treffpunkt) fuer jedes Paar, auch "direct" - Grundlage fuer die
         * Bindung an allen Relays; ein alter Treffpunkt nennt sie nur bei "relay". */
        x = js_get(el, ee, "link_id", &xe);
        js_num(x, xe, &lid);
        x = js_get(el, ee, "ticket", &xe);
        js_str(x, xe, tk, sizeof tk);
        if (lid >= 1 && lid <= 4294967295.0 && tk[0]) {
            uint8_t tkb[TICKET_LEN];
            if (unhex(tk, tkb, TICKET_LEN)) {
                if (!L->have_ticket || L->relay_link != (uint32_t)lid) {
                    int ri;
                    for (ri = 0; ri < TUN_MAX_RELAYS; ri++) { /* neue link_id: alle Bindungen neu */
                        L->rb[ri].ok = 0;
                        L->rb[ri].have_cookie = 0;
                        L->rb[ri].bind_last = 0;
                    }
                }
                memcpy(L->ticket, tkb, TICKET_LEN);
                L->relay_link = (uint32_t)lid;
                L->have_ticket = 1;
                SecureZeroMemory(tkb, sizeof tkb);
            }
        }
        if (strcmp(via, "direct") == 0) {
            if (L->want != 1) tl_logf(t, "LOCROUTE: c%u -> direkt (%.1f ms, Modus %s)", L->cid, ms, mode);
            L->want = 1;
            continue;
        }
        x = js_get(el, ee, "relay", &xe);
        js_str(x, xe, rname, sizeof rname);
        x = js_get(el, ee, "host", &xe);
        js_str(x, xe, host, sizeof host);
        x = js_get(el, ee, "port", &xe);
        js_num(x, xe, &port);
        if (!(port >= 0 && port <= 65535) || !(lid >= 0 && lid <= 4294967295.0)) continue; /* W329 */
        {
            trelay *r = lb_relay(t, rname, 0);
            int idx;
            if (!r || (host[0] && strcmp(host, r->host) != 0) || (port > 0 && (uint16_t)port != r->port)) {
                lb_relay_set(t, rname, host, (uint16_t)port);
                r = lb_relay(t, rname, 0);
            }
            if (!r || !r->resolved || !L->have_ticket || L->relay_link != (uint32_t)lid) {
                tl_logf(t, "LOCROUTE: c%u -> Relay %s unbrauchbar", L->cid, rname);
                continue;
            }
            idx = (int)(r - t->relays);
            if (L->want != 2 || L->want_ri != idx) {
                tl_logf(t, "LOCROUTE: c%u -> Relay %s (%.1f ms, Modus %s)%s", L->cid, rname, ms, mode,
                        L->rt_on ? " - Vorschlag, Wahl im Client (W395)" : "");
            }
            L->want_ri = idx;
            L->want = 2;
        }
    }
}

/* W334: Verbindung auf Anfang setzen (neues Paar): alles abbauen, Eintrag mit derselben cid neu belegen. */
static tlink *tl_link_fresh(loctun *t, tlink *L)
{
    uint32_t cid = L->cid;
    if (L->up || L->e2e.valid || L->lsn != INVALID_SOCKET) {
        tl_link_drop(t, L, "neues Paar vom Treffpunkt");
    }
    L->used = 0;
    return tl_link_by_cid(t, cid, 1);
}

static void lb_on_peer_info(loctun *t, const char *m, const char *e)
{
    const char *v, *ve, *el, *ee, *x = NULL, *xe = NULL;   /* W332: /sdl C4703 */
    double peer = 0, acct = 0, lobby = 0, suite = 0;
    char pn_hex[40] = "", pub_hex[80] = "", pilot[24] = "";
    uint8_t pn[16], pub[32], th[32];
    sbe2e_side me, pa;
    tlink *L;
    struct sockaddr_in ep, cand[TUN_MAX_CAND];
    int ncand = 0, k, i;
    v = js_get(m, e, "peer", &ve);
    if (!js_num(v, ve, &peer) || !(peer > 0 && peer <= 4294967295.0)) return; /* W329: Bereich vor (uint32_t) */
    v = js_get(m, e, "pair_nonce", &ve);
    js_str(v, ve, pn_hex, sizeof pn_hex);
    v = js_get(m, e, "e2e", &ve);
    x = v ? js_get(v, ve, "suite", &xe) : NULL;
    js_num(x, xe, &suite);
    x = v ? js_get(v, ve, "pub", &xe) : NULL;
    js_str(x, xe, pub_hex, sizeof pub_hex);
    v = js_get(m, e, "account", &ve);
    js_num(v, ve, &acct);
    v = js_get(m, e, "lobby_id", &ve);
    js_num(v, ve, &lobby);
    v = js_get(m, e, "pilot", &ve);
    js_str(v, ve, pilot, sizeof pilot);
    if ((uint32_t)peer == t->my_cid || !unhex(pn_hex, pn, 16) || suite != 1.0 || !unhex(pub_hex, pub, 32) ||
        !(acct >= 0 && acct <= 4294967295.0) || !(lobby >= 0 && lobby <= 4294967295.0) || !pilot[0] ||
        strlen(pilot) > 16) {
        tl_logf(t, "LOCSEC: peer_info fuer c%u unvollstaendig/ungueltig - verworfen", (unsigned)peer);
        return;
    }
    /* Adressen fuer den Durchstich */
    v = js_get(m, e, "public_ep", &ve);
    if (js_ep(v, ve, &ep) && tl_dest_ok(t, ep.sin_addr.s_addr)) cand[ncand++] = ep;
    v = js_get(m, e, "local_eps", &ve);
    for (el = js_arr_first(v, ve); el && ncand < TUN_MAX_CAND; el = js_arr_next(el, ve, &ee)) {
        const char *y = js_skip(el, ve, 0);
        int dup = 0;
        if (!y || !js_ep(el, y, &ep) || !tl_dest_ok(t, ep.sin_addr.s_addr)) continue;
        for (k = 0; k < ncand; k++) dup |= sa_eq(&cand[k], &ep);
        if (!dup) cand[ncand++] = ep;
    }
    L = tl_link_by_cid(t, (uint32_t)peer, 1);
    if (!L) return;
    if (L->e2e.valid || L->blocked) {
        if (memcmp(L->pair_nonce, pn, 16) == 0 && memcmp(L->peer_pub, pub, 32) == 0) {
            /* gleiches Paar (z. B. neue NAT-Adresse nach BIND2 des Partners): nur Adressen auffrischen. Ein wegen
             * Verstoss gesperrter Partner bleibt gesperrt, bis der Treffpunkt ein neues Paar bildet. */
            if (!L->blocked) {
                memcpy(L->cand, cand, sizeof cand);
                L->ncand = ncand;
                if (!L->direct_ok && !t->c.nodirect && !L->punching) {
                    L->punching = 1;
                    L->punch_start = tl_now(t);
                    L->punch_last = 0;
                }
            }
            return;
        }
        if (memcmp(L->pair_nonce, pn, 16) == 0) {
            /* W333 (SECURITY.md §14, Fuer W334 Nr. 2): gleiches pair_nonce, aber anderer Schluessel -> Abbruch,
             * nie still neu ableiten (Treffpunkt fehlerhaft oder Eingriff) */
            L->blocked = 1;
            sbe2e_link_free(&L->e2e);
            tl_logf(t, "LOCSEC: peer_info fuer c%u mit gleichem pair_nonce, aber anderem Schluessel - getrennt", L->cid);
            return;
        }
        L = tl_link_fresh(t, L);
        if (!L) return;
    }
    /* Schluessel je Paar (SECURITY.md §4.3) */
    memset(&me, 0, sizeof me);
    memset(&pa, 0, sizeof pa);
    me.cid = t->my_cid;
    me.account = t->acct;
    strcpy_s(me.pilot, sizeof me.pilot, t->my_pilot);
    memcpy(me.pub, t->my_pub, 32);
    pa.cid = (uint32_t)peer;
    pa.account = (uint32_t)acct;
    strcpy_s(pa.pilot, sizeof pa.pilot, pilot);
    memcpy(pa.pub, pub, 32);
    memcpy(L->pair_nonce, pn, 16);
    memcpy(L->peer_pub, pub, 32);
    L->account = (uint32_t)acct;
    L->info_at = tl_now(t); /* W353 */
    strcpy_s(L->pilot, sizeof L->pilot, pilot);
    if (sbe2e_transcript(th, (uint32_t)lobby, pn, &me, &pa) != 0 ||
        sbe2e_derive(&t->kp, pub, th, t->my_cid, (uint32_t)peer, &L->e2e) != 0) {
        L->blocked = 1;
        tl_logf(t, "LOCSEC: Schluessel fuer c%u abgelehnt (ungueltiger Schluessel/Identitaet) - keine Verbindung", L->cid);
        _snprintf_s(pn_hex, sizeof pn_hex, _TRUNCATE, "%u", L->cid);
        {
            char b[96];
            _snprintf_s(b, sizeof b, _TRUNCATE, "{\"t\":\"link_report\",\"peer\":%u,\"kind\":\"failed\"}", L->cid);
            lb_send(t, b);
        }
        SecureZeroMemory(th, sizeof th);
        return;
    }
    SecureZeroMemory(th, sizeof th);
    for (i = 0; i < TUN_MAX_LINKS; i++) { /* sid-Kollision mit einer anderen eigenen Verbindung (~1e-8) */
        tlink *o = &t->links[i];
        if (o != L && o->used && o->e2e.valid && o->e2e.sid == L->e2e.sid) {
            char b[96];
            sbe2e_link_free(&L->e2e);
            L->blocked = 1;
            tl_logf(t, "LOCSEC: Kennung fuer c%u doppelt - Verbindung abgelehnt", L->cid);
            _snprintf_s(b, sizeof b, _TRUNCATE, "{\"t\":\"link_report\",\"peer\":%u,\"kind\":\"failed\"}", L->cid);
            lb_send(t, b);
            return;
        }
    }
    if (sbe2e_random(&L->probe_id, 8) != 0 || L->probe_id == 0) {
        sbe2e_link_free(&L->e2e);
        L->blocked = 1;
        return;
    }
    memcpy(L->cand, cand, sizeof cand);
    L->ncand = ncand;
    if (!L->direct_ok && !t->c.nodirect) {
        L->punching = 1;
        L->punch_start = tl_now(t);
        L->punch_last = 0;
        L->direct_failed = 0;
    } else if (t->c.nodirect) {
        L->direct_failed = 1;
        L->rep_dirty = 1;
    }
    tl_logf(t, "LOCLOBBY: Mitspieler c%u (%s, %s), %d Adressen fuer den Durchstich, Ende-zu-Ende-Schluessel bereit%s",
            L->cid, L->pilot, L->account ? "Konto" : "Gast", L->ncand, t->c.nodirect ? " (direkt aus: TEST)" : "");
}

static void lb_on_lobby_state(loctun *t, const char *m, const char *e)
{
    const char *lb, *lbe, *v, *ve, *el;
    double id = 0;
    int i;
    char code[24] = "";
    lb = js_get(m, e, "lobby", &lbe);
    if (!lb) return;
    v = js_get(lb, lbe, "id", &ve);
    js_num(v, ve, &id);
    if (!(id >= 0 && id <= 2147483647.0)) return;
    t->lb_lobby_id = (int)id;
    /* W334: Raumcode der Lobby (nur an Mitglieder), Anzeige im Launcher; nur Crockford-Zeichen und '-' */
    v = js_get(lb, lbe, "code", &ve);
    if (!v) v = js_get(m, e, "code", &ve);
    if (js_str(v, ve, code, sizeof code)) {
        size_t k, l = strlen(code);
        int ok = l >= 8 && l <= 9;
        for (k = 0; k < l && ok; k++) ok = (code[k] >= '0' && code[k] <= '9') || (code[k] >= 'A' && code[k] <= 'Z') || code[k] == '-';
        if (ok && strcmp(code, t->lobby_code) != 0) {
            strcpy_s(t->lobby_code, sizeof t->lobby_code, code);
            tl_logf(t, "LOCLOBBY: Raumcode der Lobby bereit (im Launcher sichtbar)");
        }
    }
    if (!t->lb_joined) {
        t->lb_joined = 1;
        t->lb_state = LB_LOBBY;
        t->lb_err[0] = 0; /* W353: in der Lobby - alter Fehlergrund gilt nicht mehr */
        t->lb_host_left = 0; /* W366 */
        tl_logf(t, "LOCLOBBY: in Lobby %d", t->lb_lobby_id);
    }
    /* Mitspieler, die gegangen sind: Verbindung beenden */
    v = js_get(lb, lbe, "members", &ve);
    for (i = 0; i < TUN_MAX_LINKS; i++) {
        tlink *L = &t->links[i];
        int found = 0;
        if (!L->used || L->cid == 0) continue;
        for (el = js_arr_first(v, ve); el; el = js_arr_next(el, ve, NULL)) {
            const char *ee = js_skip(el, ve, 0), *x, *xe;
            double cid = 0;
            if (!ee) break;
            x = js_get(el, ee, "client_id", &xe);
            if (js_num(x, xe, &cid) && cid > 0 && cid <= 4294967295.0 && (uint32_t)cid == L->cid) found = 1;
        }
        if (!found && !L->up) {
            tl_link_drop(t, L, "nicht mehr in der Lobby");
            L->used = 0; /* noch nicht verbunden: verwerfen */
        } else if (!found && !L->down_logged) {
            tl_logf(t, "LOCLOBBY: c%u hat die Lobby verlassen", L->cid);
            L->down_logged = 1;
        }
    }
}


/* ====================================================================== W373 LOC-PUBLIC-LOBBY
 * Oeffentliche Lobbys (loc_lobby_vis=public, ohne Raumcode; LOC_BOOT §49, net/SECURITY.md §23):
 *  - Host: Sobald das Spiel ein Match anbietet (Lobby-Zustand G+0x15 = 0x22/0x23, "wartet auf Beitritte"), legt der
 *    Transport eine oeffentliche Lobby an. rules = eigene vip + Sitze/Lager/Mission/Zaehler aus dem Spielspeicher
 *    (nur Werte, geprueft wie beim Empfaenger) + gerundete Relay-Pings (Ping-Schaetzung). Aenderungen -> lobby_update,
 *    Missionsbeginn -> lobby_start (unsichtbar), Host verlaesst das Match ohne Mitspieler -> lobby_leave.
 *  - Suchender: Die Suche des Spiels (Typ 1, einmal je Suche, Antwortfenster ~3 s) holt die Liste offener Lobbys;
 *    je Lobby beantwortet ein "Geist" (Sockets auf vip:Stellvertreter+201/+202) die Suche lokal mit einer Host-Info
 *    (Typ 2, Felder nur aus geprueften Zahlen gebaut, Gastfilter vor dem Senden). Waehlt das Spiel den Eintrag
 *    (Typ 3 an die vip), tritt der Transport der Lobby bei; der Client wartet ohne Antwort beliebig lange (B), die
 *    Beitrittsanfrage geht ueber die Verbindung, sobald sie steht. Fehlschlag -> Typ 7 (6 B) vom Geist.
 * Keine Adressen in Liste und Log; Namen nur druckbares ASCII, gekuerzt. */
static int pub_lockstep(loctun *t)
{
    LONG l = t->n->st.last34_ms;
    return l && (LONG)(GetTickCount() - (DWORD)l) < 2000;
}

/* Host-Zustand aus dem Spiel (loc_net.c schreibt pub_w): 1 = gueltig. */
static int pub_snap(loctun *t, uint8_t s[8], uint8_t c[2], int *state)
{
    LONG w0 = t->n->pub_w[0], w1 = t->n->pub_w[1], w2 = t->n->pub_w[2];
    if (!(w2 & 0x1000000)) return 0;
    put32(s, (uint32_t)w0);
    put32(s + 4, (uint32_t)w1);
    *state = (int)(w2 & 0xFF);
    c[0] = (uint8_t)(w2 >> 8);
    c[1] = (uint8_t)(w2 >> 16);
    return 1;
}

/* Werte wie loc_guard hostinfo_ok + Wertebereiche des Originals (LOC_BOOT §49.2). */
static int pub_fields_ok(const uint8_t s[8], const uint8_t c[2])
{
    int a = (int8_t)s[2], b = (int8_t)s[3];
    return s[0] <= 3 && s[1] <= 3 && a >= 1 && b >= 0 && a + b <= 10 && s[4] <= 2 && s[5] < 0x1B && s[5] != 0x18 &&
           s[5] != 0x1A && s[6] <= 2 && s[7] <= 1 && (int8_t)c[0] >= 0 && (int8_t)c[0] <= a && (int8_t)c[1] >= 0 &&
           (int8_t)c[1] <= b;
}

static void pub_ghost_close(tghost *g)
{
    if (g->s[0] != INVALID_SOCKET) closesocket(g->s[0]);
    if (g->s[1] != INVALID_SOCKET) closesocket(g->s[1]);
    g->s[0] = g->s[1] = INVALID_SOCKET;
    g->used = 0;
}

static void pub_ghosts_clear(loctun *t)
{
    int i;
    for (i = 0; i < TUN_MAX_GHOST; i++) {
        if (t->gh[i].used) pub_ghost_close(&t->gh[i]);
    }
    t->pub_join = -1;
}

static tghost *pub_ghost_by_vip(loctun *t, uint32_t vip)
{
    int i;
    for (i = 0; i < TUN_MAX_GHOST; i++) {
        if (t->gh[i].used && t->gh[i].vip == vip) return &t->gh[i];
    }
    return NULL;
}

/* Socket des Geists fuer Spielport gp (1201/1202) auf vip:Stellvertreter-Basis+(gp-1000), wie ein Mitspieler. */
static SOCKET pub_ghost_sock(loctun *t, tghost *g, int k)
{
    struct sockaddr_in a;
    BOOL off = FALSE;
    DWORD got = 0;
    SOCKET s;
    if (g->s[k] != INVALID_SOCKET) return g->s[k];
    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return s;
    WSAIoctl(s, 0x9800000Cu, &off, sizeof off, NULL, 0, &got, NULL, NULL);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = g->vip;
    a.sin_port = htons((uint16_t)(t->c.proxy_base + (k ? 202 : 201)));
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    g->s[k] = s;
    return s;
}

static void pub_ghost_send(loctun *t, tghost *g, uint16_t gp, const uint8_t *d, int len)
{
    struct sockaddr_in a;
    SOCKET s;
    uint16_t hp = locnet_port_out(t->n, t->n->self_ip, gp);
    /* W373: auch selbst gebaute Rahmen durch den Gastfilter (Verteidigung in der Tiefe) und nur an gebundene Ports */
    if (!lguard_udp_ok(gp, d, len) || !locnet_udp_bound(t->n, hp)) return;
    s = pub_ghost_sock(t, g, gp == 1202);
    if (s == INVALID_SOCKET) return;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = t->n->self_ip;
    a.sin_port = htons(hp);
    sendto(s, (const char *)d, len, 0, (struct sockaddr *)&a, sizeof a);
}

/* Host-Info Typ 2 (100 B) eines Geists fuer die laufende Suche (Nonce-Echo). Layout LOC_BOOT §49.2. */
static void pub_hostinfo(loctun *t, const tghost *g, uint8_t d[100])
{
    memset(d, 0, 100);
    d[0] = 2;
    memcpy(d + 4, t->pub_nonce, 8);
    d[0x0C] = 0x00; /* XNKID System-Link-Typ (oberes Halbbyte 0), eindeutig je Lobby */
    memcpy(d + 0x0D, "SBP", 3);
    put32(d + 0x10, (uint32_t)g->lid);
    memcpy(d + 0x14, "SBPUBLIC", 8); /* XNKEY: die HLE prueft keine Schluessel */
    put32(d + 0x1C, (uint32_t)g->lid);
    locnet_xnaddr(t->n, g->vip, d + 0x24);
    memcpy(d + 0x48, g->sf, 8);
    memcpy(d + 0x50, g->name, strlen(g->name)); /* <= 15 Zeichen, Rest 0 */
    d[0x60] = g->cf[0];
    d[0x61] = g->cf[1];
}

static void pub_answer(loctun *t, double now)
{
    int i;
    uint8_t d[100];
    if (t->pub_t1_at <= 0 || now - t->pub_t1_at > 2500.0) return; /* nur auf eine Suche, im Antwortfenster (~3 s) */
    for (i = 0; i < TUN_MAX_GHOST; i++) {
        tghost *g = &t->gh[i];
        if (!g->used || g->ans == t->pub_seq) continue;
        g->ans = t->pub_seq;
        pub_hostinfo(t, g, d);
        pub_ghost_send(t, g, 1201, d, 100);
        t->pub_answered++;
    }
}

static void pub_list_req(loctun *t, double now)
{
    t->pub_list_at = now;
    lb_send(t, "{\"t\":\"lobby_list\",\"game\":\"loc\",\"open\":true}");
}

/* Lobby verlassen (eigener Abgang): Verbindungen abbauen wie bei lobby_closed. */
static void pub_leave(loctun *t, const char *why, double now)
{
    int i;
    lb_send(t, "{\"t\":\"lobby_leave\"}");
    for (i = 0; i < TUN_MAX_LINKS; i++) {
        tlink *L = &t->links[i];
        if (!L->used || L->cid == 0) continue;
        tl_link_drop(t, L, "Lobby verlassen");
        L->used = 0;
    }
    tl_logf(t, "LOCPUB: Lobby %d verlassen (%s)", t->lb_lobby_id, why);
    t->lb_joined = 0;
    t->lb_listed = 1;
    if (t->lb_state == LB_LOBBY) t->lb_state = LB_HELLO;
    t->pub_host = t->pub_started = 0;
    t->pub_host_cid = 0;
    t->pub_host_vip = 0;
    t->pub_nmem = 0;
    t->pub_idle_at = 0;
    t->pub_off_at = 0;
    t->pub_create_at = now + 2000.0;
}

/* rules der eigenen Lobby: vip, Spielfelder, gerundete Relay-Pings (rt_<Relay>, 10-ms-Stufen). */
static int pub_rules(loctun *t, const uint8_t s[8], const uint8_t c[2], char *out, int cap)
{
    int i, n, nr = 0;
    n = _snprintf_s(out, (size_t)cap, _TRUNCATE,
                    "{\"vip\":%u,\"fa\":%u,\"fb\":%u,\"ma\":%d,\"mb\":%d,\"tm\":%u,\"ms\":%u,\"cf\":%u,\"op\":%u,"
                    "\"ca\":%d,\"cb\":%d",
                    (unsigned)ntohl(t->n->self_ip), s[0], s[1], (int8_t)s[2], (int8_t)s[3], s[4], s[5], s[6], s[7],
                    (int8_t)c[0], (int8_t)c[1]);
    for (i = 0; i < t->nrelays && nr < 4 && n > 0 && n < cap - 64; i++) {
        trelay *r = &t->relays[i];
        size_t k, l = strlen(r->name);
        int ok = l > 0 && l <= 24;
        for (k = 0; k < l && ok; k++) ok = isalnum((unsigned char)r->name[k]) || r->name[k] == '_' || r->name[k] == '-';
        if (!ok || !t->relays_done || r->ms <= 0) continue;
        n += _snprintf_s(out + n, (size_t)(cap - n), _TRUNCATE, ",\"rt_%s\":%d", r->name,
                         ((int)(r->ms + 9.999) / 10) * 10 > 990 ? 990 : ((int)(r->ms + 9.999) / 10) * 10);
        nr++;
    }
    if (n <= 0 || n >= cap - 2) return 0;
    out[n++] = '}';
    out[n] = 0;
    return 1;
}

static void pub_publish(loctun *t, const uint8_t s[8], const uint8_t c[2], int create, double now)
{
    char rules[400], b[600], nm[64];
    if (!pub_rules(t, s, c, rules, sizeof rules)) return;
    memcpy(t->pub_sig, s, 8);
    memcpy(t->pub_sig + 8, c, 2);
    t->pub_upd_at = now;
    if (create) {
        int maxp = (int8_t)s[2] + (int8_t)s[3];
        tl_js_esc(t->my_pilot[0] ? t->my_pilot : "Pilot", nm, sizeof nm);
        _snprintf_s(b, sizeof b, _TRUNCATE,
                    "{\"t\":\"lobby_create\",\"name\":\"%s\",\"game\":\"loc\",\"mode\":\"syslink\",\"map\":\"m%02u\","
                    "\"max_players\":%d,\"rules\":%s,\"visibility\":\"public\"}",
                    nm, s[5], maxp < 2 ? 2 : (maxp > 10 ? 10 : maxp), rules);
        t->pub_creating = 1;
        tl_logf(t, "LOCPUB: Match angeboten - oeffentliche Lobby anlegen (Mission %u, Sitze %d/%d)", s[5],
                (int8_t)s[2], (int8_t)s[3]);
    } else {
        _snprintf_s(b, sizeof b, _TRUNCATE, "{\"t\":\"lobby_update\",\"rules\":%s}", rules);
    }
    lb_send(t, b);
}

/* Anzeigename "Pilot 120ms" (<= 15 Zeichen + NUL, nur druckbares ASCII ohne Steuerzeichen). */
static void pub_name(const char *pilot, int est, char out[16])
{
    char suf[12] = "";
    int i, o = 0, room;
    if (est > 0) _snprintf_s(suf, sizeof suf, _TRUNCATE, " %dms", est > 999 ? 999 : est);
    room = 15 - (int)strlen(suf);
    if (strncmp(pilot, "Guest-", 6) == 0 && pilot[6]) { /* Gast: "~Name" (Platz; bleibt von Kontonamen unterscheidbar) */
        out[o++] = '~';
        pilot += 6;
    }
    for (i = 0; pilot[i] && o < room; i++) {
        char ch = pilot[i];
        out[o++] = (ch >= 0x20 && ch <= 0x7E) ? ch : '?';
    }
    while (o > 0 && out[o - 1] == ' ') o--;
    if (o == 0) out[o++] = '?';
    out[o] = 0;
    strcat_s(out, 16, suf);
}

/* Liste offener oeffentlicher Lobbys -> Geister (jede Zahl vor der Verwendung geprueft). */
static void pub_on_list(loctun *t, const char *m, const char *e, double now)
{
    const char *v, *ve, *el, *more, *moree;
    int i, seen[TUN_MAX_GHOST], bad = 0, n = 0;
    memset(seen, 0, sizeof seen);
    v = js_get(m, e, "lobbies", &ve);
    more = js_get(m, e, "more", &moree);
    for (el = js_arr_first(v, ve); el; el = js_arr_next(el, ve, NULL)) {
        const char *ee = js_skip(el, ve, 0), *x, *xe, *ru, *rue;
        char pilot[24] = "", state[12] = "", key[40];
        double id = 0, f[11], rt;
        static const char *const k[11] = {"vip", "fa", "fb", "ma", "mb", "tm", "ms", "cf", "op", "ca", "cb"};
        uint8_t s8[8], c2[2];
        uint32_t vip;
        int j, est = -1, ok = 1;
        tghost *g;
        if (!ee) break;
        if (++n > 64) break; /* Obergrenze je Liste */
        x = js_get(el, ee, "id", &xe);
        js_num(x, xe, &id);
        x = js_get(el, ee, "state", &xe);
        js_str(x, xe, state, sizeof state);
        x = js_get(el, ee, "host_pilot", &xe);
        js_str(x, xe, pilot, sizeof pilot);
        ru = js_get(el, ee, "rules", &rue);
        for (j = 0; j < 11 && ok; j++) {
            x = ru ? js_get(ru, rue, k[j], &xe) : NULL;
            ok = js_num(x, xe, &f[j]) && f[j] >= (j == 0 ? 0.0 : -128.0) && f[j] <= (j == 0 ? 4294967295.0 : 127.0) &&
                 f[j] == floor(f[j]);
        }
        if (!ok || !(id >= 1 && id <= 2147483647.0) || strcmp(state, "open") != 0 || !pilot[0]) {
            bad += ok ? 0 : 1;
            continue;
        }
        if (t->lb_joined && (int)id == t->lb_lobby_id) continue; /* eigene Lobby: der echte Host antwortet */
        for (j = 0; j < 8; j++) s8[j] = (uint8_t)(int)f[j + 1];
        c2[0] = (uint8_t)(int)f[9];
        c2[1] = (uint8_t)(int)f[10];
        vip = htonl((uint32_t)f[0]);
        if (!pub_fields_ok(s8, c2) || (int8_t)c2[0] + (int8_t)c2[1] >= (int8_t)s8[2] + (int8_t)s8[3]) {
            bad++;
            continue; /* ungueltig bzw. voll */
        }
        {
            uint32_t h = ntohl(vip);
            int clash = (h >> 24) != 127u || h == 0x7F000001u || (h & 0xFFu) == 0u || (h & 0xFFu) == 255u ||
                        (vip & 0xFFFFFF00u) == 0 || ((vip ^ t->n->self_ip) & htonl(0x00FFFFFFu)) == 0;
            for (j = 0; j < TUN_MAX_LINKS && !clash; j++)
                clash = t->links[j].used && t->links[j].vip == vip;
            for (j = 0; j < TUN_MAX_GHOST && !clash; j++) /* abEnet = letzte 3 Adressbytes: eindeutig */
                clash = t->gh[j].used && t->gh[j].lid != (int)id && ((t->gh[j].vip ^ vip) & htonl(0x00FFFFFFu)) == 0;
            if (clash) {
                bad++;
                continue;
            }
        }
        for (j = 0; j < t->nrelays; j++) { /* Ping-Schaetzung ueber das gemeinsame Relay mit der kleinsten Summe */
            trelay *r = &t->relays[j];
            if (!t->relays_done || r->ms <= 0 || strlen(r->name) > 24) continue;
            _snprintf_s(key, sizeof key, _TRUNCATE, "rt_%s", r->name);
            x = ru ? js_get(ru, rue, key, &xe) : NULL;
            if (js_num(x, xe, &rt) && rt >= 0 && rt <= 990 && (est < 0 || rt + r->ms < est)) est = (int)(rt + r->ms + 0.5);
        }
        g = NULL;
        for (j = 0; j < TUN_MAX_GHOST; j++) {
            if (t->gh[j].used && t->gh[j].lid == (int)id) g = &t->gh[j];
        }
        if (g && g->vip != vip) { /* andere vip unter derselben Lobby: neu anlegen */
            if (t->pub_join == (int)(g - t->gh)) continue;
            pub_ghost_close(g);
            g = NULL;
        }
        if (!g) {
            for (j = 0; j < TUN_MAX_GHOST && !g; j++) {
                if (!t->gh[j].used) g = &t->gh[j];
            }
            if (!g) break;
            memset(g, 0, sizeof *g);
            g->used = 1;
            g->lid = (int)id;
            g->vip = vip;
            g->s[0] = g->s[1] = INVALID_SOCKET;
            g->ans = t->pub_seq - 1;
        }
        memcpy(g->sf, s8, 8);
        memcpy(g->cf, c2, 2);
        g->est = est;
        pub_name(pilot, est, g->name);
        g->seen = now;
        seen[g - t->gh] = 1;
    }
    for (i = 0; i < TUN_MAX_GHOST; i++) { /* nicht mehr gelistet (voll, gestartet, geschlossen) -> weg */
        if (t->gh[i].used && !seen[i] && i != t->pub_join && (!more || now - t->gh[i].seen > 30000.0))
            pub_ghost_close(&t->gh[i]);
    }
    if (bad && !t->pub_bad_logged) {
        t->pub_bad_logged = 1;
        tl_logf(t, "LOCPUB: %d Listeneintraege ungueltig/voll - nicht angezeigt", bad);
    }
    n = 0;
    for (i = 0; i < TUN_MAX_GHOST; i++) n += t->gh[i].used;
    if (n != t->pub_ngames) tl_logf(t, "LOCPUB: %d offene oeffentliche Spiele", n);
    t->pub_ngames = n;
    pub_answer(t, now);
}

/* lobby_state im oeffentlichen Modus: Host und Mitgliederzahl merken. */
static void pub_on_state(loctun *t, const char *m, const char *e)
{
    const char *lb, *lbe = NULL, *v, *ve = NULL, *el;   /* /sdl C4703 */
    int nm = 0;
    uint32_t host = 0;
    lb = js_get(m, e, "lobby", &lbe);
    v = lb ? js_get(lb, lbe, "members", &ve) : NULL;
    for (el = js_arr_first(v, ve); el; el = js_arr_next(el, ve, NULL)) {
        const char *ee = js_skip(el, ve, 0), *x, *xe;
        double cid = 0;
        if (!ee) break;
        nm++;
        x = js_get(el, ee, "client_id", &xe);
        js_num(x, xe, &cid);
        x = js_get(el, ee, "host", &xe);
        if (x && xe - x >= 4 && strncmp(x, "true", 4) == 0 && cid > 0 && cid <= 4294967295.0) host = (uint32_t)cid;
    }
    t->pub_nmem = nm;
    if (host) t->pub_host_cid = host;
    if (t->pub_creating && host == t->my_cid) {
        t->pub_creating = 0;
        t->pub_host = 1;
        t->pub_started = 0;
        tl_logf(t, "LOCPUB: oeffentliche Lobby %d angelegt", t->lb_lobby_id);
    }
}

/* Synthetische Ablehnung (Typ 7, 6 B, Grund 0) vom Geist an das Spiel (B: Client verlaesst den Wartezustand). */
static void pub_reject(loctun *t, int gi, const char *why, double now)
{
    static const uint8_t rej[6] = {7, 0, 0, 0, 0, 0};
    if (gi >= 0 && gi < TUN_MAX_GHOST && t->gh[gi].used) {
        pub_ghost_send(t, &t->gh[gi], 1202, rej, 6);
        pub_ghost_close(&t->gh[gi]);
    }
    tl_logf(t, "LOCPUB: Beitritt abgelehnt (%s)", why);
    t->pub_join = -1;
    t->pub_joining = 0;
    t->pub_t3_len = 0;
    if (t->lb_joined && !t->pub_host) pub_leave(t, why, now);
}

/* Verbindung steht: Geist der Lobby schliessen (Stellvertreter-Ports frei), gemerkte Beitrittsanfrage nachreichen. */
static void pub_on_link_up(loctun *t, tlink *L)
{
    tghost *g = pub_ghost_by_vip(t, L->vip);
    if (g) {
        int gi = (int)(g - t->gh);
        pub_ghost_close(g);
        if (gi == t->pub_join) t->pub_join = -1;
    }
    if (t->pub_joining && L->cid == t->pub_host_cid) {
        if (L->vip != t->pub_join_vip) {
            t->pub_join_bad = 1; /* Host meldet eine andere vip als gelistet -> Ablehnung im naechsten pub_tick */
            return;
        }
        t->pub_joining = 0;
        t->pub_host_vip = L->vip;
        t->pub_idle_at = tl_now(t);
        if (t->pub_t3_len > 0) {
            int k, slot = -1;
            for (k = 0; k < TUN_DQ; k++) {
                if (!L->dq[k].used) { slot = k; break; }
            }
            if (slot >= 0) {
                L->dq[slot].used = 1;
                L->dq[slot].id = L->dg_next++;
                L->dq[slot].src = t->pub_t3_src;
                L->dq[slot].dst = 1202;
                L->dq[slot].len = (uint16_t)t->pub_t3_len;
                L->dq[slot].tx = 0;
                memcpy(L->dq[slot].data, t->pub_t3, (size_t)t->pub_t3_len);
                InterlockedIncrement(&t->st.dg_tx);
            }
            t->pub_t3_len = 0;
        }
        tl_logf(t, "LOCPUB: Verbindung zum Host c%u steht - Beitrittsanfrage weitergereicht", L->cid);
    }
}

static int pub_host_stream(loctun *t)
{
    int i;
    for (i = 0; i < TUN_MAX_STREAMS; i++) {
        const tstream *st = &t->streams[i];
        if (st->used && st->link >= 0 && st->link < TUN_MAX_LINKS && t->links[st->link].used &&
            t->links[st->link].vip == t->pub_host_vip)
            return 1;
    }
    return 0;
}

static void pub_tick(loctun *t, double now)
{
    uint8_t s[8], c[2];
    int state = -1, snap = pub_snap(t, s, c, &state), open_host;
    open_host = snap && (state == 0x22 || state == 0x23) && pub_fields_ok(s, c);
    /* --- Host --- */
    if (!t->lb_joined) {
        if (open_host && !t->pub_creating && t->pub_join < 0 && !t->pub_joining && now >= t->pub_create_at) {
            pub_publish(t, s, c, 1, now);
            t->pub_create_at = now + 3000.0;
        }
        if (t->pub_creating && now >= t->pub_create_at) t->pub_creating = 0; /* keine Antwort: spaeter erneut */
    } else if (t->pub_host) {
        if (!t->pub_started) {
            if (pub_lockstep(t) || (!open_host && t->pub_nmem > 1 && t->pub_off_at && now - t->pub_off_at >= 2000.0)) {
                lb_send(t, "{\"t\":\"lobby_start\"}");
                t->pub_started = 1;
                tl_logf(t, "LOCPUB: Match beginnt - Lobby %d nicht mehr gelistet", t->lb_lobby_id);
            } else if (!open_host && t->pub_nmem <= 1 && t->pub_off_at && now - t->pub_off_at >= 3000.0) {
                pub_leave(t, "Host hat das Match verlassen", now);
                return;
            } else if (open_host && memcmp(t->pub_sig, s, 8) == 0 && memcmp(t->pub_sig + 8, c, 2) == 0) {
                t->pub_chg_at = 0;
            } else if (open_host) {
                if (!t->pub_chg_at) t->pub_chg_at = now; /* erst nach 1 s Ruhe (keine Zwischenstaende) */
                if (now - t->pub_chg_at >= 1000.0 && now - t->pub_upd_at >= 1500.0) {
                    pub_publish(t, s, c, 0, now);
                    t->pub_chg_at = 0;
                }
            }
        } else if (open_host && t->pub_nmem <= 1 && !pub_lockstep(t)) {
            pub_leave(t, "Match zu Ende, Host allein", now); /* danach neue offene Lobby */
            return;
        }
        if (open_host) t->pub_off_at = 0;
        else if (!t->pub_off_at) t->pub_off_at = now;
        return;
    }
    /* --- Suchender --- */
    if (t->pub_t1_new) {
        t->pub_t1_new = 0;
        t->pub_seq++;
        pub_answer(t, now);                                    /* sofort aus dem Zwischenspeicher */
        if (now - t->pub_list_at >= 1500.0) pub_list_req(t, now); /* und frisch (Antwort im Fenster) */
    } else if (!t->lb_joined && t->pub_t1_at > 0 && now - t->pub_t1_at < 60000.0 && now - t->pub_list_at >= 10000.0) {
        pub_list_req(t, now); /* Suchbildschirm offen: Liste warm halten */
    }
    if (t->pub_join >= 0 && !t->pub_joining && t->gh[t->pub_join].used) {
        char b[96];
        if (t->lb_joined) pub_leave(t, "anderes Spiel gewaehlt", now);
        t->pub_joining = 1;
        t->pub_join_at = now;
        t->pub_join_vip = t->gh[t->pub_join].vip;
        t->pub_join_bad = 0;
        t->pub_host_cid = 0;
        _snprintf_s(b, sizeof b, _TRUNCATE, "{\"t\":\"lobby_join\",\"lobby_id\":%d}", t->gh[t->pub_join].lid);
        lb_send(t, b);
        tl_logf(t, "LOCPUB: Spiel gewaehlt - trete Lobby %d bei", t->gh[t->pub_join].lid);
    }
    if (t->pub_joining && (t->pub_join_bad || now - t->pub_join_at > 25000.0)) {
        pub_reject(t, t->pub_join, t->pub_join_bad ? "Host-Adresse passt nicht" : "keine Verbindung in 25 s", now);
        return;
    }
    if (t->lb_joined && !t->pub_joining && t->pub_host_vip) { /* Mitglied ohne Spielverbindung: Platz freigeben */
        if (pub_host_stream(t)) t->pub_idle_at = now;
        else if (now - t->pub_idle_at > 20000.0) pub_leave(t, "keine Spielverbindung zum Host", now);
    }
}

/* W334: Lobby betreten - mit Raumcode beitreten, sonst eigene Code-Lobby anlegen (Standard, SECURITY.md §3.5);
 * Rolle "join" ohne Code: oeffentliche Lobby gleichen Namens suchen. */
static void lb_enter(loctun *t)
{
    char b[400], r[200];
    if (t->pub) { /* W373: oeffentlich - Lobby erst, wenn das Spiel ein Match anbietet (pub_tick) */
        t->lb_listed = 1;
        return;
    }
    if (t->c.code[0]) {
        tl_js_esc(t->c.code, r, sizeof r);
        _snprintf_s(b, sizeof b, _TRUNCATE, "{\"t\":\"lobby_join\",\"code\":\"%s\"}", r);
        lb_send(t, b);
    } else if (_stricmp(t->c.role, "join") == 0) {
        lb_list(t);
    } else {
        tl_js_esc(t->c.room, r, sizeof r); /* W329; v2: nur Anzeigename */
        _snprintf_s(b, sizeof b, _TRUNCATE,
                    "{\"t\":\"lobby_create\",\"name\":\"%s\",\"game\":\"loc\",\"mode\":\"syslink\",\"map\":\"any\","
                    "\"max_players\":10,\"rules\":{},\"visibility\":\"code\"}",
                    r);
        lb_send(t, b);
    }
    t->lb_listed = 1;
}

static void lb_on_line(loctun *t, const char *m, const char *e, double now)
{
    const char *v, *ve;
    char typ[24] = "";
    v = js_get(m, e, "t", &ve);
    if (!js_str(v, ve, typ, sizeof typ)) return;
    if (strcmp(typ, "auth_challenge") == 0 && t->acct) { /* W328: Beweis mit dem Spielschluessel */
        char nonce[40] = "", msg[96], b[128], hex[65];
        double cid = 0;
        uint8_t mac[32];
        v = js_get(m, e, "nonce", &ve);
        js_str(v, ve, nonce, sizeof nonce);
        v = js_get(m, e, "client_id", &ve);
        js_num(v, ve, &cid);
        if (strlen(nonce) != 32 || !(cid > 0 && cid <= 4294967295.0)) {
            lb_close(t, "auth", now);
            return;
        }
        _snprintf_s(msg, sizeof msg, _TRUNCATE, "steelnet-auth-v1|%s|%u", nonce, (unsigned)cid);
        tl_hmac(t->gkey, 32, msg, strlen(msg), mac);
        sb_sha256_hex(mac, hex);
        _snprintf_s(b, sizeof b, _TRUNCATE, "{\"t\":\"auth\",\"proof\":\"%s\"}", hex);
        lb_send(t, b);
        SecureZeroMemory(b, sizeof b);
        SecureZeroMemory(mac, sizeof mac);
        SecureZeroMemory(hex, sizeof hex);
        return;
    }
    if (strcmp(typ, "welcome") == 0) {
        double cid = 0, up = 0, ver = 0;
        char key[80] = "", eff[24] = "";
        const char *el;
        size_t k;
        v = js_get(m, e, "v", &ve);
        js_num(v, ve, &ver);
        if (ver != 2.0) { /* W334: nur v2 */
            lb_close(t, "welcome v != 2", now);
            return;
        }
        v = js_get(m, e, "client_id", &ve);
        js_num(v, ve, &cid);
        v = js_get(m, e, "udp_key", &ve);
        js_str(v, ve, key, sizeof key);
        v = js_get(m, e, "udp_port", &ve);
        js_num(v, ve, &up);
        v = js_get(m, e, "pilot", &ve); /* W328: wirksamer Name (Kontoname bzw. Guest-...), geht ins Transkript */
        js_str(v, ve, eff, sizeof eff);
        /* W329: Zahlen vom Netz vor der Umwandlung begrenzen (double -> uint ausserhalb des Bereichs ist UB) */
        if (!(cid > 0 && cid <= 4294967295.0) || !(up >= 1 && up <= 65535) || !unhex(key, t->udp_key, 32) || !eff[0] ||
            strlen(eff) > 16) {
            SecureZeroMemory(key, sizeof key);
            lb_close(t, "welcome", now);
            return;
        }
        SecureZeroMemory(key, sizeof key);
        for (k = 0; eff[k]; k++) {
            if ((unsigned char)eff[k] < 0x20 || (unsigned char)eff[k] > 0x7E) {
                lb_close(t, "welcome", now);
                return;
            }
        }
        strcpy_s(t->my_pilot, sizeof t->my_pilot, eff);
        t->my_cid = (uint32_t)cid;
        t->bctr = 0;
        t->srv_udp = t->lb_addr;
        t->srv_udp.sin_port = htons((uint16_t)up);
        t->lb_state = LB_HELLO; /* bis zur Lobby */
        tl_logf(t, "LOCLOBBY: angemeldet als c%u", t->my_cid);
        tl_logf(t, "LOCLOBBY: Pilot \"%s\" (%s)", eff, t->acct ? "Konto" : "Gast");
        if (t->acct) InterlockedExchange(&t->auth_state, 2);
        lb_bind(t, now);
        /* Relays des Treffpunkts */
        v = js_get(m, e, "relays", &ve);
        for (el = js_arr_first(v, ve); el; el = js_arr_next(el, ve, NULL)) {
            const char *ee = js_skip(el, ve, 0), *x, *xe;
            char name[24] = "", host[96] = "";
            double port = 0;
            if (!ee) break;
            x = js_get(el, ee, "name", &xe);
            js_str(x, xe, name, sizeof name);
            x = js_get(el, ee, "host", &xe);
            js_str(x, xe, host, sizeof host);
            x = js_get(el, ee, "port", &xe);
            js_num(x, xe, &port);
            if (name[0] && port >= 1 && port <= 65535) lb_relay_set(t, name, host, (uint16_t)port);
        }
        t->relays_start = now;
        t->relays_done = 0;
        lb_enter(t);
        t->lb_list_last = now;
    } else if (strcmp(typ, "lobby_list") == 0 && t->pub) { /* W373 */
        pub_on_list(t, m, e, now);
    } else if (strcmp(typ, "lobby_list") == 0 && !t->lb_joined) {
        const char *el;
        double found = 0;
        v = js_get(m, e, "lobbies", &ve);
        for (el = js_arr_first(v, ve); el; el = js_arr_next(el, ve, NULL)) {
            const char *ee = js_skip(el, ve, 0), *x, *xe;
            char name[40] = "", state[12] = "";
            double id = 0;
            if (!ee) break;
            x = js_get(el, ee, "name", &xe);
            js_str(x, xe, name, sizeof name);
            x = js_get(el, ee, "state", &xe);
            js_str(x, xe, state, sizeof state);
            x = js_get(el, ee, "id", &xe);
            js_num(x, xe, &id);
            if (strcmp(name, t->c.room) == 0 && strcmp(state, "open") == 0) found = id;
        }
        if (found > 2147483647.0) found = 0; /* W329: Bereich vor (int) */
        if (found > 0) {
            char b[96];
            _snprintf_s(b, sizeof b, _TRUNCATE, "{\"t\":\"lobby_join\",\"lobby_id\":%d}", (int)found);
            lb_send(t, b);
        }
        t->lb_listed = 0; /* ohne Treffer spaeter erneut */
    } else if (strcmp(typ, "lobby_state") == 0) {
        lb_on_lobby_state(t, m, e);
        if (t->pub) pub_on_state(t, m, e); /* W373 */
    } else if (strcmp(typ, "peer_info") == 0) {
        lb_on_peer_info(t, m, e);
    } else if (strcmp(typ, "route") == 0) {
        lb_on_route(t, m, e);
    } else if (strcmp(typ, "lobby_closed") == 0) {
        int i;
        tl_logf(t, "LOCLOBBY: Lobby geschlossen (Host gegangen)");
        for (i = 0; i < TUN_MAX_LINKS; i++) { /* §4.2 Punkt 5: Schluessel loeschen, Stellvertreter schliessen */
            tlink *L = &t->links[i];
            if (!L->used || L->cid == 0) continue;
            tl_link_drop(t, L, "Lobby geschlossen");
            L->used = 0;
        }
        t->lobby_code[0] = 0;
        t->lb_joined = 0;
        t->lb_listed = 0;
        t->lb_list_last = now;
        if (t->lb_state == LB_LOBBY) t->lb_state = LB_HELLO;
        if (t->pub) { /* W373: oeffentlich - das Spiel meldet den Abgang selbst, zurueck zur Suche */
            t->pub_host = t->pub_started = 0;
            t->pub_host_vip = 0;
            if (t->pub_joining) pub_reject(t, t->pub_join, "Lobby geschlossen", now);
        } else {
            t->lb_host_left = 1; /* W366: Launcher meldet "Host hat die Lobby geschlossen" (nicht "Code falsch") */
            strcpy_s(t->lb_err, sizeof t->lb_err, "closed");
        }
    } else if (strcmp(typ, "error") == 0) {
        char code[32] = "";
        v = js_get(m, e, "code", &ve);
        js_str(v, ve, code, sizeof code);
        tl_logf(t, "LOCLOBBY: Fehler vom Treffpunkt: %s", code);
        if (strcmp(code, "auth_failed") == 0) { /* W328: Konto abgelehnt (abgelaufen/abgemeldet) -> als Gast weiter */
            tl_logf(t, "LOCLOBBY: Konto-Anmeldung abgelehnt - weiter als Gast (Launcher: neu anmelden)");
            t->acct = 0;
            SecureZeroMemory(t->gkey, sizeof t->gkey);
            InterlockedExchange(&t->auth_state, -1);
        }
        if (strcmp(code, "version_required") == 0) { /* W420: Treffpunkt verlangt eine neuere Client-Version */
            char msg[96] = "", nv[16] = "";
            const char *q;
            v = js_get(m, e, "msg", &ve);
            js_str(v, ve, msg, sizeof msg);
            q = strstr(msg, "client version ");
            if (q) {
                char tmp[24];
                int i;
                q += 15;
                for (i = 0; i < 23 && q[i] && q[i] != ' '; i++) tmp[i] = q[i];
                tmp[i] = 0;
                tl_ver_clean(tmp, i, nv, (int)sizeof nv);
            }
            EnterCriticalSection(&t->cs);
            strcpy_s(t->need_ver, sizeof t->need_ver, nv);
            t->peer_ver[0] = 0;
            LeaveCriticalSection(&t->cs);
            InterlockedExchange(&t->ver_state, 1);
            InterlockedExchange(&t->sec, SEC_VERSION);
            tl_logf(t, "LOCVER: Treffpunkt verlangt Client-Version %s (eigene %s) - Online-Spiel aus", nv[0] ? nv : "?",
                    SB_NET_VERSION);
            lb_close(t, code, now);
            t->lb_next_try = now + 600000.0;
            return;
        }
        if (strcmp(code, "upgrade_required") == 0 || strcmp(code, "e2e_required") == 0) {
            InterlockedExchange(&t->sec, SEC_UPGRADE);
            tl_logf(t, "LOCSEC: Treffpunkt lehnt die Protokollversion ab - Online-Spiel aus");
            lb_close(t, code, now);
            t->lb_next_try = now + 600000.0;
            return;
        }
        if (strcmp(code, "rate_limited") == 0) {
            /* W333 (SECURITY.md §14, Fuer W334 Nr. 5): zu viele falsche Raumcodes von dieser Adresse */
            tl_logf(t, "LOCLOBBY: zu viele Versuche - bitte kurz warten (neuer Versuch in 60 s)");
            strcpy_s(t->lb_err, sizeof t->lb_err, "rate"); /* W353 */
            t->lb_list_last = now + 50000.0;
        }
        if (strcmp(code, "lobby_not_found") == 0 && t->c.code[0]) {
            tl_logf(t, "LOCLOBBY: Raumcode nicht gefunden (Lobby zu oder Code falsch) - neuer Versuch in 10 s");
            strcpy_s(t->lb_err, sizeof t->lb_err, t->lb_host_left ? "closed" : "code"); /* W353, W366 */
        }
        if (strcmp(code, "lobby_full") == 0) strcpy_s(t->lb_err, sizeof t->lb_err, "full"); /* W353 */
        if (t->pub && t->pub_joining && !t->lb_joined && /* W373: Beitritt gescheitert -> Spiel verlaesst den Wartezustand */
            (strcmp(code, "lobby_full") == 0 || strcmp(code, "lobby_started") == 0 ||
             strcmp(code, "lobby_not_found") == 0 || strcmp(code, "rate_limited") == 0 ||
             strcmp(code, "already_in_lobby") == 0)) {
            pub_reject(t, t->pub_join, code, now);
        }
        if (t->pub && t->pub_creating && !t->lb_joined) t->pub_creating = 0; /* W373: Anlegen abgelehnt */
        if (!t->lb_joined) t->lb_listed = 0; /* z. B. lobby_full: spaeter erneut */
    }
}

static void lb_poll(loctun *t, double now)
{
    if (!t->c.lobby[0]) return;
    if (t->lb_state == LB_OFF) {
        if (now >= t->lb_next_try) lb_connect(t, now);
        return;
    }
    if (t->lb_s == INVALID_SOCKET) return;
    if (t->lb_state == LB_CONNECTING) {
        fd_set w, x;
        struct timeval tv = {0, 0};
        FD_ZERO(&w);
        FD_ZERO(&x);
        FD_SET(t->lb_s, &w);
        FD_SET(t->lb_s, &x);
        if (select(0, NULL, &w, &x, &tv) > 0) {
            if (FD_ISSET(t->lb_s, &x)) {
                lb_close(t, "connect", now);
                return;
            }
            /* W334: TLS mit SPKI-Pin; vor bestandener Pruefung geht kein Anwendungsbyte hinaus */
            if (sbtls_client_new(&t->tls, t->lb_s, t->lb_host, (const uint8_t(*)[32])t->pins, t->npins) != SBTLS_OK) {
                tl_logf(t, "LOCSEC: %s", sbtls_error(t->tls));
                lb_close(t, "tls", now);
                return;
            }
            t->lb_state = LB_TLS;
            t->lb_next_try = now + 10000.0; /* Handshake + welcome: 10 s (SECURITY.md §3.3) */
        } else if (now >= t->lb_next_try) {
            lb_close(t, "timeout", now);
            return;
        } else {
            return;
        }
    }
    if (t->lb_state == LB_TLS) {
        int r = sbtls_handshake(t->tls);
        if (r == SBTLS_WANT_IO) {
            if (now >= t->lb_next_try) lb_close(t, "tls-timeout", now);
            return;
        }
        if (r != SBTLS_OK) {
            if (sbtls_pin_failed(t->tls)) {
                InterlockedExchange(&t->sec, SEC_PIN);
                tl_logf(t, "LOCSEC: Server-Pruefung fehlgeschlagen (Pin passt nicht) - keine Daten gesendet");
            } else {
                tl_logf(t, "LOCSEC: %s", sbtls_error(t->tls));
            }
            lb_close(t, "tls", now);
            return;
        }
        if (sbe2e_keypair(&t->kp, t->my_pub) != 0) {
            tl_logf(t, "LOCSEC: Schluesselpaar nicht erzeugbar - Online-Spiel aus");
            InterlockedExchange(&t->sec, SEC_SELFTEST);
            lb_close(t, "e2e", now);
            return;
        }
        InterlockedCompareExchange(&t->sec, SEC_OK, SEC_PIN);
        tl_logf(t, "LOCLOBBY: Treffpunkt verbunden (%s, Server-Pin bestaetigt)", sbtls_version(t->tls));
        lb_hello(t);
        t->lb_ping_last = now;
        if (t->lb_s == INVALID_SOCKET) return;
    }
    if (sbtls_flush(t->tls) == SBTLS_FAIL) {
        lb_close(t, "tls-senden", now);
        return;
    }
    for (;;) {
        int r = sbtls_read(t->tls, t->lb_in + t->lb_inlen, (int)sizeof t->lb_in - 1 - t->lb_inlen);
        char *nl;
        if (r == SBTLS_CLOSED) {
            lb_close(t, "eof", now);
            return;
        }
        if (r == SBTLS_FAIL) {
            tl_logf(t, "LOCSEC: %s", sbtls_error(t->tls));
            lb_close(t, "tls", now);
            return;
        }
        if (r == SBTLS_WANT_IO) break;
        t->lb_inlen += r;
        t->lb_in[t->lb_inlen] = 0;
        while ((nl = (char *)memchr(t->lb_in, '\n', (size_t)t->lb_inlen)) != NULL) {
            int l = (int)(nl - t->lb_in);
            lb_on_line(t, t->lb_in, t->lb_in + l, now);
            if (t->lb_s == INVALID_SOCKET) return;
            memmove(t->lb_in, nl + 1, (size_t)(t->lb_inlen - l - 1));
            t->lb_inlen -= l + 1;
            t->lb_in[t->lb_inlen] = 0;
        }
        if (t->lb_inlen >= (int)sizeof t->lb_in - 1) {
            lb_close(t, "zeile", now);
            return;
        }
    }
    if (!t->my_cid && now >= t->lb_next_try) {
        lb_close(t, "welcome-timeout", now);
        return;
    }
    if (now - t->lb_ping_last >= 15000.0) {
        lb_send(t, "{\"t\":\"ping\"}");
        t->lb_ping_last = now;
        if (t->lb_s == INVALID_SOCKET) return;
    }
    if (t->my_cid && (now - t->lb_bind_last >= (t->bound ? 25000.0 : 300.0))) lb_bind(t, now);
    if (t->pub && t->my_cid) pub_tick(t, now); /* W373 */
    if (t->my_cid && !t->pub && !t->lb_joined && t->lb_listed == 0 && now - t->lb_list_last >= (t->c.code[0] ? 10000.0 : 1000.0)) {
        lb_enter(t);
        t->lb_list_last = now;
    }
}

/* W395: Relay-Index einer Absenderadresse (-1 = keines). */
static int tl_relay_by_addr(loctun *t, const struct sockaddr_in *a)
{
    int i;
    for (i = 0; i < t->nrelays; i++) {
        if (t->relays[i].resolved && sa_eq(a, &t->relays[i].addr)) return i;
    }
    return -1;
}

/* W395: Soll die Verbindung an Relay ri gebunden sein? Mit eigener Wegwahl an allen Relays (dieselbe link_id, das
 * Ticket gilt auf jedem Relay mit dem gemeinsamen Schluessel), sonst nur am vom Treffpunkt gewaehlten. */
static int tl_bind_want(loctun *t, const tlink *L, int ri)
{
    if (!L->used || L->blocked || !L->e2e.valid || !L->have_ticket || L->relay_link == 0) return 0;
    if (ri < 0 || ri >= t->nrelays || !t->relays[ri].resolved) return 0;
    if (L->want == 2 && L->want_ri == ri) return 1;
    if (!t->c.route_auto) return 0;
    /* Wartung (Treffpunkt nennt das Relay nicht mehr): keine neue Bindung, ein laufender Weg laeuft aus */
    return !t->relay_usable_set || (t->relay_usable & (1u << ri)) || (L->path == 2 && L->relay_idx == ri && L->rb[ri].ok);
}

/* W334: TBIND_CONFIRM mit Ticket und Rueckweg-Cookie (SECURITY.md §5.2). */
static void tl_tbind_confirm(loctun *t, tlink *L, int ri)
{
    uint8_t p[5 + TICKET_LEN + 16];
    memcpy(p, "SBN1", 4);
    p[4] = SN_TBIND_CONFIRM;
    memcpy(p + 5, L->ticket, TICKET_LEN);
    memcpy(p + 5 + TICKET_LEN, L->rb[ri].cookie, 16);
    tl_sendto(t, &t->relays[ri].addr, p, sizeof p);
}

static void lb_rx_sbn(loctun *t, const uint8_t *p, int len, const struct sockaddr_in *from, double now)
{
    int i;
    if (len < 5) return;
    switch (p[4]) {
    case SN_BIND_ACK:
        if (len == 11 && sa_eq(from, &t->srv_udp)) {
            if (!t->bound) tl_logf(t, "LOCLOBBY: UDP beim Treffpunkt gebunden");
            t->bound = 1;
        }
        break;
    case SN_PONG:
        if (len == 13) {
            for (i = 0; i < t->nrelays; i++) {
                trelay *r = &t->relays[i];
                int k;
                if (!sa_eq(from, &r->addr) || memcmp(r->nonce, p + 5, 7) != 0) continue;
                k = p[12];
                if (k < 5 && r->ping_at[k] > 0) {
                    if (r->nrtt < 5) r->rtt[r->nrtt++] = now - r->ping_at[k];
                    r->ping_at[k] = 0;
                }
            }
        }
        break;
    case SN_TBIND_COOKIE:
        /* Cookie kommt nur an die echte Absenderadresse (O2). Es traegt keinen Link-Bezug: fuer jede wartende
         * Bindung an diesem Relay versuchen (falsche Kombinationen verwirft das Relay). */
        if (len == 21) {
            int ri = tl_relay_by_addr(t, from);
            for (i = 0; i < TUN_MAX_LINKS && ri >= 0; i++) {
                tlink *L = &t->links[i];
                if (tl_bind_want(t, L, ri) && !L->rb[ri].ok) {
                    memcpy(L->rb[ri].cookie, p + 5, 16);
                    L->rb[ri].have_cookie = 1;
                    tl_tbind_confirm(t, L, ri);
                }
            }
        }
        break;
    case SN_TBIND_ACK:
        if (len == 9) {
            uint32_t lid = get32be(p + 5);
            int ri = tl_relay_by_addr(t, from);
            for (i = 0; i < TUN_MAX_LINKS && ri >= 0; i++) {
                tlink *L = &t->links[i];
                if (tl_bind_want(t, L, ri) && L->relay_link == lid && !L->rb[ri].ok) {
                    L->rb[ri].ok = 1;
                    L->rt_last_ok[1 + ri] = now;
                    tl_logf(t, "LOCTUN: Relay %s fuer c%u gebunden", t->relays[ri].name, L->cid);
                }
            }
        }
        break;
    case SN_RELAY_FROM:
        if (len > 9) {
            uint32_t lid = get32be(p + 5);
            int ri = tl_relay_by_addr(t, from);
            for (i = 0; i < TUN_MAX_LINKS && ri >= 0 && lid != 0; i++) {
                tlink *L = &t->links[i];
                /* W395: jedes Relay, an dem die link_id gebunden sein soll (Echtheit pruefte SBT2) */
                if (L->used && L->relay_link == lid && tl_bind_want(t, L, ri)) {
                    tl_rx_sbt(t, p + 9, len - 9, from, i, ri, now); /* Nutzlast = SBT2 (sonst verworfen) */
                    break;
                }
            }
        }
        break;
    default:
        break;
    }
}

/* ====================================================================== Thread-Zyklus */
static void tl_rx_udp(loctun *t, double now)
{
    uint8_t b[2048];
    for (;;) {
        struct sockaddr_in from;
        int fl = sizeof from;
        int r = recvfrom(t->us, (char *)b, sizeof b, 0, (struct sockaddr *)&from, &fl);
        if (r < 0) {
            if (WSAGetLastError() == WSAECONNRESET || WSAGetLastError() == WSAEMSGSIZE) continue; /* W329: Uebergrosses verwerfen, weiterlesen */
            break;
        }
        if (r < 5 || t->netcut) continue; /* W366 TEST: Netz weg - Empfang verwerfen */
        if (t->nrsim && tl_rsim_cut(t, tl_rsim_find(t, &from))) continue; /* W395 TEST: Weg ausgefallen */
        if (memcmp(b, "SBT2", 4) == 0) {
            tl_rx_sbt(t, b, r, &from, -1, -1, now);
        } else if (memcmp(b, "SBN1", 4) == 0) {
            lb_rx_sbn(t, b, r, &from, now);
        } else {
            InterlockedIncrement(&t->st.e2e_bad); /* W334: SBT1/SBP1 und alles andere ohne Ausnahme verworfen */
        }
    }
}

static void tl_accept(loctun *t, int li)
{
    tlink *L = &t->links[li];
    for (;;) {
        struct sockaddr_in a;
        int l = sizeof a;
        u_long nb = 1;
        SOCKET c = accept(L->lsn, (struct sockaddr *)&a, &l);
        tstream *st;
        uint16_t id;
        uint8_t od[4];
        if (c == INVALID_SOCKET) break;
        if (l < (int)sizeof a || a.sin_addr.s_addr != t->n->self_ip) {
            /* W334 (SECURITY.md §6.5): der Stellvertreter nimmt nur Verbindungen des eigenen Spiels an */
            closesocket(c);
            InterlockedIncrement(&t->st.bad);
            continue;
        }
        tl_nodelay(c);
        ioctlsocket(c, FIONBIO, &nb);
        id = (uint16_t)((++L->stream_ctr << 1) | (ntohl(t->n->self_ip) < ntohl(L->vip) ? 0 : 1));
        st = tl_stream_new(t, li, id, c);
        if (!st) {
            closesocket(c);
            continue;
        }
        st->t_open = tl_now(t); /* W409: Diagnose "Gegenseite schloss sofort" */
        tl_sock_events(t, c, FD_READ | FD_WRITE | FD_CLOSE);
        put16(od, 1000);
        put16(od + 2, 0);
        tl_queue_msg(t, L, S_OPEN, id, od, 4);
        tl_logf(t, "LOCTUN: Strom %u zu c%u geoeffnet (Spiel-TCP 1000)", id, L->cid);
    }
}

/* ---------- W335 NET-AUDIT: Sende-Fuzzer fuer das Spiel-Fuzzing (SECURITY.md §6.6 Punkt 2) ----------
 * TEST: verfaelscht Felder in den Rahmen, die das EIGENE Spiel an den Mitspieler sendet - Typ (Bytes 0/1), Laenge und
 * die Anzahl von 0x34 (Byte 4) bleiben, damit der Gastfilter des Empfaengers die Rahmen durchlaesst. Nur aktiv mit
 * SB_LOC_FUZZ_TX und dem Testweg ohne Treffpunkt (loc_transport_peers); im Online-Pfad nie. Log nur Typ + Feldoffset. */
static uint32_t fz_rand(loctun *t)
{
    uint32_t x = t->fz_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return t->fz_rng = x ? x : 0x9E3779B9u;
}

static uint8_t fz_byte(loctun *t, uint8_t v)
{
    static const uint8_t k[] = {0x00, 0xFF, 0x7F, 0x80, 0x0A, 0x10, 0x41, 0xFE, 0x01, 0x40};
    switch (fz_rand(t) % 5) {
    case 0: return (uint8_t)fz_rand(t);
    case 1: return (uint8_t)(v + 1);
    case 2: return (uint8_t)(v - 1);
    case 3: return v ? v : 0x41; /* Null-Ende von Zeichenketten entfernen */
    default: return k[fz_rand(t) % sizeof k];
    }
}

/* Typen mit Feldern, die der Spielcode als Index/Anzahl/Laenge/Text nutzen kann (SECURITY.md §6.6) */
static int fz_target(unsigned typ)
{
    switch (typ) {
    case 0x01: case 0x02: case 0x03: case 0x04: case 0x06: case 0x0A: case 0x0B: case 0x0D: case 0x13: case 0x1B:
    case 0x20: case 0x22: case 0x27: case 0x0E: case 0x30:
        return 1;
    default:
        return 0;
    }
}

/* W339 TEST (SB_LOC_FUZZ_RULE=1): gezielt die Felder der Inhaltsregeln Stufe 2 verletzen (Erwartung beim Empfaenger:
 * Verstoss, Mitspieler getrennt). Rueckgabe: 1 = Byte an Offset ofs ersetzen durch *v. */
static int fz_rule_byte(unsigned typ, int ofs, uint8_t *v, int udp)
{
    if (udp) {
        if ((typ == 0x03 || typ == 0x04) && ofs >= 0x10 && ofs < 0x20) return *v = 'G', 1; /* Gamertag ohne NUL */
        if (typ == 0x06 && ofs >= 0x3C && ofs < 0x4C) return *v = 'G', 1;
        return 0;
    }
    switch (typ) {
    case 0x0B: case 0x0D: if (ofs >= 0x3C && ofs < 0x4C) return *v = 'G', 1; break;
    case 0x27: if (ofs >= 0x2C && ofs < 0x2C + 23) return *v = 'N', 1; break;
    case 0x0E: case 0x30: if (ofs == 6 || ofs == 7) return *v = 0xFF, 1; break;
    case 0x2A: case 0x2B: if (ofs == 4 || ofs == 5) return *v = 0xFF, 1; break;
    case 0x20: case 0x2E: case 0x22: if (ofs >= 4 && ofs < 8) return *v = 0xFF, 1; break;
    case 0x25: if (ofs == 4) return *v = 0x7F, 1; break;
    default: break;
    }
    return 0;
}

static void fz_note(loctun *t, unsigned typ, int ofs)
{
    if (InterlockedIncrement(&t->fz_bytes) <= 4000) tl_logf(t, "LOCFUZZ: Typ 0x%02X Feld +0x%X", typ, ofs);
}

static void tl_fuzz_stream(loctun *t, tstream *st, uint8_t *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (st->fz.rem <= 0) {
            int fl;
            st->fz.h[st->fz.hl++] = b[i];
            fl = lguard_frame_len(st->fz.h, st->fz.hl);
            if (fl < 0 && st->fz.hl < 5) continue;
            if (fl <= 0) { /* eigener Strom nicht zerlegbar: Fuzzer fuer diesen Strom aus */
                st->fz.hl = 0;
                st->fz.rem = 0x7FFFFFFF;
                st->fz.mut = 0;
                continue;
            }
            st->fz.typ = (unsigned)st->fz.h[0] | ((unsigned)st->fz.h[1] << 8);
            st->fz.fofs = st->fz.hl;
            st->fz.rem = fl - st->fz.hl;
            st->fz.hl = 0;
            /* grosse Rahmen mit Index-/Anzahl-/Textfeldern (SECURITY.md §6.6) oft und dichter, Lockstep selten */
            st->fz.mut = fz_target(st->fz.typ) ? (fz_rand(t) % 2) == 0 : (fz_rand(t) % 12) == 0;
            if (t->fz_rule == 2) st->fz.mut = 0; /* W339 TEST: nur Regel-Felder */
            st->fz.pb = (fz_target(st->fz.typ) ? 12.0 : 3.0) / (double)fl;
            {
                uint8_t dummy;
                int has = 0, o;
                for (o = 2; o < fl && o < 0x60 && !has; o++) has = fz_rule_byte(st->fz.typ, o, &dummy, 0);
                st->fz.rule = t->fz_rule && has && (fz_rand(t) % 3) == 0; /* W339: jeder 3. Rahmen mit Regel-Feld */
                if (st->fz.rule) tl_logf(t, "LOCFUZZ: Regel-Treffer Typ 0x%02X", st->fz.typ);
            }
            if (st->fz.mut) InterlockedIncrement(&t->fz_frames);
            continue;
        }
        if (st->fz.rule) { /* W339 TEST: Regel-Feld verletzen */
            uint8_t v;
            if (fz_rule_byte(st->fz.typ, st->fz.fofs, &v, 0)) b[i] = v;
        }
        if (st->fz.mut && (fz_rand(t) % 1000000u) < (uint32_t)(st->fz.pb * 1000000.0)) {
            b[i] = fz_byte(t, b[i]);
            fz_note(t, st->fz.typ, st->fz.fofs);
        }
        st->fz.fofs++;
        st->fz.rem--;
    }
}

static void tl_fuzz_dgram(loctun *t, uint8_t *d, int len)
{
    int k, m;
    if (t->fz_rule && len >= 0x4C && (fz_rand(t) % 3) == 0) { /* W339 TEST: Regel-Feld im Datagramm verletzen */
        unsigned typ = (unsigned)d[0] | ((unsigned)d[1] << 8);
        int hit = 0;
        for (k = 2; k < len && k < 0x60; k++) {
            uint8_t v;
            if (fz_rule_byte(typ, k, &v, 1)) {
                d[k] = v;
                hit = 1;
            }
        }
        if (hit) tl_logf(t, "LOCFUZZ: Regel-Treffer UDP Typ 0x%02X", typ);
    }
    if (t->fz_rule == 2) return; /* W339 TEST: nur Regel-Felder */
    if (len < 3 || (fz_rand(t) % (fz_target((unsigned)d[0] | ((unsigned)d[1] << 8)) ? 2u : 8u)) != 0) return;
    InterlockedIncrement(&t->fz_dg);
    m = 1 + (int)(fz_rand(t) % (len > 64 ? 12u : 3u));
    for (k = 0; k < m; k++) {
        int pos = 2 + (int)(fz_rand(t) % (uint32_t)(len - 2));
        d[pos] = fz_byte(t, d[pos]);
        fz_note(t, (unsigned)d[0] | ((unsigned)d[1] << 8), pos);
    }
}

static void tl_stream_read(loctun *t, tstream *st)
{
    tlink *L = &t->links[st->link];
    uint8_t b[TUN_MSG];
    if (st->held) { /* W409: OPEN gehalten - Mitspieler schloss inzwischen bzw. Zeit fuer den naechsten Versuch */
        int r, err;
        if (st->fin_rx) {
            tl_stream_give_up(t, st);
            return;
        }
        if (tl_now(t) < st->retry_at) return;
        st->held = 0;
        r = tl_stream_connect(t, st, &err);
        if (r < 0) {
            tl_stream_give_up(t, st);
            return;
        }
        if (r == 0) {
            tl_stream_refused(t, st, err);
            return;
        }
    }
    if (st->connecting) {
        fd_set w, x;
        struct timeval tv = {0, 0};
        FD_ZERO(&w);
        FD_ZERO(&x);
        FD_SET(st->s, &w);
        FD_SET(st->s, &x);
        if (select(0, NULL, &w, &x, &tv) > 0) {
            if (FD_ISSET(st->s, &x)) {
                /* W409: connect ans eigene Spiel abgelehnt (vorher still S_FIN) - halten/neu versuchen bzw. sichtbar
                 * aufgeben (tl_stream_refused) */
                int err = 0, el = sizeof err;
                if (getsockopt(st->s, SOL_SOCKET, SO_ERROR, (char *)&err, &el) != 0 || err == 0) err = WSAECONNREFUSED;
                tl_stream_refused(t, st, err);
                return;
            }
            st->connecting = 0;
            if (st->tries && !(L->bad_logged & 16384)) {
                L->bad_logged |= 16384;
                tl_logf(t, "LOCTUN: Strom %u von c%u: eigenes Spiel nahm TCP 1000 nach %d Versuchen an (%.0f ms)", st->id,
                        L->cid, st->tries + 1, tl_now(t) - st->t_open);
            }
        } else {
            return;
        }
    }
    while (!st->rd_closed) {
        int r;
        if (L->snd_next - L->snd_una >= TUN_SQ - 4 || L->sq_bytes >= TUN_FLOW_BYTES) break; /* Flusskontrolle */
        r = recv(st->s, (char *)b, sizeof b, 0);
        if (r > 0) {
            st->tx_any = 1;
            if (t->fz_on) tl_fuzz_stream(t, st, b, r); /* W335 TEST */
            tl_queue_msg(t, L, S_DATA, st->id, b, r);
            continue;
        }
        if (r < 0 && WSAGetLastError() == WSAEWOULDBLOCK) break;
        if (st->peer && !st->tx_any && !(L->bad_logged & 128)) {
            /* W409: das eigene Spiel nahm den Strom an und schloss ihn vor jedem Byte (z. B. Netz-HLE verwarf die
             * Verbindung: "kein verbundener Mitspieler", bzw. das Spiel schloss seinen Lauschsocket) */
            char gi[64];
            int e = r == 0 ? 0 : WSAGetLastError();
            L->bad_logged |= 128;
            tl_logf(t, "LOCTUN: Strom %u von c%u: eigenes Spiel schloss die Verbindung sofort (nach %.0f ms, %s %d, %s)",
                    st->id, L->cid, tl_now(t) - st->t_open, r == 0 ? "Ende" : "Fehler", e, tl_game_info(t, gi, sizeof gi));
        }
        st->rd_closed = 1;
        if (!st->fin_sent) {
            tl_queue_msg(t, L, S_FIN, st->id, NULL, 0);
            st->fin_sent = 1;
        }
    }
    if (st->used) tl_stream_flush(t, st);
}

static void tl_relay_pings(loctun *t, double now)
{
    int i, all = 1;
    if (t->relays_done || t->nrelays == 0 || !t->my_cid) {
        if (t->nrelays == 0 && t->my_cid && !t->relays_done) t->relays_done = 1;
        return;
    }
    for (i = 0; i < t->nrelays; i++) {
        trelay *r = &t->relays[i];
        if (!r->resolved) continue;
        if (r->pings < 5 && now - t->relays_start >= r->pings * 100.0) {
            uint8_t p[13];
            if (r->pings == 0) locnet_random(r->nonce, 7);
            memcpy(p, "SBN1", 4);
            p[4] = SN_PING;
            memcpy(p + 5, r->nonce, 7);
            p[12] = (uint8_t)r->pings;
            r->ping_at[r->pings] = now;
            tl_sendto(t, &r->addr, p, 13);
            r->pings++;
        }
        if (r->pings < 5 || (r->nrtt < 5 && now - t->relays_start < 1500.0)) all = 0;
    }
    if (all) {
        for (i = 0; i < t->nrelays; i++) {
            trelay *r = &t->relays[i];
            double v[5], tmp;
            int a, b2;
            if (r->nrtt == 0) {
                r->ms = -1;
                continue;
            }
            memcpy(v, r->rtt, sizeof v);
            for (a = 0; a < r->nrtt; a++)
                for (b2 = a + 1; b2 < r->nrtt; b2++)
                    if (v[b2] < v[a]) { tmp = v[a]; v[a] = v[b2]; v[b2] = tmp; }
            r->ms = v[r->nrtt / 2];
            tl_logf(t, "LOCROUTE: Ping Relay %s = %.1f ms (%d/5 Antworten)", r->name, r->ms, r->nrtt);
        }
        t->relays_done = 1;
        lb_report(t);
    }
}

/* W395: Name eines Weg-Slots (Log/Overlay). */
static const char *tl_slot_name(loctun *t, int s)
{
    if (s == 0) return "direkt";
    if (s >= 1 && s <= t->nrelays) return t->relays[s - 1].name;
    return "keiner";
}

/* W395: Messwerte aller nutzbaren Wege einer Verbindung als eine Logzeile (nur Namen und Zahlen). */
static void tl_route_summary(loctun *t, tlink *L, double now, char *b, int cap)
{
    int s, o = 0;
    b[0] = 0;
    for (s = 0; s < LOCROUTE_MAXP && o >= 0 && o < cap; s++) {
        const locroute_path *p = &L->rt.p[s];
        int r;
        if (!p->avail) continue;
        if (p->n) {
            r = _snprintf_s(b + o, (size_t)(cap - o), _TRUNCATE, "%s%s %.1f/%.1f/%.0f%%%s%s", o ? ", " : "",
                            tl_slot_name(t, s), p->srtt, p->jit, 100.0 * locroute_loss(p, now),
                            locroute_alive(p, now) == 0 ? " aus" : "", s == L->rt.active ? "*" : "");
        } else {
            r = _snprintf_s(b + o, (size_t)(cap - o), _TRUNCATE, "%s%s -%s", o ? ", " : "", tl_slot_name(t, s),
                            s == L->rt.active ? "*" : "");
        }
        if (r < 0) break;
        o += r;
    }
}

/* Durchstich, Pings, HELLO, Relay-Bindung, Wegwahl, Lebenszeichen. */
static void tl_link_timers(loctun *t, tlink *L, double now)
{
    int k;
    if (L->drop_pending) tl_link_drop(t, L, "Paketgrenze 2^32 erreicht");
    if (L->blocked || !L->e2e.valid) return; /* W334: ohne Schluessel kein Paket */
    if (L->punching) {
        if (now - L->punch_start >= 3000.0) {
            L->punching = 0;
            L->direct_failed = 1;
            L->rep_dirty = 1;
            tl_logf(t, "LOCTUN: direkter Weg zu c%u gescheitert (3 s) - Relay noetig", L->cid);
        } else if (now - L->punch_last >= 100.0) {
            uint8_t p[HDR + 8]; /* W334: PROBE als SBT2 (jede Sendung neuer ctr, nicht faelschbar) */
            tl_hdr(p, T_PROBE);
            memcpy(p + HDR, &L->probe_id, 8);
            for (k = 0; k < L->ncand && L->e2e.valid; k++) tl_send_to(t, L, &L->cand[k], p, sizeof p);
            L->punch_last = now;
        }
    }
    /* Ping des direkten Weges (5 Proben, 100 ms Abstand; ohne Antwort nach 1 s erneut) */
    if (L->direct_ok && L->direct_rtt <= 0) {
        if (L->dpings < 5 && now >= L->dping_next) {
            uint8_t p[HDR + 4];
            tl_hdr(p, T_PING);
            put32(p + HDR, (uint32_t)L->dpings);
            L->dping_at[L->dpings] = now;
            tl_send_to(t, L, &L->direct, p, sizeof p);
            L->dpings++;
            L->dping_next = now + 100.0;
        } else if (L->dpings >= 5 && (L->ndrtt >= 5 || now - L->dping_next > 1000.0)) {
            if (L->ndrtt == 0) {
                L->dpings = 0; /* alle verloren: neue Runde */
            } else {
                double v[5], tmp;
                int a, b2;
                memcpy(v, L->drtt, sizeof v);
                for (a = 0; a < L->ndrtt; a++)
                    for (b2 = a + 1; b2 < L->ndrtt; b2++)
                        if (v[b2] < v[a]) { tmp = v[a]; v[a] = v[b2]; v[b2] = tmp; }
                L->direct_rtt = v[L->ndrtt / 2] > 0.01 ? v[L->ndrtt / 2] : 0.01;
                L->rep_dirty = 1;
                tl_logf(t, "LOCROUTE: Ping direkt c%u = %.1f ms (%d/5 Antworten)", L->cid, L->direct_rtt, L->ndrtt);
            }
        }
    }
    /* Relay-Bindungen (W395: mit eigener Wegwahl an allen Relays, sonst am vom Treffpunkt gewaehlten) */
    for (k = 0; k < t->nrelays; k++) {
        if (!tl_bind_want(t, L, k)) continue;
        if (L->rb[k].ok && L->rt_on && 1 + k < LOCROUTE_MAXP && now - L->rt_last_ok[1 + k] > 3000.0) {
            /* gebunden, aber seit 3 s keine Antwort (Relay neu gestartet bzw. NAT-Zuordnung weg): neu binden */
            L->rb[k].ok = 0;
            L->rb[k].have_cookie = 0;
            L->rb[k].tries = 10;
            tl_logf(t, "LOCROUTE: Relay %s fuer c%u antwortet nicht - neu binden", t->relays[k].name, L->cid);
        }
        if (!L->rb[k].ok && now - L->rb[k].bind_last >= (L->rb[k].tries < 10 ? 200.0 : 1000.0)) {
            /* W334: TBIND bindet nicht mehr - das Relay antwortet mit einem Cookie an unsere echte Adresse, erst
             * TBIND_CONFIRM (Ticket + Cookie) bindet (SECURITY.md §5.2). */
            if (L->rb[k].have_cookie) {
                tl_tbind_confirm(t, L, k);
            } else {
                uint8_t p[5 + TICKET_LEN];
                memcpy(p, "SBN1", 4);
                p[4] = SN_TBIND;
                memcpy(p + 5, L->ticket, TICKET_LEN);
                tl_sendto(t, &t->relays[k].addr, p, sizeof p);
            }
            L->rb[k].bind_last = now;
            L->rb[k].tries++;
        }
    }
    /* Wegwahl umsetzen */
    {
        int np = L->path, nri = L->relay_idx;
        if (!L->rt_on && t->c.route_auto && L->peer_route && t->c.lobby[0] && L->path != 0) {
            /* W395: beide Seiten waehlen selbst - ab jetzt alle Wege messen, Start = bisheriger Weg */
            L->rt_on = 1;
            locroute_init(&L->rt);
            tl_logf(t, "LOCROUTE: c%u Wegwahl im Client (direkt + %d Relays), Start %s", L->cid, t->nrelays,
                    L->path == 1 ? "direkt" : (L->relay_idx >= 0 ? t->relays[L->relay_idx].name : "Relay"));
        }
        if (L->rt_on) {
            int s, why = 0, ready, prev;
            locroute_set_avail(&L->rt, 0, L->direct_ok && !t->c.nodirect, now);
            for (s = 0; s < t->nrelays && 1 + s < LOCROUTE_MAXP; s++) locroute_set_avail(&L->rt, 1 + s, L->rb[s].ok && tl_bind_want(t, L, s), now);
            if (L->rt.active < 0 && L->path != 0) L->rt.active = L->path == 1 ? 0 : 1 + L->relay_idx;
            for (s = 0; s < LOCROUTE_MAXP; s++) {
                unsigned short seq;
                if (locroute_probe_due(&L->rt, s, now, &seq)) {
                    uint8_t p[HDR + RP_LEN];
                    tl_hdr(p, T_PING);
                    p[HDR] = 'R';
                    p[HDR + 1] = 'T';
                    p[HDR + 2] = (uint8_t)s;
                    p[HDR + 3] = 0;
                    put16(p + HDR + 4, seq);
                    put16(p + HDR + 6, 0);
                    tl_send_slot(t, L, s, p, sizeof p);
                }
            }
            ready = L->route_seen || now - L->info_at > 6000.0; /* Treffpunkt hat die Wege (Ticket) genannt */
            prev = L->rt.active;
            s = locroute_step(&L->rt, now, ready, &why);
            if (L->rt.kick) { /* Verdacht/Ausfall: unbestaetigte Nachrichten jetzt neu (aktiv + zweiter Weg) */
                uint32_t q;
                L->rt.kick = 0;
                for (q = L->snd_una; q != L->snd_next; q++) {
                    tmsg *m = L->sq[q % TUN_SQ];
                    if (m && m->tx > 0) m->last = -1e9;
                }
            }
            if (s >= 0) {
                np = s == 0 ? 1 : 2;
                nri = s == 0 ? L->relay_idx : s - 1;
            }
            if (why) {
                char sum[256];
                tl_route_summary(t, L, now, sum, sizeof sum);
                if (why == 3) {
                    tl_logf(t, "LOCROUTE: c%u Ausfall %s -> %s (%.0f ms ohne Antwort) | %s", L->cid, tl_slot_name(t, prev),
                            tl_slot_name(t, s), L->rt.switch_ms, sum);
                } else {
                    tl_logf(t, "LOCROUTE: c%u %s %s -> %s | %s", L->cid,
                            why == 1 ? "Wahl nach Messung" : why == 2 ? "Wechsel (dauerhaft besser)" : "vorlaeufig",
                            tl_slot_name(t, prev), tl_slot_name(t, s), sum);
                }
            }
        } else if (L->want == 2) {
            if (L->want_ri >= 0 && L->rb[L->want_ri].ok) {
                np = 2;
                nri = L->want_ri;
            }
        } else if (L->want == 1 || L->cid == 0 || !t->c.lobby[0]) {
            if (L->direct_ok) np = 1;
        } else if (L->direct_ok && L->path == 0) {
            np = 1; /* vorlaeufig direkt, bis der Treffpunkt entscheidet */
        }
        if (np != L->path || (np == 2 && nri != L->relay_idx)) {
            L->rep_link = 1;
            if (L->path != 0 && !L->rt_on) {
                tl_logf(t, "LOCROUTE: c%u Weg %s -> %s", L->cid, L->path == 1 ? "direkt" : "Relay",
                        np == 1 ? "direkt" : "Relay");
            }
            L->path = np;
            L->relay_idx = nri;
        }
    }
    if (L->rep_dirty && t->lb_state != LB_OFF && t->relays_done && t->my_cid) {
        L->rep_dirty = 0;
        lb_report(t);
    }
    /* Weg dem Treffpunkt melden (Statistik), sobald die Verbindung steht und die RTT gemessen ist */
    if (L->rep_link && L->up && L->cid && L->rtt_n >= 4 && t->lb_s != INVALID_SOCKET && t->my_cid) {
        char b[128];
        _snprintf_s(b, sizeof b, _TRUNCATE, "{\"t\":\"link_report\",\"peer\":%u,\"kind\":\"%s\",\"rtt_ms\":%.1f}",
                    L->cid, L->path == 2 ? "relay" : "direct", L->srtt);
        lb_send(t, b);
        L->rep_link = 0;
    }
    if (L->got_hello && !L->up && L->path) tl_link_up(t, L); /* W334: HELLO kam vor dem eigenen Weg */
    /* HELLO bis der Mitspieler uns kennt */
    if (L->path != 0 && (!L->got_hello || !L->peer_got) && now - L->hello_last >= 200.0) tl_hello(t, L, now);
    /* Lebenszeichen und Stille */
    if (L->up) {
        if (now - L->last_tx >= 200.0) tl_build_send(t, L, now, 1);
        if (now - L->last_rx >= 15000.0 && !L->down_logged) {
            tl_logf(t, "LOCTUN: c%u seit 15 s still", L->cid);
            L->down_logged = 1;
        } else if (now - L->last_rx < 15000.0) {
            L->down_logged = 0;
        }
    }
}

static double tl_next_wake(loctun *t, double now)
{
    double nx = now + 50.0, gap = (double)t->c.gap_ms;
    int i, j;
    if (t->c.lobby[0] || t->nrelays) nx = now + 20.0;
    for (i = 0; i < TUN_MAX_LINKS; i++) {
        tlink *L = &t->links[i];
        uint32_t s;
        double rto;
        if (!L->used) continue;
        if (L->punching || (L->path && !L->peer_got)) {
            if (now + 20.0 < nx) nx = now + 20.0;
        }
        if (!L->up) continue;
        rto = tl_rto(L);
        if (L->ack_pending && L->ack_due < nx) nx = L->ack_due;
        if (L->last_tx + 200.0 < nx) nx = L->last_tx + 200.0;
        for (s = L->snd_una, j = 0; s != L->snd_next && j < 64; s++, j++) {
            tmsg *m = L->sq[s % TUN_SQ];
            double d;
            if (!m) continue;
            d = m->tx == 0 ? now : (m->tx < L->red ? m->last + gap : m->last + rto);
            if (d < nx) nx = d;
        }
        for (j = 0; j < TUN_DQ; j++) {
            if (L->dq[j].used) {
                double d = L->dq[j].tx == 0 ? now : L->dq[j].last + gap;
                if (d < nx) nx = d;
            }
        }
        if (t->c.hold_ms > 0) {
            tmsg *m = L->rq[L->rcv_next % TUN_RQ];
            if (m && m->arr + t->c.hold_ms < nx) nx = m->arr + t->c.hold_ms;
        }
    }
    return nx;
}

static void tl_qos_update(loctun *t)
{
    double worst = 0;
    int i, any = 0;
    for (i = 0; i < TUN_MAX_LINKS; i++) {
        tlink *L = &t->links[i];
        double v;
        if (!L->used || !L->up || L->rtt_n == 0) continue;
        v = L->srtt + 4.0 * L->rttvar + (double)t->c.qos_reserve_ms;
        if (v > worst) worst = v;
        any = 1;
    }
    if (any) {
        int r = (int)(worst + 0.5);
        if (r > 4999) r = 4999;
        if (r < 1) r = 1;
        t->n->rtt_ms = r; /* XNetQosLookup meldet das (Startverzoegerung des Spiels) */
    }
}

static void tl_write_status(loctun *t, double now)
{
    FILE *f;
    char tmp[280];
    int i, up = 0, relay = 0, stuck = 0;
    double worst = 0;
    if (!t->c.status_path[0] || now - t->status_last < 2000.0) return;
    t->status_last = now;
    for (i = 0; i < TUN_MAX_LINKS; i++) {
        tlink *L = &t->links[i];
        /* W353: Mitspieler bekannt, aber nach 20 s weder direkt noch ueber ein Relay erreichbar */
        if (L->used && !L->up && L->cid && !L->blocked && L->info_at > 0 && now - L->info_at > 20000.0) stuck++;
        if (!L->used || !L->up) continue;
        up++;
        if (L->path == 2) relay++;
        if (L->srtt > worst) worst = L->srtt;
    }
    _snprintf_s(tmp, sizeof tmp, _TRUNCATE, "%s.tmp", t->c.status_path);
    if (fopen_s(&f, tmp, "w") != 0 || !f) return;
    fprintf(f, "transport=udp\nlobby=%s\nroute=%s\nroute_mode=%s\npeers=%d\nrelay_links=%d\nping_ms=%.0f\n",
            t->lb_state == LB_LOBBY ? "lobby" : (t->lb_state == LB_OFF ? (t->c.lobby[0] ? "getrennt" : "aus") : "verbinde"),
            up == 0 ? "keiner" : (relay == 0 ? "direkt" : (relay == up ? "relay" : "gemischt")),
            t->route_mode[0] ? t->route_mode : "-", up, relay, worst);
    /* W334: Sicherheitszustand und Raumcode fuer den Launcher */
    fprintf(f, "sec=%s\nlobby_code=%s\n",
            tl_sec_name(t), t->lobby_code);
    if (t->ver_state) /* W420: Versionsablehnung (Launcher-Meldung mit Link zur Release-Seite) */
        fprintf(f, "ver=%s\nneed_ver=%s\npeer_ver=%s\nown_ver=%s\n", t->ver_state == 1 ? "server" : "peer", t->need_ver,
                t->peer_ver[0] ? t->peer_ver : "-", SB_NET_VERSION);
    /* W353: Fehlergrund (dns, connect, lost, code, full, rate, path; leer = keiner) - nur Zustaende, keine Adressen */
    fprintf(f, "err=%s\n", t->lb_err[0] ? t->lb_err : (t->lb_joined && stuck ? "path" : ""));
    if (t->pub) /* W373: nur Zustand und Zahlen */
        fprintf(f, "vis=public\npub=%s\npub_games=%d\npub_answered=%ld\n",
                t->pub_host ? (t->pub_started ? "host_started" : "host") : (t->lb_joined ? "joined" : "browse"),
                t->pub_ngames, t->pub_answered);
    fclose(f);
    MoveFileExA(tmp, t->c.status_path, MOVEFILE_REPLACE_EXISTING);
}

static DWORD WINAPI tl_thread(LPVOID arg)
{
    loctun *t = (loctun *)arg;
    HANDLE hs[3];
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    hs[0] = t->ev_net;
    hs[1] = t->ev_app;
    hs[2] = t->timer;
    while (!t->stop) {
        double now, nx;
        int i;
        DWORD w;
        EnterCriticalSection(&t->cs);
        now = tl_now(t);
        if (t->c.netcut_file[0] && now - t->netcut_chk >= 100.0) { /* W366 TEST: Netz kurz weg (Marker-Datei) */
            LONG cut = GetFileAttributesA(t->c.netcut_file) != INVALID_FILE_ATTRIBUTES;
            t->netcut_chk = now;
            if (cut != t->netcut) {
                t->netcut = cut;
                tl_logf(t, "LOCTUN: TEST Netz %s", cut ? "unterbrochen (alle UDP-Pakete verloren)" : "wieder da");
            }
        }
        tl_rx_udp(t, now);
        lb_poll(t, now);
        tl_relay_pings(t, now);
        for (i = 0; i < TUN_MAX_LINKS; i++) {
            if (t->links[i].used && t->links[i].lsn != INVALID_SOCKET) tl_accept(t, i);
        }
        for (i = 0; i < TUN_MAX_STREAMS; i++) {
            if (t->streams[i].used) tl_stream_read(t, &t->streams[i]);
        }
        now = tl_now(t);
        for (i = 0; i < TUN_MAX_LINKS; i++) {
            tlink *L = &t->links[i];
            int k;
            if (!L->used) continue;
            tl_link_timers(t, L, now);
            for (k = 0; k < 16 && tl_build_send(t, L, now, 0) == 2; k++) {
            }
        }
        tl_qos_update(t);
        tl_write_status(t, now);
        nx = tl_next_wake(t, tl_now(t));
        now = tl_now(t);
        LeaveCriticalSection(&t->cs);
        if (nx <= now) continue;
        if (nx - now > 100.0) nx = now + 100.0;
        if (t->timer) {
            /* Hochaufloesender Wartezeitgeber: timeBeginPeriod wirkt unter Windows 11 fuer verborgene Prozesse
             * nicht (sonst 15,6-ms-Stufen bei den Kopien und Bestaetigungen). */
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)((nx - now) * 10000.0);
            SetWaitableTimer(t->timer, &due, 0, NULL, NULL, FALSE);
            w = (DWORD)(nx - now) + 20;
            WaitForMultipleObjects(3, hs, FALSE, w);
        } else {
            w = (DWORD)(nx - now);
            WaitForMultipleObjects(2, hs, FALSE, w ? w : 1);
        }
    }
    return 0;
}

/* ====================================================================== oeffentliche Funktionen */
void loctun_cfg_default(loctun_cfg *c)
{
    memset(c, 0, sizeof *c);
    c->proxy_base = 21000;
    c->red_max = 8;
    c->gap_ms = 15;
    c->qos_reserve_ms = 100;
    strcpy_s(c->room, sizeof c->room, "loc");
    strcpy_s(c->pilot, sizeof c->pilot, "Pilot");
    strcpy_s(c->role, sizeof c->role, "auto");
    c->route_auto = 1; /* W395 */
    c->open_hold_ms = 3000; /* W409 */
}

int loctun_available(void) { return 1; }

static void tl_add_test_peers(loctun *t)
{
    char tmp[256], *ctx = NULL, *tok;
    uint32_t idx = 0;
    strncpy_s(tmp, sizeof tmp, t->c.peers, _TRUNCATE);
    for (tok = strtok_s(tmp, ",; ", &ctx); tok; tok = strtok_s(NULL, ",; ", &ctx)) {
        char host[64];
        uint16_t port;
        struct sockaddr_in a;
        tlink *L;
        if (!tl_split_hostport(tok, host, sizeof host, &port) || inet_pton(AF_INET, host, &a.sin_addr) != 1) {
            tl_logf(t, "LOCTUN: loc_transport_peers: \"%s\" ist kein ip:port - uebergangen", tok);
            continue;
        }
        if (!locnet_ip_allowed(a.sin_addr.s_addr)) {
            tl_logf(t, "LOCTUN: loc_transport_peers: nur Loopback/privat ohne Treffpunkt - uebergangen");
            continue;
        }
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        if (idx > 0) {
            /* W334: der Testweg hat EIN festes Geheimnis (SECURITY.md §4.7) -> gleiche sid; nur ein fester Mitspieler */
            tl_logf(t, "LOCTUN: loc_transport_peers: nur ein fester Mitspieler mit SBT2 - weitere uebergangen");
            break;
        }
        L = tl_link_by_cid(t, 0x80000000u | ++idx, 1);
        if (!L) break;
        L->cid = 0; /* TEST-Verbindung ohne Treffpunkt */
        {
            /* lo/hi: kleinerer Transport-Port (bei Gleichstand kleinere Adresse) ist "lo" */
            uint32_t mine = ntohl(t->bind_ip), theirs = ntohl(a.sin_addr.s_addr);
            int lo = t->uport < port || (t->uport == port && mine < theirs);
            if ((t->uport == port && mine == theirs) || sbe2e_link_test(lo, &L->e2e) != 0 ||
                sbe2e_random(&L->probe_id, 8) != 0) {
                tl_logf(t, "LOCTUN: loc_transport_peers: Testschluessel nicht moeglich - uebergangen");
                L->used = 0;
                break;
            }
        }
        L->cand[0] = a;
        L->ncand = 1;
        if (t->c.nodirect) {
            tl_logf(t, "LOCTUN: loc_transport_nodirect ohne Treffpunkt: kein Weg");
        } else {
            L->punching = 1;
            L->punch_start = tl_now(t);
        }
    }
}

loctun *loctun_start(const loctun_cfg *c, locnet *n, locnetsim *sim, loctun_log_fn log)
{
    LARGE_INTEGER f;
    struct sockaddr_in a;
    int l = sizeof a;
    BOOL off = FALSE;
    DWORD got = 0;
    u_long nb = 1;
    int rb = 1 << 20;
    char ipb[16];
    loctun *t = (loctun *)calloc(1, sizeof *t);
    if (!t) return NULL;
    t->c = *c;
    t->n = n;
    t->sim = sim;
    t->log = log;
    t->lb_s = INVALID_SOCKET;
    QueryPerformanceFrequency(&f);
    t->qpf_ms = 1000.0 / (double)f.QuadPart;
    t->t0 = 0;
    t->t0 = tl_now(t) - 1000.0; /* Zeitstempel > 0 */
    if (t->c.route_sim[0]) tl_rsim_setup(t); /* W395 TEST */
    {
        /* W334: Start-Selbsttest der Krypto-Bausteine (SECURITY.md §4.8). Abweichung (z. B. falsche Bytereihenfolge,
         * abweichendes CNG) darf nie zu "ungeschuetzt, aber laeuft" fuehren -> Transport aus. */
        char why[96];
        if (sbe2e_selftest(why, sizeof why) != 0) {
            tl_logf(t, "LOCSEC: Krypto-Selbsttest fehlgeschlagen (%s) - Online-Spiel aus", why);
            if (t->c.status_path[0]) {
                FILE *sf;
                if (fopen_s(&sf, t->c.status_path, "w") == 0 && sf) {
                    fprintf(sf, "transport=udp\nlobby=aus\nsec=selftest_failed\n");
                    fclose(sf);
                }
            }
            free(t);
            return NULL;
        }
    }
    t->c.code[sizeof t->c.code - 1] = 0;
    t->c.spki[sizeof t->c.spki - 1] = 0;
    if (t->c.gap_ms < 1) t->c.gap_ms = 15;
    if (t->c.red_max < 1) t->c.red_max = 8;
    if (t->c.red_max > 16) t->c.red_max = 16;
    if (t->c.proxy_base < 2000 || t->c.proxy_base > 64000) t->c.proxy_base = 21000;
    n->tun_base = t->c.proxy_base; /* W373: Absender ohne Eintrag (Geister) wie Mitspieler abbilden */
    t->c.vis[sizeof t->c.vis - 1] = 0;
    t->pub = t->c.lobby[0] && !t->c.code[0] && _stricmp(t->c.vis, "public") == 0; /* W373: Code geht immer privat */
    t->pub_list_at = -1e9; /* W373: erste Suche holt die Liste sofort */
    t->pub_join = -1;
    if (t->c.lobby[0]) {
        if (!tl_split_hostport(t->c.lobby, t->lb_host, sizeof t->lb_host, &t->lb_port)) {
            tl_logf(t, "LOCLOBBY: loc_lobby=\"%s\" ist kein host:port - Treffpunkt aus", t->c.lobby);
            t->c.lobby[0] = 0;
        }
    }
    if (t->c.lobby[0]) { /* W328: Konto aus cfg.auth bzw. Umgebung SB_LOC_AUTH (danach geloescht) */
        char a[100];
        unsigned acct = 0;
        long long exp = 0;
        int off2 = 0;
        if (t->c.auth[0]) {
            strcpy_s(a, sizeof a, t->c.auth);
        } else {
            DWORD gl = GetEnvironmentVariableA("SB_LOC_AUTH", a, sizeof a);
            if (gl == 0 || gl >= sizeof a) a[0] = 0; /* W329: zu lang = Puffer ungefuellt (nicht terminiert) */
        }
        SetEnvironmentVariableA("SB_LOC_AUTH", NULL);
        SecureZeroMemory(t->c.auth, sizeof t->c.auth);
        if (a[0]) {
            if (sscanf_s(a, "%u:%lld:%n", &acct, &exp, &off2) == 2 && off2 > 0 && acct > 0 &&
                strlen(a + off2) == 64 && unhex(a + off2, t->gkey, 32)) {
                t->acct = acct;
                t->acct_exp = exp;
                t->auth_state = 1;
                tl_logf(t, "LOCLOBBY: Anmeldung mit Konto a%u", acct);
            } else {
                tl_logf(t, "LOCLOBBY: SB_LOC_AUTH ungueltig - Gast");
            }
            SecureZeroMemory(a, sizeof a);
        }
    }
    if (t->c.lobby[0] && t->c.relays[0]) { /* zusaetzliche Relays "name=host:port,..." */
        char tmp[256], *ctx = NULL, *tok;
        strncpy_s(tmp, sizeof tmp, t->c.relays, _TRUNCATE);
        for (tok = strtok_s(tmp, ", ", &ctx); tok; tok = strtok_s(NULL, ", ", &ctx)) {
            char *eq = strchr(tok, '='), host[96];
            uint16_t port;
            if (!eq || eq == tok || !tl_split_hostport(eq + 1, host, sizeof host, &port)) {
                tl_logf(t, "LOCLOBBY: loc_relays: \"%s\" ist kein name=host:port - uebergangen", tok);
                continue;
            }
            *eq = 0;
            lb_relay_set(t, tok, host, port);
        }
    }
    /* Bindung: Loopback, solange Treffpunkt und Mitspieler lokal sind, sonst alle Schnittstellen */
    t->bind_ip = htonl(INADDR_LOOPBACK);
    if (t->c.lobby[0]) {
        struct sockaddr_in la;
        if (tl_resolve(t->lb_host, t->lb_port, &la) && !locnet_ip_allowed(la.sin_addr.s_addr)) {
            t->bind_ip = INADDR_ANY;
        } else if (tl_resolve(t->lb_host, t->lb_port, &la) && (ntohl(la.sin_addr.s_addr) >> 24) != 127) {
            t->bind_ip = INADDR_ANY; /* LAN-Treffpunkt */
        }
    } else if (t->c.peers[0] && strncmp(t->c.peers, "127.", 4) != 0) {
        t->bind_ip = INADDR_ANY;
    }
    t->us = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (t->us == INVALID_SOCKET) {
        free(t);
        return NULL;
    }
    WSAIoctl(t->us, 0x9800000Cu, &off, sizeof off, NULL, 0, &got, NULL, NULL);
    setsockopt(t->us, SOL_SOCKET, SO_RCVBUF, (const char *)&rb, sizeof rb);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = t->bind_ip;
    a.sin_port = htons(t->c.port);
    if (bind(t->us, (struct sockaddr *)&a, sizeof a) != 0) {
        tl_logf(t, "LOCTUN: UDP-Port %u nicht verfuegbar (%d) - Transport aus", t->c.port, WSAGetLastError());
        closesocket(t->us);
        free(t);
        return NULL;
    }
    getsockname(t->us, (struct sockaddr *)&a, &l);
    t->uport = ntohs(a.sin_port);
    ioctlsocket(t->us, FIONBIO, &nb);
    InitializeCriticalSection(&t->cs);
    t->ev_net = CreateEventW(NULL, FALSE, FALSE, NULL);
    t->ev_app = CreateEventW(NULL, FALSE, FALSE, NULL);
    t->timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    if (!t->timer) t->timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
    WSAEventSelect(t->us, t->ev_net, FD_READ);
    if (t->c.peers[0] && t->c.lobby[0]) {
        /* W335 (NET-AUDIT N1, SECURITY.md §4.7): der Testweg hat ein festes, oeffentlich bekanntes Geheimnis - mit
         * Treffpunkt (Release-Pfad, Bindung auf alle Schnittstellen) nie aktiv */
        tl_logf(t, "LOCSEC: loc_transport_peers (Testweg) mit Treffpunkt nicht erlaubt - uebergangen");
    } else if (t->c.peers[0]) {
        char fz[24];
        DWORD fl = GetEnvironmentVariableA("SB_LOC_FUZZ_TX", fz, sizeof fz);
        tl_add_test_peers(t);
        if (fl > 0 && fl < sizeof fz && atoi(fz) > 0) { /* W335 TEST: Sende-Fuzzer nur auf dem Testweg */
            t->fz_rng = (uint32_t)atoi(fz) * 2654435761u + (uint32_t)GetCurrentProcessId();
            t->fz_on = 1;
            t->fz_rule = GetEnvironmentVariableA("SB_LOC_FUZZ_RULE", fz, sizeof fz) > 0 ? atoi(fz) : 0; /* W339 TEST: 1 = mit, 2 = nur Regel-Felder */
            tl_logf(t, "LOCFUZZ: Sende-Fuzzer aktiv (TEST, Testweg ohne Treffpunkt)%s",
                    t->fz_rule ? ", Regel-Felder gezielt (W339)" : "");
        }
    }
    timeBeginPeriod(1); /* Abstaende der Kopien (gap_ms) brauchen 1-ms-Wartezeiten */
    t->th = CreateThread(NULL, 0, tl_thread, t, 0, NULL);
    if (!t->th) {
        closesocket(t->us);
        CloseHandle(t->ev_net);
        CloseHandle(t->ev_app);
        DeleteCriticalSection(&t->cs);
        free(t);
        return NULL;
    }
    tl_logf(t, "LOCTUN: aktiv, UDP %s:%u, Stellvertreter-Basis %u, Redundanz %s (max %d, Abstand %d ms), Haltezeit %d ms, "
            "QoS-Reserve %d ms%s%s%s",
            locnet_ip_str(t->bind_ip, ipb), t->uport, t->c.proxy_base, t->c.red_fixed ? "fest" : "adaptiv",
            t->c.red_fixed ? t->c.red_fixed : t->c.red_max, t->c.gap_ms, t->c.hold_ms, t->c.qos_reserve_ms,
            t->c.lobby[0] ? ", Treffpunkt gesetzt" : "", t->c.peers[0] ? ", feste Mitspieler (TEST)" : "",
            t->c.nodirect ? ", direkt aus (TEST)" : "");
    return t;
}

void loctun_stop(loctun *t)
{
    int i, k;
    if (!t) return;
    t->stop = 1;
    SetEvent(t->ev_app);
    WaitForSingleObject(t->th, 5000);
    CloseHandle(t->th);
    timeEndPeriod(1);
    for (i = 0; i < t->nrsim; i++) {
        if (t->rsim[i].sim) locnetsim_stop(t->rsim[i].sim); /* W395 TEST */
    }
    for (i = 0; i < TUN_MAX_STREAMS; i++) {
        if (t->streams[i].used) tl_stream_free(t, &t->streams[i]);
    }
    for (i = 0; i < TUN_MAX_LINKS; i++) {
        tlink *L = &t->links[i];
        uint32_t s;
        if (L->lsn != INVALID_SOCKET && L->used) closesocket(L->lsn);
        for (k = 0; k < L->ndpx; k++) closesocket(L->dpx[k].s);
        for (s = 0; s < TUN_SQ; s++) free(L->sq[s]);
        for (s = 0; s < TUN_RQ; s++) free(L->rq[s]);
    }
    for (i = 0; i < TUN_MAX_LINKS; i++) sbe2e_link_free(&t->links[i].e2e); /* W334 */
    if (t->tls) sbtls_free(t->tls);
    if (t->lb_s != INVALID_SOCKET) closesocket(t->lb_s);
    sbe2e_kp_free(&t->kp);
    SecureZeroMemory(t->udp_key, sizeof t->udp_key);
    SecureZeroMemory(t->gkey, sizeof t->gkey); /* W328 */
    closesocket(t->us);
    CloseHandle(t->ev_net);
    CloseHandle(t->ev_app);
    if (t->timer) CloseHandle(t->timer);
    DeleteCriticalSection(&t->cs);
    free(t);
}

void loctun_bye_all(loctun *t)
{
    int i, k, n = 0;
    DWORD t0 = GetTickCount();
    if (!t) return;
    while (!TryEnterCriticalSection(&t->cs)) { /* nie haengen: der Prozess endet ohnehin */
        if (GetTickCount() - t0 > 200u) return;
        Sleep(1);
    }
    for (i = 0; i < TUN_MAX_LINKS; i++) {
        tlink *L = &t->links[i];
        uint8_t bye[1];
        if (!L->used || !L->e2e.valid || L->blocked || !L->path) continue;
        for (k = 0; k < 2; k++) {
            bye[0] = T_BYE;
            tl_send_link(t, L, L->path, bye, 1);
        }
        n++;
    }
    if (n) tl_logf(t, "LOCTUN: Client endet - BYE an %d Mitspieler", n);
    LeaveCriticalSection(&t->cs);
    if (n && t->sim) Sleep(150); /* Test-Netzmodell (loc_netsim) stellt verzoegert zu - vor dem Prozessende abwarten */
}

int loctun_dgram(loctun *t, uint32_t dst_vip, uint16_t src_gp, uint16_t dst_gp, const char *buf, int len)
{
    int i, sent = 0;
    if (!t || len <= 0 || len > TUN_PT - DHDR - 10) {
        if (t) InterlockedIncrement(&t->st.dg_drop);
        return -1;
    }
    EnterCriticalSection(&t->cs);
    if (t->pub) { /* W373: Suche (Typ 1) beantworten die Geister; Beitritt (Typ 3) an einen Geist -> Lobby beitreten */
        unsigned typ = len >= 2 ? (unsigned)(uint8_t)buf[0] | ((unsigned)(uint8_t)buf[1] << 8) : 0;
        if (dst_vip == INADDR_BROADCAST && dst_gp == 1201 && typ == 1 && len == 0x14) {
            memcpy(t->pub_nonce, buf + 4, 8);
            t->pub_t1_at = tl_now(t);
            t->pub_t1_new = 1;
            SetEvent(t->ev_app);
        } else if (dst_vip != INADDR_BROADCAST && dst_gp == 1202 && typ == 3 && len == 0x2C4 && !tl_link_by_vip(t, dst_vip)) {
            tghost *g = pub_ghost_by_vip(t, dst_vip);
            if (g) {
                if (t->pub_join < 0 || t->pub_join == (int)(g - t->gh)) {
                    t->pub_join = (int)(g - t->gh);
                    memcpy(t->pub_t3, buf, (size_t)len);
                    t->pub_t3_len = len;
                    t->pub_t3_src = src_gp;
                }
                LeaveCriticalSection(&t->cs);
                SetEvent(t->ev_app);
                return len;
            }
        }
    }
    for (i = 0; i < TUN_MAX_LINKS; i++) {
        tlink *L = &t->links[i];
        int k, slot = -1;
        uint16_t dst = dst_gp;
        if (!L->used || !L->up) continue;
        if (dst_vip != INADDR_BROADCAST && L->vip != dst_vip) continue;
        if (!(dst >= 1000 && dst <= 1999)) { /* Antwort an einen rohen Stellvertreter-Port: zurueckuebersetzen */
            for (k = 0; k < L->ndpx; k++) {
                if (L->dpx[k].local == dst) dst = L->dpx[k].gp;
            }
        }
        for (k = 0; k < TUN_DQ; k++) {
            if (!L->dq[k].used) { slot = k; break; }
        }
        if (slot < 0) {
            InterlockedIncrement(&t->st.dg_drop);
            continue;
        }
        L->dq[slot].used = 1;
        L->dq[slot].id = L->dg_next++;
        L->dq[slot].src = src_gp;
        L->dq[slot].dst = dst;
        L->dq[slot].len = (uint16_t)len;
        L->dq[slot].tx = 0;
        memcpy(L->dq[slot].data, buf, (size_t)len);
        if (t->fz_on) tl_fuzz_dgram(t, L->dq[slot].data, len); /* W335 TEST */
        InterlockedIncrement(&L->s_dgtx);
        InterlockedIncrement(&t->st.dg_tx);
        sent++;
    }
    LeaveCriticalSection(&t->cs);
    if (sent) SetEvent(t->ev_app);
    return (sent || dst_vip == INADDR_BROADCAST) ? len : -1;
}

void loctun_set_app(loctun *t, uint16_t gp, loctun_app_fn fn)
{
    if (!t) return;
    t->app_fn = NULL;
    t->app_gp = gp;
    t->app_fn = fn;
}

int loctun_version_state(loctun *t, char *need, int ncap, char *peer, int pcap)
{
    int st;
    if (!t) return 0;
    EnterCriticalSection(&t->cs);
    st = (int)t->ver_state;
    if (need && ncap > 0) strncpy_s(need, (size_t)ncap, t->need_ver, _TRUNCATE);
    if (peer && pcap > 0) strncpy_s(peer, (size_t)pcap, t->peer_ver, _TRUNCATE);
    LeaveCriticalSection(&t->cs);
    return st;
}

void loctun_set_app2(loctun *t, uint16_t gp, loctun_app_fn fn)
{
    if (!t) return;
    t->app2_fn = NULL;
    t->app2_gp = gp;
    t->app2_fn = fn;
}

int loctun_is_peer(loctun *t, uint32_t vip)
{
    int r;
    if (!t) return 0;
    EnterCriticalSection(&t->cs);
    r = tl_link_by_vip(t, vip) != NULL || (t->pub && pub_ghost_by_vip(t, vip) != NULL); /* W373: Geister */
    LeaveCriticalSection(&t->cs);
    return r;
}

int loctun_links_up(loctun *t)
{
    int i, n = 0;
    if (!t) return 0;
    EnterCriticalSection(&t->cs);
    for (i = 0; i < TUN_MAX_LINKS; i++) n += t->links[i].used && t->links[i].up;
    LeaveCriticalSection(&t->cs);
    return n;
}

int loctun_link_info(loctun *t, int idx, int *path, double *srtt_ms, int *red, uint32_t *vip, char *relay, int rlen)
{
    int i, k = 0, r = 0;
    EnterCriticalSection(&t->cs);
    for (i = 0; i < TUN_MAX_LINKS; i++) {
        tlink *L = &t->links[i];
        if (!L->used) continue;
        if (k++ != idx) continue;
        if (path) *path = L->up ? L->path : 0;
        if (srtt_ms) *srtt_ms = L->srtt;
        if (red) *red = L->red;
        if (vip) *vip = L->vip;
        if (relay && rlen > 0) {
            strncpy_s(relay, (size_t)rlen, L->relay_idx >= 0 ? t->relays[L->relay_idx].name : "", _TRUNCATE);
        }
        r = 1;
        break;
    }
    LeaveCriticalSection(&t->cs);
    return r;
}

int loctun_peer_hud_get(loctun *t, int idx, loctun_peer_hud *o) /* W374 F3-Overlay */
{
    int i, k = 0, r = 0;
    if (!t || !o) return 0;
    EnterCriticalSection(&t->cs);
    for (i = 0; i < TUN_MAX_LINKS; i++) {
        tlink *L = &t->links[i];
        if (!L->used) continue;
        if (k++ != idx) continue;
        o->up = L->up ? 1 : 0;
        o->path = L->up ? L->path : 0;
        o->srtt = L->srtt;
        o->rtt_min = L->rtt_min;
        o->rttvar = L->rttvar;
        o->loss_in = L->loss_in;
        o->loss_peer = L->loss_peer;
        strncpy_s(o->pilot, sizeof o->pilot, L->pilot, _TRUNCATE);
        /* W395: aktiver Weg und Alternativen */
        o->route[0] = 0;
        if (L->up && L->path == 1) strcpy_s(o->route, sizeof o->route, "direct");
        else if (L->up && L->path == 2 && L->relay_idx >= 0) {
            strncpy_s(o->route, sizeof o->route, t->relays[L->relay_idx].name, _TRUNCATE);
        }
        o->nalt = 0;
        o->nswitch = L->rt.nswitch;
        o->nfail = L->rt.nfail;
        o->rt_on = L->rt_on;
        o->decided = L->rt.decided;
        if (L->rt_on) {
            double now = tl_now(t);
            int s;
            for (s = 0; s < LOCROUTE_MAXP && o->nalt < LOCTUN_HUD_ALT; s++) {
                const locroute_path *p = &L->rt.p[s];
                if (!p->avail) continue;
                strncpy_s(o->alt[o->nalt].name, sizeof o->alt[0].name, s == 0 ? "direct" : t->relays[s - 1].name,
                          _TRUNCATE);
                o->alt[o->nalt].rtt = p->n ? p->srtt : -1.0;
                o->alt[o->nalt].state = locroute_alive(p, now);
                o->alt[o->nalt].active = s == L->rt.active;
                o->nalt++;
            }
        }
        r = 1;
        break;
    }
    LeaveCriticalSection(&t->cs);
    return r;
}

const loctun_stats *loctun_get_stats(loctun *t) { return &t->st; }
uint16_t loctun_port(loctun *t) { return t->uport; }
int loctun_auth_state(loctun *t)
{
    return t ? (int)t->auth_state : 0;
}

int loctun_lobby_state(loctun *t)
{
    if (!t || !t->c.lobby[0]) return 0;
    return t->lb_state == LB_OFF || t->lb_state == LB_CONNECTING ? 1 : (t->lb_joined ? 3 : 2);
}

void loctun_log_stats(loctun *t, int waited_s)
{
    int i;
    if (!t) return;
    EnterCriticalSection(&t->cs);
    for (i = 0; i < TUN_MAX_LINKS; i++) {
        tlink *L = &t->links[i];
        if (!L->used) continue;
        tl_logf(t,
                "LOCTUN: t=%ds c%u %s Weg=%s%s RTT=%.1f (min %.1f, var %.1f) ms Verlust ein=%d.%d%% Partner=%d.%d%% "
                "N=%d Pakete tx=%ld rx=%ld Nachrichten neu=%ld Kopien=%ld RTO=%ld gerettet=%ld doppelt=%ld "
                "Datagramme tx=%ld rx=%ld offen=%u",
                waited_s, L->cid, L->up ? "verbunden" : "getrennt",
                L->path == 1 ? "direkt" : (L->path == 2 ? "relay:" : "keiner"),
                L->path == 2 && L->relay_idx >= 0 ? t->relays[L->relay_idx].name : "", L->srtt, L->rtt_min,
                L->rttvar, L->loss_in / 10, L->loss_in % 10, L->loss_peer / 10, L->loss_peer % 10, L->red, L->s_tx,
                L->s_rx, L->s_new, L->s_copy, L->s_rto, L->s_saved, L->s_dup, L->s_dgtx, L->s_dgrx,
                L->snd_next - L->snd_una);
        if (L->rt_on) { /* W395: alle Wege (RTT/Schwankung/Verlust, * = aktiv) */
            char sum[256];
            tl_route_summary(t, L, tl_now(t), sum, sizeof sum);
            tl_logf(t, "LOCROUTE: t=%ds c%u Wege %s | Wechsel=%d Ausfall=%d", waited_s, L->cid, sum, L->rt.nswitch,
                    L->rt.nfail);
        }
    }
    if (t->fz_on) /* W335 TEST */
        tl_logf(t, "LOCFUZZ: t=%ds Rahmen verfaelscht=%ld Bytes=%ld Datagramme=%ld Gastfilter-Verstoesse=%ld", waited_s,
                t->fz_frames, t->fz_bytes, t->fz_dg, t->st.guard);
    LeaveCriticalSection(&t->cs);
}

int loctun_lobby_code(loctun *t, char *buf, int cap)
{
    if (!buf || cap <= 0) return 0;
    buf[0] = 0;
    if (!t) return 0;
    EnterCriticalSection(&t->cs);
    strncpy_s(buf, (size_t)cap, t->lobby_code, _TRUNCATE);
    LeaveCriticalSection(&t->cs);
    return buf[0] != 0;
}

/* W373 TEST: Steuerzeile ohne Netz einspeisen, dann einen Schritt der oeffentlichen Logik rechnen. */
int loctun_test_line(loctun *t, uint32_t my_cid, const char *line, char *out, int cap)
{
    double now;
    if (!t || !out || cap < 1) return -1;
    out[0] = 0;
    EnterCriticalSection(&t->cs);
    now = tl_now(t);
    t->test_cap = out;
    t->test_cap_len = 0;
    t->test_cap_max = cap;
    if (my_cid) {
        t->my_cid = my_cid;
        if (t->lb_state == LB_OFF) t->lb_state = LB_HELLO;
        if (!t->my_pilot[0]) strcpy_s(t->my_pilot, sizeof t->my_pilot, t->c.pilot[0] ? t->c.pilot : "Pilot");
    }
    if (line) lb_on_line(t, line, line + strlen(line), now);
    if (t->pub && t->my_cid) pub_tick(t, now);
    t->test_cap = NULL;
    LeaveCriticalSection(&t->cs);
    return t->pub;
}

int loctun_pub_games(loctun *t)
{
    int i, n = 0;
    if (!t) return 0;
    EnterCriticalSection(&t->cs);
    for (i = 0; i < TUN_MAX_GHOST; i++) n += t->gh[i].used;
    LeaveCriticalSection(&t->cs);
    return n;
}

int loctun_sec_state(loctun *t)
{
    return t ? (int)t->sec : 0;
}

int loctun_diag(loctun *t, char *buf, int cap)
{
    int i, up = 0, relay = 0, known = 0, lin = 0;
    double worst = 0;
    char rn[24] = "";
    if (!buf || cap <= 0) return 0;
    buf[0] = 0;
    if (!t) return 0;
    EnterCriticalSection(&t->cs);
    for (i = 0; i < TUN_MAX_LINKS; i++) {
        tlink *L = &t->links[i];
        if (!L->used) continue;
        known++;
        if (!L->up) continue;
        up++;
        if (L->path == 2) {
            relay++;
            if (L->relay_idx >= 0) strncpy_s(rn, sizeof rn, t->relays[L->relay_idx].name, _TRUNCATE);
        }
        if (L->srtt > worst) worst = L->srtt;
        if (L->loss_in > lin) lin = L->loss_in;
    }
    _snprintf_s(buf, (size_t)cap, _TRUNCATE,
                "meeting_point=%s lobby=%s route=%s%s%s peers=%d/%d ping_ms=%.0f loss_in=%d.%d%% sec=%s err=%s",
                t->c.lobby[0] ? "yes" : "no",
                t->lb_joined ? "joined" : (t->lb_state == LB_OFF ? "off" : "connecting"),
                up == 0 ? "none" : (relay == 0 ? "direct" : (relay == up ? "relay" : "mixed")), relay ? ":" : "",
                relay ? rn : "", up, known, worst, lin / 10, lin % 10,
                tl_sec_name(t), t->lb_err[0] ? t->lb_err : "-");
    LeaveCriticalSection(&t->cs);
    return 1;
}
