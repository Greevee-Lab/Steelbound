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
 *                               2 = 720p; ungueltig = 0; W414: fehlt = 1)
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

/* W326 LOC-WIDE: Bildformat im LoC-Profil. W414: 16:9 ist Standard und nicht mehr experimentell, 720p
 * bliebe experimentell (nicht angeboten). LOC_WIDE_720P = 0 nimmt den Eintrag "720p (16:9)"
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

/* W402 LAUNCHER-PERGAME: jedes Spiel hat eigene Einstellungen (Nutzer 21.11.). Steel Battalion: user.cfg
 * (wie bisher, dazu die launcher_*-Schluessel). Line of Contact: user_loc.cfg im selben Ordner; der Launcher
 * startet LoC mit SB_USER_CFG=<user_loc.cfg> (hcfg.c), der Client liest und speichert (ESC-Menue) dann nur dort.
 * Masken fuer lgame_cfg_save_ex (Bit = Index in kKeys). */
#define LGAME_KEY_PERGAME "launcher_pergame" /* 1 = Uebernahme der gemeinsamen Werte erledigt */
#define LGAME_LOC_CFG_NAME "user_loc.cfg"
enum {
    LGAME_K_GAME = 1, LGAME_K_IMAGE = 2, LGAME_K_SERVER = 4, LGAME_K_WIDE = 8, LGAME_K_PERGAME = 16,
    LGAME_K_LAUNCHER = 1 | 2 | 4 | 16 /* gehoeren in user.cfg */
};
/* Schluessel aus mask schreiben (ersetzen/anhaengen), Schluessel aus drop entfernen, alles andere bleibt. */
int lgame_cfg_save_ex(const char *usercfg_path, const lgame_state_t *st, unsigned mask, unsigned drop);
/* Einstellungsdatei des Spiels: SB = usercfg, LoC = user_loc.cfg im Ordner von usercfg. */
void lgame_cfg_path(const char *usercfg, int game, char *out, size_t cap);
/* 1 = user.cfg traegt launcher_pergame=1 */
int lgame_pergame_marked(const char *usercfg);
/* Einmalige Uebernahme: user.cfg vorhanden und nicht markiert -> (falls user_loc.cfg fehlt) Kopie ohne
 * launcher_*-, Pfad- und Update-Zeilen nach user_loc.cfg, dann Markierung in user.cfg (loc_widescreen dort
 * entfernt). 1 = uebernommen, 0 = nichts zu tun (neu oder schon markiert), -1 = Schreibfehler. Idempotent. */
int lgame_pergame_migrate(const char *usercfg, const char *loccfg);

/* W414 LOC-DEFAULT-169/-FADE (Nutzer 21.11.): in Line of Contact ist "16:9 (Spielmodus)" der Standard
 * (lgame_defaults), dazu Breitbild-Raender "Fade out" 8 %/100 %. Steel Battalion bleibt 4:3 (Original).
 * Einmalige Umstellung je user_loc.cfg: Markierung launcher_loc169=1 (launcher_* liest der Client nicht).
 * Danach bleibt jede manuelle Wahl erhalten (auch 4:3). user.cfg wird nie veraendert. */
#define LGAME_KEY_LOC169 "launcher_loc169"
#define LGAME_LOC169_EDGE_MODE 2 /* Fade out (Abdunklung) */
#define LGAME_LOC169_EDGE_W 8    /* Verlaufsbreite % */
#define LGAME_LOC169_EDGE_S 100  /* Staerke % */
/* 1 = user_loc.cfg traegt launcher_loc169=1 */
int lgame_loc169_marked(const char *loccfg);
/* Einmal-Umstellung: Datei vorhanden und nicht markiert -> loc_widescreen=1, edge_mode=2, edge_fade_width=8,
 * edge_fade_strength=100, launcher_loc169=1 (ersetzen/anhaengen, fremde Zeilen bleiben). 1 = umgestellt,
 * 0 = nichts zu tun (Datei fehlt = Standard gilt, oder schon markiert), -1 = Schreibfehler. Idempotent. */
int lgame_loc169_migrate(const char *loccfg);
/* Beim Schreiben der LoC-Datei durch den Launcher: loc_widescreen=<wide> + Markierung (legt die Datei an);
 * with_edges = 1 schreibt zusaetzlich die Rand-Standardwerte (neue Datei). 0 = ok, -1 = Fehler. */
int lgame_loc169_mark(const char *loccfg, int wide, int with_edges);

/* W438 CTRL-RESET-040 (Nutzerentscheid 22.11.): In 0.4.0 wird die Steuerung einmalig fuer alle auf
 * "default_full" gestellt (START auf Backspace, Enter frei fuer den LoC-Chat). Markierung
 * launcher_ctrl_reset_040=1 in user.cfg (launcher_* liest der Client nicht). Eigene user_*.ini bleiben als
 * Dateien unveraendert, es wird nur das aktive Profil umgestellt. */
#define LGAME_KEY_CTRL040 "launcher_ctrl_reset_040"
#define LGAME_CTRL040_PROFILE "default_full"
/* 1 = user.cfg traegt launcher_ctrl_reset_040=1 */
int lgame_ctrl040_marked(const char *usercfg);
/* Markierung setzen (Launcher speichert user.cfg; neue Installationen bekommen so nie einen Reset).
 * Datei fehlt -> nichts (1). 0 = ok/war schon da, -1 = Fehler. */
int lgame_ctrl040_mark(const char *usercfg);
/* Einmal-Reset: user.cfg fehlt (neue Installation) oder ist markiert -> 0, nichts geschrieben. Sonst
 * input_profile=default_full in user.cfg und, falls vorhanden, in loccfg (user_loc.cfg); danach die Markierung.
 * prev_sb/prev_loc: bisheriges Profil je Spiel ("" = Standard/Legacy bzw. LoC-Datei fehlt). Rueckgabe:
 * Bit 0 = SB umgestellt, Bit 1 = LoC umgestellt (je nur, wenn vorher ein anderes Profil galt), 0x100 =
 * nur markiert (galt schon default_full), -1 = Schreibfehler (keine Markierung, naechster Start versucht es
 * erneut). Idempotent; laeuft genau einmal je user.cfg. */
int lgame_ctrl040_reset(const char *usercfg, const char *loccfg, char *prev_sb, size_t cap_sb, char *prev_loc,
                        size_t cap_loc);

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
    int ver_unknown;        /* W384 (GH #52): LoC, aber unbekannte Fassung (z. B. Titel-Update) */
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
/* W353/W389: Freigabe der Ausgabe ohne Test-Haken (intern 1, community mit SB_LOC_PLAY, Standard seit 0.3.0). */
int lgame_loc_build_ready(void);
/* Startversuch LoC: 0 = darf starten, 1 = gesperrt (msg = "Coming soon"-Text
 * bzw. Grund aus der Abbild-Pruefung). */
int lgame_loc_start_gate(const lgame_img_t *r, char *msg, size_t cap);
/* W384 (GH #52): --set-Anfang fuer den LoC-Start (ACP, ';' maskiert). ISO/xiso ->
 * "loc_boot=1;game_image=<Abbild>"; .xbe oder entpackter Ordner -> Ordner-Modus
 * "loc_boot=1;game_image=;xbe_path=<xbe>;media_root=<Ordner>" (Client liest game_image
 * nur als ISO). xbe_u8 = Pfad fuer --xbe (u8). */
void lgame_loc_image_set(const char *img_u8, char *set, size_t cap, char *xbe_u8, size_t xcap);

/* Selbsttest der reinen Logik (Temp-Ordner, synthetische XBE ohne
 * Spielbytes). Meldet PASS/FAIL per printf (cli_printf). */
void lgame_selftest(int *pass, int *fail);

#ifdef __cplusplus
}
#endif
