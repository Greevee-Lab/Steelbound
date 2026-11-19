/* audio_spatial.h - W356 AUDIO-2: 3D-Klang ohne Fremddaten (reine DSP, ohne Spiel-/Windows-Bezug).
 *
 * 1. HRTF fuer Kopfhoerer (audio_hrtf): analytisches Kugelkopf-Modell nach Brown & Duda
 *    ("A Structural Model for Binaural Sound Synthesis", IEEE Trans. Speech Audio Proc. 6(5), 1998):
 *    je Ohr Laufzeit (Woodworth, Kopfradius 8,75 cm) und Kopfschatten-Filter erster Ordnung
 *    H(s) = (1 + alpha s / 2w0) / (1 + s / 2w0), w0 = c/a, alpha(theta) = 1,05 + 0,95 cos(1,2 theta)
 *    (theta = Winkel Quelle/Ohrachse, alpha_min 0,1 bei 150 Grad). Dazu fuer Quellen hinten eine
 *    Hoehendaempfung (Ohrmuschel-Schatten, bis -6 dB bei 4 kHz), damit vorn/hinten unterscheidbar
 *    sind. Keine gemessenen Impulsantworten, keine Fremddaten (Formeln aus der Literatur).
 * 2. Surround (audio_surround): Pegel je Ecke FL/FR/BL/BR (konstante Leistung, Paar-Panning
 *    zwischen den Lautsprechern bei +-30 und +-110 Grad) wie die vierkanalige 3D-Ausgabe der Xbox.
 * 3. 5.1-Faltung (audio_downmix): ITU-R BS.775 (C und Surround -3 dB, LFE -6 dB), normiert.
 * Selbsttest: tests/audio_reverb_selftest.c. */
#ifndef AUDIO_SPATIAL_H
#define AUDIO_SPATIAL_H
#include <stdint.h>

#define AUSP_HR_DL 64 /* Verzoegerungsleitung (> 0,66 ms bei 44,1 kHz + Interpolation) */

typedef struct {
    float dl[AUSP_HR_DL]; /* Mono-Eingang (Verzoegerungsleitung) */
    uint32_t w;
    int init; /* 0 = Parameter beim naechsten Block ohne Rampe setzen */
    float t[2]; /* Laufzeit je Ohr (Samples) am Blockende: 0 links, 1 rechts */
    float b0[2], b1[2], a1[2]; /* Kopfschatten je Ohr */
    float x1[2], y1[2]; /* Filterzustand */
    float ra; /* Ein-Pol-Tiefpass Rueckseite (1 = aus) */
    float rs[2];
} ausp_hrtf_t;

/* Parameter fuer eine Richtung (x rechts, y oben, z vorn; normiert). fs = Abtastrate. */
void ausp_hrtf_params(const float dir[3], float fs, float t[2], float b0[2], float b1[2], float a1[2], float *ra);
/* Block: in = Mono (n Frames), out = Stereo additiv. Parameter rampen linear ueber den Block. */
void ausp_hrtf_block(ausp_hrtf_t *h, const float dir[3], float fs, const float *in, uint32_t n, float *out);

/* Surround-Gewichte FL, FR, BL, BR fuer eine Richtung (Summe der Quadrate = 1). */
void ausp_quad_gains(const float dir[3], float g[4]);

/* 5.1 (FL FR C LFE BL BR) -> Stereo. norm = 1: ITU-Koeffizienten, auf Vollaussteuerung samt
 * XDK-2D-Headroom (-6 dB) normiert (Faktor 2/2,914); norm = 0: alte Faltung (BL/BR 0,707, LFE 0,5, x1). */
void ausp_downmix51(const float *m, int norm, float *l, float *r);
float ausp_downmix_norm(int norm);

#endif
