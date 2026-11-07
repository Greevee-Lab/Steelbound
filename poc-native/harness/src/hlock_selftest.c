/* hlock_selftest.c - W119 LOADLOCK: Suspend-Fenster der Host-Sperren messen.
 *
 * Ohne Spiel, deterministisch: Ziel-Thread Z nimmt/gibt eine Sperre in
 * Dauerschleife; Suspender S suspendiert Z N-mal, prueft wie SUSPSAFE den
 * Owner und vergleicht mit dem echten Zustand ("Sperre belegt" - nur Z
 * benutzt sie, belegt => Z haelt sie). Fehltreffer = Z haelt die Sperre,
 * der Owner-Check sagt "frei" -> SUSPSAFE liesse Z suspendiert (Deadlock).
 *
 * Varianten:
 *   cs/nosync   CRITICAL_SECTION.OwningThread direkt nach SuspendThread (alt)
 *   cs/sync     dto., aber erst GetThreadContext (SuspendThread asynchron)
 *   hl1/sync    hlock Modell 1 (ntdll-Nachbildung, Owner getrennt)
 *   hl0/nosync  hlock Modell 0 (atomarer Owner), ohne Sync
 *   hl0/sync    hlock Modell 0 + Sync (safe_suspend_sync=1)
 *   hl0/mark    hlock Modell 0 + SUSPMARK (safe_suspend_sync=2, Standard):
 *               Marke vor SuspendThread, Check ohne Sync, Loeschen vor Resume
 * Zusaetzlich "async": Zustand direkt nach SuspendThread != Zustand nach
 * GetThreadContext (Z lief nach der Rueckkehr von SuspendThread weiter).
 *
 * Aufruf: hlock_selftest [N=200000] [kernZ=16] [kernS=18]
 * Exit 0 = hl0/sync und hl0/mark ohne Fehltreffer. */
#include "hlock.h"
#include "hcfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

hcfg_t g_cfg; /* hlock liest diag_lock_model/diag_lock_inject */

static CRITICAL_SECTION g_cs;
static hlock_t g_hl;
static volatile LONG g_stop;
static volatile LONG g_kind; /* 0 = CS, 1 = hlock */
static volatile LONG g_spin;

static DWORD WINAPI target(LPVOID p)
{
    (void)p;
    while (!g_stop) {
        if (g_kind == 0) {
            EnterCriticalSection(&g_cs);
            g_spin++;
            LeaveCriticalSection(&g_cs);
        } else {
            hlock_enter(&g_hl);
            g_spin++;
            hlock_leave(&g_hl);
        }
    }
    return 0;
}

/* Sperre belegt? (nur Z benutzt sie -> belegt = Z haelt sie) */
static int busy(void)
{
    if (g_kind == 0) {
        return (g_cs.LockCount & 1) == 0; /* Vista+: Bit0 = 0 -> belegt */
    }
    return hlock_busy(&g_hl);
}

static DWORD owner(void)
{
    if (g_kind == 0) {
        return (DWORD)(uintptr_t)g_cs.OwningThread;
    }
    return hlock_owner(&g_hl);
}

/* sync: 0 = ohne, 1 = GetThreadContext, 2 = SUSPMARK */
static int run(const char *name, int kind, int model, int sync, int n,
               int coreZ)
{
    HANDLE th;
    DWORD tid;
    int i, miss = 0, held = 0, async = 0;
    CONTEXT cx;
    g_kind = kind;
    g_cfg.diag_lock_model = model;
    g_stop = 0;
    g_spin = 0;
    g_hlock_backoff = 0;
    th = CreateThread(NULL, 0, target, NULL, 0, &tid);
    SetThreadAffinityMask(th, (DWORD_PTR)1 << coreZ);
    Sleep(20);
    for (i = 0; i < n; i++) {
        int b0, b1;
        DWORD o;
        if (sync == 2) {
            hlock_susp_mark(tid);
        }
        SuspendThread(th);
        b0 = busy();
        if (sync == 1) {
            memset(&cx, 0, sizeof cx);
            cx.ContextFlags = CONTEXT_CONTROL;
            GetThreadContext(th, &cx);
        }
        o = owner(); /* SUSPSAFE-Check */
        if (sync != 1) {
            /* Wahrheit erst nach erzwungener Synchronisation messen. */
            memset(&cx, 0, sizeof cx);
            cx.ContextFlags = CONTEXT_CONTROL;
            GetThreadContext(th, &cx);
        }
        b1 = busy();
        if (b0 != b1) {
            async++;
        }
        if (b1) {
            held++;
            if (o != tid) {
                miss++;
            }
        }
        if (sync == 2) {
            hlock_susp_unmark(tid);
        }
        ResumeThread(th);
        if ((i & 7) == 0) {
            SwitchToThread();
        }
    }
    g_stop = 1;
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
    printf("%-11s N=%d  gehalten=%d  Fehltreffer(Owner sagt frei)=%d  async=%d  Runden=%ld  Rueckzuege=%ld\n",
           name, n, held, miss, async, (long)g_spin, (long)g_hlock_backoff);
    return miss;
}

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 200000;
    int cz = argc > 2 ? atoi(argv[2]) : 16;
    int cs = argc > 3 ? atoi(argv[3]) : 18;
    int fix1, fix2;
    SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << cs);
    InitializeCriticalSection(&g_cs);
    printf("hlock_selftest: N=%d Ziel-Kern %d, Suspender-Kern %d\n", n, cz, cs);
    run("cs/nosync", 0, 0, 0, n, cz);
    run("cs/sync", 0, 0, 1, n, cz);
    run("hl1/sync", 1, 1, 1, n, cz);
    run("hl0/nosync", 1, 0, 0, n, cz);
    fix1 = run("hl0/sync", 1, 0, 1, n, cz);
    fix2 = run("hl0/mark", 1, 0, 2, n, cz);
    printf("ERGEBNIS: hl0/sync %s, hl0/mark %s\n",
           fix1 ? "FEHLER" : "OK (0 Fehltreffer)",
           fix2 ? "FEHLER" : "OK (0 Fehltreffer)");
    return (fix1 || fix2) ? 1 : 0;
}
