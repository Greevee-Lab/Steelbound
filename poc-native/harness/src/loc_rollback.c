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
 * liegt nur im Speicher und wird je Zyklus verworfen).
 *
 * W464 NET-CORE: aufgeteilt (Modulkarte in loc_rb_int.h): hier Grundzustand, Threads/Ruhepunkt, Zyklen, Haken,
 * Optionen, Installation und oeffentliche Funktionen; gemeinsamer Zustand in loc_rb_int.h. */
#include "loc_rb_int.h"

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
int s_ring;            /* loc_rb=1: Ring je Tick */
int s_ww = 1, s_depth = 16, s_pool = 8192, s_verify, s_pdpause = 1, s_shadowchk, s_hot = 2, s_cool = 64;
/* W338: MmCreateKernelStack-Speicher mitsichern (W330: ja). Laeuft ein Thread darauf, macht der Schreibschutz die
 * Ausnahmezustellung unmoeglich (harter Abbruch ohne Bericht, A12-A14 nach dem ersten Schuss) - mit R2 aus. */
int s_kstack = 1;
long s_memev_seen;

static int s_mode;            /* 0 nur Ring (loc_rb), 1 Erfassen, 2 Zyklen */
static u32 s_wait_fn = RB_WAIT_FN;
volatile LONG s_skipwait;
DWORD s_game_tid;
HANDLE s_rthread;
int s_ready, s_failed;

/* Optionen */
int s_k[8] = {1, 2, 4, 8}, s_nk = 4, s_render, s_warm = 300, s_max, s_census = 1, s_perturb, s_twice, s_freeze;
int s_ctl;              /* aktueller Zyklus ist Gegenprobe (Eingabe absichtlich veraendert) */
static unsigned s_ctl_n, s_ctl_det, s_ctl_k[8], s_ctl_k_det[8];

long long s_qpf = 1;
long long rb_now(void)
{
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return q.QuadPart;
}
u32 rb_us(long long d) { return d <= 0 ? 0u : (u32)(d * 1000000 / s_qpf); }
u32 rd32(u32 va)
{
    u32 v = 0;
    __try { v = *(volatile const u32 *)(uintptr_t)va; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return v;
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
static int s_rthread_scan_n; /* angelegte Gast-Threads beim letzten Scan */
void rb_rthread_refresh(const char *why)
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
void rb_rthread_check(void)
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
u32 s_rs_n, s_rs_double, s_rs_swap, s_rs_spur, s_rs_prev0, s_rs_max_ms, s_rs_rescue, s_lx_n, s_lx_spec,
    s_lx_rep, s_lx_held, s_spec_endstop, s_rep_endstop, s_spec_abandon_n;
int s_endguard = 1;        /* loc_rb_opts endguard=0: alte Wirkung (TEST, Nachweis vorher) */
static int s_exithook = 1;        /* loc_rb_opts exithook=0: kein Haken am Schleifenende */
static int s_sguard_ms = 2000;    /* loc_rb_opts sguard=N: Waechter (ms), 0 = aus */
static int s_test_end = -1, s_test_end_spec, s_test_end_done; /* TEST tend=N, tendspec=1 */
int s_rep_ended;           /* Missionsende im Neurechnen schon behandelt (rbi_mission_end nicht erneut) */
static const char *rb_rs_why_name(LONG w) { return w == 2 ? "Vorlauf" : (w == 1 ? "Ruhepunkt/Neurechnen" : "-"); }
void rb_rs_resume(void)
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
int rb_rs_suspend(HANDLE h, int why)
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
int rb_loop_exits(void)
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
void rb_freeze_others(void)
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
void rb_thaw_others(void)
{
    int i;
    for (i = 0; i < s_nfrz; i++) ResumeThread(s_frz[i]);
    s_nfrz = 0;
}

/* Warten, bis der Render-Thread frei ist (Auftrag 0), dann anhalten. Zeit in us. */
int rb_quiesce(int suspend)
{
    long long t0 = rb_now();
    int spins = 0;
    while (rd32(RB_RJOB) == 1u) {
        if (rb_us(rb_now() - t0) > 100000u) {
            s_q_timeout++;
            rbh_add(RBH_QWAIT, rb_now() - t0); /* W469 */
            s_rbh_events |= RBH_EV_QTIMEOUT;
            return 0;
        }
        spins++;
        SwitchToThread();
    }
    if (spins) s_q_busy++;
    rb_put(s_q_wait, rb_us(rb_now() - t0));
    rbh_add(RBH_QWAIT, rb_now() - t0); /* W469 */
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

/* W469 LOC-RB-HITCH: Ruhepunkt vor dem Sichern (Tick-Ende). Bisher rb_quiesce(0) mit bis zu 100 ms Warten auf den
 * Render-Auftrag; uebersetzt der Render-Thread gerade Shader (40-70 ms je Programm), stand der Spiel-Thread so lange
 * (W469 V0: alle Einzel-Haenger nach dem Ringstart, beide Seiten im selben Tick). Der Lockstep wartet dort nicht. Das
 * Sichern selbst braucht keinen freien Render-Thread: Schreibzugriffe aller Threads werden verfolgt (WP/Write-Watch,
 * loc_rb_ring.c) und landen in dieser oder der naechsten Sicherung - genau so lief es bisher nach der Zeitgrenze.
 * Jetzt wartet das Sichern hoechstens s_qwait ms (loc_rb_opts qwait=, Standard 8, 100 = alt). Laden/Neurechnen halten
 * den Render-Thread weiter an (rb_quiesce(1), unveraendert). */
int s_qwait = 8;
u32 s_q_short; /* Sichern ohne freien Render-Thread (Grenze s_qwait erreicht) */
/* W469: Die Xbox-Threads teilen sich einen Kern (krnl_sync, Uniprozessor-Nachbildung). Laeuft der Render-Auftrag am
 * Ruhepunkt noch, verdraengte er den Spiel-Thread mitten in der Ruhepunkt-Arbeit um eine Zeitscheibe (W469 V2/V3 bei
 * 30 ms: Sichern 23-50 ms bzw. Regler 42 ms Wanduhr bei ~1 ms CPU). Deshalb laeuft die Arbeit am Ruhepunkt einer
 * Eingabeschicht-Mission (rb_after, ~1-2 ms je Tick) mit THREAD_PRIORITY_HIGHEST (loc_rb_opts cprio=0: aus). Warten
 * darin geschieht per SwitchToThread/Sleep (gibt den Kern auch an niedrigere Threads ab) oder als begrenzter Spin;
 * das Feld-Warten des Spiels selbst laeuft wieder mit der alten Prioritaet. */
int s_cprio = 1;
static int s_cprio_old = THREAD_PRIORITY_ERROR_RETURN;
void rb_commit_prio(int on)
{
    if (!s_cprio) return;
    if (on) {
        s_cprio_old = GetThreadPriority(GetCurrentThread());
        if (s_cprio_old != THREAD_PRIORITY_ERROR_RETURN && s_cprio_old < THREAD_PRIORITY_HIGHEST)
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        else
            s_cprio_old = THREAD_PRIORITY_ERROR_RETURN;
    } else if (s_cprio_old != THREAD_PRIORITY_ERROR_RETURN) {
        SetThreadPriority(GetCurrentThread(), s_cprio_old);
        s_cprio_old = THREAD_PRIORITY_ERROR_RETURN;
    }
}
void rb_quiesce_commit(void)
{
    long long t0 = rb_now();
    u32 lim = (u32)(s_qwait < 0 ? 0 : (s_qwait > 100 ? 100 : s_qwait)) * 1000u;
    int spins = 0;
    while (rd32(RB_RJOB) == 1u) {
        if (rb_us(rb_now() - t0) >= lim) {
            s_q_short++;
            s_rbh_events |= RBH_EV_QTIMEOUT;
            break;
        }
        spins++;
        SwitchToThread();
    }
    if (spins) s_q_busy++;
    rb_put(s_q_wait, rb_us(rb_now() - t0));
    rbh_add(RBH_QWAIT, rb_now() - t0);
}

/* ================= Zyklen ================= */
static u32 s_cyc, s_cyc_ok, s_cyc_hash_bad, s_cyc_mem_bad, s_cyc_div, s_cyc_skip, s_cyc_k[8], s_cyc_k_ok[8];
static int s_ki, s_kcur, s_fwd, s_rep;
static long long s_t_last, s_t_rep0;
static unsigned s_lst_frames;  /* Lockstep-Ticks seit Missionsbeginn (eigene Zaehlung) */
static u32 s_tick_b;
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
    if (!hash_ok) { s_cyc_hash_bad++; }
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

u32 rb_rep_limit(void) { return s_rbi_ep ? s_rep_end : s_rec_len; }
int rb_r2_acc(void)
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
int rb_ring_check_regions(void)
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
    s_rbh_events |= RBH_EV_REBUILD; /* W469 */
    /* W338: Andere Threads (Render-Thread, Lade-Threads 0x28DE0 ueber ReadFile -> rbr_prewrite) duerfen waehrend
     * Stop/Aufbau weiterlaufen: rbr_stop wartet, bis kein Handler/Prewrite mehr in den alten Puffern arbeitet (A10-A12:
     * Haenger bzw. harter Abbruch nach dem Neuaufbau beim ersten Schuss). Kein Anhalten - ein mitten im Handler
     * angehaltener Thread wuerde das Warten blockieren. */
    {
        long long th = rb_now(); /* W469 */
        rb_build_regions();
        rbh_add(RBH_REBUILD, rb_now() - th);
    }
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

static void rb_after_body(void)
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
    if (s_rbi_mst == RBN_ST_TENT || s_rbi_mst == RBN_ST_PEND || s_rbi_mst == RBN_ST_PRE) { /* W346: Aushandlung je Tick */
        /* W469: vor der Entscheidung entnimmt keine Seite (das Spiel steht ohnehin); statt einmal je Feld (50 ms) bis zu
         * s_negpoll ms fein nachsehen, damit die Mission sofort nach Entscheidung/Bestaetigung weiterlaeuft. */
        long long th = rb_now();
        rbi_mission_poll();
        while ((s_rbi_mst == RBN_ST_TENT || s_rbi_mst == RBN_ST_PEND || s_rbi_mst == RBN_ST_PRE) && s_negpoll > 0 &&
               rb_us(rb_now() - th) < (u32)s_negpoll * 1000u) {
            Sleep(1);
            rbi_mission_poll();
        }
        rbh_add(RBH_MPOLL, rb_now() - th);
    }
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
        long long th = rb_now(), th2; /* W469: Phasen des ersten Aufbaus */
        s_rbh_events |= RBH_EV_SETUP;
        s_game_tid = GetCurrentThreadId();
        rb_threads_log();
        th2 = rb_now();
        rbh_add(RBH_SETUP, th2 - th); /* Thread-Liste */
        if (s_wpbench) rb_wp_bench(); /* W469: Mikromessung (5 x 1024 Schreibfehler, ~50 ms) nur noch auf Wunsch */
        rbh_add(RBH_BENCH, rb_now() - th2);
        th2 = rb_now();
        rb_collect_stacks();
        rbh_add(RBH_STACKS, rb_now() - th2);
        th = rb_now();
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
        rbh_add(RBH_CHAIN, rb_now() - th); /* W469 */
        th = rb_now();
        /* W330: Bereiche/Ring erst nach allen Code-Patches (Ketten, Zensus): VirtualProtect dort hebt sonst den
         * Ring-Schreibschutz unverfolgt auf */
        rb_build_regions();
        if (!rb_alloc_snaps(s_mode == 2)) { s_failed = 1; return; }
        if (!s_ring) {
            long long t = rb_now();
            rb_copy_out(s_snapA);
            rb_put(s_t_full, rb_us(rb_now() - t));
        }
        rbh_add(RBH_SETUP, rb_now() - th); /* W469 */
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
        if (s_phase != PH_REPLAY) rb_quiesce_commit(); /* W469 */
        {
            long long th = rb_now(); /* W469 */
            rb_ring_commit(s_phase == PH_REPLAY);
            rbh_add(RBH_COMMIT, rb_now() - th);
        }
    }
    if (s_mode == 1) rb_dirty_step();
    else if (s_mode == 2) rb_cycle_step();
}

/* W376 LOC-LOCAL-AIM: vor dem Feld-Warten (Tick fertig simuliert, Render-Vorbereitung abgegeben). Der Render-Thread
 * zeichnet diesen Zustand waehrend des Wartens; die Vorhersage liegt so vor seinem Swap bereit (Messlauf W376_A5: nach
 * dem Warten veroeffentlicht zeigte Liste N schon den Zustand N+1). */
static void __cdecl rb_before(void)
{
    rbh_before(); /* W469 */
    loc_aim_game_tick(s_phase == PH_REPLAY, (s_rbi_ep && s_rbc) ? rbi_cons(s_rbc, rbi_self(s_rbc)) : -1,
                      (s_rbi_ep && s_rbc && rbi_active(s_rbc) && !rbi_replaying(s_rbc)) ? (const void *)s_rbc : NULL);
    if (s_rbi_ep && s_rbc && rbi_active(s_rbc) && !rbi_replaying(s_rbc) && s_phase != PH_REPLAY)
        loc_smooth_tick(rd32(RB_TICK)); /* W385: Lage + Versatz fuer die Liste dieses Ticks */
}

/* W469: Ruhepunkt mit Haenger-Zuordnung (Eintritt/Ende fuer LOCHITCH) */
static void __cdecl rb_after(void)
{
    int boost = s_rbi_ep; /* W469: nur in einer Eingabeschicht-Mission */
    rbh_after_in();
    if (boost) rb_commit_prio(1);
    rb_after_body();
    if (boost) rb_commit_prio(0);
    rbh_after_out();
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
        else if (strncmp(p, "adrtt=", 6) == 0) { s_auto_drtt = atoi(p + 6); }   /* W447 Auto: Grenze DIREKT RTT */
        else if (strncmp(p, "adjit=", 6) == 0) { s_auto_djit = atoi(p + 6); }   /* W447: Grenze DIREKT Schwankung */
        else if (strncmp(p, "adgood=", 7) == 0) { s_auto_good = atoi(p + 7); }  /* W447: Regler DIREKT */
        else if (strncmp(p, "adstart=", 8) == 0) { s_auto_start1 = atoi(p + 8); } /* W447: DIREKT ab Start 1 Tick */
        else if (strncmp(p, "adlcpm=", 7) == 0) { s_auto_lcpm = atoi(p + 7); }
        else if (strncmp(p, "adhcpm=", 7) == 0) { s_auto_hcpm = atoi(p + 7); }
        else if (strncmp(p, "adjump=", 7) == 0) { s_auto_hjump = atoi(p + 7); }
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
        else if (strncmp(p, "wpbench=", 8) == 0) { s_wpbench = atoi(p + 8); } /* W469 DIAG */
        else if (strncmp(p, "qwait=", 6) == 0) { s_qwait = atoi(p + 6); }     /* W469: Wartegrenze Sichern (ms) */
        else if (strncmp(p, "cprio=", 6) == 0) { s_cprio = atoi(p + 6); }     /* W469: Sichern mit erhoehter Prioritaet */
        else if (strncmp(p, "negpoll=", 8) == 0) { s_negpoll = atoi(p + 8); } /* W469: Aushandlung fein pollen (ms) */
        else if (strncmp(p, "preagree=", 9) == 0) { s_pre_on = atoi(p + 9) ? 1 : 0; } /* W475: Vorab-Einigung in der Lobby */
        else if (strncmp(p, "pretest=", 8) == 0) { s_pre_test = atoi(p + 8); } /* W475 TEST: Rueckfall beim Start */
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
    if (s_ring) {
        long long tp = rb_now();
        int pr = rbr_prealloc((unsigned)s_pool, RB_SHADOW_PRE); /* W469: nicht erst beim Ringstart im Spiel-Thread */
        HLOG_I("LOCRB: RING eingeschaltet (loc_rb=1): Write-Watch fuer NtAlloc=%d, Tiefe %d, Puffer %d Seiten, verify=%d, "
               "Present-Ruhe=%d", g_loc_rb_ww, s_depth, s_pool, s_verify, s_pdpause);
        HLOG_I("LOCRB: W469 Ring-Puffer und Schatten (%u Seiten) vorab angelegt: %s, %u us", RB_SHADOW_PRE,
               pr == 0 ? "ok" : "FEHLT (Anlage beim Ringstart wie bisher)", rb_us(rb_now() - tp));
    }
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
    rbh_log(waited_s); /* W469 */
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
                                                                                             : (s_rbi_mst == RBN_ST_PEND ? "wait" : (s_rbi_mst == RBN_ST_PRE ? "pre" : "lobby")))),
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
                                     : (s_rbi_mst == RBN_ST_LS ? 1 : (s_rbi_mst == RBN_ST_TENT ? 4 : (s_rbi_mst == RBN_ST_PEND || s_rbi_mst == RBN_ST_PRE ? 5 : 0)));
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
    o->net_mode = loc_rb_net_mode(); /* W447 */
    o->prof = (int)s_auto_prof;
    o->prof_rtt = (int)s_auto_rtt;
    o->prof_var = (int)s_auto_var;
    return 1;
}

/* W447 LOC-NET-AUTO: Netzmodus zur Laufzeit (ESC-Menue) und Lobby-Anzeige */
int loc_rb_net_mode(void)
{
    LONG m = s_auto_mode;
    if (!g_cfg.loc_rb_input || !s_rbi || !s_rbn) return 2;
    return (m >= 0 && m <= 2) ? (int)m : (g_cfg.loc_net_auto ? 0 : 1);
}

int loc_rb_net_mode_set(int mode)
{
    if (mode < 0 || mode > 2) return 0;
    if (!g_cfg.loc_rb_input || !s_rbi || !s_rbn) return mode == 2;
    EnterCriticalSection(&s_rbn_cs);
    rbn_set_want(s_rbn, mode != 2, mode == 0);
    LeaveCriticalSection(&s_rbn_cs);
    InterlockedExchange(&s_auto_mode, mode);
    g_cfg.loc_net_auto = mode == 0;
    HLOG_I("LOCRBI-AUTO: Netzmodus -> %s (ESC-Menue, ab der naechsten Mission)",
           mode == 0 ? "Auto" : mode == 1 ? "Rollback" : "Lockstep");
    return 1;
}

/* W474 CHATTABS: Mission laut Eingabeschicht (wie loc_rb_lobby.in_mission), ohne Sperre; 0 ohne Eingabeschicht. */
int loc_rb_in_mission(void)
{
    int m = s_rbi_mst;
    if (!g_cfg.loc_rb_input || !s_rbi) return 0;
    return m == RBN_ST_R2 || m == RBN_ST_LS || m == RBN_ST_TENT || m == RBN_ST_PEND;
}

int loc_rb_lobby_read(loc_rb_lobby *o)
{
    u32 now = GetTickCount();
    int pi[LOC_RB_LOBBY_PEERS], rt[LOC_RB_LOBBY_PEERS], wa[LOC_RB_LOBBY_PEERS], k;
    if (!o) return 0;
    memset(o, 0, sizeof *o);
    o->rtt = o->var = -1;
    o->net_mode = loc_rb_net_mode();
    if (!g_cfg.loc_rb_input || !s_rbi || !s_rbn) return 0;
    o->in_mission = s_rbi_mst == RBN_ST_R2 || s_rbi_mst == RBN_ST_LS || s_rbi_mst == RBN_ST_TENT || s_rbi_mst == RBN_ST_PEND ||
                    s_rbi_mst == RBN_ST_PRE;
    EnterCriticalSection(&s_rbn_cs);
    o->agreed = rbn_auto_agreed(s_rbn, now);
    o->rtt = rbn_rtt_max(s_rbn, now);
    o->var = rbn_rttvar_max(s_rbn, now);
    o->npeer = rbn_peer_list(s_rbn, now, pi, rt, wa, LOC_RB_LOBBY_PEERS);
    LeaveCriticalSection(&s_rbn_cs);
    for (k = 0; k < o->npeer; k++) {
        o->peer[k].pidx = pi[k];
        o->peer[k].rtt = rt[k];
        o->peer[k].wauto = wa[k];
    }
    if (s_rbi_mst == RBN_ST_R2 && s_auto_prof >= 0) { /* laufende Mission: gewaehltes Profil und wirksames D */
        o->prof = (int)s_auto_prof;
        o->d_start = s_rbl ? rbl_dstd(s_rbl) : (int)s_rbi_reg_d;
        o->d_target = (int)s_rbi_reg_d;
        return 1;
    }
    /* Lobby: Vorschau wie die Entscheidung des Hosts beim Missionsbeginn */
    o->prof = o->agreed ? rbn_pick_profile(o->rtt, o->var, s_auto_drtt, s_auto_djit) : RBN_PROF_FIXED;
    o->d_start = s_rbi_delay > 0 ? s_rbi_delay : rbn_pick_delay(o->rtt, s_rbi_dmin, s_rbi_dmax);
    o->d_target = (s_rbi_delay > 0 || !s_rbl_cfg.on) ? o->d_start
                  : (o->prof == RBN_PROF_DIRECT ? 1 : rbn_pick_low(o->rtt, o->d_start, s_rbl_cfg.lowmax));
    if (o->d_target < 1) o->d_target = 1;
    if (o->prof == RBN_PROF_DIRECT && s_auto_start1 > 0 && o->rtt >= 0 && o->rtt <= s_auto_start1 && s_rbi_delay <= 0)
        o->d_start = 1; /* Start gleich mit 1 Tick */
    return 1;
}

/* W367 LOC-WMA: 1 = der aufrufende Thread ist der Spiel-Thread und rechnet gerade neu (Wiedergabe der Aufzeichnung).
 * Fuer HLE-Einstiege ohne eigene Aufzeichnung (WMA-Playlist): beim Neurechnen nichts ein zweites Mal ausloesen. */
int loc_rb_resim(void)
{
    return s_phase == PH_REPLAY && GetCurrentThreadId() == s_game_tid;
}
