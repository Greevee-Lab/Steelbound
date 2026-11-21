/* loc_lsarb_game.c - LOC-LOCKSTEP-ARBITER: Anbindung des Pufferreglers (loc_lsarb.c) an FUN_0001D010 (nur LoC-Profil).
 *
 * Thunk @0x1D0BF (Befehl "mov ebx,[0x3071A0]", 6 Byte): mit loc_ls_arbiter=1 ersetzt lsarb_game_new die Regelung bis
 * 0x1D381 (Kopf mit Maske/Flags in [esp+0x10] der Spielfunktion); mit 0 laeuft der Originalcode, auf Wunsch nur
 * beobachtet (loc_sync=1: Messzeile LOCLSARB, Vorhersage des Original-Rauswurfs). Mit Rollback (R2, loc_rb_input=1)
 * liegt dort schon rbi_reg_thunk (loc_rollback.c); dessen Lockstep-Zweig springt hierher.
 * Logs nur mit Zaehlern/Fuellstaenden, keine Eingaben, keine Paketinhalte. */
#include <winsock2.h>
#include <windows.h>
#include "loc_lsarb.h"
#include "loc_lsarb_game.h"
#include "hcfg.h"
#include "hlog.h"
#include "loc_net.h"
#include "loc_net_core.h"

#include <string.h>

typedef uint32_t u32;

#define LA_VA 0x1D0BFu
#define LA_MASK 0x306830u
#define LA_NPL 0x3071A0u
#define LA_LOBBY 0x361708u /* Zeiger; +0x5180+4i = Ringfuellstand */
#define LA_SELF 0x3071B0u
#define LA_LOCAL 0x306A84u
#define LA_D 0x306F04u
#define LA_PEAK 0x3067B4u
#define LA_ACC 0x30719Cu
#define LA_TICK 0x3070F4u
#define LA_STEPS 0x306944u
#define LA_COOL 0x306B58u
#define LA_LASTT 0x2C9B40u
#define LA_STALLF 0x306C34u
#define LA_XTRA 0x28DFA8u /* je Spieler: angewandte Zusatzrahmen (FUN_0001D4C0) */

#define G32(a) (*(volatile u32 *)(uintptr_t)(a))
#define GI(a) (*(volatile int *)(uintptr_t)(a))

static int s_on, s_mode, s_obs;
static lsarb_cfg s_cfg;
static lsarb_st s_st;
static lsarb_ovars s_ov;
/* Messfenster (je Tick einmal) */
static u32 s_stat_tick, s_win_t0, s_win_n, s_win_stall0, s_win_xtra0, s_mask_last, s_logs_kick, s_logs_mask, s_have;
static long long s_win_lsum;
static int s_win_lmin, s_win_lmax, s_win_rmin, s_win_qmax, s_win_tk;
static long long s_win_rsum;
static u32 s_win_rn, s_spike_logged, s_win_add0, s_win_drop0, s_win_fast0, s_win_rtt0;
static int s_rtt_last = -1;
/* F3 */
static volatile LONG s_hud_ms, s_hud_lead, s_hud_tk;

static u32 la_xtra_sum(int npl)
{
    u32 s = 0;
    int i;
    for (i = 0; i < npl && i < LSARB_MAXP; i++) s += G32(LA_XTRA + 4u * (u32)i);
    return s;
}

static void la_fill(lsarb_in *in)
{
    int i;
    u32 lob = G32(LA_LOBBY);
    memset(in, 0, sizeof *in);
    in->npl = GI(LA_NPL);
    if (in->npl > LSARB_MAXP) in->npl = LSARB_MAXP;
    if (in->npl < 0) in->npl = 0;
    in->mask = G32(LA_MASK) & 0x3FFu;
    in->self = GI(LA_SELF);
    in->local = GI(LA_LOCAL);
    for (i = 0; i < in->npl; i++) in->q[i] = lob ? GI(lob + 0x5180u + 4u * (u32)i) : 0;
    in->tick = G32(LA_TICK);
    in->stall_total = G32(LA_STALLF);
}

/* RTT-Schaetzung mit Jitter-Reserve (ms): eigener Transport = schlechtester verbundener Mitspieler SRTT + 2*RTTVAR,
 * sonst der QoS-Wert des Netz-HLE (Verzoegerungsstufe: Strecke + 2*Jitter); -1 = unbekannt */
static int la_rtt(void)
{
    loc_net_hud h;
    struct locnet *n;
    if (loc_net_hud_read(&h) && h.tun && h.up > 0 && h.ping > 0.0) return (int)(h.ping + 2.0 * h.jitter + 0.5);
    n = loc_net_core();
    if (n && n->rtt_ms > 0) return n->rtt_ms;
    return -1;
}

static void la_win_reset(const lsarb_in *in)
{
    s_win_t0 = in->tick;
    s_win_n = 0;
    s_win_lsum = 0;
    s_win_rsum = 0;
    s_win_rn = 0;
    s_win_lmin = 0x7FFF;
    s_win_lmax = 0;
    s_win_rmin = 0x7FFF;
    s_win_qmax = 0;
    s_win_stall0 = in->stall_total;
    s_win_xtra0 = la_xtra_sum(in->npl);
    s_win_add0 = s_st.n_add;
    s_win_drop0 = s_st.n_drop;
    s_win_fast0 = s_st.n_fast;
    s_win_rtt0 = s_st.n_add_rtt;
}

static void la_stats(const lsarb_in *in, const lsarb_out *o)
{
    int tk = lsarb_timekeeper(in->npl, in->mask);
    if (!s_have || in->tick < s_stat_tick) { /* neue Mission */
        s_have = 1;
        s_mask_last = in->mask;
        s_spike_logged = 0;
        lsarb_reset(&s_st, &s_cfg);
        s_rtt_last = -1;
        la_win_reset(in);
        HLOG_I("LOCLSARB: Mission im Lockstep, Regler %s, Taktgeber Platz %d (%s), Plaetze %d", s_mode ? "neu" : "Original",
               tk, tk == in->self ? "wir" : "Mitspieler", in->npl);
    }
    if ((in->mask & s_mask_last) != s_mask_last && s_logs_mask < 40) { /* Platz faellt aus der Aktiv-Maske */
        s_logs_mask++;
        HLOG_W("LOCLSARB: Aktiv-Maske 0x%X -> 0x%X bei Tick %u (eigener Platz %d, Taktgeber %d, qmax %d, Regler %s)",
               s_mask_last, in->mask, in->tick, in->self, tk, o->qmax, s_mode ? "neu" : "Original");
    }
    s_mask_last = in->mask;
    if (in->tick == s_stat_tick) return;
    s_stat_tick = in->tick;
    s_win_n++;
    s_win_lsum += o->lead;
    if (o->lead < s_win_lmin) s_win_lmin = o->lead;
    if (o->lead > s_win_lmax) s_win_lmax = o->lead;
    if (o->r >= 0) {
        s_win_rsum += o->r;
        s_win_rn++;
        if (o->r < s_win_rmin) s_win_rmin = o->r;
    }
    if (o->qmax > s_win_qmax) s_win_qmax = o->qmax;
    s_win_tk = tk == in->self;
    InterlockedExchange(&s_hud_ms, (LONG)GetTickCount());
    InterlockedExchange(&s_hud_lead, o->lead);
    InterlockedExchange(&s_hud_tk, s_win_tk);
    if (s_win_n >= 200u) { /* 10 s */
        u32 st = in->stall_total - s_win_stall0, xt = la_xtra_sum(in->npl) - s_win_xtra0;
        double la = (double)s_win_lsum / (double)s_win_n;
        HLOG_I("LOCLSARB: t=%us Regler=%s Taktgeber=%s Vorlauf L=%.2f (%d..%d) = %d ms Rest r=%d/%.2f qmax=%d Stau+%u "
               "Zusatz+%u Entscheid +%u/+%u -%u/-%u RTT=%d Untergrenze=%d Spitzen>45=%u",
               in->tick / 20u, s_mode ? "neu" : "Original", s_win_tk ? "ja" : "nein", la, s_win_lmin, s_win_lmax,
               (int)(la * 50.0 + 0.5), s_win_rmin == 0x7FFF ? -1 : s_win_rmin,
               s_win_rn ? (double)s_win_rsum / (double)s_win_rn : -1.0, s_win_qmax, st, xt, s_st.n_add - s_win_add0,
               s_st.n_add_rtt - s_win_rtt0, s_st.n_drop - s_win_drop0, s_st.n_fast - s_win_fast0, s_rtt_last,
               s_st.need, s_st.n_spike);
        la_win_reset(in);
    }
}

/* loc_ls_arbiter=0 (nur mit loc_sync=1): beobachten, Originalcode laeuft danach */
static void __cdecl la_observe(void)
{
    lsarb_in in;
    lsarb_out o;
    la_fill(&in);
    lsarb_measure(&in, &o);
    if (lsarb_timekeeper(in.npl, in.mask) == in.self && G32(LA_LASTT) != in.tick && G32(LA_COOL) == 0 &&
        o.qmax > LSARB_KICK_Q) {
        u32 vict = 0;
        int i;
        s_st.n_spike++;
        for (i = 0; i < in.npl; i++)
            if ((in.mask & (1u << i)) && in.q[i] == o.qmax) vict |= 1u << i;
        if (s_logs_kick < 40) {
            s_logs_kick++;
            HLOG_W("LOCLSARB: Original-Regel @0x1D31D bei Tick %u: qmax %d > 45 -> Taktgeber nimmt Plaetze 0x%X aus der "
                   "Maske (vollster Ring; eigener Platz %d, knappster Mitspieler-Ring %d)",
                   in.tick, o.qmax, vict, in.self, o.r);
        }
    }
    if ((in.tick % 20u) == 0 && in.tick != s_stat_tick) { /* nur Anzeige: RTT und Untergrenze des neuen Reglers */
        s_rtt_last = la_rtt();
        lsarb_need_update(&s_st, &s_cfg, s_rtt_last);
    }
    la_stats(&in, &o);
}

/* loc_ls_arbiter=1: Regelung 0x1D0BF..0x1D381 ersetzen; flags = [esp+0x10] von FUN_0001D010 */
static void __cdecl la_new(u32 *flags)
{
    lsarb_in in;
    lsarb_out o;
    u32 peak;
    la_fill(&in);
    s_ov.lasttick = G32(LA_LASTT);
    s_ov.have_last = 1;
    lsarb_tk_step(&s_cfg, &s_st, &s_ov, &in, &o);
    G32(LA_LASTT) = s_ov.lasttick;
    GI(LA_STEPS) = o.steps;
    peak = G32(LA_PEAK);
    if ((u32)o.qmax > peak) G32(LA_PEAK) = (u32)o.qmax;
    if (in.self >= 0 && in.self < LSARB_MAXP) G32(LA_ACC) += (u32)in.q[in.self];
    GI(LA_D) = o.lead > 0 ? o.lead : 1; /* Anzeige (LOCSYNC Verz=): wirksamer Vorlauf */
    if (o.tk_pass && (in.tick % 20u) == 0) { /* RTT-Untergrenze je Sekunde */
        s_rtt_last = la_rtt();
        lsarb_need_update(&s_st, &s_cfg, s_rtt_last);
    }
    *flags |= o.flags_or;
    if (o.tk_pass && o.qmax > LSARB_KICK_Q && s_spike_logged < 20 && (s_st.n_spike % 100u) == 1u) {
        s_spike_logged++;
        HLOG_I("LOCLSARB: qmax %d > 45 bei Tick %u - kein Rauswurf (Original haette Platz mit vollstem Ring entfernt), "
               "Vorlauf %d, knappster Mitspieler-Ring %d", o.qmax, in.tick, o.lead, o.r);
    }
    la_stats(&in, &o);
}

static int s_mode_ref, s_obs_ref; /* fuer den Thunk (einfache Wortvergleiche) */

__declspec(naked) void loc_lsarb_reg_entry(void)
{
    __asm {
        cmp dword ptr [s_mode_ref], 0
        jne la_newp
        cmp dword ptr [s_obs_ref], 0
        je la_orig
        pushad
        call la_observe
        popad
    la_orig:
        mov ebx, dword ptr ds:[3071A0h]
        push 1D0C5h
        ret
    la_newp:
        pushad
        lea eax, [esp + 30h]
        push eax
        call la_new
        add esp, 4
        popad
        push 1D381h
        ret
    }
}

int loc_lsarb_hud(int *mode, int *lead, int *tk)
{
    LONG t = s_hud_ms;
    if (!s_on || !t || GetTickCount() - (DWORD)t > 2000u) return 0;
    if (mode) *mode = s_mode;
    if (lead) *lead = (int)s_hud_lead;
    if (tk) *tk = (int)s_hud_tk;
    return 1;
}

int loc_lsarb_install(void)
{
    static const uint8_t orig[6] = {0x8B, 0x1D, 0xA0, 0x71, 0x30, 0x00}; /* mov ebx,[3071A0h] */
    uint8_t *p = (uint8_t *)(uintptr_t)LA_VA;
    int bad;
    s_mode = g_cfg.loc_ls_arbiter > 0 ? 1 : 0;
    s_obs = s_mode || g_cfg.loc_sync > 0;
    lsarb_cfg_default(&s_cfg);
    bad = lsarb_cfg_parse(&s_cfg, g_cfg.loc_ls_opts);
    if (bad) HLOG_W("LOCLSARB: loc_ls_opts mit %d unbekannten/ungueltigen Teilen (ignoriert)", bad);
    lsarb_reset(&s_st, &s_cfg);
    if (!s_obs) return 0; /* Original ohne Beobachtung: kein Eingriff (R2-Thunk springt trotzdem sicher hierher) */
    if (memcmp(p, orig, sizeof orig) == 0) {
        DWORD old;
        intptr_t rel = (intptr_t)(void *)loc_lsarb_reg_entry - (intptr_t)(p + 5);
        VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &old);
        p[0] = 0xE9;
        memcpy(p + 1, &rel, 4);
        p[5] = 0x90;
        VirtualProtect(p, 6, old, &old);
        FlushInstructionCache(GetCurrentProcess(), p, 6);
    } else if (p[0] != 0xE9) {
        HLOG_E("LOCBYTES LOCLSARB @0x%X: Byte-Mismatch - Regler aus (Original-Lockstep)", LA_VA);
        return 0;
    } /* sonst: R2-Thunk liegt dort und springt im Lockstep-Zweig hierher */
    s_mode_ref = s_mode;
    s_obs_ref = s_obs;
    s_on = 1;
    HLOG_I("LOCLSARB: aktiv @0x%X, Regler %s (loc_ls_arbiter=%d): Lmin %d, Stau %d/%d Ticks, Rahmen weg ab Rest %d "
           "(%d Ticks), Abbau ab Rest %d, Sperre %d..%d, RTT-Reserve %d ms, %s",
           LA_VA, s_mode ? "neu" : "Original (nur Messung)", g_cfg.loc_ls_arbiter, s_cfg.lmin, s_cfg.add_stalls, s_cfg.add_win, s_cfg.drop_rmin, s_cfg.drop_win, s_cfg.fast_rmin, s_cfg.hold0,
           s_cfg.hold_max, s_cfg.need_add_ms,
           s_mode ? "kein Rauswurf nach Warteschlange" : "Original-Regeln inkl. Grenze 45 (@0x1D31D)");
    return 1;
}
