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
/* --- W228 BIND-DIALOG: Darstellungs-Helfer (rein, testbar) ------------- */
/* Gruppe einer Aktion fuer die gegliederte Liste (Reihenfolge = Anzeige):
 * 0 Tasten (bi0-38), 1 Achsen, 2 Tasten fuer Achsen (.neg/.pos), 3 Gaenge,
 * 4 Tuner, 5 Client-Aktionen. -1 = ungueltig. */
#define LCTRL_GROUP_N 6
int lctrl_elem_group(int idx);
/* Achse zu einem Tasten-Helfer ("rot.pos" -> Index von "rot"), sonst -1. */
int lctrl_helper_axis(int idx);
/* Sichtbarkeit im Detailbereich der gewaehlten Aktion (Bitmaske). */
#define LCTRL_VIS_MOUSE 1   /* Maus X/Y: nur Achsen aim_* / sight_* */
#define LCTRL_VIS_JOY 2     /* Joystick-Einstellungen: JOY-Achsenquelle da */
#define LCTRL_VIS_JOYDZ 4   /* Totzone: analoge JOY-Achsenquelle da */
#define LCTRL_VIS_JOYHINT 8 /* Hinweis: Achse ohne JOY-Achsenquelle */
unsigned lctrl_bind_detail_vis(const lctrl_bind_t *b, int idx);
/* Eine Quelle (Position ai) entfernen, Rest rueckt auf. 0 = ok, 1 = nein. */
int lctrl_bind_remove_alt(lctrl_bind_t *b, int idx, int ai);
/* Konflikte nur fuer eine Aktion: je eigener Quelle die anderen Aktionen
 * mit derselben Quelle. Rueckgabe = Anzahl Eintraege (0 = keine). */
typedef struct {
    int alt;      /* Position der Quelle in der Zeile idx */
    int n;        /* Anzahl anderer Aktionen */
    int other[8]; /* deren Indizes */
} lctrl_conf1_t;
int lctrl_bind_conflicts_for(const lctrl_bind_t *b, int idx,
                             lctrl_conf1_t *out, int max);
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
 * bzw. "Z:zone=lo-hi" (W226-Regeln s. u.). out = Teil hinter "JOYn:".
 * Rueckgabe 1 = Taste, 4 = Zone mit Ruhestellung abgelehnt,
 * 2 = POV, 3 = Achse, 0 = nichts. */
int lctrl_joy_learn(const imap_joy_state_t *base, const imap_joy_state_t *prev,
                    const imap_joy_state_t *cur, unsigned axmask, int npov,
                    int idx, char *out, size_t cap);
/* W226 JOY-AXIS-DIR: verlangte Lern-Richtung je Element: +1 = rechts /
 * Pedal treten / Gas (Wert steigt), -1 = nach vorn/oben (Zielhebel Y,
 * Sicht Y; DirectInput-Minimum), 0 = kein Achsen-Element. Lernen gegen die
 * Richtung setzt inv=1; Pedal aus Mittelstellung zusaetzlich half=1.
 * Tasten/.pos/.neg lernen nur die Richtung ("Z+"/"Z-" bzw. Zone ab halbem
 * Weg), Zonen um die Stellung nur gear.r/n/1..5; eine Zone, die die
 * Ruhestellung enthielte, wird abgelehnt (lctrl_joy_learn Rueckgabe 4). */
int lctrl_joy_dir_expect(int idx);
/* Hinweistext (LT) fuer den Lern-Prompt, NULL = keiner. */
const char *lctrl_joy_dir_hint(int idx);
/* Akkord-'+' wie input_map.c (W226): '+' vor Ende/':'/'+' gehoert zum
 * Namen ("JOY0:Z+"). Zeiger auf das Trenn-'+' oder NULL. */
const char *lctrl_chord_plus(const char *s);
/* W226: Lernmodus ueber mehrere Abtastungen (50 ms): Basis erst nach 3
 * gueltigen, ruhigen Lesungen (Lesefehler = kein Lernen); sind alle Achsen
 * exakt gleich (Startwert vor dem ersten Geraete-Bericht), bestaetigt erst
 * die erste Aenderung je Achse die Basis; steht eine Achse exakt auf
 * einem Endwert und springt in einer Abtastung um > 25 %, gilt der Sprung
 * ebenfalls als erster Bericht (neue Basis). Achse wird erst uebernommen,
 * wenn sie >= 40 % ausgelenkt und 2 Abtastungen ruhig ist (bzw. mit der
 * Spitzenstellung, wenn sie zurueckfedert). */
typedef struct {
    imap_joy_state_t base, prev;
    int16_t peak[IJOY_AXES];
    int peakd[IJOY_AXES];
    unsigned unconf, unext;
    int settle, have_base, stable, cand;
} lctrl_joy_learner_t;
void lctrl_joy_learner_reset(lctrl_joy_learner_t *L);
/* ok = Lesen erfolgreich. Rueckgabe wie lctrl_joy_learn (0 = weiter). */
int lctrl_joy_learner_step(lctrl_joy_learner_t *L, int ok,
                           const imap_joy_state_t *cur, unsigned axmask,
                           int npov, int idx, char *out, size_t cap);
/* Einzelne JOY-Achsenquelle: 1 = analog ("JOY0:X:dz=.."), 2 = Achse als
 * Taste ("Z+", "Z-", "Z:zone=a-b"), 0 = keine. slot/axis optional. */
int lctrl_joy_src_axis(const char *alt, int *slot, int *axis);
/* Vorschau wie im Client (input_map.c imap_joy_value bzw. Zone): raw =
 * DirectInput-Wert. Analog: *res -1..1 (pedal: 0..1), Rueckgabe 1; Taste:
 * *res 0/1, Rueckgabe 2; 0 = keine JOY-Achsenquelle. */
int lctrl_joy_eval(const char *alt, int pedal, int raw, float *res);
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
