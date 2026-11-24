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
 * Nutzerwunsch 14.11., besser zu sehen, nicht die Original-Optik),
 * 5 = Enhanced (W465, nur Line of Contact, s. sb_gp_visible),
 * 6 = Enhanced Vivid (W478, nur Line of Contact: Enhanced mit den Farb-/Kontrastwerten von vor W471).
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

#define SB_GP_NPRE 7 /* Original, dezent, stark, Performance, Klare Sicht, Enhanced (W465, nur LoC), Vivid (W478, nur LoC) */
#define SB_GP_PERFORMANCE 3
#define SB_GP_CLEAR 4 /* W252 CLEAR-VIEW */
#define SB_GP_ENHANCED 5 /* W465 LOC-ENHANCED */
/* W478 LOC-ENHANCED-VIVID (Nutzer 24.11.): "Enhanced Vivid" = Enhanced mit kraeftigeren Farben und mehr Kontrast,
 * nur Line of Contact, nur waehlbar (Standard bleibt Enhanced, keine Migration). Werte = Enhanced bis W471
 * (gfx_loc_rev 2, kSbGpEnhRev2 unten): Kontrast 110, Saettigung 110, Farbtemperatur 6, Gain 105, Bloom 150,
 * shadow_sun 140; alles andere (Schatten, weiche Kante, Cockpit-Licht, FXAA, Effekte) wie Enhanced. Kein
 * Experimental-Hinweis (Enhanced-Variante). Die Einmal-Umstellung alter Dateien (Marke < SB_GP_LOC_REV) macht aus
 * genau diesen Werten weiter Enhanced - eine Vivid-Wahl traegt immer schon die aktuelle Marke. */
#define SB_GP_VIVID 6
#define SB_GP_SUN_REV2 140 /* W471: shadow_sun bis W471 (hcfg-Standard, nicht in der Datei); W478: Vivid */

/* W465 LOC-ENHANCED (Nutzer 24.11.): sichtbare Presets je Spiel. Steel Battalion: Original, Performance,
 * Klare Sicht (+ Benutzerdefiniert); "Remastered dezent/stark" (Spalten 1/2) sind nur aus der Auswahl
 * genommen, bis das Remaster richtig umgesetzt ist - Werte und Code bleiben. Line of Contact: Original und
 * Enhanced (Standard). Enhanced = SB-Effekte auf LoC (Cockpit-Leuchten/-Licht, Farbe, FXAA, SB-Bloom 150 %
 * inkl. Bloom der Lock-On-Kamera, Licht bei Feuer/Explosionen; render_shfx.c); kein Experimental-Hinweis
 * (Nutzerwunsch: Standard). Alte Werte: SB Remastered -> Original, LoC Performance/Klare Sicht/ohne Marke ->
 * Enhanced (Launcher, Marke gfx_loc_rev in user_loc.cfg; Client: LoC-Spieler-cfg ohne Marke = Enhanced). */
#define SB_GP_LOC_REV 3 /* W466: 2 = Enhanced mit Schatten (Rev 1 = W465-Enhanced ohne Schatten); W471: 3 = Original mit
                         * den Schatten des Spiels, Enhanced auf SB-Original-Optik (gedeckter, Kontrast 100) */
#define SB_GP_LOC_REV_KEY "gfx_loc_rev"
static int sb_gp_visible(int preset, int loc)
{
    if (loc) { return preset == 0 || preset == SB_GP_ENHANCED || preset == SB_GP_VIVID; } /* W478: + Vivid */
    return preset == 0 || preset == SB_GP_PERFORMANCE || preset == SB_GP_CLEAR;
}

typedef struct {
    const char *key; /* cfg-Schluessel (hcfg.c / user.cfg) */
    int v[SB_GP_NPRE];
} sb_gp_key_t;

static const sb_gp_key_t kSbGfxPreset[] = {
    /* Shader (W121/W131, Reihenfolge wie launcher_cfg.c kShxKey) */
    { "shading",          {   1,   1,   1,   1,   0,    1,    1 } },
    { "world_fog",        { 100, 100, 100, 100, 100,  100,  100 } },
    { "world_contrast",   { 100, 100, 110, 100, 100,  100,  110 } }, /* W471: Enhanced 100 (Nutzer: "etwas zu krass") */
    { "world_gamma",      { 100, 100, 100, 100, 100,  100,  100 } },
    { "world_sharpen",    {   0,  60,  60,   0,   0,   40,   40 } },
    { "bloom_strength",   { 100, 100, 120, 100, 100,  130,  150 } }, /* W471: Enhanced 130 (vorher 150) */
    { "cockpit_glow",     {   0,   0,   0,   0,   0,  100,  100 } },
    { "cockpit_spill",    {   0,   0,   0,   0,   0,   70,   70 } },
    { "cockpit_light",    { 100, 100, 100, 100, 100,   80,   80 } },
    { "world_exposure",   { 100, 100, 100, 100, 100,  100,  100 } },
    { "tonemap",          {   0,   0,   1,   0,   0,    0,    0 } },
    { "bloom_threshold",  {   0,   0,  20,   0,   0,   20,   20 } },
    { "light_world",      { 100, 100, 100, 100, 100,  100,  100 } },
    { "light_cockpit",    { 100, 100, 100, 100, 100,  100,  100 } },
    { "world_focus",      { 100, 100,  50, 100, 100,  100,  100 } },
    { "aa_mode",          {   0,   3,   3,   0,   0,    1,    1 } },
    { "ssao",             {   0,  25,  60,   0,   0,    0,    0 } },
    { "color_sat",        { 100, 105, 115, 100, 100,   95,  110 } }, /* W471: Enhanced gedeckt (SB-Original-Optik) */
    { "color_temp",       {   0,   0,  10,   0,   0,    0,    6 } }, /* W471: Enhanced neutral */
    { "color_lift",       {   0,   0,   0,   0,   0,    0,    0 } },
    { "color_gamma",      { 100, 100, 100, 100, 100,  100,  100 } },
    { "color_gain",       { 100, 103, 110, 100, 100,  100,  105 } }, /* W471: Enhanced ohne Aufhellung */
    { "shade_shadow",     {   0,   1,   1,   0,   0,    1,    1 } }, /* W466: LoC Enhanced = Schatten des Spiels */
    { "shadow_soft",      {   0,   0,   1,   0,   0,    1,    1 } },
    { "shadow_sun",       {   0,   0,   0,   0,   0,  115, SB_GP_SUN_REV2 } }, /* W471: Sonne/Schatten (W466, nur LoC mit Schatten); W478: Vivid 140 */
    /* Effekte (W133, ESC-Seite "Effekte"; fx_interp: 0/1) */
    { "fx_soft",          {   0,  60, 100,   0,   0,    0,    0 } },
    { "fx_embers",        {   0,  50, 120,   0,   0,    0,    0 } },
    { "fx_light",         {   0,  40, 100,   0,   0,    0,    0 } },
    { "fx_light_cockpit", {   0,  30,  80,   0,   0,  150,  150 } },
    { "fx_interp",        {   0,   1,   1,   0,   0,    0,    0 } },
    /* HD-Texturen aus Packs */
    { "hd_textures",      {   0,   1,   1,   0,   0,    0,    0 } },
    /* W207: selection owns rate/scaling; existing cfg files stay readable. */
    /* W296 PRESET-ORIG: Original = Monitor (auto, -1 = SB_FPS_AUTO aus sb_fpsrate.h) */
    { "interp_fps",      {  -1, 120, 120,  60, 120,   -1,   -1 } },
    { "rt_scale",        {   0,   0,   0,   1,   0,    0,    0 } },
    { "rt_scale_dyn",    {   0,   0,   0,   0,   0,    0,    0 } },
    { "fs_interp_match", {   1,   1,   1,   0,   1,    1,    1 } },
    /* W252 CLEAR-VIEW: Monitor-Effekte (Korn/Scanlines/Stoerung) */
    { "monitor_fx",      {   1,   1,   1,   1,   0,    1,    1 } },
    /* W266 PRESET-EDGE: Breitbild-Raender; W278: Modus 1 = 4:3 (Breite/Staerke fuer Modus 2) */
    { "edge_mode",          {   1,   1,   1,   1,   1,    1,    1 } }, /* W278: 4:3 mit Balken */
    { "edge_fade_width",    {   8,   8,   8,   8,   8,    8,    8 } },
    { "edge_fade_strength", { 100, 100, 100, 100, 100,  100,  100 } },
};
#define SB_GP_NKEY ((int)(sizeof kSbGfxPreset / sizeof kSbGfxPreset[0]))
#define SB_GP_FX0 25 /* erster Effekt-Schluessel (fx_soft) */
#define SB_GP_NFX 5
#define SB_GP_HD 30  /* hd_textures */
#define SB_GP_FPS 31
#define SB_GP_RT 32
#define SB_GP_DYN 33
#define SB_GP_MATCH 34
#define SB_GP_MFX 35 /* W252 monitor_fx */
#define SB_GP_EDGE 36 /* W266 edge_mode, edge_fade_width, edge_fade_strength (36..38) */
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

/* W471 LOC-SHADOW-ORIG (Nutzer 24.11.): "Was im Originalspiel drin ist, gehoert ins Preset Original." Line of
 * Contact zeichnet auf der Xbox selbst einen Schattenpuffer und vergleicht ihn im Welt-PS (W466); unser Client
 * hat ihn bis W471 nur in Enhanced ausgewertet. In LoC ist Original darum Spalte 0 PLUS shade_shadow = 1 (harter
 * Vergleich = bilinearer 2x2-PCF wie die NV2A-Hardware, shadow_soft 0, shadow_sun 0). Steel Battalion bleibt
 * Spalte 0 unveraendert (Golden). Performance (Spalte 3 = Original mit anderer Bildrate, in LoC nicht waehlbar)
 * ebenso. Wert der Zeile row im Preset q fuer das Spiel (loc = Line of Contact). */
#define SB_GP_SHADOW 22 /* Zeile shade_shadow */
static int sb_gp_val(int row, int q, int loc)
{
    if (loc && (q == 0 || q == SB_GP_PERFORMANCE) && row == SB_GP_SHADOW) { return 1; } /* Performance = Original + Rate */
    return kSbGfxPreset[row].v[q];
}

/* W471: Enhanced bis W471 (gfx_loc_rev 2; Rev 1 = dasselbe mit shade_shadow/shadow_soft 0): Werte, die von der
 * heutigen Spalte SB_GP_ENHANCED abweichen. Nur fuer die Einmal-Umstellung (Client sb_gfx_loc_default, Launcher
 * lgame_loc_gfx_migrate): genau diese Optik -> neues Enhanced; Original (Spalte 0 ohne Schatten) -> Original mit
 * Schatten; eigene Werte bleiben (fehlt shadow_sun, wird der bis W471 wirksame Wert 140 eingetragen). */
typedef struct { const char *key; int v; } sb_gp_kv_t;
static const sb_gp_kv_t kSbGpEnhRev2[] = {
    { "world_contrast", 110 }, { "color_sat", 110 }, { "color_temp", 6 }, { "color_gain", 105 },
    { "bloom_strength", 150 }, { "shadow_sun", SB_GP_SUN_REV2 },
};
#define SB_GP_NENHREV2 ((int)(sizeof kSbGpEnhRev2 / sizeof kSbGpEnhRev2[0]))
/* Wert der Zeile row im Enhanced bis W471 (rev 2; rev 1: Schatten 0/0). */
static int sb_gp_enh_old(int row, int rev)
{
    int i;
    const char *k = kSbGfxPreset[row].key;
    if (rev == 1 && (row == SB_GP_SHADOW || row == SB_GP_SHADOW + 1)) { return 0; }
    for (i = 0; i < SB_GP_NENHREV2; i++) {
        const char *a = kSbGpEnhRev2[i].key, *b = k;
        while (*a && *a == *b) { a++; b++; }
        if (*a == *b) { return kSbGpEnhRev2[i].v; }
    }
    return kSbGfxPreset[row].v[SB_GP_ENHANCED];
}
#define SB_GP_SUN 24 /* Zeile shadow_sun */

/* W478: Selbsttest-Hilfe - Spalte Vivid = Enhanced bis W471 (Rev 2) in allen Zeilen? -1 = ja, sonst erste
 * abweichende Zeile. Haelt die beiden Quellen (Tabelle, kSbGpEnhRev2) zusammen. */
static int sb_gp_vivid_check(void)
{
    int r;
    for (r = 0; r < SB_GP_NKEY; r++) {
        if (kSbGfxPreset[r].v[SB_GP_VIVID] != sb_gp_enh_old(r, 2)) { return r; }
    }
    return -1;
}
