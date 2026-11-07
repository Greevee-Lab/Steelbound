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
#include <stdlib.h>
#include <wchar.h>

/* ---- Modulgrenzen (andere Dateien) ---- */
int sb_render_offscreen(void);                 /* d3d11_render.c: 1 = FPS-INTERP-Offscreen-BB */
int sb_render_views_rebuild(const char *why);  /* d3d11_render.c: Views neu (interp_fps-Wechsel) */
HWND sb_render_hwnd(void);                     /* d3d11_render.c */
int imouse_subclassed(void);                   /* input_mouse.c */
void imouse_release_keys(void);                /* input_mouse.c (UI-Thread) */

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
    OI_CWIDEN, /* W166 COCKPIT-WIDEN (Seite "Shader 2") */
    /* W133 REM-FX: Seite "Effekte" (OV_PAGE_FX) */
    OI_FXPRESET, OI_FXSOFT, OI_FXEMB, OI_FXLIGHT, OI_FXLCKP, OI_FXIP, OI_FXCMP,
    /* W134 VR-PROTO: Seite "VR" (OV_PAGE_VR) */
    OI_VRON, OI_VRREC, OI_VRSEAT, OI_VRSCALE, OI_VRIPD, OI_VRPREV,
    OI_N
};
/* Zeilen je Seite (Seite 0 = W118 unveraendert, Testskripte zaehlen DOWN;
 * W173: Grafik-Preset als letzte Inhaltszeile, die Zeilen davor bleiben) */
static const int kOvPage0[] = { OI_FPS, OI_SHADING, OI_MFX, OI_GRAIN, OI_GLITCH, OI_EDGE, OI_EDGEW, OI_EDGES,
                                OI_MMODE, OI_MSENS, OI_HD, OI_HDSCALE, OI_WINMODE, OI_RES, OI_PRESET, OI_SAVE, OI_CLOSE };
static const int kOvPage1[] = { OI_SHADING, OI_MFX, OI_GRAIN, OI_GLITCH,
                                OI_EXPO, OI_TONE, OI_WGAM, OI_WSHARP, OI_FOCUS, OI_WCON, OI_FOG, OI_LWORLD, OI_RTS,
                                OI_BLOOM, OI_BTHR,
                                OI_LCKP, OI_CGLOW, OI_CSPILL, OI_CLIGHT,
                                OI_ABCMP, OI_SAVE, OI_CLOSE };
static const int kOvPage2[] = { OI_PRESET, OI_AA, OI_SSAO, OI_CSAT, OI_CTEMP, OI_CLIFT, OI_CGAM, OI_CGAIN,
                                OI_SHADOW, OI_SHSOFT, OI_VQASYNC, OI_LABEL, OI_CWIDEN, OI_ABCMP, OI_SAVE, OI_CLOSE };
#define OV_NP2 ((int)(sizeof kOvPage2 / sizeof kOvPage2[0]))
/* W133 REM-FX: Seite "Effekte" (Index 3, nach "Shader 2") */
#define OV_PAGE_FX 3
#define OV_PAGE_VR 4 /* W134 VR-PROTO: Seite "VR" (nach "Effekte") */
#define OV_NPAGES 5
static const int kOvPageFx[] = { OI_FXPRESET, OI_FXSOFT, OI_FXEMB, OI_FXLIGHT, OI_FXLCKP, OI_FXIP, OI_FXCMP,
                                 OI_SAVE, OI_CLOSE };
#define OV_NPFX ((int)(sizeof kOvPageFx / sizeof kOvPageFx[0]))
static const int kOvPageVr[] = { OI_VRON, OI_VRREC, OI_VRSEAT, OI_VRSCALE, OI_VRIPD, OI_VRPREV, OI_SAVE, OI_CLOSE };
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
static wchar_t g_ov_status[96];
static ULONGLONG g_ov_status_until;
static char g_ov_save_path[600];

/* ---- Test-Hook (overlay_test) ---- */
typedef struct { unsigned at; int rel; int mi; int act; int vk; } ov_step_t;
enum { OA_KEY, OA_SHOT, OA_FOCUS1, OA_FOCUS0, OA_LOG, OA_PHUDPOS };
static ov_step_t g_ov_steps[96];
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
    case OI_CWIDEN: return g_bb_w * 3u > g_bb_h * 4u; /* W166: nur im Breitbild wirksam */
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
    case OI_CWIDEN: /* W166: wirkt sofort (je Bild gelesen) */
        g_cfg.cockpit_widen = !g_cfg.cockpit_widen;
        HLOG_I("OVERLAY: cockpit_widen -> %d (f=%u)", g_cfg.cockpit_widen, g_present_no);
        ov_status_key("ovl.st_cwiden");
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
    { "cockpit_widen", "render" }, /* W166 */
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
    else if (strcmp(k, "cockpit_widen") == 0) { v = g_cfg.cockpit_widen; } /* W166 */
    else if (strcmp(k, "vis_query_async") == 0) { v = g_cfg.vis_query_async; }
    else if (strcmp(k, "fx_soft") == 0) { v = g_cfg.fx_soft; } /* W133 */
    else if (strcmp(k, "fx_embers") == 0) { v = g_cfg.fx_embers; }
    else if (strcmp(k, "fx_light") == 0) { v = g_cfg.fx_light; }
    else if (strcmp(k, "fx_light_cockpit") == 0) { v = g_cfg.fx_light_cockpit; }
    else if (strcmp(k, "fx_interp") == 0) { v = g_cfg.fx_interp; }
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
    int done[64], n = 0;
    int i;
    if (nk > 64) { nk = 64; }
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
static void ov_key(int vk)
{
    switch (vk) {
    case VK_UP:
        g_ov_sel = (g_ov_sel + ov_nrows() - 1) % ov_nrows();
        if (ov_row(g_ov_sel) == OI_RES) { g_ov_sel--; }
        g_ov_dirty = 1;
        break;
    case VK_DOWN:
        g_ov_sel = (g_ov_sel + 1) % ov_nrows();
        if (ov_row(g_ov_sel) == OI_RES) { g_ov_sel++; }
        g_ov_dirty = 1;
        break;
    case VK_LEFT:
        ov_change(ov_row(g_ov_sel), -1);
        break;
    case VK_RIGHT:
        ov_change(ov_row(g_ov_sel), +1);
        break;
    case VK_TAB: /* W121: Seite Allgemein -> Shader (W131: -> Shader 2, W133: -> Effekte -> Allgemein) */
        g_ov_page = (g_ov_page + 1) % OV_NPAGES;
        g_ov_sel = 0;
        g_ov_dirty = 1;
        HLOG_I("OVERLAY: Seite -> %s (f=%u)", g_ov_page == OV_PAGE_VR ? "VR" : g_ov_page == OV_PAGE_FX ? "Effekte" : g_ov_page == 2 ? "Shader 2" : g_ov_page ? "Shader" : "Allgemein", g_present_no);
        break;
    case VK_RETURN:
    case VK_SPACE:
        if (ov_row(g_ov_sel) == OI_SAVE) {
            int n = ov_save();
            if (n >= 0) {
                HLOG_I("OVERLAY: gespeichert -> %s (%d Keys, f=%u)", g_ov_save_path, n, g_present_no);
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
            InterlockedExchange(&g_ov_open, 0);
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

int sb_ovl_wndproc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)w;
    if (!g_cfg.overlay) { return 0; }
    switch (msg) {
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            if (!(lp & 0x40000000)) { /* keine Autowiederholung */
                LONG now = !g_ov_open;
                if (now) { InterlockedExchange(&g_ov_swallow_k, 0); InterlockedExchange(&g_ov_swallow_m, 0); }
                InterlockedExchange(&g_ov_open, now);
                imouse_release_keys(); /* gehaltene Spiel-Tasten loesen */
            }
            return 1;
        }
        if (!g_ov_open || wp == VK_F10) { return 0; }
        if (wp == VK_UP || wp == VK_DOWN || wp == VK_LEFT || wp == VK_RIGHT) {
            ov_qpush((int)wp); /* Autowiederholung erlaubt */
        } else if ((wp == VK_RETURN || wp == VK_SPACE || wp == 'B' || wp == VK_TAB) && !(lp & 0x40000000)) {
            ov_qpush((int)wp);
        } else {
            InterlockedIncrement(&g_ov_swallow_k);
        }
        return 1;
    case WM_KEYUP:
        if (wp == VK_ESCAPE) { return 1; }
        if (!g_ov_open || wp == VK_F10) { return 0; }
        return 1;
    case WM_CHAR:
        return g_ov_open ? 1 : (wp == 0x1B);
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
    case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK: case WM_MBUTTONDBLCLK: case WM_XBUTTONDBLCLK:
        if (!g_ov_open) { return 0; }
        InterlockedIncrement(&g_ov_swallow_m);
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
        default: ov_post_key(h, s->vk); break;
        }
        HLOG_I("OVERLAY test: Schritt %d/%d f=%u %s vk=0x%02X", g_ov_step, g_ov_nsteps, n,
               s->act == OA_SHOT ? "SHOT" : s->act == OA_LOG ? "LOG" : s->act == OA_KEY ? "Taste" : "Fokus",
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

void sb_ovl_present_begin(unsigned n)
{
    int open;
    if (!g_cfg.overlay) { return; }
    if (!g_ov_init) { ov_init_state(); }
    ov_test_tick(n);
    open = g_ov_open != 0;
    if (open != g_ov_was_open) {
        g_ov_was_open = open;
        g_ov_dirty = 1;
        if (open) {
            InterlockedExchange(&g_ov_blocked_q, 0);
            HLOG_I("OVERLAY: offen (f=%u) -> echte Eingaben gehen ans Overlay", n);
        } else {
            HLOG_I("OVERLAY: zu (f=%u; verschluckt: tasten=%ld maustasten=%ld, gesperrte Abfragen=%ld)", n,
                   (long)g_ov_swallow_k, (long)g_ov_swallow_m, (long)g_ov_blocked_q);
            ov_log_values("zu");
            ov_log_shader("zu");
            ov_log_shader2("zu");
            ov_log_fx("zu");
        }
    }
    while (g_ov_qt != g_ov_qh) {
        int vk = (int)g_ov_q[g_ov_qt & 63];
        InterlockedIncrement(&g_ov_qt);
        if (g_ov_open) { ov_key(vk); }
    }
    if (g_ov_status_until && GetTickCount64() > g_ov_status_until) {
        g_ov_status_until = 0;
        g_ov_status[0] = 0;
        g_ov_dirty = 1;
    }
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
        "ovl.lb_shsoft", "ovl.lb_vqasync", "ovl.lb_label", "ovl.lb_cwiden",
        /* W133 Seite Effekte (W173: Effekt-Stufe, Teil des Grafik-Presets) */
        "ovl.lb_fxlevel", "ovl.lb_fxsoft", "ovl.lb_fxemb", "ovl.lb_fxlight", "ovl.lb_fxlightckp",
        "ovl.lb_fxip", "ovl.lb_cmp",
        /* W134 Seite VR */
        "ovl.lb_vron", "ovl.lb_vrrec", "ovl.lb_vrseat", "ovl.lb_vrscale", "ovl.lb_vripd",
        "ovl.lb_vrprev" };
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
    case OI_CWIDEN: ov_wkey(o, cap, g_cfg.cockpit_widen ? "ovl.v_widened" : "ovl.v_orig"); break; /* W166 */
    default: break;
    }
}

/* Panel-Groesse in Zielpixeln (Faktor aus der 4:3-Box) */
static void ov_layout(unsigned boxh, unsigned *w, unsigned *h)
{
    g_ovf = (float)boxh / 540.0f;
    if (g_ovf < 1.0f) { g_ovf = 1.0f; }
    *w = (unsigned)S(262.0f);
    *h = (unsigned)(S(20.0f) + S(4.0f) + (OV_NP0 > OV_NP1 ? OV_NP0 : OV_NP1) * S(12.5f) + S(6.0f) + 3 * S(11.0f) + S(6.0f)); /* W143: + FPS-Hinweis */
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

static void ov_paint(void)
{
    int W = (int)ov_bw, H = (int)ov_bh, i, k, y, rowh = S(12.5f), pad = S(6.0f), b = S(1.0f), cl = S(8.0f);
    int head = S(20.0f), ty;
    wchar_t lb[64], vb[64];
    ov_fill(0, 0, W, H, OV_C_BG);
    /* Rahmen + Eckwinkel (HUD-Klammern) */
    ov_fill(0, 0, W, b, OV_C_LINE);
    ov_fill(0, H - b, W, H, OV_C_LINE);
    ov_fill(0, 0, b, H, OV_C_LINE);
    ov_fill(W - b, 0, W, H, OV_C_LINE);
    ov_fill(0, 0, cl, 2 * b + 1, OV_C_TXT);
    ov_fill(0, 0, 2 * b + 1, cl, OV_C_TXT);
    ov_fill(W - cl, H - 2 * b - 1, W, H, OV_C_TXT);
    ov_fill(W - 2 * b - 1, H - cl, W, H, OV_C_TXT);
    /* Kopfzeile */
    ov_fill(b + S(2.0f), b + S(2.0f), W - b - S(2.0f), head, OV_C_HEAD);
    ty = b + S(2.0f) + (head - b - S(2.0f) - ov_font_px) / 2 - S(0.5f);
    ov_text(pad + S(2.0f), ty, L"\u25A0 SYS-CONFIG", OV_C_BG, 0);
    {
        wchar_t tab[32];
        ov_wkey(tab, 32, g_ov_page == OV_PAGE_VR ? "ovl.tab_vr" : g_ov_page == OV_PAGE_FX ? "ovl.tab_fx" :
                          g_ov_page == 2 ? "ovl.tab_shader2" : g_ov_page ? "ovl.tab_shader" : "ovl.tab_settings");
        ov_text(W - pad - S(2.0f), ty, tab, OV_C_BG, 1);
    }
    y = head + S(4.0f);
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
        ov_label(i, lb, 64);
        ov_text(pad + S(6.0f), yt, lb, (i == OI_SAVE || i == OI_CLOSE) ? OV_C_TXT : c, 0);
        if (i != OI_SAVE && i != OI_CLOSE) {
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
                            i == OI_RES)) ||
            (g_ov_page == 1 && (i == OI_GLITCH || i == OI_RTS || i == OI_BTHR || i == OI_CLIGHT || i == OI_ABCMP)) ||
            (g_ov_page == 2 && (i == OI_PRESET || i == OI_SSAO || i == OI_CGAIN || i == OI_SHSOFT || i == OI_ABCMP)) ||
            (g_ov_page == OV_PAGE_FX && (i == OI_FXPRESET || i == OI_FXIP || i == OI_FXCMP)) || /* W133 */
            (g_ov_page == OV_PAGE_VR && (i == OI_VRREC || i == OI_VRIPD || i == OI_VRPREV))) { /* W134 */
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
    x0 = bx + (unsigned)S(10.0f);
    y0 = by + (unsigned)S(10.0f);
    if (x0 + pw > bx + bw) { x0 = bx; }
    if (y0 + ph > by + bh) { y0 = by; }
    if (lk[0] != x0 || lk[1] != y0 || lk[2] != pw || lk[3] != ph) {
        lk[0] = x0; lk[1] = y0; lk[2] = pw; lk[3] = ph;
        HLOG_I("OVERLAY panel %ux%u @%u,%u in Box %ux%u@%u,%u (bb %ux%u, Faktor %.2f, Schrift %d px)", pw, ph, x0,
               y0, bw, bh, bx, by, g_bb_w, g_bb_h, (double)g_ovf, ov_font_px);
    }
    if (save) { sb_ovl_save_take(&ov_bbsave, rtv, x0, y0, pw, ph); }
    sb_ovl_quad(rtv, ov_srv, x0, y0, pw, ph);
}
