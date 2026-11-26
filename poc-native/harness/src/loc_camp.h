/* loc_camp.h - W491 CAMP-CLIENT: LoC-Kampagne im Client (Kern ohne Spielspeicher und ohne Netz, testbar).
 *
 * Das Spiel holt sich ueber XOnlineGetServiceInfo(0x43430009) Adresse und Port des Kampagnenservers und baut je
 * Transaktion eine kurze TCP-Verbindung auf (LOC_RE1.md §5). Wir melden eine VIRTUELLE Adresse; die Netz-HLE lenkt
 * connect() zu dieser Adresse auf eine Loopback-Bruecke um (loc_camp_bridge.c). Die Bruecke oeffnet TLS zum
 * Steelbound-Zugang des Kampagnenservers (Pin), sendet den Vorspann mit dem Kampagnen-Ticket und reicht danach die
 * Rahmen des Original-Protokolls unveraendert durch. Das Spiel sieht nie die echte Adresse, einen Schluessel oder
 * das Ticket. Doku: docs/loc/LOC_CAMPAIGN.md "Client-Weg (W491)". */
#ifndef LOC_CAMP_H
#define LOC_CAMP_H

#include <stddef.h>
#include <stdint.h>

/* Virtuelle Dienstadresse 127.250.67.9 (Host-Reihenfolge; ausserhalb der Instanz- und Transport-vips 127.0.0.2-9 und
 * 127.64-127.127) und Spielport. Der Port steht in XONLINE_SERVICE_INFO in Host-Reihenfolge (das Spiel ruft htons,
 * FUN_0005D1xx), die Adresse in Netz-Reihenfolge (wird unveraendert in sockaddr_in kopiert). */
#define LOCCAMP_VIP 0x7FFA4309u
#define LOCCAMP_GAME_PORT 23457u

/* Steelbound-Zugang (CAMP-GATEWAY, W490): Produktivsaison 47100, Testkampagne 47103, ein Pin fuer beide. */
#define LOCCAMP_PORT_SEASON 47100u
#define LOCCAMP_PORT_TEST 47103u
#define LOCCAMP_ID_SEASON "steelbound-s1"
#define LOCCAMP_ID_TEST "steelbound-test"
#define LOC_CAMP_BETA_MIN_SIDE 1 /* Beta 0.6.0 (Nutzer 25.11.): Kampagnenmission ab 1 Pilot je Seite; Original 3 (loc_camp_min_side=0) */

#define LOCCAMP_TICKET_MIN 129
#define LOCCAMP_TICKET_MAX 285
#define LOCCAMP_PRE_HEAD 8 /* "SBGW", Version, Flags, u16 Ticketlaenge (Big-Endian) */
#define LOCCAMP_PRE_MAX (LOCCAMP_PRE_HEAD + LOCCAMP_TICKET_MAX)
#define LOCCAMP_GW_REPLY 6 /* "SBGW", Version, Status */
#define LOCCAMP_FRAME_MAX 0x1400 /* Nutzlast je Rahmen [u16 len][u16 cmd][payload] (LOC_RE1 §5) */
#define LOCCAMP_RENEW_S 300      /* Ticket so lange vor Ablauf erneuern */

/* Status der Zugangsantwort */
enum { LOCCAMP_GW_OK = 0, LOCCAMP_GW_REFUSED = 1, LOCCAMP_GW_BUSY = 2, LOCCAMP_GW_UNSUPPORTED = 3 };

/* Vorspann bauen. Rueckgabe Laenge bzw. -1 (Ticket ausserhalb 129..285 Byte, Puffer zu klein). */
int loccamp_preamble(const uint8_t *ticket, int tlen, uint8_t *out, int cap);
/* Antwort des Zugangs: 0..3 = Status, -2 = noch unvollstaendig (n < 6), -1 = kaputt (falsche Magie/Version,
 * unbekannter Status). */
int loccamp_gw_status(const uint8_t *r, int n);
/* Rahmenpruefung Server -> Spiel: > 0 = Laenge des ersten vollstaendigen Rahmens (Kopf + Nutzlast), 0 = noch
 * unvollstaendig, -1 = Nutzlast > 0x1400 (Verbindung trennen). */
int loccamp_frame(const uint8_t *b, int n);
/* base64url ohne '=' (wie steelnet) -> Bytes. Rueckgabe Laenge bzw. -1 (ungueltiges Zeichen, Puffer zu klein). */
int loccamp_b64url_dec(const char *in, uint8_t *out, int cap);
/* Grobe Ticketpruefung im Client (die Signatur prueft nur der Server): Groesse, Magie "SBCT", Version 1, Konto-ID
 * = angemeldetes Konto, Pseudo-XUID passt. exp = "gueltig bis" aus dem Ticket. 1 = plausibel. */
int loccamp_ticket_ok(const uint8_t *t, int n, uint32_t account, long long *exp);
/* 1 = neues Ticket holen (keins da, oder Ablauf in weniger als LOCCAMP_RENEW_S Sekunden). */
int loccamp_renew_due(long long now, long long expires, int have);

/* Zustand der Kampagne im Client */
enum {
    LOCCAMP_ST_DOWN = -1,   /* nicht verfuegbar (Ticket abgelehnt/aus, Zugang nicht erreichbar, Pin falsch) */
    LOCCAMP_ST_OFF = 0,     /* abgeschaltet (loc_camp=0, kein Live-Modus, Notfall "Kampagne aus") */
    LOCCAMP_ST_PENDING = 1, /* wird geprueft (Ticket + Zugang) */
    LOCCAMP_ST_READY = 2    /* Ticket da, Zugang erreichbar (Pin bestaetigt) */
};
/* XOnlineGetServiceInfo(0x43430009): READY -> S_OK, sonst 0x80151102 ("temporarily unavailable", Campaign grau). */
uint32_t loccamp_service_hr(int state);
/* XONLINE_SERVICE_INFO (12 B): dwServiceID, IN_ADDR (Netz-Reihenfolge), WORD Port (Host-Reihenfolge), WORD 0. */
void loccamp_service_info(uint8_t out[12]);
/* 1 = Ziel ist die virtuelle Kampagnenadresse (ip_net, port_net wie in sockaddr_in). */
int loccamp_is_vip(uint32_t ip_net, uint16_t port_net);
/* Auswahl -> Kampagnen-ID und Zugangsport: "test" -> steelbound-test/47103, sonst steelbound-s1/47100. */
const char *loccamp_campaign_id(const char *sel, uint16_t *port);
/* Logon-Entscheidung: 1 = Logon darf abgeschlossen werden (Kampagne entschieden oder Wartezeit vorbei). */
#define LOCCAMP_LOGON_WAIT_MS 12000u /* Ticket + TLS + Vorspann; danach Logon ohne Kampagne (grau) */
int loccamp_logon_ready(int state, uint32_t waited_ms, uint32_t max_ms);
const char *loccamp_state_name(int state);

/* W492 CAMP-E2E: Reinforcements-Bild (FUN_00118E50, Aufrufer 0x1191D5/0x1192E3). Das Spiel sucht per Pfeiltaste das
 * naechste gesetzte Bit in Maske = Shop-VTs mit Limit != 0 ([[0x722E70]+0x490]) | Garage-VTs ([+0x494]); bei Maske 0
 * endet die Suche nie. Auch mit gueltigen Serverdaten moeglich (Lager ohne VT im Shop dieses Turns, z. B. RB vor
 * Turn 3 / JAR vor Turn 5, Bestand ausverkauft, eigene VTs verloren). 1 = Suche nicht starten (Cursor bleibt). */
int loccamp_reinf_blocked(uint32_t mask);
/* Nachbildung der Suche des Spiels (fuer den Selbsttest): neuer Index in *out, Rueckgabe 1 = geaendert, 0 = gleich,
 * -1 = das Original kaeme nicht zurueck (Maske 0; hier nach 64 Schritten abgebrochen). step +1/-1. */
int loccamp_reinf_next(uint16_t cur, uint32_t mask, int step, uint16_t *out);

/* W515 FACTIONSWAP (#110): HQ-Menue der Kampagne (FUN_000EF6B0, Zustand 0). Fuer einen Piloten mit Seite (Pilot+0x3B
 * != 4, 4 = neutral) sperrt das Spiel ENLISTMENT OFFICE (Menue-ID 0x1E0), wenn der Zug ausserhalb 3..6 liegt:
 *   0xEF737 cmp ax,3 / jb 0xEF743 / cmp ax,6 / jbe 0xEF754 / 0xEF743 push 1,1,0x1E0 / call FUN_000E1350
 * Patch (loc_camp_faction_swap=1): jb -> jmp 0xEF754 (dasselbe Ziel wie "Zug 3..6"). */
#define LOCCAMP_SWAP_VA 0xEF737u
extern const uint8_t loccamp_swap_orig[12];
extern const uint8_t loccamp_swap_new[12];
/* Nachbildung: 1 = Enlistment Office im HQ grau. campaign = [[0x278850]+0x14]==0, faction = Pilot+0x3B (0..3, 4
 * neutral), turn = aktueller Zug (FUN_000DE460), swap = Patch aktiv. Ohne Kampagne ist der Punkt immer grau
 * (FUN_000EF4A0), neutral immer offen (nur dort kann man sich einschreiben). */
int loccamp_enlist_locked(int campaign, int faction, int turn, int swap);
/* Nach einem erfolgreichen Wechsel (SetUserFaction-Status 0) verlaesst das Spiel das Enlistment Office mit
 * FUN_00112730: Flag [0x6DF450]+0x14 gesetzt -> Bildschirm 8 (MAIN MENU), sonst 0xC (HQ). Im Pilotendatensatz steht
 * dann noch die Garage der alten Seite (Pilot+0x3C, 60 x 8 B; MISSION SELECT bleibt so faelschlich frei). Patch:
 * Bildschirm 3 (CAMPAIGN-Menue, FUN_00110740) laedt beim Betreten Pilot und Garage neu vom Server (0x1040 + 0x1021,
 * FUN_0010FEA0 -> FUN_000E5840) und zeigt LISTED PILOT; A fuehrt wie nach der Anmeldung ins MAIN MENU.
 *   0x112738 test cl,cl / je 0x112747 / push 8 -> push 3 */
#define LOCCAMP_SWAP_RELOAD_VA 0x112738u
extern const uint8_t loccamp_swap_reload_orig[6];
extern const uint8_t loccamp_swap_reload_new[6];
#define LOCCAMP_SCREEN_CAMPAIGN 3 /* CAMPAIGN-Menue (Pilot laden, LISTED PILOT) */
#define LOCCAMP_SCREEN_MAIN 8     /* MAIN MENU */

#endif
