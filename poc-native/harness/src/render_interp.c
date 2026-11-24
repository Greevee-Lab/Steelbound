/* render_interp.c - FPS-INTERP (W69): Zwischenbilder ueber den 30-Hz-Spieltakt.
 *
 * Die Mission tickt mit 30 Hz (1 Update + 1 Swap + wait(2 VBlanks), FPS120.md).
 * Mit [render] interp_fps>0 zeichnet dieses Modul in der Mission jeden Tick
 * (Swap bis Swap) auf D3D11-Ebene auf und gibt ihn als Zwischenbilder aus:
 *  - Aufzeichnung: jeder Draw des IM-Pfads (sb_im_submit) und der Aspect-
 *    Balken mit seinen fertigen Vertexdaten (liegen im Tick-VB, den der
 *    Spiel-Draw selbst beschreibt), MVP + has_tex und dem Pipeline-Zustand
 *    per Get* (RTV/DSV, Viewport, Rasterizer/Scissor, Blend, DepthStencil +
 *    Ref, Textur t0); Clears mit Ziel-View und Werten. Shader/Layout/Sampler
 *    sind im IM-Pfad konstant. Keine Spiel-Speicher-Zugriffe bei Wiedergabe.
 *  - Schatten-Ziele: das Spiel zeichnet (bei interp_fps>0) in einen Offscreen-
 *    Backbuffer (d3d11_render.c). Jede vom Tick benutzte Render-Ressource
 *    (BB, BB-Tiefe, RTs als Ziel oder Textur) bekommt einen Schatten; am Swap
 *    wird der Spiel-Stand jeder Ressource je Liste gesichert. Wiedergaben
 *    zeichnen nur in Schatten (Anfangsstand = Ende des Vorticks), nie in
 *    Spiel-Ziele -> der Takt darf mitten im naechsten Spiel-Tick praesentieren.
 *  - Exakte Wiedergabe (t = 1) pixelgleich zum Spiel-Bild (interp_verify);
 *    das gezeigte Tick-Bild selbst ist die Sicherung des Spiel-BB.
 *  - Zwischenbild (0 < t < 1): Welt-Draws (3D-Pfad oder VSH-P2) des Ticks N
 *    werden ihrem Partner im Tick N-1 zugeordnet (Signatur aus Ziel, VS,
 *    Textur, PS, Primitiv, Vertexzahl + Hash der Eingangs-Positionen) und
 *    clip-linear interpoliert: pos = lerp(p0*M0, p1*M1, t) im Interp-VS (zwei
 *    Streams, zwei Matrizen) - Kamera, bewegte Objekte, CPU-transformierte
 *    RT-Paesse. 2D/RHW (HUD), unveraenderte und nicht zuordenbare Draws exakt
 *    aus Tick N.
 *  - Takt Echtzeit (vblank_hz>0): fester 1/interp_fps-Takt (QPC). Ein Bild zur
 *    Zeit tau zeigt den Spielzustand zur Zeit tau - L (L ~ 1 Tick + Jitter);
 *    Zustandszeit eines Ticks = Fensteranfang (Feldwechsel) + mittlere Rechen-
 *    zeit. Gezeigt wird aus den Spiel-Hooks (Draws/Clears des naechsten Ticks,
 *    Feld-Poll, VBlank-Warten) - alle auf dem Context-Thread.
 *    Testtakt (vblank_hz=0): K = interp_fps/30 Bilder (t = k/K) sofort am Swap.
 * interp_fps=0: g_ip_recording bleibt 0, sb_interp_swap kehrt sofort mit 0
 * zurueck -> exakt der bisherige Pfad.
 * W463 RCORE: aufgeteilt in render_interp*.c (Modulkarte docs/RENDER_ARCH.md); hier: FPS-INTERP Kern: Takt-Einstieg am Swap (sb_interp_swap), Reset/Freigaben, Verifikation, Einzelaufnahme (rec1). */
#include "render_interp_int.h"
static unsigned ip_serial;
static LONGLONG ip_ts_laste; /* W393: vorige Abweichung der Tick-Zeitbasis (pace_ts_step) */
static LONGLONG ip_need[32];  /* Swap_N - Zustandszeit_N-1 */
volatile LONG li_cp_rec, li_cp_ok, li_cp_rep; /* W340: Kopier-Ops aufgezeichnet / mit Schatten / wiedergegeben (LOCINTERP stat) */
volatile LONG wl_fix_lists, wl_fix_draws; /* W412 LOC-WORLD-LAG: Listen/Draws mit Himmel-Kamera (Zeile LOCINTERP wlag) */
/* W435 LOC-SKY-LAG-REG: Listen/Draws mit Himmel-Sicht-Korrektur (Zeile LOCINTERP sky435); DIAG Bit 4: Wiedergaben mit
 * Sicht-Korrektur, Himmel-Draws darin, davon korrigiert; Listen mit K (W390) und davon mit Himmel-K (W412) */
volatile LONG sk_lists, sk_draws, sk_rep, sk_rep_on, sk_rep_sky, sk_rep_skyc, sk_rep_na, sk_klists, sk_klists_sky, sk_klists_has;
volatile LONG sk_rejlerp; /* W435: Himmel unplausibel -> Matrix-Lerp (statt exakt aus Tick N) */
double sk_last_ang, sk_ang_sum, sk_ang_max; /* DIAG(W435): Winkel der Sicht-Korrektur (rad), Summe/Max in Wiedergaben mit Korrektur */
volatile LONG li_cp_seen; /* W347: Kopien waehrend der Aufzeichnung (SB: INTERP selfcopy) */

int g_ip_recording; /* 1 = Draws/Clears dieses Ticks werden aufgezeichnet */
unsigned g_ip_skip_tot; /* W122 PERF-HUD: verfallene Zwischenbilder gesamt */
unsigned g_ip_sigparts[3]; /* DIAG: VS/Textur/PS des naechsten rec_draw (render_im.c) */
unsigned g_ip_ib1; /* W264 DRAW-OPT2: Indexpuffer-Abschnitt+1 des naechsten rec_draw_src (render_im.c) */

/* DIAG (interp_diag>=3): CPU-Kopie der Positionen je Liste (Ausreisser-Analyse) */
float (*ip_cpos[IP_LISTS])[4];
/* DIAG(W224): Clip-w je Tick-VB-Vertex (nur interp_eyediag > 0).
 * W237: bei P2-Draws im RT (Strom w = 1, CPU hat schon durch w geteilt) die echte Clip-w
 * aus render_im.c (g_ip_tw), dazu NDC x/y des Stroms (Sprung je Vertex, ip_cj). */
const float *g_ip_tw;
unsigned g_ip_tw_n, g_ip_tw_prim;

/* ---------- Verifikation (interp_verify) ---------- */

ID3D11Texture2D *ip_vstg[2];

int ip_stage(int k, ID3D11Resource *src)
{
    if (!ip_vstg[k]) {
        D3D11_TEXTURE2D_DESC dd;
        ID3D11Texture2D_GetDesc((ID3D11Texture2D *)src, &dd);
        dd.Usage = D3D11_USAGE_STAGING;
        dd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        dd.BindFlags = 0;
        dd.MiscFlags = 0;
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &dd, NULL, &ip_vstg[k]))) {
            ip_vstg[k] = NULL;
            return 0;
        }
    }
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)ip_vstg[k], src);
    return 1;
}

unsigned sb_interp_pairs(void)
{
    return ip_pairs_total;
}
static ip_rl_t *ip_rl;
static unsigned ip_rl_n, ip_rl_cap, ip_swapno;
static SRWLOCK ip_rl_lock = SRWLOCK_INIT;
static volatile LONG st_rl_tot;

void sb_interp_release_later(IUnknown *u)
{
    if (!u) { return; }
    if (!ip_init_done || (g_cfg.interp_fps <= 0 && ip_nhist == 0 && ip_rec < 0)) { IUnknown_Release(u); return; }
    AcquireSRWLockExclusive(&ip_rl_lock);
    if (ip_rl_n == ip_rl_cap) {
        unsigned nc = ip_rl_cap ? ip_rl_cap * 2u : 64u;
        ip_rl_t *p = (ip_rl_t *)realloc(ip_rl, nc * sizeof *p);
        if (!p) { /* kein Speicher: lieber festhalten als zu frueh freigeben */
            ReleaseSRWLockExclusive(&ip_rl_lock);
            return;
        }
        ip_rl = p;
        ip_rl_cap = nc;
    }
    ip_rl[ip_rl_n].u = u;
    ip_rl[ip_rl_n].due = ip_swapno + IP_LISTS + 1u;
    ip_rl_n++;
    ReleaseSRWLockExclusive(&ip_rl_lock);
    if (InterlockedIncrement(&st_rl_tot) <= 4) {
        HLOG_I("INTERP W361: Objekt einer Zwischenbild-Liste ersetzt -> Freigabe nach %u Swaps (Render-Thread abgewartet)",
               IP_LISTS + 1u);
    }
}

/* Faellige (all: alle) Objekte freigeben. Unter ctx_lock. */
static void ip_rl_drain(int all)
{
    unsigned i, k = 0;
    int quiet = 0;
    if (!ip_rl_n) { return; }
    AcquireSRWLockExclusive(&ip_rl_lock);
    for (i = 0; i < ip_rl_n; i++) {
        if (all || (int)(ip_swapno - ip_rl[i].due) >= 0) {
            if (!quiet) { ip_rt_quiesce(); quiet = 1; }
            IUnknown_Release(ip_rl[i].u);
        } else {
            ip_rl[k++] = ip_rl[i];
        }
    }
    ip_rl_n = k;
    ReleaseSRWLockExclusive(&ip_rl_lock);
}

/* W81 WINMODE: vor ResizeBuffers alles freigeben, was Backbuffer-Ressourcen
 * (oder ihre Views) haelt: Aufzeichnungs-/Historienlisten (o->rtv/dsv/srv),
 * Schatten-Eintraege (AddRef auf Spiel-BB/RTs, Kopien in BB-Groesse) und
 * die Verify-Staging-Texturen. Der Takt stoppt; die naechsten zwei Ticks
 * bauen die Historie in der neuen Groesse neu auf. Aufruf unter ctx_lock. */
void sb_interp_reset(const char *why)
{
    int i, nsh = 0;
    if (!ip_init_done) { return; }
    ip_rt_quiesce(); /* W126 RENDER-THREAD: Render-Thread liest Listen/Schatten */
    for (i = 0; i < IP_LISTS; i++) { ip_list_reset(&g_ipl[i]); }
    ip_rl_drain(1); /* W361: keine Liste mehr -> alles Verzoegerte frei */
    ip_nhist = 0;
    ip_nneed = 0;
    ip_live = 0;
    ip_rec = -1;
    g_ip_recording = 0;
    ip_shot_ser = 0;
    for (i = 0; i < IP_SH_MAX; i++) {
        if (g_ish[i].res) { nsh++; }
        ip_sh_free(&g_ish[i]);
    }
    for (i = 0; i < 2; i++) {
        if (ip_vstg[i]) { ID3D11Texture2D_Release(ip_vstg[i]); ip_vstg[i] = NULL; }
    }
    HLOG_I("INTERP: Reset (%s): Listen, %d Schatten, Staging freigegeben", why ? why : "", nsh);
}

/* Schatten-BB (exakte Wiedergabe) gegen gesicherten Spiel-BB vergleichen. */
static void ip_verify_check(ip_list_t *B, int bi)
{
    D3D11_MAPPED_SUBRESOURCE m0, m1;
    unsigned x, y, ndiff = 0, maxd = 0, x0 = 0xFFFFFFFFu, y0 = 0xFFFFFFFFu, x1 = 0, y1 = 0;
    ip_sh_t *e;
    if (B->bbref < 0) { return; }
    e = &g_ish[B->bbref];
    if (!ip_stage(0, (ID3D11Resource *)e->sh) || !ip_stage(1, (ID3D11Resource *)e->bak[bi])) { return; }
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)ip_vstg[0], 0, D3D11_MAP_READ, 0, &m0))) {
        return;
    }
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)ip_vstg[1], 0, D3D11_MAP_READ, 0, &m1))) {
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)ip_vstg[0], 0);
        return;
    }
    for (y = 0; y < g_bb_h; y++) {
        const unsigned char *a = (const unsigned char *)m0.pData + (size_t)y * m0.RowPitch;
        const unsigned char *b = (const unsigned char *)m1.pData + (size_t)y * m1.RowPitch;
        if (memcmp(a, b, (size_t)g_bb_w * 4u) == 0) { continue; }
        for (x = 0; x < g_bb_w; x++) {
            unsigned c, d = 0;
            for (c = 0; c < 3u; c++) {
                unsigned dc = a[x * 4u + c] > b[x * 4u + c] ? a[x * 4u + c] - b[x * 4u + c]
                                                             : b[x * 4u + c] - a[x * 4u + c];
                if (dc > d) { d = dc; }
            }
            if (d) {
                ndiff++;
                if (d > maxd) { maxd = d; }
                if (x < x0) { x0 = x; }
                if (y < y0) { y0 = y; }
                if (x > x1) { x1 = x; }
                if (y > y1) { y1 = y; }
            }
        }
    }
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)ip_vstg[1], 0);
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)ip_vstg[0], 0);
    st_verify++;
    if (ndiff) { st_verify_bad++; }
    HLOG_I("INTERP verify f=%u: diff=%u px max=%u box=%u,%u-%u,%u (exakte Wiedergabe vs. Spiel-Bild)",
           B->frame, ndiff, maxd, ndiff ? x0 : 0, ndiff ? y0 : 0, x1, y1);
}

/* ---------- GPU-Zeit einer Wiedergabe (interp_diag, Timestamp-Queries) ---------- */
static ID3D11Query *ip_qd, *ip_qt[2];
static int ip_q_pend;

int ip_gpu_begin(void)
{
    D3D11_QUERY_DESC qd;
    if (g_cfg.interp_diag < 1 || ip_q_pend || (st_presents % 37u) != 0u) { return 0; }
    if (!ip_qd) {
        memset(&qd, 0, sizeof qd);
        qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        if (FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &ip_qd))) { ip_qd = NULL; return 0; }
        qd.Query = D3D11_QUERY_TIMESTAMP;
        if (FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &ip_qt[0])) ||
            FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &ip_qt[1]))) { return 0; }
    }
    ID3D11DeviceContext_Begin(g_ctx, (ID3D11Asynchronous *)ip_qd);
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)ip_qt[0]);
    return 1;
}

void ip_gpu_end(void)
{
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)ip_qt[1]);
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)ip_qd);
    ip_q_pend = 1;
}

static void ip_gpu_poll(void)
{
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
    UINT64 t0, t1;
    if (!ip_q_pend) { return; }
    if (ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)ip_qd, &dj, sizeof dj,
                                    D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) { return; }
    ip_q_pend = 0;
    if (dj.Disjoint || !dj.Frequency) { return; }
    if (ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)ip_qt[0], &t0, sizeof t0, 0) != S_OK ||
        ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)ip_qt[1], &t1, sizeof t1, 0) != S_OK) {
        return;
    }
    {
        double ms = (double)(t1 - t0) * 1000.0 / (double)dj.Frequency;
        st_gpu_ms += ms;
        st_gpu_n++;
        if (ms > st_gpu_max) { st_gpu_max = ms; }
    }
}

/* ---------- Bilder zeigen ---------- */

int ip_list_index(const ip_list_t *L)
{
    return (int)(L - g_ipl);
}

int sb_interp_swap(void)
{
    int mission, took = 0, k, li;
    static unsigned npair_log, ndump;
    LONGLONG t0, now;
    ip_list_t *L = NULL, *A = NULL;
    if (g_cfg.interp_fps <= 0) { return 0; }
    if (!ip_init()) { return 0; }
    t0 = ip_now();
    if (g_cfg.im_lock_split > 0 && ip_rec >= 0 && ip_rec < IP_LISTS) { ip_snap_classify(&g_ipl[ip_rec]); } /* LOC-IM-LOCK: ohne ctx_lock */
    sb_ctx_enter();
    if (g_cfg.imlock_diag) { memset(isw_q, 0, sizeof isw_q); isw_last = ip_now(); } /* DIAG(LOC-IM-LOCK) */
    ip_swapno++; /* W361: Takt der verzoegerten Freigabe */
    ip_gpu_poll();
    ISW(0);
    if (g_cfg.interp_guard_diag) { ip_gd_poll(); } /* DIAG(W265) */
    mission = ip_mission();
    ip_K = g_cfg.interp_fps / ip_tick_hz(); /* W323: LoC-Mission 20 Hz (120 -> K=6), SB 30 */
    if (ip_K < 1) { ip_K = 1; }
    if (ip_K > 8) { ip_K = 8; }
    ip_dt = ip_qpf.QuadPart / g_cfg.interp_fps;
    /* 1. Aufnahme abschliessen und sichern. */
    if (ip_rec >= 0) {
        li = ip_rec;
        L = &g_ipl[li];
        ip_rec = -1;
        st_ticks++;
        L->frame = g_present_no;
        L->serial = ++ip_serial;
        L->tswap = t0;
        if (!mission || L->overflow || !L->nops) {
            if (L->overflow) { st_ovf++; }
            ip_list_reset(L);
            ip_hist_clear();
            L = NULL;
        } else {
            ip_list_snap(L, li);
            ISW(1);
            if (!L->snap_ok) {
                ip_list_reset(L);
                ip_hist_clear();
                L = NULL;
            }
        }
    }
    if (L) {
        L->ready = 1;
        if (g_sba_loc) { li_ticks++; } /* W323 */
        if (g_cfg.ip_vb_gpu > 0) { ip_gpu_copy(L); } /* PERF(PERF-M2,W174) */
        ISW(2);
        ip_w_upload(L); /* FIX(W237) interp_hlerp */
        ISW(3);
        if (g_cfg.interp_rhash & 2) { ip_rh_take(L, L->rh); } /* DIAG(W273) */
        if (g_sba_loc && (g_cfg.loc_aim_diag & 10)) { li_aim_diag(L); } /* DIAG(W376) */
        if (g_sba_loc && (g_cfg.loc_local_aim || (g_cfg.loc_aim_diag & 32))) { li_aim_attach(L); } /* W376 LOC-LOCAL-AIM */
        ISW(4);
        if (g_sba_loc && (g_cfg.loc_hud_diag & 1)) { li_hud_census(L); } /* DIAG(W394) */
        L->hm_ok = 0; L->hm_vt = 0; L->hm_n = 0; L->hm_ncmp = 0; L->hm_nrad = 0; L->hm_gok = 0; L->hm_dyaw = 0.0f; /* W394 */
        if (g_sba_loc && (g_cfg.loc_rb_smooth || g_cfg.loc_own_predict || (g_cfg.loc_rb_smooth_diag & 10))) { li_smooth_apply(L); } /* W385 LOC-SMOOTH, W390 */
        if (g_sba_loc && g_cfg.loc_hud_predict && !L->hm_ok) { li_hud_plain(L); } /* W394 LOC-HUD-VIEW */
        ISW(5);
        A = ip_nhist ? &g_ipl[ip_hist[ip_nhist - 1]] : NULL;
        /* 2. Zuordnung gegen den Vortick. */
        if (A) {
            int log = (g_cfg.interp_diag >= 1) &&
                      ((npair_log < 20u && L->nops >= 500u) || (ip_pairs_total % 300u) == 0u);
            if (log) { npair_log++; }
            if (g_cfg.swap_match_unlock) {
                /* PERF(PERF-TICK,W168) swap_match_unlock=1: Zuordnung ist reine CPU-Arbeit (keine Context-Aufrufe)
                 * auf L (noch nicht in der Historie) und A->ops[].taken (liest die Wiedergabe nicht) -> ctx_lock
                 * waehrenddessen abgeben, damit der Present-Takt nicht 1-3 ms auf den Swap wartet. Ohne Takt-Thread
                 * (Testtakt) wirkungslos. */
                int cr;
                ISW(6); isw_end(); /* DIAG(LOC-IM-LOCK): Halte-Abschnitt endet mit der Freigabe */
                cr = sb_ctx_release_all();
                L->pair_ok = ip_match(A, L, log);
                sb_ctx_reacquire(cr);
            } else
            L->pair_ok = ip_match(A, L, log);
            L->prevser = A->serial;
            if (g_cfg.fxr_diag) { /* DIAG(REM-FX,W133): Zuordnung der Effekt-Draws */
                unsigned q;
                for (q = 0; q < L->nops; q++) {
                    const ip_op_t *fo = &L->ops[q];
                    if (fo->kind == IP_DRAW && fo->fxk) { fxr_interp_note(fo->fxk, fo->why, fo->flag && L->pair_ok); }
                }
            }
            if (g_cfg.interp_diag >= 2 && ndump < 2u && L->nops >= 500u) {
                ndump++;
                ip_dump(L);
            }
            st_pairs++;
            ip_pairs_total++;
        } else {
            st_nopair++;
        }
        if (g_sba_loc && L->la_ok) { li_aim_reticle(L, A); } /* W376 LOC-LOCAL-AIM: braucht die Zuordnung */
        if (g_sba_loc && g_cfg.loc_hud_predict) { li_hud_mark(L, A); } /* W394 LOC-HUD-VIEW: nach dem Fadenkreuz */
        if (g_sba_loc && (g_cfg.loc_wlag_diag & 1)) { li_wlag_after(L); } /* DIAG(W412 LOC-WORLD-LAG) */
        if (g_sba_loc && (g_cfg.loc_stat_diag & 1)) { li_stat_after(A, L); } /* DIAG(W415 LOC-STATIC-LAG) */
        /* 3. Zustandszeit: Fensteranfang (Feldwechsel) + mittlere Rechenzeit r
         *    (regelmaessiger als die Swap-Zeit selbst); Testtakt: Swap-Zeit. */
        L->tstate = t0;
        LONGLONG vb = ip_vb_get(); /* W323: LoC schreibt der Haupt-Thread */
        if (g_sba_loc && L->tvb0 && g_cfg.loc_interp_anchor) { vb = L->tvb0; } /* W340: Feld des Tick-Anfangs */
        if (g_cfg.vblank_hz > 0 && vb && t0 - vb < ip_qpf.QuadPart / 10) {
            double r = (double)(t0 - vb);
            double rms = r * 1000.0 / (double)ip_qpf.QuadPart;
            ip_rbar = ip_rbar > 0.0 ? ip_rbar * 0.9 + r * 0.1 : r;
            L->tstate = vb + (LONGLONG)ip_rbar;
            st_r_ms += rms;
            if (rms > st_r_max) { st_r_max = rms; }
            st_r_n++;
            ip_r_last_ms = rms; /* PERF(TICK-PERF,W125): interp_gpu_tight */
        }
        if (A && L->tstate <= A->tstate) { L->tstate = A->tstate + 1; }
        if (g_sba_loc && A) { /* W323: Zustandsabstand gegen 1/Takt */
            double dv = fabs(ip_ms(A->tstate, L->tstate) - 1000.0 / (double)ip_tick_hz());
            if (dv > li_dev_max) { li_dev_max = dv; }
            if (dv > 2.0) { li_dev2++; }
        }
        if (A && ip_pace_on() && ip_tick_hz() > 0) {
            /* W393 LOC-PACING: geglaettete Tick-Zeitbasis. Die Zustandszeit liegt auf dem Feldraster (Feld des Tick-
             * Anfangs + mittlere Rechenzeit); beginnt ein Tick ein Feld (16,7 ms) zu spaet und holt der naechste auf,
             * lief die Anzeige einen Tick 25 % langsamer und dann 50 % schneller (Mikroruckeln). Jetzt: Erwartung
             * = vorige Zustandszeit + 1/Takt, die Abweichung fliesst nur zu 1/8 ein (hoechstens 4 % des Takts je Tick);
             * mehr als 1/2 Takt bzw. zweimal in Folge mehr als 1/8 Takt daneben (Zeit verloren) setzt neu an. */
            LONGLONG P = ip_qpf.QuadPart / ip_tick_hz(), e = L->tstate - (A->tstate + P);
            int sn;
            L->tstate = (LONGLONG)pace_ts_step(A->tstate, L->tstate, P, &sn, &ip_ts_laste);
            if (sn) { pc_ts_snap++; } else { pc_ts_s++; }
            if (!sn && (e > P / 8 || e < -P / 8)) { pc_ts_big++; } /* DIAG: so viel haette die Anzeige geruckt */
        }
        /* 4. Latenz L = max(Swap_N - Zustand_N-1) der letzten 32 Ticks + 1 ms. */
        if (A) {
            unsigned w;
            LONGLONG mx = 0;
            ip_need[ip_nneed++ % 32u] = t0 - A->tstate;
            for (w = 0; w < 32u && w < ip_nneed; w++) {
                if (ip_need[w] > mx) { mx = ip_need[w]; }
            }
            ip_lat = mx + ip_qpf.QuadPart / 1000;
            if (ip_lat > ip_qpf.QuadPart / 12) { ip_lat = ip_qpf.QuadPart / 12; } /* <= 83 ms */
            st_lat_ms += (double)ip_lat * 1000.0 / (double)ip_qpf.QuadPart;
            st_lat_n++;
        }
        /* 5. Historie: juengste 3 Listen behalten. */
        if (ip_nhist == IP_HIST) {
            ip_rt_drop(ip_hist[0]); /* W126 RENDER-THREAD: Auftrag mit dieser Liste abwarten/verwerfen */
            ip_list_reset(&g_ipl[ip_hist[0]]);
            memmove(ip_hist, ip_hist + 1, (IP_HIST - 1) * sizeof ip_hist[0]);
            ip_nhist--;
        }
        ip_hist[ip_nhist++] = li;
        ip_shots_tick(L);
        if (A && g_cfg.interp_diag >= 3 && g_cfg.seam_diag > 0) { ip_seam_diag(A, ip_list_index(A), L, li); } /* DIAG(W406) */
        if (A && g_cfg.interp_diag >= 3 && g_cfg.interp_ddrel > 0) { /* DIAG(W251 INTERP-GROUND) */
            unsigned m0 = sb_pf_m0(), r = (unsigned)g_cfg.interp_ddrel;
            if (m0 && L->frame >= m0 + r && L->frame < m0 + r + 3u) {
                if (!ip_dd_lo) { ip_dd_lo = L->serial; }
                ip_dd_ser = L->serial;
                ip_diag_tick(A, ip_list_index(A), L, li);
            }
        } else if (A && g_cfg.interp_diag >= 3 && g_cfg.interp_ddfile[0]) { /* DIAG(W284 INTERP-RAWCHG) */
            unsigned m0 = sb_pf_m0();
            if (m0 && L->frame >= m0 && ip_ddf_has(L->frame - m0)) {
                L->ddsel = 1;
                ip_diag_tick(A, ip_list_index(A), L, li);
            }
        } else if (A && g_cfg.interp_diag >= 3 && ip_shot_ser == L->serial) {
            ip_diag_tick(A, ip_list_index(A), L, li);
        }
        if (A && g_cfg.interp_verify > 0 && (ip_pairs_total % (unsigned)g_cfg.interp_verify) == 1u) {
            /* W126 RENDER-THREAD: exakte Wiedergabe ueber denselben Pfad wie die
             * Zwischenbilder (Command-List des Render-Threads) pruefen. */
            ID3D11CommandList *vcl = ip_rt_active() ? ip_rt_get(A, L, 1.0f) : NULL;
            if (vcl) { ip_rt_exec(vcl); }
            else if (!ip_dead_n) { ip_replay(L, li, A, ip_list_index(A), 1.0f); } /* W361: tote View nie binden */
            ip_verify_check(L, li);
        }
        if (A && g_cfg.interp_mode == 1 && L->pair_ok && sb_rp_want()) { ip_rp_tick(A, ip_list_index(A), L, li); } /* W295 WORLD-REPROJ */
    }
    /* 6. Naechste Aufnahme (freie Liste). */
    if (mission) {
        for (k = 0; k < IP_LISTS; k++) {
            int inh = 0, j;
            for (j = 0; j < ip_nhist; j++) {
                if (ip_hist[j] == k) { inh = 1; }
            }
            if (!inh) { break; }
        }
        ip_list_reset(&g_ipl[k]);
        ip_bset_rotate(&g_ipl[k]); /* PERF(TICK-PERF,W125) interp_bsets */
        ip_rec = (ip_vb_ensure(&g_ipl[k]) && ip_rq_ensure(&g_ipl[k])) ? k : -1;
        if (ip_rec >= 0 && g_cfg.render_thread > 0) { /* W126: s0 am Tick-Anfang (Deferred-Wiedergabe) */
            ID3D11DeviceContext_PSGetSamplers(g_ctx, 0, 1, &g_ipl[k].smp0);
        }
    } else {
        ip_hist_clear();
        ip_rec = -1;
    }
    g_ip_recording = (ip_rec >= 0);
    now = ip_now();
    st_rec_ms += ip_ms(t0, now);
    if (g_phud_meas) { sb_phud_interp_q(now - t0, 0); } /* W122 PERF-HUD: Aufzeichnung/Zuordnung */
    /* 7. Zeigen. Testtakt: K Bilder (t = k/K) des Paars sofort am Swap.
     *    Echtzeit: Takt starten bzw. weiterlaufen lassen (zeigt auch das
     *    Spiel-Bild dieses Ticks, um L verzoegert). */
    if (L && ip_K > 1) {
        if (g_cfg.vblank_hz <= 0) {
            if (A) {
                char tag[8];
                if (g_cfg.interp_rhash & 2) { ip_rh_check(A, "A", L->frame, 0); ip_rh_check(L, "B", L->frame, 0); } /* DIAG(W273) */
                for (k = 1; k <= ip_K; k++) {
                    _snprintf_s(tag, sizeof tag, _TRUNCATE, "%d", k);
                    if (ip_rt_ok && k + 1 < ip_K) { ip_rt_nA = A; ip_rt_nB = L; ip_rt_nt = (float)(k + 1) / (float)ip_K; } /* W126 */
                    ip_show(A, L, k >= ip_K ? 1.0f : (float)k / (float)ip_K, tag);
                    ip_rt_nA = NULL;
                }
                if (g_cfg.interp_rhash & 2) { ip_rh_check(A, "A", L->frame, 9); } /* DIAG(W273): nach den Bildern */
                took = 1;
            }
        } else if (ip_nhist >= 2) {
            if (!ip_live) {
                ip_live = 1;
                ip_tnext = now;
                ip_tlast_present = 0;
                if (pd_wake) { SetEvent(pd_wake); } /* W138: Takt-Thread sofort auf den Interp-Takt */
            }
            took = 1;
        }
    }
    if (!took) { ip_live = 0; }
    ip_rl_drain(0); /* W361: faellige ersetzte Shader/Layouts (vor dem neuen Auftrag) */
    if (ip_sh_rc_watch(1, NULL, NULL, "swap") || ip_dead_n) { ip_dead_purge(); } /* W361: Zaehlerwache der Schatten-Views */
    if (ip_live) { ip_rt_prefetch(); } /* W126 RENDER-THREAD: Historie neu -> naechstes Bild vorbereiten */
    ISW(6); isw_end(); /* DIAG(LOC-IM-LOCK) */
    sb_ctx_leave();
    if (ip_live) { ip_poll(); }
    if (g_cfg.interp_diag >= 1 && st_ticks >= 300u) { sb_ctx_enter(); ip_stat_log(); sb_ctx_leave(); } /* W138: Zaehler auch vom Takt-Thread */
    return took;
}

/* ---------- ISSUE-PHOTO: Aufzeichnung bei Bedarf ohne FPS-INTERP (interp_fps=0) ----------
 * Fotos ueber Fenstergroesse (4K/8K/xN), "nur Aussenwelt", HUD aus und die freie Kamera brauchen die Draw-Liste
 * eines Ticks (sb_interp_photo_render). Mit interp_fps=0 zeichnet sb_interp_swap nichts auf; auf Wunsch des
 * Fotomodus bzw. Schnell-Screenshots (render_photo.c) werden hier ZWEI Ticks aufgezeichnet - derselbe Weg wie
 * mit FPS-INTERP (g_ip_recording, Tick-VB, Schatten), nur ohne Zwischenbilder, ohne Zuordnung und ohne
 * Offscreen-BB: das Spiel zeichnet weiter direkt in den Swapchain-BB, gesichert wird am Swap vor der
 * Endbild-Nachbearbeitung. Der erste Tick (A) liefert nur den Anfangsstand der Ziele fuer den zweiten (B, das
 * Foto) - wie der Vortick mit FPS-INTERP; mit B allein startet die Wiedergabe vom Endstand von B, und Ziele, die
 * das Spiel nicht loescht, sondern ueberblendet, weichen ab (Messung: 4600-9100 px, max 4-7). Ohne Wunsch je
 * Present zwei Vergleiche, keine Aufzeichnung, keine Puffer (Standardpfad bitgleich). Spiel-Thread. */
static volatile LONG ip_r1_want;
static int ip_r1_st;     /* SB_REC1_* (ohne WAIT) */
static int ip_r1_n;      /* fertige Ticks dieser Aufzeichnung (0 = A laeuft, 1 = B laeuft bzw. folgt) */
static int ip_r1_next;   /* A fertig: B beginnt nach dem naechsten Present */
static LONGLONG ip_r1_t0;

void sb_interp_rec1_request(void)
{
    InterlockedExchange(&ip_r1_want, 1);
}

int sb_interp_rec1_state(void)
{
    if (ip_r1_want || ip_r1_next) { return SB_REC1_WAIT; }
    if (ip_r1_st == SB_REC1_RUN && ip_rec < 0) { return SB_REC1_FAIL; } /* zwischendurch sb_interp_reset */
    if (ip_r1_st == SB_REC1_RUN && ip_r1_n == 0) { return SB_REC1_WAIT; } /* erst A; die Pause gilt nach B */
    if (ip_r1_st == SB_REC1_READY && !(ip_nhist >= 1 && g_ipl[ip_hist[ip_nhist - 1]].ready)) { return SB_REC1_NONE; }
    return ip_r1_st;
}

/* Naechste Liste aufnehmen (k = Listen-Index). 1 = laeuft. Unter ctx_lock. */
static int ip_r1_start(int k)
{
    ip_list_reset(&g_ipl[k]);
    ip_rec = (ip_vb_ensure(&g_ipl[k]) && ip_rq_ensure(&g_ipl[k])) ? k : -1;
    g_ip_recording = (ip_rec >= 0);
    ip_r1_st = g_ip_recording ? SB_REC1_RUN : SB_REC1_FAIL;
    return g_ip_recording;
}

/* sb_render_present nach sb_pause_point: der folgende Tick wird aufgezeichnet (A bzw. nach A B). */
void sb_interp_rec1_arm(void)
{
    int mission, ok = 0;
    if (!ip_r1_want && !ip_r1_next) { return; }
    if (ip_r1_next) { /* B direkt nach A */
        ip_r1_next = 0;
        if (ip_rec >= 0 || g_cfg.interp_fps > 0 || sb_render_offscreen()) { ip_r1_st = SB_REC1_FAIL; return; }
        sb_ctx_enter();
        ok = ip_r1_start(1);
        sb_ctx_leave();
        if (!ok) { HLOG_W("PHOTO: Aufzeichnung bei Bedarf: Tick-Puffer fuer B fehlgeschlagen"); }
        return;
    }
    InterlockedExchange(&ip_r1_want, 0);
    ip_r1_st = SB_REC1_FAIL;
    if (g_cfg.interp_fps > 0 || sb_render_offscreen() || ip_rec >= 0) { return; } /* FPS-INTERP zeichnet selbst auf */
    if (!ip_init()) {
        HLOG_W("PHOTO: Aufzeichnung bei Bedarf nicht moeglich (INTERP-Init)");
        return;
    }
    sb_ctx_enter();
    mission = ip_mission();
    if (mission) {
        ip_hist_clear(); /* alte Bedarfs-Listen */
        ip_r1_n = 0;
        ok = ip_r1_start(0);
        ip_r1_t0 = ip_now();
    }
    sb_ctx_leave();
    HLOG_I("PHOTO: Aufzeichnung bei Bedarf (interp_fps=0): %s (f=%u)",
           !mission ? "ausserhalb der Mission - keine" : ok ? "die naechsten 2 Ticks" : "Tick-Puffer fehlgeschlagen",
           g_present_no);
}

/* Swap (sb_render_present_body, direkter BB vor der Endbild-Nachbearbeitung): Tick sichern. */
void sb_interp_rec1_end(void)
{
    ip_list_t *L;
    int li, ok = 0;
    unsigned nops, nverts;
    if (ip_r1_st != SB_REC1_RUN) { return; }
    if (ip_rec < 0 || g_cfg.interp_fps > 0 || sb_render_offscreen()) { ip_r1_st = SB_REC1_FAIL; return; }
    sb_ctx_enter();
    li = ip_rec;
    L = &g_ipl[li];
    ip_rec = -1;
    g_ip_recording = 0;
    L->frame = g_present_no;
    L->serial = ++ip_serial;
    L->tswap = L->tstate = ip_now();
    nops = L->nops;
    nverts = L->nverts;
    if (ip_mission() && !L->overflow && L->nops) {
        ip_list_snap(L, li);
        ok = L->snap_ok;
    }
    if (ok) {
        L->ready = 1;
        ip_hist[ip_r1_n] = li;
        ip_nhist = ip_r1_n + 1;
        ip_r1_n++;
        if (ip_r1_n >= 2) { ip_r1_st = SB_REC1_READY; } else { ip_r1_next = 1; } /* A fertig -> B */
    } else {
        ip_list_reset(L);
        ip_r1_st = SB_REC1_FAIL;
    }
    sb_ctx_leave();
    HLOG_I("PHOTO: Aufzeichnung bei Bedarf %s %s: Tick f=%u ops=%u vertices=%u ziele=%d ueberlauf=%d (%.1f ms)",
           li ? "B" : "A", ok ? "fertig" : "fehlgeschlagen", L->frame, nops, nverts, ok ? L->nrefs : 0, L->overflow,
           ip_ms(ip_r1_t0, ip_now()));
}

/* Nach dem Foto bzw. beim Verlassen des Fotomodus: Listen, Schatten und Tick-Puffer frei (keine Dauerkosten). */
void sb_interp_rec1_free(void)
{
    int i;
    InterlockedExchange(&ip_r1_want, 0);
    ip_r1_next = 0;
    ip_r1_n = 0;
    if (ip_r1_st == SB_REC1_NONE || !ip_init_done || g_cfg.interp_fps > 0 || sb_render_offscreen()) {
        ip_r1_st = SB_REC1_NONE;
        return;
    }
    sb_ctx_enter();
    sb_interp_reset("Foto: Aufzeichnung bei Bedarf frei");
    for (i = 0; i < IP_LISTS; i++) {
        ip_list_t *L = &g_ipl[i];
        if (L->vb) { ID3D11Buffer_Release(L->vb); L->vb = NULL; }
        if (L->rqxsrv) { ID3D11ShaderResourceView_Release(L->rqxsrv); L->rqxsrv = NULL; }
        if (L->rqx) { ID3D11Buffer_Release(L->rqx); L->rqx = NULL; }
        if (L->rqc) { ID3D11Buffer_Release(L->rqc); L->rqc = NULL; }
        if (L->wb) { ID3D11Buffer_Release(L->wb); L->wb = NULL; }
        free(L->ipw);
        L->ipw = NULL;
        L->vbcap = L->rqxcap = L->rqccap = L->wbcap = L->ipwcap = 0;
        L->nodisc = 0;
    }
    ip_r1_st = SB_REC1_NONE;
    sb_ctx_leave();
}
