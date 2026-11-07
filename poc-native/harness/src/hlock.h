/* hlock.h - W119 LOADLOCK: Harness-Host-Sperre mit atomarem Owner.
 *
 * Ersetzt CRITICAL_SECTION fuer die SUSPSAFE-geprueften Host-Sperren
 * (g_ht, g_csmap, g_unimpl, g_ctx, g_au). Grund: Windows setzt
 * CRITICAL_SECTION.OwningThread in RtlEnterCriticalSection erst NACH der
 * atomaren Uebernahme (lock btr LockCount; ntdll+0x20 -> Owner-Store +0x2A)
 * und loescht ihn in RtlLeaveCriticalSection VOR der Freigabe. Suspendiert
 * das Spiel (NtSuspendThread) einen Thread genau in diesem Fenster, sieht
 * SUSPSAFE "frei" und laesst ihn suspendiert -> Sperre belegt, alle Threads
 * in cs_for (V_C11gold: Lader steht bei RtlEnterCriticalSection+0x25).
 *
 * hlock: Das Sperrwort IST die Owner-Tid (InterlockedCompareExchange 0->tid,
 * Freigabe InterlockedExchange tid->0). Owner gesetzt <=> Sperre belegt,
 * ohne Zwischenzustand. Reentrant (wie CRITICAL_SECTION). Warten: kurz
 * spinnen, dann WaitOnAddress. Statisch null-initialisiert = frei (keine
 * Init noetig, kein Lazy-Init-Race).
 *
 * DIAG(LOADLOCK): diag_lock_model=1 bildet das ntdll-Verhalten nach (eigenes
 * Sperrbit, Owner getrennt gesetzt/geloescht) - nur zum Nachweis;
 * diag_lock_inject=N weitet bei jedem N-ten Erwerb/Freigabe das Fenster
 * (Modell 1) bzw. die Sperr-Region (Modell 0) per Sleep(1) auf. */
#ifndef HLOCK_H
#define HLOCK_H
#include <windows.h>

typedef struct {
    volatile LONG owner;     /* Tid des Halters, 0 = frei (Modell 0: Sperrwort) */
    volatile LONG locked;    /* nur Modell 1 (ntdll-Nachbildung): Sperrbit */
    volatile LONG waiters;   /* Anzahl in WaitOnAddress */
    LONG recursion;          /* nur vom Halter benutzt */
    volatile LONG model;     /* Modell beim Erwerb (fuer Leave) */
    volatile LONG reg;       /* in SUSPMARK-Liste eingetragen */
    volatile LONG n_cont;    /* DIAG: Erwerbe mit Konkurrenz (gespinnt) */
    volatile LONG n_wait;    /* DIAG: WaitOnAddress-Aufrufe */
} hlock_t;

void hlock_enter(hlock_t *l);
void hlock_leave(hlock_t *l);
/* Owner-Tid (0 = frei bzw. Modell 1 im Fenster). */
DWORD hlock_owner(const hlock_t *l);
/* Belegt (Modell 0: = Owner != 0; Modell 1: Sperrbit). */
int hlock_busy(const hlock_t *l);
/* DIAG(LOADLOCK): Zustand als Text "T1234/r1/w2" bzw. "frei". */
const char *hlock_str(const hlock_t *l, char *buf, size_t cap);
/* DIAG(LOADLOCK): Zahl der injizierten Fenster (diag_lock_inject). */
extern volatile LONG g_hlock_injected;
/* SUSPMARK (safe_suspend_sync=2): Ziel vor SuspendThread markieren
 * (0 = Liste voll -> GetThreadContext), Marke bei NtResumeThread loeschen. */
int hlock_susp_mark(DWORD tid);
void hlock_susp_unmark(DWORD tid);
/* DIAG(LOADLOCK): Zahl der SUSPMARK-Rueckzuege (Ziel gab Sperre zurueck). */
extern volatile LONG g_hlock_backoff;
/* DIAG(LOADLOCK): Injektion nur in diesem Thread (letztes Suspend-Ziel). */
extern volatile DWORD g_hlock_inject_tid;

#endif
