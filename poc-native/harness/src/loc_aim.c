/* loc_aim.c - W376 LOC-LOCAL-AIM (siehe loc_aim.h). */
#include "loc_aim.h"
#include "hcfg.h"
#include "hlog.h"
#include "sb_profile.h"
#include "xdk_hooks.h"
#include "loc_rbi.h"
#include "loc_aim_law.h"
#include "loc_vpf.h"
#include "loc_hud_math.h"

#include <stdio.h>
#include <share.h>
#include <string.h>
#include <windows.h>
#include <math.h>

typedef uint32_t u32;

#define LA_PLAYER 0x306A84u  /* eigener Spieler-Index (W341) */
#define LA_PBLOCK 0x5C5168u  /* Spielerblock 0 (Flags, W341) */
#define LA_PSTRIDE 0xAE8u
#define LA_VT0 0x5C50D0u     /* VT-Objekt 0 (Spielerblock - 0x98, W376) */
#define LA_DATA0 0x21F340u   /* .data der LoC-XBE */
#define LA_DATA1 0x743278u

static FILE *s_fp, *s_fd;
static unsigned s_ndump;

void *loc_aim_diag_file(const char *name)
{
    char path[600];
    FILE *f = NULL;
    if (!g_cfg.loc_replay_tick[0] && !g_cfg.loc_steps[0]) {
        return NULL; /* DIAG nur in Testlaeufen mit Testfolge (keine Aufzeichnung echter Spielerlaeufe) */
    }
    sprintf_s(path, sizeof path, "%s/%s", hcfg_out_dir(), name);
    f = _fsopen(path, "ab", _SH_DENYNO); /* waehrend des Laufs lesbar */
    return f;
}

/* DIAG loc_aim_diag Bit 1: eigener Spielerblock je Live-Tick (nur Spielzustand, keine Eingaben);
 * Bit 4: ganzes .data in den Ticks loc_aim_diag_from .. +loc_aim_diag_n (Slot). */
static void la_diag_tick(int slot)
{
    u32 hdr[4];
    u32 pl = 0xFFFFFFFFu;
    __try {
        pl = *(volatile const u32 *)(uintptr_t)LA_PLAYER;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (pl >= 10u) {
        return;
    }
    hdr[0] = 0x4D49414Cu; /* "LAIM" */
    hdr[1] = hooks_frame_count();
    hdr[2] = (u32)slot;
    hdr[3] = pl;
    if ((g_cfg.loc_aim_diag & 1) && s_ndump < 20000u) {
        if (!s_fp) {
            s_fp = (FILE *)loc_aim_diag_file("locaim_player.bin");
        }
        if (s_fp) {
            __try {
                fwrite(hdr, sizeof hdr, 1, s_fp);
                fwrite((const void *)(uintptr_t)(LA_PBLOCK + pl * LA_PSTRIDE), LA_PSTRIDE, 1, s_fp);
                s_ndump++;
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }
    if ((g_cfg.loc_aim_diag & 4) && g_cfg.loc_aim_diag_n > 0 && slot >= g_cfg.loc_aim_diag_from && slot < g_cfg.loc_aim_diag_from + g_cfg.loc_aim_diag_n) {
        if (!s_fd) {
            s_fd = (FILE *)loc_aim_diag_file("locaim_data.bin");
        }
        if (s_fd) {
            __try {
                fwrite(hdr, sizeof hdr, 1, s_fd);
                fwrite((const void *)(uintptr_t)LA_DATA0, LA_DATA1 - LA_DATA0, 1, s_fd);
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            fflush(s_fd);
        }
    }
}

/* ---- Vorhersage (loc_local_aim) ---- */
static loc_aim_pub_t s_pub[2];
static volatile LONG s_pub_seq;
static unsigned s_st_n, s_st_ok, s_st_nmax, s_st_hist[5];
static double s_st_sum, s_st_max, s_st_asum, s_st_amax;
static unsigned s_st_t0;

static int la_read(la_state_t *s, u32 *pl_out)
{
    u32 pl, b;
    __try {
        pl = *(volatile const u32 *)(uintptr_t)LA_PLAYER;
        if (pl >= 10u) {
            return 0;
        }
        b = LA_VT0 + pl * LA_PSTRIDE;
        s->yt = *(volatile const float *)(uintptr_t)(b + 0x40Cu);
        s->pt = *(volatile const float *)(uintptr_t)(b + 0x410u);
        s->ys = *(volatile const float *)(uintptr_t)(b + 0x404u);
        s->ps = *(volatile const float *)(uintptr_t)(b + 0x408u);
        s->zoom = *(volatile const float *)(uintptr_t)(b + 0x414u);
        s->ax = *(volatile const float *)(uintptr_t)(b + 0x27Cu);
        s->ay = *(volatile const float *)(uintptr_t)(b + 0x278u);
        s->arate = *(volatile const float *)(uintptr_t)(b + 0x270u);
        s->f94 = *(volatile const u32 *)(uintptr_t)(b + 0x94u);
        s->f98 = *(volatile const u32 *)(uintptr_t)(b + 0x98u);
        s->fa0 = *(volatile const u32 *)(uintptr_t)(b + 0xA0u);
        s->inv_sy = *(volatile const short *)(uintptr_t)(b + 0x53Cu) == 1;
        s->inv_ay = *(volatile const short *)(uintptr_t)(b + 0x53Au) == 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    *pl_out = pl;
    return 1;
}

static void la_publish(const loc_aim_pub_t *v)
{
    LONG q = s_pub_seq + 1;
    s_pub[q & 1] = *v;
    s_pub[q & 1].seq = (uint32_t)q;
    InterlockedExchange(&s_pub_seq, q);
}

static int la_copy(loc_aim_pub_t *out);
int loc_aim_get(loc_aim_pub_t *out)
{
    if (!g_cfg.loc_local_aim || !g_sba_loc) {
        return 0;
    }
    return la_copy(out);
}
int loc_aim_peek(loc_aim_pub_t *out)
{
    memset(out, 0, sizeof *out);
    la_copy(out);
    return out->seq != 0u;
}
static int la_copy(loc_aim_pub_t *out)
{
    LONG q, q2;
    int k;
    for (k = 0; k < 3; k++) {
        q = s_pub_seq;
        if (!q) {
            return 0;
        }
        *out = s_pub[q & 1];
        q2 = s_pub_seq;
        if (q2 == q || q2 == q + 1) { /* q + 1 schreibt den anderen Platz */
            return out->ok;
        }
    }
    return 0;
}

static void la_stat(const loc_aim_pub_t *v)
{
    unsigned now = GetTickCount();
    s_st_n++;
    if (v->ok) {
        double a = fabs((double)v->dys) + fabs((double)v->dps), b = fabs((double)v->dax) + fabs((double)v->day);
        s_st_ok++;
        s_st_sum += a; if (a > s_st_max) s_st_max = a;
        s_st_asum += b; if (b > s_st_amax) s_st_amax = b;
        s_st_hist[v->n < 4 ? v->n : 4]++;
        if ((unsigned)v->n > s_st_nmax) s_st_nmax = (unsigned)v->n;
    }
    if (!s_st_t0) s_st_t0 = now;
    if (now - s_st_t0 >= 10000u) {
        if (g_cfg.loc_aim_diag & 16)
            HLOG_I("LOCAIM: Ticks=%u gueltig=%u Saetze 0/1/2/3/4+=%u/%u/%u/%u/%u | Sicht-Delta mittel %.4f max %.4f rad | "
                   "Arm-Delta mittel %.4f max %.4f rad",
                   s_st_n, s_st_ok, s_st_hist[0], s_st_hist[1], s_st_hist[2], s_st_hist[3], s_st_hist[4],
                   s_st_ok ? s_st_sum / s_st_ok : 0.0, s_st_max, s_st_ok ? s_st_asum / s_st_ok : 0.0, s_st_amax);
        s_st_n = s_st_ok = s_st_nmax = 0;
        memset(s_st_hist, 0, sizeof s_st_hist);
        s_st_sum = s_st_max = s_st_asum = s_st_amax = 0.0;
        s_st_t0 = now;
    }
}

/* W394: Poll des juengsten Satzes, Zeit, Spanne bis zum naechsten Poll (Mittel des Tickabstands minus Abstand Poll ->
 * Veroeffentlichung); DIAG Bit 2: Umrechnung Block -> Satz pruefen (nur Zaehler). */
static void la_vpf_meta(loc_aim_pub_t *v)
{
    static long long s_last, s_f;
    static double s_tp = 50.0, s_dl = 5.0;
    LARGE_INTEGER q;
    long long tp;
    double dt;
    QueryPerformanceCounter(&q);
    if (!s_f) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); s_f = f.QuadPart; }
    v->poll = loc_vpf_poll_seq();
    v->tq = q.QuadPart;
    if (s_last) {
        dt = (double)(q.QuadPart - s_last) * 1000.0 / (double)s_f;
        if (dt > 20.0 && dt < 150.0) s_tp += 0.05 * (dt - s_tp);
    }
    s_last = q.QuadPart;
    tp = loc_vpf_poll_time();
    if (tp) {
        dt = (double)(q.QuadPart - tp) * 1000.0 / (double)s_f;
        if (dt >= 0.0 && dt < 40.0) s_dl += 0.05 * (dt - s_dl);
    }
    v->wspan_ms = (float)(s_tp - s_dl < 10.0 ? 10.0 : s_tp - s_dl);
    if ((g_cfg.loc_hud_diag & 2) && v->have_pst && (g_cfg.loc_replay_tick[0] || g_cfg.loc_steps[0])) {
        /* DIAG(W394, nur Testfolge): Umrechnung Block -> Satz je Achse pruefen (Zaehler; erste Abweichungen als Paar) */
        static unsigned n, ok[4], bad[4], nlog, seen[4][8];
        static int smn[3] = {127, 127, 127}, smx[3] = {-128, -128, -128};
        static float ysmn = 99.0f, ysmx = -99.0f;
        static long long t0;
        uint8_t fin[4];
        int k;
        if (loc_vpf_fin(v->poll, fin)) {
            n++;
            if ((int8_t)fin[2] < smn[0]) smn[0] = (int8_t)fin[2]; /* W396 DIAG: Spanne Sicht im Block / Satz / Kopf */
            if ((int8_t)fin[2] > smx[0]) smx[0] = (int8_t)fin[2];
            if ((int8_t)fin[3] < smn[1]) smn[1] = (int8_t)fin[3];
            if ((int8_t)fin[3] > smx[1]) smx[1] = (int8_t)fin[3];
            if ((int8_t)v->last_in[12] < smn[2]) smn[2] = (int8_t)v->last_in[12];
            if ((int8_t)v->last_in[12] > smx[2]) smx[2] = (int8_t)v->last_in[12];
            if (v->ys < ysmn) ysmn = v->ys;
            if (v->ys > ysmx) ysmx = v->ys;
            for (k = 0; k < 4; k++) {
                uint8_t ex4[4];
                int want, got = (int8_t)v->last_in[10 + k];
                lh_axes_to_frame(fin[0], fin[1], (int8_t)fin[2], (int8_t)fin[3], ex4);
                want = (int8_t)ex4[k];
                if (want == got) ok[k]++;
                else {
                    bad[k]++;
                    if (nlog < 96u && !(seen[k][fin[k] >> 5] & (1u << (fin[k] & 31u)))) { /* je Achse und Wert einmal */
                        seen[k][fin[k] >> 5] |= 1u << (fin[k] & 31u);
                        nlog++;
                        HLOG_I("LOCVPF conv: Achse %d Block %u -> Satz %d (erwartet %d)", k, fin[k], got, want);
                    }
                }
            }
        }
        if (!t0) t0 = q.QuadPart;
        if (q.QuadPart - t0 > 10 * s_f) {
            HLOG_I("LOCVPF: Polls=%u Umrechnung ok/falsch Ziel x %u/%u y %u/%u Sicht x %u/%u y %u/%u | Spanne %.1f ms (Tick %.1f, Poll->Ruhepunkt %.1f)",
                   n, ok[0], bad[0], ok[1], bad[1], ok[2], bad[2], ok[3], bad[3], v->wspan_ms, s_tp, s_dl);
            HLOG_I("LOCVPF Sicht: Block x %d..%d y %d..%d | Satz x %d..%d | Kopf-Gier sim %.3f..%.3f rad", smn[0], smx[0],
                   smn[1], smx[1], smn[2], smx[2], ysmn, ysmx);
            smn[0] = smn[1] = smn[2] = 127;
            smx[0] = smx[1] = smx[2] = -128;
            ysmn = 99.0f;
            ysmx = -99.0f;
            t0 = q.QuadPart;
            n = 0;
            memset(ok, 0, sizeof ok);
            memset(bad, 0, sizeof bad);
        }
    }
}

static void la_predict(const void *rbc)
{
    loc_aim_pub_t v;
    la_state_t s0, s;
    uint8_t pend[16][RBI_IN];
    u32 pl = 0;
    int n = 0, i;
    memset(&v, 0, sizeof v);
    v.gframe = hooks_frame_count();
    if (rbc && la_read(&s0, &pl) && la_can(&s0)) {
        n = rbi_own_pending((const rbi_t *)rbc, pend, 16);
        s = s0;
        v.ok = 1;
        for (i = 0; i < n; i++) {
            if (!la_step(&s, pend[i])) {
                v.ok = 0;
                break;
            }
        }
        if (v.ok) {
            v.n = n;
            if (n > 0) { /* W394: Basis fuer den Zusatz-Schritt je Bild */
                v.pst = s;
                v.have_pst = 1;
                memcpy(v.last_in, pend[n - 1], 14);
            }
            v.dys = (float)la_wrap(s.ys - s0.ys);
            v.dps = (float)la_wrap(s.ps - s0.ps);
            v.dax = (float)la_wrap(s.ax - s0.ax);
            v.day = (float)la_wrap(s.ay - s0.ay);
        }
        v.ys = (float)s0.ys; v.ps = (float)s0.ps; v.ax = (float)s0.ax; v.ay = (float)s0.ay;
        v.yt = (float)s0.yt; v.pt = (float)s0.pt;
    }
    la_vpf_meta(&v);
    la_publish(&v);
    la_stat(&v);
    if (g_cfg.loc_aim_diag & 32) { /* DIAG(W376): Zeit + Delta je Ruhepunkt (Messung Latenz/Nachziehen) */
        static FILE *fp;
        LARGE_INTEGER q;
        if (!fp) {
            fp = (FILE *)loc_aim_diag_file("locaim_pub.bin");
        }
        if (fp) {
            QueryPerformanceCounter(&q);
            fwrite(&q, 8, 1, fp);
            fwrite(&s_pub[s_pub_seq & 1], sizeof(loc_aim_pub_t), 1, fp);
            fflush(fp);
        }
    }
}

void loc_aim_game_tick(int replay, int slot, const void *rbc)
{
    if (!g_sba_loc || replay) {
        return;
    }
    if (g_cfg.loc_aim_diag & 5) {
        la_diag_tick(slot);
    }
    if (g_cfg.loc_local_aim || (g_cfg.loc_aim_diag & 16)) {
        la_predict(rbc);
    }
}
