/* loc_boot.c - W275 LOC-BOOT: Line of Contact im Client starten (siehe loc_boot.h).
 *
 * Aufbau (PoC, "so weit es ohne Disc-Abbild geht"):
 *  1. Profil: sb_profile_select_loc() - SB-Adressen der XDK-Funktionen werden
 *     ueber loc_profile.inc (aus docs/loc/LOC_HOOKS.tsv) auf LoC abgebildet,
 *     alle anderen SB-Adressen auf eine Null-Seite (sb_profile.c).
 *  2. Kernel: krnl_loc_enable() - 31 LoC-only-Ordinals (krnl_loc.c),
 *     T:/U: unter Title-ID 43430009, eigener Spielstandordner <hdd_root>_LOC.
 *  3. Hooks: dieselben D3D-/XAPI-/XInput-Handler wie fuer SB (patch_jmp mit
 *     SB-Adresse -> SBA() -> LoC-Adresse). Nur Eintraege, die das Profil
 *     kennt (Status "ok", ABI geprueft: gleiches "ret N"). SB-Spielcode-,
 *     MMIO- und Bink-Patches entfallen.
 *  4. Audio: LoC nutzt XACT statt der SB-DirectSound-Wrapper. Bis zum
 *     XACT-HLE (Plan: docs/loc/LOC_BOOT.md) stummschaltende Stubs auf alle
 *     37 vom Spiel gerufenen XACT-Einstiege + DirectSoundCreate/
 *     XAudioDownloadEffectsImage/UseFullHRTF/DoWork. Wichtig: das Spiel
 *     wiederholt XAudioDownloadEffectsImage("D:\media\sndeff\dspimage.bin")
 *     ohne Abbruch, bis es gelingt - ohne Stub haengt der Boot dort.
 * Alles nur mit [expert] loc_boot=1 und erkannter LoC-XBE; SB unberuehrt. */
#include "loc_boot.h"
#include "loc_stub_va.h"
#include "hooks_int.h"
#include "fs_emu.h"
#include "hcfg.h"
#include "hlog.h"
#include "kernel_hle.h"
#include "sb_profile.h"
#include "xdk_hooks.h"
#include "sb_rauto.h"
#include "loc_mem.h"
#include "sb_mauto.h"
#include "sb_cchain.h"
#include "loc_net.h"
#include "loc_pad.h"
#include "loc_audio.h"
#include "loc_sync.h"
#include "loc_rollback.h" /* W325 */
#include "loc_lsarb_game.h" /* LOC-LOCKSTEP-ARBITER */
#include "loc_rulenet.h" /* W420 LOC-CLIPDUMP */
#include "loc_chatnet.h" /* W437 LOC-CHAT */
#include "loc_camp.h" /* W492 CAMP-E2E: Reinforcements-Schutz */
#include "sb_tex.h" /* W297: Texttextur nach Lock neu hochladen */
#include "loc_render.h" /* W297: Textebene */
#include "d3d11_render.h" /* W342: sb_render_im_vdata (SetVertexData2s/4s) */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <intrin.h>

void krnl_loc_enable(void);   /* krnl_loc.c */
unsigned krnl_loc_used(void); /* krnl_loc.c */

typedef uint32_t u32;

/* ---------- Null-Seiten-Meldungen (SB-Adressen ohne LoC-Gegenstueck) ---------- */
static volatile LONG s_miss_logged;
static u32 s_miss_seen[64];

static void loc_miss(u32 eu)
{
    LONG k;
    unsigned i;
    for (i = 0; i < 64; i++) {
        if (s_miss_seen[i] == eu) {
            return;
        }
    }
    k = InterlockedIncrement(&s_miss_logged);
    if (k <= 64) {
        s_miss_seen[k - 1] = eu;
        HLOG_I("LOCBOOT: SB-Adresse 0x%X ohne LoC-Gegenstueck -> Null-Seite (#%ld)", eu, k);
    }
}

/* W493: Bildzaehler fuer die LOCMEM-Quarantaene */
static uint32_t loc_mem_clock(void) { return (uint32_t)hooks_frame_count(); }

void loc_boot_select(void)
{
    sb_profile_select_loc();
    g_sba_loc_miss = loc_miss;
    krnl_loc_enable();
    { /* eigener Spielstandordner (wie US: nie mit SB mischen) */
        char d[sizeof g_cfg.hdd_root];
        if (g_cfg.hdd_root_loc[0]) {
            strcpy_s(d, sizeof d, g_cfg.hdd_root_loc);
        } else {
            size_t n;
            strcpy_s(d, sizeof d, g_cfg.hdd_root);
            n = strlen(d);
            while (n > 1 && (d[n - 1] == '/' || d[n - 1] == '\\')) {
                d[--n] = 0;
            }
            strcat_s(d, sizeof d, "_LOC");
        }
        strcpy_s(g_cfg.hdd_root, sizeof g_cfg.hdd_root, d);
    }
    /* SB-spezifische Spielstand-Pruefung/-Schutz gelten nicht fuer LoC. */
    g_cfg.save_check = 0;
    g_cfg.save_lock = 0;
    /* W320 LOC-PERF: Die LoC-Mission taktet selbst mit 20 Hz (FUN_00033050(3), 3 VBlank-Felder je Bild)
     * und hat keine Zwischenbilder. Mit interp_fps>0 wiederholte der Present-Takt das Bild nur (120/s,
     * GPU-Last) und verschob den Swap des Spiel-Render-Threads um 0-25 ms (Ruckeln, W320 P1/J1).
     * loc_direct_present=1: Bild direkt praesentieren (Offscreen-BB weg, wie interp_fps=0 von Anfang an). */
    if (g_cfg.loc_interp && g_cfg.interp_fps > 0) {
        /* W323 LOC-INTERP: Zwischenbilder im 20-Hz-Missionstakt (render_interp.c, Weiche loc_render_interp_mission).
         * interp_fps bleibt; Menue/Lobby praesentiert das Spiel direkt (keine Wiederhol-Presents im LoC-Profil). */
        extern int sb_render_offscreen(void);
        HLOG_I("LOCBOOT: interp_fps %d bleibt (loc_interp, Zwischenbilder im %d-Hz-Missionstakt), offscreen=%d",
               g_cfg.interp_fps, loc_render_tick_hz(), sb_render_offscreen());
    } else if (g_cfg.loc_direct_present && g_cfg.interp_fps > 0) {
        extern int sb_render_views_rebuild(const char *why);
        extern int sb_render_offscreen(void);
        int was = g_cfg.interp_fps;
        g_cfg.interp_fps = 0;
        if (sb_render_offscreen() && !sb_render_views_rebuild("LoC: direkt praesentieren")) {
            g_cfg.interp_fps = was;
        }
        HLOG_I("LOCBOOT: interp_fps %d -> %d (loc_direct_present, LoC ohne Zwischenbilder), offscreen=%d", was,
               g_cfg.interp_fps, sb_render_offscreen());
    }
    /* SB-Replay-Ketten (Menuefolge Steel Battalion) nie in LoC einspeisen. */
    g_cfg.replay_steps[0] = 0;
    g_cfg.replay[0] = 0;
    if (g_cfg.loc_replay[0]) { /* W277: eigene Test-Tastenfolge fuer LoC-Menues */
        strcpy_s(g_cfg.replay, sizeof g_cfg.replay, g_cfg.loc_replay);
        HLOG_I("LOCBOOT: Test-Tastenfolge loc_replay aktiv (%s)", g_cfg.loc_replay);
    }
    /* W277: Replay-/Missions-Autostart folgen SB-Szenen und zeigen bei
     * Fehlschlag einen pausierenden Hinweis (LOCISO1: Stillstand ab f=14). */
    sb_rauto_disable();
    sb_mauto_disable();
    sb_cchain_disable(); /* W303 */
    /* W277: Contig/VB-Daten mit Xbox-Spiegel (LoC liest [res+4]|0x80000000). */
    if (!loc_mem_init()) {
        HLOG_E("LOCBOOT: Contig-Spiegel nicht verfuegbar - ABBRUCH (exit 8)");
        hlog_close();
        ExitProcess(8);
    }
    loc_mem_config(g_cfg.loc_mem_reclaim, (uint32_t)g_cfg.loc_mem_quar, loc_mem_clock); /* W493 LOC-MEMLEAK */
    HLOG_I("LOCBOOT: Profil %s aktiv, Spielstaende getrennt: hdd_root=%s (save_check/save_lock/SB-Replay aus)",
           sb_profile_name(), g_cfg.hdd_root);
}

/* ---------- W277: LastError (XDK 5788) ----------
 * LoC-GetLastError @0x12AD8A: "mov eax,[0x73E0D4]; mov ecx,fs:[4];
 * mov eax,[ecx+eax*4]; mov eax,[eax+8]" - Index = XBE-_tls_index (im Lauf
 * -37), Feld +8. Der SB-Pfad (Index [0x6E8A80], Feld +4) traf hier die
 * Null-Seite (Index 0) und schrieb in einen fremden Slot: LoC sah nach
 * ReadFile(overlapped) nie ERROR_IO_PENDING (997) und wiederholte
 * XPhysicalAlloc/ReadFile/XPhysicalFree ohne Ende (FUN_0004B920, W277). */
#define LOC_TLS_INDEX_VA 0x73E0D4u
#define LOC_XAPI_GLE_OFF 8u
void loc_set_last_error(unsigned int *slots, unsigned int code)
{
    static volatile LONG logged;
    int idx;
    if (!slots) {
        return;
    }
    idx = *(volatile int *)(uintptr_t)LOC_TLS_INDEX_VA;
    if (InterlockedExchange(&logged, 1) == 0) {
        HLOG_I("LOCGLE: _tls_index=[0x%X]=%d, LastError bei Block+%u (XDK 5788)", LOC_TLS_INDEX_VA, idx,
               LOC_XAPI_GLE_OFF);
    }
    if (idx >= -1024 && idx < 2048) {
        u32 blk = slots[idx];
        if (!blk) {
            blk = (u32)(uintptr_t)calloc(1, 64); /* Thread ohne Prefill */
            if (blk) {
                slots[idx] = blk;
            }
        }
        if (blk) {
            *(u32 *)(uintptr_t)(blk + LOC_XAPI_GLE_OFF) = code;
        }
    }
}

/* ---------- Audio-Stubs (XACT/DSOUND), stumm ---------- */
#define LOC_E_FAIL 0x80004005u
static uint8_t s_fake_pool[48][256];
static volatile LONG s_fake_n;
static u32 loc_fake(void)
{
    LONG k = InterlockedIncrement(&s_fake_n);
    if (k > 48) {
        k = 48; /* Rest teilt sich das letzte Objekt (nur Null-Speicher) */
    }
    return (u32)(uintptr_t)s_fake_pool[k - 1];
}

static void loc_out(u32 pp, u32 v)
{
    if (pp) {
        __try { *(u32 *)(uintptr_t)pp = v; }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

static volatile LONG s_stub_seen[LOC_N_STUBS];
static volatile LONG s_stub_n[LOC_N_STUBS]; /* W363 DIAG(audio_api_diag): Aufrufe je Einstieg (Stub oder HLE) */
void loc_api_count(int id) { if (id >= 0 && id < (int)LOC_N_STUBS) { InterlockedIncrement(&s_stub_n[id]); } }
long loc_api_calls(int id) { return (id >= 0 && id < (int)LOC_N_STUBS) ? s_stub_n[id] : 0; }
static void loc_stub_first(int id, const char *name)
{
    loc_api_count(id);
    if (id >= 0 && id < (int)LOC_N_STUBS && InterlockedExchange(&s_stub_seen[id], 1) == 0) {
        HLOG_I("LOCSTUB: %s erster Aufruf", name);
    }
}

/* Stubs mit fester Argumentzahl (stdcall, = ret N/4, vom Generator geprueft). */
#define LS0(id, nm, rv) static u32 __stdcall ls_##nm(void) { loc_stub_first(id, #nm); return rv; }
#define LS1(id, nm, rv) static u32 __stdcall ls_##nm(u32 a) { (void)a; loc_stub_first(id, #nm); return rv; }
#define LS2(id, nm, rv) static u32 __stdcall ls_##nm(u32 a, u32 b) { (void)a; (void)b; loc_stub_first(id, #nm); return rv; }
#define LS3(id, nm, rv) static u32 __stdcall ls_##nm(u32 a, u32 b, u32 c) { (void)a; (void)b; (void)c; loc_stub_first(id, #nm); return rv; }
#define LS4(id, nm, rv) static u32 __stdcall ls_##nm(u32 a, u32 b, u32 c, u32 d) { (void)a; (void)b; (void)c; (void)d; loc_stub_first(id, #nm); return rv; }
#define LS5(id, nm, rv) static u32 __stdcall ls_##nm(u32 a, u32 b, u32 c, u32 d, u32 e) { (void)a; (void)b; (void)c; (void)d; (void)e; loc_stub_first(id, #nm); return rv; }
#define LS8(id, nm, rv) static u32 __stdcall ls_##nm(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f, u32 g, u32 h) { (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; loc_stub_first(id, #nm); return rv; }

/* Erzeuger: Ausgabezeiger erhaelt ein eigenes Null-Objekt. */
static u32 __stdcall ls_DirectSoundCreate(u32 guid, u32 pp, u32 unk)
{
    (void)guid; (void)unk;
    loc_stub_first(0, "DirectSoundCreate");
    loc_out(pp, loc_fake());
    return 0;
}
LS4(1, XAudioDownloadEffectsImage, 0)    /* S_OK: sonst Endlosschleife ohne dspimage.bin */
LS0(2, DirectSoundUseFullHRTF, 0)
LS0(3, DirectSoundDoWork, 0)
LS0(4, XACTEngineDoWork, 0)
static u32 __stdcall ls_XACTEngineCreate(u32 params, u32 pp)
{
    (void)params;
    loc_stub_first(5, "XACTEngineCreate");
    loc_out(pp, loc_fake());
    return 0;
}
static u32 __stdcall ls_IXACTEngine_CreateSoundSource(u32 e, u32 flags, u32 pp)
{
    (void)e; (void)flags;
    loc_stub_first(6, "IXACTEngine_CreateSoundSource");
    loc_out(pp, loc_fake());
    return 0;
}
static u32 __stdcall ls_IXACTEngine_CreateSoundBank(u32 e, u32 data, u32 size, u32 pp)
{
    (void)e; (void)data; (void)size;
    loc_stub_first(7, "IXACTEngine_CreateSoundBank");
    loc_out(pp, loc_fake());
    return 0;
}
static u32 __stdcall ls_IXACTEngine_RegisterWaveBank(u32 e, u32 data, u32 size, u32 pp)
{
    (void)e; (void)data; (void)size;
    loc_stub_first(8, "IXACTEngine_RegisterWaveBank");
    loc_out(pp, loc_fake());
    return 0;
}
static u32 __stdcall ls_IXACTEngine_RegisterStreamedWaveBank(u32 e, u32 params, u32 pp)
{
    (void)e; (void)params;
    loc_stub_first(9, "IXACTEngine_RegisterStreamedWaveBank");
    loc_out(pp, loc_fake());
    return 0;
}
LS2(10, IXACTEngine_UnRegisterWaveBank, 0)
LS3(11, IXACTEngine_SetMasterVolume, 0)
LS3(12, IXACTEngine_SetI3dl2Listener, 0)
LS8(13, IXACTEngine_SetListenerOrientation, 0)
LS5(14, IXACTEngine_SetListenerPosition, 0)
LS5(15, IXACTEngine_SetListenerVelocity, 0)
LS3(16, IXACTEngine_GlobalPause, 0)
LS2(17, IXACTEngine_RegisterNotification, 0)
LS3(18, IXACTEngine_GetNotification, LOC_E_FAIL) /* != 0 = keine Benachrichtigung (Aufrufer 0x8B6A0) */
LS1(19, IXACTEngine_CommitDeferredSettings, 0)
LS2(20, IXACTEngine_EnableHeadphones, 0)
LS2(21, IXACTEngine_SetParameterControl, 0)
static u32 __stdcall ls_IXACTSoundBank_GetSoundCueIndexFromFriendlyName(u32 sb, u32 name, u32 pidx)
{
    (void)sb; (void)name;
    loc_stub_first(22, "IXACTSoundBank_GetSoundCueIndexFromFriendlyName");
    loc_out(pidx, 0);
    return 0;
}
LS3(23, IXACTSoundBank_PrepareEx, 0)
LS3(24, IXACTSoundBank_PlayEx, 0)
LS4(25, IXACTSoundBank_Stop, 0)
static u32 __stdcall ls_IXACTSoundBank_CreateWmaPlayList(u32 sb, u32 a, u32 b, u32 pp)
{
    (void)sb; (void)a; (void)b;
    loc_stub_first(26, "IXACTSoundBank_CreateWmaPlayList");
    loc_out(pp, loc_fake());
    return 0;
}
LS1(27, IXACTWmaPlayList_Release, 0)
LS3(28, IXACTWmaPlayList_Add, 0)
LS1(29, IXACTWmaPlayList_Next, 0)
LS5(30, IXACTWmaPlayList_GetCurrentSongInfo, LOC_E_FAIL)
LS2(31, IXACTWmaPlayList_SetPlaybackBehavior, 0)
LS1(32, IXACTSoundSource_Release, 0)
LS5(33, IXACTSoundSource_SetPosition, 0)
LS3(34, IXACTSoundSource_SetI3DL2Source, 0)
LS5(35, IXACTSoundSource_SetVelocity, 0)
LS2(36, IXACTSoundSource_SetMixBins, 0)
LS2(37, IXACTSoundSource_SetMixBinVolumes, 0)
LS1(38, IXACTSoundSource_StopSoundCues, 0)
static u32 __stdcall ls_IXACTSoundSource_GetStatus(u32 src, u32 pst)
{
    (void)src;
    loc_stub_first(39, "IXACTSoundSource_GetStatus");
    loc_out(pst, 0);
    return 0;
}

static u32 __stdcall ls_XInitDevices(u32 n, u32 types)
{
    (void)n; (void)types;
    loc_stub_first(40, "XInitDevices (USB-Stapel entfaellt, XInput-Hooks melden SBC)");
    return 0;
}

LS1(41, D3DDevice_SetRenderState_MultiSampleMode, 0)
LS1(42, D3DDevice_SetRenderState_MultiSampleRenderTargetMode, 0)
LS1(43, D3DDevice_SetRenderState_MultiSampleAntiAlias, 0)

LS1(44, XSetProcessQuantumLength, 0)

/* W342 LOC-DISPLAY: SetVertexData2s/4s (XDK 5788, SB linkt sie nicht). LoC zeichnet damit alle
 * UI-Grundformen (Linie Begin(2), Rahmen Begin(3), Feld/Verlauf Begin(6); FUN_000ECEC0..ED0C0).
 * Ohne Hook schrieb die Original-Funktion nur in den Push-Buffer: Begin/End kamen ohne Vertex an,
 * Menues ohne Rahmen, Kopfbalken, Zeilenfelder, Auswahlrahmen und Kartenraster. NV2A wandelt
 * die 16-Bit-Werte vorzeichenbehaftet und unnormiert in float (2S: z = 0, w = 1). */
static u32 __stdcall ls_D3DDevice_SetVertexData2s(u32 reg, u32 a, u32 b)
{
    loc_stub_first(47, "D3DDevice_SetVertexData2s (IM-Pfad)");
    sb_render_im_vdata(reg, (float)(int16_t)a, (float)(int16_t)b, 0.0f, 1.0f);
    return 0;
}
static u32 __stdcall ls_D3DDevice_SetVertexData4s(u32 reg, u32 a, u32 b, u32 c, u32 d)
{
    loc_stub_first(48, "D3DDevice_SetVertexData4s (IM-Pfad)");
    sb_render_im_vdata(reg, (float)(int16_t)a, (float)(int16_t)b, (float)(int16_t)c, (float)(int16_t)d);
    return 0;
}

#define LOC_WSASYSNOTREADY 10091u
static u32 __stdcall ls_XNetStartup(u32 params)
{
    (void)params;
    if (InterlockedExchange(&s_stub_seen[45], 1) == 0) {
        HLOG_I("LOCSTUB: XNetStartup -> WSASYSNOTREADY (kein Netz im Offline-Boot, Aufrufer 0x%X)",
               (u32)(uintptr_t)_ReturnAddress());
    }
    return LOC_WSASYSNOTREADY;
}
static u32 __stdcall ls_WSAStartup(u32 ver, u32 data)
{
    (void)ver; (void)data;
    if (InterlockedExchange(&s_stub_seen[46], 1) == 0) {
        HLOG_I("LOCSTUB: WSAStartup -> WSASYSNOTREADY (Aufrufer 0x%X)", (u32)(uintptr_t)_ReturnAddress());
    }
    return LOC_WSASYSNOTREADY;
}

/* Direkter 5-Byte-JMP auf eine LoC-Adresse (Stubs ohne SB-Gegenstueck). */
static unsigned s_nstub;
static void loc_patch(u32 va, void *handler, const char *name)
{
    uint8_t *p = (uint8_t *)(uintptr_t)va;
    DWORD old;
    intptr_t rel = (uint8_t *)handler - (p + 5);
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    p[0] = 0xE9;
    memcpy(p + 1, &rel, 4);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    s_nstub++;
    HLOG_I("LOCHOOK %s @0x%X (Stub)", name, va);
}
#define LOC_STUB(nm) loc_patch(LOC_VA_##nm, (void *)ls_##nm, #nm)

/* SB-Hook nur setzen, wenn das LoC-Profil die Adresse kennt. */
static unsigned s_nhook, s_nskip;
static void loc_hook(u32 sb_va, void *handler, const char *name)
{
    if (!sb_profile_loc_has(sb_va)) {
        s_nskip++;
        HLOG_W("LOCHOOK %s (SB 0x%X): kein LoC-Gegenstueck - uebergangen", name, sb_va);
        return;
    }
    patch_jmp(sb_va, handler, name);
    s_nhook++;
}

/* XGetDeviceChanges: nur der Original-Controller (SBC) gilt als eingesteckt.
 * LoC fragt auch Gamepads ab (FUN_0001A6C0) und ruft dann das ungehookte
 * XInputGetCapabilities auf dem Fake-Handle -> Absturz. SBC-Typ = SB 0x2557A4
 * -> LoC 0x1E39A4 (Profil). */
static u32 __stdcall loc_XGetDeviceChanges(u32 type, u32 pIns, u32 pRem)
{
    if (type == SBA(0x2557A4u)) {
        return hook_XGetDeviceChanges(type, pIns, pRem);
    }
    if (loc_pad_is_gamepad_type(type)) { /* W286: virtuelles Gamepad (loc_pad.c) */
        loc_pad_device_changes(pIns, pRem);
        return 0;
    }
    loc_out(pIns, 0);
    loc_out(pRem, 0);
    return 0;
}

/* W286: XInputOpen/GetState - Gamepad-Typ bzw. -Handle an das virtuelle Pad (loc_pad.c). */
static u32 __stdcall loc_XInputOpen(u32 type, u32 port, u32 flags, u32 poll)
{
    u32 h = loc_pad_open(type, port);
    return h ? h : hook_XInputOpen(type, port, flags, poll);
}
static u32 __stdcall loc_XInputGetState(u32 h, u32 pState)
{
    return loc_pad_is_handle(h) ? loc_pad_get_state(pState) : hook_XInputGetState(h, pState);
}

/* SB-Aufrufstelle (E8 rel32) auf handler umlenken, nur wenn abgebildet. */
static void loc_hook_call(u32 sb_va, void *handler, const char *name)
{
    uint8_t *p;
    intptr_t rel;
    DWORD old;
    if (!sb_profile_loc_has(sb_va)) {
        s_nskip++;
        HLOG_W("LOCHOOK %s (SB 0x%X): kein LoC-Gegenstueck - uebergangen", name, sb_va);
        return;
    }
    p = (uint8_t *)(uintptr_t)SBA(sb_va);
    if (p[0] != 0xE8) {
        HLOG_E("LOCHOOK %s @0x%X: kein CALL (0x%02X) - ABBRUCH", name, (u32)(uintptr_t)p, p[0]);
        hlog_close();
        ExitProcess(7);
    }
    rel = (uint8_t *)handler - (p + 5);
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    memcpy(p + 1, &rel, 4);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    s_nhook++;
    HLOG_I("HOOK %s @0x%X (call)", name, (u32)(uintptr_t)p);
}

/* LoC-eigener Byte-Patch (XDK-5788-Interna ohne SB-Gegenstueck): alle
 * Originalbytes werden vorher verglichen, sonst Abbruch (exit 7). */
static unsigned s_nbytes;
static void loc_patch_bytes(u32 va, const uint8_t *expect, const uint8_t *data, u32 len, const char *name)
{
    uint8_t *p = (uint8_t *)(uintptr_t)va;
    DWORD old;
    if (memcmp(p, expect, len) != 0) {
        HLOG_E("LOCBYTES %s @0x%X: Byte-Mismatch - ABBRUCH (exit 7)", name, va);
        hlog_close();
        ExitProcess(7);
    }
    VirtualProtect(p, len, PAGE_EXECUTE_READWRITE, &old);
    memcpy(p, data, len);
    VirtualProtect(p, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, len);
    s_nbytes++;
    HLOG_I("LOCBYTES %s @0x%X len=%u", name, va, len);
}

/* W277: Stub mit Bytevergleich (Adresse ohne Generator-Eintrag). */
static unsigned s_nextra; /* W277: Stubs/Patches ohne Generator-Eintrag */
static void loc_patch_checked(u32 va, const uint8_t *expect, u32 len, void *handler, const char *name)
{
    if (memcmp((const void *)(uintptr_t)va, expect, len) != 0) {
        HLOG_E("LOCBYTES %s @0x%X: Byte-Mismatch - ABBRUCH (exit 7)", name, va);
        hlog_close();
        ExitProcess(7);
    }
    loc_patch(va, handler, name);
    s_nstub--;
    s_nextra++;
}

/* W277: IDirectSound_Release auf unseren stummen DSOUND-Objekten (keine
 * VTable). Aufrufer u. a. die Sprach-/Headset-Init FUN_00132838 nach dem
 * ersten Tastendruck am Titel (findet kein Geraet -> Release). */
static u32 __stdcall ls_IDirectSound_Release(u32 self)
{
    static volatile LONG seen;
    (void)self;
    if (InterlockedExchange(&seen, 1) == 0) {
        HLOG_I("LOCSTUB: IDirectSound_Release erster Aufruf (Aufrufer 0x%X)", (u32)(uintptr_t)_ReturnAddress());
    }
    return 0;
}

/* W277: D3DSurface_LockRect @0x188750 -> intern Lock2DSurface @0x18B940
 * (stdcall 6: pSurf, 0, 0, pLocked, pRect, flags). Wrapper ruft das Original
 * und protokolliert (gedeckelt) das Ergebnis; XFONT-TextOut schreibt dorthin. */
typedef u32(__stdcall *loc_lock2d_t)(u32, u32, u32, u32, u32, u32);
static u32 __stdcall ls_D3DSurface_LockRect(u32 surf, u32 plocked, u32 prect, u32 flags)
{
    static volatile LONG n;
    u32 r = ((loc_lock2d_t)(uintptr_t)0x18B940u)(surf, 0, 0, plocked, prect, flags);
    { /* W297 LOC-MISSION: XFONT schreibt nach dem Lock per CPU in die Texttextur (Parent, z. B.
       * 0x7264E4, 640x480 LIN). Ohne Markierung blieb die einmal hochgeladene Kopie stehen ->
       * Fragetexte der YES/NO-Dialoge unsichtbar, nur fruehe Texte (Laufschrift) sichtbar. */
        u32 parent = 0;
        __try { parent = ((const u32 *)(uintptr_t)surf)[5]; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (parent) { sb_tex_mark_dirty(parent); }
        sb_tex_mark_dirty(surf);
        { /* Textebene merken (CPU-Clear bei D3D-Clear, loc_render.c) */
            u32 lp = 0, lb = 0;
            __try { lp = ((u32 *)(uintptr_t)plocked)[0]; lb = ((u32 *)(uintptr_t)plocked)[1]; }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
            loc_render_text_lock(surf, lb, lp);
        }
    }
    if (InterlockedIncrement(&n) <= 12) {
        u32 pitch = 0, bits = 0, f[6];
        memset(f, 0xEE, sizeof f);
        __try {
            pitch = ((u32 *)(uintptr_t)plocked)[0];
            bits = ((u32 *)(uintptr_t)plocked)[1];
            memcpy(f, (void *)(uintptr_t)surf, sizeof f);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        HLOG_I("LOCLOCK #%ld: surf=0x%X rect=0x%X flags=0x%X -> pitch=%u bits=0x%X | Common=%08X Data=%08X Lock=%08X "
               "Format=%08X Size=%08X Parent=%08X (RA 0x%X)",
               (long)n, surf, prect, flags, pitch, bits, f[0], f[1], f[2], f[3], f[4], f[5],
               (u32)(uintptr_t)_ReturnAddress());
    }
    return r;
}

/* W277: Bodenmaterial-Abfrage 0xB6919 (Spielcode, Fahrzeug-Update):
 *   movsx ecx,word [esi+0x740]; mov edx,[0x2F70AC]; lea ecx,[ecx*3];
 *   shl ecx,5; cmp word [ecx+edx+2],0x458; je 0xB6959
 * Index 0x7FFF (= "kein Bodenpolygon", z. B. Titel-Demo) liest auf der Xbox
 * 3 MB hinter der Tabelle im flachen RAM einen beliebigen Wert; im Client
 * liegt die Tabelle im Host-Heap -> Zugriffsverletzung (W277J). Die Stelle
 * wertet negative/0x7FFF-Indizes als "nicht 0x458" (weiter bei 0xB6935). */
static volatile LONG s_ground_sentinel;
static void loc_ground_note(void)
{
    if (InterlockedIncrement(&s_ground_sentinel) == 1) {
        HLOG_I("LOCFIX: Bodenmaterial-Index 0x7FFF bei 0xB6919 (kein Polygon) -> als 'nicht 0x458' gewertet");
    }
}
static __declspec(naked) void loc_ground_check(void)
{
    __asm {
        movsx ecx, word ptr [esi + 0x740]
        cmp ecx, 0x7FFF
        je sentinel
        test ecx, ecx
        js sentinel
        mov edx, dword ptr ds:[0x2F70AC]
        lea ecx, [ecx + ecx * 2]
        shl ecx, 5
        cmp word ptr [ecx + edx + 2], 0x458
        je is458
        push 0xB6935
        ret
    is458:
        push 0xB6959
        ret
    sentinel:
        pushad
        call loc_ground_note
        popad
        push 0xB6935
        ret
    }
}

/* ---------------------------------------------------------------- W492 CAMP-E2E: Reinforcements-Auswahl
 * FUN_00118E50 (EBX = &Index u16, EDX = Maske, ESI = Schritt +1/-1, Rueckgabe AX) sucht das naechste gesetzte Bit in
 * Shop-VTs | Garage-VTs und kommt bei Maske 0 nie zurueck (Spiel-Thread haengt). Das kann auch mit gueltigen
 * Kampagnendaten passieren (Lager ohne VT im Shop dieses Turns, Bestand ausverkauft, eigene VTs verloren); nur
 * Xbox-Live-Kampagne (loc_live=1). Schutz: Maske 0 -> Rueckgabe 0 (Cursor bleibt, wie "keine Aenderung"). */
static volatile LONG s_reinf_blocked;
static int __cdecl loc_reinf_check(uint32_t mask)
{
    if (!loccamp_reinf_blocked(mask)) return 0;
    if (InterlockedIncrement(&s_reinf_blocked) == 1) {
        HLOG_I("LOCCAMP: Reinforcements-Auswahl ohne VT (Garage und Shop leer) - Cursor bleibt (Schutz 0x118E50)");
    }
    return 1;
}
static __declspec(naked) void loc_reinf_guard(void)
{
    __asm {
        pushad
        push edx
        call loc_reinf_check
        add esp, 4
        test eax, eax
        popad
        jnz blocked
        xor eax, eax
        mov ax, word ptr [ebx]
        push 0x118E55
        ret
    blocked:
        xor eax, eax
        ret
    }
}

/* W492 TEST/DIAG (nur mit loc_camp_min_side): Missionsende FUN_00029C60(Code, Verzoegerung) mit Aufrufer und
 * Teamlage ins Log (Teilnehmermaske [0x306830], Anzahl [0x3071A0], je Spieler Team u16 [0x5C515C + i*0xAE8] und
 * Flags [0x5C5168 + i*0xAE8]). Keine Eingaben. */
static volatile LONG s_end_notes;
static void __cdecl loc_end_note(u32 ret, u32 code, u32 delay)
{
    char buf[256];
    int i, n, o;
    if (*(volatile u32 *)(uintptr_t)0x3068E0u != 0 || InterlockedIncrement(&s_end_notes) > 6) return;
    n = (int)*(volatile u32 *)(uintptr_t)0x3071A0u;
    o = sprintf_s(buf, sizeof buf, "maske=0x%X n=%d", *(volatile u32 *)(uintptr_t)0x306830u, n);
    for (i = 0; i < n && i < 10 && o > 0 && o < (int)sizeof buf - 40; i++) {
        o += sprintf_s(buf + o, sizeof buf - o, " p%d:team=%u fl=0x%X", i,
                       (unsigned)*(volatile uint16_t *)(uintptr_t)(0x5C515Cu + (u32)i * 0xAE8u),
                       *(volatile u32 *)(uintptr_t)(0x5C5168u + (u32)i * 0xAE8u));
    }
    HLOG_I("LOCCAMP: Missionsende Code %u (Verz. %u) von 0x%X, %s", code, delay, ret, buf);
}
static __declspec(naked) void loc_end_hook(void)
{
    __asm {
        pushad
        push dword ptr [esp + 40]
        push dword ptr [esp + 40]
        push dword ptr [esp + 40]
        call loc_end_note
        add esp, 12
        popad
        mov eax, dword ptr ds:[0x3068E0]
        push 0x29C65
        ret
    }
}

/* ---------------------------------------------------------------- W408 LOC-UNLOCK-ALL
 * Alle VTs, Missionen und Items immer frei (Nutzer 21.11., kein Schalter; LOC_RE1 §4, LOC_BOOT §74).
 * CONFIG.DAT liegt im Speicher bei 0x7279B4 (0x24 B): +0x18 VT-Maske, +0x1C Missions-Maske, +0x20 Item-Maske.
 * Der originale Freischaltcode (FUN_000C8B60 -> FUN_00126B50, Ton 0x1C8) setzt
 *     Missionen |= alle Stufentabellen 0x286374..0x286398 (10 x u32, ergibt 0x02FFFFFF);  VT = 0xFFFFFFEF;
 *     FUN_000A5900 (CONFIG.DAT speichern)
 * und laesst die Items unberuehrt; die Item-Maske waechst sonst nur ueber den Kampagnen-Fortschritt (FUN_00126CB0:
 * Bit i, sobald der Pilot Item i besitzt). "Alle Items" = ODER der Item-Stufentabelle 0x286354 (8 x u32, ergibt
 * 0x3FFFFFFF), dieselbe Vereinigung, die FUN_000E7A40 bei voller Stufe bildet.
 * Hier gleich nach jedem Laden (FUN_000EB760, Ende 0xEB7B5) und nach jeder Vorgabe (FUN_000EBC40, Ende 0xEBCE9):
 * dieselben Zuweisungen wie FUN_00126B50 plus Items, ohne eigenes Speichern - das Spiel schreibt CONFIG.DAT wie
 * ueblich selbst (fehlende Datei: FUN_000EFD90 -> Vorgabe + FUN_000A5900; nach Missionen FUN_00126CB0).
 * Die VT-Auswahl der Match-Lobby folgt der VT-Maske (Lauf: alter Pilot mit 4 Garagen-VTs waehlt aus allen 31).
 * Bestehende Free-/Link-Piloten (LIVEUSER/LINKUSER): die Garage entsteht nur bei der Neuanlage aus der VT-Maske
 * (FUN_00126E30: je VT ein Platz [0]=1/[1]=Id); der Hangar zeigt nur VTs der Garage (FUN_000611F0 ueber
 * FUN_00127AB0). Nach dem Lesen des Pilotblocks (FUN_000A5E40, Aufrufe 0xF00B1 im Speicherschritt der Szene 6 und
 * 0x10FEDF in FUN_0010FEA0) werden fehlende VTs in freie Garagenplaetze ergaenzt; so hat der Pilot dieselben VTs wie
 * ein neu angelegter. Item-Bestaende (+0x21C) bleiben unberuehrt: auch ein neu angelegter Pilot hat dort nach dem
 * Speichern 0 (Lauf), die Items folgen der Item-Maske. Kampagnen-Piloten (Modus 0) bleiben unberuehrt. */
#define LOCU_VTS 0x7279CCu       /* CONFIG +0x18 */
#define LOCU_MIS 0x7279D0u       /* CONFIG +0x1C */
#define LOCU_ITEMS 0x7279D4u     /* CONFIG +0x20 */
#define LOCU_TAB_ITEM 0x286354u  /* Item-Stufentabelle, 8 x u32 */
#define LOCU_TAB_MIS 0x286374u   /* Missions-Stufentabelle, 10 x u32 (FUN_00126B50) */
#define LOCU_VT_ALL 0xFFFFFFEFu  /* wie FUN_00126B50 (Id 4 zaehlt nie) */
#define LOCU_MODE 0x72799Cu      /* u8: 0 Kampagne, 1 Live Free Mission, 2 System Link */
#define LOCU_BLOCK 0x727AE0u     /* Pilotblock, Byte 0 = Pilot tot/ungueltig */
#define LOCU_PILOT_PTR 0x72702Cu /* Zeiger auf den aktiven Piloten (0x718 B) */
#define LOCU_GARAGE 0x3Cu        /* 60 x 8 B */
#define LOCU_GARAGE_N 60

static volatile LONG s_unlock_logs;
static void __cdecl loc_unlock_cfg(u32 after_load)
{
    volatile u32 *vt = (volatile u32 *)(uintptr_t)LOCU_VTS;
    volatile u32 *mi = (volatile u32 *)(uintptr_t)LOCU_MIS;
    volatile u32 *it = (volatile u32 *)(uintptr_t)LOCU_ITEMS;
    const u32 *tm = (const u32 *)(uintptr_t)LOCU_TAB_MIS, *ti = (const u32 *)(uintptr_t)LOCU_TAB_ITEM;
    u32 o_vt = *vt, o_mi = *mi, o_it = *it, mis = 0, itm = 0;
    int i;
    for (i = 0; i < 10; i++) mis |= tm[i];
    for (i = 0; i < 8; i++) itm |= ti[i];
    *vt = LOCU_VT_ALL;
    *mi = o_mi | mis;
    *it = o_it | itm;
    if (InterlockedIncrement(&s_unlock_logs) <= 16) {
        HLOG_I("LOCUNLOCK: CONFIG nach %s: VT %08X->%08X Missionen %08X->%08X Items %08X->%08X",
               after_load ? "Laden" : "Vorgabe", o_vt, *vt, o_mi, *mi, o_it, *it);
    }
}
/* ersetzt 0xEB7B5 "mov [0x7279D0],ecx; ret" (Ende FUN_000EB760; AL = Ladeergebnis bleibt erhalten) */
static __declspec(naked) void loc_unlock_after_load(void)
{
    __asm {
        mov dword ptr ds:[0x7279D0], ecx
        push eax
        push ecx
        push edx
        push 1
        call loc_unlock_cfg
        add esp, 4
        pop edx
        pop ecx
        pop eax
        ret
    }
}
/* ersetzt 0xEBCE9 "jmp 0xEB7C0" (Ende FUN_000EBC40) */
static __declspec(naked) void loc_unlock_after_defaults(void)
{
    __asm {
        push eax
        push ecx
        push edx
        push 0
        call loc_unlock_cfg
        add esp, 4
        pop edx
        pop ecx
        pop eax
        push 0xEB7C0
        ret
    }
}

static int locu_owned(const uint8_t *p, unsigned vt)
{
    int s;
    for (s = 0; s < LOCU_GARAGE_N; s++) {
        const uint8_t *g = p + LOCU_GARAGE + s * 8;
        if (g[0] == 1 && g[1] == vt) return 1;
    }
    return 0;
}
static int locu_free_slot(const uint8_t *p)
{
    int s, k;
    for (s = 0; s < LOCU_GARAGE_N; s++) {
        const uint8_t *g = p + LOCU_GARAGE + s * 8;
        for (k = 0; k < 8 && g[k] == 0; k++) {}
        if (k == 8) return s;
    }
    return -1;
}
/* Fehlende freigeschaltete VTs in freie Garagenplaetze (wie FUN_00126E30: [0]=1, [1]=Id). */
static void loc_unlock_pilot(void)
{
    unsigned mode = *(const uint8_t *)(uintptr_t)LOCU_MODE;
    uint8_t *p = (uint8_t *)(uintptr_t)*(const u32 *)(uintptr_t)LOCU_PILOT_PTR;
    u32 vtm = *(const u32 *)(uintptr_t)LOCU_VTS;
    int added = 0, full = 0, used = 0, k;
    unsigned vt;
    if (mode != 1 && mode != 2) return;
    if (!p || *(const uint8_t *)(uintptr_t)LOCU_BLOCK != 0) return; /* kein bzw. ungueltiger Pilot */
    for (k = 0; k < 16 && p[0x28 + k] == 0; k++) {}
    if (k == 16) return; /* Name leer = kein Pilot */
    for (vt = 0; vt < 32; vt++) {
        int s;
        if (!(vtm & (1u << vt)) || vt == 4 || locu_owned(p, vt)) continue;
        s = locu_free_slot(p);
        if (s < 0) { full++; continue; }
        p[LOCU_GARAGE + s * 8] = 1;
        p[LOCU_GARAGE + s * 8 + 1] = (uint8_t)vt;
        added++;
    }
    if (added == 0 && full == 0) return; /* Pilot schon vollstaendig */
    for (k = 0; k < LOCU_GARAGE_N; k++) used += p[LOCU_GARAGE + k * 8] == 1;
    HLOG_I("LOCUNLOCK: Pilot (Modus %u) ergaenzt: %d VTs, Garage %d/%d belegt%s", mode, added, used, LOCU_GARAGE_N,
           full ? ", Garage voll" : "");
}
typedef u32(__cdecl *locu_pilot_read_t)(void);
/* ersetzt den Aufruf FUN_000A5E40 (Pilotblock lesen) bei 0x10FEDF; Rueckgabe AL = 0 gelesen */
static u32 __cdecl loc_unlock_pilot_read(void)
{
    u32 r = ((locu_pilot_read_t)(uintptr_t)0xA5E40u)();
    HLOG_I("LOCUNLOCK: Pilot gelesen (Modus %u, Ergebnis %u)", (unsigned)*(const uint8_t *)(uintptr_t)LOCU_MODE,
           r & 0xFFu);
    if ((r & 0xFFu) == 0) loc_unlock_pilot();
    return r;
}

static void locu_patch(u32 va, const uint8_t *expect, u32 len, uint8_t op, void *handler, const char *name)
{
    uint8_t *p = (uint8_t *)(uintptr_t)va;
    intptr_t rel = (uint8_t *)handler - (p + 5);
    DWORD old;
    if (memcmp(p, expect, len) != 0) {
        HLOG_E("LOCBYTES %s @0x%X: Byte-Mismatch - ABBRUCH (exit 7)", name, va);
        hlog_close();
        ExitProcess(7);
    }
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    p[0] = op;
    memcpy(p + 1, &rel, 4);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    HLOG_I("LOCUNLOCK: Eingriff %s @0x%X", name, va);
}
/* W495 HATCH-LIGHTS (Steelbound#102): Lichtleiste des oberen Monitors haengt in der Startsequenz im Himmel.
 * Befund: FUN_0007B890 zeichnet die Cockpit-Lichtgruppe mit FUN_00075200 (je Teil MVP = Welt[Zeichen-Slot
 * [0x3071E0]] x ViewProj 0x3078D0). Im Lichtmodell ist die Lichtleiste ein Kind des Lukenlichts; ihre
 * Spur in der Startanimation dreht genau gegen die Luke, die Welt (lokal x Eltern-Welt, FUN_00085140) bleibt
 * fest auf dem Platz des geschlossenen Monitors. Unsere Nachbildung rechnet das wie der Spielcode.
 * Korrektur (loc_hatch_lights=1): vor dem Zeichnen erhaelt jedes Teil der Gruppe mit bewegtem Elternteil
 * (Eltern-Index >= 1) im Zeichen-Slot die Welt Endlage (letzter Schluessel der Spur) x Eltern-Welt. Das Licht
 * faehrt starr mit Luke und Monitor; in der Endlage (Luke zu) ist das Ergebnis gleich. Spielzustand und
 * Animationsdaten bleiben unveraendert (das Spiel rechnet die Welt beim naechsten Update selbst neu). */
typedef void(__cdecl *loc_euler_mtx_t)(float *out, const float *rot3, const float *pos3); /* FUN_00084B70 */
typedef void(__cdecl *loc_draw_lights_t)(int ent);                                       /* FUN_00075200 */
static void hl_mul(float *o, const float *a, const float *b) /* o = a x b (Zeilenvektoren, wie FUN_0012F3F9) */
{
    float r[16];
    unsigned i, j;
    for (i = 0; i < 4u; i++) {
        for (j = 0; j < 4u; j++) {
            r[i * 4u + j] = a[i * 4u] * b[j] + a[i * 4u + 1u] * b[4u + j] + a[i * 4u + 2u] * b[8u + j] + a[i * 4u + 3u] * b[12u + j];
        }
    }
    memcpy(o, r, sizeof r);
}
static unsigned s_hl_fixed;
static void hl_fix(int ei)
{
    __try {
        const u32 en = *(const u32 *)(uintptr_t)0x2F70ACu + (u32)ei * 0x60u;
        const u32 an = *(const u32 *)(uintptr_t)(en + 0x4Cu), parts = *(const u32 *)(uintptr_t)(en + 0x50u);
        const u32 np = *(const uint8_t *)(uintptr_t)(en + 0x36u), drw = *(const u32 *)(uintptr_t)0x3071E0u;
        u32 mdl, par, p;
        if (!an || !parts || np < 3u || drw > 2u) return;
        if (*(const uint8_t *)(uintptr_t)(en + 0x48u) & 2u) return; /* gespiegelte Spuren: unberuehrt */
        mdl = *(const u32 *)(uintptr_t)(*(const u32 *)(uintptr_t)0x560BD4u + *(const uint16_t *)(uintptr_t)(en + 2u) * 4u);
        par = mdl ? *(const u32 *)(uintptr_t)(mdl + 0x18u) : 0u;
        if (!par) return;
        for (p = 2; p < np; p++) {
            __declspec(align(16)) float loc[16];
            const int q = *(const signed char *)(uintptr_t)(par + p);
            u32 tr, n, last;
            if (q < 1 || (u32)q >= p) continue; /* Wurzel bzw. Kind der Wurzel: unveraendert */
            tr = an + *(const u32 *)(uintptr_t)(an + p * 4u);
            n = *(const uint16_t *)(uintptr_t)(tr + 2u);
            if (n < 2u) continue; /* keine Animation */
            last = tr + 4u + (n - 1u) * 0x1Cu; /* Schluessel: u16, u16 Zeit, Rotation xyz, Position xyz */
            ((loc_euler_mtx_t)(uintptr_t)0x84B70u)(loc, (const float *)(uintptr_t)(last + 4u), (const float *)(uintptr_t)(last + 0x10u));
            hl_mul((float *)(uintptr_t)(parts + p * 0x1E0u + (drw + 4u) * 0x40u), loc,
                   (const float *)(uintptr_t)(parts + (u32)q * 0x1E0u + (drw + 4u) * 0x40u));
            if (s_hl_fixed++ == 0) {
                HLOG_I("HATCH-LIGHTS: Ent %d Teil %u (Eltern %d) faehrt in Endlage mit (Spur %u Schluessel)", ei, p, q, n);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
/* ersetzt den Aufruf FUN_00075200 in FUN_0007B890 (0x7B9EE) */
static void __cdecl loc_hatch_draw(int ei)
{
    hl_fix(ei);
    ((loc_draw_lights_t)(uintptr_t)0x75200u)(ei);
}

static void loc_unlock_install(void)
{
    static const uint8_t k_load[7] = { 0x89, 0x0D, 0xD0, 0x79, 0x72, 0x00, 0xC3 };  /* mov [0x7279D0],ecx; ret */
    static const uint8_t k_defaults[5] = { 0xE9, 0xD2, 0xFA, 0xFF, 0xFF };            /* jmp 0xEB7C0 */
    static const uint8_t k_pilot[5] = { 0xE8, 0x5C, 0x5F, 0xF9, 0xFF };               /* call 0xA5E40 */
    static const uint8_t k_pilot6[5] = { 0xE8, 0x8A, 0x5D, 0xFB, 0xFF };              /* call 0xA5E40 */
    locu_patch(0xEB7B5u, k_load, 7, 0xE9, (void *)loc_unlock_after_load, "CONFIG geladen (FUN_000EB760)");
    locu_patch(0xEBCE9u, k_defaults, 5, 0xE9, (void *)loc_unlock_after_defaults, "CONFIG-Vorgabe (FUN_000EBC40)");
    locu_patch(0x10FEDFu, k_pilot, 5, 0xE8, (void *)loc_unlock_pilot_read, "Pilot gelesen (FUN_000A5E40, Kampagne)");
    locu_patch(0xF00B1u, k_pilot6, 5, 0xE8, (void *)loc_unlock_pilot_read, "Pilot gelesen (FUN_000A5E40, Szene 6)");
}

/* XDK-5788-Interna (Musterkatalog fuer spaeteren Scan, docs/loc/LOC_BOOT.md). */
static void loc_xdk_internals(void)
{
    /* XAPI-Init FUN_0012EBE2: liest den PE-Kopf des Kernels (0x80010000),
     * sucht dessen INIT-Sektion und setzt per sgdt/cli/ljmp einen GDT-
     * Deskriptor (FUN_0012EE20). Ohne echte GDT bedeutungslos -> ret.
     * Aufruf aus FUN_0012EC46, wenn [Prcb+0x250] == 0 (Fake-PRCB). */
    static const uint8_t k_gdt_exp[6] = { 0x55, 0x8B, 0xEC, 0x51, 0x51, 0xA1 };
    static const uint8_t k_ret[1] = { 0xC3 };
    loc_patch_bytes(0x12EBE2u, k_gdt_exp, k_ret, 1, "XAPI-KernelInitGdt(ret)");
    { /* W277: IDirectSound_Release @0x1A0ACA (mov eax,[esp+4]; lea ecx,[eax-8]; ...) */
        static const uint8_t k_dsrel[8] = { 0x8B, 0x44, 0x24, 0x04, 0x8D, 0x48, 0xF8, 0xF7 };
        loc_patch_checked(0x1A0ACAu, k_dsrel, 8, (void *)ls_IDirectSound_Release, "IDirectSound_Release");
    }
    { /* W277: Bodenmaterial-Abfrage 0xB6919 (28 Bytes bis 0xB6935, siehe loc_ground_check) */
        static const uint8_t k_ground[28] = { 0x0F, 0xBF, 0x8E, 0x40, 0x07, 0x00, 0x00, 0x8B, 0x15, 0xAC, 0x70, 0x2F,
                                              0x00, 0x8D, 0x0C, 0x49, 0xC1, 0xE1, 0x05, 0x66, 0x81, 0x7C, 0x11, 0x02,
                                              0x58, 0x04, 0x74, 0x24 };
        loc_patch_checked(0xB6919u, k_ground, 28, (void *)loc_ground_check, "Bodenmaterial-Index(0x7FFF)");
    }
    { /* W277: D3DSurface_LockRect @0x188750 (Wrapper, ruft Lock2DSurface 0x18B940) */
        static const uint8_t k_slock[8] = { 0x8B, 0x44, 0x24, 0x10, 0x8B, 0x4C, 0x24, 0x0C };
        loc_patch_checked(0x188750u, k_slock, 8, (void *)ls_D3DSurface_LockRect, "D3DSurface_LockRect(diag)");
    }
}

static u32 s_pushbuf[16]; /* Fake-Pushbuffer-Zelle (get == put = idle), wie SB */

void loc_boot_install(void)
{
    char pm[200];
    int bad = sb_profile_verify(pm, sizeof pm);
    if (bad) {
        HLOG_E("%s - %d von %u Eintraegen weichen ab, ABBRUCH (exit 8)", pm, bad, sb_profile_check_entries());
        hlog_close();
        ExitProcess(8);
    }
    HLOG_I("PROFILE: %s aktiv (%u Abbildungen, %u Erstbyte-Pruefungen ok, Rest -> Null-Seite)",
           sb_profile_name(), sb_profile_text_entries(), sb_profile_check_entries());
    HLOG_I("LOCKRNL: %u LoC-only-Ordinals installiert", krnl_loc_used());
    /* FS-Segment (Xbox-KPCR): Musterscan ueber alle Code-Sektionen vor .rdata
     * (.text bis XPP, 0x12000-0x1EC720; Overlays seg00..30 sind Daten). */
    fs_emu_patch_scan(0x12000u, 0x1EC720u);

    /* --- D3D8 (XDK 5788, gleiche Namen/ABI wie 4627) --- */
    loc_hook(0x210B20u, hook_Direct3D_CreateDevice, "Direct3D_CreateDevice");
    loc_hook(0x20DDE0u, hook_D3DDevice_Swap, "D3DDevice_Swap");
    loc_hook(0x20DF80u, hook_D3DDevice_Clear, "D3DDevice_Clear");
    loc_hook(0x210AA0u, hook_D3DDevice_Begin, "D3DDevice_Begin");
    loc_hook(0x210AE0u, hook_D3DDevice_End, "D3DDevice_End");
    loc_hook(0x20E830u, hook_D3DDevice_SetTexture, "D3DDevice_SetTexture");
    loc_hook(0x20FB00u, hook_D3DDevice_SetVertexShader, "D3DDevice_SetVertexShader");
    loc_hook(0x210040u, hook_D3DDevice_SetPixelShader, "D3DDevice_SetPixelShader");
    loc_hook(0x20E640u, hook_D3DDevice_GetBackBuffer2, "D3DDevice_GetBackBuffer2");
    loc_hook(0x20E370u, hook_D3DDevice_SetRenderTarget, "D3DDevice_SetRenderTarget");
    loc_hook(0x20BAD0u, hook_D3DDevice_SetRenderState_Simple, "D3DDevice_SetRenderState_Simple(fastcall)");
    loc_hook(0x2109B0u, hook_D3DDevice_SetVertexData2f, "D3DDevice_SetVertexData2f");
    loc_hook(0x2109F0u, hook_D3DDevice_SetVertexData4f, "D3DDevice_SetVertexData4f");
    loc_hook(0x210A50u, hook_D3DDevice_SetVertexDataColor, "D3DDevice_SetVertexDataColor");
    loc_hook(0x20E690u, hook_D3DDevice_GetRenderTarget2, "D3DDevice_GetRenderTarget2");
    loc_hook(0x20E6D0u, hook_D3DDevice_SetViewport, "D3DDevice_SetViewport");
    loc_hook(0x20E390u, hook_D3DDevice_SetTransform, "D3DDevice_SetTransform");
    loc_hook(0x210F70u, hook_D3DResource_Release, "D3DResource_Release");
    loc_hook(0x20BB00u, hook_D3D_SetRenderStateNotInline, "SetRenderStateNotInline");
    loc_hook(0x20BE30u, hook_D3D_SetRenderState_EdgeAntiAlias, "SetRenderState_EdgeAntiAlias");
    loc_hook(0x20BE70u, hook_D3D_SetRenderState_ShadowFunc, "SetRenderState_ShadowFunc");
    loc_hook(0x20BEB0u, hook_D3D_SetRenderState_FogColor, "SetRenderState_FogColor");
    loc_hook(0x20BF00u, hook_D3D_SetRenderState_CullMode, "SetRenderState_CullMode");
    loc_hook(0x20BFF0u, hook_D3D_SetRenderState_TextureFactor, "SetRenderState_TextureFactor");
    loc_hook(0x20C050u, hook_D3D_SetRenderState_LineWidth, "SetRenderState_LineWidth");
    loc_hook(0x20C0C0u, hook_D3D_SetRenderState_Dxt1NoiseEnable, "SetRenderState_Dxt1NoiseEnable");
    loc_hook(0x20C390u, hook_D3D_SetTextureState_TexCoordIndex, "SetTextureState_TexCoordIndex");
    loc_hook(0x20C4A0u, hook_D3D_SetTextureState_BumpEnv, "SetTextureState_BumpEnv");
    loc_hook(0x20C510u, hook_D3D_SetTextureState_BorderColor, "SetTextureState_BorderColor");
    loc_hook(0x20C550u, hook_D3D_SetTextureState_ColorKeyColor, "SetTextureState_ColorKeyColor");
    loc_hook(0x20C590u, hook_D3D_SetTextureStageStateNotInline, "SetTextureStageStateNotInline");
    loc_hook(0x20CF40u, hook_D3D_SetRenderState_ZEnable, "SetRenderState_ZEnable");
    loc_hook(0x20CFD0u, hook_D3D_SetRenderState_StencilEnable, "SetRenderState_StencilEnable");
    loc_hook(0x20D060u, hook_D3D_SetRenderState_StencilFail, "SetRenderState_StencilFail");
    loc_hook(0x20D100u, hook_D3D_SetRenderState_OcclusionCullEnable, "SetRenderState_OcclusionCullEnable");
    loc_hook(0x20EB80u, hook_GetDisplayFieldStatus, "D3D-GetDisplayFieldStatus(counter)");
    loc_hook(0x20E520u, hook_BlockUntilVerticalBlank, "D3D-BlockUntilVerticalBlank(vblank_hz)");
    loc_hook(0x20EBC0u, hook_D3D_SetTileNoWait, "D3D-SetTileNoWait(no-op)");
    loc_hook(0x212820u, hook_Get2DSurfaceDesc, "D3D-Get2DSurfaceDesc(640x480)");
    loc_hook(0x20F4E0u, hook_D3D_VSConst1, "D3D-VSConst1(cdecl)");
    loc_hook(0x20F540u, hook_D3D_VSConst4, "D3D-VSConst4");
    loc_hook(0x210220u, hook_D3D_PSConst, "D3D-PSConst(cdecl)");
    loc_hook(0x211810u, hook_D3D_MakeSpace, "D3D-MakeSpace");
    loc_hook(0x211130u, hook_D3D_CreateIB2, "D3D-CreateIB2");
    loc_hook(0x211170u, hook_D3D_CreateVB2, "D3D-CreateVB2");
    loc_hook(0x20E4C0u, hook_D3D_QueryStatus, "D3D-QueryStatus(ready)");
    if (g_cfg.loc_visq == 1) { /* W297: echte Sichtbarkeitstests (render_visq.c, W117) - Objekte der
                                * Entity-Tabelle zeichnet FUN_000462A0 nur bei Pixelzahl > Schwelle */
        loc_hook(0x20EA80u, hook_D3D_BeginVisTest, "D3D-BeginVisibilityTest(query)");
        loc_hook(0x20EB20u, hook_D3D_EndVisTest, "D3D-EndVisibilityTest(query)");
        g_visq_on = 1;
    }
    loc_hook(0x20F750u, hook_D3D_SetStream, "D3D-SetStream");
    loc_hook(0x20E9B0u, hook_D3D_SetIndices, "D3D-SetIndices");
    loc_hook(0x210740u, hook_D3D_DrawIdx, "D3D-DrawIdx");
    loc_hook(0x211530u, hook_D3D_KickOff, "D3D-KickOff(no-op)");
    loc_hook(0x210C20u, hook_D3DTexture_LockRect, "D3D-TexLockRect");
    loc_hook(0x2116D0u, hook_D3D_BlockOnTime, "D3D-BlockOnTime(no-op)");
    /* --- XAPI --- */
    loc_hook(0x18CFC0u, hook_XapiSelectCachePartition, "XAPI-SelectCachePartition(Cache=1)");
    loc_hook(0x192363u, hook_XLaunchNewImageA, "XLAUNCH-NewImageA(IGNORE)");
    loc_hook(0x18D1EBu, hook_XMountUtilityDrive, "XAPI-MountUtilityDrive");
    loc_hook(0x18D97Fu, hook_ReadFile, "XAPI-ReadFile(sync)");
    loc_hook(0x191250u, hook_PerfFrequency, "XAPI-PerfFreq(TSC)");
    loc_hook(0x19273Fu, hook_XapiMapLetterToDirectory, "XAPI-MapLetter(No-Op)");
    loc_hook(0x190D21u, (void *)krnl_GetExitCodeThread, "XAPI-GetExitCodeThread");
    loc_hook_call(0x190E2Fu, hook_XapiNotifyWrap, "XapiNotifyWrap(prefill)");
    /* --- XInput (SBC) --- */
    loc_hook(0x256784u, loc_XInputOpen, "XIN-Open(fake)");
    loc_hook(0x2567DAu, hook_XInputClose, "XIN-Close(no-op)");
    loc_hook(0x2569C3u, loc_XInputGetState, "XIN-GetState(kb)");
    loc_hook(0x256A32u, hook_XInputSetState, "XIN-SetState(no-op)");
    loc_hook(0x25649Eu, loc_XGetDeviceChanges, "XIN-GetDeviceChanges(nur SBC)");

    /* Fake-D3D-Geraet wie im SB-Pfad (D3D__pDevice: SB 0x21A8A0 -> LoC 0x1935B8). */
    *(u32 *)(uintptr_t)SBA(0x21A8A0u) = (u32)(uintptr_t)g_fake_d3d;
    s_pushbuf[0] = 0x1000u;
    *(u32 *)(g_fake_d3d + 0x30) = 0x1000u;
    *(u32 *)(g_fake_d3d + 0x34) = (u32)(uintptr_t)&s_pushbuf[0];
    *(u32 *)(g_fake_d3d + 0x0) = (u32)(uintptr_t)g_d3d_pool;
    *(u32 *)(g_fake_d3d + 0x4) = (u32)(uintptr_t)(g_d3d_pool + sizeof g_d3d_pool);
    *(u32 *)(g_fake_d3d + 0x380) = (u32)(uintptr_t)g_fake_obj;
    /* 5788: Unterobjekt-Zeiger liegt bei +0x794 statt +0x380 (FUN_00183A40
     * "test [[dev+0x794]+4],2", SB 0x20FA03 "[dev+0x380]"); +0/+4 (put/limit)
     * sind gleich (FUN_00188FD0 vs. SB 0x2119A0). */
    *(u32 *)(g_fake_d3d + 0x794) = (u32)(uintptr_t)g_fake_obj;
    HLOG_I("LOCBOOT: [0x%X] = Fake-D3D (D3D__pDevice)", SBA(0x21A8A0u));

    /* --- Audio: XACT/DSOUND stumm (Plan XACT-HLE: docs/loc/LOC_BOOT.md) --- */
    LOC_STUB(DirectSoundCreate);
    LOC_STUB(XAudioDownloadEffectsImage);
    LOC_STUB(DirectSoundUseFullHRTF);
    LOC_STUB(DirectSoundDoWork);
    LOC_STUB(XACTEngineDoWork);
    LOC_STUB(XACTEngineCreate);
    LOC_STUB(IXACTEngine_CreateSoundSource);
    LOC_STUB(IXACTEngine_CreateSoundBank);
    LOC_STUB(IXACTEngine_RegisterWaveBank);
    LOC_STUB(IXACTEngine_RegisterStreamedWaveBank);
    LOC_STUB(IXACTEngine_UnRegisterWaveBank);
    LOC_STUB(IXACTEngine_SetMasterVolume);
    LOC_STUB(IXACTEngine_SetI3dl2Listener);
    LOC_STUB(IXACTEngine_SetListenerOrientation);
    LOC_STUB(IXACTEngine_SetListenerPosition);
    LOC_STUB(IXACTEngine_SetListenerVelocity);
    LOC_STUB(IXACTEngine_GlobalPause);
    LOC_STUB(IXACTEngine_RegisterNotification);
    LOC_STUB(IXACTEngine_GetNotification);
    LOC_STUB(IXACTEngine_CommitDeferredSettings);
    LOC_STUB(IXACTEngine_EnableHeadphones);
    LOC_STUB(IXACTEngine_SetParameterControl);
    LOC_STUB(IXACTSoundBank_GetSoundCueIndexFromFriendlyName);
    LOC_STUB(IXACTSoundBank_PrepareEx);
    LOC_STUB(IXACTSoundBank_PlayEx);
    LOC_STUB(IXACTSoundBank_Stop);
    LOC_STUB(IXACTSoundBank_CreateWmaPlayList);
    LOC_STUB(IXACTWmaPlayList_Release);
    LOC_STUB(IXACTWmaPlayList_Add);
    LOC_STUB(IXACTWmaPlayList_Next);
    LOC_STUB(IXACTWmaPlayList_GetCurrentSongInfo);
    LOC_STUB(IXACTWmaPlayList_SetPlaybackBehavior);
    LOC_STUB(IXACTSoundSource_Release);
    LOC_STUB(IXACTSoundSource_SetPosition);
    LOC_STUB(IXACTSoundSource_SetI3DL2Source);
    LOC_STUB(IXACTSoundSource_SetVelocity);
    LOC_STUB(IXACTSoundSource_SetMixBins);
    LOC_STUB(IXACTSoundSource_SetMixBinVolumes);
    LOC_STUB(IXACTSoundSource_StopSoundCues);
    LOC_STUB(IXACTSoundSource_GetStatus);
    LOC_STUB(XInitDevices);
    LOC_STUB(D3DDevice_SetRenderState_MultiSampleMode);
    LOC_STUB(D3DDevice_SetRenderState_MultiSampleRenderTargetMode);
    LOC_STUB(D3DDevice_SetRenderState_MultiSampleAntiAlias);
    LOC_STUB(XSetProcessQuantumLength);
    if (g_cfg.loc_vdata_s) { /* W342 LOC-DISPLAY: UI-Grundformen (Standard 1) */
        LOC_STUB(D3DDevice_SetVertexData2s);
        LOC_STUB(D3DDevice_SetVertexData4s);
    }
    loc_audio_install(); /* W298: XACT-HLE ueberschreibt die stummen Audio-Stubs (loc_audio.c) */
    if (loc_net_install()) { /* W286: System-Link-Netz-HLE (loc_net.c) + Menue-Gamepad (loc_pad.c) */
        loc_pad_install();
        loc_sync_install(); /* W297: Lockstep-Zustandshash (loc_sync=1) */
        loc_pace_install(); /* W320: Missionstakt (loc_pace_diag/loc_fields, Standard aus) */
        loc_rb_install(); /* W325: Rollback-Spike (loc_rb_spike, Standard aus) */
        loc_lsarb_install(); /* LOC-LOCKSTEP-ARBITER: Pufferregler im Lockstep (nach loc_rb_install) */
    } else { /* sonst "Netz nicht bereit" wie W275 */
        LOC_STUB(XNetStartup);
        LOC_STUB(WSAStartup);
    }
    loc_xdk_internals();
    loc_render_sblook_apply(); /* W459: SB look (Experimental, Standard aus) */
    loc_unlock_install(); /* W408: alle VTs/Missionen/Items frei (immer, nur LoC) */
    if (g_cfg.loc_hatch_lights) { /* W495 HATCH-LIGHTS (#102) */
        static const uint8_t k_lights[5] = { 0xE8, 0x0D, 0x98, 0xFF, 0xFF }; /* call 0x75200 */
        locu_patch(0x7B9EEu, k_lights, 5, 0xE8, (void *)loc_hatch_draw, "Cockpit-Lichtgruppe (FUN_00075200, W495)");
    }
    if (g_cfg.loc_live) { /* W492: Reinforcements-Auswahl mit leerer Maske (nur Xbox-Live-Kampagne) */
        static const uint8_t k_reinf[5] = { 0x33, 0xC0, 0x66, 0x8B, 0x03 }; /* xor eax,eax; mov ax,[ebx] */
        loc_patch_checked(0x118E50u, k_reinf, 5, (void *)loc_reinf_guard, "Reinforcements-Auswahl (0x118E50, Live)");
    }
    if (g_cfg.loc_live && g_cfg.loc_camp && g_cfg.loc_camp_faction_swap) {
        /* W515 FACTIONSWAP (#110): HQ-Menue der Kampagne (FUN_000EF6B0, Zustand 0): fuer einen Piloten mit Seite
         * (Pilot+0x3B != 4) und gueltigen Zugdaten (FUN_000DED10 == 1) sperrt das Spiel den Punkt ENLISTMENT OFFICE
         * (Menue-ID 0x1E0, FUN_000E1350(0x1E0,1,1)), wenn der Zug ausserhalb 3..6 liegt. Freigabe: jb (Zug < 3) ->
         * jmp hinter die Sperre (0xEF754). Neutrale Piloten und Free Mission laufen nicht durch diesen Zweig. */
        loc_patch_bytes(LOCCAMP_SWAP_VA, loccamp_swap_orig, loccamp_swap_new, 12,
                        "Enlistment Office in jedem Zug (0xEF73B, loc_camp_faction_swap)");
        /* Nach dem Wechsel (Server-Status 0, kein Rauswurf) ins CAMPAIGN-Menue statt ins MAIN MENU: das laedt Pilot und
         * Garage der neuen Seite neu (FUN_00112730 push 8 -> push 3, siehe loc_camp.h). */
        loc_patch_bytes(LOCCAMP_SWAP_RELOAD_VA, loccamp_swap_reload_orig, loccamp_swap_reload_new, 6,
                        "Nach Seitenwechsel Pilot+Garage neu laden (0x11273C, loc_camp_faction_swap)");
    }
    if (g_cfg.loc_live && !g_cfg.loc_camp_min_side) {
        g_cfg.loc_camp_min_side = LOC_CAMP_BETA_MIN_SIDE; /* Nutzer 25.11.: Beta 0.6.0 - 1 Pilot je Seite reicht (Schluessel 0 = Original) */
    }
    if (g_cfg.loc_live && g_cfg.loc_camp_min_side > 0) {
        /* W492 TEST: BEGIN verlangt im Original je Seite min = Kampagne ? 3 : 1 (FUN_000FC4B0 @0xFC538:
         * mov esi,eax; neg esi; sbb esi,esi; and esi,2; push 1; inc esi) -> mov esi,N; push 1; 5x nop */
        static const uint8_t k_min[12] = { 0x8B, 0xF0, 0xF7, 0xDE, 0x1B, 0xF6, 0x83, 0xE6, 0x02, 0x6A, 0x01, 0x46 };
        uint8_t d[12] = { 0xBE, 0, 0, 0, 0, 0x6A, 0x01, 0x90, 0x90, 0x90, 0x90, 0x90 };
        d[1] = (uint8_t)g_cfg.loc_camp_min_side;
        loc_patch_bytes(0xFC538u, k_min, d, 12, "TEST BEGIN-Mindestzahl je Seite (0xFC538, loc_camp_min_side)");
        { /* FUN_00052BA0 (Lobby-Zustaende 0x49..0x4C, Kampagnenzweig [G+4]==0): eigene + andere Seite <= 2 -> Zustand
           * 0x51 "session was canceled" (cmp eax,2 @0x52BC9). TEST: Schwelle min(Original, 2*N-1) */
            static const uint8_t k_tot[3] = { 0x83, 0xF8, 0x02 };
            uint8_t t[3] = { 0x83, 0xF8, 0x02 };
            if (g_cfg.loc_camp_min_side == 1) t[2] = 0x01;
            if (t[2] != k_tot[2]) loc_patch_bytes(0x52BC9u, k_tot, t, 3, "TEST Abbruchschwelle Teilnehmer (0x52BC9, loc_camp_min_side)");
        }
        if (g_cfg.loc_camp_min_side == 1) { /* Einsatz: Modus [0x306B00]==0 und je Seite genau 1 aktiver Pilot
            * ([0x306770]==1, [0x306774]==1) -> sofort Ende Code 5/1/2 nach Punkten (0x2C18D..0x2C1BD). TEST: jne -> jmp */
            static const uint8_t k_1v1[4] = { 0x85, 0xC0, 0x75, 0x3A };
            static const uint8_t d_1v1[4] = { 0x85, 0xC0, 0xEB, 0x3A };
            loc_patch_bytes(0x2C18Du, k_1v1, d_1v1, 4, "TEST Einsatzende 1 gegen 1 aus (0x2C18F, loc_camp_min_side=1)");
        }
        { /* DIAG Missionsende (mov eax,[0x3068E0]) */
            static const uint8_t k_end[5] = { 0xA1, 0xE0, 0x68, 0x30, 0x00 };
            loc_patch_checked(0x29C60u, k_end, 5, (void *)loc_end_hook, "TEST DIAG Missionsende (0x29C60)");
        }
        HLOG_W("LOCCAMP: loc_camp_min_side=%d - BEGIN ab %d Pilot(en) je Seite (Original Kampagne 3, Beta-Standard 1)",
               g_cfg.loc_camp_min_side, g_cfg.loc_camp_min_side);
    }
    loc_rule_install(); /* W420: Clip dumping unterbinden (Host-Regel, Standard an, Abgleich im Match) */
    loc_chat_install(); /* W437: Textchat LoC online (Standard an, nur mit Treffpunkt + sec=ok) */
    HLOG_I("LOCBOOT: %u XDK-Hooks, %u uebergangen, %u Stubs (von %u), %u Byte-Patches installiert",
           s_nhook, s_nskip, s_nstub, (unsigned)LOC_N_STUBS, s_nbytes);
    HLOG_I("LOCBOOT: %u W277-Eingriffe mit Bytevergleich (IDirectSound_Release, D3DSurface_LockRect, "
           "Bodenmaterial-Index)", s_nextra);
}

int loc_boot_veh_note(EXCEPTION_POINTERS *ep)
{
    static volatile LONG n;
    LONG k = InterlockedIncrement(&n);
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    const CONTEXT *cx = ep->ContextRecord;
    uint8_t b[8];
    u32 st[6];
    if (k > 40) {
        return 0;
    }
    memset(b, 0xEE, sizeof b);
    memset(st, 0xEE, sizeof st);
    __try { memcpy(b, (const void *)(uintptr_t)cx->Eip, 8); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try { memcpy(st, (const void *)(uintptr_t)cx->Esp, sizeof st); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    HLOG_W("LOCVEH #%ld: Code=0x%lX EIP=0x%08X Ziel=0x%X (%s) Bytes=%02X %02X %02X %02X %02X %02X "
           "EAX=%08X ECX=%08X EDX=%08X ESI=%08X EDI=%08X ESP=%08X",
           k, er->ExceptionCode, cx->Eip,
           er->NumberParameters >= 2 ? (unsigned)er->ExceptionInformation[1] : 0u,
           er->NumberParameters >= 1 ? (er->ExceptionInformation[0] ? "schreiben" : "lesen") : "-",
           b[0], b[1], b[2], b[3], b[4], b[5], cx->Eax, cx->Ecx, cx->Edx, cx->Esi, cx->Edi, cx->Esp);
    HLOG_W("LOCVEH #%ld: Stack %08X %08X %08X %08X %08X %08X EBP=%08X", k, st[0], st[1], st[2], st[3], st[4], st[5],
           cx->Ebp);
    return 1;
}

void loc_boot_tick(int waited_s)
{
    if (waited_s % 10 == 0) {
        HLOG_I("LOCBOOT: t=%ds Frames=%u Threads=%d Null-Seite=%u", waited_s, hooks_frame_count(),
               krnl_xb_thread_count(), sb_profile_loc_misses());
        loc_net_tick(waited_s); /* W286 */
        loc_audio_tick(waited_s); /* W298 */
        loc_sync_tick(waited_s); /* W297 */
        loc_rb_tick(waited_s); /* W325 */
        loc_mem_tick(); /* W493: abgelaufene Quarantaene freigeben */
        if (waited_s % 30 == 0) {
            loc_mem_stats();
        }
    }
}
