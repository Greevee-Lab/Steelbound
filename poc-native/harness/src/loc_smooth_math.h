/* loc_smooth_math.h - W385 LOC-SMOOTH: reine Mathematik der weichen Rollback-Korrektur (ohne Spielspeicher, ohne D3D;
 * Selbsttest render_selftest "LOCSMOOTH").
 *
 * Nach einem Rollback springt die simulierte Lage eines VT vom vorhergesagten auf den korrigierten Stand desselben
 * Spielticks. Nur die Anzeige zeigt den Sprung abklingend: Versatz E = vorhergesagt - korrigiert (Weltlage x/y/z in
 * Spieleinheiten, Gier in rad), Gewicht je folgender Liste w_k = 1 - k/N (k = 0 erste Liste nach dem Rollback, N
 * Listen ~ Glaettungsdauer / 50 ms). Die Zwischenbilder interpolieren zwischen den Listen -> lineares Abklingen ueber
 * N Ticks. Ein weiterer Rollback waehrend des Abklingens addiert seinen Versatz zum Rest (Anzeige bleibt stetig).
 *
 * Matrizen: Zeilenvektor wie im Spiel (clip = [p,1] * MVP), Element m[r*4+c], double.
 * Gier wie im Spiel (W380 loc_dodge): Vorwaerts = (sin a, 0, cos a) in (x, z); Ry(d) dreht Gier a nach a + d. */
#pragma once
#include <math.h>
#include <string.h>

#define LS_PI 3.14159265358979323846

typedef struct {
    double off0[4]; /* Versatz bei k = 0: dx, dy, dz, dGier */
    int k;          /* naechster Schritt (0 = erste Liste nach dem Rollback) */
    int n;          /* Schritte bis 0 */
} ls_track_t;

static double ls_wrap(double a)
{
    while (a > LS_PI) a -= 2.0 * LS_PI;
    while (a < -LS_PI) a += 2.0 * LS_PI;
    return a;
}

/* Gewicht der k-ten Liste nach dem Rollback (k = 0 -> 1, k >= n -> 0). */
static double ls_weight(int k, int n)
{
    if (n < 1 || k >= n) return 0.0;
    if (k <= 0) return 1.0;
    return 1.0 - (double)k / (double)n;
}

/* Aktueller (zuletzt veroeffentlichter) Versatz: Gewicht des vorigen Schritts. */
static void ls_track_cur(const ls_track_t *t, double out[4])
{
    double w = t->k > 0 ? ls_weight(t->k - 1, t->n) : 0.0;
    int i;
    for (i = 0; i < 4; i++) out[i] = t->off0[i] * w;
}

/* Neue Korrektur E (vorhergesagt - korrigiert): Rest + E, Abklingen beginnt neu mit n Schritten. */
static void ls_track_add(ls_track_t *t, const double e[4], int n)
{
    double cur[4];
    int i;
    ls_track_cur(t, cur);
    for (i = 0; i < 4; i++) t->off0[i] = cur[i] + e[i];
    t->off0[3] = ls_wrap(t->off0[3]);
    t->k = 0;
    t->n = n < 1 ? 1 : n;
}

static void ls_track_reset(ls_track_t *t) { memset(t, 0, sizeof *t); }

/* Versatz fuer die naechste Liste; schaltet einen Schritt weiter. Rueckgabe 1 = Versatz != 0. */
static int ls_track_step(ls_track_t *t, double out[4])
{
    double w = ls_weight(t->k, t->n);
    int i, nz = 0;
    for (i = 0; i < 4; i++) {
        out[i] = t->off0[i] * w;
        if (out[i] != 0.0) nz = 1;
    }
    if (t->k < t->n) t->k++;
    if (!nz) ls_track_reset(t);
    return nz;
}

static void ls_m4_id(double m[16])
{
    memset(m, 0, 16 * sizeof m[0]);
    m[0] = m[5] = m[10] = m[15] = 1.0;
}
static void ls_m4_mul(double o[16], const double a[16], const double b[16])
{
    double t[16];
    int r, c;
    for (r = 0; r < 4; r++)
        for (c = 0; c < 4; c++)
            t[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] + a[r * 4 + 3] * b[12 + c];
    memcpy(o, t, sizeof t);
}
static void ls_m4_trans(double m[16], double x, double y, double z)
{
    ls_m4_id(m);
    m[12] = x;
    m[13] = y;
    m[14] = z;
}
/* Ry(d): x' = x cos d + z sin d, z' = -x sin d + z cos d (Gier a -> a + d). */
static void ls_m4_yaw(double m[16], double d)
{
    double c = cos(d), s = sin(d);
    ls_m4_id(m);
    m[0] = c;
    m[2] = -s;
    m[8] = s;
    m[10] = c;
}

/* Objekt eines anderen Spielers: korrigierte Lage p, Versatz off -> Weltmatrix-Zusatz X = T(-p) Ry(dG) T(p + E)
 * (W' = W * X: um den eigenen Mittelpunkt drehen, dann verschieben). */
static void ls_obj_xform(double X[16], const double p[3], const double off[4])
{
    double a[16], b[16];
    ls_m4_trans(a, -p[0], -p[1], -p[2]);
    ls_m4_yaw(b, off[3]);
    ls_m4_mul(a, a, b);
    ls_m4_trans(b, p[0] + off[0], p[1] + off[1], p[2] + off[2]);
    ls_m4_mul(X, a, b);
}

/* Eigene Kamera (Auge e, Versatz off des eigenen VT): die Welt so bewegen, dass die echte Kamera sie wie von der
 * vorhergesagten Kamera (Auge e + E, Gier + dG) sieht: K = T(-E - e) Ry(-dG) T(e). */
static void ls_cam_xform(double K[16], const double e[3], const double off[4])
{
    double a[16], b[16];
    ls_m4_trans(a, -off[0] - e[0], -off[1] - e[1], -off[2] - e[2]);
    ls_m4_yaw(b, -off[3]);
    ls_m4_mul(a, a, b);
    ls_m4_trans(b, e[0], e[1], e[2]);
    ls_m4_mul(K, a, b);
}

/* Punkt (Zeilenvektor, w = 1) mal Matrix, Ergebnis xyz (affin). */
static void ls_xform_pt(double o[3], const double p[3], const double m[16])
{
    int c;
    double t[3];
    for (c = 0; c < 3; c++) t[c] = p[0] * m[c] + p[1] * m[4 + c] + p[2] * m[8 + c] + m[12 + c];
    memcpy(o, t, sizeof t);
}

/* Auge aus der Kamera VP (Zeilenvektor): Sichtursprung -> clip (0, 0, b, 0), also Auge = Zeile 2 von VP^-1 / w.
 * Rueckgabe 0, wenn w ~ 0. */
static int ls_eye_of(double e[3], const double VPi[16])
{
    double w = VPi[11];
    if (fabs(w) < 1e-12) return 0;
    e[0] = VPi[8] / w;
    e[1] = VPi[9] / w;
    e[2] = VPi[10] / w;
    return 1;
}

/* ======================================================================================================================
 * W387 LOC-SMOOTH-OPP: andere VTs leicht in der Vergangenheit zeigen (nur Anzeige, wie Overwatch/Rocket League).
 *
 * Je VT eine kurze Lagen-Historie je Spieltick (live und aus der Neurechnung, also immer der juengste Stand). Gezeigt
 * wird die Lage bei T - d (d in Ticks, Bruchteil linear zwischen zwei Ticks, Gier kuerzester Weg). Ein Tick, der d
 * Ticks zurueckliegt, ist gegen Korrekturen der Tiefe <= d gefeit: Ein Rollback der Tiefe k aendert nur T-k+1..T.
 * Der Versatz fuer die Liste ist off = Lage(T - d) - Lage(T) (wie W385: X = T(-p) Ry(dG) T(p + E)).
 *
 * Auto-Regelung (loc_smooth_opp = 1): Je Korrektur "Bedarf" m = kleinste Verzoegerung (0..3 Ticks), bei der die
 * angezeigte Lage keinen sichtbaren Sprung (>= vis) gemacht haette. Fenster 15 s: >= 2 Ereignisse mit Bedarf >= 1 ->
 * Ziel 1; >= 3 mit Bedarf >= 2 -> Ziel 2 (wenn erlaubt). Gesenkt wird erst nach weiteren 10 s ohne solche Ereignisse
 * (Hysterese). d folgt dem Ziel mit hoechstens 1/20 Tick je Tick (auf) bzw. 1/40 (ab): die Gegner laufen dabei
 * hoechstens 5 % langsamer bzw. 2,5 % schneller, nie ein Sprung. Ganz ohne sichtbare Korrekturen geht d gegen 0.
 * ==================================================================================================================== */
#define LS_HN 8u      /* Ring je VT (Ticks), Index Spieltick & (LS_HN - 1) */
#define LS_AW 300u    /* Auto: Fenster in Ticks (15 s) */
#define LS_AHOLD 200u /* Auto: ruhige Ticks nach dem Fenster vor dem Absenken (10 s) */
#define LS_AEV 64u    /* Auto: gemerkte Ereignisse */
#define LS_AUP (1.0 / 20.0)
#define LS_ADN (1.0 / 40.0)
#define LS_YAW_ARM 2000.0 /* Gier-Sprung -> Weg (Hebel ~2 m) fuer die Sichtbarkeit */

typedef struct {
    unsigned tick;
    int ok;
    unsigned obj;
    double p[4]; /* x, y, z, Gier */
} ls_hent_t;
typedef struct {
    ls_hent_t e[LS_HN];
} ls_hist_t;

static void ls_hist_reset(ls_hist_t *h) { memset(h, 0, sizeof *h); }

static void ls_hist_put(ls_hist_t *h, unsigned tick, unsigned obj, const double p[4])
{
    ls_hent_t *e = &h->e[tick & (LS_HN - 1u)];
    e->tick = tick;
    e->ok = 1;
    e->obj = obj;
    memcpy(e->p, p, sizeof e->p);
}

static const ls_hent_t *ls_hist_get(const ls_hist_t *h, unsigned tick, unsigned obj)
{
    const ls_hent_t *e = &h->e[tick & (LS_HN - 1u)];
    return (e->ok && e->tick == tick && e->obj == obj) ? e : (const ls_hent_t *)0;
}

/* Lage bei T - d (0 <= d <= LS_HN - 2). Fehlt der aeltere Tick, gilt der juengere (kleinere Verzoegerung); fehlt auch
 * dieser, 0. Rueckgabe 2 = vollstaendig, 1 = verkuerzt, 0 = nichts. */
static int ls_hist_at(const ls_hist_t *h, unsigned T, unsigned obj, double d, double out[4])
{
    unsigned lo;
    double f;
    const ls_hent_t *a, *b;
    int k;
    if (d < 0.0) d = 0.0;
    if (d > (double)(LS_HN - 2u)) d = (double)(LS_HN - 2u);
    lo = (unsigned)d;
    f = d - (double)lo;
    a = ls_hist_get(h, T - lo, obj);
    if (!a) { /* juengsten vorhandenen Tick dazwischen suchen */
        while (lo > 0u) {
            lo--;
            a = ls_hist_get(h, T - lo, obj);
            if (a) break;
        }
        if (!a) return 0;
        memcpy(out, a->p, 4 * sizeof out[0]);
        return 1;
    }
    if (f <= 0.0) {
        memcpy(out, a->p, 4 * sizeof out[0]);
        return 2;
    }
    b = ls_hist_get(h, T - lo - 1u, obj);
    if (!b) {
        memcpy(out, a->p, 4 * sizeof out[0]);
        return 1;
    }
    for (k = 0; k < 3; k++) out[k] = a->p[k] + (b->p[k] - a->p[k]) * f;
    out[3] = ls_wrap(a->p[3] + ls_wrap(b->p[3] - a->p[3]) * f);
    return 2;
}

/* Versatz der Anzeige: Lage(T - d) - Lage(T) (Gier gewickelt). Rueckgabe wie ls_hist_at (0 -> Versatz 0). */
static int ls_delay_off(const ls_hist_t *h, unsigned T, unsigned obj, double d, double off[4])
{
    double a[4], c[4];
    int r, k;
    memset(off, 0, 4 * sizeof off[0]);
    if (d <= 0.0) return 2;
    if (!ls_hist_at(h, T, obj, 0.0, c)) return 0;
    r = ls_hist_at(h, T, obj, d, a);
    if (!r) return 0;
    for (k = 0; k < 3; k++) off[k] = a[k] - c[k];
    off[3] = ls_wrap(a[3] - c[3]);
    return r;
}

/* Sprungweite eines Eintrags vorher/nachher (Weg + Gier * Hebel); fehlt einer, 0 (nie gezeigt bzw. neu). */
static double ls_hent_jump(const ls_hent_t *a, const ls_hent_t *b)
{
    double dx, dy, dz;
    if (!a || !b) return 0.0;
    dx = a->p[0] - b->p[0];
    dy = a->p[1] - b->p[1];
    dz = a->p[2] - b->p[2];
    return sqrt(dx * dx + dy * dy + dz * dz) + fabs(ls_wrap(a->p[3] - b->p[3])) * LS_YAW_ARM;
}

/* Bedarf einer Korrektur: kleinste ganze Verzoegerung m (0..mx), bei der Tick T-m vorher/nachher nicht sichtbar
 * (>= vis) abweicht; keine -> mx + 1. */
static int ls_need(const ls_hist_t *pre, const ls_hist_t *post, unsigned T, unsigned obj, double vis, int mx)
{
    int m;
    for (m = 0; m <= mx; m++)
        if (ls_hent_jump(ls_hist_get(pre, T - (unsigned)m, obj), ls_hist_get(post, T - (unsigned)m, obj)) < vis) return m;
    return mx + 1;
}

typedef struct {
    double d;      /* angezeigte Verzoegerung (Ticks) */
    int target;    /* Ziel (Ticks) */
    unsigned now;  /* Ticks seit Reset */
    unsigned ev_t[LS_AEV];
    unsigned char ev_need[LS_AEV];
    unsigned ev_n;
    unsigned calm1, calm2; /* ruhige Ticks fuer das Absenken 1 -> 0 bzw. 2 -> 1 */
} ls_auto_t;

static void ls_auto_reset(ls_auto_t *a) { memset(a, 0, sizeof *a); }

/* Korrektur mit Bedarf need (>= 1 zaehlt; 0 = unsichtbar, wird ignoriert). */
static void ls_auto_event(ls_auto_t *a, int need)
{
    if (need < 1) return;
    a->ev_t[a->ev_n % LS_AEV] = a->now;
    a->ev_need[a->ev_n % LS_AEV] = (unsigned char)(need > 255 ? 255 : need);
    a->ev_n++;
}

static unsigned ls_auto_count(const ls_auto_t *a, int need)
{
    unsigned i, n = 0, m = a->ev_n < LS_AEV ? a->ev_n : LS_AEV;
    for (i = 0; i < m; i++) {
        unsigned k = (a->ev_n - 1u - i) % LS_AEV;
        if (a->now - a->ev_t[k] >= LS_AW) break; /* aelter als das Fenster (Ereignisse sind zeitlich geordnet) */
        if (a->ev_need[k] >= need) n++;
    }
    return n;
}

/* Ein Live-Tick. mode 0 aus (d = 0), 1 auto (0..maxd), 2 fest 1 Tick. Rueckgabe d. */
static double ls_auto_step(ls_auto_t *a, int mode, int maxd)
{
    a->now++;
    if (maxd < 1) maxd = 1;
    if (maxd > 2) maxd = 2;
    if (mode == 0) {
        a->target = 0;
        a->d = 0.0;
        a->calm1 = a->calm2 = 0;
        return 0.0;
    }
    if (mode == 2) {
        a->target = 1;
    } else {
        unsigned n1 = ls_auto_count(a, 1), n2 = ls_auto_count(a, 2);
        if (n2 >= 3u && maxd >= 2) {
            a->target = 2;
            a->calm2 = 0;
        }
        if (n1 >= 2u && a->target < 1) {
            a->target = 1;
            a->calm1 = 0;
        }
        if (a->target > maxd) a->target = maxd;
        if (a->target >= 2) {
            a->calm2 = n2 == 0u ? a->calm2 + 1u : 0u;
            if (a->calm2 >= LS_AHOLD) {
                a->target = 1;
                a->calm2 = 0;
                a->calm1 = 0;
            }
        } else if (a->target == 1) {
            a->calm1 = n1 == 0u ? a->calm1 + 1u : 0u;
            if (a->calm1 >= LS_AHOLD) {
                a->target = 0;
                a->calm1 = 0;
            }
        }
    }
    if (a->d < (double)a->target) {
        a->d += LS_AUP;
        if (a->d > (double)a->target) a->d = (double)a->target;
    } else if (a->d > (double)a->target) {
        a->d -= LS_ADN;
        if (a->d < (double)a->target) a->d = (double)a->target;
    }
    return a->d;
}

/* ---- W390 LOC-OWN-PREDICT: eigene VT-Lage aus dem spekulativen Vorlauf (nur Anzeige) ----
 * Lage = {x, y, z, Gier}. Ein Vorlauf liefert die Lagen der Ticks gt[0..n-1] (gt[k] = Startick + k + 1); die letzte ist
 * die Anzeige des folgenden Live-Ticks (Vorsprung n - 1 Ticks). */

/* Korrektur zwischen dem zuletzt gezeigten Vorlauf (Lage pp fuer Tick pgt) und dem neuen (gleicher Tick): c = alt - neu.
 * Rueckgabe 1 = vergleichbar (Tick im neuen Vorlauf enthalten), 0 = nicht. */
static int ls_own_corr(unsigned pgt, const float pp[4], int n, const unsigned *gt, const float (*p)[4], const int *ok,
                       double c[4])
{
    int k, i;
    for (k = n - 1; k >= 0; k--) {
        if (!ok[k] || gt[k] != pgt) continue;
        for (i = 0; i < 3; i++) c[i] = (double)pp[i] - (double)p[k][i];
        c[3] = ls_wrap((double)pp[3] - (double)p[k][3]);
        return 1;
    }
    return 0;
}

/* Anzeige-Versatz: vorhergesagt - simuliert + Rest (abklingende Korrekturen). Zu gross (Neustart/Teleport, > maxd) -> 0.
 * Rueckgabe 1 = Versatz gilt. */
static int ls_own_offset(const float pred[4], const double sim[4], const double res[4], double maxd, double out[4])
{
    int i;
    double d2 = 0.0;
    for (i = 0; i < 3; i++) {
        out[i] = (double)pred[i] - sim[i] + res[i];
        d2 += out[i] * out[i];
    }
    out[3] = ls_wrap(ls_wrap((double)pred[3] - sim[3]) + res[3]);
    if (!(d2 <= maxd * maxd) || fabs(out[3]) > 1.2) { /* > 1,2 rad in hoechstens ~9 Ticks: kein Vorlauf, ein Sprung */
        for (i = 0; i < 4; i++) out[i] = 0.0;
        return 0;
    }
    return 1;
}

/* W390: Kamera-Versatz, wenn sich der eigene VT um seinen Mittelpunkt c dreht (Gier dG) und um off verschiebt: das Auge
 * e (vor/ueber c) wandert mit, vorhergesagtes Auge = c + off + (e - c) Ry(dG). Ergebnis fuer ls_cam_xform. */
static void ls_cam_off_pivot(const double off[4], const double e[3], const double c[3], double out[4])
{
    double r[16], d[3], q[3];
    int i;
    ls_m4_yaw(r, off[3]);
    for (i = 0; i < 3; i++) d[i] = e[i] - c[i];
    q[0] = d[0] * r[0] + d[1] * r[4] + d[2] * r[8];
    q[1] = d[0] * r[1] + d[1] * r[5] + d[2] * r[9];
    q[2] = d[0] * r[2] + d[1] * r[6] + d[2] * r[10];
    for (i = 0; i < 3; i++) out[i] = off[i] + q[i] - d[i];
    out[3] = off[3];
}

/* W390: Vorsprung des Vorlaufs aendert sich um dlead Ticks (W386 wechselt D): ohne Ausgleich stuende die Anzeige einen
 * Tick (dlead < 0) bzw. spraenge einen Tick vor (dlead > 0). Ausgleich c = -dlead * (letzter Schritt je Tick). */
static void ls_own_leadchg(const float plast[4], const float pprev[4], int dlead, double c[4])
{
    int i;
    for (i = 0; i < 3; i++) c[i] = -(double)dlead * ((double)plast[i] - (double)pprev[i]);
    c[3] = -(double)dlead * ls_wrap((double)plast[3] - (double)pprev[3]);
}
