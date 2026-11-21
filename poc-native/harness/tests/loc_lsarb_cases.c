/* loc_lsarb_cases.c - LOC-LOCKSTEP-ARBITER: Selbsttest des Pufferreglers (src/loc_lsarb.c), Teil von loc_rbi_selftest.
 *
 * Synthetisch, ohne Spieldaten. Ein Lockstep-Modell nach FUN_0001D4C0/FUN_0001D010 (Durchgang alle 50 ms in fester
 * Phase, Ringe je Spieler, eigener Ring fuellt sich beim Senden, Stau = Durchgang ohne Entnahme, Zusatzrahmen/Rahmen-weg
 * aus dem Rahmen des Taktgebers, TCP-Reihenfolge mit Laufzeit + Schwankung + Spitze) mit zwei Clients. Geprueft:
 *  1. Konfiguration, RTT-Untergrenze mit Hysterese, Taktgeber, Original-Regel @0x1D31D (vollster Ring faellt raus).
 *  2. Beide Seiten wenden Zusatzrahmen/Rahmen-weg im selben Tick an (gleiche Tick->Rahmen-Folge), auch gemischt
 *     (Taktgeber neu, Mitspieler Original und umgekehrt): nur der Taktgeber entscheidet.
 *  3. Neuer Regler: Vorlauf nah ~2, fern ~3-4 Ticks, nie Rauswurf; Original: mehr Vorlauf, Rauswurf bei Spitze. */
#include "loc_lsarb.h"

#include <stdio.h>
#include <string.h>

typedef uint32_t u32;

static int s_fail, s_ok;
#define LCHECK(c, ...)                                                                                                 \
    do {                                                                                                               \
        if (c) s_ok++;                                                                                                 \
        else {                                                                                                         \
            s_fail++;                                                                                                  \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

/* ---- Modell ---- */
#define MQ 512
typedef struct { u32 flags, set; int arr; } mfr; /* arr = Ankunftszeit (ms) */
typedef struct {
    int newarb;            /* 1 = neuer Regler, 0 = Original */
    mfr q[2][MQ];          /* Ringe je Spieler (FIFO) */
    int qh[2], qt[2];
    u32 tick, stall, sent; /* Spieltick, Stau-Durchgaenge, gesendete eigene Rahmen (= Satznummer) */
    lsarb_cfg cfg;
    lsarb_st st;
    lsarb_ovars ov;
    u32 drop_pending;
    long long lsum;
    u32 ln, kicked;
    u32 log_set[6000];     /* Tick -> Satz, mit dem der Tick gerechnet wurde */
    uint8_t log_x[6000];   /* Tick -> Zusatzrahmen angewandt */
} mcl;

typedef struct { int owd, jit, spike_t0, spike_t1, spike_ms, phase_b, rtt_meas, pb; } mlink; /* pb = Takt B (ms, 0 = 50) */

static u32 s_mr = 0x2468ACE1u;
static int mrnd(int n)
{
    s_mr ^= s_mr << 13;
    s_mr ^= s_mr >> 17;
    s_mr ^= s_mr << 5;
    return n > 0 ? (int)(s_mr % (u32)n) : 0;
}

static int s_last_arr[2];
static void m_send(mcl *C, int me, mcl *O, const mlink *k, int now, u32 flags)
{
    mfr f;
    int a;
    f.flags = flags;
    f.set = C->sent++;
    f.arr = now;
    C->q[me][C->qt[me]++ % MQ] = f; /* eigener Ring sofort */
    a = now + k->owd + mrnd(k->jit + 1);
    if (now >= k->spike_t0 && now < k->spike_t1) a += k->spike_ms;
    if (a < s_last_arr[me]) a = s_last_arr[me]; /* TCP: Reihenfolge bleibt */
    s_last_arr[me] = a;
    f.arr = a;
    O->q[me][O->qt[me]++ % MQ] = f;
}

static int m_fill(const mcl *C, int i, int now)
{
    int n = 0, j;
    for (j = C->qh[i]; j < C->qt[i]; j++) {
        if (C->q[i][j % MQ].arr <= now) n++;
        else break;
    }
    return n;
}

/* ein Durchgang von Client me (Platz me, Taktgeber = Platz 0) */
static void m_pass(mcl *C, int me, mcl *O, const mlink *k, int now)
{
    lsarb_in in;
    lsarb_out o;
    int i, extra = 0;
    u32 tkflags = 0, set = 0;
    memset(&in, 0, sizeof in);
    in.npl = 2;
    in.mask = C->kicked ? 0u : 3u;
    in.self = in.local = me;
    for (i = 0; i < 2; i++) in.q[i] = m_fill(C, i, now);
    if (C->kicked) return;
    if (in.q[0] == 0 || in.q[1] == 0) {
        C->stall++;
        return;
    }
    C->lsum += in.q[me];
    C->ln++;
    /* Entnahme (FUN_0001D4C0): ein Satz; Rahmen-weg im Taktgeber-Rahmen -> der Satz verfaellt, der Tick rechnet mit dem
     * naechsten (fehlt der noch, wartet der Tick; ein Zusatzrahmen-Bit bleibt bis zum Tickende gemerkt) */
    for (;;) {
        mfr *t = &C->q[0][C->qh[0] % MQ];
        tkflags = t->flags;
        set = t->set;
        C->qh[0]++;
        C->qh[1]++;
        if (tkflags & LSARB_F_EXTRA) C->drop_pending |= 2u;
        if (!(tkflags & LSARB_F_DROP)) break;
        if (m_fill(C, 0, now) == 0 || m_fill(C, 1, now) == 0) {
            C->stall++;
            return;
        }
    }
    extra = (C->drop_pending & 2u) ? 1 : 0;
    C->drop_pending = 0;
    if (C->tick < 6000u) {
        C->log_set[C->tick] = set;
        C->log_x[C->tick] = (uint8_t)extra;
    }
    C->tick++;
    in.tick = C->tick;
    in.stall_total = C->stall;
    /* Rahmenbau (FUN_0001D010) */
    for (i = 0; i <= extra; i++) {
        u32 fl = 0;
        if (C->newarb) {
            if ((in.tick % 20u) == 0) lsarb_need_update(&C->st, &C->cfg, k->rtt_meas);
            lsarb_tk_step(&C->cfg, &C->st, &C->ov, &in, &o);
        } else {
            lsarb_orig_step(&C->ov, &in, &o);
        }
        fl = o.flags_or;
        if (o.mask_clear) {
            fl |= o.mask_clear << 1; /* Markierung im Modell: Rauswurf */
            C->kicked = 1;
            O->kicked = 1; /* alle Clients entfernen beim Entnehmen dieses Rahmens */
        }
        m_send(C, me, O, k, now, fl);
    }
}

typedef struct { double la, lb; u32 sa, sb, kicked, ticks, same, xa; } mres;

static void m_run(const mlink *k, int new_a, int new_b, int ms, mres *r)
{
    static mcl A, B;
    int t, i, L0, delay0;
    memset(&A, 0, sizeof A);
    memset(&B, 0, sizeof B);
    s_mr = 0x2468ACE1u;
    s_last_arr[0] = s_last_arr[1] = 0;
    A.newarb = new_a;
    B.newarb = new_b;
    lsarb_cfg_default(&A.cfg);
    lsarb_cfg_default(&B.cfg);
    lsarb_reset(&A.st, &A.cfg);
    lsarb_reset(&B.st, &B.cfg);
    delay0 = (k->owd < 100 ? 100 : k->owd) / 50; /* clamp(Ping = RTT/2, 100, 5000)/50 */
    A.ov.D = B.ov.D = A.ov.delay0 = B.ov.delay0 = delay0;
    L0 = delay0 + 2;
    for (i = 0; i < L0; i++) { /* Vorlauf beim Start (FUN_0001D3D0) */
        m_send(&A, 0, &B, k, 0, 0);
        m_send(&B, 1, &A, k, k->phase_b, 0);
    }
    for (t = 50; t < ms; t += 1) {
        if (t % 50 == 0) m_pass(&A, 0, &B, k, t);
        if ((t - k->phase_b) % (k->pb > 0 ? k->pb : 50) == 0 && t > k->phase_b) m_pass(&B, 1, &A, k, t);
    }
    memset(r, 0, sizeof *r);
    r->la = A.ln ? (double)A.lsum / A.ln : 0;
    r->lb = B.ln ? (double)B.lsum / B.ln : 0;
    r->sa = A.stall;
    r->sb = B.stall;
    r->kicked = A.kicked || B.kicked;
    r->ticks = A.tick < B.tick ? A.tick : B.tick;
    r->same = 1;
    for (i = 0; i < (int)r->ticks && i < 6000; i++) {
        if (A.log_set[i] != B.log_set[i] || A.log_x[i] != B.log_x[i]) { r->same = 0; break; }
        r->xa += A.log_x[i];
    }
}

static void unit_tests(void)
{
    lsarb_cfg c;
    lsarb_st st;
    lsarb_ovars ov;
    lsarb_in in;
    lsarb_out o;
    lsarb_cfg_default(&c);
    LCHECK(lsarb_cfg_parse(&c, "lmin=3,drop_rmin=4;hold0=50") == 0 && c.lmin == 3 && c.drop_rmin == 4 && c.hold0 == 50,
           "loc_ls_opts nicht uebernommen");
    LCHECK(lsarb_cfg_parse(&c, "foo=1,lmin") == 2, "ungueltige Teile nicht gezaehlt");
    lsarb_cfg_default(&c);
    lsarb_reset(&st, &c);
    LCHECK(lsarb_need_update(&st, &c, 34) == 2, "Untergrenze nah (RTT 30 + 2x2) != 2");
    LCHECK(lsarb_need_update(&st, &c, 186) == 3, "Untergrenze fern (RTT 170 + 2x8) != 3");
    LCHECK(lsarb_need_update(&st, &c, 266) == 4, "Untergrenze Uebersee (RTT 250 + 2x8) != 4");
    LCHECK(lsarb_need_update(&st, &c, 245) == 4, "Hysterese: 245 ms muss bei 4 bleiben");
    LCHECK(lsarb_need_update(&st, &c, 205) == 3, "Hysterese: 205 ms muss auf 3");
    LCHECK(lsarb_need_update(&st, &c, -1) == 2, "RTT unbekannt -> Lmin");
    LCHECK(lsarb_timekeeper(4, 0xCu) == 2 && lsarb_timekeeper(2, 0) == -1, "Taktgeber falsch");
    /* Original-Regel @0x1D31D: der Taktgeber mit vollstem Ring nimmt sich selbst aus der Maske */
    memset(&ov, 0, sizeof ov);
    ov.D = ov.delay0 = 2;
    memset(&in, 0, sizeof in);
    in.npl = 2;
    in.mask = 3;
    in.self = in.local = 0;
    in.q[0] = 47;
    in.q[1] = 3;
    in.tick = 101;
    lsarb_orig_step(&ov, &in, &o);
    LCHECK(o.tk_pass && o.mask_clear == 1u, "Original: Platz 0 (vollster Ring) muesste fallen (0x%X)", o.mask_clear);
    in.self = in.local = 1; /* gleiche Lage, aber wir sind nicht Taktgeber: keine Entscheidung */
    memset(&ov, 0, sizeof ov);
    ov.D = ov.delay0 = 2;
    lsarb_orig_step(&ov, &in, &o);
    LCHECK(!o.tk_pass && o.mask_clear == 0u, "Original: Nicht-Taktgeber darf nicht entscheiden");
    /* neu: gleiche Lage -> kein Rauswurf, Abbau */
    memset(&ov, 0, sizeof ov);
    lsarb_reset(&st, &c);
    in.self = in.local = 0;
    lsarb_tk_step(&c, &st, &ov, &in, &o);
    LCHECK(o.mask_clear == 0u && st.n_spike == 1u, "neu: Rauswurf oder Spitze nicht gezaehlt");
}

static void scen(const char *name, const mlink *k, int ms, double lmax_new, double lmin_gain)
{
    mres ro, rn, rm1, rm2;
    m_run(k, 0, 0, ms, &ro);
    m_run(k, 1, 1, ms, &rn);
    m_run(k, 1, 0, ms, &rm1); /* Taktgeber neu, Mitspieler Original */
    m_run(k, 0, 1, ms, &rm2); /* Taktgeber Original, Mitspieler neu */
    printf("  %-22s Original: L %.2f/%.2f Stau %u/%u Ticks %u Zusatz %u %s | neu: L %.2f/%.2f Stau %u/%u Ticks %u Zusatz %u"
           " %s\n",
           name, ro.la, ro.lb, ro.sa, ro.sb, ro.ticks, ro.xa, ro.kicked ? "RAUSWURF" : "-", rn.la, rn.lb, rn.sa, rn.sb,
           rn.ticks, rn.xa, rn.kicked ? "RAUSWURF" : "-");
    LCHECK(ro.same && rn.same && rm1.same && rm2.same, "%s: Seiten wenden Rahmen nicht im selben Tick an", name);
    LCHECK(!rn.kicked && !rm1.kicked, "%s: neuer Regler warf jemanden raus", name);
    LCHECK(rm1.ticks == rn.ticks && rm1.sa == rn.sa && rm1.xa == rn.xa,
           "%s: Mitspieler-Schalter aenderte das Ergebnis (nur der Taktgeber zaehlt)", name);
    LCHECK(rm2.ticks == ro.ticks && rm2.sa == ro.sa && rm2.kicked == ro.kicked,
           "%s: Mitspieler-Schalter aenderte das Original-Ergebnis", name);
    LCHECK(rn.la <= lmax_new, "%s: Vorlauf neu %.2f > %.2f", name, rn.la, lmax_new);
    if (!ro.kicked) {
        LCHECK(rn.la <= ro.la + 0.15, "%s: neu (%.2f) mehr Vorlauf als Original (%.2f)", name, rn.la, ro.la);
        if (lmin_gain > 0.0)
            LCHECK(rn.la + lmin_gain <= ro.la, "%s: neu (%.2f) nicht um %.1f unter Original (%.2f)", name, rn.la, lmin_gain, ro.la);
        LCHECK(rn.sa + rn.sb <= ro.sa + ro.sb + 20u, "%s: neu mehr Stau (%u/%u) als Original (%u/%u)", name, rn.sa, rn.sb, ro.sa, ro.sb);
    }
    LCHECK(rn.ticks + 60u >= (ro.kicked ? 0u : ro.ticks) * 98u / 100u, "%s: neu zu langsam (%u Ticks vs. %u)", name, rn.ticks, ro.ticks);
}

int lsarb_selftest(int *ok)
{
    mlink k;
    printf("LOC-LOCKSTEP-ARBITER (loc_lsarb): Einheiten + Lockstep-Modell (2 Clients, Taktgeber Platz 0)\n");
    unit_tests();
    memset(&k, 0, sizeof k);
    k.owd = 15; k.jit = 4; k.phase_b = 17; k.rtt_meas = 38;
    scen("nah 30 ms", &k, 240000, 2.3, 0.0);
    k.owd = 85; k.jit = 8; k.phase_b = 31; k.rtt_meas = 186;
    scen("fern 170 ms", &k, 240000, 3.6, 0.0);
    k.owd = 75; k.jit = 30; k.phase_b = 9; k.rtt_meas = 210;
    scen("fern 150 ms, Jitter 30", &k, 240000, 4.2, 1.0);
    k.owd = 85; k.jit = 8; k.phase_b = 31; k.rtt_meas = 186;
    k.spike_t0 = 60000; k.spike_t1 = 64000; k.spike_ms = 2600; /* Spitze: 4 s lang +2,6 s je Richtung */
    scen("fern + Spitze 2,6 s", &k, 240000, 4.2, 0.0);
    memset(&k, 0, sizeof k);
    k.owd = 15; k.jit = 4; k.phase_b = 17; k.rtt_meas = 38; k.pb = 55; /* Mitspieler 10 % langsamer (schwacher PC) */
    {
        mres ro, rn;
        m_run(&k, 0, 0, 240000, &ro);
        m_run(&k, 1, 1, 240000, &rn);
        printf("  %-22s Original: L %.2f Stau %u Ticks %u %s | neu: L %.2f Stau %u Ticks %u %s\n", "nah, B 10% langsamer",
               ro.la, ro.sa, ro.ticks, ro.kicked ? "RAUSWURF" : "-", rn.la, rn.sa, rn.ticks, rn.kicked ? "RAUSWURF" : "-");
        /* Original im Modell nur zur Info: im echten Lauf (V5, loc_fields=4) hielt die Original-Regelung L ~3 */
        LCHECK(!rn.kicked && rn.la <= 7.0 && rn.same, "langsamer Mitspieler: neu Rauswurf oder Vorlauf %.2f > 7", rn.la);
    }
    memset(&k, 0, sizeof k); /* wie Lauf V6: ab 40 s einfache Laufzeit 2,5 s (RTT 5000) */
    k.owd = 85; k.jit = 6; k.phase_b = 31; k.rtt_meas = 5018;
    k.spike_t0 = 40000; k.spike_t1 = 1000000000; k.spike_ms = 2415;
    {
        mres ro, rn;
        m_run(&k, 0, 0, 240000, &ro);
        m_run(&k, 1, 1, 240000, &rn);
        printf("  %-22s Original: L %.2f Stau %u Ticks %u %s | neu: L %.2f Stau %u Ticks %u %s\n", "RTT 5000 ab 40 s",
               ro.la, ro.sa, ro.ticks, ro.kicked ? "RAUSWURF" : "-", rn.la, rn.sa, rn.ticks, rn.kicked ? "RAUSWURF" : "-");
        LCHECK(ro.kicked, "RTT 5000: Original-Regel (Warteschlange > 45) muesste greifen (wie Lauf V6)");
        LCHECK(!rn.kicked && rn.same && rn.ticks > ro.ticks, "RTT 5000: neu Rauswurf/abweichend/kuerzer");
    }
    *ok += s_ok;
    return s_fail;
}
