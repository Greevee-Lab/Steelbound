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
static int test_loc_mission; /* W341: Stub fuer loc_pad.c */
int loc_mouse_mission(void) { return test_loc_mission; }
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
#define SB_STATE_SELFTEST /* W232: sb_in_mission liest ueber imouse_test_state */
#include "sb_state.c"
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
    { extern int sb_state_w232_selftest(void); fail += sb_state_w232_selftest(); }
    { extern int imouse_w260_selftest(void); fail += imouse_w260_selftest(); }
    { extern int imouse_w299_selftest(void); fail += imouse_w299_selftest(); }
    { extern int imouse_w341_selftest(void); fail += imouse_w341_selftest(); }
    { extern int imouse_w436_selftest(void); fail += imouse_w436_selftest(); }
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

/* W232 STATE-LOADCOUNT: zentrale Missionserkennung sb_in_mission (sb_state.c,
 * hier mit derselben Test-Lesefunktion wie input_mouse.c). Der Ladezaehler
 * [0x2EEA34] darf das Ergebnis nie beeinflussen. */
static int w232_case(const char *name, unsigned counter, unsigned scene, unsigned loop,
                     unsigned demo, int want, int want_or_demo)
{
    int got, god, bad = 0;
    w225_set(counter, scene, loop, demo);
    got = sb_in_mission();
    god = sb_state_mission_or_demo();
    if (got != want || god != want_or_demo || sb_state_loadcount() != counter ||
        sb_state_mission_eval(scene, loop, demo) != want) { bad = 1; }
    printf("W232 %-34s ladezaehler=%-2u szene=0x%02X mloop=%u demo=%u -> mission=%d (soll %d) mission_or_demo=%d %s\n",
           name, counter, scene, loop, demo, got, want, god, bad ? "FAIL" : "OK");
    return bad;
}
int sb_state_w232_selftest(void)
{
    int fail = 0, c;
    fail += w232_case("Logos", 1, 0x00, 0, 0, 0, 0);
    fail += w232_case("Titel-Demo (Nutzerweg, Zaehler 2)", 2, 0x2C, 1, 1, 0, 1);
    fail += w232_case("Titel-Demo 2 (Testkette, Zaehler 3)", 3, 0x2C, 1, 1, 0, 1);
    fail += w232_case("Spielmenue", 3, 0x0C, 0, 0, 0, 0);
    fail += w232_case("Missions-Load", 4, 0x2B, 0, 0, 0, 0);
    fail += w232_case("Mission 1 (Testkette, Zaehler 4)", 4, 0x2C, 1, 0, 1, 1);
    fail += w232_case("Mission 1 (Nutzerweg, Zaehler 2)", 2, 0x2C, 1, 0, 1, 1);
    fail += w232_case("Missionsende (mloop 0)", 4, 0x2C, 0, 0, 0, 0);
    fail += w232_case("Nachbereitung", 4, 0x30, 0, 0, 0, 0);
    fail += w232_case("Mission 2 (Zaehler 5)", 5, 0x2C, 1, 0, 1, 1);
    fail += w232_case("Mission neu geladen (Zaehler 6)", 6, 0x2C, 1, 0, 1, 1);
    /* Wrap 1..23: Kampagne immer Mission, Demo nie. */
    for (c = 1; c <= 23; c++) {
        w225_set((unsigned)c, 0x2C, 1, 0);
        if (!sb_in_mission()) { printf("W232 FAIL Mission ladezaehler=%d\n", c); fail++; }
        w225_set((unsigned)c, 0x2C, 1, 1);
        if (sb_in_mission()) { printf("W232 FAIL Titel-Demo ladezaehler=%d\n", c); fail++; }
    }
    w225_set(4, 0x2C, 1, 0);
    printf("W232 STATE-LOADCOUNT: Fehler=%d\n", fail);
    return fail;
}

/* ---- W260 MOUSE-WHEEL: Rad-Nachrichten der echten Subclass ----
 * Rasten werden nur mit Fokus und ohne Menue angenommen; offenes ESC-Menue
 * laesst das Rad dem Menue (hier: Gatter, im Client nimmt sb_ovl_wndproc
 * die Nachricht vorher); Maus frei (F10) bzw. Fokusverlust verwirft. */
static int w260_pend(int d) { return (int)g_wheel.pend[d]; }
int imouse_w260_selftest(void)
{
    int fail = 0, dx, dy, blocked;
    unsigned m, f = 5000;
#define W260(c, name) do { int ok_ = (c); printf("W260 %-52s %s\n", name, ok_ ? "OK" : "FAIL"); if (!ok_) fail++; } while (0)
    foreground = (HWND)(INT_PTR)1;
    w225_set(4, 0x2C, 1, 0); /* Mission */
    menu(0); g_want = 1; g_force = 0;
    event(IMOUSE_MSG_TEST_FOCUS, 1);
    imap_wheel_clear(&g_wheel);
    imouse_poll(&dx, &dy, &blocked);
    W260(g_captured && !blocked, "Mission gefangen, Maus nicht gesperrt");
    event(WM_MOUSEWHEEL, MAKEWPARAM(0, 120));
    W260(w260_pend(IMAP_WHEEL_UP) == 1 && w260_pend(IMAP_WHEEL_DOWN) == 0, "Raste hoch -> 1x WHEEL_UP");
    event(WM_MOUSEWHEEL, MAKEWPARAM(0, (WORD)(SHORT)-240));
    W260(w260_pend(IMAP_WHEEL_DOWN) == 2 && w260_pend(IMAP_WHEEL_UP) == 1, "2 Rasten runter -> 2x WHEEL_DOWN, hoch getrennt");
    event(WM_MOUSEHWHEEL, MAKEWPARAM(0, 120));
    event(WM_MOUSEHWHEEL, MAKEWPARAM(0, (WORD)(SHORT)-120));
    W260(w260_pend(IMAP_WHEEL_RIGHT) == 1 && w260_pend(IMAP_WHEEL_LEFT) == 1, "Kipprad rechts/links getrennt");
    m = imouse_wheel_mask(f, blocked);
    W260(m == 0xFu, "Impuls aller vier Richtungen im selben Frame");
    W260(imouse_wheel_mask(f, 0) == 0xFu, "gleicher Frame = gleiches Ergebnis");
    W260(imouse_wheel_mask(f + 1, 0) == 0xFu && imouse_wheel_mask(f + 2, 0) == 0 &&
         imouse_wheel_mask(f + 3, 0) == 0, "2 Frames gedrueckt, 2 Frames los");
    W260(imouse_wheel_mask(f + 4, 0) == (1u << IMAP_WHEEL_DOWN) &&
         imouse_wheel_mask(f + 8, 0) == 0, "2. Raste runter als eigener Impuls, dann Ruhe");
    f += 20;
    /* ESC-Menue offen: Rad gehoert dem Menue, nichts angestaut. */
    menu(1);
    event(WM_MOUSEWHEEL, MAKEWPARAM(0, 120));
    event(WM_MOUSEWHEEL, MAKEWPARAM(0, (WORD)(SHORT)-120));
    W260(w260_pend(IMAP_WHEEL_UP) == 0 && w260_pend(IMAP_WHEEL_DOWN) == 0, "ESC-Menue offen: keine Rasten fuers Spiel");
    menu(0);
    W260(imouse_wheel_mask(f, 0) == 0, "nach Menue: kein Nachfeuern");
    f += 10;
    /* Angestaute Rasten beim Menue-Oeffnen verwerfen. */
    event(WM_MOUSEWHEEL, MAKEWPARAM(0, 360));
    menu(1); menu(0);
    W260(w260_pend(IMAP_WHEEL_UP) == 0, "Menue-Oeffnen verwirft angestaute Rasten");
    /* F10: Maus frei in der Mission -> gesperrt, Rasten verworfen. */
    event(WM_KEYDOWN, VK_F10);
    imouse_poll(&dx, &dy, &blocked);
    event(WM_MOUSEWHEEL, MAKEWPARAM(0, 120));
    W260(!g_captured && blocked, "F10: Maus frei, Spiel-Eingabe gesperrt");
    W260(imouse_wheel_mask(f, blocked) == 0 && w260_pend(IMAP_WHEEL_UP) == 0, "F10 frei: Raste verworfen");
    event(WM_KEYDOWN, VK_F10);
    imouse_poll(&dx, &dy, &blocked);
    W260(g_captured && !blocked && imouse_wheel_mask(f + 1, blocked) == 0, "F10 wieder gefangen: kein Nachfeuern");
    f += 10;
    /* Fokusverlust: verwerfen + ohne Fokus nichts annehmen. */
    event(WM_MOUSEWHEEL, MAKEWPARAM(0, 120));
    event(IMOUSE_MSG_TEST_FOCUS, 0);
    W260(w260_pend(IMAP_WHEEL_UP) == 0, "Fokusverlust verwirft Rasten");
    event(WM_MOUSEWHEEL, MAKEWPARAM(0, 120));
    W260(w260_pend(IMAP_WHEEL_UP) == 0, "ohne Fokus keine Rasten");
    event(IMOUSE_MSG_TEST_FOCUS, 1);
    imap_wheel_clear(&g_wheel);
    printf("W260 MOUSE-WHEEL (Fenster): Fehler=%d\n", fail);
#undef W260
    return fail;
}

/* W299 REPLAY: Fokus-Klick (Fenster per Klick aktiviert) geht nicht ans Spiel. Nur Fenster-Nachrichten,
 * keine echte Maus, kein Fokuswechsel. */
int imouse_w299_selftest(void)
{
    int fail = 0, saved_eat = g_cfg.focus_click_eat, saved_cap = g_cfg.mouse_capture;
#define W299(c, name) do { if (!(c)) { printf("W299 FAIL: %s\n", name); fail++; } else { printf("W299 ok: %s\n", name); } } while (0)
    menu(0);
    g_cfg.focus_click_eat = 1;
    g_cfg.mouse_capture = 0; /* reine Nachrichten-Tasten, keine Fang-Logik */
    g_cfg.input_source = 1;
    event(IMOUSE_MSG_TEST_FOCUS, 1);
    imsg_clear();
    g_eat_arm = 0; g_eat_up = 0; g_ma_nc = 0;
    {
        unsigned n0 = g_eat_n;
        imouse_wndproc(g_mwnd, WM_MOUSEACTIVATE, (WPARAM)g_mwnd, MAKELPARAM(HTCLIENT, WM_LBUTTONDOWN));
        event(WM_ACTIVATEAPP, TRUE); event(WM_ACTIVATE, WA_CLICKACTIVE); event(WM_SETFOCUS, 0);
        event(WM_LBUTTONDOWN, MK_LBUTTON);
        W299(!g_kdown[VK_LBUTTON] && g_eat_n == n0 + 1, "Klick-Aktivierung: Druck verschluckt");
        event(WM_LBUTTONUP, 0);
        W299(!g_kdown[VK_LBUTTON] && g_eat_up == 0, "Klick-Aktivierung: Loslassen verschluckt");
        event(WM_LBUTTONDOWN, MK_LBUTTON);
        W299(g_kdown[VK_LBUTTON] && g_eat_n == n0 + 1, "naechster Klick geht ans Spiel");
        event(WM_LBUTTONUP, 0);
        W299(!g_kdown[VK_LBUTTON], "naechster Klick losgelassen");
    }
    /* Tastatur-Aktivierung (Alt-Tab zurueck): erster Klick ist ein echter Klick. */
    event(WM_ACTIVATE, WA_INACTIVE); event(WM_KILLFOCUS, 0);
    event(IMOUSE_MSG_TEST_FOCUS, 1);
    event(WM_ACTIVATE, WA_ACTIVE); event(WM_SETFOCUS, 0);
    event(WM_LBUTTONDOWN, MK_LBUTTON);
    W299(g_kdown[VK_LBUTTON], "Alt-Tab (WA_ACTIVE): erster Klick geht ans Spiel");
    event(WM_LBUTTONUP, 0);
    /* Klick auf Titel/Rahmen aktiviert: kein Client-Klick zu verschlucken. */
    imouse_wndproc(g_mwnd, WM_MOUSEACTIVATE, (WPARAM)g_mwnd, MAKELPARAM(HTCAPTION, WM_LBUTTONDOWN));
    event(WM_ACTIVATE, WA_CLICKACTIVE);
    event(WM_LBUTTONDOWN, MK_LBUTTON);
    W299(g_kdown[VK_LBUTTON], "Aktivierung ueber Titelleiste: Klick danach geht ans Spiel");
    event(WM_LBUTTONUP, 0);
    event(WM_ACTIVATE, WA_CLICKACTIVE);
    event(WM_NCLBUTTONDOWN, HTCAPTION);
    event(WM_LBUTTONDOWN, MK_LBUTTON);
    W299(g_kdown[VK_LBUTTON], "Rahmen-Klick verwirft die Sperre");
    event(WM_LBUTTONUP, 0);
    /* Rechte Taste: eigenes Loslassen, linke bleibt unberuehrt. */
    event(WM_ACTIVATE, WA_CLICKACTIVE);
    event(WM_RBUTTONDOWN, MK_RBUTTON);
    W299(!g_kdown[VK_RBUTTON] && g_eat_up == 2u, "Klick-Aktivierung rechts verschluckt");
    event(WM_LBUTTONDOWN, MK_LBUTTON);
    W299(g_kdown[VK_LBUTTON], "linke Taste danach normal");
    event(WM_LBUTTONUP, 0); event(WM_RBUTTONUP, 0);
    W299(!g_kdown[VK_LBUTTON] && !g_kdown[VK_RBUTTON] && g_eat_up == 0, "beide losgelassen");
    /* Seitentaste X2. */
    event(WM_ACTIVATE, WA_CLICKACTIVE);
    event(WM_XBUTTONDOWN, MAKEWPARAM(0, XBUTTON2));
    W299(!g_kdown[VK_XBUTTON2] && g_eat_up == 16u, "Klick-Aktivierung Seitentaste verschluckt");
    event(WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON2));
    W299(g_eat_up == 0, "Seitentaste losgelassen");
    /* Zeitablauf: kommt der Klick erst spaeter, ist er ein echter Klick. */
    event(WM_ACTIVATE, WA_CLICKACTIVE);
    g_eat_arm -= IMOUSE_EAT_MS + 10u; /* simuliert: Aktivierung liegt > 1 s zurueck */
    event(WM_LBUTTONDOWN, MK_LBUTTON);
    W299(g_kdown[VK_LBUTTON], "Zeitablauf: spaeter Klick geht ans Spiel");
    event(WM_LBUTTONUP, 0);
    /* Fokusverlust verwirft die Sperre. */
    event(WM_ACTIVATE, WA_CLICKACTIVE);
    event(WM_ACTIVATE, WA_INACTIVE);
    event(IMOUSE_MSG_TEST_FOCUS, 1);
    event(WM_LBUTTONDOWN, MK_LBUTTON);
    W299(g_kdown[VK_LBUTTON], "Fokusverlust verwirft die Sperre");
    event(WM_LBUTTONUP, 0);
    /* Schalter aus: wie bisher. */
    g_cfg.focus_click_eat = 0;
    event(WM_ACTIVATE, WA_CLICKACTIVE);
    event(WM_LBUTTONDOWN, MK_LBUTTON);
    W299(g_kdown[VK_LBUTTON], "focus_click_eat=0: Klick-Aktivierung geht ans Spiel (bisher)");
    event(WM_LBUTTONUP, 0);
    imsg_clear();
    g_eat_arm = 0; g_eat_up = 0;
    g_cfg.focus_click_eat = saved_eat;
    g_cfg.mouse_capture = saved_cap;
    printf("W299 Fokus-Klick (Fenster): Fehler=%d\n", fail);
#undef W299
    return fail;
}

/* ---- W341 LOC-MOUSE: Fangen im LoC-Profil ----
 * Vorher las das Soll die SB-Zustandsadressen (im LoC-Profil Null-Seite) -> nie gefangen, Deltas 0.
 * Jetzt: LoC-Mission (loc_mouse_mission, hier Stub) -> gefangen und Raw-Deltas wirken; SB-Merkmale
 * zaehlen im LoC-Profil nicht; loc_mouse=0 = altes Verhalten; Menue/F10 wie in SB. */
int imouse_w341_selftest(void)
{
    extern int g_sba_loc;
    int fail = 0, dx, dy, blocked, save_loc = g_sba_loc;
    menu(0); g_want = 1; g_force = 0;
    g_cfg.loc_mouse = 1;
    event(IMOUSE_MSG_TEST_FOCUS, 1);
    g_sba_loc = 1;
    w225_set(4, 0x2C, 1, 0); /* SB-Mission sichtbar - im LoC-Profil ohne Bedeutung */
    test_loc_mission = 0;
    event(WM_TIMER, IMOUSE_TIMER_ID);
    fail += w225_reason("W341 LoC Karte/Lobby (SB-Merkmal Mission)", 0, "ausserhalb_mission");
    test_loc_mission = 1;
    event(WM_TIMER, IMOUSE_TIMER_ID);
    fail += w225_reason("W341 LoC Mission", 1, "mission");
    imouse_poll(&dx, &dy, &blocked);
    event(IMOUSE_MSG_TEST_MOVE, 0); /* leer */
    imouse_wndproc(g_mwnd, IMOUSE_MSG_TEST_MOVE, (WPARAM)(INT_PTR)12, (LPARAM)(INT_PTR)-5);
    imouse_poll(&dx, &dy, &blocked);
    if (dx != 12 || dy != -5 || blocked) {
        printf("W341 FAIL Deltas in der LoC-Mission dx=%d dy=%d blocked=%d (soll 12,-5,0)\n", dx, dy, blocked);
        fail++;
    } else {
        printf("W341 LoC Mission: Deltas kommen an (dx=%d dy=%d) OK\n", dx, dy);
    }
    menu(1); fail += w225_reason("W341 LoC Mission + eigenes Menue", 0, "menue");
    menu(0); fail += w225_reason("W341 LoC Mission nach Menue", 1, "mission");
    event(WM_KEYDOWN, VK_F10); fail += w225_reason("W341 LoC F10 frei", 0, "f10_frei");
    event(WM_KEYDOWN, VK_F10); fail += w225_reason("W341 LoC F10 zurueck", 1, "mission");
    g_cfg.loc_mouse = 0;
    event(WM_TIMER, IMOUSE_TIMER_ID);
    fail += w225_reason("W341 loc_mouse=0 (altes Verhalten)", 0, "ausserhalb_mission");
    g_cfg.loc_mouse = 1;
    test_loc_mission = 0;
    event(WM_TIMER, IMOUSE_TIMER_ID);
    fail += w225_reason("W341 LoC Missionsende", 0, "ausserhalb_mission");
    g_sba_loc = save_loc;
    event(WM_TIMER, IMOUSE_TIMER_ID);
    fail += w225_reason("W341 SB-Profil unveraendert (Mission)", 1, "mission");
    printf("W341 LOC-MOUSE: Fehler=%d\n", fail);
    return fail;
}

/* W436 LOC-EJECT (GitHub #76): kurzer Druck Umschalt+Entf ganz zwischen zwei Abfragen des Spiels (Down und Up kommen
 * als Fenster-Nachrichten vor dem naechsten Poll, Launcher-Weg input_source=1). Die naechste Abfrage sieht beide
 * Tasten gleichzeitig (Akkord -> Eject), das Folgebild auch noch (IMSG_MIN_FRAMES), danach ist alles los. Ebenso,
 * wenn Umschalt gehalten bleibt und nur Entf kurz getippt wird. Je Spieltick fragt LoC den SBC genau einmal ab
 * (LOCRBI-LAT "mehrere=0"), direkt danach entsteht der eigene Rahmen -> der Druck landet in einem Rahmen. */
int imouse_w436_selftest(void)
{
    int fail = 0, sh, de;
    unsigned f = 5000;
    int save_src = g_cfg.input_source;
    g_cfg.input_source = 1;
    imsg_clear();
    sh = imsg_key(VK_SHIFT, f);
    de = imsg_key(VK_DELETE, f);
    if (sh || de) { printf("W436 FAIL vorher gedrueckt (%d %d)\n", sh, de); fail++; }
    event(WM_KEYDOWN, VK_SHIFT);
    event(WM_KEYDOWN, VK_DELETE);
    event(WM_KEYUP, VK_DELETE);
    event(WM_KEYUP, VK_SHIFT);
    f++;
    sh = imsg_key(VK_SHIFT, f);
    de = imsg_key(VK_DELETE, f);
    if (!sh || !de) { printf("W436 FAIL kurzer Akkord nicht in der naechsten Abfrage (%d %d)\n", sh, de); fail++; }
    sh = imsg_key(VK_SHIFT, f + 1);
    de = imsg_key(VK_DELETE, f + 1);
    if (!sh || !de) { printf("W436 FAIL kurzer Akkord fehlt im Folgebild (%d %d)\n", sh, de); fail++; }
    sh = imsg_key(VK_SHIFT, f + 2);
    de = imsg_key(VK_DELETE, f + 2);
    if (sh || de) { printf("W436 FAIL Akkord klemmt (%d %d)\n", sh, de); fail++; }
    event(WM_KEYDOWN, VK_SHIFT); /* Umschalt gehalten, Entf kurz zwischen zwei Abfragen */
    sh = imsg_key(VK_SHIFT, f + 3);
    event(WM_KEYDOWN, VK_DELETE);
    event(WM_KEYUP, VK_DELETE);
    sh = imsg_key(VK_SHIFT, f + 4);
    de = imsg_key(VK_DELETE, f + 4);
    if (!sh || !de) { printf("W436 FAIL Entf-Tipp bei gehaltenem Umschalt verloren (%d %d)\n", sh, de); fail++; }
    event(WM_KEYUP, VK_SHIFT);
    imsg_clear();
    g_cfg.input_source = save_src;
    printf("W436 LOC-EJECT kurzer Druck (Fenster-Nachrichten): Fehler=%d\n", fail);
    return fail;
}
