/* loc_lsarb.c - LOC-LOCKSTEP-ARBITER: Pufferregler des Original-Lockstep (reines Modul, siehe loc_lsarb.h). */
#include "loc_lsarb.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

void lsarb_cfg_default(lsarb_cfg *c)
{
    c->lmin = 2;
    c->lmax = 60;
    c->add_win = 40;
    c->add_stalls = 3;
    c->add_cd = 10;
    c->add_max = 4;
    c->drop_win = 100;
    c->drop_rmin = 2;
    c->drop_cd = 20;
    c->fast_win = 20;
    c->fast_rmin = 5;
    c->fast_cd = 10;
    c->hold0 = 100;
    c->hold_max = 1200;
    c->need_add_ms = 50;
}

static int cl(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

int lsarb_cfg_parse(lsarb_cfg *c, const char *s)
{
    static const struct { const char *k; size_t off; int lo, hi; } K[] = {
        {"lmin", offsetof(lsarb_cfg, lmin), 1, 20},           {"lmax", offsetof(lsarb_cfg, lmax), 2, 100},
        {"add_win", offsetof(lsarb_cfg, add_win), 4, 200},    {"add_stalls", offsetof(lsarb_cfg, add_stalls), 1, 100},
        {"add_cd", offsetof(lsarb_cfg, add_cd), 0, 200},      {"add_max", offsetof(lsarb_cfg, add_max), 0, 60},      {"drop_win", offsetof(lsarb_cfg, drop_win), 4, 250},
        {"drop_rmin", offsetof(lsarb_cfg, drop_rmin), 1, 50}, {"drop_cd", offsetof(lsarb_cfg, drop_cd), 1, 1000},
        {"fast_win", offsetof(lsarb_cfg, fast_win), 4, 250},  {"fast_rmin", offsetof(lsarb_cfg, fast_rmin), 2, 100},
        {"fast_cd", offsetof(lsarb_cfg, fast_cd), 1, 1000},   {"hold0", offsetof(lsarb_cfg, hold0), 0, 10000},
        {"hold_max", offsetof(lsarb_cfg, hold_max), 0, 100000}, {"need_add_ms", offsetof(lsarb_cfg, need_add_ms), 0, 1000},
    };
    int bad = 0;
    if (!s) return 0;
    while (*s) {
        char key[24];
        size_t n = 0, i;
        long v;
        char *e;
        while (*s == ',' || *s == ';' || *s == ' ') s++;
        if (!*s) break;
        while (s[n] && s[n] != '=' && s[n] != ',' && s[n] != ';' && n < sizeof key - 1) n++;
        memcpy(key, s, n);
        key[n] = 0;
        s += n;
        if (*s != '=') { bad++; while (*s && *s != ',' && *s != ';') s++; continue; }
        s++;
        v = strtol(s, &e, 10);
        if (e == s) { bad++; while (*s && *s != ',' && *s != ';') s++; continue; }
        s = e;
        for (i = 0; i < sizeof K / sizeof K[0]; i++) {
            if (strcmp(K[i].k, key) == 0) {
                *(int *)((char *)c + K[i].off) = cl((int)v, K[i].lo, K[i].hi);
                break;
            }
        }
        if (i == sizeof K / sizeof K[0]) bad++;
        while (*s && *s != ',' && *s != ';') s++;
    }
    if (c->lmax < c->lmin + 1) c->lmax = c->lmin + 1;
    return bad;
}

void lsarb_reset(lsarb_st *st, const lsarb_cfg *c)
{
    memset(st, 0, sizeof *st);
    st->hold = c->hold0;
    st->need = c->lmin;
}

int lsarb_need_update(lsarb_st *st, const lsarb_cfg *c, int rtt)
{
    int cand, cand_hi;
    if (c->need_add_ms <= 0 || rtt <= 0) {
        st->need = c->lmin;
        return st->need;
    }
    if (rtt > 10000) rtt = 10000;
    cand = (rtt + c->need_add_ms + 99) / 100;
    cand_hi = (rtt + c->need_add_ms + 40 + 99) / 100; /* Hysterese: runter erst 40 ms unter der Schwelle */
    if (cand < c->lmin) cand = c->lmin;
    if (cand > c->lmax - 1) cand = c->lmax - 1;
    if (cand > st->need) st->need = cand;
    else if (cand < st->need && cand_hi < st->need) st->need = cand;
    if (st->need < c->lmin) st->need = c->lmin;
    return st->need;
}

int lsarb_timekeeper(int npl, uint32_t mask)
{
    int i;
    for (i = 0; i < npl && i < 32; i++) {
        if (mask & (1u << i)) return i;
    }
    return -1;
}

void lsarb_measure(const lsarb_in *in, lsarb_out *o)
{
    int i, qmax = 0, qmin = 0x7F, r = 0x7FFF, npl = in->npl > LSARB_MAXP ? LSARB_MAXP : in->npl;
    memset(o, 0, sizeof *o);
    for (i = 0; i < npl; i++) {
        if (!(in->mask & (1u << i))) continue;
        if (in->q[i] > qmax) qmax = in->q[i];
        if (in->q[i] < qmin) qmin = in->q[i];
        if (i != in->self && in->q[i] < r) r = in->q[i];
    }
    if (qmax < 1) qmax = 1; /* wie 0x1D111..0x1D12B */
    if (qmax > 10000) qmax = 10000;
    o->qmax = qmax;
    o->qmin = qmin;
    o->r = r == 0x7FFF ? -1 : r;
    o->lead = (in->self >= 0 && in->self < LSARB_MAXP) ? in->q[in->self] : 0;
    o->steps = 1;
    /* 0x1D202: Nachholen (zwei Schritte je Bild), wenn der eigene Ring der knappste ist (wir liegen zurueck) */
    if (qmax - qmin > 1 && in->local >= 0 && in->local < LSARB_MAXP && (in->mask & (1u << in->local)) &&
        in->q[in->local] == qmin)
        o->steps = 2;
}

/* Taktgeber-Durchgang einmal je Tick wie 0x1D23C..0x1D25A ([0x2C9B40] = zuletzt bearbeiteter Tick) */
static int tk_pass(lsarb_ovars *ov, const lsarb_in *in)
{
    if (lsarb_timekeeper(in->npl, in->mask) != in->self) {
        ov->lasttick = in->tick;
        ov->have_last = 1;
        return 0;
    }
    if (ov->have_last && ov->lasttick == in->tick) return 0;
    ov->lasttick = in->tick;
    ov->have_last = 1;
    return 1;
}

void lsarb_orig_step(lsarb_ovars *ov, const lsarb_in *in, lsarb_out *o)
{
    int i, npl = in->npl > LSARB_MAXP ? LSARB_MAXP : in->npl, f, dmax, D = ov->D, all;
    lsarb_measure(in, o);
    /* 0x1D156..0x1D1FC: D-Regelung (jeder Client, jeder Aufruf) */
    f = cl(6 - D, 1, 3);
    if (ov->cool == 0 && (in->tick % 20u) == 0 && o->qmax - o->qmin > 2) D++;
    if ((in->tick % (uint32_t)(f * 300)) == 0 && o->qmax - o->qmin < 2) D--;
    dmax = cl(3 * ov->delay0, 6, 10);
    if (D < 2) D = 2;
    else if (D > dmax) D = dmax;
    ov->D = D;
    if (!tk_pass(ov, in)) return;
    o->tk_pass = 1;
    if (ov->cool != 0) { /* 0x1D37A */
        ov->cool--;
        return;
    }
    if (o->qmin >= D) {
        ov->xcnt = 0;
    } else if (ov->xcnt <= 0) {
        ov->xcnt++;
    } else {
        o->flags_or |= LSARB_F_EXTRA; /* 0x1D2CE */
        ov->cool = o->qmax;
        goto kick;
    }
    for (all = 1, i = 0; i < npl; i++) { /* 0x1D2E5: alle >= D+1 -> Rahmen weg */
        if ((in->mask & (1u << i)) && in->q[i] < D + 1) { all = 0; break; }
    }
    if (all) {
        o->flags_or |= LSARB_F_DROP;
        ov->cool = o->qmax;
    }
kick: /* 0x1D31D */
    if (o->qmax > LSARB_KICK_Q) {
        for (i = 0; i < npl; i++) {
            if ((in->mask & (1u << i)) && in->q[i] == o->qmax) o->mask_clear |= 1u << i;
        }
    }
}

static void win(const lsarb_st *st, int w, int *rmin, int *stalls)
{
    int k, n = (int)(st->hn < (uint32_t)w ? st->hn : (uint32_t)w), rm = 255, s = 0;
    for (k = 0; k < n; k++) {
        uint32_t j = (st->hn - 1u - (uint32_t)k) % LSARB_HIST;
        if (st->hr[j] < rm) rm = st->hr[j];
        s += st->hs[j];
    }
    *rmin = rm;
    *stalls = s;
}

void lsarb_tk_step(const lsarb_cfg *c, lsarb_st *st, lsarb_ovars *ov, const lsarb_in *in, lsarb_out *o)
{
    uint32_t sd;
    int L, r, rmin, stalls, floor;
    lsarb_measure(in, o);
    if (!tk_pass(ov, in)) return;
    o->tk_pass = 1;
    if (st->have && in->tick < st->last_tick) lsarb_reset(st, c); /* neue Mission im selben Prozess */
    sd = st->have && in->stall_total >= st->last_stall ? in->stall_total - st->last_stall : 0u;
    st->last_stall = in->stall_total;
    st->last_tick = in->tick;
    st->have = 1;
    L = o->lead;
    r = o->r;
    if (o->qmax > LSARB_KICK_Q) st->n_spike++;
    if (r < 0) return; /* allein: nichts zu regeln */
    st->hr[st->hn % LSARB_HIST] = (uint8_t)cl(r, 0, 255);
    st->hs[st->hn % LSARB_HIST] = (uint8_t)cl((int)(sd > 255u ? 255u : sd), 0, 255);
    st->hn++;
    if (st->cd > 0) st->cd--;
    if (st->hold > c->hold0 && in->tick - st->last_add > (uint32_t)(4 * st->hold)) { /* Sperrzeit klingt ab */
        st->hold /= 2;
        if (st->hold < c->hold0) st->hold = c->hold0;
        st->last_add = in->tick;
    }
    if (st->cd > 0) return;
    floor = st->need > c->lmin ? st->need : c->lmin;
    win(st, c->add_win, &rmin, &stalls);
    if (L < c->lmax && ((stalls >= c->add_stalls && L < floor + c->add_max) || L < floor)) {
        uint32_t k;
        o->flags_or |= LSARB_F_EXTRA;
        st->cd = L + c->add_cd;
        if (stalls >= c->add_stalls) {
            st->n_add++;
            if (st->last_drop && in->tick - st->last_drop < (uint32_t)(2 * st->hold)) {
                st->hold *= 2; /* Hin und Her: Rahmen-weg laenger sperren */
                if (st->hold > c->hold_max) st->hold = c->hold_max;
            }
        } else {
            st->n_add_rtt++;
        }
        st->last_add = in->tick;
        for (k = 0; k < LSARB_HIST; k++) st->hs[k] = 0; /* derselbe Stau zaehlt nicht zweimal */
        return;
    }
    if (L <= floor) return;
    if (st->hn >= (uint32_t)c->fast_win) {
        win(st, c->fast_win, &rmin, &stalls);
        if (stalls == 0 && rmin >= c->fast_rmin) { /* grosser Ueberhang: begrenzt abbauen statt rauswerfen */
            o->flags_or |= LSARB_F_DROP;
            st->cd = c->fast_cd;
            st->n_fast++;
            st->last_drop = in->tick;
            return;
        }
    }
    if (st->hn >= (uint32_t)c->drop_win && in->tick - st->last_add >= (uint32_t)st->hold) {
        win(st, c->drop_win, &rmin, &stalls);
        if (stalls == 0 && rmin >= c->drop_rmin) {
            o->flags_or |= LSARB_F_DROP;
            st->cd = c->drop_cd;
            st->n_drop++;
            st->last_drop = in->tick;
        }
    }
}
