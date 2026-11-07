/* input_tap.c - W92 INPUT-TAPS: Tap-Warteschlange je Taste (s. Header).
 * Harness-unabhaengig (nur C + Win32-Interlocked), testbar in
 * input_selftest.c. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "input_tap.h"

/* UI->Spiel-Uebergabe (Interlocked): */
static volatile LONG t_unseen[256];  /* 1 = laufender Druck noch nie als Pegel gesehen */
static volatile LONG t_pending[256]; /* angestaute fertige Tipper (0..ITAP_MAX) */
/* Spiel-Thread allein (Poll-Wiedergabe): */
static unsigned t_poll;              /* Poll-Zaehler (itap_poll_begin) */
static unsigned t_last[256];         /* Poll der letzten Auswertung */
static unsigned char t_cur[256];     /* Ergebnis im laufenden Poll (Cache) */
static unsigned char t_phase[256];   /* 1 = letzter Poll war angespielter Druck -> jetzt Luecke */
static unsigned char t_prev[256];    /* zuletzt gemeldeter Wert (fuer Luecke nach Live-Druck) */

void itap_init(void)
{
    int i;
    for (i = 0; i < 256; i++) {
        t_unseen[i] = 0;
        t_pending[i] = 0;
        t_last[i] = 0;
        t_cur[i] = 0;
        t_phase[i] = 0;
        t_prev[i] = 0;
    }
    t_poll = 1;
}

void itap_clear(void)
{
    itap_init();
}

void itap_down_edge(int vk)
{
    if (vk <= 0 || vk > 255) return;
    InterlockedExchange(&t_unseen[vk], 1);
}

void itap_up_edge(int vk)
{
    LONG p, np;
    if (vk <= 0 || vk > 255) return;
    /* Nur ein Druck, den noch kein Poll als gehalten gesehen hat, wird
     * angestaut (live gesehener Druck -> kein Phantom-Tipp nach Loslassen).
     * Doppelte down-Flanken (Autowiederholung) setzen unseen nur erneut. */
    if (InterlockedExchange(&t_unseen[vk], 0) != 1) return;
    do {
        p = t_pending[vk];
        if (p >= ITAP_MAX) return;
        np = p + 1;
    } while (InterlockedCompareExchange(&t_pending[vk], np, p) != p);
}

void itap_poll_begin(void)
{
    if (++t_poll == 0) {
        /* Ueberlauf (alle ~2 Jahre bei 60 Hz): Cache ungueltig machen. */
        int i;
        for (i = 0; i < 256; i++) t_last[i] = 0;
        t_poll = 1;
    }
}

int itap_eval(int vk, int phys_down)
{
    int res;
    if (vk <= 0 || vk > 255) return 0;
    if (t_last[vk] == t_poll) return t_cur[vk];
    t_last[vk] = t_poll;
    if (phys_down) {
        /* Gehalten: immer gedrueckt, kein Abspielen (bitgenau wie bisher).
         * Der laufende Druck ist damit live gesehen (kein Nachtrag). */
        InterlockedExchange(&t_unseen[vk], 0);
        res = 1;
        t_phase[vk] = 0;
    } else if (t_phase[vk]) {
        /* Nach angespieltem Druck: genau ein Poll Luecke. */
        res = 0;
        t_phase[vk] = 0;
    } else {
        LONG p = t_pending[vk];
        if (p > 0 && t_prev[vk]) {
            /* Nach Live-Druck: erst ein Poll Luecke, sonst saehe das Spiel
             * Dauer-an statt einer neuen Flanke. */
            res = 0;
        } else if (p > 0) {
            InterlockedDecrement(&t_pending[vk]);
            res = 1;
            t_phase[vk] = 1;
        } else {
            res = 0;
        }
    }
    t_prev[vk] = (unsigned char)res;
    t_cur[vk] = (unsigned char)res;
    return res;
}

int itap_pending(int vk)
{
    if (vk <= 0 || vk > 255) return 0;
    return (int)t_pending[vk];
}
