/* sctrace.c - Syscall-Tracer, siehe sctrace.h.
 * Pro Ordinal ein 32-Byte-Slot: PUSHAD/PUSHFD, ordinal+retaddr loggen
 * (cdecl-Logger, Caller-Cleanup), POPFD/POPAD, JMP impl. Stapel-, Register-
 * und Flag-neutral; Aritaet-transparent (JMP statt CALL).
 */
#include "sctrace.h"
#include "hlog.h"
#include "hcfg.h"
#include "kernel_hle.h" /* krnl_name */
#include <string.h>
#include <intrin.h>
#include <windows.h>

#define SC_MAX 512u
#define SC_SLOT 32u

static uint8_t *g_scpool; /* Code + Impl-Zellen */
static uint32_t g_sclogfn; /* Adresse von sctrace_log */

static void __cdecl sctrace_log(uint32_t ordinal, uint32_t retaddr)
{
    /* Spiel-ESP = Logger-ESP + ret(4) + args(8) + EFLAGS(4) + PUSHAD(32). */
    void *ra = _AddressOfReturnAddress();
    HLOG_T("SC %3u %-28s <- 0x%08X esp=0x%08X", ordinal,
           krnl_name(ordinal), retaddr,
           (uint32_t)(uintptr_t)ra + 0x30u);
}

int sctrace_active(void)
{
    return g_cfg.syscall_trace != 0;
}

void sctrace_init(void)
{
    if (g_scpool) {
        return;
    }
    g_scpool = VirtualAlloc(NULL, SC_MAX * SC_SLOT, MEM_COMMIT | MEM_RESERVE,
                            PAGE_EXECUTE_READWRITE);
    if (!g_scpool) {
        HLOG_E("SCTRACE: Pool fehlgeschlagen (GLE=%lu)", GetLastError());
        ExitProcess(7);
    }
    g_sclogfn = (uint32_t)(uintptr_t)&sctrace_log;
    memset(g_scpool, 0xCC, SC_MAX * SC_SLOT); /* INT3-Padding */
    HLOG_I("SCTRACE: Pool bereit (%u Slots)", SC_MAX);
}

void *sctrace_wrap(unsigned ordinal, void *impl)
{
    /* Slot-Layout: [code 28][implcell 4] */
    uint8_t *t;
    uint32_t cell_log, cell_impl;
    if (ordinal >= SC_MAX || !impl) {
        return impl;
    }
    if (!g_scpool) {
        sctrace_init();
    }
    t = g_scpool + ordinal * SC_SLOT;
    /* Zellen: logfn global (eigene Seite unten), impl pro Slot */
    cell_impl = (uint32_t)(uintptr_t)(t + 28);
    memcpy(t + 28, &impl, 4);
    /* Eigene Zelle fuer logfn-Adresse hinter dem Pool-Slot? Wir brauchen eine
     * absolute Speicherstelle: Pool-Ende + 0. */
    {
        static uint32_t logcell = 0;
        if (!logcell) {
            logcell = g_sclogfn;
        }
        cell_log = (uint32_t)(uintptr_t)&logcell;
        *t++ = 0x60;                            /* PUSHAD */
        *t++ = 0x9C;                            /* PUSHFD */
        /* Stack: [ESP]=EFLAGS [ESP+4..36]=PUSHAD [ESP+36]=ret */
        *t++ = 0xFF; *t++ = 0x74; *t++ = 0x24; *t++ = 0x24; /* PUSH [ESP+36] */
        *t++ = 0x68;                            /* PUSH ordinal */
        memcpy(t, &ordinal, 4);
        t += 4;
        *t++ = 0xFF; *t++ = 0x15;               /* CALL [logcell] */
        memcpy(t, &cell_log, 4);
        t += 4;
        *t++ = 0x83; *t++ = 0xC4; *t++ = 0x08;   /* ADD ESP,8 */
        *t++ = 0x9D;                            /* POPFD */
        *t++ = 0x61;                            /* POPAD */
        *t++ = 0xFF; *t++ = 0x25;               /* JMP [implcell] */
        memcpy(t, &cell_impl, 4);
        t += 4;
    }
    FlushInstructionCache(GetCurrentProcess(), g_scpool + ordinal * SC_SLOT,
                          SC_SLOT);
    return g_scpool + ordinal * SC_SLOT;
}

