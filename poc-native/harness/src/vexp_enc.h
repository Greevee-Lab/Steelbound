#pragma once
/* vexp_enc.h - W255 VIDEO-EXPORT (experimentell): MP4-Encoder (Media Foundation
 * Sink Writer, H.264 + optional AAC) und Zeitachse, ohne D3D/Spiel (Selbsttest
 * tests/vexp_selftest.c). Eine Instanz je Prozess.
 *
 * Bilder: RGBA8 (R,G,B,A je Byte), w*h*4, oben links zuerst. Der Encoder
 * wandelt in NV12 (BT.709, 16..235) und schreibt jedes Bild repeat-mal mit
 * fester Bildrate (konstante Zeitachse: Bild n = n/fps s).
 * Ton: PCM s16 stereo 44100 Hz (Mischer-Format), beliebiger Thread. Die Spur
 * wird beim Schliessen auf genau Bilder/fps Sekunden gekuerzt bzw. mit Stille
 * aufgefuellt; vxe_audio_skip verwirft vorab N fuehrende Frames (Versatz
 * Tonstart -> erstes Bild). */
#include <wchar.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VXE_RATE 44100u

/* Startet Encoder-Thread und Sink Writer. path = Zieldatei (Container immer
 * MP4, Endung egal). hw: 1 = Hardware-MFT erlauben (Rueckfall Software).
 * info: kurze Beschreibung ("H264 HW 1920x1080 30fps 12000kbit/s AAC").
 * Rueckgabe 1 = bereit, 0 = Fehler (info nennt hr). */
int vxe_open(const wchar_t *path, unsigned w, unsigned h, unsigned fps, unsigned kbps, int audio, int hw,
             char *info, size_t cap);
/* Freier Bildpuffer (RGBA, w*h*4); blockiert bis frei. NULL = Encoder tot. */
unsigned char *vxe_frame_begin(void);
/* Puffer aus vxe_frame_begin abgeben: repeat Bilder (>= 1). */
void vxe_frame_end(unsigned repeat);
/* Ton anhaengen (frames Stereo-Frames). Bei vollem Puffer wartet der Aufrufer
 * hoechstens ~1 s, danach wird verworfen (gezaehlt). */
void vxe_audio(const short *pcm, unsigned frames);
void vxe_audio_skip(unsigned frames);
/* Abschluss: Rest schreiben, Finalize. Rueckgabe 1 = gueltige Datei.
 * frames/seconds = geschriebene Bilder/Dauer, afr = geschriebene Ton-Frames,
 * adrop = verworfene Ton-Frames (Ueberlauf). */
int vxe_close(unsigned *frames, double *seconds, unsigned long long *afr, unsigned long long *adrop);
/* Abbruch ohne gueltige Datei (Datei bleibt unvollstaendig, Aufrufer loescht). */
void vxe_abort(void);
int vxe_failed(void);
/* letztes HRESULT des Encoders (auch nach vxe_close/vxe_abort) */
unsigned long vxe_hr(void);
/* Encoder-Durchsatz fuers Log: mittlere Wandel-+Schreibzeit je Bild in ms. */
double vxe_ms_per_frame(void);

/* ---- Zeitachse (rein, Selbsttest) ----
 * Eingangsbilder tragen eine Zeit T (VBlanks, steigend); Ausgabebild n zeigt
 * das letzte Eingangsbild mit T <= t0 + n*step (Halten). vx_tl_feed liefert,
 * wie viele Ausgabebilder das VORIGE Eingangsbild belegt; danach gilt das neue
 * als vorig. Das erste Bild legt t0 fest. */
typedef struct {
    double t0, step;
    unsigned n;
    int started;
} vx_tl_t;
void vx_tl_init(vx_tl_t *t, double step);
unsigned vx_tl_feed(vx_tl_t *t, double T);
unsigned vx_tl_end(vx_tl_t *t, double T_end);
/* Bild k (1..nimg) eines Swaps zwischen Feldzaehler c0 und c1. */
double vx_batch_time(double c0, double c1, unsigned k, unsigned nimg);
/* RGBA -> NV12 (BT.709, begrenzter Bereich), w/h gerade. */
void vx_rgba_to_nv12(const unsigned char *rgba, unsigned w, unsigned h, unsigned char *nv12);
/* Auto-Bitrate in kbit/s (0,15 bit/Pixel: 1080p30 ~9,3, 1440p60 ~33, 2160p60 ~75 Mbit/s; 2000..100000). */
unsigned vx_auto_kbps(unsigned w, unsigned h, unsigned fps);

#ifdef __cplusplus
}
#endif
