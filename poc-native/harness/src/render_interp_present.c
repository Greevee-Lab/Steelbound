/* render_interp_present.c - FPS-INTERP Takt/Praesentation: Zeitbasis, Latenz-Pacing, dyn. Render-Skalierung, Render-Thread, Present-Takt-Thread.
 * W463 RCORE: aus render_interp.c ausgegliedert (reines Verschieben); gemeinsamer Zustand in
 * render_interp_int.h, Modulkarte docs/RENDER_ARCH.md. */
#include "render_interp_int.h"
static int ip_in_poll;        /* Rekursionsschutz */
LONGLONG ip_tnext;     /* naechstes Present (QPC) */
LONGLONG ip_dt;        /* 1/interp_fps (QPC) */
LONGLONG ip_lat;       /* Latenz L (QPC) */
/* W393 LOC-PACING interp_pace_clock: Anzeige-Latenz, die ip_lat (Ziel) nur weich folgt (stetige Bilduhr), und die
 * Faelligkeit, fuer die sie gilt. Schreiben nur ip_poll (Bild gezeigt), lesen ip_pair_at - beide unter ctx_lock. */
static LONGLONG ip_lat_d, ip_lat_due;
int ip_pace_on(void)
{
    return g_cfg.interp_pace_clock >= 2 || (g_cfg.interp_pace_clock == 1 && g_sba_loc);
}
/* Latenz fuer die Faelligkeit due ohne Uebernahme (Vorbestellung und Bild sehen denselben Wert). */
static LONGLONG ip_lat_at(LONGLONG due)
{
    if (!ip_pace_on() || ip_lat_d <= 0) { return ip_lat; }
    if (due <= ip_lat_due) { return ip_lat_d; }
    /* L steigt: Uhr bis 20 % langsamer; L faellt: bis 2 % schneller; > 100 ms (Haenger) sofort */
    return (LONGLONG)pace_lat_step(ip_lat_d, ip_lat, due - ip_lat_due, g_cfg.interp_pace_up > 0 ? g_cfg.interp_pace_up : 200,
                                   g_cfg.interp_pace_dn > 0 ? g_cfg.interp_pace_dn : 20, ip_qpf.QuadPart / 10);
}
/* W393: Halten (naechster Tick fehlt, juengster Stand B steht exakt). Die Bilduhr dehnt sich mit: Latenz so, dass die
 * gezeigte Zeit genau bei B bleibt. Kommt der Tick, laeuft das Bild nahtlos von B weiter (vorher: Sprung um die
 * Haltezeit nach vorn) und holt die Latenz mit hoechstens interp_pace_dn wieder ab. Ueber Ziel + 100 ms (Haenger)
 * nicht weiter dehnen - dann springt die Uhr wie bisher (Snap). */
static unsigned pc_elastic; /* DIAG */
static void ip_lat_hold(LONGLONG due, const ip_list_t *B)
{
    LONGLONG need;
    if (!g_cfg.interp_pace_hold || !ip_pace_on() || ip_lat_d <= 0 || !B) { return; }
    need = due - B->tstate;
    if (pace_lat_hold(ip_lat_at(due), need, ip_lat, ip_qpf.QuadPart / 10) == need) {
        ip_lat_d = need;
        ip_lat_due = due;
        pc_elastic++;
    }
}
/* Nach dem gezeigten Bild: Latenz fuer due uebernehmen. */
static void ip_lat_commit(LONGLONG due)
{
    if (!ip_pace_on()) { ip_lat_d = 0; return; }
    ip_lat_d = ip_lat_d > 0 ? ip_lat_at(due) : ip_lat;
    ip_lat_due = due;
}
unsigned ip_nneed;
static LONGLONG ip_vb_qpc;    /* letzter Feldwechsel im Feld-Poll (QPC) */
double ip_rbar;        /* mittlere Rechenzeit r (QPC-Einheiten) */
LONGLONG ip_tlast_present;
static LONGLONG ip_tlast_present_pre; /* DIAG(W393): QPC direkt vor dem letzten Present */
static volatile LONG pv_presents; /* W393: Zwischenbilder seit dem letzten VBlank (pace_vsync/pace_diag) */
HANDLE pd_wake; /* W138 PERF-FIX: Takt-Thread wecken (Interp-Takt startet) */

unsigned ip_shot_seq, ip_shot_left, ip_shot_next, ip_pairs_total;
unsigned ip_shot_ser; /* Liste, deren Bilder gespeichert werden (0 = keine) */

/* Statistik */
unsigned st_ticks, st_pairs, st_presents, st_lerp, st_static, st_nomatch,
    st_rawchg, st_reject, st_2d, st_cut, st_skip, st_hold, st_late, st_nopair, st_ovf,
    st_verify, st_verify_bad, st_exact, st_rawlerp, st_rt2d, st_geo;
/* POPIN (W91): Instanz-Zuordnung (gleiche Signatur + gleiche Eingangsdaten,
 * mehrere Kandidaten = mehrfach gezeichnetes Modell, z. B. Haeuser) */
unsigned st_imulti, st_iwrong, st_iamb, st_ipass;
unsigned st_ipass_false; /* W273: Fehltreffer der alten 16-Bit-Maske (mit interp_pass_exact verworfen) */
double st_rec_ms, st_rep_ms, st_rep_max, st_pres_ms, st_r_ms, st_r_max, st_lat_ms;
unsigned st_r_n, st_rep_n, st_lat_n;
LONGLONG st_cap_q, st_t0;
double st_gpu_ms, st_gpu_max;
unsigned st_gpu_n;

static void ip_ev_mark(void); /* W125 TICK-PERF */
int ip_in_rec; /* W122 PERF-HUD: ip_poll aus rec_draw/rec_clear (Zeit steckt in cap) */

double ip_ms(LONGLONG a, LONGLONG b)
{
    return (double)(b - a) * 1000.0 / (double)ip_qpf.QuadPart;
}

LONGLONG ip_now(void)
{
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return q.QuadPart;
}

/* Hash ueber die Position (erste 4 Worte) von hoechstens 64 gleichmaessig
 * verteilten Vertices + n. Stichprobe genuegt: "gleich" heisst nur "exakt
 * aus Tick N zeichnen" bzw. "Vertex i auf Vertex i lerpen" - beides bleibt
 * bei einem unentdeckten Unterschied wohldefiniert. */
/* W323 LOC-INTERP: Spieltakt der Zwischenbilder. SB: 30 Hz (unveraendert). LoC-Profil: Missionstakt
 * 20 Hz (FUN_00033050(3) = 3 VBlank-Felder je Bild). Nur fuer K (Testtakt: K Bilder je Tick) und Logs;
 * der Echtzeit-Takt ist zeitbasiert (t aus den Zustandszeiten), braucht also keine Taktannahme. */
int ip_tick_hz(void)
{
    return g_sba_loc ? loc_render_tick_hz() : 30;
}

/* W323: Zeit des letzten Feldwechsels. Im LoC-Profil schreibt der Haupt-Thread (Feld-Poll) und liest der
 * Spiel-Render-Thread (Swap) -> 64 Bit atomar (x86-32). SB: ein Thread, unveraendert. */
LONGLONG ip_vb_get(void)
{
    return g_sba_loc ? InterlockedCompareExchange64(&ip_vb_qpc, 0, 0) : ip_vb_qpc;
}
static ID3D11Query *dy_dj[DY_N], *dy_t0[DY_N], *dy_t1[DY_N];
static int dy_st[DY_N], dy_open = -1, dy_last_dir, dy_fail;
static double dy_acc, dy_last_avg;
static unsigned dy_n, dy_switches;
static LONGLONG dy_win_q, dy_hi_since, dy_lo_since, dy_last_switch, dy_up_block;
static volatile LONG g_rt_dyn_want;
static unsigned dy_up_veto, dy_wait_n; /* W165: Hochschalten per Prognose verworfen / Wechsel wartet auf Saetze */
static int dy_pref_done;               /* W165: Satz eine Stufe tiefer vorab bestellt */

static void dy_poll(void)
{
    int k;
    for (k = 0; k < DY_N; k++) {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        UINT64 a, b;
        if (dy_st[k] != 2) { continue; }
        if (ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)dy_dj[k], &dj, sizeof dj,
                                        D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) { continue; }
        dy_st[k] = 0;
        if (dj.Disjoint || !dj.Frequency) { continue; }
        if (ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)dy_t0[k], &a, sizeof a, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)dy_t1[k], &b, sizeof b, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            b < a) { continue; }
        dy_acc += (double)(b - a) * 1000.0 / (double)dj.Frequency;
        dy_n++;
    }
}

static void dy_begin(void)
{
    int k;
    if (g_cfg.rt_scale_dyn <= 0 || dy_fail || dy_open >= 0 || !rq_in_mission()) { return; }
    dy_poll();
    for (k = 0; k < DY_N && dy_st[k] != 0; k++) {}
    if (k >= DY_N) { return; }
    if (!dy_dj[k]) {
        D3D11_QUERY_DESC qd;
        memset(&qd, 0, sizeof qd);
        qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        if (FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &dy_dj[k]))) { dy_dj[k] = NULL; dy_fail = 1; return; }
        qd.Query = D3D11_QUERY_TIMESTAMP;
        if (FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &dy_t0[k])) ||
            FAILED(ID3D11Device_CreateQuery(g_dev, &qd, &dy_t1[k]))) { dy_fail = 1; return; }
    }
    ID3D11DeviceContext_Begin(g_ctx, (ID3D11Asynchronous *)dy_dj[k]);
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)dy_t0[k]);
    dy_st[k] = 1;
    dy_open = k;
}

static void dy_ctrl(void)
{
    LONGLONG now = ip_now(), f = ip_qpf.QuadPart;
    unsigned cur, base, mn;
    double avg, hi, lo;
    LONG want = 0;
    if (!dy_win_q) { dy_win_q = now; }
    if (now - dy_win_q < f / 4) { return; }
    dy_win_q = now;
    if (!dy_n) { return; }
    avg = dy_acc / (double)dy_n;
    dy_acc = 0.0;
    dy_n = 0;
    dy_last_avg = avg;
    cur = sb_rtt_dyn_factor();
    base = sb_rtt_base_factor();
    mn = (unsigned)(g_cfg.rt_scale_dyn_min > 0 ? g_cfg.rt_scale_dyn_min : 1);
    if (mn > base) { mn = base; }
    hi = (double)g_cfg.rt_scale_dyn_hi / 10.0;
    lo = (double)g_cfg.rt_scale_dyn_lo / 10.0;
    if (avg > hi) { if (!dy_hi_since) { dy_hi_since = now; } } else { dy_hi_since = 0; }
    if (avg < lo) { if (!dy_lo_since) { dy_lo_since = now; } } else { dy_lo_since = 0; }
    if (dy_last_switch && now - dy_last_switch < 2 * f) { return; }
    if (dy_hi_since && now - dy_hi_since >= f && cur > mn) { want = (LONG)cur - 1; }
    else if (dy_lo_since && now - dy_lo_since >= 8 * f && cur < base && now >= dy_up_block) {
        /* W165 rt_scale_dyn_keep: nur hoch, wenn auch die obere Stufe nach Prognose (Pixel ~ n^2,
         * obere Schranke: alles skaliert) unter der Runter-Schwelle bleibt -> kein Pendeln */
        double pr = avg * (double)((cur + 1u) * (cur + 1u)) / (double)(cur * cur);
        if (g_cfg.rt_scale_dyn_keep <= 0 || pr < hi) { want = (LONG)cur + 1; }
        else { dy_up_veto++; }
    }
    if (want) { InterlockedExchange(&g_rt_dyn_want, want); }
}

static void dy_end(void)
{
    if (dy_open < 0) { return; }
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)dy_t1[dy_open]);
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)dy_dj[dy_open]);
    dy_st[dy_open] = 2;
    dy_open = -1;
    dy_ctrl();
}

void sb_rt_dyn_apply(void)
{
    LONG w;
    unsigned cur;
    int r;
    LONGLONG now;
    if (g_cfg.rt_scale_dyn <= 0) { return; }
    if (g_cfg.rt_scale_dyn_keep > 0 && rq_in_mission()) {
        /* W165: Saetze eine Stufe unter dem aktuellen Faktor vorab im Hilfs-Thread anlegen
         * (einmal je Stufe; neue RTs werden beim Wechsel nachbestellt) */
        unsigned c0 = sb_rtt_dyn_factor(), mn0 = (unsigned)(g_cfg.rt_scale_dyn_min > 0 ? g_cfg.rt_scale_dyn_min : 1);
        if (c0 > mn0 && dy_pref_done != (int)c0) {
            sb_ctx_enter();
            sb_rtt_keep_prefetch(c0 - 1u);
            sb_ctx_leave();
            dy_pref_done = (int)c0;
        }
    }
    w = InterlockedExchange(&g_rt_dyn_want, 0);
    if (!w) { return; }
    cur = sb_rtt_dyn_factor();
    if ((unsigned)w == cur) { return; }
    sb_ctx_enter();
    r = sb_rtt_rescale((unsigned)w);
    sb_ctx_leave();
    if (r == -2) { /* W165 keep: Saetze entstehen noch im Hilfs-Thread -> naechster Swap */
        if (dy_wait_n++ == 0) { HLOG_I("RT-DYN: Faktor %u -> %ld wartet auf vorab angelegte RTs (W165 keep)", cur, w); }
        InterlockedCompareExchange(&g_rt_dyn_want, w, 0);
        return;
    }
    if (r < 0) { InterlockedCompareExchange(&g_rt_dyn_want, w, 0); return; } /* RT gebunden: naechster Swap */
    now = ip_now();
    if ((unsigned)w < cur && dy_last_dir > 0 && now - dy_last_switch < 15 * ip_qpf.QuadPart) {
        dy_up_block = now + 60 * ip_qpf.QuadPart; /* gependelt: 60 s nicht hochschalten */
    }
    dy_last_dir = (unsigned)w > cur ? 1 : -1;
    dy_last_switch = now;
    dy_hi_since = dy_lo_since = 0;
    dy_switches++;
    HLOG_I("RT-DYN: Faktor %u -> %ld (%d RTs neu angelegt, GPU %.2f ms je Zwischenbild, Wechsel #%u, f=%u)",
           cur, w, r, dy_last_avg, dy_switches, g_present_no);
    if (g_cfg.rt_scale_dyn_keep > 0) { /* W165: neue Zeile (obige bleibt fuer Skripte gleich) */
        unsigned mk, jb, sw;
        double jms;
        sb_rtt_keep_stat(&mk, &jb, &sw, &jms);
        HLOG_I("RT-DYN keep: getauscht ohne Neuanlage, Saetze vorab=%u (Auftraege %u, letzter %.1f ms im Hilfs-Thread), "
               "gewartet=%u Swaps, hoch verworfen (Prognose)=%u", mk, jb, jms, dy_wait_n, dy_up_veto);
        dy_wait_n = 0;
    }
}
static ip_rtjob_t ip_rtj;
static HANDLE ip_rt_th, ip_rt_ev;
static DWORD ip_rt_tid;
static ip_cx_t ip_rt_cx;
int ip_rt_ok, ip_rt_tried;
unsigned st_rt_hit, st_rt_miss, st_rt_wait, st_rt_stale, st_rt_n, st_rt_fail, st_rt_exec;
double st_rt_ms, st_rt_max, st_rt_wait_ms, st_rt_exec_ms;
static ip_cx_t ip_cx_imm_v;
static ip_rc_t ip_rc_got; /* DIAG(PERF-M2,W174): Zaehler des zuletzt abgeholten Auftrags */
ip_list_t *ip_rt_nA, *ip_rt_nB; /* Testtakt: Folgebild (A, B, t) fuer ip_rt_get */
float ip_rt_nt;

const ip_cx_t *ip_cx_imm(void)
{
    ip_cx_imm_v.c = g_ctx;
    ip_cx_imm_v.c1 = g_rq_ctx1;
    ip_cx_imm_v.cb0 = ip_cb0;
    ip_cx_imm_v.cb1 = ip_cb1;
    ip_cx_imm_v.deferred = 0;
    return &ip_cx_imm_v;
}

static DWORD WINAPI ip_rt_main(LPVOID p)
{
    (void)p;
    for (;;) {
        WaitForSingleObject(ip_rt_ev, INFINITE);
        if (ip_rtj.st == IP_RT_REQ) {
            ip_rtjob_t *j = &ip_rtj;
            ID3D11CommandList *cl = NULL;
            j->q0 = ip_now();
            if (ip_sh_rc_watch(0, j->ai >= 0 ? &g_ipl[j->ai] : NULL, &g_ipl[j->bi], "rt")) { /* W361: tote View -> nicht binden */
                cl = NULL;
            } else {
            ip_replay_ex(&g_ipl[j->bi], j->bi, j->ai >= 0 ? &g_ipl[j->ai] : NULL, j->ai, j->t, 0, &ip_rt_cx);
            j->rc = ip_rc_ctx[1]; /* DIAG(PERF-M2,W174) */
            if (FAILED(ID3D11DeviceContext_FinishCommandList(ip_rt_cx.c, FALSE, &cl))) { cl = NULL; }
            }
            j->cl = cl;
            j->q1 = ip_now();
            InterlockedExchange(&j->st, IP_RT_DONE);
        }
    }
}

static int ip_rt_start(void)
{
    D3D11_BUFFER_DESC bd;
    D3D11_FEATURE_DATA_THREADING th;
    ID3D11DeviceContext *dc = NULL;
    if (ip_rt_tried) { return ip_rt_ok; }
    ip_rt_tried = 1;
    if (!g_dev || !ip_cb0) { return 0; }
    if (FAILED(ID3D11Device_CreateDeferredContext(g_dev, 0, &dc)) || !dc) {
        HLOG_W("INTERP rt: CreateDeferredContext fehlgeschlagen -> Wiedergabe im Spiel-Thread");
        return 0;
    }
    memset(&ip_rt_cx, 0, sizeof ip_rt_cx);
    ip_rt_cx.c = dc;
    if (g_rq_ctx1 && FAILED(ID3D11DeviceContext_QueryInterface(dc, &IID_ID3D11DeviceContext1, (void **)&ip_rt_cx.c1))) {
        ip_rt_cx.c1 = NULL;
        HLOG_W("INTERP rt: Deferred-Context ohne ID3D11DeviceContext1 -> Wiedergabe im Spiel-Thread");
        return 0;
    }
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = 80;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &ip_rt_cx.cb0)) ||
        FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &ip_rt_cx.cb1))) {
        HLOG_W("INTERP rt: Konstantenpuffer fehlgeschlagen -> Wiedergabe im Spiel-Thread");
        return 0;
    }
    ip_rt_cx.deferred = 1;
    (void)sb_ta_sampler(0, 0); /* Cache vorwaermen: der Render-Thread liest ihn nur */
    ip_rt_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!ip_rt_ev) { return 0; }
    ip_rt_th = CreateThread(NULL, 0, ip_rt_main, NULL, 0, &ip_rt_tid);
    if (!ip_rt_th) { HLOG_W("INTERP rt: CreateThread fehlgeschlagen (%lu)", GetLastError()); return 0; }
    if (g_cfg.render_thread_core >= 0 && g_cfg.render_thread_core < 32) {
        SetThreadAffinityMask(ip_rt_th, (DWORD_PTR)1 << g_cfg.render_thread_core);
    }
    memset(&th, 0, sizeof th);
    ID3D11Device_CheckFeatureSupport(g_dev, D3D11_FEATURE_THREADING, &th, sizeof th);
    ip_rt_ok = 1;
    HLOG_I("INTERP rt: Render-Thread T%04lx bereit (Deferred-Context, Kern %d, DriverCommandLists=%d)",
           ip_rt_tid, g_cfg.render_thread_core, th.DriverCommandLists ? 1 : 0);
    return 1;
}

/* Render-Thread fuer diesen Lauf aktiv? Diagnosen, die den Immediate-Context
 * waehrend der Wiedergabe brauchen (perf_gsplit, interp_diag>=3), behalten den
 * alten Pfad. */
int ip_rt_active(void)
{
    if (g_cfg.render_thread <= 0 || g_cfg.interp_diag >= 3 || g_cfg.perf_gsplit > 0) { return 0; }
    return ip_rt_start();
}

static void ip_rt_wait(void)
{
    while (ip_rtj.st == IP_RT_REQ) {
        int k;
        for (k = 0; k < 64 && ip_rtj.st == IP_RT_REQ; k++) { YieldProcessor(); }
        if (ip_rtj.st == IP_RT_REQ) { SwitchToThread(); }
    }
}

/* Fertigen Auftrag abholen (Statistik), Platz freigeben; Command-List an den Aufrufer. */
static ID3D11CommandList *ip_rt_collect(void)
{
    ID3D11CommandList *cl = ip_rtj.cl;
    double ms = ip_ms(ip_rtj.q0, ip_rtj.q1);
    ip_rtj.cl = NULL;
    ip_rc_got = ip_rtj.rc; /* DIAG(PERF-M2,W174) */
    st_rt_n++;
    st_rt_ms += ms;
    if (ms > st_rt_max) { st_rt_max = ms; }
    if (!cl) { st_rt_fail++; }
    InterlockedExchange(&ip_rtj.st, IP_RT_FREE);
    return cl;
}

/* Offenen Auftrag abwarten und verwerfen (vor Reset/Verdraengung). */
void ip_rt_quiesce(void)
{
    ID3D11CommandList *cl;
    if (ip_rtj.st == IP_RT_FREE) { return; }
    ip_rt_wait();
    st_rt_stale++;
    cl = ip_rt_collect();
    if (cl) { ID3D11CommandList_Release(cl); }
}

/* Liste li verlaesst die Historie: offenen Auftrag mit li verwerfen. */
void ip_rt_drop(int li)
{
    if (ip_rtj.st != IP_RT_FREE && (ip_rtj.ai == li || ip_rtj.bi == li)) { ip_rt_quiesce(); }
}

static int ip_rt_match(int ai, int bi, unsigned aser, unsigned bser, float t)
{
    return ip_rtj.ai == ai && ip_rtj.bi == bi && ip_rtj.aser == aser && ip_rtj.bser == bser &&
           memcmp(&ip_rtj.t, &t, sizeof t) == 0;
}

static void ip_rt_post(int ai, int bi, unsigned aser, unsigned bser, float t)
{
    ip_rtj.ai = ai;
    ip_rtj.bi = bi;
    ip_rtj.aser = aser;
    ip_rtj.bser = bser;
    ip_rtj.t = t;
    ip_rtj.cl = NULL;
    InterlockedExchange(&ip_rtj.st, IP_RT_REQ);
    SetEvent(ip_rt_ev);
}

/* Command-List der Wiedergabe (A, B, t): vorbereiteter Auftrag (ggf. abwarten)
 * oder neu ueber den Render-Thread (synchron). NULL = Fehlschlag. */
ID3D11CommandList *ip_rt_get(ip_list_t *A, ip_list_t *B, float t)
{
    int ai = A ? ip_list_index(A) : -1, bi = ip_list_index(B);
    unsigned aser = A ? A->serial : 0u, bser = B->serial;
    LONGLONG q0 = ip_now();
    if (ip_rtj.st != IP_RT_FREE && ip_rt_match(ai, bi, aser, bser, t)) {
        if (ip_rtj.st == IP_RT_REQ) { st_rt_wait++; ip_rt_wait(); }
        st_rt_hit++;
    } else {
        ip_rt_quiesce();
        ip_rt_post(ai, bi, aser, bser, t);
        ip_rt_wait();
        st_rt_miss++;
    }
    st_rt_wait_ms += ip_ms(q0, ip_now());
    {
        ID3D11CommandList *cl = ip_rt_collect();
        if (ip_rt_nA) { /* Testtakt: naechstes Bild sofort vorbestellen (Aufzeichnung waehrend Ausfuehrung/Present) */
            ip_rt_post(ip_list_index(ip_rt_nA), ip_list_index(ip_rt_nB), ip_rt_nA->serial, ip_rt_nB->serial, ip_rt_nt);
            ip_rt_nA = NULL;
        }
        return cl;
    }
}


/* Naechstes faelliges Zwischenbild vorbereiten (nach Bild bzw. Swap). */
void ip_rt_prefetch(void)
{
    ip_list_t *A, *B;
    float t;
    int hold, ai, bi;
    if (!ip_rt_ok || g_cfg.render_thread <= 0 || !ip_live || ip_nhist < 1) { return; }
    ip_pair_at(ip_tnext, &A, &B, &t, &hold);
    if (!A || hold || t >= 1.0f) { return; }
    ai = ip_list_index(A);
    bi = ip_list_index(B);
    if (ip_rtj.st != IP_RT_FREE) {
        if (ip_rt_match(ai, bi, A->serial, B->serial, t)) { return; }
        if (ip_rtj.st == IP_RT_REQ) { return; } /* laeuft noch: nicht abbrechbar */
        ip_rt_quiesce();
    }
    ip_rt_post(ai, bi, A->serial, B->serial, t);
}

/* Command-List im Spiel-Thread ausfuehren (Immediate-Zustand bleibt). */
void ip_rt_exec(ID3D11CommandList *cl)
{
    LONGLONG q0 = ip_now();
    WD_CALL("ip_exec_cl", NULL, ID3D11DeviceContext_ExecuteCommandList(g_ctx, cl, TRUE));
    ID3D11CommandList_Release(cl);
    st_rt_exec++;
    st_rt_exec_ms += ip_ms(q0, ip_now());
}

void ip_show(ip_list_t *A, ip_list_t *B, float t, const char *tag)
{
    int bi = ip_list_index(B), ai = A ? ip_list_index(A) : -1;
    LONGLONG t0 = ip_now(), t1, tph = t0; /* tph: W122 PERF-HUD */
    ID3D11Resource *img;
    double ms;
    int gpm = 0; /* DIAG(GPU-COST,W146) perf_gpass: Messung dieses Bildes */
    if (B->bbref < 0) { return; }
    if ((g_cfg.reproj_opt & 16) && A && t < 1.0f) { t = 0.001f; } /* W295 DIAG: Zwischenbild ~ Tick N-1 (Pruefmass Bewegung) */
    if (g_cfg.vr_enable && sb_vr_preview_active() && sb_vr_preview_show(A, B, t)) {
        /* W134 VR-PROTO: Desktop-Vorschau (beide Augen bzw. ein Auge aus derselben Aufzeichnung) */
        t1 = ip_now();
        ms = ip_ms(t0, t1);
        st_rep_ms += ms; st_rep_n++;
        if (ms > st_rep_max) { st_rep_max = ms; }
        if (t >= 1.0f || !A) { st_exact++; }
        t0 = ip_now();
        if (GetCurrentThreadId() == g_pd_tid) { g_phud_src = 1; } /* W138 perf_trace */
        sb_render_present_raw();
        g_phud_src = 0;
        ip_ev_mark();
        t1 = ip_now();
        st_pres_ms += ip_ms(t0, t1);
        if (ip_tlast_present && t1 - ip_tlast_present > ip_dt * 3 / 2) { st_late++; }
        ip_tlast_present = t1;
        st_presents++;
        (void)bi; (void)ai; (void)tph;
        return;
    }
    if (t >= 1.0f || !A || (g_cfg.vr_enable && sb_vr_window_mirrors())) {
        /* W183 VR-FIX3: zeigt das Fenster den VR-Spiegel, entfaellt die Mono-Wiedergabe (sb_render_blit
         * kopiert dann nicht, sondern zeigt das Augenbild) */
        img = (ID3D11Resource *)g_ish[B->bbref].bak[bi];
        st_exact++;
    } else {
        int gq, gd;
        ID3D11CommandList *cl = ip_rt_active() ? ip_rt_get(A, B, t) : NULL; /* W126 RENDER-THREAD */
        if (!cl && ip_dead_n) { ip_dead_purge(); return; } /* W361: tote Schatten-View - dieses Bild entfaellt */
        if (!cl) { gpm = sb_gp_begin(0); } /* DIAG(GPU-COST,W146) */
        dy_begin(); /* W146 rt_scale_dyn */
        gq = ip_gpu_begin();
        gd = gd_begin(); /* DIAG(PERF-M2,W174) */
        if (cl) { ip_rt_exec(cl); }
        else { ip_replay(B, bi, A, ai, t); }
        if (gd >= 0) { gd_end(gd, cl ? &ip_rc_got : &ip_rc_ctx[0], B->serial, t); }
        if (gq) { ip_gpu_end(); }
        img = (ID3D11Resource *)g_ish[B->bbref].sh;
        t1 = ip_now();
        ms = ip_ms(t0, t1);
        st_rep_ms += ms;
        st_rep_n++;
        if (ms > st_rep_max) { st_rep_max = ms; }
    }
    if (tag && ip_shot_ser && B->serial == ip_shot_ser) {
        char nm[64];
        _snprintf_s(nm, sizeof nm, _TRUNCATE, "ip_%06u_%s", B->frame, tag);
        sb_render_shot_name(nm, img);
        if (g_cfg.interp_diag >= 3 && A && t < 1.0f && ip_dbg_ps) {
            /* DIAG: Klassifikationsbild (Farbe je Zuordnungs-Ergebnis) */
            ip_replay_ex(B, bi, A, ai, t, 1, ip_cx_imm());
            _snprintf_s(nm, sizeof nm, _TRUNCATE, "ipc_%06u_%s", B->frame, tag);
            sb_render_shot_name(nm, img);
            ip_replay(B, bi, A, ai, t); /* gezeigtes Bild wiederherstellen */
        }
    }
    if (((ip_dd_ser && B->serial >= ip_dd_lo && B->serial <= ip_dd_ser) || B->ddsel) && ip_id_ps && A) { ip_id_dump(A, ai, B, bi, t); } /* DIAG(W251, W284) */
    if ((g_cfg.loc_interp_dbg & 12) && g_sba_loc && A && t < 1.0f && sb_pf_active()) { li_sh_dump(B, t); } /* DIAG(W349) */
    sb_render_blit(img);
    if (gpm) { sb_gp_end(); } /* DIAG(GPU-COST,W146) */
    dy_end(); /* W146 rt_scale_dyn */
    sb_pf_note((t >= 1.0f || !A) ? 1.0f : t, B->frame); /* DIAG(POPIN,W91): pf_ring */
    if (g_sba_loc) { li_note(A, B, t); } /* W323 */
    t0 = ip_now();
    ip_tlast_present_pre = t0; /* DIAG(W393) pace_diag */
    InterlockedIncrement(&pv_presents); /* W393: Bilder je VBlank-Intervall */
    if (GetCurrentThreadId() == g_pd_tid) { g_phud_src = 1; } /* W138 perf_trace */
    sb_render_present_raw();
    g_phud_src = 0;
    ip_ev_mark(); /* PERF(TICK-PERF,W125) interp_gpu_q */
    t1 = ip_now();
    st_pres_ms += ip_ms(t0, t1);
    if (ip_tlast_present && t1 - ip_tlast_present > ip_dt * 3 / 2) { st_late++; }
    ip_tlast_present = t1;
    st_presents++;
    if (g_phud_meas && GetCurrentThreadId() != g_pd_tid) { sb_phud_interp_q(ip_now() - tph, ip_in_rec ? 1 : (g_phud_in_wait ? 2 : 0)); } /* W122 PERF-HUD: Interp-CPU (Wiedergabe + Present); W138: nicht aus dem Takt-Thread */
}
static ID3D11Query *ip_ev[IP_EV_N];
static unsigned ip_ev_n;
double ip_r_last_ms; /* Rechenzeit des letzten Ticks (Feldwechsel -> Swap) */
unsigned g_ip_gskip; /* DIAG(TICK-PERF,W125): wegen GPU-Rueckstau verfallene Zwischenbilder */
unsigned g_ip_gtight; /* DIAG(TICK-PERF,W125): Pruefungen mit verschaerfter Grenze */

static void ip_ev_mark(void)
{
    ID3D11Query **q;
    if (g_cfg.interp_gpu_q <= 0) { return; }
    q = &ip_ev[ip_ev_n % IP_EV_N];
    if (!*q) {
        D3D11_QUERY_DESC qd;
        memset(&qd, 0, sizeof qd);
        qd.Query = D3D11_QUERY_EVENT;
        if (FAILED(ID3D11Device_CreateQuery(g_dev, &qd, q))) { *q = NULL; return; }
    }
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)*q);
    ip_ev_n++;
}

static int ip_gpu_behind(void)
{
    unsigned f = (unsigned)g_cfg.interp_gpu_q;
    BOOL done = FALSE;
    HRESULT hr;
    ID3D11Query *q;
    if (g_cfg.interp_gpu_q <= 0) { return 0; }
    /* interp_gpu_tight=T: war der letzte Tick laenger als T ms, gilt Grenze 1
     * (nur Bilder einreihen, wenn die GPU alles Gezeigte fertig hat). */
    {
        /* PERF(PERF-SPIKE,W318) interp_gpu_tight_imm: ohne Render-Thread (render_thread=0, AMD-Weg) gilt die
         * Grenze 1 ab T ms Tick (Standard 30), wenn interp_gpu_tight nicht gesetzt ist - zusammen mit
         * interp_bsets_imm bleibt die GPU-Warteschlange flach und der Takt-Thread blockiert nicht unter
         * ctx_lock (W318_v2 gegen g50r0: Spiel wartet 0,2 statt 2,1 s je 10 s, 30,0 statt 25,9 Ticks/s) */
        int tight = g_cfg.interp_gpu_tight;
        if (tight <= 0 && g_cfg.render_thread <= 0) { tight = g_cfg.interp_gpu_tight_imm; }
        if (f > 1u && tight > 0 && ip_r_last_ms > (double)tight) { f = 1u; g_ip_gtight++; }
    }
    if (ip_ev_n < f) { return 0; }
    if (f > IP_EV_N - 1u) { f = IP_EV_N - 1u; }
    q = ip_ev[(ip_ev_n - f) % IP_EV_N];
    if (!q) { return 0; }
    hr = ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)q, &done, sizeof done,
                                     D3D11_ASYNC_GETDATA_DONOTFLUSH);
    return (hr == S_FALSE) ? 1 : 0;
}

/* Paar fuer die Faelligkeit due: tstate(A) <= due - L < tstate(B) (Historie:
 * aelteste zuerst); hold = juengster Tick exakt halten (naechster fehlt). */
void ip_pair_at(LONGLONG due, ip_list_t **pA, ip_list_t **pB, float *pt, int *phold)
{
    LONGLONG v = due - ip_lat_at(due); /* W393: stetige Bilduhr (aus: ip_lat) */
    ip_list_t *A = NULL, *B;
    float t = 1.0f;
    int i;
    *phold = 0;
    B = &g_ipl[ip_hist[ip_nhist - 1]];
    if (v >= B->tstate) {
        *phold = 1; /* naechster Tick noch nicht da: juengsten exakt halten */
    } else {
        for (i = ip_nhist - 1; i >= 1; i--) {
            ip_list_t *a = &g_ipl[ip_hist[i - 1]], *b = &g_ipl[ip_hist[i]];
            if (v >= a->tstate) {
                A = a;
                B = b;
                t = (float)((double)(v - a->tstate) / (double)(b->tstate - a->tstate));
                break;
            }
        }
        if (!A) { B = &g_ipl[ip_hist[0]]; t = 1.0f; }
    }
    *pA = A;
    *pB = B;
    *pt = t;
}

/* Echtzeit-Takt: faelliges Bild zeigen (Zustand zur Zeit tau - L). */
static int pd_owns_clock(void); /* W138 PERF-FIX */
/* ---------- W393 LOC-PACING pace_vsync: Zwischenbild-Takt an den VBlank koppeln ----------
 * Mit present_interval=1 (VSync an) liefen die Zwischenbilder bisher mit dem Timer (Present-Intervall 0): Bei
 * interp_fps = Monitorfrequenz wandert die Phase Timer<->VBlank langsam, liegt sie am VBlank, fallen zwei Bilder in
 * ein Komponier-Intervall (eins nie gezeigt, eins doppelt). Ein eigener Thread wartet auf den VBlank (DXGI) und
 * schaetzt Periode und Phase; die Faelligkeiten liegen dann auf dem Raster VBlank + Periode/2 (Mitte zwischen zwei
 * VBlanks, weit weg vom Komponier-Zeitpunkt), alle k VBlanks (k = Monitor/interp_fps, nur ganzzahlig +-3 %).
 * Present selbst bleibt Intervall 0 (blockiert nie unter ctx_lock). Ohne Kopplung zaehlt der Thread nur (pace_diag). */
static HANDLE pv_th;
static volatile LONGLONG pv_last, pv_per; /* letzter VBlank, Periode (QPC, geglaettet) */
static volatile LONG pv_n0, pv_n1, pv_n2, pv_snap_n;
static LONGLONG pv_st_d, pv_of_d; /* DIAG: Raster fuer die PACE-Zeile */
/* W468 PERF-PACING frame_pacing=1 (Standard): auch SB koppelt im Fenster/randlos (DWM komponiert im Monitortakt) mit
 * VSync aus an das VBlank-Raster; exklusiv nur mit VSync an (sonst laege der Riss mittig). Ob das Verhaeltnis
 * Monitor/interp_fps ganzzahlig ist, prueft pace_vb_grid (sonst Timer wie bisher). */
static int pv_sb_want(void)
{
    if (g_sba_loc || g_cfg.frame_pacing != 1) { return 0; }
    /* Live-Zustand aus WINMODE (sb_render_exclusive wird nur beim VBlank-Warten aufgefrischt und bliebe nach
     * Exklusiv -> Fenster stehen, solange der Thread ruht) */
    return g_cfg.pace_sim_mhz > 0 || sb_winmode_current() != 2 || g_cfg.present_interval == 1;
}
static int pv_lock_want(void)
{
    if (pv_sb_want()) { return 1; } /* W468 */
    if (g_cfg.pace_vsync == 3) { return g_sba_loc && !sb_render_exclusive(); } /* auch VSync aus, nur im Fenster (DWM) */
    return g_cfg.present_interval == 1 && (g_cfg.pace_vsync == 2 || (g_cfg.pace_vsync == 1 && g_sba_loc));
}
/* Raster der Kopplung (pace_vb_grid), 0 = keine (Periode unbekannt, Thread steht oder nicht ganzzahlig) */
static int pv_lock_grid(LONGLONG *step, LONGLONG *off)
{
    LONGLONG per = InterlockedCompareExchange64(&pv_per, 0, 0);
    if (!pv_lock_want() || per <= 0 || g_cfg.interp_fps <= 0) { return 0; }
    if (ip_now() - InterlockedCompareExchange64(&pv_last, 0, 0) > ip_qpf.QuadPart / 4) { return 0; } /* Thread steht */
    return pace_vb_grid((double)g_cfg.interp_fps, per, ip_qpf.QuadPart, step, off);
}
/* Faelligkeit t auf den naechstgelegenen Rasterpunkt legen. */
static LONGLONG pv_snap(LONGLONG t, LONGLONG step, LONGLONG off)
{
    LONGLONG vb = InterlockedCompareExchange64(&pv_last, 0, 0);
    InterlockedIncrement(&pv_snap_n);
    return (LONGLONG)pace_vb_next(t - step / 2, vb, step, off);
}
/* W468 TEST pace_sim_hz: kuenstlicher VBlank-Takt vb_k = q0 + k*per (QPC), unabhaengig vom echten Monitor. So laesst
 * sich die Kopplung fuer 60/144/165 Hz an einem Rechner pruefen; q0/per stehen im Log (PACESIM) und im pace_log
 * (Kopf), scripts/pace_sim.py rechnet mit demselben Raster. */
static LONGLONG pv_sim_q0;
static double pv_sim_per;
static int pv_sim_wait(void)
{
    static HANDLE tm;
    static LONGLONG k;
    LONGLONG now = ip_now(), tgt;
    if (!pv_sim_q0) {
        pv_sim_per = (double)ip_qpf.QuadPart * 1000.0 / (double)g_cfg.pace_sim_mhz;
        pv_sim_q0 = now;
        k = 0;
        tm = CreateWaitableTimerExW(NULL, NULL, 0x00000002u, TIMER_ALL_ACCESS);
        HLOG_I("PACESIM: kuenstlicher VBlank %.3f Hz q0=%lld per=%.3f qpf=%lld", g_cfg.pace_sim_mhz / 1000.0,
               (long long)pv_sim_q0, pv_sim_per, (long long)ip_qpf.QuadPart);
    }
    k++;
    tgt = pv_sim_q0 + (LONGLONG)((double)k * pv_sim_per);
    if (tgt <= now) { /* verpasst: auf den naechsten Rasterpunkt */
        k = (LONGLONG)((double)(now - pv_sim_q0) / pv_sim_per) + 1;
        tgt = pv_sim_q0 + (LONGLONG)((double)k * pv_sim_per);
    }
    if (tm && tgt - now > ip_qpf.QuadPart / 1000) {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)((double)(tgt - now - ip_qpf.QuadPart / 1000) * 1e7 / (double)ip_qpf.QuadPart);
        if (SetWaitableTimer(tm, &due, 0, NULL, NULL, FALSE)) { WaitForSingleObject(tm, 50); }
    }
    while (ip_now() < tgt) { YieldProcessor(); }
    return 1;
}
static DWORD WINAPI pv_main(LPVOID p)
{
    LONGLONG last = 0;
    (void)p;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    for (;;) {
        LONGLONG q;
        if (!(pv_lock_want() || g_cfg.pace_diag) || !sb_pd_on() || !ip_live) { last = 0; Sleep(50); continue; }
        if (!(g_cfg.pace_sim_mhz > 0 ? pv_sim_wait() : sb_render_vblank_wait())) { last = 0; Sleep(50); continue; }
        q = ip_now();
        if (last) {
            LONGLONG d = q - last, per = pv_per;
            /* 30..400 Hz; Ausreisser (verpasster VBlank) nicht in die Periode */
            if (d > ip_qpf.QuadPart / 400 && d < ip_qpf.QuadPart / 30) {
                if (per <= 0 || d < per * 3 / 2) {
                    per = per > 0 ? per + (d - per) / 16 : d;
                    InterlockedExchange64(&pv_per, per);
                }
            }
            { LONG c = InterlockedExchange(&pv_presents, 0);
              if (c <= 0) { InterlockedIncrement(&pv_n0); } else if (c == 1) { InterlockedIncrement(&pv_n1); } else { InterlockedIncrement(&pv_n2); } }
        } else {
            InterlockedExchange(&pv_presents, 0);
        }
        InterlockedExchange64(&pv_last, q);
        last = q;
    }
}
static float pc_ft[PC_N], pc_dg[PC_N];
static int pc_nft, pc_ndg;
static LONGLONG pc_last_q, pc_last_lat;
static double pc_last_g = -1.0;
unsigned pc_kept, pc_latchg, pc_ph[12], pc_skip0, pc_lockw1, pc_ts_s, pc_ts_snap, pc_ts_big;
static double pc_latchg_max, pc_lockw_max;
static void pc_note(const ip_list_t *A, const ip_list_t *B, float t, LONGLONG due)
{
    double g = (A && t < 1.0f) ? (double)A->serial + (double)t * (double)(B->serial - A->serial) : (double)B->serial;
    LONGLONG q = ip_tlast_present_pre, lat = ip_lat_at(due);
    if (pc_last_q && pc_nft < PC_N) { pc_ft[pc_nft++] = (float)ip_ms(pc_last_q, q); }
    if (pc_last_g >= 0.0 && pc_ndg < PC_N && g >= pc_last_g - 8.0) { pc_dg[pc_ndg++] = (float)(g - pc_last_g); }
    if (pc_last_lat && lat != pc_last_lat) {
        double d = fabs(ip_ms(pc_last_lat, lat));
        if (d > 0.5) { pc_latchg++; }
        if (d > pc_latchg_max) { pc_latchg_max = d; }
    }
    if (B && ip_tick_hz() > 0) { /* Phase der Faelligkeit gegen die Zustandszeit des juengsten Ticks */
        const ip_list_t *N = &g_ipl[ip_hist[ip_nhist - 1]];
        pc_ph[pace_phase_bin(due, N->tstate, ip_qpf.QuadPart / ip_tick_hz(), 12)]++;
    }
    pc_last_q = q;
    pc_last_g = g;
    pc_last_lat = lat;
}

/* ---------- W468 DIAG pace_log: je gezeigtem Zwischenbild QPC vor Present, Faelligkeit, gezeigte Spielzeit ----------
 * Puffer im Takt-Thread (unter ctx_lock gefuellt), alle ~10 s an pace_log.csv neben dem Log angehaengt. Auswertung
 * scripts/pace_sim.py (Bildabstaende, Anzeige bei 60/144/165 Hz bzw. VRR nachgerechnet). Keine Eingaben. */
typedef struct { LONGLONG q, due, cv; unsigned char hold; } pl_ent_t;
static pl_ent_t *pl_buf;
static int pl_n, pl_tot;
static int pl_head;
static void pl_note(const ip_list_t *A, const ip_list_t *B, float t, int hold, LONGLONG due)
{
    pl_ent_t *e;
    if (pl_tot >= g_cfg.pace_log) { return; }
    if (!pl_buf) { pl_buf = (pl_ent_t *)calloc(8192, sizeof *pl_buf); if (!pl_buf) { return; } }
    if (pl_n >= 8192) { return; }
    e = &pl_buf[pl_n++];
    pl_tot++;
    e->q = ip_tlast_present_pre;
    e->due = due;
    e->cv = (A && t < 1.0f) ? A->tstate + (LONGLONG)((double)t * (double)(B->tstate - A->tstate)) : B->tstate;
    e->hold = (unsigned char)(hold ? 1 : 0);
}
static void pl_flush(void)
{
    char path[MAX_PATH];
    const char *lp = hlog_path();
    const char *sl;
    FILE *f = NULL;
    int i;
    if (!pl_n || !lp || !lp[0]) { pl_n = 0; return; }
    sl = strrchr(lp, '\\');
    if (!sl || strrchr(lp, '/') > sl) { sl = strrchr(lp, '/'); }
    _snprintf_s(path, sizeof path, _TRUNCATE, "%.*space_log.csv", sl ? (int)(sl - lp + 1) : 0, lp);
    if (fopen_s(&f, path, pl_head ? "a" : "w") == 0 && f) {
        if (!pl_head) {
            fprintf(f, "# W468 pace_log qpf=%lld interp_fps=%d frame_pacing=%d sim_q0=%lld sim_per=%.3f sim_mhz=%d\n",
                    (long long)ip_qpf.QuadPart, g_cfg.interp_fps, g_cfg.frame_pacing, (long long)pv_sim_q0, pv_sim_per,
                    g_cfg.pace_sim_mhz);
            fprintf(f, "q,due,cv,hold\n");
            pl_head = 1;
        }
        for (i = 0; i < pl_n; i++) {
            fprintf(f, "%lld,%lld,%lld,%u\n", (long long)pl_buf[i].q, (long long)pl_buf[i].due, (long long)pl_buf[i].cv,
                    (unsigned)pl_buf[i].hold);
        }
        fclose(f);
    }
    pl_n = 0;
}

void ip_poll(void)
{
    LONGLONG now;
    ip_list_t *A = NULL, *B = NULL;
    float t = 1.0f;
    int hold;
    char tag[16];
    if (!ip_live || ip_in_poll || ip_nhist < 1) { return; }
    if (pd_owns_clock()) { return; } /* W138 present_decouple: Bilder zeigt nur der Takt-Thread */
    now = ip_now();
    if (now < ip_tnext) { return; }
    ip_in_poll = 1;
    ip_pair_at(ip_tnext, &A, &B, &t, &hold);
    if (hold) { st_hold++; ip_lat_hold(ip_tnext, B); } /* W393: elastische Bilduhr */
    if (g_sba_loc && (hold || !A || t >= 1.0f) && B->serial == li_last_ser && li_last_t >= 1.0f) {
        /* W323 LOC-INTERP: naechster Tick noch nicht da und das juengste Bild steht schon exakt -> nicht erneut
         * praesentieren (keine Wiederhol-Presents im LoC-Profil, W320); das Fenster zeigt es ohnehin weiter. */
        li_kept++;
        pc_kept++; /* DIAG(W393) */
        ip_tnext += ip_dt;
        now = ip_now();
        while (ip_tnext <= now) { ip_tnext += ip_dt; st_skip++; g_ip_skip_tot++; }
        ip_in_poll = 0;
        return;
    }
    _snprintf_s(tag, sizeof tag, _TRUNCATE, "t%03u", (unsigned)(t * 1000.0f + 0.5f));
    sb_ctx_enter();
    if (A && t < 1.0f && ip_gpu_behind()) { /* PERF(TICK-PERF,W125): GPU hinterher -> Zwischenbild verfaellt */
        sb_ctx_leave();
        g_ip_gskip++; st_skip++; g_ip_skip_tot++;
        ip_tnext += ip_dt;
        now = ip_now();
        while (ip_tnext <= now) { ip_tnext += ip_dt; st_skip++; g_ip_skip_tot++; }
        ip_rt_prefetch(); /* W126 RENDER-THREAD */
        ip_in_poll = 0;
        return;
    }
    { unsigned np0 = st_presents;
    ip_show(A, B, t, tag);
    if (st_presents != np0) {
        ip_lat_commit(ip_tnext); /* W393 */
        if (g_cfg.pace_diag) { pc_note(A, B, t, ip_tnext); } /* DIAG(W393) */
        if (g_cfg.pace_log > 0) { pl_note(A, B, t, hold, ip_tnext); } /* DIAG(W468) */
    } }
    sb_ctx_leave();
    ip_tnext += ip_dt;
    now = ip_now();
    while (ip_tnext <= now) { ip_tnext += ip_dt; st_skip++; g_ip_skip_tot++; }
    { LONGLONG st, of; if (pv_lock_grid(&st, &of)) { ip_tnext = pv_snap(ip_tnext, st, of); } } /* W393 pace_vsync */
    ip_rt_prefetch(); /* W126 RENDER-THREAD: naechstes Bild vorbereiten */
    ip_in_poll = 0;
}


/* ---------- PERF(PERF-FIX,W138): Present-Takt-Thread (present_decouple=1) ----------
 * Bisher zeigte nur der Spiel-Thread Bilder: Zwischenbilder aus den Hooks
 * (Feld-Poll, rec_draw/rec_clear, Swap), Menues/Filme/Ladebilder am Present
 * des Spiels. Steckt der Spiel-Thread in Spielcode ohne Hook (Spiellogik
 * ~11 ms jeden 2. Tick, Ladecode 113-120 ms), stand die Ausgabe (W137 §3).
 * Der Takt-Thread (Host-Thread, kein Xbox-Thread, wird nie suspendiert,
 * haelt ausser ctx keine Sperre) wartet mit hochaufloesendem Timer auf die
 * naechste Faelligkeit und zeigt dann unter ctx_lock (Vortritt g_pd_want):
 *   - Interp-Takt aktiv (Mission, Film): das faellige Bild wie bisher ip_poll
 *     (gleiche Paarwahl ip_pair_at, gleiche Wiedergabe, gleicher Blit); der
 *     Spiel-Thread zeigt selbst keine Zwischenbilder mehr.
 *   - sonst: liegt das letzte echte Present laenger als 1/interp_fps + 0,5 ms
 *     zurueck, den Swapchain-BB erneut (Bild unveraendert, nur keine
 *     eingefrorene Ausgabe). Das Spiel-Bild selbst bleibt unberuehrt.
 * Nur Echtzeit (vblank_hz>0) mit Offscreen-BB (interp_fps>0), nicht mit VR.
 * Testtakt (Golden, interp_verify im Testtakt): Thread ohne Wirkung. */
static HANDLE pd_th, pd_timer;
static DWORD pd_tid_l;
static volatile LONG pd_started; /* 0 = nie, 1 = laeuft, 2 = Start fehlgeschlagen */
static LONGLONG pd_deadline;
static unsigned pd_n_ip, pd_n_hold, pd_n_late, pd_n_lockw, pd_n_iter, pd_n_gw, pd_n_gw_ok;
static unsigned pd_n_rbwait, pd_n_rbskip; /* W390: Ruhe (Vorlauf/Neurechnen) abgewartet bzw. Bild ausgelassen */
static double pd_lockw_ms, pd_lockw_max, pd_late_max, pd_wake_err_max, pd_gw_ms;
static LONGLONG pd_stat_q;

extern volatile LONG g_sb_paused; /* sb_pause.c (W188) */
int sb_pd_on(void)
{
    /* W188 GAME-MENU: in der Pause zeigt die Pause-Schleife die Bilder (Takt-Thread ruht) */
    return pd_started == 1 && g_cfg.present_decouple > 0 && g_cfg.vblank_hz > 0 && !g_cfg.vr_enable &&
           g_cfg.interp_fps > 0 && sb_render_offscreen() && !g_sb_paused;
}

static int pd_owns_clock(void)
{
    return pd_started == 1 && GetCurrentThreadId() != g_pd_tid && sb_pd_on();
}
static double pd_hold_ms, pd_hold_max; /* DIAG(PERF-TICK,W168) ctx_wait_diag */
static unsigned pd_hold_n05;
static void pd_stat_log(LONGLONG now)
{
    double secs = pd_stat_q ? ip_ms(pd_stat_q, now) / 1000.0 : 0.0;
    if (pd_stat_q && secs < 10.0) { return; }
    if (g_cfg.pace_log > 0) { pl_flush(); } /* DIAG(W468) */
    if (pd_stat_q) {
        HLOG_I("PRESENT-TAKT: %.1fs zwischenbilder=%u wiederholt=%u spaet(>1ms)=%u max_spaet=%.2fms "
               "sperre_gewartet=%u (%.3fms mittel, max %.2fms) weckfehler_max=%.2fms runden=%u live=%d "
               "gpu_gewartet=%u (danach gezeigt %u, %.2fms mittel)",
               secs, pd_n_ip, pd_n_hold, pd_n_late, pd_late_max, pd_n_lockw,
               pd_n_lockw ? pd_lockw_ms / pd_n_lockw : 0.0, pd_lockw_max, pd_wake_err_max, pd_n_iter, ip_live,
               pd_n_gw, pd_n_gw_ok, pd_n_gw ? pd_gw_ms / pd_n_gw : 0.0);
    }
    if (pd_stat_q && g_cfg.pace_diag) { /* DIAG(W393 LOC-PACING): eigene Zeile, nur Zaehler und Zeiten */
        double soll = 1000.0 / (double)(g_cfg.interp_fps > 0 ? g_cfg.interp_fps : 120);
        pace_ft_t f = pace_ft_sum(pc_ft, pc_nft, soll);
        pace_g_t gg = pace_g_sum(pc_dg, pc_ndg);
        unsigned k, phm = 0, pht = 0, sk = g_ip_skip_tot - pc_skip0;
        for (k = 0; k < 12u; k++) { pht += pc_ph[k]; if (pc_ph[k] > phm) { phm = pc_ph[k]; } }
        HLOG_I("PACE: %.1fs fps=%d bilder=%d abstand p50=%.2f p99=%.2f max=%.2fms ausserhalb1ms=%d (%.2f%%) "
               "ausgelassen=%u (%.1f/min) ruhe_ausgelassen=%u ruhe_gewartet=%u doppelt=%d gehalten=%u g_med=%.4f g_abw99=%.1f%% "
               "spruenge=%d L=%.1fms L_ziel=%.1fms L_wechsel=%u (max %.1fms) sperre>1ms=%u (max %.2fms) phase_max=%.0f%% uhr=%d vorlauf_frei=%d "
               "vblank=%.2fHz gekoppelt=%d vb_bilder0/1/2+=%ld/%ld/%ld tick_glatt=%u (abw>1/8 %u) tick_neu=%u gedehnt=%u",
               secs, g_cfg.interp_fps, f.n + (f.n ? 1 : 0), f.p50, f.p99, f.mx, f.out1, f.n ? 100.0 * f.out1 / f.n : 0.0,
               sk, sk * 60.0 / secs, pd_n_rbskip, pd_n_rbwait, gg.same, pc_kept, gg.med, gg.dev99 * 100.0, gg.jump,
               (double)ip_lat_at(ip_tnext) * 1000.0 / (double)ip_qpf.QuadPart, (double)ip_lat * 1000.0 / (double)ip_qpf.QuadPart,
               pc_latchg, pc_latchg_max, pc_lockw1, pc_lockw_max, pht ? 100.0 * phm / pht : 0.0, ip_pace_on(), g_cfg.loc_pd_spec,
               pv_per > 0 ? (double)ip_qpf.QuadPart / (double)pv_per : 0.0, pv_lock_grid(&pv_st_d, &pv_of_d), pv_n0, pv_n1, pv_n2, pc_ts_s, pc_ts_big, pc_ts_snap, pc_elastic);
        pc_ts_s = pc_ts_big = pc_ts_snap = pc_elastic = 0;
        InterlockedExchange(&pv_n0, 0); InterlockedExchange(&pv_n1, 0); InterlockedExchange(&pv_n2, 0);
        pc_nft = pc_ndg = 0;
        pc_kept = pc_latchg = pc_lockw1 = 0;
        pc_latchg_max = pc_lockw_max = 0.0;
        memset(pc_ph, 0, sizeof pc_ph);
        pc_skip0 = g_ip_skip_tot;
    }
    if (pd_stat_q && g_cfg.imlock_diag) { sb_iml_log(secs); sb_ctx_enter(); isw_log(secs); sb_ctx_leave(); } /* DIAG(LOC-IM-LOCK) */
    if (pd_stat_q && g_sba_loc && (pd_n_rbwait || pd_n_rbskip)) { /* W390: eigene Zeile (PRESENT-TAKT wird geparst) */
        HLOG_I("PRESENT-RUHE: %.1fs Ruhe abgewartet=%u Bild ausgelassen=%u", secs, pd_n_rbwait, pd_n_rbskip);
    }
    if (pd_stat_q && g_cfg.ctx_wait_diag) { /* DIAG(PERF-TICK,W168) */
        extern volatile LONGLONG g_ctxw_q, g_ctxw_tick_max;
        extern volatile LONG g_ctxw_n;
        HLOG_I("CTXWAIT %.1fs: spiel_wartet=%.1fms (n=%ld, max je Tick %.2fms) takt_haelt=%.3fms mittel (max %.2fms, >0.5ms: %u) runden=%u",
               secs, (double)g_ctxw_q * 1000.0 / (double)ip_qpf.QuadPart, (long)g_ctxw_n,
               (double)g_ctxw_tick_max * 1000.0 / (double)ip_qpf.QuadPart,
               pd_n_iter ? pd_hold_ms / pd_n_iter : 0.0, pd_hold_max, pd_hold_n05, pd_n_iter);
        g_ctxw_q = 0; g_ctxw_n = 0; g_ctxw_tick_max = 0;
        pd_hold_ms = pd_hold_max = 0.0; pd_hold_n05 = 0;
    }
    if (pd_stat_q && g_sba_loc && (li_mid || li_exact)) { /* W323 LOC-INTERP */
        HLOG_I("LOCINTERP stat: %.1fs takt=%dHz interp_fps=%d ticks=%u bilder=%u zwischen=%u exakt=%u wiederholt=%u "
               "gehalten=%u L=%.1fms r=%.1fms zustand_abw>2ms=%u (max %.1fms) kopien=%ld/%ld/%ld rawchg_matrix=%u rawchg_vertex=%u reject_geo=%u",
               secs, ip_tick_hz(), g_cfg.interp_fps, li_ticks, li_mid + li_exact, li_mid, li_exact, li_same, li_kept,
               (double)ip_lat * 1000.0 / (double)ip_qpf.QuadPart, ip_rbar * 1000.0 / (double)ip_qpf.QuadPart,
               li_dev2, li_dev_max, li_cp_rec, li_cp_ok, li_cp_rep, li_rz_alt, li_rz_vl, li_rj_cam2);
        li_cp_rec = li_cp_ok = li_cp_rep = 0;
        li_rz_alt = li_rz_vl = li_rj_cam2 = 0;
        if (g_cfg.loc_wlag_fix) { /* W412 LOC-WORLD-LAG: eigene Zeile (LOCINTERP stat bleibt unveraendert) */
            HLOG_I("LOCINTERP wlag: %.1fs Himmel mit vorhergesagter Kamera: Listen=%ld Draws=%ld", secs,
                   (long)InterlockedExchange(&wl_fix_lists, 0), (long)InterlockedExchange(&wl_fix_draws, 0));
        }
        if (g_cfg.loc_sky_fix || (g_cfg.loc_wlag_diag & 4)) { /* W435 LOC-SKY-LAG-REG: eigene Zeile (LOCINTERP stat bleibt unveraendert) */
            HLOG_I("LOCINTERP sky435: %.1fs Himmel mit Sicht-Korrektur: Listen=%ld Draws=%ld | Himmel unplausibel->Lerp=%ld", secs,
                   (long)InterlockedExchange(&sk_lists, 0), (long)InterlockedExchange(&sk_draws, 0),
                   (long)InterlockedExchange(&sk_rejlerp, 0));
            if (g_cfg.loc_wlag_diag & 4) { /* DIAG(W435) */
                long rep = InterlockedExchange(&sk_rep, 0), ron = InterlockedExchange(&sk_rep_on, 0);
                long rsk = InterlockedExchange(&sk_rep_sky, 0), rskc = InterlockedExchange(&sk_rep_skyc, 0);
                long rna = InterlockedExchange(&sk_rep_na, 0), kl = InterlockedExchange(&sk_klists, 0);
                long kls = InterlockedExchange(&sk_klists_sky, 0), klh = InterlockedExchange(&sk_klists_has, 0);
                HLOG_I("LOCWLAG sicht: %.1fs Wiedergaben=%ld mit Sicht-Korrektur=%ld (Winkel Mittel %.3f max %.3f Grad) | darin "
                       "Himmel-Draws=%ld korrigiert=%ld (%.1f %%) sonstige nicht-affin=%ld | Listen mit K=%ld Himmel mit K=%ld Himmel erkannt=%ld",
                       secs, rep, ron, ron ? sk_ang_sum / (double)ron * 57.29577951 : 0.0, sk_ang_max * 57.29577951, rsk, rskc,
                       rsk ? 100.0 * (double)rskc / (double)rsk : 0.0, rna, kl, kls, klh);
                sk_ang_sum = sk_ang_max = 0.0;
            }
        }
        if (g_cfg.loc_ret_fix) { /* W439 LOC-RETICLE-JUMP: eigene Zeile (LOCINTERP stat bleibt unveraendert) */
            HLOG_I("LOCINTERP ret439: %.1fs Fadenkreuz-Draws exakt aus Tick N, Verschiebung gegen B=%ld | Kandidaten "
                   "verworfen (Bewegung)=%ld", secs, (long)InterlockedExchange(&lr_retx, 0), (long)InterlockedExchange(&lr_rjmv, 0));
        }
        if (g_cfg.loc_ret_mark) { /* W444 LOC-RETICLE-MARK: eigene Zeile */
            HLOG_I("LOCINTERP retmark444: %.1fs Listen mit Fadenkreuz=%ld | Teile ueber Form=%ld Partner=%ld Start-Pruefung=%ld | "
                   "fremd verworfen=%ld | Vorlage um Gedaechtnis ergaenzt=%ld", secs, (long)InterlockedExchange(&lrm_st_lists, 0),
                   (long)InterlockedExchange(&lrm_st_form, 0), (long)InterlockedExchange(&lrm_st_pair, 0),
                   (long)InterlockedExchange(&lrm_st_seed, 0), (long)InterlockedExchange(&lrm_st_fremd, 0),
                   (long)InterlockedExchange(&lrm_st_mem, 0));
        }
        if (g_cfg.loc_ret_diag & 2) { /* DIAG(W439 LOC-RETICLE-JUMP): eigene Zeile */
            HLOG_I("LOCRET: %.1fs Wiedergaben=%ld mit Fadenkreuz=%ld verloren=%ld exakt=%ld ohne Vorhersage=%ld | Spruenge=%ld (gesamt %ld) "
                   "Abweichung max %.4f NDC", secs, (long)InterlockedExchange(&lr_rep, 0), (long)InterlockedExchange(&lr_ret, 0),
                   (long)InterlockedExchange(&lr_lost, 0), (long)InterlockedExchange(&lr_exact, 0), (long)InterlockedExchange(&lr_nola, 0),
                   (long)InterlockedExchange(&lr_jumps, 0), (long)lr_jumps_all, lr_err_max);
            lr_err_max = 0.0;
        }
        if (g_cfg.loc_stat_fix) { /* W415 LOC-STATIC-LAG: eigene Zeile (LOCINTERP stat bleibt unveraendert) */
            HLOG_I("LOCINTERP stat415: %.1fs Welt-Kamera ok=%u verworfen=%u keine=%u | Gelaende mit Welt-Kamera=%u Weltkoordinaten ohne Partner mit Welt-Kamera=%u",
                   secs, st_w415_cam, st_w415_rej, st_w415_nocam, st_w415_rzm, st_w415_nm);
            st_w415_cam = st_w415_rej = st_w415_nocam = st_w415_rzm = st_w415_nm = 0;
        }
        if (g_cfg.loc_tree_fix) { /* W506 ISSUE-105: eigene Zeile (LOCINTERP stat bleibt unveraendert) */
            HLOG_I("LOCINTERP baum506: %.1fs kleine Welt-Draws mit Welt-Kamera=%u | exakt -> mit Welt-Kamera=%u falscher Partner -> mit Welt-Kamera=%u Partner passt=%u bewegt=%u",
                   secs, st_w506_n, st_w506_ex, st_w506_part, st_w506_keep, st_w506_mov);
            st_w506_n = st_w506_ex = st_w506_part = st_w506_keep = st_w506_mov = 0;
        }
        if (g_cfg.loc_interp_rawrzb) { /* W410 LOC-GFX-JITTER: eigene Zeile (LOCINTERP stat bleibt unveraendert) */
            HLOG_I("LOCINTERP rawrzb: %.1fs rawlerp->Matrix-Lerp=%u (>=1000 Vertices %u) Gelaende mit Kamera=%u Kamera passt nicht=%u bewegt=%u",
                   secs, st_w410_rzb, st_w410_big, st_w410_rzm, st_w410_cam, st_w410_mov);
            st_w410_rzb = st_w410_big = st_w410_cam = st_w410_mov = st_w410_rzm = 0;
        }
        if (g_cfg.loc_interp_eyeblind) { /* W448 LOC-CHAFF-LOOK: eigene Zeile */
            HLOG_I("LOCINTERP eye448: %.1fs Netze hinter der Kamera exakt statt Matrix-Lerp=%u", secs, st_w448_eye);
            st_w448_eye = 0;
        }
        if (g_cfg.interp_diag >= 1 && li_rj_n) { /* DIAG(W340) */
            HLOG_I("LOCINTERP reject: %u (w-Wechsel %u, Sprung <0.75 %u, <1 %u, <2 %u, >=2 %u NDC; >=1000 Vertices %u), davon Kamera-Lerp %u",
                   li_rj_n, li_rj_w, li_rj_j[0], li_rj_j[1], li_rj_j[2], li_rj_j[3], li_rj_big, li_rj_cam);
            li_rj_n = li_rj_w = li_rj_big = li_rj_cam = 0;
            memset(li_rj_j, 0, sizeof li_rj_j);
        }
        li_mid = li_exact = li_same = li_ticks = li_dev2 = li_kept = 0;
        li_dev_max = 0.0;
        if (g_cfg.loc_local_aim) { /* W376 LOC-LOCAL-AIM: eigene Zeile (LOCINTERP stat bleibt unveraendert) */
            HLOG_I("LOCAIM render: %.1fs Listen mit Vorhersage=%ld (Welt-Draws je Liste %ld, Fadenkreuz-Draws gesamt %ld) "
                   "Wiedergaben gueltig=%ld korrigiert=%ld",
                   secs, li_la_lists, li_la_lists ? li_la_draws / li_la_lists : 0L, li_la_rets, li_la_rep, li_la_on);
            InterlockedExchange(&li_la_lists, 0); InterlockedExchange(&li_la_draws, 0); InterlockedExchange(&li_la_rets, 0);
            InterlockedExchange(&li_la_rep, 0); InterlockedExchange(&li_la_on, 0);
        }
        if (g_cfg.loc_hud_predict || g_cfg.loc_view_perframe) { /* W394 LOC-HUD-VIEW: eigene Zeile, nur Zaehler */
            HLOG_I("LOCHUD: %.1fs Listen Gegner<100m=%ld mit Marken=%ld (Draws %ld, Kandidaten %ld, verworfen Bewegung %ld "
                   "ruhend %ld) Kompass=%ld | Wiedergaben=%ld verschoben=%ld "
                   "Abweichung Marke vorher mittel %.1f max %.1f, Rest mittel %.2f max %.2f px@640 | Kompass %.3f NDC/rad | "
                   "Radar Listen=%ld Richtung %+.0f (n=%d) | Sicht je Bild ohne/Basis/Zusatz/gehalten=%ld/%ld/%ld/%ld",
                   secs, li_hm_near, li_hm_lists, li_hm_draws, li_hm_cand, li_hm_rej_mv, li_hm_rej_st, li_hm_cmp, li_hm_rep, li_hm_on,
                   li_hm_px_n ? li_hm_px_sum / li_hm_px_n : 0.0, li_hm_px_max,
                   li_hm_res_n ? li_hm_res_sum / li_hm_res_n : 0.0, li_hm_res_max, li_cmp_k, li_hm_rad, li_rad_k, li_rad_n,
                   li_vpf_n[0], li_vpf_n[1], li_vpf_n[2], li_vpf_n[3]);
            InterlockedExchange(&li_hm_near, 0); InterlockedExchange(&li_hm_rad, 0);
            InterlockedExchange(&li_hm_cand, 0); InterlockedExchange(&li_hm_rej_mv, 0); InterlockedExchange(&li_hm_rej_st, 0); li_hm_res_n = 0; li_hm_res_sum = 0.0; li_hm_res_max = 0.0;
            InterlockedExchange(&li_hm_lists, 0); InterlockedExchange(&li_hm_draws, 0); InterlockedExchange(&li_hm_cmp, 0);
            InterlockedExchange(&li_hm_rep, 0); InterlockedExchange(&li_hm_on, 0);
            li_hm_px_n = 0; li_hm_px_sum = 0.0; li_hm_px_max = 0.0;
            InterlockedExchange(&li_vpf_n[0], 0); InterlockedExchange(&li_vpf_n[1], 0);
            InterlockedExchange(&li_vpf_n[2], 0); InterlockedExchange(&li_vpf_n[3], 0);
        }
        if (g_cfg.loc_local_aim) { /* W505 ISSUE-106: eigene Zeile (LOCHUD bleibt unveraendert), nur Zaehler */
            unsigned sm[3];
            double rel;
            extern volatile LONG g_lvpf_repeat_n;
            li_vpf_smooth_take(sm, &rel);
            if (sm[0]) {
                HLOG_I("LOCVPF-GLATT: %.1fs Bilder in Drehung=%u stehend=%u (%.1f %%) Sprung=%u (%.1f %%) Schritt/Soll mittel=%.2f "
                       "| Vorschau mit Eingabeprofil (letzter Satz)=%ld Schalter=%d", secs, sm[0], sm[1], 100.0 * sm[1] / sm[0], sm[2],
                       100.0 * sm[2] / sm[0], rel, InterlockedExchange(&g_lvpf_repeat_n, 0), g_cfg.loc_vpf_repeat);
            }
        }
    }
    {   /* W361: sollte nie vorkommen (ip_list_refs_check, ip_sh_rc_watch) */
        static LONG rc_last;
        if (pd_stat_q && (st_sh_stale || st_rc_bad != rc_last)) {
            HLOG_W("INTERP W361 stat: %.1fs %ld Wiedergaben mit ersetztem Schatten-Eintrag (gesamt %ld), Zaehler-Abweichungen "
                   "gesamt %ld, davon gehalten %ld", secs, st_sh_stale, st_sh_stale_tot, st_rc_bad, st_rc_fix);
            InterlockedExchange(&st_sh_stale, 0);
            rc_last = st_rc_bad;
        }
    }
    pd_stat_q = now;
    pd_n_ip = pd_n_hold = pd_n_late = pd_n_lockw = pd_n_iter = pd_n_gw = pd_n_gw_ok = 0;
    pd_n_rbwait = pd_n_rbskip = 0;
    pd_lockw_ms = pd_lockw_max = pd_late_max = pd_wake_err_max = pd_gw_ms = 0.0;
}

/* Waere das faellige Bild wegen GPU-Rueckstau (interp_gpu_q) verfallen? (unter ctx_lock) */
static int pd_gpu_would_skip(void)
{
    ip_list_t *A, *B;
    float t;
    int hold;
    if (!ip_live || ip_nhist < 1) { return 0; }
    ip_pair_at(ip_tnext, &A, &B, &t, &hold);
    return A && t < 1.0f && ip_gpu_behind();
}
static void pd_lock(void)
{
    LONGLONG q0 = g_cfg.imlock_diag ? ip_now() : 0;
    void *site = q0 ? sb_iml_holder_site() : NULL;
    InterlockedExchange(&g_pd_want, 1);
    sb_ctx_enter();
    InterlockedExchange(&g_pd_want, 0);
    if (q0) { sb_iml_pd_wait(ip_now() - q0, site ? site : sb_iml_holder_site()); }
}

/* Ein Schritt unter ctx_lock; setzt pd_deadline. */
static void pd_step(void)
{
    LONGLONG now = ip_now();
    LONGLONG dt = ip_qpf.QuadPart / (g_cfg.interp_fps > 0 ? g_cfg.interp_fps : 30);
    if (!sb_pd_on()) { pd_deadline = now + dt; return; }
    if (ip_live && ip_nhist >= 1) {
        if (now >= ip_tnext) {
            unsigned np = st_presents;
            double late;
            /* GPU hinterher (interp_gpu_q): bis 1/2 Bildperiode ohne Sperre auf die GPU
             * warten, statt das Bild sofort verfallen zu lassen (Luecke 2 x 8,3 ms).
             * Der Spiel-Thread laeuft dabei weiter. Danach entscheidet ip_poll wie bisher. */
            if (pd_gpu_would_skip()) {
                int wp = g_cfg.pd_gpu_wait > 0 ? (g_cfg.pd_gpu_wait > 90 ? 90 : g_cfg.pd_gpu_wait) : 50; /* W165 GPU-COST2 */
                LONGLONG lim = ip_tnext + ip_dt * wp / 100, g0 = ip_now();
                pd_n_gw++;
                while (ip_now() < lim) {
                    LONGLONG w0;
                    sb_ctx_leave();
                    w0 = ip_now();
                    while (ip_now() - w0 < ip_qpf.QuadPart / 4000) { YieldProcessor(); } /* 0,25 ms */
                    /* PERF(PERF-TICK,W168) pd_gw_nopri=1: im GPU-Warten die Sperre ohne Vortritt nehmen - der
                     * Spiel-Thread musste sonst alle 0,25 ms weichen (CTXWAIT: bis 5800 Wartevorgaenge je 10 s) */
                    if (g_cfg.pd_gw_nopri) {
                        /* frei werdende Sperre abwarten (bis 0,5 ms), dann ohne Vortritt nehmen; sonst wie bisher */
                        const hlock_t *hl = sb_ctx_hlock();
                        LONGLONG s0 = ip_now();
                        int got = 0;
                        while (ip_now() - s0 < ip_qpf.QuadPart / 2000) {
                            if (!hlock_busy(hl)) { sb_ctx_enter(); got = 1; break; }
                            YieldProcessor();
                        }
                        if (!got) { pd_lock(); }
                    } else { pd_lock(); }
                    if (!pd_gpu_would_skip()) { pd_n_gw_ok++; break; }
                }
                pd_gw_ms += ip_ms(g0, ip_now());
                now = ip_now();
            }
            late = ip_ms(ip_tnext, now);
            ip_poll();
            if (st_presents != np) {
                pd_n_ip++;
                if (late > 1.0) { pd_n_late++; }
                if (late > pd_late_max) { pd_late_max = late; }
            }
        }
        pd_deadline = ip_tnext;
        return;
    }
    if (g_sba_loc) { /* W323 LOC-INTERP: keine Wiederhol-Presents im LoC-Profil (W320: Ruckeln); Menue/Lobby und
                      * die ersten Missionsbilder praesentiert das Spiel selbst */
        extern int sb_lm_step(LONGLONG now, LONGLONG *deadline); /* W377 LOC-MENU-FPS: Menues mit der Bildrate */
        if (sb_lm_step(now, &pd_deadline)) { return; }
        pd_deadline = now + dt;
        return;
    }
    {
        LONGLONG hp = dt + ip_qpf.QuadPart / 2000; /* 1/interp_fps + 0,5 ms */
        if (g_pd_last_q && now - g_pd_last_q >= hp) {
            if (g_cfg.warm_gpu_soft & 4) { /* W165: erst zeigen, dann Last -> sie hat die ganze Bildperiode */
                if (sb_render_present_hold()) { pd_n_hold++; }
                sb_warm_gpu_step(ip_now());
            } else {
                sb_warm_gpu_step(now); /* W139 PERF-WARM warm_gpu: GPU-Takt in der Ladephase halten */
                if (sb_render_present_hold()) { pd_n_hold++; }
            }
            pd_deadline = ip_now() + dt;
        } else {
            pd_deadline = g_pd_last_q ? g_pd_last_q + hp : now + dt;
        }
    }
}

/* W330 LOC-ROLLBACK-R1 (nur LoC-Profil): Present-Takt-Thread ruht, solange der Rollback den Gast-Speicher
 * laedt und Ticks neu rechnet (W325 D2: sonst Absturz im Treiber, Render-Thread der Zwischenbilder).
 * on=1 wartet, bis ein laufender Schritt und ein offener Auftrag des Render-Threads fertig sind
 * (hoechstens 200 ms). Rueckgabe 1 = ruhig. */
static volatile LONG pd_rb_pause, pd_rb_busy;
int sb_interp_rb_pause(int on)
{
    LONGLONG t0;
    if (!on) {
        InterlockedExchange(&pd_rb_pause, 0);
        return 1;
    }
    InterlockedExchange(&pd_rb_pause, 1);
    if (!ip_qpf.QuadPart) { QueryPerformanceFrequency(&ip_qpf); }
    t0 = ip_now();
    while (InterlockedCompareExchange(&pd_rb_busy, 0, 0) || InterlockedCompareExchange(&ip_rtj.st, 0, 0) == IP_RT_REQ) {
        if (ip_now() - t0 > ip_qpf.QuadPart / 5) { return 0; }
        SwitchToThread();
    }
    return 1;
}

static DWORD WINAPI pd_main(LPVOID p)
{
    HANDLE hs[2];
    (void)p;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    hs[0] = pd_timer;
    hs[1] = pd_wake;
    pd_deadline = ip_now();
    for (;;) {
        LONGLONG now = ip_now(), early = ip_qpf.QuadPart / 2500; /* 0,4 ms vorher wach, Rest spinnen */
        LONGLONG q0;
        double w;
        if (pd_deadline - now > early) {
            LARGE_INTEGER due; /* relativ, 100-ns-Einheiten */
            due.QuadPart = -(LONGLONG)((double)(pd_deadline - early - now) * 1e7 / (double)ip_qpf.QuadPart);
            if (due.QuadPart > -1) { due.QuadPart = -1; }
            if (SetWaitableTimer(pd_timer, &due, 0, NULL, NULL, FALSE)) {
                WaitForMultipleObjects(2, hs, FALSE, 100);
            } else {
                Sleep(1);
            }
        }
        if (WaitForSingleObject(pd_wake, 0) == WAIT_OBJECT_0) { pd_deadline = ip_now(); }
        while ((now = ip_now()) < pd_deadline) { YieldProcessor(); } /* <= 0,4 ms; SwitchToThread gaebe den Kern ganze Zeitscheiben ab */
        w = ip_ms(pd_deadline, now);
        if (w > pd_wake_err_max) { pd_wake_err_max = w; }
        if (!sb_pd_on()) { /* Testtakt / ohne Offscreen-BB / VR: ruhen, keine Sperre nehmen */
            pd_deadline = now + (ip_dt > 0 ? ip_dt : ip_qpf.QuadPart / 120);
            continue;
        }
        q0 = ip_now();
        if (g_sba_loc) { /* W330 LOC-ROLLBACK-R1: Ruhe beim Laden/Neurechnen (sb_interp_rb_pause) */
            extern int sb_lm_prelock(LONGLONG now, LONGLONG *deadline); /* W377 LOC-MENU-FPS */
            InterlockedExchange(&pd_rb_busy, 1);
            if (pd_rb_pause) {
                LONGLONG w0 = ip_now();
                InterlockedExchange(&pd_rb_busy, 0);
                /* W390 LOC-OWN-PREDICT: kurze Ruhe (Vorlauf je Tick ~1 ms, kurzes Neurechnen) abwarten statt das Bild
                 * auszulassen (sonst bei jedem Vorlauf, der in einen Bildtermin faellt, ein fehlendes Bild). Hoechstens
                 * 4 ms, dann wie bisher auslassen. Gegenseitiger Ausschluss wie oben (busy setzen, Ruhe erneut pruefen). */
                while (pd_rb_pause && ip_now() - w0 < ip_qpf.QuadPart / 250) { SwitchToThread(); }
                if (!pd_rb_pause) {
                    InterlockedExchange(&pd_rb_busy, 1);
                    if (!pd_rb_pause) { pd_n_rbwait++; goto pd_rb_go; }
                    InterlockedExchange(&pd_rb_busy, 0);
                }
                pd_n_rbskip++;
                pd_deadline = now + (ip_dt > 0 ? ip_dt : ip_qpf.QuadPart / 120);
                continue;
            }
        pd_rb_go:
            if (sb_lm_prelock(now, &pd_deadline)) { /* W377: Menue, Spiel arbeitet gerade -> ctx_lock nicht anfassen */
                InterlockedExchange(&pd_rb_busy, 0);
                continue;
            }
        }
        pd_lock();
        w = ip_ms(q0, ip_now());
        if (w > 1.0) { pc_lockw1++; } /* DIAG(W393) */
        if (w > pc_lockw_max) { pc_lockw_max = w; }
        if (w > 0.05) {
            pd_n_lockw++;
            pd_lockw_ms += w;
            if (w > pd_lockw_max) { pd_lockw_max = w; }
        }
        pd_n_iter++;
        q0 = ip_now();
        pd_step();
        sb_ctx_leave();
        if (g_sba_loc) { InterlockedExchange(&pd_rb_busy, 0); } /* W330 */
        if (g_cfg.ctx_wait_diag) { /* DIAG(PERF-TICK,W168): Haltezeit des Takt-Threads */
            double hm = ip_ms(q0, ip_now());
            pd_hold_ms += hm;
            if (hm > pd_hold_max) { pd_hold_max = hm; }
            if (hm > 0.5) { pd_hold_n05++; }
        }
        pd_stat_log(ip_now());
    }
}

void sb_pd_start(void)
{
    if (pd_started || g_cfg.present_decouple <= 0) { return; }
    if (g_cfg.vr_enable) { /* W183 VR-FIX3: im VR-Betrieb ruht der Takt ohnehin (sb_pd_on) -> Thread gar nicht starten */
        if (InterlockedCompareExchange(&pd_started, 2, 0) == 0) {
            HLOG_I("PRESENT-TAKT: aus im VR-Betrieb (Fenster folgt dem Interp-Takt, VR-Thread taktet die Brille)");
        }
        return;
    }
    if (InterlockedCompareExchange(&pd_started, 2, 0) != 0) { return; }
    if (!ip_qpf.QuadPart) { QueryPerformanceFrequency(&ip_qpf); }
    if (ip_dt <= 0) { ip_dt = ip_qpf.QuadPart / (g_cfg.interp_fps > 0 ? g_cfg.interp_fps : 120); }
    /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (0x2, Windows 10 1803+); sonst normal */
    pd_timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002u, TIMER_ALL_ACCESS);
    if (!pd_timer) { pd_timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS); }
    pd_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!pd_timer || !pd_wake) {
        HLOG_W("PRESENT-TAKT: Timer/Event fehlgeschlagen (%lu) -> aus", GetLastError());
        return; /* pd_started bleibt 2: sb_pd_on = 0 */
    }
    pd_th = CreateThread(NULL, 0, pd_main, NULL, CREATE_SUSPENDED, &pd_tid_l);
    if (!pd_th) {
        HLOG_W("PRESENT-TAKT: CreateThread fehlgeschlagen (%lu) -> aus", GetLastError());
        return;
    }
    g_pd_tid = pd_tid_l;
    if (g_cfg.present_core >= 0 && g_cfg.present_core < 32) {
        SetThreadAffinityMask(pd_th, (DWORD_PTR)1 << g_cfg.present_core);
    }
    if ((g_cfg.pace_vsync > 0 && g_sba_loc) || g_cfg.pace_vsync == 2 || g_cfg.pace_diag ||
        (!g_sba_loc && g_cfg.frame_pacing == 1)) { /* W393: VBlank-Thread (ruht ohne Kopplung/Messung); W468: SB mit frame_pacing=1 */
        pv_th = CreateThread(NULL, 0, pv_main, NULL, 0, NULL);
        if (pv_th && g_cfg.present_core >= 0 && g_cfg.present_core < 32) { SetThreadAffinityMask(pv_th, (DWORD_PTR)1 << g_cfg.present_core); }
    }
    HLOG_I("PRESENT-TAKT: Thread T%04lx gestartet (Kern %d, Takt %d Hz, Echtzeit=%d, Offscreen=%d)", pd_tid_l,
           g_cfg.present_core, g_cfg.interp_fps, g_cfg.vblank_hz > 0, sb_render_offscreen());
    InterlockedExchange(&pd_started, 1);
    ResumeThread(pd_th);
}

int sb_interp_pending(void)
{
    return ip_live;
}

void sb_interp_field(unsigned counter)
{
    static unsigned last;
    if (counter != last) {
        last = counter;
        if (g_sba_loc) { /* W323: Leser ist der Spiel-Render-Thread -> 64 Bit atomar. Anker = Feldgrenze selbst
                          * (vblank_qpc: Feld k beginnt bei QPC k*f/hz), nicht der Zeitpunkt, zu dem der Haupt-Thread
                          * sie sieht - der sieht sie unter Last bis ~15 ms spaet (A1/A2, ein Kern je Client). */
            LONGLONG now = ip_now(), vb = now;
            if (g_cfg.vblank_qpc && g_cfg.vblank_hz > 0 && ip_qpf.QuadPart > 0) {
                unsigned long long hz = (unsigned long long)g_cfg.vblank_hz, f = (unsigned long long)ip_qpf.QuadPart;
                unsigned long long k = (unsigned long long)now * hz / f;
                vb = (LONGLONG)(k * f / hz);
                if (vb > now || now - vb > (LONGLONG)(f / hz)) { vb = now; }
            }
            InterlockedExchange64(&ip_vb_qpc, vb);
        }
        else { ip_vb_qpc = ip_now(); } /* Feldwechsel: Spiel-Warten endet hier */
    }
    if (ip_live) {
        if (g_sba_loc) { /* W323 LOC-INTERP: Feld-Poll (Haupt-Thread) und Swap (Spiel-Render-Thread) laufen
                          * nebeneinander -> Paarwahl nur unter ctx_lock. Mit Takt-Thread zeigt nur er die Bilder:
                          * dann gar nicht sperren (sonst sieht der Haupt-Thread den Feldwechsel bis 15 ms spaet, A1) */
            if (pd_owns_clock()) { return; }
            sb_ctx_enter();
            ip_poll();
            sb_ctx_leave();
        } else { ip_poll(); }
    }
}

void sb_interp_idle(unsigned max_ms)
{
    LONGLONG now;
    unsigned ms;
    ip_poll();
    if (!ip_live || pd_owns_clock()) { /* W138: Takt-Thread zeigt die Bilder -> nur warten */
        Sleep(max_ms > 1u ? max_ms - 1u : 0u);
        return;
    }
    now = ip_now();
    ms = (ip_tnext > now) ? (unsigned)((ip_tnext - now) * 1000 / ip_qpf.QuadPart) : 0u;
    if (max_ms > 1u && ms > max_ms - 1u) { ms = max_ms - 1u; }
    Sleep(ms);
}

int ip_mission(void)
{
    unsigned scene = 0, mloop = 0;
    if (g_sba_loc) { return loc_render_interp_mission(); } /* W323 LOC-INTERP: LoC-Missionsbild (Spiel-Render-Thread) */
    __try {
        scene = *(volatile unsigned *)(uintptr_t)M3D_SCENE_ADDR;
        mloop = *(volatile unsigned *)(uintptr_t)M3D_MLOOP_ADDR;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return scene == 0x2Cu && mloop == 1u;
}
