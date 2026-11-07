/* audio_out.h - DirectSound-HLE: Stimmen, PCM-Mixer, WinMM-Ausgabe, WAV-Capture.
 * W4/A7: Spiel-Puffer/Streams werden als HLE-Stimmen gefuehrt (eigene Vtable),
 * PCM (auch Xbox-ADPCM, mehrkanalig) wird nach Stereo/44100 gemischt und per
 * WinMM ausgegeben; audio_capture=1 schreibt zusaetzlich <out>/audio.wav.
 * Alle Funktionen sind NULL-tolerant und geben bei fremden Zeigern S_OK-
 * vernuenftige Defaults (nie Crash). */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t au32;

/* Erzeugung: desc = DSBUFFERDESC/DSSTREAMDESC im Spielspeicher (wird defensiv
 * geparst: WAVEFORMATEX-Zeiger wird validiert). Rueckgabe: Stimme (HLE-Zeiger,
 * [0]=Vtable, [1]=Magic) oder 0. */
au32 audio_buffer_create(au32 desc, au32 retaddr);
au32 audio_stream_create(au32 desc, au32 retaddr);
au32 audio_ds_create(void); /* HLE-DirectSound-Objekt */
int audio_is_voice(au32 p); /* Magic-Probe (SEH-gesichert) */
int audio_is_ds(au32 p);
void audio_addref(au32 v);
void audio_release(au32 v);
void audio_ds_release(au32 o);

/* Datenpfad Puffer */
void audio_set_buffer_data(au32 v, au32 pdata, au32 bytes);
void audio_play(au32 v, au32 flags);
void audio_stop(au32 v);
au32 audio_get_status(au32 v); /* DSBSTATUS-Bits */
void audio_get_pos(au32 v, au32 pPlay, au32 pWrite);
void audio_set_pos(au32 v, au32 byteOff);
au32 audio_lock(au32 v, au32 off, au32 bytes, au32 pp1, au32 pb1, au32 pp2,
                au32 pb2, au32 flags); /* S_OK / E_FAIL */
au32 audio_unlock(au32 v, au32 p1, au32 b1, au32 p2, au32 b2);
/* Datenpfad Stream (Vtable-Slots 2/3/4) */
void audio_get_info(au32 v, au32 pInfo);
void audio_process(au32 v, au32 pInPkt, au32 pOutPkt); /* nimmt immer an (Xbox); Vollzug async (audio_process_async) */
/* Setter (werden gespeichert, beeinflussen den Mixer) */
void audio_set_volume(au32 v, int db100); /* Hundertstel-dB, 0 = max */
void audio_set_format(au32 v, au32 pwfx);
void audio_set_frequency(au32 v, au32 hz);
void audio_set_notify(au32 v, au32 count, au32 pNotifies);
void audio_set_loop(au32 v, au32 start, au32 len);
void audio_set_mixbins(au32 v, au32 pMixBins); /* volle Zuweisung (A7B audio_pan) */
void audio_set_mixbin_volumes(au32 v, au32 pMixBins); /* Update (A7B audio_pan) */
void audio_set_headroom(au32 v, au32 headroom); /* zusaetzliche Daempfung (audio_pan) */
void audio_set_pitch(au32 v, int pitch); /* Xbox-Pitch -> Frequenz (audio_freq) */
void audio_set_play_region(au32 v, au32 start, au32 len);
void audio_pause(au32 v, au32 pause);
void audio_flush(au32 v);
void audio_duplicate(au32 v, au32 ppOut);
void audio_get_caps(au32 o, au32 pCaps);
void audio_dowork(void); /* Notification-Pumpe (aus DirectSoundDoWork) */

/* Stream-Vtable (16 Slots, Layout wie Original @0x26F4A4); Slot-Funktionen
 * stehen in audio_out.c (feste __stdcall-Signaturen pro Slot). */
extern void *g_audio_stream_vtable[16];
/* Puffer-Vtable (nichts ruft sie auf, reine Absicherung): alle Slots S_OK. */
extern void *g_audio_buffer_vtable[16];

/* W89 DIAG(SFX): Ereignis an einer Stimme protokollieren (audio_sfx_diag > 0;
 * nur Puffer ausser Bink). ret = Ruecksprung ins Spiel. */
void audio_sfx_evt(au32 v, const char *what, au32 a, au32 b, au32 c, au32 ret);

/* W89 DS3D (audio_3d): Stimmen-/Listener-Setter (Float-Bits wie vom Spiel).
 * which: 0 = MinDistance, 1 = MaxDistance. */
void audio_3d_voice_pos(au32 v, au32 x, au32 y, au32 z);
void audio_3d_voice_allparams(au32 v, au32 pParams);
void audio_3d_voice_dist(au32 v, int which, au32 f);
void audio_3d_listener_pos(au32 x, au32 y, au32 z);
void audio_3d_listener_orient(au32 fx, au32 fy, au32 fz, au32 tx, au32 ty, au32 tz);
void audio_3d_rolloff(au32 r);

/* W102 AUDIO-TUT: Voice-Filter (DSFILTERDESC: Mode, Q, 4 Koeffizienten; Mode 1
 * = DLS2-Tiefpass, wirkt bei audio_filter=1), I3DL2-Quelle (DSI3DL2BUFFER, nur
 * Diag), SetEG (nur Diag). */
void audio_set_filter(au32 v, au32 pDesc);
void audio_set_i3dl2(au32 v, au32 pParams);
void audio_note_eg(au32 v, au32 pDesc);

/* Statistik fuers Log (einmalig pro Lauf bei Bedarf). */
void audio_log_stats(void);

#ifdef __cplusplus
}
#endif
