/* audio_reverb_selftest.c - W348 AUDIO-DSP: Selbsttest I3DL2-Hall und Huellkurven
 * (src/audio_fx.c), synthetisch, ohne Spiel und ohne Spieldaten.
 *
 * Hall (Impuls bzw. Rauschen ueber den Send-Puffer, wie der Mischer):
 *  1. Nachhallzeit: Schroeder-Rueckwaertsintegration (-5..-35 dB) gegen flDecayTime
 *     (1,0 / 2,7 / 5,0 s, flDecayHFRatio 1,0), Toleranz +-10 %.
 *  2. HF-Daempfung: flDecayHFRatio 0,5 -> Hochpass-Anteil klingt schneller ab.
 *  3. Pegel: Energie Nachhall = lReverb^2 (+-1,5 dB), Reflexionen = lReflections^2
 *     (+-1 dB), lRoom wirkt als Faktor, Raum -10000 = aus (exakt 0).
 *  4. Keine Uebersteuerung: Impuls 1,0 -> Spitze < 1; Rauschen -6 dBFS mit den
 *     Spiel-Presets (Polsterzelle, Steingang, Stadt) -> Spitze < 1, endlich.
 *  5. Stabilitaet: Grenzwerte (20 s, HF-Ratio 2,0, Dichte/Diffusion 0 und 100)
 *     30 s Rauschen + 60 s Stille -> endlich, Rest < -60 dB.
 *  6. Vorverzoegerung: erste Ausgabe nicht vor flReflectionsDelay.
 *  7. Kosten je 10-ms-Block < 0,5 ms.
 * Huellkurven (aufx_voice_play/aufx_chunk):
 *  8. Amplitude: Delay/Attack/Hold/Decay/Sustain zu den erwarteten Zeiten (+-1 Chunk).
 *  9. Multi-EG: lPitchScale -128 bei Sustain 255 -> Tonhoehe x0,5, OFF -> x2^(ps/128).
 * Exit 0 = alles gruen. */
#include "../src/audio_int.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

hcfg_t g_cfg;
int g_sba_loc; /* sb_profile.c */
uint64_t g_mixed_total; /* audio_mix.c */
void au_lock(void) {}
void au_unlock(void) {}
void hlog_msg(int level, const char *fmt, ...)
{
    (void)level;
    (void)fmt;
}

void aufx_test_reset(void);

static int g_fail;
#define CHECK(c, ...)                                     \
    do {                                                  \
        if (!(c)) {                                       \
            printf("FEHLER %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
            g_fail++;                                     \
        }                                                 \
    } while (0)

#define BLK 441u /* 10 ms */

static void lis(au32 *d, int room, int roomhf, float decay, float hfr, int refl, float rd, int rev, float vd,
                float diff, float dens)
{
    float f;
    d[0] = (au32)room;
    d[1] = (au32)roomhf;
    f = 0.0f;
    memcpy(&d[2], &f, 4);
    memcpy(&d[3], &decay, 4);
    memcpy(&d[4], &hfr, 4);
    d[5] = (au32)refl;
    memcpy(&d[6], &rd, 4);
    d[7] = (au32)rev;
    memcpy(&d[8], &vd, 4);
    memcpy(&d[9], &diff, 4);
    memcpy(&d[10], &dens, 4);
    f = 5000.0f;
    memcpy(&d[11], &f, 4);
}

static unsigned g_rng = 12345u;
static float noise(void)
{
    g_rng = g_rng * 1664525u + 1013904223u;
    return ((float)(g_rng >> 8) / 8388608.0f) - 1.0f;
}

/* Laesst den Hall 'secs' Sekunden laufen; Eingang: imp (Impuls bei Frame 0) oder
 * Rauschen mit Amplitude namp in den ersten nsecs Sekunden. Ausgabe Stereo. */
static float *run(const au32 *d, double secs, int imp, float namp, double nsecs, double *ms_per_10)
{
    au32 nb = (au32)(secs * MIXRATE / BLK), b, i;
    float *out = (float *)calloc((size_t)nb * BLK * 2, sizeof(float));
    LARGE_INTEGER q0, q1, qf;
    aufx_test_reset();
    audio_set_i3dl2_listener(d, 0);
    QueryPerformanceFrequency(&qf);
    QueryPerformanceCounter(&q0);
    for (b = 0; b < nb; b++) {
        float *s, *m = out + (size_t)b * BLK * 2;
        aufx_reverb_begin(BLK);
        s = aufx_reverb_send();
        if (s) {
            if (imp && b == 0)
                s[0] = s[1] = 1.0f;
            if (namp > 0.0f && b * BLK < nsecs * MIXRATE)
                for (i = 0; i < BLK; i++) {
                    float x = noise() * namp;
                    s[i * 2] = x;
                    s[i * 2 + 1] = 0.7f * x + 0.3f * noise() * namp;
                }
        }
        aufx_reverb_end(m, BLK);
    }
    QueryPerformanceCounter(&q1);
    if (ms_per_10)
        *ms_per_10 = (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)qf.QuadPart / nb;
    return out;
}

static double energy(const float *o, size_t f0, size_t f1)
{
    double e = 0.0;
    size_t i;
    for (i = f0; i < f1; i++)
        e += (double)o[i * 2] * o[i * 2] + (double)o[i * 2 + 1] * o[i * 2 + 1];
    return e;
}

/* Schroeder: T60 aus der Steigung -5..-35 dB der rueckwaerts integrierten Energie (Summe L+R). */
static double t60(const float *o, size_t n)
{
    double *edc = (double *)malloc(n * sizeof(double)), acc = 0.0, tot, t5 = -1, t35 = -1;
    size_t i;
    for (i = n; i-- > 0;) {
        acc += (double)o[i * 2] * o[i * 2] + (double)o[i * 2 + 1] * o[i * 2 + 1];
        edc[i] = acc;
    }
    tot = edc[0];
    for (i = 0; i < n; i++) {
        double db = 10.0 * log10(edc[i] / tot + 1e-30);
        if (t5 < 0 && db <= -5.0)
            t5 = (double)i / MIXRATE;
        if (t35 < 0 && db <= -35.0) {
            t35 = (double)i / MIXRATE;
            break;
        }
    }
    free(edc);
    if (t5 < 0 || t35 < 0)
        return -1.0;
    return (t35 - t5) * 2.0;
}

static float peak(const float *o, size_t n, int *bad)
{
    float p = 0.0f;
    size_t i;
    for (i = 0; i < n * 2; i++) {
        float a = fabsf(o[i]);
        if (!(o[i] == o[i]) || a > 1e30f)
            (*bad)++;
        else if (a > p)
            p = a;
    }
    return p;
}

static void test_reverb(void)
{
    au32 d[12];
    static const float decays[3] = { 1.0f, 2.7f, 5.0f };
    int k, bad = 0;
    double ms;
    g_cfg.audio_reverb = 1;
    /* 1. Nachhallzeit */
    for (k = 0; k < 3; k++) {
        float *o;
        double t;
        size_t n = (size_t)((decays[k] * 1.6 + 0.5) * MIXRATE / BLK) * BLK;
        lis(d, 0, 0, decays[k], 1.0f, -10000, 0.007f, 0, 0.011f, 100.0f, 100.0f);
        o = run(d, (double)n / MIXRATE, 1, 0.0f, 0.0, NULL);
        t = t60(o, n);
        printf("T60: Soll %.2f s, gemessen %.3f s (%+.1f %%)\n", decays[k], t, (t / decays[k] - 1.0) * 100.0);
        CHECK(t > 0 && fabs(t / decays[k] - 1.0) <= 0.10, "T60 %.3f statt %.2f", t, decays[k]);
        free(o);
    }
    /* 2. HF-Daempfung: Hochpass (Differenz) klingt bei HF-Ratio 0,5 schneller ab als breitbandig */
    {
        size_t n = (size_t)(3.0 * MIXRATE / BLK) * BLK, i;
        float *o, *h;
        double tb, th;
        lis(d, 0, 0, 1.5f, 0.5f, -10000, 0.007f, 0, 0.011f, 100.0f, 100.0f);
        o = run(d, (double)n / MIXRATE, 1, 0.0f, 0.0, NULL);
        h = (float *)calloc(n * 2, sizeof(float));
        for (i = 1; i < n; i++) {
            h[i * 2] = o[i * 2] - o[(i - 1) * 2];
            h[i * 2 + 1] = o[i * 2 + 1] - o[(i - 1) * 2 + 1];
        }
        tb = t60(o, n);
        th = t60(h, n);
        printf("HF-Ratio 0,5: T60 breit %.3f s, Hochpass %.3f s\n", tb, th);
        CHECK(th > 0 && th < 0.85 * tb, "HF-Abklingen %.3f nicht schneller als %.3f", th, tb);
        free(o);
        free(h);
    }
    /* 3. Pegel (Nachhall-Energie unabhaengig von der Nachhallzeit) */
    {
        for (k = 0; k < 3; k++) {
            size_t n2 = (size_t)((decays[k] * 1.6 + 0.5) * MIXRATE / BLK) * BLK;
            float *o2;
            double db2;
            lis(d, 0, 0, decays[k], 1.0f, -10000, 0.007f, 0, 0.011f, 100.0f, 100.0f);
            o2 = run(d, (double)n2 / MIXRATE, 1, 0.0f, 0.0, NULL);
            db2 = 10.0 * log10(energy(o2, 0, n2) / 2.0);
            printf("Nachhall-Energie T60 %.1f s: %+.2f dB\n", decays[k], db2);
            CHECK(fabs(db2) <= 1.5, "Nachhall-Energie %+.2f dB bei T60 %.1f", db2, decays[k]);
            free(o2);
        }
    }
    {
        size_t n = (size_t)(4.0 * MIXRATE / BLK) * BLK;
        float *o;
        double e, db;
        lis(d, 0, 0, 1.5f, 1.0f, -10000, 0.007f, 0, 0.011f, 100.0f, 100.0f);
        o = run(d, (double)n / MIXRATE, 1, 0.0f, 0.0, NULL);
        e = energy(o, 0, n);
        db = 10.0 * log10(e / 2.0); /* Eingang: Impuls 1 auf beiden Kanaelen = Energie 2 */
        printf("Nachhall-Energie bei lReverb 0: %+.2f dB\n", db);
        CHECK(fabs(db) <= 1.5, "Nachhall-Energie %+.2f dB statt 0", db);
        free(o);
        lis(d, 0, 0, 1.5f, 1.0f, -10000, 0.007f, -1000, 0.011f, 100.0f, 100.0f);
        o = run(d, (double)n / MIXRATE, 1, 0.0f, 0.0, NULL);
        e = energy(o, 0, n);
        db = 10.0 * log10(e / 2.0);
        printf("Nachhall-Energie bei lReverb -1000: %+.2f dB\n", db);
        CHECK(fabs(db + 10.0) <= 1.5, "Nachhall-Energie %+.2f dB statt -10", db);
        free(o);
        lis(d, 0, 0, 1.5f, 1.0f, -600, 0.007f, -10000, 0.011f, 100.0f, 100.0f);
        o = run(d, (double)n / MIXRATE, 1, 0.0f, 0.0, NULL);
        e = energy(o, 0, n);
        db = 10.0 * log10(e / 2.0);
        printf("Reflexions-Energie bei lReflections -600: %+.2f dB\n", db);
        CHECK(fabs(db + 6.0) <= 1.0, "Reflexions-Energie %+.2f dB statt -6", db);
        free(o);
        lis(d, -10000, 0, 1.5f, 1.0f, 0, 0.007f, 0, 0.011f, 100.0f, 100.0f);
        o = run(d, 1.0, 1, 0.0f, 0.0, NULL);
        e = energy(o, 0, (size_t)(1.0 * MIXRATE / BLK) * BLK);
        printf("Raum -10000: Energie %.3g\n", e);
        CHECK(e == 0.0, "Raum aus, aber Energie %.3g", e);
        free(o);
    }
    /* 4. Keine Uebersteuerung (Impuls + Spiel-Presets mit Rauschen -6 dBFS) */
    {
        static const float pr[3][10] = {
            { -1000, -6000, 0.17f, 0.10f, -1204, 0.001f, 207, 0.002f, 100, 100 },  /* Polsterzelle */
            { -1000, -237, 2.70f, 0.79f, -1214, 0.013f, 395, 0.020f, 100, 100 },   /* Steingang */
            { -1000, -800, 1.49f, 0.67f, -2273, 0.007f, -2217, 0.011f, 50, 100 }, /* Stadt */
        };
        size_t n = (size_t)(6.0 * MIXRATE / BLK) * BLK;
        float *o, p;
        lis(d, 0, 0, 2.7f, 1.0f, 0, 0.007f, 0, 0.011f, 100.0f, 100.0f);
        o = run(d, 3.0, 1, 0.0f, 0.0, NULL);
        p = peak(o, (size_t)(3.0 * MIXRATE / BLK) * BLK, &bad);
        printf("Impuls 1,0 (Raum 0, Hall 0 dB): Spitze %.3f\n", p);
        CHECK(p < 1.0f && !bad, "Impuls-Spitze %.3f", p);
        free(o);
        for (k = 0; k < 3; k++) {
            lis(d, (int)pr[k][0], (int)pr[k][1], pr[k][2], pr[k][3], (int)pr[k][4], pr[k][5], (int)pr[k][6], pr[k][7],
                pr[k][8], pr[k][9]);
            o = run(d, 6.0, 0, 0.5f * powf(10.0f, pr[k][0] / 2000.0f), 4.0, &ms); /* lRoom wirkt am Send */
            p = peak(o, n, &bad);
            printf("Preset %d, Rauschen -6 dBFS: Spitze %.3f, Kosten %.3f ms je 10 ms\n", k, p, ms);
            CHECK(p < 1.0f && !bad, "Preset %d Spitze %.3f", k, p);
            CHECK(ms < 0.5, "Kosten %.3f ms je 10 ms", ms);
            free(o);
        }
    }
    /* 5. Stabilitaet an den Grenzen */
    {
        static const float lim[4][3] = { { 20.0f, 2.0f, 100.0f }, { 20.0f, 0.1f, 0.0f }, { 0.1f, 2.0f, 0.0f },
                                         { 20.0f, 1.0f, 50.0f } };
        for (k = 0; k < 4; k++) {
            size_t n = (size_t)(90.0 * MIXRATE / BLK) * BLK, t0 = (size_t)(30.0 * MIXRATE / BLK) * BLK;
            float *o, p;
            double e0, e1;
            lis(d, 0, 0, lim[k][0], lim[k][1], 1000, 0.3f, 2000, 0.1f, lim[k][2], lim[k][2]);
            o = run(d, 90.0, 0, 0.5f, 30.0, NULL);
            bad = 0;
            p = peak(o, n, &bad);
            e0 = energy(o, t0 - MIXRATE, t0) + 1e-30;
            e1 = energy(o, n - MIXRATE, n);
            printf("Grenze %d (T60 %.1f, HF %.1f, Diff/Dichte %.0f): Spitze %.2f, Rest %.1f dB\n", k, lim[k][0],
                   lim[k][1], lim[k][2], p, 10.0 * log10(e1 / e0 + 1e-30));
            CHECK(!bad && p < 100.0f, "Grenze %d instabil (Spitze %.2f, bad %d)", k, p, bad);
            CHECK(e1 / e0 < 1e-6, "Grenze %d klingt nicht ab (%.1f dB)", k, 10.0 * log10(e1 / e0));
            free(o);
        }
    }
    /* 6. Vorverzoegerung */
    {
        size_t n = (size_t)(0.5 * MIXRATE / BLK) * BLK, i, first = 0;
        float *o;
        lis(d, 0, 0, 1.0f, 1.0f, 0, 0.1f, 0, 0.05f, 100.0f, 100.0f);
        o = run(d, 0.5, 1, 0.0f, 0.0, NULL);
        for (i = 0; i < n; i++)
            if (fabsf(o[i * 2]) + fabsf(o[i * 2 + 1]) > 1e-9f) {
                first = i;
                break;
            }
        printf("Erste Ausgabe nach %.1f ms (Reflexionsverzoegerung 100 ms)\n", first * 1000.0 / MIXRATE);
        CHECK(first >= (size_t)(0.099 * MIXRATE), "Hall vor der Vorverzoegerung (%zu)", first);
        free(o);
    }
}

static void eg_desc(au32 *d, au32 eg, au32 mode, au32 dl, au32 at, au32 ho, au32 de, au32 re, au32 su, int ps)
{
    memset(d, 0, 10 * 4);
    d[0] = eg;
    d[1] = mode;
    d[2] = dl;
    d[3] = at;
    d[4] = ho;
    d[5] = de;
    d[6] = re;
    d[7] = su;
    d[8] = (au32)ps;
}

static void test_eg(void)
{
    static audio_voice_t v;
    au32 d[10];
    int c;
    double unit = 512.0 / 48000.0;
    g_cfg.audio_eg = 1;
    memset(&v, 0, sizeof v);
    v.kind = 1;
    v.dop = 1.0f;
    /* EA: Delay 10, Attack 20, Hold 10, Decay 30, Sustain 128 */
    eg_desc(d, 1, 1, 10, 20, 10, 30, 10, 128, 0);
    aufx_eg_set(&v, d);
    v.playing = 1;
    aufx_voice_play(&v);
    for (c = 1; c <= 200; c++) {
        double t = c * (double)BLK / MIXRATE, want;
        aufx_chunk(&v, BLK);
        if (t < 10 * unit)
            want = 0.0;
        else if (t < 30 * unit)
            want = (t - 10 * unit) / (20 * unit);
        else if (t < 40 * unit)
            want = 1.0;
        else {
            want = exp(-6.91 * (t - 40 * unit) / (30 * unit));
            if (want < 128.0 / 255.0)
                want = 128.0 / 255.0;
        }
        if (fabs(v.eg_lv[1] - want) > 0.06 && fabs(t - 10 * unit) > 0.011 && fabs(t - 30 * unit) > 0.011 &&
            fabs(t - 40 * unit) > 0.011)
            CHECK(0, "EA t=%.3f Pegel %.3f statt %.3f", t, v.eg_lv[1], want);
    }
    CHECK(fabs(v.eg_lv[1] - 128.0f / 255.0f) < 1e-4, "Sustain %.4f", v.eg_lv[1]);
    printf("EA nach 2 s: Pegel %.3f (Sustain 128/255), Phase %d\n", v.eg_lv[1], v.eg_ph[1]);
    /* EF: Pitch-Skala -128 bei Sustain 255 -> x0,5 */
    memset(&v, 0, sizeof v);
    v.kind = 1;
    v.dop = 1.0f;
    eg_desc(d, 0, 5, 0, 0, 0, 0, 0, 255, -128);
    aufx_eg_set(&v, d);
    v.playing = 1;
    aufx_voice_play(&v);
    aufx_chunk(&v, BLK);
    printf("EF Sustain 255, Skala -128: Schritt x%.4f\n", v.step_mul);
    CHECK(fabsf(v.step_mul - 0.5f) < 1e-4f, "EF-Pitch %.4f statt 0,5", v.step_mul);
    /* EF im Zustand OFF (Modus 0) wirkt mit Pegel 1 (MCPX-Nachbau) */
    memset(&v, 0, sizeof v);
    v.kind = 1;
    v.dop = 1.0f;
    eg_desc(d, 0, 0, 0, 0, 0, 0, 0, 0, 64);
    aufx_eg_set(&v, d);
    v.playing = 1;
    aufx_voice_play(&v);
    aufx_chunk(&v, BLK);
    CHECK(fabsf(v.step_mul - 1.4142f) < 1e-3f, "EF OFF, Skala 64: %.4f statt 1,414", v.step_mul);
    /* W456 (LoC #93): Release beim StopEx(ENVELOPE) - TE017-Huellkurve (Attack 46, Release 562, Sustain 255, Modus 1):
     * nach 1 s Sustain 1,0; Release vom aktuellen Pegel exp(-6,91 t/T), Stimme nach T = 6,0 s aus. Ohne Release (0)
     * bzw. EA OFF: aufx_voice_release = 0 (Aufrufer stoppt sofort). */
    memset(&v, 0, sizeof v);
    v.kind = 1;
    v.dop = 1.0f;
    eg_desc(d, 1, 1, 0, 46, 0, 0, 562, 255, 0);
    aufx_eg_set(&v, d);
    v.playing = 1;
    aufx_voice_play(&v);
    CHECK(v.eg_lv[1] == 0.0f, "W456 Attack beginnt bei 0 (%.3f)", v.eg_lv[1]);
    for (c = 0; c < (int)(0.25 * MIXRATE / BLK); c++)
        aufx_chunk(&v, BLK);
    CHECK(v.eg_lv[1] > 0.4f && v.eg_lv[1] < 0.6f, "W456 Attack nach 0,25 s %.3f statt ~0,51", v.eg_lv[1]);
    for (; c < (int)(1.0 * MIXRATE / BLK); c++)
        aufx_chunk(&v, BLK);
    CHECK(fabsf(v.eg_lv[1] - 1.0f) < 1e-4f && v.eg_ph[1] == 5, "W456 Sustain %.3f Phase %d", v.eg_lv[1], v.eg_ph[1]);
    CHECK(aufx_voice_release(&v) == 1 && v.eg_ph[1] == 6, "W456 Release startet nicht");
    CHECK(aufx_voice_release(&v) == 0, "W456 zweites Release");
    {
        int n1 = (int)(1.0 * MIXRATE / BLK), nT = (int)(562 * unit * MIXRATE / BLK) + 2, k;
        for (k = 0; k < n1; k++)
            aufx_chunk(&v, BLK);
        CHECK(v.playing && fabs(v.eg_lv[1] - exp(-6.91 * 1.0 / (562 * unit))) < 0.02, "W456 Release nach 1 s %.3f",
              v.eg_lv[1]);
        printf("W456 Release (TE017, T=%.2f s): Pegel nach 1 s %.3f, ", 562 * unit, v.eg_lv[1]);
        for (; k < nT; k++)
            aufx_chunk(&v, BLK);
        CHECK(!v.playing && v.eg_lv[1] == 0.0f, "W456 Stimme nach Release nicht aus (spielt %d, %.4f)", v.playing,
              v.eg_lv[1]);
        printf("nach T aus\n");
    }
    memset(&v, 0, sizeof v);
    v.kind = 1;
    v.dop = 1.0f;
    eg_desc(d, 1, 1, 0, 0, 0, 0, 0, 255, 0); /* Release 0 */
    aufx_eg_set(&v, d);
    v.playing = 1;
    aufx_voice_play(&v);
    aufx_chunk(&v, BLK);
    CHECK(aufx_voice_release(&v) == 0, "W456 Release 0 muss sofort stoppen");
    eg_desc(d, 1, 0, 0, 0, 0, 0, 100, 255, 0); /* Modus OFF (Pool-Stimme ohne Huellkurve) */
    aufx_eg_set(&v, d);
    v.playing = 0;
    aufx_voice_play(&v);
    v.playing = 1;
    aufx_chunk(&v, BLK);
    CHECK(v.eg_lv[1] == 1.0f && aufx_voice_release(&v) == 0, "W456 EA OFF: Pegel %.3f, kein Release", v.eg_lv[1]);
    /* audio_eg=0: neutral */
    g_cfg.audio_eg = 0;
    aufx_chunk(&v, BLK);
    CHECK(v.step_mul == 1.0f && v.eg_lv[1] == 1.0f, "audio_eg=0 nicht neutral");
}

/* ---------------- W356 AUDIO-2 ---------------- */
static double db(double x)
{
    return 20.0 * log10(x > 1e-30 ? x : 1e-30);
}

/* Amplitude eines Sinus f nach dem Ein-Pol-Tiefpass a (eingeschwungen). */
static double lp_amp(float a, double f)
{
    float y = 0.0f;
    double mx = 0.0;
    int i;
    for (i = 0; i < MIXRATE; i++) {
        float x = (float)sin(2.0 * 3.14159265358979 * f * i / MIXRATE);
        y += a * (x - y);
        if (i > MIXRATE / 2 && fabs(y) > mx)
            mx = fabs(y);
    }
    return mx;
}

static void i3d_vals(audio_voice_t *v, int direct, int directhf, int obs, float obs_lfr, int occ, float occ_lfr)
{
    v->has_i3dl2 = 1;
    v->i3d_direct = direct;
    v->i3d_directhf = directhf;
    v->i3d_room = 0;
    v->i3d_roomhf = 0;
    v->i3d_obs = obs;
    v->i3d_obs_lfr = obs_lfr;
    v->i3d_occ = occ;
    v->i3d_occ_lfr = occ_lfr;
}

/* 10. Verdeckung: Direktpfad LF/HF nach I3DL2-Regel, Raumanteil nur bei Occlusion, Schalter aus = neutral. */
static void test_occlusion(void)
{
    static audio_voice_t v;
    au32 d[12];
    float g, a, s0, s1;
    double hf;
    g_cfg.audio_reverb = 1;
    g_cfg.audio_occlusion = 1;
    aufx_test_reset();
    lis(d, -1000, 0, 1.49f, 0.83f, -2602, 0.007f, 200, 0.011f, 100.0f, 100.0f); /* Raum an, hfref 5 kHz */
    audio_set_i3dl2_listener(d, 0);
    aufx_reverb_begin(BLK);
    memset(&v, 0, sizeof v);
    v.cflags = 0x10u;
    v.is3d = 1;
    /* SB-Wert aus dem Spiel: Occlusion -1044 mB, LF-Ratio 0,26 */
    i3d_vals(&v, 0, 0, 0, 0.0f, -1044, 0.26f);
    g = aufx_voice_direct(&v, &a);
    hf = lp_amp(a, 5000.0);
    printf("Occlusion -1044/0,26: Direkt LF %.2f dB (Soll %.2f), HF bei 5 kHz %.2f dB (Soll %.2f)\n", db(g),
           -1044 * 0.26 / 100.0, db(hf), -1044 * 0.74 / 100.0);
    CHECK(fabs(db(g) - (-1044 * 0.26 / 100.0)) < 0.05, "Occlusion LF %.2f dB", db(g));
    CHECK(fabs(db(hf) - (-1044 * 0.74 / 100.0)) < 0.5, "Occlusion HF %.2f dB", db(hf));
    s1 = aufx_voice_send(&v);
    v.i3d_occ = 0;
    s0 = aufx_voice_send(&v);
    CHECK(s0 > 0.0f && fabs(db(s1 / s0) - (-1044 * 0.26 / 100.0)) < 0.05, "Occlusion Raumanteil %.2f dB", db(s1 / s0));
    /* Obstruction: nur Direktpfad */
    i3d_vals(&v, 0, 0, -2000, 0.5f, 0, 0.0f);
    g = aufx_voice_direct(&v, &a);
    s1 = aufx_voice_send(&v);
    CHECK(fabs(db(g) + 10.0) < 0.05 && fabs(db(lp_amp(a, 5000.0)) + 10.0) < 0.5, "Obstruction %.2f dB", db(g));
    CHECK(fabs(db(s1 / s0)) < 0.01, "Obstruction darf den Raum nicht daempfen (%.2f dB)", db(s1 / s0));
    /* lDirect / lDirectHF */
    i3d_vals(&v, -600, -1200, 0, 0.0f, 0, 0.0f);
    g = aufx_voice_direct(&v, &a);
    CHECK(fabs(db(g) + 6.0) < 0.05 && fabs(db(lp_amp(a, 5000.0)) + 12.0) < 0.5, "lDirect/lDirectHF %.2f dB", db(g));
    /* neutral: alles 0, oder Schalter aus */
    i3d_vals(&v, 0, 0, 0, 0.0f, 0, 0.0f);
    g = aufx_voice_direct(&v, &a);
    CHECK(g == 1.0f && a == 1.0f, "neutral %.3f/%.3f", g, a);
    i3d_vals(&v, -600, -600, -2000, 0.5f, -1044, 0.26f);
    g_cfg.audio_occlusion = 0;
    g = aufx_voice_direct(&v, &a);
    s1 = aufx_voice_send(&v);
    CHECK(g == 1.0f && a == 1.0f && fabs(db(s1 / s0)) < 0.01, "audio_occlusion=0 nicht neutral");
    g_cfg.audio_occlusion = 1;
}

static double rms(const float *o, size_t n, int ch, size_t from)
{
    double e = 0.0;
    size_t i;
    for (i = from; i < n; i++)
        e += (double)o[i * 2 + ch] * o[i * 2 + ch];
    return sqrt(e / (double)(n - from));
}

/* HRTF fuer eine Richtung: Sinus f (0 = Rauschen), n Frames in 10-ms-Bloecken. */
static float g_hr_lp; /* > 0: Rauschen tiefpassgefiltert (Ein-Pol-Koeffizient) */
static void hrtf_run(const float dir[3], double f, float *out, size_t n)
{
    static ausp_hrtf_t h;
    static float in[BLK];
    size_t b, i;
    float lp = 0.0f;
    memset(&h, 0, sizeof h);
    memset(out, 0, n * 2 * sizeof(float));
    g_rng = 777u;
    for (b = 0; b + BLK <= n; b += BLK) {
        for (i = 0; i < BLK; i++) {
            in[i] = f > 0.0 ? (float)(0.5 * sin(2.0 * 3.14159265358979 * f * (double)(b + i) / MIXRATE)) : 0.5f * noise();
            if (g_hr_lp > 0.0f && f <= 0.0) {
                lp += g_hr_lp * (in[i] - lp);
                in[i] = lp;
            }
        }
        ausp_hrtf_block(&h, dir, (float)MIXRATE, in, BLK, out + b * 2);
    }
}

/* 11. HRTF (Kugelkopf): Laufzeit, Pegel je Ohr und Frequenz, vorn symmetrisch, hinten dunkler, Energie. */
static void test_hrtf(void)
{
    const size_t n = BLK * 100; /* 1 s */
    float *o = (float *)malloc(n * 2 * sizeof(float));
    static const float right[3] = { 1.0f, 0.0f, 0.0f }, front[3] = { 0.0f, 0.0f, 1.0f },
                       back[3] = { 0.0f, 0.0f, -1.0f }, left[3] = { -1.0f, 0.0f, 0.0f };
    double ild_hf, ild_lf, fr_hf, bk_hf, fr_lf, bk_lf, e0, e90, maxd = 0.0, ms;
    int lag, best = 0;
    double bestc = -1e30;
    size_t i;
    LARGE_INTEGER q0, q1, qf;
    /* Laufzeit (Kreuzkorrelation, Rauschen) */
    hrtf_run(right, 0.0, o, n);
    for (lag = -40; lag <= 40; lag++) {
        double c = 0.0;
        for (i = 4410; i < n - 50; i++)
            c += (double)o[i * 2 + 1] * o[(i + lag) * 2];
        if (c > bestc) {
            bestc = c;
            best = lag;
        }
    }
    printf("HRTF rechts: links verzoegert um %d Samples = %.3f ms (Soll 0,656 ms)\n", best, best * 1000.0 / MIXRATE);
    CHECK(best >= 27 && best <= 31, "ITD %d Samples", best);
    /* Pegel je Ohr: 6 kHz (Kopfschatten), 300 Hz */
    hrtf_run(right, 6000.0, o, n);
    ild_hf = db(rms(o, n, 1, n / 2) / rms(o, n, 0, n / 2));
    hrtf_run(right, 300.0, o, n);
    ild_lf = db(rms(o, n, 1, n / 2) / rms(o, n, 0, n / 2));
    printf("HRTF rechts: Pegelunterschied R-L 6 kHz %.1f dB, 300 Hz %.1f dB\n", ild_hf, ild_lf);
    CHECK(ild_hf > 10.0, "ILD 6 kHz %.1f dB", ild_hf);
    CHECK(ild_lf > 0.0 && ild_lf < 3.0, "ILD 300 Hz %.1f dB", ild_lf);
    hrtf_run(left, 6000.0, o, n);
    CHECK(fabs(db(rms(o, n, 0, n / 2) / rms(o, n, 1, n / 2)) - ild_hf) < 0.01, "links/rechts nicht spiegelgleich");
    /* vorn: beide Ohren gleich */
    hrtf_run(front, 0.0, o, n);
    for (i = 0; i < n; i++)
        if (fabs(o[i * 2] - o[i * 2 + 1]) > maxd)
            maxd = fabs(o[i * 2] - o[i * 2 + 1]);
    CHECK(maxd < 1e-6, "vorn L != R (%.2e)", maxd);
    /* vorn/hinten: hinten dunkler, tiefe Lage gleich */
    hrtf_run(front, 6000.0, o, n);
    fr_hf = rms(o, n, 0, n / 2);
    hrtf_run(back, 6000.0, o, n);
    bk_hf = rms(o, n, 0, n / 2);
    hrtf_run(front, 300.0, o, n);
    fr_lf = rms(o, n, 0, n / 2);
    hrtf_run(back, 300.0, o, n);
    bk_lf = rms(o, n, 0, n / 2);
    printf("HRTF hinten gegen vorn: 6 kHz %.1f dB, 300 Hz %.2f dB\n", db(bk_hf / fr_hf), db(bk_lf / fr_lf));
    CHECK(db(bk_hf / fr_hf) < -3.0, "hinten nicht dunkler (%.1f dB)", db(bk_hf / fr_hf));
    CHECK(fabs(db(bk_lf / fr_lf)) < 0.5, "hinten 300 Hz %.2f dB", db(bk_lf / fr_lf));
    /* Gesamtenergie seitlich gegen vorn: weisses Rauschen (nur Ausgabe, Kopfschatten hebt HF zugewandt
     * physikalisch an) und spieltypisches Signal (Rauschen tiefpassgefiltert 1 kHz, Schwerpunkt der
     * Spielmischung ~300 Hz): Grenze +-3 dB */
    hrtf_run(front, 0.0, o, n);
    e0 = energy(o, n / 10, n);
    hrtf_run(right, 0.0, o, n);
    e90 = energy(o, n / 10, n);
    printf("HRTF Energie seitlich gegen vorn: weisses Rauschen %.2f dB", 10.0 * log10(e90 / e0));
    g_hr_lp = 0.13f; /* Ein-Pol ~1 kHz */
    hrtf_run(front, 0.0, o, n);
    e0 = energy(o, n / 10, n);
    hrtf_run(right, 0.0, o, n);
    e90 = energy(o, n / 10, n);
    g_hr_lp = 0.0f;
    printf(", Rauschen bis 1 kHz %.2f dB\n", 10.0 * log10(e90 / e0));
    CHECK(fabs(10.0 * log10(e90 / e0)) < 3.0, "Energie seitlich %.2f dB", 10.0 * log10(e90 / e0));
    /* Richtungswechsel je Block (Rampe): endlich, kein Knacken ueber 1 */
    {
        static ausp_hrtf_t h;
        static float in[BLK];
        double pk = 0.0;
        size_t b;
        memset(&h, 0, sizeof h);
        memset(o, 0, n * 2 * sizeof(float));
        for (b = 0; b + BLK <= n; b += BLK) {
            float a = (float)b / (float)n * 12.0f, dir[3];
            dir[0] = sinf(a);
            dir[1] = 0.0f;
            dir[2] = cosf(a);
            for (i = 0; i < BLK; i++)
                in[i] = 0.5f * noise();
            ausp_hrtf_block(&h, dir, (float)MIXRATE, in, BLK, o + b * 2);
        }
        for (i = 0; i < n * 2; i++)
            if (!(o[i] == o[i]) || fabs(o[i]) > pk)
                pk = o[i] == o[i] ? fabs(o[i]) : 1e9;
        printf("HRTF Drehung (12 rad/s, Rauschen 0,5): Spitze %.2f, endlich\n", pk);
        CHECK(pk < 1.2, "Drehung: Spitze %.2f", pk); /* Kopfschatten bis x2 bei HF zugewandt */
    }
    /* Kosten: 32 Stimmen je 10-ms-Block */
    QueryPerformanceFrequency(&qf);
    QueryPerformanceCounter(&q0);
    for (i = 0; i < 32; i++)
        hrtf_run(right, 0.0, o, n);
    QueryPerformanceCounter(&q1);
    ms = (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)qf.QuadPart / 100.0; /* 100 Bloecke */
    printf("HRTF Kosten: %.3f ms je 10-ms-Block bei 32 Stimmen (inkl. Testsignal)\n", ms);
    CHECK(ms < 0.5, "HRTF Kosten %.3f ms", ms);
    free(o);
}

/* 12. Downmix-Pegel und Surround-Gewichte. */
static void test_downmix(void)
{
    float m[6], l, r, g[4];
    int k;
    static const float dirs[5][3] = { { 0, 0, 1 }, { 1, 0, 0 }, { 0, 0, -1 }, { -0.9397f, 0, -0.3420f }, { 0, 1, 0 } };
    /* alt: bitgleich zur Formel vor W356 */
    m[0] = 0.3f; m[1] = -0.2f; m[2] = 0.5f; m[3] = 0.25f; m[4] = -0.4f; m[5] = 0.1f;
    ausp_downmix51(m, 0, &l, &r);
    CHECK(l == m[0] + 0.707f * m[2] + 0.5f * m[3] + 0.707f * m[4] && r == m[1] + 0.707f * m[2] + 0.5f * m[3] + 0.707f * m[5],
          "Downmix 0 nicht bitgleich");
    /* normiert: Vollaussteuerung auf allen Kanaelen -> 2,0 (mit -6 dB 2D-Headroom = 0 dBFS) */
    for (k = 0; k < 6; k++)
        m[k] = 1.0f;
    ausp_downmix51(m, 1, &l, &r);
    printf("Downmix normiert: alle Kanaele 1,0 -> L %.3f R %.3f (Soll 2,0; mit 2D-Headroom 1,0)\n", l, r);
    CHECK(fabs(l - 2.0f) < 1e-4 && fabs(r - 2.0f) < 1e-4, "Downmix Vollaussteuerung %.3f", l);
    /* Verhaeltnisse: C und Surround -3 dB, LFE -6 dB gegen FL; C mittig */
    {
        float fl, c, lfe, sl, rr;
        memset(m, 0, sizeof m); m[0] = 1.0f; ausp_downmix51(m, 1, &fl, &rr);
        CHECK(rr == 0.0f, "FL auf rechts");
        memset(m, 0, sizeof m); m[2] = 1.0f; ausp_downmix51(m, 1, &c, &rr);
        CHECK(c == rr, "Mitte nicht mittig");
        memset(m, 0, sizeof m); m[3] = 1.0f; ausp_downmix51(m, 1, &lfe, &rr);
        memset(m, 0, sizeof m); m[4] = 1.0f; ausp_downmix51(m, 1, &sl, &rr);
        CHECK(rr == 0.0f, "SL auf rechts");
        printf("Downmix je Kanal (dB gegen FL): C %.2f, LFE %.2f, SL %.2f; FL selbst %.2f dB\n", db(c / fl), db(lfe / fl),
               db(sl / fl), db(fl));
        CHECK(fabs(db(c / fl) + 3.01) < 0.05 && fabs(db(lfe / fl) + 6.02) < 0.05 && fabs(db(sl / fl) + 3.01) < 0.05,
              "Downmix-Koeffizienten");
    }
    /* Surround: konstante Leistung, Richtung stimmt */
    for (k = 0; k < 5; k++) {
        float p;
        ausp_quad_gains(dirs[k], g);
        p = g[0] * g[0] + g[1] * g[1] + g[2] * g[2] + g[3] * g[3];
        CHECK(fabs(p - 1.0f) < 1e-4, "Surround Leistung %d: %.4f", k, p);
    }
    ausp_quad_gains(dirs[0], g);
    CHECK(fabs(g[0] - 0.7071f) < 1e-3 && fabs(g[1] - 0.7071f) < 1e-3 && g[2] == 0.0f && g[3] == 0.0f, "Surround vorn");
    ausp_quad_gains(dirs[2], g);
    CHECK(g[0] == 0.0f && g[1] == 0.0f && fabs(g[2] - 0.7071f) < 1e-3 && fabs(g[3] - 0.7071f) < 1e-3, "Surround hinten");
    ausp_quad_gains(dirs[1], g);
    printf("Surround rechts (90 Grad): FL %.2f FR %.2f BL %.2f BR %.2f\n", g[0], g[1], g[2], g[3]);
    CHECK(g[1] > 0.3f && g[3] > g[1] && g[0] == 0.0f && g[2] == 0.0f, "Surround rechts");
    ausp_quad_gains(dirs[3], g);
    CHECK(g[2] > 0.99f, "Surround hinten links (BL %.2f)", g[2]);
}

/* Hoerprobe (synthetisch, ohne Spieldaten): Rauschimpulse nacheinander vorn, rechts, hinten, links, je 4 s;
 * A = Stereo-Balance wie audio_3d (vorn = hinten), B = HRTF. 16 Bit Stereo 44,1 kHz. */
static void wav16(const char *path, const float *x, size_t n)
{
    FILE *f;
    unsigned riff = 36u + (unsigned)n * 4u, rate = MIXRATE, br = MIXRATE * 4u, dsz = (unsigned)n * 4u;
    unsigned short pcm = 1, ch = 2, al = 4, bits = 16;
    unsigned fmtsz = 16;
    size_t i;
    if (fopen_s(&f, path, "wb") != 0 || !f)
        return;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmtsz, 4, 1, f);
    fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&br, 4, 1, f);
    fwrite(&al, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&dsz, 4, 1, f);
    for (i = 0; i < n * 2; i++) {
        float s = x[i] > 1.0f ? 1.0f : x[i] < -1.0f ? -1.0f : x[i];
        short v = (short)(s * 32767.0f);
        fwrite(&v, 2, 1, f);
    }
    fclose(f);
}

static void hrtf_demo(const char *pa, const char *pb)
{
    static const float dirs[4][3] = { { 0, 0, 1 }, { 1, 0, 0 }, { 0, 0, -1 }, { -1, 0, 0 } };
    const size_t seg = (size_t)MIXRATE * 4, n = seg * 4;
    float *a = (float *)calloc(n * 2, sizeof(float)), *b = (float *)calloc(n * 2, sizeof(float));
    static float in[BLK];
    static ausp_hrtf_t h;
    float lp = 0.0f, lp2 = 0.0f;
    size_t s, i;
    memset(&h, 0, sizeof h);
    for (s = 0; s + BLK <= n; s += BLK) {
        const float *d = dirs[s / seg];
        float pl = d[0] > 0.0f ? 1.0f - d[0] : 1.0f, pr = d[0] < 0.0f ? 1.0f + d[0] : 1.0f;
        for (i = 0; i < BLK; i++) {
            size_t t = s + i;
            float env = ((t % (MIXRATE / 2)) < (size_t)(MIXRATE / 5)) ? 1.0f : 0.0f; /* 200 ms an / 300 ms aus */
            float x = noise();
            lp += 0.35f * (x - lp); /* etwas dunkler als weiss (wie Spielgeraeusche) */
            lp2 += 0.35f * (lp - lp2);
            in[i] = 0.25f * env * lp2;
            a[t * 2] = in[i] * pl;
            a[t * 2 + 1] = in[i] * pr;
        }
        ausp_hrtf_block(&h, d, (float)MIXRATE, in, BLK, b + s * 2);
    }
    wav16(pa, a, n);
    wav16(pb, b, n);
    printf("HRTF-Hoerprobe: %s / %s (vorn, rechts, hinten, links je 4 s)\n", pa, pb);
    free(a);
    free(b);
}

int main(int argc, char **argv)
{
    if (argc == 4 && strcmp(argv[1], "--hrtf-demo") == 0) {
        hrtf_demo(argv[2], argv[3]);
        return 0;
    }
    test_reverb();
    test_eg();
    test_occlusion();
    test_hrtf();
    test_downmix();
    if (g_fail)
        printf("audio_reverb_selftest: %d FEHLER\n", g_fail);
    else
        printf("audio_reverb_selftest: ALLE OK\n");
    return g_fail ? 1 : 0;
}
