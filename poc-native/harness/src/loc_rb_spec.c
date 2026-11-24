/* loc_rb_spec.c - W464 NET-CORE: aus loc_rollback.c verschoben (W390 LOC-OWN-PREDICT/W393), unveraendert.
 * Eigener spekulativer Vorlauf (nur Anzeige): Start/Abbruch/Ende des Vorlaufs, Kosten-Rueckfall, Missionsende. */
#include "loc_rb_int.h"

/* ================= W390 LOC-OWN-PREDICT: spekulativer Vorlauf ================= */
int s_spec_want, s_spec_n, s_spec_phase0;
u32 s_spec_gt0, s_spec_gt[RB_SPEC_MAXT], s_spec_reg0, s_spec_rjob_live, s_spec_fld_live;
static long s_spec_memev0;
static float s_spec_pose[RB_SPEC_MAXT][4];
int s_spec_pose_ok[RB_SPEC_MAXT];
static long long s_spec_t0;
u32 s_spec_runs, s_spec_ticks, s_spec_early, s_spec_skip[8], s_spec_rev_fail, s_spec_reg_grow, s_spec_memev,
    s_spec_ac_leak, s_spec_verify_n, s_spec_verify_bad, s_spec_files;
rb_ser_t *s_spec_s_us, *s_spec_s_rev, *s_spec_s_pages; /* je Vorlauf: Gesamtzeit, Zuruecksetzen (us), Seiten */
static u32 s_spec_win[RB_SPEC_WIN]; /* gleitend: Gesamtzeit der letzten 100 Vorlaeufe (Rueckfall) */
static int s_spec_prep;  /* Render-Vorbereitung im Vorlauf (loc_own_predict_prep) */
static volatile LONG s_spec_hud_us, s_spec_hud_p95, s_spec_hud_lead; /* F3: letzter Vorlauf, p95 (us), Vorlauf in Ticks */

int loc_rb_spec(void) { return s_spec && GetCurrentThreadId() == s_game_tid; }
void loc_rb_spec_note(int kind)
{
    if (kind > 0 && kind < 6) s_spec_ev.kfile[kind]++;
}
u32 s_sc_spec[512], s_sc_live[512];
void loc_rb_sc_count(unsigned ord)
{
    if (ord >= 512u || GetCurrentThreadId() != s_game_tid) return;
    if (s_spec) s_sc_spec[ord]++;
    else if (s_phase == PH_RECORD) s_sc_live[ord]++;
}

/* W390: F3-Overlay - 0 = nicht aktiv; mode 1 an, 2 Pause, 3 aus fuer die Mission; Vorsprung (ms), Kosten (us, p95). */
int loc_rb_ownpred_hud(int *mode, int *lead_ms, int *cost_us)
{
    if (!g_cfg.loc_own_predict || !s_rbi_ep || s_rbi_mst != RBN_ST_R2 || !s_spec_runs) return 0;
    *mode = s_spec_off_mission ? 3 : (s_spec_pause_until ? 2 : 1);
    *lead_ms = (int)(s_spec_hud_lead > 0 ? (s_spec_hud_lead - 1) * 50 : 0);
    *cost_us = (int)(s_spec_hud_p95 ? s_spec_hud_p95 : s_spec_hud_us);
    return 1;
}

static int rb_spec_skip(int why)
{
    if (why >= 0 && why < 8) s_spec_skip[why]++;
    return 0;
}

/* Am Ende eines Live-Ticks (rbi_after), wenn der Schatten = aktueller Stand ist (snap_ok). 1 = Vorlauf gestartet. */
int rb_spec_try_start(int snap_ok)
{
    int d;
    u32 k;
    float p[4];
    if (!g_cfg.loc_own_predict || !s_rbi_ep || !s_rbc) return 0;
    if (s_spec_xcn) { /* XInput aus dem letzten Vorlauf, das der Live-Tick nicht abgeholt hat */
        for (k = 0; k < s_spec_xcn; k++)
            if (!s_spec_xc[k].used) s_spec_xc_unused++;
        s_spec_xcn = 0;
    }
    if (s_spec_off_mission) return rb_spec_skip(4);
    /* W411: Missionsende laeuft (Zaehler != 0) bzw. die Schleife endet nach diesem Tick: kein Vorlauf - er wuerde die
     * Schleife in der verworfenen Zeitlinie verlassen (Render-Thread angehalten, Auftrag 1 -> ewiges Kreisen @0x2F355). */
    if (s_endguard && rd32(RB_END_CNT) != 0) return rb_spec_skip(7);
    if (!snap_ok) return rb_spec_skip(0);
    if (!s_ring || !s_ready || s_failed || !rbr_active() || !rbi_active(s_rbc) || rbi_replaying(s_rbc) ||
        rbi_free_running(s_rbc) || s_phase != PH_RECORD || s_rbi_mst != RBN_ST_R2)
        return rb_spec_skip(6);
    if (s_spec_pause_until && (LONG)(GetTickCount() - s_spec_pause_until) < 0) return rb_spec_skip(4);
    s_spec_pause_until = 0;
    if (g_exreg_n != s_reg_gen || g_loc_rb_memev != s_memev_seen || s_r_force_rebuild) return rb_spec_skip(1);
    d = rbi_delay_eff(s_rbc);
    if (d < 0) return rb_spec_skip(6);
    s_spec_want = g_cfg.loc_own_predict_ticks > 0 ? g_cfg.loc_own_predict_ticks : d + 1 - s_spec_level;
    if (s_spec_want < 1) s_spec_want = 1;
    if (s_spec_want > RB_SPEC_MAXT) s_spec_want = RB_SPEC_MAXT;
    if (!loc_smooth_own_read(p)) return rb_spec_skip(5);
    if (rd32(RB_RJOB) == 1u) return rb_spec_skip(2); /* Render-Thread zeichnet noch: nicht warten */
    rb_rthread_check();
    if (s_rthread) {
        if (!rb_rs_suspend(s_rthread, 2)) return rb_spec_skip(2); /* W411: gebucht */
        if (rd32(RB_RJOB) == 1u) {
            rb_rs_resume(); /* W411 */
            return rb_spec_skip(2);
        }
    }
    /* W393 LOC-PACING loc_pd_spec=1: Present-Takt laeuft weiter. Er liest nur Host-Daten (aufgezeichnete Listen,
     * Lage per Seqlock, loc_smooth) und den D3D-Kontext unter ctx_lock; der Vorlauf schreibt nur Gast-Speicher
     * (danach zurueckgesetzt), der Render-Thread des Spiels ruht. So faellt nie ein Bild wegen des Vorlaufs aus. */
    s_spec_pd = s_pdpause && !g_cfg.loc_pd_spec;
    if (s_spec_pd && !sb_interp_rb_pause(1)) {
        sb_interp_rb_pause(0);
        rb_rs_resume(); /* W411: nur, wenn gebucht */
        return rb_spec_skip(3);
    }
    if (s_freeze) rb_freeze_others();
    { /* Belegungen aus frueheren Vorlaeufen, die die echte Zeitlinie nicht wollte: ohne Besitzer (Leck, gezaehlt) */
        u32 now = rd32(RB_TICK), w = 0;
        for (k = 0; k < s_spec_ac_n; k++) {
            if (s_spec_ac[k].gt + 300u < now) s_spec_ac_leak++;
            else s_spec_ac[w++] = s_spec_ac[k];
        }
        s_spec_ac_n = w;
    }
    s_spec_gt0 = rd32(RB_TICK);
    s_spec_esp0 = s_rb_esp0;
    s_spec_caller = s_rb_caller;
    s_spec_rjob_live = rd32(RB_RJOB);
    s_spec_fld_live = rd32(RBI_FIELD_LAST);
    s_spec_reg0 = g_exreg_n;
    s_spec_memev0 = g_loc_rb_memev;
    memset(s_spec_k, 0, sizeof s_spec_k);
    memset(s_spec_pose_ok, 0, sizeof s_spec_pose_ok);
    s_spec_own_have = 0;
    s_spec_n = 0;
    s_spec_cm_n = 0;
    s_spec_prep = g_cfg.loc_own_predict_prep;
    /* Render-Vorbereitung (W359: reiner Spielspeicher - Freigabe-Warteschlange, Doppelpuffer) ist fuer die VT-Lage
     * ohne Belang und wird ohnehin zurueckgesetzt: im Vorlauf standardmaessig auslassen (Auftrag gilt als belegt). */
    *(volatile u32 *)(uintptr_t)RB_RJOB = rbi_rjob_replay(s_spec_prep, RBI_RJOB_LOAD, rd32(RB_RJOB));
    s_spec_phase0 = s_phase;
    s_phase = PH_REPLAY;
    s_spec = 1;
    InterlockedExchange(&s_skipwait, 1);
    s_spec_t0 = rb_now();
    return 1;
}

void rb_spec_finish(void)
{
    u32 pages = 0, chk = 0, us = 0, tot, k;
    int rv = rbr_revert(&pages, &chk, &us);
    if (rv != 0) {
        s_spec_rev_fail++;
        if (s_spec_rev_fail <= 4) HLOG_E("LOCOWNPRED: Zuruecksetzen nach dem Vorlauf fehlgeschlagen (Ring aus) - Zustand weicht ab");
    }
    *(volatile u32 *)(uintptr_t)RB_RJOB = s_spec_rjob_live;
    *(volatile u32 *)(uintptr_t)RBI_FIELD_LAST = s_spec_fld_live;
    for (k = 0; k < s_spec_cm_n; k++) { /* im Vorlauf committet: Inhalt gehoert der verworfenen Zeitlinie */
        if (s_pcom_n < RB_PCOM_MAX) {
            s_pcom[s_pcom_n].a = s_spec_cm[k].a;
            s_pcom[s_pcom_n].end = s_spec_cm[k].end;
            s_pcom[s_pcom_n].live = 0;
            s_pcom_n++;
        } else {
            s_pcom_full++;
        }
    }
    s_spec_cm_n = 0;
    s_spec_files += (u32)krnl_file_spec_restore();
    if (g_exreg_n != s_spec_reg0) s_spec_reg_grow++; /* Neuaufbau folgt im naechsten rbi_after (wie jede Belegung) */
    if (g_loc_rb_memev != s_spec_memev0) s_spec_memev++;
    if ((g_cfg.loc_own_predict_diag & 1) && (s_spec_runs % 50u) == 0u) { /* DIAG: Schatten = Live nach dem Zuruecksetzen */
        u32 a[2] = {0, 0}, nd = rbr_shadow_diff(a, 2);
        s_spec_verify_n++;
        if (nd) {
            s_spec_verify_bad++;
            if (s_spec_verify_bad <= 8) HLOG_W("LOCOWNPRED: DIAG Pruefung nach dem Zuruecksetzen: %u Seiten abweichend (erste 0x%X)", nd, a[0]);
        }
    }
    if (s_freeze) rb_thaw_others();
    rb_rs_resume(); /* W411: nur, wenn gebucht */
    if (s_spec_pd) sb_interp_rb_pause(0); /* W393 */
    s_spec = 0;
    s_phase = s_spec_phase0;
    InterlockedExchange(&s_skipwait, 0);
    tot = rb_us(rb_now() - s_spec_t0);
    s_spec_runs++;
    s_spec_ticks += (u32)s_spec_n;
    rb_put(s_spec_s_us, tot);
    rb_put(s_spec_s_rev, us);
    rb_put(s_spec_s_pages, pages);
    s_spec_win[s_spec_win_n++ % RB_SPEC_WIN] = tot;
    InterlockedExchange(&s_spec_hud_us, (LONG)tot);
    InterlockedExchange(&s_spec_hud_lead, (LONG)s_spec_n);
    if (s_spec_win_n >= RB_SPEC_WIN && (s_spec_win_n % 20u) == 0u) {
        /* Rueckfall: p75 der letzten 100 Vorlaeufe > Grenze (dauerhaft teuer, z. B. schwache CPU; einzelne Ausreisser
         * durch Fremdlast loesen nicht aus). Stufe 1: ein Tick weniger; danach 30 s Pause (Stufe wieder 0); nach 5
         * Pausen aus fuer die Mission. */
        u32 tmp[RB_SPEC_WIN], lim = (u32)(g_cfg.loc_own_predict_max_ms > 0 ? g_cfg.loc_own_predict_max_ms : 5) * 1000u, p75;
        memcpy(tmp, s_spec_win, sizeof tmp);
        qsort(tmp, RB_SPEC_WIN, sizeof tmp[0], rb_u32cmp);
        p75 = tmp[(RB_SPEC_WIN * 75) / 100];
        InterlockedExchange(&s_spec_hud_p95, (LONG)tmp[(RB_SPEC_WIN * 95) / 100]);
        if (p75 > lim) {
            s_spec_win_n = 0;
            if (s_spec_level == 0 && s_spec_n > 1) {
                s_spec_level = 1;
                s_spec_degr++;
                HLOG_W("LOCOWNPRED: Vorlauf teuer (p75 %u us > %u us) - ein Tick weniger", p75, lim);
            } else if (++s_spec_paused_n >= 5) {
                s_spec_off_mission = 1;
                HLOG_W("LOCOWNPRED: Vorlauf zu teuer (p75 %u us > %u us) - fuer diese Mission aus (5. Pause)", p75, lim);
            } else {
                s_spec_level = 0;
                s_spec_pause_until = GetTickCount() + 30000u;
                if (!s_spec_pause_until) s_spec_pause_until = 1;
                HLOG_W("LOCOWNPRED: Vorlauf zu teuer (p75 %u us > %u us) - 30 s Pause (%d. Mal)", p75, lim, s_spec_paused_n);
            }
        }
    }
    loc_smooth_own_pred(s_spec_gt0 + 1u, s_spec_n, s_spec_gt, (const float(*)[4])s_spec_pose, s_spec_pose_ok);
}

/* W411: Vorlauf ohne Zuruecksetzen beenden (nur Spiel-Thread). Fuer Lagen, in denen Zuruecksetzen falsch waere: die
 * Missionsschleife ist schon verlassen bzw. eine neue Mission laeuft (Spieltick kleiner als beim Start). Mit endguard=1
 * tritt das nicht ein (Zaehler bleiben 0); ohne ihn bleibt der Stand der verworfenen Zeitlinie stehen (Mission ist zu Ende). */
void rb_spec_abandon(const char *why)
{
    s_spec_abandon_n++;
    HLOG_W("LOCOWNPRED: Vorlauf ohne Zuruecksetzen beendet (%s, Spieltick %u, Start %u, %d Ticks)", why, rd32(RB_TICK),
           s_spec_gt0, s_spec_n);
    s_spec_cm_n = 0;
    if (s_freeze) rb_thaw_others();
    rb_rs_resume();
    if (s_spec_pd) sb_interp_rb_pause(0);
    s_spec = 0;
    s_phase = s_spec_phase0;
    s_spec_off_mission = 1;
    memset(s_spec_pose_ok, 0, sizeof s_spec_pose_ok);
    InterlockedExchange(&s_skipwait, 0);
}

/* rb_after waehrend des Vorlaufs: ein Vorlauf-Tick ist fertig gerechnet. */
void rb_spec_tick(void)
{
    *(volatile u32 *)(uintptr_t)RB_RJOB = rbi_rjob_replay(s_spec_prep, RBI_RJOB_TICK, rd32(RB_RJOB));
    if (s_spec_n < RB_SPEC_MAXT) {
        s_spec_gt[s_spec_n] = rd32(RB_TICK);
        s_spec_pose_ok[s_spec_n] = loc_smooth_own_read(s_spec_pose[s_spec_n]);
    }
    s_spec_n++;
    if (s_spec_n >= s_spec_want) {
        rb_spec_finish();
    } else if (s_endguard && rb_loop_exits()) {
        /* W411: die verworfene Zeitlinie wuerde die Missionsschleife jetzt verlassen (Ende-Zaehler an der Grenze, z. B.
         * Rauswurf/Abbruch mit Grenze 0): sofort zuruecksetzen - die Ende-Pruefung danach sieht den Live-Stand. */
        s_spec_endstop++;
        if (s_spec_endstop <= 4)
            HLOG_I("LOCOWNPRED: Vorlauf erreicht Missionsende (Tick %u, Zaehler %u/%u) - vorzeitig zurueckgesetzt",
                   rd32(RB_TICK), rd32(RB_END_CNT), rd32(RB_END_LIM));
        rb_spec_finish();
    } else if (rb_us(rb_now() - s_spec_t0) > (u32)(g_cfg.loc_own_predict_max_ms > 0 ? g_cfg.loc_own_predict_max_ms : 5) * 3000u) {
        s_spec_early++; /* einzelner Ausreisser: mit weniger Vorlauf beenden */
        rb_spec_finish();
    }
}

