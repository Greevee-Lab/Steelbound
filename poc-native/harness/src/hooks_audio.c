/* hooks_audio.c - XDK-Hooks: DirectSound (Fake-Pfad audio=0, DS-Wrapper,
 * HLE-Slots DSa_* -> audio_out, ds_audio_install).
 * W68 SPLIT: 1:1 aus xdk_hooks.c verschoben. */
#include "hooks_int.h"

/* IDirectSound_CreateSoundStream (0x221658, 4 Args, RET 0x10). M1: kein
 * Audio (MCP-Audio-HW fehlt) -> S_OK + Fake-Stream (stumm). E_FAIL+NULL geht
 * nicht: Spiel ruft Stop(NULL) ohne Pruefung (Fault [0+0x20] @0x21E7F1).
 * (Arg2 [ESP+8] = ppStream (LEA, Caller-Stack).) */
u32 __stdcall hook_DS_CreateSoundStream(u32 a1, u32 ppStream, u32 a3,
                                               u32 a4)
{
    static u32 calls;
    (void)a1;
    (void)a3;
    (void)a4;
    if (ppStream) {
        *(u32 *)(uintptr_t)ppStream = (u32)(uintptr_t)g_fake_stream;
    }
    if (hlog_throttle(&calls)) {
        HLOG_T("DSB CreateSoundStream -> S_OK (Fake, stumm)");
    }
    return 0; /* S_OK */
}

/* CDirectSoundBuffer_Stop (0x21E7C5, 1 Arg, RET 4). M1: S_OK (stumm). */
u32 __stdcall hook_DS_BufferStop(u32 buf)
{
    static u32 calls;
    (void)buf;
    if (hlog_throttle(&calls)) {
        HLOG_T("DSB BufferStop -> S_OK (stumm)");
    }
    return 0; /* S_OK */
}

/* CDirectSoundBuffer_GetStatus (0x21E86D, 2 Args, RET 8). M1: S_OK +
 * Status=0 (STOPPED, stumm). */
u32 __stdcall hook_DS_BufferGetStatus(u32 buf, u32 pStatus)
{
    static u32 calls;
    extern unsigned g_present_no;
    (void)buf;
    /* BIKFIX-AUS: verzoerrt Timing (Load-Loop). */
    if (pStatus) {
        *(u32 *)(uintptr_t)pStatus = 0;
    }
    if (hlog_throttle(&calls)) {
        HLOG_T("DSB BufferGetStatus -> S_OK");
    }
    return 0; /* S_OK */
}

/* FUN_00106E30: DS-Wrapper (thiscall, 2 Args, RET 8) -> SetI3DL2Source.
 * Faultet bei NULL-this ([ECX+4]). M1: S_OK (stumm). */
u32 __stdcall hook_DS_Wrapper106E30(u32 a1, u32 a2)
{
    static u32 calls;
    (void)a1;
    (void)a2;
    if (hlog_throttle(&calls)) {
        HLOG_T("DSB Wrapper106E30 -> S_OK (stumm)");
    }
    return 0; /* S_OK */
}

/* FUN_001066C0: DS-Setter (thiscall, 2 Args, RET 8): [ECX+0x3C]=ARG1.
 * Faultet bei NULL-this. M1: S_OK (stumm). */
u32 __stdcall hook_DS_Wrapper1066C0(u32 a1, u32 a2)
{
    static u32 calls;
    (void)a1;
    (void)a2;
    if (hlog_throttle(&calls)) {
        HLOG_T("DSB Wrapper1066C0 -> S_OK (stumm)");
    }
    return 0; /* S_OK */
}

/* DS-Wrapper-RET-N (0x106xxx, vermessen 2026-10-28, fuer Bedarfs-Hooks):
 * 1065B0:RET4 106710:RET10 106750:RET10 106790:RET8 1067B0:RET8
 * 106810:RET8 106850:RET8 1068B0:RET4 106DE0:RET10 106FA0:TAILCALL. */

/* FUN_00106E00: DS-Setter+Call (thiscall, 2 Args, RET 8): [ECX+0x40]=ARG1,
 * dann CALL 0x2208C6. Faultet bei NULL-this. M1: S_OK (stumm). */
u32 __stdcall hook_DS_Wrapper106E00(u32 a1, u32 a2)
{
    static u32 calls;
    (void)a1;
    (void)a2;
    if (hlog_throttle(&calls)) {
        HLOG_T("DSB Wrapper106E00 -> S_OK (stumm)");
    }
    return 0; /* S_OK */
}

/* Wrapper106DE0 (0x106DE0, ECX-this + 4 Args, RET 0x10): ruft
 * 0x2209C7 mit [this+4] als Arg1. Mission ruft mit this=NULL
 * (kein DSound-Geraet) -> Fault [0+4]. M2b: S_OK (stumm). */
u32 __stdcall hook_DS_Wrapper106DE0(u32 a1, u32 a2, u32 a3,
                                           u32 a4)
{
    static u32 calls;
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    if (hlog_throttle(&calls)) {
        HLOG_T("DS Wrapper106DE0 -> S_OK (stumm)");
    }
    return 0;
}

/* CDirectSound_SetI3DL2Listener (0x220EAE, 3 Args, RET 0xC). M1: S_OK. */
u32 __stdcall hook_DS_SetI3DL2Listener(u32 a1, u32 a2, u32 a3)
{
    static u32 calls;
    (void)a1;
    (void)a2;
    (void)a3;
    if (hlog_throttle(&calls)) {
        HLOG_T("DSB SetI3DL2Listener -> S_OK (stumm)");
    }
    return 0; /* S_OK */
}

/* DSound-intern (0x21E58C, 2 Args, RET 8): Voice/Buffer-Fortschritt
 * ([EDX+0x10]-Objekt, Zähler +0x1C/+0x20). Caller reicht NULL durch
 * (kein echtes Audio) -> [0+0x10]-Crash. M1: NULL -> 0 (stumm);
 * non-NULL ebenfalls 0 + WARN (Fall bisher nicht beobachtet). */
u32 __stdcall hook_DS_21E58C(u32 a1, u32 a2)
{
    static u32 calls, warned;
    (void)a2;
    if (a1 && !warned) {
        warned = 1;
        HLOG_W("DS 21E58C: non-NULL a1=0x%X (unerwartet, -> 0)", a1);
    }
    if (hlog_throttle(&calls)) {
        HLOG_T("DS 21E58C(a1=0x%X) -> 0", a1);
    }
    return 0;
}

/* A7 (W4): DirectSound-HLE-Hooks. Nur installiert wenn [audio] audio=1
 * (sonst laufen die alten Stubs/Originale wie bisher). Alle Signaturen per
 * RET-N vermessen (s. BERICHT W4). Fremd-/NULL-this -> S_OK, nie Crash. */
#define dsa_ret() ((u32)(uintptr_t)_ReturnAddress())

/* Generische Accept-Hooks pro argc (Setter etc.): Stimme/DS-Objekt prüfen. */
static u32 __stdcall hook_DSa_A4(u32 v)
{
    static u32 w;
    if (v && !audio_is_voice(v) && !audio_is_ds(v) && hlog_throttle(&w))
        HLOG_W("AUDIO fremd-this 0x%X (ret=0x%X)", v, dsa_ret());
    return 0;
}
static u32 __stdcall hook_DSa_A8(u32 v, u32 a)
{
    (void)a;
    return hook_DSa_A4(v);
}
static u32 __stdcall hook_DSa_A12(u32 v, u32 a, u32 b)
{
    (void)a;
    (void)b;
    return hook_DSa_A4(v);
}
static u32 __stdcall hook_DSa_A16(u32 v, u32 a, u32 b, u32 c)
{
    (void)a;
    (void)b;
    (void)c;
    return hook_DSa_A4(v);
}
static u32 __stdcall hook_DSa_A20(u32 v, u32 a, u32 b, u32 c, u32 d)
{
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    return hook_DSa_A4(v);
}
static u32 __stdcall hook_DSa_A32(u32 v, u32 a, u32 b, u32 c, u32 d, u32 e,
                                 u32 f, u32 g)
{
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    (void)e;
    (void)f;
    (void)g;
    return hook_DSa_A4(v);
}

/* W89 DIAG(SFX): Ereignis-Spur (nur bei audio_sfx_diag > 0 aktiv). */
#define SFXD(v, w, a, b, c)                                                  \
    do {                                                                     \
        if (g_cfg.audio_sfx_diag > 0)                                        \
            audio_sfx_evt((v), (w), (a), (b), (c), dsa_ret());               \
    } while (0)
/* W89: 3D-/Filter-Setter mit Spur (sonst wie die generischen Accept-Hooks). */
static u32 __stdcall hook_DSa_BufSetAllParams(u32 v, u32 p, u32 apply)
{
    au32 d[16];
    memset(d, 0, sizeof d);
    if (g_cfg.audio_sfx_diag > 0 && p) {
        __try {
            memcpy(d, (void *)(uintptr_t)p, sizeof d);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        /* DS3DBUFFER: size, pos xyz, vel xyz, cone in/out, orient xyz,
         * outsideVol, minDist, maxDist, mode */
        SFXD(v, "SetAllParams", d[1], d[2], d[3]);
        SFXD(v, "SetAllParams2", d[13], d[14], d[15]);
    }
    (void)apply;
    if (g_cfg.audio_3d && v && p)
        audio_3d_voice_allparams(v, p);
    return hook_DSa_A4(v);
}
static u32 __stdcall hook_DSa_BufSetPosition3D(u32 v, u32 x, u32 y, u32 z,
                                               u32 apply)
{
    SFXD(v, "SetPosition", x, y, z);
    (void)apply;
    if (g_cfg.audio_3d && v)
        audio_3d_voice_pos(v, x, y, z);
    return hook_DSa_A4(v);
}
/* W89 DS3D: Stream-Distanzen (v, float, apply), Listener-Setter. */
static u32 __stdcall hook_DSa_StrSetMaxDist(u32 v, u32 f, u32 apply)
{
    (void)apply;
    if (g_cfg.audio_3d && v)
        audio_3d_voice_dist(v, 1, f);
    return hook_DSa_A4(v);
}
static u32 __stdcall hook_DSa_StrSetMinDist(u32 v, u32 f, u32 apply)
{
    (void)apply;
    if (g_cfg.audio_3d && v)
        audio_3d_voice_dist(v, 0, f);
    return hook_DSa_A4(v);
}
static u32 __stdcall hook_DSa_DSSetPosition(u32 o, u32 x, u32 y, u32 z, u32 apply)
{
    if (g_cfg.audio_sfx_diag > 0) { /* W89 DIAG(SFXL): Listener-Wechsel */
        static u32 lx, ly, lz, n;
        if ((x != lx || y != ly || z != lz) && n < 3000) {
            float fx, fy, fz;
            memcpy(&fx, &x, 4);
            memcpy(&fy, &y, 4);
            memcpy(&fz, &z, 4);
            n++;
            HLOG_I("DIAG(SFXL) Listener-Pos (%.1f,%.1f,%.1f) apply=%u ret=0x%X",
                   fx, fy, fz, apply, dsa_ret());
            lx = x;
            ly = y;
            lz = z;
        }
    }
    (void)apply;
    if (g_cfg.audio_3d)
        audio_3d_listener_pos(x, y, z);
    return hook_DSa_A4(o);
}
static u32 __stdcall hook_DSa_DSSetOrientation(u32 o, u32 fx, u32 fy, u32 fz,
                                               u32 tx, u32 ty, u32 tz,
                                               u32 apply)
{
    (void)apply;
    if (g_cfg.audio_3d)
        audio_3d_listener_orient(fx, fy, fz, tx, ty, tz);
    return hook_DSa_A4(o);
}
static u32 __stdcall hook_DSa_DSSetRolloff(u32 o, u32 f, u32 apply)
{
    (void)apply;
    if (g_cfg.audio_3d)
        audio_3d_rolloff(f);
    return hook_DSa_A4(o);
}
static u32 __stdcall hook_DSa_BufSetEG(u32 v, u32 p)
{
    SFXD(v, "SetEG", p, 0, 0);
    if (v)
        audio_note_eg(v, p); /* W102 DIAG */
    if (g_cfg.audio_sfx_diag > 0 && p) { /* W89 DIAG(SFXEG): Inhalt je Desc */
        static u32 seen_p[64], seen_h[64], n;
        u32 d[10], h = 0, i;
        memset(d, 0, sizeof d);
        __try {
            memcpy(d, (void *)(uintptr_t)p, sizeof d);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        for (i = 0; i < 10; i++)
            h = h * 31u + d[i];
        for (i = 0; i < n && (seen_p[i] != p || seen_h[i] != h); i++)
            ;
        if (i == n && n < 64) {
            seen_p[n] = p;
            seen_h[n] = h;
            n++;
            /* DSENVELOPEDESC: EG, Mode, Delay, Attack, Hold, Decay, Release,
             * Sustain, PitchScale, FilterCutOff */
            HLOG_I("DIAG(SFXEG) p=0x%X eg=%u mode=%u delay=%u attack=%u hold=%u "
                   "decay=%u release=%u sustain=%u pitch=%d filter=%d ret=0x%X",
                   p, d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], (int)d[8],
                   (int)d[9], dsa_ret());
        }
    }
    return hook_DSa_A4(v);
}
static u32 __stdcall hook_DSa_BufSetFilter(u32 v, u32 p)
{
    SFXD(v, "SetFilter", p, 0, 0);
    if (v && p) /* W102: DSFILTERDESC speichern (Wirkung bei audio_filter=1) */
        audio_set_filter(v, p);
    return hook_DSa_A4(v);
}
/* W102: Stream-SetFilter (0x21F654, v + Desc) und SetEG/SetI3DL2Source
 * (Puffer 0x220A31 / Stream 0x220B3F: v, Params, apply) mit Speicherung. */
static u32 __stdcall hook_DSa_StrSetFilter(u32 v, u32 p)
{
    if (v && p)
        audio_set_filter(v, p);
    return hook_DSa_A4(v);
}
static u32 __stdcall hook_DSa_StrSetEG(u32 v, u32 p)
{
    if (v)
        audio_note_eg(v, p);
    return hook_DSa_A4(v);
}
static u32 __stdcall hook_DSa_SetI3DL2Src(u32 v, u32 p, u32 apply)
{
    (void)apply;
    if (v && p)
        audio_set_i3dl2(v, p);
    return hook_DSa_A4(v);
}

/* Erzeugung: (this,pDesc,ppOut,0) bzw. global (pDesc,ppOut). */
static u32 __stdcall hook_DSa_CreateSoundStream(u32 a1, u32 pDesc, u32 ppOut,
                                                u32 a4)
{
    au32 v;
    (void)a1;
    (void)a4;
    v = audio_stream_create(pDesc, dsa_ret());
    if (ppOut)
        *(u32 *)(uintptr_t)ppOut = v;
    return 0;
}
static u32 __stdcall hook_DSa_CreateSoundBuffer(u32 a1, u32 pDesc, u32 ppOut,
                                                u32 a4)
{
    au32 v;
    (void)a1;
    (void)a4;
    v = audio_buffer_create(pDesc, dsa_ret());
    if (ppOut)
        *(u32 *)(uintptr_t)ppOut = v;
    return 0;
}
static u32 __stdcall hook_DSa_DirectSoundCreate(u32 a1, u32 ppOut, u32 a3)
{
    au32 o;
    (void)a1;
    (void)a3;
    o = audio_ds_create();
    if (ppOut)
        *(u32 *)(uintptr_t)ppOut = o;
    return 0;
}
static u32 __stdcall hook_DSa_DirectSoundCreateBuffer(u32 pDesc, u32 ppOut)
{
    au32 v = audio_buffer_create(pDesc, dsa_ret());
    if (ppOut)
        *(u32 *)(uintptr_t)ppOut = v;
    return 0;
}
static u32 __stdcall hook_DSa_DirectSoundCreateStream(u32 pDesc, u32 ppOut)
{
    au32 v = audio_stream_create(pDesc, dsa_ret());
    if (ppOut)
        *(u32 *)(uintptr_t)ppOut = v;
    return 0;
}
/* Puffer-Release/AddRef + DS-Release (ret4). */
static u32 __stdcall hook_DSa_BufferRelease(u32 v)
{
    audio_release(v);
    return 0;
}
static u32 __stdcall hook_DSa_BufferAddRef(u32 v)
{
    audio_addref(v);
    return 0;
}
static u32 __stdcall hook_DSa_DSRelease(u32 o)
{
    audio_ds_release(o);
    return 0;
}
static u32 __stdcall hook_DSa_DSCommit(u32 o)
{
    (void)o;
    return 0;
}
/* Play/Stop/Status/Position. */
static u32 __stdcall hook_DSa_Play(u32 v, u32 r1, u32 r2, u32 flags)
{
    SFXD(v, "Play", flags, 0, 0);
    (void)r1;
    (void)r2;
    if (v)
        audio_play(v, flags);
    return 0;
}
static u32 __stdcall hook_DSa_Stop(u32 v)
{
    if (v)
        audio_stop(v);
    return 0;
}
static u32 __stdcall hook_DSa_StopEx(u32 v, u32 a, u32 b, u32 c)
{
    SFXD(v, "StopEx", a, b, c);
    (void)a;
    (void)b;
    (void)c;
    if (v)
        audio_stop(v);
    return 0;
}
static u32 __stdcall hook_DSa_GetStatus(u32 v, u32 pStatus)
{
    u32 st = v ? audio_get_status(v) : 0;
    if (pStatus)
        *(u32 *)(uintptr_t)pStatus = st;
    return 0;
}
/* W89 (audio_ithis_fix): Die oeffentlichen Wrapper IDirectSoundBuffer_Stop
 * (0x21F53C) und _GetStatus (0x21F598) rufen die C-Ebene (0x21E7C5/0x21E86D,
 * dort sassen die Hooks) mit this-0x1C (Interface -> CDirectSoundBuffer).
 * Unsere Stimme ist das Interface-Objekt -> au_lookup schlug fehl: Stop wirkte
 * nie, GetStatus meldete immer STOPPED. Das Spiel (Stimmen-Pool 0x107B60)
 * hielt so jede SFX-Stimme fuer frei und vergab sie sofort neu (jeder Ton
 * nach Millisekunden abgeschnitten). Hier am Wrapper = richtiger Zeiger.
 * Einzige Aufrufer der C-Ebene sind diese Wrapper (xbe_disasm callers). */
static u32 __stdcall hook_DSa_IStop(u32 v)
{
    SFXD(v, "Stop", 0, 0, 0);
    if (v)
        audio_stop(v);
    return 0;
}
static u32 __stdcall hook_DSa_IGetStatus(u32 v, u32 pStatus)
{
    u32 st = v ? audio_get_status(v) : 0;
    if (pStatus)
        *(u32 *)(uintptr_t)pStatus = st;
    SFXD(v, "GetStatus", st, 0, 0);
    return 0;
}
static u32 __stdcall hook_DSa_GetCurrentPosition(u32 v, u32 pPlay, u32 pWrite)
{
    audio_get_pos(v, pPlay, pWrite);
    return 0;
}
static u32 __stdcall hook_DSa_SetCurrentPosition(u32 v, u32 off)
{
    SFXD(v, "SetCurPos", off, 0, 0);
    if (v)
        audio_set_pos(v, off);
    return 0;
}
static u32 __stdcall hook_DSa_SetBufferData(u32 v, u32 pData, u32 bytes)
{
    SFXD(v, "SetBufferData", pData, bytes, 0);
    if (v)
        audio_set_buffer_data(v, pData, bytes);
    return 0;
}
static u32 __stdcall hook_DSa_Lock(u32 v, u32 off, u32 bytes, au32 pp1,
                                   au32 pb1, au32 pp2, au32 pb2, u32 flags)
{
    if (!v)
        return 0x80004005u;
    return audio_lock(v, off, bytes, pp1, pb1, pp2, pb2, flags);
}
static u32 __stdcall hook_DSa_Unlock(u32 v, au32 p1, au32 b1, au32 p2, au32 b2)
{
    if (v)
        audio_unlock(v, p1, b1, p2, b2);
    return 0;
}
/* Setter mit Wirkung. */
static u32 __stdcall hook_DSa_SetVolume(u32 v, u32 vol)
{
    SFXD(v, "SetVolume", vol, 0, 0);
    if (v)
        audio_set_volume(v, (int)vol);
    return 0;
}
static u32 __stdcall hook_DSa_SetFormat(u32 v, u32 pwfx)
{
    SFXD(v, "SetFormat", pwfx, 0, 0);
    if (v)
        audio_set_format(v, pwfx);
    return 0;
}
static u32 __stdcall hook_DSa_SetFrequency(u32 v, u32 hz)
{
    SFXD(v, "SetFrequency", hz, 0, 0);
    if (v)
        audio_set_frequency(v, hz);
    return 0;
}
static u32 __stdcall hook_DSa_SetNotify(u32 v, u32 count, u32 pN)
{
    SFXD(v, "SetNotify", count, pN, 0);
    if (v)
        audio_set_notify(v, count, pN);
    return 0;
}
static u32 __stdcall hook_DSa_SetLoop(u32 v, u32 start, u32 len)
{
    SFXD(v, "SetLoopRegion", start, len, 0);
    if (v)
        audio_set_loop(v, start, len);
    return 0;
}
/* A7B: Pan (MixBins), Headroom, Pitch mit Wirkung (audio_pan/audio_freq). */
static u32 __stdcall hook_DSa_SetMixBins(u32 v, u32 pMixBins)
{
    SFXD(v, "SetMixBins", pMixBins, 0, 0);
    if (v)
        audio_set_mixbins(v, pMixBins);
    return 0;
}
static u32 __stdcall hook_DSa_SetMixBinVolumes(u32 v, u32 pMixBins)
{
    SFXD(v, "SetMixBinVol", pMixBins, 0, 0);
    if (v)
        audio_set_mixbin_volumes(v, pMixBins);
    return 0;
}
static u32 __stdcall hook_DSa_SetHeadroom(u32 v, u32 hr)
{
    SFXD(v, "SetHeadroom", hr, 0, 0);
    if (v)
        audio_set_headroom(v, hr);
    return 0;
}
static u32 __stdcall hook_DSa_SetPitch(u32 v, u32 pitch)
{
    SFXD(v, "SetPitch", pitch, 0, 0);
    if (v)
        audio_set_pitch(v, (int)pitch);
    return 0;
}
static u32 __stdcall hook_DSa_SetPlayRegion(u32 v, u32 start, u32 len)
{
    SFXD(v, "SetPlayRegion", start, len, 0);
    if (v)
        audio_set_play_region(v, start, len);
    return 0;
}
static u32 __stdcall hook_DSa_Pause(u32 v, u32 p)
{
    if (v)
        audio_pause(v, p);
    return 0;
}
static u32 __stdcall hook_DSa_FlushEx(u32 v, u32 a, u32 b, u32 c)
{
    (void)a;
    (void)b;
    (void)c;
    if (v)
        audio_flush(v);
    return 0;
}
static u32 __stdcall hook_DSa_Duplicate(u32 v, u32 a, u32 ppOut)
{
    (void)a;
    audio_duplicate(v, ppOut);
    return 0;
}
static u32 __stdcall hook_DSa_GetCaps(u32 o, u32 pCaps)
{
    /* W24 (audio_caps_clamp): Xbox-DSOUNDCAPS ist 12 Bytes (dwSize, dwFlags,
     * dwMaxSecondarySampleRate). Das Spiel uebergibt teils dwSize > Puffer
     * (belegt: 28 bei 16-Byte-Stack-Slot im Banner-Handler 0x110650) ->
     * audio_get_caps nullte sonst Return-Adresse + Args auf dem Stack ->
     * Sprung nach 0 beim Missionsende. Nur (12,1024] klemmen: <=12 ist
     * claim-konform, >1024 schreibt audio_get_caps ohnehin nichts. */
    if (g_cfg.audio_caps_clamp && pCaps) {
        __try {
            u32 sz = *(volatile u32 *)(uintptr_t)pCaps;
            if (sz > 12 && sz <= 1024)
                *(volatile u32 *)(uintptr_t)pCaps = 12;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    audio_get_caps(o, pCaps);
    return 0;
}
static u32 __stdcall hook_DSa_DoWork(void)
{
    static u32 calls;
    audio_dowork();
    if (hlog_throttle(&calls))
        HLOG_T("AUDIO DoWork -> S_OK (HLE)");
    return 0;
}

/* Installation aller A7-Hooks (nur bei audio=1). Adressen + RET-N vermessen:
 * Erzeugung/Methoden s. BERICHT W4 (Inventar-Tabelle). */
void ds_audio_install(void)
{
    /* Erzeugung */
    patch_jmp(0x221658u, hook_DSa_CreateSoundStream, "DSa-CreateSoundStream");
    patch_jmp(0x221733u, hook_DSa_CreateSoundBuffer, "DSa-CreateSoundBuffer");
    patch_jmp(0x2218EDu, hook_DSa_DirectSoundCreate, "DSa-DirectSoundCreate");
    patch_jmp(0x221934u, hook_DSa_DirectSoundCreateBuffer, "DSa-DSCBuffer");
    patch_jmp(0x22198Bu, hook_DSa_DirectSoundCreateStream, "DSa-DSCStream");
    /* Puffer-Leben + Transport */
    patch_jmp(0x21E123u, hook_DSa_BufferAddRef, "DSa-BufAddRef");
    patch_jmp(0x21E139u, hook_DSa_BufferRelease, "DSa-BufRelease");
    patch_jmp(0x21E11Eu, hook_DSa_Unlock, "DSa-Unlock");
    patch_jmp(0x21F518u, hook_DSa_Play, "DSa-Play");
    patch_jmp(0x21E7C5u, hook_DSa_Stop, "DSa-Stop");
    if (g_cfg.audio_ithis_fix) { /* W89: I-Wrapper (richtiger this) */
        patch_jmp(0x21F53Cu, hook_DSa_IStop, "DSa-IStop");
        patch_jmp(0x21F598u, hook_DSa_IGetStatus, "DSa-IGetStatus");
    }
    patch_jmp(0x21F554u, hook_DSa_StopEx, "DSa-StopEx");
    patch_jmp(0x21E86Du, hook_DSa_GetStatus, "DSa-GetStatus");
    patch_jmp(0x21F5B4u, hook_DSa_GetCurrentPosition, "DSa-GetCurPos");
    patch_jmp(0x21F5D4u, hook_DSa_SetCurrentPosition, "DSa-SetCurPos");
    patch_jmp(0x2213F9u, hook_DSa_SetBufferData, "DSa-SetBufferData");
    patch_jmp(0x21F5F0u, hook_DSa_Lock, "DSa-Lock");
    patch_jmp(0x21F620u, hook_DSa_SetNotify, "DSa-SetNotify");
    patch_jmp(0x21F578u, hook_DSa_SetLoop, "DSa-SetLoop");
    patch_jmp(0x220A51u, hook_DSa_SetPlayRegion, "DSa-SetPlayRegion");
    patch_jmp(0x21F421u, hook_DSa_Duplicate, "DSa-Duplicate");
    /* Puffer-Setter (Wirkung: Volume/Format/Frequenz) */
    patch_jmp(0x21F438u, hook_DSa_SetVolume, "DSa-BufSetVolume");
    patch_jmp(0x220872u, hook_DSa_SetFormat, "DSa-BufSetFormat");
    patch_jmp(0x22088Eu, hook_DSa_SetFrequency, "DSa-BufSetFreq");
    patch_jmp(0x21F454u, hook_DSa_SetPitch, "DSa-BufSetPitch");
    patch_jmp(0x21F48Cu, hook_DSa_BufSetEG, "DSa-BufSetEG");
    patch_jmp(0x21F4A8u, hook_DSa_BufSetFilter, "DSa-BufSetFilter");
    patch_jmp(0x21F4C4u, hook_DSa_SetHeadroom, "DSa-BufSetHeadroom");
    patch_jmp(0x21F4E0u, hook_DSa_SetMixBins, "DSa-BufSetMixBins");
    patch_jmp(0x21F4FCu, hook_DSa_SetMixBinVolumes, "DSa-BufSetMixBinVol");
    patch_jmp(0x2208C6u, hook_DSa_BufSetAllParams, "DSa-BufSetAllParams");
    patch_jmp(0x2209C7u, hook_DSa_BufSetPosition3D, "DSa-BufSetPosition");
    patch_jmp(0x2209FCu, hook_DSa_A20, "DSa-BufSetVelocity");
    patch_jmp(0x220A31u, hook_DSa_SetI3DL2Src, "DSa-BufSetI3DL2Src");
    /* Stream-Setter direkt (Vtable: GetInfo/Status/Process in audio_out.c) */
    patch_jmp(0x21F640u, hook_DSa_SetVolume, "DSa-StrSetVolume");
    patch_jmp(0x220A71u, hook_DSa_SetFormat, "DSa-StrSetFormat");
    patch_jmp(0x21F668u, hook_DSa_Pause, "DSa-StrPause");
    patch_jmp(0x21F66Du, hook_DSa_FlushEx, "DSa-StrFlushEx");
    patch_jmp(0x21F64Fu, hook_DSa_StrSetEG, "DSa-StrSetEG");
    patch_jmp(0x21F654u, hook_DSa_StrSetFilter, "DSa-StrSetFilter");
    patch_jmp(0x21F659u, hook_DSa_SetHeadroom, "DSa-StrSetHeadroom");
    patch_jmp(0x21F65Eu, hook_DSa_SetMixBins, "DSa-StrSetMixBins");
    patch_jmp(0x220AB8u, hook_DSa_StrSetMaxDist, "DSa-StrSetMaxDist");
    patch_jmp(0x220AD0u, hook_DSa_StrSetMinDist, "DSa-StrSetMinDist");
    patch_jmp(0x220AEDu, hook_DSa_BufSetPosition3D, "DSa-StrSetPosition");
    patch_jmp(0x220B16u, hook_DSa_A20, "DSa-StrSetVelocity");
    patch_jmp(0x220B3Fu, hook_DSa_SetI3DL2Src, "DSa-StrSetI3DL2Src");
    /* DirectSound-Objekt */
    patch_jmp(0x21E108u, hook_DSa_DSRelease, "DSa-DSRelease");
    patch_jmp(0x21F3BEu, hook_DSa_GetCaps, "DSa-DSGetCaps");
    patch_jmp(0x22085Au, hook_DSa_DSCommit, "DSa-DSCommit");
    patch_jmp(0x21F3DAu, hook_DSa_A20, "DSa-DSDownloadFX");
    patch_jmp(0x21F401u, hook_DSa_A12, "DSa-DSSetMixBinHead");
    patch_jmp(0x22083Eu, hook_DSa_A8, "DSa-DSHeadphones");
    patch_jmp(0x2212DDu, hook_DSa_A12, "DSa-DSSetDistFactor");
    patch_jmp(0x221301u, hook_DSa_DSSetOrientation, "DSa-DSSetOrient");
    patch_jmp(0x22134Bu, hook_DSa_DSSetPosition, "DSa-DSSetPosition");
    patch_jmp(0x221380u, hook_DSa_DSSetRolloff, "DSa-DSSetRolloff");
    patch_jmp(0x2213A4u, hook_DSa_A20, "DSa-DSSetVelocity");
    patch_jmp(0x2213D9u, hook_DSa_A12, "DSa-DSSetI3DL2List");
    patch_jmp(0x21F685u, hook_DSa_DoWork, "DSa-DoWork");
    HLOG_I("AUDIO HLE-Hooks installiert (audio=1)");
}
