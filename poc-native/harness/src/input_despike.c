/* input_despike.c - W439 LOC-RETICLE-JUMP (#78): Ausreisser-Filter fuer analoge Achsen (siehe input_despike.h). */
#include "input_despike.h"

#include <string.h>

static int ds_abs(int v) { return v < 0 ? -v : v; }

void despike_init(despike_t *d, int n, int thr, unsigned hold_ms)
{
    memset(d, 0, sizeof *d);
    d->n = n < 0 ? 0 : (n > DESPIKE_MAX_AX ? DESPIKE_MAX_AX : n);
    d->thr = thr > 0 ? thr : 1;
    d->hold_ms = hold_ms;
}

void despike_reset(despike_t *d, const int *v)
{
    int k;
    for (k = 0; k < d->n; k++) {
        d->out[k] = v[k];
        d->has_cand[k] = 0;
    }
    d->init = 1;
}

void despike_feed(despike_t *d, const int *v, uint64_t now_ms, int *out)
{
    int k;
    if (!d->init) {
        despike_reset(d, v);
    }
    for (k = 0; k < d->n; k++) {
        int x = v[k], o = d->out[k];
        if (!d->has_cand[k]) {
            if (ds_abs(x - o) <= d->thr) {
                d->out[k] = x;
            } else {
                d->cand[k] = x;
                d->cand_t[k] = now_ms;
                d->has_cand[k] = 1;
                d->held++;
            }
        } else {
            int c = d->cand[k];
            int same_dir = (c > o && x > o) || (c < o && x < o);
            if (ds_abs(x - o) <= d->thr) { /* zurueck: Kandidat war ein Ausreisser */
                d->out[k] = x;
                d->has_cand[k] = 0;
                d->spikes++;
            } else if (ds_abs(x - c) <= d->thr || (same_dir && ds_abs(x - o) >= ds_abs(c - o) - d->thr)) {
                d->out[k] = x; /* bestaetigt bzw. weiter in dieselbe Richtung: echte schnelle Bewegung */
                d->has_cand[k] = 0;
                d->accepted++;
            } else if (now_ms - d->cand_t[k] >= d->hold_ms) {
                d->out[k] = x; /* Haltezeit um: juengsten Wert nehmen */
                d->has_cand[k] = 0;
                d->accepted++;
            } else {
                d->cand[k] = x; /* weiterer weiter Wert: neuer Kandidat, Haltezeit laeuft ab dem ersten */
            }
        }
        out[k] = d->out[k];
    }
}

void despike_read(despike_t *d, uint64_t now_ms, int *out)
{
    int k;
    for (k = 0; k < d->n; k++) {
        if (d->has_cand[k] && now_ms - d->cand_t[k] >= d->hold_ms) {
            d->out[k] = d->cand[k];
            d->has_cand[k] = 0;
            d->accepted++;
        }
        out[k] = d->out[k];
    }
}
