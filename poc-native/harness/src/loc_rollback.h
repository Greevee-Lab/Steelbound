/* loc_rollback.h - W325 LOC-ROLLBACK-SPIKE: Machbarkeit Rollback im LoC-HLE messen
 * (loc_rb_spike=1/2, Standard 0, nur LoC-Profil + Netz-HLE). */
#pragma once
#include <stdint.h>

int loc_rb_install(void);   /* nach loc_sync_install/loc_pace_install (loc_boot.c) */
void loc_rb_tick(int waited_s); /* Watchdog-Takt (Sekunden), Logzeilen LOCRB alle 10 s */

/* W330 LOC-ROLLBACK-R1 (loc_rb=1, nur LoC-Profil). Ohne loc_rb bleiben die Zeiger 0/Null: SB-Pfad unveraendert. */
extern void (*volatile g_loc_rb_prewrite)(uintptr_t a, unsigned n); /* vor Kernel-Schreibzugriffen in Gast-Puffer */
extern int g_loc_rb_ww;                 /* 1 = NtAllocateVirtualMemory mit MEM_WRITE_WATCH reservieren */
extern volatile long g_loc_rb_memev;    /* Freigabe/Schutzwechsel im Gast-Speicher -> Bereiche neu */
#define LOC_RB_PREWRITE(a, n) do { if (g_loc_rb_prewrite) g_loc_rb_prewrite((uintptr_t)(a), (unsigned)(n)); } while (0)
