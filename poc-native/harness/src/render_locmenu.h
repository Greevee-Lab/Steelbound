/* render_locmenu.h - W377 LOC-MENU-FPS: reine Entscheidungslogik (Selbsttest render_selftest "LOCMENU").
 * Wiederholbild-Takt der LoC-Menues (render_locmenu.c) und Ring der Textebenen-Inhalte (sb_tex.c). */
#ifndef RENDER_LOCMENU_H
#define RENDER_LOCMENU_H

/* Bildrate der Menues: interp_fps (Bildrate des Spielers), gedeckelt durch fps_limit ("Menu frame limit", 0 = aus). */
static __inline int lm_rate(int interp_fps, int fps_limit)
{
    int r = interp_fps > 0 ? interp_fps : 60;
    if (fps_limit > 0 && fps_limit < r) { r = fps_limit; }
    return r < 1 ? 1 : r;
}

enum { LM_WAIT = 0, LM_REPEAT = 1, LM_SKIP_PHASE = 2, LM_SKIP_GPU = 3 };

/* Takt der Wiederholbilder, fest an die Phase der Spiel-Presents gekoppelt (alle Zeiten in derselben Einheit, z. B.
 * QPC). Je Spielbild n Presents (Spiel + n-1 Wiederholungen) im Abstand slot = per/n; n per Bruchteil-Akkumulator,
 * sodass im Mittel genau per/dt Presents je Spielbild entstehen (30-Hz-Menue: 120 -> 4, 144 -> 4,8, Limit 60 -> 2,
 * <= 30 -> 1 = keine Wiederholung). Spiel-Presents werden nie verzoegert; ist eins ueberfaellig (langsames Bild),
 * laufen die Wiederholungen im selben Abstand weiter. */
typedef struct {
    long long game;   /* letztes Spiel-Present (0 = noch keins) */
    long long per;    /* gleitende Periode der Spiel-Presents (0 = unbekannt) */
    long long slot;   /* Abstand der Presents im laufenden Spielbild */
    int n, k;         /* Presents im laufenden Spielbild / naechste Wiederholung (Index ab 1) */
    double credit;    /* Bruchteil-Akkumulator */
} lm_clock_t;

/* Gleitende Periode: nur Abstaende in (lo, hi) zaehlen (Menue-/Ladetakt), Gewicht 1/8. */
static __inline long long lm_period(long long per, long long d, long long lo, long long hi)
{
    if (d <= lo || d >= hi) { return per; }
    return per ? per + (d - per) / 8 : d;
}

/* Spiel-Present zur Zeit t; dt = 1/Bildrate; lo/hi = gueltige Periodenlaengen. */
static __inline void lm_clock_game(lm_clock_t *c, long long t, long long dt, long long lo, long long hi)
{
    if (c->game) { c->per = lm_period(c->per, t - c->game, lo, hi); }
    c->game = t;
    c->k = 1;
    if (c->per > 0 && dt > 0) {
        c->credit += (double)c->per / (double)dt;
        c->n = (int)c->credit;
        if (c->n < 1) { c->n = 1; }
        c->credit -= (double)c->n;
        if (c->credit < 0.0 || c->credit >= 1.0) { c->credit = 0.0; }
        c->slot = c->per / c->n;
    } else {
        c->n = 1;
        c->slot = 0;
    }
}

/* Entscheidung des Takt-Threads. gpu_idle = vorletztes Present fertig. LM_WAIT: nicht faellig; LM_SKIP_PHASE: alle
 * Wiederholungen dieses Spielbilds gezeigt, das naechste Spiel-Present steht an; LM_SKIP_GPU: GPU hinterher;
 * LM_REPEAT: Wiederholbild jetzt, danach lm_clock_done(). *deadline = naechster Weckzeitpunkt. */
static __inline int lm_clock_decide(const lm_clock_t *c, long long now, long long dt, int gpu_idle, long long *deadline)
{
    long long due;
    if (!c->game || c->slot <= 0) { /* Periode noch unbekannt (erste Spielbilder im Menue) */
        *deadline = now + dt;
        return LM_WAIT;
    }
    due = c->game + (long long)c->k * c->slot;
    if (now < due) {
        *deadline = due;
        return LM_WAIT;
    }
    if (c->k >= c->n && now < c->game + c->per + c->slot / 2) { /* Platz fuer das naechste Spiel-Present */
        *deadline = c->game + c->per + c->slot / 2;
        return LM_SKIP_PHASE;
    }
    if (!gpu_idle) {
        *deadline = now + c->slot / 3;
        return LM_SKIP_GPU;
    }
    *deadline = due + c->slot;
    return LM_REPEAT;
}

/* nach einem Wiederholbild zur Zeit now: naechster Platz (verpasste Plaetze nicht nachholen) */
static __inline void lm_clock_done(lm_clock_t *c, long long now)
{
    if (c->slot > 0) { c->k = (int)((now - c->game) / c->slot) + 1; }
}

/* 64-Bit-Hash ueber den ganzen Inhalt, SSE2 (der Harness ist 32-Bit: 64-Bit-Multiplikationen waeren dort
 * emuliert). Akkumulation wie XXH3 (je 16 Byte: (d ^ k) lo32 * hi32 + vertauschte Daten), vier Spuren, am Ende
 * gefaltet. n16 = Anzahl 16-Byte-Bloecke (640x480x4 = 76800). 0 nie als Ergebnis. */
#include <emmintrin.h>
static __inline unsigned long long lm_hash64(const void *src, unsigned n16)
{
    const __m128i *p = (const __m128i *)src;
    __m128i acc[4], key[4];
    unsigned i;
    unsigned long long out[2], h;
    acc[0] = _mm_set_epi32(0x85EBCA77, 0xC2B2AE3D, 0x27D4EB4F, 0x165667B1);
    acc[1] = _mm_set_epi32(0x9E3779B1, 0x7F4A7C15, 0xBF58476D, 0x1CE4E5B9);
    acc[2] = _mm_set_epi32(0x94D049BB, 0x133111EB, 0x2545F491, 0x4F6CDD1D);
    acc[3] = _mm_set_epi32(0xD6E8FEB8, 0x6659FD93, 0xA0761D64, 0x78BD642F);
    key[0] = _mm_set_epi32(0xBE4BA423, 0x396CFEB8, 0x1CAD21F7, 0x2C81017C);
    key[1] = _mm_set_epi32(0xDB979083, 0xE96DCAA6, 0xF7C2B2F8, 0x03F3C1A9);
    key[2] = _mm_set_epi32(0x78E5C0CC, 0x4EE6E6A5, 0x9D4BAE8C, 0x30C1A6E4);
    key[3] = _mm_set_epi32(0x3E5D3F6C, 0x0E1C0A47, 0x7D9EE2D1, 0xC5F1A2B3);
    __m128i pos = _mm_set_epi32(0x61C88647, 0x3C6EF372, 0x7F4A7C15, 0x2545F491), step = _mm_set1_epi32((int)0x9E3779B9);
    for (i = 0; i + 4 <= n16; i += 4) {
        unsigned j;
        /* Schluessel je Position (sonst waere die Summe vertauschungsinvariant: derselbe Text in einer anderen Zeile
         * ergaebe denselben Hash) */
        pos = _mm_add_epi32(pos, step);
        for (j = 0; j < 4; j++) {
            __m128i d = _mm_loadu_si128(p + i + j);
            __m128i dk = _mm_xor_si128(d, _mm_xor_si128(key[j], pos));
            __m128i prod = _mm_mul_epu32(dk, _mm_shuffle_epi32(dk, _MM_SHUFFLE(0, 3, 0, 1)));
            acc[j] = _mm_add_epi64(acc[j], _mm_add_epi64(prod, _mm_shuffle_epi32(d, _MM_SHUFFLE(1, 0, 3, 2))));
        }
        if ((i & 255u) == 252u) { /* alle 64 Runden durchmischen (Reihenfolge der Abschnitte zaehlt) */
            for (j = 0; j < 4; j++) {
                __m128i a = _mm_xor_si128(acc[j], _mm_srli_epi64(acc[j], 47));
                acc[j] = _mm_add_epi64(_mm_mul_epu32(a, _mm_set1_epi32((int)0x9E3779B1)),
                                       _mm_slli_epi64(_mm_mul_epu32(_mm_srli_epi64(a, 32), _mm_set1_epi32((int)0x9E3779B1)), 32));
            }
        }
    }
    for (; i < n16; i++) {
        __m128i d = _mm_loadu_si128(p + i);
        __m128i dk = _mm_xor_si128(d, _mm_xor_si128(key[0], _mm_set1_epi32((int)i)));
        __m128i prod = _mm_mul_epu32(dk, _mm_shuffle_epi32(dk, _MM_SHUFFLE(0, 3, 0, 1)));
        acc[0] = _mm_add_epi64(acc[0], _mm_add_epi64(prod, _mm_shuffle_epi32(d, _MM_SHUFFLE(1, 0, 3, 2))));
    }
    /* falten: Spuren mischen (Multiplikation 32x32 je Haelfte), dann 2 x 64 Bit */
    for (i = 0; i < 4; i++) {
        __m128i a = _mm_xor_si128(acc[i], _mm_srli_epi64(acc[i], 47));
        a = _mm_xor_si128(a, key[i]);
        acc[i] = _mm_add_epi64(_mm_mul_epu32(a, _mm_set1_epi32((int)0x9E3779B1)),
                               _mm_slli_epi64(_mm_mul_epu32(_mm_srli_epi64(a, 32), _mm_set1_epi32((int)0x9E3779B1)), 32));
    }
    _mm_storeu_si128((__m128i *)out, _mm_xor_si128(_mm_add_epi64(acc[0], acc[2]), _mm_add_epi64(acc[1], acc[3])));
    h = out[0] ^ (out[1] * 0x94D049BB133111EBull) ^ (unsigned long long)n16;
    h ^= h >> 29;
    return h ? h : 1ull;
}

/* Ring der zuletzt hochgeladenen Textebenen-Inhalte (Schluessel: Kopf xptr, Groesse, Hash, Device). Der Ring haelt
 * je Eintrag eine eigene Referenz auf die SRV; Verdraengen gibt sie ab (Cache-Eintraege halten ihre eigene). */
#ifdef __d3d11_h__
typedef struct {
    unsigned xptr, w, h;
    unsigned long long h64;
    unsigned h32;                  /* Refresh-Hash des Cache-Eintrags (tex_data_hash) zu diesem Inhalt */
    ID3D11ShaderResourceView *srv;
    const void *dev;
    unsigned use;
} lm_txr_t;

static __inline int lm_txr_find(lm_txr_t *r, int n, unsigned *tick, unsigned xptr, unsigned w, unsigned h,
                                unsigned long long h64, const void *dev)
{
    int k;
    for (k = 0; k < n; k++) {
        if (r[k].srv && r[k].xptr == xptr && r[k].h64 == h64 && r[k].w == w && r[k].h == h && r[k].dev == dev) {
            r[k].use = ++*tick;
            return k;
        }
    }
    return -1;
}

static __inline void lm_txr_put(lm_txr_t *r, int n, unsigned *tick, unsigned xptr, unsigned w, unsigned h,
                                unsigned long long h64, unsigned h32, const void *dev, ID3D11ShaderResourceView *srv)
{
    int k, best = 0;
    if (!srv || !h64) { return; }
    for (k = 0; k < n; k++) {
        if (r[k].srv && r[k].xptr == xptr && r[k].h64 == h64 && r[k].w == w && r[k].h == h && r[k].dev == dev) {
            return;
        }
        if (!r[k].srv) { best = k; break; }
        if (r[k].use < r[best].use) { best = k; }
    }
    if (r[best].srv) { ID3D11ShaderResourceView_Release(r[best].srv); }
    ID3D11ShaderResourceView_AddRef(srv);
    r[best].srv = srv;
    r[best].xptr = xptr; r[best].w = w; r[best].h = h;
    r[best].h64 = h64; r[best].h32 = h32; r[best].dev = dev;
    r[best].use = ++*tick;
}
#endif

#endif
