#pragma once
/* sb_gfxpreset.h - W173 LAUNCHER-P4: einheitliches Grafik-Preset.
 *
 * Einzige Quelle der Preset-Werte fuer den Client (render_shfx.c,
 * render_fx_rem.c, ESC-Menue sb_overlay.c) und den Launcher
 * (launcher_cfg.c). Das Preset BESITZT genau diese Schluessel: weicht einer
 * davon ab, heisst der Zustand "Benutzerdefiniert" (Custom).
 *
 * Spalten: 0 = Original (HD) = Client-Standard (hcfg.c/sb_native.cfg, also
 * Optik bitgleich zum Standardpfad; shading=1 ist die NV2A-getreue Darstellung
 * seit 5.11., kein Remaster), 1 = Remastered dezent, 2 = Remastered stark, 3 = Original Performance.
 * W207: 60 FPS, native RTs (1), no dynamic scale or exclusive Hz override.
 * Rate/Skala: Spieler-Standard 120/auto (Harness-Teststandard rt_scale=1
 * bleibt unveraendert). Auswahl setzt diese Werte; Laden migriert nichts.
 * Werte der Shader-Schluessel wie W131/W136 (BERICHT_M2), Effekte wie W133.
 *
 * Nicht im Preset (PO 7.11.2026): hud_label_avoid (Lesbarkeit, eigene
 * Option Grafik > HUD, Standard 0). Ebenfalls nicht: Ausgabeformat,
 * Cockpit-Monitor, Texturdetail (hd_scale, Seite Packs).
 *
 * hd_textures steht fuer "HD-Texturen aus Packs" (Client wirksam als
 * remaster && hd_textures; der Launcher schreibt remaster = hd_textures).
 *
 * Effect-Pack-Ausnahme (PACK_FORMAT 9.3): Schluessel, die ein aktives
 * Effect Pack setzt, gelten im Spiel vor dem Preset und zaehlen bei der
 * Erkennung nicht (Launcher: lcfg_gfx_preset, Client: sb_gfx_preset_match).
 * ASCII only. */

#define SB_GP_NPRE 4 /* Original (HD), dezent, stark, Performance */
#define SB_GP_PERFORMANCE 3

typedef struct {
    const char *key; /* cfg-Schluessel (hcfg.c / user.cfg) */
    int v[SB_GP_NPRE];
} sb_gp_key_t;

static const sb_gp_key_t kSbGfxPreset[] = {
    /* Shader (W121/W131, Reihenfolge wie launcher_cfg.c kShxKey) */
    { "shading",          {   1,   1,   1,   1 } },
    { "world_fog",        { 100, 100, 100, 100 } },
    { "world_contrast",   { 100, 100, 110, 100 } },
    { "world_gamma",      { 100, 100, 100, 100 } },
    { "world_sharpen",    {   0,  60,  60,   0 } },
    { "bloom_strength",   { 100, 100, 120, 100 } },
    { "cockpit_glow",     {   0,   0,   0,   0 } },
    { "cockpit_spill",    {   0,   0,   0,   0 } },
    { "cockpit_light",    { 100, 100, 100, 100 } },
    { "world_exposure",   { 100, 100, 100, 100 } },
    { "tonemap",          {   0,   0,   1,   0 } },
    { "bloom_threshold",  {   0,   0,  20,   0 } },
    { "light_world",      { 100, 100, 100, 100 } },
    { "light_cockpit",    { 100, 100, 100, 100 } },
    { "world_focus",      { 100, 100,  50, 100 } },
    { "aa_mode",          {   0,   3,   3,   0 } },
    { "ssao",             {   0,  25,  60,   0 } },
    { "color_sat",        { 100, 105, 115, 100 } },
    { "color_temp",       {   0,   0,  10,   0 } },
    { "color_lift",       {   0,   0,   0,   0 } },
    { "color_gamma",      { 100, 100, 100, 100 } },
    { "color_gain",       { 100, 103, 110, 100 } },
    { "shade_shadow",     {   0,   1,   1,   0 } },
    { "shadow_soft",      {   0,   0,   1,   0 } },
    /* Effekte (W133, ESC-Seite "Effekte"; fx_interp: 0/1) */
    { "fx_soft",          {   0,  60, 100,   0 } },
    { "fx_embers",        {   0,  50, 120,   0 } },
    { "fx_light",         {   0,  40, 100,   0 } },
    { "fx_light_cockpit", {   0,  30,  80,   0 } },
    { "fx_interp",        {   0,   1,   1,   0 } },
    /* HD-Texturen aus Packs */
    { "hd_textures",      {   0,   1,   1,   0 } },
    /* W207: selection owns rate/scaling; existing cfg files stay readable. */
    { "interp_fps",      { 120, 120, 120,  60 } },
    { "rt_scale",        {   0,   0,   0,   1 } },
    { "rt_scale_dyn",    {   0,   0,   0,   0 } },
    { "fs_interp_match", {   1,   1,   1,   0 } },
};
#define SB_GP_NKEY ((int)(sizeof kSbGfxPreset / sizeof kSbGfxPreset[0]))
#define SB_GP_FX0 24 /* erster Effekt-Schluessel (fx_soft) */
#define SB_GP_NFX 5
#define SB_GP_HD 29  /* hd_textures */
#define SB_GP_FPS 30
#define SB_GP_RT 31
#define SB_GP_DYN 32
#define SB_GP_MATCH 33

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
