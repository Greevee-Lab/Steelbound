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
static unsigned g_movie_trace_left, g_movie_trace_frame;
static volatile LONG g_movie_presented_frame; /* W205: diagnostic snapshot after Present */

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
    if (g_cfg.audio_sync_diag)
        InterlockedExchange(&g_movie_presented_frame, (LONG)sb_pause_movie_frame());
    if (g_cfg.pause_diag && !g_pause_req && g_movie_trace_left) {
        unsigned frame = sb_pause_movie_frame();
        if (g_after_n == 1u || (frame && frame != g_movie_trace_frame)) {
            sb_pause_movie_log("nachher");
            g_movie_trace_frame = frame;
            --g_movie_trace_left;
        }
    }
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
    sb_tsc_pause(1);
    sb_timing_pause_begin();
    krnl_time_pause(1);
    InterlockedExchange(&g_sb_paused, 1); /* Mischer stumm, Takt-Thread ruht (sb_pd_on) */
    g_pause_n++;
    if (g_cfg.pause_diag) {
        sb_pause_movie_log("vorher");
        g_movie_trace_frame = sb_pause_movie_frame();
        g_movie_trace_left = g_movie_trace_frame ? 90u : 0u;
    }
    if (!sb_render_pause_begin()) {
        HLOG_W("PAUSE: Bild nicht gesichert (kein Device) - Pause ohne Bild");
    }
    HLOG_I("PAUSE: an #%u (f=%u feld=%u tmr=%u vblank_hz=%d interp_fps=%d vr=%d)", g_pause_n, f0, fld0, tmr0,
           g_cfg.vblank_hz, g_cfg.interp_fps, g_cfg.vr_enable);
    per = qf.QuadPart / sb_ovl_pause_fps();
    next = q0;
    while (g_pause_req) {
        MSG msg;
        while (!g_cfg.ui_thread && PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { /* alter Pfad: hier pumpen */
            if (msg.message == WM_QUIT) {
                hlog_close();
                ExitProcess(0);
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        sb_ovl_pause_tick();
        if (!g_pause_req) {
            break;
        }
        /* W200: the selected rate takes effect on the next pause frame. */
        per = qf.QuadPart / sb_ovl_pause_fps();
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
    if (g_cfg.pause_diag) { sb_pause_movie_log("angehalten"); }
    sb_render_pause_end();
    sb_ovl_present_end(); /* W200: commit before clocks/present thread resume */
    sb_tsc_pause(0);
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

/* W192: lesender Beleg fuer den Film-Player 0x46C20. BINK: Width/Height/
 * Frames/FrameNum/LastFrameNum, Pointer wird beim Schliessen genullt. */
unsigned sb_pause_movie_frame(void)
{
    unsigned frame = 0;
    __try {
        const volatile unsigned *b = *(const volatile unsigned *volatile *)(uintptr_t)0x3B8AD0u;
        if (*(volatile unsigned *)(uintptr_t)0x35642Cu == 2u && b && b[0] && b[1] && b[3] <= b[2]) {
            frame = b[3];
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return frame;
}

unsigned sb_pause_movie_presented_frame(void)
{
    return (unsigned)InterlockedCompareExchange(&g_movie_presented_frame, 0, 0);
}

void sb_pause_movie_log(const char *where)
{
    unsigned frame = sb_pause_movie_frame();
    if (frame) {
        HLOG_I("BINK PAUSE: %s f=%u movie_frame=%u tsc=%llu", where, g_present_no, frame, sb_tsc_now());
    }
}
