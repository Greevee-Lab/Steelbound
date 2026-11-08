/* W196: real capture/message implementation, simulated focus only.
 * Physical cursor APIs are spies: even an accidental call fails the test. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "hcfg.h"
static int physical, overlay;
static HCURSOR cursor_value;
static HWND foreground = (HWND)(INT_PTR)1;
unsigned test_mode, test_scene, test_loop;
unsigned imouse_test_state(unsigned addr)
{
    return addr == 0x2EEA34u ? test_mode : addr == 0x6B96DCu ? test_scene : test_loop;
}
static HWND WINAPI test_foreground(void) { return foreground; }
static BOOL WINAPI test_clip(const RECT *r) { (void)r; physical++; return TRUE; }
static HCURSOR WINAPI test_cursor(HCURSOR c) { physical++; cursor_value = c; return c; }
static HWND WINAPI test_capture(HWND w) { physical++; return w; }
static BOOL WINAPI test_release(void) { physical++; return TRUE; }
static LRESULT WINAPI test_old(WNDPROC p, HWND w, UINT m, WPARAM a, LPARAM b)
{ (void)p; (void)w; (void)m; (void)a; (void)b; return 0; }
#define GetForegroundWindow test_foreground
#define ClipCursor test_clip
#define SetCursor test_cursor
#define SetCapture test_capture
#define ReleaseCapture test_release
#define CallWindowProcA test_old
#define IMOUSE_SELFTEST
#include "input_mouse.c"
hcfg_t g_cfg;
unsigned g_frames;
int sb_ovl_open(void) { return overlay; }
int sb_ovl_wndproc(HWND w, UINT m, WPARAM a, LPARAM b) { return 0; }
int sb_phud_wndproc(HWND w, UINT m, WPARAM a, LPARAM b) { return 0; }
int sb_vr_wndproc(unsigned m, unsigned a, long b) { return 0; }
void hlog_msg(int level, const char *fmt, ...)
{
    (void)level;
    if (strncmp(fmt, "MOUSE-MENU:", 11) == 0) {
        va_list ap;
        va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); putchar('\n');
    }
}

int imouse_selftest(void)
{
    int fail = 0, i, dx, dy, blocked;
    const char *pages[] = { "ESC", "Einstellungen", "Feedback", "Quit", "Pause", "Foto-Panel" };
#define VERIFY(c, name) do { if (!(c)) { printf("W196 FAIL: %s\n", name); fail++; } } while (0)
    g_mwnd = (HWND)(INT_PTR)1;
    g_cfg.mouse_capture = g_cfg.input_test_hook = 1;
    g_cfg.input_source = 1;
    g_test_focus = g_focus = 1;
    g_installed = 1;
    test_mode = 2; test_scene = 0; test_loop = 0;
    imouse_capture_on(g_mwnd, "title");
    VERIFY(!g_captured, "Titel frei/sichtbar");
    imouse_poll(&dx, &dy, &blocked);
    VERIFY(!blocked && !dx && !dy, "Titel: Maustasten erlaubt, keine Zieldeltas");
    test_mode = 3;
    for (i = 0; i < 0x2C; i++) {
        test_scene = i;
        imouse_capture_on(g_mwnd, "menu");
        VERIFY(!g_captured, "Spielmenues frei/sichtbar");
    }
    test_mode = 2; test_scene = 0x2C; test_loop = 1;
    imouse_capture_on(g_mwnd, "demo");
    VERIFY(!g_captured, "Titel-Demo frei");
    test_mode = 4;
    VERIFY(!g_captured, "Missionsbeginn wartet auf Klick");
    imouse_wndproc(g_mwnd, WM_LBUTTONDOWN, 0, 0);
    VERIFY(g_captured, "Mission Klick gefangen/unsichtbar");
    for (i = 0; i < sizeof pages / sizeof pages[0]; i++) {
        overlay = 1;
        imouse_wndproc(g_mwnd, WM_TIMER, IMOUSE_TIMER_ID, 0);
        imouse_menu_capture(1); /* redundant page change must retain restore */
        imouse_focus_gain(g_mwnd, "focus");
        imouse_wndproc(g_mwnd, WM_KEYDOWN, VK_F10, 0);
        VERIFY(!g_captured, pages[i]);
        VERIFY(imouse_wndproc(g_mwnd, WM_SETCURSOR, 0, HTCLIENT) == TRUE, "Cursor handled");
        overlay = 0;
        imouse_menu_capture(0);
        VERIFY(g_captured, "Schliessen stellt Capture wieder her");
        printf("W196 %s: frei sichtbar -> geschlossen gefangen unsichtbar (simuliert)\n", pages[i]);
    }
    overlay = 1; imouse_menu_capture(1);
    test_scene = 0x30; test_loop = 0;
    overlay = 0; imouse_menu_capture(0);
    VERIFY(!g_captured, "Missionsende waehrend Menue");
    g_cfg.mouse_capture_outside_mission = 1;
    imouse_capture_on(g_mwnd, "compat");
    VERIFY(g_captured, "Opt-in ausserhalb Mission");
    g_cfg.mouse_capture_outside_mission = 0;
    imouse_wndproc(g_mwnd, WM_TIMER, IMOUSE_TIMER_ID, 0);
    VERIFY(!g_captured, "Mission verlassen gibt frei");
    imouse_menu_capture(1); imouse_menu_capture(0);
    VERIFY(!g_captured, "Vorher freie Maus bleibt frei");
    test_mode = 4; test_scene = 0x2C; test_loop = 1;
    /* W203: no previous capture, including a previous F10 release. */
    g_want = 0;
    overlay = 1; imouse_menu_capture(1);
    VERIFY(!g_captured, "W203 focused menu remains free");
    overlay = 0; imouse_menu_capture(0);
    VERIFY(g_captured && g_want, "W203 focus + option restores without prior capture");
    for (i = 0; i < 3; i++) {
        g_want = 1;
        imouse_capture_on(g_mwnd, "W203 independent captured start");
        printf("W203 %s: mission -> menu -> return (test focus)\n",
               i == 0 ? "ESC/ESC" : i == 1 ? "settings/back/close" : "photo/back/close");
        VERIFY(imouse_wndproc(g_mwnd, IMOUSE_MSG_TEST_STATE, 0, 0), "W203 initially captured");
        overlay = 1; imouse_menu_capture(1);
        imouse_menu_capture(1); /* page/back, including redundant open */
        VERIFY(!imouse_wndproc(g_mwnd, IMOUSE_MSG_TEST_STATE, 0, 0), "W203 menu free");
        /* Close notification before the aggregate flag clears. */
        imouse_menu_capture(0);
        VERIFY(!g_captured && g_menu_restore, "W203 blocked close retains request");
        overlay = 0; test_loop = 0;
        imouse_wndproc(g_mwnd, WM_TIMER, IMOUSE_TIMER_ID, 0);
        VERIFY(!g_captured && g_menu_restore, "W203 resume waits for mission loop");
        test_loop = 1;
        imouse_wndproc(g_mwnd, WM_TIMER, IMOUSE_TIMER_ID, 0);
        VERIFY(imouse_wndproc(g_mwnd, IMOUSE_MSG_TEST_STATE, 0, 0), "W203 recaptured after resume");
        imouse_wndproc(g_mwnd, WM_LBUTTONUP, 0, 0);
        imouse_poll(&dx, &dy, &blocked); /* release capture-click suppression */
        imouse_wndproc(g_mwnd, IMOUSE_MSG_TEST_MOVE, 7, -3);
        imouse_poll(&dx, &dy, &blocked);
        VERIFY(dx == 7 && dy == -3 && !blocked, "W203 aiming deltas reach poll after close");
    }
    /* An abandoned request must not capture in the next mission. */
    overlay = 1; imouse_menu_capture(1);
    overlay = 0; test_loop = 0; imouse_menu_capture(0);
    test_mode = 3;
    imouse_wndproc(g_mwnd, WM_TIMER, IMOUSE_TIMER_ID, 0);
    test_mode = 4; test_loop = 1;
    imouse_wndproc(g_mwnd, WM_TIMER, IMOUSE_TIMER_ID, 0);
    VERIFY(!g_captured && !g_menu_restore, "W203 mission exit cancels pending restore");
    g_cfg.mouse_capture = 0;
    overlay = 1; imouse_menu_capture(1);
    overlay = 0; imouse_menu_capture(0);
    VERIFY(!g_captured, "W203 capture option off stays free");
    g_cfg.mouse_capture = 1;
    /* Reopen while resume is pending, then close again (rapid double ESC). */
    overlay = 1; imouse_menu_capture(1);
    overlay = 0; test_loop = 0; imouse_menu_capture(0);
    overlay = 1; imouse_menu_capture(1); imouse_menu_capture(1);
    VERIFY(!g_captured && g_menu_had_cap, "W203 reopen preserves pending restore");
    test_loop = 1; overlay = 0; imouse_menu_capture(0);
    VERIFY(g_captured && !g_menu_restore, "W203 repeated close restores once");
    /* Focus loss during a deferred resume cancels it without OS effects. */
    overlay = 1; imouse_menu_capture(1);
    overlay = 0; test_loop = 0; imouse_menu_capture(0);
    imouse_wndproc(g_mwnd, IMOUSE_MSG_TEST_FOCUS, 0, 0);
    foreground = NULL;
    test_loop = 1;
    imouse_wndproc(g_mwnd, WM_TIMER, IMOUSE_TIMER_ID, 0);
    VERIFY(!g_captured && !g_menu_restore, "W203 deferred restore cancelled on focus loss");
    imouse_wndproc(g_mwnd, IMOUSE_MSG_TEST_FOCUS, 1, 0);
    foreground = g_mwnd;
    imouse_capture_on(g_mwnd, "focus-loss");
    overlay = 1; imouse_menu_capture(1);
    imouse_focus_lose("test");
    overlay = 0; imouse_menu_capture(0);
    VERIFY(!g_captured, "Schliessen ohne Fokus faengt nicht");
    VERIFY(physical == 0, "Keine physischen Cursor-APIs im Test-Fokus");
    printf("W196 Titel/Pilot/Missionswahl/Briefing: frei sichtbar; physische Aufrufe=%d; Fehler=%d\n", physical, fail);
    /* Verify the actual WM_SETCURSOR choice with API spies, no OS effects. */
    g_focus = 1;
    imouse_wndproc(g_mwnd, WM_SETCURSOR, 0, HTCLIENT);
    VERIFY(physical == 1 && cursor_value != NULL, "Freier Vordergrund setzt Pfeil");
    g_captured = 1;
    imouse_wndproc(g_mwnd, WM_SETCURSOR, 0, HTCLIENT);
    VERIFY(physical == 2 && cursor_value == NULL, "Gefangener Vordergrund verbirgt Pfeil");
    printf("W196 WM_SETCURSOR: Pfeil/NULL mit API-Spies geprueft; Fehler=%d\n", fail);
    return fail;
}
