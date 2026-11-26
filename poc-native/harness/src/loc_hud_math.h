/* loc_hud_math.h - W394 LOC-HUD-VIEW / LOC-VIEW-PERFRAME: reine Rechnungen (ohne Spielspeicher; Selbsttest
 * render_selftest "LOCHUD").
 *
 * Belegt (W394, DIAG-Paare Block/Satz in den Laeufen C2/C3): Der 14-Byte-Eingabesatz traegt den Zielhebel als Stufen wie in
 * SB (0x7F und 0x80 = 0, darueber v - 0x80, darunter v - 0x7F), y mit umgekehrtem Vorzeichen; Sicht x unveraendert, Sicht y
 * bitweise invertiert (Ruhewert -1). */
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "loc_aim_law.h"

static int lh_conv_aim(int v) { return v >= 0x80 ? v - 0x80 : v - 0x7F; }
static int8_t lh_s8(int v) { return (int8_t)(v > 127 ? 127 : (v < -128 ? -128 : v)); }

/* Satz-Bytes 10..13 aus den Block-Achsen (aiming_x/y uint8, sight_x/y int8). */
static void lh_axes_to_frame(int ax, int ay, int sx, int sy, uint8_t out4[4])
{
    out4[0] = (uint8_t)lh_s8(lh_conv_aim(ax));
    out4[1] = (uint8_t)lh_s8(-lh_conv_aim(ay));
    out4[2] = (uint8_t)lh_s8(sx);
    out4[3] = (uint8_t)lh_s8(~sy);
}

/* Vorschau-Satz: base = Satz des letzten Polls, fin = dessen Block-Achsen (ax, ay, sx, sy wie im Block), neue Achsen
 * ax..sy; nur Achsen mit have[k] werden ersetzt (Satz + Aenderung, damit unbekannte Zusatzwirkungen des Spiels erhalten
 * bleiben). */
static void lh_frame(const uint8_t base[14], const uint8_t fin[4], int ax, int ay, int sx, int sy, const int have[4],
                     uint8_t out[14])
{
    memcpy(out, base, 14);
    if (have[0]) out[10] = (uint8_t)lh_s8((int8_t)base[10] + lh_conv_aim(ax) - lh_conv_aim(fin[0]));
    if (have[1]) out[11] = (uint8_t)lh_s8((int8_t)base[11] - (lh_conv_aim(ay) - lh_conv_aim(fin[1])));
    if (have[2]) out[12] = (uint8_t)lh_s8((int8_t)base[12] + sx - (int8_t)fin[2]);
    if (have[3]) out[13] = (uint8_t)lh_s8((int8_t)base[13] - (sy - (int8_t)fin[3]));
}

/* Gewicht des Zusatz-Schritts: Zeit seit der Vorhersage / Spanne bis zum naechsten Poll, 0..1. */
static double lh_weight(double ms_since_pub, double span_ms)
{
    if (!(span_ms > 1.0)) span_ms = 50.0;
    return la_clamp(ms_since_pub / span_ms, 0.0, 1.0);
}

/* Angezeigte Sicht/Arm: p + w * (s1 - p) je Winkel (mit Umbruch); v = {Sicht-Gier, -Nick, Arm x, y}. */
static void lh_blend(const la_state_t *p, const la_state_t *s1, double w, double v[4])
{
    v[0] = la_wrap(p->ys + w * la_wrap(s1->ys - p->ys));
    v[1] = la_wrap(p->ps + w * la_wrap(s1->ps - p->ps));
    v[2] = la_wrap(p->ax + w * la_wrap(s1->ax - p->ax));
    v[3] = la_wrap(p->ay + w * la_wrap(s1->ay - p->ay));
}

/* Marke: angezeigte Lage c (Clip, [q, 1] * VP), optional Sicht-Korrektur C (Zeilenvektor), simulierte Bildlage a.
 * Rueckgabe 0 = hinter der Kamera; sonst Verschiebung (NDC, je Achse auf +-0,5 begrenzt). */
static int lh_mark_shift(const double c[4], const float *C, double ax, double ay, double *dx, double *dy)
{
    double b[4];
    int k;
    for (k = 0; k < 4; k++) b[k] = C ? c[0] * C[k] + c[1] * C[4 + k] + c[2] * C[8 + k] + c[3] * C[12 + k] : c[k];
    if (b[3] < 1e-3) return 0;
    *dx = la_clamp(b[0] / b[3] - ax, -0.5, 0.5);
    *dy = la_clamp(b[1] / b[3] - ay, -0.5, 0.5);
    return 1;
}
/* W439 LOC-RETICLE-JUMP: Verschiebung fuer Fadenkreuz-Draws, die im Zwischenbild exakt aus Tick N (B) gezeichnet
 * werden. r = Soll - lerp(a, b, t) (Verschiebung fuer gelerpte Draws), a/b = simulierte Mitte in A/B. Gezeichnet liegt
 * der exakte Draw bei b, also braucht er Soll - b = r + lerp(a, b, t) - b. Ohne Zwischenbild (lerp 0) gilt r. */
static void lh_ret_exact_shift(double rx, double ry, const float a[2], const float b[2], double t, int lerp, double out[2])
{
    out[0] = rx;
    out[1] = ry;
    if (lerp) {
        out[0] = rx + ((double)a[0] + ((double)b[0] - a[0]) * t) - b[0];
        out[1] = ry + ((double)a[1] + ((double)b[1] - a[1]) * t) - b[1];
    }
}

/* W505 ISSUE-106: Vorschau-Rahmen fuer den Zusatz-Schritt der Sicht je Bild. Mit Legacy-Belegung kennt loc_vpf den
 * Satz des naechsten Polls aus dem Maus-Akku (lh_frame). Mit Eingabeprofil (input_profile, seit 0.4.0 Standard
 * default_full) gibt es diese Vorschau nicht; bisher fiel die Anzeige dann auf die Basis (Vorhersage des letzten Ticks)
 * zurueck - die gezeigte Sicht stand 5 von 6 Bildern still und sprang je Tick (Drehen wirkt wie 20 FPS). Jetzt gilt der
 * letzte eigene Satz als naechster Satz (gehaltener Hebel dreht weiter); die naechste echte Vorhersage korrigiert. */
static void lh_frame_repeat(const uint8_t base[14], uint8_t out[14])
{
    memcpy(out, base, 14);
}

/* W505: Gewicht des Zusatz-Schritts bei Wiederholung (Eingabeprofil): Bildzeit seit der Vorhersage / Abstand zweier
 * Vorhersagen, linear in [-0,5, 1,5] (nicht auf 0..1 begrenzt): Mit dem naechsten Tick wird s1 zur neuen Basis und w
 * faellt um genau 1 - die gezeigte Sicht bleibt stetig, auch fuer Bilder kurz vor bzw. nach der Veroeffentlichung. */
static double lh_rep_weight(double ms_since_pub, double tick_ms)
{
    if (!(tick_ms > 1.0)) tick_ms = 50.0;
    return la_clamp(ms_since_pub / tick_ms, -0.5, 1.5);
}

/* W505: Gleichmaessigkeit der gezeigten Sicht-Gier je Bild in Drehphasen (Zaehler fuer LOCVPF-GLATT und Selbsttest).
 * Ring der letzten 7 Bilder (~1 Tick bei 120 FPS): Drehrate ueber den Ring; bewertet wird der mittlere Schritt gegen
 * Rate * Bildabstand: stehend < 20 %, Sprung > 200 %. Bilder mit Abstand < 2 ms ersetzen das letzte (doppelte Wiedergabe). */
typedef struct {
    double y[7], ms[7];
    int n;
    unsigned turn, stall, jump;
    double rel_sum;
} lh_smooth_t;

static void lh_smooth_add(lh_smooth_t *s, double ms, double yaw)
{
    int k;
    if (s->n > 0 && ms - s->ms[s->n - 1] < 2.0) { /* gleiches Bild: letzte Probe ersetzen */
        s->ms[s->n - 1] = ms;
        s->y[s->n - 1] = s->n > 1 ? s->y[s->n - 2] + la_wrap(yaw - s->y[s->n - 2]) : yaw;
        return;
    }
    if (s->n > 0 && ms - s->ms[s->n - 1] > 100.0) s->n = 0; /* Luecke (Pause, Menue) */
    if (s->n == 7) {
        for (k = 0; k < 6; k++) { s->y[k] = s->y[k + 1]; s->ms[k] = s->ms[k + 1]; }
        s->n = 6;
    }
    s->ms[s->n] = ms;
    s->y[s->n] = s->n ? s->y[s->n - 1] + la_wrap(yaw - s->y[s->n - 1]) : yaw; /* stetig (ohne Umbruch) */
    s->n++;
    if (s->n == 7 && s->ms[6] > s->ms[0]) {
        double rate = (s->y[6] - s->y[0]) / (s->ms[6] - s->ms[0]); /* rad/ms */
        double exp = fabs(rate) * (s->ms[3] - s->ms[2]), d = fabs(s->y[3] - s->y[2]);
        if (fabs(rate) * 1000.0 > 0.2 && exp > 0.0) { /* Drehung > 0,2 rad/s */
            s->turn++;
            s->rel_sum += d / exp;
            if (d < 0.2 * exp) s->stall++;
            else if (d > 2.0 * exp) s->jump++;
        }
    }
}
