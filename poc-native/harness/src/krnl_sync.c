/* krnl_sync.c - Kernel-HLE: Handle-Tabelle, Timeouts, Critical-Section-Map,
 * Xbox-Threads, Sync-Objekte/Waits, SUSPSAFE-Suspend, Zeit, KEVENT-Tabelle.
 * W68 SPLIT: 1:1 aus kernel_hle.c verschoben. */
#include "krnl_int.h"

static ht_entry_t g_ht[XB_HANDLE_MAX];
static hlock_t g_ht_cs; /* W119 LOADLOCK: hlock (atomarer Owner) */

void ht_init_once(void)
{
    /* W119: hlock ist statisch null-initialisiert (frei), keine Init. */
}

u32 ht_alloc(ht_kind_t kind, HANDLE host)
{
    unsigned i;
    ht_init_once();
    hlock_enter(&g_ht_cs);
    for (i = 0; i < XB_HANDLE_MAX; i++) {
        if (g_ht[i].kind == HT_FREE) {
            g_ht[i].host = host; /* Commit-Reihenfolge: kind als
             * LETZTES (Leser ohne Lock sehen FREE oder konsistent). */
            g_ht[i].path[0] = '\0';
            g_ht[i].is_dir = 0;
            g_ht[i].delete_on_close = 0;
            g_ht[i].dir_state = 0;
            g_ht[i].seek_pos = 0;
            g_ht[i].seek_valid = 0;
            g_ht[i].stale = NULL;
            g_ht[i].stale_len = 0;
            g_ht[i].vfs = 0; /* W150 */
            g_ht[i].vfs_node = -1;
            g_ht[i].vfs_pos = 0;
            g_ht[i].vfs_dir_k = 0;
            g_ht[i].vfs_mask[0] = 0;
            g_ht[i].kind = kind;
            hlock_leave(&g_ht_cs);
            return XB_HANDLE_BASE + i;
        }
    }
    hlock_leave(&g_ht_cs);
    return 0;
}

ht_entry_t *ht_get(u32 xh)
{
    static ht_entry_t pseudo_thread;
    static ht_entry_t pseudo_process;
    if (xh == 0xFFFFFFFFu) { /* CURRENT_THREAD */
        pseudo_thread.kind = HT_THREAD;
        pseudo_thread.host = GetCurrentThread();
        return &pseudo_thread;
    }
    if (xh == 0xFFFFFFFEu) { /* CURRENT_PROCESS */
        pseudo_process.kind = HT_THREAD;
        pseudo_process.host = GetCurrentProcess();
        return &pseudo_process;
    }
    if (xh < XB_HANDLE_BASE || xh >= XB_HANDLE_BASE + XB_HANDLE_MAX) {
        return NULL;
    }
    if (g_ht[xh - XB_HANDLE_BASE].kind == HT_FREE) {
        return NULL;
    }
    return &g_ht[xh - XB_HANDLE_BASE];
}

void ht_free(u32 xh)
{
    hlock_enter(&g_ht_cs);
    if (xh >= XB_HANDLE_BASE && xh < XB_HANDLE_BASE + XB_HANDLE_MAX) {
        /* THREAD-Slots nie freigeben: stale Suspend/Resume/Terminate
         * nach Wiederverwendung traefe den FALSCHEN Thread (Hang
         * boot302: Loader 165212 suspendiert). 2048 Slots reichen. */
        if (g_ht[xh - XB_HANDLE_BASE].kind != HT_THREAD) {
            g_ht[xh - XB_HANDLE_BASE].kind = HT_FREE;
        }
    }
    hlock_leave(&g_ht_cs);
}

/* ---------- Timeout: 100ns (negativ=relativ) -> Millisekunden ---------- */
static DWORD xb_timeout_ms(const int64_t *t)
{
    int64_t v;
    int64_t ms;
    if (!t) {
        return INFINITE;
    }
    v = *t;
    if (v >= 0) {
        /* absolut: Delta zu jetzt (FILETIME-Basis) */
        FILETIME ft;
        uint64_t now;
        GetSystemTimeAsFileTime(&ft);
        now = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
        if ((uint64_t)v <= now) {
            return 0;
        }
        ms = (int64_t)(((uint64_t)v - now) / 10000);
    } else {
        ms = (-v) / 10000;
    }
    if (ms > (int64_t)(INFINITE - 1)) {
        return INFINITE - 1;
    }
    return (DWORD)ms;
}

/* ---------- Critical-Section-Map (Xbox-Adresse -> Host-CS) ----------
 * FAIRCS (31.10.): Host-CRITICAL_SECTIONs sind seit Vista/2003 SP1
 * UNFAIR: wer Leave+Enter in Folge macht, ueberholt den Wartenden.
 * Xbox-Kernel (NT5) uebergibt bei Leave direkt an den Wartenden
 * (LockCount-Reservierung + Auto-Reset-Event, kein Retry). Spiel verlaesst
 * sich darauf: Main gibt eine CS pro Frame kurz frei, Loader wartet ->
 * mit Host-CS nur alle 100-500 s ein Treffer ("NOW LOADING"-Hang nach
 * effect.efp, retry1.log). fair_cs=1 bildet NT5 nach, 0 = alter Host-CS. */
#define CS_MAP_MAX 256
typedef struct {
    volatile LONG lock_count; /* -1 = frei, >=0 = Halter + Wartende */
    volatile DWORD owner;
    LONG recursion;
    HANDLE ev; /* Auto-Reset: 1 SetEvent = 1 Uebergabe */
} fair_cs_t;
typedef struct {
    u32 xaddr;
    CRITICAL_SECTION cs;
    fair_cs_t f;
} cs_slot_t;
static cs_slot_t g_csmap[CS_MAP_MAX];
static hlock_t g_csmap_cs; /* W119 LOADLOCK: hlock (atomarer Owner) */

static cs_slot_t *cs_for(u32 xaddr, int create)
{
    unsigned i;
    hlock_enter(&g_csmap_cs);
    for (i = 0; i < CS_MAP_MAX; i++) {
        if (g_csmap[i].xaddr == xaddr) {
            hlock_leave(&g_csmap_cs);
            return &g_csmap[i];
        }
    }
    if (create) {
        for (i = 0; i < CS_MAP_MAX; i++) {
            if (g_csmap[i].xaddr == 0) {
                InitializeCriticalSection(&g_csmap[i].cs);
                g_csmap[i].f.lock_count = -1;
                g_csmap[i].f.owner = 0;
                g_csmap[i].f.recursion = 0;
                g_csmap[i].f.ev = CreateEventA(NULL, FALSE, FALSE, NULL);
                g_csmap[i].xaddr = xaddr; /* zuletzt: Slot erst jetzt sichtbar */
                hlock_leave(&g_csmap_cs);
                return &g_csmap[i];
            }
        }
    }
    hlock_leave(&g_csmap_cs);
    return NULL;
}

static void fair_cs_enter(fair_cs_t *f)
{
    DWORD me = GetCurrentThreadId();
    if (f->owner == me) {
        f->recursion++;
        return;
    }
    if (InterlockedIncrement(&f->lock_count) != 0) {
        /* Platz reserviert: Leave des Halters uebergibt per Event. */
        WaitForSingleObject(f->ev, INFINITE);
    }
    f->owner = me;
    f->recursion = 1;
}

static void fair_cs_leave(fair_cs_t *f)
{
    if (f->owner != GetCurrentThreadId()) {
        return; /* Leave ohne Besitz: ignorieren statt Zaehler zerstoeren */
    }
    if (--f->recursion > 0) {
        return;
    }
    f->owner = 0;
    if (InterlockedDecrement(&f->lock_count) >= 0) {
        SetEvent(f->ev); /* Wartender wird Besitzer (NT5-Uebergabe) */
    }
}

/* DIAG (FAIRCS): lange CS-Wartezeiten sichtbar machen (pro Caller 1x). */
static void cs_wait_log(u32 xaddr, u32 caller, DWORD took)
{
    static u32 seen[32];
    static int nseen;
    int i;
    if (took < 1000u) return;
    for (i = 0; i < nseen; i++) {
        if (seen[i] == caller) return;
    }
    if (nseen >= 32) return;
    seen[nseen++] = caller;
    HLOG_W("KRNL CS-WAIT: cs=0x%X call=0x%X took=%lums fair=%d (#%d)",
           xaddr, caller, took, g_cfg.fair_cs, nseen);
}

/* ---------- Xbox-Threads ---------- */
typedef struct {
    u32 start_routine; /* Xbox-VA */
    u32 start_context;
    u32 system_routine; /* Xbox-VA oder 0 */
} xb_thread_param_t;

/* Zentraler Thread-Einstieg: blendet Loader-PE-Header ein, da CreateThread
 * sie validiert (RtlImageNtHeader), das Spiel sie aber ueberschrieben hat. */
void *krnl_CreateThread(void *fn, void *param, unsigned long flags,
                          unsigned long *tid)
{
    /* DIAG(HDRSWAP,W94): Ueberlappende Tausch-Fenster zaehlen. Zwei
     * gleichzeitige Aufrufe (z. B. Spiel-PsCreateSystemThreadEx + Audio-
     * Mixer-Start aus einem anderen Xbox-Thread, ~6 s nach Start) sichern
     * den bereits eingetauschten Loader-Header als "Spiel-Stand" und
     * stellen ihn zurueck -> Spiel liest dauerhaft PE-Bytes. */
    static volatile LONG in_swap, n_overlap, n_lost;
    HANDLE h;
    LONG depth;
    if (xbe_hdr_hybrid_active()) { /* FIX(HDRSWAP,W94): kein Tausch noetig */
        return CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)fn, param,
                            (DWORD)flags, (DWORD *)tid);
    }
    depth = InterlockedIncrement(&in_swap);
    if (depth > 1) {
        LONG k = InterlockedIncrement(&n_overlap);
        if (k <= 8 || (k & 0xFF) == 0) {
            HLOG_W("HDRSWAP: Ueberlappung #%ld (Tiefe %ld, T%04x) - Spiel-Header gefaehrdet",
                   k, depth, GetCurrentThreadId());
        }
    }
    xbe_swap_headers(g_img, 1);
    h = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)fn, param, (DWORD)flags,
                     (DWORD *)tid);
    xbe_swap_headers(g_img, 0);
    InterlockedDecrement(&in_swap);
    /* Spiel-Header nach dem Ruecktausch noch da? (Zertifikat-Zeiger) */
    if (g_img && g_img->file &&
        *(volatile u32 *)(uintptr_t)(g_img->base + 0x118u) !=
            *(const u32 *)(g_img->file + 0x118u)) {
        LONG k = InterlockedIncrement(&n_lost);
        if (k <= 8) {
            HLOG_E("HDRSWAP: Spiel-Header verloren #%ld: [0x%X]=0x%X statt 0x%X (T%04x)",
                   k, g_img->base + 0x118u,
                   *(volatile u32 *)(uintptr_t)(g_img->base + 0x118u),
                   *(const u32 *)(g_img->file + 0x118u), GetCurrentThreadId());
        }
    }
    return h;
}

/* Xbox-Threads fuer den EIP-Sampler (emu_main) */
#define XB_TR_MAX 32
static struct {
    HANDLE h;
    u32 routine;
} g_xb_threads[XB_TR_MAX];
static int g_xb_nthreads;
HANDLE g_main_thread_h; /* W22: Duplikat des Main-Handles (CPU-DIAG) */

void *krnl_main_thread(void)
{
    return g_main_thread_h;
}

int krnl_xb_thread_count(void)
{
    return g_xb_nthreads;
}

int krnl_xb_thread_get(int i, void **h, u32 *routine)
{
    if (i < 0 || i >= g_xb_nthreads) {
        return 0;
    }
    *h = g_xb_threads[i].h;
    *routine = g_xb_threads[i].routine;
    return 1;
}

/* Borrowed Win32-Handle fuer XBOX-File-Handle (XAPI-Hooks, z.B. ReadFile).
 * NICHT schliessen! Gibt NULL bei ungueltigem/keinem File-Handle. */
void *krnl_file_host(u32 xh)
{
    ht_entry_t *e = ht_get(xh);
    if (!e || e->kind != HT_FILE) {
        return NULL;
    }
    return e->host;
}

/* Borrowed Win32-Handle fuer XBOX-Event-Handle (ReadFile-Overlapped).
 * NICHT schliessen! NULL bei ungueltig/kein Event. */
void *krnl_event_host(u32 xh)
{
    ht_entry_t *e = ht_get(xh);
    if (!e || e->kind != HT_EVENT) {
        return NULL;
    }
    return e->host;
}

/* XAPILIB GetExitCodeThread (0x190D21, statisch): Xbox-Handles sind
 * 0x100+Slot (keine Pointer) -> Win32-Exit-Code via Host-Handle. */
unsigned long __stdcall krnl_GetExitCodeThread(unsigned long xh,
                                               unsigned long *code)
{
    ht_entry_t *e = ht_get((u32)xh);
    DWORD c = 0x103; /* STILL_ACTIVE */
    if (e && e->kind == HT_THREAD && e->host) {
        if (!GetExitCodeThread(e->host, &c)) {
            c = 0x103;
        }
    }
    if (code) {
        *code = c;
    }
    return 1; /* TRUE */
}

static DWORD WINAPI xb_thread_wrapper(LPVOID p)
{
    xb_thread_param_t *tp = (xb_thread_param_t *)p;
    typedef void (__stdcall *xbox_start_fn)(u32);
    typedef u32 (__stdcall *xbox_sys_fn)(u32, u32);

    fs_emu_alloc_thread(); /* Fake-KTHREAD + TLS-Puffer */
    HLOG_I("KRNL: Xbox-Thread startet (routine=0x%X ctx=0x%X sys=0x%X)",
           tp->start_routine, tp->start_context, tp->system_routine);
    if (tp->system_routine) {
        /* Startup ruft selbst die Spiel-Routine und kehrt nie zurueck
         * (endet in PsTerminateSystemThread). Slot[0]-Prefill passiert
         * in startup via XapiNotify-Wrapper (xdk_hooks). */
        ((xbox_sys_fn)(uintptr_t)tp->system_routine)(tp->start_routine, tp->start_context);
    } else {
        fs_emu_prefill_xapi();
        ((xbox_start_fn)(uintptr_t)tp->start_routine)(tp->start_context);
    }
    free(tp);
    HLOG_I("KRNL: Xbox-Thread endet");
    return 0;
}

/* ----- Critical Sections ----- */
u32 __stdcall hle_RtlInitializeCriticalSection(u32 cs)
{
    if (!cs_for(cs, 1)) {
        HLOG_E("CS-Map voll!");
    }
    return X_STATUS_SUCCESS;
}

u32 __stdcall hle_RtlEnterCriticalSection(u32 cs)
{
    cs_slot_t *s = cs_for(cs, 1);
    if (s) {
        DWORD t0 = GetTickCount();
        if (g_cfg.fair_cs) {
            fair_cs_enter(&s->f);
        } else if (!TryEnterCriticalSection(&s->cs)) {
            EnterCriticalSection(&s->cs);
        }
        cs_wait_log(cs, (u32)(uintptr_t)_ReturnAddress(), GetTickCount() - t0);
    }
    return X_STATUS_SUCCESS;
}

u32 __stdcall hle_RtlLeaveCriticalSection(u32 cs)
{
    cs_slot_t *s = cs_for(cs, 0);
    if (s) {
        if (g_cfg.fair_cs) {
            fair_cs_leave(&s->f);
        } else {
            LeaveCriticalSection(&s->cs);
        }
    }
    return X_STATUS_SUCCESS;
}

/* ----- Sync-Objekte ----- */
u32 __stdcall hle_NtCreateEvent(u32 ph, u32 attr, u32 type, u32 initial)
{
    HANDLE h;
    u32 xh;
    (void)attr;
    h = CreateEvent(NULL, type == X_NotificationEvent ? TRUE : FALSE,
                    initial ? TRUE : FALSE, NULL);
    if (!h) {
        return X_STATUS_NO_MEMORY;
    }
    xh = ht_alloc(HT_EVENT, h);
    *(u32 *)(uintptr_t)ph = xh;
    return X_STATUS_SUCCESS;
}

u32 __stdcall hle_NtCreateMutant(u32 ph, u32 attr, u32 owner)
{
    HANDLE h;
    u32 xh;
    (void)attr;
    h = CreateMutex(NULL, owner ? TRUE : FALSE, NULL);
    if (!h) {
        return X_STATUS_NO_MEMORY;
    }
    xh = ht_alloc(HT_MUTANT, h);
    *(u32 *)(uintptr_t)ph = xh;
    return X_STATUS_SUCCESS;
}

u32 __stdcall hle_NtSetEvent(u32 xh, u32 pPrev)
{
    ht_entry_t *e = ht_get(xh);
    if (!e || (e->kind != HT_EVENT && e->kind != HT_MUTANT)) {
        return X_STATUS_INVALID_HANDLE;
    }
    SetEvent(e->host);
    if (pPrev) {
        *(u32 *)(uintptr_t)pPrev = 0;
    }
    return X_STATUS_SUCCESS;
}

u32 __stdcall hle_NtClearEvent(u32 xh)
{
    ht_entry_t *e = ht_get(xh);
    if (!e || e->kind != HT_EVENT) {
        return X_STATUS_INVALID_HANDLE;
    }
    ResetEvent(e->host);
    return X_STATUS_SUCCESS;
}

u32 __stdcall hle_NtReleaseMutant(u32 xh, u32 pPrev)
{
    ht_entry_t *e = ht_get(xh);
    if (!e || e->kind != HT_MUTANT) {
        return X_STATUS_INVALID_HANDLE;
    }
    if (pPrev) {
        *(u32 *)(uintptr_t)pPrev = 0;
    }
    /* DIAG (boot490): Halter-Wechsel sichtbar (Deadlock-Jagd!). */
    { static unsigned nrel, lastrel; nrel++;
      if (nrel - lastrel >= 5000u) { lastrel = nrel;
          HLOG_I("KRNL rel-mut: h=0x%X total=%u", xh, nrel); } }
    return ReleaseMutex(e->host) ? X_STATUS_SUCCESS : X_STATUS_INVALID_HANDLE;
}

static u32 wait_result(DWORD r)
{
    if (r == WAIT_OBJECT_0) {
        return X_STATUS_WAIT_0;
    }
    if (r == WAIT_TIMEOUT) {
        return X_STATUS_TIMEOUT;
    }
    if (r == WAIT_ABANDONED) {
        return X_STATUS_ABANDONED;
    }
    return X_STATUS_INVALID_HANDLE;
}

u32 __stdcall hle_NtWaitForSingleObject(u32 xh, u32 alertable, u32 pTimeout)
{
    ht_entry_t *e = ht_get(xh);
    DWORD ms, r;
    if (!e) {
        return X_STATUS_INVALID_HANDLE;
    }
    if (e->kind == HT_FILE && e->vfs) {
        return X_STATUS_WAIT_0; /* W150: Abbild-I/O ist synchron fertig (signalisiert) */
    }
    ms = xb_timeout_ms((const int64_t *)(uintptr_t)pTimeout);
    /* A5: MUTANT-Polling entfernt (W3_mpA/B gleich, echte INFINITE-Waits
     * kehren sofort zurueck). */
    r = alertable ? WaitForSingleObjectEx(e->host, ms, TRUE)
                  : WaitForSingleObject(e->host, ms);
    return wait_result(r);
    /* unreachable */
    return X_STATUS_SUCCESS;
}

u32 __stdcall hle_NtWaitForSingleObjectEx(u32 xh, u32 mode, u32 alertable,
                                                 u32 pTimeout)
{
    (void)mode;
    return hle_NtWaitForSingleObject(xh, alertable, pTimeout);
}

u32 __stdcall hle_NtWaitForMultipleObjectsEx(u32 count, u32 pHandles, u32 waitType,
                                                    u32 mode, u32 alertable, u32 pTimeout)
{
    HANDLE hs[MAXIMUM_WAIT_OBJECTS];
    u32 i;
    DWORD ms, r;
    (void)mode;
    if (count == 0 || count > MAXIMUM_WAIT_OBJECTS) {
        return X_STATUS_INVALID_PARAMETER;
    }
    for (i = 0; i < count; i++) {
        ht_entry_t *e = ht_get(((u32 *)(uintptr_t)pHandles)[i]);
        if (!e) {
            return X_STATUS_INVALID_HANDLE;
        }
        hs[i] = e->host;
    }
    ms = xb_timeout_ms((const int64_t *)(uintptr_t)pTimeout);
    /* BIKWAITFIX-REVERT: 0x101/102 sind System-Events (Menue-Logik!),
     * Fake stoert Menue (Load-Loop). Nur DELAYFIX (Bink-Thread wach). */
    /* A5: MULTI-Polling entfernt (s. Single-Wait). */
    r = alertable ? WaitForMultipleObjectsEx(count, hs, waitType == 0, ms, TRUE)
                  : WaitForMultipleObjects(count, hs, waitType == 0, ms);
    if (r >= WAIT_OBJECT_0 && r < WAIT_OBJECT_0 + count) {
        return X_STATUS_WAIT_0 + (r - WAIT_OBJECT_0);
    }
    if (r >= WAIT_ABANDONED_0 && r < WAIT_ABANDONED_0 + count) {
        return X_STATUS_ABANDONED + (r - WAIT_ABANDONED_0);
    }
    return wait_result(r);
}

/* ---------- SUSPSAFE (W10): nie suspendiert in einer Host-Sperre ----------
 * V_W4a-Deadlock: Lade-Thread (0x17F20) steht suspendiert in ht_alloc (EIP in
 * RtlLeaveCriticalSection, haelt g_ht_cs), Main blockiert in ht_alloc/
 * EnterCriticalSection auf dasselbe Schloss - und Main ist der einzige
 * Thread, der den Lader je fortsetzen wuerde. Auf der Xbox gibt es diese
 * Host-Sperren nicht. safe_suspend=1: nach SuspendThread pruefen, ob das Ziel
 * eine Harness-Sperre haelt (Owner-Tid) -> kurz fortsetzen und erneut
 * versuchen (begrenzt; alle Sperr-Regionen sind kurz und wartefrei).
 * W119 LOADLOCK: Die fruehere Annahme "SuspendThread kehrt erst zurueck,
 * wenn das Ziel steht" gilt NICHT (Suspend ist asynchron) -> GetThreadContext
 * synchronisiert (safe_suspend_sync=1). Und CRITICAL_SECTION.OwningThread
 * hinkt dem Sperrwort hinterher (Fenster in Rtl{Enter,Leave}CriticalSection)
 * -> Host-Sperren sind jetzt hlocks (Owner = Sperrwort, s. hlock.h). Erst
 * beides zusammen macht den Check exakt: steht das Ziel ohne Sperre, kann es
 * danach keine mehr greifen.
 * Zaehler-Semantik: jedes Retry = Resume+Suspend (netto 0), der Aufruf
 * suspendiert netto genau 1x, prev = letztes SuspendThread-Ergebnis. hlog
 * (TryEnter+Drop) und die fair_cs-Spiel-Sperren (Spiel-Protokoll, gaelte
 * auch auf Xbox) sind ausgenommen. */
static int susp_nlog;

/* Fremdmodul-Sperren (dort definiert, Owner-Tid oder 0 = frei/uninit). */
extern unsigned long sb_ctx_owner_tid(void);
extern unsigned long audio_lock_owner_tid(void);
extern const hlock_t *sb_ctx_hlock(void);
extern const hlock_t *audio_hlock(void);

/* W119 LOADLOCK: Host-Sperren sind hlocks; Owner-Tid wird beim Erwerb
 * atomar mit der Uebernahme gesetzt (vorher CRITICAL_SECTION.OwningThread:
 * Fenster nach "lock btr" bzw. vor der Freigabe -> V_C11gold-Haenger). */
static DWORD susp_cs_owner(const hlock_t *l)
{
    return hlock_owner(l);
}

/* W119 FIX(LOADLOCK): SuspendThread ist asynchron - das Ziel kann nach der
 * Rueckkehr noch kurz laufen (und eine Sperre greifen). GetThreadContext
 * wartet, bis es wirklich steht. Liefert EIP (0 = unbekannt). */
static DWORD susp_sync(HANDLE h)
{
    CONTEXT cx;
    memset(&cx, 0, sizeof cx);
    cx.ContextFlags = CONTEXT_CONTROL;
    if (GetThreadContext(h, &cx)) {
        return cx.Eip;
    }
    return 0;
}

/* DIAG(LOADLOCK): letzter NtSuspendThread (Ziel, EIP nach Sync, Zeit). */
static volatile DWORD g_susp_last_tid, g_susp_last_eip, g_susp_last_ms;
static volatile LONG g_susp_last_ret;

static const char *susp_held_lock(DWORD tid)
{
    if (!tid) {
        return NULL;
    }
    if (susp_cs_owner(&g_ht_cs) == tid) {
        return "g_ht_cs";
    }
    if (susp_cs_owner(&g_csmap_cs) == tid) {
        return "g_csmap_cs";
    }
    if (susp_cs_owner(&g_unimpl_cs) == tid) {
        return "g_unimpl_cs";
    }
    if (sb_ctx_owner_tid() == tid) {
        return "g_ctx_cs";
    }
    if (audio_lock_owner_tid() == tid) {
        return "g_au_cs";
    }
    return NULL;
}

static const char *susp_owner_str(DWORD tid, char *buf, size_t cap)
{
    if (!tid) {
        strcpy_s(buf, cap, "frei");
    } else {
        _snprintf_s(buf, cap, _TRUNCATE, "T%04x", tid & 0xFFFFu);
    }
    return buf;
}

/* DIAG(SUSPSAFE): Beim STALL: welche Harness-Sperre haelt welcher Thread?
 * (Owner-Tid der Host-Sperren + belegte Spiel-CS.) Vom Watchdog gerufen.
 * W119 DIAG(LOADLOCK): zusaetzlich voller hlock-Zustand (belegt ohne Owner =
 * Fenster), SuspendCount + EIP aller Xbox-Threads, letzter NtSuspendThread. */
void krnl_lock_diag(void)
{
    char b_ht[16], b_cm[16], b_ui[16], b_cx[16], b_au[16];
    char l_ht[48], l_cm[48], l_ui[48], l_cx[48], l_au[48];
    unsigned i, n = 0;
    int t;
    HLOG_W("DIAG(LOADLOCK): hlock ht=%s csmap=%s unimpl=%s ctx=%s au=%s (Modell %d, inject=%d, injiziert=%ld, sync=%d)",
           hlock_str(&g_ht_cs, l_ht, sizeof l_ht),
           hlock_str(&g_csmap_cs, l_cm, sizeof l_cm),
           hlock_str(&g_unimpl_cs, l_ui, sizeof l_ui),
           hlock_str(sb_ctx_hlock(), l_cx, sizeof l_cx),
           hlock_str(audio_hlock(), l_au, sizeof l_au),
           g_cfg.diag_lock_model, g_cfg.diag_lock_inject,
           (long)g_hlock_injected, g_cfg.safe_suspend_sync);
    HLOG_W("DIAG(LOADLOCK): letzter NtSuspendThread: Ziel T%04x EIP=0x%08X prev=%ld vor %lu ms",
           g_susp_last_tid & 0xFFFFu, g_susp_last_eip, (long)g_susp_last_ret,
           g_susp_last_tid ? GetTickCount() - g_susp_last_ms : 0ul);
    for (t = 0; t < g_xb_nthreads; t++) {
        HANDLE h = g_xb_threads[t].h;
        DWORD sc, eip;
        if (!h) {
            continue;
        }
        sc = SuspendThread(h);
        if (sc == (DWORD)-1) {
            continue;
        }
        eip = susp_sync(h);
        ResumeThread(h);
        HLOG_W("DIAG(LOADLOCK): thr%d rt=0x%X T%04x SuspendCount=%lu EIP=0x%08X%s",
               t, g_xb_threads[t].routine, GetThreadId(h) & 0xFFFFu, sc, eip,
               sc ? " (SUSPENDIERT)" : "");
    }
    HLOG_W("DIAG(SUSPSAFE): Host-Sperren: ht=%s csmap=%s unimpl=%s ctx=%s au=%s",
           susp_owner_str(susp_cs_owner(&g_ht_cs), b_ht, sizeof b_ht),
           susp_owner_str(susp_cs_owner(&g_csmap_cs), b_cm, sizeof b_cm),
           susp_owner_str(susp_cs_owner(&g_unimpl_cs), b_ui, sizeof b_ui),
           susp_owner_str((DWORD)sb_ctx_owner_tid(), b_cx, sizeof b_cx),
           susp_owner_str((DWORD)audio_lock_owner_tid(), b_au, sizeof b_au));
    for (i = 0; i < CS_MAP_MAX && n < 8; i++) {
        if (g_csmap[i].xaddr && g_csmap[i].f.owner) {
            HLOG_W("DIAG(SUSPSAFE): Spiel-CS 0x%X haelt T%04x", g_csmap[i].xaddr,
                   g_csmap[i].f.owner & 0xFFFFu);
            n++;
        }
    }
}

/* W145 DIAG(LOADLOCK): Halter aller Host-Sperren, die KEIN Xbox-Thread sind
 * (VR-Thread, Present-Takt, UI ...): Name + Tid fuer den Halter-Stack des
 * Watchdogs (emu_main.c). Rueckgabe = Anzahl (doppelte Tids nur einmal). */
int krnl_lock_foreign_owners(unsigned long *tids, const char **names, int cap)
{
    const hlock_t *locks[5];
    const char *ln[5] = { "ht", "csmap", "unimpl", "ctx", "au" };
    int i, j, t, n = 0;
    locks[0] = &g_ht_cs; locks[1] = &g_csmap_cs; locks[2] = &g_unimpl_cs;
    locks[3] = sb_ctx_hlock(); locks[4] = audio_hlock();
    for (i = 0; i < 5 && n < cap; i++) {
        DWORD o = hlock_owner(locks[i]);
        int dup = 0, xb = 0;
        if (!o) { continue; }
        for (j = 0; j < n; j++) { if (tids[j] == o) { dup = 1; } }
        for (t = 0; t < g_xb_nthreads; t++) {
            if (g_xb_threads[t].h && GetThreadId(g_xb_threads[t].h) == o) { xb = 1; }
        }
        if (dup || xb) { continue; }
        tids[n] = o;
        names[n] = ln[i];
        n++;
    }
    return n;
}

/* DIAG(LOADLOCK): Konkurrenz je Host-Sperre (Laufende, krnl_dump_stats). */
void krnl_lock_stats(void)
{
    const hlock_t *L[5];
    static const char *nm[5] = {"ht", "csmap", "unimpl", "ctx", "au"};
    int i;
    L[0] = &g_ht_cs;
    L[1] = &g_csmap_cs;
    L[2] = &g_unimpl_cs;
    L[3] = sb_ctx_hlock();
    L[4] = audio_hlock();
    for (i = 0; i < 5; i++) {
        HLOG_I("KRNL LOADLOCK hlock %s: Konkurrenz=%ld Waits=%ld (spin=%d)",
               nm[i], (long)L[i]->n_cont, (long)L[i]->n_wait,
               g_cfg.hlock_spin);
    }
}

u32 __stdcall hle_NtSuspendThread(u32 xh, u32 pPrev)
{
    ht_entry_t *e = ht_get(xh);
    DWORD c;
    int marked;
    static int nlog;
    if (!e || e->kind != HT_THREAD) {
        HLOG_W("NtSuspendThread(0x%X) -> INVALID_HANDLE", xh);
        return X_STATUS_INVALID_HANDLE;
    }
    /* W119 FIX(LOADLOCK): SuspendThread ist asynchron. sync=2 (SUSPMARK):
     * Ziel VOR dem Suspend markieren, damit es nach dem Check keine Sperre
     * mehr behalten kann (hlock.c); sync=1 bzw. Markenliste voll:
     * GetThreadContext wartet, bis das Ziel steht (teuer). */
    {
        DWORD tt0 = GetThreadId(e->host);
        marked = 0;
        if (g_cfg.safe_suspend && g_cfg.safe_suspend_sync == 2 && tt0 &&
            tt0 != GetCurrentThreadId()) {
            marked = hlock_susp_mark(tt0);
        }
    }
    c = SuspendThread(e->host);
    if (c == (DWORD)-1) {
        DWORD gle = GetLastError();
        if (marked) {
            hlock_susp_unmark(GetThreadId(e->host));
        }
        HLOG_W("NtSuspendThread(0x%X) -> Host-Fehler %lu", xh, gle);
        return X_STATUS_INVALID_HANDLE;
    }
    {
        DWORD eip = 0;
        if (g_cfg.safe_suspend_sync == 1 ||
            (g_cfg.safe_suspend_sync == 2 && !marked)) {
            eip = susp_sync(e->host);
        }
        g_susp_last_tid = GetThreadId(e->host);
        if (GetThreadId(e->host) != GetCurrentThreadId()) {
            g_hlock_inject_tid = g_susp_last_tid; /* DIAG(LOADLOCK) */
        }
        g_susp_last_eip = eip;
        g_susp_last_ret = (LONG)c;
        g_susp_last_ms = GetTickCount();
    }
    /* SUSPSAFE (W10): Ziel nicht suspendiert lassen, solange es eine
     * Host-Sperre haelt (Deadlock V_W4a). Selbst-Suspend ist harmlos
     * (eigener Thread laeuft weiter) und braucht kein Retry. */
    if (g_cfg.safe_suspend) {
        DWORD me = GetCurrentThreadId();
        DWORD tt = GetThreadId(e->host);
        if (tt != 0 && tt != me) {
            int i = 0, dead = 0;
            const char *held = susp_held_lock(tt);
            while (held && i < 200) {
                i++;
                if (ResumeThread(e->host) == (DWORD)-1) {
                    dead = 1; /* Ziel tot o.ae.: Stand lassen */
                    break;
                }
                susp_n_retry++;
                /* Ziel kurz laufen lassen: SwitchToThread reicht bei
                 * gleicher/hoeherer Prio; der Loader laeuft zeitweise Idle
                 * (thread_priorities) -> dann echtes Sleep. */
                if ((i & 15) == 0) {
                    Sleep(1);
                } else {
                    SwitchToThread();
                }
                c = SuspendThread(e->host);
                if (c == (DWORD)-1) {
                    HLOG_W("NtSuspendThread(0x%X) -> Host-Fehler %lu (Retry)",
                           xh, GetLastError());
                    return X_STATUS_INVALID_HANDLE;
                }
                if (g_cfg.safe_suspend_sync == 1 ||
                    (g_cfg.safe_suspend_sync == 2 && !marked)) {
                    g_susp_last_eip = susp_sync(e->host); /* W119 */
                }
                held = susp_held_lock(tt);
            }
            if (held) {
                susp_n_giveup++;
                HLOG_W("DIAG(SUSPSAFE): NtSuspendThread(0x%X): Ziel T%04x haelt %s nach 200 Versuchen weiter -> suspendiert gelassen",
                       xh, tt & 0xFFFFu, held);
            } else if (i > 0 && !dead && susp_nlog < 8) {
                susp_nlog++;
                HLOG_I("DIAG(SUSPSAFE): NtSuspendThread(0x%X): Ziel T%04x hielt Host-Sperre, nach %d Retry(s) sauber (#%d)",
                       xh, tt & 0xFFFFu, i, susp_nlog);
            }
        }
    }
    susp_n_suspend++;
    if (nlog < 16) {
        nlog++;
        HLOG_I("NtSuspendThread(0x%X) -> prev=%lu (#%d, von T%04x)", xh, c,
               nlog, GetCurrentThreadId());
    }
    if (pPrev) {
        *(u32 *)(uintptr_t)pPrev = c;
    }
    return X_STATUS_SUCCESS;
}

u32 __stdcall hle_NtResumeThread(u32 xh, u32 pPrev)
{
    ht_entry_t *e = ht_get(xh);
    DWORD c;
    static int nlog;
    if (!e || e->kind != HT_THREAD) {
        HLOG_W("NtResumeThread(0x%X) -> INVALID_HANDLE", xh);
        return X_STATUS_INVALID_HANDLE;
    }
    /* W119 SUSPMARK: Marke loeschen (Ziel darf wieder Sperren nehmen). */
    if (g_cfg.safe_suspend_sync == 2) {
        hlock_susp_unmark(GetThreadId(e->host));
    }
    c = ResumeThread(e->host);
    if (c == (DWORD)-1) {
        HLOG_W("NtResumeThread(0x%X) -> Host-Fehler %lu", xh,
               GetLastError());
        return X_STATUS_INVALID_HANDLE;
    }
    if (nlog < 16) {
        nlog++;
        HLOG_I("NtResumeThread(0x%X) -> prev=%lu (#%d, von T%04x)", xh, c,
               nlog, GetCurrentThreadId());
    }
    /* M2c: Yield nach Resume (Storm hungert Worker sonst aus). Hack ->
     * resume_yield_ms (mit fair_cs evtl. unnoetig, A/B pruefen). */
    if (g_cfg.resume_yield_ms > 0) {
        Sleep((DWORD)g_cfg.resume_yield_ms);
    }
    if (pPrev) {
        /* M2c: PreviousSuspendCount (VOR dem Decrement, wie Win+Xbox). */
        *(u32 *)(uintptr_t)pPrev = c;
    }
    return X_STATUS_SUCCESS;
}

/* ----- Threads ----- */
u32 __stdcall hle_PsCreateSystemThreadEx(u32 phThread, u32 extSize, u32 kernStack,
                                                u32 tlsSize, u32 pTid, u32 start,
                                                u32 ctx, u32 suspended, u32 dbg,
                                                u32 sysRoutine)
{
    HANDLE h;
    DWORD tid;
    u32 xh;
    xb_thread_param_t *tp;
    (void)extSize;
    (void)kernStack;
    (void)tlsSize;
    (void)dbg;
    tp = malloc(sizeof *tp);
    if (!tp) {
        return X_STATUS_NO_MEMORY;
    }
    tp->start_routine = start;
    tp->start_context = ctx;
    tp->system_routine = sysRoutine;
    h = krnl_CreateThread(xb_thread_wrapper, tp,
                          suspended ? CREATE_SUSPENDED : 0, &tid);
    if (!h) {
        HLOG_E("PsCreateSystemThreadEx: host CreateThread FEHLER %lu", GetLastError());
        free(tp);
        return X_STATUS_NO_MEMORY;
    }
    /* Xbox = Uniprozessor: alle Xbox-Threads auf Core 0 (verbirgt SMP-Races,
     * die auf echter HW nie auftreten; Host-Helfer bleiben frei). */
    {   /* PARALLEL: Kern pro Instanz (SB_CORE=n), Default Core 0 */
        static DWORD_PTR mask;
        if (!mask) {
            char cb[16];
            DWORD n = GetEnvironmentVariableA("SB_CORE", cb, sizeof cb);
            int core = (n && n < sizeof cb) ? atoi(cb) : 0;
            if (core < 0 || core > 31) core = 0;
            mask = (DWORD_PTR)1 << core;
            HLOG_I("KRNL: Xbox-Threads auf Core %d", core);
        }
        SetThreadAffinityMask(h, mask);
    }
    xh = ht_alloc(HT_THREAD, h);
    if (g_xb_nthreads < XB_TR_MAX) {
        /* Eigenes Duplikat fuer Sampler (Spiel schliesst Original via
         * NtClose, z.B. Entry nach CreateThread)! */
        HANDLE dup = NULL;
        DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &dup,
                        0, FALSE, DUPLICATE_SAME_ACCESS);
        g_xb_threads[g_xb_nthreads].h = dup ? dup : h;
        g_xb_threads[g_xb_nthreads].routine = start;
        g_xb_nthreads++;
    }
    *(u32 *)(uintptr_t)phThread = xh;
    watch_dr_on_thread(h, tid); /* W41 WATCHDR: DR auf neuem Xbox-Thread (nur bei watch=...) */
    if (pTid) {
        *(u32 *)(uintptr_t)pTid = tid;
    }
    HLOG_I("PsCreateSystemThreadEx(routine=0x%X ctx=0x%X) -> xh=0x%X tid=%lu", start,
           ctx, xh, tid);
    return X_STATUS_SUCCESS;
}

u32 __stdcall hle_PsTerminateSystemThread(u32 status)
{
    HLOG_I("PsTerminateSystemThread(%u)", status);
    ExitThread(status);
    return 0; /* unreachable */
}

/* XAPI SetThreadPriority (0x190C1B): ObReferenceObjectByHandle schreibt
 * im HLE kein Objekt -> a0 ist weiter das Xbox-Thread-Handle. Xbox mappt
 * 15/-15 auf 16/-16 (LOW_REALTIME/Idle). Spiel setzt den Loader (0x311588)
 * zeitweise auf -15: auf der 1-Kern-Xbox laeuft er dann nur, wenn Main
 * wartet. thread_priorities=1 bildet das nach (Xbox-Threads sind ohnehin
 * auf Core 0 gepinnt, s. PsCreateSystemThreadEx). */
u32 __stdcall hle_KeSetBasePriorityThread(u32 a0, u32 prio)
{
    int inc = (int)prio;
    int wp;
    ht_entry_t *e;
    static int nlog;
    if (!g_cfg.thread_priorities) {
        HLOG_T("KeSetBasePriorityThread(prio=%d) -> ignoriert", inc);
        return 0;
    }
    e = ht_get(a0);
    if (!e || e->kind != HT_THREAD) {
        HLOG_T("KeSetBasePriorityThread(0x%X, %d) -> kein Thread", a0, inc);
        return 0;
    }
    if (inc >= 16) {
        wp = THREAD_PRIORITY_TIME_CRITICAL;
    } else if (inc <= -16) {
        wp = THREAD_PRIORITY_IDLE;
    } else {
        wp = inc > 2 ? 2 : (inc < -2 ? -2 : inc);
    }
    SetThreadPriority(e->host, wp);
    if (nlog < 32) {
        nlog++;
        HLOG_I("KeSetBasePriorityThread(0x%X, %d) -> Host-Prio %d (#%d)",
               a0, inc, wp, nlog);
    }
    return 0;
}

u32 __stdcall hle_KeDelayExecutionThread(u32 mode, u32 alertable, u32 pInterval)
{
    DWORD ms;
    (void)mode;
    ms = xb_timeout_ms((const int64_t *)(uintptr_t)pInterval);
    /* A5: 30-s-Deckel entfernt (W3_kdA/B gleich; INFINITE in 20/20 Laeufen
     * bis Missionsstart nie beobachtet). Xbox-Semantik: echt warten. */
    /* BIKDELAYFIX-AUS: verzoerrt Timing (Load-Loop). */
    SleepEx(ms, alertable ? TRUE : FALSE);
    return X_STATUS_SUCCESS;
}

/* ----- Zeit ----- */
u32 __stdcall hle_KeQuerySystemTime(u32 pTime)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ((u32 *)(uintptr_t)pTime)[0] = ft.dwLowDateTime;
    ((u32 *)(uintptr_t)pTime)[1] = ft.dwHighDateTime;
    return 0;
}

extern uint64_t g_kit_off;     /* kernel_hle.c: W188 Pausen-Versatz (QPC-Einheiten, 0 = bisher) */
extern volatile LONG g_kit_hold;
extern LARGE_INTEGER g_kit_q0;
unsigned __int64 __stdcall hle_KeQueryInterruptTime(void)
{
    static uint64_t freq;
    uint64_t now;
    if (!freq) {
        QueryPerformanceFrequency((LARGE_INTEGER *)&freq);
    }
    if (g_kit_hold) { /* W188: Pause - Zeit steht beim Pausenbeginn */
        now = (uint64_t)g_kit_q0.QuadPart;
    } else {
        QueryPerformanceCounter((LARGE_INTEGER *)&now);
    }
    return (now - g_kit_off) * 10000000ull / freq;
}

/* M2c: KEVENT-Tabelle (Addr -> Host-Event). */
#define KEV_MAX 256
static struct { u32 ev; HANDLE h; } g_kev[KEV_MAX];
static HANDLE kev_get(u32 ev, int create)
{
    unsigned i;
    for (i = 0; i < KEV_MAX; i++) {
        if (g_kev[i].ev == ev && g_kev[i].h) { return g_kev[i].h; }
    }
    if (!create || !ev) { return NULL; }
    /* Validieren: KEVENT-Header (Type 0/1, Size 4/5). */
    {
        unsigned char ty = 0xFF, sz = 0;
        __try { ty = *(unsigned char *)(uintptr_t)ev;
                sz = *(unsigned char *)(uintptr_t)(ev + 2); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
        if (ty > 1 || (sz != 4 && sz != 5)) { return NULL; }
        for (i = 0; i < KEV_MAX; i++) {
            if (!g_kev[i].ev) {
                HANDLE h = CreateEventA(NULL, ty == 0 ? TRUE : FALSE,
                                        FALSE, NULL);
            if (!h) { return NULL; }
            g_kev[i].ev = ev; g_kev[i].h = h;
            return h;
        }
        }
    }
    return NULL;
}
u32 __stdcall hle_KeSetEvent(u32 ev, u32 inc, u32 wait)
{
    static u32 calls;
    HANDLE h;
    (void)inc;
    (void)wait;
    if (hlog_throttle(&calls)) {
        HLOG_T("KeSetEvent(0x%X)", ev);
    }
    h = kev_get(ev, 1);
    if (h) { SetEvent(h); }
    __try { *(LONG *)(uintptr_t)(ev + 4) = 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}

u32 __stdcall hle_KeWaitForSingleObject(u32 obj, u32 reason, u32 mode, u32 alert,
                                               u32 timeout)
{
    static u32 calls;
    (void)reason;
    (void)mode;
    (void)alert;
    (void)timeout;
    {
        HANDLE h = kev_get(obj, 1);
        if (h) {
            DWORD ms = xb_timeout_ms((const int64_t *)(uintptr_t)timeout);
            DWORD wr;
            if (hlog_throttle(&calls)) {
                HLOG_T("KeWait(0x%X, %ums)", obj, ms);
            }
            wr = WaitForSingleObject(h, ms);
            return wait_result(wr);
        }
    }
    if (hlog_throttle(&calls)) {
        HLOG_T("KeWaitForSingleObject(0x%X) -> SUCCESS (kein Event)", obj);
    }
    return X_STATUS_SUCCESS;
}
