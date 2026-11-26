/* render_interp_loc.c - FPS-INTERP LoC: lokale Zielvorhersage, Weltverzug-Glaettung, HUD-/Fadenkreuz-Marken (W376..W448).
 * W463 RCORE: aus render_interp.c ausgegliedert (reines Verschieben); gemeinsamer Zustand in
 * render_interp_int.h, Modulkarte docs/RENDER_ARCH.md. */
#include "render_interp_int.h"

/* Swap N (sb_render_present, vor dem Present). 1 = Present uebernommen. */

/* ---------- DIAG(W376 LOC-LOCAL-AIM) loc_aim_diag ----------
 * Bit 2: je aufgezeichnetem Tick-Bild die Kamera-Matrix der Welt im Backbuffer (haeufigste mvp der Draws vor der
 * ersten BB-Kopie) nach locaim_cam.bin. Bit 8: Draw-Zensus (jedes 200. Bild, hoechstens 8) nach locaim_census.txt
 * mit M = mvp * Kamera^-1 (affin = Welt-Objekt; Verschiebung relativ zum Auge). Nur Spielbild, keine Eingaben. */
/* Weltziel der Liste (Messlaeufe W376_A1/A4): vor dem Absetzen zeichnet LoC die Aussenwelt in den Backbuffer, danach in
 * ein eigenes Render-Ziel (Hauptsicht), das das Cockpit im Backbuffer als Fenster-Textur zeigt; der Lock-On-Monitor
 * folgt als weiteres Ziel. Je Ziel folgt auf die Welt eine Kopie dieses Ziels (W340 IP_COPY: Glow-Kette), danach
 * HUD/2D. Weltziel = erstes Ziel (Reihenfolge der Ops) mit >= 32 3D-Draws vor seiner ersten Kopie; split = diese Kopie.
 * Kein solches Ziel: NULL (keine Welt, keine Korrektur). */
ID3D11RenderTargetView *la_world_rtv(const ip_list_t *L, unsigned *split)
{
    enum { NT = 8 };
    ID3D11RenderTargetView *tg[NT];
    unsigned cnt[NT], nt = 0, i, k;
    for (i = 0; i < L->nops; i++) {
        const ip_op_t *o = &L->ops[i];
        if (o->kind == IP_DRAW && o->cls && o->nv >= 3u && o->rtv) {
            for (k = 0; k < nt && tg[k] != o->rtv; k++) {}
            if (k == nt) { if (nt == NT) { continue; } tg[nt] = o->rtv; cnt[nt] = 0; nt++; }
            cnt[k]++;
        } else if (o->kind == IP_COPY && o->cps) {
            for (k = 0; k < nt && tg[k] != o->cps; k++) {}
            if (k < nt && cnt[k] >= 32u) { *split = i; return tg[k]; }
            if (k < nt) { cnt[k] = 0; } /* kleine Gruppe (z. B. Kartenfenster): weiter suchen */
        }
    }
    *split = 0;
    return NULL;
}
static int la_cam_of(const ip_list_t *L, float cam[16], unsigned *cnt, unsigned *split)
{
    enum { NC = 64 };
    unsigned cidx[NC], cn[NC], nc = 0, i, k, best = 0, sp = L->nops;
    ID3D11RenderTargetView *wt = la_world_rtv(L, &sp);
    if (!wt) { return 0; }
    for (i = 0; i < sp; i++) {
        const ip_op_t *o = &L->ops[i];
        if (o->kind != IP_DRAW || o->rtv != wt || o->nv < 3u || !o->cls) { continue; }
        for (k = 0; k < nc; k++) { if (memcmp(L->ops[cidx[k]].mvp, o->mvp, 64) == 0) { cn[k]++; break; } }
        if (k == nc && nc < NC) { cidx[nc] = i; cn[nc] = 1; nc++; }
    }
    if (!nc) { return 0; }
    for (k = 1; k < nc; k++) { if (cn[k] > cn[best]) { best = k; } }
    memcpy(cam, L->ops[cidx[best]].mvp, 64);
    *cnt = cn[best];
    *split = sp;
    return 1;
}

void li_aim_diag(const ip_list_t *L)
{
    static FILE *fc, *fz;
    static unsigned nl, ncz;
    float cam[16], ci[16], M[16];
    unsigned cnt = 0, sp = 0, i;
    nl++;
    if (!la_cam_of(L, cam, &cnt, &sp)) {
        static unsigned nfail;
        nfail++;
        if ((g_cfg.loc_aim_diag & 8) && (nfail == 1u || nfail == 100u || nfail == 1000u || nfail == 3000u)) { /* Zensus ohne Kamera */
            if (!fz) { fz = (FILE *)loc_aim_diag_file("locaim_census.txt"); }
            if (fz) {
                fprintf(fz, "NOCAM frame=%u serial=%u nops=%u g_rtv=%p\n", L->frame, L->serial, L->nops, (void *)g_rtv);
                for (i = 0; i < L->nops; i++) {
                    const ip_op_t *o = &L->ops[i];
                    fprintf(fz, "%u k=%u rtv=%p cps=%p cls=%u nv=%u\n", i, o->kind, (void *)o->rtv, (void *)o->cps, o->cls, o->nv);
                }
                fflush(fz);
            }
        }
        return;
    }
    if (g_cfg.loc_aim_diag & 2) {
        unsigned hdr[12];
        float pv[4];
        loc_aim_pub_t pb;
        if (!fc) { fc = (FILE *)loc_aim_diag_file("locaim_cam.bin"); }
        if (fc) {
            loc_aim_peek(&pb);
            hdr[0] = 0x3241434Cu; hdr[1] = L->frame; hdr[2] = L->serial; hdr[3] = (unsigned)L->tswap;
            hdr[4] = cnt; hdr[5] = L->nops; hdr[6] = sp; hdr[7] = (unsigned)(L->tswap >> 32);
            hdr[8] = pb.seq; hdr[9] = pb.gframe; hdr[10] = hooks_frame_count(); hdr[11] = (unsigned)pb.n;
            pv[0] = pb.ys; pv[1] = pb.ps; pv[2] = pb.ax; pv[3] = pb.ay;
            fwrite(hdr, sizeof hdr, 1, fc);
            fwrite(cam, 64, 1, fc);
            fwrite(pv, sizeof pv, 1, fc);
            fflush(fc);
        }
    }
    if ((g_cfg.loc_aim_diag & 8) && ip_m4inv(ci, cam) &&
        (g_cfg.loc_aim_diag_n > 0 ? (L->serial >= (unsigned)g_cfg.loc_aim_diag_from &&
                                     L->serial < (unsigned)(g_cfg.loc_aim_diag_from + g_cfg.loc_aim_diag_n) && (L->serial & 3u) == 0u)
                                  : ((nl % 200u) == 0u && ncz < 8u))) { /* Fenster (Serial, jede 4. Liste) bzw. jede 200. */
        if (!fz) { fz = (FILE *)loc_aim_diag_file("locaim_census.txt"); }
        if (!fz) { return; }
        ncz++;
        fprintf(fz, "LIST frame=%u serial=%u nops=%u split=%u camcnt=%u cam=", L->frame, L->serial, L->nops, sp, cnt);
        for (i = 0; i < 16; i++) { fprintf(fz, "%.6g%s", cam[i], i < 15 ? "," : "\n"); }
        for (i = 0; i < L->nops; i++) {
            const ip_op_t *o = &L->ops[i];
            float aff;
            if (o->kind != IP_DRAW) { fprintf(fz, "%u k=%u\n", i, o->kind); continue; }
            ip_m4mul(M, o->mvp, ci);
            aff = fabsf(M[3]) + fabsf(M[7]) + fabsf(M[11]) + fabsf(M[15] - 1.0f);
            fprintf(fz, "%u bb=%d rt=%p cls=%u why=%u fl=%u nv=%u tex=%u eq=%d aff=%.3g T=%.3f,%.3f,%.3f s=%.3f rn0=%.3f,%.3f,%.3f\n",
                    i, o->rtv == g_rtv, (void *)o->sh_rtv, o->cls, o->why, o->flag, o->nv, o->has_tex,
                    memcmp(o->mvp, cam, 64) == 0, aff, M[12], M[13], M[14],
                    sqrtf(M[0] * M[0] + M[1] * M[1] + M[2] * M[2]), o->rn[0][0], o->rn[0][1], o->rn[0][2]);
        }
        fflush(fz);
    }
}

/* W394 LOC-HUD-VIEW: je anderem VT simulierte Bildlage a = proj_VP(p) und angezeigte Lage c = [q, 1] * VP mit
 * q = K * X_j * p (W387-Versatz X_j, W390-Kamera K; jeweils nur wenn aktiv). VP = simulierte Kamera der Liste. */
static void li_hud_data(ip_list_t *L, const double VP[16], const loc_smooth_pub_t *pb, const double *K, double X[][16],
                        unsigned objon)
{
    int j, k;
    L->hm_ok = 0;
    L->hm_gok = pb->own >= 0 && pb->own < LS_MAXV && pb->v[pb->own].ok;
    L->hm_gier = L->hm_gok ? pb->v[pb->own].yaw : 0.0f;
    L->hm_dyaw = (K && L->hm_gok && pb->v[pb->own].on) ? pb->v[pb->own].off[3] : 0.0f;
    for (j = 0; j < LS_MAXV; j++) {
        double p[3], q[3], a[4];
        if (j == pb->own || !pb->v[j].ok) { L->hm_c[j][3] = 0.0; continue; }
        for (k = 0; k < 3; k++) { p[k] = pb->v[j].p[k]; }
        for (k = 0; k < 4; k++) { a[k] = p[0] * VP[k] + p[1] * VP[4 + k] + p[2] * VP[8 + k] + VP[12 + k]; }
        if (a[3] < 1e-3) { L->hm_c[j][3] = 0.0; continue; }
        L->hm_a[j][0] = (float)(a[0] / a[3]);
        L->hm_a[j][1] = (float)(a[1] / a[3]);
        memcpy(q, p, sizeof q);
        if (objon & (1u << j)) { ls_xform_pt(q, q, X[j]); }
        if (K) { ls_xform_pt(q, q, K); }
        for (k = 0; k < 4; k++) { L->hm_c[j][k] = q[0] * VP[k] + q[1] * VP[4 + k] + q[2] * VP[8 + k] + VP[12 + k]; }
        L->hm_ok = 1;
    }
}

/* W394: ohne Versatz (li_smooth_apply ohne Wirkung): Daten aus dem juengsten Stand, simulierte Kamera der Liste. */
void li_hud_plain(ip_list_t *L)
{
    loc_smooth_pub_t pb;
    float cam[16];
    double VP[16];
    unsigned cnt = 0, sp = 0, i;
    if (!loc_smooth_peek(&pb) || !la_cam_of(L, cam, &cnt, &sp) || cnt < 8u) { return; }
    for (i = 0; i < 16; i++) { VP[i] = cam[i]; }
    li_hud_data(L, VP, &pb, NULL, NULL, 0u);
}
static struct {
    unsigned ok[WL_RING], gt[WL_RING];
    double ysim[WL_RING], ydisp[WL_RING];
} wl_r;
static unsigned wl_lists, wl_cam, wl_turn, wl_agree, wl_oppo, wl_errn[WL_SH];
static double wl_lead_sum, wl_lead_abs, wl_rate_sum, wl_err[WL_SH], wl_lead_max;
static unsigned wl_cls_n[7], wl_cls_v[7], wl_cls_ex[7], wl_cls_exv[7], wl_cls_cm[7]; /* Draws, Vertices, davon exakt, Kamera-Lerp */
static unsigned wl_cls_lists;
static unsigned wl_why[4][IP_W_N + 2]; /* Welt mit K: Gruppe 0 Netz >= 1000 Vertices, 1 untexturiert <= 32 (Linien/Pfosten), 2 sonst, 3 Himmel (W435);
                                         * Spalte IP_W_N = davon rzb (Tick-N-Vertices per Matrix), IP_W_N + 1 = rzm (mit der Kamera) */
static double wl_cls_lagdeg[7]; /* Summe Versatz der Klasse gegen die gezeigte Kamera (Grad) */
static LONGLONG wl_tlog;
static double wl_yaw_of(const double e[3], const double f[3])
{
    return atan2(f[0] - e[0], f[2] - e[2]);
}
static double wl_wrap(double a)
{
    while (a > 3.14159265358979) { a -= 6.28318530717959; }
    while (a < -3.14159265358979) { a += 6.28318530717959; }
    return a;
}
static double wl_last_lead; /* Gier gezeigt - simuliert der juengsten Liste (rad) */
static void wl_cam_note(const double VPi[16], const double *K, uint32_t gtick)
{
    double e[3], f[3], q4[4], e2[3], f2[3], Ki[16], ys, yd, rate = 0.0;
    unsigned k, s, slot;
    int have_prev;
    wl_lists++;
    if (!ls_eye_of(e, VPi)) { return; }
    for (k = 0; k < 4; k++) { q4[k] = 0.5 * VPi[8 + k] + VPi[12 + k]; }
    if (fabs(q4[3]) < 1e-12) { return; }
    for (k = 0; k < 3; k++) { f[k] = q4[k] / q4[3]; }
    ys = wl_yaw_of(e, f);
    yd = ys;
    if (K && la_m4inv_d(Ki, K)) { /* gezeigt: Weltpunkt q erscheint, wo die simulierte Kamera q * K sieht -> Auge e * K^-1 */
        ls_xform_pt(e2, e, Ki);
        ls_xform_pt(f2, f, Ki);
        yd = wl_yaw_of(e2, f2);
        wl_cam++;
    }
    wl_last_lead = wl_wrap(yd - ys);
    slot = gtick % WL_RING;
    have_prev = wl_r.ok[(gtick - 1u) % WL_RING] && wl_r.gt[(gtick - 1u) % WL_RING] == gtick - 1u;
    if (have_prev) { rate = wl_wrap(ys - wl_r.ysim[(gtick - 1u) % WL_RING]); }
    wl_r.ok[slot] = 1; wl_r.gt[slot] = gtick; wl_r.ysim[slot] = ys; wl_r.ydisp[slot] = yd;
    if (!have_prev || fabs(rate) < 0.1 * 3.14159265358979 / 180.0) { return; } /* nur gleichmaessiges Drehen (>= 0,1 Grad/Tick) */
    wl_turn++;
    wl_rate_sum += fabs(rate);
    wl_lead_sum += wl_last_lead * (rate > 0.0 ? 1.0 : -1.0); /* positiv = in Drehrichtung voraus */
    wl_lead_abs += fabs(wl_last_lead);
    if (fabs(wl_last_lead) > wl_lead_max) { wl_lead_max = fabs(wl_last_lead); }
    if (fabs(wl_last_lead) > 0.02 * 3.14159265358979 / 180.0) {
        if ((wl_last_lead > 0.0) == (rate > 0.0)) { wl_agree++; } else { wl_oppo++; }
    }
    for (s = 0; s < WL_SH; s++) { /* gezeigt bei Tick g gegen simuliert bei g + (s - 3) */
        int sh = (int)s - 3;
        uint32_t gd = sh >= 0 ? gtick - (uint32_t)sh : gtick, gs = sh >= 0 ? gtick : gtick + (uint32_t)(-sh);
        unsigned id = gd % WL_RING, is = gs % WL_RING;
        if (sh < 0) { /* simuliert spaeter als jetzt: gezeigt von frueher (g + sh) gegen simuliert jetzt */
            gd = gtick - (uint32_t)(-sh); gs = gtick; id = gd % WL_RING; is = gs % WL_RING;
            if (!(wl_r.ok[id] && wl_r.gt[id] == gd)) { continue; }
            wl_err[s] += fabs(wl_wrap(wl_r.ysim[id] - wl_r.ydisp[is])); /* simuliert frueher gegen gezeigt jetzt */
            wl_errn[s]++;
            continue;
        }
        if (!(wl_r.ok[id] && wl_r.gt[id] == gd && wl_r.ok[is] && wl_r.gt[is] == gs)) { continue; }
        wl_err[s] += fabs(wl_wrap(wl_r.ydisp[id] - wl_r.ysim[is]));
        wl_errn[s]++;
    }
}

/* Nach der Zuordnung: Klassen im Weltziel, gelerpt oder exakt (nur Listen mit Drehung bzw. Vorhersage) */
void li_wlag_after(const ip_list_t *L)
{
    unsigned i;
    int any = 0;
    LONGLONG now = ip_now();
    for (i = 0; i < L->nops; i++) {
        const ip_op_t *o = &L->ops[i];
        unsigned c = o->wlc;
        if (o->kind != IP_DRAW || !c || c > 6u) { continue; }
        any = 1;
        wl_cls_n[c]++;
        wl_cls_v[c] += o->nv;
        if (!o->flag) { wl_cls_ex[c]++; wl_cls_exv[c] += o->nv; }
        else if (o->rzm) { wl_cls_cm[c]++; }
        if (c == 2u || c == 3u || c == 5u) { wl_cls_lagdeg[c] += fabs(wl_last_lead) * 180.0 / 3.14159265358979; }
        if (c == 1u || c == 5u) {
            unsigned g = o->nv >= 1000u ? 0u : ((!o->has_tex && o->nv <= 32u) ? 1u : 2u);
            wl_why[g][o->why < IP_W_N ? o->why : 0]++;
            if (o->flag && o->rzb) { wl_why[g][IP_W_N]++; }
            if (o->flag && o->rzm) { wl_why[g][IP_W_N + 1]++; }
        }
        if (c == 6u || o->lsk) { /* W435: Himmel (mit K oder nur erkannt) */
            wl_why[3][o->why < IP_W_N ? o->why : 0]++;
            if (o->flag && o->rzb) { wl_why[3][IP_W_N]++; }
            if (o->flag && o->rzm) { wl_why[3][IP_W_N + 1]++; }
        }
    }
    if (any) { wl_cls_lists++; }
    if (!wl_tlog) { wl_tlog = now; }
    if (ip_ms(wl_tlog, now) >= 10000.0) {
        double dg = 180.0 / 3.14159265358979, tn = wl_turn ? (double)wl_turn : 1.0, cl = wl_cls_lists ? (double)wl_cls_lists : 1.0;
        unsigned s, best = 0;
        char eb[256];
        int n = 0;
        for (s = 0; s < WL_SH; s++) {
            double m = wl_errn[s] ? wl_err[s] / wl_errn[s] * dg : -1.0;
            n += _snprintf_s(eb + n, sizeof eb - (size_t)n, _TRUNCATE, "%s%+d:%.3f", s ? " " : "", (int)s - 3, m);
            if (wl_errn[s] && (!wl_errn[best] || wl_err[s] / wl_errn[s] < wl_err[best] / wl_errn[best])) { best = s; }
        }
        HLOG_I("LOCWLAG: %.1fs Listen=%u mit Kamera K=%u Drehung=%u (Rate %.3f Grad/Tick) | gezeigt-simuliert Mittel %+.3f "
               "(|.| %.3f, max %.3f) Grad, Richtung mit/gegen Drehung=%u/%u | Fehler gezeigt(g) gegen simuliert(g+s) Grad %s -> "
               "bester s=%+d",
               ip_ms(wl_tlog, now) / 1000.0, wl_lists, wl_cam, wl_turn, wl_rate_sum / tn * dg, wl_lead_sum / tn * dg,
               wl_lead_abs / tn * dg, wl_lead_max * dg, wl_agree, wl_oppo, eb, (int)best - 3);
        HLOG_I("LOCWLAG klassen: Listen=%u je Liste Draws/Vertices/exakt/Kamera-Lerp | WeltK %.1f/%.0f/%.1f/%.1f | eigen %.1f/%.0f/%.1f/%.1f "
               "(Versatz %.3f Grad) | nicht-affin %.1f/%.0f/%.1f/%.1f (Versatz %.3f Grad) | VT %.1f/%.0f/%.1f/%.1f | Welt-ohne-K %.1f/%.0f/%.1f/%.1f "
               "| Himmel-mit-K %.1f/%.0f/%.1f/%.1f",
               wl_cls_lists, wl_cls_n[1] / cl, wl_cls_v[1] / cl, wl_cls_ex[1] / cl, wl_cls_cm[1] / cl, wl_cls_n[2] / cl,
               wl_cls_v[2] / cl, wl_cls_ex[2] / cl, wl_cls_cm[2] / cl, wl_cls_n[2] ? wl_cls_lagdeg[2] / wl_cls_n[2] : 0.0,
               wl_cls_n[3] / cl, wl_cls_v[3] / cl, wl_cls_ex[3] / cl, wl_cls_cm[3] / cl,
               wl_cls_n[3] ? wl_cls_lagdeg[3] / wl_cls_n[3] : 0.0, wl_cls_n[4] / cl, wl_cls_v[4] / cl, wl_cls_ex[4] / cl,
               wl_cls_cm[4] / cl, wl_cls_n[5] / cl, wl_cls_v[5] / cl, wl_cls_ex[5] / cl, wl_cls_cm[5] / cl,
               wl_cls_n[6] / cl, wl_cls_v[6] / cl, wl_cls_ex[6] / cl, wl_cls_cm[6] / cl);
        for (s = 0; s < 4u; s++) {
            const unsigned *w = wl_why[s];
            HLOG_I("LOCWLAG why: Welt %s je Liste static=%.1f lerp=%.1f (rzb %.1f, rzm %.1f) rawlerp=%.1f nomatch=%.1f rawchg=%.1f reject=%.1f",
                   s == 0u ? "Netz>=1000" : (s == 1u ? "Linien/Pfosten" : (s == 2u ? "Objekte" : "Himmel")), w[IP_W_STATIC] / cl, w[IP_W_LERP] / cl,
                   w[IP_W_N] / cl, w[IP_W_N + 1] / cl, w[IP_W_RAWLERP] / cl, w[IP_W_NOMATCH] / cl, w[IP_W_RAWCHG] / cl,
                   w[IP_W_REJECT] / cl);
        }
        memset(wl_why, 0, sizeof wl_why);
        wl_lists = wl_cam = wl_turn = wl_agree = wl_oppo = 0;
        wl_lead_sum = wl_lead_abs = wl_rate_sum = wl_lead_max = 0.0;
        memset(wl_err, 0, sizeof wl_err); memset(wl_errn, 0, sizeof wl_errn);
        memset(wl_cls_n, 0, sizeof wl_cls_n); memset(wl_cls_v, 0, sizeof wl_cls_v); memset(wl_cls_ex, 0, sizeof wl_cls_ex);
        memset(wl_cls_exv, 0, sizeof wl_cls_exv); memset(wl_cls_cm, 0, sizeof wl_cls_cm); memset(wl_cls_lagdeg, 0, sizeof wl_cls_lagdeg);
        wl_cls_lists = 0;
        wl_tlog = now;
    }
}
static float ls15_v[LS15_G][LS15_S][LS15_CAP];
static unsigned ls15_n[LS15_G][LS15_S], ls15_nd[LS15_G][LS15_S], ls15_big[LS15_G][LS15_S];
static double ls15_mx[LS15_G][LS15_S];
static unsigned ls15_lists, ls15_turn, ls15_noterr, ls15_terr_raw, ls15_law, ls15_ncons, ls15_nocons, ls15_ln, ls15_pln;
static double ls15_dev_sum[2], ls15_dev_max[2]; /* 0 = groesstes Netz, 1 = W410-Kamera (rzm) gegen die Mehrheits-Kamera, px */
static unsigned ls15_dev_n[2], ls15_dev_big[2];
static double ls15_cam_sum, ls15_cam_max;
static LONGLONG ls15_tlog;
static ls15_top_t ls15_top[8];
static int ls15_fcmp(const void *x, const void *y)
{
    float a = *(const float *)x, b = *(const float *)y;
    return a < b ? -1 : (a > b);
}
static int ls15_ndc(const float c[4], double *x, double *y)
{
    if (c[3] <= 1e-3f) { return 0; }
    *x = (double)c[0] / (double)c[3];
    *y = (double)c[1] / (double)c[3];
    return 1;
}
static void ls15_vmul(float out[4], const float c[4], const double D[16])
{
    int j;
    for (j = 0; j < 4; j++) {
        out[j] = (float)(c[0] * D[0 * 4 + j] + c[1] * D[1 * 4 + j] + c[2] * D[2 * 4 + j] + c[3] * D[3 * 4 + j]);
    }
}
/* Tickweg (waagrecht, px) der Bildmitte in Tiefe 0,99 unter D (Clip, Tick N -> N-1) */
static float ls15_cmove(const double D[16], double hw)
{
    float c[4] = { 0.0f, 0.0f, 0.99f, 1.0f }, d[4];
    ls15_vmul(d, c, D);
    if (d[3] <= 1e-6f) { return -999.0f; }
    return (float)((double)d[0] / (double)d[3] * hw);
}
void li_stat_after(const ip_list_t *A, const ip_list_t *B)
{
    static const float ts[5] = { 1.0f / 6.0f, 2.0f / 6.0f, 3.0f / 6.0f, 4.0f / 6.0f, 5.0f / 6.0f };
    const double hw = 0.5 * (double)(g_bb_w ? g_bb_w : 640u), hh = 0.5 * (double)(g_bb_h ? g_bb_h : 480u);
    const ip_op_t *tr = NULL;
    double Di[16], cam = 0.0;
    unsigned i, q, k, g, s;
    int ti = -1;
    LONGLONG now = ip_now();
    if (A && B && B->pair_ok) {
        for (i = 0; i < B->nops; i++) { /* Gelaende = groesster perspektivische Welt-Draw mit Tiefe */
            const ip_op_t *o = &B->ops[i];
            if (o->kind != IP_DRAW || !o->cls || !o->dsv || ip_mvp_affine(o->mvp) || o->nv < 1000u) { continue; }
            if (!tr || o->nv > tr->nv) { tr = o; ti = (int)i; }
        }
        if (!tr) { ls15_noterr++; }
        else {
            /* Kamera = Mehrheit der Welt-Draws: fuer jeden ruhenden Welt-Draw (Matrix W*C*P) ist M_B^-1*M_A = P^-1*C_B^-1*C_A*P
             * unabhaengig von seiner Weltmatrix W -> haeufigstes D unter den gelerpten, bewegten Matrix-Draws des Ziels
             * (gezaehlt je eigener Matrix; Teile mit gleicher Matrix zaehlen einmal). */
            enum { NC = 16 };
            static double cD[NC][16];
            unsigned cn[NC], ncl = 0, bestc = 0;
            const float *clast[NC];
            const ip_op_t *ta = (tr->match >= 0 && (unsigned)tr->match < A->nops) ? &A->ops[tr->match] : NULL;
            double Dt[16], Dw[16], Dwc[16];
            int dtok = 0, dwok = 0, wcok = 0;
            static float mv[512];
            unsigned nmv = 0;
            for (i = 0; i < B->nops; i++) {
                const ip_op_t *o = &B->ops[i];
                const ip_op_t *a = (o->match >= 0 && (unsigned)o->match < A->nops) ? &A->ops[o->match] : NULL;
                double Dx[16];
                unsigned c;
                if (o->kind != IP_DRAW || !o->cls || !o->dsv || o->rtv != tr->rtv || !o->flag || !a || o->why != IP_W_LERP ||
                    o->rzb || o->rzm || ip_mvp_affine(o->mvp) || memcmp(a->mvp, o->mvp, sizeof o->mvp) == 0) { continue; }
                if (!zc_dmat(Dx, o->mvp, a->mvp)) { continue; }
                if (nmv < 512u) { mv[nmv++] = ls15_cmove(Dx, hw); }
                for (c = 0; c < ncl; c++) { if (zc_derr(Dx, cD[c]) <= 1e-3) { break; } }
                if (c == ncl) {
                    if (ncl >= NC) { continue; }
                    memcpy(cD[c], Dx, sizeof Dx); cn[c] = 0; clast[c] = NULL; ncl++;
                }
                if (!clast[c] || memcmp(clast[c], o->mvp, sizeof o->mvp) != 0) { cn[c]++; clast[c] = o->mvp; }
            }
            for (k = 1; k < ncl; k++) { if (cn[k] > cn[bestc]) { bestc = k; } }
            if (tr->flag && ta) {
                dtok = zc_dmat(Dt, tr->mvp, IP_AMVP(tr, ta)); /* M_B^-1 * M_A' des groessten Netzes */
                if (tr->why == IP_W_RAWLERP && !tr->rzb) { ls15_terr_raw++; } /* Vertex-Lerp: Matrizen allein beschreiben es nicht */
            } else if (!tr->flag) {
                for (k = 0; k < 16; k++) { Dt[k] = (k % 5) == 0 ? 1.0 : 0.0; }
                dtok = 1;
            }
            rz_cam_pre(A, B); /* W410-Kamera (haeufigstes Matrix-Paar, rzm) zum Vergleich */
            dwok = w410_cam_inv(A, B, tr, Dw);
            wcok = w415_world_cam(A, B, tr, Dwc, NULL); /* W415 Welt-Kamera (Bezug, auch ohne Fix) */
            if ((g_cfg.loc_stat_diag & 16) && ls15_pln < 400u && (g_cfg.pf_ring <= 0 || sb_pf_active())) {
                /* DIAG(W415): Tickweg der Bildmitte (waagrecht, px, Tick N-1 -> N gesehen von N) je Kamera-Kandidat */
                float ca[16], cb2[16];
                unsigned cnt = 0, sp = 0;
                double Dl[16];
                int lok = la_cam_of(A, ca, &cnt, &sp) && la_cam_of(B, cb2, &cnt, &sp) && zc_dmat(Dl, cb2, ca);
                if (nmv) { qsort(mv, nmv, sizeof(float), ls15_fcmp); }
                ls15_pln++;
                HLOG_I("LOCSTAT paar: f=%u Welt=%.2f W410=%.2f Mehrheit=%.2f (%u) groesstes=%.2f (n=%u why=%s rzm=%d) la_cam=%.2f | gelerpte Matrix-Draws %u: "
                       "p10 %.2f p50 %.2f p90 %.2f", B->frame, wcok ? ls15_cmove(Dwc, hw) : -999.0, dwok ? ls15_cmove(Dw, hw) : -999.0, ncl ? ls15_cmove(cD[bestc], hw) : -999.0,
                       ncl ? cn[bestc] : 0u, dtok ? ls15_cmove(Dt, hw) : -999.0, tr->nv, pr_why(tr->why), tr->rzm,
                       lok ? ls15_cmove(Dl, hw) : -999.0, nmv, nmv ? mv[nmv / 10u] : 0.0f, nmv ? mv[nmv / 2u] : 0.0f,
                       nmv ? mv[(nmv * 9u) / 10u] : 0.0f);
            }
            /* Bezug: Kamera der Welt-Draws in Weltkoordinaten (haeufigstes gleiches Matrix-Paar, W410/W294; damit bewegt rzm
             * das Gelaende). Bit 4: stattdessen die Mehrheit gleicher D (Messlauf W415_R2b: das sind die Teile des eigenen VT). */
            if ((g_cfg.loc_stat_diag & 4) && ncl && cn[bestc] >= 3u) { memcpy(Di, cD[bestc], sizeof Di); ls15_ncons += cn[bestc]; }
            else if (wcok) { memcpy(Di, Dwc, sizeof Di); ls15_ncons++; }
            else if (dwok) { memcpy(Di, Dw, sizeof Di); ls15_nocons++; }
            else if (dtok) { memcpy(Di, Dt, sizeof Di); ls15_nocons++; }
            else { ls15_noterr++; tr = NULL; }
            if (tr) { /* Abweichung groesstes Netz bzw. Mehrheits-D gegen den Bezug, Probepunkte im Bild (Tiefe 0,99) bei t = 1/6 */
                int px, py, w;
                for (w = 0; w < 2; w++) {
                    const double *Dq = w ? cD[bestc] : Dt;
                    double mx = 0.0;
                    if (!(w ? (ncl > 0u) : dtok)) { continue; }
                    for (px = -1; px <= 1; px++) {
                        for (py = -1; py <= 1; py++) {
                            float cb[4] = { 0.6f * (float)px, 0.6f * (float)py, 0.99f, 1.0f }, c1[4], c2[4], e1[4], e2[4];
                            double x1, y1, x2, y2, d;
                            ls15_vmul(c1, cb, Di); ls15_vmul(c2, cb, Dq);
                            for (k = 0; k < 4; k++) { e1[k] = c1[k] + (cb[k] - c1[k]) * ts[0]; e2[k] = c2[k] + (cb[k] - c2[k]) * ts[0]; }
                            if (!ls15_ndc(e1, &x1, &y1) || !ls15_ndc(e2, &x2, &y2)) { continue; }
                            d = sqrt((x1 - x2) * (x1 - x2) * hw * hw + (y1 - y2) * (y1 - y2) * hh * hh);
                            if (d > mx) { mx = d; }
                        }
                    }
                    ls15_dev_sum[w] += mx; ls15_dev_n[w]++;
                    if (mx > ls15_dev_max[w]) { ls15_dev_max[w] = mx; }
                    if (mx > 0.5) { ls15_dev_big[w]++; }
                }
            }
        }
        if (tr) {
            ls15_lists++;
            { /* Kamerabewegung: Probepunkte bei t = 1/6 gegen Tick N (px) */
                int px, py;
                for (px = -1; px <= 1; px++) {
                    for (py = -1; py <= 1; py++) {
                        float cb[4] = { 0.6f * (float)px, 0.6f * (float)py, 0.99f, 1.0f }, cd[4], cc[4];
                        double x0, y0, x1, y1, d;
                        ls15_vmul(cd, cb, Di);
                        for (k = 0; k < 4; k++) { cc[k] = cd[k] + (cb[k] - cd[k]) * ts[0]; }
                        if (!ls15_ndc(cb, &x0, &y0) || !ls15_ndc(cc, &x1, &y1)) { continue; }
                        d = sqrt((x1 - x0) * (x1 - x0) * hw * hw + (y1 - y0) * (y1 - y0) * hh * hh);
                        if (d > cam) { cam = d; }
                    }
                }
            }
            ls15_cam_sum += cam;
            if (cam > ls15_cam_max) { ls15_cam_max = cam; }
            if (cam >= 0.5) { ls15_turn++; }
            for (i = 0; i < B->nops; i++) {
                const ip_op_t *o = &B->ops[i];
                const ip_op_t *a = (o->match >= 0 && (unsigned)o->match < A->nops) ? &A->ops[o->match] : NULL;
                double best = 0.0, bx = 0.0, by = 0.0;
                int lerp, any = 0;
                if (o->kind != IP_DRAW || !o->cls || !o->dsv || o->rtv != tr->rtv || ip_mvp_affine(o->mvp)) { continue; }
                if (o->wlc == 2u || o->wlc == 4u) { continue; } /* eigener VT bzw. geglaetteter Gegner-VT (bewegt) */
                lerp = o->flag && a != NULL;
                g = o->nv >= 1000u ? 0u : ((!o->has_tex && o->nv <= 32u) ? 1u : ((o->has_tex && o->nv <= 12u) ? 3u : 2u)); /* W506: 3 = Billboards */
                s = !lerp ? 0u : (o->rzm ? 3u : (o->rzb ? 2u : (o->why == IP_W_RAWLERP ? 4u : 1u)));
                if (o->law != tr->law) { ls15_law++; }
                for (q = 0; q < 4; q++) {
                    float cb[4], ca[4], cd[4];
                    ip_clip(o->pref[q], o->mvp, cb);
                    if (cb[3] <= 1e-3f) { continue; }
                    if (lerp) { ip_clip(o->rzb ? o->pref[q] : a->pref[q], IP_AMVP(o, a), ca); if (ca[3] <= 1e-3f) { continue; } }
                    ls15_vmul(cd, cb, Di);
                    if (cd[3] <= 1e-3f) { continue; }
                    for (k = 0; k < 5; k++) {
                        float cs[4], cg[4];
                        double xs, ys, xg, yg, d;
                        unsigned j;
                        for (j = 0; j < 4; j++) {
                            cs[j] = lerp ? ca[j] + (cb[j] - ca[j]) * ts[k] : cb[j];  /* gezeigt */
                            cg[j] = cd[j] + (cb[j] - cd[j]) * ts[k];                 /* Gelaende an derselben Weltstelle */
                        }
                        if (!ls15_ndc(cs, &xs, &ys) || !ls15_ndc(cg, &xg, &yg)) { continue; }
                        if (fabs(xg) > 1.2 || fabs(yg) > 1.2) { continue; } /* ausserhalb des Bildes */
                        d = sqrt((xs - xg) * (xs - xg) * hw * hw + (ys - yg) * (ys - yg) * hh * hh);
                        any = 1;
                        if (d > best) { best = d; bx = xg; by = yg; }
                    }
                }
                if (!any) { continue; }
                if ((g_cfg.loc_stat_diag & 8) && s == 0u && best > 0.5 && ls15_ln < 600u && (g_cfg.pf_ring <= 0 || sb_pf_active())) {
                    /* DIAG(W415): exakter Draw mit Versatz - Matrix gegen die Kamera (X = C_B^-1 * M_B: reine Verschiebung = Weltkoordinaten
                     * mit eigenem Ursprung), Partner-Lage */
                    const ip_op_t *cr = w415_cam_ref(B, o);
                    double X[16], rot = -1.0;
                    int xok = cr && zc_dmat(X, cr->mvp, o->mvp);
                    if (xok) { rot = 0.0; for (k = 0; k < 12; k++) { if ((k & 3) != 3) { double d = fabs(X[k] - ((k % 5) == 0 ? 1.0 : 0.0)); if (d > rot) { rot = d; } } } }
                    ls15_ln++;
                    HLOG_I("LOCSTAT ex: f=%u i=%u n=%u tex=%u why=%s m=%d px=%.2f ndc=%.2f,%.2f w=%.0f gleich_kamera=%d rot=%.5f T=%.1f,%.1f,%.1f vs=%X ps=%X",
                           B->frame, i, o->nv, o->has_tex, pr_why(o->why), o->match, best, bx, by, (double)o->rn[0][2],
                           cr ? memcmp(cr->mvp, o->mvp, sizeof o->mvp) == 0 : -1, rot, xok ? X[12] : 0.0, xok ? X[13] : 0.0, xok ? X[14] : 0.0,
                           o->pvs, o->pps);
                }
                if ((g_cfg.loc_stat_diag & 32) && g == 3u && best > 0.5 && ls15_ln < 1200u && (g_cfg.pf_ring <= 0 || sb_pf_active())) {
                    /* DIAG(W506 ISSUE-105): Billboard (Baum?) mit Versatz - Art, Partner, Shader/Textur, Blend-/Tiefenzustand */
                    ls15_ln++;
                    HLOG_I("LOCSTAT bb: f=%u i=%u n=%u why=%s art=%u m=%d px=%.2f ndc=%.2f,%.2f w=%.0f vs=%X tx=%X ps=%X bs=%p ds=%p fxk=%u",
                           B->frame, i, o->nv, pr_why(o->why), s, o->match, best, bx, by, (double)o->rn[0][2], o->pvs, o->pxp,
                           o->pps, (void *)o->bs, (void *)o->ds, o->fxk);
                }
                ls15_nd[g][s]++;
                if (ls15_n[g][s] < LS15_CAP) { ls15_v[g][s][ls15_n[g][s]++] = (float)best; }
                if (best > ls15_mx[g][s]) { ls15_mx[g][s] = best; }
                if (best > 0.5) { ls15_big[g][s]++; }
                if ((g_cfg.loc_stat_diag & 2) && best > ls15_top[7].px) {
                    ls15_top_t e;
                    int p = 7;
                    e.px = (float)best; e.f = B->frame; e.i = i; e.nv = o->nv; e.tex = o->has_tex; e.why = o->why; e.st = s;
                    e.wlc = o->wlc; e.g = g; e.x = (float)bx; e.y = (float)by;
                    while (p > 0 && ls15_top[p - 1].px < e.px) { ls15_top[p] = ls15_top[p - 1]; p--; }
                    ls15_top[p] = e;
                }
            }
        }
    }
    if (!ls15_tlog) { ls15_tlog = now; }
    if (ip_ms(ls15_tlog, now) >= 10000.0) {
        static const char *gn[LS15_G] = { "Netz>=1000", "Linien/Pfosten", "Objekte", "Billboards" };
        static const char *sn[LS15_S] = { "exakt", "lerp", "rzb", "rzm", "vlerp" };
        char b[900];
        int n;
        HLOG_I("LOCSTAT: %.1fs Paare=%u davon Kamera bewegt (>=0,5 px je Zwischenbild)=%u Kamera mittel %.2f max %.2f px "
               "| ohne Gelaende-Bezug=%u Gelaende Vertex-Lerp=%u law-abweichend=%u | Bild %ux%u",
               ip_ms(ls15_tlog, now) / 1000.0, ls15_lists, ls15_turn, ls15_lists ? ls15_cam_sum / ls15_lists : 0.0,
               ls15_cam_max, ls15_noterr, ls15_terr_raw, ls15_law, g_bb_w, g_bb_h);
        HLOG_I("LOCSTAT kamera: Bezug-Kamera %u Paare (Wert %.1f), ohne Bezug (groesstes Netz) %u | Abweichung bei t=1/6 "
               "gegen den Bezug: groesstes Netz mittel %.2f max %.2f px (>0,5px %u/%u) | Mehrheits-D mittel %.2f max %.2f px (>0,5px %u/%u)",
               ls15_lists - ls15_nocons, (ls15_lists > ls15_nocons) ? (double)ls15_ncons / (double)(ls15_lists - ls15_nocons) : 0.0,
               ls15_nocons, ls15_dev_n[0] ? ls15_dev_sum[0] / ls15_dev_n[0] : 0.0, ls15_dev_max[0], ls15_dev_big[0], ls15_dev_n[0],
               ls15_dev_n[1] ? ls15_dev_sum[1] / ls15_dev_n[1] : 0.0, ls15_dev_max[1], ls15_dev_big[1], ls15_dev_n[1]);
        for (g = 0; g < LS15_G; g++) {
            n = _snprintf_s(b, sizeof b, _TRUNCATE, "LOCSTAT %s:", gn[g]);
            for (s = 0; s < LS15_S; s++) {
                double p95 = 0.0;
                if (ls15_n[g][s]) {
                    qsort(ls15_v[g][s], ls15_n[g][s], sizeof(float), ls15_fcmp);
                    p95 = ls15_v[g][s][(unsigned)((ls15_n[g][s] - 1u) * 0.95)];
                }
                n += _snprintf_s(b + n, sizeof b - (size_t)n, _TRUNCATE, " %s n=%u >0,5px=%u p95=%.2f max=%.2f |", sn[s],
                                 ls15_nd[g][s], ls15_big[g][s], p95, ls15_mx[g][s]);
            }
            HLOG_I("%s", b);
        }
        if (g_cfg.loc_stat_diag & 2) {
            for (k = 0; k < 8 && ls15_top[k].px > 0.0f; k++) {
                const ls15_top_t *e = &ls15_top[k];
                HLOG_I("LOCSTAT top %u: px=%.2f f=%u i=%u n=%u tex=%u gruppe=%s art=%s why=%s wlc=%u ndc=%.2f,%.2f", k, e->px, e->f, e->i,
                       e->nv, e->tex, gn[e->g], sn[e->st], pr_why(e->why), e->wlc, e->x, e->y);
            }
        }
        memset(ls15_n, 0, sizeof ls15_n); memset(ls15_nd, 0, sizeof ls15_nd); memset(ls15_big, 0, sizeof ls15_big);
        memset(ls15_mx, 0, sizeof ls15_mx); memset(ls15_top, 0, sizeof ls15_top);
        ls15_lists = ls15_turn = ls15_noterr = ls15_terr_raw = ls15_law = ls15_ncons = ls15_nocons = 0;
        memset(ls15_dev_sum, 0, sizeof ls15_dev_sum); memset(ls15_dev_max, 0, sizeof ls15_dev_max);
        memset(ls15_dev_n, 0, sizeof ls15_dev_n); memset(ls15_dev_big, 0, sizeof ls15_dev_big);
        ls15_cam_sum = ls15_cam_max = 0.0;
        ls15_tlog = now;
    }
}

/* W412 LOC-WORLD-LAG: Draw in Weltkoordinaten mit eigener Tiefe (Himmel): Spalten x, y, w der mvp gleich der Kamera VP,
 * nur die Tiefenspalte z weicht ab (Messlauf W412_P6: Himmels-Quad n=6, Tiefe auf die Ferne gelegt). mvp * VP^-1 ist
 * dann nicht affin, die Bildlage aber dieselbe wie bei einem Welt-Draw -> Kamera K davor (K * mvp) ist exakt. */
static int wl_same_view(const float mvp[16], const double VP[16])
{
    static const int cols[3] = {0, 1, 3};
    int r, k;
    for (k = 0; k < 3; k++) {
        for (r = 0; r < 4; r++) {
            double a = (double)mvp[r * 4 + cols[k]], b = VP[r * 4 + cols[k]];
            if (fabs(a - b) > 1e-4 * (1.0 + fabs(b))) { return 0; }
        }
    }
    return 1;
}

/* W385 LOC-SMOOTH: Rollback-Korrektur weich zeigen. Am Swap (vor der Zuordnung) die Welt-Draws dieser Liste mit dem
 * abklingenden Versatz des Spiel-Threads verschieben: Draws, deren Weltursprung (mvp * Kamera^-1, affin) einem anderen
 * VT am naechsten liegt (Radius), um dessen Versatz (W' = W * X); mit eigenem Versatz die ganze Welt ausser den Draws am
 * eigenen VT so, dass die Kamera vom vorhergesagten Stand aus sieht (W' = W * K). Danach Bezugs-NDC neu. Ohne Versatz
 * bleibt die Liste unberuehrt (bitgleich). Die Zwischenbilder interpolieren zwischen den so verschobenen Listen. */
void li_smooth_apply(ip_list_t *L)
{
    loc_smooth_pub_t pb;
    float cam[16];
    double VP[16], VPi[16], K[16], X[LS_MAXV][16], eye[3], r2;
    unsigned cnt = 0, sp = 0, i, nobj = 0, ncam = 0, nown = 0, nvt = 0, nwd = 0, nsky = 0;
    unsigned hist[LS_MAXV][6], scnt[LS_MAXV];
    double sraw[LS_MAXV][3], sdisp[LS_MAXV][3], sbd[LS_MAXV]; /* Anker-Draw je VT (naechster Weltursprung) */
    int j, camon = 0, objon = 0, diag = (g_cfg.loc_rb_smooth_diag & 2) != 0, apply, haveye = 0;
    int meas = (g_cfg.loc_rb_smooth_diag & 8) != 0; /* DIAG(W385): sichtbare Lageaenderung je Liste messen */
    int wlag = (g_cfg.loc_wlag_diag & 1) != 0; /* DIAG(W412) */
    static unsigned s_dn, s_dl, s_pser;
    static double s_pdisp[LS_MAXV][3], s_peye[3], s_pstep[LS_MAXV][3], s_pestep[3];
    static double s_praw[LS_MAXV][3], s_prstep[LS_MAXV][3], s_peyer[3], s_perstep[3]; /* rohe Folge (ohne Versatz) */
    static int s_pok[LS_MAXV], s_peok, s_psok[LS_MAXV], s_pesok;
    ID3D11RenderTargetView *wt;
    LARGE_INTEGER lsq0, lsq1, lsqf; /* W387: Kosten je Liste mit Versatz (F2-Regel) */
    QueryPerformanceCounter(&lsq0);
    apply = loc_smooth_get(&pb);
    if (!apply && wlag) { /* DIAG(W412): auch ohne Versatz Klassen und Kamera messen (Gegenprobe) */
        if (!loc_smooth_peek(&pb)) { memset(&pb, 0, sizeof pb); pb.own = -1; }
    } else if (!apply) {
        if (!(diag || meas) || !loc_smooth_peek(&pb)) { return; }
        if (!meas && ((++s_dl % 200u) != 0u || s_dn >= 12u)) { return; } /* DIAG-Zensus: jede 200. Liste, hoechstens 12 */
        if (meas && diag && ((++s_dl % 200u) != 0u || s_dn >= 12u)) { diag = 0; }
    }
    memset(scnt, 0, sizeof scnt);
    memset(sraw, 0, sizeof sraw);
    memset(sdisp, 0, sizeof sdisp);
    if (!la_cam_of(L, cam, &cnt, &sp) || cnt < 8u) { return; }
    wt = la_world_rtv(L, &sp);
    if (!wt) { return; }
    for (i = 0; i < 16; i++) { VP[i] = cam[i]; }
    if (!la_m4inv_d(VPi, VP)) { return; }
    haveye = ls_eye_of(eye, VPi);
    if (apply && pb.own >= 0 && pb.own < LS_MAXV && pb.v[pb.own].ok && pb.v[pb.own].on && haveye) {
        double off[4], oc[4], c3[3];
        for (j = 0; j < 4; j++) { off[j] = pb.v[pb.own].off[j]; }
        for (j = 0; j < 3; j++) { c3[j] = pb.v[pb.own].p[j]; }
        ls_cam_off_pivot(off, eye, c3, oc); /* W390: das Auge dreht mit dem VT um dessen Mittelpunkt */
        ls_cam_xform(K, eye, oc);
        camon = 1;
    }
    for (j = 0; j < LS_MAXV; j++) {
        double p[3], off[4];
        int k;
        if (!apply || j == pb.own || !pb.v[j].ok || !pb.v[j].on) { continue; }
        for (k = 0; k < 3; k++) { p[k] = pb.v[j].p[k]; }
        for (k = 0; k < 4; k++) { off[k] = pb.v[j].off[k]; }
        ls_obj_xform(X[j], p, off);
        objon |= 1 << j;
        nvt++;
    }
    if (g_cfg.loc_hud_predict && apply) { li_hud_data(L, VP, &pb, camon ? K : NULL, X, (unsigned)objon); } /* W394 */
    if (wlag) { wl_cam_note(VPi, camon ? K : NULL, pb.gtick ? pb.gtick : L->serial); } /* DIAG(W412) */
    if (camon || objon) { /* W390: Abweichung einer Zielmarkierung (simulierter Stand, Bildpixel bei 640x480) vom gezeichneten VT */
        double wmax = 0.0, wown = 0.0;
        for (j = 0; j < LS_MAXV; j++) {
            double p4[3], q[3], a[4], b[4], c4[4];
            int k;
            if (j == pb.own || !pb.v[j].ok) { continue; }
            for (k = 0; k < 3; k++) { p4[k] = pb.v[j].p[k]; }
            for (k = 0; k < 4; k++) { a[k] = p4[0] * VP[k] + p4[1] * VP[4 + k] + p4[2] * VP[8 + k] + VP[12 + k]; }
            if (a[3] < 1e-3 || fabs(a[0] / a[3]) > 1.2 || fabs(a[1] / a[3]) > 1.2) { continue; }
            if (camon) { /* nur eigene Kamera */
                ls_xform_pt(q, p4, K);
                for (k = 0; k < 4; k++) { c4[k] = q[0] * VP[k] + q[1] * VP[4 + k] + q[2] * VP[8 + k] + VP[12 + k]; }
                if (c4[3] > 1e-3) {
                    double dx = (c4[0] / c4[3] - a[0] / a[3]) * 320.0, dy = (c4[1] / c4[3] - a[1] / a[3]) * 240.0;
                    if (sqrt(dx * dx + dy * dy) > wown) { wown = sqrt(dx * dx + dy * dy); }
                }
            }
            if (objon & (1 << j)) { ls_xform_pt(q, p4, X[j]); } else { memcpy(q, p4, sizeof q); }
            if (camon) { ls_xform_pt(q, q, K); }
            for (k = 0; k < 4; k++) { b[k] = q[0] * VP[k] + q[1] * VP[4 + k] + q[2] * VP[8 + k] + VP[12 + k]; }
            if (b[3] > 1e-3) {
                double dx = (b[0] / b[3] - a[0] / a[3]) * 320.0, dy = (b[1] / b[3] - a[1] / a[3]) * 240.0;
                if (sqrt(dx * dx + dy * dy) > wmax) { wmax = sqrt(dx * dx + dy * dy); }
            }
        }
        if (camon) { /* gedachte Ziele geradeaus in 30 m und 100 m (unabhaengig davon, ob ein VT im Bild ist) */
            double q4[4], fw[3], fl, t30 = 0.0, t100 = 0.0;
            int k, rr;
            for (k = 0; k < 4; k++) { q4[k] = 0.5 * VPi[8 + k] + VPi[12 + k]; } /* NDC (0, 0, 0.5) zurueck in die Welt */
            if (fabs(q4[3]) > 1e-9) {
                for (k = 0; k < 3; k++) { fw[k] = q4[k] / q4[3] - eye[k]; }
                fl = sqrt(fw[0] * fw[0] + fw[1] * fw[1] + fw[2] * fw[2]);
                for (rr = 0; rr < 2 && fl > 1e-6; rr++) {
                    double R = rr ? 100000.0 : 30000.0, p4[3], q[3], a[4], b[4], dx, dy;
                    for (k = 0; k < 3; k++) { p4[k] = eye[k] + fw[k] / fl * R; }
                    ls_xform_pt(q, p4, K);
                    for (k = 0; k < 4; k++) {
                        a[k] = p4[0] * VP[k] + p4[1] * VP[4 + k] + p4[2] * VP[8 + k] + VP[12 + k];
                        b[k] = q[0] * VP[k] + q[1] * VP[4 + k] + q[2] * VP[8 + k] + VP[12 + k];
                    }
                    if (a[3] < 1e-3 || b[3] < 1e-3) { continue; }
                    dx = (b[0] / b[3] - a[0] / a[3]) * 320.0;
                    dy = (b[1] / b[3] - a[1] / a[3]) * 240.0;
                    if (rr) { t100 = sqrt(dx * dx + dy * dy); } else { t30 = sqrt(dx * dx + dy * dy); }
                }
            }
            loc_smooth_render_mark_virt(t30, t100);
        }
        loc_smooth_render_mark(wmax, wown);
    }
    if (!camon && !objon && !diag && !meas && !wlag) { return; }
    memset(hist, 0, sizeof hist);
    r2 = (double)pb.radius * (double)pb.radius;
    for (i = 0; i < L->nops && i < sp; i++) {
        ip_op_t *o = &L->ops[i];
        double W[16], M[16], bd = -1.0;
        int r, c, best = -1, q;
        if (o->kind != IP_DRAW || o->rtv != wt || !o->cls) { continue; }
        for (r = 0; r < 4; r++)
            for (c = 0; c < 4; c++)
                W[r * 4 + c] = (double)o->mvp[r * 4] * VPi[c] + (double)o->mvp[r * 4 + 1] * VPi[4 + c] +
                               (double)o->mvp[r * 4 + 2] * VPi[8 + c] + (double)o->mvp[r * 4 + 3] * VPi[12 + c];
        if (fabs(W[3]) + fabs(W[7]) + fabs(W[11]) + fabs(W[15] - 1.0) >= 0.02) { /* kein Weltobjekt */
            static unsigned s_wn;
            o->wlc = 3;
            if ((g_cfg.loc_wlag_diag & 2) && s_wn < 24u && (L->serial % 97u) == 0u) { /* DIAG(W412): Matrix nicht-affiner Draws */
                s_wn++;
                HLOG_I("LOCWLAG naff: ser=%u i=%u n=%u tex=%u yawoff=%.5f mvp=%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g "
                       "VP=%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g",
                       L->serial, i, o->nv, o->has_tex, camon ? pb.v[pb.own].off[3] : 0.0f, o->mvp[0], o->mvp[1], o->mvp[2], o->mvp[3],
                       o->mvp[4], o->mvp[5], o->mvp[6], o->mvp[7], o->mvp[8], o->mvp[9], o->mvp[10], o->mvp[11], o->mvp[12],
                       o->mvp[13], o->mvp[14], o->mvp[15], VP[0], VP[1], VP[2], VP[3], VP[4], VP[5], VP[6], VP[7], VP[8], VP[9],
                       VP[10], VP[11], VP[12], VP[13], VP[14], VP[15]);
            }
            if (camon && g_cfg.loc_wlag_fix && wl_same_view(o->mvp, VP)) { /* W412: Himmel mit derselben Kamera wie die Welt */
                double Md[16];
                for (q = 0; q < 16; q++) { Md[q] = (double)o->mvp[q]; }
                ls_m4_mul(Md, K, Md);
                for (q = 0; q < 16; q++) { o->mvp[q] = (float)Md[q]; }
                for (q = 0; q < 4; q++) { /* Bezugs-NDC wie bei der Aufzeichnung (Zuordnung/Plausibilitaet) */
                    float cc[4];
                    ip_clip(o->pref[q], o->mvp, cc);
                    o->rn[q][2] = cc[3];
                    o->rn[q][0] = cc[3] > 1e-6f ? cc[0] / cc[3] : 0.0f;
                    o->rn[q][1] = cc[3] > 1e-6f ? cc[1] / cc[3] : 0.0f;
                }
                o->wlc = 6;
                nsky++;
            }
            continue;
        }
        nwd++;
        for (j = 0; j < LS_MAXV; j++) {
            double dx, dy, dz, d2;
            if (!pb.v[j].ok) { continue; }
            dx = W[12] - pb.v[j].p[0]; dy = W[13] - pb.v[j].p[1]; dz = W[14] - pb.v[j].p[2];
            d2 = dx * dx + dy * dy + dz * dz;
            if (diag) {
                double d = sqrt(d2);
                hist[j][d < 2000.0 ? 0 : (d < 4000.0 ? 1 : (d < 7000.0 ? 2 : (d < 10000.0 ? 3 : (d < 20000.0 ? 4 : 5))))]++;
            }
            if (d2 <= r2 && (best < 0 || d2 < bd)) { best = j; bd = d2; }
        }
        if (best >= 0 && best == pb.own) { /* bewegt sich mit der Kamera */
            o->wlc = 2; /* W412 */
            static unsigned s_own_ln;
            if ((g_cfg.loc_rb_smooth_diag & 16) && camon && s_own_ln < 600u && (g_cfg.pf_ring <= 0 || sb_pf_active())) { /* DIAG(W410) */
                s_own_ln++;
                HLOG_I("LOCSMOOTH own: ser=%u i=%u n=%u tex=%u topo=%u d=%.0f W=%.0f,%.0f,%.0f ndc0=%.2f,%.2f w=%.1f ndc3=%.2f,%.2f w=%.1f",
                       L->serial, i, o->nv, o->has_tex, o->topo, sqrt(bd), W[12], W[13], W[14], o->rn[0][0], o->rn[0][1],
                       o->rn[0][2], o->rn[3][0], o->rn[3][1], o->rn[3][2]);
            }
            nown++;
            continue;
        }
        memcpy(M, W, sizeof M);
        if (best >= 0 && (objon & (1 << best))) { ls_m4_mul(M, M, X[best]); nobj++; }
        if (camon) { ls_m4_mul(M, M, K); ncam++; }
        if (meas && best >= 0 && (!scnt[best] || bd < sbd[best])) { /* Anker: roh W, angezeigt W * X * K (wie gesehen) */
            for (q = 0; q < 3; q++) { sraw[best][q] = W[12 + q]; sdisp[best][q] = M[12 + q]; }
            sbd[best] = bd;
            scnt[best] = 1;
        }
        o->wlc = camon ? 1 : ((best >= 0 && (objon & (1 << best))) ? 4 : 5); /* W412 */
        if (!(best >= 0 && (objon & (1 << best))) && !camon) { continue; }
        ls_m4_mul(M, M, VP);
        for (q = 0; q < 16; q++) { o->mvp[q] = (float)M[q]; }
        for (q = 0; q < 4; q++) { /* Bezugs-NDC wie bei der Aufzeichnung (Zuordnung/Plausibilitaet) */
            float cc[4];
            ip_clip(o->pref[q], o->mvp, cc);
            o->rn[q][2] = cc[3];
            o->rn[q][0] = cc[3] > 1e-6f ? cc[0] / cc[3] : 0.0f;
            o->rn[q][1] = cc[3] > 1e-6f ? cc[1] / cc[3] : 0.0f;
        }
    }
    if (nsky) { InterlockedIncrement(&wl_fix_lists); InterlockedExchangeAdd(&wl_fix_draws, (LONG)nsky); } /* W412 */
    if (camon && (g_cfg.loc_wlag_diag & 4)) { /* DIAG(W435): Listen mit K, davon Himmel mit K, davon Himmel erkannt (li_aim_attach) */
        InterlockedIncrement(&sk_klists);
        if (nsky) { InterlockedIncrement(&sk_klists_sky); }
        if (L->la_ok && L->la_nsk) { InterlockedIncrement(&sk_klists_has); }
    }
    if (apply) {
        loc_smooth_render_note(nobj, ncam, nown, nvt);
        QueryPerformanceCounter(&lsq1); /* W387 */
        QueryPerformanceFrequency(&lsqf);
        loc_smooth_render_cost((double)(lsq1.QuadPart - lsq0.QuadPart) * 1e6 / (double)lsqf.QuadPart);
    }
    if (meas) { /* DIAG(W385): Schritt gegen die zuletzt gezeigte Liste (Mittel der Draws je VT bzw. Auge) */
        int cont = s_pser && L->serial == s_pser + 1u;
        for (j = 0; j < LS_MAXV; j++) { /* Ruck (zweite Differenz) des Anker-Draws: roh gegen die zuletzt gezeigte Lage */
            double sr[3], sd[3], a = 0.0, b = 0.0;
            int q;
            if (j == pb.own || !scnt[j]) { s_pok[j] = 0; s_psok[j] = 0; continue; }
            if (cont && s_pok[j]) {
                for (q = 0; q < 3; q++) { sr[q] = sraw[j][q] - s_praw[j][q]; sd[q] = sdisp[j][q] - s_pdisp[j][q]; }
                if (s_psok[j]) {
                    for (q = 0; q < 3; q++) {
                        a += (sr[q] - s_prstep[j][q]) * (sr[q] - s_prstep[j][q]);
                        b += (sd[q] - s_pstep[j][q]) * (sd[q] - s_pstep[j][q]);
                    }
                    loc_smooth_render_step(0, sqrt(a), sqrt(b), pb.v[j].on);
                }
                memcpy(s_pstep[j], sd, sizeof sd);
                memcpy(s_prstep[j], sr, sizeof sr);
                s_psok[j] = 1;
            } else {
                s_psok[j] = 0;
            }
            memcpy(s_pdisp[j], sdisp[j], sizeof s_pdisp[j]);
            memcpy(s_praw[j], sraw[j], sizeof s_praw[j]);
            s_pok[j] = 1;
        }
        if (haveye && pb.own >= 0 && pb.own < LS_MAXV) {
            double ed[3], a = 0.0, b = 0.0;
            int q;
            for (q = 0; q < 3; q++) ed[q] = eye[q] + (camon ? pb.v[pb.own].off[q] : 0.0); /* Kamera sieht von e + E */
            if (cont && s_peok) {
                double sr[3], sd[3];
                for (q = 0; q < 3; q++) { sr[q] = eye[q] - s_peyer[q]; sd[q] = ed[q] - s_peye[q]; }
                if (s_pesok) {
                    for (q = 0; q < 3; q++) {
                        a += (sr[q] - s_perstep[q]) * (sr[q] - s_perstep[q]);
                        b += (sd[q] - s_pestep[q]) * (sd[q] - s_pestep[q]);
                    }
                    loc_smooth_render_step(1, sqrt(a), sqrt(b), camon);
                }
                memcpy(s_pestep, sd, sizeof sd);
                memcpy(s_perstep, sr, sizeof sr);
                s_pesok = 1;
            } else {
                s_pesok = 0;
            }
            memcpy(s_peye, ed, sizeof ed);
            memcpy(s_peyer, eye, sizeof s_peyer);
            s_peok = 1;
        }
        s_pser = L->serial;
    }
    if (diag && s_dn < 12u) { /* DIAG(W385): Abstand Weltursprung der Draws -> VT (Eichung Radius), keine Eingaben */
        static FILE *fz;
        if (!fz) { fz = (FILE *)loc_aim_diag_file("locsmooth_census.txt"); }
        if (fz) {
            s_dn++;
            fprintf(fz, "LIST serial=%u gtick=%u own=%d radius=%.0f welt=%u apply=%d obj=%u cam=%u eigen=%u\n", L->serial,
                    pb.gtick, pb.own, pb.radius, nwd, apply, nobj, ncam, nown);
            for (j = 0; j < LS_MAXV; j++) {
                if (!pb.v[j].ok) { continue; }
                fprintf(fz, "  VT%d p=%.1f,%.1f,%.1f gier=%.3f off=%.2f,%.2f,%.2f,%.4f  Draws <2000/4000/7000/10000/20000/sonst=%u/%u/%u/%u/%u/%u\n",
                        j, pb.v[j].p[0], pb.v[j].p[1], pb.v[j].p[2], pb.v[j].yaw, pb.v[j].off[0], pb.v[j].off[1],
                        pb.v[j].off[2], pb.v[j].off[3], hist[j][0], hist[j][1], hist[j][2], hist[j][3], hist[j][4], hist[j][5]);
            }
            fflush(fz);
        }
    }
}

/* W376 LOC-LOCAL-AIM: am Swap Vorhersage-Delta und Weltkamera an die Liste haengen, Welt-Draws markieren (Backbuffer,
 * 3D, vor der ersten BB-Kopie, mvp * Kamera^-1 affin = Objekt in der Welt; Cockpit/HUD liegen hinter der Kopie bzw.
 * haben eine eigene Projektion). */
void li_aim_attach(ip_list_t *L)
{
    loc_aim_pub_t pb;
    float cam[16];
    double VPi[16];
    unsigned cnt = 0, sp = 0, i, nw = 0;
    ID3D11RenderTargetView *wt;
    L->la_ok = 0;
    if (g_cfg.loc_local_aim) { if (!loc_aim_get(&pb)) { return; } }
    else if (!loc_aim_peek(&pb) || !pb.ok) { return; } /* DIAG(W376) Bit 32: Vergleichslauf mit Schalter aus */
    if (!la_cam_of(L, cam, &cnt, &sp) || cnt < 8u) { return; }
    wt = la_world_rtv(L, &sp);
    for (i = 0; i < 16; i++) { L->la_vp[i] = cam[i]; }
    if (!la_m4inv_d(VPi, L->la_vp)) { return; }
    L->la_nsk = L->la_nskl = L->la_nna = 0;
    for (i = 0; i < L->nops && i < sp; i++) {
        ip_op_t *o = &L->ops[i];
        double c3[4];
        int k, j;
        o->law = 0;
        o->lsk = 0;
        if (o->kind != IP_DRAW || o->rtv != wt || !o->cls) { continue; }
        for (k = 0; k < 4; k++) {
            c3[k] = 0.0;
            for (j = 0; j < 4; j++) { c3[k] += (double)o->mvp[k * 4 + j] * VPi[j * 4 + 3]; }
        }
        if (fabs(c3[0]) + fabs(c3[1]) + fabs(c3[2]) + fabs(c3[3] - 1.0) < 0.02) { o->law = 1; nw++; }
        else if (wl_same_view(o->mvp, L->la_vp)) {
            /* W435 LOC-SKY-LAG-REG: Himmel (Spalten x, y, w = Kamera, eigene Tiefe; W412). Die Sicht-Korrektur C =
             * VP^-1 * G * VP dreht um das Auge; ihre Zeile z ist (0, 0, 1, 0), d. h. x, y, w des Bildes haengen nur von
             * x, y, w ab -> mvp * C zeigt den Himmel genau dort, wo die korrigierte Kamera ihn sieht (Selbsttest SKY435). */
            o->lsk = 1;
            L->la_nsk++;
            if (g_cfg.loc_sky_fix) { o->law = 1; L->la_nskl++; }
        } else if (o->dsv && !ip_mvp_affine(o->mvp)) { /* sonstige perspektivische Draws ohne affinen Weltbezug (DIAG) */
            static unsigned s_nal;
            L->la_nna++;
            if ((g_cfg.loc_wlag_diag & 4) && s_nal < 24u && (L->serial % 97u) == 0u) {
                s_nal++;
                HLOG_I("LOCWLAG sicht-na: ser=%u i=%u n=%u tex=%u mvp=%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g "
                       "VP=%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g",
                       L->serial, i, o->nv, o->has_tex, o->mvp[0], o->mvp[1], o->mvp[2], o->mvp[3], o->mvp[4], o->mvp[5],
                       o->mvp[6], o->mvp[7], o->mvp[8], o->mvp[9], o->mvp[10], o->mvp[11], o->mvp[12], o->mvp[13], o->mvp[14],
                       o->mvp[15], L->la_vp[0], L->la_vp[1], L->la_vp[2], L->la_vp[3], L->la_vp[4], L->la_vp[5], L->la_vp[6],
                       L->la_vp[7], L->la_vp[8], L->la_vp[9], L->la_vp[10], L->la_vp[11], L->la_vp[12], L->la_vp[13],
                       L->la_vp[14], L->la_vp[15]);
            }
        }
    }
    if (L->la_nskl) { InterlockedIncrement(&sk_lists); InterlockedExchangeAdd(&sk_draws, (LONG)L->la_nskl); } /* W435 */
    L->la_d[0] = pb.dys; L->la_d[1] = pb.dps; L->la_d[2] = pb.dax; L->la_d[3] = pb.day;
    L->la_s[0] = pb.ys; L->la_s[1] = pb.ps; /* W394 */
    { /* Fadenkreuz (Messlauf W376_A9): Mitte im Weltziel x = -py tan(Arm_x - Sicht_x), y = -py tan(Arm_y + Sicht_y),
       * py = Projektion der Weltkamera (|Spalte 1| / |Spalte 3|) */
        double c1 = sqrt(L->la_vp[1] * L->la_vp[1] + L->la_vp[5] * L->la_vp[5] + L->la_vp[9] * L->la_vp[9]);
        double c3 = sqrt(L->la_vp[3] * L->la_vp[3] + L->la_vp[7] * L->la_vp[7] + L->la_vp[11] * L->la_vp[11]);
        double py = c3 > 0.0 ? c1 / c3 : 0.0, rx = (double)pb.ax - pb.ys, ry = (double)pb.ay + pb.ps;
        double rx2 = rx + (double)pb.dax - pb.dys, ry2 = ry + (double)pb.day + pb.dps;
        if (py > 0.0) loc_aimassist_note_py(py); /* W442: Rand der Zielhilfe */
        L->la_rc[0] = (float)(-py * tan(la_clamp(rx, -1.4, 1.4)));
        L->la_rc[1] = (float)(-py * tan(la_clamp(ry, -1.4, 1.4)));
        L->la_rd[0] = (float)(-py * tan(la_clamp(rx2, -1.4, 1.4))) - L->la_rc[0];
        L->la_rd[1] = (float)(-py * tan(la_clamp(ry2, -1.4, 1.4))) - L->la_rc[1];
    }
    L->la_seq = pb.seq;
    L->la_ok = nw > 0u;
    InterlockedIncrement(&li_la_lists);
    InterlockedExchangeAdd(&li_la_draws, (LONG)nw);
}

/* W376: Fadenkreuz-Draws markieren (o->law = 2): kleine untexturierte 2D-Draws (<= 64 Vertices) im Weltziel nach dessen
 * Kopie, erster Vertex in der Box +-0,16 / +-0,12 NDC um die erwartete Mitte (A9: 7 Draws, Rahmen + Marken, alle
 * anderen HUD-Elemente >= 0,3 entfernt). Hat sich die Mitte gegen den Vortick bewegt, muss sich ein zugeordneter
 * Draw ebenso bewegen (+-0,01), sonst ist er ein ruhendes Element, das zufaellig in der Box liegt. */
/* DIAG(W444 LOC-RETICLE-MARK) loc_ret_diag Bit 4 (nur Testfolge): je Liste alle kleinen 2D-Draws im Weltziel nach der
 * Kopie im Umkreis 0,6 NDC der Formel-Mitte (Op, Partner, Signatur, Vertexzahl, Bildlage, Markierung) nach
 * locret_cand.txt. Nur Bildlagen/Draw-Merkmale, keine Eingaben. */
static void li_ret_cand_dump(const ip_list_t *L, const ip_list_t *A, unsigned sp, ID3D11RenderTargetView *wt)
{
    static FILE *f;
    static unsigned nl;
    unsigned i;
    if (nl >= 40000u) { return; }
    if (!f) { f = (FILE *)loc_aim_diag_file("locret_cand.txt"); if (!f) { return; } }
    nl++;
    fprintf(f, "L %u %u rc=%.5f,%.5f arc=%.5f,%.5f aok=%d aret=%d n=%u rj=%u sp=%u nops=%u\n", L->serial, A ? A->serial : 0u,
            L->la_rc[0], L->la_rc[1], A ? A->la_rc[0] : 0.0f, A ? A->la_rc[1] : 0.0f, A ? A->la_ok : 0, A ? A->la_ret : 0,
            L->la_nret, L->la_rjmv, sp, L->nops);
    for (i = sp + 1u; i < L->nops; i++) {
        const ip_op_t *o = &L->ops[i];
        const ip_op_t *a = (A && o->match >= 0 && (unsigned)o->match < A->nops) ? &A->ops[o->match] : NULL;
        if (o->kind != IP_DRAW || o->cls || o->nv > 256u || o->rtv != wt) { continue; }
        if (fabs(o->rn[0][0] - L->la_rc[0]) > 0.6f || fabs(o->rn[0][1] - L->la_rc[1]) > 0.6f) { continue; }
        fprintf(f, "D %u m=%d al=%d law=%d fl=%d tex=%u nv=%u tp=%u sig=%08x s0=%08x gk=%08x vs=%08x px=%08x ps=%08x bs=%p "
                "r=%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f a=%.5f,%.5f\n",
                i, o->match, a ? a->law : -1, o->law, o->flag, o->has_tex, o->nv, o->topo, o->sig, o->sig0, o->gkey, o->pvs,
                o->pxp, o->pps, (void *)o->bs, o->rn[0][0], o->rn[0][1], o->rn[1][0], o->rn[1][1], o->rn[2][0], o->rn[2][1],
                o->rn[3][0], o->rn[3][1], a ? a->rn[0][0] : 9.0f, a ? a->rn[0][1] : 9.0f);
    }
}

/* W444 LOC-RETICLE-MARK (loc_ret_mark, loc_ret_mark.h): Fadenkreuz-Teile ueber Form-Identitaet zur Vorliste (ergaenzt
 * um Teile aus dem Gedaechtnis, hoechstens LRM_MEMT Ticks alt), Zuordnungs-Partner mit Formwechsel und die alte Pruefung
 * (Kasten + Formel-Bewegung) als Start. Nur im Swap (ein Aufrufer). Zaehler fuer die Zeile LOCINTERP retmark444. */
static lrm_mem_t lrm_mem[LRM_MAXP];
static int lrm_mem_n;
static void lrm_part_of(const ip_op_t *o, lrm_part_t *p)
{
    int k;
    for (k = 0; k < 4; k++) { p->p[k][0] = o->rn[k][0]; p->p[k][1] = o->rn[k][1]; }
    p->nv = o->nv;
    p->grp = o->sig0;
}
static void li_aim_reticle_w444(ip_list_t *L, const ip_list_t *A, unsigned sp, ID3D11RenderTargetView *wt, int moved)
{
    lrm_part_t prev[LRM_MAXP], mk[LRM_MAXP];
    unsigned prev_op[LRM_MAXP], cop[LRM_MAXC], i, n = 0;
    lrm_cand_t c[LRM_MAXC];
    unsigned char why[LRM_MAXC];
    int np = 0, nc = 0, k, j, na;
    if (A && A->la_ret) {
        for (i = 0; i < A->nops && np < LRM_MAXP; i++) {
            const ip_op_t *a = &A->ops[i];
            if (a->kind != IP_DRAW || !LA_IS_RET(a->law)) { continue; }
            lrm_part_of(a, &prev[np]);
            prev_op[np] = i;
            np++;
        }
    }
    na = np;
    np = lrm_prev_add_mem(prev, NULL, np, lrm_mem, lrm_mem_n, L->serial);
    for (k = na; k < np; k++) { prev_op[k] = 0xFFFFFFFFu; }
    if (np > na) { InterlockedIncrement(&lrm_st_mem); }
    for (i = sp + 1u; i < L->nops && nc < LRM_MAXC; i++) {
        ip_op_t *o = &L->ops[i];
        int seed = 1;
        if (o->kind != IP_DRAW || o->cls || o->has_tex || o->nv > 64u || o->rtv != wt) { continue; }
        if (fabs(o->rn[0][0] - L->la_rc[0]) > LRM_WIN || fabs(o->rn[0][1] - L->la_rc[1]) > LRM_WIN) { continue; }
        /* alte Pruefung (W376/W394/W439) als Start-Regel */
        if (fabs(o->rn[0][0] - L->la_rc[0]) > 0.16f || fabs(o->rn[0][1] - L->la_rc[1]) > 0.12f) {
            if (!g_cfg.loc_view_perframe || !A || !A->la_ret || o->match < 0 || (unsigned)o->match >= A->nops ||
                !LA_IS_RET(A->ops[o->match].law) || fabs(o->rn[0][0] - L->la_rc[0]) > 0.35f || fabs(o->rn[0][1] - L->la_rc[1]) > 0.3f) {
                seed = 0;
            }
        }
        if (seed && moved && o->match >= 0 && (unsigned)o->match < A->nops) {
            const ip_op_t *a = &A->ops[o->match];
            float ex = (o->rn[0][0] - a->rn[0][0]) - (L->la_rc[0] - A->la_rc[0]);
            float ey = (o->rn[0][1] - a->rn[0][1]) - (L->la_rc[1] - A->la_rc[1]);
            if (fabs(ex) > 0.01f || fabs(ey) > 0.01f) { L->la_rjmv++; seed = 0; }
        }
        lrm_part_of(o, &c[nc].d);
        c[nc].seed = (unsigned char)seed;
        c[nc].prev = -1;
        if (A && o->match >= 0) {
            for (j = 0; j < np; j++) { if (prev_op[j] == (unsigned)o->match) { c[nc].prev = j; break; } }
        }
        cop[nc] = i;
        nc++;
    }
    lrm_mark(prev, np, c, nc, L->la_rc, why);
    for (k = 0; k < nc; k++) {
        ip_op_t *o;
        if (!why[k]) { if (c[k].seed) { InterlockedIncrement(&lrm_st_fremd); } continue; }
        o = &L->ops[cop[k]];
        o->law = 2;
        if (g_cfg.loc_ret_fix && !(o->flag && A && o->match >= 0 && (unsigned)o->match < A->nops)) {
            o->law = LA_LAW_RETX; /* W439: im Zwischenbild exakt aus Tick N -> Verschiebung gegen die Mitte von B */
            InterlockedIncrement(&lr_retx);
        }
        if (why[k] == LRM_FORM) { InterlockedIncrement(&lrm_st_form); }
        else if (why[k] == LRM_PAIR) { InterlockedIncrement(&lrm_st_pair); }
        else { InterlockedIncrement(&lrm_st_seed); }
        if (n < LRM_MAXP) { mk[n] = c[k].d; }
        n++;
    }
    if (n) {
        lrm_mem_n = lrm_mem_update(lrm_mem, lrm_mem_n, mk, n < LRM_MAXP ? (int)n : LRM_MAXP, L->serial);
        InterlockedIncrement(&lrm_st_lists);
    }
    L->la_ret = n > 0u;
    L->la_nret = n;
    InterlockedExchangeAdd(&li_la_rets, (LONG)n);
    InterlockedExchangeAdd(&lr_rjmv, (LONG)L->la_rjmv);
    if ((g_cfg.loc_ret_diag & 4) && (g_cfg.loc_replay_tick[0] || g_cfg.loc_steps[0])) { li_ret_cand_dump(L, A, sp, wt); }
}

void li_aim_reticle(ip_list_t *L, const ip_list_t *A)
{
    unsigned sp = 0, i, n = 0;
    ID3D11RenderTargetView *wt = la_world_rtv(L, &sp);
    int moved = A && A->la_ok && (fabs(L->la_rc[0] - A->la_rc[0]) + fabs(L->la_rc[1] - A->la_rc[1]) > 0.01);
    L->la_ret = 0;
    L->la_nret = L->la_rjmv = 0;
    if (!wt || !sp) { return; }
    if (g_cfg.loc_ret_mark) { li_aim_reticle_w444(L, A, sp, wt, moved); return; } /* W444 LOC-RETICLE-MARK */
    for (i = sp + 1u; i < L->nops; i++) {
        ip_op_t *o = &L->ops[i];
        if (o->kind != IP_DRAW || o->cls || o->has_tex || o->nv > 64u || o->rtv != wt) { continue; }
        if (fabs(o->rn[0][0] - L->la_rc[0]) > 0.16f || fabs(o->rn[0][1] - L->la_rc[1]) > 0.12f) {
            /* W394 (nur mit loc_view_perframe): Partner eines Fadenkreuz-Draws der Vorliste bleibt Fadenkreuz (weitere Box),
             * sonst faellt das Fadenkreuz fuer einen Tick auf den simulierten Stand zurueck (Sprung, Lauf C4) */
            if (!g_cfg.loc_view_perframe || !A || !A->la_ret || o->match < 0 || (unsigned)o->match >= A->nops ||
                !LA_IS_RET(A->ops[o->match].law) || fabs(o->rn[0][0] - L->la_rc[0]) > 0.35f || fabs(o->rn[0][1] - L->la_rc[1]) > 0.3f) {
                continue;
            }
        }
        if (moved && o->match >= 0 && (unsigned)o->match < A->nops) {
            /* W439: Beim schnellen Zielen/Drehen weicht die Formel-Mitte vom gezeichneten Fadenkreuz ab; diese Pruefung
             * verwirft dann Teile (Messung V10: 145 Listen mit 1 statt 7 Draws). Mit loc_ret_mark (W444) ist sie nur noch
             * eine von drei Regeln (li_aim_reticle_w444, loc_ret_mark.h). */
            const ip_op_t *a = &A->ops[o->match];
            float ex = (o->rn[0][0] - a->rn[0][0]) - (L->la_rc[0] - A->la_rc[0]);
            float ey = (o->rn[0][1] - a->rn[0][1]) - (L->la_rc[1] - A->la_rc[1]);
            if (fabs(ex) > 0.01f || fabs(ey) > 0.01f) { L->la_rjmv++; continue; }
        }
        o->law = 2;
        if (g_cfg.loc_ret_fix && !(o->flag && A && o->match >= 0 && (unsigned)o->match < A->nops)) {
            o->law = LA_LAW_RETX; /* W439: im Zwischenbild exakt aus Tick N -> Verschiebung gegen die Mitte von B */
            InterlockedIncrement(&lr_retx);
        }
        n++;
    }
    L->la_ret = n > 0u;
    L->la_nret = n;
    InterlockedExchangeAdd(&li_la_rets, (LONG)n);
    InterlockedExchangeAdd(&lr_rjmv, (LONG)L->la_rjmv);
    if ((g_cfg.loc_ret_diag & 4) && (g_cfg.loc_replay_tick[0] || g_cfg.loc_steps[0])) { li_ret_cand_dump(L, A, sp, wt); }
}
long long isw_q[ISW_N], isw_last;
static unsigned isw_nc, isw_ns;
static double isw_sum[ISW_N], isw_mx, isw_mxp[ISW_N];
void isw_end(void)
{
    double t = 0.0, v[ISW_N];
    int k;
    if (!isw_last) { return; }
    for (k = 0; k < ISW_N; k++) { v[k] = (double)isw_q[k] * 1000.0 / (double)ip_qpf.QuadPart; t += v[k]; }
    isw_nc++;
    if (t > 1.0) { isw_ns++; for (k = 0; k < ISW_N; k++) { isw_sum[k] += v[k]; } }
    if (t > isw_mx) { isw_mx = t; memcpy(isw_mxp, v, sizeof v); }
    isw_last = 0;
}
void isw_log(double secs)
{
    HLOG_I("IMLOCK swap: %.1fs swaps=%u >1ms=%u teile_lang gpu_poll=%.1f snap=%.1f gpu_copy=%.1f w_upload=%.1f aim=%.1f smooth=%.1f rest=%.1f "
           "| laengster %.2fms gpu_poll=%.2f snap=%.2f gpu_copy=%.2f w_upload=%.2f aim=%.2f smooth=%.2f rest=%.2f",
           secs, isw_nc, isw_ns, isw_sum[0], isw_sum[1], isw_sum[2], isw_sum[3], isw_sum[4], isw_sum[5], isw_sum[6],
           isw_mx, isw_mxp[0], isw_mxp[1], isw_mxp[2], isw_mxp[3], isw_mxp[4], isw_mxp[5], isw_mxp[6]);
    HLOG_I("IMLOCK snap: %.1fs sh_ref=%u (%.1fms, davon CopyResource %u in %.1fms) unref=%.1fms",
           secs, isw_gr_n, (double)isw_gr_q * 1000.0 / (double)ip_qpf.QuadPart, isw_cp_n, (double)isw_cp_q * 1000.0 / (double)ip_qpf.QuadPart,
           (double)isw_un_q * 1000.0 / (double)ip_qpf.QuadPart);
    isw_cp_q = isw_un_q = isw_gr_q = 0; isw_cp_n = isw_gr_n = 0;
    isw_nc = isw_ns = 0; isw_mx = 0.0; memset(isw_sum, 0, sizeof isw_sum); memset(isw_mxp, 0, sizeof isw_mxp);
}

/* DIAG(W394) loc_hud_diag Bit 1: HUD-Zensus, solange ein anderer VT im Bild ist (vor li_smooth_apply: simulierte Kamera).
 * Je Liste die VT-Lagen in NDC und alle Draws nach der ersten Kopie des Weltziels bzw. in anderen Zielen (2D/HUD) mit
 * ihren 4 Bezugs-Vertices; jede 4. Ausgabe mit Bild beim naechsten Present. Nur Spielzustand, keine Eingaben. */
void li_hud_census(const ip_list_t *L)
{
    static FILE *fz;
    static unsigned nseen, nout;
    loc_smooth_pub_t pb;
    float cam[16];
    double VP[16];
    unsigned cnt = 0, sp = 0, i;
    int j, vis = 0;
    ID3D11RenderTargetView *wt, *rts[16];
    unsigned nrt = 0;
    extern int loc_mouse_mission(void); /* loc_pad.c: eigener VT abgesetzt */
    if (nout >= 80u || !loc_mouse_mission() || !loc_smooth_peek(&pb) || !la_cam_of(L, cam, &cnt, &sp) || cnt < 8u) { return; }
    wt = la_world_rtv(L, &sp);
    for (i = 0; i < 16; i++) { VP[i] = cam[i]; }
    for (j = 0; j < LS_MAXV; j++) {
        double a[4];
        int k;
        if (j == pb.own || !pb.v[j].ok) { continue; }
        for (k = 0; k < 4; k++) { a[k] = pb.v[j].p[0] * VP[k] + pb.v[j].p[1] * VP[4 + k] + pb.v[j].p[2] * VP[8 + k] + VP[12 + k]; }
        if (a[3] > 1e-3 && a[3] < 250000.0 && fabs(a[0] / a[3]) < 1.0 && fabs(a[1] / a[3]) < 1.0) { vis = 1; } /* nah (< ~250 m) */
    }
    if (!vis || (nseen++ % 8u) != 0u) { return; }
    if (!fz) { fz = (FILE *)loc_aim_diag_file("lochud_census.txt"); }
    if (!fz) { return; }
    nout++;
    if ((nout % 4u) == 1u) { extern unsigned g_shot_census; g_shot_census = 1; }
    fprintf(fz, "LIST frame=%u serial=%u nops=%u split=%u shot=%d own=%d gtick=%u cam=", L->frame, L->serial, L->nops, sp,
            (nout % 4u) == 1u, pb.own, pb.gtick);
    for (i = 0; i < 16; i++) { fprintf(fz, "%.6g%s", cam[i], i < 15 ? "," : "\n"); }
    for (j = 0; j < LS_MAXV; j++) {
        double a[4];
        int k;
        if (!pb.v[j].ok) { continue; }
        for (k = 0; k < 4; k++) { a[k] = pb.v[j].p[0] * VP[k] + pb.v[j].p[1] * VP[4 + k] + pb.v[j].p[2] * VP[8 + k] + VP[12 + k]; }
        fprintf(fz, "  VT%d p=%.0f,%.0f,%.0f gier=%.4f ndc=%.4f,%.4f w=%.1f off=%.1f,%.1f,%.1f,%.4f on=%d\n", j, pb.v[j].p[0],
                pb.v[j].p[1], pb.v[j].p[2], pb.v[j].yaw, a[3] > 1e-6 ? a[0] / a[3] : 9.0, a[3] > 1e-6 ? a[1] / a[3] : 9.0, a[3],
                pb.v[j].off[0], pb.v[j].off[1], pb.v[j].off[2], pb.v[j].off[3], pb.v[j].on);
    }
    for (i = 0; i < L->nops; i++) {
        const ip_op_t *o = &L->ops[i];
        unsigned r;
        if (o->kind == IP_COPY) { fprintf(fz, "%u COPY\n", i); continue; }
        if (o->kind != IP_DRAW) { continue; }
        if (o->rtv == wt && i < sp && o->cls) { continue; } /* Welt (3D vor der Kopie) */
        for (r = 0; r < nrt && rts[r] != o->rtv; r++) {}
        if (r == nrt && nrt < 16u) { rts[nrt++] = o->rtv; }
        fprintf(fz, "%u rt=%u%s cls=%u nv=%u tex=%u srv=%p topo=%u vp=%.0f,%.0f,%.0f,%.0f sig=%08X rn=%.4f,%.4f;%.4f,%.4f;%.4f,%.4f;%.4f,%.4f w=%.3g\n",
                i, r, o->rtv == wt ? "W" : "", o->cls, o->nv, o->has_tex, (void *)o->srv, o->topo, o->vp.TopLeftX, o->vp.TopLeftY,
                o->vp.Width, o->vp.Height, o->sig, o->rn[0][0], o->rn[0][1], o->rn[1][0], o->rn[1][1], o->rn[2][0], o->rn[2][1],
                o->rn[3][0], o->rn[3][1], o->rn[0][2]);
    }
    fflush(fz);
}

/* W394 LOC-HUD-VIEW: HUD-Draws mit Weltbezug markieren (Zensus W394 C3, LoC-Hauptsicht 512x512 nach der Weltkopie):
 *  - Zielmarke/Lock-On-Rahmen je VT j: kleine untexturierte Linien-Draws (<= 32 Vertices, ausser dem festen 22er-Kreuz)
 *    mit Mitte in +-0,16 um die simulierte Bildlage a_j (Marke sitzt ~0,04 hoeher), dazu Beschriftungen (texturiert,
 *    6 Vertices) rechts oben daneben. Bewegt sich der Partner-Draw gegen die Vorliste anders als a_j (> 0,012), ist es
 *    ein ruhendes HUD-Element bzw. das Fadenkreuz -> nicht markiert; ohne Bewegung nur Linien mit 4/8/24 Vertices.
 *  - Kompassband: untexturierter Linien-Draw mit >= 100 Vertices, alle Bezugspunkte bei y -0,65..-0,48, Breite > 1.
 * Fadenkreuz-Draws (law 2) bleiben beim Fadenkreuz. */
void li_hud_mark(ip_list_t *L, const ip_list_t *A)
{
    unsigned sp = 0, i, n = 0, ncmp = 0;
    int j;
    ID3D11RenderTargetView *wt;
    if (!L->hm_ok && !L->hm_gok) { return; }
    wt = la_world_rtv(L, &sp);
    if (!wt || !sp) { return; }
    for (j = 0; j < LS_MAXV; j++) {
        if (L->hm_c[j][3] > 0.0 && L->hm_c[j][3] < 100000.0 && fabs(L->hm_a[j][0]) < 0.9 && fabs(L->hm_a[j][1]) < 0.9) {
            InterlockedIncrement(&li_hm_near);
            break;
        }
    }
    if (L->hm_gok) { /* Radar (Kopf oben, Zensus C3): eigenes quadratisches Ziel <= 512 mit Karte (texturiert, >= 300 Vertices)
                       * und >= 8 kleinen untexturierten Linienzuegen (Ring, Marken) - die Lock-On-Kamera hat keine solchen */
        ID3D11RenderTargetView *rt = NULL, *cand[4];
        unsigned nc = 0, nr = 0, k2, nls;
        for (i = 0; i < L->nops; i++) {
            const ip_op_t *o = &L->ops[i];
            if (o->kind == IP_DRAW && o->cls && o->has_tex && o->nv >= 300u && o->rtv && o->rtv != wt && o->rtv != g_rtv &&
                o->vp.Width == o->vp.Height && o->vp.Width <= 512.0f) {
                for (k2 = 0; k2 < nc && cand[k2] != o->rtv; k2++) {}
                if (k2 == nc && nc < 4u) { cand[nc++] = o->rtv; }
            }
        }
        for (k2 = 0; k2 < nc && !rt; k2++) {
            for (i = 0, nls = 0; i < L->nops; i++) {
                const ip_op_t *o = &L->ops[i];
                if (o->kind == IP_DRAW && o->rtv == cand[k2] && !o->has_tex && o->topo == 3u && o->nv <= 16u) { nls++; }
            }
            if (nls >= 8u) { rt = cand[k2]; }
        }
        for (i = 0; rt && i < L->nops; i++) {
            ip_op_t *o = &L->ops[i];
            if (o->kind != IP_DRAW || !o->cls || o->rtv != rt || o->law) { continue; }
            if (A && A->hm_gok && o->has_tex && o->nv >= 300u && o->match >= 0 && (unsigned)o->match < A->nops) {
                /* Eichung: Drehung der Weltachse x im Clip (Karte) gegen die eigene Gier */
                const ip_op_t *a = &A->ops[o->match];
                double dg = la_wrap((double)L->hm_gier - A->hm_gier);
                double th = la_wrap(atan2(o->mvp[1], o->mvp[0]) - atan2(a->mvp[1], a->mvp[0]));
                if (fabs(dg) > 0.003 && fabs(dg) < 0.2 && fabs(fabs(th / dg) - 1.0) < 0.2) {
                    double sg = th / dg > 0.0 ? 1.0 : -1.0;
                    if (li_rad_n > 0 && sg != li_rad_k) { li_rad_n = 0; }
                    li_rad_k = sg;
                    if (li_rad_n < 1000) { li_rad_n++; }
                }
            }
            o->law = LA_LAW_RAD;
            nr++;
        }
        L->hm_nrad = nr;
        if (nr) { InterlockedIncrement(&li_hm_rad); }
    }
    for (i = sp + 1u; i < L->nops; i++) {
        ip_op_t *o = &L->ops[i];
        double cx, cy, ymin = 9.0, ymax = -9.0, xmin = 9.0, xmax = -9.0;
        int q, best = -1;
        if (o->kind != IP_DRAW || o->cls || o->rtv != wt || o->law) { continue; }
        for (q = 0; q < 4; q++) {
            if (o->rn[q][0] < xmin) { xmin = o->rn[q][0]; } if (o->rn[q][0] > xmax) { xmax = o->rn[q][0]; }
            if (o->rn[q][1] < ymin) { ymin = o->rn[q][1]; } if (o->rn[q][1] > ymax) { ymax = o->rn[q][1]; }
        }
        if (!o->has_tex && o->nv >= 100u && ymin > -0.65 && ymax < -0.48 && xmax - xmin > 1.0) { /* Kompassband */
            o->law = LA_LAW_CMP;
            ncmp++;
            if (A && A->hm_gok && L->hm_gok && o->match >= 0 && (unsigned)o->match < A->nops) { /* Eichung NDC je rad */
                double dg = la_wrap((double)L->hm_gier - A->hm_gier), dx = o->rn[0][0] - A->ops[o->match].rn[0][0];
                if (fabs(dg) > 0.003 && fabs(dg) < 0.2 && fabs(dx / dg - li_cmp_k) < 0.25) { li_cmp_k += 0.05 * (dx / dg - li_cmp_k); }
            }
            continue;
        }
        if (!L->hm_ok || o->nv > 32u || o->nv == 22u) { continue; }
        cx = (o->rn[0][0] + o->rn[1][0] + o->rn[2][0] + o->rn[3][0]) * 0.25;
        cy = (o->rn[0][1] + o->rn[1][1] + o->rn[2][1] + o->rn[3][1]) * 0.25;
        for (j = 0; j < LS_MAXV && best < 0; j++) {
            double ax = L->hm_a[j][0], ay = L->hm_a[j][1];
            if (L->hm_c[j][3] <= 0.0 || fabs(ax) > 1.3 || fabs(ay) > 1.3) { continue; }
            if (!o->has_tex) {
                if (fabs(cx - ax) < 0.16 && fabs(cy - ay - 0.04) < 0.16) { best = j; }
            } else if (o->nv == 6u && cx - ax > 0.05 && cx - ax < 0.5 && cy - ay > -0.05 && cy - ay < 0.25) {
                best = j;
            }
        }
        if (best < 0) { continue; }
        InterlockedIncrement(&li_hm_cand);
        {
            int moved_ok = -1; /* -1 unbekannt, 0 passt nicht, 1 passt */
            double ex = 0.0, ey = 0.0;
            if (A && A->hm_ok && A->hm_c[best][3] > 0.0 && o->match >= 0 && (unsigned)o->match < A->nops) {
                const ip_op_t *a = &A->ops[o->match];
                double ax0 = (a->rn[0][0] + a->rn[1][0] + a->rn[2][0] + a->rn[3][0]) * 0.25;
                double ay0 = (a->rn[0][1] + a->rn[1][1] + a->rn[2][1] + a->rn[3][1]) * 0.25;
                ex = L->hm_a[best][0] - A->hm_a[best][0];
                ey = L->hm_a[best][1] - A->hm_a[best][1];
                moved_ok = fabs((cx - ax0) - ex) < 0.012 && fabs((cy - ay0) - ey) < 0.012;
            }
            if (moved_ok == 0) { InterlockedIncrement(&li_hm_rej_mv); continue; }
            if (fabs(ex) + fabs(ey) < 0.004 && (o->has_tex || (o->nv != 4u && o->nv != 8u && o->nv != 24u))) {
                InterlockedIncrement(&li_hm_rej_st);
                continue;
            }
        }
        o->law = (unsigned char)(3 + best);
        L->hm_vt |= 1u << best;
        n++;
    }
    L->hm_n = n;
    L->hm_ncmp = ncmp;
    if (ncmp) { InterlockedIncrement(&li_hm_cmp); }
    if (n) { InterlockedIncrement(&li_hm_lists); InterlockedExchangeAdd(&li_hm_draws, (LONG)n); }
}
