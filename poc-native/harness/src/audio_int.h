/* audio_int.h - interner Header der DirectSound-HLE (W68 SPLIT).
 * Gemeinsame Includes, Konstanten, Stimmen-/DS-Typen und die zwischen
 * audio_out.c (Stimmen/DS-API), audio_fmt.c (Formate/Dekoder/FIFO) und
 * audio_mix.c (Mixer-Thread, WinMM/WASAPI-Ausgabe, WAV-Capture) geteilten
 * Globals/Funktionen. Nur von diesen drei Modulen einzubinden. */
#ifndef AUDIO_INT_H
#define AUDIO_INT_H
#include "audio_out.h"
#include "hlog.h"
#include "hcfg.h"
#include "kernel_hle.h"
#include "hlock.h" /* W119 LOADLOCK */
#include "audio_spatial.h" /* W356: HRTF, Surround, Downmix */
#include <windows.h>
#include <mmsystem.h>
#include <objbase.h> /* COM (WASAPI) */
#include <mmdeviceapi.h> /* IMMDeviceEnumerator (WASAPI) */
#include <audioclient.h> /* IAudioClient/RenderClient (WASAPI) */
#include <propsys.h> /* IPropertyStore (Geraetename) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define AUDIO_MAGIC 0x41554456u /* 'AUDV' */
#define AUDIO_DS_MAGIC 0x41554453u /* 'AUDS' */
#define MIXRATE 44100
#define MIXCHUNK 882 /* 20 ms */
#define FIFO_CAP_FRAMES (MIXRATE * 3) /* 3 s Backpressure-Grenze */
#define FIFO_HARD_CAP (16 * 1024 * 1024) /* Bytes, danach Drop */
#define AU_RING_FADE 220 /* ISSUE-10: Ein-/Ausblenden bei Unterlauf/Reset, 5 ms */

/* DSBSTATUS-Bits (Xbox wie PC) */
#define DSBSTATUS_PLAYING 0x00000001u
#define DSBSTATUS_LOOPING 0x00000004u
/* XMEDIAPACKET-Status */
#define XMP_PENDING 0u
#define XMP_SUCCESS 1u
/* DSBPLAY-Flags */
#define DSBPLAY_LOOPING 0x00000001u
#define DSBPN_OFFSETSTOP 0xFFFFFFFFu

typedef struct {
    uint16_t tag, ch;
    uint32_t rate, avg;
    uint16_t align, bits;
} wfx_t;

typedef struct audio_notify {
    au32 off;
    au32 xhev;
} audio_notify_t;

/* W34: In-Flight-Paket (Xbox: Process = PENDING, Vollzug async, sobald der
 * Mixer die Frames in Echtzeit konsumiert hat). frames_end bezieht sich auf
 * pc_done (monoton, nie Reset - anders als played, das SetPos nullt). */
typedef struct audio_inflight {
    au32 pdwCompleted, pdwStatus; /* Spiel-Zeiger (stabile Slots, keine Stacks) */
    HANDLE hev; /* Host-Event (beim Submit aufgeloest, NULL = keins) */
    au32 done_size; /* completed-Groesse beim Vollzug */
    uint64_t frames_end; /* Vollzug sobald pc_done >= frames_end */
} audio_inflight_t;

typedef struct audio_voice {
    void *vtable; /* +0: HLE-Vtable (Spiel ruft Slots direkt) */
    au32 magic; /* +4 */
    au32 kind; /* +8: 1=Puffer, 2=Stream */
    LONG refcount; /* +12 */
    au32 id;
    /* Format (Quelle) */
    wfx_t fmt;
    int have_fmt;
    au32 buflen; /* Lock-/Ringpuffer (Puffer) */
    uint8_t *lockbuf;
    au32 lock_n1, lock_n2; /* W22-DIAG: zuletzt gewaehrte Lock-Bereiche */
    au32 lock_off; /* W22: Lock-Offset der letzten Lockung (Ring-Frontier) */
    uint64_t ring_front; /* W22: absolute Ring-Frontier (Append-Stand) */
    uint64_t ring_contig; /* W83: seit letzter Luecke/Reset lueckenlos angehaengte Ring-Bytes */
    uint64_t dbg_clip_bytes; /* W22-DIAG: per Ring-Clip uebersprungene Bytes */
    /* statische Daten (SetBufferData, dekodiert nach Stereo-float) */
    float *data;
    au32 datalen; /* Frames */
    au32 playhead; /* Frames */
    int looping;
    au32 loop_start, loop_len; /* Frames */
    /* FIFO (Stream-Pakete, Unlock-Appends; Stereo-float @MIXRATE) */
    float *fifo;
    au32 fifo_cap, fifo_rd, fifo_wr, fifo_n; /* Frames */
    /* W34: Async-Vollzug (nur Process-Pfad, kind==2) */
    uint64_t pc_done; /* konsumierte FIFO-Frames, monoton, nie Reset */
    audio_inflight_t inflight[8]; /* Spiel: <=3 gleichzeitig */
    au32 ninflight;
    /* Zustand */
    int playing; /* Play() aktiv */
    int paused;
    int active; /* je Daten gesehen (Stream: --> PLAYING) */
    float gain; /* aus SetVolume */
    uint64_t played; /* gerenderte Frames (Positionsuhr) */
    uint64_t dbg_consumed; /* W14: verbraucht, nie zurueckgesetzt (DIAG) */
    uint64_t dbg_unlock_n; /* W22: Unlock-Aufrufe (Feed-Seite, DIAG) */
    uint64_t dbg_unlock_bytes; /* W22: per Unlock gefuetterte Bytes (DIAG) */
    uint64_t dbg_sil_bytes; /* W22: davon Stille (0x00/0x80-Muster, DIAG) */
    /* W22-DIAG: Lock-Kadenz (Bink-Feed 6 % zu schnell?). Erste 8 + jede
     * 128. Lockung mit QPC/TID/off/Bytes + zuletzt gemeldeten Cursorn. */
    uint64_t dbg_lock_n, dbg_lock_bytes, dbg_lock_qpc0, dbg_lock_qpc1;
    DWORD dbg_lock_tid0;
    au32 dbg_last_play, dbg_last_wr;
    struct {
        uint64_t qpc;
        DWORD tid;
        au32 off, bytes, grant, play, wr;
    } dbg_lock_hist[40];
    au32 dbg_lock_hist_n;
    /* W83 DIAG(BINKWRAP): SetBufferData-Spielspeicher + Schatten des
     * Lock-Puffers nach dem letzten Unlock (audio_wrap_diag=1) */
    au32 dbg_setbuf_p;
    uint8_t *dbg_shadow;
    uint64_t src_consumed; /* verbrauchte Quell-Bytes (Position) */
    au32 src_rate; /* wirksame Quell-Rate (SetFrequency) */
    /* A7B: Pan (MixBins), Headroom, Frequenz-Schritt */
    float pan_l, pan_r; /* Stereo-Pan aus MixBins (audio_pan), Default 1/1 */
    float hr_gain; /* aus SetHeadroom (audio_pan) */
    float bin_gain[8]; /* pro MixBin, linear (audio_pan) */
    au32 dec_rate; /* Quellrate bei Dekodierung (fuer freq_step) */
    float freq_step; /* Wiedergabe-Schritt (audio_freq), 1 = normal */
    float play_frac; /* fraktionaler Playhead-Anteil */
    /* Notifications */
    audio_notify_t notifies[8];
    au32 nnotify;
    uint64_t last_pump_pos;
    au32 create_ret; /* W89: Erzeuger-Ruecksprung (0x2435FC = Bink, 0x106C59 = SFX-Pool) */
    au32 dbg_last_status; /* W89 DIAG(SFX): letzter gemeldeter GetStatus (+1, 0 = nie) */
    const char *dbg_set_what[8]; /* W89 DIAG(SFX): Setter-Dedup (letzter Wert je Art) */
    au32 dbg_set_val[8], dbg_set_val2[8];
    double dbg_energy; /* W89 DIAG(SFX): Mix-Energie seit letzter Statistik */
    /* W89 DS3D (audio_3d, audio_3d.c): erst aktiv, wenn das Spiel positioniert */
    int is3d;
    au32 mode3d; /* 0 NORMAL, 1 HEADRELATIVE, 2 DISABLE */
    float p3[3], min3d, max3d;
    float g3d, pan3l, pan3r; /* vom Mixer pro Chunk berechnet */
    float dbg_d3; /* letzte Distanz (DIAG) */
    uint64_t dbg_eframes;
    uint64_t play_mix; /* W95 DIAG(SFXN): g_mixed_total beim letzten Play */
    au32 play_flags; /* W95: Flags des letzten Play */
    /* W102 AUDIO-TUT: Voice-Filter (SetFilter, DSFILTERDESC) + Pegel-Diag */
    au32 flt_mode, flt_q, flt_c[4]; /* letzter DSFILTERDESC (Mode/Q/Koeff.) */
    au32 flt_n; /* SetFilter-Aufrufe */
    float flt_f, flt_qd; /* SVF-Koeffizienten @MIXRATE (audio_filter) */
    float svf_lp[2], svf_bp[2]; /* SVF-Zustand L/R */
    int has_i3dl2; /* W102 DIAG: SetI3DL2Source gesehen */
    int i3d_direct, i3d_directhf, i3d_room, i3d_roomhf; /* lDirect/HF, lRoom/HF (1/100 dB) */
    au32 desc_bins; /* W102 DIAG: Mixbin-Zeiger im Erzeuger-Desc */
    au32 eg_p, eg_n; /* W102 DIAG: letzter SetEG-Desc, Aufrufe */
    double lv_e, lv_eraw; /* W102 DIAG(LVL): Energie (nach/vor Filter) seit letzter Sekunde */
    uint64_t lv_fr; /* Frames mit Daten seit letzter Sekunde */
    double lv_etot; /* Energie gesamt */
    au32 lv_secs; /* Sekunden mit Pegel > -60 dBFS */
    au32 mix_pass; /* PERF(PERF-TICK,W168) audio_mix_yield: in diesem Mischdurchgang schon gemischt */
    int ucat; /* W303 AUDIO-VOLUME: explizite Nutzerkategorie (AVOL_*, audio_vol.h; 0 = automatisch) */
    /* ISSUE-10 (audio_ring.c, audio_ring_underrun): Unterlauf/Cursor-Reset der Ringstimmen */
    int ring_fed; /* Unlock-Daten seit Erzeugung angehaengt */
    int ring_starved; /* FIFO lief leer bzw. Reset: naechster FIFO-Ton wird eingeblendet */
    au32 ring_ramp; /* verbleibende Einblend-Frames */
    au32 ring_hold; /* nach Reset: ausgeblendeter Rest, ohne Faktor */
    int ring_once; /* Stimme hat schon Frames gespielt (vor einem Cursor-Reset) */
    /* W343 DIAG(PITCH) (audio_pitch_diag): Frequenz-Setter je Stimme seit letzter Zeile */
    au32 pd_nf, pd_np; /* SetFrequency- / SetPitch-Aufrufe */
    int pd_pmin, pd_pmax; /* Pitch-Rohwerte */
    au32 pd_fmin, pd_fmax; /* angeforderte Frequenz (Hz) */
    au32 pd_drop, pd_clamp, pd_orig; /* verworfen / Schritt geklemmt / DSBFREQUENCY_ORIGINAL */
    int pd_lmin, pd_lmax; /* Pitch seit Erzeugung (audio_pitch_diag=2: Solo bei lmin != lmax) */
    au32 pd_eg_h[2]; /* letzter Inhalts-Hash je EG (Amplitude/Multi) */
    /* W348 AUDIO-DSP (audio_fx.c, audio_3d.c) */
    au32 cflags; /* DSBUFFERDESC/DSSTREAMDESC dwFlags beim Erzeugen */
    float vel3[3]; /* DS3D-Geschwindigkeit (audio_doppler) */
    float dop; /* Doppler-Faktor der Tonhoehe (1 = aus), vom Mischer je Chunk */
    float distf, dopf; /* Xbox-DS3DBUFFER DistanceFactor/DopplerFactor der Stimme (Standard 1) */
    au32 eg[2][8]; /* SetEG je EG (0 = Multi/EF, 1 = Amplitude/EA): mode, delay, attack, hold, decay, release, sustain, gesetzt */
    int eg_ps, eg_fc; /* EF Pitch-/Filter-Skala (signed 8 Bit) */
    int eg_ph[2]; /* laufende Phase je EG (DSEG_MODE_*) */
    double eg_t[2]; /* Zeit in der Phase (s) */
    float eg_lv[2]; /* Pegel 0..1 je EG */
    float eg_rel0[2]; /* Pegel beim Release-Beginn */
    float eg_ea0; /* EA-Pegel am Chunk-Anfang (Rampe) */
    float step_mul; /* Doppler x EF-Pitch (1 = neutral) */
    float rv_send, rv_sendhf; /* Hall-Send linear (lRoom) und HF-Faktor (lRoomHF) */
    float rv_lp[2]; /* Zustand HF-Daempfung des Sends */
    int rv_bin_mb; /* Pegel des I3DL2-Mixbins (DSMIXBIN_I3DL2 = 10) in mB, Standard 0 */
    /* W356 AUDIO-2 (audio_fx.c, audio_spatial.c) */
    float i3d_rolloff; /* DSI3DL2BUFFER flRoomRolloffFactor der Quelle */
    int i3d_obs, i3d_occ; /* Obstruction/Occlusion lHFLevel (mB) */
    float i3d_obs_lfr, i3d_occ_lfr; /* flLFRatio (LF-Daempfung = lHFLevel x Ratio) */
    float oc_lp[2]; /* Zustand HF-Daempfung Direktpfad (audio_occlusion) */
    float dir3[3]; /* Richtung zur Quelle im Hoerer-Raum (x rechts, y oben, z vorn), normiert; vom Mischer */
    ausp_hrtf_t hr; /* HRTF-Zustand (audio_hrtf) */
    float *fifox; /* 5.1-Quellen: C/LFE/BL/BR je FIFO-Frame (gleiche Indizes wie fifo), sonst NULL */
    float *datax; /* 5.1-Quellen: C/LFE/BL/BR je Daten-Frame (SetBufferData), sonst NULL */
    struct audio_voice *next;
} audio_voice_t;

typedef struct {
    void *vtable;
    au32 magic;
    LONG refcount;
} audio_ds_t;

/* ---- Geteilte Globals (Definition in audio_out.c) ---- */
extern audio_voice_t *g_voices;
extern HANDLE g_mix_thread;
extern HWAVEOUT g_wo;
extern int g_wo_ok;
extern LONG g_wo_queued;
extern FILE *g_wav;
extern au32 g_wav_bytes;
extern char g_wav_path[600];
extern uint64_t g_mixed_total;
extern au32 g_stat_packets, g_stat_bytes, g_stat_drops, g_stat_voices;
extern au32 g_stat_bufplay, g_stat_mixbin, g_stat_freq, g_stat_wa_under;
extern float g_peak;
extern au32 g_stat_stop; /* W95: Stop auf spielender Puffer-Stimme */
extern au32 g_stat_cut, g_stat_end; /* W89: SFX abgeschnitten (neue Daten waehrend Wiedergabe) / natuerlich zu Ende */

/* W343 DIAG(PITCH): Zeile je Stimme mit Frequenz-Settern (alle 10 s, audio_pitch_diag) */
void audio_pitch_diag_log(void);

/* W89 DIAG(SFX): Ereignis-Spur einer Puffer-Stimme (audio_sfx_diag) */
void au_sfx_trace(audio_voice_t *v, const char *what, au32 a, au32 b, au32 c, au32 ret);

/* ---- audio_out.c ---- */
void au_lock(void);
void au_unlock(void);

/* ---- audio_fmt.c ---- */
int au_probe_read(au32 p, void *dst, au32 n);
int au_probe_write(au32 p, const void *src, au32 n);
int au_wfx_ok(const wfx_t *w);
int au_parse_desc(au32 kind, au32 desc, wfx_t *wfx, au32 *buflen, au32 *flags);
au32 au_adpcm_decode(const uint8_t *src, au32 nbytes, int ch, float *dst, au32 dst_cap_frames);
au32 au_pcm_convert(const uint8_t *src, au32 nbytes, int ch, int bits, au32 src_rate, float *dst, au32 dst_cap_frames);
au32 au_fifo_append(audio_voice_t *v, const float *s, au32 n);
/* W356 AUDIO-2: 5.1-Quellen getrennt (x = C/LFE/SL/SR je Frame), s. audio_fmt.c */
au32 au_fifo_append_x(audio_voice_t *v, const float *s, const float *x, au32 n);
au32 au_pcm_convert_x(const uint8_t *src, au32 nbytes, int ch, int bits, au32 src_rate, float *dst, float *xdst,
                      au32 dst_cap_frames, int norm);
int au_keep51(void); /* audio_mix.c: 5.1-Quellen ungefaltet speichern (Surround moeglich) */
void ausp_log_stats(void); /* audio_mix.c: Raumklang-Modus, HRTF-Kosten */

/* ---- audio_3d.c (W89) ---- */
void au3d_listener_pos(au32 x, au32 y, au32 z);
void au3d_listener_orient(au32 fx, au32 fy, au32 fz, au32 tx, au32 ty, au32 tz);
void au3d_rolloff(au32 r);
void au3d_voice_pos(audio_voice_t *v, au32 x, au32 y, au32 z);
void au3d_voice_dist(audio_voice_t *v, au32 mind, au32 maxd);
void au3d_voice_mode(audio_voice_t *v, au32 mode);
void au3d_voice_allparams(audio_voice_t *v, const au32 *d);
void au3d_update(audio_voice_t *v);
void au3d_log_stats(void);

void au3d_doppler(audio_voice_t *v); /* W348: v->dop (audio_doppler) */
void au3d_voice_vel(audio_voice_t *v, au32 x, au32 y, au32 z);

/* ---- audio_fx.c (W348 AUDIO-DSP) ---- */
void aufx_voice_play(audio_voice_t *v); /* Huellkurven starten (Play) */
void aufx_eg_set(audio_voice_t *v, const au32 *d); /* DSENVELOPEDESC */
void aufx_note_stopex(audio_voice_t *v, au32 flags); /* W456 DIAG (SB-StopEx); au_lock */
int aufx_voice_release(audio_voice_t *v); /* W456: EA in die Release-Phase (1) oder keine laufende EA mit Release (0); au_lock */
void aufx_chunk(audio_voice_t *v, au32 n); /* je Chunk: EG weiter, step_mul, eg_ea0 -> eg_lv[1] */
void aufx_reverb_begin(au32 n); /* Send-Puffer leeren */
float *aufx_reverb_send(void); /* Stereo-Send-Puffer (n Frames) oder NULL */
void aufx_reverb_end(float *mix, au32 n); /* Hall rechnen, in mix addieren */
float aufx_voice_send(audio_voice_t *v); /* Send-Pegel (linear) je Chunk, setzt rv_sendhf */
/* W356 (audio_occlusion): Direktpfad der I3DL2-Quelle (lDirect + Obstruction/Occlusion), Pegel linear,
 * *a_hf = Ein-Pol-Tiefpass-Koeffizient fuer die HF-Daempfung (1 = keine). */
float aufx_voice_direct(audio_voice_t *v, float *a_hf);
/* W356 Surround: wie aufx_reverb_end, rear != NULL: Nachhall auf vorn/hinten verteilt (je -3 dB,
 * hinten eigene FDN-Kombination), rear = Stereo-Paar BL/BR (n Frames, additiv) */
void aufx_reverb_end2(float *mix, float *rear, au32 n);
void aufx_log_stats(void);

/* ---- audio_out.c (W102) ---- */
void au_filter_coeffs(audio_voice_t *v);

/* ---- audio_ring.c (ISSUE-10) ---- */
au32 au_ring_reset_flush(audio_voice_t *v);
float au_ring_gain(audio_voice_t *v);
int au_ring_first_start(const audio_voice_t *v, int fix);
uint64_t au_ring_place(uint64_t ao, au32 n, uint64_t front, uint64_t cur, au32 buflen);

/* ---- audio_mix.c ---- */
void au_complete_due(audio_voice_t *v, int all, au32 st);
uint64_t au_src_pos(audio_voice_t *v);
void au_ensure_thread(void);

#endif /* AUDIO_INT_H */
