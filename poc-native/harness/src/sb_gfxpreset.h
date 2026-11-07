#pragma once
/* sb_gfxpreset.h - W173 LAUNCHER-P4: einheitliches Grafik-Preset.
 *
 * Einzige Quelle der Preset-Werte fuer den Client (render_shfx.c,
 * render_fx_rem.c, ESC-Menue sb_overlay.c) und den Launcher
 * (launcher_cfg.c). Das Preset BESITZT genau diese Schluessel: weicht einer
 * davon ab, heisst der Zustand "Benutzerdefiniert" (Custom).
 *
 * Spalten: 0 = Original (HD) = Client-Standard (hcfg.c/sb_native.cfg, also
 * bitgleich zum Standardpfad; shading=1 ist die NV2A-getreue Darstellung
 * seit 5.11., kein Remaster), 1 = Remastered dezent, 2 = Remastered stark.
 * Werte der Shader-Schluessel wie W131/W136 (BERICHT_M2), Effekte wie W133.
 *
 * Nicht im Preset (PO 7.11.2026): hud_label_avoid (Lesbarkeit, eigene
 * Option Grafik > HUD, Standard 0). Ebenfalls nicht: Anzeige, Bildrate,
 * Cockpit-Monitor, Texturdetail (hd_scale, Seite Packs).
 *
 * hd_textures steht fuer "HD-Texturen aus Packs" (Client wirksam als
 * remaster && hd_textures; der Launcher schreibt remaster = hd_textures).
 *
 * Effect-Pack-Ausnahme (PACK_FORMAT 9.3): Schluessel, die ein aktives
 * Effect Pack setzt, gelten im Spiel vor dem Preset und zaehlen bei der
 * Erkennung nicht (Launcher: lcfg_gfx_preset, Client: sb_gfx_preset_match).
 * ASCII only. */

#define SB_GP_NPRE 3 /* Original (HD), dezent, stark */

typedef struct {
    const char *key; /* cfg-Schluessel (hcfg.c / user.cfg) */
    int v[SB_GP_NPRE];
} sb_gp_key_t;

static const sb_gp_key_t kSbGfxPreset[] = {
    /* Shader (W121/W131, Reihenfolge wie launcher_cfg.c kShxKey) */
    { "shading",          {   1,   1,   1 } },
    { "world_fog",        { 100, 100, 100 } },
    { "world_contrast",   { 100, 100, 110 } },
    { "world_gamma",      { 100, 100, 100 } },
    { "world_sharpen",    {   0,  60,  60 } },
    { "bloom_strength",   { 100, 100, 120 } },
    { "cockpit_glow",     {   0,   0,   0 } },
    { "cockpit_spill",    {   0,   0,   0 } },
    { "cockpit_light",    { 100, 100, 100 } },
    { "world_exposure",   { 100, 100, 100 } },
    { "tonemap",          {   0,   0,   1 } },
    { "bloom_threshold",  {   0,   0,  20 } },
    { "light_world",      { 100, 100, 100 } },
    { "light_cockpit",    { 100, 100, 100 } },
    { "world_focus",      { 100, 100,  50 } },
    { "aa_mode",          {   0,   3,   3 } },
    { "ssao",             {   0,  25,  60 } },
    { "color_sat",        { 100, 105, 115 } },
    { "color_temp",       {   0,   0,  10 } },
    { "color_lift",       {   0,   0,   0 } },
    { "color_gamma",      { 100, 100, 100 } },
    { "color_gain",       { 100, 103, 110 } },
    { "shade_shadow",     {   0,   1,   1 } },
    { "shadow_soft",      {   0,   0,   1 } },
    /* Effekte (W133, ESC-Seite "Effekte"; fx_interp: 0/1) */
    { "fx_soft",          {   0,  60, 100 } },
    { "fx_embers",        {   0,  50, 120 } },
    { "fx_light",         {   0,  40, 100 } },
    { "fx_light_cockpit", {   0,  30,  80 } },
    { "fx_interp",        {   0,   1,   1 } },
    /* HD-Texturen aus Packs */
    { "hd_textures",      {   0,   1,   1 } },
};
#define SB_GP_NKEY ((int)(sizeof kSbGfxPreset / sizeof kSbGfxPreset[0]))
#define SB_GP_FX0 24 /* erster Effekt-Schluessel (fx_soft) */
#define SB_GP_NFX 5
#define SB_GP_HD 29  /* hd_textures */

/* Index des Schluessels, -1 = nicht im Preset. */
static int sb_gp_find(const char *key)
{
    int i;
    for (i = 0; i < SB_GP_NKEY; i++) {
        const char *a = kSbGfxPreset[i].key, *b = key;
        while (*a && *a == *b) { a++; b++; }
        if (*a == *b) { return i; }
    }
    return -1;
}
