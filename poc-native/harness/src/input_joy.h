#pragma once
/* input_joy.h - W221 HOTAS (experimentell): beliebige Joysticks, HOTAS,
 * Lenkraeder und Pedale ueber DirectInput 8 (dinput8.dll, dynamisch
 * geladen - ohne JOY-Quellen im Profil wird die DLL nie angefasst).
 *
 * Warum DirectInput 8 statt Raw Input/HID: DirectInput liefert fuer jedes
 * HID-Spielgeraet fertig kalibrierte Achsen (Bereich per DIPROP_RANGE),
 * POV-Hats, Tastenanzahl, Produktname und stabile GUIDs, liest im
 * Hintergrund ohne Fenster-Fokus und braucht keinen eigenen Parser fuer
 * HID-Report-Deskriptoren (HidP_* je Geraet waere deutlich fehleranfaelliger).
 * Doppelerfassung wird vermieden: XInput-Geraete (Pfad enthaelt "IG_")
 * und der Original-Controller (VID 0x0A7B, PID 0xD000; laeuft ueber
 * WinUSB) werden bei der Aufzaehlung uebersprungen - beide bleiben in ihren
 * bestehenden Pfaden.
 *
 * Wird von sb_emu (Hook, Hintergrund-Thread) und vom Launcher (Geraeteliste,
 * Live-Anzeige, Lernmodus) gleichermassen benutzt. Nur C + Win32.
 * Doku: poc-native/docs/EINGABE.md Abschnitt 15.
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IJOY_AXES 8    /* X Y Z RX RY RZ S0 S1 */
#define IJOY_BTNS 128
#define IJOY_POVS 4
#define IJOY_MAX_DEV 16 /* aufgezaehlte Geraete */
#define IMAP_JOY_MAX 4  /* Profil-Slots JOY0..JOY3 */

/* Zustand eines Geraets (Abbild DIJOYSTATE2, reduziert). Achsen
 * -32768..32767 (DIPROP_RANGE), fehlende Achsen bleiben 0. pov:
 * Hundertstel Grad 0..35999, -1 = Mitte. */
typedef struct {
    int16_t axis[IJOY_AXES];
    uint32_t btn[IJOY_BTNS / 32];
    int16_t pov[IJOY_POVS];
} imap_joy_state_t;

typedef struct {
    char name[96];    /* Instanzname (UTF-8/ASCII) */
    char product[96]; /* Produktname */
    char guid[40];    /* Instanz-GUID {xxxxxxxx-...} */
    char pguid[40];   /* Produkt-GUID (Data1 = PID<<16 | VID) */
    unsigned axmask;  /* Bit i = Achse i vorhanden */
    int nbtn, npov;
    /* W282 ISSUE6: Diagnose fuer die Geraeteliste. */
    int ncaps_axes;   /* Achsen laut Treiber (DIDEVCAPS.dwAxes) */
    unsigned axnorm;  /* Bit i = Achse i per Software auf -32768..32767
                       * umgerechnet (Treiber nahm DIPROP_RANGE nicht an) */
    int axremap;      /* 1 = Achsen per Objekt-Liste zugeordnet (Treiber
                       * meldete Achsen ausserhalb der Standard-GUIDs) */
    /* W312 JOY-CALIB: Geraeteinfo (nur Metadaten, nie Eingabewerte). */
    unsigned short vid, pid; /* aus der Produkt-GUID */
    unsigned devtype, capflags; /* DIDEVCAPS.dwDevType / dwFlags */
    int ncaps_btn, ncaps_pov;   /* laut Treiber (ungekuerzt) */
    int fmt;          /* 0 = Standard-Format, 1 = Achsen-Rueckfall (W282),
                       * 2 = eigenes Format je Objekt (W312) */
    long open_hr;     /* letzter Fehlercode von Acquire (0 = ok) */
} ijoy_info_t;

/* W312 JOY-CALIB: ein DirectInput-Objekt des Geraets - NUR Metadaten
 * (Typ, Usage, Flags, Bereich, wohin es gelesen wird), nie Werte. */
#define IJOY_OBJ_MAX 64
enum {
    IJOY_K_X = 0, IJOY_K_Y, IJOY_K_Z, IJOY_K_RX, IJOY_K_RY, IJOY_K_RZ,
    IJOY_K_SLIDER = 6, /* GUID_Slider */
    IJOY_K_AXIS = 7,   /* Achse mit unbekannter GUID (Vendor-Usage, Aileron, ...) */
    IJOY_K_POV = 8,
    IJOY_K_BTN = 9,
    IJOY_K_OTHER = 10  /* Kollektion, Ausgabe/Aktor ohne Daten, ... */
};
typedef struct {
    int kind;                    /* IJOY_K_* */
    unsigned type, flags;        /* dwType, dwFlags (DIDFT_* / DIDOI_*) */
    unsigned short upage, usage; /* HID-Usage (0 = unbekannt) */
    char name[48];               /* tszName (UTF-8) */
    long lo, hi;                 /* DIPROP_RANGE nach Einrichtung (Achsen) */
    long llo, lhi;               /* DIPROP_LOGICALRANGE (lo == hi: unbekannt) */
    int read;                    /* gelesen in: Achsen-Slot 0..7, POV 0..3,
                                  * Taste 0..127; -1 = nicht gelesen */
} ijoy_obj_t;

/* W312 (rein, testbar): Was das Standard-Format (GUID X..RZ, 2 Slider,
 * 4 POV, 128 Tasten, je "erstes freies passendes Objekt") von dieser
 * Objektliste erfasst. read[i] wie ijoy_obj_t.read. Rueckgabe Achsmaske. */
unsigned ijoy_std_capture(const ijoy_obj_t *o, int n, int *read);
/* W312 (rein, testbar): eigener Plan je Objekt - JEDE Achse (auch ohne
 * Standard-GUID) auf einen Slot (ijoy_plan_axes), POVs der Reihe nach,
 * Tasten der Reihe nach. Rueckgabe Achsmaske. */
unsigned ijoy_plan_objects(const ijoy_obj_t *o, int n, int *read);
/* W312 (rein): 1 = der Plan liest mehr Objekte als das Standard-Format
 * (dann eigenes Format je Objekt), 0 = Standard genuegt (bitgleich). */
int ijoy_need_own_format(const ijoy_obj_t *o, int n);
/* W312: Objektliste eines geoeffneten Geraets (Anzahl, *out bleibt gueltig
 * bis zur naechsten Aufzaehlung). */
int ijoy_objects(int i, const ijoy_obj_t **out);
/* W312 (rein): Geraeteinfo als Text fuer "Geraeteinfo kopieren" und das
 * Diagnosepaket - Name, VID/PID, Zahlen, Objektliste. NIE Eingabewerte.
 * Rueckgabe Laenge. */
int ijoy_devinfo_text(const ijoy_info_t *in, const ijoy_obj_t *o, int n,
                      char *out, int cap);

/* W312 JOY-CALIB: Kalibrierung je Geraet (VID/PID) und Achsen-Slot. Werte
 * im normierten Bereich -32768..32767 (nach DIPROP_RANGE). mid =
 * IJOY_CAL_NOMID: linear lo..hi (Schubhebel/Pedal), sonst Mitte. dz =
 * Totzone in Prozent (0..50; mittig um die Mitte, linear am lo-Ende).
 * Ohne Eintrag bleibt jeder Wert bitgleich. */
#define IJOY_CAL_NOMID (-99999)
#define IJOY_CAL_MAX 32
typedef struct {
    int lo, mid, hi, dz, inv;
} ijoy_cal_t;
int16_t ijoy_cal_apply(const ijoy_cal_t *c, int16_t v);
/* "046D:C283,X,lo,mid|-,hi,dz,inv" <-> Eintrag. 1 = ok. */
int ijoy_cal_parse(const char *val, unsigned short *vid, unsigned short *pid,
                   int *axis, ijoy_cal_t *c);
int ijoy_cal_format(unsigned short vid, unsigned short pid, int axis,
                    const ijoy_cal_t *c, char *out, int cap);
/* Tabelle (Client: aus user.cfg joy_calN=..., Launcher: Dialog). c NULL =
 * Eintrag entfernen. 0 = ok, -1 = voll/ungueltig. */
int ijoy_cal_set(unsigned short vid, unsigned short pid, int axis,
                 const ijoy_cal_t *c);
int ijoy_cal_get(unsigned short vid, unsigned short pid, int axis,
                 ijoy_cal_t *c); /* 1 = vorhanden */
int ijoy_cal_count(void);
int ijoy_cal_entry(int k, unsigned short *vid, unsigned short *pid, int *axis,
                   ijoy_cal_t *c); /* 1 = ok */
void ijoy_cal_clear(void);

/* W282 ISSUE6 (rein, testbar): Rohwert aus dem Geraetebereich lo..hi auf
 * -32768..32767; lo/hi = -32768/32767 ist bitgleich (Wert nur begrenzt). */
int16_t ijoy_norm_axis(long v, long lo, long hi);
/* W282 ISSUE6 (rein, testbar): Achsen-Objekte eines Geraets auf die 8
 * Slots verteilen. kind: 0..5 = X Y Z RX RY RZ, 6 = Slider, 7 = sonstige
 * Achse (unbekannte GUID). Standard-Achsen auf ihren Slot, Slider auf
 * S0/S1, Rest bzw. Doppelte auf freie Slots (S0, S1, Z, RX, RY, RZ, X,
 * Y). slot[i] = Slot von Objekt i oder -1 (kein Platz). Rueckgabe Maske. */
unsigned ijoy_plan_axes(const int *kind, int n, int *slot);
/* W282 ISSUE6: Xbox-kompatible Geraete (XInput), die die letzte
 * Aufzaehlung bewusst uebersprungen hat (sie laufen ueber den Pad-Pfad
 * PAD:...). Rueckgabe Anzahl, Namen in names (max Eintraege). */
int ijoy_skipped_xinput(char names[][96], int max);

/* Achsen-/POV-Namen fuer das Profil ("X","Y","Z","RX","RY","RZ","S0","S1"). */
const char *ijoy_axis_name(int ax);
int ijoy_axis_parse(const char *name); /* -1 = unbekannt */

/* Spec-Abgleich (rein, testbar): spec = "{GUID}" (Instanz- oder
 * Produkt-GUID), sonst Teilstring von Name/Produkt (ohne Gross/Klein),
 * optional "@k" = k-ter Treffer (1-basiert). Leere spec = Geraet Nummer
 * 'slot' in Aufzaehlungsreihenfolge. Rueckgabe Index oder -1. */
int ijoy_match_list(const ijoy_info_t *list, int n, const char *spec,
                    int slot);

/* ---- Geraete (synchron; Launcher und Client-Thread) ---- */
int ijoy_init(void);       /* dinput8 laden; 0 = ok, -1 = nicht verfuegbar */
int ijoy_scan(void);       /* (neu) aufzaehlen + oeffnen; Anzahl */
int ijoy_count(void);
const ijoy_info_t *ijoy_info(int i);
int ijoy_read(int i, imap_joy_state_t *st); /* 1 = ok, 0 = verloren */
/* W312: wie ijoy_read, zusaetzlich (raw != NULL) der Stand OHNE
 * Kalibrierung (Launcher: Live-Ansicht, Kalibrieren). Nur Anzeige. */
int ijoy_read2(int i, imap_joy_state_t *st, imap_joy_state_t *raw);
void ijoy_shutdown(void);

/* ---- Client: Slot-Leser im Hintergrund-Thread ----
 * Startet einen Thread (Poll ~4 ms, Neu-Aufzaehlung alle 3 s solange ein
 * belegter Slot kein Geraet hat). Der Hook liest nur den letzten Stand
 * (nie blockierend). specs[i] wie ijoy_match_list, used = Bitmaske der
 * im Profil benutzten Slots. 0 = gestartet, -1 = dinput8 fehlt. */
int ijoy_slots_start(const char specs[IMAP_JOY_MAX][96], unsigned used);
/* Letzter Stand: Rueckgabe Bitmaske der Slots mit Geraet. */
unsigned ijoy_slots_get(imap_joy_state_t out[IMAP_JOY_MAX]);
/* Fuer Log: Name des Geraets in Slot i ("" = keins). */
void ijoy_slot_name(int i, char *out, int cap);

/* W305 (JOY-HOTPLUG): Test-Schnittstelle - NUR Selbsttest (input_selftest).
 * Ersetzt DirectInput durch nfake Fake-Geraete und fuehrt den Slot-Takt mit
 * simulierter Zeit (kein Thread). Nie im Spieler- oder Launcher-Pfad. */
typedef struct {
    unsigned scans, opens, releases, read_fails;
    unsigned bad_release, bad_read; /* Doppel-Freigabe / Lesen nach Freigabe */
} ijoy_test_stats_t;
void ijoy_test_begin(int nfake, const char specs[IMAP_JOY_MAX][96], unsigned used);
void ijoy_test_plug(int k, int present); /* 0 = abziehen, 1 = einstecken (neue Generation) */
void ijoy_test_axis(int k, long v);
void ijoy_test_run(unsigned ms);
void ijoy_test_stats(ijoy_test_stats_t *out);
void ijoy_test_end(void);
/* W312: Fake-Geraet k mit dieser Objektliste (Wingman/Hori/HOTAS) statt
 * fester X/Y - Aufzaehlung, Format-Entscheidung, Lesen ueber den Plan.
 * Werte je Objekt per ijoy_test_obj_value (Achse: Rohwert im Objekt-
 * bereich lo..hi, Taste 0/1, POV Hundertstel Grad / -1). */
void ijoy_test_objects(int k, const ijoy_obj_t *o, int n);
void ijoy_test_obj_value(int k, int obj, long v);

#ifdef __cplusplus
}
#endif
