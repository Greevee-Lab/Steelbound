/* W196: real capture/message implementation, simulated focus only.
 * Physical cursor APIs are spies: even an accidental call fails the test. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "hcfg.h"
static int physical, overlay, minimized;
static HCURSOR cursor_value;
static HWND foreground = (HWND)(INT_PTR)1;
unsigned test_mode, test_scene, test_loop, test_demo;
unsigned imouse_test_state(unsigned addr)
{
    return addr == 0x2EEA34u ? test_mode : addr == 0x6B96DCu ? test_scene :
        addr == 0x3B8CDCu ? test_demo : test_loop;
}
/* W225: PostMessage spy (game-thread sync request), no real window. */
static UINT posted_msg;
static int posted_n;
static BOOL WINAPI test_post(HWND w, UINT m, WPARAM a, LPARAM b)
{ (void)w; (void)a; (void)b; posted_msg = m; posted_n++; return TRUE; }
#define PostMessageA test_post
static char last_mouse[512]; /* last "MOUSE: soll=" diagnostic line */
static HWND WINAPI test_foreground(void) { return foreground; }
static BOOL WINAPI test_clip(const RECT *r) { (void)r; physical++; return TRUE; }
static HCURSOR WINAPI test_cursor(HCURSOR c) { physical++; cursor_value = c; return c; }
static HWND WINAPI test_capture(HWND w) { physical++; return w; }
static BOOL WINAPI test_release(void) { physical++; return TRUE; }
static LRESULT WINAPI test_old(WNDPROC p, HWND w, UINT m, WPARAM a, LPARAM b)
{ (void)p; (void)w; (void)m; (void)a; (void)b; return 0; }
static int registrations, bad_registration;
static BOOL WINAPI test_register(PCRAWINPUTDEVICE r, UINT n, UINT size)
{
    registrations++;
    if (n != 1 || size != sizeof *r || r->dwFlags != 0 ||
        r->usUsagePage != 1 || r->usUsage != 2 || !r->hwndTarget)
        bad_registration++;
    return TRUE;
}
static UINT WINAPI test_raw(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT hdr)
{
    RAWINPUT *r = (RAWINPUT *)data;
    (void)h; (void)cmd; (void)hdr;
    memset(r, 0, sizeof *r);
    r->header.dwType = RIM_TYPEMOUSE;
    r->data.mouse.lLastX = 7; r->data.mouse.lLastY = -3;
    *size = sizeof *r;
    return sizeof *r;
}
#define RegisterRawInputDevices test_register
#define GetRawInputData test_raw
static BOOL WINAPI test_rect(HWND w, LPRECT r)
{ (void)w; r->left = r->top = 0; r->right = 640; r->bottom = 480; return TRUE; }
static BOOL WINAPI test_screen(HWND w, LPPOINT p) { (void)w; (void)p; return TRUE; }
static BOOL WINAPI test_iconic(HWND w) { (void)w; return minimized; }
#define GetClientRect test_rect
#define ClientToScreen test_screen
#define IsIconic test_iconic
#define GetForegroundWindow test_foreground
#define ClipCursor test_clip
#define SetCursor test_cursor
#define SetCapture test_capture
#define ReleaseCapture test_release
#define CallWindowProcW test_old
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
    if (strncmp(fmt, "MOUSE-FOCUS:", 12) == 0) {
        va_list ap;
        va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); putchar('\n');
    }
    if (strncmp(fmt, "MOUSE: soll=", 12) == 0) {
        va_list ap;
        va_start(ap, fmt); vsnprintf(last_mouse, sizeof last_mouse, fmt, ap); va_end(ap);
        printf("  %s\n", last_mouse);
    }
}

/* All events use the production subclass. Page names exercise the shared
 * aggregate overlay gate; no rendered page or real desktop is involved. */
static void event(UINT msg, WPARAM wp)
{
    imouse_wndproc(g_mwnd, msg, wp, 0);
}
static int check_state(const char *step, int capture)
{
    int dx, dy, blocked, bad;
    event(IMOUSE_MSG_TEST_STATE, 0);
    imouse_poll(&dx, &dy, &blocked); /* clear button suppression */
    event(WM_INPUT, 0);
    imouse_poll(&dx, &dy, &blocked);
    bad = (g_captured != capture || dx != (capture ? 7 : 0) ||
        dy != (capture ? -3 : 0) || (capture && blocked));
    printf("W208 %s: capture=%d sichtbar=%d raw=%d,%d %s\n",
        step, (int)g_captured, !g_captured, dx, dy, bad ? "FAIL" : "OK");
    return bad;
}
static void menu(int open)
{
    overlay = open;
    imouse_menu_capture(open);
}
int imouse_selftest(void)
{
    int fail = 0, page, path, activation, order, before, i;
    const char *pages[] = { "ESC", "Foto", "Feedback", "Einstellungen", "Quit", "Pause" };
#define VERIFY(c, name) do { if (!(c)) { printf("W208 FAIL: %s\n", name); fail++; } } while (0)
    g_mwnd = (HWND)(INT_PTR)1;
    g_cfg.mouse_capture = g_cfg.input_test_hook = g_cfg.input_focus_only = 1;
    g_cfg.input_source = 1; g_installed = 1;
    test_mode = 4; test_scene = 0x2C; test_loop = 1;
    event(IMOUSE_MSG_TEST_FOCUS, 1);
    fail += check_state("initial focus", 1);
    /* Cross product: all menu/focus paths, activation styles, and focus
     * message orders. Click activation is checked BEFORE the click arrives. */
    for (page = 0; page < sizeof pages / sizeof pages[0]; page++)
    for (path = 0; path < 4; path++)
    for (activation = 0; activation < 3; activation++)
    for (order = 0; order < 2; order++) {
        printf("W208 sequence page=%s path=%d activation=%s order=%d\n", pages[page], path,
            activation == 0 ? "Alt-Tab" : activation == 1 ? "Taskleiste" : "Klick", order);
        menu(0); g_want = 1; event(IMOUSE_MSG_TEST_FOCUS, 1);
        fail += check_state("start", 1);
        if (path != 1) { menu(1); fail += check_state("open", 0); }
        if (path != 0) {
            event(order ? WM_KILLFOCUS : WM_ACTIVATE, order ? 0 : WA_INACTIVE);
            fail += check_state("loss first", 0);
            event(order ? WM_ACTIVATE : WM_KILLFOCUS, order ? WA_INACTIVE : 0);
            event(WM_ACTIVATEAPP, 0);
            fail += check_state("loss duplicate", 0);
            if (path == 3) { menu(0); fail += check_state("close unfocused", 0); }
            before = registrations;
            event(order ? WM_SETFOCUS : WM_ACTIVATE,
                order ? 0 : activation == 2 ? WA_CLICKACTIVE : WA_ACTIVE);
            fail += check_state("gain first", path == 2 ? 0 : 1);
            event(order ? WM_ACTIVATE : WM_SETFOCUS,
                order ? (activation == 2 ? WA_CLICKACTIVE : WA_ACTIVE) : 0);
            fail += check_state("gain duplicate", path == 2 ? 0 : 1);
            VERIFY(registrations == before + 1, "Raw Input registered once on focus return");
        }
        if (path == 0 || path == 2) { menu(0); fail += check_state("close focused", 1); }
        if (activation == 2) {
            event(WM_LBUTTONDOWN, 0); event(WM_LBUTTONUP, 0);
            imsg_clear(); fail += check_state("activation click", 1);
        }
    }
    /* Delayed mission resume and stale close while aggregate overlay is open. */
    menu(1); imouse_menu_capture(0);
    fail += check_state("stale close still open", 0);
    overlay = 0; test_loop = 0; event(WM_TIMER, IMOUSE_TIMER_ID);
    fail += check_state("resume loop missing", 0);
    event(IMOUSE_MSG_TEST_FOCUS, 0);
    test_loop = 1; event(IMOUSE_MSG_TEST_FOCUS, 1);
    fail += check_state("resume after focus hook", 1);
    menu(1); overlay = 0; test_loop = 0; imouse_menu_capture(0);
    event(WM_KILLFOCUS, 0); event(WM_SETFOCUS, 0);
    fail += check_state("return before loop", 0);
    test_loop = 1; event(WM_TIMER, IMOUSE_TIMER_ID);
    fail += check_state("timer restores delayed mission", 1);
    menu(1); menu(0); menu(1); menu(1); menu(0);
    fail += check_state("rapid reopen", 1);
    /* Manual release must survive both menu and focus transitions. */
    event(WM_KEYDOWN, VK_F10);
    fail += check_state("F10 release", 0);
    menu(1); menu(0); fail += check_state("manual release survives focused menu", 0);
    menu(1); event(WM_KILLFOCUS, 0); menu(0); event(WM_SETFOCUS, 0);
    fail += check_state("manual release survives menu and focus", 0);
    event(WM_KEYDOWN, VK_F10); fail += check_state("F10 recapture", 1);
    event(WM_KEYDOWN, VK_F10); event(WM_LBUTTONDOWN, 0); event(WM_LBUTTONUP, 0);
    imsg_clear(); fail += check_state("click overrides manual release", 1);
    menu(1); event(WM_KEYDOWN, VK_F10);
    fail += check_state("F10 release inside menu", 0);
    menu(0); fail += check_state("F10 menu preference persists", 0);
    event(WM_KEYDOWN, VK_F10);
    imouse_wndproc(g_mwnd, WM_KEYDOWN, VK_F10, 0x40000000);
    fail += check_state("F10 repeat ignored", 1);
    event(IMOUSE_MSG_TEST_MOVE, 11);
    { int dx, dy, blocked; imouse_poll(&dx, &dy, &blocked);
      VERIFY(dx == 11 && dy == 0 && !blocked, "test movement hook"); }
    g_cfg.mouse_capture = 0; event(WM_TIMER, IMOUSE_TIMER_ID);
    fail += check_state("option off", 0);
    g_cfg.mouse_capture = 1; event(WM_TIMER, IMOUSE_TIMER_ID);
    fail += check_state("option on", 1);
    test_mode = 3;
    for (i = 0; i < 0x2C; i++) {
        test_scene = i; event(WM_TIMER, IMOUSE_TIMER_ID);
        VERIFY(!g_captured, "outside mission free");
    }
    test_mode = 2; test_scene = 0x2C; test_demo = 1;
    fail += check_state("title demo", 0);
    g_cfg.mouse_capture_outside_mission = 1;
    event(WM_TIMER, IMOUSE_TIMER_ID); fail += check_state("explicit outside opt-in", 1);
    menu(1); fail += check_state("opt-in still respects menu", 0);
    menu(0); g_cfg.mouse_capture_outside_mission = 0;
    event(WM_TIMER, IMOUSE_TIMER_ID); fail += check_state("default outside free", 0);
    test_mode = 4; test_demo = 0; event(WM_TIMER, IMOUSE_TIMER_ID);
    fail += check_state("mission entry automatic", 1);
    minimized = 1; event(WM_ACTIVATE, MAKEWPARAM(WA_INACTIVE, 1));
    fail += check_state("minimized", 0);
    minimized = 0; event(WM_ACTIVATE, WA_ACTIVE);
    fail += check_state("restored", 1);
    test_demo = 0;
    { extern int imouse_w225_selftest(void); fail += imouse_w225_selftest(); }
    VERIFY(!bad_registration, "foreground-only Raw Input registration");
    VERIFY(physical == 0, "test focus never invokes physical cursor APIs");
    printf("W208 simulated physical calls=%d\n", physical);
    /* Actual foreground branch with cursor API spies (no real cursor changes). */
    g_test_focus = 0; g_want = 0;
    imouse_wndproc(g_mwnd, WM_SETCURSOR, 0, HTCLIENT);
    VERIFY(cursor_value != NULL, "free foreground arrow");
    g_want = 1;
    imouse_wndproc(g_mwnd, WM_SETCURSOR, 0, HTCLIENT);
    VERIFY(cursor_value == NULL, "captured foreground invisible");
    event(WM_KILLFOCUS, 0);
    VERIFY(cursor_value != NULL, "loss restores arrow");
    event(WM_SETFOCUS, 0);
    VERIFY(cursor_value == NULL, "focus gain hides without WM_SETCURSOR or click");
    cursor_value = (HCURSOR)(INT_PTR)1;
    event(WM_TIMER, IMOUSE_TIMER_ID);
    VERIFY(cursor_value == NULL, "timer repairs visible cursor while captured");
    before = physical; foreground = NULL;
    event(WM_KILLFOCUS, 0); event(WM_SETFOCUS, 0);
    VERIFY(physical == before, "no physical capture without foreground");
    printf("W208 MOUSE-FOCUS: Fehler=%d\n", fail);
    return fail;
}

/* ---- W225 MOUSE-ARM: real user flow from the alpha.5 log ----
 * Title -> START without title demo -> menus -> mission at load counter 2
 * (not 4!), ESC/settings/feedback/photo/welcome, Alt-Tab, F10, both mouse
 * modes, title demo, mission 2 (counter 5), F10 outside the mission. */
static int w225_reason(const char *step, int capture, const char *why)
{
    int bad = check_state(step, capture);
    char want[64];
    snprintf(want, sizeof want, "soll=%s grund=%s ", capture ? "gefangen" : "frei", why);
    if (!strstr(last_mouse, want)) {
        printf("W225 FAIL %s: erwartet '%s', Diagnose '%s'\n", step, want, last_mouse);
        bad++;
    }
    return bad;
}
static void w225_set(unsigned counter, unsigned scene, unsigned loop, unsigned demo)
{
    test_mode = counter; test_scene = scene; test_loop = loop; test_demo = demo;
}
/* Game-thread poll sees the new state; UI thread applies it on the posted
 * sync message only (no other window message in between). */
static int w225_poll_sync(const char *step, int capture)
{
    int dx, dy, blocked, n0 = posted_n, bad = 0;
    imouse_poll(&dx, &dy, &blocked);
    if (posted_n != n0 + 1 || posted_msg != IMOUSE_MSG_SYNC) {
        printf("W225 FAIL %s: kein Abgleich vom Spiel-Thread angefordert\n", step);
        bad++;
    }
    imouse_poll(&dx, &dy, &blocked);
    if (posted_n != n0 + 1) {
        printf("W225 FAIL %s: Abgleich doppelt angefordert\n", step);
        bad++;
    }
    imouse_wndproc(g_mwnd, IMOUSE_MSG_SYNC, 0, 0);
    if (g_captured != capture || g_sync_pending) {
        printf("W225 FAIL %s: nach Abgleich capture=%d\n", step, (int)g_captured);
        bad++;
    }
    printf("W225 %s: capture=%d %s\n", step, (int)g_captured, bad ? "FAIL" : "OK");
    return bad;
}
static int w225_mission_block(const char *mode)
{
    int fail = 0, i;
    const char *pages[] = { "ESC", "Einstellungen", "Feedback", "Foto", "Willkommen" };
    char name[96];
    printf("W225 Mission (%s)\n", mode);
    fail += w225_reason("mission (ladezaehler 2)", 1, "mission");
    if (!strstr(last_mouse, g_cfg.mouse_mode ? "modus=absolut" : "modus=relativ")) {
        printf("W225 FAIL modus in Diagnose: %s\n", last_mouse); fail++;
    }
    for (i = 0; i < 5; i++) {
        snprintf(name, sizeof name, "%s auf", pages[i]); menu(1);
        fail += w225_reason(name, 0, "menue");
        snprintf(name, sizeof name, "%s zu", pages[i]); menu(0);
        fail += w225_reason(name, 1, "mission");
    }
    /* Alt-Tab away and back (real message order WM_ACTIVATE, WM_KILLFOCUS). */
    event(WM_ACTIVATE, WA_INACTIVE); event(WM_KILLFOCUS, 0); event(WM_ACTIVATEAPP, 0);
    fail += w225_reason("Alt-Tab weg", 0, "kein_fokus");
    event(WM_ACTIVATE, WA_ACTIVE); event(WM_SETFOCUS, 0);
    fail += w225_reason("Alt-Tab zurueck", 1, "mission");
    /* Alt-Tab away, menu opened meanwhile (controller pause), back, close. */
    event(WM_KILLFOCUS, 0); menu(1); event(WM_SETFOCUS, 0);
    fail += w225_reason("zurueck ins offene Menue", 0, "menue");
    menu(0); fail += w225_reason("Menue zu nach Rueckkehr", 1, "mission");
    event(WM_KEYDOWN, VK_F10); fail += w225_reason("F10 frei", 0, "f10_frei");
    event(WM_KEYDOWN, VK_F10); fail += w225_reason("F10 fangen", 1, "mission");
    event(IMOUSE_MSG_TEST_MOVE, 5);
    { int dx, dy, blocked; imouse_poll(&dx, &dy, &blocked);
      if (dx != 5 || blocked) { printf("W225 FAIL Zielhebel-Delta %d\n", dx); fail++; } }
    return fail;
}
int imouse_w225_selftest(void)
{
    int fail = 0, mode, c;
    for (mode = 0; mode < 2; mode++) {
        g_cfg.mouse_mode = mode;
        printf("W225 Nutzerablauf mouse_mode=%s\n", mode ? "absolute" : "relative");
        menu(0); g_want = 1; g_force = 0;
        event(IMOUSE_MSG_TEST_FOCUS, 1);
        w225_set(1, 0, 0, 0);
        event(WM_TIMER, IMOUSE_TIMER_ID);
        fail += w225_reason("Titel", 0, "ausserhalb_mission");
        menu(1); fail += w225_reason("Willkommen im Titel", 0, "menue");
        menu(0); fail += w225_reason("Willkommen zu", 0, "ausserhalb_mission");
        event(WM_KEYDOWN, VK_F10); fail += w225_reason("F10 im Titel", 1, "f10_manuell");
        event(WM_KEYDOWN, VK_F10); fail += w225_reason("F10 im Titel zurueck", 0, "ausserhalb_mission");
        event(WM_LBUTTONDOWN, 0); event(WM_LBUTTONUP, 0); imsg_clear();
        fail += w225_reason("Klick im Spielmenue", 0, "ausserhalb_mission");
        w225_set(1, 0x1, 0, 0); event(WM_TIMER, IMOUSE_TIMER_ID);
        w225_set(1, 0xC, 0, 0); event(WM_TIMER, IMOUSE_TIMER_ID);
        fail += w225_reason("Spielmenue", 0, "ausserhalb_mission");
        w225_set(2, 0x2B, 0, 0); event(WM_TIMER, IMOUSE_TIMER_ID);
        fail += w225_reason("Missions-Load", 0, "ausserhalb_mission");
        /* Mission at load counter 2 (old code: never captured). Applied by
         * the per-frame game-thread check without any other window message. */
        w225_set(2, 0x2C, 1, 0);
        fail += w225_poll_sync("Missionsbeginn per Frame-Abgleich", 1);
        fail += w225_mission_block(mode ? "absolut" : "relativ");
        w225_set(2, 0x2C, 0, 0);
        fail += w225_poll_sync("Missionsende per Frame-Abgleich", 0);
    }
    g_cfg.mouse_mode = 0;
    /* Title demo stays free at any counter; campaign missions capture at
     * any counter (mission 2 = 5 after mission 1, wrap 1..23). */
    for (c = 1; c <= 23; c++) {
        w225_set(c, 0x2C, 1, 1); event(WM_TIMER, IMOUSE_TIMER_ID);
        if (g_captured) { printf("W225 FAIL Titel-Demo ladezaehler=%d gefangen\n", c); fail++; }
        w225_set(c, 0x2C, 1, 0); event(WM_TIMER, IMOUSE_TIMER_ID);
        if (!g_captured) { printf("W225 FAIL Mission ladezaehler=%d frei\n", c); fail++; }
    }
    w225_set(2, 0x2C, 1, 1); fail += w225_reason("Titel-Demo", 0, "ausserhalb_mission");
    w225_set(5, 0x2C, 1, 0); fail += w225_reason("Mission 2 (ladezaehler 5)", 1, "mission");
    /* Manual F10 capture in a game menu ends at the next mission edge. */
    w225_set(5, 0x30, 0, 0); fail += w225_reason("Nachbereitung", 0, "ausserhalb_mission");
    event(WM_KEYDOWN, VK_F10); fail += w225_reason("F10 Nachbereitung", 1, "f10_manuell");
    w225_set(6, 0x2C, 1, 0); fail += w225_reason("F10 -> Mission 3", 1, "mission");
    w225_set(6, 0x2C, 0, 0); fail += w225_reason("nach Mission 3 frei", 0, "ausserhalb_mission");
    event(WM_KEYDOWN, VK_F10); menu(1);
    fail += w225_reason("F10 manuell, dann Menue", 0, "menue");
    menu(0); fail += w225_reason("Menue zu, manuell bleibt", 1, "f10_manuell");
    event(WM_KILLFOCUS, 0); fail += w225_reason("manuell ohne Fokus", 0, "kein_fokus");
    event(WM_SETFOCUS, 0); fail += w225_reason("manuell Fokus zurueck", 1, "f10_manuell");
    event(WM_KEYDOWN, VK_F10); fail += w225_reason("manuell aus", 0, "ausserhalb_mission");
    w225_set(4, 0x2C, 1, 0); event(WM_TIMER, IMOUSE_TIMER_ID);
    printf("W225 MOUSE-ARM: Fehler=%d\n", fail);
    return fail;
}
