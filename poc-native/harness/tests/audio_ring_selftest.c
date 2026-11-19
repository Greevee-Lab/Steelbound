/* audio_ring_selftest.c - ISSUE-10 (GitHub #10): Selbsttest Unterlauf/Cursor-Reset
 * der Bink-Ringstimmen mit synthetischem Ring, ohne Spiel und ohne Spieldaten.
 *
 * Nachgestellt wird die in I10_st1 gemessene Folge (BERICHT_M2, ISSUE-10):
 * Ring 26624 Frames (Bink, mono 16 Bit), Fuetterung haelt den Ring voll; dann
 * blockiert die Fuetterung 3 s, die FIFO laeuft leer; danach holt der Schreiber
 * 22016 Frames im Schwall nach, setzt den Cursor auf 0 (SetCurrentPosition)
 * und beschreibt den ganzen Ring neu, beginnend an seiner Tonuhr (= erster noch
 * ungespielter Frame). Signal: Gleitton 200 -> 1600 Hz (stetig, nicht periodisch).
 *
 * Verwendet die echten Funktionen au_pcm_convert/au_fifo_append (audio_fmt.c)
 * und au_ring_reset_flush/au_ring_gain (audio_ring.c). Der Verbraucher bildet
 * den FIFO-/Momentaufnahme-Zweig von au_render_voice (audio_mix.c) nach.
 *
 * Prueft je alt (audio_ring_underrun=0) und neu (=1):
 *  1. Ohne Unterlauf: Ausgabe alt == neu bitgleich.
 *  2. Waehrend des Haengers: alt = alte Ring-Momentaufnahme hoerbar, neu = exakt 0.
 *  3. Wiederholung: hoechste normierte Korrelation zwischen Ausgabe nach dem
 *     Haenger und frueherer Ausgabe (Versatz 0,1-1,5 s): alt hoch, neu niedrig.
 *  4. Lage danach: FIFO-Fuellstand (= Verzug Ton gegen Uhr): alt +22016 Frames,
 *     neu Ringlaenge wie vor dem Haenger.
 *  5. Knacken: groesster Sprung zwischen zwei Ausgabe-Frames ab Haengerbeginn,
 *     neu nicht groesser als der Signal-Hoechstwert ohne Haenger (+10 %).
 *  6. Szenario B: Reset bei laufender Stimme, Ring mehrfach neu beschrieben
 *     (au_ring_place): alt FIFO-Wachstum + Wiederholung, neu Ringlaenge.
 *  7. W205-Startgatter nur fuer den ersten Start (au_ring_first_start).
 *  8. Reset bei laufender Wiedergabe: Rest aus-, neuer Inhalt eingeblendet.
 * Exit 0 = alles gruen. */
#include "../src/audio_int.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

hcfg_t g_cfg;      /* audio_fmt.c liest g_cfg.audio_buflen */
au32 g_stat_drops; /* audio_fmt.c */

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

#define RING 26624u     /* Frames (Bink-Ring, 53248 Bytes mono 16 Bit) */
#define TICK MIXCHUNK   /* 882 Frames = 20 ms */
#define CATCHUP 22016u  /* gemessener Schwall nach dem Haenger (I10_st1) */
#define STALL_TICKS 150 /* 3 s */
#define SECS 12
#define TOTAL (MIXRATE * SECS)
#define SRCN (MIXRATE * (SECS + 4))

static int16_t *g_src; /* Filmton (Quelle), Frame k */

static void make_src(void)
{
    au32 k;
    double ph = 0.0;
    g_src = (int16_t *)malloc(SRCN * sizeof(int16_t));
    for (k = 0; k < SRCN; k++) {
        double f = 200.0 + 1400.0 * (double)k / SRCN;
        ph += 2.0 * 3.14159265358979 * f / MIXRATE;
        g_src[k] = (int16_t)(16000.0 * sin(ph));
    }
}

/* Schreiber: n Frames ab Quellposition pos per PCM-Dekoder an die FIFO */
static void feed(audio_voice_t *v, au32 pos, au32 n)
{
    float *tmp = (float *)malloc((size_t)(n + 16) * 2 * sizeof(float));
    au32 got = au_pcm_convert((const uint8_t *)(g_src + pos), n * 2u, 1, 16, MIXRATE, tmp, n + 16);
    au_fifo_append(v, tmp, got);
    v->ring_fed = 1;
    free(tmp);
}

/* Verbraucher: Nachbildung au_render_voice (FIFO, sonst Momentaufnahme in Schleife) */
static void mix(audio_voice_t *v, float *out, au32 n, int fix)
{
    au32 i;
    int ru = fix && v->ring_fed;
    for (i = 0; i < n; i++) {
        float l = 0.0f, r = 0.0f;
        if (v->fifo_n) {
            l = v->fifo[v->fifo_rd * 2];
            r = v->fifo[v->fifo_rd * 2 + 1];
            v->fifo_rd = (v->fifo_rd + 1) % v->fifo_cap;
            v->fifo_n--;
            if (ru && ((v->ring_hold | v->ring_ramp | (au32)v->ring_starved) || v->fifo_n < AU_RING_FADE)) {
                float k = au_ring_gain(v);
                l *= k;
                r *= k;
            }
        } else if (v->data && v->datalen) {
            l = v->data[v->playhead * 2];
            r = v->data[v->playhead * 2 + 1];
            v->playhead = (v->playhead + 1) % v->datalen;
            if (ru) {
                l = r = 0.0f;
                v->ring_starved = 1;
            }
        }
        out[i * 2] = l;
        out[i * 2 + 1] = r;
        v->played++;
    }
}

typedef struct {
    float *out; /* Stereo, TOTAL Frames */
    au32 stall_at, stall_end; /* Ausgabe-Frames */
    au32 fifo_before, fifo_after;
    au32 flushed;
} run_t;

static void run(int fix, int stall, run_t *res)
{
    audio_voice_t v;
    au32 t, wpos = 0, tick, ticks = TOTAL / TICK, stall_tick = 200, last_played = 0;
    memset(&v, 0, sizeof v);
    memset(res, 0, sizeof *res);
    res->out = (float *)calloc((size_t)TOTAL * 2, sizeof(float));
    /* SetBufferData: Momentaufnahme des Rings beim Start (hier mit Filmton gefuellt) */
    v.datalen = RING;
    v.data = (float *)malloc((size_t)RING * 2 * sizeof(float));
    au_pcm_convert((const uint8_t *)g_src, RING * 2u, 1, 16, MIXRATE, v.data, RING);
    feed(&v, 0, RING);
    wpos = RING;
    for (tick = 0; tick < ticks; tick++) {
        int stalled = stall && tick >= stall_tick && tick < stall_tick + STALL_TICKS;
        if (stall && tick == stall_tick) {
            res->stall_at = tick * TICK;
            res->fifo_before = v.fifo_n;
        }
        if (stall && tick == stall_tick + STALL_TICKS) {
            au32 clock;
            res->stall_end = tick * TICK;
            /* Schwall, dann Cursor-Reset + ganzer Ring ab Tonuhr (erster ungespielter Frame) */
            feed(&v, wpos, CATCHUP);
            wpos += CATCHUP;
            clock = wpos - v.fifo_n;
            if (fix && v.played && v.fifo_n)
                res->flushed = au_ring_reset_flush(&v);
            v.played = 0;
            last_played = 0;
            feed(&v, clock, RING);
            wpos = clock + RING;
        } else if (!stalled) {
            au32 consumed = (au32)(v.played - last_played); /* Ring frei = gespielt */
            if (consumed) {
                feed(&v, wpos, consumed);
                wpos += consumed;
            }
            last_played = (au32)v.played;
        }
        if (stall && tick == stall_tick + STALL_TICKS + 100)
            res->fifo_after = v.fifo_n;
        t = tick * TICK;
        mix(&v, res->out + (size_t)t * 2, TICK, fix);
        if (stalled)
            last_played = (au32)v.played; /* Uhr laeuft, Schreiber sieht nichts */
    }
    free(v.data);
    free(v.fifo);
}

/* Szenario B (I10_ns/I10_st3): Nach dem Cursor-Reset beschreibt Bink den Ring
 * mehrmals neu (ganz ab 0, nochmals ganz ab 0, dann ab x bis Ringende), waehrend
 * die Stimme weiterlaeuft. Unlock-Modell wie audio_unlock (Ring-Frontier ao,
 * Ueberlapp ueberschreibt ungespielte FIFO-Frames, Rest wird angehaengt). */
#define BUFLEN (RING * 2u)
static uint64_t g_front;

static void unlock_model(audio_voice_t *v, au32 roff, au32 nbytes, au32 srcpos, int fix)
{
    uint64_t ao = g_front - g_front % BUFLEN + roff, ov;
    au32 skip = 0, j, nf = nbytes / 2;
    float *tmp = (float *)malloc((size_t)(nf + 16) * 2 * sizeof(float));
    while (ao + BUFLEN <= g_front)
        ao += BUFLEN;
    if (fix && v->ring_fed)
        ao = au_ring_place(ao, nbytes, g_front, v->played * 2u, BUFLEN);
    au_pcm_convert((const uint8_t *)(g_src + srcpos), nbytes, 1, 16, MIXRATE, tmp, nf + 16);
    if (ao < g_front) {
        ov = g_front - ao;
        skip = ov > nbytes ? nbytes : (au32)ov;
        for (j = 0; j < skip / 2; j++) { /* au_ring_overwrite */
            au32 dist = (au32)(ov / 2) - j;
            if (dist == 0 || dist > v->fifo_n)
                continue;
            v->fifo[((v->fifo_wr + v->fifo_cap - dist) % v->fifo_cap) * 2] = tmp[j * 2];
            v->fifo[((v->fifo_wr + v->fifo_cap - dist) % v->fifo_cap) * 2 + 1] = tmp[j * 2 + 1];
        }
    }
    if (nbytes > skip)
        au_fifo_append(v, tmp + (skip / 2) * 2, nf - skip / 2);
    if (ao + nbytes > g_front)
        g_front = ao + nbytes;
    v->ring_fed = 1;
    free(tmp);
}

static void run_b(int fix, run_t *res)
{
    audio_voice_t v;
    au32 tick, ticks = TOTAL / TICK, reset_tick = 200, wpos, last_played = 0;
    memset(&v, 0, sizeof v);
    memset(res, 0, sizeof *res);
    res->out = (float *)calloc((size_t)TOTAL * 2, sizeof(float));
    g_front = 0;
    unlock_model(&v, 0, BUFLEN, 0, fix);
    wpos = RING;
    for (tick = 0; tick < ticks; tick++) {
        if (tick == reset_tick) {
            au32 clock = wpos - v.fifo_n; /* Tonuhr = erster ungespielter Frame */
            res->stall_at = tick * TICK;
            res->fifo_before = v.fifo_n;
            if (fix && v.played && v.fifo_n)
                res->flushed = au_ring_reset_flush(&v);
            v.played = 0; /* SetCurrentPosition(0) */
            last_played = 0;
            g_front = 0;
            unlock_model(&v, 0, BUFLEN, clock, fix);
            mix(&v, res->out + (size_t)tick * TICK * 2, TICK, fix); /* Stimme laeuft weiter */
            unlock_model(&v, 0, BUFLEN, clock, fix); /* zweiter Durchgang ganz */
            unlock_model(&v, 7680, BUFLEN - 7680, clock + 3840, fix); /* ab x bis Ringende */
            unlock_model(&v, 30720, BUFLEN - 30720, clock + 15360, fix);
            wpos = clock + RING;
            last_played = (au32)v.played;
            continue;
        }
        if (tick > 0) {
            au32 consumed = (au32)(v.played - last_played);
            if (consumed) {
                unlock_model(&v, (au32)(g_front % BUFLEN), consumed * 2u, wpos, fix);
                wpos += consumed;
            }
            last_played = (au32)v.played;
        }
        if (tick == reset_tick + 100)
            res->fifo_after = v.fifo_n;
        mix(&v, res->out + (size_t)tick * TICK * 2, TICK, fix);
    }
    free(v.fifo);
}

/* hoechste normierte Korrelation der Ausgabe ab a (Laenge len) gegen frueheres
 * Ausgangssignal im Versatz [lo, hi] Frames (Schritt 1) */
static double max_repeat(const float *o, au32 a, au32 len, au32 lo, au32 hi, au32 *best_lag)
{
    double best = 0.0, e0 = 0.0;
    au32 lag, i;
    for (i = 0; i < len; i++)
        e0 += (double)o[(a + i) * 2] * o[(a + i) * 2];
    for (lag = lo; lag <= hi && lag <= a; lag++) {
        double c = 0.0, e1 = 0.0, nc;
        for (i = 0; i < len; i++) {
            double x = o[(a + i) * 2], y = o[(a + i - lag) * 2];
            c += x * y;
            e1 += y * y;
        }
        nc = (e0 > 0 && e1 > 0) ? c / sqrt(e0 * e1) : 0.0;
        if (nc > best) {
            best = nc;
            *best_lag = lag;
        }
    }
    return best;
}

static double rms(const float *o, au32 a, au32 n)
{
    double e = 0.0;
    au32 i;
    for (i = 0; i < n; i++)
        e += (double)o[(a + i) * 2] * o[(a + i) * 2];
    return sqrt(e / n);
}

static double max_step(const float *o, au32 a, au32 b)
{
    double m = 0.0;
    au32 i;
    for (i = a + 1; i < b; i++) {
        double d = fabs((double)o[i * 2] - o[(i - 1) * 2]);
        if (d > m)
            m = d;
    }
    return m;
}

int main(void)
{
    run_t a0, a1, s0, s1;
    double sig_step, rep0, rep1, st0, st1, sil0, sil1;
    au32 lag0 = 0, lag1 = 0, win;
    make_src();

    /* 1. ohne Unterlauf bitgleich */
    run(0, 0, &a0);
    run(1, 0, &a1);
    CHECK(memcmp(a0.out, a1.out, (size_t)TOTAL * 2 * sizeof(float)) == 0, "ohne Unterlauf nicht bitgleich");
    sig_step = max_step(a0.out, 0, TOTAL);
    printf("ohne Unterlauf: alt == neu bitgleich (%u Frames), groesster Signalschritt %.4f\n", TOTAL, sig_step);

    /* mit Haenger */
    run(0, 1, &s0);
    run(1, 1, &s1);
    CHECK(s0.stall_at && s0.stall_end > s0.stall_at, "Haenger nicht ausgefuehrt");

    /* 2. waehrend des Haengers (nach dem Leerlaufen der FIFO) */
    win = s0.stall_end - (s0.stall_at + s0.fifo_before) - TICK;
    sil0 = rms(s0.out, s0.stall_at + s0.fifo_before + TICK / 2, win);
    sil1 = rms(s1.out, s1.stall_at + s1.fifo_before + TICK / 2, win);
    printf("Haenger: FIFO vorher %u Frames, Ausgabe danach %.2f s: RMS alt %.4f (alte Ringdaten), neu %.6f\n",
           s0.fifo_before, (double)win / MIXRATE, sil0, sil1);
    CHECK(sil0 > 0.1, "alt: alte Ringdaten waehrend des Haengers erwartet (RMS %.4f)", sil0);
    CHECK(sil1 == 0.0, "neu: Stille waehrend des Haengers erwartet (RMS %.6f)", sil1);

    /* 3. Wiederholung nach dem Haenger: 0,25-s-Fenster ab 0,6 s nach Wiederanlauf */
    rep0 = max_repeat(s0.out, s0.stall_end + CATCHUP + MIXRATE / 10, MIXRATE / 4, MIXRATE / 10, MIXRATE * 3 / 2, &lag0);
    rep1 = max_repeat(s1.out, s1.stall_end + CATCHUP + MIXRATE / 10, MIXRATE / 4, MIXRATE / 10, MIXRATE * 3 / 2, &lag1);
    printf("Wiederholung (Korrelation mit frueherer Ausgabe, 0,1-1,5 s): alt %.3f bei +%.3f s, neu %.3f bei +%.3f s\n",
           rep0, (double)lag0 / MIXRATE, rep1, (double)lag1 / MIXRATE);
    CHECK(rep0 > 0.9, "alt: Wiederholung erwartet (%.3f)", rep0);
    CHECK(rep1 < 0.5, "neu: keine Wiederholung erwartet (%.3f)", rep1);

    /* 4. Lage danach */
    printf("FIFO 2 s nach Wiederanlauf: alt %u, neu %u Frames (Ring %u); verworfen neu %u Frames\n",
           s0.fifo_after, s1.fifo_after, RING, s1.flushed);
    CHECK(s0.fifo_after >= RING + CATCHUP - TICK, "alt: Verzug +%u erwartet", CATCHUP);
    CHECK(s1.fifo_after <= RING + AU_RING_FADE, "neu: Verzug wie vor dem Haenger erwartet (%u)", s1.fifo_after);

    /* 5. Knacken ab Haengerbeginn */
    st0 = max_step(s0.out, s0.stall_at, TOTAL);
    st1 = max_step(s1.out, s1.stall_at, TOTAL);
    printf("groesster Sprung ab Haengerbeginn: alt %.4f, neu %.4f (Signal %.4f)\n", st0, st1, sig_step);
    CHECK(st1 <= sig_step * 1.1, "neu: Sprung %.4f > Signal %.4f (Knacken)", st1, sig_step);

    /* 6. Szenario B: Reset bei laufender Stimme + mehrfaches Neubeschreiben des Rings */
    {
        run_t b0, b1;
        double r0, r1;
        au32 l0 = 0, l1 = 0, at;
        run_b(0, &b0);
        run_b(1, &b1);
        at = b0.stall_at + MIXRATE;
        r0 = max_repeat(b0.out, at, MIXRATE / 4, MIXRATE / 10, MIXRATE * 3 / 2, &l0);
        r1 = max_repeat(b1.out, at, MIXRATE / 4, MIXRATE / 10, MIXRATE * 3 / 2, &l1);
        printf("Reset + Neubeschreiben: FIFO 2 s danach alt %u, neu %u Frames (Ring %u); Wiederholung alt %.3f "
               "(+%.3f s), neu %.3f\n", b0.fifo_after, b1.fifo_after, RING, r0, (double)l0 / MIXRATE, r1);
        CHECK(b0.fifo_after > RING * 2, "alt: FIFO-Wachstum erwartet (%u)", b0.fifo_after);
        CHECK(b1.fifo_after <= RING + AU_RING_FADE, "neu: FIFO wie Ring erwartet (%u)", b1.fifo_after);
        CHECK(r0 > 0.9 && r1 < 0.5, "Wiederholung alt %.3f / neu %.3f", r0, r1);
        /* Einzelfaelle: Neubeschreiben nach Reset (zurueck), Streaming bis Cursor + Ring (bleibt),
         * Ueberlapp vor der Frontier (bleibt, W83), erste Runde (bleibt) */
        CHECK(au_ring_place(53248, 53248, 53248, 1764, BUFLEN) == 0 &&
                  au_ring_place(60928, 45568, 53248, 1764, BUFLEN) == 7680 &&
                  au_ring_place(800000, 4000, 800000, 800000 + 4000 - BUFLEN, BUFLEN) == 800000 &&
                  au_ring_place(790000, 60000, 800000, 700000, BUFLEN) == 790000 &&
                  au_ring_place(1000, 53248, 0, 0, BUFLEN) == 1000,
              "au_ring_place Einzelfaelle");
        free(b0.out);
        free(b1.out);
    }

    /* 7. W205-Startgatter nur beim ersten Start (Leitspur nach Reset nicht anhalten) */
    {
        audio_voice_t v;
        memset(&v, 0, sizeof v);
        CHECK(au_ring_first_start(&v, 1) && au_ring_first_start(&v, 0), "Startgatter: neue Stimme");
        v.ring_once = 1;
        CHECK(!au_ring_first_start(&v, 1) && au_ring_first_start(&v, 0), "Startgatter: nach Reset (neu frei, alt gehalten)");
        printf("Startgatter: erste Wiedergabe gehalten, nach Cursor-Reset frei (alt: gehalten)\n");
    }

    /* 8. Reset waehrend laufender Wiedergabe (kein Unterlauf): Rest ausblenden, dann einblenden */
    {
        audio_voice_t v;
        au32 j, bad = 0;
        float one[2 * 1000], prev = 1.0f;
        memset(&v, 0, sizeof v);
        for (j = 0; j < 2000; j++)
            one[j] = 0.5f;
        au_fifo_append(&v, one, 1000);
        v.ring_fed = 1;
        j = au_ring_reset_flush(&v);
        CHECK(j == 1000 - AU_RING_FADE && v.fifo_n == AU_RING_FADE, "Reset: verworfen %u, Rest %u", j, v.fifo_n);
        au_fifo_append(&v, one, 1000); /* neuer Ring-Inhalt */
        for (j = 0; j < AU_RING_FADE * 2; j++) {
            float s = v.fifo[v.fifo_rd * 2];
            v.fifo_rd = (v.fifo_rd + 1) % v.fifo_cap;
            v.fifo_n--;
            s *= au_ring_gain(&v);
            if (j < AU_RING_FADE ? s > prev + 1e-6f : s < prev - 1e-6f)
                bad++; /* erst fallend bis 0, dann steigend */
            if (j == AU_RING_FADE - 1 && s != 0.0f)
                bad++;
            prev = s;
        }
        CHECK(!bad && fabsf(prev - 0.5f) < 0.01f, "Reset-Blende: %u Abweichungen, Ende %.4f", bad, prev);
        printf("Reset bei laufender Wiedergabe: %u Frames ausgeblendet, dann %u eingeblendet (Ende %.3f)\n",
               AU_RING_FADE, AU_RING_FADE, prev);
        free(v.fifo);
    }

    free(a0.out);
    free(a1.out);
    free(s0.out);
    free(s1.out);
    free(g_src);
    printf("audio_ring_selftest: %s (%d Fehler)\n", g_fail ? "FEHLER" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
