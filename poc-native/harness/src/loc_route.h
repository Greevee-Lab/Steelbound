/* loc_route.h - W395 LOC-ROUTE: Wegwahl je Mitspieler im Client (reine Logik, ohne Netz, testbar).
 *
 * Jede Verbindung kennt bis zu LOCROUTE_MAXP Wege: Slot 0 = direkt (NAT-Durchstich), Slot 1+i = Relay i
 * (dieselbe link_id auf jedem Relay, Ticket des Treffpunkts). Der Transport (loc_tun.c) schickt auf jedem nutzbaren
 * Weg kleine Proben (SBT2 T_PING mit Kennung "RT", Echo T_PONG ueber denselben Weg) und meldet hier Sendung/Antwort. Daraus:
 *  - je Weg geglaettete RTT, Schwankung (mittlere Abweichung) und Verlust (letzte LOCROUTE_RING Proben);
 *  - Bewertung = RTT + LOCROUTE_JIT_W * Schwankung + LOCROUTE_LOSS_W * Verlust (ms);
 *  - erste Wahl nach dem Messfenster (alle nutzbaren Wege parallel), direkt bei Gleichstand bevorzugt;
 *  - danach Wechsel nur, wenn ein anderer Weg LOCROUTE_SUSTAIN_MS lang um mehr als LOCROUTE_HYST_MS besser ist
 *    (und hoechstens alle LOCROUTE_MIN_GAP_MS), sofort bei Ausfall des aktiven Weges (keine Antwort);
 *  - nach jedem Wechsel bzw. bei verdaechtigem aktivem Weg wird LOCROUTE_DUAL_MS lang doppelt gesendet.
 * Jede Seite waehlt selbst; der Empfaenger nimmt jeden authentifizierten Weg an (kein Abstimmungsprotokoll). */
#pragma once

#define LOCROUTE_MAXP 9           /* 0 = direkt, 1..8 = Relay (TUN_MAX_RELAYS) */
#define LOCROUTE_RING 32          /* Proben fuer die Verlustschaetzung */
#define LOCROUTE_MEASURE_MS 2500.0 /* Messfenster beim Beitritt (mindestens) */
#define LOCROUTE_MEASURE_MAX 8000.0 /* spaetestens dann wird gewaehlt */
#define LOCROUTE_MIN_SAMPLES 8    /* Antworten je Weg fuer eine Bewertung */
#define LOCROUTE_IVL_FAST 100.0   /* Probenabstand im Messfenster (ms) */
#define LOCROUTE_IVL 200.0        /* Probenabstand danach fuer die Alternativen (ms) */
#define LOCROUTE_IVL_ACTIVE 100.0 /* Probenabstand auf dem aktiven Weg (schnelle Ausfallerkennung) */
#define LOCROUTE_IVL_DEAD 1000.0  /* Probenabstand fuer ausgefallene Wege (Wiederkehr erkennen) */
#define LOCROUTE_JIT_W 2.0
#define LOCROUTE_LOSS_W 400.0     /* ms Strafe bei 100 % Verlust (5 % = 20 ms) */
#define LOCROUTE_TIE_MS 5.0       /* direkt bleibt bevorzugt, solange ein Relay nicht um max(5 ms, 10 %) besser ist */
#define LOCROUTE_TIE_REL 0.10
#define LOCROUTE_HYST_MS 25.0     /* Wechsel im Spiel erst ab diesem Vorsprung ... */
#define LOCROUTE_SUSTAIN_MS 3000.0 /* ... ueber diese Dauer */
#define LOCROUTE_MIN_GAP_MS 10000.0 /* kein erneuter Komfortwechsel davor (Ausfall wechselt immer) */
#define LOCROUTE_DUAL_MS 1000.0   /* doppelt senden nach einem Wechsel */
#define LOCROUTE_HYST_REL 0.10    /* W494: Vorsprung mindestens 10 % der Bewertung (weite Wege), sonst HYST_MS */
#define LOCROUTE_FAR_MS 150.0     /* W494: ab dieser Basis (RTT + 4 Schwankung) toleriert die Ausfallgrenze mehr ... */
#define LOCROUTE_FAR_K 1.5        /* ... und zwar 1,5 ms je ms darueber (Burst-Verlust auf weiten Wegen) */

typedef struct locroute_path {
    int avail;                 /* Weg nutzbar (direkt bestaetigt bzw. Relay gebunden) */
    int n;                     /* Antworten seit avail */
    double srtt, jit;          /* ms */
    double avail_at, last_ok;  /* ms: seit wann nutzbar, letzte Antwort */
    double next_probe;         /* ms */
    double ivl;                /* zuletzt genutzter Probenabstand (Schwellen fuer verdaechtig/ausgefallen) */
    unsigned short seq;        /* naechste Probennummer */
    unsigned short rs[LOCROUTE_RING]; /* Ring: Probennummer, Sendezeit, beantwortet */
    double rt[LOCROUTE_RING];
    unsigned char rg[LOCROUTE_RING];
} locroute_path;

typedef struct locroute {
    locroute_path p[LOCROUTE_MAXP];
    int active;                /* aktiver Slot, -1 = keiner */
    int decided;               /* erste Wahl nach dem Messfenster getroffen */
    double start;              /* ms: erster nutzbarer Weg (Beginn des Messfensters), 0 = noch keiner */
    int cand;                  /* Wechselkandidat (-1) */
    double cand_since;
    int dual;                  /* zweiter Sende-Slot (-1 = keiner) */
    double dual_until;
    double last_switch;
    int nswitch, nfail;        /* Wechsel nach der ersten Wahl (besser oder Ausfall) / davon wegen Ausfall */
    double switch_ms;          /* Dauer des letzten Ausfallwechsels: letzte Antwort des alten Weges -> Wechsel */
    int kick;                  /* 1 = offene Nachrichten jetzt (auch ueber den zweiten Weg) neu senden; Transport loescht */
    int nsuspect;              /* Verdachtsfaelle (aktiver Weg ohne rechtzeitige Antwort) */
} locroute;

void locroute_init(locroute *r);
/* W494: Zuschlag der Ausfallgrenze fuer weite Wege (ms) und Wechsel-Vorsprung zur Bewertung score (ms). */
double locroute_far_extra(double base);
double locroute_hyst(double score);
/* Weg nutzbar/nicht mehr nutzbar (Messwerte werden beim Wiedereintritt neu aufgebaut). */
void locroute_set_avail(locroute *r, int slot, int avail, double now);
/* Probe faellig? Liefert 1 und die Probennummer, traegt die Sendung ein. */
int locroute_probe_due(locroute *r, int slot, double now, unsigned short *seq);
/* Antwort auf Probe seq (RTT in ms, aus der eigenen Sendezeit). 0 = unbekannt/zu alt (ignoriert). */
int locroute_probe_ack(locroute *r, int slot, unsigned short seq, double now);
/* Verlust 0..1 aus beantworteten/abgelaufenen Proben im Ring. */
double locroute_loss(const locroute_path *p, double now);
/* Antwortet der Weg noch? 2 = ja, 1 = verdaechtig (Antworten ueberfaellig), 0 = ausgefallen bzw. nicht nutzbar. */
int locroute_alive(const locroute_path *p, double now);
/* Bewertung in ms (kleiner = besser); < 0 = noch keine Bewertung (zu wenig Antworten). */
double locroute_score(const locroute_path *p, double now);
/* Bester bewerteter lebender Weg (direkt bei Gleichstand bevorzugt), -1 = keiner. Ohne Bewertung (fallback=1): erster
 * lebender Weg, direkt zuerst. */
int locroute_best(const locroute *r, double now, int fallback);
/* Ein Schritt (Zeitgeber, ~20 ms): liefert den aktiven Slot (-1 = keiner). ready = Messfenster darf enden (z. B.
 * Treffpunkt hat die Wege genannt); *why = Grund eines Wechsels (0 keiner, 1 erste Wahl, 2 besser, 3 Ausfall,
 * 4 vorlaeufig). */
int locroute_step(locroute *r, double now, int ready, int *why);
/* Zweiter Sende-Slot fuer jetzt (-1 = nur aktiv). */
int locroute_dual(const locroute *r, double now);
