#pragma once
/* launcher_layout.h - W281 LAUNCHER-SIZE: Layout-Varianten des Launchers
 * (Entwurf zur Auswahl). Alle Layoutwerte stehen hier zentral, damit die
 * Zeichenfunktionen in launcher_gui.c nur parametrisiert werden.
 *
 *  0 = heute (960x640 logisch, unveraendert, Standard)
 *  a = "Groesser, gleiches Layout": gleiche Anordnung, Geometrie und Schrift
 *      skaliert (UI-Groesse Kompakt/Normal/Gross), passt auf die Arbeitsflaeche
 *  b = "Mehr Platz, neu geordnet": breiteres, ziehbares Fenster (Standard
 *      1280x800 logisch, Mindestgroesse), das 960x640-Entwurfsraster wird auf
 *      die Flaeche abgebildet (Abstaende wachsen, Schrift nicht gestreckt),
 *      Spielen-Seite mit Start-Bereich und Statuskarten.
 *
 * Reine Funktionen (ohne Fenster), Selbsttest llay_selftest. */
#ifdef __cplusplus
extern "C" {
#endif

/* Entwurfsraster (logische 96-dpi-Einheiten, wie launcher_gui.c) */
#define LLAY_DW 960
#define LLAY_DH 640
#define LLAY_TITLE 48
#define LLAY_NAV 208
#define LLAY_BOT 64

enum { LLAY_0 = 0, LLAY_A = 1, LLAY_B = 2, LLAY_N };
enum { LLAY_SZ_COMPACT = 0, LLAY_SZ_NORMAL = 1, LLAY_SZ_LARGE = 2, LLAY_SZ_N };

typedef struct {
    int id;        /* LLAY_* */
    int size;      /* LLAY_SZ_* (nur a/b) */
    int geo_pct;   /* Geometrie in % der Monitor-DPI (nach dem Einpassen) */
    int font_pct;  /* Schrift in % der Monitor-DPI (nach dem Einpassen) */
    int tw, th;    /* Zielflaeche logisch (Geometrie-Einheiten) */
    int nav;       /* Breite der Navigation auf der Zielflaeche */
    int resizable; /* Fenster ziehbar (b) */
    int min_w, min_h; /* Mindestgroesse logisch (b) */
} llay_t;

/* "0"|"a"|"b" (auch "A"/"B"/"heute") -> LLAY_*, -1 = unbekannt. */
int llay_parse(const char *v);
const char *llay_name(int id); /* "0" | "a" | "b" */
/* "compact"|"normal"|"large" (auch 0/1/2) -> LLAY_SZ_*, -1 = unbekannt. */
int llay_size_parse(const char *v);
const char *llay_size_name(int size);

/* Ausgangswerte der Variante (ohne Einpassen). */
void llay_init(llay_t *L, int id, int size);
/* Einpassen: Fenster (tw x th bei geo_pct der Monitor-DPI) muss in die
 * Arbeitsflaeche work_w x work_h (Pixel) passen. b verkleinert zuerst die
 * Flaeche bis zur Mindestgroesse, dann (wie a) die Skalierung, nie unter
 * LLAY_MIN_PCT. Layout 0 bleibt unveraendert (heutiges Verhalten).
 * Rueckgabe: 1 = verkleinert, 0 = passte. */
#define LLAY_MIN_PCT 60
int llay_fit(llay_t *L, int mon_dpi, int work_w, int work_h);
/* Fenstergroesse in Pixeln bei Monitor-DPI. */
void llay_window_px(const llay_t *L, int mon_dpi, int *w, int *h);
/* b: neue Flaeche aus der Pixelgroesse nach dem Ziehen (mit Mindestgroesse). */
void llay_resize_px(llay_t *L, int mon_dpi, int w_px, int h_px);

/* Abbildung Entwurfsraster -> Zielflaeche (nur b, sonst Identitaet).
 * Titel- und Fussband: Elemente der rechten Haelfte bleiben am rechten
 * Rand, Fussleiste links folgt der Navigation. Inhalt: linear. */
int llay_mapx(const llay_t *L, int x, int y);
int llay_mapy(const llay_t *L, int y);
/* Rechteck abbilden; sw/sh = Breite/Hoehe mitstrecken (sonst behalten).
 * out = { x, y, w, h } auf der Zielflaeche. */
void llay_map_rect(const llay_t *L, int x, int y, int w, int h, int sw, int sh,
                   int out[4]);
/* Streckregel fuer gezeichnete Flaechen/Texte (Karten strecken, Punkte,
 * Banner, einzeilige Beschriftungen behalten die Hoehe). */
void llay_map_paint(const llay_t *L, int x, int y, int w, int h, int out[4]);

/* b: Spielen-Seite im Entwurfsraster. Statuskarten 0..5 (3 x 2; 0..4 =
 * Bereitschaftszeilen Spiel/Grafik/Anzeige/Packs/Steuerung, 5 = Primaerfeuer),
 * "Aendern"-Knopf je Karte, Startbereich. Rueckgabe out = { x, y, w, h }. */
#define LLAY_BCARD_N 6
void llay_b_card(int i, int out[4]);
void llay_b_card_go(int i, int out[4]);
void llay_b_start_area(int out[4]);  /* Karte um die Startknoepfe */
void llay_b_start_big(int out[4]);   /* LAUNCH GAME */
void llay_b_start_vr(int out[4]);    /* PLAY IN VR */
void llay_b_start_hint(int out[4]);  /* Grund-/VR-Zeile unter den Knoepfen */

/* Rechtecke { x, y, w, h } ueberlappen (echte Schnittflaeche > 0)? */
int llay_overlap(const int a[4], const int b[4]);

/* Selbsttest (rein): Parser, Einpassen kleiner Arbeitsflaechen, Abbildung,
 * Karten ohne Ueberlappung. rep(name, ok, detail) je Fall. */
typedef void (*llay_rep_fn)(const char *name, int ok, const char *detail);
void llay_selftest(llay_rep_fn rep);

#ifdef __cplusplus
}
#endif
