/* loc_camp_bridge.h - W491 CAMP-CLIENT: Kampagnen-Bruecke der Netz-HLE (Loopback -> TLS zum Steelbound-Zugang).
 *
 * Nur im Xbox-Live-Modus mit Treffpunkt. Eigener Netz-Thread; der Spiel-Thread wartet nie (connect/recv/send des
 * Spiels laufen auf echten, nicht blockierenden Loopback-Sockets). Je Kampagnen-Transaktion des Spiels eine eigene
 * Verbindung: TLS (Pin) -> Vorspann "SBGW" + Ticket -> Original-Protokoll unveraendert in beide Richtungen.
 * Doku: docs/loc/LOC_CAMPAIGN.md "Client-Weg (W491)". */
#pragma once
#include <winsock2.h>
#include <stdint.h>
#include "loc_camp.h"

struct loctun;

typedef struct loccamp_cfg {
    char host[128];     /* Zugang (Hostname), "" = SB_STEELNET_HOST */
    uint16_t port;      /* 0 = nach Kampagne (47100 Saison, 47103 Test) */
    char campaign[33];  /* Kampagnen-ID fuer das Ticket (steelbound-s1 / steelbound-test) */
    char spki[65];      /* zusaetzlicher Pin, NUR fuer ein Loopback-Ziel (lokaler Test-Zugang); "" = keiner */
} loccamp_cfg;

typedef void (*loccamp_log_fn)(const char *msg);

/* Startet Ticketanforderung, Bereitschaftspruefung und Bruecken-Thread. 1 = laeuft. */
int loccamp_bridge_start(struct loctun *t, const loccamp_cfg *c, loccamp_log_fn log);
void loccamp_bridge_stop(void);
/* LOCCAMP_ST_* (OFF, solange nicht gestartet) */
int loccamp_bridge_state(void);
/* connect()-Umlenkung im Netz-HLE: s = Spiel-Socket, dst = Ziel des Spiels. 1 = Ziel war die virtuelle
 * Kampagnenadresse und wurde ersetzt (dst = 127.0.0.1:Brueckenport; s ist an 127.0.0.1 gebunden und bei der Bruecke
 * angemeldet). 0 = kein Kampagnenziel. -1 = Kampagnenziel, aber Bruecke nicht bereit (connect scheitert). */
int loccamp_bridge_redirect(SOCKET s, struct sockaddr_in *dst);
/* Zaehler fuer Log/Selbsttest: Verbindungen, erfolgreiche Vorspaenne, Ablehnungen, Fehler. */
void loccamp_bridge_counts(long *conns, long *ok, long *refused, long *failed);
