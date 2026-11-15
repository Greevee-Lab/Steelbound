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
} locnet;

/* 1 = Loopback (127/8), privat (10/8, 172.16/12, 192.168/16) oder link-local (169.254/16). */
int locnet_ip_allowed(uint32_t ip_net);

/* addr: "auto" oder IPv4; peers: "auto" oder "ip[:basis],..."; base 1000..64000.
 * Rueckgabe 1 = bereit. Ruft WSAStartup selbst. */
int locnet_init(locnet *n, const char *addr, int base, const char *peers, int rtt_ms, locnet_log_fn log);
void locnet_shutdown(locnet *n);

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
int locnet_recvfrom(locnet *n, SOCKET s, char *buf, int len, int flags, struct sockaddr *from, int *fromlen);

/* XNADDR (36 B): ina, inaOnline, wPortOnline, abEnet[6], abOnline[20]. */
void locnet_xnaddr(const locnet *n, uint32_t ip_net, uint8_t out[LOCNET_XNADDR_SIZE]);
uint32_t locnet_xnaddr_ip(const uint8_t xna[LOCNET_XNADDR_SIZE]);
void locnet_create_key(uint8_t kid[8], uint8_t key[16]);
void locnet_random(uint8_t *p, unsigned len);

/* XNQOS-Block fuer cnt Ziele (alle "erreicht", RTT = rtt_ms), Freigabe mit locnet_qos_free. */
void *locnet_qos_alloc(const locnet *n, unsigned cnt, unsigned probes);
void locnet_qos_free(void *q);

const char *locnet_ip_str(uint32_t ip_net, char buf[16]);
