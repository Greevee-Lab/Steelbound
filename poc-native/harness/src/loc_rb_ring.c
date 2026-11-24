/* loc_rb_ring.c - W330 LOC-ROLLBACK-R1: inkrementeller Zustandsring (Beschreibung: loc_rb_ring.h).
 *
 * Seitenzustand pst[p]: 0 = sauber (WP: schreibgeschuetzt), 1 = seit dem letzten Sichern beschrieben
 * (WP: Schutz aufgehoben). Sichern (rbr_commit):
 *   1. WW-Bereiche: GetWriteWatch(RESET) -> pst = 1.
 *   2. je Seite mit pst = 1: pst = 0, WP-Seiten wieder schuetzen (Laeufe zusammengefasst, Spiegel mit),
 *      DANACH vergleichen/kopieren. Reihenfolge wichtig: Schreibzugriffe anderer Threads vor dem
 *      Schutz landen in der Kopie, danach loesen sie einen neuen Fehler aus (naechster Tick).
 *   3. geaenderte Seite: alter Inhalt (S) in den Ring, S = Live.
 * Laden (rbr_load): fuer jede Seite der k neuesten Ticks den aeltesten alten Inhalt nach Live und S
 * schreiben (WP-Schutz vorher aufheben, pst = 1: das naechste Sichern vergleicht sie, gleiche Seiten
 * kosten nur den Vergleich). */
#include <windows.h>
#include <string.h>
#include <stdlib.h>

#include "loc_rb_ring.h"

typedef uint32_t u32;
#define PG 4096u
#define RBR_RMAX 512
#define RBR_TMAX 256

typedef struct {
    u32 va, len, kind, pg0, npg;
    int method;
    u32 alias[2];
    int nalias;
    DWORD prot_rw, prot_ro;
} reg_t;

typedef struct { u32 lo, hi; int reg; u32 base; } lk_t; /* Suchtabelle: Bereich + Spiegel -> Bereich */

static reg_t s_r[RBR_RMAX];
static int s_nr;
static lk_t s_lk[RBR_RMAX * 3];
static int s_nlk;
static u32 s_npg;
static uint8_t *s_S;               /* Schatten */
static uint8_t *s_S_keep;          /* W469: Schatten-Speicher, bleibt ueber rbr_stop erhalten (vorab reserviert) */
static u32 s_S_cap;                /* Seiten in s_S_keep */
static volatile uint8_t *s_pst;    /* Seitenzustand */
static u32 *s_stamp, s_gen;        /* Laden: Seite schon zurueckgeschrieben */
static u32 *s_dl;                  /* Sichern: Liste gemeldeter Seiten */
/* Heisse Seiten (WP): in hot_n aufeinanderfolgenden Ticks beschrieben -> bleiben offen (pst = 1) und werden
 * jeden Tick nur verglichen (kein Schreibschutz-Fehler). Nach hot_cool unveraenderten Ticks wieder geschuetzt.
 * Invariante: WP-Seite offen <=> pst = 1. */
static uint8_t *s_streak, *s_hotf, *s_cexp;
static uint16_t *s_cold;
static u32 *s_lastc, *s_coolat, s_ncommit; /* coolat: Sicherung, bei der die Seite zuletzt abgekuehlt ist */
static u32 s_hot_n = 2, s_hot_cool = 64, s_nhot;
static volatile int s_active;
static volatile LONG s_veh_in; /* W338: laufende Schreibschutz-Handler (rbr_stop wartet, bevor es Puffer freigibt) */
static PVOID s_veh;
static u32 s_owner;
static long long s_qpf = 1;

/* Ring */
typedef struct { u32 pg; u32 pad; } ent_t;
static uint8_t *s_pool;            /* cap Seiten */
static ent_t *s_ent;
static u32 s_cap, s_depth_max;
static struct { u32 e0, ne; } s_t[RBR_TMAX];
static u32 s_t0, s_nt;             /* aeltester Tick (Index in s_t), Anzahl */
static u32 s_used, s_head;         /* belegte Plaetze, naechster freier Platz */
static u32 s_ww_cap;
static PVOID *s_ww_addr;

/* Statistik */
static volatile LONG s_faults, s_faults_foreign, s_faults_alias, s_prewrites, s_prot_fail;
static volatile LONG64 s_fault_qpc;
#define RBR_WMAX 32
static rbr_writer_t s_w[RBR_WMAX];
static volatile LONG s_wlock;

static long long now_qpc(void)
{
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return q.QuadPart;
}
static u32 us_of(long long d) { return d <= 0 ? 0u : (u32)(d * 1000000 / s_qpf); }
uint64_t rbr_qpf(void) { return (uint64_t)s_qpf; }

static DWORD ro_of(DWORD p)
{
    DWORD m = p & 0xFF, hi = p & ~0xFFu;
    if (m == PAGE_EXECUTE_READWRITE || m == PAGE_EXECUTE_WRITECOPY) return PAGE_EXECUTE_READ | hi;
    return PAGE_READONLY | hi;
}

int rbr_init(unsigned depth, unsigned pool_pages)
{
    LARGE_INTEGER f;
    if (s_active) return -1;
    QueryPerformanceFrequency(&f);
    s_qpf = f.QuadPart ? f.QuadPart : 1;
    s_nr = 0;
    s_nlk = 0;
    s_npg = 0;
    s_depth_max = depth < 1 ? 1 : (depth > RBR_TMAX - 1 ? RBR_TMAX - 1 : depth);
    if (pool_pages < 64) pool_pages = 64;
    if (s_pool && s_cap != pool_pages) {
        VirtualFree(s_pool, 0, MEM_RELEASE);
        free(s_ent);
        s_pool = NULL;
        s_ent = NULL;
    }
    if (!s_pool) {
        s_pool = (uint8_t *)VirtualAlloc(NULL, (SIZE_T)pool_pages * PG, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        s_ent = (ent_t *)calloc(pool_pages, sizeof(ent_t));
        if (!s_pool || !s_ent) return -1;
    }
    s_cap = pool_pages;
    rbr_reset_ring();
    return 0;
}

static int ww_probe(u32 va, u32 len)
{
    PVOID a[4];
    ULONG_PTR n = 4;
    ULONG gran = 0;
    (void)len;
    return GetWriteWatch(0, (PVOID)(uintptr_t)va, PG, a, &n, &gran) == 0;
}

int rbr_add(uint32_t va, uint32_t len, int want_ww, const uint32_t *alias, int nalias, uint32_t kind)
{
    reg_t *r;
    MEMORY_BASIC_INFORMATION mi;
    int i;
    if (s_active || s_nr >= RBR_RMAX || !len || (va & (PG - 1)) || (len & (PG - 1)) || nalias > 2) return -1;
    if (!VirtualQuery((void *)(uintptr_t)va, &mi, sizeof mi) || mi.State != MEM_COMMIT) return -1;
    r = &s_r[s_nr];
    memset(r, 0, sizeof *r);
    r->va = va;
    r->len = len;
    r->kind = kind;
    r->pg0 = s_npg;
    r->npg = len / PG;
    r->prot_rw = mi.Protect;
    r->prot_ro = ro_of(mi.Protect);
    r->method = (want_ww && ww_probe(va, len)) ? RBR_WW : RBR_WP;
    r->nalias = r->method == RBR_WP ? nalias : 0; /* WW: Spiegel nicht noetig (kein Schutz) */
    for (i = 0; i < r->nalias; i++) r->alias[i] = alias[i];
    s_npg += r->npg;
    s_nr++;
    return r->method;
}

static int lk_cmp(const void *a, const void *b)
{
    const lk_t *x = (const lk_t *)a, *y = (const lk_t *)b;
    return x->lo < y->lo ? -1 : x->lo > y->lo;
}

/* VA (auch Spiegel) -> globaler Seitenindex, -1 wenn nicht verfolgt */
static int page_of(uintptr_t a, int *preg, int *palias)
{
    int lo = 0, hi = s_nlk - 1;
    u32 v = (u32)a;
    while (lo <= hi) {
        int m = (lo + hi) >> 1;
        if (v < s_lk[m].lo) hi = m - 1;
        else if (v >= s_lk[m].hi) lo = m + 1;
        else {
            const reg_t *r = &s_r[s_lk[m].reg];
            if (preg) *preg = s_lk[m].reg;
            if (palias) *palias = s_lk[m].base != r->va;
            return (int)(r->pg0 + (v - s_lk[m].lo) / PG);
        }
    }
    return -1;
}

static uint8_t *live_of(const reg_t *r, u32 p) { return (uint8_t *)(uintptr_t)(r->va + (p - r->pg0) * PG); }

/* Schutz fuer einen Seitenlauf [p, p+n) eines Bereichs setzen (ro=1) oder aufheben, Spiegel mit */
static int prot_run(const reg_t *r, u32 p, u32 n, int ro)
{
    DWORD old;
    u32 off = (p - r->pg0) * PG;
    int i, ok = VirtualProtect((void *)(uintptr_t)(r->va + off), n * PG, ro ? r->prot_ro : r->prot_rw, &old);
    for (i = 0; i < r->nalias; i++)
        ok &= VirtualProtect((void *)(uintptr_t)(r->alias[i] + off), n * PG, ro ? r->prot_ro : r->prot_rw, &old);
    if (!ok) InterlockedIncrement(&s_prot_fail);
    return ok;
}

static void note_writer(u32 va, u32 eip)
{
    u32 tid = GetCurrentThreadId();
    int i;
    while (InterlockedExchange(&s_wlock, 1)) YieldProcessor();
    for (i = 0; i < RBR_WMAX; i++) {
        if (s_w[i].tid == tid || !s_w[i].tid) {
            if (!s_w[i].tid) {
                s_w[i].tid = tid;
                s_w[i].first_eip = eip;
                s_w[i].first_va = va;
            }
            s_w[i].n++;
            s_w[i].last_va = va;
            break;
        }
    }
    InterlockedExchange(&s_wlock, 0);
}

static LONG rbr_veh_body(EXCEPTION_POINTERS *ep);
static LONG CALLBACK rbr_veh(EXCEPTION_POINTERS *ep)
{
    LONG r;
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    InterlockedIncrement(&s_veh_in); /* W338: vor der s_active-Pruefung zaehlen, rbr_stop wartet auf 0 */
    r = rbr_veh_body(ep);
    InterlockedDecrement(&s_veh_in);
    return r;
}

static LONG rbr_veh_body(EXCEPTION_POINTERS *ep)
{
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    long long t0;
    int reg = -1, al = 0, p;
    u32 a;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2 || er->ExceptionInformation[0] != 1 ||
        !s_active)
        return EXCEPTION_CONTINUE_SEARCH;
    t0 = now_qpc();
    a = (u32)er->ExceptionInformation[1];
    p = page_of(a, &reg, &al);
    if (p < 0 || s_r[reg].method != RBR_WP) return EXCEPTION_CONTINUE_SEARCH;
    if (!prot_run(&s_r[reg], (u32)p, 1, 0)) return EXCEPTION_CONTINUE_SEARCH; /* z. B. inzwischen freigegeben */
    s_pst[p] = 1;
    InterlockedIncrement(&s_faults);
    if (al) InterlockedIncrement(&s_faults_alias);
    if (s_owner && GetCurrentThreadId() != s_owner) {
        InterlockedIncrement(&s_faults_foreign);
        note_writer(a, (u32)ep->ContextRecord->Eip);
    }
    InterlockedAdd64(&s_fault_qpc, now_qpc() - t0);
    return EXCEPTION_CONTINUE_EXECUTION;
}

void rbr_set_owner(uint32_t tid) { s_owner = tid; }

/* W469 LOC-RB-HITCH: Schatten (und Ring-Puffer) vorab anlegen und jede Seite einmal beschreiben, damit Ringstart und
 * Neuaufbau keine 11 000 Erstzugriffe auf frischen Speicher im Spiel-Thread bezahlen (W469 V1: rbr_start ~23 ms). */
static int rbr_touch(uint8_t *p, u32 pages)
{
    u32 i;
    if (!p) return -1;
    for (i = 0; i < pages; i++) ((volatile uint8_t *)p)[(size_t)i * PG] = 0;
    return 0;
}
static int rbr_shadow_reserve(u32 pages)
{
    if (s_S_keep && s_S_cap >= pages) return 0;
    if (s_S_keep) VirtualFree(s_S_keep, 0, MEM_RELEASE);
    s_S_cap = 0;
    s_S_keep = (uint8_t *)VirtualAlloc(NULL, (SIZE_T)pages * PG, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (rbr_touch(s_S_keep, pages) != 0) return -1;
    s_S_cap = pages;
    return 0;
}
int rbr_prealloc(unsigned pool_pages, unsigned shadow_pages)
{
    int r = 0;
    if (s_active) return -1;
    if (pool_pages < 64) pool_pages = 64;
    if (!s_pool) {
        s_pool = (uint8_t *)VirtualAlloc(NULL, (SIZE_T)pool_pages * PG, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        s_ent = (ent_t *)calloc(pool_pages, sizeof(ent_t));
        if (!s_pool || !s_ent) {
            if (s_pool) VirtualFree(s_pool, 0, MEM_RELEASE);
            free(s_ent);
            s_pool = NULL;
            s_ent = NULL;
            r = -1;
        } else {
            s_cap = pool_pages;
            (void)rbr_touch(s_pool, pool_pages);
        }
    }
    if (shadow_pages && rbr_shadow_reserve(shadow_pages) != 0) r = -1;
    return r;
}

int rbr_start(void)
{
    int i, j;
    if (s_active || !s_nr) return -1;
    s_nlk = 0;
    for (i = 0; i < s_nr; i++) {
        s_lk[s_nlk].lo = s_r[i].va;
        s_lk[s_nlk].hi = s_r[i].va + s_r[i].len;
        s_lk[s_nlk].reg = i;
        s_lk[s_nlk].base = s_r[i].va;
        s_nlk++;
        for (j = 0; j < s_r[i].nalias; j++) {
            s_lk[s_nlk].lo = s_r[i].alias[j];
            s_lk[s_nlk].hi = s_r[i].alias[j] + s_r[i].len;
            s_lk[s_nlk].reg = i;
            s_lk[s_nlk].base = s_r[i].alias[j];
            s_nlk++;
        }
    }
    qsort(s_lk, (size_t)s_nlk, sizeof s_lk[0], lk_cmp);
    s_S = rbr_shadow_reserve(s_npg + s_npg / 8u) == 0 ? s_S_keep : NULL; /* W469: vorab reserviert, sonst +12 % Reserve */
    s_pst = (volatile uint8_t *)calloc(s_npg + 8, 1);
    s_stamp = (u32 *)calloc(s_npg + 8, sizeof(u32));
    s_dl = (u32 *)calloc(s_npg + 8, sizeof(u32));
    s_streak = (uint8_t *)calloc(s_npg + 8, 1);
    s_hotf = (uint8_t *)calloc(s_npg + 8, 1);
    s_cold = (uint16_t *)calloc(s_npg + 8, sizeof(uint16_t));
    s_cexp = (uint8_t *)calloc(s_npg + 8, 1);
    s_coolat = (u32 *)calloc(s_npg + 8, sizeof(u32));
    s_lastc = (u32 *)calloc(s_npg + 8, sizeof(u32));
    s_ncommit = 1;
    s_nhot = 0;
    s_ww_cap = 0;
    for (i = 0; i < s_nr; i++)
        if (s_r[i].method == RBR_WW && s_r[i].npg > s_ww_cap) s_ww_cap = s_r[i].npg;
    s_ww_addr = s_ww_cap ? (PVOID *)calloc(s_ww_cap, sizeof(PVOID)) : NULL;
    if (!s_S || !s_pst || !s_stamp || !s_dl || !s_streak || !s_hotf || !s_cold || !s_cexp || !s_coolat || !s_lastc || (s_ww_cap && !s_ww_addr)) {
        rbr_stop();
        return -1;
    }
    s_gen = 0;
    s_faults = s_faults_foreign = s_faults_alias = s_prewrites = s_prot_fail = 0;
    s_fault_qpc = 0;
    memset(s_w, 0, sizeof s_w);
    s_veh = AddVectoredExceptionHandler(1, rbr_veh);
    s_active = 1;
    /* erst schuetzen/zuruecksetzen, dann kopieren */
    for (i = 0; i < s_nr; i++) {
        if (s_r[i].method == RBR_WP) prot_run(&s_r[i], s_r[i].pg0, s_r[i].npg, 1);
        else ResetWriteWatch((void *)(uintptr_t)s_r[i].va, s_r[i].len);
    }
    for (i = 0; i < s_nr; i++) memcpy(s_S + (size_t)s_r[i].pg0 * PG, (void *)(uintptr_t)s_r[i].va, s_r[i].len);
    rbr_reset_ring();
    return 0;
}

static u32 s_stop_us[4]; /* W372 DIAG: Schutz zurueck, Warten auf Handler, Freigeben (us), Warteschleifen */
void rbr_stop_times(uint32_t out[4]) { memcpy(out, s_stop_us, sizeof s_stop_us); }

void rbr_stop(void)
{
    int i;
    long long t0 = now_qpc(), t1, t2;
    if (s_active) {
        for (i = 0; i < s_nr; i++)
            if (s_r[i].method == RBR_WP) prot_run(&s_r[i], s_r[i].pg0, s_r[i].npg, 0);
    }
    s_active = 0;
    MemoryBarrier();
    t1 = now_qpc();
    { /* W338: andere Threads koennen noch im Handler stecken (Seitentabelle, Merkfeld) - erst danach freigeben */
        int spins = 0;
        while (s_veh_in && spins++ < 200000) { /* bis ~0,2 s: verdraengte Threads beenden ihren Handler */
            if (spins & 255) YieldProcessor();
            else SwitchToThread();
        }
        s_stop_us[3] = (u32)spins;
    }
    t2 = now_qpc();
    s_stop_us[0] = us_of(t1 - t0);
    s_stop_us[1] = us_of(t2 - t1);
    if (s_veh) RemoveVectoredExceptionHandler(s_veh);
    s_veh = NULL;
    s_S = NULL; /* W469: Speicher bleibt in s_S_keep (naechster Start/Neuaufbau ohne neue Allokation) */
    free((void *)s_pst);
    s_pst = NULL;
    free(s_stamp);
    s_stamp = NULL;
    free(s_dl);
    s_dl = NULL;
    free(s_streak);
    free(s_hotf);
    free(s_cold);
    free(s_cexp);
    free(s_coolat);
    free(s_lastc);
    s_streak = s_hotf = s_cexp = NULL;
    s_cold = NULL;
    s_lastc = s_coolat = NULL;
    free(s_ww_addr);
    s_ww_addr = NULL;
    s_nr = 0;
    s_nlk = 0;
    s_npg = 0;
    rbr_reset_ring();
    s_stop_us[2] = us_of(now_qpc() - t2);
}

int rbr_active(void) { return s_active; }
uint32_t rbr_pages(void) { return s_npg; }
uint32_t rbr_pages_method(int method)
{
    u32 n = 0;
    int i;
    for (i = 0; i < s_nr; i++)
        if (s_r[i].method == method) n += s_r[i].npg;
    return n;
}

void rbr_reset_ring(void)
{
    s_t0 = s_nt = 0;
    s_used = s_head = 0;
}
unsigned rbr_depth(void) { return s_nt; }

static void drop_oldest(void)
{
    if (!s_nt) return;
    s_used -= s_t[s_t0].ne;
    s_t0 = (s_t0 + 1) % RBR_TMAX;
    s_nt--;
}


/* Platz fuer einen Eintrag am Kopf schaffen (aelteste Ticks verwerfen). 0 = kein Platz (Tick zu gross). */
static int ring_room(u32 cur_ne, rbr_commit_t *c)
{
    while (s_used + cur_ne + 1 > s_cap) {
        if (!s_nt) return 0;
        drop_oldest();
        c->dropped++;
    }
    return 1;
}

static int reg_of_page(u32 q)
{
    int lo = 0, hi = s_nr - 1;
    while (lo <= hi) { /* Bereiche nach pg0 aufsteigend */
        int m = (lo + hi) >> 1;
        if (q < s_r[m].pg0) hi = m - 1;
        else if (q >= s_r[m].pg0 + s_r[m].npg) lo = m + 1;
        else return m;
    }
    return -1;
}

int rbr_commit(rbr_commit_t *st)
{
    rbr_commit_t c;
    long long t0 = now_qpc(), t1;
    u32 nd = 0, ne = 0, e0 = s_head, k;
    int i, ovf = 0, ri = 0;
    memset(&c, 0, sizeof c);
    if (!s_active) return -1;
    /* 1. Write-Watch einsammeln (mit Zuruecksetzen) */
    for (i = 0; i < s_nr; i++) {
        reg_t *r = &s_r[i];
        ULONG_PTR n = s_ww_cap, j;
        ULONG gran = 0;
        if (r->method != RBR_WW) continue;
        if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, (void *)(uintptr_t)r->va, r->len, s_ww_addr, &n, &gran) != 0) continue;
        for (j = 0; j < n; j++) s_pst[r->pg0 + ((u32)(uintptr_t)s_ww_addr[j] - r->va) / PG] = 1;
    }
    t1 = now_qpc();
    c.us_ww = us_of(t1 - t0);
    /* 2. gemeldete Seiten sammeln, pst = 0, WP-Laeufe wieder schuetzen (vor dem Kopieren) */
    for (i = 0; i < s_nr; i++) {
        reg_t *r = &s_r[i];
        u32 q, end = r->pg0 + r->npg, run0 = 0, run = 0;
        const volatile uint8_t *ps = s_pst;
        for (q = r->pg0; q < end; q++) {
            if ((q & 7u) == 0 && q + 8 <= end && *(const volatile uint64_t *)(ps + q) == 0) { /* 8 saubere Seiten */
                if (run) { prot_run(r, run0, run, 1); run = 0; }
                q += 7;
                continue;
            }
            if (ps[q]) {
                s_dl[nd++] = q;
                if (r->method == RBR_WP) {
                    c.wp++;
                    if (s_hotf[q]) { /* heiss: offen lassen, pst bleibt 1 */
                        c.hot++;
                        if (run) { prot_run(r, run0, run, 1); run = 0; }
                        continue;
                    }
                    s_streak[q] = (uint8_t)(s_lastc[q] + 1 == s_ncommit && s_streak[q] < 255 ? s_streak[q] + 1 : 1);
                    s_lastc[q] = s_ncommit;
                    if (s_hot_n && s_streak[q] >= s_hot_n) { /* wird heiss */
                        /* schnell wieder heiss nach dem Abkuehlen (Seite wird jeden Tick mit gleichem Inhalt
                         * beschrieben): naechstes Abkuehlen doppelt so spaet (hoechstens 64-fach) */
                        if (s_coolat[q] && s_ncommit - s_coolat[q] <= 16u && s_cexp[q] < 6) s_cexp[q]++;
                        s_hotf[q] = 1;
                        s_cold[q] = 0;
                        s_nhot++;
                        c.hot++;
                        if (run) { prot_run(r, run0, run, 1); run = 0; }
                        continue;
                    }
                    s_pst[q] = 0;
                    if (!run) run0 = q;
                    run++;
                    continue;
                }
                s_pst[q] = 0;
                c.ww++;
            }
            if (run) { prot_run(r, run0, run, 1); run = 0; }
        }
        if (run) prot_run(r, run0, run, 1);
    }
    c.dirty = nd;
    c.us_prot = us_of(now_qpc() - t1);
    t1 = now_qpc();
    /* 3. vergleichen; geaendert -> alter Inhalt in den Ring, S = Live (Liste ist nach Bereichen sortiert) */
    while (s_nt >= s_depth_max) { drop_oldest(); c.dropped++; }
    for (k = 0; k < nd; k++) {
        u32 q = s_dl[k];
        uint8_t *sv = s_S + (size_t)q * PG, *lv;
        while (ri < s_nr && q >= s_r[ri].pg0 + s_r[ri].npg) ri++;
        if (ri >= s_nr) break;
        lv = live_of(&s_r[ri], q);
        if (memcmp(lv, sv, PG) == 0) {
            if (s_hotf[q] && ++s_cold[q] >= (s_hot_cool << s_cexp[q])) { /* abkuehlen: schuetzen, nachpruefen */
                s_hotf[q] = 0;
                s_coolat[q] = s_ncommit;
                s_streak[q] = 0;
                s_nhot--;
                c.cooled++;
                s_pst[q] = 0;
                prot_run(&s_r[ri], q, 1, 1);
                if (memcmp(lv, sv, PG) != 0) { /* gerade noch beschrieben (fremder Thread): naechster Tick */
                    prot_run(&s_r[ri], q, 1, 0);
                    s_pst[q] = 1;
                }
            }
            continue;
        }
        if (s_hotf[q]) s_cold[q] = 0;
        c.saved++;
        if (!ovf) {
            if (!ring_room(ne, &c)) {
                ovf = 1; /* Tick groesser als der Ring: Ring wird leer, Schatten bleibt richtig */
            } else {
                u32 slot = (e0 + ne) % s_cap;
                memcpy(s_pool + (size_t)slot * PG, sv, PG);
                s_ent[slot].pg = q;
                ne++;
            }
        }
        memcpy(sv, lv, PG);
    }
    if (ovf) {
        rbr_reset_ring();
        c.overflow = 1;
    } else {
        u32 ti = (s_t0 + s_nt) % RBR_TMAX;
        s_t[ti].e0 = e0;
        s_t[ti].ne = ne;
        s_nt++;
        s_used += ne;
        s_head = (e0 + ne) % s_cap;
    }
    s_ncommit++;
    c.hot_total = s_nhot;
    c.us_copy = us_of(now_qpc() - t1);
    c.us_total = us_of(now_qpc() - t0);
    if (st) *st = c;
    return 0;
}

/* W390 LOC-OWN-PREDICT: alle seit dem letzten Sichern beschriebenen Seiten auf den Schatten S (= Stand des letzten
 * rbr_commit) zuruecksetzen, ohne den Ring zu veraendern (spekulativer Vorlauf: danach wieder genau der gesicherte
 * Stand). Gemeldete Seiten bleiben offen (pst = 1) und werden beim naechsten Sichern nur verglichen (gleich -> nichts
 * in den Ring). pages = zurueckgeschriebene Seiten, checked = verglichene. 0 = ok, -1 = nicht aktiv. */
int rbr_revert(uint32_t *pages, uint32_t *checked, uint32_t *us)
{
    long long t0 = now_qpc();
    u32 n = 0, nc = 0;
    int i;
    if (!s_active) return -1;
    for (i = 0; i < s_nr; i++) { /* Write-Watch ohne Zuruecksetzen: das naechste rbr_commit sieht sie noch */
        reg_t *r = &s_r[i];
        ULONG_PTR cnt = s_ww_cap, j;
        ULONG gran = 0;
        if (r->method != RBR_WW) continue;
        if (GetWriteWatch(0, (void *)(uintptr_t)r->va, r->len, s_ww_addr, &cnt, &gran) != 0) continue;
        for (j = 0; j < cnt; j++) s_pst[r->pg0 + ((u32)(uintptr_t)s_ww_addr[j] - r->va) / PG] = 1;
    }
    for (i = 0; i < s_nr; i++) {
        reg_t *r = &s_r[i];
        u32 q, end = r->pg0 + r->npg;
        const volatile uint8_t *ps = s_pst;
        for (q = r->pg0; q < end; q++) {
            uint8_t *lv, *sv;
            if ((q & 7u) == 0 && q + 8 <= end && *(const volatile uint64_t *)(ps + q) == 0) {
                q += 7;
                continue;
            }
            if (!ps[q]) continue;
            nc++;
            lv = live_of(r, q);
            sv = s_S + (size_t)q * PG;
            if (memcmp(lv, sv, PG) == 0) continue;
            memcpy(lv, sv, PG);
            n++;
        }
    }
    if (pages) *pages = n;
    if (checked) *checked = nc;
    if (us) *us = us_of(now_qpc() - t0);
    return 0;
}

int rbr_load(unsigned k, uint32_t *pages, uint32_t *us)
{
    long long t0 = now_qpc();
    u32 n = 0, t, first;
    if (!s_active || k > s_nt) return -1;
    if (!k) {
        if (pages) *pages = 0;
        if (us) *us = 0;
        return 0;
    }
    if (++s_gen == 0) {
        memset(s_stamp, 0, (size_t)s_npg * sizeof(u32));
        s_gen = 1;
    }
    first = s_nt - k; /* aeltester zu ladender Tick (relativ zu s_t0) */
    /* aeltester Tick zuerst: seine alten Inhalte gewinnen, spaetere Ticks derselben Seite uebergehen */
    for (t = first; t < s_nt; t++) {
        u32 ti = (s_t0 + t) % RBR_TMAX, e;
        for (e = 0; e < s_t[ti].ne; e++) {
            u32 slot = (s_t[ti].e0 + e) % s_cap, q = s_ent[slot].pg;
            int ri;
            if (s_stamp[q] == s_gen) continue;
            s_stamp[q] = s_gen;
            ri = reg_of_page(q);
            if (ri < 0) continue;
            if (s_r[ri].method == RBR_WP && !s_pst[q]) prot_run(&s_r[ri], q, 1, 0); /* offen <=> pst = 1 */
            s_pst[q] = 1;
            memcpy(live_of(&s_r[ri], q), s_pool + (size_t)slot * PG, PG);
            memcpy(s_S + (size_t)q * PG, s_pool + (size_t)slot * PG, PG);
            n++;
        }
    }
    /* Ring um k Ticks kuerzen */
    for (t = 0; t < k; t++) {
        u32 ti = (s_t0 + s_nt - 1) % RBR_TMAX;
        s_used -= s_t[ti].ne;
        s_head = s_t[ti].e0;
        s_nt--;
    }
    if (pages) *pages = n;
    if (us) *us = us_of(now_qpc() - t0);
    return 0;
}

static void rbr_prewrite_body(uintptr_t a, uint32_t n);
void rbr_prewrite(uintptr_t a, uint32_t n)
{
    InterlockedIncrement(&s_veh_in); /* W338: wie der Handler - rbr_stop gibt erst frei, wenn niemand mehr drin ist */
    if (s_active && n) rbr_prewrite_body(a, n);
    InterlockedDecrement(&s_veh_in);
}

static void rbr_prewrite_body(uintptr_t a, uint32_t n)
{
    uintptr_t e;
    if (!s_active || !n) return;
    e = a + n;
    a &= ~(uintptr_t)(PG - 1);
    for (; a < e; a += PG) {
        int reg = -1, p = page_of(a, &reg, NULL);
        if (p < 0 || s_r[reg].method != RBR_WP) continue;
        if (s_pst[p]) continue; /* schon offen */
        InterlockedIncrement(&s_prewrites);
        prot_run(&s_r[reg], (u32)p, 1, 0);
        s_pst[p] = 1;
    }
}

void rbr_stats(rbr_stat_t *st)
{
    st->faults = (u32)s_faults;
    st->faults_foreign = (u32)s_faults_foreign;
    st->faults_alias = (u32)s_faults_alias;
    st->prewrites = (u32)s_prewrites;
    st->fault_qpc = (uint64_t)s_fault_qpc;
    st->prot_fail = (u32)s_prot_fail;
}

unsigned rbr_writers(rbr_writer_t *w, unsigned cap)
{
    unsigned i, n = 0;
    while (InterlockedExchange(&s_wlock, 1)) YieldProcessor();
    for (i = 0; i < RBR_WMAX && n < cap; i++)
        if (s_w[i].tid) w[n++] = s_w[i];
    InterlockedExchange(&s_wlock, 0);
    return n;
}

void rbr_full_copy(uint8_t *dst)
{
    int i;
    for (i = 0; i < s_nr; i++) memcpy(dst + (size_t)s_r[i].pg0 * PG, (void *)(uintptr_t)s_r[i].va, s_r[i].len);
}

static uint32_t diff_pages(const uint8_t *ref, uint32_t *addr, uint32_t cap)
{
    int i;
    u32 n = 0;
    for (i = 0; i < s_nr; i++) {
        const uint8_t *lv = (const uint8_t *)(uintptr_t)s_r[i].va, *rv = ref + (size_t)s_r[i].pg0 * PG;
        u32 o;
        for (o = 0; o < s_r[i].len; o += PG) {
            if (memcmp(lv + o, rv + o, PG) == 0) continue;
            if (n < cap && addr) {
                u32 b;
                for (b = 0; b < PG; b += 4)
                    if (*(const u32 *)(lv + o + b) != *(const u32 *)(rv + o + b)) break;
                addr[n] = s_r[i].va + o + b;
            }
            n++;
        }
    }
    return n;
}

uint32_t rbr_full_diff(const uint8_t *src, uint32_t *addr, uint32_t cap) { return diff_pages(src, addr, cap); }
uint32_t rbr_shadow_diff(uint32_t *addr, uint32_t cap) { return s_S ? diff_pages(s_S, addr, cap) : 0; }

void rbr_set_hot(unsigned n, unsigned cool)
{
    s_hot_n = n > 255 ? 255 : n;
    s_hot_cool = cool < 1 ? 1 : (cool > 255 ? 255 : cool);
}

uint32_t rbr_shadow_copy(uint8_t *dst)
{
    if (!s_S) return 0;
    memcpy(dst, s_S, (size_t)s_npg * PG);
    return s_npg;
}

/* LOC-DESYNC-REPAIR (21.11.): Stand k Ticks zurueck nach dst, ohne Live, Schatten oder Ring zu aendern.
 * Wie rbr_load: je Seite gewinnt der aelteste alte Inhalt der k neuesten Ticks. */
uint32_t rbr_snapshot_at(unsigned k, uint8_t *dst)
{
    u32 t;
    if (!s_S || !dst || k > s_nt) return 0;
    memcpy(dst, s_S, (size_t)s_npg * PG);
    if (!k) return s_npg;
    if (++s_gen == 0) {
        memset(s_stamp, 0, (size_t)s_npg * sizeof(u32));
        s_gen = 1;
    }
    for (t = s_nt - k; t < s_nt; t++) {
        u32 ti = (s_t0 + t) % RBR_TMAX, e;
        for (e = 0; e < s_t[ti].ne; e++) {
            u32 slot = (s_t[ti].e0 + e) % s_cap, q = s_ent[slot].pg;
            if (q >= s_npg || s_stamp[q] == s_gen) continue;
            s_stamp[q] = s_gen;
            memcpy(dst + (size_t)q * PG, s_pool + (size_t)slot * PG, PG);
        }
    }
    return s_npg;
}

int rbr_region_info(int i, uint32_t *va, uint32_t *len, uint32_t *kind, int *method, uint32_t *hot)
{
    u32 q, n = 0;
    if (i < 0 || i >= s_nr) return 0;
    *va = s_r[i].va;
    *len = s_r[i].len;
    *kind = s_r[i].kind;
    *method = s_r[i].method;
    if (s_hotf)
        for (q = s_r[i].pg0; q < s_r[i].pg0 + s_r[i].npg; q++) n += s_hotf[q];
    *hot = n;
    return 1;
}

/* ================= W362 LOC-RING-INIT-EXIT: Thread-Stacks des Prozesses ================= */
typedef LONG(NTAPI *rbr_nqit_t)(HANDLE, int, PVOID, ULONG, PULONG);
typedef struct { LONG st; PVOID teb; PVOID cid[2]; ULONG_PTR aff; LONG pr, bpr; } rbr_tbi_t;
typedef LONG(NTAPI *rbr_ngnt_t)(HANDLE, HANDLE, ACCESS_MASK, ULONG, ULONG, PHANDLE);

static int rd_self(u32 a, void *p, size_t n)
{
    SIZE_T got = 0;
    return a && ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)a, p, n, &got) && got == n;
}

int rbr_alloc_span(uint32_t x, uint32_t *pa, uint32_t *pb)
{
    MEMORY_BASIC_INFORMATION mi;
    u32 ab, e;
    if (!x || !VirtualQuery((void *)(uintptr_t)x, &mi, sizeof mi) || mi.State == MEM_FREE) return 0;
    ab = (u32)(uintptr_t)mi.AllocationBase;
    e = ab;
    for (;;) {
        MEMORY_BASIC_INFORMATION m2;
        if (!VirtualQuery((void *)(uintptr_t)e, &m2, sizeof m2) || (u32)(uintptr_t)m2.AllocationBase != ab ||
            m2.State == MEM_FREE)
            break;
        e = (u32)(uintptr_t)m2.BaseAddress + (u32)m2.RegionSize;
        if (e - ab > 0x10000000u || e < ab) break;
    }
    *pa = ab;
    *pb = e;
    return e > ab;
}

static void stk_put(rbr_stack_t *out, int cap, int *n, u32 x, u32 tid, int bits)
{
    u32 a, b;
    int k;
    if (*n >= cap || !rbr_alloc_span(x, &a, &b)) return;
    if (b - a > 0x400000u) { /* wie die ESP-Ausnahmen: in grosser Belegung nur 1 MB um den Stack-Zeiger */
        u32 lo = (x & ~(PG - 1)) - 0x100000u, hi = (x & ~(PG - 1)) + 0x100000u;
        if (lo > a && lo < x) a = lo;
        if (hi < b && hi > x) b = hi;
    }
    for (k = 0; k < *n; k++)
        if (out[k].a == a && out[k].b == b) return;
    out[*n].a = a;
    out[*n].b = b;
    out[*n].tid = tid;
    out[*n].bits = bits;
    (*n)++;
}

int rbr_thread_stacks(rbr_stack_t *out, int cap, unsigned *nthr, unsigned *noteb)
{
    /* NtGetNextThread: nur die Threads dieses Prozesses (Toolhelp zaehlt alle Threads des Systems: ~130 ms) */
    static rbr_nqit_t nqit;
    static rbr_ngnt_t ngnt;
    HANDLE cur = NULL, nx = NULL;
    int n = 0;
    unsigned thr = 0, bad = 0;
    if (!nqit) {
        HMODULE nt = GetModuleHandleA("ntdll.dll");
        nqit = (rbr_nqit_t)(void *)GetProcAddress(nt, "NtQueryInformationThread");
        ngnt = (rbr_ngnt_t)(void *)GetProcAddress(nt, "NtGetNextThread");
    }
    if (nthr) *nthr = 0;
    if (noteb) *noteb = 0;
    if (!nqit || !ngnt) return 0;
    while (thr < 4096 && ngnt(GetCurrentProcess(), cur, THREAD_QUERY_INFORMATION, 0, 0, &nx) >= 0) {
        rbr_tbi_t bi;
        u32 t, t32 = 0, t64 = 0, self32 = 0, v = 0, tid;
        unsigned long long self64 = 0, q = 0;
        if (cur) CloseHandle(cur);
        cur = nx;
        thr++;
        tid = GetThreadId(cur);
        memset(&bi, 0, sizeof bi);
        if (nqit(cur, 0 /* ThreadBasicInformation */, &bi, sizeof bi, NULL) < 0) bi.teb = NULL;
        t = (u32)(uintptr_t)bi.teb;
        /* TEB-Art am Selbstzeiger erkennen (nicht an fs: der Gast-Thread kann ein eigenes fs haben) */
        if (rd_self(t + 0x18, &self32, 4) && self32 == t) t32 = t;          /* TEB32.NtTib.Self */
        else if (rd_self(t + 0x30, &self64, 8) && self64 == t) t64 = t;     /* TEB64.NtTib.Self */
        if (t32 && t32 > 0x2000 && rd_self(t32 - 0x2000 + 0x30, &self64, 8) && self64 == t32 - 0x2000) t64 = t32 - 0x2000;
        if (t64 && !t32 && rd_self(t64 + 0x2000 + 0x18, &self32, 4) && self32 == t64 + 0x2000) t32 = t64 + 0x2000;
        if (!t32 && !t64) { bad++; continue; }
        if (t32) {
            if (rd_self(t32 + 0x4, &v, 4) && v > 4) stk_put(out, cap, &n, v - 4, tid, 32);  /* StackBase */
            if (rd_self(t32 + 0x8, &v, 4) && v) stk_put(out, cap, &n, v, tid, 32);          /* StackLimit */
            if (rd_self(t32 + 0xE0C, &v, 4) && v) stk_put(out, cap, &n, v, tid, 32);        /* DeallocationStack */
        }
        if (t64) { /* WoW64: 64-Bit-Stack desselben Threads (Systemaufrufe, Ausnahmezustellung) */
            if (rd_self(t64 + 0x8, &q, 8) && q > 8 && q <= 0xFFFFFFFFull) stk_put(out, cap, &n, (u32)q - 8, tid, 64);
            if (rd_self(t64 + 0x10, &q, 8) && q && q <= 0xFFFFFFFFull) stk_put(out, cap, &n, (u32)q, tid, 64);
            if (rd_self(t64 + 0x1478, &q, 8) && q && q <= 0xFFFFFFFFull) stk_put(out, cap, &n, (u32)q, tid, 64);
        }
    }
    if (cur) CloseHandle(cur);
    if (nthr) *nthr = thr;
    if (noteb) *noteb = bad;
    return n;
}
