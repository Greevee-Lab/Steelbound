#pragma once
/* sb_cchain.h - W303 CAMPAIGN-CHAIN: Testwerkzeug fuer die Kampagnen-Kette
 * ([input] campaign_chain=<N>, Standard 0 = aus; nie fuer Spieler sichtbar,
 * kein Launcher-/ESC-Eintrag).
 *
 * Prueft Mission N -> Missionsende/Debriefing -> Speichern -> Briefing ->
 * Mission N+1 automatisch, auch fuer Missionen, die kein Bot gewinnt:
 *   WAIT   bis eine Spielermission laeuft (Szene 0x2C, Schleife 1, Phase 5,
 *          keine Demo, Einblendung f30 vorbei)
 *   HOLD   campaign_chain_hold Frames stehen, dann Missionsende-Testhook:
 *          derselbe Sieg-Aufruf wie in den Missionsskripten
 *          (FUN_0002C280(0) + FUN_0002C250(0xD6, 0.01); Training wie 0x4A880
 *          mit Story-Film-Flags), keine Zustands-Pokes auf Wertung/Speichern
 *   END    bis das Spiel den Sieg uebernommen hat (Phase 0, Schleife 0)
 *   POST   Folgebildschirme wie ein Spieler (Hauptwaffe auf COMBAT RESULTS,
 *          Befoerderung, Abblende, Titel; BRIEFING: 3x runter + COMPLETE;
 *          CONFIRMATION: hoch + YES; Auswahl nach mno 15: Zweig 0/1), bis die
 *          naechste Mission laedt (Szene 0x2B) bzw. der Titel kommt
 *   NAV    nur mit campaign_chain_load=1: vom Titel ueber CAMPAIGN ->
 *          LISTED PILOT -> erster Pilot in die gespeicherte Mission (Ladetest)
 * Alle Uebergaenge nur ueber Szene/Schleife/Phase, Zeitgrenzen in echter Zeit
 * ohne Pause. Ohne campaign_chain/campaign_chain_load kehren alle Hooks sofort
 * zurueck (Standardpfad unveraendert). */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { CC_OFF = 0, CC_NAV, CC_WAIT, CC_HOLD, CC_END, CC_POST, CC_DONE, CC_FAIL };
enum { CC_ERR_NONE = 0, CC_ERR_WAIT, CC_ERR_WIN, CC_ERR_STUCK, CC_ERR_NAV, CC_ERR_LOST, CC_ERR_GATE };
enum { CC_ACT_NONE = 0, CC_ACT_WIN = 1, CC_ACT_SNAP = 2 };
/* Tasten wie sb_mauto (Spielbedeutung): OK = Hauptwaffe, UP/DOWN = Sichtwechsel-Hebel */
enum { CC_KEY_NONE = 0, CC_KEY_OK, CC_KEY_BACK, CC_KEY_UP, CC_KEY_DOWN, CC_KEY_LEFT, CC_KEY_RIGHT };

#define CC_SC_TITLE   0x00u
#define CC_SC_MENU    0x01u
#define CC_SC_PILOT   0x08u
#define CC_SC_PLIST   0x0Au
#define CC_SC_TROOP   0x17u /* Truppenwahl A/B (Zweig nach mno 15) beim Laden, sichtbarer Text */
#define CC_SC_BRSEL   0x21u /* Zweig-Auswahl beim Laden (Missionswahl 51, FUN_000E2D20) */
#define CC_SC_BRIEF   0x1Bu
#define CC_SC_CONFIRM 0x22u
#define CC_SC_VTSEL   0x23u
#define CC_SC_LOAD    0x2Bu
#define CC_SC_PLAY    0x2Cu
#define CC_SC_RESULTS 0x2Du
#define CC_SC_NEXT    0x36u

typedef struct {
    unsigned scene;   /* [0x6B96DC] */
    unsigned mloop;   /* [0x356F58] */
    unsigned phase;   /* u16 [0x66ABF4]: 5 laeuft, 0 Sieg, 1/2/3/4 Enden */
    unsigned mno;     /* u16 [0x3B89A0] */
    int demo;         /* [0x3B8CDC] == 1 (Titel-Demo) */
    int film;         /* float [0x356F30] >= 1 (Laden/Einblendung vor Missionsbeginn) */
    int dialog;       /* [0x686D44] != 0 (Auswahl/Dialog offen) */
    int cursor;       /* [0x6B9C50] Cursor der Zweig-Auswahl (0 = mno 16, 1 = mno 18) */
} cc_in_t;

typedef struct {
    int phase, err;
    int want;          /* Siege in Folge (campaign_chain) */
    int won;           /* bisher ausgeloeste Siege */
    int chained;       /* Folgemissionen geladen */
    int branch;        /* Auswahl nach mno 15: 0 = mno 16, 1 = mno 18 */
    int load;          /* 1 = Ladetest vom Titel */
    int chapter;       /* Ladetest, Kapitelwahl 0x0D: 0 = Vorwahl (HAI SHI DAO 2080), 1 = 2082 */
    unsigned hold;     /* Frames in der Mission bis zum Testhook */
    int key;
    unsigned key_until, next_f;
    int step;          /* Unterschritt (BRIEFING/CONFIRMATION/Menues) */
    unsigned step_scene;
    unsigned last_scene;
    unsigned ms_phase, ms_scene;
    unsigned f_begin;
    int tries;
    int title_end;     /* 1 = nach dem letzten Sieg kam der Titel statt einer Mission */
    int act;           /* Aktion fuer die Anbindung in diesem Schritt (CC_ACT_*) */
    unsigned win_mno;  /* mno des letzten Siegs */
    int bkey;          /* Zweig-Auswahl: aktuelle Cursortaste (wechselt, falls wirkungslos) */
    int bcur_before;
    int bd_try, bd_sub; /* Briefing-Abfrage: Versuch (Richtung) und Unterschritt */
    unsigned dlg_f;     /* Frame, ab dem der Zweig-Dialog offen ist (0 = zu) */
    unsigned end_phase; /* Missionsphase beim Wechsel END -> POST (0 = Sieg, 1 = gefallen, ...) */
} cc_t;

void cc_init(cc_t *c, int want, int branch, unsigned hold, int load);
/* Ein Spiel-Frame f; dt_ms = Zeit seit dem letzten Aufruf (0 in der Pause).
 * Liefert die (neue) Phase; c->act meldet eine Aktion fuer genau diesen Schritt. */
int cc_step(cc_t *c, unsigned f, const cc_in_t *in, unsigned dt_ms);
int cc_key(const cc_t *c, unsigned f);
/* 1 = Werkzeug fuehrt gerade Menues/Folgebildschirme (echte Eingabe gesperrt). */
int cc_navigating(const cc_t *c);
const char *cc_phase_name(int phase);
int sb_cchain_selftest(void);

/* ---- Client-Anbindung (sb_cchain.c) ---- */
void sb_cchain_disable(void);  /* LoC-Profil */
void sb_cchain_init(void);     /* nach cfg + --set (emu_main) */
void sb_cchain_tick(void);     /* je Swap (hooks_d3d) */
void sb_cchain_apply(void *sbc_block); /* je Eingabe-Abfrage (hooks_input) */
int sb_cchain_active(void);

#ifdef __cplusplus
}
#endif
