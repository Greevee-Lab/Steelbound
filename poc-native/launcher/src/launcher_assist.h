#pragma once
/* launcher_assist.h - W143 CTRL-ASSIST: Engine hinter dem grafischen
 * Controller-Test- und Kalibrier-Assistenten des Launchers.
 *
 * Quelle (USB-Geraet oder Fixture-Datei), gefuehrte Pruefung ueber den
 * gemeinsamen Kern sbc_test (eigener Arbeiter-Thread), Kalibrier-Erfassung,
 * Lampensteuerung und chirurgisches Schreiben der eigenen Zeilen in
 * user.cfg [input]. Oberflaeche (Dialog) in launcher_gui.c; Ereignisse
 * kommen ueber assist_cb_t (Vorsicht: aus dem Arbeiter-Thread - die
 * Oberflaeche muss marshallen; Textzeiger nur im Callback gueltig).
 */
#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "input_sbc_usb.h"
#include "sbc_test.h"

/* Quelle */
enum {
    ASSIST_SRC_NONE = 0,
    ASSIST_SRC_DEVICE = 1,
    ASSIST_SRC_FIXTURE = 2
};

/* Ereignisse (assist_cb_t.event). */
enum {
    ASSIST_EV_STATE = 1,  /* neuer Report (Live-Ansicht neu zeichnen) */
    ASSIST_EV_SOURCE,     /* a = ASSIST_SRC_* (Quelle gewechselt/erkannt) */
    ASSIST_EV_TEST_START, /* a = Schrittindex (Schritt beginnt) */
    ASSIST_EV_TEST_LINE,  /* text = Aufforderung/Logzeile des Kerns */
    ASSIST_EV_TEST_DONE,  /* a = Schrittindex, b = Status (Schritt fertig) */
    ASSIST_EV_TEST_END,   /* a = letzter Schritt, b = abgebrochen (Lauf zu Ende) */
    ASSIST_EV_CAL_DONE,   /* a = Achse/Quelle (s. ASSIST_CAL_*), Erfassung fertig */
    ASSIST_EV_LED         /* a = Folgenindex 0..38 (Lampenfolge laeuft) */
};

/* Erfassungs-Quellen (ASSIST_EV_CAL_DONE a, assist_cal_axis/assist_cal_extra). */
enum {
    ASSIST_CAL_TUNER = 100,
    ASSIST_CAL_GEAR = 101
};

typedef struct {
    void (*event)(void *ctx, int ev, int a, int b, const char *text);
    void *ctx;
} assist_cb_t;

typedef struct assist assist_t;

assist_t *assist_create(const assist_cb_t *cb);
void assist_destroy(assist_t *as);

/* Quelle oeffnen: fixture = NULL/leer -> USB-Geraet (automatisch);
 * sonst Fixture-Datei. Rueckgabe ASSIST_SRC_* (NONE = kein Geraet und
 * keine/falsche Fixture - Dialog bleibt nutzbar, Hinweis anzeigen). */
int assist_open(assist_t *as, const char *fixture);
void assist_close(assist_t *as);
int assist_source(assist_t *as);
int assist_source_err(assist_t *as); /* SBC_ERR_* der letzten Oeffnung */

/* Timer-Aufruf der Oberflaeche (ca. 10 Hz): pollt die Quelle (nur wenn der
 * Arbeiter idle ist), versucht verlorene Geraete neu zu oeffnen (1/s).
 * Feuert ASSIST_EV_STATE/ASSIST_EV_SOURCE bei Aenderung. */
void assist_poll(assist_t *as);

/* Letzter Report (1 = gueltig). */
int assist_has_report(assist_t *as);
void assist_raw(assist_t *as, uint8_t raw[SBC_USB_IN_LEN]);
void assist_state(assist_t *as, sbc_state_t *st);

/* Schneller Takt (nur Fixture/Shots): Uhr mal 20, Schlaf durch 20. */
void assist_set_fast(assist_t *as, int on);

/* ---- Gefuehrte Pruefung (Arbeiter-Thread, ein Auftrag zugleich) ---- */
int assist_test_busy(assist_t *as);
int assist_test_current(assist_t *as); /* Schrittindex oder -1 */
/* Schritte from..49 nacheinander fahren (Ergebnisse ab from neu). */
void assist_test_run(assist_t *as, int from);
/* Einen Schritt fahren (Ergebnis ab idx neu). */
void assist_test_step(assist_t *as, int idx);
/* Taste einspeisen ('\r' = ueberspringen/fertig, 'j'/'n', 0x1B = Abbruch). */
void assist_test_key(assist_t *as, int key);
/* Arbeiter anhalten (Schritt bricht ab, Ergebnisse bleiben). */
void assist_test_stop(assist_t *as);
const sbc_test_step_t *assist_steps(assist_t *as);
const sbc_test_res_t *assist_results(assist_t *as);
void assist_results_clear(assist_t *as, int from);
/* Ergebnisdatei im Format sbc_test_<datum>.txt (geteilter Schreiber).
 * path = NULL waehlt sbc_test_<stempel>.txt neben der Exe. 0 = ok. */
int assist_write_report(assist_t *as, const char *path);

/* ---- Kalibrier-Erfassung (Arbeiter, ein Auftrag zugleich) ---- */
/* Achse a (0..7, sbc_test_axes): Ruhe + Endanschlaege erfassen. */
void assist_cal_axis(assist_t *as, int a);
/* Tuner/Gang: nur Rasten pruefen (ASSIST_CAL_TUNER/_GEAR). */
void assist_cal_extra(assist_t *as, int what);
/* Letzte Erfassung abholen (1 = gueltig). */
int assist_cal_result(assist_t *as, int *what_out, sbc_test_res_t *out);
/* Totzonen-Vorschau: Rohwert des aktuellen Reports gegen Spielwert mit
 * (lo,mid,hi,dead) bzw. Nennbereich, falls set = 0. 1 = Report da. */
int assist_preview(assist_t *as, int a, int set, int lo, int mid, int hi,
                   int dead, int *raw_out, int *game_out);

/* ---- Lampen ---- */
/* Eine LED setzen (Geraet: senden; Fixture: Mock-Anzeige). 0 = ok. */
int assist_led_set(assist_t *as, int led, int level);
int assist_led_all(assist_t *as, int level);
/* Folge auf dem Arbeiter (Fortschritt per ASSIST_EV_LED). 1 = gestartet. */
int assist_led_seq_start(assist_t *as);
void assist_led_seq_stop(assist_t *as);
/* Mock-Pegel (Fixture) bzw. zuletzt gesetzte Pegel (Geraet) je LED. */
int assist_led_level(assist_t *as, int led);

/* ---- user.cfg (ohne Geraet nutzbar) ---- */
/* Eigene Zeilen ("key=value") in [input] setzen: vorhandene eigene Zeilen
 * ersetzen, Rest der Datei bytegleich erhalten (inkl. Zeilenenden).
 * 0 = ok, 1 = Datei/Formatfehler. */
int assist_cfg_apply(const char *user_cfg, const char lines[][96], int n);
/* Eigene Zeilen entfernen (Werkswerte). 0 = ok, 1 = Fehler. */
int assist_cfg_reset(const char *user_cfg);
/* Eigene Zeilen aus [input] lesen (Vorgaben der Regler). Rueckgabe Anzahl
 * (0 = keine/keine Datei), lines[i] = "key=value". */
int assist_cfg_read(const char *user_cfg, char lines[][96], int cap);
/* 1 = Key gehoert dem Assistenten (sbc_cal_xx, sbc_dead_xx, pad_dz_xx). */
int assist_cfg_owns(const char *key);

#ifdef __cplusplus
}
#endif
