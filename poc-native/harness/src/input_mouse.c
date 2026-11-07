/* input_mouse.c - W79 INPUT-REAL: echte Tastatur/Maus im Spielerbetrieb.
 * Beschreibung s. input_mouse.h. Schalter: [input] input_source,
 * mouse_capture, input_focus_only, input_test_hook (hcfg.h). Ohne
 * input_source=1 und mouse_capture=1 wird nichts installiert (bisheriges
 * Verhalten bitgenau). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include "hcfg.h"
#include "hlog.h"
#include "input_mouse.h"
#include "input_tap.h"
#include "sb_overlay.h" /* W118 REM-OVERLAY */
#include "sb_perfhud.h" /* W122 PERF-HUD */
#include "vr.h" /* W134 VR-PROTO */

#define IMOUSE_MSG_INIT (WM_APP + 0x79)
#define IMOUSE_TIMER_ID 0x7901
#define IMSG_MIN_FRAMES 2u /* kurzer Tipp bleibt >= 2 Spiel-Frames sichtbar */

static volatile HWND g_mwnd;          /* eigenes SB_NATIVE-Fenster */
static WNDPROC g_old_proc;
static volatile LONG g_installed;     /* 1 = Subclass gesetzt, -1 = gescheitert */
static volatile LONG g_want = 1;      /* Nutzerwunsch Maus fangen (F10 schaltet) */
static volatile LONG g_captured;      /* 1 = Maus (logisch) gefangen */
static volatile LONG g_suppress_btn;  /* 1 = Maustasten bis Loslassen sperren */
static volatile LONG g_raw_dx, g_raw_dy;
static volatile LONG g_raw_n;         /* Anzahl Raw-Pakete (Beleg/Log) */
static volatile LONG g_focus;         /* logischer Fokus (Nachrichten/Test) */
static volatile LONG g_test_focus;    /* 1 = Fokus per Test-Hook simuliert */
/* W196: UI-thread-owned menu latch; all capture paths share the gate. */
static int g_menu_open, g_menu_had_cap;
static unsigned imouse_state_u32(unsigned addr)
{
#ifdef IMOUSE_SELFTEST
    extern unsigned imouse_test_state(unsigned addr);
    return imouse_test_state(addr);
#else
    __try { return *(volatile unsigned *)(uintptr_t)addr; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
#endif
}
static int imouse_in_mission(void)
{
    /* Same mission markers as the overlay; mode excludes the title demo. */
    return imouse_state_u32(0x2EEA34u) == 4u &&
        imouse_state_u32(0x6B96DCu) == 0x2Cu &&
        imouse_state_u32(0x356F58u) == 1u;
}
static int imouse_allowed(void)
{
    return !g_menu_open && !sb_ovl_open() &&
        (g_cfg.mouse_capture_outside_mission || imouse_in_mission());
}

/* Nachrichten-Tastenzustand (input_source=1). Schreiber: UI-Thread
 * (Fenster-Nachrichten), Leser: Spiel-Thread (XInput-Poll). */
static volatile LONG g_kdown[256];    /* 1 = gedrueckt */
static volatile LONG g_kpend[256];    /* 1 = Druck vom Spiel noch nicht gesehen */
static volatile LONG g_kgen;          /* +1 bei Fokusverlust (Tipps verwerfen) */
static unsigned g_kseen[256];         /* Spiel-Frame des ersten Lesens (Spiel-Thread) */
static LONG g_kseen_gen[256];         /* g_kgen beim Lesen; 0 = ungueltig */

/* Eigenes Fenster suchen: SB_NATIVE-Klasse, gleicher Prozess (andere
 * Client-Instanzen paralleler Laeufe haben dieselbe Klasse). */
static HWND imouse_find_window(void)
{
    HWND h = NULL;
    DWORD me = GetCurrentProcessId();
    if (g_mwnd) return g_mwnd;
    while ((h = FindWindowExA(NULL, h, "SB_NATIVE", NULL)) != NULL) {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid == me) {
            g_mwnd = h;
            return h;
        }
    }
    return NULL;
}

/* Physische Wirkung (ClipCursor/Cursor/SetCapture) NUR, wenn unser Fenster
 * wirklich das Vordergrundfenster ist - ein unsichtbares Testfenster mit
 * simuliertem Fokus darf den Cursor des Nutzers nie einsperren. */
static int imouse_is_fg(HWND w)
{
    return !g_test_focus && w && GetForegroundWindow() == w;
}

int imouse_has_focus(void)
{
    HWND h;
    if (g_installed == 1) return g_focus != 0;
    h = imouse_find_window();
    return h != NULL && GetForegroundWindow() == h;
}

static void imouse_clip(HWND w)
{
    RECT rc;
    POINT a, b;
    if (!imouse_is_fg(w)) return;
    GetClientRect(w, &rc);
    a.x = rc.left;
    a.y = rc.top;
    b.x = rc.right;
    b.y = rc.bottom;
    ClientToScreen(w, &a);
    ClientToScreen(w, &b);
    rc.left = a.x;
    rc.top = a.y;
    rc.right = b.x;
    rc.bottom = b.y;
    ClipCursor(&rc);
}

static void imouse_capture_on(HWND w, const char *why)
{
    if (!g_cfg.mouse_capture || !imouse_allowed() || IsIconic(w)) return;
    imouse_clip(w);
    if (!g_captured) {
        int phys = imouse_is_fg(w);
        InterlockedExchange(&g_raw_dx, 0);
        InterlockedExchange(&g_raw_dy, 0);
        InterlockedExchange(&g_suppress_btn, 1);
        InterlockedExchange(&g_captured, 1);
        if (phys) SetCursor(NULL);
        HLOG_I("INPUT: Maus gefangen (%s; F10 = freigeben)%s", why,
               phys ? "" : " [logisch: Fenster nicht im Vordergrund, kein ClipCursor]");
    }
}

static void imouse_capture_off(const char *why)
{
    if (g_captured) {
        InterlockedExchange(&g_captured, 0);
        if (imouse_is_fg(g_mwnd)) {
            ClipCursor(NULL);
            if (GetCapture() == g_mwnd) ReleaseCapture();
            SetCursor(LoadCursorA(NULL, IDC_ARROW));
        }
        InterlockedExchange(&g_raw_dx, 0);
        InterlockedExchange(&g_raw_dy, 0);
        HLOG_I("INPUT: Maus freigegeben (%s)", why);
    }
}

/* Alle Nachrichten-Tasten loslassen (Fokusverlust: nichts klemmt). */
static void imsg_clear(void)
{
    int i;
    for (i = 0; i < 256; i++) {
        InterlockedExchange(&g_kdown[i], 0);
        InterlockedExchange(&g_kpend[i], 0);
    }
    InterlockedIncrement(&g_kgen);
    itap_clear(); /* W92: angestaute Tipper verwerfen */
    if (imouse_is_fg(g_mwnd) && GetCapture() == g_mwnd) ReleaseCapture();
}

static void imsg_set(int vk, int down)
{
    if (vk <= 0 || vk > 255) return;
    if (down) {
        LONG was = InterlockedExchange(&g_kdown[vk], 1);
        InterlockedExchange(&g_kpend[vk], 1);
        if (g_cfg.key_tap_queue && !was) itap_down_edge(vk); /* W92: nur Flanke */
    } else {
        LONG was = InterlockedExchange(&g_kdown[vk], 0);
        if (g_cfg.key_tap_queue && was) itap_up_edge(vk); /* W92 */
    }
    if (g_cfg.input_diag)
        HLOG_I("INPUT: msg vk=0x%02X %s (fokus=%d%s)", vk,
               down ? "down" : "up", (int)g_focus,
               g_test_focus ? " test" : "");
}

/* Tasten-Nachricht: SHIFT/CTRL/ALT zusaetzlich als Links-/Rechts-Variante
 * fuehren (GetAsyncKeyState kennt beide; Profile duerfen numerische VKs wie
 * 0xA0 binden); die generische Taste ist gedrueckt, solange eine Seite es ist. */
static void imsg_key_msg(WPARAM wp, LPARAM lp, int down)
{
    int vk = (int)wp, side = 0, l = 0, r = 0;
    UINT sc = (UINT)((lp >> 16) & 0xFF);
    int ext = (int)((lp >> 24) & 1);
    if (vk == VK_SHIFT) {
        side = (int)MapVirtualKeyA(sc, MAPVK_VSC_TO_VK_EX);
        if (side != VK_RSHIFT) side = VK_LSHIFT;
        l = VK_LSHIFT; r = VK_RSHIFT;
    } else if (vk == VK_CONTROL) {
        side = ext ? VK_RCONTROL : VK_LCONTROL;
        l = VK_LCONTROL; r = VK_RCONTROL;
    } else if (vk == VK_MENU) {
        side = ext ? VK_RMENU : VK_LMENU;
        l = VK_LMENU; r = VK_RMENU;
    }
    if (!side) {
        imsg_set(vk, down);
        return;
    }
    imsg_set(side, down);
    if (down) imsg_set(vk, 1);
    else if (!g_kdown[l] && !g_kdown[r]) imsg_set(vk, 0);
}

static void imouse_focus_gain(HWND w, const char *why)
{
    if (!g_focus) {
        InterlockedExchange(&g_focus, 1);
        HLOG_I("INPUT: Fenster-Fokus erhalten (%s)", why);
    }
    if (g_want) imouse_capture_on(w, why);
}

static void imouse_focus_lose(const char *why)
{
    if (g_focus) {
        InterlockedExchange(&g_focus, 0);
        HLOG_I("INPUT: Fenster-Fokus verloren (%s) -> Tasten geloest", why);
    }
    imsg_clear();
    imouse_capture_off(why);
    InterlockedExchange(&g_test_focus, 0);
}

static int imouse_btn_vk(UINT msg)
{
    switch (msg) {
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        return VK_LBUTTON;
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
        return VK_RBUTTON;
    default:
        return VK_MBUTTON;
    }
}

/* W110: Seitentaste aus WM_XBUTTONDOWN/UP (HIWORD(wp) = 1/2). */
static int imouse_xbtn_vk(WPARAM wp)
{
    return HIWORD(wp) == XBUTTON2 ? VK_XBUTTON2 : VK_XBUTTON1;
}

static int imouse_any_btn_down(void)
{
    return g_kdown[VK_LBUTTON] || g_kdown[VK_RBUTTON] ||
        g_kdown[VK_MBUTTON] || g_kdown[VK_XBUTTON1] ||
        g_kdown[VK_XBUTTON2];
}

static LRESULT CALLBACK imouse_wndproc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    /* Reconcile also controller pause requests on the UI thread, before
     * any input or cursor handling. Repeated open calls are idempotent. */
    imouse_menu_capture(sb_ovl_open());
    if (g_captured && !imouse_allowed()) imouse_capture_off("ausserhalb Mission");
    /* W118 REM-OVERLAY: ESC und (bei offenem Overlay) Tasten/Maustasten
     * gehen ans Overlay, nicht an die Spiel-Eingabe. */
    if (sb_phud_wndproc(w, msg, wp, lp)) return 0; /* W122 PERF-HUD: F2 */
    if (g_cfg.vr_enable && sb_vr_wndproc(msg, (unsigned)wp, (long)lp)) return 0; /* W134 VR-PROTO: Recentering/Kopf */
    if (g_cfg.overlay && sb_ovl_wndproc(w, msg, wp, lp)) return 0;
    switch (msg) {
    case IMOUSE_MSG_INIT: {
        if (g_cfg.mouse_capture) {
            RAWINPUTDEVICE rid;
            rid.usUsagePage = 0x01; /* Generic Desktop */
            rid.usUsage = 0x02;     /* Maus */
            rid.dwFlags = 0;        /* nur im Vordergrund (kein INPUTSINK) */
            rid.hwndTarget = w;
            if (!RegisterRawInputDevices(&rid, 1, sizeof rid)) {
                HLOG_W("INPUT: Raw Input nicht registriert (GLE=%lu)",
                       GetLastError());
            }
            HLOG_I("INPUT: Maus fangen aktiv (Raw Input; F10 = freigeben/fangen, "
                   "Fokusverlust gibt frei)");
        }
        SetTimer(w, IMOUSE_TIMER_ID, 250, NULL);
        HLOG_I("INPUT: Fenster-Eingabe installiert (input_source=%d "
               "mouse_capture=%d input_test_hook=%d, Fokus=%d)",
               g_cfg.input_source, g_cfg.mouse_capture, g_cfg.input_test_hook,
               imouse_is_fg(w));
        if (imouse_is_fg(w)) imouse_focus_gain(w, "Start");
        return 0;
    }
    case WM_INPUT: {
        RAWINPUT ri;
        UINT sz = sizeof ri;
        if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &sz,
                            sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            ri.header.dwType == RIM_TYPEMOUSE &&
            !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) && g_captured) {
            InterlockedExchangeAdd(&g_raw_dx, ri.data.mouse.lLastX);
            InterlockedExchangeAdd(&g_raw_dy, ri.data.mouse.lLastY);
            InterlockedIncrement(&g_raw_n);
        }
        break; /* DefWindowProc (Original) raeumt WM_INPUT auf */
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) imouse_focus_lose("Fokus verloren");
        else if (!HIWORD(wp)) imouse_focus_gain(w, "Fokus");
        break;
    case WM_ACTIVATEAPP:
        if (!wp) imouse_focus_lose("App deaktiviert");
        break;
    case WM_SETFOCUS:
        imouse_focus_gain(w, "Fokus");
        break;
    case WM_KILLFOCUS:
        imouse_focus_lose("Fokus verloren");
        break;
    case WM_SIZE:
    case WM_MOVE:
    case WM_EXITSIZEMOVE:
        if (g_captured) imouse_clip(w);
        break;
    case WM_TIMER:
        if (wp == IMOUSE_TIMER_ID) {
            /* Abgleich mit dem echten Vordergrund (verpasste Aktivierungs-
             * Nachrichten; Clip verwirft Windows z. B. bei UAC/Strg+Alt+Entf).
             * Simulierter Test-Fokus bleibt unberuehrt. */
            if (!g_test_focus) {
                int fg = imouse_is_fg(w);
                if (g_focus && !fg) imouse_focus_lose("kein Vordergrund");
                else if (!g_focus && fg) imouse_focus_gain(w, "Vordergrund");
                else if (g_captured && fg) imouse_clip(w);
            }
            return 0;
        }
        break;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            if (imouse_is_fg(w)) SetCursor(g_captured ? NULL : LoadCursorA(NULL, IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
    case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK: case WM_MBUTTONDBLCLK:
        if (g_cfg.mouse_capture && g_want && !g_captured && g_focus)
            imouse_capture_on(w, "Klick");
        if (g_cfg.input_source == 1) {
            imsg_set(imouse_btn_vk(msg), 1);
            if (g_captured && imouse_is_fg(w)) SetCapture(w); /* Loslassen ausserhalb */
        }
        break;
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP:
        if (g_cfg.input_source == 1) {
            imsg_set(imouse_btn_vk(msg), 0);
            if (imouse_is_fg(w) && !imouse_any_btn_down() && GetCapture() == w)
                ReleaseCapture();
        }
        break;
    case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK:
        if (g_cfg.mouse_capture && g_want && !g_captured && g_focus)
            imouse_capture_on(w, "Klick");
        if (g_cfg.input_source == 1) {
            imsg_set(imouse_xbtn_vk(wp), 1);
            if (g_captured && imouse_is_fg(w)) SetCapture(w); /* Loslassen ausserhalb */
        }
        break;
    case WM_XBUTTONUP:
        if (g_cfg.input_source == 1) {
            imsg_set(imouse_xbtn_vk(wp), 0);
            if (imouse_is_fg(w) && !imouse_any_btn_down() && GetCapture() == w)
                ReleaseCapture();
        }
        break;
    case WM_CAPTURECHANGED:
        break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (wp == VK_F10) {
            if (!(lp & 0x40000000)) { /* keine Autowiederholung */
                if (g_captured) {
                    InterlockedExchange(&g_want, 0);
                    imouse_capture_off("F10");
                } else {
                    InterlockedExchange(&g_want, 1);
                    if (g_focus) imouse_capture_on(w, "F10");
                }
            }
            return 0; /* F10 nicht an DefWindowProc (Menue-Modus) */
        }
        /* W81-Folge (Master): Alt+Enter schaltet den Darstellungsmodus und
         * darf im Cockpit nicht als ENTER-Tipp ankommen (Kontext-Bit 29). */
        if (msg == WM_SYSKEYDOWN && wp == VK_RETURN && (lp & 0x20000000) &&
            g_cfg.alt_enter)
            break;
        if (g_cfg.input_source == 1 && !(lp & 0x40000000))
            imsg_key_msg(wp, lp, 1);
        break;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (wp == VK_F10) return 0;
        if (g_cfg.input_source == 1) imsg_key_msg(wp, lp, 0);
        break;
    case WM_DESTROY:
        imouse_capture_off("Fenster zu");
        break;
    default:
        if (g_cfg.input_test_hook && msg == IMOUSE_MSG_TEST_STATE) {
            HLOG_I("MOUSE-MENU: mission=%d menu=%d gefangen=%d sichtbar=%d test=%d physisch=%d",
                imouse_in_mission(), g_menu_open, (int)g_captured, !g_captured,
                (int)g_test_focus, imouse_is_fg(w));
            return g_captured;
        }
        if (g_cfg.input_test_hook && msg == IMOUSE_MSG_TEST_FOCUS) {
            /* Test-Hook: Fokus logisch setzen/nehmen (ohne Vordergrund). */
            if (wp) {
                InterlockedExchange(&g_test_focus, 1);
                imouse_focus_gain(w, "Test-Fokus");
            } else {
                imouse_focus_lose("Test-Fokus weg");
            }
            return 0;
        }
        if (g_cfg.input_test_hook && msg == IMOUSE_MSG_TEST_MOVE) {
            /* Test-Hook: Mausdelta wie ein Raw-Input-Paket einspeisen. */
            if (g_captured) {
                InterlockedExchangeAdd(&g_raw_dx, (LONG)(INT_PTR)wp);
                InterlockedExchangeAdd(&g_raw_dy, (LONG)(INT_PTR)lp);
                InterlockedIncrement(&g_raw_n);
            }
            if (g_cfg.input_diag)
                HLOG_I("INPUT: test move dx=%d dy=%d (gefangen=%d)",
                       (int)(INT_PTR)wp, (int)(INT_PTR)lp, (int)g_captured);
            return 0;
        }
        break;
    }
    return CallWindowProcA(g_old_proc, w, msg, wp, lp);
}

static void imouse_install(void)
{
    HWND h;
    LONG_PTR old;
    if (g_installed) return;
    h = imouse_find_window();
    if (!h) return;
    old = SetWindowLongPtrA(h, GWLP_WNDPROC, (LONG_PTR)imouse_wndproc);
    if (!old) {
        HLOG_W("INPUT: Fenster-Eingabe nicht installiert (GLE=%lu)",
               GetLastError());
        InterlockedExchange(&g_installed, -1);
        return;
    }
    g_old_proc = (WNDPROC)old;
    InterlockedExchange(&g_installed, 1);
    /* Raw-Input-Registrierung + Erst-Fokus auf dem Fenster-Thread. */
    PostMessageA(h, IMOUSE_MSG_INIT, 0, 0);
}

/* W118: Subclass aktiv? (d3d11_render.c fragt das Overlay sonst selbst) */
int imouse_subclassed(void)
{
    return g_installed == 1;
}

/* W118: alle Nachrichten-Tasten loesen (Overlay oeffnet/schliesst; UI-Thread) */
void imouse_release_keys(void)
{
    if (g_installed == 1) imsg_clear();
}

/* W188 GAME-MENU: Menue offen -> Maus freigeben (Zeiger sichtbar, kein
 * ClipCursor), Menue zu -> wieder fangen, wenn sie vorher gefangen war und
 * das Fenster den Fokus hat. UI-Thread. */
void imouse_menu_capture(int open)
{
    open = open != 0;
    if (open == g_menu_open) return;
    g_menu_open = open;
    if (open) {
        g_menu_had_cap = g_captured != 0;
        imouse_release_keys();
        imouse_capture_off("Menue offen");
        if (imouse_is_fg(g_mwnd)) {
            ClipCursor(NULL);
            SetCursor(LoadCursorA(NULL, IDC_ARROW));
        }
    } else {
        if (g_menu_had_cap && g_mwnd && g_focus && g_want)
            imouse_capture_on(g_mwnd, "Menue zu");
        g_menu_had_cap = 0;
    }
}

int imsg_key(int vk, unsigned frame)
{
    LONG gen;
    if (vk <= 0 || vk > 255) return 0;
    if (g_cfg.key_tap_queue)
        return itap_eval(vk, (int)g_kdown[vk]); /* W92: Warteschlange statt 2-Frame-Halte */
    gen = g_kgen + 1; /* 0 bleibt "ungueltig" */
    if (InterlockedExchange(&g_kpend[vk], 0)) {
        g_kseen[vk] = frame;
        g_kseen_gen[vk] = gen;
    }
    if (g_kdown[vk]) return 1;
    /* Kurzer Tipp (Down+Up zwischen zwei Polls): noch IMSG_MIN_FRAMES
     * Spiel-Frames als gedrueckt melden, ausser Fokus ging seither verloren. */
    return g_kseen_gen[vk] == gen && frame - g_kseen[vk] < IMSG_MIN_FRAMES;
}

int imsg_real_vk(int vk)
{
    extern unsigned g_frames;
    if (g_cfg.input_source == 1) return imsg_key(vk, g_frames);
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

int imouse_poll(int *dx, int *dy, int *blocked)
{
    int raw = 0;
    if (g_cfg.key_tap_queue) itap_poll_begin(); /* W92: ein Poll = ein Wiedergabe-Schritt */
    *dx = *dy = 0;
    *blocked = 0;
    if ((g_cfg.mouse_capture || g_cfg.input_source == 1) && !g_installed)
        imouse_install();
    if (g_cfg.mouse_capture && g_installed == 1) {
        raw = 1;
        *dx = (int)InterlockedExchange(&g_raw_dx, 0);
        *dy = (int)InterlockedExchange(&g_raw_dy, 0);
        if (g_suppress_btn &&
            !imsg_real_vk(VK_LBUTTON) && !imsg_real_vk(VK_RBUTTON) &&
            !imsg_real_vk(VK_MBUTTON) && !imsg_real_vk(VK_XBUTTON1) &&
            !imsg_real_vk(VK_XBUTTON2))
            InterlockedExchange(&g_suppress_btn, 0);
        if (!g_captured || g_suppress_btn) {
            *dx = *dy = 0;
            /* Free pointer in the game's own menus still permits buttons
             * (e.g. primary fire = confirm); never feed aim deltas there. */
            *blocked = g_suppress_btn || g_menu_open || sb_ovl_open() ||
                imouse_in_mission() || g_cfg.mouse_capture_outside_mission;
        }
    }
    if (g_cfg.input_focus_only && !imouse_has_focus()) *blocked = 1;
    return raw;
}
