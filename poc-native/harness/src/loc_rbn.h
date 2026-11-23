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
#define RBN_GRACE_MS 4000u   /* W370: Nachfrist fuer ein fehlendes/veraltetes Hallo eines bekannten Rollback-Mitspielers */

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

/* Missionsbeginn. self = eigener Spielerindex (0 = Host), npeers = tatsaechlich verbundene Mitspieler (W370: nicht
 * "hoechster Platz", nach einem Wiederbeitritt bleiben Plaetze leer).
 * Rueckgabe Zustand: LS (sofort Lockstep), TENT (Host: R2 angeboten bzw. W370 Nachfrist, warten), PEND (Client: warten).
 * W370 Nachfrist: Fehlt beim Host die Bestaetigung eines Mitspielers, kennt er aber genug Mitspieler mit Wunsch
 * (Hallo frueher gesehen), wartet er bis RBN_GRACE_MS, bevor er Lockstep entscheidet. Der Client wartet ebenso auf einen
 * bekannten Rollback-Host, dessen Hallo gerade fehlt. Vor der Entscheidung entnimmt keine Seite (Rueckfall sicher). */
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
/* W370: Client mit RBN_WHY_HOST_LS: Grund des Hosts (RBN_WHY_*, 0 = unbekannt, z. B. aelterer Host). Der Host traegt
 * ihn bei einer Lockstep-Entscheidung in Byte 27 des Hallos ein (sonst 0). */
int rbn_host_why(const rbn_t *n);
/* W370: Host wartet noch auf das Hallo eines bekannten Rollback-Mitspielers (Nachfrist, Zustand TENT, noch keine
 * Entscheidung gesendet), 1 = ja. */
int rbn_host_waiting(const rbn_t *n);
/* W370: Mitspieler fuer das Log: "Platz 2 Wunsch 1 bestaetigt 1 Alter 120 ms RTT 190 ms; ..." (nur Zahlen). */
int rbn_peers_desc(const rbn_t *n, uint32_t now, char *buf, int cap);
/* Mitspieler meldet fuer die laufende Mission Lockstep, obwohl hier R2 laeuft (Uneinigkeit), 1 = ja. */
int rbn_peer_disagrees(const rbn_t *n);
/* Groesste aktuelle Umlaufzeit aller frischen Mitspieler in ms, -1 = unbekannt. */
int rbn_rtt_max(const rbn_t *n, uint32_t now);
/* Adaptive Verzoegerung aus Umlaufzeit (geglaettet + 2x Schwankung): Rahmen brauchen RTT/2 plus Reserve bis zum
 * Mitspieler; ein Tick = 50 ms. D = ceil((rtt/2 + 35) / 50), begrenzt auf [dmin, dmax] (Standard 2..3); unbekannt = 2.
 * Praktisch: LAN/Nachbar/EU<->USA (bis ~130 ms) 2, schwankende oder weite Strecken 3. D = 1 nur fest (loc_rb_delay=1):
 * W346 A1 ergab damit bei 45 ms 6x mehr Haenger - W386: fast alle im Missionsstart, solange der Ring fehlt; die
 * niedrige Verzoegerung (rbl_*) senkt deshalb erst nach dem Ringstart um 1 Tick. */
int rbn_pick_delay(int rtt_ms, int dmin, int dmax);
const rbn_stats_t *rbn_stats(const rbn_t *n);

/* ---- W447 LOC-NET-AUTO: Netzmodus "Auto" (Profil Direkt/Rollback je Mission) ----
 * Kein harter Moduswechsel im Match: der Motor bleibt die Eingabeschicht (R2) mit adaptiver Verzoegerung. Auto waehlt
 * beim Missionsbeginn ein Profil (Host fuer alle, im Hallo verteilt):
 *  - DIREKT: alle Umlaufzeiten klein und ruhig -> Ziel 1 Tick Verzoegerung (Eingaben kommen rechtzeitig an, praktisch
 *    keine Korrekturen, Gefuehl wie Lockstep ohne Einfrieren bei Spitzen); schneller auf 1 Tick, strengere Grenzen.
 *  - ROLLBACK: sonst; Regelung und Werkzeuge wie bisher (W385/W386/W387/W390).
 * Hallo Flags b3 = "Auto gewuenscht" (loc_net_auto=1). Auto gilt nur, wenn der Host und jeder Mitspieler Auto wollen;
 * sonst Profil FEST (= Rollback wie bisher). Lockstep (loc_rb_input=0 bzw. Wunsch aus) gewinnt wie bisher.
 * Host traegt das Profil bei einer R2-Entscheidung in Byte 27 ein (0 fest, 1 Auto-Direkt, 2 Auto-Rollback). */
enum { RBN_PROF_FIXED = 0, RBN_PROF_DIRECT = 1, RBN_PROF_ROLLBACK = 2 };
/* Wunsch zur Laufzeit aendern (ESC-Menue; wirkt ab der naechsten Mission): want = Eingabeschicht (0 = Lockstep),
 * want_auto = Auto. */
void rbn_set_want(rbn_t *n, int want, int want_auto);
int rbn_want(const rbn_t *n);
int rbn_want_auto(const rbn_t *n);
/* Profil aus Umlaufzeit (rtt_eff = Mittel + 2 Schwankung wie rbn_rtt_max, ms) und Schwankung (rttvar, ms):
 * DIREKT, wenn rtt_eff <= direct_rtt und rttvar <= direct_jit, sonst ROLLBACK; unbekannt (< 0) = ROLLBACK. */
int rbn_pick_profile(int rtt_eff, int rttvar, int direct_rtt, int direct_jit);
/* Groesste Schwankung (rttvar, ms) aller frischen Mitspieler, -1 = unbekannt. */
int rbn_rttvar_max(const rbn_t *n, uint32_t now);
/* Host: Grenzen fuer DIREKT setzen (Standard 80 ms / 15 ms). */
void rbn_set_direct(rbn_t *n, int direct_rtt, int direct_jit);
/* Host: DIREKT beginnt die Mission gleich mit 1 Tick (statt d_std, Regler startet niedrig, rbl_start_low), wenn die
 * groesste RTT (wie rbn_rtt_max) <= start_rtt ms ist; 0 = nie. */
void rbn_set_direct_start(rbn_t *n, int start_rtt);
/* Profil der laufenden R2-Mission (RBN_PROF_*; Host: eigene Entscheidung, Client: vom Host). */
int rbn_profile(const rbn_t *n);
/* Lobby: Auto mit allen frischen Mitspielern vereinbart (alle wollen Auto)? 1 = ja. */
int rbn_auto_agreed(const rbn_t *n, uint32_t now);
/* Lobby-Anzeige: frische Mitspieler (Platz, geglaettete RTT ms, Auto-Wunsch); Rueckgabe Anzahl (<= cap). */
int rbn_peer_list(const rbn_t *n, uint32_t now, int *pidx, int *rtt, int *wauto, int cap);

/* ---- W386 LOC-LATENCY: Verzoegerung adaptiv um 1 Tick senken ("niedrige Verzoegerung") ----
 * Der Host regelt fuer alle gleich; Wechsel nur an sicheren Stellen. Ablauf:
 *  - Missionsbeginn mit der ausgehandelten Verzoegerung d_std (rbn_pick_delay, bewaehrt; der Ring steht noch nicht,
 *    eine spaete Fremd-Eingabe waere dort ein Stau). d_low = rbn_pick_low(...) = d_std - 1 bis lowmax ms Umlaufzeit.
 *  - Jede Seite meldet alle 10 s im Paket "SBRL" ihr Fenster: Ring bereit, angewandtes D, Korrekturen je min,
 *    geschaetzte Korrekturen und Tiefe bei D-1 (Puffer < 1 Tick und Vorhersage haette nicht gestimmt, rbi_est_depth),
 *    Tiefe, Neurechnen p95, Haenger (> 100 ms) und sichtbaren Sprung (W385).
 *  - Host senkt auf d_low, wenn alle Seiten good_n Fenster in Folge gut sind (Schaetzer unter den l-Grenzen), und geht
 *    zurueck auf d_std, wenn eine Seite schwer (sofort) bzw. zwei Fenster in Folge schlecht ist (h-Grenzen).
 *    Abklingzeit nach dem Zurueckgehen cool_s (verdoppelt je Mal, hoechstens 4x), hoechstens max_up Mal je Mission.
 *  - Entscheidung = {Folge, D, ab Slot S}; S = eigener naechster Slot + Umlaufzeit in Ticks + lead. Jede Seite wendet
 *    sie mit rbi_set_delay an (nur die Zuordnung der eigenen Rahmen; die Slots sind die Wahrheit -> deterministisch,
 *    auch wenn eine Seite spaeter wechselt).
 * Rein (kein Log, keine Sockets); Selbsttest tests/loc_rbi_selftest.c. Keine Eingaben im Paket, nur Zaehler. */
#define RBL_LEN 32
#define RBL_MAXP 10
#define RBL_WIN_MS 10000u

typedef struct {
    int ready;        /* Ring bereit (R2 live, Ring aktiv, kein Freilauf) */
    int dcur;         /* angewandte Verzoegerung 1..8 */
    unsigned win;     /* Fensternummer 1..65535 (steigt) */
    unsigned cpm;     /* Korrekturen (Rollbacks) je min im Fenster */
    unsigned est_cpm; /* geschaetzte Korrekturen je min bei D-1 */
    unsigned dep;     /* groesste Rollback-Tiefe im Fenster (Ticks) */
    unsigned est_dep; /* geschaetzte Tiefe bei D-1 (95 % der spaeten Faelle, Ticks) */
    unsigned rp;      /* Neurechnen p95 im Fenster (0,1 ms) */
    unsigned hang;    /* Haenger > 100 ms im Fenster */
    unsigned jump;    /* groesster sichtbarer Sprung im Fenster (Spieleinheiten ~ mm) */
} rbl_rep_t;

typedef struct {
    int on;               /* 1 = senken erlaubt */
    int lowmax;           /* senken nur bis zu dieser Umlaufzeit (ms, gemessen wie rbn_rtt_max) */
    int lcpm, hcpm;       /* gut: geschaetzt <= lcpm; schlecht: tatsaechlich > hcpm */
    int ldep, hdep;       /* gut: geschaetzte Tiefe <= ldep; schlecht: Tiefe >= hdep, schwer >= hdep + 2 */
    int lrp, hrp;         /* Neurechnen p95 (0,1 ms): gut <= lrp; schwer > hrp */
    int lhang, hhang;     /* Haenger je Fenster: gut <= lhang; schlecht >= hhang, schwer >= 2 * hhang */
    int hjump;            /* schlecht: Sprung > hjump */
    int hang6;            /* W386: zurueck, wenn die Haenger (je Fenster die groessere Seite) der letzten 6 Fenster in der
                           * niedrigen Stufe zusammen >= hang6 (sofort; Lauf Nwlan/Nwlan2: wackliges WLAN 0,5/Fenster) */
    int good_n;           /* gute Fenster in Folge vor dem Senken */
    int cool_s;           /* Abklingzeit nach dem Zurueckgehen (s) */
    int max_up;           /* hoechstens so oft zurueck je Mission */
    int lead;             /* Vorlauf der Ankuendigung (Slots) zusaetzlich zur Umlaufzeit */
} rbl_cfg_t;

enum { /* Grund der letzten Entscheidung (Log/Selbsttest) */
    RBL_WHY_NONE = 0,
    RBL_WHY_GOOD = 1,     /* gesenkt: alle Fenster gut */
    RBL_WHY_CPM = 2,      /* zurueck: zu viele Korrekturen */
    RBL_WHY_DEPTH = 3,    /* zurueck: Rollbacks zu tief */
    RBL_WHY_REPLAY = 4,   /* zurueck: Neurechnen zu teuer (langsame CPU) */
    RBL_WHY_HANG = 5,     /* zurueck: Haenger */
    RBL_WHY_JUMP = 6,     /* zurueck: sichtbare Spruenge */
    RBL_WHY_NOTREADY = 7, /* zurueck: eine Seite ohne Ring */
    RBL_WHY_N = 8
};

typedef struct {
    uint32_t tx, rx_ok, rx_bad, rx_epoch, evals, skipped, down, up, last_why, peers_seen;
} rbl_stats_t;

typedef struct rbl rbl_t;

void rbl_cfg_default(rbl_cfg_t *c);
int rbn_pick_low(int rtt_ms, int d_std, int lowmax); /* d_std - 1 (>= 1) bis lowmax, sonst d_std */
rbl_t *rbl_new(void);
void rbl_free(rbl_t *l);
/* Missionsbeginn (R2 ausgehandelt): Regler zuruecksetzen. Zaehler laufen weiter. */
void rbl_begin(rbl_t *l, const rbl_cfg_t *c, uint32_t epoch, int self, int npeers, int d_std, int d_low);
void rbl_end(rbl_t *l);
int rbl_active(const rbl_t *l);
int rbl_target(const rbl_t *l); /* Host: aktuelles Ziel; Client: zuletzt empfangenes D (0 = keine Entscheidung) */
int rbl_dstd(const rbl_t *l);
int rbl_dlow(const rbl_t *l);
/* Paket SBRL (Bericht + beim Host die letzte Entscheidung). Rueckgabe Laenge oder 0. */
int rbl_tx(rbl_t *l, const rbl_rep_t *own, uint8_t *buf, int cap);
int rbl_is_pkt(const uint8_t *b, int len);
int rbl_peek_pidx(const uint8_t *b, int len); /* Absenderindex fuer die Bindungspruefung (-1 = kein SBRL) */
/* Empfang (vollstaendig geprueft, dann uebernommen). 1 = ok, 0 = andere Mission/inaktiv, -1 = ungueltig. */
int rbl_rx(rbl_t *l, uint32_t now_ms, const uint8_t *b, int len);
/* Host: eigenes neues Fenster auswerten (mit den juengsten Berichten der Mitspieler). next_slot = Slot des naechsten
 * eigenen Rahmens, rtt_ms = groesste Umlaufzeit. 1 = neue Entscheidung (rbl_take_decision liefert sie). */
int rbl_host_step(rbl_t *l, uint32_t now_ms, const rbl_rep_t *own, uint32_t next_slot, int rtt_ms);
/* Alle: neue, noch nicht angewandte Entscheidung abholen (1 = ja) und als angewandt merken. */
int rbl_take_decision(rbl_t *l, int *d, uint32_t *slot);
/* TEST (loc_rb_opts ltest=N): Host entscheidet sofort fuer d (d_std bzw. d_low), ohne Regelung. 1 = neue Entscheidung. */
int rbl_force(rbl_t *l, int d, uint32_t next_slot, int rtt_ms);
const rbl_stats_t *rbl_stats(const rbl_t *l);
/* W447: nach rbl_begin - Mission laeuft schon mit d_low (Profil DIREKT mit Start 1 Tick). Bis alle Seiten den Ring melden,
 * ist "Ring fehlt" kein Grund zurueck; sonst gelten die Grenzen der niedrigen Stufe (zurueck auf d_std). */
void rbl_start_low(rbl_t *l);
int rbl_startlow(const rbl_t *l);
