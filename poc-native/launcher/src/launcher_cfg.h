#pragma once
/* launcher_cfg.h - Einstellungen des Launchers (A10, W26/W47).
 * Verwaltet die Nutzer-cfg user.cfg (liegt neben sb_native.cfg im
 * Client-Ordner, ueberschreibt sb_native.cfg; s. hcfg.c) sowie benannte
 * Profile (profiles/<name>.cfg im Client-Ordner). ASCII only. */
#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

typedef struct {
    char xbe_path[512];
    char media_root[512];
    char hdd_root[512];
    char game_image[512];   /* W150 ISO-DIRECT: Spiel-Abbild (Vorrang vor xbe_path/media_root), leer = Ordner-Modus */
    unsigned width, height;
    int present_interval; /* 0 = aus, 1 = VSync */
    int fs_interp_match; /* W207: exclusive refresh matching, default 1 */
    int fps_limit;        /* 0 = aus */
    int audio;            /* 0 = stumm, 1 = an */
    int audio_live;       /* 0 = nur Capture, 1 = Live-Ton */
    int aspect_mode;      /* W47: 1 = Pillarbox + Hor+ (nie strecken, S3.10) */
    char input_profile[64]; /* W78: Eingabe-Profil (profiles/<name>.ini), leer = Legacy-Tastatur */
    int sbc_usb;            /* W78: 1 = Original-Controller per WinUSB lesen */
    int sbc_mouse_off;      /* W128: 1 = Maus-Achsen aus, solange der Controller aktiv ist */
    int aim_assist;         /* W236: Zielhilfe 0 = Original, 1 = Fadenkreuz am Rand dreht die Sicht, 2 = Waffenarm fest (Altwert aim_turn=1 -> 1) */
    int auto_gear;          /* W218: 1 = automatischer Gangwechsel (vereinfachte Steuerung) */
    int game_tick;          /* W234 PAL50: Spieltakt 30 (Standard) oder 25 (wie EU-Konsole am 50-Hz-TV) */
    int interp_fps;         /* W78: Zwischenbilder 0/60/120 */
    int window_mode;        /* W81: 0 = Fenster, 1 = randloses Vollbild, 2 = exklusives Vollbild */
    int fs_hz;              /* W81: exklusiv: Bildwiederholrate (0 = Desktop) */
    int mouse_mode;         /* W87: 1 = absolut (Fadenkreuz folgt der Maus), 0 = relativ */
    int mouse_sens;         /* W87: Maus-Empfindlichkeit 1..16 ([input] mouse_sens) */
    /* W107 EDGE-MODE: [render] Bildraender ausserhalb der 4:3-Box (Hor+-Rand) */
    int edge_mode;          /* 0 = aus (volles Hor+), 1 = harter Schnitt, 2 = Abdunklung */
    int edge_fade_width;    /* Verlaufsbreite in % der 4:3-Boxbreite (0..50) */
    int edge_fade_strength; /* Endabdunklung in % (0..100) */
    int cockpit_panini;     /* W191 UW-PERSP: Panini der Cockpit-Ansicht 0 = aus, 1..100 % ([render] cockpit_panini) */
    int cockpit_width;      /* W235 COCKPIT-WIDTH: leichte Breitenanpassung der Cockpit-Ansicht 0 = Original, 1..10 % ([render] cockpit_width) */
    int cockpit_persp;      /* W191 UW-PERSP: Cockpit-Perspektive 0 = Original, 1..100 = Staerke in % ([render] cockpit_persp, nur Desktop) */
    /* W103 REM-FRAME: [remaster] (Client: hcfg.c, Standard alles aus) */
    int remaster;           /* Hauptschalter 0 = Original, 1 = Einzelschalter gelten */
    int hd_textures;        /* HD-Texturen (remaster/textures/x<hd_scale>) */
    int hd_scale;           /* 2 oder 4 (W114: Freigaben gelten immer; alt 0 -> 4) */
    int fx_enhanced;        /* Platzhalter */
    int ultrawide;          /* Platzhalter */
    int postfx;             /* Platzhalter */
    int monitor_grain;      /* W105: Monitor-Korn/Scanlines 0..100 % ([render] monitor_grain, 100 = Original) */
    int monitor_glitch;     /* W105: Monitor-Signalstoerung 0..100 % ([render] monitor_glitch, 100 = Original) */
    /* W131 SHADER-OPT2: Shader-Optionen ([render], Client hcfg.c; ESC-Menue
     * "Shader"/"Shader 2"). Reihenfolge = lcfg_shx_key(); Standard = Original. */
    int shx[25]; /* W153: + LSHX_LABEL */
    /* W173 LAUNCHER-P4: Effekte [render] fx_* (Client render_fx_rem.c, ESC
     * "Effekte"); gehoeren zum einheitlichen Grafik-Preset. */
    int fx[5];
    /* W134 VR-PROTO: [vr] (Client vr.c, Standard aus) */
    int vr_enable;          /* 0 = aus, 1 = VR (Brille per OpenXR, sonst Desktop-Vorschau) */
    char vr_preview[16];    /* W159: Desktop-Vorschau sbs|left|right|off
                                 * (Standard left = ein Auge); vr_enable
                                 * entscheidet NICHT mehr ueber den Startweg:
                                 * "Spielen" erzwingt aus, "Spielen in VR" an
                                 * (Start-Parameter, user.cfg unveraendert) */
    /* W183 VR-FIX3: VR-Leistung (Client vr.c, nur im VR-Betrieb wirksam). Preset
     * setzt die sechs Werte; Tabelle gespiegelt aus vr.c (kVrPreset). */
    int vr_preset;          /* 0 eigene, 1 Leistung, 2 Ausgewogen (Standard), 3 Qualitaet */
    int vr_render_scale;    /* Render-Aufloesung je Auge in % der Runtime-Empfehlung (Standard 80) */
    int vr_refresh_hz;      /* Bildrate der Brille (0 = Runtime) */
    int vr_mirror_res;      /* Desktop-Spiegel in % der Fenstergroesse */
    int vr_world_tick;      /* 1 = Aussenwelt/Monitore im Spieltakt */
    int vr_shading;         /* 1 = Cockpit-Shading in VR */
    int vr_rt_scale;        /* Welt-/Monitor-RT-Faktor im VR-Betrieb (0 = wie rt_scale) */
    /* W142 L10N-EN: [ui] lang ("en" | "de"). Community: "en"; private
     * Erststart: "de" (lcfg_defaults, SB_EDITION). */
    char lang[8];
    /* W148 PACK-LAUNCHER: [render] Render-Skalierung (Spieler-Standard: Auto dynamisch) */
    int rt_scale;             /* 0 = auto, 1..4 = fester Faktor */
    int rt_scale_dyn;         /* 1 = dynamisch (Regler), 0 = fest */
    /* W190 SAVE-LOCK: [game] save_lock (Client krnl_file.c, Standard 0 = Original) */
    int save_lock;            /* 1 = Spielstaende schuetzen (Testhilfe, Aenderungen beim Beenden verworfen) */
    /* W195 PHOTO-MODE: [photo] (Client render_photo.c) */
    char photo_dir[400];      /* Fotoordner (UTF-8 intern, ANSI in der Datei); leer = Standard (<Nutzerdaten>\Screenshots) */
    int photo_scale;          /* Standard-Aufnahmeaufloesung 1/2/4/8 x Fenster */
    int photo_jpg;            /* 0 = PNG, 1 = JPG */
    int photo_aspect;         /* Bildformat ohne Cockpit: 0 Bildschirm (Ausschnitt), 1 Bildschirm (weit), 2 Kamera (wie Monitor) */
} launcher_settings_t;

/* W131 SHADER-OPT2: Shader-Keys (Index in shx[]) */
enum {
    LSHX_SHADING, LSHX_FOG, LSHX_CON, LSHX_GAM, LSHX_SHARP, LSHX_BLOOM, LSHX_GLOW, LSHX_SPILL,
    LSHX_LIGHT, LSHX_EXPO, LSHX_TONE, LSHX_BTHR, LSHX_LWORLD, LSHX_LCKP, LSHX_FOCUS, LSHX_AA,
    LSHX_SSAO, LSHX_SAT, LSHX_TEMP, LSHX_LIFT, LSHX_CGAM, LSHX_GAIN, LSHX_SHADOW, LSHX_SOFT,
    LSHX_LABEL, LSHX_N /* W153 HUD-LABEL-OPT */
};
const char *lcfg_shx_key(int i);
int lcfg_shx_default(int i);
/* W173 LAUNCHER-P4: Effekt-Keys (Index in fx[]) */
enum { LFX_SOFT, LFX_EMBERS, LFX_LIGHT, LFX_LCKP, LFX_INTERP, LFX_N };
const char *lcfg_fx_key(int i);

/* W173 LAUNCHER-P4: EIN Grafik-Preset (UX-Konzept 3.3, PO 7.11.2026).
 * Es besitzt alle Optik-Schluessel: shading + 22 Shader-Werte + Schatten,
 * fx_* und HD-Texturen (Werte: harness/src/sb_gfxpreset.h, dieselbe Tabelle
 * wie im Client/ESC-Menue). Nicht dabei: hud_label_avoid (eigene Option
 * Grafik > HUD), Ausgabeformat, Cockpit-Monitor, Texturdetail.
 * W207: Bildrate, Welt-Aufloesung/Dynamik und Hz-Anpassung gehoeren dazu.
 * Abgeleitet (kein eigener Schalter): remaster = hd_textures, fx_enhanced =
 * irgendein Effekt > 0, ultrawide/postfx = 0 (Platzhalter). */
enum { LGP_ORIGINAL = 0, LGP_SUBTLE = 1, LGP_STRONG = 2, LGP_PERFORMANCE = 3, LGP_CUSTOM = 4 };
/* Schluessel-Index k (0..LGK_N-1): 0..LSHX_N-1 = shx[] (LSHX_LABEL gehoert
 * nicht dazu), LGK_FX0.. = fx[], LGK_HD = HD-Texturen. */
#define LGK_FX0 LSHX_N
#define LGK_HD (LSHX_N + LFX_N)
#define LGK_FPS (LGK_HD + 1)
#define LGK_RT (LGK_HD + 2)
#define LGK_DYN (LGK_HD + 3)
#define LGK_MATCH (LGK_HD + 4)
#define LGK_N (LGK_HD + 5)
typedef unsigned long long lgp_mask_t; /* Bit k = Schluessel k */
const char *lcfg_gfx_key(int k);              /* cfg-Name ("" = keiner) */
int lcfg_gfx_owned(int k);                    /* 1 = gehoert zum Preset */
int lcfg_gfx_value(const launcher_settings_t *s, int k); /* wirksam (HD: remaster && hd_textures) */
int lcfg_gfx_preset_value(int preset, int k); /* Sollwert (0..2), -1 = nicht im Preset */
/* Effect-Pack-Ausnahme: Maske der Schluessel, die ein aktives Effect Pack
 * setzt (Name wie kSbFxParams, z. B. "color_sat"; fremde Namen ignoriert). */
void lcfg_gfx_lock_key(lgp_mask_t *m, const char *key);
/* Erkanntes Preset (LGP_*); gesperrte Schluessel zaehlen nicht. */
int lcfg_gfx_preset(const launcher_settings_t *s, lgp_mask_t lock);
/* Abweichungen vom Preset p (Anzahl bzw. Maske; ohne gesperrte). */
int lcfg_gfx_diff(const launcher_settings_t *s, int p, lgp_mask_t lock, lgp_mask_t *mask);
/* Basis von Benutzerdefiniert: Preset mit den wenigsten Abweichungen. */
int lcfg_gfx_base(const launcher_settings_t *s, lgp_mask_t lock);
/* Preset 0..3 anwenden (alle Schluessel + Ableitungen; hud_label_avoid
 * und hd_scale bleiben). */
void lcfg_gfx_apply(launcher_settings_t *s, int preset);
/* Ableitungen setzen: remaster = hd_textures = (remaster && hd_textures),
 * fx_enhanced = irgendein fx > 0, ultrawide = postfx = 0. */
void lcfg_gfx_normalize(launcher_settings_t *s);
/* Alte Dateien: remaster=1 + fx_enhanced=1 ohne fx-Werte bedeutete im
 * Client "Effekte dezent" (render_fx_rem.c fx_cfg_once) -> explizit. */
void lcfg_gfx_migrate(launcher_settings_t *s);
/* Effekt-Stufe allein (0..2, LGP_CUSTOM = eigene Werte), gesperrte zaehlen nicht. */
int lcfg_fx_level(const launcher_settings_t *s, lgp_mask_t lock);
void lcfg_fx_apply_level(launcher_settings_t *s, int level);

void lcfg_defaults(launcher_settings_t *s);void lcfg_defaults(launcher_settings_t *s);
/* Datei parsen (gleiche Semantik wie hcfg: Sections egal, key=value,
 * # Kommentar). Unbekannte Keys ignoriert. 0 = geparst, 1 = fehlt. */
int lcfg_load_file(const char *path, launcher_settings_t *s);
/* W183 VR-FIX3: VR-Leistungs-Preset (1..3) anwenden / erkennen (0 = eigene) */
void lcfg_vr_preset_apply(launcher_settings_t *s, int preset);
int lcfg_vr_preset_match(const launcher_settings_t *s);
int lcfg_vr_preset_value(int preset, int k); /* k: 0 Aufloesung, 1 Bildrate, 2 Spiegel, 3 Aussenwelt, 4 Shading, 5 Monitor-RT */
/* Wie lcfg_load_file, zaehlt zusaetzlich die key=value-Zeilen (*keys,
 * darf NULL sein). Kaputte Zeilen (ohne '=') werden still uebersprungen:
 * eine teilweise lesbare Datei liefert trotzdem ihre gueltigen Keys. */
int lcfg_load_file_count(const char *path, launcher_settings_t *s, int *keys);
/* user.cfg schreiben; vorhandene fremde Zeilen/Kommentare bleiben erhalten.
 * 0 = ok, -1 = Schreibfehler. */
int lcfg_save_user(const char *path, const launcher_settings_t *s);
/* W79 INPUT-REAL: Spielerbetrieb-Block in user.cfg schreiben (ersetzt
 * vorhandene Zeilen dieser Keys, haengt fehlende an; fremde Zeilen bleiben):
 * replay_steps=, replay=, movie_skip=0, turbo_until_mode/frame=0,
 * screenshot_frame/every=0, audio_capture=0, window_hidden=0,
 * mouse_capture=1, input_focus_only=1, key_toggles=1, input_source=1, key_tap_queue=1,
 * input_test_hook=0. Aufruf beim Start
 * (GUI + CLI --start). 0 = ok, -1 = Schreibfehler. */
int lcfg_write_player_block(const char *path);
/* Block-Inhalt (Selbsttest/Anzeige): Anzahl, Key i (+ Wert). */
int lcfg_player_block_n(void);
const char *lcfg_player_block_key(int i, const char **value);
/* "k=v;k=v" anwenden (fuer --set; \; = maskiertes Semikolon).
 * Unbekannte Keys ignoriert. Immer 0. */
int lcfg_apply_set(launcher_settings_t *s, const char *set);
/* Verstaendliche Vorabpruefung: 0 = ok, 1 = Fehler (Text in err, aktive
 * Sprache via sb_lang; Fehlermeldungen sind Spielertexte). */
int lcfg_validate(const launcher_settings_t *s, char *err, size_t errcap);
/* W172 LAUNCHER-P6: wirksame Programmdatei (Ordner-Modus mit leerem
 * xbe_path: <media_root>/default.xbe; sonst xbe_path). lcfg_save_user
 * schreibt diesen Pfad, Start/Kommandozeile nutzen ihn. */
void lcfg_xbe_effective(const launcher_settings_t *s, char *out, size_t cap);
/* W150 ISO-DIRECT: Spiel-Abbild pruefen (lesbar, XDVDFS, default.xbe,
 * Spielversion). 0 = ok (msg = "Image OK: <Fassung>"), sonst Fehlertext
 * (Sprachschluessel cfg.image_*) in msg. Pfad UTF-8. */
int lcfg_image_check(const char *image_u8, char *msg, size_t cap);
/* W150: SHA-256 (hex) der default.xbe im Abbild. 1 = ok. */
int lcfg_image_xbe_sha256(const char *image_u8, char hex[65]);

/* W231 US-LAUNCHER: Fassung des Spiels (aus sb_gamever, gleiche Pruefung wie
 * der Client). EU = unterstuetzte PAL-Fassung; US = bekannte NTSC-U-Fassung
 * (experimental, solange sb_gamever sie nicht als supported fuehrt);
 * US_OTHER = Steel Battalion mit Region NA, aber unbekannter Hash. */
enum { LCFG_ED_NONE = 0, LCFG_ED_EU = 1, LCFG_ED_US = 2, LCFG_ED_US_OTHER = 3,
       LCFG_ED_OTHER = 4 };
typedef struct {
    int edition;      /* LCFG_ED_* (NONE = nicht lesbar/kein Spiel gewaehlt) */
    int startable;    /* 1 = Launcher startet (EU, US) */
    int needs_allow;  /* 1 = Client braucht [expert] allow_ntsc_u=1 (US, nicht supported) */
} lcfg_gameinfo_t;
/* Einordnung eines Pruefergebnisses (rein, testbar). Argument: sb_gamever_t*. */
void lcfg_gameinfo_from(const void *gamever, lcfg_gameinfo_t *gi);
/* Spielertext zum Pruefergebnis (sb_gamever_t*): 1 = startbar (msg =
 * gui.image_ok[_us]), 0 = nicht (msg = cfg.image_version/_us_other). */
int lcfg_gamever_msg(const void *gamever, char *msg, size_t cap);
/* Fassung des eingestellten Spiels (Abbild oder Ordner-Modus) lesen.
 * 1 = gelesen, 0 = kein lesbares Spiel (gi->edition = LCFG_ED_NONE). */
int lcfg_game_info(const launcher_settings_t *s, lcfg_gameinfo_t *gi);
/* Spielstand-Ordner je Fassung: EU = base, US = base\us (getrennt, gleiche
 * Title-ID 43430002 -> sonst vermischt). Rein, testbar. */
void lcfg_hdd_for_edition(const char *hdd_base, int edition, char *out, size_t cap);
/* --set-Zusatz je Fassung an die Kette set (Kapazitaet cap) anhaengen:
 * US: allow_ntsc_u=1 (nur solange needs_allow) und hdd_root_us=<us_hdd_acp>
 * (";" maskiert als "\;"). EU/sonst: nichts. Rein, testbar. 0 = ok,
 * 1 = Kette zu lang (unveraendert). */
int lcfg_edition_set_args(const lcfg_gameinfo_t *gi, const char *us_hdd_acp,
                          char *set, size_t cap);
/* Sprachschluessel fuer einen Client-Exitcode (5 = Fassung unbekannt,
 * 8 = Profilpruefung fehlgeschlagen) je Fassung; NULL = keine Meldung. */
const char *lcfg_exit_msg_key(unsigned long code, int edition);
/* W150 USER-DATA: Pfad der Spieler-cfg (legacy: <client>/user.cfg,
 * Profil/portabel: <Datenordner>/user.cfg). */
void lcfg_user_cfg_path(const char *client_dir, char *out, size_t cap);
/* Verzeichnis anlegen (mit Eltern). 0 = ok, -1 = Fehler. */
int lcfg_mkdir_p(const char *dir);
/* Client-Ordner finden (sb_ldr.exe + sb_native.cfg): override (z. B.
 * --client-dir), sonst SB_HARNESS_DIR, sonst Exe-Ordner, sonst
 * SB_HARNESS_DIR_DEFAULT (Buildzeit). out = bester Ordner, has_ldr = 1
 * wenn sb_ldr.exe dort liegt. Rueckgabe 0 = mit Lader, 1 = ohne. */
int lcfg_find_client_dir(const char *override_dir, char *out, size_t cap,
                         int *has_ldr);

/* --- Profile (W47): benannte Einstellungs-Saetze --------------------- */
/* Profilname-Regel: 1..63 Zeichen aus [A-Za-z0-9_.-]. 1 = ok, 0 = nein. */
int lcfg_profile_name_ok(const char *name);
/* Pfad profiles/<name>.cfg im Client-Ordner (prueft den Namen).
 * 0 = ok, 1 = ungueltiger Name. */
int lcfg_profile_path(const char *client_dir, const char *name, char *out,
                      size_t cap);
/* Alle Profile alphabetisch auflisten (NULL-Eintraege moeglich: names[i]
 * wird nur bei i < Rueckgabe beschrieben). Rueckgabe = Anzahl (0 = keine).
 * Fehlt der Ordner, ist das kein Fehler (0, kein Eintrag). */
int lcfg_profile_list(const char *client_dir, char names[][64], int max);
/* Profil laden: Defaults -> Profil-Datei. 0 = ok, 1 = Fehler (Text in err).
 * Fehlende Datei und ungueltiger Name sind Fehler (anders als user.cfg,
 * die optional ist). */
int lcfg_profile_load(const char *client_dir, const char *name,
                      launcher_settings_t *s, char *err, size_t errcap);
/* Profil speichern (legt profiles/ bei Bedarf an, erhaelt fremde Zeilen
 * wie lcfg_save_user). 0 = ok, 1 = Fehler (Text in err). */
int lcfg_profile_save(const char *client_dir, const char *name,
                      const launcher_settings_t *s, char *err, size_t errcap);
/* Profil loeschen. 0 = ok, 1 = Fehler (Text in err). */
int lcfg_profile_delete(const char *client_dir, const char *name, char *err,
                        size_t errcap);
/* Standard-Profil (Name in profiles/standard.txt). get: 0 = gesetzt
 * (Name in name), 1 = keines. set: 0 = ok (Name muss existieren),
 * 1 = Fehler. set mit name=NULL/"" loescht die Markierung. */
int lcfg_profile_get_default(const char *client_dir, char *name, size_t cap);
int lcfg_profile_set_default(const char *client_dir, const char *name,
                             char *err, size_t errcap);

#ifdef __cplusplus
}
#endif
