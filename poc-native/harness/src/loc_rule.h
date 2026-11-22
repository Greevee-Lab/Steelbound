/* loc_rule.h - W420 LOC-CLIPDUMP: Match-Regeln des Hosts ("Clip dumping unterbinden") - reiner Kern.
 *
 * Reines Modul ohne Sockets, Spielspeicher, Log oder Konfiguration (Selbsttest tests/loc_net_selftest.c, rule_tests).
 * Die Anbindung (Kanal, Spielzustand, Trennen, Anzeige) macht loc_boot.c (loc_rule_*).
 *
 * Ablauf:
 *  - Jeder Client ab W420 sendet alle 200 ms ein "SBRU" an alle Mitspieler (eigener Kanal, Spielport 1261, im eigenen
 *    Transport als SBT2-Datagramm, also authentisch und verschluesselt): Regelsatz-Version, eigene Host-Einstellung,
 *    in der Mission die angewandte Regel. Aeltere Clients senden nichts und verwerfen das Paket (Port nicht gebunden).
 *  - Regel des Matches = Einstellung des Hosts. Host: eigene Einstellung. Beitretender: Einstellung aus dem SBRU der
 *    Adresse, an die das eigene Spiel die Beitrittsanfrage (UDP 1202 Typ 3) geschickt hat. Kein SBRU von dort = aelterer
 *    Host = Original (0). Offline: eigene Einstellung. In der Mission bleibt die Regel fest.
 *  - Zulassen (Host): Weicht die Regel vom Original ab, muss jeder verbundene Mitspieler innerhalb der Frist ein gueltiges
 *    SBRU geschickt haben (sonst aelterer Client -> getrennt, bevor eine Mission mit abweichender Regel laufen kann).
 *  - Gegenprobe in der Mission: angewandte Regel Host <-> Mitspieler gleich, sonst trennen (kein stilles Weiterspielen).
 * Sicherheit: feste Laenge, alle Felder geprueft, bevor etwas uebernommen wird (fail closed: ungueltig = verworfen und
 * gezaehlt). Keine Eingaben, keine Namen im Paket. */
#pragma once
#include <stdint.h>

#define LRU_PKT_LEN 16
#define LRU_RULESET 1        /* Regelsatz dieser Fassung */
#define LRU_BIT_CLIPDUMP 1u  /* Regel 1: Magazinwechsel setzt die Feuerpause der Hauptwaffe nicht zurueck */
#define LRU_KNOWN 1u         /* alle Regel-Bits, die diese Fassung kennt */
#define LRU_MAXPEER 12
#define LRU_FRESH_MS 3000u   /* SBRU gilt so lange als aktuell */
#define LRU_ADMIT_MS 4000u   /* Frist fuer das erste SBRU eines verbundenen Mitspielers (Host) */
#define LRU_PORT_GP 1261u    /* Spielport-Abbild des Regelkanals (Basis + 261; R2-Kanal 1260) */

/* Rolle fuer die Entscheidung */
enum { LRU_ROLE_OFFLINE = 0, LRU_ROLE_HOST = 1, LRU_ROLE_JOIN = 2 };
/* Quelle der Regel (Log/Overlay) */
enum { LRU_SRC_LOCAL = 0, LRU_SRC_HOST_SELF = 1, LRU_SRC_HOST = 2, LRU_SRC_HOST_OLD = 3, LRU_SRC_HOST_NEWER = 4,
       LRU_SRC_N = 5 };

typedef struct {
    int ver, ruleset, pidx, in_mission;
    uint32_t nonce;
    unsigned pref, applied;
} lru_pkt;

typedef struct {
    int used;
    uint32_t ip;          /* Absender (Adresse wie im Spiel, Netzwerk-Byte-Reihenfolge) */
    uint32_t nonce;
    uint32_t first_ms, last_ms;
    int ruleset, pidx, in_mission;
    unsigned pref, applied;
} lru_peer;

typedef struct {
    uint32_t nonce;
    lru_peer p[LRU_MAXPEER];
    uint32_t rx_ok, rx_bad, rx_self, rx_full, tx;
} lru_t;

void lru_init(lru_t *r, uint32_t nonce);
/* Paket bauen; Rueckgabe Laenge (LRU_PKT_LEN) oder 0. applied nur in der Mission. */
int lru_build(uint8_t *b, int cap, uint32_t nonce, int pidx, unsigned pref, int in_mission, unsigned applied);
/* Strenge Pruefung: 1 = gueltig (out gefuellt), 0 = ungueltig. */
int lru_parse(const uint8_t *b, int len, lru_pkt *out);
/* Empfang: 1 = uebernommen, 0 = eigenes Paket, -1 = ungueltig/Tabelle voll (gezaehlt). */
int lru_rx(lru_t *r, uint32_t now, uint32_t ip, const uint8_t *b, int len);
/* Eintrag zur Adresse (NULL = nie ein gueltiges SBRU gesehen). */
const lru_peer *lru_find(const lru_t *r, uint32_t ip);
int lru_fresh(const lru_peer *P, uint32_t now);
/* Regel des Matches (reine Funktion): role, host_ip (nur LRU_ROLE_JOIN), eigene Einstellung. *src = Quelle.
 * LRU_SRC_HOST_NEWER: Host nutzt einen neueren Regelsatz mit unbekannten Regeln -> Rueckgabe 0, Aufrufer trennt. */
unsigned lru_decide(const lru_t *r, int role, uint32_t host_ip, unsigned local_pref, int *src);
/* Host: Mitspieler an Adresse ip, seit connected_ms verbunden, zulassen? 1 = ja (bzw. Frist laeuft), 0 = aelterer Client
 * bei abweichender Regel (trennen). */
int lru_admit(const lru_t *r, uint32_t ip, unsigned host_rule, uint32_t now, uint32_t connected_ms);
/* Gegenprobe in der Mission: 1 = Mitspieler meldet frisch "in der Mission" mit anderer angewandter Regel. */
int lru_mismatch(const lru_peer *P, unsigned applied, uint32_t now);
const char *lru_src_name(int src); /* "lokal", "host", ... (Log) */

/* Missionsklammer (W420, Bestaetigungslauf): Die Regel wird mit dem ersten Missionshinweis eingefroren und erst wieder
 * frei, wenn eine Lobby-Handlung nach dem Einfrieren erkennbar ist: eigenes Spiel bietet ein Match an (Marke des
 * Host-Lobby-Zustands aendert sich) oder schickt eine neue Beitrittsanfrage (Marke aendert sich). Luecken der
 * Missionshinweise (Rollback-Rahmen, Nachbesprechung, Spieltick) aendern also nichts; eine neue Runde (Host erstellt
 * neu bzw. Beitritt) gibt die Regel frei, bevor die naechste Mission sie wieder einfriert. */
typedef struct {
    int frozen;
    uint32_t host_mark, join_mark; /* Marken beim Einfrieren */
} lru_phase;
/* Lobby-Teil: 1 = gerade freigegeben (Regel neu bestimmen). */
int lru_phase_lobby(lru_phase *p, uint32_t host_mark, uint32_t join_mark);
/* Missions-Teil: mission_ev = Hinweis "Mission laeuft". 1 = gerade eingefroren. */
int lru_phase_mission(lru_phase *p, int mission_ev, uint32_t host_mark, uint32_t join_mark);
