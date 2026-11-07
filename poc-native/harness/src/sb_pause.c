/* sb_pause.c - W188 GAME-MENU: Spielpause. Beschreibung s. sb_pause.h. */
#include <windows.h>
#include <stdint.h>
#include "hcfg.h"
#include "hlog.h"
#include "sb_pause.h"

extern unsigned g_present_no; /* d3d11_render.c */

volatile LONG g_sb_paused;
static volatile LONG g_pause_req;
static unsigned g_pause_n;
static unsigned g_after_n;            /* Presents seit dem letzten Fortsetzen (Beleg-Zeilen +1/+30) */
static unsigned g_after_tmr0, g_after_fld0;

/* Missions-Countdown [0x3563F0] (u32, -1 je Missions-Durchlauf; diag_states STA_TMR_ADDR) */
static unsigned pause_tmr(void)
{
    unsigned v = 0;
    __try { v = *(volatile unsigned *)(uintptr_t)0x3563F0u; }
    __except (EXCEPTION_EXECUTE_HANDLER) { v = 0; }
    return v;
}

void sb_pause_request(int on, const char *why)
{
    LONG v = on ? 1 : 0;
    if (InterlockedExchange(&g_pause_req, v) != v) {
        HLOG_I("PAUSE: Wunsch %s (%s, f=%u)", v ? "an" : "aus", why ? why : "", g_present_no);
    }
}

int sb_pause_requested(void)
{
    return g_pause_req != 0;
}

unsigned sb_pause_count(void)
{
    return g_pause_n;
}

void sb_pause_point(void)
{
    LARGE_INTEGER qf, q0, q1, next, now;
    unsigned nfr = 0, tmr0, fld0, f0;
    LONGLONG per;
    if (!g_pause_req) {
        if (g_after_n) { /* Beleg: Countdown/Feldzaehler laufen nach dem Fortsetzen ohne Sprung weiter */
            g_after_n++;
            if (g_after_n == 2u || g_after_n == 31u) {
                HLOG_I("PAUSE nachher: +%u Bilder f=%u feld=%u (+%d) tmr=%u (%+d)", g_after_n - 1u, g_present_no,
                       sb_timing_field(), (int)(sb_timing_field() - g_after_fld0), pause_tmr(),
                       (int)(pause_tmr() - g_after_tmr0));
            }
            if (g_after_n > 31u) { g_after_n = 0; }
        }
        return;
    }
    QueryPerformanceFrequency(&qf);
    QueryPerformanceCounter(&q0);
    tmr0 = pause_tmr();
    fld0 = sb_timing_field();
    f0 = g_present_no;
    sb_timing_pause_begin();
    krnl_time_pause(1);
    InterlockedExchange(&g_sb_paused, 1); /* Mischer stumm, Takt-Thread ruht (sb_pd_on) */
    g_pause_n++;
    if (!sb_render_pause_begin()) {
        HLOG_W("PAUSE: Bild nicht gesichert (kein Device) - Pause ohne Bild");
    }
    HLOG_I("PAUSE: an #%u (f=%u feld=%u tmr=%u vblank_hz=%d interp_fps=%d vr=%d)", g_pause_n, f0, fld0, tmr0,
           g_cfg.vblank_hz, g_cfg.interp_fps, g_cfg.vr_enable);
    per = qf.QuadPart / 60; /* Pausenbilder mit 60 Hz */
    next = q0;
    while (g_pause_req) {
        MSG msg;
        while (!g_cfg.ui_thread && PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { /* alter Pfad: hier pumpen */
            if (msg.message == WM_QUIT) {
                hlog_close();
                ExitProcess(0);
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        sb_ovl_pause_tick();
        if (!g_pause_req) {
            break;
        }
        sb_render_pause_frame();
        nfr++;
        next.QuadPart += per;
        QueryPerformanceCounter(&now);
        if (now.QuadPart - next.QuadPart > per * 6) { /* lange haengen geblieben: nicht nachholen */
            next = now;
        }
        while (now.QuadPart < next.QuadPart) {
            LONGLONG ms = (next.QuadPart - now.QuadPart) * 1000 / qf.QuadPart;
            Sleep(ms > 1 ? (DWORD)(ms - 1) : 0);
            QueryPerformanceCounter(&now);
        }
    }
    sb_render_pause_end();
    InterlockedExchange(&g_sb_paused, 0);
    krnl_time_pause(0);
    sb_timing_pause_end();
    QueryPerformanceCounter(&q1);
    HLOG_I("PAUSE: aus #%u (dauer %.0f ms, %u Pausenbilder, f=%u feld=%u (vorher %u) tmr=%u (vorher %u))", g_pause_n,
           (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)qf.QuadPart, nfr, g_present_no, sb_timing_field(),
           fld0, pause_tmr(), tmr0);
    g_after_n = 1;
    g_after_tmr0 = pause_tmr();
    g_after_fld0 = sb_timing_field();
}
