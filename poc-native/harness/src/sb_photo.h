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
 *   - W198 Monitor-Effekt und Shader: temporaer fuer diese Foto-Sitzung; beim
 *     Verlassen exakte Wiederherstellung, keine Speicherung. Live wie W193,
 *     auch fuer gekachelte Aufnahmen. Fehlender Tick-Zustand wird angezeigt.
 *   - Aufnahme-Aufloesung x1/x2/x4/x8 (Fensteraufloesung bzw. Fensterhoehe ohne Cockpit),
 *     gekachelt gerendert (Kacheln <= 4096 x 1024, Viewport-Grenze 32767), PNG oder JPG.
 * Fotos: photo_dir (leer = <Nutzerdaten>\Screenshots, legacy/Tests: <Ausgabe>\Screenshots),
 * Name Steelbound_JJJJ-MM-TT_hh-mm-ss_M<nn>.<png|jpg>. Nur Client-Bild (Wiedergabe in
 * eigene Ziele), nie Desktop. VR: aus (Hinweis).
 *
 * W290 QUICK-SHOT: eine Aufnahme-Konfiguration fuer Fotomodus und Schnell-Screenshot in [photo]
 * (photo_view = wie im Spiel / ohne Cockpit = Vollbild-Ansicht des Spiels / nur Aussenwelt, photo_size =
 * Fenster/4K/8K/xN, photo_format, photo_aspect nur fuer die Aussenwelt). Schnell-Screenshot (Aktion quick_shot,
 * F12): sb_overlay.c fordert ggf. die Vollbild-Ansicht an (sb_flat_*), dann eine Pause ohne Menue; das erste
 * Pausenbild nimmt auf (sb_photo_quick_run), danach geht es weiter. Ist das Spiel in der Vollbild-Ansicht oder
 * steht edge_mode=1, ist das Foto der 4:3-Rahmen des Spielbilds.
 * Alle Funktionen laufen auf dem Spiel-Thread in der Pause (unter ctx_lock), ausser
 * sb_photo_dir (beliebig). Ohne geoeffneten Fotomodus bzw. Schnell-Screenshot wird nichts gerechnet. */
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ID3D11Resource;

#define SB_PHOTO_NSCALE 4 /* x1, x2, x4, x8 (alt, W195) */
/* W290 QUICK-SHOT: Groessen-Liste der Aufnahme (Panel/Launcher-Reihenfolge): Fenster, 4K, 8K, x2, x4, x8.
 * Werte wie [photo] photo_size: 1/2/4/8 = x Fenster, -4 = 4K, -8 = 8K. */
#define SB_PHOTO_NSIZE 6
extern const int kSbPhotoSize[SB_PHOTO_NSIZE];
/* Ansicht der Aufnahme ([photo] photo_view) */
#define SB_PV_GAME 0  /* wie im Spiel (Cockpit bzw. die gerade gezeigte Vollbild-Ansicht) */
#define SB_PV_FLAT 1  /* ohne Cockpit: Vollbild-Ansicht des Spiels (W268-Flag nur fuer die Aufnahme) */
#define SB_PV_WORLD 2 /* nur Aussenwelt: Kamerabild ohne HUD/Monitor (W195 "Cockpit aus") */
/* W428 PHOTO-CINEMATIC: wie "nur Aussenwelt" (Vollbild-Aussensicht ohne HUD und Monitor-Effekt), zusaetzlich ohne
 * die Teile des eigenen VT (Waffenarm/Geschuetz), die das Spiel ins Welt-RT zeichnet. Shader-, Licht- und
 * Nachbearbeitungs-Einstellungen bleiben unveraendert; live wird nichts umgeschaltet. Nur SB (LoC: wie Aussenwelt). */
#define SB_PV_CINE 3
#define SB_PV_N 4
/* W445 PHOTO-CINE-SELF: Reichweite eigene VT-Teile ([photo] photo_cine_self, Zehntel-Einheiten ~ dm): Panel/Launcher-Schritte
 * 0 (aus) und SB_CSELF_LO..SB_CSELF_HI in SB_CSELF_STEP. Gleiche Liste im Launcher (launcher_gui.c, kCself). */
#define SB_CSELF_LO 10
#define SB_CSELF_HI 120
#define SB_CSELF_STEP 5
#define SB_CSELF_N (1 + (SB_CSELF_HI - SB_CSELF_LO) / SB_CSELF_STEP + 1)

typedef struct {
    int monitor_fx, shading, effect_pending; /* W198: temporary options, missing recorded state */
    int active;        /* Fotomodus offen */
    int rec;           /* Aufzeichnung des Ticks vorhanden (Mission; FPS-INTERP bzw. ISSUE-PHOTO bei Bedarf): Aussenwelt, freie Kamera, > Fenster */
    int vr;            /* VR-Betrieb: Fotomodus aus */
    int cockpit;       /* 1 = Cockpit-Pfad (Ansicht wie im Spiel / ohne Cockpit), 0 = nur Aussenwelt */
    int view;          /* W290: SB_PV_* (gewaehlt) */
    int world;         /* W428: Aussenwelt-Pfad (nur Aussenwelt oder Cinematic) */
    int cine_ok;       /* W428: Cinematic hier waehlbar (SB, nicht LoC) */
    int flat;          /* W290: Spiel zeigt gerade die Vollbild-Ansicht */
    int stepping;      /* W290: Ansicht wird umgeschaltet (Spiel laeuft kurz weiter) */
    int freecam;       /* 1 = freie Kamera */
    float yaw, pitch;  /* Grad */
    float dist;        /* Weg von der Spielkamera (Spiel-Einheiten) */
    float max_yaw, max_pitch, max_dist;
    int size;          /* W290: Index 0..SB_PHOTO_NSIZE-1 in kSbPhotoSize */
    int jpg;           /* 1 = JPG */
    int aspect;        /* Bildformat nur Aussenwelt: 0 Bildschirm (Ausschnitt, nur cfg), 1 breit (experimentell), 2 4:3 wie Monitor */
    unsigned sw[SB_PHOTO_NSIZE], sh[SB_PHOTO_NSIZE]; /* Ausgabegroesse je Groesse */
    int savail[SB_PHOTO_NSIZE];                      /* Groesse moeglich */
    int busy;          /* Aufnahme angefordert */
    int last_ok;       /* letzte Aufnahme: 1 ok, 0 keine, -1 Fehler */
    char last_name[160];
    unsigned last_w, last_h;
    double last_ms;
    unsigned nshot;    /* abgeschlossene Aufnahmen (ok oder Fehler) */
} sb_photo_ui_t;

/* W290: Ergebnis eines Schnell-Screenshots */
#define SB_QS_NOTE_NOREC 1  /* keine Tick-Aufzeichnung: nur Fenstergroesse / wie im Spiel */
#define SB_QS_NOTE_NOFLAT 2 /* ohne Cockpit hier nicht moeglich -> wie im Spiel */
#define SB_QS_NOTE_SMALLER 4 /* gewaehlte Groesse ueber den Grafik-Grenzen -> kleiner */
#define SB_QS_NOTE_NET 8     /* W357: Netzspiel - Fenstergroesse, ohne Pause (sb_pausenet.h) */
typedef struct {
    int ok;            /* 1 gespeichert, 0 Fehler */
    int notes;         /* SB_QS_NOTE_* */
    char name[160];
    unsigned w, h;
    double ms;
} sb_photo_qres_t;

void sb_photo_enter(void);
void sb_photo_leave(void);
int sb_photo_active(void);
void sb_photo_get(sb_photo_ui_t *u);
void sb_photo_set_effect(int shader, int on);
void sb_photo_set_view(int v);  /* W290: SB_PV_*, ohne Cockpit fordert die Vollbild-Ansicht an (Spiel laeuft kurz) */
void sb_photo_set_freecam(int on);
void sb_photo_set_size(int idx); /* W290: Index in kSbPhotoSize */
void sb_photo_set_jpg(int jpg);
void sb_photo_set_aspect(int a); /* 0..2, s. sb_photo_ui_t.aspect */
/* W290: Fotomodus schaltet die Ansicht um -> Pause kurz aufheben (Spiel zeichnet die Ansicht neu). Je Bild rufen. */
int sb_photo_stepping(void);
void sb_photo_step_tick(void);
/* W290 QUICK-SHOT: Schnell-Screenshot mit den [photo]-Einstellungen (ohne Panel). arm: beim naechsten
 * Pausenbild aufnehmen; run: im Pausenbild (d3d11_render.c); result: 1 = fertig (einmal geliefert). */
void sb_photo_quick_arm(void);
int sb_photo_quick_pending(void);
void sb_photo_quick_run(struct ID3D11Resource *still);
int sb_photo_quick_result(sb_photo_qres_t *r);
/* ISSUE-PHOTO: ohne FPS-INTERP (interp_fps=0) vor der Pause zwei Ticks aufzeichnen, wenn die Einstellungen sie
 * brauchen (Groesse ueber Fenster, nur Aussenwelt, HUD aus). begin: 1 = angefordert; wait: 1 = Pause darf kommen
 * (Tick laeuft bzw. fertig/nicht moeglich/Zeit); free: danach freigeben (im Fotomodus erst beim Verlassen). */
int sb_photo_quick_rec_begin(void);
int sb_photo_quick_rec_wait(void);
void sb_photo_quick_rec_free(void);
/* W290: wirksame Groesse ([photo] photo_size; nicht gesetzt -> photo_scale > 1, sonst 4K) */
int sb_photo_size_eff(void);
/* W290: Vollbild-Ansicht fuer eine Aufnahme (hooks_d3d.c, W268-Flag). state: 0 wartet, 1 bereit, -1 nicht moeglich */
void sb_flat_request(int on);
int sb_flat_state(void);
int sb_flat_now(void);
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
