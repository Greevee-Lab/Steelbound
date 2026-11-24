/* loc_rbn_pre_cases.c - W475 LOC-RB-PREAGREE: Selbsttest der Vorab-Einigung in der Lobby (loc_rbn.c, Paket SBRP) und
 * rbi_prime/rbi_rebind (loc_rbi.c). Teil von loc_rbi_selftest.
 *
 * Netz-Zwilling mit bis zu 4 Knoten (Knoten 0 = Host): Hallo + SBRP im 200-ms-Takt mit Einweg-Verzoegerung, Verlust,
 * Beitritt; Rahmen der Eingabeschicht als Ereignis {Epoche} alle 50 ms von jedem Knoten in R2 bzw. PRE (wie rbi_rx:
 * angenommen nur mit gleicher Epoche -> Beweis rbn_pre_proof bzw. Bestaetigung rbn_confirm_peer).
 * Pruefungen je Lauf: am Ende alle wollenden Knoten R2 mit gleicher Epoche UND Verzoegerung oder alle Lockstep; ein Knoten,
 * der einmal R2 ist (entnimmt), wechselt die Epoche nie; der Host nutzt die Vorab-Einigung nur, wenn jeder Client sie
 * auch nutzt bzw. ihr folgt. */
#include "loc_rbi.h"
#include "loc_rbn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t u32;

static int s_fail, *s_ok;
#define PCHECK(c, ...)                                                                                                 \
    do {                                                                                                               \
        if (c) (*s_ok)++;                                                                                              \
        else {                                                                                                         \
            s_fail++;                                                                                                  \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

static u32 s_prng = 0x47511u;
static u32 prnd(void)
{
    s_prng ^= s_prng << 13;
    s_prng ^= s_prng >> 17;
    s_prng ^= s_prng << 5;
    return s_prng;
}

#define PN 4
#define PQ 1024
typedef struct {
    int to, kind; /* kind 0 = Paket (b/len), 1 = Rahmen mit Epoche ep */
    u32 at, ep;
    uint64_t src;
    int len;
    uint8_t b[RBN_PRE_MAX > RBN_PKT_MAX ? RBN_PRE_MAX : RBN_PKT_MAX];
} pq_t;

typedef struct {
    int n;                 /* Knoten */
    rbn_t *r[PN];
    int on[PN];            /* im Netz (Hallo laeuft) */
    int self[PN];          /* Spielerindex im Hallo (0xFF/-1 Lobby) */
    int started[PN];       /* Mission begonnen */
    u32 r2_ep[PN];         /* Epoche beim ersten R2 (0 = nie) */
    int r2_d[PN];
    int ep_changed[PN];    /* R2 und danach andere Epoche (darf nie) */
    u32 owd, loss;
    pq_t q[PQ];
    u32 now;
} pnet_t;

static uint64_t psrc(int i) { return ((uint64_t)(0x0A000010u + (u32)i) << 16) | 2000u; }
static u32 pip(int i) { return (u32)(psrc(i) >> 16); }

static void pq_put(pnet_t *N, int to, int kind, u32 ep, uint64_t src, const uint8_t *b, int len)
{
    int k;
    if (N->loss && prnd() % 100u < N->loss) return;
    for (k = 0; k < PQ; k++)
        if (!N->q[k].len && !N->q[k].kind) {
            pq_t *P = &N->q[k];
            P->to = to;
            P->kind = kind;
            P->at = N->now + N->owd + (prnd() % 3u);
            P->ep = ep;
            P->src = src;
            P->len = kind ? 1 : len;
            if (!kind) memcpy(P->b, b, (size_t)len);
            return;
        }
}

static void pn_init(pnet_t *N, int n, u32 owd)
{
    int i;
    memset(N, 0, sizeof *N);
    N->n = n;
    N->owd = owd;
    N->now = 5000;
    for (i = 0; i < n; i++) {
        N->r[i] = rbn_new(1, 0x11110000u * (u32)(i + 1) + 0x77u + (prnd() & 0xFF00u), 2, 3, 0);
        N->on[i] = 1;
        N->self[i] = i == 0 ? 0 : -1;
        rbn_set_lobby(N->r[i], i == 0 ? RBN_ROLE_HOST : RBN_ROLE_JOIN, i == 0 ? 0 : pip(0), i == 0 ? 0xC0FFEEu : 0);
    }
}
static void pn_free(pnet_t *N)
{
    int i;
    for (i = 0; i < N->n; i++) rbn_free(N->r[i]);
}

static void pn_note(pnet_t *N, int i)
{
    if (rbn_state(N->r[i]) != RBN_ST_R2) return;
    if (!N->r2_ep[i]) {
        N->r2_ep[i] = rbn_epoch(N->r[i]);
        N->r2_d[i] = rbn_delay(N->r[i]);
    } else if (N->r2_ep[i] != rbn_epoch(N->r[i]) || N->r2_d[i] != rbn_delay(N->r[i])) {
        N->ep_changed[i] = 1;
    }
}

static void pn_step(pnet_t *N, u32 ms)
{
    u32 end = N->now + ms;
    for (; (int32_t)(end - N->now) > 0; N->now += 10) {
        int k, i, j;
        for (k = 0; k < PQ; k++) {
            pq_t *P = &N->q[k];
            if (!P->len || (int32_t)(N->now - P->at) < 0) continue;
            if (N->on[P->to]) {
                rbn_t *R = N->r[P->to];
                if (P->kind) { /* Rahmen: rbi_rx nimmt nur die eigene Epoche an */
                    int st = rbn_state(R);
                    if ((st == RBN_ST_PRE || st == RBN_ST_R2 || st == RBN_ST_TENT) && P->ep == rbn_epoch(R)) {
                        if (st == RBN_ST_PRE) rbn_pre_proof(R, P->src);
                        else if (st == RBN_ST_TENT) rbn_confirm_peer(R, P->src);
                    }
                } else if (rbn_is_hello(P->b, P->len)) {
                    rbn_rx(R, N->now, P->b, P->len, P->src);
                } else {
                    rbn_pre_rx(R, N->now, P->b, P->len, P->src);
                }
            }
            P->len = 0;
            P->kind = 0;
        }
        if (N->now % 200u == 0) {
            for (i = 0; i < N->n; i++) {
                uint8_t b[RBN_PRE_MAX > RBN_PKT_MAX ? RBN_PRE_MAX : RBN_PKT_MAX];
                int len, pl;
                uint8_t pb[RBN_PRE_MAX];
                if (!N->on[i]) continue;
                len = rbn_tx(N->r[i], N->now, N->self[i], b, (int)sizeof b);
                rbn_pre_tick(N->r[i], N->now);
                pl = rbn_pre_tx(N->r[i], N->now, pb, (int)sizeof pb);
                for (j = 0; j < N->n; j++) {
                    if (j == i || !N->on[j]) continue;
                    if (len > 0) pq_put(N, j, 0, 0, psrc(i), b, len);
                    if (pl > 0) pq_put(N, j, 0, 0, psrc(i), pb, pl);
                }
            }
        }
        if (N->now % 50u == 0) { /* Rahmen der Eingabeschicht (Host in TENT sendet keine: keine Entnahme, kein p0) */
            for (i = 0; i < N->n; i++) {
                int st = rbn_state(N->r[i]);
                if (!N->on[i] || !N->started[i] || (st != RBN_ST_R2 && st != RBN_ST_PRE)) continue;
                for (j = 0; j < N->n; j++)
                    if (j != i && N->on[j]) pq_put(N, j, 1, rbn_epoch(N->r[i]), psrc(i), NULL, 0);
            }
        }
        for (i = 0; i < N->n; i++) {
            if (N->started[i]) (void)rbn_poll(N->r[i], N->now);
            pn_note(N, i);
        }
    }
}

static int pn_start(pnet_t *N, int i, int npeers)
{
    int st;
    N->self[i] = i;
    N->started[i] = 1;
    if (i == 0) {
        u32 ips[PN];
        int k, c = 0;
        for (k = 1; k < N->n; k++)
            if (N->on[k]) ips[c++] = pip(k);
        rbn_pre_set_conn(N->r[0], c, ips);
    }
    st = rbn_mission_start(N->r[i], N->now, i, npeers);
    pn_note(N, i);
    return st;
}

static void pn_end(pnet_t *N)
{
    int i;
    for (i = 0; i < N->n; i++) {
        rbn_mission_end(N->r[i]);
        N->started[i] = 0;
        N->r2_ep[i] = 0;
        N->r2_d[i] = 0;
        N->ep_changed[i] = 0;
    }
}

/* Ende einer Mission: einig? (alle R2 gleiche Epoche/D oder alle LS), nie Epochenwechsel nach R2 */
static int pn_consistent(pnet_t *N, char *why, int cap)
{
    int i, r2 = 0, ls = 0;
    for (i = 0; i < N->n; i++) {
        int st;
        if (!N->on[i] || !N->started[i]) continue;
        st = rbn_state(N->r[i]);
        if (N->ep_changed[i]) {
            snprintf(why, (size_t)cap, "Knoten %d wechselte nach R2 die Epoche", i);
            return 0;
        }
        if (st == RBN_ST_R2) r2++;
        else if (st == RBN_ST_LS) ls++;
        else {
            snprintf(why, (size_t)cap, "Knoten %d noch unentschieden (Zustand %d)", i, st);
            return 0;
        }
        if (st == RBN_ST_R2 && (rbn_epoch(N->r[i]) != rbn_epoch(N->r[0]) || rbn_delay(N->r[i]) != rbn_delay(N->r[0]))) {
            snprintf(why, (size_t)cap, "Knoten %d R2 Epoche %08X D=%d, Host %08X D=%d", i, rbn_epoch(N->r[i]),
                     rbn_delay(N->r[i]), rbn_epoch(N->r[0]), rbn_delay(N->r[0]));
            return 0;
        }
    }
    if (r2 && ls) {
        snprintf(why, (size_t)cap, "gemischt: %d R2, %d Lockstep", r2, ls);
        return 0;
    }
    why[0] = 0;
    return 1;
}

/* 1. gueltig: Host Fall a (sofort R2), Client PRE -> Beweis per Rahmen; beide gleiche Epoche/D */
static void pre_valid(u32 owd, int expect_d)
{
    pnet_t N;
    int sa, sb;
    char why[160];
    pn_init(&N, 2, owd);
    pn_step(&N, 4000);
    PCHECK(rbn_pre_ready(N.r[0], N.now), "owd %u: Vorschlag bestaetigt (Host bereit)", owd);
    PCHECK(rbn_pre_bound(N.r[1], NULL), "owd %u: Client gebunden", owd);
    sa = pn_start(&N, 0, 1);
    PCHECK(sa == RBN_ST_R2 && rbn_pre_used(N.r[0]) && rbn_pre_miss(N.r[0]) == 0, "owd %u: Host sofort R2 (st %d miss %d)", owd,
           sa, rbn_pre_miss(N.r[0]));
    pn_step(&N, 100); /* Client laedt etwas laenger */
    sb = pn_start(&N, 1, 1);
    PCHECK(sb == RBN_ST_PRE && rbn_epoch(N.r[1]) == rbn_epoch(N.r[0]) && rbn_delay(N.r[1]) == rbn_delay(N.r[0]),
           "owd %u: Client PRE mit Epoche/D des Hosts (st %d)", owd, sb);
    PCHECK(rbn_delay(N.r[0]) == expect_d, "owd %u: D=%d erwartet %d", owd, rbn_delay(N.r[0]), expect_d);
    pn_step(&N, owd + 80);
    PCHECK(rbn_state(N.r[1]) == RBN_ST_R2, "owd %u: Client nach Rahmen des Hosts R2 (st %d)", owd, rbn_state(N.r[1]));
    PCHECK(rbn_pre_stats(N.r[1])->pre_r2_frame + rbn_pre_stats(N.r[1])->pre_r2_hello == 1 && !rbn_pre_stats(N.r[1])->pre_diverge,
           "owd %u: Beweis per Rahmen bzw. Hallo, keine Abweichung", owd);
    pn_step(&N, 3000);
    PCHECK(pn_consistent(&N, why, sizeof why), "owd %u: einig (%s)", owd, why);
    PCHECK(!rbn_peer_disagrees(N.r[0]) && !rbn_peer_disagrees(N.r[1]), "owd %u: keine Uneinigkeit", owd);
    /* zweite Mission: wieder Vorab-Einigung (neue Folge, alte nicht wiederverwendbar) */
    {
        u32 ep1 = rbn_epoch(N.r[0]);
        pn_step(&N, 20000);
        pn_end(&N);
        pn_step(&N, 3000);
        PCHECK(rbn_pre_ready(N.r[0], N.now) && rbn_pre_bound(N.r[1], NULL), "owd %u: Mission 2 wieder vereinbart", owd);
        sb = pn_start(&N, 1, 1); /* Client zuerst */
        pn_step(&N, 200);
        PCHECK(rbn_state(N.r[1]) == RBN_ST_PRE, "owd %u: Mission 2 Client wartet in PRE (st %d)", owd, rbn_state(N.r[1]));
        sa = pn_start(&N, 0, 1);
        pn_step(&N, owd + 100);
        PCHECK(sa == RBN_ST_R2 && rbn_state(N.r[1]) == RBN_ST_R2 && rbn_epoch(N.r[0]) != ep1, "owd %u: Mission 2 R2, neue Epoche",
               owd);
        pn_step(&N, 2000);
        PCHECK(pn_consistent(&N, why, sizeof why), "owd %u: Mission 2 einig (%s)", owd, why);
    }
    pn_free(&N);
}

/* 1b. Beweis nur per Hallo (Rahmen gehen verloren): Client folgt der Hallo-Entscheidung c */
static void pre_valid_hello(void)
{
    pnet_t N;
    char why[160];
    int k;
    pn_init(&N, 2, 30);
    pn_step(&N, 4000);
    pn_start(&N, 0, 1);
    pn_start(&N, 1, 1);
    for (k = 0; k < 20; k++) { /* Rahmen verwerfen: nur Hallo/SBRP zustellen */
        int q;
        for (q = 0; q < PQ; q++)
            if (N.q[q].kind) N.q[q].len = N.q[q].kind = 0;
        pn_step(&N, 10);
    }
    pn_step(&N, 400);
    PCHECK(rbn_state(N.r[1]) == RBN_ST_R2 && rbn_pre_stats(N.r[1])->pre_r2_hello >= 1 ||
               rbn_pre_stats(N.r[1])->pre_r2_frame >= 1,
           "Hallo-Beweis: Client R2 (st %d)", rbn_state(N.r[1]));
    PCHECK(pn_consistent(&N, why, sizeof why), "Hallo-Beweis: einig (%s)", why);
    pn_free(&N);
}

/* 2. Spieler tritt kurz vor dem Start bei: Host nutzt die Einigung nicht (bisheriger Weg), gebundener Client folgt */
static void pre_join_late(void)
{
    pnet_t N;
    char why[160];
    int sa, sb, sc;
    pn_init(&N, 3, 30);
    N.on[2] = 0;
    pn_step(&N, 4000);
    PCHECK(rbn_pre_bound(N.r[1], NULL), "Beitritt: Client 1 gebunden");
    N.on[2] = 1; /* Beitritt */
    pn_step(&N, 400); /* Host sieht das Hallo (neuer Vorschlag), Bestaetigungen fehlen noch */
    sa = pn_start(&N, 0, 2);
    PCHECK(sa == RBN_ST_TENT && !rbn_pre_used(N.r[0]), "Beitritt: Host bisheriger Weg (st %d miss %d)", sa, rbn_pre_miss(N.r[0]));
    sb = pn_start(&N, 1, 2);
    sc = pn_start(&N, 2, 2);
    pn_step(&N, 3000);
    PCHECK(pn_consistent(&N, why, sizeof why), "Beitritt: einig (%s), Starts %d/%d/%d", why, sa, sb, sc);
    PCHECK(rbn_state(N.r[0]) == RBN_ST_R2, "Beitritt: R2 fuer alle (st %d/%d/%d)", rbn_state(N.r[0]), rbn_state(N.r[1]),
           rbn_state(N.r[2]));
    pn_free(&N);
}

/* 3. Verzoegerung aendert sich (RTT steigt): neuer Vorschlag nach der Hysterese; Starts ueber das ganze Fenster */
static void pre_delay_change(void)
{
    int off, bad = 0, used = 0, n = 0;
    char why[160], first[200] = "";
    for (off = 0; off <= 2400; off += 40) {
        pnet_t N;
        pn_init(&N, 2, 10);
        pn_step(&N, 4000);
        N.owd = 120; /* RTT ~240 ms -> D 3 statt 2 */
        pn_step(&N, (u32)off);
        pn_start(&N, 0, 1);
        if (rbn_pre_used(N.r[0])) used++;
        pn_step(&N, 30);
        pn_start(&N, 1, 1);
        pn_step(&N, 3000);
        n++;
        if (!pn_consistent(&N, why, sizeof why) || rbn_state(N.r[0]) != RBN_ST_R2) {
            if (!bad++) snprintf(first, sizeof first, "Versatz %d: %s (st %d/%d)", off, why, rbn_state(N.r[0]), rbn_state(N.r[1]));
        }
        pn_free(&N);
    }
    PCHECK(bad == 0, "Verzoegerungswechsel: %d von %d Starts uneinig - %s", bad, n, first);
    PCHECK(used > 0 && used < n, "Verzoegerungswechsel: Einigung teils genutzt (%d von %d)", used, n);
}

/* 4. Replay: alter Vorschlag (vorige Mission bzw. vorheriger Host-Prozess) wird nicht angenommen */
static void pre_replay(void)
{
    pnet_t N;
    uint8_t old[RBN_PRE_MAX];
    int ol, r;
    u32 seq0 = 0, seq1 = 0;
    pn_init(&N, 2, 20);
    pn_step(&N, 4000);
    ol = rbn_pre_tx(N.r[0], N.now, old, (int)sizeof old);
    PCHECK(ol > RBN_PRE_HEAD && old[5] == RBN_PRE_PROP && rbn_pre_bound(N.r[1], &seq0), "Replay: Vorschlag mitgeschnitten");
    pn_start(&N, 0, 1);
    pn_start(&N, 1, 1);
    pn_step(&N, 2000);
    pn_end(&N);
    pn_step(&N, 3000);
    PCHECK(rbn_pre_bound(N.r[1], &seq1) && seq1 > seq0, "Replay: neue Bindung mit hoeherer Folge (%u > %u)", seq1, seq0);
    r = rbn_pre_rx(N.r[1], N.now, old, ol, psrc(0));
    PCHECK(r == 0 && rbn_pre_bound(N.r[1], &seq1) && seq1 > seq0, "Replay: alte Folge ignoriert (r %d, Bindung %u)", r, seq1);
    /* nach dem Missionsbeginn (Bindung verbraucht): alter Vorschlag ergibt keine neue Bindung */
    pn_start(&N, 0, 1);
    pn_start(&N, 1, 1);
    pn_step(&N, 500);
    pn_end(&N);
    N.on[0] = 0; /* Host weg: nur noch das Replay */
    r = rbn_pre_rx(N.r[1], N.now, old, ol, psrc(0));
    PCHECK(r == 0 && !rbn_pre_bound(N.r[1], NULL), "Replay: nach der Mission keine Bindung (r %d)", r);
    /* Host-Prozess neu (andere Kennung): altes Paket passt nicht zur Hallo-Bindung */
    rbn_free(N.r[0]);
    N.r[0] = rbn_new(1, 0x5A5A0000u + 0x99u, 2, 3, 0);
    rbn_set_lobby(N.r[0], RBN_ROLE_HOST, 0, 0xC0FFEEu);
    N.on[0] = 1;
    pn_step(&N, 1000);
    r = rbn_pre_rx(N.r[1], N.now, old, ol, psrc(0));
    PCHECK(r == 0, "Replay: Vorschlag eines frueheren Host-Prozesses ignoriert (r %d)", r);
    pn_free(&N);
}

/* 5. falsche Mission / falscher Absender / falsche Parameter */
static void pre_wrong(void)
{
    pnet_t N;
    uint8_t b[RBN_PRE_MAX], ack[RBN_PRE_MAX];
    int l, r, al;
    pn_init(&N, 3, 20);
    pn_step(&N, 4000);
    l = rbn_pre_tx(N.r[0], N.now, b, (int)sizeof b);
    /* Vorschlag von einem anderen Absender (Client 2 als "Host") */
    r = rbn_pre_rx(N.r[1], N.now, b, l, psrc(2));
    PCHECK(r == 0, "falscher Absender: ignoriert (r %d)", r);
    /* Parameter veraendert, Pruefsumme alt -> ungueltig */
    b[25] = (uint8_t)(b[25] == 3 ? 2 : 3);
    r = rbn_pre_rx(N.r[1], N.now, b, l, psrc(0));
    PCHECK(r == -1, "veraenderte Verzoegerung: verworfen (r %d)", r);
    b[25] = (uint8_t)(b[25] == 3 ? 2 : 3);
    /* Mission laeuft beim Client: Vorschlag fuer "eine andere Mission" wird nicht angenommen */
    {
        pnet_t M;
        uint8_t mb[RBN_PRE_MAX];
        int ml;
        pn_init(&M, 2, 20);
        pn_step(&M, 4000);
        pn_start(&M, 0, 1);
        pn_start(&M, 1, 1);
        pn_step(&M, 500);
        pn_end(&M);
        pn_step(&M, 3000);
        ml = rbn_pre_tx(M.r[0], M.now, mb, (int)sizeof mb);
        pn_start(&M, 1, 1); /* Client in der naechsten Mission (Bindung verbraucht) */
        r = rbn_pre_rx(M.r[1], M.now, mb, ml, psrc(0));
        PCHECK(r == 0 && !rbn_pre_bound(M.r[1], NULL), "in der Mission: Vorschlag ignoriert (r %d)", r);
        pn_free(&M);
    }
    /* Bestaetigung mit falscher Pruefsumme bzw. von einem Nicht-Teilnehmer */
    al = rbn_pre_tx(N.r[1], N.now, ack, (int)sizeof ack);
    PCHECK(al == RBN_PRE_HEAD && ack[5] == RBN_PRE_ACK, "Bestaetigung gebaut");
    ack[20] ^= 1;
    r = rbn_pre_rx(N.r[0], N.now, ack, al, psrc(1));
    PCHECK(r == 0, "Bestaetigung mit falscher Pruefsumme ignoriert (r %d)", r);
    ack[20] ^= 1;
    r = rbn_pre_rx(N.r[0], N.now, ack, al, psrc(2));
    PCHECK(r == 0, "Bestaetigung von fremder Adresse ignoriert (r %d)", r);
    /* Vorschlag ohne eigene Kennung: keine Bindung */
    {
        pnet_t M;
        pn_init(&M, 2, 20);
        rbn_set_want(M.r[1], 1, 0);
        pn_step(&M, 4000);
        PCHECK(rbn_pre_bound(M.r[1], NULL), "Teilnehmer gebunden");
        pn_free(&M);
    }
    pn_free(&N);
}

/* 6. Client schaltet Rollback kurz vor dem Start aus: Bindung gilt bis zur Ruecknahme; beide immer einig */
static void pre_want_off(void)
{
    int off, bad = 0, n = 0, ls_end = 0;
    char why[160], first[200] = "";
    for (off = 0; off <= 1600; off += 40) {
        pnet_t N;
        pn_init(&N, 2, 25);
        pn_step(&N, 4000);
        rbn_set_want(N.r[1], 0, 0);
        pn_step(&N, (u32)off);
        pn_start(&N, 0, 1);
        pn_step(&N, 20);
        pn_start(&N, 1, 1);
        pn_step(&N, 3000);
        n++;
        if (rbn_state(N.r[0]) == RBN_ST_LS) ls_end++;
        if (!pn_consistent(&N, why, sizeof why)) {
            if (!bad++) snprintf(first, sizeof first, "Versatz %d: %s", off, why);
        }
        pn_free(&N);
    }
    PCHECK(bad == 0, "Wunsch aus: %d von %d Starts uneinig - %s", bad, n, first);
    PCHECK(ls_end > 0, "Wunsch aus: spaete Starts im Lockstep (%d von %d)", ls_end, n);
}

/* 7. Zufall: Verlust, Verzoegerung, Beitritte, Wunsch-/Kontextwechsel, versetzte Starts - nie uneinig */
static void pre_random(int runs)
{
    int k, bad = 0, used = 0;
    char why[160], first[240] = "";
    for (k = 0; k < runs; k++) {
        pnet_t N;
        int n = 2 + (int)(prnd() % 3u), i, ev = (int)(prnd() % 6u);
        u32 t0 = 2000 + prnd() % 3000u;
        pn_init(&N, n, 5 + prnd() % 150u);
        N.loss = prnd() % 4u == 0 ? prnd() % 30u : 0;
        if (ev == 0) N.on[n - 1] = 0;
        pn_step(&N, t0);
        switch (ev) {
        case 0: N.on[n - 1] = 1; break;                                 /* Beitritt */
        case 1: rbn_set_want(N.r[1], 0, 0); break;                      /* Client: Lockstep */
        case 2: rbn_set_want(N.r[0], 1, 1); break;                      /* Host: Auto */
        case 3: rbn_set_lobby(N.r[0], RBN_ROLE_HOST, 0, prnd()); break; /* Seitenwechsel/Einstellung */
        case 4: N.owd = 5 + prnd() % 200u; break;                       /* RTT */
        default: break;
        }
        pn_step(&N, prnd() % 1500u);
        for (i = 0; i < n; i++) { /* versetzte Starts, Reihenfolge zufaellig */
            int who = (int)((prnd() + (u32)i) % (u32)n), c;
            for (c = 0; c < n && N.started[who]; c++) who = (who + 1) % n;
            if (N.on[who] && !N.started[who]) pn_start(&N, who, n - 1);
            pn_step(&N, prnd() % 300u);
        }
        for (i = 0; i < n; i++)
            if (N.on[i] && !N.started[i]) pn_start(&N, i, n - 1);
        if (rbn_pre_used(N.r[0])) used++;
        pn_step(&N, 14000);
        if (!pn_consistent(&N, why, sizeof why)) {
            if (!bad++) snprintf(first, sizeof first, "Lauf %d (n %d ev %d owd %u loss %u): %s", k, n, ev, N.owd, N.loss, why);
        }
        pn_free(&N);
    }
    PCHECK(bad == 0, "Zufall: %d von %d Laeufen uneinig - %s", bad, runs, first);
    PCHECK(used > runs / 4, "Zufall: Vorab-Einigung genutzt in %d von %d", used, runs);
    printf("W475 Zufall: %d Laeufe, Vorab-Einigung genutzt %d, uneinig %d\n", runs, used, bad);
}

/* 8. rbi_prime/rbi_rebind */
static void rbi_rebind_cases(void)
{
    rbi_t *r = rbi_new();
    uint8_t hdr[RBI_HDR] = {1, 0x10, 0, 0, 0, 0, 0, 0}, in[RBI_IN] = {0}, out[RBI_FR], pkt[RBI_PKT_MAX];
    int k, len;
    rbi_begin(r, 0xAAAA0001u, 1, 2, 10);
    for (k = 0; k < 4; k++) {
        in[0] = (uint8_t)k;
        rbi_own(r, hdr, in);
    }
    PCHECK(rbi_prime(r) && rbi_p0(r) == 4 && rbi_delay_eff(r) == 2, "rbi_prime: p0 %d deff %d", rbi_p0(r), rbi_delay_eff(r));
    len = rbi_tx(r, pkt, (int)sizeof pkt);
    PCHECK(len > RBI_PKT_HEAD, "rbi_prime: eigene Rahmen gesendet (%d)", len);
    PCHECK(!rbi_configure(r, 0xBBBB0002u, 3), "rbi_configure nach rbi_prime abgelehnt");
    PCHECK(rbi_rebind(r, 0xBBBB0002u, 3) && rbi_epoch(r) == 0xBBBB0002u && rbi_p0(r) < 0, "rbi_rebind vor der Entnahme");
    in[0] = 9;
    rbi_own(r, hdr, in);
    PCHECK(rbi_prime(r) && rbi_p0(r) == 5 && rbi_delay_eff(r) == 3, "rbi_rebind: eigene Rahmen behalten (p0 %d deff %d)",
           rbi_p0(r), rbi_delay_eff(r));
    PCHECK(rbi_take(r, 1, 0, out) == 1 && out[8] == 2, "rbi_rebind: Slot 0 = drittletzter Vorlauf-Rahmen (%u)", out[8]);
    PCHECK(!rbi_rebind(r, 0xCCCC0003u, 2), "rbi_rebind nach einer Entnahme abgelehnt");
    rbi_free(r);
}

int rbn_pre_selftest(int *ok)
{
    int k;
    s_ok = ok;
    s_fail = 0;
    pre_valid(15, 2);
    pre_valid(40, 2);
    pre_valid(90, 3);
    pre_valid_hello();
    pre_join_late();
    pre_delay_change();
    pre_replay();
    pre_wrong();
    pre_want_off();
    pre_random(400);
    rbi_rebind_cases();
    /* Formatpruefung: zufaellige Bytes nie angenommen, gueltige Vektoren selbst gebaut */
    for (k = 0; k < 20000; k++) {
        uint8_t b[RBN_PRE_MAX];
        int len = (int)(prnd() % (RBN_PRE_MAX + 1u)), i;
        for (i = 0; i < len; i++) b[i] = (uint8_t)prnd();
        if (len >= 4) memcpy(b, "SBRP", 4);
        if (len >= 5) b[4] = 1;
        if (rbn_pre_check(b, len) && len >= 6 && b[5] == RBN_PRE_PROP) {
            s_fail++;
            printf("FAIL Zufallspaket als Vorschlag gueltig (Pruefsumme)\n");
            break;
        }
    }
    printf("W475 rbn_pre_selftest: %s\n", s_fail ? "FEHLER" : "ok");
    return s_fail;
}
