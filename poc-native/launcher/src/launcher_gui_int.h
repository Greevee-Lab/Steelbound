/* launcher_gui_int.h - gemeinsamer interner Zustand der Launcher-Oberflaeche (W462 LAUNCHER-SPLIT).
 * Nur fuer launcher_gui*.c. Enthaelt die Includes, Layout-Makros, Typen und die extern-
 * Deklarationen der moduluebergreifend genutzten Funktionen/Variablen (Definition je im Modul,
 * Kommentar dort). Modulkarte: BERICHT_M2 Abschnitt W462. */
#ifndef LAUNCHER_GUI_INT_H
#define LAUNCHER_GUI_INT_H
#include "launcher_saves.h"
/* launcher_gui.c - Oberflaeche des Launchers (W82 LAUNCHER-UI2).
 * Randloses Fenster (WS_POPUP, eigene Titelleiste, DWM-Schatten),
 * Per-Monitor-DPI v2, W-API/UTF-16 an der Fenstergrenze (intern UTF-8),
 * eigene Zeichnung (dunkle Palette, halbtransparente Karten ueber einem
 * ausgeblichenen Hintergrundbild aus der Ressource, WIC). Funktionen wie
 * W26/W47/W78/W79: Grafik, Ton, Steuerung (Belegungs-Dialog), Profile,
 * Pfade, Start (user.cfg + Spielerbetrieb-Block + sb_ldr.exe).
 * Quelltext ASCII, Nutzertexte UTF-8 (Makros U8_* aus launcher_u8.h). */
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#define COBJMACROS
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <wincodec.h>
#include <math.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h> /* W328: Ablauf des Spielschluessels */
#include "launcher_app.h"
#include "launcher_cfg.h"
#include "launcher_ctrl.h"
#include "launcher_u8.h"
#include "sb_lang.h"
#include "launcher_res.h"
#include "sb_edition.h" /* W116 EDITIONS: Badge, Hintergrund, Remaster-Paket */
#include "sb_version.h" /* W301 BETA-VERSION: Phase-Badge, Fenstertitel */
#include "launcher_assist.h" /* W143 CTRL-ASSIST: Controller-Test-Dialog */
#include "sb_pack.h" /* W148 PACK-LAUNCHER: Seite "Packs" (Scan/Pruefen/packs.json) */
#include "sb_gamever.h"
#include "iso_vfs.h"
#include "sb_sha256.h" /* W148: XBE-Hash fuer V2 */
#include "sb_userdata.h" /* W150 USER-DATA */
#include "sb_savecheck.h" /* W187 KIA-LOAD: Spielstaende zuruecksetzen */
#include "sb_fpsrate.h" /* W223 FPS-RATES: Bildraten-Liste (wie ESC-Menue) */
#include <tlhelp32.h>
#include "launcher_rules.h" /* W171 LAUNCHER-P2P3: Regelwerk R1-R30 */
#include <shellapi.h> /* W148: ShellExecuteW (Ordner oeffnen) */
#include "input_sbc_usb.h"
#include "sbc_test.h"
#include "launcher_update.h" /* W250 UPDATE-CHECK */
#include "launcher_alpha.h" /* W272 ALPHA-TEST: Seite "Alpha Test" (launcher_alpha.inc) */
#include "launcher_diag.h" /* W279 DIAG-CRASH: Absturz-Karte + Diagnosepaket (launcher_diag.inc) */
#include "launcher_games.h" /* W276 LOC-LAUNCH */
#include "launcher_account.h" /* W328 STEELNET-ACCOUNT: Pilotenkonto */
#if defined(SB_HAVE_LOCTP) && SB_HAVE_LOCTP
#include "launcher_loctp.h" /* W336 LOC-TESTPARTNER (nur interne Ausgabe) */
#endif
#include "steelnet_pins.h" /* W334 LOC-SEC: SB_STEELNET_HOST/PORT/SPKI1/2 (Online-Karte LoC) */
#include "launcher_orig.h" /* W278 ORIG-FLAG */
#include "sb_origflag.h" /* W278: SB_OFG_* */
#include "launcher_layout.h" /* W281 LAUNCHER-SIZE: Layout 0/a/b (Entwurf) */

#define printf cli_printf
#define WIN_W 960
#define WIN_H 656                  /* W401 LAUNCHER-HEADER-POLISH: 640 -> 656 (Kopf +16, Inhalt gleich hoch) */
#define TITLE_H 72                 /* W397: 48 -> 56; W401: 56 -> 72 (Untertitel, Wortmarken, Balken mit Luft) */
#define NAV_W 208
#define NAVSUB_H 30                /* W399: Unterpunkt der Seitenleiste (Multiplayer > Match & network / Account) */
#define BOT_H 64
#define HDR_GAME_X 22                                  /* Wortmarke SB (Text bei x = 26 = Symbolspalte der Navigation) */
#define HDR_GAME_Y 30
#define HDR_GAME_W 214
#define HDR_GAME_H 36                                  /* Text 26 + Luft 5 + Balken 3 + Rand 2 */
#define HDR_GAME_GAP 20
#define HDR_GAME_PAD 4                                 /* Text-/Balkeneinzug im Tab */
#define HDR_GAME_END (HDR_GAME_X + 2 * HDR_GAME_W + HDR_GAME_GAP) /* 470: rechts davon die Gruppe */
#define HDR_SUB_Y 11                                   /* Projektzeile (+ Edition/Beta) */
#define HDR_SUB_H 16
#define HDR_WB_W 40                                    /* Fenster-Knoepfe _ x (Ecke oben rechts) */
#define HDR_WB_H 30                                    /* W403: 32 -> 30 (Zahnrad darunter, keine Ueberlappung) */
#define HDR_WB_X (WIN_W - 2 * HDR_WB_W)                /* 880 */
#define HDR_CHIP_Y 31                                  /* Gruppe: mittig zur Wortmarkenzeile (Text 30..56) */
#define HDR_CHIP_H 28
#define HDR_GEAR_W 36
#define HDR_GEAR_X (WIN_W - 28 - HDR_GEAR_W)           /* 896 */
#define HDR_CHIP_R (HDR_GEAR_X - 10)                    /* rechte Kante des Status-Chips */
#define CX0 (NAV_W + 28)           /* Inhalt links */
#define CW (WIN_W - CX0 - 28)      /* Inhalt Breite (696) */
#define CY0 (TITLE_H + 16)         /* Inhalt oben (W397: bleibt 72, Abstand zum Kopf 24 -> 16) */
#define COLW ((CW - 16) / 2)       /* Spalte (340) */
#define COL2 (CX0 + COLW + 16)
#define PKL_W 400                  /* linke Karten (Liste, Texturdetail) */
#define PKR_X (CX0 + PKL_W + 16)   /* rechte Karte (gewaehltes Pack) */
#define PKR_W (CW - PKL_W - 16)    /* 280 */
#define PK_ROW_H 38                /* Listenzeile (zwei Textzeilen) */
enum { P_PLAY, P_DISPLAY, P_GFX, P_PACKS, P_CTRL, P_AUDIO, P_ADV, P_ALPHA, P_N }; /* W272: + Alpha Test */
#define P_LNET P_PACKS /* W392 LAUNCHER-NETTAB: bei LoC steht auf dem Packs-Platz die Seite Netzwerk (W397: Multiplayer) */
#define P_LSET P_N
/* W470 LAUNCHER-HUB: Bereiche Spiel / Optionen (P_LSET) / steelnet (P_NETI). Optionen und steelnet stehen ausserhalb
 * der Spiel-Navigation und haben eigene Reiter (Tabelle je Bereich in launcher_gui_hub.c). */
#define P_NETI (P_N + 1)  /* steelnet > Info */
#define P_HUB_LAST P_NETI /* hoechste Seitennummer (Schleifen ueber alle Seiten) */
enum { HUB_GAME, HUB_OPTS, HUB_NET, HUB_N };
#define ID_HUBNAV0 2960   /* Reiter der Bereiche Optionen/steelnet (2960..2979, Lage = Tabelle) */
#define ID_HUBNAV_END 2980
#define ID_HUB_GOOPT 2980 /* steelnet > Info ohne Stufe steelnet: "Open options" */
#define ID_HUB_GOACC 2981 /* W472: steelnet > Info ohne (gueltige) Anmeldung: Weg zur Konto-Seite */
#define PG_ALL (-1)
#define PG_NOTPLAY (-2)
#define PG_DIALOG (-3)
/* --- IDs ---------------------------------------------------------- */
#define ID_NAV0 1000
#define ID_NAVSUB_MATCH 2870 /* W399: Multiplayer > Match & network (Unterpunkt der Leiste, nur LoC auf Multiplayer) */
#define ID_NAVSUB_ACC 2871   /* W399: Multiplayer > Account */
#define ID_MIN 1010
#define ID_CLOSE 1011
#define ID_START_BIG 1020
#define ID_START 1021
#define ID_START_VR 1038   /* W159: Spielen in VR (nur mit OpenXR-Runtime) */
#define ID_START_MENU 1039 /* W171: Fussleiste "Launch v" (Bildschirm / VR) */
#define ID_UNDO 1040       /* W171: Rueckgaengig (Muster C, Strg+Z) */
#define ID_DIAG 1041       /* W171: Diagnose ein-/ausklappen (Erweitert) */
#define ID_SAVE 1022
#define ID_BG 1023
#define ID_PRESET 101
#define ID_W 102
#define ID_H 103
#define ID_VSYNC 104
#define ID_FPS 105
#define ID_PACING 109 /* W468 PERF-PACING: Bildtakt Monitor/Timer/VRR (Erweitert > Feinabstimmung) */
#define ID_WINMODE 106
#define ID_ASPECT 107
#define ID_IPFPS 108
#define ID_MONFX 1024 /* W105: Monitor-Effekte */
#define ID_AAMODE 1032   /* W131: Kantenglaettung */
#define ID_VR 1033       /* W134 VR-PROTO, W159: VR-Vorschau (sbs / links / rechts / nur Brille) */
#define ID_LANG 1034     /* W142: Sprache (Sidebar, alle Seiten) */
#define ID_LANG_LBL 1035 /* W142: Beschriftung dazu */
#define ID_CTRL_ASSIST 1036 /* W143: Controller-Assistent oeffnen */
#define ID_REM_LABEL 1037 /* W153: HUD-Etikett weicht aus (W173: eigene Option Grafik > HUD, nicht im Preset) */
#define ID_FSHZ 110 /* W81: Bildwiederholrate (exklusiv) */
#define ID_REM_SCALE 112 /* W172: unsichtbarer Traeger fuer hd_scale (Texturdetail auf Packs) */
#define ID_EDGE 117   /* W107 EDGE-MODE: Bildraender */
#define ID_EDGE_W 118 /* W107: Verlaufsbreite */
#define ID_EDGE_S 119 /* W107: Abdunklung */
#define ID_AUDIO 120
#define ID_LIVE 121
#define ID_RTSCALE 123     /* W148: Render-Skalierung (Grafik) */
#define ID_PK_LIST 300     /* W148: Packs-Seite */
#define ID_PK_UP 301
#define ID_PK_DOWN 302
#define ID_PK_TOGGLE 303
#define ID_PK_ADD 304
#define ID_PK_DEL 305
#define ID_PK_CHECK 306
#define ID_PK_FOLDER 307
#define ID_PK_CAPRESET 311
#define ID_CTRL_PROF 140
#define ID_CTRL_REFRESH 141
#define ID_CTRL_BIND 142
#define ID_CTRL_SBC 143
#define ID_CTRL_SBCCHK 144
#define ID_CTRL_MOUSEOFF 1030 /* W128: Maus aus bei aktivem Controller */
#define ID_KEYS 145
#define ID_MOUSE_MODE 146
#define ID_MOUSE_SENS 147
#define ID_PROF_COMBO 160
#define ID_PROF_LOAD 161
#define ID_PROF_SAVE 162
#define ID_PROF_DEL 163
#define ID_PROF_DFLT 164
#define ID_XBE 180
#define ID_XBE_B 181
#define ID_MEDIA 182
#define ID_MEDIA_B 183
#define ID_HDD 184
#define ID_IMAGE 185   /* W150 ISO-DIRECT: Spiel-Abbild */
#define ID_IMAGE_B 186
#define ID_IMAGE_C 187
#define ID_SUM_GO0 1050    /* Spielen: "Aendern" je Bereitschaftszeile (5) */
#define SUM_GO_N 5
#define ID_PLAY_FOLDER 1055 /* Einrichtungskarte: "Ordner verwenden" */
#define ID_HDD_OPEN 1056   /* Erweitert: Spielstaende oeffnen */
#define ID_DATA_OPEN 1057  /* Erweitert: Datenort oeffnen */
#define ID_PK_LVL2 1058    /* Packs: Texturdetail 2x (Segment) */
#define ID_PK_LVL4 1059    /* Packs: Texturdetail 4x (Segment) */
#define ID_PK_CAT0 1060    /* Packs: Grenze je Kategorie (SB_PACK_NCAT Kombis) */
#define ID_PK_CREATOR 1069 /* Packs: Pack Creator oeffnen */
#define ID_PK_CHECKALL 1070
#define ID_GFX_PACKS 1071  /* Grafik: Sprung zur Seite Packs (Texturdetail) */
#define ID_GP0 1180        /* Original/dezent/stark/Performance/Klare Sicht/Enhanced/Vivid/Custom (1180..1187, W252, W465, W478) */
#define ID_GP_RESET 1084   /* Benutzerdefiniert: auf die Basis zuruecksetzen */
#define ID_GFX_LIGHT 1085  /* Weltbeleuchtung (shading) */
#define ID_GFX_SHADOW 1086 /* Schatten (shade_shadow) */
#define ID_GFX_SOFT 1087   /* Weiche Schatten (shadow_soft) */
#define ID_GFX_FX 1088     /* Effekte (Stufe der fx_*) */
#define ID_GFX_TEX 1089    /* Texturen: Original / HD aus Packs */
#define ID_CTRL_BROWSER 1090 /* W179: Tastenuebersicht als HTML im Browser */
#define ID_VRPRESET 1091   /* W183 VR-FIX3: VR-Leistung (Leistung/Ausgewogen/Qualitaet/Eigene) */
#define ID_VRRES 1092      /* W183 VR-FIX3: VR-Render-Aufloesung (% der Runtime-Empfehlung) */
#define ID_CTRL_ZADIG 1093 /* W185: Einrichtungsanleitung als HTML im Browser */
#define ID_SAVES_IMPORT 1198
#define ID_SAVES_EXPORT 1199
#define ID_SAVES_REPLAYS 1197 /* W254 REPLAY-LAUNCHER: Replays abspielen (experimentell) */
#define ID_SAVES_RESET 1094 /* W187 KIA-LOAD: Spielstaende zuruecksetzen (mit Sicherung) */
#define ID_SAVE_LOCK 1095  /* W190 SAVE-LOCK: Spielstaende schuetzen (Testhilfe) */
#define ID_PH_DIR 1096     /* W195 PHOTO-MODE: Extras - Fotoordner (Feld) */
#define ID_PH_BROWSE 1097  /* W195: Fotoordner durchsuchen */
#define ID_PH_OPEN 1098    /* W195: Fotoordner oeffnen */
#define ID_PH_RES 1099     /* W195: Standard-Aufnahmeaufloesung */
#define ID_PH_FMT 1100     /* W195: Format PNG/JPG */
#define ID_PH_ASP 1101     /* W195: Bildformat ohne Cockpit */
#define ID_CPERSP 1102 /* W191 UW-PERSP: Cockpit-Perspektive (Anzeige > Seitenverhaeltnis) */
#define ID_CPANINI 1103 /* W191 UW-PERSP: Panini der Cockpit-Ansicht */
#define ID_CTRL_AIMASSIST 1104 /* W236 AIM-ASSIST: Zielhilfe (Original / Rand dreht Sicht / Waffenarm fest) */
#define ID_CWIDTH 1107 /* W235 COCKPIT-WIDTH: leichte Breitenanpassung der Cockpit-Ansicht */
#define ID_CTRL_AUTOGEAR 1105 /* W218 SIMPLE-CONTROLS: automatischer Gangwechsel */
#define ID_CTRL_JOY 1106 /* W221 HOTAS (experimentell): Joystick-Dialog */
#define ID_CTRL_EXPORT 2930 /* W419 CTRL-EXPORT: Steuerung als .sbcontrols exportieren */
#define ID_CTRL_IMPORT 2931 /* W419: .sbcontrols importieren (als neues Profil) */
#define ID_CTRL_DESPIKE 2932 /* W439 LOC-RETICLE-JUMP: Ausreisser-Filter analoger Achsen (Standard an) */
#define ID_PAL50 1108 /* W234 PAL50: Spieltakt 25 (wie EU-Konsole am 50-Hz-TV) */
#define ID_CHIDE 1109 /* W268 COCKPIT-HIDE: Cockpit ausblenden (Grafik > HUD & Cockpit); W319: Auswahl Cockpit / Vollbild-Ansicht / Monitoransicht (+ Felder) */
#define ID_STICK_AIM 1111  /* W271 (Merge: 1109 belegt durch W268) STICK-RELATIVE: Stick-Zielen Position / wie eine Maus */
#define ID_STICK_SENS 1112 /* W271: Empfindlichkeit relativer Stick 1..16 */
#define ID_PH_VIEW 1113    /* W290 QUICK-SHOT: Ansicht der Aufnahme (wie im Spiel / ohne Cockpit / nur Aussenwelt) */
#define ID_RP_LABEL 1114   /* W290: REPLAY-Schriftzug der Wiedergabe zeigen */
#define ID_RP_LOGO 1115    /* W290: Spiel-Logo der Wiedergabe zeigen */
#define ID_WM_MEDIA 1116   /* W290: Wasserzeichen im Video-Export */
#define ID_PH_HUD 1117     /* W290: HUD im Foto */
#define ID_VOL_VOICE 1121
#define ID_CTRL_ALLY 1123  /* W424 SB-ALLIED-SQUADS: verbuendete Trupps kaempfen auch fern vom Spieler (nur SB) */
#define ID_HRTF 1122       /* W356 AUDIO-2: Kopfhoerer (3D-Klang/HRTF wie die Konsole) */
#define ID_PH_CSELF 1125   /* W445 PHOTO-CINE-SELF: Reichweite eigene VT-Teile (Cinematic) */
#define PHY_FMT 288        /* Format | Bildformat (vorher 290) */
#define PHY_CS 326         /* W445: Cinematic-Reichweite */
#define PHY_RP 362         /* Ueberschrift Replays & Video-Export (vorher 330) */
#define PHY_T1 380         /* Schalter Zeile 1 (vorher 348) */
#define PHY_T2 410         /* Schalter Zeile 2 (vorher 382) */
#define PHY_TXT 442        /* Hinweis/Fotoordner (vorher 420, Hoehe 44 -> 34) */
#define PHY_TXTH 34
#define VOL_N 21          /* Listen 100 %, 95 %, ... 0 % (5er-Schritte, wie ESC) */
#define EXY 18             /* W303: Karte "Fotos & Screenshots" 18 tiefer (Platz fuer die Lautstaerke) */
/* W250 UPDATE-CHECK: Erstabfrage + Hinweis (Spielen), Einstellung (Erweitert) */
#define ID_UPD_YES 1150
#define ID_UPD_NO 1151
#define ID_UPD_OPEN 1152
#define ID_UPD_AUTO 1153
#define ID_UPD_NOW 1154
#define ID_LSET_OPEN 2900  /* Zahnrad (Kopf, rechts): Launcher-Einstellungen */
#define ID_NETM0 2901      /* ... 2903: Auswahl steelnet / Nur Version / Offline (Seite P_LSET) */
#define ID_NETQ0 2904      /* ... 2906: Erstabfrage, gleiche drei Antworten (Karte auf Spielen, SB und LoC) */
#define ID_MP_GOSET 2908   /* LoC > Multiplayer: Hinweis "nicht verbunden" -> Launcher-Einstellungen */
#define ID_LOC_MP_GO 2909  /* LoC > Spielen: Karte Multiplayer -> Seite Multiplayer */
#define ID_LSET_RELPAGE 2910 /* Seite P_LSET: Release-Seite oeffnen (nur bei neuerer Version) */
#define LSET_LANG_Y CY0    /* Seite P_LSET: Karte Sprache & Fenster (rechts oben) */
#define LSET_R_X COL2
enum { NETM_STEEL, NETM_VER, NETM_OFF, NETM_N };
/* --- Farben ------------------------------------------------------- */
#define C_BASE RGB(11, 13, 17)
#define C_PANEL RGB(21, 24, 30)
#define C_TEXT RGB(234, 237, 242)
#define C_DIM RGB(152, 160, 172)
#define C_FAINT RGB(108, 116, 128)
#define C_ACCENT RGB(226, 150, 58)
#define C_ACCENT_HI RGB(242, 168, 78)
#define C_ACCENT_LO RGB(196, 126, 42)
#define C_ON_ACCENT RGB(24, 18, 10)
#define C_INPUT RGB(30, 34, 42)
#define C_INPUT_BRD RGB(62, 69, 82)
#define C_BTN RGB(44, 49, 60)
#define C_BTN_HI RGB(58, 64, 77)
#define C_OK RGB(92, 196, 124)
#define C_WARN RGB(232, 176, 64)
#define C_ERR RGB(236, 96, 84)
#define C_DANGER RGB(206, 112, 102) /* W399/W418: gedaempftes Rot (Konto loeschen) */
#define C_DIALOG RGB(24, 27, 33)
/* --- Steuerelement-Register ---------------------------------------- */
enum {
    K_BTN = 1, K_ACCENT, K_BIG, K_NAV, K_TOGGLE, K_TBMIN, K_TBCLOSE,
    K_SEG, /* W172: Segment-Knopf (gewaehlt = Akzent) */
    K_RADIO, /* W173: Optionsfeld mit Erklaerung ("Titel\nHilfe") */
    K_EDIT = 10, K_COMBO, K_COMBOEDIT, K_MEMO, K_STATIC, K_LIST,
    K_TRACK = 17, /* W143: Schieberegler (Totzonen) */
    K_PKLIST = 18, /* W172: Packs-Liste (eigene Zeichnung, Ziehen) */
    K_ICON = 19 /* W287: Symbol mit Tooltip (Static, eigene Zeichnung, Text = zugaenglicher Name) */
};
typedef struct {
    HWND h;
    HWND parent;
    int id;
    int page;
    int kind;
    int x, y, w, h_; /* logisch */
    int checked;
    int hover;
} uictl_t;
#define UI_MAX 320 /* W287: + Symbole; W373: 240 war voll (letzter Knopf des Belegungsdialogs fehlte); W487: 288 voll (+4 Gamertag) */
#define EDGE_W1 88
/* W278 ORIG-FLAG: Hinweis "nicht die Original-Optik" (Block "W278 ORIGFLAG" vor build_controls) */
#define ID_ORIG_RESET 2780     /* Spielen: "Back to original" (nur Grafik) */
#define ID_ORIG_ONCE 2781      /* Spielen: Einmal-Start im Original-Modus */
#define ID_ORIG_RESET_LOC 2782 /* LoC-Spielen: "Back to original" (W293: im Band unten) */
#define ID_XEXP_DISP 2783  /* Anzeige: Seitenverhaeltnis */
#define ID_XEXP_IMAGE 2784 /* Grafik: Bild */
#define ID_XEXP_WORLD 2785 /* Grafik: Welt */
#define ID_XEXP_HUD 2786   /* Grafik: HUD & Cockpit */
#define ID_XEXP_PACKS 2787 /* Packs: installierte Packs */
#define ID_XWARN_SB 2788   /* Spielen (SB): Hinweisband */
#define ID_XWARN_LOC 2789  /* Spielen (LoC): Hinweisband (W293: unten wie SB) */
#define ID_XEXP_FIRST ID_XEXP_DISP
#define ID_XEXP_LAST ID_XEXP_PACKS
#define ID_LAYOUT_LBL 2790 /* W287: Beschriftung "Groesse" vor der Layout-Kombi */
#define ID_XEXP_PHOTO 2791 /* W290: Kolben an der Karte "Fotos & Screenshots" (breit, REPLAY/Logo aus) */
#define ID_SHOWADV 2792    /* W291 ESC-REWORK: "Erweiterte Einstellungen zeigen" (Anzeige + Grafik) */
#define HDR_TIP_ID0 0x7F40              /* W403: Tooltip-Werkzeuge der Kopf-Chips (+ HDR_CHIP_ST/UPD) */
/* Schriften */
enum { F_BODY, F_SMALL, F_SEMI, F_CARD, F_BRAND, F_HERO, F_BIG, F_ICON,
       F_MONO, F_TINY, F_ICONSM, F_GAME, F_N }; /* W398: F_GAME = Wortmarken-Tabs im Kopf */
#define SX(v) (MulDiv((v), g_dpi, 96))
typedef struct {
    HDC dc;
    HBITMAP bmp;
    HGDIOBJ old;
    uint32_t *px;
    int w, h;
} canvas_t;
static const struct {
    const char *label; /* NULL = lkey uebersetzen */
    unsigned w, h;
    const char *lkey;
} kPresets[] = {
    { "640 " U8_TIMES " 480 (4:3)", 640, 480, NULL },
    { "1280 " U8_TIMES " 720 (16:9)", 1280, 720, NULL },
    { "1600 " U8_TIMES " 900 (16:9)", 1600, 900, NULL },
    { "1920 " U8_TIMES " 1080 (16:9)", 1920, 1080, NULL },
    { "2560 " U8_TIMES " 1440 (16:9)", 2560, 1440, NULL },
    { "2560 " U8_TIMES " 1080 (21:9)", 2560, 1080, NULL },
    { "3440 " U8_TIMES " 1440 (21:9)", 3440, 1440, NULL },
    { NULL, 0, 0, "gui.preset_custom" },
};
#define KPRESETS_N ((int)(sizeof kPresets / sizeof kPresets[0]))
/* W81: Bildwiederholrate im exklusiven Vollbild (0 = Desktop-Rate). */
static const struct {
    int hz;
    const char *label; /* NULL = lkey uebersetzen */
    const char *lkey;
} kFsHz[] = {
    { 0, NULL, "gui.fshz_desktop" }, { 60, "60 Hz", NULL },   { 75, "75 Hz", NULL },   { 100, "100 Hz", NULL },
    { 120, "120 Hz", NULL }, { 144, "144 Hz", NULL }, { 165, "165 Hz", NULL }, { 240, "240 Hz", NULL },
};
#define KFSHZ_N ((int)(sizeof kFsHz / sizeof kFsHz[0]))
static const int kEdgeW[] = { 5, 8, 10, 12, 18, 25 };
#define KEDGEW_N ((int)(sizeof kEdgeW / sizeof kEdgeW[0]))
static const int kEdgeS[] = { 60, 80, 90, 100 };
#define KEDGES_N ((int)(sizeof kEdgeS / sizeof kEdgeS[0]))
#define ID_LAYOUT 1290 /* W281: Layout-Vorschau (Navigation unten, nur a/b) */
#define XI_W 30 /* W287: Experimentell-Symbol in der Kartenkopfzeile (wie unten) */
#define LAY_TEX_Y (CY0 + 316)  /* W291: Karte Texturen (links unter der Voreinstellung) */
#define LAY_ADV_Y (CY0 + 470)  /* W291: Schalter "Erweiterte Einstellungen zeigen" */
/* Seite Spielen: Kurzstatus (Zeilen) */
typedef struct {
    const char *label;
    char value[400];
    COLORREF col;
} sumrow_t;
#define SUM_N 5
enum { PSR_TXT, PSR_MSG, PSR_DISC, PSR_FOLDERQ, PSR_N };
typedef struct {
    const char *text;
    int font, x, y, w, h, single, reason;
    COLORREF col;
} psrow_t;
#define UPD_X (CX0 + 340)
#define UPD_Y (CY0 + 78)
#define UPD_W (CW - 340)
/* W183 VR-FIX3: Stufen der VR-Render-Aufloesung (Kombi ID_VRRES) */
static const int kVrRes[] = { 50, 60, 65, 70, 80, 85, 90, 100, 120 };
#define KVRRES_N ((int)(sizeof kVrRes / sizeof kVrRes[0]))
/* W178 COMM-BG: beide Editionen haben die Launcher-Karte (Hintergrundbild). */
#define ADV_DIAG_Y (CY0 + 160) /* W468: +24 (Zeile Bildtakt); W250: +32 (Launcher-Karte mit Update-Schalter); W397: Karte weg, -120 */
#define ADV_DIAG_DY 34 /* W190: Diagnose-Zeilen unter dem Schalter "Spielstaende schuetzen" */
/* Standardbelegung (Legacy) = keyboard_default (input_map.c eingebaut). */
static const char ks_legacy_text[] =
    "[buttons]\n"
    "bi0 = KEY:SPACE, MOUSE:LEFT\n"
    "bi1 = KEY:F, MOUSE:RIGHT\n"
    "bi2 = KEY:L, MOUSE:MIDDLE\n"
    "bi3 = KEY:SHIFT+KEY:DELETE\n" /* W403: Eject (Standard, Akkord gegen Versehen) */
    "bi4 = KEY:H\n"
    "bi5 = KEY:I\n"
    "bi6 = KEY:ENTER\n"
    "bi8 = KEY:M\n"
    "bi9 = KEY:TAB\n"
    "bi11 = KEY:Z\n"
    "bi12 = KEY:X\n"
    "bi13 = KEY:V\n"
    "bi18 = KEY:C\n"
    "bi20 = KEY:O\n"
    "bi21 = KEY:N\n"
    "bi27 = KEY:R\n"
    "[axes]\n"
    "aim_x.neg = KEY:LEFT:cond=!SHIFT\n"
    "aim_x.pos = KEY:RIGHT:cond=!SHIFT\n"
    "aim_x = MOUSE:X:mode=aim\n"
    "aim_y.neg = KEY:UP:cond=!SHIFT\n"
    "aim_y.pos = KEY:DOWN:cond=!SHIFT\n"
    "aim_y = MOUSE:Y:mode=aim\n"
    "rot.neg = KEY:A\n"
    "rot.pos = KEY:D\n"
    "sight_x.neg = KEY:LEFT\n"
    "sight_x.pos = KEY:RIGHT\n"
    "sight_x = MOUSE:X:mode=sight\n"
    "sight_y.neg = KEY:UP\n"
    "sight_y.pos = KEY:DOWN\n"
    "sight_y = MOUSE:Y:mode=sight\n"
    "pedal_l = KEY:LCTRL, MOUSE:X1, MOUSE:X2\n"
    "pedal_m = KEY:S\n"
    "pedal_r = KEY:W\n"
    "[gear]\n"
    "gear.up = KEY:E\n"
    "gear.down = KEY:Q\n"
    "tuner.up = KEY:T\n"
    "tuner.down = KEY:G\n"
    "[vr]\n"
    "vr_recenter = KEY:F8\n"
    "[client]\n"
    "pause = KEY:PAUSE\n"
    "feedback_shot = KEY:F11\n"
    "feedback_clip = KEY:F9\n"
    "photo = KEY:SHIFT+KEY:F12\n"
    "quick_shot = KEY:F12\n"
    "orig_view = KEY:SHIFT+KEY:F8\n"
    "replay_exit = KEY:BACKSPACE\n"
    "net_hud = KEY:F3\n"; /* W283; W290 photo/quick_shot; W299 replay_exit; W374 net_hud */
enum { KS_BTN, KS_TOG, KS_EJECT, KS_DIAL, KS_GEAR, KS_LEVER, KS_TRIG,
       KS_KNOB, KS_SIGHT, KS_SBTN, KS_PEDAL, KS_ROT, KS_LEDR, KS_LEDG,
       KS_LEDB };
enum { KS_G_LEFT, KS_G_CENTER, KS_G_RIGHT, KS_G_PEDALS, KS_G_N };
typedef struct {
    const char *id;    /* CSS-Kennung: el-<id>, co-<id>, e-<id> */
    const char *lkey;  /* Name (Tabelle, Tooltip) */
    const char *skey;  /* Kurzname in der Zeichnung (NULL = lkey) */
    const char *elems; /* Profil-Elemente, je ein Leerzeichen getrennt */
    unsigned char shape, grp;
    short x, y, w, h;  /* Form (Knopf/Hebel: Rechteck; Kreis: Mitte+r) */
    short lx, ly, lw;  /* Callout-Kasten; lw = 0: Zeilen unter der Kappe */
    short ax, ay;      /* Leader-Start (0 = keiner) */
} ks_elem_t;
#define KS_C(id, lk, sk, el, sh, g, x, y, w, h) \
    { id, lk, sk, el, sh, g, x, y, w, h, 0, 0, 0, 0, 0 }
#define KS_COM(id, c) \
    KS_C(id, "bind." id, NULL, id, KS_BTN, KS_G_CENTER, 428 + (c) * 46, 76, \
         40, 30)
#define KS_LED(id, sk, sh, c, r) \
    KS_C(id, "bind." id, sk, id, sh, KS_G_CENTER, 668 + (c) * 78, \
         76 + (r) * 80, 72, 26)
#define KS_ROW(id, c, y) \
    KS_C(id, "bind." id, NULL, id, KS_BTN, KS_G_CENTER, 434 + (c) * 152, y, \
         140, 24)
#define KS_MM(id, sk, c, r) \
    KS_C(id, "bind." id, sk, id, KS_BTN, KS_G_RIGHT, 936 + (c) * 116, \
         384 + (r) * 80, 108, 24)
#define KS_TG(id, sk, x, y) \
    KS_C(id, "bind." id, sk, id, KS_TOG, KS_G_LEFT, x, y, 88, 24)
static const ks_elem_t ks_elems[] = {
    /* Linker Block */
    { "gear", "gui.svg_gear", NULL,
      "gear.up gear.down gear.r gear.n gear.1 gear.2 gear.3 gear.4 gear.5",
      KS_GEAR, KS_G_LEFT, 26, 92, 116, 360, 24, 470, 120, 84, 452 },
    { "rot", "gui.svg_rot", NULL, "rot rot.neg rot.pos",
      KS_ROT, KS_G_LEFT, 176, 196, 204, 16, 160, 68, 114, 252, 182 },
    { "sight", "gui.svg_sight", NULL,
      "sight_x sight_y sight_x.neg sight_x.pos sight_y.neg sight_y.pos",
      KS_SIGHT, KS_G_LEFT, 278, 204, 14, 0, 282, 250, 114, 284, 217 },
    { "bi33", "bind.bi33", NULL, "bi33",
      KS_SBTN, KS_G_LEFT, 278, 204, 6, 0, 282, 68, 114, 281, 199 },
    KS_TG("bi34", "gui.svg_t_filter", 158, 552),
    KS_TG("bi35", "gui.svg_t_oxygen", 194, 472),
    KS_TG("bi36", "gui.svg_t_fuel", 254, 552),
    KS_TG("bi37", "gui.svg_t_buffer", 288, 472),
    KS_TG("bi38", "gui.svg_t_vtloc", 310, 392),
    /* Mittelblock */
    KS_COM("bi28", 0), KS_COM("bi29", 1), KS_COM("bi30", 2),
    KS_COM("bi31", 3), KS_COM("bi32", 4),
    { "tuner", "gui.svg_tuner", NULL, "tuner.up tuner.down",
      KS_DIAL, KS_G_CENTER, 540, 250, 58, 0, 480, 344, 120, 540, 312 },
    KS_LED("bi16", NULL, KS_LEDR, 0, 0),
    KS_LED("bi17", "gui.svg_s_ext", KS_LEDR, 1, 0),
    KS_LED("bi18", NULL, KS_LEDR, 2, 0),
    KS_LED("bi19", NULL, KS_LEDG, 0, 1),
    KS_LED("bi20", NULL, KS_LEDG, 1, 1),
    KS_LED("bi21", NULL, KS_LEDG, 2, 1),
    KS_LED("bi22", NULL, KS_LEDB, 0, 2),
    KS_LED("bi23", NULL, KS_LEDB, 1, 2),
    KS_LED("bi24", NULL, KS_LEDB, 2, 2),
    KS_ROW("bi13", 0, 456), KS_ROW("bi14", 1, 456), KS_ROW("bi15", 2, 456),
    KS_ROW("bi25", 0, 540), KS_ROW("bi26", 1, 540), KS_ROW("bi27", 2, 540),
    /* Rechter Block: Zielhebel. bi0 (Report "MainWeapon") = grosse
     * Daumentaste oben, bi1 ("Fire", Sub-Waffe) = Abzug vorn, bi2 =
     * Lock-On oben (Quellen uneinig, s. BERICHT_M2 W184). */
    { "aim", "gui.svg_aim", NULL,
      "aim_x aim_y aim_x.neg aim_x.pos aim_y.neg aim_y.pos",
      KS_LEVER, KS_G_RIGHT, 1046, 104, 66, 220, 926, 236, 96, 1046, 200 },
    { "bi0", "bind.bi0", NULL, "bi0",
      KS_TRIG, KS_G_RIGHT, 1080, 112, 22, 18, 1124, 76, 96, 1102, 121 },
    { "bi1", "bind.bi1", NULL, "bi1",
      KS_TRIG, KS_G_RIGHT, 1030, 146, 16, 44, 926, 150, 96, 1030, 168 },
    { "bi2", "bind.bi2", NULL, "bi2",
      KS_KNOB, KS_G_RIGHT, 1062, 121, 8, 0, 926, 76, 96, 1054, 121 },
    { "bi3", "bind.bi3", "gui.svg_eject", "bi3",
      KS_EJECT, KS_G_RIGHT, 1250, 84, 36, 40, 1206, 152, 94, 1268, 136 },
    KS_MM("bi7", "gui.svg_s_open", 0, 0), KS_MM("bi8", NULL, 1, 0),
    KS_MM("bi9", NULL, 0, 1), KS_MM("bi10", NULL, 1, 1),
    KS_MM("bi11", NULL, 0, 2), KS_MM("bi12", NULL, 1, 2),
    KS_C("bi4", "bind.bi4", NULL, "bi4", KS_BTN, KS_G_RIGHT, 1192, 380, 100,
         34),
    KS_C("bi5", "bind.bi5", NULL, "bi5", KS_BTN, KS_G_RIGHT, 1192, 464, 100,
         34),
    KS_C("bi6", "bind.bi6", NULL, "bi6", KS_BTN, KS_G_RIGHT, 1192, 548, 100,
         34),
    /* Pedale: Slide-Step links, Bremse Mitte, Gas rechts */
    { "pedal_l", "gui.svg_pedal_l", NULL, "pedal_l",
      KS_PEDAL, KS_G_PEDALS, 452, 716, 84, 124, 420, 868, 150, 0, 0 },
    { "pedal_m", "gui.svg_pedal_m", NULL, "pedal_m",
      KS_PEDAL, KS_G_PEDALS, 590, 738, 140, 102, 586, 868, 150, 0, 0 },
    { "pedal_r", "gui.svg_pedal_r", NULL, "pedal_r",
      KS_PEDAL, KS_G_PEDALS, 780, 704, 84, 136, 752, 868, 150, 0, 0 },
};
enum { KS_SRC_KEY, KS_SRC_MOUSE, KS_SRC_PAD, KS_SRC_N };
static const char ks_fixture_text[] =
    "# W182 Fixture (keine Spieldaten)\n"
    "[buttons]\n"
    "bi0 = KEY:SPACE, MOUSE:LEFT, PAD:RB\n"
    "bi1 = KEY:F, MOUSE:RIGHT, PAD:B\n"
    "bi2 = KEY:L, MOUSE:MIDDLE, PAD:LB\n"
    "bi3 = PAD:BACK+PAD:Y\n"
    "bi4 = KEY:H, PAD:BACK+PAD:X\n"
    "bi5 = KEY:I, PAD:BACK+PAD:A\n"
    "bi6 = KEY:ENTER, PAD:START\n"
    "bi8 = KEY:M\n"
    "bi9 = KEY:TAB\n"
    "bi10 = PAD:DL, PAD:DR\n"
    "bi11 = KEY:Z\n"
    "bi12 = KEY:X\n"
    "bi13 = KEY:V\n"
    "bi17 = PAD:L3\n"
    "bi18 = KEY:C, PAD:A\n"
    "bi20 = KEY:O, PAD:BACK+PAD:B\n"
    "bi21 = KEY:N\n"
    "bi25 = PAD:Y\n"
    "bi26 = PAD:X\n"
    "bi27 = KEY:R, PAD:LB+PAD:RB\n"
    "bi28 = KEY:F5\n"
    "bi33 = PAD:R3\n"
    "bi34 = PAD:BACK+PAD:DL\n"
    "bi35 = PAD:BACK+PAD:DR\n"
    "[axes]\n"
    "aim_x.neg = KEY:LEFT:cond=!SHIFT\n"
    "aim_x.pos = KEY:RIGHT:cond=!SHIFT\n"
    "aim_y.neg = KEY:UP:cond=!SHIFT\n"
    "aim_y.pos = KEY:DOWN:cond=!SHIFT\n"
    "aim_x = MOUSE:X:mode=aim, PAD:LX:dz=0.15:curve=1.5\n"
    "aim_y = MOUSE:Y:mode=aim, PAD:LY:dz=0.15:inv=1\n"
    "rot.neg = KEY:A\n"
    "rot.pos = KEY:D\n"
    "rot = PAD:RX:dz=0.12\n"
    "sight_x.neg = KEY:LEFT:cond=SHIFT\n"
    "sight_x.pos = KEY:RIGHT:cond=SHIFT\n"
    "sight_y = PAD:RY:inv=1\n"
    "pedal_l = KEY:LCTRL, MOUSE:X1, PAD:BACK+PAD:LT\n"
    "pedal_m = KEY:S, PAD:LT:dz=0.05\n"
    "pedal_r = KEY:W, PAD:RT:dz=0.05\n"
    "[gear]\n"
    "gear.up = KEY:E, PAD:BACK+PAD:RB\n"
    "gear.down = KEY:Q, PAD:BACK+PAD:LB\n"
    "gear.n = KEY:0\n"
    "tuner.up = KEY:T, PAD:DU\n"
    "tuner.down = KEY:G, PAD:DD\n";
#define ZADIG_URL "https://zadig.akeo.ie/"
#define KRC_MAX 13
static const struct {
    int rule;
    int ids[KRC_MAX];
} kRuleCtl[] = {
    { 1, { ID_MEDIA, ID_MEDIA_B, ID_XBE, ID_XBE_B, 0 } },
    { 3, { ID_START_BIG, ID_START_VR, ID_START, ID_START_MENU, 0 } },
    { 4, { ID_START_BIG, ID_START_VR, ID_START, ID_START_MENU, 0 } },
    { 5, { ID_START_VR, 0 } },
    { 7, { ID_PRESET, ID_W, ID_H, 0 } },
    { 8, { ID_FSHZ, 0 } },
    { 11, { ID_EDGE, ID_EDGE_W, ID_EDGE_S, 0 } },
    { 12, { ID_EDGE_W, ID_EDGE_S, 0 } },
    { 15, { ID_GFX_SHADOW, ID_GFX_SOFT, 0 } }, /* W173: Schatten brauchen Weltbeleuchtung */
    { 16, { ID_GFX_SOFT, 0 } },
    { 18, { ID_PK_LVL2, ID_PK_LVL4, ID_PK_CAT0, ID_PK_CAT0 + 1, ID_PK_CAT0 + 2,
            ID_PK_CAT0 + 3, ID_PK_CAT0 + 4, ID_PK_CAT0 + 5, ID_PK_CAT0 + 6,
            ID_PK_CAT0 + 7, ID_PK_CAT0 + 8, ID_PK_CAPRESET, 0 } },
    { 19, { ID_PK_CAT0, ID_PK_CAT0 + 1, ID_PK_CAT0 + 2, ID_PK_CAT0 + 3,
            ID_PK_CAT0 + 4, ID_PK_CAT0 + 5, ID_PK_CAT0 + 6, ID_PK_CAT0 + 7,
            ID_PK_CAT0 + 8, ID_PK_CAPRESET, 0 } },
    { 20, { ID_GFX_FX, 0 } }, /* W173: Effekt-Stufe (Muster D) */
    { 21, { ID_PK_TOGGLE, 0 } },
    { 23, { ID_CTRL_MOUSEOFF, 0 } },
    { 24, { ID_MOUSE_MODE, ID_MOUSE_SENS, 0 } },
    { 27, { ID_LIVE, 0 } },
    { 31, { ID_PK_ADD, 0 } },
};
#define KRULECTL_N ((int)(sizeof kRuleCtl / sizeof kRuleCtl[0]))
#define IDB_LIST 501
#define IDB_NAME 511
#define IDB_RESULT 514
#define IDB_CLEAR 533  /* W282 ISSUE6: leeres Layout (alle Spiel-Elemente leeren) */
static const char kCxFixture[] =
    "# Steelbound controls: key bindings and control settings.\n"
    "[steelbound_controls]\nformat = 1\nlauncher = 0.3.2\ngame = sb\nprofile = user_hotas\n"
    "future_key = 1\n"
    "\n[settings]\nmouse_mode = relative\nmouse_sens = 6\nstick_aim = relative\nstick_aim_sens = 5\n"
    "aim_assist = 0\nauto_gear = 0\nsbc_usb = 0\nsbc_mouse_off = 0\nmouse_invert_y = 1\npad_dz_stick = 12\n"
    "joy_cal0 = 044F:B10A,X,-30000,200,31000,4,0\nfuture_setting = 3\n"
    "\n[profile]\njoy0 = T.16000M\njoy1 = TWCS Throttle\n"
    "bi0 = KEY:SPACE, MOUSE:LEFT, JOY0:B1\nbi1 = KEY:F, MOUSE:RIGHT, JOY0:B2\n"
    "aim_x = MOUSE:X:mode=aim, JOY0:X:dz=0.05:mode=position\naim_y = MOUSE:Y:mode=aim, JOY0:Y:dz=0.05:inv=1\n"
    "rot = JOY0:RZ:dz=0.1:curve=1.5\npedal_r = KEY:W, JOY1:Z:inv=1\npedal_m = KEY:S\npedal_l = KEY:LCTRL\n"
    "gear.up = KEY:E, JOY1:B1\ngear.down = KEY:Q, JOY1:B2\ntuner.up = KEY:T, JOY0:POV1U\n"
    "tuner.down = KEY:G, JOY0:POV1D\nchaff = KEY:C, PAD:LB+PAD:X\npause = KEY:P\n"
    "orig_view = KEY:SHIFT+KEY:F8\n"
    "\n[end]\n";
#define PK_MAXN 32
#define NAV_ORDER_N 7
#define ID_GAME_SB 2760
#define ID_GAME_LOC 2761
#define ID_LOC_START 2762
#define ID_LOC_IMAGE 2763
#define ID_LOC_CHECK 2764
#define ID_LOC_BROWSE 2765
#define ID_LOC_FOLDER 2766
#define ID_LOC_GO0 2767 /* W293: "Change" je Bereitschaftszeile (LOC_SUM_N) */
#define LOC_SUM_N 5
#define ID_LOC_SBLOOK 2779  /* W459: Schalter "SB look (Experimental)" (LoC > Erweitert, unter Bildformat) */
#define ID_LOC_WIDE 2772   /* W326 LOC-WIDE: Auswahl Bildformat (LoC > Erweitert) */
#define ID_LOC_WIDE_XI 2773 /* W326: Experimentell-Symbol an der Karte "Bildformat" */
#define ID_LOC_ONL 2774       /* W334 LOC-SEC: Schalter "Online-Spiel" (Karte "Online (steelnet)", LoC-Spielen) */
#define ID_LOC_CODE 2775      /* W334: Raumcode zum Beitreten (leer = eigene Lobby mit Raumcode) */
#define ID_LOC_CODE_COPY 2776 /* W334: Raumcode der eigenen Lobby kopieren (nur sichtbar mit Code) */
#define ID_LOC_VIS 2777       /* W373 LOC-PUBLIC-LOBBY: Auswahl Oeffentlich (Standard) / Privat (mit Raumcode) */
#define ID_LOC_SMOOTH 2778    /* W387 LOC-SMOOTH-OPP: Auswahl Gegner glaetten Aus / Auto (Standard) / Fest 1 Tick */
#define ID_LNET_RB 2793    /* W447: Auswahl Netzmodus Auto (Standard) / Rollback / Lockstep (frueher Schalter Rollback-Netcode, loc_rb_input) */
#define ID_LNET_OWN 2794   /* Schalter eigene Bewegung sofort (loc_own_predict) */
#define ID_LNET_LOWD 2795  /* Schalter adaptive niedrigere Eingabeverzoegerung (loc_rb_opts lowd=0 = aus) */
#define ID_LNET_RESET 2796 /* Knopf Standard wiederherstellen (alles an, Gegner glaetten Auto) */
#define ID_LNET_CLIP 2797  /* W420 LOC-CLIPDUMP: Schalter "Clip dumping unterbinden (Host-Regel)" */
#define ID_LNET_VERPAGE 2798 /* W420: Knopf Release-Seite bei Versionsablehnung (steelnet, nur dann sichtbar) */
#define ID_LNET_CHAT 2799  /* W437 LOC-CHAT: Schalter "Chat" (loc_chat, Standard an) */
#define ID_LNET_MODE 2800  /* W440 LOC-LOBBY-MODES: Auswahl Lobby-Modus beim Hosten (Sim = Standard / Arcade) */
#define ID_LNET_SNCHAT 2801 /* W446 STEELNET-CHAT: Schalter "steelnet chat" (loc_snchat, Standard an) */
#define ID_LNET_PRIV 2802 /* W450 LAUNCHER-PRIVACY-INFO: Knopf "Chat privacy" (kleines Fenster mit dem Datenschutz-Hinweis) */
#define ID_LNET_KIND 2803    /* W454 LOC-LAN-MODE: Auswahl Online (steelnet) / LAN (Karte Match, Kopf) */
#define ID_LNET_LANROOM 2804 /* W454: Raumname im LAN (leer = "LAN") */
#define ID_LNET_LANPASS 2805 /* W454: Raumpasswort im LAN (optional, nie gespeichert) */
#define ID_LNET_TAB0 2806   /* W473 MPPOLISH: Reiter "Network" der Netzwerk-Karte (Segment) */
#define ID_LNET_TAB1 2807   /* W473: Reiter "Lobby & chat" */
#define ID_LNET_MSN 2808    /* W473: Schalter steelnet-Chat im Spiel erlauben (user_loc.cfg loc_chat_mission_sn, Standard 0) */
#define ID_LNET_MLOBBY 2809 /* W473: Schalter Lobby-Chat im Spiel erlauben (loc_chat_mission_lobby, Standard 1) */
#define ID_LNET_MTEAM 2810  /* W473: Schalter Team-Chat im Spiel erlauben (loc_chat_mission_team, Standard 1) */
#define ID_LNET_CAMP 2812   /* W491 CAMP-CLIENT: nur intern - Auswahl Kampagne Saison / Testkampagne (Lobby & Chat) */
#define ID_NRQ_YES 2813     /* W496 NET-TELEMETRY: Dialog "anonyme Netzwerkstatistik" - Ja (Standard-Knopf) */
#define ID_NRQ_NO 2814      /* W496: ... Nein danke */
#define ID_LNET_NETREP 2815 /* W496: Schalter "Anonyme Netzwerkstatistik" (LoC > Netzwerk, Reiter Network) */
#define ID_LNET_CHATBAR 2816 /* W508 CHATBAR: Schalter "Chat in den Seitenbalken" (loc_chat_pos, Standard an = auto) */
/* W496 (launcher_gui_netrep.c) */
extern int g_nrq_cfg, g_nrq_dismiss, g_nrq_open, g_nrq_asked_now;
int nrq_should_ask(int cfg, int dismiss, int steel, int upd_ask);
int nrq_consent(void);
void nrq_set_append(char *set, size_t cap, int onl);
__declspec(noinline) void w496_gui_selftest(int *pass, int *fail);
void nrq_load(void);
void nrq_eval(void);
void nrq_set(int yes);
void nrq_dismiss(void);
void nrq_build(HWND w);
void nrq_relabel(void);
int nrq_visible(int id);
int nrq_command(int id);
void nrq_paint(void);
#define LOC_SUM_Y (CY0 + 216) /* Karte "Ready to play" (wie SB) */
#define LOC_SUM_H 220         /* 8 px Luft bis zum Band (OB_Y_LOC) */
#define LOC_DISC_Y CY0        /* Erweitert: Disc-Karte */
#define LOC_DISC_H 220
#define LOC_DATA_Y (CY0 + 232)
#define LOC_DATA_H 118
#define LOC_ONL_Y CY0 /* W397: Karte Account auf LoC > Multiplayer rechts oben (frueher Erweitert unten links) */
#define LOC_ONL_H 184 /* W473: +34 (Match: Zeile Verbindung; Konto: Zustand + Erklaerung) */
#define ACC_X COL2    /* W397: x der Karte Account */
#define LOC_WIDE_Y (CY0 + 136) /* W397: Launcher-Karte weg */
#define LOC_WIDE_H 204
#define LOC_SBL_Y (LOC_WIDE_Y + LOC_WIDE_H + 8) /* W459: Karte "SB look" unter Bildformat (endet 8 px ueber dem Band) */
#define LOC_SBL_H 88
#define LOC_NET_X UPD_X
#define LOC_NET_Y UPD_Y
#define LOC_NET_W UPD_W
#define LOC_NET_H 138 /* W373: drei Zeilen (Schalter + Sichtbarkeit, Raumcode, Hinweis), Vertrauenshinweis in der Karte */
#define ONL_TIMER 26 /* W334: loc_net_status.txt nachlesen (2 s, nur LoC-Spielen) */
#define LOC_ISO "D:\\Games\\Line of Contact.iso"
#define LOC_SB_ISO "D:\\Games\\Steel Battalion (Europe).iso"
#define ID_LOCTP_OPEN 2880
#define ID_LOCTP_STOP 2881
#define LOC_SMOOTH_N 3
#define LNET_RBOPTS_MAX 127 /* hcfg.h loc_rb_opts[128] */
#define LNET_MODE_KEY "launcher_loc_play_mode"
/* W353 LOC-DURCHSTICH: err= (Fehlergrund des Clients, loc_tun.c tl_write_status) -> Meldung de/en */
static const char *const kOnlErr[][2] = {
    { "dns", "gui.loc_onl_err_dns" },         /* kein Netz bzw. Treffpunkt-Name nicht aufloesbar */
    { "connect", "gui.loc_onl_err_connect" }, /* Aufbau zum Treffpunkt gescheitert (Netz/Firewall/Dienst aus) */
    { "lost", "gui.loc_onl_err_lost" },       /* Verbindung zum Treffpunkt verloren, neuer Versuch */
    { "code", "gui.loc_onl_err_code" },       /* Raumcode nicht gefunden */
    { "full", "gui.loc_onl_err_full" },       /* Lobby voll */
    { "rate", "gui.loc_onl_err_rate" },       /* zu viele Versuche */
    { "path", "gui.loc_onl_err_path" },       /* Mitspieler weder direkt noch ueber Relay erreichbar */
    { "closed", "gui.loc_onl_err_closed" },   /* W366: Host hat die Lobby geschlossen (Spiel beendet) */
};
#define ONL_ERR_N ((int)(sizeof kOnlErr / sizeof kOnlErr[0]))
#define ID_LOC_GATE_GO 2920 /* LoC > Spielen: "steelnet waehlen" -> Launcher-Einstellungen */
#define ID_LOC_IMG_GO 2921 /* LoC > Spielen: "Disc-Image waehlen" -> LoC > Erweitert */
#ifndef SB_NET_VERSION
#define SB_NET_VERSION ""
#endif
#define ID_LOC_VER_GO 2922 /* LoC > Spielen: "Release-Seite oeffnen" bei Versionssperre */
#define WM_APP_ACC (WM_APP + 22) /* Ergebnis eines Konto-Auftrags (l = acc_job_t*) */
#define ID_ACC_NAME 2850       /* Karte/Seite: Kontoname */
#define ID_ACC_CREATE 2851     /* Karte/Seite: Konto erstellen */
#define ID_ACC_X_LOGIN 2852    /* W418: Login und Passwort (W502: Knopf, oeffnet das Formular in der Karte) */
#define ID_ACC_SIGNIN 2853     /* Karte: Ich habe schon ein Konto (-> Kontoseite) */
#define ID_ACC_DISCORD 2854    /* Karte: Mit Discord verbinden */
#define ID_ACC_MANAGE 2855     /* Karte: Konto verwalten (-> Kontoseite) */
#define ID_LOC_MP_ACC 2856     /* W418: LoC > Spielen, Karte Multiplayer -> Kontoseite */
#define ID_ACC_V_DISCORD 2857  /* Seite: Discord anmelden / sichern / verbinden */
#define ID_ACC_V_LOGIN 2858    /* Seite: Login-Name */
#define ID_ACC_V_PASS 2859     /* Seite: Passwort (maskiert) */
#define ID_ACC_V_LOGINBTN 2860 /* Seite: anmelden / sichern / speichern */
#define ID_ACC_V_CODE 2861     /* Seite: ein Codefeld (Wiederherstellungs- oder Uebertragungscode, Format entscheidet) */
#define ID_ACC_V_CODEBTN 2862
#define ID_ACC_X_CODE 2863     /* W418: "Mit einem Code" (W502: Knopf, oeffnet das Codeformular in der Karte) */
/* 2864: frueher Aufklapper "Verwalten" (W502: Karte "Kontoeinstellungen", immer sichtbar) */
#define ID_ACC_V_RECSHOW 2865  /* Seite (mit Konto): Code anzeigen */
#define ID_ACC_V_RECCOPY 2866  /* Seite (mit Konto): Code kopieren */
#define ID_ACC_V_DELETE 2867   /* Seite: Konto loeschen (Rueckfrage) */
#define ID_ACC_V_FORGET 2868   /* Seite: auf diesem PC abmelden */
#define ACC_PRIV_Y (WIN_H - BOT_H - 46) /* Datenschutzzeile (zwei Zeilen, ueber der Fussleiste) */
#define ACC_TIMER 418        /* W418: Namenspruefung beim Tippen (entprellt) */
/* W487 LIVE-GAMERTAG: Gamertag-Felder (ausserhalb ID_ACC_FIRST..LAST, dort liegen ID_NAVSUB_*) */
#define ID_ACC_GT 2874       /* Kontoseite, kein Konto: Gamertag beim Erstellen (Vorschlag vom Dienst) */
#define ID_ACC_X_GT 2875     /* Kontoseite, Gamertag-Karte mit Gamertag: "Aendern ..." (W502: Knopf statt Aufklapper im Kopf) */
#define ID_ACC_V_GT 2876     /* Kontoseite, Gamertag-Karte: neuer Gamertag */
#define ID_ACC_V_GTBTN 2877  /* Kontoseite, Gamertag-Karte: setzen/aendern */
#define ACC_GT_TIMER 487     /* W487: Gamertag-Pruefung beim Tippen (entprellt) */
/* W502 ACCOUNT-PAGE: Uebersicht "Was du spielen kannst" (je Zeile ein Knopf zur naechsten Aktion) und Abbrechen der
 * eingebetteten Formulare (Gamertag aendern, Login, Code) */
#define ID_ACC_O_ONLINE 2882   /* Zeile Online: Konto erstellen (kein Konto) bzw. Erneut versuchen (Status unbekannt) */
#define ID_ACC_O_LIVE 2883     /* Zeile Xbox Live & Kampagne: Gamertag waehlen (springt zur Gamertag-Karte) */
#define ID_ACC_O_SEC 2884      /* Zeile Gesichert: Konto sichern (springt zur Karte Sichern) */
#define ID_ACC_V_GTCANCEL 2885
#define ID_ACC_V_LGCANCEL 2886
#define ID_ACC_V_CDCANCEL 2887
#define ACC_W502_ID(id) ((id) >= ID_ACC_O_ONLINE && (id) <= ID_ACC_V_CDCANCEL)
#define ACC_HL_TIMER 502       /* Hervorhebung der Zielkarte nach einem Sprung */
enum { ACC_CARD_NONE, ACC_CARD_OV, ACC_CARD_GT, ACC_CARD_SEC, ACC_CARD_SET, ACC_CARD_NEW, ACC_CARD_HAVE, ACC_CARD_OFF };
enum { ACC_OP_STATUS = 1, ACC_OP_CHECK, ACC_OP_CREATE, ACC_OP_DISCORD_LINK, ACC_OP_DISCORD_LOGIN,
       ACC_OP_LOGIN_SET, ACC_OP_LOGIN, ACC_OP_TRANSFER, ACC_OP_RECOVERY, ACC_OP_DELETE,
       ACC_OP_GT_CHECK, ACC_OP_GT_SET };
typedef struct {
    HWND h;
    int op, gen;
    char a[64];          /* Name / Login / Code / W487: Gamertag */
    char b[300];         /* Passwort (wird geloescht) */
    char c[64];          /* W487: Gamertag beim Erstellen bzw. Vorschlagsbasis (gamertag_check suggest) */
    int fid;             /* W487: Feld der Gamertag-Pruefung (ID_ACC_GT / ID_ACC_V_GT) */
    char dir[1200];
    lacc_srv_t srv;
    lacc_store_t st;     /* Eingang: aktuelles Konto; Ausgang: neues Konto */
    lacc_status_t status;
    lacc_gtcheck_t gtc;  /* W487 */
    lacc_gtset_t gts;    /* W487 */
    int avail, rc;
    char err[64];
} acc_job_t;
/* W418: Zustand der Kontoseite (ein Zustand, ein naechster Schritt) */
enum { ACCS_OFF, ACCS_NONE, ACCS_UNSEC, ACCS_SEC, ACCS_UNK };
/* W502 ACCOUNT-PAGE: eine Rechnung fuer Steuerelemente und Zeichnen (logisch). Mit Konto: Uebersicht, Gamertag,
 * darunter Sichern (links) | Kontoeinstellungen (rechts). Kein Konto: Uebersicht, darunter Erstellen | Schon ein Konto.
 * -1 = Teil nicht da. */
typedef struct {
    int st;
    int ov_y, ov_h;          /* Uebersicht "Was du spielen kannst" */
    int gt_y, gt_h;          /* Gamertag-Karte (mit Konto) */
    int lo_y, lo_h;          /* untere Reihe */
    int lx, lw, rx, rw;      /* linke / rechte Karte der unteren Reihe */
    int r_dc, r_lg, r_rec;   /* Zeilen der Karte Sichern (y) */
} acc_geo_t;
typedef struct {
    const char *label;
    char value[400];
    COLORREF col;
} loc_sumrow_t;
#define OB_Y_LOC (CY0 + 444)
#define OB_H_LOC 52
#define XI_W 30 /* W287: Experimentell-Symbol in der Kartenkopfzeile */
#define SNP_FAST_MS (60 * 1000)      /* Fenster sichtbar: Timertakt und Abstand */
#define SNP_SLOW_MS (5 * 60 * 1000)  /* minimiert/verborgen */
#define HDR_SUB_SEP " \xC2\xB7 "
enum { HDR_CHIP_ST, HDR_CHIP_UPD };
typedef struct {
    int n;
    int kind[2], x[2], w[2], shortened;
    char txt[2][96];
    COLORREF col[2], dot[2];
} hdr_lay_t;
#define W220_ISO "D:\\Games\\Steel Battalion (Europe).iso"
#define W220_ISO_US "D:\\Games\\Steel Battalion (USA).iso"
#define W220_ISO_JP "D:\\Games\\Steel Battalion (Japan).iso" /* W231: nicht unterstuetzt */
#define W220_DIR "D:\\Games\\Steel Battalion"
#define GT_OK(name)                    \
    do {                               \
        printf("PASS %s\n", name);     \
        (*pass)++;                     \
    } while (0)
#define GT_FAIL(name, fmt, ...)                          \
    do {                                                 \
        printf("FAIL %s: " fmt "\n", name, __VA_ARGS__); \
        (*fail)++;                                       \
    } while (0)

#if defined(SB_HAVE_LOCTP) && SB_HAVE_LOCTP
#define LOCTP_TIMER 336 /* W336: Haupt-Timer des Testpartners (launcher_loctp_gui.inc) */
#endif

/* ---- moduluebergreifend genutzte Funktionen und Variablen (W462) ---- */
extern const char *const kPageKey[P_N];
extern const wchar_t kPageIcon[P_N];
extern int g_alpha_tools;
extern int g_show_adv;
extern int g_loctp_mode_cfg;
int gui_page_count(void);
const char *gui_page_key(int p);
int gui_page_from_name(const char *v);
extern const int kVolId[4];
extern const char *const kVolKey[4];
extern int g_vol_raw[4];
extern const char *g_upd_ver;
extern int g_upd_cfg;
extern int g_upd_ask;
extern int g_upd_busy;
extern int g_upd_gen;
extern int g_upd_manual;
extern lupd_res_t g_upd_res;
extern char g_img_msg[600];
extern int g_img_ok;
extern uictl_t g_ui[UI_MAX];
extern int g_ui_n;
extern gui_opts_t g_o;
extern HWND g_hwnd;
extern int g_dpi;
extern int g_dpi_font;
extern llay_t g_lay;
extern int g_page;
extern int g_updating;
extern char g_client_dir[1024];
extern char g_client_disp[1024];
extern int g_has_ldr;
extern char g_note[256];
extern int g_note_kind;
extern char g_status[1024];
extern int g_status_kind;
extern char g_aspect_info[512];
extern char g_sbc_txt[512];
extern char g_pad_txt[160];
extern char g_padlive_txt[512];
extern char g_prof_info[256];
extern char g_profile_line[256];
extern int g_focus_id;
extern unsigned g_prev_h;
extern unsigned g_prev_w;
extern int g_prev_edge;
extern int g_prev_es;
extern int g_prev_ew;
extern int g_bg_on;
extern lr_res_t g_rr[LR_N];
extern const lr_env_t *g_env_fix;
extern int g_diag_open;
extern int g_sbc_rc;
extern int g_disp_hz;
extern int g_disp_hz_max;
extern int g_mon_alt_hz;
extern unsigned g_mon_h;
extern unsigned g_mon_w;
extern char g_dflt_setup[64];
extern int g_confirm_auto;
extern launcher_settings_t g_undo_s;
extern int g_undo_on;
extern int g_setup_force;
extern int g_bb_setup;
extern int g_data_ok;
extern int g_edition;
extern void *g_client_proc;
extern int g_client_ed;
extern char g_data_err[300];
extern HWND g_tip;
extern int g_creator;
extern launcher_settings_t g_gfx;
extern int g_tex_src;
extern int g_game;
const char *page_name_key(int p);
extern unsigned g_orig_mask;
extern int g_rem_pkg;
int cfg_read_key(const char *path, const char *key, char *out,
                        size_t cap);
int is_dir_u8(const char *p);
int rem_pkg_detect(const char *client_dir, char *dir, size_t cap);
extern launcher_settings_t g_s;
extern launcher_settings_t g_cur;
extern HFONT g_f[F_N];
extern int g_has_icons;
extern int g_icon_fluent;
extern wchar_t g_face_body[64];
void ui_settext(HWND h, const char *u8);
void ui_gettext(HWND h, char *out, size_t cap);
void trim_u8(char *s);
uictl_t *ui_find(int id);
uictl_t *ui_by_hwnd(HWND h);
HWND ctl(int id);
int ui_checked(int id);
void ui_set_checked(int id, int on);
int ui_msgbox(HWND owner, const char *text, const char *title,
                     UINT flags);
extern canvas_t g_base;
extern canvas_t g_bb;
void cv_free(canvas_t *c);
int cv_make(canvas_t *c, int w, int h);
void cv_fill(canvas_t *c, int x, int y, int w, int h, COLORREF col,
                    int alpha, int rad);
void cv_ring(canvas_t *c, int x, int y, int w, int h, COLORREF col,
                    int alpha, int rad, int t);
void cv_text(canvas_t *c, HFONT f, const char *u8, int x, int y,
                    int w, int h, COLORREF col, UINT fmt);
void cv_textw(canvas_t *c, HFONT f, const wchar_t *wt, int n, int x,
                     int y, int w, int h, COLORREF col, UINT fmt);
int lay_b(void);
int lay_tw(void);
int lay_th(void);
void lay_px(int x, int y, int w, int h, int sw, int sh, RECT *r);
void lay_paint_px(int x, int y, int w, int h, RECT *r);
void L_fill(int x, int y, int w, int h, COLORREF c, int a, int r);
void L_ring(int x, int y, int w, int h, COLORREF c, int a, int r);
void L_text(int font, const char *u8, int x, int y, int w, int h,
                   COLORREF c, UINT fmt);
void fonts_free(void);
void fonts_make(void);
extern uint32_t *g_img;
extern int g_img_h;
extern int g_img_w;
extern uint32_t *g_bgs;
int bg_load(void);
void bg_cover_rect(int sw, int sh, int dw, int dh, WICRect *out);
int bg_scaled(int dw, int dh);
int png_save(const uint32_t *px, int w, int h, const char *path);
void ui_cfg_path(const char *client_dir, char *out, size_t cap);
int ui_cfg_load_key(const char *client_dir, const char *key, int dflt);
void ui_cfg_load_str(const char *client_dir, const char *key, char *out, size_t cap);
extern int g_lay_cfg_id;
extern int g_lay_cfg_size;
int ui_cfg_load_tools(const char *client_dir);
int ui_cfg_load_bg(const char *client_dir);
int ui_cfg_save_bg(const char *client_dir, int bg);
HWND mk(HWND parent, int kind, int id, int page, int x, int y, int w,
               int h, const char *text, DWORD extra);
void ui_px_rect(const uictl_t *u, RECT *r);
void ui_place(uictl_t *u);
int disp_preview_y(void);
void lay_gfx_apply(void);
void gfx_preset_layout(void); /* W465: sichtbare Preset-Optionsfelder ohne Luecken */
int ui_visible_on(const uictl_t *u, int page);
void keys_tabstops(void);
void ui_relayout(void);
void base_build(void);
void card(int x, int y, int w, int h, const char *title);
void label(int font, const char *t, int x, int y, int w, int h,
                  COLORREF c);
void label1(int font, const char *t, int x, int y, int w, int h,
                   COLORREF c);
void dot(int x, int y, COLORREF c);
void aspect_box_for(unsigned w, unsigned h, unsigned *bw, unsigned *bh);
extern sumrow_t g_sum[SUM_N];
extern const int kSumPage[SUM_N];
extern char g_vr_hint[256];
extern int g_vr_present;
int paint_reason(int rule, int x, int y, int w, int h);
int first_rule(const int *rules, int n);
int launch_blocked(void);
extern char g_img_path[1024];
extern char g_imgfix_path[1024];
extern int g_imgfix_rc;
extern char g_imgfix_msg[600];
extern int g_imgfix_ed;
int img_check(const char *path, char *msg, size_t cap);
void imgfix_set(const char *path, int rc, const char *msg);
void play_setup_rows(psrow_t *rw);
void upd_tag_disp(const char *tag, char *out, size_t cap);
void vrres_fill(HWND c);
int vrres_index(int pct);
void extras_default_dir(char *out, size_t cap);
int extras_dir_ok(const char *d);
void extras_dir(char *out, size_t cap);
void adv_data_loc(char *path, size_t cap, const char **mode);
void adv_hdd_path(char *out, size_t cap);
void saves_edition_hint(const char *src_u8, int edition, char *out, size_t cap);
extern int g_save_test_busy;
int client_running(void);
int adv_saves_reset_at(const char *hdd_u8, int check_running);
void adv_saves_transfer(int exporting);
void paint_adv_shared(void);
void redraw_all(void);
void redraw_rect(int x, int y, int w, int h);
void set_status_k(const char *msg, int kind);
void set_status_undo(const char *msg, const launcher_settings_t *before);
void set_status(const char *msg);
unsigned xwarn_page_mask(int page);
int xexp_preset(int p);
void xexp_sfx(char *out, size_t cap);
int xexp_title_len(int id, const wchar_t *txt, int n1);
void draw_item(const DRAWITEMSTRUCT *d);
extern HBRUSH g_br_dialog;
extern HBRUSH g_br_input;
LRESULT ctlcolor(HDC dc, HWND child, int is_static_bg);
void set_num(int id, unsigned v);
unsigned get_num(int id);
void lay_combo_fill(HWND c);
void combo_add(HWND cb, const char *u8);
void cself_fill(HWND cb);
int mon_sim_apply(const char *spec);
void mon_detect_rates(void);
extern int g_ip_keep;
int ip_sel_of(int v);
int ip_val(int sel, int dflt);
void ip_fill(HWND c);
void ip_set(int v);
void ip_status(void);
void sync_preset(void);
void cpersp_fill(HWND c);
void cwidth_fill(HWND c);
void edge_ui_set(int mode, int width, int strength);
void edge_from_settings(const launcher_settings_t *s);
void edge_to_settings(launcher_settings_t *s);
void update_aspect_info(void);
void refresh_ctrl_profiles(const char *keep);
void ctrl_profile_from_ui(char *out, size_t cap);
void update_ctrl_overview(void);
int ctrl_profile_load(const char *prof, char *buf, size_t cap,
                             size_t *n_out);
void keyshtml_escape(const char *s, char *out, size_t cap);
void ks_atom(const char *a, char *out, size_t cap, int *src);
int ks_selfcheck(const char *html, int prof, char *why, size_t cap);
int ks_layoutcheck(char *why, size_t cap);
const char *ks_uncovered(void);
void keyshtml_build(const char *memo, const char *profile,
                           const char *prof_text, const char *prof_missing,
                           const char *mouse_lbl, int mouse_sens,
                           const char *pad_txt, const char *pad_live,
                           const char *sbc_txt, char *out, size_t cap);
void keyshtml_open(void);
void zadig_build(char *out, size_t cap);
void zadig_open(void);
void update_sbc_status(void);
void update_vr_button(void);
void joy_card_scan(void);
int update_pad_status(void);
void refresh_profiles(void);
void profile_name_from_ui(char *out, size_t cap);
void gfx_name(int p, const launcher_settings_t *s, char *out, size_t cap);
void gfx_state_name(const launcher_settings_t *s, char *out, size_t cap);
void ui_settext_if(HWND h, const char *u8);
extern const int kMonFx[4][2];
extern int g_monfx_glitch;
extern int g_monfx_grain;
void gfx_sync_ui(void);
void ui_from_settings(const launcher_settings_t *s);
void ui_to_settings(launcher_settings_t *s);
void update_summary(void);
int path_eq(const char *a, const char *b);
void xbe_derived(const launcher_settings_t *s, char *out, size_t cap);
#if !SB_EDITION_COMMUNITY
int creator_path(char *out, size_t cap);
#endif
int rem_avail(void);
int play_setup_shown(void);
int ui_refresh_visibility(void);
const char *loc_lock_key(int id) /* NULL = nicht gesperrt */;
void tip_text(int id, char *out, size_t cap);
void tips_place(void);
void tips_build(void);
void tips_dispinfo(NMTTDISPINFOW *di);
void ui_apply_rules(void);
int ui_confirm(const char *text, const char *title);
extern char g_ucfg_test[1200];
void user_cfg_path(char *out, size_t cap);
void game_cfg_path(int game, char *out, size_t cap);
extern launcher_settings_t g_disk_base;
extern int g_disk_base_ok;
void disk_base_sync(const launcher_settings_t *s);
int disk_merge(launcher_settings_t *s);
int save_user_merged(const char *upath);
int disk_refresh(void);
int do_save(int complain);
extern char g_start_args[1600];
void game_info_ui(const launcher_settings_t *s, lcfg_gameinfo_t *gi);
int client_exit_report(unsigned long code, int edition, int show_box);
extern int g_vx_pending;
void client_exit_poll(void);
int do_start_ex(int dry, int vr, const char *extra);
int do_start(int dry, int vr);
void browse_xbe(void);
void check_image_ex(int status);
void check_image(void);
void image_chosen(const wchar_t *file);
void browse_image(void);
void xbe_cue_update(void);
void setup_auto_sync(int from_list);
int media_drop_bad_image(void);
void browse_media(void);
extern lctrl_bind_t g_bind;
extern lctrl_bind_t g_bind_base;
extern int g_bind_lb_prev;
extern void (*g_bind_testfn)(HWND hd);
extern int g_bind_sel;
extern int g_cap_mode;
extern lctrl_pad_t g_cap_prev;
extern lctrl_pad_learner_t g_cap_plrn;
HWND bctl(HWND hd, int id);
const char *bind_wheel_src(UINT msg, WPARAM wp);
void bind_src_disp(const char *alt, char *out, size_t cap);
void list_add(HWND lb, const char *u8, LPARAM data, int with_data);
void bind_end_capture(HWND hd, const char *prompt);
void bind_cap_joy_init(void);
void bind_start_capture(HWND hd, int mode);
void bind_cap_key(HWND hd, int vk, LPARAM lp);
int bind_cap_keyup(HWND hd, int vk);
void bind_cap_mouse(HWND hd, UINT msg, WPARAM wp);
void bind_cap_pad_state(HWND hd, const lctrl_pad_t *curp);
void bind_cap_joy(HWND hd);
int bind_save(HWND hd);
void offscreen_pos(int *x, int *y);
int is_offscreen(HWND h);
void bind_dialog_modal(const char *base_name, const char *base_path);
extern const char *const kBtnShort[39];
void assist_dialog_modal(const char *fixture);
void open_assist_dialog(void);
extern int g_joy_demo;
extern int g_joy_done;
extern int g_joy_have;
extern int g_joy_n;
extern int g_joy_ok;
extern ijoy_info_t g_joy_list[IJOY_MAX_DEV];
extern char g_joy_xi[8][96];
extern int g_joy_nxi;
void joy_cal_load(void);
int joy_devinfo_all(char *out, int cap, int only);
void joy_dialog_modal(void);
extern char g_rp_args[1000];
extern char g_vx_dir[700];
extern char g_vx_dir_open[700];
extern char g_vx_id[40];
void rp_video_default_dir(char *out, size_t cap);
void rp_export_done(unsigned long code);
int replay_dialog_modal(int shot);
void replay_dialog_open(void);
void open_bind_dialog(void);
extern char g_cx_name[64];
int cx_export_to(const char *path, char *msg, size_t cap);
int cx_import_check(const char *path, const ijoy_info_t *list, int nlist, int sbc_present, char *q,
                           size_t qcap);
int cx_import_apply(int cfg, char *msg, size_t cap);
void ctrl_export(void);
void ctrl_import(void);
int cx_put_text(const char *path, const char *text, size_t n);
void cx_shot_state(const char *state);
extern sb_pack_order_t g_pk_ord[PK_MAXN];
extern int g_pk_n;
extern char g_pk_level[8];
extern int g_pk_ncaps;
extern int g_pk_chk[PK_MAXN];
extern sb_pack_cov_t g_pk_cov[PK_MAXN];
extern int g_pk_covby[PK_MAXN * PK_MAXN];
extern int g_pk_fx_hit[SB_FXN];
extern int g_pk_fx_val[SB_FXN];
extern int g_pk_sel;
void packs_json_path(char *out, size_t cap);
int packs_count(void);
int packs_status_of(int i);
int packs_enabled_of(int i);
int packs_is_fx(int i);
const char *packs_level_word(void);
int packs_cap_stored(int c);
int packs_cap_of(int c);
void packs_cap_set(int c, int v);
void packs_load(void);
void packs_save(void);
void packs_draw_row(const DRAWITEMSTRUCT *d);
void packs_refresh(int load);
int packs_sel(void);
int packs_add_path(const char *src_u8, int is_dir);
void packs_add_menu(void);
void packs_drop(HDROP hd);
void packs_del(void);
void packs_move_to(int from, int to);
void packs_move(int dir);
void packs_toggle_idx(int i);
void packs_toggle(void);
void packs_check(void);
void packs_check_all(void);
void packs_folder(void);
#if !SB_EDITION_COMMUNITY
void packs_creator_open(void);
#endif
void packs_set_level(int lv);
void packs_cap_reset(void);
lgp_mask_t gfx_lock_now(void);
void packs_env(lr_env_t *e);
LRESULT CALLBACK pklist_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp,
                                    UINT_PTR sid, DWORD_PTR ref);
void paint_packs(void);
void show_page(int p);
const int *nav_order(void);
int nav_next(int page, int dir);
void ui_apply_language(void);
int gametab_id(int id) /* W398: Wortmarken-Tabs im Kopf */;
extern lgame_state_t g_loc;
extern lgame_img_t g_loc_res;
extern char g_loc_res_path[512];
extern int g_loc_checked;
extern char g_locfix_path[512];
extern lgame_img_t g_locfix;
extern char g_loc_last_cmd[1600];
extern char g_loc_last_cfg[1200];
int loctab_may_write(void);
extern int g_loc169_migrated;
void loctab_load(void);
extern int g_pg_base_ok[LGAME_N];
extern int g_pg_ok[LGAME_N];
void pergame_mark(void);
void pergame_load(int game, launcher_settings_t *s);
void pergame_init(void);
extern int g_loc_online;
extern int g_loc_public;
extern int g_loc_smooth_opp;
extern int g_loc_rb_on;
extern int g_loc_net_auto;
extern int g_loc_own_pred;
extern int g_loc_lowd;
extern int g_loc_clip;
extern int g_loc_chat;
extern int g_loc_snchat;
extern int g_loc_chatbar; /* W508 */
extern int g_loc_arcade;
extern int g_loc_lan;        /* W454 LOC-LAN-MODE: 1 = Multiplayer nur im LAN (ohne steelnet) */
extern char g_loc_lan_room[17];
void lan_kind_pick(void);
void lan_kind_set(int v);
void lan_field_changed(int id);
int lan_room_valid(const char *r);
int lan_set_append(char *set, size_t cap);
/* W487 LIVE-GAMERTAG / W501 ONLINE-BOTH: Verbindung der Karte Match nur noch Online/LAN. Online mit steelnet-Konto und
 * Gamertag (lnet_live_on) startet mit loc_live=1 per --set (im Spiel System Link oder Xbox Live waehlbar), ohne
 * Gamertag nur System Link, LAN nie. launcher_loc_live=1 (alt) wird beim Laden einmalig zu Online (onl_load).
 * W485 MERGEFIX: einziger Weg ist dieses Dropdown (der W485-Schalter unter Lobby & Chat entfiel). */
void lnet_kind_set(int k);   /* 0 = Online (steelnet), 1 = LAN */
int lnet_kind_get(void);
int lnet_live_ok(void);      /* W485/W487: Live startbar = nicht LAN, steelnet-Konto mit Gamertag (acc_live_state OK) */
int lnet_live_on(void);      /* W501: Online mit Gamertag = Stufe steelnet + lnet_live_ok */
int live_set_append(char *set, size_t cap, int onl); /* ";loc_live=1" nur bei onl + lnet_live_ok; 1 = angehaengt */
enum { ACC_LIVE_OK, ACC_LIVE_OFF, ACC_LIVE_NOACC, ACC_LIVE_UNK, ACC_LIVE_NOGT, ACC_LIVE_GTOFF };
int acc_live_state(void);    /* Startbarkeit Live aus dem letzten status (kein Netz) */
const char *acc_live_text(int st); /* Hinweis (aktive Sprache), NULL bei ACC_LIVE_OK */
int acc_live_prestart(void);  /* W501: 1 = jetzt starten; 0 = Gamertag-Stand unbekannt, Status laeuft, Start folgt */
void acc_gt_tick(void);      /* W487: entprellte Gamertag-Pruefung (ACC_GT_TIMER) */
void acc_gt_status_text(const char *status, long long next_at, const char *name, char *out, size_t cap);
const char *acc_card_gt(char *buf, size_t cap, COLORREF *c);   /* Karte Match & network (gesichert) */
const char *acc_gt_line1(char *buf, size_t cap, COLORREF *c);  /* Gamertag-Karte Zeile 1 */
const char *acc_gt_limit(char *buf, size_t cap);               /* Gamertag-Karte Grenze */
int live_info(char *out, size_t cap, COLORREF *col);           /* Infozeile der Karte Match (Online, W501) */
/* W491 CAMP-CLIENT: Kampagnenstatus fuer Online mit Gamertag (W501; launcher_camp.h). Verfuegbar = Konto + Gamertag (acc_live_state)
 * + aktuelle Version (loc_ver_gate) + Zugang erreichbar (camp_kick: einmalige TLS-Pruefung mit Pin im Hintergrund,
 * Ergebnis 5 min gecacht, Fehler nach 60 s erneut). Start mit loc_live=1: loc_camp=1, intern mit Testkampagne
 * zusaetzlich loc_camp_id=test (Community nie). */
#define WM_APP_CAMP (WM_APP + 49)
extern int g_camp_test;              /* intern: 1 = Testkampagne (user.cfg launcher_loc_camp_test); Community immer 0 */
extern int g_camp_reach[2];          /* LCAMP_R_* je Kampagne (0 Saison, 1 Test) */
extern int g_camp_busy, g_camp_gen, g_camp_dry, g_camp_dry_n;
int camp_sel(void);                  /* 0 Saison, 1 Testkampagne (Community immer 0) */
int camp_status(void);               /* LCAMP_* */
const char *camp_text(int st, char *buf, size_t cap); /* Hinweis (aktive Sprache), eine Zeile */
int camp_kick(void);                 /* Pruefung anstossen, wenn faellig; 1 = gestartet (Selbsttest: gezaehlt) */
void camp_apply(int k, int reach);   /* Ergebnis uebernehmen (UI-Thread; Selbsttest direkt) */
void camp_msg(LPARAM lp, WPARAM wp); /* WM_APP_CAMP */
void camp_fixture(int reach);        /* Shots/Selbsttest: beide Kampagnen mit diesem Ergebnis, nie Netz */
int camp_set_append(char *set, size_t cap); /* ";loc_camp=1" (+ intern ";loc_camp_id=test"); nur nach live_set_append */
void camp_pick(void);                /* intern: Auswahl Saison/Testkampagne (ID_LNET_CAMP) */
int camp_info2(char *l1, size_t c1, char *l2, size_t c2, COLORREF *col2); /* Live bereit: Zeile 1 + Kampagne; sonst 0 */
const char *camp_texts_fit(void);    /* Selbsttest: NULL = alle Kampagnenzeilen passen einzeilig, sonst der Text */
extern int g_acc_x_gt;
extern int g_acc_gt_user;
extern char g_acc_gtmsg[200];
extern int g_acc_gtmsg_id;
extern int g_acc_live_pending;
int loc_tip_id(int id) /* W387: Tooltip am Element (tips_build, Text in xtip_text) */;
extern int g_onl_fix_srv;
extern int g_onl_fixture;
extern char g_onl_sec[24];
extern char g_onl_code[16];
extern char g_onl_status_test[1200];
extern ULONGLONG g_onl_since;
extern char g_onl_err[12];
int onl_srv_on(void);
ULONGLONG onl_now_ft(void);
int onl_code_norm(const char *in, char *out, size_t cap);
int onl_set_append(char *set, size_t cap, int on, int srv, const char *raw, int pub);
int loc_smooth_parse(const char *v);
void loc_smooth_set_append(char *set, size_t cap, int v);
int lnet_bool_parse(const char *v);
int lnet_set_last(const char *set, const char *key, char *out, size_t cap);
int lnet_rbopts_merge(const char *base, int lowd_on, char *out, size_t cap);
void loc_net_set_append(char *set, size_t cap);
void loc_rule_set_append(char *set, size_t cap);
int lnet_is_default(void);
int onl_status_parse(const char *path, char *sec, size_t sc, char *code, size_t cc);
void onl_status_err(const char *path, char *err, size_t cap);
int onl_status_refresh(void);
int onl_info(char *out, size_t cap, COLORREF *col);
void onl_load(void);
int onl_save_key(const char *key, int val);
int onl_save(void);
void onl_sync_ui(void);
void onl_redraw(void);
void onl_copy(void);
void onl_vis_pick(void);
extern const char *const kLocSmooth[LOC_SMOOTH_N];
void loc_smooth_pick(void);
void lnet_netmode_fill(void);
int lnet_rb_ui(void);
void lnet_netmode_pick(int v);
void lnet_mode_pick(void);
void lnet_toggle(int id);
void lnet_reset(void);
void lnet_tab_pick(int tab);     /* W473 */
int lnet_tab_hidden(int id);     /* W473 */
void lnet_mission_flush(void);   /* W473 */
int lnet_hint_rect(int i, RECT *rc, const char **txt, int *id); /* W473: Selbsttest */
int lnet_label_rect(int i, RECT *rc, char *txt, size_t cap);    /* W473: Selbsttest */
int lnet_row_count(void);        /* W473 */
int hub_acc_value(char *t, size_t cap, COLORREF *c); /* W473 */
int hub_acc_fits(const char *t); /* W473: Selbsttest */
int acc_card_fits(char *why, size_t cap, const char *tag); /* W473: Selbsttest */
const char *mp_labels_fit(void); /* W473: Selbsttest */
int acc_card_state(void);        /* W473 */
const char *acc_card_why(int st); /* W473 */
int ui_text_need(int font, const char *u8, int x, int y, int w, int h, int single, int *need, int *have); /* W473 */
int ui_text_fits(int font, const char *u8, int x, int y, int w, int h, int single); /* W473 */
extern int g_lnet_tab, g_loc_msn, g_loc_mlobby, g_loc_mteam, g_lnet_m_pending; /* W473 */
void paint_lnet(void);
extern int g_pv_done;
extern int g_pv_mode;
int privacy_texts(char *out, int cap);
int privacy_dialog_modal(int mode);
void onl_tick(void);
void onl_paint(void);
void onl_build(HWND w);
void onl_relabel(void);
void loctab_wide_fill(void);
void loctab_wide_sync(void);
int loctab_wide_val(void);
int pergame_box_169(void) /* W402: Bildformat des aktiven Spiels ist 16:9 (nur LoC) */;
void loctab_wide_pick(void);
void loctab_sblook_sync(void);   /* W459 */
int loctab_sblook_reset(void);   /* W459: Back to original (LoC) */
void loctab_data_dir(char *out, size_t cap);
extern int g_locgate_skip;
extern int g_in_gui_selftest;
int loc_net_ok(void);
int loc_gate_shown(void);
int loc_img_gate(char *m, size_t cap);
extern char g_locver_min[16];
extern const char *g_locver_own_fix;
extern int g_ver_open_n;
const char *locver_own(void);
int loc_ver_gate(void);
void locver_fixture(const char *min_ver);
int loc_launch_ok(void);
void loc_gate_msg(char *m, size_t cap);
void loc_gate_sync(void);
int loctab_visible(const uictl_t *u, int page);
void nav_sub_layout(void);
void loctab_nav_layout(void);
void loctab_set_game(int game);
void loctab_check(void);
void loctab_browse(void);
void loctab_open_folder(void);
void loctab_start(void);
extern lacc_store_t g_acc;
extern int g_acc_loaded;
extern lacc_status_t g_acc_st;
extern int g_acc_st_ok;
extern int g_acc_view;
extern int g_acc_busy;
extern int g_acc_gen;
extern int g_acc_fixture;
extern int g_acc_fix_srv;
extern char g_acc_rec[48];
extern char g_acc_dir_test[1200];
extern int g_acc_last_op;
extern int g_acc_last_rc;
extern int g_acc_try_op;
extern char g_acc_last_err[64];
extern int g_acc_x_code;
extern int g_acc_x_login;
extern char g_acc_st_err[24];
extern char g_acc_fmsg[200];
extern int g_acc_fmsg_id;
extern COLORREF g_acc_fmsg_col;
void mp_card_acc_line(char *t, size_t cap);
int acc_state(void);
void acc_geo(acc_geo_t *g);
void acc_x_reset(void);
int acc_code_kind(const char *s);
void acc_refresh(void);
void acc_done(acc_job_t *j);
void acc_page_shown(void);
void acc_play_shown(void);
int acc_danger_id(int id);
int acc_visible(int id, int page);
/* W502 ACCOUNT-PAGE: Einsprung fuer andere Seiten (z. B. W501 Match ohne Gamertag): Kontoseite oeffnen, Gamertag-Karte
 * zeigen, Feld fokussieren (mit Gamertag: Knopf "Aendern"), Karte kurz hervorheben. Nur im eigenen Fenster, kein
 * Vordergrund. */
void acc_open_gamertag(void);
void acc_hl_tick(void);          /* ACC_HL_TIMER: Hervorhebung nach einem Sprung beenden */
extern int g_acc_focus_id;       /* Selbsttest: zuletzt angeforderter Fokus der Kontoseite (ID) */
extern int g_acc_hl;             /* hervorgehobene Karte (ACC_CARD_*, 0 = keine) */
int acc_page_cards(RECT *r, int *kind, int cap); /* Selbsttest: gezeichnete Karten der Kontoseite (logisch) */
int acc_page_texts_fit(char *why, size_t cap);   /* Selbsttest: alle Texte der Seite passen (Zustand, Sprache) */
int acc_view_active(void);
int acc_sub_active(void);
void acc_layout(void);
void acc_nav_sub(int acc);
void acc_ensure_loaded(void); /* W472: gespeichertes Konto einmal laden (steelnet > Info) */
void acc_name_tick(void);
wchar_t *acc_launch_env(char *set, size_t cap);
int acc_sum_row(char *val, size_t cap, COLORREF *col);
int acc_paint_card(void);
const char *acc_unk_text(COLORREF *c);
void acc_paint_view(void);
int loctab_command(int id, int code);
void loctab_result(char *msg, size_t cap, COLORREF *col);
void loctab_display_value(const launcher_settings_t *s, char *out, size_t cap);
void loctab_sum_rows(loc_sumrow_t *r);
void paint_loc_play(void);
void paint_loc_adv(void);
#if defined(SB_HAVE_LOCTP) && SB_HAVE_LOCTP
void loctp_mode_from_cfg(void);
int loctp_visible_id(int id, int page);
int loctp_cfg_from_ui(loctp_cfg_t *c, char *err, size_t cap);
void loctp_tick(void);
int loctp_command(int id, int code);
void loctp_gui_selftest(int *pass, int *fail);
void loctp_mode_selftest(int *pass, int *fail);
int loctp_err_shot(const char *out_dir);
#endif
void loctab_build(HWND w);
void loctab_relabel(void);
void loctab_fill_done(void);
int loctab_apply_state(const char *name);
void acc_fixture(int srv, int have, int discord, int login, int view);
void acc_apply_state(const char *name);
void loctab_state_after(const char *name);
int orig_banner_on(void);
int orig_visible(int id, int page);
void xtip_text(int id, char *out, size_t cap);
void xicon_texts(void);
void orig_paint_play(int loc);
void orig_relabel(void);
void orig_reset_now(void);
int orig_once_start(int dry);
int orig_command(int id, int code);
void build_controls(void);
void snap_settings(launcher_settings_t *out);
void upd_open_page(void);
void ver_open_page(void);
int netq_should_ask(const char *ver, int upd_cfg, int online);
int netm_get(void);
void netm_sync_ui(void);
extern int g_snp_state;
extern int g_snp_busy;
extern int g_snp_dry;
extern int g_snp_dry_n;
extern int g_snp_gen;
extern int g_snp_live;
extern int g_snp_gam;
extern int g_snp_has;
extern int g_snp_lob;
extern int g_snp_pl;
int snp_kick(void);
int snp_due(int shown, ULONGLONG since_ms);
void snp_apply(const lacc_probe_t *r);
int hdr_text_w(int font, const char *u8, int extra) /* logische Breite (96 dpi) */;
int hdr_sub_parts(const char *txt[3], COLORREF col[3], int w[3]);
void hdr_paint_sub(void);
void hdr_chip_text(int kind, int brief, char *out, size_t cap, COLORREF *col, COLORREF *dot);
void hdr_layout(hdr_lay_t *L);
extern RECT g_hdr_hit[2];
extern int g_hdr_hit_kind[2];
extern int g_hdr_hit_n;
int hdr_hit(POINT p);
void hdr_tip_text(int kind, char *out, size_t cap);
void hdr_click(int kind);
int gametab_tw(HWND hw, int *font) /* Breite des Namens in Pixeln im Tab (Selbsttest) */;
void gametab_paint(canvas_t *c, const uictl_t *u, int w, int h, const wchar_t *txt, int n, int focus);
void hdr_paint_status(void);
void netq_paint(int x, int y, int w);
void lset_open(void);
void paint_lset(void);
void lset_build(HWND w);
void lset_relabel(void);
int lset_visible(int id, int page);
void on_command(int id, int code);
extern int g_lay_work_h;
extern int g_lay_work_w;
void lay_switch(int id, int size);
extern char g_lay_ovr[4];
extern char g_lay_ovr_size[12];
int lay_cfg_id(void);
int lay_cfg_size(void);
int gui_init(const char *client_override);
HWND gui_create(int offscreen);
void w220_fixture(const char *path, int rc);
int w220_state(const char *name);
extern char g_ctrl040_msg[512];
extern int g_ctrl040_rc;
void ctrl040_reset(void);
void gui_teardown(void);
int shot_window(HWND w, const char *path, int png);
int bind_shot_save(HWND hd);
void pump(void);
void alpha_page_shown(void);
void paint_alpha(void);
void alpha_build_controls(HWND w);
void alpha_relabel(void);
int dg_ctl_visible(int id);
int dg_paint_play(void);
void dg_build_controls(HWND w);
void dg_relabel(void);
void write_file(const char *path, const char *text);
void gt_read(const char *path, char *buf, size_t cap);
void gt_cfg_set(const char *path, const char *key, const char *val);
void gt_setenv(const char *name, const char *v);
void loctab_gui_selftest(int *pass, int *fail);
void gameswitch_gui_selftest(int *pass, int *fail);
void header_polish_selftest(int *pass, int *fail);
void loc_gate_gui_selftest(int *pass, int *fail);
void loc_lan_gui_selftest(int *pass, int *fail); /* W454 */
void loc_live_gui_selftest(int *pass, int *fail); /* W487 */
void camp_gui_selftest(int *pass, int *fail);     /* W491 */
void r1_polish_gui_selftest(int *pass, int *fail);
__declspec(noinline) void locver_gui_selftest(int *pass, int *fail);
void pergame_gui_selftest(int *pass, int *fail);
void loc_save_gui_selftest(int *pass, int *fail);
void orig_gui_selftest(int *pass, int *fail);
void xicon_gui_selftest(int *pass, int *fail);
void lay_gui_selftest(int *pass, int *fail);
void adv_gui_selftest(int *pass, int *fail);
__declspec(noinline) void locip_gui_selftest(int *pass, int *fail);
__declspec(noinline) void acc_gui_selftest(int *pass, int *fail);
__declspec(noinline) void accpage_gui_selftest(int *pass, int *fail);
__declspec(noinline) void accux_gui_selftest(int *pass, int *fail);
__declspec(noinline) void w487_gt_gui_selftest(int *pass, int *fail); /* W487 */
__declspec(noinline) void w502_accpage_gui_selftest(int *pass, int *fail); /* W502 ACCOUNT-PAGE (Fall 75e) */
__declspec(noinline) void onl_gui_selftest(int *pass, int *fail);
__declspec(noinline) void smooth_gui_selftest(int *pass, int *fail);
__declspec(noinline) void lnet_gui_selftest(int *pass, int *fail);
__declspec(noinline) void privacy_gui_selftest(int *pass, int *fail);
__declspec(noinline) void locwide_gui_selftest(int *pass, int *fail);
__declspec(noinline) void locsblook_gui_selftest(int *pass, int *fail); /* W459 */
__declspec(noinline) void w465_gui_selftest(int *pass, int *fail); /* W465 */
__declspec(noinline) void w476_gui_selftest(int *pass, int *fail); /* W476 */
__declspec(noinline) void w478_gui_selftest(int *pass, int *fail); /* W478 */
__declspec(noinline) void w486_gui_selftest(int *pass, int *fail); /* W486 */
__declspec(noinline) void loc169_gui_selftest(int *pass, int *fail);
__declspec(noinline) void fpsmon_gui_selftest(int *pass, int *fail);
void porig_gui_selftest(int *pass, int *fail);
void locl_gui_selftest(int *pass, int *fail);

/* --- W470 LAUNCHER-HUB (launcher_gui_hub.c) ------------------------ */
typedef struct {
    int state;        /* hdr_steel_state: 1 online, 0 offline, -1 unbekannt */
    int own;          /* eigene Pruefung: -1 keine, 0 offline, 1 online */
    int ms;           /* Antwortzeit */
    const char *ver;  /* Serverversion ("" = keine) */
    int hh, mm;       /* Zeitpunkt der letzten Pruefung */
    int has;          /* Live-Zahlen vorhanden */
    int players, lobbies, games;
    int today, peak;  /* -1 = nicht geliefert */
} snp_view_t;
void snp_view(snp_view_t *v);
int hdr_steel_state(void);
int hub_area_of(int page);
int gametab_active(const uictl_t *u); /* W473: Spiel-Reiter gewaehlt UND Spielbereich offen */
int hub_visible(int id, int page);  /* -1 = keine Vorgabe */
int hub_command(int id, int code);  /* 1 = erledigt */
int hub_wndproc(UINT msg, WPARAM wp, LPARAM lp); /* 1 = erledigt */
void hub_build(HWND w);
void hub_relabel(void);
void hub_page_shown(int page);      /* aus show_page (merkt Spielseite, Timer der Lobbyliste) */
int hub_game_page(void);            /* zuletzt offene Seite im Spielbereich */
int hub_tab_page(int id);           /* Reiter-Id -> Seite, sonst -1 */
wchar_t hub_tab_icon(int id);
void hub_paint_nav(void);
void paint_neti(void);
void hub_net_changed(void);
void hub_lobby_fixture(int n);      /* Bilder/Selbsttest: n Lobbys (-1 = Server ohne Liste, -2 = wartet, -3 = Fehler,
                                     * W472: -4 ohne Konto, -5 Anmeldung abgelehnt, -6 zu viele Anfragen) */
void hub_lobby_result(const lacc_lobbies_t *r); /* W472 Selbsttest: Ergebnis wie aus dem Abruf uebernehmen */
extern ULONGLONG g_hubl_wait;       /* W472: Abstand bis zum naechsten Abruf (ms) */
extern int g_hubl_dry, g_hubl_dry_n;
extern ULONGLONG g_hubl_t0;
extern int g_hubl_state;
extern lacc_lobbies_t g_hubl;
int hub_lobby_due(ULONGLONG since_ms);
int hub_lobby_kick(void);
void hub_tick(void);
__declspec(noinline) void hub_gui_selftest(int *pass, int *fail);
__declspec(noinline) void mppolish_gui_selftest(int *pass, int *fail); /* W473 */

#endif /* LAUNCHER_GUI_INT_H */
