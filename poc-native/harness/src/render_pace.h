/* render_pace.h - W393 LOC-PACING: reine Hilfen fuer den Bildtakt (ohne Windows/D3D, auch render_selftest).
 *
 * 1. Kennzahlen eines Messfensters (pace_diag): Bildabstand p50/p99/max, Anteil ausserhalb +-1 ms vom Soll,
 *    gezeigte Spielzeit je Bild (Delta g in Ticks): Median, p99-Abweichung, doppelte Phase (g steht), Spruenge.
 * 2. Stetige Bilduhr (interp_pace_clock): Die Darstellungs-Latenz L (Faelligkeit - L = gezeigte Zustandszeit)
 *    folgt ihrem Ziel nur mit begrenzter Geschwindigkeit. Bisher sprang L (Maximum der letzten 32 Ticks) um
 *    bis zu ~30 ms, wenn ein spaeter Tick ins Fenster kam oder herausfiel; die gezeigte Spielzeit sprang dann
 *    vor bzw. zurueck (Ruck). Jetzt laeuft die Bilduhr hoechstens um up/dn Promille langsamer bzw. schneller.
 *    Grosse Spruenge (> snap) uebernimmt sie sofort (Haenger: ein Ruck ist dann ohnehin unvermeidbar). */
#ifndef RENDER_PACE_H
#define RENDER_PACE_H

#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct {
    int n;              /* Werte */
    double p50, p99, mx; /* Bildabstand (ms) */
    int out1;           /* |Abstand - Soll| > 1 ms */
} pace_ft_t;

typedef struct {
    int n;
    double med;         /* Median Delta g (Ticks je Bild) */
    double dev99;       /* p99 |Delta g / Median - 1| (Anteil) */
    int same;           /* Delta g <= 1e-4 Ticks: gleiche Zwischenbild-Phase doppelt */
    int jump;           /* Delta g > 1,5 x Median */
} pace_g_t;

static int pace_cmp_f(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* Quantil q (0..1) einer sortierten Folge (naechster Rang). */
static double pace_q(const float *s, int n, double q)
{
    int i;
    if (n <= 0) { return 0.0; }
    i = (int)(q * (double)(n - 1) + 0.5);
    if (i < 0) { i = 0; }
    if (i > n - 1) { i = n - 1; }
    return (double)s[i];
}

/* Bildabstaende v[0..n) in ms (wird sortiert), Soll in ms. */
static pace_ft_t pace_ft_sum(float *v, int n, double soll)
{
    pace_ft_t r;
    int i;
    r.n = n; r.p50 = r.p99 = r.mx = 0.0; r.out1 = 0;
    if (n <= 0) { return r; }
    for (i = 0; i < n; i++) {
        if (fabs((double)v[i] - soll) > 1.0) { r.out1++; }
    }
    qsort(v, (size_t)n, sizeof v[0], pace_cmp_f);
    r.p50 = pace_q(v, n, 0.50);
    r.p99 = pace_q(v, n, 0.99);
    r.mx = (double)v[n - 1];
    return r;
}

/* Delta g je Bild (Ticks), v wird ueberschrieben (Abweichungen) und sortiert. */
static pace_g_t pace_g_sum(float *v, int n)
{
    pace_g_t r;
    int i;
    r.n = n; r.med = r.dev99 = 0.0; r.same = r.jump = 0;
    if (n <= 0) { return r; }
    for (i = 0; i < n; i++) {
        if (v[i] <= 1e-4f) { r.same++; }
    }
    qsort(v, (size_t)n, sizeof v[0], pace_cmp_f);
    r.med = pace_q(v, n, 0.50);
    if (r.med <= 1e-6) { return r; }
    for (i = 0; i < n; i++) {
        if ((double)v[i] > 1.5 * r.med) { r.jump++; }
        v[i] = (float)fabs((double)v[i] / r.med - 1.0);
    }
    qsort(v, (size_t)n, sizeof v[0], pace_cmp_f);
    r.dev99 = pace_q(v, n, 0.99);
    return r;
}

/* Stetige Bilduhr: neue Anzeige-Latenz aus der bisherigen (cur), dem Ziel (want) und der seit dem letzten Schritt
 * vergangenen Zeit dt (gleiche Einheit, z. B. QPC). up/dn = hoechste Aenderung je Zeiteinheit in Promille (L steigt:
 * Uhr langsamer; L faellt: Uhr schneller). snap: |want - cur| darueber sofort uebernehmen (0 = nie). cur <= 0 = Start. */
static long long pace_lat_step(long long cur, long long want, long long dt, int up, int dn, long long snap)
{
    long long d, lim;
    if (cur <= 0 || dt < 0) { return want; }
    d = want - cur;
    if (snap > 0 && (d > snap || -d > snap)) { return want; }
    if (d > 0) {
        lim = dt * up / 1000;
        if (d > lim) { d = lim; }
    } else {
        lim = dt * dn / 1000;
        if (-d > lim) { d = -lim; }
    }
    return cur + d;
}

/* Geglaettete Tick-Zeitbasis: Zustandszeit eines neuen Ticks aus der vorigen (prev), der gemessenen (raw, Feldraster)
 * und dem Takt P. Erwartung prev + P, Abweichung zu 1/8 (hoechstens P/25 je Tick) - so verschwindet einzelnes
 * Feld-Zittern (ein Tick ein Feld spaet, der naechste holt auf). Neu ansetzen (*snap = 1, raw): mehr als P/2 spaeter
 * bzw. frueher, oder zum zweiten Mal in Folge mehr als P/8 in dieselbe Richtung (*laste = vorige Abweichung). Dann hat
 * das Spiel dauerhaft Zeit verloren bzw. die Phase gewechselt - die Glaettung wuerde sonst ueber viele Ticks
 * nachhinken und die Anzeige-Latenz ueber ihre Grenze treiben (Dauer-Halten). Ergebnis immer > prev. */
static long long pace_ts_step(long long prev, long long raw, long long P, int *snap, long long *laste)
{
    long long pe = prev + P, e = raw - pe, c = e / 8, lim = P / 25, r, le = *laste;
    *snap = 0;
    *laste = e;
    if (P <= 0) { return raw > prev ? raw : prev + 1; }
    if (e > P / 2 || e < -P / 2 || (e > P / 8 && le > P / 8) || (e < -P / 8 && le < -P / 8)) {
        *snap = 1;
        *laste = 0;
        r = raw;
    } else {
        if (c > lim) { c = lim; }
        if (c < -lim) { c = -lim; }
        r = pe + c;
    }
    return r > prev ? r : prev + 1;
}

/* Elastische Bilduhr beim Halten: braucht die gezeigte Zeit die Latenz need (= Faelligkeit - Zustandszeit des
 * juengsten Ticks), um exakt beim juengsten Stand zu bleiben, und ist need groesser als die bisherige (cur), aber
 * unter Ziel + snap, gilt need. Sonst bleibt cur. */
static long long pace_lat_hold(long long cur, long long need, long long target, long long snap)
{
    return (need > cur && need < target + snap) ? need : cur;
}

/* Phase eines Zeitpunkts im Tick (0..nb-1): (t - t0) mod P in nb Faechern (P > 0). */
static int pace_phase_bin(long long t, long long t0, long long P, int nb)
{
    long long m;
    if (P <= 0 || nb <= 0) { return 0; }
    m = (t - t0) % P;
    if (m < 0) { m += P; }
    return (int)(m * nb / P);
}

/* VBlank-Kopplung: naechster Bildtermin auf dem Raster vb + off + j*step (j ganzzahlig, auch negativ) nach dem
 * Zeitpunkt after (vb = ein VBlank). Liefert den kleinsten Termin > after. */
static long long pace_vb_next(long long after, long long vb, long long step, long long off)
{
    long long b = vb + off, j;
    if (step <= 0) { return after + 1; }
    j = (after - b) / step;
    if (after - b < 0 && (after - b) % step) { j--; } /* abrunden */
    return b + (j + 1) * step;
}

/* Raster zur Bildrate fps und VBlank-Periode per (QPC): k VBlanks je Bild bzw. m Bilder je VBlank (ganzzahlig +-3 %).
 * step/off: Bildabstand und Lage im VBlank-Intervall (Mitte zwischen zwei VBlanks bzw. gleichmaessig dazwischen, nie
 * auf dem VBlank). 0 = keine ganzzahlige Beziehung (Timer bleibt). */
static int pace_vb_grid(double fps, long long per, long long qpf, long long *step, long long *off)
{
    double hz, r;
    long long ri;
    if (fps <= 0.0 || per <= 0 || qpf <= 0) { return 0; }
    hz = (double)qpf / (double)per;
    r = hz / fps;
    if (r >= 0.97) { /* k VBlanks je Bild */
        ri = (long long)(r + 0.5);
        if (ri < 1 || fabs(r - (double)ri) > 0.03 * (double)ri) { return 0; }
        *step = per * ri;
        *off = per / 2;
    } else { /* m Bilder je VBlank */
        r = fps / hz;
        ri = (long long)(r + 0.5);
        if (ri < 2 || ri > 4 || fabs(r - (double)ri) > 0.03 * (double)ri) { return 0; }
        *step = per / ri;
        *off = *step / 2;
    }
    return 1;
}

/* 3. Letzter fertiger Stand ohne Sperre (ein Schreiber, beliebig viele Leser): drei Plaetze und eine Folgenummer.
 *    Der Schreiber fuellt Platz (q+1)%3 und gibt dann q+1 frei; ein Leser kopiert Platz q%3 und prueft danach, dass
 *    hoechstens eine weitere Freigabe kam (dann schreibt der Schreiber hoechstens Platz (q+2)%3, nie q%3). Mit nur
 *    zwei Plaetzen (bisher loc_smooth) konnte eine zweite Freigabe waehrend des Lesens (Live-Tick + Vorlauf ~1 ms
 *    auseinander) den gelesenen Platz ueberschreiben. Der Leser wartet nie; scheitern 4 Versuche, liefert er 0. */
#define PACE_PUB_SLOTS 3
static void pace_pub_write(volatile LONG *seq, void *slots, size_t sz, const void *v)
{
    LONG q = *seq + 1;
    memcpy((char *)slots + (size_t)(q % PACE_PUB_SLOTS) * sz, v, sz);
    InterlockedExchange(seq, q); /* volle Barriere: Inhalt vor der Freigabe sichtbar */
}
static int pace_pub_read(volatile LONG *seq, const void *slots, size_t sz, void *out)
{
    int k;
    for (k = 0; k < 4; k++) {
        LONG q = *seq, q2;
        if (!q) { return 0; }
        _ReadWriteBarrier();
        memcpy(out, (const char *)slots + (size_t)(q % PACE_PUB_SLOTS) * sz, sz);
        _ReadWriteBarrier();
        q2 = *seq;
        if ((LONG)(q2 - q) <= 1) { return 1; }
    }
    return 0;
}

#endif
