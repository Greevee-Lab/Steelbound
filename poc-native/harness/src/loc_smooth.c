/* loc_smooth.c - W385 LOC-SMOOTH (siehe loc_smooth.h). Spiel-Thread: Lagen vor/nach dem Rollback, Abklingen,
 * Veroeffentlichung je Tick; Render-Thread: nur loc_smooth_get / loc_smooth_render_note. */
#include "loc_smooth.h"
#include "loc_smooth_math.h"
#include "loc_rbi.h"
#include "hcfg.h"
#include "hlog.h"
#include "sb_profile.h"
#include "loc_aim.h" /* W390: DIAG-Datei (loc_aim_diag_file) */
#include "loc_sync.h"
#include "render_pace.h" /* W393 LOC-PACING: letzter fertiger Stand ohne Sperre */

#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef uint32_t u32;

#define LS_PLAYER 0x306A84u /* eigener Spieler-Index (W341) */
#define LS_VT0 0x5C50D0u    /* VT-Objekt 0 (W376) */
#define LS_VSTRIDE 0xAE8u
#define LS_NS 4096u         /* Ringe fuer Perzentile */
#define LS_EPS_POS 0.05     /* kleinere Spruenge zaehlen nicht (Spieleinheiten) */
#define LS_EPS_YAW 1e-4     /* rad */
#define LS_WIN 4            /* Ticks nach einer Korrektur fuer die sichtbare Lageaenderung */

typedef struct {
    int ok;
    u32 obj;
    double p[3];
    double yaw;
} ls_vt_t;

static ls_vt_t s_pre[LS_MAXV];
static int s_pre_own = -1, s_pre_ok;
static u32 s_pre_tick;
static ls_track_t s_tr[LS_MAXV];
static loc_smooth_pub_t s_pub[PACE_PUB_SLOTS]; /* W393: drei Plaetze (render_pace.h), vorher zwei */
static volatile LONG s_pub_seq;
/* W387 LOC-SMOOTH-OPP: Lagen-Historie je VT (live + Neurechnung), Stand vor dem Rollback, Auto-Regelung, Statistik */
static ls_hist_t s_hist[LS_MAXV], s_hpre[LS_MAXV];
static int s_hpre_ok;
static ls_auto_t s_auto;
static double s_opp_d;                 /* Verzoegerung der zuletzt veroeffentlichten Liste (Ticks) */
static u32 s_opp_ev[4];                /* Korrekturen nach Bedarf 0 (unsichtbar), 1, 2, 3+ Ticks */
static u32 s_opp_hidden;               /* Korrekturen, deren Sprung die Verzoegerung ganz verdeckt hat (Bedarf <= d) */
static u32 s_opp_short, s_opp_none;    /* Historie verkuerzt bzw. fehlend (Versatz kleiner/0) */
static u32 s_opp_far;                  /* Verzoegerungs-Versatz > loc_rb_smooth_max (Neustart) -> 0 */
static double s_opp_dsum, s_opp_dmax;  /* Summe d je Live-Tick (Mittel), Maximum */
static u32 s_opp_dn, s_opp_dt[3];      /* Live-Ticks; Anteil d = 0, 0 < d < 1, d >= 1 */
static u32 s_opp_tgt_up, s_opp_tgt_dn; /* Zielwechsel auf/ab */
static u32 s_opp_pubtick, s_opp_tk_eq, s_opp_tk_ne; /* Pruefung: Rollback-Tick == zuletzt veroeffentlichter Tick */

/* Statistik (Spiel-Thread; die Zeile liest sie auf demselben Thread) */
typedef struct {
    float v[LS_NS];
    u32 n;
} ls_ring_t;
static ls_ring_t s_jo, s_jw, s_dist, s_wr, s_ws, s_cr, s_cs, s_base;
static ls_ring_t s_jd; /* W387: Sprung der angezeigten Lage anderer VTs je Korrektur (vor der W385-Glaettung) */
static ls_ring_t s_rcost; /* W387: Render-Kosten je Liste mit Versatz (us), unter s_rcs */
static ls_ring_t s_rv_wr, s_rv_ws, s_rv_b, s_rc_wr, s_rc_ws; /* Render: VT-Draws im Fenster roh/angezeigt, sonst; Auge */
/* W390 LOC-OWN-PREDICT: eigene Lage aus dem spekulativen Vorlauf (loc_rollback.c), nur Anzeige */
static u32 s_op_gt;              /* Anzeige-Tick des letzten Vorlaufs (= Live-Tick, der als naechstes gezeigt wird) */
static float s_op_p[4];          /* Lage fuer diese Anzeige (letzter Vorlauf-Tick) */
static int s_op_ok, s_op_lead;   /* gueltig; Vorsprung in Ticks */
static u32 s_op_pgt;             /* letzter Tick des vorigen Vorlaufs (Korrektur-Vergleich) */
static float s_op_pp[4];
static int s_op_prev;
static u32 s_op_g1;              /* erster Vorlauf-Tick (= Anzeige-Tick) und seine Lage: Vergleich mit dem Live-Tick */
static float s_op_p1[4];
static int s_op_p1ok;
static ls_track_t s_op_res;      /* abklingende Korrekturen */
static double s_op_loff[4];      /* zuletzt angezeigter Versatz (Halten, wenn ein Vorlauf ausfaellt) */
static int s_op_lon, s_op_hold;
static ls_ring_t s_op_c, s_op_cy, s_op_off, s_op_jr, s_op_jd, s_op_cons; /* Korrektur, Gier-Korrektur, Versatz, Ruck roh/angezeigt, Vorlauf-Tick 1 gegen live */
static double s_op_cmax, s_op_cymax, s_op_offmax, s_op_jrmax, s_op_jdmax, s_op_consmax, s_op_leadsum;
static u32 s_op_leadchg; /* Wechsel des Vorsprungs (D) geglaettet */
static u32 s_op_runs, s_op_used, s_op_held, s_op_none, s_op_far, s_op_cvis, s_op_cn, s_op_nocmp, s_op_ticks_on;
static double s_op_jlast[3], s_op_jstep[3], s_op_jrlast[3], s_op_jrstep[3];
static int s_op_jok, s_op_jsok;
static FILE *s_op_file;
static double s_yo_max, s_yw_max;
static double s_wjump; /* W386: groesster Sprung im laufenden Fenster */
double loc_smooth_window_jump(void)
{
    double j = s_wjump;
    s_wjump = 0.0;
    return j;
}
static u32 s_meas, s_tickmis, s_smoothed, s_smoothed_cam, s_toobig, s_noread, s_contact;
static double s_contact_max;
static u32 s_cat_n[RBI_MIS_N];
static double s_cat_max[RBI_MIS_N];
static u32 s_depth_n[4]; /* Tiefe 1, 2, 3-4, >= 5 Ticks */
/* Fenster nach einer Korrektur */
static int s_win, s_cwin;
static double s_win_r, s_win_s, s_cwin_r, s_cwin_s;
static double s_last_raw[LS_MAXV][3], s_last_disp[LS_MAXV][3];
static double s_lstep_raw[LS_MAXV][3], s_lstep_disp[LS_MAXV][3]; /* letzter Schritt je Tick (Ruck = zweite Differenz) */
static int s_lstep_ok[LS_MAXV];
static int s_last_ok[LS_MAXV];
static u32 s_live_ticks, s_t0_ticks;
/* Render-Zaehler (Render-Thread) */
static volatile LONG s_r_lists, s_r_obj, s_r_cam, s_r_own, s_r_vts;
/* DIAG Bit 8: sichtbare Lageaenderung je Liste (Render-Thread schreibt, Zeile liest unter s_rcs) */
static SRWLOCK s_rcs = SRWLOCK_INIT;

static void ls_put(ls_ring_t *r, double v)
{
    r->v[r->n % LS_NS] = (float)v;
    r->n++;
}
static int ls_fcmp(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}
static void ls_pct(const ls_ring_t *r, double *p50, double *p95, double *mx)
{
    static float tmp[LS_NS];
    u32 n = r->n < LS_NS ? r->n : LS_NS;
    *p50 = *p95 = *mx = 0.0;
    if (!n) return;
    memcpy(tmp, r->v, n * sizeof tmp[0]);
    qsort(tmp, n, sizeof tmp[0], ls_fcmp);
    *p50 = tmp[(n - 1u) / 2u];
    *p95 = tmp[(u32)((double)(n - 1u) * 0.95)];
    *mx = tmp[n - 1u];
}

static int ls_steps(void)
{
    int ms = g_cfg.loc_rb_smooth_ms;
    if (ms < 50) ms = 50;
    if (ms > 500) ms = 500;
    return (ms + 25) / 50;
}

static int ls_read(ls_vt_t v[LS_MAXV], int *own)
{
    int j;
    __try {
        u32 pl = *(volatile const u32 *)(uintptr_t)LS_PLAYER;
        *own = pl < LS_MAXV ? (int)pl : -1;
        for (j = 0; j < LS_MAXV; j++) {
            u32 vt = LS_VT0 + (u32)j * LS_VSTRIDE, obj = *(volatile const u32 *)(uintptr_t)(vt + 0xCu);
            v[j].ok = 0;
            if (obj < 0x10000u || obj >= 0x80000000u) continue;
            v[j].obj = obj;
            v[j].p[0] = *(volatile const float *)(uintptr_t)(obj + 0xCu);
            v[j].p[1] = *(volatile const float *)(uintptr_t)(obj + 0x10u);
            v[j].p[2] = *(volatile const float *)(uintptr_t)(obj + 0x14u);
            v[j].yaw = *(volatile const float *)(uintptr_t)(obj + 0x1Cu);
            if (!(fabs(v[j].p[0]) < 1e7 && fabs(v[j].p[1]) < 1e7 && fabs(v[j].p[2]) < 1e7 && fabs(v[j].yaw) < 100.0)) continue;
            v[j].ok = 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return 1;
}

static double ls_len3(const double d[3]) { return sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]); }

/* W387: gelesene Lagen als Historie des Spielticks gtick ablegen. */
static void ls_hist_write(u32 gtick, const ls_vt_t v[LS_MAXV])
{
    int j;
    for (j = 0; j < LS_MAXV; j++) {
        double p[4];
        if (!v[j].ok) continue;
        p[0] = v[j].p[0];
        p[1] = v[j].p[1];
        p[2] = v[j].p[2];
        p[3] = v[j].yaw;
        ls_hist_put(&s_hist[j], gtick, v[j].obj, p);
    }
}

static int ls_opp_mode(void)
{
    int m = g_cfg.loc_smooth_opp;
    return (m >= 0 && m <= 2) ? m : 1;
}

static void ls_publish(const loc_smooth_pub_t *v);
void loc_smooth_reset(void)
{
    loc_smooth_pub_t pb;
    int j;
    for (j = 0; j < LS_MAXV; j++) {
        ls_track_reset(&s_tr[j]);
        s_last_ok[j] = 0;
        s_lstep_ok[j] = 0;
        ls_hist_reset(&s_hist[j]); /* W387 */
    }
    ls_auto_reset(&s_auto); /* W387: jede Mission beginnt ohne Verzoegerung */
    s_op_ok = s_op_prev = s_op_p1ok = s_op_lon = s_op_hold = s_op_jok = s_op_jsok = 0; /* W390 */
    ls_track_reset(&s_op_res);
    s_opp_d = 0.0;
    s_hpre_ok = 0;
    s_pre_ok = 0;
    s_win = s_cwin = 0;
    memset(&pb, 0, sizeof pb); /* kein alter Versatz in die Listen der naechsten Mission */
    pb.own = -1;
    ls_publish(&pb);
}

void loc_smooth_rb_pre(u32 gtick)
{
    if (!g_sba_loc) return;
    s_pre_ok = ls_read(s_pre, &s_pre_own);
    s_pre_tick = gtick;
    s_hpre_ok = 0;
    if (!s_pre_ok) {
        s_noread++;
        return;
    }
    if (gtick == 0xFFFFFFFFu) return; /* Messung verworfen (kein Rollback) */
    if (gtick == s_opp_pubtick) s_opp_tk_eq++;
    else s_opp_tk_ne++;
    ls_hist_write(gtick, s_pre);      /* W387: Stand vor dem Laden = vorhergesagter Tick (wie live) */
    memcpy(s_hpre, s_hist, sizeof s_hpre);
    s_hpre_ok = 1;
}

void loc_smooth_replay_tick(u32 gtick)
{
    ls_vt_t v[LS_MAXV];
    int own = -1;
    if (!g_sba_loc) return;
    if (ls_read(v, &own)) ls_hist_write(gtick, v); /* W387: korrigierter Stand dieses Neurechen-Ticks */
}

void loc_smooth_rb_post(u32 gtick, u32 cause, u32 ticks)
{
    ls_vt_t post[LS_MAXV];
    int own = -1, j, k, n = ls_steps(), on = g_cfg.loc_rb_smooth != 0;
    double jo = 0.0, yo = 0.0, jw = 0.0, yw = 0.0, dist = -1.0, mx = (double)g_cfg.loc_rb_smooth_max, jd = 0.0;
    int need = 0;
    if (!g_sba_loc || !s_pre_ok) return;
    s_pre_ok = 0;
    if (!ls_read(post, &own)) {
        s_noread++;
        return;
    }
    if (gtick != s_pre_tick || own != s_pre_own) { /* nicht derselbe Tick: nicht vergleichbar */
        s_tickmis++;
        return;
    }
    s_meas++;
    s_depth_n[ticks <= 1u ? 0 : (ticks == 2u ? 1 : (ticks <= 4u ? 2 : 3))]++;
    ls_hist_write(gtick, post); /* W387: korrigierter Stand (gleich dem letzten Neurechen-Tick) */
    for (j = 0; j < LS_MAXV; j++) {
        double e[4], d;
        if (!s_pre[j].ok || !post[j].ok || s_pre[j].obj != post[j].obj) continue;
        for (k = 0; k < 3; k++) e[k] = s_pre[j].p[k] - post[j].p[k];
        e[3] = ls_wrap(s_pre[j].yaw - post[j].yaw);
        d = ls_len3(e);
        if (j == own) {
            jw = d;
            yw = fabs(e[3]);
        } else {
            if (d > jo) jo = d;
            if (fabs(e[3]) > yo) yo = fabs(e[3]);
            if (own >= 0 && post[own].ok) { /* Abstand zum naechsten anderen VT (waagerecht) */
                double dx = post[j].p[0] - post[own].p[0], dz = post[j].p[2] - post[own].p[2], h = sqrt(dx * dx + dz * dz);
                if (dist < 0.0 || h < dist) dist = h;
            }
        }
        if (j != own && s_hpre_ok && own >= 0 && post[own].ok) {
            /* W387: anderer VT wird bei T - d gezeigt. Bedarf (kleinste Verzoegerung ohne sichtbaren Sprung) fuer die
             * Auto-Regelung; geglaettet wird nur der Sprung der angezeigten Lage - was durch die Verzoegerung schon
             * bestaetigt war, springt nicht und braucht keine Glaettung (d = 0: wie W385). */
            double vis = (double)g_cfg.loc_smooth_opp_vis, dx = post[j].p[0] - post[own].p[0],
                   dz = post[j].p[2] - post[own].p[2], h = sqrt(dx * dx + dz * dz), a[4], b[4];
            int nd;
            if (vis < 1.0) vis = 1.0;
            if (h > 10000.0) vis *= h / 10000.0; /* weiter weg wirkt derselbe Sprung kleiner (Winkel) */
            nd = ls_need(&s_hpre[j], &s_hist[j], gtick, post[j].obj, vis, 2);
            if (nd > need) need = nd;
            if (s_opp_d > 0.0 && ls_hist_at(&s_hpre[j], gtick, post[j].obj, s_opp_d, a) &&
                ls_hist_at(&s_hist[j], gtick, post[j].obj, s_opp_d, b)) {
                for (k = 0; k < 3; k++) e[k] = a[k] - b[k];
                e[3] = ls_wrap(a[3] - b[3]);
                d = ls_len3(e);
            }
            if (d > jd) jd = d;
        }
        if (d < LS_EPS_POS && fabs(e[3]) < LS_EPS_YAW) continue;
        if (d > mx) { /* Neustart/Teleport: sofort */
            s_toobig++;
            ls_track_reset(&s_tr[j]);
            continue;
        }
        if (on) {
            ls_track_add(&s_tr[j], e, n);
            if (j == own) s_smoothed_cam++;
            else s_smoothed++;
        }
    }
    if (s_hpre_ok) { /* W387 */
        ls_put(&s_jd, jd);
        s_opp_ev[need > 3 ? 3 : need]++;
        if (need >= 1 && (double)need <= s_opp_d + 1e-9) s_opp_hidden++;
        ls_auto_event(&s_auto, need);
        s_hpre_ok = 0;
    }
    ls_put(&s_jo, jo);
    ls_put(&s_jw, jw);
    if (jo > s_wjump) s_wjump = jo; /* W386 */
    if (jw > s_wjump) s_wjump = jw;
    if (yo > s_yo_max) s_yo_max = yo;
    if (yw > s_yw_max) s_yw_max = yw;
    if (dist >= 0.0) ls_put(&s_dist, dist);
    if (dist >= 0.0 && dist < 2.0 * (double)g_cfg.loc_rb_smooth_r) {
        s_contact++;
        if (jo > s_contact_max) s_contact_max = jo;
    }
    for (k = 0; k < RBI_MIS_N; k++) {
        if (!(cause & (1u << k))) continue;
        s_cat_n[k]++;
        if (jo > s_cat_max[k]) s_cat_max[k] = jo;
    }
    if (jo >= LS_EPS_POS || yo >= LS_EPS_YAW) {
        if (!s_win) s_win_r = s_win_s = 0.0;
        s_win = LS_WIN;
    }
    if (jw >= LS_EPS_POS || yw >= LS_EPS_YAW) {
        if (!s_cwin) s_cwin_r = s_cwin_s = 0.0;
        s_cwin = LS_WIN;
    }
    if ((g_cfg.loc_rb_smooth_diag & 1) && (g_cfg.loc_replay_tick[0] || g_cfg.loc_steps[0]))
        HLOG_I("LOCSMOOTH rb: Tick %u Tiefe %u Ursache 0x%X Gegner %.2f (Gier %.3f Grad) eigen %.2f (Gier %.3f Grad) "
               "Abstand %.0f", gtick, ticks, cause, jo, yo * 180.0 / LS_PI, jw, yw * 180.0 / LS_PI, dist);
}

static void ls_publish(const loc_smooth_pub_t *v)
{
    loc_smooth_pub_t w = *v;
    w.seq = (uint32_t)(s_pub_seq + 1);
    pace_pub_write(&s_pub_seq, s_pub, sizeof s_pub[0], &w);
}

/* ---- W390 LOC-OWN-PREDICT ---- */
int loc_smooth_own_read(float out[4])
{
    __try {
        u32 pl = *(volatile const u32 *)(uintptr_t)LS_PLAYER, obj;
        if (pl >= LS_MAXV) return 0;
        obj = *(volatile const u32 *)(uintptr_t)(LS_VT0 + pl * LS_VSTRIDE + 0xCu);
        if (obj < 0x10000u || obj >= 0x80000000u) return 0;
        out[0] = *(volatile const float *)(uintptr_t)(obj + 0xCu);
        out[1] = *(volatile const float *)(uintptr_t)(obj + 0x10u);
        out[2] = *(volatile const float *)(uintptr_t)(obj + 0x14u);
        out[3] = *(volatile const float *)(uintptr_t)(obj + 0x1Cu);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return fabs(out[0]) < 1e7 && fabs(out[1]) < 1e7 && fabs(out[2]) < 1e7 && fabs(out[3]) < 100.0;
}

void loc_smooth_own_pred(uint32_t gt_disp, int n, const uint32_t *gt, const float (*p)[4], const int *ok)
{
    int last = n - 1, i;
    if (!g_sba_loc) return;
    if (n < 1 || n > 16 || !ok[last]) {
        s_op_none++;
        s_op_ok = 0;
        return;
    }
    s_op_runs++;
    if (s_op_prev && s_op_lon) { /* Korrektur gegen den vorigen Vorlauf am gemeinsamen Tick (Lage, die gezeigt wurde) */
        double c[4], len;
        if (ls_own_corr(s_op_pgt, s_op_pp, n, gt, p, ok, c)) {
            len = sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
            s_op_cn++;
            ls_put(&s_op_c, len);
            ls_put(&s_op_cy, fabs(c[3]) * 180.0 / LS_PI);
            if (len > s_op_cmax) s_op_cmax = len;
            if (fabs(c[3]) > s_op_cymax) s_op_cymax = fabs(c[3]);
            if (len + fabs(c[3]) * LS_YAW_ARM >= (double)g_cfg.loc_smooth_opp_vis) s_op_cvis++;
            if (len > (double)g_cfg.loc_rb_smooth_max) {
                ls_track_reset(&s_op_res);
                s_op_far++;
            } else if (len > LS_EPS_POS || fabs(c[3]) > LS_EPS_YAW) {
                ls_track_add(&s_op_res, c, ls_steps());
            }
            if ((g_cfg.loc_own_predict_diag & 4) && len >= 20.0 && (g_cfg.loc_replay_tick[0] || g_cfg.loc_steps[0]))
                HLOG_I("LOCOWNPRED corr: Tick %u Korrektur %.1f Einheiten Gier %.3f Grad (Vorlauf %d Ticks)", gt_disp, len,
                       c[3] * 180.0 / LS_PI, n);
        } else {
            s_op_nocmp++;
        }
    }
    if (s_op_prev && s_op_lon && last != s_op_lead && last >= 1 && ok[last - 1]) { /* Vorsprung wechselt (D): weich */
        double c[4];
        ls_own_leadchg(p[last], p[last - 1], last - s_op_lead, c);
        ls_track_add(&s_op_res, c, 2 * ls_steps());
        s_op_leadchg++;
    }
    s_op_gt = gt_disp;
    for (i = 0; i < 4; i++) s_op_p[i] = p[last][i];
    s_op_ok = 1;
    s_op_lead = last;
    s_op_pgt = gt[last];
    for (i = 0; i < 4; i++) s_op_pp[i] = p[last][i];
    s_op_prev = 1;
    s_op_g1 = gt[0];
    for (i = 0; i < 4; i++) s_op_p1[i] = p[0][i];
    s_op_p1ok = ok[0] && gt[0] == gt_disp;
}

/* Versatz der eigenen Lage fuer die Liste dieses Ticks (ersetzt den W385-Versatz des eigenen VT, solange ein Vorlauf
 * gilt). Rueckgabe 1 = Versatz != 0. */
static int ls_op_apply(u32 gtick, const ls_vt_t *v, double off[4], int nz)
{
    double res[4], o[4], sim[4], e0[4], zero[4] = {0.0, 0.0, 0.0, 0.0};
    int use = s_op_ok && s_op_gt == gtick, k, steps = ls_steps();
    for (k = 0; k < 3; k++) sim[k] = v->p[k];
    sim[3] = v->yaw;
    s_op_ticks_on++;
    if (s_op_p1ok && s_op_g1 == gtick) { /* Vorlauf-Tick 1 gegen den Live-Tick (gleich, solange die Vorhersage stimmte) */
        double d = sqrt((s_op_p1[0] - sim[0]) * (s_op_p1[0] - sim[0]) + (s_op_p1[1] - sim[1]) * (s_op_p1[1] - sim[1]) +
                        (s_op_p1[2] - sim[2]) * (s_op_p1[2] - sim[2]));
        ls_put(&s_op_cons, d);
        if (d > s_op_consmax) s_op_consmax = d;
    }
    s_op_p1ok = 0;
    if (use) {
        s_op_ok = 0;
        if (!ls_own_offset(s_op_p, sim, zero, (double)g_cfg.loc_rb_smooth_max, e0)) {
            s_op_far++;
            ls_track_reset(&s_op_res);
            s_op_lon = 0;
            return nz;
        }
        if (!s_op_lon) { /* (wieder) einsetzen: vom angezeigten Stand weich in den Vorlauf (2x Glaettungszeit) */
            for (k = 0; k < 4; k++) e0[k] = -e0[k];
            ls_track_add(&s_op_res, e0, 2 * steps);
        }
        (void)ls_track_step(&s_op_res, res);
        if (!ls_own_offset(s_op_p, sim, res, (double)g_cfg.loc_rb_smooth_max, o)) {
            s_op_far++;
            ls_track_reset(&s_op_res);
            s_op_lon = 0;
            return nz;
        }
        s_op_lon = 1;
        s_op_hold = 0;
        s_op_used++;
        s_op_leadsum += (double)s_op_lead;
        memcpy(s_op_loff, o, sizeof o);
    } else if (s_op_lon && s_op_hold < 2) { /* Vorlauf fiel aus (Neuaufbau, Render beschaeftigt): Versatz halten */
        s_op_hold++;
        s_op_held++;
        memcpy(o, s_op_loff, sizeof o);
    } else {
        if (s_op_lon) { /* endgueltig ohne Vorlauf: weich zurueck auf die Simulation */
            ls_track_reset(&s_op_res);
            ls_track_add(&s_op_res, s_op_loff, 2 * steps);
            s_op_lon = 0;
        }
        if (!ls_track_step(&s_op_res, o)) return nz;
    }
    {
        double len = sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
        ls_put(&s_op_off, len);
        if (len > s_op_offmax) s_op_offmax = len;
    }
    memcpy(off, o, sizeof o);
    return fabs(o[0]) + fabs(o[1]) + fabs(o[2]) + fabs(o[3]) > 0.0;
}

/* Ruck (zweite Differenz) der eigenen Lage je Tick: simuliert und angezeigt; DIAG-Datei je Tick (nur Testfolge). */
static void ls_op_note(u32 gtick, const ls_vt_t *v, const double off[4], int on)
{
    double d[3], r[3], a = 0.0, b = 0.0;
    int k, have = 0;
    for (k = 0; k < 3; k++) {
        d[k] = v->p[k] + (on ? off[k] : 0.0);
        r[k] = v->p[k];
    }
    if (s_op_jok) {
        double sd[3], sr[3];
        for (k = 0; k < 3; k++) {
            sd[k] = d[k] - s_op_jlast[k];
            sr[k] = r[k] - s_op_jrlast[k];
        }
        if (s_op_jsok) {
            for (k = 0; k < 3; k++) {
                a += (sr[k] - s_op_jrstep[k]) * (sr[k] - s_op_jrstep[k]);
                b += (sd[k] - s_op_jstep[k]) * (sd[k] - s_op_jstep[k]);
            }
            a = sqrt(a);
            b = sqrt(b);
            have = !(a > (double)g_cfg.loc_rb_smooth_max || b > (double)g_cfg.loc_rb_smooth_max); /* nicht bei Neustart/Teleport */
        }
        if (have) {
            ls_put(&s_op_jr, a);
            ls_put(&s_op_jd, b);
            if (a > s_op_jrmax) s_op_jrmax = a;
            if (b > s_op_jdmax) s_op_jdmax = b;
        }
        memcpy(s_op_jstep, sd, sizeof sd);
        memcpy(s_op_jrstep, sr, sizeof sr);
        s_op_jsok = 1;
    }
    memcpy(s_op_jlast, d, sizeof d);
    memcpy(s_op_jrlast, r, sizeof r);
    s_op_jok = 1;
    if ((g_cfg.loc_own_predict_diag & 2) && (g_cfg.loc_replay_tick[0] || g_cfg.loc_steps[0])) { /* nur Lagen, keine Eingaben */
        if (!s_op_file) {
            s_op_file = (FILE *)loc_aim_diag_file("locownpred.csv");
            if (s_op_file) fprintf(s_op_file, "gtick;rahmen;x;y;z;gier;dx;dy;dz;dgier;an;vorlauf\n");
        }
        if (s_op_file)
            fprintf(s_op_file, "%u;%u;%.2f;%.2f;%.2f;%.5f;%.2f;%.2f;%.2f;%.5f;%d;%d\n", gtick, loc_sync_frames(), v->p[0], v->p[1],
                    v->p[2], v->yaw,
                    on ? off[0] : 0.0, on ? off[1] : 0.0, on ? off[2] : 0.0, on ? off[3] : 0.0, on, s_op_lead);
    }
}

void loc_smooth_tick(u32 gtick)
{
    ls_vt_t cur[LS_MAXV];
    loc_smooth_pub_t pb;
    int own = -1, j, k;
    double wr = 0.0, ws = 0.0, cr = 0.0, cs = 0.0;
    if (!g_sba_loc) return;
    memset(&pb, 0, sizeof pb);
    s_live_ticks++;
    if (!ls_read(cur, &own)) return;
    pb.gtick = gtick;
    pb.own = own;
    pb.radius = (float)g_cfg.loc_rb_smooth_r;
    { /* W387: Historie dieses Ticks, Auto-Regelung (je Live-Tick ein Schritt) */
        int mode = ls_opp_mode(), tg = s_auto.target;
        ls_hist_write(gtick, cur);
        s_opp_pubtick = gtick;
        s_opp_d = own >= 0 ? ls_auto_step(&s_auto, mode, g_cfg.loc_smooth_opp_max) : 0.0;
        if (s_auto.target > tg) s_opp_tgt_up++;
        else if (s_auto.target < tg) s_opp_tgt_dn++;
        pb.opp_mode = mode;
        pb.opp_ms = (float)(s_opp_d * 50.0);
        s_opp_dsum += s_opp_d;
        if (s_opp_d > s_opp_dmax) s_opp_dmax = s_opp_d;
        s_opp_dn++;
        s_opp_dt[s_opp_d <= 0.0 ? 0 : (s_opp_d < 1.0 ? 1 : 2)]++;
    }
    for (j = 0; j < LS_MAXV; j++) {
        double off[4] = {0.0, 0.0, 0.0, 0.0}, disp[3], dr[3], dd[3];
        int nz;
        if (!cur[j].ok) {
            ls_track_reset(&s_tr[j]);
            s_last_ok[j] = 0;
            s_lstep_ok[j] = 0;
            continue;
        }
        nz = ls_track_step(&s_tr[j], off);
        if (j != own && s_opp_d > 0.0) { /* W387: anderer VT bei T - d (plus W385-Rest der angezeigten Lage) */
            double od[4];
            int r = ls_delay_off(&s_hist[j], gtick, cur[j].obj, s_opp_d, od);
            if (r == 1) s_opp_short++;
            else if (r == 0) s_opp_none++;
            if (r && ls_len3(od) > (double)g_cfg.loc_rb_smooth_max) { /* Neustart/Teleport: nicht verzoegern */
                s_opp_far++;
                r = 0;
            }
            if (r) {
                for (k = 0; k < 4; k++) off[k] += od[k];
                off[3] = ls_wrap(off[3]);
                if (fabs(off[0]) + fabs(off[1]) + fabs(off[2]) + fabs(off[3]) > 0.0) nz = 1;
            }
        }
        if (j == own && g_cfg.loc_own_predict) { /* W390: eigene Lage aus dem Vorlauf (D + 1 Ticks voraus) */
            nz = ls_op_apply(gtick, &cur[j], off, nz);
            ls_op_note(gtick, &cur[j], off, nz);
        }
        pb.v[j].ok = 1;
        for (k = 0; k < 3; k++) pb.v[j].p[k] = (float)cur[j].p[k];
        pb.v[j].yaw = (float)cur[j].yaw;
        pb.v[j].on = nz;
        for (k = 0; k < 4; k++) pb.v[j].off[k] = (float)off[k];
        if (nz) pb.any = 1;
        for (k = 0; k < 3; k++) disp[k] = cur[j].p[k] + off[k];
        if (s_last_ok[j]) { /* Ruck je Tick (zweite Differenz der Lage): simuliert (roh) und angezeigt (mit Versatz).
                             * Ein Sprung E erscheint roh als Spitze ~|E|, mit Abklingen ueber N Ticks als ~|E|/N. */
            double a = 0.0, b = 0.0, jr[3], jd[3];
            for (k = 0; k < 3; k++) {
                dr[k] = cur[j].p[k] - s_last_raw[j][k];
                dd[k] = disp[k] - s_last_disp[j][k];
                jr[k] = dr[k] - s_lstep_raw[j][k];
                jd[k] = dd[k] - s_lstep_disp[j][k];
            }
            if (s_lstep_ok[j]) {
                a = ls_len3(jr);
                b = ls_len3(jd);
            }
            memcpy(s_lstep_raw[j], dr, sizeof dr);
            memcpy(s_lstep_disp[j], dd, sizeof dd);
            s_lstep_ok[j] = 1;
            if (j == own) {
                cr = a;
                cs = b;
            } else {
                if (a > wr) wr = a;
                if (b > ws) ws = b;
            }
        }
        memcpy(s_last_raw[j], cur[j].p, sizeof s_last_raw[j]);
        memcpy(s_last_disp[j], disp, sizeof s_last_disp[j]);
        s_last_ok[j] = 1;
    }
    if ((g_cfg.loc_rb_smooth_diag & 4) && (g_cfg.loc_replay_tick[0] || g_cfg.loc_steps[0]) && (s_live_ticks % 40u) == 0u &&
        own >= 0 && cur[own].ok) { /* DIAG(W385): Lage der VTs alle 2 s (Spielzustand, keine Eingaben) */
        for (j = 0; j < LS_MAXV; j++) {
            double dx, dz;
            if (j == own || !cur[j].ok) continue;
            dx = cur[j].p[0] - cur[own].p[0];
            dz = cur[j].p[2] - cur[own].p[2];
            HLOG_I("LOCSMOOTH pos: Tick %u eigen VT%d %.0f,%.0f,%.0f Gier %.2f | VT%d %.0f,%.0f,%.0f Gier %.2f | Abstand %.0f", gtick,
                   own, cur[own].p[0], cur[own].p[1], cur[own].p[2], cur[own].yaw, j, cur[j].p[0], cur[j].p[1], cur[j].p[2],
                   cur[j].yaw, sqrt(dx * dx + dz * dz));
        }
    }
    if (s_win > 0) {
        if (wr > s_win_r) s_win_r = wr;
        if (ws > s_win_s) s_win_s = ws;
        if (--s_win == 0) {
            ls_put(&s_wr, s_win_r);
            ls_put(&s_ws, s_win_s);
        }
    } else if (wr > 0.0) {
        ls_put(&s_base, wr);
    }
    if (s_cwin > 0) {
        if (cr > s_cwin_r) s_cwin_r = cr;
        if (cs > s_cwin_s) s_cwin_s = cs;
        if (--s_cwin == 0) {
            ls_put(&s_cr, s_cwin_r);
            ls_put(&s_cs, s_cwin_s);
        }
    }
    ls_publish(&pb);
}

int loc_smooth_get(loc_smooth_pub_t *out)
{
    LONG q, q2;
    int k;
    if ((!g_cfg.loc_rb_smooth && !g_cfg.loc_smooth_opp && !g_cfg.loc_own_predict) || !g_sba_loc) return 0; /* W387: auch nur Verzoegerung; W390 */
    (void)q; (void)q2; (void)k;
    if (!pace_pub_read(&s_pub_seq, s_pub, sizeof s_pub[0], out)) return 0; /* W393 */
    return out->any;
}

int loc_smooth_peek(loc_smooth_pub_t *out)
{
    LONG q, q2;
    int k;
    (void)q; (void)q2; (void)k;
    memset(out, 0, sizeof *out);
    if (!pace_pub_read(&s_pub_seq, s_pub, sizeof s_pub[0], out)) { memset(out, 0, sizeof *out); return 0; } /* W393 */
    return 1;
}

int loc_smooth_opp_hud(int *mode, int *ms)
{
    loc_smooth_pub_t pb;
    if (!g_sba_loc || !loc_smooth_peek(&pb) || !pb.seq) return 0;
    *mode = pb.opp_mode;
    *ms = (int)(pb.opp_ms + 0.5f);
    return 1;
}

void loc_smooth_render_note(unsigned obj_draws, unsigned cam_draws, unsigned own_draws, unsigned vts)
{
    InterlockedIncrement(&s_r_lists);
    InterlockedExchangeAdd(&s_r_obj, (LONG)obj_draws);
    InterlockedExchangeAdd(&s_r_cam, (LONG)cam_draws);
    InterlockedExchangeAdd(&s_r_own, (LONG)own_draws);
    InterlockedExchangeAdd(&s_r_vts, (LONG)vts);
}

static ls_ring_t s_mk_all, s_mk_own; /* W390: Abweichung Zielmarkierung je Liste (Pixel), unter s_rcs */
static double s_mk_all_max, s_mk_own_max;
void loc_smooth_render_mark(double px_all, double px_own)
{
    AcquireSRWLockExclusive(&s_rcs);
    ls_put(&s_mk_all, px_all);
    ls_put(&s_mk_own, px_own);
    if (px_all > s_mk_all_max) s_mk_all_max = px_all;
    if (px_own > s_mk_own_max) s_mk_own_max = px_own;
    ReleaseSRWLockExclusive(&s_rcs);
}

static ls_ring_t s_mk_v30, s_mk_v100;
static double s_mk_v30_max, s_mk_v100_max;
void loc_smooth_render_mark_virt(double px30, double px100)
{
    AcquireSRWLockExclusive(&s_rcs);
    ls_put(&s_mk_v30, px30);
    ls_put(&s_mk_v100, px100);
    if (px30 > s_mk_v30_max) s_mk_v30_max = px30;
    if (px100 > s_mk_v100_max) s_mk_v100_max = px100;
    ReleaseSRWLockExclusive(&s_rcs);
}

void loc_smooth_render_cost(double us)
{
    AcquireSRWLockExclusive(&s_rcs);
    ls_put(&s_rcost, us);
    ReleaseSRWLockExclusive(&s_rcs);
}

void loc_smooth_render_step(int cam, double raw, double disp, int win)
{
    AcquireSRWLockExclusive(&s_rcs);
    if (cam) {
        if (win) {
            ls_put(&s_rc_wr, raw);
            ls_put(&s_rc_ws, disp);
        }
    } else if (win) {
        ls_put(&s_rv_wr, raw);
        ls_put(&s_rv_ws, disp);
    } else {
        ls_put(&s_rv_b, raw);
    }
    ReleaseSRWLockExclusive(&s_rcs);
}

void loc_smooth_log(int t_s)
{
    double o50, o95, omx, w50, w95, wmx, d50, d95, dmx, r50, r95, rmx, s50, s95, smx, c50, c95, cmx, q50, q95, qmx, b50,
        b95, bmx;
    double mins;
    if (g_sba_loc && g_cfg.loc_own_predict && s_op_ticks_on) { /* W390: nur Lagen-Statistik und Zaehler, keine Eingaben */
        double c50, c95, cmx, y50, y95, ymx, f50, f95, fmx, r50, r95, rmx, a50, a95, amx, k50, k95, kmx;
        double om = (double)s_op_ticks_on / 20.0 / 60.0;
        ls_pct(&s_op_c, &c50, &c95, &cmx);
        ls_pct(&s_op_cy, &y50, &y95, &ymx);
        ls_pct(&s_op_off, &f50, &f95, &fmx);
        ls_pct(&s_op_jr, &r50, &r95, &rmx);
        ls_pct(&s_op_jd, &a50, &a95, &amx);
        ls_pct(&s_op_cons, &k50, &k95, &kmx);
        {
            double m50, m95, mmx, n50, n95, nmx, a50, a95, amx, b50, b95, bmx;
            AcquireSRWLockShared(&s_rcs);
            ls_pct(&s_mk_own, &m50, &m95, &mmx);
            ls_pct(&s_mk_all, &n50, &n95, &nmx);
            ls_pct(&s_mk_v30, &a50, &a95, &amx);
            ls_pct(&s_mk_v100, &b50, &b95, &bmx);
            ReleaseSRWLockShared(&s_rcs);
            HLOG_I("LOCOWNPRED-MARK: t=%ds Zielmarkierung gegen gezeichneten VT (Pixel bei 640x480) nur Vorhersage p50/p95/max=%.1f/%.1f/%.1f "
                   "(max gesamt %.1f) | mit Gegner-Glaettung p50/p95/max=%.1f/%.1f/%.1f (max gesamt %.1f) | gedachtes Ziel 30 m "
                   "p50/p95/max=%.1f/%.1f/%.1f (max %.1f) 100 m=%.1f/%.1f/%.1f (max %.1f)",
                   t_s, m50, m95, mmx, s_mk_own_max, n50, n95, nmx, s_mk_all_max, a50, a95, amx, s_mk_v30_max, b50, b95, bmx,
                   s_mk_v100_max);
        }
        HLOG_I("LOCOWNPRED: t=%ds Vorlaeufe=%u angezeigt=%u (%.0f %% der Ticks) gehalten=%u ohne=%u zu gross=%u Vorsprung Mittel=%.2f T | "
               "Korrektur n=%u p50/p95/max=%.1f/%.1f/%.1f (max gesamt %.1f) Gier p95/max=%.2f/%.2f Grad sichtbar=%u (%.2f je min) "
               "ohne Vergleich=%u Vorsprung-Wechsel=%u | Versatz p50/p95/max=%.0f/%.0f/%.0f | Ruck eigen roh p50/p95/max=%.1f/%.1f/%.1f (max %.1f) "
               "angezeigt=%.1f/%.1f/%.1f (max %.1f) | Vorlauf-Tick1 gegen live p95/max=%.1f/%.1f",
               t_s, s_op_runs, s_op_used, s_op_ticks_on ? 100.0 * s_op_used / s_op_ticks_on : 0.0, s_op_held, s_op_none,
               s_op_far, s_op_used ? s_op_leadsum / s_op_used : 0.0, s_op_cn, c50, c95, cmx, s_op_cmax, y95,
               s_op_cymax * 180.0 / LS_PI, s_op_cvis, om > 0.0 ? s_op_cvis / om : 0.0, s_op_nocmp, s_op_leadchg, f50, f95, fmx, r50, r95,
               rmx, s_op_jrmax, a50, a95, amx, s_op_jdmax, k95, s_op_consmax);
        if (s_op_file) fflush(s_op_file);
    }
    if (!g_sba_loc || (!s_meas && !s_tickmis && !s_r_lists && !s_opp_dn)) return; /* W387: OPP-Zeile auch ohne Korrekturen */
    mins = (double)(s_live_ticks - s_t0_ticks) / 20.0 / 60.0;
    ls_pct(&s_jo, &o50, &o95, &omx);
    ls_pct(&s_jw, &w50, &w95, &wmx);
    ls_pct(&s_dist, &d50, &d95, &dmx);
    ls_pct(&s_wr, &r50, &r95, &rmx);
    ls_pct(&s_ws, &s50, &s95, &smx);
    ls_pct(&s_cr, &c50, &c95, &cmx);
    ls_pct(&s_cs, &q50, &q95, &qmx);
    ls_pct(&s_base, &b50, &b95, &bmx);
    /* Nur Lagen-Statistik (Spieleinheiten, Grad) und Zaehler, keine Eingaben. */
    if (s_meas || s_tickmis || s_r_lists) {
    HLOG_I("LOCSMOOTH: t=%ds Schalter=%d %d ms | Korrekturen=%u (%.2f je min) Tick anders=%u ungelesen=%u | "
           "Gegner-Sprung p50/p95/max=%.1f/%.1f/%.1f Gier max=%.2f Grad | eigen p50/p95/max=%.1f/%.1f/%.1f Gier max=%.2f Grad | "
           "Abstand p50=%.0f Kontakt=%u (max Sprung %.1f) | Tiefe 1/2/3-4/5+=%u/%u/%u/%u | "
           "Ursache Achsen=%u (%.1f) Ausweichen=%u (%.1f) Pedale=%u (%.1f) Gang=%u (%.1f) Tasten-B0=%u (%.1f) Tasten=%u (%.1f) "
           "Kopf=%u (%.1f) | Ruck je Tick nach Korrektur Gegner roh p95/max=%.1f/%.1f angezeigt=%.1f/%.1f (sonst p50/p95 "
           "%.1f/%.1f) eigen roh=%.1f/%.1f angezeigt=%.1f/%.1f | geglaettet Gegner=%u Kamera=%u zu gross=%u | Render Listen=%ld "
           "Draws VT=%ld Kamera=%ld eigen=%ld",
           t_s, g_cfg.loc_rb_smooth, g_cfg.loc_rb_smooth_ms, s_meas, mins > 0.0 ? (double)s_meas / mins : 0.0, s_tickmis,
           s_noread, o50, o95, omx, s_yo_max * 180.0 / LS_PI, w50, w95, wmx, s_yw_max * 180.0 / LS_PI, d50, s_contact,
           s_contact_max, s_depth_n[0], s_depth_n[1], s_depth_n[2], s_depth_n[3], s_cat_n[RBI_MIS_AXIS],
           s_cat_max[RBI_MIS_AXIS], s_cat_n[RBI_MIS_DODGE], s_cat_max[RBI_MIS_DODGE], s_cat_n[RBI_MIS_PEDAL],
           s_cat_max[RBI_MIS_PEDAL], s_cat_n[RBI_MIS_GEAR], s_cat_max[RBI_MIS_GEAR], s_cat_n[RBI_MIS_FIRE],
           s_cat_max[RBI_MIS_FIRE], s_cat_n[RBI_MIS_BTN], s_cat_max[RBI_MIS_BTN], s_cat_n[RBI_MIS_HDR],
           s_cat_max[RBI_MIS_HDR], r95, rmx, s95, smx, b50, b95, c95, cmx, q95, qmx, s_smoothed, s_smoothed_cam, s_toobig,
           s_r_lists, s_r_obj, s_r_cam, s_r_own);
    }
    { /* W387 LOC-SMOOTH-OPP: Verzoegerung anderer VTs und sichtbare Spruenge (nur Lagen-Statistik, keine Eingaben) */
        double j50, j95, jmx;
        u32 vis = s_opp_ev[1] + s_opp_ev[2] + s_opp_ev[3];
        double k50, k95, kmx;
        u32 kn;
        ls_pct(&s_jd, &j50, &j95, &jmx);
        AcquireSRWLockShared(&s_rcs);
        ls_pct(&s_rcost, &k50, &k95, &kmx);
        kn = s_rcost.n;
        ReleaseSRWLockShared(&s_rcs);
        HLOG_I("LOCSMOOTH-OPP: t=%ds Modus=%d max=%d vis=%d | Verzoegerung jetzt=%.0f ms Ziel=%d T Mittel=%.1f ms max=%.0f ms "
               "Anteil 0/teils/>=1 T=%.0f/%.0f/%.0f %% Ziel auf/ab=%u/%u | Korrekturen=%u sichtbar=%u (%.2f je min) Bedarf "
               "0/1/2/3+ T=%u/%u/%u/%u verdeckt=%u | angezeigter Gegner-Sprung p50/p95/max=%.1f/%.1f/%.1f | Historie "
               "verkuerzt=%u fehlt=%u Neustart=%u | Tick gleich/anders=%u/%u | Render-Kosten je Liste n=%u p50/p95/max=%.0f/%.0f/%.0f us",
               t_s, ls_opp_mode(), g_cfg.loc_smooth_opp_max, g_cfg.loc_smooth_opp_vis, s_opp_d * 50.0, s_auto.target,
               s_opp_dn ? s_opp_dsum / (double)s_opp_dn * 50.0 : 0.0, s_opp_dmax * 50.0,
               s_opp_dn ? 100.0 * s_opp_dt[0] / s_opp_dn : 0.0, s_opp_dn ? 100.0 * s_opp_dt[1] / s_opp_dn : 0.0,
               s_opp_dn ? 100.0 * s_opp_dt[2] / s_opp_dn : 0.0, s_opp_tgt_up, s_opp_tgt_dn, s_meas, vis,
               mins > 0.0 ? (double)vis / mins : 0.0, s_opp_ev[0], s_opp_ev[1], s_opp_ev[2], s_opp_ev[3], s_opp_hidden, j50,
               j95, jmx, s_opp_short, s_opp_none, s_opp_far, s_opp_tk_eq, s_opp_tk_ne, kn, k50, k95, kmx);
    }
    if (g_cfg.loc_rb_smooth_diag & 8) { /* DIAG(W385): sichtbar im Bild (Render-Listen, Spieleinheiten) */
        double a50, a95, amx, b50, b95, bmx, c50, c95, cmx, d50, d95, dmx, e50, e95, emx;
        u32 nw, nb, nc;
        AcquireSRWLockShared(&s_rcs);
        ls_pct(&s_rv_wr, &a50, &a95, &amx);
        ls_pct(&s_rv_ws, &b50, &b95, &bmx);
        ls_pct(&s_rv_b, &c50, &c95, &cmx);
        ls_pct(&s_rc_wr, &d50, &d95, &dmx);
        ls_pct(&s_rc_ws, &e50, &e95, &emx);
        nw = s_rv_wr.n;
        nb = s_rv_b.n;
        nc = s_rc_wr.n;
        ReleaseSRWLockShared(&s_rcs);
        HLOG_I("LOCSMOOTH-BILD: t=%ds Ruck Anker-Draw anderer VT je Liste im Fenster n=%u roh p50/p95/max=%.1f/%.1f/%.1f angezeigt=%.1f/%.1f/%.1f "
               "| sonst n=%u p50/p95/max=%.1f/%.1f/%.1f | Ruck eigenes Auge im Fenster n=%u roh=%.1f/%.1f/%.1f angezeigt=%.1f/%.1f/%.1f",
               t_s, nw, a50, a95, amx, b50, b95, bmx, nb, c50, c95, cmx, nc, d50, d95, dmx, e50, e95, emx);
    }
}
