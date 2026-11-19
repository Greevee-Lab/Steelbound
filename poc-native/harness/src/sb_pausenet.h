#pragma once
/* sb_pausenet.h - W357 LOC-PAUSE-NET: im Netzspiel haelt kein Client-Overlay den Spieltakt an.
 *
 * Auf der Konsole laeuft ein Netzspiel immer weiter. Haelt ein Client den Takt an (ESC-Menue, F11, Fotomodus,
 * Schnell-Screenshot), steht auch das Spiel des Mitspielers (Lockstep wartet, Rollback staut). Darum gilt im
 * LoC-Netzspiel (Mitspieler verbunden):
 *   - ESC-Menue, Feedback (F11) und Alpha-Hinweis: nur lokal eingeblendet, das Spiel laeuft weiter (Eingaben ans
 *     Spiel sind bei offenem Overlay ohnehin neutral: sb_ovl_input_blocked, imouse_release_keys);
 *   - Pause-Taste und Fotomodus (freie Kamera braucht das stehende Bild): gesperrt, kurzer Hinweis (de/en);
 *   - Schnell-Screenshot und F11-Bild: Bild ohne Pause (GPU-Kopie, Lesen ein Bild spaeter, JPEG im eigenen Thread).
 * SB und LoC allein: unveraendert (Pause wie bisher).
 *
 * Schalter [overlay] pause_net: 0 = aus (immer Pause wie bisher), 1 = automatisch (Standard: nur LoC mit
 * verbundenem Mitspieler), 2 = TEST: immer wie im Netzspiel (Selbsttest/Nachweis ohne Mitspieler).
 *
 * Reine Logik ohne Abhaengigkeiten (Selbsttest: loc_net_selftest "W357"). */

#define SB_PN_HOLD_MS 5000u /* Mitspieler gilt so lange nach dem letzten Lebenszeichen als verbunden */

/* Was angefordert wird */
enum { SB_PN_ESC = 0,  /* ESC-Menue (menu_pause) */
       SB_PN_PAUSEKEY, /* Pause-Taste */
       SB_PN_FEEDBACK, /* F11 / Feedback aus dem Menue */
       SB_PN_PHOTO,    /* Fotomodus (freie Kamera) */
       SB_PN_WELCOME,  /* Alpha-Hinweis */
       SB_PN_QSHOT,    /* Schnell-Screenshot */
       SB_PN_N };

/* Ergebnis */
enum { SB_PN_RUN = 0, /* Spiel laeuft weiter */
       SB_PN_PAUSE,   /* Spiel steht (wie bisher) */
       SB_PN_BLOCK,   /* gesperrt, Hinweis */
       SB_PN_ASYNC }; /* ohne Pause, Bild asynchron */

typedef struct {
    long rx_last;        /* letzter Stand des Empfangszaehlers */
    unsigned rx_ms;      /* GetTickCount der letzten Aenderung */
    int rx_any;          /* rx_ms gueltig (mindestens eine Aenderung) */
    int seen;            /* rx_last gueltig */
} sb_pn_live_t;

/* Lebenszeichen auswerten (je Bild). ready = LoC-Netz an; rx = Empfangszaehler der Spiel-Sockets;
 * links_up = verbundene Mitspieler im eigenen Transport; last34_ms = GetTickCount des letzten eigenen
 * Lockstep-Rahmens (0 = nie); streams = offene Spiel-TCP-Stroeme (connect + accept - Abgaenge). Empfang zaehlt nur
 * bei offenem Strom (allein: eigene Rundrufe sind kein Mitspieler). Der erste Zaehlerstand ist nur Bezug.
 * 1 = Mitspieler verbunden. */
static __inline int sb_pn_live_step(sb_pn_live_t *s, unsigned now_ms, long rx, int links_up, long last34_ms, long streams,
                                    int ready)
{
    if (!ready) {
        s->seen = 0;
        s->rx_any = 0;
        return 0;
    }
    if (!s->seen) {
        s->rx_last = rx;
        s->seen = 1;
    } else if (rx != s->rx_last) {
        s->rx_last = rx;
        if (streams > 0) {
            s->rx_ms = now_ms;
            s->rx_any = 1;
        }
    }
    if (links_up > 0) return 1;
    /* mit Vorzeichen: der Kern setzt GetTickCount()|1 (bis 1 ms "in der Zukunft") */
    if (last34_ms && (int)(now_ms - (unsigned)last34_ms) < (int)SB_PN_HOLD_MS) return 1;
    return s->rx_any && (unsigned)(now_ms - s->rx_ms) < SB_PN_HOLD_MS;
}

/* Netzspiel-Zustand aus Schalter und Lebenszeichen: mode = [overlay] pause_net, loc = LoC-Profil. */
static __inline int sb_pn_net(int mode, int loc, int live)
{
    if (mode == 2) return 1;
    return mode == 1 && loc && live;
}

/* Entscheidung je Anforderung. net = sb_pn_net(...), menu_pause = [overlay] menu_pause. */
static __inline int sb_pn_policy(int what, int net, int menu_pause)
{
    if (!net) {
        if (what == SB_PN_ESC) return menu_pause ? SB_PN_PAUSE : SB_PN_RUN;
        return (what >= 0 && what < SB_PN_N) ? SB_PN_PAUSE : SB_PN_RUN;
    }
    switch (what) {
    case SB_PN_PAUSEKEY:
    case SB_PN_PHOTO: return SB_PN_BLOCK;
    case SB_PN_QSHOT: return SB_PN_ASYNC;
    default: return SB_PN_RUN;
    }
}
