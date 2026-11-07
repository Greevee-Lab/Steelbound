#pragma once
/* sbc_test.h - Gemeinsamer Testkern: gefuehrte Controller-Pruefung (W143).
 *
 * Enthaelt Testschritte, Erkennung, Lampenfolge und Ergebnisdatei; wird von
 * sbc_probe (Konsole) und vom Launcher-Assistenten ("Steuerung -> Controller
 * testen & kalibrieren") gemeinsam genutzt (kein doppelter Code).
 *
 * Das Modul ist ein-/ausgabefrei: Ereignisquelle (Geraet/Fixture/Skript),
 * Uhr, Schlaf, OUT-Versand und Ausgabe kommen ueber sbc_test_io_t vom
 * Aufrufer. Keine globalen Zustaende (ein Kontext je Lauf, Thread-Nutzung
 * durch genau einen Arbeiter-Thread des Aufrufers).
 *
 * W143-Neutralitaet: Mitgedrueckte Tasten sind Bedienung, kein Befund.
 * Kommt das erwartete Bit UND gleichzeitig andere ("Ueberlappung"), bleibt
 * der Schritt OK, die Notiz nennt neutral "weitere Tasten gesehen: ...".
 * Nur "kam stattdessen" (erwartet fehlt) bleibt "unerwartet".
 */
#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "input_sbc_usb.h"

/* ---- Ereignismodell (Quelle -> Kern) ---- */

typedef struct {
    uint64_t t;       /* ms (echt oder virtuell) */
    int key;          /* 0 = Report, sonst Taste ('\r' = Enter, 'j', 'n', 0x1B) */
    unsigned len;
    uint8_t raw[SBC_USB_RAW_MAX];
} sbc_test_ev_t;

enum {
    SBC_TEV_REPORT = 1,
    SBC_TEV_KEY,
    SBC_TEV_TIMEOUT,
    SBC_TEV_ERR
};

/* Fortschritt (strukturiert, fuer Oberflaechen; Konsole: NULL-tolerant). */
enum {
    SBC_TP_STEP_START = 1, /* a = Schrittindex, b = Schrittart */
    SBC_TP_STEP_DONE,      /* a = Schrittindex, b = Status */
    SBC_TP_LED             /* a = Folgenindex 0..38 (Lampenfolge), b = reserviert */
};

typedef struct sbc_test_io {
    void *ctx;
    uint64_t (*now)(void *ctx);
    int (*next)(void *ctx, uint64_t deadline, sbc_test_ev_t *out);
    void (*sleep_ms)(void *ctx, unsigned ms);
    int (*send_out)(void *ctx, const uint8_t out[SBC_USB_OUT_LEN]);
    /* Aufgezeichnete OUT-Reports (Simulation/Mock; live: nout = 0, out_at NULL). */
    unsigned (*nout)(void *ctx);
    const uint8_t *(*out_at)(void *ctx, unsigned i);
    int (*is_live)(void *ctx);
    int (*has_ended)(void *ctx);  /* Quelle erschoepft (Simulation) */
    int (*err)(void *ctx);        /* letzter Fehler (SBC_ERR_*) */
    int (*aborted)(void *ctx);    /* Abbruchwunsch (Ctrl+C / Stopp-Knopf) */
    /* Ein GET_REPORT-Versuch (Fallback, nur Live-Geraet; NULL = nicht
     * verfuegbar). 1 = *len Bytes in raw gueltig. */
    int (*get_report)(void *ctx, uint8_t raw[SBC_USB_RAW_MAX],
                      unsigned *len);
    void (*emit)(void *ctx, const char *text); /* eine fertige Zeile (mit \n) */
    void (*progress)(void *ctx, int ev, int a, int b); /* NULL-tolerant */
    void (*discard_keys)(void *ctx); /* getippte Tasten verwerfen (Konsole) */
} sbc_test_io_t;

/* ---- Namen/Tabellen (CONTROLLER.md Abs.3.1/Abs.8) ---- */

extern const char *const sbc_test_btn_name[39];
extern const char *const sbc_test_btn_hint[39];
extern const char *const sbc_test_led_name[SBC_LED_COUNT];

#define SBC_TEST_TOG_FIRST 34
#define SBC_TEST_TOG_MASK (0x1FULL << SBC_TEST_TOG_FIRST)

/* 39 Tastenbits aus dem Rohreport (Bytes 2..6, Bit 39 = 6.7 nicht dabei). */
uint64_t sbc_test_raw_btns(const uint8_t *r);

/* Analogfelder im Rohreport (u16 LE). kind: 0 = u16 zentriert (Aiming),
 * 1 = s16 zentriert, 2 = Pedal (u16 ab 0). */
typedef struct {
    const char *name;
    int off;        /* Report-Byte (low) */
    int kind;
    int cal_axis;   /* SBC_AX_* */
} sbc_test_axisdef_t;

extern const sbc_test_axisdef_t sbc_test_axes[8];

/* Achswert als int: u16 (0..65535) bzw. s16 (-32768..32767). */
int sbc_test_raw_axis(const uint8_t *r, int a);
/* Ruhemitte in Spiel-Einheiten, gerundet (Rohwert-Mitte / 256). */
int sbc_test_game_units_round(int a, int v);
/* Nennmitte (Ruhe) je Achse im Rohwert. */
int sbc_test_axis_nominal_rest(int a);
/* High-Byte = Spiel-Einheit (Pedal/Aiming 0..255, s16 -128..127). */
int sbc_test_game_units(int a, int v);

/* Gang-Anzeigename ("R"/"N"/"1".."5"/"dazwischen"/"?"). */
const char *sbc_test_gear_name(int g);

/* ---- Mitschnitt unsicherer Report-Bytes (CONTROLLER.md Abs.10) ---- */

typedef struct {
    unsigned n;
    unsigned bad_len, bad_hdr;
    unsigned len_min, len_max;
    uint8_t b7[256];
    uint8_t b6h[2];
    uint8_t b24h[16];
    uint8_t lo_seen[8][256];
    uint8_t tail_nz;
} sbc_test_unk_t;

void sbc_test_unk_add(sbc_test_unk_t *u, const uint8_t *r, unsigned len);
void sbc_test_unk_report(const sbc_test_unk_t *u,
                         void (*pf)(void *opaque, const char *fmt, ...),
                         void *opaque);

/* ---- Lampenfolge ---- */

typedef struct {
    int byte;       /* OUT-Byte 2..21 */
    int hi;         /* 1 = High-Nibble */
    int led;        /* SBC_LED_* oder -1 (reserviert) */
    const char *name;
} sbc_test_led_slot_t;

#define SBC_TEST_LED_SEQ_N (SBC_LED_COUNT + 2)

/* Schritt k (0..38) der Folge: 37 Lampen in OUT-Reihenfolge, danach die
 * zwei reservierten Stellen (Byte 17 low, Byte 21 low - sollten dunkel
 * bleiben). */
void sbc_test_led_seq_slot(int k, sbc_test_led_slot_t *sl);
/* OUT-Report fuer Schritt k (k < 0 oder >= Folge: alles aus). */
void sbc_test_led_seq_report(int k, uint8_t out[SBC_USB_OUT_LEN]);
/* Folge abspielen (step_ms je Lampe). Rueckgabe SBC_OK oder Sendefehler. */
int sbc_test_led_seq_run(const sbc_test_io_t *io, unsigned step_ms,
                         int with_reserved);
/* Aufgezeichnete Folge gegen das OUT-Format pruefen (Mock/Selbsttest).
 * Rueckgabe Anzahl Fehler; msg = erster Fehler. */
int sbc_test_led_seq_verify(const sbc_test_io_t *io, int with_reserved,
                            char *msg, size_t cap);
/* dto., aber erst ab dem skip-ten aufgezeichneten Report (fuer den
 * Lampenschritt: nur die eigene Folge pruefen). */
int sbc_test_led_seq_verify_skip(const sbc_test_io_t *io, unsigned skip,
                                 int with_reserved, char *msg, size_t cap);

/* ---- Gefuehrte Pruefung ---- */

enum {
    SBC_TK_BTN = 0, SBC_TK_TOG, SBC_TK_AXIS, SBC_TK_TUNER, SBC_TK_GEAR,
    SBC_TK_LED
};
enum {
    SBC_TR_OK = 0, SBC_TR_MISSING, SBC_TR_UNEXPECTED, SBC_TR_PARTIAL,
    SBC_TR_NOTRUN
};

const char *sbc_test_resname(int status);

typedef struct {
    int kind, idx;
} sbc_test_step_t;

typedef struct {
    int status;
    char note[200];
    /* Achsen (Rohwerte) */
    int measured;
    int rest_lo, rest_hi, mn, mx;
    int full_lo, full_hi;
} sbc_test_res_t;

#define SBC_TEST_STEP_TIMEOUT_MS 60000u
#define SBC_TEST_N_STEPS (34 + 5 + 8 + 1 + 1 + 1)

/* Schrittliste fuellen (34 Tasten, 5 Schalter, 8 Achsen, Tuner, Gang,
 * Lampen). Rueckgabe = Anzahl (SBC_TEST_N_STEPS). */
int sbc_test_build_steps(sbc_test_step_t *st);

/* Schrittanzeige ("Taste Fire (bi1)", "Achse Rotation", ...). */
const char *sbc_test_step_name(const sbc_test_step_t *st, char *buf,
                               size_t cap);

typedef struct {
    const sbc_test_io_t *io;
    uint8_t cur[SBC_USB_RAW_MAX];
    unsigned len;
    sbc_test_unk_t unk;
    int aborted;
    int skip_key;   /* letzter Schritt per Enter beendet */
    uint64_t t_first, n_rep;
    int gear_seq[32], gear_nseq;
    int used_getreport; /* Start per GET_REPORT (keine Leerlauf-Reports) */
} sbc_test_ctx_t;

/* Kontext an eine Quelle binden (kein Report gelesen). */
void sbc_test_ctx_init(sbc_test_ctx_t *c, const sbc_test_io_t *io);

/* Ersten Report holen (bis 1,5 s warten). 1 = c->cur gueltig. */
int sbc_test_first_report(sbc_test_ctx_t *c);

/* Einen Schritt (blockierend) fahren. */
void sbc_test_step_button(sbc_test_ctx_t *c, int bi, int toggle,
                          sbc_test_res_t *r);
void sbc_test_step_axis(sbc_test_ctx_t *c, int a, sbc_test_res_t *r);
void sbc_test_step_tuner(sbc_test_ctx_t *c, sbc_test_res_t *r,
                         uint8_t seen[16]);
void sbc_test_step_gear(sbc_test_ctx_t *c, sbc_test_res_t *r,
                        uint8_t seen[256]);
void sbc_test_step_led(sbc_test_ctx_t *c, sbc_test_res_t *r);

/* cfg-Vorschlag je Achse (Spiel-Einheiten, Rand 2 nach innen). 1 = Zeilen. */
int sbc_test_axis_suggest(int a, const sbc_test_res_t *r, char *cal,
                          size_t ccap, char *dead, size_t dcap);
/* Kodierungs-Urteil (Aiming u16? Ruhe?). */
const char *sbc_test_axis_verdict(int a, const sbc_test_res_t *r);
/* Vorschlag sbc_gear_map aus dem Gang-Schritt (nur bei echtem Widerspruch).
 * 1 = Zeile. */
int sbc_test_gear_suggest(const sbc_test_ctx_t *c, char *out, size_t cap);

/* Ergebnis-Zaehler je Status (fuer Selbsttest/Oberflaeche). */
typedef struct {
    int n[5];
    int steps;
    char path[260];
} sbc_test_sum_t;

/* Alle Schritte fahren + Ergebnisdatei schreiben (Format wie
 * sbc_test_<datum>.txt). out_path = NULL waehlt sbc_test_<stempel>.txt
 * neben der exe. tool_tag/version erscheinen in der Kopfzeile
 * (z. B. "sbc_probe --test"/"W143", "Launcher-Assistent"/"W143").
 * Rueckgabe 0 = alles OK, sonst 1. */
int sbc_test_run_all(const sbc_test_io_t *io, const char *out_path,
                     const char *src_desc, const char *tool_tag,
                     const char *version, sbc_test_sum_t *sum);

/* Nur Ergebnisdatei schreiben (fuer schrittweise Fahrer wie den
 * Launcher-Assistenten). steps/res muessen SBC_TEST_N_STEPS Eintraege
 * haben (nicht gelaufene = SBC_TR_NOTRUN). tun_seen/gear_seen duerfen
 * NULL sein (dann ohne diese Details). */
int sbc_test_write_report(const sbc_test_ctx_t *c,
                          const sbc_test_step_t *steps,
                          const sbc_test_res_t *res, const char *out_path,
                          const char *src_desc, const char *tool_tag,
                          const char *version, int aborted,
                          sbc_test_sum_t *sum);

/* Zeitstempel ("%Y%m%d_%H%M%S" bzw. "%Y-%m-%d %H:%M:%S"). */
void sbc_test_stamp(char *out, size_t cap, int file_fmt);

#ifdef __cplusplus
}
#endif
