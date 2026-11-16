/* loc_mem.c - W277 LOC-DISC: Contig-Speicher mit Xbox-Spiegeln fuer Line of Contact.
 *
 * Befunde (W277A/W277E): LoC (XDK 5788) arbeitet mit physischen Adressen:
 *  - D3DResource_Register speichert Data = VA & 0x0FFFFFFF (phys),
 *  - Zugriffe der CPU ueber die Xbox-Spiegel 0x80000000|phys (cached, z. B.
 *    FUN_00084CC0 nach CreateVertexBuffer2: "or edi,0x80000000") und
 *    0xF0000000|phys (write-combined, Lock2DSurface 0x18B940 -> XFONT).
 * Im Client lag dort nichts -> Zugriffsverletzungen.
 *
 * Loesung: eine Pagefile-Section wird dreimal eingeblendet: bei P (< 256 MB,
 * damit phys == VA gilt - wie im SB-Pfad, Renderer/Texturpfad sehen die
 * gewohnten Zeiger), bei 0x80000000|P und bei 0xF0000000|P. Alle Ansichten
 * teilen denselben Speicher. Contig-Bloecke (MmAllocateContiguousMemory[Ex]
 * = XPhysicalAlloc) und VB/IB-Daten (CreateVertex/IndexBuffer2) kommen im
 * LoC-Profil von hier. Freigaben kommen in eine Liste und werden bei gleicher
 * Groesse wieder verwendet (Inhalt bleibt wie auf der Xbox stehen). SB nutzt
 * das nie. */
#include "loc_mem.h"
#include "hlog.h"
#include "hlock.h" /* W308 SUSP-CTLOCK */

#include <windows.h>
#include <string.h>
#include <stdlib.h>

typedef uint32_t u32;

#define LOC_MEM_PHYS 0x0FFFFFFFu /* Xbox: phys = VA & 0x0FFFFFFF */
static u32 LOC_MEM_SIZE;            /* je Ansicht, beim Start gewaehlt */
static const u32 k_mirror[2] = { 0x80000000u, 0xF0000000u };

typedef struct {
    u32 va, size;
    int used;
} loc_blk_t;

static HANDLE g_sec;
static u32 g_lo;   /* untere Ansicht */
static u32 g_off;  /* Fuellstand */
static loc_blk_t *g_blk;
static unsigned g_nblk, g_capblk;
static hlock_t g_lock; /* W308 SUSP-CTLOCK: hlock statt SRWLOCK (Xbox-Threads, SUSPSAFE sieht den Owner) */
static unsigned g_nalloc, g_nreuse, g_nfree, g_nfull;
static unsigned long long g_bytes_reuse;

static int range_free(u32 base, u32 size)
{
    u32 p = base;
    while (p - base < size) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((LPCVOID)(uintptr_t)p, &mbi, sizeof mbi) || mbi.State != MEM_FREE) {
            return 0;
        }
        p = (u32)(uintptr_t)mbi.BaseAddress + (u32)mbi.RegionSize;
        if (p <= (u32)(uintptr_t)mbi.BaseAddress) {
            break; /* Ueberlauf am Adressraumende */
        }
    }
    return 1;
}

/* Vorab-Reservierung (vor D3D/Treiber, die sonst den Adressraum < 256 MB
 * belegen): je Groesse den ersten Platz P mit freien Spiegeln suchen und alle
 * drei Bereiche reservieren. loc_mem_init gibt sie frei und blendet sofort ein. */
static u32 g_pre_p, g_pre_size;
static int find_slot(u32 size, u32 *out)
{
    u32 cand;
    for (cand = 0x01000000u; cand + size <= 0x10000000u; cand += 0x00400000u) {
        if (range_free(cand, size) && range_free(cand | k_mirror[0], size) && range_free(cand | k_mirror[1], size)) {
            *out = cand;
            return 1;
        }
    }
    return 0;
}

void loc_mem_prereserve(void)
{
    static const u32 k_sizes[] = { 224u << 20, 192u << 20, 160u << 20, 128u << 20, 96u << 20 };
    unsigned si;
    for (si = 0; si < sizeof k_sizes / sizeof k_sizes[0]; si++) {
        u32 p;
        if (find_slot(k_sizes[si], &p)) {
            void *a = VirtualAlloc((void *)(uintptr_t)p, k_sizes[si], MEM_RESERVE, PAGE_NOACCESS);
            void *b = a ? VirtualAlloc((void *)(uintptr_t)(p | k_mirror[0]), k_sizes[si], MEM_RESERVE, PAGE_NOACCESS) : NULL;
            void *c = b ? VirtualAlloc((void *)(uintptr_t)(p | k_mirror[1]), k_sizes[si], MEM_RESERVE, PAGE_NOACCESS) : NULL;
            if (c) {
                g_pre_p = p;
                g_pre_size = k_sizes[si];
                HLOG_I("LOCMEM: Vorab-Reservierung 0x%X (%u MB) + Spiegel (loc_boot=1)", p, g_pre_size >> 20);
                return;
            }
            if (b) VirtualFree(b, 0, MEM_RELEASE);
            if (a) VirtualFree(a, 0, MEM_RELEASE);
        }
    }
    HLOG_W("LOCMEM: Vorab-Reservierung nicht moeglich");
}

void loc_mem_drop_prereserve(void)
{
    if (g_pre_p) {
        VirtualFree((void *)(uintptr_t)g_pre_p, 0, MEM_RELEASE);
        VirtualFree((void *)(uintptr_t)(g_pre_p | k_mirror[0]), 0, MEM_RELEASE);
        VirtualFree((void *)(uintptr_t)(g_pre_p | k_mirror[1]), 0, MEM_RELEASE);
        HLOG_I("LOCMEM: Vorab-Reservierung freigegeben");
        g_pre_p = 0;
    }
}

static int map3(u32 cand, u32 size)
{
    void *v[3];
    v[0] = MapViewOfFileEx(g_sec, FILE_MAP_ALL_ACCESS, 0, 0, size, (void *)(uintptr_t)cand);
    v[1] = v[0] ? MapViewOfFileEx(g_sec, FILE_MAP_ALL_ACCESS, 0, 0, size, (void *)(uintptr_t)(cand | k_mirror[0])) : NULL;
    v[2] = v[1] ? MapViewOfFileEx(g_sec, FILE_MAP_ALL_ACCESS, 0, 0, size, (void *)(uintptr_t)(cand | k_mirror[1])) : NULL;
    if (!v[2]) {
        if (v[1]) UnmapViewOfFile(v[1]);
        if (v[0]) UnmapViewOfFile(v[0]);
        return 0;
    }
    return 1;
}

int loc_mem_init(void)
{
    static const u32 k_sizes[] = { 224u << 20, 192u << 20, 160u << 20, 128u << 20, 96u << 20 };
    unsigned si;
    if (g_lo) {
        return 1;
    }
    if (g_pre_p) { /* Vorab-Reservierung in Ansichten umwandeln */
        u32 p = g_pre_p, size = g_pre_size;
        loc_mem_drop_prereserve();
        g_sec = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE | SEC_COMMIT, 0, size, NULL);
        if (g_sec && map3(p, size)) {
            g_lo = p;
            LOC_MEM_SIZE = size;
        } else if (g_sec) {
            CloseHandle(g_sec);
            g_sec = NULL;
        }
    }
    for (si = 0; !g_lo && si < sizeof k_sizes / sizeof k_sizes[0]; si++) {
        u32 size = k_sizes[si], cand;
        g_sec = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE | SEC_COMMIT, 0, size, NULL);
        if (!g_sec) {
            continue;
        }
        /* P im Bereich 16..256 MB (ueber dem XBE-Abbild, phys == VA). */
        for (cand = 0x01000000u; cand + size <= 0x10000000u; cand += 0x00400000u) {
            if (!range_free(cand, size) || !range_free(cand | k_mirror[0], size) ||
                !range_free(cand | k_mirror[1], size) || !map3(cand, size)) {
                continue;
            }
            g_lo = cand;
            LOC_MEM_SIZE = size;
            break;
        }
        if (!g_lo) {
            CloseHandle(g_sec);
            g_sec = NULL;
        }
    }
    if (!g_lo) {
        HLOG_E("LOCMEM: kein freier Bereich < 256 MB mit Spiegeln 0x8/0xF gefunden");
        { /* Diagnose: freie Bereiche >= 16 MB */
            u32 p = 0x10000u;
            unsigned k = 0;
            while (p < 0xFFFF0000u && k < 40) {
                MEMORY_BASIC_INFORMATION mbi;
                u32 b, e;
                if (!VirtualQuery((LPCVOID)(uintptr_t)p, &mbi, sizeof mbi)) {
                    break;
                }
                b = (u32)(uintptr_t)mbi.BaseAddress;
                e = b + (u32)mbi.RegionSize;
                if (mbi.State == MEM_FREE && mbi.RegionSize >= (16u << 20)) {
                    HLOG_E("LOCMEM: frei 0x%08X-0x%08X (%u MB)", b, e, (u32)(mbi.RegionSize >> 20));
                    k++;
                }
                if (e <= p) {
                    break;
                }
                p = e;
            }
        }
        return 0;
    }
    HLOG_I("LOCMEM: Contig-Bereich 0x%X-0x%X (phys == VA) mit Xbox-Spiegeln 0x%X und 0x%X (%u MB, eine Section, "
           "drei Ansichten)", g_lo, g_lo + LOC_MEM_SIZE, g_lo | k_mirror[0], g_lo | k_mirror[1], LOC_MEM_SIZE >> 20);
    return 1;
}

int loc_mem_active(void)
{
    return g_lo != 0;
}

static int blk_add(u32 va, u32 size)
{
    if (g_nblk == g_capblk) {
        unsigned nc = g_capblk ? g_capblk * 2u : 4096u;
        loc_blk_t *n = (loc_blk_t *)realloc(g_blk, nc * sizeof *n);
        if (!n) {
            return 0;
        }
        g_blk = n;
        g_capblk = nc;
    }
    g_blk[g_nblk].va = va;
    g_blk[g_nblk].size = size;
    g_blk[g_nblk].used = 1;
    g_nblk++;
    return 1;
}

u32 loc_mem_alloc(u32 size, u32 align, int zero)
{
    u32 r = 0, i, base;
    if (!g_lo) {
        return 0;
    }
    if (!size) {
        size = 0x10;
    }
    size = (size + 15u) & ~15u;
    if (align < 16u) {
        align = 16u;
    }
    if (align & (align - 1u)) {
        align = 4096u;
    }
    hlock_enter(&g_lock);
    /* Wiederverwendung: freier Block gleicher Groesse und passender Ausrichtung (neueste zuerst). */
    for (i = g_nblk; i-- > 0;) {
        if (!g_blk[i].used && g_blk[i].size == size && !(g_blk[i].va & (align - 1u))) {
            g_blk[i].used = 1;
            r = g_blk[i].va;
            g_nreuse++;
            g_bytes_reuse += size;
            break;
        }
    }
    if (!r) {
        base = (g_lo + g_off + align - 1u) & ~(align - 1u);
        if (base + size > g_lo + LOC_MEM_SIZE || base < g_lo) {
            g_nfull++;
        } else if (blk_add(base, size)) {
            g_off = base + size - g_lo;
            r = base;
            g_nalloc++;
        }
    }
    hlock_leave(&g_lock);
    if (!r) {
        static volatile LONG nlog;
        if (InterlockedIncrement(&nlog) <= 8) {
            HLOG_E("LOCMEM: Bereich voll (size=0x%X, Fuellstand %u MB)", size, g_off >> 20);
        }
        return 0;
    }
    if (zero) {
        memset((void *)(uintptr_t)r, 0, size);
    }
    return r;
}

static int blk_find(u32 va)
{
    unsigned lo = 0, hi = g_nblk; /* va aufsteigend (Bump-Allokator) */
    va &= LOC_MEM_PHYS; /* Spiegeladresse -> untere Ansicht (P < 256 MB) */
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2u;
        if (g_blk[mid].va == va) {
            return (int)mid;
        }
        if (g_blk[mid].va < va) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return -1;
}

int loc_mem_free(u32 va)
{
    int k;
    if (!g_lo || !loc_mem_owns(va)) {
        return 0;
    }
    hlock_enter(&g_lock);
    k = blk_find(va);
    if (k >= 0 && g_blk[k].used) {
        g_blk[k].used = 0;
        g_nfree++;
    }
    hlock_leave(&g_lock);
    return 1;
}

u32 loc_mem_size(u32 va)
{
    int k;
    u32 s = 0;
    if (!g_lo || !loc_mem_owns(va)) {
        return 0;
    }
    hlock_enter(&g_lock);
    k = blk_find(va);
    if (k >= 0) {
        s = g_blk[k].size;
    }
    hlock_leave(&g_lock);
    return s;
}

int loc_mem_owns(u32 va)
{
    u32 top = va & 0xF0000000u, v = va & LOC_MEM_PHYS;
    if (top != 0u && top != k_mirror[0] && top != k_mirror[1]) {
        return 0;
    }
    return g_lo && v >= g_lo && v < g_lo + LOC_MEM_SIZE;
}

void loc_mem_stats(void)
{
    if (!g_lo) {
        return;
    }
    HLOG_I("LOCMEM: %u Bloecke neu, %u wiederverwendet (%.1f MB), %u Freigaben, Fuellstand %.1f MB von %u MB%s",
           g_nalloc, g_nreuse, (double)g_bytes_reuse / 1048576.0, g_nfree, (double)g_off / 1048576.0,
           LOC_MEM_SIZE >> 20, g_nfull ? " - VOLL erreicht" : "");
}
