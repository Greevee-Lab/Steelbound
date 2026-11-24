/* render_interp_match.c - FPS-INTERP Zuordnung: Draws Tick N <-> Tick N-1 (Signatur, Instanzen, Kamera-Fit, robuste Paarung, ip_match).
 * W463 RCORE: aus render_interp.c ausgegliedert (reines Verschieben); gemeinsamer Zustand in
 * render_interp_int.h, Modulkarte docs/RENDER_ARCH.md. */
#include "render_interp_int.h"

/* ---------- Zuordnung Tick N-1 -> N ---------- */

void ip_clip(const float p[4], const float m[16], float c[4])
{
    int j;
    for (j = 0; j < 4; j++) {
        c[j] = p[0] * m[0 * 4 + j] + p[1] * m[1 * 4 + j] + p[2] * m[2 * 4 + j] + p[3] * m[3 * 4 + j];
    }
}

/* Plausibilitaet einer Zuordnung a (Tick N-1) -> o (Tick N): jeder der 4
 * Bezugs-Vertices, der in beiden Ticks vor der Kamera liegt (w > 0), darf
 * hoechstens interp_maxjump (NDC) springen; ein Vorzeichenwechsel von w ist
 * unplausibel; mind. ein Bezug muss vor der Kamera liegen, ausser alle
 * liegen in beiden Ticks dahinter (unsichtbar). 1 = lerpen. */
static int ip_plausible(const ip_op_t *a, const ip_op_t *o, float maxjump)
{
    int q, front = 0, behind = 0;
    for (q = 0; q < 4; q++) {
        float c0[4], c1[4];
        ip_clip(a->pref[q], a->mvp, c0);
        ip_clip(o->pref[q], o->mvp, c1);
        if (c0[3] > 1e-6f && c1[3] > 1e-6f) {
            float dx = c1[0] / c1[3] - c0[0] / c0[3], dy = c1[1] / c1[3] - c0[1] / c0[3];
            if (dx < 0) { dx = -dx; }
            if (dy < 0) { dy = -dy; }
            if (dx > maxjump || dy > maxjump) { return 0; }
            front++;
        } else if (c0[3] < -1e-6f && c1[3] < -1e-6f) {
            behind++;
        } else {
            return 0; /* w-Wechsel oder w ~ 0 */
        }
    }
    return front > 0 || behind == 4;
}

/* FIX(W448 LOC-CHAFF-LOOK, GitHub #83) loc_interp_eyeblind: Matrix-Lerp eines Netzes (gleiche Eingangsdaten, > 8
 * Vertices), dessen 4 Bezugs-Vertices in beiden Ticks hinter der Kamera liegen, nicht zulassen -> exakt aus Tick N
 * (Regel und Begruendung in render_interp_eye.h; Chaff-Huelsen an der Augenebene). Nur LoC. 1 = exakt. */
unsigned st_w448_eye;
static int w448_eye_blind(const ip_op_t *a, const ip_op_t *o)
{
    if (!g_sba_loc || !g_cfg.loc_interp_eyeblind || g_cfg.vr_enable) { return 0; }
    return ip_eye_blind((const float (*)[4])a->pref, a->mvp, a->nv, (const float (*)[4])o->pref, o->mvp, o->nv);
}

/* OPTION(W406 M3-ROAD-JITTER) interp_p2rej: vorab transformierte Welt-Draws (P2 im Renderziel, Strom schon
 * durch w geteilt, affine Bildraum-Matrix) mit GLEICHEN Eingangsdaten scheitern an ip_plausible, sobald EIN
 * Bezugs-Vertex nahe der Augenebene liegt: dessen Pixellage ist riesig (|ndc| >> 1) und springt bei kleinster
 * Kamerabewegung weit. Der ganze Draw (z. B. ein Gelaendestueck unter dem VT) steht dann im Zwischenbild in
 * Tick-N-Lage, die gelerpten Nachbarn um (1-t)*Sprung daneben -> Versatz/Wackeln und Spalt (SEAMRJ, W406).
 * Hier zaehlen nur Bezugs-Vertices, die in beiden Ticks im erweiterten Bild (|ndc| <= 2) liegen; mindestens
 * einer muss es sein, und alle diese springen hoechstens maxjump. 1 = lerpen. */
static int ip_p2rej_ok(const ip_op_t *a, const ip_op_t *o, float maxjump)
{
    int q, nin = 0;
    if (!ip_mvp_affine(o->mvp) || !ip_mvp_affine(a->mvp) || a->rawhash != o->rawhash || a->nv != o->nv) { return 0; }
    for (q = 0; q < 4; q++) {
        float c0[4], c1[4], x0, y0, x1, y1;
        ip_clip(a->pref[q], a->mvp, c0);
        ip_clip(o->pref[q], o->mvp, c1);
        if (c0[3] <= 1e-6f || c1[3] <= 1e-6f) { return 0; }
        x0 = c0[0] / c0[3]; y0 = c0[1] / c0[3]; x1 = c1[0] / c1[3]; y1 = c1[1] / c1[3];
        if (fabsf(x0) > 2.0f || fabsf(y0) > 2.0f || fabsf(x1) > 2.0f || fabsf(y1) > 2.0f) { continue; }
        if (fabsf(x1 - x0) > maxjump || fabsf(y1 - y0) > maxjump) { return 0; }
        nin++;
    }
    return nin > 0;
}

/* FIX(W429 M3-ROAD-FIX) interp_p2hom: Welt-Draw mit GLEICHEN Eingangsdaten (gleicher Eingangs-Hash und Vertexzahl:
 * Vertex i <-> i ist derselbe Punkt), der an ip_plausible scheitert. Das Bodenstueck vor dem VT zeichnet das Spiel
 * zweimal (P2 vorab transformiert und 3D mit Matrix, W406); ein einziger Bezugs-Vertex an der Augenebene bzw.
 * hinter der Kamera (riesige bzw. gefaltete Bildlage) liess bisher einen Pass exakt stehen, waehrend der andere
 * gelerpt wurde -> Schichten senkrecht versetzt, Spalt. Mit dem homogenen Lerp (hlerp-Modus 3) behandelt der VS
 * jeden Vertex wie im 3D-Pass (Clip-Lerp, w-Wechsel -> Tick N); hier zaehlen darum nur Bezugs-Vertices, die in
 * beiden Ticks vor der Kamera (echte w > 0) und im erweiterten Bild (|ndc| <= 2) liegen: mindestens einer, und alle
 * diese springen hoechstens maxjump (Schnitt bzw. falsche Instanz -> weiter exakt). Fuer P2 und 3D dieselbe Regel.
 * P2 nur mit echter w beider Ticks (sonst Bildraum-Lerp, bleibt exakt). 1 = lerpen. */
unsigned st_rr[4]; /* W429 Ablehnungsgruende: Daten neu, ohne echte w, kein Bezug im Bild, Sprung */
static int ip_rej_relax(const ip_list_t *A, const ip_list_t *B, const ip_op_t *a, const ip_op_t *o, float maxjump)
{
    int q, nin = 0;
    const int p2 = ip_mvp_affine(o->mvp);
    if (a->rawhash != o->rawhash || a->nv != o->nv || p2 != ip_mvp_affine(a->mvp)) { st_rr[0]++; return 0; }
    if (p2 && (!a->p2w || !o->p2w || !A->wb_ok || !B->wb_ok || sb_ip_hlerp() < 2)) { st_rr[1]++; return 0; }
    for (q = 0; q < 4; q++) {
        float c0[4], c1[4], w0, w1, x0, y0, x1, y1;
        ip_clip(a->pref[q], a->mvp, c0);
        ip_clip(o->pref[q], o->mvp, c1);
        w0 = p2 ? a->prefw[q] * c0[3] : c0[3];
        w1 = p2 ? o->prefw[q] * c1[3] : c1[3];
        if (w0 <= 1e-6f || w1 <= 1e-6f || c0[3] == 0.0f || c1[3] == 0.0f) { continue; } /* hinter der Kamera: W224 je Vertex */
        x0 = c0[0] / c0[3]; y0 = c0[1] / c0[3]; x1 = c1[0] / c1[3]; y1 = c1[1] / c1[3];
        if (fabsf(x0) > 2.0f || fabsf(y0) > 2.0f || fabsf(x1) > 2.0f || fabsf(y1) > 2.0f) { continue; }
        if (fabsf(x1 - x0) > maxjump || fabsf(y1 - y0) > maxjump) { st_rr[3]++; return 0; }
        nin++;
    }
    if (!nin) { st_rr[2]++; }
    return nin > 0;
}
static int ip_hhead[1u << IP_HBITS], ip_ghead[1u << IP_HBITS];
static int *ip_hnext, *ip_gnext;
static unsigned ip_hcap;
static ip_ck_t *ip_ck;
static unsigned ip_ckcap;
static int ip_hash_build(ip_list_t *A)
{
    unsigned j;
    if (ip_hcap < A->nops) {
        int *nn = (int *)realloc(ip_hnext, A->nops * sizeof *nn);
        if (!nn) { return 0; }
        ip_hnext = nn;
        nn = (int *)realloc(ip_gnext, A->nops * sizeof *nn);
        if (!nn) { return 0; }
        ip_gnext = nn;
        ip_hcap = A->nops;
    }
    if (g_cfg.ip_match_compact && ip_ckcap < A->nops) {
        ip_ck_t *nc = (ip_ck_t *)realloc(ip_ck, A->nops * sizeof *nc);
        if (!nc) { return 0; }
        ip_ck = nc;
        ip_ckcap = A->nops;
    }
    memset(ip_hhead, 0xFF, sizeof ip_hhead);
    memset(ip_ghead, 0xFF, sizeof ip_ghead);
    for (j = A->nops; j-- > 0;) {
        const ip_op_t *a = &A->ops[j];
        unsigned h;
        if (a->kind != IP_DRAW) { continue; }
        if (g_cfg.ip_match_compact) { ip_ck[j].sig = a->sig; ip_ck[j].rawhash = a->rawhash; memcpy(ip_ck[j].rn, a->rn, sizeof a->rn); }
        h = IP_HB(a->sig);
        ip_hnext[j] = ip_hhead[h];
        ip_hhead[h] = (int)j;
        h = IP_HB(a->gkey);
        ip_gnext[j] = ip_ghead[h];
        ip_ghead[h] = (int)j;
    }
    return 1;
}

/* Freier Partner gleicher Signatur, raw: 1 = gleicher Eingangs-Hash, 0 =
 * anderer, -1 = egal. Bevorzugt der naechste ab jp (Reihenfolge), sonst der
 * naechstliegende davor. -1 = keiner. */
static int ip_pick(ip_list_t *A, const ip_op_t *o, unsigned jp, int raw)
{
    unsigned h = IP_HB(o->sig);
    int j, before = -1;
    for (j = ip_hhead[h]; j >= 0; j = ip_hnext[j]) {
        const ip_op_t *a = &A->ops[j];
        if (a->taken || a->sig != o->sig) { continue; }
        if (raw == 1 && a->rawhash != o->rawhash) { continue; }
        if (raw == 0 && a->rawhash == o->rawhash) { continue; }
        if ((unsigned)j >= jp) {
            if (before < 0 || (unsigned)j - jp <= jp - (unsigned)before) { return j; }
            return before;
        }
        before = j;
    }
    return before;
}
static float ip_cost(const ip_op_t *a, const ip_op_t *o); /* POPIN (W91) */
static int ip_pick_geo(ip_list_t *A, const ip_op_t *o, unsigned jp)
{
    int j, best = -1, besttk = -1;
    unsigned bd = 0xFFFFFFFFu, bdt = 0xFFFFFFFFu;
    for (j = ip_ghead[IP_HB(o->gkey)]; j >= 0; j = ip_gnext[j]) {
        const ip_op_t *a = &A->ops[j];
        unsigned d;
        if (a->gkey != o->gkey || a->rtv != o->rtv || a->nv != o->nv || a->rawhash != o->rawhash) {
            continue;
        }
        d = (unsigned)j >= jp ? (unsigned)j - jp : jp - (unsigned)j;
        if (g_cfg.interp_inst) { /* POPIN (W91): naechste Instanz statt Reihenfolge */
            float c = ip_cost(a, o);
            d = c >= IP_COST_INF ? 0xFFFFFFFEu : (unsigned)(c * 1.0e6f);
        }
        if (!a->taken) { if (d < bd) { bd = d; best = j; } }
        else if (d < bdt) { bdt = d; besttk = j; }
    }
    return best >= 0 ? best : besttk;
}

/* Praktisch unbewegt (W73): alle 4 Bezugs-Vertices springen < 1e-4 NDC
 * (~0,13 px bei 2560). Solche Draws (Cockpit-Teile mit bitweise leicht
 * anderer Matrix) exakt aus Tick N zeichnen - sonst weichen Zwischenbilder
 * durch Rundung um 1 Farbstufe vom stehenden Bild ab (W73_d7 A4). */
static int ip_still(const ip_op_t *a, const ip_op_t *o)
{
    int q;
    for (q = 0; q < 4; q++) {
        float c0[4], c1[4];
        ip_clip(a->pref[q], a->mvp, c0);
        ip_clip(o->pref[q], o->mvp, c1);
        if (c0[3] <= 1e-6f || c1[3] <= 1e-6f) { return 0; }
        if (fabsf(c1[0] / c1[3] - c0[0] / c0[3]) > 1e-4f || fabsf(c1[1] / c1[3] - c0[1] / c0[3]) > 1e-4f ||
            fabsf(c1[2] / c1[3] - c0[2] / c0[3]) > 1e-6f) {
            return 0;
        }
    }
    return 1;
}

/* ---------- POPIN (W91): Instanz-Zuordnung ----------
 * Befund W91: ein Modell, das mehrfach im Tick gezeichnet wird (Haeuser,
 * Cockpit-Teile: gleiche VS/Textur/PS/Vertexzahl UND gleiche Objektraum-
 * Vertices, nur andere Matrix), hat mehrere gleichwertige Partner. ip_pick
 * waehlt den naechsten freien in Draw-Reihenfolge - kommt eine Instanz ins
 * Bild (Culling) oder faellt eine heraus, verschiebt sich die Reihenfolge und
 * der Draw wird mit einer ANDEREN Instanz des Vorticks interpoliert: das Haus
 * fliegt in den Zwischenbildern von der Stelle des Nachbarn heran (Aufpoppen
 * fuer 1-3 Bilder je Tick), die richtige Instanz bleibt ohne Partner.
 * interp_inst=1: Partner = kleinster Bildsprung der 4 Bezugs-Vertices, nur
 * wenn auch umgekehrt kein anderer offener Draw des Ticks naeher an diesem
 * Partner liegt (gegenseitig naechster) und die Wahl eindeutig ist; sonst
 * exakt aus Tick N (30 Hz, aber nie falsch). */
static int ip_bhead[1u << IP_HBITS];
static int *ip_bnext;
static unsigned ip_bcap;

/* Bildsprung a -> o: groesster NDC-Sprung der 4 Bezugs-Vertices (wie
 * ip_plausible, aus den beim Aufzeichnen berechneten NDC). */
static float ip_cost_rn(const float arn[4][3], const ip_op_t *o)
{
    int q, front = 0;
    float m = 0.0f;
    for (q = 0; q < 4; q++) {
        float w0 = arn[q][2], w1 = o->rn[q][2];
        if (w0 > 1e-6f && w1 > 1e-6f) {
            float dx = fabsf(o->rn[q][0] - arn[q][0]), dy = fabsf(o->rn[q][1] - arn[q][1]);
            if (dx > m) { m = dx; }
            if (dy > m) { m = dy; }
            front++;
        } else if (!(w0 < -1e-6f && w1 < -1e-6f)) {
            return IP_COST_INF; /* w-Wechsel */
        }
    }
    return front ? m : 0.0f;
}
static float ip_cost(const ip_op_t *a, const ip_op_t *o) { return ip_cost_rn(a->rn, o); }

static int ip_bhash_build(ip_list_t *B)
{
    unsigned j;
    if (ip_bcap < B->nops) {
        int *nn = (int *)realloc(ip_bnext, B->nops * sizeof *nn);
        if (!nn) { return 0; }
        ip_bnext = nn;
        ip_bcap = B->nops;
    }
    memset(ip_bhead, 0xFF, sizeof ip_bhead);
    for (j = B->nops; j-- > 0;) {
        unsigned h;
        if (B->ops[j].kind != IP_DRAW) { continue; }
        h = IP_HB(B->ops[j].sig);
        ip_bnext[j] = ip_bhead[h];
        ip_bhead[h] = (int)j;
    }
    return 1;
}

/* Kleinster-Sprung-Kandidat in A (frei, gleiche Signatur + Eingangs-Hash).
 * *n = Zahl der Kandidaten, *c1/*c2 = bester/zweitbester Sprung. */
static unsigned st_ck_chk, st_ck_bad; /* DIAG(PERF-M2,W174) ip_match_compact=2: Selbstpruefung */
static int ip_inst_best_x(ip_list_t *A, const ip_op_t *o, int *n, float *c1, float *c2, int *second, int compact);
static int ip_inst_best(ip_list_t *A, const ip_op_t *o, int *n, float *c1, float *c2, int *second)
{
    int b = ip_inst_best_x(A, o, n, c1, c2, second, g_cfg.ip_match_compact > 0);
    if (g_cfg.ip_match_compact == 2) {
        int n2, s2, b2;
        float d1, d2;
        b2 = ip_inst_best_x(A, o, &n2, &d1, &d2, &s2, 0);
        st_ck_chk++;
        if (b2 != b || n2 != *n || s2 != *second || memcmp(&d1, c1, sizeof d1) != 0 || memcmp(&d2, c2, sizeof d2) != 0) {
            if (st_ck_bad++ < 8u) { HLOG_W("INTERP match_compact: Abweichung best %d/%d n %d/%d", b, b2, *n, n2); }
        }
        if ((st_ck_chk % 200000u) == 0u) { HLOG_I("INTERP match_compact Pruefung: %u Vergleiche, abweichend=%u", st_ck_chk, st_ck_bad); }
    }
    return b;
}
static int ip_inst_best_x(ip_list_t *A, const ip_op_t *o, int *n, float *c1, float *c2, int *second, int compact)
{
    int j, best = -1;
    *n = 0;
    *c1 = *c2 = IP_COST_INF;
    *second = -1;
    if (compact && ip_ck && ip_ckcap >= A->nops) { /* PERF(PERF-M2,W174): gleiche Pruefungen, dicht gepackt */
        for (j = ip_hhead[IP_HB(o->sig)]; j >= 0; j = ip_hnext[j]) {
            const ip_ck_t *k = &ip_ck[j];
            float c;
            if (k->sig != o->sig || k->rawhash != o->rawhash || A->ops[j].taken) { continue; }
            (*n)++;
            c = ip_cost_rn(k->rn, o);
            if (c < *c1) { *c2 = *c1; *second = best; *c1 = c; best = j; }
            else if (c < *c2) { *c2 = c; *second = j; }
        }
        return best;
    }
    for (j = ip_hhead[IP_HB(o->sig)]; j >= 0; j = ip_hnext[j]) {
        const ip_op_t *a = &A->ops[j];
        float c;
        if (a->taken || a->sig != o->sig || a->rawhash != o->rawhash) { continue; }
        (*n)++;
        c = ip_cost(a, o);
        if (c < *c1) { *c2 = *c1; *second = best; *c1 = c; best = j; }
        else if (c < *c2) { *c2 = c; *second = j; }
    }
    return best;
}

/* Gibt es in B einen anderen offenen Draw gleicher Art, der naeher an a liegt? */
static int ip_inst_closer(ip_list_t *B, unsigned oi, const ip_op_t *a, float c)
{
    int k;
    const ip_op_t *o = &B->ops[oi];
    for (k = ip_bhead[IP_HB(o->sig)]; k >= 0; k = ip_bnext[k]) {
        const ip_op_t *o2 = &B->ops[k];
        if ((unsigned)k == oi || o2->sig != o->sig || o2->rawhash != o->rawhash || o2->match >= 0) { continue; }
        if (o2->kind != IP_DRAW || o2->why != IP_W_NONE) { continue; } /* schon entschieden */
        if (ip_cost(a, o2) < c) { return 1; }
    }
    return 0;
}

/* FIX(W429 M3-ROAD-FIX) interp_p2hom: zwei Vortick-Kandidaten, die Durchgaenge DERSELBEN Lage sind (gleiche
 * Eingangsdaten, Vertexzahl, Matrix, Ziel und bitgleiche Bezugs-Vertices samt echter w), liefern im Zwischenbild
 * dieselbe Lage (aus A kommt nur die Position, Farbe/UV aus Tick N) - die Wahl ist dann gleichgueltig und nicht
 * mehrdeutig. Bisher wurden mehrfach gezeichnete Bodenstuecke (P2, ein Bezugs-Vertex an der Augenebene -> grosser
 * Sprung c1 = c2 > 0,02) "nicht eindeutig" exakt, und per interp_pass alle Durchgaenge der Instanz mit ihnen
 * (M3: ~8000 von ~8800 Spalt-Ecken exakt/gelerpt). Nur SB. */
static int ip_same_pass(const ip_op_t *x, const ip_op_t *y)
{
    if (!g_cfg.interp_p2hom || g_sba_loc) { return 0; }
    return x->nv == y->nv && x->rawhash == y->rawhash && x->rtv == y->rtv && x->p2w == y->p2w &&
           memcmp(x->mvp, y->mvp, sizeof x->mvp) == 0 && memcmp(x->pref, y->pref, sizeof x->pref) == 0 &&
           (!x->p2w || memcmp(x->prefw, y->prefw, sizeof x->prefw) == 0);
}

/* interp_inst=1: Partner fuer o (Index oi). -1 = keiner (neu im Bild bzw. nicht
 * eindeutig -> exakt). *amb = 1, wenn es Kandidaten gab, aber keiner eindeutig. */
static int ip_pick_inst(ip_list_t *A, ip_list_t *B, unsigned oi, int *amb)
{
    const ip_op_t *o = &B->ops[oi];
    int n, best, second;
    float c1, c2;
    *amb = 0;
    best = ip_inst_best(A, o, &n, &c1, &c2, &second);
    if (best < 0) { return -1; }
    if (n > 1) { st_imulti++; }
    if (c1 >= IP_COST_INF) { *amb = 1; return -1; }
    /* eindeutig: Zweitbester deutlich weiter weg (oder beide praktisch gleich nah) */
    if (second >= 0 && c2 < 2.0f * c1 && c2 > 0.02f && !ip_same_pass(&A->ops[best], &A->ops[second])) { *amb = 1; return -1; }
    if (ip_inst_closer(B, oi, &A->ops[best], c1)) { *amb = 1; return -1; }
    return best;
}

/* DIAG(POPIN,W91): Pixel-Huellrechteck der 4 Bezugs-Vertices im Ziel-Viewport
 * (grob, nur Lagevergleich). 0 = nicht vor der Kamera. */
static int ip_refbox(const ip_op_t *o, int bb[4])
{
    int q, n = 0;
    bb[0] = bb[1] = 1 << 30;
    bb[2] = bb[3] = -(1 << 30);
    for (q = 0; q < 4; q++) {
        float c[4];
        int x, y;
        ip_clip(o->pref[q], o->mvp, c);
        if (c[3] <= 1e-6f) { continue; }
        x = (int)(o->vp.TopLeftX + (c[0] / c[3] + 1.0f) * 0.5f * o->vp.Width);
        y = (int)(o->vp.TopLeftY + (1.0f - c[1] / c[3]) * 0.5f * o->vp.Height);
        if (x < bb[0]) { bb[0] = x; }
        if (y < bb[1]) { bb[1] = y; }
        if (x > bb[2]) { bb[2] = x; }
        if (y > bb[3]) { bb[3] = y; }
        n++;
    }
    return n;
}

static void ip_iwrong_log(const ip_list_t *A, const ip_list_t *B, unsigned i, int found, int best)
{
    static unsigned nlog;
    int b0[4], b1[4], b2[4];
    const ip_op_t *o = &B->ops[i];
    if (!sb_in_mission() || nlog >= 4000u) { return; } /* nur Spielermission (W232 statt Ladezaehler == 4), gedeckelt */
    if (g_cfg.pf_ring > 0 && !sb_pf_active()) { return; } /* mit pf_ring nur waehrend der Bildfolgen */
    nlog++;
    ip_refbox(o, b0);
    ip_refbox(&A->ops[found], b1);
    ip_refbox(&A->ops[best], b2);
    HLOG_I("INTERP iwrong f=%u i=%u n=%u %s sig=%08X raw=%08X partner=%d sprung=%.3f naechste=%d sprung=%.3f "
           "box=%d,%d,%d,%d partnerbox=%d,%d,%d,%d naechstebox=%d,%d,%d,%d",
           B->frame, i, o->nv, o->rtv == g_rtv ? "BB" : "RT", o->sig, o->rawhash, found,
           (double)ip_cost(&A->ops[found], o), best, (double)ip_cost(&A->ops[best], o),
           b0[0], b0[1], b0[2], b0[3], b1[0], b1[1], b1[2], b1[3], b2[0], b2[1], b2[2], b2[3]);
}

/* POPIN (W91): 16-Bit-Schluessel Instanz-Durchgang = gkey + Matrix (FNV). */
/* FIX(W251) interp_zcons: Kamerabewegung D = M_A^-1 * M_B in double (W kuerzt sich heraus). */
int zc_dmat(double D[16], const float MA[16], const float MB[16])
{
    double a[4][8];
    int i, j, k;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) { a[i][j] = MA[i * 4 + j]; a[i][j + 4] = (i == j) ? 1.0 : 0.0; }
    }
    for (i = 0; i < 4; i++) {
        int pv = i;
        double d;
        for (k = i + 1; k < 4; k++) { if (fabs(a[k][i]) > fabs(a[pv][i])) { pv = k; } }
        if (fabs(a[pv][i]) < 1e-12) { return 0; }
        if (pv != i) { for (j = 0; j < 8; j++) { double t = a[i][j]; a[i][j] = a[pv][j]; a[pv][j] = t; } }
        d = a[i][i];
        for (j = 0; j < 8; j++) { a[i][j] /= d; }
        for (k = 0; k < 4; k++) {
            if (k != i) { double f = a[k][i]; for (j = 0; j < 8; j++) { a[k][j] -= f * a[i][j]; } }
        }
    }
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            D[i * 4 + j] = a[i][4] * MB[0 * 4 + j] + a[i][5] * MB[1 * 4 + j] + a[i][6] * MB[2 * 4 + j] + a[i][7] * MB[3 * 4 + j];
        }
    }
    return 1;
}

double zc_derr(const double X[16], const double Y[16])
{
    double mx = 0.0, er = 0.0;
    int q;
    for (q = 0; q < 16; q++) {
        double d = fabs(X[q] - Y[q]), m = fabs(Y[q]);
        if (m > mx) { mx = m; }
        if (d > er) { er = d; }
    }
    return mx > 0.0 ? er / mx : 1.0;
}

unsigned st_zc_ticks, st_zc_tgt, st_zc_draws, st_zc_rel, st_zc_nocand, st_zc_cam, st_zc_ln; /* FIX(W251) interp_zcons: Zaehler */
static unsigned ip_mvpkey(const ip_op_t *o)
{
    const unsigned char *b = (const unsigned char *)o->mvp;
    unsigned h = 2166136261u ^ o->gkey, k;
    for (k = 0; k < sizeof o->mvp; k++) { h = (h ^ b[k]) * 16777619u; }
    return (h ^ (h >> 16)) & 0xFFFFu;
}
static ID3D11RenderTargetView *rz_prt[RZ_PT];
static ID3D11DepthStencilView *rz_pds[RZ_PT];
static int rz_prr[RZ_PT][8], rz_pn, rz_povf;
static unsigned rz_prc[RZ_PT][8]; /* W294 Bit 1: Anzahl Draws je Matrix-Paar der Eintraege */
/* FIX(W294 LINE-STREAK) interp_streak Bit 1: traegt das Paar a (N-1) -> r (N) eine Kamerabewegung?
 * P2-Draws im RT haben eine affine Bildraum-Matrix (die Kamera steckt schon in den Vertices), gleiche
 * Matrizen liefern D = I. In P2-lastigen Zielen (M2: Meer, Boot) waren die 8 groessten gelerpten Draws
 * alle so -> keine 3D-Kachel bestand den Welt-Test, obwohl sie in der Welt ruht. */
int ip_mvp_affine(const float m[16]) { return m[3] == 0.0f && m[7] == 0.0f && m[11] == 0.0f; }
static int zc_cam_src(const ip_op_t *a, const ip_op_t *r)
{
    if (r->rzm) { return 0; } /* W317: mit der Kamera bewegt bzw. Folge-Durchgang traegt keine eigene Kamera */
    if (!(g_cfg.interp_streak & 1)) { return 1; }
    return !ip_mvp_affine(r->mvp) && memcmp(a->mvp, r->mvp, sizeof r->mvp) != 0;
}
/* W294 Bit 2: Kamera je Renderziel (Tiefenpuffer egal), damit auch Welt-Draws ohne Tiefenpuffer
 * (Meeres-Kacheln) den Welt-Test bekommen. */
static ID3D11DepthStencilView *rz_dkey(const ip_op_t *o) { return (g_cfg.interp_streak & 2) ? NULL : o->dsv; }
unsigned st_sk_rl, st_sk_rlcam, st_sk_rlofs, st_sk_nod, st_sk_ln; /* W294: Zaehler */
unsigned st_w410_rzb, st_w410_big, st_w410_cam, st_w410_mov, st_w410_rzm; /* W410 loc_interp_rawrzb: Matrix-Lerp statt Vertex-Lerp (davon >= 1000 Vertices), Matrix passt nicht zur Kamera, Bezugs-Vertices bewegt */
static int rz_hq[RZ_HB];
static unsigned rz_hn[RZ_HB];
static unsigned rz_pkey(const ip_list_t *A, const ip_op_t *r)
{
    const unsigned *u = (const unsigned *)r->mvp, *v = (const unsigned *)A->ops[r->match].mvp;
    unsigned h = 2166136261u;
    int i;
    for (i = 0; i < 16; i++) { h = (h ^ u[i]) * 16777619u; h = (h ^ v[i]) * 16777619u; }
    return h;
}
static int rz_psame(const ip_list_t *A, const ip_list_t *B, int q1, const ip_op_t *r)
{
    const ip_op_t *o1 = &B->ops[q1];
    return memcmp(o1->mvp, r->mvp, sizeof r->mvp) == 0 &&
           memcmp(A->ops[o1->match].mvp, A->ops[r->match].mvp, sizeof r->mvp) == 0;
}
static unsigned rz_pcount(const ip_list_t *A, const ip_list_t *B, const ip_op_t *r, int add, int q)
{
    unsigned h = rz_pkey(A, r) & (RZ_HB - 1u), k;
    for (k = 0; k < RZ_HB; k++, h = (h + 1u) & (RZ_HB - 1u)) {
        if (rz_hq[h] < 0) {
            if (!add) { return 0; }
            rz_hq[h] = q; rz_hn[h] = 1;
            return 1;
        }
        if (rz_psame(A, B, rz_hq[h], r)) { if (add) { rz_hn[h]++; } return rz_hn[h]; }
    }
    return 0;
}

void rz_cam_pre(const ip_list_t *A, const ip_list_t *B)
{
    unsigned q;
    int x;
    int maj = (g_cfg.interp_streak & 1) != 0; /* W294 */
    rz_pn = 0; rz_povf = 0;
    if (maj) {
        memset(rz_hq, 0xFF, sizeof rz_hq);
        for (q = 0; q < B->nops; q++) {
            const ip_op_t *r = &B->ops[q];
            if (r->kind != IP_DRAW || r->why != IP_W_LERP || !r->flag || r->match < 0 || (unsigned)r->match >= A->nops) { continue; }
            if (!zc_cam_src(&A->ops[r->match], r)) { continue; }
            rz_pcount(A, B, r, 1, (int)q);
        }
    }
    for (q = 0; q < B->nops; q++) {
        const ip_op_t *r = &B->ops[q];
        int w, t = -1, *rr;
        if (r->kind != IP_DRAW || r->why != IP_W_LERP || !r->flag || r->match < 0 || (unsigned)r->match >= A->nops) { continue; }
        if (!zc_cam_src(&A->ops[r->match], r)) { continue; } /* W294 Bit 1 */
        for (x = 0; x < rz_pn; x++) { if (rz_prt[x] == r->rtv && rz_pds[x] == rz_dkey(r)) { t = x; break; } }
        if (t < 0) {
            if (rz_pn >= RZ_PT) { rz_povf = 1; continue; }
            t = rz_pn++;
            rz_prt[t] = r->rtv; rz_pds[t] = rz_dkey(r);
            for (x = 0; x < 8; x++) { rz_prr[t][x] = -1; }
        }
        rr = rz_prr[t];
        if (maj) { /* W294 Bit 1: haeufigste Matrix-Paare, je Paar ein Eintrag */
            unsigned c = rz_pcount(A, B, r, 0, 0), *rc = rz_prc[t];
            int dup = 0;
            for (w = 0; w < 8 && rr[w] >= 0; w++) { if (rc[w] == c && rz_psame(A, B, rr[w], r)) { dup = 1; break; } }
            if (dup) { continue; }
            for (w = 0; w < 8; w++) {
                if (rr[w] < 0 || c > rc[w] || (c == rc[w] && r->nv > B->ops[rr[w]].nv)) {
                    int z;
                    for (z = 7; z > w; z--) { rr[z] = rr[z - 1]; rc[z] = rc[z - 1]; }
                    rr[w] = (int)q; rc[w] = c;
                    break;
                }
            }
            continue;
        }
        for (w = 0; w < 8; w++) {
            if (rr[w] < 0 || r->nv > B->ops[rr[w]].nv) {
                int z;
                for (z = 7; z > w; z--) { rr[z] = rr[z - 1]; }
                rr[w] = (int)q;
                break;
            }
        }
    }
}
/* Kamerabewegungen D = M_A^-1 * M_B des Ziels von o (nach rz_cam_pre). Rueckgabe: Anzahl. */
int rz_cam(const ip_list_t *A, const ip_list_t *B, const ip_op_t *o, double D[8][16])
{
    int x, t = -1, n = 0;
    for (x = 0; x < rz_pn; x++) { if (rz_prt[x] == o->rtv && rz_pds[x] == rz_dkey(o)) { t = x; break; } }
    if (t < 0) { return 0; }
    for (x = 0; x < 8 && rz_prr[t][x] >= 0; x++) {
        const ip_op_t *r = &B->ops[rz_prr[t][x]];
        if (zc_dmat(D[n], A->ops[r->match].mvp, r->mvp)) { n++; }
    }
    return n;
}

/* FIX(W284) interp_rawzc: Bezugs-Vertices von o = die von a + fester Vektor d (w = 1)?
 * Toleranz: Rundung von p + d in float (relativ 1e-5 zum Betrag). 1 = ja, d gesetzt. */
static int rz_offset(const ip_op_t *a, const ip_op_t *o, float d[3])
{
    int q, c;
    for (q = 0; q < 4; q++) {
        if (a->pref[q][3] != 1.0f || o->pref[q][3] != 1.0f) { return 0; }
    }
    for (c = 0; c < 3; c++) { d[c] = o->pref[0][c] - a->pref[0][c]; }
    for (q = 1; q < 4; q++) {
        for (c = 0; c < 3; c++) {
            float e = o->pref[q][c] - a->pref[q][c] - d[c], m = fabsf(o->pref[q][c]) + fabsf(a->pref[q][c]) + fabsf(d[c]);
            if (fabsf(e) > 1e-5f * (1.0f + m)) { return 0; }
        }
    }
    return 1;
}
/* FIX(W294) interp_streak Bit 4: Versatz-Test fuer neu gelegte Gitter (Boden-/Wasserkacheln). Das Spiel legt
 * Gelaende- und Meeresgitter in festen Schritten (5, 10, ...) neu um die Kamera; der Partner gleicher Signatur
 * ist dann ein anderes Gitterstueck: waagerecht (x, z) um d verschoben, die Hoehe y aber verschieden
 * (M2: Wellen bis ~0,01, M4: Gelaendereihe bis ~0,4). rz_offset (Toleranz ~1e-3 in allen Achsen) lehnte
 * diese ab -> Vertex-Lerp ueber das Gitter = Kachel wandert/verformt sich, Naht zum Nachbarn. Hier:
 * d = Versatz des ersten Bezugs-Vertex, x/z aller vier Bezugs-Vertices innerhalb 0,01 + 2 % von |d_xz|,
 * y frei. rzb nutzt nur die Tick-N-Vertices und die Matrix N-1; der genaue Versatz geht nicht ein. */
static int rz_offset_grid(const ip_op_t *a, const ip_op_t *o, float d[3])
{
    int q;
    float tol;
    for (q = 0; q < 4; q++) {
        if (a->pref[q][3] != 1.0f || o->pref[q][3] != 1.0f) { return 0; }
    }
    d[0] = o->pref[0][0] - a->pref[0][0]; d[1] = o->pref[0][1] - a->pref[0][1]; d[2] = o->pref[0][2] - a->pref[0][2];
    if (fabsf(d[0]) < 0.5f && fabsf(d[2]) < 0.5f) { return 0; } /* kein Gitterschritt (Wellen/Animation: Vertex-Lerp richtig) */
    tol = 0.01f + 0.02f * (fabsf(d[0]) > fabsf(d[2]) ? fabsf(d[0]) : fabsf(d[2]));
    for (q = 1; q < 4; q++) {
        if (fabsf(o->pref[q][0] - a->pref[q][0] - d[0]) > tol || fabsf(o->pref[q][2] - a->pref[q][2] - d[2]) > tol) { return 0; }
    }
    return 1;
}
/* W349 LOC-FRAMES-2: Kamera-Pruefung der Bezugs-Vertices (render_interp_camfit.h, Selbsttest CAMFIT). */
static double li_camfit_pm(const float pa[4][4], const float *ma, const ip_op_t *o, double D[8][16], int nD)
{
    return ipcf_camfit(pa, ma, (const float (*)[4])o->pref, o->mvp, (const double (*)[16])D, nD);
}
/* Vertex-Lerp-Pruefung: Bezugs-Vertices des Partners c mit dessen Matrix (Vortick) gegen o. Matrix-Lerp-Pruefung
 * (Tick-N-Vertices mit M_A): li_camfit_pm(o->pref, a->mvp, o, ...). */
static double li_camfit(const ip_op_t *c, const ip_op_t *o, double D[8][16], int nD)
{
    return li_camfit_pm((const float (*)[4])c->pref, c->mvp, o, D, nD);
}
/* FIX(W284) interp_rawzc=2: Hash-Kette der Vortick-Draws nach sig0 (Signatur ohne Vertexzahl), bei Bedarf je Tick */
static int rz_s0head[1u << IP_HBITS], *rz_s0next;
static unsigned rz_s0cap;
static int rz_s0_build(const ip_list_t *A)
{
    unsigned j;
    if (rz_s0cap < A->nops) {
        int *nn = (int *)realloc(rz_s0next, A->nops * sizeof *nn);
        if (!nn) { return 0; }
        rz_s0next = nn;
        rz_s0cap = A->nops;
    }
    memset(rz_s0head, 0xFF, sizeof rz_s0head);
    for (j = A->nops; j-- > 0;) {
        const ip_op_t *a = &A->ops[j];
        unsigned h;
        if (a->kind != IP_DRAW) { continue; }
        h = IP_HB(a->sig0);
        rz_s0next[j] = rz_s0head[h];
        rz_s0head[h] = (int)j;
    }
    return 1;
}
/* FIX(W347 GHOST) interp_bbcam: Partner eines exakt gebliebenen rawchg-Draws (Billboard, z. B. Baum) per Kamera.
 * Baeume sind Quads (n=6/12), die das Spiel je Tick neu zur Kamera ausgerichtet in den VB schreibt (Matrix
 * gleich, Eingangsdaten neu, W347_ld5). Ein Billboard ruht in der Welt um seinen Mittelpunkt; fuer den richtigen
 * Vortick-Partner c gilt dann Schwerpunkt(c)*M_c*D ~ Schwerpunkt(o)*M_o (D = Kamerabewegung des Ziels,
 * Zeilenvektoren), die Ecken weichen nur um die Eigendrehung ab. Gewaehlt wird der kleinste Schwerpunkt-
 * Fehler (Gleichstand: Reihenfolge, deterministisch). -1 = keiner. */
unsigned st_bb_ok, st_bb_no, st_bb_far, st_bb_build; /* W347: Zaehler (INTERP bbcam) */
double st_bb_ms; /* W347: Zeit in ip_bb_pick (inkl. Rasteraufbau) je Statistikfenster */
static bbg_t bbg[BBG_N]; /* je Ziel ein Raster (Ziele wechseln innerhalb des Ticks; ein einzelnes Raster wurde je Draw neu gebaut) */
static unsigned bbg_clock;
static unsigned bbg_key(unsigned sig, int cx, int cy)
{
    unsigned h = (sig * 2654435761u) ^ ((unsigned)cx * 40503u) ^ ((unsigned)cy * 2246822519u);
    return (h ^ (h >> 15) ^ (h >> 27)) & ((1u << BBG_BITS) - 1u);
}
static bbg_t *bbg_build(const ip_list_t *A, const ip_op_t *o, const double D0[16])
{
    unsigned j;
    int k, v = 0;
    bbg_t *g;
    for (k = 0; k < BBG_N; k++) {
        g = &bbg[k];
        if (g->ser == A->serial && g->rtv == o->rtv && memcmp(g->D0, D0, sizeof g->D0) == 0) { g->used = ++bbg_clock; return g; }
    }
    for (k = 1; k < BBG_N; k++) { if (bbg[k].used < bbg[v].used) { v = k; } } /* aeltestes ersetzen */
    g = &bbg[v];
    if (g->cap < A->nops) {
        int *nn = (int *)realloc(g->next, A->nops * sizeof *nn);
        float *fx, *fy;
        if (!nn) { g->ser = 0; return NULL; }
        g->next = nn;
        fx = (float *)realloc(g->px, A->nops * sizeof *fx);
        if (!fx) { g->ser = 0; g->cap = 0; return NULL; }
        g->px = fx;
        fy = (float *)realloc(g->py, A->nops * sizeof *fy);
        if (!fy) { g->ser = 0; g->cap = 0; return NULL; }
        g->py = fy;
        {
            double (*pv)[5][4] = (double (*)[5][4])realloc(g->pv, A->nops * sizeof *pv);
            if (!pv) { g->ser = 0; g->cap = 0; return NULL; }
            g->pv = pv;
        }
        g->cap = A->nops;
    }
    memset(g->head, 0xFF, sizeof g->head);
    for (j = A->nops; j-- > 0;) {
        const ip_op_t *c = &A->ops[j];
        double nx, ny, p[4];
        unsigned h;
        g->next[j] = -1;
        if (c->kind != IP_DRAW || !c->cls || c->nv > 64u || c->rtv != o->rtv) { continue; }
        ipbb_prep((const float (*)[4])c->pref, c->mvp, g->pv[j], g->pv[j][4]);
        ipbb_row(g->pv[j][4], D0, p);
        if (p[3] <= 1e-6) { continue; }
        nx = p[0] / p[3]; ny = p[1] / p[3];
        if (fabs(nx) > 1e6 || fabs(ny) > 1e6) { continue; }
        g->px[j] = (float)nx; g->py[j] = (float)ny;
        h = bbg_key(c->sig, (int)floor(nx / BBG_CELL), (int)floor(ny / BBG_CELL));
        g->next[j] = g->head[h];
        g->head[h] = (int)j;
    }
    g->ser = A->serial; g->rtv = o->rtv; g->used = ++bbg_clock;
    st_bb_build++;
    memcpy(g->D0, D0, sizeof g->D0);
    return g;
}
static int ip_bb_pick_x(const ip_list_t *A, const ip_op_t *o, double D[8][16], int nd);
static int ip_bb_pick(const ip_list_t *A, const ip_op_t *o, double D[8][16], int nd)
{
    LONGLONG q0 = ip_now();
    int r = ip_bb_pick_x(A, o, D, nd);
    st_bb_ms += ip_ms(q0, ip_now());
    return r;
}
static int ip_bb_pick_x(const ip_list_t *A, const ip_op_t *o, double D[8][16], int nd)
{
    double best = 1e30, nx, ny, vb[4][4], cb[4];
    int j, x, bj = -1, cx, cy, gx, gy, nfull = 0;
    bbg_t *g;
    if (nd <= 0 || o->nv > 64u) { st_bb_no++; return -1; } /* nur kleine Draws (Billboards, Sprites) */
    ipbb_prep((const float (*)[4])o->pref, o->mvp, vb, cb);
    if (cb[3] <= 1e-6) { st_bb_no++; return -1; }
    nx = cb[0] / cb[3]; ny = cb[1] / cb[3];
    if (fabs(nx) > 1e6 || fabs(ny) > 1e6) { st_bb_no++; return -1; }
    g = bbg_build(A, o, D[0]);
    if (!g) { st_bb_no++; return -1; }
    cx = (int)floor((nx - 2.0 * IPBB_EC) / BBG_CELL); cy = (int)floor((ny - 2.0 * IPBB_EC) / BBG_CELL); /* nur Zellen im Vorpruef-Fenster */
    for (gy = cy; gy <= (int)floor((ny + 2.0 * IPBB_EC) / BBG_CELL) && nfull <= 24; gy++) {
        for (gx = cx; gx <= (int)floor((nx + 2.0 * IPBB_EC) / BBG_CELL) && nfull <= 24; gx++) {
            for (j = g->head[bbg_key(o->sig, gx, gy)]; j >= 0; j = g->next[j]) {
                const ip_op_t *c = &A->ops[j];
                double ec = 1e30, ev = 1e30;
                if (c->taken || c->sig != o->sig || c->nv != o->nv || c->dsv != o->dsv || c->topo != o->topo ||
                    c->ib1 != o->ib1) { continue; } /* ib1: Vertex-Lerp nur gleich abgelegt (W264) */
                /* Vorpruefung mit D[0] (Spielraum fuer die anderen Kamera-Kandidaten); dichte Partikelwolken: Deckel */
                if (fabs(g->px[j] - nx) > 2.0 * IPBB_EC || fabs(g->py[j] - ny) > 2.0 * IPBB_EC) { continue; }
                if (++nfull > 24) { break; }
                for (x = 0; x < nd; x++) {
                    double e2, e3;
                    ipbb_err2((const double (*)[4])g->pv[j], g->pv[j][4], (const double (*)[4])vb, cb, D[x], &e2, &e3);
                    if (e2 < ec) { ec = e2; ev = e3; }
                }
                if (!ipbb_ok(ec, ev)) { continue; } /* Schwerpunkt ruht nicht bzw. Ecken passen nicht -> anderes Objekt */
                if (ec < best || (ec == best && j < bj)) { best = ec; bj = j; } /* Gleichstand: Reihenfolge (deterministisch) */
            }
        }
    }
    if (bj < 0) { st_bb_far++; }
    return bj;
}
unsigned st_rz_nm, st_rz_nmno, st_rz_nmcam, st_rz_n;
double st_rz_ms, st_rz_max; /* W284: Zeit des rawzc-Blocks je Tick */
unsigned st_rz_relw, st_rz_rel, st_rz_noofs, st_rz_cam, st_rz_nocam, st_rz_other, st_rz_ln; /* FIX(W284) interp_rawzc: Zaehler */
const char *const kPR_CLS[PR_NC] = { "inst", "gitter", "quad", "vorab", "einzel", "ohne_kamera" };
static const char *const kPR_OUT[PR_NO] = { "ok", "falsch", "verpasst", "neu", "bewegt", "exakt", "statisch" };
unsigned st_pz[PR_NC][PR_NO];
unsigned st_pr_ticks, st_pr_set, st_pr_pair, st_pr_raw, st_pr_cam, st_pr_keep, st_pr_follow,
                st_pr_chg, st_pr_still, st_pr_ln, st_pr_lnd;
double st_pr_ms, st_pr_max, st_pr_ms1, st_pr_ms2, st_pr_ms3; /* W317: Zeit gesamt bzw. Kamera+A, Kandidaten, Zuordnung */
static pr_tg_t pr_tg[PR_T];
static int pr_ntg;
static float (*pr_pc)[4][4], (*pr_ac)[4][4], *pr_c1, *pr_c2, *pr_ac1, *pr_ac2;
static int *pr_b1, *pr_ab1, *pr_hn, *pr_tgo, *pr_old;
static unsigned *pr_hk;
static int (*pr_acl)[3]; /* Rasterzelle (x, y, vor der Kamera) je A-Draw */
static unsigned char *pr_in, *pr_at, *pr_orzb;
static unsigned pr_capb, pr_capa, pr_cpp;
static pr_pair_t *pr_pp;
static int pr_hh[1u << PR_HB];

static int pr_grow(void **p, unsigned n, size_t sz) { void *q = realloc(*p, (size_t)n * sz); if (!q) { return 0; } *p = q; return 1; }
static int pr_tgt(const ip_op_t *o)
{
    int x;
    ID3D11DepthStencilView *dk = rz_dkey(o);
    for (x = 0; x < pr_ntg; x++) { if (pr_tg[x].rtv == o->rtv && pr_tg[x].dk == dk) { return x; } }
    if (pr_ntg >= PR_T) { return -1; }
    memset(&pr_tg[pr_ntg], 0, sizeof pr_tg[0]);
    pr_tg[pr_ntg].rtv = o->rtv; pr_tg[pr_ntg].dk = dk; pr_tg[pr_ntg].ref = -1;
    return pr_ntg++;
}
/* vorab transformiert (Clip-w = 1 an allen Bezugs-Vertices) bzw. affine Bildraum-Matrix (P2) */
static int pr_pre(const ip_op_t *o)
{
    int q;
    if (ip_mvp_affine(o->mvp)) { return 1; }
    for (q = 0; q < 4; q++) { if (!(o->rn[q][2] > 0.999f && o->rn[q][2] < 1.001f)) { return 0; } }
    return 1;
}
static float pr_cost(const float pc[4][4], const float ac[4][4])
{
    int q;
    float m = 0.0f;
    for (q = 0; q < 4; q++) {
        float wa = ac[q][3], wp = pc[q][3], den, d;
        if ((wa > 1e-6f) != (wp > 1e-6f)) { return IP_COST_INF; } /* Augenebene gekreuzt */
        den = fmaxf(fabsf(wa), fabsf(wp));
        if (den < 1e-3f) { den = 1e-3f; }
        d = fmaxf(fabsf(pc[q][0] - ac[q][0]), fmaxf(fabsf(pc[q][1] - ac[q][1]), fabsf(pc[q][3] - ac[q][3]))) / den;
        if (d > m) { m = d; }
    }
    return m;
}
/* Raster der Lage von Bezugs-Vertex 0 (Zellen 0,25 NDC, ausserhalb +-8 geklemmt) */
static void pr_cell(const float c[4], int *cx, int *cy, int *s)
{
    float den = fmaxf(fabsf(c[3]), 1e-3f), x = c[0] / den, y = c[1] / den;
    x = fminf(fmaxf(x, -8.0f), 8.0f); y = fminf(fmaxf(y, -8.0f), 8.0f);
    *cx = (int)floorf(x * 4.0f); *cy = (int)floorf(y * 4.0f); *s = c[3] > 1e-6f;
}
static unsigned pr_hkey(unsigned sig, int cx, int cy, int s)
{
    unsigned h = sig * 2654435761u;
    h ^= (unsigned)(cx + 64) * 40503u; h = (h ^ (h >> 13)) * 16777619u;
    h ^= (unsigned)(cy + 64) * 9973u + (unsigned)s * 7919u;
    return (h ^ (h >> 15)) & ((1u << PR_HB) - 1u);
}
static int pr_pcmp(const void *x, const void *y)
{
    const pr_pair_t *p = (const pr_pair_t *)x, *q = (const pr_pair_t *)y;
    if (p->c != q->c) { return p->c < q->c ? -1 : 1; }
    if (p->o != q->o) { return p->o < q->o ? -1 : 1; }
    return p->a < q->a ? -1 : (p->a > q->a);
}
static void pr_push(unsigned *npp, float c, int o, int a)
{
    if (*npp >= pr_cpp) {
        unsigned nc = pr_cpp ? 2u * pr_cpp : 4096u;
        if (!pr_grow((void **)&pr_pp, nc, sizeof *pr_pp)) { return; }
        pr_cpp = nc;
    }
    pr_pp[*npp].c = c; pr_pp[*npp].o = o; pr_pp[*npp].a = a; (*npp)++;
}
static int pr_cand_a(const ip_op_t *a) { return a->kind == IP_DRAW && a->cls && !a->nolerp; }
static void pr_mul_di(float out[16], const float m[16], const double Di[16])
{
    int r, c;
    for (r = 0; r < 4; r++) {
        for (c = 0; c < 4; c++) {
            out[r * 4 + c] = (float)(m[r * 4 + 0] * Di[0 * 4 + c] + m[r * 4 + 1] * Di[1 * 4 + c] +
                                     m[r * 4 + 2] * Di[2 * 4 + c] + m[r * 4 + 3] * Di[3 * 4 + c]);
        }
    }
}
/* W410: D^-1 = M_B^-1 * M_A des haeufigsten gelerpten Matrix-Paars im Ziel von o (nach rz_cam_pre). 1 = ok. */
int w410_cam_inv(const ip_list_t *A, const ip_list_t *B, const ip_op_t *o, double Di[16])
{
    int x;
    for (x = 0; x < rz_pn; x++) {
        if (rz_prt[x] == o->rtv && rz_pds[x] == rz_dkey(o) && rz_prr[x][0] >= 0) {
            const ip_op_t *r = &B->ops[rz_prr[x][0]];
            return zc_dmat(Di, r->mvp, A->ops[r->match].mvp);
        }
    }
    return 0;
}
/* FIX(W415 LOC-STATIC-LAG) loc_stat_fix: Kamera der Welt fuer "mit der Kamera bewegen" (rzm) in LoC.
 * Befund W415_R2d (Mission 02 Stadt, Drehen, 120 FPS): die W410-Kamera (haeufigstes gleiche Matrix-Paar unter den
 * GELERPTEN Draws, rz_cam_pre) ist dort ein fast ruhendes Paar (Tickweg der Bildmitte 0,0-0,4 px), denn die Geometrie in
 * Weltkoordinaten (Gelaende-Kacheln, grosses Bodennetz) wird je Tick neu gebaut (rawlerp/nomatch) und zaehlt nicht mit;
 * Gebaeude haben je eigene Weltmatrix. Die gelerpten Gebaeude und die Kamera der Weltkoordinaten-Draws (haeufigste
 * gleiche Matrix der Liste, wie la_cam_of) zeigen dagegen 7-12 px Tickweg. Folge: Gelaende-Kacheln (rzm) und das
 * grosse Bodennetz (nomatch, Vertexzahl wechselt je Tick -> exakt) standen in den Zwischenbildern still, die Gebaeude
 * glitten -> Gebaeude/Masten schwimmen gegen den Boden. Kamera hier: D^-1 = C_B^-1 * C_A der haeufigsten gleichen Matrix
 * im Weltziel beider Listen, bestaetigt durch >= 3 gelerpte bewegte Matrix-Draws mit demselben Tickweg (9 Probepunkte,
 * < 2e-4 NDC). Je Tick-Paar einmal berechnet. */
static struct {
    unsigned serA, serB;
    int ok, ia;
    ID3D11RenderTargetView *rtv;
    double Di[16];
    float camB[16];
} w415_wc;
unsigned st_w415_rzm, st_w415_nm, st_w415_nocam, st_w415_cam, st_w415_rej; /* Zaehler (LOCINTERP stat415) */
static int w415_cam_find(const ip_list_t *L, ID3D11RenderTargetView **wt, int *idx, unsigned *cnt)
{
    enum { NC = 64 };
    unsigned cidx[NC], cn[NC], nc = 0, i, k, best = 0, sp = L->nops;
    *wt = la_world_rtv(L, &sp);
    if (!*wt) { return 0; }
    for (i = 0; i < sp; i++) {
        const ip_op_t *o = &L->ops[i];
        if (o->kind != IP_DRAW || o->rtv != *wt || o->nv < 3u || !o->cls || ip_mvp_affine(o->mvp)) { continue; }
        for (k = 0; k < nc; k++) { if (memcmp(L->ops[cidx[k]].mvp, o->mvp, 64) == 0) { cn[k]++; break; } }
        if (k == nc && nc < NC) { cidx[nc] = i; cn[nc] = 1; nc++; }
    }
    if (!nc) { return 0; }
    for (k = 1; k < nc; k++) { if (cn[k] > cn[best]) { best = k; } }
    *idx = (int)cidx[best];
    *cnt = cn[best];
    return 1;
}
/* groesste Abweichung (NDC) zweier Tick-Bewegungen D an 9 Probepunkten im Bild (Tiefe 0,99) */
static double w415_dev(const double X[16], const double Y[16])
{
    int px, py, j;
    double mx = 0.0;
    for (px = -1; px <= 1; px++) {
        for (py = -1; py <= 1; py++) {
            double c[4] = { 0.6 * px, 0.6 * py, 0.99, 1.0 }, a[4], b[4];
            for (j = 0; j < 4; j++) {
                a[j] = c[0] * X[0 * 4 + j] + c[1] * X[1 * 4 + j] + c[2] * X[2 * 4 + j] + c[3] * X[3 * 4 + j];
                b[j] = c[0] * Y[0 * 4 + j] + c[1] * Y[1 * 4 + j] + c[2] * Y[2 * 4 + j] + c[3] * Y[3 * 4 + j];
            }
            if (a[3] <= 1e-9 || b[3] <= 1e-9) { return 1e9; }
            mx = fmax(mx, fmax(fabs(a[0] / a[3] - b[0] / b[3]), fabs(a[1] / a[3] - b[1] / b[3])));
        }
    }
    return mx;
}
int w415_world_cam(const ip_list_t *A, const ip_list_t *B, const ip_op_t *o, double Di[16], int *ia)
{
    if (!g_sba_loc || g_cfg.vr_enable || !(g_cfg.loc_stat_fix || (g_cfg.loc_stat_diag & 1))) { return 0; } /* Diagnose misst auch ohne Fix */
    if (w415_wc.serA != A->serial || w415_wc.serB != B->serial) {
        ID3D11RenderTargetView *wa = NULL, *wb = NULL;
        int xa = -1, xb = -1;
        unsigned ca = 0, cb = 0, i, nok = 0;
        w415_wc.serA = A->serial; w415_wc.serB = B->serial; w415_wc.ok = 0;
        if (w415_cam_find(A, &wa, &xa, &ca) && w415_cam_find(B, &wb, &xb, &cb) && wa == wb && ca >= 4u && cb >= 4u &&
            zc_dmat(w415_wc.Di, B->ops[xb].mvp, A->ops[xa].mvp)) {
            for (i = 0; i < B->nops && nok < 3u; i++) { /* Bestaetigung: gelerpte, bewegte Matrix-Draws mit demselben Tickweg */
                const ip_op_t *r = &B->ops[i];
                const ip_op_t *q = (r->match >= 0 && (unsigned)r->match < A->nops) ? &A->ops[r->match] : NULL;
                double Dx[16];
                if (r->kind != IP_DRAW || !r->cls || r->rtv != wb || !r->flag || !q || r->why != IP_W_LERP || r->rzb || r->rzm ||
                    ip_mvp_affine(r->mvp) || memcmp(q->mvp, r->mvp, sizeof r->mvp) == 0) { continue; }
                if (zc_dmat(Dx, r->mvp, q->mvp) && w415_dev(Dx, w415_wc.Di) < 2e-4) { nok++; }
            }
            if (nok >= 3u) {
                w415_wc.ok = 1; w415_wc.rtv = wb; w415_wc.ia = xa;
                memcpy(w415_wc.camB, B->ops[xb].mvp, sizeof w415_wc.camB);
                st_w415_cam++;
            } else { st_w415_rej++; }
        } else { st_w415_nocam++; }
    }
    if (!w415_wc.ok || o->rtv != w415_wc.rtv) { return 0; }
    memcpy(Di, w415_wc.Di, sizeof w415_wc.Di);
    if (ia) { *ia = w415_wc.ia; }
    return 1;
}
/* W415: Bezugs-Draw (Tick N) der W410-Kamera im Ziel von o (nach rz_cam_pre), NULL = keiner */
const ip_op_t *w415_cam_ref(const ip_list_t *B, const ip_op_t *o)
{
    int x;
    for (x = 0; x < rz_pn; x++) {
        if (rz_prt[x] == o->rtv && rz_pds[x] == rz_dkey(o) && rz_prr[x][0] >= 0) { return &B->ops[rz_prr[x][0]]; }
    }
    return NULL;
}
const char *pr_why(unsigned w)
{
    return w == IP_W_LERP ? "lerp" : w == IP_W_RAWLERP ? "rawlerp" : w == IP_W_NOMATCH ? "nomatch" :
           w == IP_W_RAWCHG ? "rawchg" : w == IP_W_REJECT ? "reject" : w == IP_W_STATIC ? "static" : "?";
}

static void ip_pair_robust(ip_list_t *A, ip_list_t *B)
{
    const int on = g_cfg.interp_pair & 1, diag = g_cfg.interp_pair_diag > 0;
    const float thr = g_cfg.interp_pair_thr, mov = g_cfg.interp_pair_mov;
    unsigned i, j, npp = 0;
    int x;
    LONGLONG q0 = ip_now(), q1;
    if (g_cfg.vr_enable || g_cfg.interp_mode != 0 || !(g_cfg.interp_streak & 1)) { return; }
    if (pr_capb < B->nops) {
        if (!pr_grow((void **)&pr_pc, B->nops, sizeof *pr_pc) || !pr_grow((void **)&pr_c1, B->nops, sizeof *pr_c1) ||
            !pr_grow((void **)&pr_c2, B->nops, sizeof *pr_c2) || !pr_grow((void **)&pr_b1, B->nops, sizeof *pr_b1) ||
            !pr_grow((void **)&pr_in, B->nops, sizeof *pr_in) || !pr_grow((void **)&pr_tgo, B->nops, sizeof *pr_tgo) ||
            !pr_grow((void **)&pr_old, B->nops, sizeof *pr_old) || !pr_grow((void **)&pr_orzb, B->nops, sizeof *pr_orzb)) { return; }
        pr_capb = B->nops;
    }
    if (pr_capa < A->nops) {
        if (!pr_grow((void **)&pr_ac, A->nops, sizeof *pr_ac) || !pr_grow((void **)&pr_ac1, A->nops, sizeof *pr_ac1) ||
            !pr_grow((void **)&pr_ac2, A->nops, sizeof *pr_ac2) || !pr_grow((void **)&pr_ab1, A->nops, sizeof *pr_ab1) ||
            !pr_grow((void **)&pr_hn, A->nops, sizeof *pr_hn) || !pr_grow((void **)&pr_hk, A->nops, sizeof *pr_hk) ||
            !pr_grow((void **)&pr_at, A->nops, sizeof *pr_at) || !pr_grow((void **)&pr_acl, A->nops, sizeof *pr_acl)) { return; }
        pr_capa = A->nops;
    }
    /* 1. Kamera je Ziel: haeufigstes Matrix-Paar gelerpter 3D-Draws (W294 Bit 1); steht die Kamera (mehr ruhende
     *    als bewegte Welt-Draws), D = I. */
    rz_cam_pre(A, B);
    pr_ntg = 0;
    for (i = 0; i < B->nops; i++) {
        const ip_op_t *o = &B->ops[i];
        pr_tgo[i] = -1;
        if (o->kind != IP_DRAW || !o->cls || o->nolerp) { continue; }
        pr_tgo[i] = pr_tgt(o);
        if (pr_tgo[i] >= 0 && o->why == IP_W_STATIC && !pr_pre(o)) { pr_tg[pr_tgo[i]].nstat++; }
    }
    for (x = 0; x < pr_ntg; x++) {
        pr_tg_t *g = &pr_tg[x];
        int t, k;
        unsigned cnt = 0;
        for (t = 0; t < rz_pn; t++) { if (rz_prt[t] == g->rtv && rz_pds[t] == g->dk) { break; } }
        if (t < rz_pn && rz_prr[t][0] >= 0) { cnt = rz_prc[t][0]; }
        if (cnt >= 4u && cnt >= g->nstat) {
            const ip_op_t *r = &B->ops[rz_prr[t][0]];
            if (zc_dmat(g->Di, r->mvp, A->ops[r->match].mvp)) { g->ok = 1; g->ref = r->match; } /* D^-1 = M_B^-1*M_A */
        } else if (g->nstat >= 8u) {
            for (k = 0; k < 16; k++) { g->Di[k] = (k % 5) == 0 ? 1.0 : 0.0; }
            for (i = 0; i < B->nops && g->ref < 0; i++) { /* Bezug fuer match: ein ruhender Draw des Ziels */
                const ip_op_t *o = &B->ops[i];
                if (pr_tgo[i] == x && o->why == IP_W_STATIC && o->match >= 0 && (unsigned)o->match < A->nops) { g->ref = o->match; }
            }
            g->ok = g->ref >= 0;
        }
    }
    /* 2. Lagen: A tatsaechlich (N-1), Raster ueber A */
    memset(pr_hh, 0xFF, sizeof pr_hh);
    for (j = A->nops; j-- > 0;) {
        const ip_op_t *a = &A->ops[j];
        int q, cx, cy, s;
        pr_at[j] = 0; pr_ac1[j] = pr_ac2[j] = IP_COST_INF; pr_ab1[j] = -1;
        if (!pr_cand_a(a) || pr_pre(a)) { continue; }
        for (q = 0; q < 4; q++) { /* Clip x/y/w aus den beim Aufzeichnen berechneten NDC (z geht nicht ein) */
            float w = a->rn[q][2];
            if (w > 1e-6f) { pr_ac[j][q][0] = a->rn[q][0] * w; pr_ac[j][q][1] = a->rn[q][1] * w; pr_ac[j][q][2] = 0.0f; pr_ac[j][q][3] = w; }
            else { ip_clip(a->pref[q], a->mvp, pr_ac[j][q]); }
        }
        pr_cell(pr_ac[j][0], &cx, &cy, &s);
        pr_acl[j][0] = cx; pr_acl[j][1] = cy; pr_acl[j][2] = s;
        pr_hk[j] = pr_hkey(a->sig, cx, cy, s);
        pr_hn[j] = pr_hh[pr_hk[j]];
        pr_hh[pr_hk[j]] = (int)j;
    }
    q1 = ip_now();
    st_pr_ms1 += ip_ms(q0, q1);
    /* Menge: Welt-3D-Draws mit Kamera (ohne P2/vorab, Effekte, statisch, Geometrie-Partner W73). Reserviert bleiben
     * die Partner aller anderen Draws. */
    for (i = 0; i < B->nops; i++) {
        ip_op_t *o = &B->ops[i];
        int in;
        pr_in[i] = 0; pr_old[i] = -1; pr_b1[i] = -1; pr_c1[i] = pr_c2[i] = IP_COST_INF;
        if (o->kind != IP_DRAW) { continue; }
        in = o->cls && !o->nolerp && !o->fxk && pr_tgo[i] >= 0 && pr_tg[pr_tgo[i]].ok && !pr_pre(o) && o->why != IP_W_STATIC &&
             !(o->why == IP_W_LERP && o->match >= 0 && A->ops[o->match].rawhash != o->rawhash);
        if (in) {
            pr_in[i] = 1;
            pr_orzb[i] = (unsigned char)(o->rzb != 0);
            if (o->flag && o->match >= 0) { pr_old[i] = o->match; }
        } else if (o->match >= 0 && (unsigned)o->match < A->nops && o->why != IP_W_RAWCHG && o->why != IP_W_NOMATCH) {
            pr_at[o->match] = 1;
        }
    }
    /* 3. Kandidaten (9 Rasterzellen um die vorhergesagte Lage) und Kosten */
    for (i = 0; i < B->nops; i++) {
        ip_op_t *o = &B->ops[i];
        float Mp[16];
        int q, cx, cy, s, dx, dy, ncand = 0, nsame = 0;
        if (!pr_in[i] && !(diag && o->kind == IP_DRAW && o->cls && !o->nolerp && pr_tgo[i] >= 0 && pr_tg[pr_tgo[i]].ok && !pr_pre(o))) { continue; }
        pr_mul_di(Mp, o->mvp, pr_tg[pr_tgo[i]].Di);
        for (q = 0; q < 4; q++) { ip_clip(o->pref[q], Mp, pr_pc[i][q]); }
        pr_cell(pr_pc[i][0], &cx, &cy, &s);
        for (dy = -1; dy <= 1 && ncand < PR_CMAX; dy++) {
            for (dx = -1; dx <= 1 && ncand < PR_CMAX; dx++) {
                unsigned h = pr_hkey(o->sig, cx + dx, cy + dy, s);
                int jj;
                for (jj = pr_hh[h]; jj >= 0 && ncand < PR_CMAX; jj = pr_hn[jj]) {
                    const ip_op_t *a;
                    float c;
                    if (pr_acl[jj][0] != cx + dx || pr_acl[jj][1] != cy + dy || pr_acl[jj][2] != s) { continue; } /* Hash-Kollision */
                    a = &A->ops[jj];
                    if (a->sig != o->sig || a->rtv != o->rtv || a->nv != o->nv || a->topo != o->topo) { continue; }
                    ncand++;
                    if (a->rawhash == o->rawhash) { nsame++; }
                    c = pr_cost(pr_pc[i], pr_ac[jj]);
                    if (c < pr_c1[i] || (c == pr_c1[i] && jj < pr_b1[i])) { pr_c2[i] = pr_c1[i]; pr_c1[i] = c; pr_b1[i] = jj; }
                    else if (c < pr_c2[i]) { pr_c2[i] = c; }
                    if (!pr_in[i] || pr_at[jj] || c > mov) { continue; }
                    if (c < pr_ac1[jj] || (c == pr_ac1[jj] && (int)i < pr_ab1[jj])) { pr_ac2[jj] = pr_ac1[jj]; pr_ac1[jj] = c; pr_ab1[jj] = (int)i; }
                    else if (c < pr_ac2[jj]) { pr_ac2[jj] = c; }
                    if (c <= thr) { pr_push(&npp, c, (int)i, jj); } /* Welt-Partner; bewegte nur als bester (unten) */
                }
            }
        }
        if (pr_in[i] && pr_old[i] < 0 && pr_c1[i] > thr && pr_c1[i] <= mov && pr_b1[i] >= 0 && !pr_at[pr_b1[i]]) { /* bewegt: nur fuer bisher exakte */
            pr_push(&npp, pr_c1[i], (int)i, pr_b1[i]);
        }
        if (diag) { /* Zensus: Klasse und Ergebnis der bisherigen Zuordnung */
            int cl, out;
            const ip_op_t *m = (o->match >= 0 && (unsigned)o->match < A->nops) ? &A->ops[o->match] : NULL;
            float cm = IP_COST_INF, dd[3];
            if (nsame >= 2) { cl = 0; }
            else if (m && m->rawhash != o->rawhash && m->nv == o->nv && pr_cand_a(m) && !pr_pre(m) && rz_offset_grid(m, o, dd)) { cl = 1; }
            else if (o->nv <= 12u && !nsame) { cl = 2; }
            else { cl = 4; }
            if (m && o->flag && pr_cand_a(m) && !pr_pre(m)) {
                float mc[4][4];
                const ip_op_t *mm = o->rzb ? o : m; /* rzb: Tick-N-Vertices mit Matrix N-1 */
                for (q = 0; q < 4; q++) { ip_clip(mm->pref[q], o->rzb ? IP_AMVP(o, m) : m->mvp, mc[q]); }
                cm = pr_cost(pr_pc[i], mc);
            }
            if (o->why == IP_W_STATIC) { out = 6; }
            else if (o->flag) { out = cm <= thr ? 0 : (pr_c1[i] <= thr ? 1 : 4); }
            else { out = pr_c1[i] <= thr ? 2 : (!ncand ? 3 : 5); }
            st_pz[cl][out]++;
            if (g_cfg.interp_pair_diag >= 2 && out != 0 && out != 6 && st_pr_ln < 400u && (g_cfg.pf_ring <= 0 || sb_pf_active())) {
                st_pr_ln++;
                HLOG_I("INTERP pzd f=%u i=%u n=%u why=%s rzb=%d klasse=%s ergebnis=%s m=%d kosten_alt=%.4f best=%d kosten=%.4f zweit=%.4f kand=%d gleich=%d",
                       B->frame, i, o->nv, pr_why(o->why), o->rzb, kPR_CLS[cl], kPR_OUT[out], o->match, (double)cm,
                       pr_b1[i], (double)pr_c1[i], (double)pr_c2[i], ncand, nsame);
            }
        }
    }
    for (i = 0; diag && i < B->nops; i++) { /* Zensus: vorab transformierte und Ziele ohne Kamera nach altem Ergebnis */
        const ip_op_t *o = &B->ops[i];
        int cl, out;
        if (o->kind != IP_DRAW || !o->cls || o->nolerp || pr_tgo[i] < 0) { continue; }
        if (pr_pre(o)) { cl = 3; } else if (!pr_tg[pr_tgo[i]].ok) { cl = 5; } else { continue; }
        out = o->why == IP_W_STATIC ? 6 : (o->flag ? 0 : 5);
        st_pz[cl][out]++;
    }
    st_pr_ms2 += ip_ms(q1, ip_now());
    q1 = ip_now();
    if (!on) { goto done; }
    /* 4. global gierig nach Kosten */
    if (npp > 1u) { qsort(pr_pp, npp, sizeof *pr_pp, pr_pcmp); }
    for (i = 0; i < B->nops; i++) { if (pr_in[i]) { B->ops[i].match = -1; } }
    for (j = 0; j < npp; j++) {
        const pr_pair_t *p = &pr_pp[j];
        ip_op_t *o = &B->ops[p->o];
        const ip_op_t *a = &A->ops[p->a];
        if (o->match >= 0 || pr_at[p->a]) { continue; }
        if (p->c > thr) { /* bewegter Partner: nur gegenseitig beste und eindeutige Wahl */
            float lim = 2.0f * p->c + 0.01f;
            if (pr_b1[p->o] != p->a || pr_ab1[p->a] != p->o || pr_c2[p->o] < lim || pr_ac2[p->a] < lim) { continue; }
            if (!ip_plausible(a, o, g_cfg.interp_maxjump * (a->rawhash == o->rawhash ? 1.0f : 0.3f))) { continue; }
        }
        if (a->rawhash != o->rawhash && a->ib1 != o->ib1) { continue; } /* Vertex-Lerp nur gleich abgelegt (W264) */
        o->match = p->a;
        pr_at[p->a] = 1;
    }
    /* 5. Ergebnis je Draw */
    for (i = 0; i < B->nops; i++) {
        ip_op_t *o = &B->ops[i];
        unsigned char oldwhy;
        if (!pr_in[i]) { continue; }
        st_pr_set++;
        oldwhy = o->why;
        o->rzb = 0; o->rzm = 0;
        if (o->match >= 0) {
            const ip_op_t *a = &A->ops[o->match];
            if (a->rawhash == o->rawhash) {
                if ((a->outhash == o->outhash && memcmp(a->mvp, o->mvp, sizeof o->mvp) == 0) || ip_still(a, o)) {
                    o->flag = 0; o->why = IP_W_STATIC; st_pr_still++;
                } else {
                    o->flag = 1; o->why = IP_W_LERP; st_pr_pair++;
                    if (a->ib1 != o->ib1) { o->rzb = 1; } /* gleiche Daten, anders abgelegt: Tick-N-Vertices mit Matrix N-1 (W264-Pruefung) */
                }
            } else { o->flag = 1; o->why = IP_W_RAWLERP; st_pr_raw++; }
            if (pr_old[i] != o->match) { st_pr_chg++; }
            continue;
        }
        if (pr_old[i] >= 0) { /* alte Paarung bleibt (bewegtes Objekt; Partner ggf. mit einem weltkonsistenten Draw geteilt) */
            o->match = pr_old[i];
            pr_at[pr_old[i]] = 1;
            o->flag = 1;
            o->rzb = pr_orzb[i]; /* W284/W294: Tick-N-Vertices mit Matrix N-1 des alten Partners */
            o->why = oldwhy;
            st_pr_keep++;
            continue;
        }
        /* mit der Kamera bewegen: Matrix N-1 = M_B * D^-1 auf die Tick-N-Vertices */
        pr_mul_di(o->mva, o->mvp, pr_tg[pr_tgo[i]].Di);
        o->match = pr_tg[pr_tgo[i]].ref;
        o->rzm = 1; o->rzb = 1; o->flag = 1; o->why = IP_W_LERP;
        st_pr_cam++;
        if (oldwhy == IP_W_LERP || oldwhy == IP_W_RAWLERP) { st_pr_chg++; }
    }
    for (i = 0; g_cfg.interp_pair_diag >= 2 && i < B->nops; i++) { /* DIAG: geaenderte Entscheidungen */
        const ip_op_t *o = &B->ops[i];
        if (!pr_in[i] || st_pr_lnd >= 2000u || (g_cfg.pf_ring > 0 && !sb_pf_active())) { continue; }
        if (o->match == pr_old[i] && !o->rzm) { continue; }
        st_pr_lnd++;
        HLOG_I("INTERP prd f=%u i=%u n=%u alt=%d neu=%s/%d kamera=%d kosten=%.4f zweit=%.4f", B->frame, i, o->nv, pr_old[i], pr_why(o->why), o->match,
               o->rzm, (double)pr_c1[i], (double)pr_c2[i]);
    }
    /* 6. Bit 2: Durchgaenge derselben Instanz (Ziel, Geometrie, Matrix) folgen dem ersten Durchgang */
    if (g_cfg.interp_pair & 2) {
        static int gh[1u << IP_HBITS];
        memset(gh, 0xFF, sizeof gh);
        for (i = 0; i < B->nops; i++) { /* pr_old wird hier Hash-Kette */
            ip_op_t *o = &B->ops[i];
            const ip_op_t *L, *la;
            const float *Mprev;
            unsigned h;
            int k, lead = -1;
            if (!pr_in[i]) { continue; } /* Gruppen innerhalb der Menge (Leiter = erster Durchgang darin) */
            h = IP_HB(ip_mvpkey(o) ^ (o->gkey << 4));
            for (k = gh[h]; k >= 0; k = pr_old[k]) {
                const ip_op_t *e = &B->ops[k];
                if (e->gkey == o->gkey && e->rtv == o->rtv && e->rawhash == o->rawhash && e->nv == o->nv &&
                    memcmp(e->mvp, o->mvp, sizeof o->mvp) == 0) { lead = k; break; }
            }
            if (lead < 0) { pr_old[i] = gh[h]; gh[h] = (int)i; continue; }
            L = &B->ops[lead];
            if (!L->flag || L->match < 0 || (unsigned)L->match >= A->nops) { continue; }
            la = &A->ops[L->match];
            if (!L->rzb && la->rawhash != L->rawhash) { continue; } /* Leiter mit Vertex-Lerp: keine gemeinsame Matrix */
            Mprev = IP_AMVP(L, la);
            if (o->flag && o->match >= 0 && !o->rzb && A->ops[o->match].rawhash == o->rawhash &&
                memcmp(A->ops[o->match].mvp, Mprev, sizeof o->mvp) == 0) { continue; } /* schon gleich */
            memcpy(o->mva, Mprev, sizeof o->mva);
            o->match = L->match;
            o->rzm = 1; o->rzb = 1; o->flag = 1; o->why = IP_W_LERP;
            st_pr_follow++;
        }
    }
    st_pr_ms3 += ip_ms(q1, ip_now());
done:
    st_pr_ticks++;
    {
        double ms = ip_ms(q0, ip_now());
        st_pr_ms += ms;
        if (ms > st_pr_max) { st_pr_max = ms; }
    }
}

/* DIAG(W340 LOC-FRAMES, interp_diag>=1, nur LoC): Gruende der unplausiblen Zuordnungen (reject) */
unsigned li_rj_w, li_rj_j[4], li_rj_big, li_rj_n, li_rj_cam;
unsigned li_rz_alt, li_rz_vl, li_rj_cam2; /* W349: rawchg mit anderem Kamera-passenden Partner gleicher Signatur (LOCINTERP stat rzalt) */
static void li_rej_note(const ip_op_t *a, const ip_op_t *o)
{
    int q, wx = 0;
    float mj = 0.0f;
    for (q = 0; q < 4; q++) {
        float c0[4], c1[4];
        ip_clip(a->pref[q], a->mvp, c0);
        ip_clip(o->pref[q], o->mvp, c1);
        if (c0[3] > 1e-6f && c1[3] > 1e-6f) {
            float dx = fabsf(c1[0] / c1[3] - c0[0] / c0[3]), dy = fabsf(c1[1] / c1[3] - c0[1] / c0[3]);
            if (dx > mj) { mj = dx; }
            if (dy > mj) { mj = dy; }
        } else if (!(c0[3] < -1e-6f && c1[3] < -1e-6f)) {
            wx = 1;
        }
    }
    li_rj_n++;
    if (o->nv >= 1000u) { li_rj_big++; }
    if (wx) { li_rj_w++; }
    else { li_rj_j[mj < 0.75f ? 0 : mj < 1.0f ? 1 : mj < 2.0f ? 2 : 3]++; }
}
unsigned st_p2c[5]; /* gelerpt reject 3D, reject P2, nomatch 3D, nomatch P2, Kamera passt nicht */
unsigned st_p2e[6]; /* abgelehnt nach Fehler: <0,1 / <0,2 / <0,5 / <1 / >=1 / keine Kamera; DIAG-Verteilung */
unsigned st_p2ok[4]; /* angenommen nach Fehler: <1e-3 / <5e-3 / <2e-2 / <=0,05 */
static float ip_p2hom_err(const ip_op_t *a, const ip_op_t *o, int p2, const double D[8][16], int nD)
{
    int x, q, j;
    float best = 1e30f;
    for (x = 0; x < nD; x++) {
        float emax = 0.0f;
        for (q = 0; q < 4 && emax <= best; q++) {
            float c0[4], c1[4];
            double p[4], dx, dy, dw, nb, e;
            ip_clip(a->pref[q], a->mvp, c0);
            ip_clip(o->pref[q], o->mvp, c1);
            if (p2) { /* Strom schon durch w geteilt (w = 1): Clip-Lage = Bildlage * echte w */
                for (j = 0; j < 3; j++) { c0[j] *= a->prefw[q]; c1[j] *= o->prefw[q]; }
                c0[3] = a->prefw[q]; c1[3] = o->prefw[q];
            }
            for (j = 0; j < 4; j++) {
                p[j] = c0[0] * D[x][0 * 4 + j] + c0[1] * D[x][1 * 4 + j] + c0[2] * D[x][2 * 4 + j] + c0[3] * D[x][3 * 4 + j];
            }
            dx = p[0] - c1[0]; dy = p[1] - c1[1]; dw = p[3] - c1[3];
            nb = fabs(c1[0]) + fabs(c1[1]) + fabs(c1[3]) + fabs(p[0]) + fabs(p[1]) + fabs(p[3]);
            e = nb > 1e-9 ? 2.0 * (fabs(dx) + fabs(dy) + fabs(dw)) / nb : 1.0;
            if ((float)e > emax) { emax = (float)e; }
        }
        if (emax < best) { best = emax; }
    }
    return best;
}
static void ip_p2hom_cam(ip_list_t *A, ip_list_t *B)
{
    unsigned i;
    int pre = 0;
    for (i = 0; i < B->nops; i++) {
        ip_op_t *o = &B->ops[i];
        ip_op_t *a;
        double D[8][16];
        int nD, p2, g = -1, nm;
        float best;
        if (o->kind != IP_DRAW || !o->cls || o->flag) { continue; }
        nm = o->why == IP_W_NOMATCH;
        if (nm) {
            if (o->match >= 0) { continue; }
            g = ip_pick_geo(A, o, 0);
            if (g < 0 || A->ops[g].taken) { continue; }
            a = &A->ops[g];
        } else {
            if (o->why != IP_W_REJECT || o->match < 0 || (unsigned)o->match >= A->nops) { continue; }
            a = &A->ops[o->match];
        }
        p2 = ip_mvp_affine(o->mvp);
        if (a->kind != IP_DRAW || !a->cls || a->rawhash != o->rawhash || a->nv != o->nv || p2 != ip_mvp_affine(a->mvp) ||
            a->rtv != o->rtv || a->ib1 != o->ib1) { continue; }
        if (p2 && (!a->p2w || !o->p2w || !A->wb_ok || !B->wb_ok || sb_ip_hlerp() < 2)) { continue; }
        if (!pre) { rz_cam_pre(A, B); pre = 1; }
        nD = rz_cam(A, B, o, D);
        best = nD ? ip_p2hom_err(a, o, p2, D, nD) : 1e30f;
        if (best <= (nm ? P2HC_TOL_NM : P2HC_TOL)) {
            if (nm) { o->match = g; a->taken = 1; }
            o->flag = 1; o->why = IP_W_LERP; /* reject-Zaehler bleibt (Schnitt-Erkennung) */
            st_p2c[(nm ? 2 : 0) + (p2 ? 1 : 0)]++;
            st_p2ok[best < 1e-3f ? 0 : best < 5e-3f ? 1 : best < 2e-2f ? 2 : 3]++;
        } else {
            static unsigned nl;
            st_p2c[4]++;
            st_p2e[!nD ? 5 : best < 0.1f ? 0 : best < 0.2f ? 1 : best < 0.5f ? 2 : best < 1.0f ? 3 : 4]++;
            if (g_cfg.seam_diag > 0 && nl < (unsigned)g_cfg.seam_diag && (g_cfg.pf_ring <= 0 || sb_pf_active())) { /* W429 DIAG */
                nl++;
                HLOG_I("P2HC f=%u i=%u n=%u p2=%d %s kameras=%d fehler=%.3g", B->frame, i, o->nv, p2, nm ? "nomatch" : "reject", nD, (double)best);
            }
        }
    }
}

int ip_match(ip_list_t *A, ip_list_t *B, int log)
{
    unsigned i, jp = 0, ndraw = 0, nclr = 0, nworld = 0, nmatch = 0, nlerp = 0,
             nstatic = 0, nnomatch = 0, nrawchg = 0, nreject = 0, n2d = 0, inorder = 0,
             nrawlerp = 0, nrt2d = 0, ngeo = 0;
    unsigned bbdraws = 0;
    for (i = 0; i < A->nops; i++) { A->ops[i].taken = 0; }
    if (!ip_hash_build(A)) { return 0; }
    if (g_cfg.interp_inst) { /* POPIN (W91): ip_inst_closer prueft noch offene Draws von B */
        for (i = 0; i < B->nops; i++) { B->ops[i].match = -1; B->ops[i].why = IP_W_NONE; }
        if (!ip_bhash_build(B)) { return 0; }
    }
    for (i = 0; i < B->nops; i++) {
        ip_op_t *o = &B->ops[i];
        int found = -1, sigonly = -1, iwbest = -1;
        o->match = -1;
        o->flag = 0;
        o->rzb = 0; /* W284 */
        o->rzm = 0; /* W317 */
        o->why = IP_W_NONE;
        if (o->kind != IP_DRAW) { nclr++; continue; }
        ndraw++;
        if (o->rtv == g_rtv) { bbdraws++; }
        if (o->nolerp) { n2d++; o->why = IP_W_RT2DEX; continue; } /* MONITOR-FX (W105): Korn/Stoerung wechselt im Spieltakt, exakt aus Tick N */
        if (!o->cls && o->rtv == g_rtv) { n2d++; o->why = IP_W_BB2D; continue; } /* HUD/2D im BB: immer exakt */
        if (!o->cls) {
            /* 2D/RHW-Draw in ein RT (vom Spiel vorprojizierte Welt- oder
             * Monitor-Inhalte): nur Vertex-Zuordnung mit strenger Grenze;
             * unveraendert -> exakt. */
            int s2;
            n2d++;
            o->why = IP_W_RT2DEX;
            s2 = ip_pick(A, o, jp, 1);
            if (s2 < 0) { s2 = ip_pick(A, o, jp, -1); }
            if (s2 >= 0) {
                ip_op_t *a = &A->ops[s2];
                a->taken = 1;
                jp = (unsigned)s2 + 1u;
                o->match = s2;
                if (!(a->outhash == o->outhash && memcmp(a->mvp, o->mvp, sizeof o->mvp) == 0) &&
                    ip_plausible(a, o, g_cfg.interp_maxjump * 0.3f)) {
                    o->flag = 1;
                    o->why = IP_W_RT2DLERP;
                    nrt2d++;
                }
            }
            continue;
        }
        nworld++;
        if (g_cfg.interp_inst) {
            int amb = 0;
            found = ip_pick_inst(A, B, i, &amb);
            if (amb) { /* mehrere gleichwertige Instanzen, keine eindeutig -> exakt */
                st_iamb++;
                nnomatch++;
                o->why = IP_W_NOMATCH;
                continue;
            }
        } else {
            found = ip_pick(A, o, jp, 1);
            if (found >= 0 && g_cfg.interp_diag >= 1) { /* DIAG(POPIN,W91): Reihenfolge-Wahl vs. naechste Instanz */
                int n, second, best;
                float c1, c2, cf = ip_cost(&A->ops[found], o);
                best = ip_inst_best(A, o, &n, &c1, &c2, &second);
                if (n > 1) { st_imulti++; }
                if (best >= 0 && best != found && cf > 2.0f * c1 + 0.02f) { st_iwrong++; iwbest = best; }
            }
        }
        if (found < 0) { sigonly = ip_pick(A, o, jp, 0); }
        if (found < 0 && sigonly >= 0) {
            /* Gleiche Signatur, neue Eingangs-Positionen (z. B. kamera-relativ
             * neu gebackenes Gelaende, CPU-animiert): Vertex i <-> Vertex i,
             * nur bei kleinem Sprung aller 4 Bezugs-Vertices (strenger als
             * bei Matrix-Draws, schuetzt vor vertauschten Partikeln). */
            ip_op_t *a = &A->ops[sigonly];
            if (ip_plausible(a, o, g_cfg.interp_maxjump * ((o->fxk && g_cfg.fx_interp) ? 1.0f : 0.3f))) { /* W133 fx_interp */
                a->taken = 1;
                jp = (unsigned)sigonly + 1u;
                o->match = sigonly;
                o->flag = 1;
                o->why = IP_W_RAWLERP;
                nmatch++;
                nlerp++;
                nrawlerp++;
                continue;
            }
        }
        if (found < 0) {
            int g = ip_pick_geo(A, o, jp);
            if (g >= 0 && ip_plausible(&A->ops[g], o, g_cfg.interp_maxjump * ((o->fxk && g_cfg.fx_interp) ? 1.0f : 0.3f))) { /* W133 fx_interp */
                A->ops[g].taken = 1;
                o->match = g;
                nmatch++;
                if (memcmp(A->ops[g].mvp, o->mvp, sizeof o->mvp) == 0) {
                    nstatic++;
                    o->why = IP_W_STATIC;
                } else {
                    o->flag = 1;
                    o->why = IP_W_LERP;
                    nlerp++;
                    ngeo++;
                }
                continue;
            }
            if (sigonly >= 0) { nrawchg++; o->why = IP_W_RAWCHG; o->match = sigonly; }
            else { nnomatch++; o->why = IP_W_NOMATCH; }
            continue;
        }
        {
            ip_op_t *a = &A->ops[found];
            if ((unsigned)found == jp) { inorder++; }
            a->taken = 1;
            jp = (unsigned)found + 1u;
            o->match = found;
            nmatch++;
            if (a->outhash == o->outhash && memcmp(a->mvp, o->mvp, sizeof o->mvp) == 0) {
                nstatic++;
                o->why = IP_W_STATIC;
                continue;
            }
            if (ip_still(a, o)) { nstatic++; o->why = IP_W_STATIC; }
            else if (w448_eye_blind(a, o)) { nnomatch++; o->why = IP_W_NOMATCH; st_w448_eye++; } /* FIX(W448): exakt */
            else if (ip_plausible(a, o, g_cfg.interp_maxjump)) { o->flag = 1; nlerp++; o->why = IP_W_LERP; }
            else if (g_cfg.interp_p2rej && ip_p2rej_ok(a, o, g_cfg.interp_maxjump)) { o->flag = 1; nlerp++; o->why = IP_W_LERP; st_p2rej++; } /* W406 */
            else if (g_cfg.interp_p2hom && !g_sba_loc && !g_cfg.vr_enable && ip_rej_relax(A, B, a, o, g_cfg.interp_maxjump)) {
                /* FIX(W429): reject-Zaehler bleibt (Schnitt-Erkennung "viele unplausible Spruenge" unveraendert) */
                nreject++; o->flag = 1; o->why = IP_W_LERP; st_p2hom[ip_mvp_affine(o->mvp) ? 1 : 0]++;
            }
            else if (g_sba_loc && g_cfg.loc_sky_fix && o->lsk && a->lsk && !g_cfg.vr_enable) {
                /* W435 LOC-SKY-LAG-REG: Himmel in beiden Listen (Spalten x, y, w = Kamera) -> der Matrix-Lerp zeigt ihn genau
                 * mit der Zwischenkamera der Welt (Bezugs-Vertices des grossen Quads springen beim Drehen > interp_maxjump).
                 * reject-Zaehler bleibt (Schnitt-Erkennung unveraendert, wie W429). */
                nreject++; o->flag = 1; o->why = IP_W_LERP; InterlockedIncrement(&sk_rejlerp);
            }
            else { nreject++; o->why = IP_W_REJECT; if (g_sba_loc && g_cfg.interp_diag >= 1) { li_rej_note(a, o); } }
            if (iwbest >= 0 && o->why == IP_W_LERP) { ip_iwrong_log(A, B, i, found, iwbest); } /* DIAG(POPIN,W91) */
        }
    }
    if (g_cfg.interp_p2hom && !g_sba_loc && !g_cfg.vr_enable) { ip_p2hom_cam(A, B); } /* FIX(W429) vor interp_pass */
    if (g_cfg.interp_rawzc) {
        /* FIX(W284 INTERP-RAWCHG): "rawchg" = gleiche Signatur, neue Eingangsdaten, Vertex-Lerp an
         * ip_plausible gescheitert (Bezugs-Vertices wechseln die Augenebene oder springen weit) -> bisher
         * exakt (Tick N) zwischen gelerpten Nachbarn = W251-Tiefenmischung (W273_id2: Wasser/Fels-Draw
         * n=42, Eingangsdaten um (0,0,-5) verschoben, gewinnt bei t=0,25 gegen die gelerpte Strasse).
         * Ist die Aenderung eine reine Verschiebung p_B = p_A + d (Gelaende kamera-relativ neu
         * gebacken), gilt p_B*M_B = p_A*(T(d)*M_B): derselbe Fall wie interp_zcons mit M_B' = T(d)*M_B.
         * Passt M_A^-1*M_B' zur Kamerabewegung des Ziels, ist der Vertex-Lerp i <-> i exakt die
         * Zwischenkamera des uebrigen Bildes (auch fuer Vertices hinter der Kamera: linear im
         * Clip-Raum, die Hardware schneidet an der Nahebene). Vor interp_pass, damit weitere
         * Durchgaenge derselben Instanz nicht wegen dieses Draws exakt werden. */
        enum { RZ_T = 8 };
        ID3D11RenderTargetView *zr[RZ_T];
        ID3D11DepthStencilView *zd[RZ_T];
        double zD[RZ_T][8][16];
        int znr[RZ_T], s0ok = 0;
        unsigned nz = 0, k;
        LONGLONG rzq0 = ip_now();
        rz_cam_pre(A, B); /* Quellen = vor diesem Block gelerpte Draws (in diesem Block umgestellte zaehlen nicht mit) */
        for (i = 0; i < B->nops; i++) {
            ip_op_t *o = &B->ops[i];
            ip_op_t *a;
            float d[3] = { 0, 0, 0 };
            double Dx[16], e = 1.0, e0 = 1.0;
            float MBf[16];
            int t = -1, x, c, ofs;
            if (o->kind != IP_DRAW || !o->cls) { continue; }
            if (!o->dsv && !(g_cfg.interp_streak & 2)) { continue; }
            if (g_sba_loc && g_cfg.loc_stat_fix && !g_cfg.vr_enable && o->dsv && !o->rzb && !ip_mvp_affine(o->mvp) &&
                (o->why == IP_W_NOMATCH || o->why == IP_W_RAWCHG || o->why == IP_W_REJECT ||
                 (o->why == IP_W_RAWLERP && o->flag && o->nv >= 1000u))) {
                /* FIX(W415 LOC-STATIC-LAG): Draw in Weltkoordinaten (Matrix bitgleich der Kamera der Welt) ohne brauchbaren
                 * Partner: das grosse Bodennetz der Stadt (~45 700 Vertices; Vertexzahl wechselt je Tick -> nomatch, sonst
                 * rawlerp mit falschen Vertex-Partnern bzw. rawchg), Fern-Streifen (nomatch), weite Spruenge (reject). Blieb exakt
                 * (Tick-N-Lage, 20 Hz) bzw. wurde Vertex i <-> i gemischt, waehrend die Gebaeude glitten. Tick-N-Vertices mit der
                 * Welt-Kamera bewegen (rzm, wie W410 Regel 2): exakt fuer ruhende Welt, ohne Drehung D = I = wie bisher; fuer in
                 * der Welt Bewegtes (Partikel in Weltkoordinaten) nie schlechter als exakt. Partner ohne Zuordnung = Platzhalter
                 * (rzb liest nur Tick-N-Vertices). */
                double Dw[16];
                int ia = -1;
                if (w415_world_cam(A, B, o, Dw, &ia) && memcmp(o->mvp, w415_wc.camB, sizeof o->mvp) == 0 &&
                    ((o->match >= 0 && (unsigned)o->match < A->nops) || ia >= 0)) {
                    if (!(o->match >= 0 && (unsigned)o->match < A->nops)) { o->match = ia; }
                    if (o->why == IP_W_NOMATCH) { nnomatch--; nlerp++; }
                    else if (o->why == IP_W_RAWCHG) { nrawchg--; nlerp++; }
                    /* reject: Zaehler bleiben (Schnitt-Erkennung "viele unplausible Spruenge" unveraendert) */
                    pr_mul_di(o->mva, o->mvp, Dw);
                    o->rzm = 1; o->rzb = 1; o->flag = 1;
                    o->why = IP_W_LERP;
                    st_w415_nm++;
                    continue;
                }
            }
            if (o->flag && o->why == IP_W_RAWLERP && !o->rzb && !g_cfg.vr_enable &&
                (g_cfg.loc_interp_rawrzb >= 2 || (g_cfg.loc_interp_rawrzb == 1 && g_sba_loc)) &&
                o->match >= 0 && (unsigned)o->match < A->nops && !ip_mvp_affine(o->mvp)) {
                /* FIX(W410 LOC-GFX-JITTER) loc_interp_rawrzb: "rawlerp" = gleiche Signatur (also gleiche Vertexzahl), neue
                 * Eingangsdaten, Vertex i <-> i, weil die 4 Bezugs-Vertices kaum sprangen. LoC baut das Gelaende-Netz
                 * vor dem VT (~8400-9200 Vertices, ein Draw) je Tick neu; bei gleicher Vertexzahl gehoert Vertex i im
                 * Vortick oft zu einem ANDEREN Weltpunkt (W410 KEIL: 450-720 Vertices > 8 px, bis 914 px daneben) ->
                 * Dreiecke klappen im Zwischenbild um, durch die Loecher scheint der Himmel (weisse Keile, nur > 20 FPS).
                 * (1) Ruht der Draw in der Welt (M_A^-1*M_B passt zur Kamerabewegung des Ziels und die Bezugs-Vertices des
                 * Partners folgen ihr), ist der Matrix-Lerp der Tick-N-Vertices (rzb) genau die Zwischenkamera -
                 * unabhaengig davon, wie das Netz neu gebaut wurde. (2) Grosse Gelaende-Netze (>= 1000 Vertices, mit
                 * Tiefenpuffer), deren Gitter bzw. Ursprung neu gelegt wurde (Matrix-Test scheitert, Bezugs-Vertices folgen
                 * der Kamera nur grob, camfit <= 0,25): Tick-N-Vertices mit der Kamera bewegen (rzm). Bewegte bzw. mit
                 * der Kamera wandernde kleine Netze (Himmel, Billboards, Partikel) bleiben beim Vertex-Lerp. */
                double ew = 1.0, cf = 1e9, Di[16];
                const int big = o->nv >= 1000u && o->dsv != NULL;
                a = &A->ops[o->match];
                if (a->kind == IP_DRAW && a->cls && a->rtv == o->rtv) {
                    for (k = 0; k < nz; k++) { if (zr[k] == o->rtv && zd[k] == rz_dkey(o)) { t = (int)k; break; } }
                    if (t < 0 && nz < RZ_T) {
                        t = (int)nz++;
                        zr[t] = o->rtv; zd[t] = rz_dkey(o);
                        znr[t] = rz_cam(A, B, o, zD[t]);
                    }
                }
                if (t >= 0 && znr[t]) {
                    if (a->dsv == o->dsv && a->topo == o->topo && zc_dmat(Dx, a->mvp, o->mvp)) {
                        for (x = 0; x < znr[t]; x++) { double ex = zc_derr(Dx, zD[t][x]); if (ex < ew) { ew = ex; } }
                    }
                    /* Bezugs-Vertices des Partners: folgen sie wie ruhende Weltpunkte der Kamera (camfit)? In der Welt
                     * bewegte Netze (Partikel, Geschosse: Vertices in Weltkoordinaten, Matrix = Kamera) bestehen den
                     * Matrix-Test auch, ihre Bezugs-Vertices wandern aber -> dort bleibt der Vertex-Lerp. */
                    if (ew <= 1e-3 || big) { cf = li_camfit(a, o, zD[t], znr[t]); }
                    if ((g_cfg.loc_interp_dbg & 2) && (big || (ew <= 1e-3 && cf > 0.01)) && (g_cfg.pf_ring <= 0 || sb_pf_active()) && st_rz_ln < 1400u) { /* DIAG(W410) */
                        st_rz_ln++;
                        HLOG_I("LOCINTERP rawrzb f=%u i=%u n=%u m=%d welt=%.6f camfit=%.5f topo=%u tex=%u", B->frame, i, o->nv, o->match, ew, cf, o->topo, o->has_tex);
                    }
                    if (ew <= 1e-3 && cf <= 0.01) { /* Matrix-Lerp der Tick-N-Vertices; richtig zugeordnete Vertices unveraendert */
                        o->rzb = 1; st_w410_rzb++; if (o->nv >= 1000u) { st_w410_big++; } continue;
                    }
                    if (big && cf <= 0.25 && ((g_cfg.loc_stat_fix && w415_world_cam(A, B, o, Di, NULL)) || w410_cam_inv(A, B, o, Di))) { /* W415: Welt-Kamera */
                        if (g_cfg.loc_stat_fix && w415_wc.ok && w415_wc.rtv == o->rtv) { st_w415_rzm++; }
                        /* Gelaende-Netz mit neu gelegtem Gitter bzw. verschobenem Ursprung (Matrix-Test scheitert, Bezugs-
                         * Vertices sind andere Weltpunkte, folgen aber grob der Kamera): Tick-N-Vertices mit der Kamera
                         * bewegen, Matrix N-1 = M_B * D^-1 (wie W317 rzm). Exakt fuer ruhende Welt, ohne Vertex-Partner. */
                        pr_mul_di(o->mva, o->mvp, Di);
                        o->rzm = 1; o->rzb = 1; st_w410_rzm++;
                        continue;
                    }
                    if (ew <= 1e-3) { st_w410_mov++; } else { st_w410_cam++; }
                }
                t = -1;
            }
            if (o->flag) {
                /* FIX(W294 LINE-STREAK) interp_streak Bit 4: "rawlerp" mit reinem Eingangs-Versatz d != 0
                 * (M2-Meer: Kacheln n=6, Gitter in 5er-Schritten neu um die Kamera gelegt, Partner nach
                 * Reihenfolge). Der Vertex-Lerp schiebt die Kachel um d durch das Zwischenbild, die
                 * rawchg-Nachbarn stehen in Tick-N-Lage -> Naht zwischen den Kachelreihen, durch die der
                 * Hintergrund scheint (heller Strich, Breite ~ (1-t)). Passt M_A^-1*M_B zur Kamera (Kachel
                 * ruht in der Welt), wie rawchg: Matrix-Lerp der Tick-N-Vertices (rzb). */
                if (!(g_cfg.interp_streak & 4) || o->why != IP_W_RAWLERP || o->rzb || g_cfg.vr_enable ||
                    o->match < 0 || (unsigned)o->match >= A->nops || ip_mvp_affine(o->mvp)) { continue; }
                a = &A->ops[o->match];
                if (a->kind != IP_DRAW || !a->cls || a->rtv != o->rtv || a->dsv != o->dsv || a->topo != o->topo ||
                    !rz_offset_grid(a, o, d)) { continue; }
                st_sk_rlofs++;
                for (k = 0; k < nz; k++) { if (zr[k] == o->rtv && zd[k] == rz_dkey(o)) { t = (int)k; break; } }
                if (t < 0) {
                    if (nz >= RZ_T) { continue; }
                    t = (int)nz++;
                    zr[t] = o->rtv; zd[t] = rz_dkey(o);
                    znr[t] = rz_cam(A, B, o, zD[t]);
                }
                if (znr[t] && zc_dmat(Dx, a->mvp, o->mvp)) {
                    for (x = 0; x < znr[t]; x++) { double ex = zc_derr(Dx, zD[t][x]); if (ex < e0) { e0 = ex; } }
                }
                if (g_cfg.interp_diag >= 1 && st_sk_ln < 300u && (g_cfg.pf_ring <= 0 || sb_pf_active())) { /* DIAG(W294) */
                    st_sk_ln++;
                    HLOG_I("INTERP skd f=%u i=%u n=%u m=%d d=%.3f,%.3f,%.3f kameras=%d fehler_welt=%.6f", B->frame, i, o->nv, o->match,
                           (double)d[0], (double)d[1], (double)d[2], znr[t], e0);
                }
                if (e0 > 1e-3) { st_sk_rlcam++; continue; }
                o->rzb = 1;
                st_sk_rl++;
                continue;
            }
            if (!o->dsv) { st_sk_nod++; }
            if (o->why == IP_W_NOMATCH) {
                /* nomatch: kein Vortick-Draw gleicher Signatur und Vertexzahl (z. B. Gelaende-Stueck mit anderer
                 * Vertexzahl, W284 pf7_0540). Welt-Test mit Kandidaten gleicher Signatur ohne Vertexzahl (sig0):
                 * passt M_A^-1*M_B zur Kamera, ruht das Objekt in der Welt -> Matrix-Lerp der Tick-N-Vertices. */
                int j, cand = -1, nc = 0;
                double eb = 1.0;
                if (g_cfg.interp_rawzc < 2 || g_cfg.vr_enable) { continue; }
                if (!s0ok) {
                    s0ok = rz_s0_build(A) ? 1 : -1;
                }
                if (s0ok < 0) { continue; }
                for (k = 0; k < nz; k++) { if (zr[k] == o->rtv && zd[k] == rz_dkey(o)) { t = (int)k; break; } }
                if (t < 0) {
                    if (nz >= RZ_T) { st_rz_other++; continue; }
                    t = (int)nz++;
                    zr[t] = o->rtv; zd[t] = rz_dkey(o);
                    znr[t] = rz_cam(A, B, o, zD[t]);
                }
                if (!znr[t]) { st_rz_nocam++; continue; }
                for (j = rz_s0head[IP_HB(o->sig0)]; j >= 0 && nc < 16; j = rz_s0next[j]) {
                    const ip_op_t *c2 = &A->ops[j];
                    double ex = 1.0;
                    if (c2->sig0 != o->sig0 || !c2->cls || c2->rtv != o->rtv || c2->dsv != o->dsv || c2->topo != o->topo) { continue; }
                    nc++;
                    if (!zc_dmat(Dx, c2->mvp, o->mvp)) { continue; }
                    for (x = 0; x < znr[t]; x++) { double e2 = zc_derr(Dx, zD[t][x]); if (e2 < ex) { ex = e2; } }
                    if (ex < eb) { eb = ex; cand = j; }
                }
                if (!nc || cand < 0 || eb > 1e-3) {
                    /* FIX(W347 GHOST) interp_bbcam: gleiche Billboard-Instanzen (Baeume, gleiche Daten, je Baum eine
                     * zur Kamera gedrehte Matrix) sind beim Schwenk nach Bildsprung mehrdeutig (W91 amb -> nomatch)
                     * und bestehen den Welt-Test nicht -> Partner per Kamera-Schwerpunkt, Matrix-/Vertex-Lerp. */
                    int bj = (g_cfg.interp_bbcam && !g_sba_loc) ? ip_bb_pick(A, o, zD[t], znr[t]) : -1;
                    if (bj < 0) {
                        if (!nc) { st_rz_nmno++; } else { st_rz_nmcam++; }
                        continue;
                    }
                    A->ops[bj].taken = 1;
                    o->match = bj;
                    o->flag = 1;
                    o->why = IP_W_LERP;
                    st_bb_ok++;
                    nnomatch--;
                    nlerp++;
                    continue;
                }
                o->match = cand;
                o->rzb = 1;
                o->flag = 1;
                o->why = IP_W_LERP;
                st_rz_nm++;
                nnomatch--;
                nlerp++;
                continue;
            }
            if (o->why == IP_W_REJECT && g_sba_loc && g_cfg.loc_interp_rejcam && !g_cfg.vr_enable) {
                /* W340 LOC-FRAMES: "reject" = gleiche Eingangsdaten, aber Bezugs-Vertices springen > interp_maxjump
                 * (NDC). Bei 20 Hz und Drehung meist Vertices nahe der Augenebene (Sprung >= 2 NDC, LOCINTERP
                 * reject). Passt M_A^-1*M_B zur Kamerabewegung des Ziels, ruht das Objekt in der Welt -> der
                 * Matrix-Lerp ist genau die Zwischenkamera (linear im Clip-Raum). */
                double er = 1.0;
                if (o->match < 0 || (unsigned)o->match >= A->nops) { continue; }
                a = &A->ops[o->match];
                if (a->kind != IP_DRAW || !a->cls || a->rtv != o->rtv || a->dsv != o->dsv) { continue; }
                for (k = 0; k < nz; k++) { if (zr[k] == o->rtv && zd[k] == rz_dkey(o)) { t = (int)k; break; } }
                if (t < 0) {
                    if (nz >= RZ_T) { continue; }
                    t = (int)nz++;
                    zr[t] = o->rtv; zd[t] = rz_dkey(o);
                    znr[t] = rz_cam(A, B, o, zD[t]);
                }
                if (!znr[t] || !zc_dmat(Dx, a->mvp, o->mvp)) { continue; }
                for (x = 0; x < znr[t]; x++) { double ex = zc_derr(Dx, zD[t][x]); if (ex < er) { er = ex; } }
                if (er > 1e-3 && (g_cfg.loc_interp_dbg & 2) && (g_cfg.pf_ring <= 0 || sb_pf_active()) && st_rz_ln < 1400u) { /* DIAG(W349) */
                    st_rz_ln++;
                    HLOG_I("LOCINTERP rjerr f=%u i=%u n=%u m=%d er=%.5f nD=%d camfit=%.3f", B->frame, i, o->nv, o->match, er, znr[t],
                           li_camfit(a, o, zD[t], znr[t]));
                }
                if (er > 1e-3 && !(g_cfg.loc_interp_rzalt && er <= 0.01 && li_camfit(a, o, zD[t], znr[t]) <= 0.05)) { continue; } /* W349: Geometrie bestaetigt */
                if (er > 1e-3) { li_rj_cam2++; }
                o->flag = 1;
                o->why = IP_W_LERP;
                nreject--;
                nlerp++;
                li_rj_cam++;
                continue;
            }
            if (o->why != IP_W_RAWCHG) { continue; }
            if (o->match < 0 || (unsigned)o->match >= A->nops) { st_rz_other++; continue; }
            a = &A->ops[o->match];
            if (a->kind != IP_DRAW || !a->cls || a->rtv != o->rtv || a->dsv != o->dsv ||
                a->topo != o->topo) { st_rz_other++; continue; }
            for (k = 0; k < nz; k++) { if (zr[k] == o->rtv && zd[k] == rz_dkey(o)) { t = (int)k; break; } }
            if (t < 0) {
                if (nz >= RZ_T) { st_rz_other++; continue; }
                t = (int)nz++;
                zr[t] = o->rtv; zd[t] = rz_dkey(o);
                znr[t] = rz_cam(A, B, o, zD[t]);
            }
            if (!znr[t]) { st_rz_nocam++; continue; }
            /* e0: Objekt ruht in der Welt, nur sein Gitter wurde verschoben (Weltmatrix gleich) -> D = M_A^-1*M_B
             *     passt zur Kamera; richtig ist dann der Matrix-Lerp der Tick-N-Vertices (p_B*M_A -> p_B*M_B).
             * e1: Ursprung verschoben (p_B = p_A + d, Weltmatrix gleicht d aus) -> D = M_A^-1*(T(d)*M_B) passt;
             *     richtig ist der Vertex-Lerp i <-> i (p_A*M_A -> p_B*M_B). */
            if (zc_dmat(Dx, a->mvp, o->mvp)) {
                for (x = 0; x < znr[t]; x++) { double ex = zc_derr(Dx, zD[t][x]); if (ex < e0) { e0 = ex; } }
            }
            if (e0 > 1e-3 && g_sba_loc && g_cfg.loc_interp_rzalt && !g_cfg.vr_enable) {
                /* W349 LOC-FRAMES-2: Mehrpass-Draws gleicher Signatur (LoC-Himmel/Wolken: 3 Durchgaenge mit je 258
                 * Vertices) waehlten alle denselben Vortick-Partner (ip_pick: erster freier ab jp, der rawchg-Partner
                 * wird nicht belegt). Nur der erste passte zur Kamera, die weiteren blieben exakt -> weiche Flecken
                 * im Himmel auf dem Tickstand. Der Matrix-Lerp der Tick-N-Vertices braucht vom Partner nur M_A ->
                 * beste zur Kamera passende Matrix unter den Kandidaten gleicher Signatur ohne Vertexzahl (sig0). */
                int j2, cand = -1, nc = 0;
                double eb = e0;
                if (!s0ok) { s0ok = rz_s0_build(A) ? 1 : -1; }
                for (j2 = s0ok > 0 ? rz_s0head[IP_HB(o->sig0)] : -1; j2 >= 0 && nc < 16; j2 = rz_s0next[j2]) {
                    const ip_op_t *c2 = &A->ops[j2];
                    double ex = 1.0;
                    if (j2 == o->match || c2->sig0 != o->sig0 || !c2->cls || c2->rtv != o->rtv || c2->dsv != o->dsv ||
                        c2->topo != o->topo) { continue; }
                    nc++;
                    if (!zc_dmat(Dx, c2->mvp, o->mvp)) { continue; }
                    for (x = 0; x < znr[t]; x++) { double e2 = zc_derr(Dx, zD[t][x]); if (e2 < ex) { ex = e2; } }
                    if (ex < eb) { eb = ex; cand = j2; }
                }
                if (cand >= 0 && eb <= 1e-3) { o->match = cand; a = &A->ops[cand]; e0 = eb; li_rz_alt++; }
                else if (e0 <= 0.01 && li_camfit_pm((const float (*)[4])o->pref, a->mvp, o, zD[t], znr[t]) <= 0.05) {
                    e0 = 1e-3; /* knapp ueber der Matrix-Schwelle, Geometrie bestaetigt (Rundung grosser Weltkoordinaten) */
                    li_rz_alt++;
                }
            }
            /* Welt-Fall zuerst; der Versatz-Test (zweite Inversion) nur, wenn er scheitert (W284: Zeit je Tick halbiert) */
            ofs = (e0 > 1e-3 || g_cfg.vr_enable) && !a->taken && a->nv == o->nv && rz_offset(a, o, d); /* Vertex-Lerp braucht den Partner exklusiv */
            if (ofs) {
                /* M_B' = T(d) * M_B (Zeilenvektoren): Zeilen 0..2 wie M_B, Zeile 3 = d*M_B[0..2] + M_B[3] */
                for (c = 0; c < 16; c++) { MBf[c] = o->mvp[c]; }
                for (c = 0; c < 4; c++) {
                    MBf[12 + c] = (float)((double)d[0] * o->mvp[c] + (double)d[1] * o->mvp[4 + c] + (double)d[2] * o->mvp[8 + c] + o->mvp[12 + c]);
                }
                if (zc_dmat(Dx, a->mvp, MBf)) {
                    for (x = 0; x < znr[t]; x++) { double ex = zc_derr(Dx, zD[t][x]); if (ex < e) { e = ex; } }
                }
            }
            if (g_cfg.interp_diag >= 1 && (g_cfg.pf_ring <= 0 || sb_pf_active()) &&
                (g_cfg.interp_ddrel > 0 ? (sb_pf_m0() && B->frame >= sb_pf_m0() + (unsigned)g_cfg.interp_ddrel &&
                                           B->frame < sb_pf_m0() + (unsigned)g_cfg.interp_ddrel + 3u)
                                        : st_rz_ln < 400u)) { /* DIAG(W284) */
                st_rz_ln++;
                HLOG_I("INTERP rzd f=%u i=%u n=%u/%u m=%d versatz=%d d=%.3f,%.3f,%.3f fehler_welt=%.6f fehler_versatz=%.6f", B->frame, i,
                       o->nv, a->nv, o->match, ofs, (double)d[0], (double)d[1], (double)d[2], e0, e);
            }
            if (e0 <= 1e-3 && e0 <= e && !g_cfg.vr_enable) {
                o->rzb = 1; /* Matrix-Lerp der Tick-N-Vertices */
                st_rz_relw++;
            } else if (e <= 1e-3 && ofs) {
                st_rz_rel++;
            } else {
                /* FIX(W347 GHOST) interp_bbcam: Billboards (Baeume: n=6/12, je Tick neu zur Kamera gedreht) bestehen
                 * weder den Welt- noch den Versatz-Test und blieben exakt (Tick-N-Lage in allen Zwischenbildern) ->
                 * beim Drehen stehen die Baeume, der Rest gleitet = Doppelkanten. Partner = Vortick-Draw, dessen
                 * Drehpunkt (Ursprung der Matrix) mit der Kamera des Ziels auf den von o faellt; dann Vertex-Lerp. */
                int bj = (g_cfg.interp_bbcam && !g_sba_loc && !g_cfg.vr_enable) ? ip_bb_pick(A, o, zD[t], znr[t]) : -1;
                int vl = -1;
                if (g_sba_loc && g_cfg.loc_interp_rzalt && !g_cfg.vr_enable) {
                    /* W349 LOC-FRAMES-2: Vertex-Lerp i <-> i mit dem Kandidaten gleicher Signatur (gleiche Vertexzahl),
                     * dessen Bezugs-Vertices der Kamera folgen (li_camfit <= 0,25 der Objektgroesse): neu gebaute
                     * Billboards (Wolken/Glow-Sprites des Himmels, 6 Vertices, je Tick neu um die Kamera gedreht) und
                     * mitwandernde Himmelsgitter. Ohne Kamera-Bezug des Ziels kein Lerp. */
                    unsigned h2 = IP_HB(o->sig);
                    int j3, nc3 = 0;
                    double eb3 = 0.25;
                    for (j3 = ip_hhead[h2]; j3 >= 0 && nc3 < 64; j3 = ip_hnext[j3]) {
                        const ip_op_t *c3 = &A->ops[j3];
                        double ex3;
                        if (c3->sig != o->sig || c3->nv != o->nv || c3->kind != IP_DRAW || c3->rtv != o->rtv) { continue; }
                        nc3++;
                        ex3 = li_camfit(c3, o, zD[t], znr[t]);
                        if (c3->taken) { ex3 += 0.05; } /* freie Partner bevorzugt */
                        if (ex3 < eb3) { eb3 = ex3; vl = j3; }
                    }
                    if ((g_cfg.loc_interp_dbg & 2) && (g_cfg.pf_ring <= 0 || sb_pf_active()) && st_rz_ln < 1400u) { /* DIAG(W349) */
                        st_rz_ln++;
                        HLOG_I("LOCINTERP rzv f=%u i=%u n=%u m=%d e0=%.4f kand=%d best=%d fehler=%.3f", B->frame, i, o->nv,
                               o->match, e0, nc3, vl, vl >= 0 ? eb3 : -1.0);
                    }
                }
                if (bj < 0 && vl < 0) {
                    if (!ofs) { st_rz_noofs++; }
                    st_rz_cam++;
                    continue;
                }
                if (bj >= 0) {
                    a = &A->ops[bj];
                    o->match = bj;
                    st_bb_ok++;
                } else {
                    o->match = vl;
                    a = &A->ops[vl];
                    o->rzb = 0; /* Vertex-Lerp i <-> i */
                    li_rz_vl++;
                }
            }
            a->taken = 1;
            o->flag = 1;
            o->why = IP_W_RAWLERP;
            nrawchg--;
            nlerp++;
            nrawlerp++;
        }
        {
            double ms = ip_ms(rzq0, ip_now());
            st_rz_ms += ms; st_rz_n++;
            if (ms > st_rz_max) { st_rz_max = ms; }
        }
    }
    if (g_cfg.interp_pass) {
        /* POPIN (W91): Mehrpass-Konsistenz. Durchgaenge derselben Instanz
         * (gleiches Ziel, gleiche Geometrie = gkey, gleiche Matrix) muessen
         * gleich behandelt werden: ist einer exakt (ohne Partner, neue
         * Eingangsdaten, unplausibel), wird ein gelerpter zweiter Durchgang
         * gegen ihn verschoben gezeichnet -> Z-Fighting-Streifen/Aufblitzen
         * in Zwischenbildern. Dann alle Durchgaenge der Instanz exakt. */
        /* FIX(W273 INTERP-RACE) interp_pass_exact=1: Die alte 16-Bit-Bitmaske (ip_mvpkey) kollidiert
         * (~600 exakte Eintraege je Tick in 65536 Bit -> ~1 % der gelerpten Draws faelschlich exakt),
         * und weil gkey den Host-Zeiger der RTV enthaelt, trifft es je Lauf andere Draws: exakte Draws
         * zwischen gelerpten (Tiefenmischung W251) = helle Keile, die in einem Lauf erscheinen und im
         * anderen nicht (W265-Streuung). Jetzt Eimer per Schluessel, Mitglied nur bei voller Gleichheit
         * (Ziel, Geometrie, Vertexzahl, Matrix) -> zeigerunabhaengig und ohne Fehltreffer. */
        static unsigned char ex[1u << 13]; /* 65536 Bit */
        static int ph[1u << 16];
        static int *pn;
        static unsigned pcap;
        unsigned npass = 0, nfalse = 0;
        int exact = g_cfg.interp_pass_exact > 0;
        if (exact && pcap < B->nops) {
            int *nn = (int *)realloc(pn, B->nops * sizeof *nn);
            if (nn) { pn = nn; pcap = B->nops; } else { exact = 0; }
        }
        memset(ex, 0, sizeof ex);
        if (exact) { memset(ph, 0xFF, sizeof ph); }
        for (i = 0; i < B->nops; i++) {
            const ip_op_t *o = &B->ops[i];
            unsigned k;
            if (o->kind != IP_DRAW || !o->cls) { continue; }
            if (o->why != IP_W_NOMATCH && o->why != IP_W_RAWCHG && o->why != IP_W_REJECT) { continue; }
            k = ip_mvpkey(o);
            ex[k >> 3] |= (unsigned char)(1u << (k & 7u));
            if (exact) { pn[i] = ph[k]; ph[k] = (int)i; }
        }
        for (i = 0; i < B->nops; i++) {
            ip_op_t *o = &B->ops[i];
            unsigned k;
            int hit;
            if (o->kind != IP_DRAW || !o->cls || !o->flag) { continue; }
            k = ip_mvpkey(o);
            hit = (ex[k >> 3] & (1u << (k & 7u))) != 0;
            if (exact && hit) {
                int j, same = 0;
                for (j = ph[k]; j >= 0; j = pn[j]) {
                    const ip_op_t *e = &B->ops[j];
                    if (e->gkey == o->gkey && e->rtv == o->rtv && e->rawhash == o->rawhash && e->nv == o->nv &&
                        memcmp(e->mvp, o->mvp, sizeof o->mvp) == 0 &&
                        /* FIX(W429): P2 (vorab transformiert) traegt nur die Bildraum-Matrix - gleiche Matrix heisst dort
                         * NICHT gleiche Instanz (dasselbe Gelaende-Netz an anderer Stelle). Instanz = gleiche Bildlage. */
                        !(g_cfg.interp_p2hom && !g_sba_loc && ip_mvp_affine(o->mvp) && memcmp(e->pref, o->pref, sizeof o->pref) != 0)) {
                        same = 1; o->exj = j; break;
                    }
                }
                if (!same) { nfalse++; hit = 0; }
            }
            if (hit) { o->flag = 0; npass++; o->exr = 1; }
        }
        st_ipass += npass;
        st_ipass_false += nfalse;
        if (npass <= nlerp) { nlerp -= npass; }
    }
    if (g_cfg.interp_zcons == 1) {
        /* FIX(W251 INTERP-GROUND): Tiefen-Konsistenz. Exakte (Tick N) und gelerpte Welt-Draws im
         * selben Tiefenpuffer sind nicht vergleichbar - Beleg W251_d1 (M1-Hangar, shading=0): der
         * Hangarboden ist exakt (VS-Signatur wechselt, Bezugspunkte an der Augenebene), die spaeter
         * gezeichneten Gelaendestreifen darunter sind gelerpt und gewinnen den Tiefentest ->
         * Bodentextur ueber dem halben Hauptmonitor; umgekehrt (W251_d2) steht eine exakte Wand
         * (reject, 300 px Sprung beim Schwenk) als heller Splitter vor dem gelerpten Boden.
         * Darum: ein exakter Welt-Draw (nomatch/reject) mit einem Vortick-Draw derselben
         * Objektraum-Vertices im selben Ziel wird per Matrix gelerpt, wenn dessen Matrix-Aenderung
         * zur Kamerabewegung passt. Fuer ein ruhendes Objekt gilt D = M_A^-1 * M_B = VP_A^-1 * VP_B
         * (die Weltmatrix kuerzt sich heraus); verglichen wird mit D der 8 groessten plausibel
         * gelerpten Draws des Ziels (ein Treffer genuegt). Falsche Instanz oder bewegtes Objekt
         * passen zu keinem; unter mehreren Instanzen gewinnt die mit der kleinsten Abweichung.
         * Clip-Lerp gleicher Vertices = Transformation mit der Zwischenmatrix: dieselbe
         * Zwischenkamera wie das uebrige Bild. */
        enum { ZC_T = 8, ZC_R = 8 };
        ID3D11RenderTargetView *zr[ZC_T];
        ID3D11DepthStencilView *zd[ZC_T];
        double zD[ZC_T][ZC_R][16];
        int znr[ZC_T];
        unsigned nz = 0, k;
        for (i = 0; i < B->nops; i++) {
            ip_op_t *o = &B->ops[i];
            int t = -1, j, cand = -1, nc = 0;
            double ebest = 1.0;
            unsigned q;
            if (o->kind != IP_DRAW || !o->cls || o->flag || !o->dsv) { continue; }
            if (o->why != IP_W_NOMATCH && o->why != IP_W_REJECT) { continue; }
            for (k = 0; k < nz; k++) { if (zr[k] == o->rtv && zd[k] == o->dsv) { t = (int)k; break; } }
            if (t < 0) { /* Kamerabewegungen dieses Ziels einmal je Tick */
                int rr[ZC_R], x;
                if (nz >= ZC_T) { continue; }
                t = (int)nz++;
                zr[t] = o->rtv; zd[t] = o->dsv; znr[t] = 0;
                for (x = 0; x < ZC_R; x++) { rr[x] = -1; }
                for (q = 0; q < B->nops; q++) {
                    const ip_op_t *r = &B->ops[q];
                    int w;
                    if (r->kind != IP_DRAW || r->why != IP_W_LERP || !r->flag || r->rtv != o->rtv || r->dsv != o->dsv ||
                        r->match < 0 || (unsigned)r->match >= A->nops) { continue; }
                    if (!zc_cam_src(&A->ops[r->match], r)) { continue; } /* W294 Bit 1 */
                    for (w = 0; w < ZC_R; w++) {
                        if (rr[w] < 0 || r->nv > B->ops[rr[w]].nv) {
                            int z;
                            for (z = ZC_R - 1; z > w; z--) { rr[z] = rr[z - 1]; }
                            rr[w] = (int)q;
                            break;
                        }
                    }
                }
                for (x = 0; x < ZC_R && rr[x] >= 0; x++) {
                    if (zc_dmat(zD[t][znr[t]], A->ops[B->ops[rr[x]].match].mvp, B->ops[rr[x]].mvp)) { znr[t]++; }
                }
                if (znr[t]) { st_zc_tgt++; }
            }
            if (!znr[t]) { continue; }
            for (j = ip_ghead[IP_HB(o->gkey)]; j >= 0 && nc < 32; j = ip_gnext[j]) {
                const ip_op_t *a = &A->ops[j];
                double Dx[16], e = 1.0;
                int x;
                if (a->kind != IP_DRAW || (a->taken && j != o->match) || a->gkey != o->gkey || a->rtv != o->rtv ||
                    a->dsv != o->dsv || a->nv != o->nv || a->rawhash != o->rawhash || !a->cls) { continue; }
                nc++;
                if (memcmp(a->mvp, o->mvp, sizeof o->mvp) == 0 || !zc_dmat(Dx, a->mvp, o->mvp)) { continue; }
                for (x = 0; x < znr[t]; x++) { double ex = zc_derr(Dx, zD[t][x]); if (ex < e) { e = ex; } }
                if (e < ebest) { ebest = e; cand = j; }
            }
            if (o->nv >= 1000u && g_cfg.interp_diag >= 1 && st_zc_ln < 400u && (g_cfg.pf_ring <= 0 || sb_pf_active())) { /* DIAG(W251) */
                st_zc_ln++;
                HLOG_I("INTERP zcd f=%u i=%u n=%u why=%u nc=%d cand=%d kamera_fehler=%.6f", B->frame, i, o->nv, o->why, nc, cand, ebest);
            }
            if (!nc) { st_zc_nocand++; continue; }
            if (cand < 0 || ebest > 1e-3) { st_zc_cam++; continue; }
            A->ops[cand].taken = 1;
            o->match = cand;
            o->flag = 1;
            o->why = IP_W_LERP;
            st_zc_rel++;
            nlerp++;
        }
    } else if (g_cfg.interp_zcons == 2) {
        /* Versuch (W251): Ziel mit grossem exaktem Welt-Draw ganz exakt - Hauptmonitor dann kaum
         * noch interpoliert (W251_z0: 97 % der Zwischenbilder wie Tick N), nicht Standard. */
        ID3D11RenderTargetView *zr[8];
        ID3D11DepthStencilView *zd[8];
        unsigned nz = 0, k, nzd = 0;
        for (i = 0; i < B->nops && nz < 8u; i++) {
            const ip_op_t *o = &B->ops[i];
            int q, cross = 0, nf = 0, dup = 0;
            float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
            if (o->kind != IP_DRAW || !o->cls || o->flag || !o->dsv) { continue; }
            if (o->why != IP_W_NOMATCH && o->why != IP_W_RAWCHG && o->why != IP_W_REJECT) { continue; }
            for (q = 0; q < 4; q++) {
                if (o->rn[q][2] <= 1e-6f) { cross = 1; continue; }
                nf++;
                if (o->rn[q][0] < x0) { x0 = o->rn[q][0]; }
                if (o->rn[q][0] > x1) { x1 = o->rn[q][0]; }
                if (o->rn[q][1] < y0) { y0 = o->rn[q][1]; }
                if (o->rn[q][1] > y1) { y1 = o->rn[q][1]; }
            }
            if (!((cross && nf > 0) || (nf > 1 && (x1 - x0 > 1.0f || y1 - y0 > 1.0f)))) { continue; }
            for (k = 0; k < nz; k++) { if (zr[k] == o->rtv && zd[k] == o->dsv) { dup = 1; } }
            if (!dup) { zr[nz] = o->rtv; zd[nz] = o->dsv; nz++; }
        }
        for (i = 0; nz && i < B->nops; i++) {
            ip_op_t *o = &B->ops[i];
            if (o->kind != IP_DRAW || !o->cls || !o->flag) { continue; }
            for (k = 0; k < nz; k++) {
                if (zr[k] == o->rtv && zd[k] == o->dsv) { o->flag = 0; nzd++; break; }
            }
        }
        if (nz) { st_zc_ticks++; st_zc_draws += nzd; }
        if (nzd <= nlerp) { nlerp -= nzd; }
    }
    if (g_cfg.interp_pair || g_cfg.interp_pair_diag) { ip_pair_robust(A, B); } /* W317 INTERP-PAIR: nach W284/W294/W91/W251 (deren Ergebnis = alte Zuordnung) */
    for (i = 0; i < B->nops; i++) { /* W264 DRAW-OPT2: Lerp nur zwischen gleich abgelegten Draws (beide Liste oder beide indiziert) */
        ip_op_t *o = &B->ops[i];
        if (o->kind == IP_DRAW && o->flag && o->match >= 0 && (unsigned)o->match < A->nops &&
            A->ops[o->match].ib1 != o->ib1 && !(o->rzb && (g_cfg.interp_streak & 4))) { o->flag = 0; o->exr = 2; if (nlerp) { nlerp--; } } /* W294: rzb liest nur B */
    }
    if (g_cfg.interp_eyediag > 0) { /* DIAG(W237): Zuordnungs-Ergebnis der Welt-Draws (cls=1) */
        for (i = 0; i < B->nops; i++) {
            const ip_op_t *o = &B->ops[i];
            if (o->kind == IP_DRAW && o->cls) { st_wwhy[o->why < IP_W_N ? o->why : 0]++; }
        }
    }
    if (g_cfg.interp_eyediag > 0) {
        /* DIAG(W224 LINE-FLICKER): zwischengerechnete Draws, in denen einzelne
         * Vertices zwischen Tick N-1 und N die Augenebene kreuzen (w-Wechsel).
         * Clip-Lerp laeuft dort durch w=0 -> Projektion ins Unendliche. */
        const float *WA = ip_cw[(int)(A - g_ipl)], *WB = ip_cw[(int)(B - g_ipl)];
        for (i = 0; WA && WB && i < B->nops; i++) {
            const ip_op_t *o = &B->ops[i];
            const ip_op_t *a;
            unsigned k, nx = 0, ref = 0;
            float wn = 0.0f, wa = 0.0f;
            if (o->kind != IP_DRAW || !o->flag || o->match < 0) { continue; }
            a = &A->ops[o->match];
            if (a->nv != o->nv) { continue; }
            if (a->vstart + a->nv > ip_cwcap[(int)(A - g_ipl)] ||
                o->vstart + o->nv > ip_cwcap[(int)(B - g_ipl)]) { continue; }
            {   /* W237: P2-Vertices mit echtem w-Wechsel (Strom w = 1) und Sprung-Kriterium je Vertex */
                const float (*JA)[2] = ip_cj[(int)(A - g_ipl)], (*JB)[2] = ip_cj[(int)(B - g_ipl)];
                const unsigned char *PB2 = ip_cp2[(int)(B - g_ipl)];
                unsigned np2 = 0, nhit = 0, njmp = 0, kref = 0, nfront = 0;
                float jw0 = 0, jw1 = 0, jx0 = 0, jx1 = 0;
                for (k = 0; k < o->nv; k++) {
                    float w0 = WA[a->vstart + k], w1 = WB[o->vstart + k];
                    const float *n0 = JA[a->vstart + k], *n1 = JB[o->vstart + k];
                    float dx = fabsf(n1[0] - n0[0]), dy = fabsf(n1[1] - n0[1]);
                    float mx = 1.0f + fmaxf(fabsf(n0[0]), fabsf(n1[0])), my = 1.0f + fmaxf(fabsf(n0[1]), fabsf(n1[1]));
                    int jmp = (dx > 0.5f * mx || dy > 0.5f * my);
                    int p2 = PB2[o->vstart + k] && w0 * w1 <= 0.0f;
                    if (jmp) {
                        njmp++;
                        if (w0 > 0.0f && w1 > 0.0f) { st_jf[o->why < IP_W_N ? o->why : 0]++; nfront++; }
                        else if (w0 < 0.0f && w1 < 0.0f) { st_jb[o->why < IP_W_N ? o->why : 0]++; }
                    }
                    if (p2) {
                        if (!np2) { kref = k; jw0 = w0; jw1 = w1; jx0 = n0[0]; jx1 = n1[0]; }
                        np2++;
                        if (jmp) { nhit++; }
                    }
                }
                st_jmp_verts += njmp;
                if (nfront) { st_jfd[o->why < IP_W_N ? o->why : 0]++; }
                st_jmp_fp += njmp - nhit;
                if (np2) {
                    st_p2x_draws++;
                    st_p2x_verts += np2;
                    st_p2x_hit += nhit;
                    if (st_p2x_ln < (unsigned)g_cfg.interp_eyediag && (g_cfg.pf_ring <= 0 || sb_pf_active())) { /* mit pf_ring nur in den Bildfolgen */
                        st_p2x_ln++;
                        HLOG_I("INTERP p2x f=%u i=%u why=%u n=%u nx=%u hit=%u jmp=%u k=%u w=%.3f->%.3f x=%.2f->%.2f vs=%X",
                               B->frame, i, o->why, o->nv, np2, nhit, njmp, kref, jw0, jw1, jx0, jx1, o->pvs);
                    }
                }
            }
            for (k = 0; k < o->nv; k++) {
                float w0 = WA[a->vstart + k], w1 = WB[o->vstart + k];
                if (w0 * w1 <= 0.0f) {
                    if (!nx) { wa = w0; wn = w1; ref = k; }
                    nx++;
                }
            }
            if (!nx) { continue; }
            st_eyex_draws++;
            st_eyex_verts += nx;
            if (st_eyex_ln < (unsigned)g_cfg.interp_eyediag) {
                st_eyex_ln++;
                HLOG_I("INTERP eyex f=%u i=%u why=%s cls=%u rt=%d n=%u nx=%u k=%u w=%.3f->%.3f topo=%u vs=%X",
                       B->frame, i, o->why == IP_W_LERP ? "lerp" : o->why == IP_W_RAWLERP ? "rawlerp" :
                       o->why == IP_W_RT2DLERP ? "rt2dlerp" : "?", o->cls, o->rtv != g_rtv, o->nv, nx,
                       ref, wa, wn, o->topo, o->pvs);
            }
        }
    }
    if ((g_cfg.loc_interp_dbg & 2) && g_sba_loc && (g_cfg.pf_ring <= 0 || sb_pf_active())) { /* DIAG(W349): exakte Welt-Draws */
        static unsigned nln;
        for (i = 0; i < B->nops && nln < 1500u; i++) {
            const ip_op_t *o = &B->ops[i];
            const ip_op_t *a = (o->match >= 0 && (unsigned)o->match < A->nops) ? &A->ops[o->match] : NULL;
            if (o->kind != IP_DRAW || !o->cls || o->flag || o->why == IP_W_STATIC) { continue; }
            nln++;
            HLOG_I("LOCINTERP ex f=%u i=%u why=%s bb=%d n=%u/%d topo=%u vs=%X xp=%X ps=%X raw=%08X m=%d "
                   "ndc=%.2f,%.2f w=%.1f / %.2f,%.2f w=%.1f a=%.2f,%.2f w=%.1f / %.2f,%.2f w=%.1f",
                   B->frame, i, pr_why(o->why), o->rtv == g_rtv, o->nv, a ? (int)a->nv : -1, o->topo, o->pvs, o->pxp,
                   o->pps, o->rawhash, o->match, o->rn[0][0], o->rn[0][1], o->rn[0][2], o->rn[3][0], o->rn[3][1], o->rn[3][2],
                   a ? a->rn[0][0] : 0.f, a ? a->rn[0][1] : 0.f, a ? a->rn[0][2] : 0.f, a ? a->rn[3][0] : 0.f,
                   a ? a->rn[3][1] : 0.f, a ? a->rn[3][2] : 0.f);
        }
    }
    st_lerp += nlerp; st_static += nstatic; st_nomatch += nnomatch;
    st_rawchg += nrawchg; st_reject += nreject; st_2d += n2d; st_rawlerp += nrawlerp;
    st_rt2d += nrt2d;
    st_geo += ngeo;
    if (log) {
        HLOG_I("INTERP pair f=%u ops=%u draws=%u clears=%u bb=%u 2d=%u world=%u matched=%u inorder=%u lerp=%u (rawlerp=%u rt2d=%u) static=%u nomatch=%u rawchg=%u reject=%u verts=%u/%u",
               B->frame, B->nops, ndraw, nclr, bbdraws, n2d, nworld, nmatch, inorder,
               nlerp, nrawlerp, nrt2d, nstatic, nnomatch, nrawchg, nreject, A->nverts, B->nverts);
    }
    /* Schnitt (Kamerasprung): viele unplausible Spruenge -> Paar exakt. */
    if (nreject > 4u && nreject * 4u > nlerp + nreject) {
        st_cut++;
        for (i = 0; i < B->nops; i++) { B->ops[i].flag = 0; }
        if (log || st_cut <= 8u) {
            HLOG_I("INTERP cut f=%u reject=%u lerp=%u (Paar exakt)", B->frame, nreject, nlerp);
        }
        return 0;
    }
    return 1;
}

/* Diagnose: Draw-Liste eines Ticks (interp_diag=2, erste volle Ticks). */
void ip_dump(ip_list_t *B)
{
    unsigned i;
    for (i = 0; i < B->nops; i++) {
        const ip_op_t *o = &B->ops[i];
        if (o->kind != IP_DRAW) {
            HLOG_I("INTERP op f=%u i=%u %s rtv=%p dsv=%p", B->frame, i,
                   o->kind == IP_CLR_RTV ? "clr-rt" : o->kind == IP_COPY ? "copy" : "clr-ds", (void *)o->rtv, (void *)o->dsv);
            continue;
        }
        HLOG_I("INTERP op f=%u i=%u draw rtv=%p%s cls=%u n=%u topo=%u tex=%u sig=%08X raw=%08X out=%08X m=%d fl=%u mvp0=%.4f,%.4f,%.4f,%.4f",
               B->frame, i, (void *)o->rtv, o->rtv == g_rtv ? "(BB)" : "", o->cls, o->nv, o->topo,
               o->has_tex, o->sig, o->rawhash, o->outhash, o->match, o->flag,
               o->mvp[0], o->mvp[5], o->mvp[12], o->mvp[13]);
    }
}

/* ---------- Schatten-Ziele + Sicherung je Liste ---------- */

long long isw_cp_q, isw_un_q, isw_gr_q; unsigned isw_cp_n, isw_gr_n; /* DIAG(LOC-IM-LOCK): CopyResource / unref / ip_sh_ref je Swap */
volatile LONG st_sh_stale, st_sh_stale_tot; /* W361: Wiedergaben mit ersetztem Eintrag (je Fenster / gesamt) */
