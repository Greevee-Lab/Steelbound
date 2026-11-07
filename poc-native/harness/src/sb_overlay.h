#pragma once
/* sb_overlay.h - W118 REM-OVERLAY: Einstellungs-Overlay zur Laufzeit (ESC).
 *
 * ESC (Fenster-Nachricht an das Spielfenster) oeffnet ein kompaktes Overlay
 * im Stil der Spielmenues (gruene Monochrom-/Militaer-HUD-Optik), ESC
 * schliesst es. Das Spiel laeuft und rendert dahinter weiter (Live-
 * Vorschau); solange das Overlay offen ist, gehen echte Tastatur-/Maus-/
 * Pad-Eingaben ans Overlay, nicht ans Spiel (key_down, hooks_input).
 * Umschaltbar: Bildrate, Shading, Monitor-Effekte (Korn/Stoerung),
 * Bildraender, Maus, HD-Texturen 2x/4x (B = A/B-Vergleich); Darstellung/
 * Aufloesung nur "nach Neustart". "Als Standard speichern" schreibt die
 * Spieler-cfg (user.cfg) wie der Launcher (verwaltete Keys ersetzen,
 * Rest unveraendert).
 *
 * Schrift: Windows-Systemschrift (GDI, Consolas) in eine eigene Textur -
 * keine Spielinhalte im Code (Community Edition, EDITIONEN.md).
 *
 * Schalter: [overlay] overlay (1 = an, 0 = aus: ESC geht wie bisher durch),
 * overlay_test (Test-Hook, leer = aus), overlay_save_path (leer = user.cfg
 * neben sb_native.cfg bzw. SB_USER_CFG). Geschlossen zeichnet das Overlay
 * nichts (Standardpfad bitgleich). */
#ifdef __cplusplus
extern "C" {
#endif

#ifndef _WINDEF_
#include <windows.h>
#endif

struct ID3D11RenderTargetView;

/* Fenster-Nachricht (UI-Thread). 1 = vom Overlay verbraucht (nicht an
 * Spiel/DefWindowProc weiterreichen). */
int sb_ovl_wndproc(HWND w, UINT msg, WPARAM wp, LPARAM lp);
/* 1 = echte Eingaben duerfen das Spiel nicht erreichen (Overlay offen). */
int sb_ovl_input_blocked(void); /* zaehlt gesperrte Abfragen (Beleg) */
int sb_ovl_open(void);            /* ohne Zaehler */
int sb_ovl_peek(void);            /* W193: 1 = Menue per gehaltener Taste ausgeblendet (bleibt offen) */
/* Present-Anfang (Render-Thread): Tasten verarbeiten, Werte anwenden,
 * Test-Hook-Skript. */
void sb_ovl_present_begin(unsigned n_present);
/* Overlay auf rtv zeichnen (unter ctx_lock). save=1: Bereich vorher sichern
 * (Spiel-Backbuffer, sb_ovl_restore nach dem Present). */
void sb_ovl_draw(struct ID3D11RenderTargetView *rtv, int save);
/* Gesicherten Bereich zuruecklegen (nach dem Present, unter ctx_lock). */
void sb_ovl_restore(void);
/* Present-Ende (zwischen den Bildern): Bildraten-Wechsel, der Views
 * braucht. */
void sb_ovl_present_end(void);
/* TEST(PERF-BENCH,W137): Bildratenwechsel (interp_fps) wie aus dem Overlay anfordern */
void sb_ovl_request_fps(int fps);
/* 1 = Test-Hook-Screenshot in diesem Bild faellig (nach dem Zeichnen). */
int sb_ovl_shot_due(char *name, unsigned cap);
/* W188 GAME-MENU: ESC oeffnet zuerst das Spielmenue (Weiter, Einstellungen,
 * Fotomodus (ausgegraut), Feedback, Beenden mit Rueckfrage); die Seiten oben
 * sind eine Tab-Zeile (TAB/Umschalt+TAB, Pfeile auf der Tab-Zeile, Pad
 * LB/RB). Maus (Klick/Rad/Zeiger), Tastatur und Gamepad bedienen alle
 * Bildschirme. Offenes Menue pausiert das Spiel (menu_pause, sb_pause.c).
 * Pause-Tasten der Profil-Aktion "pause" (hooks_input.c, n=0 -> PAUSE). */
void sb_ovl_set_pause_binding(const int *vks, int n, unsigned padmask);
/* Pause-Aktion aus der Belegung (Pad/Controller, Spiel-Thread): oeffnet den Pause-Bildschirm. */
void sb_ovl_request_pause(const char *why);

/* ---- W122: kleine gemeinsame API (Look + Zeichenweg), genutzt vom
 * Leistungs-Overlay sb_perfhud.c. Farben (GDI COLORREF) und Deckung des
 * gruenen SB-UI. ---- */
#define SB_UI_C_BG    RGB(4, 22, 11)
#define SB_UI_C_TXT   RGB(140, 255, 150)
#define SB_UI_C_DIM   RGB(58, 120, 68)
#define SB_UI_C_LINE  RGB(80, 200, 100)
#define SB_UI_C_HEAD  RGB(70, 190, 88)
#define SB_UI_C_SEL   RGB(24, 84, 40)
#define SB_UI_ALPHA   0.88f
struct ID3D11ShaderResourceView;
struct ID3D11Texture2D;
struct ID3D11Resource;
/* gesicherter Backbuffer-Bereich (ohne FPS-INTERP: Spiel-BB bleibt bitgleich) */
typedef struct {
    struct ID3D11Texture2D *tex;
    struct ID3D11Resource *dst;
    unsigned w, h, x, y;
    int fmt, saved;
} sb_ovl_save_t;
void sb_ovl_save_take(sb_ovl_save_t *s, struct ID3D11RenderTargetView *rtv, unsigned x, unsigned y, unsigned w,
                      unsigned h);
void sb_ovl_save_put(sb_ovl_save_t *s);
/* Scanlines + vormultipliziertes Alpha auf eine 32-Bit-DIB (Zeilen top-down) */
void sb_ovl_ui_finish(unsigned *bits, unsigned w, unsigned h, float factor);
/* vormultiplizierte BGRA-Textur 1:1 (Texel-Load, nie gestreckt) bei x0,y0
 * auf rtv zeichnen; Kontextzustand gesichert. Unter ctx_lock. 1 = ok. */
int sb_ovl_quad(struct ID3D11RenderTargetView *rtv, struct ID3D11ShaderResourceView *srv, unsigned x0, unsigned y0,
                unsigned w, unsigned h);
/* cfg-Datei schreiben wie "Als Standard speichern" (Launcher-Verfahren):
 * erste Zeile je Key ersetzen, Rest unveraendert, fehlende Keys unter [sec]
 * anhaengen. Anzahl Keys bzw. -1. */
int sb_ovl_cfg_write(const char *path, int n, const char *const *keys, const char *const *secs,
                     const char *const *vals);

#ifdef __cplusplus
}
#endif
