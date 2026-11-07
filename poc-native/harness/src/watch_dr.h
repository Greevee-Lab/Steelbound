#pragma once
/* watch_dr.h - WATCHDR (W41): Daten-Watchpoints per x86-Debug-Register.
 * cfg: [log] watch=ADDR[:len][:w|rw],... (max 4, DR0-DR3) oder relativ
 *   ent:IDX+OFF[:len][:w|rw] (Entity-Feld: [0x351828]+IDX*0x5C+OFF,
 *   Adresse wird zur Laufzeit aufgeloest und bei Tabellenwechsel neu
 *   bewaffnet). Log: WATCHDR-Zeilen (EIP, alt/neu, Ruecksprungkette,
 *   Frame), pro EIP einmal + Zaehler. Leer = aus (Default). */
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

void watch_dr_init(void); /* cfg parsen + Main-Thread bewaffnen (nach krnl_init) */
void watch_dr_on_thread(void *h, unsigned long tid); /* neuen Xbox-Thread bewaffnen */
void watch_dr_tick(unsigned frame); /* ent:-Reload bei Tabellenwechsel (Swap-Hook) */
/* VEH-Einsprung: 1 = Treffer auf eigenem Slot behandelt (CONTINUE_EXECUTION),
 * 0 = nicht unserer (legacy SINGLE_STEP-Pfade laufen weiter). */
int watch_dr_veh(EXCEPTION_POINTERS *ep);

#ifdef __cplusplus
}
#endif
