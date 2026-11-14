#pragma once
/* sb_rauto.h - W254 REPLAY-LAUNCHER (experimentell): gespeichertes Replay
 * direkt starten ([input] replay_autoplay=<Ordner-ID>, Launcher "Play replay").
 *
 * Der Client fuehrt das Spielmenue selbst bis zur Wiedergabe, gesteuert ueber
 * Zustands-Anker statt fester Frame-Zahlen:
 *   Szene [0x6B96DC]: 0 = Logos/Titel, 1 = Hauptmenue (CAMPAIGN, MISSION LOG,
 *   REPLAY LOAD, CONFIG; Cursor klemmt oben/unten), 5 = REPLAY LOAD
 *   (Cursor auf Zeile 0, Auswahl -> "Load? YES/NO", YES vorgewaehlt),
 *   0x15/0x2B = Ladevorgang, 0x2C + Missionsschleife [0x356F58]=1 = Wiedergabe.
 * Ablauf: TITLE (Titel/Titel-Demo: Hauptwaffe, bis Szene 1) -> MENU (5x runter,
 * 1x hoch = REPLAY LOAD, Hauptwaffe, bis Szene 5) -> LIST (Hauptwaffe = Zeile 0,
 * Hauptwaffe = YES) -> CONFIRM (Laden, Kontrolle: welches .rep oeffnet das
 * Spiel) -> PLAY (Wiedergabe; Kamera optional fest) -> DONE.
 * Damit Zeile 0 sicher das gewaehlte Replay ist, listet der Client bis zum
 * Ende der Wiedergabe unter UDATA\43430002 nur den gewaehlten Ordner und
 * liest "Last Replay" (ZCACHE\st00_0.rep, nur reine Lese-Oeffnungen) aus der
 * gewaehlten Datei. Schreibzugriffe bleiben unveraendert.
 * Ohne replay_autoplay: keine Wirkung (alle Hooks kehren sofort zurueck). */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- reiner Ablauf (ohne Spielspeicher, Selbsttest in input_selftest) ---- */
enum { RA_OFF = 0, RA_TITLE, RA_MENU, RA_LIST, RA_CONFIRM, RA_PLAY, RA_DONE, RA_FAIL };
enum { RA_ERR_NONE = 0, RA_ERR_ID, RA_ERR_MISSING, RA_ERR_TITLE, RA_ERR_MENU, RA_ERR_START, RA_ERR_WRONG };
/* synthetische Eingabe (Spielbedeutung): OK = Hauptwaffe (bi0), BACK = Lock-On (bi2),
 * UP/DOWN = Sichtwechsel-Hebel (sight_y -127/+127, wie die Pfeiltasten) */
enum { RA_KEY_NONE = 0, RA_KEY_OK, RA_KEY_BACK, RA_KEY_UP, RA_KEY_DOWN };

#define RA_SC_TITLE 0x00u
#define RA_SC_MENU  0x01u
#define RA_SC_RLOAD 0x05u
#define RA_SC_FADE  0x15u
#define RA_SC_LOAD  0x2Bu
#define RA_SC_PLAY  0x2Cu

typedef struct {
    int phase, err;
    int key;              /* gedrueckte Taste ... */
    unsigned key_until;   /* ... bis (ausschliesslich) Frame */
    unsigned next_f;      /* naechste Aktion fruehestens ab Frame */
    int step;             /* Unterschritt der Phase */
    unsigned ms_phase;    /* Laufzeit der Phase in ms (ohne Pause) */
    unsigned ms_step;     /* Laufzeit seit dem letzten Unterschritt */
    int tries;            /* Wiederholungen der Phase */
    int rep_open;         /* CONFIRM: 0 = noch kein .rep, 1 = gewaehltes, 2 = anderes */
} ra_t;

void ra_init(ra_t *r);
/* Ein Spiel-Frame f mit Szene/Missionsschleife; dt_ms = Zeit seit dem letzten
 * Aufruf (0 in der Pause). Liefert die (neue) Phase. */
int ra_step(ra_t *r, unsigned f, unsigned scene, unsigned mloop, unsigned dt_ms);
/* Synthetische Taste in Frame f (RA_KEY_*), auch nach DONE/FAIL bis zum Loslassen. */
int ra_key(const ra_t *r, unsigned f);
/* 1 = Navigation laeuft (echte Eingabe gesperrt). */
int ra_navigating(const ra_t *r);
/* Ordner-ID pruefen: 1..32 Hex-Zeichen. 1 = gueltig. */
int ra_id_valid(const char *id);
const char *ra_phase_name(int phase);
/* Selbsttest (input_selftest): 0 = ok. */
int sb_rauto_selftest(void);

/* ---- Client-Anbindung (sb_rauto.c) ---- */
void sb_rauto_init(void);     /* nach cfg + --set (emu_main) */
void sb_rauto_tick(void);     /* je Swap (hooks_d3d) */
void sb_rauto_apply(void *sbc_block); /* je Eingabe-Abfrage, sbc_in_t* (hooks_input) */
/* Kernel (krnl_file.c): Ordnerliste filtern (1 = Eintrag auslassen) */
int sb_rauto_dir_skip(const char *dir_host, const char *name, int is_dir);
/* Kernel: Oeffnen beobachten/umleiten (1 = alt statt host oeffnen) */
int sb_rauto_open_hook(const char *host, uint32_t access, uint32_t disp, char *alt, unsigned cap);
/* 1 = replay_autoplay gesetzt und noch nicht beendet (Hooks aktiv) */
int sb_rauto_active(void);
/* W255 VIDEO-EXPORT: Phase RA_* (err: RA_ERR_*, darf NULL sein) */
int sb_rauto_phase(int *err);

#ifdef __cplusplus
}
#endif
