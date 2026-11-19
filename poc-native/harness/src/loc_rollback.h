/* loc_rollback.h - W325 LOC-ROLLBACK-SPIKE: Machbarkeit Rollback im LoC-HLE messen
 * (loc_rb_spike=1/2, Standard 0, nur LoC-Profil + Netz-HLE). */
#pragma once
#include <stdint.h>

int loc_rb_install(void);   /* nach loc_sync_install/loc_pace_install (loc_boot.c) */
void loc_rb_tick(int waited_s); /* Watchdog-Takt (Sekunden), Logzeilen LOCRB alle 10 s */
/* W353 LOC-DURCHSTICH: Rollback-Zustand fuer das F11-Paket (nur Zahlen/Zustaende). 0 = Eingabeschicht aus. */
int loc_rb_diag(char *buf, int cap);

/* W330 LOC-ROLLBACK-R1 (loc_rb=1, nur LoC-Profil). Ohne loc_rb bleiben die Zeiger 0/Null: SB-Pfad unveraendert. */
extern void (*volatile g_loc_rb_prewrite)(uintptr_t a, unsigned n); /* vor Kernel-Schreibzugriffen in Gast-Puffer */
extern int g_loc_rb_ww;                 /* 1 = NtAllocateVirtualMemory mit MEM_WRITE_WATCH reservieren */
extern volatile long g_loc_rb_memev;    /* Freigabe/Schutzwechsel im Gast-Speicher -> Bereiche neu */
/* W346 R3: Allokation im Spiel-Thread waehrend R2 (NtAllocateVirtualMemory, base 0). op 0 = beim Neurechnen aus der
 * Aufzeichnung bedienen (1 = bedient, *io = Adresse, Inhalt genullt), op 1 = Ergebnis *io aufzeichnen. NULL = aus.
 * W355: Commit in einen reservierten Bereich (base != 0): op 2 = beim Neurechnen bedienen (*io = base ein, Adresse aus,
 * frische Seiten genullt, kein Registereintrag), op 3 = Ergebnis aufzeichnen (*io = Adresse | 1 wenn frisch). */
extern int (*volatile g_loc_rb_alloc)(int op, unsigned size, unsigned type, unsigned prot, unsigned *io);
#define LOC_RB_PREWRITE(a, n) do { if (g_loc_rb_prewrite) g_loc_rb_prewrite((uintptr_t)(a), (unsigned)(n)); } while (0)
