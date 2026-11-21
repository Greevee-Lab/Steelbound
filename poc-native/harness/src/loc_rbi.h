/* loc_rbi.h - W338 LOC-ROLLBACK-R2: Eingabeschicht fuer Rollback (nur LoC-Profil, loc_rb_input=1).
 *
 * Reines Modul ohne Spielspeicher, Sockets, Logging oder Konfiguration (Selbsttest tests/loc_rbi_selftest.c).
 * Die Anbindung an das Spiel (Hooks FUN_0004E920/FUN_0004EC80, Ruhepunkt, Zustandsring R1, UDP-Kanal) macht
 * loc_rollback.c.
 *
 * Begriffe:
 *  - Rahmen: 22 Byte wie im Spiel: 8 Byte Kopf (n=1, Sync-Byte, 2 Byte 0, Flags-Dword) + 14 Byte Eingabesatz.
 *  - Slot n: n-te Entnahme eines Spielers seit Missionsbeginn (FUN_0004E920). Alle Clients entnehmen in Slot n
 *    denselben Rahmen eines Spielers - das ist der Kern der Synchronitaet. Der Absender bestimmt die Slot-Nummer
 *    seiner Rahmen: eigener Rahmen q (Erzeugung q seit Missionsbeginn, inkl. Vorlauf P0) gehoert zu Slot
 *    q - P0 + D (D = Eingabeverzoegerung in Ticks).
 *  - Spieltick g: ein Durchlauf bis zum Ruhepunkt (ein Bild). Commit c: Sicherung des Zustandsrings am Ruhepunkt.
 *    Ticks ohne Entnahme (reiner Stau) werden nicht gesichert (verbrauchen keine Ringtiefe).
 *  - Vorhersage: fehlt ein fremder Rahmen, wird der letzte bestaetigte wiederholt (einmalige Flags geloescht).
 *    Trifft der echte Rahmen spaeter ein und weicht ab, verlangt das Modul einen Rollback ab dem Tick, in dem der
 *    Slot entnommen wurde; die Neurechnung wiederholt die Spielticks genau (gleiche Entnahmen bzw. Staus).
 *  - Hash: Akkumulator des Spiels je Slot; "endgueltig", sobald alle Rahmen bis zu diesem Slot bestaetigt bzw. die
 *    Vorhersagen bestaetigt sind. Endgueltige Hashes werden ausgetauscht und verglichen (Desync-Zaehler).
 * Keine Eingaben im Log: das Modul liefert nur Zaehler. */
#pragma once
#include <stdint.h>

#define RBI_MAXP 10
#define RBI_HDR 8
#define RBI_IN 14
#define RBI_FR 22
#define RBI_WIN 512u          /* Slots je Spieler im Speicher (Zweierpotenz) */
#define RBI_GH 2048u          /* Spielticks Verlauf (Zweierpotenz) */
#define RBI_PKT_FRAMES 24     /* hoechstens so viele eigene Rahmen je Paket (unbestaetigte, aelteste zuerst) */
#define RBI_PKT_HASHES 4
#define RBI_PKT_HEAD 28
#define RBI_SLH 160           /* W372: Puffer-Histogramm (Ankunft vor Entnahme), 2-ms-Stufen ab RBI_SLH_MIN ms */
#define RBI_SLH_MIN (-120)
#define RBI_PKT_MAX (RBI_PKT_HEAD + RBI_MAXP * 8 + RBI_PKT_FRAMES * RBI_FR + RBI_PKT_HASHES * 12)
#define RBI_FL_TRANSIENT 0xD0000000u /* einmalige Flags (Vorzeichen, Zusatzrahmen 0x40000000/0x10000000) */

typedef struct rbi rbi_t;

typedef struct {
    uint32_t taken, taken_conf, predicted, pred_ok, pred_bad, stall_first, stall_window, stall_depth, own_frames, own_dup;
    uint32_t own_missing;          /* eigener Rahmen fuer einen Slot fehlte (sollte 0 sein) */
    uint32_t rx_ok, rx_bad, rx_epoch, rx_bind, rx_frames, rx_dup, rx_conflict, rx_far, tx_pkts, tx_frames;
    uint32_t rb_plans, rb_lost;    /* geplante Rollbacks, nicht mehr moegliche (Ring zu kurz) */
    uint32_t rb_accepted;          /* danach hingenommene Fehlvorhersagen (Desync moeglich) */
    uint32_t hash_final, hash_cmp, hash_bad, hash_skip, hash_recalc_diff;
    int32_t hash_bad_first;        /* erster abweichender Slot, -1 */
    uint32_t hb_acc[2], hb_gt[2];  /* erste Abweichung: [0] eigen, [1] Mitspieler (Akkumulator, Spieltick) */
    uint32_t replay_ticks;         /* neu gerechnete Spielticks gesamt */
    uint32_t replay_stall_mismatch;/* Neurechnung: Entnahme wie im Original nicht moeglich */
    uint32_t stall_hold;           /* W346: Stau, weil ein Neuaufbau des Rings ansteht (keine neue Vorhersage) */
    uint32_t rebuild_deferred;     /* W355: Neuaufbauten aufgeschoben (Allokation bei offener Vorhersage/Neurechnung) */
    uint32_t rebuild_defer_replay; /* W355: davon Ticks, die nur wegen einer laufenden Neurechnung warteten */
    uint32_t rebuild_forced;       /* W355: Aufschub-Grenze erreicht, Neuaufbau trotz offener Vorhersage */
    uint32_t rb_floored;           /* W355: Rollback bis zum Allokations-Tick verlaengert (Aufschub aktiv) */
    /* W372: Puffer je Fremd-Slot (nur live, nur mit rbi_clock): Entnahmezeit minus Ankunftszeit des bestaetigten Rahmens
     * in ms (negativ = kam nach der Entnahme, also vorhergesagt). slack_late = davon negativ. */
    uint32_t slack_h[RBI_SLH], slack_n, slack_late;
    uint32_t ts_actions, ts_fields; /* W372: Feld-Verlaengerungen des Zeitabgleichs (Anzahl, Felder gesamt) */
    uint32_t tt_tx, tt_rx, tt_bad;  /* W372: SBRT-Pakete */
    /* W385 LOC-SMOOTH: Fehlvorhersagen nach abweichendem Feld (je falschem Slot alle zutreffenden Bits, RBI_MIS_*),
     * Achsenfehler der falschen Slots mit der gewaehlten Vorhersage und mit Wiederholung (nur Zaehler, keine Eingaben). */
    uint32_t mis_cat[8];
    uint32_t mis_ax_err;           /* Summe |vorhergesagt - echt| der Achsen-Bytes je falschem Slot */
    uint32_t mis_ax_err_rep;       /* dasselbe, wenn wiederholt worden waere (Vergleich Trend <-> Wiederholung) */
    uint32_t pred_trend;           /* Vorhersagen, bei denen der Trend Achsen veraendert hat */
    uint32_t pred_gain, pred_loss; /* bestaetigte Slots: nur der Trend lag richtig / nur die Wiederholung laege richtig */
    /* W386 LOC-LATENCY: Verzoegerungswechsel waehrend der Mission und Schaetzer fuer D-1 (nur Zaehler). */
    uint32_t dchg_down, dchg_up;   /* angewandte Wechsel D-1 (ein eigener Rahmen verworfen) bzw. D+1 (ein Rahmen gedoppelt) */
    uint32_t dchg_late;            /* Wechsel erst nach dem angekuendigten Slot angewandt (Ankuendigung kam spaet) */
    uint32_t est_n;                /* live entnommene bestaetigte Fremd-Slots mit Ankunftszeit (Grundmenge des Schaetzers) */
    uint32_t est_late;             /* davon Puffer < 1 Tick: bei D-1 waeren sie vorhergesagt worden */
    uint32_t est_wrong;            /* davon haette die Vorhersage (Trend aus Slot n-1/n-2) nicht gestimmt -> Korrektur */
    uint32_t est_dep[4];           /* geschaetzte Tiefe bei D-1 der Faelle est_late: 1, 2, 3, >= 4 Ticks */
} rbi_stats_t;

/* W385: Kategorien einer Fehlvorhersage (Index in mis_cat, Bit in rbi_rb_cause); Byte-Index im 14-Byte-Eingabesatz. */
enum {
    RBI_MIS_AXIS = 0,  /* Dreh-, Ziel-, Sichthebel (Byte 9..13) */
    RBI_MIS_DODGE = 1, /* linkes Pedal (Byte 5, Ausweichen W380) */
    RBI_MIS_PEDAL = 2, /* uebrige Pedale (Byte 6/7) */
    RBI_MIS_GEAR = 3,  /* Gang/Tuner (Byte 8) */
    RBI_MIS_FIRE = 4,  /* Tasten Byte 0 Bit 0/1 (Lage der Waffentasten im Satz nicht belegt; W385-Laeufe: Feuer zaehlt unter RBI_MIS_BTN) */
    RBI_MIS_BTN = 5,   /* uebrige Tasten (Byte 0 Bit 2..7, Byte 1..4), u. a. Feuer und Bremse/Gas-Tasten der Testfolge */
    RBI_MIS_HDR = 6,   /* Kopf/Flags */
    RBI_MIS_N = 7
};
/* W385: Felder, in denen zwei Rahmen (22 Byte, ohne Sync-Byte) abweichen: Bitmaske 1 << RBI_MIS_*. */
uint32_t rbi_frame_cat(const uint8_t a[RBI_FR], const uint8_t b[RBI_FR]);
/* W385 LOC-SMOOTH: Vorhersage fremder Eingaben. mode 0 = letzten bestaetigten Rahmen wiederholen (W338); mode 1 = fuer
 * die analogen Achsen (Byte 9..13, int8) den Trend der letzten zwei bestaetigten Rahmen gedaempft fortsetzen:
 * v = v_l + d * sum_{k=1..m} (q/8)^k (d = v_l - v_{l-1}, m = Slots ueber l hinaus, q = 1..8, 8 = linear), ganzzahlig,
 * geklemmt; |d| > 48 (Tastenflanke auf einer Achse, z. B. A/D -> Drehhebel +-127) haelt den Wert; Tasten, Pedale, Gang und Kopf wie mode 0 (Haltezustand, einmalige Flags geloescht, nie eine Flanke).
 * Rein ganzzahlig -> auf jedem Rechner gleich. Bleibt ueber rbi_begin erhalten. */
void rbi_set_pred(rbi_t *r, int mode, int q);
int rbi_pred_mode(const rbi_t *r);
/* W385: Vorhersage aus den bestaetigten Rahmen (rein, auch fuer den Selbsttest): last = Slot l, prev = Slot l-1 (NULL =
 * nicht bestaetigt), m = n - l >= 1. */
void rbi_predict(const uint8_t last[RBI_FR], const uint8_t *prev, uint32_t m, int mode, int q, uint8_t out[RBI_FR]);
/* W385: Kategorien (1 << RBI_MIS_*) der Fehlvorhersagen seit dem letzten Aufruf mit clear = 1 (ODER). */
uint32_t rbi_rb_cause(rbi_t *r, int clear);

rbi_t *rbi_new(void);
void rbi_free(rbi_t *r);

/* Neue Mission (Epoche, beide Seiten zaehlen gleich). self = eigener Spielerindex, delay = D (1..8),
 * maxpred = hoechstens so viele Slots ueber den aeltesten unbestaetigten hinaus vorhersagen (1..64). Setzt den
 * Spieltick-Verlauf zurueck (laufender Tick g = 0, Commits 0). */
void rbi_begin(rbi_t *r, uint32_t epoch, int self, int delay, int maxpred);
int rbi_active(const rbi_t *r);
uint32_t rbi_epoch(const rbi_t *r);
int rbi_self(const rbi_t *r);
int rbi_delay_eff(const rbi_t *r);   /* wirksame Verzoegerung (min(D, P0)), -1 vor der ersten Entnahme */
int rbi_p0(const rbi_t *r);          /* Vorlauf P0, -1 vor der ersten Entnahme */
void rbi_end(rbi_t *r);              /* Mission vorbei (inaktiv) */
/* W346: Epoche und Verzoegerung nach der Aushandlung setzen; nur vor der ersten Entnahme (1 = gesetzt). */
int rbi_configure(rbi_t *r, uint32_t epoch, int delay);
/* W386 LOC-LATENCY: Verzoegerung waehrend der Mission wechseln (nur die Zuordnung der EIGENEN Rahmen zu Slots; die Slots
 * selbst sind die Wahrheit, jeder Mitspieler bekommt genau die gesendeten Rahmen -> immer deterministisch).
 * Angewandt beim ersten eigenen Live-Rahmen, dessen Slot >= from_slot waere (rbi_own, nicht im Neurechnen):
 *  - D-1: dieser Rahmen wird verworfen; seine Tasten (Byte 0..4 des Eingabesatzes) und einmaligen Flags gehen per ODER
 *    in den naechsten eigenen Rahmen, der den Slot uebernimmt (kurze Tastendruecke gehen nicht verloren).
 *  - D+1: dieser Rahmen belegt seinen Slot und (ohne einmalige Flags) zusaetzlich den naechsten.
 * Je Schritt hoechstens 1 Tick; neues D 1..8. Rueckgabe 1 = vorgemerkt, 0 = ungueltig/gleich (eine schon vorgemerkte
 * Aenderung wird ersetzt). */
int rbi_set_delay(rbi_t *r, int d, uint32_t from_slot);
int rbi_delay_pending(const rbi_t *r, int *d, uint32_t *from_slot); /* 1 = Wechsel vorgemerkt */
uint32_t rbi_own_next_slot(const rbi_t *r); /* Slot des naechsten eigenen Rahmens (0 vor der ersten Entnahme) */
uint64_t rbi_peer_src(const rbi_t *r, int i); /* W386: gebundene Absenderkennung von Spieler i (0 = keine) */
/* W386: Schaetzer "Korrekturen bei D-1" (rein, auch fuer den Selbsttest): Puffer slack_us des bestaetigten Rahmens
 * (Ankunft vor Entnahme) -> geschaetzte Tiefe bei einem Tick weniger Verzoegerung (0 = waere rechtzeitig gewesen). */
int rbi_est_depth(int32_t slack_us);
/* W346: Freilauf nach Desync bzw. Uneinigkeit: keine Rollbacks mehr, kein Stau wegen Fenster/Ringtiefe; fehlt von
 * einem Mitspieler jeder Rahmen, wird ein neutraler Rahmen (keine Eingabe) bedient. Das Spiel haengt nie. */
void rbi_free_run(rbi_t *r);
int rbi_free_running(const rbi_t *r);
/* W366 LOC-NET-EDGES: Mitspieler i hat die Mission verlassen (vom Spiel entfernt): seine offenen Vorhersagen halten
 * weder Ringtiefe noch Neuaufbau mehr auf (sonst Stau fuer immer). Zaehler angenommener Pakete je Spieler. */
void rbi_peer_gone(rbi_t *r, int i);
uint32_t rbi_gone_mask(const rbi_t *r);
uint32_t rbi_rx_count(const rbi_t *r, int i);
/* W346 R3: Neuaufbau des Rings steht an: keine neuen Vorhersagen (Stau statt Vorhersage), bis alle offenen bestaetigt
 * sind; dann baut die Anbindung an einem bestaetigten Tick neu auf (Barriere ohne Verlust). */
void rbi_hold(rbi_t *r, int on);
int rbi_open_preds(const rbi_t *r); /* 1 = Vorhersage unbestaetigt, Rollback steht an oder Neurechnung laeuft */
/* W355: Entscheidung ueber einen Ring-Neuaufbau am Ruhepunkt (nach rbi_tick_end). need = Bereiche geaendert,
 * only_alloc = nur das Allokationsregister wuchs (keine Freigabe/kein Schutzwechsel), defer_on = Aufschub erlaubt.
 * Reine Allokation bei offener Vorhersage bzw. waehrend einer Neurechnung wird aufgeschoben (nie mitten in einer
 * Neurechnung, hoechstens max_ticks Live-Ticks); bis dahin keine neue Vorhersage und jeder Rollback zielt mindestens
 * auf den Allokations-Tick (der neue Block liegt nicht im Ring; seine Belegung wird mitgerechnet).
 * Rueckgabe RBI_REBUILD_NONE / _DEFER / _NOW (jetzt neu aufbauen, danach rbi_barrier). */
enum { RBI_REBUILD_NONE = 0, RBI_REBUILD_DEFER = 1, RBI_REBUILD_NOW = 2 };
int rbi_rebuild_gate(rbi_t *r, int need, int only_alloc, int defer_on, uint32_t max_ticks);
uint32_t rbi_rebuild_wait(const rbi_t *r); /* W355: Live-Ticks des laufenden Aufschubs (0 = keiner) */
/* W355: im eben beendeten Tick wurde (wieder) belegt - auch im Neurechnen, wenn erst die korrigierte Zeitlinie belegt.
 * Laeuft ein Aufschub, sinkt die Rollback-Untergrenze auf diesen Tick (Aufruf nach rbi_tick_end). */
void rbi_rebuild_note(rbi_t *r);
/* W355 TEST: Rollback um ticks_back Spielticks verlangen, als waere dort eine Fehlvorhersage erkannt (gleiche Eingaben
 * -> die Neurechnung muss bitgleich sein; Gegenprobe des Neurechnens). 1 = vorgemerkt. */
int rbi_force_rb(rbi_t *r, uint32_t ticks_back, uint32_t avail);
/* W355: erster in dieser Mission verglichener endgueltiger Slot (-1 = noch keiner) und ob er gleich war. */
int32_t rbi_hash_first_cmp(const rbi_t *r, int *equal);
uint32_t rbi_replay_until(const rbi_t *r); /* Ende der laufenden Neurechnung (Spieltick) */

/* Eigener Rahmen aus dem Spiel (FUN_0004EC80). 1 = neu, 0 = schon vorhanden (Neurechnung: Original bleibt). */
int rbi_own(rbi_t *r, const uint8_t hdr[RBI_HDR], const uint8_t in[RBI_IN]);

/* Entnahme fuer Spieler i im laufenden Tick. cmin = kleinster Commit-Stand vor einem Entnahme-Tick, zu dem der
 * Ring noch zurueck kann (Anbindung: Commits - ladbare Tiefe, mindestens Barriere nach Ring-Neuaufbau).
 * 1 = bedient (out = Kopf+Eingabe fuer das Spiel, Sync-Byte = eigenes Sync-Byte des Slots), 0 = warten (Stau). */
int rbi_take(rbi_t *r, int i, uint32_t cmin, uint8_t out[RBI_FR]);

/* Akkumulator (Zustandshash) des Spiels beim Rahmenbau im laufenden Tick (gehoert zum zuletzt eigenen Slot). */
void rbi_acc(rbi_t *r, uint32_t acc, uint32_t gtick);
/* Naechster endgueltiger Hash in Slot-Reihenfolge (fuer die eigene Hash-Kette). 1 = geliefert. */
int rbi_final_next(rbi_t *r, uint32_t *slot, uint32_t *acc, uint32_t *gtick);

/* ---- Spieltick-Buchfuehrung (Ruhepunkt) ---- */
/* Tick-Ende: 1 = diesen Tick sichern (Commit), 0 = reiner Stau (mit dem naechsten zusammen sichern). Danach
 * beginnt der naechste Tick. *replay_done = 1, wenn damit eine Neurechnung fertig ist. */
int rbi_tick_end(rbi_t *r, int *replay_done);
/* Ring wurde neu aufgebaut bzw. ist uebergelaufen: kein Rollback vor den aktuellen Commit-Stand. */
void rbi_barrier(rbi_t *r);
uint32_t rbi_commits(const rbi_t *r);
uint32_t rbi_gtick(const rbi_t *r);
int rbi_replaying(const rbi_t *r);
int rbi_rb_pending(const rbi_t *r); /* 1 = Fehlvorhersage erkannt, Rollback steht an */
/* Rollback planen (nur ausserhalb einer Neurechnung). avail = ladbare Ring-Ticks (rbr_depth()).
 * Rueckgabe: 0 = keiner noetig, >0 = so viele Commits laden (rbr_load), danach *ticks Spielticks neu rechnen
 * (Neurechnung beginnt sofort mit dem naechsten Tick), -1 = noetig, aber nicht mehr moeglich (gezaehlt). */
int rbi_plan(rbi_t *r, uint32_t avail, uint32_t *ticks, uint32_t *slots_back);

/* ---- Kanal ---- */
/* Paket bauen (eigene unbestaetigte Rahmen, Bestaetigungen, letzte endgueltige Hashes). Rueckgabe Laenge, 0 = nichts. */
int rbi_tx(rbi_t *r, uint8_t *buf, int cap);
/* Paket pruefen und uebernehmen. src = Absenderkennung (Adresse/Port, bindet einen Spielerindex je Epoche).
 * 1 = angenommen, 0 = andere Epoche/inaktiv (ignoriert), -1 = ungueltig (verworfen, gezaehlt). */
int rbi_rx(rbi_t *r, const uint8_t *buf, int len, uint64_t src);

/* W372: Uhr des Aufrufers in us (monoton, Ueberlauf erlaubt; 0 = keine Messung). Gilt fuer die folgenden rbi_rx
 * (= Ankunftszeit der Pakete) bzw. rbi_take (= Entnahmezeit). */
void rbi_clock(rbi_t *r, uint32_t now_us);
/* W372: Perzentil pct (0..100) eines Puffer-Histogramms mit n Eintraegen in ms (0 bei n = 0). */
int rbi_slack_pct(const uint32_t *h, uint32_t n, int pct);

/* ---- W372: Zeitabgleich ueber die Feldzahl (statt Stau-Tick) ----
 * Jede Seite misst ihren Puffer je Mitspieler (Ankunft des bestaetigten Rahmens vor seiner Entnahme, Median der letzten
 * RBI_SLW Live-Slots) und meldet ihn ihm im Zusatzpaket "SBRT" (Kanal wie SBR2; aeltere Clients verwerfen es als
 * ungueltig, ohne Folgen). Vorsprung gegen Mitspieler j in ms = (sein Puffer fuer meine Rahmen - mein Puffer fuer seine)
 * / 2: Gleich viel Puffer auf beiden Seiten = Vorsprung 0 (gleicht auch ungleiche Wege aus). Ohne SBRT gilt der
 * Slot-Schaetzer (rbi_advantage * 50 ms). Wer vorne liegt, verlaengert sein naechstes Feld-Warten um 1..3 Felder
 * (16,7 ms je Feld; die Simulation rechnet weiter, nur der Takt verschiebt sich), danach Abklingzeit. */
#define RBI_SLW 9             /* Puffer-Fenster je Mitspieler (Live-Slots) */
#define RBI_TT_LEN (16 + RBI_MAXP * 4)
int rbi_is_tt(const uint8_t *buf, int len);
int rbi_tt_tx(rbi_t *r, uint8_t *buf, int cap);             /* Laenge, 0 = nichts zu melden */
int rbi_tt_rx(rbi_t *r, const uint8_t *buf, int len, uint64_t src); /* 1 angenommen, 0 ignoriert, -1 ungueltig */
/* Einmal je Live-Tick am Ruhepunkt: zusaetzliche Felder (0..3) fuer das naechste Feld-Warten. */
int rbi_tsync(rbi_t *r);
double rbi_tsync_ms(const rbi_t *r);  /* zuletzt berechneter Vorsprung in ms (> 0 = vorne) */
int rbi_tsync_src(const rbi_t *r);    /* 2 = aus SBRT (Puffer beider Seiten), 1 = Slot-Schaetzer, 0 = keiner */
int rbi_slack_med(const rbi_t *r, int i, int *ms01); /* eigener Puffer-Median fuer Mitspieler i in 0,1 ms (1 = gueltig) */

/* Zeitabgleich: empfohlene Wartezeit in Ticks (geglaettet, >= 0): eigener Vorsprung minus fremder, halbiert. */
double rbi_advantage(const rbi_t *r);

const rbi_stats_t *rbi_stats(const rbi_t *r);
/* Entnahme-Stand: naechster Slot von Spieler i (-1 = ungueltig) und Bestaetigt-bis (hoechster lueckenloser Slot). */
int rbi_cons(const rbi_t *r, int i);
/* W376 LOC-LOCAL-AIM: eigene erzeugte, noch nicht entnommene Eingabesaetze ab Slot cons (lueckenlos, <= max). */
int rbi_own_pending(const rbi_t *r, uint8_t out[][RBI_IN], int max);
/* W390 LOC-OWN-PREDICT: Rahmen der k-ten naechsten Entnahme von Spieler i (Slot cons + k) fuer den spekulativen
 * Vorlauf, nur lesend. own_new = im Vorlauf neu erzeugter eigener Rahmen (Kopf + Satz, RBI_FR) oder NULL.
 * 1 = bekannt, 2 = vorhergesagt/gehalten, 0 = nicht aktiv. */
int rbi_spec_frame(const rbi_t *r, int i, uint32_t k, const uint8_t *own_new, uint8_t out[RBI_FR]);
int rbi_contig(const rbi_t *r, int i);

/* ---- W359 LOC-RB-FIDELITY ---- */
/* Render-Auftrag des Spiels (0x306EEC: 0 frei, 1 zeichnen) beim Neurechnen. prep = 1 (Standard): die
 * Render-Vorbereitung des Spiel-Threads laeuft jeden Neurechen-Tick wie live (Freigabe-Warteschlange, Doppelpuffer),
 * gezeichnet wird nicht - der Auftrag gilt am Tick-Ende sofort als erledigt (wie live nach dem Warten aufs Zeichnen).
 * prep = 0: altes Verhalten (Auftrag beim Laden = 1 -> Vorbereitung entfaellt bis zum Ende der Neurechnung).
 * at: RBI_RJOB_LOAD (nach dem Laden), RBI_RJOB_TICK (Ende jedes Neurechen-Ticks, vor dem Sichern), RBI_RJOB_DONE
 * (Ende der Neurechnung). cur = aktueller Wert. Rueckgabe = neuer Wert. */
enum { RBI_RJOB_LOAD = 0, RBI_RJOB_TICK = 1, RBI_RJOB_DONE = 2 };
uint32_t rbi_rjob_replay(int prep, int at, uint32_t cur);

/* Plausibilitaet der Freilisten eines XDK-NT-Heaps (RTL im XBE: FreeLists[128] ab Heap+0x180, je 8 Byte; Bitmap
 * Heap+0x160; Block 16 Byte ausgerichtet, Kopf: Wort Groesse in 16-Byte-Einheiten, Byte +5 Flags (0x01 belegt),
 * Listenknoten = Block + 8 {Flink, Blink}; Liste i > 0 = Bloecke der Groesse i, Liste 0 = ab 0x80 aufsteigend).
 * rd liest ein Gast-Wort (0 = nicht lesbar bzw. ausserhalb der gesicherten Bereiche). max_steps begrenzt die Knoten
 * (danach RBI_HEAP_OK, *nodes = max_steps). *bad = Adresse des ersten Fehlers. */
enum { RBI_HEAP_OK = 0, RBI_HEAP_HDR, RBI_HEAP_PTR, RBI_HEAP_LINK, RBI_HEAP_SIZE, RBI_HEAP_BUSY, RBI_HEAP_ORDER,
       RBI_HEAP_BITMAP, RBI_HEAP_N };
typedef int (*rbi_rd32_fn)(void *ctx, uint32_t va, uint32_t *out);
int rbi_heap_check(rbi_rd32_fn rd, void *ctx, uint32_t heap, uint32_t max_steps, uint32_t *bad, uint32_t *nodes);
const char *rbi_heap_err(int code);
