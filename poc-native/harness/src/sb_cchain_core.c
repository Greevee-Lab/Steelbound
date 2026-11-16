/* sb_cchain_core.c - W303 CAMPAIGN-CHAIN: reine Ablauflogik der Kampagnen-Kette
 * (ohne Spielspeicher; Anbindung sb_cchain.c, Selbsttest im input_selftest).
 * Regeln und Phasen: sb_cchain.h. */
#include <stdio.h>
#include <string.h>
#include "sb_cchain.h"

#define CC_KEY_LEN   12u     /* Frames je Taste (wie sb_mauto, ohne Tastenwiederholung) */
#define CC_GAP       45u     /* Frames zwischen Menuetasten */
#define CC_PRESS_GAP 90u     /* Frames zwischen OK-Pulsen auf Folgebildschirmen */
#define CC_MS_WAIT   420000u /* Laden + Einblendung bis Missionsbeginn */
#define CC_MS_END    60000u  /* Testhook -> Missionsende */
#define CC_MS_RETRY  3000u   /* Testhook wiederholen, falls die Phase bei 5 bleibt */
#define CC_MS_SCENE  600000u /* gleiche Szene ohne Fortschritt (Abspann ed1j/ed2j laeuft in 0x2C) */
#define CC_MS_UNK    20000u  /* unbekannte Szene: erst danach OK-Pulse */
#define CC_MS_NAV    300000u /* Ladetest: Titel -> Mission */
#define CC_MS_NEXT_IDLE 8000u /* Szene 0x36 ohne Dialog: erst danach OK-Pulse */
#define CC_DLG_SETTLE 60u    /* Frames nach dem Oeffnen der Zweig-Auswahl bis zur ersten Taste */

static void cc_press(cc_t *c, unsigned f, int k, unsigned gap)
{
    c->key = k;
    c->key_until = f + CC_KEY_LEN;
    c->next_f = f + CC_KEY_LEN + gap;
}

static void cc_set(cc_t *c, int p)
{
    c->phase = p;
    c->ms_phase = 0;
    c->step = 0;
    c->tries = 0;
}

static int cc_running(const cc_in_t *in)
{
    return in->scene == CC_SC_PLAY && in->mloop == 1u && !in->demo && in->phase == 5u && !in->film;
}

/* Folgebildschirme, die mit der Hauptwaffe weiterschalten (SPIELZUSTAENDE §26.4,
 * KAMPAGNE_STAND §3): 0x0E/0x18/0x19 Vor-Briefing/Missionstitel, 0x23 VT SELECTION,
 * 0x2D COMBAT RESULTS, 0x2E/0x2F Befoerderung, 0x30 Dialog (nach Sieg: Kapitelende/Finale),
 * 0x31/0x32/0x33/0x34/0x35 Folgeseiten/Abblenden.
 * 0x36 nie (laeuft von selbst; ein OK beim Oeffnen der Zweig-Auswahl waere eine Wahl). */
static int cc_press_scene(unsigned sc)
{
    switch (sc) {
    case 0x0Eu: case 0x18u: case 0x19u: case 0x23u:
    case 0x2Du: case 0x2Eu: case 0x2Fu: case 0x30u: case 0x31u: case 0x32u: case 0x33u: case 0x34u: case 0x35u:
        return 1;
    default:
        return 0;
    }
}

void cc_init(cc_t *c, int want, int branch, unsigned hold, int load)
{
    memset(c, 0, sizeof *c);
    c->want = want < 0 ? 0 : want;
    c->branch = branch ? 1 : 0;
    c->hold = hold;
    c->load = load ? 1 : 0;
    c->last_scene = 0xFFFFFFFFu;
    c->bkey = CC_KEY_DOWN;
    c->bcur_before = -1;
    c->phase = c->load ? CC_NAV : CC_WAIT;
}

int cc_key(const cc_t *c, unsigned f)
{
    return (c->key != CC_KEY_NONE && f < c->key_until) ? c->key : CC_KEY_NONE;
}

int cc_navigating(const cc_t *c)
{
    return c->phase == CC_NAV || c->phase == CC_POST;
}

const char *cc_phase_name(int p)
{
    static const char *const nm[] = { "OFF", "NAV", "WAIT", "HOLD", "END", "POST", "DONE", "FAIL" };
    return (p >= 0 && p <= CC_FAIL) ? nm[p] : "?";
}

/* BRIEFING (0x1B, Vorwahl MISSION BRIEFING): 3x runter (COMPLETE), OK -> 0x22.
 * CONFIRMATION (0x22, Vorwahl NO): hoch (YES), OK -> 0x2B.
 * Bleibt die Szene nach der Folge stehen, ein weiteres OK (verschluckt). */
static void cc_menu(cc_t *c, unsigned f, unsigned sc)
{
    if (sc == CC_SC_BRIEF) {
        if (c->step < 3) {
            cc_press(c, f, CC_KEY_DOWN, CC_GAP);
            c->step++;
        } else if (c->step == 3) {
            cc_press(c, f, CC_KEY_OK, CC_GAP * 4u);
            c->step++;
        } else if (c->ms_scene > 15000u && c->step == 4) {
            cc_press(c, f, CC_KEY_OK, CC_GAP * 4u);
            c->step++;
        }
    } else { /* CC_SC_CONFIRM */
        if (c->step == 0) {
            cc_press(c, f, CC_KEY_UP, CC_GAP);
            c->step++;
        } else if (c->step == 1) {
            cc_press(c, f, CC_KEY_OK, CC_GAP * 4u);
            c->step++;
        } else if (c->ms_scene > 10000u && c->step == 2) {
            cc_press(c, f, CC_KEY_OK, CC_GAP * 4u);
            c->step++;
        }
    }
}

/* Zweig-Auswahl nach mno 15 (Szene 0x36, Dialog offen, Cursor [0x6B9C50]):
 * Cursor auf den gewuenschten Zweig, dann OK. Bewegt eine Taste den Cursor
 * nicht, naechste Richtung (runter, rechts, hoch, links). */
static void cc_branch_to(cc_t *c, unsigned f, const cc_in_t *in, int want)
{
    static const int order[4] = { CC_KEY_DOWN, CC_KEY_RIGHT, CC_KEY_UP, CC_KEY_LEFT };
    if (!c->dlg_f) {
        c->dlg_f = f ? f : 1;
    }
    if (f - c->dlg_f < CC_DLG_SETTLE) { /* W303_chain_d: OK im ersten Bild des Dialogs -> Umweg 0x21 */
        return;
    }
    if (in->cursor == want) {
        cc_press(c, f, CC_KEY_OK, CC_GAP * 4u);
        c->bcur_before = -1;
        return;
    }
    if (c->bcur_before < 0 && c->bkey == CC_KEY_DOWN && want < in->cursor) {
        c->bkey = CC_KEY_UP; /* erste Richtung zum Ziel */
    }
    if (c->bcur_before >= 0 && c->bcur_before == in->cursor) { /* letzte Taste ohne Wirkung */
        int i;
        for (i = 0; i < 4 && order[i] != c->bkey; i++) { }
        c->bkey = order[(i + 1) & 3];
    }
    c->bcur_before = in->cursor;
    cc_press(c, f, c->bkey, CC_GAP);
}

static void cc_branch(cc_t *c, unsigned f, const cc_in_t *in)
{
    cc_branch_to(c, f, in, c->branch);
}

/* Abfrage nach COMPLETE im BRIEFING (W303_chain_e, M20 "Air Drop", FUN_000E0140 mno 0x13):
 * der Zweig-Cursor [0x6B9C50] bewegt sich hier nicht, OK auf der Vorwahl fuehrt zurueck ins
 * Briefing. Je Oeffnung eine andere Richtung (hoch, links, runter, rechts, keine), dann OK. */
static void cc_brief_dialog(cc_t *c, unsigned f)
{
    static const int dir[5] = { CC_KEY_UP, CC_KEY_LEFT, CC_KEY_DOWN, CC_KEY_RIGHT, CC_KEY_NONE };
    if (!c->dlg_f) {
        c->dlg_f = f ? f : 1;
        c->bd_sub = 0;
        if (c->bd_try >= 5) { /* alle Richtungen ohne Wirkung: Start aus dem Briefing gesperrt */
            c->err = CC_ERR_GATE;
            c->act = CC_ACT_SNAP;
            cc_set(c, CC_FAIL);
            return;
        }
    }
    if (f - c->dlg_f < CC_DLG_SETTLE || c->bd_sub >= 2) {
        return;
    }
    if (c->bd_sub == 0 && dir[c->bd_try % 5] != CC_KEY_NONE) {
        cc_press(c, f, dir[c->bd_try % 5], CC_GAP);
        c->bd_sub = 1;
        return;
    }
    cc_press(c, f, CC_KEY_OK, CC_GAP * 4u);
    c->bd_sub = 2;
    c->bd_try++;
}

static void cc_post(cc_t *c, unsigned f, const cc_in_t *in)
{
    unsigned sc = in->scene;
    if (sc == CC_SC_LOAD || cc_running(in)) {
        c->chained++;
        c->act = CC_ACT_SNAP;
        c->key = CC_KEY_NONE;
        cc_set(c, CC_WAIT);
        return;
    }
    /* 0x30/0x33 nach einem Tod (Phase 1 beim Missionsende): Ausbilder-Tipp / GAME OVER.
     * Nach einem Sieg fuehrt der Sonderpfad nach mno 10/23 (Szene 0x36 setzt Phase 1,
     * W303_chain_c) ebenfalls ueber 0x30 - dort normal weiterschalten. */
    if ((sc == 0x30u || sc == 0x33u) && c->end_phase != 0u) {
        c->err = CC_ERR_LOST;
        c->act = CC_ACT_SNAP;
        cc_set(c, CC_FAIL);
        return;
    }
    if (sc == CC_SC_TITLE || sc == CC_SC_MENU || sc == 0x15u) { /* Kampagnenende: zurueck zum Titel */
        c->title_end = 1;
        c->act = CC_ACT_SNAP;
        c->key = CC_KEY_NONE;
        cc_set(c, CC_DONE);
        return;
    }
    if (c->ms_scene > CC_MS_SCENE) {
        c->err = CC_ERR_STUCK;
        c->act = CC_ACT_SNAP;
        cc_set(c, CC_FAIL);
        return;
    }
    if (f < c->next_f) {
        return;
    }
    if (sc == CC_SC_BRIEF && in->dialog) {
        cc_brief_dialog(c, f);
    } else if (sc == CC_SC_BRIEF && c->bd_try > 0 && c->step >= 4) {
        if (c->dlg_f) { c->dlg_f = 0; c->bd_sub = 0; } /* Abfrage zu, wieder im Briefing (Cursor auf COMPLETE) */
        cc_press(c, f, CC_KEY_OK, CC_PRESS_GAP * 2u); /* COMPLETE erneut -> Abfrage mit naechster Richtung */
    } else if (sc == CC_SC_BRIEF || sc == CC_SC_CONFIRM) {
        c->dlg_f = 0;
        cc_menu(c, f, sc);
    } else if (sc == CC_SC_TROOP) {
        cc_branch(c, f, in);
    } else if (sc == CC_SC_NEXT || sc == CC_SC_BRSEL) {
        if (in->dialog) {
            cc_branch(c, f, in);
        } else {
            c->dlg_f = 0;
            if (sc == CC_SC_NEXT && c->ms_scene > CC_MS_NEXT_IDLE) /* 0x36 steht ohne Dialog (W303_chain_c nach mno 10) */
                cc_press(c, f, CC_KEY_OK, CC_PRESS_GAP * 2u);
        }
    } else if (cc_press_scene(sc)) {
        cc_press(c, f, CC_KEY_OK, CC_PRESS_GAP);
    } else if (c->ms_scene > CC_MS_UNK) {
        cc_press(c, f, CC_KEY_OK, CC_PRESS_GAP * 2u);
    }
}

/* Ladetest: Titel -> CAMPAIGN -> LISTED PILOT -> erster Pilot -> Briefing. */
static void cc_nav(cc_t *c, unsigned f, const cc_in_t *in)
{
    unsigned sc = in->scene;
    if (sc == CC_SC_LOAD || cc_running(in)) {
        c->key = CC_KEY_NONE;
        cc_set(c, CC_WAIT);
        return;
    }
    if (c->ms_phase > CC_MS_NAV) {
        c->err = CC_ERR_NAV;
        cc_set(c, CC_FAIL);
        return;
    }
    if (f < c->next_f) {
        return;
    }
    switch (sc) {
    case 0x0Du: /* Kapitelwahl beim Fortsetzen (nach Kapitelende/Finale, W303_load_chain_c):
                 * HAI SHI DAO 2080 (Vorwahl) / 2082; chapter=1 -> einmal runter */
        if (c->ms_scene < 1500u) break; /* Seite blendet ein: fruehe Taste wirkungslos (W303_load_chain_c_02_m10_2082) */
        if (c->chapter && c->step == 0) { cc_press(c, f, CC_KEY_DOWN, CC_GAP); c->step++; }
        else cc_press(c, f, CC_KEY_OK, 60u);
        break;
    case CC_SC_TITLE: case 0x15u: case CC_SC_PLIST: case 0x0Eu: case 0x18u: case 0x19u:
        cc_press(c, f, CC_KEY_OK, 60u); /* Titel/Demo: Hauptwaffe-Pulse wie sb_mauto TITLE */
        break;
    case CC_SC_MENU: /* 5x hoch (klemmt auf CAMPAIGN), OK */
        if (c->step < 5) { cc_press(c, f, CC_KEY_UP, CC_GAP); c->step++; }
        else if (c->step == 5) { cc_press(c, f, CC_KEY_OK, CC_GAP * 4u); c->step++; }
        break;
    case CC_SC_PILOT: /* 4x hoch (LIST AS NEW), 1x runter (LISTED PILOT), OK */
        if (c->step < 4) { cc_press(c, f, CC_KEY_UP, CC_GAP); c->step++; }
        else if (c->step == 4) { cc_press(c, f, CC_KEY_DOWN, CC_GAP); c->step++; }
        else if (c->step == 5) { cc_press(c, f, CC_KEY_OK, CC_GAP * 4u); c->step++; }
        break;
    case CC_SC_BRIEF: case CC_SC_CONFIRM:
        cc_menu(c, f, sc);
        break;
    case CC_SC_BRSEL: /* Pilot mit Missionswahl 51 (nach mno 15): Zweig-Auswahl beim Laden */
        if (in->dialog) cc_branch(c, f, in);
        else c->dlg_f = 0;
        break;
    case CC_SC_TROOP: /* W303_load_chain_d: "Xray Platoon, which of the troops do you choose to join?"
                       * A Troop (WEI GU) / B Troop (ER TING) - Auswahl ohne Dialog-Flag */
        cc_branch(c, f, in);
        break;
    default:
        break;
    }
}

int cc_step(cc_t *c, unsigned f, const cc_in_t *in, unsigned dt_ms)
{
    c->act = CC_ACT_NONE;
    if (c->phase == CC_OFF || c->phase == CC_DONE || c->phase == CC_FAIL) {
        return c->phase;
    }
    c->ms_phase += dt_ms;
    if (in->scene != c->last_scene) {
        c->last_scene = in->scene;
        c->ms_scene = 0;
        if (c->phase == CC_POST || c->phase == CC_NAV) {
            c->step = 0;
            c->bkey = CC_KEY_DOWN;
            c->bcur_before = -1;
        }
    } else {
        c->ms_scene += dt_ms;
    }
    switch (c->phase) {
    case CC_NAV:
        cc_nav(c, f, in);
        break;
    case CC_WAIT:
        if (cc_running(in)) {
            c->f_begin = f;
            cc_set(c, c->won >= c->want ? CC_DONE : CC_HOLD);
        } else if (c->ms_phase > CC_MS_WAIT) {
            c->err = CC_ERR_WAIT;
            c->act = CC_ACT_SNAP;
            cc_set(c, CC_FAIL);
        }
        break;
    case CC_HOLD:
        if (in->phase != 5u || (in->scene == CC_SC_PLAY && in->mloop == 0u)) {
            cc_set(c, CC_END); /* Mission endete vor dem Testhook (Spielverlauf) */
        } else if (f - c->f_begin >= c->hold && cc_running(in)) {
            c->act = CC_ACT_WIN;
            c->win_mno = in->mno;
            c->won++;
            cc_set(c, CC_END);
        }
        break;
    case CC_END:
        if (in->phase != 5u && (in->mloop == 0u || in->scene != CC_SC_PLAY)) {
            c->end_phase = in->phase;
            c->key = CC_KEY_NONE;
            cc_set(c, CC_POST);
            c->ms_scene = 0;
        } else if (in->phase == 5u && c->ms_phase > CC_MS_RETRY * (unsigned)(c->tries + 1) && c->tries < 3) {
            c->tries++;
            c->act = CC_ACT_WIN; /* Sieg nicht uebernommen (z. B. Pause) - erneut */
        } else if (c->ms_phase > CC_MS_END) {
            c->err = CC_ERR_WIN;
            c->act = CC_ACT_SNAP;
            cc_set(c, CC_FAIL);
        }
        break;
    case CC_POST:
        cc_post(c, f, in);
        break;
    default:
        break;
    }
    return c->phase;
}

/* ---------------- Selbsttest ---------------- */

typedef struct {
    cc_t c;
    cc_in_t in;
    unsigned f;
    int keys[8];   /* gezaehlte Tastenflanken je CC_KEY_* */
    int last;
    int wins, snaps;
} cc_sim_t;

static void cc_sim_init(cc_sim_t *s, int want, int branch, int load)
{
    memset(s, 0, sizeof *s);
    cc_init(&s->c, want, branch, 30u, load);
    s->f = 1000u;
}

static void cc_sim_run(cc_sim_t *s, unsigned frames)
{
    unsigned i;
    for (i = 0; i < frames; i++) {
        int k;
        cc_step(&s->c, s->f, &s->in, 33u);
        if (s->c.act == CC_ACT_WIN) s->wins++;
        if (s->c.act == CC_ACT_SNAP) s->snaps++;
        k = cc_key(&s->c, s->f);
        if (k != s->last && k > 0 && k < 8) s->keys[k]++;
        s->last = k;
        s->f++;
    }
}

static void cc_sim_play(cc_sim_t *s, unsigned mno)
{
    s->in.scene = CC_SC_PLAY;
    s->in.mloop = 1;
    s->in.phase = 5;
    s->in.mno = mno;
    s->in.film = 0;
    s->in.demo = 0;
    s->in.dialog = 0;
}

static void cc_sim_scene(cc_sim_t *s, unsigned sc)
{
    s->in.scene = sc;
    s->in.mloop = 0;
    memset(s->keys, 0, sizeof s->keys);
}

int sb_cchain_selftest(void)
{
    int fails = 0;
    cc_sim_t s;
#define CCK(cond, msg) do { if (!(cond)) { fails++; printf("  CCHAIN FAIL: %s\n", msg); } } while (0)

    /* 1. Normalfall: Laden/Einblendung, Mission, Testhook, Folgebildschirme, naechste Mission */
    cc_sim_init(&s, 1, 0, 0);
    s.in.scene = CC_SC_LOAD; s.in.mloop = 0; s.in.phase = 5;
    cc_sim_run(&s, 50);
    CCK(s.c.phase == CC_WAIT && s.wins == 0, "Laden: WAIT, kein Testhook");
    cc_sim_play(&s, 4); s.in.film = 1;
    cc_sim_run(&s, 100);
    CCK(s.c.phase == CC_WAIT, "Einblendung (f30=1): noch WAIT");
    s.in.film = 0;
    cc_sim_run(&s, 20);
    CCK(s.c.phase == CC_HOLD && s.wins == 0, "Missionsbeginn: HOLD vor Ablauf von hold");
    cc_sim_run(&s, 20);
    CCK(s.wins == 1 && s.c.phase == CC_END && s.c.win_mno == 4, "nach hold: genau ein Testhook, END");
    s.in.phase = 0; s.in.mloop = 0;
    cc_sim_run(&s, 5);
    CCK(s.c.phase == CC_POST, "Phase 0 + Schleife 0: POST");
    CCK(cc_navigating(&s.c), "POST fuehrt (echte Eingabe gesperrt)");
    cc_sim_scene(&s, CC_SC_RESULTS);
    cc_sim_run(&s, 200);
    CCK(s.keys[CC_KEY_OK] >= 2 && s.keys[CC_KEY_DOWN] == 0, "COMBAT RESULTS: OK-Pulse");
    cc_sim_scene(&s, CC_SC_NEXT);
    cc_sim_run(&s, 200);
    CCK(s.keys[CC_KEY_OK] == 0, "Szene 0x36 ohne Dialog: keine Taste");
    cc_sim_scene(&s, CC_SC_BRIEF);
    cc_sim_run(&s, 400);
    CCK(s.keys[CC_KEY_DOWN] == 3 && s.keys[CC_KEY_OK] == 1, "BRIEFING: 3x runter + COMPLETE");
    cc_sim_scene(&s, CC_SC_CONFIRM);
    cc_sim_run(&s, 300);
    CCK(s.keys[CC_KEY_UP] == 1 && s.keys[CC_KEY_OK] == 1, "CONFIRMATION: hoch + YES");
    cc_sim_scene(&s, CC_SC_LOAD);
    cc_sim_run(&s, 3);
    CCK(s.c.chained == 1 && s.snaps == 1 && s.c.phase == CC_WAIT, "Laden der Folgemission: Abzug + WAIT");
    cc_sim_play(&s, 5);
    cc_sim_run(&s, 100);
    CCK(s.c.phase == CC_DONE && s.wins == 1, "Folgemission laeuft, Ziel erreicht: DONE ohne weiteren Testhook");
    CCK(cc_key(&s.c, s.f) == CC_KEY_NONE && !cc_navigating(&s.c), "DONE: keine Taste, Spieler hat Eingabe");

    /* 2. Zwei Siege in Folge */
    cc_sim_init(&s, 2, 0, 0);
    cc_sim_play(&s, 1); cc_sim_run(&s, 40);
    s.in.phase = 0; s.in.mloop = 0; cc_sim_run(&s, 3);
    cc_sim_scene(&s, CC_SC_LOAD); cc_sim_run(&s, 3);
    cc_sim_play(&s, 2); cc_sim_run(&s, 40);
    CCK(s.wins == 2 && s.c.phase == CC_END, "zweite Mission: zweiter Testhook");

    /* 3. Zweig-Auswahl: Cursor 0 -> Zweig 1 (runter), dann OK */
    cc_sim_init(&s, 1, 1, 0);
    cc_sim_play(&s, 15); cc_sim_run(&s, 40);
    s.in.phase = 0; s.in.mloop = 0; cc_sim_run(&s, 3);
    cc_sim_scene(&s, CC_SC_NEXT); s.in.dialog = 1; s.in.cursor = 0;
    cc_sim_run(&s, 50);
    CCK(s.keys[CC_KEY_DOWN] == 0 && s.keys[CC_KEY_OK] == 0, "Zweig: erste 60 Frames keine Taste");
    cc_sim_run(&s, 40);
    CCK(s.keys[CC_KEY_DOWN] == 1 && s.keys[CC_KEY_OK] == 0, "Zweig 1: erst runter");
    s.in.cursor = 1;
    cc_sim_run(&s, 100);
    CCK(s.keys[CC_KEY_OK] == 1, "Zweig 1: Cursor steht, OK");

    /* 4. Zweig-Auswahl: runter wirkungslos -> naechste Richtung (rechts) */
    cc_sim_init(&s, 1, 1, 0);
    cc_sim_play(&s, 15); cc_sim_run(&s, 40);
    s.in.phase = 0; s.in.mloop = 0; cc_sim_run(&s, 3);
    cc_sim_scene(&s, CC_SC_NEXT); s.in.dialog = 1; s.in.cursor = 0;
    cc_sim_run(&s, 200);
    CCK(s.keys[CC_KEY_DOWN] >= 1 && s.keys[CC_KEY_RIGHT] >= 1 && s.keys[CC_KEY_OK] == 0,
        "Zweig: wirkungslose Taste -> naechste Richtung, kein OK");

    /* 5. Zweig 0: Cursor steht schon -> sofort OK */
    cc_sim_init(&s, 1, 0, 0);
    cc_sim_play(&s, 15); cc_sim_run(&s, 40);
    s.in.phase = 0; s.in.mloop = 0; cc_sim_run(&s, 3);
    cc_sim_scene(&s, CC_SC_NEXT); s.in.dialog = 1; s.in.cursor = 0;
    cc_sim_run(&s, 90);
    CCK(s.keys[CC_KEY_OK] == 1 && s.keys[CC_KEY_DOWN] == 0, "Zweig 0: nach der Wartezeit OK");

    /* 6. Kampagnenende: Titel statt Mission */
    cc_sim_init(&s, 1, 0, 0);
    cc_sim_play(&s, 23); cc_sim_run(&s, 40);
    s.in.phase = 1; s.in.mloop = 0; cc_sim_run(&s, 3);
    cc_sim_scene(&s, 0x15u); cc_sim_run(&s, 3);
    CCK(s.c.phase == CC_DONE && s.c.title_end == 1 && s.snaps == 1, "Titel nach dem letzten Sieg: DONE, title_end");

    /* 7. Pilot gefallen (0x30) -> FAIL LOST */
    cc_sim_init(&s, 1, 0, 0);
    cc_sim_play(&s, 2); cc_sim_run(&s, 10);
    s.in.phase = 1; s.in.mloop = 0; cc_sim_run(&s, 3);
    CCK(s.c.phase == CC_POST && s.wins == 0, "Mission endet vor dem Testhook: POST ohne Testhook");
    cc_sim_scene(&s, 0x30u); cc_sim_run(&s, 3);
    CCK(s.c.phase == CC_FAIL && s.c.err == CC_ERR_LOST, "0x30: FAIL LOST");

    /* 7b. Sieg, danach 0x30 (Sonderpfad nach mno 10/23): weiterschalten, kein FAIL */
    cc_sim_init(&s, 1, 0, 0);
    cc_sim_play(&s, 10); cc_sim_run(&s, 40);
    s.in.phase = 0; s.in.mloop = 0; cc_sim_run(&s, 3);
    cc_sim_scene(&s, 0x30u); s.in.phase = 1; cc_sim_run(&s, 200);
    CCK(s.c.phase == CC_POST && s.keys[CC_KEY_OK] >= 2, "Sieg + 0x30 (Kapitelende): OK-Pulse, kein FAIL");

    /* 7c. BRIEFING: Ja/Nein-Abfrage (Cursor 1 = NO) -> hoch auf YES, OK */
    cc_sim_init(&s, 1, 0, 0);
    cc_sim_play(&s, 18); cc_sim_run(&s, 40);
    s.in.phase = 0; s.in.mloop = 0; cc_sim_run(&s, 3);
    cc_sim_scene(&s, CC_SC_BRIEF); cc_sim_run(&s, 300);
    memset(s.keys, 0, sizeof s.keys);
    s.in.dialog = 1; s.in.cursor = 1; cc_sim_run(&s, 150); /* OK aus dem Briefing sperrt bis +192 Frames */
    CCK(s.keys[CC_KEY_UP] == 1 && s.keys[CC_KEY_OK] == 0, "BRIEFING-Abfrage 1: hoch");
    cc_sim_run(&s, 100);
    CCK(s.keys[CC_KEY_OK] == 1, "BRIEFING-Abfrage 1: OK");
    s.in.dialog = 0; memset(s.keys, 0, sizeof s.keys); cc_sim_run(&s, 200);
    CCK(s.keys[CC_KEY_OK] >= 1, "BRIEFING nach Abfrage: COMPLETE erneut");
    s.in.dialog = 1; memset(s.keys, 0, sizeof s.keys); cc_sim_run(&s, 400);
    CCK(s.keys[CC_KEY_LEFT] == 1 && s.keys[CC_KEY_OK] == 1, "BRIEFING-Abfrage 2: links + OK");
    { int k;
      for (k = 0; k < 4 && s.c.phase == CC_POST; k++) { s.in.dialog = 0; cc_sim_run(&s, 200); s.in.dialog = 1; cc_sim_run(&s, 400); }
      CCK(s.c.phase == CC_FAIL && s.c.err == CC_ERR_GATE, "BRIEFING-Abfrage nach 5 Versuchen: FAIL Briefing-Tor"); }

    /* 7d. Szene 0x36 ohne Dialog > 8 s: OK-Pulse */
    cc_sim_init(&s, 1, 0, 0);
    cc_sim_play(&s, 10); cc_sim_run(&s, 40);
    s.in.phase = 0; s.in.mloop = 0; cc_sim_run(&s, 3);
    cc_sim_scene(&s, CC_SC_NEXT); cc_sim_run(&s, 200);
    CCK(s.keys[CC_KEY_OK] == 0, "0x36 ohne Dialog < 8 s: keine Taste");
    cc_sim_run(&s, 200);
    CCK(s.keys[CC_KEY_OK] >= 1, "0x36 ohne Dialog > 8 s: OK-Puls");

    /* 8. Testhook nicht uebernommen -> Wiederholung, hoechstens 3 */
    cc_sim_init(&s, 1, 0, 0);
    cc_sim_play(&s, 3); cc_sim_run(&s, 40);
    cc_sim_run(&s, 600);
    CCK(s.wins == 4 && s.c.phase == CC_END, "Phase bleibt 5: 1 + 3 Wiederholungen");
    cc_sim_run(&s, 1400);
    CCK(s.c.phase == CC_FAIL && s.c.err == CC_ERR_WIN, "danach FAIL WIN");

    /* 9. Haengende Szene -> FAIL STUCK */
    cc_sim_init(&s, 1, 0, 0);
    cc_sim_play(&s, 3); cc_sim_run(&s, 40);
    s.in.phase = 0; s.in.mloop = 0; cc_sim_run(&s, 3);
    cc_sim_scene(&s, 0x2Du); cc_sim_run(&s, 18300);
    CCK(s.c.phase == CC_FAIL && s.c.err == CC_ERR_STUCK, "gleiche Szene > 600 s: FAIL STUCK");

    /* 10. Unbekannte Szene: erst nach 20 s OK-Pulse */
    cc_sim_init(&s, 1, 0, 0);
    cc_sim_play(&s, 10); cc_sim_run(&s, 40);
    s.in.phase = 0; s.in.mloop = 0; cc_sim_run(&s, 3);
    cc_sim_scene(&s, 0x37u); cc_sim_run(&s, 500);
    CCK(s.keys[CC_KEY_OK] == 0, "unbekannte Szene < 20 s: keine Taste");
    cc_sim_run(&s, 300);
    CCK(s.keys[CC_KEY_OK] >= 1, "unbekannte Szene > 20 s: OK-Puls");

    /* 11. Ladetest: Titel -> Menue -> Pilot -> Liste -> Briefing -> Mission, want 0 */
    cc_sim_init(&s, 0, 0, 1);
    CCK(s.c.phase == CC_NAV, "Ladetest beginnt mit NAV");
    cc_sim_scene(&s, CC_SC_TITLE); s.in.demo = 0; cc_sim_run(&s, 100);
    CCK(s.keys[CC_KEY_OK] >= 1, "Titel: OK");
    cc_sim_scene(&s, CC_SC_MENU); cc_sim_run(&s, 600);
    CCK(s.keys[CC_KEY_UP] == 5 && s.keys[CC_KEY_OK] == 1, "Hauptmenue: 5x hoch + OK");
    cc_sim_scene(&s, CC_SC_PILOT); cc_sim_run(&s, 600);
    CCK(s.keys[CC_KEY_UP] == 4 && s.keys[CC_KEY_DOWN] == 1 && s.keys[CC_KEY_OK] == 1,
        "Pilotenmenue: 4x hoch, runter (LISTED PILOT), OK");
    cc_sim_scene(&s, CC_SC_PLIST); cc_sim_run(&s, 80);
    CCK(s.keys[CC_KEY_OK] >= 1, "Pilotenliste: OK");
    cc_sim_scene(&s, 0x0Du); cc_sim_run(&s, 110);
    CCK(s.keys[CC_KEY_OK] >= 1 && s.keys[CC_KEY_DOWN] == 0, "Ladetest Kapitelwahl (0x0D), Vorwahl 2080: OK");
    s.c.chapter = 1; cc_sim_scene(&s, 0x0Du); s.c.last_scene = 0xFFFFFFFFu; s.c.next_f = 0; cc_sim_run(&s, 160);
    CCK(s.keys[CC_KEY_DOWN] == 1 && s.keys[CC_KEY_OK] >= 1, "Ladetest Kapitelwahl 2082: runter + OK");
    s.c.chapter = 0;
    cc_sim_scene(&s, CC_SC_TROOP); s.in.cursor = 0; cc_sim_run(&s, 120);
    CCK(s.keys[CC_KEY_OK] == 1 && s.keys[CC_KEY_DOWN] == 0, "Ladetest Truppenwahl (0x17), Zweig 0: OK");
    cc_sim_scene(&s, CC_SC_BRIEF); cc_sim_run(&s, 400);
    CCK(s.keys[CC_KEY_DOWN] == 3 && s.keys[CC_KEY_OK] == 1, "Ladetest BRIEFING: 3x runter + COMPLETE");
    cc_sim_scene(&s, CC_SC_LOAD); cc_sim_run(&s, 3);
    CCK(s.c.phase == CC_WAIT, "Ladetest: Missions-Laden -> WAIT");
    cc_sim_play(&s, 7); cc_sim_run(&s, 3);
    CCK(s.c.phase == CC_DONE && s.wins == 0, "Ladetest want 0: Mission laeuft -> DONE ohne Testhook");

    /* 12. Titel-Demo zaehlt nicht als Mission */
    cc_sim_init(&s, 1, 0, 0);
    cc_sim_play(&s, 0); s.in.demo = 1; cc_sim_run(&s, 100);
    CCK(s.c.phase == CC_WAIT && s.wins == 0, "Titel-Demo: kein Testhook");

    printf("W303 CAMPAIGN-CHAIN: Fehler=%d\n", fails);
#undef CCK
    return fails;
}
