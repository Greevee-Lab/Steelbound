#pragma once
/* sb_origflag.h - W278 ORIG-FLAG: Einordnung aller Spieler-Einstellungen in
 * "Original" (neutral) und "Experimental" (weicht vom Original-Bild ab).
 *
 * EINZIGE Quelle fuer Client (sb_origflag.c, ESC-Menue sb_overlay.c) und
 * Launcher (launcher_orig.c): Warnhinweis, "Back to original", Einmal-Start
 * im Original-Modus, Live-Umschalttaste im Spiel und die Markierungen.
 *
 * Experimental (Nutzer 15.11.2026): Shader, Licht und Nachbearbeitung (alles
 * aus sb_gfxpreset.h ausser Bildrate und Zwischenbildern), Monitor-Effekt aus
 * bzw. "Clear view", HD-Texturpacks und Effect Packs, Breitbild (Hor+,
 * Randmodi, Cockpit-Perspektive/-Breite, Panorama), Cockpit ausblenden.
 * Dazu (Einordnung W278): HUD-Etiketten weichen aus (hud_label_avoid) und die
 * Spass-Effekte Drahtgitter/Vaporwave (W240).
 *
 * Neutral, NIE gekennzeichnet (stehen hier nur zur Doku, s. kSbOrigNeutral):
 * Bildrate (interp_fps, fps_limit, fs_interp_match) und Zwischenbild-Optionen,
 * Fenster-/Vollbildmodus, Aufloesung und Render-Skalierung (rt_scale,
 * rt_scale_dyn), Steuerung (aim_assist, auto_gear, stick_aim, Maus), Ton,
 * VR (eigene Seite), Spieltakt (game_tick), Texturdetail hd_scale (wirkt nur
 * mit hd_textures), Fotomodus.
 *
 * Spalte orig = Wert des Original-Bildes. Breitbild: edge_mode = 1 (harter
 * Schnitt = 4:3 mit schwarzen Balken, nie gestreckt). edge_fade_width/-strength
 * zaehlen nicht (wirken nur bei edge_mode = 2), werden aber beim Zuruecksetzen
 * auf den Breitbild-Standard 8 %/100 % gestellt (det = 0).
 *
 * hd_textures steht fuer "HD-Texturen" (wirksam als remaster && hd_textures);
 * Effect Packs sind kein Schluessel: Client und Launcher melden sie als
 * Gruppe SB_OFG_PACKS, sobald ein aktives Effect Pack Werte setzt. Packs
 * lassen sich im Spiel nicht live wechseln (die Umschalttaste laesst sie
 * weg; der Einmal-Start leert pack_list).
 *
 * live = 1: Wert wirkt sofort im Spiel (Umschalttaste). 0 = erst nach
 * Neustart (z. B. ssao braucht die BB-Tiefe vom Start) - die Taste setzt ihn
 * trotzdem, die Einblendung nennt nur, was sofort wirkt.
 * ASCII only. */

enum {
    SB_OFG_LIGHT = 0, /* Licht und Shader (shading, Welt-, Cockpit-Licht, Schatten) */
    SB_OFG_POST,      /* Nachbearbeitung (Kantenglaettung, AO, Farbe, Schaerfe) */
    SB_OFG_FX,        /* verbesserte Effekte (fx_*) und Spass-Effekte */
    SB_OFG_MONITOR,   /* Monitor-Effekt aus/vermindert ("Clear view") */
    SB_OFG_HD,        /* HD-Texturen aus Packs */
    SB_OFG_PACKS,     /* Effect Packs (kein Schluessel) */
    SB_OFG_WIDE,      /* Breitbild: Hor+-Rand, Cockpit-Perspektive/-Breite, Panorama */
    SB_OFG_COCKPIT,   /* Cockpit ausblenden */
    SB_OFG_HUD,       /* HUD-Etiketten weichen aus */
    SB_OFG_N
};

typedef struct {
    const char *key; /* cfg-Schluessel (hcfg.c / user.cfg) */
    int orig;        /* Wert des Original-Bildes */
    unsigned char grp;  /* SB_OFG_* */
    unsigned char det;  /* 1 = Abweichung zaehlt als experimentell */
    unsigned char live; /* 1 = wirkt im Spiel sofort */
} sb_of_key_t;

static const sb_of_key_t kSbOrigFlag[] = {
    /* Licht und Shader (sb_gfxpreset.h, Shader-Teil) */
    { "shading",            1, SB_OFG_LIGHT, 1, 1 },
    { "world_fog",        100, SB_OFG_LIGHT, 1, 1 },
    { "world_contrast",   100, SB_OFG_LIGHT, 1, 1 },
    { "world_gamma",      100, SB_OFG_LIGHT, 1, 1 },
    { "bloom_strength",   100, SB_OFG_LIGHT, 1, 1 },
    { "cockpit_glow",       0, SB_OFG_LIGHT, 1, 1 },
    { "cockpit_spill",      0, SB_OFG_LIGHT, 1, 1 },
    { "cockpit_light",    100, SB_OFG_LIGHT, 1, 1 },
    { "world_exposure",   100, SB_OFG_LIGHT, 1, 1 },
    { "tonemap",            0, SB_OFG_LIGHT, 1, 1 },
    { "bloom_threshold",    0, SB_OFG_LIGHT, 1, 1 },
    { "light_world",      100, SB_OFG_LIGHT, 1, 1 },
    { "light_cockpit",    100, SB_OFG_LIGHT, 1, 1 },
    { "world_focus",      100, SB_OFG_LIGHT, 1, 1 },
    { "shade_shadow",       0, SB_OFG_LIGHT, 1, 1 },
    { "shadow_soft",        0, SB_OFG_LIGHT, 1, 1 },
    /* Nachbearbeitung */
    { "world_sharpen",      0, SB_OFG_POST, 1, 1 },
    { "aa_mode",            0, SB_OFG_POST, 1, 1 },
    { "ssao",               0, SB_OFG_POST, 1, 0 },
    { "color_sat",        100, SB_OFG_POST, 1, 1 },
    { "color_temp",         0, SB_OFG_POST, 1, 1 },
    { "color_lift",         0, SB_OFG_POST, 1, 1 },
    { "color_gamma",      100, SB_OFG_POST, 1, 1 },
    { "color_gain",       100, SB_OFG_POST, 1, 1 },
    /* Effekte (W133) und Spass-Effekte (W240) */
    { "fx_soft",            0, SB_OFG_FX, 1, 1 },
    { "fx_embers",          0, SB_OFG_FX, 1, 1 },
    { "fx_light",           0, SB_OFG_FX, 1, 1 },
    { "fx_light_cockpit",   0, SB_OFG_FX, 1, 1 },
    { "fx_interp",          0, SB_OFG_FX, 1, 1 },
    { "fx_wireframe",       0, SB_OFG_FX, 1, 1 },
    { "fx_vaporwave",       0, SB_OFG_FX, 1, 1 },
    /* Monitor-Effekt ("Clear view" = monitor_fx 0) */
    { "monitor_fx",         1, SB_OFG_MONITOR, 1, 1 },
    { "monitor_grain",    100, SB_OFG_MONITOR, 1, 1 },
    { "monitor_glitch",   100, SB_OFG_MONITOR, 1, 1 },
    /* HD-Texturen (wirksam: remaster && hd_textures) */
    { "hd_textures",        0, SB_OFG_HD, 1, 1 },
    /* Breitbild: 4:3 mit Balken = Original */
    { "edge_mode",          1, SB_OFG_WIDE, 1, 1 },
    { "edge_fade_width",    8, SB_OFG_WIDE, 0, 1 },
    { "edge_fade_strength", 100, SB_OFG_WIDE, 0, 1 },
    { "cockpit_persp",      0, SB_OFG_WIDE, 1, 1 },
    { "cockpit_panini",     0, SB_OFG_WIDE, 1, 1 },
    { "cockpit_width",      0, SB_OFG_WIDE, 1, 1 },
    /* Cockpit ausblenden, HUD */
    { "cockpit_hide",       0, SB_OFG_COCKPIT, 1, 1 },
    { "monitor_view",       0, SB_OFG_COCKPIT, 1, 1 }, /* W319 MONITOR-VIEW */
    { "hud_label_avoid",    0, SB_OFG_HUD, 1, 0 }, /* Patch beim Start */
};
#define SB_OF_NKEY ((int)(sizeof kSbOrigFlag / sizeof kSbOrigFlag[0]))

/* Doku/Selbsttest: neutrale Schluessel (duerfen NIE in kSbOrigFlag stehen). */
static const char *const kSbOrigNeutral[] = {
    "interp_fps", "fps_limit", "fs_interp_match", "present_interval", "rt_scale", "rt_scale_dyn",
    "window_mode", "fs_hz", "width", "height", "aspect_mode", "hd_scale", "interp_hlerp", "interp_p2hom", "interp_guard",
    "mouse_mode", "mouse_sens", "aim_assist", "auto_gear", "stick_aim", "stick_aim_sens", "sbc_usb",
    "audio", "audio_live", "game_tick", "vr_enable", "vr_preset", "save_lock", "photo_scale",
};
#define SB_OF_NNEUTRAL ((int)(sizeof kSbOrigNeutral / sizeof kSbOrigNeutral[0]))

/* W424 (Einordnung W278): Gameplay-Abweichungen mit Standard AN. Sie aendern
 * das Spielgeschehen, nicht das Bild, und werden deshalb NIE als Experimental
 * gekennzeichnet, nie von "Zurueck zum Original"/Einmal-Start/Umschalttaste
 * gesetzt (duerfen nicht in kSbOrigFlag stehen). Original-Wert in Klammern:
 *   sb_allied_squads (0) - verbuendete Trupps kaempfen auch fern vom Spieler
 *                          (Nutzerentscheid 22.11.2026, nur Steel Battalion). */
static const char *const kSbOrigGameplay[] = { "sb_allied_squads" };
#define SB_OF_NGAMEPLAY ((int)(sizeof kSbOrigGameplay / sizeof kSbOrigGameplay[0]))

/* Sprachschluessel je Gruppe (Liste im Hinweis, kurz, klein geschrieben). */
static const char *const kSbOrigGroupKey[SB_OFG_N] = {
    "ovl.of_g_light", "ovl.of_g_post", "ovl.of_g_fx", "ovl.of_g_monitor", "ovl.of_g_hd",
    "ovl.of_g_packs", "ovl.of_g_wide", "ovl.of_g_cockpit", "ovl.of_g_hud"
};

/* Reihenfolge im Hinweis: was am haeufigsten Artefakte macht, zuerst. */
static const unsigned char kSbOrigGroupOrder[SB_OFG_N] = {
    SB_OFG_HD, SB_OFG_PACKS, SB_OFG_WIDE, SB_OFG_LIGHT, SB_OFG_POST, SB_OFG_FX,
    SB_OFG_MONITOR, SB_OFG_COCKPIT, SB_OFG_HUD
};

/* Index des Schluessels, -1 = nicht experimentell (neutral oder unbekannt). */
static int sb_of_find(const char *key)
{
    int i;
    for (i = 0; i < SB_OF_NKEY; i++) {
        const char *a = kSbOrigFlag[i].key, *b = key;
        while (*a && *a == *b) { a++; b++; }
        if (*a == *b) { return i; }
    }
    return -1;
}

/* Weicht Wert v des Schluessels i vom Original ab (und zaehlt)? fx_interp
 * ist ein Schalter (jeder Wert != 0 = an, wie sb_gfxpreset.h). */
static int sb_of_dev(int i, int v)
{
    if (i < 0 || i >= SB_OF_NKEY || !kSbOrigFlag[i].det) { return 0; }
    if (i == sb_of_find("fx_interp")) { return (v != 0) != (kSbOrigFlag[i].orig != 0); }
    return v != kSbOrigFlag[i].orig;
}
