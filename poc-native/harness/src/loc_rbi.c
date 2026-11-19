/* loc_rbi.c - W338 LOC-ROLLBACK-R2: Eingabeschicht fuer Rollback (Slots, Vorhersage, Kanal, Hash-Pruefung).
 * Beschreibung in loc_rbi.h. Reines Modul (kein Spielspeicher, keine Sockets, kein Log). */
#include "loc_rbi.h"

#include <stdlib.h>
#include <string.h>
#ifdef RBI_DEBUG
#include <stdio.h>
#endif

typedef uint32_t u32;

#define WM (RBI_WIN - 1u)
#define GM (RBI_GH - 1u)

typedef struct {
    uint8_t conf[RBI_WIN][RBI_FR]; /* bestaetigte Rahmen je Slot */
    u32 ctag[RBI_WIN];             /* slot + 1 */
    uint8_t srv[RBI_WIN][RBI_FR];  /* bediente Rahmen (Sync-Byte 0; Vorhersage ohne einmalige Flags) */
    u32 stag[RBI_WIN];
    uint8_t sfl[RBI_WIN];          /* 1 = vorhergesagt (offen), 2 = ok (bestaetigt bedient oder bestaetigt gleich) */
    u32 sg[RBI_WIN], sc[RBI_WIN];  /* Entnahme-Tick, Commits vor diesem Tick */
    u32 rh_acc[RBI_WIN], rh_gt[RBI_WIN], rh_tag[RBI_WIN]; /* Hashes des Mitspielers */
    uint8_t rh_cmp[RBI_WIN];
    u32 cons;                      /* naechster zu entnehmender Slot */
    int32_t contig, maxconf, verified;
    int32_t ack_of_me;             /* vom Mitspieler gemeldet: lueckenlos bestaetigt von meinen Rahmen */
    u32 rcons;                     /* vom Mitspieler gemeldet: sein naechster Slot */
    uint64_t src;
    int bound;
    u32 accept_below;              /* Fehlvorhersagen unter diesem Slot werden hingenommen (Rollback war nicht moeglich) */
} rbi_pl;

typedef struct {
    u32 tag;                       /* g + 1 */
    u32 c0;                        /* Commits vor dem Tick */
    uint8_t committed;
    uint16_t mask;                 /* Spieler mit Entnahme in diesem Tick */
    u32 cons[RBI_MAXP];            /* Stand zu Tick-Beginn */
    u32 own_prod;
} rbi_gt;

struct rbi {
    int active, self, delay, deff, maxpred, p0;
    u32 epoch;
    uint8_t oq[RBI_WIN][RBI_FR];   /* eigene Rahmen nach Erzeugung q */
    u32 oqtag[RBI_WIN];
    u32 own_prod;
    rbi_pl p[RBI_MAXP];
    u32 g, c;
    rbi_gt gt[RBI_GH];
    int replay;
    u32 replay_until;
    uint16_t rmask;
    u32 cbar;
    int rb_req;
    u32 rb_g, rb_slot;
    u32 h_acc[RBI_WIN], h_gt[RBI_WIN], h_tag[RBI_WIN];
    u32 emit_next;
    u32 last_emit[RBI_PKT_HASHES][3];
    int nlast;
    double adv;
    int freerun;                   /* W346: nach Desync/Ende: keine Rollbacks, kein Stau (Vorhersage ohne Grenze) */
    int hold;                      /* W346: Neuaufbau steht an - keine neue Vorhersage */
    int32_t hcmp_first;            /* W355: erster verglichener Slot der Mission (-1) */
    int hcmp_first_eq;
    int rbd_pend;                  /* W355: Neuaufbau aufgeschoben (reine Allokation) */
    u32 rbd_floor;                 /* W355: Spieltick der Allokation: Rollback-Ziel hoechstens hier */
    u32 rbd_ticks;                 /* W355: Live-Ticks des Aufschubs */
    rbi_stats_t st;
};

static void wr32(uint8_t *p, u32 v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static u32 rd32(const uint8_t *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

rbi_t *rbi_new(void)
{
    rbi_t *r = (rbi_t *)calloc(1, sizeof(rbi_t));
    if (r) r->st.hash_bad_first = -1;
    return r;
}
void rbi_free(rbi_t *r) { free(r); }

static void tick_start(rbi_t *r)
{
    rbi_gt *e = &r->gt[r->g & GM];
    int i;
    r->rmask = 0;
    if (r->replay && e->tag == r->g + 1u) r->rmask = e->mask; /* Original-Entnahmen dieses Ticks */
    e->tag = r->g + 1u;
    e->c0 = r->c;
    e->committed = 0;
    e->mask = 0;
    for (i = 0; i < RBI_MAXP; i++) e->cons[i] = r->p[i].cons;
    e->own_prod = r->own_prod;
}

void rbi_begin(rbi_t *r, u32 epoch, int self, int delay, int maxpred)
{
    int i;
    rbi_stats_t keep = r->st;
    memset(r, 0, sizeof *r);
    r->st = keep; /* Zaehler laufen ueber Missionen weiter */
    if (self < 0 || self >= RBI_MAXP) self = 0;
    if (delay < 1) delay = 1;
    if (delay > 8) delay = 8;
    if (maxpred < 1) maxpred = 1;
    if (maxpred > 64) maxpred = 64;
    r->self = self;
    r->delay = delay;
    r->maxpred = maxpred;
    r->epoch = epoch;
    r->p0 = -1;
    r->deff = -1;
    r->hcmp_first = -1;
    for (i = 0; i < RBI_MAXP; i++) {
        r->p[i].contig = r->p[i].maxconf = r->p[i].verified = r->p[i].ack_of_me = -1;
    }
    r->active = 1;
    tick_start(r);
}

int rbi_active(const rbi_t *r) { return r && r->active; }
u32 rbi_epoch(const rbi_t *r) { return r->epoch; }
int rbi_self(const rbi_t *r) { return r->self; }
int rbi_delay_eff(const rbi_t *r) { return r->deff; }
int rbi_p0(const rbi_t *r) { return r->p0; }
void rbi_end(rbi_t *r) { r->active = 0; r->replay = 0; r->rb_req = 0; }
int rbi_configure(rbi_t *r, u32 epoch, int delay)
{
    if (!r->active || r->p0 >= 0) return 0; /* nur vor der ersten Entnahme (Slot-Zuordnung steht danach fest) */
    if (delay < 1) delay = 1;
    if (delay > 8) delay = 8;
    r->epoch = epoch;
    r->delay = delay;
    return 1;
}
void rbi_free_run(rbi_t *r)
{
    r->freerun = 1;
    r->rb_req = 0;
}
int rbi_free_running(const rbi_t *r) { return r->freerun; }
void rbi_hold(rbi_t *r, int on) { r->hold = on ? 1 : 0; }
u32 rbi_replay_until(const rbi_t *r) { return r->replay_until; }
int rbi_open_preds(const rbi_t *r)
{
    int j;
    if (!r->active) return 0;
    /* W355: waehrend einer Neurechnung ist cons zurueckgesetzt, verified nicht - die Slot-Pruefung unten saehe dann
     * "nichts offen" (W346-Fehler: Neuaufbau + Barriere mitten in der Neurechnung, danach Rollback unmoeglich). */
    if (r->replay || r->rb_req) return 1;
    for (j = 0; j < RBI_MAXP; j++) {
        const rbi_pl *J = &r->p[j];
        u32 u = (u32)(J->verified + 1);
        if (j == r->self || u >= J->cons) continue;
        if (J->stag[u & WM] == u + 1u && J->sfl[u & WM] == 1) return 1;
        if (J->stag[u & WM] == u + 1u && J->sfl[u & WM] != 2) return 1;
    }
    return 0;
}
int32_t rbi_hash_first_cmp(const rbi_t *r, int *equal)
{
    if (equal) *equal = r->hcmp_first_eq;
    return r->hcmp_first;
}
u32 rbi_rebuild_wait(const rbi_t *r) { return r->rbd_pend ? r->rbd_ticks : 0u; }
void rbi_rebuild_note(rbi_t *r)
{
    u32 t = r->g ? r->g - 1u : 0u;
    if (r->rbd_pend && t < r->rbd_floor) r->rbd_floor = t;
}
int rbi_force_rb(rbi_t *r, u32 ticks_back, u32 avail)
{
    u32 gt, k;
    const rbi_gt *e;
    if (!r->active || r->replay || r->freerun || r->rb_req || ticks_back == 0 || ticks_back > r->g) return 0;
    gt = r->g - ticks_back;
    if (r->rbd_pend && gt > r->rbd_floor) gt = r->rbd_floor;
    e = &r->gt[gt & GM];
    if (e->tag != gt + 1u || e->c0 < r->cbar) return 0; /* nur, was der Ring sicher kann (kein "verloren") */
    k = r->c - e->c0;
    if (k == 0 || k > avail) return 0;
    r->rb_g = r->g - ticks_back;
    r->rb_slot = r->p[r->self].cons;
    r->rb_req = 1;
    return 1;
}
int rbi_rebuild_gate(rbi_t *r, int need, int only_alloc, int defer_on, u32 max_ticks)
{
    if (!need) {
        if (r->rbd_pend) { /* Bereiche wieder gleich (kommt nicht vor): Aufschub beenden */
            r->rbd_pend = 0;
            r->hold = 0;
        }
        return RBI_REBUILD_NONE;
    }
    if (r->active && !r->freerun && only_alloc && defer_on) {
        int busy = rbi_open_preds(r);
        if (!r->rbd_pend) {
            if (!busy) return RBI_REBUILD_NOW;
            r->rbd_pend = 1;
            r->rbd_floor = r->g ? r->g - 1u : 0u; /* Aufruf nach rbi_tick_end: der Tick mit der Allokation */
            r->rbd_ticks = 0;
            r->st.rebuild_deferred++;
        }
        if (r->replay) { /* nie mitten in einer Neurechnung (Barriere dort = spaeterer Rollback unmoeglich) */
            r->st.rebuild_defer_replay++;
            r->hold = 1;
            return RBI_REBUILD_DEFER;
        }
        if (busy && r->rbd_ticks < max_ticks) {
            r->rbd_ticks++;
            r->hold = 1;
            return RBI_REBUILD_DEFER;
        }
        if (busy) r->st.rebuild_forced++;
    }
    r->rbd_pend = 0;
    r->hold = 0;
    return RBI_REBUILD_NOW;
}
u32 rbi_commits(const rbi_t *r) { return r->c; }
u32 rbi_gtick(const rbi_t *r) { return r->g; }
int rbi_replaying(const rbi_t *r) { return r->replay; }
int rbi_rb_pending(const rbi_t *r) { return r->active && r->rb_req && !r->replay; }
const rbi_stats_t *rbi_stats(const rbi_t *r) { return &r->st; }
double rbi_advantage(const rbi_t *r) { return r->adv; }
int rbi_cons(const rbi_t *r, int i) { return (i >= 0 && i < RBI_MAXP) ? (int)r->p[i].cons : -1; }
int rbi_contig(const rbi_t *r, int i) { return (i >= 0 && i < RBI_MAXP) ? r->p[i].contig : -1; }

static void put_conf(rbi_pl *P, u32 s, const uint8_t *f)
{
    memcpy(P->conf[s & WM], f, RBI_FR);
    P->ctag[s & WM] = s + 1u;
    if ((int32_t)s > P->maxconf) P->maxconf = (int32_t)s;
    while (P->ctag[(u32)(P->contig + 1) & WM] == (u32)(P->contig + 2)) P->contig++;
}

static void advance_verified(rbi_pl *P)
{
    int n = 0;
    while ((u32)(P->verified + 1) < P->cons && n++ < (int)RBI_WIN) {
        u32 s = (u32)(P->verified + 1);
        if (P->stag[s & WM] != s + 1u || P->sfl[s & WM] != 2) break;
        P->verified++;
    }
}

/* Vergleich bedient <-> bestaetigt ohne Sync-Byte (Byte 1). */
static int eq_nosync(const uint8_t *a, const uint8_t *b) { return a[0] == b[0] && memcmp(a + 2, b + 2, RBI_FR - 2) == 0; }

int rbi_own(rbi_t *r, const uint8_t hdr[RBI_HDR], const uint8_t in[RBI_IN])
{
    u32 q;
    if (!r->active) return 0;
    q = r->own_prod++;
    if (r->oqtag[q & WM] == q + 1u) {
        r->st.own_dup++;
        return 0;
    }
    memcpy(r->oq[q & WM], hdr, RBI_HDR);
    memcpy(r->oq[q & WM] + RBI_HDR, in, RBI_IN);
    r->oqtag[q & WM] = q + 1u;
    r->st.own_frames++;
    if (r->p0 >= 0) {
        int32_t n = (int32_t)q - r->p0 + r->deff;
        if (n >= 0) put_conf(&r->p[r->self], (u32)n, r->oq[q & WM]);
    }
    return 1;
}

/* Rollback nicht moeglich: offene Vorhersagen bis zum aktuellen Stand hinnehmen, damit das Spiel nicht im Fenster
 * haengen bleibt (der Zustand kann abweichen; die Hash-Pruefung meldet es). */
static void accept_loss(rbi_t *r)
{
    int j;
    for (j = 0; j < RBI_MAXP; j++) {
        rbi_pl *P = &r->p[j];
        u32 s;
        if (j == r->self) continue;
        P->accept_below = P->cons;
        for (s = (u32)(P->verified + 1); s < P->cons; s++) {
            u32 ix = s & WM;
            if (P->stag[ix] == s + 1u && P->sfl[ix] == 1 && P->ctag[ix] == s + 1u) {
                P->sfl[ix] = 2;
                r->st.rb_accepted++;
            }
        }
        advance_verified(P);
    }
}

static int find_last_conf(const rbi_pl *P, u32 n, u32 *out)
{
    int32_t l = (int32_t)n - 1;
    int k = 0;
    if (P->maxconf < l) l = P->maxconf;
    for (; l >= 0 && k < (int)RBI_WIN / 2; l--, k++) {
        if (P->ctag[(u32)l & WM] == (u32)l + 1u) {
            *out = (u32)l;
            return 1;
        }
    }
    return 0;
}

int rbi_take(rbi_t *r, int i, u32 cmin, uint8_t out[RBI_FR])
{
    rbi_pl *P, *S;
    u32 n, ix, l = 0;
    const uint8_t *src;
    uint8_t pred_fr[RBI_FR];
    int pred = 0;
    if (!r->active || i < 0 || i >= RBI_MAXP) return 0;
    if (r->p0 < 0) { /* erste Entnahme der Mission: Vorlauf steht fest, eigene Rahmen bekommen Slots */
        u32 q;
        r->p0 = (int)r->own_prod;
        r->deff = r->delay < r->p0 ? r->delay : r->p0;
        for (q = r->own_prod > RBI_WIN ? r->own_prod - RBI_WIN : 0; q < r->own_prod; q++) {
            int32_t s = (int32_t)q - r->p0 + r->deff;
            if (s >= 0 && r->oqtag[q & WM] == q + 1u) put_conf(&r->p[r->self], (u32)s, r->oq[q & WM]);
        }
    }
    P = &r->p[i];
    S = &r->p[r->self];
    n = P->cons;
    ix = n & WM;
    if (r->replay && !(r->rmask & (1u << i))) return 0; /* Neurechnung: im Original hier Stau */
    if (!r->replay && !r->freerun) { /* Ringtiefe: jede Entnahme fuehrt zu einem Commit. Der aelteste offene (vorhergesagte) Slot
                       * aller Mitspieler muss danach noch ladbar sein (Reserve 2: eigener Commit + Stau-Tick). */
        int j;
        for (j = 0; j < RBI_MAXP; j++) {
            rbi_pl *J = &r->p[j];
            u32 uj = (u32)(J->verified + 1);
            if (j == r->self || uj >= J->cons || J->stag[uj & WM] != uj + 1u) continue;
            if (J->sc[uj & WM] < cmin + 2u || J->sc[uj & WM] < r->cbar) {
                r->st.stall_depth++;
                return 0;
            }
        }
    }
    if (P->ctag[ix] == n + 1u) {
        src = P->conf[ix];
    } else if (i == r->self) {
        r->st.own_missing++;
        return 0;
    } else {
        if (!find_last_conf(P, n, &l)) {
            if (r->freerun && !r->replay) { /* W346: Mitspieler weg - neutraler Rahmen (n=1, keine Flags/Eingabe) */
                memset(pred_fr, 0, RBI_FR);
                pred_fr[0] = 1;
                goto serve_pred;
            }
            if (r->replay) r->st.replay_stall_mismatch++;
            else r->st.stall_first++;
            return 0;
        }
        if (!r->replay && !r->freerun) { /* Vorhersagefenster in Slots; neue Vorhersage nur, wenn der Ring zurueck kann */
            u32 u = (u32)(P->verified + 1);
            if (r->hold) { /* W346: Neuaufbau steht an */
                r->st.stall_hold++;
                return 0;
            }
            if (r->c < cmin + 2u) {
                r->st.stall_depth++;
                return 0;
            }
            if (u < n && (int)(n - u) >= r->maxpred) {
                r->st.stall_window++;
                return 0;
            }
        }
        memcpy(pred_fr, P->conf[l & WM], RBI_FR);
        wr32(pred_fr + 4, rd32(pred_fr + 4) & ~RBI_FL_TRANSIENT);
    serve_pred:
        src = pred_fr;
        pred = 1;
    }
    memcpy(P->srv[ix], src, RBI_FR);
    P->srv[ix][1] = 0;
    P->stag[ix] = n + 1u;
    P->sfl[ix] = (uint8_t)(pred ? 1 : 2);
    P->sg[ix] = r->g;
    P->sc[ix] = r->c;
    memcpy(out, P->srv[ix], RBI_FR);
    out[1] = S->ctag[ix] == n + 1u ? S->conf[ix][1] : 0; /* eigenes Sync-Byte: die Spiel-Pruefung sieht immer Gleichheit */
    P->cons = n + 1u;
    r->gt[r->g & GM].mask |= (uint16_t)(1u << i);
    r->st.taken++;
    if (pred) r->st.predicted++;
    else {
        r->st.taken_conf++;
        advance_verified(P);
    }
    return 1;
}

static void hash_cmp(rbi_t *r, int i, u32 s)
{
    rbi_pl *P = &r->p[i];
    u32 ix = s & WM;
    if (P->rh_tag[ix] != s + 1u || P->rh_cmp[ix] || r->h_tag[ix] != s + 1u) return;
    P->rh_cmp[ix] = 1;
    r->st.hash_cmp++;
    if (r->hcmp_first < 0) {
        r->hcmp_first = (int32_t)s;
        r->hcmp_first_eq = P->rh_acc[ix] == r->h_acc[ix] && P->rh_gt[ix] == r->h_gt[ix];
    }
    if (P->rh_acc[ix] != r->h_acc[ix] || P->rh_gt[ix] != r->h_gt[ix]) {
        if (!r->st.hash_bad) {
            r->st.hash_bad_first = (int32_t)s;
            r->st.hb_acc[0] = r->h_acc[ix];
            r->st.hb_gt[0] = r->h_gt[ix];
            r->st.hb_acc[1] = P->rh_acc[ix];
            r->st.hb_gt[1] = P->rh_gt[ix];
        }
        r->st.hash_bad++;
    }
}

void rbi_acc(rbi_t *r, u32 acc, u32 gtick)
{
    u32 s, ix;
    if (!r->active || r->p0 < 0 || r->p[r->self].cons == 0) return;
    s = r->p[r->self].cons - 1u;
    ix = s & WM;
    if (s < r->emit_next) { /* schon endgueltig: Neurechnung muss dasselbe ergeben */
        if (r->h_tag[ix] == s + 1u && (r->h_acc[ix] != acc || r->h_gt[ix] != gtick)) r->st.hash_recalc_diff++;
        return;
    }
    r->h_acc[ix] = acc;
    r->h_gt[ix] = gtick;
    r->h_tag[ix] = s + 1u;
}

int rbi_final_next(rbi_t *r, u32 *slot, u32 *acc, u32 *gtick)
{
    int32_t v = 0x7FFFFFFF;
    int j, any = 0;
    u32 own = r->p[r->self].cons;
    if (!r->active || r->p0 < 0 || r->replay) return 0; /* W355: nie im Neurechnen (cons zurueckgesetzt) */
    for (j = 0; j < RBI_MAXP; j++) { /* alle Spieler, die zuletzt entnommen wurden */
        rbi_pl *J = &r->p[j];
        if (J->cons == 0 || J->cons + 64u < own) continue;
        any = 1;
        if (J->verified < v) v = J->verified;
    }
    if (!any) return 0;
    while ((int32_t)r->emit_next <= v) {
        u32 s = r->emit_next, ix = s & WM;
        if (r->h_tag[ix] != s + 1u) {
            if (v - (int32_t)s > 4) { /* in diesem Slot kein Rahmenbau gesehen */
                r->st.hash_skip++;
                r->emit_next++;
                continue;
            }
            return 0;
        }
        r->emit_next++;
        r->st.hash_final++;
        memmove(r->last_emit[1], r->last_emit[0], sizeof r->last_emit[0] * (RBI_PKT_HASHES - 1));
        r->last_emit[0][0] = s;
        r->last_emit[0][1] = r->h_acc[ix];
        r->last_emit[0][2] = r->h_gt[ix];
        if (r->nlast < RBI_PKT_HASHES) r->nlast++;
        for (j = 0; j < RBI_MAXP; j++)
            if (j != r->self) hash_cmp(r, j, s);
        *slot = s;
        *acc = r->h_acc[ix];
        *gtick = r->h_gt[ix];
        return 1;
    }
    return 0;
}

int rbi_tick_end(rbi_t *r, int *replay_done)
{
    rbi_gt *e;
    int com;
    if (replay_done) *replay_done = 0;
    if (!r->active) return 1;
    e = &r->gt[r->g & GM];
    com = e->mask != 0;
    e->committed = (uint8_t)com;
    r->c += (u32)com;
    r->g++;
    if (r->replay) {
        r->st.replay_ticks++;
        if (r->g == r->replay_until) {
            r->replay = 0;
            if (replay_done) *replay_done = 1;
        }
    }
    tick_start(r);
    return com;
}

void rbi_barrier(rbi_t *r) { r->cbar = r->c; }

int rbi_plan(rbi_t *r, u32 avail, u32 *ticks, u32 *slots_back)
{
    rbi_gt *e;
    u32 gt, gs, c0, k, own_before;
    int i;
    if (ticks) *ticks = 0;
    if (slots_back) *slots_back = 0;
    if (!r->active || !r->rb_req || r->replay) return 0;
    r->rb_req = 0;
    gt = r->rb_g;
    if (r->rbd_pend && gt > r->rbd_floor) { /* W355: der neue Block liegt nicht im Ring - ab der Allokation neu rechnen */
        gt = r->rbd_floor;
        r->st.rb_floored++;
    }
    e = &r->gt[gt & GM];
    if (e->tag != gt + 1u || gt >= r->g) {
#ifdef RBI_DEBUG
        printf("lost A self=%d gt=%u g=%u tag=%u\n", r->self, gt, r->g, e->tag);
#endif
        r->st.rb_lost++;
        accept_loss(r);
        return -1;
    }
    c0 = e->c0;
    k = r->c - c0;
    if (c0 < r->cbar || k == 0 || k > avail) {
#ifdef RBI_DEBUG
        printf("lost B self=%d gt=%u g=%u c=%u c0=%u k=%u avail=%u slot=%u\n", r->self, gt, r->g, r->c, c0, k, avail, r->rb_slot);
#endif
        r->st.rb_lost++;
        accept_loss(r);
        return -1;
    }
    gs = gt; /* reine Stau-Ticks davor wurden mit dem Entnahme-Tick zusammen gesichert -> mit neu rechnen */
    while (gs > 0) {
        rbi_gt *p = &r->gt[(gs - 1u) & GM];
        if (p->tag != gs || p->committed) break;
        gs--;
    }
    e = &r->gt[gs & GM];
    if (e->tag != gs + 1u || e->c0 != c0 || r->g - gs >= RBI_GH / 2) {
#ifdef RBI_DEBUG
        printf("lost C self=%d gs=%u gt=%u c0=%u ec0=%u\n", r->self, gs, gt, c0, e->c0);
#endif
        r->st.rb_lost++;
        accept_loss(r);
        return -1;
    }
    own_before = r->p[r->self].cons;
    for (i = 0; i < RBI_MAXP; i++) r->p[i].cons = e->cons[i];
    r->own_prod = e->own_prod;
    r->replay_until = r->g;
    r->g = gs;
    r->c = c0;
    r->replay = 1;
    tick_start(r);
    r->st.rb_plans++;
    if (ticks) *ticks = r->replay_until - gs;
    if (slots_back) *slots_back = own_before - r->p[r->self].cons;
    return (int)k;
}

/* ---- Kanal ----
 * Kopf (28 B, little endian): "SBR2", ver=1, Absender-Index, nack, nfr, nh, 3 x 0, Epoche, eigener naechster Slot,
 * erster mitgeschickter Slot, 0. Dann nack x {Spieler, 0,0,0, lueckenlos-bis (int32)}, nfr x Rahmen (22 B),
 * nh x {Slot, Akkumulator, Spieltick}. Laenge exakt. */
int rbi_tx(rbi_t *r, uint8_t *buf, int cap)
{
    rbi_pl *S;
    int32_t lo = 0x7FFFFFFF, hi;
    int j, nack = 0, nfr = 0, nh, off, any = 0;
    if (!r->active || cap < RBI_PKT_MAX || r->replay) return 0; /* W355: nie im Neurechnen (cons zurueckgesetzt) */
    S = &r->p[r->self];
    for (j = 0; j < RBI_MAXP; j++) {
        if (j == r->self || !r->p[j].bound) continue;
        any = 1;
        if (r->p[j].ack_of_me + 1 < lo) lo = r->p[j].ack_of_me + 1;
    }
    if (!any) lo = 0;
    hi = S->maxconf;
    if (lo < hi - (int32_t)RBI_WIN / 2) lo = hi - (int32_t)RBI_WIN / 2;
    if (lo < 0) lo = 0;
    off = RBI_PKT_HEAD;
    for (j = 0; j < RBI_MAXP; j++) {
        if (j == r->self || (!r->p[j].bound && r->p[j].maxconf < 0)) continue;
        buf[off] = (uint8_t)j;
        buf[off + 1] = buf[off + 2] = buf[off + 3] = 0;
        wr32(buf + off + 4, (u32)r->p[j].contig);
        off += 8;
        nack++;
    }
    if (r->p0 >= 0 && lo <= hi) {
        int32_t s;
        for (s = lo; s <= hi && nfr < RBI_PKT_FRAMES; s++) {
            if (S->ctag[(u32)s & WM] != (u32)s + 1u) break;
            memcpy(buf + off, S->conf[(u32)s & WM], RBI_FR);
            off += RBI_FR;
            nfr++;
        }
    }
    nh = r->nlast;
    for (j = 0; j < nh; j++) {
        wr32(buf + off, r->last_emit[j][0]);
        wr32(buf + off + 4, r->last_emit[j][1]);
        wr32(buf + off + 8, r->last_emit[j][2]);
        off += 12;
    }
    memcpy(buf, "SBR2", 4);
    buf[4] = 1;
    buf[5] = (uint8_t)r->self;
    buf[6] = (uint8_t)nack;
    buf[7] = (uint8_t)nfr;
    buf[8] = (uint8_t)nh;
    buf[9] = buf[10] = buf[11] = 0;
    wr32(buf + 12, r->epoch);
    wr32(buf + 16, S->cons);
    wr32(buf + 20, (u32)lo);
    wr32(buf + 24, 0);
    r->st.tx_pkts++;
    r->st.tx_frames += (u32)nfr;
    return off;
}

int rbi_rx(rbi_t *r, const uint8_t *b, int len, uint64_t src)
{
    int i, nack, nfr, nh, k, j;
    u32 first, rcons;
    const uint8_t *pa, *pf, *ph;
    rbi_pl *P;
    if (!r->active) return 0;
    if (!b || len < RBI_PKT_HEAD || memcmp(b, "SBR2", 4) != 0 || b[4] != 1 || b[9] || b[10] || b[11] || rd32(b + 24)) {
        r->st.rx_bad++;
        return -1;
    }
    i = b[5];
    nack = b[6];
    nfr = b[7];
    nh = b[8];
    if (i >= RBI_MAXP || i == r->self || nack > RBI_MAXP || nfr > RBI_PKT_FRAMES || nh > RBI_PKT_HASHES ||
        len != RBI_PKT_HEAD + nack * 8 + nfr * RBI_FR + nh * 12) {
        r->st.rx_bad++;
        return -1;
    }
    if (rd32(b + 12) != r->epoch) {
        r->st.rx_epoch++;
        return 0;
    }
    rcons = rd32(b + 16);
    first = rd32(b + 20);
    pa = b + RBI_PKT_HEAD;
    pf = pa + nack * 8;
    ph = pf + nfr * RBI_FR;
    if (first > 0x40000000u || rcons > 0x40000000u) {
        r->st.rx_bad++;
        return -1;
    }
    for (k = 0; k < nack; k++) { /* Pass 1: alles pruefen, bevor etwas uebernommen wird */
        int32_t c = (int32_t)rd32(pa + k * 8 + 4);
        if (pa[k * 8] >= RBI_MAXP || pa[k * 8 + 1] || pa[k * 8 + 2] || pa[k * 8 + 3] || c < -1 || c > 0x40000000) {
            r->st.rx_bad++;
            return -1;
        }
    }
    for (k = 0; k < nfr; k++) {
        const uint8_t *f = pf + k * RBI_FR;
        if (f[0] != 1 || f[2] || f[3]) { /* n = 1 wie im Spiel (Gastfilter-Regel 0x34: n <= 1), Fuellbytes 0 */
            r->st.rx_bad++;
            return -1;
        }
    }
    for (k = 0; k < nh; k++) {
        if (rd32(ph + k * 12) > 0x40000000u) {
            r->st.rx_bad++;
            return -1;
        }
    }
    for (j = 0; j < RBI_MAXP; j++) { /* eine Absenderkennung = ein Spieler je Epoche */
        if (j != i && r->p[j].bound && r->p[j].src == src) {
            r->st.rx_bind++;
            return -1;
        }
    }
    P = &r->p[i];
    if (P->bound && P->src != src) {
        r->st.rx_bind++;
        return -1;
    }
    P->bound = 1;
    P->src = src;
    r->st.rx_ok++;
    P->rcons = rcons;
    for (k = 0; k < nack; k++) {
        int32_t c = (int32_t)rd32(pa + k * 8 + 4);
        if (pa[k * 8] == r->self && c > P->ack_of_me) P->ack_of_me = c;
    }
    for (k = 0; k < nfr; k++) {
        u32 s = first + (u32)k, ix = s & WM;
        const uint8_t *f = pf + k * RBI_FR;
        if (s + RBI_WIN / 2 < P->cons || s >= P->cons + RBI_WIN / 2) {
            r->st.rx_far++;
            continue;
        }
        if (P->ctag[ix] == s + 1u) {
            if (memcmp(P->conf[ix], f, RBI_FR) != 0) r->st.rx_conflict++; /* erster Stand gilt */
            else r->st.rx_dup++;
            continue;
        }
        put_conf(P, s, f);
        r->st.rx_frames++;
        if (s < P->cons && P->stag[ix] == s + 1u && P->sfl[ix] == 1) {
            if (eq_nosync(P->srv[ix], f)) {
                P->sfl[ix] = 2;
                r->st.pred_ok++;
            } else if (s < P->accept_below) { /* Rollback dorthin war nicht mehr moeglich: weiter (Desync moeglich) */
                P->sfl[ix] = 2;
                r->st.pred_bad++;
                r->st.rb_accepted++;
            } else if (r->freerun) { /* W346: nach dem Ende kein Rollback mehr */
                P->sfl[ix] = 2;
                r->st.pred_bad++;
            } else {
                r->st.pred_bad++;
                if (!r->rb_req || P->sg[ix] < r->rb_g) {
                    r->rb_g = P->sg[ix];
                    r->rb_slot = s;
                }
                r->rb_req = 1;
            }
        }
    }
    advance_verified(P);
    for (k = 0; k < nh; k++) {
        u32 s = rd32(ph + k * 12), ix = s & WM;
        if (s + RBI_WIN / 2 < r->emit_next || s >= r->emit_next + RBI_WIN / 2) continue;
        if (P->rh_tag[ix] != s + 1u) {
            P->rh_tag[ix] = s + 1u;
            P->rh_acc[ix] = rd32(ph + k * 12 + 4);
            P->rh_gt[ix] = rd32(ph + k * 12 + 8);
            P->rh_cmp[ix] = 0;
        }
        if (s < r->emit_next) hash_cmp(r, i, s);
    }
    { /* Zeitabgleich: eigener Vorsprung gegen den des Mitspielers (je in Slots), halbiert, geglaettet */
        double la = (double)r->p[r->self].cons - (double)(P->maxconf + 1);
        double ra = (double)rcons - (double)(P->ack_of_me + 1);
        r->adv = 0.9 * r->adv + 0.1 * ((la - ra) / 2.0);
    }
    return 1;
}

#ifdef RBI_DEBUG
void rbi_debug_dump(const rbi_t *r, u32 cmin)
{
    int j;
    printf("  self=%d g=%u c=%u cmin=%u cbar=%u replay=%d rb_req=%d own_prod=%u p0=%d\n", r->self, r->g, r->c, cmin,
           r->cbar, r->replay, r->rb_req, r->own_prod, r->p0);
    for (j = 0; j < 2; j++) {
        const rbi_pl *P = &r->p[j];
        u32 u = (u32)(P->verified + 1);
        printf("  p%d cons=%u contig=%d maxconf=%d verified=%d ack_of_me=%d u=%u stag=%u sfl=%u sc=%u sg=%u ctag=%u\n", j,
               P->cons, P->contig, P->maxconf, P->verified, P->ack_of_me, u, P->stag[u & WM], P->sfl[u & WM],
               P->sc[u & WM], P->sg[u & WM], P->ctag[u & WM]);
    }
}
#endif

/* ---- W359 LOC-RB-FIDELITY ---- */
uint32_t rbi_rjob_replay(int prep, int at, uint32_t cur)
{
    if (prep) return 0u; /* Vorbereitung wie live; Zeichnen entfaellt, Auftrag sofort erledigt */
    if (at == RBI_RJOB_LOAD) return 1u; /* alt: Vorbereitung ueberspringen */
    if (at == RBI_RJOB_DONE) return 0u;
    return cur;
}

#define RBI_HP_FL 0x180u
#define RBI_HP_BM 0x160u
int rbi_heap_check(rbi_rd32_fn rd, void *ctx, uint32_t heap, uint32_t max_steps, uint32_t *bad, uint32_t *nodes)
{
    uint32_t i, steps = 0, bm[4];
    *bad = 0;
    *nodes = 0;
    for (i = 0; i < 4; i++) {
        if (!rd(ctx, heap + RBI_HP_BM + i * 4u, &bm[i])) { *bad = heap + RBI_HP_BM; return RBI_HEAP_HDR; }
    }
    for (i = 0; i < 128u; i++) {
        uint32_t h = heap + RBI_HP_FL + i * 8u, f, b, prev = h, n, last = 0;
        if (!rd(ctx, h, &f) || !rd(ctx, h + 4u, &b)) { *bad = h; return RBI_HEAP_HDR; }
        if (i && f != h && !(bm[i >> 5] & (1u << (i & 31u)))) { *bad = h; return RBI_HEAP_BITMAP; }
        for (n = f; n != h;) {
            uint32_t nf, nb, w0, w1, size;
            if (steps >= max_steps) { *nodes = steps; return RBI_HEAP_OK; } /* unvollstaendig, kein Fehler */
            steps++;
            if ((n & 15u) != 8u || !rd(ctx, n, &nf) || !rd(ctx, n + 4u, &nb) || !rd(ctx, n - 8u, &w0) ||
                !rd(ctx, n - 4u, &w1)) { *bad = n; return RBI_HEAP_PTR; }
            if (nb != prev) { *bad = n; return RBI_HEAP_LINK; }
            if ((w1 >> 8) & 1u) { *bad = n - 8u; return RBI_HEAP_BUSY; }
            size = w0 & 0xFFFFu;
            if (i ? size != i : size < 0x80u) { *bad = n - 8u; return RBI_HEAP_SIZE; }
            if (!i && size < last) { *bad = n - 8u; return RBI_HEAP_ORDER; }
            last = size;
            prev = n;
            n = nf;
        }
        if (b != prev) { *bad = h; return RBI_HEAP_LINK; }
    }
    *nodes = steps;
    return RBI_HEAP_OK;
}
const char *rbi_heap_err(int code)
{
    static const char *const k[RBI_HEAP_N] = {"ok", "Kopf nicht lesbar", "Knoten ungueltig", "Rueckverkettung falsch",
                                              "Blockgroesse passt nicht zur Liste", "Block als belegt markiert",
                                              "Liste 0 nicht aufsteigend", "Bitmap ohne Bit fuer volle Liste"};
    return code >= 0 && code < RBI_HEAP_N ? k[code] : "?";
}
