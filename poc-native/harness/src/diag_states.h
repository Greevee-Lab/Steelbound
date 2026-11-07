#pragma once
/* diag_states.h - DIAG(STATES): Modus/Szene-Ueberwachung (A3, s. .c). */
#ifdef __cplusplus
extern "C" {
#endif

void diag_states_tick(unsigned frame);

/* DIAG(STATE): kompakte Missions-Zustandszeile, 1x/s (A11c, s. .c). */
void diag_state_tick(unsigned frame);

/* DIAG(STARTUP): Balken/Linie/Stufe/Zaehler/Engine (W21-R2, s. .c). */
void diag_startup_tick(unsigned frame);

/* DIAG(COMBAT): Entity-Tabelle HP/Schaden (W35, s. .c). */
void diag_combat_tick(unsigned frame);

/* DIAG(GEAR): Gang-Kette Block->Out->Latch + eigene Position (W141, s. .c). */
void diag_gear_tick(unsigned frame);

#ifdef __cplusplus
}
#endif
