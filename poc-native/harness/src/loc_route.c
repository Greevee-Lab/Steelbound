/* loc_route.c - W395 LOC-ROUTE: Wegwahl je Mitspieler (reine Logik, siehe loc_route.h). */
#include "loc_route.h"
#include <math.h>
#include <string.h>

void locroute_init(locroute *r)
{
    memset(r, 0, sizeof *r);
    r->active = -1;
    r->cand = -1;
    r->dual = -1;
}

void locroute_set_avail(locroute *r, int slot, int avail, double now)
{
    locroute_path *p;
    if (slot < 0 || slot >= LOCROUTE_MAXP) return;
    p = &r->p[slot];
    avail = avail ? 1 : 0;
    if (p->avail == avail) return;
    if (avail) {
        unsigned short seq = p->seq;
        memset(p, 0, sizeof *p);
        p->seq = seq; /* Probennummern laufen weiter (spaete Antworten alter Proben treffen nichts) */
        p->avail = 1;
        p->avail_at = now;
        p->next_probe = now;
        if (r->start == 0) r->start = now > 0 ? now : 0.001;
    } else {
        p->avail = 0;
        p->n = 0;
    }
}

int locroute_probe_due(locroute *r, int slot, double now, unsigned short *seq)
{
    locroute_path *p;
    double ivl;
    int k;
    if (slot < 0 || slot >= LOCROUTE_MAXP) return 0;
    p = &r->p[slot];
    if (!p->avail || now < p->next_probe) return 0;
    if (locroute_alive(p, now) == 0 && p->n > 0) ivl = LOCROUTE_IVL_DEAD;
    else if (!r->decided || slot == r->active) ivl = slot == r->active ? LOCROUTE_IVL_ACTIVE : LOCROUTE_IVL_FAST;
    else ivl = LOCROUTE_IVL;
    if (ivl != LOCROUTE_IVL_DEAD) p->ivl = ivl;
    /* nicht aufholen (nach einer Pause des Zeitgebers keine Salve) */
    p->next_probe = (now - p->next_probe > ivl) ? now + ivl : p->next_probe + ivl;
    k = p->seq % LOCROUTE_RING;
    p->rs[k] = p->seq;
    p->rt[k] = now > 0 ? now : 0.001;
    p->rg[k] = 0;
    if (seq) *seq = p->seq;
    p->seq++;
    return 1;
}

int locroute_probe_ack(locroute *r, int slot, unsigned short seq, double now)
{
    locroute_path *p;
    double s;
    int k;
    if (slot < 0 || slot >= LOCROUTE_MAXP) return 0;
    p = &r->p[slot];
    k = seq % LOCROUTE_RING;
    if (!p->avail || p->rs[k] != seq || p->rt[k] <= 0 || p->rg[k] || p->rt[k] < p->avail_at) return 0;
    s = now - p->rt[k];
    if (s < 0 || s > 5000.0) return 0;
    p->rg[k] = 1;
    if (p->n == 0) {
        p->srtt = s;
        p->jit = s / 4.0;
    } else {
        p->jit = 0.75 * p->jit + 0.25 * fabs(s - p->srtt);
        p->srtt = 0.875 * p->srtt + 0.125 * s;
    }
    p->n++;
    p->last_ok = now;
    if (p->next_probe > now + LOCROUTE_IVL) p->next_probe = now + LOCROUTE_IVL_FAST; /* Weg wieder da: normaler Takt */
    return 1;
}

double locroute_loss(const locroute_path *p, double now)
{
    int i, ev = 0, lost = 0;
    double to = p->n ? 2.0 * p->srtt + 4.0 * p->jit + 200.0 : 1000.0;
    if (to < 400.0) to = 400.0;
    for (i = 0; i < LOCROUTE_RING; i++) {
        if (p->rt[i] <= 0 || p->rt[i] < p->avail_at) continue;
        if (p->rg[i]) {
            ev++;
        } else if (now - p->rt[i] > to) {
            ev++;
            lost++;
        }
    }
    return ev >= 4 ? (double)lost / (double)ev : 0.0;
}

int locroute_alive(const locroute_path *p, double now)
{
    double base, ago, ivl;
    if (!p->avail) return 0;
    if (p->n == 0) return (now - p->avail_at < 1500.0) ? 1 : 0;
    base = p->srtt + 4.0 * p->jit;
    ivl = p->ivl > 0 ? p->ivl : LOCROUTE_IVL;
    ago = now - p->last_ok;
    if (ago > 3.0 * ivl + base + 100.0) return 0;
    if (ago > ivl + base + 30.0) return 1;
    return 2;
}

double locroute_score(const locroute_path *p, double now)
{
    if (!p->avail || p->n < LOCROUTE_MIN_SAMPLES || locroute_alive(p, now) == 0) return -1.0;
    return p->srtt + LOCROUTE_JIT_W * p->jit + LOCROUTE_LOSS_W * locroute_loss(p, now);
}

static int best_excl(const locroute *r, double now, int fallback, int excl)
{
    int i, b = -1;
    double bs = 0, d;
    for (i = 0; i < LOCROUTE_MAXP; i++) {
        double s;
        if (i == excl) continue;
        s = locroute_score(&r->p[i], now);
        if (s < 0) continue;
        if (b < 0 || s < bs) {
            b = i;
            bs = s;
        }
    }
    if (b > 0 && excl != 0) { /* direkt bei Gleichstand bevorzugen */
        d = locroute_score(&r->p[0], now);
        if (d >= 0 && d - bs < fmax(LOCROUTE_TIE_MS, LOCROUTE_TIE_REL * d)) b = 0;
    }
    if (b >= 0 || !fallback) return b;
    for (i = 0; i < LOCROUTE_MAXP; i++) {
        if (i != excl && locroute_alive(&r->p[i], now) >= 1) return i;
    }
    return -1;
}

int locroute_best(const locroute *r, double now, int fallback)
{
    return best_excl(r, now, fallback, -1);
}

static void sw(locroute *r, int to, double now, int why, int *pwhy)
{
    if (to == r->active) return;
    r->dual = r->active;
    r->dual_until = now + LOCROUTE_DUAL_MS;
    if (why == 2 || why == 3) r->nswitch++; /* erste Wahl und vorlaeufige Wege zaehlen nicht */
    if (why == 3) {
        r->kick = 1;
        r->nfail++;
        r->switch_ms = now - r->p[r->active].last_ok;
    }
    r->active = to;
    r->last_switch = now;
    r->cand = -1;
    if (pwhy) *pwhy = why;
}

int locroute_step(locroute *r, double now, int ready, int *why)
{
    int act = r->active, alive_a, i, b;
    if (why) *why = 0;
    if (r->start == 0) return act;
    alive_a = act >= 0 ? locroute_alive(&r->p[act], now) : 0;
    if (!r->decided) {
        int all = 1;
        if (act < 0 || alive_a == 0) {
            b = best_excl(r, now, 1, act);
            if (b >= 0) sw(r, b, now, 4, why);
        }
        for (i = 0; i < LOCROUTE_MAXP; i++) {
            const locroute_path *p = &r->p[i];
            if (p->avail && p->n < LOCROUTE_MIN_SAMPLES && locroute_alive(p, now) != 0) all = 0;
        }
        if ((now - r->start >= LOCROUTE_MEASURE_MS && ready && all) || now - r->start >= LOCROUTE_MEASURE_MAX) {
            r->decided = 1;
            b = locroute_best(r, now, 1);
            if (b >= 0 && b != r->active) sw(r, b, now, 1, why);
            r->last_switch = now;
        }
        return r->active;
    }
    if (act < 0) {
        b = locroute_best(r, now, 1);
        if (b >= 0) sw(r, b, now, 4, why);
        return r->active;
    }
    if (alive_a == 0) { /* Ausfall: sofort auf den naechsten lebenden Weg */
        b = best_excl(r, now, 1, act);
        if (b >= 0) sw(r, b, now, 3, why);
        return r->active;
    }
    if (alive_a == 1) { /* verdaechtig: doppelt senden, bis klar ist, ob der Weg ausfaellt */
        b = best_excl(r, now, 1, act);
        if (b >= 0 && (r->dual != b || r->dual_until < now + 300.0)) {
            if (locroute_dual(r, now) != b) { /* Verdacht beginnt: Offenes sofort auch ueber den zweiten Weg */
                r->kick = 1;
                r->nsuspect++;
            }
            r->dual = b;
            r->dual_until = now + 300.0;
        }
    }
    b = locroute_best(r, now, 0);
    if (b >= 0 && b != act) {
        double sa = locroute_score(&r->p[act], now), sb = locroute_score(&r->p[b], now);
        if (sa >= 0 && sa - sb > LOCROUTE_HYST_MS) {
            if (r->cand != b) {
                r->cand = b;
                r->cand_since = now;
            } else if (now - r->cand_since >= LOCROUTE_SUSTAIN_MS && now - r->last_switch >= LOCROUTE_MIN_GAP_MS) {
                sw(r, b, now, 2, why);
            }
            return r->active;
        }
    }
    r->cand = -1;
    return r->active;
}

int locroute_dual(const locroute *r, double now)
{
    if (r->dual < 0 || r->dual == r->active || now >= r->dual_until || !r->p[r->dual].avail) return -1;
    return r->dual;
}
