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
