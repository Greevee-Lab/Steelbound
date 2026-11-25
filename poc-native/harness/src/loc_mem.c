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
 * das nie.
 *
 * W493 LOC-MEMLEAK (#101): Der Bereich lief nach ~1 h voll (114 778 Bloecke,
 * 5 Freigaben). Ursache: CreateVertex/IndexBuffer2 sind ganz ersetzt
 * (create_res2), D3DResource_Release ebenso - die XDK-Zerstoerung
 * (0x188270: XMemFree(Data|0x80000000), XMemFree(Kopf)) lief nie, und der
 * Ersatzkopf hatte Common = 0 statt 0x01000001 (Refcount 1). Jede geladene
 * Mission legte ihre VB/IB neu an. Jetzt (loc_mem_reclaim=1): Kopf wie das
 * Original (Common, Data, Lock), Release auf 0 gibt die Daten zurueck. Freie
 * Bloecke gehen erst in eine Quarantaene (loc_mem_quar Bilder, Inhalt bleibt
 * stehen), danach in Freilisten nach Groessenklassen (4 je Zweierpotenz,
 * VB/IB auf die Klassengroesse gerundet); der oberste freie Block senkt den
 * Fuellstand. Im spekulativen Vorlauf (loc_rb_spec) wird nichts veraendert. */
#include "loc_mem.h"
#include "hlog.h"
#include "hlock.h" /* W308 SUSP-CTLOCK */

#include <windows.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h> /* W493: _snprintf_s */

typedef uint32_t u32;

#define LOC_MEM_PHYS 0x0FFFFFFFu /* Xbox: phys = VA & 0x0FFFFFFF */
static u32 LOC_MEM_SIZE;            /* je Ansicht, beim Start gewaehlt */
static const u32 k_mirror[2] = { 0x80000000u, 0xF0000000u };

enum { BS_FREE = 0, BS_USED = 1, BS_QUAR = 2 };
typedef struct {
    u32 va, size;
    uint8_t st;    /* BS_* */
    uint8_t tag;   /* LOCMEM_T_* */
    uint16_t site; /* Index in g_site (0 = sonstige) */
    int32_t prev, next; /* W493: Freiliste der Klasse (nur BS_FREE im Rueckgabe-Modus), -1 = Ende */
    int32_t hdr;   /* W493: Kopf-Index (VB/IB) bis zum Ende der Quarantaene, sonst -1 */
} loc_blk_t;

static HANDLE g_sec;
static u32 g_lo;   /* untere Ansicht */
static u32 g_off;  /* Fuellstand */
static loc_blk_t *g_blk;
static unsigned g_nblk, g_capblk;
static hlock_t g_lock; /* W308 SUSP-CTLOCK: hlock statt SRWLOCK (Xbox-Threads, SUSPSAFE sieht den Owner) */
static unsigned g_nalloc, g_nreuse, g_nfree, g_nfull;
static unsigned long long g_bytes_reuse;

/* ---- W493: Rueckgabe-Modus, Statistik je Aufrufer ---- */
static int g_reclaim;                 /* loc_mem_config */
static u32 g_quar;                    /* Quarantaene in Bildern */
static uint32_t (*g_clock)(void);     /* Bildzaehler (hooks_frame_count) */
static volatile LONG g_full_logged;   /* Aufrufer-Statistik bei "voll" nur einmal */

#define LM_NCLS 96
static u32 g_cls[LM_NCLS];            /* Klassengroessen aufsteigend (16..256 in 16er-Schritten, dann 4 je Oktave) */
static int32_t g_fhead[LM_NCLS];
static int g_cls_ok;

typedef struct { int32_t idx; u32 frame; } lm_q_t;
static lm_q_t *g_q;                   /* Quarantaene-Ring */
static unsigned g_qcap, g_qhead, g_qn;

typedef struct {
    unsigned nalloc, nreuse, nfree, live_n;
    unsigned long long live_b, bytes;
} lm_tag_t;
static lm_tag_t g_tag[LOCMEM_T_N];
static unsigned g_q_bn;               /* Bloecke in Quarantaene */
static unsigned long long g_q_b;
static unsigned g_f_n;                /* wiederverwendbar (Freilisten) */
static unsigned long long g_f_b;
static unsigned g_nretract;           /* oberste freie Bloecke abgebaut */
static unsigned g_nforce;             /* Quarantaene vorzeitig geleert (Bereich sonst voll) */

#define LM_NSITE 48
typedef struct { u32 site; uint8_t tag; unsigned nalloc, live_n; unsigned long long live_b; } lm_site_t;
static lm_site_t g_site[LM_NSITE];    /* [0] = sonstige */
static unsigned g_nsite = 1;

/* Koepfe (VB/IB): 16 Byte = Common, Data, Lock (+ ungenutzt), in Bloecken zu 4096 */
#define LM_HCHUNK 4096u
#define LM_HMAXCH 512u
static u32 *g_hch[LM_HMAXCH];
static unsigned g_nhch;
static uint8_t *g_hst;                /* 0 frei, 1 lebend, 2 freigegeben (Quarantaene) */
static int32_t *g_hlink;              /* Freiliste der Koepfe */
static int32_t g_hfree = -1;
static unsigned g_h_live, g_h_rel, g_h_relint, g_h_odd, g_h_spec;

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
    memset(&g_blk[g_nblk], 0, sizeof g_blk[g_nblk]);
    g_blk[g_nblk].va = va;
    g_blk[g_nblk].size = size;
    g_blk[g_nblk].st = BS_USED;
    g_blk[g_nblk].prev = g_blk[g_nblk].next = -1;
    g_blk[g_nblk].hdr = -1;
    g_nblk++;
    return 1;
}

/* ---- W493: Groessenklassen ---- */
static void cls_init(void)
{
    unsigned c = 0, e, k;
    if (g_cls_ok) return;
    for (k = 1; k <= 16; k++) g_cls[c++] = k * 16u; /* 16..256 */
    for (e = 8; e < 28 && c < LM_NCLS; e++) {
        for (k = 1; k <= 4 && c < LM_NCLS; k++) g_cls[c++] = (1u << e) + k * (1u << (e - 2)); /* 320 .. 2^28 */
    }
    for (c = 0; c < LM_NCLS; c++) g_fhead[c] = -1;
    g_cls_ok = 1;
}
/* kleinste Klasse mit Groesse >= s (LM_NCLS = keine) */
static unsigned cls_ceil(u32 s)
{
    unsigned lo = 0, hi = LM_NCLS;
    while (lo < hi) {
        unsigned m = (lo + hi) / 2u;
        if (g_cls[m] < s) lo = m + 1u; else hi = m;
    }
    return lo;
}
/* groesste Klasse mit Groesse <= s (s >= 16) */
static unsigned cls_floor(u32 s)
{
    unsigned c = cls_ceil(s);
    if (c < LM_NCLS && g_cls[c] == s) return c;
    return c ? c - 1u : 0u;
}
static void fl_push(int32_t i)
{
    unsigned c = cls_floor(g_blk[i].size);
    g_blk[i].st = BS_FREE;
    g_blk[i].prev = -1;
    g_blk[i].next = g_fhead[c];
    if (g_fhead[c] >= 0) g_blk[g_fhead[c]].prev = i;
    g_fhead[c] = i;
    g_f_n++;
    g_f_b += g_blk[i].size;
}
static void fl_remove(int32_t i)
{
    unsigned c = cls_floor(g_blk[i].size);
    if (g_blk[i].prev >= 0) g_blk[g_blk[i].prev].next = g_blk[i].next;
    else g_fhead[c] = g_blk[i].next;
    if (g_blk[i].next >= 0) g_blk[g_blk[i].next].prev = g_blk[i].prev;
    g_blk[i].prev = g_blk[i].next = -1;
    g_f_n--;
    g_f_b -= g_blk[i].size;
}
/* Oberste freie Bloecke abbauen (Fuellstand sinkt). */
static void top_retract(void)
{
    while (g_nblk && g_blk[g_nblk - 1u].st == BS_FREE) {
        fl_remove((int32_t)(g_nblk - 1u));
        g_off = g_blk[g_nblk - 1u].va - g_lo;
        g_nblk--;
        g_nretract++;
    }
}
static void hdr_recycle(int32_t h);
/* Quarantaene: abgelaufene (bzw. mit force alle) Bloecke in die Freilisten. */
static void q_expire(int force)
{
    u32 now = g_clock ? g_clock() : 0u;
    int any = 0;
    while (g_qn) {
        lm_q_t *e = &g_q[g_qhead];
        loc_blk_t *b;
        if (!force && (u32)(now - e->frame) < g_quar) break;
        b = &g_blk[e->idx];
        g_qhead = (g_qhead + 1u) & (g_qcap - 1u);
        g_qn--;
        g_q_bn--;
        g_q_b -= b->size;
        if (b->hdr >= 0) {
            hdr_recycle(b->hdr);
            b->hdr = -1;
        }
        fl_push(e->idx);
        any = 1;
    }
    if (any) top_retract();
}
static int q_push(int32_t idx)
{
    if (g_qn == g_qcap) {
        unsigned nc = g_qcap ? g_qcap * 2u : 1024u, k;
        lm_q_t *n = (lm_q_t *)malloc(nc * sizeof *n);
        if (!n) return 0;
        for (k = 0; k < g_qn; k++) n[k] = g_q[(g_qhead + k) & (g_qcap - 1u)];
        free(g_q);
        g_q = n;
        g_qcap = nc;
        g_qhead = 0;
    }
    g_q[(g_qhead + g_qn) & (g_qcap - 1u)].idx = idx;
    g_q[(g_qhead + g_qn) & (g_qcap - 1u)].frame = g_clock ? g_clock() : 0u;
    g_qn++;
    return 1;
}
/* Freien Block suchen: Klasse ceil(size) (jeder passt), sonst floor(size) mit Groesse >= size. */
static int32_t fl_take(u32 size, u32 align)
{
    unsigned c = cls_ceil(size), pass;
    for (pass = 0; pass < 2; pass++) {
        int32_t i;
        unsigned n = 0;
        if (pass == 1) {
            unsigned f = cls_floor(size);
            if (f == c) break;
            c = f;
        }
        if (c >= LM_NCLS) continue;
        for (i = g_fhead[c]; i >= 0 && n < 64u; i = g_blk[i].next, n++) {
            if (g_blk[i].size >= size && !(g_blk[i].va & (align - 1u))) {
                fl_remove(i);
                return i;
            }
        }
    }
    return -1;
}
static unsigned site_find(u32 site, int tag)
{
    unsigned k;
    if (!site) return 0;
    for (k = 1; k < g_nsite; k++) {
        if (g_site[k].site == site && g_site[k].tag == (uint8_t)tag) return k;
    }
    if (g_nsite < LM_NSITE) {
        memset(&g_site[g_nsite], 0, sizeof g_site[g_nsite]);
        g_site[g_nsite].site = site;
        g_site[g_nsite].tag = (uint8_t)tag;
        return g_nsite++;
    }
    return 0;
}
static void st_used(int32_t i, int tag, u32 site, int reuse)
{
    loc_blk_t *b = &g_blk[i];
    unsigned s = site_find(site, tag);
    b->st = BS_USED;
    b->tag = (uint8_t)tag;
    b->site = (uint16_t)s;
    b->hdr = -1;
    g_tag[tag].nalloc += reuse ? 0u : 1u;
    g_tag[tag].nreuse += reuse ? 1u : 0u;
    g_tag[tag].bytes += b->size;
    g_tag[tag].live_n++;
    g_tag[tag].live_b += b->size;
    g_site[s].nalloc++;
    g_site[s].live_n++;
    g_site[s].live_b += b->size;
}
static void st_unused(int32_t i)
{
    loc_blk_t *b = &g_blk[i];
    g_tag[b->tag].nfree++;
    g_tag[b->tag].live_n--;
    g_tag[b->tag].live_b -= b->size;
    g_site[b->site].live_n--;
    g_site[b->site].live_b -= b->size;
}
/* Block i freigeben (Sperre gehalten): Rueckgabe-Modus -> Quarantaene, sonst alt (gleich wiederverwendbar). */
static void blk_release(int32_t i)
{
    st_unused(i);
    g_nfree++;
    if (!g_reclaim) {
        g_blk[i].st = BS_FREE;
        return;
    }
    g_blk[i].st = BS_QUAR;
    g_q_bn++;
    g_q_b += g_blk[i].size;
    if (!q_push(i)) { /* kein Speicher fuer den Ring: sofort frei */
        g_q_bn--;
        g_q_b -= g_blk[i].size;
        if (g_blk[i].hdr >= 0) {
            hdr_recycle(g_blk[i].hdr);
            g_blk[i].hdr = -1;
        }
        fl_push(i);
        top_retract();
        return;
    }
    if (!g_quar) q_expire(0);
}

static void full_report(u32 size);

u32 loc_mem_alloc_t(u32 size, u32 align, int zero, int tag, u32 site)
{
    u32 r = 0, i, base, want;
    int forced = 0;
    if (!g_lo) {
        return 0;
    }
    if (tag < 0 || tag >= LOCMEM_T_N) tag = LOCMEM_T_CONTIG;
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
    want = size;
    hlock_enter(&g_lock);
    cls_init();
    if (!g_reclaim) {
        /* Wiederverwendung: freier Block gleicher Groesse und passender Ausrichtung (neueste zuerst). */
        for (i = g_nblk; i-- > 0;) {
            if (g_blk[i].st == BS_FREE && g_blk[i].size == size && !(g_blk[i].va & (align - 1u))) {
                st_used((int32_t)i, tag, site, 1);
                r = g_blk[i].va;
                g_nreuse++;
                g_bytes_reuse += size;
                break;
            }
        }
    } else {
        int32_t k;
        if ((tag == LOCMEM_T_VB || tag == LOCMEM_T_IB) && cls_ceil(size) < LM_NCLS) {
            want = g_cls[cls_ceil(size)]; /* VB/IB auf die Klassengroesse: freie Bloecke passen wieder */
        }
        q_expire(0);
        for (;;) {
            k = fl_take(size, align);
            if (k >= 0) {
                st_used(k, tag, site, 1);
                r = g_blk[k].va;
                g_nreuse++;
                g_bytes_reuse += g_blk[k].size;
                break;
            }
            base = (g_lo + g_off + align - 1u) & ~(align - 1u);
            if (base + want <= g_lo + LOC_MEM_SIZE && base >= g_lo) break;
            if (forced || !g_qn) break;
            q_expire(1); /* Bereich sonst voll: Quarantaene vorzeitig leeren */
            g_nforce++;
            forced = 1;
        }
    }
    if (!r) {
        base = (g_lo + g_off + align - 1u) & ~(align - 1u);
        if (base + want > g_lo + LOC_MEM_SIZE || base < g_lo) {
            g_nfull++;
        } else if (blk_add(base, want)) {
            g_off = base + want - g_lo;
            r = base;
            g_nalloc++;
            st_used((int32_t)(g_nblk - 1u), tag, site, 0);
        }
    }
    hlock_leave(&g_lock);
    if (!r) {
        static volatile LONG nlog;
        if (InterlockedIncrement(&nlog) <= 8) {
            HLOG_E("LOCMEM: Bereich voll (size=0x%X, Fuellstand %u MB)", size, g_off >> 20);
        }
        full_report(size);
        return 0;
    }
    if (zero) {
        memset((void *)(uintptr_t)r, 0, size);
    }
    return r;
}

u32 loc_mem_alloc(u32 size, u32 align, int zero)
{
    return loc_mem_alloc_t(size, align, zero, LOCMEM_T_CONTIG, 0);
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
    if (k >= 0 && g_blk[k].st == BS_USED) {
        blk_release(k);
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
    { /* W493: je Aufrufer (Statistik, keine Inhalte) */
        loc_mem_info_t in;
        static const char *const k_nm[LOCMEM_T_N] = { "VB", "IB", "Contig", "ContigEx" };
        char b[640];
        size_t n = 0;
        int t;
        loc_mem_info(&in);
        for (t = 0; t < LOCMEM_T_N; t++) {
            n += (size_t)_snprintf_s(b + n, sizeof b - n, _TRUNCATE, "%s%s neu=%u wieder=%u frei=%u lebend=%u (%.1f MB)",
                                     t ? " | " : "", k_nm[t], in.t_nalloc[t], in.t_nreuse[t], in.t_nfree[t],
                                     in.t_live_n[t], (double)in.t_live_b[t] / 1048576.0);
            if (n >= sizeof b) break;
        }
        HLOG_I("LOCMEM-AUF: %s | Quarantaene %u (%.1f MB) | wiederverwendbar %u (%.1f MB) | Kopf lebend=%u frei=%u "
               "intern=%u | Rueckgabe=%d Quar=%u Bilder, vorzeitig=%u, abgebaut=%u",
               b, in.q_n, (double)in.q_b / 1048576.0, in.f_n, (double)in.f_b / 1048576.0, in.h_live, in.h_rel,
               in.h_relint, g_reclaim, g_quar, in.nforce, in.nretract);
    }
}

/* W493: bei "voll" einmal: Statistik + groesste lebende Aufrufer (Rueckgabeadresse im Spiel, nur Zaehler). */
static void full_report(u32 size)
{
    lm_site_t top[8];
    unsigned nt = 0, k, j;
    if (InterlockedExchange(&g_full_logged, 1) != 0) return;
    HLOG_E("LOCMEM: Bereich voll bei Anforderung 0x%X - Aufrufer-Statistik folgt (W493)", size);
    loc_mem_stats();
    hlock_enter(&g_lock);
    for (k = 0; k < g_nsite; k++) {
        lm_site_t s = g_site[k];
        if (!s.live_n) continue;
        for (j = nt; j > 0 && top[j - 1].live_b < s.live_b; j--) {
            if (j < 8) top[j] = top[j - 1];
        }
        if (j < 8) {
            top[j] = s;
            if (nt < 8) nt++;
        }
    }
    hlock_leave(&g_lock);
    for (k = 0; k < nt; k++) {
        static const char *const k_nm[LOCMEM_T_N] = { "VB", "IB", "Contig", "ContigEx" };
        HLOG_E("LOCMEM-STELLE: %s @0x%08X lebend=%u (%.1f MB), angelegt=%u", top[k].tag < LOCMEM_T_N ? k_nm[top[k].tag] : "?",
               top[k].site, top[k].live_n, (double)top[k].live_b / 1048576.0, top[k].nalloc);
    }
}

/* ---- W493: Koepfe fuer CreateVertex/IndexBuffer2 ---- */
static u32 *hdr_ptr(int32_t h) { return g_hch[(u32)h / LM_HCHUNK] + ((u32)h % LM_HCHUNK) * 4u; }
static int32_t hdr_index(u32 res)
{
    unsigned c;
    for (c = 0; c < g_nhch; c++) {
        u32 b = (u32)(uintptr_t)g_hch[c];
        if (res >= b && res < b + LM_HCHUNK * 16u) {
            if ((res - b) & 15u) return -1;
            return (int32_t)(c * LM_HCHUNK + (res - b) / 16u);
        }
    }
    return -1;
}
static void hdr_recycle(int32_t h)
{
    if (h < 0 || g_hst[h] != 2) return;
    g_hst[h] = 0; /* Inhalt bleibt bis zur naechsten Vergabe stehen (wie Xbox-Pool) */
    g_hlink[h] = g_hfree;
    g_hfree = h;
}
static int32_t hdr_get(void)
{
    int32_t h;
    if (g_hfree < 0) {
        unsigned c = g_nhch, k;
        u32 *p;
        uint8_t *nst;
        int32_t *nl;
        if (c >= LM_HMAXCH) return -1;
        p = (u32 *)VirtualAlloc(NULL, LM_HCHUNK * 16u, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!p) return -1;
        nst = (uint8_t *)realloc(g_hst, (c + 1u) * LM_HCHUNK);
        if (!nst) { VirtualFree(p, 0, MEM_RELEASE); return -1; }
        g_hst = nst;
        nl = (int32_t *)realloc(g_hlink, (c + 1u) * LM_HCHUNK * sizeof(int32_t));
        if (!nl) { VirtualFree(p, 0, MEM_RELEASE); return -1; }
        g_hlink = nl;
        g_hch[c] = p;
        g_nhch = c + 1u;
        for (k = LM_HCHUNK; k-- > 0;) {
            int32_t i = (int32_t)(c * LM_HCHUNK + k);
            g_hst[i] = 0;
            g_hlink[i] = g_hfree;
            g_hfree = i;
        }
    }
    h = g_hfree;
    g_hfree = g_hlink[h];
    return h;
}

u32 loc_mem_res2_create(u32 len, int ib, u32 site)
{
    u32 d, *p = NULL;
    int32_t h;
    if (!g_lo) return 0;
    d = loc_mem_alloc_t(len ? len : 4u, 16u, 1, ib ? LOCMEM_T_IB : LOCMEM_T_VB, site);
    if (!d) return 0;
    hlock_enter(&g_lock);
    h = hdr_get();
    if (h >= 0) {
        p = hdr_ptr(h);
        p[0] = ib ? 0x01010001u : 0x01000001u; /* wie XDK 0x188590/0x188550: Refcount 1, D3D-angelegt, Typ VB/IB */
        p[1] = d;                               /* Data = phys (untere Ansicht, phys == VA) */
        p[2] = 0u;                              /* Lock */
        p[3] = 0u;
        g_hst[h] = 1;
        g_h_live++;
    }
    hlock_leave(&g_lock);
    if (h < 0) {
        loc_mem_free(d);
        return 0;
    }
    return (u32)(uintptr_t)p;
}

int loc_mem_res2_release(u32 res, int spec, u32 *rc_out)
{
    int32_t h, k;
    u32 *p, c, rc;
    if (!g_lo || !g_nhch) return 0;
    hlock_enter(&g_lock);
    h = hdr_index(res);
    if (h < 0) {
        hlock_leave(&g_lock);
        return 0;
    }
    p = hdr_ptr(h);
    c = p[0];
    rc = c & 0xFFFFu;
    if (g_hst[h] != 1) { /* schon freigegeben (Spiel gibt doppelt frei): nichts mehr tun */
        g_h_odd++;
        *rc_out = 0;
        hlock_leave(&g_lock);
        return 1;
    }
    if (spec) { /* spekulativer Vorlauf: nur Rueckgabewert, Kopf und Speicher unberuehrt */
        g_h_spec++;
        *rc_out = rc ? rc - 1u : 0u;
        hlock_leave(&g_lock);
        return 1;
    }
    if (!rc) { /* Refcount schon 0 (unerwartet): nicht freigeben */
        g_h_odd++;
        *rc_out = 0;
        hlock_leave(&g_lock);
        return 1;
    }
    rc--;
    p[0] = (c & ~0xFFFFu) | rc;
    if (rc == 0) {
        if (c & 0x00780000u) { /* interner Refcount (XDK haelt sie noch): wie 0x1883D0 nicht zerstoeren */
            g_h_relint++;
        } else {
            g_hst[h] = 2;
            g_h_live--;
            g_h_rel++;
            k = blk_find(p[1]);
            if (k >= 0 && g_blk[k].st == BS_USED && g_blk[k].va == (p[1] & LOC_MEM_PHYS)) {
                g_blk[k].hdr = h;
                blk_release(k);
                if (g_blk[k].st == BS_FREE || !g_reclaim) hdr_recycle(h); /* ohne Quarantaene gleich */
            } else {
                hdr_recycle(h);
            }
        }
    }
    *rc_out = rc;
    hlock_leave(&g_lock);
    return 1;
}

int loc_mem_res2_owns(u32 res)
{
    int r;
    if (!g_nhch) return 0;
    hlock_enter(&g_lock);
    r = hdr_index(res) >= 0;
    hlock_leave(&g_lock);
    return r;
}

void loc_mem_config(int reclaim, uint32_t quar_frames, uint32_t (*clock)(void))
{
    hlock_enter(&g_lock);
    g_reclaim = reclaim ? 1 : 0;
    g_quar = quar_frames;
    g_clock = clock;
    hlock_leave(&g_lock);
    if (g_lo) {
        HLOG_I("LOCMEM: Rueckgabe %s (VB/IB-Release, Quarantaene %u Bilder, Groessenklassen) - loc_mem_reclaim=%d (W493)",
               reclaim ? "an" : "aus (alt)", quar_frames, reclaim ? 1 : 0);
    }
}

void loc_mem_tick(void)
{
    if (!g_lo || !g_reclaim || !g_qn) return;
    hlock_enter(&g_lock);
    q_expire(0);
    hlock_leave(&g_lock);
}

void loc_mem_info(loc_mem_info_t *o)
{
    int t;
    memset(o, 0, sizeof *o);
    hlock_enter(&g_lock);
    for (t = 0; t < LOCMEM_T_N; t++) {
        o->t_nalloc[t] = g_tag[t].nalloc;
        o->t_nreuse[t] = g_tag[t].nreuse;
        o->t_nfree[t] = g_tag[t].nfree;
        o->t_live_n[t] = g_tag[t].live_n;
        o->t_live_b[t] = g_tag[t].live_b;
    }
    o->q_n = g_q_bn;
    o->q_b = g_q_b;
    o->f_n = g_f_n;
    o->f_b = g_f_b;
    o->used = g_off;
    o->size = LOC_MEM_SIZE;
    o->nblk = g_nblk;
    o->nalloc = g_nalloc;
    o->nreuse = g_nreuse;
    o->nfree = g_nfree;
    o->nfull = g_nfull;
    o->nforce = g_nforce;
    o->nretract = g_nretract;
    o->h_live = g_h_live;
    o->h_rel = g_h_rel;
    o->h_relint = g_h_relint;
    o->h_odd = g_h_odd;
    o->h_spec = g_h_spec;
    hlock_leave(&g_lock);
}

/* Selbsttest: Bereich ohne Spiegel auf einen eigenen Puffer (< 256 MB) legen; base = 0 setzt alles zurueck. */
int loc_mem_init_test(u32 base, u32 size)
{
    unsigned c;
    hlock_enter(&g_lock);
    for (c = 0; c < g_nhch; c++) VirtualFree(g_hch[c], 0, MEM_RELEASE);
    g_nhch = 0;
    free(g_hst); g_hst = NULL;
    free(g_hlink); g_hlink = NULL;
    g_hfree = -1;
    free(g_blk); g_blk = NULL;
    g_nblk = g_capblk = 0;
    free(g_q); g_q = NULL;
    g_qcap = g_qhead = g_qn = 0;
    memset(g_tag, 0, sizeof g_tag);
    memset(g_site, 0, sizeof g_site);
    g_nsite = 1;
    g_q_bn = g_f_n = 0;
    g_q_b = g_f_b = 0;
    g_nalloc = g_nreuse = g_nfree = g_nfull = g_nforce = g_nretract = 0;
    g_h_live = g_h_rel = g_h_relint = g_h_odd = g_h_spec = 0;
    g_bytes_reuse = 0;
    g_full_logged = 0;
    g_cls_ok = 0;
    cls_init();
    g_off = 0;
    g_lo = base & LOC_MEM_PHYS;
    LOC_MEM_SIZE = base ? size : 0u;
    hlock_leave(&g_lock);
    return base != 0;
}

/* W330 LOC-ROLLBACK-R1: untere Ansicht + Groesse + Fuellstand (Spiegel: lo|0x80000000, lo|0xF0000000) */
int loc_mem_range(uint32_t *lo, uint32_t *size, uint32_t *used)
{
    if (!g_lo) return 0;
    *lo = g_lo;
    *size = LOC_MEM_SIZE;
    *used = g_off;
    return 1;
}
