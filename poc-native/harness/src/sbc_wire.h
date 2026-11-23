#pragma once
/* sbc_wire.h - Verdrahtung Original-Steel-Battalion-Controller -> Spiel
 * (W75, CTRL-SBC; CONTROLLER.md §9).
 *
 * Der Hook (hooks_input.c) fragt pro Poll den aktuellen Zustand ab und
 * bekommt ihn bereits als dekodierten sbc_state_t - egal ob vom echten
 * Geraet (WinUSB, sbc_usb=1) oder aus einer Fixture-Datei (sbc_fixture=
 * gesetzt). LEDs laufen umgekehrt ueber sbc_wire_setstate.
 * Alles hier ist nie blockierend (USB laeuft ueber Lese-Thread + Cache).
 */
#include "input_sbc_usb.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Aktuellen SBC-Zustand holen. Rueckgabe 1 = *st gueltig (Quelle aktiv),
 * 0 = Pfad inaktiv oder keine Quelle (Hook faehrt Tastatur pur).
 * sbc_usb=0 -> immer 0 (bitgenau heutiges Verhalten). */
int sbc_wire_poll(sbc_state_t *st);
/* W439: verworfene Ausreisser des Filters ctrl_despike (Zaehler, F11-Paket) */
unsigned sbc_wire_despike_spikes(void);

/* XInputSetState-Verdrahtung (Schritt 5, LEDs): Feedback-Struct des Spiels
 * (fb = 0x4AF0C0 + Port*0x56) auswerten, LED-Report ans Geraet senden
 * (gedrosselt) bzw. ohne Geraet nur loggen. Nur mit sbc_usb=1 aufrufen. */
void sbc_wire_setstate(uint32_t fb);

/* W128: Zustand fuer die ESC-Statuszeile. Nur lesen, oeffnet nichts. */
enum {
    SBC_WIRE_OFF = 0,      /* sbc_usb=0 */
    SBC_WIRE_PENDING = 1,  /* noch nicht geoeffnet (kein Poll bisher) */
    SBC_WIRE_USB_OK = 2,   /* Geraet offen */
    SBC_WIRE_NODEV = 3,    /* nicht gefunden / Treiber fehlt */
    SBC_WIRE_FIXTURE = 4,  /* Fixture-Datei statt Geraet */
    SBC_WIRE_LOST = 5      /* W305: war verbunden, abgezogen - wird gesucht
                              (sbc_wire_poll liefert neutralen Stand) */
};
int sbc_wire_status(void);

/* W305 (JOY-HOTPLUG): Verbindungs-Ereignis fuer einen Hinweis im Overlay
 * abholen und zuruecksetzen: 0 = nichts, 1 = getrennt, 2 = verbunden. */
int sbc_wire_take_event(void);

/* W305: Diagnose (Pfad WinUSB/Fixture/aus, Geraetezustand, LED-Ausgabe,
 * Anzahl LED-Aufrufe/-Sendungen/-Fehler, Oeffnungen). Nie Tasten- oder
 * Achsenwerte. sbc_wire_log_summary loggt nur mit sbc_usb=1. */
#include <stddef.h>
void sbc_wire_summary(char *out, size_t cap);
void sbc_wire_log_summary(const char *why);

#ifdef __cplusplus
}
#endif
