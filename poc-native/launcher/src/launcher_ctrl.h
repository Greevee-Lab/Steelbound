#pragma once
/* launcher_ctrl.h - Steuerungs-Helfer des Launchers (W78, CTRL-UI).
 * Eingabe-Profile auflisten, SBC-USB-/Gamepad-Status, Belegungs-Modell
 * fuer den Belegungs-Dialog (Namen wie docs/EINGABE.md). ASCII only. */
#include "input_joy.h" /* W221: ijoy_info_t, imap_joy_state_t, IMAP_JOY_MAX */

#ifdef __cplusplus
extern "C" {
#endif

/* --- Eingabe-Profile (*.ini im Client-Ordner profiles/) --------------- */
/* Namen ohne Endung, alphabetisch. Rueckgabe = Anzahl (0 = keine).
 * Fehlt der Ordner, ist das kein Fehler (nur Legacy). */
int lctrl_input_profiles(const char *client_dir, char names[][64], int max);
/* Pfad profiles/<name>.ini im Client-Ordner. 0 = ok, 1 = ungueltig. */
int lctrl_input_profile_path(const char *client_dir, const char *name,
                             char *out, size_t cap);

/* --- Benutzerprofil-Namen: Datei user_<rest>.ini ---------------------- */
/* rest: 1..40 Zeichen aus [A-Za-z0-9_.-]. 1 = ok, 0 = nein. */
int lctrl_user_rest_ok(const char *rest);
/* Dateiname "user_<rest>.ini" (ohne Pfad). 0 = ok, 1 = ungueltig. */
int lctrl_user_filename(const char *rest, char *out, size_t cap);
/* 1 = mitgeliefertes Standardprofil (nie ueberschreiben). */
int lctrl_name_shipped(const char *name);

/* --- SBC-USB-Status ---------------------------------------------------- */
/* Geraet kurz oeffnen (wie sbc_probe) und wieder schliessen.
 * Rueckgabe 0 = bereit, sonst SBC_ERR_* (input_sbc_usb.h). Text in out. */
int lctrl_sbc_status(char *out, size_t cap);

/* --- Gamepad (XInput, dynamisch geladen, kein Treiber) ----------------- */
typedef struct {
    unsigned short buttons; /* XInput-Bits (DU=1, DD=2, DL=4, DR=8, ...) */
    unsigned char lt, rt;   /* Trigger 0..255 */
    short lx, ly, rx, ry;   /* Sticks -32768..32767 */
} lctrl_pad_t;

/* Port 0..3 abfragen. 1 = verbunden (out gueltig), 0 = nein. */
int lctrl_pad_poll(int port, lctrl_pad_t *out);
/* Erster verbundener Port 0..3, -1 = keiner. */
int lctrl_pad_first(lctrl_pad_t *out);
/* Bit -> Name ("A", "DU", ...), NULL bei unbekannt. */
const char *lctrl_pad_button_name(unsigned bit);
/* Live-Zeile: Sticks -1..+1, Trigger 0..1, Tasten. */
void lctrl_pad_format(const lctrl_pad_t *p, char *out, size_t cap);
/* Neu gedrueckte Taste (Flanke prev -> cur, Trigger ab 50 %).
 * 1 = gefunden (Name in name, z. B. "A", "DU", "LT"), 0 = nichts. */
int lctrl_pad_new_press(const lctrl_pad_t *prev, const lctrl_pad_t *cur,
                        char *name, size_t cap);
/* Ausgelenkte Analogachse (ein Stick > 0.7, Rest ruhig): Name in name
 * ("LX", "LY", "RX", "RY", "LT", "RT"). 1 = gefunden, 0 = nichts. */
int lctrl_pad_deflected(const lctrl_pad_t *p, char *name, size_t cap);

/* --- Tasten-Namen (wie input_map.c imap_key_code, Rueckrichtung) ------- */
/* VK -> "A", "SPACE", "F5", sonst dezimal ("165"). */
void lctrl_key_name(int vk, char *out, size_t cap);

/* --- Belegungs-Modell (ein Profil im Dialog) --------------------------- */
typedef enum {
    LCTRL_DIG,  /* Taste: bi0-38, gear, tuner */
    LCTRL_AXIS, /* Analogachse: aim/rot/sight/pedal */
    LCTRL_NEG,  /* digitale Negativ-Endlage (.neg) */
    LCTRL_POS,  /* digitale Positiv-Endlage (.pos) */
    LCTRL_ACT   /* W159: Client-Aktion (kein Controller-Element) */
} lctrl_kind_t;

typedef struct {
    const char *key;   /* Profil-Schluessel ("bi0", "aim_x.neg", ...) */
    const char *klar;  /* Klarname ("mainweapon", "-" bei Achsen) */
    const char *lkey; /* Sprachschluessel ("bind.bi0"), Anzeige via LT() */
    lctrl_kind_t kind;
} lctrl_elem_t;

#define LCTRL_ELEM_N 73 /* W188: + Client-Aktion pause, W195: + photo */
extern const lctrl_elem_t kCtrlElems[LCTRL_ELEM_N];
/* W159: Original-Controller-Tasten als Belegungs-Quellen (SBC:<name>).
 * Kanonische Namen bi0..38, parserstabil wie input_map.c (kBiAlias).
 * bi -> Name ("CHAFF"), NULL bei ungueltig. */
const char *lctrl_sbc_btn_name(int bi);

#define LCTRL_ALT_N 4
typedef struct {
    char alt[LCTRL_ALT_N][96];
    int n_alt;
} lctrl_row_t;

typedef struct {
    lctrl_row_t row[LCTRL_ELEM_N];
    /* W221 HOTAS: Geraete-Zuordnung "joyN = <spec>" je Slot (leer = keine
     * Zeile; dann gilt im Client das N-te aufgezaehlte Geraet). Wird beim
     * Laden/Speichern erhalten (Abschnitt [joystick]). */
    char joy[IMAP_JOY_MAX][96];
} lctrl_bind_t;

void lctrl_bind_init(lctrl_bind_t *b);
/* Datei parsen (Semantik wie imap_profile_parse: '#' inline, ';'/'[',
 * element = alt, alt; unbekannt = ignorieren). 0 = ok, 1 = fehlt. */
int lctrl_bind_load_file(const char *path, lctrl_bind_t *b);
/* W182: dasselbe fuer einen Profiltext im Speicher. 0 = ok, 1 = NULL. */
int lctrl_bind_load_text(const char *text, lctrl_bind_t *b);
/* Name -> Index (bi0/bi+N, Klarname, achse[.neg/.pos], gear.*, tuner.*).
 * -1 = unbekannt. */
int lctrl_bind_elem_index(const char *name);
/* Zeile ersetzen ("A, B" oder leer = loeschen). Immer 0. */
int lctrl_bind_set(lctrl_bind_t *b, int idx, const char *alts);
/* Alternative anhaengen. 0 = ok, 1 = voll/doppelt/ungueltig. */
int lctrl_bind_add(lctrl_bind_t *b, int idx, const char *alt);
void lctrl_bind_clear(lctrl_bind_t *b, int idx);
/* "KEY:SPACE, MOUSE:LEFT" (oder "(unbelegt)"). */
void lctrl_bind_row_text(const lctrl_bind_t *b, int idx, char *out,
                         size_t cap);
/* Gleiche Quelle auf mehreren Elementen: je "QUELLE: elem1, elem2".
 * Rueckgabe = Anzahl (0 = keine Konflikte). */
int lctrl_bind_conflicts(const lctrl_bind_t *b, char out[][160], int max);
/* W159: Alternative nutzt eine Original-Controller-Taste? 1 = "SBC:..."
 * (auch im Akkord "+SBC:..."), sonst 0. Jede SBC-Taste hat eine
 * Spielfunktion (CONTROLLER.md S8): Belegung nur mit Warnung. */
int lctrl_bind_alt_is_sbc(const char *alt);
/* Als Profil speichern (nur belegte Zeilen + Kopf). 0 = ok, 1 = Fehler. */
int lctrl_bind_save(const char *path, const lctrl_bind_t *b, const char *base,
                    char *err, size_t errcap);

/* --- W221 HOTAS (experimentell): Joystick-Quellen ---------------------- */
/* Lernmodus (rein, testbar): base = Stand beim Start des Lernens, prev =
 * letzter Poll, cur = aktueller Poll. Erkennt neue Taste ("B3"), neue
 * POV-Richtung ("POV1U") oder Achsen-Auslenkung >= 40 % des Wegs relativ
 * zu base (Schubhebel/Pedale ruhen nicht in der Mitte). Auf Achsen-
 * Elementen (idx LCTRL_AXIS) nur Achsen -> analoge Quelle "Z:dz=0.05"
 * (Pedal invertiert, wenn der Weg zum Minimum fuehrt); sonst "Z+"/"Z-"
 * bzw. "Z:zone=lo-hi". out = Teil hinter "JOYn:". Rueckgabe 1 = Taste,
 * 2 = POV, 3 = Achse, 0 = nichts. */
int lctrl_joy_learn(const imap_joy_state_t *base, const imap_joy_state_t *prev,
                    const imap_joy_state_t *cur, unsigned axmask, int npov,
                    int idx, char *out, size_t cap);
/* Slot fuer Geraet dev (Index in list): passende joyN-Zeile, sonst
 * implizit benutzter Slot (N-tes Geraet), sonst naechster freier Slot mit
 * neuer Zeile joyN = <Produktname>[@k] (bzw. {GUID}). -1 = alle belegt. */
int lctrl_joy_slot_for(lctrl_bind_t *b, const ijoy_info_t *list, int n,
                       int dev);
/* 1 = Profil benutzt Slot (JOYn:/JOY: in irgendeiner Alternative). */
int lctrl_bind_uses_joy_slot(const lctrl_bind_t *b, int slot);
/* 1 = alt ist eine einzelne analoge JOY-Achsenquelle ("JOY0:X:dz=..",
 * kein +/-/zone), slot/axis optional. */
int lctrl_joy_is_axis(const char *alt, int *slot, int *axis);
/* Parameter ":key=wert" lesen (1 = vorhanden) bzw. setzen/ersetzen
 * (val NULL/"" = entfernen). 0 = ok, 1 = passt nicht in cap. */
int lctrl_joy_param_get(const char *alt, const char *key, char *out,
                        size_t cap);
int lctrl_joy_param_set(char *alt, size_t cap, const char *key,
                        const char *val);
/* Lesbar: "JOY1:B3" -> "Joystick 2 Taste 3" (Sprache via LT). 1 = JOY-
 * Quelle, 0 = keine (out unveraendert). */
int lctrl_joy_atom_text(const char *atom, char *out, size_t cap);

/* --- input_selftest --dump pruefen ------------------------------------- */
/* Profil per Client-Tool parsen lassen. 0 = Exit 0 (geparst), Text in out. */
int lctrl_run_dump(const char *client_dir, const char *profile, char *out,
                   size_t cap);

#ifdef __cplusplus
}
#endif
