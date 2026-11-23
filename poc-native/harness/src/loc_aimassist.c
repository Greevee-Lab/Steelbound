/* loc_aimassist.c - W442 LOC-AIMASSIST: Zielhilfe (aim_assist 1/2, Standard 0) in Line of Contact (siehe loc_aimassist.h).
 *
 * Stelle wie W236 in SB und W441 (Gang-Automatik): im eigenen SBC-Block nach der Abbildung (Legacy/Profil/Pad), bevor
 * das Spiel daraus den Eingabesatz bildet, den Lockstep bzw. die Rollback-Eingabeschicht an alle Mitspieler verteilt.
 * Gelesen wird nur der von loc_aim.c je Live-Tick veroeffentlichte eigene Zustand (Simulation + ausstehende eigene
 * Saetze); geschrieben nur die eigenen Sichthebel im Block. Beim Neurechnen bedient rb_xin die Abfrage aus der
 * Aufzeichnung (Hook laeuft nicht) - ein Schritt je Spieltick, wie W441.
 *
 * Nicht aktiv: im Lockstep-Rueckfall (keine Vorhersage-Basis), ausserhalb der Mission, in Sim-Lobbys (loc_assist_gate,
 * W440), bei offenem ESC-Menue oder offener Chatzeile, mit dem Original-Controller (sbc_usb, wie SB). Logzeilen nur
 * Zaehler und Zustaende, keine Eingaben. */
#include "loc_aimassist.h"
#include "loc_aim.h"
#include "loc_net.h" /* W440 loc_assist_gate */
#include "loc_pad.h"
#include "loc_chatnet.h"
#include "input_map.h"
#include "hcfg.h"
#include "hlog.h"
#include "sb_overlay.h"
#include "sbc_wire.h"
#include "loc_hud_math.h"
#include "sb_profile.h" /* g_sba_loc */

#include <windows.h>
#include <string.h>


static volatile LONG s_py_bits; /* float-Bits der juengsten Projektion py (0 = unbekannt) */
static SRWLOCK s_lk = SRWLOCK_INIT;
static laa_cfg_t s_cfg_pub;     /* unter s_lk: Einstellung des letzten Polls (fuer die Vorschau) */
static int s_mask_pub;          /* unter s_lk: gesetzte Achsen des letzten Polls */
static int s_on_pub;

static uint32_t s_used_seq;
static uint8_t s_prev_in[14];
static int s_have_prev, s_logged, s_active;
static unsigned s_polls, s_nx, s_ny, s_base_pred, s_base_none, s_base_fill, s_missions, s_chk_ok, s_chk_bad, s_same_n;
static volatile LONG s_prev_n, s_prev_set;

void loc_aimassist_note_py(double py)
{
    float f = (float)py;
    LONG b;
    if (!(py > 0.05 && py < 100.0)) return;
    memcpy(&b, &f, 4);
    InterlockedExchange(&s_py_bits, b);
}

static double laa_py(void)
{
    LONG b = s_py_bits;
    float f;
    memcpy(&f, &b, 4);
    return b ? (double)f : 0.0;
}

static int laa_gate_ok(void)
{
    if (!g_sba_loc || !loc_assist_gate(g_cfg.aim_assist)) return 0;
    if (g_cfg.aim_assist != 1 && g_cfg.aim_assist != 2) return 0;
    if (g_cfg.sbc_usb) { /* Original-Controller bleibt immer echt (wie SB) */
        int sst = sbc_wire_status();
        if (sst == SBC_WIRE_USB_OK || sst == SBC_WIRE_FIXTURE) return 0;
    }
    if ((g_cfg.overlay && sb_ovl_open()) || loc_chat_typing()) return 0;
    return 1;
}

static void laa_pub(const laa_cfg_t *c, int mask, int on)
{
    AcquireSRWLockExclusive(&s_lk);
    if (c) s_cfg_pub = *c;
    s_mask_pub = mask;
    s_on_pub = on;
    ReleaseSRWLockExclusive(&s_lk);
}

void loc_aimassist_poll(void *blkp, unsigned mouse_axes)
{
    imap_block_t *blk = (imap_block_t *)blkp;
    loc_aim_pub_t P;
    la_state_t base;
    laa_cfg_t c;
    uint8_t in[14], ax4[4];
    int mask, diag;
    if (!blk) return;
    if (!laa_gate_ok() || !loc_mouse_mission()) {
        if (s_active) {
            s_active = 0;
            s_have_prev = 0;
            loc_aimassist_log();
        }
        laa_pub(NULL, 0, 0);
        return;
    }
    if (!s_active) {
        s_active = 1;
        s_missions++;
        s_have_prev = 0;
    }
    s_polls++;
    if (s_polls % 600u == 0u) loc_aimassist_log(); /* alle 30 s */
    memset(&c, 0, sizeof c);
    c.mode = g_cfg.aim_assist;
    c.edge_x = c.edge_y = laa_edge(g_cfg.aim_assist_edge, laa_py());
    c.lock_axes = c.mode == 2 ? (mouse_axes & 3u) : 0u;
    if (!loc_aim_peek(&P) || !P.have_bst || (P.seq == s_used_seq && s_same_n >= 1)) {
        /* keine Basis: Lockstep (eigene ausstehende Saetze unbekannt), Sonderpfad oder Veroeffentlichung veraltet */
        s_base_none++;
        s_have_prev = 0;
        laa_pub(&c, 0, 0);
        return;
    }
    base = P.bst;
    s_base_pred++;
    if (P.seq == s_used_seq && s_have_prev) { /* zweiter Poll ohne neuen Ruhepunkt: eigenen letzten Satz nachtragen */
        if (!la_step(&base, s_prev_in)) base = P.bst;
        s_base_fill++;
        s_same_n++;
    } else {
        s_same_n = 0;
        if (s_have_prev && P.n > 0) { /* Pruefung: juengster ausstehender Satz = unser letzter (Achsen) */
            if (memcmp(P.last_in + 10, s_prev_in + 10, 4) == 0) s_chk_ok++;
            else s_chk_bad++;
        }
    }
    s_used_seq = P.seq;
    /* Satz aus dem Block (Byte 10..13 wie das Spiel; Zoomtasten wie im juengsten eigenen Satz, ohne Stick-Klick) */
    memset(in, 0, sizeof in);
    if (P.n > 0) in[0] = (uint8_t)(P.last_in[0] & 0x0Cu);
    lh_axes_to_frame(blk->aiming_x, blk->aiming_y, blk->sight_x, blk->sight_y, ax4);
    memcpy(in + 10, ax4, 4);
    mask = laa_apply(&base, &c, in);
    if (mask & 1) {
        blk->sight_x = (int8_t)in[12];
        s_nx++;
    }
    if (mask & 2) {
        blk->sight_y = (int8_t)~(int8_t)in[13];
        s_ny++;
    }
    memcpy(s_prev_in, in, 14);
    s_have_prev = 1;
    laa_pub(&c, mask, 1);
    if (!s_logged) {
        s_logged = 1;
        HLOG_I("LOCAA: Zielhilfe aktiv (Stufe %d, Rand %d%% = %.3f/%.3f rad, py=%.3f, Basis Vorhersage)", c.mode,
               g_cfg.aim_assist_edge, c.edge_x, c.edge_y, laa_py());
    }
    diag = g_cfg.loc_aa_diag && (g_cfg.loc_replay_tick[0] || g_cfg.loc_steps[0]); /* DIAG nur in Testfolgen */
    if (diag && (s_polls % 5u) == 0u) {
        /* Spielzustand (Sicht/Arm) und Wirkung je 5 Ticks - nur mit synthetischen Testfolgen */
        HLOG_I("LOCAA-Diag t=%u yt=%.4f ys=%.4f ax=%.4f pt=%.4f ps=%.4f ay=%.4f z=%.0f rel=%.4f/%.4f mask=%d n=%d py=%.3f",
               s_polls, base.yt, base.ys, base.ax, base.pt, base.ps, base.ay, base.zoom, base.ax - base.ys,
               base.ay + base.ps, mask, P.n, laa_py());
    }
}

int loc_aimassist_preview(const la_state_t *pst, const uint8_t last_in[14], uint8_t F[14])
{
    laa_cfg_t c;
    int mask, on, r;
    if (!pst || !last_in || !F || !g_sba_loc) return 0;
    AcquireSRWLockShared(&s_lk);
    c = s_cfg_pub;
    mask = s_mask_pub;
    on = s_on_pub;
    ReleaseSRWLockShared(&s_lk);
    if (!on) return 0;
    /* Achsen, die der letzte Poll gesetzt hat und die seither unveraendert sind: der naechste Poll rechnet sie neu */
    if ((mask & 1) && F[12] == last_in[12]) F[12] = 0;
    if ((mask & 2) && F[13] == last_in[13]) F[13] = 0xFF;
    r = laa_apply(pst, &c, F);
    InterlockedIncrement(&s_prev_n);
    if (r) InterlockedIncrement(&s_prev_set);
    return r != 0;
}

void loc_aimassist_log(void)
{
    if (!s_polls) return;
    HLOG_I("LOCAA: Polls=%u gesetzt x=%u y=%u | Basis Vorhersage=%u (nachgetragen %u, Satz-Pruefung ok/falsch %u/%u) "
           "keine Basis=%u | Vorschau %ld/%ld | Missionen %u",
           s_polls, s_nx, s_ny, s_base_pred, s_base_fill, s_chk_ok, s_chk_bad, s_base_none, (long)s_prev_set,
           (long)s_prev_n, s_missions);
}
