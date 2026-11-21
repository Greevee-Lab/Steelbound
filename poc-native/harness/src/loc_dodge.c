/* loc_dodge.c - W380 LOC-DODGE: Zaehler fuer den Seitenschritt (Slide-Step, linkes Pedal) im LoC-Profil.
 *
 * Original-Mechanik (LoC-VAs, nur gelesen, LOC_BOOT "W380: LOC-DODGE"):
 *  - 0xAB8A2: Energie VT+0x528 > 0 und Flanke des linken Pedals (FUN_0001A330(spieler,0,6,0xC), Eingabesatz Byte 5)
 *    -> FUN_000B0C30: nur ohne laufenden Slide (+0x94 Bit 0x1000000) und mit Energie >= 0,4 * Max (+0x524)
 *    -> Zustand (VT+4, VT+5) = (2,6).
 *  - Zustand (2,6) @0xAC160: Anlauf-Animation (FUN_000B0D90(obj,0x13,0,0x28), mit Ton). Erst an ihrem Ende liest das
 *    Spiel den AKTUELLEN Drehhebel (idx 0x10): > 0 -> (2,9) mit +0x94 Bit 0x1000 (rechts), < 0 -> (2,9) mit 0x800
 *    (links), neutral -> (2,7) vorwaerts bzw. im R-Gang (2,8) rueckwaerts.
 *  - 0xAB8ED: Pedal waehrend des Slides losgelassen -> +0x9C |= 0x80 -> Abbruch nach (2,0). Gehalten: Energie -0,5 je
 *    Tick; leer -> Abbruch.
 *
 * loc_dodge_diag=1 (DIAG, Standard 0, nur mit Testfolge loc_replay_tick/loc_steps): je Lockstep-Rahmen (Spiel-Thread,
 * aus ls_record, nicht beim Neurechnen) den eigenen VT lesen und Versuche zaehlen: Druck (Flanke im entnommenen
 * Eingabesatz), Anlauf (Zustand 2,6 = Ton), Schritt je Richtung, Abbruch (nur Ton), abgelehnt (kein Anlauf),
 * Richtung korrekt (Drehhebel beim Druck = entschiedene Richtung). Keine Aufzeichnung echter Eingaben. */
#include "loc_dodge.h"
#include "hcfg.h"
#include "hlog.h"
#include "loc_rollback.h"

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>

typedef uint32_t u32;

#define LD_PLAYER 0x306A84u  /* eigener Spieler-Index (W341) */
#define LD_VT0 0x5C50D0u     /* VT-Objekt 0 (W376) */
#define LD_VSTRIDE 0xAE8u
#define LD_REC0 0x2C5EB2u    /* aufgeweiteter Eingabesatz Spieler 0 (FUN_0001B870), Index = Byte-Offset */
#define LD_RSTRIDE 0x4F0u

static struct {
    int open;            /* laufender Versuch */
    u32 t0;              /* Rahmen des Drucks */
    int rot0;            /* Drehhebel beim Druck: -1/0/+1 */
    int windup;          /* Anlauf gesehen */
    u32 tw;              /* Rahmen Anlauf-Beginn */
    int step;            /* Schrittzustand (7/8/9) laeuft */
    int dir;             /* entschiedene Richtung -1/0/+1 */
    u32 ts;              /* Rahmen Schritt-Beginn */
    u32 hold;            /* Rahmen mit gehaltenem Pedal seit dem Druck */
    int prev_ped;
    unsigned char prev_s4, prev_s5;
    float x0, z0, yaw0;
} s;

static struct {
    unsigned press, windup, step_l, step_r, step_f, step_b, abort_, reject, reject_energy, dir_ok, dir_bad, superseded;
    unsigned shortstep;          /* Schritt mit Strecke < LD_SHORT (wirkt wie "nur Ton") */
    unsigned wsum, wmin, wmax, wn;
    float dsum;
    unsigned dn;
} c;

#define LD_SHORT 200.0f /* Strecke (Spieleinheiten), darunter gilt ein Schritt als "nichts passiert" (W380: 3 Ticks Pedal = 90, 9 Ticks = 2612) */

static u32 s_frame;
static int s_on = -1;

static int ld_sign(int v) { return v > 25 ? 1 : (v < -25 ? -1 : 0); } /* Totzone wie FUN_0001B0A0 (|x| < 25,6) */

static void ld_close(const char *what, int dir)
{
    if (!s.open) return;
    if (g_cfg.loc_dodge_diag & 2) {
        HLOG_I("LOCDODGE: Versuch Rahmen %u: %s (Hebel beim Druck %d, Entscheidung %d, Pedal %u Rahmen)", s.t0, what,
               s.rot0, dir, s.hold);
    }
    s.open = 0;
    s.step = 0;
}

static void ld_frame(void)
{
    u32 pl, vt, obj;
    int ped, rot, gear;
    unsigned char s4, s5;
    u32 f94;
    float en, enmax, x = 0.f, z = 0.f, yaw = 0.f;
    if (loc_rb_resim()) return;
    s_frame++;
    __try {
        pl = *(volatile const u32 *)(uintptr_t)LD_PLAYER;
        if (pl >= 10u) return;
        vt = LD_VT0 + pl * LD_VSTRIDE;
        ped = *(volatile const unsigned char *)(uintptr_t)(LD_REC0 + pl * LD_RSTRIDE + 0xCu);
        rot = *(volatile const signed char *)(uintptr_t)(LD_REC0 + pl * LD_RSTRIDE + 0x10u);
        s4 = *(volatile const unsigned char *)(uintptr_t)(vt + 4u);
        s5 = *(volatile const unsigned char *)(uintptr_t)(vt + 5u);
        f94 = *(volatile const u32 *)(uintptr_t)(vt + 0x94u);
        gear = *(volatile const signed char *)(uintptr_t)(vt + 0x538u);
        en = *(volatile const float *)(uintptr_t)(vt + 0x528u);
        enmax = *(volatile const float *)(uintptr_t)(vt + 0x524u);
        obj = *(volatile const u32 *)(uintptr_t)(vt + 0xCu);
        if (obj >= 0x10000u && obj < 0x80000000u) {
            x = *(volatile const float *)(uintptr_t)(obj + 0xCu);
            z = *(volatile const float *)(uintptr_t)(obj + 0x14u);
            yaw = *(volatile const float *)(uintptr_t)(obj + 0x1Cu);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (ped && !s.prev_ped) { /* Flanke im entnommenen Eingabesatz */
        if (s.open) { c.superseded++; ld_close(s.step ? "ueberholt im Schritt" : (s.windup ? "ueberholt nach Anlauf" : "ueberholt"), s.dir); }
        c.press++;
        s.open = 1;
        s.t0 = s_frame;
        s.rot0 = ld_sign(rot);
        s.windup = 0;
        s.step = 0;
        s.hold = 0;
        s.x0 = x;
        s.z0 = z;
        s.yaw0 = yaw;
        if (g_cfg.loc_dodge_diag & 2) {
            HLOG_I("LOCDODGE: Druck Rahmen %u Energie %.1f/%.1f (%s) Zustand %u,%u Gang %d", s_frame, en, enmax,
                   en >= 0.4f * enmax ? "genug" : "zu wenig", s4, s5, gear);
        }
        if (!(en >= 0.4f * enmax)) {
            c.reject++;
            c.reject_energy++;
            ld_close("abgelehnt (Energie)", 0);
        }
    }
    s.prev_ped = ped;
    if (s.open && ped) s.hold++;
    if (s.open) {
        if (s.step) {
            if (!(s4 == 2 && s5 == s.prev_s5 && s.prev_s4 == 2)) { /* Schritt vorbei: Strecke auswerten */
                float dx = x - s.x0, dz = z - s.z0;
                float d = sqrtf(dx * dx + dz * dz);
                float fw = dx * sinf(s.yaw0) + dz * cosf(s.yaw0), sd = dx * cosf(s.yaw0) - dz * sinf(s.yaw0);
                char what[96];
                c.dsum += d;
                c.dn++;
                if (d < LD_SHORT) c.shortstep++;
                sprintf_s(what, sizeof what, "Schritt %s, %u Rahmen, Strecke %.2f (vor %.2f seitlich %.2f)%s",
                          s.prev_s5 == 7 ? "vor" : (s.prev_s5 == 8 ? "zurueck" : (s.dir > 0 ? "rechts" : "links")),
                          s_frame - s.ts, d, fw, sd, d < LD_SHORT ? " = kaum Bewegung" : "");
                ld_close(what, s.dir);
            }
        } else if (s4 == 2 && s5 == 6 && !s.windup) {
            s.windup = 1;
            s.tw = s_frame;
            c.windup++;
        } else if (s.windup && s4 == 2 && (s5 == 7 || s5 == 8 || s5 == 9)) {
            u32 w = s_frame - s.tw;
            s.dir = (s5 == 7 || s5 == 8) ? 0 : ((f94 & 0x1000u) ? 1 : -1);
            c.wsum += w; c.wn++;
            if (!c.wmin || w < c.wmin) c.wmin = w;
            if (w > c.wmax) c.wmax = w;
            if (s5 == 7) c.step_f++;
            else if (s5 == 8) c.step_b++;
            else if (s.dir > 0) c.step_r++;
            else c.step_l++;
            if (s.dir == s.rot0) c.dir_ok++;
            else c.dir_bad++;
            s.step = 1;
            s.ts = s_frame;
        } else if (s.windup && !(s4 == 2 && s5 == 6)) {
            c.abort_++;
            ld_close("abgebrochen (nur Anlauf/Ton)", 0);
        } else if (!s.windup && s_frame - s.t0 > 3u) {
            c.reject++;
            ld_close("abgelehnt (kein Anlauf)", 0);
        }
    }
    s.prev_s4 = s4;
    s.prev_s5 = s5;
    if (s_frame % 200u == 0u) loc_dodge_log();
}

/* Aufruf aus dem Thunk von ls_record: der sichert nur Ganzzahlregister. x87/SSE des Spiels hier sichern, dann frei. */
static __declspec(align(16)) unsigned char s_fx[512];
void loc_dodge_frame(void)
{
    if (s_on < 0) s_on = g_cfg.loc_dodge_diag && (g_cfg.loc_replay_tick[0] || g_cfg.loc_steps[0]);
    if (!s_on) return;
    __asm {
        fxsave s_fx
        fninit
    }
    ld_frame();
    __asm { fxrstor s_fx }
}

void loc_dodge_log(void)
{
    if (s_on <= 0) return;
    HLOG_I("LOCDODGE: Druecke=%u Anlauf=%u Schritte links=%u rechts=%u vor=%u zurueck=%u | nur Ton (Abbruch)=%u "
           "kaum Bewegung=%u abgelehnt=%u (Energie %u) ueberholt=%u | Richtung korrekt=%u falsch=%u | Anlauf Ticks "
           "min/mittel/max=%u/%u/%u | Strecke mittel %.2f",
           c.press, c.windup, c.step_l, c.step_r, c.step_f, c.step_b, c.abort_, c.shortstep, c.reject,
           c.reject_energy, c.superseded, c.dir_ok, c.dir_bad, c.wmin, c.wn ? c.wsum / c.wn : 0u, c.wmax,
           c.dn ? c.dsum / (float)c.dn : 0.f);
}
