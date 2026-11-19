/* audio_spatial.c - W356 AUDIO-2: HRTF (Kugelkopf-Modell), Surround-Gewichte, 5.1-Faltung.
 * Beschreibung und Quellen: audio_spatial.h. Reine DSP, ohne Sperren und ohne Spielbezug. */
#include "audio_spatial.h"
#include <math.h>
#include <string.h>

#define SP_PI 3.14159265358979f
#define HEAD_A 0.0875f /* Kopfradius (m), Brown & Duda */
#define SOUND_C 343.0f /* Schallgeschwindigkeit (m/s) */
#define REAR_DB (-6.0f) /* Hoehendaempfung ganz hinten bei REAR_F */
#define REAR_F 4000.0f

/* Ein-Pol-Tiefpass y += a (x - y) mit |H(f)| = g (wie audio_fx.c lp_coeff). */
static float sp_lp_coeff(float g, float f, float fs)
{
    double w, c, gg, b;
    if (g >= 0.9999f)
        return 1.0f;
    if (g < 0.001f)
        g = 0.001f;
    w = 2.0 * 3.14159265358979 * (f > 0.45f * fs ? 0.45f * fs : f) / fs;
    c = cos(w);
    gg = (double)g * g;
    b = (1.0 - gg * c - sqrt(2.0 * gg * (1.0 - c) - gg * gg * (1.0 - c * c))) / (1.0 - gg);
    if (b < 0.0)
        b = 0.0;
    if (b > 0.9999)
        b = 0.9999;
    return (float)(1.0 - b);
}

void ausp_hrtf_params(const float dir[3], float fs, float t[2], float b0[2], float b1[2], float a1[2], float *ra)
{
    const float beta = 2.0f * SOUND_C / HEAD_A, k = 2.0f * fs, ac = HEAD_A / SOUND_C;
    int e;
    for (e = 0; e < 2; e++) {
        /* Ohrachse links (-x) bzw. rechts (+x); theta = Winkel zwischen Quelle und Ohrachse */
        float ct = e ? dir[0] : -dir[0], th, al, tau;
        if (ct > 1.0f)
            ct = 1.0f;
        if (ct < -1.0f)
            ct = -1.0f;
        th = acosf(ct);
        al = 1.05f + 0.95f * cosf(th * 1.2f); /* theta/150 Grad x 180 Grad */
        /* Woodworth: zugewandt (theta < 90 Grad) (a/c)(1 - cos), abgewandt Umweg um den Kopf */
        tau = th < 0.5f * SP_PI ? ac * (1.0f - ct) : ac * (1.0f + th - 0.5f * SP_PI);
        t[e] = tau * fs;
        b0[e] = (beta + al * k) / (beta + k);
        b1[e] = (beta - al * k) / (beta + k);
        a1[e] = (beta - k) / (beta + k);
    }
    {
        float rear = dir[2] < 0.0f ? -dir[2] : 0.0f;
        *ra = rear > 0.0f ? sp_lp_coeff(powf(10.0f, REAR_DB * rear / 20.0f), REAR_F, fs) : 1.0f;
    }
}

void ausp_hrtf_block(ausp_hrtf_t *h, const float dir[3], float fs, const float *in, uint32_t n, float *out)
{
    float t[2], b0[2], b1[2], a1[2], ra;
    float dt[2], db0[2], db1[2], da1[2], dra, inv;
    uint32_t i;
    int e;
    if (!n)
        return;
    ausp_hrtf_params(dir, fs, t, b0, b1, a1, &ra);
    if (!h->init) {
        memset(h->dl, 0, sizeof h->dl);
        h->w = 0;
        for (e = 0; e < 2; e++) {
            h->x1[e] = h->y1[e] = h->rs[e] = 0.0f;
            h->t[e] = t[e];
            h->b0[e] = b0[e];
            h->b1[e] = b1[e];
            h->a1[e] = a1[e];
        }
        h->ra = ra;
        h->init = 1;
    }
    inv = 1.0f / (float)n;
    for (e = 0; e < 2; e++) {
        dt[e] = (t[e] - h->t[e]) * inv;
        db0[e] = (b0[e] - h->b0[e]) * inv;
        db1[e] = (b1[e] - h->b1[e]) * inv;
        da1[e] = (a1[e] - h->a1[e]) * inv;
    }
    dra = (ra - h->ra) * inv;
    for (i = 0; i < n; i++) {
        float x = in[i];
        h->dl[h->w] = x;
        for (e = 0; e < 2; e++) {
            float d = h->t[e] + dt[e] * (float)(i + 1), fr, s, y;
            int i0;
            if (d < 0.0f)
                d = 0.0f;
            if (d > (float)(AUSP_HR_DL - 2))
                d = (float)(AUSP_HR_DL - 2);
            i0 = (int)d;
            fr = d - (float)i0;
            s = h->dl[(h->w - (uint32_t)i0) & (AUSP_HR_DL - 1)] * (1.0f - fr) +
                h->dl[(h->w - (uint32_t)i0 - 1u) & (AUSP_HR_DL - 1)] * fr;
            y = (h->b0[e] + db0[e] * (float)(i + 1)) * s + (h->b1[e] + db1[e] * (float)(i + 1)) * h->x1[e] -
                (h->a1[e] + da1[e] * (float)(i + 1)) * h->y1[e];
            if (!(y == y) || y > 1e4f || y < -1e4f) /* Schutz */
                y = 0.0f;
            h->x1[e] = s;
            h->y1[e] = y;
            h->rs[e] += (h->ra + dra * (float)(i + 1)) * (y - h->rs[e]);
            out[i * 2 + e] += h->rs[e];
        }
        h->w = (h->w + 1u) & (AUSP_HR_DL - 1);
    }
    for (e = 0; e < 2; e++) {
        h->t[e] = t[e];
        h->b0[e] = b0[e];
        h->b1[e] = b1[e];
        h->a1[e] = a1[e];
    }
    h->ra = ra;
}

/* Lautsprecher im Uhrzeigersinn ab vorn (Grad): FR 30, BR 110, BL 250, FL 330. */
void ausp_quad_gains(const float dir[3], float g[4])
{
    static const float pos[4] = { 30.0f, 110.0f, 250.0f, 330.0f };
    static const int spk[4] = { 1, 3, 2, 0 }; /* Index in g: FL 0, FR 1, BL 2, BR 3 */
    float az, p;
    int k;
    g[0] = g[1] = g[2] = g[3] = 0.0f;
    if (dir[0] * dir[0] + dir[2] * dir[2] < 1e-8f) { /* genau oben/unten: alle gleich */
        g[0] = g[1] = g[2] = g[3] = 0.5f;
        return;
    }
    az = atan2f(dir[0], dir[2]) * 180.0f / SP_PI; /* rechts positiv */
    if (az < 0.0f)
        az += 360.0f;
    for (k = 0; k < 4; k++) {
        float a = pos[k], b = pos[(k + 1) & 3], span, x = az;
        if (b < a)
            b += 360.0f;
        if (x < a)
            x += 360.0f;
        span = b - a;
        if (x >= a && x <= b) {
            p = (x - a) / span;
            g[spk[k]] = cosf(p * 0.5f * SP_PI);
            g[spk[(k + 1) & 3]] = sinf(p * 0.5f * SP_PI);
            return;
        }
    }
    g[0] = g[1] = 0.7071f; /* nicht erreichbar */
}

float ausp_downmix_norm(int norm)
{
    return norm ? 2.0f / (1.0f + 0.7071f + 0.5f + 0.7071f) : 1.0f;
}

void ausp_downmix51(const float *m, int norm, float *l, float *r)
{
    float k = ausp_downmix_norm(norm), c = norm ? 0.7071f : 0.707f; /* 0: bitgleich zur alten Faltung */
    *l = (m[0] + c * m[2] + 0.5f * m[3] + c * m[4]) * k;
    *r = (m[1] + c * m[2] + 0.5f * m[3] + c * m[5]) * k;
}
