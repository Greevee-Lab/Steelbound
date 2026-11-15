#pragma once
/* sb_mauto.h - W272 ALPHA-TEST (Alpha-Testwerkzeug): Teststart direkt in eine
 * Mission mit einem gewaehlten VT ([input] mission_autostart=<mno>, Launcher
 * Seite "Alpha Test" › "Start test").
 *
 * Der Launcher startet den Client auf einem frischen, leeren Test-Spielstand-
 * ordner (hdd_root/hdd_root_us = <Datenordner>\alpha_test\HDD[_US]); echte
 * Spielstaende werden nie beruehrt. Der Client fuehrt das Spielmenue selbst,
 * gesteuert ueber Zustands-Anker (Szene [0x6B96DC], Missionsschleife
 * [0x356F58]) statt fester Frame-Zahlen (Muster W254 replay_autoplay):
 *   TITLE  Titel/Titel-Demo/"Unable to load Player Record": Hauptwaffe, bis Szene 1
 *   MENU   Hauptmenue: 5x hoch (klemmt auf CAMPAIGN), Hauptwaffe, bis Szene 8
 *   PILOT  Pilotenmenue: 4x hoch (LIST AS NEW), Hauptwaffe, bis Szene 0xC
 *   NAME   Namenseingabe: Tastenfolge (Name "A", COMPLETE, YES), bis Szene 0xD
 *   DIFF   HAI SHI DAO 2080 [ROOKIE]/[NORMAL]: Hauptwaffe, bis Laden (0xF/0x2B)
 *   LOAD   Laden: Missionsnummer [0x3B89A0], VT-Modell [0x3B8AA8] und
 *          Schwierigkeit [0x69CA88] je Frame schreiben (Muster W27/W243)
 *   MISSION Film/Missionsbeginn (optional Film per Lock-On ueberspringen),
 *          DONE sobald float [0x356F30] auf 0 faellt (Missionsbeginn)
 * Fuer mno >= 1 stellt der Client TEXnn.BIN aus den Spieldaten in den
 * Test-ZCACHE (das Spiel kopiert sie sonst nur zur Titelzeit fuer mno 0, W27).
 * Ohne mission_autostart: keine Wirkung (alle Hooks kehren sofort zurueck). */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- reiner Ablauf (ohne Spielspeicher, Selbsttest in input_selftest) ---- */
enum { MA_OFF = 0, MA_TITLE, MA_MENU, MA_PILOT, MA_NAME, MA_DIFF, MA_LOAD, MA_MISSION, MA_DONE, MA_FAIL };
enum { MA_ERR_NONE = 0, MA_ERR_ARGS, MA_ERR_TITLE, MA_ERR_MENU, MA_ERR_PILOT, MA_ERR_NAME, MA_ERR_DIFF, MA_ERR_LOAD };
/* synthetische Eingabe (Spielbedeutung): OK = Hauptwaffe (bi0), BACK = Lock-On (bi2),
 * UP/DOWN/LEFT/RIGHT = Sichtwechsel-Hebel (sight_y/sight_x +-127, wie die Pfeiltasten),
 * MA_KEY_BTN + n = SBC-Taste bi n (Diagnose-Tastenfolgen) */
enum { MA_KEY_NONE = 0, MA_KEY_OK, MA_KEY_BACK, MA_KEY_UP, MA_KEY_DOWN, MA_KEY_LEFT, MA_KEY_RIGHT, MA_KEY_BTN = 0x100 };

#define MA_SC_TITLE  0x00u
#define MA_SC_MENU   0x01u
#define MA_SC_PILOT  0x08u
#define MA_SC_NAME   0x0Cu
#define MA_SC_DIFF   0x0Du
#define MA_SC_PRELD  0x0Fu
#define MA_SC_FADE   0x15u
#define MA_SC_LOAD   0x2Bu
#define MA_SC_PLAY   0x2Cu

#define MA_SEQ_MAX 48

typedef struct {
    int keys[MA_SEQ_MAX];
    int n;
} ma_seq_t;

typedef struct {
    int phase, err;
    int key;              /* gedrueckte Taste ... */
    unsigned key_until;   /* ... bis (ausschliesslich) Frame */
    unsigned next_f;      /* naechste Aktion fruehestens ab Frame */
    int step;             /* Unterschritt der Phase */
    unsigned ms_phase;    /* Laufzeit der Phase in ms (ohne Pause) */
    unsigned ms_step;     /* Laufzeit seit dem letzten Unterschritt */
    int tries;            /* Wiederholungen der Phase */
    int skip_film;        /* MISSION: Film per Lock-On ueberspringen */
    int skips;            /* MISSION: Lock-On-Pulse bisher */
    unsigned f_play;      /* MISSION: Frame des Schleifenbeginns */
    ma_seq_t name_seq;    /* NAME: Tastenfolge */
    ma_seq_t diff_seq;    /* DIFF: Tastenfolge */
} ma_t;

void ma_init(ma_t *m, int skip_film);
/* Tastenfolge "OK,D,D,U,L,R,B,b27" (OK/B=BACK/U/D/L/R/bN) -> seq. 0 = ok. */
int ma_seq_parse(const char *s, ma_seq_t *q);
/* Ein Spiel-Frame f mit Szene/Missionsschleife und film (1 = float [0x356F30]
 * steht auf 1,0: Film/Einblendung vor Missionsbeginn); dt_ms = Zeit seit dem
 * letzten Aufruf (0 in der Pause). Liefert die (neue) Phase. */
int ma_step(ma_t *m, unsigned f, unsigned scene, unsigned mloop, int film, unsigned dt_ms);
/* Synthetische Taste in Frame f (MA_KEY_*), auch nach DONE/FAIL bis zum Loslassen. */
int ma_key(const ma_t *m, unsigned f);
/* 1 = Menuefuehrung laeuft (echte Eingabe gesperrt). */
int ma_navigating(const ma_t *m);
/* 1 = Missionswerte schreiben (Missions-Laden). */
int ma_poking(const ma_t *m, unsigned scene);
const char *ma_phase_name(int phase);
/* Argumente pruefen: mno 0..23, vt -1 oder 1..22 (ohne 12), diff -1 oder 0..5.
 * 0 = gueltig, sonst MA_ERR_ARGS. */
int ma_args_valid(int mno, int vt, int diff);
/* Selbsttest (input_selftest): 0 = ok. */
int sb_mauto_selftest(void);

/* ---- Client-Anbindung (sb_mauto.c) ---- */
void sb_mauto_disable(void);  /* W277: LoC-Profil */
void sb_mauto_init(void);     /* nach cfg + --set (emu_main) */
void sb_mauto_tick(void);     /* je Swap (hooks_d3d) */
void sb_mauto_apply(void *sbc_block); /* je Eingabe-Abfrage, sbc_in_t* (hooks_input) */
/* 1 = mission_autostart gesetzt und noch nicht beendet */
int sb_mauto_active(void);

#ifdef __cplusplus
}
#endif
