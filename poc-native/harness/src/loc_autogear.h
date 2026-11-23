/* loc_autogear.h - W441 LOC-AUTOGEAR: automatischer Gangwechsel (auto_gear) in Line of Contact.
 * Nur lokale Eingabe-Transformation im eigenen SBC-Block vor dem Eingabewort (siehe loc_autogear.c). */
#ifndef LOC_AUTOGEAR_H
#define LOC_AUTOGEAR_H

#include "input_map.h"

/* Je SBC-Abfrage des Spiel-Threads (hooks_input.c, nach der Abbildung). gear_st = Gang-Zustand des
 * Abbildungspfads, gear_direct = Gang-Direktwahl aktiv (Automatik setzt aus). Ohne auto_gear und ohne
 * DIAG (input_diag mit Testfolge) ohne Wirkung. */
void loc_autogear_poll(imap_block_t *blk, uint8_t *gear_st, int gear_direct);
/* Zaehlerzeile LOCGEAR (Wechsel hoch/runter/R, nur Anzahlen). */
void loc_autogear_log(void);

#endif
