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
#include <winsock2.h> /* W338: vor windows.h (Eingabekanal) */
#include <windows.h>

#include "loc_rollback.h"
#include "loc_lsarb_game.h" /* LOC-LOCKSTEP-ARBITER */
#include "hcfg.h"
#include "hlog.h"
#include "hooks_int.h"
#include "kernel_hle.h"
#include "loc_pad.h"
#include "loc_rb_ring.h" /* W330 LOC-ROLLBACK-R1 */
#include "loc_mem.h"
#include "d3d11_render.h" /* W330: sb_interp_rb_pause */
#include "loc_rbi.h"       /* W338 LOC-ROLLBACK-R2 */
#include "loc_net.h"
#include "loc_net_core.h"
#include "loc_netsim.h"
#include "loc_tun.h"
#include "loc_sync.h"
#include "loc_rbn.h"       /* W346 LOC-ROLLBACK-R4: Aushandlung */
#include "loc_audio.h"     /* W375: Rollback-Waisen (Schleifen-Cues) stoppen */
#include "loc_rb_xkey.h"    /* W381 (#61): Stopp-/Status-Schluessel mit Handle bzw. Quelle */
#include "loc_rb_sndloop.h" /* W413 (#65): Ton nach dem Neurechnen mit der korrigierten Zeitlinie abgleichen */
#include "loc_guard.h"
#include "sb_lang.h"
#include "sb_origview.h"   /* W346: Einblendung bei Desync */
#include "loc_aim.h"       /* W376 LOC-LOCAL-AIM */
#include "loc_smooth.h"    /* W385 LOC-SMOOTH */

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
int (*volatile g_loc_rb_alloc)(int op, unsigned size, unsigned type, unsigned prot, unsigned *io); /* W346 R3 */
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
/* W338: MmCreateKernelStack-Speicher mitsichern (W330: ja). Laeuft ein Thread darauf, macht der Schreibschutz die
 * Ausnahmezustellung unmoeglich (harter Abbruch ohne Bericht, A12-A14 nach dem ersten Schuss) - mit R2 aus. */
static int s_kstack = 1;
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
unsigned krnl_memreg_ab(unsigned i); /* krnl_mem.c (W338) */
static unsigned s_stale; /* W338: veraltete Registereintraege beim Bereichsaufbau uebersprungen */
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
#define RB_EXMAX 320
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
static int s_stkx = 1, s_abchk = 1;
#define RB_HSMAX 256
static rbr_stack_t s_hs[RB_HSMAX];
static int s_nhs;
static unsigned s_hs_thr, s_hs_noteb;
static u32 s_hs_in, s_ab_skip, s_ab_skip_kb, s_w362_logged;
static u32 *s_abs;
static unsigned s_nabs, s_abs_cap;
static void rb_host_stacks(void) { s_nhs = rbr_thread_stacks(s_hs, RB_HSMAX, &s_hs_thr, &s_hs_noteb); }
static int rb_u32cmp(const void *x, const void *y)
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
    if (!s_r_gen) hlog_flush(); /* W362: erster Aufbau - Log vor dem Schutz leeren (stilles Ende eingrenzen) */
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
static int s_avlog;
static HANDLE s_avlog_h = INVALID_HANDLE_VALUE;
static volatile LONG s_avlog_n;
static volatile LONG s_avlog_arm;
static LONG CALLBACK rb_avlog_veh(EXCEPTION_POINTERS *ep)
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
static int s_r_force_rebuild;   /* W338: Ring beim naechsten Pruefen neu aufbauen */
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
#define RB_VQC 64
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

static void rb_build_regions(void)
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
       EV_XGN, EV_XPLAY, EV_XPREP, EV_XSTOP, EV_XSTAT, EV_XSTOPSRC, EV_XWORK, EV_NTALLOC, EV_NTCOMMIT, EV_N };
static const char *const k_evname[] = {"-", "select", "recv", "recvfrom", "send", "sendto", "ioctlsocket",
                                       "WSAGetLastError", "XInputGetState", "Akkumulator",
                                       "XACT GetNotification", "XACT PlayEx", "XACT PrepareEx", "XACT Stop",
                                       "XACT SoundSource_GetStatus", "XACT StopSoundCues", "XACTEngineDoWork",
                                       "NtAllocateVirtualMemory", "NtAllocateVirtualMemory(Commit)"};
typedef struct { uint8_t fn, pad; uint16_t n; u32 ret, ih; } rb_ev_t; /* ih = Hash der Eingangsargumente */
#define RB_REC_CAP (4u << 20)
static uint8_t *s_rec;
static u32 s_rec_len, s_rec_cur;
static u32 s_rec_events;
enum { PH_IDLE = 0, PH_RECORD, PH_REPLAY };
static volatile int s_phase;
/* ---- W390 LOC-OWN-PREDICT: spekulativer Vorlauf nur fuer die Anzeige ----
 * Nach einem Live-Tick mit Sichern (Schatten = aktueller Stand) rechnet der Spiel-Thread D + 1 Ticks weiter (eigene
 * schon erzeugte Rahmen + im Vorlauf neu erzeugter Rahmen, fremde vorhergesagt), merkt die eigene VT-Lage und setzt den
 * Speicher mit rbr_revert auf den gesicherten Stand zurueck. Phase dabei PH_REPLAY (Render-/Ton-Pfade wie beim
 * Neurechnen), zusaetzlich s_spec = 1: alle Haken ohne Wirkung nach aussen (kein Senden, kein Empfang, kein Ton, keine
 * Aufzeichnung, kein Hash, keine Freigabe). */
static volatile int s_spec;
typedef struct {
    u32 send, sel, recv, ioctl, wsa, xin_live, xin_rep, xgn, xcue, xstop, acc, alloc, commit, take, take_pred, ec80;
    u32 kfile[6]; /* loc_rb_spec_note: 1 Lesen, 2 Schreiben, 3 Schliessen, 4 Datei-Info, 5 Freigabe */
} rb_spec_ev_t;
static rb_spec_ev_t s_spec_ev; /* abgefangene Nebenwirkungen (gesamt) */
#define RB_SPEC_XC 4
static int s_spec_pd; /* W393: dieser Vorlauf haelt den Present-Takt an (loc_pd_spec=0) */
static struct { u32 h, ret, n, used; uint8_t d[32]; } s_spec_xc[RB_SPEC_XC]; /* XInput im Vorlauf live -> naechster Live-Tick */
static u32 s_spec_xcn, s_spec_xc_hit, s_spec_xc_unused;
static uint8_t s_spec_own_new[22]; /* RBI_FR: im Vorlauf erzeugter eigener Rahmen (Kopf + Satz) */
static int s_spec_own_have;
static int s_spec_paused_n, s_spec_off_mission; /* Kosten-Rueckfall je Mission */
static u32 s_rb_esp0, s_rb_caller, s_spec_esp0, s_spec_caller, s_spec_ctx_bad; /* Aufrufer/Stapel des Feld-Wartens */
static u32 s_spec_degr;
static int s_spec_level; /* Rueckfall-Stufe: 0 = D + 1 Ticks, 1 = D Ticks (danach Pause) */
static DWORD s_spec_pause_until;
static u32 s_spec_win_n;
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

static u32 rb_rep_limit(void); /* W338: Ende der Wiedergabe (R2: Ende des aktuellen Neurechen-Ticks) */
/* Naechstes Ereignis der Wiedergabe; NULL bei Abweichung (Art oder Eingangsargumente). */
static int s_rbi_ep;      /* W338 (vorgezogen) */
static u32 s_rep_kc[EV_N]; /* W346 R3: Wiedergabe-Zeiger je Ereignisart im laufenden Neurechen-Tick */
static u32 s_rep_end;      /* W338 (vorgezogen) */
static u32 s_rbi_send_live; /* W346 R3: beim Neurechnen live gesendet (nur die korrigierte Zeitlinie sendet) */
static const rb_ev_t *rb_rep_get(int fn, u32 ih, const uint8_t **data)
{
    const rb_ev_t *e;
    if (s_rbi_ep) { /* W346 R3: je Ereignisart in Reihenfolge (eine andere Verschraenkung der Arten ist kein Fehler) */
        u32 c = s_rep_kc[fn];
        while (c + sizeof(rb_ev_t) <= s_rep_end) {
            e = (const rb_ev_t *)(s_rec + c);
            if (e->fn == (uint8_t)fn) {
                if (e->ih != ih) break;
                s_rep_kc[fn] = c + (u32)sizeof *e + e->n;
                s_cnt_ev[fn][1]++;
                if (data) *data = (const uint8_t *)(e + 1);
                return e;
            }
            c += (u32)sizeof *e + e->n;
        }
        if (!s_rep_div) { s_rep_div_fn = (u32)fn; s_rep_div_at = c; }
        s_rep_div++;
        return NULL;
    }
    if (s_rec_cur + sizeof(rb_ev_t) > rb_rep_limit()) {
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
    if (s_spec) { /* W390: Vorlauf - nichts lesbar (die echte Zeitlinie holt es ab) */
        s_spec_ev.sel++;
        if (r) *(u32 *)(uintptr_t)r = 0;
        if (w) *(u32 *)(uintptr_t)w = 0;
        if (x) *(u32 *)(uintptr_t)x = 0;
        return 0;
    }
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
    if (s_spec) { s_spec_ev.recv++; s_fake_err = WSAEWOULDBLOCK_X; return 0xFFFFFFFFu; } /* W390 */
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
    if (s_spec) { s_spec_ev.recv++; s_fake_err = WSAEWOULDBLOCK_X; return 0xFFFFFFFFu; } /* W390 */
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

/* W355: Beim Neurechnen (R2) ein Senden gleicher Laenge auf demselben Socket im selben Tick suchen. Der TCP-Strom
 * hat die Nachricht der ersten Rechnung schon - eine zweite Kopie (nur der Inhalt, z. B. das Sync-Byte des
 * 0x34-Herzschlags, ist anders) waere eine doppelte Nachricht beim Mitspieler. W346 schickte bei jedem
 * ueberlappenden Rollback jede solche Nachricht erneut live (Nutzerlauf: 612 Kopien in 5 s). */
static u32 s_rbi_toff[RBI_GH]; /* W355: vorgezogen (Definition unten, R2) */
static void rb_pcom_clear(void); /* W355 */
static void rb_pcom_rollback(void);
static rbi_t *s_rbc;
static u32 s_rbi_send_same; /* W355: beim Neurechnen als schon gesendet gewertet (gleiche Laenge, anderer Inhalt) */
static u32 s_rbi_sendto_rep, s_rbi_sendto_same; /* W355: Datagramme beim Neurechnen */
static u32 s_rbi_send_rep, s_rbi_send_exact, s_rbi_send_nosock, s_rbi_send_otherlen, s_rbi_send_logged; /* W355 DIAG */
static const rb_ev_t *rb_rep_get_sl(int fn, u32 s, u32 len, u32 toh)
{
    u32 c = s_rep_kc[fn], nk = fn == EV_SENDTO ? 12u : 8u;
    while (c + sizeof(rb_ev_t) <= s_rep_end) {
        const rb_ev_t *e = (const rb_ev_t *)(s_rec + c);
        u32 nx = c + (u32)sizeof *e + e->n;
        if (e->fn == (uint8_t)fn && e->n == nk) {
            const u32 *d = (const u32 *)(e + 1);
            if (d[0] == s && d[1] == len && (nk == 8u || d[2] == toh)) {
                s_rep_kc[fn] = nx;
                s_cnt_ev[fn][1]++;
                return e;
            }
        }
        c = nx;
    }
    return NULL;
}

static u32 __stdcall rb_send(u32 s, u32 buf, u32 len, u32 flags)
{
    u32 ih;
    if (!rb_mine()) return o_send(s, buf, len, flags);
    if (s_spec) { s_spec_ev.send++; return len; } /* W390: Vorlauf sendet nie */
    ih = fnv(s * 31u + len, (const void *)(uintptr_t)buf, len > 0x2000 ? 0x2000 : len); /* Inhalt nur als Hash */
    if (s_phase == PH_RECORD) {
        u32 ret = o_send(s, buf, len, flags), sl[2];
        sl[0] = s; /* W355: Socket und Laenge (kein Inhalt) fuer die Zuordnung beim Neurechnen */
        sl[1] = len;
        rb_rec_put(EV_SEND, ret, ih, sl, 8u);
        return ret;
    } else {
        const rb_ev_t *e = rb_rep_get(EV_SEND, ih, NULL); /* unterdrueckt: nicht senden */
        if (s_rbi_ep) s_rbi_send_rep++;
        if (e && s_rbi_ep) s_rbi_send_exact++;
        if (!e && s_rbi_ep) {
            e = rb_rep_get_sl(EV_SEND, s, len, 0);
            if (e) { s_rbi_send_same++; return e->ret; }
            { /* W355 DIAG: was lag in diesem Tick an Sendungen vor (nur Socket/Laengen, kein Inhalt) */
                u32 c = s_rbi_toff[rbi_gtick(s_rbc) & (RBI_GH - 1u)], nsock = 0, nother = 0, l0 = 0, n = 0;
                while (c + sizeof(rb_ev_t) <= s_rep_end) {
                    const rb_ev_t *x = (const rb_ev_t *)(s_rec + c);
                    if (x->fn == EV_SEND && x->n == 8u) {
                        const u32 *dd = (const u32 *)(x + 1);
                        n++;
                        if (dd[0] == s) { nsock++; if (dd[1] != len) { nother++; if (!l0) l0 = dd[1]; } }
                    }
                    c += (u32)sizeof *x + x->n;
                }
                if (!nsock) s_rbi_send_nosock++;
                else s_rbi_send_otherlen++;
                if (s_rbi_send_logged < 12) {
                    s_rbi_send_logged++;
                    HLOG_I("LOCRBI: DIAG Neurechnen Senden live: Tick %u Laenge %u | Aufzeichnung des Ticks: %u Sendungen, %u auf "
                           "dem Socket, %u mit anderer Laenge (erste %u), Fenster %u..%u", rbi_gtick(s_rbc), len, n, nsock, nother,
                           l0, s_rbi_toff[rbi_gtick(s_rbc) & (RBI_GH - 1u)], s_rep_end);
                }
            }
            s_rbi_send_live++;
            return o_send(s, buf, len, flags); /* W346: nur die korrigierte Zeitlinie sendet */
        }
        return e ? e->ret : len;
    }
}

static u32 __stdcall rb_sendto(u32 s, u32 buf, u32 len, u32 flags, u32 to, u32 tolen)
{
    u32 ih;
    if (!rb_mine()) return o_sendto(s, buf, len, flags, to, tolen);
    if (s_spec) { s_spec_ev.send++; return len; } /* W390 */
    ih = fnv(s * 31u + len, (const void *)(uintptr_t)buf, len > 0x2000 ? 0x2000 : len);
    if (s_phase == PH_RECORD) {
        u32 ret = o_sendto(s, buf, len, flags, to, tolen), sl[3];
        sl[0] = s; /* W355: Socket, Laenge, Ziel (Hash, kein Inhalt) fuer die Zuordnung beim Neurechnen */
        sl[1] = len;
        sl[2] = fnv(2166136261u, (const void *)(uintptr_t)to, to && tolen <= 32u ? tolen : 0u);
        rb_rec_put(EV_SENDTO, ret, ih, sl, 12u);
        return ret;
    } else {
        const rb_ev_t *e = rb_rep_get(EV_SENDTO, ih, NULL);
        if (s_rbi_ep) s_rbi_sendto_rep++;
        if (!e && s_rbi_ep) { /* W355: gleiches Datagramm (Socket/Laenge/Ziel) der ersten Rechnung = schon gesendet */
            e = rb_rep_get_sl(EV_SENDTO, s, len, fnv(2166136261u, (const void *)(uintptr_t)to, to && tolen <= 32u ? tolen : 0u));
            if (e) { s_rbi_sendto_same++; return e->ret; }
            s_rbi_send_live++;
            return o_sendto(s, buf, len, flags, to, tolen); /* W346: nur die korrigierte Zeitlinie sendet */
        }
        return e ? e->ret : len;
    }
}

static u32 __stdcall rb_ioctl(u32 s, u32 cmd, u32 argp)
{
    u32 ih;
    if (argp) LOC_RB_PREWRITE(argp, 4); /* W330 */
    if (!rb_mine()) return o_ioctl(s, cmd, argp);
    if (s_spec) { s_spec_ev.ioctl++; if (argp) *(u32 *)(uintptr_t)argp = 0; return 0; } /* W390: 0 Bytes bereit */
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
    if (s_spec) { u32 f = s_fake_err; s_spec_ev.wsa++; s_fake_err = 0; return f ? f : WSAEWOULDBLOCK_X; } /* W390 */
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

static long long s_lat_xin_q; /* W386: Zeitpunkt der letzten SBC-Abfrage des Spiel-Threads (live, QPC) */
static u32 s_lat_xin_n;        /* W386: SBC-Abfragen live */
static u32 __stdcall rb_xin(u32 h, u32 pState)
{
    if (pState) LOC_RB_PREWRITE(pState, 32); /* W330 */
    if (!rb_mine() || !pState) return o_xin(h, pState);
    if (s_spec) { /* W390: Vorlauf fragt das Geraet live ab (genau einmal je Handle); der folgende Live-Tick bekommt
                   * denselben Stand (keine doppelte Abfrage, kein verlorener Mausweg). Inhalt bleibt im Speicher. */
        u32 k, n = loc_pad_is_handle(h) ? 22u : RB_XIN_LEN, ret;
        for (k = 0; k < s_spec_xcn; k++) {
            if (s_spec_xc[k].h == h) {
                memcpy((void *)(uintptr_t)pState, s_spec_xc[k].d, s_spec_xc[k].n);
                s_spec_ev.xin_rep++;
                return s_spec_xc[k].ret;
            }
        }
        ret = o_xin(h, pState);
        s_spec_ev.xin_live++;
        if (s_spec_xcn < RB_SPEC_XC && n <= sizeof s_spec_xc[0].d) {
            s_spec_xc[s_spec_xcn].h = h;
            s_spec_xc[s_spec_xcn].ret = ret;
            s_spec_xc[s_spec_xcn].n = n;
            s_spec_xc[s_spec_xcn].used = 0;
            memcpy(s_spec_xc[s_spec_xcn].d, (const void *)(uintptr_t)pState, n);
            s_spec_xcn++;
        }
        return ret;
    }
    if (s_phase == PH_RECORD) {
        u32 ret, k;
        int cached = 0;
        if (!loc_pad_is_handle(h)) { /* W386: nur Zeitpunkt, keine Eingabe */
            s_lat_xin_q = rb_now();
            s_lat_xin_n++;
        }
        for (k = 0; k < s_spec_xcn; k++) { /* W390: im Vorlauf schon abgefragt -> denselben Stand verwenden */
            if (s_spec_xc[k].h == h && !s_spec_xc[k].used) {
                s_spec_xc[k].used = 1;
                memcpy((void *)(uintptr_t)pState, s_spec_xc[k].d, s_spec_xc[k].n);
                ret = s_spec_xc[k].ret;
                cached = 1;
                s_spec_xc_hit++;
                break;
            }
        }
        if (!cached) ret = o_xin(h, pState);
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
static u32 s_xc_dup, s_xc_new, s_xc_orphan; /* W346 R3: Ton beim Neurechnen doppelt / nachgeholt / verwaist (Zaehler) */
static u32 s_xc_orphan_stop; /* W375: davon als Schleife gestoppt */
static int s_xkey_legacy;    /* W381 DIAG loc_rb_opts xkey=0: Schluessel wie bis W380 (Gegenprobe #61) */
static u32 s_xc_stop_live;   /* W381: Stopps, die beim Neurechnen live liefen (nur die korrigierte Zeitlinie stoppt) */
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
    if (s_spec) { s_spec_ev.xgn++; return 0x80004005u; } /* W390: keine Meldung im Vorlauf */
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
/* W346 R3: Cue-Starts beim Neurechnen. Erste Rechnung (Aufzeichnung) und bereits beim Neurechnen live gestartete Cues
 * (Nebentabelle) gelten im ganzen Neurechen-Fenster: gleicher Cue (Parameter-Hash) = wiederverwenden (kein zweiter
 * Start, gleicher Handle). Nur was die korrigierte Zeitlinie zusaetzlich startet, startet live (nachgeholt). */
#define RB_XSIDE 256
typedef struct { u32 g, ih, ret, h, serial; uint8_t fn, used, live; } rb_xside_t; /* W413: serial = laufende Cue-Nummer */
static rb_xside_t s_xside[RB_XSIDE];
static u32 s_xside_n;
static u32 s_xwin0, s_xwin1, s_xwin_g0, s_xwin_g1; /* Fenster der laufenden Neurechnung (Aufzeichnung, Spielticks) */
static u32 rb_rbi_gtick(void);

/* ---- W413 LOC-SOUND-LOOP (#65): Ton nach dem Neurechnen mit der korrigierten Zeitlinie abgleichen (loc_rb_sndloop.h).
 * loc_sndloop_fix (Standard 1) stoppt, loc_sndloop_diag zaehlt/loggt auch ohne Fix (Gegenprobe: was liefe weiter). ---- */
#define RBS_NET 64
typedef struct { u32 serial, h, pp; uint16_t miss; uint8_t touched, origin; } rbs_net_t; /* origin 0 Aufzeichnung, 1 nachgeholt */
static rbs_net_t s_rbs_net[RBS_NET];
static u32 s_rbs_net_n, s_rbs_net_drop, s_rbs_net_stop, s_rbs_net_would, s_rbs_net_umiss;
static u32 s_rbs_rec_loop, s_rbs_rec_dead;          /* verwaist aus der Aufzeichnung: davon Schleife / als tot markiert */
static u32 s_rbs_side_orphan, s_rbs_side_loop, s_rbs_side_stop; /* nachgeholte Waisen (Nebentabelle) */
static u32 s_rbs_gap, s_rbs_gap_loop, s_rbs_gap_stop;  /* erledigter Index-/Quellen-Stopp: Instanz spielte noch */
static volatile LONG s_rbs_log_n;
static int rbs_act(void) { return g_cfg.loc_sndloop_fix != 0; }
static int rbs_on(void) { return g_cfg.loc_sndloop_fix || g_cfg.loc_sndloop_diag; }
#define RBS_LOG(...)                                                                                                 \
    do {                                                                                                             \
        if (g_cfg.loc_sndloop_diag && InterlockedIncrement(&s_rbs_log_n) <= 400) HLOG_I(__VA_ARGS__);                \
    } while (0)
static void rbs_net_add(u32 serial, u32 h, u32 pp, int touched, int origin)
{
    u32 i;
    if (!rbs_on() || g_cfg.loc_sndloop_grace <= 0 || !serial || !h || !pp) return;
    for (i = 0; i < s_rbs_net_n; i++)
        if (s_rbs_net[i].serial == serial) {
            s_rbs_net[i].touched |= (uint8_t)(touched != 0);
            return;
        }
    if (s_rbs_net_n >= RBS_NET) {
        s_rbs_net_drop++;
        return;
    }
    s_rbs_net[s_rbs_net_n].serial = serial;
    s_rbs_net[s_rbs_net_n].h = h;
    s_rbs_net[s_rbs_net_n].pp = pp;
    s_rbs_net[s_rbs_net_n].miss = 0;
    s_rbs_net[s_rbs_net_n].touched = (uint8_t)(touched != 0);
    s_rbs_net[s_rbs_net_n].origin = (uint8_t)origin;
    s_rbs_net_n++;
}
static void rbs_net_touch(u32 serial)
{
    u32 i;
    for (i = 0; i < s_rbs_net_n; i++)
        if (s_rbs_net[i].serial == serial) s_rbs_net[i].touched = 1;
}
static u32 rb_rd_u32(u32 p);
/* Sicherheitsnetz je Live-Tick (nach dem Neurechnen, Spiel-Thread): Schleife, die eine Neurechnung beruehrt hat und deren
 * Handle-Ablage im Spiel sie grace Ticks lang nicht mehr enthaelt, endet. Unberuehrte Schleifen werden nur gezaehlt. */
static void rbs_net_tick(void)
{
    u32 i, w = 0;
    for (i = 0; i < s_rbs_net_n; i++) {
        rbs_net_t *x = &s_rbs_net[i];
        int cue, loops, st;
        char nm[40];
        st = loc_audio_cue_info(x->serial, &cue, &loops, NULL, nm, (int)sizeof nm);
        if (st == 0) continue; /* beendet: Eintrag weg */
        if (st == 2 && loops) {
            u32 owner = rb_rd_u32(x->pp);
            if (!x->touched) {
                if (owner != x->h) s_rbs_net_umiss++; /* Gegenprobe: Ablage leer ohne Neurechnung (Netz greift dort nie) */
            } else if (lrs_net_step(owner, x->h, &x->miss, (u32)g_cfg.loc_sndloop_grace)) {
                if (rbs_act() && loc_audio_stop_serial(x->serial, 1)) {
                    s_rbs_net_stop++;
                    RBS_LOG("LOCSNDLOOP Netz: Cue %d '%s' (%s) ohne Besitzer seit %u Ticks -> gestoppt", cue, nm,
                            x->origin ? "nachgeholt" : "Aufzeichnung", (u32)x->miss);
                    continue;
                }
                if (!rbs_act() && x->miss == (uint16_t)g_cfg.loc_sndloop_grace) {
                    s_rbs_net_would++;
                    RBS_LOG("LOCSNDLOOP Netz: Cue %d '%s' (%s) ohne Besitzer seit %u Ticks -> spielt weiter (Fix aus)", cue,
                            nm, x->origin ? "nachgeholt" : "Aufzeichnung", (u32)x->miss);
                }
            }
        }
        s_rbs_net[w++] = *x;
    }
    s_rbs_net_n = w;
}
/* Erledigter Stopp nach Index (cue >= 0) bzw. StopSoundCues (cue < 0, Quelle src) am Neurechen-Tick: Instanzen, die die
 * korrigierte Zeitlinie zu diesem Zeitpunkt kennt (Nebentabelle, in dieser Neurechnung verwendete Aufzeichnung) und die
 * noch spielen, hat der damalige Live-Stopp nicht getroffen -> live stoppen. */
static void rbs_inst_cover(u32 serial, int32_t cue, u32 src, const char *from)
{
    int icue, loops, st;
    u32 isrc;
    char nm[40];
    if (!serial) return;
    st = loc_audio_cue_info(serial, &icue, &loops, &isrc, nm, (int)sizeof nm);
    if (st == 0 || !lrs_stop_covers(cue, src, icue, isrc)) return;
    s_rbs_gap++;
    if (loops) s_rbs_gap_loop++;
    if (rbs_act() && loc_audio_stop_serial(serial, 0)) s_rbs_gap_stop++;
    RBS_LOG("LOCSNDLOOP Stopp-Luecke: %s %d erledigt (erste Rechnung), Instanz Cue %d '%s' Schleife=%s (%s) spielte noch -> %s",
            cue >= 0 ? "Index" : "Quelle", cue >= 0 ? (int)cue : (int)(src & 0xFFFFu), icue, nm, loops ? "ja" : "nein",
            from, rbs_act() ? "gestoppt" : "spielt weiter (Fix aus)");
}
static void rbs_stop_cover(int32_t cue, u32 src)
{
    u32 c, i, t = rb_rbi_gtick();
    if (!rbs_on() || !s_rbi_ep) return;
    for (i = 0; i < s_xside_n; i++) {
        const rb_xside_t *x = &s_xside[i];
        if (lrs_side_in_timeline(x->g, x->used, s_xwin_g0, t)) rbs_inst_cover(x->serial, cue, src, "nachgeholt");
    }
    for (c = s_xwin0; c + sizeof(rb_ev_t) <= s_xwin1 && s_xwin1 <= s_rec_len;) {
        const rb_ev_t *e = (const rb_ev_t *)(s_rec + c);
        if ((e->fn == EV_XPLAY || e->fn == EV_XPREP) && (e->pad & LRS_PAD_USED) && e->n >= 8u)
            rbs_inst_cover(((const u32 *)(e + 1))[1], cue, src, "erste Rechnung, spaeter im Fenster");
        c += (u32)sizeof *e + e->n;
    }
}
static const rb_ev_t *rb_rep_xcue(int fn, u32 ih, const uint8_t **d, u32 *side_h, int *side_hit)
{
    u32 c, pass, cur0 = s_rec_cur;
    *side_hit = 0;
    for (pass = 0; pass < 2; pass++) { /* zuerst ab dem laufenden Tick, dann vom Fensterbeginn */
        u32 a = pass ? s_xwin0 : cur0, b = pass ? cur0 : s_xwin1;
        for (c = a; c + sizeof(rb_ev_t) <= b;) {
            rb_ev_t *e = (rb_ev_t *)(s_rec + c);
            if (e->fn == (uint8_t)fn && e->ih == ih && !(e->pad & (LRS_PAD_USED | LRS_PAD_DEAD))) { /* W413: tote nie */
                e->pad |= 1;
                s_cnt_ev[fn][1]++;
                *d = (const uint8_t *)(e + 1);
                return e;
            }
            c += (u32)sizeof *e + e->n;
        }
    }
    for (c = 0; c < s_xside_n; c++) {
        rb_xside_t *x = &s_xside[c];
        if (x->fn == (uint8_t)fn && x->ih == ih && !x->used && x->g >= s_xwin_g0 && x->g < s_xwin_g1) {
            x->used = 1;
            *side_h = x->h;
            *side_hit = 1;
            return NULL;
        }
    }
    return NULL;
}
static int rb_xcue_seen(int fn, u32 ih) /* gleicher Cue im Fenster schon verwendet (Zaehler doppelt) */
{
    u32 c;
    for (c = s_xwin0; c + sizeof(rb_ev_t) <= s_xwin1;) {
        const rb_ev_t *e = (const rb_ev_t *)(s_rec + c);
        if (e->fn == (uint8_t)fn && e->ih == ih) return 1;
        c += (u32)sizeof *e + e->n;
    }
    return 0;
}
static void rb_xside_add(int fn, u32 ih, u32 ret, u32 h, u32 serial)
{
    u32 g = rb_rbi_gtick(), i, w = 0;
    for (i = 0; i < s_xside_n; i++) /* alte Eintraege verwerfen */
        if (s_xside[i].g + RBI_GH / 2u > g) s_xside[w++] = s_xside[i];
    s_xside_n = w;
    if (s_xside_n >= RB_XSIDE) return;
    s_xside[s_xside_n].g = g;
    s_xside[s_xside_n].ih = ih;
    s_xside[s_xside_n].ret = ret;
    s_xside[s_xside_n].h = h;
    s_xside[s_xside_n].serial = serial;
    s_xside[s_xside_n].fn = (uint8_t)fn;
    s_xside[s_xside_n].used = 1;
    s_xside[s_xside_n].live = 1;
    s_xside_n++;
}
static u32 rb_xcue_call(int fn, fn3_t o, u32 sb, u32 params, u32 pp)
{
    u32 ih;
    if (s_spec) { s_spec_ev.xcue++; return 0; } /* W390: kein Ton aus dem Vorlauf (S_OK wie stumm, Handle unberuehrt) */
    ih = fnv(2166136261u, (const void *)(uintptr_t)params, params ? 12u : 0u) * 31u + (pp != 0);
    if (s_rbi_ep && s_phase == PH_REPLAY) {
        const uint8_t *d = NULL;
        u32 sh = 0, ret, h;
        int side = 0;
        const rb_ev_t *ev = rb_rep_xcue(fn, ih, &d, &sh, &side);
        if (ev) {
            if (ev->n && pp) { LOC_RB_PREWRITE(pp, 4); memcpy((void *)(uintptr_t)pp, d, 4); }
            return ev->ret;
        }
        if (side) {
            if (pp) { LOC_RB_PREWRITE(pp, 4); *(u32 *)(uintptr_t)pp = sh; }
            return 0;
        }
        if (rb_xcue_seen(fn, ih)) s_xc_dup++; /* korrigierte Zeitlinie startet denselben Cue oefter als die erste */
        s_xc_new++;
        if (pp) LOC_RB_PREWRITE(pp, 4);
        {
            u32 s0 = loc_audio_last_serial(), ser;
            ret = o(sb, params, pp); /* nachgeholt: nur die korrigierte Zeitlinie startet ihn */
            h = rb_rd_u32(pp);
            ser = loc_audio_last_serial(); /* W413: laufende Nummer (Waise der naechsten Neurechnung, Netz) */
            if (ser == s0) ser = 0;
            rb_xside_add(fn, ih, ret, h, ser);
            if (ser && rbs_on()) {
                int lp = 0, st = loc_audio_cue_info(ser, NULL, &lp, NULL, NULL, 0);
                if (fn == EV_XPREP || (st == 2 && lp)) rbs_net_add(ser, h, pp, 1, 1);
            }
        }
        return ret;
    }
    if (s_phase == PH_RECORD) {
        /* W375 (#51): neben dem Handle die laufende Cue-Nummer mitschreiben (0 = kein neuer Cue), damit eine Schleife,
         * die nur diese Zeitlinie startet, nach einem Rollback gezielt gestoppt werden kann (rbi_replay_finish). */
        u32 s0 = loc_audio_last_serial(), hs[2], ret = o(sb, params, pp);
        hs[0] = rb_rd_u32(pp);
        hs[1] = loc_audio_last_serial();
        if (hs[1] == s0) hs[1] = 0;
        rb_rec_put(fn, ret, ih, hs, 8u);
        if (s_rbi_ep && hs[1] && rbs_on()) { /* W413: Schleifen fuer das Sicherheitsnetz merken (beruehrt erst im Fenster) */
            int lp = 0, st = loc_audio_cue_info(hs[1], NULL, &lp, NULL, NULL, 0);
            if (fn == EV_XPREP || (st == 2 && lp)) rbs_net_add(hs[1], hs[0], pp, 0, 0);
        }
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
    if (s_spec) { s_spec_ev.xstop++; return 0; } /* W390 */
    u32 key = loc_rb_xkey_stop(cue, flags, pcue, s_xkey_legacy); /* W381 (#61): Handle im Schluessel */
    if (s_phase == PH_RECORD) {
        u32 ret = o_xstop(sb, cue, flags, pcue);
        rb_rec_put(EV_XSTOP, ret, key, NULL, 0);
        return ret;
    } else {
        const rb_ev_t *ev = rb_rep_get(EV_XSTOP, key, NULL);
        if (!ev && s_rbi_ep) { s_xc_stop_live++; return o_xstop(sb, cue, flags, pcue); } /* W346: nur die korrigierte Zeitlinie stoppt */
        if (ev && s_rbi_ep && (int)cue >= 0) rbs_stop_cover((int32_t)cue, 0); /* W413: Index-Stopp erledigt - Luecken */
        return ev ? ev->ret : 0;
    }
}
static u32 __stdcall rb_xstat(u32 sp, u32 pst)
{
    if (!rb_mine()) return o_xstat(sp, pst);
    if (s_spec) return o_xstat(sp, pst); /* W390: nur lesend */
    if (s_phase == PH_RECORD) {
        u32 ret = o_xstat(sp, pst), v = rb_rd_u32(pst);
        rb_rec_put(EV_XSTAT, ret, loc_rb_xkey_src(sp, s_xkey_legacy), &v, pst ? 4u : 0u); /* W381: Quelle im Schluessel */
        return ret;
    } else {
        const uint8_t *d;
        const rb_ev_t *ev = rb_rep_get(EV_XSTAT, loc_rb_xkey_src(sp, s_xkey_legacy), &d);
        if (!ev) return o_xstat(sp, pst);
        if (ev->n && pst) { LOC_RB_PREWRITE(pst, 4); memcpy((void *)(uintptr_t)pst, d, 4); }
        return ev->ret;
    }
}
static u32 __stdcall rb_xstopsrc(u32 sp)
{
    if (!rb_mine()) return o_xstopsrc(sp);
    if (s_spec) { s_spec_ev.xstop++; return 0; } /* W390 */
    if (s_phase == PH_RECORD) {
        u32 ret = o_xstopsrc(sp);
        rb_rec_put(EV_XSTOPSRC, ret, loc_rb_xkey_src(sp, s_xkey_legacy), NULL, 0); /* W381: Quelle im Schluessel */
        return ret;
    } else {
        const rb_ev_t *ev = rb_rep_get(EV_XSTOPSRC, loc_rb_xkey_src(sp, s_xkey_legacy), NULL);
        if (!ev && s_rbi_ep) { s_xc_stop_live++; return o_xstopsrc(sp); } /* W346 */
        if (ev && s_rbi_ep && sp) rbs_stop_cover(-1, sp); /* W413: StopSoundCues erledigt - Luecken */
        return ev ? ev->ret : 0;
    }
}
static u32 __stdcall rb_xwork(void)
{
    if (!rb_mine()) return o_xwork();
    if (s_spec) return 0; /* W390: kein Ton-Fortschritt im Vorlauf */
    if (s_phase == PH_RECORD) return o_xwork(); /* nicht aufzeichnen: auch aus dem Feld-Warten (entfaellt beim Neurechnen) */
    s_cnt_ev[EV_XWORK][1]++;                     /* beim Neurechnen zurueckgehalten (kein Ton-Fortschritt) */
    return 0;
}

/* Akkumulator (Zustandssumme je Lockstep-Rahmen) vor dem Lesen+Loeschen abgreifen. Im Neurechnen
 * direkt zur Spielfunktion (loc_sync zaehlt dann nicht mit: seine Rahmen/Hashes bleiben die der
 * Zeitlinie, die der Mitspieler sieht). */
static int rb_r2_acc(void); /* W338 */
static int s_rbi_acc_chained;  /* W338: Akkumulator-Kette schon bei der Installation */
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
    if (s_spec && GetCurrentThreadId() == s_game_tid) { /* W390: Vorlauf - kein Hash, kein loc_sync (direkt zur Spielfunktion) */
        s_spec_ev.acc++;
        s_acc_tgt = s_acc_game;
        return;
    }
    if (rb_r2_acc()) { /* W338: vor der Thread-Pruefung (Spiel-Thread ist vor dem Ringstart noch unbekannt) */ /* W338: Akkumulator je Slot in die Eingabeschicht; im Neurechnen loc_sync auslassen */
        if (s_phase == PH_REPLAY) s_acc_tgt = s_acc_game;
        return;
    }
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

/* W375 LOC-ROUND2: Das Spiel startet den Render-Thread (0x2C580) je Mission neu. s_rthread blieb der Thread aus
 * Mission 1; in Mission 2 scheiterte das Anhalten (beendeter Thread) bei jedem Tick (Ruhe-Fehler 20/s, Ring sicherte
 * nichts, kein Rollback, Stau nach ~36 s auf beiden Seiten, Laeufe R11/W366 E3h). Hier den lebenden Render-Thread
 * (der zuletzt gestartete gewinnt) neu bestimmen. */
static u32 s_rthread_new;
static int s_rthread_scan_n; /* angelegte Gast-Threads beim letzten Scan */
static void rb_rthread_refresh(const char *why)
{
    int i, n = krnl_xb_thread_count();
    HANDLE best = NULL;
    s_rthread_scan_n = krnl_xb_thread_created();
    for (i = 0; i < n; i++) {
        void *h = NULL;
        unsigned r = 0;
        if (!krnl_xb_thread_get(i, &h, &r) || r != RB_RTHREAD || !h) continue;
        if (WaitForSingleObject((HANDLE)h, 0) == WAIT_TIMEOUT) best = (HANDLE)h; /* lebt */
    }
    if (best && best != s_rthread) {
        s_rthread_new++;
        HLOG_I("LOCRB: Render-Thread neu bestimmt (%s): tid %lu -> %lu", why, s_rthread ? GetThreadId(s_rthread) : 0ul,
               GetThreadId(best));
        s_rthread = best;
    }
}
static int rb_rthread_dead(void)
{
    return s_rthread && WaitForSingleObject(s_rthread, 0) != WAIT_TIMEOUT;
}
/* W375: neu scannen, wenn der alte Render-Thread beendet ist oder das Spiel seit dem letzten Scan Threads angelegt hat
 * (Mission 3: der Thread aus Mission 2 lebte noch, der neue entstand erst nach dem Missionsbeginn - Lauf R13). */
static void rb_rthread_check(void)
{
    if (krnl_xb_thread_created() != s_rthread_scan_n) rb_rthread_refresh("neue Gast-Threads");
    else if (rb_rthread_dead()) rb_rthread_refresh("alter beendet");
}

/* ================= W411 LOC-FREEZE-SUSP: Anhalten des Render-Threads mit Buchfuehrung =================
 * Befund (Nutzer-Log 21.11., 3 Spieler, R2): Spiel-Thread kreist in FUN_0002F100 @0x2F355 ("warte bis Render-Auftrag
 * [0x306EEC] == 0, dann Auftrag 2 = Ende") - die Missionsschleife ist verlassen (Ende-Zaehler [0x3068E0] hat die Grenze
 * [0x306C10] erreicht). Der Render-Thread (0x2C580) steht mit SuspendCount 1: angehalten vom Vorlauf (W390,
 * rb_spec_try_start, Auftrag 1) bzw. Neurechnen (rb_quiesce). Fortgesetzt wird nur im naechsten rb_after - den gibt es
 * nach dem Verlassen der Schleife nicht mehr -> ewiges Kreisen. Gleiches Bild in W409 T1 (Rauswurf: Ende mit Grenze 0).
 * Ab hier: genau ein gebuchtes Anhalten (kein doppeltes, Fortsetzen nur, wenn wir halten), Ende-Pruefung vor dem
 * Verlassen der Schleife (rb_loop_exits), Haken am Schleifenende (rb_loop_exit) und ein Waechter-Thread. */
static volatile LONG s_rs_held;   /* 1 = wir halten genau ein Anhalten des Render-Threads */
static HANDLE s_rs_h;             /* angehaltener Thread (gesetzt vor s_rs_held) */
static volatile LONG s_rs_t0;     /* GetTickCount beim Anhalten */
static volatile LONG s_rs_why;    /* 1 Ruhepunkt (Neurechnen/Zyklus), 2 Vorlauf */
static volatile LONG s_after_seq; /* je rb_after + 1 (Waechter: Spiel-Thread erreicht den Ruhepunkt) */
static u32 s_rs_n, s_rs_double, s_rs_swap, s_rs_spur, s_rs_prev0, s_rs_max_ms, s_rs_rescue, s_lx_n, s_lx_spec,
    s_lx_rep, s_lx_held, s_spec_endstop, s_rep_endstop, s_spec_abandon_n;
static int s_endguard = 1;        /* loc_rb_opts endguard=0: alte Wirkung (TEST, Nachweis vorher) */
static int s_exithook = 1;        /* loc_rb_opts exithook=0: kein Haken am Schleifenende */
static int s_sguard_ms = 2000;    /* loc_rb_opts sguard=N: Waechter (ms), 0 = aus */
static int s_test_end = -1, s_test_end_spec, s_test_end_done; /* TEST tend=N, tendspec=1 */
static int s_rep_ended;           /* Missionsende im Neurechnen schon behandelt (rbi_mission_end nicht erneut) */
static const char *rb_rs_why_name(LONG w) { return w == 2 ? "Vorlauf" : (w == 1 ? "Ruhepunkt/Neurechnen" : "-"); }
static void rb_rs_resume(void)
{
    DWORD prev, ms;
    if (!InterlockedExchange(&s_rs_held, 0)) { /* nichts gebucht (Waechter oder Haken war schneller): nie blind fortsetzen */
        s_rs_spur++;
        return;
    }
    ms = GetTickCount() - (DWORD)s_rs_t0;
    if (ms > s_rs_max_ms) s_rs_max_ms = ms;
    prev = ResumeThread(s_rs_h);
    if (prev == 0) s_rs_prev0++; /* war nicht angehalten (fremdes Fortsetzen) */
}
/* 1 = angehalten (genau einmal gebucht). Haelt schon ein Anhalten fuer denselben Thread: nicht doppelt anhalten. */
static int rb_rs_suspend(HANDLE h, int why)
{
    if (s_rs_held) {
        s_rs_double++;
        if (s_rs_h == h) return 1;
        s_rs_swap++;
        rb_rs_resume(); /* Render-Thread gewechselt: alten zuerst freigeben */
    }
    if (!rb_suspend_safe(h)) return 0;
    s_rs_h = h;
    InterlockedExchange(&s_rs_why, why);
    InterlockedExchange(&s_rs_t0, (LONG)GetTickCount());
    InterlockedExchange(&s_rs_held, 1);
    s_rs_n++;
    return 1;
}
/* Ende-Pruefung von FUN_0002F100 direkt nach dem Feld-Warten (0x2F2FE..0x2F34C), exakt nachgebildet: Zaehler != 0 ->
 * Zaehler + 1, < 0 -> 0, > Grenze -> Grenze; gleich Grenze -> Schleife wird verlassen. rb_after laeuft im Thunk am CALL
 * @0x2F2F9, danach folgt ohne weiteren Spielcode genau diese Pruefung. */
#define RB_END_CNT 0x3068E0u
#define RB_END_LIM 0x306C10u
static int rb_loop_exits(void)
{
    int c = (int)rd32(RB_END_CNT), l = (int)rd32(RB_END_LIM);
    if (!c) return 0;
    c++;
    if (c < 0) c = 0;
    else if (c > l) c = l;
    return c == l;
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
    if (suspend) rb_rthread_check(); /* W375: neue Mission, neuer Render-Thread */
    if (suspend && s_rthread) {
        long long t1 = rb_now();
        if (!rb_rs_suspend(s_rthread, 1)) { /* W330: haelt dauernd eine Sperre -> Zyklus verwerfen (W411: gebucht) */
            s_q_timeout++;
            return 0;
        }
        rb_put(s_q_susp, rb_us(rb_now() - t1));
        if (rd32(RB_RJOB) == 1u) { /* doch noch einen Auftrag gesehen */
            rb_rs_resume(); /* W411 */
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
    rb_rs_resume(); /* W411: nur, wenn gebucht */
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
                rb_rs_resume(); /* W411: nur, wenn gebucht */
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

/* ================= W338 LOC-ROLLBACK-R2: Eingabeschicht (loc_rb_input=1) =================
 * Hooks: FUN_0004E920 (Entnahme eines Rahmens) -> rbi_take, FUN_0004EC80 (eigener 0x34-Rahmen: TCP an die
 * Mitspieler + eigener Ring) -> rbi_own + eigener UDP-Kanal. Die Regelung in FUN_0001D010 (0x1D0BF..0x1D381:
 * Verzoegerung, Zusatzrahmen, Schritte je Bild) entfaellt ([0x306944] = 1). Am Ruhepunkt: Commit nur in Ticks mit
 * Entnahme, Empfang, Rollback (rbr_load + getreue Neurechnung ohne Feld-Warten, Aufrufe des Spiel-Threads aus der
 * laufenden Aufzeichnung), Senden, endgueltige Hashes (loc_sync), Zeitabgleich. Das Spiel-Netz (TCP 1000) traegt
 * weiter Lobby, Start und Ende. Keine Eingaben im Log. */
#define RBI_E920 0x4E920u
#define RBI_EC80 0x4EC80u
#define RBI_REG 0x1D0BFu
#define RBI_STEPS 0x306944u
#define RBI_DELAYV 0x306F04u
#define RBI_PREFILL 0x2C9B2Cu
#define RBI_SELF 0x3071B0u
#define RBI_NPL 0x3071A0u
#define RBI_TAKENW 0x309474u
#define RBI_SENTW 0x309368u
#define RBI_CHAN_GP 1260u /* Spielport-Abbild des Kanals (Basis + 260); Spiel nutzt 1000 und 1201-1203, 1200 = Doppelstart-Schutz */
#define RBI_GM (RBI_GH - 1u)
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

static int s_rbi, s_rbi_last_pre, s_rbi_delay = 2, s_rbi_maxpred = 10, s_rbi_tsync = 2;
static int s_rbi_pred = 1, s_rbi_predq = 8; /* W385: Vorhersage fremder Achsen (0 Wiederholung W338, 1 Trend; Standard 1 linear nach Messreihe K3/K4: Korrekturen -82 %), Daempfung q/8 */
/* W386 LOC-LATENCY: niedrige Verzoegerung (Regler loc_rbn.c rbl_*, Paket SBRL), Fenster 10 s auf dem Spiel-Thread. */
static rbl_cfg_t s_rbl_cfg;
static rbl_t *s_rbl;
static void rbl_rx_src(const uint8_t *b, int len, uint64_t src);
static int s_rbl_test; /* TEST ltest=N: Host wechselt alle N s zwischen d_std und d_low (Wechsel-Mechanik pruefen) */
static u32 s_ls_cause, s_ls_ticks;          /* W385: Ursache/Tiefe des laufenden Rollbacks (Messung) */
/* W372: early = 1: eigenen Rahmen sofort senden (im Hook FUN_0004EC80 statt am naechsten Ruhepunkt, ~1 Tick frueher);
 * tsync = 2: Zeitabgleich ueber die Feldzahl des Feld-Wartens (rbi_tsync, SBRT), 1 = W338-Stau-Tick, 0 = aus. */
static int s_rbi_early = 1, s_rbi_sent_early;
static volatile LONG s_wait_extra; /* W372: zusaetzliche Felder fuer das naechste Feld-Warten (Thunk) */
static u32 s_rbi_tt_tick, s_rbi_wait_ext_n, s_rbi_wait_ext_f;
/* W372: FUN_00033050(N) zaehlt Felder seit dem zuletzt gesehenen Feld [0x306F8C] (gedeckelt auf N). Der Ring laedt
 * beim Rollback auch diesen Wert (alter Feldstand), die Neurechen-Ticks warten nicht -> der erste Live-Tick danach
 * sah ">= N Felder vergangen" und wartete gar nicht: jede Seite lief je Rollback einen Tick vor (W338-Zeitabgleich
 * bremste das mit je einem Stau-Tick). Fix: Live-Wert vor dem Laden merken, nach dem Neurechnen zuruecksetzen. */
#define RBI_FIELD_LAST 0x306F8Cu
static int s_rbi_fldfix = 1;
static u32 s_rbi_fld_live, s_rbi_fld_n, s_rbi_fld_diff;
static int s_rbi_rec = 1; /* DIAG rec=0: Spiel-Aufrufe nicht aufzeichnen (dann kein Rollback moeglich) */
/* TEST chanloss=N (Prozent, unabhaengig) bzw. chanloss=ge (Gilbert-Elliott wie Profil mobile-bad: 1/60 -> schlecht,
 * 1/4 -> gut, im schlechten Zustand 80 % Verlust): nur ausgehende Kanalpakete, fuer Messungen mit uloss:0 in der
 * Verzoegerungsstufe (Lobby-Suche bleibt deterministisch). */
static int s_rbi_chanloss, s_rbi_chanloss_ge, s_rbi_ge_bad;
static u32 s_rbi_chanloss_rng = 0x9E3779B9u, s_rbi_chanloss_n;
static int rbi_chan_drop(void)
{
    u32 r;
    if (!s_rbi_chanloss && !s_rbi_chanloss_ge) return 0;
    s_rbi_chanloss_rng ^= s_rbi_chanloss_rng << 13;
    s_rbi_chanloss_rng ^= s_rbi_chanloss_rng >> 17;
    s_rbi_chanloss_rng ^= s_rbi_chanloss_rng << 5;
    r = s_rbi_chanloss_rng % 100000u;
    if (s_rbi_chanloss_ge) {
        if (!s_rbi_ge_bad && r < 1667u) s_rbi_ge_bad = 1;
        else if (s_rbi_ge_bad && r < 25000u) s_rbi_ge_bad = 0;
        s_rbi_chanloss_rng = s_rbi_chanloss_rng * 1103515245u + 12345u;
        r = s_rbi_chanloss_rng % 100000u;
        if (s_rbi_ge_bad && r < 80000u) { s_rbi_chanloss_n++; return 1; }
        return 0;
    }
    if (r < (u32)s_rbi_chanloss * 1000u) { s_rbi_chanloss_n++; return 1; }
    return 0;
}
static rbi_t *s_rbc;
static u32 s_rbi_epoch;
static uint8_t s_rbi_out[RBI_MAXP][RBI_FR];
static uint8_t *s_rbi_tramp;
typedef u32(__cdecl *rbi_fn2_t)(u32, u32);
static rbi_fn2_t o_e920, o_ec80;
static u32 s_rbi_reg_d = 2; /* Anzeige der Verzoegerung in [0x306F04] (LOCSYNC Verz=) */
static SOCKET s_rbi_sock = INVALID_SOCKET;
static int s_rbi_chan;            /* 1 = eigener UDP-Socket (Original-Transport), 2 = ueber loc_tun (SBT2) */
/* Kanal ueber loc_tun: Empfang im Transport-Thread -> Warteschlange -> Spiel-Thread (rbi_poll) */
#define RBI_TQ 64
static CRITICAL_SECTION s_rbi_tq_cs;
static struct { uint64_t src; int len; u32 t; uint8_t b[RBI_PKT_MAX]; } s_rbi_tq[RBI_TQ]; /* W372: t = Ankunft (us) */
static int s_rbi_tq_r, s_rbi_tq_w;
static volatile LONG s_rbi_tq_drop;
static u32 s_rbi_toff[RBI_GH];
static long long s_rbi_t_rep0;
static rb_ser_t *s_rbi_s_ticks, *s_rbi_s_slots, *s_rbi_s_load, *s_rbi_s_rep;
static u32 s_rbi_rb_n, s_rbi_rb_last, s_rbi_lost_logged, s_rbi_qfail, s_rbi_ts_waits, s_rbi_ts_ms, s_rbi_bar,
    s_rbi_peer_bad, s_rbi_tx_err, s_rbi_compact, s_rbi_hash_logged, s_rbi_load_fail, s_rbi_missions;
static u32 s_rbi_win_sum, s_rbi_win_n; /* Rollbacks/s im Fenster */
/* W372 LOC-RB-TIMING: Zeitleiste je Live-Tick (ms nach dem Feldwechsel = Ende des Feld-Wartens): eigener Rahmen,
 * erste Fremd-Entnahme, Senden; Puffer-Histogramm im Modul (rbi_clock). Nur Zeiten und Zaehler. */
#define RBI_TH 128
static long long s_tim_base;
static u32 s_tim_t0, s_tim_own_seen, s_tim_take_seen, s_tim_own_t, s_tim_own_pend;
static u32 s_tim_h_own[RBI_TH], s_tim_h_take[RBI_TH], s_tim_h_send[RBI_TH], s_tim_own_first, s_tim_ticks;
/* W386: Abtastung -> Senden: SBC-Abfrage nach Feldwechsel, SBC-Abfrage -> eigener Rahmen, Abfragen je Tick (live) */
static u32 s_lat_h_xin[RBI_TH], s_lat_h_x2o[RBI_TH], s_lat_xin_tick0, s_lat_multi, s_lat_ticks, s_lat_none, s_lat_before;
static u32 s_tim_slh[RBI_SLH], s_tim_sln, s_tim_sll; /* Stand beim letzten Log (Fenster = Differenz) */
static double s_tim_adv_min, s_tim_adv_max, s_tim_adv_sum;
static u32 s_tim_adv_n;
static u32 rbi_us_now(void) { return rb_us(rb_now() - s_tim_base) | 1u; }
static void tim_h(u32 *h, u32 t, u32 since)
{
    u32 ms = (t - since) / 1000u;
    h[ms >= RBI_TH ? RBI_TH - 1 : ms]++;
}
static int tim_pct(const u32 *h, int pct)
{
    u32 n = 0, acc = 0, want;
    int b;
    for (b = 0; b < RBI_TH; b++) n += h[b];
    if (!n) return -1;
    want = (u32)(((uint64_t)n * (u32)pct + 99u) / 100u);
    if (want < 1) want = 1;
    for (b = 0; b < RBI_TH; b++) {
        acc += h[b];
        if (acc >= want) return b;
    }
    return RBI_TH - 1;
}
/* W386: eigener Rahmen (live, erster im Tick): wann las das Spiel die SBC-Eingabe dazu? */
static void lat_note_own(void)
{
    u32 tx;
    if (!s_lat_xin_q) return;
    tx = rb_us(s_lat_xin_q - s_tim_base) | 1u;
    if ((int32_t)(tx - s_tim_t0) >= 0) {
        tim_h(s_lat_h_xin, tx, s_tim_t0);
    } else {
        s_lat_before++; /* Abfrage im vorigen Tick (vor dem Feldwechsel) */
    }
    if ((int32_t)(s_tim_own_t - tx) >= 0) tim_h(s_lat_h_x2o, s_tim_own_t, tx);
}
static int s_rbi_defer_on = 1;       /* W346 R3 defer=0: Neuaufbau sofort (W338-Verhalten) */
static u32 s_rbi_defer_ticks, s_rbi_defer_n, s_rbi_defer_clean;
static u32 s_rbi_dflags[2], s_rbi_dtick_own, s_rbi_dmulti; /* DIAG */
static u32 s_rbi_ts_run, s_rbi_ts_cool; /* Zeitabgleich */
static int s_rbi_ts_stall, s_rbi_ts_in;  /* Stau-Tick angefordert / laeuft */
static u32 s_rbi_tick_takes;            /* Entnahmen im laufenden Tick */
static u32 s_rbi_dstream[2], s_rbi_dn[2], s_rbi_dchk[2][10], s_rbi_dcnt[10];
/* W346 R4: Aushandlung je Mission (loc_rbn), Kanal-Thread (Empfang Kanal 1 + Hallo 5 Hz), Rueckfall, Desync-Ende */
static rbn_t *s_rbn;
static CRITICAL_SECTION s_rbn_cs;
static HANDLE s_rbi_thr;
static volatile LONG s_rbi_thr_stop;
static volatile int s_rbi_mst;          /* RBN_ST_* der laufenden Mission (IDLE = keine) */
static volatile LONG s_rbi_self_hello = 0xFF; /* Spielerindex fuer das Hallo (0xFF = unbekannt) */
static u32 s_rbi_ls_missions, s_rbi_r2_missions, s_rbi_fb_n, s_rbi_end_desync, s_rbi_end_disagree, s_rbi_last_why;
static int s_rbi_hcmp_logged; /* W355 */
static unsigned s_rbi_reg_tick; /* W355: Registerstand beim letzten Ruhepunkt */
/* W359 rprep=1 (Standard): Beim Neurechnen laeuft die Render-Vorbereitung des Spiel-Threads jeden Tick wie live
 * (0x2F256: Freigabe-Warteschlange 0x84AA0 leeren, Doppelpuffer 0xBBB00/0x32880/0xBBBB0 kopieren, Index 0x306A6C
 * tauschen, Bildzaehler 0x306A74); nur das Zeichnen im Render-Thread entfaellt. rprep=0: altes Verhalten (Auftrag = 1,
 * Vorbereitung uebersprungen - Heap- und Puffer-Verlauf weicht vom Original ab). */
static int s_rbi_prep = 1;
/* W359 heapchk (Absicherung, Beschreibung bei rbi_heapchk) */
static int s_rbi_heapchk = 1;
static int s_rbi_heap_bad;      /* 1 = beschaedigt erkannt (Ende im naechsten Live-Tick) */
static u32 s_rbi_hc_tick, s_rbi_end_heap;
static u32 s_rbi_hc_n, s_rbi_hc_nodes_max, s_rbi_hc_us_max, s_rbi_hc_bad, s_rbi_hc_part, s_rbi_hc_vq, s_rbi_hc_busy;
static int s_rbi_rbtest_n, s_rbi_rbtest_k; /* W355 TEST rbtest=N,K: alle N Live-Ticks Rollback um K Ticks erzwingen */
static u32 s_rbi_rbtest_cnt, s_rbi_rbtest_ok, s_rbi_rbtest_next;
static int s_rbi_force_desync = -1;     /* TEST desync=N: eigener Hash von Slot N verfaelscht (Gegenprobe Desync-Ende) */
static int s_rbi_dmin = 2, s_rbi_dmax = 3; /* W346: Grenzen der adaptiven Verzoegerung (D=1 nur fest) */
static long long s_rbi_lt_last;         /* DIAG langer Tick: Zeit des letzten Ruhepunkts */
static u32 s_rbi_lt_n, s_rbi_lt_max, s_rbi_lt_logged;
static const char *const k_rbn_why[RBN_WHY_N] = {"-", "eigener Schalter aus", "Mitspieler ohne Rollback",
                                                 "kein Rollback-Host", "Mitspieler meldet Lockstep", "Frist abgelaufen",
                                                 "Host entschied Lockstep"};
/* W370: Kurzform fuer F11-Paket/Status (englisch, stabil) und Sprachschluessel der Einblendung */
static const char *const k_rbn_why_slug[RBN_WHY_N] = {"none", "self_off", "peer_no_rollback", "no_rollback_host",
                                                      "peer_lockstep", "timeout", "host_lockstep"};
static const char *const k_rbn_why_key[RBN_WHY_N] = {"ovl.rb_why_none", "ovl.rb_why_self_off", "ovl.rb_why_peer_no_rollback",
                                                     "ovl.rb_why_no_rollback_host", "ovl.rb_why_peer_lockstep",
                                                     "ovl.rb_why_timeout", "ovl.rb_why_host_lockstep"};
static u32 s_rbi_last_host_why; /* W370: Client: Grund des Hosts (0 = unbekannt) */
/* W374 LOC-NET-OVERLAY: letzte Rollback-Tiefen mit Zeitstempel (Ring, je Rollback ein Speicher) und Vorsprung des
 * letzten Live-Ticks - nur fuer das F3-Overlay (loc_rb_hud_read, eigener Thread, liest nur). */
#define RBI_HUD_N 128
static volatile u32 s_rbi_hud_d[RBI_HUD_N], s_rbi_hud_t[RBI_HUD_N];
static volatile LONG s_rbi_hud_n;
static volatile double s_rbi_hud_adv;
static int s_rbi_npeers = -1;    /* W370: Mitspieler der laufenden Mission (Verbindungstabelle bzw. Plaetze - 1) */

/* W370 LOC-RB-FALLBACK-US: verbundene Mitspieler aus der Verbindungstabelle des Spiels (C = *(0x3116AC), 10 Eintraege
 * zu 16 Byte, Byte 0 Bit 0 belegt, LOC_BOOT §35). RBI_NPL ist der hoechste Platz + 1: nach einem Wiederbeitritt bleibt
 * ein Platz leer (erster Menschentest: Gast auf Platz 2 von 3, Host erwartete 2 Mitspieler -> Lockstep).
 * Rueckgabe Anzahl (Plaetze 1..9), -1 = Tabelle nicht lesbar; *mask = belegte Plaetze. */
static int rbi_conn_peers(u32 *mask)
{
    int k, c = 0;
    u32 m = 0;
    __try {
        u32 tab = *(volatile const u32 *)(uintptr_t)0x3116ACu;
        if (!tab) return -1;
        for (k = 1; k < 10; k++)
            if (*(volatile const uint8_t *)(uintptr_t)(tab + (u32)k * 0x10u) & 1u) {
                m |= 1u << k;
                c++;
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    if (mask) *mask = m;
    return c;
}

static void rbi_pct(rb_ser_t *s, u32 *p50, u32 *p95, u32 *mx)
{
    static u32 tmp[RB_SER_N];
    u32 n = s ? s->n : 0;
    *p50 = *p95 = *mx = 0;
    if (!n) return;
    memcpy(tmp, s->v, n * sizeof(u32));
    qsort(tmp, n, sizeof(u32), rb_cmp);
    *p50 = tmp[n / 2];
    *p95 = tmp[(n * 95u) / 100u];
    *mx = tmp[n - 1];
}

static int rbi_peer_ok(const locnet *n, u32 ip, uint16_t port)
{
    int i;
    if (ip == n->self_ip || !locnet_ip_allowed(ip)) return 0;
    for (i = 0; i < n->npeers; i++)
        if (n->peers[i].ip == ip && locnet_port_out(n, ip, RBI_CHAN_GP) == port) return 1;
    return 0;
}

static void rbi_poll(void)
{
    locnet *n = loc_net_core();
    uint8_t b[RBI_PKT_MAX + 16];
    int k;
    if (!s_rbi_chan || !n || !s_rbc) return;
    if (s_rbi_chan == 1) { /* W346: in der Mission liest der Spiel-Thread den Socket selbst (wie W338): der Kanal-Thread
                            * teilt sich den Kern des Prozesses und kaeme erst nach einer Zeitscheibe dran (M_fern2:
                            * 0,17 Rollbacks/s statt 0,01, 10 s Zeitabgleich-Stau). Hallo -> Aushandlung. */
        for (k = 0; k < 64; k++) {
            struct sockaddr_in f;
            int fl = (int)sizeof f, r;
            int len = recvfrom(s_rbi_sock, (char *)b, (int)sizeof b, 0, (struct sockaddr *)&f, &fl);
            uint64_t src;
            if (len < 0) {
                if (WSAGetLastError() == WSAECONNRESET) continue;
                break;
            }
            if (fl < (int)sizeof f || f.sin_family != AF_INET || !rbi_peer_ok(n, f.sin_addr.s_addr, ntohs(f.sin_port))) {
                s_rbi_peer_bad++;
                continue;
            }
            src = ((uint64_t)f.sin_addr.s_addr << 16) | ntohs(f.sin_port);
            if (rbn_is_hello(b, len)) {
                EnterCriticalSection(&s_rbn_cs);
                (void)rbn_rx(s_rbn, GetTickCount(), b, len, src);
                LeaveCriticalSection(&s_rbn_cs);
                continue;
            }
            rbi_clock(s_rbc, rbi_us_now()); /* W372: Ankunft = jetzt (Spiel-Thread liest selbst) */
            if (rbl_is_pkt(b, len)) { /* W386: Bericht/Entscheidung niedrige Verzoegerung */
                rbl_rx_src(b, len, src);
                continue;
            }
            if (rbi_is_tt(b, len)) { /* W372: Puffer-Meldung des Mitspielers */
                (void)rbi_tt_rx(s_rbc, b, len, src);
                continue;
            }
            r = rbi_rx(s_rbc, b, len, src);
            if (r == 1 && s_rbi_mst == RBN_ST_TENT) {
                EnterCriticalSection(&s_rbn_cs);
                rbn_confirm_peer(s_rbn, src);
                LeaveCriticalSection(&s_rbn_cs);
            }
        }
    }
    /* Warteschlange: Kanal 2 (Transport-Thread) bzw. Reste, die der Kanal-Thread vor dem Missionsbeginn empfing */
    for (k = 0; k < RBI_TQ; k++) {
        uint64_t src;
        int len, r;
        EnterCriticalSection(&s_rbi_tq_cs);
        if (s_rbi_tq_r == s_rbi_tq_w) {
            LeaveCriticalSection(&s_rbi_tq_cs);
            break;
        }
        src = s_rbi_tq[s_rbi_tq_r].src;
        len = s_rbi_tq[s_rbi_tq_r].len;
        memcpy(b, s_rbi_tq[s_rbi_tq_r].b, (size_t)len);
        rbi_clock(s_rbc, s_rbi_tq[s_rbi_tq_r].t); /* W372: Ankunft im Empfangs-Thread */
        s_rbi_tq_r = (s_rbi_tq_r + 1) % RBI_TQ;
        LeaveCriticalSection(&s_rbi_tq_cs);
        if (rbl_is_pkt(b, len)) { /* W386 */
            rbl_rx_src(b, len, src);
            continue;
        }
        if (rbi_is_tt(b, len)) { /* W372 */
            (void)rbi_tt_rx(s_rbc, b, len, src);
            continue;
        }
        r = rbi_rx(s_rbc, b, len, src);
        if (r == 1 && s_rbi_mst == RBN_ST_TENT) { /* Host: SBR2 mit dieser Epoche = Mitspieler hat R2 angenommen */
            EnterCriticalSection(&s_rbn_cs);
            rbn_confirm_peer(s_rbn, src);
            LeaveCriticalSection(&s_rbn_cs);
        }
    }
}

/* W346: Empfangsweg beider Kanaele (Thread des Kanals bzw. des Transports): Hallo -> Aushandlung (vollstaendig
 * geprueft in rbn_rx), sonst Warteschlange fuer den Spiel-Thread (Pruefung in rbi_rx). */
static void rbi_dispatch(uint64_t src, const uint8_t *d, int len)
{
    if (len <= 0 || len > RBI_PKT_MAX) {
        InterlockedIncrement(&s_rbi_tq_drop);
        return;
    }
    if (rbn_is_hello(d, len)) {
        EnterCriticalSection(&s_rbn_cs);
        (void)rbn_rx(s_rbn, GetTickCount(), d, len, src);
        LeaveCriticalSection(&s_rbn_cs);
        return;
    }
    EnterCriticalSection(&s_rbi_tq_cs);
    if ((s_rbi_tq_w + 1) % RBI_TQ == s_rbi_tq_r) {
        InterlockedIncrement(&s_rbi_tq_drop);
    } else {
        s_rbi_tq[s_rbi_tq_w].src = src;
        s_rbi_tq[s_rbi_tq_w].len = len;
        s_rbi_tq[s_rbi_tq_w].t = rbi_us_now();
        memcpy(s_rbi_tq[s_rbi_tq_w].b, d, (size_t)len);
        s_rbi_tq_w = (s_rbi_tq_w + 1) % RBI_TQ;
    }
    LeaveCriticalSection(&s_rbi_tq_cs);
}

/* An alle Mitspieler senden (Kanal 1: eigener Socket ueber die Verzoegerungsstufe, Kanal 2: SBT2). Thread-sicher. */
static void rbi_send_all(const uint8_t *b, int len)
{
    locnet *n = loc_net_core();
    int i;
    if (!n || len <= 0) return;
    if (s_rbi_chan == 2) {
        if (loctun_dgram(n->tun, INADDR_BROADCAST, RBI_CHAN_GP, RBI_CHAN_GP, (const char *)b, len) < 0) s_rbi_tx_err++;
        return;
    }
    for (i = 0; i < n->npeers; i++) {
        struct sockaddr_in to;
        int r;
        if (n->peers[i].ip == n->self_ip) continue;
        memset(&to, 0, sizeof to);
        to.sin_family = AF_INET;
        to.sin_addr.s_addr = n->peers[i].ip;
        to.sin_port = htons(locnet_port_out(n, n->peers[i].ip, RBI_CHAN_GP));
        if (n->sim) r = locnetsim_send(n->sim, s_rbi_sock, 0, (const char *)b, len, 0, &to); /* W322-Pruefstand */
        else r = sendto(s_rbi_sock, (const char *)b, len, 0, (const struct sockaddr *)&to, (int)sizeof to);
        if (r < 0) s_rbi_tx_err++;
    }
}

/* W346: Kanal-Thread: Empfang Kanal 1 (select 20 ms) und Hallo alle 200 ms (beide Kanaele, auch in der Lobby). */
static DWORD WINAPI rbi_chan_thread(LPVOID unused)
{
    DWORD next = GetTickCount();
    (void)unused;
    while (!s_rbi_thr_stop) {
        if (s_rbi_chan == 1 && (s_rbi_mst == RBN_ST_IDLE || s_rbi_mst == RBN_ST_LS)) { /* in der Mission: Spiel-Thread */
            fd_set rs;
            struct timeval tv = {0, 20000};
            FD_ZERO(&rs);
            FD_SET(s_rbi_sock, &rs);
            if (select(0, &rs, NULL, NULL, &tv) > 0) {
                locnet *n = loc_net_core();
                int k;
                for (k = 0; k < 64 && n; k++) {
                    uint8_t b[RBI_PKT_MAX + 16];
                    struct sockaddr_in f;
                    int fl = (int)sizeof f;
                    int len = recvfrom(s_rbi_sock, (char *)b, (int)sizeof b, 0, (struct sockaddr *)&f, &fl);
                    if (len < 0) {
                        if (WSAGetLastError() == WSAECONNRESET) continue;
                        break;
                    }
                    if (fl < (int)sizeof f || f.sin_family != AF_INET ||
                        !rbi_peer_ok(n, f.sin_addr.s_addr, ntohs(f.sin_port))) {
                        s_rbi_peer_bad++;
                        continue;
                    }
                    rbi_dispatch(((uint64_t)f.sin_addr.s_addr << 16) | ntohs(f.sin_port), b, len);
                }
            }
        } else {
            Sleep(20);
        }
        {
            DWORD now = GetTickCount();
            if ((LONG)(now - next) >= 0) {
                uint8_t hb[RBN_PKT_MAX];
                int hl;
                next = now + 200;
                EnterCriticalSection(&s_rbn_cs);
                hl = rbn_tx(s_rbn, now, (int)s_rbi_self_hello, hb, (int)sizeof hb);
                LeaveCriticalSection(&s_rbn_cs);
                if (hl > 0) rbi_send_all(hb, hl);
            }
        }
    }
    return 0;
}

static void rbi_send(void)
{
    locnet *n = loc_net_core();
    uint8_t b[RBI_PKT_MAX];
    int len, i;
    if (!s_rbi_chan || !n) return;
    len = rbi_tx(s_rbc, b, (int)sizeof b);
    if (len <= 0) return;
    if (s_phase != PH_REPLAY && s_tim_own_pend) { /* W372: Abstand eigener Rahmen -> Senden */
        s_tim_own_pend = 0;
        tim_h(s_tim_h_send, rbi_us_now(), s_tim_own_t);
    }
    if (rbi_chan_drop()) return; /* TEST chanloss */
    (void)i;
    rbi_send_all(b, len);
    if (s_rbi_tsync == 2 && s_phase != PH_REPLAY && !(++s_rbi_tt_tick & 1u)) { /* W372: Puffer melden (10/s) */
        len = rbi_tt_tx(s_rbc, b, (int)sizeof b);
        if (len > 0) rbi_send_all(b, len);
    }
}

static void rbi_tun_rx(uint32_t vip, const uint8_t *d, int len) /* Transport-Thread */
{
    rbi_dispatch((uint64_t)vip << 16, d, len);
}

static int rbi_sock_open(void)
{
    locnet *n = loc_net_core();
    struct sockaddr_in a;
    u_long nb = 1;
    BOOL off = FALSE, excl = TRUE;
    DWORD ret = 0;
    SOCKET s;
    if (!n) {
        HLOG_W("LOCRBI: Netz aus - Eingabeschicht aus (Original-Lockstep)");
        return 0;
    }
    if (n->tun) { /* eigener Transport (W324): Kanal als SBT2-Datagramm an Spielport 1260, nie an den Gast */
        if (!loctun_available()) {
            HLOG_W("LOCRBI: Transport nicht verfuegbar - Eingabeschicht aus (Original-Lockstep)");
            return 0;
        }
        loctun_set_app(n->tun, (uint16_t)RBI_CHAN_GP, rbi_tun_rx);
        s_rbi_chan = 2;
        HLOG_I("LOCRBI: Eingabekanal ueber den eigenen Transport (SBT2-Datagramm, Spielport %u, nicht an das Spiel)",
               RBI_CHAN_GP);
        return 1;
    }
    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return 0;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&excl, (int)sizeof excl);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = n->self_ip;
    a.sin_port = htons(locnet_port_out(n, n->self_ip, RBI_CHAN_GP));
    if (bind(s, (const struct sockaddr *)&a, (int)sizeof a) != 0) {
        HLOG_W("LOCRBI: Kanal-Port %u nicht frei (WSA %d) - Eingabeschicht aus", ntohs(a.sin_port), WSAGetLastError());
        closesocket(s);
        return 0;
    }
    ioctlsocket(s, FIONBIO, &nb);
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, (DWORD)sizeof off, NULL, 0, &ret, NULL, NULL);
    s_rbi_sock = s;
    s_rbi_chan = 1;
    HLOG_I("LOCRBI: Eingabekanal UDP Port %u (Basis+260), nur bekannte Mitspieler (%d)", ntohs(a.sin_port), n->npeers);
    return 1;
}

/* ================= W386 LOC-LATENCY: niedrige Verzoegerung (Regler rbl_* in loc_rbn.c) =================
 * Spiel-Thread: alle 10 s ein Fenster (Korrekturen, Schaetzer D-1, Tiefe, Neurechnen p95, Haenger, Sprung) -> Bericht im
 * Paket SBRL (jede Sekunde wiederholt, mit der letzten Entscheidung des Hosts); der Host wertet aus und entscheidet
 * fuer alle. Wechsel nur ueber rbi_set_delay (eigene Rahmen ab einem angekuendigten Slot), nie im Neurechnen. Wechselt
 * D, beginnt das Fenster neu (Berichte mischen nie zwei Verzoegerungen). Nur Zaehler im Log, keine Eingaben. */
#define RBL_RPW 256
static u32 s_rbl_rp[RBL_RPW], s_rbl_rp_n;  /* Neurechnen (us) im Fenster */
static u32 s_rbl_wdep;                     /* groesste Rollback-Tiefe im Fenster (Ticks) */
static u32 s_rbl_t0, s_rbl_tsend, s_rbl_win, s_rbl_rb0, s_rbl_hang0, s_rbl_ttest, s_rbl_tmis;
static rbi_stats_t s_rbl_st0;
static rbl_rep_t s_rbl_rep;
static int s_rbl_have;
static u32 s_rbl_bind_bad, s_rbl_switch_n, s_rbl_dticks[9];

static void rbl_note_replay(u32 us)
{
    s_rbl_rp[s_rbl_rp_n % RBL_RPW] = us;
    s_rbl_rp_n++;
}
static int rbl_cmp_u32(const void *a, const void *b)
{
    u32 x = *(const u32 *)a, y = *(const u32 *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}
static int rbl_ready(void)
{
    return s_rbi_ep && s_ring && s_ready && !s_failed && rbr_active() && s_rbi_rec && rbr_depth() >= 2u &&
           !rbi_free_running(s_rbc) && s_phase == PH_RECORD;
}
static void rbl_win_reset(u32 now)
{
    s_rbl_t0 = now;
    s_rbl_rb0 = s_rbi_rb_n;
    s_rbl_st0 = *rbi_stats(s_rbc);
    s_rbl_hang0 = loc_sync_hangs();
    s_rbl_wdep = 0;
    s_rbl_rp_n = 0;
    (void)loc_smooth_window_jump();
}
static void rbl_rx_src(const uint8_t *b, int len, uint64_t src)
{
    int pidx = rbl_peek_pidx(b, len);
    if (!s_rbl) return;
    if (pidx >= 0 && rbi_peer_src(s_rbc, pidx) != src) { /* nur vom gebundenen Absender dieses Spielers (SBR2) */
        s_rbl_bind_bad++;
        return;
    }
    (void)rbl_rx(s_rbl, GetTickCount(), b, len);
}
static const char *const k_rbl_why[RBL_WHY_N] = {"-", "gut", "Korrekturen", "Tiefe", "Neurechnen", "Haenger", "Sprung",
                                                 "ohne Ring/Bericht"};

static void rbl_window(u32 now)
{
    const rbi_stats_t *st = rbi_stats(s_rbc);
    u32 dt = now - s_rbl_t0, late, need, acc = 0, n, k;
    rbl_rep_t r;
    int rtt;
    if (dt < 1000u) dt = 1000u;
    memset(&r, 0, sizeof r);
    r.ready = rbl_ready();
    r.dcur = rbi_delay_eff(s_rbc) >= 1 ? rbi_delay_eff(s_rbc) : (int)s_rbi_reg_d;
    s_rbl_win = s_rbl_win >= 0xFFFFu ? 1u : s_rbl_win + 1u;
    r.win = s_rbl_win;
    r.cpm = (u32)((uint64_t)(s_rbi_rb_n - s_rbl_rb0) * 60000u / dt);
    r.est_cpm = (u32)((uint64_t)(st->est_wrong - s_rbl_st0.est_wrong) * 60000u / dt);
    late = st->est_late - s_rbl_st0.est_late;
    if (late) {
        need = (late * 95u + 99u) / 100u;
        for (k = 0; k < 4u; k++) {
            acc += st->est_dep[k] - s_rbl_st0.est_dep[k];
            if (acc >= need) break;
        }
        r.est_dep = k + 1u > 4u ? 4u : k + 1u;
    }
    r.dep = s_rbl_wdep;
    n = s_rbl_rp_n < RBL_RPW ? s_rbl_rp_n : RBL_RPW;
    if (n) {
        u32 tmp[RBL_RPW];
        memcpy(tmp, s_rbl_rp, n * sizeof tmp[0]);
        qsort(tmp, n, sizeof tmp[0], rbl_cmp_u32);
        if (n >= 2u) n--; /* der teuerste je Fenster zaehlt nicht (Ring-Neuaufbau am Ende eines Neurechnens, ~35 ms einmalig,
                           * N100); eine langsame CPU hat viele teure Neurechnungen */
        r.rp = (tmp[(n * 95u + 99u) / 100u - 1u] + 99u) / 100u;
        if (r.rp > 255u) r.rp = 255u;
    }
    r.hang = loc_sync_hangs() - s_rbl_hang0;
    {
        double j = loc_smooth_window_jump();
        r.jump = j > 65535.0 ? 65535u : (unsigned)(j + 0.5);
    }
    s_rbl_rep = r;
    s_rbl_have = 1;
    rbl_win_reset(now);
    EnterCriticalSection(&s_rbn_cs);
    rtt = rbn_rtt_max(s_rbn, now);
    LeaveCriticalSection(&s_rbn_cs);
    if (rbi_self(s_rbc) == 0 && s_rbl_cfg.on && !s_rbl_test &&
        rbl_host_step(s_rbl, now, &r, rbi_own_next_slot(s_rbc), rtt)) {
        const rbl_stats_t *ls = rbl_stats(s_rbl);
        s_rbl_tsend = now - 1000u; /* Entscheidung sofort senden (danach jede Sekunde wiederholt) */
        HLOG_I("LOCRBI-LOWD: Host entscheidet D=%d (Grund %s, RTT %d ms, runter=%u hoch=%u)", rbl_target(s_rbl),
               ls->last_why < RBL_WHY_N ? k_rbl_why[ls->last_why] : "?", rtt, ls->down, ls->up);
    }
    {
        const rbl_stats_t *ls = rbl_stats(s_rbl);
        HLOG_I("LOCRBI-LOWD: Fenster %u D=%d Ziel=%d (Start %d, niedrig %d) bereit=%d | Korrekturen/min=%u geschaetzt bei "
               "D-1=%u Tiefe max=%u geschaetzt=%u Neurechnen p95=%.1f ms Haenger=%u Sprung=%u | ausgewertet=%u "
               "uebersprungen=%u runter=%u hoch=%u Grund=%s | SBRL tx=%u rx=%u schlecht=%u Epoche=%u Bindung=%u",
               r.win, r.dcur, rbl_target(s_rbl), rbl_dstd(s_rbl), rbl_dlow(s_rbl), r.ready, r.cpm, r.est_cpm, r.dep,
               r.est_dep, r.rp / 10.0, r.hang, r.jump, ls->evals, ls->skipped, ls->down, ls->up,
               ls->last_why < RBL_WHY_N ? k_rbl_why[ls->last_why] : "?", ls->tx, ls->rx_ok, ls->rx_bad, ls->rx_epoch,
               s_rbl_bind_bad);
    }
}

/* Ruhepunkt (live, mit und ohne Ring): Entscheidung anwenden, Fenster, Bericht senden. */
static void rbl_tick(void)
{
    u32 now;
    int d, dc;
    u32 sl;
    if (!s_rbl || !rbl_active(s_rbl) || !s_rbi_ep || rbi_replaying(s_rbc)) return;
    now = GetTickCount();
    if (rbl_take_decision(s_rbl, &d, &sl)) {
        int ok = rbi_set_delay(s_rbc, d, sl);
        HLOG_I("LOCRBI-LOWD: Wechsel auf D=%d ab Slot %u %s (eigener naechster Slot %u)", d, sl,
               ok ? "vorgemerkt" : "nicht noetig", rbi_own_next_slot(s_rbc));
    }
    dc = rbi_delay_eff(s_rbc);
    if (dc >= 1 && dc <= 8) {
        s_rbl_dticks[dc]++;
        if ((u32)dc != s_rbi_reg_d) {
            const rbi_stats_t *st = rbi_stats(s_rbc);
            HLOG_I("LOCRBI-LOWD: Verzoegerung jetzt %d Ticks (vorher %u) bei Slot %u, Wechsel runter=%u hoch=%u spaet=%u",
                   dc, s_rbi_reg_d, rbi_own_next_slot(s_rbc), st->dchg_down, st->dchg_up, st->dchg_late);
            s_rbi_reg_d = (u32)dc;
            s_rbl_switch_n++;
            rbl_win_reset(now); /* Fenster mischen nie zwei Verzoegerungen */
        }
    }
    if (s_rbl_test > 0 && rbi_self(s_rbc) == 0 && now - s_rbl_ttest >= (u32)s_rbl_test * 1000u && rbl_ready()) {
        int rtt, to = rbl_target(s_rbl) == rbl_dstd(s_rbl) ? rbl_dlow(s_rbl) : rbl_dstd(s_rbl);
        s_rbl_ttest = now;
        EnterCriticalSection(&s_rbn_cs);
        rtt = rbn_rtt_max(s_rbn, now);
        LeaveCriticalSection(&s_rbn_cs);
        if (rbl_force(s_rbl, to, rbi_own_next_slot(s_rbc), rtt)) {
            s_rbl_tsend = now - 1000u;
            HLOG_I("LOCRBI-LOWD: TEST erzwungener Wechsel auf D=%d", to);
        }
    }
    if (now - s_rbl_t0 >= RBL_WIN_MS) rbl_window(now);
    if (s_rbl_have && s_rbl_cfg.on && now - s_rbl_tsend >= 1000u) { /* Bericht + Entscheidung jede Sekunde */
        uint8_t b[RBL_LEN];
        int len;
        s_rbl_tsend = now;
        len = rbl_tx(s_rbl, &s_rbl_rep, b, (int)sizeof b);
        if (len > 0) rbi_send_all(b, len);
    }
}

/* Missionsbeginn mit R2 (Host und Client): Regler zuruecksetzen. */
static void rbl_mission_begin(u32 epoch, int delay, int rtt)
{
    int dl;
    u32 now = GetTickCount();
    if (!s_rbl) return;
    dl = (s_rbl_cfg.on && s_rbi_delay <= 0) ? rbn_pick_low(rtt, delay, s_rbl_cfg.lowmax) : delay;
    rbl_begin(s_rbl, &s_rbl_cfg, epoch, rbi_self(s_rbc), s_rbi_npeers, delay, dl);
    memset(s_rbl_dticks, 0, sizeof s_rbl_dticks);
    s_rbl_have = 0;
    s_rbl_win = 0;
    s_rbl_tsend = now;
    s_rbl_ttest = now;
    s_rbl_tmis = now;
    s_rbl_switch_n = 0;
    rbl_win_reset(now);
    if (s_rbl_cfg.on)
        HLOG_I("LOCRBI-LOWD: Mission: niedrige Verzoegerung %s (Start D=%d, niedrig D=%d bis %d ms, RTT %d ms)%s",
               dl < delay ? "moeglich" : "nicht moeglich", delay, dl, s_rbl_cfg.lowmax, rtt,
               s_rbl_test ? " - TEST Wechsel erzwungen" : "");
}

static void rbl_mission_end(void)
{
    const rbi_stats_t *st;
    u32 tot = 0, k;
    if (!s_rbl || !rbl_active(s_rbl)) return;
    st = rbi_stats(s_rbc);
    for (k = 1; k <= 8u; k++) tot += s_rbl_dticks[k];
    if (tot)
        HLOG_I("LOCRBI-LOWD: Mission Ende: Ticks je D 1/2/3/4=%u/%u/%u/%u (%.0f/%.0f/%.0f %%), Wechsel %u (runter=%u hoch=%u "
               "spaet=%u), Schaetzer spaet=%u falsch=%u von %u",
               s_rbl_dticks[1], s_rbl_dticks[2], s_rbl_dticks[3], s_rbl_dticks[4], 100.0 * s_rbl_dticks[1] / tot,
               100.0 * s_rbl_dticks[2] / tot, 100.0 * s_rbl_dticks[3] / tot, s_rbl_switch_n, st->dchg_down, st->dchg_up,
               st->dchg_late, st->est_late, st->est_wrong, st->est_n);
    rbl_end(s_rbl);
}

static void rbi_left_reset(void); /* W366 */
/* W346: R2 fuer diese Mission ausgehandelt (Epoche/Verzoegerung stehen fest, noch keine Entnahme). */
static void rbi_go_r2(u32 epoch, int delay)
{
    s_spec_off_mission = s_spec_paused_n = 0; /* W390: jede Mission beginnt mit Vorlauf an */
    s_spec_level = 0;
    s_spec_pause_until = 0;
    s_spec_win_n = 0;
    int rtt;
    if (!rbi_configure(s_rbc, epoch, delay)) { /* darf nicht vorkommen (Entnahme vor der Entscheidung) */
        HLOG_W("LOCRBI: Aushandlung zu spaet (Entnahme schon erfolgt) - Mission im Original-Lockstep");
        rbi_end(s_rbc);
        s_rbi_mst = RBN_ST_LS;
        s_rbi_fb_n++;
        return;
    }
    EnterCriticalSection(&s_rbn_cs);
    rtt = rbn_rtt_max(s_rbn, GetTickCount());
    LeaveCriticalSection(&s_rbn_cs);
    s_rbi_epoch = epoch;
    s_rbi_r2_missions++;
    s_rbi_reg_d = (u32)delay;
    memset(s_rbi_toff, 0, sizeof s_rbi_toff);
    s_rec_len = s_rec_cur = 0; /* Aufzeichnung je Mission neu (Tick 0 ab Offset 0) */
    if (s_ready && s_rec && s_rbi_rec) s_phase = PH_RECORD;
    s_rbi_mst = RBN_ST_R2;
    rbi_left_reset(); /* W366 */
    s_rbi_ep = 1;
    loc_sync_ext_on(1);
    HLOG_I("LOCRBI: Mission %u mit eigener Eingabeschicht (ausgehandelt): Spieler %u von %u, Epoche %08X, Verzoegerung %d "
           "Ticks (%s, Umlaufzeit %d ms), Fenster %d Slots, Zeitabgleich %d", s_rbi_missions, rd32(RBI_SELF), rd32(RBI_NPL),
           epoch, delay, s_rbi_delay > 0 ? "fest" : "adaptiv", rtt, s_rbi_maxpred, s_rbi_tsync);
    rbl_mission_begin(epoch, delay, rtt); /* W386 */
}

static void rbi_fallback_ls(int why)
{
    char pd[400];
    int hw;
    s_rbi_mst = RBN_ST_LS;
    s_rbi_ls_missions++;
    s_rbi_last_why = (u32)why;
    EnterCriticalSection(&s_rbn_cs);
    hw = rbn_host_why(s_rbn);
    rbn_peers_desc(s_rbn, GetTickCount(), pd, (int)sizeof pd);
    LeaveCriticalSection(&s_rbn_cs);
    s_rbi_last_host_why = why == RBN_WHY_HOST_LS ? (u32)hw : 0u;
    if (why != RBN_WHY_SELF_OFF) s_rbi_fb_n++;
    HLOG_I("LOCRBI: Mission %u im Original-Lockstep (%s)", s_rbi_missions,
           why > 0 && why < RBN_WHY_N ? k_rbn_why[why] : "?");
    /* W370: Einzelheiten beidseitig (nur Zahlen) */
    HLOG_I("LOCRBI: Rueckfall-Grund %s%s%s, Mitspieler erwartet %d: %s", why > 0 && why < RBN_WHY_N ? k_rbn_why_slug[why] : "?",
           s_rbi_last_host_why ? ", Grund des Hosts " : "",
           s_rbi_last_host_why && s_rbi_last_host_why < RBN_WHY_N ? k_rbn_why_slug[s_rbi_last_host_why] : "", s_rbi_npeers, pd);
    if (why != RBN_WHY_SELF_OFF) { /* W370: Hinweis im Spiel (de/en): Rollback aus, hoehere Eingabeverzoegerung */
        wchar_t t1[160], fmt[240], t2[240], rs[120];
        int rk = (s_rbi_last_host_why > 0 && s_rbi_last_host_why < RBN_WHY_N) ? (int)s_rbi_last_host_why : why;
        sb_lang_w("loc.rb_off_t", t1, 160);
        sb_lang_w(k_rbn_why_key[rk > 0 && rk < RBN_WHY_N ? rk : 0], rs, 120);
        sb_lang_w("loc.rb_off_d", fmt, 240);
        if (wcsstr(fmt, L"%s")) _snwprintf_s(t2, 240, _TRUNCATE, fmt, rs);
        else wcsncpy_s(t2, 240, fmt, _TRUNCATE);
        sb_ov_toast_msg(t1, t2, 8000u);
    }
}

/* W346: Missionsbeginn (Vorlauf FUN_0001D3D0): Aushandlung starten. Vor der Entscheidung entnimmt keine Seite. */
static void rbi_mission_open(void)
{
    int st, self, npl, d, why;
    u32 ep;
    if (!s_rbi || !s_rbi_chan || !s_rbc || !s_rbn) return;
    self = (int)rd32(RBI_SELF);
    npl = (int)rd32(RBI_NPL);
    InterlockedExchange(&s_rbi_self_hello, (self >= 0 && self < 10) ? self : 0xFF);
    { /* W370: Host zaehlt die verbundenen Mitspieler (nicht hoechster Platz) */
        u32 mask = 0;
        int cp = self == 0 ? rbi_conn_peers(&mask) : -1;
        s_rbi_npeers = cp > 0 ? cp : npl - 1;
        if (self == 0)
            HLOG_I("LOCRBI: Mission %u: Plaetze %d (NPL), verbunden 0x%03X -> %d Mitspieler erwartet%s", s_rbi_missions + 1,
                   npl, mask, s_rbi_npeers, cp > 0 ? "" : " (Tabelle leer/unlesbar, Plaetze - 1)");
    }
    EnterCriticalSection(&s_rbn_cs);
    st = rbn_mission_start(s_rbn, GetTickCount(), self, s_rbi_npeers);
    ep = rbn_epoch(s_rbn);
    d = rbn_delay(s_rbn);
    why = rbn_why(s_rbn);
    LeaveCriticalSection(&s_rbn_cs);
    s_rbi_missions++;
    if (s_rbi_missions > 1) rb_rthread_refresh("Missionsbeginn"); /* W375: Render-Thread je Mission neu */
    if (st == RBN_ST_LS) {
        rbi_fallback_ls(why);
        return;
    }
    rbi_begin(s_rbc, ep, self, d > 0 ? d : 2, s_rbi_maxpred); /* eigene Rahmen ab dem Vorlauf mitschreiben */
    loc_smooth_reset(); /* W385 */
    s_rbi_hcmp_logged = 0;
    s_rbi_rbtest_next = 0;
    rb_pcom_clear();
    s_rbi_heap_bad = 0; /* W359 */
    s_rbi_hc_tick = 0;
    s_rbi_reg_tick = g_exreg_n;
    s_rbi_mst = st;
    {
        char pd[400];
        int hw;
        EnterCriticalSection(&s_rbn_cs);
        hw = rbn_host_waiting(s_rbn);
        rbn_peers_desc(s_rbn, GetTickCount(), pd, (int)sizeof pd);
        LeaveCriticalSection(&s_rbn_cs);
        HLOG_I("LOCRBI: Mission %u: Aushandlung (%s), Spieler %d von %d", s_rbi_missions,
               st == RBN_ST_TENT ? (hw ? "Host wartet auf das Hallo eines Mitspielers (Nachfrist)"
                                       : "Host bietet Rollback an, wartet auf Bestaetigung")
                                 : "Client wartet auf die Entscheidung des Hosts",
               self, npl);
        HLOG_I("LOCRBI: Mission %u: Mitspieler %s", s_rbi_missions, pd); /* W370 */
    }
}

/* W346: Aushandlung am Ruhepunkt weiterfuehren (TENT/PEND). */
static void rbi_mission_poll(void)
{
    int st, d, why;
    u32 ep;
    rbi_poll();
    EnterCriticalSection(&s_rbn_cs);
    st = rbn_poll(s_rbn, GetTickCount());
    ep = rbn_epoch(s_rbn);
    d = rbn_delay(s_rbn);
    why = rbn_why(s_rbn);
    LeaveCriticalSection(&s_rbn_cs);
    if (st == RBN_ST_R2) {
        rbi_go_r2(ep, d);
    } else if (st == RBN_ST_LS) {
        rbi_end(s_rbc);
        rbi_fallback_ls(why);
    }
}

/* W346 R3: Allokation des Spiel-Threads waehrend R2 (krnl_mem). Aufzeichnen: Ergebnis je Aufruf. Neurechnen: derselbe
 * Block wie in der ersten Rechnung (genullt wie frischer Speicher) statt eines neuen (kein Leck, gleiche Adresse). */
static u32 s_rbi_alloc_rec, s_rbi_alloc_rep, s_rbi_alloc_new;
static u32 s_rbi_alloc_other, s_rbi_alloc_miss, s_rbi_alloc_nocommit; /* W355 DIAG */
static u32 s_rbi_com_rec, s_rbi_com_rep, s_rbi_com_miss, s_rbi_com_new; /* W355: Commit in reservierten Bereich */
static u32 s_rbi_com_logged;
/* W355: Commits (base != 0) seit dem letzten Ring-Neuaufbau, egal in welcher Phase (die erste Belegung kann aus der
 * korrigierten Zeitlinie kommen, also im Neurechnen). Diese Seiten liegen nicht im Ring; jeder Rollback im Aufschub
 * zielt hoechstens auf den fruehesten dieser Ticks (rbi_rebuild_note) - nach dem Laden sind sie in der neuen Zeitlinie
 * also noch nicht belegt ("verworfen", live = 0), behalten aber den Inhalt der alten. Der erste Commit eines verworfenen
 * Bereichs (live oder beim Neurechnen) wird wie frisch bedient: genullt, kein Registereintrag. Ein erneuter Commit in
 * derselben Zeitlinie (live = 1) aendert nichts (wie im Original: Commit belegter Seiten laesst den Inhalt). */
#define RB_PCOM_MAX 64
static struct { u32 a, end, live; } s_pcom[RB_PCOM_MAX];
static u32 s_pcom_n, s_pcom_full, s_rbi_com_same, s_rbi_com_drop;
static void rb_pcom_clear(void) { s_pcom_n = 0; }
static u32 s_rbi_pcom_drop_log; /* W366 */
static void rb_pcom_rollback(void) /* nach rbr_load: alle Eintraege gehoeren zur verworfenen Zeitlinie */
{
    u32 k;
    for (k = 0; k < s_pcom_n; k++) {
        if (s_pcom[k].live) s_rbi_com_drop++;
        s_pcom[k].live = 0;
    }
}
static int s_memlog;   /* W359 DIAG memlog=1: Commit/Freigabe/Rollback in R2 protokollieren (nur Adressen/Ticks) */
static u32 s_memlog_n, s_memlog_lt;
/* W390: Belegungen des Vorlaufs. Ein Block, den der Vorlauf belegt hat, gehoert nach dem Zuruecksetzen niemandem; die
 * echte Zeitlinie bekommt ihn bei derselben Belegung (gleiche Groesse/Art) genullt wieder (kein Leck, wie beim
 * Neurechnen). Commits in Reservierungen bekommen einen verworfenen Eintrag (der echte Commit nullt die Seiten). */
#define RB_SPEC_AC 16
static struct { u32 ih, a, size, gt; } s_spec_ac[RB_SPEC_AC];
static u32 s_spec_ac_n, s_spec_ac_hit, s_spec_ac_drop;
#define RB_SPEC_CM 16
static struct { u32 a, end; } s_spec_cm[RB_SPEC_CM];
static u32 s_spec_cm_n, s_spec_cm_full;
static int rb_alloc_cb(int op, unsigned size, unsigned type, unsigned prot, unsigned *io)
{
    u32 ih = size * 31u + type * 7u + prot;
    if (s_spec && GetCurrentThreadId() == s_game_tid) { /* W390: spekulativer Vorlauf */
        if (op == 1 && *io) { /* belegt: fuer die echte Zeitlinie vormerken */
            s_spec_ev.alloc++;
            if (s_spec_ac_n < RB_SPEC_AC) {
                s_spec_ac[s_spec_ac_n].ih = ih;
                s_spec_ac[s_spec_ac_n].a = *io;
                s_spec_ac[s_spec_ac_n].size = size;
                s_spec_ac[s_spec_ac_n].gt = rd32(RB_TICK);
                s_spec_ac_n++;
            } else {
                s_spec_ac_drop++;
            }
        } else if (op == 3) { /* Commit in Reservierung */
            u32 a3 = *io & ~0xFFFu, e3 = (a3 + size + 0xFFFu) & ~0xFFFu, k;
            s_spec_ev.commit++;
            for (k = 0; k < s_pcom_n; k++)
                if (a3 >= s_pcom[k].a && e3 <= s_pcom[k].end) break;
            if (k == s_pcom_n) {
                if (s_spec_cm_n < RB_SPEC_CM) {
                    s_spec_cm[s_spec_cm_n].a = a3;
                    s_spec_cm[s_spec_cm_n].end = e3;
                    s_spec_cm_n++;
                } else {
                    s_spec_cm_full++;
                }
            }
        }
        return 0; /* belegen/committen wie das System (Inhalt ist nach dem Zuruecksetzen ohne Besitzer) */
    }
    if (op == 0 && s_spec_ac_n && s_rbi_ep && s_phase == PH_RECORD && rb_mine()) { /* W390: Block aus dem Vorlauf */
        u32 k;
        for (k = 0; k < s_spec_ac_n; k++) {
            if (s_spec_ac[k].ih != ih) continue;
            __try {
                LOC_RB_PREWRITE(s_spec_ac[k].a, (size + 0xFFFu) & ~0xFFFu);
                memset((void *)(uintptr_t)s_spec_ac[k].a, 0, size);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                break;
            }
            *io = s_spec_ac[k].a;
            rb_rec_put(EV_NTALLOC, 0, ih, io, 4); /* ein spaeteres Neurechnen bedient denselben Block */
            s_rbi_alloc_rec++;
            s_spec_ac[k] = s_spec_ac[--s_spec_ac_n];
            s_spec_ac_hit++;
            return 1;
        }
    }
    if (s_rbi_ep && op == 0 && s_phase != PH_IDLE && GetCurrentThreadId() != s_game_tid) s_rbi_alloc_other++;
    if (s_rbi_ep && op == 3 && s_rbi_com_logged < 8) { /* W355 DIAG: Commit in reservierten Bereich (Adresse/Groesse) */
        s_rbi_com_logged++;
        HLOG_I("LOCRBI: DIAG Commit 0x%X Groesse 0x%X frisch=%u Phase %d Spiel-Thread %d Spieltick %u", *io & ~0xFFFu, size,
               *io & 1u, s_phase, GetCurrentThreadId() == s_game_tid, rd32(RB_TICK));
    }
    if (s_memlog && s_rbi_ep && op >= 2 && s_memlog_n < 600) { /* W359 DIAG memlog */
        s_memlog_n++;
        s_memlog_lt = rd32(RB_TICK);
        HLOG_I("LOCRBI: DIAG memlog %s 0x%X Groesse 0x%X Typ 0x%X Phase %d Spiel-Thread %d Spieltick %u Slot %d", op == 2 ? "Commit?" :
               (op == 3 ? "Commit" : (op == 5 ? "Freigabe" : "?")), *io & ~0xFFFu, size, type, s_phase,
               GetCurrentThreadId() == s_game_tid, rd32(RB_TICK), s_rbc ? rbi_cons(s_rbc, rbi_self(s_rbc)) : -1);
    }
    if (op == 5) { /* W366: Decommit/Freigabe - diese Seiten sind nicht mehr belegt. Ohne Abgleich meldete op 2 einen
                    * spaeteren Commit derselben Seiten als "schon belegt" und liess ihn aus: Das Spiel schrieb in
                    * zurueckgegebene Heap-Seiten (zweites Match ohne Neustart, Laden der Mission: AV 0x12810A). */
        u32 fa = *io & ~0xFFFu, fe = size ? ((*io + size + 0xFFFu) & ~0xFFFu) : fa + 0x2000000u, k, n = 0;
        for (k = 0; k < s_pcom_n;) {
            if (s_pcom[k].end > fa && s_pcom[k].a < fe) {
                s_pcom[k] = s_pcom[--s_pcom_n]; /* nur weniger "schon belegt": danach echter Commit (sicher) */
                n++;
            } else {
                k++;
            }
        }
        if (n && s_rbi_pcom_drop_log < 8) {
            s_rbi_pcom_drop_log++;
            HLOG_I("LOCRBI: Freigabe 0x%X Groesse 0x%X - %u Commit-Eintraege verworfen (spaeterer Commit wieder echt)", fa,
                   size, n);
        }
        return 0;
    }
    if (!s_rbi_ep || !rb_mine()) return 0;
    if (op == 3) { /* W355: Commit aufzeichnen (Adresse, frisch) */
        u32 a3 = *io & ~0xFFFu, e3 = (a3 + size + 0xFFFu) & ~0xFFFu, k;
        for (k = 0; k < s_pcom_n; k++) /* schon in der Liste (erneuter Commit derselben Seiten)? */
            if (a3 >= s_pcom[k].a && e3 <= s_pcom[k].end) break;
        if (k == s_pcom_n) {
            if (s_pcom_n < RB_PCOM_MAX) {
                s_pcom[s_pcom_n].a = a3;
                s_pcom[s_pcom_n].end = e3;
                s_pcom[s_pcom_n].live = 1;
                s_pcom_n++;
            } else {
                s_pcom_full++;
            }
        }
        if (s_phase == PH_RECORD) {
            u32 v = *io;
            rb_rec_put(EV_NTCOMMIT, 0, ih ^ ((v & ~0xFFFu) * 13u), &v, 4); /* gleiche Basis = gleicher Aufruf */
            s_rbi_com_rec++;
        } else if (s_phase == PH_REPLAY) {
            s_rbi_com_new++; /* nur die korrigierte Zeitlinie committet */
        }
        return 0;
    }
    if (op == 2) { /* W355: Neurechnen - derselbe Commit wie in der ersten Rechnung */
        u32 a, end, k;
        (void)ih;
        a = *io & ~0xFFFu;
        end = (*io + size + 0xFFFu) & ~0xFFFu;
        for (k = 0; k < s_pcom_n; k++)
            if (a >= s_pcom[k].a && end <= s_pcom[k].end) break;
        if (k == s_pcom_n || end <= a) { /* nicht seit dem letzten Neuaufbau belegt: Original */
            if (s_phase == PH_REPLAY) s_rbi_com_miss++;
            return 0;
        }
        if (s_pcom[k].live) { /* in dieser Zeitlinie schon belegt: Commit aendert nichts (kein Registereintrag) */
            s_rbi_com_same++;
            *io = a;
            return 1;
        }
        __try {
            MEMORY_BASIC_INFORMATION mb;
            if (!VirtualQuery((LPCVOID)(uintptr_t)a, &mb, sizeof mb) || mb.State != MEM_COMMIT ||
                (u32)((uintptr_t)mb.BaseAddress + mb.RegionSize) < end) { s_rbi_com_miss++; return 0; }
            /* verworfene Zeitlinie hatte den Bereich belegt: Inhalt wie frisch committet. Nur dieser Teilbereich gilt
             * danach als belegt; reicht der Eintrag weiter, wird er geteilt (Rest bleibt verworfen). */
            LOC_RB_PREWRITE(a, end - a);
            memset((void *)(uintptr_t)a, 0, end - a);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            s_rbi_com_miss++;
            return 0;
        }
        if (s_pcom[k].a == a && s_pcom[k].end == end) {
            s_pcom[k].live = 1;
        } else if (s_pcom_n + 2u <= RB_PCOM_MAX) { /* teilen: [a0,a) verworfen, [a,end) belegt, [end,e0) verworfen */
            u32 a0 = s_pcom[k].a, e0 = s_pcom[k].end;
            s_pcom[k].a = a;
            s_pcom[k].end = end;
            s_pcom[k].live = 1;
            if (a0 < a) { s_pcom[s_pcom_n].a = a0; s_pcom[s_pcom_n].end = a; s_pcom[s_pcom_n].live = 0; s_pcom_n++; }
            if (end < e0) { s_pcom[s_pcom_n].a = end; s_pcom[s_pcom_n].end = e0; s_pcom[s_pcom_n].live = 0; s_pcom_n++; }
        } else {
            s_pcom_full++; /* kein Platz zum Teilen: ganzer Eintrag gilt als belegt (Rest: erneuter Commit nullt nicht) */
            s_pcom[k].live = 1;
        }
        *io = a;
        s_rbi_com_rep++;
        return 1;
    }
    if (op == 1) {
        if (s_phase == PH_RECORD) {
            u32 v = *io;
            rb_rec_put(EV_NTALLOC, 0, ih, &v, 4);
            s_rbi_alloc_rec++;
        } else if (s_phase == PH_REPLAY) {
            s_rbi_alloc_new++; /* nur die korrigierte Zeitlinie belegt */
        }
        return 0;
    }
    if (s_phase == PH_REPLAY) {
        const uint8_t *d;
        const rb_ev_t *e = rb_rep_get(EV_NTALLOC, ih, &d);
        u32 a;
        if (!e || e->n != 4) { s_rbi_alloc_miss++; return 0; }
        memcpy(&a, d, 4);
        __try {
            MEMORY_BASIC_INFORMATION mb;
            if (!VirtualQuery((LPCVOID)(uintptr_t)a, &mb, sizeof mb) || mb.State != MEM_COMMIT) { s_rbi_alloc_nocommit++; return 0; }
            LOC_RB_PREWRITE(a, (size + 0xFFFu) & ~0xFFFu);
            memset((void *)(uintptr_t)a, 0, size);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
        *io = a;
        s_rbi_alloc_rep++;
        return 1;
    }
    return 0;
}

/* ---- W359 DIAG rbver=N: Neurechnen mit gleichen Eingaben gegen den Live-Stand pruefen ----
 * Jeder N-te erzwungene Rollback (rbtest): vor dem Laden alle Ring-Bereiche kopieren (Live-Stand Tick T), nach dem
 * Neurechnen bis Tick T Wort fuer Wort vergleichen. Gleiche Eingaben -> jede Abweichung ist Zustand, den Laden +
 * Neurechnen nicht originaltreu herstellt. Logs: nur Adressen, Zaehler und Werte des Spielspeichers (keine Eingaben). */
static int s_rbv_n, s_rbv_arm, s_rbv_on, s_rbv_div;
static uint8_t *s_rbv_buf;
static size_t s_rbv_cap;
static unsigned s_rbv_gen;
static u32 s_rbv_reb, s_rbv_tick, s_rbv_cnt, s_rbv_bad, s_rbv_skip, s_rbv_logged, s_rbv_pages_sum;
#define RBV_H 384
typedef struct { u32 page, n, words, first; } rbv_h_t;
static rbv_h_t s_rbv_h[RBV_H];
static unsigned s_rbv_nh, s_rbv_hfull;
static void rbv_take(void)
{
    if (!s_rbv_arm) return;
    s_rbv_arm = 0;
    if (s_total > s_rbv_cap) {
        if (s_rbv_buf) VirtualFree(s_rbv_buf, 0, MEM_RELEASE);
        s_rbv_cap = s_total + 0x400000u;
        s_rbv_buf = (uint8_t *)VirtualAlloc(NULL, s_rbv_cap, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!s_rbv_buf) { s_rbv_cap = 0; s_rbv_skip++; return; }
    }
    __try {
        rb_copy_out(s_rbv_buf);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        s_rbv_skip++;
        return;
    }
    s_rbv_gen = s_reg_gen;
    s_rbv_reb = s_r_rebuild;
    s_rbv_tick = rd32(RB_TICK);
    s_rbv_on = 1;
}
static void rbv_hadd(u32 page, u32 words, u32 first)
{
    unsigned i;
    for (i = 0; i < s_rbv_nh; i++) {
        if (s_rbv_h[i].page == page) {
            s_rbv_h[i].n++;
            s_rbv_h[i].words += words;
            return;
        }
    }
    if (s_rbv_nh >= RBV_H) { s_rbv_hfull++; return; }
    s_rbv_h[s_rbv_nh].page = page;
    s_rbv_h[s_rbv_nh].n = 1;
    s_rbv_h[s_rbv_nh].words = words;
    s_rbv_h[s_rbv_nh].first = first;
    s_rbv_nh++;
}
static void rbv_check(void)
{
    int i;
    u32 np = 0, nw = 0, ex[8][3], nex = 0;
    if (!s_rbv_on) return;
    s_rbv_on = 0;
    if (s_rbv_gen != s_reg_gen || s_rbv_reb != s_r_rebuild || rd32(RB_TICK) != s_rbv_tick) {
        s_rbv_skip++;
        return;
    }
    __try {
        for (i = 0; i < s_nreg; i++) {
            const u32 *live = (const u32 *)(uintptr_t)s_reg[i].va;
            const u32 *ref = (const u32 *)(s_rbv_buf + s_reg[i].off);
            u32 o, w;
            for (o = 0; o < s_reg[i].len / 4u; o += RB_PG / 4u) {
                u32 pw = 0, first = 0;
                if (memcmp(live + o, ref + o, RB_PG) == 0) continue;
                for (w = 0; w < RB_PG / 4u; w++) {
                    if (live[o + w] == ref[o + w]) continue;
                    if (!pw++) first = s_reg[i].va + (o + w) * 4u;
                    if (nex < 8) {
                        ex[nex][0] = s_reg[i].va + (o + w) * 4u;
                        ex[nex][1] = ref[o + w];
                        ex[nex][2] = live[o + w];
                        nex++;
                    }
                }
                np++;
                nw += pw;
                rbv_hadd(s_reg[i].va + o * 4u, pw, first);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        s_rbv_skip++;
        return;
    }
    s_rbv_cnt++;
    s_rbv_pages_sum += np;
    if (!np) return;
    s_rbv_bad++;
    if (s_rbv_logged < 40) {
        char b[512];
        int n = 0;
        u32 k;
        s_rbv_logged++;
        for (k = 0; k < nex && n < (int)sizeof b - 40; k++)
            n += snprintf(b + n, sizeof b - (size_t)n, " 0x%X:%08X->%08X", ex[k][0], ex[k][1], ex[k][2]);
        b[sizeof b - 1] = 0;
        HLOG_W("LOCRBV: Neurechnen != Live bei Tick %u: %u Seiten, %u Woerter |%s", s_rbv_tick, np, nw, b);
    }
}
static int rbv_cmp(const void *a, const void *b)
{
    const rbv_h_t *p = (const rbv_h_t *)a, *q = (const rbv_h_t *)b;
    return p->n < q->n ? 1 : (p->n > q->n ? -1 : (p->page > q->page) - (p->page < q->page));
}
/* W359 DIAG csn=1: Gast-Sperren (RtlEnterCriticalSection), die andere Threads als der Spiel-Thread in R2 betreten
 * (Sperre, Ruecksprung, Thread-Start) - zeigt, welche Gast-Threads gemeinsame Strukturen (Heap) mitbenutzen. */
extern void (*volatile g_loc_cs_note)(u32 cs, u32 ret);
static int s_csn;
#define CSN_N 48
static struct { u32 cs, ret, tid, n; } s_csn_t[CSN_N];
static volatile LONG s_csn_lock;
static u32 s_csn_full;
static void rb_cs_note(u32 cs, u32 ret)
{
    u32 tid = GetCurrentThreadId(), i;
    if (!s_rbi_ep || tid == s_game_tid) return;
    while (InterlockedExchange(&s_csn_lock, 1)) SwitchToThread();
    for (i = 0; i < CSN_N && s_csn_t[i].n; i++)
        if (s_csn_t[i].cs == cs && s_csn_t[i].ret == ret && s_csn_t[i].tid == tid) break;
    if (i < CSN_N) {
        if (!s_csn_t[i].n) { s_csn_t[i].cs = cs; s_csn_t[i].ret = ret; s_csn_t[i].tid = tid; }
        s_csn_t[i].n++;
    } else {
        s_csn_full++;
    }
    InterlockedExchange(&s_csn_lock, 0);
}
static u32 rb_thread_start_of(u32 tid)
{
    int i, n = krnl_xb_thread_count();
    for (i = 0; i < n; i++) {
        void *h = NULL;
        unsigned r = 0;
        if (krnl_xb_thread_get(i, &h, &r) && GetThreadId((HANDLE)h) == tid) return r;
    }
    return 0;
}
static void rb_csn_log(int waited_s)
{
    char b[1400];
    int n = 0;
    u32 i;
    if (!s_csn) return;
    for (i = 0; i < CSN_N && s_csn_t[i].n && n < (int)sizeof b - 64; i++)
        n += snprintf(b + n, sizeof b - (size_t)n, " cs=0x%X ret=0x%X start=0x%X n=%u", s_csn_t[i].cs, s_csn_t[i].ret,
                      rb_thread_start_of(s_csn_t[i].tid), s_csn_t[i].n);
    b[n < (int)sizeof b ? n : (int)sizeof b - 1] = 0;
    HLOG_I("LOCRBV: t=%ds Gast-Sperren anderer Threads (Spiel-Thread ausgenommen)%s:%s", waited_s,
           s_csn_full ? " (Tabelle voll)" : "", n ? b : " keine");
}
static void rbv_log(int waited_s)
{
    char b[1400];
    int n = 0;
    unsigned i;
    rb_csn_log(waited_s);
    if (!s_rbv_n) return;
    qsort(s_rbv_h, s_rbv_nh, sizeof s_rbv_h[0], rbv_cmp);
    for (i = 0; i < s_rbv_nh && i < 40 && n < (int)sizeof b - 48; i++)
        n += snprintf(b + n, sizeof b - (size_t)n, " %X:%u/%u@%X", s_rbv_h[i].page, s_rbv_h[i].n, s_rbv_h[i].words,
                       s_rbv_h[i].first);
    b[n < (int)sizeof b ? n : (int)sizeof b - 1] = 0;
    HLOG_I("LOCRBV: t=%ds geprueft=%u abweichend=%u uebersprungen=%u Seiten-Summe=%u verschiedene Seiten=%u%s | Seite:Pruefungen/"
           "Woerter@erstes:%s",
           waited_s, s_rbv_cnt, s_rbv_bad, s_rbv_skip, s_rbv_pages_sum, s_rbv_nh, s_rbv_hfull ? " (Tabelle voll)" : "", b);
}

/* ---- W359 Absicherung: Freilisten des Spiel-Heaps pruefen (heapchk=1, Standard). Nach jeder Neurechnung und je
 * Sekunde live. Beschaedigt -> sauberes Ende (Meldung) statt Absturz/Haenger in der Liste. Heap-Zeiger: 0x306F84
 * (Spiel), 0x73EAB4 (XAPI-Prozess-Heap). Gelesen wird nur innerhalb der gesicherten Bereiche. ---- */
static int rb_heap_rd(void *ctx, u32 va, u32 *out)
{
    int lo = 0, hi = s_nreg - 1, ok = 0;
    u32 k;
    (void)ctx;
    while (lo <= hi && !ok) { /* s_reg aufsteigend (schneller Weg) */
        int m = (lo + hi) / 2;
        if (va < s_reg[m].va) hi = m - 1;
        else if (va - s_reg[m].va + 4u > s_reg[m].len) lo = m + 1;
        else ok = 1;
    }
    for (k = 0; !ok && k < s_pcom_n; k++) /* seit dem letzten Neuaufbau belegt (noch nicht im Ring, z. B. Heap-Wachstum) */
        if (va >= s_pcom[k].a && va + 4u <= s_pcom[k].end) ok = 1;
    if (!ok) { /* sonst: belegter, lesbarer Speicher (Verkettung/Groesse pruefen weiter streng) */
        MEMORY_BASIC_INFORMATION mb;
        if (va < 0x10000u || !VirtualQuery((LPCVOID)(uintptr_t)va, &mb, sizeof mb) || mb.State != MEM_COMMIT ||
            (mb.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !(mb.Protect & 0xEEu))
            return 0;
        s_rbi_hc_vq++;
    }
    __try {
        *out = *(volatile const u32 *)(uintptr_t)va;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return 1;
}
int krnl_cs_try_enter(u32 cs);                     /* krnl_sync.c (W359) */
u32 __stdcall hle_RtlLeaveCriticalSection(u32 cs); /* krnl_sync.c */
static void rbi_heapchk(int after_replay)
{
    static const u32 k_hp[2] = {0x306F84u, 0x73EAB4u};
    u32 h[2], i;
    long long t0;
    if (!s_rbi_heapchk || s_rbi_heap_bad || !s_ring || !s_nreg || rbi_free_running(s_rbc)) return;
    t0 = rb_now();
    h[0] = rd32(k_hp[0]);
    h[1] = rd32(k_hp[1]);
    for (i = 0; i < 2; i++) {
        u32 bad = 0, nodes = 0, us;
        int rc;
        u32 lk;
        if (!h[i] || (i && h[1] == h[0])) continue;
        /* Sperre des Heaps (RtlFreeHeap 0x12970C, nur ohne HEAP_NO_SERIALIZE in Heap+0x18): andere Threads nie mitten im
         * Umketten lesen */
        lk = (rd32(h[i] + 0x18u) & 1u) ? 0u : rd32(h[i] + 0x580u);
        if (lk && !krnl_cs_try_enter(lk)) {
            s_rbi_hc_busy++;
            continue;
        }
        rc = rbi_heap_check(rb_heap_rd, NULL, h[i], 20000u, &bad, &nodes);
        if (lk) (void)hle_RtlLeaveCriticalSection(lk);
        s_rbi_hc_n++;
        if (nodes > s_rbi_hc_nodes_max) s_rbi_hc_nodes_max = nodes;
        if (nodes >= 20000u) s_rbi_hc_part++;
        if (rc != RBI_HEAP_OK) {
            s_rbi_hc_bad++;
            s_rbi_heap_bad = 1;
            HLOG_W("LOCRBI: Heap-Pruefung: Spiel-Heap 0x%X beschaedigt (%s, Adresse 0x%X) bei Slot %d, Spieltick %u%s - "
                   "Sitzung wird sauber beendet", h[i], rbi_heap_err(rc), bad, rbi_cons(s_rbc, rbi_self(s_rbc)),
                   rd32(RB_TICK), after_replay ? " (nach Neurechnung)" : "");
        }
        us = rb_us(rb_now() - t0);
        if (us > s_rbi_hc_us_max) s_rbi_hc_us_max = us;
    }
}

static void rbi_replay_finish(void)
{
    { /* W346 R3: Cues der ersten Rechnung, die die korrigierte Zeitlinie nicht mehr startet (Zaehler) */
        u32 c;
        for (c = s_xwin0; c + sizeof(rb_ev_t) <= s_xwin1 && s_xwin1 <= s_rec_len;) {
            rb_ev_t *e = (rb_ev_t *)(s_rec + c);
            if ((e->fn == EV_XPLAY || e->fn == EV_XPREP) && !(e->pad & (LRS_PAD_USED | LRS_PAD_DEAD))) {
                u32 ser = e->n >= 8u ? ((const u32 *)(e + 1))[1] : 0u;
                int icue = -1, lp = 0, st = 0, stopped;
                char nm[40] = "";
                s_xc_orphan++;
                if (rbs_on() && ser) st = loc_audio_cue_info(ser, &icue, &lp, NULL, nm, (int)sizeof nm);
                if (st == 2 && lp) s_rbs_rec_loop++;
                /* W375 (#51): Schleife ohne Besitzer (Handle nur im verworfenen Stand) - sonst laeuft sie ewig, auch in
                 * den Menues (Eject-Alarm). Einmal-Cues laufen aus wie bisher. */
                stopped = ser && loc_audio_stop_orphan(ser);
                if (stopped) s_xc_orphan_stop++;
                if (stopped && rbs_act()) { /* W413: tot - eine spaetere Neurechnung startet ihn live statt Handle */
                    e->pad |= LRS_PAD_DEAD;
                    s_rbs_rec_dead++;
                }
                RBS_LOG("LOCSNDLOOP verwaist: Cue %d '%s' Schleife=%s Start=erste Rechnung (vorhergesagt) Fenster=%u..%u -> %s",
                        icue, nm, lp ? "ja" : "nein", s_xwin_g0, s_xwin_g1,
                        stopped ? "gestoppt (W375)" : (st == 0 ? "schon aus" : (lp ? "spielt weiter" : "laeuft aus")));
            }
            e->pad &= (uint8_t)~LRS_PAD_USED;
            c += (u32)sizeof *e + e->n;
        }
        if (rbs_on()) { /* W413: nachgeholte Waisen (Nebentabelle): von einer frueheren Neurechnung live gestartet, von dieser nicht */
            u32 i, w = 0;
            for (i = 0; i < s_xside_n; i++) {
                rb_xside_t *x = &s_xside[i];
                if (lrs_side_orphan(x->g, x->used, s_xwin_g0, s_xwin_g1)) {
                    int icue = -1, lp = 0, st = 0, stopped = 0;
                    char nm[40] = "";
                    s_rbs_side_orphan++;
                    if (x->serial) st = loc_audio_cue_info(x->serial, &icue, &lp, NULL, nm, (int)sizeof nm);
                    if (st == 2 && lp) {
                        s_rbs_side_loop++;
                        if (rbs_act()) stopped = loc_audio_stop_serial(x->serial, 1);
                        if (stopped) s_rbs_side_stop++;
                    }
                    RBS_LOG("LOCSNDLOOP verwaist: Cue %d '%s' Schleife=%s Start=Neurechnen (nachgeholt, Tick %u) Fenster=%u..%u -> %s",
                            icue, nm, lp ? "ja" : "nein", x->g, s_xwin_g0, s_xwin_g1,
                            stopped ? "gestoppt" : (st == 0 ? "schon aus" : (lp ? "spielt weiter (Fix aus)" : "laeuft aus")));
                    if (stopped) continue; /* Eintrag weg: keine Zeitlinie besitzt ihn mehr */
                }
                s_xside[w++] = *x;
            }
            s_xside_n = w;
        }
        s_xwin0 = s_xwin1 = 0;
    }
    if (!s_render || s_rbi_prep) /* kein halber Auftrag fuer den Render-Thread */
        *(volatile u32 *)(uintptr_t)RB_RJOB = rbi_rjob_replay(s_rbi_prep, RBI_RJOB_DONE, rd32(RB_RJOB));
    rbi_heapchk(1); /* W359: Heap nach dem Neurechnen pruefen (Render-Thread steht noch) */
    if (s_freeze) rb_thaw_others();
    rb_rs_resume(); /* W411: nur, wenn gebucht */
    if (s_pdpause) sb_interp_rb_pause(0);
    { /* W372: Feldbezug wieder live (sonst wartet der naechste Tick nicht) */
        u32 f = rd32(RBI_FIELD_LAST);
        s_rbi_fld_n++;
        if (f != s_rbi_fld_live) {
            s_rbi_fld_diff++;
            if (s_rbi_fldfix) *(volatile u32 *)(uintptr_t)RBI_FIELD_LAST = s_rbi_fld_live;
        }
    }
    InterlockedExchange(&s_skipwait, 0);
    s_phase = PH_RECORD;
    rb_put(s_rbi_s_rep, rb_us(rb_now() - s_rbi_t_rep0));
    rbl_note_replay((u32)rb_us(rb_now() - s_rbi_t_rep0)); /* W386 */
}

static void rbi_mission_end(void)
{
    const rbi_stats_t *st = rbi_stats(s_rbc);
    rbl_mission_end(); /* W386 */
    if (s_rbi_ep) {
        if (rbi_replaying(s_rbc) && !s_rep_ended) rbi_replay_finish(); /* W411: schon beim Missionsende im Neurechnen */
        HLOG_I("LOCRBI: Mission %u Ende: Slot %d, Rollbacks %u, verloren %u, Hash verglichen %u abweichend %u",
               s_rbi_missions, rbi_cons(s_rbc, rbi_self(s_rbc)), st->rb_plans, st->rb_lost, st->hash_cmp, st->hash_bad);
        rbv_log(-1); /* W359 DIAG */
        s_rbi_ep = 0;
        loc_sync_ext_on(0);
        s_phase = PH_IDLE;
    }
    rbi_end(s_rbc);
    s_rep_ended = 0; /* W411 */
    s_rbs_net_n = 0; /* W413: Missionsende stoppt die Schleifen der Mission (W375) */
    loc_smooth_reset(); /* W385 */
    s_rbi_mst = RBN_ST_IDLE;
    EnterCriticalSection(&s_rbn_cs);
    rbn_mission_end(s_rbn);
    LeaveCriticalSection(&s_rbn_cs);
}

/* W346: Synchronisation verloren (Hash-Abweichung) bzw. Uneinigkeit: kein Rollback mehr, nie haengen (Freilauf),
 * Verbindung zum Mitspieler wie beim Gastfilter trennen (definierter Weg des Spiels) und den Nutzer informieren. */
static void rbi_session_end(int desync)
{
    locnet *n = loc_net_core();
    wchar_t t1[160], t2[240];
    int k, killed = 0;
    if (rbi_free_running(s_rbc)) return;
    rbi_free_run(s_rbc);
    if (desync == 2) s_rbi_end_heap++; /* W359: Heap-Pruefung */
    else if (desync) s_rbi_end_desync++;
    else s_rbi_end_disagree++;
    HLOG_W("LOCRBI: %s bei Slot %d - Rollback aus, Verbindung zum Mitspieler wird getrennt (Meldung an den Nutzer)",
           desync == 2 ? "Spielzustand beschaedigt (Heap-Pruefung)"
                       : (desync ? "Synchronisation verloren (Hash-Abweichung)" : "Netzmodus uneinig"),
           rbi_cons(s_rbc, rbi_self(s_rbc)));
    __try { /* Spiel-TCP-Verbindungen (Tabelle wie LOCGUARD) */
        u32 tab = *(volatile const u32 *)(uintptr_t)0x3116ACu;
        for (k = 0; tab && k < 10; k++) {
            SOCKET sk = (SOCKET) * (volatile const u32 *)(uintptr_t)(tab + (u32)k * 0x10u + 4u);
            if (sk != INVALID_SOCKET && sk != 0 && n) {
                locnet_guard_kill(n, sk, LGUARD_V_CONTENT);
                killed++;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    HLOG_I("LOCRBI: %d Spiel-Verbindungen getrennt", killed);
    if (desync == 2) { /* W359 */
        sb_lang_w("loc.rb_heap_t", t1, 160);
        sb_lang_w("loc.rb_heap_d", t2, 240);
    } else if (desync) {
        sb_lang_w("loc.rb_desync_t", t1, 160);
        sb_lang_w("loc.rb_desync_d", t2, 240);
    } else {
        sb_lang_w("loc.rb_disagree_t", t1, 160);
        sb_lang_w("loc.rb_disagree_d", t2, 240);
    }
    sb_ov_toast_msg(t1, t2, 9000u);
}

static u32 rbi_cmin(void)
{
    u32 c = rbi_commits(s_rbc), d;
    if (!s_ring || !s_ready || s_failed || !rbr_active() || !s_rbi_rec) return c; /* kein Ring/keine Aufzeichnung: keine Vorhersage */
    d = rbr_depth();
    return c > d ? c - d : 0;
}

/* W366 LOC-NET-EDGES: Abgang eines Mitspielers mitten in der Mission (Spiel geschlossen, Absturz, Netz weg). Das Spiel
 * entfernt ihn selbst (Stillezaehler > 200 bzw. Verbindungsreset -> Aktiv-Maske [0x306830] ohne sein Bit) und spielt
 * allein weiter (LOC_RE1 §6.3). Die Eingabeschicht wartete aber weiter auf die Bestaetigung seiner vorhergesagten
 * Slots (Ringtiefe) -> Stau fuer immer (Lauf E2a: Spieltick steht bei 948). Erkennung: sein Bit war in dieser Mission
 * gesetzt, ist jetzt weg, das eigene Bit steht noch, und seit >= 3 s kam kein Paket von ihm (lebende Mitspieler
 * senden SBR2/SBRH laufend) -> nie mehr auf ihn warten; ist keiner mehr da, Freilauf (kein Rollback, kein Stau). */
static u32 s_rbi_act_seen, s_rbi_rxn[RBI_MAXP], s_rbi_rxt[RBI_MAXP], s_rbi_left_n;
static void rbi_left_reset(void)
{
    int k;
    s_rbi_act_seen = 0;
    for (k = 0; k < RBI_MAXP; k++) {
        s_rbi_rxn[k] = 0;
        s_rbi_rxt[k] = GetTickCount();
    }
}
static void rbi_check_left(void)
{
    /* Spielerzahl [RBI_NPL] nicht verwenden: das Spiel zaehlt sie beim Entfernen sofort herunter (Lauf E2f) */
    u32 act = rd32(RB_ACTIVE), now = GetTickCount(), gone;
    int self = rbi_self(s_rbc), k, left = 0, rest = 0;
    if (self < 0 || self >= RBI_MAXP || !((act >> self) & 1u)) return;
    gone = rbi_gone_mask(s_rbc);
    for (k = 0; k < RBI_MAXP && k < 32; k++) {
        u32 b = 1u << k, n;
        if (k == self) continue;
        n = rbi_rx_count(s_rbc, k);
        if (n != s_rbi_rxn[k]) {
            s_rbi_rxn[k] = n;
            s_rbi_rxt[k] = now;
        }
        if (act & b) s_rbi_act_seen |= b;
        if (!(s_rbi_act_seen & b)) continue;
        if (!(gone & b) && !(act & b) && now - s_rbi_rxt[k] >= 3000u) {
            rbi_peer_gone(s_rbc, k);
            gone |= b;
            left++;
            s_rbi_left_n++;
            HLOG_W("LOCRBI: Mitspieler %d hat die Mission verlassen (vom Spiel entfernt, seit %u ms still) bei Slot %d - "
                   "Eingabeschicht wartet nicht mehr auf ihn", k, now - s_rbi_rxt[k], rbi_cons(s_rbc, self));
        }
        if (!(gone & b)) rest++;
    }
    if (left && !rest && !rbi_free_running(s_rbc)) {
        wchar_t t1[160], t2[240];
        rbi_free_run(s_rbc);
        HLOG_W("LOCRBI: kein Mitspieler mehr in der Mission - allein weiter (Freilauf, kein Rollback)");
        sb_lang_w("loc.rb_left_t", t1, 160);
        sb_lang_w("loc.rb_left_d", t2, 240);
        sb_ov_toast_msg(t1, t2, 9000u);
    }
}

/* FUN_0004E920(i, &kopf): eax = Eingabesatz (14 B), *kopf = Rahmenkopf (8 B); 0 = Stau. cdecl, Spiel-Thread. */
static uint8_t s_spec_out[RBI_MAXP][RBI_FR]; /* W390: eigene Puffer (s_rbi_out bleibt der echte Stand) */
static u32 s_spec_k[RBI_MAXP];                /* W390: Entnahmen je Spieler im laufenden Vorlauf */
static u32 __cdecl rbi_e920(u32 i, u32 pout)
{
    if (s_spec && i < RBI_MAXP) { /* W390: Vorlauf - Rahmen ohne Zustandsaenderung der Eingabeschicht */
        int r = rbi_spec_frame(s_rbc, (int)i, s_spec_k[i], s_spec_own_have ? s_spec_own_new : NULL, s_spec_out[i]);
        if (!r) {
            *(volatile u32 *)(uintptr_t)pout = 0;
            return 0;
        }
        s_spec_k[i]++;
        s_spec_ev.take++;
        if (r == 2) s_spec_ev.take_pred++;
        *(volatile uint16_t *)(uintptr_t)(RBI_TAKENW + i * 2u) += 1;
        *(volatile u32 *)(uintptr_t)pout = (u32)(uintptr_t)s_spec_out[i];
        return (u32)(uintptr_t)(s_spec_out[i] + RBI_HDR);
    }
    if (s_rbi_mst == RBN_ST_TENT || s_rbi_mst == RBN_ST_PEND) { /* W346: vor der Entscheidung keine Entnahme (Stau) */
        *(volatile u32 *)(uintptr_t)pout = 0;
        return 0;
    }
    if (!s_rbi_ep || i >= RBI_MAXP) return o_e920(i, pout);
    if (s_phase != PH_REPLAY) {
        rbi_check_left(); /* W366 */
        if (s_rbi_ts_in || (s_rbi_ts_stall && !s_rbi_tick_takes)) { /* Zeitabgleich: ganzer Tick als Stau */
            if (!s_rbi_ts_in) s_rbi_ts_waits++;
            s_rbi_ts_in = 1;
            s_rbi_ts_stall = 0;
            *(volatile u32 *)(uintptr_t)pout = 0;
            return 0;
        }
        rbi_poll();
    }
    rbi_clock(s_rbc, rbi_us_now()); /* W372: Entnahmezeit */
    if (s_phase != PH_REPLAY && !s_tim_take_seen && i != (u32)rbi_self(s_rbc)) {
        s_tim_take_seen = 1;
        tim_h(s_tim_h_take, rbi_us_now(), s_tim_t0);
    }
    if (!rbi_take(s_rbc, (int)i, rbi_cmin(), s_rbi_out[i])) {
        *(volatile u32 *)(uintptr_t)pout = 0;
        return 0;
    }
    *(volatile uint16_t *)(uintptr_t)(RBI_TAKENW + i * 2u) += 1; /* wie das Original (Zaehler der Entnahmen) */
    s_rbi_tick_takes++;
    s_rbi_dflags[i == (u32)rbi_self(s_rbc) ? 0 : 1] |= *(const u32 *)(s_rbi_out[i] + 4); /* DIAG: Flags (keine Eingaben) */
    if (i < 2 && s_phase != PH_REPLAY) { /* DIAG: Pruefsumme des Eingabestroms je Spieler (ohne Sync-Byte), keine Eingaben */
        uint8_t t[RBI_FR];
        memcpy(t, s_rbi_out[i], RBI_FR);
        t[1] = 0;
        s_rbi_dstream[i] = fnv(s_rbi_dstream[i] * 31u + s_rbi_dn[i]++, t, RBI_FR);
        if (s_rbi_dn[i] % 25u == 0 && s_rbi_dn[i] <= 250u) {
            s_rbi_dchk[i][s_rbi_dn[i] / 25u - 1u] = s_rbi_dstream[i];
            if (i == 1) s_rbi_dcnt[s_rbi_dn[i] / 25u - 1u] = rd32(0x5C5A40u) | (rd32(RB_TICK) << 16);
        }
    }
    *(volatile u32 *)(uintptr_t)pout = (u32)(uintptr_t)s_rbi_out[i];
    return (u32)(uintptr_t)(s_rbi_out[i] + RBI_HDR);
}

volatile long g_loc_rb_own_ms; /* W420 LOC-CLIPDUMP (loc_rollback.h) */
/* FUN_0004EC80(daten, kopf): eigener Rahmen; im Original TCP an alle + eigener Ring. Rueckgabe 1. */
static u32 __cdecl rbi_ec80(u32 data, u32 hdr)
{
    int pre;
    InterlockedExchange(&g_loc_rb_own_ms, (LONG)(GetTickCount() | 1u)); /* W420: eigener Rahmen = Mission laeuft */
    if (s_spec) { /* W390: Vorlauf - erster neu erzeugter eigener Rahmen = der des naechsten Live-Ticks (gleiche Abfrage) */
        s_spec_ev.ec80++;
        if (!s_spec_own_have) {
            memcpy(s_spec_own_new, (const void *)(uintptr_t)hdr, RBI_HDR);
            memcpy(s_spec_own_new + RBI_HDR, (const void *)(uintptr_t)data, RBI_IN);
            s_spec_own_have = 1;
        }
        return o_ec80(data, hdr); /* Spielzustand wie live; das TCP-Senden unterdrueckt rb_send */
    }
    pre = rd32(RBI_PREFILL) == 1u;
    if (pre && !s_rbi_last_pre) { /* Vorlauf von FUN_0001D3D0 = neue Mission */
        if (s_rbi_mst != RBN_ST_IDLE) rbi_mission_end();
        rbi_mission_open(); /* W346: Aushandlung */
    }
    s_rbi_last_pre = pre;
    if (s_rbi_mst == RBN_ST_TENT || s_rbi_mst == RBN_ST_PEND) { /* W346: eigene Rahmen schon mitschreiben */
        rbi_own(s_rbc, (const uint8_t *)(uintptr_t)hdr, (const uint8_t *)(uintptr_t)data);
        return o_ec80(data, hdr);
    }
    if (!s_rbi_ep) return o_ec80(data, hdr);
    if (s_rbi_dtick_own++ && s_phase != PH_REPLAY) s_rbi_dmulti++; /* DIAG: mehr als ein eigener Rahmen je Tick */
    if (s_rbi_early && s_phase != PH_REPLAY) { /* W372: Rahmen gleich nach dem Erzeugen senden (statt ~50 ms spaeter) */
        if (rbi_own(s_rbc, (const uint8_t *)(uintptr_t)hdr, (const uint8_t *)(uintptr_t)data)) {
            if (!s_tim_own_seen) { /* W372: Zeitpunkt des eigenen Rahmens im Tick */
                s_tim_own_seen = 1;
                s_tim_own_t = rbi_us_now();
                s_tim_own_pend = 1;
                tim_h(s_tim_h_own, s_tim_own_t, s_tim_t0);
                lat_note_own(); /* W386 */
                if (!s_tim_take_seen) s_tim_own_first++;
            }
            rbi_poll();
            rbi_send();
            s_rbi_sent_early = 1;
        }
        return o_ec80(data, hdr);
    }
    if (s_phase != PH_REPLAY && !s_tim_own_seen) { /* W372: Zeitpunkt des eigenen Rahmens im Tick */
        s_tim_own_seen = 1;
        s_tim_own_t = rbi_us_now();
        s_tim_own_pend = 1;
        tim_h(s_tim_h_own, s_tim_own_t, s_tim_t0);
        lat_note_own(); /* W386 */
        if (!s_tim_take_seen) s_tim_own_first++;
    }
    rbi_own(s_rbc, (const uint8_t *)(uintptr_t)hdr, (const uint8_t *)(uintptr_t)data);
    /* Der 0x34-Rahmen geht weiter wie im Original ueber TCP (und in den eigenen Ring): Das Spiel fuehrt je Mitspieler
     * einen Stille-Zaehler ([0x366498+i*16+8], Reset bei jedem TCP-Empfang in FUN_00053B20, > 200 Ticks = Mitspieler
     * weg in FUN_00050EF0) und tauscht in den Kopfbytes 2/3 Werte je Ziel aus. Der Inhalt wird nicht mehr entnommen
     * (FUN_0004E920 bedient die Eingabeschicht), niemand wartet darauf. Im Neurechnen unterdrueckt rb_send das Senden. */
    return o_ec80(data, hdr);
}

/* 0x1D0BF in FUN_0001D010: mit R2 Regelung ueberspringen (weiter bei 0x1D381, Kopf mit Maske/Flags steht in
 * [esp+0x10]), sonst Lockstep-Zweig loc_lsarb_reg_entry (Originalbefehl "mov ebx,[0x3071A0]" und weiter bei 0x1D0C5
 * bzw. neuer Pufferregler, loc_lsarb_game.c). eax bleibt erhalten. */
static __declspec(naked) void rbi_reg_thunk(void)
{
    __asm {
        cmp dword ptr [s_rbi_ep], 0
        je rbi_orig
        mov dword ptr ds:[306944h], 1
        push ecx
        mov ecx, dword ptr [s_rbi_reg_d]
        mov dword ptr ds:[306F04h], ecx
        pop ecx
        push 1D381h
        ret
    rbi_orig:
        jmp loc_lsarb_reg_entry /* LOC-LOCKSTEP-ARBITER: Lockstep-Zweig (Original oder neuer Regler) */
    }
}

static void rbi_rec_compact(void)
{
    u32 g = rbi_gtick(s_rbc), keep, base, j, span = RBI_GH / 2u - 1u;
    if (s_rec_len < RB_REC_CAP / 2u) return;
    if (s_rec_len > (RB_REC_CAP / 4u) * 3u) span = 256u;
    keep = g > span ? g - span : 0;
    base = s_rbi_toff[keep & RBI_GM];
    if (base == 0 || base > s_rec_len) return;
    memmove(s_rec, s_rec + base, s_rec_len - base);
    s_rec_len -= base;
    for (j = keep; j <= g; j++) s_rbi_toff[j & RBI_GM] -= base;
    s_rbi_compact++;
}

static void rbi_set_rep_cursor(void)
{
    u32 g = rbi_gtick(s_rbc), k;
    s_rec_cur = s_rbi_toff[g & RBI_GM];
    s_rep_end = s_rbi_toff[(g + 1u) & RBI_GM];
    if (s_rep_end < s_rec_cur) s_rep_end = s_rec_cur;
    for (k = 0; k < EV_N; k++) s_rep_kc[k] = s_rec_cur; /* W346 R3 */
}
static u32 rb_rbi_gtick(void) { return s_rbc ? rbi_gtick(s_rbc) : 0; }

static void rbi_try_rollback(void)
{
    u32 ticks = 0, sb = 0, pages = 0, us = 0;
    int k;
    if (!rbi_rb_pending(s_rbc)) return;
    if (!s_ring || !s_ready || s_failed || !rbr_active()) {
        k = rbi_plan(s_rbc, 0, &ticks, &sb); /* nicht moeglich -> gezaehlt */
        return;
    }
    if (!rb_quiesce(1)) { /* Render-Thread beschaeftigt: naechster Tick */
        s_rbi_qfail++;
        return;
    }
    if (s_pdpause && !sb_interp_rb_pause(1)) s_r_pd_fail++;
    if (s_freeze) rb_freeze_others();
    rbv_take(); /* W359 DIAG rbver: Live-Stand vor dem Laden */
    s_rbi_fld_live = rd32(RBI_FIELD_LAST); /* W372: Feldbezug des Feld-Wartens (live) */
    loc_smooth_rb_pre(rd32(RB_TICK)); /* W385: vorhergesagte Lagen dieses Ticks (vor dem Laden) */
    s_ls_cause = rbi_rb_cause(s_rbc, 1);
    k = rbi_plan(s_rbc, rbr_depth(), &ticks, &sb);
    s_ls_ticks = ticks;
    if (k > 0 && rbr_load((unsigned)k, &pages, &us) != 0) { /* darf nicht vorkommen (k <= rbr_depth) */
        s_rbi_load_fail++;
        k = -2;
    }
    if (k <= 0) {
        loc_smooth_rb_pre(0xFFFFFFFFu); /* W385: kein Rollback -> Messung verwerfen (Tick passt nie) */
        s_rbv_on = 0;
        if (s_freeze) rb_thaw_others();
        rb_rs_resume(); /* W411: nur, wenn gebucht */
        if (s_pdpause) sb_interp_rb_pause(0);
        if (k < 0 && s_rbi_lost_logged < 8) {
            s_rbi_lost_logged++;
            HLOG_W("LOCRBI: Rollback nicht moeglich (%s) bei Slot %d, Ring %u Ticks - Desync moeglich",
                   k == -2 ? "Laden fehlgeschlagen" : "zu weit zurueck/Barriere", rbi_cons(s_rbc, rbi_self(s_rbc)),
                   rbr_depth());
        }
        return;
    }
    rb_pcom_rollback(); /* W355: Commits seit dem Neuaufbau gehoeren jetzt zur verworfenen Zeitlinie */
    if (s_memlog && s_memlog_n < 600 && s_memlog_lt && rd32(RB_TICK) + 40u >= s_memlog_lt && rd32(RB_TICK) <= s_memlog_lt + 40u) {
        s_memlog_n++;
        HLOG_I("LOCRBI: DIAG memlog Rollback %d Ticks (%u Slots) -> Spieltick %u, Neurechnen bis %u", k, ticks, rd32(RB_TICK),
               rbi_replay_until(s_rbc));
    }
    rb_put(s_rbi_s_load, us);
    rb_put(s_rbi_s_ticks, ticks);
    if (ticks > s_rbl_wdep) s_rbl_wdep = ticks; /* W386 */
    rb_put(s_rbi_s_slots, sb);
    { /* W374: Ring fuer das F3-Overlay (Tiefe p95 der letzten 10 s) */
        LONG k = s_rbi_hud_n % RBI_HUD_N;
        s_rbi_hud_d[k] = ticks;
        s_rbi_hud_t[k] = GetTickCount();
        InterlockedIncrement(&s_rbi_hud_n);
    }
    s_rbi_rb_n++;
    if (s_rbi_prep || !s_render) /* W359: Vorbereitung beim Neurechnen wie live (rprep=0: ueberspringen) */
        *(volatile u32 *)(uintptr_t)RB_RJOB = rbi_rjob_replay(s_rbi_prep, RBI_RJOB_LOAD, rd32(RB_RJOB));
    s_phase = PH_REPLAY;
    InterlockedExchange(&s_skipwait, 1);
    { /* W346 R3: Fenster der Neurechnung fuer die Cue-Zuordnung */
        u32 c, i;
        s_xwin_g0 = rbi_gtick(s_rbc);
        s_xwin_g1 = rbi_replay_until(s_rbc);
        s_xwin0 = s_rbi_toff[s_xwin_g0 & RBI_GM];
        s_xwin1 = s_rec_len; /* der laufende Tick ist noch nicht abgeschlossen: bis zum Ende der Aufzeichnung */
        if (s_xwin0 > s_xwin1) s_xwin0 = s_xwin1;
        for (c = s_xwin0; c + sizeof(rb_ev_t) <= s_xwin1;) {
            rb_ev_t *e = (rb_ev_t *)(s_rec + c);
            e->pad &= (uint8_t)~1u;
            if (s_rbs_net_n && (e->fn == EV_XPLAY || e->fn == EV_XPREP) && e->n >= 8u)
                rbs_net_touch(((const u32 *)(e + 1))[1]); /* W413: Start im Fenster - vom Neurechnen beruehrt */
            c += (u32)sizeof *e + e->n;
        }
        for (i = 0; i < s_xside_n; i++) s_xside[i].used = 0;
    }
    rbi_set_rep_cursor();
    s_rbi_t_rep0 = rb_now();
}

/* LOC-DESYNC-REPAIR (21.11.) DIAG sdump=X,S,N (loc_rb_opts): Zustand (Schattenkopie = Stand nach dem Sichern) am
 * Spieltick X, X+S, ... (N-mal) nach <out>\rb_state_<Tick>.bin, nur wenn keine Vorhersage offen ist (bestaetigter Stand).
 * Messgrundlage, ob sich ein Zustand auf einen anderen Client uebertragen laesst (Groesse, Lage der Bereiche, eigene
 * Daten je Client). Spielspeicher: Datei bleibt im Laufordner, nie ins Repo. Keine Eingaben. */
static int s_sd_x, s_sd_step = 100, s_sd_n;
static u32 s_sd_next, s_sd_done, s_sd_skip;
static void rbi_state_dump(int snap_ok)
{
    u32 gt = rd32(RB_TICK), npg, nreg = 0, hdr[8], i;
    uint8_t *buf;
    char fn[600];
    const char *od;
    HANDLE h;
    DWORD wr;
    long long t0;
    if (s_sd_n <= 0 || s_sd_done + s_sd_skip >= (u32)s_sd_n || !s_ring || !rbr_active() || rbi_replaying(s_rbc)) return;
    if (!s_sd_next) s_sd_next = (u32)s_sd_x;
    if (gt < s_sd_next) return;
    s_sd_next = gt + (u32)(s_sd_step > 0 ? s_sd_step : 100);
    if (!snap_ok || rbi_open_preds(s_rbc)) {
        s_sd_skip++;
        HLOG_I("LOCRBI: DIAG Zustandsabzug Tick %u uebersprungen (%s)", gt, snap_ok ? "Vorhersage offen" : "kein Sichern");
        return;
    }
    npg = rbr_pages();
    buf = (uint8_t *)malloc((size_t)npg * 4096u);
    if (!buf) return;
    t0 = rb_now();
    rbr_shadow_copy(buf);
    od = hcfg_out_dir();
    _snprintf_s(fn, sizeof fn, _TRUNCATE, "%s\\rb_state_%u.bin", od && od[0] ? od : ".", gt);
    h = CreateFileA(fn, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        uint32_t va, len, kind, hot;
        int meth;
        while (rbr_region_info((int)nreg, &va, &len, &kind, &meth, &hot)) nreg++;
        hdr[0] = 0x44534253u; /* "SBSD" */
        hdr[1] = 1;
        hdr[2] = npg;
        hdr[3] = nreg;
        hdr[4] = gt;
        hdr[5] = rd32(RBI_SELF);
        hdr[6] = (u32)rbi_cons(s_rbc, rbi_self(s_rbc));
        hdr[7] = 0;
        WriteFile(h, hdr, sizeof hdr, &wr, NULL);
        for (i = 0; i < nreg; i++) {
            u32 r4[4];
            rbr_region_info((int)i, &va, &len, &kind, &meth, &hot);
            r4[0] = va;
            r4[1] = len;
            r4[2] = kind;
            r4[3] = (u32)meth;
            WriteFile(h, r4, sizeof r4, &wr, NULL);
        }
        WriteFile(h, buf, npg * 4096u, &wr, NULL);
        CloseHandle(h);
        s_sd_done++;
        HLOG_I("LOCRBI: DIAG Zustandsabzug Tick %u Slot %u: %u Seiten (%u KB), %u Bereiche, %.1f ms", gt, hdr[6], npg,
               npg * 4u, nreg, rb_us(rb_now() - t0) / 1000.0);
    }
    free(buf);
}

static void rbi_finals(void)
{
    u32 sl, acc, gt;
    const rbi_stats_t *st;
    while (rbi_final_next(s_rbc, &sl, &acc, &gt)) loc_sync_ext_frame(sl, acc, gt);
    st = rbi_stats(s_rbc);
    if (!s_rbi_hcmp_logged) { /* W355: frueh und eindeutig belegen, dass die Hash-Pruefung von Anfang an laeuft */
        int eq = 0;
        int32_t f = rbi_hash_first_cmp(s_rbc, &eq);
        if (f >= 0) {
            s_rbi_hcmp_logged = 1;
            HLOG_I("LOCRBI: Hash-Pruefung aktiv: erster Vergleich mit dem Mitspieler bei Slot %d %s (danach jeder endgueltige "
                   "Slot; Abweichung -> Zeile DESYNC)", f, eq ? "gleich" : "VERSCHIEDEN");
        }
    }
    if (st->hash_bad && !s_rbi_hash_logged) {
        s_rbi_hash_logged = 1;
        HLOG_W("LOCRBI: DESYNC erkannt: Zustandshash von Slot %d weicht vom Mitspieler ab (Hash-Pruefung R2): "
               "eigen %08X/Tick %u, Mitspieler %08X/Tick %u",
               st->hash_bad_first, st->hb_acc[0], st->hb_gt[0], st->hb_acc[1], st->hb_gt[1]);
    }
    if (!rbi_free_running(s_rbc)) { /* W346 R4: Desync bzw. Uneinigkeit -> sauberes Ende mit Meldung */
        int dis;
        EnterCriticalSection(&s_rbn_cs);
        (void)rbn_poll(s_rbn, GetTickCount());
        dis = rbn_peer_disagrees(s_rbn);
        LeaveCriticalSection(&s_rbn_cs);
        if (st->hash_bad) rbi_session_end(1);
        else if (dis) rbi_session_end(0);
    }
}

/* Ticks ohne Ring (vor dem Aufbau bzw. nach Fehlschlag): Buchfuehrung, Kanal, keine Vorhersage. */
static void rbi_after_noring(void)
{
    (void)rbi_tick_end(s_rbc, NULL);
    s_rbi_dtick_own = 0;
    s_rbi_tick_takes = 0;
    s_rbi_ts_in = 0;
    s_rbi_toff[rbi_gtick(s_rbc) & RBI_GM] = s_rec_len;
    rbi_poll();
    if (!s_rbi_sent_early) rbi_send(); /* W372: in diesem Tick schon gesendet (frueh) */
    s_rbi_sent_early = 0;
    rbi_finals();
    rbl_tick(); /* W386 */
}

/* ================= W390 LOC-OWN-PREDICT: spekulativer Vorlauf ================= */
#define RB_SPEC_MAXT 9
static int s_spec_want, s_spec_n, s_spec_phase0;
static u32 s_spec_gt0, s_spec_gt[RB_SPEC_MAXT], s_spec_reg0, s_spec_rjob_live, s_spec_fld_live;
static long s_spec_memev0;
static float s_spec_pose[RB_SPEC_MAXT][4];
static int s_spec_pose_ok[RB_SPEC_MAXT];
static long long s_spec_t0;
static u32 s_spec_runs, s_spec_ticks, s_spec_early, s_spec_skip[8], s_spec_rev_fail, s_spec_reg_grow, s_spec_memev,
    s_spec_ac_leak, s_spec_verify_n, s_spec_verify_bad, s_spec_files;
static rb_ser_t *s_spec_s_us, *s_spec_s_rev, *s_spec_s_pages; /* je Vorlauf: Gesamtzeit, Zuruecksetzen (us), Seiten */
#define RB_SPEC_WIN 100
static u32 s_spec_win[RB_SPEC_WIN]; /* gleitend: Gesamtzeit der letzten 100 Vorlaeufe (Rueckfall) */
static int s_spec_prep;  /* Render-Vorbereitung im Vorlauf (loc_own_predict_prep) */
static volatile LONG s_spec_hud_us, s_spec_hud_p95, s_spec_hud_lead; /* F3: letzter Vorlauf, p95 (us), Vorlauf in Ticks */

int loc_rb_spec(void) { return s_spec && GetCurrentThreadId() == s_game_tid; }
void loc_rb_spec_note(int kind)
{
    if (kind > 0 && kind < 6) s_spec_ev.kfile[kind]++;
}
static u32 s_sc_spec[512], s_sc_live[512];
void loc_rb_sc_count(unsigned ord)
{
    if (ord >= 512u || GetCurrentThreadId() != s_game_tid) return;
    if (s_spec) s_sc_spec[ord]++;
    else if (s_phase == PH_RECORD) s_sc_live[ord]++;
}

/* W390: F3-Overlay - 0 = nicht aktiv; mode 1 an, 2 Pause, 3 aus fuer die Mission; Vorsprung (ms), Kosten (us, p95). */
int loc_rb_ownpred_hud(int *mode, int *lead_ms, int *cost_us)
{
    if (!g_cfg.loc_own_predict || !s_rbi_ep || s_rbi_mst != RBN_ST_R2 || !s_spec_runs) return 0;
    *mode = s_spec_off_mission ? 3 : (s_spec_pause_until ? 2 : 1);
    *lead_ms = (int)(s_spec_hud_lead > 0 ? (s_spec_hud_lead - 1) * 50 : 0);
    *cost_us = (int)(s_spec_hud_p95 ? s_spec_hud_p95 : s_spec_hud_us);
    return 1;
}

static int rb_spec_skip(int why)
{
    if (why >= 0 && why < 8) s_spec_skip[why]++;
    return 0;
}

/* Am Ende eines Live-Ticks (rbi_after), wenn der Schatten = aktueller Stand ist (snap_ok). 1 = Vorlauf gestartet. */
static int rb_spec_try_start(int snap_ok)
{
    int d;
    u32 k;
    float p[4];
    if (!g_cfg.loc_own_predict || !s_rbi_ep || !s_rbc) return 0;
    if (s_spec_xcn) { /* XInput aus dem letzten Vorlauf, das der Live-Tick nicht abgeholt hat */
        for (k = 0; k < s_spec_xcn; k++)
            if (!s_spec_xc[k].used) s_spec_xc_unused++;
        s_spec_xcn = 0;
    }
    if (s_spec_off_mission) return rb_spec_skip(4);
    /* W411: Missionsende laeuft (Zaehler != 0) bzw. die Schleife endet nach diesem Tick: kein Vorlauf - er wuerde die
     * Schleife in der verworfenen Zeitlinie verlassen (Render-Thread angehalten, Auftrag 1 -> ewiges Kreisen @0x2F355). */
    if (s_endguard && rd32(RB_END_CNT) != 0) return rb_spec_skip(7);
    if (!snap_ok) return rb_spec_skip(0);
    if (!s_ring || !s_ready || s_failed || !rbr_active() || !rbi_active(s_rbc) || rbi_replaying(s_rbc) ||
        rbi_free_running(s_rbc) || s_phase != PH_RECORD || s_rbi_mst != RBN_ST_R2)
        return rb_spec_skip(6);
    if (s_spec_pause_until && (LONG)(GetTickCount() - s_spec_pause_until) < 0) return rb_spec_skip(4);
    s_spec_pause_until = 0;
    if (g_exreg_n != s_reg_gen || g_loc_rb_memev != s_memev_seen || s_r_force_rebuild) return rb_spec_skip(1);
    d = rbi_delay_eff(s_rbc);
    if (d < 0) return rb_spec_skip(6);
    s_spec_want = g_cfg.loc_own_predict_ticks > 0 ? g_cfg.loc_own_predict_ticks : d + 1 - s_spec_level;
    if (s_spec_want < 1) s_spec_want = 1;
    if (s_spec_want > RB_SPEC_MAXT) s_spec_want = RB_SPEC_MAXT;
    if (!loc_smooth_own_read(p)) return rb_spec_skip(5);
    if (rd32(RB_RJOB) == 1u) return rb_spec_skip(2); /* Render-Thread zeichnet noch: nicht warten */
    rb_rthread_check();
    if (s_rthread) {
        if (!rb_rs_suspend(s_rthread, 2)) return rb_spec_skip(2); /* W411: gebucht */
        if (rd32(RB_RJOB) == 1u) {
            rb_rs_resume(); /* W411 */
            return rb_spec_skip(2);
        }
    }
    /* W393 LOC-PACING loc_pd_spec=1: Present-Takt laeuft weiter. Er liest nur Host-Daten (aufgezeichnete Listen,
     * Lage per Seqlock, loc_smooth) und den D3D-Kontext unter ctx_lock; der Vorlauf schreibt nur Gast-Speicher
     * (danach zurueckgesetzt), der Render-Thread des Spiels ruht. So faellt nie ein Bild wegen des Vorlaufs aus. */
    s_spec_pd = s_pdpause && !g_cfg.loc_pd_spec;
    if (s_spec_pd && !sb_interp_rb_pause(1)) {
        sb_interp_rb_pause(0);
        rb_rs_resume(); /* W411: nur, wenn gebucht */
        return rb_spec_skip(3);
    }
    if (s_freeze) rb_freeze_others();
    { /* Belegungen aus frueheren Vorlaeufen, die die echte Zeitlinie nicht wollte: ohne Besitzer (Leck, gezaehlt) */
        u32 now = rd32(RB_TICK), w = 0;
        for (k = 0; k < s_spec_ac_n; k++) {
            if (s_spec_ac[k].gt + 300u < now) s_spec_ac_leak++;
            else s_spec_ac[w++] = s_spec_ac[k];
        }
        s_spec_ac_n = w;
    }
    s_spec_gt0 = rd32(RB_TICK);
    s_spec_esp0 = s_rb_esp0;
    s_spec_caller = s_rb_caller;
    s_spec_rjob_live = rd32(RB_RJOB);
    s_spec_fld_live = rd32(RBI_FIELD_LAST);
    s_spec_reg0 = g_exreg_n;
    s_spec_memev0 = g_loc_rb_memev;
    memset(s_spec_k, 0, sizeof s_spec_k);
    memset(s_spec_pose_ok, 0, sizeof s_spec_pose_ok);
    s_spec_own_have = 0;
    s_spec_n = 0;
    s_spec_cm_n = 0;
    s_spec_prep = g_cfg.loc_own_predict_prep;
    /* Render-Vorbereitung (W359: reiner Spielspeicher - Freigabe-Warteschlange, Doppelpuffer) ist fuer die VT-Lage
     * ohne Belang und wird ohnehin zurueckgesetzt: im Vorlauf standardmaessig auslassen (Auftrag gilt als belegt). */
    *(volatile u32 *)(uintptr_t)RB_RJOB = rbi_rjob_replay(s_spec_prep, RBI_RJOB_LOAD, rd32(RB_RJOB));
    s_spec_phase0 = s_phase;
    s_phase = PH_REPLAY;
    s_spec = 1;
    InterlockedExchange(&s_skipwait, 1);
    s_spec_t0 = rb_now();
    return 1;
}

static void rb_spec_finish(void)
{
    u32 pages = 0, chk = 0, us = 0, tot, k;
    int rv = rbr_revert(&pages, &chk, &us);
    if (rv != 0) {
        s_spec_rev_fail++;
        if (s_spec_rev_fail <= 4) HLOG_E("LOCOWNPRED: Zuruecksetzen nach dem Vorlauf fehlgeschlagen (Ring aus) - Zustand weicht ab");
    }
    *(volatile u32 *)(uintptr_t)RB_RJOB = s_spec_rjob_live;
    *(volatile u32 *)(uintptr_t)RBI_FIELD_LAST = s_spec_fld_live;
    for (k = 0; k < s_spec_cm_n; k++) { /* im Vorlauf committet: Inhalt gehoert der verworfenen Zeitlinie */
        if (s_pcom_n < RB_PCOM_MAX) {
            s_pcom[s_pcom_n].a = s_spec_cm[k].a;
            s_pcom[s_pcom_n].end = s_spec_cm[k].end;
            s_pcom[s_pcom_n].live = 0;
            s_pcom_n++;
        } else {
            s_pcom_full++;
        }
    }
    s_spec_cm_n = 0;
    s_spec_files += (u32)krnl_file_spec_restore();
    if (g_exreg_n != s_spec_reg0) s_spec_reg_grow++; /* Neuaufbau folgt im naechsten rbi_after (wie jede Belegung) */
    if (g_loc_rb_memev != s_spec_memev0) s_spec_memev++;
    if ((g_cfg.loc_own_predict_diag & 1) && (s_spec_runs % 50u) == 0u) { /* DIAG: Schatten = Live nach dem Zuruecksetzen */
        u32 a[2] = {0, 0}, nd = rbr_shadow_diff(a, 2);
        s_spec_verify_n++;
        if (nd) {
            s_spec_verify_bad++;
            if (s_spec_verify_bad <= 8) HLOG_W("LOCOWNPRED: DIAG Pruefung nach dem Zuruecksetzen: %u Seiten abweichend (erste 0x%X)", nd, a[0]);
        }
    }
    if (s_freeze) rb_thaw_others();
    rb_rs_resume(); /* W411: nur, wenn gebucht */
    if (s_spec_pd) sb_interp_rb_pause(0); /* W393 */
    s_spec = 0;
    s_phase = s_spec_phase0;
    InterlockedExchange(&s_skipwait, 0);
    tot = rb_us(rb_now() - s_spec_t0);
    s_spec_runs++;
    s_spec_ticks += (u32)s_spec_n;
    rb_put(s_spec_s_us, tot);
    rb_put(s_spec_s_rev, us);
    rb_put(s_spec_s_pages, pages);
    s_spec_win[s_spec_win_n++ % RB_SPEC_WIN] = tot;
    InterlockedExchange(&s_spec_hud_us, (LONG)tot);
    InterlockedExchange(&s_spec_hud_lead, (LONG)s_spec_n);
    if (s_spec_win_n >= RB_SPEC_WIN && (s_spec_win_n % 20u) == 0u) {
        /* Rueckfall: p75 der letzten 100 Vorlaeufe > Grenze (dauerhaft teuer, z. B. schwache CPU; einzelne Ausreisser
         * durch Fremdlast loesen nicht aus). Stufe 1: ein Tick weniger; danach 30 s Pause (Stufe wieder 0); nach 5
         * Pausen aus fuer die Mission. */
        u32 tmp[RB_SPEC_WIN], lim = (u32)(g_cfg.loc_own_predict_max_ms > 0 ? g_cfg.loc_own_predict_max_ms : 5) * 1000u, p75;
        memcpy(tmp, s_spec_win, sizeof tmp);
        qsort(tmp, RB_SPEC_WIN, sizeof tmp[0], rb_u32cmp);
        p75 = tmp[(RB_SPEC_WIN * 75) / 100];
        InterlockedExchange(&s_spec_hud_p95, (LONG)tmp[(RB_SPEC_WIN * 95) / 100]);
        if (p75 > lim) {
            s_spec_win_n = 0;
            if (s_spec_level == 0 && s_spec_n > 1) {
                s_spec_level = 1;
                s_spec_degr++;
                HLOG_W("LOCOWNPRED: Vorlauf teuer (p75 %u us > %u us) - ein Tick weniger", p75, lim);
            } else if (++s_spec_paused_n >= 5) {
                s_spec_off_mission = 1;
                HLOG_W("LOCOWNPRED: Vorlauf zu teuer (p75 %u us > %u us) - fuer diese Mission aus (5. Pause)", p75, lim);
            } else {
                s_spec_level = 0;
                s_spec_pause_until = GetTickCount() + 30000u;
                if (!s_spec_pause_until) s_spec_pause_until = 1;
                HLOG_W("LOCOWNPRED: Vorlauf zu teuer (p75 %u us > %u us) - 30 s Pause (%d. Mal)", p75, lim, s_spec_paused_n);
            }
        }
    }
    loc_smooth_own_pred(s_spec_gt0 + 1u, s_spec_n, s_spec_gt, (const float(*)[4])s_spec_pose, s_spec_pose_ok);
}

/* W411: Vorlauf ohne Zuruecksetzen beenden (nur Spiel-Thread). Fuer Lagen, in denen Zuruecksetzen falsch waere: die
 * Missionsschleife ist schon verlassen bzw. eine neue Mission laeuft (Spieltick kleiner als beim Start). Mit endguard=1
 * tritt das nicht ein (Zaehler bleiben 0); ohne ihn bleibt der Stand der verworfenen Zeitlinie stehen (Mission ist zu Ende). */
static void rb_spec_abandon(const char *why)
{
    s_spec_abandon_n++;
    HLOG_W("LOCOWNPRED: Vorlauf ohne Zuruecksetzen beendet (%s, Spieltick %u, Start %u, %d Ticks)", why, rd32(RB_TICK),
           s_spec_gt0, s_spec_n);
    s_spec_cm_n = 0;
    if (s_freeze) rb_thaw_others();
    rb_rs_resume();
    if (s_spec_pd) sb_interp_rb_pause(0);
    s_spec = 0;
    s_phase = s_spec_phase0;
    s_spec_off_mission = 1;
    memset(s_spec_pose_ok, 0, sizeof s_spec_pose_ok);
    InterlockedExchange(&s_skipwait, 0);
}

/* rb_after waehrend des Vorlaufs: ein Vorlauf-Tick ist fertig gerechnet. */
static void rb_spec_tick(void)
{
    *(volatile u32 *)(uintptr_t)RB_RJOB = rbi_rjob_replay(s_spec_prep, RBI_RJOB_TICK, rd32(RB_RJOB));
    if (s_spec_n < RB_SPEC_MAXT) {
        s_spec_gt[s_spec_n] = rd32(RB_TICK);
        s_spec_pose_ok[s_spec_n] = loc_smooth_own_read(s_spec_pose[s_spec_n]);
    }
    s_spec_n++;
    if (s_spec_n >= s_spec_want) {
        rb_spec_finish();
    } else if (s_endguard && rb_loop_exits()) {
        /* W411: die verworfene Zeitlinie wuerde die Missionsschleife jetzt verlassen (Ende-Zaehler an der Grenze, z. B.
         * Rauswurf/Abbruch mit Grenze 0): sofort zuruecksetzen - die Ende-Pruefung danach sieht den Live-Stand. */
        s_spec_endstop++;
        if (s_spec_endstop <= 4)
            HLOG_I("LOCOWNPRED: Vorlauf erreicht Missionsende (Tick %u, Zaehler %u/%u) - vorzeitig zurueckgesetzt",
                   rd32(RB_TICK), rd32(RB_END_CNT), rd32(RB_END_LIM));
        rb_spec_finish();
    } else if (rb_us(rb_now() - s_spec_t0) > (u32)(g_cfg.loc_own_predict_max_ms > 0 ? g_cfg.loc_own_predict_max_ms : 5) * 3000u) {
        s_spec_early++; /* einzelner Ausreisser: mit weniger Vorlauf beenden */
        rb_spec_finish();
    }
}

static int rb_ring_check_regions(void);
static void rbi_after(void)
{
    int done = 0, com, snap_ok = 0; /* W390: Schatten = aktueller Stand (Sichern bzw. Neuaufbau in diesem Tick) */
    if (s_phase == PH_REPLAY && (s_rbi_prep || !s_render)) /* W359: Render-Vorbereitung dieses Neurechen-Ticks lief wie
                                                            * live; Auftrag gilt als erledigt (gezeichnet wird nicht) */
        *(volatile u32 *)(uintptr_t)RB_RJOB = rbi_rjob_replay(s_rbi_prep, RBI_RJOB_TICK, rd32(RB_RJOB));
    com = rbi_tick_end(s_rbc, &done);
    s_rbi_dtick_own = 0;
    s_rbi_tick_takes = 0;
    s_rbi_ts_in = 0;
    { /* W346 R3: neue Allokationen (nur Register gewachsen, keine Freigabe/kein Schutzwechsel) aendern bestehende
       * Bereiche nicht: Neuaufbau aufschieben, bis keine Vorhersage mehr offen ist (bestaetigter Tick, Barriere ohne
       * Verlust); bis dahin keine neue Vorhersage. Freigaben/Fehler bauen wie bisher sofort neu. */
        int need = g_exreg_n != s_reg_gen || g_loc_rb_memev != s_memev_seen || s_r_force_rebuild;
        int only_alloc = need && g_loc_rb_memev == s_memev_seen && !s_r_force_rebuild;
        u32 waited = rbi_rebuild_wait(s_rbc);
        int gate;
        if (g_exreg_n != s_rbi_reg_tick) { /* W355: Belegung in diesem Tick (auch im Neurechnen) -> Untergrenze */
            s_rbi_reg_tick = g_exreg_n;
            rbi_rebuild_note(s_rbc);
        }
        gate = rbi_rebuild_gate(s_rbc, need, only_alloc, s_rbi_defer_on, 200u);
        /* W355: Entscheidung im Modul (Selbsttest); nie mitten in einer Neurechnung, Rollback ab Allokations-Tick */
        if (gate == RBI_REBUILD_DEFER) {
            if (!s_rbi_defer_ticks++) {
                s_rbi_defer_n++;
                HLOG_I("LOCRBI: Neuaufbau (neue Allokation) aufgeschoben bei Slot %d - offene Vorhersagen erst bestaetigen%s",
                       rbi_cons(s_rbc, rbi_self(s_rbc)), rbi_replaying(s_rbc) ? " (Neurechnung laeuft)" : "");
            }
            need = 0;
        } else if (gate == RBI_REBUILD_NOW && s_rbi_defer_ticks) {
            if (!rbi_open_preds(s_rbc)) {
                s_rbi_defer_clean++;
                HLOG_I("LOCRBI: Neuaufbau nach %u Ticks (%u live) an bestaetigtem Stand (kein Rollback verloren)",
                       s_rbi_defer_ticks, waited);
            } else {
                HLOG_W("LOCRBI: Neuaufbau nach %u Ticks trotz offener Vorhersage (Aufschub-Grenze)", s_rbi_defer_ticks);
            }
            s_rbi_defer_ticks = 0;
        } else if (gate == RBI_REBUILD_NONE) {
            s_rbi_defer_ticks = 0;
        }
        if (!need) goto rbi_no_rebuild;
    }
    if (rb_ring_check_regions()) { /* Ring neu: kein Rollback vor diesen Stand */
        rbi_barrier(s_rbc);
        s_rbi_bar++;
        rb_pcom_clear(); /* W355: die Seiten liegen jetzt im Ring */
        snap_ok = rbr_active();
    } else
    rbi_no_rebuild:
    if (com) {
        u32 ovf0 = s_r_ovf;
        if (s_phase != PH_REPLAY) (void)rb_quiesce(0);
        rb_ring_commit(s_phase == PH_REPLAY);
        snap_ok = !s_r_force_rebuild && rbr_active(); /* W390 */
        if (s_r_force_rebuild) (void)rb_ring_check_regions(); /* Sichern scheiterte: sofort neu, kein Laden */
        if (s_r_ovf != ovf0 || !rbr_active() || rbr_depth() == 0) {
            rbi_barrier(s_rbc);
            s_rbi_bar++;
        }
    }
    if (s_phase == PH_REPLAY) loc_smooth_replay_tick(rd32(RB_TICK)); /* W387: Lagen-Historie auf den korrigierten Stand */
    if (rbi_replaying(s_rbc)) { /* naechster Neurechen-Tick */
        if (s_endguard && rb_loop_exits() && !s_rep_ended) {
            /* W411: Die korrigierte Zeitlinie beendet die Mission in diesem Neurechen-Tick: die Schleife wird verlassen,
             * ein weiteres rb_after kommt nicht. Threads jetzt fortsetzen (sonst kreist der Spiel-Thread @0x2F355). */
            s_rep_endstop++;
            s_rep_ended = 1;
            HLOG_W("LOCRBI: Missionsende im Neurechnen (Spieltick %u, Zaehler %u/%u) - Neurechnung beendet, Threads fortgesetzt",
                   rd32(RB_TICK), rd32(RB_END_CNT), rd32(RB_END_LIM));
            rbi_replay_finish();
            return;
        }
        rbi_set_rep_cursor();
        return;
    }
    if (done) {
        rbv_check(); /* W359 DIAG: vor dem Fortsetzen der Threads */
        rbi_replay_finish();
        loc_smooth_rb_post(rd32(RB_TICK), s_ls_cause, s_ls_ticks); /* W385: korrigierte Lagen desselben Ticks */
    } else if (++s_rbi_hc_tick >= 20u) { /* W359: je Sekunde live (nach dem Sichern, Render-Thread fertig) */
        s_rbi_hc_tick = 0;
        rbi_heapchk(0);
    }
    if (s_rbi_heap_bad && !rbi_free_running(s_rbc)) rbi_session_end(2); /* W359: sauberes Ende statt Absturz */
    s_rbi_toff[rbi_gtick(s_rbc) & RBI_GM] = s_rec_len;
    rbi_rec_compact();
    if (s_rbs_net_n && s_rbi_ep) rbs_net_tick(); /* W413: Sicherheitsnetz (Live-Tick, nach dem Neurechnen) */
    rbi_poll();
    if (!s_rbi_sent_early) rbi_send(); /* W372: in diesem Tick schon gesendet (frueh) */
    s_rbi_sent_early = 0;
    rbi_finals();
    rbi_state_dump(snap_ok); /* LOC-DESYNC-REPAIR DIAG sdump */
    rbl_tick(); /* W386 */
    if (s_rbi_rbtest_n && s_ring && s_ready && !s_failed && rbr_active() &&
        rbi_gtick(s_rbc) >= s_rbi_rbtest_next) { /* W355 TEST: Neurechnen mit gleichen Eingaben muss bitgleich sein */
        s_rbi_rbtest_next = rbi_gtick(s_rbc) + (u32)s_rbi_rbtest_n; /* einmal je N Live-Ticks (nach dem Neurechnen
                                                                     * steht der Tick wieder gleich) */
        s_rbi_rbtest_cnt++;
        {
            int f = rbi_force_rb(s_rbc, (u32)s_rbi_rbtest_k, rbr_depth());
            s_rbi_rbtest_ok += (u32)f;
            if (f && s_rbv_n && s_rbi_rbtest_ok % (u32)s_rbv_n == 0) s_rbv_arm = 1; /* W359 DIAG */
        }
    }
    rbi_try_rollback();
    if (s_rbi_tsync == 2 && !rbi_replaying(s_rbc)) { /* W372: Feldzahl statt Stau-Tick (Freilauf: rbi_tsync = 0) */
        int ex = rbi_tsync(s_rbc);
        if (ex > 0) InterlockedExchange(&s_wait_extra, ex);
    } else if (s_rbi_tsync == 1 && !rbi_replaying(s_rbc) && !rbi_free_running(s_rbc)) { /* W366: allein kein Zeitabgleich */
        /* Zeitabgleich: Die Missionsschleife ist an den Bildtakt gebunden (Feld-Warten holt Pausen nach). Wer anhaltend
         * vorne liegt (geglaettet > 0,75 Slots ueber 10 Ticks), laesst einen ganzen Tick ohne Entnahme laufen (Stau wie
         * im Original, die Simulation steht), danach 10 Ticks Abklingzeit. */
        double a = rbi_advantage(s_rbc);
        s_rbi_hud_adv = a; /* W374: F3-Overlay */
        /* W355: bei grossem Vorsprung staerker abbremsen (W338: hoechstens 1 Stau je 11 Ticks = 9 %; im Nutzerlauf lief
         * eine Seite so 8-10 Slots vor, jede Vorhersage war 10 Slots tief). > 1,5 Slots: 1 je 5 Ticks, > 3: 1 je 3. */
        int need = a > 3.0 ? 2 : (a > 1.5 ? 4 : 10), cool = a > 3.0 ? 1 : (a > 1.5 ? 1 : 10);
        if (s_rbi_ts_cool) s_rbi_ts_cool--;
        if (a > 0.75) s_rbi_ts_run++;
        else s_rbi_ts_run = 0;
        if (s_rbi_ts_run >= (u32)need && !s_rbi_ts_cool) {
            s_rbi_ts_stall = 1;
            s_rbi_ts_ms += 50;
            s_rbi_ts_run = 0;
            s_rbi_ts_cool = (u32)cool;
        }
    }
    if (!rbi_replaying(s_rbc)) (void)rb_spec_try_start(snap_ok); /* W390: eigene Lage D + 1 Ticks voraus (nur Anzeige) */
}

static void rbi_patch_jmp(u32 va, const void *dst, unsigned len)
{
    uint8_t *p = (uint8_t *)(uintptr_t)va;
    intptr_t rel = (const uint8_t *)dst - (p + 5);
    DWORD old;
    VirtualProtect(p, len, PAGE_EXECUTE_READWRITE, &old);
    p[0] = 0xE9;
    memcpy(p + 1, &rel, 4);
    if (len > 5) memset(p + 5, 0x90, len - 5);
    VirtualProtect(p, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, len);
}

static void *rbi_tramp(uint8_t *mem, u32 va, unsigned n)
{
    intptr_t rel;
    memcpy(mem, (const void *)(uintptr_t)va, n);
    mem[n] = 0xE9;
    rel = (intptr_t)(va + n) - (intptr_t)(mem + n + 5);
    memcpy(mem + n + 1, &rel, 4);
    return mem;
}

static void rbi_install(void)
{
    static const uint8_t e920[6] = {0x8B, 0x44, 0x24, 0x04, 0x8B, 0xC8};       /* mov eax,[esp+4]; mov ecx,eax */
    static const uint8_t ec80[8] = {0x83, 0xEC, 0x20, 0xA1, 0x74, 0x96, 0x28, 0x00}; /* sub esp,20h; mov eax,[289674h] */
    static const uint8_t reg[6] = {0x8B, 0x1D, 0xA0, 0x71, 0x30, 0x00};        /* mov ebx,[3071A0h] */
    if (memcmp((const void *)(uintptr_t)RBI_E920, e920, 6) || memcmp((const void *)(uintptr_t)RBI_EC80, ec80, 8) ||
        memcmp((const void *)(uintptr_t)RBI_REG, reg, 6)) {
        HLOG_E("LOCBYTES LOCRBI @0x%X/0x%X/0x%X: Byte-Mismatch - ABBRUCH (exit 7)", RBI_E920, RBI_EC80, RBI_REG);
        hlog_close();
        ExitProcess(7);
    }
    s_rbc = rbi_new();
    rbi_set_pred(s_rbc, s_rbi_pred, s_rbi_predq); /* W385 */
    s_rbl = rbl_new(); /* W386 */
    s_rbi_tramp = (uint8_t *)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    InitializeCriticalSection(&s_rbi_tq_cs);
    InitializeCriticalSection(&s_rbn_cs);
    {
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        s_rbn = rbn_new(1, (u32)(q.QuadPart ^ (q.QuadPart >> 32) ^ (GetCurrentProcessId() << 16)) | 1u, s_rbi_dmin,
                        s_rbi_dmax, s_rbi_delay);
    }
    if (!s_rbc || !s_rbn || !s_rbi_tramp || !rbi_sock_open()) {
        HLOG_W("LOCRBI: Eingabeschicht nicht verfuegbar - Missionen laufen im Original-Lockstep");
        return;
    }
    o_e920 = (rbi_fn2_t)rbi_tramp(s_rbi_tramp, RBI_E920, 6);
    o_ec80 = (rbi_fn2_t)rbi_tramp(s_rbi_tramp + 32, RBI_EC80, 8);
    rbi_patch_jmp(RBI_E920, (const void *)rbi_e920, 6);
    rbi_patch_jmp(RBI_EC80, (const void *)rbi_ec80, 8);
    rbi_patch_jmp(RBI_REG, (const void *)rbi_reg_thunk, 6);
    /* Akkumulator ab Missionsbeginn (nicht erst beim Ringstart): jeder Slot bekommt seinen Hash, die Ketten beider
     * Seiten beginnen bei Slot 0 */
    if (rb_chain_call(RB_ACC_CALL, (void *)rb_acc_thunk, &s_acc_next, "Akkumulator")) s_rbi_acc_chained = 1;
    s_rbi_s_ticks = rb_ser_new("R2 Rollback-Tiefe (Spielticks)");
    s_rbi_s_slots = rb_ser_new("R2 Rollback-Tiefe (Slots)");
    s_rbi_s_load = rb_ser_new("R2 Ring laden (us)");
    s_rbi_s_rep = rb_ser_new("R2 Neurechnen gesamt (us)");
    s_spec_s_us = rb_ser_new("W390 Vorlauf gesamt (us)");
    s_spec_s_rev = rb_ser_new("W390 Vorlauf zuruecksetzen (us)");
    s_spec_s_pages = rb_ser_new("W390 Vorlauf Seiten");
    s_rbi = 1;
    g_loc_rb_alloc = rb_alloc_cb; /* W346 R3 */
    s_tim_base = rb_now(); /* W372: Zeitbasis der Messung (vor dem Kanal-Thread) */
    s_rbi_thr = CreateThread(NULL, 0, rbi_chan_thread, NULL, 0, NULL); /* W346: Empfang + Hallo */
    HLOG_I("LOCRBI: aktiv (loc_rb_input=1, je Mission ausgehandelt): Entnahme @0x%X, Senden @0x%X, Regelung @0x%X, "
           "Verzoegerung %s%d (Grenzen %d..%d), Fenster %d, Zeitabgleich %d, Kanal-Thread %s", RBI_E920, RBI_EC80, RBI_REG,
           s_rbi_delay > 0 ? "fest " : "adaptiv, Start ", s_rbi_delay > 0 ? s_rbi_delay : 2, s_rbi_dmin, s_rbi_dmax,
           s_rbi_maxpred, s_rbi_tsync, s_rbi_thr ? "laeuft" : "FEHLT");
}

static void rbi_log(int waited_s)
{
    const rbi_stats_t *st;
    u32 d50, d95, dmx, s50, s95, smx, r50, r95, rmx, l50, l95, lmx, rbw;
    if (!s_rbi || !s_rbc) return;
    { /* W346 R4: Telemetrie als Zaehlerzeile (nur Zahlen, keine Eingaben) - auch in der Lobby (Aushandlung) */
        rbn_stats_t ns;
        int rtt, fb = 0, k;
        static u32 last_rb;
        u32 a50, a95, amx, t50, t95, tmx, rbw10 = s_rbi_rb_n - last_rb;
        last_rb = s_rbi_rb_n;
        EnterCriticalSection(&s_rbn_cs);
        ns = *rbn_stats(s_rbn);
        rtt = rbn_rtt_max(s_rbn, GetTickCount());
        LeaveCriticalSection(&s_rbn_cs);
        for (k = 1; k < RBN_WHY_N; k++)
            if (k != RBN_WHY_SELF_OFF) fb += (int)ns.fallback[k];
        rbi_pct(s_rbi_s_ticks, &t50, &t95, &tmx);
        rbi_pct(s_rbi_s_rep, &a50, &a95, &amx);
        HLOG_I("LOCRBI-TEL: t=%ds Modus=%s Verz=%d RTT=%d ms | Rollbacks/s=%.2f gesamt=%u Tiefe p50/p95/max=%u/%u/%u Ticks "
               "Neurechnen p95=%u us max=%u | Missionen R2=%u Lockstep=%u Rueckfaelle=%d (letzter Grund %u) Ende Desync=%u "
               "uneinig=%u | Ton doppelt=%u nachgeholt=%u verwaist=%u | Neuaufbau aufgeschoben=%u sauber=%u Barrieren=%u | "
               "Neurechnen Allokation=%u neu=%u Senden live=%u | lange Ticks=%u max=%u ms | Hallo tx=%u rx=%u schlecht=%u voll=%u",
               waited_s,
               s_rbi_mst == RBN_ST_R2 ? (rbi_free_running(s_rbc) ? "R2-Ende" : "R2")
                                      : (s_rbi_mst == RBN_ST_LS ? "Lockstep"
                                                                : (s_rbi_mst == RBN_ST_TENT ? "Angebot"
                                                                                            : (s_rbi_mst == RBN_ST_PEND ? "Warten" : "Lobby"))),
               s_rbi_ep ? (int)s_rbi_reg_d : 0, rtt, rbw10 / 10.0, s_rbi_rb_n, t50, t95, tmx, a95, amx, s_rbi_r2_missions,
               s_rbi_ls_missions, fb, s_rbi_last_why, s_rbi_end_desync, s_rbi_end_disagree, s_xc_dup, s_xc_new, s_xc_orphan,
               s_rbi_defer_n, s_rbi_defer_clean, s_rbi_bar, s_rbi_alloc_rep, s_rbi_alloc_new, s_rbi_send_live, s_rbi_lt_n,
               s_rbi_lt_max, ns.tx, ns.rx_ok, ns.rx_bad, ns.rx_full);
        { /* W385: Vorhersage-Vergleich (nur Zaehler) und Lagen-Spruenge je Rollback */
            const rbi_stats_t *w = rbi_stats(s_rbc);
            HLOG_I("LOCRBI-PRED: t=%ds Modus=%d q=%d | Vorhersagen=%u richtig=%u falsch=%u Trend wirksam=%u | nur Trend richtig=%u "
                   "nur Wiederholung richtig=%u | Achsenfehler falsch Summe=%u (Wiederholung %u) | falsch nach Feld Achsen=%u "
                   "Ausweichen=%u Pedale=%u Gang=%u Tasten-B0=%u Tasten=%u Kopf=%u",
                   waited_s, rbi_pred_mode(s_rbc), s_rbi_predq, w->predicted, w->pred_ok, w->pred_bad, w->pred_trend,
                   w->pred_gain, w->pred_loss, w->mis_ax_err, w->mis_ax_err_rep, w->mis_cat[RBI_MIS_AXIS],
                   w->mis_cat[RBI_MIS_DODGE], w->mis_cat[RBI_MIS_PEDAL], w->mis_cat[RBI_MIS_GEAR], w->mis_cat[RBI_MIS_FIRE],
                   w->mis_cat[RBI_MIS_BTN], w->mis_cat[RBI_MIS_HDR]);
            loc_smooth_log(waited_s);
        }
        if (g_cfg.loc_own_predict && (s_spec_runs || s_spec_skip[0] + s_spec_skip[1] + s_spec_skip[2] + s_spec_skip[6])) {
            /* W390: Kosten und abgefangene Nebenwirkungen des Vorlaufs (nur Zaehler/Zeiten) */
            u32 u50, u95, umx, v50, v95, vmx, g50, g95, gmx;
            rbi_pct(s_spec_s_us, &u50, &u95, &umx);
            rbi_pct(s_spec_s_rev, &v50, &v95, &vmx);
            rbi_pct(s_spec_s_pages, &g50, &g95, &gmx);
            HLOG_I("LOCOWNPRED-COST: t=%ds Vorlaeufe=%u Ticks=%u frueh beendet=%u | Kosten je Tick p50/p95/max=%u/%u/%u us "
                   "Zuruecksetzen=%u/%u/%u us Seiten=%u/%u/%u | ausgelassen ohne-Sichern=%u Neuaufbau=%u Render=%u Bildtakt=%u "
                   "Pause=%u Lage=%u sonst=%u | Stufe=%d (weniger %u) Pausen=%d aus=%d Ruecksetz-Fehler=%u andere-Stelle=%u | abgefangen Senden=%u select=%u recv=%u "
                   "ioctl=%u XInput live/wieder=%u/%u (Live-Tick uebernommen=%u ungenutzt=%u) XACT Start=%u Stopp=%u Meldung=%u "
                   "Hash=%u Rahmen=%u (vorhergesagt %u) eigene=%u | Kernel Lesen=%u Schreiben=%u Schliessen=%u Datei-Info=%u "
                   "Freigabe=%u Positionen=%u | Belegung=%u (uebernommen %u Leck %u) Commit=%u Register+=%u Schutz=%u | "
                   "Pruefung %u/%u abweichend",
                   waited_s, s_spec_runs, s_spec_ticks, s_spec_early, u50, u95, umx, v50, v95, vmx, g50, g95, gmx,
                   s_spec_skip[0], s_spec_skip[1], s_spec_skip[2], s_spec_skip[3], s_spec_skip[4], s_spec_skip[5],
                   s_spec_skip[6], s_spec_level, s_spec_degr, s_spec_paused_n, s_spec_off_mission, s_spec_rev_fail, s_spec_ctx_bad, s_spec_ev.send, s_spec_ev.sel,
                   s_spec_ev.recv, s_spec_ev.ioctl, s_spec_ev.xin_live, s_spec_ev.xin_rep, s_spec_xc_hit, s_spec_xc_unused,
                   s_spec_ev.xcue, s_spec_ev.xstop, s_spec_ev.xgn, s_spec_ev.acc, s_spec_ev.take, s_spec_ev.take_pred,
                   s_spec_ev.ec80, s_spec_ev.kfile[1], s_spec_ev.kfile[2], s_spec_ev.kfile[3], s_spec_ev.kfile[4],
                   s_spec_ev.kfile[5], s_spec_files, s_spec_ev.alloc, s_spec_ac_hit, s_spec_ac_leak, s_spec_ev.commit,
                   s_spec_reg_grow, s_spec_memev, s_spec_verify_bad, s_spec_verify_n);
            if (g_cfg.syscall_trace == 2) { /* W390 DIAG: Kernel-Aufrufe im Vorlauf (Ordinal, Name, Anzahl; live zum Vergleich) */
                char b[1024];
                int o, l = 0;
                for (o = 0; o < 512 && l < (int)sizeof b - 64; o++)
                    if (s_sc_spec[o] || s_sc_live[o])
                        l += _snprintf_s(b + l, sizeof b - (size_t)l, _TRUNCATE, " %d:%s=%u/%u", o, krnl_name((unsigned)o),
                                         s_sc_spec[o], s_sc_live[o]);
                HLOG_I("LOCOWNPRED-SC: t=%ds Kernel Vorlauf/live:%s", waited_s, l ? b : " -");
            }
        }
        if (s_xc_orphan_stop) /* W375 (#51): eigene Zeile (geparste Zeilen unveraendert) */
            HLOG_I("LOCRBI-TEL3: t=%ds Ton verwaist=%u davon Schleife gestoppt=%u", waited_s, s_xc_orphan, s_xc_orphan_stop);
        if (s_rbs_side_stop || s_rbs_gap_stop || s_rbs_net_stop || s_rbs_rec_dead) /* W413 (#65): eigene Zeile */
            HLOG_I("LOCRBI-TEL5: t=%ds Ton-Abgleich nach dem Neurechnen: nachgeholte Waisen gestoppt=%u Stopp-Luecken gestoppt=%u "
                   "Sicherheitsnetz=%u Waisen tot markiert=%u", waited_s, s_rbs_side_stop, s_rbs_gap_stop, s_rbs_net_stop,
                   s_rbs_rec_dead);
        if (g_cfg.loc_sndloop_diag) { /* W413 DIAG: noch spielende Schleifen und Zaehler je Luecke (auch ohne Fix) */
            char b[600];
            int na = loc_audio_loops_active(b, (int)sizeof b);
            u32 tch = 0, i;
            for (i = 0; i < s_rbs_net_n; i++) tch += s_rbs_net[i].touched;
            HLOG_I("LOCSNDLOOP: t=%ds Fix=%d aktive Schleifen=%d [%s] | verwaist Aufzeichnung=%u (Schleife %u, gestoppt %u, tot %u) "
                   "nachgeholt=%u (Schleife %u, gestoppt %u) | Stopp-Luecken=%u (Schleife %u, gestoppt %u) | Netz verfolgt=%u "
                   "beruehrt=%u gestoppt=%u wuerde=%u voll=%u Ablage-leer-unberuehrt=%u | Vorlauf-Starts unterdrueckt=%u",
                   waited_s, g_cfg.loc_sndloop_fix, na, b, s_xc_orphan, s_rbs_rec_loop, s_xc_orphan_stop, s_rbs_rec_dead,
                   s_rbs_side_orphan, s_rbs_side_loop, s_rbs_side_stop, s_rbs_gap, s_rbs_gap_loop, s_rbs_gap_stop, s_rbs_net_n,
                   tch, s_rbs_net_stop, s_rbs_net_would, s_rbs_net_drop, s_rbs_net_umiss, s_spec_ev.xcue);
        }
        if (s_xc_stop_live) /* W381 (#61): eigene Zeile */
            HLOG_I("LOCRBI-TEL4: t=%ds Ton-Stopps beim Neurechnen live=%u (Schluessel %s)", waited_s, s_xc_stop_live,
                   s_xkey_legacy ? "alt" : "mit Handle/Quelle");
        if (s_rbc) { /* W355: Zusatzzeile (nur Zahlen) */
            const rbi_stats_t *w = rbi_stats(s_rbc);
            HLOG_I("LOCRBI-TEL2: t=%ds Senden gleich=%u live=%u | Neuaufbau aufgeschoben=%u davon Neurechnung=%u erzwungen=%u "
                   "Rollback ab Allokation=%u | verloren=%u hingenommen=%u | Hash verglichen=%u abweichend=%u erster=%d "
                   "Neurechnung-anders=%u | Allokation aufgez.=%u bedient=%u fehlt=%u ungueltig=%u fremd=%u neu=%u | rbtest %u/%u | "
                   "Senden Neurechnen %u exakt=%u ohne-Socket=%u andere-Laenge=%u | Datagramme Neurechnen %u gleich=%u | "
                   "Commit aufgez.=%u bedient=%u fehlt=%u neu=%u offen=%u voll=%u gleich=%u verworfen=%u | "
                   "Heap-Pruefung %u Knoten max=%u unvollst.=%u Zeit max=%u us Fehler=%u Ende=%u ausserhalb=%u belegt=%u",
                   waited_s, s_rbi_send_same, s_rbi_send_live, w->rebuild_deferred, w->rebuild_defer_replay,
                   w->rebuild_forced, w->rb_floored, w->rb_lost, w->rb_accepted, w->hash_cmp, w->hash_bad, w->hash_bad_first,
                   w->hash_recalc_diff, s_rbi_alloc_rec, s_rbi_alloc_rep, s_rbi_alloc_miss, s_rbi_alloc_nocommit,
                   s_rbi_alloc_other, s_rbi_alloc_new, s_rbi_rbtest_ok, s_rbi_rbtest_cnt, s_rbi_send_rep, s_rbi_send_exact,
                   s_rbi_send_nosock, s_rbi_send_otherlen, s_rbi_sendto_rep, s_rbi_sendto_same, s_rbi_com_rec, s_rbi_com_rep, s_rbi_com_miss, s_rbi_com_new,
                   s_pcom_n, s_pcom_full, s_rbi_com_same, s_rbi_com_drop, s_rbi_hc_n, s_rbi_hc_nodes_max, s_rbi_hc_part,
                   s_rbi_hc_us_max, s_rbi_hc_bad, s_rbi_end_heap, s_rbi_hc_vq, s_rbi_hc_busy);
            rbv_log(waited_s); /* W359 DIAG */
        }
        s_rbi_lt_max = 0;
    }
    if (!s_rbi_r2_missions) return;
    st = rbi_stats(s_rbc);
    rbi_pct(s_rbi_s_ticks, &d50, &d95, &dmx);
    rbi_pct(s_rbi_s_slots, &s50, &s95, &smx);
    rbi_pct(s_rbi_s_rep, &r50, &r95, &rmx);
    rbi_pct(s_rbi_s_load, &l50, &l95, &lmx);
    rbw = s_rbi_rb_n - s_rbi_rb_last;
    s_rbi_rb_last = s_rbi_rb_n;
    HLOG_I("LOCRBI: t=%ds Mission=%u aktiv=%d Slot=%d Verz=%d P0=%d | Entnahmen=%u bestaetigt=%u Vorhersagen=%u richtig=%u "
           "falsch=%u | Rollbacks=%u +%u/10s Tiefe p50/p95/max=%u/%u/%u Ticks %u/%u/%u Slots Neurechnen p50/p95/max=%u/%u/%u us "
           "Laden=%u/%u/%u us verloren=%u hingenommen=%u Ladefehler=%u Ruhe-Fehler=%u Barrieren=%u | Stau Erst=%u Fenster=%u Tiefe=%u | "
           "Hash endgueltig=%u verglichen=%u abweichend=%u erster=%d Neurechnung-anders=%u | Kanal tx=%u/%u rx=%u/%u "
           "doppelt=%u schlecht=%u fremd=%u Bindung=%u Epoche=%u Konflikt=%u Sendefehler=%u Warteschlange-voll=%ld | Zeitabgleich=%.2f "
           "Wartezeiten=%u/%u ms | Aufzeichnung=%u KB Kompakt=%u Aufruf-Abw=%u Kanalverlust-Test=%u",
           waited_s, s_rbi_epoch, s_rbi_ep, rbi_cons(s_rbc, rbi_self(s_rbc)), rbi_delay_eff(s_rbc), rbi_p0(s_rbc), st->taken,
           st->taken_conf, st->predicted, st->pred_ok, st->pred_bad, s_rbi_rb_n, rbw, d50, d95, dmx, s50, s95, smx, r50, r95,
           rmx, l50, l95, lmx, st->rb_lost, st->rb_accepted, s_rbi_load_fail, s_rbi_qfail, s_rbi_bar, st->stall_first, st->stall_window,
           st->stall_depth, st->hash_final, st->hash_cmp, st->hash_bad, st->hash_bad_first, st->hash_recalc_diff,
           st->tx_pkts, st->tx_frames, st->rx_ok, st->rx_frames, st->rx_dup, st->rx_bad, s_rbi_peer_bad, st->rx_bind,
           st->rx_epoch, st->rx_conflict, s_rbi_tx_err, s_rbi_tq_drop, rbi_advantage(s_rbc), s_rbi_ts_waits, s_rbi_ts_ms, s_rec_len / 1024u,
           s_rbi_compact, s_rep_div, s_rbi_chanloss_n);
    { /* W372: Zeitleiste und Puffer im 10-s-Fenster */
        u32 wh[RBI_SLH], wn = st->slack_n - s_tim_sln, wl = st->slack_late - s_tim_sll;
        int k, smed = 0;
        for (k = 0; k < (int)RBI_SLH; k++) {
            wh[k] = st->slack_h[k] - s_tim_slh[k];
            s_tim_slh[k] = st->slack_h[k];
        }
        s_tim_sln = st->slack_n;
        s_tim_sll = st->slack_late;
        HLOG_I("LOCRBI-TIM: t=%ds Puffer ms p5/p25/p50/p95=%d/%d/%d/%d spaet=%u von %u (%.1f%%) gesamt p5/p50=%d/%d "
               "spaet=%u von %u | Tick ms nach Feldwechsel: eigener Rahmen p50/p95=%d/%d Fremd-Entnahme p50/p95=%d/%d "
               "eigener vor Entnahme=%u von %u | Rahmen->Senden ms p50/p95=%d/%d | Vorsprung min/mittel/max=%.2f/%.2f/%.2f "
               "Stau-Ticks Zeitabgleich=%u | Zeitabgleich %d: Vorsprung %.1f ms (Quelle %d) Feld+ %u mal/%u Felder (Thunk %u/%u) "
               "Puffer-Median %d ms SBRT tx/rx/schlecht=%u/%u/%u | Feldbezug nach Neurechnen anders %u von %u (%s)",
               waited_s, rbi_slack_pct(wh, wn, 5), rbi_slack_pct(wh, wn, 25), rbi_slack_pct(wh, wn, 50),
               rbi_slack_pct(wh, wn, 95), wl, wn, wn ? 100.0 * wl / wn : 0.0, rbi_slack_pct(st->slack_h, st->slack_n, 5),
               rbi_slack_pct(st->slack_h, st->slack_n, 50), st->slack_late, st->slack_n, tim_pct(s_tim_h_own, 50),
               tim_pct(s_tim_h_own, 95), tim_pct(s_tim_h_take, 50), tim_pct(s_tim_h_take, 95), s_tim_own_first, s_tim_ticks,
               tim_pct(s_tim_h_send, 50), tim_pct(s_tim_h_send, 95), s_tim_adv_min,
               s_tim_adv_n ? s_tim_adv_sum / s_tim_adv_n : 0.0, s_tim_adv_max, s_rbi_ts_waits, s_rbi_tsync,
               rbi_tsync_ms(s_rbc), rbi_tsync_src(s_rbc), st->ts_actions, st->ts_fields, s_rbi_wait_ext_n, s_rbi_wait_ext_f,
               rbi_slack_med(s_rbc, rbi_self(s_rbc) == 0 ? 1 : 0, &smed) ? smed / 10 : -999, st->tt_tx, st->tt_rx, st->tt_bad,
               s_rbi_fld_diff, s_rbi_fld_n, s_rbi_fldfix ? "zurueckgesetzt" : "nur gezaehlt");
        /* W386: Abtastung der eigenen Eingabe (nur Zeiten): SBC-Abfrage nach Feldwechsel, Abfrage -> eigener Rahmen
         * (Rahmen -> Senden steht in LOCRBI-TIM), Ticks ohne bzw. mit mehreren Abfragen, Abfrage im Vortick. */
        HLOG_I("LOCRBI-LAT: t=%ds SBC-Abfrage ms nach Feldwechsel p50/p95=%d/%d | Abfrage->eigener Rahmen ms p50/p95/max=%d/%d/%d | "
               "Ticks=%u ohne Abfrage=%u mehrere=%u Abfrage im Vortick=%u | D=%u",
               waited_s, tim_pct(s_lat_h_xin, 50), tim_pct(s_lat_h_xin, 95), tim_pct(s_lat_h_x2o, 50),
               tim_pct(s_lat_h_x2o, 95), tim_pct(s_lat_h_x2o, 100), s_lat_ticks, s_lat_none, s_lat_multi, s_lat_before,
               s_rbi_ep ? s_rbi_reg_d : 0u);
        memset(s_lat_h_xin, 0, sizeof s_lat_h_xin);
        memset(s_lat_h_x2o, 0, sizeof s_lat_h_x2o);
        s_lat_ticks = s_lat_none = s_lat_multi = s_lat_before = 0;
        memset(s_tim_h_own, 0, sizeof s_tim_h_own);
        memset(s_tim_h_take, 0, sizeof s_tim_h_take);
        memset(s_tim_h_send, 0, sizeof s_tim_h_send);
        s_tim_own_first = s_tim_ticks = 0;
        s_tim_adv_n = 0;
        s_tim_adv_sum = 0;
    }
    HLOG_I("LOCRBI: DIAG Startpunkt %d/%d (Wahl %d/%d) Zaehler %u Aktiv 0x%X Spieltick %u Stau %u | eigene Rahmen %u (doppelt %u, "
           "mehrere je Tick %u) Flags eigen 0x%X fremd 0x%X Entnahmezaehler %u/%u", (int)rd32(0x5C5A44u),
           (int)rd32(0x5C5A44u + 0xAE8u), (int)rd32(0x5C5A48u), (int)rd32(0x5C5A48u + 0xAE8u), rd32(0x5C5A40u),
           rd32(RB_ACTIVE), rd32(RB_TICK), rd32(RB_STALL), st->own_frames, st->own_dup, s_rbi_dmulti, s_rbi_dflags[0],
           s_rbi_dflags[1], rd32(RBI_TAKENW) & 0xFFFFu, rd32(RBI_TAKENW) >> 16);
    HLOG_I("LOCRBI: DIAG Eingabestrom je Spieler (kein Zustandshash; Rechner vergleichen: gleiche Werte = gleiche "
           "Eingaben) p0 %u/%08X p1 %u/%08X | Spieler0 522=%u 98=0x%X 8c=%u 92=%u | Spieler1 522=%u 98=0x%X 8c=%u 92=%u | "
           "lokal %u netz %u", s_rbi_dn[0], s_rbi_dstream[0], s_rbi_dn[1], s_rbi_dstream[1],
           rd32(0x5C50D0u + 0x522u) & 0xFFu, rd32(0x5C50D0u + 0x98u), rd32(0x5C50D0u + 0x8Cu) & 0xFFFFu,
           rd32(0x5C50D0u + 0x92u) & 0xFFFFu, rd32(0x5C50D0u + 0xAE8u + 0x522u) & 0xFFu, rd32(0x5C50D0u + 0xAE8u + 0x98u),
           rd32(0x5C50D0u + 0xAE8u + 0x8Cu) & 0xFFFFu, rd32(0x5C50D0u + 0xAE8u + 0x92u) & 0xFFFFu, rd32(0x306A84u), rd32(RBI_SELF));
    {
        int k;
        for (k = 0; k < 10; k++)
            if (s_rbi_dchk[0][k] || s_rbi_dchk[1][k])
                HLOG_I("LOCRBI: DIAG Eingabestrom bis Slot %d Spieler0=%08X Spieler1=%08X Zaehler %u Spieltick %u", (k + 1) * 25, s_rbi_dchk[0][k],
                       s_rbi_dchk[1][k], s_rbi_dcnt[k] & 0xFFFFu, s_rbi_dcnt[k] >> 16);
    }
    (void)s_rbi_win_sum;
    (void)s_rbi_win_n;
}

static u32 rb_rep_limit(void) { return s_rbi_ep ? s_rep_end : s_rec_len; }
static int rb_r2_acc(void)
{
    u32 acc;
    if (!s_rbi_ep) return 0;
    acc = rd32(RB_ACC);
    if (s_rbi_force_desync >= 0 && rbi_cons(s_rbc, rbi_self(s_rbc)) == s_rbi_force_desync + 1)
        acc ^= 0x5A5A5A5Au; /* TEST desync=N: nur der gemeldete Hash dieses Slots, Spielzustand unveraendert */
    rbi_acc(s_rbc, acc, rd32(RB_TICK));
    return 1;
}

/* Aufruf nach jedem Feld-Warten (bzw. statt dessen im Neurechnen), Spiel-Thread, Register gesichert. */
/* W330: Bereiche geaendert (Allokation/Freigabe/Schutzwechsel)? Laufenden Zyklus verwerfen, Ring neu. */
static int rb_ring_check_regions(void)
{
    if (g_exreg_n == s_reg_gen && g_loc_rb_memev == s_memev_seen && !s_r_force_rebuild) return 0;
    s_r_force_rebuild = 0;
    if (s_phase != PH_IDLE && !s_rbi_ep) { /* W338: R2 behaelt Neurechnung/Phase, Barriere setzt der Aufrufer */ /* Zyklus abbrechen: Zeitlinie B wiederherstellen (alte Bereiche) */
        if (s_phase == PH_REPLAY) {
            __try { (void)rb_diff(s_snapB, 1, NULL, 0); } __except (EXCEPTION_EXECUTE_HANDLER) {}
            if (s_freeze) rb_thaw_others();
            rb_rs_resume(); /* W411: nur, wenn gebucht */
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
    /* W338: Andere Threads (Render-Thread, Lade-Threads 0x28DE0 ueber ReadFile -> rbr_prewrite) duerfen waehrend
     * Stop/Aufbau weiterlaufen: rbr_stop wartet, bis kein Handler/Prewrite mehr in den alten Puffern arbeitet (A10-A12:
     * Haenger bzw. harter Abbruch nach dem Neuaufbau beim ersten Schuss). Kein Anhalten - ein mitten im Handler
     * angehaltener Thread wuerde das Warten blockieren. */
    rb_build_regions();
    if (s_avlog) { /* DIAG: ab jetzt protokollieren (vorgeschaltet, auch vor dem Ring-Handler) */
        static PVOID hv;
        if (hv) RemoveVectoredExceptionHandler(hv);
        hv = AddVectoredExceptionHandler(0, rb_avlog_veh); /* zuletzt: nur, was kein anderer Handler erledigt */
        InterlockedExchange(&s_avlog_n, 0);
        InterlockedExchange(&s_avlog_arm, 1);
    }
    if (s_mode == 2 && !rb_alloc_snaps(1)) s_failed = 1;
    return 1;
}

/* W411 TEST tend=N (loc_rb_opts, Standard aus): ab Spieltick N einmal das Missionsende des Spiels ausloesen wie bei
 * Rauswurf/Abbruch: FUN_00029C60(9, 0) = Zaehler 1, Grenze 0 -> die Ende-Pruefung nach diesem rb_after verlaesst die
 * Schleife. tendspec=1: im ersten Vorlauf-Tick ab N (verworfene Zeitlinie), sonst im Live-Tick. */
static void rb_test_end(const char *where)
{
    typedef u32(__cdecl * end_fn)(u32, u32);
    u32 r;
    s_test_end_done++;
    r = ((end_fn)(uintptr_t)0x29C60u)(9u, 0u);
    HLOG_W("LOCRB: TEST tend=%d: Missionsende ausgeloest (%s, Spieltick %u, Ergebnis %u, Zaehler %u/%u, Vorlauf %d, "
           "Render-Thread angehalten %ld)", s_test_end, where, rd32(RB_TICK), r, rd32(RB_END_CNT), rd32(RB_END_LIM), s_spec,
           s_rs_held);
}

static void __cdecl rb_after(void)
{
    { /* W390: Aufrufer des Feld-Wartens und Stapeltiefe (Thunk: pushad + pushfd + Ruecksprung = 40 Bytes) */
        const u32 *ra = (const u32 *)_AddressOfReturnAddress();
        s_rb_esp0 = (u32)(uintptr_t)(ra + 10);
        s_rb_caller = ra[10];
    }
    InterlockedIncrement(&s_after_seq); /* W411: Waechter (Spiel-Thread erreicht den Ruhepunkt) */
    if (s_spec && rd32(RB_TICK) < s_spec_gt0) /* W411: Vorlauf aus einer verlassenen Missionsschleife, neue Mission */
        rb_spec_abandon("neue Mission");
    if (s_spec && s_test_end >= 0 && s_test_end_spec && !s_test_end_done && rd32(RB_TICK) >= (u32)s_test_end &&
        s_spec_n + 1 < s_spec_want) /* nicht im letzten Vorlauf-Tick (dort setzt der Vorlauf ohnehin zurueck) */
        rb_test_end("Vorlauf");
    if (s_spec) { /* W390: Tick des spekulativen Vorlaufs fertig */
        if (s_rb_esp0 != s_spec_esp0 || s_rb_caller != s_spec_caller) { /* andere Schleife/Stapeltiefe als beim Start */
            if (!s_spec_ctx_bad++)
                HLOG_W("LOCOWNPRED: Vorlauf kam von anderer Stelle zurueck (Aufrufer 0x%X statt 0x%X, Stapel 0x%X statt 0x%X) - "
                       "Vorlauf fuer diese Mission aus", s_rb_caller, s_spec_caller, s_rb_esp0, s_spec_esp0);
            s_spec_off_mission = 1;
            memset(s_spec_pose_ok, 0, sizeof s_spec_pose_ok); /* Lagen nicht anzeigen */
            rb_spec_finish();
            return;
        }
        rb_spec_tick();
        return;
    }
    if (s_wait_extra && !s_skipwait) { /* W372: verlaengertes Feld-Warten ist vorbei (im Neurechnen: noch offen) */
        s_rbi_wait_ext_n++;
        s_rbi_wait_ext_f += (u32)s_wait_extra;
        InterlockedExchange(&s_wait_extra, 0);
    }
    if (s_rbi_ep && s_phase != PH_REPLAY) { /* W372: Feldwechsel = Beginn des naechsten Live-Ticks */
        s_tim_t0 = rbi_us_now();
        s_tim_own_seen = s_tim_take_seen = 0;
        s_tim_ticks++;
        { /* W386: SBC-Abfragen im vergangenen Tick */
            u32 nx = s_lat_xin_n - s_lat_xin_tick0;
            s_lat_xin_tick0 = s_lat_xin_n;
            s_lat_ticks++;
            if (!nx) s_lat_none++;
            else if (nx > 1u) s_lat_multi++;
        }
        if (rbi_active(s_rbc)) {
            double a = rbi_advantage(s_rbc);
            if (!s_tim_adv_n || a < s_tim_adv_min) s_tim_adv_min = a;
            if (!s_tim_adv_n || a > s_tim_adv_max) s_tim_adv_max = a;
            s_tim_adv_sum += a;
            s_tim_adv_n++;
        }
    }
    if (s_rbi_mst == RBN_ST_TENT || s_rbi_mst == RBN_ST_PEND) rbi_mission_poll(); /* W346: Aushandlung je Tick */
    if (s_rbi_ep) { /* W346 DIAG: langer Tick (Abstand zweier Ruhepunkte) */
        long long t = rb_now();
        if (s_rbi_lt_last) {
            u32 ms = rb_us(t - s_rbi_lt_last) / 1000u;
            if (ms > s_rbi_lt_max) s_rbi_lt_max = ms;
            if (ms >= 300u) {
                s_rbi_lt_n++;
                if (s_rbi_lt_logged++ < 20)
                    HLOG_W("LOCRBI: langer Tick %u ms bei Slot %d (Neurechnen=%d, Rollbacks bisher %u, Stau %u)", ms,
                           rbi_cons(s_rbc, rbi_self(s_rbc)), rbi_replaying(s_rbc), s_rbi_rb_n, rd32(RB_STALL));
            }
        }
        s_rbi_lt_last = t;
    } else {
        s_rbi_lt_last = 0;
    }
    if (!rd32(RB_ACTIVE) && !s_rbi_ep) { /* W338: mit R2 laeuft der Lockstep auch bei Aktiv-Maske 0 weiter (VT
                                           * absetzen nach dem Startzaehler); eine neue Mission erkennt rbi_ec80 */
        s_lst_frames = 0;
        return;
    }
    if (s_rbi && !s_rbi_ep) { /* W346: Lockstep-Mission (ausgehandelt) bzw. Aushandlung: kein Ring, keine Aufzeichnung */
        s_lst_frames = 0;
        return;
    }
    if (s_failed) {
        if (s_rbi_ep) rbi_after_noring(); /* W338: ohne Ring keine Vorhersage, Kanal laeuft weiter */
        return;
    }
    if (++s_lst_frames < (unsigned)s_warm && s_phase == PH_IDLE) {
        if (s_rbi_ep) rbi_after_noring();
        return;
    }
    if (s_ready && s_rbi_ep) { /* W338 R2: eigener Ablauf am Ruhepunkt */
        if (s_test_end >= 0 && !s_test_end_spec && s_test_end_done < 5 && s_phase != PH_REPLAY &&
            rd32(RB_TICK) >= (u32)s_test_end && rd32(RB_END_CNT) == 0)
            rb_test_end("Live-Tick"); /* W411 TEST (erneut, falls ein Rollback den Stand davor geladen hat) */
        if (s_phase == PH_IDLE && s_rbi_rec) s_phase = PH_RECORD;
        rbi_after();
        return;
    }
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
            if (!s_rbi_acc_chained) ok &= rb_chain_call(RB_ACC_CALL, (void *)rb_acc_thunk, &s_acc_next, "Akkumulator");
            if (s_xact && (s_mode == 2 || s_rbi)) { /* W330 xact=1; W338: auch fuer R2 (Standard dort 1) */
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
                if (q) rb_rs_resume(); /* W411 */
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
        if (s_rbi_ep) { /* W338: ab hier Aufzeichnung + Ring; dieser Tick ohne Commit (Ring frisch) */
            s_rec_len = s_rec_cur = 0;
            if (s_rbi_rec) s_phase = PH_RECORD;
            rbi_after_noring();
        }
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

/* W376 LOC-LOCAL-AIM: vor dem Feld-Warten (Tick fertig simuliert, Render-Vorbereitung abgegeben). Der Render-Thread
 * zeichnet diesen Zustand waehrend des Wartens; die Vorhersage liegt so vor seinem Swap bereit (Messlauf W376_A5: nach
 * dem Warten veroeffentlicht zeigte Liste N schon den Zustand N+1). */
static void __cdecl rb_before(void)
{
    loc_aim_game_tick(s_phase == PH_REPLAY, (s_rbi_ep && s_rbc) ? rbi_cons(s_rbc, rbi_self(s_rbc)) : -1,
                      (s_rbi_ep && s_rbc && rbi_active(s_rbc) && !rbi_replaying(s_rbc)) ? (const void *)s_rbc : NULL);
    if (s_rbi_ep && s_rbc && rbi_active(s_rbc) && !rbi_replaying(s_rbc) && s_phase != PH_REPLAY)
        loc_smooth_tick(rd32(RB_TICK)); /* W385: Lage + Versatz fuer die Liste dieses Ticks */
}

static __declspec(naked) void rb_thunk(void)
{
    __asm {
        cmp dword ptr [s_skipwait], 0
        jne rb_skip
        pushad
        pushfd
        call rb_before
        popfd
        popad
        mov eax, dword ptr [esp + 4]
        add eax, dword ptr [s_wait_extra] /* W372: Zeitabgleich verlaengert das Feld-Warten */
        push eax
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

/* ================= W411: Haken am Ende der Missionsschleife =================
 * FUN_0002F100 @0x2F352: pop esi; pop ebp; pop ebx; dann "warte bis [0x306EEC] == 0" (@0x2F355) und Auftrag 2 (Ende)
 * @0x2F35E. Der Haken laeuft auf dem Spiel-Thread, bevor er auf den Render-Thread wartet: Vorlauf/Neurechnen, die die
 * Schleife verlassen haben, werden hier beendet und ein noch gebuchtes Anhalten des Render-Threads freigegeben. */
#define RB_LX_VA 0x2F352u
static const u32 s_lx_rjob = RB_RJOB;
static const u32 s_lx_ret = 0x2F35Eu;
static void __cdecl rb_loop_exit(void)
{
    LONG held = s_rs_held, why = s_rs_why;
    int spec = s_spec, rep = s_rbi_ep && s_rbc && rbi_replaying(s_rbc) && !s_rep_ended;
    s_lx_n++;
    if (spec) {
        s_lx_spec++;
        rb_spec_abandon("Missionsschleife im Vorlauf verlassen");
    }
    if (rep) {
        s_lx_rep++;
        s_rep_ended = 1;
        HLOG_W("LOCRBI: Missionsschleife im Neurechnen verlassen (Spieltick %u) - Neurechnung beendet, Threads fortgesetzt",
               rd32(RB_TICK));
        rbi_replay_finish();
    }
    if (s_rs_held) {
        s_lx_held++;
        HLOG_W("LOCRB: Render-Thread beim Verlassen der Missionsschleife noch angehalten (%s) - fortgesetzt",
               rb_rs_why_name(s_rs_why));
        rb_rs_resume();
    }
    if (s_lx_n <= 8 || spec || rep || held)
        HLOG_I("LOCRB: Missionsschleife verlassen (Spieltick %u, Ende-Zaehler %u/%u, Render-Auftrag %u) | vorher: Vorlauf=%d "
               "Neurechnen=%d Render-Thread angehalten=%ld (%s)",
               rd32(RB_TICK), rd32(RB_END_CNT), rd32(RB_END_LIM), rd32(RB_RJOB), spec, rep, held, rb_rs_why_name(why));
}
static __declspec(naked) void rb_exit_thunk(void)
{
    __asm {
        pushad
        pushfd
        call rb_loop_exit
        popfd
        popad
        pop esi /* ersetzte Originalbefehle @0x2F352 */
        pop ebp
        pop ebx
    lx_spin: /* Original @0x2F355: warten, bis der Render-Thread den Auftrag erledigt hat */
        mov eax, dword ptr [s_lx_rjob]
        mov eax, dword ptr [eax]
        test eax, eax
        jne lx_spin
        jmp dword ptr [s_lx_ret]
    }
}
static int rb_exit_install(void)
{
    static const uint8_t expect[23] = {0x5E, 0x5D, 0x5B, 0xA1, 0xEC, 0x6E, 0x30, 0x00, 0x85, 0xC0, 0x75, 0xF7,
                                       0xC7, 0x05, 0xEC, 0x6E, 0x30, 0x00, 0x02, 0x00, 0x00, 0x00, 0xC3};
    uint8_t *p = (uint8_t *)(uintptr_t)RB_LX_VA;
    intptr_t rel;
    DWORD old;
    if (memcmp(p, expect, sizeof expect) != 0) {
        HLOG_W("LOCRB: Schleifenende @0x%X: Bytes passen nicht - Haken aus", RB_LX_VA);
        return 0;
    }
    rel = (uint8_t *)(void *)rb_exit_thunk - (p + 5);
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    p[0] = 0xE9;
    memcpy(p + 1, &rel, 4);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return 1;
}

/* W411 Waechter: Haelt der Rollback den Render-Thread laenger als sguard ms angehalten und erreicht der Spiel-Thread so
 * lange keinen Ruhepunkt mehr (rb_after), dann Logzeile und fortsetzen. Ein lebender Vorlauf/Neurechnen ruft rb_after
 * je Tick (ms); 2 s ohne Ruhepunkt bei angehaltenem Render-Thread ist immer ein Haenger. */
static DWORD WINAPI rb_guard_thread(LPVOID unused)
{
    LONG seq_last = -1;
    DWORD seq_t = GetTickCount();
    (void)unused;
    for (;;) {
        DWORD now, age;
        LONG seq;
        Sleep(200);
        now = GetTickCount();
        seq = s_after_seq;
        if (seq != seq_last) {
            seq_last = seq;
            seq_t = now;
        }
        if (!s_rs_held) continue;
        age = now - (DWORD)s_rs_t0;
        if (age < (DWORD)s_sguard_ms || now - seq_t < (DWORD)s_sguard_ms) continue;
        if (!InterlockedExchange(&s_rs_held, 0)) continue; /* Spiel-Thread war schneller */
        {
            DWORD prev = ResumeThread(s_rs_h);
            s_rs_rescue++;
            HLOG_W("LOCRB: WAECHTER Render-Thread seit %lu ms von uns angehalten (%s), Spiel-Thread seit %lu ms ohne "
                   "Ruhepunkt (Spieltick %u, Phase %d, Vorlauf %d, Ende-Zaehler %u/%u) - fortgesetzt (vorher %lu)",
                   age, rb_rs_why_name(s_rs_why), now - seq_t, rd32(RB_TICK), s_phase, s_spec, rd32(RB_END_CNT),
                   rd32(RB_END_LIM), prev);
        }
    }
}

/* W411: Zustand fuer STALL-Zeilen und Haenger-Bericht (nur Zahlen). */
int loc_rb_susp_diag(char *buf, int cap)
{
    if (!buf || cap <= 0) return 0;
    _snprintf_s(buf, (size_t)cap, _TRUNCATE,
                "render_held=%ld held_ms=%lu why=%s phase=%d spec=%d replaying=%d end=%u/%u rjob=%u | suspends=%u "
                "double=%u swap=%u spurious=%u prev0=%u max_ms=%u guard_rescues=%u loop_exits=%u (spec %u replay %u held %u) "
                "spec_endstop=%u replay_endstop=%u abandon=%u",
                s_rs_held, s_rs_held ? GetTickCount() - (DWORD)s_rs_t0 : 0ul, rb_rs_why_name(s_rs_why), s_phase, s_spec,
                (s_rbi_ep && s_rbc) ? rbi_replaying(s_rbc) : 0, s_ready ? rd32(RB_END_CNT) : 0u,
                s_ready ? rd32(RB_END_LIM) : 0u, s_ready ? rd32(RB_RJOB) : 0u, s_rs_n, s_rs_double, s_rs_swap, s_rs_spur,
                s_rs_prev0, s_rs_max_ms, s_rs_rescue, s_lx_n, s_lx_spec, s_lx_rep, s_lx_held, s_spec_endstop,
                s_rep_endstop, s_spec_abandon_n);
    return s_ring;
}
void loc_rb_stall_diag(void)
{
    char b[600];
    if (!s_ring) return;
    loc_rb_susp_diag(b, (int)sizeof b);
    HLOG_W("LOCRB: STALL-Diag %s", b);
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
        else if (strncmp(p, "endguard=", 9) == 0) { s_endguard = atoi(p + 9); } /* W411: 0 = alte Wirkung (TEST) */
        else if (strncmp(p, "exithook=", 9) == 0) { s_exithook = atoi(p + 9); } /* W411 */
        else if (strncmp(p, "sguard=", 7) == 0) { s_sguard_ms = atoi(p + 7); }  /* W411: Waechter ms, 0 = aus */
        else if (strncmp(p, "tendspec=", 9) == 0) { s_test_end_spec = atoi(p + 9); } /* W411 TEST */
        else if (strncmp(p, "tend=", 5) == 0) { s_test_end = atoi(p + 5); }     /* W411 TEST */
        else if (strncmp(p, "ww=", 3) == 0) { s_ww = atoi(p + 3); }             /* W330 */
        else if (strncmp(p, "depth=", 6) == 0) { s_depth = atoi(p + 6); }
        else if (strncmp(p, "pool=", 5) == 0) { s_pool = atoi(p + 5); }
        else if (strncmp(p, "verify=", 7) == 0) { s_verify = atoi(p + 7); }
        else if (strncmp(p, "pdpause=", 8) == 0) { s_pdpause = atoi(p + 8); }
        else if (strncmp(p, "shadow=", 7) == 0) { s_shadowchk = atoi(p + 7); }
        else if (strncmp(p, "hot=", 4) == 0) { s_hot = atoi(p + 4); }
        else if (strncmp(p, "xact=", 5) == 0) { s_xact = atoi(p + 5); }
        else if (strncmp(p, "xkey=", 5) == 0) { s_xkey_legacy = atoi(p + 5) == 0; } /* W381 DIAG: xkey=0 = alter Schluessel */
        else if (strncmp(p, "cool=", 5) == 0) { s_cool = atoi(p + 5); }
        else if (strncmp(p, "maxpred=", 8) == 0) { s_rbi_maxpred = atoi(p + 8); } /* W338 */
        else if (strncmp(p, "pred=", 5) == 0) { s_rbi_pred = atoi(p + 5); }     /* W385 */
        else if (strncmp(p, "predq=", 6) == 0) { s_rbi_predq = atoi(p + 6); }   /* W385 */
        else if (strncmp(p, "lowd=", 5) == 0) { s_rbl_cfg.on = atoi(p + 5); }    /* W386 niedrige Verzoegerung */
        else if (strncmp(p, "lowmax=", 7) == 0) { s_rbl_cfg.lowmax = atoi(p + 7); }
        else if (strncmp(p, "lcpm=", 5) == 0) { s_rbl_cfg.lcpm = atoi(p + 5); }
        else if (strncmp(p, "hcpm=", 5) == 0) { s_rbl_cfg.hcpm = atoi(p + 5); }
        else if (strncmp(p, "ldep=", 5) == 0) { s_rbl_cfg.ldep = atoi(p + 5); }
        else if (strncmp(p, "hdep=", 5) == 0) { s_rbl_cfg.hdep = atoi(p + 5); }
        else if (strncmp(p, "lrp=", 4) == 0) { s_rbl_cfg.lrp = atoi(p + 4); }
        else if (strncmp(p, "hrp=", 4) == 0) { s_rbl_cfg.hrp = atoi(p + 4); }
        else if (strncmp(p, "lhang=", 6) == 0) { s_rbl_cfg.lhang = atoi(p + 6); }
        else if (strncmp(p, "hhang=", 6) == 0) { s_rbl_cfg.hhang = atoi(p + 6); }
        else if (strncmp(p, "hjump=", 6) == 0) { s_rbl_cfg.hjump = atoi(p + 6); }
        else if (strncmp(p, "hang6=", 6) == 0) { s_rbl_cfg.hang6 = atoi(p + 6); }
        else if (strncmp(p, "lgood=", 6) == 0) { s_rbl_cfg.good_n = atoi(p + 6); }
        else if (strncmp(p, "lcool=", 6) == 0) { s_rbl_cfg.cool_s = atoi(p + 6); }
        else if (strncmp(p, "lmaxup=", 7) == 0) { s_rbl_cfg.max_up = atoi(p + 7); }
        else if (strncmp(p, "llead=", 6) == 0) { s_rbl_cfg.lead = atoi(p + 6); }
        else if (strncmp(p, "ltest=", 6) == 0) { s_rbl_test = atoi(p + 6); }   /* W386 TEST: Wechsel alle N s erzwingen */
        else if (strncmp(p, "kstack=", 7) == 0) { s_kstack = atoi(p + 7); }
        else if (strncmp(p, "avlog=", 6) == 0) { s_avlog = atoi(p + 6); }
        else if (strncmp(p, "stkx=", 5) == 0) { s_stkx = atoi(p + 5); }   /* W362 DIAG */
        else if (strncmp(p, "abchk=", 6) == 0) { s_abchk = atoi(p + 6); } /* W362 DIAG */
        else if (strncmp(p, "rec=", 4) == 0) { s_rbi_rec = atoi(p + 4); }
        else if (strncmp(p, "chanloss=", 9) == 0) {
            if (strncmp(p + 9, "ge", 2) == 0) s_rbi_chanloss_ge = 1;
            else s_rbi_chanloss = atoi(p + 9);
        }
        else if (strncmp(p, "tsync=", 6) == 0) { s_rbi_tsync = atoi(p + 6); }
        else if (strncmp(p, "early=", 6) == 0) { s_rbi_early = atoi(p + 6); } /* W372 */
        else if (strncmp(p, "fldfix=", 7) == 0) { s_rbi_fldfix = atoi(p + 7); } /* W372 */
        else if (strncmp(p, "desync=", 7) == 0) { s_rbi_force_desync = atoi(p + 7); } /* W346 TEST */
        else if (strncmp(p, "sdump=", 6) == 0) { /* LOC-DESYNC-REPAIR DIAG: sdump=X[,S[,N]] */
            const char *c = strchr(p + 6, ','), *c2 = c ? strchr(c + 1, ',') : NULL, *e = strchr(p, ';');
            s_sd_x = atoi(p + 6);
            s_sd_n = 1;
            if (c && (!e || c < e)) s_sd_step = atoi(c + 1);
            if (c2 && (!e || c2 < e)) s_sd_n = atoi(c2 + 1);
            if (s_sd_n > 8) s_sd_n = 8;
        }
        else if (strncmp(p, "rbtest=", 7) == 0) { /* W355 TEST */
            const char *c = strchr(p + 7, ',');
            s_rbi_rbtest_n = atoi(p + 7);
            s_rbi_rbtest_k = c ? atoi(c + 1) : 4;
            if (s_rbi_rbtest_n < 0) s_rbi_rbtest_n = 0;
            if (s_rbi_rbtest_k < 1) s_rbi_rbtest_k = 1;
            if (s_rbi_rbtest_k > 30) s_rbi_rbtest_k = 30;
        }
        else if (strncmp(p, "defer=", 6) == 0) { s_rbi_defer_on = atoi(p + 6); }
        else if (strncmp(p, "rbver=", 6) == 0) { s_rbv_n = atoi(p + 6); } /* W359 DIAG */
        else if (strncmp(p, "memlog=", 7) == 0) { s_memlog = atoi(p + 7); } /* W359 DIAG */
        else if (strncmp(p, "rprep=", 6) == 0) { s_rbi_prep = atoi(p + 6); } /* W359 */
        else if (strncmp(p, "heapchk=", 8) == 0) { s_rbi_heapchk = atoi(p + 8); } /* W359 */
        else if (strncmp(p, "csn=", 4) == 0) { /* W359 DIAG */
            s_csn = atoi(p + 4);
            g_loc_cs_note = s_csn ? rb_cs_note : NULL;
        }
        else if (strncmp(p, "dmin=", 5) == 0) { s_rbi_dmin = atoi(p + 5); }           /* W346 */
        else if (strncmp(p, "dmax=", 5) == 0) { s_rbi_dmax = atoi(p + 5); }
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
    if (g_cfg.loc_rb_spike <= 0 && g_cfg.loc_rb <= 0 && g_cfg.loc_rb_input <= 0) return 0;
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
    s_ring = g_cfg.loc_rb > 0 || g_cfg.loc_rb_input > 0; /* W338: R2 braucht den Ring */
    if (g_cfg.loc_rb_input > 0) { /* W338: Standardwerte fuer R2 (loc_rb_opts kann sie aendern) */
        if (s_mode) HLOG_W("LOCRB: loc_rb_spike und loc_rb_input schliessen sich aus - nur R2");
        s_mode = 0;
        /* Ring erst 300 Lockstep-Ticks nach Missionsbeginn (wie W330): mit fruehem Start (warm=1) endete der Prozess
         * hart beim Missionsuebergang bzw. Neuaufbau (C1-C5, auch ohne R2). Bis dahin laeuft R2 ohne Vorhersage. */
        s_warm = 300;
        s_xact = 1;
        s_kstack = 0;
        s_rbi_delay = g_cfg.loc_rb_delay;
    }
    if (s_ring && s_mode == 1) {
        HLOG_W("LOCRB: loc_rb_spike=1 (Vergleich je Tick) und loc_rb=1 schliessen sich aus - nur Ring");
        s_mode = 0;
    }
    rbl_cfg_default(&s_rbl_cfg); /* W386: Standard niedrige Verzoegerung (loc_rb_opts lowd=0 schaltet ab) */
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
    if (s_avlog) {
        char fn[600];
        const char *od = hcfg_out_dir();
        _snprintf_s(fn, sizeof fn, _TRUNCATE, "%s\\rb_avlog.txt", od && od[0] ? od : ".");
        s_avlog_h = CreateFileA(fn, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    if (g_cfg.loc_rb_input > 0) {
        if (s_rbi_delay < 0) s_rbi_delay = 0; /* W346: 0 = adaptiv nach Umlaufzeit (dmin..dmax) */
        if (s_rbi_delay > 8) s_rbi_delay = 8;
        if (s_rbi_dmin < 1) s_rbi_dmin = 1;
        if (s_rbi_dmax > 8) s_rbi_dmax = 8;
        if (s_rbi_dmax < s_rbi_dmin) s_rbi_dmax = s_rbi_dmin;
        if (s_rbi_maxpred < 1) s_rbi_maxpred = 1;
        if (s_rbi_maxpred > s_depth - 4) s_rbi_maxpred = s_depth - 4;
        rbi_install(); /* W338 */
    }
    if (s_ring) { /* W411 LOC-FREEZE-SUSP: Schleifenende-Haken + Waechter */
        int lx = s_exithook ? rb_exit_install() : 0;
        if (s_sguard_ms > 0) {
            HANDLE th = CreateThread(NULL, 64 * 1024, rb_guard_thread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
            if (th) CloseHandle(th);
            else s_sguard_ms = 0;
        }
        HLOG_I("LOCRB: W411 Render-Thread-Schutz: Ende-Pruefung=%d Schleifenende-Haken=%d Waechter=%d ms%s", s_endguard, lx,
               s_sguard_ms, s_test_end >= 0 ? " | TEST tend aktiv" : "");
    }
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
    if (s_rbi) rbi_log(waited_s); /* W338 */
    if (!s_ready) return;
    if (s_ring) rb_ring_log(waited_s);
    if (s_ring) { /* W411: Buchfuehrung Render-Thread (alle 60 s bzw. bei neuen Auffaelligkeiten) */
        static u32 last;
        u32 now = s_rs_double + s_rs_swap + s_rs_prev0 + s_rs_rescue + s_lx_spec + s_lx_rep + s_lx_held + s_spec_endstop +
                  s_rep_endstop + s_spec_abandon_n + s_lx_n;
        if (waited_s % 60 == 0 || now != last) {
            char b[600];
            last = now;
            loc_rb_susp_diag(b, (int)sizeof b);
            HLOG_I("LOCRB-SUSP: t=%ds %s | Vorlauf ausgelassen Missionsende=%u", waited_s, b, s_spec_skip[7]);
        }
    }
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
        if (s_mode == 2 || s_rbi) { /* W338: auch R2 (Aufzeichnung/Wiedergabe beim Neurechnen) */
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
            if (s_mode == 2) rb_census_log();
        }
    }
}

/* W353 LOC-DURCHSTICH: Rollback-Zustand fuer das F11-Paket (sb_feedback.c), nur Zahlen/Zustaende, keine Eingaben. */
int loc_rb_diag(char *buf, int cap)
{
    if (!buf || cap <= 0) return 0;
    buf[0] = 0;
    if (!g_cfg.loc_rb_input) {
        _snprintf_s(buf, (size_t)cap, _TRUNCATE, "input_layer=off (original lockstep)");
        return 0;
    }
    _snprintf_s(buf, (size_t)cap, _TRUNCATE,
                "input_layer=on mode=%s delay=%d rollbacks=%u missions_r2=%u missions_lockstep=%u last_fallback=%u "
                "end_desync=%u end_disagree=%u last_fallback_reason=%s%s%s peers=%d",
                s_rbi_mst == RBN_ST_R2 ? (s_rbc && rbi_free_running(s_rbc) ? "r2-end" : "r2")
                                       : (s_rbi_mst == RBN_ST_LS ? "lockstep"
                                                                 : (s_rbi_mst == RBN_ST_TENT ? "offer"
                                                                                             : (s_rbi_mst == RBN_ST_PEND ? "wait" : "lobby"))),
                s_rbi_ep ? (int)s_rbi_reg_d : 0, s_rbi_rb_n, s_rbi_r2_missions, s_rbi_ls_missions, s_rbi_last_why,
                s_rbi_end_desync, s_rbi_end_disagree, s_rbi_last_why < RBN_WHY_N ? k_rbn_why_slug[s_rbi_last_why] : "?",
                s_rbi_last_host_why ? "/host:" : "",
                s_rbi_last_host_why && s_rbi_last_host_why < RBN_WHY_N ? k_rbn_why_slug[s_rbi_last_host_why] : "",
                s_rbi_npeers);
    return 1;
}

/* W374 LOC-NET-OVERLAY (F3): Zaehler fuer das Netz-Overlay (Abtast-Thread des Overlays, hoechstens 2x/s). */
static int rbi_hud_ucmp(const void *a, const void *b)
{
    u32 x = *(const u32 *)a, y = *(const u32 *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

int loc_rb_eff_delay(void)
{
    if (!g_cfg.loc_rb_input || !s_rbi || !s_rbi_ep || s_rbi_mst != RBN_ST_R2) return 0;
    return (int)s_rbi_reg_d;
}

int loc_rb_hud_read(loc_rb_hud *o)
{
    u32 now = GetTickCount(), tmp[RBI_HUD_N];
    LONG n, i, k = 0;
    int rk;
    if (!o) return 0;
    memset(o, 0, sizeof *o);
    o->npeers = -1;
    if (!g_cfg.loc_rb_input || !s_rbi) return 0;
    o->mode = s_rbi_mst == RBN_ST_R2 ? (s_rbc && rbi_free_running(s_rbc) ? 3 : 2)
                                     : (s_rbi_mst == RBN_ST_LS ? 1 : (s_rbi_mst == RBN_ST_TENT ? 4 : (s_rbi_mst == RBN_ST_PEND ? 5 : 0)));
    rk = (s_rbi_last_host_why > 0 && s_rbi_last_host_why < RBN_WHY_N) ? (int)s_rbi_last_host_why : (int)s_rbi_last_why;
    if (rk < 0 || rk >= RBN_WHY_N) rk = 0;
    o->why = rk;
    o->why_key = k_rbn_why_key[rk];
    o->delay = s_rbi_ep ? (int)s_rbi_reg_d : 0;
    if (s_rbn) {
        EnterCriticalSection(&s_rbn_cs);
        o->rtt_ms = rbn_rtt_max(s_rbn, now);
        LeaveCriticalSection(&s_rbn_cs);
    }
    o->rb_total = s_rbi_rb_n;
    if (s_rbc) {
        const rbi_stats_t *st = rbi_stats(s_rbc);
        o->taken = st->taken;
        o->predicted = st->predicted;
    }
    o->adv = s_rbi_hud_adv;
    o->tsync = s_rbi_tsync;
    o->buf_ms = -999;
    if (s_rbc) {
        int smed = 0;
        o->tsync_ms = rbi_tsync_ms(s_rbc);
        if (rbi_slack_med(s_rbc, rbi_self(s_rbc) == 0 ? 1 : 0, &smed)) o->buf_ms = smed / 10;
    }
    o->ts_waits = s_rbi_ts_waits;
    o->npeers = s_rbi_npeers;
    n = s_rbi_hud_n;
    for (i = n - 1; i >= 0 && i >= n - RBI_HUD_N; i--) {
        LONG j = i % RBI_HUD_N;
        if (now - s_rbi_hud_t[j] > 10000u) break;
        tmp[k++] = s_rbi_hud_d[j];
    }
    if (k > 0) {
        qsort(tmp, (size_t)k, sizeof tmp[0], rbi_hud_ucmp);
        o->depth_p95 = tmp[((u32)k * 95u) / 100u];
    }
    o->depth_n = (unsigned)k;
    return 1;
}

/* W367 LOC-WMA: 1 = der aufrufende Thread ist der Spiel-Thread und rechnet gerade neu (Wiedergabe der Aufzeichnung).
 * Fuer HLE-Einstiege ohne eigene Aufzeichnung (WMA-Playlist): beim Neurechnen nichts ein zweites Mal ausloesen. */
int loc_rb_resim(void)
{
    return s_phase == PH_REPLAY && GetCurrentThreadId() == s_game_tid;
}
