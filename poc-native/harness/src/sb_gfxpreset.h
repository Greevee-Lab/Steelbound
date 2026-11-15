#pragma once
/* sb_gfxpreset.h - W173 LAUNCHER-P4: einheitliches Grafik-Preset.
 *
 * Einzige Quelle der Preset-Werte fuer den Client (render_shfx.c,
 * render_fx_rem.c, ESC-Menue sb_overlay.c) und den Launcher
 * (launcher_cfg.c). Das Preset BESITZT genau diese Schluessel: weicht einer
 * davon ab, heisst der Zustand "Benutzerdefiniert" (Custom).
 *
 * Spalten: 0 = Original = Client-Standard (hcfg.c/sb_native.cfg, also
 * Optik bitgleich zum Standardpfad; shading=1 ist die NV2A-getreue Darstellung
 * seit 5.11., kein Remaster; Ausnahme Breitbild-Raender, s. W266 unten),
 * 1 = Remastered dezent, 2 = Remastered stark, 3 = Performance (bis W296 "Original - Performance"),
 * 4 = Klare Sicht (W252: wie Original, aber shading=0 und monitor_fx=0;
 * Nutzerwunsch 14.11., besser zu sehen, nicht die Original-Optik).
 * W207: 60 FPS, native RTs (1), no dynamic scale or exclusive Hz override.
 * Rate/Skala: Spieler-Standard auto (W296; vorher 120)/auto (Harness-Teststandard rt_scale=1
 * bleibt unveraendert). Auswahl setzt diese Werte; Laden migriert nichts
 * (Ausnahme W296: altes Original 120 -> auto, s. unten).
 * Werte der Shader-Schluessel wie W131/W136 (BERICHT_M2), Effekte wie W133.
 *
 * W252: monitor_fx gehoert zum Preset (nur 0/1; 2 = DIAG-Altpfad ist
 * Benutzerdefiniert). monitor_grain/monitor_glitch bleiben eigene Werte.
 *
 * W266 PRESET-EDGE (Nutzer 14.11.): die Breitbild-Raender gehoeren zum Preset,
 * in allen fuenf Spalten "Ausblenden" mit 8 % Breite / 100 % Staerke
 * (edge_mode=2, edge_fade_width=8, edge_fade_strength=100 = Spieler-Standard
 * in sb_player.cfg und Launcher). Die Maske wirkt nur ausserhalb der 4:3-Box
 * (Hor+-Rand der 3D-Szene); das Bild in der Box bleibt unveraendert. Die
 * Test-cfg (sb_native.cfg) und hcfg.c behalten 0/12/100, damit Golden
 * bitgleich bleibt - ein Testlauf zeigt im ESC-Menue daher "Benutzerdefiniert".
 *
 * W278 ORIG-FLAG (Nutzer 15.11.): Standard ist 4:3 mit schwarzen Balken
 * (edge_mode=1 = harter Schnitt an der 4:3-Box, nie gestreckt) in ALLEN
 * Spalten - ein Presetwechsel aendert weiterhin nie den Bildausschnitt.
 * Breitbild (Hor+-Rand) ist eine eigene, experimentelle Wahl auf der Seite
 * Anzeige; deren Standard "Ausblenden 8 %/100 %" (W233/W266) steht weiter in
 * edge_fade_width/-strength. Einordnung Original/Experimental: sb_origflag.h.
 * Experimentell sind die Spalten 1, 2 (Remastered) und 4 (Klare Sicht);
 * 0 (Original) und 3 (Performance: nur Bildrate/Aufloesung) sind original.
 *
 * W296 PRESET-ORIG (Nutzer 15.11.): Es gibt nur ein "Original" (Name ohne
 * Bildrate); es setzt interp_fps = -1 = Monitor (auto, W223). Spalte 3 heisst
 * "Performance" (Werte gleich). Alte Dateien (ohne gfx_preset_rev) mit
 * Original-Werten und 120 + fs_interp_match=1 waren das bisherige Original:
 * sie werden beim Laden einmal auf auto umgestellt (Launcher
 * lcfg_gfx_migrate_fps, Client sb_gfx_migrate_fps). Wer die Datei danach
 * speichert, schreibt gfx_preset_rev=SB_GP_REV; eine dann bewusst gewaehlte
 * feste Rate (auch 120) bleibt so fuer immer erhalten.
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

#define SB_GP_NPRE 5 /* Original, dezent, stark, Performance, Klare Sicht */
#define SB_GP_PERFORMANCE 3
#define SB_GP_CLEAR 4 /* W252 CLEAR-VIEW */

typedef struct {
    const char *key; /* cfg-Schluessel (hcfg.c / user.cfg) */
    int v[SB_GP_NPRE];
} sb_gp_key_t;

static const sb_gp_key_t kSbGfxPreset[] = {
    /* Shader (W121/W131, Reihenfolge wie launcher_cfg.c kShxKey) */
    { "shading",          {   1,   1,   1,   1,   0 } },
    { "world_fog",        { 100, 100, 100, 100, 100 } },
    { "world_contrast",   { 100, 100, 110, 100, 100 } },
    { "world_gamma",      { 100, 100, 100, 100, 100 } },
    { "world_sharpen",    {   0,  60,  60,   0,   0 } },
    { "bloom_strength",   { 100, 100, 120, 100, 100 } },
    { "cockpit_glow",     {   0,   0,   0,   0,   0 } },
    { "cockpit_spill",    {   0,   0,   0,   0,   0 } },
    { "cockpit_light",    { 100, 100, 100, 100, 100 } },
    { "world_exposure",   { 100, 100, 100, 100, 100 } },
    { "tonemap",          {   0,   0,   1,   0,   0 } },
    { "bloom_threshold",  {   0,   0,  20,   0,   0 } },
    { "light_world",      { 100, 100, 100, 100, 100 } },
    { "light_cockpit",    { 100, 100, 100, 100, 100 } },
    { "world_focus",      { 100, 100,  50, 100, 100 } },
    { "aa_mode",          {   0,   3,   3,   0,   0 } },
    { "ssao",             {   0,  25,  60,   0,   0 } },
    { "color_sat",        { 100, 105, 115, 100, 100 } },
    { "color_temp",       {   0,   0,  10,   0,   0 } },
    { "color_lift",       {   0,   0,   0,   0,   0 } },
    { "color_gamma",      { 100, 100, 100, 100, 100 } },
    { "color_gain",       { 100, 103, 110, 100, 100 } },
    { "shade_shadow",     {   0,   1,   1,   0,   0 } },
    { "shadow_soft",      {   0,   0,   1,   0,   0 } },
    /* Effekte (W133, ESC-Seite "Effekte"; fx_interp: 0/1) */
    { "fx_soft",          {   0,  60, 100,   0,   0 } },
    { "fx_embers",        {   0,  50, 120,   0,   0 } },
    { "fx_light",         {   0,  40, 100,   0,   0 } },
    { "fx_light_cockpit", {   0,  30,  80,   0,   0 } },
    { "fx_interp",        {   0,   1,   1,   0,   0 } },
    /* HD-Texturen aus Packs */
    { "hd_textures",      {   0,   1,   1,   0,   0 } },
    /* W207: selection owns rate/scaling; existing cfg files stay readable. */
    /* W296 PRESET-ORIG: Original = Monitor (auto, -1 = SB_FPS_AUTO aus sb_fpsrate.h) */
    { "interp_fps",      {  -1, 120, 120,  60, 120 } },
    { "rt_scale",        {   0,   0,   0,   1,   0 } },
    { "rt_scale_dyn",    {   0,   0,   0,   0,   0 } },
    { "fs_interp_match", {   1,   1,   1,   0,   1 } },
    /* W252 CLEAR-VIEW: Monitor-Effekte (Korn/Scanlines/Stoerung) */
    { "monitor_fx",      {   1,   1,   1,   1,   0 } },
    /* W266 PRESET-EDGE: Breitbild-Raender; W278: Modus 1 = 4:3 (Breite/Staerke fuer Modus 2) */
    { "edge_mode",          {   1,   1,   1,   1,   1 } }, /* W278: 4:3 mit Balken */
    { "edge_fade_width",    {   8,   8,   8,   8,   8 } },
    { "edge_fade_strength", { 100, 100, 100, 100, 100 } },
};
#define SB_GP_NKEY ((int)(sizeof kSbGfxPreset / sizeof kSbGfxPreset[0]))
#define SB_GP_FX0 24 /* erster Effekt-Schluessel (fx_soft) */
#define SB_GP_NFX 5
#define SB_GP_HD 29  /* hd_textures */
#define SB_GP_FPS 30
#define SB_GP_RT 31
#define SB_GP_DYN 32
#define SB_GP_MATCH 33
#define SB_GP_MFX 34 /* W252 monitor_fx */
#define SB_GP_EDGE 35 /* W266 edge_mode, edge_fade_width, edge_fade_strength (35..37) */
#define SB_GP_NEDGE 3

/* W296 PRESET-ORIG: Stand der Preset-Tabelle in user.cfg ([render]
 * gfx_preset_rev). 0/fehlt = vor W296 (Original = 120 FPS), 1 = Original =
 * Monitor (auto). Nur fuer die einmalige Umstellung alter Dateien. */
#define SB_GP_REV 1
#define SB_GP_REV_KEY "gfx_preset_rev"
#define SB_GP_LEGACY_ORIG_FPS 120 /* Bildrate des alten "Original - 120 FPS" */

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
