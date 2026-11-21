/* render_locmenu.c - W377 LOC-MENU-FPS: LoC-Menues mit der eingestellten Bildrate (nur LoC-Profil).
 *
 * Die LoC-Menueschleife wartet nach jedem Swap (0x3C18E) auf 2 VBlank-Felder (FUN_00033050(2) @0x3C193,
 * LOC_BOOT §19.2) -> 30 Spielbilder/s. Seit W323 zeigte der Present-Takt-Thread (W138) im LoC-Profil keine
 * Wiederholbilder: W320 hatte gemessen, dass frei laufende Wiederhol-Presents (120/s) den Swap des Spiels um
 * 0-25 ms verschoben (GPU-Warteschlange voll, Present blockiert unter ctx_lock).
 *
 * loc_menu_hz=1: In LoC-Menues (Lockstep aus, [0x306830] == 0; Mission/Einsatzkarte bleiben unberuehrt) zeigt
 * der Takt-Thread das letzte Spielbild erneut (sb_render_present_hold: nur Present des unveraenderten
 * Swapchain-BB, wie SB-Menues W138), mit der Bildrate interp_fps, gedeckelt durch fps_limit ("Menu frame
 * limit"). Spieltakt, Menuelogik und Bildinhalt bleiben unveraendert (kein Zwischenbild, keine Extrapolation).
 * Damit das W320-Problem nicht zurueckkommt:
 *   - phasenrichtig: je Spielbild n gleich verteilte Presents (lm_clock_t, im Mittel genau die Bildrate); nach
 *     dem letzten Wiederholbild bleibt der Platz des naechsten Spiel-Presents frei -> leere Warteschlange;
 *   - nur bei freier GPU: Event-Query nach jedem Present; ist das vorletzte Present noch offen, entfaellt das
 *     Wiederholbild (hoechstens ein Present in Arbeit);
 *   - nur waehrend das Spiel im Feldtakt wartet (Poll von GetDisplayFieldStatus in den letzten 2 ms): solange es
 *     Menuelogik rechnet und zeichnet, nimmt der Takt-Thread ctx_lock gar nicht erst (sb_lm_prelock).
 * Zeile LOCMENU (DIAG loc_menu_diag) zaehlt Wiederholbilder und ausgelassene.
 */
#define COBJMACROS
#include <windows.h>
#include <intrin.h> /* W391: __rdtsc (DIAG LOCTEX) */
#include <d3d11.h>
#include "hcfg.h"
#include "hlog.h"
#include "render_int.h"
#include "sb_profile.h"
#include "render_locmenu.h"

#define LM_ACTIVE_VA 0x306830u /* Lockstep aktiv (LOC_BOOT §19.6, loc_render_interp_mission) */

static LONGLONG lm_qpf;
static lm_clock_t lm_clk;               /* Takt der Wiederholbilder (render_locmenu.h) */
static LONGLONG lm_dt;                   /* Bildperiode (QPC), vom Takt-Thread gesetzt */
static ID3D11Query *lm_ev[2];            /* Events nach den letzten beiden Presents (Ring) */
static int lm_ev_pend[2], lm_evk, lm_ev_fail;
static int lm_on_last = -1;
static unsigned lm_n_rep, lm_n_gpu, lm_n_phase, lm_n_game; /* DIAG(W377) je LOCMENU-Fenster */
static int lm_slot_skip; /* Wiederholbild-Platz schon als ausgelassen gezaehlt */
static volatile LONGLONG lm_poll_q; /* letzter Feld-Poll des Spiels (GetDisplayFieldStatus = Warten im Feldtakt) */
static unsigned lm_n_busy;           /* DIAG: Weckrunden ohne Sperre, weil das Spiel arbeitete */

static LONGLONG lm_now(void)
{
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return q.QuadPart;
}

static void lm_init_qpf(void)
{
    if (!lm_qpf) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        lm_qpf = f.QuadPart ? f.QuadPart : 10000000;
    }
}

/* LoC-Menue? (Lockstep aus). Nur Lesen. */
static int lm_menu(void)
{
    unsigned act = 1;
    __try {
        act = *(volatile const unsigned *)(uintptr_t)LM_ACTIVE_VA;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        act = 1;
    }
    return act == 0u;
}

/* Event nach einem Present setzen (unter ctx_lock). Ring aus zwei Events: ein Present darf noch in Arbeit sein
 * (das Event eines Presents wird beim Blt-Weg erst mit dem naechsten VBlank fertig), das davor muss fertig sein. */
static void lm_ev_end(void)
{
    int k;
    if (!g_dev || !g_ctx || lm_ev_fail) { return; }
    for (k = 0; k < 2; k++) {
        if (!lm_ev[k]) {
            D3D11_QUERY_DESC qd;
            memset(&qd, 0, sizeof qd);
            qd.Query = D3D11_QUERY_EVENT;
            if (FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &lm_ev[k])) || !lm_ev[k]) {
                lm_ev[k] = NULL;
                lm_ev_fail = 1;
                HLOG_W("LOCMENU: Event-Query fehlgeschlagen -> Wiederholbilder ohne GPU-Pruefung");
                return;
            }
        }
    }
    k = lm_evk;
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)lm_ev[k]);
    lm_ev_pend[k] = 1;
    lm_evk = k ^ 1;
}

/* GPU mit dem vorletzten Present fertig? (unter ctx_lock, ohne Warten; Flags 0 reicht das Event an die GPU weiter -
 * mit DONOTFLUSH bliebe es bis zum naechsten Spiel-Present im Puffer) */
static int lm_gpu_idle(void)
{
    BOOL done = FALSE;
    int k = lm_evk; /* naechster Platz = Event von vor zwei Presents */
    if (!lm_ev[k] || !lm_ev_pend[k]) { return 1; }
    if (ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)lm_ev[k], &done, sizeof done, 0) == S_OK && done) {
        lm_ev_pend[k] = 0;
        return 1;
    }
    return 0;
}

static int lm_active(void)
{
    return g_sba_loc && g_cfg.loc_menu_hz > 0;
}

/* Spiel-Present (d3d11_render.c, nach Present, unter ctx_lock wenn der Takt-Thread laeuft). */
void sb_lm_note_game_present(LONGLONG q, int locked)
{
    if (!lm_active()) { return; }
    lm_init_qpf();
    /* 10..125 ms: Menue-/Ladetakt; Laengeres (Laden) zaehlt nicht zur Periode */
    lm_clock_game(&lm_clk, q, lm_dt ? lm_dt : lm_qpf / 120, lm_qpf / 100, lm_qpf / 8);
    lm_n_game++;
    lm_slot_skip = 0;
    if (locked) { lm_ev_end(); }
}

/* Spiel-Thread pollt den Feldtakt (hooks_timing.c, FUN_00033050 wartet) -> es arbeitet gerade nicht. */
void sb_lm_field_poll(void)
{
    if (!lm_active()) { return; }
    InterlockedExchange64(&lm_poll_q, lm_now());
}

/* Takt-Thread vor ctx_lock (render_interp.c pd_main): Wiederholbilder nur, solange das Spiel im Feldtakt wartet.
 * Arbeitet es (Menuelogik + Zeichnen, kein Poll seit 2 ms), bleibt die Sperre unberuehrt -> keine Konkurrenz mit
 * seinem Bild; naechster Blick in 1 ms. 1 = diese Runde auslassen (*deadline gesetzt). */
int sb_lm_prelock(LONGLONG now, LONGLONG *deadline)
{
    LONGLONG pq;
    if (!lm_active() || lm_on_last != 1 || !lm_qpf) { return 0; }
    pq = InterlockedCompareExchange64(&lm_poll_q, 0, 0);
    if (now - pq > lm_qpf / 500) {
        *deadline = now + lm_qpf / 1000;
        lm_n_busy++;
        return 1;
    }
    return 0;
}

/* Takt-Thread (render_interp.c pd_step, unter ctx_lock, ohne Zwischenbild-Takt). 1 = behandelt, *deadline
 * gesetzt; 0 = nicht zustaendig (Mission/Einsatzkarte, aus) -> alter Weg (keine Wiederholbilder). */
int sb_lm_step(LONGLONG now, LONGLONG *deadline)
{
    LONGLONG dt;
    int rate, on;
    if (!lm_active()) { return 0; }
    on = lm_menu();
    if (on != lm_on_last) {
        HLOG_I("LOCMENU: Wiederholbilder %s (Bildrate %d, Menue-Limit %d, f=%u)", on ? "an" : "aus",
               g_cfg.interp_fps, g_cfg.fps_limit, g_present_no);
        lm_on_last = on;
    }
    if (!on) { return 0; }
    lm_init_qpf();
    rate = lm_rate(g_cfg.interp_fps, g_cfg.fps_limit);
    dt = lm_qpf / rate;
    lm_dt = dt;
    switch (lm_clock_decide(&lm_clk, now, dt, lm_gpu_idle(), deadline)) {
    case LM_WAIT:
        return 1;
    case LM_SKIP_PHASE:
        if (!lm_slot_skip) { lm_n_phase++; lm_slot_skip = 1; }
        return 1;
    case LM_SKIP_GPU:
        if (!lm_slot_skip) { lm_n_gpu++; lm_slot_skip = 1; }
        return 1;
    default:
        break;
    }
    lm_slot_skip = 0;
    if (sb_render_present_hold()) {
        lm_n_rep++;
        lm_ev_end();
    }
    lm_clock_done(&lm_clk, now);
    return 1;
}

/* DIAG(W377) loc_menu_diag: Zeile LOCMENU alle 300 Spiel-Presents (d3d11_render.c). */
void sb_lm_diag(unsigned n_present)
{
    extern long long g_lm_rttq, g_lm_upq, g_lm_hashq, g_lm_txq;
    extern unsigned g_lm_rttn, g_lm_upn, g_lm_txskip, g_lm_txupd, g_lm_hashn;
    extern unsigned g_lm_vq_hit, g_lm_vq_miss, g_lm_vq_bad;
    static unsigned last_n;
    if (!g_sba_loc || g_cfg.loc_menu_diag <= 0) { return; }
    if (n_present - last_n < 300u) { return; }
    last_n = n_present;
    lm_init_qpf();
    HLOG_I("LOCMENU f=%u menue=%d spielbilder=%u wiederholt=%u ausgelassen_gpu=%u ausgelassen_phase=%u spiel_arbeitet=%u periode=%.2fms | "
           "tex: rtt=%.2fms (n=%u) upload=%.2fms (n=%u) refresh_hash=%.2fms (n=%u) text_cache=%.2fms gleich=%u ring=%u vq=%u/%u (Abweichung %u)",
           n_present, lm_on_last, lm_n_game, lm_n_rep, lm_n_gpu, lm_n_phase, lm_n_busy,
           lm_clk.per ? (double)lm_clk.per * 1000.0 / (double)lm_qpf : 0.0,
           (double)g_lm_rttq * 1000.0 / (double)lm_qpf, g_lm_rttn, (double)g_lm_upq * 1000.0 / (double)lm_qpf,
           g_lm_upn, (double)g_lm_hashq * 1000.0 / (double)lm_qpf, g_lm_hashn, (double)g_lm_txq * 1000.0 / (double)lm_qpf,
           g_lm_txskip, g_lm_txupd, g_lm_vq_hit, g_lm_vq_miss, g_lm_vq_bad);
    { /* DIAG(W391) LOCTEX: sb_tex_get Wanduhr gegen CPU-Zeit des Threads (TSC), Abbau der Invalidierungsliste, Suche */
        extern long long g_lt_wall, g_lt_cyc, g_lt_drain, g_lt_find;
        extern unsigned g_lt_n;
        static long long tq0, tc0;
        LARGE_INTEGER q;
        long long tc = (long long)__rdtsc();
        double hz = 0.0;
        QueryPerformanceCounter(&q);
        if (tq0 && q.QuadPart > tq0) { hz = (double)(tc - tc0) * (double)lm_qpf / (double)(q.QuadPart - tq0); }
        tq0 = q.QuadPart; tc0 = tc;
        HLOG_I("LOCTEX f=%u gets=%u wand=%.2fms cpu=%.2fms (TSC %.0f MHz) abbau=%.2fms suche=%.2fms", n_present, g_lt_n,
               (double)g_lt_wall * 1000.0 / (double)lm_qpf, hz > 0.0 ? (double)g_lt_cyc * 1000.0 / hz : -1.0, hz / 1e6,
               (double)g_lt_drain * 1000.0 / (double)lm_qpf, (double)g_lt_find * 1000.0 / (double)lm_qpf);
        g_lt_wall = g_lt_cyc = g_lt_drain = g_lt_find = 0;
        g_lt_n = 0;
    }
    lm_n_game = lm_n_rep = lm_n_gpu = lm_n_phase = lm_n_busy = 0;
    g_lm_rttq = g_lm_upq = g_lm_hashq = g_lm_txq = 0;
    g_lm_rttn = g_lm_upn = g_lm_txskip = g_lm_txupd = g_lm_hashn = 0;
    g_lm_vq_hit = g_lm_vq_miss = g_lm_vq_bad = 0;
}
