/* hlock.c - W119 LOADLOCK: Harness-Host-Sperre mit atomarem Owner
 * (Begruendung s. hlock.h). */
#include "hlock.h"
#include "hcfg.h"
#include <stdio.h>

volatile LONG g_hlock_injected;
volatile DWORD g_hlock_inject_tid;
static volatile LONG g_hlock_acq;

/* DIAG(LOADLOCK): jedes N-te Ereignis Fenster/Region aufweiten - nur im
 * zuletzt per NtSuspendThread suspendierten Thread (g_hlock_inject_tid,
 * i. d. R. der Lader), damit der Suspender selbst nicht gebremst wird. */
static void hlock_inject(void)
{
    int n = g_cfg.diag_lock_inject;
    if (n > 0 && g_hlock_inject_tid == GetCurrentThreadId() &&
        (InterlockedIncrement(&g_hlock_acq) % n) == 0) {
        InterlockedIncrement(&g_hlock_injected);
        Sleep(1);
    }
}

/* ---- SUSPMARK (W119): asynchrones SuspendThread exakt abfangen ----
 * SuspendThread kehrt zurueck, bevor ein laufendes Ziel wirklich steht; in
 * diesen Mikrosekunden koennte es nach dem SUSPSAFE-Check noch eine Sperre
 * greifen. GetThreadContext (safe_suspend_sync=1) wartet das ab, kostet aber
 * viel (wartet, bis der Idle-Lader auf dem gemeinsamen Kern eingeplant wird:
 * Mission/Demo -40..50 % Bilder, W119_ab_*). safe_suspend_sync=2: der
 * Suspender markiert das Ziel VOR SuspendThread (hlock_susp_mark); greift
 * das Ziel danach eine aeusserste Sperre (haelt keine andere), gibt es sie
 * sofort zurueck und wartet, bis NtResumeThread die Marke loescht. Dekker:
 * Marke setzen (Interlocked) -> SuspendThread -> Owner lesen bzw. Sperre
 * nehmen (Interlocked) -> Marke lesen; eine Seite sieht immer die andere. */
#define HL_PEND_MAX 4
#define HL_REG_MAX 16
static volatile LONG g_pend[HL_PEND_MAX];
static volatile LONG g_pend_n;
static hlock_t *volatile g_reg[HL_REG_MAX];
static volatile LONG g_nreg;
volatile LONG g_hlock_backoff;

int hlock_susp_mark(DWORD tid)
{
    int i;
    for (i = 0; i < HL_PEND_MAX; i++) {
        if ((DWORD)g_pend[i] == tid) {
            return 1;
        }
    }
    for (i = 0; i < HL_PEND_MAX; i++) {
        if (InterlockedCompareExchange(&g_pend[i], (LONG)tid, 0) == 0) {
            InterlockedIncrement(&g_pend_n);
            return 1;
        }
    }
    return 0; /* voll: Aufrufer synchronisiert per GetThreadContext */
}

void hlock_susp_unmark(DWORD tid)
{
    int i;
    for (i = 0; i < HL_PEND_MAX; i++) {
        if (tid && InterlockedCompareExchange(&g_pend[i], 0, (LONG)tid) ==
                       (LONG)tid) {
            InterlockedDecrement(&g_pend_n);
        }
    }
}

static int hl_pending(DWORD me)
{
    int i;
    if (!g_pend_n) {
        return 0;
    }
    for (i = 0; i < HL_PEND_MAX; i++) {
        if ((DWORD)g_pend[i] == me) {
            return 1;
        }
    }
    return 0;
}

static void hl_register(hlock_t *l)
{
    if (InterlockedCompareExchange(&l->reg, 1, 0) == 0) {
        LONG i = InterlockedIncrement(&g_nreg) - 1;
        if (i < HL_REG_MAX) {
            g_reg[i] = l;
        }
    }
}

/* Haelt der Thread ausser l noch eine andere (registrierte) hlock? */
static int hl_holds_other(const hlock_t *l, DWORD me)
{
    LONG i, n = g_nreg;
    for (i = 0; i < n && i < HL_REG_MAX; i++) {
        hlock_t *o = g_reg[i];
        if (o && o != l && (DWORD)o->owner == me) {
            return 1;
        }
    }
    return 0;
}

static void hl_release(hlock_t *l, int m1)
{
    volatile LONG *w;
    if (m1) {
        w = &l->locked;
        l->owner = 0;
        hlock_inject(); /* ntdll-Fenster: Owner 0, noch belegt */
        InterlockedExchange(w, 0);
    } else {
        w = &l->owner;
        InterlockedExchange(w, 0);
    }
    if (l->waiters) {
        WakeByAddressSingle((PVOID)w);
    }
}

void hlock_enter(hlock_t *l)
{
    DWORD me = GetCurrentThreadId();
    int m1 = g_cfg.diag_lock_model == 1;
    volatile LONG *w;
    LONG v;
    int spin = 0, maxspin = g_cfg.hlock_spin > 0 ? g_cfg.hlock_spin : 64;
    if ((DWORD)l->owner == me) {
        l->recursion++;
        return;
    }
    if (!l->reg) {
        hl_register(l);
    }
    w = m1 ? &l->locked : &l->owner;
    v = m1 ? 1 : (LONG)me;
    for (;;) {
        LONG cur = *w;
        if (cur == 0 && InterlockedCompareExchange(w, v, 0) == 0) {
            if (m1) {
                hlock_inject(); /* ntdll-Fenster: belegt, Owner noch 0 */
                l->owner = (LONG)me;
            }
            /* SUSPMARK: nach Erwerb Marke pruefen (s. o.). */
            if (g_pend_n && hl_pending(me) && !hl_holds_other(l, me)) {
                hl_release(l, m1);
                InterlockedIncrement(&g_hlock_backoff);
                while (hl_pending(me)) {
                    SwitchToThread();
                }
                continue;
            }
            break;
        }
        if (spin < maxspin) {
            if (spin == 0) {
                InterlockedIncrement(&l->n_cont);
            }
            spin++;
            YieldProcessor();
            continue;
        }
        InterlockedIncrement(&l->n_wait);
        InterlockedIncrement(&l->waiters);
        cur = *w;
        if (cur != 0) {
            WaitOnAddress(w, &cur, sizeof cur, INFINITE);
        }
        InterlockedDecrement(&l->waiters);
    }
    l->model = m1;
    l->recursion = 1;
    if (!m1) {
        hlock_inject(); /* Region: Owner sichtbar -> SUSPSAFE erkennt */
    }
}

void hlock_leave(hlock_t *l)
{
    if ((DWORD)l->owner != GetCurrentThreadId()) {
        return; /* Leave ohne Besitz: ignorieren (wie fair_cs) */
    }
    if (--l->recursion > 0) {
        return;
    }
    hl_release(l, l->model);
}

DWORD hlock_owner(const hlock_t *l)
{
    return (DWORD)l->owner;
}

int hlock_busy(const hlock_t *l)
{
    return l->owner != 0 || l->locked != 0;
}

const char *hlock_str(const hlock_t *l, char *buf, size_t cap)
{
    DWORD o = (DWORD)l->owner;
    if (!hlock_busy(l)) {
        _snprintf_s(buf, cap, _TRUNCATE, "frei");
    } else if (!o) {
        _snprintf_s(buf, cap, _TRUNCATE, "BELEGT-ohne-Owner/w%ld",
                    (long)l->waiters);
    } else {
        _snprintf_s(buf, cap, _TRUNCATE, "T%04x/r%ld/w%ld", o & 0xFFFFu,
                    (long)l->recursion, (long)l->waiters);
    }
    return buf;
}
