/* loc_aimassist.h - W442 LOC-AIMASSIST (GitHub #79): Zielhilfe aim_assist 1/2 ("Cockpit folgt der Waffe") in Line of
 * Contact. Oben das reine Gesetz (ohne Spielspeicher, Selbsttest render_selftest "LOCAA"), unten die Hook-API.
 *
 * Wie W236 in Steel Battalion, aber mit dem LoC-Modell (loc_aim_law.h, W376): der Sichthebel (Satz-Byte 12/13) ist eine
 * Raten-Steuerung auf das Sicht-Soll (Gier +0x40C, +-pi/2; Nick +0x410, +-pi/4; Rate k = (60 - Zoom) / 55 * 0,0005 je
 * Stufe und Tick, Totzone |Hebel| < 25,6), der Zielhebel (Byte 10/11) eine Positions-Steuerung des Waffenarms mit
 * nichtlinearer Glaettung und Ratenbegrenzung. Das Fadenkreuz steht im Bild bei x = -py tan(Arm_x - Sicht_x),
 * y = -py tan(Arm_y + Sicht_y) (Messlauf W376_A9).
 *   Stufe 1 "Rand dreht die Sicht": liegt (Arm - Sicht) ueber dem Rand, wird das Sicht-Soll so weit nachgefuehrt, dass
 *            das Fadenkreuz am Rand bleibt (innerhalb des Rands keine Wirkung).
 *   Stufe 2 "Waffenarm fest": auf Achsen aus der Maus (lock_axes) folgt das Sicht-Soll dem Arm ganz (Fadenkreuz in der
 *            Mitte); andere Achsen wie Stufe 1.
 * Gesteuert wird auf den Arm am Ende des Ticks, in dem der Satz wirkt (la_step mit dem Satz), ausgehend vom Zustand nach
 * allen eigenen, schon erzeugten Saetzen (Vorhersage W376: Basis = Simulation + ausstehende eigene Saetze). Eigene
 * Sicht-Eingabe (Hebel ausserhalb der Totzone) gewinnt je Achse. Nur der eigene Satz vor dem Eingabewort wird
 * veraendert, nie der Spielzustand - alle Seiten bleiben deterministisch. */
#pragma once
#include <stdint.h>
#include <math.h>
#include "loc_aim_law.h"

#define LAA_DZ 25 /* |Hebel| <= 25 -> 0 (FUN_0001B0A0: < 25,6) */
#define LAA_YAW_MAX 1.5707963705062866
#define LAA_PIT_MAX 0.7853981852531433
#define LAA_LEAD 1.5 /* Nachlauf der sichtbaren Sicht bei gleichmaessiger Drehung: (1 - 0,4) / 0,4 Ticks */

typedef struct {
    int mode;              /* 1 Rand, 2 Waffenarm fest */
    double edge_x, edge_y; /* Rand in rad (Arm - Sicht) */
    unsigned lock_axes;    /* Stufe 2: Bit 0 x, Bit 1 y (Achse kommt von der Maus) */
} laa_cfg_t;

/* Hebelwert ohne Wirkung im Spiel (Totzone) */
static int laa_neutral(int8_t v)
{
    return (v < 0 ? -(int)v : (int)v) <= LAA_DZ;
}

/* Sicht-Soll einer Achse (Arm a und Sicht v im selben Vorzeichen) */
static double laa_target(double a, double v, int lock, double edge, double vmax)
{
    double vd = v, rel = a - v;
    if (lock) vd = a;
    else if (rel > edge) vd = a - edge;
    else if (rel < -edge) vd = a + edge;
    return la_clamp(vd, -vmax, vmax);
}

/* Wirksamer Hebelwert fuer die Aenderung du (Stufen): gerundet; in der Totzone der kleinste wirksame Schritt (ab halbem
 * Schritt) oder 0 - Restfehler <= halber Schritt, kein Pendeln. */
static int laa_q(double du)
{
    double r = du < 0.0 ? -floor(-du + 0.5) : floor(du + 0.5);
    int q;
    if (r > 127.0) r = 127.0;
    if (r < -127.0) r = -127.0;
    q = (int)r;
    if ((q < 0 ? -q : q) <= LAA_DZ) q = (2.0 * fabs(du) >= (double)(LAA_DZ + 1)) ? (du < 0.0 ? -(LAA_DZ + 1) : LAA_DZ + 1) : 0;
    return q;
}

/* Neues Sicht-Soll einer Achse im Vorzeichen des Arms: a1 = Arm am Tickende, a0 = Arm davor, t = Sicht-Soll.
 * Das Fadenkreuz haengt an der SICHTBAREN Sicht (A9), die dem Soll je Tick um 0,4 folgt (v' = v + 0,4 (t' - v)) und
 * bei gleichmaessiger Drehung um 1,5 Tickschritte hinter ihm liegt. Soll an den um diese Nachlaufzeit vorgezogenen Arm
 * (a1 + 1,5 (a1 - a0)) - sonst liefe das Fadenkreuz bei schnellem Zielen um bis ~0,08 rad ueber den Rand bzw. aus der
 * Mitte (Messlauf W442 A1: 0,511 statt 0,433 rad). Der Vorhalt haengt nur am Arm, nicht an der Sicht: kein Regelkreis
 * ueber das Bild, kein Pendeln. Innerhalb des Rands nichts. Rueckgabe 1 = Soll aendern. */
static int laa_axis_soll(double a1, double a0, double t, int lock, double edge, double vmax, double *t_new)
{
    double vd = laa_target(a1 + LAA_LEAD * (a1 - a0), t, lock, edge, vmax);
    *t_new = vd;
    return vd != t;
}

/* Satz in (14 Byte, Zielhebel Byte 10/11 endgueltig) auf den Zustand b (nach allen eigenen ausstehenden Saetzen):
 * Sichthebel Byte 12/13 setzen, wo die eigene Sicht-Eingabe neutral ist. Rueckgabe Bit 0 = x gesetzt, Bit 1 = y
 * gesetzt (0 = nichts veraendert). Sonderpfade (Sicht-Ruecksetzen, Flag +0x98 Bit 4: Arm folgt der Sicht) -> 0. */
static int laa_apply(const la_state_t *b, const laa_cfg_t *c, uint8_t in[14])
{
    la_state_t s1;
    double k, tn;
    int mask = 0, q;
    if (!b || !c || !in || (c->mode != 1 && c->mode != 2)) return 0;
    if (!la_can(b) || (b->f98 & 4u)) return 0;
    s1 = *b;
    if (!la_step(&s1, in)) return 0; /* Stick-Klick: Spiel setzt die Sicht zurueck */
    k = (60.0 - s1.zoom) * 0.0181818176060915 * 0.0005000000237487257;
    if (!(k > 1e-7)) return 0;
    if (laa_neutral((int8_t)in[12]) &&
        laa_axis_soll(s1.ax, b->ax, b->yt, c->mode == 2 && (c->lock_axes & 1u), c->edge_x, LAA_YAW_MAX, &tn)) {
        q = laa_q((b->yt - tn) / k); /* yt' = yt - k * Hebel */
        if (q) {
            in[12] = (uint8_t)(int8_t)q;
            mask |= 1;
        }
    }
    /* Fadenkreuz-Nick = Arm_y + Sicht-Nick: Sicht im Vorzeichen des Arms ist -pt */
    if (laa_neutral((int8_t)in[13]) &&
        laa_axis_soll(s1.ay, b->ay, -b->pt, c->mode == 2 && (c->lock_axes & 2u), c->edge_y, LAA_PIT_MAX, &tn)) {
        q = laa_q((b->pt + tn) / k); /* pt' = pt - k * (inv ? -Hebel : Hebel), neues Soll pt = -tn */
        if (q) {
            if (b->inv_sy) q = -q;
            in[13] = (uint8_t)(int8_t)q;
            mask |= 2;
        }
    }
    return mask;
}

/* Rand (rad) aus dem Anteil der halben Bildbreite/-hoehe (pct 30..100) und der Projektion py der Weltkamera
 * (Fadenkreuz-NDC = -py tan(Winkel), W376_A9). */
static double laa_edge(int pct, double py)
{
    if (!(py > 0.05 && py < 100.0)) py = 1.7320508075688772; /* Rueckfall: 60 Grad Bildwinkel */
    return atan((double)pct / 100.0 / py);
}

/* ---- Hook-API (loc_aimassist.c) ---- */
/* Spiel-Thread, je SBC-Abfrage in der Mission (hooks_input.c, nach der Abbildung, vor dem Eingabewort): blk = eigener
 * SBC-Block (imap_block_t *), mouse_axes = Achsen aus der Maus (m_aim_src & 3). */
void loc_aimassist_poll(void *blk, unsigned mouse_axes);
/* Render-Thread (W394 Sicht je Bild): Vorschau-Satz F (naechster Poll) mit der Zielhilfe ergaenzen, Basis pst, juengster
 * eigener Satz last_in. Rueckgabe 1 = veraendert. */
int loc_aimassist_preview(const la_state_t *pst, const uint8_t last_in[14], uint8_t F[14]);
/* Render-Thread: Projektion der Weltkamera (py) fuer den Rand. */
void loc_aimassist_note_py(double py);
/* Zaehlerzeile LOCAA (alle 30 s aus dem Hook, Missionsende). */
void loc_aimassist_log(void);
