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
#include "sb_pack.h"     /* W256 PACK-ANYVER: SB_PACK_FIT_* */
#include "sb_userdata.h" /* W150 USER-DATA */
#include "sb_gfxpreset.h" /* W173 LAUNCHER-P4: Preset-Werte (wie Client) */
#include "sb_fpsrate.h" /* W223 FPS-RATES: gueltige Bildraten */

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
    s->volume_master = s->volume_music = s->volume_sfx = s->volume_voice = 100; /* W303 AUDIO-VOLUME */
    s->audio_hrtf = 0; /* W356: Lautsprecher (Kopfhoerer-HRTF nur auf Wunsch) */
    s->aspect_mode = 1; /* W47: Pillarbox + Hor+, nie strecken (S3.10) */
    s->input_profile[0] = '\0'; /* W78: leer = Legacy-Tastatur */
    s->sbc_usb = 0;             /* W78: USB-Controller aus */
    s->sbc_mouse_off = 1;       /* W128: Maus-Achsen aus bei aktivem Controller */
    s->aim_assist = 0;          /* W236 AIM-ASSIST: Standard Original */
    s->auto_gear = 0;
    s->ctrl_despike = 1; /* W439: Ausreisser-Filter an (Nutzerentscheid 22.11.), 0 = Original */
    s->sb_allied_squads = 1; /* W424 SB-ALLIED-SQUADS: Standard an (Nutzerentscheid 22.11.), 0 = Original */
    s->cockpit_hide = 0; /* W268 */
    s->monitor_view = 0; /* W319 */
    s->stick_aim = 0;           /* W271 STICK-RELATIVE: Standard Position (Original) */
    s->stick_aim_sens = 4;
    s->game_tick = 30;          /* W234 PAL50: Standard 30 Ticks/s */
    s->save_lock = 0;           /* W190 SAVE-LOCK: Original-Verhalten */
    s->photo_dir[0] = '\0';     /* W195 PHOTO-MODE: Standardordner */
    s->photo_scale = 1;
    s->photo_jpg = 0;
    s->photo_aspect = 2; /* W290: 4:3 wie Monitor (W195: 1 = weit) */
    s->photo_view = 0;   /* W290 QUICK-SHOT: wie im Spiel */
    s->photo_size = -4;  /* W290: 4K (alte cfg mit photo_scale > 1 ohne photo_size: dieser Faktor) */
    s->photo_hud = 1;    /* W290: HUD im Foto */
    s->photo_cine_self = LCFG_CSELF_DEF; /* W445 */
    s->replay_label = 1; /* W290: Original */
    s->replay_logo = 1;
    s->watermark_media = 1;
    s->interp_fps = SB_FPS_AUTO; /* W296 PRESET-ORIG: Standard = Original = Monitor (auto); vorher 120 (W78) */
    s->gfx_preset_rev = SB_GP_REV; /* W296: neuer Stand; Laden der Nutzer-cfg setzt vorher 0 (app_load_effective, Profile) */
    s->window_mode = 0;         /* W81: Fenster (Client-Standard) */
    s->fs_hz = 0;               /* W81: Desktop-Rate */
    s->mouse_mode = 1;          /* W87: Spieler-Standard absolut */
    s->mouse_sens = 4;          /* W87: wie Client-Standard (hcfg) */
    s->edge_mode = 1;           /* W278 ORIG-FLAG (Nutzer 15.11.): 4:3 mit schwarzen Balken = Original (vorher 2 = Abdunklung, 5.11.); Test-cfg bleibt 0 */
    s->edge_fade_width = 8;    /* Nutzer 14.11.: 8 % (vorher 10 %, 5.11.); gilt, sobald jemand Breitbild (Abdunklung) waehlt */
    s->edge_fade_strength = 100;
    s->cockpit_persp = 0;       /* W191 UW-PERSP: Original */
    s->cockpit_panini = 0;      /* W191: Panini aus */
    s->cockpit_width = 0;       /* W235: Original (nicht gestreckt) */
    s->remaster = 0;            /* W103: Original bitgenau */
    s->hd_textures = 0;
    s->hd_scale = 4;
    s->fx_enhanced = 0;
    s->ultrawide = 0;
    s->postfx = 0;
    memset(s->fx, 0, sizeof s->fx); /* W173: Effekte = Original (wie hcfg) */
    s->monitor_grain = 100;     /* W105: Original */
    s->monitor_glitch = 100;    /* W105: Original */
    s->monitor_fx = 1;          /* W252: Original (wie hcfg) */
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
    if (k == LGK_MFX) { return "monitor_fx"; } /* W252 */
    if (k == LGK_EDGE) { return "edge_mode"; } /* W266 */
    if (k == LGK_EDGEW) { return "edge_fade_width"; }
    if (k == LGK_EDGES) { return "edge_fade_strength"; }
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
    if (k == LGK_MFX) { return s->monitor_fx; } /* W252 */
    if (k == LGK_EDGE) { return s->edge_mode; } /* W266 */
    if (k == LGK_EDGEW) { return s->edge_fade_width; }
    if (k == LGK_EDGES) { return s->edge_fade_strength; }
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
        int v;
        if (gfx_row(k) < 0) { /* W296: nicht im Preset (-1 ist jetzt ein Wert: interp_fps auto) */
            continue;
        }
        v = lcfg_gfx_preset_value(preset, k);
        if (k < LSHX_N) {
            s->shx[k] = v;
        } else if (k < LGK_FX0 + LFX_N) {
            s->fx[k - LGK_FX0] = v;
        } else if (k == LGK_FPS) { s->interp_fps = v;
        } else if (k == LGK_RT) { s->rt_scale = v;
        } else if (k == LGK_DYN) { s->rt_scale_dyn = v;
        } else if (k == LGK_MATCH) { s->fs_interp_match = v;
        } else if (k == LGK_MFX) { s->monitor_fx = v; /* W252 */
        } else if (k == LGK_EDGE) { s->edge_mode = v; /* W266 */
        } else if (k == LGK_EDGEW) { s->edge_fade_width = v;
        } else if (k == LGK_EDGES) { s->edge_fade_strength = v;
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

int lcfg_gfx_migrate_fps(launcher_settings_t *s)
{
    int done = 0;
    if (s->gfx_preset_rev < SB_GP_REV && s->interp_fps == SB_GP_LEGACY_ORIG_FPS && s->fs_interp_match == 1) {
        /* Erkennung ohne Effect-Pack-Sperre: gespeicherte Werte muessen selbst
         * Original sein (ein Pack aendert sie nie, er gilt nur im Spiel). */
        launcher_settings_t t = *s;
        t.interp_fps = SB_FPS_AUTO;
        if (lcfg_gfx_diff(&t, LGP_ORIGINAL, 0, NULL) == 0) {
            s->interp_fps = SB_FPS_AUTO;
            done = 1;
        }
    }
    s->gfx_preset_rev = SB_GP_REV;
    return done;
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

static int is_path_key(const char *k);

static void set_key(launcher_settings_t *s, const char *k, const char *v)
{
    /* W332 SEC: zu lange Pfadwerte (user.cfg/Profil bis 4 KB je Zeile, Felder 512) verwerfen statt strcpy_s-Abbruch
     * (Absturz beim Start); ein gekuerzter Pfad waere ein anderer Pfad. */
    if (is_path_key(k) && strcmp(k, "photo_dir") != 0 && strlen(v) >= sizeof s->xbe_path) {
        return;
    }
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
    } else if (strcmp(k, "audio_hrtf") == 0) { /* W356 */
        s->audio_hrtf = atoi(v) ? 1 : 0;
    } else if (strcmp(k, "volume_master") == 0 || strcmp(k, "volume_music") == 0 || /* W303 AUDIO-VOLUME */
               strcmp(k, "volume_sfx") == 0 || strcmp(k, "volume_voice") == 0) {
        int vol = atoi(v); /* 0..100 klemmen (wie hcfg) */
        vol = vol < 0 ? 0 : (vol > 100 ? 100 : vol);
        if (strcmp(k, "volume_master") == 0) { s->volume_master = vol; }
        else if (strcmp(k, "volume_music") == 0) { s->volume_music = vol; }
        else if (strcmp(k, "volume_sfx") == 0) { s->volume_sfx = vol; }
        else { s->volume_voice = vol; }
    } else if (strcmp(k, "aspect_mode") == 0) {
        s->aspect_mode = atoi(v);
    } else if (strcmp(k, "input_profile") == 0) {
        strncpy_s(s->input_profile, sizeof s->input_profile, v,
                    sizeof s->input_profile - 1); /* kuerzt statt Fehler */
    } else if (strcmp(k, "sbc_usb") == 0) {
        s->sbc_usb = atoi(v);
    } else if (strcmp(k, "sbc_mouse_off") == 0) {
        s->sbc_mouse_off = atoi(v);
    } else if (strcmp(k, "aim_assist") == 0) { /* W236 */
        s->aim_assist = atoi(v);
        if (s->aim_assist < 0 || s->aim_assist > 2) s->aim_assist = 0;
    } else if (strcmp(k, "aim_turn") == 0) {
        /* W218-Altwert: "Fadenkreuz dreht Mech" entfallen -> Stufe 1
         * (Fadenkreuz am Rand dreht die Sicht); 0 laesst aim_assist stehen. */
        if (atoi(v)) s->aim_assist = 1;
    } else if (strcmp(k, "auto_gear") == 0) { /* W218 */
        s->auto_gear = atoi(v) ? 1 : 0;
    } else if (strcmp(k, "sb_allied_squads") == 0) { /* W424 */
        s->sb_allied_squads = atoi(v) ? 1 : 0;
    } else if (strcmp(k, "ctrl_despike") == 0) { /* W439 */
        s->ctrl_despike = atoi(v) ? 1 : 0;
    } else if (strcmp(k, "cockpit_hide") == 0) { /* W268 */
        s->cockpit_hide = atoi(v) ? 1 : 0;
    } else if (strcmp(k, "monitor_view") == 0) { /* W319 */
        s->monitor_view = atoi(v);
        if (s->monitor_view < 0 || s->monitor_view > 2) s->monitor_view = 0;
    } else if (strcmp(k, "fx_wireframe") == 0) { /* W278 */
        s->fx_wireframe = atoi(v) ? 1 : 0;
    } else if (strcmp(k, "fx_vaporwave") == 0) { /* W278 */
        s->fx_vaporwave = atoi(v) ? 1 : 0;
    } else if (strcmp(k, "stick_aim") == 0) { /* W271: position|relative (wie hcfg) */
        s->stick_aim = (_stricmp(v, "relative") == 0 || _stricmp(v, "rate") == 0 ||
                        _stricmp(v, "relativ") == 0 || atoi(v) == 1) ? 1 : 0;
    } else if (strcmp(k, "stick_aim_sens") == 0) { /* W271 */
        s->stick_aim_sens = atoi(v);
        if (s->stick_aim_sens < 1) s->stick_aim_sens = 1;
        if (s->stick_aim_sens > 16) s->stick_aim_sens = 16;
    } else if (strcmp(k, "game_tick") == 0) { /* W234 PAL50: nur 25 oder 30 */
        s->game_tick = atoi(v) == 25 ? 25 : 30;
    } else if (strcmp(k, "save_lock") == 0) { /* W190 */
        s->save_lock = atoi(v) ? 1 : 0;
    } else if (strcmp(k, "photo_dir") == 0) { /* W195 */
        strncpy_s(s->photo_dir, sizeof s->photo_dir, v, _TRUNCATE);
    } else if (strcmp(k, "photo_scale") == 0) {
        int n = atoi(v);
        s->photo_scale = (n == 2 || n == 4 || n == 8) ? n : 1;
        if (s->photo_scale > 1 && s->photo_size == -4) { s->photo_size = s->photo_scale; } /* W290: alte cfg */
    } else if (strcmp(k, "photo_format") == 0) {
        s->photo_jpg = (_stricmp(v, "jpg") == 0 || _stricmp(v, "jpeg") == 0) ? 1 : 0;
    } else if (strcmp(k, "photo_aspect") == 0) {
        int n = atoi(v);
        s->photo_aspect = (n >= 0 && n <= 2) ? n : 2;
    } else if (strcmp(k, "photo_view") == 0) { /* W290 QUICK-SHOT */
        int n = atoi(v);
        s->photo_view = (n >= 0 && n <= 3) ? n : 0;
    } else if (strcmp(k, "photo_size") == 0) { /* W290 */
        int n = (_stricmp(v, "4k") == 0) ? -4 : (_stricmp(v, "8k") == 0) ? -8 : atoi(v);
        s->photo_size = (n == 1 || n == 2 || n == 4 || n == 8 || n == -4 || n == -8) ? n : -4;
    } else if (strcmp(k, "photo_hud") == 0) { /* W290 */
        s->photo_hud = atoi(v) ? 1 : 0;
    } else if (strcmp(k, "photo_cine_self") == 0) { /* W445: aufs Raster (0 = aus) */
        s->photo_cine_self = lcfg_cself_clamp(atoi(v));
    } else if (strcmp(k, "replay_label") == 0) { /* W290 */
        s->replay_label = atoi(v) ? 1 : 0;
    } else if (strcmp(k, "replay_logo") == 0) { /* W290 */
        s->replay_logo = atoi(v) ? 1 : 0;
    } else if (strcmp(k, "watermark_media") == 0) { /* W290 */
        s->watermark_media = atoi(v) ? 1 : 0;
    } else if (strcmp(k, "interp_fps") == 0) {
        s->interp_fps = atoi(v);
    } else if (strcmp(k, SB_GP_REV_KEY) == 0) { /* W296 */
        s->gfx_preset_rev = atoi(v);
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
    } else if (strcmp(k, "cockpit_width") == 0) { /* W235 */
        s->cockpit_width = atoi(v);
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
    } else if (strcmp(k, "monitor_fx") == 0) { /* W252 */
        s->monitor_fx = atoi(v);
        if (s->monitor_fx < 0 || s->monitor_fx > 2) { s->monitor_fx = 1; }
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

int lcfg_photo_size_eff(const launcher_settings_t *s)
{
    int n = s->photo_size;
    return (n == 1 || n == 2 || n == 4 || n == 8 || n == -4 || n == -8) ? n : -4;
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
    int n = 0, saw_view = 0, saw_asp = 0; /* W290: Altwert photo_aspect=1 */

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
        if (strcmp(k, "photo_view") == 0) { saw_view = 1; } /* W290 */
        if (strcmp(k, "photo_aspect") == 0) { saw_asp = 1; }
        n++;
    }
    /* W290: Bis W290 schrieb der Launcher immer photo_aspect=1 (damaliger Standard "weit"). Ohne photo_view stammt
     * der Wert aus dieser Zeit und war keine Wahl: neuer Standard 4:3 (2). */
    if (saw_asp && !saw_view && s->photo_aspect == 1) { s->photo_aspect = 2; }
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
    { "volume_master", "audio" },   { "volume_music", "audio" }, /* W303 AUDIO-VOLUME */
    { "volume_sfx", "audio" },      { "volume_voice", "audio" },
    { "audio_hrtf", "audio" }, /* W356 */
    { "input_profile", "input" },  { "sbc_usb", "input" },
    { "sbc_mouse_off", "input" }, /* W128 */
    { "aim_assist", "input" },     { "auto_gear", "input" }, /* W218, W236 */
    { "aim_turn", "input" }, /* W236: Altwert neutralisieren (immer 0) */
    { "sb_allied_squads", "gameplay" }, /* W424 */
    { "ctrl_despike", "input" }, /* W439 */
    { "stick_aim", "input" },      { "stick_aim_sens", "input" }, /* W271 */
    { "save_lock", "game" }, /* W190 */
    { "game_tick", "game" }, /* W234 PAL50 */
    { "photo_dir", "photo" },       { "photo_scale", "photo" }, /* W195 PHOTO-MODE */
    { "photo_format", "photo" },    { "photo_aspect", "photo" },
    { "photo_view", "photo" },      { "photo_size", "photo" }, /* W290 QUICK-SHOT */
    { "photo_hud", "photo" }, /* W290 */
    { "photo_cine_self", "photo" }, /* W445 */
    { "replay_label", "render" },   { "replay_logo", "render" }, /* W290 */
    { "watermark_media", "overlay" }, /* W290 */
    { "interp_fps", "render" },
    { SB_GP_REV_KEY, "render" }, /* W296 PRESET-ORIG: Stand der Preset-Tabelle (immer SB_GP_REV) */
    { "window_mode", "display" },   { "fs_hz", "display" },
    { "mouse_mode", "input" },      { "mouse_sens", "input" },
    { "edge_mode", "render" },      { "edge_fade_width", "render" }, /* W107 */
    { "edge_fade_strength", "render" },
    { "cockpit_persp", "render" }, /* W191 */
    { "cockpit_panini", "render" }, /* W191 */
    { "cockpit_width", "render" }, /* W235 */
    { "cockpit_hide", "render" }, /* W268 */
    { "monitor_view", "render" }, /* W319 */
    { "fx_wireframe", "render" },   { "fx_vaporwave", "render" }, /* W278: Hinweis/Zuruecksetzen */
    { "remaster", "remaster" },     { "hd_textures", "remaster" }, /* W103 */
    { "hd_scale", "remaster" },     { "fx_enhanced", "remaster" },
    { "ultrawide", "remaster" },    { "postfx", "remaster" },
    { "monitor_grain", "render" },  { "monitor_glitch", "render" },
    { "monitor_fx", "render" }, /* W252: gehoert zum Grafik-Preset */
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
    } else if (strcmp(k, "photo_size") == 0) { /* W290: 4k/8k/1/2/4/8 (Client: hcfg) */
        int n = lcfg_photo_size_eff(s);
        if (n < 0) { _snprintf_s(out, cap, _TRUNCATE, "%dk", -n); }
        else { _snprintf_s(out, cap, _TRUNCATE, "%d", n); }
    } else if (strcmp(k, "input_profile") == 0) {
        strcpy_s(out, cap, s->input_profile);
    } else if (strcmp(k, "mouse_mode") == 0) {
        strcpy_s(out, cap, s->mouse_mode ? "absolute" : "relative");
    } else if (strcmp(k, "stick_aim") == 0) { /* W271 */
        strcpy_s(out, cap, s->stick_aim ? "relative" : "position");
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
        } else if (strcmp(k, "audio_hrtf") == 0) { /* W356 */
            v = s->audio_hrtf ? 1 : 0;
        } else if (strcmp(k, "volume_master") == 0) { /* W303 AUDIO-VOLUME */
            v = s->volume_master < 0 ? 0 : (s->volume_master > 100 ? 100 : s->volume_master);
        } else if (strcmp(k, "volume_music") == 0) {
            v = s->volume_music < 0 ? 0 : (s->volume_music > 100 ? 100 : s->volume_music);
        } else if (strcmp(k, "volume_sfx") == 0) {
            v = s->volume_sfx < 0 ? 0 : (s->volume_sfx > 100 ? 100 : s->volume_sfx);
        } else if (strcmp(k, "volume_voice") == 0) {
            v = s->volume_voice < 0 ? 0 : (s->volume_voice > 100 ? 100 : s->volume_voice);
        } else if (strcmp(k, "aspect_mode") == 0) {
            v = s->aspect_mode;
        } else if (strcmp(k, "sbc_usb") == 0) {
            v = s->sbc_usb;
        } else if (strcmp(k, "sbc_mouse_off") == 0) {
            v = s->sbc_mouse_off;
        } else if (strcmp(k, "aim_assist") == 0) { /* W236 */
            v = (s->aim_assist >= 0 && s->aim_assist <= 2) ? s->aim_assist : 0;
        } else if (strcmp(k, "aim_turn") == 0) { /* W236: Altwert, immer 0 */
            v = 0;
        } else if (strcmp(k, "auto_gear") == 0) { /* W218 */
            v = s->auto_gear ? 1 : 0;
        } else if (strcmp(k, "sb_allied_squads") == 0) { /* W424 */
            v = s->sb_allied_squads ? 1 : 0;
        } else if (strcmp(k, "ctrl_despike") == 0) { /* W439 */
            v = s->ctrl_despike ? 1 : 0;
        } else if (strcmp(k, "cockpit_hide") == 0) { /* W268 */
            v = s->cockpit_hide ? 1 : 0;
        } else if (strcmp(k, "monitor_view") == 0) { /* W319 */
            v = (s->monitor_view >= 0 && s->monitor_view <= 2) ? s->monitor_view : 0;
        } else if (strcmp(k, "fx_wireframe") == 0) { /* W278 */
            v = s->fx_wireframe ? 1 : 0;
        } else if (strcmp(k, "fx_vaporwave") == 0) {
            v = s->fx_vaporwave ? 1 : 0;
        } else if (strcmp(k, "stick_aim_sens") == 0) { /* W271 */
            v = (s->stick_aim_sens >= 1 && s->stick_aim_sens <= 16) ? s->stick_aim_sens : 4;
        } else if (strcmp(k, "game_tick") == 0) { /* W234 PAL50 */
            v = s->game_tick == 25 ? 25 : 30;
        } else if (strcmp(k, "save_lock") == 0) { /* W190 */
            v = s->save_lock ? 1 : 0;
        } else if (strcmp(k, "photo_aspect") == 0) { /* W195 */
            v = (s->photo_aspect >= 0 && s->photo_aspect <= 2) ? s->photo_aspect : 2;
        } else if (strcmp(k, "photo_view") == 0) { /* W290 */
            v = (s->photo_view >= 0 && s->photo_view <= 3) ? s->photo_view : 0;
        } else if (strcmp(k, "photo_hud") == 0) { /* W290 */
            v = s->photo_hud ? 1 : 0;
        } else if (strcmp(k, "photo_cine_self") == 0) { /* W445 */
            v = lcfg_cself_clamp(s->photo_cine_self);
        } else if (strcmp(k, "replay_label") == 0) { /* W290 */
            v = s->replay_label ? 1 : 0;
        } else if (strcmp(k, "replay_logo") == 0) {
            v = s->replay_logo ? 1 : 0;
        } else if (strcmp(k, "watermark_media") == 0) {
            v = s->watermark_media ? 1 : 0;
        } else if (strcmp(k, "photo_scale") == 0) { /* W195 */
            v = (s->photo_scale == 2 || s->photo_scale == 4 || s->photo_scale == 8) ? s->photo_scale : 1;
        } else if (strcmp(k, "interp_fps") == 0) {
            v = s->interp_fps;
        } else if (strcmp(k, SB_GP_REV_KEY) == 0) { /* W296: wer speichert, kennt Original = auto */
            v = SB_GP_REV;
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
        } else if (strcmp(k, "cockpit_width") == 0) { /* W235 */
            v = s->cockpit_width;
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
        } else if (strcmp(k, "monitor_fx") == 0) { /* W252 */
            v = s->monitor_fx;
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

/* W332 SEC: Laenge nach _snprintf_s(..., _TRUNCATE) fortschreiben; gekuerzt (-1) = Puffer voll (cap - 1),
 * nie rueckwaerts oder hinter das Ende. */
static size_t save_add(size_t len, size_t cap, int r)
{
    if (r < 0 || len + (size_t)r >= cap) {
        return cap ? cap - 1 : 0;
    }
    return len + (size_t)r;
}

int lcfg_save_user(const char *path, const launcher_settings_t *s)
{
    return lcfg_save_user_ex(path, s, 0);
}

/* W402 PERGAME: SB-Spielpfade (xbe_path, media_root, hdd_root, game_image) gehoeren nicht in user_loc.cfg */
static int key_is_sb_path(const char *k)
{
    return strcmp(k, "xbe_path") == 0 || strcmp(k, "media_root") == 0 || strcmp(k, "hdd_root") == 0 ||
           strcmp(k, "game_image") == 0;
}

int lcfg_save_user_ex(const char *path, const launcher_settings_t *s, int flags)
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
            if ((flags & LCFG_SAVE_NO_PATHS) && key_is_sb_path(kManaged[i][0])) {
                continue;
            }
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
    out_cap = (size_t)ftell(f);
    if (out_cap > 1024 * 1024) {
        fclose(f);
        return -1;
    }
    /* W332 SEC: echte Obergrenze statt fester 4 KB Reserve - jede verwaltete Zeile kann beim Ersetzen/Anhaengen
     * auf "key=<Wert bis 511>" (+ Section-Kopf) wachsen, fgets teilt ueberlange Zeilen (+1 Zeilenende je Teil).
     * Vorher lief out_len bei Kuerzung (_snprintf_s -> -1) rueckwaerts bzw. hinter den Puffer. */
    out_cap += out_cap / (sizeof line - 1) + 4096 + KMANAGED_N * (sizeof kval + 128);
    fseek(f, 0, SEEK_SET);
    out_text = (char *)malloc(out_cap);
    if (!out_text) {
        fclose(f);
        return -1;
    }
    out_text[0] = '\0'; /* W332 SEC: leere alte Datei -> strstr unten las sonst uninitialisiert hinter den Puffer */
    /* Alte Zeilen uebernehmen, verwaltete Keys ersetzen (erstes Treffen). */
    while (fgets(line, sizeof line, f)) {
        char *nl = line + strcspn(line, "\r\n");
        int replaced = 0;
        *nl = '\0';
        for (i = 0; i < KMANAGED_N; i++) {
            if ((flags & LCFG_SAVE_NO_PATHS) && key_is_sb_path(kManaged[i][0]) && line_is_key(line, kManaged[i][0])) {
                replaced = 1; /* W402: Zeile entfaellt */
                break;
            }
            if (!done[i] && line_is_key(line, kManaged[i][0])) {
                key_value(s, kManaged[i][0], kval, sizeof kval);
                out_len = save_add(out_len, out_cap, _snprintf_s(out_text + out_len,
                                               out_cap - out_len, _TRUNCATE,
                                               "%s=%s\n", kManaged[i][0], kval));
                done[i] = 1;
                replaced = 1;
                break;
            }
        }
        if (!replaced) {
            out_len = save_add(out_len, out_cap, _snprintf_s(out_text + out_len,
                                           out_cap - out_len, _TRUNCATE, "%s\n",
                                           line));
        }
    }
    fclose(f);
    /* Fehlende Keys ans Ende (eigene Section falls noetig). */
    for (i = 0; i < KMANAGED_N; i++) {
        if (!done[i] && !((flags & LCFG_SAVE_NO_PATHS) && key_is_sb_path(kManaged[i][0]))) {
            char hdr[64];
            _snprintf_s(hdr, sizeof hdr, _TRUNCATE, "[%s]", kManaged[i][1]);
            if (!strstr(out_text, hdr)) {
                out_len = save_add(out_len, out_cap, _snprintf_s(out_text + out_len,
                                               out_cap - out_len, _TRUNCATE,
                                               "%s\n", hdr));
            }
            key_value(s, kManaged[i][0], kval, sizeof kval);
            out_len = save_add(out_len, out_cap, _snprintf_s(out_text + out_len,
                                           out_cap - out_len, _TRUNCATE,
                                           "%s=%s\n", kManaged[i][0], kval));
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

int lcfg_merge_external(launcher_settings_t *ui, const launcher_settings_t *base,
                        const launcher_settings_t *disk)
{
    /* Entscheidung je Key gegen den unveraenderten Oberflaechenstand (u0),
     * damit die Reihenfolge der Uebernahmen keine Rolle spielt. */
    launcher_settings_t u0 = *ui;
    size_t i;
    int n = 0;
    for (i = 0; i < KMANAGED_N; i++) {
        const char *k = kManaged[i][0];
        char vu[512], vb[512], vd[512];
        if (is_path_key(k) || strcmp(k, "aim_turn") == 0) {
            continue; /* Pfade: Launcher; aim_turn: Altwert ohne eigenes Feld */
        }
        key_value(&u0, k, vu, sizeof vu);
        key_value(base, k, vb, sizeof vb);
        if (strcmp(vu, vb) != 0) {
            continue; /* in der Oberflaeche geaendert -> Launcher gewinnt */
        }
        key_value(disk, k, vd, sizeof vd);
        if (strcmp(vd, vu) != 0) {
            set_key(ui, k, vd);
            n++;
        }
    }
    return n;
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
    { "orig_hint", "overlay", "1" },         /* W278 ORIG-FLAG: Start-Hinweis bei experimenteller Grafik */
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

/* W231 US-LAUNCHER: Fassung einordnen (siehe launcher_cfg.h). */
void lcfg_gameinfo_from(const void *gamever, lcfg_gameinfo_t *gi)
{
    const sb_gamever_t *gv = (const sb_gamever_t *)gamever;
    memset(gi, 0, sizeof *gi);
    if (!gv || !gv->xbe_ok) {
        gi->edition = gv && gv->sha256[0] ? LCFG_ED_OTHER : LCFG_ED_NONE;
        return;
    }
    if (gv->title_id == 0x43430002u && gv->region == 1u) {
        /* NA: bekannt (ntsc_u) bzw. spaeter supported -> startbar */
        gi->edition = (gv->ntsc_u || gv->supported) ? LCFG_ED_US : LCFG_ED_US_OTHER;
        gi->startable = gi->edition == LCFG_ED_US;
        gi->needs_allow = gv->ntsc_u && !gv->supported;
        return;
    }
    if (gv->supported) {
        gi->edition = LCFG_ED_EU;
        gi->startable = 1;
        return;
    }
    gi->edition = LCFG_ED_OTHER;
}

/* W231: Pruefergebnis -> Spielertext. 1 = startbar (msg = "Image OK ..."). */
int lcfg_gamever_msg(const void *gamever, char *msg, size_t cap)
{
    const sb_gamever_t *gv = (const sb_gamever_t *)gamever;
    lcfg_gameinfo_t gi;
    char tid[16];
    lcfg_gameinfo_from(gv, &gi);
    if (gi.edition == LCFG_ED_US) {
        _snprintf_s(msg, cap, _TRUNCATE, LT(gi.needs_allow ? "gui.image_ok_us" : "gui.image_ok"),
                    gv->label);
        return 1;
    }
    if (gi.startable) {
        _snprintf_s(msg, cap, _TRUNCATE, LT("gui.image_ok"), gv->label);
        return 1;
    }
    _snprintf_s(tid, sizeof tid, _TRUNCATE, "%08X", gv->title_id);
    if (gi.edition == LCFG_ED_US_OTHER) {
        _snprintf_s(msg, cap, _TRUNCATE, LT("cfg.image_us_other"), tid,
                    sb_gamever_region_name(gv->region), gv->version);
    } else {
        _snprintf_s(msg, cap, _TRUNCATE, LT("cfg.image_version"), tid,
                    sb_gamever_region_name(gv->region), gv->version);
    }
    return 0;
}

/* W369 LAUNCHER-XBE-PICK: entpackter Spielordner statt Abbild (siehe
 * launcher_cfg.h). */
static int is_sep(char c)
{
    return c == '\\' || c == '/';
}

int lcfg_image_as_folder(const char *p, char *dir, size_t dcap, char *xbe, size_t xcap)
{
    DWORD a;
    size_t n;
    char d[1024];
    if (!p || !p[0] || (a = u8_GetFileAttributes(p)) == INVALID_FILE_ATTRIBUTES) {
        return 0;
    }
    strcpy_s(d, sizeof d, p);
    n = strlen(d);
    if (a & FILE_ATTRIBUTE_DIRECTORY) { /* Ordner: nur mit default.xbe darin */
        char x[1100];
        while (n > 3 && is_sep(d[n - 1])) {
            d[--n] = '\0';
        }
        _snprintf_s(x, sizeof x, _TRUNCATE, "%s%sdefault.xbe", d, is_sep(d[n - 1]) ? "" :
                    (strchr(d, '/') && !strchr(d, '\\')) ? "/" : "\\"); /* Trennerstil des Pfads */
        a = u8_GetFileAttributes(x);
        if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) {
            return 0;
        }
        strcpy_s(dir, dcap, d);
        strcpy_s(xbe, xcap, x);
        return 1;
    }
    if (n <= 4 || _stricmp(d + n - 4, ".xbe") != 0) {
        return 0;
    }
    strcpy_s(xbe, xcap, d);
    while (n > 0 && !is_sep(d[n - 1])) { /* Dateiname weg */
        n--;
    }
    while (n > 1 && is_sep(d[n - 1]) && !(n == 3 && d[1] == ':')) { /* Trenner weg, "C:\" bleibt */
        n--;
    }
    d[n] = '\0';
    if (!d[0]) {
        strcpy_s(d, sizeof d, ".");
    }
    strcpy_s(dir, dcap, d);
    return 1;
}

int lcfg_image_folder_mode(launcher_settings_t *s)
{
    char d[1024], x[1100];
    if (!s->game_image[0] || !lcfg_image_as_folder(s->game_image, d, sizeof d, x, sizeof x)) {
        return 0;
    }
    strcpy_s(s->media_root, sizeof s->media_root, d);
    strcpy_s(s->xbe_path, sizeof s->xbe_path, x);
    s->game_image[0] = '\0';
    return 1;
}

/* W369: Line of Contact (World-Disc oder Pilot Test) gehoert in den LoC-Tab. */
static int tid_is_loc(const sb_gamever_t *gv)
{
    return gv->xbe_ok && (gv->title_id == 0x43430009u || gv->title_id == 0x4343000Du);
}

/* W369: Programmdatei lesen (max. 64 MiB). NULL = nicht lesbar. */
static uint8_t *read_xbe_file(const char *path_u8, size_t *n)
{
    FILE *f = NULL;
    long l = 0;
    uint8_t *b = NULL;
    *n = 0;
    if (u8_fopen_s(&f, path_u8, "rb") != 0 || !f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) == 0 && (l = ftell(f)) > 0 && l < 64L * 1024 * 1024 &&
        fseek(f, 0, SEEK_SET) == 0 && (b = (uint8_t *)malloc((size_t)l)) != NULL &&
        fread(b, 1, (size_t)l, f) != (size_t)l) {
        free(b);
        b = NULL;
    }
    fclose(f);
    if (b) {
        *n = (size_t)l;
    }
    return b;
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
    {
        char d[1024], x[1100];
        if (lcfg_image_as_folder(image_u8, d, sizeof d, x, sizeof x)) { /* W369: Ordner/.xbe */
            size_t fn = 0;
            xb = read_xbe_file(x, &fn);
            memset(&gv, 0, sizeof gv);
            if (xb) {
                sb_gamever_check(xb, fn, &gv);
                free(xb);
            }
            if (!gv.xbe_ok) {
                _snprintf_s(msg, cap, _TRUNCATE, LT("cfg.image_xbebad"), x);
                return 2;
            }
            if (tid_is_loc(&gv)) {
                strncpy_s(msg, cap, LT("cfg.image_is_loc"), _TRUNCATE);
                return 5;
            }
            return lcfg_gamever_msg(&gv, msg, cap) ? 0 : 4;
        }
    }
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
    if (tid_is_loc(&gv)) { /* W369: LoC-Abbild im SB-Tab */
        strncpy_s(msg, cap, LT("cfg.image_is_loc"), _TRUNCATE);
        return 5;
    }
    return lcfg_gamever_msg(&gv, msg, cap) ? 0 : 4;
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

/* W256 PACK-ANYVER: Fassungs-Kurzname -> Anzeigename. */
static const char *pk_ver_name(const char *n)
{
    if (!n || !n[0]) {
        return "?";
    }
    if (strcmp(n, "Europe") == 0) {
        return LT("gui.pk_ver_eu");
    }
    if (strcmp(n, "USA") == 0) {
        return LT("gui.pk_ver_us");
    }
    return n;
}

int lcfg_pack_fit_text(int fit, const char *made, const char *active,
                       char *out, size_t cap)
{
    switch (fit) {
    case SB_PACK_FIT_FAMILY:
        strncpy_s(out, cap, LT("gui.pk_fit_family"), _TRUNCATE);
        return 1;
    case SB_PACK_FIT_EXACT:
        strncpy_s(out, cap, LT("gui.pk_fit_yes"), _TRUNCATE);
        return 1;
    case SB_PACK_FIT_OTHER:
        _snprintf_s(out, cap, _TRUNCATE, LT("gui.pk_fit_other"),
                    pk_ver_name(made), pk_ver_name(active));
        return 1;
    case SB_PACK_FIT_NO:
        strncpy_s(out, cap, LT("gui.pk_fit_no"), _TRUNCATE);
        return 0;
    default:
        strncpy_s(out, cap, LT("gui.pk_fit_unk"), _TRUNCATE);
        return 0;
    }
}

/* W231: default.xbe des eingestellten Spiels lesen (Abbild oder Ordner). */
static uint8_t *read_game_xbe(const launcher_settings_t *s, uint32_t *n)
{
    uint8_t *xb = NULL;
    *n = 0;
    if (s->game_image[0]) {
        char acp[1024], e[320];
        iso_vfs_t *v;
        DWORD a = u8_GetFileAttributes(s->game_image);
        if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) {
            return NULL;
        }
        u8_to_acp(s->game_image, acp, sizeof acp);
        v = iso_vfs_open(acp, e, sizeof e);
        if (!v) {
            return NULL;
        }
        if (iso_vfs_read_all(v, "default.xbe", &xb, n) != 0) {
            xb = NULL;
        }
        iso_vfs_close(v);
        return xb;
    } else {
        char xbe[1024];
        FILE *xf = NULL;
        long xl;
        lcfg_xbe_effective(s, xbe, sizeof xbe);
        if (!xbe[0] || u8_fopen_s(&xf, xbe, "rb") != 0 || !xf) {
            return NULL;
        }
        fseek(xf, 0, SEEK_END);
        xl = ftell(xf);
        fseek(xf, 0, SEEK_SET);
        xb = (xl > 0 && xl < 64L * 1024 * 1024) ? (uint8_t *)malloc((size_t)xl) : NULL;
        if (xb && fread(xb, 1, (size_t)xl, xf) != (size_t)xl) {
            free(xb);
            xb = NULL;
        }
        fclose(xf);
        *n = xb ? (uint32_t)xl : 0;
        return xb;
    }
}

int lcfg_game_info(const launcher_settings_t *s, lcfg_gameinfo_t *gi)
{
    uint32_t n = 0;
    uint8_t *xb = read_game_xbe(s, &n);
    sb_gamever_t gv;
    memset(gi, 0, sizeof *gi);
    if (!xb) {
        return 0;
    }
    sb_gamever_check(xb, n, &gv);
    free(xb);
    lcfg_gameinfo_from(&gv, gi);
    return 1;
}

void lcfg_hdd_for_edition(const char *hdd_base, int edition, char *out, size_t cap)
{
    size_t n;
    strcpy_s(out, cap, hdd_base ? hdd_base : "");
    if (edition != LCFG_ED_US) {
        return;
    }
    n = strlen(out);
    while (n > 0 && (out[n - 1] == '\\' || out[n - 1] == '/')) {
        out[--n] = '\0';
    }
    strcat_s(out, cap, n ? "\\us" : "us");
}

int lcfg_edition_set_args(const lcfg_gameinfo_t *gi, const char *us_hdd_acp,
                          char *set, size_t cap)
{
    char add[1400], esc[1200];
    size_t i, j = 0;
    add[0] = '\0';
    if (!gi || gi->edition != LCFG_ED_US) {
        return 0;
    }
    if (gi->needs_allow) { /* entfaellt, sobald sb_gamever US als supported fuehrt */
        strcat_s(add, sizeof add, "allow_ntsc_u=1");
    }
    if (us_hdd_acp && us_hdd_acp[0]) {
        for (i = 0; us_hdd_acp[i] && j + 3 < sizeof esc; i++) {
            if (us_hdd_acp[i] == ';') {
                esc[j++] = '\\';
            }
            esc[j++] = us_hdd_acp[i];
        }
        esc[j] = '\0';
        if (us_hdd_acp[i]) {
            return 1;
        }
        if (add[0]) {
            strcat_s(add, sizeof add, ";");
        }
        if (strlen(add) + 12 + strlen(esc) + 1 > sizeof add) {
            return 1;
        }
        strcat_s(add, sizeof add, "hdd_root_us="); /* W230-Merge: Client nimmt den Ordner so wie er ist */
        strcat_s(add, sizeof add, esc);
    }
    if (!add[0]) {
        return 0;
    }
    if (strlen(set) + 1 + strlen(add) + 1 > cap) {
        return 1;
    }
    if (set[0]) {
        strcat_s(set, cap, ";");
    }
    strcat_s(set, cap, add);
    return 0;
}

const char *lcfg_exit_msg_key(unsigned long code, int edition)
{
    if (code == 5) {
        return edition == LCFG_ED_US ? "gui.exit5_us" : "gui.exit5";
    }
    if (code == 8) {
        return "gui.exit8";
    }
    if (code == 39) { /* W339: LoC-Netzspiel nach Gastcode-Ausnahme beendet (Absturz = Ende, SECURITY.md §6.6) */
        return "gui.exit39";
    }
    return NULL;
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
    return lcfg_validate_ex(s, err, errcap, 0);
}

int lcfg_validate_ex(const launcher_settings_t *s, char *err, size_t errcap, int flags)
{
    DWORD a;
    char probe[1024];
    char xbe[1024]; /* W172: Programmdatei (im Ordner-Modus abgeleitet) */
    int nogame = (flags & LCFG_V_NO_GAME) != 0; /* W375: LoC-Tab speichert ohne SB-Spieldaten (#50) */

#define FAIL(fmt, ...)                                     \
    do {                                                   \
        _snprintf_s(err, errcap, _TRUNCATE, fmt, __VA_ARGS__); \
        return 1;                                          \
    } while (0)

    xbe[0] = 0;
    if (nogame) {
        goto paths_ok; /* W375: SB-Spieldaten (Abbild/Ordner/xbe) nicht pruefen, uebrige Werte schon */
    }
    if (s->game_image[0]) { /* W369: .xbe/Ordner im Abbild-Feld = Ordner-Modus */
        launcher_settings_t t = *s;
        if (lcfg_image_folder_mode(&t)) {
            return lcfg_validate_ex(&t, err, errcap, flags);
        }
    }
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
                sb_gamever_check(xb, (size_t)xn, &gv);
                if (tid_is_loc(&gv)) { /* W369: LoC-xbe im SB-Tab (Erweitert) */
                    strncpy_s(err, errcap, LT("cfg.image_is_loc"), _TRUNCATE);
                    fclose(xf);
                    free(xb);
                    return 1;
                }
                if (!lcfg_gamever_msg(&gv, err, errcap)) { /* W231: US experimental startbar */
                    fclose(xf);
                    free(xb);
                    return 1;
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
            if ((s->game_image[0] || nogame) && pi2 < 2) {
                continue; /* W150: Abbild-Modus nutzt xbe_path/media_root nicht (W375: ohne Spieldaten-Pruefung) */
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
    if (sb_fps_choice_idx(s->interp_fps) < 0) { /* W223: auto (-1), 0 = 30, 60..240 aus der Liste */
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
    if (s->cockpit_width < 0 || s->cockpit_width > 10) { /* W235 */
        FAIL(LT("cfg.bad_cwidth"), s->cockpit_width);
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
    s->gfx_preset_rev = 0; /* W296: Stand kommt aus der Datei (fehlt = alt) */
    if (lcfg_load_file(path, s) != 0) {
        _snprintf_s(err, errcap, _TRUNCATE, LT("cfg.prof_notfound"), name, path);
        return 1;
    }
    lcfg_gfx_migrate_fps(s); /* W296 PRESET-ORIG: altes Original 120 -> auto */
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
