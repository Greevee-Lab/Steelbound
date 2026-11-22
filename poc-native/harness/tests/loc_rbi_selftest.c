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
 *  9. LOC-LOCKSTEP-ARBITER: Pufferregler des Lockstep-Rueckfalls (tests/loc_lsarb_cases.c).
 *  8. W372: Zeitstrecke in us mit Feldraster (16,7 ms, 3 Felder je Tick), Weg je Richtung, Schwankung, Phasen- und
 *     Startversatz, Taktdrift: Puffer-Messung (rbi_clock), fruehes Senden, Zeitabgleich ueber die Feldzahl
 *     (rbi_tsync, SBRT) gegen Gegenproben (spaetes Senden, kein Zeitabgleich); SBRT-Pruefung und Fuzz.
 * Exit 0 = alles gruen. */
#include "loc_rbi.h"
#include "loc_state.h" /* W382 */

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
    u32 edges[NP];     /* W380: Pedal-Flanken (Eingabesatz Byte 5, 0 -> != 0) je Spieler, wie FUN_0001A330(.,0,6,0xC) */
    uint8_t ped[NP];
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
    u32 tprod;                   /* W380: angenommene eigene Rahmen (Index q der Eingabeschicht) */
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
    for (i = 0; i < NP; i++) h = (h ^ s->edges[i] ^ ((u32)s->ped[i] << 16)) * 16777619u; /* W380 */
    return (h ^ s->tick) * 16777619u;
}

static int g_tap;           /* W380: Pedal-Tipps: Byte 5 = 0xFF fuer g_tap Rahmen je 13 eigene Rahmen (0 = Zufallsbyte) */
static int g_tap_deff;      /* W380: wirksame Verzoegerung (Slot = Rahmen - Vorlauf + deff) */
static u32 g_tap_inv, g_tap_take, g_tap_miss; /* W380: live erfundene Flanken / gepruefte Entnahmen / Rahmen != Wahrheit */
static u32 g_tap_at[MAXS + 2];  /* W380: Flanken von B in A's Zeitlinie nach Tick t (Neurechnen ueberschreibt) */
static int tap_truth(u32 q) { return q >= 4u && (q % 13u) < (u32)g_tap; } /* Vorlauf (4 Rahmen) ohne Pedal */
static int g_ramp;          /* W385: Achsen (Byte 9..13) als Rampen mit wechselnder Steigung, Tasten in langen Abschnitten */
static int g_predm, g_predq = 6; /* W385: Vorhersagemodus beider Seiten */

static void own_frame(inst_t *X, int prefill)
{
    uint8_t hdr[RBI_HDR] = {1, 0, 0, 0, 0x03, 0, 0, 0}, in[RBI_IN];
    u32 v = prefill ? 0u : ((X->prod / g_chg) * 2654435761u) ^ (u32)(X->me * 977u); /* aendert sich alle g_chg Rahmen */
    int k;
    hdr[1] = (uint8_t)X->st.acc;
    for (k = 0; k < RBI_IN; k++) in[k] = (uint8_t)(v >> ((k & 3) * 8));
    if (g_tap) in[5] = tap_truth(X->tprod) ? 0xFF : 0; /* W380: Muster ueber den Rahmenindex der Eingabeschicht */
    if (g_ramp && !prefill) { /* W385: wie Maus/Hebel - stetige Bewegung, Steigung wechselt alle 23 Rahmen (auch 0 = Halten) */
        u32 q = X->prod, seg = q / 23u, sg;
        u32 bs = (q / 61u) * 2246822519u ^ (u32)X->me;
        for (k = 0; k < 5; k++) {
            int raw = 40 * k, v, t;
            for (sg = 0; sg <= seg; sg++) {
                u32 h = (sg * 2654435761u) ^ (u32)(X->me * 40503u) ^ (u32)(k * 0x9E3779B9u);
                int slope = (int)((h >> 11) % 11u) - 5;
                if ((h >> 3) % 3u == 0u) slope = 0;
                raw += slope * (int)(sg < seg ? 23u : q % 23u);
            }
            t = ((raw % 400) + 400) % 400; /* an den Grenzen spiegeln (Anschlag) */
            v = t < 200 ? t - 100 : 300 - t;
            in[9 + k] = (uint8_t)(int8_t)v;
        }
        in[0] = (uint8_t)(bs & 0x0Fu); /* Tasten: lange Abschnitte (Halten) */
        in[1] = 0; in[2] = 0; in[3] = 0; in[4] = 0;
        in[5] = in[6] = in[7] = 0; in[8] = 1;
    }
    X->prod++;
    if (rbi_own(X->r, hdr, in)) X->tprod++;
}

static u32 g_need_mask; /* W366: 0 = alle Spieler; sonst entnimmt das Spiel nur diese (Mitspieler entfernt) */
static u32 g_sp_n, g_sp_bad, g_sp_pred, g_sp_own_n, g_sp_own_bad; /* W390: Vorlauf-Rahmen geprueft/abweichend/vorhergesagt */
static u32 g_op_n, g_op_bad, g_op_full; /* W376: rbi_own_pending geprueft / abweichend / mit D Eintraegen */
static int g_op_D;
static u32 g_op_h[17];
/* ein Spieltick (bis zum Ruhepunkt) */
static void toy_tick(inst_t *X)
{
    u32 cmin = rbi_commits(X->r) > X->depth ? rbi_commits(X->r) - X->depth : 0;
    u32 need = g_need_mask ? g_need_mask : (1u << NP) - 1u;
    int i;
    uint8_t pend[16][RBI_IN];
    int npend = (X->st.have & (1u << X->me)) ? 0 : rbi_own_pending(X->r, pend, 16); /* W376 */
    uint8_t spf[NP][RBI_FR]; /* W390: Vorlauf-Rahmen fuer die naechste Entnahme (nur lesend) */
    int spr[NP];
    for (i = 0; i < NP; i++) {
        spr[i] = rbi_replaying(X->r) ? 0 : rbi_spec_frame(X->r, i, 0, NULL, spf[i]);
        if (i == X->me && spr[i] && !rbi_replaying(X->r) && npend < 16 && !(X->st.have & (1u << X->me))) { /* W390: eigene k = 0..npend-1 = ausstehende Saetze */
            int k2;
            uint8_t f2[RBI_FR], nw[RBI_FR];
            for (k2 = 0; k2 < npend; k2++) {
                g_sp_own_n++;
                if (rbi_spec_frame(X->r, i, (u32)k2, NULL, f2) != 1 || memcmp(f2 + RBI_HDR, pend[k2], RBI_IN) != 0) g_sp_own_bad++;
            }
            memset(nw, 0xA5, sizeof nw); /* erster fehlender eigener Slot: neu erzeugter Rahmen */
            if (rbi_spec_frame(X->r, i, (u32)npend, nw, f2) != 1 || memcmp(f2 + 2, nw + 2, RBI_FR - 2) != 0 || f2[1] != nw[1]) g_sp_own_bad++;
            if (rbi_spec_frame(X->r, i, (u32)npend + 1u, nw, f2) != 2 || ((f2[4] | (f2[5] << 8) | (f2[6] << 16) | ((u32)f2[7] << 24)) & RBI_FL_TRANSIENT) || memcmp(f2 + RBI_HDR, nw + RBI_HDR, RBI_IN) != 0)
                g_sp_own_bad++; /* danach halten */
        }
    }
    for (i = 0; i < NP; i++) {
        if ((X->st.have & (1u << i)) || !(need & (1u << i))) continue;
        if (rbi_take(X->r, i, cmin, X->st.buf[i])) {
            X->st.have |= 1u << i;
            if (spr[i] && !rbi_replaying(X->r)) { /* W390: gleicher Rahmen wie die echte Entnahme (bestaetigt bzw. Vorhersage) */
                g_sp_n++;
                if (spr[i] == 2) g_sp_pred++;
                if (memcmp(spf[i], X->st.buf[i], RBI_FR) != 0) g_sp_bad++;
            }
            if (i == X->me && npend > 0 && !rbi_replaying(X->r)) { /* W376: erster ausstehender = jetzt entnommener */
                g_op_n++;
                if (memcmp(pend[0], X->st.buf[i] + RBI_HDR, RBI_IN) != 0) g_op_bad++;
                if (npend == g_op_D) g_op_full++;
                g_op_h[npend < 16 ? npend : 16]++;
            }
        }
    }
    if ((X->st.have & need) != need) { /* Stau */
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
    for (i = 0; i < NP; i++) { /* W380: Pedal-Flanke wie im Spiel aus dem entnommenen Satz */
        uint8_t pd = X->st.buf[i][RBI_HDR + 5];
        if (g_tap && i != X->me && !rbi_replaying(X->r)) { /* live: Flanke nur, wenn die Wahrheit sie hat */
            u32 q = rbi_cons(X->r, i) - 1u + 4u - (u32)g_tap_deff;
            int tr = tap_truth(q), trp = q > 0 && tap_truth(q - 1u);
            g_tap_take++;
            if ((pd != 0) != tr) g_tap_miss++;
            if (pd && !X->st.ped[i] && !(tr && !trp)) g_tap_inv++;
        }
        if (pd && !X->st.ped[i]) X->st.edges[i]++;
        X->st.ped[i] = pd;
    }
    for (i = 0; i < NP; i++) { /* Anwenden (deterministisch aus den Eingaben) */
        u32 in = (u32)X->st.buf[i][RBI_HDR] | ((u32)X->st.buf[i][RBI_HDR + 1] << 8);
        if (g_ramp) { int k2; for (k2 = 9; k2 <= 13; k2++) in = in * 31u + X->st.buf[i][RBI_HDR + k2]; } /* W385: Achsen wirken */
        X->st.vel[i] = X->st.vel[i] * 3u + in;
        X->st.pos[i] += X->st.vel[i] ^ X->st.tick;
    }
    X->st.have = 0;
    X->st.tick++;
    if (g_tap && X->me == 0 && X->st.tick <= MAXS) g_tap_at[X->st.tick] = X->st.edges[1]; /* W380 */
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

/* W372: Zeitstrecke (us): Ankunftszeit an rbi_clock, SBRT, Senden um g_sendlag verzoegert (Gegenprobe "spaet") */
static int g_tclock, g_tt;
static int g_rx_old;     /* W375: Missionswechsel - alte Pakete tolerieren */
static u32 g_rx_old_rej;
static u32 g_sendlag, g_ttn[2];

static void q_deliver(inst_t *X, u32 now, inst_t *peer)
{
    int k;
    for (k = 0; k < QN; k++) {
        if (s_q[k].used && s_q[k].to == X->me && s_q[k].at <= now) {
            if (g_tclock) rbi_clock(X->r, s_q[k].at);
            if (g_rx_old) { /* W375: Pakete der vorigen Mission (alte Epoche) duerfen abgelehnt werden */
                int ok = rbi_is_tt(s_q[k].b, s_q[k].len) ? rbi_tt_rx(X->r, s_q[k].b, s_q[k].len, (uint64_t)(100 + peer->me))
                                                         : rbi_rx(X->r, s_q[k].b, s_q[k].len, (uint64_t)(100 + peer->me));
                if (ok != 1) g_rx_old_rej++;
            } else if (rbi_is_tt(s_q[k].b, s_q[k].len))
                CHECK(rbi_tt_rx(X->r, s_q[k].b, s_q[k].len, (uint64_t)(100 + peer->me)) == 1, "SBRT abgelehnt");
            else
                CHECK(rbi_rx(X->r, s_q[k].b, s_q[k].len, (uint64_t)(100 + peer->me)) == 1, "rx abgelehnt");
            s_q[k].used = 0;
        }
    }
}

/* Ruhepunkt: Commit, Empfang, Rollback planen, senden, endgueltige Hashes */
/* W386: Plan fuer Verzoegerungswechsel im Zwilling (Zeitschritt, neues D, Vorlauf in Slots je Seite) */
typedef struct { u32 t; int d; int lead[2]; u32 done[2]; } dchg_t;
static dchg_t g_dchg[16];
static int g_dchg_n;
static void toy_rest(inst_t *X, u32 now, const scen_t *sc, inst_t *peer, int delay)
{
    int done = 0, com, rebuild = 0;
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
    q_deliver(X, now, peer);
    if (g_dchg_n) { /* W386: Verzoegerungswechsel nach Plan (je Seite eigener Vorlauf, auch zu spaet) */
        int k;
        for (k = 0; k < g_dchg_n; k++)
            if (g_dchg[k].t == now && g_dchg[k].done[X->me & 1] != now + 1u) {
                g_dchg[k].done[X->me & 1] = now + 1u;
                (void)rbi_set_delay(X->r, g_dchg[k].d, (u32)((int)rbi_own_next_slot(X->r) + g_dchg[k].lead[X->me & 1]));
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
            q_put(peer->me, now + g_sendlag + (u32)delay + j, b, n);
        }
        if (g_tt && !(++g_ttn[X->me & 1] & 1u)) { /* W372: Puffer melden (jeder 2. Tick, wie die Anbindung) */
            n = rbi_tt_tx(X->r, b, (int)sizeof b);
            if (n > 0) q_put(peer->me, now + g_sendlag + (u32)delay + (sc->jitter ? rnd() % (u32)(sc->jitter + 1) : 0), b, n);
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
    rbi_set_pred(X->r, g_predm, g_predq); /* W385 (bleibt ueber rbi_begin erhalten) */
    rbi_begin(X->r, 7, me, sc->D, sc->maxpred);
    X->p0 = 4;
    for (k = 0; k < X->p0; k++) own_frame(X, 1); /* Vorlauf wie FUN_0001D3D0 */
    X->snap[0] = X->st;
}

static u32 g_last_bad;
static int g_w385_capture;          /* W385: Zaehler der Strecke vor dem Freigeben sichern */
static rbi_stats_t g_w385_st[2];
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
    if (g_tap) { /* W380: Flanken von B in A's Zeitlinie gegen die Wahrheit (Slots 0..T-1, T = 20 Ticks vor Ende:
                  * dort ist alles bestaetigt bzw. neu gerechnet; die letzten Slots sind noch Vorhersage) */
        u32 n = 0, k, T = A.st.tick > 20u ? A.st.tick - 20u : 0u;
        for (k = 0; k < T; k++) {
            u32 q = k + 4u - (u32)g_tap_deff;
            if (tap_truth(q) && !(q > 0 && tap_truth(q - 1u))) n++;
        }
        printf("%-28s W380 Pedal-Tipps (%d Rahmen): Flanken B in A bis Tick %u: %u, Wahrheit %u | live erfunden %u, "
               "live abweichend %u von %u\n", name, g_tap, T, g_tap_at[T], n, g_tap_inv, g_tap_miss, g_tap_take);
        CHECK(g_tap_at[T] == n, "%s: W380 Flanken von B in der Zeitlinie von A %u statt %u", name, g_tap_at[T], n);
        CHECK(n > 100u, "%s: W380 zu wenige Tipps (%u)", name, n);
        CHECK(g_tap_inv == 0u, "%s: W380 Vorhersage erfand %u Pedal-Flanken (live)", name, g_tap_inv);
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
    if (g_w385_capture) { g_w385_st[0] = *sa; g_w385_st[1] = *sb; } /* W385 */
    rbi_free(A.r);
    rbi_free(B.r);
}

/* W380 LOC-DODGE: Flanke bei Vorhersage und Neurechnen. Das Spiel startet den Seitenschritt an der Flanke des
 * linken Pedals im entnommenen Satz (Byte 5). B tippt das Pedal (1 bzw. 8 Rahmen je 13); A sagt B voraus
 * (letzter bestaetigter Satz) und rechnet nach Fehlvorhersagen neu. Pruefung: die endgueltige Zeitlinie von A
 * enthaelt jede Flanke von B genau einmal (Neurechnen holt sie nach, keine doppelt), live erfindet die
 * Vorhersage keine Flanke (sie wiederholt nur Halten), beide Seiten synchron. */
/* W381 (GitHub #61 "bleeping is not stopping"): Ton-Stopps beim Neurechnen. Modell von rb_xstop/rb_xstopsrc mit
 * rb_rep_get (R3): erste Rechnung (verworfene Zeitlinie) laeuft live und wird je Tick aufgezeichnet; beim Neurechnen
 * gilt ein Aufruf als erledigt, wenn das naechste aufgezeichnete Ereignis derselben Art denselben Schluessel hat,
 * sonst laeuft er live (Zeiger bleibt stehen). Pruefung: jeder Stopp der korrigierten Zeitlinie ist am Ende wirklich
 * ausgefuehrt (kein endloser Ton); gleiche Folgen stoppen nichts doppelt. Gegenprobe: alter Schluessel (ohne
 * Handle/Quelle) verliert Stopps. */
#include "loc_rb_xkey.h"
enum { XK_STOP = 0, XK_SRC = 1 };
typedef struct { int kind; u32 cue, flags, obj; } xk_call_t; /* obj = Handle bzw. Quelle */
static u32 xk_key(const xk_call_t *c, int legacy)
{
    return c->kind == XK_STOP ? loc_rb_xkey_stop(c->cue, c->flags, c->obj, legacy) : loc_rb_xkey_src(c->obj, legacy);
}
/* Rueckgabe: Zahl der Stopps der korrigierten Zeitlinie, die nie ausgefuehrt wurden; *nlive = live beim Neurechnen */
static int xk_tick(const xk_call_t *pred, int np, const xk_call_t *corr, int nc, int legacy, int *nlive)
{
    u32 done[64];
    int nd = 0, i, j, cur[2] = {0, 0}, lost = 0;
    *nlive = 0;
    for (i = 0; i < np; i++) done[nd++] = (u32)pred[i].kind << 31 | pred[i].obj | (pred[i].obj ? 0u : 0x40000000u | pred[i].cue);
    for (i = 0; i < nc; i++) {
        int k = corr[i].kind, hit = 0;
        for (j = cur[k]; j < np; j++) {
            if (pred[j].kind != k) continue;
            if (xk_key(&pred[j], legacy) == xk_key(&corr[i], legacy)) { cur[k] = j + 1; hit = 1; }
            break;
        }
        if (!hit) { (*nlive)++; if (nd < 64) done[nd++] = (u32)k << 31 | corr[i].obj | (corr[i].obj ? 0u : 0x40000000u | corr[i].cue); }
    }
    for (i = 0; i < nc; i++) {
        u32 want = (u32)corr[i].kind << 31 | corr[i].obj | (corr[i].obj ? 0u : 0x40000000u | corr[i].cue);
        for (j = 0; j < nd && done[j] != want; j++) {}
        if (j == nd) lost++;
    }
    return lost;
}
static void w381_xkey_cases(void)
{
    const u32 M1 = 0xFFFFFFFFu; /* Cue -1 = Stopp per Handle (so stoppt LoC, W379) */
    int lost, nl, legacy, r, lost_new = 0, lost_old = 0, dbl_new = 0;
    { /* 1: verworfene Zeitlinie stoppt den Motorton, korrigierte im selben Tick den Warnton */
        xk_call_t p[] = {{XK_STOP, M1, 0, 0x10A0}}, c[] = {{XK_STOP, M1, 0, 0x10C0}};
        lost = xk_tick(p, 1, c, 1, 0, &nl);
        CHECK(lost == 0 && nl == 1, "W381 Fall 1: Warnton-Stopp verloren=%d live=%d", lost, nl);
        lost = xk_tick(p, 1, c, 1, 1, &nl);
        CHECK(lost == 1, "W381 Gegenprobe Fall 1: alter Schluessel verlor den Stopp nicht (%d)", lost);
        printf("%-28s W381 Stopp anderer Handle im selben Tick: neu verloren 0, alt verloren %d\n", "Ton beim Neurechnen", lost);
    }
    { /* 2: gleiche Stopps in beiden Zeitlinien -> nichts doppelt live */
        xk_call_t p[] = {{XK_STOP, M1, 0, 0x2000}, {XK_STOP, 7, 0, 0}, {XK_SRC, 0, 0, 0x3000}},
                  c[] = {{XK_STOP, M1, 0, 0x2000}, {XK_STOP, 7, 0, 0}, {XK_SRC, 0, 0, 0x3000}};
        lost = xk_tick(p, 3, c, 3, 0, &nl);
        CHECK(lost == 0 && nl == 0, "W381 Fall 2: gleiche Folge verloren=%d live=%d (erwartet 0/0)", lost, nl);
    }
    { /* 3: Stopp nach Index (Handle 0) - Schluessel wie bisher */
        CHECK(loc_rb_xkey_stop(5, 1, 0, 0) == loc_rb_xkey_stop(5, 1, 0, 1), "W381 Fall 3: Index-Stopp-Schluessel geaendert");
        CHECK(loc_rb_xkey_stop(M1, 0, 0x1000, 0) != loc_rb_xkey_stop(M1, 0, 0x1040, 0), "W381 Fall 3: Handles gleich");
        CHECK(loc_rb_xkey_stop(M1, 0, 0x1000, 0) != loc_rb_xkey_stop(M1, 0, 0, 0), "W381 Fall 3: Handle wie Index");
    }
    { /* 4: StopSoundCues anderer 3D-Quelle */
        xk_call_t p[] = {{XK_SRC, 0, 0, 0x5000}}, c[] = {{XK_SRC, 0, 0, 0x5100}};
        lost = xk_tick(p, 1, c, 1, 0, &nl);
        CHECK(lost == 0, "W381 Fall 4: StopSoundCues der Quelle verloren (%d)", lost);
        lost = xk_tick(p, 1, c, 1, 1, &nl);
        CHECK(lost == 1, "W381 Gegenprobe Fall 4: alter Schluessel verlor nicht (%d)", lost);
    }
    /* 5: Zufallsfolgen (8 Handles, 2 Quellen, 0..4 Stopps je Zeitlinie und Tick) */
    for (legacy = 0; legacy < 2; legacy++) {
        s_rng = 381u;
        for (r = 0; r < 4000; r++) {
            xk_call_t p[4], c[4];
            int np = (int)(rnd() % 5u), nc = (int)(rnd() % 5u), i, same = (rnd() % 4u) == 0;
            for (i = 0; i < np; i++) {
                int src = (rnd() % 5u) == 0;
                p[i].kind = src ? XK_SRC : XK_STOP; p[i].cue = src ? 0 : M1; p[i].flags = 0;
                p[i].obj = src ? 0x7000u + (rnd() % 2u) * 0x40u : 0x1000u + (rnd() % 8u) * 0x40u;
            }
            if (same) { nc = np; memcpy(c, p, sizeof p); }
            else for (i = 0; i < nc; i++) {
                int src = (rnd() % 5u) == 0;
                c[i].kind = src ? XK_SRC : XK_STOP; c[i].cue = src ? 0 : M1; c[i].flags = 0;
                c[i].obj = src ? 0x7000u + (rnd() % 2u) * 0x40u : 0x1000u + (rnd() % 8u) * 0x40u;
            }
            lost = xk_tick(p, np, c, nc, legacy, &nl);
            if (legacy) lost_old += lost; else { lost_new += lost; if (same && nl) dbl_new++; }
        }
    }
    printf("%-28s W381 Zufall 4000 Ticks: verlorene Stopps neu %d, alt %d; gleiche Folge doppelt live %d\n",
           "Ton beim Neurechnen", lost_new, lost_old, dbl_new);
    CHECK(lost_new == 0, "W381 Zufall: %d Stopps verloren (neuer Schluessel)", lost_new);
    CHECK(dbl_new == 0, "W381 Zufall: gleiche Folge stoppte %d-mal doppelt", dbl_new);
    CHECK(lost_old > 0, "W381 Gegenprobe Zufall: alter Schluessel verlor keinen Stopp (Fall prueft nichts)");
}

/* W413 (GitHub #65 "sounds repeat endlessly online"): Schleifen-Cues nach mehreren, sich ueberlappenden Neurechnungen.
 * Modell von loc_rollback.c (rb_xcue_call/rb_rep_xcue/Nebentabelle, rb_xstop mit rb_rep_get je Art in Reihenfolge,
 * rbi_replay_finish, W375-Waisen) mit einem Spielzwilling: Feuer gedrueckt -> MG-Schleife starten (Handle in die Ablage
 * des Spiels), losgelassen -> Stopp per Handle, alle 37 Ticks Stopp nach Index (Ablage leer). Eingabe des Mitspielers in
 * kurzen Stoessen, Bestaetigung mit 1..6 Ticks Verzoegerung, Vorhersage = letzte bestaetigte Eingabe. Am Ende ist alles
 * bestaetigt: es darf genau die Schleife spielen, deren Handle die endgueltige Zeitlinie haelt. Gegenprobe ohne Fix:
 * endlose Schleifen (nachgeholte Waisen, erledigte Index-Stopps). Ohne Fehlvorhersage greift der Fix nie. */
#include "loc_rb_sndloop.h"
#define W4_T 700
#define W4_EV 16384
#define W4_MG 7
enum { W4_PLAY = 0, W4_STOPH, W4_STOPIDX };
typedef struct { int type; u32 key, h, serial; uint8_t pad; } w4_ev_t;
typedef struct { u32 serial; int cue, playing; } w4_inst_t;
typedef struct { u32 g, key, h, serial; int used; } w4_side_t;
typedef struct { u32 serial, h; int touched; uint16_t miss; } w4_net_t;
static struct {
    w4_ev_t ev[W4_EV];
    int nev, toff[W4_T + 2];
    w4_inst_t in[8192];
    int nin;
    w4_side_t side[512];
    int nside;
    w4_net_t net[64];
    int nnet;
    int fix, grace, replay, cur_t, kc[3], rep_end, win0, win1;
    u32 g0, g1, slot_hist[W4_T + 2];
    int n_live_start, n_live_stop, n_fix_stops;
} W4;
static w4_inst_t *w4_inst(u32 serial)
{
    int i;
    for (i = 0; i < W4.nin; i++) if (W4.in[i].serial == serial) return &W4.in[i];
    return NULL;
}
static u32 w4_start(int cue)
{
    w4_inst_t *x = &W4.in[W4.nin++];
    x->serial = (u32)W4.nin;
    x->cue = cue;
    x->playing = 1;
    return x->serial;
}
static int w4_stop_serial(u32 serial) /* 1 = gestoppt (spielte) */
{
    w4_inst_t *x = w4_inst(serial);
    if (!x || !x->playing) return 0;
    x->playing = 0;
    return 1;
}
static void w4_rec(int type, u32 key, u32 h, u32 serial)
{
    w4_ev_t *e = &W4.ev[W4.nev++];
    e->type = type; e->key = key; e->h = h; e->serial = serial; e->pad = 0;
}
static int w4_rep_get(int type, u32 key) /* rb_rep_get (R3): je Art in Reihenfolge, Abweichung = Zeiger bleibt */
{
    int c;
    for (c = W4.kc[type]; c < W4.rep_end; c++) {
        if (W4.ev[c].type != type) continue;
        if (W4.ev[c].key != key) break;
        W4.kc[type] = c + 1;
        return 1;
    }
    return 0;
}
static void w4_net_add(u32 serial, u32 h, int touched)
{
    if (W4.nnet < 64) { W4.net[W4.nnet].serial = serial; W4.net[W4.nnet].h = h; W4.net[W4.nnet].touched = touched; W4.net[W4.nnet].miss = 0; W4.nnet++; }
}
static u32 w4_play(int cue)
{
    int c, pass, i;
    u32 ser;
    if (!W4.replay) {
        ser = w4_start(cue);
        w4_rec(W4_PLAY, (u32)cue, ser, ser);
        if (W4.fix) w4_net_add(ser, ser, 0);
        return ser;
    }
    for (pass = 0; pass < 2; pass++) { /* rb_rep_xcue: ab dem laufenden Tick, dann vom Fensterbeginn */
        int a = pass ? W4.win0 : W4.toff[W4.cur_t], b = pass ? W4.toff[W4.cur_t] : W4.win1;
        for (c = a; c < b; c++) {
            w4_ev_t *e = &W4.ev[c];
            if (e->type == W4_PLAY && e->key == (u32)cue && !(e->pad & (LRS_PAD_USED | LRS_PAD_DEAD))) {
                e->pad |= LRS_PAD_USED;
                return e->h;
            }
        }
    }
    for (i = 0; i < W4.nside; i++) {
        w4_side_t *x = &W4.side[i];
        if (x->key == (u32)cue && !x->used && x->g >= W4.g0 && x->g < W4.g1) { x->used = 1; return x->h; }
    }
    ser = w4_start(cue); /* nachgeholt */
    W4.n_live_start++;
    if (W4.nside >= 512) return ser;
    W4.side[W4.nside].g = (u32)W4.cur_t; W4.side[W4.nside].key = (u32)cue; W4.side[W4.nside].h = ser;
    W4.side[W4.nside].serial = ser; W4.side[W4.nside].used = 1; W4.nside++;
    if (W4.fix) w4_net_add(ser, ser, 1);
    return ser;
}
static void w4_stop_h(u32 h)
{
    if (!W4.replay) { w4_stop_serial(h); w4_rec(W4_STOPH, h, 0, 0); return; }
    if (!w4_rep_get(W4_STOPH, h)) { W4.n_live_stop++; w4_stop_serial(h); }
}
static void w4_stop_idx(int cue)
{
    int i, c;
    if (!W4.replay || !w4_rep_get(W4_STOPIDX, 0x80000000u | (u32)cue)) {
        if (W4.replay) W4.n_live_stop++;
        for (i = 0; i < W4.nin; i++) if (W4.in[i].cue == cue) W4.in[i].playing = 0;
        if (!W4.replay) w4_rec(W4_STOPIDX, 0x80000000u | (u32)cue, 0, 0);
        return;
    }
    if (!W4.fix) return; /* erledigt (alt): nichts live */
    for (i = 0; i < W4.nside; i++) { /* rbs_stop_cover */
        w4_side_t *x = &W4.side[i];
        w4_inst_t *n = w4_inst(x->serial);
        if (lrs_side_in_timeline(x->g, x->used, W4.g0, (u32)W4.cur_t) && n && lrs_stop_covers(cue, 0, n->cue, 0))
            W4.n_fix_stops += w4_stop_serial(x->serial);
    }
    for (c = W4.win0; c < W4.win1; c++) {
        w4_ev_t *e = &W4.ev[c];
        w4_inst_t *n = w4_inst(e->serial);
        if (e->type == W4_PLAY && (e->pad & LRS_PAD_USED) && n && lrs_stop_covers(cue, 0, n->cue, 0))
            W4.n_fix_stops += w4_stop_serial(e->serial);
    }
}
static u32 w4_game_tick(int t, int fire, u32 s) /* Spielzwilling: Ablage s = Handle der MG-Schleife */
{
    if (t % 37 == 36 && s) { w4_stop_idx(W4_MG); return 0; }
    if (fire && !s) return w4_play(W4_MG);
    if (!fire && s) { w4_stop_h(s); return 0; }
    return s;
}
static void w4_replay(int m, int t, const int *inp, int conf, int *used_in)
{
    int k, c, i, w = 0;
    u32 s = m ? W4.slot_hist[m - 1] : 0;
    W4.replay = 1;
    W4.g0 = (u32)m; W4.g1 = (u32)t;
    W4.toff[t] = W4.nev;
    W4.win0 = W4.toff[m]; W4.win1 = W4.nev;
    for (c = W4.win0; c < W4.win1; c++) {
        W4.ev[c].pad &= (uint8_t)~LRS_PAD_USED;
        if (W4.ev[c].type == W4_PLAY)
            for (i = 0; i < W4.nnet; i++) if (W4.net[i].serial == W4.ev[c].serial) W4.net[i].touched = 1;
    }
    for (i = 0; i < W4.nside; i++) W4.side[i].used = 0;
    for (k = m; k < t; k++) {
        int in = k <= conf ? inp[k] : inp[conf];
        W4.cur_t = k;
        W4.kc[0] = W4.kc[1] = W4.kc[2] = W4.toff[k];
        W4.rep_end = W4.toff[k + 1];
        used_in[k] = in;
        s = w4_game_tick(k, in, s);
        W4.slot_hist[k] = s;
    }
    for (c = W4.win0; c < W4.win1; c++) { /* rbi_replay_finish: W375-Waisen, W413 tot markieren */
        w4_ev_t *e = &W4.ev[c];
        if (e->type == W4_PLAY && !(e->pad & (LRS_PAD_USED | LRS_PAD_DEAD)) && w4_stop_serial(e->serial) && W4.fix)
            e->pad |= LRS_PAD_DEAD;
        e->pad &= (uint8_t)~LRS_PAD_USED;
    }
    for (i = 0; i < W4.nside; i++) { /* W413: nachgeholte Waisen */
        w4_side_t *x = &W4.side[i];
        if (W4.fix && lrs_side_orphan(x->g, x->used, W4.g0, W4.g1) && w4_stop_serial(x->serial)) { W4.n_fix_stops++; continue; }
        W4.side[w++] = *x;
    }
    W4.nside = w;
    W4.replay = 0;
}
/* Ein Lauf: Rueckgabe Zahl der endlosen Schleifen am Ende (*missing = Handle der Zeitlinie spielt nicht) */
static int w4_run(u32 seed, int fix, int grace, int perfect, int *missing, int *rbs)
{
    static int inp[W4_T], used_in[W4_T];
    int t, conf = -1, k, endless = 0, i;
    u32 fin;
    memset(&W4, 0, sizeof W4);
    W4.fix = fix; W4.grace = grace;
    s_rng = seed;
    inp[0] = 0;
    for (t = 1; t < W4_T; t++) inp[t] = (rnd() % 100u) < 30u ? !inp[t - 1] : inp[t - 1];
    *rbs = 0;
    for (t = 0; t < W4_T; t++) {
        int nc = perfect ? t - 1 : t - 1 - (int)(rnd() % 6u), m = -1, in;
        if (nc > conf) {
            for (k = conf + 1; k <= nc && m < 0; k++) if (used_in[k] != inp[k]) m = k;
            if (m >= 0) { w4_replay(m, t, inp, nc, used_in); (*rbs)++; }
            conf = nc;
        }
        in = perfect || t <= conf ? inp[t] : (conf >= 0 ? inp[conf] : 0);
        W4.toff[t] = W4.nev;
        used_in[t] = in;
        W4.slot_hist[t] = w4_game_tick(t, in, t ? W4.slot_hist[t - 1] : 0);
        W4.toff[t + 1] = W4.nev;
        if (W4.fix) { /* Sicherheitsnetz je Live-Tick */
            int w = 0;
            for (i = 0; i < W4.nnet; i++) {
                w4_net_t *x = &W4.net[i];
                w4_inst_t *n = w4_inst(x->serial);
                if (!n || !n->playing) continue;
                if (x->touched && lrs_net_step(W4.slot_hist[t], x->h, &x->miss, (u32)W4.grace)) { W4.n_fix_stops += w4_stop_serial(x->serial); continue; }
                W4.net[w++] = *x;
            }
            W4.nnet = w;
        }
    }
    for (k = conf + 1, t = -1; k < W4_T && t < 0; k++) if (used_in[k] != inp[k]) t = k; /* alles bestaetigt */
    if (t >= 0) { w4_replay(t, W4_T, inp, W4_T - 1, used_in); (*rbs)++; }
    fin = W4.slot_hist[W4_T - 1];
    for (i = 0; i < W4.nin; i++) if (W4.in[i].playing && W4.in[i].serial != fin) endless++;
    *missing = fin && !(w4_inst(fin) && w4_inst(fin)->playing);
    return endless;
}
static void w413_sndloop_cases(void)
{
    int r, miss, rbs, e_old = 0, e_new = 0, e_nonet = 0, m_old = 0, m_new = 0, runs_old = 0, rb_tot = 0;
    for (r = 0; r < 200; r++) {
        int e = w4_run(4130u + (u32)r, 0, 0, 0, &miss, &rbs);
        e_old += e; m_old += miss; runs_old += e > 0; rb_tot += rbs;
        e_new += w4_run(4130u + (u32)r, 1, 30, 0, &miss, &rbs); m_new += miss;
        e_nonet += w4_run(4130u + (u32)r, 1, 0, 0, &miss, &rbs);
    }
    printf("%-28s W413 200 Laeufe (%d Neurechnungen): endlose Schleifen alt %d (in %d Laeufen), neu %d, neu ohne Netz %d; "
           "Ton fehlt alt %d neu %d\n", "Ton beim Neurechnen", rb_tot, e_old, runs_old, e_new, e_nonet, m_old, m_new);
    CHECK(e_old > 0, "W413 Gegenprobe: ohne Fix keine endlose Schleife (Strecke prueft nichts)");
    CHECK(e_new == 0, "W413: %d endlose Schleifen mit Fix", e_new);
    CHECK(e_nonet == 0, "W413: %d endlose Schleifen mit Fix ohne Sicherheitsnetz", e_nonet);
    CHECK(m_new <= m_old, "W413: mit Fix fehlt der Ton oefter (%d > %d)", m_new, m_old);
    { /* Ohne Fehlvorhersage: kein Neurechnen, Fix ohne jede Wirkung (gleiche Starts/Stopps) */
        int ea, eb, ma, mb, ra, rb2, nin_a, fs;
        ea = w4_run(4999u, 0, 0, 1, &ma, &ra); nin_a = W4.nin;
        eb = w4_run(4999u, 1, 30, 1, &mb, &rb2); fs = W4.n_fix_stops;
        CHECK(ra == 0 && rb2 == 0 && ea == 0 && eb == 0 && fs == 0 && nin_a == W4.nin && W4.n_live_start == 0,
              "W413: ohne Fehlvorhersage wirkt der Fix (Neurechnungen %d/%d, Stopps %d, Starts %d/%d)", ra, rb2, fs, nin_a, W4.nin);
    }
    { /* Einzelfall nachgeholte Waise: R1 startet die Schleife nachgeholt, R2 nicht mehr */
        static int inp1[W4_T], used1[W4_T];
        int fx;
        for (fx = 0; fx < 2; fx++) {
            int i, playing = 0;
            memset(&W4, 0, sizeof W4);
            memset(inp1, 0, sizeof inp1);
            memset(used1, 0, sizeof used1);
            W4.fix = fx;
            for (i = 0; i < 10; i++) { W4.toff[i] = W4.nev; W4.slot_hist[i] = w4_game_tick(i, 0, i ? W4.slot_hist[i - 1] : 0); W4.toff[i + 1] = W4.nev; }
            inp1[5] = 1; /* R1 (Neurechnen ab Tick 5, bestaetigt bis 5): Feuer an Tick 5, vorhergesagt gehalten */
            w4_replay(5, 10, inp1, 5, used1);
            inp1[5] = 0; /* R2 (Neurechnen ab Tick 4): Korrektur - nie gefeuert */
            w4_replay(4, 10, inp1, 9, used1);
            for (i = 0; i < W4.nin; i++) playing += W4.in[i].playing;
            if (fx) CHECK(playing == 0, "W413 Fall nachgeholte Waise: %d Schleife(n) spielen mit Fix", playing);
            else CHECK(playing == 1, "W413 Gegenprobe nachgeholte Waise: ohne Fix spielen %d (erwartet 1)", playing);
        }
    }
}

static void w380_cases(void)
{
    static const struct { int dab, dba, jit, loss, D, tap; const char *n; } k[] = {
        {3, 3, 3, 10, 2, 1, "W380 Tipp 1, 3+3, Verlust"},
        {3, 3, 3, 10, 2, 8, "W380 Tipp 8 (Hilfe), 3+3"},
        {2, 2, 2, 0, 1, 1, "W380 Tipp 1, 2+2, D=1"},
        {5, 5, 4, 5, 3, 2, "W380 Tipp 2, 5+5, D=3"},
    };
    scen_t s;
    int i;
    for (i = 0; i < (int)(sizeof k / sizeof k[0]); i++) {
        memset(&s, 0, sizeof s);
        s.delay_ab = k[i].dab;
        s.delay_ba = k[i].dba;
        s.jitter = k[i].jit;
        s.loss_pct = k[i].loss;
        s.D = k[i].D;
        s.maxpred = 10;
        s.ticks = 6000;
        g_tap = k[i].tap;
        g_tap_deff = k[i].D;
        g_tap_inv = g_tap_take = g_tap_miss = 0;
        scenario(k[i].n, &s, 1);
        CHECK(g_tap_miss > 0u, "%s: W380 keine Fehlvorhersage des Pedals (Fall prueft nichts)", k[i].n);
    }
    g_tap = 0;
}

/* W385 LOC-SMOOTH: Vorhersage fremder Achsen mit gedaempftem Trend. (a) rbi_predict rein: Modus 0 = Wiederholung wie
 * W338 (einmalige Flags geloescht), Modus 1 linear/gedaempft, Klemmung, Vorzeichen, Tasten/Pedale/Gang/Kopf halten,
 * ohne Vorgaenger = Wiederholung. (b) rbi_frame_cat. (c) Strecken mit Achsen-Rampen auf beiden Seiten in beiden Modi:
 * synchron (Hashes gleich, keine verlorenen Rollbacks), Trend senkt die Fehlvorhersagen; Vergleichszaehler. */
static void w385_cases(void)
{
    uint8_t a[RBI_FR], b[RBI_FR], o[RBI_FR], o2[RBI_FR];
    static const struct { int dab, dba, jit, D; const char *n; } k[] = {
        {3, 3, 2, 2, "W385 Rampen 3+3, D=2"},
        {4, 4, 3, 2, "W385 Rampen 4+4 j3, D=2"},
        {5, 5, 2, 3, "W385 Rampen 5+5, D=3"},
    };
    int i, m;
    memset(a, 0, sizeof a); memset(b, 0, sizeof b);
    a[0] = b[0] = 1;
    a[4] = 0x03; a[7] = 0xD0; /* einmalige Flags (0xD0000000) */
    b[4] = 0x03;
    a[RBI_HDR + 0] = 0x05; b[RBI_HDR + 0] = 0x05;   /* Tasten */
    a[RBI_HDR + 5] = 0xFF; b[RBI_HDR + 5] = 0x00;   /* linkes Pedal: Flanke in a */
    a[RBI_HDR + 8] = 2; b[RBI_HDR + 8] = 1;         /* Gang */
    a[RBI_HDR + 9] = 10; b[RBI_HDR + 9] = 5;        /* Drehhebel +5 je Slot */
    a[RBI_HDR + 10] = (uint8_t)(int8_t)-20; b[RBI_HDR + 10] = (uint8_t)(int8_t)-12; /* Zielhebel x -8 */
    a[RBI_HDR + 11] = 120; b[RBI_HDR + 11] = 100;   /* Zielhebel y +20 -> Klemmung */
    a[RBI_HDR + 12] = 7; b[RBI_HDR + 12] = 7;       /* Sicht x ruhig */
    a[RBI_HDR + 13] = (uint8_t)(int8_t)-128; b[RBI_HDR + 13] = (uint8_t)(int8_t)-100; /* Klemmung unten */
    rbi_predict(a, b, 1, 0, 6, o);
    CHECK(memcmp(o, a, 4) == 0 && o[7] == 0 && memcmp(o + RBI_HDR, a + RBI_HDR, RBI_IN) == 0, "W385 Modus 0 = Wiederholung");
    rbi_predict(a, NULL, 2, 1, 8, o);
    CHECK(memcmp(o + RBI_HDR, a + RBI_HDR, RBI_IN) == 0 && o[7] == 0, "W385 ohne Vorgaenger = Wiederholung");
    rbi_predict(a, b, 1, 1, 8, o); /* linear, ein Slot */
    CHECK((int8_t)o[RBI_HDR + 9] == 15 && (int8_t)o[RBI_HDR + 10] == -28 && (int8_t)o[RBI_HDR + 11] == 127 &&
          (int8_t)o[RBI_HDR + 12] == 7 && (int8_t)o[RBI_HDR + 13] == -128, "W385 linear m=1 (%d %d %d %d %d)",
          (int8_t)o[RBI_HDR + 9], (int8_t)o[RBI_HDR + 10], (int8_t)o[RBI_HDR + 11], (int8_t)o[RBI_HDR + 12], (int8_t)o[RBI_HDR + 13]);
    CHECK(o[RBI_HDR + 0] == 0x05 && o[RBI_HDR + 5] == 0xFF && o[RBI_HDR + 8] == 2 && o[7] == 0 && o[4] == 0x03,
          "W385 Tasten/Pedal/Gang halten, Flags geloescht");
    rbi_predict(a, b, 3, 1, 8, o);
    CHECK((int8_t)o[RBI_HDR + 9] == 25 && (int8_t)o[RBI_HDR + 10] == -44, "W385 linear m=3 (%d %d)", (int8_t)o[RBI_HDR + 9],
          (int8_t)o[RBI_HDR + 10]);
    rbi_predict(a, b, 1, 1, 6, o); /* 10 + 5 * 0,75 = 13,75 -> 14; -20 - 8 * 0,75 = -26 */
    CHECK((int8_t)o[RBI_HDR + 9] == 14 && (int8_t)o[RBI_HDR + 10] == -26, "W385 gedaempft m=1 (%d %d)", (int8_t)o[RBI_HDR + 9],
          (int8_t)o[RBI_HDR + 10]);
    rbi_predict(a, b, 2, 1, 6, o); /* 0,75 + 0,5625 = 1,3125: 10 + 6,5625 -> 17; -20 - 10,5 -> -31 (Rundung weg von 0) */
    CHECK((int8_t)o[RBI_HDR + 9] == 17 && (int8_t)o[RBI_HDR + 10] == -31, "W385 gedaempft m=2 (%d %d)", (int8_t)o[RBI_HDR + 9],
          (int8_t)o[RBI_HDR + 10]);
    for (m = 1; m <= 40; m++) { /* Grenze: gedaempfte Summe < q/(8-q) = 3, nie ueber Klemmung, monoton */
        rbi_predict(a, b, (u32)m, 1, 6, o);
        rbi_predict(a, b, (u32)(m + 1), 1, 6, o2);
        CHECK((int8_t)o[RBI_HDR + 9] <= 25 && (int8_t)o2[RBI_HDR + 9] >= (int8_t)o[RBI_HDR + 9], "W385 Grenze m=%d", m);
    }
    { /* Tastenflanke auf einer Achse (A/D -> Drehhebel 0 -> -127 bzw. zurueck): halten, nicht -128/+127 fortsetzen */
        uint8_t c[RBI_FR], d2[RBI_FR];
        memcpy(c, a, RBI_FR); memcpy(d2, b, RBI_FR);
        c[RBI_HDR + 9] = (uint8_t)(int8_t)-127; d2[RBI_HDR + 9] = 0;
        rbi_predict(c, d2, 1, 1, 8, o);
        CHECK((int8_t)o[RBI_HDR + 9] == -127, "W385 Flanke gedrueckt halten (%d)", (int8_t)o[RBI_HDR + 9]);
        c[RBI_HDR + 9] = 0; d2[RBI_HDR + 9] = (uint8_t)(int8_t)-127;
        rbi_predict(c, d2, 2, 1, 8, o);
        CHECK((int8_t)o[RBI_HDR + 9] == 0, "W385 Flanke losgelassen halten (%d)", (int8_t)o[RBI_HDR + 9]);
        c[RBI_HDR + 9] = 40; d2[RBI_HDR + 9] = (uint8_t)(int8_t)-8; /* d = 48: gerade noch Trend */
        rbi_predict(c, d2, 1, 1, 8, o);
        CHECK((int8_t)o[RBI_HDR + 9] == 88, "W385 Grenze d=48 (%d)", (int8_t)o[RBI_HDR + 9]);
    }
    CHECK(rbi_frame_cat(a, a) == 0u, "W385 Kategorie gleich");
    rbi_predict(a, b, 1, 0, 8, o);
    memcpy(o2, o, RBI_FR);
    o2[1] ^= 0x55; /* Sync-Byte zaehlt nicht */
    CHECK(rbi_frame_cat(o, o2) == 0u, "W385 Kategorie ohne Sync-Byte");
    o2[RBI_HDR + 10]++;
    CHECK(rbi_frame_cat(o, o2) == (1u << RBI_MIS_AXIS), "W385 Kategorie Achse");
    o2[RBI_HDR + 5] = 0;
    CHECK(rbi_frame_cat(o, o2) == ((1u << RBI_MIS_AXIS) | (1u << RBI_MIS_DODGE)), "W385 Kategorie Ausweichen");
    memcpy(o2, o, RBI_FR); o2[RBI_HDR + 0] ^= 1u;
    CHECK(rbi_frame_cat(o, o2) == (1u << RBI_MIS_FIRE), "W385 Kategorie Feuer");
    o2[RBI_HDR + 0] ^= 1u; o2[RBI_HDR + 0] ^= 8u; o2[RBI_HDR + 7] ^= 1u; o2[RBI_HDR + 8] ^= 1u; o2[6] ^= 1u;
    CHECK(rbi_frame_cat(o, o2) == ((1u << RBI_MIS_BTN) | (1u << RBI_MIS_PEDAL) | (1u << RBI_MIS_GEAR) | (1u << RBI_MIS_HDR)),
          "W385 Kategorie Tasten/Pedale/Gang/Kopf");
    { /* Einstellung ueberlebt rbi_begin */
        rbi_t *r = rbi_new();
        rbi_set_pred(r, 1, 6);
        rbi_begin(r, 3, 0, 2, 10);
        CHECK(rbi_pred_mode(r) == 1, "W385 Modus nach rbi_begin");
        rbi_free(r);
    }
    for (i = 0; i < (int)(sizeof k / sizeof k[0]); i++) { /* Modus: 0 Wiederholung, 1 Trend linear (q 8), 2 gedaempft (q 6) */
        scen_t s;
        u32 bad[3], pred[3], gain[3], loss[3], err[3], errr[3], cat_ax[3];
        int mode;
        for (mode = 0; mode < 3; mode++) {
            const rbi_stats_t *sa, *sb;
            memset(&s, 0, sizeof s);
            s.delay_ab = k[i].dab;
            s.delay_ba = k[i].dba;
            s.jitter = k[i].jit;
            s.D = k[i].D;
            s.maxpred = 10;
            s.ticks = 6000;
            g_ramp = 1;
            g_predm = mode ? 1 : 0;
            g_predq = mode == 2 ? 6 : 8;
            s_rng = 0x2545F491u + (u32)i; /* gleiche Netzstrecke in allen Modi */
            g_w385_capture = 1;
            scenario(k[i].n, &s, 1);
            g_w385_capture = 0;
            sa = &g_w385_st[0];
            sb = &g_w385_st[1];
            bad[mode] = sa->pred_bad + sb->pred_bad;
            pred[mode] = sa->predicted + sb->predicted;
            err[mode] = sa->mis_ax_err + sb->mis_ax_err;
            gain[mode] = sa->pred_gain + sb->pred_gain;
            loss[mode] = sa->pred_loss + sb->pred_loss;
            errr[mode] = sa->mis_ax_err_rep + sb->mis_ax_err_rep;
            cat_ax[mode] = sa->mis_cat[RBI_MIS_AXIS] + sb->mis_cat[RBI_MIS_AXIS];
        }
        printf("%-28s W385 Vorhersagen W/lin/gedaempft %u/%u/%u falsch %u/%u/%u | nur Trend richtig lin %u ged %u, nur "
               "Wiederholung lin %u ged %u | Achsenfehler je falschem Slot W %.1f lin %.1f ged %.1f\n", k[i].n, pred[0], pred[1],
               pred[2], bad[0], bad[1], bad[2], gain[1], gain[2], loss[1], loss[2], bad[0] ? (double)err[0] / bad[0] : 0.0,
               bad[1] ? (double)err[1] / bad[1] : 0.0, bad[2] ? (double)err[2] / bad[2] : 0.0);
        CHECK(bad[1] < bad[0], "%s: linearer Trend senkt die Fehlvorhersagen nicht (%u >= %u)", k[i].n, bad[1], bad[0]);
        CHECK(gain[1] > loss[1], "%s: linearer Trend trifft seltener als die Wiederholung (%u <= %u)", k[i].n, gain[1], loss[1]);
        CHECK(cat_ax[0] > 0u && gain[0] == 0u && loss[0] == 0u, "%s: Modus 0 Vergleichszaehler", k[i].n);
        (void)errr;
    }
    g_ramp = 0;
    g_predm = 0;
}

/* W375 LOC-ROUND2: Missionswechsel im selben Prozess. Drei Missionen hintereinander auf DENSELBEN Instanzen (rbi_t,
 * Kanal-Warteschlange); zwischen den Missionen "Menue" (nichts entnommen), Pakete der alten Mission noch unterwegs.
 * Je Mission: neue Epoche, frischer Spielstand (Mission neu geladen), Vorlauf, Rollbacks (B->A spaet), endgueltige
 * Hashes beider Seiten gleich, kein Dauerstau. Gegenprobe: Mission 2 ohne Neubeginn (alter Zustand der Eingabeschicht,
 * Slots/Zaehler laufen weiter) staut bzw. weicht ab. */
static int mswitch_run(int fresh, u32 *ticks_m, u32 *cmp_m, u32 *diff_m, u32 *rb_m)
{
    static inst_t A, B;
    scen_t sc;
    int m;
    u32 t, s;
    memset(&sc, 0, sizeof sc);
    sc.D = 2;
    sc.maxpred = 10;
    sc.delay_ab = 1;
    sc.delay_ba = 4; /* A sagt B voraus -> Rollbacks */
    sc.jitter = 1;
    memset(s_q, 0, sizeof s_q);
    g_alloc = 0;
    g_expect_bad = !fresh;
    inst_init(&A, 0, &sc);
    inst_init(&B, 1, &sc);
    for (m = 0; m < 3; m++) {
        u32 a0, b0, cmp = 0, diff = 0, rb0;
        if (m > 0) {
            inst_t *X[2] = {&A, &B};
            int i, k;
            for (i = 0; i < 2; i++) { /* Missionsende (Vorlauf der naechsten Mission = rbi_mission_end + Neubeginn) */
                rbi_t *keep = X[i]->r;
                if (fresh) {
                    rbi_end(keep);
                    rbi_begin(keep, 7u + (u32)m * 0x10000u, i, sc.D, sc.maxpred);
                }
                memset(X[i], 0, sizeof *X[i]);
                X[i]->me = i;
                X[i]->r = keep;
                X[i]->p0 = 4;
                for (k = 0; k < X[i]->p0; k++) own_frame(X[i], 1);
                X[i]->snap[0] = X[i]->st;
            }
        }
        a0 = (u32)rbi_cons(A.r, 0);
        b0 = (u32)rbi_cons(B.r, 1);
        rb0 = rbi_stats(A.r)->rb_plans + rbi_stats(B.r)->rb_plans;
        g_rx_old = m > 0;
        for (t = 0; t < 1500; t++) {
            u32 now = (u32)m * 100000u + t;
            if (t == 40) g_rx_old = 0; /* alte Pakete sind zugestellt */
            do {
                toy_tick(&A);
                toy_rest(&A, now, &sc, &B, sc.delay_ab);
            } while (rbi_replaying(A.r));
            do {
                toy_tick(&B);
                toy_rest(&B, now, &sc, &A, sc.delay_ba);
            } while (rbi_replaying(B.r));
        }
        for (s = 0; s < MAXS; s++) {
            if (A.fin_has[s] && B.fin_has[s]) {
                cmp++;
                if (A.fin_acc[s] != B.fin_acc[s]) diff++;
            }
        }
        ticks_m[m] = (u32)rbi_cons(A.r, 0) - a0 < (u32)rbi_cons(B.r, 1) - b0 ? (u32)rbi_cons(A.r, 0) - a0
                                                                              : (u32)rbi_cons(B.r, 1) - b0;
        cmp_m[m] = cmp;
        diff_m[m] = diff;
        rb_m[m] = rbi_stats(A.r)->rb_plans + rbi_stats(B.r)->rb_plans - rb0;
    }
    g_rx_old = 0;
    g_expect_bad = 0;
    rbi_free(A.r);
    rbi_free(B.r);
    return 0;
}

static void mission_switch_tests(void)
{
    u32 tk[3], cmp[3], diff[3], rb[3];
    int m;
    g_rx_old_rej = 0;
    mswitch_run(1, tk, cmp, diff, rb);
    for (m = 0; m < 3; m++) {
        printf("W375 Missionswechsel: Mission %d Slots %u, verglichen %u, Abw. %u, Rollbacks %u\n", m + 1, tk[m], cmp[m],
               diff[m], rb[m]);
        CHECK(tk[m] > 1400u, "W375: Mission %d staut (%u Slots in 1500 Ticks)", m + 1, tk[m]);
        CHECK(cmp[m] > 1000u && diff[m] == 0, "W375: Mission %d nicht synchron (%u/%u)", m + 1, diff[m], cmp[m]);
        CHECK(rb[m] > 0, "W375: Mission %d ohne Rollback (Strecke zu harmlos)", m + 1);
    }
    CHECK(g_rx_old_rej > 0, "W375: keine alten Pakete beim Missionswechsel (Strecke ohne Altlast)");
    mswitch_run(0, tk, cmp, diff, rb); /* Gegenprobe: ohne Neubeginn */
    printf("W375 Gegenprobe ohne Neubeginn: Mission 2 Slots %u, verglichen %u, Abw. %u\n", tk[1], cmp[1], diff[1]);
    CHECK(tk[1] < 1400u || diff[1] > 0 || cmp[1] < 1000u, "W375: Gegenprobe ohne Neubeginn zeigte keinen Fehler");
}

/* W366 LOC-NET-EDGES: B verlaesst die Mission bei Tick 1500 (sendet nichts mehr), das Spiel bei A entnimmt danach nur
 * noch eigene Rahmen (Aktiv-Maske ohne B). Ohne rbi_peer_gone haelt die Ringtiefe A fest (offene Vorhersagen fuer B
 * werden nie bestaetigt: Stau fuer immer, Lauf E2a); mit rbi_peer_gone laeuft A weiter. Rueckgabe: Ticks von A danach. */
static u32 leave_case(int gone)
{
    static inst_t A, B;
    scen_t sc;
    u32 t, c0 = 0;
    memset(&sc, 0, sizeof sc);
    sc.D = 2;
    sc.maxpred = 10;
    sc.ticks = 3000;
    sc.delay_ab = 1; /* B->A spaet: A sagt B voraus (offene Vorhersagen beim Abgang) */
    sc.delay_ba = 9;
    memset(s_q, 0, sizeof s_q);
    g_alloc = 0;
    g_expect_bad = !gone;
    inst_init(&A, 0, &sc);
    inst_init(&B, 1, &sc);
    for (t = 0; t < sc.ticks; t++) {
        if (t == 1500) {
            int k;
            for (k = 0; k < QN; k++) { /* B stuerzt ab: seine Pakete unterwegs gehen verloren (A hat vorhergesagt) */
                if (s_q[k].used && s_q[k].to == 0) s_q[k].used = 0;
            }
            g_need_mask = 1u;
            if (gone) rbi_peer_gone(A.r, 1);
            c0 = (u32)rbi_cons(A.r, 0);
        }
        do {
            toy_tick(&A);
            toy_rest(&A, t, &sc, &B, sc.delay_ab);
        } while (rbi_replaying(A.r));
        if (t < 1500) {
            do {
                toy_tick(&B);
                toy_rest(&B, t, &sc, &A, sc.delay_ba);
            } while (rbi_replaying(B.r));
        }
    }
    g_need_mask = 0;
    g_expect_bad = 0;
    CHECK(rbi_rx_count(A.r, 1) > 100u, "W366: Paketzaehler des Mitspielers (%u)", rbi_rx_count(A.r, 1));
    CHECK(rbi_gone_mask(A.r) == (gone ? 2u : 0u), "W366: Abgangsmaske 0x%X", rbi_gone_mask(A.r));
    return (u32)rbi_cons(A.r, 0) - c0;
}

static void leave_tests(void)
{
    u32 with = leave_case(1), without = leave_case(0);
    /* Die Gegenprobe staut im Spielmodell nicht (es bleiben beim Abgang keine offenen Vorhersagen ueber der
     * Ringtiefe); den Stau ohne Behebung belegt der Spiellauf E2a (BERICHT_M2 W366). Hier nur Info. */
    printf("W366 Abgang in der Mission: A nach dem Abgang von B %u Ticks weiter (mit rbi_peer_gone), %u ohne (Info)\n",
           with, without);
    CHECK(with >= 1400u, "W366: A laeuft nach dem Abgang nicht weiter (%u Ticks)", with);
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
    int self[2];      /* W370: Spielerindex im Hallo (-1 = Lobby/unbekannt) */
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
                len = rbn_tx(N->n[s], N->now, N->self[s], b, (int)sizeof b);
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
    N->self[0] = 0;
    N->self[1] = 1;
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

/* W370 LOC-RB-FALLBACK-US: erster Menschentest (DE <-> US-Westkueste, ~190 ms) fiel auf Lockstep zurueck. */
static void w370_cases(void)
{
    nnet_t N;
    int st, k;
    char d[400];
    printf("Aushandlung W370 (hohe RTT, Platzluecke, verspaeteter Gast, Hallo-Luecke, veralteter Index)\n");
    /* Platzluecke: Gast nach Wiederbeitritt auf Platz 2 von 3. Alte Zaehlung (Plaetze - 1 = 2) -> Lockstep (Nutzerfund) */
    nn_init(&N, 1, 1, 95, 0);
    N.self[1] = -1;
    nn_step(&N, 3000);
    st = rbn_mission_start(N.n[0], N.now, 0, 2);
    CHECK(st == RBN_ST_LS && rbn_why(N.n[0]) == RBN_WHY_PEER_NO, "Platzluecke alt: 2 erwartet, 1 da -> Lockstep (%d)", st);
    N.self[1] = 2;
    st = rbn_mission_start(N.n[1], N.now, 2, 1);
    CHECK(st == RBN_ST_PEND, "Platzluecke: Gast wartet (%d)", st);
    nn_step(&N, 1500);
    CHECK(rbn_state(N.n[1]) == RBN_ST_LS && rbn_why(N.n[1]) == RBN_WHY_HOST_LS && rbn_host_why(N.n[1]) == RBN_WHY_PEER_NO,
          "Gast: Host entschied Lockstep, Grund des Hosts im Hallo (%d/%d/%d)", rbn_state(N.n[1]), rbn_why(N.n[1]),
          rbn_host_why(N.n[1]));
    rbn_peers_desc(N.n[0], N.now, d, (int)sizeof d);
    CHECK(strstr(d, "Platz 2 Wunsch 1 bestaetigt 1") != NULL, "Mitspieler-Text (%s)", d);
    nn_free(&N);
    /* Platzluecke neu: verbundene Mitspieler (1) gezaehlt, Gast Index 2 -> R2 */
    nn_init(&N, 1, 1, 95, 0);
    N.self[1] = -1;
    nn_step(&N, 3000);
    st = rbn_mission_start(N.n[0], N.now, 0, 1);
    N.self[1] = 2;
    nn_step(&N, 400);
    rbn_mission_start(N.n[1], N.now, 2, 1);
    nn_step(&N, 3000);
    CHECK(st == RBN_ST_TENT && rbn_state(N.n[0]) == RBN_ST_R2 && rbn_state(N.n[1]) == RBN_ST_R2 &&
              rbn_epoch(N.n[0]) == rbn_epoch(N.n[1]) && rbn_delay(N.n[0]) == 3,
          "Platzluecke neu: beide R2, D=3 (%d/%d D%d)", rbn_state(N.n[0]), rbn_state(N.n[1]), rbn_delay(N.n[0]));
    nn_free(&N);
    /* hohe RTT (300/500 ms) + langsamer Gast (beginnt 3 s bzw. 9 s spaeter) -> R2 */
    for (k = 0; k < 4; k++) {
        static const u32 owd[4] = {150, 250, 150, 250}, late[4] = {3000, 3000, 9000, 9000};
        nn_init(&N, 1, 1, owd[k], 0);
        N.self[1] = -1;
        nn_step(&N, 4000);
        rbn_mission_start(N.n[0], N.now, 0, 1);
        nn_step(&N, late[k]);
        N.self[1] = 1;
        rbn_mission_start(N.n[1], N.now, 1, 1);
        nn_step(&N, 3000);
        CHECK(rbn_state(N.n[0]) == RBN_ST_R2 && rbn_state(N.n[1]) == RBN_ST_R2 && rbn_epoch(N.n[0]) == rbn_epoch(N.n[1]) &&
                  rbn_delay(N.n[0]) == 3 && rbn_delay(N.n[1]) == 3,
              "RTT %u ms, Gast %u ms spaeter: beide R2 (%d/%d)", 2 * owd[k], late[k], rbn_state(N.n[0]), rbn_state(N.n[1]));
        nn_free(&N);
    }
    /* Gast viel zu spaet (> Frist): beide Lockstep, einig */
    nn_init(&N, 1, 1, 100, 0);
    nn_step(&N, 3000);
    rbn_mission_start(N.n[0], N.now, 0, 1);
    nn_step(&N, RBN_WAIT_MS + 1000);
    rbn_mission_start(N.n[1], N.now, 1, 1);
    nn_step(&N, RBN_WAIT_MS + 1000);
    CHECK(rbn_state(N.n[0]) == RBN_ST_LS && rbn_state(N.n[1]) == RBN_ST_LS, "Gast > Frist: beide Lockstep (%d/%d)",
          rbn_state(N.n[0]), rbn_state(N.n[1]));
    nn_free(&N);
    /* Hallo-Luecke beim Host (Gast-Hallos 3,5 s verloren, z. B. Stau beim langsamen Gast) -> Nachfrist, dann R2 */
    nn_init(&N, 1, 1, 100, 0);
    nn_step(&N, 3000);
    N.drop_to[0] = 1;
    nn_step(&N, 3500);
    st = rbn_mission_start(N.n[0], N.now, 0, 1);
    CHECK(st == RBN_ST_TENT && rbn_host_waiting(N.n[0]), "Hallo-Luecke: Host wartet (Nachfrist) (%d)", st);
    nn_step(&N, 1000);
    N.drop_to[0] = 0;
    rbn_mission_start(N.n[1], N.now, 1, 1);
    nn_step(&N, 3000);
    CHECK(rbn_state(N.n[0]) == RBN_ST_R2 && rbn_state(N.n[1]) == RBN_ST_R2 && rbn_epoch(N.n[0]) == rbn_epoch(N.n[1]),
          "Hallo-Luecke: beide R2 (%d/%d)", rbn_state(N.n[0]), rbn_state(N.n[1]));
    nn_free(&N);
    /* Hallo-Luecke dauerhaft -> nach der Nachfrist Lockstep, Gast erfaehrt den Grund */
    nn_init(&N, 1, 1, 100, 0);
    nn_step(&N, 3000);
    N.drop_to[0] = 1;
    nn_step(&N, 3500);
    rbn_mission_start(N.n[0], N.now, 0, 1);
    rbn_mission_start(N.n[1], N.now, 1, 1);
    nn_step(&N, RBN_GRACE_MS + 1500);
    CHECK(rbn_state(N.n[0]) == RBN_ST_LS && rbn_why(N.n[0]) == RBN_WHY_PEER_NO && rbn_state(N.n[1]) == RBN_ST_LS &&
              rbn_host_why(N.n[1]) == RBN_WHY_PEER_NO,
          "Hallo-Luecke dauerhaft: beide Lockstep mit Grund (%d/%d %d/%d)", rbn_state(N.n[0]), rbn_why(N.n[0]),
          rbn_state(N.n[1]), rbn_host_why(N.n[1]));
    nn_free(&N);
    /* Gast meldet in der Lobby noch Index 0 (war vorher Host) -> Nachfrist, mit Missionsbeginn Index 1 -> R2 */
    nn_init(&N, 1, 1, 95, 0);
    N.self[1] = 0;
    nn_step(&N, 3000);
    st = rbn_mission_start(N.n[0], N.now, 0, 1);
    CHECK(st == RBN_ST_TENT && rbn_host_waiting(N.n[0]), "Index 0 beim Gast: Host wartet (%d)", st);
    nn_step(&N, 500);
    N.self[1] = 1;
    rbn_mission_start(N.n[1], N.now, 1, 1);
    nn_step(&N, 3000);
    CHECK(rbn_state(N.n[0]) == RBN_ST_R2 && rbn_state(N.n[1]) == RBN_ST_R2, "Index 0 beim Gast: beide R2 (%d/%d)",
          rbn_state(N.n[0]), rbn_state(N.n[1]));
    nn_free(&N);
    /* Community-Paket: Rollback aus (loc_rb_input=0, Standard ohne Launcher) beim Gast -> Host sofort Lockstep (keine
     * Nachfrist, der Gast sendet nie ein Hallo); der Launcher setzt beim Online-Start loc_rb_input=1 */
    nn_init(&N, 1, 0, 95, 0);
    nn_step(&N, 3000);
    st = rbn_mission_start(N.n[0], N.now, 0, 1);
    CHECK(st == RBN_ST_LS && !rbn_host_waiting(N.n[0]) && rbn_why(N.n[0]) == RBN_WHY_PEER_NO, "Gast ohne Rollback: sofort (%d)",
          st);
    nn_free(&N);
    /* Byte 27 (Grund): nur bei Lockstep-Entscheidung des Hosts gueltig */
    {
        rbn_t *a = rbn_new(1, 0x31310001u, 2, 3, 0), *b = rbn_new(1, 0x42420001u, 2, 3, 0);
        uint8_t p[RBN_PKT_MAX], q[RBN_PKT_MAX];
        int n;
        n = rbn_tx(b, 5000, -1, p, (int)sizeof p);
        CHECK(rbn_rx(a, 5000, p, n, 7) == 1, "Hallo B");
        memcpy(q, p, (size_t)n);
        q[27] = 1;
        CHECK(rbn_rx(a, 5001, q, n, 7) == -1, "Grund ohne Entscheidung verworfen");
        n = rbn_tx(a, 5010, 0, p, (int)sizeof p);
        CHECK(rbn_rx(b, 5010, p, n, 3) == 1, "Hallo A");
        CHECK(rbn_mission_start(a, 5100, 0, 1) == RBN_ST_TENT && rbn_host_waiting(a), "bekannt, unbestaetigt: Nachfrist");
        CHECK(rbn_poll(a, 5100 + RBN_GRACE_MS + 10) == RBN_ST_LS && rbn_why(a) == RBN_WHY_PEER_NO, "Nachfrist -> Lockstep");
        n = rbn_tx(a, 9200, 0, p, (int)sizeof p);
        CHECK(p[27] == RBN_WHY_PEER_NO && p[24] == RBN_MODE_LS, "Grund im Hallo (%u)", p[27]);
        CHECK(rbn_rx(b, 9200, p, n, 3) == 1, "Lockstep-Entscheidung mit Grund angenommen");
        memcpy(q, p, (size_t)n);
        q[27] = RBN_WHY_N;
        CHECK(rbn_rx(b, 9201, q, n, 3) == -1, "Grund ausserhalb verworfen");
        rbn_free(a);
        rbn_free(b);
    }
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
    CHECK(st == RBN_ST_PEND, "Client ohne frisches Host-Hallo wartet (W370 Nachfrist) (%d)", st);
    nn_step(&N, RBN_GRACE_MS + 1000);
    CHECK(rbn_state(N.n[1]) == RBN_ST_LS && rbn_why(N.n[1]) == RBN_WHY_HOST_NO, "Client nach Nachfrist Lockstep (%d/%d)",
          rbn_state(N.n[1]), rbn_why(N.n[1]));
    CHECK(rbn_state(N.n[0]) == RBN_ST_LS && rbn_why(N.n[0]) == RBN_WHY_PEER_LS, "Host faellt auf Lockstep zurueck (%d/%d)",
          rbn_state(N.n[0]), rbn_why(N.n[0]));
    nn_free(&N);
    w370_cases();
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

/* ---- W372 LOC-RB-TIMING: Zeitstrecke mit Feldraster ---- */
#define W372_FIELD 16667u
typedef struct {
    const char *name;
    int D;
    u32 ab_ms, ba_ms, jit_ms; /* Weg A->B, B->A, Schwankung */
    u32 phase_us, start_b_ms; /* Feldphase von B, B startet spaeter (Startversatz) */
    u32 field_b;              /* Feldlaenge von B in us (Drift; 0 = wie A) */
    int tsync, tt, late_send;
    u32 ticks;
} w372_t;

static int g_w372_cap;            /* W386: Zaehler der Zeitstrecke sichern */
static rbi_stats_t g_w372_st[2];
static void w372_run(const w372_t *c, double *late_a, double *late_b, double *ms_a, double *ms_b, u32 *act)
{
    static inst_t A, B;
    scen_t sc;
    u32 tA = 0, tB, nA = 0, nB = 0, half_a = 0, half_b = 0, ln_a0 = 0, ln_b0 = 0, lt_a0 = 0, lt_b0 = 0;
    u32 fb = c->field_b ? c->field_b : W372_FIELD;
    double sum_a = 0, sum_b = 0;
    u32 cnt_a = 0, cnt_b = 0;
    const rbi_stats_t *sa, *sb;
    memset(&sc, 0, sizeof sc);
    memset(s_q, 0, sizeof s_q);
    sc.D = c->D;
    sc.maxpred = 10;
    sc.jitter = (int)(c->jit_ms * 1000u);
    g_alloc = 0;
    g_tclock = 1;
    g_tt = c->tt;
    g_sendlag = c->late_send ? 3u * W372_FIELD : 0u;
    inst_init(&A, 0, &sc);
    inst_init(&B, 1, &sc);
    tB = c->start_b_ms * 1000u + c->phase_us;
    while (nA < c->ticks || nB < c->ticks) {
        inst_t *X, *P;
        u32 now, *nx, *tx, fl, del;
        int ex;
        if (nB >= c->ticks || (nA < c->ticks && tA <= tB)) {
            X = &A; P = &B; now = tA; tx = &tA; nx = &nA; fl = W372_FIELD; del = c->ab_ms * 1000u;
        } else {
            X = &B; P = &A; now = tB; tx = &tB; nx = &nB; fl = fb; del = c->ba_ms * 1000u;
        }
        q_deliver(X, now, P); /* Empfang vor der Entnahme (wie rbi_poll in FUN_0004E920) */
        rbi_clock(X->r, now);
        do {
            toy_tick(X);
            toy_rest(X, now, &sc, P, (int)del);
        } while (rbi_replaying(X->r));
        ex = c->tsync ? rbi_tsync(X->r) : 0;
        *tx = now + (3u + (u32)ex) * fl;
        (*nx)++;
        if (*nx == c->ticks / 2) { /* zweite Haelfte auswerten (nach dem Einschwingen) */
            if (X == &A) { half_a = 1; ln_a0 = rbi_stats(A.r)->slack_n; lt_a0 = rbi_stats(A.r)->slack_late; }
            else { half_b = 1; ln_b0 = rbi_stats(B.r)->slack_n; lt_b0 = rbi_stats(B.r)->slack_late; }
        }
        if (*nx > c->ticks / 2) {
            if (X == &A) { sum_a += rbi_tsync_ms(A.r); cnt_a++; }
            else { sum_b += rbi_tsync_ms(B.r); cnt_b++; }
        }
    }
    sa = rbi_stats(A.r);
    sb = rbi_stats(B.r);
    *late_a = half_a && sa->slack_n > ln_a0 ? 100.0 * (sa->slack_late - lt_a0) / (sa->slack_n - ln_a0) : 100.0;
    *late_b = half_b && sb->slack_n > ln_b0 ? 100.0 * (sb->slack_late - lt_b0) / (sb->slack_n - ln_b0) : 100.0;
    *ms_a = cnt_a ? sum_a / cnt_a : 0;
    *ms_b = cnt_b ? sum_b / cnt_b : 0;
    *act = sa->ts_fields + sb->ts_fields;
    printf("W372 %-34s spaet A/B %5.1f%%/%5.1f%% | Puffer p50 A/B %d/%d ms | Vorsprung A/B %6.1f/%6.1f ms | Felder+ A/B %u/%u "
           "| SBRT rx %u/%u | Rollbacks %u/%u verloren %u/%u\n",
           c->name, *late_a, *late_b, rbi_slack_pct(sa->slack_h, sa->slack_n, 50), rbi_slack_pct(sb->slack_h, sb->slack_n, 50),
           *ms_a, *ms_b, sa->ts_fields, sb->ts_fields, sa->tt_rx, sb->tt_rx, sa->rb_plans, sb->rb_plans, sa->rb_lost, sb->rb_lost);
    CHECK(sa->rb_lost == 0 && sb->rb_lost == 0, "%s: verlorene Rollbacks", c->name);
    CHECK(sa->hash_bad == 0 && sb->hash_bad == 0, "%s: Desync", c->name);
    CHECK(A.sync_bad == 0 && B.sync_bad == 0, "%s: Sync-Byte", c->name);
    CHECK(sa->hash_cmp > c->ticks / 4, "%s: Hash-Austausch zu selten (%u)", c->name, sa->hash_cmp);
    if (g_w372_cap) { g_w372_st[0] = *sa; g_w372_st[1] = *sb; } /* W386 */
    rbi_free(A.r);
    rbi_free(B.r);
    g_tclock = g_tt = 0;
    g_sendlag = 0;
}

static void w372_tt_tests(void)
{
    rbi_t *a = rbi_new(), *b = rbi_new();
    uint8_t p[RBI_PKT_MAX], t[RBI_TT_LEN + 8], m[RBI_TT_LEN + 8];
    uint8_t hdr[RBI_HDR] = {1, 0, 0, 0, 0x03, 0, 0, 0}, in[RBI_IN] = {0};
    int n, k, i, len;
    u32 s;
    rbi_begin(a, 9, 0, 2, 10);
    rbi_begin(b, 9, 1, 2, 10);
    rbi_clock(a, 1000);
    rbi_clock(b, 1000);
    for (k = 0; k < 4; k++) { rbi_own(a, hdr, in); rbi_own(b, hdr, in); }
    CHECK(rbi_tt_tx(a, t, (int)sizeof t) == 0, "SBRT ohne Messwerte gesendet");
    /* B nimmt Pakete von A an (bindet A), misst Puffer: A sendet frueh, B entnimmt 30 ms spaeter */
    for (s = 0; s < 40; s++) {
        uint8_t o[RBI_FR];
        u32 now = 100000u + s * 50000u;
        rbi_own(a, hdr, in);
        (void)rbi_take(a, 0, 0, o);
        n = rbi_tx(a, p, (int)sizeof p);
        rbi_clock(b, now);
        if (n > 0) CHECK(rbi_rx(b, p, n, 77) == 1, "SBR2 abgelehnt");
        rbi_clock(b, now + 30000u);
        (void)rbi_take(b, 0, 0, o);
        (void)rbi_take(b, 1, 0, o);
        rbi_own(b, hdr, in);
        (void)rbi_tick_end(a, NULL);
        (void)rbi_tick_end(b, NULL);
    }
    CHECK(rbi_slack_med(b, 0, &k) && k >= 790 && k <= 810, "Puffer-Median %d (erwartet ~800 = 30 ms + 1 Tick Vorlauf)", k);
    len = rbi_tt_tx(b, t, (int)sizeof t);
    CHECK(len == 20, "SBRT-Laenge %d", len);
    /* A: SBRT erst nach Bindung (SBR2 von B) */
    CHECK(rbi_tt_rx(a, t, len, 88) == -1, "SBRT ohne Bindung angenommen");
    n = rbi_tx(b, p, (int)sizeof p);
    CHECK(n > 0 && rbi_rx(a, p, n, 88) == 1, "SBR2 von B abgelehnt");
    CHECK(rbi_tt_rx(a, t, len, 88) == 1, "SBRT abgelehnt");
    CHECK(rbi_tt_rx(a, t, len, 89) == -1, "SBRT von fremder Adresse angenommen");
    /* Grenzfaelle */
    memcpy(m, t, (size_t)len); m[4] = 2; CHECK(rbi_tt_rx(a, m, len, 88) == -1, "SBRT ver 2 angenommen");
    memcpy(m, t, (size_t)len); m[7] = 1; CHECK(rbi_tt_rx(a, m, len, 88) == -1, "SBRT Fuellbyte angenommen");
    memcpy(m, t, (size_t)len); m[6] = 0; CHECK(rbi_tt_rx(a, m, 16, 88) == -1, "SBRT n=0 angenommen");
    memcpy(m, t, (size_t)len); CHECK(rbi_tt_rx(a, m, len - 1, 88) == -1, "SBRT kurz angenommen");
    memcpy(m, t, (size_t)len); m[5] = 0; CHECK(rbi_tt_rx(a, m, len, 88) == -1, "SBRT vom eigenen Index angenommen");
    memcpy(m, t, (size_t)len); m[5] = RBI_MAXP; CHECK(rbi_tt_rx(a, m, len, 88) == -1, "SBRT Index zu gross");
    memcpy(m, t, (size_t)len); m[18] = 0xB9; m[19] = 0x0B; CHECK(rbi_tt_rx(a, m, len, 88) == -1, "SBRT Puffer 3001 angenommen");
    memcpy(m, t, (size_t)len); m[17] = 1; CHECK(rbi_tt_rx(a, m, len, 88) == -1, "SBRT Eintrag-Fuellbyte angenommen");
    memcpy(m, t, (size_t)len); m[8] ^= 1; CHECK(rbi_tt_rx(a, m, len, 88) == 0, "SBRT andere Epoche nicht ignoriert");
    CHECK(rbi_rx(a, t, len, 88) == -1, "SBRT als SBR2 angenommen (aeltere Clients verwerfen es)");
    for (i = 0; i < 20000; i++) { /* Fuzz: nie Absturz, nur gueltige Werte uebernommen */
        int l = (int)(rnd() % (RBI_TT_LEN + 4));
        memcpy(m, t, (size_t)len);
        for (k = 0; k < 3; k++) m[rnd() % (u32)(RBI_TT_LEN + 4)] = (uint8_t)rnd();
        if (rnd() & 1) l = len;
        (void)rbi_tt_rx(a, m, l, 88);
        (void)rbi_rx(a, m, l, 88);
    }
    CHECK(rbi_stats(a)->tt_bad > 1000, "SBRT-Fuzz: zu wenige verworfen");
    rbi_free(a);
    rbi_free(b);
}

static void w372_cases(void)
{
    /* eu-uswest wie im Menschentest: RTT ~182 ms, D = 3; B startet 2,3 Ticks spaeter (Startversatz) */
    static const w372_t base = {"", 3, 91, 91, 5, 7000, 117, 0, 1, 1, 0, 6000};
    w372_t c;
    double la, lb, ma, mb, ref_late, ref_nots;
    u32 act;
    w372_tt_tests();
    c = base; c.name = "eu-uswest D=3 frueh, Feld-Abgleich";
    w372_run(&c, &la, &lb, &ma, &mb, &act);
    CHECK(la < 2.0 && lb < 2.0, "%s: Vorhersage-Anteil %.1f/%.1f%% (Ziel < 2)", c.name, la, lb);
    CHECK(ma < 15.0 && mb < 15.0 && act > 0, "%s: Vorsprung %.1f/%.1f ms, Felder %u", c.name, ma, mb, act);
    c = base; c.name = "dto., spaet senden (alt)"; c.late_send = 1;
    w372_run(&c, &la, &lb, &ma, &mb, &act);
    ref_late = la > lb ? la : lb;
    c = base; c.name = "dto., frueh, ohne Zeitabgleich"; c.tsync = 0;
    w372_run(&c, &la, &lb, &ma, &mb, &act);
    ref_nots = la > lb ? la : lb;
    printf("W372 Gegenproben: spaet senden %.1f%%, ohne Zeitabgleich %.1f%% (hoechster Anteil einer Seite)\n", ref_late,
           ref_nots);
    CHECK(ref_late > 5.0, "Gegenprobe spaetes Senden zeigt kaum Vorhersagen (%.1f%%)", ref_late);
    CHECK(ref_nots > 50.0, "Gegenprobe ohne Zeitabgleich zeigt kaum Vorhersagen (%.1f%%)", ref_nots);
    c = base; c.name = "Weg ungleich 50/132 ms"; c.ab_ms = 50; c.ba_ms = 132;
    w372_run(&c, &la, &lb, &ma, &mb, &act);
    CHECK(la < 2.0 && lb < 2.0, "%s: Vorhersage-Anteil %.1f/%.1f%%", c.name, la, lb);
    c = base; c.name = "B-Takt 0,3% langsamer (Drift)"; c.field_b = 16717;
    w372_run(&c, &la, &lb, &ma, &mb, &act);
    CHECK(la < 3.0 && lb < 3.0, "%s: Vorhersage-Anteil %.1f/%.1f%%", c.name, la, lb);
    c = base; c.name = "A-Takt 0,3% langsamer (Drift)"; c.field_b = 16617;
    w372_run(&c, &la, &lb, &ma, &mb, &act);
    CHECK(la < 3.0 && lb < 3.0, "%s: Vorhersage-Anteil %.1f/%.1f%%", c.name, la, lb);
    c = base; c.name = "ohne SBRT (Slot-Schaetzer)"; c.tt = 0;
    w372_run(&c, &la, &lb, &ma, &mb, &act);
    CHECK(la < 5.0 && lb < 5.0, "%s: Vorhersage-Anteil %.1f/%.1f%%", c.name, la, lb);
    c = base; c.name = "RTT 250 ms, Schwankung 10 ms"; c.ab_ms = c.ba_ms = 125; c.jit_ms = 10;
    w372_run(&c, &la, &lb, &ma, &mb, &act);
    CHECK(la < 5.0 && lb < 5.0, "%s: Vorhersage-Anteil %.1f/%.1f%%", c.name, la, lb);
    c = base; c.name = "Startversatz 10 Ticks"; c.start_b_ms = 500;
    w372_run(&c, &la, &lb, &ma, &mb, &act);
    CHECK(la < 2.0 && lb < 2.0, "%s: Vorhersage-Anteil %.1f/%.1f%%", c.name, la, lb);
}

/* W382 LOC-ISSUES-B (#53/#55-#58): Spieltakt-Messung und LoC-Zustandszeile fuer das Feedback-Paket. Zeitbasis 1 MHz.
 * Faelle: 20 Hz gleichmaessig, Fenster 60 s, Stuetzpunkte hoechstens je Sekunde, zu kurze Messung, neue Mission
 * (Tick faellt), Langsamlauf, Zeile mit/ohne Messung (keine SB-Felder). */
static void w382_cases(void)
{
    loc_tickrate_t r;
    loc_state_t st;
    char line[240];
    double hz, span;
    long long t;
    unsigned tick;
    memset(&r, 0, sizeof r);
    for (t = 0, tick = 0; t <= 120000000LL; t += 50000, tick++) loc_tickrate_push(&r, t, tick, 1000000); /* 120 s, 20 Hz */
    hz = loc_tickrate_hz(&r, 60.0, &span);
    CHECK(hz > 19.99 && hz < 20.01, "W382: 20 Hz gemessen als %.3f", hz);
    CHECK(span >= 59.0 && span <= 61.0, "W382: Fenster %.1f s statt ~60", span);
    CHECK(r.n == LOC_TR_N, "W382: Ring %d statt %d (je Sekunde ein Stuetzpunkt)", r.n, LOC_TR_N);
    memset(&r, 0, sizeof r);
    for (t = 0, tick = 0; t <= 3000000LL; t += 50000, tick++) loc_tickrate_push(&r, t, tick, 1000000); /* 3 s */
    CHECK(loc_tickrate_hz(&r, 60.0, &span) == 0.0 && span == 0.0, "W382: unter 5 s keine Aussage");
    memset(&r, 0, sizeof r);
    for (t = 0, tick = 5000; t <= 30000000LL; t += 50000, tick++) loc_tickrate_push(&r, t, tick, 1000000);
    for (tick = 0; t <= 50000000LL; t += 52000, tick++) loc_tickrate_push(&r, t, tick, 1000000); /* neue Mission, 19,23 Hz */
    hz = loc_tickrate_hz(&r, 60.0, &span);
    CHECK(r.resets == 1u, "W382: neue Mission nicht erkannt (%u)", r.resets);
    CHECK(hz > 19.1 && hz < 19.4 && span < 20.5, "W382: nach Neuanfang %.3f Hz ueber %.1f s", hz, span);
    memset(&st, 0, sizeof st);
    st.lockstep = 1; st.frame = 3400; st.tick = 3450; st.hz = 19.877; st.span_s = 60; st.scene = 6; st.step = 2;
    st.interp_fps = 165; st.render_thread = 0; st.vblank_hz = 60;
    loc_state_line(line, sizeof line, &st);
    printf("W382 Zustandszeile: %s\n", line);
    CHECK(strstr(line, "game=loc phase=lockstep frame=3400 tick=3450 sim_hz=19.88 (60 s, original 20)") != NULL,
          "W382: Zeile '%s'", line);
    CHECK(strstr(line, "interp_fps=165 render_thread=0") != NULL && strstr(line, "loadcount") == NULL,
          "W382: Zeile ohne Bild-Weg oder mit SB-Feld '%s'", line);
    memset(&st, 0, sizeof st);
    loc_state_line(line, sizeof line, &st);
    CHECK(strstr(line, "phase=menu") != NULL && strstr(line, "sim_hz=n/a") != NULL, "W382: Menue-Zeile '%s'", line);
    CHECK(loc_state_line(line, 16, &st) == 15 && strlen(line) == 15, "W382: Kuerzung");
}

/* W386 LOC-LATENCY: niedrige Verzoegerung. (a) rbi_est_depth/rbn_pick_low rein; (b) Wechsel der Zuordnung eigener
 * Rahmen im Einzelmodul (D-1 verwirft einen Rahmen und uebernimmt Tasten/Flags, D+1 doppelt ohne einmalige Flags);
 * (c) Zwilling mit Wechseln waehrend Vorhersagen/Rollbacks (runter, hoch, eine Seite spaet, mehrmals) - synchron;
 * (d) Regler rbl_* (senken nach guten Fenstern, zurueck bei schwer/schlecht, Abklingzeit, Grenze, Bewertungsregeln);
 * (e) Paket SBRL: Rundweg, Grenzfaelle, verfaelschte Pakete. */
static void w386_unit_switch(void)
{
    rbi_t *r = rbi_new();
    uint8_t hdr[RBI_HDR] = {1, 0, 0, 0, 0x03, 0, 0, 0}, in[RBI_IN], out[RBI_FR];
    int q, d;
    u32 sl;
    rbi_begin(r, 9, 0, 2, 10);
    CHECK(rbi_set_delay(r, 1, 0) == 0, "W386 Wechsel vor der ersten Entnahme abgelehnt");
    for (q = 0; q < 4; q++) { /* Vorlauf q0..q3 */
        memset(in, 0, sizeof in);
        in[9] = (uint8_t)q;
        rbi_own(r, hdr, in);
    }
    CHECK(rbi_take(r, 0, 0, out) == 1 && out[RBI_HDR + 9] == 2, "W386 Slot 0 = Rahmen 2 (P0 4, D 2)");
    (void)rbi_tick_end(r, NULL);
    CHECK(rbi_delay_eff(r) == 2 && rbi_own_next_slot(r) == 2u, "W386 naechster eigener Slot 2 (%u)", rbi_own_next_slot(r));
    for (q = 4; q < 6; q++) { /* q4 -> Slot 2, q5 -> Slot 3 */
        memset(in, 0, sizeof in);
        in[9] = (uint8_t)q;
        rbi_own(r, hdr, in);
    }
    CHECK(rbi_set_delay(r, 3, 4) == 1 && rbi_delay_pending(r, &d, &sl) && d == 3, "W386 hoch vorgemerkt");
    CHECK(rbi_set_delay(r, 2, 4) == 0 && !rbi_delay_pending(r, NULL, NULL), "W386 gleiches D hebt Vormerkung auf");
    CHECK(rbi_set_delay(r, 9, 4) == 0 && rbi_set_delay(r, 0, 4) == 0, "W386 D ausserhalb 1..8 abgelehnt");
    CHECK(rbi_set_delay(r, 1, 4) == 1, "W386 runter vorgemerkt");
    memset(in, 0, sizeof in); /* q6: wuerde Slot 4 belegen -> verworfen, Taste + einmaliges Flag uebernehmen */
    in[9] = 6;
    in[0] = 0x01;
    in[3] = 0x80;
    hdr[7] = 0x40; /* Flags-Dword 0x40000003: einmaliges Flag */
    rbi_own(r, hdr, in);
    hdr[7] = 0;
    CHECK(rbi_delay_eff(r) == 1 && rbi_stats(r)->dchg_down == 1u, "W386 D-1 angewandt");
    memset(in, 0, sizeof in); /* q7 -> Slot 4 (mit Uebertrag) */
    in[9] = 7;
    in[1] = 0x02;
    rbi_own(r, hdr, in);
    for (q = 1; q <= 4; q++) {
        CHECK(rbi_take(r, 0, 0, out) == 1, "W386 Entnahme Slot %d", q);
        (void)rbi_tick_end(r, NULL);
        if (q < 4) CHECK(out[RBI_HDR + 9] == (uint8_t)(q + 2), "W386 Slot %d = Rahmen %d (%u)", q, q + 2, out[RBI_HDR + 9]);
    }
    CHECK(out[RBI_HDR + 9] == 7 && out[RBI_HDR + 0] == 0x01 && out[RBI_HDR + 1] == 0x02 && out[RBI_HDR + 3] == 0x80 &&
              out[7] == 0x40 && out[4] == 0x03,
          "W386 Slot 4 = Rahmen 7 mit Tasten/Flag des verworfenen Rahmens 6 (%u %02X %02X %02X fl %02X)", out[RBI_HDR + 9],
          out[RBI_HDR], out[RBI_HDR + 1], out[RBI_HDR + 3], out[7]);
    CHECK(rbi_own_next_slot(r) == 5u, "W386 nach D-1 naechster Slot 5 (%u)", rbi_own_next_slot(r));
    CHECK(rbi_set_delay(r, 3, 5) == 1, "W386 hoch (Schritt) vorgemerkt");
    CHECK(rbi_delay_pending(r, &d, NULL) && d == 2, "W386 hoechstens 1 Tick je Schritt (%d)", d);
    memset(in, 0, sizeof in); /* q8 -> Slot 5 und (ohne Flag) Slot 6 */
    in[9] = 8;
    in[2] = 0x04;
    hdr[7] = 0x40;
    rbi_own(r, hdr, in);
    hdr[7] = 0;
    memset(in, 0, sizeof in);
    in[9] = 9; /* q9 -> Slot 7 */
    rbi_own(r, hdr, in);
    CHECK(rbi_delay_eff(r) == 2 && rbi_stats(r)->dchg_up == 1u, "W386 D+1 angewandt");
    CHECK(rbi_take(r, 0, 0, out) == 1 && out[RBI_HDR + 9] == 8 && out[7] == 0x40, "W386 Slot 5 = Rahmen 8 mit Flag");
    (void)rbi_tick_end(r, NULL);
    CHECK(rbi_take(r, 0, 0, out) == 1 && out[RBI_HDR + 9] == 8 && out[7] == 0 && out[RBI_HDR + 2] == 0x04,
          "W386 Slot 6 = Rahmen 8 gedoppelt, Taste gehalten, ohne einmaliges Flag");
    (void)rbi_tick_end(r, NULL);
    CHECK(rbi_take(r, 0, 0, out) == 1 && out[RBI_HDR + 9] == 9, "W386 Slot 7 = Rahmen 9");
    (void)rbi_tick_end(r, NULL);
    CHECK(rbi_stats(r)->own_missing == 0u, "W386 kein eigener Rahmen fehlte");
    /* spaete Ankuendigung: Slot liegt schon hinter dem naechsten eigenen -> beim naechsten Rahmen, gezaehlt */
    CHECK(rbi_set_delay(r, 1, 3) == 1, "W386 spaet vorgemerkt");
    memset(in, 0, sizeof in);
    rbi_own(r, hdr, in);
    CHECK(rbi_delay_eff(r) == 1 && rbi_stats(r)->dchg_late == 1u, "W386 spaete Ankuendigung angewandt und gezaehlt");
    rbi_free(r);
}

static void w386_twin(void)
{
    static const struct {
        int dab, dba, jit, loss, D;
        int n;
        dchg_t ch[6];
        int dend;
        const char *name;
    } k_tw[] = {
        {3, 3, 1, 0, 3, 2, {{1500, 2, {8, 8}}, {4000, 3, {8, 8}}}, 3, "W386 3+3, D 3->2->3"},
        {3, 3, 2, 5, 3, 3, {{1200, 2, {6, 6}}, {2600, 3, {6, 6}}, {3900, 2, {6, 6}}}, 2, "W386 3+3 Verlust, D 3->2->3->2"},
        {2, 2, 1, 0, 2, 2, {{1000, 1, {8, 0}}, {3000, 2, {8, 0}}}, 2, "W386 2+2, B wechselt spaet"},
        {2, 2, 2, 0, 2, 2, {{1000, 1, {8, -6}}, {3000, 2, {-6, 8}}}, 2, "W386 2+2, Ankuendigung zu spaet"},
        {4, 4, 2, 2, 3, 1, {{800, 2, {10, 10}}}, 2, "W386 4+4, D 3->2 (Weg > D)"},
        {1, 1, 0, 0, 2, 2, {{900, 1, {5, 5}}, {2900, 2, {5, 5}}}, 2, "W386 1+1, D 2->1->2"},
    };
    unsigned i;
    scen_t s;
    for (i = 0; i < sizeof k_tw / sizeof k_tw[0]; i++) {
        int k;
        memset(&s, 0, sizeof s);
        s.delay_ab = k_tw[i].dab;
        s.delay_ba = k_tw[i].dba;
        s.jitter = k_tw[i].jit;
        s.loss_pct = k_tw[i].loss;
        s.D = k_tw[i].D;
        s.maxpred = 10;
        s.ticks = 5000;
        g_dchg_n = k_tw[i].n;
        for (k = 0; k < g_dchg_n; k++) g_dchg[k] = k_tw[i].ch[k];
        g_chg = 2;
        g_ramp = 1;
        g_predm = 1;
        g_op_n = g_op_bad = 0;
        g_op_D = -1;
        g_w385_capture = 1;
        scenario(k_tw[i].name, &s, -1);
        g_w385_capture = 0;
        CHECK(g_op_bad == 0u && g_op_n > 1000u, "%s: rbi_own_pending passte nicht zum entnommenen Rahmen (%u von %u)",
              k_tw[i].name, g_op_bad, g_op_n);
        {
            u32 dn = g_w385_st[0].dchg_down + g_w385_st[1].dchg_down, up = g_w385_st[0].dchg_up + g_w385_st[1].dchg_up;
            int want_dn = 0, want_up = 0, cur = k_tw[i].D;
            for (k = 0; k < g_dchg_n; k++) {
                if (k_tw[i].ch[k].d < cur) want_dn += 2;
                else if (k_tw[i].ch[k].d > cur) want_up += 2;
                cur = k_tw[i].ch[k].d;
            }
            printf("%-28s   W386 Wechsel runter %u hoch %u spaet %u/%u | Schaetzer spaet %u/%u falsch %u/%u von %u/%u\n", "",
                   dn, up, g_w385_st[0].dchg_late, g_w385_st[1].dchg_late, g_w385_st[0].est_late, g_w385_st[1].est_late,
                   g_w385_st[0].est_wrong, g_w385_st[1].est_wrong, g_w385_st[0].est_n, g_w385_st[1].est_n);
            CHECK(dn == (u32)want_dn && up == (u32)want_up, "%s: Wechsel runter/hoch %u/%u statt %d/%d", k_tw[i].name, dn, up,
                  want_dn, want_up);
        }
    }
    g_dchg_n = 0;
    g_chg = 7;
    g_ramp = 0;
    g_predm = 0;
}

static void w386_rep(rbl_rep_t *r, int dcur, unsigned win, unsigned est_cpm, unsigned cpm)
{
    memset(r, 0, sizeof *r);
    r->ready = 1;
    r->dcur = dcur;
    r->win = win;
    r->est_cpm = est_cpm;
    r->est_dep = est_cpm ? 1 : 0;
    r->cpm = cpm;
    r->dep = cpm ? 1 : 0;
    r->rp = cpm ? 10 : 0;
}

/* Client-Bericht ueber das Paket zum Host bringen (Rundweg wie im Netz) */
static int w386_feed(rbl_t *host, rbl_t *cli, const rbl_rep_t *r, u32 now)
{
    uint8_t b[RBL_LEN];
    int n = rbl_tx(cli, r, b, (int)sizeof b);
    return n == RBL_LEN ? rbl_rx(host, now, b, n) : -9;
}

static void w386_ctl(void)
{
    rbl_cfg_t c;
    rbl_t *h = rbl_new(), *cl = rbl_new();
    rbl_rep_t ro, rc;
    uint8_t b[RBL_LEN];
    u32 now = 100000u, sl = 0;
    int d = 0, k, w = 1;
    rbl_cfg_default(&c);
    CHECK(rbn_pick_low(160, 2, 250) == 1 && rbn_pick_low(100, 2, 250) == 1 && rbn_pick_low(240, 3, 250) == 2 &&
              rbn_pick_low(260, 3, 250) == 3 && rbn_pick_low(-1, 2, 250) == 2 && rbn_pick_low(50, 1, 250) == 1,
          "W386 rbn_pick_low");
    CHECK(rbi_est_depth(60000) == 0 && rbi_est_depth(50000) == 0 && rbi_est_depth(49999) == 1 && rbi_est_depth(0) == 1 &&
              rbi_est_depth(-1) == 2 && rbi_est_depth(-50000) == 2 && rbi_est_depth(-50001) == 3,
          "W386 rbi_est_depth");
    rbl_begin(h, &c, 0xABCD0001u, 0, 1, 3, 2);
    rbl_begin(cl, &c, 0xABCD0001u, 1, 1, 3, 2);
    CHECK(rbl_target(h) == 3 && rbl_dlow(h) == 2 && rbl_dstd(h) == 3, "W386 Start D 3, niedrig 2");
    /* ohne Bericht des Mitspielers: nie senken */
    for (k = 0; k < 4; k++, now += RBL_WIN_MS) {
        w386_rep(&ro, 3, (unsigned)w++, 50, 0);
        CHECK(rbl_host_step(h, now, &ro, 1000u, 160) == 0, "W386 ohne Mitspieler-Bericht kein Senken");
    }
    /* gute Fenster beider Seiten -> nach good_n senken */
    for (k = 0; k < 2; k++, now += RBL_WIN_MS) {
        w386_rep(&rc, 3, (unsigned)(100 + k), 80, 0);
        CHECK(w386_feed(h, cl, &rc, now) == 1, "W386 Bericht angenommen");
        w386_rep(&ro, 3, (unsigned)w++, 60, 0);
        CHECK(rbl_host_step(h, now, &ro, 2000u, 160) == (k == 1), "W386 senken erst nach 2 guten Fenstern (k %d)", k);
    }
    CHECK(rbl_target(h) == 2 && rbl_take_decision(h, &d, &sl) && d == 2 && sl == 2000u + 10u + 4u,
          "W386 Entscheidung D 2 ab Slot %u", sl);
    CHECK(!rbl_take_decision(h, NULL, NULL), "W386 Entscheidung nur einmal abholen");
    /* Client bekommt sie ueber das Paket des Hosts */
    CHECK(rbl_tx(h, &ro, b, (int)sizeof b) == RBL_LEN && rbl_rx(cl, now, b, RBL_LEN) == 1, "W386 Host-Paket angenommen");
    CHECK(rbl_take_decision(cl, &d, &sl) && d == 2 && sl == 2014u && rbl_target(cl) == 2, "W386 Client uebernimmt D 2");
    CHECK(rbl_rx(cl, now, b, RBL_LEN) == 1 && !rbl_take_decision(cl, NULL, NULL), "W386 Wiederholung nicht doppelt");
    /* Mitspieler noch auf D 3: nicht bewerten */
    w386_rep(&rc, 3, 200, 0, 0);
    w386_feed(h, cl, &rc, now);
    w386_rep(&ro, 2, (unsigned)w++, 0, 50);
    {
        u32 sk = rbl_stats(h)->skipped;
        CHECK(rbl_host_step(h, now, &ro, 3000u, 160) == 0 && rbl_stats(h)->skipped == sk + 1u, "W386 Wechsel laeuft: uebersprungen");
    }
    now += RBL_WIN_MS;
    /* niedrig, gut: bleibt */
    for (k = 0; k < 3; k++, now += RBL_WIN_MS) {
        w386_rep(&rc, 2, (unsigned)(201 + k), 0, 150);
        w386_feed(h, cl, &rc, now);
        w386_rep(&ro, 2, (unsigned)w++, 0, 140);
        CHECK(rbl_host_step(h, now, &ro, 4000u, 160) == 0, "W386 niedrig und gut: bleibt");
    }
    /* gleiches Mitspieler-Fenster zweimal: nicht bewerten */
    w386_rep(&ro, 2, (unsigned)w++, 0, 140);
    CHECK(rbl_host_step(h, now, &ro, 4000u, 160) == 0, "W386 kein neues Mitspieler-Fenster");
    now += RBL_WIN_MS;
    /* ein schlechtes Fenster (Korrekturen) -> noch nicht, zwei -> zurueck */
    w386_rep(&rc, 2, 210, 0, 900);
    w386_feed(h, cl, &rc, now);
    w386_rep(&ro, 2, (unsigned)w++, 0, 100);
    CHECK(rbl_host_step(h, now, &ro, 5000u, 160) == 0, "W386 ein schlechtes Fenster: noch nicht zurueck");
    now += RBL_WIN_MS;
    w386_rep(&rc, 2, 211, 0, 900);
    w386_feed(h, cl, &rc, now);
    w386_rep(&ro, 2, (unsigned)w++, 0, 100);
    CHECK(rbl_host_step(h, now, &ro, 5200u, 160) == 1 && rbl_target(h) == 3 && rbl_stats(h)->last_why == RBL_WHY_CPM,
          "W386 zwei schlechte Fenster: zurueck auf D 3 (Grund Korrekturen)");
    (void)rbl_take_decision(h, NULL, NULL);
    now += RBL_WIN_MS;
    /* Abklingzeit 60 s: gute Fenster senken erst danach */
    {
        u32 t0 = now;
        int lowered = 0;
        for (k = 0; k < 12 && !lowered; k++, now += RBL_WIN_MS) {
            w386_rep(&rc, 3, (unsigned)(300 + k), 40, 0);
            w386_feed(h, cl, &rc, now);
            w386_rep(&ro, 3, (unsigned)w++, 40, 0);
            if (rbl_host_step(h, now, &ro, 6000u, 160)) {
                lowered = 1;
                CHECK(now - t0 >= 60000u - RBL_WIN_MS, "W386 Abklingzeit eingehalten (%u ms)", now - t0);
            }
        }
        CHECK(lowered && rbl_target(h) == 2, "W386 nach der Abklingzeit wieder gesenkt");
        (void)rbl_take_decision(h, NULL, NULL);
    }
    /* schwer (Neurechnen > 5 ms) -> sofort zurueck */
    w386_rep(&rc, 2, 400, 0, 100);
    rc.rp = 60;
    w386_feed(h, cl, &rc, now);
    w386_rep(&ro, 2, (unsigned)w++, 0, 100);
    CHECK(rbl_host_step(h, now, &ro, 7000u, 160) == 1 && rbl_stats(h)->last_why == RBL_WHY_REPLAY,
          "W386 Neurechnen > 5 ms: sofort zurueck");
    (void)rbl_take_decision(h, NULL, NULL);
    now += 130000u; /* Abklingzeit jetzt 120 s */
    for (k = 0; k < 2; k++, now += RBL_WIN_MS) {
        w386_rep(&rc, 3, (unsigned)(410 + k), 40, 0);
        w386_feed(h, cl, &rc, now);
        w386_rep(&ro, 3, (unsigned)w++, 40, 0);
        (void)rbl_host_step(h, now, &ro, 8000u, 160);
    }
    CHECK(rbl_target(h) == 2, "W386 drittes Senken");
    (void)rbl_take_decision(h, NULL, NULL);
    /* Mitspieler ohne Ring -> sofort zurueck; danach max_up erreicht -> bleibt hoch */
    w386_rep(&rc, 2, 420, 0, 100);
    rc.ready = 0;
    w386_feed(h, cl, &rc, now);
    w386_rep(&ro, 2, (unsigned)w++, 0, 100);
    CHECK(rbl_host_step(h, now, &ro, 9000u, 160) == 1 && rbl_stats(h)->last_why == RBL_WHY_NOTREADY, "W386 ohne Ring: zurueck");
    (void)rbl_take_decision(h, NULL, NULL);
    now += 600000u;
    for (k = 0; k < 5; k++, now += RBL_WIN_MS) {
        w386_rep(&rc, 3, (unsigned)(430 + k), 10, 0);
        w386_feed(h, cl, &rc, now);
        w386_rep(&ro, 3, (unsigned)w++, 10, 0);
        CHECK(rbl_host_step(h, now, &ro, 9500u, 160) == 0, "W386 nach max_up nicht mehr senken");
    }
    /* Regeln fuer das Senken einzeln: Schaetzer, Tiefe, Neurechnen, Haenger, nicht bereit */
    {
        static const struct { int f; unsigned v; const char *n; } k_g[] = {
            {0, 301, "geschaetzt > lcpm"}, {1, 3, "geschaetzte Tiefe > 2"}, {2, 31, "Neurechnen > 3 ms"},
            {3, 2, "Haenger > 1"}, {4, 0, "nicht bereit"},
        };
        unsigned g;
        for (g = 0; g < sizeof k_g / sizeof k_g[0]; g++) {
            int dec = 0;
            rbl_begin(h, &c, 0xABCD0002u, 0, 1, 2, 1);
            rbl_begin(cl, &c, 0xABCD0002u, 1, 1, 2, 1);
            for (k = 0; k < 4; k++, now += RBL_WIN_MS) {
                w386_rep(&rc, 2, (unsigned)(500 + k), 50, 0);
                if (k_g[g].f == 0) rc.est_cpm = k_g[g].v;
                if (k_g[g].f == 1) rc.est_dep = k_g[g].v;
                if (k_g[g].f == 2) rc.rp = k_g[g].v;
                if (k_g[g].f == 3) rc.hang = k_g[g].v;
                if (k_g[g].f == 4) rc.ready = 0;
                w386_feed(h, cl, &rc, now);
                w386_rep(&ro, 2, (unsigned)w++, 50, 0);
                dec |= rbl_host_step(h, now, &ro, 100u, 100);
            }
            CHECK(!dec && rbl_target(h) == 2, "W386 kein Senken bei %s", k_g[g].n);
        }
    }
    /* Haenger ueber 6 Fenster: je Fenster 1 (unter hhang), nach 6 Fenstern zusammen 6 -> zurueck (Grund Haenger) */
    {
        int up_at = -1;
        rbl_begin(h, &c, 0xABCD0005u, 0, 1, 2, 1);
        rbl_begin(cl, &c, 0xABCD0005u, 1, 1, 2, 1);
        for (k = 0; k < 2; k++, now += RBL_WIN_MS) {
            w386_rep(&rc, 2, (unsigned)(700 + k), 10, 0);
            w386_feed(h, cl, &rc, now);
            w386_rep(&ro, 2, (unsigned)w++, 10, 0);
            (void)rbl_host_step(h, now, &ro, 100u, 100);
        }
        CHECK(rbl_target(h) == 1, "W386 Haenger-Summe: vorher gesenkt");
        for (k = 0; k < 8 && up_at < 0; k++, now += RBL_WIN_MS) {
            w386_rep(&rc, 1, (unsigned)(710 + k), 0, 60);
            rc.hang = 1;
            w386_feed(h, cl, &rc, now);
            w386_rep(&ro, 1, (unsigned)w++, 0, 60);
            if (rbl_host_step(h, now, &ro, 200u, 100)) up_at = k;
        }
        CHECK(up_at == 5 && rbl_stats(h)->last_why == RBL_WHY_HANG, "W386 Haenger-Summe 6 in 6 Fenstern: zurueck im 6. (%d)", up_at);
    }
    /* aus (lowd=0): nie senken; d_low == d_std: nie senken */
    c.on = 0;
    rbl_begin(h, &c, 0xABCD0003u, 0, 1, 2, 1);
    rbl_begin(cl, &c, 0xABCD0003u, 1, 1, 2, 1);
    for (k = 0; k < 4; k++, now += RBL_WIN_MS) {
        w386_rep(&rc, 2, (unsigned)(600 + k), 10, 0);
        w386_feed(h, cl, &rc, now);
        w386_rep(&ro, 2, (unsigned)w++, 10, 0);
        CHECK(rbl_host_step(h, now, &ro, 100u, 100) == 0, "W386 lowd=0: nie senken");
    }
    c.on = 1;
    rbl_begin(h, &c, 0xABCD0004u, 0, 1, 3, 3);
    for (k = 0; k < 4; k++, now += RBL_WIN_MS) {
        w386_rep(&ro, 3, (unsigned)w++, 10, 0);
        CHECK(rbl_host_step(h, now, &ro, 100u, 300) == 0, "W386 d_low = d_std: nie senken");
    }
    /* Client entscheidet nie */
    w386_rep(&ro, 2, 1, 0, 0);
    CHECK(rbl_host_step(cl, now, &ro, 100u, 100) == 0 && rbl_force(cl, 1, 100u, 100) == 0, "W386 Client entscheidet nie");
    rbl_free(h);
    rbl_free(cl);
}

static void w386_pkt(void)
{
    rbl_cfg_t c;
    rbl_t *h = rbl_new(), *cl = rbl_new();
    rbl_rep_t r;
    uint8_t b[RBL_LEN], g[RBL_LEN], hb[RBL_LEN];
    int n, k, ok = 0, bad = 0, acc_bad = 0;
    unsigned i;
    rbl_cfg_default(&c);
    rbl_begin(h, &c, 0x11110001u, 0, 1, 2, 1);
    rbl_begin(cl, &c, 0x11110001u, 1, 1, 2, 1);
    memset(&r, 0, sizeof r);
    r.ready = 1;
    r.dcur = 2;
    r.win = 7;
    r.cpm = 70000; /* geklemmt */
    r.est_cpm = 123;
    r.dep = 300;
    r.est_dep = 2;
    r.rp = 999;
    r.hang = 3;
    r.jump = 456;
    n = rbl_tx(cl, &r, g, (int)sizeof g);
    CHECK(n == RBL_LEN && memcmp(g, "SBRL", 4) == 0 && g[5] == 1 && g[6] == 1 && g[7] == 2, "W386 SBRL Kopf");
    CHECK(rbl_peek_pidx(g, n) == 1 && rbl_peek_pidx(g, n - 1) == -1 && rbl_is_pkt(g, n), "W386 SBRL Index");
    CHECK(rbl_rx(h, 5u, g, n) == 1, "W386 SBRL Rundweg");
    CHECK(rbl_tx(cl, &r, b, RBL_LEN - 1) == 0, "W386 SBRL zu kleiner Puffer");
    r.dcur = 0;
    CHECK(rbl_tx(cl, &r, b, RBL_LEN) == 0, "W386 SBRL dcur 0 nicht senden");
    r.dcur = 2;
    /* Host-Paket mit Entscheidung */
    CHECK(rbl_force(h, 1, 50000u, 160) == 1 && rbl_tx(h, &r, hb, RBL_LEN) == RBL_LEN && (hb[5] & 2) && hb[26] == 1,
          "W386 SBRL Host-Entscheidung im Paket");
    CHECK(rbl_rx(cl, 6u, hb, RBL_LEN) == 1, "W386 SBRL Host-Paket angenommen");
    /* Grenzfaelle (je eines falsch) */
    {
        static const struct { int off; uint8_t v; const char *n; } k_bad[] = {
            {4, 2, "Version"}, {5, 4, "Flag-Bit 2"}, {6, 10, "Index 10"}, {7, 0, "D 0"}, {7, 9, "D 9"}, {12, 0, "Fenster 0"},
            {27, 1, "Fuellbyte"}, {26, 1, "D ohne Entscheidung"}, {31, 1, "Slot ohne Entscheidung"}, {5, 3, "Entscheidung vom Client"},
            {0, 'X', "Kennung"},
        };
        for (i = 0; i < sizeof k_bad / sizeof k_bad[0]; i++) {
            memcpy(b, g, RBL_LEN);
            b[k_bad[i].off] = k_bad[i].v;
            if (k_bad[i].off == 12) b[13] = 0;
            CHECK(rbl_rx(h, 7u, b, RBL_LEN) == -1, "W386 SBRL abgelehnt: %s", k_bad[i].n);
        }
        memcpy(b, hb, RBL_LEN);
        b[26] = 0;
        CHECK(rbl_rx(cl, 7u, b, RBL_LEN) == -1, "W386 SBRL Entscheidung D 0 abgelehnt");
        memcpy(b, hb, RBL_LEN);
        b[24] = b[25] = 0;
        CHECK(rbl_rx(cl, 7u, b, RBL_LEN) == -1, "W386 SBRL Entscheidung Folge 0 abgelehnt");
        memcpy(b, hb, RBL_LEN);
        b[31] = 0x41;
        CHECK(rbl_rx(cl, 7u, b, RBL_LEN) == -1, "W386 SBRL Slot > 2^30 abgelehnt");
        CHECK(rbl_rx(h, 7u, g, RBL_LEN - 1) == -1 && rbl_rx(h, 7u, g, 0) == -1 && rbl_rx(h, 7u, NULL, RBL_LEN) == -1,
              "W386 SBRL Laenge");
        memcpy(b, g, RBL_LEN);
        b[6] = 0;
        CHECK(rbl_rx(h, 7u, b, RBL_LEN) == -1, "W386 SBRL eigener Index abgelehnt");
        memcpy(b, g, RBL_LEN);
        b[8] ^= 1;
        CHECK(rbl_rx(h, 7u, b, RBL_LEN) == 0 && rbl_stats(h)->rx_epoch >= 1u, "W386 SBRL andere Epoche ignoriert");
        rbl_end(h);
        CHECK(rbl_rx(h, 7u, g, RBL_LEN) == 0, "W386 SBRL inaktiv ignoriert");
        rbl_begin(h, &c, 0x11110001u, 0, 1, 2, 1);
    }
    /* verfaelschte Pakete: zufaellige Bytes/Bits; nie Absturz, nur gueltige werden uebernommen */
    for (k = 0; k < 20000; k++) {
        int m = (int)(rnd() % 4u), j;
        memcpy(b, (k & 1) ? g : hb, RBL_LEN);
        for (j = 0; j <= m; j++) b[rnd() % RBL_LEN] ^= (uint8_t)(1u << (rnd() % 8u));
        if (k % 7 == 0) for (j = 0; j < RBL_LEN; j++) b[j] = (uint8_t)rnd();
        n = rbl_rx((k & 1) ? h : cl, 8u, b, (k % 13 == 0) ? (int)(rnd() % (RBL_LEN + 8)) : RBL_LEN);
        if (n == 1) {
            ok++;
            if (b[7] < 1 || b[7] > 8 || (b[5] & ~3) || b[27]) acc_bad++;
        } else if (n == -1) {
            bad++;
        }
    }
    printf("W386 SBRL verfaelscht: 20000 Pakete, angenommen %d (alle gueltig: %s), abgelehnt %d\n", ok, acc_bad ? "NEIN" : "ja", bad);
    CHECK(acc_bad == 0 && bad > 10000, "W386 SBRL verfaelschte Pakete (%d angenommen ungueltig, %d abgelehnt)", acc_bad, bad);
    rbl_free(h);
    rbl_free(cl);
}

/* W386: Schaetzer "Korrekturen bei D-1" gegen die Wirklichkeit: Zeitstrecke mit Rampen (Trend-Vorhersage) einmal mit D
 * (Schaetzer) und einmal mit D-1 (tatsaechliche Fehlvorhersagen), gleiche Wege. */
static void w386_est(void)
{
    static const struct { u32 ow; int D; const char *n; } k_e[] = {
        {40, 2, "W386 Schaetzer 80 ms, D 2 -> 1"},
        {70, 2, "W386 Schaetzer 140 ms, D 2 -> 1"},
        {120, 3, "W386 Schaetzer 240 ms, D 3 -> 2"},
    };
    unsigned i;
    for (i = 0; i < sizeof k_e / sizeof k_e[0]; i++) {
        w372_t c = {"", 2, 70, 70, 4, 7000, 117, 0, 1, 1, 0, 4000};
        double la, lb, ma, mb;
        u32 act, est, real, late, n;
        char nm[96];
        c.ab_ms = c.ba_ms = k_e[i].ow;
        c.D = k_e[i].D;
        g_ramp = 1;
        g_predm = 1;
        g_chg = 2;
        g_w372_cap = 1;
        _snprintf_s(nm, sizeof nm, _TRUNCATE, "%s (D %d)", k_e[i].n, c.D);
        c.name = nm;
        w372_run(&c, &la, &lb, &ma, &mb, &act);
        est = g_w372_st[0].est_wrong + g_w372_st[1].est_wrong;
        late = g_w372_st[0].est_late + g_w372_st[1].est_late;
        n = g_w372_st[0].est_n + g_w372_st[1].est_n;
        c.D = k_e[i].D - 1;
        _snprintf_s(nm, sizeof nm, _TRUNCATE, "%s (D %d)", k_e[i].n, c.D);
        c.name = nm;
        w372_run(&c, &la, &lb, &ma, &mb, &act);
        real = g_w372_st[0].pred_bad + g_w372_st[1].pred_bad;
        g_w372_cap = 0;
        printf("%-34s geschaetzt %u (spaet %u von %u), bei D-1 tatsaechlich %u falsche Vorhersagen (%.2f)\n", k_e[i].n, est,
               late, n, real, real ? (double)est / real : 0.0);
        CHECK(n > 1000u && real > 100u && est >= real / 2u && est <= real * 2u,
              "%s: Schaetzer %u gegen tatsaechlich %u (Faktor 2)", k_e[i].n, est, real);
    }
    g_ramp = 0;
    g_predm = 0;
    g_chg = 7;
}

static void w386_cases(void)
{
    w386_est();
    w386_unit_switch();
    w386_twin();
    w386_ctl();
    w386_pkt();
}

int lsarb_selftest(int *ok); /* LOC-LOCKSTEP-ARBITER (tests/loc_lsarb_cases.c) */

int main(void)
{
    scen_t s;
    printf("loc_rbi_selftest (W338 LOC-ROLLBACK-R2, W346 R3/R4)\n");
    g_fail += lsarb_selftest(&g_ok);
    channel_tests();
    leave_tests(); /* W366 */
    channel_fuzz2();
    rbn_tests();
    memset(&s, 0, sizeof s);
    s.D = 2;
    s.maxpred = 10;
    s.ticks = 6000;
    g_op_D = 2; /* W376 LOC-LOCAL-AIM: rbi_own_pending */
    g_op_n = g_op_bad = g_op_full = 0;
    memset(g_op_h, 0, sizeof g_op_h);
    scenario("0 Ticks, D=2", &s, 0);
    printf("W376 rbi_own_pending: %u Entnahmen geprueft, %u abweichend, %u mit D=2 ausstehenden (Verteilung 1/2/3/4: %u/%u/%u/%u)\n", g_op_n, g_op_bad,
           g_op_full, g_op_h[1], g_op_h[2], g_op_h[3], g_op_h[4]);
    CHECK(g_op_n > 1000u && g_op_bad == 0u, "W376: rbi_own_pending liefert nicht den naechsten eigenen Rahmen");
    CHECK(g_op_full == g_op_n, "W376: rbi_own_pending: im ruhigen Lauf genau D ausstehende eigene Rahmen erwartet");
    g_op_D = -1;
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
    w372_cases(); /* W372 */
    w380_cases(); /* W380 */
    w385_cases(); /* W385 */
    w386_cases(); /* W386 */
    w381_xkey_cases(); /* W381 (#61) */
    w413_sndloop_cases(); /* W413 (#65) */
    w382_cases(); /* W382 */
    mission_switch_tests(); /* W375 */
    CHECK(g_sp_n > 20000u && g_sp_bad == 0u && g_sp_pred > 1000u, "W390: Vorlauf-Rahmen != echte Entnahme (%u von %u, davon vorhergesagt %u)",
          g_sp_bad, g_sp_n, g_sp_pred);
    CHECK(g_sp_own_n > 10000u && g_sp_own_bad == 0u, "W390: eigene Vorlauf-Rahmen falsch (%u von %u)", g_sp_own_bad, g_sp_own_n);
    printf("W390 Vorlauf-Rahmen: %u geprueft (vorhergesagt %u), abweichend %u; eigene %u, abweichend %u\n", g_sp_n, g_sp_pred,
           g_sp_bad, g_sp_own_n, g_sp_own_bad);
    printf("%s: %d ok, %d fehlgeschlagen\n", g_fail ? "FEHLER" : "OK", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
