/* loc_state.c - W382 LOC-ISSUES-B: Spieltakt-Messung und LoC-Zustandszeile (rein, siehe loc_state.h). */
#include "loc_state.h"
#include <stdio.h>
#include <string.h>

void loc_tickrate_push(loc_tickrate_t *r, long long t, unsigned tick, long long freq)
{
    int last;
    if (!r || freq <= 0) return;
    r->freq = freq;
    if (r->n > 0) {
        last = (r->head + LOC_TR_N - 1) % LOC_TR_N;
        if (tick < r->tick[last]) { /* neue Mission im selben Prozess: Messung neu */
            r->n = 0;
            r->head = 0;
            r->resets++;
        } else if (t - r->t[last] < freq) {
            return; /* hoechstens ein Stuetzpunkt je Sekunde */
        }
    }
    r->t[r->head] = t;
    r->tick[r->head] = tick;
    r->head = (r->head + 1) % LOC_TR_N;
    if (r->n < LOC_TR_N) r->n++;
}

double loc_tickrate_hz(const loc_tickrate_t *r, double win_s, double *span_s)
{
    int last, i, k, first;
    double span;
    if (span_s) *span_s = 0.0;
    if (!r || r->n < 2 || r->freq <= 0) return 0.0;
    last = (r->head + LOC_TR_N - 1) % LOC_TR_N;
    first = last;
    for (k = 1; k < r->n; k++) { /* aeltester Stuetzpunkt innerhalb des Fensters */
        i = (last + LOC_TR_N - k) % LOC_TR_N;
        if ((double)(r->t[last] - r->t[i]) / (double)r->freq > win_s + 0.5) break;
        first = i;
    }
    span = (double)(r->t[last] - r->t[first]) / (double)r->freq;
    if (span < 5.0) return 0.0;
    if (span_s) *span_s = span;
    return (double)(r->tick[last] - r->tick[first]) / span;
}

int loc_state_line(char *out, size_t cap, const loc_state_t *s)
{
    char hz[64];
    int n;
    if (!out || cap == 0) return 0;
    if (s->hz > 0.0)
        _snprintf_s(hz, sizeof hz, _TRUNCATE, "%.2f (%.0f s, original %d)", s->hz, s->span_s,
                    s->vblank_hz > 0 ? s->vblank_hz / 3 : 20);
    else
        strcpy_s(hz, sizeof hz, "n/a");
    n = _snprintf_s(out, cap, _TRUNCATE, "game=loc phase=%s frame=%ld tick=%u sim_hz=%s scene=%u.%u interp_fps=%d render_thread=%d",
                    s->lockstep ? "lockstep" : "menu", s->frame, s->tick, hz, s->scene,
                    s->step, s->interp_fps, s->render_thread);
    return n < 0 ? (int)strlen(out) : n;
}
