/* launcher_orig.c - W278 ORIG-FLAG (Launcher). Beschreibung s.
 * launcher_orig.h, Einordnung s. harness/src/sb_origflag.h. ASCII only. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "launcher_orig.h"
#include "launcher_u8.h"
#include "sb_lang.h"
#include "sb_gfxpreset.h"
#include "sb_origflag.h"

/* Feld je Tabellenschluessel (NULL = keins; hd_textures gesondert). */
static int *lorig_field(launcher_settings_t *s, const char *k)
{
    int i;
    for (i = 0; i < LSHX_N; i++) {
        if (strcmp(lcfg_shx_key(i), k) == 0) { return &s->shx[i]; } /* Shader + hud_label_avoid */
    }
    for (i = 0; i < LFX_N; i++) {
        if (strcmp(lcfg_fx_key(i), k) == 0) { return &s->fx[i]; }
    }
    if (strcmp(k, "fx_wireframe") == 0) { return &s->fx_wireframe; }
    if (strcmp(k, "fx_vaporwave") == 0) { return &s->fx_vaporwave; }
    if (strcmp(k, "monitor_fx") == 0) { return &s->monitor_fx; }
    if (strcmp(k, "monitor_grain") == 0) { return &s->monitor_grain; }
    if (strcmp(k, "monitor_glitch") == 0) { return &s->monitor_glitch; }
    if (strcmp(k, "hd_textures") == 0) { return &s->hd_textures; }
    if (strcmp(k, "edge_mode") == 0) { return &s->edge_mode; }
    if (strcmp(k, "edge_fade_width") == 0) { return &s->edge_fade_width; }
    if (strcmp(k, "edge_fade_strength") == 0) { return &s->edge_fade_strength; }
    if (strcmp(k, "cockpit_persp") == 0) { return &s->cockpit_persp; }
    if (strcmp(k, "cockpit_panini") == 0) { return &s->cockpit_panini; }
    if (strcmp(k, "cockpit_width") == 0) { return &s->cockpit_width; }
    if (strcmp(k, "cockpit_hide") == 0) { return &s->cockpit_hide; }
    if (strcmp(k, "monitor_view") == 0) { return &s->monitor_view; } /* W319 */
    return NULL;
}

int lorig_value(const launcher_settings_t *s, const char *key)
{
    int *p = lorig_field((launcher_settings_t *)s, key);
    if (!p) { return -1; }
    if (strcmp(key, "hd_textures") == 0) { return (s->remaster && s->hd_textures) ? 1 : 0; }
    return *p;
}

unsigned lorig_mask(const launcher_settings_t *s, int fx_pack)
{
    unsigned m = 0;
    int i;
    for (i = 0; i < SB_OF_NKEY; i++) {
        int v = lorig_value(s, kSbOrigFlag[i].key);
        if (v >= 0 && sb_of_dev(i, v)) { m |= 1u << kSbOrigFlag[i].grp; }
    }
    if (fx_pack) { m |= 1u << SB_OFG_PACKS; }
    return m;
}

void lorig_list_sep(unsigned mask, const char *sep, const char *pre, char *out, size_t cap)
{
    int k, n = 0;
    out[0] = '\0';
    for (k = 0; k < SB_OFG_N; k++) {
        int g = kSbOrigGroupOrder[k];
        if (!(mask & (1u << g))) { continue; }
        if (n++) { strncat_s(out, cap, sep, _TRUNCATE); }
        strncat_s(out, cap, pre, _TRUNCATE);
        strncat_s(out, cap, LT(kSbOrigGroupKey[g]), _TRUNCATE);
    }
}

void lorig_list(unsigned mask, char *out, size_t cap)
{
    lorig_list_sep(mask, ", ", "", out, cap);
}

unsigned lorig_preset_mask(int preset)
{
    launcher_settings_t t;
    lcfg_defaults(&t);
    lcfg_gfx_apply(&t, preset);
    return lorig_mask(&t, 0);
}

void lorig_reset(launcher_settings_t *s)
{
    int i;
    for (i = 0; i < SB_OF_NKEY; i++) {
        int *p = lorig_field(s, kSbOrigFlag[i].key);
        if (p) { *p = kSbOrigFlag[i].orig; }
    }
    s->remaster = 0;
    s->hd_textures = 0;
    lcfg_gfx_normalize(s);
}

int lorig_once_args(char *out, size_t cap)
{
    int i;
    char t[64];
    out[0] = '\0';
    for (i = 0; i < SB_OF_NKEY; i++) {
        _snprintf_s(t, sizeof t, _TRUNCATE, "%s=%d;", kSbOrigFlag[i].key, kSbOrigFlag[i].orig);
        if (strcat_s(out, cap, t) != 0) { out[0] = '\0'; return 1; }
    }
    /* HD aus, keine Packs (leere Liste: weder Texture noch Effect Packs), Einblendung */
    if (strcat_s(out, cap, "remaster=0;fx_enhanced=0;pack_list=;orig_once=1") != 0) { out[0] = '\0'; return 1; }
    return 0;
}

/* ---- Selbsttest -------------------------------------------------------- */
static void st_res(int ok, const char *name, const char *why, int *pass, int *fail)
{
    if (ok) {
        printf("PASS %s\n", name);
        (*pass)++;
    } else {
        printf("FAIL %s: %s\n", name, why);
        (*fail)++;
    }
}

static int st_has(const char *set, const char *kv)
{
    size_t n = strlen(kv);
    const char *p = set;
    while ((p = strstr(p, kv)) != NULL) {
        if ((p == set || p[-1] == ';') && (p[n] == ';' || p[n] == '\0')) { return 1; }
        p += n;
    }
    return 0;
}

void lorig_selftest(int *pass, int *fail)
{
    char why[400] = "";
    launcher_settings_t a, b;
    int i, k;

    /* --- 1: Einordnung (eine Tabelle; neutral nie experimentell; Presets) --- */
    for (i = 0; i < SB_OF_NKEY && !why[0]; i++) {
        launcher_settings_t t;
        lcfg_defaults(&t);
        if (lorig_value(&t, kSbOrigFlag[i].key) < 0) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "%s ohne Launcher-Feld", kSbOrigFlag[i].key);
        }
    }
    for (i = 0; i < SB_OF_NNEUTRAL && !why[0]; i++) {
        if (sb_of_find(kSbOrigNeutral[i]) >= 0) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "neutraler Schluessel %s steht als experimentell", kSbOrigNeutral[i]);
        }
    }
    for (i = 0; i < SB_OF_NGAMEPLAY && !why[0]; i++) { /* W424: Gameplay-Abweichung mit Standard an, nie gekennzeichnet */
        if (sb_of_find(kSbOrigGameplay[i]) >= 0) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "Gameplay-Schluessel %s steht als experimentell", kSbOrigGameplay[i]);
        }
    }
    for (k = 0; k < SB_GP_NKEY && !why[0]; k++) { /* Preset-Schluessel: alles ausser Bildrate/Aufloesung */
        const char *key = kSbGfxPreset[k].key;
        int neutral = k == SB_GP_FPS || k == SB_GP_RT || k == SB_GP_DYN || k == SB_GP_MATCH;
        if (neutral != (sb_of_find(key) < 0)) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "Preset-Schluessel %s falsch eingeordnet", key);
        } else if (!neutral && kSbGfxPreset[k].v[0] != kSbOrigFlag[sb_of_find(key)].orig) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "Original-Preset %s=%d != Tabelle %d", key, kSbGfxPreset[k].v[0],
                        kSbOrigFlag[sb_of_find(key)].orig);
        }
    }
    if (!why[0]) { /* Original/Performance original, Remastered/Klare Sicht experimentell */
        static const int want[SB_GP_NPRE] = { 0, 1, 1, 0, 1 };
        int p;
        for (p = 0; p < SB_GP_NPRE && !why[0]; p++) {
            launcher_settings_t t;
            lcfg_defaults(&t);
            lcfg_gfx_apply(&t, p);
            if ((lorig_mask(&t, 0) != 0) != want[p]) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "Preset %d: Maske 0x%X (erwartet %s)", p, lorig_mask(&t, 0),
                            want[p] ? "experimentell" : "original");
            }
        }
    }
    if (!why[0]) { /* Einzelne Gruppen */
        static const struct { const char *set; unsigned grp; } kc[] = {
            { "edge_mode=2", 1u << SB_OFG_WIDE }, { "edge_mode=0", 1u << SB_OFG_WIDE },
            { "cockpit_persp=40", 1u << SB_OFG_WIDE }, { "cockpit_panini=30", 1u << SB_OFG_WIDE },
            { "cockpit_width=4", 1u << SB_OFG_WIDE }, { "cockpit_hide=1", 1u << SB_OFG_COCKPIT },
            { "monitor_fx=0", 1u << SB_OFG_MONITOR }, { "monitor_grain=50", 1u << SB_OFG_MONITOR },
            { "remaster=1;hd_textures=1", 1u << SB_OFG_HD }, { "world_sharpen=60", 1u << SB_OFG_POST },
            { "shading=0", 1u << SB_OFG_LIGHT }, { "light_world=150", 1u << SB_OFG_LIGHT },
            { "fx_embers=50", 1u << SB_OFG_FX }, { "fx_vaporwave=1", 1u << SB_OFG_FX },
            { "hud_label_avoid=1", 1u << SB_OFG_HUD },
            /* neutral */
            { "interp_fps=60", 0 }, { "rt_scale=2", 0 }, { "window_mode=1", 0 }, { "aim_assist=1", 0 },
            { "auto_gear=1", 0 }, { "game_tick=25", 0 }, { "vr_enable=1", 0 }, { "hd_scale=2", 0 },
            { "edge_fade_width=20", 0 }, { "hd_textures=1", 0 }, /* ohne remaster nicht wirksam */
        };
        int c;
        for (c = 0; c < (int)(sizeof kc / sizeof kc[0]) && !why[0]; c++) {
            launcher_settings_t t;
            lcfg_defaults(&t);
            lcfg_apply_set(&t, kc[c].set);
            if (lorig_mask(&t, 0) != kc[c].grp) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "%s -> Maske 0x%X (erwartet 0x%X)", kc[c].set, lorig_mask(&t, 0),
                            kc[c].grp);
            }
        }
        if (!why[0]) {
            lcfg_defaults(&a);
            if (lorig_mask(&a, 1) != (1u << SB_OFG_PACKS)) { strcpy_s(why, sizeof why, "aktives Effect Pack nicht gemeldet"); }
        }
    }
    st_res(!why[0], "W278 origflag: Einordnung (eine Tabelle, neutrale Schluessel nie, Presets Original/Performance original, "
                    "Remastered/Klare Sicht experimentell, alle Gruppen)", why, pass, fail);

    /* --- 2: Standard 4:3 (Neuinstallation original, kein Hinweis) --- */
    why[0] = '\0';
    lcfg_defaults(&a);
    if (a.edge_mode != 1) { _snprintf_s(why, sizeof why, _TRUNCATE, "Standard edge_mode=%d (erwartet 1 = 4:3)", a.edge_mode); }
    else if (a.edge_fade_width != 8 || a.edge_fade_strength != 100) { strcpy_s(why, sizeof why, "Breitbild-Standard nicht 8/100"); }
    else if (lorig_mask(&a, 0) != 0) { _snprintf_s(why, sizeof why, _TRUNCATE, "Standard experimentell (0x%X)", lorig_mask(&a, 0)); }
    else if (lcfg_gfx_preset(&a, 0) != LGP_ORIGINAL) { strcpy_s(why, sizeof why, "Standard ist nicht Preset Original"); }
    st_res(!why[0], "W278 origflag: Standard = Original mit 4:3 (edge_mode=1, Breitbild-Werte 8 %/100 % bleiben), kein Hinweis",
           why, pass, fail);

    /* --- 3: Hinweis an/aus + Liste --- */
    why[0] = '\0';
    {
        char list[300];
        const char *code = sb_lang_code();
        char keep[8];
        strcpy_s(keep, sizeof keep, code ? code : "en");
        sb_lang_set("en");
        lcfg_defaults(&a);
        lcfg_apply_set(&a, "edge_mode=2;remaster=1;hd_textures=1;light_world=150");
        lorig_list(lorig_mask(&a, 0), list, sizeof list);
        if (strcmp(list, "HD textures, widescreen, lighting") != 0) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "Liste EN '%s'", list);
        }
        sb_lang_set("de");
        lorig_list(lorig_mask(&a, 1), list, sizeof list);
        if (!why[0] && strcmp(list, "HD-Texturen, Effect Packs, Breitbild, Licht") != 0) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "Liste DE '%s'", list);
        }
        sb_lang_set(keep);
        lorig_reset(&a);
        if (!why[0] && lorig_mask(&a, 0) != 0) { strcpy_s(why, sizeof why, "nach Zuruecksetzen weiter experimentell"); }
    }
    st_res(!why[0], "W278 origflag: Hinweis an/aus (Liste EN/DE in fester Reihenfolge, aus nach Zuruecksetzen)", why, pass, fail);

    /* --- 4: Back to original setzt NUR Grafik zurueck --- */
    why[0] = '\0';
    lcfg_defaults(&a);
    lcfg_gfx_apply(&a, LGP_STRONG);
    lcfg_apply_set(&a, "edge_mode=0;cockpit_persp=50;cockpit_panini=20;cockpit_width=5;cockpit_hide=1;monitor_fx=0;"
                       "monitor_grain=40;hud_label_avoid=1;fx_wireframe=1;fx_vaporwave=1;edge_fade_width=20;"
                       "interp_fps=60;rt_scale=2;window_mode=2;width=1920;height=1080;aim_assist=1;auto_gear=1;"
                       "mouse_sens=9;audio=0;vr_preset=3;game_tick=25;hd_scale=2;lang=de;stick_aim=relative");
    b = a;
    lorig_reset(&a);
    if (lorig_mask(&a, 0) != 0) { _snprintf_s(why, sizeof why, _TRUNCATE, "danach Maske 0x%X", lorig_mask(&a, 0)); }
    else if (a.edge_mode != 1 || a.edge_fade_width != 8 || a.edge_fade_strength != 100) { strcpy_s(why, sizeof why, "Raender nicht 4:3 (1/8/100)"); }
    else if (a.interp_fps != 60 || a.rt_scale != 2 || a.window_mode != 2 || a.width != 1920 || a.height != 1080) {
        strcpy_s(why, sizeof why, "Bildrate/Aufloesung/Fenster veraendert");
    } else if (a.aim_assist != 1 || a.auto_gear != 1 || a.mouse_sens != 9 || a.stick_aim != 1) {
        strcpy_s(why, sizeof why, "Steuerung veraendert");
    } else if (a.audio != 0 || a.vr_preset != b.vr_preset || a.game_tick != 25 || a.hd_scale != 2 ||
               strcmp(a.lang, "de") != 0) {
        strcpy_s(why, sizeof why, "Ton/VR/Takt/Texturdetail/Sprache veraendert");
    } else if (a.remaster || a.hd_textures || a.fx_enhanced) {
        strcpy_s(why, sizeof why, "HD/Effekte nicht aus");
    } else if (lcfg_gfx_preset(&a, 0) != LGP_PERFORMANCE && lcfg_gfx_preset(&a, 0) != LGP_CUSTOM) {
        strcpy_s(why, sizeof why, "Preset nach Zuruecksetzen unerwartet");
    }
    st_res(!why[0], "W278 origflag: Back to original setzt nur die experimentelle Grafik zurueck (FPS, Aufloesung, Fenster, "
                    "Steuerung, Ton, VR, Takt, Sprache bleiben)", why, pass, fail);

    /* --- 5: alte user.cfg (Breitbild gewaehlt) bleibt unveraendert, Hinweis an --- */
    why[0] = '\0';
    {
        char tmp[1024], cfg[1200], ln[512];
        FILE *f = NULL;
        int seen = 0;
        u8_GetTempPath(tmp, sizeof tmp);
        _snprintf_s(cfg, sizeof cfg, _TRUNCATE, "%sw278_old_user.cfg", tmp);
        if (u8_fopen_s(&f, cfg, "w") == 0 && f) {
            fputs("[render]\nedge_mode=2\nedge_fade_width=10\nedge_fade_strength=100\n# eigener Kommentar\n", f);
            fclose(f);
        }
        lcfg_defaults(&a);
        if (lcfg_load_file(cfg, &a) != 0) { strcpy_s(why, sizeof why, "alte cfg nicht lesbar"); }
        else if (a.edge_mode != 2 || a.edge_fade_width != 10) { strcpy_s(why, sizeof why, "Breitbild-Wahl beim Laden umgestellt"); }
        else if (!(lorig_mask(&a, 0) & (1u << SB_OFG_WIDE))) { strcpy_s(why, sizeof why, "Breitbild ohne Hinweis"); }
        else if (lcfg_save_user(cfg, &a) != 0) { strcpy_s(why, sizeof why, "Speichern gescheitert"); }
        else {
            f = NULL;
            if (u8_fopen_s(&f, cfg, "r") == 0 && f) {
                while (fgets(ln, sizeof ln, f)) {
                    if (strncmp(ln, "edge_mode=2", 11) == 0) { seen |= 1; }
                    if (strncmp(ln, "edge_mode=1", 11) == 0) { seen |= 2; }
                    if (strncmp(ln, "# eigener Kommentar", 19) == 0) { seen |= 4; }
                }
                fclose(f);
            }
            if (seen != 5) { _snprintf_s(why, sizeof why, _TRUNCATE, "gespeicherte Datei falsch (Bits %d)", seen); }
        }
        u8_DeleteFile(cfg);
    }
    st_res(!why[0], "W278 origflag: bestehende user.cfg mit Breitbild bleibt beim Laden/Speichern unveraendert (keine Migration), "
                    "Hinweis aktiv", why, pass, fail);

    /* --- 6: Einmal-Kette --- */
    why[0] = '\0';
    {
        char set[2048];
        launcher_settings_t t;
        if (lorig_once_args(set, sizeof set) != 0) { strcpy_s(why, sizeof why, "Kette zu lang"); }
        else if (!st_has(set, "edge_mode=1") || !st_has(set, "shading=1") || !st_has(set, "monitor_fx=1") ||
                 !st_has(set, "hd_textures=0") || !st_has(set, "remaster=0") || !st_has(set, "pack_list=") ||
                 !st_has(set, "orig_once=1") || !st_has(set, "cockpit_hide=0") || !st_has(set, "fx_vaporwave=0")) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "Kette unvollstaendig: %.200s", set);
        } else if (strstr(set, "interp_fps") || strstr(set, "rt_scale") || strstr(set, "window_mode") ||
                   strstr(set, "aim_assist") || strstr(set, "vr_")) {
            strcpy_s(why, sizeof why, "Kette enthaelt neutrale Schluessel");
        } else {
            lcfg_defaults(&t);
            lcfg_gfx_apply(&t, LGP_STRONG);
            lcfg_apply_set(&t, "edge_mode=2;cockpit_hide=1");
            lcfg_apply_set(&t, set);
            if (lorig_mask(&t, 0) != 0) { _snprintf_s(why, sizeof why, _TRUNCATE, "Kette laesst 0x%X uebrig", lorig_mask(&t, 0)); }
        }
    }
    st_res(!why[0], "W278 origflag: Einmal-Kette (alle experimentellen Werte original, HD aus, leere Packs-Liste, orig_once=1, "
                    "nichts Neutrales)", why, pass, fail);
}
