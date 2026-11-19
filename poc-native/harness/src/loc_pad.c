/* loc_pad.c - W286 LOC-SYSLINK: virtuelles Xbox-Gamepad fuer die LoC-Netzmenues.
 *
 * Befund (W286E, LOC_BOOT §14): Nach "System Link Play" zaehlt das Menue
 * (FUN_000EDFF0 -> FUN_000EDC80) die Ports mit Gamepad (Flag 1 in 0x306C58[4];
 * der SBC setzt 2). Ohne Gamepad bleibt Dialog 0x11 stehen ("Controller
 * anschliessen", Unterzustand 2). Mit genau einem Gamepad waehlt das Spiel
 * dessen Port als Menue-Controller (FUN_0011F400) und geht in Szene 6
 * (Pilot-/Benutzerdaten), danach Lobby.
 * Loesung: ein Gamepad auf Port 1 (Port 0 = SBC). Zustand aus der Tastatur
 * (key_down: echte Tasten bzw. Test-Tastenfolge loc_replay), loc_pad_keys=1:
 *   Ziffernblock 8/2/4/6 = Steuerkreuz (+ linker Stick), SPACE/Ziffernblock 0 = A,
 *   Backspace = B, Enter = START. Pfeiltasten bleiben beim SBC (Visier).
 * Die Menues fragen Pad und SBC gemeinsam ab (Sammelabfrage 0x125F70).
 * Nur im LoC-Profil mit loc_net=1. */
#include "loc_pad.h"
#include "hooks_int.h"
#include "hcfg.h"
#include "hlog.h"
#include "sb_profile.h"
#include "loc_net.h"
#include "input_mouse.h" /* W341: imouse_test_post, imouse_captured */
#include "xdk_hooks.h" /* W341: hooks_frame_count */

#include <string.h>
#include <intrin.h>

typedef uint32_t u32;

#define LOC_PAD_TYPE 0x1E37FCu       /* XAPILIB g_DeviceType_Gamepad (LoC) */
#define LOC_PAD_GETCAPS 0x1E49DAu    /* XInputGetCapabilities (2 Arg., stdcall) */
#define LOC_PAD_PORT 1u
#define LOC_PAD_HANDLE_TAG 0x50414447u /* "GDAP" */

static int s_on;
static volatile LONG s_reported;
static u32 s_handle[4] = {LOC_PAD_HANDLE_TAG, 0, 0, 0}; /* eigener Handle (Inhalt egal) */
static u32 s_packet;

int loc_pad_active(void)
{
    return s_on;
}

int loc_pad_is_gamepad_type(u32 type)
{
    return s_on && type == LOC_PAD_TYPE;
}

void loc_pad_device_changes(u32 pIns, u32 pRem)
{
    u32 ins = 0;
    if (InterlockedExchange(&s_reported, 1) == 0) {
        ins = 1u << LOC_PAD_PORT; /* einmal "eingesteckt" melden */
        HLOG_I("LOCPAD: virtuelles Gamepad an Port %u gemeldet (Menue-Controller fuer System Link)", LOC_PAD_PORT);
    }
    __try {
        if (pIns) {
            *(u32 *)(uintptr_t)pIns = ins;
        }
        if (pRem) {
            *(u32 *)(uintptr_t)pRem = 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

u32 loc_pad_open(u32 type, u32 port)
{
    if (!loc_pad_is_gamepad_type(type)) {
        return 0;
    }
    HLOG_I("LOCPAD: XInputOpen(Gamepad, Port %u) -> virtuelles Pad", port);
    return (u32)(uintptr_t)s_handle;
}

int loc_pad_is_handle(u32 h)
{
    return s_on && h == (u32)(uintptr_t)s_handle;
}

/* XINPUT_STATE: u32 dwPacketNumber; XINPUT_GAMEPAD { u16 wButtons; u8 bAnalogButtons[8];
 * s16 sThumbLX, sThumbLY, sThumbRX, sThumbRY } (22 B). */
/* W297 LOC-MISSION: Pfeil hoch/runter in den Netzmenues eindeutig. Die Sammelabfrage (0x125F70)
 * wertet SBC-Hebel Y (id 0x25, > 0x40 = hoch) entgegen unserer Pfeil-Abbildung aus; Pad und SBC
 * zusammen hoben sich auf (W286K-T). Solange kein Lockstep laeuft (Lobby, VT-Wahl, Briefing,
 * Ergebnis), gehen die Pfeile daher ans Pad-Steuerkreuz und nicht an den Hebel; in Karte und
 * Mission (0x34-Rahmen) wieder an den SBC (Startpunkt, Zielen). */
int loc_pad_menu_arrows(void)
{
    u32 mport = 0xFFFFFFFFu;
    if (!s_on || !g_cfg.loc_pad_keys) {
        return 0;
    }
    __try { /* Menue-Controller-Port (FUN_0011F400): erst nach "System Link Play" = Pad */
        mport = *(const u32 *)(uintptr_t)0x3071FCu;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return mport == LOC_PAD_PORT && loc_net_test_state(NULL, NULL, 0, 512) != 3;
}

u32 loc_pad_get_state(u32 pState)
{
    uint8_t st[22];
    uint16_t b = 0;
    memset(st, 0, sizeof st);
    *(u32 *)st = ++s_packet;
    if (!g_cfg.loc_pad_keys) { /* neutral: Bedienung ueber den SBC-Pfad (gleiche Tasten) */
        __try {
            memcpy((void *)(uintptr_t)pState, st, sizeof st);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return 0;
    }
    /* Steuerkreuz nur ueber den Ziffernblock: die Pfeiltasten gehen schon an das SBC-Visier,
     * das die Menue-Sammelabfrage (0x125F70) vertikal umgekehrt liest - beide zusammen
     * hoben sich auf (W286K-T). */
    if (key_down(VK_NUMPAD8)) b |= 0x0001;
    if (key_down(VK_NUMPAD2)) b |= 0x0002;
    if (loc_pad_menu_arrows()) { /* W297: Pfeile ausserhalb des Locksteps = Steuerkreuz */
        if (key_down(VK_UP)) b |= 0x0001;
        if (key_down(VK_DOWN)) b |= 0x0002;
    }
    if (key_down(VK_NUMPAD4)) b |= 0x0004;
    if (key_down(VK_NUMPAD6)) b |= 0x0008;
    if (key_down(VK_RETURN)) b |= 0x0010; /* START */
    memcpy(st + 4, &b, 2);
    { /* Steuerkreuz zusaetzlich als linker Stick (manche Seiten lesen nur den Stick) */
        int16_t lx = 0, ly = 0;
        if (b & 0x0004) lx = -32768;
        if (b & 0x0008) lx = 32767;
        if (b & 0x0001) ly = 32767;
        if (b & 0x0002) ly = -32768;
        memcpy(st + 14, &lx, 2);
        memcpy(st + 16, &ly, 2);
    }
    if (key_down(VK_SPACE) || key_down(VK_NUMPAD0)) st[6 + 0] = 0xFF; /* A */
    if (key_down(VK_BACK)) st[6 + 1] = 0xFF;  /* B */
    if (g_cfg.loc_net_diag) { /* DIAG: Pad-Felder des Spiels (Port 1: 0x2C9020/0x2C9060 + 0x13C) */
        static uint16_t lastb = 0xFFFF;
        static uint8_t lasta;
        if (b != lastb || st[6] != lasta) {
            uint16_t held = 0, edge = 0;
            u32 mport = 0, mon = 0, poll = 0, every = 0;
            __try {
                held = *(const uint16_t *)(uintptr_t)(0x2C9020u + 0x13Cu * LOC_PAD_PORT);
                edge = *(const uint16_t *)(uintptr_t)(0x2C9060u + 0x13Cu * LOC_PAD_PORT);
                mport = *(const u32 *)(uintptr_t)0x3071FCu;
                mon = *(const u32 *)(uintptr_t)0x30683Cu;
                poll = *(const u32 *)(uintptr_t)0x306B30u;
                every = *(const u32 *)(uintptr_t)0x306944u;
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            HLOG_I("LOCPADDIAG: f=%u Pad b=0x%X A=%u | Spiel gehalten=0x%X Flanke=0x%X Menueport=%u aktiv=%u Takt=%u/%u",
                   hooks_frame_count(), b, st[6], held, edge, mport, mon, poll, every);
            lastb = b;
            lasta = st[6];
        }
    }
    __try {
        memcpy((void *)(uintptr_t)pState, st, sizeof st);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}

/* XINPUT_CAPABILITIES (0x19 B, vom Original genullt): SubType = 1 (Gamepad). */
static u32 __stdcall loc_pad_XInputGetCapabilities(u32 h, u32 pCaps)
{
    static volatile LONG first;
    if (InterlockedExchange(&first, 1) == 0) {
        HLOG_I("LOCPAD: XInputGetCapabilities erster Aufruf (Handle %s)", loc_pad_is_handle(h) ? "Pad" : "fremd");
    }
    __try {
        if (pCaps) {
            memset((void *)(uintptr_t)pCaps, 0, 0x19);
            *(uint8_t *)(uintptr_t)pCaps = 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}

int loc_pad_install(void)
{
    static const uint8_t expect[8] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x48, 0x83, 0x65};
    uint8_t *p = (uint8_t *)(uintptr_t)LOC_PAD_GETCAPS;
    intptr_t rel;
    DWORD old;
    if (memcmp(p, expect, sizeof expect) != 0) {
        HLOG_E("LOCBYTES LOCPAD XInputGetCapabilities @0x%X: Byte-Mismatch - ABBRUCH (exit 7)", LOC_PAD_GETCAPS);
        hlog_close();
        ExitProcess(7);
    }
    rel = (uint8_t *)(void *)loc_pad_XInputGetCapabilities - (p + 5);
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    p[0] = 0xE9;
    memcpy(p + 1, &rel, 4);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    s_on = 1;
    HLOG_I("LOCPAD: aktiv (Port %u; Ziffernblock 8/2/4/6=Steuerkreuz, SPACE/0=A, Backspace=B, Enter=START; Tasten %s)",
           LOC_PAD_PORT, g_cfg.loc_pad_keys ? "an" : "aus");
    return 1;
}

/* ---------------------------------------------------------------------------
 * W341 LOC-MOUSE: Maus -> Zielhebel in der LoC-Mission.
 * Befund: Das Fangen der Maus (input_mouse.c, Soll "mission") fragte sb_in_mission(); dessen SB-Adressen
 * lesen im LoC-Profil die Null-Seite (0) -> nie gefangen -> Raw-Deltas 0, nur Maustasten wirkten.
 * LoC-Mission fuer die Maus: Lockstep aktiv ([0x306830] != 0) und eigener VT abgesetzt: Spielerflags
 * [0x5C5168 + spieler*0xAE8] ohne Bit 0x10000000 (spieler = [0x306A84]). Das Bit steht, solange der Spieler auf
 * der Einsatzkarte auf das Absetzen wartet (FUN_0002E3D0, Zweig Startpunkt/Startzaehler); FUN_000BE470 (Absetzen)
 * loescht die Flags (+0x98 = 0). Der Startpunkt [0x5C5A44] taugt nicht: er springt nach dem Absetzen auf -1
 * (Lauf W341_A1). Auf der Karte, in Lobby/Briefing/Ergebnis bleibt die Maus frei (Menues unbeeinflusst). */
#define LM_ACTIVE 0x306830u
#define LM_PLAYER 0x306A84u
#define LM_FLAGS0 0x5C5168u
#define LM_PSTRIDE 0xAE8u
#define LM_ON_MAP 0x10000000u

static volatile LONG s_lm_mission = -1; /* letzter Wert (Log bei Wechsel) */

int loc_mouse_mission(void)
{
    u32 act = 0, pl = 0xFFFFFFFFu, fl = LM_ON_MAP;
    int m;
    if (!g_sba_loc) {
        return 0;
    }
    __try {
        act = *(volatile const u32 *)(uintptr_t)LM_ACTIVE;
        if (act) {
            pl = *(volatile const u32 *)(uintptr_t)LM_PLAYER;
            if (pl < 10u) {
                fl = *(volatile const u32 *)(uintptr_t)(LM_FLAGS0 + pl * LM_PSTRIDE);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        act = 0;
    }
    m = act != 0u && pl < 10u && !(fl & LM_ON_MAP);
    if ((LONG)m != s_lm_mission) {
        InterlockedExchange(&s_lm_mission, (LONG)m);
        HLOG_I("LOCMOUSE: Zielen per Maus %s (aktiv=0x%X spieler=%u flags=0x%08X f=%u)", m ? "an" : "aus", act, pl,
               fl, hooks_frame_count());
    }
    return m;
}

/* Test-Hook: Pseudo-Tasten der Testfolge (loc_replay_tick/loc_steps) -> Nachrichten ans eigene Fenster.
 * 330/331 = Maus +x/-x, 332/333 = +y/-y (je Poll LM_STEP Counts, wie ein Raw-Input-Paket),
 * 334/335 = linke/rechte Taste gehalten (WM_xBUTTONDOWN/UP an den Flanken), 336 = Bild (naechstes Present).
 * Kein SendInput, kein Fokus/Vordergrund; der logische Fokus kommt ueber IMOUSE_MSG_TEST_FOCUS. */
#define LM_STEP 16
void loc_mouse_test_drive(void)
{
    static int focus_sent, lb, rb, shot;
    int dx = 0, dy = 0, k;
    if (!g_sba_loc || !g_cfg.loc_mouse_test || !g_cfg.input_test_hook) {
        return;
    }
    if (!focus_sent) {
        if (imouse_test_post(IMOUSE_MSG_TEST_FOCUS, 1, 0)) {
            focus_sent = 1;
            HLOG_I("LOCMOUSE: Testmodus (synthetische Maus ueber Fenster-Nachrichten, logischer Fokus)");
        }
        return;
    }
    if (key_down(330)) dx += LM_STEP;
    if (key_down(331)) dx -= LM_STEP;
    if (key_down(332)) dy += LM_STEP;
    if (key_down(333)) dy -= LM_STEP;
    if (dx || dy) {
        (void)imouse_test_post(IMOUSE_MSG_TEST_MOVE, (uintptr_t)(intptr_t)dx, (intptr_t)dy);
    }
    k = key_down(334) != 0;
    if (k != lb) {
        lb = k;
        (void)imouse_test_post(k ? WM_LBUTTONDOWN : WM_LBUTTONUP, k ? MK_LBUTTON : 0, 0);
    }
    k = key_down(335) != 0;
    if (k != rb) {
        rb = k;
        (void)imouse_test_post(k ? WM_RBUTTONDOWN : WM_RBUTTONUP, k ? MK_RBUTTON : 0, 0);
    }
    k = key_down(336) != 0;
    if (k && !shot) {
        extern unsigned g_shot_census; /* render_diag.c: Bild beim naechsten Present */
        g_shot_census = 1;
        HLOG_I("LOCMOUSE: Testbild angefordert (f=%u)", hooks_frame_count());
    }
    shot = k;
}

/* Zaehler (alle ~10 s eine Zeile, nur im LoC-Profil): Polls, davon gefangen, Polls mit Raw-Delta != 0,
 * Spanne des Zielhebels (min..max X/Y), den das Spiel in diesem Fenster gelesen hat. Keine Eingaben. */
void loc_mouse_note(int dx, int dy, uint8_t aim_x, uint8_t aim_y, uint16_t btn0)
{
    static unsigned n, ncap, nmove, nb0, nb1;
    static int xmin = 256, xmax = -1, ymin = 256, ymax = -1;
    static DWORD t0;
    DWORD t;
    if (!g_sba_loc) {
        return;
    }
    n++;
    if (imouse_captured()) ncap++;
    if (dx || dy) nmove++;
    if (btn0 & 1u) nb0++; /* bi0 Hauptwaffe */
    if (btn0 & 2u) nb1++; /* bi1 Nebenwaffe */
    if (aim_x < xmin) xmin = aim_x;
    if (aim_x > xmax) xmax = aim_x;
    if (aim_y < ymin) ymin = aim_y;
    if (aim_y > ymax) ymax = aim_y;
    t = GetTickCount();
    if (!t0) t0 = t;
    if (t - t0 >= 10000u) {
        HLOG_I("LOCMOUSE: polls=%u gefangen=%u bewegt=%u zielhebel x=%d..%d y=%d..%d bi0=%u bi1=%u mission=%ld loc_mouse=%d",
               n, ncap, nmove, xmin, xmax, ymin, ymax, nb0, nb1, (long)s_lm_mission, g_cfg.loc_mouse);
        n = ncap = nmove = nb0 = nb1 = 0;
        xmin = ymin = 256;
        xmax = ymax = -1;
        t0 = t;
    }
}
