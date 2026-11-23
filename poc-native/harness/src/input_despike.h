/* input_despike.h - W439 LOC-RETICLE-JUMP (#78): optionaler Ausreisser-Filter fuer analoge Achsen des Original-Controllers
 * (SBC ueber USB) und von Joysticks (DirectInput). Schalter ctrl_despike (Standard 1 nach Nutzerentscheid 22.11.; 0 = Originalverhalten). Gamepad und Maus
 * werden nicht gefiltert.
 *
 * Hintergrund: Der SBC nutzt Potentiometer; verschmutzte oder abgenutzte Potis liefern einzelne Ausreisser-Werte. Der Weg
 * zum Spiel hielt bisher jeden Report bis zum naechsten (Lese-Thread -> Cache -> Abfrage je Spieltick). Ein einzelner
 * Ausreisser, der zur Abfrage im Cache liegt, geht damit ganz in einen Spieltick (Zielhebel = Lage des Fadenkreuzes:
 * Fadenkreuz einen Tick woanders, dann zurueck).
 *
 * Regel je Achse (Report-Strom, der SBC sendet nur bei Aenderung):
 *  - Schritt <= thr zum ausgegebenen Wert: sofort uebernehmen (keine Verzoegerung bei normaler Bewegung).
 *  - Groesserer Sprung: als Kandidat halten (Ausgabe bleibt). Der naechste Report entscheidet:
 *      zurueck nahe der Ausgabe -> Ausreisser verworfen;
 *      nahe dem Kandidaten oder weiter in dieselbe Richtung -> echte schnelle Bewegung, uebernehmen;
 *      sonst neuer Kandidat.
 *  - Kommt kein Report, gilt der Kandidat nach hold_ms als echt (Abfrage despike_read).
 * Rein und ohne Windows-Abhaengigkeit (Selbsttest input_selftest Teil 33). Der Filter wirkt lokal vor der Eingabeschicht;
 * Replays zeichnen den gefilterten Stand auf, das Netz sieht nur gefilterte Saetze (Determinismus unveraendert). */
#ifndef INPUT_DESPIKE_H
#define INPUT_DESPIKE_H

#include <stdint.h>

#define DESPIKE_MAX_AX 8

typedef struct {
    int out[DESPIKE_MAX_AX];      /* ausgegebener Wert */
    int cand[DESPIKE_MAX_AX];     /* gehaltener Sprung-Kandidat */
    uint64_t cand_t[DESPIKE_MAX_AX]; /* Zeit des ersten Kandidaten (ms) */
    uint8_t has_cand[DESPIKE_MAX_AX];
    uint8_t init;
    int n;                        /* Achsen */
    int thr;                      /* Sprung-Schwelle (Einheit der Achse) */
    unsigned hold_ms;             /* hoechste Haltezeit eines Kandidaten */
    unsigned spikes, held, accepted; /* Zaehler: verworfene Ausreisser, gehaltene Spruenge, als echt uebernommene */
} despike_t;

/* Standard-Schwellen: 8-Bit-Achsen (SBC) bzw. 16-Bit-Achsen (Joystick), Haltezeit. */
#define DESPIKE_THR_U8 40
#define DESPIKE_THR_S16 10240
#define DESPIKE_HOLD_MS 12u

void despike_init(despike_t *d, int n, int thr, unsigned hold_ms);
/* Zustand auf v setzen (Neustart, Trennung -> neutral), kein Kandidat. */
void despike_reset(despike_t *d, const int *v);
/* Neuer Report (Werte je Achse) zur Zeit now_ms; out = gefilterte Werte. */
void despike_feed(despike_t *d, const int *v, uint64_t now_ms, int *out);
/* Abfrage ohne neuen Report: Kandidaten aelter als hold_ms gelten als echt; out = gefilterte Werte. */
void despike_read(despike_t *d, uint64_t now_ms, int *out);

#endif
