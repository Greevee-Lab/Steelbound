/* loc_tun_int.h - W464 NET-CORE: gemeinsamer interner Zustand des Transports (nur loc_tun*.c und der Fuzzer).
 * Typen, Konstanten und struct loctun wie bisher in loc_tun.c; darunter die moduluebergreifenden Funktionen.
 * Modulkarte: loc_tun.c (Thread, Zeitgeber, oeffentliche API), loc_tun_link.c (Hilfen, Stroeme, Datagramme),
 * loc_tun_wire.c (SBT2-Rahmen bauen/senden, HELLO/Version, Empfang + Pruefungen), loc_tun_lobby.c (Treffpunkt:
 * Steuerkanal, Mini-JSON, Relay/NAT-Bindung), loc_tun_pub.c (oeffentliche Lobbys). Siehe BERICHT_M2 §W464. */
#ifndef LOC_TUN_INT_H
#define LOC_TUN_INT_H
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
#include "loc_assist.h" /* W440: las_parse */
#include "loc_lan.h"    /* W454: LAN-Modus (Raumschluessel im Transkript) */
#include "loc_chat.h"   /* W446: lch_json_str_u8 (steelnet-Chat), LCH_SN_LINE_MAX */
#include <ws2tcpip.h>
#include <mmsystem.h>
#include <math.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "winmm.lib")

#define TUN_MAX_LINKS 12
#define LOCTUN_SN_LINE LCH_SN_LINE_MAX /* W446: eine ausgehende chat-Zeile */
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
#define TUN_ROUTE_SIMS 8 /* W395 TEST loc_route_sim: Wege mit eigener Verzoegerung/Ausfall (W494: auch je Mitspieler "@Prefix") */
#define TUN_RS_Q 24      /* W500: unbestaetigte eigene Steuerzeilen (Bestaetigung kommt mit jedem pong, alle 15 s) */
#define TUN_RS_LINE 4200 /* W500: wie der Sendepuffer von lb_send */
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
#define TUN_WATCH_MS 12000.0 /* W460: Abfrage der Liste fuer das Lobby-Widget hoechstens alle 12 s */
#define TUN_QOS_ONLINE_MAX 598 /* W494: online gemeldete QoS-RTT hoechstens (RttMed/2 < 300 -> Klasse 2, Beitritt klappt) */
#define LAN_MAX_PEERS 24     /* W454: bekannte LAN-Clients (Paare hoechstens TUN_MAX_LINKS) */
#define LAN_MAX_TG 4         /* W454: TEST-Ziele loc_lan_targets */
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
    lguard_bucket b_app3; /* W437: Chatkanal (eigene Bremse) */
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
    int mode;            /* W443: Lobby-Modus aus lobby_list.play_mode (LAS_SIM, LAS_ARCADE; fehlt/ungueltig = Sim) */
    uint32_t vip;        /* vip des Hosts (aus rules, geprueft) */
    char name[16];       /* Anzeigename <= 15 Zeichen */
    char pilot[24];      /* W460 LOC-LOBBY-WIDGET: Host-Pilot wie gelistet (UTF-8, ohne Steuerzeichen) */
    uint8_t sf[8], cf[2];/* Sitze/Lager/Mission/Optionen, Zaehler je Seite (geprueft) */
    SOCKET s[2];         /* vip:Stellvertreter+201 / +202 (erst beim Senden) */
    unsigned ans;        /* Suche (pub_seq), fuer die schon geantwortet wurde */
    double seen;
    int live;            /* W482 LIVE-MATCH: Host einer Live-Session (Beitritt per live_join mit live_sid) */
    uint8_t live_sid[8];
} tghost;

/* W482 LIVE-MATCH: Anfrage der XOnline-HLE an steelnet (loc_tun_live.c) */
#define TUN_LIVE_REQ 8
#define TUN_LIVE_JSON 3900
typedef struct lvreq {
    int id;              /* 0 = frei */
    int state;           /* 0 offen, 1 fertig, -1 Fehler */
    int queued, with_vip;
    double q_at, sent_at;
    char err[24];
    uint8_t sid[8], key[16];
    int nsess;
    loclive_sess *sess;  /* live_results (calloc, frei bei Abholung) */
    char json[TUN_LIVE_JSON];
} lvreq;

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
    volatile uint16_t chat_gp;       /* W437: Chatkanal (loc_chat.c), 0 = aus */
    loctun_chat_fn volatile chat_fn;
    int chat_hit;                    /* W437: dieses Paket trug Chat -> Klartextpuffer loeschen */
    /* W446 STEELNET-CHAT (Steuerkanal) */
    loctun_snchat_fn volatile sn_fn;
    volatile LONG sn_want;           /* abonnieren (loc_snchat) */
    volatile LONG sn_state;          /* loctun_snchat_state */
    int sn_sub_sent;                 /* zuletzt gesendeter Wunsch (-1 = noch keiner auf dieser Verbindung) */
    int sn_hist_done;                /* Verlauf schon angefordert (einmal je Prozess: keine Doppel nach Neuverbindung) */
    char sn_out[4][LOCTUN_SN_LINE];  /* Warteschlange UI -> Transport (unter cs) */
    int sn_out_len[4], sn_out_n;
    double lb_welcome_at;            /* W446 TEST join_delay_ms */
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
    char my_gamertag[16]; /* W487: Gamertag des Kontos aus welcome (leer = keiner gesetzt bzw. Gast) */
    char lobby_code[16];/* Raumcode der eigenen Lobby (Anzeige im Launcher) */
    volatile LONG play_mode; /* W440: Modus der aktuellen Lobby (0 keine, 1 sim, 2 arcade); liest der Eingabe-Thread */
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
    volatile LONG pub_watch; /* W460 LOC-LOBBY-WIDGET: Widget sichtbar -> Liste alle TUN_WATCH_MS frisch halten */
    double pub_list_rx;      /* W460: letzte Liste empfangen (tl_now, 0 = noch keine) */
    LONG pub_answered;
    /* W482 LIVE-MATCH (loc_tun_live.c): Xbox-Live-Modus - keine Lobby beim Start, Sessions ueber live_* */
    int live;                   /* W501: Laufzeitzustand - 1 nur, solange das Spiel im Xbox-Live-Fluss ist */
    int live_cap;               /* W501 ONLINE-BOTH: Xbox Live moeglich (loc_live=1 + Treffpunkt); Start = System Link */
    int pub0;                   /* W501: pub fuer System Link (ausserhalb von Xbox Live) */
    volatile LONG live_want;    /* W501: Wunsch des Spiel-Threads (loctun_live_mode), Netz-Thread schaltet um */
    lvreq lv_req[TUN_LIVE_REQ];
    int lv_next, lv_own;
    uint8_t lv_own_sid[8];      /* eigene Session (Keepalive) */
    double lv_keep_at;
    /* W491 CAMP-CLIENT (loc_tun_live.c): Kampagnen-Ticket (nur hier, nie im Spielspeicher) */
    char ct_camp[33];           /* Kampagnen-ID, "" = nie angefordert */
    int ct_want, ct_pending, ct_id, ct_state, ct_len; /* ct_state: 0 offen, 1 Ticket da, -1 Fehler */
    long long ct_exp;           /* gueltig bis (Unix-s) */
    double ct_sent_at, ct_retry_at;
    char ct_err[24];
    uint8_t ct_ticket[300];
    uint8_t pub_t3[0x2C4];      /* Beitrittsanfrage des eigenen Spiels (bis die Verbindung steht) */
    int pub_t3_len;
    uint16_t pub_t3_src;
    /* W395 TEST loc_route_sim: je Weg eigene Verzoegerungsstufe bzw. Ausfallfenster */
    struct { char name[24]; locnetsim *sim; double cut_a, cut_b; int cut_logged; } rsim[TUN_ROUTE_SIMS];
    int nrsim;
    int qos_capped; /* W494: QoS-Begrenzung online schon gemeldet */
    unsigned relay_usable;      /* W395: Bit i = Relay i darf gewaehlt werden (route "relays"; ohne Feld alle) */
    int relay_usable_set;
    /* W454 LOC-LAN-MODE (loc_tun_lan.c) */
    int lan, lan_loop, lan_has_pass, lan_full_logged;
    SOCKET lan_s;
    uint16_t lan_port;
    uint32_t lan_room_id;
    char lan_room[17];
    uint8_t lan_psk[32], lan_nonce[16];
    double lan_tx_at, lan_if_at;
    uint32_t lan_bc[8];          /* gerichtete Broadcast-Adressen der privaten Subnetze (Netz-Byte-Reihenfolge) */
    int lan_nbc;
    struct sockaddr_in lan_tg[LAN_MAX_TG];
    int lan_ntg;
    struct { uint32_t cid, ip; double seen, paired; int state /* 1 Paar, 2 uebergangen */, logged; uint8_t pub[32], nonce[16]; }
        lan_p[LAN_MAX_PEERS];
    lguard_bucket lan_brx, lan_breply;
    volatile LONG lan_rx, lan_bad, lan_tx, lan_ign;
    char *test_cap;             /* W373 TEST (loctun_test_line): gesendete Steuerzeilen mitschneiden */
    int test_cap_len, test_cap_max;
    struct tnetrep *nr;         /* W496 NET-TELEMETRY (loc_tun_netrep.c), NULL = Netzbericht aus */
    char lb_last_drop[12];      /* W496 Stufe 1: Code der letzten Trennung einer stehenden Anmeldung (hello "rc"), "" = keine */
    loctun_game_fn volatile game_fn;
    /* W500 LOBBY-RESUME (SECURITY.md §47): Abriss der Steuerverbindung -> dieselbe Sitzung fortsetzen */
    char rs_tok[65];            /* Fortsetzungs-Token aus dem letzten welcome (Hex), "" = Sitzung nicht fortsetzbar */
    int rs_ttl_ms;              /* Frist des Dienstes (welcome "rttl") */
    int rs_susp;                /* 1 = Verbindung weg, Sitzung (cid, E, Mitspieler) gehalten, Fortsetzung laeuft */
    int rs_try, rs_count, rs_n_ok;
    double rs_since, rs_deadline, rs_cut_next;
    uint64_t rs_rx;             /* Zeilen vom Dienst nach dem welcome (Bestaetigung "rx" an den Dienst) */
    uint64_t rs_base;           /* Folgenummer des aeltesten Eintrags in rs_q */
    int rs_qn, rs_qh;           /* Eintraege, Ringanfang (voll: aelteste verdraengt -> ggf. keine Fortsetzung) */
    char rs_q[TUN_RS_Q][TUN_RS_LINE]; /* eigene Zeilen nach dem welcome, noch nicht bestaetigt (fuer das Nachsenden) */
    int rs_ql[TUN_RS_Q];
    SOCKET rs_old_s;            /* TEST loc_lobby_cut Modus 1: liegengelassene alte Verbindung (Dienst merkt nichts) */
    sbtls *rs_old_tls;
};
/* W373: oeffentliche Lobbys - Funktionen in loc_tun_pub.c (W464: Vorausdeklarationen jetzt unten) */

/* ---- kleine Hilfen (W464: aus loc_tun.c, je Modul eingebettet) ---- */
static __inline void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static __inline void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }
static __inline uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static __inline uint32_t get32(const uint8_t *p) { return (uint32_t)get16(p) | ((uint32_t)get16(p + 2) << 16); }
static __inline void put32be(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static __inline uint32_t get32be(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static __inline const char *tl_myver(const loctun *t) { return t->c.ver_test[0] ? t->c.ver_test : SB_NET_VERSION; }

/* ---- loc_tun_link.c ---- */
void tl_logf(loctun *t, const char *fmt, ...);
double tl_now(const loctun *t);
void tl_hmac(const uint8_t *key, size_t klen, const char *msg, size_t mlen, uint8_t out[32]);
int unhex(const char *s, uint8_t *out, int n);
void tohex(const uint8_t *p, int n, char *out);
int sa_eq(const struct sockaddr_in *a, const struct sockaddr_in *b);
int tl_dest_ok(loctun *t, uint32_t ip);
int tl_resolve(const char *host, uint16_t port, struct sockaddr_in *out);
int tl_split_hostport(const char *s, char *host, int hl, uint16_t *port);
int tl_rsim_find(loctun *t, const struct sockaddr_in *a);
int tl_rsim_find_link(loctun *t, const tlink *L);
int tl_rsim_cut(loctun *t, int k);
void tl_rsim_setup(loctun *t);
void tl_sendto(loctun *t, const struct sockaddr_in *to, const uint8_t *buf, int len);
void tl_hdr(uint8_t *p, int type);
void tl_send_to(loctun *t, tlink *L, const struct sockaddr_in *to, const uint8_t *pt, int len);
void tl_send_slot(loctun *t, tlink *L, int slot, const uint8_t *pt, int len);
void tl_send_link(loctun *t, tlink *L, int path, const uint8_t *pt, int len);
tlink *tl_link_by_cid(loctun *t, uint32_t cid, int create);
tlink *tl_link_by_vip(loctun *t, uint32_t vip);
double tl_rto(const tlink *L);
void tl_rtt_sample(tlink *L, double s);
int tl_red(loctun *t, const tlink *L);
void tl_sock_events(loctun *t, SOCKET s, long ev);
void tl_nodelay(SOCKET s);
int tl_queue_msg(loctun *t, tlink *L, int sub, uint16_t stream, const uint8_t *data, int len);
tstream *tl_stream_new(loctun *t, int link, uint16_t id, SOCKET s);
void tl_stream_free(loctun *t, tstream *st);
void tl_stream_flush(loctun *t, tstream *st);
void tl_link_drop(loctun *t, tlink *L, const char *why);
/* W464: Paarbildung (aus lb_on_peer_info), Einhaengepunkt fuer LAN (W454); cand zeigt auf TUN_MAX_CAND Eintraege */
enum { TL_PAIR_OK = 0, TL_PAIR_KEY = 1, TL_PAIR_SID = 2, TL_PAIR_RAND = 3 };
int tl_link_pair(loctun *t, tlink *L, uint32_t peer, uint32_t lobby, const uint8_t pn[16], const uint8_t pub[32],
                 uint32_t acct, const char *pilot, const struct sockaddr_in *cand, int ncand);
void tl_stream_give_up(loctun *t, tstream *st);
int tl_stream_connect(loctun *t, tstream *st, int *err);
const char *tl_game_info(loctun *t, char *b, size_t cap);
void tl_stream_refused(loctun *t, tstream *st, int err);
void tl_deliver_msg(loctun *t, int li, tmsg *m);
int tl_seen_dg(tlink *L, uint16_t id);
void tl_deliver_dg(loctun *t, tlink *L, uint16_t src, uint16_t dst, const uint8_t *d, int len);

/* ---- loc_tun_wire.c ---- */
int tl_build_send(loctun *t, tlink *L, double now, int force);
void tl_ver_clean(const char *in, int n, char *out, int cap);
const char *tl_sec_name(const loctun *t);
void tl_hello(loctun *t, tlink *L, double now);
void tl_link_up(loctun *t, tlink *L);
void tl_rx_sbt(loctun *t, const uint8_t *pkt, int plen, const struct sockaddr_in *from, int via_relay_link,
                      int via_ri, double now);

/* ---- loc_tun_lobby.c ---- */
const char *js_skip(const char *p, const char *e, int depth);
const char *js_get(const char *obj, const char *e, const char *key, const char **ve);
int js_num(const char *v, const char *ve, double *out);
int js_str(const char *v, const char *ve, char *out, int cap);
const char *js_arr_first(const char *v, const char *ve);
const char *js_arr_next(const char *el, const char *ve, const char **elend);
const char *lb_my_play_mode(const loctun *t);
void lb_play_mode_set(loctun *t, int mode, const char *why);
void lb_send(loctun *t, const char *json);
void tl_js_esc(const char *in, char *out, size_t cap);
void lb_relay_set(loctun *t, const char *name, const char *host, uint16_t port);
void lb_report(loctun *t);
void lb_snchat_tick(loctun *t);
void lb_on_line(loctun *t, const char *m, const char *e, double now);
void lb_poll(loctun *t, double now);
void lb_rs_reset(loctun *t);                          /* W500: Fortsetzungszustand loeschen */
void lb_lost(loctun *t, const char *why, double now); /* W500: Abriss -> fortsetzen oder lb_close */
int tl_bind_want(loctun *t, const tlink *L, int ri);
void tl_tbind_confirm(loctun *t, tlink *L, int ri);
void lb_rx_sbn(loctun *t, const uint8_t *p, int len, const struct sockaddr_in *from, double now);

/* ---- loc_tun_lan.c (W454) ---- */
int lan_targets(loctun *t);
int lan_start(loctun *t);
void lan_poll(loctun *t, double now);
void lan_stop(loctun *t);
int lan_peers(const loctun *t, int *up);
/* ---- loc_tun_pub.c ---- */
void pub_ghosts_clear(loctun *t);
tghost *pub_ghost_by_vip(loctun *t, uint32_t vip);
void pub_on_list(loctun *t, const char *m, const char *e, double now);
void pub_on_state(loctun *t, const char *m, const char *e);
void pub_reject(loctun *t, int gi, const char *why, double now);
void pub_on_link_up(loctun *t, tlink *L);
void pub_tick(loctun *t, double now);
void pub_ghosts_close_one(tghost *g); /* W482 */
/* W482 LIVE-MATCH (loc_tun_live.c) */
void lv_reset(loctun *t, const char *why);
void lv_on_line(loctun *t, const char *typ, const char *m, const char *e, double now);
int lv_on_error(loctun *t, const char *m, const char *e);
void lv_tick(loctun *t, double now);
void pub_live_mode_tick(loctun *t, double now); /* W501: System Link <-> Xbox Live umschalten (Netz-Thread, unter cs) */
/* W496 NET-TELEMETRY (loc_tun_netrep.c): alle unter t->cs, ohne Zustimmung (t->nr == NULL) sofort zurueck */
const char *tl_drop_code(const char *why); /* Trennungsgrund -> fester Code (closed/timeout/tls/refused/version/error) */
void nr_start(loctun *t);
void nr_free(loctun *t);
void nr_on_welcome(loctun *t, const char *m, const char *e, double now);
void nr_on_drop(loctun *t, const char *why, double now);
void nr_on_host_left(loctun *t);
void nr_tick(loctun *t, double now);
void nr_exit(loctun *t);

#endif /* LOC_TUN_INT_H */
