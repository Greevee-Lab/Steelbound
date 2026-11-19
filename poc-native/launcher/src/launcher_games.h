#pragma once
/* launcher_games.h - W276 LOC-LAUNCH: Spielauswahl im Launcher
 * (Steel Battalion | Line of Contact) - reine Logik ohne Oberflaeche.
 *
 * Nutzervorgabe 14.11.: Line of Contact (LoC) kommt in DENSELBEN Launcher als
 * zweiter umschaltbarer Bereich. Gemeinsame Einstellungen (Anzeige, Grafik,
 * Steuerung, Ton) bleiben in user.cfg und gelten fuer beide Spiele; LoC hat
 * eigenen Abbild-Pfad, eigenen Datenordner und (spaeter) Online-Einstellungen.
 *
 * Ablage (user.cfg, nur Launcher; der Client ignoriert "launcher_*"):
 *   launcher_game=sb|loc        gewaehlter Bereich (fehlt = sb)
 *   launcher_loc_image=<Pfad>   LoC-Abbild (UTF-8)
 *   launcher_loc_server=<Adr.>  Platzhalter fuer den eigenen Dienst (spaeter)
 *   loc_widescreen=0|1|2        W326 LOC-WIDE: Bildformat des Spiels - ein
 *                               Client-Schluessel (kein launcher_*), den der
 *                               Client nur im LoC-Profil auswertet, beim Start
 *                               (0 = 4:3 Original, 1 = 16:9 Spielmodus,
 *                               2 = 720p; fehlt/ungueltig = 0)
 * Datenordner LoC: <Datenordner>\loc (Profil/portabel) bzw. <Client>\loc
 * (legacy) - getrennt von den SB-Spielstaenden (<...>\hdd).
 *
 * Erkennung: Title-ID aus dem XBE-Zertifikat (sb_gamever_check, unveraendert).
 * Sobald der Client LoC startet (W275 sb_gamever-Profil), schaltet
 * lgame_loc_client_ready() den Start frei - die einzige Stelle dafuer.
 * ASCII-Quelltext. */
#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

enum { LGAME_SB = 0, LGAME_LOC = 1, LGAME_N = 2 };

#define LGAME_KEY_GAME "launcher_game"
#define LGAME_KEY_LOC_IMAGE "launcher_loc_image"
#define LGAME_KEY_LOC_SERVER "launcher_loc_server"
#define LGAME_KEY_LOC_WIDE "loc_widescreen" /* W326: liest der Client (hcfg) */

/* W326 LOC-WIDE: Bildformat im LoC-Profil. 16:9 und 720p sind experimentell
 * (nicht das Originalbild). LOC_WIDE_720P = 0 nimmt den Eintrag "720p (16:9)"
 * aus der Auswahl (gespeichertes 2 gilt dann als 4:3) - die einzige Stelle.
 * W326-Befund: 720p nicht angeboten - das Spiel gibt Menue-/HUD-Schrift in
 * 640x480-Pixelkoordinaten aus (ohne Supersampling-Skalierung unskaliert oben
 * links im 1280x720-Bild, LOC_BOOT §23). Client-Wert 2 bleibt fuer Tests. */
#ifndef LOC_WIDE_720P
#define LOC_WIDE_720P 0
#endif
enum { LGAME_WIDE_43 = 0, LGAME_WIDE_169 = 1, LGAME_WIDE_720P = 2 };

/* Title-IDs (XBE-Zertifikat) */
#define LGAME_TID_SB 0x43430002u      /* Steel Battalion */
#define LGAME_TID_LOC 0x43430009u     /* Line of Contact, World-Disc CC-009 */
#define LGAME_TID_LOC_JP 0x4343000Du  /* Line of Contact "Pilot Test" (Japan) */

typedef struct {
    int game;              /* LGAME_SB | LGAME_LOC */
    char loc_image[512];   /* UTF-8, leer = nicht gewaehlt */
    char loc_server[128];  /* leer = Standard (spaeter) */
    int loc_widescreen;    /* W326: LGAME_WIDE_* (nur angebotene Modi) */
} lgame_state_t;

void lgame_defaults(lgame_state_t *st);
const char *lgame_key(int game);       /* "sb" | "loc" */
int lgame_from_key(const char *v);     /* unbekannt/leer -> LGAME_SB */

/* W326 LOC-WIDE: Anzahl der angebotenen Bildformate (3, ohne 720p 2; der
 * Index in der Auswahl ist der Wert), Wert pruefen (ungueltig oder nicht
 * angeboten -> 0 = 4:3), experimentell (!= 4:3), Sprachschluessel. */
int lgame_wide_count(void);
int lgame_wide_clamp(int v);
int lgame_wide_experimental(int v);
const char *lgame_wide_label_key(int v);

/* user.cfg lesen (fehlende Datei/Schluessel = Standard). 0 = Datei gelesen,
 * 1 = fehlt. Letzte Zeile je Schluessel gewinnt (wie hcfg). */
int lgame_cfg_load(const char *usercfg_path, lgame_state_t *st);
/* Die vier Schluessel (W326: + loc_widescreen) in user.cfg schreiben (ersetzt vorhandene Zeilen,
 * entfernt Doppelte, haengt fehlende an; fremde Zeilen bleiben). 0 = ok,
 * -1 = Schreibfehler. Leere Werte werden als "key=" geschrieben. */
int lgame_cfg_save(const char *usercfg_path, const lgame_state_t *st);

/* LoC-Datenordner: base = Datenordner (Profil/portabel) bzw. Client-Ordner
 * (legacy). out = base\loc (Endtrenner von base egal). Rein, testbar. */
void lgame_loc_dir(const char *base, char *out, size_t cap);

/* Ergebnis der LoC-Abbild-Pruefung */
enum {
    LGAME_IMG_NONE = 0,     /* kein Pfad */
    LGAME_IMG_MISSING,      /* Datei/Ordner fehlt */
    LGAME_IMG_UNREADABLE,   /* kein Xbox-Abbild/keine default.xbe/kein XBE */
    LGAME_IMG_LOC,          /* Line of Contact (World-Disc) erkannt */
    LGAME_IMG_LOC_JP,       /* Line of Contact "Pilot Test" (Japan) */
    LGAME_IMG_SB,           /* Steel Battalion statt LoC */
    LGAME_IMG_OTHER         /* anderes Spiel */
};
typedef struct {
    int kind;               /* LGAME_IMG_* */
    uint32_t title_id;      /* 0 = nicht gelesen */
    uint32_t region;
    uint32_t version;
    char sha256[65];        /* SHA-256 der default.xbe ("" = nicht gelesen) */
    int startable;          /* 1 = Client startet LoC (lgame_loc_client_ready) */
} lgame_img_t;

/* Einordnung nach Title-ID (rein, testbar; xbe_ok = Zertifikat gelesen). */
int lgame_classify(int xbe_ok, uint32_t title_id);
/* XBE-Bytes einordnen (sb_gamever_check, keine Spielbytes gespeichert). */
void lgame_check_xbe(const uint8_t *xbe, size_t n, lgame_img_t *out);
/* Pfad pruefen: Abbild (.iso/xiso), entpackter Ordner (default.xbe darin)
 * oder direkt eine .xbe-Datei. Pfad UTF-8. Rueckgabe = out->kind. */
int lgame_loc_check(const char *path_u8, lgame_img_t *out);
/* Spielertext zum Ergebnis (aktive Sprache, sb_lang). Rueckgabe: Art fuer
 * die Anzeige 0 = neutral, 1 = ok (gruen), 2 = Fehler (rot), 3 = Hinweis. */
int lgame_img_msg(const lgame_img_t *r, const char *path_u8, char *msg, size_t cap);

/* 1 = der Client kann Line of Contact starten. Heute 0 (W275 arbeitet am
 * Start); hier - und nur hier - freischalten. SB_TEST_LOC_READY=1 erzwingt 1
 * (Selbsttest des spaeteren Wegs). */
int lgame_loc_client_ready(void);
/* W353: Freigabe der Ausgabe ohne Test-Haken (intern 1, community nur mit SB_LOC_PLAY). */
int lgame_loc_build_ready(void);
/* Startversuch LoC: 0 = darf starten, 1 = gesperrt (msg = "Coming soon"-Text
 * bzw. Grund aus der Abbild-Pruefung). */
int lgame_loc_start_gate(const lgame_img_t *r, char *msg, size_t cap);

/* Selbsttest der reinen Logik (Temp-Ordner, synthetische XBE ohne
 * Spielbytes). Meldet PASS/FAIL per printf (cli_printf). */
void lgame_selftest(int *pass, int *fail);

#ifdef __cplusplus
}
#endif
