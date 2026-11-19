/* loc_rbi_selftest.c - W338 LOC-ROLLBACK-R2: Selbsttest der Eingabeschicht (src/loc_rbi.c).
 *
 * Synthetisch, ohne Spieldaten: Ein "Spielzwilling" bildet die Lockstep-Mechanik von LoC nach (Entnahme je
 * Spieler mit Stau, Anwenden erst wenn alle da sind, Akkumulator beim Rahmenbau, eigener Rahmen mit Sync-Byte,
 * Vorlauf P0) und haelt seinen Zustand wie der R1-Ring je Commit. Zwei Instanzen tauschen Pakete ueber eine
 * simulierte Strecke (Verzoegerung in Ticks, Schwankung, Verlust, unterschiedliche Taktraten). Geprueft:
 *  1. Synchron: alle endgueltigen Hashes beider Seiten gleich (eigene Ketten und Austausch), kein Desync.
 *  2. Vorhersage/Rollback: bei Verzoegerung > D gibt es Fehlvorhersagen und Rollbacks, keiner geht verloren.
 *  3. Bei 0 Verzoegerung und D >= 1 keine Vorhersage, kein Rollback (wie Lockstep, nur ohne Warten).
 *  4. Kanal: kaputte/fremde Pakete werden verworfen (Laenge, Kennung, n != 1, falscher Absender).
 *  5. Das Spiel sieht nie ein abweichendes Sync-Byte.
 *  6. W346: Aushandlung (loc_rbn) inkl. Rueckfall/Frist/Verlust, Fuzz beider Pakete (SBR2-Grenzfaelle, SBRH).
 *  7. W359: Render-Auftrag beim Neurechnen (Spielmodell mit Freigabe-Warteschlange, Gegenprobe alte Regel) und
 *     Plausibilitaetspruefung der Heap-Freilisten (beschaedigte Listen, Fuzz).
 * Exit 0 = alles gruen. */
#include "loc_rbi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t u32;

static int g_fail, g_ok;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (c) g_ok++;                                                                                                 \
        else {                                                                                                         \
            g_fail++;                                                                                                  \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

static u32 s_rng = 12345;
static u32 rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

/* ---- Spielzwilling ---- */
#define NP 2
#define RING 16
#define MAXS 20000
typedef struct {
    u32 pos[NP], vel[NP], tick, have; /* have = Bits der in dieser Runde entnommenen Spieler ([0x306804]) */
    uint8_t buf[NP][RBI_FR];
    u32 acc;
    u32 blk[4], alive; /* W355: Bloecke, die das Spiel im Kampf neu belegt (Allokation), Bit k = Block k belegt */
} toy_t;

typedef struct {
    int me;
    rbi_t *r;
    toy_t st;
    toy_t snap[RING];   /* Stand nach Commit c (Index c % RING) */
    u32 depth;          /* ladbare Commits */
    u32 prod;           /* erzeugte eigene Rahmen (nur fuer die Eingabe-Folge) */
    u32 fin_acc[MAXS];  /* endgueltige Hashes je Slot */
    uint8_t fin_has[MAXS];
    u32 nfin;
    u32 rb_ticks_max;
    int sync_bad;
    int p0;
    int streak;
    u32 ever, reg_gen, reg_seen; /* W355: Allokationsregister (waechst nur bei echter Neubelegung, nie beim Neurechnen) */
    u32 inring;                  /* W355: Bloecke, die der Ring sichert (erst nach dem Neuaufbau) */
    u32 snap_inring[RING];
    u32 defer;                   /* W355: Gegenprobe W346-Regel */
    u32 reg_note;                /* W355: Registerstand beim letzten Ruhepunkt (rbi_rebuild_note) */
    u32 forced_at;               /* W355: Zeitschritt + 1 des letzten erzwungenen Rollbacks */
    u32 rebuilds;
} inst_t;

static int g_expect_bad;    /* W355: Gegenprobe - Fehler wird erwartet (nur zaehlen, nicht pruefen) */
static int g_alloc;         /* W355: Neubelegungen an (je Strecke) */
static int g_regrow;
static int g_inalloc;       /* W355: Belegung haengt von der Eingabe von B ab (erst die korrigierte Zeitlinie belegt) */
static int g_nozero;        /* W355: Gegenprobe - erneute Belegung beim Neurechnen nullt nicht (W346: Commit-Weg) */        /* W355: Neubelegung beim Neurechnen laesst das Register erneut wachsen (Nutzerlauf: 526 -> 527) */
static u32 g_chg = 7;       /* W355: eigene Eingabe aendert sich alle g_chg Rahmen */
static const u32 k_alloc_at[4] = {500, 1500, 2600, 4100}; /* W355: Spieltick der Neubelegung (beide Seiten gleich) */

static u32 toy_hash(const toy_t *s)
{
    u32 h = 2166136261u, i;
    for (i = 0; i < NP; i++) {
        h = (h ^ s->pos[i]) * 16777619u;
        h = (h ^ s->vel[i]) * 16777619u;
    }
    for (i = 0; i < 4; i++)
        if (s->alive & (1u << i)) h = (h ^ s->blk[i] ^ (i << 24)) * 16777619u;
    return (h ^ s->tick) * 16777619u;
}

static void own_frame(inst_t *X, int prefill)
{
    uint8_t hdr[RBI_HDR] = {1, 0, 0, 0, 0x03, 0, 0, 0}, in[RBI_IN];
    u32 v = prefill ? 0u : ((X->prod / g_chg) * 2654435761u) ^ (u32)(X->me * 977u); /* aendert sich alle g_chg Rahmen */
    int k;
    hdr[1] = (uint8_t)X->st.acc;
    for (k = 0; k < RBI_IN; k++) in[k] = (uint8_t)(v >> ((k & 3) * 8));
    X->prod++;
    rbi_own(X->r, hdr, in);
}

/* ein Spieltick (bis zum Ruhepunkt) */
static void toy_tick(inst_t *X)
{
    u32 cmin = rbi_commits(X->r) > X->depth ? rbi_commits(X->r) - X->depth : 0;
    int i;
    for (i = 0; i < NP; i++) {
        if (X->st.have & (1u << i)) continue;
        if (rbi_take(X->r, i, cmin, X->st.buf[i])) X->st.have |= 1u << i;
    }
    if (X->st.have != (1u << NP) - 1u) { /* Stau */
#ifdef RBI_DEBUG
        if (++X->streak == 60) {
            void rbi_debug_dump(const rbi_t *r, u32 cmin);
            printf("STAU-SERIE inst %d have=%u depth=%u\n", X->me, X->st.have, X->depth);
            rbi_debug_dump(X->r, cmin);
        }
#endif
        return;
    }
    X->streak = 0;
    for (i = 0; i < NP; i++) {
        if (X->st.buf[i][1] != X->st.buf[X->me][1]) X->sync_bad++;
    }
    for (i = 0; i < NP; i++) { /* Anwenden (deterministisch aus den Eingaben) */
        u32 in = (u32)X->st.buf[i][RBI_HDR] | ((u32)X->st.buf[i][RBI_HDR + 1] << 8);
        X->st.vel[i] = X->st.vel[i] * 3u + in;
        X->st.pos[i] += X->st.vel[i] ^ X->st.tick;
    }
    X->st.have = 0;
    X->st.tick++;
    { /* W355: Neubelegung im Kampf (Neurechnung: dieselbe Belegung aus der Aufzeichnung, genullt, Register waechst nicht) */
        u32 k;
        for (k = 0; k < 4; k++) {
            int due = g_inalloc ? (X->st.tick >= k_alloc_at[k] && (X->st.buf[1][RBI_HDR] & 3u) == 1u)
                                : X->st.tick == k_alloc_at[k];
            if (g_alloc && due && !(X->st.alive & (1u << k))) {
                X->st.alive |= 1u << k;
                if (!(g_nozero && (X->ever & (1u << k)))) X->st.blk[k] = 0;
                if (!(X->ever & (1u << k))) {
                    X->ever |= 1u << k;
                    X->reg_gen++;
                } else if (g_regrow) {
                    X->reg_gen++;
                }
            }
            if (X->st.alive & (1u << k)) X->st.blk[k] = X->st.blk[k] * 7u + X->st.vel[0] + X->st.vel[1] + k;
        }
    }
    X->st.acc = toy_hash(&X->st);
    rbi_acc(X->r, X->st.acc, X->st.tick);
    own_frame(X, 0);
}

typedef struct {
    int used, to;
    u32 at;
    int len;
    uint8_t b[RBI_PKT_MAX];
} pkt_t;
#define QN 4096
static pkt_t s_q[QN];

typedef struct {
    int delay_ab, delay_ba, jitter, loss_pct, skip_b; /* skip_b: B laesst jeden N-ten Tick aus (langsamer) */
    int D, maxpred;
    u32 ticks;
    int alloc;   /* W355: Neubelegungen im Kampf (Ring-Neuaufbau nur ueber rbi_rebuild_gate) */
    int oldgate; /* W355: Gegenprobe mit der W346-Regel (Neuaufbau mitten in der Neurechnung) */
    int force_n, force_k; /* W355: alle force_n Ticks Rollback um force_k Ticks erzwingen (rbi_force_rb) */
} scen_t;

static void q_put(int to, u32 at, const uint8_t *b, int len)
{
    int k;
    for (k = 0; k < QN; k++) {
        if (!s_q[k].used) {
            s_q[k].used = 1;
            s_q[k].to = to;
            s_q[k].at = at;
            s_q[k].len = len;
            memcpy(s_q[k].b, b, (size_t)len);
            return;
        }
    }
}

/* Ruhepunkt: Commit, Empfang, Rollback planen, senden, endgueltige Hashes */
static void toy_rest(inst_t *X, u32 now, const scen_t *sc, inst_t *peer, int delay)
{
    int done = 0, k, com, rebuild = 0;
    u32 slot, acc, gt;
    com = rbi_tick_end(X->r, &done);
    if (sc->alloc) { /* W355: Ring-Neuaufbau nach einer Neubelegung (wie rbi_after) */
        int need = X->reg_gen != X->reg_seen;
        if (sc->oldgate) { /* W346: Aufschub nur ausserhalb der Neurechnung - sonst sofort neu */
            if (need && !rbi_replaying(X->r) && rbi_open_preds(X->r) && X->defer < 200u) {
                X->defer++;
                rbi_hold(X->r, 1);
                need = 0;
            } else if (need) {
                X->defer = 0;
                rbi_hold(X->r, 0);
            }
            rebuild = need;
        } else {
            if (X->reg_gen != X->reg_note) { /* W355: Belegung in diesem Tick -> Untergrenze */
                X->reg_note = X->reg_gen;
                rbi_rebuild_note(X->r);
            }
            rebuild = rbi_rebuild_gate(X->r, need, 1, 1, 200u) == RBI_REBUILD_NOW;
        }
    }
    if (rebuild) { /* neue Bereiche, Grundstand = jetzt, Barriere */
        u32 c = rbi_commits(X->r);
        X->reg_seen = X->reg_gen;
        X->inring = X->ever;
        X->snap[c % RING] = X->st;
        X->snap_inring[c % RING] = X->inring;
        X->depth = 0;
        X->rebuilds++;
        rbi_barrier(X->r);
    } else if (com) { /* Commit */
        u32 c = rbi_commits(X->r);
        X->snap[c % RING] = X->st;
        X->snap_inring[c % RING] = X->inring;
        if (X->depth < RING - 1) X->depth++;
    }
    if (rbi_replaying(X->r)) return;
    for (k = 0; k < QN; k++) {
        if (s_q[k].used && s_q[k].to == X->me && s_q[k].at <= now) {
            CHECK(rbi_rx(X->r, s_q[k].b, s_q[k].len, (uint64_t)(100 + peer->me)) == 1, "rx abgelehnt");
            s_q[k].used = 0;
        }
    }
    if (sc->force_n && (now % (u32)sc->force_n) == (u32)X->me && X->forced_at != now + 1u) {
        X->forced_at = now + 1u; /* W355: gleiche Eingaben -> Neurechnung bitgleich (einmal je Zeitschritt) */
        (void)rbi_force_rb(X->r, (u32)sc->force_k, X->depth);
    }
    {
        u32 ticks = 0, sb = 0;
        int kk = rbi_plan(X->r, X->depth, &ticks, &sb);
        CHECK(kk >= 0 || g_expect_bad, "Rollback nicht mehr moeglich (inst %d)", X->me);
        if (kk > 0) {
            toy_t cur = X->st;
            u32 ci = rbi_commits(X->r) % RING, b;
            X->st = X->snap[ci];
            for (b = 0; b < 4; b++) /* W355: Bloecke ausserhalb des Rings behalten ihren aktuellen Inhalt */
                if (!(X->snap_inring[ci] & (1u << b))) X->st.blk[b] = cur.blk[b];
            X->depth -= (u32)kk;
            if (ticks > X->rb_ticks_max) X->rb_ticks_max = ticks;
        }
    }
    {
        uint8_t b[RBI_PKT_MAX];
        int n = rbi_tx(X->r, b, (int)sizeof b);
        if (n > 0 && (int)(rnd() % 100) >= sc->loss_pct) {
            u32 j = sc->jitter ? rnd() % (u32)(sc->jitter + 1) : 0;
            q_put(peer->me, now + (u32)delay + j, b, n);
        }
    }
    while (rbi_final_next(X->r, &slot, &acc, &gt)) {
        if (slot < MAXS) {
            X->fin_acc[slot] = acc;
            X->fin_has[slot] = 1;
            X->nfin++;
        }
    }
}

static void inst_init(inst_t *X, int me, const scen_t *sc)
{
    int k;
    memset(X, 0, sizeof *X);
    X->me = me;
    X->r = rbi_new();
    rbi_begin(X->r, 7, me, sc->D, sc->maxpred);
    X->p0 = 4;
    for (k = 0; k < X->p0; k++) own_frame(X, 1); /* Vorlauf wie FUN_0001D3D0 */
    X->snap[0] = X->st;
}

static u32 g_last_bad;
static void scenario(const char *name, const scen_t *sc, int expect_rb)
{
    static inst_t A, B;
    u32 t, s, cmp = 0, diff = 0;
    const rbi_stats_t *sa, *sb;
    memset(s_q, 0, sizeof s_q);
    g_alloc = sc->alloc;
    inst_init(&A, 0, sc);
    inst_init(&B, 1, sc);
    for (t = 0; t < sc->ticks; t++) {
        do { /* Neurechnung laeuft ohne Feld-Warten: alle Ticks im selben Zeitschritt */
            toy_tick(&A);
            toy_rest(&A, t, sc, &B, sc->delay_ab);
        } while (rbi_replaying(A.r));
        if (!(sc->skip_b && (t % (u32)sc->skip_b) == 0)) {
            do {
                toy_tick(&B);
                toy_rest(&B, t, sc, &A, sc->delay_ba);
            } while (rbi_replaying(B.r));
        }
    }
    for (s = 0; s < MAXS; s++) {
        if (A.fin_has[s] && B.fin_has[s]) {
            cmp++;
            if (A.fin_acc[s] != B.fin_acc[s]) diff++;
        }
    }
    sa = rbi_stats(A.r);
    sb = rbi_stats(B.r);
    printf("%-28s Slots A/B %u/%u, endgueltig %u/%u, verglichen %u, Abw. %u | Vorh. %u/%u falsch %u/%u Rollbacks "
           "%u/%u (max %u Ticks) verloren %u/%u | Hash-Austausch %u/%u schlecht %u/%u | Stau Fenster %u/%u Tiefe %u/%u\n",
           name, rbi_cons(A.r, 0), rbi_cons(B.r, 1), A.nfin, B.nfin, cmp, diff, sa->predicted, sb->predicted,
           sa->pred_bad, sb->pred_bad, sa->rb_plans, sb->rb_plans, A.rb_ticks_max > B.rb_ticks_max ? A.rb_ticks_max : B.rb_ticks_max,
           sa->rb_lost, sb->rb_lost, sa->hash_cmp, sb->hash_cmp, sa->hash_bad, sb->hash_bad, sa->stall_window,
           sb->stall_window, sa->stall_depth, sb->stall_depth);
    if (sc->alloc) {
        printf("%-28s   Neuaufbau A/B %u/%u, aufgeschoben %u/%u (Neurechnung %u/%u, erzwungen %u/%u), Rollback ab Allokation "
               "%u/%u, hingenommen %u/%u\n", "", A.rebuilds, B.rebuilds, sa->rebuild_deferred, sb->rebuild_deferred,
               sa->rebuild_defer_replay, sb->rebuild_defer_replay, sa->rebuild_forced, sb->rebuild_forced, sa->rb_floored,
               sb->rb_floored, sa->rb_accepted, sb->rb_accepted);
        CHECK(A.rebuilds >= 4 && B.rebuilds >= 4, "%s: Neuaufbauten fehlen (%u/%u)", name, A.rebuilds, B.rebuilds);
    }
    if (g_expect_bad) {
        g_last_bad = diff + sa->rb_lost + sb->rb_lost + sa->hash_bad + sb->hash_bad;
        rbi_free(A.r);
        rbi_free(B.r);
        return;
    }
    CHECK(cmp > sc->ticks / 3, "%s: zu wenige endgueltige Hashes verglichen (%u)", name, cmp);
    CHECK(diff == 0, "%s: %u Hashes verschieden", name, diff);
    CHECK(sa->hash_bad == 0 && sb->hash_bad == 0, "%s: Hash-Austausch meldet Desync", name);
    CHECK(sa->hash_cmp > sc->ticks / 4 && sb->hash_cmp > sc->ticks / 4, "%s: Hash-Austausch zu selten", name);
    CHECK(sa->rb_lost == 0 && sb->rb_lost == 0, "%s: verlorene Rollbacks", name);
    CHECK(sa->own_missing == 0 && sb->own_missing == 0, "%s: eigener Rahmen fehlte", name);
    CHECK(sa->hash_recalc_diff == 0 && sb->hash_recalc_diff == 0, "%s: Neurechnung aenderte endgueltigen Hash", name);
    CHECK(sa->replay_stall_mismatch == 0 && sb->replay_stall_mismatch == 0, "%s: Neurechnung wich vom Original ab", name);
    CHECK(A.sync_bad == 0 && B.sync_bad == 0, "%s: Spiel sah abweichendes Sync-Byte", name);
    if (expect_rb > 0) CHECK(sa->rb_plans + sb->rb_plans > 0 && sa->pred_bad + sb->pred_bad > 0, "%s: kein Rollback", name);
    if (expect_rb == 0)
        CHECK(sa->predicted + sb->predicted == 0 && sa->rb_plans + sb->rb_plans == 0, "%s: unerwartete Vorhersage", name);
    rbi_free(A.r);
    rbi_free(B.r);
}

static void channel_tests(void)
{
    rbi_t *a = rbi_new(), *b = rbi_new();
    uint8_t p[RBI_PKT_MAX], q[RBI_PKT_MAX];
    uint8_t hdr[RBI_HDR] = {1, 0x55, 0, 0, 3, 0, 0, 0}, in[RBI_IN] = {0}, out[RBI_FR];
    int n, k, rejected = 0;
    rbi_begin(a, 3, 0, 1, 8);
    rbi_begin(b, 3, 1, 1, 8);
    for (k = 0; k < 4; k++) rbi_own(a, hdr, in);
    CHECK(rbi_take(a, 0, 0, out) == 1, "eigene Entnahme");
    n = rbi_tx(a, p, (int)sizeof p);
    CHECK(n > RBI_PKT_HEAD, "Paket mit Rahmen (%d)", n);
    CHECK(rbi_rx(b, p, n, 1) == 1, "gueltiges Paket angenommen");
    CHECK(rbi_rx(b, p, n, 2) == -1, "fremder Absender fuer denselben Spieler verworfen");
    CHECK(rbi_rx(b, p, n - 1, 1) == -1, "Laenge falsch verworfen");
    memcpy(q, p, (size_t)n);
    q[0] = 'X';
    CHECK(rbi_rx(b, q, n, 1) == -1, "Kennung falsch verworfen");
    memcpy(q, p, (size_t)n);
    q[5] = 1; /* Absender = eigener Index des Empfaengers */
    CHECK(rbi_rx(b, q, n, 1) == -1, "eigener Index verworfen");
    memcpy(q, p, (size_t)n);
    q[RBI_PKT_HEAD + q[6] * 8] = 2; /* n = 2 im ersten Rahmen */
    CHECK(rbi_rx(b, q, n, 1) == -1, "Rahmen mit n=2 verworfen");
    memcpy(q, p, (size_t)n);
    q[7] = 200;
    CHECK(rbi_rx(b, q, n, 1) == -1, "zu viele Rahmen verworfen");
    memcpy(q, p, (size_t)n);
    q[12] ^= 1; /* andere Epoche: ignoriert */
    CHECK(rbi_rx(b, q, n, 1) == 0, "andere Epoche ignoriert");
    for (k = 0; k < 20000; k++) { /* Zufallsverfaelschung: nie Absturz, nie Annahme mit n != 1 */
        int len = (int)(rnd() % (u32)(n + 8)), j;
        memcpy(q, p, (size_t)n);
        for (j = 0; j < 3; j++) q[rnd() % (u32)n] ^= (uint8_t)(1u << (rnd() % 8));
        if (len > n) len = n;
        if (rbi_rx(b, q, len, 1) < 0) rejected++;
    }
    CHECK(rejected > 10000, "Zufallsverfaelschung: zu wenig verworfen (%d)", rejected);
    printf("Kanal: %d von 20000 verfaelschten Paketen verworfen, Statistik schlecht=%u Bindung=%u\n", rejected,
           rbi_stats(b)->rx_bad, rbi_stats(b)->rx_bind);
    rbi_free(a);
    rbi_free(b);
}

/* ---- W346 LOC-ROLLBACK-R4: Aushandlung (src/loc_rbn.c) ----
 * Zwei Aushandlungs-Instanzen (Host A = Spieler 0, Client B = Spieler 1) tauschen Hallo-Pakete ueber eine Strecke
 * mit Einweg-Verzoegerung (ms) und Verlust. Geprueft: beide R2 mit gleicher Epoche/Verzoegerung, Rueckfall auf
 * Lockstep bei fehlendem Wunsch/Uneinigkeit/Frist, keine veraltete Entscheidung, adaptive Verzoegerung, Fuzz. */
#include "loc_rbn.h"

#define NQ 256
typedef struct {
    int to;
    u32 at;
    int len;
    uint8_t b[RBN_PKT_MAX];
} npkt_t;
typedef struct {
    rbn_t *n[2];
    int send[2];      /* sendet Hallo (loc_rb_input=1) */
    u32 owd;          /* Einweg-Verzoegerung ms */
    int loss;         /* Prozent */
    int drop_to[2];   /* alles an diese Seite verwerfen */
    npkt_t q[NQ];
    u32 now;
} nnet_t;

static void nn_step(nnet_t *N, u32 ms)
{
    u32 end = N->now + ms;
    for (; N->now < end; N->now += 10) {
        int k, s;
        for (k = 0; k < NQ; k++) {
            npkt_t *P = &N->q[k];
            if (P->len && (int32_t)(N->now - P->at) >= 0) {
                if (!N->drop_to[P->to]) rbn_rx(N->n[P->to], N->now, P->b, P->len, (uint64_t)(P->to ? 1 : 2));
                P->len = 0;
            }
        }
        if (N->now % 200u == 0) {
            for (s = 0; s < 2; s++) {
                uint8_t b[RBN_PKT_MAX];
                int len;
                if (!N->send[s]) continue;
                len = rbn_tx(N->n[s], N->now, s, b, (int)sizeof b);
                if (len <= 0 || (N->loss && (int)(rnd() % 100u) < N->loss)) continue;
                for (k = 0; k < NQ; k++)
                    if (!N->q[k].len) {
                        N->q[k].to = 1 - s;
                        N->q[k].at = N->now + N->owd;
                        N->q[k].len = len;
                        memcpy(N->q[k].b, b, (size_t)len);
                        break;
                    }
            }
        }
        for (s = 0; s < 2; s++) (void)rbn_poll(N->n[s], N->now);
    }
}

static void nn_init(nnet_t *N, int wantA, int wantB, u32 owd, int dfix)
{
    memset(N, 0, sizeof *N);
    N->n[0] = rbn_new(wantA, 0xA1110000u + 7u, 2, 3, dfix);
    N->n[1] = rbn_new(wantB, 0xB2220000u + 9u, 2, 3, dfix);
    N->send[0] = wantA;
    N->send[1] = wantB;
    N->owd = owd;
    N->now = 1000;
}
static void nn_free(nnet_t *N)
{
    rbn_free(N->n[0]);
    rbn_free(N->n[1]);
}

static void rbn_case_ok(u32 owd, int expect_d)
{
    nnet_t N;
    int sa, sb;
    nn_init(&N, 1, 1, owd, 0);
    nn_step(&N, 3000); /* Lobby */
    sa = rbn_mission_start(N.n[0], N.now, 0, 1);
    nn_step(&N, 300);  /* Client laedt etwas laenger */
    sb = rbn_mission_start(N.n[1], N.now, 1, 1);
    CHECK(sa == RBN_ST_TENT && sb == RBN_ST_PEND, "owd %u: Start TENT/PEND (%d/%d)", owd, sa, sb);
    nn_step(&N, 2000);
    CHECK(rbn_state(N.n[0]) == RBN_ST_R2 && rbn_state(N.n[1]) == RBN_ST_R2, "owd %u: beide R2 (%d/%d)", owd,
          rbn_state(N.n[0]), rbn_state(N.n[1]));
    CHECK(rbn_epoch(N.n[0]) == rbn_epoch(N.n[1]) && rbn_epoch(N.n[0]) != 0, "owd %u: gleiche Epoche", owd);
    CHECK(rbn_delay(N.n[0]) == rbn_delay(N.n[1]) && rbn_delay(N.n[0]) == expect_d, "owd %u: Verzoegerung %d/%d erwartet %d",
          owd, rbn_delay(N.n[0]), rbn_delay(N.n[1]), expect_d);
    nn_step(&N, 3000);
    CHECK(!rbn_peer_disagrees(N.n[0]) && !rbn_peer_disagrees(N.n[1]), "owd %u: keine Uneinigkeit", owd);
    /* zweite Mission: Client beginnt zuerst - darf die alte Entscheidung nicht nehmen */
    nn_step(&N, 60000);
    rbn_mission_end(N.n[0]);
    rbn_mission_end(N.n[1]);
    sb = rbn_mission_start(N.n[1], N.now, 1, 1);
    nn_step(&N, 1000);
    CHECK(rbn_state(N.n[1]) == RBN_ST_PEND, "owd %u: Mission 2 Client wartet (keine alte Entscheidung) %d", owd,
          rbn_state(N.n[1]));
    sa = rbn_mission_start(N.n[0], N.now, 0, 1);
    nn_step(&N, 2000);
    CHECK(rbn_state(N.n[0]) == RBN_ST_R2 && rbn_state(N.n[1]) == RBN_ST_R2 && rbn_epoch(N.n[0]) == rbn_epoch(N.n[1]),
          "owd %u: Mission 2 beide R2, gleiche Epoche", owd);
    (void)sa;
    nn_free(&N);
}

static void rbn_tests(void)
{
    nnet_t N;
    int k, st, rejected = 0, acc = 0;
    uint8_t p[RBN_PKT_MAX], q[RBN_PKT_MAX + 8];
    printf("Aushandlung (W346 R4)\n");
    CHECK(rbn_pick_delay(-1, 2, 3) == 2 && rbn_pick_delay(0, 2, 3) == 2 && rbn_pick_delay(0, 1, 3) == 1 &&
              rbn_pick_delay(30, 2, 3) == 2 && rbn_pick_delay(90, 2, 3) == 2 && rbn_pick_delay(130, 2, 3) == 2 &&
              rbn_pick_delay(140, 2, 3) == 3 && rbn_pick_delay(200, 2, 3) == 3 && rbn_pick_delay(900, 2, 8) == 8,
          "adaptive Verzoegerung");
    rbn_case_ok(0, 2);
    rbn_case_ok(15, 2);  /* Nachbar 30 ms RTT */
    rbn_case_ok(40, 2);  /* EU <-> USA 80-90 ms RTT */
    rbn_case_ok(100, 3); /* 200 ms RTT */
    /* Client ohne loc_rb_input (sendet nichts): Host Lockstep */
    nn_init(&N, 1, 0, 20, 0);
    nn_step(&N, 3000);
    st = rbn_mission_start(N.n[0], N.now, 0, 1);
    CHECK(st == RBN_ST_LS && rbn_why(N.n[0]) == RBN_WHY_PEER_NO, "Client ohne Rollback: Host Lockstep (%d)", st);
    nn_free(&N);
    /* Host ohne loc_rb_input: Client Lockstep sofort */
    nn_init(&N, 0, 1, 20, 0);
    nn_step(&N, 3000);
    st = rbn_mission_start(N.n[1], N.now, 1, 1);
    CHECK(st == RBN_ST_LS && rbn_why(N.n[1]) == RBN_WHY_HOST_NO, "Host ohne Rollback: Client Lockstep (%d)", st);
    nn_free(&N);
    /* Host bietet an, Client kennt den Host nicht (Pakete an B verloren) -> Client Lockstep, Host faellt zurueck */
    nn_init(&N, 1, 1, 20, 0);
    nn_step(&N, 3000);
    N.drop_to[1] = 1;
    nn_step(&N, 2830); /* letztes A-Hallo bei B um 3820 (veraltet ab 6820), B-Hallo von 6800 mit Bestaetigung bei A frisch */
    st = rbn_mission_start(N.n[0], N.now, 0, 1);
    CHECK(st == RBN_ST_TENT, "Host bietet an (%d)", st);
    st = rbn_mission_start(N.n[1], N.now, 1, 1);
    CHECK(st == RBN_ST_LS, "Client ohne Host-Kenntnis Lockstep (%d)", st);
    nn_step(&N, 1000);
    CHECK(rbn_state(N.n[0]) == RBN_ST_LS && rbn_why(N.n[0]) == RBN_WHY_PEER_LS, "Host faellt auf Lockstep zurueck (%d/%d)",
          rbn_state(N.n[0]), rbn_why(N.n[0]));
    nn_free(&N);
    /* Frist: keine Pakete mehr -> beide Lockstep */
    nn_init(&N, 1, 1, 20, 0);
    nn_step(&N, 3000);
    N.drop_to[0] = N.drop_to[1] = 1;
    st = rbn_mission_start(N.n[0], N.now, 0, 1);
    CHECK(st == RBN_ST_TENT, "Frist: Host bietet an");
    st = rbn_mission_start(N.n[1], N.now, 1, 1);
    CHECK(st == RBN_ST_PEND, "Frist: Client wartet");
    nn_step(&N, RBN_WAIT_MS + 500);
    CHECK(rbn_state(N.n[0]) == RBN_ST_LS && rbn_state(N.n[1]) == RBN_ST_LS && rbn_why(N.n[0]) == RBN_WHY_TIMEOUT &&
              rbn_why(N.n[1]) == RBN_WHY_TIMEOUT,
          "Frist: beide Lockstep");
    nn_free(&N);
    /* Verlust 30 %: trotzdem einig (R2 beide oder Lockstep beide) */
    for (k = 0; k < 40; k++) {
        int a, b;
        nn_init(&N, 1, 1, 40, 0);
        N.loss = 30;
        nn_step(&N, 2000 + (u32)(rnd() % 3000u));
        rbn_mission_start(N.n[0], N.now, 0, 1);
        nn_step(&N, (u32)(rnd() % 50u) * 10u);
        rbn_mission_start(N.n[1], N.now, 1, 1);
        nn_step(&N, RBN_WAIT_MS + 1000);
        a = rbn_state(N.n[0]);
        b = rbn_state(N.n[1]);
        CHECK(a == b && (a == RBN_ST_R2 || a == RBN_ST_LS), "Verlust 30%% Lauf %d: uneinig (%d/%d)", k, a, b);
        if (a == RBN_ST_R2) CHECK(rbn_epoch(N.n[0]) == rbn_epoch(N.n[1]), "Verlust: Epoche");
        nn_free(&N);
    }
    /* Paketpruefung + Fuzz */
    {
        rbn_t *a = rbn_new(1, 0x11110001u, 1, 3, 0), *b = rbn_new(1, 0x22220001u, 1, 3, 0);
        int n;
        n = rbn_tx(b, 5000, 1, p, (int)sizeof p);
        CHECK(rbn_rx(a, 5000, p, n, 7) == 1, "gueltiges Hallo angenommen");
        n = rbn_tx(a, 5010, -1, p, (int)sizeof p);
        CHECK(rbn_rx(b, 5010, p, n, 3) == 1, "Hallo mit Eintrag angenommen");
        n = rbn_tx(b, 5020, 1, p, (int)sizeof p);
        CHECK(rbn_rx(a, 5020, p, n, 7) == 1, "Hallo mit Bestaetigung angenommen");
        CHECK(rbn_mission_start(a, 5100, 0, 1) == RBN_ST_TENT, "Host bietet R2 an");
        n = rbn_tx(a, 5200, 0, p, (int)sizeof p); /* mit Entscheidung und Eintrag */
        CHECK(n == RBN_PKT_HEAD + RBN_PKT_ENT, "Hallo mit Eintrag (%d)", n);
        CHECK(rbn_rx(b, 5200, p, n, 3) == 1, "Hallo mit Entscheidung angenommen");
        CHECK(rbn_rx(b, 5200, p, n - 1, 3) == -1, "Laenge falsch verworfen");
        CHECK(rbn_rx(b, 5200, p, RBN_PKT_MAX + 1, 3) == -1, "zu lang verworfen");
        CHECK(rbn_rx(b, 5200, p, 3, 3) == -1, "zu kurz verworfen");
        memcpy(q, p, (size_t)n);
        q[4] = 2;
        CHECK(rbn_rx(b, 5200, q, n, 3) == -1, "Version verworfen");
        memcpy(q, p, (size_t)n);
        q[6] |= 8;
        CHECK(rbn_rx(b, 5200, q, n, 3) == -1, "unbekannte Flags verworfen");
        memcpy(q, p, (size_t)n);
        q[25] = 9;
        CHECK(rbn_rx(b, 5200, q, n, 3) == -1, "Verzoegerung > 8 verworfen");
        memcpy(q, p, (size_t)n);
        q[25] = 0;
        CHECK(rbn_rx(b, 5200, q, n, 3) == -1, "R2 ohne Verzoegerung verworfen");
        memcpy(q, p, (size_t)n);
        q[5] = 1;
        CHECK(rbn_rx(b, 5200, q, n, 3) == -1, "Entscheidung nicht vom Spieler 0 verworfen");
        memcpy(q, p, (size_t)n);
        q[8] = q[9] = q[10] = q[11] = 0;
        CHECK(rbn_rx(b, 5200, q, n, 3) == -1, "Kennung 0 verworfen");
        memcpy(q, p, (size_t)n);
        q[RBN_PKT_HEAD + 11] = 1;
        CHECK(rbn_rx(b, 5200, q, n, 3) == -1, "Fuellbyte im Eintrag verworfen");
        CHECK(rbn_rx(b, 5200, p, n, 4) == -1, "gleiche Kennung von anderer Adresse verworfen");
        for (k = 0; k < 200000; k++) { /* Zufallsverfaelschung: nie Absturz; nichts Ungueltiges wird R2-faehig */
            int len = (int)(rnd() % (u32)(RBN_PKT_MAX + 8)), j, r;
            memcpy(q, p, (size_t)n);
            for (j = 0; j < 1 + (int)(rnd() % 4u); j++) q[rnd() % (u32)n] ^= (uint8_t)(1u << (rnd() % 8));
            if (k % 7 == 0) len = n;
            if (k % 11 == 0) for (j = 0; j < RBN_PKT_MAX + 8; j++) q[j] = (uint8_t)rnd();
            r = rbn_rx(b, 6000 + (u32)k, q, len, 100 + (uint64_t)(k % 5));
            if (r < 0) rejected++;
            else if (r > 0) acc++;
        }
        CHECK(rejected > 100000, "Fuzz: zu wenig verworfen (%d)", rejected);
        printf("Aushandlung Fuzz: %d von 200000 verworfen, %d angenommen, schlecht=%u voll=%u\n", rejected, acc,
               rbn_stats(b)->rx_bad, rbn_stats(b)->rx_full);
        rbn_free(a);
        rbn_free(b);
    }
}

/* W346: Kanal-Fuzz der Eingabeschicht erweitert: Grenzlaengen und Feld-Extreme jedes Kopffelds. */
static void channel_fuzz2(void)
{
    rbi_t *a = rbi_new(), *b = rbi_new();
    uint8_t p[RBI_PKT_MAX], q[RBI_PKT_MAX + 64];
    uint8_t hdr[RBI_HDR] = {1, 0x55, 0, 0, 3, 0, 0, 0}, in[RBI_IN] = {0}, out[RBI_FR];
    int n, k, f, rej = 0, tot = 0;
    static const uint32_t ext[] = {0u, 1u, 0x3FFFFFFFu, 0x40000000u, 0x40000001u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu};
    rbi_begin(a, 9, 0, 2, 8);
    rbi_begin(b, 9, 1, 2, 8);
    for (k = 0; k < 30; k++) rbi_own(a, hdr, in);
    CHECK(rbi_take(a, 0, 0, out) == 1, "Fuzz2: eigene Entnahme");
    n = rbi_tx(a, p, (int)sizeof p);
    for (k = -2; k <= 2; k++) { /* Laengen um die exakte Laenge */
        if (k == 0) continue;
        memset(q, 0, sizeof q);
        memcpy(q, p, (size_t)n);
        CHECK(rbi_rx(b, q, n + k, 1) == -1, "Fuzz2: Laenge %+d verworfen", k);
    }
    for (f = 12; f + 4 <= RBI_PKT_HEAD; f += 4) { /* jedes Kopf-Dword mit Extremwerten */
        size_t e;
        for (e = 0; e < sizeof ext / sizeof ext[0]; e++) {
            int r;
            memcpy(q, p, (size_t)n);
            q[f] = (uint8_t)ext[e];
            q[f + 1] = (uint8_t)(ext[e] >> 8);
            q[f + 2] = (uint8_t)(ext[e] >> 16);
            q[f + 3] = (uint8_t)(ext[e] >> 24);
            r = rbi_rx(b, q, n, 1);
            tot++;
            if (r < 0) rej++;
            if (f == 16 || f == 20) CHECK(ext[e] <= 0x40000000u || r != 1, "Fuzz2: Feld %d Wert %08X angenommen", f, ext[e]);
        }
    }
    for (k = 4; k < 12; k++) { /* Zaehlbytes 0..255 */
        int v;
        for (v = 0; v < 256; v += 17) {
            memcpy(q, p, (size_t)n);
            q[k] = (uint8_t)v;
            (void)rbi_rx(b, q, n, 1);
            tot++;
        }
    }
    printf("Kanal-Fuzz2: %d Grenzfaelle, %d verworfen, kein Absturz\n", tot, rej);
    rbi_free(a);
    rbi_free(b);
}

/* ---- W359 LOC-RB-FIDELITY ---- */
/* 1. Spielmodell der Render-Uebergabe: Spiellogik reiht Freigaben in eine Warteschlange (max. 127, voll = sofort frei),
 *    die Render-Vorbereitung (nur bei Auftrag 0) leert sie, kopiert den Doppelpuffer, tauscht den Index und setzt den
 *    Auftrag; der Render-Thread zeichnet (live) und setzt ihn zurueck. Live gegen erzwungene Rollbacks (rbtest 3,8)
 *    mit gleichen Eingaben: mit rbi_rjob_replay(prep=1) bitgleich, mit der alten Regel (prep=0) weicht der Heap ab,
 *    obwohl der Spiel-Hash gleich bleibt (genau das Fehlerbild von W355/W359). */
typedef struct {
    u32 bm[8];        /* Spielzeug-Heap: 256 Bloecke, 1 = belegt (niedrigster freier zuerst wie ein Heap) */
    uint16_t obj[48]; /* Objekt -> Block (0xFFFF = frei) */
    uint16_t q[127];
    uint16_t pad;
    u32 qn, idx, frame, rjob, buf[2][8], logic[8], acc, immediate;
} w9_toy_t;
static int w9_alloc(w9_toy_t *t)
{
    int b;
    for (b = 0; b < 256; b++)
        if (!(t->bm[b >> 5] & (1u << (b & 31)))) {
            t->bm[b >> 5] |= 1u << (b & 31);
            return b;
        }
    return -1;
}
static void w9_free(w9_toy_t *t, int b) { t->bm[b >> 5] &= ~(1u << (b & 31)); }
static void w9_tick(w9_toy_t *t, u32 in)
{
    int k, i;
    for (k = 0; k < 3; k++) { /* Logik: Objekte entstehen/vergehen je nach Eingabe */
        int o = (int)((in >> (k * 6)) % 48u);
        if (t->obj[o] == 0xFFFF) {
            int b = w9_alloc(t);
            if (b >= 0) t->obj[o] = (uint16_t)b;
        } else {
            if (t->qn < 127u) t->q[t->qn++] = t->obj[o]; /* aufgeschobene Freigabe (Render-Thread koennte zeichnen) */
            else {
                w9_free(t, t->obj[o]);
                t->immediate++;
            }
            t->obj[o] = 0xFFFF;
        }
    }
    for (i = 0; i < 8; i++) t->logic[i] = t->logic[i] * 31u + (in >> i) + (u32)i;
    t->acc = 0;
    for (i = 0; i < 8; i++) t->acc = t->acc * 16777619u ^ t->logic[i]; /* Spiel-Hash: nur Spielwerte, keine Adressen */
    if (t->rjob == 0) { /* Render-Vorbereitung (0x2F256) */
        while (t->qn) w9_free(t, t->q[--t->qn]);
        memcpy(t->buf[t->idx], t->logic, sizeof t->logic);
        t->idx ^= 1u;
        t->frame++;
        t->rjob = 1;
    }
}
static u32 w9_in(u32 tick)
{
    u32 x = tick * 2654435761u;
    x ^= x >> 13;
    return x * 0x5bd1e995u;
}
static void w9_run(int prep, int rb, u32 ticks, w9_toy_t *out, u32 *hash_eq_all)
{
    static w9_toy_t snap[16];
    w9_toy_t t;
    u32 g;
    memset(&t, 0, sizeof t);
    memset(t.obj, 0xFF, sizeof t.obj);
    *hash_eq_all = 1;
    for (g = 1; g <= ticks; g++) {
        w9_tick(&t, w9_in(g));
        t.rjob = 0; /* live: Render-Thread zeichnet, Sichern wartet darauf (rb_quiesce) */
        snap[g & 15] = t;
        if (rb && g > 8 && g % 3u == 0) { /* erzwungener Rollback um 8 Ticks mit gleichen Eingaben */
            w9_toy_t ref = t;
            u32 j;
            t = snap[(g - 8) & 15];
            t.rjob = rbi_rjob_replay(prep, RBI_RJOB_LOAD, t.rjob);
            for (j = g - 7; j <= g; j++) {
                w9_tick(&t, w9_in(j));
                t.rjob = rbi_rjob_replay(prep, RBI_RJOB_TICK, t.rjob);
                snap[j & 15] = t;
            }
            t.rjob = rbi_rjob_replay(prep, RBI_RJOB_DONE, t.rjob);
            if (t.acc != ref.acc) *hash_eq_all = 0;
        }
    }
    *out = t;
}
static void w359_model_cases(void)
{
    static w9_toy_t ref, a, b;
    u32 heq0, heq1, heq2;
    int same_a, heap_b;
    w9_run(1, 0, 3000, &ref, &heq0);
    w9_run(1, 1, 3000, &a, &heq1);
    w9_run(0, 1, 3000, &b, &heq2);
    same_a = memcmp(&ref, &a, sizeof ref) == 0;
    heap_b = memcmp(ref.bm, b.bm, sizeof ref.bm) == 0;
    CHECK(same_a, "W359: Neurechnen mit Render-Vorbereitung weicht vom Live-Stand ab");
    CHECK(heq1 && heq2, "W359: Spiel-Hash nach dem Neurechnen verschieden (Modell)");
    CHECK(!heap_b || ref.qn != b.qn || ref.frame != b.frame,
          "W359: Gegenprobe alte Regel (Vorbereitung uebersprungen) zeigte keine Abweichung");
    printf("W359 Modell: live/rprep=1 bitgleich=%d, alte Regel: Heap gleich=%d Warteschlange %u/%u Bildzaehler %u/%u "
           "sofort frei %u/%u, Spiel-Hash gleich=%u\n",
           same_a, heap_b, ref.qn, b.qn, ref.frame, b.frame, ref.immediate, b.immediate, heq2);
    CHECK(rbi_rjob_replay(1, RBI_RJOB_LOAD, 1) == 0 && rbi_rjob_replay(1, RBI_RJOB_TICK, 1) == 0 &&
              rbi_rjob_replay(1, RBI_RJOB_DONE, 1) == 0,
          "W359: rprep=1 muss den Auftrag immer freigeben");
    CHECK(rbi_rjob_replay(0, RBI_RJOB_LOAD, 0) == 1 && rbi_rjob_replay(0, RBI_RJOB_TICK, 1) == 1 &&
              rbi_rjob_replay(0, RBI_RJOB_DONE, 1) == 0,
          "W359: rprep=0 (alt) falsch");
}

/* 2. Heap-Pruefung (rbi_heap_check) auf einem nachgebauten XDK-NT-Heap in einem Puffer. */
#define HB 0x10000000u
#define HSZ 0x10000u
static uint8_t s_hp[HSZ];
static int s_hp_unread; /* 1 = Kopf nicht lesbar */
static int hp_rd(void *ctx, u32 va, u32 *out)
{
    (void)ctx;
    if (va < HB || va - HB + 4u > HSZ || (s_hp_unread && va - HB < 0x400u)) return 0;
    memcpy(out, s_hp + (va - HB), 4);
    return 1;
}
static void hp_w(u32 va, u32 v) { memcpy(s_hp + (va - HB), &v, 4); }
static u32 hp_r(u32 va)
{
    u32 v;
    memcpy(&v, s_hp + (va - HB), 4);
    return v;
}
static void hp_list(int i, const u32 *blk, const u32 *sz, int n, u32 flags_last)
{
    u32 h = HB + 0x180u + (u32)i * 8u, prev = h;
    int k;
    for (k = 0; k < n; k++) {
        u32 node = blk[k] + 8u;
        hp_w(blk[k], sz[k] | (0x22u << 16));                   /* Groesse | Vorgaenger */
        hp_w(blk[k] + 4u, (k == n - 1 ? flags_last : 0u) << 8); /* Flags (Byte +5) */
        hp_w(prev, node);                                       /* Flink des Vorgaengers (Kopf oder Knoten) */
        hp_w(node + 4u, prev);                                  /* Blink */
        prev = node;
    }
    hp_w(prev, h);
    hp_w(h + 4u, prev);
    if (i && n) hp_w(HB + 0x160u + (u32)(i >> 5) * 4u, hp_r(HB + 0x160u + (u32)(i >> 5) * 4u) | (1u << (i & 31)));
}
static void hp_build(void)
{
    static const u32 b3[2] = {HB + 0x1000u, HB + 0x2000u}, s3[2] = {3, 3};
    static const u32 b0[3] = {HB + 0x3000u, HB + 0x4000u, HB + 0x5000u}, s0[3] = {0x80, 0x100, 0x100};
    static const u32 b7[1] = {HB + 0x6010u}, s7[1] = {7};
    int i;
    memset(s_hp, 0, sizeof s_hp);
    s_hp_unread = 0;
    for (i = 0; i < 128; i++) { /* leere Listen: Flink = Blink = Kopf */
        hp_w(HB + 0x180u + (u32)i * 8u, HB + 0x180u + (u32)i * 8u);
        hp_w(HB + 0x184u + (u32)i * 8u, HB + 0x180u + (u32)i * 8u);
    }
    hp_list(3, b3, s3, 2, 0x10u);
    hp_list(0, b0, s0, 3, 0x10u);
    hp_list(7, b7, s7, 1, 0u);
}
static int hp_chk(u32 *bad, u32 max)
{
    u32 nodes = 0;
    return rbi_heap_check(hp_rd, NULL, HB, max, bad, &nodes);
}
static void w359_heap_cases(void)
{
    u32 bad = 0, nodes = 0, n1 = HB + 0x1008u, n2 = HB + 0x2008u;
    int rc, it, det = 0;
    hp_build();
    rc = rbi_heap_check(hp_rd, NULL, HB, 20000u, &bad, &nodes);
    CHECK(rc == RBI_HEAP_OK && nodes == 6, "W359 Heap: gueltiger Heap rc=%d (%s) Knoten %u", rc, rbi_heap_err(rc), nodes);
    hp_build();
    hp_w(n2 + 4u, 0); /* Blink = 0 (F2: Ausketten mit prev = 0) */
    CHECK(hp_chk(&bad, 20000u) == RBI_HEAP_LINK && bad == n2, "W359 Heap: Blink 0 nicht erkannt");
    hp_build();
    hp_w(n1, 0); /* Flink = 0 (GT: Knoten mit next = 0) */
    CHECK(hp_chk(&bad, 20000u) == RBI_HEAP_PTR, "W359 Heap: Flink 0 nicht erkannt");
    hp_build();
    hp_w(n1, HB + 0x50008u); /* ausserhalb (A2: ein Bit falsch -> ungueltige Seite) */
    CHECK(hp_chk(&bad, 20000u) == RBI_HEAP_PTR, "W359 Heap: Zeiger ausserhalb nicht erkannt");
    hp_build();
    hp_w(n1, n2 + 4u); /* falsch ausgerichtet */
    CHECK(hp_chk(&bad, 20000u) == RBI_HEAP_PTR, "W359 Heap: Ausrichtung nicht erkannt");
    hp_build();
    hp_w(n2, n1); /* Zyklus ohne Kopf (R2/GT2: Endlosschleife 0x1297E7) */
    CHECK(hp_chk(&bad, 20000u) == RBI_HEAP_LINK, "W359 Heap: Zyklus nicht erkannt");
    hp_build();
    hp_w(HB + 0x1000u, 4u | (0x22u << 16)); /* Groesse passt nicht zur Liste 3 */
    CHECK(hp_chk(&bad, 20000u) == RBI_HEAP_SIZE && bad == HB + 0x1000u, "W359 Heap: Groesse nicht erkannt");
    hp_build();
    hp_w(HB + 0x2004u, 0x11u << 8); /* als belegt markiert (doppelt vergeben) */
    CHECK(hp_chk(&bad, 20000u) == RBI_HEAP_BUSY, "W359 Heap: Belegt-Bit nicht erkannt");
    hp_build();
    hp_w(HB + 0x3000u, 0x200u | (0x22u << 16)); /* Liste 0 nicht aufsteigend */
    CHECK(hp_chk(&bad, 20000u) == RBI_HEAP_ORDER, "W359 Heap: Reihenfolge Liste 0 nicht erkannt");
    hp_build();
    hp_w(HB + 0x4000u, 0x40u | (0x22u << 16)); /* zu klein fuer Liste 0 */
    CHECK(hp_chk(&bad, 20000u) == RBI_HEAP_SIZE, "W359 Heap: kleiner Block in Liste 0 nicht erkannt");
    hp_build();
    hp_w(HB + 0x160u, 0); /* Bitmap ohne Bit */
    CHECK(hp_chk(&bad, 20000u) == RBI_HEAP_BITMAP, "W359 Heap: Bitmap nicht erkannt");
    hp_build();
    hp_w(HB + 0x184u + 3u * 8u, n1); /* Kopf-Blink falsch */
    CHECK(hp_chk(&bad, 20000u) == RBI_HEAP_LINK, "W359 Heap: Kopf-Blink nicht erkannt");
    hp_build();
    s_hp_unread = 1;
    CHECK(hp_chk(&bad, 20000u) == RBI_HEAP_HDR, "W359 Heap: unlesbarer Kopf nicht erkannt");
    hp_build();
    hp_w(n2, n1); /* Begrenzung: nach max_steps ohne Fehler abbrechen */
    rc = rbi_heap_check(hp_rd, NULL, HB, 1u, &bad, &nodes);
    CHECK(rc == RBI_HEAP_OK && nodes == 1, "W359 Heap: Begrenzung rc=%d Knoten %u", rc, nodes);
    for (it = 0; it < 20000; it++) { /* Fuzz: zufaellige Aenderungen - nie Absturz, Schaeden werden gemeldet */
        u32 off;
        hp_build();
        off = (rnd() % (0x6100u / 4u)) * 4u;
        hp_w(HB + off, rnd() % 3u ? hp_r(HB + off) ^ (1u << (rnd() % 32u)) : rnd());
        if (hp_chk(&bad, 20000u) != RBI_HEAP_OK) det++;
    }
    printf("W359 Heap-Fuzz: 20000 Aenderungen, %d als beschaedigt erkannt\n", det);
    CHECK(det > 0, "W359 Heap-Fuzz erkannte nichts");
}

int main(void)
{
    scen_t s;
    printf("loc_rbi_selftest (W338 LOC-ROLLBACK-R2, W346 R3/R4)\n");
    channel_tests();
    channel_fuzz2();
    rbn_tests();
    memset(&s, 0, sizeof s);
    s.D = 2;
    s.maxpred = 10;
    s.ticks = 6000;
    scenario("0 Ticks, D=2", &s, 0);
    s.D = 1;
    scenario("0 Ticks, D=1", &s, -1); /* Simulation: B->A kommt einen halben Tick spaet */
    s.delay_ab = s.delay_ba = 2;
    s.D = 1;
    scenario("2 Ticks je Richtung, D=1", &s, 1);
    s.delay_ab = 1;
    s.delay_ba = 3;
    s.jitter = 2;
    s.D = 2;
    scenario("1/3+2 Ticks, D=2", &s, 1);
    s.delay_ab = s.delay_ba = 3;
    s.jitter = 3;
    s.loss_pct = 10;
    scenario("3+3 Ticks, 10% Verlust", &s, 1);
    s.skip_b = 9;
    scenario("dto., B 10% langsamer", &s, 1);
    s.delay_ab = s.delay_ba = 12;
    s.jitter = 0;
    s.loss_pct = 0;
    s.skip_b = 0;
    s.maxpred = 8;
    scenario("12 Ticks (> Fenster)", &s, 1);
    /* W355 LOC-DESYNC-250: Neubelegung im Kampf bei offener Vorhersage und laufender Neurechnung (Nutzerlauf: Neuaufbau
     * mitten in der Neurechnung -> Barriere -> Rollback unmoeglich -> Desync). Erst die Gegenprobe mit der W346-Regel
     * (muss den Fehler zeigen), dann dieselben Strecken mit rbi_rebuild_gate. */
    {
        static const struct { int dab, dba, jit, loss, skip, D, chg, regrow; const char *n; } k_al[] = {
            {3, 3, 3, 10, 9, 3, 7, 0, "Allok. 3+3, B langsam"},
            {3, 3, 2, 2, 5, 3, 1, 0, "Allok. 3+3, Eingabe je Tick"},
            {5, 5, 4, 5, 4, 3, 2, 0, "Allok. 5+5, B 25% langsamer"},
            {2, 6, 3, 0, 0, 2, 3, 0, "Allok. 2/6 asymmetrisch"},
            {3, 3, 2, 2, 5, 3, 1, 1, "Allok. neu beim Neurechnen"},
            {5, 5, 4, 5, 4, 3, 2, 1, "dto., 5+5, B langsam"},
        };
        unsigned i;
        u32 bad_old = 0;
        for (i = 0; i < sizeof k_al / sizeof k_al[0]; i++) {
            char nm[64];
            memset(&s, 0, sizeof s);
            s.delay_ab = k_al[i].dab;
            s.delay_ba = k_al[i].dba;
            s.jitter = k_al[i].jit;
            s.loss_pct = k_al[i].loss;
            s.skip_b = k_al[i].skip;
            s.D = k_al[i].D;
            s.maxpred = 10;
            s.ticks = 6000;
            s.alloc = 1;
            g_chg = (u32)k_al[i].chg;
            g_regrow = k_al[i].regrow;
            s.oldgate = 1;
            g_expect_bad = 1;
            _snprintf_s(nm, sizeof nm, _TRUNCATE, "%s (W346)", k_al[i].n);
            scenario(nm, &s, 1);
            bad_old += g_last_bad;
            g_expect_bad = 0;
            s.oldgate = 0;
            scenario(k_al[i].n, &s, 1);
        }
        /* erzwungene Rollbacks (rbtest) ueber Neubelegungen und Aufschub */
        memset(&s, 0, sizeof s);
        s.delay_ab = s.delay_ba = 3;
        s.jitter = 2;
        s.D = 3;
        s.maxpred = 10;
        s.ticks = 6000;
        s.alloc = 1;
        s.force_n = 7;
        s.force_k = 6;
        g_chg = 3;
        g_regrow = 1;
        scenario("Allok. + erzwungene Rollbacks", &s, 1);
        /* Belegung haengt von der Eingabe ab (zuerst im Neurechnen), erneuter Commit beim Neurechnen genullt bzw. nicht */
        memset(&s, 0, sizeof s);
        s.delay_ab = s.delay_ba = 3;
        s.jitter = 2;
        s.loss_pct = 2;
        s.skip_b = 5;
        s.D = 3;
        s.maxpred = 10;
        s.ticks = 6000;
        s.alloc = 1;
        g_chg = 1;
        g_regrow = 1;
        g_inalloc = 1;
        g_nozero = 1;
        g_expect_bad = 1;
        scenario("Allok. eingabeabh. (ohne Nullen)", &s, 1);
        printf("Gegenprobe ohne Nullen: %u Fehler\n", g_last_bad);
        CHECK(g_last_bad > 0, "Gegenprobe: erneuter Commit ohne Nullen zeigte keinen Fehler");
        g_expect_bad = 0;
        g_nozero = 0;
        scenario("Allok. eingabeabhaengig", &s, 1);
        g_inalloc = 0;
        g_chg = 7;
        g_regrow = 0;
        printf("Gegenprobe W346-Regel: %u Fehler (verloren/abweichend) ueber alle Allokations-Strecken\n", bad_old);
        CHECK(bad_old > 0, "Gegenprobe: die W346-Regel zeigte den Fehler nicht (Strecken zu harmlos)");
    }
    w359_model_cases(); /* W359 */
    w359_heap_cases();
    printf("%s: %d ok, %d fehlgeschlagen\n", g_fail ? "FEHLER" : "OK", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
