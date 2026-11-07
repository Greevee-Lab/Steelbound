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
#include "render_qa.h"
#include "sb_lang.h" /* W142 L10N */
#include "sb_overlay.h"
#include "sb_perfhud.h" /* W122: Test-Aktion PHUDPOS */
#include "sb_remaster.h"
#include "input_mouse.h"
#include "sbc_wire.h" /* W128: Statuszeile Original-Controller */
#include "render_fx_rem.h" /* W133 REM-FX: Seite "Effekte" */
#include "vr.h" /* W134 VR-PROTO: Seite "VR" */
#include "sb_pause.h" /* W188 GAME-MENU: Pause */
#include "sb_feedback.h" /* W188 GAME-MENU: Feedback-Paket */
#include "input_map.h" /* W188 GAME-MENU: Gamepad im Menue (imap_pad_poll) */
#include "kernel_hle.h" /* W190 SAVE-LOCK: krnl_savelock_* */
#include <stdlib.h>
#include <wchar.h>

/* ---- Modulgrenzen (andere Dateien) ---- */
int sb_render_offscreen(void);                 /* d3d11_render.c: 1 = FPS-INTERP-Offscreen-BB */
int sb_render_views_rebuild(const char *why);  /* d3d11_render.c: Views neu (interp_fps-Wechsel) */
HWND sb_render_hwnd(void);                     /* d3d11_render.c */
int imouse_subclassed(void);                   /* input_mouse.c */
void imouse_release_keys(void);                /* input_mouse.c (UI-Thread) */
void imouse_menu_capture(int open);            /* input_mouse.c (UI-Thread, W188): Maus frei im Menue */
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
    OI_N
};
/* Zeilen je Seite (Seite 0 = W118 unveraendert, Testskripte zaehlen DOWN;
 * W173: Grafik-Preset als letzte Inhaltszeile, die Zeilen davor bleiben) */
static const int kOvPage0[] = { OI_FPS, OI_SHADING, OI_MFX, OI_GRAIN, OI_GLITCH, OI_EDGE, OI_EDGEW, OI_EDGES,
                                OI_MMODE, OI_MSENS, OI_HD, OI_HDSCALE, OI_WINMODE, OI_RES, OI_PRESET, OI_MPAUSE,
                                OI_SAVELOCK, OI_SLCOMMIT, /* W190 */
                                OI_SAVE, OI_CLOSE }; /* W188: + ESC-Menue pausiert */
static const int kOvPage1[] = { OI_SHADING, OI_MFX, OI_GRAIN, OI_GLITCH,
                                OI_EXPO, OI_TONE, OI_WGAM, OI_WSHARP, OI_FOCUS, OI_WCON, OI_FOG, OI_LWORLD, OI_RTS,
                                OI_BLOOM, OI_BTHR,
                                OI_LCKP, OI_CGLOW, OI_CSPILL, OI_CLIGHT,
                                OI_ABCMP, OI_SAVE, OI_CLOSE };
static const int kOvPage2[] = { OI_PRESET, OI_AA, OI_SSAO, OI_CSAT, OI_CTEMP, OI_CLIFT, OI_CGAM, OI_CGAIN,
                                OI_SHADOW, OI_SHSOFT, OI_VQASYNC, OI_LABEL, OI_ABCMP, OI_SAVE, OI_CLOSE };
#define OV_NP2 ((int)(sizeof kOvPage2 / sizeof kOvPage2[0]))
/* W133 REM-FX: Seite "Effekte" (Index 3, nach "Shader 2") */
#define OV_PAGE_FX 3
#define OV_PAGE_VR 4 /* W134 VR-PROTO: Seite "VR" (nach "Effekte") */
#define OV_NPAGES 5
static const int kOvPageFx[] = { OI_FXPRESET, OI_FXSOFT, OI_FXEMB, OI_FXLIGHT, OI_FXLCKP, OI_FXIP, OI_FXCMP,
                                 OI_SAVE, OI_CLOSE };
#define OV_NPFX ((int)(sizeof kOvPageFx / sizeof kOvPageFx[0]))
static const int kOvPageVr[] = { OI_VRON, OI_VRREC, OI_VRSEAT, OI_VRFWD, OI_VRSCALE, OI_VRIPD, OI_VRPREV,
                                 OI_VRPRESET, OI_VRRES, OI_VRHZ, OI_VRMIRROR, OI_VRWORLD, OI_VRSHADE, OI_SAVE, OI_CLOSE };
/* W183: Stufen der Render-Aufloesung (% der Runtime-Empfehlung) und Bildraten der Brille (0 = Runtime) */
static const int kOvVrRes[] = { 50, 60, 65, 70, 80, 85, 90, 100, 110, 120 };
static const int kOvVrHz[] = { 0, 72, 80, 90, 120 };
#define OV_NVRRES ((int)(sizeof kOvVrRes / sizeof kOvVrRes[0]))
#define OV_NVRHZ ((int)(sizeof kOvVrHz / sizeof kOvVrHz[0]))
#define OV_NPVR ((int)(sizeof kOvPageVr / sizeof kOvPageVr[0]))
static int g_ov_vr = -1;                /* W134: gewaehltes vr_enable (wirkt nach Neustart), -1 = noch nicht gelesen */
static int g_ov_vrprev = -1;            /* W134: gewaehlte Vorschau 0 sbs, 1 left, 2 right, 3 off (nach Neustart) */
#define OV_NP0 ((int)(sizeof kOvPage0 / sizeof kOvPage0[0]))
#define OV_NP1 ((int)(sizeof kOvPage1 / sizeof kOvPage1[0]))
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
static int g_ov_fps;                    /* gewaehlte Bildrate (0/60/120) */
static int g_ov_fps_pend;               /* 1 = Wechsel am Present-Ende faellig */
static int g_ov_winmode;                /* Darstellung (nach Neustart) */
static int g_ov_label0;                  /* W153: hud_label_avoid beim Start ("*" bei Abweichung) */
static int g_ov_avail;                  /* HD-Paket: 1 = x2, 2 = x4 */
static int g_ov_dirty = 1;              /* Panel neu zeichnen */
static ULONGLONG g_ov_sl_arm;           /* W190: "Uebernehmen" scharf bis (zweites ENTER bestaetigt) */
/* ---- W188 GAME-MENU: Bildschirme ----
 * SC_SET = Einstellungen (W118-Seiten), SC_MENU = Spielmenue (erste Ebene),
 * SC_QUIT = Rueckfrage Beenden, SC_PAUSED = Pause-Taste, SC_FB* = Feedback. */
enum { SC_SET = 0, SC_MENU, SC_QUIT, SC_PAUSED, SC_FBKIND, SC_FBTEXT, SC_FBREVIEW };
enum { MI_CONTINUE = 0, MI_SETTINGS, MI_PHOTO, MI_FEEDBACK, MI_QUIT, MI_N };
/* interne Tasten im Ring (ueber dem VK-Bereich) */
#define OK_BTAB    0x1001  /* Umschalt+Tab / LB: Seite zurueck */
#define OK_PAUSE   0x1002  /* Pause-Taste (Belegung) */
#define OK_WUP     0x1003  /* Mausrad */
#define OK_WDN     0x1004
#define OK_PSTART  0x1005  /* Pad START */
#define OK_CHAR    0x20000 /* | Zeichen (UTF-16), Texteingabe */
#define OK_CLICK   0x40000000 /* | x << 15 | y (Backbuffer-Pixel) */
#define OV_MSG_TEST   (WM_APP + 0x1A8) /* TEST(W188): wp = interne Taste (UI-Thread schiebt sie in den Ring) */
#define OV_MSG_CLOSED (WM_APP + 0x1A9) /* W188: Overlay zu -> UI-Thread faengt die Maus wieder */
static int g_ov_scr = SC_SET;            /* Render-Thread */
static volatile LONG g_ov_req = -1;      /* UI-Thread: Bildschirm beim Oeffnen */
static volatile LONG g_ov_textmode;      /* 1 = Texteingabe (WM_CHAR ans Overlay) */
static int g_ov_msel;                    /* Auswahl in den Menue-Bildschirmen */
static volatile LONG g_ov_pause_vk[4] = { VK_PAUSE, 0, 0, 0 }; /* Pause-Tasten (Profil-Aktion "pause") */
static volatile LONG g_ov_pause_pad;     /* Pad-Tasten der Aktion "pause" (Bits IMAP_PAD_*) */
static unsigned g_ov_vframes;            /* Pausenbilder (Test-Hook-Takt laeuft in der Pause weiter) */
static wchar_t g_fb_text[161];
static int g_fb_len, g_fb_kind, g_fb_have, g_fb_pending;
static sb_fb_pkg_t g_fb_pkg;
/* Trefferflaechen (Backbuffer-Pixel, Render-Thread schreibt beim Zeichnen) */
static int g_hit_x0, g_hit_y0, g_hit_w, g_hit_h, g_hit_rows_y, g_hit_rowh, g_hit_nrows, g_hit_valx;
static int g_hit_tab_y0, g_hit_tab_y1, g_hit_tab_x[6];
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
       OA_PEEK }; /* W193: Taste "Menue ausblenden" druecken (vk 1) / loslassen (vk 0) */
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

/* W121: Zeilen der aktuellen Seite */
/* W128: mit sbc_usb=1 steht hinter "Empfindlichkeit" (Index 9) die Zeile
 * OI_SBCMOUSE; ohne sbc_usb bleibt Seite 0 exakt W118 (Testskripte). */
#define OV_SBC_ROW 10
static int ov_nrows(void)
{
    if (g_ov_page == OV_PAGE_FX) { return OV_NPFX; } /* W133 */
    if (g_ov_page == OV_PAGE_VR) { return OV_NPVR; } /* W134 */
    return g_ov_page == 2 ? OV_NP2 : g_ov_page ? OV_NP1 : OV_NP0 + (g_cfg.sbc_usb ? 1 : 0);
}
static int ov_row(int k)
{
    if (g_ov_page == OV_PAGE_FX) { /* W133 */
        return kOvPageFx[k];
    }
    if (g_ov_page == OV_PAGE_VR) { /* W134 */
        return kOvPageVr[k];
    }
    if (g_ov_page == 2) {
        return kOvPage2[k];
    }
    if (g_ov_page) {
        return kOvPage1[k];
    }
    if (g_cfg.sbc_usb) {
        if (k == OV_SBC_ROW) {
            return OI_SBCMOUSE;
        }
        if (k > OV_SBC_ROW) {
            k--;
        }
    }
    return kOvPage0[k];
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
    case OI_RES: return 0;
    case OI_SLCOMMIT: return krnl_savelock_active(); /* W190: nur mit aktivem Schutz */
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
    case OI_SHSOFT: return g_cfg.shading != 0 && g_cfg.shade_shadow != 0;
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

static void ov_change(int i, int dir)
{
    static const int fps_v[3] = { 0, 60, 120 };
    int k;
    if (!ov_enabled(i)) { return; }
    switch (i) {
    case OI_FPS:
        for (k = 0; k < 3 && fps_v[k] != g_ov_fps; k++) {}
        if (k >= 3) { k = 2; }
        k = ov_clamp(k + dir, 0, 2);
        if (fps_v[k] == g_ov_fps) { return; }
        g_ov_fps = fps_v[k];
        g_ov_fps_pend = 1;
        HLOG_I("OVERLAY: Bildrate -> %d (interp_fps, f=%u)", g_ov_fps ? g_ov_fps : 30, g_present_no);
        break;
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
        int q = sb_gfx_preset_match(), q0 = q, hd;
        if (q < 0) { q = dir > 0 ? -1 : 3; }
        q = ov_clamp(q + dir, 0, 2);
        if (q == q0) { return; }
        sb_gfx_part_apply(q, SB_GFX_ALL);
        hd = q > 0;
        if (hd != (ov_hd_on() ? 1 : 0)) {
            if (g_ov_avail) {
                ov_hd_apply(hd, ov_hd_scale(), "Preset:");
            } else { /* ohne Paket nur der Wert (wirkt, sobald ein Pack da ist) */
                g_cfg.remaster = hd;
                g_cfg.hd_textures = hd;
            }
        }
        HLOG_I("OVERLAY: preset -> %s (f=%u)", shfx_preset_name(q), g_present_no);
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
    { "interp_fps", "render" },     { "shading", "render" },
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
    /* W133 REM-FX */
    { "fx_soft", "render" },        { "fx_embers", "render" },
    { "fx_light", "render" },       { "fx_light_cockpit", "render" },
    { "fx_interp", "render" },
    { "mouse_mode", "input" },      { "mouse_sens", "input" },
    { "sbc_mouse_off", "input" }, /* W128 */
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
    { "menu_pause", "overlay" }, /* W188 */
    { "save_lock", "game" }, /* W190 */
};
#define OV_NKEYS (sizeof kOvKeys / sizeof kOvKeys[0])

static void ov_key_value(const char *k, char *out, size_t cap)
{
    int v = 0;
    if (strcmp(k, "mouse_mode") == 0) {
        strcpy_s(out, cap, g_cfg.mouse_mode ? "absolute" : "relative");
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
    if (strcmp(k, "interp_fps") == 0) { v = g_ov_fps; }
    else if (strcmp(k, "shading") == 0) { v = g_cfg.shading; }
    else if (strcmp(k, "monitor_fx") == 0) { v = g_cfg.monitor_fx; }
    else if (strcmp(k, "monitor_grain") == 0) { v = g_cfg.monitor_grain; }
    else if (strcmp(k, "monitor_glitch") == 0) { v = g_cfg.monitor_glitch; }
    else if (strcmp(k, "edge_mode") == 0) { v = g_cfg.edge_mode; }
    else if (strcmp(k, "edge_fade_width") == 0) { v = g_cfg.edge_fade_width; }
    else if (strcmp(k, "edge_fade_strength") == 0) { v = g_cfg.edge_fade_strength; }
    else if (strcmp(k, "mouse_sens") == 0) { v = g_cfg.mouse_sens; }
    else if (strcmp(k, "sbc_mouse_off") == 0) { v = g_cfg.sbc_mouse_off; }
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
    else if (strcmp(k, "vis_query_async") == 0) { v = g_cfg.vis_query_async; }
    else if (strcmp(k, "fx_soft") == 0) { v = g_cfg.fx_soft; } /* W133 */
    else if (strcmp(k, "fx_embers") == 0) { v = g_cfg.fx_embers; }
    else if (strcmp(k, "fx_light") == 0) { v = g_cfg.fx_light; }
    else if (strcmp(k, "fx_light_cockpit") == 0) { v = g_cfg.fx_light_cockpit; }
    else if (strcmp(k, "fx_interp") == 0) { v = g_cfg.fx_interp; }
    else if (strcmp(k, "menu_pause") == 0) { v = g_cfg.menu_pause; } /* W188 */
    else if (strcmp(k, "save_lock") == 0) { v = g_cfg.save_lock; } /* W190 */
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
static const char *ov_page_name(int pg)
{
    return pg == OV_PAGE_VR ? "VR" : pg == OV_PAGE_FX ? "Effekte" : pg == 2 ? "Shader 2" : pg ? "Shader" : "Allgemein";
}

/* W188: Seite wechseln (Tab-Zeile, TAB/Umschalt+TAB, Pad LB/RB). tabrow=1: Fokus bleibt auf der Tab-Zeile. */
static void ov_page_set(int pg, int tabrow)
{
    g_ov_page = (pg + OV_NPAGES) % OV_NPAGES;
    g_ov_sel = tabrow ? -1 : 0;
    g_ov_dirty = 1;
    HLOG_I("OVERLAY: Seite -> %s (f=%u)", ov_page_name(g_ov_page), g_present_no);
}

/* W188: Bildschirm wechseln (Render-Thread) */
static void ov_screen(int sc, int sel)
{
    static const char *const nm[] = { "Einstellungen", "Spielmenue", "Beenden?", "Pause", "Feedback-Art",
                                      "Feedback-Text", "Feedback-Paket" };
    if (g_ov_scr != sc) {
        HLOG_I("MENU: Bildschirm -> %s (f=%u)", nm[sc], g_present_no);
    }
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
    InterlockedExchange(&g_ov_textmode, 0);
    InterlockedExchange(&g_ov_peek, 0); /* W193 */
    InterlockedExchange(&g_ov_open, 0);
}

/* Einstellungen (W118-Seiten) */
static void ov_key_set(int vk)
{
    int n = ov_nrows();
    switch (vk) {
    case VK_UP: /* W188: Tab-Zeile (-1) gehoert zum Umlauf */
        g_ov_sel = g_ov_sel < 0 ? n - 1 : g_ov_sel - 1;
        if (g_ov_sel >= 0 && ov_row(g_ov_sel) == OI_RES) { g_ov_sel--; }
        g_ov_dirty = 1;
        break;
    case VK_DOWN:
        g_ov_sel = g_ov_sel + 1 >= n ? -1 : g_ov_sel + 1;
        if (g_ov_sel >= 0 && ov_row(g_ov_sel) == OI_RES) { g_ov_sel++; }
        g_ov_dirty = 1;
        break;
    case VK_LEFT:
        if (g_ov_sel < 0) { ov_page_set(g_ov_page - 1, 1); }
        else { ov_change(ov_row(g_ov_sel), -1); }
        break;
    case VK_RIGHT:
        if (g_ov_sel < 0) { ov_page_set(g_ov_page + 1, 1); }
        else { ov_change(ov_row(g_ov_sel), +1); }
        break;
    case VK_TAB: /* W121: Seite Allgemein -> Shader (W131: -> Shader 2, W133: -> Effekte -> Allgemein) */
        ov_page_set(g_ov_page + 1, g_ov_sel < 0);
        break;
    case OK_BTAB: /* W188: Umschalt+TAB / Pad LB */
        ov_page_set(g_ov_page - 1, g_ov_sel < 0);
        break;
    case VK_ESCAPE: /* W188: zurueck ins Spielmenue (game_menu=0: zu wie W118) */
        if (g_cfg.game_menu) { ov_screen(SC_MENU, MI_SETTINGS); }
        else { ov_close("ESC"); }
        break;
    case VK_RETURN:
    case VK_SPACE:
        if (g_ov_sel < 0) { /* Tab-Zeile: naechste Seite */
            ov_page_set(g_ov_page + 1, 1);
        } else if (ov_row(g_ov_sel) == OI_SAVE) {
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
        } else if (ov_row(g_ov_sel) == OI_CLOSE) {
            if (g_cfg.game_menu) { ov_screen(SC_MENU, MI_SETTINGS); } /* W188: "Zurueck" */
            else { ov_close("Schliessen"); }
        } else if (ov_row(g_ov_sel) == OI_SLCOMMIT) { /* W190: bewusste Aktion, zweites ENTER bestaetigt */
            ov_sl_commit();
        } else {
            ov_change(ov_row(g_ov_sel), +1);
        }
        break;
    case 'B':
        if (g_ov_page == OV_PAGE_FX) { /* W133: Seite Effekte - B = Vergleich Original <-> Effekt-Optionen */
            ov_change(OI_FXCMP, +1);
            g_ov_dirty = 1;
        } else if (g_ov_page >= 1) { /* W121: Seite Shader (W131: und Shader 2) - B = Vergleich Original <-> Optionen */
            ov_change(OI_ABCMP, +1);
            g_ov_dirty = 1;
        } else if (g_ov_avail) {
            ov_hd_apply(!ov_hd_on(), ov_hd_scale(), "A/B:");
            g_ov_dirty = 1;
        } else {
            ov_status_key("ovl.st_ab_none");
        }
        break;
    default:
        break;
    }
}

/* W188: Zeilenzahl der Menue-Bildschirme */
static int ov_scr_rows(void)
{
    switch (g_ov_scr) {
    case SC_MENU: return MI_N;
    case SC_QUIT: return 2;
    case SC_PAUSED: return 2;
    case SC_FBKIND: return 3;
    case SC_FBTEXT: return 1;
    case SC_FBREVIEW: return 3;
    default: return ov_nrows();
    }
}

/* W188: Feedback-Paket anlegen (Render-Thread in der Pause: Spielbild liegt als Kopie vor) */
static void ov_fb_create(void)
{
    char shot[700];
    g_fb_pending = 0;
    g_fb_text[g_fb_len] = 0;
    if (!sb_feedback_begin(&g_fb_pkg, g_fb_kind)) {
        ov_status_key("ovl.st_fb_fail");
        return;
    }
    _snprintf_s(shot, sizeof shot, _TRUNCATE, "%s\\screenshot.jpg", g_fb_pkg.dir);
    sb_ctx_enter();
    g_fb_pkg.shot_ok = sb_render_pause_jpg(shot, &g_fb_pkg.shot_w, &g_fb_pkg.shot_h);
    sb_ctx_leave();
    sb_feedback_finish(&g_fb_pkg, g_fb_text);
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
        case MI_PHOTO: /* Einsprung fuer PHOTO-MODE (spaetere Aufgabe): vorerst ausgegraut */
            HLOG_I("MENU: Fotomodus (noch nicht verfuegbar, f=%u)", g_present_no);
            ov_status_key("ovl.st_photo_soon");
            break;
        case MI_FEEDBACK: ov_screen(SC_FBKIND, 0); break;
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
            if (c >= 32 && c != 127 && g_fb_len < 160) { g_fb_text[g_fb_len++] = c; g_fb_text[g_fb_len] = 0; }
            g_ov_dirty = 1;
            return;
        }
        if (vk == VK_BACK) {
            if (g_fb_len > 0) { g_fb_text[--g_fb_len] = 0; }
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
    if (g_ov_scr == SC_SET && y >= g_hit_tab_y0 && y < g_hit_tab_y1 && x >= g_hit_x0 && x < g_hit_x0 + g_hit_w) {
        int t;
        if (!click) { return; }
        for (t = 0; t < OV_NPAGES; t++) {
            if (x >= g_hit_tab_x[t] && x < g_hit_tab_x[t + 1]) {
                if (t != g_ov_page) { ov_page_set(t, 1); }
                g_ov_sel = -1;
                g_ov_dirty = 1;
                break;
            }
        }
        return;
    }
    r = ov_hit_row(x, y);
    if (r < 0) { return; }
    if (g_ov_scr == SC_SET) {
        if (ov_row(r) == OI_RES) { return; }
        if (g_ov_sel != r) { g_ov_sel = r; g_ov_dirty = 1; }
        if (click) {
            int it = ov_row(r);
            if (it == OI_SAVE || it == OI_CLOSE || it == OI_SLCOMMIT || x < g_hit_valx) { ov_key_set(VK_RETURN); }
            else { ov_change(it, x < g_hit_valx + (g_hit_x0 + g_hit_w - g_hit_valx) / 2 ? -1 : +1); }
        }
        return;
    }
    if (g_ov_msel != r) { g_ov_msel = r; g_ov_dirty = 1; }
    if (click) {
        HLOG_I("MENU: Klick Zeile %d (f=%u)", r, g_present_no);
        ov_activate(r);
    }
}

static void ov_key(int vk)
{
    if (vk & OK_CLICK) {
        ov_mouse((vk >> 15) & 0x7FFF, vk & 0x7FFF, 1);
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
    case OV_MSG_CLOSED: /* W188: Overlay zu -> Maus wieder fangen (falls vorher gefangen) */
        imouse_menu_capture(0);
        return 1;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            if (!(lp & 0x40000000)) { /* keine Autowiederholung */
                if (!g_ov_open) { ov_open_ui(g_cfg.game_menu ? SC_MENU : SC_SET); } /* W188: zuerst das Spielmenue */
                else { ov_qpush(VK_ESCAPE); } /* zurueck/zu entscheidet der Render-Thread je Bildschirm */
            }
            return 1;
        }
        if (!g_ov_textmode && wp != VK_F10 && ov_is_pause_vk((int)wp)) { /* W188: Pause-Taste */
            if (!(lp & 0x40000000)) {
                if (!g_ov_open) { ov_open_ui(SC_PAUSED); }
                else { ov_qpush(OK_PAUSE); }
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
    case WM_KEYUP:
        if (wp == VK_ESCAPE) { return 1; }
        if (g_ov_peek && ov_peek_vk() > 0 && (int)wp == ov_peek_vk()) { /* W193 */
            InterlockedExchange(&g_ov_peek, 0);
            HLOG_I("MENU: wieder eingeblendet (f=%u)", g_present_no);
            return g_ov_open ? 1 : 0;
        }
        if (!g_ov_open || wp == VK_F10) { return 0; }
        return 1;
    case WM_CHAR:
        if (g_ov_open && g_ov_textmode && wp >= 32 && wp != 127) { /* W188: Texteingabe (ANSI-Fenster) */
            char c = (char)wp;
            wchar_t wc = 0;
            if (wp < 128 || MultiByteToWideChar(CP_ACP, 0, &c, 1, &wc, 1) != 1) { wc = (wchar_t)wp; }
            ov_qpush(OK_CHAR | (int)wc);
        }
        return g_ov_open ? 1 : (wp == 0x1B);
    case WM_MOUSEMOVE: /* W188: Zeiger ueber den Zeilen waehlt (Render-Thread), Spiel-Eingabe bleibt gesperrt */
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
        ov_qpush((int)ov_click_code(w, lp)); /* W188: Klick waehlt/loest aus */
        return 1;
    case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
    case WM_RBUTTONDBLCLK: case WM_MBUTTONDBLCLK: case WM_XBUTTONDBLCLK:
        if (!g_ov_open) { return 0; }
        InterlockedIncrement(&g_ov_swallow_m);
        if (msg == WM_RBUTTONDOWN) { ov_qpush(VK_ESCAPE); } /* W188: rechte Maustaste = zurueck */
        return 1;
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_XBUTTONUP:
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
        else if (_stricmp(act, "PEEKDN") == 0) { s->act = OA_PEEK; s->vk = 1; } /* W193 */
        else if (_stricmp(act, "PEEKUP") == 0) { s->act = OA_PEEK; s->vk = 0; }
        else if (_strnicmp(act, "TXT=", 4) == 0) { s->act = OA_TXT; strcpy_s(s->txt, sizeof s->txt, act + 4); }
        else if (_strnicmp(act, "MROW", 4) == 0 && act[4]) { s->act = OA_MROW; s->vk = atoi(act + 4); }
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

static void ov_test_tick(unsigned n)
{
    HWND h;
    if (!g_cfg.overlay_test[0]) { return; }
    if (!g_ov_test_parsed) { ov_test_parse(); }
    { /* Mission = Modus 4 ([0x2EEA34], wie diag_states) + Szene 0x2C + mloop 1
       * (rq_in_mission allein trifft auch die Titel-Demo, Modus 2) */
        unsigned mode = 0;
        int in;
        __try { mode = *(volatile unsigned *)(uintptr_t)0x2EEA34u; }
        __except (EXCEPTION_EXECUTE_HANDLER) { mode = 0; }
        in = mode == 4u && rq_in_mission();
        if (in && !g_ov_in_mission && g_ov_nmission < 8) {
            g_ov_mission_f[g_ov_nmission++] = n;
            HLOG_I("OVERLAY test: Missionsbeginn #%d f=%u", g_ov_nmission, n);
        }
        g_ov_in_mission = in;
    }
    h = sb_render_hwnd();
    while (g_ov_step < g_ov_nsteps && h) {
        ov_step_t *s = &g_ov_steps[g_ov_step];
        unsigned due;
        if (s->rel == 1) {
            if (s->mi >= g_ov_nmission) { return; }
            due = g_ov_mission_f[s->mi] + s->at;
        } else if (s->rel == 2) {
            due = g_ov_prev_f + s->at;
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
        case OA_LOG: ov_log_values("test"); ov_log_shader("test"); ov_log_shader2("test"); ov_log_fx("test"); break;
        case OA_PHUDPOS: sb_phud_cycle_pos(); break;
        case OA_TKEY: PostMessageA(h, OV_MSG_TEST, (WPARAM)s->vk, 0); break; /* W188 */
        case OA_TXT: { /* W188: Zeichen per WM_CHAR wie TranslateMessage ('_' = Leerzeichen) */
            const char *q;
            for (q = s->txt; *q; q++) { PostMessageA(h, WM_CHAR, (WPARAM)(unsigned char)(*q == '_' ? ' ' : *q), 1); }
            break;
        }
        case OA_MROW: case OA_MTAB: { /* W188: Klick auf die Mitte von Zeile/Tab (Backbuffer -> Client) */
            RECT rc;
            int bx, by;
            if (s->act == OA_MROW) {
                bx = g_hit_x0 + g_hit_w / 3;
                by = g_hit_rows_y + s->vk * g_hit_rowh + g_hit_rowh / 2;
            } else {
                int t = ov_clamp(s->vk, 0, OV_NPAGES - 1);
                bx = (g_hit_tab_x[t] + g_hit_tab_x[t + 1]) / 2;
                by = (g_hit_tab_y0 + g_hit_tab_y1) / 2;
            }
            if (GetClientRect(h, &rc) && g_bb_w && g_bb_h) {
                bx = (int)((long long)bx * rc.right / (long long)g_bb_w);
                by = (int)((long long)by * rc.bottom / (long long)g_bb_h);
            }
            PostMessageA(h, WM_MOUSEMOVE, 0, MAKELPARAM(bx, by));
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
        case OA_STATE: /* W188: Beleg-Zeile Spielzustand */
        {
            unsigned tmr = 0;
            __try { tmr = *(volatile unsigned *)(uintptr_t)0x3563F0u; }
            __except (EXCEPTION_EXECUTE_HANDLER) { tmr = 0; }
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
        { IMAP_PAD_LB, OK_BTAB }, { IMAP_PAD_RB, VK_TAB }, { IMAP_PAD_X, 'B' } };
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
        if (seen) {
            cur = ps.buttons;
            if (ps.ly > 16000) { cur |= IMAP_PAD_DU; }
            if (ps.ly < -16000) { cur |= IMAP_PAD_DD; }
            if (ps.lx < -16000) { cur |= IMAP_PAD_DL; }
            if (ps.lx > 16000) { cur |= IMAP_PAD_DR; }
        }
    }
    edge = g_pad_prev == 0xFFFF ? 0 : (unsigned short)(cur & ~g_pad_prev);
    g_pad_prev = cur;
    if (edge & (unsigned short)g_ov_pause_pad) { ov_key(OK_PAUSE); edge &= (unsigned short)~g_ov_pause_pad; }
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

/* Ein Overlay-Schritt (Render-Thread): Test-Hook, Oeffnen/Schliessen, Tasten, Pad, Maus, Pause-Wunsch. */
static void ov_frame(unsigned n)
{
    int open;
    ov_test_tick(n);
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
            g_fb_pending = 0;
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
        if (g_fb_pending && g_sb_paused) { ov_fb_create(); }
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
            else if (g_ov_scr >= SC_FBKIND) { want = 1; why = "Feedback"; }
            else if (g_cfg.menu_pause) { want = 1; why = "ESC-Menue"; }
            else { why = "menu_pause=0"; }
        }
        sb_pause_request(want, why);
    }
}

void sb_ovl_present_begin(unsigned n)
{
    if (!g_cfg.overlay) { return; }
    if (!g_ov_init) { ov_init_state(); }
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

/* TEST(PERF-BENCH,W137): Bildratenwechsel wie aus dem Overlay anfordern
 * (rt_from_film + rt_interp_fps), wirkt am naechsten Present-Ende. */
void sb_ovl_request_fps(int fps)
{
    g_ov_fps = fps > 0 ? fps : 0;
    g_ov_fps_pend = 1;
    HLOG_I("OVERLAY: Bildrate -> %d angefordert (Test, f=%u)", g_ov_fps, g_present_no);
}

void sb_ovl_present_end(void)
{
    if (!g_ov_fps_pend) { return; }
    g_ov_fps_pend = 0;
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
        sb_ctx_leave();
        g_cfg.interp_fps = g_ov_fps;
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
        "ovl.lb_savelock", "ovl.lb_slcommit" };
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
        if (g_ov_fps == 0) { ov_wkey(o, cap, "ovl.v_30orig"); }
        else { _snwprintf_s(o, cap, _TRUNCATE, L"%d", g_ov_fps); }
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
    case OI_HD:
        if (!g_ov_avail) { ov_wkey(o, cap, "ovl.v_nopkg"); }
        else { ov_wkey(o, cap, ov_hd_on() ? "ovl.v_tex_hd" : "ovl.v_orig"); } /* W173: wie Launcher "Texturen" */
        break;
    case OI_HDSCALE: wcsncpy_s(o, cap, ov_hd_scale() == 2 ? L"2\u00D7" : L"4\u00D7", _TRUNCATE); break;
    case OI_WINMODE: {
        char nb[64];
        _snprintf_s(nb, sizeof nb, _TRUNCATE, "%s%s", LT(wm[g_ov_winmode]),
                    g_ov_winmode != ov_clamp(g_cfg.window_mode, 0, 2) ? " *" : "");
        ov_wu8(o, cap, nb);
        break;
    }
    case OI_RES: _snwprintf_s(o, cap, _TRUNCATE, L"%u\u00D7%u *", g_bb_w, g_bb_h); break;
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
    case OI_VRON: { /* W134 VR-PROTO */
        int v = g_ov_vr >= 0 ? g_ov_vr : g_cfg.vr_enable;
        char nb[64];
        _snprintf_s(nb, sizeof nb, _TRUNCATE, "%s%s", LT(v ? "ovl.v_on" : "ovl.v_off"),
                    v != (g_cfg.vr_enable ? 1 : 0) ? " *" : "");
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
        _snprintf_s(nb, sizeof nb, _TRUNCATE, "%s%s", LT(pv[k]), g_ov_vrprev >= 0 ? " *" : "");
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
        const char *mark = v != g_cfg.rt_scale ? " *" : "";
        char nb[64];
        if (dyn) { _snprintf_s(nb, sizeof nb, _TRUNCATE, LT("ovl.v_rtdyn"), sb_rtt_dyn_factor(), mark); }
        else if (v == 1) { _snprintf_s(nb, sizeof nb, _TRUNCATE, LT("ovl.v_origmark"), mark); }
        else if (v == 0) { _snprintf_s(nb, sizeof nb, _TRUNCATE, LT("ovl.v_auto"), (g_bb_h + 639u) / 640u > 4u ? 4u : (g_bb_h + 639u) / 640u, mark); }
        else { _snprintf_s(nb, sizeof nb, _TRUNCATE, "%d\xC3\x97%s", v, mark); }
        ov_wu8(o, cap, nb);
        break;
    }
    case OI_PRESET: { /* W131, W143; W173 LAUNCHER-P4: einheitliches Preset, der Standard heisst nach
                       * der echten Bildrate "Original (HD) - <n> FPS" (PO 7.11.; 30 = Original) */
        static const char *const pn[3] = { "ovl.v_orig_hd", "ovl.v_subtle", "ovl.v_strong" };
        int q = sb_gfx_preset_match();
        if (q == 0) {
            char nb[96];
            _snprintf_s(nb, sizeof nb, _TRUNCATE, LT("ovl.v_orig_hd"), g_ov_fps > 0 ? g_ov_fps : 30);
            ov_wu8(o, cap, nb);
        } else {
            ov_wkey(o, cap, q > 0 ? pn[q] : "ovl.v_custom");
        }
        break;
    }
    case OI_AA: {
        static const char *const am[4] = { "ovl.v_offorig", "ovl.v_fxaa_final", "ovl.v_fxaa_world", "ovl.v_fxaa_both" };
        ov_wkey(o, cap, am[ov_clamp(g_cfg.aa_mode, 0, 3)]);
        break;
    }
    case OI_SSAO: /* W131: ohne BB-Tiefen-SRV (Start mit ssao=0) erst nach Neustart -> "*" */
        if (g_cfg.ssao <= 0) { ov_wkey(o, cap, "ovl.v_offorig"); }
        else { _snwprintf_s(o, cap, _TRUNCATE, L"%d %%%s", g_cfg.ssao, g_bb_dsrv ? L"" : L" *"); }
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
    case OI_LABEL: { /* W153: "*" bei Abweichung vom Startwert (Neustart) */
        char nb[64];
        _snprintf_s(nb, sizeof nb, _TRUNCATE, "%s%s", LT(g_cfg.hud_label_avoid ? "ovl.v_avoid" : "ovl.v_orig"),
                    g_cfg.hud_label_avoid != g_ov_label0 ? " *" : "");
        ov_wu8(o, cap, nb);
        break;
    }
    default: break;
    }
}

/* Panel-Groesse in Zielpixeln (Faktor aus der 4:3-Box) */
#define OV_TABH 13.0f /* W188: Tab-Zeile der Einstellungen */
static int g_rel_rows_y, g_rel_rowh, g_rel_nrows, g_rel_valx, g_rel_tab_y0, g_rel_tab_y1, g_rel_tab_x[OV_NPAGES + 1];

/* W188: Textzeilen ueber den Auswahlzeilen je Menue-Bildschirm */
static int ov_scr_lines(void)
{
    switch (g_ov_scr) {
    case SC_MENU: return krnl_savelock_active() ? 1 : 0; /* W190: Hinweis "Spielstaende geschuetzt" */
    case SC_QUIT: return 1;
    case SC_PAUSED: return 1;
    case SC_FBKIND: return 2;
    case SC_FBTEXT: return 3;
    case SC_FBREVIEW: return 7;
    default: return 0;
    }
}

static void ov_layout(unsigned boxh, unsigned *w, unsigned *h)
{
    g_ovf = (float)boxh / 540.0f;
    if (g_ovf < 1.0f) { g_ovf = 1.0f; }
    if (g_ov_scr != SC_SET) { /* W188: Menue-Bildschirme */
        int nl = ov_scr_lines();
        *w = (unsigned)S(g_ov_scr >= SC_FBKIND ? 300.0f : 210.0f);
        *h = (unsigned)(S(20.0f) + S(5.0f) + nl * S(11.0f) + (nl ? S(5.0f) : 0) + ov_scr_rows() * S(12.5f) + S(6.0f) +
                        2 * S(11.0f) + S(6.0f));
        return;
    }
    *w = (unsigned)S(262.0f);
    *h = (unsigned)(S(20.0f) + S(4.0f) + (OV_NP0 > OV_NP1 ? OV_NP0 : OV_NP1) * S(12.5f) + S(6.0f) + 3 * S(11.0f) + S(6.0f)); /* W143: + FPS-Hinweis */
    *h += (unsigned)(S(OV_TABH) + S(3.0f)); /* W188: Tab-Zeile */
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

/* W188: Tab-Zeile der Einstellungen (erste Zeile unter dem Kopf). y = Oberkante, liefert Unterkante. */
static int ov_paint_tabs(int W, int y)
{
    static const char *const tk[OV_NPAGES] = { "ovl.tab_settings", "ovl.tab_shader", "ovl.tab_shader2", "ovl.tab_fx",
                                               "ovl.tab_vr" };
    int pad = S(6.0f), th = S(OV_TABH), gap = S(3.0f), t, x, tw[OV_NPAGES], sum = 0, foc = g_ov_sel < 0;
    wchar_t nm[OV_NPAGES][32];
    for (t = 0; t < OV_NPAGES; t++) {
        SIZE sz;
        ov_wkey(nm[t], 32, tk[t]);
        GetTextExtentPoint32W(ov_dc, nm[t], (int)wcslen(nm[t]), &sz);
        tw[t] = sz.cx + 2 * S(4.0f);
        sum += tw[t];
    }
    { /* Restbreite gleichmaessig verteilen (Zeile fuellt das Panel) */
        int avail = W - 2 * pad - (OV_NPAGES - 1) * gap;
        if (avail > sum) {
            int extra = (avail - sum) / OV_NPAGES;
            for (t = 0; t < OV_NPAGES; t++) { tw[t] += extra; }
        }
    }
    x = pad;
    g_rel_tab_y0 = y;
    g_rel_tab_y1 = y + th;
    for (t = 0; t < OV_NPAGES; t++) {
        int act = t == g_ov_page, ty = y + (th - ov_font_px) / 2 - S(0.5f);
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
    g_rel_tab_x[OV_NPAGES] = x - gap;
    return y + th;
}

/* W188: Menue-Bildschirme (Spielmenue, Beenden?, Pause, Feedback) im Look der Einstellungen */
static void ov_paint_menu(void)
{
    int W = (int)ov_bw, H = (int)ov_bh, rowh = S(12.5f), pad = S(6.0f), b = S(1.0f), y, k, nl = ov_scr_lines();
    int nr = ov_scr_rows();
    wchar_t title[32], tag[48], ln[8][128], lb[64], vb[64], hint[96], rst[8];
    COLORREF lc[8];
    static const char *const rk[][MI_N] = {
        { NULL },
        { "ovl.m_continue", "ovl.m_settings", "ovl.m_photo", "ovl.m_feedback", "ovl.m_quit" },
        { "ovl.q_no", "ovl.q_yes" },
        { "ovl.m_continue", "ovl.p_menu" },
        { "ovl.fb_bug", "ovl.fb_idea", "ovl.fb_back" },
        { "ovl.fb_create" },
        { "ovl.fb_open", "ovl.fb_folder", "ovl.fb_done" },
    };
    tag[0] = 0;
    for (k = 0; k < 8; k++) { ln[k][0] = 0; lc[k] = OV_C_TXT; }
    wcscpy_s(title, 32, g_ov_scr >= SC_FBKIND ? L"\x25A0 FEEDBACK" : L"\x25A0 SYS-MENU");
    switch (g_ov_scr) {
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
        lc[1] = OV_C_DIM;
        ov_wkey(hint, 96, "ovl.hint_back");
        break;
    case SC_FBTEXT: {
        size_t L = (size_t)g_fb_len, maxc = 44;
        ov_wkey(tag, 48, g_fb_kind ? "ovl.fb_idea" : "ovl.fb_bug");
        ov_wkey(ln[0], 128, "ovl.fb_text_q");
        lc[0] = OV_C_DIM;
        _snwprintf_s(ln[1], 128, _TRUNCATE, L"> %s_", g_fb_text + (L > maxc ? L - maxc : 0));
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
        int sel = k == g_ov_msel, dis = g_ov_scr == SC_MENU && k == MI_PHOTO;
        int yt = y + (rowh - ov_font_px) / 2 - S(0.5f);
        if (sel) {
            ov_fill(b + S(2.0f), y, W - b - S(2.0f), y + rowh, OV_C_SEL);
            ov_fill(b + S(2.0f), y, b + S(4.0f), y + rowh, OV_C_TXT);
        }
        ov_wkey(lb, 64, rk[g_ov_scr][k]);
        ov_text(pad + S(6.0f), yt, lb, dis ? OV_C_DIM : OV_C_TXT, 0);
        if (dis) { /* Fotomodus: ausgegraut, "kommt bald" */
            ov_wkey(vb, 64, "ovl.m_soon");
            ov_text(W - pad - S(2.0f), yt, vb, OV_C_DIM, 1);
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

static void ov_paint(void)
{
    int W = (int)ov_bw, H = (int)ov_bh, i, k, y, rowh = S(12.5f), pad = S(6.0f), b = S(1.0f), cl = S(8.0f);
    int head = S(20.0f), ty;
    wchar_t lb[64], vb[64];
    if (g_ov_scr != SC_SET) { /* W188: Menue-Bildschirme */
        ov_paint_menu();
        return;
    }
    (void)cl;
    (void)ty;
    head = ov_frame_head(W, H, L"\u25A0 SYS-CONFIG", NULL);
    y = ov_paint_tabs(W, head + S(3.0f)) + S(3.0f); /* W188: Tab-Zeile als erste Zeile */
    g_rel_rows_y = y + S(1.0f);
    g_rel_rowh = rowh;
    g_rel_nrows = ov_nrows();
    g_rel_valx = W / 2;
    y += S(1.0f);
    for (k = 0; k < ov_nrows(); k++) {
        int en, sel = (k == g_ov_sel);
        COLORREF c;
        i = ov_row(k);
        en = ov_effective(i);
        c = en ? OV_C_TXT : OV_C_DIM;
        int yt = y + (rowh - ov_font_px) / 2 - S(0.5f);
        if (sel) {
            ov_fill(b + S(2.0f), y, W - b - S(2.0f), y + rowh, OV_C_SEL);
            ov_fill(b + S(2.0f), y, b + S(4.0f), y + rowh, OV_C_TXT);
        }
        if (i == OI_CLOSE && g_cfg.game_menu) { ov_wkey(lb, 64, "ovl.lb_back"); } /* W188: zurueck ins Spielmenue */
        else { ov_label(i, lb, 64); }
        ov_text(pad + S(6.0f), yt, lb, (i == OI_SAVE || i == OI_CLOSE) ? OV_C_TXT : c, 0);
        if (i != OI_SAVE && i != OI_CLOSE && i != OI_SLCOMMIT) { /* W190: Aktion ohne Wert */
            ov_value(i, vb, 64);
            if (sel && ov_enabled(i) && i != OI_RES) {
                wchar_t t[80];
                _snwprintf_s(t, 80, _TRUNCATE, L"\u25C4 %s \u25BA", vb);
                ov_text(W - pad - S(2.0f), yt, t, OV_C_TXT, 1);
            } else {
                ov_text(W - pad - S(2.0f), yt, vb, c, 1);
            }
        }
        y += rowh;
        if ((!g_ov_page && (i == OI_SHADING || i == OI_GLITCH || i == OI_EDGES ||
                            (i == OI_MSENS && !g_cfg.sbc_usb) || i == OI_SBCMOUSE || i == OI_HDSCALE ||
                            i == OI_RES || i == OI_MPAUSE || i == OI_SLCOMMIT)) || /* W190 */
            (g_ov_page == 1 && (i == OI_GLITCH || i == OI_RTS || i == OI_BTHR || i == OI_CLIGHT || i == OI_ABCMP)) ||
            (g_ov_page == 2 && (i == OI_PRESET || i == OI_SSAO || i == OI_CGAIN || i == OI_SHSOFT || i == OI_ABCMP)) ||
            (g_ov_page == OV_PAGE_FX && (i == OI_FXPRESET || i == OI_FXIP || i == OI_FXCMP)) || /* W133 */
            (g_ov_page == OV_PAGE_VR && (i == OI_VRREC || i == OI_VRIPD || i == OI_VRPREV || i == OI_VRSHADE))) { /* W134, W183 */
            ov_fill(pad, y - b / 2, W - pad, y - b / 2 + b, OV_C_DIM);
        }
    }
    y += S(3.0f);
    ov_fill(pad, y, W - pad, y + b, OV_C_LINE);
    y += S(3.0f);
    {
        wchar_t foot[64], rst[48], ctl[64];
        if (g_ov_page) {
            ov_wkey(foot, 64, "ovl.foot1");
        } else {
            ov_wkey(foot, 64, "ovl.foot0"); /* W173: Sprachschluessel statt fester Text */
        }
        ov_text(pad + S(2.0f), y, foot, OV_C_DIM, 0);
        y += S(11.0f);
        if (!g_ov_status[0]) {
            ov_wkey(rst, 48, "ovl.st_restart");
        }
        ov_text(pad + S(2.0f), y, g_ov_status[0] ? g_ov_status : rst,
                g_ov_status[0] ? OV_C_TXT : OV_C_DIM, 0);
        if (g_ov_page != OV_PAGE_VR) {
            /* W143: FPS-Hinweis (Remaster-Seiten; VR hat keine solchen Optionen) */
            wchar_t fps[64];
            y += S(11.0f);
            if (g_sb_paused && g_cfg.pause_live && ov_peek_vk() > 0) { /* W193: in der Pause Hinweis aufs Ausblenden */
                char pn[96];
                _snprintf_s(pn, sizeof pn, _TRUNCATE, LT("ovl.peek_note"), g_cfg.menu_peek_key);
                ov_wu8(fps, 64, pn);
            } else
            ov_wkey(fps, 64, "ovl.fps_note");
            ov_text(pad + S(2.0f), y, fps, OV_C_DIM, 0);
        }
        if (g_cfg.sbc_usb) {
            /* W128: Original-Controller-Zustand (sbc_usb=0: Zeile fehlt, Layout wie bisher) */
            int st = sbc_wire_status();
            y += S(11.0f);
            ov_wkey(ctl, 64, st == SBC_WIRE_USB_OK ? "ovl.ctl_usb" : st == SBC_WIRE_FIXTURE ? "ovl.ctl_fixture" :
                              st == SBC_WIRE_PENDING ? "ovl.ctl_search" : "ovl.ctl_none");
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

void sb_ovl_draw(ID3D11RenderTargetView *rtv, int save)
{
    unsigned bx, by, bw, bh, pw, ph, x0, y0;
    static unsigned lk[4];
    if (!g_cfg.overlay || !g_ov_open || !rtv || !g_ctx) { return; }
    if (g_ov_peek) { return; } /* W193: Menue ausgeblendet (Taste gehalten), bleibt offen */
    if (!ov_d3d_init()) { return; }
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
    if (g_ov_scr != SC_SET) { /* W188: Menue-Bildschirme mittig in der 4:3-Box */
        x0 = bx + (bw - pw) / 2;
        y0 = by + (bh - ph) / 2;
    } else {
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
        g_hit_tab_y0 = g_ov_scr == SC_SET ? (int)y0 + g_rel_tab_y0 : -1;
        g_hit_tab_y1 = g_ov_scr == SC_SET ? (int)y0 + g_rel_tab_y1 : -1;
        for (t = 0; t <= OV_NPAGES; t++) { g_hit_tab_x[t] = (int)x0 + g_rel_tab_x[t]; }
    }
    if (lk[0] != x0 || lk[1] != y0 || lk[2] != pw || lk[3] != ph) {
        lk[0] = x0; lk[1] = y0; lk[2] = pw; lk[3] = ph;
        HLOG_I("OVERLAY panel %ux%u @%u,%u in Box %ux%u@%u,%u (bb %ux%u, Faktor %.2f, Schrift %d px)", pw, ph, x0,
               y0, bw, bh, bx, by, g_bb_w, g_bb_h, (double)g_ovf, ov_font_px);
    }
    if (save) { sb_ovl_save_take(&ov_bbsave, rtv, x0, y0, pw, ph); }
    sb_ovl_quad(rtv, ov_srv, x0, y0, pw, ph);
}
