/* loc_guard.h - W334 LOC-SEC-CLIENT: Gastfilter (Grenze G4, SECURITY.md §6).
 *
 * Letzte Linie vor dem Netzcode des Spiels (LoC-XBE, 2003). Jedes Byte eines Mitspielers geht hier durch,
 * bevor es einen Socket des Spiels erreicht:
 *  - TCP 1000: je Strom ein Zerleger mit der Laengentabelle des Spiels (FUN_0004EFE0, SECURITY.md §6.2);
 *    nur ganze Rahmen bekannter Typen werden freigegeben; Laenge 0, Typ 0x34 mit n >= 0x80 (Vorzeichenfehler
 *    im Spielzerleger) oder mehr als 0x8000 unzugestellte Bytes = Verstoss.
 *  - UDP 1201/1202: genau ein Rahmen je Datagramm, Typ und Laenge exakt nach Positivliste (§6.3); alles
 *    andere (auch Port 1203/VDP) wird verworfen.
 *  - Inhaltsregeln Stufe 2 (§6.6 Punkt 1, Felder mit Disassembly-Beleg, docs/loc/LOC_RE1.md §6.5).
 * Verstoss = Mitspieler trennen (fail closed; ein verworfener Rahmen verschoebe die Rahmengrenzen im Strom).
 * Keine Inhalte im Log. Community-tauglich (keine Abhaengigkeit ausser stdint). */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LGUARD_TCP_MAX 0x8000 /* Spielpuffer 0x8004 je Peer, recv > 0x8000 fatal (LOC_RE1 §6.4) */
#define LGUARD_MAX_FRAME 0x100C
/* W339: groesster Rahmen mit Inhaltsregel (0x22). Typen mit Inhaltsregel gibt lguard_walk_tcp erst frei, wenn
 * der ganze Rahmen sichtbar und gueltig ist. */
#define LGUARD_HOLD_MAX 0x100C

/* Gruende eines Verstosses (fuer das Log, ohne Inhalte) */
enum {
    LGUARD_OK = 0,
    LGUARD_V_TYPE = 1,    /* Typ mit Laenge 0 / ausserhalb der Tabelle */
    LGUARD_V_NEG = 2,     /* 0x34 mit negativer Anzahl */
    LGUARD_V_BUFFER = 3,  /* zu viele unzugestellte Bytes */
    LGUARD_V_CONTENT = 4, /* Inhaltsregel Stufe 2 */
    LGUARD_V_STATE = 5    /* Strom nach Verstoss weiter benutzt */
};

typedef struct lguard_tcp {
    int len;    /* unzugestellte Bytes in buf */
    int bad;    /* Grund des Verstosses (0 = keiner); danach liefert der Filter nie wieder Bytes */
    uint8_t buf[LGUARD_TCP_MAX];
} lguard_tcp;

void lguard_tcp_init(lguard_tcp *g);
/* Bytestrom eines Mitspielers (TCP 1000) einspeisen. out erhaelt die freigegebenen ganzen Rahmen (cap >= n + len
 * genuegt immer; bei kleinerem cap bleiben ganze Rahmen im Puffer und kommen beim naechsten Aufruf, auch mit n = 0).
 * Rueckgabe: Anzahl freigegebener Bytes (>= 0) oder -1 = Verstoss (Grund in g->bad; dann nichts zustellen,
 * Mitspieler trennen). */
int lguard_tcp_feed(lguard_tcp *g, const uint8_t *in, int n, uint8_t *out, int cap);

/* Pruefung ohne Puffer fuer die HLE-Hooks (recv des Spiels mit MSG_PEEK): wie viele der n vorliegenden Bytes
 * duerfen zum Spiel? Rahmenkoepfe werden erst freigegeben, wenn ihr Typ (bei 0x34 auch n) entschieden ist.
 * commit = 1: Zustand fuer die freigegebenen Bytes fortschreiben. Rueckgabe >= 0 oder -1 = Verstoss (*why). */
typedef struct lguard_walk {
    int rem; /* Restbytes des laufenden Rahmens (0 = an einer Rahmengrenze) */
} lguard_walk;
int lguard_walk_tcp(lguard_walk *w, const uint8_t *p, int n, int commit, int *why);
/* Zustand um genau take Bytes fortschreiben, die vorher mit lguard_walk_tcp(p, avail) freigegeben wurden (take kann
 * mitten in einem bereits entschiedenen Rahmen enden, z. B. bei kleinem Empfangspuffer des Spiels). */
void lguard_walk_advance(lguard_walk *w, const uint8_t *p, int avail, int take);

/* UDP-Datagramm an Spielport game_port: 1 = darf zum Spiel, 0 = verwerfen. */
int lguard_udp_ok(uint16_t game_port, const uint8_t *d, int n);
/* Wie lguard_udp_ok, Grund bei Ablehnung (LGUARD_V_*). */
int lguard_udp_check(uint16_t game_port, const uint8_t *d, int n, int *why);

/* Laenge eines Rahmens wie FUN_0004EFE0: > 0 Laenge, 0 = unzulaessig, -1 = noch nicht bestimmbar (mehr Bytes). */
int lguard_frame_len(const uint8_t *f, int avail);

/* Ratenbremse (Token-Eimer): 1 = durchlassen, 0 = verwerfen. now_ms monoton. */
typedef struct lguard_bucket {
    double tokens, last;
    int init;
} lguard_bucket;
int lguard_bucket_take(lguard_bucket *b, double now_ms, double rate_per_s, double burst);

/* Kurzname des Grundes fuer das Log. */
const char *lguard_reason(int why);

#ifdef __cplusplus
}
#endif
