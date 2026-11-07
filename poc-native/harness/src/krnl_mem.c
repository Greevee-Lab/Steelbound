/* krnl_mem.c - Kernel-HLE: Speicher (NtAllocateVirtualMemory, LIFO-Reuse,
 * Ex-/Mem-Regionen, Contiguous/System/Pool-Speicher, Mm*-Abfragen).
 * W68 SPLIT: 1:1 aus kernel_hle.c verschoben. */
#include "krnl_int.h"

static int in_game_region(u32 va, u32 size)
{
    return va >= g_img->base && va + size <= g_img->image_end && va + size >= va;
}

/* ----- Speicher ----- */
static void memreg_add(u32 va, u32 size, u32 kind);
/* LIFO_REUSE */
#define LIFO_N 64
static u32 lifo_va[LIFO_N];
static u32 lifo_sz[LIFO_N];
static unsigned lifo_n;
static unsigned lifo_hit, lifo_miss;
static void *lifo_snap[LIFO_N];
static unsigned lifo_snap_n;
static void *lifo_last_snap;
static unsigned lifo_last_sz;
static void lifo_push(u32 va, u32 sz)
{
    unsigned i;
    void *snap = NULL;
    if (!va || !sz) return;
    if (sz >= 4096u && sz <= 0x2000000u && lifo_snap_n < 64) {
        __try {
            snap = malloc(sz);
            if (snap) { memcpy(snap, (void *)(uintptr_t)va, sz); }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            if (snap) { free(snap); snap = NULL; }
        }
    }
    for (i = 0; i < lifo_n && i < LIFO_N; i++)
        if (lifo_va[i] == va) return; /* schon drin */
    if (lifo_n < LIFO_N) { lifo_va[lifo_n] = va; lifo_sz[lifo_n] = sz; lifo_snap[lifo_n] = snap; lifo_n++; if (snap) lifo_snap_n++; }
    /* voll: aeltesten verwerfen (Shift) */
    else {
        if (lifo_snap[0]) { free(lifo_snap[0]); lifo_snap_n--; }
        for (i = 1; i < LIFO_N; i++) { lifo_va[i-1] = lifo_va[i]; lifo_sz[i-1] = lifo_sz[i]; lifo_snap[i-1] = lifo_snap[i]; }
        lifo_va[LIFO_N-1] = va; lifo_sz[LIFO_N-1] = sz; lifo_snap[LIFO_N-1] = snap; if (snap) lifo_snap_n++;
    }
}
static u32 lifo_pop(u32 sz)
{
    unsigned i;
    for (i = 0; i < lifo_n; i++) {
        if (lifo_sz[i] == sz) {
            u32 va = lifo_va[i];
            lifo_n--;
            lifo_va[i] = lifo_va[lifo_n]; lifo_sz[i] = lifo_sz[lifo_n];
            { void *sn = lifo_snap[i]; lifo_snap[i] = lifo_snap[lifo_n]; lifo_snap[lifo_n] = NULL;
              if (sn) lifo_snap_n--; lifo_last_snap = sn; lifo_last_sz = sz; }
            lifo_hit++;
            return va;
        }
    }
    lifo_miss++;
    return 0;
}
/* PERF(FILM-FPS,W123) mem_wc_strip: Xbox-D3D legt Texturen/Puffer mit
 * PAGE_READWRITE|PAGE_WRITECOMBINE (0x404) an (GPU liest per DMA). Auf dem Host
 * liest nur die CPU (Hashes, Upload-Kopie nach D3D11); WC-/NOCACHE-Seiten sind
 * dafuer ungecacht (~60 ns je Lesezugriff): Bink-Film 512x422 brauchte ~75 ms je
 * Bild fuer Hash + Upload. 1 = Bits weglassen (gleicher Inhalt, gleiche
 * Adressen), 0 = alt. Erste 16 Faelle -> "MEM wc-strip #n". */
static DWORD host_prot(const char *fn, u32 va, u32 size, u32 prot)
{
    static unsigned nwc;
    DWORD p = prot ? prot : PAGE_READWRITE;
    if (p & (PAGE_WRITECOMBINE | PAGE_NOCACHE)) {
        DWORD q = g_cfg.mem_wc_strip ? (p & ~(DWORD)(PAGE_WRITECOMBINE | PAGE_NOCACHE)) : p;
        if (!q) { q = PAGE_READWRITE; }
        if (nwc < 16u) {
            nwc++;
            HLOG_I("MEM wc-strip #%u: %s va=0x%X size=0x%X prot=0x%X -> 0x%X (mem_wc_strip=%d)", nwc, fn, va, size,
                   prot, q, g_cfg.mem_wc_strip);
        }
        p = q;
    }
    return p;
}

u32 __stdcall hle_NtAllocateVirtualMemory(u32 pBase, u32 zeroBits, u32 pSize,
                                                 u32 type, u32 prot)
{
    u32 base = *(u32 *)(uintptr_t)pBase;
    u32 size = *(u32 *)(uintptr_t)pSize;
    void *r;
    DWORD flProtect = host_prot("NtAllocVM", base, size, prot);
    (void)zeroBits;
    /* Xbox MEM_COMMIT/MEM_RESERVE == Win32-Werte -> Typ direkt durchreichen.
     * (MEM_RESERVE dazu-ODERn zerstoert COMMIT-in-Reservierung, GLE 487!) */
    if (type == 0) {
        type = (base == 0) ? (MEM_COMMIT | MEM_RESERVE) : MEM_COMMIT;
    }
    if (base == 0) {
        u32 want = (size + 0xFFFu) & ~0xFFFu;
        u32 re = lifo_pop(want);
        if (re) {
            r = VirtualAlloc((LPVOID)(uintptr_t)re, size, type, flProtect);
            if (!r) { r = VirtualAlloc(NULL, size, type, flProtect); if (lifo_last_snap) { free(lifo_last_snap); lifo_last_snap = NULL; } }
            else { if (lifo_hit < 8) HLOG_I("LIFO-REUSE va=0x%X sz=0x%X", re, want);
                /* NORESTORE (boot506): Snapshot NICHT zurueckspielen
                 * (artfremde Reuse-Korruption!). Adressen bleiben stabil. */
                if (lifo_last_snap) { free(lifo_last_snap); lifo_last_snap = NULL; } }
        } else {
            r = VirtualAlloc(NULL, size, type, flProtect);
        }
    } else if (in_game_region(base, size)) {
        /* Loader-Region: Seiten gehoeren uns, nur RWX + nullen */
        DWORD old;
        VirtualProtect((LPVOID)(uintptr_t)base, size, PAGE_EXECUTE_READWRITE, &old);
        memset((void *)(uintptr_t)base, 0, size);
        r = (void *)(uintptr_t)base;
    } else {
        r = VirtualAlloc((LPVOID)(uintptr_t)base, size, type, flProtect);
    }
    if (!r) {
        HLOG_W("NtAllocateVirtualMemory(base=0x%X size=0x%X) FEHLER %lu", base, size,
               GetLastError());
        return X_STATUS_NO_MEMORY;
    }
    *(u32 *)(uintptr_t)pBase = (u32)(uintptr_t)r;
    *(u32 *)(uintptr_t)pSize = (size + 0xFFFu) & ~0xFFFu;
    memreg_add((u32)(uintptr_t)r, size, 2);
    HLOG_T("NtAllocateVirtualMemory -> 0x%X (0x%X)", (u32)(uintptr_t)r, size);
    return X_STATUS_SUCCESS;
}

static int exreg_find(u32 va); /* M2c (Def. unten) */
static void low_alloc_note_free(u32 base); /* W122 contig_fast (Def. unten) */
u32 __stdcall hle_NtFreeVirtualMemory(u32 pBase, u32 pSize, u32 type)
{
    u32 base = *(u32 *)(uintptr_t)pBase;
    u32 size = *(u32 *)(uintptr_t)pSize;
    int ok;
    if (in_game_region(base, 1)) {
        /* Loader-Region: nur nullen, nie freigeben */
        if (size && size < 0x1000000u) {
            memset((void *)(uintptr_t)base, 0, size);
        }
        HLOG_T("NtFreeVirtualMemory(0x%X, Loader-Region -> genullt)", base);
        return X_STATUS_SUCCESS;
    }
    /* M2c: NUR echte Ex-Bloecke (kind 1) behalten; NtAlloc/Pool echt
     * freigeben (sonst Zyklen bei Re-Parses, z.B. Font-Liste 0x18E695).
     * M2d: >=4KB BEHALTEN (Tabellen-Arenen, 0x2627B-UAF!), nur kleine
     * Bloecke (Font-Nodes) freigeben. */
    if (exreg_find(base) == 1) {
        static unsigned nexk;
        if (nexk < 8) { nexk++; HLOG_I("EX-FREE via NtFreeVirtualMemory: va=0x%X (behalten)", base); }
        *(u32 *)(uintptr_t)pSize = 0;
        return X_STATUS_SUCCESS;
    }
    if (type & MEM_RELEASE) {
        u32 keep = (size + 0xFFFu) & ~0xFFFu;
        if (keep && keep < 0x2000000u) lifo_push(base, keep);
        size = 0; /* Win32 verlangt dwSize=0 bei MEM_RELEASE (sonst GLE 87) */
    }
    {
        static unsigned nfr;
        if (nfr < 64) { nfr++; HLOG_I("NtFreeVirt: base=0x%X size=0x%X type=0x%X", base, size, type); }
    }
    ok = VirtualFree((LPVOID)(uintptr_t)base, size, type);
    if (!ok) {
        HLOG_W("NtFreeVirtualMemory(0x%X) FEHLER %lu", base, GetLastError());
    } else {
        *(u32 *)(uintptr_t)pSize = size;
        if (type & MEM_RELEASE) { low_alloc_note_free(base); } /* W122: Suchwissen von low_alloc verwerfen (nur RELEASE macht Adressraum frei) */
    }
    return ok ? X_STATUS_SUCCESS : X_STATUS_INVALID_PARAMETER;
}

u32 __stdcall hle_NtQueryVirtualMemory(u32 base, u32 pInfo)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((LPCVOID)(uintptr_t)base, &mbi, sizeof mbi)) {
        return X_STATUS_INVALID_PARAMETER;
    }
    /* MEMORY_BASIC_INFORMATION ist ABI-identisch (7 DWORDs) */
    memcpy((void *)(uintptr_t)pInfo, &mbi, sizeof mbi);
    return X_STATUS_SUCCESS;
}

/* M2c: kein Rescue-Release (s. hle_MmFreeContiguousMemory: pures Leak;
 * Xbox-Semantik: freier Contig-Speicher behaelt Inhalt). */
/* M2c: Ex-Alloc-Registry (welche VAs stammen aus MmAllocateContiguousMemory?).
 * NtFreeVirtualMemory gibt diese Bloecke NIE frei (stale Reads!). Ring, 512. */
#define EXREG_MAX 8192
static struct { u32 vaddr, len; u32 kind; } g_exreg[EXREG_MAX];
/* kind: 1=Ex, 2=NtAlloc, 3=Ex(freed/leak), 4=NtAlloc(freed) */
unsigned g_exreg_n;
static void exreg_add(u32 va, u32 size)
{
    g_exreg[g_exreg_n % EXREG_MAX].vaddr = va;
    g_exreg[g_exreg_n % EXREG_MAX].len = size;
    g_exreg[g_exreg_n % EXREG_MAX].kind = 1;
    g_exreg_n++;
}
static void memreg_add(u32 va, u32 size, u32 kind)
{
    g_exreg[g_exreg_n % EXREG_MAX].vaddr = va;
    g_exreg[g_exreg_n % EXREG_MAX].len = size;
    g_exreg[g_exreg_n % EXREG_MAX].kind = kind;
    g_exreg_n++;
}
/* PERF(PERF-M2,W174) krnl_reg_cache=1: krnl_phys_to_virt/krnl_memreg_find durchsuchen das
 * Register (bis 8192 Eintraege) linear - je Textur-/RT-Zugriff (M2-Kampf: 12 % bzw. 3 % der
 * Uebergabe-Proben in schweren Ticks, W174_r2). Ergebnis-Cache je Thread (TLS, keine Sperren),
 * gueltig solange das Register unveraendert ist (jede Aenderung erhoeht g_exreg_n). Gleiche
 * Antworten wie der Scan (reine Funktion von Adresse + Register); Mehrdeutigkeiten (PHYSALIAS-
 * Diagnose) werden nicht gecacht, mit contig_diag kein Cache. */
#define KRC_N 512u
typedef struct { unsigned gen, key, res, base, len; } krc_t;
static __declspec(thread) krc_t g_krc_p2v[KRC_N], g_krc_mf[KRC_N];
static unsigned krc_slot(unsigned k) { return ((k >> 4) ^ (k >> 13) ^ (k >> 22)) & (KRC_N - 1u); }
static unsigned krnl_memreg_find_scan(unsigned va, unsigned *pbase, unsigned *plen);
unsigned krnl_memreg_find(unsigned va, unsigned *pbase, unsigned *plen)
{
    krc_t *c;
    unsigned gen, b = 0, l = 0, r;
    if (!g_cfg.krnl_reg_cache || g_cfg.contig_diag) { return krnl_memreg_find_scan(va, pbase, plen); }
    gen = g_exreg_n + 1u;
    c = &g_krc_mf[krc_slot(va)];
    if (c->gen == gen && c->key == va) {
        if (c->res) { if (pbase) { *pbase = c->base; } if (plen) { *plen = c->len; } }
        return c->res;
    }
    r = krnl_memreg_find_scan(va, &b, &l);
    c->gen = gen; c->key = va; c->res = r; c->base = b; c->len = l;
    if (r) { if (pbase) { *pbase = b; } if (plen) { *plen = l; } }
    return r;
}
/* M2c: Range-Match (VEH-Herkunft). */
static unsigned krnl_memreg_find_scan(unsigned va, unsigned *pbase, unsigned *plen)
{
    unsigned k, cnt = (g_exreg_n < EXREG_MAX) ? g_exreg_n : EXREG_MAX;
    for (k = 0; k < cnt; k++) {
        unsigned b = g_exreg[k].vaddr, e = b + g_exreg[k].len;
        if (b && va >= b && va < e) {
            if (pbase) { *pbase = b; }
            if (plen) { *plen = g_exreg[k].len; }
            return g_exreg[k].kind;
        }
    }
    return 0;
}
/* M2c: PHYS (lower28) -> VIRT (ExBloecke). 0 = kein Treffer. */
static unsigned krnl_phys_to_virt_scan(unsigned phys, int *alias);
unsigned krnl_phys_to_virt(unsigned phys)
{
    krc_t *c;
    unsigned gen, pm = phys & 0x0FFFFFFFu, r;
    int alias = 0;
    if (!g_cfg.krnl_reg_cache || g_cfg.contig_diag) { return krnl_phys_to_virt_scan(phys, &alias); }
    gen = g_exreg_n + 1u;
    c = &g_krc_p2v[krc_slot(pm)];
    if (c->gen == gen && c->key == pm && c->len == (unsigned)g_cfg.phys_exact) { return c->res; } /* PERF(PERF-M2,W174) */
    r = krnl_phys_to_virt_scan(phys, &alias);
    if (!alias) { c->gen = gen; c->key = pm; c->res = r; c->len = (unsigned)g_cfg.phys_exact; }
    return r;
}
static unsigned krnl_phys_to_virt_scan(unsigned phys, int *alias)
{
    unsigned k, cnt = (g_exreg_n < EXREG_MAX) ? g_exreg_n : EXREG_MAX;
    unsigned pm = phys & 0x0FFFFFFFu;
    unsigned ex = 0, fm = 0;
    /* FIX(PHYSALIAS,W73): Low28 ist mehrdeutig, sobald Contig-Bloecke oberhalb
     * 256 MB liegen (HIGH-Fallback 0x1xxxxxxx/0x2xxxxxxx); der erste Treffer
     * kann das Ende eines fremden Blocks sein. Hypothese zum Absturz 0x24E1D3
     * (W69_i60, W73_d2/d4/d6b; Film-YUV-Wandlung schrieb bis an die DLL-Basis
     * 0x30000000): Lock der Film-Textur (Data = Blockanfang) traf so einen
     * fremden, zu kleinen Block. Ein Block, der exakt an der Adresse beginnt,
     * ist das gemeinte Ziel (Texturen/Puffer zeigen auf ihren Blockanfang). */
    for (k = 0; k < cnt; k++) {
        unsigned b = g_exreg[k].vaddr;
        if (!b || g_exreg[k].kind != 1) { continue; }
        {
            unsigned bm = b & 0x0FFFFFFFu;
            unsigned em = bm + g_exreg[k].len;
            if (!ex && bm == pm) { ex = b; }
            if (!fm && pm >= bm && pm < em) { fm = b + (pm - bm); }
        }
    }
    if (ex && fm && ex != fm) {
        static unsigned nalias;
        *alias = 1; /* DIAG(W73): Mehrdeutigkeit tatsaechlich aufgetreten */
        if (nalias < 16u || g_cfg.contig_diag) {
            nalias++;
            HLOG_W("PHYSALIAS #%u: phys=0x%X erster Treffer=0x%X exakter Blockanfang=0x%X -> %s",
                   nalias, phys, fm, ex, g_cfg.phys_exact ? "exakt" : "erster (alt)");
        }
    }
    if (g_cfg.phys_exact && ex) { return ex; }
    return fm;
}
static int exreg_find(u32 va)
{
    unsigned k, cnt = (g_exreg_n < EXREG_MAX) ? g_exreg_n : EXREG_MAX;
    for (k = 0; k < cnt; k++) {
        if (g_exreg[k].vaddr == va) {
            return (int)g_exreg[k].kind;
        }
    }
    return 0;
}

/* M2c: Low-Allocator (Xbox maskt 0x0FFFFFFF: Addrs muessen <256MB!).
 * W122: Der alte Weg (contig_fast=0) sucht bei JEDER Anforderung ab
 * 0x0F000000 abwaerts in 64-KB-Schritten (VirtualQuery liefert die Region ab
 * der abgefragten Seite) - und MmFreeContiguousMemory gibt nie frei. Jeder
 * Block verlaengert also alle spaeteren Suchen um mindestens eine Abfrage
 * (W122_r6: 47-61 % der Spiel-Thread-Proben in low_alloc bei Explosionen).
 * Ab Missionsbeginn ist der Bereich voll (W122_r7: HIGH-Fallback, je
 * Anforderung ~3600 erfolglose Abfragen = 40-75 ms; Effekte fordern im
 * ~100-ms-Takt 0xB800/0x16000 an).
 * contig_fast=1: Suche wie bisher ab 0x0F000000, aber belegte Allokationen
 * werden am Stueck uebersprungen (s. low_alloc_scan) - gleiche Adresse,
 * viel weniger Abfragen. contig_fast=2 = Pruefmodus (alte Suche trocken
 * mitlaufen lassen, Adressen vergleichen, Zeile LOWALLOC). contig_fast=3 =
 * EXPERIMENT: zusaetzlich Merkpunkt + Obergrenze der freien Luecke (nicht
 * exakt: Freigaben ausserhalb der HLE, z. B. Treiber, sieht er nicht). */
unsigned g_la_n, g_la_steps, g_la_diff, g_la_skip; /* DIAG: Anforderungen, Abfragen, Abweichungen, ohne Suche abgelehnt */
long long g_la_q;                                  /* DIAG: Zeit in low_alloc (QPC, nur mit perf_log/HUD) */
static char *volatile g_la_hint;                   /* Suchbeginn (NULL = 0x0F000000) */
static volatile u32 g_la_maxfree = 0xFFFFFFFFu;    /* Obergrenze der groessten freien Luecke (unbekannt = max) */
static void low_alloc_note_free(u32 base)
{
    if (base >= 0x01000000u && base < 0x10000000u) {
        g_la_hint = NULL;
        g_la_maxfree = 0xFFFFFFFFu;
    }
}
/* jump=1: in einer belegten Region direkt unter ihre ganze Allokation
 * springen (AllocationBase) statt 64-KB-weise weiterzufragen. Exakt: alle
 * Adressen zwischen AllocationBase und p gehoeren zu derselben, nicht freien
 * Allokation; der alte Weg haette dort nur erfolglos weitergefragt, und sein
 * naechster Punkt unterhalb ist das groesste 64-KB-Vielfache < AllocationBase. */
static char *low_alloc_scan(char *p, u32 need, char **ffree, int dry, int jump, u32 *maxseen, unsigned *steps)
{
    char *lo = (char *)0x01000000;
    MEMORY_BASIC_INFORMATION mi;
    while (p >= lo) {
        if (VirtualQuery(p, &mi, sizeof mi) == 0) { break; }
        (*steps)++;
        if (mi.State == MEM_FREE) {
            if (ffree && !*ffree) { *ffree = p; }
            if (maxseen && (u32)mi.RegionSize > *maxseen) { *maxseen = (u32)mi.RegionSize; }
            if (mi.RegionSize >= need) {
                char *at = (char *)mi.BaseAddress + mi.RegionSize - need;
                void *r;
                if (dry) { return at; }
                r = VirtualAlloc(at, need, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                if (r) { return (char *)r; }
            }
        } else if (jump && mi.AllocationBase && (char *)mi.AllocationBase <= p) {
            p = (char *)(((uintptr_t)mi.AllocationBase - 1u) & ~(uintptr_t)0xFFFFu);
            continue;
        }
        p = (char *)mi.BaseAddress - 0x10000;
    }
    return NULL;
}

unsigned g_la_steps_old; /* DIAG (contig_fast=2): Abfragen der alten Suche */
static void *low_alloc(u32 size)
{
    extern volatile int g_phud_meas;
    char *top = (char *)0x0F000000, *ff = NULL, *start = NULL, *r, *want = NULL;
    u32 need = (size + 0xFFFFu) & ~0xFFFFu;
    int mode = g_cfg.contig_fast;
    LARGE_INTEGER q0, q1;
    if (g_phud_meas) { QueryPerformanceCounter(&q0); }
    if (need < 0x10000u) { need = 0x10000u; }
    g_la_n++;
    if (mode == 2) { want = low_alloc_scan(top, need, NULL, 1, 0, NULL, &g_la_steps_old); } /* DIAG: alte Suche trocken */
    if (mode == 3 && need > g_la_maxfree) { /* EXPERIMENT: keine Luecke gross genug seit der letzten vollen Suche */
        g_la_skip++;
        r = NULL;
    } else if (mode == 3) { /* EXPERIMENT: Merkpunkt (nicht exakt bei Fremd-Freigaben) */
        u32 maxseen = 0;
        start = g_la_hint ? g_la_hint : top;
        r = low_alloc_scan(start, need, &ff, 0, 1, &maxseen, &g_la_steps);
        if (ff) { g_la_hint = ff; }
        if (!r) { g_la_maxfree = maxseen; }
    } else { /* 0 = alt (64-KB-Schritte), 1/2 = Spruenge ueber belegte Allokationen (exakt) */
        r = low_alloc_scan(top, need, NULL, 0, mode != 0, NULL, &g_la_steps);
    }
    if (mode == 2 && r != want) {
        g_la_diff++;
        if (g_la_diff <= 16u) {
            HLOG_W("LOWALLOC diff #%u: size=0x%X neu=0x%X alt=0x%X", g_la_diff, size, (u32)(uintptr_t)r,
                   (u32)(uintptr_t)want);
        }
    }
    if (mode == 2 && (g_la_n % 200u) == 0u) {
        HLOG_I("LOWALLOC n=%u abfragen neu=%u alt=%u abweichend=%u", g_la_n, g_la_steps, g_la_steps_old, g_la_diff);
    }
    if (g_phud_meas) { QueryPerformanceCounter(&q1); g_la_q += q1.QuadPart - q0.QuadPart; }
    return r;
}

u32 __stdcall hle_MmAllocateContiguousMemory(u32 size)
{ 
    static unsigned nreu;
    void *r = low_alloc(size ? size : 0x1000);
    {
        static unsigned nhifall;
        if (!r) {
            r = VirtualAlloc(NULL, size ? size : 0x1000, MEM_COMMIT | MEM_RESERVE,
                             PAGE_READWRITE);
            if (nhifall < 8) { nhifall++; HLOG_W("MmContig HIGH-Fallback (0x%X)", size); }
        }
    }
    (void)nreu;
    if (r) { exreg_add((u32)(uintptr_t)r, size ? size : 0x1000); }
    {
        static unsigned nhia; /* DIAG(W73): Lage aller Contig-Bloecke (nicht-Ex) */
        if (g_cfg.contig_diag) { /* W86-Rueckbau: nur mit DIAG-Schalter (Standard 0 = aus) */
            nhia++;
            HLOG_I("MmContig Block #%u: va=0x%X size=0x%X", nhia, (u32)(uintptr_t)r, size);
        }
    }
    HLOG_T("MmAllocateContiguousMemory(0x%X) -> 0x%X", size, (u32)(uintptr_t)r);
    return (u32)(uintptr_t)r;
}

u32 __stdcall hle_MmAllocateContiguousMemoryEx(u32 size, u32 low, u32 high,
                                                      u32 align, u32 prot)
{
    void *r;
    (void)low;
    (void)high;
    (void)align;
    r = low_alloc(size ? size : 0x1000);
    if (prot) { prot = host_prot("MmContigEx", (u32)(uintptr_t)r, size, prot); } /* W123 */
    if (r && prot && prot != PAGE_READWRITE) {
        DWORD oo;
        VirtualProtect(r, size ? size : 0x1000, prot, &oo);
    }
    if (!r) {
        static unsigned nhifx;
        r = VirtualAlloc(NULL, size ? size : 0x1000, MEM_COMMIT | MEM_RESERVE,
                         prot ? prot : PAGE_READWRITE);
        if (nhifx < 8) { nhifx++; HLOG_W("MmContigEx HIGH-Fallback (0x%X)", size); }
    }
    if (r) { exreg_add((u32)(uintptr_t)r, size ? size : 0x1000); }
    {
        /* DIAG(W73): Lage der High-Bloecke (Absturz 0x24E1D3 schrieb bis an
         * die DLL-Basis 0x30000000, W69_i60/W73_d2) - Adresse + Groesse. */
        static unsigned nhiaddr;
        if (g_cfg.contig_diag) { /* W86-Rueckbau: nur mit DIAG-Schalter (Standard 0 = aus) */
            nhiaddr++;
            HLOG_I("MmContigEx Block #%u: va=0x%X size=0x%X end=0x%X", nhiaddr,
                   (u32)(uintptr_t)r, size, (u32)(uintptr_t)r + size);
        }
    }
    HLOG_T("MmAllocateContiguousMemoryEx(0x%X) -> 0x%X", size, (u32)(uintptr_t)r);
    return (u32)(uintptr_t)r;
}

u32 __stdcall hle_MmFreeContiguousMemory(u32 va)
{
    /* M2c: pures Leak (Xbox: freier Contig-Speicher behaelt Inhalt; das
     * Spiel sampelt stale Structs!). Session-begrenzt, kein LRU (PoC). */
    static unsigned nfree;
    if (nfree < 8) { nfree++; HLOG_I("MmContig free: va=0x%X (behalten)", va); }
    return 0;
}

static HANDLE g_pool_heap;

u32 __stdcall hle_MmAllocateSystemMemory(u32 size, u32 flags)
{
    void *r;
    (void)flags;
    if (!g_pool_heap) {
        g_pool_heap = HeapCreate(0, 0, 0);
    }
    r = HeapAlloc(g_pool_heap, HEAP_ZERO_MEMORY, size ? size : 8);
    if (r) { memreg_add((u32)(uintptr_t)r, size ? size : 8, 5); }
    return (u32)(uintptr_t)r;
}

u32 __stdcall hle_MmFreeSystemMemory(u32 va, u32 flags)
{
    (void)flags;
    if (g_pool_heap && va) {
        HeapFree(g_pool_heap, 0, (void *)(uintptr_t)va);
    }
    return 0;
}

u32 __stdcall hle_ExAllocatePool(u32 size)
{
    if (!g_pool_heap) {
        g_pool_heap = HeapCreate(0, 0, 0);
    }
    {
        void *pr = HeapAlloc(g_pool_heap, HEAP_ZERO_MEMORY, size ? size : 8);
        if (pr) { memreg_add((u32)(uintptr_t)pr, size ? size : 8, 5); }
        return (u32)(uintptr_t)pr;
    }
}

u32 __stdcall hle_ExAllocatePoolWithTag(u32 size, u32 tag)
{
    (void)tag;
    return hle_ExAllocatePool(size);
}

u32 __stdcall hle_ExFreePool(u32 va)
{
    if (g_pool_heap && va) {
        HeapFree(g_pool_heap, 0, (void *)(uintptr_t)va);
    }
    return 0;
}

u32 __stdcall hle_ExQueryPoolBlockSize(u32 va)
{
    SIZE_T s = 0;
    if (g_pool_heap && va) {
        s = HeapSize(g_pool_heap, 0, (void *)(uintptr_t)va);
        if (s == (SIZE_T)-1) {
            s = 0;
        }
    }
    return (u32)s;
}

u32 __stdcall hle_MmQueryAllocationSize(u32 va)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((LPCVOID)(uintptr_t)va, &mbi, sizeof mbi)) {
        return 0;
    }
    return (u32)mbi.RegionSize;
}

u32 __stdcall hle_MmQueryStatistics(u32 buf)
{
    /* MM_STATISTICS (Cxbx types.h): 9x u32 = 36 B. Length ist IN/OUT;
     * nur min(Length,36) schreiben (nie Stack zerstoeren!). Werte: echte
     * Xbox-Groessenordnung (64 MB RAM). */
    static const u32 vals[9] = {
        36, 0x4000, 0x3000, 0x2000000, 0x1000000, 0x100, 0x200, 0x100, 0x400
    };
    if (!buf) {
        return 0xC000000Du; /* INVALID_PARAMETER */
    }
    {
        u32 *m = (u32 *)(uintptr_t)buf;
        u32 n = m[0];
        u32 i;
        if (n < 4 || n > 36) {
            HLOG_W("MmQueryStatistics: Length=%u abgewiesen", n);
            return 0xC000000Du;
        }
        for (i = 0; i < n / 4; i++) {
            m[i] = vals[i];
        }
    }
    HLOG_T("MmQueryStatistics -> Xbox-Werte");
    return X_STATUS_SUCCESS;
}

u32 __stdcall hle_MmQueryAddressProtect(u32 va)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((LPCVOID)(uintptr_t)va, &mbi, sizeof mbi)) {
        return 0;
    }
    { /* W123 DIAG: fragt das Spiel den (bei mem_wc_strip=1 ohne WC gemeldeten) Schutz ab? */
        static unsigned nq;
        if (nq < 8u) { nq++; HLOG_I("MEM query-prot #%u: va=0x%X -> 0x%X", nq, va, (unsigned)mbi.Protect); }
    }
    return mbi.Protect;
}

u32 __stdcall hle_MmSetAddressProtect(u32 va, u32 size, u32 prot)
{
    DWORD old;
    prot = host_prot("MmSetProt", va, size, prot); /* W123 */
    if (!VirtualProtect((LPVOID)(uintptr_t)va, size, prot, &old)) {
        HLOG_W("MmSetAddressProtect(0x%X) FEHLER", va);
        return 0xC0000045u; /* INVALID_PAGE_PROTECTION */
    }
    return X_STATUS_SUCCESS;
}

u32 __stdcall hle_MmPersistContiguousMemory(u32 a0, u32 a1, u32 a2)
{
    (void)a0;
    (void)a1;
    (void)a2;
    return X_STATUS_SUCCESS;
}

u32 __stdcall hle_MmClaimGpuInstanceMemory(u32 size, u32 a1)
{
    void *r = VirtualAlloc(NULL, size ? size : 0x1000, MEM_COMMIT | MEM_RESERVE,
                           PAGE_READWRITE);
    (void)a1;
    HLOG_I("MmClaimGpuInstanceMemory(0x%X) -> 0x%X", size, (u32)(uintptr_t)r);
    return (u32)(uintptr_t)r;
}

unsigned __int64 __stdcall hle_MmGetPhysicalAddress(u32 va)
{
    return (unsigned __int64)va; /* Identitaet: kein echtes DMA in M1 */
}

u32 __stdcall hle_MmLockUnlockBufferPages(u32 a0, u32 a1, u32 a2)
{
    (void)a0;
    (void)a1;
    (void)a2;
    return X_STATUS_SUCCESS;
}

u32 __stdcall hle_MmLockUnlockPhysicalPage(u32 a0, u32 a1)
{
    (void)a0;
    (void)a1;
    return X_STATUS_SUCCESS;
}
