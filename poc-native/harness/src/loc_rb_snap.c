/* loc_rb_snap.c - W464 NET-CORE: aus loc_rollback.c verschoben (W325/W330/W338/W362), unveraendert.
 * Snapshot/Restore: Messreihen (p50/p99/max), Gast-Speicherbereiche (Abbild, Heaps, Stacks, nie fremder Speicher),
 * Zustandsring (Sichern je Tick, Laden, Schatten-/Ladepruefung), Seiten-Diff und Mikromessung Schreibschutz. */
#include "loc_rb_int.h"

/* ================= Messreihen (p50/p99/max je Fenster und gesamt) ================= */
rb_ser_t *rb_ser_new(const char *name)
{
    rb_ser_t *s = (rb_ser_t *)calloc(1, sizeof *s);
    if (s) s->name = name;
    return s;
}
void rb_put(rb_ser_t *s, u32 v)
{
    if (s && s->n < RB_SER_N) s->v[s->n++] = v;
}
int rb_cmp(const void *a, const void *b)
{
    u32 x = *(const u32 *)a, y = *(const u32 *)b;
    return x < y ? -1 : x > y;
}
void rb_pct(rb_ser_t *s, u32 *p50, u32 *p99, u32 *mx)
{
    static u32 tmp[RB_SER_N];
    u32 n = s ? s->n : 0;
    *p50 = *p99 = *mx = 0;
    if (!n) return;
    memcpy(tmp, s->v, n * sizeof(u32));
    qsort(tmp, n, sizeof(u32), rb_cmp);
    *p50 = tmp[n / 2];
    *p99 = tmp[(n * 99u) / 100u];
    *mx = tmp[n - 1];
}
void rb_logser(rb_ser_t *s, const char *unit)
{
    u32 a, b, c;
    if (!s || !s->n) return;
    rb_pct(s, &a, &b, &c);
    HLOG_I("LOCRB: %s n=%u p50=%u p99=%u max=%u %s", s->name, s->n, a, b, c, unit);
}

/* ================= Gast-Speicher: Bereiche ================= */
int krnl_memreg_get(unsigned i, unsigned *va, unsigned *len, unsigned *kind); /* krnl_mem.c (W325) */
unsigned krnl_memreg_ab(unsigned i); /* krnl_mem.c (W338) */
static unsigned s_stale; /* W338: veraltete Registereintraege beim Bereichsaufbau uebersprungen */
extern unsigned g_exreg_n;
extern xbe_image_t *g_img; /* kernel_hle.c */

rb_reg_t s_reg[RB_RMAX];
int s_nreg;
size_t s_total;        /* Bytes */
unsigned s_reg_gen;    /* g_exreg_n beim Aufbau */
uint8_t *s_snapA, *s_snapB, *s_snapC;
static size_t s_snap_cap;

static int rb_writable(DWORD p)
{
    if (p & (PAGE_GUARD | PAGE_NOACCESS)) return 0;
    p &= 0xFF;
    return p == PAGE_READWRITE || p == PAGE_EXECUTE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_WRITECOPY;
}

static int rb_ivcmp(const void *x, const void *y)
{
    const rb_iv_t *p = (const rb_iv_t *)x, *q = (const rb_iv_t *)y;
    return p->a < q->a ? -1 : p->a > q->a;
}

/* Bereiche: Abbild (beschreibbare Seiten) + alle registrierten Gast-Allokationen (krnl_mem-Register),
 * seitenweise gerundet, verschmolzen, nur zugesagte beschreibbare Seiten. */
static int rb_ab_foreign(u32 a, u32 e, u32 kind, u32 ab); /* W362 (unten) */
static void rb_add_committed(u32 a, u32 b, u32 kind)
{
    while (a < b && s_nreg < RB_RMAX) {
        MEMORY_BASIC_INFORMATION mi;
        u32 e;
        if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi)) break;
        e = (u32)(uintptr_t)mi.BaseAddress + (u32)mi.RegionSize;
        if (e > b || e < a) e = b;
        if (mi.State == MEM_COMMIT && rb_writable(mi.Protect) &&
            !rb_ab_foreign(a, e, kind, (u32)(uintptr_t)mi.AllocationBase)) {
            if (s_nreg && s_reg[s_nreg - 1].va + s_reg[s_nreg - 1].len == a && s_reg[s_nreg - 1].kind == kind &&
                s_reg[s_nreg - 1].prot == mi.Protect && s_reg[s_nreg - 1].ab == (u32)(uintptr_t)mi.AllocationBase) {
                /* W330: gleicher Schutz und gleiche Belegung (VirtualProtect scheitert ueber Belegungsgrenzen) */
                s_reg[s_nreg - 1].len += e - a;
            } else {
                s_reg[s_nreg].va = a;
                s_reg[s_nreg].len = e - a;
                s_reg[s_nreg].kind = kind;
                s_reg[s_nreg].prot = mi.Protect;
                s_reg[s_nreg].ab = (u32)(uintptr_t)mi.AllocationBase;
                s_nreg++;
            }
        }
        a = e;
    }
}

/* Thread-Stacks aus der Sicherung nehmen: Laden darf weder die aktiven Rahmen des Spiel-Threads (der
 * Thunk selbst laeuft dort) noch Stacks laufender Gast-Threads ueberschreiben. Stack = die Belegung,
 * in der ESP liegt (bei Windows-Threads die ganze Stack-Reservierung), hoechstens 4 MB um ESP. */
static struct { u32 a, b; } s_ex[RB_EXMAX];
static int s_nex;
static int s_nex_fix = -1; /* W362: Eintraege aus rb_collect_stacks (ESP-basiert, einmal); Host-Stacks kommen je Aufbau dazu */
static void rb_ex_add(u32 esp, const char *who, int idx)
{
    MEMORY_BASIC_INFORMATION mi;
    u32 ab, a, e;
    if (s_nex >= RB_EXMAX || !VirtualQuery((void *)(uintptr_t)esp, &mi, sizeof mi)) return;
    ab = (u32)(uintptr_t)mi.AllocationBase;
    a = ab;
    e = ab;
    for (;;) { /* Ende der Belegung */
        MEMORY_BASIC_INFORMATION m2;
        if (!VirtualQuery((void *)(uintptr_t)e, &m2, sizeof m2) || (u32)(uintptr_t)m2.AllocationBase != ab) break;
        e = (u32)(uintptr_t)m2.BaseAddress + (u32)m2.RegionSize;
        if (e - a > 0x10000000u) break;
    }
    if (e - a > 0x400000u) { /* grosse Gast-Belegung: nur 1 MB unter und 1 MB ueber ESP */
        u32 lo = (esp & ~(RB_PG - 1)) - 0x100000u, hi = (esp & ~(RB_PG - 1)) + 0x100000u;
        if (lo > a) a = lo;
        if (hi < e) e = hi;
    }
    s_ex[s_nex].a = a;
    s_ex[s_nex].b = e;
    s_nex++;
    HLOG_I("LOCRB: Stack ausgenommen: %s %d ESP=0x%X Belegung 0x%X-0x%X (%u KB)", who, idx, esp, a, e, (e - a) / 1024u);
}
void rb_collect_stacks(void)
{
    int i, n = krnl_xb_thread_count();
    s_nex = 0;
    rb_ex_add((u32)(uintptr_t)_AddressOfReturnAddress(), "Spiel-Thread", -1);
    for (i = 0; i < n; i++) {
        void *h = NULL;
        unsigned r = 0;
        CONTEXT c;
        if (!krnl_xb_thread_get(i, &h, &r) || GetThreadId((HANDLE)h) == s_game_tid) continue;
        if (SuspendThread((HANDLE)h) == (DWORD)-1) continue;
        c.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext((HANDLE)h, &c)) rb_ex_add(c.Esp, "Gast-Thread", i);
        ResumeThread((HANDLE)h);
    }
    s_nex_fix = s_nex;
}

/* ================= W362 LOC-RING-INIT-EXIT: nie fremden Speicher schuetzen =================
 * Befund (W358/W361, 5 Laeufe; W362 N1-N4): Partner endet still direkt beim ersten Ring-Aufbau (Exit 0xC0000005,
 * ohne Dump/WER). Das Spiel gibt beim Missionsstart Belegungen frei, Windows legt die Stacks des neuen Gast-Render-
 * Threads (32 Bit + WoW64-64-Bit) an dieselben Basen; Teil-Zusagen blieben im Register "lebend" (Wurzel: krnl_mem.c
 * exreg_mark_released) und der Ring schuetzte den 64-Bit-Stack. Schreibt ein Thread auf eine geschuetzte Seite seines
 * eigenen Stacks, kann Windows den Ausnahmerahmen nicht zustellen und beendet den Prozess ohne Handler (kein VEH,
 * kein Filter, kein WER; loc_rb_selftest W362). Die ESP-Ausnahmen oben decken nur Gast-Threads ab, die sich anhalten
 * lassen (2 von 15), nur deren 32-Bit-Stack und keine Host-Threads.
 * Fix 1 (stkx): je Aufbau ALLE Threads des Prozesses aufzaehlen (rbr_thread_stacks), Stack-Reservierung aus dem TEB (32 Bit
 *   und unter WoW64 auch 64 Bit, wenn unter 4 GB) komplett ausnehmen.
 * Fix 2 (abchk): nur Seiten schuetzen/sichern, deren Belegung (AllocationBase) zu einem lebenden Registereintrag
 *   gehoert. Ein Eintrag, dessen Adressraum teils freigegeben und neu (fremd) belegt wurde, deckte sonst fremde
 *   Belegungen mit ab (W338-Pruefung sah nur die Startadresse).
 * stkx=0 / abchk=0 (loc_rb_opts, DIAG): nur melden, nicht ausnehmen (Nachstellung). */
int s_stkx = 1, s_abchk = 1;
static rbr_stack_t s_hs[RB_HSMAX];
static int s_nhs;
static unsigned s_hs_thr, s_hs_noteb;
static u32 s_hs_in, s_ab_skip, s_ab_skip_kb, s_w362_logged;
static u32 *s_abs;
static unsigned s_nabs, s_abs_cap;
static void rb_host_stacks(void) { s_nhs = rbr_thread_stacks(s_hs, RB_HSMAX, &s_hs_thr, &s_hs_noteb); }
int rb_u32cmp(const void *x, const void *y)
{
    u32 a = *(const u32 *)x, b = *(const u32 *)y;
    return a < b ? -1 : a > b;
}
static void rb_abs_add(u32 ab)
{
    if (s_nabs >= s_abs_cap) {
        unsigned nc = s_abs_cap ? s_abs_cap * 2u : 1024u;
        u32 *p = (u32 *)realloc(s_abs, nc * sizeof(u32));
        if (!p) return;
        s_abs = p;
        s_abs_cap = nc;
    }
    s_abs[s_nabs++] = ab;
}
static int rb_abs_has(u32 ab)
{
    return s_nabs && bsearch(&ab, s_abs, s_nabs, sizeof(u32), rb_u32cmp) != NULL;
}
/* 1 = Abschnitt [a,e) gehoert zu keiner lebenden Gast-Belegung (nur Register-Arten Contig/NtAlloc/Pool) -> nicht
 * schuetzen/sichern; abchk=0: nur melden */
static int rb_ab_foreign(u32 a, u32 e, u32 kind, u32 ab)
{
    int k;
    if (kind != 1 && kind != 2 && kind != 5) return 0;
    if (rb_abs_has(ab)) return 0;
    s_ab_skip++;
    s_ab_skip_kb += (e - a) / 1024u;
    if (s_w362_logged < 48) {
        const char *who = "";
        u32 tid = 0;
        for (k = 0; k < s_nhs; k++)
            if (s_hs[k].a < e && a < s_hs[k].b) { who = s_hs[k].bits == 64 ? " = 64-Bit-Stack" : " = Thread-Stack"; tid = s_hs[k].tid; break; }
        s_w362_logged++;
        HLOG_W("LOCRB: W362 fremde Belegung im Gast-Bereich 0x%X-0x%X (%u KB, Belegung 0x%X, Art %u)%s (TID %lu)%s", a, e,
               (e - a) / 1024u, ab, kind, who, (unsigned long)tid,
               s_abchk ? " - ausgelassen" : " - NUR GEMELDET (abchk=0)");
    }
    return s_abchk;
}
static void rb_add_range(u32 a, u32 b, u32 kind)
{
    int k;
    for (k = 0; k < s_nex && a < b; k++) {
        if (s_ex[k].b <= a || s_ex[k].a >= b) continue;
        if (s_ex[k].a > a) rb_add_range(a, s_ex[k].a, kind);
        a = s_ex[k].b;
    }
    if (a < b) rb_add_committed(a, b, kind);
}

/* ================= W330: Zustandsring ================= */
rb_ser_t *s_r_commit, *s_r_commit_rep, *s_r_total, *s_r_dirty, *s_r_saved, *s_r_faults, *s_r_fault_us,
    *s_r_ringload, *s_r_ldpages, *s_r_ww, *s_r_prot, *s_r_copy, *s_r_setup;
u32 s_r_n, s_r_ovf, s_r_drop, s_r_rebuild, s_r_shadow_bad, s_r_ld_bad, s_r_ld_n, s_r_pd_fail, s_r_abort;
static u32 s_r_faults_last, s_r_foreign_last;
rb_ser_t *s_r_cpu;            /* CPU-Zyklen des Spiel-Threads je Sichern (ohne Verdraengung) */
static long long s_tsc_q0;
static unsigned long long s_tsc0;
double rb_tsc_hz(void)
{
    long long q = rb_now();
    unsigned long long t = __rdtsc();
    if (!s_tsc_q0 || q - s_tsc_q0 < s_qpf / 10) return 0.0;
    return (double)(t - s_tsc0) * (double)s_qpf / (double)(q - s_tsc_q0);
}
static uint64_t s_r_fqpc_last;
static unsigned s_r_gen;
/* Fenster (10 s) fuer Ruhe/Kampf-Vergleich */
rb_win_t s_w_total, s_w_commit, s_w_saved, s_w_dirty, s_w_faults;
u32 s_r_hot_last, s_r_cooled, s_r_foreign_win;
static void rb_win_put(rb_win_t *w, u32 v) { if (w->n < RB_WIN_N) w->v[w->n++] = v; }
void rb_win_pct(rb_win_t *w, u32 *p50, u32 *p99, u32 *mx)
{
    static u32 tmp[RB_WIN_N];
    u32 n = w->n;
    *p50 = *p99 = *mx = 0;
    if (!n) return;
    memcpy(tmp, w->v, n * sizeof(u32));
    qsort(tmp, n, sizeof(u32), rb_cmp);
    *p50 = tmp[n / 2];
    *p99 = tmp[(n * 99u) / 100u];
    *mx = tmp[n - 1];
}

static int rb_is_contig(u32 va, u32 len, u32 *lo, u32 *size)
{
    u32 used;
    if (!loc_mem_range(lo, size, &used)) return 0;
    return va >= *lo && va + len <= *lo + *size;
}

static void rb_ring_setup(void)
{
    long long t = rb_now();
    int i, nww = 0, nwp = 0, nal = 0;
    u32 clo = 0, csz = 0;
    g_loc_rb_prewrite = NULL;
    if (rbr_active()) rbr_stop();
    if (rbr_init((unsigned)s_depth, (unsigned)s_pool) != 0) {
        HLOG_E("LOCRB: Ring-Puffer (%d Seiten) nicht verfuegbar - Ring aus", s_pool);
        s_ring = 0;
        return;
    }
    rbr_set_hot((unsigned)s_hot, (unsigned)s_cool);
    for (i = 0; i < s_nreg; i++) {
        u32 al[2];
        int na = 0, m;
        if (rb_is_contig(s_reg[i].va, s_reg[i].len, &clo, &csz)) { /* LoC-Contig: Spiegel mitschuetzen (loc_mem.c) */
            al[0] = s_reg[i].va | 0x80000000u;
            al[1] = s_reg[i].va | 0xF0000000u;
            na = 2;
            nal++;
        }
        m = rbr_add(s_reg[i].va, s_reg[i].len, s_ww && s_reg[i].kind == 2, al, na, s_reg[i].kind);
        if (m == RBR_WW) nww++;
        else if (m == RBR_WP) nwp++;
        else HLOG_W("LOCRB: Bereich 0x%X (%u KB) nicht angemeldet", s_reg[i].va, s_reg[i].len / 1024u);
    }
    if (!s_r_gen) { /* W362: erster Aufbau - Log vor dem Schutz leeren (stilles Ende eingrenzen) */
        long long th = rb_now(); /* W469 */
        hlog_flush();
        rbh_add(RBH_FLUSH, rb_now() - th);
    }
    if (rbr_start() != 0) {
        HLOG_E("LOCRB: Ring-Start fehlgeschlagen - Ring aus");
        s_ring = 0;
        return;
    }
    rbr_set_owner(s_game_tid);
    g_loc_rb_prewrite = rb_prewrite_cb;
    s_memev_seen = g_loc_rb_memev;
    s_r_gen++;
    {
        rbr_stat_t st;
        rbr_stats(&st);
        s_r_faults_last = st.faults;
        s_r_foreign_last = st.faults_foreign;
        s_r_fqpc_last = st.fault_qpc;
    }
    rb_put(s_r_setup, rb_us(rb_now() - t));
    HLOG_I("LOCRB: RING aktiv (#%u): %u Seiten (%u KB), Write-Watch %d Bereiche/%u Seiten, Schreibschutz %d Bereiche/%u "
           "Seiten (%d mit Spiegeln), Tiefe %d Ticks, Puffer %d Seiten, Aufbau %u us",
           s_r_gen, rbr_pages(), rbr_pages() * 4u, nww, rbr_pages_method(RBR_WW), nwp, rbr_pages_method(RBR_WP), nal,
           s_depth, s_pool, rb_us(rb_now() - t));
}

/* W338 DIAG (loc_rb_opts avlog=1): Zugriffsverletzungen aller Threads nach einem Ring-Neuaufbau sperrfrei in eine Datei
 * (Thread, EIP, Adresse, Art) - fuer Abbrueche ohne Bericht. Keine Inhalte. */
int s_avlog;
int s_wpbench; /* W469 */
HANDLE s_avlog_h = INVALID_HANDLE_VALUE;
volatile LONG s_avlog_n;
volatile LONG s_avlog_arm;
LONG CALLBACK rb_avlog_veh(EXCEPTION_POINTERS *ep)
{
    char b[200];
    DWORD w;
    int n;
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    if (!s_avlog_arm || s_avlog_h == INVALID_HANDLE_VALUE) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&s_avlog_n) > 400) return EXCEPTION_CONTINUE_SEARCH;
    n = wsprintfA(b, "t=%lu tid=%lu code=0x%08lX eip=0x%08lX art=%lu addr=0x%08lX esp=0x%08lX\r\n", GetTickCount(),
                  GetCurrentThreadId(), er->ExceptionCode, (DWORD)ep->ContextRecord->Eip,
                  er->NumberParameters > 0 ? (DWORD)er->ExceptionInformation[0] : 0xFFFFFFFFu,
                  er->NumberParameters > 1 ? (DWORD)er->ExceptionInformation[1] : 0u, (DWORD)ep->ContextRecord->Esp);
    WriteFile(s_avlog_h, b, (DWORD)n, &w, NULL);
    return EXCEPTION_CONTINUE_SEARCH;
}
static u32 s_r_commit_av;       /* W338: Zugriffsfehler beim Sichern (Bereich weg) */
int s_r_force_rebuild;   /* W338: Ring beim naechsten Pruefen neu aufbauen */
/* Tick-Ende: inkrementell sichern (Spiel-Thread am Ruhepunkt, Render-Thread frei) */
void rb_ring_commit(int replay)
{
    rbr_commit_t c;
    rbr_stat_t st;
    u32 df, dfor, fus;
    ULONG64 cy0 = 0, cy1 = 0;
    if (!rbr_active()) return;
    if (!s_tsc_q0) { s_tsc_q0 = rb_now(); s_tsc0 = __rdtsc(); }
    QueryThreadCycleTime(GetCurrentThread(), &cy0);
    __try { /* W338: Seite eines angemeldeten Bereichs nicht mehr lesbar (z. B. Heap gab sie zurueck) -> neu aufbauen */
        rbr_commit(&c);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        s_r_commit_av++;
        s_r_force_rebuild = 1;
        if (s_r_commit_av <= 4) HLOG_W("LOCRB: RING Sichern: Bereich nicht lesbar (Tick %u) - Neuaufbau", rd32(RB_TICK));
        return;
    }
    QueryThreadCycleTime(GetCurrentThread(), &cy1);
    if (!replay) rb_put(s_r_cpu, (u32)((cy1 - cy0) / 100u)); /* in 100 Zyklen */
    rbr_stats(&st);
    df = st.faults - s_r_faults_last;
    dfor = st.faults_foreign - s_r_foreign_last;
    fus = (u32)((st.fault_qpc - s_r_fqpc_last) * 1000000ull / (uint64_t)s_qpf);
    s_r_faults_last = st.faults;
    s_r_foreign_last = st.faults_foreign;
    s_r_fqpc_last = st.fault_qpc;
    s_r_n++;
    rb_put(replay ? s_r_commit_rep : s_r_commit, c.us_total);
    if (!replay) {
        rb_put(s_r_total, c.us_total + fus);
        rb_put(s_r_dirty, c.dirty);
        rb_put(s_r_saved, c.saved);
        rb_put(s_r_faults, df);
        rb_put(s_r_fault_us, fus);
        rb_put(s_r_ww, c.us_ww);
        rb_put(s_r_prot, c.us_prot);
        rb_put(s_r_copy, c.us_copy);
        rb_win_put(&s_w_total, c.us_total + fus);
        rb_win_put(&s_w_commit, c.us_total);
        rb_win_put(&s_w_saved, c.saved);
        rb_win_put(&s_w_dirty, c.dirty);
        rb_win_put(&s_w_faults, df);
    }
    (void)dfor;
    s_r_ovf += c.overflow;
    s_r_drop += c.dropped;
    s_r_hot_last = c.hot_total;
    s_r_cooled += c.cooled;
    if (s_shadowchk) { /* DIAG: Schatten = Live (prueft die Verfolgung; ~5 ms) */
        u32 a[4] = {0};
        u32 nd = rbr_shadow_diff(a, 4);
        if (nd) {
            s_r_shadow_bad++;
            if (s_r_shadow_bad <= 10)
                HLOG_W("LOCRB: RING Schatten != Live nach dem Sichern: %u Seiten (0x%X 0x%X 0x%X 0x%X) Tick %u", nd, a[0], a[1],
                       a[2], a[3], rd32(RB_TICK));
        }
    }
}

/* W372: VirtualQuery mit Zwischenspeicher je Bereich. Ein Ergebnis gilt fuer alle Seiten in [BaseAddress,
 * BaseAddress + RegionSize) (gleiche Attribute, gleiche AllocationBase). Ohne Speicher lief je Registereintrag eine
 * Abfrage, die im grossen einheitlichen Contig-Bereich bis zu dessen Ende laeuft (RegionSize): ~830 Eintraege kosteten
 * 115-127 ms je Neuaufbau (Gast-Laptop beim Freund ~470 ms = langer Tick bzw. "Neurechnen max 549 ms"). */
static MEMORY_BASIC_INFORMATION s_vqc[RB_VQC];
static unsigned s_vqc_n, s_vqc_next, s_vq_calls, s_vq_hits;
static SIZE_T rb_vq(unsigned va, MEMORY_BASIC_INFORMATION *mi)
{
    unsigned k;
    SIZE_T r;
    for (k = 0; k < s_vqc_n; k++) {
        uintptr_t b = (uintptr_t)s_vqc[k].BaseAddress;
        if ((uintptr_t)va >= b && (uintptr_t)va - b < s_vqc[k].RegionSize) {
            *mi = s_vqc[k];
            s_vq_hits++;
            return sizeof *mi;
        }
    }
    s_vq_calls++;
    r = VirtualQuery((void *)(uintptr_t)va, mi, sizeof *mi);
    if (r && mi->RegionSize) {
        s_vqc[s_vqc_next] = *mi;
        s_vqc_next = (s_vqc_next + 1u) % RB_VQC;
        if (s_vqc_n < RB_VQC) s_vqc_n++;
    }
    return r;
}

void rb_build_regions(void)
{
    static rb_iv_t iv[8192 + 64];
    xbe_image_t *img = rb_img();
    unsigned i, n = 0, cnt = g_exreg_n < 8192u ? g_exreg_n : 8192u;
    u32 kb[8] = {0}, st4[4] = {0, 0, 0, 0}, t_reg;
    long long t_b0 = rb_now(), t_b1;
    if (s_ring && rbr_active()) { /* W330: erst den alten Ring stoppen (Schutz zurueck), sonst fehlen geschuetzte Seiten */
        g_loc_rb_prewrite = NULL;
        rbr_stop();
        rbr_stop_times(st4);
    }
    t_b1 = rb_now();
    s_vqc_n = s_vqc_next = s_vq_calls = s_vq_hits = 0; /* W372: Speicher je Neuaufbau neu */
    s_nreg = 0;
    s_reg_gen = g_exreg_n;
    s_nabs = 0; /* W362: lebende Belegungen */
    s_ab_skip = s_ab_skip_kb = 0;
    if (img) {
        iv[n].a = img->base & ~(RB_PG - 1);
        iv[n].b = img->image_end;
        iv[n].kind = 0;
        n++;
    }
    for (i = 0; i < cnt; i++) {
        unsigned va = 0, len = 0, kind = 0;
        if (!krnl_memreg_get(i, &va, &len, &kind) || !va || !len) continue;
        if (kind == 3 || kind == 4) continue; /* freigegeben */
        { /* W338: Eintrag gehoert nicht mehr zur selben Belegung (freigegeben, Adressraum wiederverwendet) -> weglassen */
            unsigned ab = krnl_memreg_ab(i);
            MEMORY_BASIC_INFORMATION mi;
            if (ab && (!rb_vq(va, &mi) || mi.State == MEM_FREE || (u32)(uintptr_t)mi.AllocationBase != ab)) {
                s_stale++;
                continue;
            }
            if (ab > 1u) rb_abs_add(ab); /* W362 */
            else if (rb_vq(va, &mi) && mi.State != MEM_FREE)
                rb_abs_add((u32)(uintptr_t)mi.AllocationBase); /* Basis unbekannt: wie bisher die an der Startadresse */
        }
        iv[n].a = va & ~(RB_PG - 1);
        iv[n].b = (va + len + RB_PG - 1) & ~(RB_PG - 1);
        iv[n].kind = kind;
        n++;
    }
    t_reg = rb_us(rb_now() - t_b1); /* W372 DIAG */
    if (s_ring) { /* W330: nicht registrierter Gast-Speicher: TLS des Spiel-Threads, MmCreateKernelStack */
        unsigned va = 0, len = 0, k;
        if (fs_emu_tls_range(&va, &len)) {
            iv[n].a = va & ~(RB_PG - 1);
            iv[n].b = (va + len + RB_PG - 1) & ~(RB_PG - 1);
            iv[n].kind = 6;
            n++;
            HLOG_I("LOCRB: TLS des Spiel-Threads 0x%X-0x%X aufgenommen", va, va + len);
        }
        for (k = 0; k < 16 && s_kstack; k++) { /* W338: mit R2 Standard aus (kstack=0) */
            if (!loc_kstack_get(k, &va, &len)) continue;
            iv[n].a = va & ~(RB_PG - 1);
            iv[n].b = (va + len + RB_PG - 1) & ~(RB_PG - 1);
            iv[n].kind = 7;
            n++;
            HLOG_I("LOCRB: Kernel-Stack (MmCreateKernelStack) 0x%X-0x%X aufgenommen (laufende Stacks bleiben ausgenommen)", va,
                   va + len);
        }
    }
    qsort(iv, n, sizeof iv[0], rb_ivcmp);
    {   /* verschmelzen (Ueberlappung) */
        unsigned j = 0;
        for (i = 0; i < n; i++) {
            if (j && iv[i].a <= iv[j - 1].b) {
                if (iv[i].b > iv[j - 1].b) iv[j - 1].b = iv[i].b;
            } else {
                iv[j++] = iv[i];
            }
        }
        n = j;
    }
    if (s_nabs > 1) qsort(s_abs, s_nabs, sizeof(u32), rb_u32cmp); /* W362 */
    { /* W362: Stacks aller Threads, die in Gast-Bereichen liegen, ausnehmen (stkx=0: nur melden) */
        static u32 nbuild;
        int k;
        long long t0 = rb_now();
        if (s_nex_fix >= 0) s_nex = s_nex_fix;
        rb_host_stacks();
        s_hs_in = 0;
        for (k = 0; k < s_nhs; k++) {
            unsigned m;
            for (m = 0; m < n; m++) {
                if (iv[m].a < s_hs[k].b && s_hs[k].a < iv[m].b) break;
            }
            if (m == n) continue;
            s_hs_in++;
            if (s_w362_logged < 48) {
                s_w362_logged++;
                HLOG_W("LOCRB: W362 %d-Bit-Stack von TID %lu 0x%X-0x%X (%u KB) liegt im Gast-Bereich 0x%X-0x%X Art %u%s",
                       s_hs[k].bits, (unsigned long)s_hs[k].tid, s_hs[k].a, s_hs[k].b, (s_hs[k].b - s_hs[k].a) / 1024u,
                       iv[m].a, iv[m].b, iv[m].kind, s_stkx ? " - ausgenommen" : " - NUR GEMELDET (stkx=0)");
            }
            if (s_stkx && s_nex < RB_EXMAX) {
                s_ex[s_nex].a = s_hs[k].a;
                s_ex[s_nex].b = s_hs[k].b;
                s_nex++;
            }
        }
        if (nbuild++ < 3 || s_hs_in)
            HLOG_I("LOCRB: W362 Stack-Pruefung: Threads=%u ohne TEB=%u Stacks=%u davon im Gast-Bereich=%u, %u us", s_hs_thr,
                   s_hs_noteb, (unsigned)s_nhs, s_hs_in, rb_us(rb_now() - t0));
    }
    for (i = 0; i < n; i++) rb_add_range(iv[i].a, iv[i].b, iv[i].kind);
    if (s_ab_skip) HLOG_W("LOCRB: W362 fremde Belegungen: %u Abschnitte, %u KB%s", s_ab_skip, s_ab_skip_kb,
                          s_abchk ? " ausgelassen" : " NUR GEMELDET (abchk=0)");
    s_total = 0;
    for (i = 0; i < (unsigned)s_nreg; i++) {
        s_reg[i].off = s_total;
        s_total += s_reg[i].len;
        kb[s_reg[i].kind < 8 ? s_reg[i].kind : 5] += s_reg[i].len / 1024u;
    }
    HLOG_I("LOCRB: Bereiche=%d gesamt=%u KB (Abbild %u KB, Contig/Ex %u KB, NtAlloc %u KB, Pool %u KB, TLS %u KB, "
           "Kernel-Stack %u KB) Register=%u veraltet=%u | Abbau %u us (Schutz %u, Handler-Warten %u/%u Runden, Freigabe %u) "
           "Register-Durchlauf %u us (Abfragen %u, aus Speicher %u), bis hier %u us",
           s_nreg, (unsigned)(s_total / 1024u), kb[0], kb[1], kb[2], kb[5], kb[6], kb[7], g_exreg_n, s_stale,
           rb_us(t_b1 - t_b0), st4[0], st4[1], st4[3], st4[2], t_reg, s_vq_calls, s_vq_hits, rb_us(rb_now() - t_b0));
    {
        static int nlog;
        for (i = 0; i < (unsigned)s_nreg && nlog < 64; i++, nlog++)
            HLOG_I("LOCRB: Bereich %u 0x%X-0x%X %u KB Art %u", i, s_reg[i].va, s_reg[i].va + s_reg[i].len,
                   s_reg[i].len / 1024u, s_reg[i].kind);
    }
    {   /* GetWriteWatch je Bereich (geht nur bei Reservierung mit MEM_WRITE_WATCH) */
        static int probed;
        unsigned ok = 0, bad = 0;
        DWORD gle = 0;
        if (!probed) {
            probed = 1;
            for (i = 0; i < (unsigned)s_nreg; i++) {
                static void *addrs[16];
                ULONG_PTR cnt = 16;
                ULONG gran = 0;
                if (GetWriteWatch(0, (void *)(uintptr_t)s_reg[i].va, s_reg[i].len, addrs, &cnt, &gran) == 0) ok++;
                else { bad++; gle = GetLastError(); }
            }
            HLOG_I("LOCRB: GetWriteWatch auf Gast-Bereichen: verfuegbar=%u nicht verfuegbar=%u (GLE=%lu)", ok, bad, gle);
        }
    }
    if (s_ring) rb_ring_setup();
}

int rb_alloc_snaps(int two)
{
    size_t need = s_total + (s_total >> 4) + (1u << 20);
    if (s_snap_cap >= s_total && s_snapA && (!two || s_snapB) && (!two || !s_twice || s_snapC)) return 1;
    if (s_snapA) VirtualFree(s_snapA, 0, MEM_RELEASE);
    if (s_snapB) VirtualFree(s_snapB, 0, MEM_RELEASE);
    if (s_snapC) VirtualFree(s_snapC, 0, MEM_RELEASE);
    s_snapA = (uint8_t *)VirtualAlloc(NULL, need, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    s_snapB = two ? (uint8_t *)VirtualAlloc(NULL, need, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) : NULL;
    s_snapC = (two && s_twice) ? (uint8_t *)VirtualAlloc(NULL, need, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) : NULL;
    s_snap_cap = need;
    if (!s_snapA || (two && !s_snapB) || (two && s_twice && !s_snapC)) {
        HLOG_E("LOCRB: Puffer %u KB nicht verfuegbar (GLE=%lu) - Spike aus", (unsigned)(need / 1024u), GetLastError());
        return 0;
    }
    return 1;
}

void rb_copy_out(uint8_t *dst)
{
    int i;
    for (i = 0; i < s_nreg; i++) memcpy(dst + s_reg[i].off, (void *)(uintptr_t)s_reg[i].va, s_reg[i].len);
}

/* Seitenweiser Vergleich live <-> snap. copy: 1 = snap -> live (Laden), 2 = live -> snap (Sichern),
 * 0 = nur zaehlen. Liefert Anzahl abweichender Seiten; kinds[] je Bereichsart. Optional Adressen. */
u32 s_diff_addr[RB_DIFF_ADDR];
unsigned s_ndiff_addr;
unsigned rb_diff(uint8_t *snap, int copy, unsigned *kinds, int want_addr)
{
    int i;
    unsigned n = 0;
    s_ndiff_addr = 0;
    for (i = 0; i < s_nreg; i++) {
        uint8_t *live = (uint8_t *)(uintptr_t)s_reg[i].va;
        uint8_t *sp = snap + s_reg[i].off;
        u32 o;
        for (o = 0; o < s_reg[i].len; o += RB_PG) {
            if (memcmp(live + o, sp + o, RB_PG) != 0) {
                n++;
                if (kinds) kinds[s_reg[i].kind < 6 ? s_reg[i].kind : 5]++;
                if (want_addr && s_ndiff_addr < RB_DIFF_ADDR) {
                    u32 b;
                    for (b = 0; b < RB_PG; b += 4) { /* erstes abweichendes Wort */
                        if (*(u32 *)(live + o + b) != *(u32 *)(sp + o + b)) break;
                    }
                    s_diff_addr[s_ndiff_addr++] = s_reg[i].va + o + b;
                }
                if (copy == 1) memcpy(live + o, sp + o, RB_PG);
                else if (copy == 2) memcpy(sp + o, live + o, RB_PG);
            }
        }
    }
    return n;
}

/* Abweichende Bytes (16-B-Bloecke) zwischen live und snap auf einer Seite zaehlen (fuer Kompressions-
 * bzw. Delta-Abschaetzung). */
unsigned rb_diff_blocks(uint8_t *snap)
{
    int i;
    unsigned n = 0;
    for (i = 0; i < s_nreg; i++) {
        const uint8_t *live = (const uint8_t *)(uintptr_t)s_reg[i].va;
        const uint8_t *sp = snap + s_reg[i].off;
        u32 o, b;
        for (o = 0; o < s_reg[i].len; o += RB_PG) {
            if (memcmp(live + o, sp + o, RB_PG) == 0) continue;
            for (b = 0; b < RB_PG; b += 16) {
                if (memcmp(live + o + b, sp + o + b, 16) != 0) n++;
            }
        }
    }
    return n;
}

/* Adress-Histogramm abweichender Seiten nach dem Neurechnen (Ursachensuche) */
struct rb_ah1_s s_ah[RB_AH];
unsigned s_nah;
rb_ah_t s_ah2[RB_AH]; /* Neurechnung 1 <-> 2 */
unsigned s_nah2;
void rb_ah_put(rb_ah_t *t, unsigned *pn, u32 addr)
{
    unsigned i;
    u32 pg = addr & ~(RB_PG - 1);
    for (i = 0; i < *pn; i++) {
        if (t[i].page == pg) {
            t[i].n++;
            return;
        }
    }
    if (*pn < RB_AH) {
        t[*pn].page = pg;
        t[*pn].first = addr;
        t[*pn].n = 1;
        (*pn)++;
    }
}
void rb_ah_add(u32 addr) { rb_ah_put((rb_ah_t *)s_ah, &s_nah, addr); }

/* ================= Mikromessung: Schreibschutz-Verfolgung (Alternative zu GetWriteWatch) =========
 * 1024 eigene Seiten schreibgeschuetzt, erster Schreibzugriff je Seite -> eigener VEH-Handler hebt den
 * Schutz auf und merkt die Seite. Kosten je Seite = Fehlerbehandlung + VirtualProtect. Nur eigener Puffer. */
static uint8_t *s_wp_buf;
static volatile LONG s_wp_hits;
static LONG CALLBACK rb_wp_veh(EXCEPTION_POINTERS *ep)
{
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    uintptr_t a;
    DWORD old;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2 || !s_wp_buf)
        return EXCEPTION_CONTINUE_SEARCH;
    a = (uintptr_t)er->ExceptionInformation[1];
    if (a < (uintptr_t)s_wp_buf || a >= (uintptr_t)s_wp_buf + 1024u * RB_PG) return EXCEPTION_CONTINUE_SEARCH;
    VirtualProtect((void *)(a & ~(uintptr_t)(RB_PG - 1)), RB_PG, PAGE_READWRITE, &old);
    InterlockedIncrement(&s_wp_hits);
    return EXCEPTION_CONTINUE_EXECUTION;
}
void rb_wp_bench(void)
{
    void *h;
    DWORD old;
    long long t0, t1, t2;
    unsigned i, rep;
    u32 best_fault = 0xFFFFFFFFu, best_prot = 0xFFFFFFFFu;
    s_wp_buf = (uint8_t *)VirtualAlloc(NULL, 1024u * RB_PG, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!s_wp_buf) return;
    h = AddVectoredExceptionHandler(1, rb_wp_veh);
    for (rep = 0; rep < 5; rep++) {
        t0 = rb_now();
        VirtualProtect(s_wp_buf, 1024u * RB_PG, PAGE_READONLY, &old); /* Schutz setzen (ein Aufruf) */
        t1 = rb_now();
        for (i = 0; i < 1024u; i++) s_wp_buf[i * RB_PG] = (uint8_t)i; /* je Seite ein Fehler */
        t2 = rb_now();
        if (rb_us(t2 - t1) < best_fault) best_fault = rb_us(t2 - t1);
        if (rb_us(t1 - t0) < best_prot) best_prot = rb_us(t1 - t0);
    }
    if (h) RemoveVectoredExceptionHandler(h);
    HLOG_I("LOCRB: Schreibschutz-Verfolgung (eigener Puffer, 1024 Seiten, bestes von 5): Schutz setzen %u us, "
           "1024 Erstzugriffe %u us (%u ns je Seite), Treffer=%ld",
           best_prot, best_fault, (unsigned)(best_fault * 1000u / 1024u), s_wp_hits);
    VirtualFree(s_wp_buf, 0, MEM_RELEASE);
    s_wp_buf = NULL;
}

