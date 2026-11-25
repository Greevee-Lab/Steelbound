/* loc_rb_int.h - W464 NET-CORE: gemeinsamer interner Zustand der Rollback-Anbindung (nur loc_rollback*.c / loc_rb_*.c).
 * Includes, LoC-Adressen, Konstanten und Typen wie bisher in loc_rollback.c; darunter der modulfremd genutzte
 * Datei-Zustand (extern) und die modulfremd genutzten Funktionen. Modulkarte (BERICHT_M2 W464):
 *   loc_rollback.c   Grundzustand, Threads/Ruhepunkt/Anhalten, Zyklen, Haken vor/nach dem Feld-Warten,
 *                    Missionsschleifen-Ende, Optionen, Installation, Statuslog, oeffentliche Funktionen
 *   loc_rb_snap.c    Messreihen, Gast-Speicherbereiche, Zustandsring (Sichern/Laden), Diff, Schreibschutz-Messung
 *   loc_rb_rec.c     Aufzeichnung/Wiedergabe der Spiel-Aufrufe (Netz, XInput, XACT), Ton-Abgleich, HLE-Zensus
 *   loc_rb_input.c   Eingabeschicht R2 (Kanal, Zeitbasis, Latenzregler, Netzmodus Auto, Aushandlung je Mission,
 *                    Installation/Statuslog der Eingabeschicht)
 *   loc_rb_resim.c   Neurechnen/Resync: Gegenprobe rbver, Heap-Pruefung, Rollback ausloesen, Missions-/Sitzungsende
 *   loc_rb_spec.c    eigener spekulativer Vorlauf (W390, nur Anzeige) */
#ifndef LOC_RB_INT_H
#define LOC_RB_INT_H
#include <winsock2.h> /* W338: vor windows.h (Eingabekanal) */
#include <windows.h>

#include "loc_rollback.h"
#include "loc_lsarb_game.h" /* LOC-LOCKSTEP-ARBITER */
#include "hcfg.h"
#include "hlog.h"
#include "hooks_int.h"
#include "kernel_hle.h"
#include "loc_pad.h"
#include "loc_rb_ring.h" /* W330 LOC-ROLLBACK-R1 */
#include "loc_mem.h"
#include "d3d11_render.h" /* W330: sb_interp_rb_pause */
#include "loc_rbi.h"       /* W338 LOC-ROLLBACK-R2 */
#include "loc_net.h"
#include "loc_net_core.h"
#include "loc_netsim.h"
#include "loc_tun.h"
#include "loc_sync.h"
#include "loc_rbn.h"       /* W346 LOC-ROLLBACK-R4: Aushandlung */
#include "loc_audio.h"     /* W375: Rollback-Waisen (Schleifen-Cues) stoppen */
#include "loc_rb_xkey.h"    /* W381 (#61): Stopp-/Status-Schluessel mit Handle bzw. Quelle */
#include "loc_rb_sndloop.h" /* W413 (#65): Ton nach dem Neurechnen mit der korrigierten Zeitlinie abgleichen */
#include "loc_guard.h"
#include "sb_lang.h"
#include "sb_origview.h"   /* W346: Einblendung bei Desync */
#include "loc_aim.h"       /* W376 LOC-LOCAL-AIM */
#include "loc_smooth.h"    /* W385 LOC-SMOOTH */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <intrin.h> /* W330: __rdtsc (CPU-Zeit-Eichung) */

typedef uint32_t u32;

/* ---- LoC-VAs (gelesen, Ghidra/capstone) ---- */
#define RB_CALL_VA 0x2F2F7u   /* push 3; call FUN_00033050 */
#define RB_WAIT_FN 0x33050u
#define RB_ACC_CALL 0x1D391u  /* CALL FUN_000C9400 in FUN_0001D010 (Akkumulator lesen+loeschen) */
#define RB_ACC_FN 0xC9400u
#define RB_ACC 0x6D0C58u
#define RB_TICK 0x3070F4u
#define RB_STALL 0x3069C0u
#define RB_ACTIVE 0x306830u
#define RB_RJOB 0x306EECu     /* Render-Auftrag: 0 frei, 1 zeichnen, 2 Ende */
#define RB_RSKIP 0x30694Cu    /* Bilder ohne Render-Vorbereitung (Render-Thread noch beschaeftigt) */
#define RB_DESYNC 0x306790u
#define RB_RTHREAD 0x2C580u   /* Start des Spiel-Render-Threads */
#define RB_XIN_VA 0x1E4BB2u   /* XInputGetState (LoC) */
#define RB_SELECT 0x17081Cu
#define RB_RECV 0x170845u
#define RB_RECVFROM 0x17085Fu
#define RB_SEND 0x17086Eu
#define RB_SENDTO 0x170888u
#define RB_IOCTL 0x1707D6u
#define RB_WSAERR 0x170CD1u
#define RB_XIN_LEN (4u + (u32)sizeof(sbc_in_t)) /* XINPUT_STATE des SBC (0x1C) */


/* ---- Konstanten und Typen der Module (W464: in Originalreihenfolge hierher gehoben) ---- */
#define RB_SER_N 65536
typedef struct { const char *name; u32 n; u32 v[RB_SER_N]; } rb_ser_t;
#define RB_PG 4096u
#define RB_RMAX 4096
typedef struct { u32 va, len, kind; size_t off; DWORD prot; u32 ab; } rb_reg_t; /* kind: 0 Abbild, 1 Contig/Ex, 2 NtAlloc, 5 Pool, 6 TLS, 7 Kernel-Stack */
typedef struct { u32 a, b, kind; } rb_iv_t;
#define RB_EXMAX 320
#define RB_HSMAX 256
#define RB_WIN_N 4096
typedef struct { u32 n; u32 v[RB_WIN_N]; } rb_win_t;
#define RB_VQC 64
#define RB_DIFF_ADDR 64
#define RB_AH 256
typedef struct { u32 page, first, n; } rb_ah_t;
enum { EV_SELECT = 1, EV_RECV, EV_RECVFROM, EV_SEND, EV_SENDTO, EV_IOCTL, EV_WSAERR, EV_XIN, EV_ACC,
       EV_XGN, EV_XPLAY, EV_XPREP, EV_XSTOP, EV_XSTAT, EV_XSTOPSRC, EV_XWORK, EV_NTALLOC, EV_NTCOMMIT, EV_N };
typedef struct { uint8_t fn, pad; uint16_t n; u32 ret, ih; } rb_ev_t; /* ih = Hash der Eingangsargumente */
#define RB_REC_CAP (4u << 20)
enum { PH_IDLE = 0, PH_RECORD, PH_REPLAY };
typedef struct {
    u32 send, sel, recv, ioctl, wsa, xin_live, xin_rep, xgn, xcue, xstop, acc, alloc, commit, take, take_pred, ec80;
    u32 kfile[6]; /* loc_rb_spec_note: 1 Lesen, 2 Schreiben, 3 Schliessen, 4 Datei-Info, 5 Freigabe */
} rb_spec_ev_t;
#define RB_SPEC_XC 4
typedef u32(__stdcall *fn2_t)(u32, u32);
typedef u32(__stdcall *fn3_t)(u32, u32, u32);
typedef u32(__stdcall *fn4_t)(u32, u32, u32, u32);
typedef u32(__stdcall *fn5_t)(u32, u32, u32, u32, u32);
typedef u32(__stdcall *fn6_t)(u32, u32, u32, u32, u32, u32);
typedef u32(__stdcall *fn0_t)(void);
#define WSAEWOULDBLOCK_X 10035u
#define RB_XGN_VA 0x1C0CAAu
#define RB_XPLAY_VA 0x1C0DC0u
#define RB_XPREP_VA 0x1C0D8Du
#define RB_XSTOP_VA 0x1C0DF3u
#define RB_XSTAT_VA 0x1C0EE9u
#define RB_XSTOPSRC_VA 0x1C0EE4u
#define RB_XWORK_VA 0x1C08C9u
typedef u32(__stdcall *fn1_t)(u32);
#define RB_XSIDE 256
typedef struct { u32 g, ih, ret, h, serial; uint8_t fn, used, live; } rb_xside_t; /* W413: serial = laufende Cue-Nummer */
#define RBS_NET 64
typedef struct { u32 serial, h, pp; uint16_t miss; uint8_t touched, origin; } rbs_net_t; /* origin 0 Aufzeichnung, 1 nachgeholt */
#define RBS_LOG(...)                                                                                                 \
    do {                                                                                                             \
        if (g_cfg.loc_sndloop_diag && InterlockedIncrement(&s_rbs_log_n) <= 400) HLOG_I(__VA_ARGS__);                \
    } while (0)
#define RB_ACC_MAX 64
#define RB_HC_MAX 1024
#define RB_END_CNT 0x3068E0u
#define RB_END_LIM 0x306C10u
#define RBI_E920 0x4E920u
#define RBI_EC80 0x4EC80u
#define RBI_REG 0x1D0BFu
#define RBI_STEPS 0x306944u
#define RBI_DELAYV 0x306F04u
#define RBI_PREFILL 0x2C9B2Cu
#define RBI_SELF 0x3071B0u
#define RBI_NPL 0x3071A0u
#define RBI_TAKENW 0x309474u
#define RBI_SENTW 0x309368u
#define RBI_CHAN_GP 1260u /* Spielport-Abbild des Kanals (Basis + 260); Spiel nutzt 1000 und 1201-1203, 1200 = Doppelstart-Schutz */
#define RBI_GM (RBI_GH - 1u)
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#define RBI_FIELD_LAST 0x306F8Cu
typedef u32(__cdecl *rbi_fn2_t)(u32, u32);
#define RBI_TQ 64
#define RBI_TH 128
#define RBI_HUD_N 128
#define RBL_RPW 256
#define RB_PCOM_MAX 64
#define RB_SPEC_AC 16
#define RB_SPEC_CM 16
#define RBV_H 384
typedef struct { u32 page, n, words, first; } rbv_h_t;
#define CSN_N 48
#define RB_SPEC_MAXT 9
#define RB_SPEC_WIN 100
#define RB_LX_VA 0x2F352u

/* W464: vorher anonyme struct-Typen modulfremd genutzter Datei-Variablen (s_ah, s_spec_xc, s_pcom, s_spec_ac, s_spec_cm) */
struct rb_ah1_s { u32 page, first, n; };
struct rb_spec_xc_s { u32 h, ret, n, used; uint8_t d[32]; };
struct rb_pcom_s { u32 a, end, live; };
struct rb_spec_ac_s { u32 ih, a, size, gt; };
struct rb_spec_cm_s { u32 a, end; };

/* ---- externe Deklarationen, die mehrere Module brauchen ---- */
int fs_emu_tls_range(unsigned *va, unsigned *len);           /* fs_emu.c (W330) */
int loc_kstack_get(unsigned i, unsigned *va, unsigned *len); /* krnl_loc.c (W330) */
extern unsigned g_exreg_n;
extern xbe_image_t *g_img; /* kernel_hle.c */
extern void (*volatile g_loc_cs_note)(u32 cs, u32 ret);

/* ---- Zustand aus loc_rb_snap.c ---- */
extern rb_reg_t s_reg[RB_RMAX];
extern int s_nreg;
extern size_t s_total;
extern unsigned s_reg_gen;
extern uint8_t *s_snapA, *s_snapB, *s_snapC;
extern int s_stkx, s_abchk;
extern rb_ser_t *s_r_commit, *s_r_commit_rep, *s_r_total, *s_r_dirty, *s_r_saved, *s_r_faults, *s_r_fault_us, *s_r_ringload, *s_r_ldpages, *s_r_ww, *s_r_prot, *s_r_copy, *s_r_setup;
extern u32 s_r_n, s_r_ovf, s_r_drop, s_r_rebuild, s_r_shadow_bad, s_r_ld_bad, s_r_ld_n, s_r_pd_fail, s_r_abort;
extern rb_ser_t *s_r_cpu;
extern rb_win_t s_w_total, s_w_commit, s_w_saved, s_w_dirty, s_w_faults;
extern u32 s_r_hot_last, s_r_cooled, s_r_foreign_win;
extern int s_avlog;
extern int s_wpbench; /* W469 DIAG loc_rb_opts wpbench=1: Mikromessung Schreibschutz beim ersten Aufbau (Standard aus) */
extern HANDLE s_avlog_h;
extern volatile LONG s_avlog_n;
extern volatile LONG s_avlog_arm;
extern int s_r_force_rebuild;
extern u32 s_diff_addr[RB_DIFF_ADDR];
extern unsigned s_ndiff_addr;
extern struct rb_ah1_s s_ah[RB_AH];
extern unsigned s_nah;
extern rb_ah_t s_ah2[RB_AH];
extern unsigned s_nah2;

/* ---- Zustand aus loc_rb_rec.c ---- */
extern const char *const k_evname[];
extern uint8_t *s_rec;
extern u32 s_rec_len, s_rec_cur;
extern u32 s_rec_events;
extern volatile int s_phase;
extern volatile int s_spec;
extern rb_spec_ev_t s_spec_ev;
extern int s_spec_pd;
extern struct rb_spec_xc_s s_spec_xc[RB_SPEC_XC];
extern u32 s_spec_xcn, s_spec_xc_hit, s_spec_xc_unused;
extern uint8_t s_spec_own_new[22];
extern int s_spec_own_have;
extern int s_spec_paused_n, s_spec_off_mission;
extern u32 s_rb_esp0, s_rb_caller, s_spec_esp0, s_spec_caller, s_spec_ctx_bad;
extern u32 s_spec_degr;
extern int s_spec_level;
extern DWORD s_spec_pause_until;
extern u32 s_spec_win_n;
extern u32 s_nt_rec, s_nt_rep;
extern u32 s_rep_div;
extern u32 s_rep_div_fn, s_rep_div_at;
extern u32 s_cnt_ev[EV_N][2];
extern int s_rbi_ep;
extern u32 s_rep_kc[EV_N];
extern u32 s_rep_end;
extern u32 s_rbi_send_live;
extern fn5_t o_select;
extern fn4_t o_recv, o_send;
extern fn6_t o_recvfrom, o_sendto;
extern fn3_t o_ioctl;
extern fn0_t o_wsaerr;
extern fn2_t o_xin;
extern u32 s_rbi_toff[RBI_GH];
extern rbi_t *s_rbc;
extern u32 s_rbi_send_same;
extern u32 s_rbi_sendto_rep, s_rbi_sendto_same;
extern u32 s_rbi_send_rep, s_rbi_send_exact, s_rbi_send_nosock, s_rbi_send_otherlen, s_rbi_send_logged;
extern long long s_lat_xin_q;
extern u32 s_lat_xin_n;
extern int s_xact;
extern u32 s_xc_dup, s_xc_new, s_xc_orphan;
extern u32 s_xc_orphan_stop;
extern int s_xkey_legacy;
extern u32 s_xc_stop_live;
extern fn3_t o_xgn, o_xplay, o_xprep;
extern fn4_t o_xstop;
extern fn2_t o_xstat;
extern fn1_t o_xstopsrc;
extern fn0_t o_xwork;
extern rb_xside_t s_xside[RB_XSIDE];
extern u32 s_xside_n;
extern u32 s_xwin0, s_xwin1, s_xwin_g0, s_xwin_g1;
extern rbs_net_t s_rbs_net[RBS_NET];
extern u32 s_rbs_net_n, s_rbs_net_drop, s_rbs_net_stop, s_rbs_net_would, s_rbs_net_umiss;
extern u32 s_rbs_rec_loop, s_rbs_rec_dead;
extern u32 s_rbs_side_orphan, s_rbs_side_loop, s_rbs_side_stop;
extern u32 s_rbs_gap, s_rbs_gap_loop, s_rbs_gap_stop;
extern volatile LONG s_rbs_log_n;
extern int s_rbi_acc_chained;
extern u32 s_acc_next;
extern u32 s_acc_rec[RB_ACC_MAX], s_acc_rep[RB_ACC_MAX];
extern u32 s_nacc_rec, s_nacc_rep;

/* ---- Zustand aus loc_rb_input.c ---- */
extern int s_rbi, s_rbi_last_pre, s_rbi_delay, s_rbi_maxpred, s_rbi_tsync;
extern int s_rbi_pred, s_rbi_predq;
extern rbl_cfg_t s_rbl_cfg;
extern rbl_t *s_rbl;
extern int s_rbl_test;
extern u32 s_ls_cause, s_ls_ticks;
extern int s_rbi_early, s_rbi_sent_early;
extern volatile LONG s_wait_extra;
extern u32 s_rbi_tt_tick, s_rbi_wait_ext_n, s_rbi_wait_ext_f;
extern int s_rbi_fldfix;
extern u32 s_rbi_fld_live, s_rbi_fld_n, s_rbi_fld_diff;
extern int s_rbi_rec;
extern int s_rbi_chanloss, s_rbi_chanloss_ge, s_rbi_ge_bad;
extern rbi_t *s_rbc;
extern uint8_t s_rbi_out[RBI_MAXP][RBI_FR];
extern rbi_fn2_t o_e920, o_ec80;
extern u32 s_rbi_reg_d;
extern u32 s_rbi_toff[RBI_GH];
extern long long s_rbi_t_rep0;
extern rb_ser_t *s_rbi_s_ticks, *s_rbi_s_slots, *s_rbi_s_load, *s_rbi_s_rep;
extern u32 s_rbi_rb_n, s_rbi_rb_last, s_rbi_lost_logged, s_rbi_qfail, s_rbi_ts_waits, s_rbi_ts_ms, s_rbi_bar, s_rbi_peer_bad, s_rbi_tx_err, s_rbi_compact, s_rbi_hash_logged, s_rbi_load_fail, s_rbi_missions;
extern u32 s_tim_t0, s_tim_own_seen, s_tim_take_seen, s_tim_own_t, s_tim_own_pend;
extern u32 s_tim_h_own[RBI_TH], s_tim_h_take[RBI_TH], s_tim_h_send[RBI_TH], s_tim_own_first, s_tim_ticks;
extern u32 s_lat_h_xin[RBI_TH], s_lat_h_x2o[RBI_TH], s_lat_xin_tick0, s_lat_multi, s_lat_ticks, s_lat_none, s_lat_before;
extern double s_tim_adv_min, s_tim_adv_max, s_tim_adv_sum;
extern u32 s_tim_adv_n;
extern int s_rbi_defer_on;
extern u32 s_rbi_dflags[2], s_rbi_dtick_own, s_rbi_dmulti;
extern int s_rbi_ts_stall, s_rbi_ts_in;
extern u32 s_rbi_tick_takes;
extern u32 s_rbi_dstream[2], s_rbi_dn[2], s_rbi_dchk[2][10], s_rbi_dcnt[10];
extern rbn_t *s_rbn;
extern CRITICAL_SECTION s_rbn_cs;
extern volatile int s_rbi_mst;
extern u32 s_rbi_ls_missions, s_rbi_r2_missions, s_rbi_end_desync, s_rbi_end_disagree, s_rbi_last_why;
extern int s_rbi_hcmp_logged;
extern int s_rbi_prep;
extern int s_rbi_heapchk;
extern int s_rbi_heap_bad;
extern u32 s_rbi_hc_tick, s_rbi_end_heap;
extern u32 s_rbi_hc_n, s_rbi_hc_nodes_max, s_rbi_hc_us_max, s_rbi_hc_bad, s_rbi_hc_part, s_rbi_hc_vq, s_rbi_hc_busy;
extern int s_rbi_rbtest_n, s_rbi_rbtest_k;
extern int s_rbi_force_desync;
extern int s_rbi_dmin, s_rbi_dmax, s_rbi_dmax_set;
#define RBI_DMAX_ONLINE 8 /* W494: Obergrenze der adaptiven Verzoegerung online (Ticks, 400 ms; ausgelegt bis 500 ms RTT + Schwankung) */
extern long long s_rbi_lt_last;
extern u32 s_rbi_lt_n, s_rbi_lt_max, s_rbi_lt_logged;
extern const char *const k_rbn_why_slug[RBN_WHY_N];
extern const char *const k_rbn_why_key[RBN_WHY_N];
extern u32 s_rbi_last_host_why;
extern volatile u32 s_rbi_hud_d[RBI_HUD_N], s_rbi_hud_t[RBI_HUD_N];
extern volatile LONG s_rbi_hud_n;
extern volatile double s_rbi_hud_adv;
extern int s_rbi_npeers;
extern u32 s_rbl_wdep;
extern int s_auto_drtt, s_auto_djit;
extern int s_auto_start1;
extern int s_auto_good, s_auto_lcpm, s_auto_hcpm, s_auto_hjump;
extern volatile LONG s_auto_prof;
extern volatile LONG s_auto_rtt, s_auto_var;
extern volatile LONG s_auto_mode;
extern struct rb_pcom_s s_pcom[RB_PCOM_MAX];
extern u32 s_pcom_n, s_pcom_full, s_rbi_com_same, s_rbi_com_drop;
extern int s_memlog;
extern u32 s_memlog_n, s_memlog_lt;
extern struct rb_spec_ac_s s_spec_ac[RB_SPEC_AC];
extern u32 s_spec_ac_n, s_spec_ac_hit;
extern struct rb_spec_cm_s s_spec_cm[RB_SPEC_CM];
extern u32 s_spec_cm_n;

/* ---- Zustand aus loc_rb_resim.c ---- */
extern int s_rbv_n, s_rbv_arm, s_rbv_on, s_rbv_div;
extern int s_csn;
extern u32 s_spec_k[RBI_MAXP];
extern int s_sd_x, s_sd_step, s_sd_n;

/* ---- Zustand aus loc_rb_spec.c ---- */
extern int s_spec_want, s_spec_n, s_spec_phase0;
extern u32 s_spec_gt0, s_spec_gt[RB_SPEC_MAXT], s_spec_reg0, s_spec_rjob_live, s_spec_fld_live;
extern int s_spec_pose_ok[RB_SPEC_MAXT];
extern u32 s_spec_runs, s_spec_ticks, s_spec_early, s_spec_skip[8], s_spec_rev_fail, s_spec_reg_grow, s_spec_memev, s_spec_ac_leak, s_spec_verify_n, s_spec_verify_bad, s_spec_files;
extern rb_ser_t *s_spec_s_us, *s_spec_s_rev, *s_spec_s_pages;
extern u32 s_sc_spec[512], s_sc_live[512];

/* ---- Zustand aus loc_rollback.c ---- */
extern int s_ring;
extern int s_ww, s_depth, s_pool, s_verify, s_pdpause, s_shadowchk, s_hot, s_cool;
extern int s_kstack;
extern long s_memev_seen;
extern volatile LONG s_skipwait;
extern DWORD s_game_tid;
extern HANDLE s_rthread;
extern int s_ready, s_failed;
extern int s_k[8], s_nk, s_render, s_warm, s_max, s_census, s_perturb, s_twice, s_freeze;
extern int s_ctl;
extern long long s_qpf;
extern u32 s_rs_n, s_rs_double, s_rs_swap, s_rs_spur, s_rs_prev0, s_rs_max_ms, s_rs_rescue, s_lx_n, s_lx_spec, s_lx_rep, s_lx_held, s_spec_endstop, s_rep_endstop, s_spec_abandon_n;
extern int s_endguard;
extern int s_rep_ended;

/* ---- Funktionen aus loc_rb_snap.c ---- */
rb_ser_t *rb_ser_new(const char *name);
void rb_put(rb_ser_t *s, u32 v);
int rb_cmp(const void *a, const void *b);
void rb_pct(rb_ser_t *s, u32 *p50, u32 *p99, u32 *mx);
void rb_logser(rb_ser_t *s, const char *unit);
void rb_collect_stacks(void);
int rb_u32cmp(const void *x, const void *y);
double rb_tsc_hz(void);
void rb_win_pct(rb_win_t *w, u32 *p50, u32 *p99, u32 *mx);
LONG CALLBACK rb_avlog_veh(EXCEPTION_POINTERS *ep);
void rb_ring_commit(int replay);
void rb_build_regions(void);
int rb_alloc_snaps(int two);
void rb_copy_out(uint8_t *dst);
unsigned rb_diff(uint8_t *snap, int copy, unsigned *kinds, int want_addr);
unsigned rb_diff_blocks(uint8_t *snap);
void rb_ah_put(rb_ah_t *t, unsigned *pn, u32 addr);
void rb_ah_add(u32 addr);
void rb_wp_bench(void);

/* ---- Funktionen aus loc_rb_rec.c ---- */
u32 fnv(u32 h, const void *p, u32 n);
int rb_mine(void);
void rb_rec_put(int fn, u32 ret, u32 ih, const void *data, u32 n);
const rb_ev_t *rb_rep_get(int fn, u32 ih, const uint8_t **data);
u32 __stdcall rb_select(u32 nfds, u32 r, u32 w, u32 x, u32 tv);
u32 __stdcall rb_recv(u32 s, u32 buf, u32 len, u32 flags);
u32 __stdcall rb_recvfrom(u32 s, u32 buf, u32 len, u32 flags, u32 from, u32 pfl);
u32 __stdcall rb_send(u32 s, u32 buf, u32 len, u32 flags);
u32 __stdcall rb_sendto(u32 s, u32 buf, u32 len, u32 flags, u32 to, u32 tolen);
u32 __stdcall rb_ioctl(u32 s, u32 cmd, u32 argp);
u32 __stdcall rb_wsaerr(void);
u32 __stdcall rb_xin(u32 h, u32 pState);
u32 __stdcall rb_xgn(u32 e, u32 pdesc, u32 pout);
void rbs_net_touch(u32 serial);
void rbs_net_tick(void);
u32 __stdcall rb_xplay(u32 sb, u32 params, u32 pp);
u32 __stdcall rb_xprep(u32 sb, u32 params, u32 pp);
u32 __stdcall rb_xstop(u32 sb, u32 cue, u32 flags, u32 pcue);
u32 __stdcall rb_xstat(u32 sp, u32 pst);
u32 __stdcall rb_xstopsrc(u32 sp);
u32 __stdcall rb_xwork(void);
void rb_acc_thunk(void);
int rb_chain_jmp(u32 va, void *mine, void **orig, const char *name);
int rb_chain_call(u32 va, void *mine, u32 *orig, const char *name);
void rb_census_install(void);
void rb_census_log(void);

/* ---- Funktionen aus loc_rb_input.c ---- */
u32 rbi_us_now(void);
void tim_h(u32 *h, u32 t, u32 since);
void lat_note_own(void);
void rbi_poll(void);
void rbi_send(void);
void rbl_note_replay(u32 us);
void rbl_tick(void);
void rbl_mission_end(void);
void rbi_mission_open(void);
void rbi_mission_poll(void);
int rbi_pre_step(void); /* W475 */
void rb_pcom_clear(void);
void rb_pcom_rollback(void);
void rbi_after(void);
void rbi_install(void);
void rbi_log(int waited_s);

/* ---- Funktionen aus loc_rb_resim.c ---- */
void rbv_check(void);
void rb_cs_note(u32 cs, u32 ret);
void rbv_log(int waited_s);
void rbi_heapchk(int after_replay);
void rbi_replay_finish(void);
void rbi_session_end(int desync);
void rbi_left_reset(void);
u32 __cdecl rbi_e920(u32 i, u32 pout);
u32 __cdecl rbi_ec80(u32 data, u32 hdr);
void rbi_reg_thunk(void);
void rbi_rec_compact(void);
void rbi_set_rep_cursor(void);
u32 rb_rbi_gtick(void);
void rbi_try_rollback(void);
void rbi_state_dump(int snap_ok);
void rbi_finals(void);
void rbi_after_noring(void);

/* ---- Funktionen aus loc_rb_spec.c ---- */
int rb_spec_try_start(int snap_ok);
void rb_spec_finish(void);
void rb_spec_abandon(const char *why);
void rb_spec_tick(void);

/* ---- Funktionen aus loc_rollback.c ---- */
long long rb_now(void);
u32 rb_us(long long d);
u32 rd32(u32 va);
void rb_rthread_refresh(const char *why);
void rb_rthread_check(void);
void rb_rs_resume(void);
int rb_rs_suspend(HANDLE h, int why);
int rb_loop_exits(void);
void rb_freeze_others(void);
void rb_thaw_others(void);
int rb_quiesce(int suspend);
void rb_quiesce_commit(void); /* W469: vor dem Sichern, Wartegrenze s_qwait */
void rb_commit_prio(int on);  /* W469: Ruhepunkt-Arbeit mit erhoehter Prioritaet (loc_rb_opts cprio=) */
#define RB_SHADOW_PRE 14336u /* W469: Schatten vorab (Seiten, 56 MB; LoC-Mission ~11 300 Seiten) */
extern int s_qwait, s_negpoll;
extern int s_pre_on, s_pre_test; /* W475 */
extern u32 s_q_short;
u32 rb_rep_limit(void);
int rb_r2_acc(void);
int rb_ring_check_regions(void);

/* ---- Funktionen aus loc_rb_hitch.c (W469: Haenger-Zuordnung, Spiel-Thread) ---- */
enum { RBH_QWAIT = 0, RBH_COMMIT, RBH_REBUILD, RBH_SETUP, RBH_BENCH, RBH_STACKS, RBH_FLUSH, RBH_LOAD, RBH_MPOLL, RBH_RBL,
       RBH_RBI, RBH_CHAIN, RBH_N };
#define RBH_EV_SETUP 0x01u   /* erster Ring-Aufbau (bereit) */
#define RBH_EV_REBUILD 0x02u /* Ring-Neuaufbau (neue Allokation) */
#define RBH_EV_DCHG 0x04u    /* Verzoegerung gewechselt */
#define RBH_EV_LOAD 0x08u    /* Rollback geladen */
#define RBH_EV_MSTART 0x10u  /* Mission mit Eingabeschicht begonnen */
#define RBH_EV_QTIMEOUT 0x20u /* Ruhepunkt: Render-Thread nicht frei (Zeitgrenze) */
extern u32 s_rbh_events;
void rbh_add(int ph, long long d);
void rbh_before(void);
void rbh_after_in(void);
void rbh_after_out(void);
void rbh_log(int waited_s);

/* ---- kleine Hilfen (W464: Einzeiler, je Modul eingebettet) ---- */
static __inline void rb_prewrite_cb(uintptr_t a, unsigned n) { rbr_prewrite(a, n); }
static __inline xbe_image_t *rb_img(void) { return g_img; }
static __inline int rbs_act(void) { return g_cfg.loc_sndloop_fix != 0; }
static __inline int rbs_on(void) { return g_cfg.loc_sndloop_fix || g_cfg.loc_sndloop_diag; }

#endif /* LOC_RB_INT_H */
