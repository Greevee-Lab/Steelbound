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
