/* loc_ret_mark.h - W444 LOC-RETICLE-MARK (GitHub #78, Ursache 2): Fadenkreuz-Teile robust erkennen (reine Rechnung, ohne
 * D3D/Spielspeicher; Selbsttest render_selftest "RETMARK444").
 *
 * Befund W444 (DIAG loc_ret_diag Bit 4, Lauf A0): Das Fadenkreuz sind 7 untexturierte Linien-Draws einer Gruppe (gleiche
 * Signatur ohne Vertexzahl): Ring (40 Vertices) und Kreuz (8, 4 x 4, 24). Nur der Ring folgt der Formel-Mitte (A9); das
 * Kreuz bewegt sich beim Drehen eigenstaendig (bis 0,34 NDC neben der Formel-Mitte, je Tick bis 0,06 anders), und auch der
 * Ring weicht in y ab. Die Bewegungs-Pruefung gegen die Formel (W376/W439) verwarf deshalb Teile. Dazu kommt ein fremder
 * Draw derselben Gruppe (8 Vertices, andere Form, ruhend), der zwischen Ring und Kreuz gezeichnet werden kann.
 *
 * Erkennung: (1) Identitaet ueber die Form: ein Kandidat gleicher Gruppe und Vertexzahl mit gleicher Form (Bezugs-Vertices
 * relativ zum ersten, +-LRM_SHAPE) wie ein Fadenkreuz-Teil der Vorlage ist dasselbe Teil; jedes Vorlagen-Teil hoechstens
 * einmal, zuerst eindeutige Paare, dann nach Bewegung (passend zu einem schon erkannten Teil, die Teile bewegen sich in
 * starren Gruppen). (2) Teile, deren Form sich aendert (Einblend-Animation): Partner der Zuordnung, wenn die Bewegung zu
 * einem erkannten Teil passt. (3) Kandidaten der alten Pruefung (Kasten + Formel-Bewegung) wie bisher, ausser ein weiterer
 * Draw eines schon vergebenen Teil-Typs (Gruppe + Vertexzahl) = fremd.
 * Vorlage = Fadenkreuz-Teile der Vorliste, ergaenzt um Teile aus dem Gedaechtnis (zuletzt erkannte Teile, hoechstens
 * LRM_MEMT Ticks alt), die in der Vorliste fehlten. */
#pragma once
#include <math.h>

#define LRM_MAXP 16       /* Fadenkreuz-Teile je Liste (gemessen 7) */
#define LRM_MAXC 64       /* Kandidaten je Liste */
#define LRM_SHAPE 0.004f  /* Formgleichheit je Koordinate (NDC); 2D-Teile ohne Drehung sind bitgleich */
#define LRM_WIN 0.75f     /* Fenster um die Formel-Mitte (gemessen max 0,46) */
#define LRM_MAXD 0.5f     /* groesste Bewegung je Tick fuer die Form-Identitaet */
#define LRM_PAIRD 0.08f   /* Partner mit Formwechsel: Abweichung zur Bewegung eines erkannten Teils */
#define LRM_MEMT 40u      /* Gedaechtnis: hoechstes Alter in Ticks */

typedef struct {
    float p[4][2];   /* Bezugs-Vertices (NDC x/y: erster, 1/3, 2/3, letzter) */
    unsigned nv;     /* Vertexzahl */
    unsigned grp;    /* Gruppenmerkmal (Signatur ohne Vertexzahl) */
} lrm_part_t;

typedef struct {
    lrm_part_t d;
    int prev;            /* Index des Zuordnungs-Partners in der Vorlage (prev[]), -1 = keiner */
    unsigned char seed;  /* besteht die alte Pruefung (Kasten um die Formel-Mitte + Bewegung wie die Formel) */
} lrm_cand_t;

typedef struct { lrm_part_t d; unsigned ser; } lrm_mem_t;

enum { LRM_NO = 0, LRM_SEED = 1, LRM_FORM = 2, LRM_PAIR = 3 };

static int lrm_same_shape(const lrm_part_t *a, const lrm_part_t *b)
{
    int k;
    if (a->nv != b->nv || a->grp != b->grp) { return 0; }
    for (k = 1; k < 4; k++) {
        if (fabs((a->p[k][0] - a->p[0][0]) - (b->p[k][0] - b->p[0][0])) > LRM_SHAPE ||
            fabs((a->p[k][1] - a->p[0][1]) - (b->p[k][1] - b->p[0][1])) > LRM_SHAPE) { return 0; }
    }
    return 1;
}

static int lrm_in_win(const lrm_part_t *p, const float rc[2])
{
    return fabs(p->p[0][0] - rc[0]) <= LRM_WIN && fabs(p->p[0][1] - rc[1]) <= LRM_WIN;
}

/* prev: Vorlage (np, 0 = keine), c: Kandidaten (nc), rc: Formel-Mitte dieser Liste.
 * Ausgabe why[i] (LRM_*), Rueckgabe Anzahl markierter Teile. */
static int lrm_mark(const lrm_part_t *prev, int np, const lrm_cand_t *c, int nc, const float rc[2], unsigned char *why)
{
    unsigned char used[LRM_MAXP], ci[LRM_MAXC], cj[LRM_MAXP];
    float mvx[LRM_MAXC + LRM_MAXP], mvy[LRM_MAXC + LRM_MAXP];
    int i, j, n = 0, nmv = 0;
    if (np > LRM_MAXP) { np = LRM_MAXP; }
    if (nc > LRM_MAXC) { nc = LRM_MAXC; }
    for (j = 0; j < np; j++) { used[j] = 0; cj[j] = 0; }
    for (i = 0; i < nc; i++) { why[i] = LRM_NO; ci[i] = 0; }
    /* (1a) eindeutige Form-Paare (Kandidat und Vorlagen-Teil haben je genau einen Formgleichen) */
    for (i = 0; i < nc; i++) {
        if (!lrm_in_win(&c[i].d, rc)) { continue; }
        for (j = 0; j < np; j++) {
            if (lrm_same_shape(&c[i].d, &prev[j])) { if (ci[i] < 255) { ci[i]++; } if (cj[j] < 255) { cj[j]++; } }
        }
    }
    for (i = 0; i < nc; i++) {
        if (ci[i] != 1) { continue; }
        for (j = 0; j < np; j++) {
            float dx, dy;
            if (cj[j] != 1 || used[j] || !lrm_same_shape(&c[i].d, &prev[j])) { continue; }
            dx = c[i].d.p[0][0] - prev[j].p[0][0];
            dy = c[i].d.p[0][1] - prev[j].p[0][1];
            if (fabs(dx) > LRM_MAXD || fabs(dy) > LRM_MAXD) { continue; }
            why[i] = LRM_FORM; used[j] = 1; mvx[nmv] = dx; mvy[nmv] = dy; nmv++; n++;
            break;
        }
    }
    for (;;) { /* (1b) uebrige Form-Paare: kleinste Abweichung zur Bewegung eines erkannten Teils (sonst kleinste Bewegung) */
        int bi = -1, bj = -1, k;
        float bc = 1e9f, bdx = 0.0f, bdy = 0.0f;
        for (i = 0; i < nc; i++) {
            if (why[i] || !ci[i]) { continue; }
            for (j = 0; j < np; j++) {
                float dx, dy, cst;
                if (used[j] || !lrm_same_shape(&c[i].d, &prev[j])) { continue; }
                dx = c[i].d.p[0][0] - prev[j].p[0][0];
                dy = c[i].d.p[0][1] - prev[j].p[0][1];
                if (fabs(dx) > LRM_MAXD || fabs(dy) > LRM_MAXD) { continue; }
                cst = (float)sqrt(dx * dx + dy * dy);
                for (k = 0; k < nmv; k++) {
                    float ex = dx - mvx[k], ey = dy - mvy[k], e = (float)sqrt(ex * ex + ey * ey);
                    if (k == 0 || e < cst) { cst = e; }
                }
                if (cst < bc) { bc = cst; bi = i; bj = j; bdx = dx; bdy = dy; }
            }
        }
        if (bi < 0) { break; }
        why[bi] = LRM_FORM; used[bj] = 1; mvx[nmv] = bdx; mvy[nmv] = bdy; nmv++; n++;
    }
    for (i = 0; i < nc; i++) { /* (2) Partner mit Formwechsel, Bewegung passt zu einem erkannten Teil */
        const lrm_part_t *a;
        float dx, dy;
        int k, ok = 0;
        j = c[i].prev;
        if (why[i] || j < 0 || j >= np || used[j]) { continue; }
        a = &prev[j];
        if (a->nv != c[i].d.nv || a->grp != c[i].d.grp || !lrm_in_win(&c[i].d, rc)) { continue; }
        dx = c[i].d.p[0][0] - a->p[0][0];
        dy = c[i].d.p[0][1] - a->p[0][1];
        if (!nmv) { ok = fabs(dx) <= LRM_PAIRD && fabs(dy) <= LRM_PAIRD; }
        for (k = 0; k < nmv && !ok; k++) { ok = fabs(dx - mvx[k]) <= LRM_PAIRD && fabs(dy - mvy[k]) <= LRM_PAIRD; }
        if (!ok) { continue; }
        why[i] = LRM_PAIR; used[j] = 1; mvx[nmv] = dx; mvy[nmv] = dy; nmv++; n++;
    }
    for (i = 0; i < nc; i++) { /* (3) alte Pruefung; mit Vorlage nur deren Gruppe, weiterer Draw eines vergebenen Typs = fremd */
        int same_type = 0, free_type = 0, same_grp = 0;
        if (why[i] || !c[i].seed) { continue; }
        for (j = 0; j < np; j++) {
            if (prev[j].grp != c[i].d.grp) { continue; }
            same_grp = 1;
            if (prev[j].nv != c[i].d.nv) { continue; }
            same_type = 1;
            if (!used[j]) { free_type = 1; }
        }
        if (np > 0 && (!same_grp || (same_type && !free_type))) { continue; }
        why[i] = LRM_SEED;
        n++;
    }
    return n;
}

/* Vorlage ergaenzen: Gedaechtnis-Teile (Alter <= LRM_MEMT), fuer die die Vorlage kein formgleiches Teil hat (Zaehlung je
 * Form, gleiche Teile duerfen mehrfach vorkommen). src[] der neuen Teile = -1. Rueckgabe neue Anzahl. */
static int lrm_prev_add_mem(lrm_part_t *prev, int *src, int np, const lrm_mem_t *mem, int nmem, unsigned ser)
{
    unsigned char used[LRM_MAXP];
    int i, j, n0 = np;
    for (j = 0; j < LRM_MAXP; j++) { used[j] = 0; }
    for (i = 0; i < nmem && np < LRM_MAXP; i++) {
        int hit = 0;
        if (ser - mem[i].ser > LRM_MEMT) { continue; }
        for (j = 0; j < n0 && !hit; j++) {
            if (!used[j] && lrm_same_shape(&mem[i].d, &prev[j])) { used[j] = 1; hit = 1; }
        }
        if (hit) { continue; }
        prev[np] = mem[i].d;
        if (src) { src[np] = -1; }
        np++;
    }
    return np;
}

/* Gedaechtnis nach der Markierung: markierte Teile (Alter 0) + alte Eintraege ohne formgleiches markiertes Teil
 * (Alter <= LRM_MEMT). Rueckgabe neue Anzahl. */
static int lrm_mem_update(lrm_mem_t *mem, int nmem, const lrm_part_t *mk, int nm, unsigned ser)
{
    lrm_mem_t out[LRM_MAXP];
    unsigned char used[LRM_MAXP];
    int i, j, n = 0;
    if (nm <= 0) { return nmem; }
    for (j = 0; j < LRM_MAXP; j++) { used[j] = 0; }
    for (j = 0; j < nm && n < LRM_MAXP; j++) { out[n].d = mk[j]; out[n].ser = ser; n++; }
    for (i = 0; i < nmem && n < LRM_MAXP; i++) {
        int hit = 0;
        if (ser - mem[i].ser > LRM_MEMT) { continue; }
        for (j = 0; j < nm && j < LRM_MAXP && !hit; j++) {
            if (!used[j] && lrm_same_shape(&mem[i].d, &mk[j])) { used[j] = 1; hit = 1; }
        }
        if (!hit) { out[n++] = mem[i]; }
    }
    for (i = 0; i < n; i++) { mem[i] = out[i]; }
    return n;
}
