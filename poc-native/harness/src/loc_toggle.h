/* loc_toggle.h - W451 LOC-TOGGLE-SYNC: toggle_assist (Kippschalter 1-5) in Line of Contact (GitHub #87).
 * Nur lokale Eingabe im eigenen SBC-Block vor dem Eingabewort (siehe loc_toggle.c). */
#ifndef LOC_TOGGLE_H
#define LOC_TOGGLE_H

#include "input_map.h"

/* Je SBC-Abfrage des Spiel-Threads (hooks_input.c, nach imap_toggles_apply, nur key_toggles + toggle_assist +
 * g_sba_loc). latch = Rast des Abbildungspfads, chg = gerade umgeklappte Schalter, frame = Bildzaehler (Log). */
void loc_toggle_poll(imap_block_t *blk, uint8_t *latch, unsigned chg, unsigned frame);

#endif
