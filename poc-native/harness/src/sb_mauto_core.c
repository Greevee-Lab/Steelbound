/* sb_mauto_core.c - W272 ALPHA-TEST: reiner Ablauf des Teststarts (Menue-
 * Fuehrung bis in die Mission, keine Spielspeicher-Zugriffe, kein Logging) +
 * Selbsttest. Anbindung an Spiel/Eingabe/Overlay in sb_mauto.c, Beschreibung
 * in sb_mauto.h. Muster: sb_rauto_core.c (W254). */
#include "sb_mauto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MA_PRESS      12u   /* Tastendauer (W241: 12 Frames stabil, 20 springt doppelt) */
#define MA_GAP_TITLE  60u   /* Abstand der Hauptwaffen-Pulse im Titel / in der Titel-Demo */
#define MA_GAP_NAV    24u   /* Abstand der Cursor-Schritte */
#define MA_GAP_SEQ    60u   /* Abstand in den Tastenfolgen (Namenseingabe, Schwierigkeit) */
#define MA_SETTLE     40u   /* nach Szenenwechsel (32-Frame-Einblendung + Reserve) */
#define MA_SETTLE_LONG 60u
#define MA_MS_TITLE   240000u /* Logos + Titel + ggf. Titel-Demos (Echtzeit) */
#define MA_MS_STEP    6000u   /* ohne Reaktion nach dem letzten Schritt -> Wiederholung/Fehler */
#define MA_MS_LOAD    120000u /* Laden bis Missionsschleife */
#define MA_MS_FILM    300000u /* Film + Einblendung bis Missionsbeginn */
#define MA_TRIES      2
#define MA_FILM_WAIT  90u    /* Film-Skip fruehestens ab Schleifenframe 90 (Spiel: [0x3B8AB0] > 60) */
#define MA_FILM_GAP   150u
#define MA_FILM_SKIPS 4

/* Standard-Tastenfolgen (Lauf W272_x2 belegt, s. BERICHT_M2 "W272: ALPHA-TEST"):
 * Namenseingabe: OK = "A" (Cursor steht auf A), 6x runter klemmt auf COMPLETE, OK ->
 * "Enter Date of Birth" (COMPLETE vorgewaehlt), OK -> YES/NO (YES vorgewaehlt), OK ->
 * Szene 0xD. Zwei OK als Reserve (wirken nur, solange Szene 0xC bleibt: COMPLETE/YES).
 * Schwierigkeit (Szene 0xD, [NORMAL] vorgewaehlt): OK -> Laden. */
static const int k_name_seq[] = { MA_KEY_OK, MA_KEY_DOWN, MA_KEY_DOWN, MA_KEY_DOWN, MA_KEY_DOWN,
                                  MA_KEY_DOWN, MA_KEY_DOWN, MA_KEY_OK, MA_KEY_OK, MA_KEY_OK,
                                  MA_KEY_OK, MA_KEY_OK };
static const int k_diff_seq[] = { MA_KEY_OK };

void ma_init(ma_t *m, int skip_film)
{
    memset(m, 0, sizeof *m);
    m->phase = MA_TITLE;
    m->skip_film = skip_film ? 1 : 0;
    memcpy(m->name_seq.keys, k_name_seq, sizeof k_name_seq);
    m->name_seq.n = (int)(sizeof k_name_seq / sizeof k_name_seq[0]);
    memcpy(m->diff_seq.keys, k_diff_seq, sizeof k_diff_seq);
    m->diff_seq.n = (int)(sizeof k_diff_seq / sizeof k_diff_seq[0]);
}

int ma_seq_parse(const char *s, ma_seq_t *q)
{
    ma_seq_t t;
    memset(&t, 0, sizeof t);
    if (!s || !q) return 1;
    while (*s) {
        char tok[16];
        size_t n = 0;
        int k;
        while (*s == ' ' || *s == ',') s++;
        if (!*s) break;
        while (*s && *s != ',' && *s != ' ' && n + 1 < sizeof tok) tok[n++] = *s++;
        tok[n] = 0;
        if (*s && *s != ',' && *s != ' ') return 1; /* Token zu lang */
        if (!_stricmp(tok, "OK")) k = MA_KEY_OK;
        else if (!_stricmp(tok, "B") || !_stricmp(tok, "BACK")) k = MA_KEY_BACK;
        else if (!_stricmp(tok, "U")) k = MA_KEY_UP;
        else if (!_stricmp(tok, "D")) k = MA_KEY_DOWN;
        else if (!_stricmp(tok, "L")) k = MA_KEY_LEFT;
        else if (!_stricmp(tok, "R")) k = MA_KEY_RIGHT;
        else if ((tok[0] == 'b') && tok[1] >= '0' && tok[1] <= '9') {
            char *e;
            long v = strtol(tok + 1, &e, 10);
            if (*e || v < 0 || v > 47) return 1;
            k = MA_KEY_BTN + (int)v;
        } else return 1;
        if (t.n >= MA_SEQ_MAX) return 1;
        t.keys[t.n++] = k;
    }
    if (t.n == 0) return 1;
    *q = t;
    return 0;
}

int ma_args_valid(int mno, int vt, int diff)
{
    if (mno < 0 || mno > 23) return MA_ERR_ARGS;
    if (vt != -1 && (vt < 1 || vt > 22 || vt == 12)) return MA_ERR_ARGS;
    if (diff < -1 || diff > 5) return MA_ERR_ARGS;
    return MA_ERR_NONE;
}

int ma_navigating(const ma_t *m)
{
    return m->phase >= MA_TITLE && m->phase <= MA_LOAD;
}

int ma_poking(const ma_t *m, unsigned scene)
{
    if (m->phase == MA_LOAD) return scene == MA_SC_DIFF || scene == MA_SC_PRELD || scene == MA_SC_LOAD;
    if (m->phase == MA_DIFF) return m->step > 0 && (scene == MA_SC_DIFF || scene == MA_SC_PRELD);
    return 0;
}

int ma_key(const ma_t *m, unsigned f)
{
    if (m->phase == MA_OFF || m->key == MA_KEY_NONE) return MA_KEY_NONE;
    return f < m->key_until ? m->key : MA_KEY_NONE;
}

static void ma_press(ma_t *m, unsigned f, int key, unsigned gap)
{
    m->key = key;
    m->key_until = f + MA_PRESS;
    m->next_f = f + MA_PRESS + gap;
    m->ms_step = 0;
}

static void ma_release(ma_t *m)
{
    m->key = MA_KEY_NONE;
    m->key_until = 0;
}

static void ma_phase(ma_t *m, int ph, unsigned f, unsigned settle)
{
    m->phase = ph;
    m->step = 0;
    m->tries = 0;
    m->ms_phase = 0;
    m->ms_step = 0;
    m->next_f = f + settle;
}

static void ma_fail(ma_t *m, unsigned f, int err, int key)
{
    if (key == MA_KEY_BACK) ma_press(m, f, MA_KEY_BACK, 0);
    else ma_release(m);
    m->err = err;
    m->phase = MA_FAIL;
}

const char *ma_phase_name(int p)
{
    static const char *const nm[] = { "OFF", "TITLE", "MENU", "PILOT", "NAME", "DIFF", "LOAD", "MISSION", "DONE", "FAIL" };
    return (p >= 0 && p <= MA_FAIL) ? nm[p] : "?";
}

static int ma_loading(unsigned scene)
{
    return scene == MA_SC_PRELD || scene == MA_SC_FADE || scene == MA_SC_LOAD;
}

/* Cursor-Menue: n Schritte key, dann OK; Erfolg = Szene want. */
static void ma_menu(ma_t *m, unsigned f, unsigned scene, unsigned here, unsigned want, int key, int n,
                    int next_phase, int err)
{
    if (scene == want) {
        ma_release(m);
        ma_phase(m, next_phase, f, MA_SETTLE_LONG);
        return;
    }
    if (scene != here) {
        if (ma_loading(scene)) { ma_release(m); return; } /* Uebergang: abwarten */
        ma_fail(m, f, err, MA_KEY_BACK); /* anderer Bildschirm: zurueck, melden */
        return;
    }
    if (m->step > n) { /* OK gedrueckt, Szene unveraendert */
        if (m->ms_step > MA_MS_STEP) {
            if (++m->tries >= MA_TRIES) { ma_fail(m, f, err, MA_KEY_NONE); return; }
            m->step = 0;
            m->next_f = f;
        }
        return;
    }
    if (f < m->next_f) return;
    if (m->step < n) ma_press(m, f, key, MA_GAP_NAV);
    else ma_press(m, f, MA_KEY_OK, 0);
    m->step++;
}

/* Tastenfolge in Szene here; Erfolg = Szene want (oder Laden bei want == ~0u). */
static void ma_seq(ma_t *m, unsigned f, unsigned scene, unsigned here, unsigned want, const ma_seq_t *q,
                   int next_phase, int err)
{
    int done = want == ~0u ? ma_loading(scene) : scene == want;
    if (done) {
        ma_release(m);
        ma_phase(m, next_phase, f, want == ~0u ? 0u : MA_SETTLE_LONG);
        if (next_phase == MA_LOAD) m->step = 1;
        return;
    }
    if (scene != here) {
        if (ma_loading(scene)) { ma_release(m); return; }
        ma_fail(m, f, err, MA_KEY_NONE);
        return;
    }
    if (m->step >= q->n) { /* Folge vollstaendig, Szene unveraendert */
        if (m->ms_step > MA_MS_STEP) ma_fail(m, f, err, MA_KEY_NONE);
        return;
    }
    if (f < m->next_f) return;
    ma_press(m, f, q->keys[m->step], MA_GAP_SEQ);
    m->step++;
}

int ma_step(ma_t *m, unsigned f, unsigned scene, unsigned mloop, int film, unsigned dt_ms)
{
    int playing = scene == MA_SC_PLAY && mloop == 1u;
    if (!ma_navigating(m) && m->phase != MA_MISSION) return m->phase;
    m->ms_phase += dt_ms;
    m->ms_step += dt_ms;
    switch (m->phase) {
    case MA_TITLE:
        if (scene == MA_SC_MENU && mloop == 0u) { /* Hauptmenue: sofort loslassen */
            ma_release(m);
            ma_phase(m, MA_MENU, f, MA_SETTLE);
            break;
        }
        if (m->ms_phase > MA_MS_TITLE) { ma_fail(m, f, MA_ERR_TITLE, MA_KEY_NONE); break; }
        if (!((scene == MA_SC_TITLE && mloop == 0u) || playing)) { ma_release(m); break; }
        /* Titel: START; Titel-Demo: abbrechen; "Unable to load Player Record": bestaetigen */
        if (f >= m->next_f) ma_press(m, f, MA_KEY_OK, MA_GAP_TITLE);
        break;
    case MA_MENU: /* 5x hoch: Cursor klemmt auf CAMPAIGN (erste Zeile) */
        ma_menu(m, f, scene, MA_SC_MENU, MA_SC_PILOT, MA_KEY_UP, 5, MA_PILOT, MA_ERR_MENU);
        break;
    case MA_PILOT: /* 4x hoch: LIST AS NEW (erste Zeile) */
        ma_menu(m, f, scene, MA_SC_PILOT, MA_SC_NAME, MA_KEY_UP, 4, MA_NAME, MA_ERR_PILOT);
        break;
    case MA_NAME:
        ma_seq(m, f, scene, MA_SC_NAME, MA_SC_DIFF, &m->name_seq, MA_DIFF, MA_ERR_NAME);
        break;
    case MA_DIFF:
        ma_seq(m, f, scene, MA_SC_DIFF, ~0u, &m->diff_seq, MA_LOAD, MA_ERR_DIFF);
        break;
    case MA_LOAD:
        if (playing) {
            ma_release(m);
            ma_phase(m, MA_MISSION, f, 0);
            m->f_play = f;
            break;
        }
        if (m->ms_phase > MA_MS_LOAD) { ma_fail(m, f, MA_ERR_LOAD, MA_KEY_NONE); break; }
        if (!ma_loading(scene) && scene != MA_SC_DIFF && scene != MA_SC_PLAY) ma_fail(m, f, MA_ERR_LOAD, MA_KEY_NONE);
        break;
    case MA_MISSION:
        if (!playing || !film || m->ms_phase > MA_MS_FILM) { /* Missionsbeginn (oder Ende/Abbruch) */
            ma_release(m);
            m->phase = MA_DONE;
            break;
        }
        if (m->skip_film && m->skips < MA_FILM_SKIPS && f >= m->f_play + MA_FILM_WAIT && f >= m->next_f) {
            ma_press(m, f, MA_KEY_BACK, MA_FILM_GAP); /* Lock-On = Film ueberspringen (W14) */
            m->skips++;
        }
        break;
    }
    return m->phase;
}

/* ---- Selbsttest: Spielmodell mit den Menues des Neu-Piloten-Wegs ---- */
static int g_ma_fails;
#define MA_CHECK(c, msg) do { if (!(c)) { printf("  FEHLER mauto: %s\n", msg); g_ma_fails++; } } while (0)

typedef struct {
    unsigned scene, mloop;
    int film;
    int cur;            /* Cursor Haupt-/Pilotenmenue */
    int name_done;      /* Namenseingabe: Anzahl gesehener Tasten */
    int prev_key;
    int loading, load_left, film_left, presses;
    int nores_pilot;    /* Pilotenmenue reagiert nicht */
    int skipped;
} ma_game_t;

static void ma_game_frame(ma_game_t *g, int key)
{
    int edge = key != MA_KEY_NONE && key != g->prev_key;
    g->prev_key = key;
    if (g->loading) {
        if (--g->load_left <= 0) { g->loading = 0; g->scene = MA_SC_PLAY; g->mloop = 1; g->film = 1; }
        return;
    }
    if (g->scene == MA_SC_PLAY && g->mloop == 1) {
        if (edge && key == MA_KEY_BACK) { g->skipped++; g->film_left = 1; }
        if (g->film && --g->film_left <= 0) g->film = 0;
        return;
    }
    if (!edge) return;
    g->presses++;
    switch (g->scene) {
    case MA_SC_TITLE:
        if (key == MA_KEY_OK) { g->scene = MA_SC_MENU; g->cur = 2; }
        break;
    case MA_SC_MENU:
        if (key == MA_KEY_UP && g->cur > 0) g->cur--;
        if (key == MA_KEY_DOWN && g->cur < 3) g->cur++;
        if (key == MA_KEY_OK) { g->scene = g->cur == 0 ? MA_SC_PILOT : 0x05u; g->cur = 1; }
        break;
    case MA_SC_PILOT:
        if (g->nores_pilot) break;
        if (key == MA_KEY_UP && g->cur > 0) g->cur--;
        if (key == MA_KEY_OK && g->cur == 0) g->scene = MA_SC_NAME;
        break;
    case MA_SC_NAME:
        if (++g->name_done >= (int)(sizeof k_name_seq / sizeof k_name_seq[0])) g->scene = MA_SC_DIFF;
        break;
    case MA_SC_DIFF:
        if (key == MA_KEY_OK) { g->scene = MA_SC_LOAD; g->loading = 1; g->load_left = 200; }
        break;
    }
}

static int ma_sim(ma_game_t *g, ma_t *m, unsigned frames)
{
    unsigned f;
    for (f = 1; f <= frames; f++) {
        int k;
        ma_step(m, f, g->scene, g->mloop, g->film, 16);
        k = ma_key(m, f);
        ma_game_frame(g, k);
        if (m->phase == MA_DONE || (m->phase == MA_FAIL && ma_key(m, f) == MA_KEY_NONE)) break;
    }
    return m->phase;
}

int sb_mauto_selftest(void)
{
    ma_t m;
    ma_game_t g;
    ma_seq_t q;
    g_ma_fails = 0;
    /* 1: Normalfall mit Film und Film-Skip */
    ma_init(&m, 1);
    memset(&g, 0, sizeof g);
    g.film_left = 100000;
    MA_CHECK(ma_sim(&g, &m, 60000) == MA_DONE, "Normalfall endet mit DONE");
    MA_CHECK(m.err == MA_ERR_NONE, "Normalfall ohne Fehler");
    MA_CHECK(g.skipped >= 1, "Film per Lock-On uebersprungen");
    /* 2: ohne Skip: wartet auf das Filmende */
    ma_init(&m, 0);
    memset(&g, 0, sizeof g);
    g.film_left = 3000;
    MA_CHECK(ma_sim(&g, &m, 60000) == MA_DONE && g.skipped == 0, "ohne Skip: kein Lock-On, DONE nach Film");
    /* 3: Pilotenmenue reagiert nicht -> FAIL PILOT */
    ma_init(&m, 0);
    memset(&g, 0, sizeof g);
    g.nores_pilot = 1;
    MA_CHECK(ma_sim(&g, &m, 200000) == MA_FAIL && m.err == MA_ERR_PILOT, "Pilotenmenue ohne Reaktion: FAIL PILOT");
    /* 4: falscher Bildschirm im Hauptmenue -> FAIL MENU + BACK */
    ma_init(&m, 0);
    m.phase = MA_MENU;
    MA_CHECK(ma_step(&m, 5, 0x05u, 0, 0, 16) == MA_FAIL && m.err == MA_ERR_MENU && ma_key(&m, 6) == MA_KEY_BACK,
             "falscher Eintrag: FAIL MENU mit BACK");
    /* 5: Werte nur im Laden schreiben */
    ma_init(&m, 0);
    m.phase = MA_LOAD;
    MA_CHECK(ma_poking(&m, MA_SC_LOAD) && ma_poking(&m, MA_SC_PRELD) && !ma_poking(&m, MA_SC_PLAY), "Poke-Fenster Laden");
    m.phase = MA_NAME;
    MA_CHECK(!ma_poking(&m, MA_SC_LOAD), "kein Poke in der Namenseingabe");
    /* 6: Laden haengt -> FAIL LOAD */
    ma_init(&m, 0);
    m.phase = MA_LOAD;
    {
        unsigned f;
        for (f = 1; f < 20000 && m.phase == MA_LOAD; f++) ma_step(&m, f, MA_SC_LOAD, 0, 1, 16);
        MA_CHECK(m.phase == MA_FAIL && m.err == MA_ERR_LOAD, "Laden-Zeitablauf: FAIL LOAD");
    }
    /* 7: Titel-Zeitablauf ohne gehaltene Taste */
    ma_init(&m, 0);
    {
        unsigned f;
        for (f = 1; f < 40000 && m.phase == MA_TITLE; f++) ma_step(&m, f, MA_SC_LOAD, 0, 1, 16);
        MA_CHECK(m.phase == MA_FAIL && m.err == MA_ERR_TITLE && ma_key(&m, f) == MA_KEY_NONE, "Titel-Zeitablauf");
    }
    /* 8: Argumente */
    MA_CHECK(ma_args_valid(0, -1, -1) == 0 && ma_args_valid(23, 22, 5) == 0 && ma_args_valid(5, 16, 1) == 0, "gueltige Argumente");
    MA_CHECK(ma_args_valid(24, -1, -1) && ma_args_valid(-1, -1, -1) && ma_args_valid(1, 12, -1) &&
             ma_args_valid(1, 0, -1) && ma_args_valid(1, 23, -1) && ma_args_valid(1, -1, 6), "ungueltige Argumente");
    /* 9: Tastenfolgen */
    MA_CHECK(ma_seq_parse("OK,D,d,U,L,R,B,b27", &q) == 0 && q.n == 8 && q.keys[7] == MA_KEY_BTN + 27 &&
             q.keys[1] == MA_KEY_DOWN, "Tastenfolge parsen");
    MA_CHECK(ma_seq_parse("", &q) != 0 && ma_seq_parse("OK,X", &q) != 0 && ma_seq_parse("b99", &q) != 0, "Tastenfolge Fehler");
    printf("mauto (W272 Teststart): %s (%d Fehler)\n", g_ma_fails ? "FEHLER" : "OK", g_ma_fails);
    return g_ma_fails;
}
