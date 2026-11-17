/* sb_overlay.c - W118 REM-OVERLAY: Einstellungs-Overlay zur Laufzeit (ESC).
 * Beschreibung s. sb_overlay.h.
 *
 * Threads: Fenster-Nachrichten kommen auf dem UI-Thread an (sb_ovl_wndproc):
 * ESC schaltet g_ov_open sofort (Eingabesperre greift ab dem naechsten
 * XInput-Poll), alle anderen Overlay-Tasten wandern in einen kleinen Ring.
 * Der Render-Thread (sb_ovl_present_begin, Anfang jedes Presents) arbeitet
 * den Ring ab und setzt die Werte in g_cfg - zwischen zwei Bildern, wie die
 * bestehenden Laufzeit-Umschalter (shade_ab, fs_interp_match). Gezeichnet
 * wird nur bei offenem Overlay: ohne FPS-INTERP direkt vor dem Screenshot
 * auf den Spiel-Backbuffer (Bereich wird gesichert und nach dem Present
 * zurueckgelegt), mit FPS-INTERP auf den Swapchain-BB nach jeder Kopie
 * (sb_render_blit) - Aufzeichnung/interp_verify bleiben unberuehrt.
 *
 * Panel: GDI zeichnet Text (Windows-Systemschrift Consolas, eigene Farben)
 * in eine 32-Bit-DIB in Zielpixeln (Faktor = Hoehe der 4:3-Box / 540) ->
 * dynamische Textur -> Quad 1:1 per Texel-Load (kein Resampling, nie
 * gestreckt). Lage: oben links in der 4:3-Box (sb_aspect_box), also auch
 * im Ultrawide eingepasst. */
#include "render_int.h"
#include "sb_profile.h" /* W229 SBA() */
#include "sb_gfxpreset.h"
#include "sb_fpsrate.h" /* W223 FPS-RATES */
#include "render_qa.h"
#include "sb_lang.h" /* W142 L10N */
#include "sb_watermark.h" /* PO: sb_version_str fuer W222 */
#include "sb_overlay.h"
#include "sb_perfhud.h" /* W122: Test-Aktion PHUDPOS */
#include "sb_remaster.h"
#include "input_mouse.h"
#include "sbc_wire.h" /* W128: Statuszeile Original-Controller */
#include "render_fx_rem.h" /* W133 REM-FX: Seite "Effekte" */
#include "vr.h" /* W134 VR-PROTO: Seite "VR" */
#include "sb_pause.h" /* W188 GAME-MENU: Pause */
#include "render_clip.h"
#include "sb_feedback.h" /* W188 GAME-MENU: Feedback-Paket */
#include "input_map.h" /* W188 GAME-MENU: Gamepad im Menue (imap_pad_poll) */
#include "kernel_hle.h" /* W190 SAVE-LOCK: krnl_savelock_* */
#include "sb_photo.h" /* W195 PHOTO-MODE */
#include "sb_rauto.h" /* W290: Test-Anker pN */
#include "sb_origview.h" /* W278 ORIG-FLAG: Umschalttaste, EXP-Markierung */
#include "sb_origflag.h"
#include <stdlib.h>
#include <ctype.h> /* W195: toupper (Test-Hook KDN=/KUP=) */
#include <wchar.h>
#include <time.h> /* W222: Build-Datum */
#include "sb_state.h" /* W232: sb_in_mission */

/* ---- Modulgrenzen (andere Dateien) ---- */
int sb_render_offscreen(void);                 /* d3d11_render.c: 1 = FPS-INTERP-Offscreen-BB */
int sb_render_views_rebuild(const char *why);  /* d3d11_render.c: Views neu (interp_fps-Wechsel) */
HWND sb_render_hwnd(void);                     /* d3d11_render.c */
int imouse_subclassed(void);                   /* input_mouse.c */
void imouse_release_keys(void);                /* input_mouse.c (UI-Thread) */
int sb_render_pause_jpg(const char *path, unsigned *ow, unsigned *oh); /* d3d11_render.c (W188, unter ctx_lock) */

/* ------------------------------------------------------------------ */
/* Zustand                                                             */
/* ------------------------------------------------------------------ */
enum {
    OI_FPS, OI_SHADING, OI_MFX, OI_GRAIN, OI_GLITCH, OI_EDGE, OI_EDGEW, OI_EDGES,
    OI_MMODE, OI_MSENS, OI_HD, OI_HDSCALE, OI_WINMODE, OI_RES, OI_SAVE, OI_CLOSE, OI_N0,
    /* W121 SHADE-FX: Seite "Shader" (TAB) */
    OI_FOG = OI_N0, OI_WCON, OI_WGAM, OI_WSHARP, OI_BLOOM, OI_CGLOW, OI_CSPILL, OI_CLIGHT, OI_ABCMP,
    /* W121 Teil 2: Belichtung, Tonemapping, Bloom-Schwelle, Licht Welt/Cockpit, Welt-Aufloesung */
    OI_EXPO, OI_TONE, OI_BTHR, OI_LWORLD, OI_LCKP, OI_RTS, OI_FOCUS,
    /* W128: Maus bei Original-Controller (nur mit sbc_usb=1 sichtbar) */
    OI_SBCMOUSE,
    /* W131 SHADER-OPT2: Seite "Shader 2" (TAB von "Shader") */
    OI_PRESET, OI_AA, OI_SSAO, OI_CSAT, OI_CTEMP, OI_CLIFT, OI_CGAM, OI_CGAIN, OI_SHADOW, OI_SHSOFT, OI_VQASYNC,
    OI_LABEL, /* W153 HUD-LABEL-OPT (Seite "Shader 2") */
    OI_CPERSP, /* W191 UW-PERSP: Cockpit-Perspektive (Seite "Shader 2") */
    OI_CPANINI, /* W191 UW-PERSP: Panini der Cockpit-Ansicht (Seite "Shader 2") */
    OI_CWIDTH, /* W235 COCKPIT-WIDTH: Breite der Cockpit-Ansicht (Seite "Shader 2") */
    /* W133 REM-FX: Seite "Effekte" (OV_PAGE_FX) */
    OI_FXPRESET, OI_FXSOFT, OI_FXEMB, OI_FXLIGHT, OI_FXLCKP, OI_FXIP, OI_FXCMP,
    /* W134 VR-PROTO: Seite "VR" (OV_PAGE_VR) */
    OI_VRON, OI_VRREC, OI_VRSEAT, OI_VRSCALE, OI_VRIPD, OI_VRPREV,
    /* W183 VR-FIX3: Sitz vor/zurueck, Leistung (Preset, Aufloesung, Bildrate, Spiegel, Aussenwelt, Shading) */
    OI_VRFWD, OI_VRPRESET, OI_VRRES, OI_VRHZ, OI_VRMIRROR, OI_VRWORLD, OI_VRSHADE,
    /* W188 GAME-MENU: Seite 0 "ESC-Menue pausiert" */
    OI_MPAUSE,
    /* W190 SAVE-LOCK: Seite 0 "Spielstaende schuetzen" + "Geschuetzte Aenderungen uebernehmen" */
    OI_SAVELOCK, OI_SLCOMMIT,
    /* W218 SIMPLE-CONTROLS: Seite 0 Ziel-Kopplung + Gang-Automatik */
    OI_ASSIST, OI_AUTOGEAR,
    /* W271 STICK-RELATIVE: Seite 0 Stick-Zielen + Empfindlichkeit */
    OI_STICKAIM, OI_STICKSENS,
    /* W242 VR-SHELL: Seite "VR" dunkle Huelle + Rand-Vignette */
    OI_VRSHELL, OI_VRVIG,
    /* W240 FX-EXPERIMENTAL: Seite "Experimental" (OV_PAGE_XP) */
    OI_XWIRE, OI_XVAPOR,
    OI_CHIDE, /* W268 COCKPIT-HIDE: Cockpit ausblenden (Seite "Shader 2", nach Breite) */
    /* W291 ESC-REWORK: Sammelregler (setzen vorhandene Schluessel gemeinsam), Klappzeile, Aktionen, Info */
    OI_SMON, OI_STEX, OI_SLIGHT, OI_SSHADOW, OI_SBLOOM, OI_SGLOW, OI_SLOOK,
    OI_ADV, OI_RESET, OI_TEXINFO,
    OI_RPLABEL, OI_RPLOGO, OI_WMMEDIA, /* W290-Schalter (REPLAY-Hinweis, Logo, Wasserzeichen im Video) */
    OI_VOLM, OI_VOLMU, OI_VOLSFX, OI_VOLVOX, /* W303 AUDIO-VOLUME: Gesamt, Musik, Effekte, Sprache/Funk */
    OI_RPFX, /* W299 REPLAY-FX: Replay-Bildfilter live (Vollbild-Ansicht, experimentell) */
    OI_MONVIEW, /* W319 MONITOR-VIEW: Monitoransicht (Gruppe Cockpit, nach "Cockpit") */
    OI_N
};
/* W291 ESC-REWORK: Gruppen statt Seiten (Namen und Reihenfolge wie die
 * Launcher-Grafikseiten). Je Gruppe Hauptzeilen und eingeklappte Feinregler
 * ("Erweitert"); darunter "Auf Original" (wo es Tabellen-Schluessel gibt) und
 * "Als Standard speichern". Die Schnellseite enthaelt nur Verknuepfungen auf
 * Zeilen anderer Gruppen (Ausnahme: Preset). VR nur bei VR, Debug nur mit
 * Alpha-Tools (esc_debug) oder aktivem Spass-Effekt. */
enum { OG_QUICK, OG_GAME, OG_DISP, OG_CKP, OG_WORLD, OG_FX, OG_COLOR, OG_TEX, OG_AUDIO, OG_VR, OG_DEBUG, OG_N };
#define OV_NPAGES OG_N
static const short kOgQuick[] = { OI_PRESET, OI_FPS, OI_WINMODE, OI_CHIDE, OI_SMON, OI_STEX };
static const short kOgGame[] = { OI_ASSIST, OI_AUTOGEAR, OI_MMODE, OI_MSENS, OI_STICKAIM, OI_STICKSENS };
static const short kOgGameA[] = { OI_SBCMOUSE, OI_MPAUSE, OI_SAVELOCK, OI_SLCOMMIT };
static const short kOgDisp[] = { OI_FPS, OI_WINMODE, OI_RES, OI_RTS, OI_EDGE };
static const short kOgDispA[] = { OI_EDGEW, OI_EDGES, OI_CPERSP, OI_CPANINI, OI_CWIDTH, OI_VRON }; /* Breitbild wie Launcher */
static const short kOgCkp[] = { OI_CHIDE, OI_MONVIEW, OI_SMON }; /* + W319 */
static const short kOgCkpA[] = { OI_GRAIN, OI_GLITCH, OI_LABEL, OI_RPLABEL, OI_RPLOGO, OI_WMMEDIA }; /* Korn/Stoerung unter Monitor-Effekt; + W290 */
static const short kOgWorld[] = { OI_SHADING, OI_SLIGHT, OI_SSHADOW, OI_FOG, OI_SBLOOM, OI_SGLOW };
static const short kOgWorldA[] = { OI_EXPO, OI_TONE, OI_WGAM, OI_WCON, OI_LWORLD, OI_LCKP, OI_CLIGHT, OI_CGLOW, OI_CSPILL,
                                   OI_BLOOM, OI_BTHR }; /* Schatten: Sammelregler ist die Einstellung (wie Launcher) */
static const short kOgFx[] = { OI_FXPRESET, OI_AA, OI_SSAO, OI_WSHARP };
static const short kOgFxA[] = { OI_FXSOFT, OI_FXEMB, OI_FXLIGHT, OI_FXLCKP, OI_FXIP, OI_FOCUS, OI_RPFX }; /* + W299 */
static const short kOgColor[] = { OI_SLOOK };
static const short kOgColorA[] = { OI_CSAT, OI_CTEMP, OI_CLIFT, OI_CGAM, OI_CGAIN };
static const short kOgTex[] = { OI_STEX, OI_TEXINFO };
/* W303 AUDIO-VOLUME: Gruppe "Ton" (Launcher "Ton & Extras"); hinter Texturen, damit die Seitenleisten-
 * Indizes der Test-Hooks (MTAB2 = Anzeige) gleich bleiben. Lautstaerke ist neutral (kein Kolben). */
static const short kOgAudio[] = { OI_VOLM, OI_VOLMU, OI_VOLSFX, OI_VOLVOX };
static const short kOgVr[] = { OI_VRPRESET, OI_VRREC, OI_VRSEAT, OI_VRFWD, OI_VRSCALE, OI_VRMIRROR };
static const short kOgVrA[] = { OI_VRIPD, OI_VRPREV, OI_VRRES, OI_VRHZ, OI_VRWORLD, OI_VRSHADE, OI_VRSHELL, OI_VRVIG };
static const short kOgDebug[] = { OI_XWIRE, OI_XVAPOR, OI_VQASYNC, OI_ABCMP, OI_FXCMP };
#define OG_ROWS(a) a, (int)(sizeof a / sizeof a[0])
typedef struct {
    const char *key, *log; /* Sprachschluessel, Logname */
    const short *main; int nmain;
    const short *adv; int nadv;
    int reset;             /* 1 = Zeile "Auf Original" (bzw. Schnell: alles, Debug: alles aus) */
} ov_grp_t;
static const ov_grp_t kOvGrp[OG_N] = {
    { "ovl.grp_quick", "Schnell", OG_ROWS(kOgQuick), NULL, 0, 1 },
    { "ovl.grp_game", "Spiel & Steuerung", OG_ROWS(kOgGame), OG_ROWS(kOgGameA), 0 },
    { "ovl.grp_display", "Anzeige", OG_ROWS(kOgDisp), OG_ROWS(kOgDispA), 1 },
    { "ovl.grp_cockpit", "Cockpit & Monitor", OG_ROWS(kOgCkp), OG_ROWS(kOgCkpA), 1 },
    { "ovl.grp_world", "Welt & Licht", OG_ROWS(kOgWorld), OG_ROWS(kOgWorldA), 1 },
    { "ovl.grp_fx", "Effekte", OG_ROWS(kOgFx), OG_ROWS(kOgFxA), 1 },
    { "ovl.grp_color", "Farbe", OG_ROWS(kOgColor), OG_ROWS(kOgColorA), 1 },
    { "ovl.grp_tex", "Texturen", OG_ROWS(kOgTex), NULL, 0, 1 },
    { "ovl.grp_audio", "Ton", OG_ROWS(kOgAudio), NULL, 0, 1 }, /* W303: "Auf Original" = alle 100 % */
    { "ovl.grp_vr", "VR", OG_ROWS(kOgVr), OG_ROWS(kOgVrA), 0 },
    { "ovl.grp_debug", "Debug", OG_ROWS(kOgDebug), NULL, 0, 1 },
};
static int g_ov_adv[OG_N];              /* W291: "Erweitert" je Gruppe aufgeklappt (Sitzung) */
#define OV_MAXROWS 40
static short g_ov_rv[OV_MAXROWS];       /* W291: sichtbare Zeilen der aktuellen Gruppe */
static int g_ov_nrv;
/* W183: Stufen der Render-Aufloesung (% der Runtime-Empfehlung) und Bildraten der Brille (0 = Runtime) */
static const int kOvVrRes[] = { 50, 60, 65, 70, 80, 85, 90, 100, 110, 120 };
static const int kOvVrHz[] = { 0, 72, 80, 90, 120 };
#define OV_NVRRES ((int)(sizeof kOvVrRes / sizeof kOvVrRes[0]))
#define OV_NVRHZ ((int)(sizeof kOvVrHz / sizeof kOvVrHz[0]))
static int g_ov_vr = -1;                /* W134: gewaehltes vr_enable (wirkt nach Neustart), -1 = noch nicht gelesen */
static int g_ov_vrprev = -1;            /* W134: gewaehlte Vorschau 0 sbs, 1 left, 2 right, 3 off (nach Neustart) */
extern int g_shfx_bypass; /* render_shfx.c: 1 = A/B "Original" */

static volatile LONG g_ov_open;         /* 1 = offen (UI-Thread schreibt) */
static volatile LONG g_ov_q[64];        /* Tasten-Ring UI -> Render */
static volatile LONG g_ov_qh, g_ov_qt;
static volatile LONG g_ov_swallow_k, g_ov_swallow_m; /* verschluckte Spiel-Tasten/Maustasten */
static volatile LONG g_ov_blocked_q;    /* gesperrte echte Tasten-Abfragen (key_down) */
static int g_ov_was_open;               /* Render-Thread: letzter gesehener Zustand */
static int g_ov_sel;                    /* gewaehlte Zeile (Index in der Seite) */
static int g_ov_page;                   /* W121: 0 = Allgemein, 1 = Shader (W131: 2 = Shader 2) */
static int g_ov_rts = -1;               /* W121/2: gewaehlte Welt-Aufloesung (rt_scale, wirkt nach Neustart) */
static int g_ov_dyn = -1;               /* W148: gewaehlte Dynamik (rt_scale_dyn, wirkt sofort) */
static int g_ov_init;
static int g_ov_fps;                    /* gewaehlte Bildrate (0 = 30, sonst Bilder/s; bei auto die aufgeloeste Rate) */
static int g_ov_auto;                   /* W223: 1 = Monitor (auto) gewaehlt */
static int g_ov_fps_pend;               /* 1 = Wechsel am Present-Ende faellig */
static volatile LONG g_ov_mon_hz;       /* W309: letzte bekannte Rate des Spielmonitors (0 = unbekannt) */
static int g_ov_winmode;                /* Darstellung (nach Neustart) */
static int g_ov_label0;                  /* W153: hud_label_avoid beim Start ("*" bei Abweichung) */
static int g_ov_avail;                  /* HD-Paket: 1 = x2, 2 = x4 */
static int g_ov_dirty = 1;              /* Panel neu zeichnen */
static ULONGLONG g_ov_sl_arm;           /* W190: "Uebernehmen" scharf bis (zweites ENTER bestaetigt) */
/* ---- W188 GAME-MENU: Bildschirme ----
 * SC_SET = Einstellungen (W118-Seiten), SC_MENU = Spielmenue (erste Ebene),
 * SC_QUIT = Rueckfrage Beenden, SC_PAUSED = Pause-Taste, SC_FB* = Feedback. */
enum { SC_SET = 0, SC_MENU, SC_QUIT, SC_PAUSED, SC_FBKIND, SC_FBTEXT, SC_FBREVIEW, SC_PHOTO /* W195 */,
       SC_WELCOME /* W222: Alpha-Hinweis beim Start */ };
#define OV_IS_FB(sc) ((sc) >= SC_FBKIND && (sc) <= SC_FBREVIEW)
enum { MI_CONTINUE = 0, MI_SETTINGS, MI_PHOTO, MI_FEEDBACK, MI_QUIT, MI_N };
/* interne Tasten im Ring (ueber dem VK-Bereich) */
#define OK_BTAB    0x1001  /* Umschalt+Tab / LB: Seite zurueck */
#define OK_PAUSE   0x1002  /* Pause-Taste (Belegung) */
#define OK_WUP     0x1003  /* Mausrad */
#define OK_WDN     0x1004
#define OK_PSTART  0x1005  /* Pad START */
#define OK_PHOTO   0x1006  /* W195: Foto-Taste (Belegung "photo") bei offenem Menue */
#define OK_PSHOT   0x1007  /* W195: Pad Y im Fotomodus = Foto aufnehmen */
#define OK_CHAR    0x20000 /* | Zeichen (UTF-16), Texteingabe */
#define OK_CLICK   0x40000000 /* | x << 15 | y (Backbuffer-Pixel) */
#define OV_MSG_TEST   (WM_APP + 0x1A8) /* TEST(W188): wp = interne Taste (UI-Thread schiebt sie in den Ring) */
#define OV_MSG_CLOSED (WM_APP + 0x1A9) /* W188: Overlay zu -> UI-Thread faengt die Maus wieder */
#define OV_MSG_OPENED (WM_APP + 0x1AA) /* W222: Overlay vom Render-Thread geoeffnet -> UI-Thread gibt Maus/Tasten frei */
static volatile LONG g_ov_welcome_on;    /* W222: Alpha-Hinweis offen (UI-Thread liest: beliebige Taste schliesst) */
static int g_ov_welcome_done;            /* W222: einmal je Programmstart (Render-Thread) */
static unsigned g_ov_welcome_f0;         /* W222: erstes Bild (Wartezeit auf die Profil-Belegung) */
static volatile LONG g_fb_bind_known;    /* W222: Feedback-Belegung aus dem Profil uebernommen */
static volatile LONG g_ov_notice_req;    /* W254: Hinweis angefordert (beliebiger Thread) */
static int g_ov_notice_on;               /* W254: Hinweis-Bildschirm zeigt g_ov_notice statt Alpha-Hinweis */
static char g_ov_notice[6][160];         /* W254: Zeilen (UTF-8, schon uebersetzt) */
static const char *g_ov_notice_tkey = "ovl.ra_title", *g_ov_notice_gkey = "ovl.ra_tag"; /* W272: Titel/Kennzeichen */
static int g_ov_scr = SC_SET;            /* Render-Thread */
static volatile LONG g_ov_req = -1;      /* UI-Thread: Bildschirm beim Oeffnen */
static volatile LONG g_ov_textmode;      /* 1 = Texteingabe (WM_CHAR ans Overlay) */
static int g_ov_msel;                    /* Auswahl in den Menue-Bildschirmen */
static volatile LONG g_ov_pause_vk[4] = { VK_PAUSE, 0, 0, 0 }; /* Pause-Tasten (Profil-Aktion "pause") */
static volatile LONG g_ov_pause_pad;     /* Pad-Tasten der Aktion "pause" (Bits IMAP_PAD_*) */
static volatile LONG g_ov_photo_vk[4] = { VK_F12, 0, 0, 0 }; /* W195: Foto-Tasten (Profil-Aktion "photo") */
static volatile LONG g_ov_photo_mod[4] = { 1, 0, 0, 0 };      /* W290: Zusatztasten je Foto-Taste (Bit 0 Shift, 1 Strg, 2 Alt); Standard Shift+F12 */
static volatile LONG g_ov_photo_pad;     /* W195: Pad-Tasten der Aktion "photo" */
/* W290 QUICK-SHOT: Tasten der Aktion "quick_shot" (Standard F12) und Ablauf des Schnell-Screenshots */
static volatile LONG g_ov_qs_vk[4] = { VK_F12, 0, 0, 0 };
static volatile LONG g_ov_qs_mod[4] = { 0, 0, 0, 0 };
static volatile LONG g_ov_qs_pad;        /* Pad-Tasten der Aktion "quick_shot" (bei offenem Fotomodus = Foto) */
static volatile LONG g_qs_req;           /* 1 = Schnell-Screenshot angefordert (Fenster/Hook/Test) */
enum { QS_IDLE, QS_FLAT, QS_PAUSE, QS_REC }; /* ISSUE-PHOTO: QS_REC = Tick-Aufzeichnung bei Bedarf (interp_fps=0) */
static int g_qs_state, g_qs_notes;
static unsigned g_qs_t0;                 /* Present-Nr. beim Start (Zeitgrenze Vollbild-Ansicht) */
static ULONGLONG g_qs_ms0;
/* W195 PHOTO-MODE: Foto-Panel (Bildschirm SC_PHOTO) */
static int g_ph_tab, g_ph_sel, g_ph_from_menu;
static volatile LONG g_ph_keys;          /* gehaltene Kamera-Tasten (Bits PK_*, UI-Thread) */
static volatile LONG g_ph_mdx, g_ph_mdy; /* Maus-Ziehen (Client-Pixel, aufsummiert, UI-Thread) */
static volatile LONG g_ph_drag;          /* 1 = Ziehen laeuft (Start ausserhalb des Panels) */
static LONG g_ph_lx, g_ph_ly;            /* letzte Zieh-Position (UI-Thread) */
static ULONGLONG g_ph_t;                 /* letzter Kamera-Schritt (ms) */
static int g_ph_last_shown;              /* zuletzt gemeldete Aufnahme (Status) */
static int g_ph_pad_rx, g_ph_pad_ry, g_ph_pad_lx, g_ph_pad_ly, g_ph_pad_tr; /* Pad-Achsen (Fotomodus) */
enum { PK_W = 1, PK_S = 2, PK_A = 4, PK_D = 8, PK_Q = 16, PK_E = 32, PK_J = 64, PK_L = 128, PK_I = 256, PK_K = 512 };
static unsigned g_ov_vframes;            /* Pausenbilder (Test-Hook-Takt laeuft in der Pause weiter) */
static wchar_t g_fb_text[161];
static wchar_t g_fb_high; /* W192: noch nicht vollstaendiges UTF-16-Paar */
static int g_fb_len, g_fb_kind, g_fb_have, g_fb_pending;
/* W211: UI publishes a request; only the render thread owns the package. */
static volatile LONG g_fb_hot_request;
static volatile LONG g_fb_vk[2][4] = {{ VK_F11, 0, 0, 0 }, { VK_F9, 0, 0, 0 }};
static int g_fb_hot, g_fb_capture, g_fb_clip_prompt;
static sb_fb_pkg_t g_fb_pkg;
/* Trefferflaechen (Backbuffer-Pixel, Render-Thread schreibt beim Zeichnen) */
static int g_hit_x0, g_hit_y0, g_hit_w, g_hit_h, g_hit_rows_y, g_hit_rowh, g_hit_nrows, g_hit_valx;
static int g_hit_tab_y0, g_hit_tab_y1, g_hit_tab_x[OV_NPAGES + 1];
static int g_hit_side_h, g_hit_rows_x0, g_hit_head_y1; /* W291: Seitenleiste (Eintragshoehe), Listenbeginn, Kopf (Zurueck) */
static volatile LONG g_ov_mx = -1, g_ov_my = -1, g_ov_mgen; /* letzte Mausposition (UI-Thread) */
static LONG g_ov_mgen_seen;
static unsigned g_pad_inject;            /* TEST(W188): Pad-Tasten fuer den naechsten Poll */
static wchar_t g_ov_status[96];
static ULONGLONG g_ov_status_until;
static char g_ov_save_path[600];

/* ---- Test-Hook (overlay_test) ---- */
typedef struct { unsigned at; int rel; int mi; int act; int vk; char txt[28]; } ov_step_t;
enum { OA_KEY, OA_SHOT, OA_FOCUS1, OA_FOCUS0, OA_LOG, OA_PHUDPOS,
       OA_TKEY, OA_TXT, OA_MROW, OA_MTAB, OA_PAD, OA_STATE, /* W188 */
       OA_PEEK, /* W193: Taste "Menue ausblenden" druecken (vk 1) / loslassen (vk 0) */
       OA_PHOTO, OA_KDN, OA_KUP, OA_MDRAG, OA_CHAR, /* W192 OA_CHAR, W195: Foto-Taste, Taste halten/loslassen, Ziehen dx_dy */
       OA_QSHOT, /* W290: Schnell-Screenshot (Taste der Belegung bzw. mit Ansicht/Groesse direkt) */
       OA_MHOV, /* W197: Maus nur ueber Zeile N bewegen (VR-Zeiger) */
       OA_WMSG, /* W261: Fenster-Nachrichtenfolge Alt-Tab/Minimieren (vk = Art) */
       OA_CHECK }; /* W291: Struktur-Selbsttest der ESC-Gruppen (Log "OVERLAY check:") */
static ov_step_t g_ov_steps[160]; /* W193: 96 -> 160 (Pause-Live-Belege) */
static int g_ov_nsteps, g_ov_step, g_ov_test_parsed;
static unsigned g_ov_mission_f[8];      /* Startbild der k-ten Mission (fuer mN / mK.N) */
static int g_ov_nmission, g_ov_in_mission;
static unsigned g_ov_prev_f;            /* Bild des letzten Schritts (fuer +N) */
static int g_ov_shot_due;
static unsigned g_ov_shot_n;

/* ---- D3D ---- */
static ID3D11VertexShader *ov_vs;
static ID3D11PixelShader *ov_ps;
static ID3D11InputLayout *ov_lay;
static ID3D11Buffer *ov_vb;
static ID3D11BlendState *ov_bs;
static ID3D11RasterizerState *ov_rs;
static ID3D11DepthStencilState *ov_ds;
static ID3D11Texture2D *ov_tex;
static ID3D11ShaderResourceView *ov_srv;
static unsigned ov_tw, ov_th;
static int ov_state;                     /* 0 = noch nicht, 1 = bereit, -1 = gescheitert */
static sb_ovl_save_t ov_bbsave;          /* gesicherter BB-Bereich (W122: gemeinsame API) */

/* ---- GDI ---- */
static HDC ov_dc;
static HBITMAP ov_bmp, ov_bmp_old;
static unsigned *ov_bits;
static unsigned ov_bw, ov_bh;
static HFONT ov_font, ov_font_old;
static int ov_font_px;

/* ------------------------------------------------------------------ */
/* Hilfen                                                              */
/* ------------------------------------------------------------------ */
static void ov_status(const wchar_t *s)
{
    wcsncpy_s(g_ov_status, sizeof g_ov_status / sizeof g_ov_status[0], s, _TRUNCATE);
    g_ov_status_until = GetTickCount64() + 4000u;
    g_ov_dirty = 1;
}

/* W142: Schluessel-Text (UTF-8) nach wchar wandeln. */
static void ov_wkey(wchar_t *o, size_t cap, const char *key)
{
    sb_lang_w(key, o, (int)cap);
}

static void ov_wu8(wchar_t *o, size_t cap, const char *u8)
{
    if (!u8 || MultiByteToWideChar(CP_UTF8, 0, u8, -1, o, (int)cap) <= 0) {
        o[0] = 0;
    }
}

static void ov_status_key(const char *key)
{
    wchar_t b[96];
    ov_wkey(b, 96, key);
    ov_status(b);
}

static int ov_clamp(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* W291 ESC-REWORK: Gruppe sichtbar? VR nur bei VR (oder ausstehendem VR-Start),
 * Debug nur mit Alpha-Tools (Launcher -> esc_debug) oder solange ein
 * Spass-Effekt an ist (sonst waere er unsichtbar eingeschaltet). */
static int ov_grp_vis(int g)
{
    if (g == OG_VR) { return g_cfg.vr_enable || g_ov_vr == 1; }
    if (g == OG_DEBUG) { return g_cfg.esc_debug || g_cfg.fx_wireframe || g_cfg.fx_vaporwave; }
    return g >= 0 && g < OG_N;
}

/* W291: Zeile unter den gegebenen Umstaenden vorhanden (Controller-Zeile nur mit sbc_usb). */
static int ov_row_avail(int i)
{
    return i != OI_SBCMOUSE || g_cfg.sbc_usb;
}

/* W296 PRESET-ORIG: Verlaufsbreite/Abdunklung stehen bei "Fade out" (edge_mode=2)
 * sichtbar direkt unter der Breitbild-Zeile (wie im Launcher), sonst gedimmt
 * unter "Erweitert". Heimat bleibt kOgDispA (OVERLAY check). */
static int ov_row_promoted(int i)
{
    return (i == OI_EDGEW || i == OI_EDGES) && g_cfg.edge_mode == 2;
}

/* W291: sichtbare Feinregler der Gruppe (W296: ohne die nach oben geholten) */
static int ov_grp_nadv(int g)
{
    int k, n = 0;
    for (k = 0; k < kOvGrp[g].nadv; k++) { n += ov_row_avail(kOvGrp[g].adv[k]) && !ov_row_promoted(kOvGrp[g].adv[k]); }
    return n;
}

/* W291: sichtbare Zeilen der aktuellen Gruppe neu aufbauen (billig, je Aufruf):
 * Hauptzeilen, "Erweitert" (+ Feinregler, wenn aufgeklappt), Aktionen. */
static void ov_rows_build(void)
{
    static int s_page = -1;
    const ov_grp_t *gp;
    int k, n = 0, prev;
    if (g_ov_page < 0 || g_ov_page >= OG_N || !ov_grp_vis(g_ov_page)) { g_ov_page = OG_QUICK; }
    /* W296: gewaehlte Zeile derselben Gruppe merken (Verlauf erscheint/verschwindet mit "Fade out") */
    prev = (g_ov_page == s_page && g_ov_sel >= 0 && g_ov_sel < g_ov_nrv) ? g_ov_rv[g_ov_sel] : -1;
    s_page = g_ov_page;
    gp = &kOvGrp[g_ov_page];
    for (k = 0; k < gp->nmain && n < OV_MAXROWS; k++) {
        if (ov_row_avail(gp->main[k])) { g_ov_rv[n++] = gp->main[k]; }
        if (gp->main[k] == OI_EDGE && g_ov_page != OG_QUICK) { /* W296: Verlauf bei "Fade out" sichtbar */
            int j;
            for (j = 0; j < gp->nadv && n < OV_MAXROWS; j++) {
                if (ov_row_promoted(gp->adv[j]) && ov_row_avail(gp->adv[j])) { g_ov_rv[n++] = gp->adv[j]; }
            }
        }
    }
    if (ov_grp_nadv(g_ov_page) > 0) {
        g_ov_rv[n++] = OI_ADV;
        if (g_ov_adv[g_ov_page]) {
            for (k = 0; k < gp->nadv && n < OV_MAXROWS; k++) {
                if (ov_row_avail(gp->adv[k]) && !ov_row_promoted(gp->adv[k])) { g_ov_rv[n++] = gp->adv[k]; }
            }
        }
    }
    if (gp->reset && n < OV_MAXROWS) { g_ov_rv[n++] = OI_RESET; }
    if (n < OV_MAXROWS) { g_ov_rv[n++] = OI_SAVE; }
    g_ov_nrv = n;
    if (prev >= 0 && (g_ov_sel >= n || g_ov_rv[g_ov_sel] != prev)) { /* W296: Auswahl folgt der Zeile */
        for (k = 0; k < n; k++) {
            if (g_ov_rv[k] == prev) { g_ov_sel = k; break; }
        }
    }
    if (g_ov_sel >= n) { g_ov_sel = n - 1; } /* zugeklappt oder Gruppe verschwunden (VR aus) */
}

static int ov_nrows(void)
{
    ov_rows_build();
    return g_ov_nrv;
}

static int ov_row(int k)
{
    ov_rows_build();
    return (k >= 0 && k < g_ov_nrv) ? g_ov_rv[k] : OI_SAVE;
}

/* W291: Zeile ist nur Information (nicht waehlbar) */
static int ov_row_info(int i)
{
    return i == OI_RES || i == OI_TEXINFO;
}

static int ov_hd_on(void)
{
    return g_cfg.remaster && g_cfg.hd_textures;
}

static int ov_hd_scale(void)
{
    return g_cfg.hd_scale == 2 ? 2 : 4;
}

/* Zeile bedienbar? (gedimmte Zeilen ueberspringt die Auswahl nicht, aendern
 * sich aber nicht) */
static int ov_enabled(int i)
{
    switch (i) {
    case OI_GRAIN: case OI_GLITCH: return g_cfg.monitor_fx != 0;
    case OI_EDGEW: case OI_EDGES: return g_cfg.edge_mode == 2;
    case OI_MSENS: return 1;
    case OI_HD: return g_ov_avail != 0;
    case OI_HDSCALE: return g_ov_avail != 0 && ov_hd_on();
    case OI_RES: case OI_TEXINFO: return 0;
    case OI_STEX: return g_ov_avail != 0; /* W291: Texturen (Sammel) nur mit Paket */
    case OI_SLCOMMIT: return krnl_savelock_active(); /* W190: nur mit aktivem Schutz */
    case OI_XWIRE: case OI_XVAPOR: return !g_cfg.vr_enable; /* W240: in VR gesperrt */
    case OI_CHIDE: return !g_cfg.vr_enable; /* W268: in VR gesperrt (flache Leinwand statt Cockpit) */
    case OI_MONVIEW: return !g_cfg.vr_enable; /* W319: in VR gesperrt */
    case OI_FPS: return !(g_sba_loc && !g_cfg.loc_interp); /* W323: LoC ohne Zwischenbilder -> fest 20 Hz (Original) */
    default: return 1;
    }
}

/* W121: Zeile wirkt gerade (Welt-Optionen nur mit Shading) - sonst gedimmt */
static int ov_effective(int i)
{
    switch (i) {
    case OI_FOG: case OI_WCON: case OI_WGAM: case OI_WSHARP: case OI_BLOOM:
    case OI_EXPO: case OI_TONE: case OI_BTHR: case OI_LWORLD: case OI_LCKP: case OI_FOCUS: return g_cfg.shading != 0;
    case OI_SHADOW: case OI_VQASYNC: return g_cfg.shading != 0; /* W131 */
    case OI_SLIGHT: case OI_SBLOOM: case OI_SSHADOW: return g_cfg.shading != 0; /* W291: Sammel wie die Einzelwerte */
    case OI_SHSOFT: return g_cfg.shading != 0 && g_cfg.shade_shadow != 0;
    case OI_CPERSP: case OI_CPANINI: case OI_CWIDTH: return !g_cfg.vr_enable; /* W191/W235: nur Desktop, in VR ohne Wirkung (gedimmt) */
    case OI_RPFX: return g_cfg.cockpit_hide && !g_cfg.vr_enable && g_cfg.monitor_view <= 0; /* W299: wirkt nur in der Vollbild-Ansicht */
    case OI_CHIDE: return !g_cfg.vr_enable && g_cfg.monitor_view <= 0; /* W319: Monitoransicht hat Vorrang (gedimmt) */
    default: return ov_enabled(i);
    }
}

static void ov_log_values(const char *why)
{
    HLOG_I("OVERLAY werte (%s): fps=%d shading=%d monitor_fx=%d grain=%d glitch=%d edge=%d/%d/%d "
           "maus=%s/%d hd=%d x%d darstellung=%d f=%u", why, g_cfg.interp_fps, g_cfg.shading,
           g_cfg.monitor_fx, g_cfg.monitor_grain, g_cfg.monitor_glitch, g_cfg.edge_mode,
           g_cfg.edge_fade_width, g_cfg.edge_fade_strength, g_cfg.mouse_mode ? "absolut" : "relativ",
           g_cfg.mouse_sens, ov_hd_on(), ov_hd_scale(), g_ov_winmode, g_present_no);
}

static void ov_log_shader(const char *why)
{
    HLOG_I("OVERLAY shader (%s): nebel=%d kontrast=%d gamma=%d schaerfe=%d bloom=%d glow=%d spill=%d "
           "licht=%d vergleich=%s belichtung=%d tonemap=%d bloom_schwelle=%d licht_welt=%d licht_cockpit=%d "
           "rt_scale=%d randunschaerfe=%d f=%u", why, g_cfg.world_fog, g_cfg.world_contrast, g_cfg.world_gamma,
           g_cfg.world_sharpen, g_cfg.bloom_strength, g_cfg.cockpit_glow, g_cfg.cockpit_spill,
           g_cfg.cockpit_light, g_shfx_bypass ? "original" : "optionen", g_cfg.world_exposure, g_cfg.tonemap,
           g_cfg.bloom_threshold, g_cfg.light_world, g_cfg.light_cockpit, g_ov_rts >= 0 ? g_ov_rts : g_cfg.rt_scale,
           g_cfg.world_focus, g_present_no);
}

/* W131 SHADER-OPT2: eigene Zeile (OVERLAY shader bleibt unveraendert) */
static void ov_log_shader2(const char *why)
{
    int pm = shfx_preset_match();
    HLOG_I("OVERLAY shader2 (%s): preset=%s aa=%d ssao=%d saettigung=%d temperatur=%d lift=%d farbgamma=%d gain=%d "
           "schatten=%d schatten_weich=%d visq_async=%d f=%u", why, shfx_preset_name(pm), g_cfg.aa_mode, g_cfg.ssao,
           g_cfg.color_sat, g_cfg.color_temp, g_cfg.color_lift, g_cfg.color_gamma, g_cfg.color_gain,
           g_cfg.shade_shadow, g_cfg.shadow_soft, g_cfg.vis_query_async, g_present_no);
}

/* W133 REM-FX: eigene Zeile (OVERLAY werte/shader/shader2 bleiben unveraendert) */
static void ov_log_fx(const char *why)
{
    HLOG_I("OVERLAY effekte (%s): preset=%d soft=%d glut=%d licht=%d licht_cockpit=%d interp=%d vergleich=%s f=%u", why,
           fxr_preset_match(), g_cfg.fx_soft, g_cfg.fx_embers, g_cfg.fx_light, g_cfg.fx_light_cockpit, g_cfg.fx_interp,
           g_fxr_bypass ? "original" : "optionen", g_present_no);
}

/* ------------------------------------------------------------------ */
/* Werte aendern (Render-Thread)                                       */
/* ------------------------------------------------------------------ */
static void ov_hd_apply(int on, int scale, const char *why)
{
    int r = sb_rem_live_set(on, scale);
    if (r < 0) {
        ov_status_key("ovl.st_hd_none");
        HLOG_W("OVERLAY: HD %s x%d nicht moeglich (kein Paket) f=%u", on ? "an" : "aus", scale, g_present_no);
        return;
    }
    sb_tex_relive_all(); /* Texturen neu hochladen (Original bzw. Ersatz) */
    HLOG_I("OVERLAY: %s HD-Texturen %s x%d (Textur-Cache neu, f=%u)", why, on ? "an" : "aus", scale,
           g_present_no);
    ov_status_key(on ? (scale == 2 ? "ovl.st_hd2" : "ovl.st_hd4") : "ovl.st_hd_off");
}

/* W190 SAVE-LOCK: "Geschuetzte Aenderungen uebernehmen" - erstes ENTER macht
 * scharf (Hinweis), zweites innerhalb von 4 s uebernimmt (alter Stand wird
 * vorher unter <hdd>\_backup\savelock-<zeit>\ gesichert). */
static void ov_sl_commit(void)
{
    ULONGLONG now = GetTickCount64();
    int rc;
    if (!krnl_savelock_active()) {
        ov_status_key("ovl.st_sl_none");
        return;
    }
    if (!g_ov_sl_arm || now > g_ov_sl_arm) {
        g_ov_sl_arm = now + 4000u;
        ov_status_key("ovl.st_sl_confirm");
        return;
    }
    g_ov_sl_arm = 0;
    rc = krnl_savelock_commit();
    HLOG_I("OVERLAY: Spielstaende uebernehmen -> %d (f=%u)", rc, g_present_no);
    ov_status_key(rc == 0 ? "ovl.st_sl_done" : rc == 1 ? "ovl.st_sl_busy" : "ovl.st_sl_cfail");
}

/* ---- W291 ESC-REWORK: Sammelregler --------------------------------------
 * Setzen nur vorhandene Schluessel gemeinsam (kein neuer Render-Pfad, kein
 * neuer Schluessel). Anzeige = Stufe, deren Werte exakt passen, sonst
 * "Benutzerdefiniert"; von dort springt der erste Druck aufs Original. */
typedef struct {
    const char *log;            /* Logname (OVERLAY: sammel <log> -> <stufe>) */
    int nk;
    const char *key[5];         /* cfg-Schluessel (Log, EXP-Zustand) */
    int *f[5];
    int ns;
    short v[6][5];
    const char *lbl[6];         /* Sprachschluessel je Stufe; NULL = "<erster Wert> %" */
    int orig;                   /* Stufe des Originals */
} ov_sum_t;
enum { SM_LIGHT, SM_SHADOW, SM_BLOOM, SM_GLOW, SM_LOOK, SM_N };
static const ov_sum_t kOvSum[SM_N] = {
    { "licht", 2, { "light_world", "light_cockpit" }, { &g_cfg.light_world, &g_cfg.light_cockpit }, 5,
      { { 60, 60 }, { 80, 80 }, { 100, 100 }, { 120, 120 }, { 140, 140 } }, { NULL, NULL, "ovl.v_orig", NULL, NULL }, 2 },
    { "schatten", 2, { "shade_shadow", "shadow_soft" }, { &g_cfg.shade_shadow, &g_cfg.shadow_soft }, 3,
      { { 0, 0 }, { 1, 0 }, { 1, 1 } }, { "ovl.v_offorig", "ovl.v_hard", "ovl.v_softsh" }, 0 },
    { "bloom", 2, { "bloom_strength", "bloom_threshold" }, { &g_cfg.bloom_strength, &g_cfg.bloom_threshold }, 3,
      { { 0, 0 }, { 100, 0 }, { 120, 20 } }, { "ovl.v_off", "ovl.v_orig", "ovl.v_look_strong" }, 1 },
    { "cockpit_leuchten", 2, { "cockpit_glow", "cockpit_spill" }, { &g_cfg.cockpit_glow, &g_cfg.cockpit_spill }, 3,
      { { 0, 0 }, { 30, 20 }, { 60, 40 } }, { "ovl.v_offorig", "ovl.v_look_subtle", "ovl.v_look_strong" }, 0 },
    { "look", 5, { "color_sat", "color_temp", "color_lift", "color_gamma", "color_gain" },
      { &g_cfg.color_sat, &g_cfg.color_temp, &g_cfg.color_lift, &g_cfg.color_gamma, &g_cfg.color_gain }, 6,
      { { 100, 0, 0, 100, 100 }, { 105, 0, 0, 100, 103 }, { 115, 10, 0, 100, 110 }, { 105, 30, 0, 100, 100 },
        { 95, -30, 0, 100, 100 }, { 90, 10, -5, 95, 110 } },
      { "ovl.v_orig", "ovl.v_look_subtle", "ovl.v_look_strong", "ovl.v_look_warm", "ovl.v_look_cold",
        "ovl.v_look_cinema" }, 0 },
};
/* Launcher kMonFx: Original, nur Korn, nur Stoerung (Korn %, Stoerung %); Stufe 3 = Aus (monitor_fx 0) */
static const int kOvMonFx[3][2] = { { 100, 100 }, { 100, 0 }, { 0, 100 } };

static int ov_sum_idx(int i)
{
    switch (i) {
    case OI_SLIGHT: return SM_LIGHT;
    case OI_SSHADOW: return SM_SHADOW;
    case OI_SBLOOM: return SM_BLOOM;
    case OI_SGLOW: return SM_GLOW;
    case OI_SLOOK: return SM_LOOK;
    default: return -1;
    }
}

static int ov_sum_match(int sm)
{
    const ov_sum_t *m = &kOvSum[sm];
    int q, k;
    for (q = 0; q < m->ns; q++) {
        for (k = 0; k < m->nk && *m->f[k] == m->v[q][k]; k++) {}
        if (k == m->nk) { return q; }
    }
    return -1;
}

static void ov_sum_change(int sm, int dir)
{
    const ov_sum_t *m = &kOvSum[sm];
    int q0 = ov_sum_match(sm), q, k;
    q = q0 < 0 ? m->orig : ov_clamp(q0 + dir, 0, m->ns - 1);
    if (q == q0) { return; }
    for (k = 0; k < m->nk; k++) {
        if (*m->f[k] != m->v[q][k]) {
            *m->f[k] = m->v[q][k];
            HLOG_I("OVERLAY: %s -> %d (f=%u)", m->key[k], *m->f[k], g_present_no);
        }
    }
    HLOG_I("OVERLAY: sammel %s -> %d (f=%u)", m->log, q, g_present_no);
}

/* Monitor-Effekt: 0 Original, 1 nur Korn, 2 nur Stoerung, 3 Aus, -1 Zwischenwerte, -2 Altpfad A/B */
static int ov_mon_stage(void)
{
    int q;
    if (g_cfg.monitor_fx == 0) { return 3; }
    if (g_cfg.monitor_fx != 1) { return -2; }
    for (q = 0; q < 3; q++) {
        if (g_cfg.monitor_grain == kOvMonFx[q][0] && g_cfg.monitor_glitch == kOvMonFx[q][1]) { return q; }
    }
    return -1;
}

static void ov_mon_change(int dir)
{
    int q0 = ov_mon_stage(), q = q0 < 0 ? 0 : ov_clamp(q0 + dir, 0, 3);
    if (q == q0) { return; }
    g_cfg.monitor_fx = q == 3 ? 0 : 1;
    HLOG_I("OVERLAY: monitor_fx -> %d (f=%u)", g_cfg.monitor_fx, g_present_no);
    if (q < 3) { /* Aus laesst Korn/Stoerung stehen (wie Launcher W252) */
        g_cfg.monitor_grain = kOvMonFx[q][0];
        g_cfg.monitor_glitch = kOvMonFx[q][1];
        HLOG_I("OVERLAY: monitor_grain -> %d (f=%u)", g_cfg.monitor_grain, g_present_no);
        HLOG_I("OVERLAY: monitor_glitch -> %d (f=%u)", g_cfg.monitor_glitch, g_present_no);
    }
    HLOG_I("OVERLAY: sammel monitor -> %d (f=%u)", q, g_present_no);
}

/* Texturen: 0 Original, 1 HD 2x, 2 HD 4x (nur vorhandene Stufen, g_ov_avail Bit 0 = x2, Bit 1 = x4) */
static int ov_tex_stage(void)
{
    return !ov_hd_on() ? 0 : (ov_hd_scale() == 2 ? 1 : 2);
}

static void ov_tex_change(int dir)
{
    int q = ov_tex_stage();
    for (;;) {
        q += dir;
        if (q < 0 || q > 2) { return; }
        if (q == 0 || (g_ov_avail & (q == 1 ? 1 : 2))) { break; }
    }
    ov_hd_apply(q != 0, q == 1 ? 2 : (q == 2 ? 4 : ov_hd_scale()), "Texturen:");
    HLOG_I("OVERLAY: sammel texturen -> %d (f=%u)", q, g_present_no);
}

/* ---- W291: "Auf Original" je Gruppe / "Zurueck zum Original" (Schnell) ----
 * Nur Tabellen-Schluessel aus sb_origflag.h (wie Launcher lorig_reset und die
 * Umschalttaste). Packs, Bildrate, Fenster, Steuerung, VR bleiben. Wirkt live,
 * gespeichert wird erst mit "Als Standard speichern". */
static const struct { const char *key; unsigned char og; } kOvKeyGrp[] = {
    { "edge_mode", OG_DISP }, { "edge_fade_width", OG_DISP }, { "edge_fade_strength", OG_DISP },
    { "cockpit_hide", OG_CKP }, { "monitor_view", OG_CKP }, { "monitor_fx", OG_CKP }, { "monitor_grain", OG_CKP }, { "monitor_glitch", OG_CKP },
    { "cockpit_persp", OG_DISP }, { "cockpit_panini", OG_DISP }, { "cockpit_width", OG_DISP },
    { "hud_label_avoid", OG_CKP }, { "replay_label", OG_CKP }, { "replay_logo", OG_CKP },
    { "shading", OG_WORLD }, { "world_fog", OG_WORLD }, { "world_contrast", OG_WORLD }, { "world_gamma", OG_WORLD },
    { "bloom_strength", OG_WORLD }, { "cockpit_glow", OG_WORLD }, { "cockpit_spill", OG_WORLD },
    { "cockpit_light", OG_WORLD }, { "world_exposure", OG_WORLD }, { "tonemap", OG_WORLD },
    { "bloom_threshold", OG_WORLD }, { "light_world", OG_WORLD }, { "light_cockpit", OG_WORLD },
    { "shade_shadow", OG_WORLD }, { "shadow_soft", OG_WORLD },
    { "fx_soft", OG_FX }, { "fx_embers", OG_FX }, { "fx_light", OG_FX }, { "fx_light_cockpit", OG_FX },
    { "fx_interp", OG_FX }, { "aa_mode", OG_FX }, { "ssao", OG_FX }, { "world_sharpen", OG_FX }, { "world_focus", OG_FX },
    { "color_sat", OG_COLOR }, { "color_temp", OG_COLOR }, { "color_lift", OG_COLOR }, { "color_gamma", OG_COLOR },
    { "color_gain", OG_COLOR },
    { "hd_textures", OG_TEX },
    { "fx_wireframe", OG_DEBUG }, { "fx_vaporwave", OG_DEBUG },
};
static int g_ov_undo;                    /* 1 = "Zurueck zum Original" lief, Zeile heisst "Rueckgaengig" */
static int g_ov_usnap[SB_OF_NKEY], g_ov_usnap_rem, g_ov_usnap_hd;
static int g_ov_usnap_rpfx; /* W299: replay_fx_live (kein Tabellen-Schluessel) */

static int ov_key_grp(const char *key)
{
    int k;
    for (k = 0; k < (int)(sizeof kOvKeyGrp / sizeof kOvKeyGrp[0]); k++) {
        if (strcmp(kOvKeyGrp[k].key, key) == 0) { return kOvKeyGrp[k].og; }
    }
    return -1;
}

static void ov_reset(int og)
{
    int i, n = 0, hd = 0;
    if (og == OG_QUICK) { /* Schnappschuss fuer "Rueckgaengig" */
        for (i = 0; i < SB_OF_NKEY; i++) {
            int *p = sb_ov_field(kSbOrigFlag[i].key);
            g_ov_usnap[i] = p ? *p : kSbOrigFlag[i].orig;
        }
        g_ov_usnap_rem = g_cfg.remaster;
        g_ov_usnap_hd = g_cfg.hd_textures;
        g_ov_usnap_rpfx = g_cfg.replay_fx_live;
    }
    if ((og == OG_QUICK || og == OG_FX) && g_cfg.replay_fx_live) { /* W299: Replay-Filter live ist nicht original */
        g_cfg.replay_fx_live = 0;
        HLOG_I("OVERLAY: replay_fx_live -> 0 (f=%u)", g_present_no);
        n++;
    }
    for (i = 0; i < SB_OF_NKEY; i++) {
        const char *k = kSbOrigFlag[i].key;
        int *p;
        if (og != OG_QUICK && ov_key_grp(k) != og) { continue; }
        if (strcmp(k, "hd_textures") == 0) { hd = 1; continue; }
        p = sb_ov_field(k);
        if (p && *p != kSbOrigFlag[i].orig) {
            *p = kSbOrigFlag[i].orig;
            HLOG_I("OVERLAY: %s -> %d (f=%u)", k, *p, g_present_no);
            n++;
        }
    }
    if (hd && ov_hd_on()) {
        if (g_ov_avail) { ov_hd_apply(0, ov_hd_scale(), "Original:"); }
        else { g_cfg.hd_textures = 0; }
        n++;
    }
    if (og == OG_AUDIO) { /* W303: Lautstaerke (nicht in kSbOrigFlag, neutral) -> 100 % */
        int *vp[4] = { &g_cfg.volume_master, &g_cfg.volume_music, &g_cfg.volume_sfx, &g_cfg.volume_voice };
        for (i = 0; i < 4; i++) {
            if (*vp[i] != 100) { *vp[i] = 100; n++; }
        }
    }
    if (og == OG_QUICK || og == OG_DEBUG) { /* Vergleich (B) gehoert zum Debug */
        g_shfx_bypass = 0;
        g_fxr_bypass = 0;
    }
    sb_render_pause_dirty();
    HLOG_I("OVERLAY: auf Original (%s): %d Werte (f=%u)", kOvGrp[og].log, n, g_present_no);
    if (og == OG_QUICK) { g_ov_undo = 1; }
    ov_status_key(og == OG_QUICK ? "ovl.st_reset_all" : og == OG_DEBUG ? "ovl.st_alloff" : "ovl.st_reset_grp");
}

static void ov_undo(void)
{
    int i, hd_want = (g_ov_usnap_rem && g_ov_usnap_hd) ? 1 : 0;
    for (i = 0; i < SB_OF_NKEY; i++) {
        int *p;
        if (strcmp(kSbOrigFlag[i].key, "hd_textures") == 0) { continue; }
        p = sb_ov_field(kSbOrigFlag[i].key);
        if (p) { *p = g_ov_usnap[i]; }
    }
    if (hd_want && !ov_hd_on() && g_ov_avail) { ov_hd_apply(1, ov_hd_scale(), "Rueckgaengig:"); }
    g_cfg.remaster = g_ov_usnap_rem;
    g_cfg.hd_textures = g_ov_usnap_hd;
    g_cfg.replay_fx_live = g_ov_usnap_rpfx; /* W299 */
    g_ov_undo = 0;
    sb_render_pause_dirty();
    HLOG_I("OVERLAY: Rueckgaengig (eigene Werte zurueck, f=%u)", g_present_no);
    ov_status_key("ovl.st_undo");
}

/* W223 FPS-RATES: Auswahl fuer cfg/Preset-Erkennung (auto = SB_FPS_AUTO). */
static int ov_fps_sel(void)
{
    return g_ov_auto ? SB_FPS_AUTO : g_ov_fps;
}

/* W223: Wiederholrate des Monitors mit dem Spielfenster (0 = unbekannt). */
static int ov_monitor_hz(void)
{
    HWND h = sb_render_hwnd();
    int hz = sb_fps_monitor_hz(h ? MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY) : NULL);
    InterlockedExchange(&g_ov_mon_hz, hz); /* W309: Kennzeichen in der Anzeige */
    return hz;
}

/* W309 FPS-MONITOR: Rate des Spielmonitors aus sb_winmode_tick (feste Rate,
 * Monitorwechsel); nur gemerkt fuer das Kennzeichen in der Anzeige. */
void sb_ovl_monitor_hz(int hz)
{
    InterlockedExchange(&g_ov_mon_hz, hz);
}

static void ov_change(int i, int dir)
{
    int k;
    if (!ov_enabled(i)) { return; }
    switch (i) {
    case OI_FPS: { /* W223 FPS-RATES: Monitor (auto), 30, 60 ... 240; W309: nur bis zur Monitorrate */
        SbFpsItem fl[SB_FPS_LIST_MAX];
        int hz0 = ov_monitor_hz(), nl = sb_fps_list(hz0, ov_fps_sel(), fl, SB_FPS_LIST_MAX);
        int k0 = sb_fps_list_idx(fl, nl, ov_fps_sel());
        if (k0 < 0) { /* alter Sonderwert (z. B. aus der cfg): naechster Listenwert darueber */
            for (k0 = 1; k0 < nl - 1 && fl[k0].v < g_ov_fps; k0++) {}
        }
        k = ov_clamp(k0 + dir, 0, nl - 1);
        if (k == k0) { return; }
        if (fl[k].v == SB_FPS_AUTO) {
            int hz = ov_monitor_hz();
            g_ov_auto = 1;
            g_ov_fps = sb_fps_auto_from_hz(hz);
            HLOG_I("OVERLAY: Bildrate auto -> %d (Monitor %d Hz, f=%u)", g_ov_fps, hz, g_present_no);
        } else {
            g_ov_auto = 0;
            g_ov_fps = fl[k].v;
        }
        g_ov_fps_pend = 1;
        HLOG_I("OVERLAY: Bildrate -> %d (interp_fps, f=%u)", g_ov_fps ? g_ov_fps : 30, g_present_no);
        /* W309: Statuszeile nennt, warum ein Wert ruckeln kann (ueber Monitor / kein Teiler) */
        ov_status_key((fl[k].flags & SB_FPS_F_ABOVE) ? "ovl.st_fps_above" :
                      (fl[k].flags & SB_FPS_F_JUDDER) ? "ovl.st_fps_judder" : "ovl.st_fps");
        break;
    }
    case OI_SHADING:
        g_cfg.shading = !g_cfg.shading;
        HLOG_I("OVERLAY: shading -> %d (f=%u)", g_cfg.shading, g_present_no);
        break;
    case OI_MFX:
        g_cfg.monitor_fx = g_cfg.monitor_fx ? 0 : 1;
        HLOG_I("OVERLAY: monitor_fx -> %d (f=%u)", g_cfg.monitor_fx, g_present_no);
        break;
    case OI_GRAIN:
        g_cfg.monitor_grain = ov_clamp(g_cfg.monitor_grain + dir * 10, 0, 100);
        HLOG_I("OVERLAY: monitor_grain -> %d (f=%u)", g_cfg.monitor_grain, g_present_no);
        break;
    case OI_GLITCH:
        g_cfg.monitor_glitch = ov_clamp(g_cfg.monitor_glitch + dir * 10, 0, 100);
        HLOG_I("OVERLAY: monitor_glitch -> %d (f=%u)", g_cfg.monitor_glitch, g_present_no);
        break;
    case OI_EDGE:
        g_cfg.edge_mode = (g_cfg.edge_mode + (dir > 0 ? 1 : 2)) % 3;
        HLOG_I("OVERLAY: edge_mode -> %d (f=%u)", g_cfg.edge_mode, g_present_no);
        break;
    case OI_EDGEW:
        g_cfg.edge_fade_width = ov_clamp(g_cfg.edge_fade_width + dir * 2, 0, 50);
        HLOG_I("OVERLAY: edge_fade_width -> %d (f=%u)", g_cfg.edge_fade_width, g_present_no);
        break;
    case OI_EDGES:
        g_cfg.edge_fade_strength = ov_clamp(g_cfg.edge_fade_strength + dir * 10, 0, 100);
        HLOG_I("OVERLAY: edge_fade_strength -> %d (f=%u)", g_cfg.edge_fade_strength, g_present_no);
        break;
    case OI_MMODE:
        g_cfg.mouse_mode = !g_cfg.mouse_mode;
        HLOG_I("OVERLAY: mouse_mode -> %s (f=%u)", g_cfg.mouse_mode ? "absolute" : "relative", g_present_no);
        break;
    case OI_MSENS:
        g_cfg.mouse_sens = ov_clamp(g_cfg.mouse_sens + dir, 1, 16);
        HLOG_I("OVERLAY: mouse_sens -> %d (f=%u)", g_cfg.mouse_sens, g_present_no);
        break;
    case OI_ASSIST: /* W236: 3 Stufen, wirkt sofort (Hook liest g_cfg je Poll) */
        g_cfg.aim_assist = (g_cfg.aim_assist + (dir > 0 ? 1 : 2)) % 3;
        HLOG_I("OVERLAY: aim_assist -> %d (f=%u)", g_cfg.aim_assist, g_present_no);
        ov_status_key(g_cfg.aim_assist == 2 ? "ovl.st_assist2" :
                      g_cfg.aim_assist == 1 ? "ovl.st_assist1" : "ovl.st_assist0");
        break;
    case OI_AUTOGEAR: /* W218 */
        g_cfg.auto_gear = !g_cfg.auto_gear;
        HLOG_I("OVERLAY: auto_gear -> %d (f=%u)", g_cfg.auto_gear, g_present_no);
        break;
    case OI_STICKAIM: /* W271: wirkt sofort (Hook liest g_cfg je Poll) */
        g_cfg.stick_aim = !g_cfg.stick_aim;
        HLOG_I("OVERLAY: stick_aim -> %s (f=%u)", g_cfg.stick_aim ? "relative" : "position", g_present_no);
        ov_status_key(g_cfg.stick_aim ? "ovl.st_stick1" : "ovl.st_stick0");
        break;
    case OI_STICKSENS: /* W271 */
        g_cfg.stick_aim_sens = ov_clamp(g_cfg.stick_aim_sens + dir, 1, 16);
        HLOG_I("OVERLAY: stick_aim_sens -> %d (f=%u)", g_cfg.stick_aim_sens, g_present_no);
        break;
    case OI_SBCMOUSE: /* W128: wirkt sofort (Hook liest g_cfg je Poll) */
        g_cfg.sbc_mouse_off = !g_cfg.sbc_mouse_off;
        HLOG_I("OVERLAY: sbc_mouse_off -> %d (f=%u)", g_cfg.sbc_mouse_off, g_present_no);
        break;
    case OI_HD:
        ov_hd_apply(!ov_hd_on(), ov_hd_scale(), "Schalter:");
        break;
    case OI_HDSCALE:
        if (!(g_ov_avail & (ov_hd_scale() == 4 ? 1 : 2))) {
            ov_status_key("ovl.st_hd_stage");
            return;
        }
        ov_hd_apply(1, ov_hd_scale() == 4 ? 2 : 4, "Stufe:");
        break;
    case OI_FOG: { /* Original 100 / Reduziert 50 / Aus 0 (Zwischenwerte von Hand bleiben bis zum Wechsel) */
        static const int fv[3] = { 100, 50, 0 };
        int q = g_cfg.world_fog >= 75 ? 0 : (g_cfg.world_fog >= 25 ? 1 : 2);
        q = ov_clamp(q + dir, 0, 2);
        if (fv[q] == g_cfg.world_fog) { return; }
        g_cfg.world_fog = fv[q];
        HLOG_I("OVERLAY: world_fog -> %d (f=%u)", g_cfg.world_fog, g_present_no);
        break;
    }
    case OI_WCON:
        g_cfg.world_contrast = ov_clamp(g_cfg.world_contrast + dir * 10, 50, 200);
        HLOG_I("OVERLAY: world_contrast -> %d (f=%u)", g_cfg.world_contrast, g_present_no);
        break;
    case OI_WGAM:
        g_cfg.world_gamma = ov_clamp(g_cfg.world_gamma + dir * 10, 50, 200);
        HLOG_I("OVERLAY: world_gamma -> %d (f=%u)", g_cfg.world_gamma, g_present_no);
        break;
    case OI_WSHARP:
        g_cfg.world_sharpen = ov_clamp(g_cfg.world_sharpen + dir * 10, 0, 100);
        HLOG_I("OVERLAY: world_sharpen -> %d (f=%u)", g_cfg.world_sharpen, g_present_no);
        break;
    case OI_BLOOM:
        g_cfg.bloom_strength = ov_clamp(g_cfg.bloom_strength + dir * 10, 0, 200);
        HLOG_I("OVERLAY: bloom_strength -> %d (f=%u)", g_cfg.bloom_strength, g_present_no);
        break;
    case OI_CGLOW:
        g_cfg.cockpit_glow = ov_clamp(g_cfg.cockpit_glow + dir * 10, 0, 100);
        HLOG_I("OVERLAY: cockpit_glow -> %d (f=%u)", g_cfg.cockpit_glow, g_present_no);
        break;
    case OI_CSPILL:
        g_cfg.cockpit_spill = ov_clamp(g_cfg.cockpit_spill + dir * 10, 0, 100);
        HLOG_I("OVERLAY: cockpit_spill -> %d (f=%u)", g_cfg.cockpit_spill, g_present_no);
        break;
    case OI_CLIGHT:
        g_cfg.cockpit_light = ov_clamp(g_cfg.cockpit_light + dir * 10, 30, 100);
        HLOG_I("OVERLAY: cockpit_light -> %d (f=%u)", g_cfg.cockpit_light, g_present_no);
        break;
    case OI_EXPO: { /* Belichtung in Viertel-Blenden-Naehe: 25..400 % */
        static const int ev[] = { 25, 35, 50, 60, 70, 80, 90, 100, 110, 125, 150, 175, 200, 250, 300, 400 };
        int q, nq = (int)(sizeof ev / sizeof ev[0]);
        for (q = 0; q < nq - 1 && ev[q] < g_cfg.world_exposure; q++) {}
        q = ov_clamp(q + dir, 0, nq - 1);
        if (ev[q] == g_cfg.world_exposure) { return; }
        g_cfg.world_exposure = ev[q];
        HLOG_I("OVERLAY: world_exposure -> %d (f=%u)", g_cfg.world_exposure, g_present_no);
        break;
    }
    case OI_TONE:
        g_cfg.tonemap = (g_cfg.tonemap + (dir > 0 ? 1 : 2)) % 3;
        HLOG_I("OVERLAY: tonemap -> %d (f=%u)", g_cfg.tonemap, g_present_no);
        break;
    case OI_BTHR:
        g_cfg.bloom_threshold = ov_clamp(g_cfg.bloom_threshold + dir * 10, 0, 90);
        HLOG_I("OVERLAY: bloom_threshold -> %d (f=%u)", g_cfg.bloom_threshold, g_present_no);
        break;
    case OI_LWORLD:
        g_cfg.light_world = ov_clamp(g_cfg.light_world + dir * 10, 0, 200);
        HLOG_I("OVERLAY: light_world -> %d (f=%u)", g_cfg.light_world, g_present_no);
        break;
    case OI_FOCUS:
        g_cfg.world_focus = ov_clamp(g_cfg.world_focus + dir * 10, 0, 100);
        HLOG_I("OVERLAY: world_focus -> %d (f=%u)", g_cfg.world_focus, g_present_no);
        break;
    case OI_LCKP:
        g_cfg.light_cockpit = ov_clamp(g_cfg.light_cockpit + dir * 10, 0, 200);
        HLOG_I("OVERLAY: light_cockpit -> %d (f=%u)", g_cfg.light_cockpit, g_present_no);
        break;
    case OI_RTS: { /* W148: Auto-dyn, Auto-fest, 1..4 (Skala wirkt nach Neustart, dyn sofort) */
        static const int rv[6] = { -2, 0, 1, 2, 3, 4 }; /* -2 = Auto (dynamisch) */
        int q, cur;
        if (g_ov_rts < 0) { g_ov_rts = g_cfg.rt_scale; }
        if (g_ov_dyn < 0) { g_ov_dyn = g_cfg.rt_scale_dyn ? 1 : 0; }
        cur = g_ov_dyn ? -2 : g_ov_rts;
        for (q = 0; q < 6 && rv[q] != cur; q++) {}
        if (q >= 6) { q = 0; }
        q = ov_clamp(q + dir, 0, 5);
        if (rv[q] < 0) { /* Auto (dynamisch): dyn sofort, Skala 1 -> auto */
            g_ov_dyn = 1;
            g_cfg.rt_scale_dyn = 1;
            if (g_ov_rts == 1) { g_ov_rts = 0; }
        } else {
            g_ov_dyn = 0;
            g_cfg.rt_scale_dyn = 0;
            g_ov_rts = rv[q];
        }
        HLOG_I("OVERLAY: rt_scale -> %d dyn=%d (Skala wirkt nach Neustart, f=%u)", g_ov_rts, g_ov_dyn, g_present_no);
        ov_status_key("ovl.st_rt");
        break;
    }
    case OI_PRESET: { /* W131, W173 LAUNCHER-P4: einheitliches Grafik-Preset Original -> dezent -> stark
                       * (Shader + Effekte + HD-Texturen, sb_gfxpreset.h; Effect-Pack-Schluessel bleiben) */
        int q = sb_gfx_preset_match_for(ov_fps_sel(), g_ov_rts >= 0 ? g_ov_rts : g_cfg.rt_scale), q0 = q, hd;
        if (q < 0) { q = dir > 0 ? -1 : SB_GP_NPRE; }
        q = ov_clamp(q + dir, 0, SB_GP_NPRE - 1);
        if (q == q0) { return; }
        sb_gfx_part_apply(q, SB_GFX_ALL);
        if (kSbGfxPreset[SB_GP_FPS].v[q] == SB_FPS_AUTO) { /* W296 PRESET-ORIG: Original = Monitor (auto) */
            int hz = ov_monitor_hz();
            g_ov_auto = 1;
            g_ov_fps = sb_fps_auto_from_hz(hz);
            HLOG_I("OVERLAY: Bildrate auto -> %d (Preset, Monitor %d Hz, f=%u)", g_ov_fps, hz, g_present_no);
        } else {
            g_ov_fps = kSbGfxPreset[SB_GP_FPS].v[q];
            g_ov_auto = 0; /* W223 */
        }
        g_ov_fps_pend = 1; /* W200: commit under context lock after pause */
        g_ov_rts = kSbGfxPreset[SB_GP_RT].v[q];
        g_ov_dyn = g_cfg.rt_scale_dyn = kSbGfxPreset[SB_GP_DYN].v[q];
        g_cfg.fs_interp_match = kSbGfxPreset[SB_GP_MATCH].v[q];
        if (!sb_rem_fx_locked(kSbGfxPreset[SB_GP_MFX].key)) { /* W252: Monitor-Effekte live */
            g_cfg.monitor_fx = kSbGfxPreset[SB_GP_MFX].v[q];
        }
        sb_gfx_edge_apply(q); /* W266 PRESET-EDGE: Breitbild-Raender live (je Present gelesen) */
        ov_status_key(q == SB_GP_PERFORMANCE ? "ovl.st_performance" : q == SB_GP_CLEAR ? "ovl.st_clear" : "ovl.st_rt");
        hd = kSbGfxPreset[SB_GP_HD].v[q];
        if (hd != (ov_hd_on() ? 1 : 0)) {
            if (g_ov_avail) {
                ov_hd_apply(hd, ov_hd_scale(), "Preset:");
            } else { /* ohne Paket nur der Wert (wirkt, sobald ein Pack da ist) */
                g_cfg.remaster = hd;
                g_cfg.hd_textures = hd;
            }
        }
        HLOG_I("OVERLAY: preset -> %s (f=%u)", shfx_preset_name(q), g_present_no);
        HLOG_I("OVERLAY: edge (preset): mode=%d breite=%d staerke=%d", g_cfg.edge_mode, g_cfg.edge_fade_width,
               g_cfg.edge_fade_strength); /* W266 */
        ov_log_shader2("preset");
        ov_log_fx("preset");
        break;
    }
    case OI_AA:
        g_cfg.aa_mode = (g_cfg.aa_mode + (dir > 0 ? 1 : 3)) % 4;
        HLOG_I("OVERLAY: aa_mode -> %d (f=%u)", g_cfg.aa_mode, g_present_no);
        break;
    case OI_SSAO:
        g_cfg.ssao = ov_clamp(g_cfg.ssao + dir * 10, 0, 100);
        HLOG_I("OVERLAY: ssao -> %d (BB-Tiefe %s, f=%u)", g_cfg.ssao, g_bb_dsrv ? "da" : "fehlt: nach Neustart", g_present_no);
        if (g_cfg.ssao > 0 && !g_bb_dsrv) { ov_status_key("ovl.st_ssao"); }
        break;
    case OI_CSAT:
        g_cfg.color_sat = ov_clamp(g_cfg.color_sat + dir * 5, 0, 200);
        HLOG_I("OVERLAY: color_sat -> %d (f=%u)", g_cfg.color_sat, g_present_no);
        break;
    case OI_CTEMP:
        g_cfg.color_temp = ov_clamp(g_cfg.color_temp + dir * 10, -100, 100);
        HLOG_I("OVERLAY: color_temp -> %d (f=%u)", g_cfg.color_temp, g_present_no);
        break;
    case OI_CLIFT:
        g_cfg.color_lift = ov_clamp(g_cfg.color_lift + dir * 5, -50, 50);
        HLOG_I("OVERLAY: color_lift -> %d (f=%u)", g_cfg.color_lift, g_present_no);
        break;
    case OI_CGAM:
        g_cfg.color_gamma = ov_clamp(g_cfg.color_gamma + dir * 5, 50, 200);
        HLOG_I("OVERLAY: color_gamma -> %d (f=%u)", g_cfg.color_gamma, g_present_no);
        break;
    case OI_CGAIN:
        g_cfg.color_gain = ov_clamp(g_cfg.color_gain + dir * 5, 50, 200);
        HLOG_I("OVERLAY: color_gain -> %d (f=%u)", g_cfg.color_gain, g_present_no);
        break;
    case OI_SHADOW: /* W127 Schattenpuffer: wirkt ab dem naechsten Tiefenpass */
        g_cfg.shade_shadow = g_cfg.shade_shadow ? 0 : 1;
        HLOG_I("OVERLAY: shade_shadow -> %d (f=%u)", g_cfg.shade_shadow, g_present_no);
        break;
    case OI_SHSOFT:
        g_cfg.shadow_soft = !g_cfg.shadow_soft;
        HLOG_I("OVERLAY: shadow_soft -> %d (f=%u)", g_cfg.shadow_soft, g_present_no);
        break;
    case OI_VQASYNC:
        g_cfg.vis_query_async = !g_cfg.vis_query_async;
        HLOG_I("OVERLAY: vis_query_async -> %d (f=%u)", g_cfg.vis_query_async, g_present_no);
        break;
    case OI_CPERSP: /* W191 UW-PERSP: 0 (Original) .. 100 % in 10er-Schritten, wirkt sofort (je Bild gelesen) */
        g_cfg.cockpit_persp = ov_clamp((g_cfg.cockpit_persp / 10) * 10 + dir * 10, 0, 100);
        HLOG_I("OVERLAY: cockpit_persp -> %d (f=%u)", g_cfg.cockpit_persp, g_present_no);
        sb_render_pause_dirty(); /* W193 PAUSE-LIVE: Standbild mit der neuen Perspektive neu zeichnen */
        ov_status_key(g_cfg.vr_enable ? "ovl.st_cpersp_vr" : "ovl.st_cpersp");
        break;
    case OI_CPANINI: /* W191 UW-PERSP: Panini 0..100 % in 10er-Schritten, live (Nachbearbeitung je gezeigtem Bild) */
        g_cfg.cockpit_panini = ov_clamp((g_cfg.cockpit_panini / 10) * 10 + dir * 10, 0, 100);
        HLOG_I("OVERLAY: cockpit_panini -> %d (f=%u)", g_cfg.cockpit_panini, g_present_no);
        sb_render_pause_dirty(); /* W209: redraw on either camera slider */
        ov_status_key(g_cfg.vr_enable ? "ovl.st_cpersp_vr" : "ovl.st_cpanini");
        break;
    case OI_CWIDTH: /* W235 COCKPIT-WIDTH: 0 (Original) .. 10 % in 1er-Schritten, live (Nachbearbeitung je gezeigtem Bild) */
        g_cfg.cockpit_width = ov_clamp(g_cfg.cockpit_width + dir, 0, 10);
        HLOG_I("OVERLAY: cockpit_width -> %d (f=%u)", g_cfg.cockpit_width, g_present_no);
        sb_render_pause_dirty(); /* wie W209: Standbild-Abgleich der Cockpit-Ansicht */
        ov_status_key(g_cfg.vr_enable ? "ovl.st_cpersp_vr" : "ovl.st_cwidth");
        break;
    case OI_CHIDE: /* W268 COCKPIT-HIDE: wirkt im laufenden Spiel ab dem naechsten Bild (Swap-Hook), nicht im Pause-Standbild */
        g_cfg.cockpit_hide = !g_cfg.cockpit_hide;
        HLOG_I("OVERLAY: cockpit_hide -> %d (f=%u)", g_cfg.cockpit_hide, g_present_no);
        ov_status_key(g_cfg.cockpit_hide ? "ovl.st_chide_on" : "ovl.st_chide_off");
        break;
    case OI_MONVIEW: /* W319 MONITOR-VIEW: Aus / Hauptmonitor / + Radar und FRONT VIEW (Umlauf), wirkt ab dem naechsten Spielbild */
        g_cfg.monitor_view = (g_cfg.monitor_view + dir + 3) % 3;
        HLOG_I("OVERLAY: monitor_view -> %d (f=%u)", g_cfg.monitor_view, g_present_no);
        ov_status_key(g_cfg.vr_enable ? "ovl.st_mview_vr" : g_cfg.monitor_view == 2 ? "ovl.st_mview_2" :
                      g_cfg.monitor_view == 1 ? "ovl.st_mview_1" : "ovl.st_mview_0");
        break;
    case OI_LABEL: /* W153: wirkt ab Neustart (Patch beim Start) */
        g_cfg.hud_label_avoid = !g_cfg.hud_label_avoid;
        HLOG_I("OVERLAY: hud_label_avoid -> %d (wirkt nach Neustart, f=%u)", g_cfg.hud_label_avoid, g_present_no);
        ov_status_key("ovl.st_label");
        break;
    case OI_FXPRESET: { /* W133: Original -> dezent -> stark (setzt die Einzelwerte) */
        int q = fxr_preset_match(), q0 = q;
        if (q < 0) { q = dir > 0 ? -1 : 3; }
        q = ov_clamp(q + dir, 0, 2);
        if (q == q0) { return; }
        fxr_preset_apply(q);
        HLOG_I("OVERLAY: effekt-preset -> %d (f=%u)", q, g_present_no);
        ov_log_fx("preset");
        break;
    }
    case OI_FXSOFT:
        g_cfg.fx_soft = ov_clamp(g_cfg.fx_soft + dir * 20, 0, 200);
        HLOG_I("OVERLAY: fx_soft -> %d (f=%u)", g_cfg.fx_soft, g_present_no);
        break;
    case OI_FXEMB:
        g_cfg.fx_embers = ov_clamp(g_cfg.fx_embers + dir * 20, 0, 200);
        HLOG_I("OVERLAY: fx_embers -> %d (f=%u)", g_cfg.fx_embers, g_present_no);
        break;
    case OI_FXLIGHT:
        g_cfg.fx_light = ov_clamp(g_cfg.fx_light + dir * 20, 0, 200);
        HLOG_I("OVERLAY: fx_light -> %d (f=%u)", g_cfg.fx_light, g_present_no);
        break;
    case OI_FXLCKP:
        g_cfg.fx_light_cockpit = ov_clamp(g_cfg.fx_light_cockpit + dir * 20, 0, 200);
        HLOG_I("OVERLAY: fx_light_cockpit -> %d (f=%u)", g_cfg.fx_light_cockpit, g_present_no);
        break;
    case OI_FXIP:
        g_cfg.fx_interp = !g_cfg.fx_interp;
        HLOG_I("OVERLAY: fx_interp -> %d (f=%u)", g_cfg.fx_interp, g_present_no);
        break;
    case OI_FXCMP:
        g_fxr_bypass = !g_fxr_bypass;
        HLOG_I("OVERLAY: Effekt-Vergleich -> %s (f=%u)", g_fxr_bypass ? "Original" : "Optionen", g_present_no);
        ov_status_key(g_fxr_bypass ? "ovl.st_cmp_orig" : "ovl.st_cmp_opt");
        break;
    case OI_ABCMP:
        g_shfx_bypass = !g_shfx_bypass;
        HLOG_I("OVERLAY: Vergleich -> %s (Shader-Optionen %s, f=%u)", g_shfx_bypass ? "Original" : "Optionen",
               g_shfx_bypass ? "aus" : "an", g_present_no);
        ov_status_key(g_shfx_bypass ? "ovl.st_cmp_orig" : "ovl.st_cmp_opt");
        break;
    case OI_XWIRE: /* W240 FX-EXPERIMENTAL: sofort ab dem naechsten Spielbild (FPS-INTERP zeichnet mit) */
        g_cfg.fx_wireframe = !g_cfg.fx_wireframe;
        HLOG_I("OVERLAY: fx_wireframe -> %d (f=%u)", g_cfg.fx_wireframe, g_present_no);
        ov_status_key(!g_cfg.fx_wireframe ? "ovl.st_xp_off" : g_sb_paused ? "ovl.st_xwire_pause" : "ovl.st_xwire");
        break;
    case OI_XVAPOR: /* W240 FX-EXPERIMENTAL: Nachbearbeitung, sofort (auch Pause-Standbild) */
        g_cfg.fx_vaporwave = !g_cfg.fx_vaporwave;
        HLOG_I("OVERLAY: fx_vaporwave -> %d (f=%u)", g_cfg.fx_vaporwave, g_present_no);
        ov_status_key(g_cfg.fx_vaporwave ? "ovl.st_xvapor" : "ovl.st_xp_off");
        break;
    case OI_VRON: /* W134 VR-PROTO: wirkt nach Neustart */
        if (g_ov_vr < 0) { g_ov_vr = g_cfg.vr_enable ? 1 : 0; }
        g_ov_vr = !g_ov_vr;
        HLOG_I("OVERLAY: vr_enable -> %d (wirkt nach Neustart, f=%u)", g_ov_vr, g_present_no);
        ov_status_key("ovl.st_vr_restart");
        break;
    case OI_VRREC:
        sb_vr_recenter("ESC-Menue");
        HLOG_I("OVERLAY: VR Recentering (f=%u)", g_present_no);
        ov_status_key(g_cfg.vr_enable ? "ovl.st_vr_center" : "ovl.st_vr_off");
        break;
    case OI_VRSEAT:
        g_cfg.vr_seat_height = ov_clamp(g_cfg.vr_seat_height + dir * 10, -500, 500);
        HLOG_I("OVERLAY: vr_seat_height -> %d (f=%u)", g_cfg.vr_seat_height, g_present_no);
        break;
    case OI_VRSCALE:
        g_cfg.vr_world_scale = ov_clamp(g_cfg.vr_world_scale + dir * 5, 25, 400);
        HLOG_I("OVERLAY: vr_world_scale -> %d (f=%u)", g_cfg.vr_world_scale, g_present_no);
        break;
    case OI_VRIPD:
        g_cfg.vr_ipd = ov_clamp(g_cfg.vr_ipd + dir * 5, 500, 800);
        HLOG_I("OVERLAY: vr_ipd -> %d (f=%u)", g_cfg.vr_ipd, g_present_no);
        break;
    case OI_VRPREV: {
        static const char *const pv[4] = { "sbs", "left", "right", "off" };
        if (g_ov_vrprev < 0) {
            g_ov_vrprev = _stricmp(g_cfg.vr_preview, "left") == 0 ? 1 : _stricmp(g_cfg.vr_preview, "right") == 0 ? 2 :
                          (_stricmp(g_cfg.vr_preview, "off") == 0 || _stricmp(g_cfg.vr_preview, "xr") == 0) ? 3 : 0;
        }
        g_ov_vrprev = (g_ov_vrprev + (dir > 0 ? 1 : 3)) % 4;
        HLOG_I("OVERLAY: vr_preview -> %s (wirkt nach Neustart, f=%u)", pv[g_ov_vrprev], g_present_no);
        ov_status_key("ovl.st_prev_restart");
        break;
    }
    case OI_VRFWD: /* W183: Sitz vor/zurueck, wirkt sofort */
        g_cfg.vr_seat_forward = ov_clamp(g_cfg.vr_seat_forward + dir * 10, -500, 800);
        HLOG_I("OVERLAY: vr_seat_forward -> %d (f=%u)", g_cfg.vr_seat_forward, g_present_no);
        break;
    case OI_VRPRESET: { /* W183: Leistung -> Ausgewogen -> Qualitaet (setzt die Einzelwerte) */
        int q = sb_vr_preset_match(), rt0 = g_cfg.vr_rt_scale;
        if (q <= 0) { q = dir > 0 ? 0 : 4; }
        q = ov_clamp(q + dir, 1, 3);
        sb_vr_preset_apply(q);
        HLOG_I("OVERLAY: vr_preset -> %d (aufloesung %d%%, bildrate %d, spiegel %d%%, aussenwelt %s, shading %d, monitor-rt %d; f=%u)", q,
               g_cfg.vr_render_scale, g_cfg.vr_refresh_hz, g_cfg.vr_mirror_res, g_cfg.vr_world_tick ? "spieltakt" : "je-bild",
               g_cfg.vr_shading, g_cfg.vr_rt_scale, g_present_no);
        if (g_cfg.vr_rt_scale != rt0) { ov_status_key("ovl.st_vrrt_restart"); }
        break;
    }
    case OI_VRRES: {
        int k, cur = -1;
        for (k = 0; k < OV_NVRRES; k++) { if (kOvVrRes[k] <= g_cfg.vr_render_scale) { cur = k; } }
        if (cur >= 0 && kOvVrRes[cur] == g_cfg.vr_render_scale) { cur += dir; }
        else if (dir > 0) { cur++; } /* Zwischenwert: rechts = naechste Stufe darueber, links = darunter */
        cur = ov_clamp(cur, 0, OV_NVRRES - 1);
        g_cfg.vr_render_scale = kOvVrRes[cur];
        g_cfg.vr_preset = sb_vr_preset_match();
        HLOG_I("OVERLAY: vr_render_scale -> %d (f=%u)", g_cfg.vr_render_scale, g_present_no);
        break;
    }
    case OI_VRHZ: {
        int k, cur = 0;
        for (k = 0; k < OV_NVRHZ; k++) { if (kOvVrHz[k] == g_cfg.vr_refresh_hz) { cur = k; } }
        cur = (cur + (dir > 0 ? 1 : OV_NVRHZ - 1)) % OV_NVRHZ;
        g_cfg.vr_refresh_hz = kOvVrHz[cur];
        g_cfg.vr_preset = sb_vr_preset_match();
        HLOG_I("OVERLAY: vr_refresh_hz -> %d (f=%u)", g_cfg.vr_refresh_hz, g_present_no);
        break;
    }
    case OI_VRMIRROR: { /* W183: fensterfuellend -> Balken -> halbe Aufloesung -> aus */
        int m = !g_cfg.vr_mirror ? 3 : (g_cfg.vr_mirror_res < 100 ? 2 : (g_cfg.vr_mirror_fit ? 1 : 0));
        m = (m + (dir > 0 ? 1 : 3)) % 4;
        g_cfg.vr_mirror = m != 3;
        g_cfg.vr_mirror_fit = m == 1 ? 1 : 0;
        g_cfg.vr_mirror_res = m == 2 ? 50 : 100;
        g_cfg.vr_preset = sb_vr_preset_match();
        HLOG_I("OVERLAY: vr_mirror -> %d fit=%d res=%d (f=%u)", g_cfg.vr_mirror, g_cfg.vr_mirror_fit, g_cfg.vr_mirror_res, g_present_no);
        break;
    }
    case OI_VRWORLD:
        g_cfg.vr_world_tick = !g_cfg.vr_world_tick;
        g_cfg.vr_preset = sb_vr_preset_match();
        HLOG_I("OVERLAY: vr_world_tick -> %d (f=%u)", g_cfg.vr_world_tick, g_present_no);
        break;
    case OI_VRSHADE:
        g_cfg.vr_shading = !g_cfg.vr_shading;
        g_cfg.vr_preset = sb_vr_preset_match();
        HLOG_I("OVERLAY: vr_shading -> %d (f=%u)", g_cfg.vr_shading, g_present_no);
        break;
    case OI_VRSHELL: /* W242 VR-SHELL: wirkt sofort (naechstes VR-Bild) */
        g_cfg.vr_shell = !g_cfg.vr_shell;
        HLOG_I("OVERLAY: vr_shell -> %d (f=%u)", g_cfg.vr_shell, g_present_no);
        break;
    case OI_VRVIG:
        g_cfg.vr_vignette = ov_clamp((g_cfg.vr_vignette + 5) / 10 * 10 + dir * 10, 0, 100);
        HLOG_I("OVERLAY: vr_vignette -> %d (f=%u)", g_cfg.vr_vignette, g_present_no);
        break;
    case OI_MPAUSE: /* W188 GAME-MENU: wirkt sofort (Wunsch je Bild) */
        g_cfg.menu_pause = !g_cfg.menu_pause;
        HLOG_I("OVERLAY: menu_pause -> %d (f=%u)", g_cfg.menu_pause, g_present_no);
        break;
    case OI_SAVELOCK: /* W190 SAVE-LOCK: Einschalten wirkt sofort, Ausschalten erst beim Beenden */
        g_cfg.save_lock = !g_cfg.save_lock;
        if (g_cfg.save_lock && !krnl_savelock_active()) {
            if (krnl_savelock_begin("ESC-Menue") == 0) {
                ov_status_key("ovl.st_sl_on");
            } else {
                g_cfg.save_lock = 0;
                ov_status_key("ovl.st_sl_fail");
            }
        } else if (krnl_savelock_active()) {
            ov_status_key(g_cfg.save_lock ? "ovl.st_sl_on" : "ovl.st_sl_pending");
        } else {
            ov_status_key("ovl.st_sl_off");
        }
        g_ov_sl_arm = 0;
        HLOG_I("OVERLAY: save_lock -> %d (Schutz aktiv=%d, f=%u)", g_cfg.save_lock, krnl_savelock_active(),
               g_present_no);
        break;
    case OI_SMON: ov_mon_change(dir); break; /* W291 */
    case OI_RPLABEL: /* W290-Schalter: wirken ab dem naechsten Bild (Draw-Filter) bzw. beim naechsten Video-Export */
        g_cfg.replay_label = !g_cfg.replay_label;
        HLOG_I("OVERLAY: replay_label -> %d (f=%u)", g_cfg.replay_label, g_present_no);
        break;
    case OI_RPLOGO:
        g_cfg.replay_logo = !g_cfg.replay_logo;
        HLOG_I("OVERLAY: replay_logo -> %d (f=%u)", g_cfg.replay_logo, g_present_no);
        break;
    case OI_WMMEDIA:
        g_cfg.watermark_media = !g_cfg.watermark_media;
        HLOG_I("OVERLAY: watermark_media -> %d (f=%u)", g_cfg.watermark_media, g_present_no);
        break;
    case OI_VOLM: case OI_VOLMU: case OI_VOLSFX: case OI_VOLVOX: { /* W303: 5er-Schritte, Mischer liest laufend */
        int *vp = i == OI_VOLM ? &g_cfg.volume_master : i == OI_VOLMU ? &g_cfg.volume_music :
                  i == OI_VOLSFX ? &g_cfg.volume_sfx : &g_cfg.volume_voice;
        *vp = ov_clamp((*vp + dir * 5) / 5 * 5, 0, 100);
        HLOG_I("OVERLAY: %s -> %d (f=%u)", i == OI_VOLM ? "volume_master" : i == OI_VOLMU ? "volume_music" :
               i == OI_VOLSFX ? "volume_sfx" : "volume_voice", *vp, g_present_no);
        break;
    }
    case OI_RPFX: /* W299 REPLAY-FX: Aus, EFFECT #1..#8 (Umlauf), wirkt ab dem naechsten Bild */
        g_cfg.replay_fx_live = (g_cfg.replay_fx_live + dir + 9) % 9;
        HLOG_I("OVERLAY: replay_fx_live -> %d (f=%u)", g_cfg.replay_fx_live, g_present_no);
        break;
    case OI_STEX: ov_tex_change(dir); break; /* W291 */
    case OI_SLIGHT: case OI_SSHADOW: case OI_SBLOOM: case OI_SGLOW: case OI_SLOOK: /* W291 */
        ov_sum_change(ov_sum_idx(i), dir);
        break;
    case OI_ADV: /* W291: rechts auf, links zu */
        if ((dir > 0) == (g_ov_adv[g_ov_page] != 0)) { return; }
        g_ov_adv[g_ov_page] = dir > 0;
        HLOG_I("OVERLAY: Erweitert %s (%s, f=%u)", dir > 0 ? "auf" : "zu", kOvGrp[g_ov_page].log, g_present_no);
        break;
    case OI_WINMODE:
        g_ov_winmode = (g_ov_winmode + (dir > 0 ? 1 : 2)) % 3;
        HLOG_I("OVERLAY: window_mode -> %d (wirkt nach Neustart, f=%u)", g_ov_winmode, g_present_no);
        ov_status_key("ovl.st_win_restart");
        break;
    default:
        return;
    }
    g_ov_dirty = 1;
}

/* ------------------------------------------------------------------ */
/* Speichern: user.cfg wie der Launcher (launcher_cfg.c lcfg_save_user):  */
/* erste Zeile je verwaltetem Key ersetzen, Rest unveraendert, fehlende */
/* Keys unter ihrer Section anhaengen.                                 */
/* ------------------------------------------------------------------ */
typedef struct { const char *key, *sec; } ov_key_t;
static const ov_key_t kOvKeys[] = {
    { "fs_interp_match", "render" },
    { "interp_fps", "render" },     { "shading", "render" },
    { SB_GP_REV_KEY, "render" }, /* W296 PRESET-ORIG: Stand der Preset-Tabelle (immer SB_GP_REV) */
    { "monitor_fx", "render" },     { "monitor_grain", "render" },
    { "monitor_glitch", "render" }, { "edge_mode", "render" },
    { "edge_fade_width", "render" }, { "edge_fade_strength", "render" },
    /* W121 SHADE-FX (vor [input]: neue Datei -> unter [render]) */
    { "world_fog", "render" },      { "world_contrast", "render" },
    { "world_gamma", "render" },    { "world_sharpen", "render" },
    { "bloom_strength", "render" }, { "cockpit_glow", "render" },
    { "cockpit_spill", "render" },  { "cockpit_light", "render" },
    /* W121 Teil 2 */
    { "world_exposure", "render" }, { "tonemap", "render" },
    { "bloom_threshold", "render" }, { "light_world", "render" },
    { "light_cockpit", "render" },  { "rt_scale", "render" },
    { "rt_scale_dyn", "render" }, /* W148 */
    { "world_focus", "render" },
    /* W131 SHADER-OPT2 */
    { "aa_mode", "render" },        { "ssao", "render" },
    { "color_sat", "render" },      { "color_temp", "render" },
    { "color_lift", "render" },     { "color_gamma", "render" },
    { "color_gain", "render" },     { "shade_shadow", "render" },
    { "shadow_soft", "render" },    { "vis_query_async", "render" },
    { "hud_label_avoid", "render" }, /* W153 */
    { "replay_label", "render" },   { "replay_logo", "render" }, /* W290-Schalter (Abschnitte wie launcher_cfg.c) */
    { "watermark_media", "overlay" },
    { "replay_fx_live", "render" }, /* W299 */
    { "cockpit_persp", "render" }, /* W191 */
    { "cockpit_panini", "render" }, /* W191 */
    { "cockpit_width", "render" }, /* W235 */
    { "cockpit_hide", "render" }, /* W268 */
    { "monitor_view", "render" }, /* W319 */
    /* W133 REM-FX */
    { "fx_soft", "render" },        { "fx_embers", "render" },
    { "fx_light", "render" },       { "fx_light_cockpit", "render" },
    { "fx_interp", "render" },
    { "fx_wireframe", "render" },   { "fx_vaporwave", "render" }, /* W240 */
    { "mouse_mode", "input" },      { "mouse_sens", "input" },
    { "sbc_mouse_off", "input" }, /* W128 */
    { "aim_assist", "input" },      { "auto_gear", "input" }, /* W218, W236 */
    { "aim_turn", "input" }, /* W236: Altwert neutralisieren (immer 0) */
    { "stick_aim", "input" },       { "stick_aim_sens", "input" }, /* W271 */
    { "remaster", "remaster" },     { "hd_textures", "remaster" },
    { "hd_scale", "remaster" },     { "window_mode", "display" },
    /* W134 VR-PROTO */
    { "vr_enable", "vr" },          { "vr_preview", "vr" },
    { "vr_seat_height", "vr" },     { "vr_world_scale", "vr" },
    { "vr_ipd", "vr" },
    /* W183 VR-FIX3 */
    { "vr_seat_forward", "vr" },    { "vr_preset", "vr" },
    { "vr_render_scale", "vr" },    { "vr_refresh_hz", "vr" },
    { "vr_mirror", "vr" },          { "vr_mirror_fit", "vr" },
    { "vr_mirror_res", "vr" },      { "vr_world_tick", "vr" },
    { "vr_shading", "vr" },         { "vr_rt_scale", "vr" },
    { "vr_shell", "vr" },           { "vr_vignette", "vr" }, /* W242 */
    { "menu_pause", "overlay" }, /* W188 */
    { "save_lock", "game" }, /* W190 */
    { "volume_master", "audio" },   { "volume_music", "audio" }, /* W303 AUDIO-VOLUME */
    { "volume_sfx", "audio" },      { "volume_voice", "audio" },
};
#define OV_NKEYS (sizeof kOvKeys / sizeof kOvKeys[0])

static void ov_key_value(const char *k, char *out, size_t cap)
{
    int v = 0;
    if (strcmp(k, "mouse_mode") == 0) {
        strcpy_s(out, cap, g_cfg.mouse_mode ? "absolute" : "relative");
        return;
    }
    if (strcmp(k, "stick_aim") == 0) { /* W271 */
        strcpy_s(out, cap, g_cfg.stick_aim ? "relative" : "position");
        return;
    }
    if (strcmp(k, "vr_preview") == 0) { /* W134 */
        static const char *const pv[4] = { "sbs", "left", "right", "off" };
        if (g_ov_vrprev >= 0) { strcpy_s(out, cap, pv[g_ov_vrprev]); } else { strcpy_s(out, cap, g_cfg.vr_preview); }
        return;
    }
    if (strcmp(k, "rt_scale") == 0) { /* W121/2: wie der Launcher ("auto" oder Faktor) */
        v = g_ov_rts >= 0 ? g_ov_rts : g_cfg.rt_scale;
        if (v == 0) { strcpy_s(out, cap, "auto"); } else { _snprintf_s(out, cap, _TRUNCATE, "%d", v); }
        return;
    }
    if (strcmp(k, "rt_scale_dyn") == 0) { v = g_cfg.rt_scale_dyn ? 1 : 0; } /* W148 */
    if (strcmp(k, "fs_interp_match") == 0) { v = g_cfg.fs_interp_match; }
    else if (strcmp(k, "interp_fps") == 0) { v = ov_fps_sel(); } /* W223: auto -> -1 */
    else if (strcmp(k, SB_GP_REV_KEY) == 0) { v = SB_GP_REV; } /* W296: wer speichert, kennt Original = auto */
    else if (strcmp(k, "shading") == 0) { v = g_cfg.shading; }
    else if (strcmp(k, "monitor_fx") == 0) { v = g_cfg.monitor_fx; }
    else if (strcmp(k, "monitor_grain") == 0) { v = g_cfg.monitor_grain; }
    else if (strcmp(k, "monitor_glitch") == 0) { v = g_cfg.monitor_glitch; }
    else if (strcmp(k, "edge_mode") == 0) { v = g_cfg.edge_mode; }
    else if (strcmp(k, "cockpit_persp") == 0) { v = g_cfg.cockpit_persp; } /* W191 */
    else if (strcmp(k, "cockpit_panini") == 0) { v = g_cfg.cockpit_panini; } /* W191 */
    else if (strcmp(k, "cockpit_width") == 0) { v = g_cfg.cockpit_width; } /* W235 */
    else if (strcmp(k, "cockpit_hide") == 0) { v = g_cfg.cockpit_hide ? 1 : 0; } /* W268 */
    else if (strcmp(k, "monitor_view") == 0) { v = g_cfg.monitor_view; } /* W319 */
    else if (strcmp(k, "edge_fade_width") == 0) { v = g_cfg.edge_fade_width; }
    else if (strcmp(k, "edge_fade_strength") == 0) { v = g_cfg.edge_fade_strength; }
    else if (strcmp(k, "mouse_sens") == 0) { v = g_cfg.mouse_sens; }
    else if (strcmp(k, "sbc_mouse_off") == 0) { v = g_cfg.sbc_mouse_off; }
    else if (strcmp(k, "aim_assist") == 0) { v = g_cfg.aim_assist; } /* W236 */
    else if (strcmp(k, "aim_turn") == 0) { v = 0; } /* W236: Altwert (W218) neutral */
    else if (strcmp(k, "auto_gear") == 0) { v = g_cfg.auto_gear; }
    else if (strcmp(k, "stick_aim_sens") == 0) { v = g_cfg.stick_aim_sens; } /* W271 */
    else if (strcmp(k, "remaster") == 0) { v = g_cfg.remaster; }
    else if (strcmp(k, "hd_textures") == 0) { v = g_cfg.hd_textures; }
    else if (strcmp(k, "hd_scale") == 0) { v = ov_hd_scale(); }
    else if (strcmp(k, "window_mode") == 0) { v = g_ov_winmode; }
    else if (strcmp(k, "vr_enable") == 0) { v = g_ov_vr >= 0 ? g_ov_vr : g_cfg.vr_enable; } /* W134 */
    else if (strcmp(k, "vr_seat_height") == 0) { v = g_cfg.vr_seat_height; }
    else if (strcmp(k, "vr_world_scale") == 0) { v = g_cfg.vr_world_scale; }
    else if (strcmp(k, "vr_ipd") == 0) { v = g_cfg.vr_ipd; }
    else if (strcmp(k, "vr_seat_forward") == 0) { v = g_cfg.vr_seat_forward; } /* W183 */
    else if (strcmp(k, "vr_preset") == 0) { v = g_cfg.vr_preset; }
    else if (strcmp(k, "vr_render_scale") == 0) { v = g_cfg.vr_render_scale; }
    else if (strcmp(k, "vr_refresh_hz") == 0) { v = g_cfg.vr_refresh_hz; }
    else if (strcmp(k, "vr_mirror") == 0) { v = g_cfg.vr_mirror; }
    else if (strcmp(k, "vr_mirror_fit") == 0) { v = g_cfg.vr_mirror_fit; }
    else if (strcmp(k, "vr_mirror_res") == 0) { v = g_cfg.vr_mirror_res; }
    else if (strcmp(k, "vr_world_tick") == 0) { v = g_cfg.vr_world_tick; }
    else if (strcmp(k, "vr_shading") == 0) { v = g_cfg.vr_shading; }
    else if (strcmp(k, "vr_rt_scale") == 0) { v = g_cfg.vr_rt_scale; }
    else if (strcmp(k, "vr_shell") == 0) { v = g_cfg.vr_shell; } /* W242 */
    else if (strcmp(k, "vr_vignette") == 0) { v = g_cfg.vr_vignette; }
    else if (strcmp(k, "world_fog") == 0) { v = g_cfg.world_fog; }
    else if (strcmp(k, "world_contrast") == 0) { v = g_cfg.world_contrast; }
    else if (strcmp(k, "world_gamma") == 0) { v = g_cfg.world_gamma; }
    else if (strcmp(k, "world_sharpen") == 0) { v = g_cfg.world_sharpen; }
    else if (strcmp(k, "bloom_strength") == 0) { v = g_cfg.bloom_strength; }
    else if (strcmp(k, "cockpit_glow") == 0) { v = g_cfg.cockpit_glow; }
    else if (strcmp(k, "cockpit_spill") == 0) { v = g_cfg.cockpit_spill; }
    else if (strcmp(k, "cockpit_light") == 0) { v = g_cfg.cockpit_light; }
    else if (strcmp(k, "world_exposure") == 0) { v = g_cfg.world_exposure; }
    else if (strcmp(k, "tonemap") == 0) { v = g_cfg.tonemap; }
    else if (strcmp(k, "bloom_threshold") == 0) { v = g_cfg.bloom_threshold; }
    else if (strcmp(k, "light_world") == 0) { v = g_cfg.light_world; }
    else if (strcmp(k, "light_cockpit") == 0) { v = g_cfg.light_cockpit; }
    else if (strcmp(k, "world_focus") == 0) { v = g_cfg.world_focus; }
    else if (strcmp(k, "aa_mode") == 0) { v = g_cfg.aa_mode; } /* W131 */
    else if (strcmp(k, "ssao") == 0) { v = g_cfg.ssao; }
    else if (strcmp(k, "color_sat") == 0) { v = g_cfg.color_sat; }
    else if (strcmp(k, "color_temp") == 0) { v = g_cfg.color_temp; }
    else if (strcmp(k, "color_lift") == 0) { v = g_cfg.color_lift; }
    else if (strcmp(k, "color_gamma") == 0) { v = g_cfg.color_gamma; }
    else if (strcmp(k, "color_gain") == 0) { v = g_cfg.color_gain; }
    else if (strcmp(k, "shade_shadow") == 0) { v = g_cfg.shade_shadow; }
    else if (strcmp(k, "shadow_soft") == 0) { v = g_cfg.shadow_soft; }
    else if (strcmp(k, "hud_label_avoid") == 0) { v = g_cfg.hud_label_avoid; } /* W153 */
    else if (strcmp(k, "replay_label") == 0) { v = g_cfg.replay_label ? 1 : 0; } /* W290-Schalter */
    else if (strcmp(k, "replay_logo") == 0) { v = g_cfg.replay_logo ? 1 : 0; }
    else if (strcmp(k, "watermark_media") == 0) { v = g_cfg.watermark_media ? 1 : 0; }
    else if (strcmp(k, "replay_fx_live") == 0) { v = g_cfg.replay_fx_live; } /* W299 */
    else if (strcmp(k, "vis_query_async") == 0) { v = g_cfg.vis_query_async; }
    else if (strcmp(k, "fx_soft") == 0) { v = g_cfg.fx_soft; } /* W133 */
    else if (strcmp(k, "fx_embers") == 0) { v = g_cfg.fx_embers; }
    else if (strcmp(k, "fx_light") == 0) { v = g_cfg.fx_light; }
    else if (strcmp(k, "fx_light_cockpit") == 0) { v = g_cfg.fx_light_cockpit; }
    else if (strcmp(k, "fx_interp") == 0) { v = g_cfg.fx_interp; }
    else if (strcmp(k, "fx_wireframe") == 0) { v = g_cfg.fx_wireframe; } /* W240 */
    else if (strcmp(k, "fx_vaporwave") == 0) { v = g_cfg.fx_vaporwave; } /* W240 */
    else if (strcmp(k, "menu_pause") == 0) { v = g_cfg.menu_pause; } /* W188 */
    else if (strcmp(k, "save_lock") == 0) { v = g_cfg.save_lock; } /* W190 */
    else if (strcmp(k, "volume_master") == 0) { v = g_cfg.volume_master; } /* W303 */
    else if (strcmp(k, "volume_music") == 0) { v = g_cfg.volume_music; }
    else if (strcmp(k, "volume_sfx") == 0) { v = g_cfg.volume_sfx; }
    else if (strcmp(k, "volume_voice") == 0) { v = g_cfg.volume_voice; }
    _snprintf_s(out, cap, _TRUNCATE, "%d", v);
}

static int ov_line_is_key(const char *line, const char *key)
{
    size_t n;
    line += strspn(line, " \t");
    n = strlen(key);
    if (strncmp(line, key, n) != 0) { return 0; }
    line += n + strspn(line + n, " \t");
    return *line == '=';
}

/* W122: gemeinsamer Schreiber (sb_overlay.h): n Keys (key/sec/val) in die
 * cfg-Datei path - erste Zeile je Key ersetzen, Rest unveraendert, fehlende
 * Keys unter [sec] anhaengen. Rueckgabe: Anzahl geschriebener Keys, -1 = Fehler. */
int sb_ovl_cfg_write(const char *path, int nk, const char *const *keys, const char *const *secs,
                     const char *const *vals)
{
    FILE *f = NULL;
    char *txt, line[4096];
    size_t len = 0, cap;
    long sz = 0;
    int done[96], n = 0; /* W188: 64 -> 96 (Overlay schreibt 63 Keys) */
    int i;
    if (nk > 96) { nk = 96; }
    memset(done, 0, sizeof done);
    if (!path || !path[0] || nk <= 0) { return -1; }
    if (fopen_s(&f, path, "r") == 0 && f) {
        fseek(f, 0, SEEK_END);
        sz = ftell(f);
        fseek(f, 0, SEEK_SET);
    }
    if (sz < 0 || sz > 1024 * 1024) {
        if (f) { fclose(f); }
        return -1;
    }
    cap = (size_t)sz + 8192;
    txt = (char *)malloc(cap);
    if (!txt) {
        if (f) { fclose(f); }
        return -1;
    }
    txt[0] = '\0';
    if (!f) {
        len += (size_t)_snprintf_s(txt + len, cap - len, _TRUNCATE,
                                   "# user.cfg - Spieler-Einstellungen (Launcher/Overlay), ueberschreibt sb_native.cfg\n");
    } else {
        while (fgets(line, sizeof line, f)) {
            char *nl = line + strcspn(line, "\r\n");
            int rep = 0;
            *nl = '\0';
            for (i = 0; i < nk; i++) {
                if (!done[i] && ov_line_is_key(line, keys[i])) {
                    len += (size_t)_snprintf_s(txt + len, cap - len, _TRUNCATE, "%s=%s\n", keys[i], vals[i]);
                    done[i] = 1;
                    rep = 1;
                    n++;
                    break;
                }
            }
            if (!rep) { len += (size_t)_snprintf_s(txt + len, cap - len, _TRUNCATE, "%s\n", line); }
        }
        fclose(f);
        f = NULL;
    }
    for (i = 0; i < nk; i++) {
        char hdr[48];
        if (done[i]) { continue; }
        _snprintf_s(hdr, sizeof hdr, _TRUNCATE, "[%s]", secs[i]);
        if (!strstr(txt, hdr)) { len += (size_t)_snprintf_s(txt + len, cap - len, _TRUNCATE, "%s\n", hdr); }
        len += (size_t)_snprintf_s(txt + len, cap - len, _TRUNCATE, "%s=%s\n", keys[i], vals[i]);
        n++;
    }
    if (fopen_s(&f, path, "w") != 0 || !f) {
        free(txt);
        return -1;
    }
    fwrite(txt, 1, len, f);
    fclose(f);
    free(txt);
    return n;
}

static int ov_save(void)
{
    const char *keys[OV_NKEYS], *secs[OV_NKEYS], *vals[OV_NKEYS];
    char vbuf[OV_NKEYS][64];
    size_t i;
    for (i = 0; i < OV_NKEYS; i++) {
        keys[i] = kOvKeys[i].key;
        secs[i] = kOvKeys[i].sec;
        ov_key_value(kOvKeys[i].key, vbuf[i], sizeof vbuf[i]);
        vals[i] = vbuf[i];
    }
    return sb_ovl_cfg_write(g_ov_save_path, (int)OV_NKEYS, keys, secs, vals);
}

/* ------------------------------------------------------------------ */
/* Tasten (Render-Thread)                                              */
/* ------------------------------------------------------------------ */
/* W291: Gruppe direkt waehlen (Seitenleiste, Test-Hook). tabrow=1: Fokus bleibt in der Seitenleiste. */
static void ov_page_set(int pg, int tabrow)
{
    if (pg < 0 || pg >= OG_N || !ov_grp_vis(pg)) { pg = OG_QUICK; }
    g_ov_page = pg;
    g_ov_sel = tabrow ? -1 : 0;
    g_ov_dirty = 1;
    HLOG_I("OVERLAY: Seite -> %s (f=%u)", kOvGrp[g_ov_page].log, g_present_no);
}

/* W291: naechste/vorige sichtbare Gruppe (TAB, LB/RB, Seitenleiste hoch/runter) */
static void ov_page_step(int dir, int tabrow)
{
    int g = g_ov_page, k;
    for (k = 0; k < OG_N; k++) {
        g = (g + dir + OG_N) % OG_N;
        if (ov_grp_vis(g)) { break; }
    }
    ov_page_set(g, tabrow);
}

/* W291: n-te sichtbare Gruppe (Seitenleiste von oben), -1 = keine */
static int ov_grp_at(int n)
{
    int g;
    for (g = 0; g < OG_N; g++) {
        if (ov_grp_vis(g) && n-- == 0) { return g; }
    }
    return -1;
}

/* W188: Bildschirm wechseln (Render-Thread) */
/* W290 QUICK-SHOT: Aufnahme-Einstellungen des Fotomodus (eine Konfiguration mit dem Schnell-Screenshot) beim
 * Verlassen in user.cfg merken, wenn sie sich geaendert haben (Verfahren wie "Als Standard speichern"). */
static int g_ph_cfg0[5];
static void ov_ph_cfg_snap(int *v)
{
    v[0] = g_cfg.photo_view;
    v[1] = sb_photo_size_eff();
    v[2] = g_cfg.photo_jpg;
    v[3] = g_cfg.photo_aspect;
    v[4] = g_cfg.photo_hud;
}

static void ov_ph_persist(void)
{
    static const char *const keys[5] = { "photo_view", "photo_size", "photo_format", "photo_aspect", "photo_hud" };
    static const char *const secs[5] = { "photo", "photo", "photo", "photo", "photo" };
    char b0[8], b1[8], b3[8];
    const char *vals[5];
    int now[5], n;
    ov_ph_cfg_snap(now);
    if (memcmp(now, g_ph_cfg0, sizeof now) == 0) { return; }
    memcpy(g_ph_cfg0, now, sizeof now);
    if (!g_ov_save_path[0]) {
        HLOG_I("PHOTO: Einstellungen geaendert, kein Speicherpfad (user.cfg) - gelten bis zum Ende");
        return;
    }
    _snprintf_s(b0, sizeof b0, _TRUNCATE, "%d", now[0]);
    if (now[1] < 0) { _snprintf_s(b1, sizeof b1, _TRUNCATE, "%dk", -now[1]); }
    else { _snprintf_s(b1, sizeof b1, _TRUNCATE, "%d", now[1]); }
    _snprintf_s(b3, sizeof b3, _TRUNCATE, "%d", now[3]);
    vals[0] = b0;
    vals[1] = b1;
    vals[2] = now[2] ? "jpg" : "png";
    vals[3] = b3;
    vals[4] = now[4] ? "1" : "0";
    n = sb_ovl_cfg_write(g_ov_save_path, 5, keys, secs, vals);
    if (n >= 0) { HLOG_I("PHOTO: Einstellungen gemerkt (photo_view=%s photo_size=%s photo_format=%s photo_aspect=%s photo_hud=%s)", b0, b1, vals[2], b3, vals[4]); }
    else { HLOG_W("PHOTO: Einstellungen merken fehlgeschlagen"); }
}

static void ov_screen(int sc, int sel)
{
    static const char *const nm[] = { "Einstellungen", "Spielmenue", "Beenden?", "Pause", "Feedback-Art",
                                      "Feedback-Text", "Feedback-Paket", "Fotomodus", "Alpha-Hinweis" };
    if (g_ov_scr != sc) {
        HLOG_I("MENU: Bildschirm -> %s (f=%u)", nm[sc], g_present_no);
        if (g_ov_scr == SC_PHOTO) { sb_photo_leave(); ov_ph_persist(); InterlockedExchange(&g_ph_keys, 0); } /* W195, W290 */
    }
    if (sc == SC_PHOTO && (g_ov_scr != sc || !sb_photo_active())) { /* W195: auch beim Wiederoeffnen (Bildschirm blieb SC_PHOTO) */
        sb_photo_ui_t pu;
        ov_ph_cfg_snap(g_ph_cfg0); /* W290 */
        sb_photo_enter();
        sb_photo_get(&pu);
        g_ph_tab = 0;
        g_ph_sel = 0;
        g_ph_last_shown = (int)pu.nshot; /* keine alte Statuszeile aus der vorigen Sitzung */
        g_ph_t = 0;
    }
    if (sc != SC_WELCOME) { InterlockedExchange(&g_ov_welcome_on, 0); } /* W222 */
    g_ov_scr = sc;
    g_ov_msel = sel;
    g_ov_status[0] = 0; /* Statuszeile gehoert zum Bildschirm */
    g_ov_status_until = 0;
    if (sc == SC_SET) {
        g_ov_sel = 0;
    }
    InterlockedExchange(&g_ov_textmode, sc == SC_FBTEXT);
    g_ov_dirty = 1;
}

/* W193 PAUSE-LIVE: "Menue kurz ausblenden" - gehaltene Taste (menu_peek_key) blendet das
 * Panel aus, das Menue bleibt offen (Eingabesperre, Pause). UI-Thread setzt, Zeichnen liest. */
static volatile LONG g_ov_peek;
static int g_ov_peek_vk = -2; /* -2 = noch nicht gelesen, 0 = aus */
static int ov_is_pause_vk(int vk);
static int ov_peek_vk(void)
{
    if (g_ov_peek_vk == -2) {
        const char *k = g_cfg.menu_peek_key;
        int vk = (!k[0] || _stricmp(k, "OFF") == 0 || strcmp(k, "0") == 0) ? 0 : imap_key_name(k);
        if (vk < 0) {
            HLOG_W("MENU: menu_peek_key '%s' unbekannt - Ausblenden aus", k);
            vk = 0;
        }
        if (vk == VK_ESCAPE || vk == VK_RETURN || vk == VK_SPACE || vk == VK_TAB || vk == VK_UP || vk == VK_DOWN ||
            vk == VK_LEFT || vk == VK_RIGHT || vk == VK_BACK || vk == 'B' || ov_is_pause_vk(vk)) {
            HLOG_W("MENU: menu_peek_key '%s' ist eine Menue-Taste - Ausblenden aus", k);
            vk = 0;
        }
        g_ov_peek_vk = vk;
    }
    return g_ov_peek_vk;
}

int sb_ovl_peek(void)
{
    return g_ov_peek != 0;
}

static void ov_close(const char *why)
{
    HLOG_I("MENU: zu (%s, f=%u)", why, g_present_no);
    if (g_ov_scr == SC_PHOTO) { sb_photo_leave(); ov_ph_persist(); InterlockedExchange(&g_ph_keys, 0); InterlockedExchange(&g_ph_drag, 0); } /* W195, W290 */
    InterlockedExchange(&g_ov_textmode, 0);
    InterlockedExchange(&g_ov_peek, 0); /* W193 */
    InterlockedExchange(&g_ov_welcome_on, 0); /* W222 */
    g_ov_notice_on = 0; /* W254 */
    g_ov_undo = 0; /* W291: "Rueckgaengig" gilt nur, solange das Menue offen ist */
    InterlockedExchange(&g_ov_open, 0);
}

/* W291: Zeile k waehlbar? (Info-Zeilen ueberspringt die Auswahl) */
static int ov_sel_ok(int k)
{
    return k >= 0 && k < ov_nrows() && !ov_row_info(ov_row(k));
}

/* W291: Speichern (wie bisher OI_SAVE) */
static void ov_do_save(void)
{
    int ns = ov_save();
    if (ns >= 0) {
        HLOG_I("OVERLAY: gespeichert -> %s (%d Keys, f=%u)", g_ov_save_path, ns, g_present_no);
        ov_log_values("gespeichert");
        ov_log_shader("gespeichert");
        ov_log_shader2("gespeichert");
        ov_log_fx("gespeichert");
        ov_status_key("ovl.st_saved");
    } else {
        HLOG_W("OVERLAY: Speichern nach %s fehlgeschlagen", g_ov_save_path);
        ov_status_key("ovl.st_savefail");
    }
}

/* W291: zurueck ins Spielmenue (game_menu=0: zu wie W118) */
static void ov_set_back(const char *why)
{
    if (g_cfg.game_menu) { ov_screen(SC_MENU, MI_SETTINGS); }
    else { ov_close(why); }
}

/* Einstellungen (W291: Seitenleiste + Gruppen). g_ov_sel = -1: Fokus in der Seitenleiste. */
static void ov_key_set(int vk)
{
    int n = ov_nrows(), k;
    switch (vk) {
    case VK_UP:
        if (g_ov_sel < 0) { ov_page_step(-1, 1); break; }
        for (k = g_ov_sel - 1; k >= 0 && !ov_sel_ok(k); k--) {}
        g_ov_sel = k; /* ueber der ersten Zeile: Seitenleiste */
        g_ov_dirty = 1;
        break;
    case VK_DOWN:
        if (g_ov_sel < 0) { ov_page_step(+1, 1); break; }
        for (k = g_ov_sel + 1; k < n && !ov_sel_ok(k); k++) {}
        if (k >= n) { for (k = 0; k < n && !ov_sel_ok(k); k++) {} } /* Umlauf in der Liste */
        g_ov_sel = k < n ? k : 0;
        g_ov_dirty = 1;
        break;
    case VK_LEFT:
        if (g_ov_sel >= 0) { ov_change(ov_row(g_ov_sel), -1); }
        break;
    case VK_RIGHT:
        if (g_ov_sel < 0) { g_ov_sel = 0; g_ov_dirty = 1; } /* Seitenleiste -> Liste */
        else { ov_change(ov_row(g_ov_sel), +1); }
        break;
    case VK_TAB: /* W291: naechste sichtbare Gruppe */
        ov_page_step(+1, g_ov_sel < 0);
        break;
    case OK_BTAB: /* W188: Umschalt+TAB / Pad LB */
        ov_page_step(-1, g_ov_sel < 0);
        break;
    case VK_ESCAPE:
        ov_set_back("ESC");
        break;
    case VK_RETURN:
    case VK_SPACE: {
        int it = g_ov_sel < 0 ? -1 : ov_row(g_ov_sel);
        if (it < 0) { /* Seitenleiste: in die Liste */
            g_ov_sel = 0;
            g_ov_dirty = 1;
        } else if (it == OI_SAVE) {
            ov_do_save();
        } else if (it == OI_CLOSE) {
            ov_set_back("Schliessen");
        } else if (it == OI_RESET) { /* W291: Auf Original / Zurueck zum Original / Rueckgaengig / Alles aus */
            if (g_ov_page == OG_QUICK && g_ov_undo) { ov_undo(); }
            else { ov_reset(g_ov_page); }
            g_ov_dirty = 1;
        } else if (it == OI_ADV) { /* W291: auf-/zuklappen */
            ov_change(OI_ADV, g_ov_adv[g_ov_page] ? -1 : +1);
        } else if (it == OI_SLCOMMIT) { /* W190: bewusste Aktion, zweites ENTER bestaetigt */
            ov_sl_commit();
        } else {
            ov_change(it, +1);
        }
        break;
    }
    case 'B': /* W291: Vergleich (B) nur mit sichtbarer Debug-Gruppe */
        if (ov_grp_vis(OG_DEBUG)) {
            ov_change(OI_ABCMP, +1);
            g_ov_dirty = 1;
        }
        break;
    default:
        break;
    }
}

static int ov_ph_nrows(void); /* W195 */
/* W188: Zeilenzahl der Menue-Bildschirme */
static int ov_scr_rows(void)
{
    switch (g_ov_scr) {
    case SC_MENU: return MI_N;
    case SC_QUIT: return 2;
    case SC_PAUSED: return 2;
    case SC_FBKIND: return 3;
    case SC_FBTEXT: return 1;
    case SC_FBREVIEW: return 5;
    case SC_PHOTO: return ov_ph_nrows(); /* W195 */
    case SC_WELCOME: return 1; /* W222: "Weiter" */
    default: return ov_nrows();
    }
}

/* W188: Feedback-Paket anlegen (Render-Thread in der Pause: Spielbild liegt als Kopie vor) */
static void ov_fb_shot(void)
{
    char shot[700];
    _snprintf_s(shot, sizeof shot, _TRUNCATE, "%s\\screenshot.jpg", g_fb_pkg.dir);
    sb_ctx_enter();
    g_fb_pkg.shot_ok = sb_render_pause_jpg(shot, &g_fb_pkg.shot_w, &g_fb_pkg.shot_h);
    sb_ctx_leave();
}

static void ov_fb_create(void)
{
    g_fb_pending = 0;
    g_fb_text[g_fb_len] = 0;
    if (!g_fb_hot && !sb_feedback_begin(&g_fb_pkg, g_fb_kind)) {
        ov_status_key("ovl.st_fb_fail");
        return;
    }
    if (!g_fb_hot) { ov_fb_shot(); }
    g_fb_pkg.kind = g_fb_kind;
    if (!sb_feedback_finish(&g_fb_pkg, g_fb_text)) {
        ov_status_key("ovl.st_fb_fail");
        return;
    }
    if (g_cfg.overlay_test[0] && ov_dc) {
        static const wchar_t probe[] = L"\x00E4\x00F6\x00FC\x00DF\x00E9";
        WORD glyphs[5];
        unsigned i, have = 0;
        if (GetGlyphIndicesW(ov_dc, probe, 5, glyphs, GGI_MARK_NONEXISTING_GLYPHS) != GDI_ERROR) {
            for (i = 0; i < 5; ++i) { if (glyphs[i] != 0xFFFF) { ++have; } }
        }
        HLOG_I("FEEDBACK test: Unicode-Fenster=%d lateinische_Glyphen=%u/5 UTF16-Einheiten=%u",
               IsWindowUnicode(sb_render_hwnd()), have, (unsigned)g_fb_len);
    }
    g_fb_have = 1;
    ov_screen(SC_FBREVIEW, 0);
}

/* W188: Auswahl in einem Menue-Bildschirm ausloesen */
static void ov_activate(int k)
{
    switch (g_ov_scr) {
    case SC_MENU:
        switch (k) {
        case MI_CONTINUE: ov_close("Weiter"); break;
        case MI_SETTINGS: ov_screen(SC_SET, 0); break;
        case MI_PHOTO: /* W195 PHOTO-MODE */
            if (!g_cfg.photo_mode || g_cfg.vr_enable) {
                HLOG_I("MENU: Fotomodus nicht verfuegbar (%s, f=%u)", g_cfg.vr_enable ? "VR" : "photo_mode=0", g_present_no);
                ov_status_key(g_cfg.vr_enable ? "ovl.ph_st_vr" : "ovl.ph_st_off");
            } else {
                g_ph_from_menu = 1;
                ov_screen(SC_PHOTO, 0);
            }
            break;
        case MI_FEEDBACK: g_fb_hot = 0; ov_screen(SC_FBKIND, 0); break;
        case MI_QUIT: ov_screen(SC_QUIT, 0); break;
        default: break;
        }
        break;
    case SC_QUIT:
        if (k == 1) {
            HWND h = sb_render_hwnd();
            HLOG_I("MENU: Beenden bestaetigt (f=%u) -> Fenster schliessen", g_present_no);
            if (h) { PostMessageA(h, WM_CLOSE, 0, 0); } /* gleicher Weg wie das Fenster-X (sb_wndproc) */
        } else {
            ov_screen(SC_MENU, MI_QUIT);
        }
        break;
    case SC_PAUSED:
        if (k == 0) { ov_close("Weiter (Pause)"); }
        else { ov_screen(SC_MENU, MI_CONTINUE); }
        break;
    case SC_FBKIND:
        if (k == 2) { ov_screen(SC_MENU, MI_FEEDBACK); break; }
        g_fb_kind = k;
        g_fb_len = 0;
        g_fb_text[0] = 0;
        g_fb_high = 0;
        HLOG_I("FEEDBACK: Art %s (f=%u)", k ? "Idee" : "Bug", g_present_no);
        ov_screen(SC_FBTEXT, 0);
        break;
    case SC_FBTEXT:
        if (g_sb_paused) { ov_fb_create(); }
        else { g_fb_pending = 1; } /* Pause greift erst am Ende dieses Bildes */
        break;
    case SC_FBREVIEW:
        if (k == 0 || k == 1) {
            sb_feedback_open(&g_fb_pkg, sb_render_hwnd(), k == 0 ? 1 : 2, g_cfg.overlay_test[0] != 0);
            ov_status_key(k == 0 ? "ovl.st_fb_opened" : "ovl.st_fb_folder");
        } else if (k == 3) {
            sb_ctx_enter();
            if (sb_clip_start(g_fb_pkg.dir)) {
                g_fb_pkg.clip = 0;
                g_fb_pkg.clip_bytes = g_fb_pkg.clip_w = g_fb_pkg.clip_h = 0;
                ov_close("Clip aufnehmen");
                sb_pause_request(0, "Clip aufnehmen");
            } else { ov_status_key("ovl.clip_fail"); }
            sb_ctx_leave();
        } else if (k == 4) {
            sb_clip_remove(g_fb_pkg.dir);
            g_fb_pkg.clip = 0; g_fb_pkg.clip_bytes = g_fb_pkg.clip_w = g_fb_pkg.clip_h = 0;
            sb_feedback_finish(&g_fb_pkg, g_fb_text);
            HLOG_I("CLIP: removed");
        } else {
            ov_screen(SC_MENU, MI_FEEDBACK);
        }
        break;
    default:
        break;
    }
    g_ov_dirty = 1;
}

/* W188: Menue-Bildschirme (alles ausser Einstellungen) */
static void ov_key_menu(int vk)
{
    int n = ov_scr_rows();
    if (g_ov_scr == SC_FBTEXT) {
        if (vk & OK_CHAR) {
            wchar_t c = (wchar_t)(vk & 0xFFFF);
            if (c >= 0xD800 && c <= 0xDBFF) {
                g_fb_high = c; /* erst komplette Codepoints ins Textfeld */
            } else {
                if (c >= 0xDC00 && c <= 0xDFFF) {
                    if (g_fb_high && g_fb_len <= 158) {
                        g_fb_text[g_fb_len++] = g_fb_high;
                        g_fb_text[g_fb_len++] = c;
                    }
                } else if (c >= 32 && c != 127 && g_fb_len < 160) {
                    g_fb_text[g_fb_len++] = c;
                }
                g_fb_high = 0; /* isolierte Surrogate verwerfen */
                g_fb_text[g_fb_len] = 0;
            }
            g_ov_dirty = 1;
            return;
        }
        if (vk == VK_BACK) {
            g_fb_high = 0;
            if (g_fb_len > 0) {
                wchar_t last = g_fb_text[--g_fb_len];
                if (last >= 0xDC00 && last <= 0xDFFF && g_fb_len > 0) { --g_fb_len; }
                g_fb_text[g_fb_len] = 0;
            }
            g_ov_dirty = 1;
            return;
        }
    } else if (vk & OK_CHAR) {
        return;
    }
    switch (vk) {
    case VK_UP: case OK_WUP:
        g_ov_msel = (g_ov_msel + n - 1) % n;
        g_ov_dirty = 1;
        break;
    case VK_DOWN: case OK_WDN:
        g_ov_msel = (g_ov_msel + 1) % n;
        g_ov_dirty = 1;
        break;
    case VK_LEFT: case VK_RIGHT:
        if (g_ov_scr == SC_QUIT) { g_ov_msel = !g_ov_msel; g_ov_dirty = 1; }
        break;
    case VK_RETURN: case VK_SPACE:
        ov_activate(g_ov_msel);
        break;
    case VK_ESCAPE:
        switch (g_ov_scr) {
        case SC_MENU: ov_close("ESC"); break;
        case SC_QUIT: ov_screen(SC_MENU, MI_QUIT); break;
        case SC_PAUSED:
            if (g_cfg.game_menu) { ov_screen(SC_MENU, MI_CONTINUE); } else { ov_close("ESC (Pause)"); }
            break;
        case SC_FBKIND: ov_screen(SC_MENU, MI_FEEDBACK); break;
        case SC_FBTEXT: g_fb_pending = 0; ov_screen(SC_FBKIND, g_fb_kind); break;
        case SC_FBREVIEW: ov_screen(SC_MENU, MI_FEEDBACK); break;
        default: break;
        }
        break;
    case OK_PSTART: /* Pad START: im Menue/Pause = weiter, sonst zurueck */
        if (g_ov_scr == SC_MENU || g_ov_scr == SC_PAUSED) { ov_close("START"); }
        else { ov_key_menu(VK_ESCAPE); }
        break;
    case OK_PAUSE:
        if (g_ov_scr == SC_PAUSED) { ov_close("Pause-Taste"); }
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* W195 PHOTO-MODE: Foto-Panel (Bildschirm SC_PHOTO, Render-Thread)     */
/* ------------------------------------------------------------------ */
/* W290: PR_VIEW (vorher PR_COCKPIT) = Ansicht wie im Spiel / ohne Cockpit / nur Aussenwelt; PR_MECH und PR_SHADER
 * stehen nicht mehr im Panel (tote Zeile bzw. Grafik-Einstellung des ESC-Menues). */
enum { PR_VIEW, PR_FREECAM, PR_MECH, PR_RES, PR_FMT, PR_SHOT, PR_BACK, PR_YAW, PR_PITCH, PR_DIST, PR_RESET, PR_ASPECT, PR_MFX, PR_SHADER,
       PR_HUD /* W290: HUD im Foto an/aus (Monitor-Effekt bleibt) */ };
static const int kPhRows0[] = { PR_VIEW, PR_ASPECT, PR_MFX, PR_HUD, PR_FREECAM, PR_RES, PR_FMT, PR_SHOT, PR_BACK };
static const int kPhRows1[] = { PR_FREECAM, PR_YAW, PR_PITCH, PR_DIST, PR_RESET, PR_SHOT, PR_BACK };
#define PH_NR0 ((int)(sizeof kPhRows0 / sizeof kPhRows0[0]))
#define PH_NR1 ((int)(sizeof kPhRows1 / sizeof kPhRows1[0]))
#define PH_NTABS 2

static int ov_ph_nrows(void)
{
    return g_ph_tab ? PH_NR1 : PH_NR0;
}

static int ov_ph_row(int k)
{
    return g_ph_tab ? kPhRows1[k] : kPhRows0[k];
}

static int ov_ph_enabled(int it, const sb_photo_ui_t *u)
{
    switch (it) {
    case PR_FREECAM: return u->rec;
    case PR_MECH: return 0; /* Studie Stufe 3: nicht umgesetzt (BERICHT W195) */
    case PR_ASPECT: return u->rec && u->view == SB_PV_WORLD; /* W290: Bildformat nur bei "nur Aussenwelt" */
    case PR_MFX: return u->view != SB_PV_WORLD; /* W290: Aussenwelt hat keinen Monitor */
    case PR_HUD: return u->view != SB_PV_WORLD && u->rec; /* W290: HUD aus braucht die Aufzeichnung */
    case PR_YAW: case PR_PITCH: case PR_DIST: case PR_RESET: return u->freecam;
    default: return 1;
    }
}

static int ov_photo_allowed(void)
{
    return g_cfg.photo_mode && !g_cfg.vr_enable;
}

static void ov_ph_back(const char *why)
{
    if (g_ph_from_menu && g_cfg.game_menu) { ov_screen(SC_MENU, MI_PHOTO); }
    else { ov_close(why); }
}

static void ov_ph_shot(void)
{
    if (sb_photo_stepping()) { ov_status_key("ovl.ph_st_step"); return; } /* W290 */
    if (sb_photo_request()) { ov_status_key("ovl.ph_st_busy"); }
    else { ov_status_key("ovl.ph_st_vr"); }
}

static void ov_ph_tab_set(int t)
{
    g_ph_tab = (t + PH_NTABS) % PH_NTABS;
    if (g_ph_sel >= ov_ph_nrows()) { g_ph_sel = ov_ph_nrows() - 1; }
    HLOG_I("PHOTO: Seite -> %s (f=%u)", g_ph_tab ? "Kamera" : "Foto", g_present_no);
    g_ov_dirty = 1;
}

static void ov_ph_change(int it, int dir)
{
    sb_photo_ui_t u;
    sb_photo_get(&u);
    switch (it) {
    case PR_VIEW: { /* W290: wie im Spiel -> ohne Cockpit -> nur Aussenwelt (ohne Aufzeichnung nicht) */
        int v = (u.view + (dir < 0 ? 2 : 1)) % 3;
        if (v == SB_PV_WORLD && !u.rec) { v = dir < 0 ? SB_PV_FLAT : SB_PV_GAME; ov_status_key("ovl.ph_st_norec"); }
        sb_photo_set_view(v);
        if (sb_photo_stepping()) { ov_status_key("ovl.ph_st_step"); }
        break;
    }
    case PR_FREECAM:
        if (!u.rec) { ov_status_key("ovl.ph_st_norec"); break; }
        sb_photo_set_freecam(!u.freecam);
        break;
    case PR_MFX: /* W290: die Live-Einstellung Monitor-Effekt (gilt weiter wie im ESC-Menue) */
        if (u.view == SB_PV_WORLD) { ov_status_key("ovl.ph_st_mfx_world"); break; }
        sb_photo_set_effect(0, !u.monitor_fx);
        break;
    case PR_SHADER: sb_photo_set_effect(1, !u.shading); break;
    case PR_HUD: /* W290 */
        if (u.view == SB_PV_WORLD) { ov_status_key("ovl.ph_st_mfx_world"); break; }
        if (!u.rec) { ov_status_key("ovl.ph_st_norec"); break; }
        g_cfg.photo_hud = g_cfg.photo_hud ? 0 : 1;
        HLOG_I("PHOTO: HUD %s (f=%u)", g_cfg.photo_hud ? "an" : "aus", g_present_no);
        break;
    case PR_MECH: ov_status_key("ovl.ph_st_mech"); break;
    case PR_ASPECT: /* W290: 4:3 (wie Monitor) <-> breit (experimentell); "Ausschnitt" nur per cfg */
        if (!u.rec || u.view != SB_PV_WORLD) { ov_status_key("ovl.ph_st_aspect"); break; }
        sb_photo_set_aspect(u.aspect == 2 ? 1 : 2);
        break;
    case PR_RES: { /* W290: Fenster, 4K, 8K, x2, x4, x8 */
        int k, i = u.size;
        for (k = 0; k < SB_PHOTO_NSIZE; k++) {
            i = (i + (dir < 0 ? SB_PHOTO_NSIZE - 1 : 1)) % SB_PHOTO_NSIZE;
            if (u.savail[i]) { break; }
        }
        if (!u.savail[i]) { i = 0; }
        sb_photo_set_size(i);
        HLOG_I("PHOTO: Groesse -> %d (%ux%u, f=%u)", kSbPhotoSize[i], u.sw[i], u.sh[i], g_present_no);
        if (!u.rec) { ov_status_key("ovl.ph_st_norec"); }
        break;
    }
    case PR_FMT:
        sb_photo_set_jpg(!u.jpg);
        HLOG_I("PHOTO: Format -> %s (f=%u)", !u.jpg ? "JPG" : "PNG", g_present_no);
        break;
    case PR_YAW: if (u.freecam) { sb_photo_cam_move(-(float)dir * 5.0f, 0.0f, 0.0f, 0.0f, 0.0f); } break; /* rechts = +, Gieren positiv = links */
    case PR_PITCH: if (u.freecam) { sb_photo_cam_move(0.0f, (float)dir * 5.0f, 0.0f, 0.0f, 0.0f); } break;
    case PR_DIST: if (u.freecam) { sb_photo_cam_move(0.0f, 0.0f, (float)dir * 0.5f, 0.0f, 0.0f); } break;
    default: break;
    }
    g_ov_dirty = 1;
}

static void ov_ph_activate(int it)
{
    switch (it) {
    case PR_SHOT: ov_ph_shot(); break;
    case PR_BACK: ov_ph_back("Zurueck"); break;
    case PR_RESET: sb_photo_cam_reset(); break;
    default: ov_ph_change(it, +1); break;
    }
    g_ov_dirty = 1;
}

static void ov_key_photo(int vk)
{
    int n = ov_ph_nrows();
    if (vk & OK_CHAR) { return; }
    switch (vk) {
    case VK_UP: case OK_WUP: g_ph_sel = g_ph_sel < 0 ? n - 1 : g_ph_sel - 1; break;
    case VK_DOWN: case OK_WDN: g_ph_sel = g_ph_sel + 1 >= n ? -1 : g_ph_sel + 1; break;
    case VK_LEFT:
        if (g_ph_sel < 0) { ov_ph_tab_set(g_ph_tab - 1); } else { ov_ph_change(ov_ph_row(g_ph_sel), -1); }
        break;
    case VK_RIGHT:
        if (g_ph_sel < 0) { ov_ph_tab_set(g_ph_tab + 1); } else { ov_ph_change(ov_ph_row(g_ph_sel), +1); }
        break;
    case VK_TAB: ov_ph_tab_set(g_ph_tab + 1); break;
    case OK_BTAB: ov_ph_tab_set(g_ph_tab - 1); break;
    case VK_RETURN: case VK_SPACE:
        if (g_ph_sel < 0) { ov_ph_tab_set(g_ph_tab + 1); } else { ov_ph_activate(ov_ph_row(g_ph_sel)); }
        break;
    case VK_ESCAPE: ov_ph_back("ESC"); return;
    case OK_PSTART: ov_close("START"); return;
    case OK_PAUSE: ov_close("Pause-Taste"); return;
    case OK_PHOTO: case OK_PSHOT: ov_ph_shot(); break;
    default: break;
    }
    g_ov_dirty = 1;
}

/* Kamera-Schritt je Bild (Tasten, Maus-Ziehen, Pad) + Status der letzten Aufnahme */
static void ov_ph_tick(void)
{
    ULONGLONG now = GetTickCount64();
    float dt = g_ph_t ? (float)(now - g_ph_t) / 1000.0f : 0.0f;
    LONG k = g_ph_keys, mx = InterlockedExchange(&g_ph_mdx, 0), my = InterlockedExchange(&g_ph_mdy, 0);
    float yaw = 0.0f, pitch = 0.0f, f = 0.0f, r = 0.0f, up = 0.0f;
    sb_photo_ui_t u;
    g_ph_t = now;
    if (dt > 0.1f) { dt = 0.1f; }
    {
        int was = sb_photo_stepping();
        sb_photo_step_tick(); /* W290: Ansicht umschalten (Spiel laeuft kurz) */
        if (was && !sb_photo_stepping()) { g_ov_status[0] = 0; g_ov_dirty = 1; }
    }
    sb_photo_get(&u);
    { /* Zensus (Aufzeichnung ja/nein) kommt mit dem ersten Pausenbild: Panel dann neu zeichnen */
        static int last_rec = -1;
        if (u.rec != last_rec) { last_rec = u.rec; g_ov_dirty = 1; }
    }
    if (u.nshot != (unsigned)g_ph_last_shown) {
        wchar_t w[96];
        g_ph_last_shown = (int)u.nshot;
        if (u.last_ok > 0) {
            char nb[200];
            const char *sn = u.last_name; /* Anzeige ohne "Steelbound_JJJJ-MM-TT_" (Zeile bleibt kurz) */
            if (strncmp(sn, "Steelbound_", 11) == 0 && strlen(sn) > 22) { sn += 22; }
            _snprintf_s(nb, sizeof nb, _TRUNCATE, LT("ovl.ph_st_saved"), sn, u.last_w, u.last_h);
            ov_wu8(w, 96, nb);
            ov_status(w);
        } else {
            ov_status_key("ovl.ph_st_fail");
        }
    }
    if (!u.freecam) { return; }
    yaw = -(float)mx * 0.15f; /* nach rechts ziehen = nach rechts drehen (Gieren positiv = links) */
    pitch = -(float)my * 0.15f;
    if (k & PK_J) { yaw += 45.0f * dt; }
    if (k & PK_L) { yaw -= 45.0f * dt; }
    if (k & PK_I) { pitch += 45.0f * dt; }
    if (k & PK_K) { pitch -= 45.0f * dt; }
    if (k & PK_W) { f += 2.0f * dt; }
    if (k & PK_S) { f -= 2.0f * dt; }
    if (k & PK_D) { r += 2.0f * dt; }
    if (k & PK_A) { r -= 2.0f * dt; }
    if (k & PK_E) { up += 2.0f * dt; }
    if (k & PK_Q) { up -= 2.0f * dt; }
    { /* Pad: rechter Stick drehen, linker Stick fahren, Trigger hoch/runter (Totzone 25 %) */
        const int dz = 8192;
        if (g_ph_pad_rx > dz || g_ph_pad_rx < -dz) { yaw -= (float)g_ph_pad_rx / 32767.0f * 90.0f * dt; }
        if (g_ph_pad_ry > dz || g_ph_pad_ry < -dz) { pitch += (float)g_ph_pad_ry / 32767.0f * 60.0f * dt; }
        if (g_ph_pad_ly > dz || g_ph_pad_ly < -dz) { f += (float)g_ph_pad_ly / 32767.0f * 3.0f * dt; }
        if (g_ph_pad_lx > dz || g_ph_pad_lx < -dz) { r += (float)g_ph_pad_lx / 32767.0f * 3.0f * dt; }
        if (g_ph_pad_tr > 40 || g_ph_pad_tr < -40) { up += (float)g_ph_pad_tr / 255.0f * 2.0f * dt; }
    }
    if (yaw != 0.0f || pitch != 0.0f || f != 0.0f || r != 0.0f || up != 0.0f) {
        sb_photo_cam_move(yaw, pitch, f, r, up);
        g_ov_dirty = 1;
    }
}

/* ---- W291: Struktur-Selbsttest (Test-Hook CHECK) ---------------------------
 * Heimat-Schluessel je Zeile: die Zeile ist DIE Einstellung dieses Schluessels.
 * Sammelregler ohne eigene Heimat (ihre Einzelregler stehen unter Erweitert);
 * Monitor-Effekt und Texturen ersetzen ihre alten Zeilen und sind Heimat. */
static const char *ov_oi_home(int i)
{
    switch (i) {
    case OI_FPS: return "interp_fps";
    case OI_SHADING: return "shading";
    case OI_GRAIN: return "monitor_grain";
    case OI_GLITCH: return "monitor_glitch";
    case OI_EDGE: return "edge_mode";
    case OI_EDGEW: return "edge_fade_width";
    case OI_EDGES: return "edge_fade_strength";
    case OI_MMODE: return "mouse_mode";
    case OI_MSENS: return "mouse_sens";
    case OI_WINMODE: return "window_mode";
    case OI_FOG: return "world_fog";
    case OI_WCON: return "world_contrast";
    case OI_WGAM: return "world_gamma";
    case OI_WSHARP: return "world_sharpen";
    case OI_BLOOM: return "bloom_strength";
    case OI_CGLOW: return "cockpit_glow";
    case OI_CSPILL: return "cockpit_spill";
    case OI_CLIGHT: return "cockpit_light";
    case OI_EXPO: return "world_exposure";
    case OI_TONE: return "tonemap";
    case OI_BTHR: return "bloom_threshold";
    case OI_LWORLD: return "light_world";
    case OI_LCKP: return "light_cockpit";
    case OI_RTS: return "rt_scale,rt_scale_dyn";
    case OI_FOCUS: return "world_focus";
    case OI_SBCMOUSE: return "sbc_mouse_off";
    case OI_AA: return "aa_mode";
    case OI_SSAO: return "ssao";
    case OI_CSAT: return "color_sat";
    case OI_CTEMP: return "color_temp";
    case OI_CLIFT: return "color_lift";
    case OI_CGAM: return "color_gamma";
    case OI_CGAIN: return "color_gain";
    case OI_SSHADOW: return "shade_shadow,shadow_soft"; /* W291: einzige Schatten-Zeile */
    case OI_VQASYNC: return "vis_query_async";
    case OI_LABEL: return "hud_label_avoid";
    case OI_CPERSP: return "cockpit_persp";
    case OI_CPANINI: return "cockpit_panini";
    case OI_CWIDTH: return "cockpit_width";
    case OI_FXSOFT: return "fx_soft";
    case OI_FXEMB: return "fx_embers";
    case OI_FXLIGHT: return "fx_light";
    case OI_FXLCKP: return "fx_light_cockpit";
    case OI_FXIP: return "fx_interp";
    case OI_VRON: return "vr_enable";
    case OI_VRSEAT: return "vr_seat_height";
    case OI_VRSCALE: return "vr_world_scale";
    case OI_VRIPD: return "vr_ipd";
    case OI_VRPREV: return "vr_preview";
    case OI_VRFWD: return "vr_seat_forward";
    case OI_VRPRESET: return "vr_preset,vr_rt_scale";
    case OI_VRRES: return "vr_render_scale";
    case OI_VRHZ: return "vr_refresh_hz";
    case OI_VRMIRROR: return "vr_mirror,vr_mirror_fit,vr_mirror_res";
    case OI_VRWORLD: return "vr_world_tick";
    case OI_VRSHADE: return "vr_shading";
    case OI_MPAUSE: return "menu_pause";
    case OI_SAVELOCK: return "save_lock";
    case OI_ASSIST: return "aim_assist";
    case OI_AUTOGEAR: return "auto_gear";
    case OI_STICKAIM: return "stick_aim";
    case OI_STICKSENS: return "stick_aim_sens";
    case OI_VRSHELL: return "vr_shell";
    case OI_VRVIG: return "vr_vignette";
    case OI_XWIRE: return "fx_wireframe";
    case OI_XVAPOR: return "fx_vaporwave";
    case OI_CHIDE: return "cockpit_hide";
    case OI_MONVIEW: return "monitor_view"; /* W319 */
    case OI_SMON: return "monitor_fx";
    case OI_RPLABEL: return "replay_label";
    case OI_RPLOGO: return "replay_logo";
    case OI_WMMEDIA: return "watermark_media";
    case OI_VOLM: return "volume_master"; /* W303 */
    case OI_VOLMU: return "volume_music";
    case OI_VOLSFX: return "volume_sfx";
    case OI_VOLVOX: return "volume_voice";
    case OI_RPFX: return "replay_fx_live"; /* W299 */
    case OI_STEX: return "remaster,hd_textures,hd_scale";
    default: return "";
    }
}

static int ov_list_has(const char *list, const char *key)
{
    size_t n = strlen(key);
    const char *p = list;
    while (*p) {
        if (strncmp(p, key, n) == 0 && (p[n] == ',' || p[n] == 0)) { return 1; }
        p = strchr(p, ',');
        if (!p) { break; }
        p++;
    }
    return 0;
}

static void ov_struct_check(void)
{
    static const char *const exempt[] = { "fs_interp_match", "aim_turn", SB_GP_REV_KEY }; /* setzt das Preset bzw. Altwert; W296 Dateistand */
    unsigned char seen[OI_N];
    int g, k, j, nfail = 0, nrows = 0, nkey = 0, page0 = g_ov_page, d0, d1, dv, esc0 = g_cfg.esc_debug;
    int adv0[OG_N];
    memset(seen, 0, sizeof seen);
    for (g = 0; g < OG_N; g++) {
        const ov_grp_t *gp = &kOvGrp[g];
        for (k = 0; k < gp->nmain + gp->nadv; k++) {
            int i = k < gp->nmain ? gp->main[k] : gp->adv[k - gp->nmain];
            if (g == OG_QUICK) { continue; }
            if (seen[i]) { HLOG_W("OVERLAY check: Zeile %d doppelt (Gruppe %s)", i, gp->log); nfail++; }
            seen[i] = 1;
            nrows++;
        }
    }
    for (k = 0; k < kOvGrp[OG_QUICK].nmain; k++) { /* Schnell = Verknuepfungen (Preset hat dort seine Heimat) */
        int i = kOvGrp[OG_QUICK].main[k];
        if (i != OI_PRESET && !seen[i]) { HLOG_W("OVERLAY check: Schnell-Zeile %d ohne Heimat", i); nfail++; }
    }
    for (j = 0; j < (int)OV_NKEYS; j++) {
        const char *key = kOvKeys[j].key;
        int cnt = 0, x;
        for (x = 0; x < (int)(sizeof exempt / sizeof exempt[0]); x++) { if (strcmp(exempt[x], key) == 0) { cnt = -1; } }
        if (cnt < 0) { continue; }
        for (k = 0; k < OI_N; k++) { if (seen[k] && ov_list_has(ov_oi_home(k), key)) { cnt++; } }
        nkey++;
        if (cnt != 1) { HLOG_W("OVERLAY check: Schluessel %s hat %d Heimat-Zeilen (erwartet 1)", key, cnt); nfail++; }
    }
    for (g = 0; g < OG_N; g++) { adv0[g] = g_ov_adv[g]; }
    for (g = 0; g < OG_N; g++) { /* erreichbar: aufgeklappt steht jede verfuegbare Zeile in der Liste */
        const ov_grp_t *gp = &kOvGrp[g];
        int closed, open;
        g_ov_page = g;
        g_ov_adv[g] = 0;
        closed = ov_nrows();
        g_ov_adv[g] = 1;
        open = ov_nrows();
        if (g_ov_page != g) { continue; } /* unsichtbare Gruppe (VR/Debug) */
        for (k = 0; k < gp->nmain + gp->nadv; k++) {
            int i = k < gp->nmain ? gp->main[k] : gp->adv[k - gp->nmain], hit = 0, r;
            if (!ov_row_avail(i)) { continue; }
            for (r = 0; r < g_ov_nrv; r++) { hit |= g_ov_rv[r] == i; }
            if (!hit) { HLOG_W("OVERLAY check: Zeile %d in %s nicht erreichbar", i, gp->log); nfail++; }
        }
        if (ov_grp_nadv(g) && open != closed + ov_grp_nadv(g)) {
            HLOG_W("OVERLAY check: Erweitert in %s klappt nicht (%d -> %d)", gp->log, closed, open);
            nfail++;
        }
        HLOG_I("OVERLAY check: Gruppe %s sichtbar %d Zeilen, aufgeklappt %d", gp->log, closed, open);
    }
    for (g = 0; g < OG_N; g++) { g_ov_adv[g] = adv0[g]; }
    g_ov_page = page0;
    dv = ov_grp_vis(OG_DEBUG);
    g_cfg.esc_debug = 0;
    d0 = ov_grp_vis(OG_DEBUG);
    g_cfg.esc_debug = 1;
    d1 = ov_grp_vis(OG_DEBUG);
    g_cfg.esc_debug = esc0;
    if (!d1 || (d0 && !g_cfg.fx_wireframe && !g_cfg.fx_vaporwave)) { nfail++; }
    HLOG_I("OVERLAY check: Debug sichtbar=%d (esc_debug=%d; mit 0: %d, mit 1: %d), VR sichtbar=%d", dv, esc0, d0, d1,
           ov_grp_vis(OG_VR));
    HLOG_I("OVERLAY check: %s (%d Gruppen, %d Zeilen, %d Schluessel, %d Fehler)", nfail ? "FAIL" : "OK", OG_N, nrows, nkey,
           nfail);
    g_ov_dirty = 1;
}

/* W188: Mausklick/Bewegung (Backbuffer-Pixel) auf die zuletzt gezeichneten Flaechen */
static int ov_hit_row(int x, int y)
{
    if (x < g_hit_x0 || x >= g_hit_x0 + g_hit_w || y < g_hit_rows_y || g_hit_rowh <= 0) { return -2; }
    if ((y - g_hit_rows_y) / g_hit_rowh >= g_hit_nrows) { return -2; }
    return (y - g_hit_rows_y) / g_hit_rowh;
}

static void ov_mouse(int x, int y, int click)
{
    int r;
    if (g_ov_scr == SC_PHOTO) { /* W195: Tab-Zeile und Zeilen des Foto-Panels */
        if (y >= g_hit_tab_y0 && y < g_hit_tab_y1 && x >= g_hit_x0 && x < g_hit_x0 + g_hit_w) {
            int t;
            if (!click) { return; }
            for (t = 0; t < PH_NTABS; t++) {
                if (x >= g_hit_tab_x[t] && x < g_hit_tab_x[t + 1]) {
                    if (t != g_ph_tab) { ov_ph_tab_set(t); }
                    g_ph_sel = -1;
                    g_ov_dirty = 1;
                    break;
                }
            }
            return;
        }
        r = ov_hit_row(x, y);
        if (r < 0) { return; }
        if (g_ph_sel != r) { g_ph_sel = r; g_ov_dirty = 1; }
        if (click) {
            int it = ov_ph_row(r);
            HLOG_I("PHOTO: Klick Zeile %d (f=%u)", r, g_present_no);
            if (it == PR_SHOT || it == PR_BACK || it == PR_RESET || x < g_hit_valx) { ov_ph_activate(it); }
            else { ov_ph_change(it, x < g_hit_valx + (g_hit_x0 + g_hit_w - g_hit_valx) / 2 ? -1 : +1); }
        }
        return;
    }
    if (g_ov_scr == SC_SET) { /* W291: Kopf rechts = Zurueck, Seitenleiste = Gruppe, rechts davon die Zeilen */
        if (y >= g_hit_y0 && y < g_hit_head_y1 && x >= g_hit_x0 + g_hit_w / 2 && x < g_hit_x0 + g_hit_w) {
            if (click) { ov_set_back("Klick"); }
            return;
        }
        if (y >= g_hit_tab_y0 && y < g_hit_tab_y1 && x >= g_hit_tab_x[0] && x < g_hit_tab_x[1] && g_hit_side_h > 0) {
            int g;
            if (!click) { return; }
            g = ov_grp_at((y - g_hit_tab_y0) / g_hit_side_h);
            if (g >= 0) {
                if (g != g_ov_page) { ov_page_set(g, 1); }
                g_ov_sel = -1;
                g_ov_dirty = 1;
            }
            return;
        }
        if (x < g_hit_rows_x0) { return; }
        r = ov_hit_row(x, y);
        if (r < 0 || ov_row_info(ov_row(r))) { return; }
        if (g_ov_sel != r) { g_ov_sel = r; g_ov_dirty = 1; }
        if (click) {
            int it = ov_row(r);
            if (it == OI_SAVE || it == OI_CLOSE || it == OI_SLCOMMIT || it == OI_RESET || it == OI_ADV || x < g_hit_valx) {
                ov_key_set(VK_RETURN);
            } else {
                ov_change(it, x < g_hit_valx + (g_hit_x0 + g_hit_w - g_hit_valx) / 2 ? -1 : +1);
            }
        }
        return;
    }
    r = ov_hit_row(x, y);
    if (r < 0) { return; }
    if (g_ov_msel != r) { g_ov_msel = r; g_ov_dirty = 1; }
    if (click) {
        HLOG_I("MENU: Klick Zeile %d (f=%u)", r, g_present_no);
        ov_activate(r);
    }
}

static void ov_key(int vk)
{
    if (g_ov_scr == SC_WELCOME) { /* W222: beliebige Taste, Klick oder Pad-Taste schliesst den Alpha-Hinweis */
        if ((vk & OK_CHAR) && !(vk & OK_CLICK)) { return; } /* Klick-Code traegt x ab Bit 15 (kann Bit 17 setzen) */
        HLOG_I("WELCOME: geschlossen (%s 0x%X, f=%u)", (vk & OK_CLICK) ? "Klick" : "Taste", (vk & OK_CLICK) ? 0u : (unsigned)vk,
               g_present_no);
        ov_close("Alpha-Hinweis");
        return;
    }
    if (vk & OK_CLICK) {
        ov_mouse((vk >> 15) & 0x7FFF, vk & 0x7FFF, 1);
        return;
    }
    if (vk == OK_PHOTO && g_ov_scr != SC_PHOTO) { /* W195: Foto-Taste bei offenem Menue -> Fotomodus */
        if (!ov_photo_allowed()) { ov_status_key(g_cfg.vr_enable ? "ovl.ph_st_vr" : "ovl.ph_st_off"); return; }
        if (g_ov_scr == SC_FBTEXT) { return; }
        g_ph_from_menu = g_cfg.game_menu != 0;
        ov_screen(SC_PHOTO, 0);
        return;
    }
    if (g_ov_scr == SC_PHOTO) { /* W195 */
        ov_key_photo(vk);
        return;
    }
    if (g_ov_scr == SC_SET) {
        if (vk == OK_WUP) { vk = VK_UP; }
        else if (vk == OK_WDN) { vk = VK_DOWN; }
        else if (vk == OK_PSTART) { vk = VK_ESCAPE; }
        else if (vk == OK_PAUSE || (vk & OK_CHAR)) { return; }
        ov_key_set(vk);
    } else {
        ov_key_menu(vk);
    }
}

/* ------------------------------------------------------------------ */
/* Fenster-Nachrichten (UI-Thread)                                     */
/* ------------------------------------------------------------------ */
static void ov_qpush(int vk)
{
    LONG h = g_ov_qh;
    if (h - g_ov_qt >= 64) { return; } /* voll: Taste verwerfen */
    g_ov_q[h & 63] = vk;
    MemoryBarrier();
    InterlockedExchange(&g_ov_qh, h + 1);
}

/* W188: Pause-Tasten (Profil-Aktion "pause", hooks_input.c); leer -> Standard PAUSE */
void sb_ovl_set_pause_binding(const int *vks, int n, unsigned padmask)
{
    int i;
    for (i = 0; i < 4; i++) { InterlockedExchange(&g_ov_pause_vk[i], i < n ? vks[i] : 0); }
    if (n <= 0) { InterlockedExchange(&g_ov_pause_vk[0], VK_PAUSE); }
    InterlockedExchange(&g_ov_pause_pad, (LONG)padmask);
    HLOG_I("MENU: Pause-Belegung: Tasten %s0x%02X 0x%02X 0x%02X 0x%02X, Pad-Maske 0x%04X", n <= 0 ? "(Standard) " : "",
           (unsigned)g_ov_pause_vk[0], (unsigned)g_ov_pause_vk[1], (unsigned)g_ov_pause_vk[2], (unsigned)g_ov_pause_vk[3],
           padmask);
}

/* W195 PHOTO-MODE: Foto-Tasten (Profil-Aktion "photo", hooks_input.c); leer -> Standard Shift+F12 (W290).
 * W290: mit Zusatztasten (mods wie imap_bind_key_combos; NULL = keine). */
void sb_ovl_set_photo_binding(const int *vks, const int *mods, int n, unsigned padmask)
{
    int i;
    for (i = 0; i < 4; i++) {
        InterlockedExchange(&g_ov_photo_vk[i], i < n ? vks[i] : 0);
        InterlockedExchange(&g_ov_photo_mod[i], i < n && mods ? mods[i] : 0);
    }
    if (n <= 0) { InterlockedExchange(&g_ov_photo_vk[0], VK_F12); InterlockedExchange(&g_ov_photo_mod[0], 1); }
    InterlockedExchange(&g_ov_photo_pad, (LONG)padmask);
    HLOG_I("MENU: Foto-Belegung: Tasten %s0x%02X 0x%02X 0x%02X 0x%02X, Pad-Maske 0x%04X", n <= 0 ? "(Standard) " : "",
           (unsigned)g_ov_photo_vk[0], (unsigned)g_ov_photo_vk[1], (unsigned)g_ov_photo_vk[2], (unsigned)g_ov_photo_vk[3],
           padmask);
}

/* W290 QUICK-SHOT: Tasten der Aktion "quick_shot" (hooks_input.c). n = 0: Taste aus (Profilzeile leer). */
void sb_ovl_set_qshot_binding(const int *vks, const int *mods, int n, unsigned padmask)
{
    int i;
    for (i = 0; i < 4; i++) {
        InterlockedExchange(&g_ov_qs_vk[i], i < n ? vks[i] : 0);
        InterlockedExchange(&g_ov_qs_mod[i], i < n && mods ? mods[i] : 0);
    }
    InterlockedExchange(&g_ov_qs_pad, (LONG)padmask);
    HLOG_I("MENU: Schnell-Screenshot-Belegung: Tasten 0x%02X/%u 0x%02X/%u, Pad-Maske 0x%04X", (unsigned)g_ov_qs_vk[0],
           (unsigned)g_ov_qs_mod[0], (unsigned)g_ov_qs_vk[1], (unsigned)g_ov_qs_mod[1], padmask);
}

/* W290: Schnell-Screenshot anfordern (Fenster, Pad-Flanke im Hook, Test-Hook). Laufende Aufnahme: ignoriert. */
void sb_ovl_request_qshot(const char *why)
{
    if (!g_cfg.overlay || !g_cfg.photo_mode) { return; }
    if (g_qs_state != QS_IDLE || g_qs_req) { HLOG_I("QUICKSHOT: Druck ignoriert (Aufnahme laeuft)"); return; }
    InterlockedExchange(&g_qs_req, 1);
    HLOG_I("QUICKSHOT: angefordert (%s, f=%u)", why ? why : "", g_present_no);
}

/* W290: Zusatztasten jetzt (Bit 0 Shift, 1 Strg, 2 Alt) */
static int ov_mods_now(void)
{
    return ((GetKeyState(VK_SHIFT) & 0x8000) ? 1 : 0) | ((GetKeyState(VK_CONTROL) & 0x8000) ? 2 : 0) |
           ((GetKeyState(VK_MENU) & 0x8000) ? 4 : 0);
}

/* W290: Tastenname "Shift+F12" der ersten Belegung (leer = keine F-/Buchstabentaste) */
static void ov_key_label(const volatile LONG *vk, const volatile LONG *mod, wchar_t *out, int cap)
{
    wchar_t k[16];
    int v = (int)vk[0], m = (int)mod[0];
    out[0] = 0;
    if (v >= VK_F1 && v <= VK_F24) { _snwprintf_s(k, 16, _TRUNCATE, L"F%d", v - VK_F1 + 1); }
    else if ((v >= 'A' && v <= 'Z') || (v >= '0' && v <= '9')) { _snwprintf_s(k, 16, _TRUNCATE, L"%c", (wchar_t)v); }
    else if (v == VK_SNAPSHOT) { wcscpy_s(k, 16, L"PrtSc"); }
    else { return; }
    _snwprintf_s(out, cap, _TRUNCATE, L"%s%s%s%s", (m & 2) ? L"Ctrl+" : L"", (m & 4) ? L"Alt+" : L"", (m & 1) ? L"Shift+" : L"", k);
}

/* W211: no fallback after loading a profile: empty bindings stay disabled. */
void sb_ovl_set_feedback_binding(int clip, const int *vks, int n)
{
    int i;
    for (i = 0; i < 4; ++i) { InterlockedExchange(&g_fb_vk[clip != 0][i], i < n ? vks[i] : 0); }
    InterlockedOr(&g_fb_bind_known, clip ? 2 : 1); /* W222: Alpha-Hinweis zeigt die echte Belegung */
    g_ov_dirty = 1;
}

void sb_ovl_request_feedback(int clip)
{
    if (!g_cfg.overlay || g_ov_open || sb_clip_active()) { return; }
    if (InterlockedCompareExchange(&g_fb_hot_request, clip ? 2 : 1, 0)) { return; }
    if (!clip) {
        InterlockedExchange(&g_ov_req, SC_FBKIND);
        InterlockedExchange(&g_ov_open, 1);
        sb_pause_request(1, "Feedback hotkey");
    }
}

/* W195: Foto-Aktion vom Spiel-Thread (Pad/Controller-Bindung, Flanke) */
void sb_ovl_request_photo(const char *why)
{
    if (!g_cfg.overlay || !g_cfg.photo_mode || g_cfg.vr_enable) { return; }
    if (g_ov_open) { /* Ring gehoert dem UI-Thread: ueber dessen Nachricht */
        HWND h = sb_render_hwnd();
        if (h) { PostMessageA(h, OV_MSG_TEST, (WPARAM)OK_PHOTO, 0); }
        return;
    }
    HLOG_I("MENU: Fotomodus angefordert (%s)", why ? why : "");
    g_ph_from_menu = 0;
    InterlockedExchange(&g_ov_req, SC_PHOTO);
    InterlockedExchange(&g_ov_open, 1);
}

static int ov_is_photo_vk(int vk)
{
    int i, m = ov_mods_now();
    for (i = 0; i < 4; i++) {
        if (g_ov_photo_vk[i] && g_ov_photo_vk[i] == vk && g_ov_photo_mod[i] == m) { return 1; } /* W290: Zusatztasten exakt */
    }
    return 0;
}

static int ov_is_qshot_vk(int vk) /* W290 */
{
    int i, m = ov_mods_now();
    for (i = 0; i < 4; i++) {
        if (g_ov_qs_vk[i] && g_ov_qs_vk[i] == vk && g_ov_qs_mod[i] == m) { return 1; }
    }
    return 0;
}

/* W195: Kamera-Taste -> Bit (Fotomodus, gehalten) */
static LONG ov_ph_keybit(int vk)
{
    switch (vk) {
    case 'W': return PK_W;
    case 'S': return PK_S;
    case 'A': return PK_A;
    case 'D': return PK_D;
    case 'Q': return PK_Q;
    case 'E': return PK_E;
    case 'J': return PK_J;
    case 'L': return PK_L;
    case 'I': return PK_I;
    case 'K': return PK_K;
    default: return 0;
    }
}

/* W188: Pause-Aktion vom Spiel-Thread (Pad/Controller-Bindung, Flanke) */
void sb_ovl_request_pause(const char *why)
{
    if (!g_cfg.overlay || g_ov_open) { return; }
    HLOG_I("MENU: Pause angefordert (%s)", why ? why : "");
    InterlockedExchange(&g_ov_req, SC_PAUSED);
    InterlockedExchange(&g_ov_open, 1);
}

static int ov_is_pause_vk(int vk)
{
    int i;
    for (i = 0; i < 4; i++) {
        if (g_ov_pause_vk[i] && g_ov_pause_vk[i] == vk) { return 1; }
    }
    return 0;
}

/* UI-Thread: Overlay mit Bildschirm sc oeffnen */
static void ov_open_ui(int sc)
{
    InterlockedExchange(&g_ov_swallow_k, 0);
    InterlockedExchange(&g_ov_swallow_m, 0);
    InterlockedExchange(&g_ov_req, sc);
    InterlockedExchange(&g_ov_open, 1);
    imouse_release_keys(); /* gehaltene Spiel-Tasten loesen */
    imouse_menu_capture(1); /* W188: Maus frei (Zeiger sichtbar) */
}

/* Client-Koordinaten (lParam) -> Backbuffer-Pixel */
static LONG ov_click_code(HWND w, LPARAM lp)
{
    RECT rc;
    int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
    if (GetClientRect(w, &rc) && rc.right > 0 && rc.bottom > 0) {
        x = (int)((long long)x * (long long)g_bb_w / rc.right);
        y = (int)((long long)y * (long long)g_bb_h / rc.bottom);
    }
    x = ov_clamp(x, 0, 0x7FFF);
    y = ov_clamp(y, 0, 0x7FFF);
    return OK_CLICK | ((LONG)x << 15) | (LONG)y;
}

int sb_ovl_wndproc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    if (!g_cfg.overlay) { return 0; }
    switch (msg) {
    case OV_MSG_TEST: /* TEST(W188): interne Taste (z. B. Umschalt+TAB, Mausrad) ueber den Ring */
        if (g_ov_open) { ov_qpush((int)wp); }
        return 1;
    case OV_MSG_CLOSED: /* W203: queued close may arrive after another open. */
        imouse_menu_capture(sb_ovl_open());
        return 1;
    case OV_MSG_OPENED: /* W222: wie ov_open_ui, aber vom Render-Thread angestossen */
        if (sb_ovl_open()) {
            imouse_release_keys();
            imouse_menu_capture(1);
        }
        return 1;
    case WM_KEYDOWN:
        if (g_ov_open && g_ov_welcome_on && wp != VK_F10) { /* W222: beliebige Taste schliesst den Alpha-Hinweis */
            if (!(lp & 0x40000000)) { ov_qpush(VK_RETURN); }
            return 1;
        }
        if (wp == VK_ESCAPE) {
            if (!(lp & 0x40000000)) { /* keine Autowiederholung */
                if (!g_ov_open) { ov_open_ui(g_cfg.game_menu ? SC_MENU : SC_SET); } /* W188: zuerst das Spielmenue */
                else { ov_qpush(VK_ESCAPE); } /* zurueck/zu entscheidet der Render-Thread je Bildschirm */
            }
            return 1;
        }
        if (!g_ov_textmode) {
            int c, i;
            for (c = 0; c < 2; ++c) for (i = 0; i < 4; ++i) {
                if (g_fb_vk[c][i] && wp == (WPARAM)g_fb_vk[c][i]) {
                    if (!(lp & 0x40000000)) {
                        sb_ovl_request_feedback(c);
                        if (g_ov_open) { imouse_release_keys(); imouse_menu_capture(1); }
                    }
                    return 1;
                }
            }
        }
        if (!g_ov_textmode && !g_ov_open && sb_ov_key((unsigned)wp, (lp & 0x40000000) != 0)) { return 1; } /* W278: Original <-> eigene */
        if (!g_ov_textmode && wp != VK_F10 && ov_is_pause_vk((int)wp)) { /* W188: Pause-Taste */
            if (!(lp & 0x40000000)) {
                if (!g_ov_open) { ov_open_ui(SC_PAUSED); }
                else { ov_qpush(OK_PAUSE); }
            }
            return 1;
        }
        if (!g_ov_textmode && wp != VK_F10 && ov_is_qshot_vk((int)wp) && g_cfg.photo_mode) { /* W290: Schnell-Screenshot */
            if (!(lp & 0x40000000)) {
                if (!g_ov_open) { sb_ovl_request_qshot("Taste"); }
                else { ov_qpush(OK_PSHOT); } /* im Fotomodus = Foto, in anderen Menues ohne Wirkung */
            }
            return 1;
        }
        if (!g_ov_textmode && wp != VK_F10 && ov_is_photo_vk((int)wp) && g_cfg.photo_mode) { /* W195: Foto-Taste */
            if (!(lp & 0x40000000)) {
                if (!g_ov_open) {
                    if (g_cfg.vr_enable) { HLOG_I("MENU: Foto-Taste im VR-Betrieb ohne Wirkung (Fotomodus aus)"); }
                    else { g_ph_from_menu = 0; ov_open_ui(SC_PHOTO); }
                } else {
                    ov_qpush(OK_PHOTO);
                }
            }
            return 1;
        }
        if (!g_ov_open || wp == VK_F10) { return 0; }
        if (!g_ov_textmode && ov_peek_vk() > 0 && (int)wp == ov_peek_vk()) { /* W193: Menue ausblenden, solange gehalten */
            if (!InterlockedExchange(&g_ov_peek, 1)) { HLOG_I("MENU: ausgeblendet (Taste gehalten, f=%u)", g_present_no); }
            return 1;
        }
        if (g_ov_textmode) { /* W188: Feedback-Text - Zeichen kommen als WM_CHAR */
            if (wp == VK_BACK || ((wp == VK_RETURN) && !(lp & 0x40000000))) { ov_qpush((int)wp); }
            return 1;
        }
        if (g_ov_scr == SC_PHOTO && ov_ph_keybit((int)wp)) { /* W195: Kamera-Tasten (gehalten) */
            InterlockedOr(&g_ph_keys, ov_ph_keybit((int)wp));
            return 1;
        }
        if (wp == VK_UP || wp == VK_DOWN || wp == VK_LEFT || wp == VK_RIGHT) {
            ov_qpush((int)wp); /* Autowiederholung erlaubt */
        } else if (wp == VK_TAB && !(lp & 0x40000000)) {
            ov_qpush((GetKeyState(VK_SHIFT) & 0x8000) ? OK_BTAB : VK_TAB); /* W188: Umschalt+TAB = Seite zurueck */
        } else if ((wp == VK_RETURN || wp == VK_SPACE || wp == 'B') && !(lp & 0x40000000)) {
            ov_qpush((int)wp);
        } else {
            InterlockedIncrement(&g_ov_swallow_k);
        }
        return 1;
    case WM_SYSKEYDOWN: /* W283: Alt+Taste als Vergleichstaste (sonst unveraendert durchgereicht) */
        if (!g_ov_textmode && !g_ov_open && sb_ov_key((unsigned)wp, (lp & 0x40000000) != 0)) { return 1; }
        if (!g_ov_textmode && !g_ov_open && g_cfg.photo_mode && ov_is_qshot_vk((int)wp)) { /* W290: Alt+Taste */
            if (!(lp & 0x40000000)) { sb_ovl_request_qshot("Taste"); }
            return 1;
        }
        if (!g_ov_textmode && !g_ov_open && g_cfg.photo_mode && !g_cfg.vr_enable && ov_is_photo_vk((int)wp)) {
            if (!(lp & 0x40000000)) { g_ph_from_menu = 0; ov_open_ui(SC_PHOTO); }
            return 1;
        }
        return 0;
    case WM_KEYUP:
        if (wp == VK_ESCAPE) { return 1; }
        /* W290: Druck-Taste meldet Windows nur als WM_KEYUP (falls belegt; Windows 11 oeffnet sonst das Snipping Tool) */
        if (wp == VK_SNAPSHOT && !g_ov_textmode && g_cfg.photo_mode && ov_is_qshot_vk(VK_SNAPSHOT)) {
            if (!g_ov_open) { sb_ovl_request_qshot("Taste"); } else { ov_qpush(OK_PSHOT); }
            return 1;
        }
        if (ov_ph_keybit((int)wp) && (g_ph_keys & ov_ph_keybit((int)wp))) { /* W195: Kamera-Taste los */
            InterlockedAnd(&g_ph_keys, ~ov_ph_keybit((int)wp));
            return g_ov_open ? 1 : 0;
        }
        if (g_ov_peek && ov_peek_vk() > 0 && (int)wp == ov_peek_vk()) { /* W193 */
            InterlockedExchange(&g_ov_peek, 0);
            HLOG_I("MENU: wieder eingeblendet (f=%u)", g_present_no);
            return g_ov_open ? 1 : 0;
        }
        if (!g_ov_open || wp == VK_F10) { return 0; }
        return 1;
    case WM_CHAR:
        if (g_ov_open && g_ov_textmode && wp >= 32 && wp != 127 && wp <= 0xFFFF) {
            ov_qpush(OK_CHAR | (int)wp); /* W192: Unicode-Fenster liefert UTF-16 */
        }
        return g_ov_open ? 1 : (wp == 0x1B);
    case WM_MOUSEMOVE: /* W188: Zeiger ueber den Zeilen waehlt (Render-Thread), Spiel-Eingabe bleibt gesperrt */
        if (g_ov_open && g_ph_drag) { /* W195: Ziehen im Fotomodus dreht die Kamera */
            LONG x = (short)LOWORD(lp), y = (short)HIWORD(lp);
            if (!(wp & MK_LBUTTON)) {
                InterlockedExchange(&g_ph_drag, 0);
            } else {
                InterlockedExchangeAdd(&g_ph_mdx, x - g_ph_lx);
                InterlockedExchangeAdd(&g_ph_mdy, y - g_ph_ly);
                g_ph_lx = x;
                g_ph_ly = y;
            }
            return 0;
        }
        if (g_ov_open) {
            LONG c = ov_click_code(w, lp);
            InterlockedExchange(&g_ov_mx, (c >> 15) & 0x7FFF);
            InterlockedExchange(&g_ov_my, c & 0x7FFF);
            InterlockedIncrement(&g_ov_mgen);
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (!g_ov_open) { return 0; }
        ov_qpush(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? OK_WUP : OK_WDN);
        return 1;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
        if (!g_ov_open) { return 0; }
        InterlockedIncrement(&g_ov_swallow_m);
        if (g_ov_scr == SC_PHOTO) { /* W195: Klick ausserhalb des Panels (oder Panel ausgeblendet) = Kamera ziehen */
            LONG c = ov_click_code(w, lp);
            int bx = (int)((c >> 15) & 0x7FFF), by = (int)(c & 0x7FFF);
            if (g_ov_peek || bx < g_hit_x0 || bx >= g_hit_x0 + g_hit_w || by < g_hit_y0 || by >= g_hit_y0 + g_hit_h) {
                g_ph_lx = (short)LOWORD(lp);
                g_ph_ly = (short)HIWORD(lp);
                InterlockedExchange(&g_ph_drag, 1);
                if (msg == WM_LBUTTONDOWN && !g_cfg.overlay_test[0]) { SetCapture(w); }
                return 1;
            }
        }
        ov_qpush((int)ov_click_code(w, lp)); /* W188: Klick waehlt/loest aus */
        return 1;
    case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
    case WM_RBUTTONDBLCLK: case WM_MBUTTONDBLCLK: case WM_XBUTTONDBLCLK:
        if (!g_ov_open) { return 0; }
        InterlockedIncrement(&g_ov_swallow_m);
        if (msg == WM_RBUTTONDOWN || g_ov_welcome_on) { ov_qpush(VK_ESCAPE); } /* W188: rechte Maustaste = zurueck; W222: jede Taste schliesst den Hinweis */
        return 1;
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_XBUTTONUP:
        if (msg == WM_LBUTTONUP && g_ph_drag) { /* W195 */
            InterlockedExchange(&g_ph_drag, 0);
            if (GetCapture() == w) { ReleaseCapture(); }
        }
        return g_ov_open ? 1 : 0;
    default:
        return 0;
    }
}

int sb_ovl_input_blocked(void)
{
    if (g_ov_open) {
        InterlockedIncrement(&g_ov_blocked_q);
        return 1;
    }
    return 0;
}

int sb_ovl_open(void)
{
    return g_ov_open != 0;
}

/* ------------------------------------------------------------------ */
/* Test-Hook overlay_test: "<bild>:<aktion>,..." (Bild = Present-Nr.,   */
/* mN = N Bilder nach Missionsbeginn, +N = N nach dem vorigen Schritt).  */
/* (mK.N = N Bilder nach Beginn der K-ten Mission, Modus 4).               */
/* Aktionen: ESC UP DOWN LEFT RIGHT ENTER SPACE B, einzelne Buchstaben/  */
/* Ziffern, VK<hex>, SHOT, FOCUS1/FOCUS0 (input_test_hook), LOG.        */
/* W261: ALTTAB0/ALTTAB1 (Nachrichtenfolge Alt-Tab weg/zurueck),       */
/* SIZEMIN/SIZERES (WM_SIZE), ICONIC/UNICONIC (nur window_hidden=1).   */
/* Tasten gehen per PostMessage an das EIGENE Fenster (fokusneutral).   */
/* ------------------------------------------------------------------ */
static void ov_test_parse(void)
{
    const char *p = g_cfg.overlay_test;
    g_ov_test_parsed = 1;
    while (*p && g_ov_nsteps < (int)(sizeof g_ov_steps / sizeof g_ov_steps[0])) {
        ov_step_t *s = &g_ov_steps[g_ov_nsteps];
        char act[32];
        int n = 0;
        memset(s, 0, sizeof *s);
        while (*p == ' ' || *p == ',') { p++; }
        if (!*p) { break; }
        if (*p == 'm' || *p == 'M') {
            s->rel = 1;
            p++;
            s->mi = 0;
            { /* mK.N = K-te Mission (1-basiert) */
                const char *q = p;
                unsigned k = (unsigned)strtoul(q, (char **)&q, 10);
                if (*q == '.' && k >= 1u && k <= 8u) { s->mi = (int)k - 1; p = q + 1; }
            }
        }
        else if (*p == '+') { s->rel = 2; p++; }
        else if (*p == 'b') { s->rel = 3; p++; } /* W192: Bink-Bildnummer */
        else if (*p == 'p') { s->rel = 4; p++; } /* W290: Bilder nach Beginn der Replay-Wiedergabe (REPLAY-AUTO PLAY) */
        s->at = (unsigned)strtoul(p, (char **)&p, 10);
        if (*p != ':') { HLOG_W("OVERLAY test: Syntaxfehler bei '%s'", p); break; }
        p++;
        while (*p && *p != ',' && n < 31) { act[n++] = *p++; }
        act[n] = '\0';
        s->act = OA_KEY;
        if (_stricmp(act, "SHOT") == 0) { s->act = OA_SHOT; }
        else if (_stricmp(act, "FOCUS1") == 0) { s->act = OA_FOCUS1; }
        else if (_stricmp(act, "FOCUS0") == 0) { s->act = OA_FOCUS0; }
        else if (_stricmp(act, "LOG") == 0) { s->act = OA_LOG; }
        else if (_stricmp(act, "PHUDPOS") == 0) { s->act = OA_PHUDPOS; } /* W122: Leistungs-Overlay Ecke (wie Shift+F2) */
        else if (_stricmp(act, "ESC") == 0) { s->vk = VK_ESCAPE; }
        else if (_stricmp(act, "UP") == 0) { s->vk = VK_UP; }
        else if (_stricmp(act, "DOWN") == 0) { s->vk = VK_DOWN; }
        else if (_stricmp(act, "LEFT") == 0) { s->vk = VK_LEFT; }
        else if (_stricmp(act, "RIGHT") == 0) { s->vk = VK_RIGHT; }
        else if (_stricmp(act, "ENTER") == 0) { s->vk = VK_RETURN; }
        else if (_stricmp(act, "SPACE") == 0) { s->vk = VK_SPACE; }
        else if (_stricmp(act, "LCLICK") == 0) { s->vk = VK_LBUTTON; }
        /* W188 GAME-MENU: Umschalt+TAB, Mausrad, Texteingabe, Mausklick auf Zeile/Tab, Pad-Tasten, Zustand */
        else if (_stricmp(act, "TAB") == 0) { s->vk = VK_TAB; }
        else if (_stricmp(act, "BTAB") == 0) { s->act = OA_TKEY; s->vk = OK_BTAB; }
        else if (_stricmp(act, "WHEELU") == 0) { s->act = OA_TKEY; s->vk = OK_WUP; }
        else if (_stricmp(act, "WHEELD") == 0) { s->act = OA_TKEY; s->vk = OK_WDN; }
        else if (_stricmp(act, "BACK") == 0) { s->vk = VK_BACK; }
        else if (_stricmp(act, "PAUSE") == 0) { s->vk = VK_PAUSE; }
        else if (_stricmp(act, "STATE") == 0) { s->act = OA_STATE; }
        else if (_stricmp(act, "CHECK") == 0) { s->act = OA_CHECK; } /* W291 */
        else if (_stricmp(act, "PEEKDN") == 0) { s->act = OA_PEEK; s->vk = 1; } /* W193 */
        else if (_stricmp(act, "PEEKUP") == 0) { s->act = OA_PEEK; s->vk = 0; }
        else if (_stricmp(act, "F11") == 0) { s->vk = VK_F11; }
        else if (_stricmp(act, "F9") == 0) { s->vk = VK_F9; }
        else if (_stricmp(act, "PHOTO") == 0) { s->act = OA_PHOTO; } /* W195 */
        else if (_stricmp(act, "QSHOT") == 0) { s->act = OA_QSHOT; s->vk = -1; } /* W290: Schnell-Screenshot (Taste der Belegung) */
        else if (_strnicmp(act, "QSHOT=", 6) == 0) { /* W290: QSHOT=<ansicht>.<groesse>, z. B. QSHOT=1.4k */
            const char *q = act + 6;
            s->act = OA_QSHOT;
            s->vk = (int)strtol(q, (char **)&q, 10);
            s->txt[0] = 0;
            if (*q == '.') { strcpy_s(s->txt, sizeof s->txt, q + 1); }
        }
        /* W261 FILM-ALTTAB: echte Nachrichtenfolge eines Alt-Tab an das EIGENE Fenster (kein Fokuswechsel) */
        else if (_stricmp(act, "ALTTAB0") == 0) { s->act = OA_WMSG; s->vk = 0; }
        else if (_stricmp(act, "ALTTAB1") == 0) { s->act = OA_WMSG; s->vk = 1; }
        else if (_stricmp(act, "SIZEMIN") == 0) { s->act = OA_WMSG; s->vk = 2; }
        else if (_stricmp(act, "SIZERES") == 0) { s->act = OA_WMSG; s->vk = 3; }
        else if (_stricmp(act, "ICONIC") == 0) { s->act = OA_WMSG; s->vk = 4; }
        else if (_stricmp(act, "UNICONIC") == 0) { s->act = OA_WMSG; s->vk = 5; }
        else if (_stricmp(act, "CLICKACT") == 0) { s->act = OA_WMSG; s->vk = 6; } /* W299: Fenster per Linksklick aktivieren (Fokus-Klick) */
        else if (_strnicmp(act, "KDN=", 4) == 0 && act[4]) { s->act = OA_KDN; s->vk = toupper((unsigned char)act[4]); }
        else if (_strnicmp(act, "KUP=", 4) == 0 && act[4]) { s->act = OA_KUP; s->vk = toupper((unsigned char)act[4]); }
        else if (_strnicmp(act, "MDRAG", 5) == 0 && act[5]) { s->act = OA_MDRAG; strcpy_s(s->txt, sizeof s->txt, act + 5); }
        else if (_strnicmp(act, "U+", 2) == 0) { s->act = OA_CHAR; s->vk = (int)strtoul(act + 2, NULL, 16); }
        else if (_strnicmp(act, "TXT=", 4) == 0) { s->act = OA_TXT; strcpy_s(s->txt, sizeof s->txt, act + 4); }
        else if (_strnicmp(act, "MROW", 4) == 0 && act[4]) { s->act = OA_MROW; s->vk = atoi(act + 4); }
        else if (_strnicmp(act, "MHOV", 4) == 0 && act[4]) { s->act = OA_MHOV; s->vk = atoi(act + 4); } /* W197 */
        else if (_strnicmp(act, "MTAB", 4) == 0 && act[4]) { s->act = OA_MTAB; s->vk = atoi(act + 4); }
        else if (_strnicmp(act, "PAD", 3) == 0 && act[3]) {
            static const struct { const char *n; unsigned b; } pb[] = {
                { "A", IMAP_PAD_A }, { "B", IMAP_PAD_B }, { "X", IMAP_PAD_X }, { "Y", IMAP_PAD_Y },
                { "UP", IMAP_PAD_DU }, { "DOWN", IMAP_PAD_DD }, { "LEFT", IMAP_PAD_DL }, { "RIGHT", IMAP_PAD_DR },
                { "LB", IMAP_PAD_LB }, { "RB", IMAP_PAD_RB }, { "START", IMAP_PAD_START }, { "BACK", IMAP_PAD_BACK } };
            size_t q;
            s->act = OA_PAD;
            for (q = 0; q < sizeof pb / sizeof pb[0]; q++) {
                if (_stricmp(act + 3, pb[q].n) == 0) { s->vk = (int)pb[q].b; }
            }
            if (!s->vk) { HLOG_W("OVERLAY test: unbekannte Pad-Taste '%s'", act); continue; }
        }
        else if (_strnicmp(act, "VK", 2) == 0 && act[2]) { s->vk = (int)strtoul(act + 2, NULL, 16); }
        else if (n == 1 && ((act[0] >= 'A' && act[0] <= 'Z') || (act[0] >= '0' && act[0] <= '9'))) { s->vk = act[0]; }
        else if (n == 1 && act[0] >= 'a' && act[0] <= 'z') { s->vk = act[0] - 32; }
        else { HLOG_W("OVERLAY test: unbekannte Aktion '%s'", act); continue; }
        g_ov_nsteps++;
    }
    HLOG_I("OVERLAY test: %d Schritte aus overlay_test", g_ov_nsteps);
}

static void ov_post_key(HWND h, int vk)
{
    if (vk == VK_LBUTTON) {
        PostMessageA(h, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(100, 100));
        PostMessageA(h, WM_LBUTTONUP, 0, MAKELPARAM(100, 100));
        return;
    }
    {
        LPARAM sc = (LPARAM)MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC) << 16;
        PostMessageA(h, WM_KEYDOWN, (WPARAM)vk, 1 | sc);
        PostMessageA(h, WM_KEYUP, (WPARAM)vk, (LPARAM)(1 | sc | 0xC0000000u));
    }
}

static unsigned g_ov_play_f; /* W290: Present-Nr. beim Beginn der Replay-Wiedergabe (Test-Anker pN) */
static void ov_test_tick(unsigned n)
{
    HWND h;
    if (!g_cfg.overlay_test[0]) { return; }
    if (!g_ov_test_parsed) { ov_test_parse(); }
    { /* W232: Mission = sb_in_mission (Szene 0x2C + mloop 1 ohne Titel-Demo;
       * rq_in_mission allein trifft auch die Demo, der Ladezaehler [0x2EEA34]
       * war nur in der ersten Mission der Testkette 4) */
        int in = sb_in_mission();
        if (in && !g_ov_in_mission && g_ov_nmission < 8) {
            g_ov_mission_f[g_ov_nmission++] = n;
            HLOG_I("OVERLAY test: Missionsbeginn #%d f=%u", g_ov_nmission, n);
        }
        g_ov_in_mission = in;
    }
    if (!g_ov_play_f && sb_rauto_phase(NULL) == RA_PLAY) { /* W290: Anker fuer pN */
        g_ov_play_f = n;
        HLOG_I("OVERLAY test: Replay-Wiedergabe ab f=%u", n);
    }
    h = sb_render_hwnd();
    while (g_ov_step < g_ov_nsteps && h) {
        ov_step_t *s = &g_ov_steps[g_ov_step];
        unsigned due;
        if (s->rel == 3) {
            if (sb_pause_movie_frame() < s->at) { return; }
            due = n;
        } else if (s->rel == 1) {
            if (s->mi >= g_ov_nmission) { return; }
            due = g_ov_mission_f[s->mi] + s->at;
        } else if (s->rel == 2) {
            due = g_ov_prev_f + s->at;
        } else if (s->rel == 4) { /* W290 */
            if (!g_ov_play_f) { return; }
            due = g_ov_play_f + s->at;
        } else {
            due = s->at;
        }
        if (n < due) { return; }
        g_ov_prev_f = n;
        g_ov_step++;
        switch (s->act) {
        case OA_SHOT: g_ov_shot_due = 1; break;
        case OA_FOCUS1: PostMessageA(h, IMOUSE_MSG_TEST_FOCUS, 1, 0); break;
        case OA_FOCUS0: PostMessageA(h, IMOUSE_MSG_TEST_FOCUS, 0, 0); break;
        case OA_LOG: sb_phud_test_log(); PostMessageA(h, IMOUSE_MSG_TEST_STATE, 0, 0); ov_log_values("test"); ov_log_shader("test"); ov_log_shader2("test"); ov_log_fx("test"); break;
        case OA_PHUDPOS: sb_phud_cycle_pos(); break;
        case OA_TKEY: PostMessageA(h, OV_MSG_TEST, (WPARAM)s->vk, 0); break; /* W188 */
        case OA_CHAR: PostMessageW(h, WM_CHAR, (WPARAM)s->vk, 1); break;
        case OA_TXT: { /* W192: Testtext ist UTF-8, WM_CHAR liefert UTF-16 */
            wchar_t text[32], *q;
            if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s->txt, -1, text, 32)) {
                for (q = text; *q; q++) { PostMessageW(h, WM_CHAR, *q == L'_' ? L' ' : *q, 1); }
            }
            break;
        }
        case OA_MROW: case OA_MTAB: case OA_MHOV: { /* W188: Klick auf die Mitte von Zeile/Tab (Backbuffer -> Client) */
            RECT rc;
            int bx, by;
            if (s->act == OA_MROW || s->act == OA_MHOV) {
                bx = g_ov_scr == SC_SET ? g_hit_rows_x0 + (g_hit_x0 + g_hit_w - g_hit_rows_x0) / 4 /* W291: rechts der Seitenleiste */
                                        : g_hit_x0 + g_hit_w / 3;
                by = g_hit_rows_y + s->vk * g_hit_rowh + g_hit_rowh / 2;
            } else {
                int t = ov_clamp(s->vk, 0, OV_NPAGES - 1);
                if (g_ov_scr == SC_SET) { /* W291: t-ter sichtbarer Eintrag der Seitenleiste */
                    bx = (g_hit_tab_x[0] + g_hit_tab_x[1]) / 2;
                    by = g_hit_tab_y0 + t * g_hit_side_h + g_hit_side_h / 2;
                } else {
                    bx = (g_hit_tab_x[t] + g_hit_tab_x[t + 1]) / 2;
                    by = (g_hit_tab_y0 + g_hit_tab_y1) / 2;
                }
            }
            if (GetClientRect(h, &rc) && g_bb_w && g_bb_h) {
                bx = (int)((long long)bx * rc.right / (long long)g_bb_w);
                by = (int)((long long)by * rc.bottom / (long long)g_bb_h);
            }
            PostMessageA(h, WM_MOUSEMOVE, 0, MAKELPARAM(bx, by));
            if (s->act == OA_MHOV) { break; } /* W197: nur zeigen (Zeile wird gewaehlt, kein Klick) */
            PostMessageA(h, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(bx, by));
            PostMessageA(h, WM_LBUTTONUP, 0, MAKELPARAM(bx, by));
            break;
        }
        case OA_PAD: g_pad_inject |= (unsigned)s->vk; break; /* W188: naechster Pad-Poll */
        case OA_PEEK: /* W193: Taste "Menue ausblenden" einzeln druecken bzw. loslassen (gehalten) */
            if (ov_peek_vk() > 0) {
                LPARAM sc = (LPARAM)MapVirtualKeyA((UINT)ov_peek_vk(), MAPVK_VK_TO_VSC) << 16;
                if (s->vk) { PostMessageA(h, WM_KEYDOWN, (WPARAM)ov_peek_vk(), 1 | sc); }
                else { PostMessageA(h, WM_KEYUP, (WPARAM)ov_peek_vk(), (LPARAM)(1 | sc | 0xC0000000u)); }
            }
            break;
        case OA_PHOTO: /* W195; W290: Belegung mit Zusatztaste (Shift+F12) ist per PostMessage nicht darstellbar */
            if (g_ov_photo_mod[0] == 0) { ov_post_key(h, g_ov_photo_vk[0] ? (int)g_ov_photo_vk[0] : VK_F12); }
            else { sb_ovl_request_photo("Test"); }
            break;
        case OA_QSHOT: /* W290 */
            if (s->vk < 0) { /* Taste der Belegung als PostMessage (wie ein echter Druck, ohne Zusatztaste) */
                if (g_ov_qs_vk[0] && g_ov_qs_mod[0] == 0) { ov_post_key(h, (int)g_ov_qs_vk[0]); }
                else { sb_ovl_request_qshot("Test"); }
            } else {
                g_cfg.photo_view = s->vk;
                if (s->txt[0]) {
                    char sz[16];
                    const char *dot = strchr(s->txt, '.');
                    strncpy_s(sz, sizeof sz, s->txt, dot ? (size_t)(dot - s->txt) : _TRUNCATE);
                    g_cfg.photo_size = (_stricmp(sz, "4k") == 0) ? -4 : (_stricmp(sz, "8k") == 0) ? -8 : atoi(sz);
                    if (dot) { g_cfg.photo_hud = atoi(dot + 1) ? 1 : 0; } /* QSHOT=<ansicht>.<groesse>.<hud> */
                }
                HLOG_I("OVERLAY test: Schnell-Screenshot mit Ansicht %d, Groesse %d, HUD %d", g_cfg.photo_view, g_cfg.photo_size,
                       g_cfg.photo_hud);
                sb_ovl_request_qshot("Test");
            }
            break;
        case OA_WMSG: { /* W261: Alt-Tab weg/zurueck bzw. Minimieren/Wiederherstellen nachstellen */
            RECT rc = { 0, 0, 0, 0 };
            GetClientRect(h, &rc);
            if (s->vk == 0) { /* Reihenfolge wie Windows beim Wegwechseln */
                PostMessageA(h, WM_ACTIVATE, WA_INACTIVE, 0);
                PostMessageA(h, WM_KILLFOCUS, 0, 0);
                PostMessageA(h, WM_ACTIVATEAPP, FALSE, 0);
            } else if (s->vk == 1) {
                PostMessageA(h, WM_ACTIVATEAPP, TRUE, 0);
                PostMessageA(h, WM_ACTIVATE, WA_ACTIVE, 0);
                PostMessageA(h, WM_SETFOCUS, 0, 0);
            } else if (s->vk == 2) {
                PostMessageA(h, WM_SIZE, SIZE_MINIMIZED, 0);
            } else if (s->vk == 3) {
                PostMessageA(h, WM_SIZE, SIZE_RESTORED, MAKELPARAM(rc.right, rc.bottom));
            } else if (s->vk == 6) { /* W299: Reihenfolge wie Windows bei Klick in das inaktive Fenster */
                PostMessageA(h, WM_MOUSEACTIVATE, (WPARAM)h, MAKELPARAM(HTCLIENT, WM_LBUTTONDOWN));
                PostMessageA(h, WM_ACTIVATEAPP, TRUE, 0);
                PostMessageA(h, WM_ACTIVATE, WA_CLICKACTIVE, 0);
                PostMessageA(h, WM_SETFOCUS, 0, 0);
                PostMessageA(h, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(rc.right / 2, rc.bottom / 2));
                PostMessageA(h, WM_LBUTTONUP, 0, MAKELPARAM(rc.right / 2, rc.bottom / 2));
            } else if (g_cfg.window_hidden) { /* echtes Minimieren nur am unsichtbaren Testfenster, nie aktivierend */
                ShowWindow(h, s->vk == 4 ? SW_SHOWMINNOACTIVE : SW_SHOWNOACTIVATE);
            } else {
                HLOG_W("OVERLAY test: ICONIC/UNICONIC nur mit window_hidden=1");
            }
            HLOG_I("OVERLAY test: Fenster-Folge %d (Client %ldx%ld, iconic=%d)", s->vk, rc.right, rc.bottom,
                   IsIconic(h) ? 1 : 0);
            break;
        }
        case OA_KDN: case OA_KUP: { /* W195: Taste einzeln druecken bzw. loslassen (gehalten) */
            LPARAM sc = (LPARAM)MapVirtualKeyA((UINT)s->vk, MAPVK_VK_TO_VSC) << 16;
            if (s->act == OA_KDN) { PostMessageA(h, WM_KEYDOWN, (WPARAM)s->vk, 1 | sc); }
            else { PostMessageA(h, WM_KEYUP, (WPARAM)s->vk, (LPARAM)(1 | sc | 0xC0000000u)); }
            break;
        }
        case OA_MDRAG: { /* W195: Linke Taste rechts in der 4:3-Box druecken, um dx_dy (Client-Pixel) ziehen, loslassen */
            RECT rc;
            unsigned abx, aby, abw, abh;
            int x0, y0, dx = 0, dy = 0, q;
            const char *pp = s->txt;
            dx = (int)strtol(pp, (char **)&pp, 10);
            if (*pp == '_') { dy = (int)strtol(pp + 1, NULL, 10); }
            sb_aspect_box(&abx, &aby, &abw, &abh);
            x0 = (int)(abx + abw * 3u / 4u);
            y0 = (int)(aby + abh / 2u);
            if (GetClientRect(h, &rc) && g_bb_w && g_bb_h) {
                x0 = (int)((long long)x0 * rc.right / (long long)g_bb_w);
                y0 = (int)((long long)y0 * rc.bottom / (long long)g_bb_h);
            }
            PostMessageA(h, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x0, y0));
            for (q = 1; q <= 4; q++) {
                PostMessageA(h, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x0 + dx * q / 4, y0 + dy * q / 4));
            }
            PostMessageA(h, WM_LBUTTONUP, 0, MAKELPARAM(x0 + dx, y0 + dy));
            break;
        }
        case OA_CHECK: ov_struct_check(); break; /* W291 */
        case OA_STATE: /* W188: Beleg-Zeile Spielzustand */
        {
            unsigned tmr = 0;
            __try { tmr = *(volatile unsigned *)(uintptr_t)SBA(0x3563F0u); }
            __except (EXCEPTION_EXECUTE_HANDLER) { tmr = 0; }
            sb_pause_movie_log("test");
            HLOG_I("MENU test: Zustand f=%u feld=%u tmr=%u pausiert=%d bildschirm=%d offen=%d", g_present_no,
                   sb_timing_field(), tmr, (int)g_sb_paused, g_ov_scr, (int)g_ov_open);
        }
            break;
        default: ov_post_key(h, s->vk); break;
        }
        HLOG_I("OVERLAY test: Schritt %d/%d f=%u %s vk=0x%02X", g_ov_step, g_ov_nsteps, n,
               s->act == OA_SHOT ? "SHOT" : s->act == OA_LOG ? "LOG" : s->act == OA_KEY ? "Taste" :
               s->act >= OA_TKEY ? "Aktion" : "Fokus",
               s->vk);
    }
}

int sb_ovl_shot_due(char *name, unsigned cap)
{
    if (!g_ov_shot_due) { return 0; }
    g_ov_shot_due = 0;
    g_ov_shot_n++;
    _snprintf_s(name, cap, _TRUNCATE, "ovl_%02u_f%06u", g_ov_shot_n, g_present_no);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Present (Render-Thread)                                             */
/* ------------------------------------------------------------------ */
static void ov_init_state(void)
{
    const char *u;
    g_ov_init = 1;
    g_ov_fps = g_cfg.interp_fps > 0 ? g_cfg.interp_fps : 0;
    g_ov_auto = g_cfg.interp_auto ? 1 : 0; /* W223 */
    g_ov_winmode = ov_clamp(g_cfg.window_mode, 0, 2);
    g_ov_label0 = g_cfg.hud_label_avoid ? 1 : 0; /* W153 */
    g_ov_avail = sb_rem_live_avail();
    if (g_cfg.overlay_save_path[0]) {
        strcpy_s(g_ov_save_path, sizeof g_ov_save_path, g_cfg.overlay_save_path);
    } else if ((u = hcfg_user_cfg_path()) != NULL && u[0]) {
        strcpy_s(g_ov_save_path, sizeof g_ov_save_path, u);
    }
    HLOG_I("OVERLAY: bereit (ESC oeffnet/schliesst; speichern nach %s; HD-Paket x2=%d x4=%d)",
           g_ov_save_path[0] ? g_ov_save_path : "(kein Pfad)", (g_ov_avail & 1) != 0, (g_ov_avail & 2) != 0);
    ov_log_values("start");
    ov_log_shader("start");
    ov_log_shader2("start");
    ov_log_fx("start");
}

/* W188: Gamepad im Menue (Render-Thread). Steuerkreuz/linker Stick = Pfeile
 * (Wiederholung), A = Enter, B = zurueck, START = weiter, LB/RB = Seite,
 * X = Vergleich (B-Taste), Pad-Tasten der Aktion "pause" = Pause-Taste. */
static unsigned short g_pad_prev = 0xFFFF;
static void ov_pad_tick(void)
{
    static ULONGLONG next_poll, rep_t;
    static int rep_vk, seen;
    static unsigned seq;
    static const struct { unsigned short bit; int vk; } map[] = {
        { IMAP_PAD_DU, VK_UP }, { IMAP_PAD_DD, VK_DOWN }, { IMAP_PAD_DL, VK_LEFT }, { IMAP_PAD_DR, VK_RIGHT },
        { IMAP_PAD_A, VK_RETURN }, { IMAP_PAD_B, VK_ESCAPE }, { IMAP_PAD_START, OK_PSTART },
        { IMAP_PAD_LB, OK_BTAB }, { IMAP_PAD_RB, VK_TAB }, { IMAP_PAD_X, 'B' }, { IMAP_PAD_Y, OK_PSHOT } };
    ULONGLONG now = GetTickCount64();
    unsigned short cur, edge;
    size_t k;
    if (g_pad_inject) { /* TEST(W188): eingespeiste Tasten statt XInput */
        cur = (unsigned short)g_pad_inject;
        g_pad_inject = 0;
        if (g_pad_prev == 0xFFFF) { g_pad_prev = 0; }
        HLOG_I("MENU test: Pad-Tasten 0x%04X", cur);
    } else {
        imap_pad_state_t ps;
        if (now < next_poll) {
            cur = g_pad_prev == 0xFFFF ? 0 : g_pad_prev;
            goto repeat;
        }
        next_poll = now + (seen ? 16u : 500u);
        seen = imap_pad_poll(0x80000000u | ++seq, &ps);
        cur = 0;
        g_ph_pad_lx = g_ph_pad_ly = g_ph_pad_rx = g_ph_pad_ry = g_ph_pad_tr = 0;
        if (seen) {
            int camsticks = 0;
            cur = ps.buttons;
            if (g_ov_scr == SC_PHOTO) { /* W195: freie Kamera - Sticks/Trigger steuern die Kamera, Steuerkreuz das Panel */
                sb_photo_ui_t pu;
                sb_photo_get(&pu);
                camsticks = pu.freecam;
                if (camsticks) {
                    g_ph_pad_lx = ps.lx; g_ph_pad_ly = ps.ly; g_ph_pad_rx = ps.rx; g_ph_pad_ry = ps.ry;
                    g_ph_pad_tr = (int)ps.rt - (int)ps.lt;
                }
            }
            if (!camsticks) {
                if (ps.ly > 16000) { cur |= IMAP_PAD_DU; }
                if (ps.ly < -16000) { cur |= IMAP_PAD_DD; }
                if (ps.lx < -16000) { cur |= IMAP_PAD_DL; }
                if (ps.lx > 16000) { cur |= IMAP_PAD_DR; }
            }
        }
    }
    edge = g_pad_prev == 0xFFFF ? 0 : (unsigned short)(cur & ~g_pad_prev);
    g_pad_prev = cur;
    if (edge & (unsigned short)g_ov_pause_pad) { ov_key(OK_PAUSE); edge &= (unsigned short)~g_ov_pause_pad; }
    if (edge & (unsigned short)g_ov_photo_pad) { ov_key(OK_PHOTO); edge &= (unsigned short)~g_ov_photo_pad; } /* W195 */
    if (edge & (unsigned short)g_ov_qs_pad) { ov_key(OK_PSHOT); edge &= (unsigned short)~g_ov_qs_pad; } /* W290: im Fotomodus = Foto */
    for (k = 0; k < sizeof map / sizeof map[0]; k++) {
        if (edge & map[k].bit) {
            ov_key(map[k].vk);
            if (map[k].bit & (IMAP_PAD_DU | IMAP_PAD_DD | IMAP_PAD_DL | IMAP_PAD_DR)) {
                rep_vk = map[k].vk;
                rep_t = now + 400u;
            }
        }
    }
repeat:
    if (rep_vk && (cur & (IMAP_PAD_DU | IMAP_PAD_DD | IMAP_PAD_DL | IMAP_PAD_DR))) {
        if (now >= rep_t) {
            ov_key(rep_vk);
            rep_t = now + 110u;
        }
    } else {
        rep_vk = 0;
    }
}

/* W290 QUICK-SHOT: Ablauf eines Schnell-Screenshots (Render-Thread, je Bild auch in der Pause).
 * Anforderung -> (ohne Cockpit: Vollbild-Ansicht anfordern, bis das Spiel sie gezeichnet hat) -> kurze Pause
 * ohne Menue (Aufnahme im ersten Pausenbild, sb_photo_quick_run) -> weiter, Einblendung "Screenshot saved". */
static void ov_qs_toast(const sb_photo_qres_t *r)
{
    wchar_t t1[160], t2[200], nb[200];
    t2[0] = 0;
    if (r->ok) {
        const char *sn = r->name;
        if (strncmp(sn, "Steelbound_", 11) == 0 && strlen(sn) > 22) { sn += 22; }
        sb_lang_w("ovl.qs_saved", t1, 160);
        MultiByteToWideChar(CP_UTF8, 0, sn, -1, nb, 200);
        _snwprintf_s(t2, 200, _TRUNCATE, L"%s  \x00B7  %u\x00D7%u", nb, r->w, r->h);
        if (r->notes) {
            wchar_t nt[120];
            sb_lang_w((r->notes & SB_QS_NOTE_NOFLAT) ? "ovl.qs_n_noflat" : (r->notes & SB_QS_NOTE_NOREC) ? "ovl.qs_n_norec"
                                                                                                        : "ovl.qs_n_smaller", nt, 120);
            wcsncat_s(t2, 200, L"  \x00B7  ", _TRUNCATE);
            wcsncat_s(t2, 200, nt, _TRUNCATE);
        }
    } else {
        sb_lang_w("ovl.qs_fail", t1, 160);
    }
    sb_ov_toast_msg(t1, t2, r->notes ? 3500u : 2200u);
}

/* ISSUE-PHOTO: vor der Pause ggf. einen Tick aufzeichnen (interp_fps=0), sonst gleich pausieren */
static void ov_qs_to_pause(void)
{
    if (sb_photo_quick_rec_begin()) { g_qs_state = QS_REC; return; }
    g_qs_state = QS_PAUSE;
    sb_photo_quick_arm();
}

static void ov_qs_tick(unsigned n)
{
    if (g_qs_req && g_qs_state == QS_IDLE) {
        InterlockedExchange(&g_qs_req, 0);
        if (g_ov_open || sb_clip_active()) {
            HLOG_I("QUICKSHOT: Menue/Clip offen - ignoriert (f=%u)", n);
        } else if (g_cfg.vr_enable) {
            wchar_t t1[160];
            sb_lang_w("ovl.qs_vr", t1, 160);
            sb_ov_toast_msg(t1, L"", 2500u);
            HLOG_I("QUICKSHOT: im VR-Betrieb ohne Wirkung");
        } else {
            g_qs_notes = 0;
            g_qs_t0 = n;
            g_qs_ms0 = GetTickCount64();
            if (g_cfg.photo_view == SB_PV_FLAT && !sb_flat_now()) {
                sb_flat_request(1);
                g_qs_state = QS_FLAT;
            } else {
                ov_qs_to_pause();
            }
        }
    }
    if (g_qs_state == QS_FLAT) {
        int st = sb_flat_state();
        if (st == 1) {
            HLOG_I("QUICKSHOT: Vollbild-Ansicht bereit nach %u ms", (unsigned)(GetTickCount64() - g_qs_ms0));
            ov_qs_to_pause();
        } else if (st < 0 || GetTickCount64() - g_qs_ms0 > 3000u) {
            sb_flat_request(0);
            g_qs_notes |= SB_QS_NOTE_NOFLAT;
            ov_qs_to_pause();
        }
    }
    if (g_qs_state == QS_REC && sb_photo_quick_rec_wait()) { /* ISSUE-PHOTO: Pause am Ende des aufgezeichneten Ticks */
        g_qs_state = QS_PAUSE;
        sb_photo_quick_arm();
    }
    if (g_qs_state == QS_PAUSE) {
        sb_photo_qres_t r;
        if (sb_photo_quick_result(&r)) {
            sb_flat_request(0);
            sb_photo_quick_rec_free(); /* ISSUE-PHOTO */
            g_qs_state = QS_IDLE;
            r.notes |= g_qs_notes;
            if (r.ok) {
                HLOG_I("QUICKSHOT: fertig (%ux%u, Hinweise 0x%X, %.0f ms Aufnahme, %u ms gesamt)", r.w, r.h, (unsigned)r.notes, r.ms,
                       (unsigned)(GetTickCount64() - g_qs_ms0));
            } else {
                HLOG_W("QUICKSHOT: fehlgeschlagen (Hinweise 0x%X)", (unsigned)r.notes);
            }
            ov_qs_toast(&r);
        }
    }
}

/* Ein Overlay-Schritt (Render-Thread): Test-Hook, Oeffnen/Schliessen, Tasten, Pad, Maus, Pause-Wunsch. */
static void ov_frame(unsigned n)
{
    int open;
    ov_test_tick(n);
    {
        LONG request = InterlockedExchange(&g_fb_hot_request, 0);
        if (request) {
            g_fb_hot = 1;
            g_fb_capture = g_fb_clip_prompt = 0;
            g_fb_kind = g_fb_len = g_fb_high = g_fb_have = 0;
            g_fb_text[0] = 0;
            if (sb_feedback_begin(&g_fb_pkg, 0)) {
                g_fb_have = 1;
                g_fb_capture = request == 1;
                g_fb_clip_prompt = request == 2;
                if (request == 2) {
                    int started;
                    sb_ctx_enter();
                    started = sb_clip_start(g_fb_pkg.dir);
                    sb_ctx_leave();
                    if (!started) {
                        g_fb_clip_prompt = 0;
                        g_fb_pkg.clip = -1;
                        InterlockedExchange(&g_ov_req, SC_FBKIND);
                        InterlockedExchange(&g_ov_open, 1);
                        ov_status_key("ovl.clip_fail");
                    }
                }
                HLOG_I("FEEDBACK hotkey: %s", request == 1 ? "screenshot" : "clip");
            } else {
                g_fb_hot = 0;
                InterlockedExchange(&g_ov_req, SC_FBKIND);
                InterlockedExchange(&g_ov_open, 1);
                ov_status_key("ovl.st_fb_fail");
            }
        }
    }
    ov_qs_tick(n); /* W290 QUICK-SHOT */
    if (sb_clip_active()) {
        int result;
        sb_ctx_enter();
        result = sb_clip_poll(&g_fb_pkg.clip_bytes, &g_fb_pkg.clip_w, &g_fb_pkg.clip_h);
        sb_ctx_leave();
        if (result) {
            g_fb_pkg.clip = result;
            sb_feedback_finish(&g_fb_pkg, g_fb_text);
            InterlockedExchange(&g_ov_req, g_fb_clip_prompt ? SC_FBKIND : SC_FBREVIEW);
            InterlockedExchange(&g_ov_open, 1);
            g_ov_was_open = 0;
            g_fb_clip_prompt = 0;
            HLOG_I("CLIP: return feedback result=%d", result);
        } else if (g_ov_open) { ov_close("Clip laeuft"); }
    }
    if (g_ov_notice_req && !g_ov_open && !sb_clip_active()) { /* W254: Hinweis (Replay-Start) */
        HWND h = sb_render_hwnd();
        InterlockedExchange(&g_ov_notice_req, 0);
        g_ov_notice_on = 1;
        HLOG_I("NOTICE: Hinweis offen (f=%u)", g_present_no);
        InterlockedExchange(&g_ov_welcome_on, 1);
        InterlockedExchange(&g_ov_req, SC_WELCOME);
        InterlockedExchange(&g_ov_open, 1);
        if (h) { PostMessageA(h, OV_MSG_OPENED, 0, 0); }
    }
    if (g_cfg.alpha_welcome && !g_ov_welcome_done) { /* W222: Alpha-Hinweis einmal je Programmstart, sobald Bilder laufen */
        if (!g_ov_welcome_f0) { g_ov_welcome_f0 = n ? n : 1; }
        if (g_ov_open || sb_clip_active()) {
            g_ov_welcome_done = 1; /* Spieler war schneller (ESC/Taste) - nicht mehr nachreichen */
            HLOG_I("WELCOME: uebersprungen (Overlay schon offen, f=%u)", g_present_no);
        } else if ((g_fb_bind_known & 3) == 3 || !g_cfg.input_profile[0] || n - g_ov_welcome_f0 >= 120u) {
            HWND h = sb_render_hwnd();
            g_ov_welcome_done = 1;
            HLOG_I("WELCOME: Alpha-Hinweis offen (f=%u, Belegung %s)", g_present_no,
                   (g_fb_bind_known & 3) == 3 ? "aus Profil" : "Standard");
            InterlockedExchange(&g_ov_welcome_on, 1);
            InterlockedExchange(&g_ov_req, SC_WELCOME);
            InterlockedExchange(&g_ov_open, 1);
            if (h) { PostMessageA(h, OV_MSG_OPENED, 0, 0); } /* Maus frei, gehaltene Spiel-Tasten los (UI-Thread) */
        }
    }
    open = g_ov_open != 0;
    if (open != g_ov_was_open) {
        g_ov_was_open = open;
        g_ov_dirty = 1;
        if (open) {
            LONG rq = InterlockedExchange(&g_ov_req, -1);
            ov_screen(rq >= 0 ? (int)rq : (g_cfg.game_menu ? SC_MENU : SC_SET), 0);
            g_pad_prev = 0xFFFF; /* gehaltene Pad-Tasten loesen nichts aus */
            g_ov_mgen_seen = g_ov_mgen;
            InterlockedExchange(&g_ov_blocked_q, 0);
            HLOG_I("OVERLAY: offen (f=%u) -> echte Eingaben gehen ans Overlay", g_present_no);
        } else {
            HWND h = sb_render_hwnd();
            HLOG_I("OVERLAY: zu (f=%u; verschluckt: tasten=%ld maustasten=%ld, gesperrte Abfragen=%ld)", g_present_no,
                   (long)g_ov_swallow_k, (long)g_ov_swallow_m, (long)g_ov_blocked_q);
            ov_log_values("zu");
            ov_log_shader("zu");
            ov_log_shader2("zu");
            ov_log_fx("zu");
            g_fb_pending = g_fb_capture = 0;
            InterlockedExchange(&g_ov_textmode, 0);
            if (h) { PostMessageA(h, OV_MSG_CLOSED, 0, 0); } /* W188: Maus wieder fangen (UI-Thread) */
        }
    }
    while (g_ov_qt != g_ov_qh) {
        int vk = (int)g_ov_q[g_ov_qt & 63];
        InterlockedIncrement(&g_ov_qt);
        if (g_ov_open) { ov_key(vk); }
    }
    if (g_ov_open) {
        LONG mg = g_ov_mgen;
        ov_pad_tick();
        if (mg != g_ov_mgen_seen) { /* W188: Zeiger waehlt die Zeile darunter */
            g_ov_mgen_seen = mg;
            ov_mouse((int)g_ov_mx, (int)g_ov_my, 0);
        }
        if (g_fb_capture && g_sb_paused) {
            g_fb_capture = 0;
            ov_fb_shot();
            g_ov_dirty = 1;
            sb_feedback_finish(&g_fb_pkg, g_fb_text);
            HLOG_I("FEEDBACK hotkey: screenshot ready=%d paused=%d size=%ux%u",
                   g_fb_pkg.shot_ok, g_sb_paused, g_fb_pkg.shot_w, g_fb_pkg.shot_h);
        }
        if (g_fb_pending && g_sb_paused) { ov_fb_create(); }
        if (g_ov_scr == SC_PHOTO) { ov_ph_tick(); } /* W195 */
    }
    if (g_ov_status_until && GetTickCount64() > g_ov_status_until) {
        g_ov_status_until = 0;
        g_ov_status[0] = 0;
        g_ov_dirty = 1;
    }
    { /* W188: Pause-Wunsch (Spiel steht am Ende des naechsten Presents) */
        int want = 0;
        const char *why = "Menue zu";
        if (g_ov_open) {
            if (g_ov_scr == SC_PAUSED) { want = 1; why = "Pause-Taste"; }
            else if (OV_IS_FB(g_ov_scr)) { want = 1; why = "Feedback"; }
            else if (g_ov_scr == SC_PHOTO) { want = !sb_photo_stepping(); why = want ? "Fotomodus" : "Fotomodus: Ansicht"; } /* W195, W290 */
            else if (g_ov_scr == SC_WELCOME) { want = 1; why = "Alpha-Hinweis"; } /* W222 */
            else if (g_cfg.menu_pause) { want = 1; why = "ESC-Menue"; }
            else { why = "menu_pause=0"; }
        }
        if (!want && g_qs_state == QS_PAUSE) { want = 1; why = "Schnell-Screenshot"; } /* W290 */
        sb_pause_request(want, why);
    }
}

void sb_ovl_present_begin(unsigned n)
{
    if (!g_cfg.overlay) { return; }
    if (!g_ov_init) { ov_init_state(); }
    sb_ov_tick(n); /* W278 ORIG-FLAG: Umschalten (Taste), Start-Hinweis; vor dem Menue-Takt */
    if (g_cfg.sbc_usb) { /* W305 JOY-HOTPLUG: Original-Controller getrennt/verbunden */
        int sev = sbc_wire_take_event();
        if (sev) {
            wchar_t t1[160], t2[200];
            sb_lang_w(sev == 1 ? "ovl.sbc_lost" : "ovl.sbc_back", t1, 160);
            sb_lang_w(sev == 1 ? "ovl.sbc_lost2" : "ovl.sbc_back2", t2, 200);
            sb_ov_toast_msg(t1, t2, sev == 1 ? 6000u : 3000u);
        }
    }
    ov_frame(n + g_ov_vframes); /* W188: Test-Takt zaehlt die Pausenbilder mit */
}

/* W188: ein Pausenbild (Spiel-Thread in sb_pause_point) */
void sb_ovl_pause_tick(void)
{
    if (!g_cfg.overlay) {
        sb_pause_request(0, "kein Overlay");
        return;
    }
    if (!g_ov_init) { ov_init_state(); }
    g_ov_vframes++;
    sb_ctx_enter(); /* VR-/Render-Pfade zeichnen das Overlay unter ctx_lock */
    ov_frame(g_present_no + g_ov_vframes);
    sb_ctx_leave();
}

/* W223 FPS-RATES: Monitor (auto) folgt dem Monitor des Fensters (Fenster/
 * randlos; Aufruf aus sb_winmode_tick bei Monitor- bzw. Moduswechsel).
 * Wechsel ueber denselben Weg wie die ESC-Auswahl (Present-Ende, Context-
 * Sperre). 0 = Overlay noch nicht bereit (spaeter erneut versuchen). */
int sb_ovl_fps_monitor(int hz)
{
    int v;
    if (!g_ov_init) { return 0; }
    InterlockedExchange(&g_ov_mon_hz, hz); /* W309 */
    if (!g_ov_auto) { return 1; }
    v = sb_fps_auto_from_hz(hz);
    if (v != g_ov_fps) {
        HLOG_I("OVERLAY: Bildrate auto -> %d (Monitor %d Hz, Fenster, f=%u)", v, hz, g_present_no);
        g_ov_fps = v;
        g_ov_fps_pend = 1;
        g_ov_dirty = 1;
    }
    return 1;
}

/* TEST(PERF-BENCH,W137): Bildratenwechsel wie aus dem Overlay anfordern
 * (rt_from_film + rt_interp_fps), wirkt am naechsten Present-Ende. */
void sb_ovl_request_fps(int fps)
{
    g_ov_fps = fps > 0 ? fps : 0;
    g_ov_auto = 0; /* W223 */
    g_ov_fps_pend = 1;
    HLOG_I("OVERLAY: Bildrate -> %d angefordert (Test, f=%u)", g_ov_fps, g_present_no);
}

/* W200: pause presentation follows the selection without discarding the
 * recorded still image. Apply render resources when leaving the pause. */
int sb_ovl_pause_fps(void)
{
    return g_ov_fps > 0 ? g_ov_fps : 30;
}

void sb_ovl_present_end(void)
{
    if (!g_ov_fps_pend) { return; }
    g_ov_fps_pend = 0;
    g_cfg.interp_auto = g_ov_auto; /* W223: Auswahl auto/fest (Rate selbst folgt unten) */
    if (g_sba_loc && !g_cfg.loc_interp && g_ov_fps > 0) { /* W323: ohne loc_interp kaemen nur Wiederhol-Presents (W320) */
        HLOG_I("OVERLAY: Bildrate %d im LoC-Profil ohne loc_interp -> 20 Hz (Original)", g_ov_fps);
        g_ov_fps = 0;
        g_ov_auto = 0;
        g_cfg.interp_auto = 0;
    }
    if (g_ov_fps == g_cfg.interp_fps) { return; }
    if (g_ov_fps > 0 && !sb_render_offscreen()) {
        g_cfg.interp_fps = g_ov_fps;
        if (!sb_render_views_rebuild("Overlay: Bildrate")) {
            HLOG_W("OVERLAY: Bildrate %d: Views nicht neu -> zurueck auf 30", g_ov_fps);
            g_cfg.interp_fps = 0;
            g_ov_fps = 0;
        }
    } else {
        sb_ctx_enter();
        sb_interp_reset("Overlay: Bildrate");
        g_cfg.interp_fps = g_ov_fps;
        sb_ctx_leave();
    }
    HLOG_I("OVERLAY: Bildrate aktiv: interp_fps=%d (%s, f=%u)", g_cfg.interp_fps,
           g_cfg.interp_fps ? "Zwischenbilder" : "30 Hz Original", g_present_no);
    g_ov_dirty = 1;
}

/* ------------------------------------------------------------------ */
/* Panel (GDI)                                                         */
/* ------------------------------------------------------------------ */
#define OV_C_BG     SB_UI_C_BG
#define OV_C_TXT    SB_UI_C_TXT
#define OV_C_DIM    SB_UI_C_DIM
#define OV_C_LINE   SB_UI_C_LINE
#define OV_C_HEAD   SB_UI_C_HEAD
#define OV_C_SEL    SB_UI_C_SEL
#define OV_C_EXP    RGB(255, 196, 92) /* W278 ORIG-FLAG: experimentell und aktiv */
#define OV_ALPHA    SB_UI_ALPHA

static float g_ovf = 1.0f;
static int S(float x)
{
    int v = (int)(x * g_ovf + 0.5f);
    return v < 1 ? 1 : v;
}

static void ov_fill(int x0, int y0, int x1, int y1, COLORREF c)
{
    RECT r;
    HBRUSH b = CreateSolidBrush(c);
    r.left = x0; r.top = y0; r.right = x1; r.bottom = y1;
    FillRect(ov_dc, &r, b);
    DeleteObject(b);
}

static void ov_text(int x, int y, const wchar_t *s, COLORREF c, int right)
{
    SetTextColor(ov_dc, c);
    SetTextAlign(ov_dc, (right ? TA_RIGHT : TA_LEFT) | TA_TOP);
    TextOutW(ov_dc, x, y, s, (int)wcslen(s));
}

/* W287 EXP-ICON: Kolben (Segoe Fluent Icons U+F196) statt "EXP", wenn die
 * Schrift das Zeichen hat (Windows 11). Sonst bleibt der Text "EXP"
 * (Windows 10 mit MDL2 ohne Kolben, Wine/Proton ohne Symbolschrift).
 * Zwei Groessen im Speicher (Desktop- und VR-Panel wechseln sich ab). */
#define OV_EXP_GLYPH L"\xF196"
static HFONT ov_exp_font(void)
{
    static HFONT f[2];
    static int px[2], ok[2], next;
    int i;
    for (i = 0; i < 2; ++i) {
        if (f[i] && px[i] == ov_font_px) { return ok[i] ? f[i] : NULL; }
    }
    if (!ov_dc) { return NULL; }
    i = next;
    next ^= 1;
    if (f[i]) { DeleteObject(f[i]); }
    px[i] = ov_font_px;
    ok[i] = 0;
    f[i] = CreateFontW(-ov_font_px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                       CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe Fluent Icons");
    if (f[i]) {
        wchar_t face[64] = L"";
        WORD g = 0xFFFF;
        HFONT o = (HFONT)SelectObject(ov_dc, f[i]);
        GetTextFaceW(ov_dc, 64, face);
        if (_wcsicmp(face, L"Segoe Fluent Icons") == 0 &&
            GetGlyphIndicesW(ov_dc, OV_EXP_GLYPH, 1, &g, GGI_MARK_NONEXISTING_GLYPHS) != GDI_ERROR && g != 0xFFFF) {
            ok[i] = 1;
        }
        SelectObject(ov_dc, o);
        HLOG_I("ORIGFLAG: ESC-Markierung %s (%d px)", ok[i] ? "Symbol" : "Text EXP", ov_font_px);
    }
    return ok[i] ? f[i] : NULL;
}

/* Symbol an x/y (TA_TOP wie ov_text), Rueckgabe = Breite in Pixeln. Ohne
 * Symbolschrift steht der Ersatztext da (W291: auch fuer die Neustart-Marke). */
static int ov_icon_mark(int x, int y, const wchar_t *glyph, const wchar_t *alt, COLORREF c)
{
    HFONT fi = ov_exp_font();
    SIZE sz = { 0, 0 };
    if (fi) {
        HFONT o = (HFONT)SelectObject(ov_dc, fi);
        GetTextExtentPoint32W(ov_dc, glyph, 1, &sz);
        ov_text(x, y + (ov_font_px - sz.cy) / 2, glyph, c, 0);
        SelectObject(ov_dc, o);
    } else {
        GetTextExtentPoint32W(ov_dc, alt, (int)wcslen(alt), &sz);
        ov_text(x, y, alt, c, 0);
    }
    return sz.cx;
}

/* Markierung an x/y (TA_TOP wie ov_text), Rueckgabe = Breite in Pixeln. */
static int ov_exp_mark(int x, int y, COLORREF c)
{
    wchar_t tg[16];
    ov_wkey(tg, 16, "ovl.of_tag");
    return ov_icon_mark(x, y, OV_EXP_GLYPH, tg, c);
}

/* W291: Neustart-Marke (Segoe Fluent Icons "Refresh" E72C, sonst "*") */
#define OV_RS_GLYPH L"\xE72C"
static int ov_rs_mark(int x, int y, COLORREF c)
{
    return ov_icon_mark(x, y, OV_RS_GLYPH, L"*", c);
}

/* W291: 0 = wirkt sofort, 1 = wirkt nach Neustart, 2 = gewaehlter Wert wartet auf den Neustart */
static int ov_rs_state(int i)
{
    switch (i) {
    case OI_WINMODE: return g_ov_winmode != ov_clamp(g_cfg.window_mode, 0, 2) ? 2 : 1;
    case OI_RES: return 1;
    case OI_RTS: return (g_ov_rts >= 0 && g_ov_rts != g_cfg.rt_scale) ? 2 : 1;
    case OI_LABEL: return g_cfg.hud_label_avoid != g_ov_label0 ? 2 : 1;
    case OI_VRON: return (g_ov_vr >= 0 && g_ov_vr != (g_cfg.vr_enable ? 1 : 0)) ? 2 : 1;
    case OI_VRPREV: return g_ov_vrprev >= 0 ? 2 : 1;
    case OI_SSAO: return g_bb_dsrv ? 0 : (g_cfg.ssao > 0 ? 2 : 1); /* nur ohne BB-Tiefe vom Start */
    case OI_SAVELOCK: return (krnl_savelock_active() && !g_cfg.save_lock) ? 2 : 0; /* Ausschalten wirkt beim Beenden */
    default: return 0;
    }
}

/* W278 ORIG-FLAG: Markierung "EXP" je Zeile (0 = neutral, 1 = experimentell
 * und original, 2 = weicht gerade vom Original ab). Einordnung: sb_origflag.h. */
static int ov_exp_state(int i)
{
    static const struct { int oi; const char *key; } kx[] = {
        { OI_SHADING, "shading" }, { OI_MFX, "monitor_fx" }, { OI_GRAIN, "monitor_grain" },
        { OI_GLITCH, "monitor_glitch" }, { OI_EDGE, "edge_mode" }, { OI_EDGEW, "edge_fade_width" },
        { OI_EDGES, "edge_fade_strength" }, { OI_HD, "hd_textures" }, { OI_HDSCALE, "hd_textures" },
        { OI_FOG, "world_fog" }, { OI_WCON, "world_contrast" }, { OI_WGAM, "world_gamma" },
        { OI_WSHARP, "world_sharpen" }, { OI_BLOOM, "bloom_strength" }, { OI_CGLOW, "cockpit_glow" },
        { OI_CSPILL, "cockpit_spill" }, { OI_CLIGHT, "cockpit_light" }, { OI_EXPO, "world_exposure" },
        { OI_TONE, "tonemap" }, { OI_BTHR, "bloom_threshold" }, { OI_LWORLD, "light_world" },
        { OI_LCKP, "light_cockpit" }, { OI_FOCUS, "world_focus" }, { OI_AA, "aa_mode" }, { OI_SSAO, "ssao" },
        { OI_CSAT, "color_sat" }, { OI_CTEMP, "color_temp" }, { OI_CLIFT, "color_lift" },
        { OI_CGAM, "color_gamma" }, { OI_CGAIN, "color_gain" }, { OI_SHADOW, "shade_shadow" },
        { OI_SHSOFT, "shadow_soft" }, { OI_LABEL, "hud_label_avoid" }, { OI_CPERSP, "cockpit_persp" },
        { OI_CPANINI, "cockpit_panini" }, { OI_CWIDTH, "cockpit_width" }, { OI_CHIDE, "cockpit_hide" }, { OI_MONVIEW, "monitor_view" },
        { OI_FXSOFT, "fx_soft" }, { OI_FXEMB, "fx_embers" }, { OI_FXLIGHT, "fx_light" },
        { OI_FXLCKP, "fx_light_cockpit" }, { OI_FXIP, "fx_interp" }, { OI_XWIRE, "fx_wireframe" },
        { OI_XVAPOR, "fx_vaporwave" },
    };
    int k;
    if (i == OI_PRESET) { /* Preset: Licht, Nachbearbeitung, Effekte, Monitor, HD, Effect Packs */
        return sb_ov_group_state((1u << SB_OFG_LIGHT) | (1u << SB_OFG_POST) | (1u << SB_OFG_FX) |
                                 (1u << SB_OFG_MONITOR) | (1u << SB_OFG_HD) | (1u << SB_OFG_PACKS));
    }
    if (i == OI_FXPRESET) { return sb_ov_group_state((1u << SB_OFG_FX) | (1u << SB_OFG_PACKS)); }
    if (ov_sum_idx(i) >= 0) { /* W291: Sammelregler = staerkster Zustand seiner Schluessel */
        const ov_sum_t *m = &kOvSum[ov_sum_idx(i)];
        int st = 0, q;
        for (k = 0; k < m->nk; k++) { q = sb_ov_key_state(m->key[k]); if (q > st) { st = q; } }
        return st;
    }
    if (i == OI_SMON) {
        int a = sb_ov_key_state("monitor_fx"), b2 = sb_ov_key_state("monitor_grain"), c2 = sb_ov_key_state("monitor_glitch");
        return a > b2 ? (a > c2 ? a : c2) : (b2 > c2 ? b2 : c2);
    }
    if (i == OI_STEX) { return sb_ov_key_state("hd_textures"); }
    if (i == OI_RPFX) { return g_cfg.replay_fx_live ? 2 : 1; } /* W299: experimentell (Kolben, bernstein wenn an) */
    if (i == OI_ADV) { /* W291: Feinregler der Gruppe zusammengefasst */
        const ov_grp_t *gp = &kOvGrp[g_ov_page];
        int st = 0, q;
        for (k = 0; k < gp->nadv; k++) {
            if (!ov_row_avail(gp->adv[k]) || ov_row_promoted(gp->adv[k])) { continue; } /* W296: oben sichtbar */
            q = ov_exp_state(gp->adv[k]);
            if (q > st) { st = q; }
        }
        return st;
    }
    for (k = 0; k < (int)(sizeof kx / sizeof kx[0]); k++) {
        if (kx[k].oi == i) { return sb_ov_key_state(kx[k].key); }
    }
    return 0;
}

static void ov_label(int i, wchar_t *o, size_t cap)
{
    /* W142: Sprachschluessel in OI-Reihenfolge (Anzeige via sb_lang_w). */
    static const char *const lb[OI_N] = {
        "ovl.lb_fps", "ovl.lb_shading", "ovl.lb_monfx", "ovl.lb_grain", "ovl.lb_glitch",
        "ovl.lb_edge", "ovl.lb_edgew", "ovl.lb_edges", "ovl.lb_mmode", "ovl.lb_msens",
        "ovl.lb_hd", "ovl.lb_hdscale", "ovl.lb_winmode", "ovl.lb_res",
        "ovl.lb_save", "ovl.lb_close",
        /* W121 Seite Shader */
        "ovl.lb_fog", "ovl.lb_wcon", "ovl.lb_wgam", "ovl.lb_wsharp", "ovl.lb_bloom",
        "ovl.lb_cglow", "ovl.lb_cspill", "ovl.lb_clight", "ovl.lb_cmp",
        /* W121 Teil 2 */
        "ovl.lb_expo", "ovl.lb_tone", "ovl.lb_bthr", "ovl.lb_lworld", "ovl.lb_lckp",
        "ovl.lb_rts", "ovl.lb_focus",
        /* W128 */
        "ovl.lb_sbcmouse",
        /* W131 */
        "ovl.lb_preset", "ovl.lb_aa", "ovl.lb_ssao", "ovl.lb_csat", "ovl.lb_ctemp",
        "ovl.lb_clift", "ovl.lb_cgam", "ovl.lb_cgain", "ovl.lb_shadow",
        "ovl.lb_shsoft", "ovl.lb_vqasync", "ovl.lb_label",
        /* W191 */
        "ovl.lb_cpersp", "ovl.lb_cpanini",
        /* W235 */
        "ovl.lb_cwidth",
        /* W133 Seite Effekte (W173: Effekt-Stufe, Teil des Grafik-Presets) */
        "ovl.lb_fxlevel", "ovl.lb_fxsoft", "ovl.lb_fxemb", "ovl.lb_fxlight", "ovl.lb_fxlightckp",
        "ovl.lb_fxip", "ovl.lb_cmp",
        /* W134 Seite VR */
        "ovl.lb_vron", "ovl.lb_vrrec", "ovl.lb_vrseat", "ovl.lb_vrscale", "ovl.lb_vripd",
        "ovl.lb_vrprev",
        /* W183 */
        "ovl.lb_vrfwd", "ovl.lb_vrpreset", "ovl.lb_vrres", "ovl.lb_vrhz", "ovl.lb_vrmirror", "ovl.lb_vrworld",
        "ovl.lb_vrshade",
        /* W188 */
        "ovl.lb_mpause",
        /* W190 */
        "ovl.lb_savelock", "ovl.lb_slcommit",
        /* W218 */
        "ovl.lb_assist", "ovl.lb_autogear",
        /* W271 */
        "ovl.lb_stickaim", "ovl.lb_sticksens",
        /* W242 */
        "ovl.lb_vrshell", "ovl.lb_vrvig",
        /* W240 */
        "ovl.lb_xwire", "ovl.lb_xvapor",
        /* W268 */
        "ovl.lb_chide",
        /* W291 */
        "ovl.lb_smon", "ovl.lb_hd", "ovl.lb_slight", "ovl.lb_shadow", "ovl.lb_sbloom", "ovl.lb_sglow", "ovl.lb_slook",
        "ovl.lb_adv", "ovl.lb_reset_grp", "ovl.lb_texinfo",
        /* W290-Schalter (Namen wie im Launcher, Karte "Fotos & Screenshots") */
        "ovl.lb_rplabel", "ovl.lb_rplogo", "ovl.lb_wmmedia",
        /* W303 AUDIO-VOLUME */
        "ovl.lb_volm", "ovl.lb_volmu", "ovl.lb_volsfx", "ovl.lb_volvox",
        /* W299 */
        "ovl.lb_rpfx",
        /* W319 */
        "ovl.lb_monview" };
    ov_wkey(o, cap, lb[i]);
}

static void ov_value(int i, wchar_t *o, size_t cap)
{
    /* W142: Sprachschluessel; Zahlen-/Einheitenformate bleiben. */
    static const char *const em[3] = { "ovl.v_fullw", "ovl.v_cut", "ovl.v_dim" }; /* W173: Glossar Breitbild-Raender */
    static const char *const wm[3] = { "ovl.v_win", "ovl.v_borderless", "ovl.v_excl" };
    o[0] = 0;
    switch (i) {
    case OI_FPS:
        if (g_ov_auto) { /* W223: "Monitor (auto, <n>)" */
            char nb[64];
            _snprintf_s(nb, sizeof nb, _TRUNCATE, LT("ovl.v_fps_auto"), g_ov_fps > 0 ? g_ov_fps : g_cfg.game_tick);
            ov_wu8(o, cap, nb);
        } else if (g_ov_fps == 0) { /* W234; W323: LoC-Mission taktet mit 20 Hz */
            ov_wkey(o, cap, g_sba_loc ? "ovl.v_20orig" : g_cfg.game_tick == 25 ? "ovl.v_25orig" : "ovl.v_30orig");
        }
        else { /* W309: "120 (kann ruckeln)" / "240 (ueber Monitor)" nach der Rate des Spielmonitors */
            int f = sb_fps_flags((int)g_ov_mon_hz, g_ov_fps);
            char nb[64];
            if (f) {
                _snprintf_s(nb, sizeof nb, _TRUNCATE, LT((f & SB_FPS_F_ABOVE) ? "ovl.v_fps_above" : "ovl.v_fps_judder"),
                            g_ov_fps);
                ov_wu8(o, cap, nb);
            } else { _snwprintf_s(o, cap, _TRUNCATE, L"%d", g_ov_fps); }
        }
        break;
    case OI_SHADING: ov_wkey(o, cap, g_cfg.shading ? "ovl.v_on" : "ovl.v_off"); break;
    case OI_MPAUSE: ov_wkey(o, cap, g_cfg.menu_pause ? "ovl.v_on" : "ovl.v_off"); break; /* W188 */
    case OI_SAVELOCK: /* W190: Zustand der Sitzung; "an bis Beenden" = ausgeschaltet, wirkt beim Beenden */
        ov_wkey(o, cap, !krnl_savelock_active() ? "ovl.v_off" : g_cfg.save_lock ? "ovl.v_on" : "ovl.v_sl_until_exit");
        break;
    case OI_MFX:
        ov_wkey(o, cap, g_cfg.monitor_fx == 0 ? "ovl.v_off" : (g_cfg.monitor_fx == 2 ? "ovl.v_altab" : "ovl.v_orig"));
        break;
    case OI_GRAIN: _snwprintf_s(o, cap, _TRUNCATE, L"%d %%", g_cfg.monitor_grain); break;
    case OI_GLITCH: _snwprintf_s(o, cap, _TRUNCATE, L"%d %%", g_cfg.monitor_glitch); break;
    case OI_EDGE: ov_wkey(o, cap, em[ov_clamp(g_cfg.edge_mode, 0, 2)]); break;
    case OI_EDGEW: _snwprintf_s(o, cap, _TRUNCATE, L"%d %%", g_cfg.edge_fade_width); break;
    case OI_EDGES: _snwprintf_s(o, cap, _TRUNCATE, L"%d %%", g_cfg.edge_fade_strength); break;
    case OI_MMODE: ov_wkey(o, cap, g_cfg.mouse_mode ? "ovl.v_abs" : "ovl.v_rel"); break;
    case OI_MSENS: _snwprintf_s(o, cap, _TRUNCATE, L"%d", g_cfg.mouse_sens); break;
    case OI_SBCMOUSE: ov_wkey(o, cap, g_cfg.sbc_mouse_off ? "ovl.v_mouseoff" : "ovl.v_parallel"); break;
    case OI_ASSIST: /* W236 */
        ov_wkey(o, cap, g_cfg.aim_assist == 2 ? "ovl.v_assist2" :
                        g_cfg.aim_assist == 1 ? "ovl.v_assist1" : "ovl.v_offorig");
        break;
    case OI_AUTOGEAR: ov_wkey(o, cap, g_cfg.auto_gear ? "ovl.v_on" : "ovl.v_off"); break;
    case OI_STICKAIM: ov_wkey(o, cap, g_cfg.stick_aim ? "ovl.v_stickrel" : "ovl.v_stickpos"); break; /* W271 */
    case OI_STICKSENS: _snwprintf_s(o, cap, _TRUNCATE, L"%d", g_cfg.stick_aim_sens); break;
    case OI_HD:
        if (!g_ov_avail) { ov_wkey(o, cap, "ovl.v_nopkg"); }
        else { ov_wkey(o, cap, ov_hd_on() ? "ovl.v_tex_hd" : "ovl.v_orig"); } /* W173: wie Launcher "Texturen" */
        break;
    case OI_HDSCALE: wcsncpy_s(o, cap, ov_hd_scale() == 2 ? L"2\u00D7" : L"4\u00D7", _TRUNCATE); break;
    case OI_WINMODE: {
        char nb[64];
        _snprintf_s(nb, sizeof nb, _TRUNCATE, "%s", LT(wm[g_ov_winmode])); /* W291: Neustart = Marke am Text */
        ov_wu8(o, cap, nb);
        break;
    }
    case OI_RES: _snwprintf_s(o, cap, _TRUNCATE, L"%u\u00D7%u", g_bb_w, g_bb_h); break;
    case OI_FOG:
        if (g_cfg.world_fog == 100) { ov_wkey(o, cap, "ovl.v_orig"); }
        else if (g_cfg.world_fog == 50) { ov_wkey(o, cap, "ovl.v_reduced"); }
        else if (g_cfg.world_fog == 0) { ov_wkey(o, cap, "ovl.v_off"); }
        else { _snwprintf_s(o, cap, _TRUNCATE, L"%d %%", g_cfg.world_fog); }
        break;
    case OI_WCON: case OI_WGAM: case OI_BLOOM: case OI_CLIGHT: {
        int v = i == OI_WCON ? g_cfg.world_contrast : i == OI_WGAM ? g_cfg.world_gamma :
                i == OI_BLOOM ? g_cfg.bloom_strength : g_cfg.cockpit_light;
        if (v == 100) { ov_wkey(o, cap, "ovl.v_orig"); }
        else { _snwprintf_s(o, cap, _TRUNCATE, L"%d %%", v); }
        break;
    }
    case OI_WSHARP: case OI_CGLOW: case OI_CSPILL: {
        int v = i == OI_WSHARP ? g_cfg.world_sharpen : i == OI_CGLOW ? g_cfg.cockpit_glow : g_cfg.cockpit_spill;
        if (v <= 0) { ov_wkey(o, cap, "ovl.v_offorig"); }
        else { _snwprintf_s(o, cap, _TRUNCATE, L"%d %%", v); }
        break;
    }
    case OI_ABCMP: ov_wkey(o, cap, g_shfx_bypass ? "ovl.v_orig" : "ovl.v_opt"); break;
    case OI_FXPRESET: ov_wkey(o, cap, fxr_preset_key(fxr_preset_match())); break; /* W133 */
    case OI_FXSOFT: case OI_FXEMB: case OI_FXLIGHT: case OI_FXLCKP: {
        int v = i == OI_FXSOFT ? g_cfg.fx_soft : i == OI_FXEMB ? g_cfg.fx_embers : i == OI_FXLIGHT ? g_cfg.fx_light :
                g_cfg.fx_light_cockpit;
        if (v <= 0) { ov_wkey(o, cap, "ovl.v_offorig"); }
        else { _snwprintf_s(o, cap, _TRUNCATE, L"%d %%", v); }
        break;
    }
    case OI_FXIP: ov_wkey(o, cap, g_cfg.fx_interp ? "ovl.v_interp" : "ovl.v_offorig"); break;
    case OI_CHIDE: /* W268; W291: "Cockpit: An (Original) / Aus (Vollbild-Ansicht)" wie W290 */
        ov_wkey(o, cap, g_cfg.vr_enable ? "ovl.v_xp_vr" : g_cfg.cockpit_hide ? "ovl.v_ckp_off" : "ovl.v_ckp_on");
        break;
    case OI_MONVIEW: /* W319 */
        ov_wkey(o, cap, g_cfg.vr_enable ? "ovl.v_xp_vr" : g_cfg.monitor_view == 2 ? "ovl.v_mview_2" :
                        g_cfg.monitor_view == 1 ? "ovl.v_mview_1" : "ovl.v_offorig");
        break;
    case OI_RPLABEL: ov_wkey(o, cap, g_cfg.replay_label ? "ovl.v_onorig" : "ovl.v_off"); break; /* W290-Schalter */
    case OI_RPLOGO: ov_wkey(o, cap, g_cfg.replay_logo ? "ovl.v_onorig" : "ovl.v_off"); break;
    case OI_WMMEDIA: ov_wkey(o, cap, g_cfg.watermark_media ? "ovl.v_on" : "ovl.v_off"); break;
    case OI_VOLM: _snwprintf_s(o, cap, _TRUNCATE, L"%d %%", g_cfg.volume_master); break; /* W303 */
    case OI_VOLMU: _snwprintf_s(o, cap, _TRUNCATE, L"%d %%", g_cfg.volume_music); break;
    case OI_VOLSFX: _snwprintf_s(o, cap, _TRUNCATE, L"%d %%", g_cfg.volume_sfx); break;
    case OI_VOLVOX: _snwprintf_s(o, cap, _TRUNCATE, L"%d %%", g_cfg.volume_voice); break;
    case OI_RPFX: { /* W299: Namen der Spiel-Filter ("EFFECT #n") mit Beschreibung */
        static const char *const fk[9] = { "ovl.v_offorig", "ovl.v_rpfx1", "ovl.v_rpfx2", "ovl.v_rpfx3", "ovl.v_rpfx4",
                                           "ovl.v_rpfx5", "ovl.v_rpfx6", "ovl.v_rpfx7", "ovl.v_rpfx8" };
        int k = g_cfg.replay_fx_live;
        ov_wkey(o, cap, fk[k >= 0 && k <= 8 ? k : 0]);
        break;
    }
    case OI_SMON: { /* W291 */
        static const char *const mk[4] = { "ovl.v_orig", "ovl.v_mon_grain", "ovl.v_mon_glitch", "ovl.v_off" };
        int q = ov_mon_stage();
        ov_wkey(o, cap, q >= 0 ? mk[q] : q == -2 ? "ovl.v_altab" : "ovl.v_custom");
        break;
    }
    case OI_STEX: { /* W291 */
        static const char *const tk[3] = { "ovl.v_orig", "ovl.v_hd2", "ovl.v_hd4" };
        ov_wkey(o, cap, !g_ov_avail ? "ovl.v_nopkg" : tk[ov_tex_stage()]);
        break;
    }
    case OI_SLIGHT: case OI_SSHADOW: case OI_SBLOOM: case OI_SGLOW: case OI_SLOOK: { /* W291 */
        const ov_sum_t *m = &kOvSum[ov_sum_idx(i)];
        int q = ov_sum_match(ov_sum_idx(i));
        if (q < 0) { ov_wkey(o, cap, "ovl.v_custom"); }
        else if (m->lbl[q]) { ov_wkey(o, cap, m->lbl[q]); }
        else { _snwprintf_s(o, cap, _TRUNCATE, L"%d %%", m->v[q][0]); }
        break;
    }
    case OI_XWIRE: case OI_XVAPOR: /* W240 */
        ov_wkey(o, cap, g_cfg.vr_enable ? "ovl.v_xp_vr" : (i == OI_XWIRE ? g_cfg.fx_wireframe : g_cfg.fx_vaporwave) ?
                        "ovl.v_on" : "ovl.v_offorig");
        break;
    case OI_VRON: { /* W134 VR-PROTO */
        int v = g_ov_vr >= 0 ? g_ov_vr : g_cfg.vr_enable;
        char nb[64];
        _snprintf_s(nb, sizeof nb, _TRUNCATE, "%s", LT(v ? "ovl.v_on" : "ovl.v_off"));
        ov_wu8(o, cap, nb);
        break;
    }
    case OI_VRREC: {
        const char *st = sb_vr_state_text();
        _snwprintf_s(o, cap, _TRUNCATE, L"%S", st);
        break;
    }
    case OI_VRSEAT: _snwprintf_s(o, cap, _TRUNCATE, L"%+d mm", g_cfg.vr_seat_height); break;
    case OI_VRSCALE: _snwprintf_s(o, cap, _TRUNCATE, L"%d %%", g_cfg.vr_world_scale); break;
    case OI_VRIPD: _snwprintf_s(o, cap, _TRUNCATE, L"%d.%d mm", g_cfg.vr_ipd / 10, g_cfg.vr_ipd % 10); break;
    case OI_VRPREV: {
        static const char *const pv[4] = { "ovl.v_eye_both", "ovl.v_eye_left", "ovl.v_eye_right", "ovl.v_off" };
        int k = g_ov_vrprev;
        char nb[64];
        if (k < 0) {
            k = _stricmp(g_cfg.vr_preview, "left") == 0 ? 1 : _stricmp(g_cfg.vr_preview, "right") == 0 ? 2 :
                (_stricmp(g_cfg.vr_preview, "off") == 0 || _stricmp(g_cfg.vr_preview, "xr") == 0) ? 3 : 0;
        }
        _snprintf_s(nb, sizeof nb, _TRUNCATE, "%s", LT(pv[k]));
        ov_wu8(o, cap, nb);
        break;
    }
    case OI_VRFWD: _snwprintf_s(o, cap, _TRUNCATE, L"%+d mm", g_cfg.vr_seat_forward); break; /* W183 */
    case OI_VRPRESET: {
        static const char *const pk[4] = { "ovl.v_vrp_custom", "ovl.v_vrp_perf", "ovl.v_vrp_bal", "ovl.v_vrp_qual" };
        ov_wkey(o, cap, pk[ov_clamp(sb_vr_preset_match(), 0, 3)]);
        break;
    }
    case OI_VRRES: _snwprintf_s(o, cap, _TRUNCATE, L"%d %%", g_cfg.vr_render_scale); break;
    case OI_VRHZ:
        if (g_cfg.vr_refresh_hz <= 0) { ov_wkey(o, cap, "ovl.v_vrhz_rt"); }
        else { _snwprintf_s(o, cap, _TRUNCATE, L"%d Hz", g_cfg.vr_refresh_hz); }
        break;
    case OI_VRMIRROR:
        ov_wkey(o, cap, !g_cfg.vr_mirror ? "ovl.v_vrm_off" : (g_cfg.vr_mirror_res < 100 ? "ovl.v_vrm_half" :
                        (g_cfg.vr_mirror_fit ? "ovl.v_vrm_bars" : "ovl.v_vrm_fill")));
        break;
    case OI_VRWORLD: ov_wkey(o, cap, g_cfg.vr_world_tick ? "ovl.v_vrw_tick" : "ovl.v_vrw_frame"); break;
    case OI_VRSHADE: ov_wkey(o, cap, g_cfg.vr_shading ? "ovl.v_on" : "ovl.v_off"); break;
    case OI_VRSHELL: ov_wkey(o, cap, g_cfg.vr_shell ? "ovl.v_on" : "ovl.v_off"); break; /* W242 */
    case OI_VRVIG:
        if (g_cfg.vr_vignette <= 0) { ov_wkey(o, cap, "ovl.v_off"); }
        else { _snwprintf_s(o, cap, _TRUNCATE, L"%d %%", g_cfg.vr_vignette > 100 ? 100 : g_cfg.vr_vignette); }
        break;
    case OI_FXCMP: ov_wkey(o, cap, g_fxr_bypass ? "ovl.v_orig" : "ovl.v_opt"); break;
    case OI_EXPO: case OI_LWORLD: case OI_LCKP: case OI_FOCUS: {
        int v = i == OI_EXPO ? g_cfg.world_exposure : i == OI_LWORLD ? g_cfg.light_world :
                i == OI_FOCUS ? g_cfg.world_focus : g_cfg.light_cockpit;
        if (v == 100) { ov_wkey(o, cap, "ovl.v_orig"); }
        else { _snwprintf_s(o, cap, _TRUNCATE, L"%d %%", v); }
        break;
    }
    case OI_TONE: {
        static const char *const tm[3] = { "ovl.v_offorig", "ovl.v_reinhard", "ovl.v_aces" };
        ov_wkey(o, cap, tm[ov_clamp(g_cfg.tonemap, 0, 2)]);
        break;
    }
    case OI_BTHR:
        if (g_cfg.bloom_threshold <= 0) { ov_wkey(o, cap, "ovl.v_offorig"); }
        else { _snwprintf_s(o, cap, _TRUNCATE, L"%d %%", g_cfg.bloom_threshold); }
        break;
    case OI_RTS: {
        int dyn = g_ov_dyn >= 0 ? g_ov_dyn : (g_cfg.rt_scale_dyn ? 1 : 0);
        int v = g_ov_rts >= 0 ? g_ov_rts : g_cfg.rt_scale;
        const char *mark = ""; /* W291: Neustart = Marke am Text */
        char nb[64];
        if (dyn) { _snprintf_s(nb, sizeof nb, _TRUNCATE, LT("ovl.v_rtdyn"), sb_rtt_dyn_factor(), mark); }
        else if (v == 1) { _snprintf_s(nb, sizeof nb, _TRUNCATE, LT("ovl.v_origmark"), mark); }
        else if (v == 0) { _snprintf_s(nb, sizeof nb, _TRUNCATE, LT("ovl.v_auto"), (g_bb_h + 639u) / 640u > 4u ? 4u : (g_bb_h + 639u) / 640u, mark); }
        else { _snprintf_s(nb, sizeof nb, _TRUNCATE, "%d\xC3\x97%s", v, mark); }
        ov_wu8(o, cap, nb);
        break;
    }
    case OI_PRESET: { /* W131, W143; W173 LAUNCHER-P4: einheitliches Preset. W296 PRESET-ORIG: nur
                       * "Original" (Monitor-Rate) ohne Bildrate im Namen, Spalte 3 "Performance" */
        static const char *const pn[SB_GP_NPRE] = { "ovl.v_orig_hd", "ovl.v_subtle", "ovl.v_strong", "ovl.v_performance",
                                                    "ovl.v_clear" /* W252 */ };
        int q = sb_gfx_preset_match_for(ov_fps_sel(), g_ov_rts >= 0 ? g_ov_rts : g_cfg.rt_scale);
        ov_wkey(o, cap, q >= 0 ? pn[q] : "ovl.v_custom");
        break;
    }
    case OI_AA: {
        static const char *const am[4] = { "ovl.v_offorig", "ovl.v_fxaa_final", "ovl.v_fxaa_world", "ovl.v_fxaa_both" };
        ov_wkey(o, cap, am[ov_clamp(g_cfg.aa_mode, 0, 3)]);
        break;
    }
    case OI_SSAO: /* W131: ohne BB-Tiefen-SRV (Start mit ssao=0) erst nach Neustart -> "*" */
        if (g_cfg.ssao <= 0) { ov_wkey(o, cap, "ovl.v_offorig"); }
        else { _snwprintf_s(o, cap, _TRUNCATE, L"%d %%", g_cfg.ssao); }
        break;
    case OI_CSAT: case OI_CGAM: case OI_CGAIN: {
        int v = i == OI_CSAT ? g_cfg.color_sat : i == OI_CGAM ? g_cfg.color_gamma : g_cfg.color_gain;
        if (v == 100) { ov_wkey(o, cap, "ovl.v_orig"); }
        else { _snwprintf_s(o, cap, _TRUNCATE, L"%d %%", v); }
        break;
    }
    case OI_CTEMP:
        if (g_cfg.color_temp == 0) { ov_wkey(o, cap, "ovl.v_orig"); }
        else {
            char nb[64];
            _snprintf_s(nb, sizeof nb, _TRUNCATE, "%+d (%s)", g_cfg.color_temp,
                        LT(g_cfg.color_temp > 0 ? "ovl.v_warmer" : "ovl.v_colder"));
            ov_wu8(o, cap, nb);
        }
        break;
    case OI_CLIFT:
        if (g_cfg.color_lift == 0) { ov_wkey(o, cap, "ovl.v_orig"); }
        else { _snwprintf_s(o, cap, _TRUNCATE, L"%+d", g_cfg.color_lift); }
        break;
    case OI_SHADOW: ov_wkey(o, cap, g_cfg.shade_shadow ? "ovl.v_on" : "ovl.v_offprev"); break;
    case OI_SHSOFT: ov_wkey(o, cap, g_cfg.shadow_soft ? "ovl.v_soft33" : "ovl.v_prev22"); break;
    case OI_VQASYNC: ov_wkey(o, cap, g_cfg.vis_query_async ? "ovl.v_lat1" : "ovl.v_block"); break;
    case OI_CPERSP: /* W191 */
        if (g_cfg.cockpit_persp <= 0) { ov_wkey(o, cap, "ovl.v_orig"); }
        else { _snwprintf_s(o, cap, _TRUNCATE, L"%d %%", g_cfg.cockpit_persp); }
        break;
    case OI_CPANINI: /* W191 */
        if (g_cfg.cockpit_panini <= 0) { ov_wkey(o, cap, "ovl.v_orig"); }
        else { _snwprintf_s(o, cap, _TRUNCATE, L"%d %%", g_cfg.cockpit_panini); }
        break;
    case OI_CWIDTH: /* W235 */
        if (g_cfg.cockpit_width <= 0) { ov_wkey(o, cap, "ovl.v_orig"); }
        else { _snwprintf_s(o, cap, _TRUNCATE, L"+%d %%", g_cfg.cockpit_width); }
        break;
    case OI_LABEL: { /* W153: "*" bei Abweichung vom Startwert (Neustart) */
        char nb[64];
        _snprintf_s(nb, sizeof nb, _TRUNCATE, "%s", LT(g_cfg.hud_label_avoid ? "ovl.v_avoid" : "ovl.v_orig"));
        ov_wu8(o, cap, nb);
        break;
    }
    default: break;
    }
}

/* Panel-Groesse in Zielpixeln (Faktor aus der 4:3-Box) */
#define OV_TABH 13.0f /* W188: Tab-Zeile der Einstellungen */
static int g_rel_rows_y, g_rel_rowh, g_rel_nrows, g_rel_valx, g_rel_tab_y0, g_rel_tab_y1, g_rel_tab_x[OV_NPAGES + 1];
static int g_rel_side_h, g_rel_rows_x0, g_rel_head; /* W291: Seitenleiste, Listenbeginn, Kopfhoehe */

/* W188: Textzeilen ueber den Auswahlzeilen je Menue-Bildschirm */
static int ov_scr_lines(void)
{
    switch (g_ov_scr) {
    case SC_MENU: return krnl_savelock_active() ? 1 : 0; /* W190: Hinweis "Spielstaende geschuetzt" */
    case SC_QUIT: return 1;
    case SC_PAUSED: return 1;
    case SC_FBKIND: return 2;
    case SC_FBTEXT: return 3;
    case SC_FBREVIEW: return 8;
    case SC_WELCOME: return 9; /* W222: mit zwei Leerzeilen */
    default: return 0;
    }
}

/* W291: Masse der Einstellungen (Einheiten der 540er-Box) */
#define OV_SET_W 380.0f
#define OV_SIDE_W 112.0f
#define OV_SIDE_H 13.0f
/* Zeilen einer Gruppe mit aufgeklappten Feinreglern (inkl. Aktionen) */
static int ov_grp_rows_max(int g)
{
    const ov_grp_t *gp = &kOvGrp[g];
    int k, n = 0, na = 0;
    for (k = 0; k < gp->nadv; k++) { na += ov_row_avail(gp->adv[k]); } /* W296: unabhaengig von "Fade out" (nach oben geholt) */
    for (k = 0; k < gp->nmain; k++) { n += ov_row_avail(gp->main[k]); }
    return n + (na ? 1 + na : 0) + gp->reset + 1;
}

static int ov_set_body_h(void)
{
    int g, m = 0, rows, side = OG_N * S(OV_SIDE_H);
    for (g = 0; g < OG_N; g++) { if (ov_grp_rows_max(g) > m) { m = ov_grp_rows_max(g); } }
    rows = m * S(12.5f);
    return rows > side ? rows : side;
}

static void ov_layout(unsigned boxh, unsigned *w, unsigned *h)
{
    g_ovf = (float)boxh / 540.0f;
    if (g_ovf < 1.0f) { g_ovf = 1.0f; }
    if (g_ov_scr == SC_PHOTO) { /* W195: Foto-Panel (Kopf, Tab-Zeile, 7 Zeilen, 4 Fusszeilen) */
        int nr = PH_NR0 > PH_NR1 ? PH_NR0 : PH_NR1;
        *w = (unsigned)S(272.0f);
        *h = (unsigned)(S(20.0f) + S(3.0f) + S(OV_TABH) + S(4.0f) + nr * S(12.5f) + S(6.0f) + 4 * S(11.0f) + S(6.0f));
        return;
    }
    if (g_ov_scr != SC_SET) { /* W188: Menue-Bildschirme */
        int nl = ov_scr_lines();
        *w = (unsigned)S((OV_IS_FB(g_ov_scr) || g_ov_scr == SC_WELCOME) ? 300.0f : 210.0f); /* W222: Hinweis breit */
        *h = (unsigned)(S(20.0f) + S(5.0f) + nl * S(11.0f) + (nl ? S(5.0f) : 0) + ov_scr_rows() * S(12.5f) + S(6.0f) +
                        2 * S(11.0f) + S(6.0f));
        return;
    }
    /* W291: Seitenleiste + Zeilen; Hoehe fest fuer die laengste aufgeklappte Gruppe (kein Springen beim Aufklappen) */
    *w = (unsigned)S(OV_SET_W);
    *h = (unsigned)(S(20.0f) + S(3.0f) + ov_set_body_h() + S(6.0f) + 3 * S(11.0f) + S(6.0f));
    if (g_cfg.sbc_usb) {
        *h += (unsigned)S(11.0f); /* W128: Zeile "Controller: ..." (nur sbc_usb=1) */
    }
}

static int ov_gdi_size(unsigned w, unsigned h)
{
    BITMAPINFO bi;
    if (ov_dc && w == ov_bw && h == ov_bh) { return 1; }
    if (!ov_dc) {
        ov_dc = CreateCompatibleDC(NULL);
        if (!ov_dc) { return 0; }
    }
    if (ov_bmp) {
        SelectObject(ov_dc, ov_bmp_old);
        DeleteObject(ov_bmp);
        ov_bmp = NULL;
    }
    if (ov_font) {
        SelectObject(ov_dc, ov_font_old);
        DeleteObject(ov_font);
        ov_font = NULL;
    }
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = (LONG)w;
    bi.bmiHeader.biHeight = -(LONG)h; /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    ov_bmp = CreateDIBSection(ov_dc, &bi, DIB_RGB_COLORS, (void **)&ov_bits, NULL, 0);
    if (!ov_bmp || !ov_bits) { return 0; }
    ov_bmp_old = (HBITMAP)SelectObject(ov_dc, ov_bmp);
    ov_font_px = S(10.5f);
    ov_font = CreateFontW(-ov_font_px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                          OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN,
                          L"Consolas");
    if (ov_font) { ov_font_old = (HFONT)SelectObject(ov_dc, ov_font); }
    SetBkMode(ov_dc, TRANSPARENT);
    ov_bw = w;
    ov_bh = h;
    return 1;
}

/* Rahmen + Eckwinkel (HUD-Klammern) + Kopfzeile - gemeinsam fuer alle Bildschirme (W188) */
static int ov_frame_head(int W, int H, const wchar_t *title, const wchar_t *tag)
{
    int b = S(1.0f), cl = S(8.0f), head = S(20.0f), pad = S(6.0f), ty;
    ov_fill(0, 0, W, H, OV_C_BG);
    ov_fill(0, 0, W, b, OV_C_LINE);
    ov_fill(0, H - b, W, H, OV_C_LINE);
    ov_fill(0, 0, b, H, OV_C_LINE);
    ov_fill(W - b, 0, W, H, OV_C_LINE);
    ov_fill(0, 0, cl, 2 * b + 1, OV_C_TXT);
    ov_fill(0, 0, 2 * b + 1, cl, OV_C_TXT);
    ov_fill(W - cl, H - 2 * b - 1, W, H, OV_C_TXT);
    ov_fill(W - 2 * b - 1, H - cl, W, H, OV_C_TXT);
    ov_fill(b + S(2.0f), b + S(2.0f), W - b - S(2.0f), head, OV_C_HEAD);
    ty = b + S(2.0f) + (head - b - S(2.0f) - ov_font_px) / 2 - S(0.5f);
    ov_text(pad + S(2.0f), ty, title, OV_C_BG, 0);
    if (tag && tag[0]) { ov_text(W - pad - S(2.0f), ty, tag, OV_C_BG, 1); }
    return head;
}

/* W222: Tastenname einer Belegung fuer den Alpha-Hinweis (0 = nicht belegt) */
static void ov_vk_wname(int vk, wchar_t *o, size_t cap)
{
    UINT sc;
    if (vk <= 0) { wcscpy_s(o, cap, L"--"); return; }
    if (vk >= VK_F1 && vk <= VK_F24) { _snwprintf_s(o, cap, _TRUNCATE, L"F%d", vk - VK_F1 + 1); return; }
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) { _snwprintf_s(o, cap, _TRUNCATE, L"%c", vk); return; }
    if (vk == VK_ESCAPE) { wcscpy_s(o, cap, L"Esc"); return; }
    sc = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
    if (!sc || GetKeyNameTextW((LONG)(sc << 16), o, (int)cap) <= 0) { _snwprintf_s(o, cap, _TRUNCATE, L"0x%02X", vk); }
}

/* W222: Build-Datum von sb_emu.dll (PE-Zeitstempel, wie das Feedback-Paket) */
static void ov_build_date(char *o, size_t cap)
{
    HMODULE m = GetModuleHandleA("sb_emu.dll");
    strcpy_s(o, cap, "?");
    if (m) {
        const IMAGE_DOS_HEADER *dh = (const IMAGE_DOS_HEADER *)m;
        const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const char *)m + dh->e_lfanew);
        time_t t = (time_t)nt->FileHeader.TimeDateStamp;
        struct tm g;
        if (gmtime_s(&g, &t) == 0) { _snprintf_s(o, cap, _TRUNCATE, "%04d-%02d-%02d", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday); }
    }
}

/* W188: Menue-Bildschirme (Spielmenue, Beenden?, Pause, Feedback) im Look der Einstellungen */
static void ov_paint_menu(void)
{
    int W = (int)ov_bw, H = (int)ov_bh, rowh = S(12.5f), pad = S(6.0f), b = S(1.0f), y, k, nl = ov_scr_lines();
    int nr = ov_scr_rows();
    wchar_t title[32], tag[48], ln[9][128], lb[64], vb[64], hint[96], rst[8]; /* W222: 9 Zeilen (Alpha-Hinweis) */
    COLORREF lc[9];
    static const char *const rk[][MI_N] = {
        { NULL },
        { "ovl.m_continue", "ovl.m_settings", "ovl.m_photo", "ovl.m_feedback", "ovl.m_quit" },
        { "ovl.q_no", "ovl.q_yes" },
        { "ovl.m_continue", "ovl.p_menu" },
        { "ovl.fb_bug", "ovl.fb_idea", "ovl.fb_back" },
        { "ovl.fb_create" },
        { "ovl.fb_open", "ovl.fb_folder", "ovl.fb_done", "ovl.clip_record", "ovl.clip_remove" },
        { NULL }, /* SC_PHOTO: eigenes Panel */
        { "ovl.m_continue" }, /* W222 SC_WELCOME */
    };
    tag[0] = 0;
    for (k = 0; k < 9; k++) { ln[k][0] = 0; lc[k] = OV_C_TXT; }
    wcscpy_s(title, 32, OV_IS_FB(g_ov_scr) ? L"\x25A0 FEEDBACK" : L"\x25A0 SYS-MENU");
    switch (g_ov_scr) {
    case SC_WELCOME: { /* W222 ALPHA-WELCOME: Dank, Feedback-Tasten (echte Belegung), GitHub-Hinweis */
        char nb[160], bd[16];
        wchar_t kn[24], ds[96];
        static const char *const dk[3] = { "ovl.w_k_shot", "ovl.w_k_clip", "ovl.w_k_menu" };
        int j;
        if (g_ov_notice_on) { /* W254: Hinweis (z. B. Replay-Start) im selben Bildschirm */
            wcscpy_s(title, 32, L"\x25A0 ");
            ov_wkey(title + 2, 30, g_ov_notice_tkey); /* W272: Titel je Hinweis (Standard Replay-Start) */
            ov_wkey(tag, 48, g_ov_notice_gkey);
            for (j = 0; j < 6; j++) { ov_wu8(ln[j], 128, g_ov_notice[j]); }
            lc[0] = OV_C_HEAD;
            ov_wkey(hint, 96, "ovl.hint_welcome");
            break;
        }
        wcscpy_s(title, 32, L"\x25A0 ");
        ov_wkey(title + 2, 30, "ovl.w_title");
        if (sb_version_str()[0]) { /* PO: echte Release-Version (W219 SB_VERSION) statt Build-Datum */
            _snprintf_s(nb, sizeof nb, _TRUNCATE, LT("ovl.w_version"), sb_version_str());
        } else {
            ov_build_date(bd, sizeof bd);
            _snprintf_s(nb, sizeof nb, _TRUNCATE, LT("ovl.w_build"), bd);
        }
        ov_wu8(tag, 48, nb);
        ov_wkey(ln[0], 128, "ovl.w_thanks");
        lc[0] = OV_C_HEAD;
        ov_wkey(ln[1], 128, "ovl.w_break");
        for (j = 0; j < 3; j++) {
            ov_vk_wname(j == 2 ? VK_ESCAPE : (int)g_fb_vk[j][0], kn, 24);
            ov_wkey(ds, 96, dk[j]);
            _snwprintf_s(ln[3 + j], 128, _TRUNCATE, L"%-5s %s", kn, ds);
        }
        ov_wkey(ln[7], 128, "ovl.w_reports");
        ov_wkey(ln[8], 128, "ovl.w_helps");
        lc[8] = OV_C_DIM;
        ov_wkey(hint, 96, "ovl.hint_welcome");
        break;
    }
    case SC_MENU:
        if (sb_pause_requested()) { ov_wkey(tag, 48, "ovl.hdr_paused"); }
        if (krnl_savelock_active()) { /* W190 SAVE-LOCK: sichtbarer Hinweis, solange der Schutz aktiv ist */
            ov_wkey(ln[0], 128, "ovl.sl_note");
            lc[0] = OV_C_HEAD;
        }
        ov_wkey(hint, 96, "ovl.hint_menu");
        break;
    case SC_QUIT:
        ov_wkey(ln[0], 128, "ovl.q_quit");
        ov_wkey(hint, 96, "ovl.hint_quit");
        break;
    case SC_PAUSED:
        ov_wkey(tag, 48, "ovl.hdr_paused");
        ov_wkey(ln[0], 128, "ovl.p_info");
        lc[0] = OV_C_DIM;
        ov_wkey(hint, 96, "ovl.hint_paused");
        break;
    case SC_FBKIND:
        ov_wkey(ln[0], 128, "ovl.fb_kind_q");
        ov_wkey(ln[1], 128, "ovl.fb_nosend");
        if (g_fb_hot && g_fb_have) {
            if (g_fb_pkg.clip > 0) {
                char nb[160];
                _snprintf_s(nb, sizeof nb, _TRUNCATE, LT(g_fb_pkg.clip == 1 ? "ovl.clip_attached" : "ovl.clip_jpg"),
                            (double)g_fb_pkg.clip_bytes / 1000000.0);
                ov_wu8(ln[1], 128, nb);
            } else {
                ov_wkey(ln[1], 128, g_fb_pkg.clip < 0 ? "ovl.clip_fail" :
                        g_fb_pkg.shot_ok ? "ovl.fb_c_shot" : "ovl.fb_c_noshot");
            }
        }
        lc[1] = OV_C_DIM;
        ov_wkey(hint, 96, "ovl.hint_back");
        break;
    case SC_FBTEXT: {
        size_t L = (size_t)g_fb_len, maxc = 44;
        ov_wkey(tag, 48, g_fb_kind ? "ovl.fb_idea" : "ovl.fb_bug");
        ov_wkey(ln[0], 128, "ovl.fb_text_q");
        lc[0] = OV_C_DIM;
        {
            int start = L > maxc ? L - maxc : 0;
            if (g_fb_text[start] >= 0xDC00 && g_fb_text[start] <= 0xDFFF) { ++start; }
            _snwprintf_s(ln[1], 128, _TRUNCATE, L"> %s_", g_fb_text + start);
        }
        _snwprintf_s(ln[2], 128, _TRUNCATE, L"%d/160", g_fb_len);
        lc[2] = OV_C_DIM;
        ov_wkey(hint, 96, "ovl.hint_text");
        break;
    }
    case SC_FBREVIEW: {
        char nb[160];
        size_t dl = strlen(g_fb_pkg.dir_show);
        ov_wkey(tag, 48, g_fb_pkg.kind ? "ovl.fb_idea" : "ovl.fb_bug");
        ov_wkey(ln[0], 128, "ovl.fb_created");
        _snprintf_s(nb, sizeof nb, _TRUNCATE, "%s%s", dl > 46 ? "..." : "", g_fb_pkg.dir_show + (dl > 46 ? dl - 43 : 0));
        ov_wu8(ln[1], 128, nb);
        lc[1] = OV_C_DIM;
        ov_wkey(ln[2], 128, g_fb_pkg.shot_ok ? "ovl.fb_c_shot" : "ovl.fb_c_noshot");
        _snprintf_s(nb, sizeof nb, _TRUNCATE, LT("ovl.fb_c_log"), g_fb_pkg.log_lines);
        ov_wu8(ln[3], 128, nb);
        ov_wkey(ln[4], 128, "ovl.fb_c_info");
        ov_wkey(ln[5], 128, "ovl.fb_c_text");
        ov_wkey(ln[6], 128, "ovl.fb_c_not");
        lc[6] = OV_C_DIM;
        if (g_fb_pkg.clip > 0) {
            _snprintf_s(nb, sizeof nb, _TRUNCATE, LT(g_fb_pkg.clip == 1 ? "ovl.clip_attached" : "ovl.clip_jpg"),
                        (double)g_fb_pkg.clip_bytes / 1000000.0);
            ov_wu8(ln[7], 128, nb);
        } else { ov_wkey(ln[7], 128, g_fb_pkg.clip < 0 ? "ovl.clip_fail" : "ovl.clip_none"); }
        ov_wkey(hint, 96, "ovl.hint_back");
        break;
    }
    default:
        hint[0] = 0;
        break;
    }
    y = ov_frame_head(W, H, title, tag) + S(5.0f);
    for (k = 0; k < nl; k++) {
        if (g_ov_scr == SC_FBTEXT && k == 1) { /* Eingabefeld */
            ov_fill(pad, y - S(1.0f), W - pad, y + S(11.0f), OV_C_SEL);
            ov_fill(pad, y + S(11.0f) - b, W - pad, y + S(11.0f), OV_C_TXT);
        }
        ov_text(pad + S(2.0f), y, ln[k], lc[k], 0);
        y += S(11.0f);
    }
    if (nl) { y += S(5.0f); }
    g_rel_rows_y = y;
    g_rel_rowh = rowh;
    g_rel_nrows = nr;
    g_rel_valx = W;
    for (k = 0; k < nr; k++) {
        int sel = k == g_ov_msel, dis = g_ov_scr == SC_MENU && k == MI_PHOTO && !ov_photo_allowed(); /* W195 */
        int yt = y + (rowh - ov_font_px) / 2 - S(0.5f);
        if (sel) {
            ov_fill(b + S(2.0f), y, W - b - S(2.0f), y + rowh, OV_C_SEL);
            ov_fill(b + S(2.0f), y, b + S(4.0f), y + rowh, OV_C_TXT);
        }
        ov_wkey(lb, 64, rk[g_ov_scr][k]);
        ov_text(pad + S(6.0f), yt, lb, dis ? OV_C_DIM : OV_C_TXT, 0);
        if (dis) { /* W195: Fotomodus ausgegraut (VR bzw. photo_mode=0) */
            ov_wkey(vb, 64, g_cfg.vr_enable ? "ovl.ph_m_vr" : "ovl.v_off");
            ov_text(W - pad - S(2.0f), yt, vb, OV_C_DIM, 1);
        } else if (g_ov_scr == SC_MENU && k == MI_PHOTO && !sel && (ov_key_label(g_ov_photo_vk, g_ov_photo_mod, vb, 64), vb[0])) {
            ov_text(W - pad - S(2.0f), yt, vb, OV_C_DIM, 1); /* W195: Taste (W290: Shift+F12) */
        } else if (sel) {
            ov_text(W - pad - S(2.0f), yt, L"\x25C4", OV_C_TXT, 1);
        }
        y += rowh;
    }
    y += S(3.0f);
    ov_fill(pad, y, W - pad, y + b, OV_C_LINE);
    y += S(3.0f);
    ov_text(pad + S(2.0f), y, hint, OV_C_DIM, 0);
    y += S(11.0f);
    rst[0] = 0;
    ov_text(pad + S(2.0f), y, g_ov_status[0] ? g_ov_status : rst, OV_C_TXT, 0);
    GdiFlush();
    sb_ovl_ui_finish(ov_bits, ov_bw, ov_bh, g_ovf);
}

/* W195 PHOTO-MODE: Foto-Panel (Look der Einstellungen: Kopf, Tab-Zeile, Zeilen Bezeichnung/Wert, Fusszeilen) */
static void ov_paint_photo(void)
{
    static const char *const tk[PH_NTABS] = { "ovl.ph_tab_photo", "ovl.ph_tab_cam" };
    static const char *const lk[] = { "ovl.ph_view", "ovl.ph_freecam", "ovl.ph_mech", "ovl.ph_res", "ovl.ph_fmt",
                                      "ovl.ph_shot", "ovl.lb_back", "ovl.ph_yaw", "ovl.ph_pitch", "ovl.ph_dist",
                                      "ovl.ph_reset", "ovl.ph_aspect", "ovl.ph_monitor_fx", "ovl.ph_shader", "ovl.ph_hud" };
    int W = (int)ov_bw, H = (int)ov_bh, rowh = S(12.5f), pad = S(6.0f), b = S(1.0f), th = S(OV_TABH), gap = S(3.0f);
    int y, k, t, x, tw[PH_NTABS], sum = 0, foc = g_ph_sel < 0;
    wchar_t lb[80], vb[80], nm[PH_NTABS][32], tag[48];
    sb_photo_ui_t u;
    (void)H;
    sb_photo_get(&u);
    tag[0] = 0;
    ov_wkey(tag, 48, "ovl.hdr_paused");
    y = ov_frame_head(W, H, L"\x25A0 PHOTO", tag) + gap;
    /* Tab-Zeile */
    for (t = 0; t < PH_NTABS; t++) {
        SIZE sz;
        ov_wkey(nm[t], 32, tk[t]);
        GetTextExtentPoint32W(ov_dc, nm[t], (int)wcslen(nm[t]), &sz);
        tw[t] = sz.cx + 2 * S(4.0f);
        sum += tw[t];
    }
    {
        int avail = W - 2 * pad - (PH_NTABS - 1) * gap;
        if (avail > sum) { for (t = 0; t < PH_NTABS; t++) { tw[t] += (avail - sum) / PH_NTABS; } }
    }
    x = pad;
    g_rel_tab_y0 = y;
    g_rel_tab_y1 = y + th;
    for (t = 0; t < PH_NTABS; t++) {
        int act = t == g_ph_tab, ty = y + (th - ov_font_px) / 2 - S(0.5f);
        g_rel_tab_x[t] = x;
        if (act) {
            ov_fill(x, y, x + tw[t], y + th, foc ? OV_C_HEAD : OV_C_SEL);
            ov_fill(x, y + th - S(1.0f), x + tw[t], y + th, OV_C_TXT);
        } else {
            ov_fill(x, y + th - S(1.0f), x + tw[t], y + th, OV_C_DIM);
        }
        SetTextAlign(ov_dc, TA_CENTER | TA_TOP);
        SetTextColor(ov_dc, act ? (foc ? OV_C_BG : OV_C_TXT) : OV_C_DIM);
        TextOutW(ov_dc, x + tw[t] / 2, ty, nm[t], (int)wcslen(nm[t]));
        x += tw[t] + gap;
    }
    g_rel_tab_x[PH_NTABS] = x - gap;
    y += th + S(4.0f);
    g_rel_rows_y = y;
    g_rel_rowh = rowh;
    g_rel_nrows = ov_ph_nrows();
    g_rel_valx = W * 9 / 20;
    for (k = 0; k < ov_ph_nrows(); k++) {
        int it = ov_ph_row(k), sel = k == g_ph_sel, en = ov_ph_enabled(it, &u), act = it == PR_SHOT || it == PR_BACK || it == PR_RESET;
        int yt = y + (rowh - ov_font_px) / 2 - S(0.5f);
        COLORREF c = en ? OV_C_TXT : OV_C_DIM;
        if (sel) {
            ov_fill(b + S(2.0f), y, W - b - S(2.0f), y + rowh, OV_C_SEL);
            ov_fill(b + S(2.0f), y, b + S(4.0f), y + rowh, OV_C_TXT);
        }
        ov_wkey(lb, 80, lk[it]);
        if (it == PR_SHOT) { /* Taste anzeigen (W290: Schnell-Screenshot-Taste, im Fotomodus = Foto) */
            wchar_t kl[32];
            ov_key_label(g_ov_qs_vk, g_ov_qs_mod, kl, 32);
            if (!kl[0]) { ov_key_label(g_ov_photo_vk, g_ov_photo_mod, kl, 32); }
            if (kl[0]) {
                size_t L = wcslen(lb);
                _snwprintf_s(lb + L, 80 - L, _TRUNCATE, L" (%s)", kl);
            }
        }
        ov_text(pad + S(6.0f), yt, lb, act ? (en ? OV_C_TXT : OV_C_DIM) : c, 0);
        if (it == PR_ASPECT) { /* W290: breit ist experimentell (Kolben, bernstein wenn gewaehlt) */
            SIZE lsz;
            GetTextExtentPoint32W(ov_dc, lb, (int)wcslen(lb), &lsz);
            ov_exp_mark(pad + S(6.0f) + lsz.cx + S(4.0f), yt, u.aspect == 1 && en ? OV_C_EXP : OV_C_DIM);
        }
        vb[0] = 0;
        switch (it) {
        case PR_MFX: ov_wkey(vb, 80, u.monitor_fx ? "ovl.v_on" : "ovl.v_off"); break;
        case PR_HUD: ov_wkey(vb, 80, g_cfg.photo_hud ? "ovl.v_on" : "ovl.v_off"); break; /* W290 */
        case PR_SHADER: ov_wkey(vb, 80, u.shading ? "ovl.ph_original" : "ovl.v_off"); break;
        case PR_VIEW: {
            static const char *const vk3[3] = { "ovl.ph_v_game", "ovl.ph_v_flat", "ovl.ph_v_world" };
            ov_wkey(vb, 80, u.stepping ? "ovl.ph_v_step" : vk3[u.view >= 0 && u.view <= 2 ? u.view : 0]);
            break;
        }
        case PR_ASPECT: {
            static const char *const ak[3] = { "ovl.ph_asp_crop", "ovl.ph_asp_wide", "ovl.ph_asp_square" };
            ov_wkey(vb, 80, ak[u.aspect >= 0 && u.aspect <= 2 ? u.aspect : 2]);
            break;
        }
        case PR_FREECAM: ov_wkey(vb, 80, u.freecam ? "ovl.v_on" : "ovl.v_off"); break;
        case PR_MECH: ov_wkey(vb, 80, "ovl.ph_v_na"); break;
        case PR_RES: { /* W290: Fenster / 4K / 8K / xN mit Pixeln */
            unsigned w = u.sw[u.size], hh = u.sh[u.size];
            int sv = kSbPhotoSize[u.size];
            wchar_t nmv[32];
            if (sv == 1) { ov_wkey(nmv, 32, "ovl.ph_sz_window"); }
            else if (sv < 0) { _snwprintf_s(nmv, 32, _TRUNCATE, L"%dK", -sv); }
            else { _snwprintf_s(nmv, 32, _TRUNCATE, L"\u00D7%d", sv); }
            _snwprintf_s(vb, 80, _TRUNCATE, L"%s  %u\u00D7%u", nmv, w, hh);
            break;
        }
        case PR_FMT: wcscpy_s(vb, 80, u.jpg ? L"JPG" : L"PNG"); break;
        case PR_YAW: _snwprintf_s(vb, 80, _TRUNCATE, L"%+.0f\u00B0 / \u00B1%.0f\u00B0", (double)-u.yaw, (double)u.max_yaw); break;
        case PR_PITCH: _snwprintf_s(vb, 80, _TRUNCATE, L"%+.0f\u00B0 / \u00B1%.0f\u00B0", (double)u.pitch, (double)u.max_pitch); break;
        case PR_DIST: _snwprintf_s(vb, 80, _TRUNCATE, L"%.1f / %.0f", (double)u.dist, (double)u.max_dist); break;
        default: break;
        }
        if (vb[0]) {
            if (sel && en && it != PR_MECH) {
                wchar_t tt[96];
                _snwprintf_s(tt, 96, _TRUNCATE, L"\u25C4 %s \u25BA", vb);
                ov_text(W - pad - S(2.0f), yt, tt, OV_C_TXT, 1);
            } else {
                ov_text(W - pad - S(2.0f), yt, vb, c, 1);
            }
        }
        y += rowh;
        if (it == PR_ASPECT) { ov_fill(pad, y - b / 2, W - pad, y - b / 2 + b, OV_C_DIM); }
        if (it == PR_FMT || it == PR_RESET) { ov_fill(pad, y - b / 2, W - pad, y - b / 2 + b, OV_C_DIM); }
    }
    y += S(3.0f);
    ov_fill(pad, y, W - pad, y + b, OV_C_LINE);
    y += S(3.0f);
    {
        wchar_t l1[96], l2[96], l3[128], st[96];
        char nb[256], dshow[200];
        ov_wkey(l1, 96, "ovl.ph_hint1");
        ov_wkey(l2, 96, u.effect_pending ? "ovl.ph_effect_pending" : u.freecam ? "ovl.ph_hint_cam" : (u.rec ? "ovl.ph_hint_quick" : "ovl.ph_st_norec"));
        sb_photo_dir_show(dshow, sizeof dshow);
        _snprintf_s(nb, sizeof nb, _TRUNCATE, LT("ovl.ph_dir"), dshow);
        ov_wu8(l3, 128, nb);
        ov_text(pad + S(2.0f), y, l1, OV_C_DIM, 0);
        y += S(11.0f);
        ov_text(pad + S(2.0f), y, l2, OV_C_DIM, 0);
        y += S(11.0f);
        ov_text(pad + S(2.0f), y, l3, OV_C_DIM, 0);
        y += S(11.0f);
        st[0] = 0;
        if (u.busy) { ov_wkey(st, 96, "ovl.ph_st_busy"); }
        else if (u.stepping) { ov_wkey(st, 96, "ovl.ph_st_step"); } /* W290 */
        ov_text(pad + S(2.0f), y, g_ov_status[0] ? g_ov_status : st, OV_C_TXT, 0);
    }
    GdiFlush();
    sb_ovl_ui_finish(ov_bits, ov_bw, ov_bh, g_ovf);
}

static void ov_paint(void)
{
    int W = (int)ov_bw, H = (int)ov_bh, i, k, y, rowh = S(12.5f), pad = S(6.0f), b = S(1.0f), cl = S(8.0f);
    int head = S(20.0f), ty;
    wchar_t lb[64], vb[64];
    if (g_ov_scr == SC_PHOTO) { /* W195 */
        ov_paint_photo();
        return;
    }
    if (g_ov_scr != SC_SET) { /* W188: Menue-Bildschirme */
        ov_paint_menu();
        return;
    }
    (void)cl;
    (void)ty;
    { /* W291: Kopf rechts "Zurueck (ESC)" (Mausklick dort = zurueck) */
        wchar_t bk[48];
        ov_wkey(bk, 48, g_cfg.game_menu ? "ovl.lb_back" : "ovl.lb_close");
        head = ov_frame_head(W, H, L"\u25A0 SYS-CONFIG", bk);
    }
    g_rel_head = head;
    y = head + S(3.0f);
    { /* W291: Seitenleiste (sichtbare Gruppen, Kolben bei Abweichung in der Gruppe) */
        int sx0 = pad, sx1 = pad + S(OV_SIDE_W), sh = S(OV_SIDE_H), g, n = 0, foc = g_ov_sel < 0;
        g_rel_tab_x[0] = sx0;
        g_rel_tab_x[1] = sx1;
        g_rel_tab_y0 = y;
        g_rel_side_h = sh;
        for (g = 0; g < OG_N; g++) {
            int act = g == g_ov_page, yy = y + n * sh, yt2 = yy + (sh - ov_font_px) / 2 - S(0.5f), st = 0, k2;
            wchar_t nm[40];
            if (!ov_grp_vis(g)) { continue; }
            if (act) {
                ov_fill(sx0, yy, sx1 - S(3.0f), yy + sh, foc ? OV_C_HEAD : OV_C_SEL);
                ov_fill(sx0, yy, sx0 + S(2.0f), yy + sh, OV_C_TXT);
            }
            ov_wkey(nm, 40, kOvGrp[g].key);
            ov_text(sx0 + S(5.0f), yt2, nm, act ? (foc ? OV_C_BG : OV_C_TXT) : OV_C_DIM, 0);
            for (k2 = 0; k2 < kOvGrp[g].nmain && st < 2; k2++) {
                if (ov_row_avail(kOvGrp[g].main[k2]) && ov_exp_state(kOvGrp[g].main[k2]) == 2) { st = 2; }
            }
            for (k2 = 0; k2 < kOvGrp[g].nadv && st < 2; k2++) {
                if (ov_row_avail(kOvGrp[g].adv[k2]) && ov_exp_state(kOvGrp[g].adv[k2]) == 2) { st = 2; }
            }
            if (st == 2 && g != OG_QUICK) { ov_exp_mark(sx1 - S(16.0f), yt2, OV_C_EXP); }
            n++;
        }
        g_rel_tab_y1 = y + n * sh;
        ov_fill(sx1 - b, y, sx1, y + ov_set_body_h(), OV_C_DIM);
        g_rel_rows_x0 = sx1 + S(4.0f);
    }
    g_rel_rows_y = y;
    g_rel_rowh = rowh;
    g_rel_nrows = ov_nrows();
    g_rel_valx = g_rel_rows_x0 + (W - g_rel_rows_x0) / 2;
    {
        int rx = g_rel_rows_x0, ybody = y, sepdone = 0;
        for (k = 0; k < ov_nrows(); k++) {
            int en, sel = (k == g_ov_sel), act;
            COLORREF c;
            int yt;
            i = ov_row(k);
            act = i == OI_SAVE || i == OI_CLOSE || i == OI_RESET || i == OI_SLCOMMIT;
            en = ov_effective(i);
            c = en ? OV_C_TXT : OV_C_DIM;
            yt = y + (rowh - ov_font_px) / 2 - S(0.5f);
            if (i == OI_ADV || ((i == OI_RESET || i == OI_SAVE) && !sepdone)) { /* Trennlinie ueber Erweitert und den Aktionen */
                ov_fill(rx, y - b / 2, W - pad, y - b / 2 + b, OV_C_DIM);
                if (i != OI_ADV) { sepdone = 1; }
            }
            if (sel) {
                ov_fill(rx, y, W - b - S(2.0f), y + rowh, OV_C_SEL);
                ov_fill(rx, y, rx + S(2.0f), y + rowh, OV_C_TXT);
            }
            if (i == OI_ADV) { /* W291: "Erweitert > (n)" */
                wchar_t ad[40];
                ov_wkey(ad, 40, "ovl.lb_adv");
                _snwprintf_s(lb, 64, _TRUNCATE, L"%s %s (%d)", ad, g_ov_adv[g_ov_page] ? L"\u25BC" : L"\u25BA",
                             ov_grp_nadv(g_ov_page));
            } else if (i == OI_RESET) {
                ov_wkey(lb, 64, g_ov_page == OG_QUICK ? (g_ov_undo ? "ovl.lb_undo" : "ovl.lb_reset_all") :
                                g_ov_page == OG_DEBUG ? "ovl.lb_alloff" : "ovl.lb_reset_grp");
            } else if (i == OI_CLOSE && g_cfg.game_menu) { ov_wkey(lb, 64, "ovl.lb_back"); } /* W188: zurueck ins Spielmenue */
            else { ov_label(i, lb, 64); }
            ov_text(rx + S(4.0f), yt, lb, (act || i == OI_ADV) ? OV_C_TXT : c, 0);
            { /* W278/W287 Kolben, W291 Neustart-Marke hinter dem Text */
                int xs = ov_exp_state(i), rs = ov_rs_state(i), mx;
                SIZE lsz;
                GetTextExtentPoint32W(ov_dc, lb, (int)wcslen(lb), &lsz);
                mx = rx + S(4.0f) + lsz.cx + S(4.0f);
                if (xs) { mx += ov_exp_mark(mx, yt, xs == 2 ? OV_C_EXP : OV_C_DIM) + S(3.0f); }
                if (rs) { ov_rs_mark(mx, yt, rs == 2 ? OV_C_TXT : OV_C_DIM); }
            }
            if (!act && i != OI_ADV && i != OI_TEXINFO) { /* Aktionen und Info ohne Wert */
                ov_value(i, vb, 64);
                if (sel && ov_enabled(i) && !ov_row_info(i)) {
                    wchar_t t[80];
                    _snwprintf_s(t, 80, _TRUNCATE, L"\u25C4 %s \u25BA", vb);
                    ov_text(W - pad - S(2.0f), yt, t, OV_C_TXT, 1);
                } else {
                    ov_text(W - pad - S(2.0f), yt, vb, c, 1);
                }
            }
            y += rowh;
        }
        y = ybody + ov_set_body_h();
    }
    y += S(3.0f);
    ov_fill(pad, y, W - pad, y + b, OV_C_LINE);
    y += S(3.0f);
    {
        wchar_t foot[80], ctl[64], leg[64];
        ov_wkey(foot, 80, "ovl.foot_set");
        ov_text(pad + S(2.0f), y, foot, OV_C_DIM, 0);
        y += S(11.0f);
        if (g_ov_status[0]) { ov_text(pad + S(2.0f), y, g_ov_status, OV_C_TXT, 0); }
        y += S(11.0f);
        if (g_ov_page == OG_DEBUG) { /* W240: Hinweis "experimental, just for fun" */
            ov_wkey(leg, 64, g_cfg.vr_enable ? "ovl.xp_note_vr" : "ovl.xp_note");
            ov_text(pad + S(2.0f), y, leg, OV_C_DIM, 0);
        } else if (g_sb_paused && g_cfg.pause_live && ov_peek_vk() > 0) { /* W193: in der Pause Hinweis aufs Ausblenden */
            char pn[96];
            _snprintf_s(pn, sizeof pn, _TRUNCATE, LT("ovl.peek_note"), g_cfg.menu_peek_key);
            ov_wu8(leg, 64, pn);
            ov_text(pad + S(2.0f), y, leg, OV_C_DIM, 0);
        } else if (ov_exp_font()) { /* W287/W291: "<Kolben> experimentell ... <Neustart> nach Neustart" */
            int x = pad + S(2.0f);
            SIZE lsz;
            x += ov_exp_mark(x, y, OV_C_DIM) + S(2.0f);
            ov_wkey(leg, 64, "ovl.leg_exp");
            ov_text(x, y, leg, OV_C_DIM, 0);
            GetTextExtentPoint32W(ov_dc, leg, (int)wcslen(leg), &lsz);
            x += lsz.cx + S(8.0f);
            x += ov_rs_mark(x, y, OV_C_DIM) + S(2.0f);
            ov_wkey(leg, 64, "ovl.leg_rs");
            ov_text(x, y, leg, OV_C_DIM, 0);
        } else {
            ov_wkey(leg, 64, "ovl.leg_txt");
            ov_text(pad + S(2.0f), y, leg, OV_C_DIM, 0);
        }
        if (g_cfg.sbc_usb) {
            /* W128: Original-Controller-Zustand (sbc_usb=0: Zeile fehlt, Layout wie bisher) */
            int st = sbc_wire_status();
            y += S(11.0f);
            ov_wkey(ctl, 64, st == SBC_WIRE_USB_OK ? "ovl.ctl_usb" : st == SBC_WIRE_FIXTURE ? "ovl.ctl_fixture" :
                              st == SBC_WIRE_PENDING ? "ovl.ctl_search" :
                              st == SBC_WIRE_LOST ? "ovl.ctl_lost" : "ovl.ctl_none"); /* W305 */
            ov_text(pad + S(2.0f), y, ctl,
                    st == SBC_WIRE_USB_OK || st == SBC_WIRE_FIXTURE ? OV_C_TXT : OV_C_DIM, 0);
        }
    }
    GdiFlush();
    sb_ovl_ui_finish(ov_bits, ov_bw, ov_bh, g_ovf);
}

/* W122: Scanlines (jede 2. Zeile leicht dunkler, ab Faktor 2) +
 * vormultipliziertes Alpha (Deckung SB_UI_ALPHA) - gemeinsamer Look. */
void sb_ovl_ui_finish(unsigned *bits, unsigned w, unsigned h, float factor)
{
    unsigned n = w * h, px;
    for (px = 0; px < n; px++) {
        unsigned c = bits[px], r = (c >> 16) & 255u, g = (c >> 8) & 255u, bl = c & 255u;
        float a = OV_ALPHA, k = (((px / w) & 1u) && factor >= 2.0f) ? 0.86f : 1.0f;
        r = (unsigned)((float)r * a * k + 0.5f);
        g = (unsigned)((float)g * a * k + 0.5f);
        bl = (unsigned)((float)bl * a * k + 0.5f);
        bits[px] = ((unsigned)(a * 255.0f + 0.5f) << 24) | (r << 16) | (g << 8) | bl;
    }
}

/* ------------------------------------------------------------------ */
/* D3D11                                                               */
/* ------------------------------------------------------------------ */
typedef struct { float pos[2]; float org[2]; } ov_vtx;

static const char kOV_VS[] =
    "struct I { float2 p : POSITION; float2 o : TEXCOORD0; };\n"
    "struct O { float4 p : SV_Position; nointerpolation float2 o : TEXCOORD0; };\n"
    "O main(I i) { O r; r.p = float4(i.p, 0.0, 1.0); r.o = i.o; return r; }\n";
/* 1:1 Texel-Load ab der Panel-Ecke (kein Filter, nie gestreckt); Textur
 * ist vormultipliziert. */
static const char kOV_PS[] =
    "Texture2D t : register(t0);\n"
    "struct O { float4 p : SV_Position; nointerpolation float2 o : TEXCOORD0; };\n"
    "float4 main(O i) : SV_Target { return t.Load(int3(int2(i.p.xy - i.o), 0)); }\n";

static int ov_d3d_init(void)
{
    ID3DBlob *vsb = NULL, *psb = NULL;
    D3D11_INPUT_ELEMENT_DESC ly[2];
    D3D11_BUFFER_DESC bd;
    D3D11_BLEND_DESC bld;
    D3D11_RASTERIZER_DESC rd;
    D3D11_DEPTH_STENCIL_DESC dsd;
    HRESULT hr;
    if (ov_state) { return ov_state > 0; }
    ov_state = -1;
    if (!g_dev) { return 0; }
    if (!sb_im_compile(kOV_VS, "vs_4_0", &vsb) || !sb_im_compile(kOV_PS, "ps_4_0", &psb)) {
        HLOG_E("OVERLAY: Shader-Uebersetzung gescheitert -> Overlay unsichtbar");
        if (vsb) { vsb->lpVtbl->Release(vsb); }
        return 0;
    }
    hr = ID3D11Device_CreateVertexShader(g_dev, vsb->lpVtbl->GetBufferPointer(vsb),
                                         vsb->lpVtbl->GetBufferSize(vsb), NULL, &ov_vs);
    memset(ly, 0, sizeof ly);
    ly[0].SemanticName = "POSITION";
    ly[0].Format = DXGI_FORMAT_R32G32_FLOAT;
    ly[1].SemanticName = "TEXCOORD";
    ly[1].Format = DXGI_FORMAT_R32G32_FLOAT;
    ly[1].AlignedByteOffset = 8;
    if (SUCCEEDED(hr)) {
        hr = ID3D11Device_CreateInputLayout(g_dev, ly, 2, vsb->lpVtbl->GetBufferPointer(vsb),
                                            vsb->lpVtbl->GetBufferSize(vsb), &ov_lay);
    }
    vsb->lpVtbl->Release(vsb);
    if (SUCCEEDED(hr)) {
        hr = ID3D11Device_CreatePixelShader(g_dev, psb->lpVtbl->GetBufferPointer(psb),
                                            psb->lpVtbl->GetBufferSize(psb), NULL, &ov_ps);
    }
    psb->lpVtbl->Release(psb);
    if (SUCCEEDED(hr)) {
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = sizeof(ov_vtx) * 6;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &ov_vb);
    }
    if (SUCCEEDED(hr)) {
        memset(&bld, 0, sizeof bld);
        bld.RenderTarget[0].BlendEnable = TRUE;
        bld.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        bld.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
        bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED |
            D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
        hr = ID3D11Device_CreateBlendState(g_dev, &bld, &ov_bs);
    }
    if (SUCCEEDED(hr)) {
        memset(&rd, 0, sizeof rd);
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        hr = ID3D11Device_CreateRasterizerState(g_dev, &rd, &ov_rs);
    }
    if (SUCCEEDED(hr)) {
        memset(&dsd, 0, sizeof dsd);
        dsd.DepthEnable = FALSE;
        dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        dsd.DepthFunc = D3D11_COMPARISON_ALWAYS;
        hr = ID3D11Device_CreateDepthStencilState(g_dev, &dsd, &ov_ds);
    }
    if (FAILED(hr)) {
        HLOG_E("OVERLAY: D3D-Ressourcen gescheitert (hr=0x%08lX) -> Overlay unsichtbar", (unsigned long)hr);
        return 0;
    }
    ov_state = 1;
    return 1;
}

static int ov_tex_upload(void)
{
    D3D11_MAPPED_SUBRESOURCE map;
    unsigned y;
    if (!ov_tex || ov_tw != ov_bw || ov_th != ov_bh) {
        D3D11_TEXTURE2D_DESC td;
        if (ov_srv) { ID3D11ShaderResourceView_Release(ov_srv); ov_srv = NULL; }
        if (ov_tex) { ID3D11Texture2D_Release(ov_tex); ov_tex = NULL; }
        memset(&td, 0, sizeof td);
        td.Width = ov_bw;
        td.Height = ov_bh;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &ov_tex)) ||
            FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)ov_tex, NULL, &ov_srv))) {
            HLOG_W("OVERLAY: Panel-Textur %ux%u gescheitert", ov_bw, ov_bh);
            return 0;
        }
        ov_tw = ov_bw;
        ov_th = ov_bh;
    }
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)ov_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        return 0;
    }
    for (y = 0; y < ov_bh; y++) {
        memcpy((unsigned char *)map.pData + (size_t)y * map.RowPitch, ov_bits + (size_t)y * ov_bw, ov_bw * 4u);
    }
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)ov_tex, 0);
    return 1;
}

/* Bereich (x,y,w,h) des Ziels sichern (nur Spiel-Backbuffer ohne Interp).
 * W122: gemeinsame API (sb_overlay.h), auch fuer das Leistungs-Overlay. */
void sb_ovl_save_take(sb_ovl_save_t *s, ID3D11RenderTargetView *rtv, unsigned x, unsigned y, unsigned w,
                      unsigned h)
{
    ID3D11Resource *res = NULL;
    D3D11_TEXTURE2D_DESC dd;
    D3D11_BOX bx;
    s->saved = 0;
    ID3D11RenderTargetView_GetResource(rtv, &res);
    if (!res) { return; }
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)res, &dd);
    if (!s->tex || s->w != w || s->h != h || s->fmt != (int)dd.Format) {
        if (s->tex) { ID3D11Texture2D_Release(s->tex); s->tex = NULL; }
        dd.Width = w;
        dd.Height = h;
        dd.MipLevels = 1;
        dd.ArraySize = 1;
        dd.SampleDesc.Count = 1;
        dd.SampleDesc.Quality = 0;
        dd.BindFlags = 0;
        dd.CPUAccessFlags = 0;
        dd.MiscFlags = 0;
        dd.Usage = D3D11_USAGE_DEFAULT;
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &dd, NULL, &s->tex))) {
            s->tex = NULL;
            ID3D11Resource_Release(res);
            return;
        }
        s->w = w;
        s->h = h;
        s->fmt = (int)dd.Format;
    }
    bx.left = x; bx.top = y; bx.front = 0;
    bx.right = x + w; bx.bottom = y + h; bx.back = 1;
    ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)s->tex, 0, 0, 0, 0, res, 0, &bx);
    if (s->dst) { ID3D11Resource_Release(s->dst); }
    s->dst = res; /* behaelt die GetResource-Referenz */
    s->x = x;
    s->y = y;
    s->saved = 1;
}

void sb_ovl_save_put(sb_ovl_save_t *s)
{
    if (!s->saved) { return; }
    s->saved = 0;
    if (s->dst && s->tex) {
        ID3D11DeviceContext_CopySubresourceRegion(g_ctx, s->dst, 0, s->x, s->y, 0, (ID3D11Resource *)s->tex, 0,
                                                  NULL);
    }
    if (s->dst) { ID3D11Resource_Release(s->dst); s->dst = NULL; }
}

void sb_ovl_restore(void)
{
    sb_ovl_save_put(&ov_bbsave);
}

/* W122: vormultiplizierte BGRA-Textur srv (w x h) 1:1 bei (x0,y0) auf rtv
 * zeichnen (Texel-Load, nie gestreckt), Kontextzustand gesichert/zurueck.
 * Unter ctx_lock. 1 = gezeichnet. Gemeinsame API (sb_overlay.h). */
int sb_ovl_quad(ID3D11RenderTargetView *rtv, ID3D11ShaderResourceView *srv, unsigned x0, unsigned y0,
                unsigned pw, unsigned ph)
{
    unsigned r;
    ov_vtx v[6];
    D3D11_MAPPED_SUBRESOURCE map;
    D3D11_VIEWPORT vp;
    UINT stride = sizeof(ov_vtx), off = 0;
    ID3D11RenderTargetView *s_rtv = NULL;
    ID3D11DepthStencilView *s_dsv = NULL;
    ID3D11BlendState *s_bs = NULL;
    FLOAT s_bf[4];
    UINT s_mask = 0, s_sref = 0, s_nvp = 1, s_vbs = 0, s_vbo = 0;
    ID3D11DepthStencilState *s_ds = NULL;
    ID3D11RasterizerState *s_rs = NULL;
    D3D11_VIEWPORT s_vp;
    ID3D11InputLayout *s_lay = NULL;
    ID3D11Buffer *s_vb = NULL;
    D3D11_PRIMITIVE_TOPOLOGY s_topo;
    ID3D11VertexShader *s_vs = NULL;
    ID3D11PixelShader *s_ps = NULL;
    ID3D11ShaderResourceView *s_srv = NULL;
    if (!rtv || !srv || !g_ctx || !ov_d3d_init()) { return 0; }
    for (r = 0; r < 6; r++) {
        static const int cx[6] = { 0, 1, 1, 0, 1, 0 };
        static const int cy[6] = { 0, 0, 1, 0, 1, 1 };
        float px = (float)(x0 + (cx[r] ? pw : 0)), py = (float)(y0 + (cy[r] ? ph : 0));
        v[r].pos[0] = px / (float)g_bb_w * 2.0f - 1.0f;
        v[r].pos[1] = 1.0f - py / (float)g_bb_h * 2.0f;
        v[r].org[0] = (float)x0;
        v[r].org[1] = (float)y0;
    }
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)ov_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        return 0;
    }
    memcpy(map.pData, v, sizeof v);
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)ov_vb, 0);

    ID3D11DeviceContext_OMGetRenderTargets(g_ctx, 1, &s_rtv, &s_dsv);
    ID3D11DeviceContext_OMGetBlendState(g_ctx, &s_bs, s_bf, &s_mask);
    ID3D11DeviceContext_OMGetDepthStencilState(g_ctx, &s_ds, &s_sref);
    ID3D11DeviceContext_RSGetState(g_ctx, &s_rs);
    ID3D11DeviceContext_RSGetViewports(g_ctx, &s_nvp, &s_vp);
    ID3D11DeviceContext_IAGetInputLayout(g_ctx, &s_lay);
    ID3D11DeviceContext_IAGetVertexBuffers(g_ctx, 0, 1, &s_vb, &s_vbs, &s_vbo);
    ID3D11DeviceContext_IAGetPrimitiveTopology(g_ctx, &s_topo);
    ID3D11DeviceContext_VSGetShader(g_ctx, &s_vs, NULL, NULL);
    ID3D11DeviceContext_PSGetShader(g_ctx, &s_ps, NULL, NULL);
    ID3D11DeviceContext_PSGetShaderResources(g_ctx, 0, 1, &s_srv);

    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width = (float)g_bb_w;
    vp.Height = (float)g_bb_h;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_OMSetBlendState(g_ctx, ov_bs, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, ov_ds, 0);
    ID3D11DeviceContext_RSSetState(g_ctx, ov_rs);
    ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &vp);
    ID3D11DeviceContext_IASetInputLayout(g_ctx, ov_lay);
    ID3D11DeviceContext_IASetVertexBuffers(g_ctx, 0, 1, &ov_vb, &stride, &off);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(g_ctx, ov_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_ctx, ov_ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, &srv);
    ID3D11DeviceContext_Draw(g_ctx, 6, 0);

    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, &s_srv);
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &s_rtv, s_dsv);
    ID3D11DeviceContext_OMSetBlendState(g_ctx, s_bs, s_bf, s_mask);
    ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, s_ds, s_sref);
    ID3D11DeviceContext_RSSetState(g_ctx, s_rs);
    if (s_nvp) { ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &s_vp); }
    ID3D11DeviceContext_IASetInputLayout(g_ctx, s_lay);
    ID3D11DeviceContext_IASetVertexBuffers(g_ctx, 0, 1, &s_vb, &s_vbs, &s_vbo);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, s_topo);
    ID3D11DeviceContext_VSSetShader(g_ctx, s_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_ctx, s_ps, NULL, 0);
    if (s_srv) { ID3D11ShaderResourceView_Release(s_srv); }
    if (s_rtv) { ID3D11RenderTargetView_Release(s_rtv); }
    if (s_dsv) { ID3D11DepthStencilView_Release(s_dsv); }
    if (s_bs) { ID3D11BlendState_Release(s_bs); }
    if (s_ds) { ID3D11DepthStencilState_Release(s_ds); }
    if (s_rs) { ID3D11RasterizerState_Release(s_rs); }
    if (s_lay) { ID3D11InputLayout_Release(s_lay); }
    if (s_vb) { ID3D11Buffer_Release(s_vb); }
    if (s_vs) { ID3D11VertexShader_Release(s_vs); }
    if (s_ps) { ID3D11PixelShader_Release(s_ps); }
    return 1;
}

/* ------------------------------------------------------------------ */
/* W197 VR-MENU: Panel in eigener Aufloesung fuer die VR-Flaeche        */
/* ------------------------------------------------------------------ */
/* Gemeinsame Textur fuer VR-Panels (auch sb_perfhud.c): DYNAMIC hochladen,
 * in eine DEFAULT-Textur mit Mip-Kette kopieren, Mips bilden (vormultipliziert
 * -> Box-Filter korrekt). Unter ctx_lock. */
int sb_ovl_vrtex_upload(sb_ovl_vrtex_t *t, const unsigned *bits, unsigned w, unsigned h)
{
    D3D11_MAPPED_SUBRESOURCE map;
    unsigned y;
    if (!g_dev || !g_ctx || !t || !bits || !w || !h) { return 0; }
    if (!t->tex || !t->up || !t->srv || t->w != w || t->h != h) {
        D3D11_TEXTURE2D_DESC td;
        if (t->srv) { ID3D11ShaderResourceView_Release(t->srv); t->srv = NULL; }
        if (t->tex) { ID3D11Texture2D_Release(t->tex); t->tex = NULL; }
        if (t->up) { ID3D11Texture2D_Release(t->up); t->up = NULL; }
        t->w = t->h = 0;
        memset(&td, 0, sizeof td);
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &t->up))) { t->up = NULL; return 0; }
        td.MipLevels = 0; /* volle Kette */
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        td.CPUAccessFlags = 0;
        td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &t->tex)) ||
            FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)t->tex, NULL, &t->srv))) {
            static unsigned nw;
            if (nw++ < 2u) { HLOG_W("VR-MENU: Panel-Textur %ux%u gescheitert", w, h); }
            if (t->tex) { ID3D11Texture2D_Release(t->tex); t->tex = NULL; }
            ID3D11Texture2D_Release(t->up); t->up = NULL;
            t->srv = NULL;
            return 0;
        }
        t->w = w;
        t->h = h;
    }
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)t->up, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) { return 0; }
    for (y = 0; y < h; y++) {
        memcpy((unsigned char *)map.pData + (size_t)y * map.RowPitch, bits + (size_t)y * w, w * 4u);
    }
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)t->up, 0);
    ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)t->tex, 0, 0, 0, 0, (ID3D11Resource *)t->up, 0, NULL);
    ID3D11DeviceContext_GenerateMips(g_ctx, t->srv);
    return 1;
}

/* Zweiter GDI-Zeichenkontext (VR-Aufloesung): Tausch mit den Desktop-Globalen,
 * damit ov_layout/ov_gdi_size/ov_paint unveraendert in beiden Groessen zeichnen. */
typedef struct {
    HDC dc;
    HBITMAP bmp, bmp_old;
    unsigned *bits;
    unsigned bw, bh;
    HFONT font, font_old;
    int font_px;
    float f;
} ov_gdi_t;
static ov_gdi_t ov_vrg = { NULL, NULL, NULL, NULL, 0, 0, NULL, NULL, 0, 1.0f };
static sb_ovl_vrtex_t ov_vrt;
static int ov_vr_scr = -1, ov_vr_rows_y, ov_vr_rowh, ov_vr_nrows;
static float ov_vr_f;
static long long ov_vr_q;

static void ov_gdi_swap(ov_gdi_t *o)
{
    HDC dc = ov_dc;
    HBITMAP b = ov_bmp, bo = ov_bmp_old;
    unsigned *bits = ov_bits;
    unsigned bw = ov_bw, bh = ov_bh;
    HFONT fo = ov_font, foo = ov_font_old;
    int fp = ov_font_px;
    float f = g_ovf;
    ov_dc = o->dc; ov_bmp = o->bmp; ov_bmp_old = o->bmp_old; ov_bits = o->bits; ov_bw = o->bw; ov_bh = o->bh;
    ov_font = o->font; ov_font_old = o->font_old; ov_font_px = o->font_px; g_ovf = o->f;
    o->dc = dc; o->bmp = b; o->bmp_old = bo; o->bits = bits; o->bw = bw; o->bh = bh;
    o->font = fo; o->font_old = foo; o->font_px = fp; o->f = f;
}

/* Render-Thread (sb_ovl_draw, unter ctx_lock): VR-Panel neu zeichnen, wenn sich
 * das Menue aenderte (force = g_ov_dirty), der Bildschirm wechselte oder spaetestens
 * alle 500 ms. Trefferflaechen (g_rel_*) des Desktop-Panels bleiben unberuehrt. */
static void ov_vr_paint(int force)
{
    int rel[9], rtx[OV_NPAGES + 1], t;
    unsigned pw, ph, boxh = (unsigned)(g_cfg.vr_menu_px > 0 ? g_cfg.vr_menu_px : 1620);
    long long now = rq_qpc();
    LARGE_INTEGER qf;
    QueryPerformanceFrequency(&qf);
    if (boxh < 540u) { boxh = 540u; } else if (boxh > 4320u) { boxh = 4320u; }
    if (!force && ov_vrt.srv && ov_vr_scr == g_ov_scr && now - ov_vr_q < qf.QuadPart / 2) { return; }
    rel[0] = g_rel_rows_y; rel[1] = g_rel_rowh; rel[2] = g_rel_nrows; rel[3] = g_rel_valx; rel[4] = g_rel_tab_y0; rel[5] = g_rel_tab_y1;
    rel[6] = g_rel_side_h; rel[7] = g_rel_rows_x0; rel[8] = g_rel_head; /* W291 */
    for (t = 0; t <= OV_NPAGES; t++) { rtx[t] = g_rel_tab_x[t]; }
    ov_gdi_swap(&ov_vrg);
    ov_layout(boxh, &pw, &ph);
    if (ov_gdi_size(pw, ph)) {
        ov_paint();
        if (sb_ovl_vrtex_upload(&ov_vrt, ov_bits, ov_bw, ov_bh)) {
            if (ov_vr_scr != g_ov_scr) {
                static unsigned nlog;
                if (nlog++ < 40u) {
                    HLOG_I("VR-MENU: Panel Bildschirm %d %ux%u Texel (Faktor %.2f, Schrift %d px)", g_ov_scr, ov_bw, ov_bh,
                           (double)g_ovf, ov_font_px);
                }
            }
            ov_vr_scr = g_ov_scr;
            ov_vr_rows_y = g_rel_rows_y;
            ov_vr_rowh = g_rel_rowh;
            ov_vr_nrows = g_rel_nrows;
            ov_vr_f = g_ovf;
        }
    }
    ov_gdi_swap(&ov_vrg);
    g_rel_rows_y = rel[0]; g_rel_rowh = rel[1]; g_rel_nrows = rel[2]; g_rel_valx = rel[3]; g_rel_tab_y0 = rel[4]; g_rel_tab_y1 = rel[5];
    g_rel_side_h = rel[6]; g_rel_rows_x0 = rel[7]; g_rel_head = rel[8]; /* W291 */
    for (t = 0; t <= OV_NPAGES; t++) { g_rel_tab_x[t] = rtx[t]; }
    ov_vr_q = now;
}

/* VR-Thread (unter ctx_lock): Panel + Mausposition auf dem Panel. Die Maus bedient
 * weiter die Desktop-Trefferflaechen (Fenster); ihre Lage relativ zum Desktop-Panel
 * wird abschnittsweise (Kopf, Zeilen, Fuss) auf das VR-Panel abgebildet, so dass der
 * Zeiger in der Brille genau auf der Zeile steht, die ein Klick trifft. */
int sb_ovl_vr_panel(sb_ovl_vr_t *o)
{
    int mx = g_ov_mx, my = g_ov_my;
    memset(o, 0, sizeof *o);
    o->open = (g_cfg.overlay && g_ov_open) ? 1 : 0;
    o->screen = g_ov_scr;
    if (!o->open || g_ov_peek || g_ov_req >= 0 || !ov_vrt.srv || ov_vr_scr != g_ov_scr || ov_vr_f <= 0.0f) { return 0; } /* g_ov_req: Bildschirm beim Oeffnen noch nicht gesetzt */
    o->srv = ov_vrt.srv;
    o->tw = ov_vrt.w;
    o->th = ov_vrt.h;
    o->uw = (float)ov_vrt.w / ov_vr_f;
    o->uh = (float)ov_vrt.h / ov_vr_f;
    if (mx >= 0 && my >= 0 && g_hit_w > 0 && g_hit_h > 0 && (unsigned)mx < g_bb_w && (unsigned)my < g_bb_h) {
        float dx = (float)(mx - g_hit_x0), dy = (float)(my - g_hit_y0), yv;
        float ryd = (float)(g_hit_rows_y - g_hit_y0), red = ryd + (float)(g_hit_nrows * g_hit_rowh), hd = (float)g_hit_h;
        float ryv = (float)ov_vr_rows_y, rev = ryv + (float)(ov_vr_nrows * ov_vr_rowh), hv = (float)ov_vrt.h;
        if (g_hit_rowh > 0 && ov_vr_rowh > 0 && ryd > 0.0f && hd > red && hv > rev && g_hit_nrows == ov_vr_nrows) {
            if (dy < ryd) { yv = dy * ryv / ryd; }
            else if (dy < red) { yv = ryv + (dy - ryd) * (float)ov_vr_rowh / (float)g_hit_rowh; }
            else { yv = rev + (dy - red) * (hv - rev) / (hd - red); }
        } else {
            yv = dy * hv / hd;
        }
        o->cu = dx / (float)g_hit_w;
        o->cv = yv / hv;
        o->cursor = 1;
    }
    return 1;
}

void sb_ovl_draw(ID3D11RenderTargetView *rtv, int save)
{
    unsigned bx, by, bw, bh, pw, ph, x0, y0;
    static unsigned lk[4];
    if (sb_clip_active() && rtv && g_ctx) {
        wchar_t rec[64];
        g_ovf = 1.5f;
        if (!ov_d3d_init() || !ov_gdi_size(152, 32)) { return; }
        ov_fill(0, 0, 152, 32, RGB(24, 8, 8));
        if (sb_clip_seconds()) { _snwprintf_s(rec, 64, _TRUNCATE, L"\x25CF REC %d", sb_clip_seconds()); }
        else { ov_wkey(rec, 64, "ovl.clip_finishing"); }
        ov_text(8, 5, rec, RGB(255, 100, 100), 0);
        sb_ovl_ui_finish(ov_bits, 152, 32, 1.5f);
        if (ov_tex_upload()) {
            if (save) { sb_ovl_save_take(&ov_bbsave, rtv, 16, 16, 152, 32); }
            sb_ovl_quad(rtv, ov_srv, 16, 16, 152, 32);
        }
        g_ov_dirty = 1;
        return;
    }
    if (!g_cfg.overlay || !g_ov_open || !rtv || !g_ctx) { return; }
    if (g_ov_peek) { return; } /* W193: Menue ausgeblendet (Taste gehalten), bleibt offen */
    if (!ov_d3d_init()) { return; }
    if (sb_vr_panels_on()) { ov_vr_paint(g_ov_dirty); } /* W197 VR-MENU: eigene Textur fuer die Flaeche in der Brille */
    sb_aspect_box(&bx, &by, &bw, &bh);
    ov_layout(bh, &pw, &ph);
    if (pw + 8u > bw || ph + 8u > bh) { /* winziges Fenster: auf die Box begrenzen (Faktor 1) */
        if (pw > bw || ph > bh) { return; }
    }
    if (!ov_gdi_size(pw, ph)) { return; }
    if (g_ov_dirty || ov_tw != pw || ov_th != ph) {
        ov_paint();
        if (!ov_tex_upload()) { return; }
        g_ov_dirty = 0;
    }
    if (g_ov_scr != SC_SET && g_ov_scr != SC_PHOTO) { /* W188: Menue-Bildschirme mittig in der 4:3-Box */
        x0 = bx + (bw - pw) / 2;
        y0 = by + (bh - ph) / 2;
    } else { /* Einstellungen, W195 Foto-Panel: oben links (Bild bleibt frei) */
        x0 = bx + (unsigned)S(10.0f);
        y0 = by + (unsigned)S(10.0f);
    }
    if (x0 + pw > bx + bw) { x0 = bx; }
    if (y0 + ph > by + bh) { y0 = by; }
    { /* W188: Trefferflaechen fuer die Maus (Backbuffer-Pixel) */
        int t;
        g_hit_x0 = (int)x0;
        g_hit_y0 = (int)y0;
        g_hit_w = (int)pw;
        g_hit_h = (int)ph;
        g_hit_rows_y = (int)y0 + g_rel_rows_y;
        g_hit_rowh = g_rel_rowh;
        g_hit_nrows = g_rel_nrows;
        g_hit_valx = (int)x0 + g_rel_valx;
        g_hit_tab_y0 = (g_ov_scr == SC_SET || g_ov_scr == SC_PHOTO) ? (int)y0 + g_rel_tab_y0 : -1;
        g_hit_tab_y1 = (g_ov_scr == SC_SET || g_ov_scr == SC_PHOTO) ? (int)y0 + g_rel_tab_y1 : -1;
        for (t = 0; t <= OV_NPAGES; t++) { g_hit_tab_x[t] = (int)x0 + g_rel_tab_x[t]; }
        g_hit_side_h = g_ov_scr == SC_SET ? g_rel_side_h : 0; /* W291 */
        g_hit_rows_x0 = (int)x0 + (g_ov_scr == SC_SET ? g_rel_rows_x0 : 0);
        g_hit_head_y1 = g_ov_scr == SC_SET ? (int)y0 + g_rel_head : -1;
    }
    if (lk[0] != x0 || lk[1] != y0 || lk[2] != pw || lk[3] != ph) {
        lk[0] = x0; lk[1] = y0; lk[2] = pw; lk[3] = ph;
        HLOG_I("OVERLAY panel %ux%u @%u,%u in Box %ux%u@%u,%u (bb %ux%u, Faktor %.2f, Schrift %d px)", pw, ph, x0,
               y0, bw, bh, bx, by, g_bb_w, g_bb_h, (double)g_ovf, ov_font_px);
    }
    if (save) { sb_ovl_save_take(&ov_bbsave, rtv, x0, y0, pw, ph); }
    sb_ovl_quad(rtv, ov_srv, x0, y0, pw, ph);
}

/* W254 REPLAY-LAUNCHER: Hinweis mit bis zu 6 Zeilen (UTF-8, uebersetzt) im
 * Stil des Alpha-Hinweises (beliebige Taste schliesst, Spiel pausiert). Oeffnet
 * beim naechsten Bild, sobald kein anderes Menue offen ist. */
void sb_ovl_notice(const char *const *lines, int n)
{
    sb_ovl_notice_ex("ovl.ra_title", "ovl.ra_tag", lines, n);
}

/* W272 ALPHA-TEST: wie sb_ovl_notice, mit eigenem Titel/Kennzeichen (Sprachschluessel). */
void sb_ovl_notice_ex(const char *title_key, const char *tag_key, const char *const *lines, int n)
{
    int j;
    if (!g_cfg.overlay) {
        return;
    }
    g_ov_notice_tkey = title_key ? title_key : "ovl.ra_title";
    g_ov_notice_gkey = tag_key ? tag_key : "ovl.ra_tag";
    for (j = 0; j < 6; j++) {
        strcpy_s(g_ov_notice[j], sizeof g_ov_notice[j], (j < n && lines[j]) ? lines[j] : "");
    }
    InterlockedExchange(&g_ov_notice_req, 1);
}
