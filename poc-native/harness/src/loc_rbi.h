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
} rbi_stats_t;

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
/* W346: Freilauf nach Desync bzw. Uneinigkeit: keine Rollbacks mehr, kein Stau wegen Fenster/Ringtiefe; fehlt von
 * einem Mitspieler jeder Rahmen, wird ein neutraler Rahmen (keine Eingabe) bedient. Das Spiel haengt nie. */
void rbi_free_run(rbi_t *r);
int rbi_free_running(const rbi_t *r);
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

/* Zeitabgleich: empfohlene Wartezeit in Ticks (geglaettet, >= 0): eigener Vorsprung minus fremder, halbiert. */
double rbi_advantage(const rbi_t *r);

const rbi_stats_t *rbi_stats(const rbi_t *r);
/* Entnahme-Stand: naechster Slot von Spieler i (-1 = ungueltig) und Bestaetigt-bis (hoechster lueckenloser Slot). */
int rbi_cons(const rbi_t *r, int i);
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
