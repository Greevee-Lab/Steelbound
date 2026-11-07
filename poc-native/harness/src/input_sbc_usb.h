#pragma once
/* input_sbc_usb.h - Original-Steel-Battalion-Controller per WinUSB lesen.
 *
 * W49-Vorarbeit (Welle 4, CTRL-SBC): eigenstaendiges Modul ohne
 * Harness-Abhaengigkeit (nur C + Win32/WinUSB). Noch NICHT in
 * xdk_hooks.c verdrahtet (W44) - nur diese Schnittstelle + sbc_probe.
 * Protokoll: poc-native/docs/CONTROLLER.md.
 */
#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* USB-Kennung des Original-Controllers (xboxdevwiki, belegt). */
#define SBC_USB_VID 0x0A7B
#define SBC_USB_PID 0xD000

/* Report-Laengen (inkl. 2-Byte-Header ID+Laenge). */
#define SBC_USB_IN_LEN 26
#define SBC_USB_OUT_LEN 22

/* Fehlercodes (alle < 0, ausser ok). Ohne Geraet: sauberer Code, nie Absturz. */
enum {
    SBC_OK = 0,
    SBC_ERR_NODEVICE = -1, /* kein 0A7B:D000 am USB (oder WinUSB nicht gebunden) */
    SBC_ERR_DRIVER = -2,   /* Geraet da, aber WinUSB-Pfad scheitert (Zadig fehlt?) */
    SBC_ERR_IO = -3,       /* Transfer-/Pipe-Fehler am offenen Geraet */
    SBC_ERR_TIMEOUT = -4,  /* kein Report in der Frist (poll_dauert an) */
    SBC_ERR_BADREPORT = -5,/* Laenge/Header passt nicht (falsches Geraet?) */
    SBC_ERR_ARG = -6       /* NULL-Zeiger / falsche Pufferlaenge */
};

/* Neutraler Controller-Zustand (ein IN-Report, dekodiert). Buttons bi0..38
 * in exakt der Belegung des Spiel-SBC-Blocks (CONTROLLER.md §8):
 * buttons[0] = bi0..15, buttons[1] = bi16..31, buttons[2] = bi32..38. */
typedef struct {
    uint16_t buttons[3];
    uint8_t aiming_x, aiming_y;  /* 0..255, Mitte 0x80 */
    int8_t rot_lever;            /* -128..127, Mitte 0 */
    int8_t sight_x, sight_y;     /* dto. */
    uint8_t pedal_l, pedal_m, pedal_r; /* 0 = los, 0xFF = voll */
    uint8_t tuner;               /* 0..12 (13..15 = Blende, roh) */
    uint8_t gear;                /* 7=R, 8=N, 9..13 = 1..5, 0 = dazwischen */
    uint8_t reserved7;           /* Report-Byte 7 (reserviert, Diagnose) */
} sbc_state_t;

/* LED-Ziele am Controller (OUT-Nibbles, CONTROLLER.md §4). */
typedef enum {
    SBC_LED_EJECT = 0, SBC_LED_HATCH, SBC_LED_IGNITION, SBC_LED_START,
    SBC_LED_OPENCLOSE, SBC_LED_MAPZOOM, SBC_LED_MODE, SBC_LED_SUBMON,
    SBC_LED_ZOOMIN, SBC_LED_ZOOMOUT, SBC_LED_FSS, SBC_LED_MANIP,
    SBC_LED_LINECOLOR, SBC_LED_WASHING, SBC_LED_EXTING, SBC_LED_CHAFF,
    SBC_LED_TANKDETACH, SBC_LED_OVERRIDE, SBC_LED_NIGHTSCOPE, SBC_LED_F1,
    SBC_LED_F2, SBC_LED_F3, SBC_LED_MAINWC, SBC_LED_SUBWC,
    SBC_LED_MAGAZINE, SBC_LED_COMM1, SBC_LED_COMM2, SBC_LED_COMM3,
    SBC_LED_COMM4, SBC_LED_COMM5,
    SBC_LED_GEAR_R, SBC_LED_GEAR_N, SBC_LED_GEAR_1, SBC_LED_GEAR_2,
    SBC_LED_GEAR_3, SBC_LED_GEAR_4, SBC_LED_GEAR_5,
    SBC_LED_COUNT /* = 37 */
} sbc_led_t;

/* LED-Sollwerte: level je LED 0x0 (aus) .. 0xF (voll). */
typedef struct {
    uint8_t level[SBC_LED_COUNT];
} sbc_leds_t;

typedef struct sbc_usb_dev sbc_usb_dev_t;

/* Geraet oeffnen (erstes 0A7B:D000 mit WinUSB-Bindung).
 * Rueckgabe SBC_OK oder SBC_ERR_NODEVICE/_DRIVER/_ARG. */
int sbc_usb_open(sbc_usb_dev_t **out);

/* Geraet schliessen (NULL-tolerant). */
void sbc_usb_close(sbc_usb_dev_t *dev);

/* Einen IN-Report lesen (Timeout ~100 ms) und nach *st dekodieren.
 * Rueckgabe SBC_OK, SBC_ERR_TIMEOUT (nichts Neues - kein Fehler),
 * SBC_ERR_IO/_BADREPORT/_ARG. */
int sbc_usb_poll(sbc_usb_dev_t *dev, sbc_state_t *st);

/* LED-Sollwerte als OUT-Report senden (Rueckgabe wie poll, ohne TIMEOUT). */
int sbc_usb_set_leds(sbc_usb_dev_t *dev, const sbc_leds_t *leds);

/* ---- reine Funktionen (ohne Geraet, Fixture-testbar) ---- */

/* 26-Byte-IN-Report -> Zustand. Rueckgabe SBC_OK/_BADREPORT/_ARG. */
int sbc_usb_parse_report(const uint8_t raw[SBC_USB_IN_LEN], sbc_state_t *st);

/* LEDs -> 22-Byte-OUT-Report (Header + Nibbles). */
void sbc_usb_build_led_report(const sbc_leds_t *leds, uint8_t out[SBC_USB_OUT_LEN]);
int sbc_usb_parse_led_report(const uint8_t raw[SBC_USB_OUT_LEN], sbc_leds_t *leds);

/* Zustand -> 24-Byte-SBC-Block (exakt pState+4-Layout: buttons, je
 * Pad+Wert, Tuner, Gang). Rueckgabe: immer 0. */
int sbc_usb_state_to_block(const sbc_state_t *st, uint8_t blk[24]);

/* Alle LEDs auf einen Wert setzen (0x0 = aus, 0xF = voll). */
void sbc_usb_leds_all(sbc_leds_t *leds, uint8_t level);

/* ---- W128 (SBC-LIVE): Roh-Zugriff fuer sbc_probe --dump/--watch/--test ---- */

#define SBC_USB_RAW_MAX 32 /* max. Paketgroesse EP IN/OUT (Deskriptor) */

/* Einen IN-Report roh lesen (bis 32 Bytes, Timeout ~100 ms). *len = Anzahl
 * gelesener Bytes. Rueckgabe SBC_OK/_TIMEOUT/_IO/_ARG (keine Header-Pruefung). */
int sbc_usb_poll_raw(sbc_usb_dev_t *dev, uint8_t raw[SBC_USB_RAW_MAX], unsigned *len);

/* Report per Control-Transfer GET_REPORT holen (Klasse/Interface, 0xA1 01
 * wValue 0x0100) - Fallback, falls das Geraet im Leerlauf keine
 * Interrupt-Reports schickt. HYPOTHESE (XID-Doku), am Geraet unbelegt. */
int sbc_usb_get_report(sbc_usb_dev_t *dev, uint8_t raw[SBC_USB_RAW_MAX], unsigned *len);

/* 22-Byte-OUT-Report unveraendert senden (auch reservierte Nibbles, fuer
 * die Lampenfolge --leds seq). Rueckgabe SBC_OK/_IO/_ARG. */
int sbc_usb_send_out_raw(sbc_usb_dev_t *dev, const uint8_t out[SBC_USB_OUT_LEN]);

/* ---- W128: Kalibrierung (Raender/Mitte/Totzone je Achse, cfg-Schalter) ----
 * Werte in Spiel-Einheiten = High-Byte der Report-Achse (Aiming/Pedale
 * 0..255, Rotation/Sight -128..127). Nicht gesetzte Achsen bleiben
 * unveraendert (bitgleich). Keys ([input]):
 *   sbc_cal_<achse>=lo,mid,hi   (federnde Achsen: rot sight_x sight_y)
 *   sbc_cal_aim_x|y=lo,hi       (W141: Zielhebel absolut, linear lo..hi,
 *                               keine Mitte, keine Totzone; Drei-Wert-Form
 *                               wird gelesen, mid ignoriert, Rueckgabe 2)
 *   sbc_cal_<pedal>=lo,hi       (pedal_l pedal_m pedal_r; lo = Ruhewert)
 *   sbc_dead_<achse>=N          (Totzone um mid bzw. ueber lo, 0 = aus;
 *                               W141: sbc_dead_aim_* wird ignoriert, Rueckgabe 2)
 *   sbc_gear_map=r,n,1,2,3,4,5  (Rohwerte des Gang-Bytes, s. sbc_cal_t) */
enum {
    SBC_AX_AIM_X = 0, SBC_AX_AIM_Y, SBC_AX_ROT, SBC_AX_SIGHT_X, SBC_AX_SIGHT_Y,
    SBC_AX_PEDAL_L, SBC_AX_PEDAL_M, SBC_AX_PEDAL_R,
    SBC_AX_COUNT /* = 8 */
};

typedef struct {
    int set;          /* 1 = Achse wird umgerechnet */
    int lo, mid, hi;  /* gemessene Raender/Mitte (Pedal: mid = lo) */
    int dead;         /* Totzone (Spiel-Einheiten) */
} sbc_axis_cal_t;

typedef struct {
    sbc_axis_cal_t ax[SBC_AX_COUNT];
    /* W128: Gang-Zuordnung, falls das Geraet nicht 7..13 liefert (am Blech
     * 5.11. gesehen: Gang-Byte = 1 in Ruhe - Kodierung offen). Key
     * sbc_gear_map=<R>,<N>,<1>,<2>,<3>,<4>,<5> (Rohwerte 0..255 bzw. -128..-1);
     * gesetzt: Rohwert gear_raw[k] -> 7+k, jeder andere Wert -> 0 (dazwischen,
     * Hook haelt den letzten Gang). */
    int gear_map_set;
    uint8_t gear_raw[7];
} sbc_cal_t;

/* Achsen-Kurzname ("aim_x" ...) bzw. -1 bei unbekanntem Namen. */
const char *sbc_usb_axis_name(int axis);
int sbc_usb_axis_find(const char *name);

/* cfg-Key/Wert uebernehmen. Rueckgabe 1 = Key erkannt + Wert gueltig,
 * 2 (SBC_CAL_NOTE) = ok, aber Hinweis faellig (W141: Aiming-Drei-Wert-Form,
 * Mittlerer ignoriert, bzw. sbc_dead_aim_* ignoriert - Aufrufer loggt),
 * 0 = kein sbc_cal_/sbc_dead_-Key, -1 = Key erkannt, Wert ungueltig. */
#define SBC_CAL_NOTE 2
int sbc_usb_cal_set(sbc_cal_t *cal, const char *key, const char *val);

/* Kalibrierung auf einen dekodierten Zustand anwenden (NULL-tolerant). */
void sbc_usb_cal_apply(const sbc_cal_t *cal, sbc_state_t *st);

/* W141: Verhaltens-Abbildung Gang-Rohwert -> Stellung (Spiel-Mapper
 * 0x4A5E7+, Disassembly + Live-Beleg Nutzer 6.11.): 0=R, 1=N, 2..6 =
 * 1..5. Gang, -1 = 0 (dazwischen, Spiel+Hook halten den letzten Gang),
 * -2 = ungueltig (Klasse mod 8 == 6 ausser 254: Out 0x80, keine
 * Stellung). 254->R und 255->N sind Spezialfaelle im Mapper, sonst
 * zaehlt nur v&7 (7->R, 0->N, 1..5->1..5). */
int sbc_usb_gear_effective(int raw);

/* ---- W128: Auto-Kalibrierung (Ruhewerte beim Start erfassen) ----
 * Die ersten Zustaende nach dem Oeffnen werden gesammelt; ist ein Fenster
 * von SBC_AC_WIN Zustaenden auf allen Achsen ruhig (max-min <= 3), gilt der
 * Mittelwert als Ruhelage. Am Blech (Nutzer 5.11.): Aiming 0x76/0x7C,
 * Pedal Mitte 0x17 in Ruhe - ohne Abgleich zielt/bremst das Spiel dauernd.
 * Achsen mit sbc_cal_<achse> in der cfg behalten die cfg-Werte.
 * W141: Aiming X/Y nehmen nicht teil (Reibungshebel ohne Mitte): ihre
 * Ruhe wird nur mitprotokolliert (ac->rest, Logzeile), stoert das
 * Ruhig-Fenster nicht und erzeugt keine Kalibrierung. */
#define SBC_AC_WIN 24
#define SBC_AC_TRIES 40
typedef struct {
    int n, tries, done, stable;
    int mn[SBC_AX_COUNT], mx[SBC_AX_COUNT];
    long sum[SBC_AX_COUNT];
    int rest[SBC_AX_COUNT];
} sbc_autocal_t;

/* 1 = Zielhebel-Achse (W141: Reibungshebel, absolut, ohne Mitte/Totzone). */
int sbc_usb_axis_is_aim(int axis);

/* Achswert in Spiel-Einheiten lesen/schreiben (SBC_AX_*). */
int sbc_usb_axis_get(const sbc_state_t *st, int axis);

/* Einen Zustand einspeisen. Rueckgabe 1 = gerade fertig geworden
 * (ac->stable: 1 = Ruhelage erfasst, 0 = nie ruhig, aufgegeben). */
int sbc_usb_autocal_feed(sbc_autocal_t *ac, const sbc_state_t *st);

/* Wirksame Kalibrierung: cfg-Achsen unveraendert, sonst aus der Ruhelage
 * (zentrierte Achsen: Mitte = Ruhe, Totzone axis_dead; Pedale: lo = Ruhe,
 * Totzone pedal_dead). Ruhelagen weit neben der Nennmitte (> 48 Einheiten,
 * Hebel gehalten?) werden nicht uebernommen. */
void sbc_usb_autocal_build(const sbc_autocal_t *ac, const sbc_cal_t *cfg,
                           int axis_dead, int pedal_dead, sbc_cal_t *out);

/* Fehlercode -> Text (statisch, nie NULL). */
const char *sbc_usb_strerror(int err);

#ifdef __cplusplus
}
#endif
