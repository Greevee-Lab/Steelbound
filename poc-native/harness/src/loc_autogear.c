/* loc_autogear.c - W441 LOC-AUTOGEAR: automatischer Gangwechsel (auto_gear, Standard 0) in Line of Contact.
 *
 * Wie W218 in Steel Battalion: die Schaltlogik imap_autogear_step (input_map.c) setzt im eigenen SBC-Block nach der
 * Abbildung den Gang (und in R Gas/Bremse getauscht). Das ist dieselbe Stelle, an der Tastatur, Pad und Profil den
 * Block fuellen: das Spiel bildet daraus den Eingabesatz (Byte 8 Gang, 5-7 Pedale), den Lockstep bzw. die
 * Rollback-Eingabeschicht an alle Mitspieler verteilt. Alle Seiten rechnen also mit demselben Gang - kein
 * Spielzustand wird geschrieben, nur gelesen (Lage und Gier des eigenen VT als Geschwindigkeitsquelle).
 *
 * Rollback: Beim Neurechnen bedient loc_rollback.c (rb_xin) die SBC-Abfrage aus der Aufzeichnung, dieser Hook laeuft
 * dann nicht; im Vorlauf (W390) laeuft er genau einmal und der folgende Live-Tick bekommt denselben Block. Damit
 * genau ein Schritt je Spieltick.
 *
 * Nicht aktiv: ohne Mission (loc_mouse_mission: Lockstep aktiv, eigener VT abgesetzt, nicht auf der Karte), bei
 * offenem ESC-Menue, mit dem Original-Controller (sbc_usb, wie SB) und solange eine Gang-Direktwahl greift.
 *
 * LoC-VAs (nur gelesen, W376/W380): eigener Spieler [0x306A84], VT 0x5C50D0 + Spieler * 0xAE8, Objekt [VT+0xC] mit
 * x +0xC / z +0x14 / Gier +0x1C, Gang VT+0x538 (nur DIAG). Logzeilen nur Gangwechsel und Zaehler, keine Eingaben. */
#include "loc_net.h" /* W440 loc_assist_gate */
#include "loc_autogear.h"
#include "hcfg.h"
#include "hlog.h"
#include "loc_pad.h"
#include "sb_overlay.h"
#include "sbc_wire.h"

#include <windows.h>
#include <stdint.h>
#include <string.h>

typedef uint32_t u32;

#define LAG_PLAYER 0x306A84u
#define LAG_VT0 0x5C50D0u
#define LAG_VSTRIDE 0xAE8u

static imap_autogear_t s_ag;
static imap_autogear_cfg_t s_agc;
static imap_agvel_t s_vel;
static int s_agc_init, s_active, s_paused_logged, s_on_logged;
static unsigned s_step;  /* Schritte (= SBC-Abfragen in der Mission) */
static float s_v;
static unsigned s_up, s_down, s_rev, s_rfwd, s_missions;

static int lag_read(float *x, float *z, float *hy, int *vtgear)
{
    u32 pl, vt, obj;
    __try {
        pl = *(volatile const u32 *)(uintptr_t)LAG_PLAYER;
        if (pl >= 10u) return 0;
        vt = LAG_VT0 + pl * LAG_VSTRIDE;
        obj = *(volatile const u32 *)(uintptr_t)(vt + 0xCu);
        if (obj < 0x10000u || obj >= 0x80000000u) return 0;
        *x = *(volatile const float *)(uintptr_t)(obj + 0xCu);
        *z = *(volatile const float *)(uintptr_t)(obj + 0x14u);
        *hy = *(volatile const float *)(uintptr_t)(obj + 0x1Cu);
        *vtgear = *(volatile const signed char *)(uintptr_t)(vt + 0x538u);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return 1;
}

static void lag_reset(void)
{
    memset(&s_ag, 0, sizeof s_ag);
    memset(&s_vel, 0, sizeof s_vel);
    s_v = 0.0f;
}

void loc_autogear_poll(imap_block_t *blk, uint8_t *gear_st, int gear_direct)
{
    float x = 0.0f, z = 0.0f, hy = 0.0f;
    int vtgear = 0, diag;
    uint8_t g0;
    int g1;
    if (!blk) return;
    diag = g_cfg.input_diag && (g_cfg.loc_replay_tick[0] || g_cfg.loc_steps[0]); /* DIAG nur mit Testfolge */
    if (!loc_assist_gate(g_cfg.auto_gear) && !diag) return; /* W440: Sim-Lobby = aus */
    if (g_cfg.sbc_usb) { /* Original-Controller bleibt immer echt (wie SB) */
        int sst = sbc_wire_status();
        if (sst == SBC_WIRE_USB_OK || sst == SBC_WIRE_FIXTURE) return;
    }
    if (g_cfg.overlay && sb_ovl_open()) return;
    if (!loc_mouse_mission() || !lag_read(&x, &z, &hy, &vtgear)) {
        if (s_active) { /* Missionsende/Eject/Karte: Verlauf und gelernte Endgeschwindigkeiten verwerfen */
            s_active = 0;
            lag_reset();
        }
        return;
    }
    if (!s_active) {
        s_active = 1;
        s_missions++;
        lag_reset();
    }
    s_step++;
    if (s_step % 600u == 0u) loc_autogear_log(); /* alle 30 s, nur bei Wechseln */
    imap_agvel_push(&s_vel, x, z, hy, s_step, LOC_AG_JUMP);
    if (!s_agc_init) {
        imap_autogear_defaults_loc(&s_agc);
        s_agc_init = 1;
    }
    s_v = imap_agvel_loc_v(&s_vel, blk, s_agc.pedal_on);
    if (diag && g_cfg.input_diag >= 2) { /* DIAG Messung W441: Lage je Tick (Spielzustand, keine Eingabe) */
        HLOG_I("LOCGEAR-Pos t=%u x=%.1f z=%.1f hy=%.4f vt_gang=%d", s_step, x, z, hy, vtgear);
    }
    if (!loc_assist_gate(g_cfg.auto_gear)) { /* W440 */
        if (s_step % 10u == 0u) { /* DIAG ohne Automatik: Geschwindigkeit je Gang (Messung W441) */
            HLOG_I("LOCGEAR-Diag t=%u v=%.1f gang=%u vt_gang=%d", s_step, s_v, blk->gear, vtgear);
        }
        return;
    }
    if (!s_on_logged) {
        s_on_logged = 1;
        HLOG_I("LOCGEAR: Gang-Automatik aktiv (auto_gear=1) bei Schritt %u", s_step);
    }
    if (gear_direct) { /* W221: Direktwahl (HOTAS-Zonen) -> Spieler waehlt selbst */
        if (!s_paused_logged) {
            s_paused_logged = 1;
            HLOG_I("LOCGEAR: Automatik pausiert (Gang-Direktwahl aktiv)");
        }
        return;
    }
    g0 = blk->gear;
    {
        imap_autogear_cfg_t c = s_agc;
        if (!imap_agvel_loc_plateau(&s_vel, s_ag.init ? s_step - s_ag.shift_frame : 0u))
            c.v_min = 1e30f; /* LoC: Hochschalten erst am echten Plateau */
        g1 = imap_autogear_step(&s_ag, &c, blk, gear_st, s_v, s_step);
    }
    if (g1 != g0 && g0 >= 7 && g0 <= 13) {
        static const char *const gn[7] = { "R", "N", "1", "2", "3", "4", "5" };
        if (g1 == 7) s_rev++;
        else if (g0 == 7) s_rfwd++;
        else if (g1 > g0) s_up++;
        else s_down++;
        HLOG_I("LOCGEAR: Automatik Gang %s -> %s (v=%.1f vtop=%.1f) Schritt %u", gn[g0 - 7], gn[g1 - 7], s_v,
               s_ag.vtop[g0], s_step);
    }
    if (diag && s_step % 10u == 0u) {
        HLOG_I("LOCGEAR-Diag t=%u v=%.1f gang=%d vt_gang=%d", s_step, s_v, g1, vtgear);
    }
}

void loc_autogear_log(void)
{
    if (!s_up && !s_down && !s_rev) return;
    HLOG_I("LOCGEAR: Wechsel hoch=%u runter=%u nach R=%u aus R=%u, Missionen %u, Schritte %u", s_up, s_down, s_rev,
           s_rfwd, s_missions, s_step);
}
