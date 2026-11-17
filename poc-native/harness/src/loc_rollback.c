/* loc_rollback.c - W325 LOC-ROLLBACK-SPIKE: Machbarkeit Rollback-Netcode im LoC-HLE (nur Messung).
 *
 * Schalter (nur LoC-Profil + Netz-HLE, Standard aus):
 *   loc_rb_spike=1  Zustand je Tick erfassen: geaenderte Seiten des Gast-Speichers je Tick (Vergleich
 *                   mit der Kopie des Vortick), Kopier-/Vergleichszeiten. Kein Zurueckladen.
 *   loc_rb_spike=2  Determinismus-Zyklen: Zustand A bei Tick N sichern, k Ticks normal weiterlaufen
 *                   (Netz-/Eingabeaufrufe des Spiel-Threads aufzeichnen), Zustand B sichern, A laden,
 *                   dieselben k Ticks ohne Feld-Warten neu rechnen (Aufrufe aus der Aufzeichnung, Senden
 *                   unterdrueckt, Render-Thread angehalten), Ergebnis mit B vergleichen (Zustandshash je
 *                   Tick + Seitenvergleich), danach B laden und normal weiter. Der Mitspieler merkt davon
 *                   nichts (seine Sicht = Zeitlinie B, belegt ueber LOCSYNC).
 *   loc_rb_opts     "k=1,2,4,8;render=0;warm=300;max=0;census=1;twice=0;perturb=0;freeze=0"
 *                   k = Rollback-Tiefen reihum, render = 1: beim Neurechnen die Render-Vorbereitung des
 *                   Spiel-Threads mitlaufen lassen (Render-Thread bleibt angehalten), warm = Lockstep-Ticks
 *                   nach Missionsbeginn bis zum ersten Zyklus, max = hoechstens so viele Zyklen (0 = immer),
 *                   census = HLE-Aufrufe (Kernel-Thunks, XDK-Hooks) des Spiel-Threads je Phase zaehlen,
 *                   twice = 1: zweite Neurechnung gegen die erste vergleichen, perturb = N: jeder N-te Zyklus
 *                   ist Gegenprobe (SBC-Eingabe beim Neurechnen verfaelscht), freeze = 1: uebrige Gast-Threads
 *                   waehrend Laden/Neurechnen anhalten.
 *
 * Ergebnis W325 (LOC_BOOT §24, BERICHT_M2): bitgleicher Zustandshash nach Laden + Neurechnen fuer k bis 12,
 * Neurechnen ~60 us je Tick. Mit Zwischenbildern (W323) stuerzt der Present-Takt-Thread beim Neurechnen ab
 * -> Spike mit interp_fps=0 messen. Stacks des Spiel- und Render-Threads werden nicht gesichert.
 *
 * Ruhepunkt: Missionsschleife FUN_0002F100, "push 3; call FUN_00033050" @0x2F2F7 (Feld-Warten am Ende
 * jedes Bildes). Nach dem Warten stehen ebx/ebp (Konstanten) und esp der Schleife fest, der Rahmen hat
 * keine Locals; die Aufrufer darueber aendern sich in der Mission nicht. Der Thunk sitzt am CALL
 * (wie loc_pace, beide schliessen sich aus).
 *
 * Logs: nur Zaehler, Zeiten, Adressen und Hashes. Keine Eingabe- oder Paketinhalte (die Aufzeichnung
 * liegt nur im Speicher und wird je Zyklus verworfen). */
#include <windows.h>

#include "loc_rollback.h"
#include "hcfg.h"
#include "hlog.h"
#include "hooks_int.h"
#include "kernel_hle.h"
#include "loc_pad.h"
#include "loc_rb_ring.h" /* W330 LOC-ROLLBACK-R1 */
#include "loc_mem.h"
#include "d3d11_render.h" /* W330: sb_interp_rb_pause */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <intrin.h> /* W330: __rdtsc (CPU-Zeit-Eichung) */

typedef uint32_t u32;

/* ---- LoC-VAs (gelesen, Ghidra/capstone) ---- */
#define RB_CALL_VA 0x2F2F7u   /* push 3; call FUN_00033050 */
#define RB_WAIT_FN 0x33050u
#define RB_ACC_CALL 0x1D391u  /* CALL FUN_000C9400 in FUN_0001D010 (Akkumulator lesen+loeschen) */
#define RB_ACC_FN 0xC9400u
#define RB_ACC 0x6D0C58u
#define RB_TICK 0x3070F4u
#define RB_STALL 0x3069C0u
#define RB_ACTIVE 0x306830u
#define RB_RJOB 0x306EECu     /* Render-Auftrag: 0 frei, 1 zeichnen, 2 Ende */
#define RB_RSKIP 0x30694Cu    /* Bilder ohne Render-Vorbereitung (Render-Thread noch beschaeftigt) */
#define RB_DESYNC 0x306790u
#define RB_RTHREAD 0x2C580u   /* Start des Spiel-Render-Threads */
#define RB_XIN_VA 0x1E4BB2u   /* XInputGetState (LoC) */
#define RB_SELECT 0x17081Cu
#define RB_RECV 0x170845u
#define RB_RECVFROM 0x17085Fu
#define RB_SEND 0x17086Eu
#define RB_SENDTO 0x170888u
#define RB_IOCTL 0x1707D6u
#define RB_WSAERR 0x170CD1u
#define RB_XIN_LEN (4u + (u32)sizeof(sbc_in_t)) /* XINPUT_STATE des SBC (0x1C) */

/* ---- W330 LOC-ROLLBACK-R1: Zustandsring (loc_rb=1) ---- */
void (*volatile g_loc_rb_prewrite)(uintptr_t a, unsigned n);
int g_loc_rb_ww;
volatile long g_loc_rb_memev;
int fs_emu_tls_range(unsigned *va, unsigned *len);           /* fs_emu.c (W330) */
const char *krnl_susp_held_lock(unsigned long tid);          /* krnl_sync.c (W330) */
static u32 s_susp_retry, s_susp_fail;
/* Thread anhalten, aber nie liegen lassen, solange er eine Host-Sperre haelt (Log, Kernel-HLE, D3D-Kontext,
 * Audio, hlocks): sonst blockiert der Spiel-Thread beim Neurechnen (V6: Haenger). Wie SUSPSAFE. 1 = steht. */
static int rb_suspend_safe(HANDLE h)
{
    DWORD tid = GetThreadId(h);
    int i;
    for (i = 0; i < 200; i++) {
        CONTEXT c;
        if (SuspendThread(h) == (DWORD)-1) return 0;
        c.ContextFlags = CONTEXT_CONTROL;
        GetThreadContext(h, &c); /* synchronisiert das Anhalten */
        if (!hlog_lock_held_by(tid) && !krnl_susp_held_lock(tid)) return 1;
        ResumeThread(h);
        s_susp_retry++;
        if ((i & 15) == 15) Sleep(1);
        else SwitchToThread();
    }
    s_susp_fail++;
    return 0;
}
int loc_kstack_get(unsigned i, unsigned *va, unsigned *len); /* krnl_loc.c (W330) */
static int s_ring;            /* loc_rb=1: Ring je Tick */
static int s_ww = 1, s_depth = 16, s_pool = 8192, s_verify, s_pdpause = 1, s_shadowchk, s_hot = 2, s_cool = 64;
static long s_memev_seen;
static void rb_prewrite_cb(uintptr_t a, unsigned n) { rbr_prewrite(a, n); }

static int s_mode;            /* 0 nur Ring (loc_rb), 1 Erfassen, 2 Zyklen */
static u32 s_wait_fn = RB_WAIT_FN;
static volatile LONG s_skipwait;
static DWORD s_game_tid;
static HANDLE s_rthread;
static int s_ready, s_failed;

/* Optionen */
static int s_k[8] = {1, 2, 4, 8}, s_nk = 4, s_render, s_warm = 300, s_max, s_census = 1, s_perturb, s_twice, s_freeze;
static int s_ctl;              /* aktueller Zyklus ist Gegenprobe (Eingabe absichtlich veraendert) */
static unsigned s_ctl_n, s_ctl_det, s_ctl_k[8], s_ctl_k_det[8];

static long long s_qpf = 1;
static long long rb_now(void)
{
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return q.QuadPart;
}
static u32 rb_us(long long d) { return d <= 0 ? 0u : (u32)(d * 1000000 / s_qpf); }
static u32 rd32(u32 va)
{
    u32 v = 0;
    __try { v = *(volatile const u32 *)(uintptr_t)va; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return v;
}

/* ================= Messreihen (p50/p99/max je Fenster und gesamt) ================= */
#define RB_SER_N 65536
typedef struct { const char *name; u32 n; u32 v[RB_SER_N]; } rb_ser_t;
static rb_ser_t *rb_ser_new(const char *name)
{
    rb_ser_t *s = (rb_ser_t *)calloc(1, sizeof *s);
    if (s) s->name = name;
    return s;
}
static void rb_put(rb_ser_t *s, u32 v)
{
    if (s && s->n < RB_SER_N) s->v[s->n++] = v;
}
static int rb_cmp(const void *a, const void *b)
{
    u32 x = *(const u32 *)a, y = *(const u32 *)b;
    return x < y ? -1 : x > y;
}
static void rb_pct(rb_ser_t *s, u32 *p50, u32 *p99, u32 *mx)
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
static void rb_logser(rb_ser_t *s, const char *unit)
{
    u32 a, b, c;
    if (!s || !s->n) return;
    rb_pct(s, &a, &b, &c);
    HLOG_I("LOCRB: %s n=%u p50=%u p99=%u max=%u %s", s->name, s->n, a, b, c, unit);
}

/* ================= Gast-Speicher: Bereiche ================= */
int krnl_memreg_get(unsigned i, unsigned *va, unsigned *len, unsigned *kind); /* krnl_mem.c (W325) */
extern unsigned g_exreg_n;
extern xbe_image_t *g_img; /* kernel_hle.c */
static xbe_image_t *rb_img(void) { return g_img; }

#define RB_PG 4096u
#define RB_RMAX 4096
typedef struct { u32 va, len, kind; size_t off; DWORD prot; u32 ab; } rb_reg_t; /* kind: 0 Abbild, 1 Contig/Ex, 2 NtAlloc, 5 Pool, 6 TLS, 7 Kernel-Stack */
static rb_reg_t s_reg[RB_RMAX];
static int s_nreg;
static size_t s_total;        /* Bytes */
static unsigned s_reg_gen;    /* g_exreg_n beim Aufbau */
static uint8_t *s_snapA, *s_snapB, *s_snapC;
static size_t s_snap_cap;

static int rb_writable(DWORD p)
{
    if (p & (PAGE_GUARD | PAGE_NOACCESS)) return 0;
    p &= 0xFF;
    return p == PAGE_READWRITE || p == PAGE_EXECUTE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_WRITECOPY;
}

typedef struct { u32 a, b, kind; } rb_iv_t;
static int rb_ivcmp(const void *x, const void *y)
{
    const rb_iv_t *p = (const rb_iv_t *)x, *q = (const rb_iv_t *)y;
    return p->a < q->a ? -1 : p->a > q->a;
}

/* Bereiche: Abbild (beschreibbare Seiten) + alle registrierten Gast-Allokationen (krnl_mem-Register),
 * seitenweise gerundet, verschmolzen, nur zugesagte beschreibbare Seiten. */
static void rb_add_committed(u32 a, u32 b, u32 kind)
{
    while (a < b && s_nreg < RB_RMAX) {
        MEMORY_BASIC_INFORMATION mi;
        u32 e;
        if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi)) break;
        e = (u32)(uintptr_t)mi.BaseAddress + (u32)mi.RegionSize;
        if (e > b || e < a) e = b;
        if (mi.State == MEM_COMMIT && rb_writable(mi.Protect)) {
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
#define RB_EXMAX 64
static struct { u32 a, b; } s_ex[RB_EXMAX];
static int s_nex;
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
static void rb_collect_stacks(void)
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
static rb_ser_t *s_r_commit, *s_r_commit_rep, *s_r_total, *s_r_dirty, *s_r_saved, *s_r_faults, *s_r_fault_us,
    *s_r_ringload, *s_r_ldpages, *s_r_ww, *s_r_prot, *s_r_copy, *s_r_setup;
static u32 s_r_n, s_r_ovf, s_r_drop, s_r_rebuild, s_r_shadow_bad, s_r_ld_bad, s_r_ld_n, s_r_pd_fail, s_r_abort;
static u32 s_r_faults_last, s_r_foreign_last;
static rb_ser_t *s_r_cpu;            /* CPU-Zyklen des Spiel-Threads je Sichern (ohne Verdraengung) */
static long long s_tsc_q0;
static unsigned long long s_tsc0;
static double rb_tsc_hz(void)
{
    long long q = rb_now();
    unsigned long long t = __rdtsc();
    if (!s_tsc_q0 || q - s_tsc_q0 < s_qpf / 10) return 0.0;
    return (double)(t - s_tsc0) * (double)s_qpf / (double)(q - s_tsc_q0);
}
static uint64_t s_r_fqpc_last;
static unsigned s_r_gen;
/* Fenster (10 s) fuer Ruhe/Kampf-Vergleich */
#define RB_WIN_N 4096
typedef struct { u32 n; u32 v[RB_WIN_N]; } rb_win_t;
static rb_win_t s_w_total, s_w_commit, s_w_saved, s_w_dirty, s_w_faults;
static u32 s_r_hot_last, s_r_cooled, s_r_foreign_win;
static void rb_win_put(rb_win_t *w, u32 v) { if (w->n < RB_WIN_N) w->v[w->n++] = v; }
static void rb_win_pct(rb_win_t *w, u32 *p50, u32 *p99, u32 *mx)
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

/* Tick-Ende: inkrementell sichern (Spiel-Thread am Ruhepunkt, Render-Thread frei) */
static void rb_ring_commit(int replay)
{
    rbr_commit_t c;
    rbr_stat_t st;
    u32 df, dfor, fus;
    ULONG64 cy0 = 0, cy1 = 0;
    if (!rbr_active()) return;
    if (!s_tsc_q0) { s_tsc_q0 = rb_now(); s_tsc0 = __rdtsc(); }
    QueryThreadCycleTime(GetCurrentThread(), &cy0);
    rbr_commit(&c);
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

static void rb_build_regions(void)
{
    static rb_iv_t iv[8192 + 64];
    xbe_image_t *img = rb_img();
    unsigned i, n = 0, cnt = g_exreg_n < 8192u ? g_exreg_n : 8192u;
    u32 kb[8] = {0};
    if (s_ring && rbr_active()) { /* W330: erst den alten Ring stoppen (Schutz zurueck), sonst fehlen geschuetzte Seiten */
        g_loc_rb_prewrite = NULL;
        rbr_stop();
    }
    s_nreg = 0;
    s_reg_gen = g_exreg_n;
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
        iv[n].a = va & ~(RB_PG - 1);
        iv[n].b = (va + len + RB_PG - 1) & ~(RB_PG - 1);
        iv[n].kind = kind;
        n++;
    }
    if (s_ring) { /* W330: nicht registrierter Gast-Speicher: TLS des Spiel-Threads, MmCreateKernelStack */
        unsigned va = 0, len = 0, k;
        if (fs_emu_tls_range(&va, &len)) {
            iv[n].a = va & ~(RB_PG - 1);
            iv[n].b = (va + len + RB_PG - 1) & ~(RB_PG - 1);
            iv[n].kind = 6;
            n++;
            HLOG_I("LOCRB: TLS des Spiel-Threads 0x%X-0x%X aufgenommen", va, va + len);
        }
        for (k = 0; k < 16; k++) {
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
    for (i = 0; i < n; i++) rb_add_range(iv[i].a, iv[i].b, iv[i].kind);
    s_total = 0;
    for (i = 0; i < (unsigned)s_nreg; i++) {
        s_reg[i].off = s_total;
        s_total += s_reg[i].len;
        kb[s_reg[i].kind < 8 ? s_reg[i].kind : 5] += s_reg[i].len / 1024u;
    }
    HLOG_I("LOCRB: Bereiche=%d gesamt=%u KB (Abbild %u KB, Contig/Ex %u KB, NtAlloc %u KB, Pool %u KB, TLS %u KB, "
           "Kernel-Stack %u KB) Register=%u",
           s_nreg, (unsigned)(s_total / 1024u), kb[0], kb[1], kb[2], kb[5], kb[6], kb[7], g_exreg_n);
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

static int rb_alloc_snaps(int two)
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

static void rb_copy_out(uint8_t *dst)
{
    int i;
    for (i = 0; i < s_nreg; i++) memcpy(dst + s_reg[i].off, (void *)(uintptr_t)s_reg[i].va, s_reg[i].len);
}

/* Seitenweiser Vergleich live <-> snap. copy: 1 = snap -> live (Laden), 2 = live -> snap (Sichern),
 * 0 = nur zaehlen. Liefert Anzahl abweichender Seiten; kinds[] je Bereichsart. Optional Adressen. */
#define RB_DIFF_ADDR 64
static u32 s_diff_addr[RB_DIFF_ADDR];
static unsigned s_ndiff_addr;
static unsigned rb_diff(uint8_t *snap, int copy, unsigned *kinds, int want_addr)
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
static unsigned rb_diff_blocks(uint8_t *snap)
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
#define RB_AH 256
static struct { u32 page, first, n; } s_ah[RB_AH];
static unsigned s_nah;
typedef struct { u32 page, first, n; } rb_ah_t;
static rb_ah_t s_ah2[RB_AH]; /* Neurechnung 1 <-> 2 */
static unsigned s_nah2;
static void rb_ah_put(rb_ah_t *t, unsigned *pn, u32 addr)
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
static void rb_ah_add(u32 addr) { rb_ah_put((rb_ah_t *)s_ah, &s_nah, addr); }

/* ================= Aufzeichnung / Wiedergabe der Spiel-Aufrufe ================= */
enum { EV_SELECT = 1, EV_RECV, EV_RECVFROM, EV_SEND, EV_SENDTO, EV_IOCTL, EV_WSAERR, EV_XIN, EV_ACC,
       EV_XGN, EV_XPLAY, EV_XPREP, EV_XSTOP, EV_XSTAT, EV_XSTOPSRC, EV_XWORK, EV_N };
static const char *const k_evname[] = {"-", "select", "recv", "recvfrom", "send", "sendto", "ioctlsocket",
                                       "WSAGetLastError", "XInputGetState", "Akkumulator",
                                       "XACT GetNotification", "XACT PlayEx", "XACT PrepareEx", "XACT Stop",
                                       "XACT SoundSource_GetStatus", "XACT StopSoundCues", "XACTEngineDoWork"};
typedef struct { uint8_t fn, pad; uint16_t n; u32 ret, ih; } rb_ev_t; /* ih = Hash der Eingangsargumente */
#define RB_REC_CAP (4u << 20)
static uint8_t *s_rec;
static u32 s_rec_len, s_rec_cur;
static u32 s_rec_events;
enum { PH_IDLE = 0, PH_RECORD, PH_REPLAY };
static volatile int s_phase;
static u32 s_nt_rec, s_nt_rep; /* Ticks je Phase (Zensus-Normierung) */
static u32 s_rep_div;          /* Abweichungen der Aufruffolge im aktuellen Neurechnen */
static u32 s_rep_div_fn, s_rep_div_at;
static u32 s_cnt_ev[EV_N][2];  /* je Ereignisart: [0] aufgezeichnet, [1] wiedergegeben (gesamt) */

static u32 fnv(u32 h, const void *p, u32 n)
{
    const uint8_t *b = (const uint8_t *)p;
    u32 i;
    for (i = 0; i < n; i++) {
        h ^= b[i];
        h *= 16777619u;
    }
    return h;
}
static int rb_mine(void) { return s_phase != PH_IDLE && GetCurrentThreadId() == s_game_tid; }

static void rb_rec_put(int fn, u32 ret, u32 ih, const void *data, u32 n)
{
    rb_ev_t e;
    if (!s_rec || s_rec_len + sizeof e + n > RB_REC_CAP) {
        s_rep_div++; /* Aufzeichnung voll: Zyklus ungueltig */
        return;
    }
    e.fn = (uint8_t)fn;
    e.pad = 0;
    e.n = (uint16_t)n;
    e.ret = ret;
    e.ih = ih;
    memcpy(s_rec + s_rec_len, &e, sizeof e);
    if (n) memcpy(s_rec + s_rec_len + sizeof e, data, n);
    s_rec_len += (u32)sizeof e + n;
    s_rec_events++;
    s_cnt_ev[fn][0]++;
}

/* Naechstes Ereignis der Wiedergabe; NULL bei Abweichung (Art oder Eingangsargumente). */
static const rb_ev_t *rb_rep_get(int fn, u32 ih, const uint8_t **data)
{
    const rb_ev_t *e;
    if (s_rec_cur + sizeof(rb_ev_t) > s_rec_len) {
        if (!s_rep_div) { s_rep_div_fn = (u32)fn; s_rep_div_at = s_rec_events; }
        s_rep_div++;
        return NULL;
    }
    e = (const rb_ev_t *)(s_rec + s_rec_cur);
    if (e->fn != fn || e->ih != ih) {
        if (!s_rep_div) { s_rep_div_fn = (u32)fn | ((u32)e->fn << 8) | (e->fn == fn ? 0x10000u : 0u); s_rep_div_at = s_rec_cur; }
        s_rep_div++;
        return NULL;
    }
    s_rec_cur += (u32)sizeof *e + e->n;
    s_cnt_ev[fn][1]++;
    if (data) *data = (const uint8_t *)(e + 1);
    return e;
}

/* Originalziele der verketteten Sprungstellen */
typedef u32(__stdcall *fn2_t)(u32, u32);
typedef u32(__stdcall *fn3_t)(u32, u32, u32);
typedef u32(__stdcall *fn4_t)(u32, u32, u32, u32);
typedef u32(__stdcall *fn5_t)(u32, u32, u32, u32, u32);
typedef u32(__stdcall *fn6_t)(u32, u32, u32, u32, u32, u32);
typedef u32(__stdcall *fn0_t)(void);
static fn5_t o_select;
static fn4_t o_recv, o_send;
static fn6_t o_recvfrom, o_sendto;
static fn3_t o_ioctl;
static fn0_t o_wsaerr;
static fn2_t o_xin;

#define WSAEWOULDBLOCK_X 10035u
static u32 s_fake_err;

static u32 fdset_hash(u32 h, u32 p)
{
    if (!p) return fnv(h, "n", 1);
    __try {
        u32 c = *(const u32 *)(uintptr_t)p;
        if (c > 64) c = 64;
        h = fnv(h, (const void *)(uintptr_t)p, 4 + c * 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return h;
}
static u32 fdset_len(u32 p)
{
    u32 c = p ? *(const u32 *)(uintptr_t)p : 0;
    return 4 + (c > 64 ? 64 : c) * 4;
}

static u32 __stdcall rb_select(u32 nfds, u32 r, u32 w, u32 x, u32 tv)
{
    static uint8_t buf[3 * 260]; /* nur Spiel-Thread */
    u32 ih, n = 0, ret;
    if (r) LOC_RB_PREWRITE(r, 260);
    if (w) LOC_RB_PREWRITE(w, 260);
    if (x) LOC_RB_PREWRITE(x, 260);
    if (!rb_mine()) return o_select(nfds, r, w, x, tv);
    ih = fdset_hash(fdset_hash(fdset_hash(2166136261u, r), w), x);
    if (s_phase == PH_RECORD) {
        ret = o_select(nfds, r, w, x, tv);
        if (r) { u32 l = fdset_len(r); memcpy(buf + n, (void *)(uintptr_t)r, l); n += l; }
        if (w) { u32 l = fdset_len(w); memcpy(buf + n, (void *)(uintptr_t)w, l); n += l; }
        if (x) { u32 l = fdset_len(x); memcpy(buf + n, (void *)(uintptr_t)x, l); n += l; }
        rb_rec_put(EV_SELECT, ret, ih, buf, n);
        return ret;
    } else {
        const uint8_t *d;
        const rb_ev_t *e = rb_rep_get(EV_SELECT, ih, &d);
        if (!e) {
            if (r) *(u32 *)(uintptr_t)r = 0;
            if (w) *(u32 *)(uintptr_t)w = 0;
            if (x) *(u32 *)(uintptr_t)x = 0;
            return 0;
        }
        if (r) { u32 l = 4 + (*(const u32 *)(d + n) > 64 ? 64 : *(const u32 *)(d + n)) * 4; memcpy((void *)(uintptr_t)r, d + n, l); n += l; }
        if (w) { u32 l = 4 + (*(const u32 *)(d + n) > 64 ? 64 : *(const u32 *)(d + n)) * 4; memcpy((void *)(uintptr_t)w, d + n, l); n += l; }
        if (x) { u32 l = 4 + (*(const u32 *)(d + n) > 64 ? 64 : *(const u32 *)(d + n)) * 4; memcpy((void *)(uintptr_t)x, d + n, l); n += l; }
        return e->ret;
    }
}

static u32 __stdcall rb_recv(u32 s, u32 buf, u32 len, u32 flags)
{
    u32 ih, ret;
    LOC_RB_PREWRITE(buf, len); /* W330: Kernel schreibt in den Gast-Puffer */
    if (!rb_mine()) return o_recv(s, buf, len, flags);
    ih = s * 31u + len * 7u + flags;
    if (s_phase == PH_RECORD) {
        ret = o_recv(s, buf, len, flags);
        rb_rec_put(EV_RECV, ret, ih, (const void *)(uintptr_t)buf, ((int)ret > 0 && ret <= len) ? ret : 0);
        return ret;
    } else {
        const uint8_t *d;
        const rb_ev_t *e = rb_rep_get(EV_RECV, ih, &d);
        if (!e) { s_fake_err = WSAEWOULDBLOCK_X; return 0xFFFFFFFFu; }
        if (e->n) memcpy((void *)(uintptr_t)buf, d, e->n);
        return e->ret;
    }
}

static u32 __stdcall rb_recvfrom(u32 s, u32 buf, u32 len, u32 flags, u32 from, u32 pfl)
{
    static uint8_t tmp[0x1100 + 24]; /* nur Spiel-Thread */
    u32 ih, ret;
    LOC_RB_PREWRITE(buf, len); /* W330 */
    if (from) LOC_RB_PREWRITE(from, 16);
    if (pfl) LOC_RB_PREWRITE(pfl, 4);
    if (!rb_mine()) return o_recvfrom(s, buf, len, flags, from, pfl);
    ih = s * 31u + len * 7u + flags;
    if (s_phase == PH_RECORD) {
        u32 n = 0, fl;
        ret = o_recvfrom(s, buf, len, flags, from, pfl);
        fl = pfl ? *(const u32 *)(uintptr_t)pfl : 0;
        if (fl > 16) fl = 16;
        memcpy(tmp, &fl, 4);
        if (from && fl) memcpy(tmp + 4, (const void *)(uintptr_t)from, fl);
        n = 20;
        if ((int)ret > 0 && ret <= len && ret <= 0x1100) { memcpy(tmp + n, (const void *)(uintptr_t)buf, ret); n += ret; }
        rb_rec_put(EV_RECVFROM, ret, ih, tmp, n);
        return ret;
    } else {
        const uint8_t *d;
        u32 fl;
        const rb_ev_t *e = rb_rep_get(EV_RECVFROM, ih, &d);
        if (!e) { s_fake_err = WSAEWOULDBLOCK_X; return 0xFFFFFFFFu; }
        memcpy(&fl, d, 4);
        if (pfl) *(u32 *)(uintptr_t)pfl = fl;
        if (from && fl) memcpy((void *)(uintptr_t)from, d + 4, fl);
        if (e->n > 20) memcpy((void *)(uintptr_t)buf, d + 20, e->n - 20u);
        return e->ret;
    }
}

static u32 __stdcall rb_send(u32 s, u32 buf, u32 len, u32 flags)
{
    u32 ih;
    if (!rb_mine()) return o_send(s, buf, len, flags);
    ih = fnv(s * 31u + len, (const void *)(uintptr_t)buf, len > 0x2000 ? 0x2000 : len); /* Inhalt nur als Hash */
    if (s_phase == PH_RECORD) {
        u32 ret = o_send(s, buf, len, flags);
        rb_rec_put(EV_SEND, ret, ih, NULL, 0);
        return ret;
    } else {
        const rb_ev_t *e = rb_rep_get(EV_SEND, ih, NULL); /* unterdrueckt: nicht senden */
        return e ? e->ret : len;
    }
}

static u32 __stdcall rb_sendto(u32 s, u32 buf, u32 len, u32 flags, u32 to, u32 tolen)
{
    u32 ih;
    if (!rb_mine()) return o_sendto(s, buf, len, flags, to, tolen);
    ih = fnv(s * 31u + len, (const void *)(uintptr_t)buf, len > 0x2000 ? 0x2000 : len);
    if (s_phase == PH_RECORD) {
        u32 ret = o_sendto(s, buf, len, flags, to, tolen);
        rb_rec_put(EV_SENDTO, ret, ih, NULL, 0);
        return ret;
    } else {
        const rb_ev_t *e = rb_rep_get(EV_SENDTO, ih, NULL);
        return e ? e->ret : len;
    }
}

static u32 __stdcall rb_ioctl(u32 s, u32 cmd, u32 argp)
{
    u32 ih;
    if (argp) LOC_RB_PREWRITE(argp, 4); /* W330 */
    if (!rb_mine()) return o_ioctl(s, cmd, argp);
    ih = s * 31u + cmd;
    if (s_phase == PH_RECORD) {
        u32 ret = o_ioctl(s, cmd, argp), v = argp ? *(const u32 *)(uintptr_t)argp : 0;
        rb_rec_put(EV_IOCTL, ret, ih, &v, 4);
        return ret;
    } else {
        const uint8_t *d;
        const rb_ev_t *e = rb_rep_get(EV_IOCTL, ih, &d);
        if (!e) return 0;
        if (argp) memcpy((void *)(uintptr_t)argp, d, 4);
        return e->ret;
    }
}

static u32 __stdcall rb_wsaerr(void)
{
    if (!rb_mine()) return o_wsaerr();
    if (s_phase == PH_RECORD) {
        u32 ret = o_wsaerr();
        rb_rec_put(EV_WSAERR, ret, 0, NULL, 0);
        return ret;
    } else {
        const rb_ev_t *e = rb_rep_get(EV_WSAERR, 0, NULL);
        if (!e) { u32 f = s_fake_err; s_fake_err = 0; return f ? f : WSAEWOULDBLOCK_X; }
        return e->ret;
    }
}

static u32 __stdcall rb_xin(u32 h, u32 pState)
{
    if (pState) LOC_RB_PREWRITE(pState, 32); /* W330 */
    if (!rb_mine() || !pState) return o_xin(h, pState);
    if (s_phase == PH_RECORD) {
        u32 ret = o_xin(h, pState);
        /* Laenge je Geraet: virtuelles Gamepad 4+18 Byte, SBC 4+24 Byte (nur im Speicher) */
        rb_rec_put(EV_XIN, ret, h, (const void *)(uintptr_t)pState, loc_pad_is_handle(h) ? 22u : RB_XIN_LEN);
        return ret;
    } else {
        const uint8_t *d;
        const rb_ev_t *e = rb_rep_get(EV_XIN, h, &d);
        if (!e) return o_xin(h, pState);
        memcpy((void *)(uintptr_t)pState, d, e->n);
        if (s_ctl && e->n >= RB_XIN_LEN && !loc_pad_is_handle(h)) { /* Gegenprobe (nur SBC): Hebel, Gas, Zielen */
            sbc_in_t *si = (sbc_in_t *)(uintptr_t)(pState + 4);
            si->aiming_x ^= 0x80u;
            si->aiming_y ^= 0x80u;
            si->rot_lever = 127;
            si->pedal_r = 255;
            si->pedal_m = 0;
        }
        return e->ret;
    }
}

/* ---- W330 xact=1: XACT-Aufrufe des Spiel-Threads aufzeichnen; beim Neurechnen aus der Aufzeichnung bedienen,
 * nichts starten/stoppen (kein doppelter Ton, gleiche Cue-Handles im Gast-Speicher). Vorlage fuer R3. ---- */
#define RB_XGN_VA 0x1C0CAAu
#define RB_XPLAY_VA 0x1C0DC0u
#define RB_XPREP_VA 0x1C0D8Du
#define RB_XSTOP_VA 0x1C0DF3u
#define RB_XSTAT_VA 0x1C0EE9u
#define RB_XSTOPSRC_VA 0x1C0EE4u
#define RB_XWORK_VA 0x1C08C9u
static int s_xact;
static fn3_t o_xgn, o_xplay, o_xprep;
static fn4_t o_xstop;
static fn2_t o_xstat;
typedef u32(__stdcall *fn1_t)(u32);
static fn1_t o_xstopsrc;
static fn0_t o_xwork;
static u32 rb_rd_u32(u32 p)
{
    u32 v = 0;
    if (p) __try { v = *(const u32 *)(uintptr_t)p; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return v;
}
static u32 __stdcall rb_xgn(u32 e, u32 pdesc, u32 pout)
{
    if (!rb_mine()) return o_xgn(e, pdesc, pout);
    if (s_phase == PH_RECORD) {
        u32 ret = o_xgn(e, pdesc, pout);
        rb_rec_put(EV_XGN, ret, 0, (const void *)(uintptr_t)pout, (ret == 0 && pout) ? 24u : 0u);
        return ret;
    } else {
        const uint8_t *d;
        const rb_ev_t *ev = rb_rep_get(EV_XGN, 0, &d);
        if (!ev) return 0x80004005u; /* keine Meldung */
        if (ev->n && pout) { LOC_RB_PREWRITE(pout, 24); memcpy((void *)(uintptr_t)pout, d, ev->n); }
        return ev->ret;
    }
}
static u32 rb_xcue_call(int fn, fn3_t o, u32 sb, u32 params, u32 pp)
{
    u32 ih = fnv(2166136261u, (const void *)(uintptr_t)params, params ? 12u : 0u) * 31u + (pp != 0);
    if (s_phase == PH_RECORD) {
        u32 ret = o(sb, params, pp), h = rb_rd_u32(pp);
        rb_rec_put(fn, ret, ih, &h, pp ? 4u : 0u);
        return ret;
    } else {
        const uint8_t *d;
        const rb_ev_t *ev = rb_rep_get(fn, ih, &d);
        if (!ev) return 0;
        if (ev->n && pp) { LOC_RB_PREWRITE(pp, 4); memcpy((void *)(uintptr_t)pp, d, 4); }
        return ev->ret;
    }
}
static u32 __stdcall rb_xplay(u32 sb, u32 params, u32 pp)
{
    if (!rb_mine()) return o_xplay(sb, params, pp);
    return rb_xcue_call(EV_XPLAY, o_xplay, sb, params, pp);
}
static u32 __stdcall rb_xprep(u32 sb, u32 params, u32 pp)
{
    if (!rb_mine()) return o_xprep(sb, params, pp);
    return rb_xcue_call(EV_XPREP, o_xprep, sb, params, pp);
}
static u32 __stdcall rb_xstop(u32 sb, u32 cue, u32 flags, u32 pcue)
{
    if (!rb_mine()) return o_xstop(sb, cue, flags, pcue);
    if (s_phase == PH_RECORD) {
        u32 ret = o_xstop(sb, cue, flags, pcue);
        rb_rec_put(EV_XSTOP, ret, cue * 31u + flags, NULL, 0);
        return ret;
    } else {
        const rb_ev_t *ev = rb_rep_get(EV_XSTOP, cue * 31u + flags, NULL);
        return ev ? ev->ret : 0;
    }
}
static u32 __stdcall rb_xstat(u32 sp, u32 pst)
{
    if (!rb_mine()) return o_xstat(sp, pst);
    if (s_phase == PH_RECORD) {
        u32 ret = o_xstat(sp, pst), v = rb_rd_u32(pst);
        rb_rec_put(EV_XSTAT, ret, 0, &v, pst ? 4u : 0u);
        return ret;
    } else {
        const uint8_t *d;
        const rb_ev_t *ev = rb_rep_get(EV_XSTAT, 0, &d);
        if (!ev) return o_xstat(sp, pst);
        if (ev->n && pst) { LOC_RB_PREWRITE(pst, 4); memcpy((void *)(uintptr_t)pst, d, 4); }
        return ev->ret;
    }
}
static u32 __stdcall rb_xstopsrc(u32 sp)
{
    if (!rb_mine()) return o_xstopsrc(sp);
    if (s_phase == PH_RECORD) {
        u32 ret = o_xstopsrc(sp);
        rb_rec_put(EV_XSTOPSRC, ret, 0, NULL, 0);
        return ret;
    } else {
        const rb_ev_t *ev = rb_rep_get(EV_XSTOPSRC, 0, NULL);
        return ev ? ev->ret : 0;
    }
}
static u32 __stdcall rb_xwork(void)
{
    if (!rb_mine()) return o_xwork();
    if (s_phase == PH_RECORD) return o_xwork(); /* nicht aufzeichnen: auch aus dem Feld-Warten (entfaellt beim Neurechnen) */
    s_cnt_ev[EV_XWORK][1]++;                     /* beim Neurechnen zurueckgehalten (kein Ton-Fortschritt) */
    return 0;
}

/* Akkumulator (Zustandssumme je Lockstep-Rahmen) vor dem Lesen+Loeschen abgreifen. Im Neurechnen
 * direkt zur Spielfunktion (loc_sync zaehlt dann nicht mit: seine Rahmen/Hashes bleiben die der
 * Zeitlinie, die der Mitspieler sieht). */
static u32 s_acc_next = RB_ACC_FN; /* bisheriges Ziel (loc_sync-Thunk oder Spielfunktion) */
static u32 s_acc_game = RB_ACC_FN;
static u32 s_acc_tgt;
#define RB_ACC_MAX 64
static u32 s_acc_rec[RB_ACC_MAX], s_acc_rep[RB_ACC_MAX];
static u32 s_nacc_rec, s_nacc_rep;
static void __cdecl rb_acc_note(void)
{
    u32 v = rd32(RB_ACC) ^ (rd32(RB_TICK) * 0x9E3779B1u);
    s_acc_tgt = s_acc_next;
    if (GetCurrentThreadId() != s_game_tid) return;
    if (s_phase == PH_RECORD) {
        if (s_nacc_rec < RB_ACC_MAX) s_acc_rec[s_nacc_rec] = v;
        s_nacc_rec++;
    } else if (s_phase == PH_REPLAY) {
        if (s_nacc_rep < RB_ACC_MAX) s_acc_rep[s_nacc_rep] = v;
        s_nacc_rep++;
        s_acc_tgt = s_acc_game;
    }
}
static __declspec(naked) void rb_acc_thunk(void)
{
    __asm {
        pushad
        pushfd
        call rb_acc_note
        popfd
        popad
        jmp dword ptr [s_acc_tgt]
    }
}

/* rel32 an p+1 schreiben; atomar, wenn die 4 Bytes in einem ausgerichteten Quadwort liegen (andere
 * Threads koennen die Stelle gleichzeitig ausfuehren). */
static void rb_put_rel(uint8_t *p, intptr_t rel)
{
    DWORD old;
    uintptr_t a = (uintptr_t)(p + 1);
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    if ((a & 7u) <= 4u) {
        volatile LONGLONG *q = (volatile LONGLONG *)(a & ~(uintptr_t)7u);
        LONGLONG cur, nw;
        do {
            cur = *q;
            nw = cur;
            memcpy((uint8_t *)&nw + (a & 7u), &rel, 4);
        } while (InterlockedCompareExchange64(q, nw, cur) != cur);
    } else {
        memcpy(p + 1, &rel, 4);
    }
    VirtualProtect(p, 5, old, &old);
}

/* ---- Sprungstellen verketten: E9 rel32 an va -> bisheriges Ziel merken, auf mine umlenken ---- */
static int rb_chain_jmp(u32 va, void *mine, void **orig, const char *name)
{
    uint8_t *p = (uint8_t *)(uintptr_t)va;
    intptr_t rel;
    DWORD old;
    if (p[0] != 0xE9) {
        HLOG_W("LOCRB: %s @0x%X kein JMP (0x%02X) - nicht verkettet", name, va, p[0]);
        return 0;
    }
    *orig = (void *)(uintptr_t)(va + 5 + *(int32_t *)(p + 1));
    rel = (uint8_t *)mine - (p + 5);
    (void)old;
    rb_put_rel(p, rel);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return 1;
}
static int rb_chain_call(u32 va, void *mine, u32 *orig, const char *name)
{
    uint8_t *p = (uint8_t *)(uintptr_t)va;
    intptr_t rel;
    DWORD old;
    if (p[0] != 0xE8) {
        HLOG_W("LOCRB: %s @0x%X kein CALL (0x%02X) - nicht verkettet", name, va, p[0]);
        return 0;
    }
    *orig = va + 5 + *(int32_t *)(p + 1);
    rel = (uint8_t *)mine - (p + 5);
    (void)old;
    rb_put_rel(p, rel);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return 1;
}

/* ================= HLE-Zensus: welche Kernel-/XDK-HLE-Aufrufe macht der Spiel-Thread je Phase ========= */
#define RB_HC_MAX 1024
static uint8_t *s_hc_pool;
static u32 s_hc_site[RB_HC_MAX];   /* Kernel: Thunk-Index | 0x80000000, XDK: Gast-VA */
static u32 s_hc[RB_HC_MAX][3];     /* [Phase] Aufrufe */
static unsigned s_nhc;
static void __cdecl rb_hcount(u32 idx)
{
    if (idx < RB_HC_MAX && s_phase != PH_IDLE && GetCurrentThreadId() == s_game_tid) s_hc[idx][s_phase]++;
}
static void *rb_hc_slot(u32 idx, void *target)
{
    uint8_t *t = s_hc_pool + idx * 32u;
    u32 cell = (u32)(uintptr_t)(t + 28);
    u32 fnp = (u32)(uintptr_t)&rb_hcount;
    t[0] = 0x60;                 /* pushad */
    t[1] = 0x9C;                 /* pushfd */
    t[2] = 0x68; memcpy(t + 3, &idx, 4);        /* push idx */
    t[7] = 0xB8; memcpy(t + 8, &fnp, 4);        /* mov eax, rb_hcount */
    t[12] = 0xFF; t[13] = 0xD0;  /* call eax */
    t[14] = 0x83; t[15] = 0xC4; t[16] = 0x04;   /* add esp,4 */
    t[17] = 0x9D;                /* popfd */
    t[18] = 0x61;                /* popad */
    t[19] = 0xFF; t[20] = 0x25; memcpy(t + 21, &cell, 4); /* jmp [cell] */
    memcpy(t + 28, &target, 4);
    return t;
}

static int rb_is_code(u32 a)
{
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi) || mi.State != MEM_COMMIT) return 0;
    return (mi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

static void rb_census_install(void)
{
    xbe_image_t *img = rb_img();
    HMODULE me = NULL;
    u32 dll_lo = 0, dll_hi = 0, nk = 0, nx = 0;
    unsigned i;
    if (!img) return;
    s_hc_pool = (uint8_t *)VirtualAlloc(NULL, RB_HC_MAX * 32u, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!s_hc_pool) return;
    memset(s_hc_pool, 0xCC, RB_HC_MAX * 32u);
    /* Kernel-Thunks: Funktionsziele (keine Variablen); Ordinal aus der Programmdatei */
    {
        u32 *kt = (u32 *)(uintptr_t)img->kt_addr;
        static u32 raw[512];
        FILE *f = NULL;
        unsigned si;
        memset(raw, 0, sizeof raw);
        if (g_cfg.xbe_path[0] && fopen_s(&f, g_cfg.xbe_path, "rb") == 0 && f) {
            for (si = 0; si < img->n_sections; si++) {
                const section_info_t *s = &img->sections[si];
                if (img->kt_addr >= s->vaddr && img->kt_addr < s->vaddr + s->rawsize) {
                    if (fseek(f, (long)(s->rawaddr + img->kt_addr - s->vaddr), SEEK_SET) == 0) {
                        size_t got = fread(raw, 4, 512, f);
                        (void)got;
                    }
                    break;
                }
            }
            fclose(f);
        }
        DWORD kold;
        VirtualProtect(kt, 512 * 4, PAGE_EXECUTE_READWRITE, &kold);
        for (i = 0; i < 512 && kt[i] && s_nhc < RB_HC_MAX; i++) {
            if (!rb_is_code(kt[i])) continue;
            s_hc_site[s_nhc] = 0x80000000u | (i << 16) | (raw[i] & 0x1FFu);
            InterlockedExchange((volatile LONG *)&kt[i], (LONG)(uintptr_t)rb_hc_slot(s_nhc, (void *)(uintptr_t)kt[i]));
            s_nhc++;
            nk++;
        }
        VirtualProtect(kt, 512 * 4, kold, &kold);
    }
    /* XDK-Hooks: E9-Spruenge im Abbild, die von der Programmdatei abweichen und in sb_emu.dll zeigen */
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (const char *)(void *)rb_census_install, &me);
    if (me) {
        const IMAGE_DOS_HEADER *dh = (const IMAGE_DOS_HEADER *)me;
        const IMAGE_NT_HEADERS *nh = (const IMAGE_NT_HEADERS *)((const uint8_t *)me + dh->e_lfanew);
        dll_lo = (u32)(uintptr_t)me;
        dll_hi = dll_lo + nh->OptionalHeader.SizeOfImage;
    }
    if (dll_lo && g_cfg.xbe_path[0]) {
        FILE *f = NULL;
        if (fopen_s(&f, g_cfg.xbe_path, "rb") == 0 && f) {
            unsigned si;
            for (si = 0; si < img->n_sections; si++) {
                const section_info_t *s = &img->sections[si];
                uint8_t *orig;
                u32 j;
                if (!s->rawsize || !(s->flags & 4u)) continue; /* nur ausfuehrbare Abschnitte */
                orig = (uint8_t *)malloc(s->rawsize);
                if (!orig) continue;
                if (fseek(f, (long)s->rawaddr, SEEK_SET) != 0 || fread(orig, 1, s->rawsize, f) != s->rawsize) {
                    free(orig);
                    continue;
                }
                for (j = 0; j + 5 <= s->rawsize && s_nhc < RB_HC_MAX; j++) {
                    uint8_t *p = (uint8_t *)(uintptr_t)(s->vaddr + j);
                    u32 tgt, va = s->vaddr + j;
                    if (p[0] != 0xE9 || memcmp(p, orig + j, 5) == 0) continue;
                    tgt = va + 5 + *(int32_t *)(p + 1);
                    if (tgt < dll_lo || tgt >= dll_hi) continue;
                    if (va == RB_SELECT || va == RB_RECV || va == RB_RECVFROM || va == RB_SEND || va == RB_SENDTO ||
                        va == RB_IOCTL || va == RB_WSAERR || va == RB_XIN_VA) continue; /* eigene Kette */
                    {
                        void *slot = rb_hc_slot(s_nhc, (void *)(uintptr_t)tgt);
                        rb_put_rel(p, (uint8_t *)slot - (p + 5));
                        s_hc_site[s_nhc] = va;
                        s_nhc++;
                        nx++;
                    }
                    j += 4;
                }
                free(orig);
            }
            fclose(f);
            FlushInstructionCache(GetCurrentProcess(), NULL, 0);
        }
    }
    HLOG_I("LOCRB: HLE-Zensus aktiv: %u Kernel-Thunks, %u XDK-Sprungstellen (sb_emu 0x%X-0x%X)", nk, nx, dll_lo,
           dll_hi);
}

static void rb_census_log(void)
{
    unsigned i, nl = 0;
    HLOG_I("LOCRB: ZENSUS Ticks Vorlauf=%u Neurechnen=%u", s_nt_rec, s_nt_rep);
    for (i = 0; i < s_nhc; i++) {
        if (!s_hc[i][PH_RECORD] && !s_hc[i][PH_REPLAY]) continue;
        if (s_hc_site[i] & 0x80000000u) {
            u32 ord = s_hc_site[i] & 0x1FFu;
            HLOG_I("LOCRB: ZENSUS Kernel %u %s Vorlauf=%u Neurechnen=%u", ord, ord ? krnl_name(ord) : "?",
                   s_hc[i][PH_RECORD], s_hc[i][PH_REPLAY]);
        } else {
            HLOG_I("LOCRB: ZENSUS XDK @0x%X Vorlauf=%u Neurechnen=%u", s_hc_site[i], s_hc[i][PH_RECORD],
                   s_hc[i][PH_REPLAY]);
        }
        if (++nl >= 200) break;
    }
}

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
static void rb_wp_bench(void)
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

/* ================= Threads und Ruhepunkt ================= */
static void rb_threads_log(void)
{
    int i, n = krnl_xb_thread_count();
    for (i = 0; i < n; i++) {
        void *h = NULL;
        unsigned r = 0;
        if (!krnl_xb_thread_get(i, &h, &r)) continue;
        HLOG_I("LOCRB: Gast-Thread %d Start=0x%X tid=%lu%s%s", i, r, GetThreadId((HANDLE)h),
               GetThreadId((HANDLE)h) == s_game_tid ? " (Spiel-Thread, hier Ruhepunkt)" : "",
               r == RB_RTHREAD ? " (Render-Thread)" : "");
        if (r == RB_RTHREAD && !s_rthread) s_rthread = (HANDLE)h;
    }
}

static rb_ser_t *s_q_wait, *s_q_susp, *s_t_saveA, *s_t_saveB, *s_t_loadA, *s_t_loadB, *s_t_tick, *s_t_rep,
    *s_dirty_k1, *s_dirty_blk_k1, *s_dirty_pg, *s_t_cmp, *s_t_full, *s_rec_bytes, *s_t_tickfwd;
static u32 s_q_busy;           /* Render-Thread am Ruhepunkt noch beschaeftigt */
static u32 s_q_timeout;

/* freeze=1: alle uebrigen Gast-Threads (ausser Spiel- und Render-Thread) fuer Laden+Neurechnen anhalten. */
static HANDLE s_frz[64];
static int s_nfrz;
static rb_ser_t *s_q_frz;
static void rb_freeze_others(void)
{
    int i, n = krnl_xb_thread_count();
    long long t = rb_now();
    s_nfrz = 0;
    for (i = 0; i < n && s_nfrz < 64; i++) {
        void *h = NULL;
        unsigned r = 0;
        CONTEXT c;
        if (!krnl_xb_thread_get(i, &h, &r) || r == RB_RTHREAD || GetThreadId((HANDLE)h) == s_game_tid) continue;
        if (!rb_suspend_safe((HANDLE)h)) continue; /* W330: nie mit gehaltener Host-Sperre liegen lassen */
        (void)c;
        s_frz[s_nfrz++] = (HANDLE)h;
    }
    rb_put(s_q_frz, rb_us(rb_now() - t));
}
static void rb_thaw_others(void)
{
    int i;
    for (i = 0; i < s_nfrz; i++) ResumeThread(s_frz[i]);
    s_nfrz = 0;
}

/* Warten, bis der Render-Thread frei ist (Auftrag 0), dann anhalten. Zeit in us. */
static int rb_quiesce(int suspend)
{
    long long t0 = rb_now();
    int spins = 0;
    while (rd32(RB_RJOB) == 1u) {
        if (rb_us(rb_now() - t0) > 100000u) {
            s_q_timeout++;
            return 0;
        }
        spins++;
        SwitchToThread();
    }
    if (spins) s_q_busy++;
    rb_put(s_q_wait, rb_us(rb_now() - t0));
    if (suspend && s_rthread) {
        long long t1 = rb_now();
        if (!rb_suspend_safe(s_rthread)) { /* W330: haelt dauernd eine Sperre -> Zyklus verwerfen */
            s_q_timeout++;
            return 0;
        }
        rb_put(s_q_susp, rb_us(rb_now() - t1));
        if (rd32(RB_RJOB) == 1u) { /* doch noch einen Auftrag gesehen */
            ResumeThread(s_rthread);
            s_q_timeout++;
            return 0;
        }
    }
    return 1;
}

/* ================= Zyklen ================= */
static u32 s_cyc, s_cyc_ok, s_cyc_hash_bad, s_cyc_mem_bad, s_cyc_div, s_cyc_skip, s_cyc_k[8], s_cyc_k_ok[8],
    s_cyc_k_hash_bad[8];
static int s_ki, s_kcur, s_fwd, s_rep;
static long long s_t_last, s_t_rep0;
static unsigned s_lst_frames;  /* Lockstep-Ticks seit Missionsbeginn (eigene Zaehlung) */
static u32 s_tick_a, s_tick_b;
static u32 s_mem_diff_last;
static unsigned s_kinds_last[6];

static int s_pass;                  /* 2 = zweite Neurechnung laeuft (twice=1) */
static int s_p1_hash_ok;
static unsigned s_p1_nd, s_p1_kinds[6], s_p1_naddr;
static u32 s_p1_addr[4], s_p1_acc[RB_ACC_MAX], s_p1_nacc, s_p1_div;
static u32 s_tw_n, s_tw_hash_same, s_tw_mem_same, s_tw_nd_max;
static rb_ser_t *s_tw_nd;

static void rb_cycle_end(void)
{
    long long t;
    unsigned kinds[6] = {0}, nd, i;
    int hash_ok, mem_ok;
    u32 tick_r = rd32(RB_TICK);
    if (s_pass == 2) { /* zweite Neurechnung gegen die erste */
        unsigned nd2 = rb_diff(s_snapC, 0, NULL, 1);
        int same = s_nacc_rep == s_p1_nacc;
        for (i = 0; same && i < s_p1_nacc && i < RB_ACC_MAX; i++) {
            if (s_acc_rep[i] != s_p1_acc[i]) same = 0;
        }
        for (i = 0; i < s_ndiff_addr; i++) rb_ah_put(s_ah2, &s_nah2, s_diff_addr[i]);
        s_tw_n++;
        if (same) s_tw_hash_same++;
        if (!nd2) s_tw_mem_same++;
        if (nd2 > s_tw_nd_max) s_tw_nd_max = nd2;
        rb_put(s_tw_nd, nd2);
        hash_ok = s_p1_hash_ok; /* Ergebnisse der ersten Neurechnung gelten fuer die Statistik */
        nd = s_p1_nd;
        memcpy(kinds, s_p1_kinds, sizeof kinds);
        s_rep_div += s_p1_div;
        s_pass = 0;
    } else {
        /* 1. Zustandshash je Tick (Akkumulator) */
        hash_ok = s_nacc_rep == s_nacc_rec && tick_r == s_tick_b;
        for (i = 0; hash_ok && i < s_nacc_rec && i < RB_ACC_MAX; i++) {
            if (s_acc_rep[i] != s_acc_rec[i]) hash_ok = 0;
        }
        /* 2. Seitenvergleich Ergebnis <-> B (ohne Kopie), Adressen fuer die Ursachensuche */
        t = rb_now();
        nd = rb_diff(s_snapB, 0, kinds, 1);
        rb_put(s_t_cmp, rb_us(rb_now() - t));
        for (i = 0; i < s_ndiff_addr; i++) rb_ah_add(s_diff_addr[i]);
        s_p1_naddr = s_ndiff_addr < 4 ? s_ndiff_addr : 4;
        memset(s_p1_addr, 0, sizeof s_p1_addr);
        memcpy(s_p1_addr, s_diff_addr, s_p1_naddr * sizeof(u32));
        if (s_twice && !s_ctl) { /* Ergebnis sichern, A erneut laden, dieselben k Ticks nochmal */
            s_p1_hash_ok = hash_ok;
            s_p1_nd = nd;
            memcpy(s_p1_kinds, kinds, sizeof kinds);
            memcpy(s_p1_acc, s_acc_rep, sizeof s_p1_acc);
            s_p1_nacc = s_nacc_rep;
            s_p1_div = s_rep_div;
            rb_copy_out(s_snapC);
            if (s_ring) { /* W330: A aus dem Ring (die k Neurechen-Ticks sind gesichert) */
                if (rbr_load((unsigned)s_kcur, NULL, NULL) != 0) (void)rb_diff(s_snapA, 1, NULL, 0);
            } else {
                (void)rb_diff(s_snapA, 1, NULL, 0);
            }
            if (!s_render) *(volatile u32 *)(uintptr_t)RB_RJOB = 1u;
            s_rec_cur = 0;
            s_nacc_rep = 0;
            s_rep_div = 0;
            s_rep = 0;
            s_pass = 2;
            s_t_rep0 = s_t_last = rb_now();
            return; /* Neurechnen laeuft weiter (Feld-Warten bleibt aus) */
        }
    }
    mem_ok = nd == 0;
    s_mem_diff_last = nd;
    memcpy(s_kinds_last, kinds, sizeof kinds);
    /* 3. Zeitlinie B wiederherstellen (seitenweise) */
    t = rb_now();
    (void)rb_diff(s_snapB, 1, NULL, 0);
    rb_put(s_t_loadB, rb_us(rb_now() - t));
    if (s_ring) { /* W330: Schatten auf B nachziehen, Ring leeren (seine letzten k Ticks stammen aus dem Neurechnen) */
        rbr_commit(NULL);
        rbr_reset_ring();
    }
    if (s_freeze) rb_thaw_others();
    if (s_rthread) ResumeThread(s_rthread);
    if (s_pdpause) sb_interp_rb_pause(0); /* W330 */
    InterlockedExchange(&s_skipwait, 0);
    s_phase = PH_IDLE;
    if (s_ctl) { /* Gegenprobe: zaehlt getrennt */
        s_ctl_n++;
        s_ctl_k[s_ki]++;
        if (!hash_ok) { s_ctl_det++; s_ctl_k_det[s_ki]++; } /* erkannt = Zustandshash verschieden */
        if (s_ctl_n <= 8)
            HLOG_I("LOCRB: Gegenprobe %u k=%d: Hash=%s Seiten=%u", s_ctl_n, s_kcur, hash_ok ? "gleich" : "VERSCHIEDEN", nd);
        s_ctl = 0;
        s_ki = (s_ki + 1) % s_nk;
        return;
    }
    s_cyc++;
    s_cyc_k[s_ki]++;
    if (s_rep_div) s_cyc_div++;
    if (!hash_ok) { s_cyc_hash_bad++; s_cyc_k_hash_bad[s_ki]++; }
    if (!mem_ok) s_cyc_mem_bad++;
    if (hash_ok && !s_rep_div) { s_cyc_ok++; s_cyc_k_ok[s_ki]++; }
    if ((!hash_ok || s_rep_div) && (s_cyc_hash_bad + s_cyc_div) <= 20) {
        HLOG_W("LOCRB: Zyklus %u k=%d ABWEICHUNG: Hash=%s (Rahmen %u/%u, Tick %u/%u) Aufrufe=%u (erste Art 0x%X bei %u) "
               "Seiten=%u",
               s_cyc, s_kcur, hash_ok ? "gleich" : "VERSCHIEDEN", s_nacc_rep, s_nacc_rec, tick_r, s_tick_b, s_rep_div,
               s_rep_div_fn, s_rep_div_at, nd);
    }
    if (s_cyc <= 3 || (s_cyc % 500) == 0) {
        HLOG_I("LOCRB: Zyklus %u k=%d Hash=%s Rahmen=%u Aufrufe=%u Ereignisse=%u Seiten-Abweichung=%u "
               "(Abbild %u, Contig %u, NtAlloc %u, Pool %u) Adressen:%s%X %X %X %X",
               s_cyc, s_kcur, hash_ok ? "gleich" : "VERSCHIEDEN", s_nacc_rec, s_rep_div, s_rec_events, nd, kinds[0],
               kinds[1], kinds[2], kinds[5], s_p1_naddr ? " " : " -", s_p1_addr[0], s_p1_addr[1], s_p1_addr[2],
               s_p1_addr[3]);
    }
    s_ki = (s_ki + 1) % s_nk;
}

static void rb_cycle_step(void)
{
    long long t = rb_now();
    if (s_phase == PH_IDLE) {
        if (s_max && s_cyc >= (u32)s_max) return;
        if (rd32(RB_STALL) || rd32(RB_DESYNC)) { s_cyc_skip++; return; }
        if (g_exreg_n != s_reg_gen) { /* neue Allokationen: Bereiche neu */
            HLOG_I("LOCRB: Register geaendert (%u -> %u) bei Tick %u", s_reg_gen, g_exreg_n, rd32(RB_TICK));
            rb_build_regions();
            if (!rb_alloc_snaps(1)) { s_failed = 1; return; }
        }
        if (!rb_quiesce(0)) { s_cyc_skip++; return; }
        s_kcur = s_k[s_ki];
        if (s_ring && s_verify) { /* W330: Pruefkopie = Schatten (Stand des Sicherns); Live-Abweichung = fremde
                                    * Schreibzugriffe zwischen Sichern und jetzt, getrennt gezaehlt */
            u32 a[4] = {0}, nf = rbr_shadow_diff(a, 4);
            rbr_shadow_copy(s_snapA);
            if (nf) {
                s_r_foreign_win++;
                if (s_r_foreign_win <= 12)
                    HLOG_I("LOCRB: RING fremd beschrieben zwischen Sichern und Zyklusbeginn: %u Seiten (0x%X 0x%X 0x%X 0x%X)", nf,
                           a[0], a[1], a[2], a[3]);
            }
        } else if (!s_ring) {
            t = rb_now();
            rb_copy_out(s_snapA);
            rb_put(s_t_saveA, rb_us(rb_now() - t));
        }
        s_ctl = s_perturb > 0 && ((s_cyc + s_ctl_n + 1) % (unsigned)s_perturb) == 0;
        s_fwd = 0;
        s_rec_len = s_rec_cur = s_rec_events = 0;
        s_nacc_rec = s_nacc_rep = 0;
        s_rep_div = 0;
        s_tick_a = rd32(RB_TICK);
        s_phase = PH_RECORD;
        s_t_last = rb_now();
        return;
    }
    if (s_phase == PH_RECORD) {
        s_nt_rec++;
        rb_put(s_t_tickfwd, rb_us(t - s_t_last));
        s_t_last = t;
        if (++s_fwd < s_kcur) return;
        /* B sichern (Ruhepunkt: Render-Thread frei und angehalten) */
        if (!rb_quiesce(1)) { /* Zyklus verwerfen */
            s_phase = PH_IDLE;
            s_cyc_skip++;
            return;
        }
        if (s_freeze) rb_freeze_others();
        if (s_pdpause && !sb_interp_rb_pause(1)) s_r_pd_fail++; /* W330: Present-Takt der Zwischenbilder ruht */
        s_tick_b = rd32(RB_TICK);
        rb_put(s_rec_bytes, s_rec_len);
        t = rb_now();
        rb_copy_out(s_snapB);
        rb_put(s_t_saveB, rb_us(rb_now() - t));
        if (s_kcur == 1 && (!s_ring || s_verify)) { /* geaenderte Seiten je Tick (A -> B) */
            unsigned nd = 0;
            int i;
            for (i = 0; i < s_nreg; i++) {
                u32 o;
                for (o = 0; o < s_reg[i].len; o += RB_PG) {
                    if (memcmp(s_snapA + s_reg[i].off + o, s_snapB + s_reg[i].off + o, RB_PG) != 0) nd++;
                }
            }
            rb_put(s_dirty_k1, nd);
        }
        /* A laden (seitenweise), Neurechnen ohne Feld-Warten */
        t = rb_now();
        if (s_ring) { /* W330: k Ticks aus dem Ring */
            uint32_t pages = 0, us = 0;
            if (rbr_load((unsigned)s_kcur, &pages, &us) != 0) { /* zu wenig Ticks im Ring (Neuaufbau) -> verwerfen */
                if (s_pdpause) sb_interp_rb_pause(0);
                if (s_freeze) rb_thaw_others();
                if (s_rthread) ResumeThread(s_rthread);
                s_phase = PH_IDLE;
                s_cyc_skip++;
                return;
            }
            rb_put(s_r_ringload, us);
            rb_put(s_r_ldpages, pages);
            s_r_ld_n++;
            if (s_verify) { /* Stand nach dem Laden = Vollkopie A? (prueft Verfolgung + Ring) */
                u32 a[8] = {0};
                u32 nd = rbr_full_diff(s_snapA, a, 8);
                if (nd) {
                    s_r_ld_bad++;
                    if (s_r_ld_bad <= 12)
                        HLOG_W("LOCRB: RING Laden k=%d: %u Seiten != Vollkopie A (0x%X 0x%X 0x%X 0x%X 0x%X 0x%X 0x%X 0x%X)", s_kcur,
                               nd, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
                }
            }
        } else {
            unsigned nl = rb_diff(s_snapA, 1, NULL, 0);
            rb_put(s_t_loadA, rb_us(rb_now() - t));
            rb_put(s_dirty_pg, nl);
        }
        if (!s_render) *(volatile u32 *)(uintptr_t)RB_RJOB = 1u; /* Render-Vorbereitung ueberspringen */
        s_rep = 0;
        s_phase = PH_REPLAY;
        InterlockedExchange(&s_skipwait, 1);
        s_t_rep0 = s_t_last = rb_now();
        return;
    }
    /* PH_REPLAY */
    s_nt_rep++;
    rb_put(s_t_tick, rb_us(t - s_t_last));
    s_t_last = t;
    if (s_render) *(volatile u32 *)(uintptr_t)RB_RJOB = 0u; /* Auftrag verwerfen (Render-Thread steht) */
    if (++s_rep < s_kcur) return;
    rb_put(s_t_rep, rb_us(t - s_t_rep0));
    rb_cycle_end();
}

/* ---- Modus 1: geaenderte Seiten je Tick ---- */
static void rb_dirty_step(void)
{
    long long t;
    unsigned kinds[6] = {0}, nd, nb;
    if (rd32(RB_STALL)) return;
    if (g_exreg_n != s_reg_gen) {
        HLOG_I("LOCRB: Register geaendert (%u -> %u) bei Tick %u", s_reg_gen, g_exreg_n, rd32(RB_TICK));
        rb_build_regions();
        if (!rb_alloc_snaps(0)) { s_failed = 1; return; }
        t = rb_now();
        rb_copy_out(s_snapA);
        rb_put(s_t_full, rb_us(rb_now() - t));
        return;
    }
    (void)rb_quiesce(0);
    t = rb_now();
    nb = rb_diff_blocks(s_snapA);
    nd = rb_diff(s_snapA, 2, kinds, 0); /* zaehlen + Kopie nachziehen (= inkrementelles Sichern) */
    rb_put(s_t_saveA, rb_us(rb_now() - t));
    rb_put(s_dirty_k1, nd);
    rb_put(s_dirty_blk_k1, nb);
    memcpy(s_kinds_last, kinds, sizeof kinds);
}

/* Aufruf nach jedem Feld-Warten (bzw. statt dessen im Neurechnen), Spiel-Thread, Register gesichert. */
/* W330: Bereiche geaendert (Allokation/Freigabe/Schutzwechsel)? Laufenden Zyklus verwerfen, Ring neu. */
static int rb_ring_check_regions(void)
{
    if (g_exreg_n == s_reg_gen && g_loc_rb_memev == s_memev_seen) return 0;
    if (s_phase != PH_IDLE) { /* Zyklus abbrechen: Zeitlinie B wiederherstellen (alte Bereiche) */
        if (s_phase == PH_REPLAY) {
            __try { (void)rb_diff(s_snapB, 1, NULL, 0); } __except (EXCEPTION_EXECUTE_HANDLER) {}
            if (s_freeze) rb_thaw_others();
            if (s_rthread) ResumeThread(s_rthread);
            if (s_pdpause) sb_interp_rb_pause(0);
            InterlockedExchange(&s_skipwait, 0);
        }
        s_phase = PH_IDLE;
        s_pass = 0;
        s_r_abort++;
    }
    HLOG_I("LOCRB: RING Bereiche geaendert (Register %u -> %u, Freigaben/Schutz %ld -> %ld) bei Tick %u - Neuaufbau",
           s_reg_gen, g_exreg_n, s_memev_seen, g_loc_rb_memev, rd32(RB_TICK));
    s_r_rebuild++;
    rb_build_regions();
    if (s_mode == 2 && !rb_alloc_snaps(1)) s_failed = 1;
    return 1;
}

static void __cdecl rb_after(void)
{
    if (s_failed) return;
    if (!rd32(RB_ACTIVE)) { s_lst_frames = 0; return; }
    if (++s_lst_frames < (unsigned)s_warm && s_phase == PH_IDLE) return;
    if (!s_ready) {
        s_game_tid = GetCurrentThreadId();
        rb_threads_log();
        rb_wp_bench();
        rb_collect_stacks();
        if (s_mode == 2 || s_ring) { /* W330: Ring braucht die Ketten fuer rbr_prewrite (recv/recvfrom in Gast-Puffer) */
            int ok = 1;
            s_rec = (uint8_t *)malloc(RB_REC_CAP);
            ok &= rb_chain_jmp(RB_SELECT, (void *)rb_select, (void **)&o_select, "select");
            ok &= rb_chain_jmp(RB_RECV, (void *)rb_recv, (void **)&o_recv, "recv");
            ok &= rb_chain_jmp(RB_RECVFROM, (void *)rb_recvfrom, (void **)&o_recvfrom, "recvfrom");
            ok &= rb_chain_jmp(RB_SEND, (void *)rb_send, (void **)&o_send, "send");
            ok &= rb_chain_jmp(RB_SENDTO, (void *)rb_sendto, (void **)&o_sendto, "sendto");
            ok &= rb_chain_jmp(RB_IOCTL, (void *)rb_ioctl, (void **)&o_ioctl, "ioctlsocket");
            ok &= rb_chain_jmp(RB_WSAERR, (void *)rb_wsaerr, (void **)&o_wsaerr, "WSAGetLastError");
            ok &= rb_chain_jmp(RB_XIN_VA, (void *)rb_xin, (void **)&o_xin, "XInputGetState");
            ok &= rb_chain_call(RB_ACC_CALL, (void *)rb_acc_thunk, &s_acc_next, "Akkumulator");
            if (s_xact && s_mode == 2) { /* W330 xact=1 */
                int xo = 1;
                xo &= rb_chain_jmp(RB_XGN_VA, (void *)rb_xgn, (void **)&o_xgn, "XACT GetNotification");
                xo &= rb_chain_jmp(RB_XPLAY_VA, (void *)rb_xplay, (void **)&o_xplay, "XACT PlayEx");
                xo &= rb_chain_jmp(RB_XPREP_VA, (void *)rb_xprep, (void **)&o_xprep, "XACT PrepareEx");
                xo &= rb_chain_jmp(RB_XSTOP_VA, (void *)rb_xstop, (void **)&o_xstop, "XACT Stop");
                xo &= rb_chain_jmp(RB_XSTAT_VA, (void *)rb_xstat, (void **)&o_xstat, "XACT GetStatus");
                xo &= rb_chain_jmp(RB_XSTOPSRC_VA, (void *)rb_xstopsrc, (void **)&o_xstopsrc, "XACT StopSoundCues");
                xo &= rb_chain_jmp(RB_XWORK_VA, (void *)rb_xwork, (void **)&o_xwork, "XACTEngineDoWork");
                HLOG_I("LOCRB: XACT-Aufrufe beim Neurechnen aus der Aufzeichnung (xact=1): %s", xo ? "verkettet" : "UNVOLLSTAENDIG");
                if (!xo) s_xact = 0;
            }
            if (!ok || !s_rec || !s_rthread) {
                HLOG_E("LOCRB: Verkettung unvollstaendig (ok=%d rec=%p render=%p) - Spike aus", ok, (void *)s_rec,
                       (void *)s_rthread);
                s_failed = 1;
                return;
            }
            if (s_census && s_mode == 2) { /* Render-Thread waehrend des Umlenkens anhalten */
                int q = rb_quiesce(1);
                rb_census_install();
                if (q && s_rthread) ResumeThread(s_rthread);
            }
        }
        /* W330: Bereiche/Ring erst nach allen Code-Patches (Ketten, Zensus): VirtualProtect dort hebt sonst den
         * Ring-Schreibschutz unverfolgt auf */
        rb_build_regions();
        if (!rb_alloc_snaps(s_mode == 2)) { s_failed = 1; return; }
        if (!s_ring) {
            long long t = rb_now();
            rb_copy_out(s_snapA);
            rb_put(s_t_full, rb_us(rb_now() - t));
        }
        s_ready = 1;
        HLOG_I("LOCRB: bereit bei Tick %u (Lockstep-Rahmen %u), Modus %d, k=%d..%d, render=%d, Spiel-Thread %lu",
               rd32(RB_TICK), s_lst_frames, s_mode, s_k[0], s_k[s_nk - 1], s_render, s_game_tid);
        return;
    }
    if (s_ring) { /* W330: Tick-Ende sichern (vor dem Zyklus-Schritt, der laden kann) */
        if (rb_ring_check_regions()) return;
        if (s_phase != PH_REPLAY) (void)rb_quiesce(0);
        rb_ring_commit(s_phase == PH_REPLAY);
    }
    if (s_mode == 1) rb_dirty_step();
    else if (s_mode == 2) rb_cycle_step();
}

static __declspec(naked) void rb_thunk(void)
{
    __asm {
        cmp dword ptr [s_skipwait], 0
        jne rb_skip
        push dword ptr [esp + 4]
        call dword ptr [s_wait_fn]
        add esp, 4
    rb_skip:
        pushad
        pushfd
        call rb_after
        popfd
        popad
        ret
    }
}

/* Diagnose: Zustand des Spikes bei einer Zugriffsverletzung im Spiel-Thread (einmal), dann weiter wie bisher. */
static volatile LONG s_veh_once;
static LONG CALLBACK rb_veh_diag(EXCEPTION_POINTERS *ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || GetCurrentThreadId() != s_game_tid ||
        InterlockedExchange(&s_veh_once, 1))
        return EXCEPTION_CONTINUE_SEARCH;
    HLOG_W("LOCRB: ZUGRIFFSVERLETZUNG im Spiel-Thread EIP=0x%X: Phase=%d Zyklus=%u Gegenprobe=%d Durchgang=%d k=%d "
           "vor=%d neu=%d Aufzeichnung %u/%u Ereignisse Abw=%u (Art 0x%X) | Tick=%u Desync=%u Ende=%u Modus=%u Auftrag=%u",
           (u32)ep->ContextRecord->Eip, s_phase, s_cyc + s_ctl_n, s_ctl, s_pass, s_kcur, s_fwd, s_rep, s_rec_cur,
           s_rec_len, s_rep_div, s_rep_div_fn, rd32(RB_TICK), rd32(RB_DESYNC), rd32(0x3068E0u), rd32(0x306B60u),
           rd32(RB_RJOB));
    rb_census_log();
    return EXCEPTION_CONTINUE_SEARCH;
}

static void rb_parse_opts(const char *p)
{
    while (p && *p) {
        while (*p == ';' || *p == ' ') p++;
        if (strncmp(p, "k=", 2) == 0) {
            p += 2;
            s_nk = 0;
            while (*p && *p != ';' && s_nk < 8) {
                int v = (int)strtol(p, (char **)&p, 10);
                if (v >= 1 && v <= 30) s_k[s_nk++] = v;
                if (*p == ',') p++;
                else if (*p && *p != ';') p++;
            }
            if (!s_nk) { s_k[0] = 1; s_nk = 1; }
        } else if (strncmp(p, "render=", 7) == 0) { s_render = atoi(p + 7); }
        else if (strncmp(p, "warm=", 5) == 0) { s_warm = atoi(p + 5); }
        else if (strncmp(p, "max=", 4) == 0) { s_max = atoi(p + 4); }
        else if (strncmp(p, "census=", 7) == 0) { s_census = atoi(p + 7); }
        else if (strncmp(p, "perturb=", 8) == 0) { s_perturb = atoi(p + 8); }
        else if (strncmp(p, "twice=", 6) == 0) { s_twice = atoi(p + 6); }
        else if (strncmp(p, "freeze=", 7) == 0) { s_freeze = atoi(p + 7); }
        else if (strncmp(p, "ww=", 3) == 0) { s_ww = atoi(p + 3); }             /* W330 */
        else if (strncmp(p, "depth=", 6) == 0) { s_depth = atoi(p + 6); }
        else if (strncmp(p, "pool=", 5) == 0) { s_pool = atoi(p + 5); }
        else if (strncmp(p, "verify=", 7) == 0) { s_verify = atoi(p + 7); }
        else if (strncmp(p, "pdpause=", 8) == 0) { s_pdpause = atoi(p + 8); }
        else if (strncmp(p, "shadow=", 7) == 0) { s_shadowchk = atoi(p + 7); }
        else if (strncmp(p, "hot=", 4) == 0) { s_hot = atoi(p + 4); }
        else if (strncmp(p, "xact=", 5) == 0) { s_xact = atoi(p + 5); }
        else if (strncmp(p, "cool=", 5) == 0) { s_cool = atoi(p + 5); }
        while (*p && *p != ';') p++;
    }
}

int loc_rb_install(void)
{
    static const uint8_t expect[7] = {0x6A, 0x03, 0xE8, 0x52, 0x3D, 0x00, 0x00}; /* push 3; call 0x33050 */
    uint8_t *p = (uint8_t *)(uintptr_t)RB_CALL_VA;
    intptr_t rel;
    DWORD old;
    LARGE_INTEGER f;
    if (g_cfg.loc_rb_spike <= 0 && g_cfg.loc_rb <= 0) return 0;
    if (g_cfg.loc_pace_diag || g_cfg.loc_fields > 0) {
        HLOG_W("LOCRB: loc_pace_diag/loc_fields belegen @0x%X - Spike aus", RB_CALL_VA);
        return 0;
    }
    if (memcmp(p, expect, sizeof expect) != 0) {
        HLOG_E("LOCBYTES LOCRB @0x%X: Byte-Mismatch - ABBRUCH (exit 7)", RB_CALL_VA);
        hlog_close();
        ExitProcess(7);
    }
    QueryPerformanceFrequency(&f);
    s_qpf = f.QuadPart ? f.QuadPart : 1;
    s_mode = g_cfg.loc_rb_spike >= 2 ? 2 : (g_cfg.loc_rb_spike == 1 ? 1 : 0);
    s_ring = g_cfg.loc_rb > 0;
    if (s_ring && s_mode == 1) {
        HLOG_W("LOCRB: loc_rb_spike=1 (Vergleich je Tick) und loc_rb=1 schliessen sich aus - nur Ring");
        s_mode = 0;
    }
    rb_parse_opts(g_cfg.loc_rb_opts);
    if (s_depth < 12) s_depth = 12; /* R1: mindestens 12 Ticks */
    g_loc_rb_ww = s_ring && s_ww; /* vor den ersten Gast-Allokationen (Aufruf vor dem Spielstart) */
    s_q_wait = rb_ser_new("Ruhepunkt Warten auf Render-Thread (us)");
    s_q_susp = rb_ser_new("Render-Thread anhalten (us)");
    s_q_frz = rb_ser_new("Uebrige Gast-Threads anhalten (us)");
    s_t_full = rb_ser_new("Vollkopie Gast-Speicher (us)");
    s_t_saveA = rb_ser_new(s_mode == 1 ? "Inkrementell sichern je Tick inkl. Vergleich (us)" : "Sichern A Vollkopie (us)");
    s_t_saveB = rb_ser_new("Sichern B Vollkopie (us)");
    s_t_loadA = rb_ser_new("Laden A seitenweise (us)");
    s_t_loadB = rb_ser_new("Laden B seitenweise (us)");
    s_t_cmp = rb_ser_new("Vergleich Ergebnis<->B (us)");
    s_t_tick = rb_ser_new("Neurechnen je Tick (us)");
    s_t_tickfwd = rb_ser_new("Vorlauf je Tick, Echtzeit (us)");
    s_t_rep = rb_ser_new("Neurechnen k Ticks gesamt (us)");
    s_dirty_k1 = rb_ser_new("Geaenderte Seiten je Tick (4 KB)");
    s_dirty_blk_k1 = rb_ser_new("Geaenderte 16-B-Bloecke je Tick");
    s_dirty_pg = rb_ser_new("Geladene Seiten je Rollback (k Ticks)");
    s_rec_bytes = rb_ser_new("Aufzeichnung je Zyklus (Bytes)");
    s_tw_nd = rb_ser_new("Seiten Neurechnung1<->Neurechnung2");
    s_r_commit = rb_ser_new("RING Sichern je Tick (us)");
    s_r_commit_rep = rb_ser_new("RING Sichern je Neurechen-Tick (us)");
    s_r_total = rb_ser_new("RING Sichern je Tick inkl. Schreibschutz-Fehler (us)");
    s_r_dirty = rb_ser_new("RING gemeldete Seiten je Tick");
    s_r_saved = rb_ser_new("RING geaenderte Seiten je Tick");
    s_r_faults = rb_ser_new("RING Schreibschutz-Fehler je Tick");
    s_r_fault_us = rb_ser_new("RING Schreibschutz-Fehler Zeit je Tick (us)");
    s_r_ww = rb_ser_new("RING GetWriteWatch je Tick (us)");
    s_r_prot = rb_ser_new("RING Schutz setzen je Tick (us)");
    s_r_copy = rb_ser_new("RING Vergleich+Kopie je Tick (us)");
    s_r_ringload = rb_ser_new("RING Laden k Ticks (us)");
    s_r_ldpages = rb_ser_new("RING geladene Seiten je Rollback");
    s_r_setup = rb_ser_new("RING Aufbau (us)");
    s_r_cpu = rb_ser_new("RING Sichern CPU (100 Zyklen)");
    AddVectoredExceptionHandler(1, rb_veh_diag);
    rel = (uint8_t *)(void *)rb_thunk - (p + 7);
    VirtualProtect(p, 7, PAGE_EXECUTE_READWRITE, &old);
    memcpy(p + 3, &rel, 4);
    VirtualProtect(p, 7, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 7);
    HLOG_I("LOCRB: Spike aktiv, Modus %d (%s), Ruhepunkt @0x%X, Optionen \"%s\"", s_mode,
           s_mode == 1 ? "Zustand je Tick erfassen" : (s_mode == 2 ? "Determinismus-Zyklen" : "nur Ring"), RB_CALL_VA,
           g_cfg.loc_rb_opts);
    if (s_ring)
        HLOG_I("LOCRB: RING eingeschaltet (loc_rb=1): Write-Watch fuer NtAlloc=%d, Tiefe %d, Puffer %d Seiten, verify=%d, "
               "Present-Ruhe=%d", g_loc_rb_ww, s_depth, s_pool, s_verify, s_pdpause);
    return 1;
}

/* W330: Ring-Zeilen (alle 10 s Fenster, alle 60 s Reihen + fremde Schreiber) */
static void rb_writer_name(u32 eip, char *out, size_t cap)
{
    HMODULE m = NULL;
    char path[MAX_PATH];
    const char *b;
    if (eip >= 0x10000u && eip < 0x2800000u) { sprintf_s(out, cap, "Gast-Code 0x%X", eip); return; }
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (const char *)(uintptr_t)eip, &m) && m && GetModuleFileNameA(m, path, sizeof path)) {
        b = strrchr(path, '\\');
        sprintf_s(out, cap, "%s+0x%X", b ? b + 1 : path, eip - (u32)(uintptr_t)m);
    } else {
        sprintf_s(out, cap, "0x%X", eip);
    }
}

static void rb_ring_log(int waited_s)
{
    u32 a, b, c, d, e, f, g, h, k, sp50, sp99, smx;
    rbr_stat_t st;
    if (!rbr_active() && !s_r_n) return;
    rbr_stats(&st);
    rb_win_pct(&s_w_total, &a, &b, &c);
    rb_win_pct(&s_w_commit, &sp50, &sp99, &smx);
    rb_win_pct(&s_w_saved, &d, &e, &f);
    rb_win_pct(&s_w_dirty, &g, &h, &k);
    HLOG_I("LOCRB: RING t=%ds Tick=%u Ticks=%u Sichern inkl. Fehler p50/p99/max=%u/%u/%u us (ohne Fehler %u/%u/%u) | Seiten "
           "geaendert p50/p99/max=%u/%u/%u gemeldet %u/%u/%u | Tiefe=%u Ueberlauf=%u verworfen=%u Neuaufbau=%u Abbruch=%u | "
           "Fehler gesamt=%u fremd=%u Spiegel=%u prewrite=%u | heiss=%u abgekuehlt=%u",
           waited_s, rd32(RB_TICK), s_w_total.n, a, b, c, sp50, sp99, smx, d, e, f, g, h, k, rbr_depth(), s_r_ovf,
           s_r_drop, s_r_rebuild, s_r_abort, st.faults, st.faults_foreign, st.faults_alias, st.prewrites, s_r_hot_last,
           s_r_cooled);
    s_w_total.n = s_w_commit.n = s_w_saved.n = s_w_dirty.n = s_w_faults.n = 0;
    if (s_r_ld_n || s_r_ld_bad || s_r_shadow_bad || s_r_pd_fail || st.prot_fail)
        HLOG_I("LOCRB: RING Laden=%u (falsch gegen Schatten-Kopie A: %u, fremd beschrieben vor Zyklusbeginn: %u) "
               "Schatten-Fehler=%u Present-Ruhe verfehlt=%u Schutz-Fehlschlaege=%u | Anhalten mit Sperre "
               "wiederholt=%u verworfen=%u",
               s_r_ld_n, s_r_ld_bad, s_r_foreign_win, s_r_shadow_bad, s_r_pd_fail, st.prot_fail, s_susp_retry,
               s_susp_fail);
    if (waited_s % 60 == 0) {
        rbr_writer_t w[16];
        unsigned i, nw;
        rb_logser(s_r_total, "us");
        {
            double hz = rb_tsc_hz();
            u32 a1, b1, c1;
            rb_pct(s_r_cpu, &a1, &b1, &c1);
            if (hz > 0 && s_r_cpu && s_r_cpu->n)
                HLOG_I("LOCRB: RING Sichern je Tick CPU-Zeit des Spiel-Threads (ohne Verdraengung) n=%u p50=%u p99=%u max=%u us "
                       "(TSC %.0f MHz)",
                       s_r_cpu->n, (u32)(a1 * 100.0 * 1e6 / hz), (u32)(b1 * 100.0 * 1e6 / hz), (u32)(c1 * 100.0 * 1e6 / hz),
                       hz / 1e6);
        }
        rb_logser(s_r_commit, "us");
        rb_logser(s_r_commit_rep, "us");
        rb_logser(s_r_ww, "us");
        rb_logser(s_r_prot, "us");
        rb_logser(s_r_copy, "us");
        rb_logser(s_r_fault_us, "us");
        rb_logser(s_r_faults, "Fehler");
        rb_logser(s_r_dirty, "Seiten");
        rb_logser(s_r_saved, "Seiten");
        rb_logser(s_r_ringload, "us");
        rb_logser(s_r_ldpages, "Seiten");
        rb_logser(s_r_setup, "us");
        for (i = 0; i < 64; i++) { /* heisse Seiten je Bereich */
            uint32_t va, len, kind, hot;
            int meth;
            if (!rbr_region_info((int)i, &va, &len, &kind, &meth, &hot)) break;
            if (hot) HLOG_I("LOCRB: RING heisse Seiten Bereich 0x%X-0x%X Art %u %s: %u von %u", va, va + len, kind,
                            meth == RBR_WW ? "WW" : "WP", hot, len / 4096u);
        }
        nw = rbr_writers(w, 16);
        for (i = 0; i < nw; i++) {
            char where[160];
            int gi, ng = krnl_xb_thread_count(), gs = -1;
            for (gi = 0; gi < ng; gi++) {
                void *hh = NULL;
                unsigned r = 0;
                if (krnl_xb_thread_get(gi, &hh, &r) && GetThreadId((HANDLE)hh) == w[i].tid) { gs = (int)r; break; }
            }
            rb_writer_name(w[i].first_eip, where, sizeof where);
            HLOG_I("LOCRB: RING fremder Schreiber tid=%u (%s%s0x%X) Fehler=%u erste Adresse 0x%X letzte 0x%X Code %s", w[i].tid,
                   gs >= 0 ? "Gast-Thread Start " : "Host-Thread", gs >= 0 ? "=" : " ", gs >= 0 ? (u32)gs : 0u, w[i].n,
                   w[i].first_va, w[i].last_va, where);
        }
    }
}

void loc_rb_tick(int waited_s)
{
    u32 a, b, c;
    if ((!s_mode && !s_ring) || waited_s % 10 != 0) return;
    if (!s_ready) return;
    if (s_ring) rb_ring_log(waited_s);
    if (s_mode == 0) {
    } else if (s_mode == 1) {
        rb_pct(s_dirty_k1, &a, &b, &c);
        HLOG_I("LOCRB: t=%ds Tick=%u Bereiche=%d %u KB | Seiten je Tick p50/p99/max=%u/%u/%u (letzter: Abbild %u Contig %u "
               "NtAlloc %u Pool %u)",
               waited_s, rd32(RB_TICK), s_nreg, (unsigned)(s_total / 1024u), a, b, c, s_kinds_last[0], s_kinds_last[1],
               s_kinds_last[2], s_kinds_last[5]);
    } else {
        HLOG_I("LOCRB: t=%ds Tick=%u Zyklen=%u gleich=%u Hash-verschieden=%u Seiten-verschieden=%u Aufruf-Abw=%u "
               "verworfen=%u | k: %d:%u/%u %d:%u/%u %d:%u/%u %d:%u/%u | Ruhepunkt belegt=%u Zeitueberschreitung=%u",
               waited_s, rd32(RB_TICK), s_cyc, s_cyc_ok, s_cyc_hash_bad, s_cyc_mem_bad, s_cyc_div, s_cyc_skip, s_k[0],
               s_cyc_k_ok[0], s_cyc_k[0], s_k[1 % s_nk], s_cyc_k_ok[1 % s_nk], s_cyc_k[1 % s_nk], s_k[2 % s_nk],
               s_cyc_k_ok[2 % s_nk], s_cyc_k[2 % s_nk], s_k[3 % s_nk], s_cyc_k_ok[3 % s_nk], s_cyc_k[3 % s_nk], s_q_busy,
               s_q_timeout);
        if (s_perturb)
            HLOG_I("LOCRB: Gegenprobe (Eingabe verfaelscht) Hash verschieden=%u von %u | k: %d:%u/%u %d:%u/%u %d:%u/%u %d:%u/%u", s_ctl_det,
                   s_ctl_n, s_k[0], s_ctl_k_det[0], s_ctl_k[0], s_k[1 % s_nk], s_ctl_k_det[1 % s_nk], s_ctl_k[1 % s_nk],
                   s_k[2 % s_nk], s_ctl_k_det[2 % s_nk], s_ctl_k[2 % s_nk], s_k[3 % s_nk], s_ctl_k_det[3 % s_nk],
                   s_ctl_k[3 % s_nk]);
    }
    if (waited_s % 60 == 0) {
        unsigned i;
        rb_logser(s_q_wait, "us");
        rb_logser(s_q_susp, "us");
        rb_logser(s_q_frz, "us");
        rb_logser(s_t_full, "us");
        rb_logser(s_t_saveA, "us");
        rb_logser(s_t_saveB, "us");
        rb_logser(s_t_loadA, "us");
        rb_logser(s_t_loadB, "us");
        rb_logser(s_t_cmp, "us");
        rb_logser(s_t_tick, "us");
        rb_logser(s_t_tickfwd, "us");
        rb_logser(s_t_rep, "us");
        rb_logser(s_dirty_k1, "Seiten");
        rb_logser(s_dirty_blk_k1, "Bloecke");
        rb_logser(s_dirty_pg, "Seiten");
        rb_logser(s_rec_bytes, "Bytes");
        if (s_mode == 2) {
            for (i = 1; i < EV_N; i++) {
                if (i == EV_ACC) continue;
                if (s_cnt_ev[i][0] || s_cnt_ev[i][1])
                    HLOG_I("LOCRB: Aufrufe %s aufgezeichnet=%u wiedergegeben=%u", k_evname[i], s_cnt_ev[i][0],
                           s_cnt_ev[i][1]);
            }
            if (s_twice) {
                rb_logser(s_tw_nd, "Seiten");
                HLOG_I("LOCRB: Zweifach-Neurechnung n=%u Hash gleich=%u Speicher gleich=%u (max %u Seiten verschieden)",
                       s_tw_n, s_tw_hash_same, s_tw_mem_same, s_tw_nd_max);
                for (i = 0; i < s_nah2 && i < 32; i++)
                    HLOG_I("LOCRB: Neurechnung1<->2 abweichende Seite 0x%X (erstes Wort 0x%X) in %u Zyklen", s_ah2[i].page,
                           s_ah2[i].first, s_ah2[i].n);
            }
            for (i = 0; i < s_nah && i < 48; i++) {
                if (s_ah[i].n >= 2 || i < 16)
                    HLOG_I("LOCRB: Abweichende Seite 0x%X (erstes Wort 0x%X) in %u Zyklen", s_ah[i].page, s_ah[i].first,
                           s_ah[i].n);
            }
            rb_census_log();
        }
    }
}
