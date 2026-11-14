/* sb_rauto_core.c - W254 REPLAY-LAUNCHER: reiner Ablauf der Menue-Fuehrung
 * (keine Spielspeicher-Zugriffe, kein Logging) + Selbsttest. Anbindung an
 * Spiel/Kernel/Eingabe in sb_rauto.c, Beschreibung in sb_rauto.h. */
#include "sb_rauto.h"
#include <stdio.h>
#include <string.h>

#define RA_PRESS      12u   /* Tastendauer (W241: 12 Frames stabil, 20 springt doppelt) */
#define RA_GAP_TITLE  60u   /* Abstand der Hauptwaffen-Pulse im Titel / in der Titel-Demo */
#define RA_GAP_NAV    24u   /* Abstand der Cursor-Schritte im Hauptmenue */
#define RA_SETTLE     40u   /* nach Szenenwechsel (32-Frame-Einblendung + Reserve) */
#define RA_SETTLE_LIST 60u
#define RA_MS_TITLE   240000u /* Logos + Titel + ggf. Titel-Demos (Echtzeit) */
#define RA_MS_STEP    6000u   /* ohne Reaktion nach OK/YES -> Wiederholung */
#define RA_MS_CONFIRM 90000u  /* Laden bis Wiedergabe */
#define RA_TRIES      2

void ra_init(ra_t *r)
{
    memset(r, 0, sizeof *r);
    r->phase = RA_TITLE;
}

int ra_navigating(const ra_t *r)
{
    return r->phase >= RA_TITLE && r->phase <= RA_CONFIRM;
}

int ra_key(const ra_t *r, unsigned f)
{
    if (r->phase == RA_OFF || r->key == RA_KEY_NONE) return RA_KEY_NONE;
    return f < r->key_until ? r->key : RA_KEY_NONE;
}

static void ra_press(ra_t *r, unsigned f, int key, unsigned gap)
{
    r->key = key;
    r->key_until = f + RA_PRESS;
    r->next_f = f + RA_PRESS + gap;
    r->ms_step = 0;
}

static void ra_release(ra_t *r)
{
    r->key = RA_KEY_NONE;
    r->key_until = 0;
}

static void ra_phase(ra_t *r, int ph, unsigned f, unsigned settle)
{
    r->phase = ph;
    r->step = 0;
    r->ms_phase = 0;
    r->ms_step = 0;
    r->next_f = f + settle;
}

static void ra_fail(ra_t *r, unsigned f, int err, int key) /* key: RA_KEY_NONE/BACK/OK */
{
    if (key == RA_KEY_BACK) {
        ra_press(r, f, RA_KEY_BACK, 0); /* z. B. offenen YES/NO-Dialog schliessen */
    } else if (key == RA_KEY_OK) {
        ra_press(r, f, RA_KEY_OK, 0); /* Spielmeldung "Unable to load ..." bestaetigen */
    } else {
        ra_release(r);
    }
    r->err = err;
    r->phase = RA_FAIL;
}

int ra_id_valid(const char *id)
{
    size_t n = 0;
    if (!id) return 0;
    for (; id[n]; n++) {
        char c = id[n];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) return 0;
        if (n >= 32) return 0;
    }
    return n >= 1;
}

const char *ra_phase_name(int p)
{
    static const char *const nm[] = { "OFF", "TITLE", "MENU", "LIST", "CONFIRM", "PLAY", "DONE", "FAIL" };
    return (p >= 0 && p <= RA_FAIL) ? nm[p] : "?";
}

int ra_step(ra_t *r, unsigned f, unsigned scene, unsigned mloop, unsigned dt_ms)
{
    int playing = scene == RA_SC_PLAY && mloop == 1u;
    if (!ra_navigating(r) && r->phase != RA_PLAY) return r->phase;
    r->ms_phase += dt_ms;
    r->ms_step += dt_ms;
    switch (r->phase) {
    case RA_TITLE:
        if (scene == RA_SC_MENU && mloop == 0u) { /* Hauptmenue: sofort loslassen (Taste nicht ins Menue tragen) */
            ra_release(r);
            ra_phase(r, RA_MENU, f, RA_SETTLE);
            break;
        }
        if (r->ms_phase > RA_MS_TITLE) { ra_fail(r, f, RA_ERR_TITLE, 0); break; }
        if (!((scene == RA_SC_TITLE && mloop == 0u) || playing)) { /* Laden/Uebergang: nichts druecken */
            ra_release(r);
            break;
        }
        if (f >= r->next_f) ra_press(r, f, RA_KEY_OK, RA_GAP_TITLE); /* Titel: START, Titel-Demo: abbrechen */
        break;
    case RA_MENU:
        if (scene == RA_SC_RLOAD) {
            ra_release(r);
            ra_phase(r, RA_LIST, f, RA_SETTLE_LIST);
            break;
        }
        if (scene != RA_SC_MENU) { /* anderer Bildschirm (falscher Eintrag) -> zurueck, melden */
            ra_fail(r, f, RA_ERR_MENU, (scene != RA_SC_FADE && scene != RA_SC_LOAD) ? RA_KEY_BACK : RA_KEY_NONE);
            break;
        }
        if (r->step == 7) { /* OK gedrueckt, Szene 1 unveraendert */
            if (r->ms_step > RA_MS_STEP) {
                if (++r->tries >= RA_TRIES) { ra_fail(r, f, RA_ERR_MENU, 0); break; }
                r->step = 0;
                r->next_f = f;
            }
            break;
        }
        if (f < r->next_f) break;
        if (r->step < 5) { /* 5x runter: Cursor klemmt auf CONFIG (letzte Zeile) */
            ra_press(r, f, RA_KEY_DOWN, RA_GAP_NAV);
        } else if (r->step == 5) { /* 1x hoch: REPLAY LOAD */
            ra_press(r, f, RA_KEY_UP, RA_GAP_NAV + 6u);
        } else {
            ra_press(r, f, RA_KEY_OK, 0);
        }
        r->step++;
        break;
    case RA_LIST:
        if (scene != RA_SC_RLOAD) {
            if (scene == RA_SC_FADE || scene == RA_SC_LOAD || playing) { /* Laden hat schon begonnen */
                ra_release(r);
                ra_phase(r, RA_CONFIRM, f, 0);
            } else {
                ra_fail(r, f, RA_ERR_MENU, 0);
            }
            break;
        }
        if (f < r->next_f) break;
        /* Zeile 0 = gewaehltes Replay (gefilterte Liste), dann YES (vorgewaehlt) */
        ra_press(r, f, RA_KEY_OK, RA_SETTLE_LIST);
        if (++r->step >= 2) {
            r->phase = RA_CONFIRM;
            r->ms_phase = 0;
        }
        break;
    case RA_CONFIRM:
        if (r->rep_open == 2) { ra_fail(r, f, RA_ERR_WRONG, 0); break; }
        if (playing) {
            ra_release(r);
            ra_phase(r, RA_PLAY, f, 0);
            break;
        }
        if (r->ms_phase > RA_MS_CONFIRM) { ra_fail(r, f, RA_ERR_START, 0); break; }
        if (scene == RA_SC_RLOAD && r->rep_open == 1 && r->ms_step > RA_MS_STEP) {
            /* Spiel hat das Replay gelesen, laedt aber nicht ("Unable to load ...", z. B.
             * fremde Signatur ohne save_check): Meldung bestaetigen, melden */
            ra_fail(r, f, RA_ERR_START, RA_KEY_OK);
        } else if (scene == RA_SC_RLOAD && r->rep_open == 0 && r->ms_step > RA_MS_STEP) {
            /* nichts geladen: Zeile gesperrt (Fortschritt des Piloten?) -> einmal wiederholen */
            if (++r->tries >= RA_TRIES) { ra_fail(r, f, RA_ERR_START, RA_KEY_BACK); break; }
            r->phase = RA_LIST;
            r->step = 0;
            r->next_f = f;
        } else if (scene != RA_SC_RLOAD && scene != RA_SC_FADE && scene != RA_SC_LOAD && scene != RA_SC_PLAY) {
            ra_fail(r, f, RA_ERR_START, 0);
        }
        break;
    case RA_PLAY:
        if (!playing) {
            ra_release(r);
            r->phase = RA_DONE;
        }
        break;
    }
    return r->phase;
}

/* ---- Selbsttest: Zustandsfolge wie im Lauf W254_x3 (Szenen + Tasten) ---- */
static int g_ra_fails;
#define RA_CHECK(c, m) do { if (!(c)) { printf("  FEHLER rauto: %s\n", m); g_ra_fails++; } } while (0)

/* Spielmodell: reagiert auf Tasten-Flanken wie das Menue (vereinfacht). */
typedef struct {
    unsigned scene, mloop;
    int cur;              /* Hauptmenue-Cursor 0..3 (klemmt) */
    int lock_mlog;        /* MISSION LOG gesperrt (uebersprungen) */
    int dialog;           /* REPLAY LOAD: YES/NO offen */
    int row_locked;       /* Zeile 0 nicht waehlbar */
    int prev_key;
    int loading, load_left, play_left;
    int presses_ok;
} ra_game_t;

static void ra_game_frame(ra_game_t *g, ra_t *r, int key)
{
    int edge = key != RA_KEY_NONE && key != g->prev_key;
    g->prev_key = key;
    if (g->loading) {
        if (--g->load_left <= 0) { g->loading = 0; g->scene = RA_SC_PLAY; g->mloop = 1; }
        return;
    }
    if (g->scene == RA_SC_PLAY && g->mloop == 1) {
        if (--g->play_left <= 0) { g->mloop = 0; g->scene = RA_SC_RLOAD; }
        return;
    }
    if (!edge) return;
    if (g->scene == RA_SC_TITLE && key == RA_KEY_OK) { g->scene = RA_SC_MENU; return; }
    if (g->scene == RA_SC_MENU) {
        if (key == RA_KEY_DOWN) { g->cur++; if (g->cur == 1 && g->lock_mlog) g->cur++; if (g->cur > 3) g->cur = 3; }
        if (key == RA_KEY_UP) { g->cur--; if (g->cur == 1 && g->lock_mlog) g->cur--; if (g->cur < 0) g->cur = 0; }
        if (key == RA_KEY_OK) g->scene = g->cur == 2 ? RA_SC_RLOAD : (g->cur == 3 ? 0x10u : 0x08u);
        return;
    }
    if (g->scene == RA_SC_RLOAD) {
        if (key == RA_KEY_BACK) { if (g->dialog) g->dialog = 0; return; }
        if (key != RA_KEY_OK) return;
        g->presses_ok++;
        if (!g->dialog) { if (!g->row_locked) g->dialog = 1; return; }
        g->dialog = 0;
        g->scene = RA_SC_LOAD;
        g->loading = 1;
        g->load_left = 300;
        r->rep_open = 1; /* Spiel oeffnet das gewaehlte st00_0.rep */
    }
}

static int ra_sim(ra_game_t *g, ra_t *r, unsigned frames, unsigned ms_per_frame)
{
    unsigned f;
    for (f = 1; f <= frames; f++) {
        int k;
        ra_step(r, f, g->scene, g->mloop, ms_per_frame);
        k = ra_key(r, f);
        ra_game_frame(g, r, k);
        if (r->phase == RA_DONE || (r->phase == RA_FAIL && ra_key(r, f) == RA_KEY_NONE)) break;
    }
    return r->phase;
}

int sb_rauto_selftest(void)
{
    ra_t r;
    ra_game_t g;
    g_ra_fails = 0;
    /* 1: Normalfall (MISSION LOG frei) -> Wiedergabe -> Ende */
    ra_init(&r);
    memset(&g, 0, sizeof g);
    g.play_left = 500;
    RA_CHECK(ra_sim(&g, &r, 20000, 16) == RA_DONE, "Normalfall endet mit DONE");
    RA_CHECK(r.err == RA_ERR_NONE, "Normalfall ohne Fehler");
    RA_CHECK(g.presses_ok == 2, "REPLAY LOAD: genau Auswahl + YES");
    /* 2: MISSION LOG gesperrt (Cursor springt) -> gleicher Weg */
    ra_init(&r);
    memset(&g, 0, sizeof g);
    g.lock_mlog = 1;
    g.cur = 3;
    g.play_left = 50;
    RA_CHECK(ra_sim(&g, &r, 20000, 16) == RA_DONE, "MISSION LOG gesperrt: DONE");
    /* 3: Zeile gesperrt (Fortschritt) -> FAIL START, Dialog bleibt zu */
    ra_init(&r);
    memset(&g, 0, sizeof g);
    g.row_locked = 1;
    RA_CHECK(ra_sim(&g, &r, 200000, 16) == RA_FAIL, "gesperrte Zeile: FAIL");
    RA_CHECK(r.err == RA_ERR_START, "gesperrte Zeile: Fehler START");
    RA_CHECK(g.dialog == 0, "gesperrte Zeile: kein Dialog offen");
    /* 4: falsches Replay geoeffnet -> FAIL WRONG */
    ra_init(&r);
    r.phase = RA_CONFIRM;
    r.rep_open = 2;
    RA_CHECK(ra_step(&r, 10, RA_SC_LOAD, 0, 16) == RA_FAIL && r.err == RA_ERR_WRONG, "fremdes .rep: FAIL WRONG");
    /* 4b: Replay gelesen, aber nicht geladen ("Unable to load") -> FAIL START + OK */
    ra_init(&r);
    r.phase = RA_CONFIRM;
    r.rep_open = 1;
    {
        unsigned f;
        for (f = 1; f < 2000 && r.phase == RA_CONFIRM; f++) ra_step(&r, f, RA_SC_RLOAD, 0, 16);
        RA_CHECK(r.phase == RA_FAIL && r.err == RA_ERR_START && ra_key(&r, f) == RA_KEY_OK,
                 "gelesen, nicht geladen: FAIL START mit OK");
    }
    /* 5: Titel kommt nie -> FAIL TITLE nach Zeitablauf, ohne gehaltene Taste */
    ra_init(&r);
    memset(&g, 0, sizeof g);
    g.scene = RA_SC_LOAD; /* haengt im Laden */
    g.loading = 0;
    {
        unsigned f;
        for (f = 1; f < 40000 && r.phase == RA_TITLE; f++) ra_step(&r, f, RA_SC_LOAD, 0, 16);
        RA_CHECK(r.phase == RA_FAIL && r.err == RA_ERR_TITLE, "Titel-Zeitablauf: FAIL TITLE");
        RA_CHECK(ra_key(&r, f) == RA_KEY_NONE, "Titel-Zeitablauf: keine Taste");
    }
    /* 6: Taste wird beim Wechsel ins Hauptmenue sofort losgelassen */
    ra_init(&r);
    ra_step(&r, 1, RA_SC_TITLE, 0, 16);
    RA_CHECK(ra_key(&r, 2) == RA_KEY_OK, "Titel: Hauptwaffe gedrueckt");
    ra_step(&r, 3, RA_SC_MENU, 0, 16);
    RA_CHECK(ra_key(&r, 3) == RA_KEY_NONE && r.phase == RA_MENU, "Hauptmenue: sofort losgelassen");
    /* 7: Titel-Demo wird per Hauptwaffe abgebrochen (Taste in Szene 0x2C/mloop 1) */
    ra_init(&r);
    ra_step(&r, 1, RA_SC_PLAY, 1, 16);
    RA_CHECK(ra_key(&r, 2) == RA_KEY_OK, "Titel-Demo: Hauptwaffe");
    ra_step(&r, 3, RA_SC_LOAD, 0, 16);
    RA_CHECK(ra_key(&r, 3) == RA_KEY_NONE, "Laden: nichts gedrueckt");
    /* 8: falscher Hauptmenue-Eintrag -> FAIL MENU + BACK */
    ra_init(&r);
    r.phase = RA_MENU;
    RA_CHECK(ra_step(&r, 5, 0x08u, 0, 16) == RA_FAIL && r.err == RA_ERR_MENU && ra_key(&r, 6) == RA_KEY_BACK,
             "falscher Eintrag: FAIL MENU mit BACK");
    /* 9: IDs */
    RA_CHECK(ra_id_valid("5EA711E32FA5"), "ID 12 Hex");
    RA_CHECK(ra_id_valid("abc"), "ID kurz klein");
    RA_CHECK(!ra_id_valid(""), "ID leer");
    RA_CHECK(!ra_id_valid("..\\x"), "ID Pfad");
    RA_CHECK(!ra_id_valid("5EA711E32FA5G"), "ID Nicht-Hex");
    RA_CHECK(!ra_id_valid("0123456789ABCDEF0123456789ABCDEF0"), "ID zu lang");
    RA_CHECK(ra_id_valid("0123456789ABCDEF0123456789ABCDEF"), "ID 32 Zeichen");
    printf("rauto (W254 Replay-Start): %s (%d Fehler)\n", g_ra_fails ? "FEHLER" : "OK", g_ra_fails);
    return g_ra_fails;
}
