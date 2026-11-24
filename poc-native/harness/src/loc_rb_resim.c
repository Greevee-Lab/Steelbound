/* loc_rb_resim.c - W464 NET-CORE: aus loc_rollback.c verschoben (W346/W355/W359/W411), unveraendert.
 * Neurechnen/Resync: Gegenprobe rbver, Heap-Freilisten-Pruefung, Abschluss der Wiedergabe, Missions-/Sitzungsende,
 * Abgang eines Mitspielers, Haken Entnahme/Senden (E920/EC80), Rollback ausloesen, Zustandsabzug, Endstatistik. */
#include "loc_rb_int.h"

/* ---- W359 DIAG rbver=N: Neurechnen mit gleichen Eingaben gegen den Live-Stand pruefen ----
 * Jeder N-te erzwungene Rollback (rbtest): vor dem Laden alle Ring-Bereiche kopieren (Live-Stand Tick T), nach dem
 * Neurechnen bis Tick T Wort fuer Wort vergleichen. Gleiche Eingaben -> jede Abweichung ist Zustand, den Laden +
 * Neurechnen nicht originaltreu herstellt. Logs: nur Adressen, Zaehler und Werte des Spielspeichers (keine Eingaben). */
int s_rbv_n, s_rbv_arm, s_rbv_on, s_rbv_div;
static uint8_t *s_rbv_buf;
static size_t s_rbv_cap;
static unsigned s_rbv_gen;
static u32 s_rbv_reb, s_rbv_tick, s_rbv_cnt, s_rbv_bad, s_rbv_skip, s_rbv_logged, s_rbv_pages_sum;
static rbv_h_t s_rbv_h[RBV_H];
static unsigned s_rbv_nh, s_rbv_hfull;
static void rbv_take(void)
{
    if (!s_rbv_arm) return;
    s_rbv_arm = 0;
    if (s_total > s_rbv_cap) {
        if (s_rbv_buf) VirtualFree(s_rbv_buf, 0, MEM_RELEASE);
        s_rbv_cap = s_total + 0x400000u;
        s_rbv_buf = (uint8_t *)VirtualAlloc(NULL, s_rbv_cap, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!s_rbv_buf) { s_rbv_cap = 0; s_rbv_skip++; return; }
    }
    __try {
        rb_copy_out(s_rbv_buf);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        s_rbv_skip++;
        return;
    }
    s_rbv_gen = s_reg_gen;
    s_rbv_reb = s_r_rebuild;
    s_rbv_tick = rd32(RB_TICK);
    s_rbv_on = 1;
}
static void rbv_hadd(u32 page, u32 words, u32 first)
{
    unsigned i;
    for (i = 0; i < s_rbv_nh; i++) {
        if (s_rbv_h[i].page == page) {
            s_rbv_h[i].n++;
            s_rbv_h[i].words += words;
            return;
        }
    }
    if (s_rbv_nh >= RBV_H) { s_rbv_hfull++; return; }
    s_rbv_h[s_rbv_nh].page = page;
    s_rbv_h[s_rbv_nh].n = 1;
    s_rbv_h[s_rbv_nh].words = words;
    s_rbv_h[s_rbv_nh].first = first;
    s_rbv_nh++;
}
void rbv_check(void)
{
    int i;
    u32 np = 0, nw = 0, ex[8][3], nex = 0;
    if (!s_rbv_on) return;
    s_rbv_on = 0;
    if (s_rbv_gen != s_reg_gen || s_rbv_reb != s_r_rebuild || rd32(RB_TICK) != s_rbv_tick) {
        s_rbv_skip++;
        return;
    }
    __try {
        for (i = 0; i < s_nreg; i++) {
            const u32 *live = (const u32 *)(uintptr_t)s_reg[i].va;
            const u32 *ref = (const u32 *)(s_rbv_buf + s_reg[i].off);
            u32 o, w;
            for (o = 0; o < s_reg[i].len / 4u; o += RB_PG / 4u) {
                u32 pw = 0, first = 0;
                if (memcmp(live + o, ref + o, RB_PG) == 0) continue;
                for (w = 0; w < RB_PG / 4u; w++) {
                    if (live[o + w] == ref[o + w]) continue;
                    if (!pw++) first = s_reg[i].va + (o + w) * 4u;
                    if (nex < 8) {
                        ex[nex][0] = s_reg[i].va + (o + w) * 4u;
                        ex[nex][1] = ref[o + w];
                        ex[nex][2] = live[o + w];
                        nex++;
                    }
                }
                np++;
                nw += pw;
                rbv_hadd(s_reg[i].va + o * 4u, pw, first);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        s_rbv_skip++;
        return;
    }
    s_rbv_cnt++;
    s_rbv_pages_sum += np;
    if (!np) return;
    s_rbv_bad++;
    if (s_rbv_logged < 40) {
        char b[512];
        int n = 0;
        u32 k;
        s_rbv_logged++;
        for (k = 0; k < nex && n < (int)sizeof b - 40; k++)
            n += snprintf(b + n, sizeof b - (size_t)n, " 0x%X:%08X->%08X", ex[k][0], ex[k][1], ex[k][2]);
        b[sizeof b - 1] = 0;
        HLOG_W("LOCRBV: Neurechnen != Live bei Tick %u: %u Seiten, %u Woerter |%s", s_rbv_tick, np, nw, b);
    }
}
static int rbv_cmp(const void *a, const void *b)
{
    const rbv_h_t *p = (const rbv_h_t *)a, *q = (const rbv_h_t *)b;
    return p->n < q->n ? 1 : (p->n > q->n ? -1 : (p->page > q->page) - (p->page < q->page));
}
/* W359 DIAG csn=1: Gast-Sperren (RtlEnterCriticalSection), die andere Threads als der Spiel-Thread in R2 betreten
 * (Sperre, Ruecksprung, Thread-Start) - zeigt, welche Gast-Threads gemeinsame Strukturen (Heap) mitbenutzen. */
extern void (*volatile g_loc_cs_note)(u32 cs, u32 ret);
int s_csn;
static struct { u32 cs, ret, tid, n; } s_csn_t[CSN_N];
static volatile LONG s_csn_lock;
static u32 s_csn_full;
void rb_cs_note(u32 cs, u32 ret)
{
    u32 tid = GetCurrentThreadId(), i;
    if (!s_rbi_ep || tid == s_game_tid) return;
    while (InterlockedExchange(&s_csn_lock, 1)) SwitchToThread();
    for (i = 0; i < CSN_N && s_csn_t[i].n; i++)
        if (s_csn_t[i].cs == cs && s_csn_t[i].ret == ret && s_csn_t[i].tid == tid) break;
    if (i < CSN_N) {
        if (!s_csn_t[i].n) { s_csn_t[i].cs = cs; s_csn_t[i].ret = ret; s_csn_t[i].tid = tid; }
        s_csn_t[i].n++;
    } else {
        s_csn_full++;
    }
    InterlockedExchange(&s_csn_lock, 0);
}
static u32 rb_thread_start_of(u32 tid)
{
    int i, n = krnl_xb_thread_count();
    for (i = 0; i < n; i++) {
        void *h = NULL;
        unsigned r = 0;
        if (krnl_xb_thread_get(i, &h, &r) && GetThreadId((HANDLE)h) == tid) return r;
    }
    return 0;
}
static void rb_csn_log(int waited_s)
{
    char b[1400];
    int n = 0;
    u32 i;
    if (!s_csn) return;
    for (i = 0; i < CSN_N && s_csn_t[i].n && n < (int)sizeof b - 64; i++)
        n += snprintf(b + n, sizeof b - (size_t)n, " cs=0x%X ret=0x%X start=0x%X n=%u", s_csn_t[i].cs, s_csn_t[i].ret,
                      rb_thread_start_of(s_csn_t[i].tid), s_csn_t[i].n);
    b[n < (int)sizeof b ? n : (int)sizeof b - 1] = 0;
    HLOG_I("LOCRBV: t=%ds Gast-Sperren anderer Threads (Spiel-Thread ausgenommen)%s:%s", waited_s,
           s_csn_full ? " (Tabelle voll)" : "", n ? b : " keine");
}
void rbv_log(int waited_s)
{
    char b[1400];
    int n = 0;
    unsigned i;
    rb_csn_log(waited_s);
    if (!s_rbv_n) return;
    qsort(s_rbv_h, s_rbv_nh, sizeof s_rbv_h[0], rbv_cmp);
    for (i = 0; i < s_rbv_nh && i < 40 && n < (int)sizeof b - 48; i++)
        n += snprintf(b + n, sizeof b - (size_t)n, " %X:%u/%u@%X", s_rbv_h[i].page, s_rbv_h[i].n, s_rbv_h[i].words,
                       s_rbv_h[i].first);
    b[n < (int)sizeof b ? n : (int)sizeof b - 1] = 0;
    HLOG_I("LOCRBV: t=%ds geprueft=%u abweichend=%u uebersprungen=%u Seiten-Summe=%u verschiedene Seiten=%u%s | Seite:Pruefungen/"
           "Woerter@erstes:%s",
           waited_s, s_rbv_cnt, s_rbv_bad, s_rbv_skip, s_rbv_pages_sum, s_rbv_nh, s_rbv_hfull ? " (Tabelle voll)" : "", b);
}

/* ---- W359 Absicherung: Freilisten des Spiel-Heaps pruefen (heapchk=1, Standard). Nach jeder Neurechnung und je
 * Sekunde live. Beschaedigt -> sauberes Ende (Meldung) statt Absturz/Haenger in der Liste. Heap-Zeiger: 0x306F84
 * (Spiel), 0x73EAB4 (XAPI-Prozess-Heap). Gelesen wird nur innerhalb der gesicherten Bereiche. ---- */
static int rb_heap_rd(void *ctx, u32 va, u32 *out)
{
    int lo = 0, hi = s_nreg - 1, ok = 0;
    u32 k;
    (void)ctx;
    while (lo <= hi && !ok) { /* s_reg aufsteigend (schneller Weg) */
        int m = (lo + hi) / 2;
        if (va < s_reg[m].va) hi = m - 1;
        else if (va - s_reg[m].va + 4u > s_reg[m].len) lo = m + 1;
        else ok = 1;
    }
    for (k = 0; !ok && k < s_pcom_n; k++) /* seit dem letzten Neuaufbau belegt (noch nicht im Ring, z. B. Heap-Wachstum) */
        if (va >= s_pcom[k].a && va + 4u <= s_pcom[k].end) ok = 1;
    if (!ok) { /* sonst: belegter, lesbarer Speicher (Verkettung/Groesse pruefen weiter streng) */
        MEMORY_BASIC_INFORMATION mb;
        if (va < 0x10000u || !VirtualQuery((LPCVOID)(uintptr_t)va, &mb, sizeof mb) || mb.State != MEM_COMMIT ||
            (mb.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !(mb.Protect & 0xEEu))
            return 0;
        s_rbi_hc_vq++;
    }
    __try {
        *out = *(volatile const u32 *)(uintptr_t)va;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return 1;
}
int krnl_cs_try_enter(u32 cs);                     /* krnl_sync.c (W359) */
u32 __stdcall hle_RtlLeaveCriticalSection(u32 cs); /* krnl_sync.c */
void rbi_heapchk(int after_replay)
{
    static const u32 k_hp[2] = {0x306F84u, 0x73EAB4u};
    u32 h[2], i;
    long long t0;
    if (!s_rbi_heapchk || s_rbi_heap_bad || !s_ring || !s_nreg || rbi_free_running(s_rbc)) return;
    t0 = rb_now();
    h[0] = rd32(k_hp[0]);
    h[1] = rd32(k_hp[1]);
    for (i = 0; i < 2; i++) {
        u32 bad = 0, nodes = 0, us;
        int rc;
        u32 lk;
        if (!h[i] || (i && h[1] == h[0])) continue;
        /* Sperre des Heaps (RtlFreeHeap 0x12970C, nur ohne HEAP_NO_SERIALIZE in Heap+0x18): andere Threads nie mitten im
         * Umketten lesen */
        lk = (rd32(h[i] + 0x18u) & 1u) ? 0u : rd32(h[i] + 0x580u);
        if (lk && !krnl_cs_try_enter(lk)) {
            s_rbi_hc_busy++;
            continue;
        }
        rc = rbi_heap_check(rb_heap_rd, NULL, h[i], 20000u, &bad, &nodes);
        if (lk) (void)hle_RtlLeaveCriticalSection(lk);
        s_rbi_hc_n++;
        if (nodes > s_rbi_hc_nodes_max) s_rbi_hc_nodes_max = nodes;
        if (nodes >= 20000u) s_rbi_hc_part++;
        if (rc != RBI_HEAP_OK) {
            s_rbi_hc_bad++;
            s_rbi_heap_bad = 1;
            HLOG_W("LOCRBI: Heap-Pruefung: Spiel-Heap 0x%X beschaedigt (%s, Adresse 0x%X) bei Slot %d, Spieltick %u%s - "
                   "Sitzung wird sauber beendet", h[i], rbi_heap_err(rc), bad, rbi_cons(s_rbc, rbi_self(s_rbc)),
                   rd32(RB_TICK), after_replay ? " (nach Neurechnung)" : "");
        }
        us = rb_us(rb_now() - t0);
        if (us > s_rbi_hc_us_max) s_rbi_hc_us_max = us;
    }
}

void rbi_replay_finish(void)
{
    { /* W346 R3: Cues der ersten Rechnung, die die korrigierte Zeitlinie nicht mehr startet (Zaehler) */
        u32 c;
        for (c = s_xwin0; c + sizeof(rb_ev_t) <= s_xwin1 && s_xwin1 <= s_rec_len;) {
            rb_ev_t *e = (rb_ev_t *)(s_rec + c);
            if ((e->fn == EV_XPLAY || e->fn == EV_XPREP) && !(e->pad & (LRS_PAD_USED | LRS_PAD_DEAD))) {
                u32 ser = e->n >= 8u ? ((const u32 *)(e + 1))[1] : 0u;
                int icue = -1, lp = 0, st = 0, stopped;
                char nm[40] = "";
                s_xc_orphan++;
                if (rbs_on() && ser) st = loc_audio_cue_info(ser, &icue, &lp, NULL, nm, (int)sizeof nm);
                if (st == 2 && lp) s_rbs_rec_loop++;
                /* W375 (#51): Schleife ohne Besitzer (Handle nur im verworfenen Stand) - sonst laeuft sie ewig, auch in
                 * den Menues (Eject-Alarm). Einmal-Cues laufen aus wie bisher. */
                stopped = ser && loc_audio_stop_orphan(ser);
                if (stopped) s_xc_orphan_stop++;
                if (stopped && rbs_act()) { /* W413: tot - eine spaetere Neurechnung startet ihn live statt Handle */
                    e->pad |= LRS_PAD_DEAD;
                    s_rbs_rec_dead++;
                }
                RBS_LOG("LOCSNDLOOP verwaist: Cue %d '%s' Schleife=%s Start=erste Rechnung (vorhergesagt) Fenster=%u..%u -> %s",
                        icue, nm, lp ? "ja" : "nein", s_xwin_g0, s_xwin_g1,
                        stopped ? "gestoppt (W375)" : (st == 0 ? "schon aus" : (lp ? "spielt weiter" : "laeuft aus")));
            }
            e->pad &= (uint8_t)~LRS_PAD_USED;
            c += (u32)sizeof *e + e->n;
        }
        if (rbs_on()) { /* W413: nachgeholte Waisen (Nebentabelle): von einer frueheren Neurechnung live gestartet, von dieser nicht */
            u32 i, w = 0;
            for (i = 0; i < s_xside_n; i++) {
                rb_xside_t *x = &s_xside[i];
                if (lrs_side_orphan(x->g, x->used, s_xwin_g0, s_xwin_g1)) {
                    int icue = -1, lp = 0, st = 0, stopped = 0;
                    char nm[40] = "";
                    s_rbs_side_orphan++;
                    if (x->serial) st = loc_audio_cue_info(x->serial, &icue, &lp, NULL, nm, (int)sizeof nm);
                    if (st == 2 && lp) {
                        s_rbs_side_loop++;
                        if (rbs_act()) stopped = loc_audio_stop_serial(x->serial, 1);
                        if (stopped) s_rbs_side_stop++;
                    }
                    RBS_LOG("LOCSNDLOOP verwaist: Cue %d '%s' Schleife=%s Start=Neurechnen (nachgeholt, Tick %u) Fenster=%u..%u -> %s",
                            icue, nm, lp ? "ja" : "nein", x->g, s_xwin_g0, s_xwin_g1,
                            stopped ? "gestoppt" : (st == 0 ? "schon aus" : (lp ? "spielt weiter (Fix aus)" : "laeuft aus")));
                    if (stopped) continue; /* Eintrag weg: keine Zeitlinie besitzt ihn mehr */
                }
                s_xside[w++] = *x;
            }
            s_xside_n = w;
        }
        s_xwin0 = s_xwin1 = 0;
    }
    if (!s_render || s_rbi_prep) /* kein halber Auftrag fuer den Render-Thread */
        *(volatile u32 *)(uintptr_t)RB_RJOB = rbi_rjob_replay(s_rbi_prep, RBI_RJOB_DONE, rd32(RB_RJOB));
    rbi_heapchk(1); /* W359: Heap nach dem Neurechnen pruefen (Render-Thread steht noch) */
    if (s_freeze) rb_thaw_others();
    rb_rs_resume(); /* W411: nur, wenn gebucht */
    if (s_pdpause) sb_interp_rb_pause(0);
    { /* W372: Feldbezug wieder live (sonst wartet der naechste Tick nicht) */
        u32 f = rd32(RBI_FIELD_LAST);
        s_rbi_fld_n++;
        if (f != s_rbi_fld_live) {
            s_rbi_fld_diff++;
            if (s_rbi_fldfix) *(volatile u32 *)(uintptr_t)RBI_FIELD_LAST = s_rbi_fld_live;
        }
    }
    InterlockedExchange(&s_skipwait, 0);
    s_phase = PH_RECORD;
    rb_put(s_rbi_s_rep, rb_us(rb_now() - s_rbi_t_rep0));
    rbl_note_replay((u32)rb_us(rb_now() - s_rbi_t_rep0)); /* W386 */
}

static void rbi_mission_end(void)
{
    const rbi_stats_t *st = rbi_stats(s_rbc);
    rbl_mission_end(); /* W386 */
    if (s_rbi_ep) {
        if (rbi_replaying(s_rbc) && !s_rep_ended) rbi_replay_finish(); /* W411: schon beim Missionsende im Neurechnen */
        HLOG_I("LOCRBI: Mission %u Ende: Slot %d, Rollbacks %u, verloren %u, Hash verglichen %u abweichend %u",
               s_rbi_missions, rbi_cons(s_rbc, rbi_self(s_rbc)), st->rb_plans, st->rb_lost, st->hash_cmp, st->hash_bad);
        rbv_log(-1); /* W359 DIAG */
        s_rbi_ep = 0;
        loc_sync_ext_on(0);
        s_phase = PH_IDLE;
    }
    rbi_end(s_rbc);
    s_rep_ended = 0; /* W411 */
    s_rbs_net_n = 0; /* W413: Missionsende stoppt die Schleifen der Mission (W375) */
    loc_smooth_reset(); /* W385 */
    s_rbi_mst = RBN_ST_IDLE;
    EnterCriticalSection(&s_rbn_cs);
    rbn_mission_end(s_rbn);
    LeaveCriticalSection(&s_rbn_cs);
}

/* W346: Synchronisation verloren (Hash-Abweichung) bzw. Uneinigkeit: kein Rollback mehr, nie haengen (Freilauf),
 * Verbindung zum Mitspieler wie beim Gastfilter trennen (definierter Weg des Spiels) und den Nutzer informieren. */
void rbi_session_end(int desync)
{
    locnet *n = loc_net_core();
    wchar_t t1[160], t2[240];
    int k, killed = 0;
    if (rbi_free_running(s_rbc)) return;
    rbi_free_run(s_rbc);
    if (desync == 2) s_rbi_end_heap++; /* W359: Heap-Pruefung */
    else if (desync) s_rbi_end_desync++;
    else s_rbi_end_disagree++;
    HLOG_W("LOCRBI: %s bei Slot %d - Rollback aus, Verbindung zum Mitspieler wird getrennt (Meldung an den Nutzer)",
           desync == 2 ? "Spielzustand beschaedigt (Heap-Pruefung)"
                       : (desync ? "Synchronisation verloren (Hash-Abweichung)" : "Netzmodus uneinig"),
           rbi_cons(s_rbc, rbi_self(s_rbc)));
    __try { /* Spiel-TCP-Verbindungen (Tabelle wie LOCGUARD) */
        u32 tab = *(volatile const u32 *)(uintptr_t)0x3116ACu;
        for (k = 0; tab && k < 10; k++) {
            SOCKET sk = (SOCKET) * (volatile const u32 *)(uintptr_t)(tab + (u32)k * 0x10u + 4u);
            if (sk != INVALID_SOCKET && sk != 0 && n) {
                locnet_guard_kill(n, sk, LGUARD_V_CONTENT);
                killed++;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    HLOG_I("LOCRBI: %d Spiel-Verbindungen getrennt", killed);
    if (desync == 2) { /* W359 */
        sb_lang_w("loc.rb_heap_t", t1, 160);
        sb_lang_w("loc.rb_heap_d", t2, 240);
    } else if (desync) {
        sb_lang_w("loc.rb_desync_t", t1, 160);
        sb_lang_w("loc.rb_desync_d", t2, 240);
    } else {
        sb_lang_w("loc.rb_disagree_t", t1, 160);
        sb_lang_w("loc.rb_disagree_d", t2, 240);
    }
    sb_ov_toast_msg(t1, t2, 9000u);
}

static u32 rbi_cmin(void)
{
    u32 c = rbi_commits(s_rbc), d;
    if (!s_ring || !s_ready || s_failed || !rbr_active() || !s_rbi_rec) return c; /* kein Ring/keine Aufzeichnung: keine Vorhersage */
    d = rbr_depth();
    return c > d ? c - d : 0;
}

/* W366 LOC-NET-EDGES: Abgang eines Mitspielers mitten in der Mission (Spiel geschlossen, Absturz, Netz weg). Das Spiel
 * entfernt ihn selbst (Stillezaehler > 200 bzw. Verbindungsreset -> Aktiv-Maske [0x306830] ohne sein Bit) und spielt
 * allein weiter (LOC_RE1 §6.3). Die Eingabeschicht wartete aber weiter auf die Bestaetigung seiner vorhergesagten
 * Slots (Ringtiefe) -> Stau fuer immer (Lauf E2a: Spieltick steht bei 948). Erkennung: sein Bit war in dieser Mission
 * gesetzt, ist jetzt weg, das eigene Bit steht noch, und seit >= 3 s kam kein Paket von ihm (lebende Mitspieler
 * senden SBR2/SBRH laufend) -> nie mehr auf ihn warten; ist keiner mehr da, Freilauf (kein Rollback, kein Stau). */
static u32 s_rbi_act_seen, s_rbi_rxn[RBI_MAXP], s_rbi_rxt[RBI_MAXP];
void rbi_left_reset(void)
{
    int k;
    s_rbi_act_seen = 0;
    for (k = 0; k < RBI_MAXP; k++) {
        s_rbi_rxn[k] = 0;
        s_rbi_rxt[k] = GetTickCount();
    }
}
static void rbi_check_left(void)
{
    /* Spielerzahl [RBI_NPL] nicht verwenden: das Spiel zaehlt sie beim Entfernen sofort herunter (Lauf E2f) */
    u32 act = rd32(RB_ACTIVE), now = GetTickCount(), gone;
    int self = rbi_self(s_rbc), k, left = 0, rest = 0;
    if (self < 0 || self >= RBI_MAXP || !((act >> self) & 1u)) return;
    gone = rbi_gone_mask(s_rbc);
    for (k = 0; k < RBI_MAXP && k < 32; k++) {
        u32 b = 1u << k, n;
        if (k == self) continue;
        n = rbi_rx_count(s_rbc, k);
        if (n != s_rbi_rxn[k]) {
            s_rbi_rxn[k] = n;
            s_rbi_rxt[k] = now;
        }
        if (act & b) s_rbi_act_seen |= b;
        if (!(s_rbi_act_seen & b)) continue;
        if (!(gone & b) && !(act & b) && now - s_rbi_rxt[k] >= 3000u) {
            rbi_peer_gone(s_rbc, k);
            gone |= b;
            left++;
            HLOG_W("LOCRBI: Mitspieler %d hat die Mission verlassen (vom Spiel entfernt, seit %u ms still) bei Slot %d - "
                   "Eingabeschicht wartet nicht mehr auf ihn", k, now - s_rbi_rxt[k], rbi_cons(s_rbc, self));
        }
        if (!(gone & b)) rest++;
    }
    if (left && !rest && !rbi_free_running(s_rbc)) {
        wchar_t t1[160], t2[240];
        rbi_free_run(s_rbc);
        HLOG_W("LOCRBI: kein Mitspieler mehr in der Mission - allein weiter (Freilauf, kein Rollback)");
        sb_lang_w("loc.rb_left_t", t1, 160);
        sb_lang_w("loc.rb_left_d", t2, 240);
        sb_ov_toast_msg(t1, t2, 9000u);
    }
}

/* FUN_0004E920(i, &kopf): eax = Eingabesatz (14 B), *kopf = Rahmenkopf (8 B); 0 = Stau. cdecl, Spiel-Thread. */
static uint8_t s_spec_out[RBI_MAXP][RBI_FR]; /* W390: eigene Puffer (s_rbi_out bleibt der echte Stand) */
u32 s_spec_k[RBI_MAXP];                /* W390: Entnahmen je Spieler im laufenden Vorlauf */
u32 __cdecl rbi_e920(u32 i, u32 pout)
{
    if (s_spec && i < RBI_MAXP) { /* W390: Vorlauf - Rahmen ohne Zustandsaenderung der Eingabeschicht */
        int r = rbi_spec_frame(s_rbc, (int)i, s_spec_k[i], s_spec_own_have ? s_spec_own_new : NULL, s_spec_out[i]);
        if (!r) {
            *(volatile u32 *)(uintptr_t)pout = 0;
            return 0;
        }
        s_spec_k[i]++;
        s_spec_ev.take++;
        if (r == 2) s_spec_ev.take_pred++;
        *(volatile uint16_t *)(uintptr_t)(RBI_TAKENW + i * 2u) += 1;
        *(volatile u32 *)(uintptr_t)pout = (u32)(uintptr_t)s_spec_out[i];
        return (u32)(uintptr_t)(s_spec_out[i] + RBI_HDR);
    }
    if (s_rbi_mst == RBN_ST_TENT || s_rbi_mst == RBN_ST_PEND) { /* W346: vor der Entscheidung keine Entnahme (Stau) */
        *(volatile u32 *)(uintptr_t)pout = 0;
        return 0;
    }
    if (s_rbi_mst == RBN_ST_PRE && !rbi_pre_step()) { /* W475: Vorab-Einigung, Entnahme erst nach dem Beweis */
        *(volatile u32 *)(uintptr_t)pout = 0;
        return 0;
    }
    if (!s_rbi_ep || i >= RBI_MAXP) return o_e920(i, pout);
    if (s_phase != PH_REPLAY) {
        rbi_check_left(); /* W366 */
        if (s_rbi_ts_in || (s_rbi_ts_stall && !s_rbi_tick_takes)) { /* Zeitabgleich: ganzer Tick als Stau */
            if (!s_rbi_ts_in) s_rbi_ts_waits++;
            s_rbi_ts_in = 1;
            s_rbi_ts_stall = 0;
            *(volatile u32 *)(uintptr_t)pout = 0;
            return 0;
        }
        rbi_poll();
    }
    rbi_clock(s_rbc, rbi_us_now()); /* W372: Entnahmezeit */
    if (s_phase != PH_REPLAY && !s_tim_take_seen && i != (u32)rbi_self(s_rbc)) {
        s_tim_take_seen = 1;
        tim_h(s_tim_h_take, rbi_us_now(), s_tim_t0);
    }
    {
        int first = rbi_p0(s_rbc) < 0, ok = rbi_take(s_rbc, (int)i, rbi_cmin(), s_rbi_out[i]);
        if (first && rbi_p0(s_rbc) >= 0 && s_phase != PH_REPLAY) rbi_send(); /* W475: Vorlauf-Rahmen (Slots 0..D-1) sofort
                                                                            * senden, nicht erst am Ruhepunkt */
        if (!ok) {
            *(volatile u32 *)(uintptr_t)pout = 0;
            return 0;
        }
    }
    *(volatile uint16_t *)(uintptr_t)(RBI_TAKENW + i * 2u) += 1; /* wie das Original (Zaehler der Entnahmen) */
    s_rbi_tick_takes++;
    s_rbi_dflags[i == (u32)rbi_self(s_rbc) ? 0 : 1] |= *(const u32 *)(s_rbi_out[i] + 4); /* DIAG: Flags (keine Eingaben) */
    if (i < 2 && s_phase != PH_REPLAY) { /* DIAG: Pruefsumme des Eingabestroms je Spieler (ohne Sync-Byte), keine Eingaben */
        uint8_t t[RBI_FR];
        memcpy(t, s_rbi_out[i], RBI_FR);
        t[1] = 0;
        s_rbi_dstream[i] = fnv(s_rbi_dstream[i] * 31u + s_rbi_dn[i]++, t, RBI_FR);
        if (s_rbi_dn[i] % 25u == 0 && s_rbi_dn[i] <= 250u) {
            s_rbi_dchk[i][s_rbi_dn[i] / 25u - 1u] = s_rbi_dstream[i];
            if (i == 1) s_rbi_dcnt[s_rbi_dn[i] / 25u - 1u] = rd32(0x5C5A40u) | (rd32(RB_TICK) << 16);
        }
    }
    *(volatile u32 *)(uintptr_t)pout = (u32)(uintptr_t)s_rbi_out[i];
    return (u32)(uintptr_t)(s_rbi_out[i] + RBI_HDR);
}

volatile long g_loc_rb_own_ms; /* W420 LOC-CLIPDUMP (loc_rollback.h) */
/* FUN_0004EC80(daten, kopf): eigener Rahmen; im Original TCP an alle + eigener Ring. Rueckgabe 1. */
u32 __cdecl rbi_ec80(u32 data, u32 hdr)
{
    int pre;
    InterlockedExchange(&g_loc_rb_own_ms, (LONG)(GetTickCount() | 1u)); /* W420: eigener Rahmen = Mission laeuft */
    if (s_spec) { /* W390: Vorlauf - erster neu erzeugter eigener Rahmen = der des naechsten Live-Ticks (gleiche Abfrage) */
        s_spec_ev.ec80++;
        if (!s_spec_own_have) {
            memcpy(s_spec_own_new, (const void *)(uintptr_t)hdr, RBI_HDR);
            memcpy(s_spec_own_new + RBI_HDR, (const void *)(uintptr_t)data, RBI_IN);
            s_spec_own_have = 1;
        }
        return o_ec80(data, hdr); /* Spielzustand wie live; das TCP-Senden unterdrueckt rb_send */
    }
    pre = rd32(RBI_PREFILL) == 1u;
    if (pre && !s_rbi_last_pre) { /* Vorlauf von FUN_0001D3D0 = neue Mission */
        if (s_rbi_mst != RBN_ST_IDLE) rbi_mission_end();
        rbi_mission_open(); /* W346: Aushandlung */
    }
    s_rbi_last_pre = pre;
    if (s_rbi_mst == RBN_ST_TENT || s_rbi_mst == RBN_ST_PEND) { /* W346: eigene Rahmen schon mitschreiben */
        rbi_own(s_rbc, (const uint8_t *)(uintptr_t)hdr, (const uint8_t *)(uintptr_t)data);
        return o_ec80(data, hdr);
    }
    if (s_rbi_mst == RBN_ST_PRE) { /* W475: mitschreiben und (nach rbi_prime) gleich senden */
        if (rbi_own(s_rbc, (const uint8_t *)(uintptr_t)hdr, (const uint8_t *)(uintptr_t)data) && rbi_p0(s_rbc) >= 0) rbi_send();
        return o_ec80(data, hdr);
    }
    if (!s_rbi_ep) return o_ec80(data, hdr);
    if (s_rbi_dtick_own++ && s_phase != PH_REPLAY) s_rbi_dmulti++; /* DIAG: mehr als ein eigener Rahmen je Tick */
    if (s_rbi_early && s_phase != PH_REPLAY) { /* W372: Rahmen gleich nach dem Erzeugen senden (statt ~50 ms spaeter) */
        if (rbi_own(s_rbc, (const uint8_t *)(uintptr_t)hdr, (const uint8_t *)(uintptr_t)data)) {
            if (!s_tim_own_seen) { /* W372: Zeitpunkt des eigenen Rahmens im Tick */
                s_tim_own_seen = 1;
                s_tim_own_t = rbi_us_now();
                s_tim_own_pend = 1;
                tim_h(s_tim_h_own, s_tim_own_t, s_tim_t0);
                lat_note_own(); /* W386 */
                if (!s_tim_take_seen) s_tim_own_first++;
            }
            rbi_poll();
            rbi_send();
            s_rbi_sent_early = 1;
        }
        return o_ec80(data, hdr);
    }
    if (s_phase != PH_REPLAY && !s_tim_own_seen) { /* W372: Zeitpunkt des eigenen Rahmens im Tick */
        s_tim_own_seen = 1;
        s_tim_own_t = rbi_us_now();
        s_tim_own_pend = 1;
        tim_h(s_tim_h_own, s_tim_own_t, s_tim_t0);
        lat_note_own(); /* W386 */
        if (!s_tim_take_seen) s_tim_own_first++;
    }
    rbi_own(s_rbc, (const uint8_t *)(uintptr_t)hdr, (const uint8_t *)(uintptr_t)data);
    /* Der 0x34-Rahmen geht weiter wie im Original ueber TCP (und in den eigenen Ring): Das Spiel fuehrt je Mitspieler
     * einen Stille-Zaehler ([0x366498+i*16+8], Reset bei jedem TCP-Empfang in FUN_00053B20, > 200 Ticks = Mitspieler
     * weg in FUN_00050EF0) und tauscht in den Kopfbytes 2/3 Werte je Ziel aus. Der Inhalt wird nicht mehr entnommen
     * (FUN_0004E920 bedient die Eingabeschicht), niemand wartet darauf. Im Neurechnen unterdrueckt rb_send das Senden. */
    return o_ec80(data, hdr);
}

/* 0x1D0BF in FUN_0001D010: mit R2 Regelung ueberspringen (weiter bei 0x1D381, Kopf mit Maske/Flags steht in
 * [esp+0x10]), sonst Lockstep-Zweig loc_lsarb_reg_entry (Originalbefehl "mov ebx,[0x3071A0]" und weiter bei 0x1D0C5
 * bzw. neuer Pufferregler, loc_lsarb_game.c). eax bleibt erhalten. */
__declspec(naked) void rbi_reg_thunk(void)
{
    __asm {
        cmp dword ptr [s_rbi_ep], 0
        je rbi_orig
        mov dword ptr ds:[306944h], 1
        push ecx
        mov ecx, dword ptr [s_rbi_reg_d]
        mov dword ptr ds:[306F04h], ecx
        pop ecx
        push 1D381h
        ret
    rbi_orig:
        jmp loc_lsarb_reg_entry /* LOC-LOCKSTEP-ARBITER: Lockstep-Zweig (Original oder neuer Regler) */
    }
}

void rbi_rec_compact(void)
{
    u32 g = rbi_gtick(s_rbc), keep, base, j, span = RBI_GH / 2u - 1u;
    if (s_rec_len < RB_REC_CAP / 2u) return;
    if (s_rec_len > (RB_REC_CAP / 4u) * 3u) span = 256u;
    keep = g > span ? g - span : 0;
    base = s_rbi_toff[keep & RBI_GM];
    if (base == 0 || base > s_rec_len) return;
    memmove(s_rec, s_rec + base, s_rec_len - base);
    s_rec_len -= base;
    for (j = keep; j <= g; j++) s_rbi_toff[j & RBI_GM] -= base;
    s_rbi_compact++;
}

void rbi_set_rep_cursor(void)
{
    u32 g = rbi_gtick(s_rbc), k;
    s_rec_cur = s_rbi_toff[g & RBI_GM];
    s_rep_end = s_rbi_toff[(g + 1u) & RBI_GM];
    if (s_rep_end < s_rec_cur) s_rep_end = s_rec_cur;
    for (k = 0; k < EV_N; k++) s_rep_kc[k] = s_rec_cur; /* W346 R3 */
}
u32 rb_rbi_gtick(void) { return s_rbc ? rbi_gtick(s_rbc) : 0; }

void rbi_try_rollback(void)
{
    u32 ticks = 0, sb = 0, pages = 0, us = 0;
    int k;
    if (!rbi_rb_pending(s_rbc)) return;
    if (!s_ring || !s_ready || s_failed || !rbr_active()) {
        k = rbi_plan(s_rbc, 0, &ticks, &sb); /* nicht moeglich -> gezaehlt */
        return;
    }
    if (!rb_quiesce(1)) { /* Render-Thread beschaeftigt: naechster Tick */
        s_rbi_qfail++;
        return;
    }
    if (s_pdpause && !sb_interp_rb_pause(1)) s_r_pd_fail++;
    if (s_freeze) rb_freeze_others();
    rbv_take(); /* W359 DIAG rbver: Live-Stand vor dem Laden */
    s_rbi_fld_live = rd32(RBI_FIELD_LAST); /* W372: Feldbezug des Feld-Wartens (live) */
    loc_smooth_rb_pre(rd32(RB_TICK)); /* W385: vorhergesagte Lagen dieses Ticks (vor dem Laden) */
    s_ls_cause = rbi_rb_cause(s_rbc, 1);
    k = rbi_plan(s_rbc, rbr_depth(), &ticks, &sb);
    s_ls_ticks = ticks;
    if (k > 0 && rbr_load((unsigned)k, &pages, &us) != 0) { /* darf nicht vorkommen (k <= rbr_depth) */
        s_rbi_load_fail++;
        k = -2;
    }
    if (k <= 0) {
        loc_smooth_rb_pre(0xFFFFFFFFu); /* W385: kein Rollback -> Messung verwerfen (Tick passt nie) */
        s_rbv_on = 0;
        if (s_freeze) rb_thaw_others();
        rb_rs_resume(); /* W411: nur, wenn gebucht */
        if (s_pdpause) sb_interp_rb_pause(0);
        if (k < 0 && s_rbi_lost_logged < 8) {
            s_rbi_lost_logged++;
            HLOG_W("LOCRBI: Rollback nicht moeglich (%s) bei Slot %d, Ring %u Ticks - Desync moeglich",
                   k == -2 ? "Laden fehlgeschlagen" : "zu weit zurueck/Barriere", rbi_cons(s_rbc, rbi_self(s_rbc)),
                   rbr_depth());
        }
        return;
    }
    rb_pcom_rollback(); /* W355: Commits seit dem Neuaufbau gehoeren jetzt zur verworfenen Zeitlinie */
    if (s_memlog && s_memlog_n < 600 && s_memlog_lt && rd32(RB_TICK) + 40u >= s_memlog_lt && rd32(RB_TICK) <= s_memlog_lt + 40u) {
        s_memlog_n++;
        HLOG_I("LOCRBI: DIAG memlog Rollback %d Ticks (%u Slots) -> Spieltick %u, Neurechnen bis %u", k, ticks, rd32(RB_TICK),
               rbi_replay_until(s_rbc));
    }
    rb_put(s_rbi_s_load, us);
    rb_put(s_rbi_s_ticks, ticks);
    if (ticks > s_rbl_wdep) s_rbl_wdep = ticks; /* W386 */
    rb_put(s_rbi_s_slots, sb);
    { /* W374: Ring fuer das F3-Overlay (Tiefe p95 der letzten 10 s) */
        LONG k = s_rbi_hud_n % RBI_HUD_N;
        s_rbi_hud_d[k] = ticks;
        s_rbi_hud_t[k] = GetTickCount();
        InterlockedIncrement(&s_rbi_hud_n);
    }
    s_rbi_rb_n++;
    if (s_rbi_prep || !s_render) /* W359: Vorbereitung beim Neurechnen wie live (rprep=0: ueberspringen) */
        *(volatile u32 *)(uintptr_t)RB_RJOB = rbi_rjob_replay(s_rbi_prep, RBI_RJOB_LOAD, rd32(RB_RJOB));
    s_phase = PH_REPLAY;
    InterlockedExchange(&s_skipwait, 1);
    { /* W346 R3: Fenster der Neurechnung fuer die Cue-Zuordnung */
        u32 c, i;
        s_xwin_g0 = rbi_gtick(s_rbc);
        s_xwin_g1 = rbi_replay_until(s_rbc);
        s_xwin0 = s_rbi_toff[s_xwin_g0 & RBI_GM];
        s_xwin1 = s_rec_len; /* der laufende Tick ist noch nicht abgeschlossen: bis zum Ende der Aufzeichnung */
        if (s_xwin0 > s_xwin1) s_xwin0 = s_xwin1;
        for (c = s_xwin0; c + sizeof(rb_ev_t) <= s_xwin1;) {
            rb_ev_t *e = (rb_ev_t *)(s_rec + c);
            e->pad &= (uint8_t)~1u;
            if (s_rbs_net_n && (e->fn == EV_XPLAY || e->fn == EV_XPREP) && e->n >= 8u)
                rbs_net_touch(((const u32 *)(e + 1))[1]); /* W413: Start im Fenster - vom Neurechnen beruehrt */
            c += (u32)sizeof *e + e->n;
        }
        for (i = 0; i < s_xside_n; i++) s_xside[i].used = 0;
    }
    rbi_set_rep_cursor();
    s_rbi_t_rep0 = rb_now();
}

/* LOC-DESYNC-REPAIR (21.11.) DIAG sdump=X,S,N (loc_rb_opts): Zustand (Schattenkopie = Stand nach dem Sichern) am
 * Spieltick X, X+S, ... (N-mal) nach <out>\rb_state_<Tick>.bin, nur wenn keine Vorhersage offen ist (bestaetigter Stand).
 * Messgrundlage, ob sich ein Zustand auf einen anderen Client uebertragen laesst (Groesse, Lage der Bereiche, eigene
 * Daten je Client). Spielspeicher: Datei bleibt im Laufordner, nie ins Repo. Keine Eingaben. */
int s_sd_x, s_sd_step = 100, s_sd_n;
static u32 s_sd_next, s_sd_done, s_sd_skip;
void rbi_state_dump(int snap_ok)
{
    u32 gt = rd32(RB_TICK), npg, nreg = 0, hdr[8], i;
    uint8_t *buf;
    char fn[600];
    const char *od;
    HANDLE h;
    DWORD wr;
    long long t0;
    if (s_sd_n <= 0 || s_sd_done + s_sd_skip >= (u32)s_sd_n || !s_ring || !rbr_active() || rbi_replaying(s_rbc)) return;
    if (!s_sd_next) s_sd_next = (u32)s_sd_x;
    if (gt < s_sd_next) return;
    s_sd_next = gt + (u32)(s_sd_step > 0 ? s_sd_step : 100);
    if (!snap_ok || rbi_open_preds(s_rbc)) {
        s_sd_skip++;
        HLOG_I("LOCRBI: DIAG Zustandsabzug Tick %u uebersprungen (%s)", gt, snap_ok ? "Vorhersage offen" : "kein Sichern");
        return;
    }
    npg = rbr_pages();
    buf = (uint8_t *)malloc((size_t)npg * 4096u);
    if (!buf) return;
    t0 = rb_now();
    rbr_shadow_copy(buf);
    od = hcfg_out_dir();
    _snprintf_s(fn, sizeof fn, _TRUNCATE, "%s\\rb_state_%u.bin", od && od[0] ? od : ".", gt);
    h = CreateFileA(fn, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        uint32_t va, len, kind, hot;
        int meth;
        while (rbr_region_info((int)nreg, &va, &len, &kind, &meth, &hot)) nreg++;
        hdr[0] = 0x44534253u; /* "SBSD" */
        hdr[1] = 1;
        hdr[2] = npg;
        hdr[3] = nreg;
        hdr[4] = gt;
        hdr[5] = rd32(RBI_SELF);
        hdr[6] = (u32)rbi_cons(s_rbc, rbi_self(s_rbc));
        hdr[7] = 0;
        WriteFile(h, hdr, sizeof hdr, &wr, NULL);
        for (i = 0; i < nreg; i++) {
            u32 r4[4];
            rbr_region_info((int)i, &va, &len, &kind, &meth, &hot);
            r4[0] = va;
            r4[1] = len;
            r4[2] = kind;
            r4[3] = (u32)meth;
            WriteFile(h, r4, sizeof r4, &wr, NULL);
        }
        WriteFile(h, buf, npg * 4096u, &wr, NULL);
        CloseHandle(h);
        s_sd_done++;
        HLOG_I("LOCRBI: DIAG Zustandsabzug Tick %u Slot %u: %u Seiten (%u KB), %u Bereiche, %.1f ms", gt, hdr[6], npg,
               npg * 4u, nreg, rb_us(rb_now() - t0) / 1000.0);
    }
    free(buf);
}

void rbi_finals(void)
{
    u32 sl, acc, gt;
    const rbi_stats_t *st;
    while (rbi_final_next(s_rbc, &sl, &acc, &gt)) loc_sync_ext_frame(sl, acc, gt);
    st = rbi_stats(s_rbc);
    if (!s_rbi_hcmp_logged) { /* W355: frueh und eindeutig belegen, dass die Hash-Pruefung von Anfang an laeuft */
        int eq = 0;
        int32_t f = rbi_hash_first_cmp(s_rbc, &eq);
        if (f >= 0) {
            s_rbi_hcmp_logged = 1;
            HLOG_I("LOCRBI: Hash-Pruefung aktiv: erster Vergleich mit dem Mitspieler bei Slot %d %s (danach jeder endgueltige "
                   "Slot; Abweichung -> Zeile DESYNC)", f, eq ? "gleich" : "VERSCHIEDEN");
        }
    }
    if (st->hash_bad && !s_rbi_hash_logged) {
        s_rbi_hash_logged = 1;
        HLOG_W("LOCRBI: DESYNC erkannt: Zustandshash von Slot %d weicht vom Mitspieler ab (Hash-Pruefung R2): "
               "eigen %08X/Tick %u, Mitspieler %08X/Tick %u",
               st->hash_bad_first, st->hb_acc[0], st->hb_gt[0], st->hb_acc[1], st->hb_gt[1]);
    }
    if (!rbi_free_running(s_rbc)) { /* W346 R4: Desync bzw. Uneinigkeit -> sauberes Ende mit Meldung */
        int dis;
        EnterCriticalSection(&s_rbn_cs);
        (void)rbn_poll(s_rbn, GetTickCount());
        dis = rbn_peer_disagrees(s_rbn);
        LeaveCriticalSection(&s_rbn_cs);
        if (st->hash_bad) rbi_session_end(1);
        else if (dis) rbi_session_end(0);
    }
}

/* Ticks ohne Ring (vor dem Aufbau bzw. nach Fehlschlag): Buchfuehrung, Kanal, keine Vorhersage. */
void rbi_after_noring(void)
{
    (void)rbi_tick_end(s_rbc, NULL);
    s_rbi_dtick_own = 0;
    s_rbi_tick_takes = 0;
    s_rbi_ts_in = 0;
    s_rbi_toff[rbi_gtick(s_rbc) & RBI_GM] = s_rec_len;
    rbi_poll();
    if (!s_rbi_sent_early) rbi_send(); /* W372: in diesem Tick schon gesendet (frueh) */
    s_rbi_sent_early = 0;
    rbi_finals();
    rbl_tick(); /* W386 */
}

