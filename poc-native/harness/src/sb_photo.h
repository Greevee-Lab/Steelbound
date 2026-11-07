#pragma once
/* sb_photo.h - W195 PHOTO-MODE: Fotomodus (render_photo.c).
 *
 * Einstieg: Profil-Aktion "photo" (Standard F12) oder ESC-Spielmenue -> "Fotomodus".
 * Das Spiel pausiert (sb_pause); gezeigt wird das Standbild der Pause (W193). Das
 * Foto-Panel (sb_overlay.c, Bildschirm SC_PHOTO) stellt ein:
 *   - Cockpit an/aus: aus = nur die Aussenwelt, d. h. das Kamerabild, das das Spiel in
 *     das Welt-RT des Hauptmonitors zeichnet (RT31, ohne HUD-Glyphen, Korn und Cockpit),
 *     unverzerrt (Seitenverhaeltnis wie auf dem Monitor, ~4:3); Bildformat Ausschnitt/weit/Kamera.
 *   - freie Kamera (drehen/verschieben, begrenzt; Ausgangspunkt die Spielkamera): die
 *     aufgezeichneten Welt-Draws des Ticks werden mit geaenderter Sicht neu gezeichnet.
 *     Das Spiel zeichnet nur, was die Spielkamera sieht -> grosse Drehungen/Wege zeigen
 *     Luecken; daher Grenzen photo_cam_yaw/_pitch/_dist.
 *   - Aufnahme-Aufloesung x1/x2/x4/x8 (Fensteraufloesung bzw. Fensterhoehe ohne Cockpit),
 *     gekachelt gerendert (Kacheln <= 4096 x 1024, Viewport-Grenze 32767), PNG oder JPG.
 * Fotos: photo_dir (leer = <Nutzerdaten>\Screenshots, legacy/Tests: <Ausgabe>\Screenshots),
 * Name Steelbound_JJJJ-MM-TT_hh-mm-ss_M<nn>.<png|jpg>. Nur Client-Bild (Wiedergabe in
 * eigene Ziele), nie Desktop. VR: aus (Hinweis).
 *
 * Alle Funktionen laufen auf dem Spiel-Thread in der Pause (unter ctx_lock), ausser
 * sb_photo_dir (beliebig). Ohne geoeffneten Fotomodus wird nichts gerechnet. */
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ID3D11Resource;

#define SB_PHOTO_NSCALE 4 /* x1, x2, x4, x8 */

typedef struct {
    int active;        /* Fotomodus offen */
    int rec;           /* Aufzeichnung des Ticks vorhanden (Mission + FPS-INTERP): Cockpit aus, freie Kamera, x2..x8 */
    int vr;            /* VR-Betrieb: Fotomodus aus */
    int cockpit;       /* 1 = Cockpit sichtbar */
    int freecam;       /* 1 = freie Kamera */
    float yaw, pitch;  /* Grad */
    float dist;        /* Weg von der Spielkamera (Spiel-Einheiten) */
    float max_yaw, max_pitch, max_dist;
    int scale;         /* Index 0..3 */
    int jpg;           /* 1 = JPG */
    int aspect;        /* Bildformat ohne Cockpit: 0 Bildschirm (Ausschnitt), 1 Bildschirm (weit), 2 Kamera (wie Monitor, ~4:3) */
    unsigned sw[SB_PHOTO_NSCALE], sh[SB_PHOTO_NSCALE]; /* Ausgabegroesse je Stufe */
    int savail[SB_PHOTO_NSCALE];                      /* Stufe moeglich */
    int busy;          /* Aufnahme angefordert */
    int last_ok;       /* letzte Aufnahme: 1 ok, 0 keine, -1 Fehler */
    char last_name[160];
    unsigned last_w, last_h;
    double last_ms;
    unsigned nshot;    /* abgeschlossene Aufnahmen (ok oder Fehler) */
} sb_photo_ui_t;

void sb_photo_enter(void);
void sb_photo_leave(void);
int sb_photo_active(void);
void sb_photo_get(sb_photo_ui_t *u);
void sb_photo_set_cockpit(int on);
void sb_photo_set_freecam(int on);
void sb_photo_set_scale(int idx);
void sb_photo_set_jpg(int jpg);
void sb_photo_set_aspect(int a); /* 0..2, s. sb_photo_ui_t.aspect */
void sb_photo_cam_reset(void);
/* Kamera bewegen: Drehung in Grad, Weg in Spiel-Einheiten (vorwaerts, rechts, oben; Kamera-lokal). */
void sb_photo_cam_move(float dyaw, float dpitch, float fwd, float right, float up);
/* Foto anfordern (naechstes Pausenbild). 0 = nicht moeglich (z. B. VR). */
int sb_photo_request(void);
/* Pausenbild: Vorschau neu rechnen bzw. Aufnahme ausfuehren. still = Standbild der Pause
 * (Kopie bzw. Neuzeichnung W193). *raw = 1: ohne Nachbearbeitung zeigen (Aussenwelt). */
struct ID3D11Resource *sb_photo_pause_image(struct ID3D11Resource *still, int *raw);
/* Fotoordner (UTF-8). 1 = eingestellt/ermittelt. */
int sb_photo_dir(char *out, size_t cap);
/* Kurzname fuer die Anzeige (letzte 2 Pfadteile). */
void sb_photo_dir_show(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
