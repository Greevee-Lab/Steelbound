/* launcher_cfg.c - Nutzer-cfg + Profile des Launchers (A10, W26/W47).
 * ASCII only. Alle Nutzertexte deutsch. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "launcher_cfg.h"
#include "launcher_u8.h"
#include "sb_lang.h"
#include "sb_edition.h"
#include "iso_vfs.h"     /* W150 ISO-DIRECT */
#include "sb_gamever.h"  /* W150 */
#include "sb_userdata.h" /* W150 USER-DATA */
#include "sb_gfxpreset.h" /* W173 LAUNCHER-P4: Preset-Werte (wie Client) */

#ifndef SB_HARNESS_DIR_DEFAULT
#define SB_HARNESS_DIR_DEFAULT ""
#endif

void lcfg_defaults(launcher_settings_t *s)
{
    memset(s, 0, sizeof *s);
    /* W116: Standardpfade je Edition (sb_edition.h; private unveraendert) */
    strcpy_s(s->xbe_path, sizeof s->xbe_path, SB_DEFAULT_XBE_PATH);
    strcpy_s(s->media_root, sizeof s->media_root, SB_DEFAULT_MEDIA_ROOT);
    strcpy_s(s->hdd_root, sizeof s->hdd_root, "HDD");
    s->width = 2560;
    s->height = 1080;
    s->present_interval = 0;
    s->fs_interp_match = 1;
    s->fps_limit = 120;
    s->audio = 1;
    s->audio_live = 1;
    s->aspect_mode = 1; /* W47: Pillarbox + Hor+, nie strecken (S3.10) */
    s->input_profile[0] = '\0'; /* W78: leer = Legacy-Tastatur */
    s->sbc_usb = 0;             /* W78: USB-Controller aus */
    s->sbc_mouse_off = 1;       /* W128: Maus-Achsen aus bei aktivem Controller */
    s->aim_turn = 0;            /* W218 SIMPLE-CONTROLS: Standard aus */
    s->auto_gear = 0;
    s->save_lock = 0;           /* W190 SAVE-LOCK: Original-Verhalten */
    s->photo_dir[0] = '\0';     /* W195 PHOTO-MODE: Standardordner */
    s->photo_scale = 1;
    s->photo_jpg = 0;
    s->photo_aspect = 1; /* W195: Bildschirm (weit) */
    s->interp_fps = 120;        /* W78: Spieler-Standard wie Client (hcfg) */
    s->window_mode = 0;         /* W81: Fenster (Client-Standard) */
    s->fs_hz = 0;               /* W81: Desktop-Rate */
    s->mouse_mode = 1;          /* W87: Spieler-Standard absolut */
    s->mouse_sens = 4;          /* W87: wie Client-Standard (hcfg) */
    s->edge_mode = 2;           /* Nutzer 5.11.: Abdunklung als Spieler-Standard (Test-cfg bleibt 0) */
    s->edge_fade_width = 10;   /* Nutzer 5.11.: 10 % */
    s->edge_fade_strength = 100;
    s->cockpit_persp = 0;       /* W191 UW-PERSP: Original */
    s->cockpit_panini = 0;      /* W191: Panini aus */
    s->remaster = 0;            /* W103: Original bitgenau */
    s->hd_textures = 0;
    s->hd_scale = 4;
    s->fx_enhanced = 0;
    s->ultrawide = 0;
    s->postfx = 0;
    memset(s->fx, 0, sizeof s->fx); /* W173: Effekte = Original (wie hcfg) */
    s->monitor_grain = 100;     /* W105: Original */
    s->monitor_glitch = 100;    /* W105: Original */
    s->vr_enable = 0;           /* W134 VR-PROTO: Standard aus */
    /* W159: VR-Startweg nutzt ein Auge als Standard-Vorschau (einstellbar). */
    strcpy_s(s->vr_preview, sizeof s->vr_preview, "left");
    lcfg_vr_preset_apply(s, 2); /* W183 VR-FIX3: Standard "Ausgewogen" (wie hcfg) */
#if SB_EDITION_COMMUNITY
    strcpy_s(s->lang, sizeof s->lang, "en");
#else
    strcpy_s(s->lang, sizeof s->lang, "de"); /* W142: private Erstsprache */
#endif
    s->rt_scale = 0;          /* W148; Steuer 6.11.: Spieler-Standard = Auto (fest), dynamisch nur auf Wunsch */
    s->rt_scale_dyn = 0; /* Steuer 6.11.: wie hcfg (V_C25p: Wechsel erzeugt Spitzen) */
    {   /* W131: Shader-Optionen = Original (wie hcfg.c) */
        int i;
        for (i = 0; i < LSHX_N; i++) {
            s->shx[i] = lcfg_shx_default(i);
        }
    }
}

/* W131 SHADER-OPT2: Keys, Standardwerte (= Client hcfg.c; W173: shading=1
 * wie der Client seit 5.11., vorher stand hier 0). Preset-Werte:
 * sb_gfxpreset.h (W173, einzige Quelle fuer Client und Launcher). */
static const char *const kShxKey[LSHX_N] = {
    "shading", "world_fog", "world_contrast", "world_gamma", "world_sharpen", "bloom_strength",
    "cockpit_glow", "cockpit_spill", "cockpit_light", "world_exposure", "tonemap", "bloom_threshold",
    "light_world", "light_cockpit", "world_focus", "aa_mode", "ssao", "color_sat", "color_temp",
    "color_lift", "color_gamma", "color_gain", "shade_shadow", "shadow_soft", "hud_label_avoid"
};
static const int kShxDef[LSHX_N] = {
    1, 100, 100, 100, 0, 100, 0, 0, 100, 100, 0, 0, 100, 100, 100, 0, 0, 100, 0, 0, 100, 100, 0, 0, 0
};
static const char *const kFxKey[LFX_N] = { /* W173: Reihenfolge = sb_gfxpreset.h */
    "fx_soft", "fx_embers", "fx_light", "fx_light_cockpit", "fx_interp"
};

const char *lcfg_shx_key(int i)
{
    return (i >= 0 && i < LSHX_N) ? kShxKey[i] : "";
}

/* W183 VR-FIX3: Spiegel der Tabelle kVrPreset in vr.c (Client). */
static const int kVrPre[4][6] = {
    { 0, 0, 0, 0, 0, 0 },
    { 65, 72, 50, 1, 0, 1 },   /* Leistung */
    { 80, 0, 100, 1, 1, 2 },   /* Ausgewogen (Standard) */
    { 100, 0, 100, 0, 1, 0 },  /* Qualitaet */
};

int lcfg_vr_preset_value(int preset, int k)
{
    if (preset < 1 || preset > 3 || k < 0 || k > 5) { return -1; }
    return kVrPre[preset][k];
}

void lcfg_vr_preset_apply(launcher_settings_t *s, int pr)
{
    if (pr < 1 || pr > 3) { s->vr_preset = 0; return; }
    s->vr_render_scale = kVrPre[pr][0];
    s->vr_refresh_hz = kVrPre[pr][1];
    s->vr_mirror_res = kVrPre[pr][2];
    s->vr_world_tick = kVrPre[pr][3];
    s->vr_shading = kVrPre[pr][4];
    s->vr_rt_scale = kVrPre[pr][5];
    s->vr_preset = pr;
}

int lcfg_vr_preset_match(const launcher_settings_t *s)
{
    int pr;
    for (pr = 1; pr <= 3; pr++) {
        if (s->vr_render_scale == kVrPre[pr][0] && s->vr_refresh_hz == kVrPre[pr][1] &&
            s->vr_mirror_res == kVrPre[pr][2] && (s->vr_world_tick ? 1 : 0) == kVrPre[pr][3] &&
            (s->vr_shading ? 1 : 0) == kVrPre[pr][4] && s->vr_rt_scale == kVrPre[pr][5]) {
            return pr;
        }
    }
    return 0;
}

int lcfg_shx_default(int i)
{
    return (i >= 0 && i < LSHX_N) ? kShxDef[i] : 0;
}

const char *lcfg_fx_key(int i)
{
    return (i >= 0 && i < LFX_N) ? kFxKey[i] : "";
}

/* ------------------------------------------------------------------ */
/* W173 LAUNCHER-P4: einheitliches Grafik-Preset                        */
/* ------------------------------------------------------------------ */
const char *lcfg_gfx_key(int k)
{
    if (k >= 0 && k < LSHX_N) { return kShxKey[k]; }
    if (k >= LGK_FX0 && k < LGK_FX0 + LFX_N) { return kFxKey[k - LGK_FX0]; }
    if (k == LGK_FPS) { return "interp_fps"; }
    if (k == LGK_RT) { return "rt_scale"; }
    if (k == LGK_DYN) { return "rt_scale_dyn"; }
    if (k == LGK_MATCH) { return "fs_interp_match"; }
    return k == LGK_HD ? "hd_textures" : "";
}

int lcfg_gfx_owned(int k)
{
    return k >= 0 && k < LGK_N && k != LSHX_LABEL;
}

/* Zeile in sb_gfxpreset.h (-1 = nicht im Preset). */
static int gfx_row(int k)
{
    static int row[LGK_N], init;
    if (!init) {
        int i;
        for (i = 0; i < LGK_N; i++) {
            row[i] = lcfg_gfx_owned(i) ? sb_gp_find(lcfg_gfx_key(i)) : -1;
        }
        init = 1;
    }
    return (k >= 0 && k < LGK_N) ? row[k] : -1;
}

int lcfg_gfx_value(const launcher_settings_t *s, int k)
{
    if (k >= 0 && k < LSHX_N) { return s->shx[k]; }
    if (k == LGK_FX0 + LFX_INTERP) { return s->fx[LFX_INTERP] != 0; }
    if (k >= LGK_FX0 && k < LGK_FX0 + LFX_N) { return s->fx[k - LGK_FX0]; }
    if (k == LGK_FPS) { return s->interp_fps; }
    if (k == LGK_RT) { return s->rt_scale; }
    if (k == LGK_DYN) { return s->rt_scale_dyn; }
    if (k == LGK_MATCH) { return s->fs_interp_match; }
    if (k == LGK_HD) { return (s->remaster && s->hd_textures) ? 1 : 0; }
    return 0;
}

int lcfg_gfx_preset_value(int preset, int k)
{
    int r = gfx_row(k);
    if (r < 0 || preset < 0 || preset >= SB_GP_NPRE) { return -1; }
    if (k == LGK_FX0 + LFX_INTERP) { return kSbGfxPreset[r].v[preset] != 0; }
    return kSbGfxPreset[r].v[preset];
}

void lcfg_gfx_lock_key(lgp_mask_t *m, const char *key)
{
    int k;
    for (k = 0; k < LGK_N; k++) {
        if (lcfg_gfx_owned(k) && strcmp(lcfg_gfx_key(k), key) == 0) {
            *m |= (lgp_mask_t)1 << k;
            return;
        }
    }
}

int lcfg_gfx_diff(const launcher_settings_t *s, int p, lgp_mask_t lock,
                  lgp_mask_t *mask)
{
    int k, n = 0;
    lgp_mask_t m = 0;
    for (k = 0; k < LGK_N; k++) {
        if (!lcfg_gfx_owned(k) || (lock & ((lgp_mask_t)1 << k))) {
            continue;
        }
        if (lcfg_gfx_value(s, k) != lcfg_gfx_preset_value(p, k)) {
            m |= (lgp_mask_t)1 << k;
            n++;
        }
    }
    if (mask) {
        *mask = m;
    }
    return n;
}

int lcfg_gfx_preset(const launcher_settings_t *s, lgp_mask_t lock)
{
    int p;
    for (p = 0; p < SB_GP_NPRE; p++) {
        if (lcfg_gfx_diff(s, p, lock, NULL) == 0) {
            return p;
        }
    }
    return LGP_CUSTOM;
}

int lcfg_gfx_base(const launcher_settings_t *s, lgp_mask_t lock)
{
    int p, best = 0, bn = 1 << 30;
    for (p = 0; p < SB_GP_NPRE; p++) {
        int n = lcfg_gfx_diff(s, p, lock, NULL);
        if (n < bn) {
            bn = n;
            best = p;
        }
    }
    return best;
}

void lcfg_gfx_normalize(launcher_settings_t *s)
{
    int i, any = 0;
    s->remaster = s->hd_textures = (s->remaster && s->hd_textures) ? 1 : 0;
    for (i = 0; i < LFX_N; i++) {
        if (s->fx[i] > 0) {
            any = 1;
        }
    }
    s->fx_enhanced = any;
    s->ultrawide = 0;
    s->postfx = 0;
}

void lcfg_gfx_apply(launcher_settings_t *s, int preset)
{
    int k;
    if (preset < 0 || preset >= SB_GP_NPRE) {
        return; /* Benutzerdefiniert: Einzelwerte bleiben */
    }
    for (k = 0; k < LGK_N; k++) {
        int v = lcfg_gfx_preset_value(preset, k);
        if (v < 0) {
            continue;
        }
        if (k < LSHX_N) {
            s->shx[k] = v;
        } else if (k < LGK_FX0 + LFX_N) {
            s->fx[k - LGK_FX0] = v;
        } else if (k == LGK_FPS) { s->interp_fps = v;
        } else if (k == LGK_RT) { s->rt_scale = v;
        } else if (k == LGK_DYN) { s->rt_scale_dyn = v;
        } else if (k == LGK_MATCH) { s->fs_interp_match = v;
        } else {
            s->remaster = s->hd_textures = v;
        }
    }
    lcfg_gfx_normalize(s);
}

void lcfg_gfx_migrate(launcher_settings_t *s)
{
    int i, any = 0;
    for (i = 0; i < LFX_N; i++) {
        if (s->fx[i]) {
            any = 1;
        }
    }
    if (s->remaster && s->fx_enhanced && !any) {
        lcfg_fx_apply_level(s, LGP_SUBTLE);
    }
}

int lcfg_fx_level(const launcher_settings_t *s, lgp_mask_t lock)
{
    int p, k;
    for (p = 0; p <= LGP_STRONG; p++) {
        for (k = LGK_FX0; k < LGK_FX0 + LFX_N; k++) {
            if (!(lock & ((lgp_mask_t)1 << k)) &&
                lcfg_gfx_value(s, k) != lcfg_gfx_preset_value(p, k)) {
                break;
            }
        }
        if (k == LGK_FX0 + LFX_N) {
            return p;
        }
    }
    return LGP_CUSTOM;
}

void lcfg_fx_apply_level(launcher_settings_t *s, int level)
{
    int k;
    if (level < 0 || level > LGP_STRONG) {
        return;
    }
    for (k = LGK_FX0; k < LGK_FX0 + LFX_N; k++) {
        s->fx[k - LGK_FX0] = lcfg_gfx_preset_value(level, k);
    }
}

static void set_key(launcher_settings_t *s, const char *k, const char *v)
{
    if (strcmp(k, "xbe_path") == 0) {
        strcpy_s(s->xbe_path, sizeof s->xbe_path, v);
    } else if (strcmp(k, "media_root") == 0) {
        strcpy_s(s->media_root, sizeof s->media_root, v);
    } else if (strcmp(k, "hdd_root") == 0) {
        strcpy_s(s->hdd_root, sizeof s->hdd_root, v);
    } else if (strcmp(k, "game_image") == 0) { /* W150 */
        strcpy_s(s->game_image, sizeof s->game_image, v);
    } else if (strcmp(k, "width") == 0) {
        s->width = (unsigned)atoi(v);
    } else if (strcmp(k, "height") == 0) {
        s->height = (unsigned)atoi(v);
    } else if (strcmp(k, "present_interval") == 0) {
        s->present_interval = atoi(v);
    } else if (strcmp(k, "fs_interp_match") == 0) {
        s->fs_interp_match = atoi(v) ? 1 : 0;
    } else if (strcmp(k, "fps_limit") == 0) {
        s->fps_limit = atoi(v);
    } else if (strcmp(k, "audio") == 0) {
        s->audio = atoi(v);
    } else if (strcmp(k, "audio_live") == 0) {
        s->audio_live = atoi(v);
    } else if (strcmp(k, "aspect_mode") == 0) {
        s->aspect_mode = atoi(v);
    } else if (strcmp(k, "input_profile") == 0) {
        strncpy_s(s->input_profile, sizeof s->input_profile, v,
                    sizeof s->input_profile - 1); /* kuerzt statt Fehler */
    } else if (strcmp(k, "sbc_usb") == 0) {
        s->sbc_usb = atoi(v);
    } else if (strcmp(k, "sbc_mouse_off") == 0) {
        s->sbc_mouse_off = atoi(v);
    } else if (strcmp(k, "aim_turn") == 0) { /* W218 */
        s->aim_turn = atoi(v) ? 1 : 0;
    } else if (strcmp(k, "auto_gear") == 0) { /* W218 */
        s->auto_gear = atoi(v) ? 1 : 0;
    } else if (strcmp(k, "save_lock") == 0) { /* W190 */
        s->save_lock = atoi(v) ? 1 : 0;
    } else if (strcmp(k, "photo_dir") == 0) { /* W195 */
        strncpy_s(s->photo_dir, sizeof s->photo_dir, v, _TRUNCATE);
    } else if (strcmp(k, "photo_scale") == 0) {
        int n = atoi(v);
        s->photo_scale = (n == 2 || n == 4 || n == 8) ? n : 1;
    } else if (strcmp(k, "photo_format") == 0) {
        s->photo_jpg = (_stricmp(v, "jpg") == 0 || _stricmp(v, "jpeg") == 0) ? 1 : 0;
    } else if (strcmp(k, "photo_aspect") == 0) {
        int n = atoi(v);
        s->photo_aspect = (n >= 0 && n <= 2) ? n : 1;
    } else if (strcmp(k, "interp_fps") == 0) {
        s->interp_fps = atoi(v);
    } else if (strcmp(k, "vr_enable") == 0) { /* W134 */
        s->vr_enable = atoi(v) ? 1 : 0;
    } else if (strcmp(k, "vr_preview") == 0) {
        strncpy_s(s->vr_preview, sizeof s->vr_preview, v, sizeof s->vr_preview - 1);
    } else if (strcmp(k, "vr_preset") == 0) { /* W183 VR-FIX3 */
        s->vr_preset = atoi(v);
    } else if (strcmp(k, "vr_render_scale") == 0) {
        s->vr_render_scale = atoi(v);
    } else if (strcmp(k, "vr_refresh_hz") == 0) {
        s->vr_refresh_hz = atoi(v);
    } else if (strcmp(k, "vr_mirror_res") == 0) {
        s->vr_mirror_res = atoi(v);
    } else if (strcmp(k, "vr_world_tick") == 0) {
        s->vr_world_tick = atoi(v) ? 1 : 0;
    } else if (strcmp(k, "vr_shading") == 0) {
        s->vr_shading = atoi(v) ? 1 : 0;
    } else if (strcmp(k, "vr_rt_scale") == 0) {
        s->vr_rt_scale = atoi(v);
    } else if (strcmp(k, "lang") == 0) { /* W142: nur en/de gelten */
        if (_stricmp(v, "de") == 0) {
            strcpy_s(s->lang, sizeof s->lang, "de");
        } else {
            strcpy_s(s->lang, sizeof s->lang, "en");
        }
    } else if (strcmp(k, "window_mode") == 0) {
        s->window_mode = atoi(v);
    } else if (strcmp(k, "fs_hz") == 0) {
        s->fs_hz = atoi(v);
    } else if (strcmp(k, "mouse_mode") == 0) {
        /* W87: absolute|relative (wie hcfg, auch 1/0) */
        s->mouse_mode = (_stricmp(v, "absolute") == 0 ||
                         _stricmp(v, "absolut") == 0 || atoi(v) == 1) ? 1 : 0;
    } else if (strcmp(k, "mouse_sens") == 0) {
        s->mouse_sens = atoi(v);
    } else if (strcmp(k, "edge_mode") == 0) { /* W107 */
        s->edge_mode = atoi(v);
    } else if (strcmp(k, "cockpit_persp") == 0) { /* W191 */
        s->cockpit_persp = atoi(v);
    } else if (strcmp(k, "cockpit_panini") == 0) { /* W191 */
        s->cockpit_panini = atoi(v);
    } else if (strcmp(k, "edge_fade_width") == 0) {
        s->edge_fade_width = atoi(v);
    } else if (strcmp(k, "edge_fade_strength") == 0) {
        s->edge_fade_strength = atoi(v);
    } else if (strcmp(k, "remaster") == 0) { /* W103 */
        s->remaster = atoi(v) ? 1 : 0;
    } else if (strcmp(k, "hd_textures") == 0) {
        s->hd_textures = atoi(v) ? 1 : 0;
    } else if (strcmp(k, "hd_scale") == 0) {
        /* W114: nur 2 oder 4 (Freigaben aus remaster/selection.json gelten
         * immer); alt 0 (W111 "Auswahl") wird zu 4 umgedeutet */
        s->hd_scale = (atoi(v) == 2) ? 2 : 4;
    } else if (strcmp(k, "fx_enhanced") == 0) {
        s->fx_enhanced = atoi(v) ? 1 : 0;
    } else if (strcmp(k, "ultrawide") == 0) {
        s->ultrawide = atoi(v) ? 1 : 0;
    } else if (strcmp(k, "postfx") == 0) {
        s->postfx = atoi(v) ? 1 : 0;
    } else if (strcmp(k, "monitor_grain") == 0) {
        s->monitor_grain = atoi(v);
    } else if (strcmp(k, "monitor_glitch") == 0) {
        s->monitor_glitch = atoi(v);
    } else if (strcmp(k, "rt_scale") == 0) { /* W148 */
        s->rt_scale = (_stricmp(v, "auto") == 0) ? 0 : atoi(v);
        if (s->rt_scale < 0) { s->rt_scale = 0; }
        if (s->rt_scale > 4) { s->rt_scale = 4; }
    } else if (strcmp(k, "rt_scale_dyn") == 0) {
        s->rt_scale_dyn = atoi(v) ? 1 : 0;
    } else {
        int i; /* W131: Shader-Keys */
        for (i = 0; i < LSHX_N; i++) {
            if (strcmp(k, kShxKey[i]) == 0) {
                s->shx[i] = (i == LSHX_TONE && _stricmp(v, "reinhard") == 0) ? 1
                            : (i == LSHX_TONE && _stricmp(v, "aces") == 0) ? 2 : atoi(v);
                return;
            }
        }
        for (i = 0; i < LFX_N; i++) { /* W173: Effekt-Keys */
            if (strcmp(k, kFxKey[i]) == 0) {
                s->fx[i] = atoi(v);
                return;
            }
        }
    }
    /* Unbekannte Keys: ignorieren (gehoeren dem Client). */
}

static int is_path_key(const char *k)
{
    return strcmp(k, "xbe_path") == 0 || strcmp(k, "media_root") == 0 ||
           strcmp(k, "hdd_root") == 0 || strcmp(k, "game_image") == 0 ||
           strcmp(k, "photo_dir") == 0; /* W195 PHOTO-MODE */
}

int lcfg_load_file_count(const char *path, launcher_settings_t *s, int *keys)
{
    FILE *f;
    char line[4096];
    int n = 0;

    if (keys) {
        *keys = 0;
    }
    if (u8_fopen_s(&f, path, "r") != 0 || !f) {
        return 1;
    }
    while (fgets(line, sizeof line, f)) {
        char *eq, *k, *v, *end;
        k = line + strspn(line, " \t");
        if (*k == '#' || *k == ';' || *k == '[' || *k == '\n' || *k == '\0') {
            continue;
        }
        eq = strchr(k, '=');
        if (!eq) {
            continue; /* kaputte Zeile: ueberspringen, Rest gilt weiter */
        }
        *eq = '\0';
        v = eq + 1 + strspn(eq + 1, " \t");
        end = v + strcspn(v, "\r\n");
        while (end > v && (end[-1] == ' ' || end[-1] == '\t')) {
            end--;
        }
        *end = '\0';
        end = k + strlen(k);
        while (end > k && (end[-1] == ' ' || end[-1] == '\t')) {
            end--;
        }
        *end = '\0';
        if (is_path_key(k)) {
            /* W82: cfg-Dateien sind ANSI (Client liest mit A-API),
             * intern UTF-8. ASCII bleibt unveraendert. */
            char u8v[2048];
            u8_from_acp(v, u8v, sizeof u8v);
            set_key(s, k, u8v);
        } else {
            set_key(s, k, v);
        }
        n++;
    }
    fclose(f);
    if (keys) {
        *keys = n;
    }
    return 0;
}

int lcfg_load_file(const char *path, launcher_settings_t *s)
{
    return lcfg_load_file_count(path, s, NULL);
}

/* Verwaltete Keys mit Ziel-Section fuer neue user.cfg-Dateien. */
static const char *kManaged[][2] = {
    { "xbe_path", "xbe" },         { "media_root", "game" },
    { "hdd_root", "game" },         { "game_image", "game" }, /* W150 */
    { "width", "display" },
    { "height", "display" },        { "present_interval", "display" },
    { "fs_interp_match", "render" },
    { "fps_limit", "display" },     { "audio", "audio" },
    { "audio_live", "audio" },      { "aspect_mode", "render" },
    { "input_profile", "input" },   { "sbc_usb", "input" },
    { "sbc_mouse_off", "input" }, /* W128 */
    { "aim_turn", "input" },       { "auto_gear", "input" }, /* W218 */
    { "save_lock", "game" }, /* W190 */
    { "photo_dir", "photo" },       { "photo_scale", "photo" }, /* W195 PHOTO-MODE */
    { "photo_format", "photo" },    { "photo_aspect", "photo" },
    { "interp_fps", "render" },
    { "window_mode", "display" },   { "fs_hz", "display" },
    { "mouse_mode", "input" },      { "mouse_sens", "input" },
    { "edge_mode", "render" },      { "edge_fade_width", "render" }, /* W107 */
    { "edge_fade_strength", "render" },
    { "cockpit_persp", "render" }, /* W191 */
    { "cockpit_panini", "render" }, /* W191 */
    { "remaster", "remaster" },     { "hd_textures", "remaster" }, /* W103 */
    { "hd_scale", "remaster" },     { "fx_enhanced", "remaster" },
    { "ultrawide", "remaster" },    { "postfx", "remaster" },
    { "monitor_grain", "render" },  { "monitor_glitch", "render" },
    /* W131 SHADER-OPT2 (Reihenfolge = kShxKey) */
    { "shading", "render" },        { "world_fog", "render" },
    { "world_contrast", "render" }, { "world_gamma", "render" },
    { "world_sharpen", "render" },  { "bloom_strength", "render" },
    { "cockpit_glow", "render" },   { "cockpit_spill", "render" },
    { "cockpit_light", "render" },  { "world_exposure", "render" },
    { "tonemap", "render" },        { "bloom_threshold", "render" },
    { "light_world", "render" },    { "light_cockpit", "render" },
    { "world_focus", "render" },    { "aa_mode", "render" },
    { "ssao", "render" },           { "color_sat", "render" },
    { "color_temp", "render" },     { "color_lift", "render" },
    { "color_gamma", "render" },    { "color_gain", "render" },
    { "shade_shadow", "render" },   { "shadow_soft", "render" },
    { "hud_label_avoid", "render" }, /* W153 */
    /* W173 LAUNCHER-P4: Effekte gehoeren zum Grafik-Preset (vorher nur ESC) */
    { "fx_soft", "render" },        { "fx_embers", "render" },
    { "fx_light", "render" },       { "fx_light_cockpit", "render" },
    { "fx_interp", "render" },
    /* W134 VR-PROTO */
    { "vr_enable", "vr" },          { "vr_preview", "vr" },
    /* W183 VR-FIX3: VR-Leistung */
    { "vr_preset", "vr" },          { "vr_render_scale", "vr" },
    { "vr_refresh_hz", "vr" },      { "vr_mirror_res", "vr" },
    { "vr_world_tick", "vr" },      { "vr_shading", "vr" },
    { "vr_rt_scale", "vr" },
    /* W142 L10N-EN */
    { "rt_scale", "render" },   { "rt_scale_dyn", "render" }, /* W148 */
    { "lang", "ui" },
};
#define KMANAGED_N (sizeof kManaged / sizeof kManaged[0])

/* W172 LAUNCHER-P6: Programmdatei. Abbild-Modus und gesetztes Feld:
 * unveraendert xbe_path; Ordner-Modus mit leerem Feld: <Spielordner>/
 * default.xbe (Trenner wie im Ordnerpfad). Kein Ordner: "". */
void lcfg_xbe_effective(const launcher_settings_t *s, char *out, size_t cap)
{
    size_t n;
    if (s->game_image[0] || s->xbe_path[0] || !s->media_root[0]) {
        strcpy_s(out, cap, s->xbe_path);
        return;
    }
    strcpy_s(out, cap, s->media_root);
    n = strlen(out);
    if (n > 0 && out[n - 1] != '/' && out[n - 1] != '\\') {
        strcat_s(out, cap, strchr(s->media_root, '\\') ? "\\" : "/");
    }
    strcat_s(out, cap, "default.xbe");
}

static void key_value(const launcher_settings_t *s, const char *k,
                      char *out, size_t cap)
{
    /* W82: Pfade intern UTF-8, in der Datei ANSI (Client: A-API);
     * nicht darstellbar -> 8.3-Kurzname (lcfg_validate meldet den Rest). */
    if (strcmp(k, "xbe_path") == 0) {
        /* W172: Ordner-Modus ohne Feld -> aufgeloester Pfad (Client braucht
         * ihn, Konzept 8.1); sonst unveraendert. */
        char x[1024];
        lcfg_xbe_effective(s, x, sizeof x);
        u8_to_acp(x, out, cap);
    } else if (strcmp(k, "media_root") == 0) {
        u8_to_acp(s->media_root, out, cap);
    } else if (strcmp(k, "hdd_root") == 0) {
        u8_to_acp(s->hdd_root, out, cap);
    } else if (strcmp(k, "game_image") == 0) { /* W150 */
        u8_to_acp(s->game_image, out, cap);
    } else if (strcmp(k, "photo_dir") == 0) { /* W195 PHOTO-MODE */
        u8_to_acp(s->photo_dir, out, cap);
    } else if (strcmp(k, "photo_format") == 0) {
        strcpy_s(out, cap, s->photo_jpg ? "jpg" : "png");
    } else if (strcmp(k, "input_profile") == 0) {
        strcpy_s(out, cap, s->input_profile);
    } else if (strcmp(k, "mouse_mode") == 0) {
        strcpy_s(out, cap, s->mouse_mode ? "absolute" : "relative");
    } else if (strcmp(k, "vr_preview") == 0) { /* W134, W159: Standard ein Auge */
        strcpy_s(out, cap, s->vr_preview[0] ? s->vr_preview : "left");
    } else if (strcmp(k, "lang") == 0) { /* W142 */
        strcpy_s(out, cap, s->lang[0] ? s->lang : "en");
    } else if (strcmp(k, "rt_scale") == 0) { /* W148: 0 = auto */
        if (s->rt_scale <= 0) { strcpy_s(out, cap, "auto"); } else { _snprintf_s(out, cap, _TRUNCATE, "%d", s->rt_scale); }
    } else {
        int v = 0;
        if (strcmp(k, "width") == 0) {
            v = (int)s->width;
        } else if (strcmp(k, "height") == 0) {
            v = (int)s->height;
        } else if (strcmp(k, "present_interval") == 0) {
            v = s->present_interval;
        } else if (strcmp(k, "fs_interp_match") == 0) {
            v = s->fs_interp_match;
        } else if (strcmp(k, "fps_limit") == 0) {
            v = s->fps_limit;
        } else if (strcmp(k, "audio") == 0) {
            v = s->audio;
        } else if (strcmp(k, "audio_live") == 0) {
            v = s->audio_live;
        } else if (strcmp(k, "aspect_mode") == 0) {
            v = s->aspect_mode;
        } else if (strcmp(k, "sbc_usb") == 0) {
            v = s->sbc_usb;
        } else if (strcmp(k, "sbc_mouse_off") == 0) {
            v = s->sbc_mouse_off;
        } else if (strcmp(k, "aim_turn") == 0) { /* W218 */
            v = s->aim_turn ? 1 : 0;
        } else if (strcmp(k, "auto_gear") == 0) { /* W218 */
            v = s->auto_gear ? 1 : 0;
        } else if (strcmp(k, "save_lock") == 0) { /* W190 */
            v = s->save_lock ? 1 : 0;
        } else if (strcmp(k, "photo_aspect") == 0) { /* W195 */
            v = (s->photo_aspect >= 0 && s->photo_aspect <= 2) ? s->photo_aspect : 1;
        } else if (strcmp(k, "photo_scale") == 0) { /* W195 */
            v = (s->photo_scale == 2 || s->photo_scale == 4 || s->photo_scale == 8) ? s->photo_scale : 1;
        } else if (strcmp(k, "interp_fps") == 0) {
            v = s->interp_fps;
        } else if (strcmp(k, "window_mode") == 0) {
            v = s->window_mode;
        } else if (strcmp(k, "fs_hz") == 0) {
            v = s->fs_hz;
        } else if (strcmp(k, "mouse_sens") == 0) {
            v = s->mouse_sens;
        } else if (strcmp(k, "edge_mode") == 0) { /* W107 */
            v = s->edge_mode;
        } else if (strcmp(k, "edge_fade_width") == 0) {
            v = s->edge_fade_width;
        } else if (strcmp(k, "edge_fade_strength") == 0) {
            v = s->edge_fade_strength;
        } else if (strcmp(k, "cockpit_persp") == 0) { /* W191 */
            v = s->cockpit_persp;
        } else if (strcmp(k, "cockpit_panini") == 0) { /* W191 */
            v = s->cockpit_panini;
        } else if (strcmp(k, "remaster") == 0) { /* W103 */
            v = s->remaster;
        } else if (strcmp(k, "hd_textures") == 0) {
            v = s->hd_textures;
        } else if (strcmp(k, "hd_scale") == 0) {
            v = s->hd_scale;
        } else if (strcmp(k, "fx_enhanced") == 0) {
            v = s->fx_enhanced;
        } else if (strcmp(k, "ultrawide") == 0) {
            v = s->ultrawide;
        } else if (strcmp(k, "postfx") == 0) {
            v = s->postfx;
        } else if (strcmp(k, "monitor_grain") == 0) {
            v = s->monitor_grain;
        } else if (strcmp(k, "monitor_glitch") == 0) {
            v = s->monitor_glitch;
        } else if (strcmp(k, "rt_scale_dyn") == 0) { /* W148 */
            v = s->rt_scale_dyn;
        } else if (strcmp(k, "vr_enable") == 0) { /* W134 */
            v = s->vr_enable;
        } else if (strcmp(k, "vr_preset") == 0) { /* W183 */
            v = s->vr_preset;
        } else if (strcmp(k, "vr_render_scale") == 0) {
            v = s->vr_render_scale;
        } else if (strcmp(k, "vr_refresh_hz") == 0) {
            v = s->vr_refresh_hz;
        } else if (strcmp(k, "vr_mirror_res") == 0) {
            v = s->vr_mirror_res;
        } else if (strcmp(k, "vr_world_tick") == 0) {
            v = s->vr_world_tick;
        } else if (strcmp(k, "vr_shading") == 0) {
            v = s->vr_shading;
        } else if (strcmp(k, "vr_rt_scale") == 0) {
            v = s->vr_rt_scale;
        } else {
            int i; /* W131: Shader-Keys */
            for (i = 0; i < LSHX_N; i++) {
                if (strcmp(k, kShxKey[i]) == 0) {
                    v = s->shx[i];
                    break;
                }
            }
            for (i = 0; i < LFX_N; i++) { /* W173 */
                if (strcmp(k, kFxKey[i]) == 0) {
                    v = s->fx[i];
                    break;
                }
            }
        }
        _snprintf_s(out, cap, _TRUNCATE, "%d", v);
    }
}

/* Passt Zeile (ohne \r\n) auf "key ="? */
static int line_is_key(const char *line, const char *key)
{
    size_t n;
    line += strspn(line, " \t");
    n = strlen(key);
    if (strncmp(line, key, n) != 0) {
        return 0;
    }
    line += n + strspn(line + n, " \t");
    return *line == '=';
}

int lcfg_save_user(const char *path, const launcher_settings_t *s)
{
    /* Bis 1 MB alte Datei erhalten (user.cfg ist real < 4 KB). */
    static char *out_text;
    size_t i, out_len = 0, out_cap;
    char line[4096];
    char kval[512];
    int done[KMANAGED_N];
    FILE *f;

    memset(done, 0, sizeof done);
    if (u8_fopen_s(&f, path, "r") != 0 || !f) {
        /* Neu anlegen, gruppiert nach Section. */
        const char *sec = NULL;
        if (u8_fopen_s(&f, path, "w") != 0 || !f) {
            return -1;
        }
        fputs("# user.cfg - vom Launcher geschrieben, ueberschreibt sb_native.cfg\n",
              f);
        for (i = 0; i < KMANAGED_N; i++) {
            if (!sec || strcmp(sec, kManaged[i][1]) != 0) {
                sec = kManaged[i][1];
                fprintf(f, "[%s]\n", sec);
            }
            key_value(s, kManaged[i][0], kval, sizeof kval);
            fprintf(f, "%s=%s\n", kManaged[i][0], kval);
        }
        fclose(f);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    out_cap = (size_t)ftell(f) + 4096;
    if (out_cap > 1024 * 1024 + 4096) {
        fclose(f);
        return -1;
    }
    fseek(f, 0, SEEK_SET);
    out_text = (char *)malloc(out_cap);
    if (!out_text) {
        fclose(f);
        return -1;
    }
    /* Alte Zeilen uebernehmen, verwaltete Keys ersetzen (erstes Treffen). */
    while (fgets(line, sizeof line, f)) {
        char *nl = line + strcspn(line, "\r\n");
        int replaced = 0;
        *nl = '\0';
        for (i = 0; i < KMANAGED_N; i++) {
            if (!done[i] && line_is_key(line, kManaged[i][0])) {
                key_value(s, kManaged[i][0], kval, sizeof kval);
                out_len += (size_t)_snprintf_s(out_text + out_len,
                                               out_cap - out_len, _TRUNCATE,
                                               "%s=%s\n", kManaged[i][0], kval);
                done[i] = 1;
                replaced = 1;
                break;
            }
        }
        if (!replaced) {
            out_len += (size_t)_snprintf_s(out_text + out_len,
                                           out_cap - out_len, _TRUNCATE, "%s\n",
                                           line);
        }
    }
    fclose(f);
    /* Fehlende Keys ans Ende (eigene Section falls noetig). */
    for (i = 0; i < KMANAGED_N; i++) {
        if (!done[i]) {
            char hdr[64];
            _snprintf_s(hdr, sizeof hdr, _TRUNCATE, "[%s]", kManaged[i][1]);
            if (!strstr(out_text, hdr)) {
                out_len += (size_t)_snprintf_s(out_text + out_len,
                                               out_cap - out_len, _TRUNCATE,
                                               "%s\n", hdr);
            }
            key_value(s, kManaged[i][0], kval, sizeof kval);
            out_len += (size_t)_snprintf_s(out_text + out_len,
                                           out_cap - out_len, _TRUNCATE,
                                           "%s=%s\n", kManaged[i][0], kval);
        }
    }
    if (u8_fopen_s(&f, path, "w") != 0 || !f) {
        free(out_text);
        out_text = NULL;
        return -1;
    }
    fwrite(out_text, 1, out_len, f);
    fclose(f);
    free(out_text);
    out_text = NULL;
    return 0;
}

/* W79 INPUT-REAL: Spielerbetrieb-Block. sb_native.cfg ist testorientiert
 * (A1-Menue-Replay, Turbo, Auto-Screenshots, Audio-Mitschnitt); solange
 * replay_steps/replay/movie_skip gesetzt sind, ignoriert der Client jede
 * echte Tastatur/Maus (hooks_bots.c: input_replay_active). Der Launcher
 * neutralisiert diese Vorgaben beim Start in user.cfg (letzter gewinnt,
 * leerer Wert leert im Client wirklich). Testlaeufe (run_once.ps1) setzen
 * SB_NO_USERCFG=1 und sehen den Block nie. */
static const char *const kPlayer[][3] = {
    /* key, section, Wert */
    { "replay_steps", "input", "" },     /* A1-Menue-Replay aus */
    { "replay", "input", "" },           /* Frame-Replay aus */
    { "movie_skip", "input", "0" },      /* Filme nicht automatisch ueberspringen */
    { "mouse_capture", "input", "1" },   /* W79: Maus fangen (F10) */
    { "input_focus_only", "input", "1" },/* W79: echte Eingabe nur mit Fokus */
    { "key_toggles", "input", "1" },     /* W79: Tasten 1-5 = Kippschalter */
    { "input_source", "input", "1" },    /* W79: Tasten aus Fenster-Nachrichten */
    { "key_tap_queue", "input", "1" },  /* W92: schnelle Tipper nicht verlieren */
    { "input_test_hook", "input", "0" }, /* W79: Test-Hook nie im Spielerbetrieb */
    { "turbo_until_mode", "display", "0" },  /* kein Menue-Zeitraffer */
    { "turbo_until_frame", "display", "0" },
    { "window_hidden", "display", "0" },
    { "screenshot_frame", "capture", "0" },  /* keine Auto-Screenshots */
    { "screenshot_every", "capture", "0" },
    { "audio_capture", "audio", "0" },       /* kein audio.wav-Mitschnitt */
    /* W170 LAUNCHER-P1/B12: rt_scale gehoert dem Spieler (UI-Wahl aus
     * lcfg_save_user: auto oder fest 1-4) und wird hier nicht mehr auf
     * auto zurueckgesetzt. Spieler-Standard bleibt auto (lcfg_defaults);
     * Tests lesen sb_native.cfg ohne user.cfg (SB_NO_USERCFG) und sehen
     * weiter rt_scale=1. */
};
#define KPLAYER_N (sizeof kPlayer / sizeof kPlayer[0])

int lcfg_player_block_n(void)
{
    return (int)KPLAYER_N;
}

const char *lcfg_player_block_key(int i, const char **value)
{
    if (i < 0 || i >= (int)KPLAYER_N) return NULL;
    if (value) *value = kPlayer[i][2];
    return kPlayer[i][0];
}

int lcfg_write_player_block(const char *path)
{
    static char *out_text;
    size_t i, out_len = 0, out_cap = 1024 * 1024 + 64 * 1024;
    char line[4096];
    int done[KPLAYER_N];
    int added_hdr = 0;
    const char *last_sec = NULL;
    FILE *f;

    memset(done, 0, sizeof done);
    /* user.cfg ist real < 4 KB; bis 1 MB alte Datei erhalten. */
    out_text = (char *)malloc(out_cap);
    if (!out_text) return -1;
    out_text[0] = '\0';
    if (u8_fopen_s(&f, path, "r") == 0 && f) {
        while (fgets(line, sizeof line, f)) {
            char *nl = line + strcspn(line, "\r\n");
            int replaced = 0;
            *nl = '\0';
            for (i = 0; i < KPLAYER_N; i++) {
                if (line_is_key(line, kPlayer[i][0])) {
                    /* erstes Vorkommen ersetzen, weitere Dubletten fallen weg */
                    if (!done[i]) {
                        out_len += (size_t)_snprintf_s(
                            out_text + out_len, out_cap - out_len, _TRUNCATE,
                            "%s=%s\n", kPlayer[i][0], kPlayer[i][2]);
                        done[i] = 1;
                    }
                    replaced = 1;
                    break;
                }
            }
            if (!replaced && out_len + strlen(line) + 2 < out_cap) {
                out_len += (size_t)_snprintf_s(out_text + out_len,
                                               out_cap - out_len, _TRUNCATE,
                                               "%s\n", line);
            }
        }
        fclose(f);
    }
    for (i = 0; i < KPLAYER_N; i++) {
        if (done[i]) continue;
        if (!added_hdr) {
            out_len += (size_t)_snprintf_s(
                out_text + out_len, out_cap - out_len, _TRUNCATE,
                "# W79 Spielerbetrieb (Launcher-Start): Test-Vorgaben aus "
                "sb_native.cfg neutralisiert, echte Eingabe an\n");
            added_hdr = 1;
        }
        if (!last_sec || strcmp(last_sec, kPlayer[i][1]) != 0) {
            last_sec = kPlayer[i][1];
            out_len += (size_t)_snprintf_s(out_text + out_len,
                                           out_cap - out_len, _TRUNCATE,
                                           "[%s]\n", last_sec);
        }
        out_len += (size_t)_snprintf_s(out_text + out_len, out_cap - out_len,
                                       _TRUNCATE, "%s=%s\n", kPlayer[i][0],
                                       kPlayer[i][2]);
    }
    if (u8_fopen_s(&f, path, "w") != 0 || !f) {
        free(out_text);
        out_text = NULL;
        return -1;
    }
    fwrite(out_text, 1, out_len, f);
    fclose(f);
    free(out_text);
    out_text = NULL;
    return 0;
}

int lcfg_apply_set(launcher_settings_t *s, const char *set)
{
    static char buf[4096];
    char *p, *ctx = NULL;
    if (!set || !*set) {
        return 0;
    }
    strcpy_s(buf, sizeof buf, set);
    for (p = buf; *p; p++) {
        if (p[0] == '\\' && p[1] == ';') {
            *p = '\x01';
            memmove(p + 1, p + 2, strlen(p + 2) + 1);
        }
    }
    for (p = strtok_s(buf, ";", &ctx); p; p = strtok_s(NULL, ";", &ctx)) {
        char *eq = strchr(p, '=');
        char *q;
        for (q = p; *q; q++) {
            if (*q == '\x01') {
                *q = ';';
            }
        }
        if (!eq) {
            continue;
        }
        *eq = '\0';
        set_key(s, p, eq + 1);
    }
    return 0;
}

static void join_path(char *out, size_t cap, const char *dir, const char *name)
{
    size_t n = strlen(dir);
    strcpy_s(out, cap, dir);
    if (n > 0 && out[n - 1] != '/' && out[n - 1] != '\\') {
        strcat_s(out, cap, "/");
    }
    strcat_s(out, cap, name);
}

/* W150 ISO-DIRECT: Abbild pruefen (gleicher Leser wie der Client). */
int lcfg_image_check(const char *image_u8, char *msg, size_t cap)
{
    char acp[1024], e[320];
    iso_vfs_t *v;
    uint8_t *xb = NULL;
    uint32_t xn = 0;
    sb_gamever_t gv;
    DWORD a = u8_GetFileAttributes(image_u8);
    if (image_u8 && image_u8[0] && a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) {
        /* W217 FIRST-RUN: Ordner statt Abbild gewaehlt -> klarer Hinweis */
        _snprintf_s(msg, cap, _TRUNCATE, LT("cfg.image_isdir"), image_u8);
        return 1;
    }
    if (!image_u8 || !image_u8[0] || a == INVALID_FILE_ATTRIBUTES) {
        _snprintf_s(msg, cap, _TRUNCATE, LT("cfg.image_missing"), image_u8 ? image_u8 : "");
        return 1;
    }
    u8_to_acp(image_u8, acp, sizeof acp);
    v = iso_vfs_open(acp, e, sizeof e);
    if (!v) {
        _snprintf_s(msg, cap, _TRUNCATE, LT("cfg.image_notxbox"), image_u8);
        return 2;
    }
    if (iso_vfs_read_all(v, "default.xbe", &xb, &xn) != 0) {
        iso_vfs_close(v);
        _snprintf_s(msg, cap, _TRUNCATE, LT("cfg.image_noxbe"), image_u8);
        return 3;
    }
    iso_vfs_close(v);
    sb_gamever_check(xb, xn, &gv);
    free(xb);
    if (!gv.supported) {
        char tid[16];
        _snprintf_s(tid, sizeof tid, _TRUNCATE, "%08X", gv.title_id);
        _snprintf_s(msg, cap, _TRUNCATE, LT("cfg.image_version"), tid,
                    sb_gamever_region_name(gv.region), gv.version);
        return 4;
    }
    _snprintf_s(msg, cap, _TRUNCATE, LT("gui.image_ok"), gv.label);
    return 0;
}

int lcfg_image_xbe_sha256(const char *image_u8, char hex[65])
{
    char acp[1024], e[320];
    iso_vfs_t *v;
    uint8_t *xb = NULL;
    uint32_t xn = 0;
    sb_gamever_t gv;
    u8_to_acp(image_u8, acp, sizeof acp);
    v = iso_vfs_open(acp, e, sizeof e);
    if (!v) {
        return 0;
    }
    if (iso_vfs_read_all(v, "default.xbe", &xb, &xn) != 0) {
        iso_vfs_close(v);
        return 0;
    }
    iso_vfs_close(v);
    sb_gamever_check(xb, xn, &gv);
    free(xb);
    strcpy_s(hex, 65, gv.sha256);
    return 1;
}

/* W150 USER-DATA: zentrale Pfadfunktion (sb_userdata.c) fuer user.cfg. */
void lcfg_user_cfg_path(const char *client_dir, char *out, size_t cap)
{
    char acp[1024];
    if (sb_userdata_path(SB_UD_USERCFG, acp, sizeof acp) == 1) {
        u8_from_acp(acp, out, cap); /* sb_userdata liefert ANSI, Launcher rechnet UTF-8 */
        return;
    }
    _snprintf_s(out, cap, _TRUNCATE, "%s/user.cfg", client_dir);
}

int lcfg_validate(const launcher_settings_t *s, char *err, size_t errcap)
{
    DWORD a;
    char probe[1024];
    char xbe[1024]; /* W172: Programmdatei (im Ordner-Modus abgeleitet) */

#define FAIL(fmt, ...)                                     \
    do {                                                   \
        _snprintf_s(err, errcap, _TRUNCATE, fmt, __VA_ARGS__); \
        return 1;                                          \
    } while (0)

    if (s->game_image[0]) { /* W150 ISO-DIRECT: Abbild statt Ordner */
        char acp[1024];
        if (u8_to_acp(s->game_image, acp, sizeof acp) == 2) {
            FAIL(LT("cfg.ansi"), LT("cfg.what_image"), s->game_image);
        }
        if (lcfg_image_check(s->game_image, err, errcap) != 0) {
            return 1;
        }
        goto paths_ok;
    }
    /* W172 LAUNCHER-P6: Programmdatei ist im Ordner-Modus optional (aus
     * <Spielordner>/default.xbe abgeleitet). Nichts gesetzt = keine
     * Spieldateien (Abbild empfohlen). */
    if (!s->xbe_path[0] && !s->media_root[0]) {
        FAIL("%s", LT("cfg.no_media"));
    }
    lcfg_xbe_effective(s, xbe, sizeof xbe);
    a = u8_GetFileAttributes(xbe);
    if (a == INVALID_FILE_ATTRIBUTES) {
        if (!s->xbe_path[0]) {
            FAIL(LT("cfg.no_xbe"), s->media_root);
        }
        FAIL(LT("cfg.xbe_missing"),
             xbe);
    }
    if (a & FILE_ATTRIBUTE_DIRECTORY) {
        FAIL(LT("cfg.xbe_isdir"),
             xbe);
    }
    if (!s->media_root[0]) {
        FAIL("%s", LT("cfg.no_media"));
    }
    a = u8_GetFileAttributes(s->media_root);
    if (a == INVALID_FILE_ATTRIBUTES) {
        FAIL(LT("cfg.media_missing"),
             s->media_root);
    }
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
        FAIL(LT("cfg.media_nodir"),
             s->media_root);
    }
    join_path(probe, sizeof probe, s->media_root, "default.xbe");
    if (u8_GetFileAttributes(probe) == INVALID_FILE_ATTRIBUTES) {
        join_path(probe, sizeof probe, s->media_root, "Media");
        if (u8_GetFileAttributes(probe) == INVALID_FILE_ATTRIBUTES) {
            FAIL(LT("cfg.media_empty"),
                 s->media_root);
        }
    }
    { /* W150: Spielversion auch im Ordner-Modus (Patch-Adressen nur PAL) */
        FILE *xf = NULL;
        if (u8_fopen_s(&xf, xbe, "rb") == 0 && xf) {
            long xn;
            uint8_t *xb;
            fseek(xf, 0, SEEK_END);
            xn = ftell(xf);
            fseek(xf, 0, SEEK_SET);
            xb = (xn > 0 && xn < 64L * 1024 * 1024) ? (uint8_t *)malloc((size_t)xn) : NULL;
            if (xb && fread(xb, 1, (size_t)xn, xf) == (size_t)xn) {
                sb_gamever_t gv;
                if (!sb_gamever_check(xb, (size_t)xn, &gv)) {
                    char tid[16];
                    fclose(xf);
                    free(xb);
                    _snprintf_s(tid, sizeof tid, _TRUNCATE, "%08X", gv.title_id);
                    FAIL(LT("cfg.image_version"), tid,
                         sb_gamever_region_name(gv.region), gv.version);
                }
            }
            free(xb);
            fclose(xf);
        }
    }
paths_ok:
    {
        /* W82: Pfade gehen ANSI in die cfg (Client liest mit A-API). Nicht
         * darstellbare Zeichen ohne 8.3-Kurznamen kann der Client nicht
         * oeffnen -> verstaendlich ablehnen statt still kaputt starten. */
        static const char *const kWhat[3] = { "cfg.what_xbe", "cfg.what_media",
                                              "cfg.what_hdd"};
        const char *pv[3];
        char acp[1024];
        int pi2;
        pv[0] = s->game_image[0] ? s->xbe_path : xbe;
        pv[1] = s->media_root;
        pv[2] = s->hdd_root;
        for (pi2 = 0; pi2 < 3; pi2++) {
            if (s->game_image[0] && pi2 < 2) {
                continue; /* W150: Abbild-Modus nutzt xbe_path/media_root nicht */
            }
            if (u8_to_acp(pv[pi2], acp, sizeof acp) == 2) {
                FAIL(LT("cfg.ansi"),
                     LT(kWhat[pi2]), pv[pi2]);
            }
        }
    }
    if (s->width < 320 || s->width > 7680) {
        FAIL(LT("cfg.bad_width"), s->width);
    }
    if (s->height < 200 || s->height > 4320) {
        FAIL(LT("cfg.bad_height"), s->height);
    }
    if (s->fps_limit < 0 || s->fps_limit > 1000) {
        FAIL(LT("cfg.bad_fps"), s->fps_limit);
    }
    if (s->present_interval < 0 || s->present_interval > 1) {
        FAIL(LT("cfg.bad_vsync"), s->present_interval);
    }
    /* W47, Regel S3.10 (nie strecken): aspect_mode=0 streckt 2D aufs
     * Vollbild und wird darum abgelehnt; der Client kennt nur 0/1. */
    if (s->sbc_mouse_off < 0 || s->sbc_mouse_off > 1) {
        FAIL(LT("cfg.bad_mouseoff"), s->sbc_mouse_off);
    }
    if (s->sbc_usb < 0 || s->sbc_usb > 1) {
        FAIL(LT("cfg.bad_sbc"), s->sbc_usb);
    }
    if (s->interp_fps != 0 && s->interp_fps != 60 && s->interp_fps != 120) {
        FAIL(LT("cfg.bad_ipfps"), s->interp_fps);
    }
    if (s->monitor_grain < 0 || s->monitor_grain > 100 ||
        s->monitor_glitch < 0 || s->monitor_glitch > 100) {
        FAIL(LT("cfg.bad_monfx"), s->monitor_grain, s->monitor_glitch);
    }
    if (s->window_mode < 0 || s->window_mode > 2) {
        FAIL(LT("cfg.bad_winmode"),
             s->window_mode);
    }
    if (s->vr_enable < 0 || s->vr_enable > 1) { /* W134 */
        FAIL(LT("cfg.bad_vr"), s->vr_enable);
    }
    if (_stricmp(s->vr_preview, "sbs") != 0 && _stricmp(s->vr_preview, "left") != 0 &&
        _stricmp(s->vr_preview, "right") != 0 && _stricmp(s->vr_preview, "off") != 0) {
        FAIL(LT("cfg.bad_vrprev"), s->vr_preview);
    }
    if (s->vr_render_scale < 30 || s->vr_render_scale > 150) { /* W183 */
        FAIL(LT("cfg.bad_vrscale"), s->vr_render_scale);
    }
    if (s->edge_mode < 0 || s->edge_mode > 2) { /* W107 */
        FAIL(LT("cfg.bad_edge"),
             s->edge_mode);
    }
    if (s->edge_fade_width < 0 || s->edge_fade_width > 50) {
        FAIL(LT("cfg.bad_edgew"),
             s->edge_fade_width);
    }
    if (s->edge_fade_strength < 0 || s->edge_fade_strength > 100) {
        FAIL(LT("cfg.bad_edges"), s->edge_fade_strength);
    }
    if (s->cockpit_persp < 0 || s->cockpit_persp > 100) { /* W191 */
        FAIL(LT("cfg.bad_cpersp"), s->cockpit_persp);
    }
    if (s->cockpit_panini < 0 || s->cockpit_panini > 100) { /* W191 */
        FAIL(LT("cfg.bad_cpanini"), s->cockpit_panini);
    }
    if (s->fs_hz != 0 && (s->fs_hz < 24 || s->fs_hz > 500)) {
        FAIL(LT("cfg.bad_hz"), s->fs_hz);
    }
    {
        size_t pl = strlen(s->input_profile);
        size_t pi;
        if (pl >= sizeof s->input_profile) {
            FAIL("%s", LT("cfg.prof_toolong"));
        }
        for (pi = 0; pi < pl; pi++) {
            char c = s->input_profile[pi];
            int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                     (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' ||
                     c == '/' || c == '\\' || c == ':'; /* Pfad: Client erlaubt Pfade */

            if (!ok) {
                FAIL(LT("cfg.prof_bad"),
                     s->input_profile);
            }
        }
    }
    if (s->aspect_mode != 1) {
        FAIL(LT("cfg.bad_aspect"),
             s->aspect_mode);
    }
    if (_stricmp(s->lang, "en") != 0 && _stricmp(s->lang, "de") != 0) { /* W142 */
        FAIL(LT("cfg.lang_bad"), s->lang);
    }
    if (err && errcap) {
        err[0] = '\0';
    }
    return 0;
#undef FAIL
}

static int dir_has_ldr(const char *dir)
{
    char p[1024];
    DWORD a;
    if (!dir || !*dir) {
        return 0;
    }
    join_path(p, sizeof p, dir, "sb_ldr.exe");
    a = u8_GetFileAttributes(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

int lcfg_find_client_dir(const char *override_dir, char *out, size_t cap,
                         int *has_ldr)
{
    char exe[1024];
    char env[1024];
    const char *cands[4];
    int i, n = 0;

    if (override_dir && *override_dir) {
        cands[n++] = override_dir;
    }
    if (u8_GetEnv("SB_HARNESS_DIR", env, sizeof env) > 0) {
        cands[n++] = env;
    }
    if (u8_ExeDir(exe, sizeof exe)) {
        cands[n++] = exe;
    }
    if (SB_HARNESS_DIR_DEFAULT[0]) {
        cands[n++] = SB_HARNESS_DIR_DEFAULT;
    }
    for (i = 0; i < n; i++) {
        if (dir_has_ldr(cands[i])) {
            strcpy_s(out, cap, cands[i]);
            if (has_ldr) {
                *has_ldr = 1;
            }
            return 0;
        }
    }
    /* Fallback: erster Kandidat (oder CWD), auch ohne Lader. */
    strcpy_s(out, cap, n > 0 ? cands[0] : ".");
    if (has_ldr) {
        *has_ldr = 0;
    }
    return 1;
}

/* --- Profile (W47) -------------------------------------------------- */

int lcfg_profile_name_ok(const char *name)
{
    size_t i, n;
    if (!name || !*name) {
        return 0;
    }
    n = strlen(name);
    if (n > 63 || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        char c = name[i];
        int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                 (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!ok) {
            return 0;
        }
    }
    return 1;
}

int lcfg_profile_path(const char *client_dir, const char *name, char *out,
                      size_t cap)
{
    char dir[1024];
    if (!lcfg_profile_name_ok(name)) {
        return 1;
    }
    join_path(dir, sizeof dir, client_dir, "profiles");
    join_path(out, cap, dir, name);
    strcat_s(out, cap, ".cfg");
    return 0;
}

int lcfg_profile_list(const char *client_dir, char names[][64], int max)
{
    char dir[1024], pat[1100];
    u8_find_t fd;
    int n = 0, i, j;

    join_path(dir, sizeof dir, client_dir, "profiles");
    join_path(pat, sizeof pat, dir, "*.cfg");
    if (!u8_find_first(pat, &fd)) {
        return 0; /* kein Ordner / keine Profile: kein Fehler */
    }
    do {
        size_t len = strlen(fd.name);
        if (fd.attr & FILE_ATTRIBUTE_DIRECTORY) {
            continue;
        }
        if (len <= 4 || len - 4 >= 64 || n >= max) {
            continue; /* zu lang: kein gueltiger Profilname */
        }
        /* "standard.txt" faellt durch "*.cfg" ohnehin raus. */
        memcpy(names[n], fd.name, len - 4);
        names[n][len - 4] = '\0';
        if (lcfg_profile_name_ok(names[n])) {
            n++;
        }
    } while (u8_find_next(&fd));
    u8_find_close(&fd);
    /* Einfaches Bubble-Sort (wenige Profile). */
    for (i = 0; i < n; i++) {
        for (j = i + 1; j < n; j++) {
            if (strcmp(names[i], names[j]) > 0) {
                char t[64];
                strcpy_s(t, sizeof t, names[i]);
                strcpy_s(names[i], 64, names[j]);
                strcpy_s(names[j], 64, t);
            }
        }
    }
    return n;
}

int lcfg_profile_load(const char *client_dir, const char *name,
                      launcher_settings_t *s, char *err, size_t errcap)
{
    char path[1200];
    if (!lcfg_profile_name_ok(name)) {
        _snprintf_s(err, errcap, _TRUNCATE, LT("cfg.prof_badname"),
                    name ? name : LT("cfg.empty"));
        return 1;
    }
    lcfg_profile_path(client_dir, name, path, sizeof path);
    lcfg_defaults(s);
    if (lcfg_load_file(path, s) != 0) {
        _snprintf_s(err, errcap, _TRUNCATE, LT("cfg.prof_notfound"), name, path);
        return 1;
    }
    return 0;
}

/* Verzeichnis anlegen, Eltern bei Bedarf mit (rekursiv). 0 = ok. */
/* Oeffentlicher mkdir-Wrapper steht hinter ensure_dir (s. unten). */
static int ensure_dir(const char *dir)
{
    char parent[1024], *sep;
    DWORD a = u8_GetFileAttributes(dir);
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) {
        return 0;
    }
    if (u8_CreateDirectory(dir)) {
        return 0;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        return 0;
    }
    /* Eltern zuerst (einmal), dann erneut versuchen. */
    strcpy_s(parent, sizeof parent, dir);
    sep = strrchr(parent, '/');
    {
        char *bs = strrchr(parent, '\\');
        if (bs && (!sep || bs > sep)) {
            sep = bs;
        }
    }
    if (!sep || sep == parent || (sep == parent + 2 && parent[1] == ':')) {
        return -1; /* Wurzel erreicht: aufgeben */
    }
    *sep = '\0';
    if (ensure_dir(parent) != 0) {
        return -1;
    }
    return u8_CreateDirectory(dir) ? 0 : -1;
}

int lcfg_mkdir_p(const char *dir)
{
    return ensure_dir(dir);
}

int lcfg_profile_save(const char *client_dir, const char *name,
                      const launcher_settings_t *s, char *err, size_t errcap)
{
    char dir[1024], path[1200];
    if (!lcfg_profile_name_ok(name)) {
        _snprintf_s(err, errcap, _TRUNCATE, LT("cfg.prof_badname"),
                    name ? name : LT("cfg.empty"));
        return 1;
    }
    join_path(dir, sizeof dir, client_dir, "profiles");
    ensure_dir(dir); /* existiert meist schon: egal */
    lcfg_profile_path(client_dir, name, path, sizeof path);
    if (lcfg_save_user(path, s) != 0) {
        _snprintf_s(err, errcap, _TRUNCATE, LT("cfg.prof_nowrite"), name, path);
        return 1;
    }
    if (err && errcap) {
        err[0] = '\0';
    }
    return 0;
}

int lcfg_profile_delete(const char *client_dir, const char *name, char *err,
                        size_t errcap)
{
    char path[1200], dflt[64];
    if (!lcfg_profile_name_ok(name)) {
        _snprintf_s(err, errcap, _TRUNCATE, LT("cfg.prof_badname2"), name ? name : LT("cfg.empty"));
        return 1;
    }
    lcfg_profile_path(client_dir, name, path, sizeof path);
    if (!u8_DeleteFile(path)) {
        _snprintf_s(err, errcap, _TRUNCATE, LT("cfg.prof_nodel"),
                    name, GetLastError());
        return 1;
    }
    /* War es das Standard-Profil, Markierung mit loeschen. */
    if (lcfg_profile_get_default(client_dir, dflt, sizeof dflt) == 0 &&
        strcmp(dflt, name) == 0) {
        lcfg_profile_set_default(client_dir, NULL, NULL, 0);
    }
    if (err && errcap) {
        err[0] = '\0';
    }
    return 0;
}

static void default_marker_path(const char *client_dir, char *out, size_t cap)
{
    char dir[1024];
    join_path(dir, sizeof dir, client_dir, "profiles");
    join_path(out, cap, dir, "standard.txt");
}

int lcfg_profile_get_default(const char *client_dir, char *name, size_t cap)
{
    char path[1200];
    FILE *f;
    char line[128];
    default_marker_path(client_dir, path, sizeof path);
    if (u8_fopen_s(&f, path, "r") != 0 || !f) {
        return 1;
    }
    if (!fgets(line, sizeof line, f)) {
        fclose(f);
        return 1;
    }
    fclose(f);
    line[strcspn(line, "\r\n")] = '\0';
    if (!lcfg_profile_name_ok(line)) {
        return 1;
    }
    strcpy_s(name, cap, line);
    return 0;
}

int lcfg_profile_set_default(const char *client_dir, const char *name,
                             char *err, size_t errcap)
{
    char dir[1024], path[1200], check[1200];
    DWORD a;
    if (!name || !*name) {
        default_marker_path(client_dir, path, sizeof path);
        u8_DeleteFile(path); /* kein Standard: kein Fehler */
        if (err && errcap) {
            err[0] = '\0';
        }
        return 0;
    }
    if (!lcfg_profile_name_ok(name)) {
        _snprintf_s(err, errcap, _TRUNCATE, LT("cfg.prof_badname2"),
                    name);
        return 1;
    }
    lcfg_profile_path(client_dir, name, check, sizeof check);
    a = u8_GetFileAttributes(check);
    if (a == INVALID_FILE_ATTRIBUTES) {
        _snprintf_s(err, errcap, _TRUNCATE, LT("cfg.prof_noexist"),
                    name);
        return 1;
    }
    join_path(dir, sizeof dir, client_dir, "profiles");
    ensure_dir(dir);
    default_marker_path(client_dir, path, sizeof path);
    {
        FILE *f;
        if (u8_fopen_s(&f, path, "w") != 0 || !f) {
            _snprintf_s(err, errcap, _TRUNCATE, LT("cfg.prof_nodefault"), path);
            return 1;
        }
        fprintf(f, "%s\n", name);
        fclose(f);
    }
    if (err && errcap) {
        err[0] = '\0';
    }
    return 0;
}
