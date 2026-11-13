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
static int ct_find_exact(u32 va); /* W177 contig_noalias (Def. unten) */
static int g_ct_full; /* W177 contig_noalias (s. unten) */
static unsigned ct_phys_to_virt(unsigned pm); /* W177 contig_noalias (Def. unten) */
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
    if (exreg_find(base) == 1 || (g_cfg.contig_noalias && ct_find_exact(base))) { /* W177: auch nach Ring-Ueberlauf behalten */
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
/* DIAG(FILM-WHITE,W177, contig_diag=1): Ring-Ueberlauf (Contig-Eintrag
 * ueberschrieben) und Low28-Ueberlappung neuer Contig-Bloecke mit lebenden. */
static unsigned g_exreg_k1lost;
static void exreg_diag_slot(unsigned slot)
{
    if (g_exreg_n >= EXREG_MAX && g_exreg[slot].kind == 1) {
        g_exreg_k1lost++;
        if (g_exreg_k1lost <= 8u) {
            HLOG_W("PHYSDIAG Ring voll: Contig-Eintrag va=0x%X len=0x%X ueberschrieben (n=%u, verloren %u)",
                   g_exreg[slot].vaddr, g_exreg[slot].len, g_exreg_n, g_exreg_k1lost);
        }
    }
}
static void exreg_diag_alias(u32 va, u32 size)
{
    static unsigned nal;
    unsigned k, cnt = (g_exreg_n < EXREG_MAX) ? g_exreg_n : EXREG_MAX;
    u32 a0 = va & 0x0FFFFFFFu, a1 = a0 + size;
    for (k = 0; k < cnt && nal < 64u; k++) {
        u32 b = g_exreg[k].vaddr, b0, b1;
        if (!b || b == va || g_exreg[k].kind != 1) { continue; }
        b0 = b & 0x0FFFFFFFu; b1 = b0 + g_exreg[k].len;
        if (a0 < b1 && b0 < a1) {
            nal++;
            HLOG_W("PHYSDIAG Alias #%u: neu va=0x%X len=0x%X (Low28 0x%X) ueberlappt alt va=0x%X len=0x%X (Low28 0x%X) reg=%u",
                   nal, va, size, a0, b, g_exreg[k].len, b0, k);
        }
    }
}
static void exreg_add(u32 va, u32 size)
{
    if (g_cfg.contig_diag) { exreg_diag_slot(g_exreg_n % EXREG_MAX); exreg_diag_alias(va, size); } /* DIAG(FILM-WHITE,W177) */
    g_exreg[g_exreg_n % EXREG_MAX].vaddr = va;
    g_exreg[g_exreg_n % EXREG_MAX].len = size;
    g_exreg[g_exreg_n % EXREG_MAX].kind = 1;
    g_exreg_n++;
}
static void memreg_add(u32 va, u32 size, u32 kind)
{
    if (g_cfg.contig_diag) { exreg_diag_slot(g_exreg_n % EXREG_MAX); } /* DIAG(FILM-WHITE,W177) */
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
    if (g_cfg.contig_noalias && !g_ct_full) { return ct_phys_to_virt(pm); } /* FIX(FILM-WHITE,W177) */
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
/* DIAG(FILM-WHITE,W177, contig_diag=1): Upload/Lock einer Textur scheiterte
 * (zu kleiner/fremder Block) -> alle Registry-Eintraege mit passenden
 * Low28-Bits und die Lage der Kandidaten 0x0/0x1/0x2 | Low28. */
void krnl_phys_diag(unsigned xptr, unsigned phys, unsigned need)
{
    static unsigned nd;
    unsigned k, cnt = (g_exreg_n < EXREG_MAX) ? g_exreg_n : EXREG_MAX, pm = phys & 0x0FFFFFFFu, hi;
    if (!g_cfg.contig_diag || nd >= 6u) { return; }
    nd++;
    HLOG_W("PHYSDIAG #%u: xptr=0x%X phys=0x%X need=0x%X -> virt=0x%X reg_n=%u (Ring %u, Contig verloren %u)",
           nd, xptr, phys, need, krnl_phys_to_virt(phys), g_exreg_n, EXREG_MAX, g_exreg_k1lost);
    for (k = 0; k < cnt; k++) {
        unsigned b = g_exreg[k].vaddr, bm = b & 0x0FFFFFFFu;
        if (b && pm >= bm && pm - bm < g_exreg[k].len) {
            HLOG_W("PHYSDIAG #%u   reg[%u] va=0x%X len=0x%X kind=%u %s rest=0x%X", nd, k, b, g_exreg[k].len,
                   g_exreg[k].kind, bm == pm ? "exakt" : "innen", g_exreg[k].len - (pm - bm));
        }
    }
    for (hi = 0; hi <= 0x20000000u; hi += 0x10000000u) {
        MEMORY_BASIC_INFORMATION mi;
        unsigned a = pm | hi;
        memset(&mi, 0, sizeof mi);
        if (VirtualQuery((LPCVOID)(uintptr_t)a, &mi, sizeof mi)) {
            HLOG_W("PHYSDIAG #%u   VQ 0x%X: state=0x%X alloc=0x%X region=0x%X+0x%X prot=0x%X type=0x%X memreg=%u",
                   nd, a, (unsigned)mi.State, (u32)(uintptr_t)mi.AllocationBase, (u32)(uintptr_t)mi.BaseAddress,
                   (u32)mi.RegionSize, (unsigned)mi.Protect, (unsigned)mi.Type, krnl_memreg_find(a, NULL, NULL));
        }
    }
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

/* FIX(FILM-WHITE,W177) [kernel] contig_noalias=1: Contig-Bloecke ohne
 * Low28-Ueberlappung. Das Spiel speichert Textur-/Pufferadressen als
 * "Phys" = VA & 0x0FFFFFFF; krnl_phys_to_virt muss daraus den Block
 * zurueckgewinnen. Ist der Bereich < 256 MB voll, liefert der HIGH-Fallback
 * (VirtualAlloc(NULL)) Bloecke bei 0x1xxxxxxx/0x2xxxxxxx, deren Low28-Bild
 * auf lebende Bloecke (oder spaetere Identitaets-Bloecke aus low_alloc)
 * faellt. Zwei Bloecke mit gleichem Low28-Anfang sind auch fuer phys_exact
 * nicht unterscheidbar: der aeltere gewinnt (W123/W177: Briefing-Film,
 * 1,7-MB-Filmtextur -> fremder 12/36-KB-Block, Lock/Upload scheitern, Bild
 * bleibt weiss). Ausserdem teilen sich Contig- und alle anderen Bloecke
 * (Pool, NtAlloc) den 8192er-Ring g_exreg; in langen Sitzungen fallen
 * Contig-Eintraege heraus (V_C15camp: 19839 Eintraege).
 * Abhilfe: eigene, nicht ueberlaufende Contig-Tabelle + sortierte Liste der
 * belegten Low28-Bilder; low_alloc-Ergebnisse, die das Bild eines HIGH-Blocks
 * treffen, werden stillgelegt (nur reserviert), HIGH-Bloecke werden gezielt
 * dort angelegt, wo ihr Bild frei ist. Alle Allokationswege bleiben, nur die
 * Platzwahl aendert sich. 0 = alt. */
#define CT_MAX 65536
static LONGLONG g_qpc_freq_ct(void)
{
    static LONGLONG f;
    if (!f) { LARGE_INTEGER q; QueryPerformanceFrequency(&q); f = q.QuadPart ? q.QuadPart : 1; }
    return f;
}
static u32 g_ct_va[CT_MAX], g_ct_len[CT_MAX];
/* FIX(SEQ-RENDER,W214) contig_free: 1 = Block per MmFreeContiguousMemory freigegeben -> sein Low28-Bild
 * ist wieder frei (neue Bloecke duerfen es belegen), der Host-Speicher bleibt (stale Reads per VA). */
static volatile unsigned char g_ct_dead[CT_MAX];
static unsigned g_ct_nfree, g_ct_nfree_miss;
static unsigned long long g_ct_free_bytes;
/* FIX(SEQ-RENDER,W214) contig_reuse: Liste freigegebener Bloecke (Index in g_ct_*) + Freigabezeitpunkt (Quarantaene). */
static LONG *g_dl;
static unsigned g_dl_n, g_dl_cap;
static DWORD g_ct_dtick[CT_MAX];
static unsigned g_ct_nreuse;
static unsigned long long g_ct_reuse_bytes;
static volatile LONG g_ct_n;
static SRWLOCK g_ct_lock = SRWLOCK_INIT;
static u32 *g_im_lo, *g_im_hi; /* belegte Low28-Bilder, sortiert, disjunkt (nur unter g_ct_lock) */
static unsigned g_im_n, g_im_cap;
static unsigned g_ct_quar, g_ct_alias, g_ct_high; /* stillgelegt, trotzdem ueberlappend, gezielt HIGH */
#define CT_PAGE(n) (((n) + 0xFFFu) & ~0xFFFu)
#define CT_G64(n) (((unsigned long long)(n) + 0xFFFFull) & ~0xFFFFull)
/* erster Index mit hi > a */
static unsigned im_find(u32 a)
{
    unsigned lo = 0, hi = g_im_n;
    while (lo < hi) {
        unsigned m = (lo + hi) / 2u;
        if (g_im_hi[m] > a) { hi = m; } else { lo = m + 1u; }
    }
    return lo;
}
static int im_overlap(u32 a, u32 len)
{
    unsigned i = im_find(a);
    return i < g_im_n && (unsigned long long)g_im_lo[i] < (unsigned long long)a + len;
}
static int im_reserve(unsigned need)
{
    if (need > g_im_cap) {
        unsigned nc = g_im_cap ? g_im_cap * 2u : 4096u;
        u32 *nl, *nh;
        while (nc < need) { nc *= 2u; }
        nl = (u32 *)realloc(g_im_lo, nc * sizeof(u32));
        if (!nl) { return 0; }
        g_im_lo = nl;
        nh = (u32 *)realloc(g_im_hi, nc * sizeof(u32));
        if (!nh) { return 0; }
        g_im_hi = nh;
        g_im_cap = nc;
    }
    return 1;
}
/* FIX(SEQ-RENDER,W214): [a, a+len) aus der Bildliste nehmen (Intervalle kuerzen, teilen, loeschen). */
static void im_remove(u32 a, u32 len)
{
    u32 b = a + len;
    unsigned i;
    if (!len) { return; }
    if (b < a || b > 0x10000000u) { b = 0x10000000u; }
    i = im_find(a);                                     /* erstes Intervall mit hi > a */
    while (i < g_im_n && g_im_lo[i] < b) {
        u32 lo = g_im_lo[i], hi = g_im_hi[i];
        if (lo < a && hi > b) {                         /* Loch in der Mitte: teilen */
            if (!im_reserve(g_im_n + 1u)) { return; }
            memmove(g_im_lo + i + 2, g_im_lo + i + 1, (g_im_n - i - 1u) * sizeof(u32));
            memmove(g_im_hi + i + 2, g_im_hi + i + 1, (g_im_n - i - 1u) * sizeof(u32));
            g_im_n++;
            g_im_hi[i] = a;
            g_im_lo[i + 1u] = b;
            g_im_hi[i + 1u] = hi;
            return;
        }
        if (lo < a) { g_im_hi[i] = a; i++; continue; }  /* rechtes Ende kappen */
        if (hi > b) { g_im_lo[i] = b; return; }         /* linkes Ende kappen */
        memmove(g_im_lo + i, g_im_lo + i + 1, (g_im_n - i - 1u) * sizeof(u32)); /* ganz bedeckt */
        memmove(g_im_hi + i, g_im_hi + i + 1, (g_im_n - i - 1u) * sizeof(u32));
        g_im_n--;
    }
}
static void im_insert(u32 a, u32 len)
{
    unsigned i, j;
    u32 b = a + len;
    if (!len) { return; }
    if (b < a || b > 0x10000000u) { b = 0x10000000u; } /* Bild endet an der 256-MB-Grenze */
    if (!im_reserve(g_im_n + 1u)) { return; }
    i = im_find(a);                                     /* erstes Intervall mit hi > a */
    if (i > 0 && g_im_hi[i - 1u] == a) { i--; }         /* links angrenzend: verschmelzen */
    j = i;
    while (j < g_im_n && g_im_lo[j] <= b) {             /* ueberlappend/rechts angrenzend */
        if (g_im_lo[j] < a) { a = g_im_lo[j]; }
        if (g_im_hi[j] > b) { b = g_im_hi[j]; }
        j++;
    }
    if (j == i) {                                       /* neu einfuegen */
        memmove(g_im_lo + i + 1, g_im_lo + i, (g_im_n - i) * sizeof(u32));
        memmove(g_im_hi + i + 1, g_im_hi + i, (g_im_n - i) * sizeof(u32));
        g_im_n++;
    } else if (j > i + 1u) {                            /* i..j-1 zu einem zusammenfassen */
        memmove(g_im_lo + i + 1, g_im_lo + j, (g_im_n - j) * sizeof(u32));
        memmove(g_im_hi + i + 1, g_im_hi + j, (g_im_n - j) * sizeof(u32));
        g_im_n -= (j - i - 1u);
    }
    g_im_lo[i] = a;
    g_im_hi[i] = b;
}
/* kleinste 64-KB-ausgerichtete Bildadresse p >= a mit [p, p+len) frei */
static unsigned long long im_first_free(u32 a, u32 len)
{
    unsigned long long p = CT_G64(a);
    unsigned i = im_find((u32)p);
    while (i < g_im_n && p < 0x10000000ull) {
        if ((unsigned long long)g_im_lo[i] >= p + len) { break; }
        p = CT_G64(g_im_hi[i]);
        while (i < g_im_n && g_im_hi[i] <= p) { i++; }
    }
    return p;
}
/* DIAG(SEQ-RENDER,W214, contig_diag=1): Belegung des Low28-Bildes (256 MB)
 * durch Contig-Bloecke - alle 64 Bloecke eine Zeile (Summe der Intervalle,
 * registrierte Bytes, Ueberlappungen). */
static unsigned long long g_ct_bytes;
static unsigned long long im_sum(void)
{
    unsigned long long t = 0;
    unsigned i;
    for (i = 0; i < g_im_n; i++) { t += (unsigned long long)(g_im_hi[i] - g_im_lo[i]); }
    return t;
}
static void ct_diag_note(LONG n)
{
    extern unsigned g_present_no;
    if (!g_cfg.contig_diag || (n & 63) != 0) { return; }
    HLOG_I("CONTIG img: n=%ld bytes=%.1f MB bild=%.1f MB (%u Intervalle) high=%u stillgelegt=%u ueberlappend=%u frei=%u/%.1f MB (unbekannt %u) reuse=%u/%.1f MB f=%u",
           (long)n, (double)g_ct_bytes / 1048576.0, (double)im_sum() / 1048576.0, g_im_n, g_ct_high, g_ct_quar,
           g_ct_alias, g_ct_nfree, (double)g_ct_free_bytes / 1048576.0, g_ct_nfree_miss, g_ct_nreuse,
           (double)g_ct_reuse_bytes / 1048576.0, g_present_no);
}
static void ct_register(u32 va, u32 size)
{
    LONG n = g_ct_n;
    u32 len = CT_PAGE(size ? size : 0x1000u);
    g_ct_bytes += len;
    if (im_overlap(va & 0x0FFFFFFFu, len)) {
        g_ct_alias++;
        if (g_ct_alias <= 8u) {
            HLOG_W("CONTIG noalias: va=0x%X len=0x%X ueberlappt trotzdem (Fallback, #%u)", va, len, g_ct_alias);
        }
    }
    im_insert(va & 0x0FFFFFFFu, len);
    if (n >= CT_MAX) {
        if (!g_ct_full) { g_ct_full = 1; HLOG_W("CONTIG noalias: Tabelle voll (%u) -> Ring-Suche", CT_MAX); }
        return;
    }
    g_ct_va[n] = va;
    g_ct_len[n] = len;
    MemoryBarrier();
    InterlockedExchange(&g_ct_n, n + 1);
    ct_diag_note(n + 1); /* DIAG(W214) */
}
static int ct_find_exact(u32 va)
{
    LONG k, n = g_ct_n;
    for (k = 0; k < n; k++) {
        if (g_ct_va[k] == va) { return 1; }
    }
    return 0;
}
/* Phys -> Virt ueber die Contig-Tabelle (ohne Ueberlappung eindeutig; im
 * Fallback-Fall wie phys_exact: exakter Anfang vor Treffer, juengster zuerst). */
static unsigned ct_phys_to_virt(unsigned pm)
{
    LONG k, n = g_ct_n;
    unsigned ex = 0, fm = 0, dm = 0;
    LONG fk = -1;
    for (k = n - 1; k >= 0; k--) {
        u32 b = g_ct_va[k], bm = b & 0x0FFFFFFFu;
        if (pm >= bm && pm - bm < g_ct_len[k]) {
            if (g_ct_dead[k]) { /* W214: freigegeben - nur, wenn kein lebender Block das Bild belegt (stale Read) */
                if (!dm) { dm = b + (pm - bm); }
                continue;
            }
            if (bm == pm) { ex = b; break; }
            if (!fm) {
                fm = b + (pm - bm);
                fk = k;
                if (!g_ct_alias) { break; }
            }
        }
    }
    if (g_cfg.contig_diag && g_ct_alias && !ex && fm) { /* DIAG(SEQ-RENDER,W214): weiterer lebender Block mit diesem Bild? */
        LONG j;
        static volatile LONG namb;
        for (j = fk - 1; j >= 0; j--) {
            u32 b2 = g_ct_va[j], bm2 = b2 & 0x0FFFFFFFu;
            if (!g_ct_dead[j] && pm >= bm2 && pm - bm2 < g_ct_len[j] && b2 + (pm - bm2) != fm) {
                LONG c = InterlockedIncrement(&namb);
                if (c <= 32 || (c & 1023) == 0) {
                    HLOG_W("CTALIAS #%ld: phys=0x%X -> 0x%X (juengster), auch lebend 0x%X (Block 0x%X+0x%X)",
                           (long)c, pm, fm, b2 + (pm - bm2), b2, g_ct_len[j]);
                }
                break;
            }
        }
    }
    return ex ? ex : fm ? fm : dm;
}
/* FIX(SEQ-RENDER,W214) [kernel] contig_free=1: MmFreeContiguousMemory gibt das Low28-Bild des Blocks frei
 * (Host-Speicher bleibt, s. M2c). Ohne das blieb jedes Bild fuer immer belegt: je Missions-Load ~40-80 MB,
 * nach einigen Missionen war das 256-MB-Bild voll, neue Bloecke ueberlappten wieder (Fallback) und
 * Phys->Virt traf fremde Bloecke -> Texturen weiss/fremd (Alpha-Befund: mehrere Missionen hintereinander).
 * Rueckgabe 1 = Block gefunden. Aufruf ohne g_ct_lock. */
static int ct_free(u32 va)
{
    LONG k, n;
    u32 bm, len;
    if (!va) { return 0; }
    AcquireSRWLockExclusive(&g_ct_lock);
    n = g_ct_n;
    for (k = n - 1; k >= 0; k--) {
        if (g_ct_va[k] == va && !g_ct_dead[k]) { break; }
    }
    if (k < 0) { g_ct_nfree_miss++; ReleaseSRWLockExclusive(&g_ct_lock); return 0; }
    g_ct_dead[k] = 1;
    bm = va & 0x0FFFFFFFu;
    len = g_ct_len[k];
    im_remove(bm, len);
    if (g_ct_alias) { /* Fallback-Ueberlappung: Bilder lebender Bloecke im Bereich wieder eintragen */
        LONG j;
        for (j = 0; j < n; j++) {
            u32 cm = g_ct_va[j] & 0x0FFFFFFFu;
            if (!g_ct_dead[j] && cm < bm + len && cm + g_ct_len[j] > bm) { im_insert(cm, g_ct_len[j]); }
        }
    }
    g_ct_nfree++;
    g_ct_free_bytes += len;
    g_ct_dtick[k] = GetTickCount();
    if (g_dl_n + 1u > g_dl_cap) {
        unsigned nc = g_dl_cap ? g_dl_cap * 2u : 4096u;
        LONG *nd = (LONG *)realloc(g_dl, nc * sizeof(LONG));
        if (nd) { g_dl = nd; g_dl_cap = nc; }
    }
    if (g_dl_n < g_dl_cap) { g_dl[g_dl_n++] = k; }
    ReleaseSRWLockExclusive(&g_ct_lock);
    return 1;
}
/* FIX(SEQ-RENDER,W214) [kernel] contig_reuse=1: Anforderung aus einem freigegebenen Block bedienen (wie
 * physischer Speicher auf der Xbox), statt neuen Adressraum zu belegen. Bedingungen: Laenge passt (exakt
 * bevorzugt, sonst kleinster Block bis zur doppelten Groesse), seit der Freigabe >= contig_reuse_ms
 * (Quarantaene fuer spaete Lese-/GPU-Zugriffe), Low28-Bild weiterhin frei. Ohne das wuchsen Adressraum und
 * Commit je Missions-Load (~60 MB) und im Kampf je Effekt weiter (32-Bit-Prozess). Unter g_ct_lock.
 * Rueckgabe Blockadresse, *plen = Blocklaenge (Seiten). */
static void *ct_reuse(u32 size, u32 *plen)
{
    u32 need = CT_PAGE(size ? size : 0x1000u), bestlen = 0xFFFFFFFFu;
    DWORD now = GetTickCount(), q = (DWORD)(g_cfg.contig_reuse_ms > 0 ? g_cfg.contig_reuse_ms : 0);
    unsigned i, best = 0xFFFFFFFFu;
    LONG k;
    if (!g_cfg.contig_free || !g_cfg.contig_reuse || !g_dl_n) { return NULL; }
    for (i = 0; i < g_dl_n; i++) {
        u32 len;
        k = g_dl[i];
        len = g_ct_len[k];
        if (len < need || len - need > need || len >= bestlen) { continue; }
        if (now - g_ct_dtick[k] < q) { continue; }
        if (im_overlap(g_ct_va[k] & 0x0FFFFFFFu, len)) { continue; }
        best = i;
        bestlen = len;
        if (len == need) { break; }
    }
    if (best == 0xFFFFFFFFu) { return NULL; }
    k = g_dl[best];
    g_dl[best] = g_dl[--g_dl_n];
    g_ct_dead[k] = 0;
    im_insert(g_ct_va[k] & 0x0FFFFFFFu, g_ct_len[k]);
    g_ct_nreuse++;
    g_ct_reuse_bytes += g_ct_len[k];
    if (plen) { *plen = g_ct_len[k]; }
    if (g_cfg.contig_diag && (g_ct_nreuse <= 16u || (g_ct_nreuse & 255u) == 0)) {
        HLOG_I("CONTIG reuse #%u: size=0x%X -> va=0x%X len=0x%X (frei seit %lu ms)", g_ct_nreuse, size,
               g_ct_va[k], g_ct_len[k], (unsigned long)(now - g_ct_dtick[k]));
    }
    return (void *)(uintptr_t)g_ct_va[k];
}
/* low_alloc ohne Treffer auf HIGH-Bilder: Treffer stilllegen (Adressraum
 * reserviert lassen, Seiten freigeben) und weitersuchen. */
static void *low_alloc(u32 size);
static void *ct_low_alloc(u32 size)
{
    int tries;
    for (tries = 0; tries < 32; tries++) {
        void *r = low_alloc(size);
        u32 va, len;
        if (!r) { return NULL; }
        va = (u32)(uintptr_t)r;
        len = CT_PAGE(size);
        if (!im_overlap(va & 0x0FFFFFFFu, len)) { return r; }
        VirtualFree(r, (size + 0xFFFFu) & ~0xFFFFu, MEM_DECOMMIT);
        g_ct_quar++;
        if (g_ct_quar <= 8u || g_cfg.contig_diag) {
            HLOG_W("CONTIG noalias: low 0x%X+0x%X trifft HIGH-Bild -> stillgelegt (#%u)", va, len, g_ct_quar);
        }
    }
    return NULL;
}
/* HIGH-Block an einer Adresse >= 256 MB, deren Low28-Bild frei ist. */
/* Untergrenze der HIGH-Suche (256 MB). Der eigentliche Suchbeginn kommt je
 * Anforderung aus einer VirtualAlloc(NULL)-Probe (s. unten); ein gemerkter
 * "niedrigster freier Bereich" half nicht (W177_kill: ~600 Abfragen je Suche,
 * kleine freie Reste bleiben ganz unten liegen). */
static unsigned g_ct_qn, g_ct_qslow;
static LONGLONG g_ct_qsum, g_ct_qmax; /* QPC in ct_high_alloc (Summe/Max) */
/* Fortsetzungspunkt hinter dem letzten gezielt angelegten HIGH-Block: Contig-
 * Bloecke werden nie freigegeben, direkt dahinter sind Adressraum und Low28-Bild
 * meist frei -> 1-2 Abfragen statt Dutzender (W177_n6: Start an der Probe kostete
 * im Mittel ~1,4 Mio. Zyklen). Findet die Suche ab dort nichts, einmal von unten. */
static u32 g_ct_cursor = 0x10000000u;
static void *ct_high_alloc_from(u32 size, DWORD prot, unsigned *pq, unsigned long long p);
static void *ct_high_alloc_q(u32 size, DWORD prot, unsigned *pq)
{
    void *r = ct_high_alloc_from(size, prot, pq, g_ct_cursor);
    if (!r && g_ct_cursor > 0x10000000u) { r = ct_high_alloc_from(size, prot, pq, 0x10000000ull); }
    if (r) { g_ct_cursor = (u32)CT_G64((unsigned long long)(uintptr_t)r + CT_PAGE(size)); }
    return r;
}
static void *ct_high_alloc_from(u32 size, DWORD prot, unsigned *pq, unsigned long long p)
{
    u32 need = CT_PAGE(size);
    unsigned it;
    void *res = NULL;
    if (p <= 0x10000000ull) { /* nur bei Suche von unten: Suchbeginn wie der alte HIGH-Fallback: VirtualAlloc(NULL) liefert die niedrigste
       * freie Luecke dieser Groesse (Kernel-Suche, schnell); alles darunter ist zu klein.
       * Nur reservieren, sofort freigeben, dann ab dort Bild-frei suchen. */
        void *pr = VirtualAlloc(NULL, need, MEM_RESERVE, PAGE_NOACCESS);
        if (pr) {
            unsigned long long a = (unsigned long long)(uintptr_t)pr;
            VirtualFree(pr, 0, MEM_RELEASE);
            if (a > p) { p = a; }
        }
    }
    for (it = 0; it < 200000u && p + need <= 0x7FFF0000ull; it++) {
        MEMORY_BASIC_INFORMATION mi;
        unsigned long long re, c, win, q;
        void *r;
        if (!VirtualQuery((LPCVOID)(uintptr_t)p, &mi, sizeof mi)) { break; }
        (*pq)++;
        re = (unsigned long long)(uintptr_t)mi.BaseAddress + mi.RegionSize;
        if (mi.State != MEM_FREE) { p = re; continue; }
        c = CT_G64(p);
        if (c + need > re) { p = re; continue; }
        win = c & 0xF0000000ull;
        q = im_first_free((u32)(c - win), need);
        if (q + need > 0x10000000ull) { p = win + 0x10000000ull; continue; }
        if (win + q != c) { p = win + q; continue; }
        r = VirtualAlloc((LPVOID)(uintptr_t)c, size, MEM_COMMIT | MEM_RESERVE, prot);
        if (r) { g_ct_high++; res = r; break; }
        p = c + 0x10000ull;
    }
    return res;
}
/* HIGH-Platzierung beider Modi mit Kostenmessung (Wandzeit + CPU-Zyklen des
 * Threads, damit Fremdlast/Verdraengung die CPU-Kosten nicht verfaelscht):
 * na=1 gezielt (Bild frei), sonst/als Rueckfall VirtualAlloc(NULL) wie alt. */
static ULONG64 g_ct_cysum, g_ct_cymax;
static void *ct_high_alloc(u32 size, DWORD prot)
{
    LARGE_INTEGER q0, q1;
    ULONG64 c0 = 0, c1 = 0;
    unsigned nq = 0;
    void *r = NULL;
    QueryPerformanceCounter(&q0);
    QueryThreadCycleTime(GetCurrentThread(), &c0);
    if (g_cfg.contig_noalias) { r = ct_high_alloc_q(size, prot, &nq); }
    if (!r) { r = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, prot); }
    QueryThreadCycleTime(GetCurrentThread(), &c1);
    QueryPerformanceCounter(&q1);
    {
        LONGLONG d = q1.QuadPart - q0.QuadPart;
        ULONG64 cy = c1 - c0;
        g_ct_qn++; g_ct_qsum += d; if (d > g_ct_qmax) { g_ct_qmax = d; }
        g_ct_cysum += cy; if (cy > g_ct_cymax) { g_ct_cymax = cy; }
        if (d * 1000 > g_qpc_freq_ct() && g_ct_qslow++ < 8u) { /* > 1 ms Wandzeit */
            HLOG_W("CONTIG HIGH-Suche langsam: %.2f ms (CPU %.0f kZyklen), %u Abfragen, size=0x%X -> 0x%X",
                   (double)d * 1000.0 / (double)g_qpc_freq_ct(), (double)cy / 1000.0, nq, size, (u32)(uintptr_t)r);
        }
    }
    return r;
}
/* TEST(FILM-WHITE,W177) [kernel] contig_alias_test=<Groesse>: stellt die
 * ungluenstige Lage aus V_C30isokill/W123_ab_wc0 gezielt nach. Fordert das
 * Spiel einen Contig-Block genau dieser Groesse an (Filmtextur), entsteht
 * VOR ihm ein 12-KB-"HIGH"-Block mit gleichem Low28-Anfang (wie ein frueher
 * angelegter HIGH-Fallback-Block). Alt: phys_exact findet den Koeder zuerst
 * -> Lock/Upload scheitern (weisser Film). contig_noalias=1: der Koeder ist
 * registriert, der Filmblock wird neu platziert (Bild frei). 0 = aus. */
static void *ct_alias_test(void *r, u32 size, DWORD prot, int na)
{
    static unsigned nt;
    u32 va = (u32)(uintptr_t)r, dv;
    void *d;
    if (!r || nt >= 4u) { return r; }
    nt++;
    { /* erstes freies 256-MB-Fenster (ausser dem eigenen) mit gleichem Low28-Anfang */
        u32 w;
        d = NULL; dv = 0;
        for (w = 0x10000000u; w < 0x80000000u && !d; w += 0x10000000u) {
            if (w == (va & 0xF0000000u)) { continue; }
            dv = (va & 0x0FFFFFFFu) | w;
            d = VirtualAlloc((LPVOID)(uintptr_t)dv, 0x3000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        }
    }
    HLOG_W("CONTIG alias-test #%u: Block va=0x%X size=0x%X -> Koeder 0x%X %s", nt, va, size, dv, d ? "angelegt" : "FEHLER");
    if (!d) { return r; }
    exreg_add(dv, 0x3000);
    if (na) {
        ct_register(dv, 0x3000);
        VirtualFree(r, 0, MEM_RELEASE);
        r = ct_low_alloc(size);
        if (r && prot != PAGE_READWRITE) { DWORD oo; VirtualProtect(r, size, prot, &oo); }
        if (!r) { r = ct_high_alloc(size, prot); }
        HLOG_W("CONTIG alias-test #%u: noalias -> Block neu bei 0x%X", nt, (u32)(uintptr_t)r);
    }
    return r;
}
void krnl_contig_stats(void)
{
    if (g_cfg.contig_noalias) {
        HLOG_I("KRNL CONTIG noalias: %ld Bloecke, %u Bild-Intervalle, %u HIGH gezielt, %u stillgelegt, %u trotzdem ueberlappend%s",
               (long)g_ct_n, g_im_n, g_ct_high, g_ct_quar, g_ct_alias, g_ct_full ? ", Tabelle voll" : "");
        if (g_cfg.contig_diag) { /* DIAG(W214) */
            HLOG_I("KRNL CONTIG bild: %.1f MB belegt, %.1f MB registriert, %u Freigaben (%.1f MB, %u unbekannt), %u wiederverwendet (%.1f MB)",
                   (double)im_sum() / 1048576.0, (double)g_ct_bytes / 1048576.0, g_ct_nfree,
                   (double)g_ct_free_bytes / 1048576.0, g_ct_nfree_miss, g_ct_nreuse, (double)g_ct_reuse_bytes / 1048576.0);
        }
    }
    HLOG_I("KRNL CONTIG HIGH (noalias=%d): %u x, Mittel %.3f ms / %.0f kZyklen, Max %.3f ms / %.0f kZyklen, >1 ms %u x",
           g_cfg.contig_noalias, g_ct_qn,
           g_ct_qn ? (double)g_ct_qsum * 1000.0 / (double)g_qpc_freq_ct() / (double)g_ct_qn : 0.0,
           g_ct_qn ? (double)g_ct_cysum / 1000.0 / (double)g_ct_qn : 0.0,
           (double)g_ct_qmax * 1000.0 / (double)g_qpc_freq_ct(), (double)g_ct_cymax / 1000.0, g_ct_qslow);
}

u32 __stdcall hle_MmAllocateContiguousMemory(u32 size)
{ 
    static unsigned nreu;
    int na = g_cfg.contig_noalias; /* FIX(FILM-WHITE,W177) */
    void *r = NULL;
    u32 rlen = 0; /* W214 contig_reuse: Laenge des wiederverwendeten Blocks (0 = neu) */
    if (na) { AcquireSRWLockExclusive(&g_ct_lock); r = ct_reuse(size, &rlen); }
    if (r) { DWORD oo; VirtualProtect(r, rlen, PAGE_READWRITE, &oo); } /* Block war evtl. WC (W123) */
    if (!r) { r = na ? ct_low_alloc(size ? size : 0x1000) : low_alloc(size ? size : 0x1000); }
    {
        static unsigned nhifall;
        if (!r) {
            r = ct_high_alloc(size ? size : 0x1000, PAGE_READWRITE); /* W177: na=1 gezielt, sonst VirtualAlloc(NULL) wie alt */
            if (nhifall < 8) { nhifall++; HLOG_W("MmContig HIGH-Fallback (0x%X)", size); }
        }
    }
    (void)nreu;
    if (g_cfg.contig_alias_test && !rlen && size == (u32)g_cfg.contig_alias_test) { r = ct_alias_test(r, size, PAGE_READWRITE, na); } /* TEST(W177) */
    if (r) { exreg_add((u32)(uintptr_t)r, size ? size : 0x1000); }
    if (na) {
        if (r && !rlen) { ct_register((u32)(uintptr_t)r, size ? size : 0x1000); }
        ReleaseSRWLockExclusive(&g_ct_lock);
    }
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
    void *r = NULL;
    u32 rlen = 0; /* W214 contig_reuse */
    int na = g_cfg.contig_noalias; /* FIX(FILM-WHITE,W177) */
    (void)low;
    (void)high;
    (void)align;
    if (na) { AcquireSRWLockExclusive(&g_ct_lock); r = ct_reuse(size, &rlen); }
    if (!r) { r = na ? ct_low_alloc(size ? size : 0x1000) : low_alloc(size ? size : 0x1000); }
    if (prot) { prot = host_prot("MmContigEx", (u32)(uintptr_t)r, size, prot); } /* W123 */
    if (r && rlen) { /* W214: wiederverwendeter Block - Schutz immer neu setzen (vorher evtl. WC) */
        DWORD oo;
        VirtualProtect(r, rlen, prot ? prot : PAGE_READWRITE, &oo);
    } else if (r && prot && prot != PAGE_READWRITE) {
        DWORD oo;
        VirtualProtect(r, size ? size : 0x1000, prot, &oo);
    }
    if (!r) {
        static unsigned nhifx;
        r = ct_high_alloc(size ? size : 0x1000, prot ? prot : PAGE_READWRITE); /* W177: na=1 gezielt, sonst VirtualAlloc(NULL) wie alt */
        if (nhifx < 8) { nhifx++; HLOG_W("MmContigEx HIGH-Fallback (0x%X)", size); }
    }
    if (g_cfg.contig_alias_test && !rlen && size == (u32)g_cfg.contig_alias_test) { r = ct_alias_test(r, size, prot ? prot : PAGE_READWRITE, na); } /* TEST(W177) */
    if (r) { exreg_add((u32)(uintptr_t)r, size ? size : 0x1000); }
    if (na) {
        if (r && !rlen) { ct_register((u32)(uintptr_t)r, size ? size : 0x1000); }
        ReleaseSRWLockExclusive(&g_ct_lock);
    }
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
    if (g_cfg.contig_noalias && g_cfg.contig_free) { (void)ct_free(va); } /* FIX(SEQ-RENDER,W214): Bild frei, Speicher bleibt */
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
