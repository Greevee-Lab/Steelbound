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
} ijoy_info_t;

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

#ifdef __cplusplus
}
#endif
