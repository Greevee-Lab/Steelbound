/* loc_netsim.h - W322 LOC-NETLAT: Verzoegerungs-/Jitter-Stufe im LoC-Netz-HLE (DIAG/TEST).
 *
 * Bildet eine Internet-Strecke zwischen zwei System-Link-Clients nach, ohne das Spiel zu
 * veraendern: Jeder Client verzoegert seine AUSGEHENDEN Daten (TCP 1000 und UDP) um die halbe
 * Rundlaufzeit plus Jitter; beim Empfaenger meldet select/recv sie dadurch erst nach Ablauf.
 * Zwei Clients ergeben so zusammen die volle Rundlaufzeit (wie profiles.json: je Richtung rtt/2).
 *  - Warteschlange mit Ankunftszeit je Sendung, ein Sende-Thread stellt sie zur Ankunftszeit zu.
 *  - TCP: Reihenfolge bleibt erhalten (Ankunft = max(eigene, vorige)), Jitter sortiert nie um.
 *    Verlust = Neuuebertragung nach RTO (max(200 ms, RTT + 4*Jitter), verdoppelt je Wiederholung,
 *    wie W247 lockstep_model), dahinter wartet der Strom (Head-of-Line). Neuuebertragungen ziehen
 *    mit der mittleren Verlustrate (ein Burst ist nach >= 200 ms vorbei).
 *    Nagle-Modell (Standard an, wie ein unveraenderter TCP-Stapel ohne TCP_NODELAY): kleine
 *    Sendungen warten, solange ein Segment unbestaetigt ist (bis Sendezeit + RTT), und werden
 *    dann zusammen verschickt.
 *  - UDP: Verlust = verwerfen; Umordnung nur, wenn das Profil "reorder" erlaubt.
 *  - Bandbreite optional (Serialisierung je Sendung inkl. 40 B Kopf).
 * Steuerung ueber einen Schluessel loc_netsim (Standard leer = aus), Syntax siehe locnetsim_parse.
 * Kein Spielspeicher, keine Inhalte im Log (testbar: loc_net_selftest). */
#pragma once
#include <winsock2.h>
#include <stdint.h>

typedef void (*locnetsim_log_fn)(const char *msg);

#define LOCNETSIM_RAMP_MAX 12

typedef struct locnetsim_cfg {
    char name[32];       /* Profilname (Log) */
    double rtt_ms;       /* Rundlaufzeit; je Richtung die Haelfte */
    double jitter_ms;    /* Sigma der Halbnormalverteilung je Richtung */
    double loss_pct;     /* unabhaengiger Verlust je Sendung in Prozent */
    double ge_p_gb, ge_p_bg, ge_loss_bad; /* Gilbert-Elliott (0 = aus) */
    double bw_kbps;      /* 0 = unbegrenzt */
    double uloss_pct;    /* UDP-Verlust in Prozent statt loss/ge (nur mit uloss_set) */
    int uloss_set;
    int reorder;         /* UDP darf umsortiert werden (TCP nie) */
    int nagle;           /* 1 = Nagle-Modell fuer TCP, 0 = wie TCP_NODELAY */
    int nagle_set;       /* nagle ausdruecklich gesetzt (sonst folgt es loc_net_nodelay) */
    unsigned seed;       /* 0 = fest 1 (wiederholbar) */
    int ramp_n;          /* Stufen relativ zum Lockstep-Start: ab ramp_s[i] Sekunden RTT ramp_rtt[i] */
    int ramp_s[LOCNETSIM_RAMP_MAX];
    int ramp_rtt[LOCNETSIM_RAMP_MAX];
} locnetsim_cfg;

typedef struct locnetsim_stats {
    volatile LONG queued, sent, closes, tcp_retx, udp_lost, nagle_held, send_err, overflow;
    volatile LONG q_bytes, q_bytes_max;
    LONG lat_max_ms;     /* groesste Verweildauer (Sendeaufruf -> Zustellung) in ms */
} locnetsim_stats;

struct locnetsim; /* intern */
typedef struct locnetsim locnetsim;

/* Eingebaute Profile = Spiegel von poc-native/net/netsim/profiles.json (W247; loc_net_selftest
 * vergleicht beide) plus Kurznamen: dsl = eu-eu, dsl-fern = eu-useast, uebersee = eu-jp,
 * wlan = mobile-bad. Rueckgabe 1 = gefunden. */
int locnetsim_profile(const char *name, locnetsim_cfg *out);
/* Zahl der eingebauten Profile (ohne Kurznamen) und Name/Werte des i-ten (Selbsttest). */
int locnetsim_profile_count(void);
const char *locnetsim_profile_name(int i);

/* Spezifikation: kommagetrennte Teile, ein Teil ohne ':' ist ein Profilname, sonst k:v mit
 * rtt, jitter, loss, uloss (UDP-Verlust getrennt, z. B. uloss:0 fuer deterministische Suche/Beitritt),
 * bw, nagle, reorder, seed und ramp (Stufen "s>rtt/s>rtt/...", z. B.
 * ramp:60>800/120>1600). Beispiele: "dsl", "eu-jp,nagle:0", "rtt:300,jitter:20".
 * Rueckgabe 1 = gueltig und aktiv; Fehlertext in err. */
int locnetsim_parse(const char *spec, locnetsim_cfg *out, char *err, int errlen);

/* Start (Thread) / Stopp (stellt Rest sofort zu, beendet den Thread). */
locnetsim *locnetsim_start(const locnetsim_cfg *c, locnetsim_log_fn log);
void locnetsim_stop(locnetsim *s);

/* Sendung einreihen. to = NULL fuer verbundene Sockets (TCP). Rueckgabe = len (angenommen),
 * SOCKET_ERROR mit WSAEWOULDBLOCK bei voller Warteschlange bzw. mit dem gemerkten Fehler
 * einer frueheren Zustellung (z. B. WSAECONNRESET). */
int locnetsim_send(locnetsim *s, SOCKET sk, int is_tcp, const char *buf, int len, int flags,
                   const struct sockaddr_in *to);
/* closesocket: offene Sendungen werden noch zugestellt, dann geschlossen (wie ein geordnetes
 * TCP-Ende). Rueckgabe 1 = verschoben (Socket schliesst der Thread), 0 = sofort schliessen. */
int locnetsim_close(locnetsim *s, SOCKET sk);

/* Lockstep-Start (GetTickCount-Zeitpunkt) fuer ramp; aktuelle RTT. */
void locnetsim_mark_start(locnetsim *s, DWORD tick_ms);
double locnetsim_rtt_now(locnetsim *s);
const locnetsim_cfg *locnetsim_config(const locnetsim *s);
const locnetsim_stats *locnetsim_get_stats(const locnetsim *s);
/* Verweildauer-Perzentil (0..100) in ms aus den 10-ms-Klassen (Obergrenze der Klasse). */
int locnetsim_lat_pct(const locnetsim *s, int pct);
/* Offene Sendungen (Selbsttest). */
int locnetsim_pending(locnetsim *s);
