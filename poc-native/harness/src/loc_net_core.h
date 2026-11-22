/* loc_net_core.h - W286 LOC-SYSLINK: Netz-Kern fuer Line of Contact "System Link".
 *
 * Bildet die Xbox-Winsock-/XNet-Aufrufe des Spiels auf echte Host-Sockets ab:
 *  - Adresse = Identitaet: jede Instanz hat eine eigene IPv4 (Standard: erste freie
 *    aus 127.0.0.2-127.0.0.9), die auch in der XNADDR steht. Zwei Clients auf einem
 *    Rechner finden sich so ohne Port-Umrechnung im Spiel.
 *  - Ports: Spielport 1000-1999 -> Basis + (Port - 1000), Basis je Instanz/Mitspieler.
 *  - Broadcast (255.255.255.255) -> Unicast an jeden Mitspieler der Liste.
 *  - Nur Loopback/private/link-local Adressen; fremde Absender werden verworfen.
 * Ohne Bezug auf Spielspeicher (testbar: loc_net_selftest). */
#pragma once
#include <winsock2.h>
#include <stdint.h>

#define LOCNET_MAX_PEERS 16
#define LOCNET_XNADDR_SIZE 36

typedef struct locnet_peer {
    uint32_t ip;   /* Netzwerk-Byte-Reihenfolge */
    uint16_t base; /* Portbasis des Mitspielers */
} locnet_peer;

typedef struct locnet_stats {
    volatile LONG tx, rx, bc, drop, sockets, accepts, connects;
    volatile LONG guard; /* W334: vom Gastfilter in den HLE-Hooks verworfen */
    volatile LONG hold;  /* W334: recv mit unentschiedenem Rahmenkopf (WSAEWOULDBLOCK, nur Zaehler) */
    volatile LONG leave; /* W350: Stromende eines Mitspielers dem Spiel als Verbindungsreset gemeldet (Abgaenge) */
    volatile LONG last34_ms; /* W297: GetTickCount des letzten gesendeten 0x34-Rahmens (Lockstep aktiv) */
    volatile LONG typ_seen[2][8]; /* W297: Rahmentyp < 256 gesehen (Bit), [0] empfangen, [1] gesendet - nur Typ, kein Inhalt */
} locnet_stats;

typedef void (*locnet_log_fn)(const char *msg);

typedef struct locnet {
    int ready;
    uint32_t self_ip; /* Netzwerk-Byte-Reihenfolge */
    uint16_t base;
    int auto_addr;
    locnet_peer peers[LOCNET_MAX_PEERS];
    int npeers;
    SOCKET claim; /* belegt <self>:<basis+200> exklusiv (Doppelstart-Schutz) */
    uint8_t kid[8], key[16];
    int key_set;
    int rtt_ms;
    locnet_stats st;
    locnet_log_fn log;
    struct locnetsim *sim; /* W322 LOC-NETLAT: Verzoegerungsstufe (NULL = aus, Standard) */
    int fin_pass;          /* W350: 0 = Stromende eines Mitspielers -> WSAECONNRESET (Standard), 1 = recv 0 durchreichen */
    int nodelay;           /* W322: 1 = TCP_NODELAY auf den Spiel-TCP-Verbindungen (kein Nagle-Stau) */
    struct loctun *tun;    /* W324 LOC-NETPLAY: eigener UDP-Transport (NULL = Original-TCP/UDP, Standard) */
    /* W329: Host-UDP-Ports, die das Spiel an der eigenen Adresse gebunden hat (Bit je Port). Der Transport stellt
     * Datagramme von Mitspielern nur an diese Ports zu - nie an fremde Dienste auf Loopback. */
    volatile LONG udp_bound[65536 / 32];
    /* W409 LOC-MESH-KICK: Lauschsocket des Spiels (TCP 1000), 0 = keiner - der Transport nennt im Log, ob das Spiel bei
     * einem abgelehnten Strom ueberhaupt lauschte (Spiellogik) oder der connect trotz Lauschen scheiterte (Umgebung) */
    volatile LONG tcp_listen;
    /* W373 LOC-PUBLIC-LOBBY: Host-Zustand des Spiels fuer oeffentliche Lobbys (Spiel-Thread schreibt, Transport liest;
     * nur Werte): [0] S[0..3], [1] S[4..7] (G+0x5624: Lager A/B, Sitze A/B, Modus, Mission, Bestaetigung, Option),
     * [2] Bit 0-7 Lobby-Zustand G+0x15, 8-15 / 16-23 Spieler je Seite (G+0xA4/A5), Bit 24 gueltig. */
    volatile LONG pub_w[3];
    uint16_t tun_base; /* W373: Portbasis fuer Absender ohne Eintrag im Transportbetrieb (= Stellvertreter-Basis) */
    /* W420 LOC-CLIPDUMP: Ziel der letzten eigenen Beitrittsanfrage des Spiels (UDP 1202 Typ 3, Adresse wie im Spiel,
     * Netzwerk-Byte-Reihenfolge) und GetTickCount|1 dazu, 0 = keine. Der Host einer Mitspielergruppe ist fuer den
     * Beitretenden genau diese Adresse (Match-Regel des Hosts, loc_rule.c). Nur Adresse und Zeit, kein Inhalt. */
    volatile LONG join_ip, join_ms;
} locnet;

/* W339: 1 = Daten eines Mitspielers haben das Spiel erreicht (Netzsitzung, bis Prozessende). */
extern volatile LONG g_locnet_session;
/* W339: Zustandspruefung fuer UDP-Rahmen nach bestandenem Gastfilter (1 = zum Spiel, 0 = verwerfen). from_ip =
 * Absender, wie ihn das Spiel sieht (Netzwerk-Byte-Reihenfolge). NULL = keine (Selbsttest). */
typedef int (*locnet_udp_state_fn)(uint16_t game_port, const uint8_t *d, int n, uint32_t from_ip);
void locnet_set_udp_state_check(locnet_udp_state_fn f);
/* W339: Spiel-TCP-Socket wegen eines Verstosses trennen (wie der Gastfilter: shutdown, recv meldet WSAECONNRESET). */
void locnet_guard_kill(locnet *n, SOCKET s, int why);

/* 1 = Loopback (127/8), privat (10/8, 172.16/12, 192.168/16) oder link-local (169.254/16). */
int locnet_ip_allowed(uint32_t ip_net);

/* addr: "auto", "random" (W324: zufaellige 127.64-127.127.x.y, eindeutig ueber Rechner hinweg) oder IPv4;
 * peers: "auto", "none" (W324: Liste kommt vom Transport) oder "ip[:basis],..."; base 1000..64000.
 * Rueckgabe 1 = bereit. Ruft WSAStartup selbst. */
int locnet_init(locnet *n, const char *addr, int base, const char *peers, int rtt_ms, locnet_log_fn log);
void locnet_shutdown(locnet *n);
/* W324: Mitspieler zur Laufzeit ergaenzen (Transport: vip mit Stellvertreter-Basis). Thread-sicher fuer
 * gleichzeitige Leser (Eintrag erst schreiben, dann Zahl erhoehen). Vorhandene ip: Basis aktualisieren. */
void locnet_peer_add(locnet *n, uint32_t ip, uint16_t base);

/* W329: 1 = das Spiel hat UDP <self_ip>:host_port gebunden (locnet_bind bzw. implizit beim ersten sendto). */
int locnet_udp_bound(const locnet *n, uint16_t host_port);

/* Portabbildung (Host-Byte-Reihenfolge). */
uint16_t locnet_port_out(const locnet *n, uint32_t ip_net, uint16_t game_port);
uint16_t locnet_port_in(const locnet *n, uint32_t ip_net, uint16_t host_port);

/* Winsock mit Spiel-Semantik (Xbox-Strukturen = Win32-Strukturen). */
SOCKET locnet_socket(locnet *n, int af, int type, int proto);
int locnet_closesocket(locnet *n, SOCKET s);
void locnet_note_stream(locnet *n, SOCKET s, int out, const char *buf, int len); /* TCP recv/send: Typ-Erstsichtung */
int locnet_setsockopt(locnet *n, SOCKET s, int level, int opt, const char *val, int len);
int locnet_bind(locnet *n, SOCKET s, const struct sockaddr *a, int len);
int locnet_connect(locnet *n, SOCKET s, const struct sockaddr *a, int len);
SOCKET locnet_accept(locnet *n, SOCKET s, struct sockaddr *a, int *len);
int locnet_sendto(locnet *n, SOCKET s, const char *buf, int len, int flags, const struct sockaddr *to, int tolen);
int locnet_send(locnet *n, SOCKET s, const char *buf, int len, int flags); /* W322: TCP send (ueber sim, falls aktiv) */
int locnet_recvfrom(locnet *n, SOCKET s, char *buf, int len, int flags, struct sockaddr *from, int *fromlen);
/* W334: recv des Spiels durch den Gastfilter (TCP: nur Bytes mit entschiedenem gueltigem Rahmenkopf, Verstoss =
 * Verbindung getrennt; UDP: Positivliste). */
int locnet_recv(locnet *n, SOCKET s, char *buf, int len, int flags);
/* W334: 1 = eigene Adresse oder verbundener Mitspieler (XNet-HLE gibt nur solche heraus). */
int locnet_addr_known(locnet *n, uint32_t ip_net);

/* XNADDR (36 B): ina, inaOnline, wPortOnline, abEnet[6], abOnline[20]. */
void locnet_xnaddr(const locnet *n, uint32_t ip_net, uint8_t out[LOCNET_XNADDR_SIZE]);
uint32_t locnet_xnaddr_ip(const uint8_t xna[LOCNET_XNADDR_SIZE]);
void locnet_create_key(uint8_t kid[8], uint8_t key[16]);
void locnet_random(uint8_t *p, unsigned len);

/* XNQOS-Block fuer cnt Ziele (alle "erreicht", RTT = rtt_ms), Freigabe mit locnet_qos_free. */
void *locnet_qos_alloc(const locnet *n, unsigned cnt, unsigned probes);
void locnet_qos_free(void *q);

const char *locnet_ip_str(uint32_t ip_net, char buf[16]);
