/* hooks_int.h - interner Header der XDK-Hooks (W68 SPLIT).
 * Gemeinsame Includes, Typen/Konstanten und die zwischen xdk_hooks.c
 * (Patch-Helfer, Fake-Objekte, hooks_install), hooks_d3d.c, hooks_timing.c,
 * hooks_audio.c, hooks_xapi.c, hooks_input.c und hooks_bots.c geteilten
 * Globals/Funktionen. Nur von diesen Modulen einzubinden. */
#ifndef HOOKS_INT_H
#define HOOKS_INT_H
#include "xdk_hooks.h"
#include "sb_profile.h" /* W229 SBA() */
#include "hlog.h"
#include "hcfg.h"
#include "fs_emu.h"
#include "kernel_hle.h"
#include "d3d11_render.h"
#include "sb_tex.h"
#include "audio_out.h"
#include <intrin.h>
#include <math.h>
#include "diag_states.h"
#include "diag_campaign.h"
#include "diag_input.h"
#include "watch_dr.h" /* W41 WATCHDR */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <windows.h>
#include <stdio.h>

typedef uint32_t u32;
typedef uint16_t u16;

/* Xbox D3DTRANSFORMSTATETYPE (D3D8): VIEW=2, PROJECTION=3, WORLD=256+i. */
/* MISSION3D (W25): PC-D3D8-Werte (alter Pfad, xbox_transform=0). */
#define XB_D3DTS_VIEW 2u
#define XB_D3DTS_PROJECTION 3u
#define XB_D3DTS_WORLD 256u
/* Xbox-XDK-Werte (XbD3D8Types.h: VIEW=0, PROJ=1, TEX=2-5, WORLD=6-9).
 * Das Spiel ruft SetTransform AUSSCHLIESSLICH mit 0/1 (+Register) auf
 * (46 Caller, s. W25-Bericht); PC-Werte trafen nie -> Matrizen null. */
#define XX_D3DTS_VIEW 0u
#define XX_D3DTS_PROJECTION 1u
#define XX_D3DTS_WORLD0 6u

/* ---------- XInput (SBC-Controller -> Tastatur, M2b) ----------
 * Layout nach Cxbx (SBCInput/Xapi.h): XINPUT_STATE = dwPacketNumber@0 +
 * 24-Byte-SBC-Block (wButtons[3], dann je Pad+Wert-Paare; Werte liegen im
 * HIGH-Byte). Button-Index i (0..38) -> wButtons[i/16] Bit i%16; Index
 * 4/5 (Hatch/Zuendung) und 34..38 (System) sind Toggle-Buttons.
 * Tastenbelegung v1 (s. BUILD.md M2-Tabelle). */
#pragma pack(push, 1)
typedef struct {
    uint16_t buttons[3];
    uint8_t pad1, aiming_x;
    uint8_t pad2, aiming_y;
    uint8_t pad3;
    int8_t rot_lever;
    uint8_t pad4;
    int8_t sight_x;
    uint8_t pad5;
    int8_t sight_y;
    uint8_t pad6, pedal_l;
    uint8_t pad7, pedal_m;
    uint8_t pad8, pedal_r;
    uint8_t tuner, gear;
} sbc_in_t;
#pragma pack(pop)
/* A8: Layout gegen den Capcom-Mapper fixieren (pState+4 = SBC-Block). */
_Static_assert(sizeof(sbc_in_t) == 24, "sbc_in_t muss 24 Bytes sein");
_Static_assert((unsigned)(uintptr_t)&((sbc_in_t *)0)->aiming_x == 7, "aiming_x");
_Static_assert((unsigned)(uintptr_t)&((sbc_in_t *)0)->aiming_y == 9, "aiming_y");
_Static_assert((unsigned)(uintptr_t)&((sbc_in_t *)0)->sight_x == 13, "sight_x");
_Static_assert((unsigned)(uintptr_t)&((sbc_in_t *)0)->sight_y == 15, "sight_y");
_Static_assert((unsigned)(uintptr_t)&((sbc_in_t *)0)->gear == 23, "gear");

/* ---- Geteilte Globals ---- */
extern unsigned g_frames;
extern uint64_t g_qpf, g_fps_start;
extern u32 g_last_frame_ms;
extern uint8_t g_fake_device[1024];
extern uint8_t g_fake_surface[1024];
extern uint8_t g_fake_stream[1024];
extern uint8_t g_fake_obj[1024];
extern uint8_t g_fake_d3d[0x3000];
extern uint8_t g_d3d_pool[8 * 1024 * 1024];
extern u32 g_act_reads, g_act_xin;
extern u32 g_aimat_idx;
extern u32 g_drive_idx;

/* ---- Helfer/Ticks (vorher static) ---- */
float u2f(u32 v);
void xbox_set_last_error(u32 code);
void patch_jmp(u32 va, void *handler, const char *name);
int seen_state(u32 state);
int turbo_mode_active(void);
void ds_audio_install(void);
void poll_census_install(void);
void replay_steps_tick(void);
int input_replay_active(void);
int key_down(int vk);
void aimat_apply(sbc_in_t *s);
void drive_apply(sbc_in_t *s);

/* ---- Hook-Ziele fuer hooks_install (vorher static) ---- */
u32 __stdcall hook_Direct3D_CreateDevice(u32 adapter, u32 devType, u32 hwnd, u32 behavior, u32 pParams, u32 ppDev);
u32 __stdcall hook_D3DDevice_Swap(u32 flags);
u32 __stdcall hook_D3DDevice_Clear(u32 count, u32 rects, u32 flags, u32 color, u32 z, u32 stencil);
u32 __stdcall hook_D3DDevice_Begin(u32 prim);
u32 __stdcall hook_D3DDevice_End(void);
u32 __stdcall hook_D3DDevice_SetTexture(u32 stage, u32 tex);
u32 __stdcall hook_D3DDevice_SetVertexShader(u32 handle);
u32 __stdcall hook_D3DDevice_GetVertexShader(u32 pOut);
u32 __stdcall hook_D3DDevice_GetPixelShader(u32 pOut);
u32 __stdcall hook_D3DDevice_GetVertexShaderConstant(u32 reg, u32 pOut, u32 count);
u32 __stdcall hook_D3DDevice_SetPixelShader(u32 handle);
u32 __stdcall hook_D3DDevice_SetRenderTarget(u32 surf, u32 depth);
u32 __stdcall hook_D3DDevice_GetBackBuffer2(u32 idx);
u32 __fastcall hook_D3DDevice_SetRenderState_Simple(u32 state, u32 value);
u32 __stdcall hook_D3DDevice_SetVertexData2f(u32 reg, u32 a, u32 b);
u32 __stdcall hook_D3DDevice_SetVertexData4f(u32 reg, u32 a, u32 b, u32 c, u32 d);
u32 __stdcall hook_D3DDevice_SetVertexDataColor(u32 reg, u32 color);
u32 __stdcall hook_D3DDevice_GetRenderTarget2(void);
u32 __stdcall hook_D3DDevice_SetViewport(u32 pVp);
u32 __stdcall hook_D3DDevice_SetTransform(u32 state, u32 pMat);
u32 __stdcall hook_D3DResource_Release(u32 res);
u32 __stdcall hook_D3D_SetRenderStateNotInline(u32 a, u32 b);
u32 __stdcall hook_D3D_SetRenderState_ZEnable(u32 v);
u32 __stdcall hook_D3D_SetRenderState_StencilEnable(u32 v);
u32 __stdcall hook_D3D_SetRenderState_StencilFail(u32 v);
u32 __stdcall hook_GPU_regread0(void);
u32 __stdcall hook_GPU_rmw(u32 arg);
u32 __stdcall hook_Get2DSurfaceDesc(u32 surf, u32 level, u32 desc);
u32 __stdcall hook_D3D_VBLock2(u32 pThis, u32 flags);
u32 __stdcall hook_D3D_SetTileNoWait(u32 a, u32 b);
u32 __stdcall hook_D3D_PSConst(u32 reg, u32 pData, u32 count);
u32 __stdcall hook_D3D_MakeSpace(u32 a, u32 b);
u32 __stdcall hook_D3D_CreateVB2(u32 a);
u32 __stdcall hook_D3D_CreateIB2(u32 a);
u32 __stdcall hook_D3D_DrawUP(u32 prim, u32 count, u32 pData, u32 stride);
u32 __stdcall hook_D3D_QueryStatus(u32 id, u32 out, u32 exp);
void __stdcall hook_D3D_BeginVisTest(void);      /* W117 vis_query */
u32 __stdcall hook_D3D_EndVisTest(u32 index);     /* W117 vis_query */
extern int g_visq_on;
int sb_visq_active(void);                          /* render_visq.c (W117) */
void xdk_visq_live(void);                         /* xdk_hooks.c (W121 vis_query_live) */
u32 __stdcall hook_D3D_SetStream(u32 a, u32 b, u32 c);
u32 __stdcall hook_D3D_SetIndices(u32 a, u32 b);
u32 __stdcall hook_D3D_DrawIdx(u32 prim, u32 vcount, u32 pIndex);
u32 __stdcall hook_D3D_BlockOnTime(u32 a, u32 b);
u32 __stdcall hook_D3DTexture_LockRect(u32 pThis, u32 level, u32 pOut, u32 pRect, u32 flags);
u32 __fastcall hook_D3D_KickOff(u32 ecx_dev);
u32 __stdcall hook_D3D_DrawVertices(u32 prim, u32 start, u32 count);
u32 __stdcall hook_BlockUntilVerticalBlank(void);
u32 __stdcall hook_GetDisplayFieldStatus(u32 out);
u32 __stdcall hook_GPU_223E2D(u32 arg);
u32 __stdcall hook_GPU_readywait(void);
u32 __stdcall hook_GPU_readywait2(void);
u32 __stdcall hook_PerfFrequency(u32 out);
u32 __stdcall hook_PerfCounter(u32 out);
u32 __stdcall hook_BinkWait(u32 movie);
u32 __stdcall hook_DS_CreateSoundStream(u32 a1, u32 ppStream, u32 a3, u32 a4);
u32 __stdcall hook_DS_BufferStop(u32 buf);
u32 __stdcall hook_DS_BufferGetStatus(u32 buf, u32 pStatus);
u32 __stdcall hook_DS_Wrapper106E30(u32 a1, u32 a2);
u32 __stdcall hook_DS_Wrapper1066C0(u32 a1, u32 a2);
u32 __stdcall hook_DS_Wrapper106E00(u32 a1, u32 a2);
u32 __stdcall hook_DS_Wrapper106DE0(u32 a1, u32 a2, u32 a3, u32 a4);
u32 __stdcall hook_DS_SetI3DL2Listener(u32 a1, u32 a2, u32 a3);
u32 __stdcall hook_DS_21E58C(u32 a1, u32 a2);
u32 __stdcall hook_XapiSelectCachePartition(u32 a1, u32 a2, u32 pCache);
u32 __stdcall hook_XLaunchNewImageA(u32 pPath, u32 flags);
u32 __stdcall hook_XMountUtilityDrive(u32 fFormat);
u32 __stdcall hook_ReadFile(u32 hFile, u32 buf, u32 len, u32 pRead, u32 ov);
u32 __fastcall hook_108720(u32 ecx_this);
u32 __fastcall hook_107E30(u32 ecx_this);
u32 __fastcall hook_109770(u32 ecx_this, u32 edx_ign, u32 arg);
u32 __fastcall hook_1086C0(u32 ecx_this);
u32 __fastcall hook_108740(u32 ecx_this);
u32 __fastcall hook_106560(u32 ecx_this);
u32 __stdcall hook_XapiMapLetterToDirectory(u32 a1, u32 a2, u32 a3, u32 a4, u32 a5, u32 a6);
u32 __stdcall hook_XapiMediaCheck(void);
u32 __stdcall hook_XapiNotifyWrap(u32 reason);
u32 __stdcall hook_XInputOpen(u32 type, u32 port, u32 flags, u32 poll);
u32 __stdcall hook_XInputClose(u32 h);
u32 __stdcall hook_XInputSetState(u32 h, u32 fb);
u32 __stdcall hook_XGetDeviceChanges(u32 type, u32 pIns, u32 pRem);
u32 __stdcall hook_XInputGetState(u32 h, u32 pState);
u32 __stdcall hook_D3D_SetRenderState_CullMode(u32 v);
u32 __stdcall hook_D3D_SetTextureStageStateNotInline(u32 a, u32 b, u32 c);
u32 __stdcall hook_GPU_229AB9(u32 arg);
u32 __cdecl hook_D3D_VSConst1(void); /* naked */
u32 __cdecl hook_D3D_VSConst4(void); /* naked */
u32 __stdcall hook_D3D_VSConstFast(void); /* naked */
/* per D3D_STATE1/2/3 erzeugt (hooks_d3d.c) */
u32 __stdcall hook_D3D_SetRenderState_EdgeAntiAlias(u32 v);
u32 __stdcall hook_D3D_SetRenderState_ShadowFunc(u32 v);
u32 __stdcall hook_D3D_SetRenderState_FogColor(u32 v);
u32 __stdcall hook_D3D_SetRenderState_TextureFactor(u32 v);
u32 __stdcall hook_D3D_SetRenderState_LineWidth(u32 v);
u32 __stdcall hook_D3D_SetRenderState_Dxt1NoiseEnable(u32 v);
u32 __stdcall hook_D3D_SetTextureState_TexCoordIndex(u32 a, u32 b);
u32 __stdcall hook_D3D_SetTextureState_BumpEnv(u32 a, u32 b, u32 c);
u32 __stdcall hook_D3D_SetTextureState_BorderColor(u32 a, u32 b);
u32 __stdcall hook_D3D_SetTextureState_ColorKeyColor(u32 a, u32 b);
u32 __stdcall hook_D3D_SetRenderState_OcclusionCullEnable(u32 v);

#endif /* HOOKS_INT_H */
