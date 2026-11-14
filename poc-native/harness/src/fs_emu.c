/* fs_emu.c - Xbox-FS-Emulation, siehe fs_emu.h.
 * Faktenbasis: work/fs_report.txt (83 FS-Refs) + work/fs_context.txt.
 * Offsets (Xbox-KPCR, vgl. Cxbx EmuFS.cpp):
 *   FS:[0x20] Prcb              -> MOV EAX,imm32 (Fake-KPRCB-Adresse)
 *                                 (alle 7 Stellen pruefen [Prcb+0x250] auf
 *                                 NULL und ueberspringen -> keine Notifies)
 *   FS:[0x24] Irql (Byte)       -> immer 0 (PASSIVE; sonst BugCheck-Pfad!)
 *   FS:[0x28] CurrentThread     -> pro Thread: Fake-KTHREAD
 *   FS:[0x4]  TLS-Slotbasis     -> pro Thread: TLS-Puffer + 4
 *                                 (Spiel kopiert selbst Template dorthin)
 *   FS:[0x58] DpcRoutineActive  -> immer 0 (keine DPCs)
 */
#include "fs_emu.h"
#include "hlog.h"
#include "sb_profile.h" /* W229 SBA() */
#include <stdlib.h>
#include <string.h>
#include <windows.h>

typedef uint32_t u32;
typedef uint8_t u8;

/* ---------- Fake Kernel-Structs ---------- */

/* KPRCB-Fake: alle Stellen lesen [Prcb+0x250] und pruefen NULL. */
static u32 g_fake_prcb[0xC0]; /* 0x300 Bytes, genullt */

/* Pro-Thread-Xbox-State. KTHREAD-Layout (Cxbx xboxkrnl.h): +0x28 = TlsData. */
#define FS_KTHREAD_SIZE 0x40u
#define FS_TLS_OFF 0x40u /* Offset von tlsbuf_ptr in xbox_thread_t */
typedef struct {
    u32 kthread[FS_KTHREAD_SIZE / 4]; /* [10] = TlsData */
    u32 tlsbuf_ptr;                   /* Zeiger auf TLS-Puffer (mit Guard) */
} xbox_thread_t;

#define FS_TLS_DATA 8192u /* Datenteil des TLS-Puffers */
#define FS_TLS_GUARD 4096u
/* Headroom UNTER dem Puffer: Xbox nutzt negative TLS-Indizes (dieses XBE:
 * Entry errechnet -(Slots+1) = -5 aus Template-Groesse, d.h. Slot[-5] liegt
 * 20 B unter tlsbase = 16 B unter TlsData). Eine ganze Seite ist einfach
 * und haelt tlsbuf_ptr seiten-aligned. */
#define FS_TLS_HEADROOM 4096u
/* XAPI Per-Thread-Block (slot[0]): Get/SetLastError nutzen [slot0+4]. */
#define FS_XAPI_BLOCK 64u

static DWORD g_fs_wintls = TLS_OUT_OF_INDEXES; /* Windows-TLS-Index */

/* Fallback (falls je ein Host-Thread Spiel-Code mit FS betaete): laut, aber
 * nicht crashend. Normale Xbox-Threads haben immer echten State. Headroom
 * fuer negative TLS-Indizes liegt direkt davor (eine Einheit, Adjazenz
 * garantiert). */
static xbox_thread_t g_fb;
static struct {
    u32 head[FS_TLS_HEADROOM / 4];
    u32 tls[FS_TLS_DATA / 4];
} g_fb_tls;
#define FB_TLS_BASE ((u32)(uintptr_t)g_fb_tls.tls)

/* ---------- Trampolin-Pool ---------- */

static u8 *g_pool; /* 1 Seite, RWX */
#define POOL_OFF_TGV 0x00u /* Zelle: TlsGetValue-Adresse */
#define POOL_OFF_C28 0x04u /* Zelle: tramp28-Adresse */
#define POOL_OFF_C4C 0x08u /* Zelle: tramp4ecx-Adresse */
#define POOL_OFF_C4D 0x0Cu /* Zelle: tramp4edi-Adresse */
#define POOL_OFF_KFB 0x10u /* Zelle: &g_fb (KTHREAD-Fallback) */
#define POOL_OFF_TFB 0x14u /* Zelle: g_fb_tls + 4 (TLS-Fallback) */
#define POOL_OFF_T28 0x20u /* tramp28-Code (27 B) */
#define POOL_OFF_T4C 0x60u /* tramp4ecx-Code (39 B) */
#define POOL_OFF_T4D 0xA0u /* tramp4edi-Code (39 B) */
/* ACHTUNG: Slots duerfen sich nicht ueberlappen (je >= max. 39 B)! */

static void pool_write_u32(u32 off, u32 v)
{
    memcpy(g_pool + off, &v, 4);
}

/* Baut die drei Trampoline. Alle erhalten Flags (PUSHFD/POPFD) und alle
 * Register ausser dem Zielregister. */
static void pool_build_tramps(void)
{
    u8 *t;
    u32 cell_tgv = (u32)(uintptr_t)(g_pool + POOL_OFF_TGV);
    u32 widx = (u32)g_fs_wintls;

    /* tramp28 -> EAX = KTHREAD* (Struct-Offset 0) */
    t = g_pool + POOL_OFF_T28;
    *t++ = 0x9C;                                  /* PUSHFD */
    *t++ = 0x51;                                  /* PUSH ECX */
    *t++ = 0x52;                                  /* PUSH EDX */
    *t++ = 0x68; memcpy(t, &widx, 4); t += 4;     /* PUSH widx */
    *t++ = 0xFF; *t++ = 0x15;                     /* CALL [cell_tgv] */
    memcpy(t, &cell_tgv, 4); t += 4;
    *t++ = 0x85; *t++ = 0xC0;                     /* TEST EAX,EAX */
    *t++ = 0x75; *t++ = 0x05;                     /* JNZ +5 */
    {
        u32 cell_kfb = (u32)(uintptr_t)(g_pool + POOL_OFF_KFB);
        *t++ = 0xA1; memcpy(t, &cell_kfb, 4); t += 4; /* MOV EAX,[kfb] */
    }
    *t++ = 0x5A;                                  /* POP EDX */
    *t++ = 0x59;                                  /* POP ECX */
    *t++ = 0x9D;                                  /* POPFD */
    *t++ = 0xC3;                                  /* RET */

    /* tramp4ecx/tramp4edi -> ECX/EDI = [struct+0x40] + 4 */
    {
        static const u32 offs[2] = { POOL_OFF_T4C, POOL_OFF_T4D };
        static const u8 movr[2] = { 0xC1, 0xC7 }; /* 89 C1 / 89 C7 */
        int k;
        for (k = 0; k < 2; k++) {
            u32 cell_tfb = (u32)(uintptr_t)(g_pool + POOL_OFF_TFB);
            u32 four = 4;
            t = g_pool + offs[k];
            *t++ = 0x9C;
            *t++ = 0x50;                          /* PUSH EAX */
            *t++ = 0x52;                          /* PUSH EDX */
            *t++ = 0x68; memcpy(t, &widx, 4); t += 4;
            *t++ = 0xFF; *t++ = 0x15;
            memcpy(t, &cell_tgv, 4); t += 4;
            *t++ = 0x85; *t++ = 0xC0;
            *t++ = 0x75; *t++ = 0x07;             /* JNZ +7 */
            *t++ = 0xA1; memcpy(t, &cell_tfb, 4); t += 4;
            *t++ = 0xEB; *t++ = 0x08;             /* JMP +8 */
            *t++ = 0x8B; *t++ = 0x40; *t++ = 0x40; /* MOV EAX,[EAX+0x40] */
            *t++ = 0x05; memcpy(t, &four, 4); t += 4; /* ADD EAX,4 */
            *t++ = 0x89; *t++ = movr[k];
            *t++ = 0x5A;
            *t++ = 0x58;                          /* POP EAX */
            *t++ = 0x9D;
            *t++ = 0xC3;
        }
    }
    FlushInstructionCache(GetCurrentProcess(), g_pool, 4096);
}

/* ---------- Patch-Tabelle ---------- */

/* kind: 0=MOV EAX,prcb 1=XOR EAX,EAX 2=CALL[cell28] 3=CALL[cell4c] 4=CALL[cell4d] */
typedef struct {
    u32 va;
    u8 expect[8];
    u8 elen;
    u8 kind;
} fs_site_t;

#define FS_MOV_EAX_20 { 0x64, 0xA1, 0x20, 0x00, 0x00, 0x00 }
#define FS_MOVZX_24 \
    { 0x64, 0x0F, 0xB6, 0x05, 0x24, 0x00, 0x00, 0x00 }
#define FS_MOV_EAX_28 { 0x64, 0xA1, 0x28, 0x00, 0x00, 0x00 }
#define FS_MOV_ECX_04 { 0x64, 0x8B, 0x0D, 0x04, 0x00, 0x00, 0x00 }
#define FS_MOV_EDI_04 { 0x64, 0x8B, 0x3D, 0x04, 0x00, 0x00, 0x00 }
#define FS_MOV_EAX_58 { 0x64, 0xA1, 0x58, 0x00, 0x00, 0x00 }

static const fs_site_t g_sites[] = {
    /* FS:[0x20] -> EAX (Prcb) */
    { 0x190244u, FS_MOV_EAX_20, 6, 0 },
    { 0x1902BBu, FS_MOV_EAX_20, 6, 0 },
    { 0x190B52u, FS_MOV_EAX_20, 6, 0 },
    { 0x190B87u, FS_MOV_EAX_20, 6, 0 },
    { 0x192339u, FS_MOV_EAX_20, 6, 0 },
    { 0x1924ADu, FS_MOV_EAX_20, 6, 0 },
    { 0x215D7Fu, FS_MOV_EAX_20, 6, 0 },
    /* FS:[0x24] (Irql-Byte) */
    { 0x18CE5Fu, FS_MOVZX_24, 8, 1 },
    { 0x18CE87u, FS_MOVZX_24, 8, 1 },
    { 0x190EA5u, FS_MOVZX_24, 8, 1 },
    { 0x205032u, FS_MOVZX_24, 8, 1 },
    { 0x2050C0u, FS_MOVZX_24, 8, 1 },
    { 0x21E083u, FS_MOVZX_24, 8, 1 },
    { 0x223763u, FS_MOVZX_24, 8, 1 },
    { 0x22766Bu, FS_MOVZX_24, 8, 1 },
    /* FS:[0x28] -> EAX (CurrentThread) */
    { 0x18CE6Bu, FS_MOV_EAX_28, 6, 2 },
    { 0x18CE93u, FS_MOV_EAX_28, 6, 2 },
    { 0x190DD8u, FS_MOV_EAX_28, 6, 2 },
    { 0x190EB1u, FS_MOV_EAX_28, 6, 2 },
    { 0x190ECEu, FS_MOV_EAX_28, 6, 2 },
    { 0x1924C9u, FS_MOV_EAX_28, 6, 2 },
    { 0x205048u, FS_MOV_EAX_28, 6, 2 },
    { 0x2050D4u, FS_MOV_EAX_28, 6, 2 },
    /* FS:[0x4] -> ECX/EDI (TLS-Slotbasis) */
    { 0x18CE76u, FS_MOV_ECX_04, 7, 3 },
    { 0x18CE99u, FS_MOV_ECX_04, 7, 3 },
    { 0x205059u, FS_MOV_ECX_04, 7, 3 },
    { 0x2050E5u, FS_MOV_ECX_04, 7, 3 },
    { 0x2051F1u, FS_MOV_ECX_04, 7, 3 },
    { 0x1924CFu, FS_MOV_EDI_04, 7, 4 },
    /* FS:[0x58] -> EAX (DpcRoutineActive) */
    { 0x21DFF0u, FS_MOV_EAX_58, 6, 1 },
    { 0x21E015u, FS_MOV_EAX_58, 6, 1 },
};

/* ---------- API ---------- */

void fs_emu_init(void)
{
    HMODULE krnl;
    void *tgv;
    if (g_fs_wintls != TLS_OUT_OF_INDEXES) {
        return;
    }
    memset(g_fake_prcb, 0, sizeof g_fake_prcb);
    memset(&g_fb, 0, sizeof g_fb);
    memset(&g_fb_tls, 0, sizeof g_fb_tls);
    g_fb.tlsbuf_ptr = FB_TLS_BASE;
    g_fb.kthread[0x28 / 4] = FB_TLS_BASE;
    g_fb_tls.tls[1] = (u32)(uintptr_t)calloc(1, FS_XAPI_BLOCK); /* slot[0] */
    /* Negativ-Slot des echten Spiels (-5): Fallback bleibt lesbar. */
    g_fb_tls.head[(FS_TLS_HEADROOM / 4) - 4] = g_fb_tls.tls[1];

    g_fs_wintls = TlsAlloc();
    if (g_fs_wintls == TLS_OUT_OF_INDEXES) {
        HLOG_E("FS: TlsAlloc fehlgeschlagen (GLE=%lu)", GetLastError());
        ExitProcess(7);
    }
    krnl = GetModuleHandleA("kernel32.dll");
    tgv = krnl ? GetProcAddress(krnl, "TlsGetValue") : NULL;
    if (!tgv) {
        HLOG_E("FS: TlsGetValue nicht gefunden");
        ExitProcess(7);
    }
    g_pool = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE,
                          PAGE_EXECUTE_READWRITE);
    if (!g_pool) {
        HLOG_E("FS: Trampolin-Pool fehlgeschlagen (GLE=%lu)", GetLastError());
        ExitProcess(7);
    }
    pool_write_u32(POOL_OFF_TGV, (u32)(uintptr_t)tgv);
    pool_write_u32(POOL_OFF_C28,
                   (u32)(uintptr_t)(g_pool + POOL_OFF_T28));
    pool_write_u32(POOL_OFF_C4C,
                   (u32)(uintptr_t)(g_pool + POOL_OFF_T4C));
    pool_write_u32(POOL_OFF_C4D,
                   (u32)(uintptr_t)(g_pool + POOL_OFF_T4D));
    pool_write_u32(POOL_OFF_KFB, (u32)(uintptr_t)&g_fb);
    pool_write_u32(POOL_OFF_TFB, FB_TLS_BASE + 4);
    pool_build_tramps();
    HLOG_I("FS: init (wintls=%lu pool=0x%p)", g_fs_wintls, g_pool);
}

void fs_emu_alloc_thread(void)
{
    xbox_thread_t *xt;
    u8 *buf;
    DWORD old;
    if (g_fs_wintls == TLS_OUT_OF_INDEXES) {
        return;
    }
    if (TlsGetValue(g_fs_wintls)) {
        return; /* schon vorhanden */
    }
    xt = calloc(1, sizeof *xt);
    buf = VirtualAlloc(NULL, FS_TLS_HEADROOM + FS_TLS_DATA + FS_TLS_GUARD,
                       MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!xt || !buf) {
        HLOG_E("FS: Thread-State geht nicht (GLE=%lu)", GetLastError());
        ExitProcess(7);
    }
    /* Guard-Seite am Ende: TLS-Ueberlauf wird laut (VEH) statt still. */
    VirtualProtect(buf + FS_TLS_HEADROOM + FS_TLS_DATA, FS_TLS_GUARD,
                   PAGE_NOACCESS, &old);
    buf += FS_TLS_HEADROOM; /* TlsData: Headroom fuer negative Indizes */
    xt->tlsbuf_ptr = (u32)(uintptr_t)buf;
    xt->kthread[0x28 / 4] = (u32)(uintptr_t)buf; /* TlsData */
    TlsSetValue(g_fs_wintls, xt);
}

u32 *fs_emu_tls_slots(void)
{
    xbox_thread_t *xt;
    if (g_fs_wintls == TLS_OUT_OF_INDEXES) {
        return NULL;
    }
    xt = TlsGetValue(g_fs_wintls);
    if (!xt || !xt->tlsbuf_ptr) {
        return NULL;
    }
    /* +4: Slotbasis wie FS:[0x4]-Emulation (Kopfkommentar + Prefill).
     * Ohne +4 landet der Aufrufer 4 Bytes neben dem Prefill-Block
     * (eigener Block, Spiel liest stale 0 -> E_FAIL-Spin). */
    return (u32 *)(uintptr_t)(xt->tlsbuf_ptr + 4);
}

/* TLS-Index-VA aus dem XBE-TLS-Verzeichnis (Loader setzt sie, Entry
 * errechnet daraus den Slot-Index, z.B. -5 fuer dieses XBE). */
static u32 g_tls_index_va;

void fs_emu_set_tls_index_va(u32 va)
{
    g_tls_index_va = va;
}

/* XAPI-Block (LastError etc.) in den Slot legen, den das Spiel wirklich
 * liest: [tls_index_va] ist signed und oft negativ (Entry-Formel
 * -((align16(size)+4)/4)). Muss NACH XapiThreadStartup laufen (der nullt
 * die Slots), VOR der Spiel-Routine. Nur fuellen, wenn 0 (XAPI-eigener
 * Block spaeter bleibt erhalten). */
void fs_emu_prefill_xapi(void)
{
    xbox_thread_t *xt;
    u32 *slots;
    int idx = 0;
    if (g_fs_wintls == TLS_OUT_OF_INDEXES) {
        return;
    }
    xt = TlsGetValue(g_fs_wintls);
    if (!xt) {
        return;
    }
    if (g_tls_index_va) {
        idx = *(int *)(uintptr_t)g_tls_index_va;
    }
    slots = (u32 *)(uintptr_t)(xt->tlsbuf_ptr + 4);
    HLOG_I("FS prefill: tlsbuf=0x%X idx=%d slot_vorher=0x%X", xt->tlsbuf_ptr,
           idx, slots[idx]);
    if (slots[idx] == 0) {
        slots[idx] = (u32)(uintptr_t)calloc(1, FS_XAPI_BLOCK);
        if (!slots[idx]) {
            HLOG_E("FS: XAPI-Block geht nicht");
            ExitProcess(7);
        }
        HLOG_I("FS prefill: slot[%d]=0x%X gesetzt", idx, slots[idx]);
    }
    /* Slot[0] zusaetzlich fuellen (Harmlosigkeit, falls je Index 0 gilt). */
    if (idx != 0 && slots[0] == 0) {
        slots[0] = slots[idx];
    }
}

void fs_emu_patch_all(void)
{
    size_t i, n;
    if (!g_pool) {
        HLOG_E("FS: patch ohne init!");
        ExitProcess(7);
    }
    n = sizeof g_sites / sizeof g_sites[0];
    for (i = 0; i < n; i++) {
        const fs_site_t *s = &g_sites[i];
        u8 *p = (u8 *)(uintptr_t)SBA(s->va);
        u32 cell = 0;
        HLOG_T("FS: patch @0x%X kind=%u", s->va, s->kind);
        if (memcmp(p, s->expect, s->elen) != 0) {
            HLOG_E("FS: Byte-Mismatch @0x%X (erwartet %02X %02X %02X %02X, "
                   "gefunden %02X %02X %02X %02X) - ABBRUCH",
                   s->va, s->expect[0], s->expect[1], s->expect[2],
                   s->expect[3], p[0], p[1], p[2], p[3]);
            hlog_close();
            ExitProcess(7);
        }
        switch (s->kind) {
        case 0: { /* MOV EAX,prcb-imm32 + NOP (Adresse, nicht Inhalt!) */
            u32 prcb = (u32)(uintptr_t)g_fake_prcb;
            p[0] = 0xB8;
            memcpy(p + 1, &prcb, 4);
            p[5] = 0x90;
            break;
        }
        case 1: /* XOR EAX,EAX + NOPs */
            p[0] = 0x31;
            p[1] = 0xC0;
            memset(p + 2, 0x90, s->elen - 2);
            break;
        case 2:
            cell = (u32)(uintptr_t)(g_pool + POOL_OFF_C28);
            goto callcell;
        case 3:
            cell = (u32)(uintptr_t)(g_pool + POOL_OFF_C4C);
            goto callcell;
        case 4:
            cell = (u32)(uintptr_t)(g_pool + POOL_OFF_C4D);
        callcell: { /* CALL [cell] + NOPs */
            p[0] = 0xFF;
            p[1] = 0x15;
            memcpy(p + 2, &cell, 4);
            memset(p + 6, 0x90, s->elen - 6);
            break;
        }
        default:
            HLOG_E("FS: unbekannte kind %u", s->kind);
            hlog_close();
            ExitProcess(7);
        }
    }
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
    HLOG_I("FS: %u Stellen gepatcht (SEH bleibt nativ)", (unsigned)n);
}
