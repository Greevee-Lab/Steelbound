/* loc_aim_law.h - W376 LOC-LOCAL-AIM: Gesetz Eingabe -> Sicht/Waffenarm der LoC-Simulation (rein, ohne Spielspeicher;
 * Selbsttest render_selftest "LOCAIM").
 *
 * Belegt (Disassembly/Decompilat der LoC-XBE, LoC-VAs; Messlauf W376_A0 Tick fuer Tick):
 *  - Eingabesatz (14 B, FUN_0001CAE0) -> je Spieler aufgeweitet (FUN_0001B870); die Simulation liest Achsen per
 *    FUN_0001A330(spieler, 0, 0, index): 0x10 Drehhebel (Byte 9), 0x12/0x13 Zielhebel (Byte 10/11), 0x24/0x25
 *    Sichthebel (Byte 12/13), alle int8. Zoomtasten = Index 2/3 (Byte 0 Bit 2/3), Stick-Klick = Index 0x1A (Byte 2 Bit 0).
 *  - VT-Objekt = 0x5C50D0 + Spieler * 0xAE8 (Feld +0x92 = Spielermaske, +0x98 Flags = W341-Spielerflags).
 *  - Sicht (Funktion um 0xB2D02): Zoom z = +0x414 (0..55, Tasten +-2 je Tick; Flag +0x98 Bit 4: z = 55);
 *    Rate = (60 - z) / 55 * 0,0005 * Hebel (Flag Bit 4: * 0,3), Nick-Hebel mit +0x53C == 1 negiert;
 *    Soll-Gier +0x40C = klemme(wrap(Soll - Rate_x), +-pi/2), Soll-Nick +0x410 = klemme(wrap(Soll - Rate_y), +-pi/4);
 *    sichtbar (geglaettet) +0x404/+0x408 = wrap(alt + tz(0,4 * (Soll - alt))), tz = 0 unter 0,05 Grad.
 *    +0x94 Bit 0x20 (Stick-Klick u. a.): Soll 0, Annaeherung an 0 -> keine Vorhersage.
 *    Totzone (FUN_0001B0A0 nach dem Aufweiten): |Sichthebel| < 25,6 -> 0 (Ruhewert Nick -1 wie SB "~sight_y"),
 *    ebenso Drehhebel; der Zielhebel hat keine Totzone (Messlauf W376_A6: Rest-Delta 0,0009 rad ohne sie).
 *  - Waffenarm (0xAB1EF..0xAB3D6): Soll_x = Hebel_x * -0,0163625, Soll_y = Hebel_y * 0,0061359 (+0x53A == 1:
 *    negiert); g = wrap(alt + d * min(|d| * K, 1,5)) mit d = Soll - alt, K = 0,238732 (x) bzw. 0,63662 (y);
 *    neu = naehere(g, alt, max) mit max = +0x270 (x; +0x94 Bit 0x10000: * 0,8) bzw. 0,05236 (y). Ergebnis +0x27C (x)
 *    bzw. +0x278 (y). Flag +0x98 Bit 4: Arm folgt der Sicht (naehere an +0x404 bzw. -+0x408, Schritt pi).
 *  - Drehhebel (0xAC23E): setzt nur Bewegungszustaende (Drehung ueber die Animations-/Bewegungs-Zustandsmaschine)
 *    -> nicht vorhergesagt (Teil 2).
 * Double statt x87-float: Abweichung je Tick < 1e-6 rad (nur Bild). */
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

#define LA_PI 3.14159265358979323846

typedef struct {
    double yt, pt;  /* Soll-Gier/-Nick der Sicht (+0x40C/+0x410) */
    double ys, ps;  /* sichtbare Sicht (+0x404/+0x408) */
    double zoom;    /* +0x414 */
    double ax, ay;  /* Waffenarm (+0x27C/+0x278) */
    double arate;   /* +0x270 */
    uint32_t f94, f98, fa0;
    int inv_sy, inv_ay; /* +0x53C == 1, +0x53A == 1 */
} la_state_t;

static double la_wrap(double a)
{
    while (a > LA_PI) a -= 2.0 * LA_PI;
    while (a <= -LA_PI) a += 2.0 * LA_PI;
    return a;
}
static double la_clamp(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
/* FUN_000C9EB0(ziel, alt, max): Schritt hoechstens max */
static double la_toward(double target, double cur, double mx)
{
    double d = target - cur;
    if (d > 0.0 && d > mx) return la_wrap(cur + mx);
    if (d < 0.0 && -d > mx) return la_wrap(cur - mx);
    return target;
}
static double la_dz(double v) { return fabs(v) < 0.0008726646774448454 ? 0.0 : v; }

/* Vorhersage moeglich (Zustand ohne Sonderpfad)? */
static int la_can(const la_state_t *s)
{
    return (s->fa0 & 0x4000u) != 0u && (s->f94 & 0x20u) == 0u && (s->f94 & 8u) == 0u;
}

/* Ein Tick mit Eingabesatz in (14 Byte). Rueckgabe 0 = Sonderfall (Stick-Klick), Zustand dann unveraendert. */
static int la_step(la_state_t *s, const uint8_t in[14])
{
    double k, rx, ry, d, g;
    int8_t sx = (int8_t)in[12], sy = (int8_t)in[13], hx = (int8_t)in[10], hy = (int8_t)in[11];
    if (in[2] & 1u) return 0; /* Stick-Klick: Ruecksetzen der Sicht */
    if ((double)(sx < 0 ? -sx : sx) < 25.600000381469727) sx = 0; /* FUN_0001B0A0 */
    if ((double)(sy < 0 ? -sy : sy) < 25.600000381469727) sy = 0;
    if (!(s->f98 & 4u)) {
        if (in[0] & 8u) s->zoom += 2.0;
        if (in[0] & 4u) s->zoom -= 2.0;
        s->zoom = la_clamp(s->zoom, 0.0, 55.0);
    } else {
        s->zoom = 55.0;
    }
    k = (60.0 - s->zoom) * 0.0181818176060915 * 0.0005000000237487257;
    rx = k * (double)sx;
    if (s->f98 & 4u) rx *= 0.30000001192092896;
    s->yt = la_clamp(la_wrap(s->yt - rx), -1.5707963705062866, 1.5707963705062866);
    ry = k * (double)(s->inv_sy ? -sy : sy);
    if (s->f98 & 4u) ry *= 0.30000001192092896;
    s->pt = la_clamp(la_wrap(s->pt - ry), -0.7853981852531433, 0.7853981852531433);
    s->ps = la_wrap(s->ps + la_dz((s->pt - s->ps) * 0.4000000059604645));
    s->ys = la_wrap(s->ys + la_dz((s->yt - s->ys) * 0.4000000059604645));
    if (!(s->f98 & 4u)) {
        d = (double)hx * -0.01636246219277382 - s->ax;
        g = la_wrap(s->ax + d * la_clamp(fabs(d) * 0.23873241245746613, 0.0, 1.5));
        s->ax = la_toward(g, s->ax, s->arate * ((s->f94 & 0x10000u) ? 0.800000011920929 : 1.0));
        d = (double)(s->inv_ay ? -hy : hy) * 0.006135923322290182 - s->ay;
        g = la_wrap(s->ay + d * la_clamp(fabs(d) * 0.6366197466850281, 0.0, 1.5));
        s->ay = la_toward(g, s->ay, 0.05235987901687622);
    } else {
        s->ay = la_toward(-s->ps, s->ay, LA_PI);
        s->ax = la_toward(s->ys, s->ax, LA_PI);
    }
    return 1;
}

/* ---- Kamera-Korrektur fuer Welt-Draws (Zeilenvektor, clip = [p,1] * M) ----
 * Kamera VP = T(-e) R P. Die Sicht dreht die Kamera um das Auge e: Gier um die Welt-Hochachse up (Einheitsvektor),
 * Nick um die Kamera-Querachse. Neue Kamera VP' = G^-1 * VP (G = Drehung um e), fuer jedes Objekt MVP' = MVP * C
 * mit C = VP^-1 * G^-1 * VP. Rueckgabe 0, wenn VP nicht invertierbar ist. */
static int la_m4inv_d(double o[16], const double m[16])
{
    double inv[16], det;
    int i;
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (fabs(det) < 1e-30) return 0;
    det = 1.0 / det;
    for (i = 0; i < 16; i++) o[i] = inv[i] * det;
    return 1;
}
static void la_m4mul_d(double r[16], const double a[16], const double b[16])
{
    int i, j;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
}
static void la_norm3(double v[3])
{
    double n = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (n > 0.0) { v[0] /= n; v[1] /= n; v[2] /= n; }
}
/* Drehmatrix (Spaltenvektor v' = R v) um Einheitsachse a, Winkel w (Rodrigues) */
static void la_rot3(double R[9], const double a[3], double w)
{
    double c = cos(w), s = sin(w), t = 1.0 - c;
    R[0] = t * a[0] * a[0] + c;        R[1] = t * a[0] * a[1] - s * a[2]; R[2] = t * a[0] * a[2] + s * a[1];
    R[3] = t * a[0] * a[1] + s * a[2]; R[4] = t * a[1] * a[1] + c;        R[5] = t * a[1] * a[2] - s * a[0];
    R[6] = t * a[0] * a[2] - s * a[1]; R[7] = t * a[1] * a[2] + s * a[0]; R[8] = t * a[2] * a[2] + c;
}
/* Kamera-Achsen aus VP: Blick f (w-Spalte), Rechts r (x-Spalte orthogonal zu f), Auge e. */
static int la_cam_axes(const double VP[16], double f[3], double r[3], double e[3])
{
    double A[16], Ai[16], d;
    int i;
    f[0] = VP[3]; f[1] = VP[7]; f[2] = VP[11];
    la_norm3(f);
    r[0] = VP[0]; r[1] = VP[4]; r[2] = VP[8];
    d = r[0] * f[0] + r[1] * f[1] + r[2] * f[2];
    for (i = 0; i < 3; i++) r[i] -= d * f[i];
    la_norm3(r);
    /* Auge: [e,1] * VP hat x = y = w = 0 */
    memset(A, 0, sizeof A);
    A[0] = VP[0]; A[1] = VP[4]; A[2] = VP[8];
    A[4] = VP[1]; A[5] = VP[5]; A[6] = VP[9];
    A[8] = VP[3]; A[9] = VP[7]; A[10] = VP[11];
    A[15] = 1.0;
    if (!la_m4inv_d(Ai, A)) return 0;
    for (i = 0; i < 3; i++) e[i] = -(Ai[i * 4] * VP[12] + Ai[i * 4 + 1] * VP[13] + Ai[i * 4 + 2] * VP[15]);
    return 1;
}
static void la_cam_yawpitch(const double VP[16], double *yaw, double *pitch)
{
    double f[3] = {VP[3], VP[7], VP[11]};
    la_norm3(f);
    *yaw = atan2(f[0], f[2]);
    *pitch = asin(la_clamp(f[1], -1.0, 1.0));
}
/* turn = Drehung nach Bild-rechts (rad), nick = nach Bild-oben; unabhaengig von der Haendigkeit der Welt (LoC: Welt
 * rechtshaendig, Bild-rechts = -x). Gier um die Welt-Hochachse up (Vorzeichen so, dass f nach rechts dreht), Nick um
 * f x u (f dreht zur Kamera-Hochachse u). */
static int la_cam_corr(double C[16], const double VP[16], const double up[3], double turn, double nick)
{
    double f[3], r[3], e[3], u[3], Ry[9], Rp[9], G[9], W[16], VPi[16], T[16], ay[3], ap[3], d;
    int i, j;
    if (!la_cam_axes(VP, f, r, e) || !la_m4inv_d(VPi, VP)) return 0;
    u[0] = VP[1]; u[1] = VP[5]; u[2] = VP[9];
    d = u[0] * f[0] + u[1] * f[1] + u[2] * f[2];
    for (i = 0; i < 3; i++) u[i] -= d * f[i];
    la_norm3(u);
    /* Rodrigues: R f = f cos w + (a x f) sin w -> a x f = r fuer "rechts": a = f x r (auf up projiziert) */
    ay[0] = f[1] * r[2] - f[2] * r[1]; ay[1] = f[2] * r[0] - f[0] * r[2]; ay[2] = f[0] * r[1] - f[1] * r[0];
    d = ay[0] * up[0] + ay[1] * up[1] + ay[2] * up[2];
    for (i = 0; i < 3; i++) ay[i] = d >= 0.0 ? up[i] : -up[i];
    la_rot3(Ry, ay, turn);
    ap[0] = f[1] * u[2] - f[2] * u[1]; ap[1] = f[2] * u[0] - f[0] * u[2]; ap[2] = f[0] * u[1] - f[1] * u[0];
    la_norm3(ap);
    la_rot3(Rp, ap, nick);
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++) G[i * 3 + j] = Ry[i * 3] * Rp[j] + Ry[i * 3 + 1] * Rp[3 + j] + Ry[i * 3 + 2] * Rp[6 + j];
    /* G = Kameradrehung (Spaltenvektor). Punkt im neuen Bild = alter Punkt G^-1 (p - e) + e; Zeilenvektor:
     * (p - e) * G + e (G^-1 transponiert = G). */
    memset(W, 0, sizeof W);
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++) W[i * 4 + j] = G[i * 3 + j];
    W[15] = 1.0;
    for (j = 0; j < 3; j++) W[12 + j] = e[j] - (e[0] * G[j] + e[1] * G[3 + j] + e[2] * G[6 + j]);
    la_m4mul_d(T, VPi, W);
    la_m4mul_d(C, T, VP);
    return 1;
}
