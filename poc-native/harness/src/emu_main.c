/* emu_main.c - sb_emu.dll: Harness-Einstieg (wird vom Loader gerufen).
 * Export: void __stdcall sb_emulate(void). Liest eigene Commandline.
 * Log: <SB_OUT_DIR|out>/sb_harness.log (s. hcfg_out_dir).
 */
#include <stdio.h>
#include "sb_profile.h" /* W229 SBA() */
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <windows.h>
#include <dbghelp.h>
#include <intrin.h> /* __readfsdword (SEH-Kette in DIAG NULLCALL) */
#include "hlog.h"
#include "sb_lang.h" /* W142 L10N */
#include "sb_remaster.h" /* W103 REM-FRAME */
#include "hcfg.h"
#include "xbox_types.h"
#include "xbe_loader.h"
#include "kernel_hle.h"
#include "xdk_hooks.h"
#include "fs_emu.h"
#include "d3d11_render.h"
#include "watch_dr.h" /* W41 WATCHDR */
#include "sb_gamever.h" /* W150 ISO-DIRECT */
#include "sb_profile.h" /* W229 US-PROFILE */
#include "sb_feedback.h" /* W188 GAME-MENU */
#include "sb_userdata.h" /* W150 USER-DATA */
#include "sb_relaunch.h" /* W186 RESET-TITLE */

static int g_run_seconds;
static volatile LONG g_wd_waited; /* W186: Sekunden seit Watchdog-Start (Rest-Laufzeit) */

/* W94 CRASHLOG: Absturz-Spuren dauerhaft machen. Die WER-Dumps vom 3.11.
 * zeigten: bei ungueltiger Spiel-SEH-Kette (c00001a5) beendet Windows den
 * Prozess, ohne dass last_chance laeuft, und der naechste Start
 * ueberschreibt sb_harness.log. Daher bei fataler Ausnahme sofort eine
 * kurze crash_<datum>.txt neben das Log (RIP, Modul+Offset, Register,
 * Stack, cfg-Kernwerte, letzte Logzeilen). Max. 3 Dateien je Prozess. */
static char g_log_path[600];
static volatile LONG g_crash_n, g_crash_busy;
static char g_crash_dir[600]; /* W150 USER-DATA: leer = hcfg_out_dir() (bisher) */
static char g_crash_buf[24576];

static int crash_add(int n, const char *fmt, ...)
{
    va_list ap;
    int m;
    if (n < 0 || n >= (int)sizeof g_crash_buf - 2) {
        return n;
    }
    va_start(ap, fmt);
    m = _vsnprintf_s(g_crash_buf + n, sizeof g_crash_buf - (size_t)n, _TRUNCATE,
                     fmt, ap);
    va_end(ap);
    return m < 0 ? (int)strlen(g_crash_buf) : n + m;
}

static void crash_report(EXCEPTION_POINTERS *ep, const char *why)
{
    hlog_flush(); /* W168 hlog_async: gepufferte Zeilen vor dem Absturzbericht schreiben */
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    CONTEXT *cx = ep->ContextRecord;
    uintptr_t rip = (uintptr_t)er->ExceptionAddress;
    char path[700], mod[MAX_PATH] = "?";
    const char *mb = mod;
    uintptr_t mbase = 0;
    HMODULE hm = NULL;
    SYSTEMTIME st;
    HANDLE f;
    DWORD w, i;
    LONG k;
    int n = 0;

    if (InterlockedCompareExchange(&g_crash_busy, 1, 0) != 0) {
        return; /* anderer Thread schreibt gerade */
    }
    k = InterlockedIncrement(&g_crash_n);
    if (k > 3) {
        InterlockedExchange(&g_crash_busy, 0);
        return;
    }
    GetLocalTime(&st);
    _snprintf_s(path, sizeof path, _TRUNCATE,
                "%s/crash_%04u%02u%02u_%02u%02u%02u_%ld.txt", g_crash_dir[0] ? g_crash_dir : hcfg_out_dir(),
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, k);
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)rip, &hm) && hm) {
        mbase = (uintptr_t)hm;
        if (GetModuleFileNameA(hm, mod, sizeof mod)) {
            const char *s = strrchr(mod, 0x5C);
            mb = s ? s + 1 : mod;
        }
    }
    n = crash_add(n, "SB-CRASH %04u-%02u-%02u %02u:%02u:%02u.%03u  (%s)\r\n",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                  st.wMilliseconds, why);
    n = crash_add(n, "Code=0x%08lX Flags=0x%lX RIP=0x%08X  Modul=%s+0x%X%s  Thread=T%04x\r\n",
                  er->ExceptionCode, er->ExceptionFlags, (unsigned)rip, mb,
                  (unsigned)(rip - mbase),
                  (rip >= 0x10000u && rip < 0x2810000u) ? " (Xbox-VA, symbolize.py)" : "",
                  GetCurrentThreadId());
    if (er->NumberParameters >= 2) {
        n = crash_add(n, "Param: [0]=0x%X [1]=0x%X (AV: 0=lesen 1=schreiben 8=DEP, Adresse)\r\n",
                      (unsigned)er->ExceptionInformation[0],
                      (unsigned)er->ExceptionInformation[1]);
    }
    n = crash_add(n, "EAX=%08X EBX=%08X ECX=%08X EDX=%08X ESI=%08X EDI=%08X EBP=%08X ESP=%08X EIP=%08X EFL=%08X\r\n",
                  cx->Eax, cx->Ebx, cx->Ecx, cx->Edx, cx->Esi, cx->Edi, cx->Ebp,
                  cx->Esp, cx->Eip, cx->EFlags);
    n = crash_add(n, "Stack:");
    for (i = 0; i < 24; i++) {
        unsigned v = 0;
        __try {
            v = ((unsigned *)(uintptr_t)cx->Esp)[i];
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }
        n = crash_add(n, "%s%08X", (i % 8) ? " " : "\r\n  ", v);
    }
    __try {
        n = crash_add(n, "\r\nXBE-Header: [0x10000]=%08X [0x10118]=%08X (Zertifikat-Zeiger)\r\n",
                      *(volatile unsigned *)(uintptr_t)0x10000u,
                      *(volatile unsigned *)(uintptr_t)0x10118u);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        n = crash_add(n, "\r\n");
    }
    n = crash_add(n, "cfg: %ux%u window_mode=%d window_hidden=%d win_resize=%d dpi_aware=%d "
                     "fps_limit=%d present_interval=%d vblank_hz=%d interp_fps=%d rt_scale=%d "
                     "shading=%d aspect_mode=%d audio_live=%d hdr_hybrid=%d (aktiv=%d)\r\n",
                  g_cfg.width, g_cfg.height, g_cfg.window_mode, g_cfg.window_hidden,
                  g_cfg.win_resize, g_cfg.dpi_aware, g_cfg.fps_limit,
                  g_cfg.present_interval, g_cfg.vblank_hz, g_cfg.interp_fps,
                  g_cfg.rt_scale, g_cfg.shading, g_cfg.aspect_mode, g_cfg.audio_live,
                  g_cfg.hdr_hybrid, xbe_hdr_hybrid_active());
    /* letzte Logzeilen (Datei wird ungepuffert geschrieben) */
    if (g_log_path[0]) {
        HANDLE lf = CreateFileA(g_log_path, GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (lf != INVALID_HANDLE_VALUE) {
            static char tail[8192];
            LARGE_INTEGER sz;
            DWORD got = 0;
            if (GetFileSizeEx(lf, &sz)) {
                LONGLONG from = sz.QuadPart > (LONGLONG)sizeof tail - 1
                                    ? sz.QuadPart - (LONGLONG)(sizeof tail - 1) : 0;
                LARGE_INTEGER pos;
                pos.QuadPart = from;
                SetFilePointerEx(lf, pos, NULL, FILE_BEGIN);
                if (ReadFile(lf, tail, sizeof tail - 1, &got, NULL)) {
                    const char *t = tail;
                    tail[got] = '\0';
                    if (from > 0) {
                        const char *nl = strchr(tail, '\n');
                        if (nl) t = nl + 1;
                    }
                    n = crash_add(n, "--- letzte Logzeilen (%s) ---\r\n%s", g_log_path, t);
                }
            }
            CloseHandle(lf);
        }
    }
    f = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        WriteFile(f, g_crash_buf, (DWORD)strlen(g_crash_buf), &w, NULL);
        FlushFileBuffers(f);
        CloseHandle(f);
    }
    HLOG_E("CRASHLOG: %s geschrieben (%s, Code=0x%lX RIP=0x%X %s+0x%X)", path, why,
           er->ExceptionCode, (unsigned)rip, mb, (unsigned)(rip - mbase));
    InterlockedExchange(&g_crash_busy, 0);
}

/* DIAG(HDRSWAP,W94): Race-Reproduktion. Erzeugt in den ersten
 * hdr_race_test Sekunden laufend Leer-Threads ueber krnl_CreateThread
 * (wie Audio-Mixer/Spiel-Threads). Im alten Tausch-Pfad (hdr_hybrid=0)
 * sieht das Spiel dabei immer wieder die Loader-PE-Bytes. */
static DWORD WINAPI hdr_race_empty(LPVOID p)
{
    (void)p;
    return 0;
}

static DWORD WINAPI hdr_race_thread(LPVOID p)
{
    ULONGLONG t0 = GetTickCount64();
    ULONGLONG until = t0 + (ULONGLONG)g_cfg.hdr_race_test * 1000u;
    unsigned n = 0, fail = 0;
    (void)p;
    HLOG_I("HDRSWAP race_test: Start (%d s, hybrid=%d)", g_cfg.hdr_race_test,
           xbe_hdr_hybrid_active());
    while (GetTickCount64() < until) {
        HANDLE h = krnl_CreateThread(hdr_race_empty, NULL, 0, NULL);
        if (h) {
            WaitForSingleObject(h, 1000);
            CloseHandle(h);
            n++;
        } else {
            fail++;
        }
    }
    HLOG_I("HDRSWAP race_test: Ende, %u Threads erzeugt, %u Fehler", n, fail);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)inst;
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls((HMODULE)inst);
    }
    return TRUE;
}

static unsigned g_flt0;
static void veh_tbl_fix(PEXCEPTION_POINTERS ep, DWORD tls,
                        unsigned *n)
{
        /* [SIB-base+ESI*4] -> &g_flt0 (re-executes, liest 0). */
        CONTEXT *cx = ep->ContextRecord;
        unsigned char *ip = (unsigned char *)(uintptr_t)cx->Eip;
        unsigned char sib = 0;
        unsigned base = 0;
        __try { sib = ip[2]; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return; }
        base = (unsigned)(uintptr_t)&g_flt0 - cx->Esi * 4u;
        switch (sib & 7u) {
        case 0: cx->Eax = base; break;
        case 1: cx->Ecx = base; break;
        case 2: cx->Edx = base; break;
        case 3: cx->Ebx = base; break;
        case 5: cx->Ebp = base; break;
        case 6: cx->Esi = base; break;
        case 7: cx->Edi = base; break;
        default: return;
        }
        TlsSetValue(tls, (void *)0);
        (*n)++;
    }
    static LONG WINAPI veh(EXCEPTION_POINTERS *ep)
{
    /* M2c: Rekursions-Guard (eigene Probe-Faults nesten endlos, */
    /* Vectored laeuft vor SEH-__try; boot353 veh+0x45A). */
    /* M2c: Guard PRO THREAD (global frisst fremde fatale AVs). */
    static DWORD tls_veh;
    static unsigned char g_font_scratch[256];
    DWORD code;
    void *addr;
    if (!tls_veh) {
        DWORD s = TlsAlloc();
        InterlockedCompareExchange((LONG *)&tls_veh, (LONG)s, 0);
        if (tls_veh != s) { TlsFree(s); }
    }
    /* W41: eigene watch_dr-Sonden-Faults (:rw-Shadow-Read im Handler) noch
     * VOR dem Guard still fortsetzen (statt CONTINUE_SEARCH -> fatal). */
    if (tls_veh && TlsGetValue(tls_veh)) {
        DWORD nc0 = ep->ExceptionRecord->ExceptionCode;
        if (nc0 == EXCEPTION_SINGLE_STEP || nc0 == 0x4000001Eu) {
            ep->ContextRecord->Dr6 = 0;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (TlsGetValue(tls_veh))
        return EXCEPTION_CONTINUE_SEARCH;
    TlsSetValue(tls_veh, (void *)1);
    code = ep->ExceptionRecord->ExceptionCode;
    addr = ep->ExceptionRecord->ExceptionAddress;
    /* M2c: Entry-Log (Ankunftsbeweis, billig, immer). */
    {
        static LONG nentry;
        LONG k = InterlockedIncrement(&nentry);
        if (k <= 200) { /* boot494: Cap f. ALLE (0x2627B flutete 1.3GB!) */
            HLOG_W("VEH-ENTRY #%ld: Code=0x%lX RIP=0x%p T%04x",
                   k, code, addr, GetCurrentThreadId());
        }
    }
    /* M2b: WOW64 meldet HW-BPs als WX86_SINGLE_STEP (0x4000001E). */
    if (code == EXCEPTION_SINGLE_STEP || code == 0x4000001Eu) {
        /* W41 WATCHDR: eigene Slots konsumieren (watch=...). */
        if (watch_dr_veh(ep)) {
            TlsSetValue(tls_veh, (void *)0);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        TlsSetValue(tls_veh, (void *)0); return EXCEPTION_CONTINUE_EXECUTION;
    }
    /* DIAGNOSE: ALLE anderen Codes loggen (stummer Tod!), max 20. */
    if (code != EXCEPTION_ACCESS_VIOLATION &&
        code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_PRIV_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_SINGLE_STEP) {
        static int un;
        if (un < 20) {
            un++;
            HLOG_W("VEH-OTHER: Code=0x%lX RIP=0x%p", code, addr);
            /* W94 CRASHLOG: Dispatch-Abbruch (ungueltige SEH-Kette) bzw.
             * Stack-Ueberlauf enden ohne last_chance -> sofort sichern. */
            if (code == 0xC00001A5u || code == EXCEPTION_STACK_OVERFLOW) {
                crash_report(ep, code == 0xC00001A5u ? "VEH: ungueltiger SEH-Handler (c00001a5)"
                                                     : "VEH: Stack-Ueberlauf");
            }
            if (code == EXCEPTION_BREAKPOINT) {
                xb_u32 *sp;
                xb_u32 c0 = 0xDEADu, c1 = 0xDEADu, c2 = 0xDEADu;
                __try {
                    sp = (xb_u32 *)(uintptr_t)ep->ContextRecord->Esp;
                    c0 = sp[0];
                    c1 = sp[1];
                    c2 = sp[2];
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                }
                HLOG_E("BREAK-PT: ESP=0x%08X EBP=0x%08X "
                       "RET=[0x%08X 0x%08X 0x%08X]",
                       ep->ContextRecord->Esp, ep->ContextRecord->Ebp,
                       c0, c1, c2);
            }
        }
    }
    if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
        code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_INT_DIVIDE_BY_ZERO) {
        /* EIGENE DLL: sofort SEARCH (eigene __try greifen lassen!). */
        {
            xb_u32 rip0 = (xb_u32)ep->ContextRecord->Eip;
            if (rip0 >= 0x30000000u && rip0 < 0x31000000u) {
                static int nownd;
                if (nownd < 6) {
                    nownd++;
                    HLOG_W("VEH-OWN: RIP=0x%X -> SEARCH", rip0);
                }
                TlsSetValue(tls_veh, (void *)0);
                return EXCEPTION_CONTINUE_SEARCH;
            }
        }
        /* DIAG(NULLCALL, W13): Sprung auf Adresse 0 (call ueber NULL-Zeiger).
         * Der generische Dump ist hier nutzlos (RIP=0) und nach 40 Fehlern
         * gedeckelt: stattdessen Aufrufer (=[ESP]) + Aufruf-Befehl + SEH-Kette
         * (wer faengt ab?) + HRTF-Zeigerstand, jeden eindeutigen Aufrufer
         * einmal, danach nur Zaehler. nullcall_diag=0 = alter Pfad. */
        if (code == EXCEPTION_ACCESS_VIOLATION &&
            addr == (void *)(uintptr_t)0 && g_cfg.nullcall_diag) {
            static xb_u32 nc_caller[16];
            static unsigned nc_count[16], nc_n, nc_total, nc_overflow;
            CONTEXT *cx = ep->ContextRecord;
            xb_u32 caller = 0, esp0 = 0;
            unsigned char cb[8];
            unsigned i, slot = 0;
            int known = 0;
            __try {
                esp0 = (xb_u32)cx->Esp;
                caller = *(xb_u32 *)(uintptr_t)esp0;
                memcpy(cb, (void *)(uintptr_t)(caller - 8), 8);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                memset(cb, 0xEE, sizeof cb);
            }
            for (i = 0; i < nc_n; i++) {
                if (nc_caller[i] == caller) { slot = i; known = 1; break; }
            }
            nc_total++;
            if (known) {
                nc_count[slot]++;
            } else if (nc_n < 16) {
                slot = nc_n++;
                nc_caller[slot] = caller;
                nc_count[slot] = 1;
            } else {
                nc_overflow++;
                if (nc_overflow == 1) {
                    HLOG_W("NULLCALL: Tabelle voll (16 Aufrufer), "
                           "Rest nur gezaehlt");
                }
            }
            if (!known && nc_overflow == 0) {
                xb_u32 h94 = 0xDEADu, h98 = 0xDEADu, h9C = 0xDEADu;
                __try {
                    h94 = *(xb_u32 *)(uintptr_t)SBA(0x23F694u);
                    h98 = *(xb_u32 *)(uintptr_t)SBA(0x23F698u);
                    h9C = *(xb_u32 *)(uintptr_t)SBA(0x23F69Cu);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                }
                HLOG_W("NULLCALL #%u: caller=0x%08X ESP=0x%08X "
                       "EAX=0x%08X ECX=0x%08X EDX=0x%08X "
                       "ESI=0x%08X EDI=0x%08X EBP=0x%08X rw=%u",
                       nc_total, caller, esp0, cx->Eax, cx->Ecx, cx->Edx,
                       cx->Esi, cx->Edi, cx->Ebp,
                       (unsigned)ep->ExceptionRecord->ExceptionInformation[0]);
                HLOG_W("NULLCALL: bytes[caller-8]=%02X %02X %02X %02X "
                       "%02X %02X %02X %02X",
                       cb[0], cb[1], cb[2], cb[3], cb[4], cb[5], cb[6], cb[7]);
                /* FF 15 <addr32> = call [abs]: Zieladresse + Zeigerwert. */
                if (cb[2] == 0xFFu && cb[3] == 0x15u) {
                    xb_u32 tgt = 0, val = 0xDEADu;
                    memcpy(&tgt, cb + 4, 4);
                    __try { val = *(xb_u32 *)(uintptr_t)tgt; }
                    __except (EXCEPTION_EXECUTE_HANDLER) {
                    }
                    HLOG_W("NULLCALL: call [0x%08X] = 0x%08X", tgt, val);
                }
                HLOG_W("NULLCALL: HRTF [23F694]=0x%08X [23F698]=0x%08X "
                       "[23F69C]=0x%08X", h94, h98, h9C);
                /* SEH-Kette: wer faengt den Sprung nach 0 ab? */
                __try {
                    xb_u32 *rec = (xb_u32 *)(uintptr_t)__readfsdword(0);
                    int d;
                    for (d = 0; d < 4; d++) {
                        xb_u32 next, hdl;
                        char mod[MAX_PATH];
                        MEMORY_BASIC_INFORMATION smbi;
                        if (rec == (xb_u32 *)(uintptr_t)0xFFFFFFFFu || !rec)
                            break;
                        next = rec[0];
                        hdl = rec[1];
                        mod[0] = '\0';
                        if (VirtualQuery((void *)(uintptr_t)hdl,
                                         &smbi, sizeof smbi)) {
                            char full[MAX_PATH];
                            if (GetModuleFileNameA(
                                    (HMODULE)smbi.AllocationBase,
                                    full, sizeof full)) {
                                const char *b = strrchr(full, 0x5C);
                                strcpy_s(mod, sizeof mod, b ? b + 1 : full);
                            }
                        }
                        HLOG_W("NULLCALL: SEH#%d handler=0x%08X (%s)",
                               d, hdl, mod);
                        if (next == 0xFFFFFFFFu)
                            break;
                        rec = (xb_u32 *)(uintptr_t)next;
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                }
            } else if ((nc_total % 100) == 0) {
                HLOG_W("NULLCALL: total=%u unique=%u overflow=%u",
                       nc_total, nc_n, nc_overflow);
            }
            TlsSetValue(tls_veh, (void *)0);
            return EXCEPTION_CONTINUE_SEARCH;
        }
        /* M2c: Verbose-Dump drosseln (Fault-Stuerme, z.B. Hash-Probes). */
        static int nvehav;
        /* M2c: Crash-Stelle 0x2627B IMMER voll dumpen (fataler Treffer). */
        int vfull = (nvehav < 40) || (addr == (void *)(uintptr_t)SBA(0x2627Bu));
        nvehav++;
        if (addr == (void *)(uintptr_t)SBA(0x262A0u)) {
            static unsigned ntbl;
            void *fault = (void *)ep->ExceptionRecord->ExceptionInformation[1];
            MEMORY_BASIC_INFORMATION tqm;
            SIZE_T tqr = VirtualQuery(fault, &tqm, sizeof tqm);
            if (code == EXCEPTION_ACCESS_VIOLATION &&
                (tqr == 0 || tqm.State != MEM_COMMIT)) {
                unsigned before = ntbl;
                veh_tbl_fix(ep, tls_veh, &ntbl);
                if (ntbl != before) {
                    if ((ntbl & 0xFFF) == 1) {
                        HLOG_T("VEHt: tbl->0 @0x262A0 (n=%u)", "0x262A0", ntbl);
                    }
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
            }
        }
        if (addr == (void *)(uintptr_t)SBA(0x26343u)) {
            static unsigned ntbl;
            void *fault = (void *)ep->ExceptionRecord->ExceptionInformation[1];
            MEMORY_BASIC_INFORMATION tqm;
            SIZE_T tqr = VirtualQuery(fault, &tqm, sizeof tqm);
            if (code == EXCEPTION_ACCESS_VIOLATION &&
                (tqr == 0 || tqm.State != MEM_COMMIT)) {
                unsigned before = ntbl;
                veh_tbl_fix(ep, tls_veh, &ntbl);
                if (ntbl != before) {
                    if ((ntbl & 0xFFF) == 1) {
                        HLOG_T("VEHt: tbl->0 @0x26343 (n=%u)", "0x26343", ntbl);
                    }
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
            }
        }
        if (addr == (void *)(uintptr_t)SBA(0x2636Cu)) {
            static unsigned ntbl;
            void *fault = (void *)ep->ExceptionRecord->ExceptionInformation[1];
            MEMORY_BASIC_INFORMATION tqm;
            SIZE_T tqr = VirtualQuery(fault, &tqm, sizeof tqm);
            if (code == EXCEPTION_ACCESS_VIOLATION &&
                (tqr == 0 || tqm.State != MEM_COMMIT)) {
                unsigned before = ntbl;
                veh_tbl_fix(ep, tls_veh, &ntbl);
                if (ntbl != before) {
                    if ((ntbl & 0xFFF) == 1) {
                        HLOG_T("VEHt: tbl->0 @0x2636C (n=%u)", "0x2636C", ntbl);
                    }
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
            }
        }
        if (addr == (void *)(uintptr_t)SBA(0x26392u)) {
            static unsigned ntbl;
            void *fault = (void *)ep->ExceptionRecord->ExceptionInformation[1];
            MEMORY_BASIC_INFORMATION tqm;
            SIZE_T tqr = VirtualQuery(fault, &tqm, sizeof tqm);
            if (code == EXCEPTION_ACCESS_VIOLATION &&
                (tqr == 0 || tqm.State != MEM_COMMIT)) {
                unsigned before = ntbl;
                veh_tbl_fix(ep, tls_veh, &ntbl);
                if (ntbl != before) {
                    if ((ntbl & 0xFFF) == 1) {
                        HLOG_T("VEHt: tbl->0 @0x26392 (n=%u)", "0x26392", ntbl);
                    }
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
            }
        }
        { /* SIBSKIP: generischer mov r32,[SIB]-Skip bei UAF */
            xb_u32 rip0 = (xb_u32)ep->ContextRecord->Eip;
            if (code == EXCEPTION_ACCESS_VIOLATION && rip0 >= 0x10000u && rip0 < 0x100000u) {
                void *flt = (void *)ep->ExceptionRecord->ExceptionInformation[1];
                MEMORY_BASIC_INFORMATION mi; SIZE_T qr = VirtualQuery(flt, &mi, sizeof mi);
                /* BULKFIX (boot494-496): Fault-Seite LESBAR machen +
                 * Re-Execute (statt 15M Einzel-Skips!). Deckt FREI
                 * (Reserve+Commit), RESERVE (Commit) und COMMITTED+
                 * NOACCESS/GUARD (Protect->READWRITE). Nur <2GB! */
                if ((uintptr_t)flt < 0x80000000u &&
                    ep->ExceptionRecord->ExceptionInformation[0] == 0 &&
                    flt != (void *)(uintptr_t)rip0 &&
                    (qr == 0 || (mi.State & MEM_RESERVE) ||
                     (mi.State & MEM_COMMIT) == 0 ||
                     (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))) {
                    void *pg = (void *)((uintptr_t)flt & ~(uintptr_t)0xFFFu);
                    static unsigned ncm, cmlog;
                    int ok = 0;
                    if ((qr != 0 && (mi.State & MEM_COMMIT) &&
                         (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))) {
                        DWORD op = 0;
                        ok = VirtualProtect(pg, 4096, PAGE_READWRITE, &op);
                    } else {
                        ok = VirtualAlloc(pg, 4096,
                            (qr == 0 || (mi.State & MEM_COMMIT) == 0)
                                ? (MEM_RESERVE | MEM_COMMIT) : MEM_COMMIT,
                            PAGE_READWRITE) != NULL;
                    }
                    if (ok) {
                        ncm++;
                        if (ncm - cmlog >= 4096u) {
                            cmlog = ncm;
                            HLOG_W("VEH commit: page=0x%p total=%u", pg, ncm);
                        }
                        TlsSetValue(tls_veh, (void *)0);
                        return EXCEPTION_CONTINUE_EXECUTION;
                    }
                }
                if (qr == 0 || mi.State != MEM_COMMIT) {
                    unsigned char b0 = 0, b1 = 0, b2 = 0;
                    __try { b0 = *(unsigned char *)(uintptr_t)rip0;
                            b1 = *(unsigned char *)(uintptr_t)(rip0 + 1);
                            b2 = *(unsigned char *)(uintptr_t)(rip0 + 2); }
                    __except (EXCEPTION_EXECUTE_HANDLER) {}
                    if (b0 == 0x8Bu && (b1 & 0xC7u) == 0x04u) {
                        unsigned dst = (b1 >> 3) & 7u;
                        unsigned len = ((b2 & 7u) == 5u) ? 7u : 3u;
                        static unsigned nsib;
                        CONTEXT *cx0 = ep->ContextRecord;
                        int doit = 1;
                        switch (dst) {
                        case 0: cx0->Eax = 0; break;
                        case 1: cx0->Ecx = 0; break;
                        case 2: cx0->Edx = 0; break;
                        case 3: cx0->Ebx = 0; break;
                        case 4: case 5: doit = 0; break;
                        case 6: cx0->Esi = 0; break;
                        default: cx0->Edi = 0; break;
                        }
                        if (doit) {
                        cx0->Eip += len;
                        TlsSetValue(tls_veh, (void *)0);
                        nsib++;
                        if ((nsib & 0xFFF) == 1) {
                            HLOG_T("SIBSKIP @0x%X dst=%u (n=%u)", rip0, dst, nsib);
                        }
                        return EXCEPTION_CONTINUE_EXECUTION;
                        }
                    }
                }
            }
        }
        if (addr == (void *)(uintptr_t)SBA(0x2627Bu)) {
            static unsigned n2627;
            { static int lg; if (!lg) { lg = 1;
                void *flt = (void *)ep->ExceptionRecord->ExceptionInformation[1];
                MEMORY_BASIC_INFORMATION mi; SIZE_T qr = VirtualQuery(flt, &mi, sizeof mi);
                HLOG_W("2627B-DIAG: fault=0x%p qr=%u state=0x%X prot=0x%X ECX=0x%X ESI=0x%X",
                    flt, (unsigned)qr, qr ? (unsigned)mi.State : 0,
                    qr ? (unsigned)mi.Protect : 0,
                    ep->ContextRecord->Ecx, ep->ContextRecord->Esi); } }
            void *fault = (void *)ep->ExceptionRecord->ExceptionInformation[1];
            MEMORY_BASIC_INFORMATION fqm;
            SIZE_T fqr = VirtualQuery(fault, &fqm, sizeof fqm);
            if (code == EXCEPTION_ACCESS_VIOLATION &&
                (fqr == 0 || fqm.State != MEM_COMMIT ||
                 (fqm.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
                                 PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY |
                                 PAGE_EXECUTE_WRITECOPY)) == 0)) {
                ep->ContextRecord->Edx = 0;
                ep->ContextRecord->Eip += 3;
                /* LOOPCAP (53k-Fix): String-Loop-Zaehler [ESP+0x34] deckeln
                 * (UAF-String ohne Terminator -> Millionen Iterationen!). */
                __try { unsigned *pcnt = (unsigned *)(uintptr_t)
                            (ep->ContextRecord->Esp + 0x34);
                        if (*pcnt > 50000u) { *pcnt = 50000u; } }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
                TlsSetValue(tls_veh, (void *)0);
                n2627++;
                if ((n2627 & 0xFFF) == 1) {
                    HLOG_T("VEH2627: OOB->0 (n=%u)", n2627);
                }
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
        if (addr == (void *)(uintptr_t)SBA(0x21F7EDu) &&
            ep->ContextRecord->Esi == 0) {
            /* NULL-Arg (Mission-Load 25k): -> Epilog 0x21F83D (EAX=0). */
            static unsigned n21f;
            ep->ContextRecord->Eip = SBA(0x21F83Du);
            ep->ContextRecord->Eax = 1u;
            TlsSetValue(tls_veh, (void *)0);
            n21f++;
            if (n21f <= 5) {
                unsigned caller = 0, esp0 = 0;
                __try {
                    esp0 = (unsigned)ep->ContextRecord->Esp;
                    caller = *(unsigned *)(uintptr_t)esp0;
                } __except (EXCEPTION_EXECUTE_HANDLER) {}
                HLOG_I("VEH21F7ED: NULL->Epilog EAX=1 caller=0x%X esp=0x%X (n=%u)",
                       caller, esp0, n21f);
            }
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (addr == (void *)(uintptr_t)SBA(0x18E904u)) {
            /* [EDI+2]<<4 = Rueck-Offset; 0xFFFF = Header leer. */
            static unsigned f904;
            __try {
                *(unsigned short *)(uintptr_t)
                    (ep->ContextRecord->Edi + 2) = 0;
                f904++;
                if ((f904 & 0x3FF) == 1) {
                    HLOG_T("VEH904: hdr=0 (n=%u)", f904);
                }
                TlsSetValue(tls_veh, (void *)0);
                return EXCEPTION_CONTINUE_EXECUTION;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        if (addr == (void *)(uintptr_t)SBA(0x18E99Fu)) {
            static unsigned ffx;
            xb_u32 fesi = ep->ContextRecord->Esi;
            __try {
                *(xb_u32 *)(uintptr_t)(fesi + 0xC) =
                    (xb_u32)(uintptr_t)g_font_scratch;
                ffx++;
                if ((ffx & 0xFFF) == 1) {
                    HLOG_T("VEH18E: p3->scratch (n=%u)", ffx);
                }
                TlsSetValue(tls_veh, (void *)0);
                return EXCEPTION_CONTINUE_EXECUTION;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        if (!vfull) {
            if ((nvehav % 1000) == 0) {
                HLOG_W("VEH: ... (Dump #%d, Code=0x%lX RIP=0x%p) [gedrosselt]",
                       nvehav, code, addr);
            }
        } else {
        __try {
        MEMORY_BASIC_INFORMATION mbi;
        char mod[MAX_PATH];
        mod[0] = '\0';
        if (VirtualQuery(addr, &mbi, sizeof mbi)) {
            GetModuleFileNameA((HMODULE)mbi.AllocationBase, mod, sizeof mod);
        }
        HLOG_E("VEH: Code=0x%lX RIP=0x%p (%s) Fault=0x%p EAX=0x%08lX ECX=0x%08lX "
               "EDX=0x%08lX ESP=0x%08lX EBP=0x%08lX",
               code, addr, mod,
               (void *)ep->ExceptionRecord->ExceptionInformation[1],
               ep->ContextRecord->Eax, ep->ContextRecord->Ecx,
               ep->ContextRecord->Edx, ep->ContextRecord->Esp,
               ep->ContextRecord->Ebp);
        /* DIAGNOSE: ESI/EBX/EDI + [ESI+0/+4] (D3D-Objekt-Identifikation). */
        {
            xb_u32 esi = ep->ContextRecord->Esi;
            xb_u32 ebx = ep->ContextRecord->Ebx;
            xb_u32 edi = ep->ContextRecord->Edi;
            xb_u32 s0 = 0xDEADu, s4 = 0xDEADu;
            __try {
                s0 = *(xb_u32 *)(uintptr_t)esi;
                s4 = *(xb_u32 *)(uintptr_t)(esi + 4);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
            HLOG_E("VEH regs: ESI=0x%08X EBX=0x%08X EDI=0x%08X "
                   "[ESI+0]=0x%08X [ESI+4]=0x%08X",
                   esi, ebx, edi, s0, s4);
            /* M2c: Fault-Herkunft (MemReg). */
            {
                extern unsigned krnl_memreg_find(unsigned, unsigned *, unsigned *);
                unsigned fb = 0, fl = 0;
                unsigned fk = krnl_memreg_find(
                    (unsigned)ep->ExceptionRecord->ExceptionInformation[1],
                    &fb, &fl);
                HLOG_E("VEH memreg: fault-kind=%u base=0x%X len=0x%X",
                       fk, fb, fl);
                {
                    unsigned b1 = 0, l1 = 0, b2 = 0, l2 = 0, b3 = 0, l3 = 0;
                    unsigned k1 = krnl_memreg_find(ep->ContextRecord->Esi, &b1, &l1);
                    unsigned k2 = krnl_memreg_find(ep->ContextRecord->Ebx, &b2, &l2);
                    unsigned k3 = krnl_memreg_find(ep->ContextRecord->Edi, &b3, &l3);
                    HLOG_E("VEH memreg3: esi=%u:0x%X edi=%u:0x%X ebx=%u:0x%X",
                           k1, b1, k3, b3, k2, b2);
                }
            }
        }

        /* Stack-Dump: Ruecksprungadressen im Spiel-Bereich markieren */
        {
            xb_u32 *sp = (xb_u32 *)(uintptr_t)ep->ContextRecord->Esp;
            char line[512];
            int n = 0, i;
            n += _snprintf_s(line + n, sizeof line - n, _TRUNCATE, "VEH stack:");
            for (i = 0; i < 48; i++) {
                __try {
                    xb_u32 v = sp[i];
                    char mark = (v >= 0x10000 && v < 0xB00000) ? '*' : ' ';
                    n += _snprintf_s(line + n, sizeof line - n, _TRUNCATE, " %c%08X",
                                     mark, v);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    break;
                }
            }
            HLOG_E("%s (* = Spiel-Code)", line);
        }
        /* AV-SYMS: RIP + erste 6 DLL-Ret-Addrs aufloesen (PDB!). */
        {
            static int syminit;
            xb_u32 *sp = (xb_u32 *)(uintptr_t)ep->ContextRecord->Esp;
            xb_u32 rip = (xb_u32)ep->ContextRecord->Eip;
            xb_u32 addrs[7];
            int i, na = 0;
            if (!syminit) {
                syminit = 1;
                SymInitialize(GetCurrentProcess(), NULL, TRUE);
            }
            addrs[na++] = rip;
            for (i = 0; i < 48 && na < 7; i++) {
                __try {
                    xb_u32 v = sp[i];
                    if (v >= 0x50000000u && v < 0x60000000u) {
                        int k, dup = 0;
                        for (k = 0; k < na; k++)
                            if (addrs[k] == v) dup = 1;
                        if (!dup) addrs[na++] = v;
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    break;
                }
            }
            for (i = 0; i < na; i++) {
                char sbuf[sizeof(SYMBOL_INFO) + 128];
                SYMBOL_INFO *si = (SYMBOL_INFO *)sbuf;
                DWORD64 disp = 0;
                si->SizeOfStruct = sizeof(SYMBOL_INFO);
                si->MaxNameLen = 128;
                __try {
                    if (SymFromAddr(GetCurrentProcess(), addrs[i], &disp, si)) {
                        HLOG_E("VEH sym%d: 0x%08X %s+0x%X", i, addrs[i],
                               si->Name, (unsigned)disp);
                    } else {
                        HLOG_E("VEH sym%d: 0x%08X (?)", i, addrs[i]);
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                }
            }
        }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        } /* else vfull */
        /* W94 CRASHLOG: Ausnahme in Spielcode, die der VEH nicht behebt
         * (erste Chance; Spiel-SEH koennte sie noch fangen). */
        {
            xb_u32 ripx = (xb_u32)ep->ContextRecord->Eip;
            if (ripx >= 0x10000u && ripx < 0x2810000u) {
                crash_report(ep, "VEH: Spielcode-Ausnahme ohne VEH-Behandlung (erste Chance)");
            }
        }
    }
    TlsSetValue(tls_veh, (void *)0); return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI last_chance(EXCEPTION_POINTERS *ep)
{
    HLOG_E("FATAL: unbehandelte Exception 0x%lX @0x%p - Abbruch",
           ep->ExceptionRecord->ExceptionCode,
           ep->ExceptionRecord->ExceptionAddress);
    crash_report(ep, "last_chance: unbehandelte Ausnahme");
    krnl_dump_stats();
    hlog_close();
    ExitProcess(1);
    return EXCEPTION_EXECUTE_HANDLER; /* unreachable */
}

/* THREADWATCH (31.10.): alle 30 s CPU-Zeit pro Xbox-Thread. Ein Thread,
 * der >= 60 s keine CPU bekommt, waehrend Frames weiterlaufen, wird mit
 * Warteposition (Modul/Symbol) und erster Spieladresse auf dem Stack
 * gemeldet -> Starvation sichtbar statt "alle Threads in ntdll = Deadlock"
 * (Lade-Hang M4). Normales Idle (Event-Waits) meldet sich auch: Position
 * ansehen. Deckel: 24 Meldungen pro Lauf. */
static void threadwatch(int waited)
{
    extern int krnl_xb_thread_count(void);
    extern int krnl_xb_thread_get(int, void **, unsigned *);
    static ULONGLONG last_cpu[32];
    static int idle_s[32], reported[32], nrep;
    static unsigned last_ret[32]; /* pro Thread jede Warteposition nur 1x */
    static unsigned last_frames;
    unsigned fr = hooks_frame_count();
    int i, n = krnl_xb_thread_count();
    int frames_moving = fr != last_frames;
    if (waited % 30 != 0) return;
    last_frames = fr;
    for (i = 0; i < n && i < 32; i++) {
        void *h = NULL; unsigned rt = 0;
        FILETIME c, e, k, u;
        DWORD code = 0;
        ULONGLONG cpu;
        if (!krnl_xb_thread_get(i, &h, &rt) || !h) continue;
        if (!GetExitCodeThread((HANDLE)h, &code) || code != STILL_ACTIVE) continue;
        if (!GetThreadTimes((HANDLE)h, &c, &e, &k, &u)) continue;
        cpu = ((ULONGLONG)k.dwHighDateTime << 32 | k.dwLowDateTime) +
              ((ULONGLONG)u.dwHighDateTime << 32 | u.dwLowDateTime);
        if (cpu == last_cpu[i]) {
            idle_s[i] += 30;
        } else {
            idle_s[i] = 0;
            reported[i] = 0;
        }
        last_cpu[i] = cpu;
        if (idle_s[i] >= 60 && frames_moving && !reported[i] && nrep < 24) {
            CONTEXT cx;
            char mod[MAX_PATH] = "?";
            unsigned game = 0;
            reported[i] = 1;
            nrep++;
            if (SuspendThread((HANDLE)h) == (DWORD)-1) continue;
            memset(&cx, 0, sizeof cx);
            cx.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext((HANDLE)h, &cx)) {
                MEMORY_BASIC_INFORMATION mbi;
                int s;
                if (VirtualQuery((void *)(uintptr_t)cx.Eip, &mbi, sizeof mbi)) {
                    char full[MAX_PATH];
                    if (GetModuleFileNameA((HMODULE)mbi.AllocationBase, full, sizeof full)) {
                        const char *b = strrchr(full, 0x5C);
                        strcpy_s(mod, sizeof mod, b ? b + 1 : full);
                    }
                }
                __try {
                    for (s = 0; s < 256 && !game; s++) {
                        unsigned v = ((unsigned *)(uintptr_t)cx.Esp)[s];
                        if (v >= 0x12000u && v < 0x20C000u) game = v; /* .text */
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
            ResumeThread((HANDLE)h);
            if (game == last_ret[i]) {
                nrep--; /* gleiche Position wie zuletzt (regulaerer Schlaf) */
                continue;
            }
            last_ret[i] = game;
            HLOG_W("THREADWATCH: thr%d (rt=0x%X) seit %ds ohne CPU, Frames laufen: EIP=0x%08X (%s) Spiel-Ret=0x%X",
                   i, rt, idle_s[i], (unsigned)cx.Eip, mod, game);
        }
    }
}

/* W32 STALL20: Erkennt die bekannte Lade-Pause (kein Haenger): Main steht
 * in FUN_0010d8d0 (Audio-Drain-Poll, Sleep(5) bis 1024x = 5,1-s-Timeout,
 * Ruecksprung 0x10D93E) im XDK-Sleep (Ruecksprung 0x1910E7). Die Rufkette
 * wird in Reihenfolge verifiziert (0x1910E7 unten, 0x10D93E hoechstens
 * 24 Worte darueber = echter Frame, kein Stale-Wert). Nur thr0 (Main,
 * Routine 0x1924A8). Fehlschlag = 0 (fail-closed: dann STALL melden). */
static int w32_main_in_audiopoll(void)
{
    void *h = NULL; unsigned rt = 0;
    CONTEXT cx;
    int i, dist;
    if (krnl_xb_thread_count() < 1)
        return 0;
    if (!krnl_xb_thread_get(0, &h, &rt) || !h)
        return 0;
    if (rt != SBA(0x1924A8u))
        return 0;
    if (SuspendThread((HANDLE)h) == (DWORD)-1)
        return 0;
    memset(&cx, 0, sizeof cx);
    cx.ContextFlags = CONTEXT_CONTROL;
    if (GetThreadContext((HANDLE)h, &cx)) {
        __try {
            unsigned *sp = (unsigned *)(uintptr_t)cx.Esp;
            for (i = 0, dist = -1; i < 256; i++) {
                unsigned v = sp[i];
                if (dist < 0) {
                    if (v == SBA(0x1910E7u))
                        dist = 0;
                } else if (++dist > 24) {
                    break;
                } else if (v == SBA(0x10D93Eu)) {
                    ResumeThread((HANDLE)h);
                    return 1;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    ResumeThread((HANDLE)h);
    return 0;
}

/* W145 DIAG(LOADLOCK): Stack eines Sperr-Halters, der kein Xbox-Thread ist
 * (VR-Thread, Present-Takt, UI ...; W134/W145: VR-Thread hielt ctx_lock in
 * der Runtime). Sicher gegen Halter in Heap/Loader der Runtime: waehrend der
 * Suspendierung nur CONTEXT + Kopie des Stacks in einen vorab reservierten
 * Puffer (keine Allokation, kein Log); Symbolisieren erst nach ResumeThread
 * ueber eine Lese-Routine, die Stack-Adressen aus der Kopie liefert. */
#define WD_SNAP_MAX (128u * 1024u)
static int wd_syminit;
static unsigned char *wd_snap;
static DWORD wd_snap_lo, wd_snap_n;

static void wd_sym_init(void)
{
    if (!wd_syminit) {
        wd_syminit = 1;
        SymInitialize(GetCurrentProcess(), NULL, TRUE);
    }
}

static BOOL CALLBACK wd_snap_read(HANDLE proc, DWORD64 base, PVOID buf, DWORD size, LPDWORD got)
{
    DWORD a = (DWORD)base;
    SIZE_T r = 0;
    BOOL ok;
    if (wd_snap_n && a >= wd_snap_lo && size <= wd_snap_n && a - wd_snap_lo <= wd_snap_n - size) {
        memcpy(buf, wd_snap + (a - wd_snap_lo), size);
        if (got) { *got = size; }
        return TRUE;
    }
    ok = ReadProcessMemory(proc, (LPCVOID)(uintptr_t)a, buf, size, &r);
    if (got) { *got = (DWORD)r; }
    return ok;
}

static const char *wd_role(DWORD tid)
{
    extern unsigned long sb_vr_thread_tid(void);
    extern volatile DWORD g_pd_tid;
    if (tid && tid == (DWORD)sb_vr_thread_tid()) { return "VR-Thread"; }
    if (tid && tid == g_pd_tid) { return "Present-Takt-Thread"; }
    return "Host-Thread";
}

static void wd_holder_stacks(void)
{
    extern int krnl_lock_foreign_owners(unsigned long *, const char **, int);
    unsigned long tids[4];
    const char *names[4];
    int n = krnl_lock_foreign_owners(tids, names, 4), k;
    for (k = 0; k < n; k++) {
        HANDLE h;
        CONTEXT cx;
        MEMORY_BASIC_INFORMATION mbi;
        STACKFRAME64 sf;
        int di, ok = 0;
        h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tids[k]);
        HLOG_W("DIAG(LOADLOCK): Halter %s T%04x = %s (kein Xbox-Thread)%s", names[k], tids[k] & 0xFFFFu,
               wd_role(tids[k]), h ? ", Stack:" : " - OpenThread scheitert");
        if (!h) { continue; }
        if (!wd_snap) { wd_snap = (unsigned char *)VirtualAlloc(NULL, WD_SNAP_MAX, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); }
        wd_sym_init();
        wd_snap_n = 0;
        if (SuspendThread(h) == (DWORD)-1) { CloseHandle(h); continue; }
        memset(&cx, 0, sizeof cx);
        cx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (GetThreadContext(h, &cx)) {
            ok = 1;
            if (wd_snap && VirtualQuery((void *)(uintptr_t)cx.Esp, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT) {
                DWORD top = (DWORD)(uintptr_t)mbi.BaseAddress + (DWORD)mbi.RegionSize;
                DWORD len = top > cx.Esp ? top - cx.Esp : 0;
                if (len > WD_SNAP_MAX) { len = WD_SNAP_MAX; }
                __try {
                    memcpy(wd_snap, (const void *)(uintptr_t)cx.Esp, len);
                    wd_snap_lo = cx.Esp;
                    wd_snap_n = len;
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    wd_snap_n = 0;
                }
            }
        }
        ResumeThread(h);
        if (ok) {
            memset(&sf, 0, sizeof sf);
            sf.AddrPC.Offset = cx.Eip;
            sf.AddrPC.Mode = AddrModeFlat;
            sf.AddrFrame.Offset = cx.Ebp;
            sf.AddrFrame.Mode = AddrModeFlat;
            sf.AddrStack.Offset = cx.Esp;
            sf.AddrStack.Mode = AddrModeFlat;
            for (di = 0; di < 20; di++) {
                BYTE sbuf[sizeof(SYMBOL_INFO) + 64];
                SYMBOL_INFO *si = (SYMBOL_INFO *)sbuf;
                DWORD64 disp = 0;
                MEMORY_BASIC_INFORMATION fmbi;
                char fmod[MAX_PATH];
                const char *bn = "?";
                if (!StackWalk64(IMAGE_FILE_MACHINE_I386, GetCurrentProcess(), h, &sf, &cx, wd_snap_read,
                                 SymFunctionTableAccess64, SymGetModuleBase64, NULL)) {
                    break;
                }
                if (!sf.AddrPC.Offset) { break; }
                fmod[0] = 0;
                if (VirtualQuery((void *)(uintptr_t)sf.AddrPC.Offset, &fmbi, sizeof fmbi)) {
                    GetModuleFileNameA((HMODULE)fmbi.AllocationBase, fmod, sizeof fmod);
                    bn = strrchr(fmod, 0x5C);
                    bn = bn ? bn + 1 : fmod;
                }
                si->SizeOfStruct = sizeof(SYMBOL_INFO);
                si->MaxNameLen = 64;
                if (SymFromAddr(GetCurrentProcess(), sf.AddrPC.Offset, &disp, si)) {
                    HLOG_W("DIAG(LOADLOCK): Halter T%04x #%d: 0x%08X %s!%s+0x%X", tids[k] & 0xFFFFu, di,
                           (unsigned)sf.AddrPC.Offset, bn, si->Name, (unsigned)disp);
                } else {
                    HLOG_W("DIAG(LOADLOCK): Halter T%04x #%d: 0x%08X %s", tids[k] & 0xFFFFu, di,
                           (unsigned)sf.AddrPC.Offset, bn);
                }
            }
        }
        CloseHandle(h);
    }
}

/* DIAG(VR-FIX3,W183): Stack eines Host-Threads (VR-Thread steht ausserhalb eines
 * OpenXR-Aufrufs). Nur Suspendieren + CONTEXT + Stack-Kopie, Symbolisieren danach
 * (wie wd_holder_stacks). Aufruf aus dem Watchdog. */
void sb_wd_thread_stack(unsigned long tid, const char *label)
{
    HANDLE h;
    CONTEXT cx;
    MEMORY_BASIC_INFORMATION mbi;
    STACKFRAME64 sf;
    int di, ok = 0;
    h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!h) { HLOG_W("DIAG(VR-FIX3): %s T%04x - OpenThread scheitert", label, (unsigned)(tid & 0xFFFFu)); return; }
    if (!wd_snap) { wd_snap = (unsigned char *)VirtualAlloc(NULL, WD_SNAP_MAX, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); }
    wd_sym_init();
    wd_snap_n = 0;
    if (SuspendThread(h) == (DWORD)-1) { CloseHandle(h); return; }
    memset(&cx, 0, sizeof cx);
    cx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (GetThreadContext(h, &cx)) {
        ok = 1;
        if (wd_snap && VirtualQuery((void *)(uintptr_t)cx.Esp, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT) {
            DWORD top = (DWORD)(uintptr_t)mbi.BaseAddress + (DWORD)mbi.RegionSize;
            DWORD len = top > cx.Esp ? top - cx.Esp : 0;
            if (len > WD_SNAP_MAX) { len = WD_SNAP_MAX; }
            __try {
                memcpy(wd_snap, (const void *)(uintptr_t)cx.Esp, len);
                wd_snap_lo = cx.Esp;
                wd_snap_n = len;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                wd_snap_n = 0;
            }
        }
    }
    ResumeThread(h);
    if (ok) {
        memset(&sf, 0, sizeof sf);
        sf.AddrPC.Offset = cx.Eip; sf.AddrPC.Mode = AddrModeFlat;
        sf.AddrFrame.Offset = cx.Ebp; sf.AddrFrame.Mode = AddrModeFlat;
        sf.AddrStack.Offset = cx.Esp; sf.AddrStack.Mode = AddrModeFlat;
        for (di = 0; di < 24; di++) {
            BYTE sbuf[sizeof(SYMBOL_INFO) + 64];
            SYMBOL_INFO *si = (SYMBOL_INFO *)sbuf;
            DWORD64 disp = 0;
            MEMORY_BASIC_INFORMATION fmbi;
            char fmod[MAX_PATH];
            const char *bn = "?";
            if (!StackWalk64(IMAGE_FILE_MACHINE_I386, GetCurrentProcess(), h, &sf, &cx, wd_snap_read,
                             SymFunctionTableAccess64, SymGetModuleBase64, NULL)) { break; }
            if (!sf.AddrPC.Offset) { break; }
            fmod[0] = 0;
            if (VirtualQuery((void *)(uintptr_t)sf.AddrPC.Offset, &fmbi, sizeof fmbi)) {
                GetModuleFileNameA((HMODULE)fmbi.AllocationBase, fmod, sizeof fmod);
                bn = strrchr(fmod, 0x5C);
                bn = bn ? bn + 1 : fmod;
            }
            si->SizeOfStruct = sizeof(SYMBOL_INFO);
            si->MaxNameLen = 64;
            if (SymFromAddr(GetCurrentProcess(), sf.AddrPC.Offset, &disp, si)) {
                HLOG_W("DIAG(VR-FIX3): %s T%04x #%d: 0x%08X %s!%s+0x%X", label, (unsigned)(tid & 0xFFFFu), di,
                       (unsigned)sf.AddrPC.Offset, bn, si->Name, (unsigned)disp);
            } else {
                HLOG_W("DIAG(VR-FIX3): %s T%04x #%d: 0x%08X %s", label, (unsigned)(tid & 0xFFFFu), di,
                       (unsigned)sf.AddrPC.Offset, bn);
            }
        }
    }
    CloseHandle(h);
}

/* W145: Test-Export fuer die Mock-Runtime (SB_VR_MOCK_BLOCK=n, "Compositor
 * wartet auf das naechste Spielbild"): Zahl der Spiel-Swaps, nur lesend. */
unsigned sb_test_frame_count(void)
{
    return hooks_frame_count();
}

static DWORD WINAPI watchdog(LPVOID unused)
{
    int waited = 0;
    (void)unused;
    for (;;) {
        Sleep(5000);
        waited += 5;
        InterlockedExchange(&g_wd_waited, waited); /* W186 */
        HLOG_I("watchdog: %.0fs, Frames=%u", (double)waited, hooks_frame_count());
        threadwatch(waited);
        { extern void sb_vr_watch(void); sb_vr_watch(); } /* W145: VR-Runtime-Haenger (vr_enable=0: sofort zurueck) */
        /* M2c: Stall-Forensik (read-only, kein SetThreadContext!). */
        {
            static unsigned last_f; static int stall_n;
            unsigned f = hooks_frame_count();
            extern volatile LONG g_sb_paused; /* W188 GAME-MENU: Pause ist kein Haenger */
            if (f == last_f && waited > 10 && g_sb_paused) {
                HLOG_I("watchdog: PAUSE (Frames=%u, Spiel angehalten - kein STALL)", f);
            } else if (f == last_f && waited > 10) {
                extern int krnl_xb_thread_count(void);
                extern int krnl_xb_thread_get(int, void **, unsigned *);
                int i, n = krnl_xb_thread_count();
                int loadpause; /* W32: bekannte Lade-Pause statt STALL */
                stall_n++;
                loadpause = g_cfg.stall_loadpause && stall_n <= 2 &&
                    w32_main_in_audiopoll();
                if (loadpause) {
                    HLOG_I("watchdog: LOAD-PAUSE #%d (Frames=%u seit 5s+, Main in Audio-Poll 0x10D93E)",
                           stall_n, f);
                } else {
                    HLOG_W("watchdog: STALL #%d (Frames=%u seit 5s+)", stall_n, f);
                }
                if (!loadpause && stall_n <= 2) {
                    extern long sb_render_device_reason(void);
                    HLOG_W("watchdog: STALL devreason=0x%lX",
                           sb_render_device_reason());
                    krnl_lock_diag(); /* DIAG(SUSPSAFE): Sperren-Owner beim STALL */
                    wd_holder_stacks(); /* W145 DIAG(LOADLOCK): Stack fremder Halter (VR/Takt) */
                }
                if (!loadpause && stall_n <= 6) {
                    for (i = 0; i < n; i++) {
                        void *h = NULL; unsigned rt = 0;
                        CONTEXT cx;
                        if (!krnl_xb_thread_get(i, &h, &rt) || !h) continue;
                        if (SuspendThread((HANDLE)h) == (DWORD)-1) continue;
                        memset(&cx, 0, sizeof cx);
                        cx.ContextFlags = CONTEXT_CONTROL;
                        if (GetThreadContext((HANDLE)h, &cx)) {
                            MEMORY_BASIC_INFORMATION smbi;
                            char smod[MAX_PATH];
                            smod[0] = 0;
                            if (VirtualQuery((void *)(uintptr_t)cx.Eip,
                                             &smbi, sizeof smbi)) {
                                GetModuleFileNameA((HMODULE)smbi.AllocationBase,
                                                   smod, sizeof smod);
                            }
                            { extern unsigned g_tex_last_data, g_tex_last_size; HLOG_W("watchdog: TEXLAST data=0x%X size=0x%X", g_tex_last_data, g_tex_last_size); } HLOG_W("watchdog: STALL thr%d rt=0x%X EIP=0x%08X ESP=0x%08X (%s)",
                                   i, rt, cx.Eip, cx.Esp, smod);
                            /* M2c: Call-Stack (nur STALL #1-2, selten = sicher). */
                            if (stall_n <= 2) {
                                STACKFRAME64 sf;
                                int di;
                                wd_sym_init(); /* W145: gemeinsam mit dem Halter-Stack */
                                memset(&sf, 0, sizeof sf);
                                sf.AddrPC.Offset = cx.Eip;
                                sf.AddrPC.Mode = AddrModeFlat;
                                sf.AddrFrame.Offset = cx.Ebp;
                                sf.AddrFrame.Mode = AddrModeFlat;
                                sf.AddrStack.Offset = cx.Esp;
                                sf.AddrStack.Mode = AddrModeFlat;
                                for (di = 0; di < 14; di++) {
                                    BYTE sbuf[sizeof(SYMBOL_INFO) + 64];
                                    SYMBOL_INFO *si = (SYMBOL_INFO *)sbuf;
                                    DWORD64 disp = 0;
                                    MEMORY_BASIC_INFORMATION fmbi;
                                    char fmod[MAX_PATH];
                                    const char *bn = "?";
                                    if (!StackWalk64(IMAGE_FILE_MACHINE_I386,
                                                     GetCurrentProcess(),
                                                     (HANDLE)h, &sf, &cx, NULL,
                                                     SymFunctionTableAccess64,
                                                     SymGetModuleBase64, NULL)) {
                                        break;
                                    }
                                    if (!sf.AddrPC.Offset) { break; }
                                    fmod[0] = 0;
                                    if (VirtualQuery((void *)(uintptr_t)sf.AddrPC.Offset,
                                                     &fmbi, sizeof fmbi)) {
                                        GetModuleFileNameA((HMODULE)fmbi.AllocationBase,
                                                           fmod, sizeof fmod);
                                        bn = strrchr(fmod, 0x5C);
                                        bn = bn ? bn + 1 : fmod;
                                    }
                                    si->SizeOfStruct = sizeof(SYMBOL_INFO);
                                    si->MaxNameLen = 64;
                                    if (SymFromAddr(GetCurrentProcess(),
                                                    sf.AddrPC.Offset, &disp, si)) {
                                        HLOG_W("watchdog: STALL thr%d #%d: 0x%08X %s!%s+0x%X",
                                               i, di, (unsigned)sf.AddrPC.Offset,
                                               bn, si->Name, (unsigned)disp);
                                    } else {
                                        HLOG_W("watchdog: STALL thr%d #%d: 0x%08X %s",
                                               i, di, (unsigned)sf.AddrPC.Offset, bn);
                                    }
                                }
                            }
                        }
                        ResumeThread((HANDLE)h);
                    }
                }
            } else { last_f = f; stall_n = 0; }
        }
        {
            unsigned rh, rlen, rov, rms;
            static unsigned last_r, last_x;
            unsigned r, x;
            if (hooks_read_hang(&rh, &rlen, &rov, &rms) && rms > 3000) {
                HLOG_W("watchdog: READ-HANG h=0x%X len=%u ov=0x%X seit %ums",
                       rh, rlen, rov, rms);
            }
            hooks_activity(&r, &x);
            HLOG_I("watchdog: Aktivitaet/5s: reads=%u xin=%u logdrop=%ld",
                   r - last_r, x - last_x, hlog_dropped());
            { /* W32 STALL20: I/O-Fortschritt + Seek-Drossel pro Fenster */
                static long last_sn, last_ss;
                static unsigned last_rb;
                static unsigned long last_tid[8];
                static long last_tsk[8];
                long sn, ss;
                unsigned rb = hooks_activity_rbytes();
                unsigned long tids[8];
                long sks[8];
                int i, nt;
                krnl_seek_stats(&sn, &ss);
                HLOG_I("watchdog: IOFort/5s: seek=%ld skip=%ld rKB=%u",
                       sn - last_sn, ss - last_ss, (rb - last_rb) / 1024u);
                last_sn = sn; last_ss = ss; last_rb = rb;
                nt = krnl_seek_tids(tids, sks, 8);
                for (i = 0; i < nt; i++) {
                    int j;
                    for (j = 0; j < 8; j++)
                        if (last_tid[j] == tids[i])
                            break;
                    if (j < 8) {
                        if (sks[i] - last_tsk[j] > 0)
                            HLOG_I("watchdog: seek-skip/5s: tid=0x%lX skips=%ld",
                                   tids[i], sks[i] - last_tsk[j]);
                        last_tsk[j] = sks[i];
                    } else {
                        for (j = 0; j < 8; j++)
                            if (!last_tid[j])
                                break;
                        if (j < 8) {
                            last_tid[j] = tids[i];
                            last_tsk[j] = sks[i];
                            if (sks[i] > 0)
                                HLOG_I("watchdog: seek-skip/5s: tid=0x%lX skips=%ld (neu)",
                                       tids[i], sks[i]);
                        }
                    }
                }
            }
            last_r = r;
            last_x = x;
            {
                unsigned used, total;
                hooks_d3d_put(&used, &total);
                HLOG_I("watchdog: D3D-put=%u/%u KB", used / 1024,
                       total / 1024);
            }
        }
        if (g_run_seconds > 0 && waited >= g_run_seconds) {
            HLOG_I("watchdog: --run-seconds erreicht, Exit(0)");
            sb_rem_shutdown(); /* W103: index.json/VRAM-Zeile */
            krnl_dump_stats();
            hlog_close();
            ExitProcess(0);
        }
    }
    /* unreachable */
}

/* W186 RESET-TITLE: Rest eines Testlaufs (--run-seconds) in Sekunden,
 * -1 = ohne Limit (Spielerbetrieb). */
int emu_run_seconds_left(void)
{
    if (g_run_seconds <= 0) {
        return -1;
    }
    return g_run_seconds - (int)g_wd_waited;
}

/* W186: geordnetes Ende wie beim --run-seconds-Ende (index.json, Statistik,
 * Log schliessen); ExitProcess macht der Aufrufer. */
void emu_shutdown_for_exit(void)
{
    sb_rem_shutdown();
    krnl_dump_stats();
    hlog_close();
}


/* Mini-Argparser auf GetCommandLineA (werden per DLL-Einstieg gerufen). */
/* W159: --set-Kette aus der Kommandozeile (Launcher-Start: vr_enable=...).
 * Wird nach allen cfg-Dateien + SB_CFG_SET gelegt (hcfg_apply_set). */
static char g_cmdline_set[8192];

static void parse_cmdline(const char **cfg_path, const char **xbe_path)
{
    static char argv0[512], a1[512], a2[512];
    const char *c = GetCommandLineA();
    /* Programmname ueberspringen (ggf. in Anfuehrungszeichen) */
    if (*c == '"') {
        c++;
        while (*c && *c != '"') {
            c++;
        }
        if (*c) {
            c++;
        }
    } else {
        while (*c && *c != ' ' && *c != '\t') {
            c++;
        }
    }
    while (*c == ' ' || *c == '\t') {
        c++;
    }
    /* Rest: --cfg X --xbe Y --run-seconds N --set "k=v;..." */
    while (*c) {
        const char *key = c;
        char *dst = NULL;
        size_t dstcap = 512;
        while (*c && *c != ' ' && *c != '\t') {
            c++;
        }
        {
            size_t klen = (size_t)(c - key);
            while (*c == ' ' || *c == '\t') {
                c++;
            }
            if (klen == 5 && memcmp(key, "--cfg", 5) == 0) {
                dst = a1;
                *cfg_path = a1;
            } else if (klen == 5 && memcmp(key, "--xbe", 5) == 0) {
                dst = a2;
                *xbe_path = a2;
            } else if (klen == 13 && memcmp(key, "--run-seconds", 13) == 0) {
                g_run_seconds = atoi(c);
                while (*c && *c != ' ' && *c != '\t') {
                    c++;
                }
                continue;
            } else if ((klen == 5 && memcmp(key, "--set", 5) == 0) ||
                       (klen == 4 && memcmp(key, "-set", 4) == 0)) {
                /* W159: naechstes Token (ggf. "...") an die --set-Kette. */
                dst = g_cmdline_set + strlen(g_cmdline_set);
                dstcap = sizeof g_cmdline_set - strlen(g_cmdline_set);
                if (g_cmdline_set[0] && dstcap > 1) {
                    *dst++ = ';';
                    *dst = '\0';
                    dstcap--;
                }
            } else if ((klen > 6 && memcmp(key, "--set=", 6) == 0) ||
                       (klen > 5 && memcmp(key, "-set=", 5) == 0)) {
                /* W159: --set=k=v (Wert im selben Token). */
                const char *v = key;
                size_t n = klen;
                while (n && *v != '=') {
                    v++;
                    n--;
                }
                if (n) {
                    size_t have, room;
                    v++;
                    n--;
                    if (n >= 2 && v[0] == '"' && v[n - 1] == '"') {
                        v++;
                        n -= 2;
                    }
                    have = strlen(g_cmdline_set);
                    room = (have + 1 < sizeof g_cmdline_set)
                               ? sizeof g_cmdline_set - have - 1
                               : 0;
                    if (room > 0 && g_cmdline_set[0]) {
                        g_cmdline_set[have++] = ';';
                        g_cmdline_set[have] = '\0';
                        room--;
                    }
                    if (n > room) {
                        n = room; /* Kette laenger als 8K: Rest faellt weg */
                    }
                    if (n > 0) {
                        memcpy(g_cmdline_set + have, v, n);
                        g_cmdline_set[have + n] = '\0';
                    }
                }
                continue;
            }
            if (dst) {
                size_t i = 0;
                if (*c == '"') {
                    c++;
                    while (*c && *c != '"' && dstcap > 0 && i + 1 < dstcap) {
                        dst[i++] = *c++;
                    }
                    if (*c) {
                        c++;
                    }
                } else {
                    while (*c && *c != ' ' && *c != '\t' && dstcap > 0 && i + 1 < dstcap) {
                        dst[i++] = *c++;
                    }
                }
                dst[i] = '\0';
            }
        }
        (void)argv0;
    }
}

/* W150: Startfehler fuer den Spieler (Abbild/Version). Testlaeufe
 * (--run-seconds) und unsichtbare Fenster zeigen nichts an (Fokus-Regel),
 * die Log-Zeile steht immer im Log. */
static void emu_user_error(const char *key, const char *detail, UINT codepage)
{
    wchar_t w[512], d[600], t[64];
    if (g_run_seconds > 0 || g_cfg.window_hidden) {
        return;
    }
    if (sb_lang_w(key, w, 512) != 0) {
        w[0] = 0;
    }
    MultiByteToWideChar(codepage, 0, detail ? detail : "", -1, d, 600);
    if (sb_lang_w("msg.start_title", t, 64) != 0) {
        t[0] = 0;
    }
    {
        wchar_t all[1200];
        _snwprintf_s(all, 1200, _TRUNCATE, L"%s\n\n%s", w, d);
        MessageBoxW(NULL, all, t, MB_OK | MB_ICONERROR);
    }
}

/* W142: fehlender Sprachschluessel (sollte nie passieren: lang_check). */
static void emu_lang_warn(const char *key)
{
    HLOG_W("LANG: Schluessel fehlt: %s", key ? key : "?");
}

/* W229 US-PROFILE: Fehlerzeile vor exit 8 (SBA ohne Tabelleneintrag). */
static void sba_fatal_log(const char *msg)
{
    HLOG_E("%s", msg);
    hlog_close();
}

/* W229: US-Profil -> Erstbytes aller Profileintraege im ungepatchten Abbild pruefen;
 * bei Abweichung klarer Abbruch (exit 8). EU: keine Aktion, keine Logzeile. */
static void sb_profile_check_or_exit(void)
{
    char pm[200];
    int bad;
    if (!g_sba_us) {
        return;
    }
    g_sba_fatal = sba_fatal_log;
    bad = sb_profile_verify(pm, sizeof pm);
    if (bad) {
        HLOG_E("%s - %d von %u Eintraegen weichen ab, ABBRUCH (exit 8)", pm, bad, sb_profile_check_entries());
        hlog_close();
        ExitProcess(8);
    }
    HLOG_I("PROFILE: %s aktiv (%u .text-Eintraege, %u Erstbyte-Pruefungen ok, sonst +0x740)",
           sb_profile_name(), sb_profile_text_entries(), sb_profile_check_entries());
}

/* W230 US-PLAY: Die US-Fassung hat dieselbe Title-ID (43430002) wie EU, ihre
 * Spielstaende (TDATA/UDATA) und der Datei-Cache (ZCACHE) duerfen sich nie mit
 * EU mischen. Eigener Ordner: [game] hdd_root_us, sonst <hdd_root>_US
 * (Geschwisterordner, gilt auch fuer SB_HDD_ROOT/Datenordner). EU unveraendert. */
static void emu_hdd_root_us(void)
{
    char d[sizeof g_cfg.hdd_root];
    if (g_cfg.hdd_root_us[0]) {
        strcpy_s(d, sizeof d, g_cfg.hdd_root_us);
    } else {
        size_t n;
        strcpy_s(d, sizeof d, g_cfg.hdd_root);
        n = strlen(d);
        while (n > 1 && (d[n - 1] == '/' || d[n - 1] == '\\')) {
            d[--n] = 0;
        }
        strcat_s(d, sizeof d, "_US");
    }
    strcpy_s(g_cfg.hdd_root, sizeof g_cfg.hdd_root, d);
    HLOG_I("PROFILE: US-Spielstaende getrennt von EU: hdd_root=%s", g_cfg.hdd_root);
}

__declspec(dllexport) void __stdcall sb_emulate(void)
{
    const char *cfg_path = "sb_native.cfg";
    const char *xbe_path = NULL;
    static char log_path[600];
    xbe_image_t img;
    sb_gamever_t gv;
    HANDLE wd;
    MEMORY_BASIC_INFORMATION mbi;

    /* Loader-Region pruefen: 0x10000 muss belegt (unser Image) sein */
    if (!VirtualQuery((void *)0x10000, &mbi, sizeof mbi) || mbi.State == MEM_FREE) {
        OutputDebugStringA("sb_emu: ohne Loader gestartet!\n");
        ExitProcess(8);
    }

    sb_relaunch_boot(); /* W186: Neustart-Kind wartet hier auf das Ende des alten Prozesses */
    hcfg_defaults();
    parse_cmdline(&cfg_path, &xbe_path);
    if (sb_relaunch_run_seconds() >= 0) { /* W186: Testlauf-Rest erben */
        g_run_seconds = sb_relaunch_run_seconds();
    }

    /* Logs/Shots pro Lauf in hcfg_out_dir() (SB_OUT_DIR, sonst "out"). */
    _snprintf_s(log_path, sizeof log_path, _TRUNCATE, "%s/sb_harness.log",
                hcfg_out_dir());
    /* W94 CRASHLOG: vorheriges Log behalten (Absturz-Log ging beim
     * Neustart verloren). Nur beim ersten Oeffnen, best effort. */
    if (!sb_relaunch_child()) { /* W186: Neustart haengt an (Log des alten Prozesses bleibt davor) */
        char prev[620];
        _snprintf_s(prev, sizeof prev, _TRUNCATE, "%s/sb_harness.prev.log",
                    hcfg_out_dir());
        MoveFileExA(log_path, prev, MOVEFILE_REPLACE_EXISTING);
    }
    strcpy_s(g_log_path, sizeof g_log_path, log_path);
    if (sb_relaunch_child()) { hlog_init_append(log_path, HLOG_INFO); }
    else { hlog_init(log_path, HLOG_INFO); }
    HLOG_I("sb_emu: Harness startet (DLL=0x%p)", GetModuleHandleA("sb_emu.dll"));
    hcfg_load(cfg_path);
    /* W159: Start-Parameter --set "k=v;..." (Launcher: vr_enable je Startweg).
     * Gewinnt ueber sb_native.cfg, user.cfg und SB_CFG_SET; die Dateien
     * bleiben unveraendert (Einzelstart-Override). */
    hcfg_apply_set(g_cmdline_set, "cmdline");
    sb_relaunch_after_cfg(); /* W186: nur im Neustart-Kind (Zeile RELAUNCH + Ersatz-Set) */
    sb_rem_fx_start(); /* W151 EFFECT-PACK: Effect-Pack-Parameter auf cfg legen */
    if (sb_userdata_path(SB_UD_CRASH, g_crash_dir, sizeof g_crash_dir) != 1) { /* W150 */
        g_crash_dir[0] = 0;
    }
    { /* W142: Spielertexte (lang/ neben sb_ldr.exe, Code aus [ui] lang) */
        char exe[1024], *s, ldir[1150];
        if (GetModuleFileNameA(NULL, exe, sizeof exe) && (s = strrchr(exe, '\\')) != NULL) {
            *s = '\0';
            _snprintf_s(ldir, sizeof ldir, _TRUNCATE, "%s/lang", exe);
            sb_lang_init(ldir, g_cfg.lang);
        } else {
            sb_lang_init(NULL, g_cfg.lang);
        }
        sb_lang_set_warn(emu_lang_warn);
    }
    /* Fenster + D3D11 HIER (pristiner Loader-Thread): NICHT auf Xbox-Spiel-Threads
     * dort haengt CreateWindowExA (Deadlock in System-Code), drum erzeugt+pumpt — der UI-Thread (frisch wie der Loader) das Fenster
     * (A6 ui_thread; 0 = alter Pfad: Loader erzeugt, Present+Pump-Thread pumpen).
     * Der Spiel-Thread presentet spaeter nur noch. Der D3D11-Immediate-
     * Context ist NICHT threadsicher -> Zugriffe per ctx_lock (s.
     * d3d11_render.c) serialisieren; ID3D11Device_* ist threadsicher. */
    sb_render_init();
    if (xbe_path) {
        strcpy_s(g_cfg.xbe_path, sizeof g_cfg.xbe_path, xbe_path);
    }
    hlog_close();
    /* A6 hlog_keep: zweites Oeffnen NICHT trunkieren (cfg-Zeilen bleiben). */
    if (g_cfg.hlog_keep || sb_relaunch_child()) { hlog_init_append(log_path, g_cfg.level); }
    else { hlog_init(log_path, g_cfg.level); }
    if (g_cfg.hlog_async) { hlog_async_start(); } /* PERF(PERF-TICK,W168) */

    AddVectoredExceptionHandler(1, veh);
    SetUnhandledExceptionFilter(last_chance);
    /* W150 ISO-DIRECT: game_image hat Vorrang vor media_root/xbe_path/--xbe. */
    if (g_cfg.game_image[0]) {
        char err[320];
        unsigned char *xb = NULL;
        unsigned xn = 0;
        if (krnl_vfs_open(g_cfg.game_image, err, sizeof err) != 0) {
            HLOG_E("ISO: Abbild '%s' nicht nutzbar: %s", g_cfg.game_image, err);
            emu_user_error("msg.image_bad", err, CP_ACP);
            ExitProcess(4);
        }
        if (krnl_vfs_read_all("default.xbe", &xb, &xn) != 0) {
            HLOG_E("ISO: default.xbe fehlt im Abbild '%s'", g_cfg.game_image);
            emu_user_error("msg.image_bad", "default.xbe", CP_ACP);
            ExitProcess(4);
        }
        HLOG_I("ISO: default.xbe aus dem Abbild (%u B; xbe_path/media_root ignoriert)", xn);
        memset(&img, 0, sizeof img);
        img.file = xb;
        img.file_size = xn;
    } else if (xbe_read(g_cfg.xbe_path, &img) != 0) {
        ExitProcess(1);
    }
    { /* W150: Spielversion (SHA-256 + Zertifikat); Patch-Adressen gelten nur fuer PAL */
        int gv_ok = sb_gamever_check(img.file, img.file_size, &gv);
        if (gv_ok && gv.ntsc_u && !g_cfg.ntsc_u) { /* W230: Notbremse [expert] ntsc_u=0 */
            HLOG_W("GAMEVER: US-Fassung per [expert] ntsc_u=0 abgeschaltet");
            gv.supported = 0;
            gv_ok = 0;
        }
        if (gv_ok && gv.ntsc_u) { /* W229/W230 US-PROFILE: experimentell unterstuetzt */
            HLOG_W("%s - Start mit US-Profil (experimentell)", gv.text);
            sb_profile_select_us();
            emu_hdd_root_us();
        } else if (!gv_ok && !g_cfg.version_check) { /* PO: Experten-Ausweg wie W150 (version_check=0) */
            HLOG_W("%s - version_check=0: Start trotzdem (falsche Patch-Adressen moeglich)", gv.text);
        } else if (!gv_ok) {
            char detail[320], tid[16];
            sb_feedback_note_gamever(&gv);
            _snprintf_s(tid, sizeof tid, _TRUNCATE, "%08X", gv.title_id);
            _snprintf_s(detail, sizeof detail, _TRUNCATE,
                        sb_lang("cfg.image_version"), tid,
                        sb_gamever_region_name(gv.region), gv.version);
            HLOG_E("%s - Start abgebrochen (nur bekanntes PAL-Profil)", gv.text);
            HLOG_E("%s", detail);
            HLOG_E("GAMEVER: unsupported version; exit=5 (before XBE parse/map/hooks)");
            emu_user_error("msg.gamever_bad", detail, CP_UTF8);
            hlog_close();
            ExitProcess(5);
        }
    }
    { /* W202: reject unknown bytes before following XBE section/TLS pointers. */
        char source[1400];
        if (g_cfg.game_image[0]) {
            _snprintf_s(source, sizeof source, _TRUNCATE, "%s/default.xbe", g_cfg.game_image);
        } else {
            strcpy_s(source, sizeof source, g_cfg.xbe_path);
        }
        if (xbe_parse_buf(img.file, img.file_size, source, &img) != 0) ExitProcess(1);
    }
    if (g_cfg.iso_selftest) { /* DIAG(ISO-DIRECT,W150) */
        krnl_vfs_selftest();
    }
    /* Preserve the PAL success log order; only validation moved before parsing. */
    sb_feedback_note_gamever(&gv);
    HLOG_I("%s", gv.text);
    if (xbe_map(&img) != 0) {
        ExitProcess(1);
    }
    /* FIX(HDRSWAP,W94): PE+XBE-Hybrid-Header statt Tausch je CreateThread. */
    if (g_cfg.hdr_hybrid) {
        xbe_hdr_hybrid(&img);
    } else {
        HLOG_I("HDRSWAP: hdr_hybrid=0 -> alter Header-Tausch je CreateThread");
    }
    /* Dateipuffer NICHT freigeben: XeLoadSection braucht ihn (M1). */

    krnl_init(&img);
    if (g_cfg.savelock_selftest) { /* DIAG(SAVE-LOCK,W190) */
        krnl_savelock_selftest();
    }
    watch_dr_init(); /* W41 WATCHDR: cfg parsen + DR bewaffnen (nur bei watch=...) */
    sb_profile_check_or_exit(); /* W229: US-Erstbytes vor Thunks/Hooks pruefen; EU: nichts */
    krnl_install_thunks(&img);
    hooks_install();
    fs_emu_patch_all();
    /* BINKPATCH-AUS (Bink-Completion noetig: TEXMOVIE/effect.efp-Load
     * wartet auf Bink-Textur. Patch -> Deadlock. Original -> langsam). */
#if 0
    {
        static const unsigned bc[] = { 0x241E80, 0x242210, 0x242715, 0x242CF0,
            0x243350, 0x243540, 0x243560, 0x243660, 0x2436B0, 0x243900,
            0x244840, 0x244880, 0x244A20, 0x2456B0, 0x245810, 0x245880,
            0x24DB9F, 0x25269A, 0x253A17, 0x253A1C, 0x253A3E, 0x253D24,
            0x253D7A, 0x253D86, 0x253F63, 0x253FD2, 0x2557A4 };
        unsigned i, np = 0;
        DWORD old;
        for (i = 0; i < sizeof bc / sizeof bc[0]; i++) {
            unsigned char *p = (unsigned char *)(uintptr_t)bc[i];
            __try {
                VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &old);
                p[0] = 0xB8; p[1] = 1; p[2] = 0; p[3] = 0; p[4] = 0;
                p[5] = 0xC3;
                VirtualProtect(p, 6, old, &old);
                np++;
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        HLOG_I("BINKPATCH: %u/27 Einstiege -> mov eax,1+ret", np);
    }
#endif

    if (g_cfg.hdr_race_test > 0) { /* DIAG(HDRSWAP,W94) */
        HANDLE rt = krnl_CreateThread(hdr_race_thread, NULL, 0, NULL);
        if (rt) CloseHandle(rt);
    }
    wd = krnl_CreateThread(watchdog, NULL, 0, NULL);
    if (wd) {
        CloseHandle(wd);
    } else {
        HLOG_E("watchdog-Thread nicht erstellbar (GLE=%lu)", GetLastError());
    }

    HLOG_I("== Entry 0x%X wird aufgerufen ==", xbe_entry(&img));
    ((void(__stdcall *)(void))(uintptr_t)xbe_entry(&img))();
    HLOG_I("== Entry kehrte zurueck (Xbox-Threads laufen weiter) ==");
    /* NICHT sofort beenden: der Watchdog beendet bei --run-seconds. Ohne
     * Limit laufen wir ewig (Abbruch via Strg+C / Task-Manager). */
    if (g_run_seconds > 0) {
        Sleep((DWORD)g_run_seconds * 1000u + 3000u); /* Watchdog zuerst */
    } else {
        for (;;) {
            Sleep(1000);
        }
    }
    krnl_dump_stats();
    hlog_close();
    ExitProcess(0);
}
