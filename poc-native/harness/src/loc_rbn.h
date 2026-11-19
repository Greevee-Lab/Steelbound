/* loc_rbn.h - W346 LOC-ROLLBACK-R4: Aushandlung der Eingabeschicht (loc_rb_input) je Mission (nur LoC-Profil).
 *
 * Reines Modul ohne Sockets, Spielspeicher, Log oder Konfiguration (Selbsttest tests/loc_rbi_selftest.c).
 * Die Anbindung (Empfangs-Thread, Kanal, Missionsbeginn) macht loc_rollback.c.
 *
 * Ablauf:
 *  - Jeder Client mit loc_rb_input=1 sendet ueber den Kanal der Eingabeschicht ein Hallo "SBRH" (5 Hz, auch in der
 *    Lobby): Wunsch, Kennung (zufaellig je Prozess), Uhrzeit fuer die Umlaufzeit, je bekanntem Mitspieler
 *    {Kennung, Echo, Haltezeit, "Wunsch gesehen"} und - vom Host - die Entscheidung der laufenden Mission.
 *    Clients mit loc_rb_input=0 oder aelteren Staenden senden nichts: dann gilt immer der Original-Lockstep.
 *  - Missionsbeginn: Der Host (Spieler 0) entscheidet. R2 nur, wenn jeder Mitspieler in den letzten 3 s ein Hallo
 *    mit Wunsch geschickt hat und darin den Wunsch des Hosts bestaetigt (beide koennen und wollen). Verzoegerung D
 *    aus der groessten gemessenen Umlaufzeit (adaptiv 1..3 Ticks, siehe rbn_pick_delay). Der Host entnimmt erst,
 *    wenn jeder Mitspieler R2 fuer diese Mission bestaetigt hat (TENT); meldet einer Lockstep oder kommt bis zur
 *    Frist keine Bestaetigung, faellt er auf Lockstep zurueck.
 *  - Client: Kennt er keinen R2-faehigen Host, gilt sofort Lockstep. Sonst wartet er (PEND, keine Entnahme) auf die
 *    Entscheidung des Hosts fuer genau diese Mission (Folge > zuletzt gesehene Folge des Hosts ausserhalb einer
 *    Mission). Frist abgelaufen = Lockstep.
 *  Weil vor der Entscheidung keine Seite entnimmt, ist der Rueckfall auf den Lockstep ohne Zustandsabweichung.
 * Sicherheit: Pakete werden vollstaendig geprueft, bevor etwas uebernommen wird (feste Laengen, Bereiche, Nullbytes),
 * fail closed (ungueltig = verworfen + gezaehlt, im Zweifel Lockstep). Keine Eingaben im Paket. */
#pragma once
#include <stdint.h>

#define RBN_MAXPEER 8
#define RBN_PKT_HEAD 36
#define RBN_PKT_ENT 12
#define RBN_PKT_MAXENT 4
#define RBN_PKT_MAX (RBN_PKT_HEAD + RBN_PKT_MAXENT * RBN_PKT_ENT)
#define RBN_FRESH_MS 3000u   /* Hallo gilt so lange als aktuell */
#define RBN_WAIT_MS 12000u   /* Frist fuer Entscheidung (Client) bzw. Bestaetigung (Host) */

enum { RBN_MODE_NONE = 0, RBN_MODE_LS = 1, RBN_MODE_R2 = 2 };
/* Zustand je Mission */
enum { RBN_ST_IDLE = 0, RBN_ST_LS, RBN_ST_TENT, RBN_ST_PEND, RBN_ST_R2 };
/* Gruende fuer Lockstep (Zaehler/Log) */
enum { RBN_WHY_NONE = 0, RBN_WHY_SELF_OFF, RBN_WHY_PEER_NO, RBN_WHY_HOST_NO, RBN_WHY_PEER_LS, RBN_WHY_TIMEOUT,
       RBN_WHY_HOST_LS, RBN_WHY_N };

typedef struct {
    uint32_t tx, rx_ok, rx_bad, rx_self, rx_full, decisions_r2, decisions_ls, fallback[RBN_WHY_N];
    uint32_t rtt_last, rtt_min, rtt_max, rtt_n;
} rbn_stats_t;

typedef struct rbn rbn_t;

/* want = loc_rb_input, nonce = zufaellige Kennung (!= 0), dmin/dmax = Grenzen der Verzoegerung (1..8),
 * dfix = feste Verzoegerung (0 = adaptiv nach Umlaufzeit). */
rbn_t *rbn_new(int want, uint32_t nonce, int dmin, int dmax, int dfix);
void rbn_free(rbn_t *n);

/* Hallo bauen (now = ms-Uhr, self = eigener Spielerindex oder -1). Rueckgabe Laenge, 0 = nichts. */
int rbn_tx(rbn_t *n, uint32_t now, int self, uint8_t *buf, int cap);
/* Paket pruefen und uebernehmen. src = Absenderkennung (Adresse). 1 = angenommen, 0 = ignoriert (eigenes), -1 =
 * ungueltig (verworfen, gezaehlt). */
int rbn_rx(rbn_t *n, uint32_t now, const uint8_t *buf, int len, uint64_t src);
/* 1 = Paket beginnt mit der Kennung "SBRH" (zur Verteilung im Empfangsweg, ohne Pruefung). */
int rbn_is_hello(const uint8_t *buf, int len);

/* Missionsbeginn. self = eigener Spielerindex (0 = Host), npeers = erwartete Mitspieler (Spieler - 1).
 * Rueckgabe Zustand: LS (sofort Lockstep), TENT (Host: R2 angeboten, warten), PEND (Client: warten). */
int rbn_mission_start(rbn_t *n, uint32_t now, int self, int npeers);
/* Bestaetigung eines Mitspielers fuer R2 dieser Mission (Host: SBR2-Paket mit passender Epoche gesehen). */
void rbn_confirm_peer(rbn_t *n, uint64_t src);
/* Zustand weiterfuehren (Frist, eingetroffene Entscheidung/Bestaetigung). Rueckgabe aktueller Zustand. */
int rbn_poll(rbn_t *n, uint32_t now);
void rbn_mission_end(rbn_t *n);
int rbn_state(const rbn_t *n);
uint32_t rbn_epoch(const rbn_t *n);   /* Epoche der laufenden R2-Mission (Host-Folge) */
int rbn_delay(const rbn_t *n);        /* Verzoegerung der laufenden R2-Mission */
int rbn_why(const rbn_t *n);          /* Grund fuer Lockstep (RBN_WHY_*) */
/* Mitspieler meldet fuer die laufende Mission Lockstep, obwohl hier R2 laeuft (Uneinigkeit), 1 = ja. */
int rbn_peer_disagrees(const rbn_t *n);
/* Groesste aktuelle Umlaufzeit aller frischen Mitspieler in ms, -1 = unbekannt. */
int rbn_rtt_max(const rbn_t *n, uint32_t now);
/* Adaptive Verzoegerung aus Umlaufzeit (geglaettet + 2x Schwankung): Rahmen brauchen RTT/2 plus Reserve bis zum
 * Mitspieler; ein Tick = 50 ms. D = ceil((rtt/2 + 35) / 50), begrenzt auf [dmin, dmax] (Standard 2..3); unbekannt = 2.
 * Praktisch: LAN/Nachbar/EU<->USA (bis ~130 ms) 2, schwankende oder weite Strecken 3. D = 1 nur fest (loc_rb_delay=1):
 * W346 A1 ergab damit bei 45 ms 6x mehr Haenger. */
int rbn_pick_delay(int rtt_ms, int dmin, int dmax);
const rbn_stats_t *rbn_stats(const rbn_t *n);
