/* loc_rb_input.c - W464 NET-CORE: aus loc_rollback.c verschoben (W338/W346/W355/W372/W386/W447), unveraendert.
 * Anbindung der Eingabeschicht R2 (loc_rbi.c, Aushandlung loc_rbn.c): Kanal-Socket und -Thread (SBT2 ueber
 * loc_tun bzw. UDP), Zeitbasis/Zeitabgleich, Latenzregler, Netzmodus Auto je Mission, Missionsbeginn/-abfrage,
 * Speicher-Rueckmeldungen, Haken nach dem Tick (rbi_after), Installation und Statuslog der Eingabeschicht. */
#include "loc_rb_int.h"

/* ================= W338 LOC-ROLLBACK-R2: Eingabeschicht (loc_rb_input=1) =================
 * Hooks: FUN_0004E920 (Entnahme eines Rahmens) -> rbi_take, FUN_0004EC80 (eigener 0x34-Rahmen: TCP an die
 * Mitspieler + eigener Ring) -> rbi_own + eigener UDP-Kanal. Die Regelung in FUN_0001D010 (0x1D0BF..0x1D381:
 * Verzoegerung, Zusatzrahmen, Schritte je Bild) entfaellt ([0x306944] = 1). Am Ruhepunkt: Commit nur in Ticks mit
 * Entnahme, Empfang, Rollback (rbr_load + getreue Neurechnung ohne Feld-Warten, Aufrufe des Spiel-Threads aus der
 * laufenden Aufzeichnung), Senden, endgueltige Hashes (loc_sync), Zeitabgleich. Das Spiel-Netz (TCP 1000) traegt
 * weiter Lobby, Start und Ende. Keine Eingaben im Log. */

int s_rbi, s_rbi_last_pre, s_rbi_delay = 2, s_rbi_maxpred = 10, s_rbi_tsync = 2;
int s_rbi_pred = 1, s_rbi_predq = 8; /* W385: Vorhersage fremder Achsen (0 Wiederholung W338, 1 Trend; Standard 1 linear nach Messreihe K3/K4: Korrekturen -82 %), Daempfung q/8 */
/* W386 LOC-LATENCY: niedrige Verzoegerung (Regler loc_rbn.c rbl_*, Paket SBRL), Fenster 10 s auf dem Spiel-Thread. */
rbl_cfg_t s_rbl_cfg;
rbl_t *s_rbl;
static void rbl_rx_src(const uint8_t *b, int len, uint64_t src);
int s_rbl_test; /* TEST ltest=N: Host wechselt alle N s zwischen d_std und d_low (Wechsel-Mechanik pruefen) */
u32 s_ls_cause, s_ls_ticks;          /* W385: Ursache/Tiefe des laufenden Rollbacks (Messung) */
/* W372: early = 1: eigenen Rahmen sofort senden (im Hook FUN_0004EC80 statt am naechsten Ruhepunkt, ~1 Tick frueher);
 * tsync = 2: Zeitabgleich ueber die Feldzahl des Feld-Wartens (rbi_tsync, SBRT), 1 = W338-Stau-Tick, 0 = aus. */
int s_rbi_early = 1, s_rbi_sent_early;
volatile LONG s_wait_extra; /* W372: zusaetzliche Felder fuer das naechste Feld-Warten (Thunk) */
u32 s_rbi_tt_tick, s_rbi_wait_ext_n, s_rbi_wait_ext_f;
/* W372: FUN_00033050(N) zaehlt Felder seit dem zuletzt gesehenen Feld [0x306F8C] (gedeckelt auf N). Der Ring laedt
 * beim Rollback auch diesen Wert (alter Feldstand), die Neurechen-Ticks warten nicht -> der erste Live-Tick danach
 * sah ">= N Felder vergangen" und wartete gar nicht: jede Seite lief je Rollback einen Tick vor (W338-Zeitabgleich
 * bremste das mit je einem Stau-Tick). Fix: Live-Wert vor dem Laden merken, nach dem Neurechnen zuruecksetzen. */
int s_rbi_fldfix = 1;
u32 s_rbi_fld_live, s_rbi_fld_n, s_rbi_fld_diff;
int s_rbi_rec = 1; /* DIAG rec=0: Spiel-Aufrufe nicht aufzeichnen (dann kein Rollback moeglich) */
/* TEST chanloss=N (Prozent, unabhaengig) bzw. chanloss=ge (Gilbert-Elliott wie Profil mobile-bad: 1/60 -> schlecht,
 * 1/4 -> gut, im schlechten Zustand 80 % Verlust): nur ausgehende Kanalpakete, fuer Messungen mit uloss:0 in der
 * Verzoegerungsstufe (Lobby-Suche bleibt deterministisch). */
int s_rbi_chanloss, s_rbi_chanloss_ge, s_rbi_ge_bad;
static u32 s_rbi_chanloss_rng = 0x9E3779B9u, s_rbi_chanloss_n;
static int rbi_chan_drop(void)
{
    u32 r;
    if (!s_rbi_chanloss && !s_rbi_chanloss_ge) return 0;
    s_rbi_chanloss_rng ^= s_rbi_chanloss_rng << 13;
    s_rbi_chanloss_rng ^= s_rbi_chanloss_rng >> 17;
    s_rbi_chanloss_rng ^= s_rbi_chanloss_rng << 5;
    r = s_rbi_chanloss_rng % 100000u;
    if (s_rbi_chanloss_ge) {
        if (!s_rbi_ge_bad && r < 1667u) s_rbi_ge_bad = 1;
        else if (s_rbi_ge_bad && r < 25000u) s_rbi_ge_bad = 0;
        s_rbi_chanloss_rng = s_rbi_chanloss_rng * 1103515245u + 12345u;
        r = s_rbi_chanloss_rng % 100000u;
        if (s_rbi_ge_bad && r < 80000u) { s_rbi_chanloss_n++; return 1; }
        return 0;
    }
    if (r < (u32)s_rbi_chanloss * 1000u) { s_rbi_chanloss_n++; return 1; }
    return 0;
}
rbi_t *s_rbc;
static u32 s_rbi_epoch;
uint8_t s_rbi_out[RBI_MAXP][RBI_FR];
static uint8_t *s_rbi_tramp;
rbi_fn2_t o_e920, o_ec80;
u32 s_rbi_reg_d = 2; /* Anzeige der Verzoegerung in [0x306F04] (LOCSYNC Verz=) */
static SOCKET s_rbi_sock = INVALID_SOCKET;
static int s_rbi_chan;            /* 1 = eigener UDP-Socket (Original-Transport), 2 = ueber loc_tun (SBT2) */
/* Kanal ueber loc_tun: Empfang im Transport-Thread -> Warteschlange -> Spiel-Thread (rbi_poll) */
static CRITICAL_SECTION s_rbi_tq_cs;
static struct { uint64_t src; int len; u32 t; uint8_t b[RBI_PKT_MAX]; } s_rbi_tq[RBI_TQ]; /* W372: t = Ankunft (us) */
static int s_rbi_tq_r, s_rbi_tq_w;
static volatile LONG s_rbi_tq_drop;
u32 s_rbi_toff[RBI_GH];
long long s_rbi_t_rep0;
rb_ser_t *s_rbi_s_ticks, *s_rbi_s_slots, *s_rbi_s_load, *s_rbi_s_rep;
u32 s_rbi_rb_n, s_rbi_rb_last, s_rbi_lost_logged, s_rbi_qfail, s_rbi_ts_waits, s_rbi_ts_ms, s_rbi_bar,
    s_rbi_peer_bad, s_rbi_tx_err, s_rbi_compact, s_rbi_hash_logged, s_rbi_load_fail, s_rbi_missions;
static u32 s_rbi_win_sum, s_rbi_win_n; /* Rollbacks/s im Fenster */
/* W372 LOC-RB-TIMING: Zeitleiste je Live-Tick (ms nach dem Feldwechsel = Ende des Feld-Wartens): eigener Rahmen,
 * erste Fremd-Entnahme, Senden; Puffer-Histogramm im Modul (rbi_clock). Nur Zeiten und Zaehler. */
static long long s_tim_base;
u32 s_tim_t0, s_tim_own_seen, s_tim_take_seen, s_tim_own_t, s_tim_own_pend;
u32 s_tim_h_own[RBI_TH], s_tim_h_take[RBI_TH], s_tim_h_send[RBI_TH], s_tim_own_first, s_tim_ticks;
/* W386: Abtastung -> Senden: SBC-Abfrage nach Feldwechsel, SBC-Abfrage -> eigener Rahmen, Abfragen je Tick (live) */
u32 s_lat_h_xin[RBI_TH], s_lat_h_x2o[RBI_TH], s_lat_xin_tick0, s_lat_multi, s_lat_ticks, s_lat_none, s_lat_before;
static u32 s_tim_slh[RBI_SLH], s_tim_sln, s_tim_sll; /* Stand beim letzten Log (Fenster = Differenz) */
double s_tim_adv_min, s_tim_adv_max, s_tim_adv_sum;
u32 s_tim_adv_n;
u32 rbi_us_now(void) { return rb_us(rb_now() - s_tim_base) | 1u; }
void tim_h(u32 *h, u32 t, u32 since)
{
    u32 ms = (t - since) / 1000u;
    h[ms >= RBI_TH ? RBI_TH - 1 : ms]++;
}
static int tim_pct(const u32 *h, int pct)
{
    u32 n = 0, acc = 0, want;
    int b;
    for (b = 0; b < RBI_TH; b++) n += h[b];
    if (!n) return -1;
    want = (u32)(((uint64_t)n * (u32)pct + 99u) / 100u);
    if (want < 1) want = 1;
    for (b = 0; b < RBI_TH; b++) {
        acc += h[b];
        if (acc >= want) return b;
    }
    return RBI_TH - 1;
}
/* W386: eigener Rahmen (live, erster im Tick): wann las das Spiel die SBC-Eingabe dazu? */
void lat_note_own(void)
{
    u32 tx;
    if (!s_lat_xin_q) return;
    tx = rb_us(s_lat_xin_q - s_tim_base) | 1u;
    if ((int32_t)(tx - s_tim_t0) >= 0) {
        tim_h(s_lat_h_xin, tx, s_tim_t0);
    } else {
        s_lat_before++; /* Abfrage im vorigen Tick (vor dem Feldwechsel) */
    }
    if ((int32_t)(s_tim_own_t - tx) >= 0) tim_h(s_lat_h_x2o, s_tim_own_t, tx);
}
int s_rbi_defer_on = 1;       /* W346 R3 defer=0: Neuaufbau sofort (W338-Verhalten) */
static u32 s_rbi_defer_ticks, s_rbi_defer_n, s_rbi_defer_clean;
u32 s_rbi_dflags[2], s_rbi_dtick_own, s_rbi_dmulti; /* DIAG */
static u32 s_rbi_ts_run, s_rbi_ts_cool; /* Zeitabgleich */
int s_rbi_ts_stall, s_rbi_ts_in;  /* Stau-Tick angefordert / laeuft */
u32 s_rbi_tick_takes;            /* Entnahmen im laufenden Tick */
u32 s_rbi_dstream[2], s_rbi_dn[2], s_rbi_dchk[2][10], s_rbi_dcnt[10];
/* W346 R4: Aushandlung je Mission (loc_rbn), Kanal-Thread (Empfang Kanal 1 + Hallo 5 Hz), Rueckfall, Desync-Ende */
rbn_t *s_rbn;
CRITICAL_SECTION s_rbn_cs;
static HANDLE s_rbi_thr;
static volatile LONG s_rbi_thr_stop;
volatile int s_rbi_mst;          /* RBN_ST_* der laufenden Mission (IDLE = keine) */
static volatile LONG s_rbi_self_hello = 0xFF; /* Spielerindex fuer das Hallo (0xFF = unbekannt) */
/* W469 LOC-RB-HITCH: Hallo sofort senden (Entscheidung des Hosts bzw. Bestaetigung des Clients). Vor der Entscheidung
 * entnimmt keine Seite (das Spiel steht); mit dem 200-ms-Takt dauerte die Aushandlung bis 2 Takte + RTT (W447: 200-390 ms
 * Haenger beim Missionsstart). Gleiches Paket, gleiche Pruefung; nur der Zeitpunkt. */
static volatile LONG s_rbi_hello_now;
static HANDLE s_rbi_hello_ev; /* weckt den Kanal-Thread (sonst Sleep 20 ms) */
int s_pre_primed, s_pre_cfg_d = -1, s_pre_host; /* W475 */
static u32 s_pre_host_t;
int s_negpoll = 40;           /* W469: Aushandlung am Ruhepunkt bis zu so vielen ms fein pollen (loc_rb_opts negpoll=) */
static void rbi_hello_kick(void)
{
    InterlockedExchange(&s_rbi_hello_now, 1);
    if (s_rbi_hello_ev) SetEvent(s_rbi_hello_ev);
}
#define RBI_HELLO_MS 200u     /* Takt in der Lobby und in der Mission (SECURITY.md Ae2: 5 Hz) */
#define RBI_HELLO_NEG_MS 50u  /* W469: Takt waehrend der Aushandlung (TENT/PEND, Bruchteile einer Sekunde je Mission) */
u32 s_rbi_ls_missions, s_rbi_r2_missions, s_rbi_end_desync, s_rbi_end_disagree, s_rbi_last_why;
int s_rbi_hcmp_logged; /* W355 */
static unsigned s_rbi_reg_tick; /* W355: Registerstand beim letzten Ruhepunkt */
/* W359 rprep=1 (Standard): Beim Neurechnen laeuft die Render-Vorbereitung des Spiel-Threads jeden Tick wie live
 * (0x2F256: Freigabe-Warteschlange 0x84AA0 leeren, Doppelpuffer 0xBBB00/0x32880/0xBBBB0 kopieren, Index 0x306A6C
 * tauschen, Bildzaehler 0x306A74); nur das Zeichnen im Render-Thread entfaellt. rprep=0: altes Verhalten (Auftrag = 1,
 * Vorbereitung uebersprungen - Heap- und Puffer-Verlauf weicht vom Original ab). */
int s_rbi_prep = 1;
/* W359 heapchk (Absicherung, Beschreibung bei rbi_heapchk) */
int s_rbi_heapchk = 1;
int s_rbi_heap_bad;      /* 1 = beschaedigt erkannt (Ende im naechsten Live-Tick) */
u32 s_rbi_hc_tick, s_rbi_end_heap;
u32 s_rbi_hc_n, s_rbi_hc_nodes_max, s_rbi_hc_us_max, s_rbi_hc_bad, s_rbi_hc_part, s_rbi_hc_vq, s_rbi_hc_busy;
int s_rbi_rbtest_n, s_rbi_rbtest_k; /* W355 TEST rbtest=N,K: alle N Live-Ticks Rollback um K Ticks erzwingen */
static u32 s_rbi_rbtest_cnt, s_rbi_rbtest_ok, s_rbi_rbtest_next;
int s_rbi_force_desync = -1;     /* TEST desync=N: eigener Hash von Slot N verfaelscht (Gegenprobe Desync-Ende) */
int s_rbi_dmin = 2, s_rbi_dmax = 3; /* W346: Grenzen der adaptiven Verzoegerung (D=1 nur fest) */
long long s_rbi_lt_last;         /* DIAG langer Tick: Zeit des letzten Ruhepunkts */
u32 s_rbi_lt_n, s_rbi_lt_max, s_rbi_lt_logged;
static const char *const k_rbn_why[RBN_WHY_N] = {"-", "eigener Schalter aus", "Mitspieler ohne Rollback",
                                                 "kein Rollback-Host", "Mitspieler meldet Lockstep", "Frist abgelaufen",
                                                 "Host entschied Lockstep"};
/* W370: Kurzform fuer F11-Paket/Status (englisch, stabil) und Sprachschluessel der Einblendung */
const char *const k_rbn_why_slug[RBN_WHY_N] = {"none", "self_off", "peer_no_rollback", "no_rollback_host",
                                                      "peer_lockstep", "timeout", "host_lockstep"};
const char *const k_rbn_why_key[RBN_WHY_N] = {"ovl.rb_why_none", "ovl.rb_why_self_off", "ovl.rb_why_peer_no_rollback",
                                                     "ovl.rb_why_no_rollback_host", "ovl.rb_why_peer_lockstep",
                                                     "ovl.rb_why_timeout", "ovl.rb_why_host_lockstep"};
u32 s_rbi_last_host_why; /* W370: Client: Grund des Hosts (0 = unbekannt) */
/* W374 LOC-NET-OVERLAY: letzte Rollback-Tiefen mit Zeitstempel (Ring, je Rollback ein Speicher) und Vorsprung des
 * letzten Live-Ticks - nur fuer das F3-Overlay (loc_rb_hud_read, eigener Thread, liest nur). */
volatile u32 s_rbi_hud_d[RBI_HUD_N], s_rbi_hud_t[RBI_HUD_N];
volatile LONG s_rbi_hud_n;
volatile double s_rbi_hud_adv;
int s_rbi_npeers = -1;    /* W370: Mitspieler der laufenden Mission (Verbindungstabelle bzw. Plaetze - 1) */

/* W370 LOC-RB-FALLBACK-US: verbundene Mitspieler aus der Verbindungstabelle des Spiels (C = *(0x3116AC), 10 Eintraege
 * zu 16 Byte, Byte 0 Bit 0 belegt, LOC_BOOT §35). RBI_NPL ist der hoechste Platz + 1: nach einem Wiederbeitritt bleibt
 * ein Platz leer (erster Menschentest: Gast auf Platz 2 von 3, Host erwartete 2 Mitspieler -> Lockstep).
 * Rueckgabe Anzahl (Plaetze 1..9), -1 = Tabelle nicht lesbar; *mask = belegte Plaetze. */
static int rbi_conn_peers(u32 *mask)
{
    int k, c = 0;
    u32 m = 0;
    __try {
        u32 tab = *(volatile const u32 *)(uintptr_t)0x3116ACu;
        if (!tab) return -1;
        for (k = 1; k < 10; k++)
            if (*(volatile const uint8_t *)(uintptr_t)(tab + (u32)k * 0x10u) & 1u) {
                m |= 1u << k;
                c++;
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    if (mask) *mask = m;
    return c;
}

static void rbi_pct(rb_ser_t *s, u32 *p50, u32 *p95, u32 *mx)
{
    static u32 tmp[RB_SER_N];
    u32 n = s ? s->n : 0;
    *p50 = *p95 = *mx = 0;
    if (!n) return;
    memcpy(tmp, s->v, n * sizeof(u32));
    qsort(tmp, n, sizeof(u32), rb_cmp);
    *p50 = tmp[n / 2];
    *p95 = tmp[(n * 95u) / 100u];
    *mx = tmp[n - 1];
}

static int rbi_peer_ok(const locnet *n, u32 ip, uint16_t port)
{
    int i;
    if (ip == n->self_ip || !locnet_ip_allowed(ip)) return 0;
    for (i = 0; i < n->npeers; i++)
        if (n->peers[i].ip == ip && locnet_port_out(n, ip, RBI_CHAN_GP) == port) return 1;
    return 0;
}

void rbi_poll(void)
{
    locnet *n = loc_net_core();
    uint8_t b[RBI_PKT_MAX + 16];
    int k;
    if (!s_rbi_chan || !n || !s_rbc) return;
    if (s_rbi_chan == 1) { /* W346: in der Mission liest der Spiel-Thread den Socket selbst (wie W338): der Kanal-Thread
                            * teilt sich den Kern des Prozesses und kaeme erst nach einer Zeitscheibe dran (M_fern2:
                            * 0,17 Rollbacks/s statt 0,01, 10 s Zeitabgleich-Stau). Hallo -> Aushandlung. */
        for (k = 0; k < 64; k++) {
            struct sockaddr_in f;
            int fl = (int)sizeof f, r;
            int len = recvfrom(s_rbi_sock, (char *)b, (int)sizeof b, 0, (struct sockaddr *)&f, &fl);
            uint64_t src;
            if (len < 0) {
                if (WSAGetLastError() == WSAECONNRESET) continue;
                break;
            }
            if (fl < (int)sizeof f || f.sin_family != AF_INET || !rbi_peer_ok(n, f.sin_addr.s_addr, ntohs(f.sin_port))) {
                s_rbi_peer_bad++;
                continue;
            }
            src = ((uint64_t)f.sin_addr.s_addr << 16) | ntohs(f.sin_port);
            if (rbn_is_hello(b, len)) {
                EnterCriticalSection(&s_rbn_cs);
                (void)rbn_rx(s_rbn, GetTickCount(), b, len, src);
                LeaveCriticalSection(&s_rbn_cs);
                continue;
            }
            if (rbn_is_pre(b, len)) { /* W475 */
                EnterCriticalSection(&s_rbn_cs);
                (void)rbn_pre_rx(s_rbn, GetTickCount(), b, len, src);
                LeaveCriticalSection(&s_rbn_cs);
                continue;
            }
            rbi_clock(s_rbc, rbi_us_now()); /* W372: Ankunft = jetzt (Spiel-Thread liest selbst) */
            if (rbl_is_pkt(b, len)) { /* W386: Bericht/Entscheidung niedrige Verzoegerung */
                rbl_rx_src(b, len, src);
                continue;
            }
            if (rbi_is_tt(b, len)) { /* W372: Puffer-Meldung des Mitspielers */
                (void)rbi_tt_rx(s_rbc, b, len, src);
                continue;
            }
            r = rbi_rx(s_rbc, b, len, src);
            if (r == 1 && (s_rbi_mst == RBN_ST_TENT || s_rbi_mst == RBN_ST_PRE)) { /* W475: PRE = Beweis vom Host */
                EnterCriticalSection(&s_rbn_cs);
                if (s_rbi_mst == RBN_ST_PRE) rbn_pre_proof(s_rbn, src);
                else rbn_confirm_peer(s_rbn, src);
                LeaveCriticalSection(&s_rbn_cs);
            }
        }
    }
    /* Warteschlange: Kanal 2 (Transport-Thread) bzw. Reste, die der Kanal-Thread vor dem Missionsbeginn empfing */
    for (k = 0; k < RBI_TQ; k++) {
        uint64_t src;
        int len, r;
        EnterCriticalSection(&s_rbi_tq_cs);
        if (s_rbi_tq_r == s_rbi_tq_w) {
            LeaveCriticalSection(&s_rbi_tq_cs);
            break;
        }
        src = s_rbi_tq[s_rbi_tq_r].src;
        len = s_rbi_tq[s_rbi_tq_r].len;
        memcpy(b, s_rbi_tq[s_rbi_tq_r].b, (size_t)len);
        rbi_clock(s_rbc, s_rbi_tq[s_rbi_tq_r].t); /* W372: Ankunft im Empfangs-Thread */
        s_rbi_tq_r = (s_rbi_tq_r + 1) % RBI_TQ;
        LeaveCriticalSection(&s_rbi_tq_cs);
        if (rbl_is_pkt(b, len)) { /* W386 */
            rbl_rx_src(b, len, src);
            continue;
        }
        if (rbi_is_tt(b, len)) { /* W372 */
            (void)rbi_tt_rx(s_rbc, b, len, src);
            continue;
        }
        r = rbi_rx(s_rbc, b, len, src);
        if (r == 1 && (s_rbi_mst == RBN_ST_TENT || s_rbi_mst == RBN_ST_PRE)) { /* Host: SBR2 mit dieser Epoche = Mitspieler
                                                                                 * hat R2 angenommen; W475 PRE: Beweis */
            EnterCriticalSection(&s_rbn_cs);
            if (s_rbi_mst == RBN_ST_PRE) rbn_pre_proof(s_rbn, src);
            else rbn_confirm_peer(s_rbn, src);
            LeaveCriticalSection(&s_rbn_cs);
        }
    }
}

/* W346: Empfangsweg beider Kanaele (Thread des Kanals bzw. des Transports): Hallo -> Aushandlung (vollstaendig
 * geprueft in rbn_rx), sonst Warteschlange fuer den Spiel-Thread (Pruefung in rbi_rx). */
static void rbi_dispatch(uint64_t src, const uint8_t *d, int len)
{
    if (len <= 0 || len > RBI_PKT_MAX) {
        InterlockedIncrement(&s_rbi_tq_drop);
        return;
    }
    if (rbn_is_hello(d, len)) {
        EnterCriticalSection(&s_rbn_cs);
        (void)rbn_rx(s_rbn, GetTickCount(), d, len, src);
        LeaveCriticalSection(&s_rbn_cs);
        return;
    }
    if (rbn_is_pre(d, len)) { /* W475: Vorab-Einigung (vollstaendig geprueft in rbn_pre_rx) */
        EnterCriticalSection(&s_rbn_cs);
        (void)rbn_pre_rx(s_rbn, GetTickCount(), d, len, src);
        LeaveCriticalSection(&s_rbn_cs);
        return;
    }
    EnterCriticalSection(&s_rbi_tq_cs);
    if ((s_rbi_tq_w + 1) % RBI_TQ == s_rbi_tq_r) {
        InterlockedIncrement(&s_rbi_tq_drop);
    } else {
        s_rbi_tq[s_rbi_tq_w].src = src;
        s_rbi_tq[s_rbi_tq_w].len = len;
        s_rbi_tq[s_rbi_tq_w].t = rbi_us_now();
        memcpy(s_rbi_tq[s_rbi_tq_w].b, d, (size_t)len);
        s_rbi_tq_w = (s_rbi_tq_w + 1) % RBI_TQ;
    }
    LeaveCriticalSection(&s_rbi_tq_cs);
}

/* An alle Mitspieler senden (Kanal 1: eigener Socket ueber die Verzoegerungsstufe, Kanal 2: SBT2). Thread-sicher. */
static void rbi_send_all(const uint8_t *b, int len)
{
    locnet *n = loc_net_core();
    int i;
    if (!n || len <= 0) return;
    if (s_rbi_chan == 2) {
        if (loctun_dgram(n->tun, INADDR_BROADCAST, RBI_CHAN_GP, RBI_CHAN_GP, (const char *)b, len) < 0) s_rbi_tx_err++;
        return;
    }
    for (i = 0; i < n->npeers; i++) {
        struct sockaddr_in to;
        int r;
        if (n->peers[i].ip == n->self_ip) continue;
        memset(&to, 0, sizeof to);
        to.sin_family = AF_INET;
        to.sin_addr.s_addr = n->peers[i].ip;
        to.sin_port = htons(locnet_port_out(n, n->peers[i].ip, RBI_CHAN_GP));
        if (n->sim) r = locnetsim_send(n->sim, s_rbi_sock, 0, (const char *)b, len, 0, &to); /* W322-Pruefstand */
        else r = sendto(s_rbi_sock, (const char *)b, len, 0, (const struct sockaddr *)&to, (int)sizeof to);
        if (r < 0) s_rbi_tx_err++;
    }
}

/* ================= W475 LOC-RB-PREAGREE: Lobby-Rolle und Kontext fuer die Vorab-Einigung =================
 * Rolle wie loc_rulenet (W420): Host = eigenes Spiel bietet ein Match an (G+0x15 0x22/0x23), juenger als die letzte
 * eigene Beitrittsanfrage; Beitretender = Beitrittsziel (join_ip). Kontext = Pruefsumme der Lobby-Einstellungen
 * (S = G+0x5624: Lager A/B, Sitze A/B, Modus, Mission, Option - ohne Bestaetigungs-Byte; Spieler je Seite
 * G+0xA4/A5): jede Aenderung (z. B. Seitenwechsel) = neuer Vorschlag. Nur Werte, keine Namen. */
int s_pre_on = 1; /* loc_rb_opts preagree=0: keine Vorab-Einigung (bisheriger Weg, Vergleich) */
int s_pre_test;   /* TEST loc_rb_opts pretest=1: Verzoegerungswechsel, 3: Beitritt genau beim Missionsbeginn (Host) */
static u32 s_pre_thost;
static volatile LONG s_pre_role, s_pre_ctx;
static void rbi_lobby_update(u32 now)
{
    locnet *n = loc_net_core();
    int ls = -1, role = RBN_ROLE_NONE;
    u32 hip = 0, ctx = 0, jt;
    __try {
        const uint8_t *g = *(const uint8_t *const *)(uintptr_t)0x361708u;
        if (g) {
            static const int k_ix[7] = {0, 1, 2, 3, 4, 5, 7};
            const uint8_t *sx = g + 0x5624;
            int k;
            ls = g[0x15];
            ctx = 2166136261u;
            for (k = 0; k < 7; k++) ctx = (ctx ^ sx[k_ix[k]]) * 16777619u;
            ctx = (ctx ^ g[0xA4]) * 16777619u;
            ctx = (ctx ^ g[0xA5]) * 16777619u;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ls = -1;
        ctx = 0;
    }
    if (ls == 0x22 || ls == 0x23) s_pre_thost = now | 1u;
    jt = n ? (u32)n->join_ms : 0u;
    if (n && s_pre_thost && (!jt || (LONG)(s_pre_thost - jt) > 0)) role = RBN_ROLE_HOST;
    else if (n && jt) {
        role = RBN_ROLE_JOIN;
        hip = (u32)n->join_ip;
    }
    if (!s_pre_on) role = RBN_ROLE_NONE;
    if (role != (int)s_pre_role)
        HLOG_I("LOCRBI-PRE: Lobby-Rolle %s", role == RBN_ROLE_HOST ? "Host" : role == RBN_ROLE_JOIN ? "Beitretender" : "keine");
    InterlockedExchange(&s_pre_role, role);
    InterlockedExchange(&s_pre_ctx, (LONG)ctx);
    EnterCriticalSection(&s_rbn_cs);
    rbn_set_lobby(s_rbn, role, hip, ctx);
    LeaveCriticalSection(&s_rbn_cs);
}

/* W475: Host beim Missionsbeginn: Adressen der verbundenen Mitspieler (Verbindungstabelle, Socket -> Gegenstelle). */
static int rbi_conn_ips(u32 *ips, int cap)
{
    int k, c = 0;
    __try {
        u32 tab = *(volatile const u32 *)(uintptr_t)0x3116ACu;
        if (!tab) return -1;
        for (k = 1; k < 10; k++) {
            const uint8_t *e = (const uint8_t *)(uintptr_t)(tab + (u32)k * 0x10u);
            if (*(volatile const uint8_t *)e & 1u) {
                SOCKET s = (SOCKET) * (volatile const u32 *)(e + 4);
                struct sockaddr_in a;
                int l = (int)sizeof a, er = WSAGetLastError();
                if (c >= cap || !s || s == INVALID_SOCKET || getpeername(s, (struct sockaddr *)&a, &l) != 0 ||
                    a.sin_family != AF_INET) {
                    WSASetLastError(er);
                    return -1;
                }
                WSASetLastError(er);
                ips[c++] = a.sin_addr.s_addr;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return c;
}

/* W346: Kanal-Thread: Empfang Kanal 1 (select 20 ms) und Hallo alle 200 ms (beide Kanaele, auch in der Lobby).
 * W469: waehrend der Aushandlung alle 50 ms und sofort nach einer Entscheidung/Bestaetigung (s_rbi_hello_now). */
static DWORD WINAPI rbi_chan_thread(LPVOID unused)
{
    DWORD next = GetTickCount();
    (void)unused;
    while (!s_rbi_thr_stop) {
        if (s_rbi_chan == 1 && (s_rbi_mst == RBN_ST_IDLE || s_rbi_mst == RBN_ST_LS)) { /* in der Mission: Spiel-Thread */
            fd_set rs;
            struct timeval tv = {0, 20000};
            FD_ZERO(&rs);
            FD_SET(s_rbi_sock, &rs);
            if (select(0, &rs, NULL, NULL, &tv) > 0) {
                locnet *n = loc_net_core();
                int k;
                for (k = 0; k < 64 && n; k++) {
                    uint8_t b[RBI_PKT_MAX + 16];
                    struct sockaddr_in f;
                    int fl = (int)sizeof f;
                    int len = recvfrom(s_rbi_sock, (char *)b, (int)sizeof b, 0, (struct sockaddr *)&f, &fl);
                    if (len < 0) {
                        if (WSAGetLastError() == WSAECONNRESET) continue;
                        break;
                    }
                    if (fl < (int)sizeof f || f.sin_family != AF_INET ||
                        !rbi_peer_ok(n, f.sin_addr.s_addr, ntohs(f.sin_port))) {
                        s_rbi_peer_bad++;
                        continue;
                    }
                    rbi_dispatch(((uint64_t)f.sin_addr.s_addr << 16) | ntohs(f.sin_port), b, len);
                }
            }
        } else if (s_rbi_hello_ev) {
            WaitForSingleObject(s_rbi_hello_ev, 20); /* W469: sofort bei rbi_hello_kick */
        } else {
            Sleep(20);
        }
        {
            DWORD now = GetTickCount();
            int neg = s_rbi_mst == RBN_ST_TENT || s_rbi_mst == RBN_ST_PEND || s_rbi_mst == RBN_ST_PRE;
            if (InterlockedExchange(&s_rbi_hello_now, 0)) next = now; /* W469 */
            else if (neg && (LONG)(next - now) > (LONG)RBI_HELLO_NEG_MS) next = now + RBI_HELLO_NEG_MS;
            if ((LONG)(now - next) >= 0) {
                uint8_t hb[RBN_PKT_MAX];
                int hl;
                next = now + (neg ? RBI_HELLO_NEG_MS : RBI_HELLO_MS);
                uint8_t pb[RBN_PRE_MAX];
                int pl;
                if (s_rbi_mst == RBN_ST_IDLE) rbi_lobby_update(now); /* W475: Rolle/Kontext nur in der Lobby */
                EnterCriticalSection(&s_rbn_cs);
                hl = rbn_tx(s_rbn, now, (int)s_rbi_self_hello, hb, (int)sizeof hb);
                rbn_pre_tick(s_rbn, now);
                pl = s_pre_on ? rbn_pre_tx(s_rbn, now, pb, (int)sizeof pb) : 0;
                LeaveCriticalSection(&s_rbn_cs);
                if (hl > 0) rbi_send_all(hb, hl);
                if (pl > 0) rbi_send_all(pb, pl); /* W475: Vorschlag/Bestaetigung/Ruecknahme im Hallo-Takt */
            }
        }
    }
    return 0;
}

void rbi_send(void)
{
    locnet *n = loc_net_core();
    uint8_t b[RBI_PKT_MAX];
    int len, i;
    if (!s_rbi_chan || !n) return;
    len = rbi_tx(s_rbc, b, (int)sizeof b);
    if (len <= 0) return;
    if (s_phase != PH_REPLAY && s_tim_own_pend) { /* W372: Abstand eigener Rahmen -> Senden */
        s_tim_own_pend = 0;
        tim_h(s_tim_h_send, rbi_us_now(), s_tim_own_t);
    }
    if (rbi_chan_drop()) return; /* TEST chanloss */
    (void)i;
    rbi_send_all(b, len);
    if (s_rbi_tsync == 2 && s_phase != PH_REPLAY && !(++s_rbi_tt_tick & 1u)) { /* W372: Puffer melden (10/s) */
        len = rbi_tt_tx(s_rbc, b, (int)sizeof b);
        if (len > 0) rbi_send_all(b, len);
    }
}

static void rbi_tun_rx(uint32_t vip, const uint8_t *d, int len) /* Transport-Thread */
{
    rbi_dispatch((uint64_t)vip << 16, d, len);
}

static int rbi_sock_open(void)
{
    locnet *n = loc_net_core();
    struct sockaddr_in a;
    u_long nb = 1;
    BOOL off = FALSE, excl = TRUE;
    DWORD ret = 0;
    SOCKET s;
    if (!n) {
        HLOG_W("LOCRBI: Netz aus - Eingabeschicht aus (Original-Lockstep)");
        return 0;
    }
    if (n->tun) { /* eigener Transport (W324): Kanal als SBT2-Datagramm an Spielport 1260, nie an den Gast */
        if (!loctun_available()) {
            HLOG_W("LOCRBI: Transport nicht verfuegbar - Eingabeschicht aus (Original-Lockstep)");
            return 0;
        }
        loctun_set_app(n->tun, (uint16_t)RBI_CHAN_GP, rbi_tun_rx);
        s_rbi_chan = 2;
        HLOG_I("LOCRBI: Eingabekanal ueber den eigenen Transport (SBT2-Datagramm, Spielport %u, nicht an das Spiel)",
               RBI_CHAN_GP);
        return 1;
    }
    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return 0;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&excl, (int)sizeof excl);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = n->self_ip;
    a.sin_port = htons(locnet_port_out(n, n->self_ip, RBI_CHAN_GP));
    if (bind(s, (const struct sockaddr *)&a, (int)sizeof a) != 0) {
        HLOG_W("LOCRBI: Kanal-Port %u nicht frei (WSA %d) - Eingabeschicht aus", ntohs(a.sin_port), WSAGetLastError());
        closesocket(s);
        return 0;
    }
    ioctlsocket(s, FIONBIO, &nb);
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, (DWORD)sizeof off, NULL, 0, &ret, NULL, NULL);
    s_rbi_sock = s;
    s_rbi_chan = 1;
    HLOG_I("LOCRBI: Eingabekanal UDP Port %u (Basis+260), nur bekannte Mitspieler (%d)", ntohs(a.sin_port), n->npeers);
    return 1;
}

/* ================= W386 LOC-LATENCY: niedrige Verzoegerung (Regler rbl_* in loc_rbn.c) =================
 * Spiel-Thread: alle 10 s ein Fenster (Korrekturen, Schaetzer D-1, Tiefe, Neurechnen p95, Haenger, Sprung) -> Bericht im
 * Paket SBRL (jede Sekunde wiederholt, mit der letzten Entscheidung des Hosts); der Host wertet aus und entscheidet
 * fuer alle. Wechsel nur ueber rbi_set_delay (eigene Rahmen ab einem angekuendigten Slot), nie im Neurechnen. Wechselt
 * D, beginnt das Fenster neu (Berichte mischen nie zwei Verzoegerungen). Nur Zaehler im Log, keine Eingaben. */
static u32 s_rbl_rp[RBL_RPW], s_rbl_rp_n;  /* Neurechnen (us) im Fenster */
u32 s_rbl_wdep;                     /* groesste Rollback-Tiefe im Fenster (Ticks) */
static u32 s_rbl_t0, s_rbl_tsend, s_rbl_win, s_rbl_rb0, s_rbl_hang0, s_rbl_ttest;
static rbi_stats_t s_rbl_st0;
static rbl_rep_t s_rbl_rep;
static int s_rbl_have;
static u32 s_rbl_bind_bad, s_rbl_switch_n, s_rbl_dticks[9];

void rbl_note_replay(u32 us)
{
    s_rbl_rp[s_rbl_rp_n % RBL_RPW] = us;
    s_rbl_rp_n++;
}
static int rbl_cmp_u32(const void *a, const void *b)
{
    u32 x = *(const u32 *)a, y = *(const u32 *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}
static int rbl_ready(void)
{
    return s_rbi_ep && s_ring && s_ready && !s_failed && rbr_active() && s_rbi_rec && rbr_depth() >= 2u &&
           !rbi_free_running(s_rbc) && s_phase == PH_RECORD;
}
static void rbl_win_reset(u32 now)
{
    s_rbl_t0 = now;
    s_rbl_rb0 = s_rbi_rb_n;
    s_rbl_st0 = *rbi_stats(s_rbc);
    s_rbl_hang0 = loc_sync_hangs();
    s_rbl_wdep = 0;
    s_rbl_rp_n = 0;
    (void)loc_smooth_window_jump();
}
static void rbl_rx_src(const uint8_t *b, int len, uint64_t src)
{
    int pidx = rbl_peek_pidx(b, len);
    if (!s_rbl) return;
    if (pidx >= 0 && rbi_peer_src(s_rbc, pidx) != src) { /* nur vom gebundenen Absender dieses Spielers (SBR2) */
        s_rbl_bind_bad++;
        return;
    }
    (void)rbl_rx(s_rbl, GetTickCount(), b, len);
}
static const char *const k_rbl_why[RBL_WHY_N] = {"-", "gut", "Korrekturen", "Tiefe", "Neurechnen", "Haenger", "Sprung",
                                                 "ohne Ring/Bericht"};

static void rbl_window(u32 now)
{
    const rbi_stats_t *st = rbi_stats(s_rbc);
    u32 dt = now - s_rbl_t0, late, need, acc = 0, n, k;
    rbl_rep_t r;
    int rtt;
    if (dt < 1000u) dt = 1000u;
    memset(&r, 0, sizeof r);
    r.ready = rbl_ready();
    r.dcur = rbi_delay_eff(s_rbc) >= 1 ? rbi_delay_eff(s_rbc) : (int)s_rbi_reg_d;
    s_rbl_win = s_rbl_win >= 0xFFFFu ? 1u : s_rbl_win + 1u;
    r.win = s_rbl_win;
    r.cpm = (u32)((uint64_t)(s_rbi_rb_n - s_rbl_rb0) * 60000u / dt);
    r.est_cpm = (u32)((uint64_t)(st->est_wrong - s_rbl_st0.est_wrong) * 60000u / dt);
    late = st->est_late - s_rbl_st0.est_late;
    if (late) {
        need = (late * 95u + 99u) / 100u;
        for (k = 0; k < 4u; k++) {
            acc += st->est_dep[k] - s_rbl_st0.est_dep[k];
            if (acc >= need) break;
        }
        r.est_dep = k + 1u > 4u ? 4u : k + 1u;
    }
    r.dep = s_rbl_wdep;
    n = s_rbl_rp_n < RBL_RPW ? s_rbl_rp_n : RBL_RPW;
    if (n) {
        u32 tmp[RBL_RPW];
        memcpy(tmp, s_rbl_rp, n * sizeof tmp[0]);
        qsort(tmp, n, sizeof tmp[0], rbl_cmp_u32);
        if (n >= 2u) n--; /* der teuerste je Fenster zaehlt nicht (Ring-Neuaufbau am Ende eines Neurechnens, ~35 ms einmalig,
                           * N100); eine langsame CPU hat viele teure Neurechnungen */
        r.rp = (tmp[(n * 95u + 99u) / 100u - 1u] + 99u) / 100u;
        if (r.rp > 255u) r.rp = 255u;
    }
    r.hang = loc_sync_hangs() - s_rbl_hang0;
    {
        double j = loc_smooth_window_jump();
        r.jump = j > 65535.0 ? 65535u : (unsigned)(j + 0.5);
    }
    s_rbl_rep = r;
    s_rbl_have = 1;
    rbl_win_reset(now);
    EnterCriticalSection(&s_rbn_cs);
    rtt = rbn_rtt_max(s_rbn, now);
    LeaveCriticalSection(&s_rbn_cs);
    if (rbi_self(s_rbc) == 0 && s_rbl_cfg.on && !s_rbl_test &&
        rbl_host_step(s_rbl, now, &r, rbi_own_next_slot(s_rbc), rtt)) {
        const rbl_stats_t *ls = rbl_stats(s_rbl);
        s_rbl_tsend = now - 1000u; /* Entscheidung sofort senden (danach jede Sekunde wiederholt) */
        HLOG_I("LOCRBI-LOWD: Host entscheidet D=%d (Grund %s, RTT %d ms, runter=%u hoch=%u)", rbl_target(s_rbl),
               ls->last_why < RBL_WHY_N ? k_rbl_why[ls->last_why] : "?", rtt, ls->down, ls->up);
    }
    {
        const rbl_stats_t *ls = rbl_stats(s_rbl);
        HLOG_I("LOCRBI-LOWD: Fenster %u D=%d Ziel=%d (Start %d, niedrig %d) bereit=%d | Korrekturen/min=%u geschaetzt bei "
               "D-1=%u Tiefe max=%u geschaetzt=%u Neurechnen p95=%.1f ms Haenger=%u Sprung=%u | ausgewertet=%u "
               "uebersprungen=%u runter=%u hoch=%u Grund=%s | SBRL tx=%u rx=%u schlecht=%u Epoche=%u Bindung=%u",
               r.win, r.dcur, rbl_target(s_rbl), rbl_dstd(s_rbl), rbl_dlow(s_rbl), r.ready, r.cpm, r.est_cpm, r.dep,
               r.est_dep, r.rp / 10.0, r.hang, r.jump, ls->evals, ls->skipped, ls->down, ls->up,
               ls->last_why < RBL_WHY_N ? k_rbl_why[ls->last_why] : "?", ls->tx, ls->rx_ok, ls->rx_bad, ls->rx_epoch,
               s_rbl_bind_bad);
    }
}

/* Ruhepunkt (live, mit und ohne Ring): Entscheidung anwenden, Fenster, Bericht senden. */
void rbl_tick(void)
{
    u32 now;
    int d, dc;
    u32 sl;
    if (!s_rbl || !rbl_active(s_rbl) || !s_rbi_ep || rbi_replaying(s_rbc)) return;
    now = GetTickCount();
    if (rbl_take_decision(s_rbl, &d, &sl)) {
        int ok = rbi_set_delay(s_rbc, d, sl);
        HLOG_I("LOCRBI-LOWD: Wechsel auf D=%d ab Slot %u %s (eigener naechster Slot %u)", d, sl,
               ok ? "vorgemerkt" : "nicht noetig", rbi_own_next_slot(s_rbc));
    }
    dc = rbi_delay_eff(s_rbc);
    if (dc >= 1 && dc <= 8) {
        s_rbl_dticks[dc]++;
        if ((u32)dc != s_rbi_reg_d) {
            const rbi_stats_t *st = rbi_stats(s_rbc);
            HLOG_I("LOCRBI-LOWD: Verzoegerung jetzt %d Ticks (vorher %u) bei Slot %u, Wechsel runter=%u hoch=%u spaet=%u",
                   dc, s_rbi_reg_d, rbi_own_next_slot(s_rbc), st->dchg_down, st->dchg_up, st->dchg_late);
            s_rbi_reg_d = (u32)dc;
            s_rbl_switch_n++;
            s_rbh_events |= RBH_EV_DCHG; /* W469 */
            rbl_win_reset(now); /* Fenster mischen nie zwei Verzoegerungen */
        }
    }
    if (s_rbl_test > 0 && rbi_self(s_rbc) == 0 && now - s_rbl_ttest >= (u32)s_rbl_test * 1000u && rbl_ready()) {
        int rtt, to = rbl_target(s_rbl) == rbl_dstd(s_rbl) ? rbl_dlow(s_rbl) : rbl_dstd(s_rbl);
        s_rbl_ttest = now;
        EnterCriticalSection(&s_rbn_cs);
        rtt = rbn_rtt_max(s_rbn, now);
        LeaveCriticalSection(&s_rbn_cs);
        if (rbl_force(s_rbl, to, rbi_own_next_slot(s_rbc), rtt)) {
            s_rbl_tsend = now - 1000u;
            HLOG_I("LOCRBI-LOWD: TEST erzwungener Wechsel auf D=%d", to);
        }
    }
    if (now - s_rbl_t0 >= RBL_WIN_MS) rbl_window(now);
    if (s_rbl_have && s_rbl_cfg.on && now - s_rbl_tsend >= 1000u) { /* Bericht + Entscheidung jede Sekunde */
        uint8_t b[RBL_LEN];
        int len;
        s_rbl_tsend = now;
        len = rbl_tx(s_rbl, &s_rbl_rep, b, (int)sizeof b);
        if (len > 0) rbi_send_all(b, len);
    }
}

/* ================= W447 LOC-NET-AUTO: Netzmodus Auto (Profil je Mission, loc_rbn.h) =================
 * Profil DIREKT: Ziel 1 Tick (d_low = 1), Regler schneller (ein gutes Fenster nach dem Ringstart) und strenger
 * (Korrekturen, Spruenge); Profil ROLLBACK bzw. FEST: Regler wie W386. Kein Moduswechsel im Match; Wechsel der
 * Verzoegerung weiter nur ueber rbl/rbi_set_delay (Slots sind die Wahrheit, Determinismus unberuehrt). */
int s_auto_drtt = 80, s_auto_djit = 15;     /* Grenzen fuer DIREKT (loc_rb_opts adrtt=/adjit=; Messreihe W447) */
int s_auto_start1 = 60;                     /* DIREKT beginnt mit 1 Tick bis zu dieser RTT (ms, loc_rb_opts adstart=, 0 = nie;
                                                    * Messreihe W447: 30 ms sauber, 80 ms ohne Ring ~2 s Stau im Start) */
int s_auto_good = 1, s_auto_lcpm = 300, s_auto_hcpm = 600, s_auto_hjump = 300; /* Regler DIREKT (Korrekturgrenzen wie W386) */
volatile LONG s_auto_prof;                  /* Profil der laufenden Mission (RBN_PROF_*), -1 = keine */
volatile LONG s_auto_rtt, s_auto_var;       /* Messwerte beim Missionsbeginn (ms) */
volatile LONG s_auto_mode = -1;             /* Wunsch zur Laufzeit: 0 Auto, 1 Rollback, 2 Lockstep (-1 = cfg) */
static const char *const k_auto_prof[3] = {"fest (Rollback)", "auto -> direkt", "auto -> rollback"};

/* Regler-Einstellungen fuer das Profil der laufenden Mission */
static void auto_rbl_cfg(int prof, rbl_cfg_t *c)
{
    *c = s_rbl_cfg;
    if (prof != RBN_PROF_DIRECT) return;
    c->good_n = s_auto_good;
    c->lcpm = s_auto_lcpm;
    c->hcpm = s_auto_hcpm;
    c->hjump = s_auto_hjump;
}

/* Missionsbeginn mit R2 (Host und Client): Regler zuruecksetzen. */
static void rbl_mission_begin(u32 epoch, int delay, int rtt)
{
    int dl, prof, var, start_low = 0;
    rbl_cfg_t cfg;
    u32 now = GetTickCount();
    if (!s_rbl) return;
    EnterCriticalSection(&s_rbn_cs);
    prof = rbn_profile(s_rbn);
    var = rbn_rttvar_max(s_rbn, now);
    LeaveCriticalSection(&s_rbn_cs);
    InterlockedExchange(&s_auto_prof, prof);
    InterlockedExchange(&s_auto_rtt, rtt);
    InterlockedExchange(&s_auto_var, var);
    auto_rbl_cfg(prof, &cfg);
    if (prof == RBN_PROF_DIRECT && delay == 1 && s_rbi_delay <= 0) { /* W447: Start mit 1 Tick, Rueckweg d_std */
        int ds = rbn_pick_delay(rtt, s_rbi_dmin, s_rbi_dmax);
        start_low = ds > 1;
        delay = ds > 1 ? ds : 2;
    }
    dl = (s_rbl_cfg.on && s_rbi_delay <= 0) ? rbn_pick_low(rtt, delay, s_rbl_cfg.lowmax) : delay;
    if (prof == RBN_PROF_DIRECT && s_rbl_cfg.on && s_rbi_delay <= 0) dl = 1; /* W447: Ziel 1 Tick */
    HLOG_I("LOCRBI-AUTO: Mission: Profil %s (RTT %d ms, Schwankung %d ms, Grenzen direkt <= %d/%d ms) -> Start D=%d, Ziel D=%d, "
           "Regler gut=%d Korrekturen/min <= %d, zurueck > %d, Sprung > %d",
           (prof >= 0 && prof <= 2) ? k_auto_prof[prof] : "?", rtt, var, s_auto_drtt, s_auto_djit, start_low ? 1 : delay, dl, cfg.good_n,
           cfg.lcpm, cfg.hcpm, cfg.hjump);
    rbl_begin(s_rbl, &cfg, epoch, rbi_self(s_rbc), s_rbi_npeers, delay, dl);
    if (start_low) {
        rbl_start_low(s_rbl);
        HLOG_I("LOCRBI-AUTO: Mission beginnt direkt mit D=1 (Rueckweg D=%d, bis zum Ringstart ohne Rueckwechsel wegen Ring)", delay);
    }
    memset(s_rbl_dticks, 0, sizeof s_rbl_dticks);
    s_rbl_have = 0;
    s_rbl_win = 0;
    s_rbl_tsend = now;
    s_rbl_ttest = now;
    s_rbl_switch_n = 0;
    rbl_win_reset(now);
    if (s_rbl_cfg.on)
        HLOG_I("LOCRBI-LOWD: Mission: niedrige Verzoegerung %s (Start D=%d, niedrig D=%d bis %d ms, RTT %d ms)%s",
               dl < delay ? "moeglich" : "nicht moeglich", delay, dl, s_rbl_cfg.lowmax, rtt,
               s_rbl_test ? " - TEST Wechsel erzwungen" : "");
}

void rbl_mission_end(void)
{
    const rbi_stats_t *st;
    u32 tot = 0, k;
    if (!s_rbl || !rbl_active(s_rbl)) return;
    st = rbi_stats(s_rbc);
    for (k = 1; k <= 8u; k++) tot += s_rbl_dticks[k];
    if (tot)
        HLOG_I("LOCRBI-LOWD: Mission Ende: Ticks je D 1/2/3/4=%u/%u/%u/%u (%.0f/%.0f/%.0f %%), Wechsel %u (runter=%u hoch=%u "
               "spaet=%u), Schaetzer spaet=%u falsch=%u von %u",
               s_rbl_dticks[1], s_rbl_dticks[2], s_rbl_dticks[3], s_rbl_dticks[4], 100.0 * s_rbl_dticks[1] / tot,
               100.0 * s_rbl_dticks[2] / tot, 100.0 * s_rbl_dticks[3] / tot, s_rbl_switch_n, st->dchg_down, st->dchg_up,
               st->dchg_late, st->est_late, st->est_wrong, st->est_n);
    rbl_end(s_rbl);
    InterlockedExchange(&s_auto_prof, -1); /* W447 */
}

/* W346: R2 fuer diese Mission ausgehandelt (Epoche/Verzoegerung stehen fest, noch keine Entnahme). */
static void rbi_go_r2(u32 epoch, int delay)
{
    s_spec_off_mission = s_spec_paused_n = 0; /* W390: jede Mission beginnt mit Vorlauf an */
    s_spec_level = 0;
    s_spec_pause_until = 0;
    s_spec_win_n = 0;
    int rtt;
    if (!rbi_configure(s_rbc, epoch, delay) &&
        !(rbi_epoch(s_rbc) == epoch && rbi_p0(s_rbc) >= 0 && s_pre_cfg_d == delay) && /* W475: PRE, schon konfiguriert */
        !rbi_rebind(s_rbc, epoch, delay)) { /* W475: Host entschied anders (nichts entnommen); sonst darf es nicht vorkommen */
        HLOG_W("LOCRBI: Aushandlung zu spaet (Entnahme schon erfolgt) - Mission im Original-Lockstep");
        rbi_end(s_rbc);
        s_rbi_mst = RBN_ST_LS;
        return;
    }
    EnterCriticalSection(&s_rbn_cs);
    rtt = rbn_rtt_max(s_rbn, GetTickCount());
    LeaveCriticalSection(&s_rbn_cs);
    s_rbi_epoch = epoch;
    s_rbi_r2_missions++;
    s_rbi_reg_d = (u32)delay;
    memset(s_rbi_toff, 0, sizeof s_rbi_toff);
    s_rec_len = s_rec_cur = 0; /* Aufzeichnung je Mission neu (Tick 0 ab Offset 0) */
    if (s_ready && s_rec && s_rbi_rec) s_phase = PH_RECORD;
    s_rbi_mst = RBN_ST_R2;
    rbi_left_reset(); /* W366 */
    s_rbi_ep = 1;
    s_rbh_events |= RBH_EV_MSTART; /* W469 */
    s_pre_cfg_d = delay;
    rbi_hello_kick(); /* W469: Client bestaetigt sofort (Host wartet darauf) */
    loc_sync_ext_on(1);
    HLOG_I("LOCRBI: Mission %u mit eigener Eingabeschicht (ausgehandelt): Spieler %u von %u, Epoche %08X, Verzoegerung %d "
           "Ticks (%s, Umlaufzeit %d ms), Fenster %d Slots, Zeitabgleich %d", s_rbi_missions, rd32(RBI_SELF), rd32(RBI_NPL),
           epoch, delay, s_rbi_delay > 0 ? "fest" : "adaptiv", rtt, s_rbi_maxpred, s_rbi_tsync);
    rbl_mission_begin(epoch, delay, rtt); /* W386 */
}

static void rbi_fallback_ls(int why)
{
    char pd[400];
    int hw;
    s_rbi_mst = RBN_ST_LS;
    s_rbi_ls_missions++;
    s_rbi_last_why = (u32)why;
    InterlockedExchange(&s_auto_prof, -1); /* W447 */
    EnterCriticalSection(&s_rbn_cs);
    hw = rbn_host_why(s_rbn);
    rbn_peers_desc(s_rbn, GetTickCount(), pd, (int)sizeof pd);
    LeaveCriticalSection(&s_rbn_cs);
    s_rbi_last_host_why = why == RBN_WHY_HOST_LS ? (u32)hw : 0u;
    HLOG_I("LOCRBI: Mission %u im Original-Lockstep (%s)", s_rbi_missions,
           why > 0 && why < RBN_WHY_N ? k_rbn_why[why] : "?");
    /* W370: Einzelheiten beidseitig (nur Zahlen) */
    HLOG_I("LOCRBI: Rueckfall-Grund %s%s%s, Mitspieler erwartet %d: %s", why > 0 && why < RBN_WHY_N ? k_rbn_why_slug[why] : "?",
           s_rbi_last_host_why ? ", Grund des Hosts " : "",
           s_rbi_last_host_why && s_rbi_last_host_why < RBN_WHY_N ? k_rbn_why_slug[s_rbi_last_host_why] : "", s_rbi_npeers, pd);
    if (why != RBN_WHY_SELF_OFF) { /* W370: Hinweis im Spiel (de/en): Rollback aus, hoehere Eingabeverzoegerung */
        wchar_t t1[160], fmt[240], t2[240], rs[120];
        int rk = (s_rbi_last_host_why > 0 && s_rbi_last_host_why < RBN_WHY_N) ? (int)s_rbi_last_host_why : why;
        sb_lang_w("loc.rb_off_t", t1, 160);
        sb_lang_w(k_rbn_why_key[rk > 0 && rk < RBN_WHY_N ? rk : 0], rs, 120);
        sb_lang_w("loc.rb_off_d", fmt, 240);
        if (wcsstr(fmt, L"%s")) _snwprintf_s(t2, 240, _TRUNCATE, fmt, rs);
        else wcsncpy_s(t2, 240, fmt, _TRUNCATE);
        sb_ov_toast_msg(t1, t2, 8000u);
    }
}

/* W346: Missionsbeginn (Vorlauf FUN_0001D3D0): Aushandlung starten. Vor der Entscheidung entnimmt keine Seite. */
static const char *const k_pre_miss[8] = {"-", "nicht genutzt: kein Vorschlag (bisheriger Weg)",
                                           "nicht genutzt: Folge ueberholt", "nicht genutzt: Wunsch geaendert",
                                           "nicht genutzt: Rolle", "nicht genutzt: Zahl der Mitspieler anders",
                                           "nicht genutzt: Bestaetigung fehlt/alt", "nicht genutzt: Adressen anders"};
void rbi_mission_open(void)
{
    int st, self, npl, d, why, pre_miss = 0;
    u32 ep;
    if (!s_rbi || !s_rbi_chan || !s_rbc || !s_rbn) return;
    self = (int)rd32(RBI_SELF);
    npl = (int)rd32(RBI_NPL);
    InterlockedExchange(&s_rbi_self_hello, (self >= 0 && self < 10) ? self : 0xFF);
    { /* W370: Host zaehlt die verbundenen Mitspieler (nicht hoechster Platz) */
        u32 mask = 0;
        int cp = self == 0 ? rbi_conn_peers(&mask) : -1;
        s_rbi_npeers = cp > 0 ? cp : npl - 1;
        if (self == 0)
            HLOG_I("LOCRBI: Mission %u: Plaetze %d (NPL), verbunden 0x%03X -> %d Mitspieler erwartet%s", s_rbi_missions + 1,
                   npl, mask, s_rbi_npeers, cp > 0 ? "" : " (Tabelle leer/unlesbar, Plaetze - 1)");
    }
    { /* W475: Host - Adressen der verbundenen Mitspieler fuer die Pruefung der Vorab-Einigung */
        u32 ips[RBN_PRE_MAXPART + 2];
        int nip = self == 0 ? rbi_conn_ips(ips, RBN_PRE_MAXPART + 2) : -1;
        if (self == 0 && s_pre_test == 3 && nip >= 0 && nip < RBN_PRE_MAXPART + 2) ips[nip++] = 0x0100007Fu; /* TEST: Beitritt */
        EnterCriticalSection(&s_rbn_cs);
        if (self == 0 && s_pre_test == 1) rbn_pre_test_bump(s_rbn); /* TEST: Verzoegerungswechsel beim Start */
        rbn_pre_set_conn(s_rbn, nip, ips);
        LeaveCriticalSection(&s_rbn_cs);
        if (self == 0 && s_pre_test) HLOG_I("LOCRBI-PRE: TEST pretest=%d (%s)", s_pre_test,
                                            s_pre_test == 1 ? "Verzoegerungswechsel beim Start" : "Beitritt beim Start");
    }
    EnterCriticalSection(&s_rbn_cs);
    st = rbn_mission_start(s_rbn, GetTickCount(), self, s_rbi_npeers);
    ep = rbn_epoch(s_rbn);
    d = rbn_delay(s_rbn);
    why = rbn_why(s_rbn);
    pre_miss = rbn_pre_miss(s_rbn);
    LeaveCriticalSection(&s_rbn_cs);
    s_pre_cfg_d = -1;
    if (self == 0 && s_pre_on)
        HLOG_I("LOCRBI-PRE: Mission %u: Vorab-Einigung %s", s_rbi_missions + 1,
               st == RBN_ST_R2 ? "genutzt - Rollback sofort (keine Netzrunde)" : k_pre_miss[pre_miss >= 0 && pre_miss < 8 ? pre_miss : 0]);
    s_rbi_missions++;
    if (s_rbi_missions > 1) rb_rthread_refresh("Missionsbeginn"); /* W375: Render-Thread je Mission neu */
    if (st == RBN_ST_LS) {
        rbi_fallback_ls(why);
        return;
    }
    rbi_begin(s_rbc, ep, self, d > 0 ? d : 2, s_rbi_maxpred); /* eigene Rahmen ab dem Vorlauf mitschreiben */
    if (st == RBN_ST_PRE) s_pre_cfg_d = d; /* W475: Epoche/D der Vorab-Einigung stehen schon */
    rbi_hello_kick(); /* W469: Angebot des Hosts bzw. "in Mission" sofort melden */
    loc_smooth_reset(); /* W385 */
    s_rbi_hcmp_logged = 0;
    s_rbi_rbtest_next = 0;
    rb_pcom_clear();
    s_rbi_heap_bad = 0; /* W359 */
    s_rbi_hc_tick = 0;
    s_rbi_reg_tick = g_exreg_n;
    s_pre_primed = 0;
    if (st == RBN_ST_R2) { /* W475: Host, Vorab-Einigung - Entscheidung steht (im Hallo). Lokal wie PRE: eigene Rahmen
                            * sofort senden; die erste Entnahme erst, wenn von jedem Mitspieler der Rahmen fuer Slot 0
                            * da ist (sonst nimmt der Host im ersten Tick nur den eigenen Slot und staut mitten im Tick -
                            * Hash von Slot 0 aus dem halben Tick; der bisherige Weg hatte die Rahmen hier schon) */
        s_pre_cfg_d = d;
        s_pre_host = 1;
        s_pre_host_t = GetTickCount();
        s_rbi_mst = RBN_ST_PRE;
        rbi_hello_kick(); /* Entscheidung sofort im Hallo (Clients ohne Bindung folgen ihr wie bisher) */
        HLOG_I("LOCRBI-PRE: Mission %u: Host entscheidet aus der Vorab-Einigung (Epoche %08X, D=%d) - Entnahme ab dem ersten "
               "Rahmen jedes Mitspielers", s_rbi_missions, ep, d);
        return;
    }
    s_pre_host = 0;
    if (st == RBN_ST_PRE) {
        s_rbi_mst = st;
        HLOG_I("LOCRBI-PRE: Mission %u: Vorab-Einigung (Epoche %08X, Verzoegerung %d) - eigene Rahmen sofort, Entnahme nach "
               "dem ersten Rahmen bzw. der Entscheidung des Hosts", s_rbi_missions, ep, d);
        return;
    }
    s_rbi_mst = st;
    {
        char pd[400];
        int hw;
        EnterCriticalSection(&s_rbn_cs);
        hw = rbn_host_waiting(s_rbn);
        rbn_peers_desc(s_rbn, GetTickCount(), pd, (int)sizeof pd);
        LeaveCriticalSection(&s_rbn_cs);
        HLOG_I("LOCRBI: Mission %u: Aushandlung (%s), Spieler %d von %d", s_rbi_missions,
               st == RBN_ST_TENT ? (hw ? "Host wartet auf das Hallo eines Mitspielers (Nachfrist)"
                                       : "Host bietet Rollback an, wartet auf Bestaetigung")
                                 : "Client wartet auf die Entscheidung des Hosts",
               self, npl);
        HLOG_I("LOCRBI: Mission %u: Mitspieler %s", s_rbi_missions, pd); /* W370 */
    }
}

/* W475: PRE (Entnahme-Haken): Slot-Zuordnung festlegen und eigene Rahmen sofort senden, Empfang (Beweis) pruefen.
 * Der Wechsel nach R2 geschieht wie im bisherigen Weg NUR am Ruhepunkt (rbi_mission_poll in rb_after): so beginnt die
 * erste Entnahme auf allen Seiten im selben Spieltick (Hash von Slot 0 gleich). Rueckgabe immer 0 (Stau). */
int rbi_pre_step(void)
{
    if (!s_pre_primed && rbi_prime(s_rbc)) {
        s_pre_primed = 1;
        rbi_send();
    }
    rbi_poll();
    return 0;
}

/* W346: Aushandlung am Ruhepunkt weiterfuehren (TENT/PEND). */
void rbi_mission_poll(void)
{
    int st, d, why;
    u32 ep;
    rbi_poll();
    if (s_rbi_mst == RBN_ST_PRE && s_pre_host) { /* W475: Host wartet auf den Slot-0-Rahmen jedes Mitspielers (hoechstens 2 s) */
        int k, have = 0, self = rbi_self(s_rbc);
        for (k = 0; k < RBI_MAXP; k++)
            if (k != self && rbi_contig(s_rbc, k) >= 0) have++;
        if (have < s_rbi_npeers && (u32)(GetTickCount() - s_pre_host_t) < 2000u) return;
        HLOG_I("LOCRBI-PRE: Mission %u: Host beginnt (%d/%d Mitspieler mit erstem Rahmen, %u ms nach Missionsbeginn)",
               s_rbi_missions, have, s_rbi_npeers, (u32)(GetTickCount() - s_pre_host_t));
        s_pre_host = 0;
    }
    EnterCriticalSection(&s_rbn_cs);
    st = rbn_poll(s_rbn, GetTickCount());
    ep = rbn_epoch(s_rbn);
    d = rbn_delay(s_rbn);
    why = rbn_why(s_rbn);
    LeaveCriticalSection(&s_rbn_cs);
    if (st == RBN_ST_R2) {
        if (s_rbi_mst == RBN_ST_PRE) {
            const rbn_pre_stats_t *ps;
            EnterCriticalSection(&s_rbn_cs);
            ps = rbn_pre_stats(s_rbn);
            HLOG_I("LOCRBI-PRE: Mission %u: %s (Epoche %08X, D=%d)", s_rbi_missions,
                   ep != rbi_epoch(s_rbc) ? "Host entschied anders - folge ihm (nichts entnommen)"
                   : rbi_self(s_rbc) == 0 ? "Host: Vorab-Einigung aktiv"
                   : ps->pre_r2_frame ? "Beweis: erster Rahmen des Hosts" : "Beweis: Entscheidung im Hallo",
                   ep, d);
            LeaveCriticalSection(&s_rbn_cs);
        }
        rbi_go_r2(ep, d);
    } else if (st == RBN_ST_LS) {
        rbi_end(s_rbc);
        rbi_fallback_ls(why);
    }
}

/* W346 R3: Allokation des Spiel-Threads waehrend R2 (krnl_mem). Aufzeichnen: Ergebnis je Aufruf. Neurechnen: derselbe
 * Block wie in der ersten Rechnung (genullt wie frischer Speicher) statt eines neuen (kein Leck, gleiche Adresse). */
static u32 s_rbi_alloc_rec, s_rbi_alloc_rep, s_rbi_alloc_new;
static u32 s_rbi_alloc_other, s_rbi_alloc_miss, s_rbi_alloc_nocommit; /* W355 DIAG */
static u32 s_rbi_com_rec, s_rbi_com_rep, s_rbi_com_miss, s_rbi_com_new; /* W355: Commit in reservierten Bereich */
static u32 s_rbi_com_logged;
/* W355: Commits (base != 0) seit dem letzten Ring-Neuaufbau, egal in welcher Phase (die erste Belegung kann aus der
 * korrigierten Zeitlinie kommen, also im Neurechnen). Diese Seiten liegen nicht im Ring; jeder Rollback im Aufschub
 * zielt hoechstens auf den fruehesten dieser Ticks (rbi_rebuild_note) - nach dem Laden sind sie in der neuen Zeitlinie
 * also noch nicht belegt ("verworfen", live = 0), behalten aber den Inhalt der alten. Der erste Commit eines verworfenen
 * Bereichs (live oder beim Neurechnen) wird wie frisch bedient: genullt, kein Registereintrag. Ein erneuter Commit in
 * derselben Zeitlinie (live = 1) aendert nichts (wie im Original: Commit belegter Seiten laesst den Inhalt). */
struct rb_pcom_s s_pcom[RB_PCOM_MAX];
u32 s_pcom_n, s_pcom_full, s_rbi_com_same, s_rbi_com_drop;
void rb_pcom_clear(void) { s_pcom_n = 0; }
static u32 s_rbi_pcom_drop_log; /* W366 */
void rb_pcom_rollback(void) /* nach rbr_load: alle Eintraege gehoeren zur verworfenen Zeitlinie */
{
    u32 k;
    for (k = 0; k < s_pcom_n; k++) {
        if (s_pcom[k].live) s_rbi_com_drop++;
        s_pcom[k].live = 0;
    }
}
int s_memlog;   /* W359 DIAG memlog=1: Commit/Freigabe/Rollback in R2 protokollieren (nur Adressen/Ticks) */
u32 s_memlog_n, s_memlog_lt;
/* W390: Belegungen des Vorlaufs. Ein Block, den der Vorlauf belegt hat, gehoert nach dem Zuruecksetzen niemandem; die
 * echte Zeitlinie bekommt ihn bei derselben Belegung (gleiche Groesse/Art) genullt wieder (kein Leck, wie beim
 * Neurechnen). Commits in Reservierungen bekommen einen verworfenen Eintrag (der echte Commit nullt die Seiten). */
struct rb_spec_ac_s s_spec_ac[RB_SPEC_AC];
u32 s_spec_ac_n, s_spec_ac_hit;
struct rb_spec_cm_s s_spec_cm[RB_SPEC_CM];
u32 s_spec_cm_n;
static int rb_alloc_cb(int op, unsigned size, unsigned type, unsigned prot, unsigned *io)
{
    u32 ih = size * 31u + type * 7u + prot;
    if (s_spec && GetCurrentThreadId() == s_game_tid) { /* W390: spekulativer Vorlauf */
        if (op == 1 && *io) { /* belegt: fuer die echte Zeitlinie vormerken */
            s_spec_ev.alloc++;
            if (s_spec_ac_n < RB_SPEC_AC) {
                s_spec_ac[s_spec_ac_n].ih = ih;
                s_spec_ac[s_spec_ac_n].a = *io;
                s_spec_ac[s_spec_ac_n].size = size;
                s_spec_ac[s_spec_ac_n].gt = rd32(RB_TICK);
                s_spec_ac_n++;
            }
        } else if (op == 3) { /* Commit in Reservierung */
            u32 a3 = *io & ~0xFFFu, e3 = (a3 + size + 0xFFFu) & ~0xFFFu, k;
            s_spec_ev.commit++;
            for (k = 0; k < s_pcom_n; k++)
                if (a3 >= s_pcom[k].a && e3 <= s_pcom[k].end) break;
            if (k == s_pcom_n) {
                if (s_spec_cm_n < RB_SPEC_CM) {
                    s_spec_cm[s_spec_cm_n].a = a3;
                    s_spec_cm[s_spec_cm_n].end = e3;
                    s_spec_cm_n++;
                }
            }
        }
        return 0; /* belegen/committen wie das System (Inhalt ist nach dem Zuruecksetzen ohne Besitzer) */
    }
    if (op == 0 && s_spec_ac_n && s_rbi_ep && s_phase == PH_RECORD && rb_mine()) { /* W390: Block aus dem Vorlauf */
        u32 k;
        for (k = 0; k < s_spec_ac_n; k++) {
            if (s_spec_ac[k].ih != ih) continue;
            __try {
                LOC_RB_PREWRITE(s_spec_ac[k].a, (size + 0xFFFu) & ~0xFFFu);
                memset((void *)(uintptr_t)s_spec_ac[k].a, 0, size);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                break;
            }
            *io = s_spec_ac[k].a;
            rb_rec_put(EV_NTALLOC, 0, ih, io, 4); /* ein spaeteres Neurechnen bedient denselben Block */
            s_rbi_alloc_rec++;
            s_spec_ac[k] = s_spec_ac[--s_spec_ac_n];
            s_spec_ac_hit++;
            return 1;
        }
    }
    if (s_rbi_ep && op == 0 && s_phase != PH_IDLE && GetCurrentThreadId() != s_game_tid) s_rbi_alloc_other++;
    if (s_rbi_ep && op == 3 && s_rbi_com_logged < 8) { /* W355 DIAG: Commit in reservierten Bereich (Adresse/Groesse) */
        s_rbi_com_logged++;
        HLOG_I("LOCRBI: DIAG Commit 0x%X Groesse 0x%X frisch=%u Phase %d Spiel-Thread %d Spieltick %u", *io & ~0xFFFu, size,
               *io & 1u, s_phase, GetCurrentThreadId() == s_game_tid, rd32(RB_TICK));
    }
    if (s_memlog && s_rbi_ep && op >= 2 && s_memlog_n < 600) { /* W359 DIAG memlog */
        s_memlog_n++;
        s_memlog_lt = rd32(RB_TICK);
        HLOG_I("LOCRBI: DIAG memlog %s 0x%X Groesse 0x%X Typ 0x%X Phase %d Spiel-Thread %d Spieltick %u Slot %d", op == 2 ? "Commit?" :
               (op == 3 ? "Commit" : (op == 5 ? "Freigabe" : "?")), *io & ~0xFFFu, size, type, s_phase,
               GetCurrentThreadId() == s_game_tid, rd32(RB_TICK), s_rbc ? rbi_cons(s_rbc, rbi_self(s_rbc)) : -1);
    }
    if (op == 5) { /* W366: Decommit/Freigabe - diese Seiten sind nicht mehr belegt. Ohne Abgleich meldete op 2 einen
                    * spaeteren Commit derselben Seiten als "schon belegt" und liess ihn aus: Das Spiel schrieb in
                    * zurueckgegebene Heap-Seiten (zweites Match ohne Neustart, Laden der Mission: AV 0x12810A). */
        u32 fa = *io & ~0xFFFu, fe = size ? ((*io + size + 0xFFFu) & ~0xFFFu) : fa + 0x2000000u, k, n = 0;
        for (k = 0; k < s_pcom_n;) {
            if (s_pcom[k].end > fa && s_pcom[k].a < fe) {
                s_pcom[k] = s_pcom[--s_pcom_n]; /* nur weniger "schon belegt": danach echter Commit (sicher) */
                n++;
            } else {
                k++;
            }
        }
        if (n && s_rbi_pcom_drop_log < 8) {
            s_rbi_pcom_drop_log++;
            HLOG_I("LOCRBI: Freigabe 0x%X Groesse 0x%X - %u Commit-Eintraege verworfen (spaeterer Commit wieder echt)", fa,
                   size, n);
        }
        return 0;
    }
    if (!s_rbi_ep || !rb_mine()) return 0;
    if (op == 3) { /* W355: Commit aufzeichnen (Adresse, frisch) */
        u32 a3 = *io & ~0xFFFu, e3 = (a3 + size + 0xFFFu) & ~0xFFFu, k;
        for (k = 0; k < s_pcom_n; k++) /* schon in der Liste (erneuter Commit derselben Seiten)? */
            if (a3 >= s_pcom[k].a && e3 <= s_pcom[k].end) break;
        if (k == s_pcom_n) {
            if (s_pcom_n < RB_PCOM_MAX) {
                s_pcom[s_pcom_n].a = a3;
                s_pcom[s_pcom_n].end = e3;
                s_pcom[s_pcom_n].live = 1;
                s_pcom_n++;
            } else {
                s_pcom_full++;
            }
        }
        if (s_phase == PH_RECORD) {
            u32 v = *io;
            rb_rec_put(EV_NTCOMMIT, 0, ih ^ ((v & ~0xFFFu) * 13u), &v, 4); /* gleiche Basis = gleicher Aufruf */
            s_rbi_com_rec++;
        } else if (s_phase == PH_REPLAY) {
            s_rbi_com_new++; /* nur die korrigierte Zeitlinie committet */
        }
        return 0;
    }
    if (op == 2) { /* W355: Neurechnen - derselbe Commit wie in der ersten Rechnung */
        u32 a, end, k;
        (void)ih;
        a = *io & ~0xFFFu;
        end = (*io + size + 0xFFFu) & ~0xFFFu;
        for (k = 0; k < s_pcom_n; k++)
            if (a >= s_pcom[k].a && end <= s_pcom[k].end) break;
        if (k == s_pcom_n || end <= a) { /* nicht seit dem letzten Neuaufbau belegt: Original */
            if (s_phase == PH_REPLAY) s_rbi_com_miss++;
            return 0;
        }
        if (s_pcom[k].live) { /* in dieser Zeitlinie schon belegt: Commit aendert nichts (kein Registereintrag) */
            s_rbi_com_same++;
            *io = a;
            return 1;
        }
        __try {
            MEMORY_BASIC_INFORMATION mb;
            if (!VirtualQuery((LPCVOID)(uintptr_t)a, &mb, sizeof mb) || mb.State != MEM_COMMIT ||
                (u32)((uintptr_t)mb.BaseAddress + mb.RegionSize) < end) { s_rbi_com_miss++; return 0; }
            /* verworfene Zeitlinie hatte den Bereich belegt: Inhalt wie frisch committet. Nur dieser Teilbereich gilt
             * danach als belegt; reicht der Eintrag weiter, wird er geteilt (Rest bleibt verworfen). */
            LOC_RB_PREWRITE(a, end - a);
            memset((void *)(uintptr_t)a, 0, end - a);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            s_rbi_com_miss++;
            return 0;
        }
        if (s_pcom[k].a == a && s_pcom[k].end == end) {
            s_pcom[k].live = 1;
        } else if (s_pcom_n + 2u <= RB_PCOM_MAX) { /* teilen: [a0,a) verworfen, [a,end) belegt, [end,e0) verworfen */
            u32 a0 = s_pcom[k].a, e0 = s_pcom[k].end;
            s_pcom[k].a = a;
            s_pcom[k].end = end;
            s_pcom[k].live = 1;
            if (a0 < a) { s_pcom[s_pcom_n].a = a0; s_pcom[s_pcom_n].end = a; s_pcom[s_pcom_n].live = 0; s_pcom_n++; }
            if (end < e0) { s_pcom[s_pcom_n].a = end; s_pcom[s_pcom_n].end = e0; s_pcom[s_pcom_n].live = 0; s_pcom_n++; }
        } else {
            s_pcom_full++; /* kein Platz zum Teilen: ganzer Eintrag gilt als belegt (Rest: erneuter Commit nullt nicht) */
            s_pcom[k].live = 1;
        }
        *io = a;
        s_rbi_com_rep++;
        return 1;
    }
    if (op == 1) {
        if (s_phase == PH_RECORD) {
            u32 v = *io;
            rb_rec_put(EV_NTALLOC, 0, ih, &v, 4);
            s_rbi_alloc_rec++;
        } else if (s_phase == PH_REPLAY) {
            s_rbi_alloc_new++; /* nur die korrigierte Zeitlinie belegt */
        }
        return 0;
    }
    if (s_phase == PH_REPLAY) {
        const uint8_t *d;
        const rb_ev_t *e = rb_rep_get(EV_NTALLOC, ih, &d);
        u32 a;
        if (!e || e->n != 4) { s_rbi_alloc_miss++; return 0; }
        memcpy(&a, d, 4);
        __try {
            MEMORY_BASIC_INFORMATION mb;
            if (!VirtualQuery((LPCVOID)(uintptr_t)a, &mb, sizeof mb) || mb.State != MEM_COMMIT) { s_rbi_alloc_nocommit++; return 0; }
            LOC_RB_PREWRITE(a, (size + 0xFFFu) & ~0xFFFu);
            memset((void *)(uintptr_t)a, 0, size);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
        *io = a;
        s_rbi_alloc_rep++;
        return 1;
    }
    return 0;
}

void rbi_after(void)
{
    int done = 0, com, snap_ok = 0; /* W390: Schatten = aktueller Stand (Sichern bzw. Neuaufbau in diesem Tick) */
    if (s_phase == PH_REPLAY && (s_rbi_prep || !s_render)) /* W359: Render-Vorbereitung dieses Neurechen-Ticks lief wie
                                                            * live; Auftrag gilt als erledigt (gezeichnet wird nicht) */
        *(volatile u32 *)(uintptr_t)RB_RJOB = rbi_rjob_replay(s_rbi_prep, RBI_RJOB_TICK, rd32(RB_RJOB));
    com = rbi_tick_end(s_rbc, &done);
    s_rbi_dtick_own = 0;
    s_rbi_tick_takes = 0;
    s_rbi_ts_in = 0;
    { /* W346 R3: neue Allokationen (nur Register gewachsen, keine Freigabe/kein Schutzwechsel) aendern bestehende
       * Bereiche nicht: Neuaufbau aufschieben, bis keine Vorhersage mehr offen ist (bestaetigter Tick, Barriere ohne
       * Verlust); bis dahin keine neue Vorhersage. Freigaben/Fehler bauen wie bisher sofort neu. */
        int need = g_exreg_n != s_reg_gen || g_loc_rb_memev != s_memev_seen || s_r_force_rebuild;
        int only_alloc = need && g_loc_rb_memev == s_memev_seen && !s_r_force_rebuild;
        u32 waited = rbi_rebuild_wait(s_rbc);
        int gate;
        if (g_exreg_n != s_rbi_reg_tick) { /* W355: Belegung in diesem Tick (auch im Neurechnen) -> Untergrenze */
            s_rbi_reg_tick = g_exreg_n;
            rbi_rebuild_note(s_rbc);
        }
        gate = rbi_rebuild_gate(s_rbc, need, only_alloc, s_rbi_defer_on, 200u);
        /* W355: Entscheidung im Modul (Selbsttest); nie mitten in einer Neurechnung, Rollback ab Allokations-Tick */
        if (gate == RBI_REBUILD_DEFER) {
            if (!s_rbi_defer_ticks++) {
                s_rbi_defer_n++;
                HLOG_I("LOCRBI: Neuaufbau (neue Allokation) aufgeschoben bei Slot %d - offene Vorhersagen erst bestaetigen%s",
                       rbi_cons(s_rbc, rbi_self(s_rbc)), rbi_replaying(s_rbc) ? " (Neurechnung laeuft)" : "");
            }
            need = 0;
        } else if (gate == RBI_REBUILD_NOW && s_rbi_defer_ticks) {
            if (!rbi_open_preds(s_rbc)) {
                s_rbi_defer_clean++;
                HLOG_I("LOCRBI: Neuaufbau nach %u Ticks (%u live) an bestaetigtem Stand (kein Rollback verloren)",
                       s_rbi_defer_ticks, waited);
            } else {
                HLOG_W("LOCRBI: Neuaufbau nach %u Ticks trotz offener Vorhersage (Aufschub-Grenze)", s_rbi_defer_ticks);
            }
            s_rbi_defer_ticks = 0;
        } else if (gate == RBI_REBUILD_NONE) {
            s_rbi_defer_ticks = 0;
        }
        if (!need) goto rbi_no_rebuild;
    }
    if (rb_ring_check_regions()) { /* Ring neu: kein Rollback vor diesen Stand */
        rbi_barrier(s_rbc);
        s_rbi_bar++;
        rb_pcom_clear(); /* W355: die Seiten liegen jetzt im Ring */
        snap_ok = rbr_active();
    } else
    rbi_no_rebuild:
    if (com) {
        u32 ovf0 = s_r_ovf;
        long long th;
        if (s_phase != PH_REPLAY) rb_quiesce_commit(); /* W469: Wartegrenze s_qwait */
        th = rb_now(); /* W469 */
        rb_ring_commit(s_phase == PH_REPLAY);
        rbh_add(RBH_COMMIT, rb_now() - th);
        snap_ok = !s_r_force_rebuild && rbr_active(); /* W390 */
        if (s_r_force_rebuild) (void)rb_ring_check_regions(); /* Sichern scheiterte: sofort neu, kein Laden */
        if (s_r_ovf != ovf0 || !rbr_active() || rbr_depth() == 0) {
            rbi_barrier(s_rbc);
            s_rbi_bar++;
        }
    }
    if (s_phase == PH_REPLAY) loc_smooth_replay_tick(rd32(RB_TICK)); /* W387: Lagen-Historie auf den korrigierten Stand */
    if (rbi_replaying(s_rbc)) { /* naechster Neurechen-Tick */
        if (s_endguard && rb_loop_exits() && !s_rep_ended) {
            /* W411: Die korrigierte Zeitlinie beendet die Mission in diesem Neurechen-Tick: die Schleife wird verlassen,
             * ein weiteres rb_after kommt nicht. Threads jetzt fortsetzen (sonst kreist der Spiel-Thread @0x2F355). */
            s_rep_endstop++;
            s_rep_ended = 1;
            HLOG_W("LOCRBI: Missionsende im Neurechnen (Spieltick %u, Zaehler %u/%u) - Neurechnung beendet, Threads fortgesetzt",
                   rd32(RB_TICK), rd32(RB_END_CNT), rd32(RB_END_LIM));
            rbi_replay_finish();
            return;
        }
        rbi_set_rep_cursor();
        return;
    }
    if (done) {
        rbv_check(); /* W359 DIAG: vor dem Fortsetzen der Threads */
        rbi_replay_finish();
        loc_smooth_rb_post(rd32(RB_TICK), s_ls_cause, s_ls_ticks); /* W385: korrigierte Lagen desselben Ticks */
    } else if (++s_rbi_hc_tick >= 20u) { /* W359: je Sekunde live (nach dem Sichern, Render-Thread fertig) */
        s_rbi_hc_tick = 0;
        rbi_heapchk(0);
    }
    if (s_rbi_heap_bad && !rbi_free_running(s_rbc)) rbi_session_end(2); /* W359: sauberes Ende statt Absturz */
    s_rbi_toff[rbi_gtick(s_rbc) & RBI_GM] = s_rec_len;
    rbi_rec_compact();
    if (s_rbs_net_n && s_rbi_ep) rbs_net_tick(); /* W413: Sicherheitsnetz (Live-Tick, nach dem Neurechnen) */
    rbi_poll();
    if (!s_rbi_sent_early) rbi_send(); /* W372: in diesem Tick schon gesendet (frueh) */
    s_rbi_sent_early = 0;
    rbi_finals();
    rbi_state_dump(snap_ok); /* LOC-DESYNC-REPAIR DIAG sdump */
    {
        long long th = rb_now(); /* W469 */
        rbl_tick(); /* W386 */
        rbh_add(RBH_RBL, rb_now() - th);
    }
    if (s_rbi_rbtest_n && s_ring && s_ready && !s_failed && rbr_active() &&
        rbi_gtick(s_rbc) >= s_rbi_rbtest_next) { /* W355 TEST: Neurechnen mit gleichen Eingaben muss bitgleich sein */
        s_rbi_rbtest_next = rbi_gtick(s_rbc) + (u32)s_rbi_rbtest_n; /* einmal je N Live-Ticks (nach dem Neurechnen
                                                                     * steht der Tick wieder gleich) */
        s_rbi_rbtest_cnt++;
        {
            int f = rbi_force_rb(s_rbc, (u32)s_rbi_rbtest_k, rbr_depth());
            s_rbi_rbtest_ok += (u32)f;
            if (f && s_rbv_n && s_rbi_rbtest_ok % (u32)s_rbv_n == 0) s_rbv_arm = 1; /* W359 DIAG */
        }
    }
    {
        long long th = rb_now(); /* W469 */
        rbi_try_rollback();
        rbh_add(RBH_LOAD, rb_now() - th);
    }
    if (s_rbi_tsync == 2 && !rbi_replaying(s_rbc)) { /* W372: Feldzahl statt Stau-Tick (Freilauf: rbi_tsync = 0) */
        int ex = rbi_tsync(s_rbc);
        if (ex > 0) InterlockedExchange(&s_wait_extra, ex);
    } else if (s_rbi_tsync == 1 && !rbi_replaying(s_rbc) && !rbi_free_running(s_rbc)) { /* W366: allein kein Zeitabgleich */
        /* Zeitabgleich: Die Missionsschleife ist an den Bildtakt gebunden (Feld-Warten holt Pausen nach). Wer anhaltend
         * vorne liegt (geglaettet > 0,75 Slots ueber 10 Ticks), laesst einen ganzen Tick ohne Entnahme laufen (Stau wie
         * im Original, die Simulation steht), danach 10 Ticks Abklingzeit. */
        double a = rbi_advantage(s_rbc);
        s_rbi_hud_adv = a; /* W374: F3-Overlay */
        /* W355: bei grossem Vorsprung staerker abbremsen (W338: hoechstens 1 Stau je 11 Ticks = 9 %; im Nutzerlauf lief
         * eine Seite so 8-10 Slots vor, jede Vorhersage war 10 Slots tief). > 1,5 Slots: 1 je 5 Ticks, > 3: 1 je 3. */
        int need = a > 3.0 ? 2 : (a > 1.5 ? 4 : 10), cool = a > 3.0 ? 1 : (a > 1.5 ? 1 : 10);
        if (s_rbi_ts_cool) s_rbi_ts_cool--;
        if (a > 0.75) s_rbi_ts_run++;
        else s_rbi_ts_run = 0;
        if (s_rbi_ts_run >= (u32)need && !s_rbi_ts_cool) {
            s_rbi_ts_stall = 1;
            s_rbi_ts_ms += 50;
            s_rbi_ts_run = 0;
            s_rbi_ts_cool = (u32)cool;
        }
    }
    if (!rbi_replaying(s_rbc)) (void)rb_spec_try_start(snap_ok); /* W390: eigene Lage D + 1 Ticks voraus (nur Anzeige) */
}

static void rbi_patch_jmp(u32 va, const void *dst, unsigned len)
{
    uint8_t *p = (uint8_t *)(uintptr_t)va;
    intptr_t rel = (const uint8_t *)dst - (p + 5);
    DWORD old;
    VirtualProtect(p, len, PAGE_EXECUTE_READWRITE, &old);
    p[0] = 0xE9;
    memcpy(p + 1, &rel, 4);
    if (len > 5) memset(p + 5, 0x90, len - 5);
    VirtualProtect(p, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, len);
}

static void *rbi_tramp(uint8_t *mem, u32 va, unsigned n)
{
    intptr_t rel;
    memcpy(mem, (const void *)(uintptr_t)va, n);
    mem[n] = 0xE9;
    rel = (intptr_t)(va + n) - (intptr_t)(mem + n + 5);
    memcpy(mem + n + 1, &rel, 4);
    return mem;
}

void rbi_install(void)
{
    static const uint8_t e920[6] = {0x8B, 0x44, 0x24, 0x04, 0x8B, 0xC8};       /* mov eax,[esp+4]; mov ecx,eax */
    static const uint8_t ec80[8] = {0x83, 0xEC, 0x20, 0xA1, 0x74, 0x96, 0x28, 0x00}; /* sub esp,20h; mov eax,[289674h] */
    static const uint8_t reg[6] = {0x8B, 0x1D, 0xA0, 0x71, 0x30, 0x00};        /* mov ebx,[3071A0h] */
    if (memcmp((const void *)(uintptr_t)RBI_E920, e920, 6) || memcmp((const void *)(uintptr_t)RBI_EC80, ec80, 8) ||
        memcmp((const void *)(uintptr_t)RBI_REG, reg, 6)) {
        HLOG_E("LOCBYTES LOCRBI @0x%X/0x%X/0x%X: Byte-Mismatch - ABBRUCH (exit 7)", RBI_E920, RBI_EC80, RBI_REG);
        hlog_close();
        ExitProcess(7);
    }
    s_rbc = rbi_new();
    rbi_set_pred(s_rbc, s_rbi_pred, s_rbi_predq); /* W385 */
    s_rbl = rbl_new(); /* W386 */
    s_rbi_tramp = (uint8_t *)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    InitializeCriticalSection(&s_rbi_tq_cs);
    InitializeCriticalSection(&s_rbn_cs);
    {
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        s_rbn = rbn_new(1, (u32)(q.QuadPart ^ (q.QuadPart >> 32) ^ (GetCurrentProcessId() << 16)) | 1u, s_rbi_dmin,
                        s_rbi_dmax, s_rbi_delay);
        if (s_rbn) { /* W447: Netzmodus Auto (loc_net_auto) und Grenzen fuer DIREKT */
            rbn_set_want(s_rbn, 1, g_cfg.loc_net_auto);
            rbn_set_direct(s_rbn, s_auto_drtt, s_auto_djit);
            rbn_set_direct_start(s_rbn, s_auto_start1);
            InterlockedExchange(&s_auto_mode, g_cfg.loc_net_auto ? 0 : 1);
            InterlockedExchange(&s_auto_prof, -1);
            HLOG_I("LOCRBI-AUTO: Netzmodus %s (Grenzen direkt: RTT <= %d ms, Schwankung <= %d ms)",
                   g_cfg.loc_net_auto ? "Auto" : "Rollback (fest)", s_auto_drtt, s_auto_djit);
        }
    }
    if (!s_rbc || !s_rbn || !s_rbi_tramp || !rbi_sock_open()) {
        HLOG_W("LOCRBI: Eingabeschicht nicht verfuegbar - Missionen laufen im Original-Lockstep");
        return;
    }
    o_e920 = (rbi_fn2_t)rbi_tramp(s_rbi_tramp, RBI_E920, 6);
    o_ec80 = (rbi_fn2_t)rbi_tramp(s_rbi_tramp + 32, RBI_EC80, 8);
    rbi_patch_jmp(RBI_E920, (const void *)rbi_e920, 6);
    rbi_patch_jmp(RBI_EC80, (const void *)rbi_ec80, 8);
    rbi_patch_jmp(RBI_REG, (const void *)rbi_reg_thunk, 6);
    /* Akkumulator ab Missionsbeginn (nicht erst beim Ringstart): jeder Slot bekommt seinen Hash, die Ketten beider
     * Seiten beginnen bei Slot 0 */
    if (rb_chain_call(RB_ACC_CALL, (void *)rb_acc_thunk, &s_acc_next, "Akkumulator")) s_rbi_acc_chained = 1;
    s_rbi_s_ticks = rb_ser_new("R2 Rollback-Tiefe (Spielticks)");
    s_rbi_s_slots = rb_ser_new("R2 Rollback-Tiefe (Slots)");
    s_rbi_s_load = rb_ser_new("R2 Ring laden (us)");
    s_rbi_s_rep = rb_ser_new("R2 Neurechnen gesamt (us)");
    s_spec_s_us = rb_ser_new("W390 Vorlauf gesamt (us)");
    s_spec_s_rev = rb_ser_new("W390 Vorlauf zuruecksetzen (us)");
    s_spec_s_pages = rb_ser_new("W390 Vorlauf Seiten");
    s_rbi = 1;
    g_loc_rb_alloc = rb_alloc_cb; /* W346 R3 */
    s_tim_base = rb_now(); /* W372: Zeitbasis der Messung (vor dem Kanal-Thread) */
    if (!s_rbi_hello_ev) s_rbi_hello_ev = CreateEventA(NULL, FALSE, FALSE, NULL); /* W469 */
    s_rbi_thr = CreateThread(NULL, 0, rbi_chan_thread, NULL, 0, NULL); /* W346: Empfang + Hallo */
    HLOG_I("LOCRBI: aktiv (loc_rb_input=1, je Mission ausgehandelt): Entnahme @0x%X, Senden @0x%X, Regelung @0x%X, "
           "Verzoegerung %s%d (Grenzen %d..%d), Fenster %d, Zeitabgleich %d, Kanal-Thread %s", RBI_E920, RBI_EC80, RBI_REG,
           s_rbi_delay > 0 ? "fest " : "adaptiv, Start ", s_rbi_delay > 0 ? s_rbi_delay : 2, s_rbi_dmin, s_rbi_dmax,
           s_rbi_maxpred, s_rbi_tsync, s_rbi_thr ? "laeuft" : "FEHLT");
}

void rbi_log(int waited_s)
{
    const rbi_stats_t *st;
    u32 d50, d95, dmx, s50, s95, smx, r50, r95, rmx, l50, l95, lmx, rbw;
    if (!s_rbi || !s_rbc) return;
    { /* W346 R4: Telemetrie als Zaehlerzeile (nur Zahlen, keine Eingaben) - auch in der Lobby (Aushandlung) */
        rbn_stats_t ns;
        int rtt, fb = 0, k;
        static u32 last_rb;
        u32 a50, a95, amx, t50, t95, tmx, rbw10 = s_rbi_rb_n - last_rb;
        last_rb = s_rbi_rb_n;
        EnterCriticalSection(&s_rbn_cs);
        ns = *rbn_stats(s_rbn);
        rtt = rbn_rtt_max(s_rbn, GetTickCount());
        LeaveCriticalSection(&s_rbn_cs);
        for (k = 1; k < RBN_WHY_N; k++)
            if (k != RBN_WHY_SELF_OFF) fb += (int)ns.fallback[k];
        rbi_pct(s_rbi_s_ticks, &t50, &t95, &tmx);
        rbi_pct(s_rbi_s_rep, &a50, &a95, &amx);
        HLOG_I("LOCRBI-TEL: t=%ds Modus=%s Verz=%d RTT=%d ms | Rollbacks/s=%.2f gesamt=%u Tiefe p50/p95/max=%u/%u/%u Ticks "
               "Neurechnen p95=%u us max=%u | Missionen R2=%u Lockstep=%u Rueckfaelle=%d (letzter Grund %u) Ende Desync=%u "
               "uneinig=%u | Ton doppelt=%u nachgeholt=%u verwaist=%u | Neuaufbau aufgeschoben=%u sauber=%u Barrieren=%u | "
               "Neurechnen Allokation=%u neu=%u Senden live=%u | lange Ticks=%u max=%u ms | Hallo tx=%u rx=%u schlecht=%u voll=%u",
               waited_s,
               s_rbi_mst == RBN_ST_R2 ? (rbi_free_running(s_rbc) ? "R2-Ende" : "R2")
                                      : (s_rbi_mst == RBN_ST_LS ? "Lockstep"
                                                                : (s_rbi_mst == RBN_ST_TENT ? "Angebot"
                                                                                            : (s_rbi_mst == RBN_ST_PEND ? "Warten" : (s_rbi_mst == RBN_ST_PRE ? "Vorab" : "Lobby")))),
               s_rbi_ep ? (int)s_rbi_reg_d : 0, rtt, rbw10 / 10.0, s_rbi_rb_n, t50, t95, tmx, a95, amx, s_rbi_r2_missions,
               s_rbi_ls_missions, fb, s_rbi_last_why, s_rbi_end_desync, s_rbi_end_disagree, s_xc_dup, s_xc_new, s_xc_orphan,
               s_rbi_defer_n, s_rbi_defer_clean, s_rbi_bar, s_rbi_alloc_rep, s_rbi_alloc_new, s_rbi_send_live, s_rbi_lt_n,
               s_rbi_lt_max, ns.tx, ns.rx_ok, ns.rx_bad, ns.rx_full);
        { /* W385: Vorhersage-Vergleich (nur Zaehler) und Lagen-Spruenge je Rollback */
            const rbi_stats_t *w = rbi_stats(s_rbc);
            HLOG_I("LOCRBI-PRED: t=%ds Modus=%d q=%d | Vorhersagen=%u richtig=%u falsch=%u Trend wirksam=%u | nur Trend richtig=%u "
                   "nur Wiederholung richtig=%u | Achsenfehler falsch Summe=%u (Wiederholung %u) | falsch nach Feld Achsen=%u "
                   "Ausweichen=%u Pedale=%u Gang=%u Tasten-B0=%u Tasten=%u Kopf=%u",
                   waited_s, rbi_pred_mode(s_rbc), s_rbi_predq, w->predicted, w->pred_ok, w->pred_bad, w->pred_trend,
                   w->pred_gain, w->pred_loss, w->mis_ax_err, w->mis_ax_err_rep, w->mis_cat[RBI_MIS_AXIS],
                   w->mis_cat[RBI_MIS_DODGE], w->mis_cat[RBI_MIS_PEDAL], w->mis_cat[RBI_MIS_GEAR], w->mis_cat[RBI_MIS_FIRE],
                   w->mis_cat[RBI_MIS_BTN], w->mis_cat[RBI_MIS_HDR]);
            loc_smooth_log(waited_s);
        }
        if (g_cfg.loc_own_predict && (s_spec_runs || s_spec_skip[0] + s_spec_skip[1] + s_spec_skip[2] + s_spec_skip[6])) {
            /* W390: Kosten und abgefangene Nebenwirkungen des Vorlaufs (nur Zaehler/Zeiten) */
            u32 u50, u95, umx, v50, v95, vmx, g50, g95, gmx;
            rbi_pct(s_spec_s_us, &u50, &u95, &umx);
            rbi_pct(s_spec_s_rev, &v50, &v95, &vmx);
            rbi_pct(s_spec_s_pages, &g50, &g95, &gmx);
            HLOG_I("LOCOWNPRED-COST: t=%ds Vorlaeufe=%u Ticks=%u frueh beendet=%u | Kosten je Tick p50/p95/max=%u/%u/%u us "
                   "Zuruecksetzen=%u/%u/%u us Seiten=%u/%u/%u | ausgelassen ohne-Sichern=%u Neuaufbau=%u Render=%u Bildtakt=%u "
                   "Pause=%u Lage=%u sonst=%u | Stufe=%d (weniger %u) Pausen=%d aus=%d Ruecksetz-Fehler=%u andere-Stelle=%u | abgefangen Senden=%u select=%u recv=%u "
                   "ioctl=%u XInput live/wieder=%u/%u (Live-Tick uebernommen=%u ungenutzt=%u) XACT Start=%u Stopp=%u Meldung=%u "
                   "Hash=%u Rahmen=%u (vorhergesagt %u) eigene=%u | Kernel Lesen=%u Schreiben=%u Schliessen=%u Datei-Info=%u "
                   "Freigabe=%u Positionen=%u | Belegung=%u (uebernommen %u Leck %u) Commit=%u Register+=%u Schutz=%u | "
                   "Pruefung %u/%u abweichend",
                   waited_s, s_spec_runs, s_spec_ticks, s_spec_early, u50, u95, umx, v50, v95, vmx, g50, g95, gmx,
                   s_spec_skip[0], s_spec_skip[1], s_spec_skip[2], s_spec_skip[3], s_spec_skip[4], s_spec_skip[5],
                   s_spec_skip[6], s_spec_level, s_spec_degr, s_spec_paused_n, s_spec_off_mission, s_spec_rev_fail, s_spec_ctx_bad, s_spec_ev.send, s_spec_ev.sel,
                   s_spec_ev.recv, s_spec_ev.ioctl, s_spec_ev.xin_live, s_spec_ev.xin_rep, s_spec_xc_hit, s_spec_xc_unused,
                   s_spec_ev.xcue, s_spec_ev.xstop, s_spec_ev.xgn, s_spec_ev.acc, s_spec_ev.take, s_spec_ev.take_pred,
                   s_spec_ev.ec80, s_spec_ev.kfile[1], s_spec_ev.kfile[2], s_spec_ev.kfile[3], s_spec_ev.kfile[4],
                   s_spec_ev.kfile[5], s_spec_files, s_spec_ev.alloc, s_spec_ac_hit, s_spec_ac_leak, s_spec_ev.commit,
                   s_spec_reg_grow, s_spec_memev, s_spec_verify_bad, s_spec_verify_n);
            if (g_cfg.syscall_trace == 2) { /* W390 DIAG: Kernel-Aufrufe im Vorlauf (Ordinal, Name, Anzahl; live zum Vergleich) */
                char b[1024];
                int o, l = 0;
                for (o = 0; o < 512 && l < (int)sizeof b - 64; o++)
                    if (s_sc_spec[o] || s_sc_live[o])
                        l += _snprintf_s(b + l, sizeof b - (size_t)l, _TRUNCATE, " %d:%s=%u/%u", o, krnl_name((unsigned)o),
                                         s_sc_spec[o], s_sc_live[o]);
                HLOG_I("LOCOWNPRED-SC: t=%ds Kernel Vorlauf/live:%s", waited_s, l ? b : " -");
            }
        }
        if (s_xc_orphan_stop) /* W375 (#51): eigene Zeile (geparste Zeilen unveraendert) */
            HLOG_I("LOCRBI-TEL3: t=%ds Ton verwaist=%u davon Schleife gestoppt=%u", waited_s, s_xc_orphan, s_xc_orphan_stop);
        if (s_rbs_side_stop || s_rbs_gap_stop || s_rbs_net_stop || s_rbs_rec_dead) /* W413 (#65): eigene Zeile */
            HLOG_I("LOCRBI-TEL5: t=%ds Ton-Abgleich nach dem Neurechnen: nachgeholte Waisen gestoppt=%u Stopp-Luecken gestoppt=%u "
                   "Sicherheitsnetz=%u Waisen tot markiert=%u", waited_s, s_rbs_side_stop, s_rbs_gap_stop, s_rbs_net_stop,
                   s_rbs_rec_dead);
        if (g_cfg.loc_sndloop_diag) { /* W413 DIAG: noch spielende Schleifen und Zaehler je Luecke (auch ohne Fix) */
            char b[600];
            int na = loc_audio_loops_active(b, (int)sizeof b);
            u32 tch = 0, i;
            for (i = 0; i < s_rbs_net_n; i++) tch += s_rbs_net[i].touched;
            HLOG_I("LOCSNDLOOP: t=%ds Fix=%d aktive Schleifen=%d [%s] | verwaist Aufzeichnung=%u (Schleife %u, gestoppt %u, tot %u) "
                   "nachgeholt=%u (Schleife %u, gestoppt %u) | Stopp-Luecken=%u (Schleife %u, gestoppt %u) | Netz verfolgt=%u "
                   "beruehrt=%u gestoppt=%u wuerde=%u voll=%u Ablage-leer-unberuehrt=%u | Vorlauf-Starts unterdrueckt=%u",
                   waited_s, g_cfg.loc_sndloop_fix, na, b, s_xc_orphan, s_rbs_rec_loop, s_xc_orphan_stop, s_rbs_rec_dead,
                   s_rbs_side_orphan, s_rbs_side_loop, s_rbs_side_stop, s_rbs_gap, s_rbs_gap_loop, s_rbs_gap_stop, s_rbs_net_n,
                   tch, s_rbs_net_stop, s_rbs_net_would, s_rbs_net_drop, s_rbs_net_umiss, s_spec_ev.xcue);
        }
        if (s_xc_stop_live) /* W381 (#61): eigene Zeile */
            HLOG_I("LOCRBI-TEL4: t=%ds Ton-Stopps beim Neurechnen live=%u (Schluessel %s)", waited_s, s_xc_stop_live,
                   s_xkey_legacy ? "alt" : "mit Handle/Quelle");
        if (s_rbc) { /* W355: Zusatzzeile (nur Zahlen) */
            const rbi_stats_t *w = rbi_stats(s_rbc);
            HLOG_I("LOCRBI-TEL2: t=%ds Senden gleich=%u live=%u | Neuaufbau aufgeschoben=%u davon Neurechnung=%u erzwungen=%u "
                   "Rollback ab Allokation=%u | verloren=%u hingenommen=%u | Hash verglichen=%u abweichend=%u erster=%d "
                   "Neurechnung-anders=%u | Allokation aufgez.=%u bedient=%u fehlt=%u ungueltig=%u fremd=%u neu=%u | rbtest %u/%u | "
                   "Senden Neurechnen %u exakt=%u ohne-Socket=%u andere-Laenge=%u | Datagramme Neurechnen %u gleich=%u | "
                   "Commit aufgez.=%u bedient=%u fehlt=%u neu=%u offen=%u voll=%u gleich=%u verworfen=%u | "
                   "Heap-Pruefung %u Knoten max=%u unvollst.=%u Zeit max=%u us Fehler=%u Ende=%u ausserhalb=%u belegt=%u",
                   waited_s, s_rbi_send_same, s_rbi_send_live, w->rebuild_deferred, w->rebuild_defer_replay,
                   w->rebuild_forced, w->rb_floored, w->rb_lost, w->rb_accepted, w->hash_cmp, w->hash_bad, w->hash_bad_first,
                   w->hash_recalc_diff, s_rbi_alloc_rec, s_rbi_alloc_rep, s_rbi_alloc_miss, s_rbi_alloc_nocommit,
                   s_rbi_alloc_other, s_rbi_alloc_new, s_rbi_rbtest_ok, s_rbi_rbtest_cnt, s_rbi_send_rep, s_rbi_send_exact,
                   s_rbi_send_nosock, s_rbi_send_otherlen, s_rbi_sendto_rep, s_rbi_sendto_same, s_rbi_com_rec, s_rbi_com_rep, s_rbi_com_miss, s_rbi_com_new,
                   s_pcom_n, s_pcom_full, s_rbi_com_same, s_rbi_com_drop, s_rbi_hc_n, s_rbi_hc_nodes_max, s_rbi_hc_part,
                   s_rbi_hc_us_max, s_rbi_hc_bad, s_rbi_end_heap, s_rbi_hc_vq, s_rbi_hc_busy);
            rbv_log(waited_s); /* W359 DIAG */
        }
        s_rbi_lt_max = 0;
    }
    if (!s_rbi_r2_missions) return;
    st = rbi_stats(s_rbc);
    rbi_pct(s_rbi_s_ticks, &d50, &d95, &dmx);
    rbi_pct(s_rbi_s_slots, &s50, &s95, &smx);
    rbi_pct(s_rbi_s_rep, &r50, &r95, &rmx);
    rbi_pct(s_rbi_s_load, &l50, &l95, &lmx);
    rbw = s_rbi_rb_n - s_rbi_rb_last;
    s_rbi_rb_last = s_rbi_rb_n;
    HLOG_I("LOCRBI: t=%ds Mission=%u aktiv=%d Slot=%d Verz=%d P0=%d | Entnahmen=%u bestaetigt=%u Vorhersagen=%u richtig=%u "
           "falsch=%u | Rollbacks=%u +%u/10s Tiefe p50/p95/max=%u/%u/%u Ticks %u/%u/%u Slots Neurechnen p50/p95/max=%u/%u/%u us "
           "Laden=%u/%u/%u us verloren=%u hingenommen=%u Ladefehler=%u Ruhe-Fehler=%u Barrieren=%u | Stau Erst=%u Fenster=%u Tiefe=%u | "
           "Hash endgueltig=%u verglichen=%u abweichend=%u erster=%d Neurechnung-anders=%u | Kanal tx=%u/%u rx=%u/%u "
           "doppelt=%u schlecht=%u fremd=%u Bindung=%u Epoche=%u Konflikt=%u Sendefehler=%u Warteschlange-voll=%ld | Zeitabgleich=%.2f "
           "Wartezeiten=%u/%u ms | Aufzeichnung=%u KB Kompakt=%u Aufruf-Abw=%u Kanalverlust-Test=%u",
           waited_s, s_rbi_epoch, s_rbi_ep, rbi_cons(s_rbc, rbi_self(s_rbc)), rbi_delay_eff(s_rbc), rbi_p0(s_rbc), st->taken,
           st->taken_conf, st->predicted, st->pred_ok, st->pred_bad, s_rbi_rb_n, rbw, d50, d95, dmx, s50, s95, smx, r50, r95,
           rmx, l50, l95, lmx, st->rb_lost, st->rb_accepted, s_rbi_load_fail, s_rbi_qfail, s_rbi_bar, st->stall_first, st->stall_window,
           st->stall_depth, st->hash_final, st->hash_cmp, st->hash_bad, st->hash_bad_first, st->hash_recalc_diff,
           st->tx_pkts, st->tx_frames, st->rx_ok, st->rx_frames, st->rx_dup, st->rx_bad, s_rbi_peer_bad, st->rx_bind,
           st->rx_epoch, st->rx_conflict, s_rbi_tx_err, s_rbi_tq_drop, rbi_advantage(s_rbc), s_rbi_ts_waits, s_rbi_ts_ms, s_rec_len / 1024u,
           s_rbi_compact, s_rep_div, s_rbi_chanloss_n);
    { /* W372: Zeitleiste und Puffer im 10-s-Fenster */
        u32 wh[RBI_SLH], wn = st->slack_n - s_tim_sln, wl = st->slack_late - s_tim_sll;
        int k, smed = 0;
        for (k = 0; k < (int)RBI_SLH; k++) {
            wh[k] = st->slack_h[k] - s_tim_slh[k];
            s_tim_slh[k] = st->slack_h[k];
        }
        s_tim_sln = st->slack_n;
        s_tim_sll = st->slack_late;
        HLOG_I("LOCRBI-TIM: t=%ds Puffer ms p5/p25/p50/p95=%d/%d/%d/%d spaet=%u von %u (%.1f%%) gesamt p5/p50=%d/%d "
               "spaet=%u von %u | Tick ms nach Feldwechsel: eigener Rahmen p50/p95=%d/%d Fremd-Entnahme p50/p95=%d/%d "
               "eigener vor Entnahme=%u von %u | Rahmen->Senden ms p50/p95=%d/%d | Vorsprung min/mittel/max=%.2f/%.2f/%.2f "
               "Stau-Ticks Zeitabgleich=%u | Zeitabgleich %d: Vorsprung %.1f ms (Quelle %d) Feld+ %u mal/%u Felder (Thunk %u/%u) "
               "Puffer-Median %d ms SBRT tx/rx/schlecht=%u/%u/%u | Feldbezug nach Neurechnen anders %u von %u (%s)",
               waited_s, rbi_slack_pct(wh, wn, 5), rbi_slack_pct(wh, wn, 25), rbi_slack_pct(wh, wn, 50),
               rbi_slack_pct(wh, wn, 95), wl, wn, wn ? 100.0 * wl / wn : 0.0, rbi_slack_pct(st->slack_h, st->slack_n, 5),
               rbi_slack_pct(st->slack_h, st->slack_n, 50), st->slack_late, st->slack_n, tim_pct(s_tim_h_own, 50),
               tim_pct(s_tim_h_own, 95), tim_pct(s_tim_h_take, 50), tim_pct(s_tim_h_take, 95), s_tim_own_first, s_tim_ticks,
               tim_pct(s_tim_h_send, 50), tim_pct(s_tim_h_send, 95), s_tim_adv_min,
               s_tim_adv_n ? s_tim_adv_sum / s_tim_adv_n : 0.0, s_tim_adv_max, s_rbi_ts_waits, s_rbi_tsync,
               rbi_tsync_ms(s_rbc), rbi_tsync_src(s_rbc), st->ts_actions, st->ts_fields, s_rbi_wait_ext_n, s_rbi_wait_ext_f,
               rbi_slack_med(s_rbc, rbi_self(s_rbc) == 0 ? 1 : 0, &smed) ? smed / 10 : -999, st->tt_tx, st->tt_rx, st->tt_bad,
               s_rbi_fld_diff, s_rbi_fld_n, s_rbi_fldfix ? "zurueckgesetzt" : "nur gezaehlt");
        /* W386: Abtastung der eigenen Eingabe (nur Zeiten): SBC-Abfrage nach Feldwechsel, Abfrage -> eigener Rahmen
         * (Rahmen -> Senden steht in LOCRBI-TIM), Ticks ohne bzw. mit mehreren Abfragen, Abfrage im Vortick. */
        HLOG_I("LOCRBI-LAT: t=%ds SBC-Abfrage ms nach Feldwechsel p50/p95=%d/%d | Abfrage->eigener Rahmen ms p50/p95/max=%d/%d/%d | "
               "Ticks=%u ohne Abfrage=%u mehrere=%u Abfrage im Vortick=%u | D=%u",
               waited_s, tim_pct(s_lat_h_xin, 50), tim_pct(s_lat_h_xin, 95), tim_pct(s_lat_h_x2o, 50),
               tim_pct(s_lat_h_x2o, 95), tim_pct(s_lat_h_x2o, 100), s_lat_ticks, s_lat_none, s_lat_multi, s_lat_before,
               s_rbi_ep ? s_rbi_reg_d : 0u);
        memset(s_lat_h_xin, 0, sizeof s_lat_h_xin);
        memset(s_lat_h_x2o, 0, sizeof s_lat_h_x2o);
        s_lat_ticks = s_lat_none = s_lat_multi = s_lat_before = 0;
        memset(s_tim_h_own, 0, sizeof s_tim_h_own);
        memset(s_tim_h_take, 0, sizeof s_tim_h_take);
        memset(s_tim_h_send, 0, sizeof s_tim_h_send);
        s_tim_own_first = s_tim_ticks = 0;
        s_tim_adv_n = 0;
        s_tim_adv_sum = 0;
    }
    HLOG_I("LOCRBI: DIAG Startpunkt %d/%d (Wahl %d/%d) Zaehler %u Aktiv 0x%X Spieltick %u Stau %u | eigene Rahmen %u (doppelt %u, "
           "mehrere je Tick %u) Flags eigen 0x%X fremd 0x%X Entnahmezaehler %u/%u", (int)rd32(0x5C5A44u),
           (int)rd32(0x5C5A44u + 0xAE8u), (int)rd32(0x5C5A48u), (int)rd32(0x5C5A48u + 0xAE8u), rd32(0x5C5A40u),
           rd32(RB_ACTIVE), rd32(RB_TICK), rd32(RB_STALL), st->own_frames, st->own_dup, s_rbi_dmulti, s_rbi_dflags[0],
           s_rbi_dflags[1], rd32(RBI_TAKENW) & 0xFFFFu, rd32(RBI_TAKENW) >> 16);
    HLOG_I("LOCRBI: DIAG Eingabestrom je Spieler (kein Zustandshash; Rechner vergleichen: gleiche Werte = gleiche "
           "Eingaben) p0 %u/%08X p1 %u/%08X | Spieler0 522=%u 98=0x%X 8c=%u 92=%u | Spieler1 522=%u 98=0x%X 8c=%u 92=%u | "
           "lokal %u netz %u", s_rbi_dn[0], s_rbi_dstream[0], s_rbi_dn[1], s_rbi_dstream[1],
           rd32(0x5C50D0u + 0x522u) & 0xFFu, rd32(0x5C50D0u + 0x98u), rd32(0x5C50D0u + 0x8Cu) & 0xFFFFu,
           rd32(0x5C50D0u + 0x92u) & 0xFFFFu, rd32(0x5C50D0u + 0xAE8u + 0x522u) & 0xFFu, rd32(0x5C50D0u + 0xAE8u + 0x98u),
           rd32(0x5C50D0u + 0xAE8u + 0x8Cu) & 0xFFFFu, rd32(0x5C50D0u + 0xAE8u + 0x92u) & 0xFFFFu, rd32(0x306A84u), rd32(RBI_SELF));
    {
        int k;
        for (k = 0; k < 10; k++)
            if (s_rbi_dchk[0][k] || s_rbi_dchk[1][k])
                HLOG_I("LOCRBI: DIAG Eingabestrom bis Slot %d Spieler0=%08X Spieler1=%08X Zaehler %u Spieltick %u", (k + 1) * 25, s_rbi_dchk[0][k],
                       s_rbi_dchk[1][k], s_rbi_dcnt[k] & 0xFFFFu, s_rbi_dcnt[k] >> 16);
    }
    (void)s_rbi_win_sum;
    (void)s_rbi_win_n;
}

