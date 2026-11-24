/* sb_ovl_loclock.h - W416 LOC-ESC-MENU: ESC-Einstellungen, die in Line of Contact
 * nichts bewirken (reine Logik, ohne Abhaengigkeiten; auch render_selftest).
 *
 * Schluessel = cfg-Schluessel der ESC-Zeile (erster Eintrag der Heimat-Liste,
 * sb_overlay.c ov_loc_key). Rueckgabe = Sprachschluessel des Grundes
 * (ovl.ll_*) oder NULL = bedienbar. Gleiche Gruende wie der Launcher
 * (launcher_gui.c kLocLock/loc_lock_key, W402): Cockpit, Monitor-Effekte,
 * verbuendete Trupps (W424; Zielhilfe/Automatik seit W440 frei mit Hinweis, sb_ovl_loc_hint), Replay, Texturpakete, VR, Spielstandschutz, Raender bei
 * 16:9. Dazu ESC-eigen "post": die Endbild-Nachbearbeitung (sb_shfx_post)
 * prueft die SB-Missionsszene (sfx_mission: SBA(0x6B96DC) == 0x2C), die im
 * LoC-Profil auf der Null-Seite liegt - Cockpit-Leuchten/-Licht, Farbe, SSAO
 * laufen dort nie. Tabelle mit Belegen: LOC_BOOT.md "W416".
 *
 * loc = 0 (Steel Battalion): immer NULL, nichts gesperrt. wide169 = LoC mit
 * Bildformat 16:9 bzw. 720p (loc_widescreen != 0). */
#ifndef SB_OVL_LOCLOCK_H
#define SB_OVL_LOCLOCK_H

#include <string.h>

static __inline int sb_ovl_ll_is(const char *key, const char *const *list)
{
    for (; *list; list++) {
        if (strcmp(key, *list) == 0) { return 1; }
    }
    return 0;
}

static __inline const char *sb_ovl_loc_lock(const char *key, int loc, int wide169)
{
    static const char *const kCockpit[] = { "cockpit_persp", "cockpit_panini", "cockpit_width", "cockpit_hide",
                                            "monitor_view", "hud_label_avoid", NULL };
    static const char *const kMonFx[] = { "monitor_fx", "monitor_grain", "monitor_glitch", NULL };
    static const char *const kAssist[] = { "aim_turn", "sb_allied_squads", NULL }; /* + W424; W440: aim_assist/auto_gear frei */
    static const char *const kReplay[] = { "replay_label", "replay_logo", "watermark_media", "replay_fx_live", NULL };
    static const char *const kPacks[] = { "remaster", "hd_textures", "hd_scale", NULL };
    static const char *const kSave[] = { "save_lock", NULL };
    static const char *const kEdge[] = { "edge_mode", "edge_fade_width", "edge_fade_strength", NULL };
    /* W465 LOC-ENHANCED: Cockpit-Leuchten/-Licht und Farbe laufen in LoC (eigene Szenenerkennung,
     * loc_render_post_scene); nur SSAO bleibt gesperrt (LoC-Tiefe nicht geprueft). */
    static const char *const kPost[] = { "ssao", NULL };
    if (!loc || !key || !key[0]) { return NULL; }
    if (sb_ovl_ll_is(key, kCockpit)) { return "ovl.ll_cockpit"; }
    if (sb_ovl_ll_is(key, kMonFx)) { return "ovl.ll_monfx"; }
    if (sb_ovl_ll_is(key, kAssist)) { return "ovl.ll_assist"; }
    if (sb_ovl_ll_is(key, kReplay)) { return "ovl.ll_replay"; }
    if (sb_ovl_ll_is(key, kPacks)) { return "ovl.ll_packs"; }
    if (strncmp(key, "vr_", 3) == 0) { return "ovl.ll_vr"; }
    if (sb_ovl_ll_is(key, kSave)) { return "ovl.ll_savelock"; }
    if (sb_ovl_ll_is(key, kPost)) { return "ovl.ll_post"; }
    if (wide169 && sb_ovl_ll_is(key, kEdge)) { return "ovl.ll_edge"; }
    return NULL;
}

/* W440 LOC-LOBBY-MODES: bedienbare Zeilen mit Hinweis in LoC (nicht gesperrt). Zielhilfe und Gang-Automatik wirken
 * nur in Arcade-Lobbys; Sim-Lobbys schalten sie fuer das Match ab (loc_assist.h). NULL = kein Hinweis. */
static __inline const char *sb_ovl_loc_hint(const char *key, int loc)
{
    if (!loc || !key) { return NULL; }
    if (strcmp(key, "aim_assist") == 0 || strcmp(key, "auto_gear") == 0) { return "ovl.ll_arcade"; }
    return NULL;
}

/* Kurzer Wert in der gesperrten Zeile (statt des Werts): "nur Steel Battalion" bzw. "fest (16:9)". */
static __inline const char *sb_ovl_loc_lock_tag(const char *reason)
{
    if (!reason) { return NULL; }
    return strcmp(reason, "ovl.ll_edge") == 0 ? "ovl.ll_tag169" : "ovl.ll_tag";
}

#endif
