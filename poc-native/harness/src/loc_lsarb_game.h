/* loc_lsarb_game.h - LOC-LOCKSTEP-ARBITER: Anbindung an das Spiel (loc_lsarb_game.c), nur LoC-Profil. */
#pragma once

/* Nach loc_rb_install (loc_boot.c): Thunk @0x1D0BF bei loc_ls_arbiter=1 oder loc_sync=1; 1 = aktiv */
int loc_lsarb_install(void);
/* Ziel des Thunks; der Lockstep-Zweig von rbi_reg_thunk (loc_rollback.c) springt ebenfalls hierher */
void loc_lsarb_reg_entry(void);
/* F3-Overlay: 1 = Lockstep-Mission laeuft (letzter Tick < 2 s), mode 1 = neuer Regler, lead = Vorlauf in Ticks,
 * tk = 1 wenn wir Taktgeber sind */
int loc_lsarb_hud(int *mode, int *lead, int *tk);
