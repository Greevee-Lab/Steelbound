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
 * W308 SUSP-CTLOCK (GitHub #27): Contig-Muster. Lader L haelt die
 * Contig-Sperre ueber eine lange VirtualQuery-Schleife (HIGH-Suche), der
 * Suspender suspendiert L wie hle_NtSuspendThread, danach will Thread B
 * freigeben (ct_free = Sperre nehmen) - mit Zeitlimit:
 *   ct/srw      rohe SRW-Sperre wie alpha.9 (SUSPSAFE sieht sie nicht):
 *               Gegenprobe, MUSS Haenger zeigen
 *   ct/hlock    hlock + SUSPSAFE-Retry (hlock_owned_by, wie susp_held_lock)
 *               + SUSPMARK: 0 Haenger, 0 Aufgaben
 *
 * Aufruf: hlock_selftest [N=200000] [kernZ=16] [kernS=18] [Nct=100]
 * Exit 0 = hl0/sync und hl0/mark ohne Fehltreffer, ct/hlock ohne Haenger
 * und ct/srw mit Haenger (Gegenprobe). */
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


/* ---- W308 SUSP-CTLOCK: Contig-Muster (GitHub #27) ---- */
static SRWLOCK g_ct_srw = SRWLOCK_INIT;
static hlock_t g_ct_hl;
static volatile LONG g_ct_mode; /* 0 = SRW (alpha.9), 1 = hlock (Fix) */
static volatile LONG g_ct_stop, g_ct_rounds;
static HANDLE g_ct_go, g_ct_done;

static void ct_lock(void)
{
    if (g_ct_mode) { hlock_enter(&g_ct_hl); } else { AcquireSRWLockExclusive(&g_ct_srw); }
}

static void ct_unlock(void)
{
    if (g_ct_mode) { hlock_leave(&g_ct_hl); } else { ReleaseSRWLockExclusive(&g_ct_srw); }
}

/* Lader: Contig-Allokation mit HIGH-Suche (400 VirtualQuery unter der Sperre,
 * wie im Tester-Log: 350-480 Abfragen, ~1,4 ms). */
static DWORD WINAPI ct_loader(LPVOID p)
{
    (void)p;
    while (!g_ct_stop) {
        unsigned long long a = 0x10000ull;
        int i;
        ct_lock();
        for (i = 0; i < 400; i++) {
            MEMORY_BASIC_INFORMATION mi;
            if (!VirtualQuery((LPCVOID)(uintptr_t)a, &mi, sizeof mi)) { a = 0x10000ull; continue; }
            a = (unsigned long long)(uintptr_t)mi.BaseAddress + mi.RegionSize;
            if (a >= 0x7FFF0000ull) { a = 0x10000ull; }
        }
        ct_unlock();
        g_ct_rounds++;
        for (i = 0; i < 64; i++) { YieldProcessor(); }
    }
    return 0;
}

/* B: MmFreeContiguousMemory -> ct_free (Sperre nehmen/geben). */
static DWORD WINAPI ct_freer(LPVOID p)
{
    (void)p;
    for (;;) {
        WaitForSingleObject(g_ct_go, INFINITE);
        if (g_ct_stop) { break; }
        ct_lock();
        ct_unlock();
        SetEvent(g_ct_done);
    }
    return 0;
}

static int run_ct(const char *name, int mode, int n, int coreZ, int *phang)
{
    HANDLE th, tb;
    DWORD tid;
    int i, hang = 0, giveup = 0, retries = 0, held_seen = 0;
    g_ct_mode = mode;
    g_cfg.diag_lock_model = 0;
    g_ct_stop = 0;
    g_ct_rounds = 0;
    g_hlock_backoff = 0;
    g_ct_go = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_ct_done = CreateEventA(NULL, FALSE, FALSE, NULL);
    th = CreateThread(NULL, 0, ct_loader, NULL, 0, &tid);
    tb = CreateThread(NULL, 0, ct_freer, NULL, 0, NULL);
    SetThreadAffinityMask(th, (DWORD_PTR)1 << coreZ);
    Sleep(20);
    for (i = 0; i < n; i++) {
        int marked = 0, r = 0;
        if (mode) { marked = hlock_susp_mark(tid); }
        SuspendThread(th);
        if (mode) {
            /* SUSPSAFE wie hle_NtSuspendThread: ohne Marke erst synchronisieren,
             * Halter kurz fortsetzen und neu suspendieren (max. 200). */
            CONTEXT cx;
            if (!marked) {
                memset(&cx, 0, sizeof cx);
                cx.ContextFlags = CONTEXT_CONTROL;
                GetThreadContext(th, &cx);
            }
            if (hlock_owned_by(tid)) { held_seen++; }
            while (hlock_owned_by(tid) && r < 200) {
                r++;
                ResumeThread(th);
                if ((r & 15) == 0) { Sleep(1); } else { SwitchToThread(); }
                SuspendThread(th);
                if (!marked) {
                    memset(&cx, 0, sizeof cx);
                    cx.ContextFlags = CONTEXT_CONTROL;
                    GetThreadContext(th, &cx);
                }
            }
            retries += r;
            if (hlock_owned_by(tid)) { giveup++; }
        }
        SetEvent(g_ct_go);
        if (WaitForSingleObject(g_ct_done, 50) == WAIT_TIMEOUT) {
            hang++; /* B steht auf der Sperre des suspendierten Laders */
            if (marked) { hlock_susp_unmark(tid); }
            ResumeThread(th);
            WaitForSingleObject(g_ct_done, INFINITE);
        } else {
            if (marked) { hlock_susp_unmark(tid); }
            ResumeThread(th);
        }
        { /* Lader 0..2 ms laufen lassen (zufaellige Phase: meist mitten in der Suche) */
            LARGE_INTEGER f, a, b;
            LONGLONG w;
            QueryPerformanceFrequency(&f);
            QueryPerformanceCounter(&a);
            w = f.QuadPart * (LONGLONG)(rand() % 2000) / 1000000;
            do { YieldProcessor(); QueryPerformanceCounter(&b); } while (b.QuadPart - a.QuadPart < w);
        }
    }
    g_ct_stop = 1;
    SetEvent(g_ct_go);
    WaitForSingleObject(th, INFINITE);
    WaitForSingleObject(tb, INFINITE);
    CloseHandle(th);
    CloseHandle(tb);
    CloseHandle(g_ct_go);
    CloseHandle(g_ct_done);
    printf("%-11s N=%d  Haenger(B>50ms)=%d  gehalten-beim-Suspend=%d  Retries=%d  Aufgaben=%d  Runden=%ld  Rueckzuege=%ld\n",
           name, n, hang, held_seen, retries, giveup, (long)g_ct_rounds, (long)g_hlock_backoff);
    *phang = hang;
    return hang + giveup;
}

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 200000;
    int cz = argc > 2 ? atoi(argv[2]) : 16;
    int cs = argc > 3 ? atoi(argv[3]) : 18;
    int nct = argc > 4 ? atoi(argv[4]) : 100;
    int fix1, fix2, fix3, h_srw = 0, h_hl = 0;
    SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << cs);
    InitializeCriticalSection(&g_cs);
    printf("hlock_selftest: N=%d Ziel-Kern %d, Suspender-Kern %d\n", n, cz, cs);
    run("cs/nosync", 0, 0, 0, n, cz);
    run("cs/sync", 0, 0, 1, n, cz);
    run("hl1/sync", 1, 1, 1, n, cz);
    run("hl0/nosync", 1, 0, 0, n, cz);
    fix1 = run("hl0/sync", 1, 0, 1, n, cz);
    fix2 = run("hl0/mark", 1, 0, 2, n, cz);
    run_ct("ct/srw", 0, nct, cz, &h_srw);
    fix3 = run_ct("ct/hlock", 1, nct, cz, &h_hl);
    printf("ERGEBNIS: hl0/sync %s, hl0/mark %s\n",
           fix1 ? "FEHLER" : "OK (0 Fehltreffer)",
           fix2 ? "FEHLER" : "OK (0 Fehltreffer)");
    printf("ERGEBNIS: ct/hlock %s, Gegenprobe ct/srw %s (%d Haenger)\n",
           fix3 ? "FEHLER" : "OK (0 Haenger)",
           h_srw ? "rot wie erwartet" : "NICHT reproduziert", h_srw);
    return (fix1 || fix2 || fix3 || !h_srw) ? 1 : 0;
}
