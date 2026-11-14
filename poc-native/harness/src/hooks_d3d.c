/* hooks_d3d.c - XDK-Hooks: D3D-Device (CreateDevice, Swap, Clear, Begin/End,
 * Texturen/Shader, RenderTarget, RenderStates, VertexData, Viewport,
 * Transform, Release, GPU-Register, Shader-Konstanten, Streams/Draws,
 * LockRect, KickOff). W68 SPLIT: 1:1 aus xdk_hooks.c verschoben. */
#include "hooks_int.h"
#include <intrin.h>
#include "sb_remaster.h" /* W103 REM-FRAME */
#include "sb_perfhud.h" /* W125 TICK-PERF */
#include "sb_state.h" /* W232: sb_in_mission */

/* ---------- D3D-Hooks ---------- */
u32 __stdcall hook_Direct3D_CreateDevice(u32 adapter, u32 devType, u32 hwnd,
                                                u32 behavior, u32 pParams, u32 ppDev)
{
    u32 w = 0, h = 0, fmt = 0, flags = 0;
    if (pParams) {
        /* X_D3DPRESENT_PARAMETERS: W@0 H@4 Fmt@8 ... */
        w = *(u32 *)(uintptr_t)(pParams + 0);
        h = *(u32 *)(uintptr_t)(pParams + 4);
        fmt = *(u32 *)(uintptr_t)(pParams + 8);
        flags = *(u32 *)(uintptr_t)(pParams + 20);
    }
    HLOG_I("D3D Direct3D_CreateDevice(%u,%u,hwnd=0x%X,beh=0x%X) %ux%u fmt=%u "
           "flags=0x%X [cfg %ux%u]",
           adapter, devType, hwnd, behavior, w, h, fmt, flags, g_cfg.width,
           g_cfg.height);
    if (ppDev) {
        *(u32 *)(uintptr_t)ppDev = (u32)(uintptr_t)g_fake_device;
    }
    sb_render_on_create_device(w, h); /* M2a: echtes Fenster + Swapchain */
    return 0; /* S_OK */
}


/* W52 IGNFIX: Gate Bit 2 (Engine-Toggle-Freigabe) als HLE-Ersatz setzen.
 * Befund: [0x6A37E0] Bit 2 hat in Mission 1 KEINEN Spiel-Schreiber (alle 91
 * Refs verifiziert; einziger absoluter Setter = Veteranen-Schwelle 0xDF96E,
 * ~25 h; C9D40-Bits sind missionsfremd; Restores propagieren nur 0) - der
 * Toggle 0x376BE (braucht zusaetzlich [0x53831C]==1, ckp==0, att!=1,
 * IGN-Halt + START-Puls, W52_t1c) kann ohne das Bit nie feuern. Bit-2-Leser:
 * nur Toggle + Veteran-Meldung (harmlos). Setzen, sobald die Mission laeuft
 * (mloop==1); das Spiel gatet den Rest selbst. Schalter ign_toggle_fix. */
static void ign_gate_tick(void)
{
    static int logged;
    u32 mloop = 0, gate = 0;
    if (!g_cfg.ign_toggle_fix) {
        return;
    }
    __try {
        mloop = *(volatile u32 *)(uintptr_t)SBA(0x356F58u);
        gate = *(volatile u32 *)(uintptr_t)SBA(0x6A37E0u);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (mloop != 1u || (gate & 4u) != 0u) {
        return;
    }
    __try {
        *(volatile u32 *)(uintptr_t)SBA(0x6A37E0u) = gate | 4u;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (!logged) {
        logged = 1;
        HLOG_I("IGNFIX: Gate Bit 2 gesetzt bei Frame %u (mloop=1)", g_frames);
    }
}

u32 __stdcall hook_D3DDevice_Swap(u32 flags)
{
    uint64_t now;
    (void)flags;
    sb_rtt_rb_sync_resolve(3); /* W215: spaetestens am Bildende */
    g_frames++;
    if (g_frames == 1) {
        QueryPerformanceFrequency((LARGE_INTEGER *)&g_qpf);
        QueryPerformanceCounter((LARGE_INTEGER *)&g_fps_start);
        HLOG_I("D3D Swap #1 (erster Frame!)");
    } else if ((g_frames % 120) == 0) {
        double dt;
        QueryPerformanceCounter((LARGE_INTEGER *)&now);
        dt = (double)(now - g_fps_start) / (double)g_qpf;
        HLOG_I("D3D Swap #%u (%.1f FPS)", g_frames, 120.0 / dt);
        g_fps_start = now;
    }
    sb_render_present(flags); /* M2a: Msg-Pump + echtes Present */
    diag_states_tick(g_frames); /* DIAG(STATES): Modus/Szene (A3, nur bei diag_states=1) */
    diag_input_tick(g_frames); /* DIAG(INPUT): SBC/Out/Flags (A8, nur bei input_diag=1) */
    diag_state_tick(g_frames); /* DIAG(STATE): Missionszustand 1x/s (A11c, nur bei state_diag=1) */
    diag_startup_tick(g_frames); /* DIAG(STARTUP): Balken/Linie/Engine (W21-R2, nur bei startup_diag=1) */
    diag_combat_tick(g_frames); /* DIAG(COMBAT): Entity-HP/Schaden (W35, nur bei combat_diag=1) */
    diag_campaign_tick(g_frames); /* DIAG(CAMPAIGN): Missionsziel/Phase (W63, nur bei campaign_diag=1) */
    diag_gear_tick(g_frames); /* DIAG(GEAR): Gang-Kette+Position (W141, nur bei gear_diag=1) */
    watch_dr_tick(g_frames); /* WATCHDR (W41): ent:-Reload bei Tabellenwechsel (nur bei watch=...) */
    replay_steps_tick(); /* A4: zustandsgesteuerte Eingaben */
    ign_gate_tick(); /* W52 IGNFIX: Gate Bit 2 (nur bei ign_toggle_fix=1) */
    /* DIAG(MEMSCAN): XBE-.data (0x279DC0-0x6ED17C, memory.tsv) alle N Frames
     * nach <out>/memscan/f_NNNNNN.bin -> scripts/memscan.py sucht Adressen,
     * die sich genau an Bildschirmwechseln aendern (Menuezustand fuer
     * mem:-Trigger, Aufgabe A1). ~4,6 MB pro Snapshot: nur gezielt nutzen. */
    if (g_cfg.memscan_every > 0 && g_frames % (u32)g_cfg.memscan_every == 0) {
        char path[600];
        FILE *f;
        extern const char *hcfg_out_dir(void);
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s/memscan", hcfg_out_dir());
        CreateDirectoryA(path, NULL);
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s/memscan/f_%06u.bin",
                    hcfg_out_dir(), g_frames);
        if (fopen_s(&f, path, "wb") == 0 && f) {
            __try {
                fwrite((const void *)(uintptr_t)SBA(0x279DC0u), 1,
                       SBA(0x6ED17Cu) - SBA(0x279DC0u), f);
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            fclose(f);
        }
    }
    if (g_cfg.rt_from_mission > 0 && g_cfg.vblank_hz <= 0) {
        /* TEST(POPIN,W91): Menues im Testtakt, Mission in Echtzeit (Szene
         * 0x2C [0x6B96DC], mloop [0x356F58] = 1 wie ip_mission, ohne Titel-Demo:
         * W232 sb_in_mission statt Ladezaehler [0x2EEA34] == 4). */
        extern void sb_timing_go_realtime(int hz);
        static u32 rt_m0;
        if ((g_cfg.rt_mission_any ? sb_state_mission_or_demo() : sb_in_mission()) && !rt_m0) { rt_m0 = g_frames; } /* W137: rt_mission_any = auch Titel-Demo; W232: sonst Spielermission statt Ladezaehler == 4 */
        if (rt_m0 && g_frames >= rt_m0 + (u32)(g_cfg.rt_delay > 0 ? g_cfg.rt_delay : 0)) {
            if (g_cfg.vblank_hz <= 0 && g_cfg.rt_interp_fps > 0) { /* TEST(PERF-BENCH,W137) */
                extern void sb_ovl_request_fps(int fps);
                sb_ovl_request_fps(g_cfg.rt_interp_fps);
            }
            sb_timing_go_realtime(g_cfg.rt_from_mission);
            if (g_cfg.rt_fps_limit > 0) { g_cfg.fps_limit = g_cfg.rt_fps_limit; }
        }
    }
    /* FPS-Limit (grob, M1). TURBO: bis turbo_until_frame ohne Limit
     * oder bis turbo_until_mode den Spielmodus erreicht (A1). Nur mit
     * zustandsgesteuertem replay_steps sinnvoll (T1/T2). */
    if (turbo_mode_active()) {
        return 0; /* Zeitraffer: kein fps_limit bis Modus-Ziel */
    }
    if (g_cfg.fps_limit > 0 &&
        (g_cfg.turbo_until_frame <= 0 ||
         g_frames >= (u32)g_cfg.turbo_until_frame)) {
        u32 budget = 1000u / (u32)g_cfg.fps_limit;
        u32 now_ms = GetTickCount();
        if (g_last_frame_ms && now_ms - g_last_frame_ms < budget) {
            Sleep(budget - (now_ms - g_last_frame_ms));
        }
        g_last_frame_ms = GetTickCount();
    }
    return 0;
}

u32 __stdcall hook_D3DDevice_Clear(u32 count, u32 rects, u32 flags, u32 color,
                                          u32 z, u32 stencil)
{
    static u32 calls;
    (void)count;
    (void)rects;
    if (hlog_throttle(&calls)) {
        HLOG_T("D3D Clear(flags=0x%X color=0x%08X)", flags, color);
    }
    { long long tbq = TB_BEGIN(); /* DIAG(TICK-PERF,W125) */
    sb_render_clear(flags, color, u2f(z), stencil); /* M2a: echt clearen */
    TB_END(TB_CLEAR, tbq); }
    return 0;
}

u32 __stdcall hook_D3DDevice_Begin(u32 prim)
{
    static u32 calls;
    if (hlog_throttle(&calls)) {
        HLOG_T("D3D Begin(%u)", prim);
    }
    sb_render_im_begin(prim); /* M2a: Statistik, Geometrie folgt (M2b) */
    return 0;
}

/* DIAG(DRAWSTORM): Spiel-Caller von End() fuer den Draw-Sturm-Detektor. */
unsigned g_im_end_caller;

u32 __stdcall hook_D3DDevice_End(void)
{
    g_im_end_caller = (unsigned)(uintptr_t)_ReturnAddress();
    static u32 calls;
    if (hlog_throttle(&calls)) {
        HLOG_T("D3D End()");
    }
    sb_render_im_end();
    return 0;
}

u32 __stdcall hook_D3DDevice_SetTexture(u32 stage, u32 tex)
{
    static u32 calls;
    if (hlog_throttle(&calls)) {
        HLOG_T("D3D SetTexture(%u, 0x%X)", stage, tex);
    }
    sb_tex_set(stage, tex); /* M2b: Textur-Cache */
    return 0;
}

static u32 g_cur_vs; /* FIX(COCKPIT,W60): zuletzt gesetzter VS-Handle (GetVertexShader) */
#define VS_MIRROR_BASE_W60 SBA(0x21BCA8u) /* = VS_MIRROR_BASE (unten), Original-Device 0x21B050+0xC58 */
u32 __stdcall hook_D3DDevice_SetVertexShader(u32 handle)
{
    static u32 calls;
    if (hlog_throttle(&calls)) {
        HLOG_T("D3D SetVertexShader(0x%X)", handle);
    }
    /* VSH-Dump (vsh_dis.py): Mikrocode je Handle einmal (NV2A-VSH).
     * Region ab base+0x114 = [Chunk-Header][bis zu 8 Slots a 4 DWORDs],
     * wiederholt; Header 0x00NN0B00 mit NN = 16 * Slots im Chunk.
     * progsize = Slot-Anzahl, dwords = Regionsgroesse in DWORDs. */
    if (handle && g_cfg.vsh_dump) {
        static u32 seen_vs[64];
        int k, found = 0;
        for (k = 0; k < 64 && seen_vs[k]; k++) {
            if (seen_vs[k] == handle) { found = 1; break; }
        }
        if (!found && k < 64) {
            u32 base = handle & ~1u;
            seen_vs[k] = handle;
            if (base >= 0x10000u) {
            __try {
                u32 *hd = (u32 *)(uintptr_t)base;
                unsigned si = 0, ri = 0, cc = 0, nslots = hd[2], nwords = hd[3];
                u32 *rg = (u32 *)(uintptr_t)(base + 0x114u);
                if (nslots > 136u) { nslots = 136u; }
                if (nwords < 1u || nwords > 1024u) { nwords = 1024u; }
                HLOG_W("VSH vs=0x%X: progsize=%u dwords=%u",
                       handle, hd[2], hd[3]);
                while (si < nslots && ri < nwords) {
                    unsigned cn, k;
                    HLOG_W("VSH vs=0x%X ch[%u]: %08X", handle, cc, rg[ri]);
                    cn = (rg[ri] >> 16) & 0xFFu;
                    ri++; cc++;
                    cn = (cn >= 16u && cn <= 128u && (cn % 16u) == 0) ? (cn / 16u) : 8u;
                    if (cn > nslots - si) { cn = nslots - si; }
                    for (k = 0; k < cn && ri + 4u <= nwords; k++) {
                        HLOG_W("VSH vs=0x%X [%02u]: %08X %08X %08X %08X",
                               handle, si, rg[ri], rg[ri + 1u], rg[ri + 2u], rg[ri + 3u]);
                        ri += 4u; si++;
                    }
                    if (k < cn) { break; }
                }
                HLOG_W("VSH vs=0x%X: %u Slots (progsize=%u dwords=%u)",
                       handle, si, hd[2], hd[3]);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                HLOG_W("VSH vs=0x%X: Struct nicht lesbar", handle);
            }
            }
        }
    }
    if (!handle) { /* DIAG(COCKPIT,W60): wer setzt VS=0 (Restore aus GetVertexShader)? */
        sb_render_drawdrop(5, 0, 0, (unsigned)(uintptr_t)_ReturnAddress());
    }
    g_cur_vs = handle; /* FIX(COCKPIT,W60): Stand fuer GetVertexShader */
    sb_render_im_shader(handle); /* M2b: Pfad-Wahl (RHW vs. 3D) */
    return 0;
}

/* FIX(COCKPIT,W60): Get*-Shader-Stand. Das Spiel sichert vor Sonder-Materialien
 * (Entity-Renderer FUN_00023d70, [mat+0x26]=1..3; ebenso 0x24410) VS/PS und
 * VS-Konstanten c13/c14 per GetVertexShader (0x20F4C0) / GetPixelShader
 * (0x20FCB0) / GetVertexShaderConstant (0x20F710) nach 0x3563B8/0x35466C/
 * 0x357DE0 und stellt sie nach dem Draw wieder her (0x24183: SetVertexShader
 * ([0x3563B8]), SetVertexShaderConstantNotInline(c13, ..., 2)). Die Originale
 * lesen das Device [0x21A8A0] (+0x384/+0x370/+0x1258+r*16); das ist hier das
 * Fake-Device (Nullen), weil die Set-Hooks es nie beschreiben -> Restore setzte
 * VS=0 (Folge-VB-Draws ohne Decl verworfen) und nullte c13/c14. Original-
 * Device = 0x21B050 (CreateDevice 0x210B63): +0x1258 = 0x21BCA8 + 96*16, d. h.
 * GetVertexShaderConstant(r) liest den Spiegel 0x21BCA8 an (r+96)*16, den die
 * VSConst-Hooks (vsconst_mirror=1) und SetVertexShaderConstantNotInline pflegen. */
u32 __stdcall hook_D3DDevice_GetVertexShader(u32 pOut)
{
    __try { if (pOut) { *(u32 *)(uintptr_t)pOut = g_cur_vs; } }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (g_cfg.drawdrop_diag && sb_render_census_on()) {
        extern unsigned g_present_no;
        HLOG_W("GETVS f=%u -> 0x%X caller=0x%X", g_present_no, g_cur_vs,
               (unsigned)(uintptr_t)_ReturnAddress());
    }
    return 0;
}
u32 __stdcall hook_D3DDevice_GetPixelShader(u32 pOut)
{
    extern unsigned g_im_pshader;
    __try { if (pOut) { *(u32 *)(uintptr_t)pOut = g_im_pshader; } }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}
u32 __stdcall hook_D3DDevice_GetVertexShaderConstant(u32 reg, u32 pOut,
                                                           u32 count)
{
    int r = (int)reg + 96; /* Register -96..95 -> Spiegel-Index 0..191 */
    if (r < 0 || count == 0 || (unsigned)r + count > 192u || !pOut) {
        return 0;
    }
    __try {
        memcpy((void *)(uintptr_t)pOut,
               (const void *)(uintptr_t)(VS_MIRROR_BASE_W60 + (u32)r * 16u),
               count * 16u);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}

/* DIAG(TEX3D,W31): letzter Pixelshader + Combiner-Stand (Zensus liest). */
unsigned g_im_pshader, g_npshader;
unsigned g_tss_n, g_tss_a, g_tss_b, g_tss_c;
u32 __stdcall hook_D3DDevice_SetPixelShader(u32 handle)
{
    static u32 calls;
    if (hlog_throttle(&calls)) {
        HLOG_T("D3D SetPixelShader(0x%X)", handle);
    }
    g_im_pshader = handle; g_npshader++;
    { extern unsigned g_rq_psconst_set; extern unsigned long long g_rq_rs_set; g_rq_psconst_set = 0; g_rq_rs_set = 0; } /* RENDER-QA: SetPixelShader laedt die Def-Werte */
    return 0;
}

/* RTT (W23): Echte Adresse 0x20E370 (M2-Hook @0x20E8D0 lag mitten in
 * D3DDevice_SetTexture und feuerte nie). Repliziert den CPU-sichtbaren
 * Original-Zustand ([dev+0x21B4]/[dev+0x21B8], keine Refcount-Aenderung
 * in D3D_CommonSetRenderTarget) und meldet an sb_rtt (Notiz immer,
 * Umleitung bei rtt=1). Fake-Surface aus GetBackBuffer2 = Backbuffer. */
u32 __stdcall hook_D3DDevice_SetRenderTarget(u32 surf, u32 depth)
{
    unsigned caller = (unsigned)(uintptr_t)_ReturnAddress();
    /* RTT-DIAG: Titel-Kompositions-Zaehler [0x4C19C0] (FUN_0004ef50 malt nur
     * bei >0, je Tabelleneintrag Draw-Helper-Aufrufe). Aenderung loggen. */
    __try {
        static unsigned short lastcx;
        static int havecx;
        static unsigned ncx;
        unsigned short cx = *(unsigned short *)(uintptr_t)SBA(0x4C19C0u);
        if (!havecx || cx != lastcx) {
            extern unsigned g_present_no;
            havecx = 1; lastcx = cx;
            if (ncx < 40) {
                ncx++;
                HLOG_I("RTT cnt #%u: [0x4C19C0]=%u f=%u", ncx, cx,
                       g_present_no);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try {
        u32 dev = *(u32 *)(uintptr_t)SBA(0x21A8A0u);
        if (dev) {
            *(u32 *)(uintptr_t)(dev + 0x21B4u) = surf;
            *(u32 *)(uintptr_t)(dev + 0x21B8u) = depth;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (surf == (u32)(uintptr_t)g_fake_surface) {
        static unsigned nfk;
        if (nfk < 2) {
            nfk++;
            HLOG_I("D3D SetRenderTarget: Fake-Surface -> Backbuffer");
        }
        sb_rtt_on_set_target(0, depth, caller);
    } else {
        long long tbq = TB_BEGIN(); /* DIAG(TICK-PERF,W125) */
        sb_rtt_on_set_target(surf, depth, caller);
        TB_END(TB_RTT, tbq);
    }
    return 0;
}
u32 __stdcall hook_D3DDevice_GetBackBuffer2(u32 idx)
{
    static u32 calls;
    if (hlog_throttle(&calls)) {
        HLOG_T("D3D GetBackBuffer2(%d)", (int)idx);
    }
    /* RTT (W23): echte BB-Surface notieren (Slot-Logik wie Original 0x20E640:
     * idx -1 -> Slot 1, 0 -> Slot 0, sonst -> Slot 2; [dev+0x21C0+slot*4]).
     * Rueckgabe bleibt Fake (keine Verhaltensaenderung). */
    __try {
        u32 dev = *(u32 *)(uintptr_t)SBA(0x21A8A0u);
        if (dev) {
            int ii = (int)idx;
            unsigned slot = (ii == -1) ? 1u : (ii == 0 ? 0u : 2u);
            u32 real = *(u32 *)(uintptr_t)(dev + 0x21C0u + slot * 4u);
            if (real) { sb_rtt_note_backbuffer(real); }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return (u32)(uintptr_t)g_fake_surface;
}

u32 __fastcall hook_D3DDevice_SetRenderState_Simple(u32 state, u32 value)
{
    /* XBLEND: ECX = NV2A-Methodenwort (Count<<18 | Methode), z. B. 0x40364 =
     * STENCIL_FUNC; Blend = 0x304/0x344/0x348 mit GL-Enums. */
    if (g_cfg.xbox_blend) {
        u32 m = state & 0x1FFCu;
        if (m == 0x304u || m == 0x344u || m == 0x348u || m == 0x350u || m == 0x34Cu) { /* W90: + BLEND_EQUATION/-COLOR */
            sb_render_blend_nv(m, value);
        }
    }
    fx_nv_note(state, value); /* FX-ORIG (W90): alle NV-Methoden spiegeln (Alpha-Test, Z-Func, Blend-Gleichung, ...) */
    /* COLORMASK (W33): Masken/Stencil mitschreiben (immer billig,
     * Anwendung hinter rs_colormask/rs_stencil). */
    { u32 m2 = state & 0x1FFCu;
      if (m2 == 0x358u || m2 == 0x35Cu || m2 == 0x32Cu ||
          (m2 >= 0x360u && m2 <= 0x378u && (m2 & 3u) == 0u)) {
          sb_render_rs_note(m2, value);
      } }
    /* SHADE-PERF (W80, Befund W77-Rest): Combiner-Laufzeitkonstanten kommen als
     * NV2A-Methoden ueber den Simple-Pfad (PSCONSTANT0_k -> SET_COMBINER_FACTOR0
     * 0x0A60+4k, PSCONSTANT1_k -> FACTOR1 0x0A80+4k, Final-Konstanten ->
     * SET_SPECULAR_FOG_FACTOR 0x1E20/0x1E24). Zaehlen (DIAG rq_perf) und mit
     * shade_pbconst=1 in den PS-State-Spiegel (Index wie D3DRS 10../18../43/44). */
    if (g_cfg.shading) {
        u32 m3 = state & 0x1FFCu;
        int ri = -1;
        if (m3 >= 0xA60u && m3 <= 0xA7Cu) { ri = 10 + (int)((m3 - 0xA60u) >> 2); }
        else if (m3 >= 0xA80u && m3 <= 0xA9Cu) { ri = 18 + (int)((m3 - 0xA80u) >> 2); }
        else if (m3 == 0x1E20u) { ri = 43; }
        else if (m3 == 0x1E24u) { ri = 44; }
        if (ri >= 0) {
            extern unsigned g_rq_rs[64];
            extern unsigned long long g_rq_rs_set;
            extern unsigned g_rq_pbc_calls[64], g_rq_pbc_last[64];
            g_rq_pbc_calls[ri]++;
            g_rq_pbc_last[ri] = value;
            if (g_cfg.shade_pbconst) { g_rq_rs[ri] = value; g_rq_rs_set |= 1ull << (unsigned)ri; }
        }
    }
    if (seen_state(state)) {
        HLOG_T("D3D SetRenderState(%u, 0x%X)", state, value);
    }
    return 0;
}

u32 __stdcall hook_D3DDevice_SetVertexData2f(u32 reg, u32 a, u32 b)
{
    static u32 calls;
    if (hlog_throttle(&calls)) {
        HLOG_T("D3D SetVertexData2f(%u)", reg);
    }
    sb_render_im_vdata(reg, u2f(a), u2f(b), 0.0f, 1.0f);
    return 0;
}

u32 __stdcall hook_D3DDevice_SetVertexData4f(u32 reg, u32 a, u32 b, u32 c,
                                                    u32 d)
{
    static u32 calls;
    if (hlog_throttle(&calls)) {
        HLOG_T("D3D SetVertexData4f(%u)", reg);
    }
    sb_render_im_vdata(reg, u2f(a), u2f(b), u2f(c), u2f(d));
    return 0;
}

u32 __stdcall hook_D3DDevice_SetVertexDataColor(u32 reg, u32 color)
{
    static u32 calls;
    if (hlog_throttle(&calls)) {
        HLOG_T("D3D SetVertexDataColor(%u, 0x%X)", reg, color);
    }
    sb_render_im_color(reg, color);
    return 0;
}

/* RTT (W23): GetRenderTarget2 (0x20E690, 0 Args) mitloggen + Original exakt
 * replizieren (AddRef via echten 0x210F30-Aufruf, SEH-geschuetzt). Zeigt,
 * womit das Spiel sichert/restauriert (BB-Erkennung) und ob dev lebt. */
u32 __stdcall hook_D3DDevice_GetRenderTarget2(void)
{
    u32 rt = 0;
    __try {
        u32 dev = *(u32 *)(uintptr_t)SBA(0x21A8A0u);
        if (dev) { rt = *(u32 *)(uintptr_t)(dev + 0x21B4u); }
    } __except (EXCEPTION_EXECUTE_HANDLER) { rt = 0; }
    if (rt) {
        typedef u32 (__stdcall *addref_fn)(u32);
        __try { ((addref_fn)(uintptr_t)SBA(0x210F30u))(rt); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return rt;
}
u32 __stdcall hook_D3DDevice_SetViewport(u32 pVp)
{
    static u32 calls;
    if (pVp) {
        /* X_D3DVIEWPORT: X@0 Y@4 W@8 H@12 MinZ@16 MaxZ@20 (floats). */
        sb_viewport vp;
        vp.x = *(u32 *)(uintptr_t)(pVp + 0);
        vp.y = *(u32 *)(uintptr_t)(pVp + 4);
        vp.w = *(u32 *)(uintptr_t)(pVp + 8);
        vp.h = *(u32 *)(uintptr_t)(pVp + 12);
        vp.minz = u2f(*(u32 *)(uintptr_t)(pVp + 16));
        vp.maxz = u2f(*(u32 *)(uintptr_t)(pVp + 20));
        sb_render_set_viewport(&vp); /* M2a: Spiel-Viewport merken */
        if (hlog_throttle(&calls)) {
            HLOG_I("D3D SetViewport(%u,%u %ux%u)", vp.x, vp.y, vp.w, vp.h);
        }
    }
    return 0;
}

u32 __stdcall hook_D3DDevice_SetTransform(u32 state, u32 pMat)
{
    static u32 calls;
    if (hlog_throttle(&calls)) {
        HLOG_T("D3D SetTransform(%u)", state);
    }
    /* M2a: Kamera getrennt erfassen (View vs. Projektion vs. Welt).
     * MISSION3D (W25): Xbox-Enum (xbox_transform=1): VIEW=0, PROJ=1,
     * WORLD0-3=6-9. Alter Pfad (0): PC-Enum (traf nie, Matrizen null). */
    { unsigned cls = (state < 10u) ? state : ((state >= 256u && state < 260u) ? 10u : 11u);
      extern void sb_render_note_st(unsigned cls);
      sb_render_note_st(cls); /* M3D-Zensus: Transform-Statistik */
    }
    if (pMat) {
        const float *m = (const float *)(uintptr_t)pMat;
        if (g_cfg.xbox_transform) {
            if (state == XX_D3DTS_VIEW) {
                sb_render_set_view(m);
            } else if (state == XX_D3DTS_PROJECTION) {
                sb_render_set_proj(m);
            } else if (state >= XX_D3DTS_WORLD0 && state < XX_D3DTS_WORLD0 + 4) {
                sb_render_set_world((int)(state - XX_D3DTS_WORLD0), m);
            }
        } else if (state == XB_D3DTS_VIEW) {
            sb_render_set_view(m);
        } else if (state == XB_D3DTS_PROJECTION) {
            sb_render_set_proj(m);
        } else if (state >= XB_D3DTS_WORLD && state < XB_D3DTS_WORLD + 4) {
            sb_render_set_world((int)(state - XB_D3DTS_WORLD), m);
        }
    }
    return 0;
}

/* RELEASEFIX (31.10.): Xbox-Semantik = Refcount (Common & 0xFFFF)
 * dekrementieren, neuen Wert zurueckgeben. Vorher fest 1 -> Spiel-Schleife
 * 0x20300 "while (Release(res) != 0)" lief endlos (B1: Stall nach 12 min
 * Mission). Speicher wird weiter NICHT freigegeben (sicher, leakt nur);
 * bei 0 Cache-Eintrag entfernen (TEXLIFE, tex_evict=1) bzw. dirty
 * markieren (tex_evict=0). res_refcount=0 = altes Verhalten. */
u32 __stdcall hook_D3DResource_Release(u32 res)
{
    static u32 calls;
    u32 rc = 1;
    if (g_cfg.res_refcount && res) {
        __try {
            u32 c = *(u32 *)(uintptr_t)res;
            rc = c & 0xFFFFu;
            if (rc) {
                rc--;
                *(u32 *)(uintptr_t)res = (c & ~0xFFFFu) | rc;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            rc = 0;
        }
        if (rc == 0) {
            if (g_cfg.tex_evict) { sb_tex_evict(res); } else { sb_tex_mark_dirty(res); }
            sb_tex_lockwatch_forget(res); /* W98 */
            sb_rem_forget(res); /* W103 */
            sb_rtt_release_surf(res); /* RTT-Notiz bei Release rc=0 loeschen */
            sb_rtt_release_data(res); /* W124 rtt_stale: gleiche Datenadresse -> RT-Eintrag veraltet */
        }
    }
    if (hlog_throttle(&calls)) {
        HLOG_T("D3D D3DResource_Release(0x%X) -> %u", res, rc);
    }
    return rc;
}

/* D3DDevice_SetRenderState/SetTextureState-Familie (XDK-D3D8, statisch
 * gelinkt, RET-N je Funktion per Disasm verifiziert). M1: No-Op (S_OK),
 * Werte bei Aenderung loggen (M2-Tuning: Fog/Sichtweite, Culling etc.). */
#define D3D_STATE1(name) \
    u32 __stdcall hook_D3D_##name(u32 v) \
    { \
        static u32 last; \
        static int have; \
        if (!have || v != last) { \
            have = 1; \
            last = v; \
            HLOG_T("D3D " #name "(0x%X)", v); \
        } \
        return 0; \
    }
#define D3D_STATE2(name) \
    u32 __stdcall hook_D3D_##name(u32 a, u32 b) \
    { \
        static u32 la, lb; \
        static int have; \
        if (!have || a != la || b != lb) { \
            have = 1; \
            la = a; \
            lb = b; \
            HLOG_T("D3D " #name "(0x%X, 0x%X)", a, b); \
        } \
        return 0; \
    }
#define D3D_STATE3(name) \
    u32 __stdcall hook_D3D_##name(u32 a, u32 b, u32 c) \
    { \
        static u32 la, lb, lc; \
        static int have; \
        if (!have || a != la || b != lb || c != lc) { \
            have = 1; \
            la = a; \
            lb = b; \
            lc = c; \
            HLOG_T("D3D " #name "(0x%X, 0x%X, 0x%X)", a, b, c); \
        } \
        return 0; \
    }

/* Generischer (State, Wert)-Weg: Blend-States (19/20/27) anwenden. */
u32 __stdcall hook_D3D_SetRenderStateNotInline(u32 a, u32 b)
{
    static u32 la, lb;
    static int have;
    if (!have || a != la || b != lb) {
        have = 1;
        la = a;
        lb = b;
        HLOG_T("D3D SetRenderStateNotInline(0x%X, 0x%X)", a, b);
    }
    { /* RENDER-QA (W77): Pixelshader-States (0..56: PS-Inputs/-Outputs,
       * PSCONSTANT0_0=10.., PSCONSTANT1_0=18.., FINALCOMBINERCONSTANT0/1=43/44)
       * mitschreiben - das Spiel setzt die Combiner-Konstanten per
       * SetRenderState statt SetPixelShaderConstant. Nur Spiegel. */
        extern unsigned g_rq_rs[64];
        extern unsigned long long g_rq_rs_set;
        extern unsigned g_rq_rs_calls;
        if (a < 57u) { g_rq_rs[a] = b; g_rq_rs_set |= 1ull << a; g_rq_rs_calls++; }
    }
    return 0;
}
D3D_STATE1(SetRenderState_EdgeAntiAlias)
/* RENDER-QA3 (W100): D3DRS_SHADOWFUNC (D3DCMP 0x200..0x207, NV 0x1E6C = Wert - 0x200)
 * merken - das Spiel setzt GREATER (0x452BD) vor der Welt und NEVER (0x45953) danach. */
unsigned g_rq_shadowfunc = 0xFFFFFFFFu;
u32 __stdcall hook_D3D_SetRenderState_ShadowFunc(u32 v)
{
    static u32 last;
    static int have;
    if (!have || v != last) {
        have = 1;
        last = v;
        HLOG_T("D3D SetRenderState_ShadowFunc(0x%X)", v);
    }
    g_rq_shadowfunc = v;
    return 0;
}
/* RENDER-QA (W77): FogColor zusaetzlich merken (Nebel im Final-Combiner). */
u32 __stdcall hook_D3D_SetRenderState_FogColor(u32 v)
{
    static u32 last;
    static int have;
    extern unsigned g_rq_fogcolor, g_rq_fogcolor_set;
    if (!have || v != last) {
        have = 1;
        last = v;
        HLOG_T("D3D SetRenderState_FogColor(0x%X)", v);
    }
    g_rq_fogcolor = v;
    g_rq_fogcolor_set = 1;
    return 0;
}
/* MISSION3D (W25): CullMode zusaetzlich merken (Zensus liest ihn). */
u32 __stdcall hook_D3D_SetRenderState_CullMode(u32 v)
{
    static u32 last;
    static int have;
    if (!have || v != last) {
        have = 1;
        last = v;
        HLOG_T("D3D SetRenderState_CullMode(0x%X)", v);
    }
    sb_render_note_cullmode(v);
    return 0;
}
D3D_STATE1(SetRenderState_TextureFactor)
D3D_STATE1(SetRenderState_LineWidth)
D3D_STATE1(SetRenderState_Dxt1NoiseEnable)
D3D_STATE2(SetTextureState_TexCoordIndex)
D3D_STATE3(SetTextureState_BumpEnv)
D3D_STATE2(SetTextureState_BorderColor)
D3D_STATE2(SetTextureState_ColorKeyColor)
/* DIAG(TEX3D,W31): Combiner-Weg mit Stand (statt D3D_STATE3-Makro). */
u32 __stdcall hook_D3D_SetTextureStageStateNotInline(u32 a, u32 b, u32 c)
{
    static u32 la, lb, lc;
    static int have;
    g_tss_n++; g_tss_a = a; g_tss_b = b; g_tss_c = c;
    if (!have || a != la || b != lb || c != lc) {
        have = 1;
        la = a;
        lb = b;
        lc = c;
        HLOG_T("D3D SetTextureStageStateNotInline(0x%X, 0x%X, 0x%X)", a, b, c);
    }
    return 0;
}
/* MISSION3D (W25): ZEnable zusaetzlich merken (Zensus liest ihn). */
u32 __stdcall hook_D3D_SetRenderState_ZEnable(u32 v)
{
    static u32 last;
    static int have;
    if (!have || v != last) {
        have = 1;
        last = v;
        HLOG_T("D3D SetRenderState_ZEnable(0x%X)", v);
    }
    sb_render_note_zenable(v);
    return 0;
}
/* COLORMASK (W33): StencilEnable zusaetzlich spiegeln (Renderer liest). */
u32 __stdcall hook_D3D_SetRenderState_StencilEnable(u32 v)
{
    static u32 last;
    static int have;
    if (!have || v != last) {
        have = 1;
        last = v;
        HLOG_T("D3D SetRenderState_StencilEnable(0x%X)", v);
    }
    sb_render_rs_stencil_enable(v);
    return 0;
}
/* COLORMASK (W33): StencilFail (0x20D060, ret 4, schreibt 0x370 + 0x1D84
 * inline). Vom Spiel nie aufgerufen (statisch belegt), Hook der
 * Vollstaendigkeit halber (Spiegel + No-Op-Rueckgabe wie Original). */
u32 __stdcall hook_D3D_SetRenderState_StencilFail(u32 v)
{
    static u32 last;
    static int have;
    if (!have || v != last) {
        have = 1;
        last = v;
        HLOG_T("D3D SetRenderState_StencilFail(0x%X)", v);
    }
    sb_render_rs_note(0x370u, v);
    return 0;
}
D3D_STATE1(SetRenderState_OcclusionCullEnable)

/* FUN_00229AB9: XDK-D3D8 GPU-Bringup (NV2A-MMIO 0xFE80xxxx: PGRAPH-Status
 * lesen/pollen, FIFO schreiben). Im Harness kein NV2A mappbar (Win32-User
 * kann 0xFE80xxxx nicht einblenden) -> M1: idle-GPU antworten. Original
 * gibt bei idle-GPU (Register=0) 0 zurueck (Pfad ueber 0x229B88). */
u32 __stdcall hook_GPU_229AB9(u32 arg)
{
    static u32 calls;
    (void)arg;
    if (hlog_throttle(&calls)) {
        HLOG_T("D3D GPU-Bringup(0x229AB9) -> 0 (idle, kein NV2A-MMIO)");
    }
    return 0;
}

/* FUN_0021E14F: NV2A-Register-Getter ([0x23FD58]!=0 -> EAX=[0xFE80200C]).
 * M1: 0 (wie Flag=0-Pfad, kein NV2A-MMIO). */
u32 __stdcall hook_GPU_regread0(void)
{
    static u32 calls;
    if (hlog_throttle(&calls)) {
        HLOG_T("D3D GPU-RegRead(0x21E14F) -> 0 (kein NV2A-MMIO)");
    }
    return 0;
}

/* NV2A-Register-RMW (XDK-D3D8, je 1 u32-Arg, RET 0x4, Rueckgabe ignoriert):
 * 0x2294F2 ([0xFE801004]), 0x229529 ([0xFE801510]), 0x229560 ([0xFE801100]),
 * 0x2295BB ([0xFE802000]). M1: No-Op (kein NV2A-MMIO im Harness). */
u32 __stdcall hook_GPU_rmw(u32 arg)
{
    static u32 calls;
    if (hlog_throttle(&calls)) {
        HLOG_T("D3D GPU-RMW(arg=0x%X) -> No-Op", arg);
    }
    return 0;
}

/* Get2DSurfaceDesc (0x212820, 3 Args (surface, level, out_desc), RET 0xC):
 * Echtes Xbox-Layout (Cxbx X_D3DSURFACE_DESC, 7 Dwords, KEIN Pool-Feld!):
 * Format@0 Type@4 Usage@8 Size@12 MS@16 Width@20 Height@24. Bytes 28+
 * gehoeren dem Aufrufer (darunter ein Funktionspointer!) — NICHT anfassen.
 * M2a: 640x480x4-Surface beschreiben + S_OK. Echte Textur-Descs mit M2b. */
u32 __stdcall hook_Get2DSurfaceDesc(u32 surf, u32 level, u32 desc)
{
    static u32 calls;
    (void)level;
    if (desc) {
        u32 *d = (u32 *)(uintptr_t)desc;
        u32 fmt = 7;
        if (surf) {
            __try {
                fmt = *(uint8_t *)(uintptr_t)(surf + 0xD); /* Format-Byte */
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                fmt = 7;
            }
        }
        d[0] = fmt;          /* Format */
        d[1] = 1;            /* Type: SURFACE */
        d[2] = 0;            /* Usage */
        d[3] = 640 * 480 * 4; /* Size */
        d[4] = 0;            /* MultiSampleType */
        d[5] = 640;          /* Width */
        d[6] = 480;          /* Height */
    }
    if (hlog_throttle(&calls)) {
        HLOG_T("D3D Get2DSurfaceDesc -> 640x480 fmt=7");
    }
    return 0;
}

/* D3DVertexBuffer_Lock2 (0x2111C0, 2 Args (pThis,flags), RET 8):
 * OOVPA-identifiziert (Cxbx 4627, 13-Byte-Pattern, genau 1 Treffer) + per
 * Disasm verifiziert (beide Caller 0x605D7/0xC81AB: push 0; push struct;
 * call; REP MOVSD nach EAX = Lock2 gibt Schreibzeiger zurueck). War als
 * "PoolAlloc" verkannt (Ring) -> VB-Fills gingen verloren, Struct blieb 0
 * (boot321). Original gibt [pThis+4]|0x80000000 (WC-Alias, existiert am
 * PC nicht); wir geben [pThis+4] pur zurueck. Pushbuffer-Seiteneffekt
 * (8-Byte-Method-Header, nur ohne READONLY) entfaellt (keine GPU). */
uint8_t g_d3d_pool[8 * 1024 * 1024];
static u32 g_d3d_pool_off;
u32 __stdcall hook_D3D_VBLock2(u32 pThis, u32 flags)
{
    static u32 calls;
    u32 data = 0;
    if (pThis) {
        __try {
            data = ((u32 *)(uintptr_t)pThis)[1];
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            data = 0;
        }
    }
    if (hlog_throttle(&calls)) {
        HLOG_T("D3D VBLock2(0x%X,0x%X)", pThis, flags);
    }
    return data;
}

/* D3D_SetTileNoWait (0x20EBC0, 2 Args, RET 8): NV2A-Tiling (Speicherlayout
 * des echten GPUs). M2a: No-Op — D3D11 verwaltet Speicher selbst. */
u32 __stdcall hook_D3D_SetTileNoWait(u32 a, u32 b)
{
    static u32 calls;
    (void)a;
    (void)b;
    if (hlog_throttle(&calls)) {
        HLOG_T("D3D SetTileNoWait -> No-Op");
    }
    return 0;
}
/* Shader-Konstanten (MISSION3D/W25, vorher No-Op M2b): Alle drei nehmen
 * ECX=Startregister, EDX=Daten (fastcall-artig). VSConst1 (0x20F4E0,
 * cdecl/ret) schreibt 1 Register (16 B), VSConst4 (0x20F540, cdecl/ret)
 * 4 Register (64 B), VSConstFast (0x20F5F0, ret 4) N dwords (1 Stack-
 * Arg). VSConst1/4 spiegeln zusaetzlich CPU-seitig nach 0x21BCA8+r*16
 * (s. Disasm), Fast nicht. Die Hooks replizieren den CPU-sichtbaren
 * Original-Zustand (vsconst_mirror=1, Default) und zaehlen (DIAG).
 * Naked (M2b-Befund: Compiler-Hook korrumpiert ECX/EDX/MMX). */
#define VS_MIRROR_BASE SBA(0x21BCA8u)
#define VS_MIRROR_NREG 192u
static unsigned g_nvc1, g_nvc4, g_nvcf;
static void __cdecl vsconst_mirror_write(unsigned kind, unsigned reg,
                                         const void *src, unsigned ndwords)
{
    if (kind == 1u) { g_nvc1++; } else if (kind == 4u) { g_nvc4++; }
    else { g_nvcf++; }
    if (!g_cfg.vsconst_mirror || !src) { return; }
    if (reg >= VS_MIRROR_NREG) { return; }
    if (ndwords > (VS_MIRROR_NREG - reg) * 4u) {
        ndwords = (VS_MIRROR_NREG - reg) * 4u;
    }
    if (!ndwords) { return; }
    __try {
        memcpy((void *)(uintptr_t)(VS_MIRROR_BASE + reg * 16u), src,
               ndwords * 4u);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
void hooks_vsconst_stats(unsigned *c1, unsigned *c4, unsigned *cf)
{
    if (c1) { *c1 = g_nvc1; }
    if (c4) { *c4 = g_nvc4; }
    if (cf) { *cf = g_nvcf; }
}
__declspec(naked) u32 __cdecl hook_D3D_VSConst1(void)
{
    __asm {
        push ebp
        mov ebp, esp
        push ebx
        push esi
        push edi
        push edx
        push ecx
        push 4
        push edx
        push ecx
        push 1
        call vsconst_mirror_write
        add esp, 16
        pop ecx
        pop edx
        pop edi
        pop esi
        pop ebx
        pop ebp
        xor eax, eax
        ret
    }
}
__declspec(naked) u32 __cdecl hook_D3D_VSConst4(void)
{
    __asm {
        push ebp
        mov ebp, esp
        push ebx
        push esi
        push edi
        push edx
        push ecx
        push 16
        push edx
        push ecx
        push 4
        call vsconst_mirror_write
        add esp, 16
        pop ecx
        pop edx
        pop edi
        pop esi
        pop ebx
        pop ebp
        xor eax, eax
        ret
    }
}
__declspec(naked) u32 __stdcall hook_D3D_VSConstFast(void)
{
    __asm {
        push ebp
        mov ebp, esp
        mov eax, dword ptr [ebp + 8]
        push ebx
        push esi
        push edi
        push edx
        push ecx
        push eax
        push edx
        push ecx
        push 5
        call vsconst_mirror_write
        add esp, 16
        pop ecx
        pop edx
        pop edi
        pop esi
        pop ebx
        pop ebp
        xor eax, eax
        ret 4
    }
}

/* PSConst ist __stdcall RET 0xC (3 Args, Ausgang 0x2104A4). */
u32 __stdcall hook_D3D_PSConst(u32 reg, u32 pData,
                                     u32 count)
{
    static u32 calls;
    (void)reg;
    (void)pData;
    (void)count;
    if (hlog_throttle(&calls)) {
        HLOG_T("D3D PSConst(reg=%u n=%u)", reg, count);
    }
    /* FIX(PSCONST,W84): Dieser Hook liegt (hooks_install, spaeter installiert)
     * ueber dem RENDER-QA-Mitschreiber (W77, ebenfalls 0x210220) und hat ihn
     * bisher verschluckt -> "SetPixelShaderConstant: 0 Aufrufe" (W77/W80) war
     * ein Messfehler. shading=1 + shade_psconst=1: in den Spiegel uebernehmen;
     * rq_diag: nur zaehlen/loggen (DIAG ohne Bildwirkung). */
    if (g_cfg.bloom_diag) { /* DIAG(SHADE-M2,W117): Aufrufer im Spiel */
        extern unsigned g_psc_ra;
        g_psc_ra = (unsigned)(uintptr_t)_ReturnAddress();
    }
    if ((g_cfg.shading && g_cfg.shade_psconst) || g_cfg.rq_diag) {
        extern unsigned __stdcall hook_D3DDevice_SetPixelShaderConstant(unsigned, unsigned, unsigned);
        return hook_D3DDevice_SetPixelShaderConstant(reg, pData, count);
    }
    return 0;
}

/* D3D_MakeRequestedSpace_8 (0x211810, RET 8): Pushbuffer-Platz.
 * Original gibt EAX=Put (Caller 0x20CD33 schreibt Methoden dorthin!).
 * Return 0 -> Fault [0] (boot312, Swap #10920). M2b: Ring-Bump aus dem
 * Pool zurueckgeben + Spiel-Put/Limit weiter zuruecksetzen. */
u32 __stdcall hook_D3D_MakeSpace(u32 a, u32 b)
{
    static u32 calls;
    u32 base, size, off;
    (void)b;
    /* M2b: SPIEL-Put/Limit ([0x21B050/54]) auf Pool (C4 schreibt dorthin,
     * nicht ins Fake-Device!). */
    base = (u32)(uintptr_t)g_d3d_pool;
    *(u32 *)(uintptr_t)SBA(0x21B050u) = base;
    *(u32 *)(uintptr_t)SBA(0x21B054u) = base + (u32)sizeof g_d3d_pool;
    *(u32 *)(g_fake_d3d + 0x0) = base;
    /* EAX = schreibbarer Put (Ring; a=Groesse, 0 -> Seite). */
    size = a ? a : 4096;
    if (size > 1024 * 1024) {
        size = 1024 * 1024;
    }
    size = (size + 15) & ~15u;
    if (g_d3d_pool_off + size > sizeof g_d3d_pool) {
        g_d3d_pool_off = 0;
    }
    off = g_d3d_pool_off;
    g_d3d_pool_off += size;
    if (hlog_throttle(&calls)) {
        HLOG_T("D3D MakeSpace(a=0x%X) -> +0x%X", a, off);
    }
    return base + off;
}

/* ENTFERNT (boot303): ExitProcess-Hook rekursierte endlos (g_orig_ep
 * nie gesetzt -> ExitProcess() sprang zurueck in den Hook -> Stack-
 * Erschoepfung bei jedem sauberen Shutdown). Bei Bedarf mit Trampolin
 * neu aufsetzen. */

/* CreateVertex/IndexBuffer2 (RET 4, 1 Arg = Laenge): Das Spiel liest
 * [Handle+4] als Daten-Zeiger (D3DResource-Layout Common@0/Data@+4) und
 * kopiert per rep movsd hinein (0x60448) -> echte Structs + Puffer!
 * (Fake-Handles zeigten auf .data-BSS -> Fault 0x29A6E802.) */
static u32 create_res2(u32 len)
{
    u32 *r, *d;
    if (!len) {
        len = 4;
    }
    r = (u32 *)calloc(2, sizeof(u32));
    if (!r) {
        return 0;
    }
    d = (u32 *)calloc(len ? len : 4, 1);
    if (!d) {
        free(r);
        return 0;
    }
    r[0] = 0; /* Common */
    r[1] = (u32)(uintptr_t)d; /* Data */
    return (u32)(uintptr_t)r;
}
u32 __stdcall hook_D3D_CreateVB2(u32 a)
{
    static u32 calls;
    u32 h = create_res2(a);
    if (hlog_throttle(&calls)) {
        HLOG_T("D3D CreateVB2(len=0x%X) -> 0x%X", a, h);
    }
    return h;
}

u32 __stdcall hook_D3D_CreateIB2(u32 a)
{
    static u32 calls;
    u32 h = create_res2(a);
    if (hlog_throttle(&calls)) {
        HLOG_T("D3D CreateIB2(len=0x%X) -> 0x%X", a, h);
    }
    return h;
}

/* Draw/Stream-Hooks (No-Op + Trace, M2b; ABI per vadump verifiziert):
 * DrawUP=RET 0x10 (4 Args: prim,count,pData,stride),
 * DrawVertices=RET 0xC (3: prim,start,count),
 * DrawIndexedVertices=RET 0xC (3: prim,vcount,pIndex),
 * SetStream=RET 0xC (3), SetIndices=RET 8 (2). */
static u32 g_last_stream_no, g_last_stream_ptr, g_last_stream_stride;
static u32 g_last_index_a, g_last_index_b;
static u32 g_last_draw_prim, g_last_draw_a, g_last_draw_b, g_last_draw_kind;
/* M2c: Stream/IB-Struct ([Common,Data]) -> Datenzeiger. */
static u32 vb_stream_data(u32 res_struct)
{
    u32 data = 0;
    if (res_struct) {
        __try {
            data = ((u32 *)(uintptr_t)res_struct)[1];
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            data = 0;
        }
    }
    return data;
}

u32 __stdcall hook_D3D_DrawUP(u32 prim, u32 count, u32 pData,
                                     u32 stride)
{
    static u32 calls;
    g_last_draw_kind = 2;
    g_last_draw_prim = prim;
    g_last_draw_a = count;
    g_last_draw_b = pData;
    if (hlog_throttle(&calls)) {
        HLOG_T("D3D DrawUP(prim=%u n=%u p=0x%X stride=%u)", prim,
               count, pData, stride);
    }
    /* M2c: UP-Daten liegen direkt an (kein Struct). */
    sb_render_vb_draw(prim, (const unsigned char *)(uintptr_t)pData,
                       stride, NULL, 0, 0, 0, count);
    return 0;
}

/* D3D-Status-Query (0x20E4C0, 3 Args (id,out,exp), RET 0xC): Fence-Poll
 * ("GPU-Tick abfragen"). Original gibt 0x88760828 (BUSY), solange Slot
 * -1 (keine GPU -> nie ready). Alle 5 Caller (0x16A1E/0x16A4D/0x1BBF9/
 * 0x1C009/0x1DA6D) spinnen bis !BUSY (boot332: 3 FPS); #3/#4 vergleichen
 * *out mit EBX (Erwartung = arg3!). M2c: sofort ready ("unendlich schnelle
 * GPU"), *out = Echo der Erwartung (arg3 ? arg3 : 1). */
u32 __stdcall hook_D3D_QueryStatus(u32 id, u32 out, u32 exp)
{
    static u32 calls;
    u32 v = exp ? exp : 1u;
    /* FIX(SHADE-M2,W117) vis_query: 0x20E4C0 ist GetVisibilityTestResult(Index,
     * pResult, pTimeStamp). Mit aktivem vis_query echtes Ergebnis der
     * Occlusion-Query (render_visq.c), unbekannter Index wie bisher. */
    if (g_visq_on && sb_visq_active()) { /* W121: Shading zur Laufzeit aus -> wieder wie bisher */
        extern int sb_visq_result(unsigned index, unsigned *out);
        unsigned n = 0;
        if (sb_visq_result(id, &n)) {
            __try {
                if (out) { *(u32 *)(uintptr_t)out = n; }
                if (exp) { ((u32 *)(uintptr_t)exp)[0] = 0; ((u32 *)(uintptr_t)exp)[1] = 0; }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
            return 0;
        }
    }
    if (out) {
        __try {
            *(u32 *)(uintptr_t)out = v;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    if (hlog_throttle(&calls)) {
        HLOG_T("D3D QueryStatus(id=0x%X)", id);
    }
    return 0;
}

/* FIX(SHADE-M2,W117) vis_query: D3DDevice_BeginVisibilityTest (0x20EA80, keine
 * Argumente) / EndVisibilityTest (0x20EB20, stdcall Index, HRESULT). Nur
 * installiert, wenn vis_query aktiv ist (Standard: nur mit shading=1). */
int g_visq_on;
void __stdcall hook_D3D_BeginVisTest(void)
{
    extern void sb_visq_begin(void);
    if (!sb_visq_active()) { return; } /* W121: Shading zur Laufzeit aus */
    sb_visq_begin();
}
u32 __stdcall hook_D3D_EndVisTest(u32 index)
{
    extern void sb_visq_end(unsigned index);
    if (!sb_visq_active()) { return 0; }
    sb_visq_end(index);
    return 0;
}

u32 __stdcall hook_D3D_SetStream(u32 a, u32 b, u32 c)
{
    static u32 calls;
    g_last_stream_no = a;
    g_last_stream_ptr = b;
    g_last_stream_stride = c;
    if (hlog_throttle(&calls)) {
        HLOG_T("D3D SetStream(0x%X,0x%X,0x%X)", a, b, c);
    }
    return 0;
}

u32 __stdcall hook_D3D_SetIndices(u32 a, u32 b)
{
    static u32 calls;
    g_last_index_a = a;
    g_last_index_b = b;
    if (hlog_throttle(&calls)) {
        HLOG_T("D3D SetIndices(0x%X,0x%X)", a, b);
    }
    return 0;
}

/* DrawIndexedVertices (0x210740): RET 0xC, 3 Args (prim,vcount,pIndex). */
u32 __stdcall hook_D3D_DrawIdx(u32 prim, u32 vcount, u32 pIndex)
{
    sb_render_drawcall(1, prim, vcount, (unsigned)(uintptr_t)_ReturnAddress()); /* DIAG(COCKPIT,W60) */
    static u32 calls;
    g_last_draw_kind = 1;
    g_last_draw_prim = prim;
    g_last_draw_a = vcount;
    g_last_draw_b = pIndex;
    if (hlog_throttle(&calls)) {
        HLOG_T("D3D DrawIdx(prim=%u n=%u pIdx=0x%X)", prim, vcount,
               pIndex);
    }
    {
        u32 vd = vb_stream_data(g_last_stream_ptr);
        u32 id = vb_stream_data(g_last_index_a);
        /* pIndex==0 -> SetIndices-Puffer; sonst pIndex als Zeiger
         * (UP-Stil; Offset-Variante wuerde hier fehlschlagen -> Drop). */
        const unsigned short *ix = id
            ? (const unsigned short *)(uintptr_t)id
            : (pIndex ? (const unsigned short *)(uintptr_t)pIndex : NULL);
        if (ix) {
            sb_render_vb_draw(prim, (const unsigned char *)(uintptr_t)vd,
                               g_last_stream_stride, ix, vcount,
                               g_last_index_b, 0, 0);
        } else {
            sb_render_drawdrop(4, 0, vcount, 0); /* DIAG(COCKPIT,W60) */
        }
    }
    return 0;
}

/* LockRect-Fallback: eigener 16-MB-Block statt 1-MB-Array in .bss (dort
 * lagen direkt dahinter g_fake_prcb/g_fb/D3D11-Zeiger -> Ueberlauf haette
 * Harness-Zustand zerschrieben). Lazy per VirtualAlloc. */
#define LOCK_FALLBACK_SIZE (16u * 1024u * 1024u)
static u32 lock_fallback_bits(void)
{
    static void *p;
    if (!p) {
        p = VirtualAlloc(NULL, LOCK_FALLBACK_SIZE, MEM_RESERVE | MEM_COMMIT,
                         PAGE_READWRITE);
    }
    return (u32)(uintptr_t)p;
}

/* D3D_BlockOnTime (0x2116D0, 2 Args, RET 8): wartet auf GPU-Zeit.
 * Original rechnet ueber Fake-Device (0x211370: [0+0x44]-Fault, dann
 * BlockOnTime-Tail [0]-Fault). Ohne GPU nie warten. Caller ignorieren
 * EAX (4/10 Stellen verifiziert). M2b: No-Op, Return 0. */
u32 __stdcall hook_D3D_BlockOnTime(u32 a, u32 b)
{
    static u32 calls;
    (void)a;
    (void)b;
    /* FIX(CULL-POPIN,W215): Das Spiel wartet hier auf die GPU (BlockOnFence
     * nach der Zellenkarte, FUN_000459B0 @0x45B9D) - jetzt den RT-Inhalt in
     * den Xbox-Speicher holen, wie ihn die Xbox an dieser Stelle sieht. */
    sb_rtt_rb_sync_resolve(1);
    if (hlog_throttle(&calls)) {
        HLOG_T("D3D BlockOnTime -> no-op (GPU idle)");
    }
    return 0;
}

/* D3DTexture_LockRect (0x210C20, 5 Args, RET 0x14):
 * (pThis, Level, pLockedRect, pRect, Flags) -> VOID (Cxbx).
 * Original rechnet gegen Fake-Surfaces (Lock2DSurface Pfad B nutzt
 * ECX-Muell) -> [out+4] wild -> memcpy-Fault (boot306, 0x87620800,
 * intermittierend!). M2b: echte Bits+Pitch aus Header, Fallback Null.
 * (D3DSurface_LockRect bleibt original: 1 Aufrufer, stabil.) */
u32 __stdcall hook_D3DTexture_LockRect(u32 pThis, u32 level,
                                              u32 pOut, u32 pRect,
                                              u32 flags)
{
    static u32 calls, nfb;
    u32 pitch = 0, bits = 0;
    int ok;
    long long tbq = TB_BEGIN(); /* DIAG(TICK-PERF,W125) */
    (void)pRect;
    ok = sb_tex_lockbits(pThis, &pitch, &bits, flags);
    TB_END(TB_LOCK, tbq);
    if (ok) {
        sb_tex_mark_dirty(pThis); /* TEXDIRTY: Neu-Upload beim naechsten Get */
        if (!(flags & 0x10u)) { sb_tex_lockwatch_note(pThis); } /* W98: nicht D3DLOCK_READONLY */
        if (!(flags & 0x10u)) { sb_rem_note_cpu(pThis); } /* W103: CPU-beschrieben -> nie ersetzen */
    }
    if (!ok) {
        pitch = 1024;
        bits = lock_fallback_bits();
        if (nfb < 3) {
            nfb++;
            HLOG_W("D3D TexLockRect Fallback #%u: tex=0x%X lvl=%u -> Null",
                   nfb, pThis, level);
        }
    }
    __try {
        if (pOut) {
            *(u32 *)(uintptr_t)pOut = pitch;
            *(u32 *)(uintptr_t)(pOut + 4) = bits;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    {
        /* DIAG(W73): Lock-Ziel neuer Texturen (Absturz 0x24E1D3: Film-YUV-
         * Wandlung schrieb ab 0x2FF80000 bis an die DLL-Basis) - Data-Feld,
         * uebersetzte Adresse und der VirtualAlloc-Bereich dahinter. */
        static u32 seen[512]; /* W86: 512 mit contig_diag=1 (Film-Textur je Lauf finden), sonst 32 */
        static unsigned nseen;
        unsigned k;
        for (k = 0; k < nseen && seen[k] != pThis; k++) {}
        if (k == nseen && g_cfg.contig_diag && nseen < 512u) { /* W86-Rueckbau: nur mit DIAG-Schalter */
            MEMORY_BASIC_INFORMATION mi;
            u32 raw = 0;
            seen[nseen++] = pThis;
            memset(&mi, 0, sizeof mi);
            __try { raw = *(u32 *)(uintptr_t)(pThis + 4); } __except (EXCEPTION_EXECUTE_HANDLER) {}
            if (bits) { VirtualQuery((LPCVOID)(uintptr_t)bits, &mi, sizeof mi); }
            HLOG_I("D3D TexLock neu #%u: tex=0x%X data=0x%X -> bits=0x%X pitch=%u ok=%d alloc=0x%X region=0x%X+0x%X state=0x%X",
                   nseen, pThis, raw, bits, pitch, ok, (u32)(uintptr_t)mi.AllocationBase,
                   (u32)(uintptr_t)mi.BaseAddress, (u32)mi.RegionSize, (unsigned)mi.State);
        }
    }
    if (hlog_throttle(&calls)) {
        HLOG_T("D3D TexLockRect(tex=0x%X lvl=%u)", pThis, level);
    }
    return 0;
}

/* CDevice_KickOff (0x211530, ECX, RET 0): GPU-Tuer + Idle-Spin
 * ([NV+0x100410]-Bit 0x10000). Ohne GPU endlos -> NOW-LOADING-Hang
 * (boot305, Spin bei 0x211574). M2b: No-Op (nichts zu kicken, GPU
 * immer idle). Alle 6 Caller ueberschreiben EAX direkt danach. */
u32 __fastcall hook_D3D_KickOff(u32 ecx_dev)
{
    static u32 calls;
    (void)ecx_dev;
    if (hlog_throttle(&calls)) {
        HLOG_T("D3D KickOff -> no-op (idle)");
    }
    return 0;
}

/* DrawVertices (0x2106A0): RET 0xC, 3 Args (prim,start,count). No-Op +
 * M2c: submitttet via sb_render_vb_draw (Decl aus aktivem VS). */
u32 __stdcall hook_D3D_DrawVertices(u32 prim, u32 start, u32 count)
{
    sb_render_drawcall(0, prim, count, (unsigned)(uintptr_t)_ReturnAddress()); /* DIAG(COCKPIT,W60) */
    static u32 calls;
    g_last_draw_kind = 0;
    g_last_draw_prim = prim;
    g_last_draw_a = start;
    g_last_draw_b = count;
    if (hlog_throttle(&calls)) {
        HLOG_T("D3D DrawVertices(prim=%u start=%u n=%u)", prim, start,
               count);
    }
    {
        u32 vd = vb_stream_data(g_last_stream_ptr);
        sb_render_vb_draw(prim, (const unsigned char *)(uintptr_t)vd,
                           g_last_stream_stride, NULL, 0, 0, start,
                           count);
    }
    return 0;
}
