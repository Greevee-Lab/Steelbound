/* launcher_gui_test.c - GUI-Selbsttest gui_selftest + Faelle W282 ff..
 * W462 LAUNCHER-SPLIT: aus launcher_gui.c verschoben (unveraendert), gemeinsamer Zustand
 * in launcher_gui_int.h. */
#include "launcher_gui_int.h"
void gui_selftest(const char *client_override, int *pass, int *fail)
{
    char tmp[1024], dir[1100], p[1300], err[1024];
    /* Ordnername mit Umlauten und sz: "w82_uemlaeut_ss" */
    const char *uml = "w82_" U8_UE "ml" U8_AE "ut_" U8_SZ;
    u8_GetTempPath(tmp, sizeof tmp);
    g_locgate_skip = 1; /* W400: aeltere LoC-Startweg-Faelle pruefen die Kette ohne steelnet (Fall 65d prueft die Sperre) */
    g_in_gui_selftest = 1; /* W403 */

    /* 20: UTF-8-Pfade (Datei, Suche, Profile in Umlaut-Ordner) */
    {
        int ok = 1;
        char names[8][64];
        launcher_settings_t a, b;
        u8_find_t fd;
        _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s%s", tmp, uml);
        lcfg_mkdir_p(dir);
        _snprintf_s(p, sizeof p, _TRUNCATE, "%s/datei_%s.txt", dir, uml);
        write_file(p, "x");
        if (u8_GetFileAttributes(p) == INVALID_FILE_ATTRIBUTES) {
            ok = 0;
        }
        _snprintf_s(p, sizeof p, _TRUNCATE, "%s/datei_*.txt", dir);
        if (!u8_find_first(p, &fd)) {
            ok = 0;
        } else {
            char want[200];
            _snprintf_s(want, sizeof want, _TRUNCATE, "datei_%s.txt", uml);
            if (strcmp(fd.name, want) != 0) {
                ok = 0;
            }
            u8_find_close(&fd);
        }
        lcfg_defaults(&a);
        a.width = 1600;
        a.height = 900;
        if (lcfg_profile_save(dir, "u8test", &a, err, sizeof err) != 0 ||
            lcfg_profile_load(dir, "u8test", &b, err, sizeof err) != 0 ||
            memcmp(&a, &b, sizeof a) != 0 ||
            lcfg_profile_list(dir, names, 8) != 1) {
            ok = 0;
        }
        lcfg_profile_delete(dir, "u8test", NULL, 0);
        _snprintf_s(p, sizeof p, _TRUNCATE, "%s/datei_%s.txt", dir, uml);
        u8_DeleteFile(p);
        if (ok) {
            GT_OK("20 u8-paths");
        } else {
            GT_FAIL("20 u8-paths", "%s", "Umlaut-Pfad nicht lesbar/schreibbar");
        }
    }

    /* 21: cfg-Kodierung: Pfad mit Umlaut -> ANSI in der Datei, zurueck
     * UTF-8; nicht darstellbares Zeichen -> Kurzname oder Ablehnung. */
    {
        int ok = 1;
        char xbe[1300], cfg[1300], raw[4096];
        launcher_settings_t a, b;
        FILE *f = NULL;
        size_t n = 0;
        _snprintf_s(xbe, sizeof xbe, _TRUNCATE, "%s/default.xbe", dir);
        write_file(xbe, "XBEH");
        _snprintf_s(cfg, sizeof cfg, _TRUNCATE, "%sw82_acp_test.cfg", tmp);
        u8_DeleteFile(cfg);
        lcfg_defaults(&a);
        strcpy_s(a.xbe_path, sizeof a.xbe_path, xbe);
        if (lcfg_save_user(cfg, &a) != 0) {
            ok = 0;
        }
        memset(raw, 0, sizeof raw);
        if (u8_fopen_s(&f, cfg, "rb") == 0 && f) {
            n = fread(raw, 1, sizeof raw - 1, f);
            fclose(f);
        }
        (void)n;
        if (GetACP() == 1252 && !strstr(raw, "w82_\xFCml\xE4ut_\xDF")) {
            ok = 0; /* ANSI-Bytes (cp1252) erwartet, kein UTF-8 */
        }
        lcfg_defaults(&b);
        lcfg_load_file(cfg, &b);
        if (strcmp(a.xbe_path, b.xbe_path) != 0) {
            ok = 0;
        }
        u8_DeleteFile(cfg);
        {
            /* Kyrillisch (nicht in cp1252): Kurzname oder klare Ablehnung */
            char cy[1200], cyx[1300], acp[1200];
            int r;
            _snprintf_s(cy, sizeof cy, _TRUNCATE,
                        "%sw82_\xD0\x96\xD0\xB8\xD0\xBB", tmp);
            lcfg_mkdir_p(cy);
            _snprintf_s(cyx, sizeof cyx, _TRUNCATE, "%s/default.xbe", cy);
            write_file(cyx, "XBEH");
            r = u8_to_acp(cyx, acp, sizeof acp);
            if (r == 1) {
                if (GetFileAttributesA(acp) == INVALID_FILE_ATTRIBUTES) {
                    ok = 0; /* Kurzname muss per A-API oeffnen */
                }
            } else if (r == 2) {
                lcfg_defaults(&b);
                strcpy_s(b.xbe_path, sizeof b.xbe_path, cyx);
                if (lcfg_validate(&b, err, sizeof err) == 0 ||
                    !strstr(err, "Zeichen")) {
                    ok = 0;
                }
            } else if (GetACP() == 1252) {
                ok = 0; /* Kyrillisch kann in cp1252 nicht verlustfrei sein */
            }
            printf("     (21: Kyrillisch -> %s)\n",
                   r == 0 ? "ANSI" : r == 1 ? "8.3-Kurzname" : "abgelehnt");
            u8_DeleteFile(cyx);
            {
                wchar_t w[1200];
                u8_to_w(cy, w, 1200);
                RemoveDirectoryW(w);
            }
        }
        if (ok) {
            GT_OK("21 cfg-ansi");
        } else {
            GT_FAIL("21 cfg-ansi", "%.200s", raw);
        }
    }

    /* 22: Oberflaeche <-> Einstellungen (unsichtbares Fenster, nie gezeigt) */
    {
        launcher_settings_t a, b;
        int ok = 0;
        gui_opts_t o;
        memset(&o, 0, sizeof o);
        static lr_env_t fe; /* W171: feste Umgebung (Monitor, Packs, VR ...) */
        o.shot = 5; /* Test: keine Meldungsfenster */
        g_o = o;
        lrules_env_default(&fe, NULL);
        g_env_fix = &fe;
        g_confirm_auto = 1; /* R14-Rueckfrage: Ja */
        if (gui_init(client_override) && gui_create(1)) {
            build_controls();
            lcfg_defaults(&a);
            _snprintf_s(a.media_root, sizeof a.media_root, _TRUNCATE, "%s",
                        dir);
            _snprintf_s(a.xbe_path, sizeof a.xbe_path, _TRUNCATE,
                        "%s/default.xbe", dir);
            strcpy_s(a.hdd_root, sizeof a.hdd_root, "HDD_" U8_UE);
            a.width = 1920;
            a.height = 1080;
            a.fps_limit = 60;
            a.present_interval = 1;
            a.audio = 0;
            a.audio_live = 1;
            a.interp_fps = 60;
            a.window_mode = 2; /* W81 */
            a.fs_hz = 144;
            a.sbc_usb = 1;
            a.mouse_mode = 0; /* W87: vom Standard abweichend */
            a.mouse_sens = 7;
            a.remaster = 1; /* W103: Benutzerdefiniert */
            a.hd_textures = 1;
            a.hd_scale = 2;
            a.fx_enhanced = 0;
            a.ultrawide = 0; /* W173: Platzhalter schreibt der Launcher immer 0 */
            a.postfx = 0;
            a.monitor_grain = 0; /* W105: "Nur Signalstoerung" */
            a.monitor_glitch = 100;
            a.edge_mode = 2; /* W107: Abdunklung, vom Standard abweichend */
            a.edge_fade_width = 18;
            a.edge_fade_strength = 80;
            strcpy_s(a.input_profile, sizeof a.input_profile,
                     "user_gibts_nicht");
            ui_from_settings(&a);
            lcfg_defaults(&b);
            ui_to_settings(&b);
            { /* W172/P6: Programmdatei = <Ordner>/default.xbe -> Feld leer
               * (automatisch), wirksam bleibt derselbe Pfad */
                char ea[1200], eb[1200], fx[64];
                lcfg_xbe_effective(&a, ea, sizeof ea);
                lcfg_xbe_effective(&b, eb, sizeof eb);
                ui_gettext(ctl(ID_XBE), fx, sizeof fx);
                ok = path_eq(ea, eb) && fx[0] == '\0' && b.xbe_path[0] == '\0';
                if (!ok) {
                    printf("     (22: Programmdatei '%s' -> '%s', Feld '%s')\n", ea, eb, fx);
                }
            }
            ok = ok &&
                 strcmp(a.media_root, b.media_root) == 0 &&
                 strcmp(a.hdd_root, b.hdd_root) == 0 &&
                 strcmp(a.input_profile, b.input_profile) == 0 &&
                 a.width == b.width && a.height == b.height &&
                 a.present_interval == b.present_interval &&
                 a.fps_limit == b.fps_limit && a.audio == b.audio &&
                 a.audio_live == b.audio_live &&
                 a.aspect_mode == b.aspect_mode && a.sbc_usb == b.sbc_usb &&
                 a.interp_fps == b.interp_fps &&
                 a.window_mode == b.window_mode && a.fs_hz == b.fs_hz &&
                 a.mouse_mode == b.mouse_mode && a.mouse_sens == b.mouse_sens &&
                 a.remaster == b.remaster && a.hd_textures == b.hd_textures &&
                 a.hd_scale == b.hd_scale && a.fx_enhanced == b.fx_enhanced &&
                 a.ultrawide == b.ultrawide && a.postfx == b.postfx &&
                 a.edge_mode == b.edge_mode && a.edge_fade_width == b.edge_fade_width &&
                 a.edge_fade_strength == b.edge_fade_strength &&
                 IsWindowEnabled(ctl(ID_EDGE_W)) != FALSE &&
                 ui_checked(ID_GP0 + LGP_CUSTOM) && /* W173: HD ohne Remaster-Werte */
                 a.monitor_grain == b.monitor_grain && a.monitor_glitch == b.monitor_glitch;
            if (ok) { /* W114: nur 2x/4x; alt hd_scale=0 (W111 Auswahl) zeigt 4x */
                a.hd_scale = 0;
                ui_from_settings(&a);
                lcfg_defaults(&b);
                ui_to_settings(&b);
                ok = b.hd_scale == 4 && ComboBox_GetCurSel(ctl(ID_REM_SCALE)) == 1 &&
                     ComboBox_GetCount(ctl(ID_REM_SCALE)) == 2;
                a.hd_scale = 2;
                ui_from_settings(&a);
                if (!ok) { printf("     (22: Stufe alt 0 -> 4x falsch)\n"); }
            }
            if (ok) { /* W173 LAUNCHER-P4: Optionsfelder setzen alle Optik-Werte */
                char t[300];
                /* W465: Remastered/Enhanced sind in SB nicht waehlbar (unsichtbar, Klick ohne Wirkung) */
                on_command(ID_GP0 + LGP_STRONG, BN_CLICKED);
                ok = ui_checked(ID_GP0 + LGP_CUSTOM) && !ui_checked(ID_GP0 + LGP_STRONG) &&
                     !ui_visible_on(ui_find(ID_GP0 + LGP_STRONG), P_GFX) &&
                     !ui_visible_on(ui_find(ID_GP0 + LGP_SUBTLE), P_GFX) &&
                     !ui_visible_on(ui_find(ID_GP0 + LGP_ENHANCED), P_GFX) &&
                     !ui_visible_on(ui_find(ID_GP0 + LGP_VIVID), P_GFX) && /* W478 */
                     ui_visible_on(ui_find(ID_GP0 + LGP_CLEAR), P_GFX);
                on_command(ID_GP0 + LGP_CLEAR, BN_CLICKED); /* ueber Benutzerdefiniert: Ja */
                lcfg_defaults(&b);
                ui_to_settings(&b);
                ok = ok && ui_checked(ID_GP0 + LGP_CLEAR) && b.remaster == 0 && b.hd_textures == 0 &&
                     b.fx_enhanced == 0 && b.ultrawide == 0 && b.postfx == 0 && b.hd_scale == 2 &&
                     b.shx[LSHX_SHADING] == 0 && b.monitor_fx == 0 && b.shx[LSHX_LABEL] == 0 && g_undo_on;
                if (ok) { /* Original: wirklich original (shading/fx/HD gesetzt) */
                    on_command(ID_GP0 + LGP_ORIGINAL, BN_CLICKED);
                    lcfg_defaults(&b);
                    ui_to_settings(&b);
                    ok = ui_checked(ID_GP0 + LGP_ORIGINAL) && b.remaster == 0 &&
                         b.hd_textures == 0 && b.fx_enhanced == 0 && b.shx[LSHX_SHADING] == 1 &&
                         b.fx[LFX_SOFT] == 0 && b.shx[LSHX_AA] == 0 &&
                         !ui_visible_on(ui_find(ID_GP_RESET), P_GFX);
                }
                if (ok) { /* Einzeloption -> Benutzerdefiniert (Basis Original) + Zuruecksetzen */
                    ComboBox_SetCurSel(ctl(ID_GFX_FX), LGP_SUBTLE);
                    on_command(ID_GFX_FX, CBN_SELCHANGE);
                    lcfg_defaults(&b);
                    ui_to_settings(&b);
                    ok = b.fx[LFX_SOFT] == 60 && b.fx_enhanced == 1 &&
                         ui_checked(ID_GP0 + LGP_CUSTOM) && g_undo_on &&
                         ui_visible_on(ui_find(ID_GP_RESET), P_GFX);
                    on_command(ID_GP_RESET, BN_CLICKED);
                    lcfg_defaults(&b);
                    ui_to_settings(&b);
                    ok = ok && ui_checked(ID_GP0 + LGP_ORIGINAL) && b.fx[LFX_SOFT] == 0;
                }
                if (ok) { /* Weltbeleuchtung aus -> Schatten grau (R15), Custom */
                    ComboBox_SetCurSel(ctl(ID_GFX_LIGHT), 1);
                    on_command(ID_GFX_LIGHT, CBN_SELCHANGE);
                    lcfg_defaults(&b);
                    ui_to_settings(&b);
                    ok = b.shx[LSHX_SHADING] == 0 && ui_checked(ID_GP0 + LGP_CUSTOM) &&
                         IsWindowEnabled(ctl(ID_GFX_SHADOW)) == FALSE &&
                         IsWindowEnabled(ctl(ID_GFX_SOFT)) == FALSE;
                    on_command(ID_GP_RESET, BN_CLICKED);
                    ok = ok && IsWindowEnabled(ctl(ID_GFX_SHADOW)) != FALSE &&
                         IsWindowEnabled(ctl(ID_GFX_SOFT)) == FALSE; /* R16: Schatten aus */
                }
                if (ok) { /* HUD-Etikett unabhaengig vom Preset (PO 7.11.) */
                    on_command(ID_REM_LABEL, BN_CLICKED);
                    ok = g_gfx.shx[LSHX_LABEL] == 1 && ui_checked(ID_GP0 + LGP_ORIGINAL);
                    on_command(ID_GP0 + LGP_CLEAR, BN_CLICKED); /* W465: Klare Sicht statt Remastered stark */
                    ok = ok && g_gfx.shx[LSHX_LABEL] == 1 && ui_checked(ID_GP0 + LGP_CLEAR) &&
                         ui_checked(ID_REM_LABEL);
                    on_command(ID_GP0 + LGP_ORIGINAL, BN_CLICKED);
                    on_command(ID_REM_LABEL, BN_CLICKED);
                    ok = ok && g_gfx.shx[LSHX_LABEL] == 0;
                }
                if (ok) { /* W296 PRESET-ORIG: Name ohne Bildrate (vorher exklusiv 144 Hz -> "144 FPS") */
                    ui_gettext(ctl(ID_GP0), t, sizeof t);
                    ok = strncmp(t, LT("gui.gfx_p_orig"), strlen(LT("gui.gfx_p_orig"))) == 0 && !strstr(t, "FPS") &&
                         !strstr(t, "144");
                    if (!ok) { printf("     (22: Name '%s')\n", t); }
                }
                if (ok) { /* W207: actual radio -> display controls -> save model. */
                    on_command(ID_GP0 + LGP_PERFORMANCE, BN_CLICKED);
                    lcfg_defaults(&b);
                    ui_to_settings(&b);
                    ok = ui_checked(ID_GP0 + LGP_PERFORMANCE) && b.interp_fps == 60 &&
                         b.rt_scale == 1 && !b.rt_scale_dyn && !b.fs_interp_match &&
                         lcfg_gfx_preset(&b, 0) == LGP_PERFORMANCE && !g_rr[9].active;
                    ip_set(120); /* W309 */
                    ui_apply_rules();
                    ui_to_settings(&b);
                    ok = ok && ui_checked(ID_GP0 + LGP_CUSTOM) &&
                         lcfg_gfx_base(&b, 0) == LGP_PERFORMANCE;
                    on_command(ID_GP_RESET, BN_CLICKED);
                    ui_to_settings(&b);
                    ok = ok && b.interp_fps == 60 && ui_checked(ID_GP0 + LGP_PERFORMANCE);
                    ComboBox_SetCurSel(ctl(ID_RTSCALE), 4);
                    ui_apply_rules();
                    ui_to_settings(&b);
                    ok = ok && b.rt_scale == 3 && ui_checked(ID_GP0 + LGP_CUSTOM);
                    on_command(ID_GP_RESET, BN_CLICKED);
                    on_command(ID_GP0 + LGP_ORIGINAL, BN_CLICKED);
                    ui_to_settings(&b);
                    ok = ok && b.interp_fps == SB_FPS_AUTO && b.rt_scale == 0 && b.fs_interp_match && /* W296 */
                         ui_checked(ID_GP0 + LGP_ORIGINAL);
                }
                if (ok) { /* W252: Klare Sicht ueber das Optionsfeld; Cockpit-Monitor folgt */
                    on_command(ID_GP0 + LGP_CLEAR, BN_CLICKED);
                    lcfg_defaults(&b);
                    ui_to_settings(&b);
                    ok = ui_checked(ID_GP0 + LGP_CLEAR) && b.shx[LSHX_SHADING] == 0 && b.monitor_fx == 0 &&
                         b.interp_fps == 120 &&
                         ComboBox_GetCurSel(ctl(ID_MONFX)) == 3 && lcfg_gfx_preset(&b, 0) == LGP_CLEAR;
                    if (!ok) { printf("     (22: Klare Sicht shading=%d mfx=%d sel=%d)\n", b.shx[LSHX_SHADING],
                                      b.monitor_fx, (int)ComboBox_GetCurSel(ctl(ID_MONFX))); }
                    if (!ok) { printf("     (22: chk=%d g=%d/%d fps=%d p=%d)\n", ui_checked(ID_GP0 + LGP_CLEAR),
                                      b.monitor_grain, b.monitor_glitch, b.interp_fps, lcfg_gfx_preset(&b, 0)); }
                    ComboBox_SetCurSel(ctl(ID_MONFX), 0); /* Monitor wieder an -> Benutzerdefiniert */
                    on_command(ID_MONFX, CBN_SELCHANGE);
                    ui_to_settings(&b);
                    ok = ok && b.monitor_fx == 1 && ui_checked(ID_GP0 + LGP_CUSTOM);
                    on_command(ID_GP_RESET, BN_CLICKED);
                    if (!ui_checked(ID_GP0 + LGP_ORIGINAL)) { on_command(ID_GP0 + LGP_ORIGINAL, BN_CLICKED); }
                    ui_to_settings(&b);
                    ok = ok && ui_checked(ID_GP0 + LGP_ORIGINAL) && b.monitor_fx == 1 &&
                         b.shx[LSHX_SHADING] == 1 && ComboBox_GetCurSel(ctl(ID_MONFX)) == 0;
                }
                if (ok) { /* W266 PRESET-EDGE: Raender (Seite Anzeige) gehoeren zum Preset */
                    edge_ui_set(1, 12, 60); /* Rand auf der Seite Anzeige geaendert -> Benutzerdefiniert */
                    ui_apply_rules();
                    lcfg_defaults(&b);
                    ui_to_settings(&b);
                    ok = b.edge_mode == 1 && ui_checked(ID_GP0 + LGP_CUSTOM) &&
                         lcfg_gfx_base(&b, 0) == LGP_ORIGINAL;
                    on_command(ID_GP0 + LGP_CLEAR, BN_CLICKED); /* Wahl setzt die Raender (nach R14-Rueckfrage); W465 */
                    if (!ui_checked(ID_GP0 + LGP_CLEAR)) { on_command(ID_GP_RESET, BN_CLICKED);
                                                           on_command(ID_GP0 + LGP_CLEAR, BN_CLICKED); }
                    ui_to_settings(&b);
                    /* W278: alle Presets 4:3 (edge_mode 1, Verlauf gesperrt); Breitbild ist eigene Wahl */
                    ok = ok && ui_checked(ID_GP0 + LGP_CLEAR) && b.edge_mode == 1 &&
                         b.edge_fade_width == 8 && b.edge_fade_strength == 100 &&
                         ComboBox_GetCurSel(ctl(ID_EDGE)) == 1 && IsWindowEnabled(ctl(ID_EDGE_W)) == FALSE;
                    ComboBox_SetCurSel(ctl(ID_EDGE), 2); /* Breitbild (Abdunklung) -> Benutzerdefiniert, Verlauf frei */
                    on_command(ID_EDGE, CBN_SELCHANGE);
                    ComboBox_SetCurSel(ctl(ID_EDGE_W), 0); /* 5 % */
                    on_command(ID_EDGE_W, CBN_SELCHANGE);
                    ui_to_settings(&b);
                    ok = ok && b.edge_mode == 2 && b.edge_fade_width == 5 && ui_checked(ID_GP0 + LGP_CUSTOM) &&
                         IsWindowEnabled(ctl(ID_EDGE_W)) != FALSE;
                    on_command(ID_GP_RESET, BN_CLICKED); /* Zuruecksetzen -> 4:3, 8 % */
                    ui_to_settings(&b);
                    ok = ok && b.edge_mode == 1 && b.edge_fade_width == 8 && ui_checked(ID_GP0 + LGP_CLEAR);
                    on_command(ID_GP0 + LGP_ORIGINAL, BN_CLICKED);
                    ui_to_settings(&b);
                    ui_gettext(ctl(ID_GP0), t, sizeof t);
                    ok = ok && ui_checked(ID_GP0 + LGP_ORIGINAL) && b.edge_mode == 1 && b.edge_fade_width == 8 &&
                         !strstr(t, "(HD)");
                    if (!ok) { printf("     (22: W266 Raender edge=%d/%d/%d '%s')\n", b.edge_mode, b.edge_fade_width,
                                      b.edge_fade_strength, t); }
                }
                if (!ok) { printf("     (22: Grafik-Preset falsch)\n"); }
                ui_from_settings(&a);
            }
            if (ok) { /* W81: randlos + Roheintrag-Rate (von Hand gesetzt) */
                a.window_mode = 1;
                a.fs_hz = 85;
                ui_from_settings(&a);
                lcfg_defaults(&b);
                ui_to_settings(&b);
                ok = b.window_mode == 1 && b.fs_hz == 85 &&
                     IsWindowEnabled(ctl(ID_FSHZ)) == FALSE;
            }
            if (ok) { /* W107: Roheintraege (von Hand) + harter Schnitt sperrt Verlauf */
                a.edge_mode = 1;
                a.edge_fade_width = 33;
                a.edge_fade_strength = 45;
                ui_from_settings(&a);
                lcfg_defaults(&b);
                ui_to_settings(&b);
                ok = b.edge_mode == 1 && b.edge_fade_width == 33 &&
                     b.edge_fade_strength == 45 &&
                     IsWindowEnabled(ctl(ID_EDGE_W)) == FALSE &&
                     IsWindowEnabled(ctl(ID_EDGE_S)) == FALSE;
                if (ok) { /* Auswahl Abdunklung gibt den Verlauf frei */
                    ComboBox_SetCurSel(ctl(ID_EDGE), 2);
                    on_command(ID_EDGE, CBN_SELCHANGE);
                    lcfg_defaults(&b);
                    ui_to_settings(&b);
                    ok = b.edge_mode == 2 && IsWindowEnabled(ctl(ID_EDGE_W)) != FALSE &&
                         IsWindowEnabled(ctl(ID_EDGE_S)) != FALSE;
                }
                if (!ok) { printf("     (22: Bildraender falsch)\n"); }
            }
            if (ok) { /* W170 LAUNCHER-P1/B4+B7+B19: Schalter ohne Wirkung sind
                 * Infozeilen (ausgeblendet), Standard 10 % ist Listeneintrag. */
                uictl_t *as = ui_find(ID_ASPECT); /* W173: Platzhalter-Schalter entfernt */
                ok = as && !ui_visible_on(as, P_DISPLAY);
                a.edge_mode = 2;
                a.edge_fade_width = 8;
                a.edge_fade_strength = 100;
                ui_from_settings(&a);
                lcfg_defaults(&b);
                ui_to_settings(&b);
                ok = ok && ComboBox_GetCount(ctl(ID_EDGE_W)) == KEDGEW_N &&
                     b.edge_fade_width == 8;
                if (!ok) { printf("     (22: B4/B7/B19 falsch)\n"); }
            }
            if (ok) { /* W170 LAUNCHER-P1/B10: Maus-Schalter folgt dem Controller */
                a.sbc_usb = 0;
                ui_from_settings(&a);
                ok = IsWindowEnabled(ctl(ID_CTRL_MOUSEOFF)) == FALSE;
                on_command(ID_CTRL_SBC, 0); /* Controller an -> Schalter frei */
                ok = ok && IsWindowEnabled(ctl(ID_CTRL_MOUSEOFF)) != FALSE;
                lcfg_defaults(&b);
                ui_to_settings(&b);
                ok = ok && b.sbc_usb == 1;
                if (!ok) { printf("     (22: Maus-Schalter folgt Controller nicht)\n"); }
            }
            if (ok) { /* W159: Kombi = Vorschau-Art; vr_enable kommt nicht aus der UI */
                strcpy_s(a.vr_preview, sizeof a.vr_preview, "sbs");
                a.vr_enable = 1; /* alte user.cfg: Startweg unberuehrt */
                ui_from_settings(&a);
                lcfg_defaults(&b);
                ui_to_settings(&b);
                ok = ComboBox_GetCurSel(ctl(ID_VR)) == 0 &&
                     strcmp(b.vr_preview, "sbs") == 0 && b.vr_enable == 0;
                strcpy_s(a.vr_preview, sizeof a.vr_preview, "right");
                ui_from_settings(&a);
                lcfg_defaults(&b);
                ui_to_settings(&b);
                ok = ok && ComboBox_GetCurSel(ctl(ID_VR)) == 2 &&
                     strcmp(b.vr_preview, "right") == 0;
                if (!ok) { printf("     (22: VR-Vorschau falsch)\n"); }
            }
            gui_teardown();
        }
        g_env_fix = NULL;
        g_confirm_auto = 0;
        if (ok) {
            GT_OK("22 ui-roundtrip");
        } else {
            GT_FAIL("22 ui-roundtrip", "%s", "Werte nach UI ungleich");
        }
    }

    /* 43-45 (W171 LAUNCHER-P2P3): Regelwerk in der Oberflaeche (je Regel
     * Fixture -> erwarteter UI-Zustand), 7-Seiten-Navigation + Aliase,
     * Tab-Reihenfolge je Seite (Fenster ausserhalb des Bildschirms, ohne
     * Fokus, wie --ui-shot). */
    {
        static lr_env_t fe;
        gui_opts_t o;
        int ok43 = 0, ok44 = 0, ok45 = 0;
        char why43[200] = "", why44[200] = "", why45[300] = "";
        memset(&o, 0, sizeof o);
        o.shot = 5;
        g_o = o;
        if (gui_init(client_override) && gui_create(1)) {
            int rule, v, i, k, p;
            build_controls();
            ok43 = 1;
            for (rule = LR_FIRST; rule <= LR_LAST && ok43; rule++) {
                for (v = 1; v >= 0 && ok43; v--) {
                    launcher_settings_t s;
                    if (v == 0 && lrules_always(rule)) {
                        continue;
                    }
                    lcfg_defaults(&s);
                    lrules_env_default(&fe, &s);
                    lrules_fixture(rule, v, &s, &fe);
                    g_env_fix = &fe;
                    ui_from_settings(&s); /* ruft ui_apply_rules */
                    if (g_rr[rule].active != ((SB_EDITION_COMMUNITY && rule == 32) ? 0 : v)) {
                        ok43 = 0;
                        _snprintf_s(why43, sizeof why43, _TRUNCATE,
                                    "R%d v=%d: Regel greift=%d", rule, v,
                                    g_rr[rule].active);
                        break;
                    }
                    for (i = 0; i < KRULECTL_N && ok43; i++) {
                        if (kRuleCtl[i].rule != rule) {
                            continue;
                        }
                        for (k = 0; k < 5 && kRuleCtl[i].ids[k]; k++) {
                            int want = !(v && lrules_locks(rule));
                            int en = IsWindowEnabled(ctl(kRuleCtl[i].ids[k])) != FALSE;
                            if (en != want) {
                                ok43 = 0;
                                _snprintf_s(why43, sizeof why43, _TRUNCATE,
                                            "R%d v=%d: Element %d frei=%d, erwartet %d",
                                            rule, v, kRuleCtl[i].ids[k], en, want);
                                break;
                            }
                        }
                    }
                }
            }
            /* Muster C: Voreinstellung ueber Benutzerdefiniert -> Rueckfrage
             * (Nein = bleibt), Ja = ersetzt + Rueckgaengig stellt her. */
            if (ok43) {
                launcher_settings_t s, b;
                lcfg_defaults(&s);
                lrules_env_default(&fe, &s);
                g_env_fix = &fe;
                s.shx[LSHX_SAT] = 77; /* Benutzerdefiniert (W173: ein Preset) */
                ui_from_settings(&s);
                g_confirm_auto = 0;
                on_command(ID_GP0 + LGP_PERFORMANCE, BN_CLICKED); /* W465: sichtbares Preset statt Remastered */
                ok43 = ui_checked(ID_GP0 + LGP_CUSTOM) && g_gfx.shx[LSHX_SAT] == 77 && !g_undo_on;
                g_confirm_auto = 1;
                on_command(ID_GP0 + LGP_PERFORMANCE, BN_CLICKED);
                ok43 = ok43 && ui_checked(ID_GP0 + LGP_PERFORMANCE) && g_undo_on;
                on_command(ID_UNDO, BN_CLICKED);
                lcfg_defaults(&b);
                ui_to_settings(&b);
                ok43 = ok43 && !g_undo_on && b.shx[LSHX_SAT] == 77 &&
                       ui_checked(ID_GP0 + LGP_CUSTOM);
                g_confirm_auto = 0;
                /* W173: Effect-Pack-Ausnahme - der vom Pack gesetzte Wert zaehlt
                 * nicht (Anzeige Original, keine R14-Rueckfrage) */
                lcfg_gfx_lock_key(&fe.gfx_lock, "color_sat");
                ui_from_settings(&s);
                ok43 = ok43 && ui_checked(ID_GP0 + LGP_ORIGINAL) && !g_rr[14].active;
                fe.gfx_lock = 0;
                if (!ok43) {
                    strcpy_s(why43, sizeof why43, "Muster C (R14 Rueckfrage/Undo)");
                }
            }
            g_env_fix = NULL;
            /* 44: sieben Seiten, Aliase, Strg+Tab-Zyklus, jede Seite hat
             * sichtbare Elemente */
            {
                static const struct { const char *n; int p; } al[] = {
                    { "play", P_PLAY }, { "display", P_DISPLAY }, { "graphics", P_GFX },
                    { "packs", P_PACKS }, { "controls", P_CTRL }, { "audio", P_AUDIO },
                    { "advanced", P_ADV }, { "start", P_PLAY }, { "grafik", P_DISPLAY },
                    { "remaster", P_GFX }, { "ton", P_AUDIO }, { "steuerung", P_CTRL },
                    { "profile", P_ADV }, { "pfade", P_ADV }, { "6", P_ADV },
                    { "alpha", P_ALPHA }, { "7", P_ALPHA }, /* W272 */
                    { "8", -1 }, { "xyz", -1 },
                };
                int seen[P_N] = { 0 }, cur = P_PLAY, n;
                ok44 = gui_page_count() == 8; /* W272: + Alpha Test */
                for (i = 0; i < (int)(sizeof al / sizeof al[0]); i++) {
                    if (gui_page_from_name(al[i].n) != al[i].p) {
                        ok44 = 0;
                        _snprintf_s(why44, sizeof why44, _TRUNCATE, "Alias '%s'", al[i].n);
                    }
                }
                for (n = 0; n < NAV_ORDER_N; n++) {
                    seen[cur] = 1;
                    cur = nav_next(cur, 1);
                }
                for (n = 0; n < P_N; n++) { /* W397: Test tools nicht mehr im Zyklus (unter dem Zahnrad) */
                    ok44 = ok44 && seen[n] == (n != P_ALPHA);
                }
                ok44 = ok44 && cur == P_PLAY && nav_next(P_PLAY, -1) == P_ADV && nav_next(P_ALPHA, 1) == P_PLAY &&
                       gui_page_from_name("settings") == P_LSET && strcmp(gui_page_key(P_LSET), "settings") == 0;
                for (p = 0; p < P_N && ok44; p++) {
                    int vis = 0;
                    for (i = 0; i < g_ui_n; i++) {
                        if (g_ui[i].parent == g_hwnd && g_ui[i].page == p &&
                            ui_visible_on(&g_ui[i], p)) {
                            vis++;
                        }
                    }
                    if (vis == 0) {
                        ok44 = 0;
                        _snprintf_s(why44, sizeof why44, _TRUNCATE,
                                    "Seite %s ohne Elemente", kPageKey[p]);
                    }
                }
                if (!ok44 && !why44[0]) {
                    strcpy_s(why44, sizeof why44, "Seitenzahl/Zyklus");
                }
            }
            /* 45: Tab-Reihenfolge: jedes sichtbare, freie Element mit
             * Tabstopp ist per Tab erreichbar (Kreis), Reihenfolge = Anlage
             * (build_controls: Seite oben links -> unten, dann rechts). */
            if (is_offscreen(g_hwnd)) {
                ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
                ok45 = 1;
                for (p = 0; p < P_N && ok45; p++) {
                    HWND h0, h;
                    int want = 0, got = 0, steps, last = -1, order_ok = 1;
                    char ord[600] = "";
                    show_page(p);
                    pump();
                    for (i = 0; i < g_ui_n; i++) {
                        uictl_t *u = &g_ui[i];
                        if (u->parent == g_hwnd && ui_visible_on(u, p) &&
                            (GetWindowLongW(u->h, GWL_STYLE) & WS_TABSTOP) &&
                            IsWindowEnabled(u->h)) {
                            want++;
                        }
                    }
                    h0 = GetNextDlgTabItem(g_hwnd, NULL, FALSE);
                    h = h0;
                    for (steps = 0; h && steps < 200; steps++) {
                        uictl_t *u = ui_by_hwnd(h);
                        if (!u) {
                            u = ui_by_hwnd(GetParent(h));
                        }
                        if (u && u->parent == g_hwnd) {
                            int idx = (int)(u - g_ui);
                            size_t ol = strlen(ord);
                            got++;
                            if (u->page == p) {
                                if (idx < last) {
                                    order_ok = 0;
                                }
                                last = idx;
                            }
                            _snprintf_s(ord + ol, sizeof ord - ol, _TRUNCATE,
                                        "%s%d", ol ? "," : "", u->id);
                        }
                        h = GetNextDlgTabItem(g_hwnd, h, FALSE);
                        if (h == h0) {
                            break;
                        }
                    }
                    printf("     (45: %s: %d Tab-Stopps: %s)\n", kPageKey[p], got, ord);
                    if (got != want || !order_ok) {
                        ok45 = 0;
                        _snprintf_s(why45, sizeof why45, _TRUNCATE,
                                    "Seite %s: %d erreichbar, %d erwartet, Reihenfolge %s",
                                    kPageKey[p], got, want, order_ok ? "ok" : "falsch");
                    }
                }
                ShowWindow(g_hwnd, SW_HIDE);
            } else {
                strcpy_s(why45, sizeof why45, "Fenster nicht ausserhalb des Bildschirms");
            }
            gui_teardown();
        }
        g_env_fix = NULL;
        if (ok43) {
            GT_OK("43 rules-ui (R1-R34 Fixture -> Freigabe/Begruendung, Muster C)");
        } else {
            GT_FAIL("43 rules-ui", "%s", why43);
        }
        if (ok44) {
            GT_OK("44 nav-8pages (Aliase, Strg+Tab-Zyklus, W272/W301 Test tools)");
        } else {
            GT_FAIL("44 nav-8pages", "%s", why44);
        }
        if (ok45) {
            GT_OK("45 tab-order");
        } else {
            GT_FAIL("45 tab-order", "%s", why45);
        }
    }

    /* 48-51 (W172 LAUNCHER-P567): Spielen (Einrichtungskarte, Sprunglinks),
     * Packs-Seite (Liste, Reihenfolge, Konflikte, Grenzen, abgelehnt,
     * leer, Hinzufuegen/Entfernen, B5), Erweitert (Datenort wie --userdata,
     * Schalter "beim Start laden"), Tooltips = Begruendung. Fenster ausserhalb
     * des Bildschirms, ohne Fokus; Test-Packs im Temp-Ordner (keine
     * Spieldaten). */
    {
        static lr_env_t fe;
        gui_opts_t o;
        int ok48 = 0, ok49 = 0, ok50 = 0, ok51 = 0, ok62 = 1;
        char why48[200] = "", why49[240] = "", why50[200] = "", why51[200] = "",
             why62[300] = "";
        char pkd[1100], pke[1100], q[1300], senv[1100];
        int had_env = u8_GetEnv("SB_PACKS_DIR", senv, sizeof senv) > 0;
        memset(&o, 0, sizeof o);
        o.shot = 5;
        g_o = o;
        _snprintf_s(pkd, sizeof pkd, _TRUNCATE, "%sw172_gui_packs", tmp);
        _snprintf_s(pke, sizeof pke, _TRUNCATE, "%sw172_gui_empty", tmp);
        lcfg_mkdir_p(pke);
        { /* Test-Packs: ta + tb (gleicher Hash -> Konflikt), Effekt-Pack fx,
           * kaputtes Pack bad (ohne manifest.json -> abgelehnt) */
            static const char *const nm[2] = { "ta", "tb" };
            static const char *const cat[2] = { "font_hud", "effect" };
            char z[65];
            int k;
            memset(z, '0', 64);
            z[64] = '\0';
            for (k = 0; k < 2; k++) {
                char m[2000];
                _snprintf_s(q, sizeof q, _TRUNCATE, "%s/%s", pkd, nm[k]);
                lcfg_mkdir_p(q);
                _snprintf_s(q, sizeof q, _TRUNCATE, "%s/%s/a.dds", pkd, nm[k]);
                write_file(q, "DDS ");
                _snprintf_s(m, sizeof m, _TRUNCATE,
                    "{ \"format\": \"sbpack\", \"format_version\": 1,\n"
                    "  \"id\": \"t.%s\", \"name\": \"Test %s\", \"version\": \"1.0.0\",\n"
                    "  \"author\": \"W172\", \"license\": \"CC0\", \"type\": \"texture\",\n"
                    "  \"hash_algo\": \"sbt1-fnv64\", \"game\": { \"xbe_sha256\": [\"%s\"] },\n"
                    "  \"entries\": [ { \"hash\": \"1111111111111111\", \"category\": \"%s\",\n"
                    "    \"orig\": { \"w\": 2, \"h\": 2 }, \"variants\": { \"x2\": { \"file\": \"a.dds\",\n"
                    "    \"w\": 4, \"h\": 4, \"fmt\": \"BC1\", \"mips\": 1, \"sha256\": \"%s\" } } }%s ] }",
                    nm[k], nm[k], z, cat[k], z,
                    k ? ",\n  { \"hash\": \"2222222222222222\", \"category\": \"cockpit\",\n"
                        "    \"orig\": { \"w\": 2, \"h\": 2 }, \"variants\": { \"x2\": { \"file\": \"a.dds\",\n"
                        "    \"w\": 4, \"h\": 4, \"fmt\": \"BC1\", \"mips\": 1, \"sha256\": \"" : "");
                if (k) { /* zweiten Eintrag schliessen */
                    size_t L = strlen(m);
                    _snprintf_s(m + L - 4, sizeof m - L + 4, _TRUNCATE,
                                "%s\" } } } ] }", z);
                }
                _snprintf_s(q, sizeof q, _TRUNCATE, "%s/%s/manifest.json", pkd, nm[k]);
                write_file(q, m);
            }
            _snprintf_s(q, sizeof q, _TRUNCATE, "%s/fx", pkd);
            lcfg_mkdir_p(q);
            {
                char m[1200];
                _snprintf_s(m, sizeof m, _TRUNCATE,
                    "{ \"format\": \"sbpack\", \"format_version\": 1,\n"
                    "  \"id\": \"t.fx\", \"name\": \"Test FX\", \"version\": \"0.3.0\",\n"
                    "  \"author\": \"W172\", \"license\": \"CC0\", \"type\": \"effect\",\n"
                    "  \"hash_algo\": \"sbt1-fnv64\", \"game\": { \"xbe_sha256\": [\"%s\"] },\n"
                    "  \"entries\": [] }", z);
                _snprintf_s(q, sizeof q, _TRUNCATE, "%s/fx/manifest.json", pkd);
                write_file(q, m);
                _snprintf_s(q, sizeof q, _TRUNCATE, "%s/fx/params.json", pkd);
                write_file(q, "{ \"params\": { \"fx_light\": 40, \"color_sat\": 110 } }");
            }
            _snprintf_s(q, sizeof q, _TRUNCATE, "%s/bad", pkd);
            lcfg_mkdir_p(q);
            _snprintf_s(q, sizeof q, _TRUNCATE, "%s/packs.json", pkd);
            write_file(q, "{ \"version\": 1, \"order\": [\n"
                          "  { \"id\": \"t.ta\", \"file\": \"ta\" },\n"
                          "  { \"id\": \"t.tb\", \"file\": \"tb\" },\n"
                          "  { \"id\": \"t.fx\", \"file\": \"fx\" },\n"
                          "  { \"id\": \"bad\", \"file\": \"bad\" } ],\n"
                          "  \"level\": \"x4\" }");
        }
        if (gui_init(client_override) && gui_create(1)) {
            launcher_settings_t s, b;
            int i;
            build_controls();
            tips_build();
            ShowWindow(g_hwnd, SW_SHOWNOACTIVATE); /* ausserhalb des Bildschirms */
            /* --- 48: Spielen --- */
            gt_setenv("SB_PACKS_DIR", pke);
            packs_load();
            lcfg_defaults(&s);
            lrules_env_default(&fe, &s);
            lrules_fixture(3, 1, &s, &fe); /* keine Spieldaten */
            g_env_fix = &fe;
            show_page(P_PLAY);
            ui_from_settings(&s);
            ok48 = play_setup_shown() && ui_visible_on(ui_find(ID_IMAGE), P_PLAY) &&
                   ui_visible_on(ui_find(ID_PLAY_FOLDER), P_PLAY) &&
                   !ui_visible_on(ui_find(ID_SUM_GO0), P_PLAY);
            if (!ok48) {
                strcpy_s(why48, sizeof why48, "nodata: keine Einrichtungskarte");
            }
            if (ok48) { /* gueltige Daten -> Bereitschaft mit 5 Sprunglinks */
                fe.data_ok = 1;
                strcpy_s(s.game_image, sizeof s.game_image, "C:/spiel.xiso");
                show_page(P_PLAY);
                ui_from_settings(&s);
                ok48 = !play_setup_shown() && !ui_visible_on(ui_find(ID_IMAGE), P_PLAY);
                for (i = 0; i < SUM_GO_N && ok48; i++) {
                    ok48 = ui_visible_on(ui_find(ID_SUM_GO0 + i), P_PLAY) &&
                           g_sum[i].label && g_sum[i].value[0];
                }
                if (!ok48) {
                    strcpy_s(why48, sizeof why48, "Bereitschaft/Sprunglinks fehlen");
                }
            }
            if (ok48) { /* Sprung zur Ursache; Spiel-Zeile: Abbild -> Karte */
                on_command(ID_SUM_GO0 + 3, BN_CLICKED);
                ok48 = g_page == P_PACKS;
                show_page(P_PLAY);
                on_command(ID_SUM_GO0 + 2, BN_CLICKED);
                ok48 = ok48 && g_page == P_DISPLAY;
                show_page(P_PLAY);
                on_command(ID_SUM_GO0, BN_CLICKED);
                ok48 = ok48 && g_page == P_PLAY && play_setup_shown() &&
                       ui_visible_on(ui_find(ID_IMAGE), P_PLAY);
                s.game_image[0] = '\0'; /* Ordner-Modus -> Erweitert */
                strcpy_s(s.media_root, sizeof s.media_root, dir);
                show_page(P_PLAY);
                ui_from_settings(&s);
                on_command(ID_SUM_GO0, BN_CLICKED);
                ok48 = ok48 && g_page == P_ADV;
                show_page(P_PLAY);
                on_command(ID_PLAY_FOLDER, BN_CLICKED);
                ok48 = ok48 && g_page == P_ADV;
                if (!ok48) {
                    strcpy_s(why48, sizeof why48, "Sprunglinks springen falsch");
                }
            }
            /* --- 62 (W220 PLAY-LAYOUT): Einrichtungskarte je Zustand ---
             * Jedes Element der Karte: echte Fenster-Sichtbarkeit, Bild passt
             * zum Zustand (kein altes Hintergrundbild), Pfad im Feld, Texte
             * eindeutig, keine Ueberlappung, richtige Ergebniszeile. */
            {
                static const struct {
                    const char *st;
                    int data_ok, setup, msg; /* msg: 0 keine, 1 Erststart, 2 ok, 3 Fehler */
                    const char *path;
                } k62[] = {
                    { "img_empty", 0, 1, 1, "" },
                    { "img_unchecked", 1, 1, 0, W220_ISO },
                    { "img_ok", 1, 1, 2, W220_ISO },
                    { "img_version", 0, 1, 3, W220_ISO_JP },
                    { "img_folder", 0, 1, 3, W220_DIR },
                    { "img_browse_bad", 0, 1, 3, W220_ISO_JP }, /* Start: leer */
                    { "img_browse", 1, 0, 0, W220_ISO },        /* Start: leer */
                };
                static const int setup_ids[] = { ID_IMAGE, ID_IMAGE_C, ID_IMAGE_B,
                                                 ID_PLAY_FOLDER };
                int si;
                for (si = 0; si < (int)(sizeof k62 / sizeof k62[0]) && ok62; si++) {
                    char t[1024], why[200] = "";
                    psrow_t rw[PSR_N];
                    int j, m, sh = k62[si].setup;
                    lcfg_defaults(&s);
                    s.game_image[0] = s.xbe_path[0] = s.media_root[0] = 0;
                    lrules_env_default(&fe, &s);
                    fe.data_ok = 0; /* Start: keine Spieldaten (R3, Karte offen) */
                    strcpy_s(fe.data_err, sizeof fe.data_err, "");
                    g_img_msg[0] = '\0';
                    g_img_path[0] = '\0';
                    show_page(P_PLAY);
                    ui_from_settings(&s);
                    fe.data_ok = k62[si].data_ok;
                    if (!k62[si].data_ok && k62[si].msg == 3) {
                        strcpy_s(fe.data_err, sizeof fe.data_err, "x");
                    }
                    w220_state(k62[si].st);
                    pump();
                    if (play_setup_shown() != sh) {
                        strcpy_s(why, sizeof why, "falsche Karte");
                    } else if (g_bb_setup != sh) {
                        strcpy_s(why, sizeof why, "Bild zeigt alte Karte (nicht neu aufgebaut)");
                    }
                    for (j = 0; j < 4 && !why[0]; j++) { /* echte Fenster */
                        uictl_t *u = ui_find(setup_ids[j]);
                        int vis = u && (GetWindowLongW(u->h, GWL_STYLE) & WS_VISIBLE) != 0;
                        if (vis != sh) {
                            _snprintf_s(why, sizeof why, _TRUNCATE,
                                        "Element %d sichtbar=%d", setup_ids[j], vis);
                        }
                    }
                    for (j = 0; j < SUM_GO_N && !why[0]; j++) {
                        uictl_t *u = ui_find(ID_SUM_GO0 + j);
                        int vis = u && (GetWindowLongW(u->h, GWL_STYLE) & WS_VISIBLE) != 0;
                        if (vis == sh) {
                            _snprintf_s(why, sizeof why, _TRUNCATE,
                                        "Aendern %d sichtbar=%d", j, vis);
                        }
                    }
                    ui_gettext(ctl(ID_IMAGE), t, sizeof t);
                    if (!why[0] && strcmp(t, k62[si].path) != 0) {
                        _snprintf_s(why, sizeof why, _TRUNCATE, "Pfadfeld '%s'", t);
                    }
                    if (!why[0] && !sh && (g_sum[0].col != C_OK ||
                                           !strstr(g_sum[0].value, "(Europe).iso"))) {
                        _snprintf_s(why, sizeof why, _TRUNCATE, "Bereitschaft Spiel '%s'",
                                    g_sum[0].value);
                    }
                    if (!why[0] && sh) {
                        /* Ergebniszeile */
                        play_setup_rows(rw);
                        m = !rw[PSR_MSG].text ? 0
                            : rw[PSR_MSG].text == g_img_msg ? (rw[PSR_MSG].col == C_OK ? 2 : 3)
                            : strcmp(rw[PSR_MSG].text, LT("gui.play_firstrun")) == 0 ? 1 : 4;
                        if (m != k62[si].msg) {
                            _snprintf_s(why, sizeof why, _TRUNCATE,
                                        "Ergebniszeile %d statt %d", m, k62[si].msg);
                        }
                        if (!why[0] && (!rw[PSR_FOLDERQ].text || !rw[PSR_TXT].text ||
                                        (SB_EDITION_COMMUNITY && !rw[PSR_DISC].text))) {
                            strcpy_s(why, sizeof why, "Hinweiszeile fehlt");
                        }
                    }
                    if (!why[0] && sh) {
                        /* sichtbare Elemente der Seite: Texte eindeutig, keine
                         * Ueberlappung untereinander und mit den Textzeilen */
                        RECT rc[64];
                        char tx[64][128];
                        int n = 0, a, b;
                        for (j = 0; j < g_ui_n && n < 48; j++) {
                            uictl_t *u = &g_ui[j];
                            if (u->parent != g_hwnd || u->page != P_PLAY ||
                                !(GetWindowLongW(u->h, GWL_STYLE) & WS_VISIBLE)) {
                                continue;
                            }
                            SetRect(&rc[n], u->x, u->y, u->x + u->w, u->y + u->h_);
                            tx[n][0] = '\0';
                            if (u->kind < K_EDIT) {
                                ui_gettext(u->h, tx[n], sizeof tx[n]);
                            }
                            n++;
                        }
                        for (j = 0; j < PSR_N; j++) {
                            if (rw[j].text) {
                                SetRect(&rc[n], rw[j].x, rw[j].y, rw[j].x + rw[j].w,
                                        rw[j].y + rw[j].h);
                                tx[n][0] = '\0';
                                if (rw[j].y < CY0 + 216 || rw[j].y + rw[j].h > CY0 + 448) {
                                    _snprintf_s(why, sizeof why, _TRUNCATE,
                                                "Zeile %d ausserhalb der Karte", j);
                                }
                                n++;
                            }
                        }
                        for (a = 0; a < n && !why[0]; a++) {
                            for (b = a + 1; b < n && !why[0]; b++) {
                                RECT x;
                                if (IntersectRect(&x, &rc[a], &rc[b])) {
                                    _snprintf_s(why, sizeof why, _TRUNCATE,
                                                "Ueberlappung %d/%d (y %ld/%ld)", a, b,
                                                rc[a].top, rc[b].top);
                                } else if (tx[a][0] && strcmp(tx[a], tx[b]) == 0) {
                                    _snprintf_s(why, sizeof why, _TRUNCATE,
                                                "Text doppelt '%s'", tx[a]);
                                }
                            }
                        }
                    }
                    if (why[0]) {
                        ok62 = 0;
                        _snprintf_s(why62, sizeof why62, _TRUNCATE, "%s: %s",
                                    k62[si].st, why);
                    }
                }
                imgfix_set(NULL, 0, NULL);
                g_img_msg[0] = '\0';
                g_img_path[0] = '\0';
                g_setup_force = 0;
            }
            { /* --- 63 (W231 US-LAUNCHER): Fixture US/EU/Japan -> Fassung, Bereitschaft,
               * Saveordner, Startparameter; Exit-Meldungen 5/8; Import-Hinweis --- */
                static const struct { const char *path; int rc, ed, us; } k63[] = {
                    { W220_ISO_US, 0, LCFG_ED_US, 1 },
                    { W220_ISO, 0, LCFG_ED_EU, 0 },
                    { W220_ISO_JP, 4, LCFG_ED_OTHER, 0 },
                };
                char why63[300] = "", hp[1200], args[1600], e63[600], keep63[16];
                launcher_settings_t s63;
                lcfg_gameinfo_t gi63;
                int i63, li63;
                const lr_env_t *fix63 = g_env_fix;
                g_env_fix = NULL; /* echter Regelweg (rules_data mit Fixture) */
                for (i63 = 0; i63 < 3 && !why63[0]; i63++) {
                    size_t hl;
                    lcfg_defaults(&s63);
                    s63.xbe_path[0] = s63.media_root[0] = '\0';
                    strcpy_s(s63.game_image, sizeof s63.game_image, k63[i63].path);
                    w220_fixture(k63[i63].path, k63[i63].rc);
                    show_page(P_PLAY);
                    ui_from_settings(&s63);
                    ui_apply_rules();
                    update_summary();
                    pump();
                    adv_hdd_path(hp, sizeof hp);
                    hl = strlen(hp);
                    game_info_ui(&s63, &gi63);
                    strcpy_s(args, sizeof args, "vr_enable=0");
                    if (g_edition != k63[i63].ed || gi63.edition != k63[i63].ed) {
                        _snprintf_s(why63, sizeof why63, _TRUNCATE, "%d: Fassung %d/%d", i63,
                                    g_edition, gi63.edition);
                    } else if (k63[i63].rc == 0 &&
                               !strstr(g_sum[0].value, LT(k63[i63].us ? "gui.ed_us_exp" : "gui.ed_eu"))) {
                        _snprintf_s(why63, sizeof why63, _TRUNCATE, "%d: Bereitschaft '%s'", i63,
                                    g_sum[0].value);
                    } else if (k63[i63].rc == 0 && g_sum[0].col != C_OK) {
                        _snprintf_s(why63, sizeof why63, _TRUNCATE, "%d: Bereitschaft nicht gruen", i63);
                    } else if (k63[i63].rc != 0 && g_data_ok) {
                        _snprintf_s(why63, sizeof why63, _TRUNCATE, "%d: Japan startbar", i63);
                    } else if ((hl > 3 && strcmp(hp + hl - 3, "\\us") == 0) != k63[i63].us) {
                        _snprintf_s(why63, sizeof why63, _TRUNCATE, "%d: Saveordner '%s'", i63, hp);
                    } else if (app_edition_start_args(g_client_dir, &s63, &gi63, args, sizeof args,
                                                      e63, sizeof e63) != 0) {
                        _snprintf_s(why63, sizeof why63, _TRUNCATE, "%d: Startparameter Fehler", i63);
                    } else if (k63[i63].us
                                   ? (strncmp(args, "vr_enable=0;allow_ntsc_u=1;hdd_root_us=", 39) != 0 ||
                                      strlen(args) < 3 ||
                                      strcmp(args + strlen(args) - 3, "\\us") != 0)
                                   : strcmp(args, "vr_enable=0") != 0) {
                        _snprintf_s(why63, sizeof why63, _TRUNCATE, "%d: Startparameter '%.80s'", i63,
                                    args);
                    }
                }
                /* Exit-Meldungen (wie nach Clientende, ohne Meldungsfenster) */
                strcpy_s(keep63, sizeof keep63, sb_lang_code());
                for (li63 = 0; li63 < 2 && !why63[0]; li63++) {
                    sb_lang_set(li63 ? "de" : "en");
                    set_status_k("x", 0);
                    if (client_exit_report(0, LCFG_ED_US, 0) || client_exit_report(1, LCFG_ED_EU, 0) ||
                        strcmp(g_status, "x") != 0) {
                        strcpy_s(why63, sizeof why63, "Exit 0/1 meldet");
                    } else if (!client_exit_report(8, LCFG_ED_US, 0) ||
                               strcmp(g_status, LT("gui.exit8")) != 0 || g_status_kind != 2 ||
                               !strstr(g_status, li63 ? "Exitcode 8" : "exit code 8") ||
                               !strstr(g_status, li63 ? "US-Fassung" : "US copy")) {
                        strcpy_s(why63, sizeof why63, "Exit 8");
                    } else if (!client_exit_report(5, LCFG_ED_US, 0) ||
                               strcmp(g_status, LT("gui.exit5_us")) != 0 ||
                               !strstr(g_status, li63 ? "US-Fassung" : "US copy")) {
                        strcpy_s(why63, sizeof why63, "Exit 5 US");
                    } else if (!client_exit_report(5, LCFG_ED_EU, 0) ||
                               strcmp(g_status, LT("gui.exit5")) != 0 ||
                               !strstr(g_status, li63 ? "Exitcode 5" : "exit code 5")) {
                        strcpy_s(why63, sizeof why63, "Exit 5 EU");
                    }
                }
                sb_lang_set(keep63);
                /* Import-Hinweis: Fassung nicht in den Daten erkennbar, nur Exportname */
                if (!why63[0]) {
                    static const struct { const char *f; int ed, warn; } h63[] = {
                        { "X:\\b\\Steelbound-saves-us-20261114-101010.zip", LCFG_ED_EU, 1 },
                        { "X:\\b\\Steelbound-saves-us-20261114-101010.zip", LCFG_ED_US, 0 },
                        { "X:\\b\\Steelbound-saves-20261114-101010.zip", LCFG_ED_US, 1 },
                        { "X:\\b\\Steelbound-saves-20261114-101010.zip", LCFG_ED_EU, 0 },
                        { "X:\\b\\my saves", LCFG_ED_US, 0 },
                    };
                    int h;
                    for (h = 0; h < 5 && !why63[0]; h++) {
                        char t63[700], want[700];
                        saves_edition_hint(h63[h].f, h63[h].ed, t63, sizeof t63);
                        if (h63[h].warn) {
                            _snprintf_s(want, sizeof want, _TRUNCATE, LT("gui.si_ed_mismatch"),
                                        LT(h63[h].ed == LCFG_ED_US ? "gui.ed_eu" : "gui.ed_us"),
                                        LT(h63[h].ed == LCFG_ED_US ? "gui.ed_us" : "gui.ed_eu"));
                        } else {
                            _snprintf_s(want, sizeof want, _TRUNCATE, LT("gui.si_ed_hint"),
                                        LT(h63[h].ed == LCFG_ED_US ? "gui.ed_us" : "gui.ed_eu"));
                        }
                        if (strcmp(t63, want) != 0) {
                            _snprintf_s(why63, sizeof why63, _TRUNCATE, "Import-Hinweis %d", h);
                        }
                    }
                }
                imgfix_set(NULL, 0, NULL);
                g_imgfix_ed = LCFG_ED_NONE;
                g_env_fix = fix63;
                set_status_k("", 0);
                if (!why63[0]) {
                    GT_OK("63 W231 US-LAUNCHER (US/EU/Japan: Fassung, Bereitschaft, Saveordner, Startparameter; Exit 5/8 EN/DE; Import-Hinweis)");
                } else {
                    GT_FAIL("63 W231 US-LAUNCHER", "%s", why63);
                }
            }
            /* --- 51: Tooltips = Begruendung der Sperre (Muster A) --- */
            {
                char tt[400];
                lcfg_defaults(&s);
                lrules_env_default(&fe, &s);
                lrules_fixture(7, 1, &s, &fe); /* Vollbild -> Aufloesung gesperrt */
                show_page(P_DISPLAY);
                ui_from_settings(&s);
                tip_text(ID_W, tt, sizeof tt);
                ok51 = g_rr[7].disable && !IsWindowEnabled(ctl(ID_W)) &&
                       tt[0] && strcmp(tt, g_rr[7].text) == 0;
                lrules_fixture(7, 0, &s, &fe);
                ui_from_settings(&s);
                tip_text(ID_W, tt, sizeof tt);
                ok51 = ok51 && tt[0] == '\0';
                lrules_fixture(23, 1, &s, &fe); /* Maus-aus ohne Controller */
                show_page(P_CTRL);
                ui_from_settings(&s);
                tip_text(ID_CTRL_MOUSEOFF, tt, sizeof tt);
                ok51 = ok51 && strcmp(tt, g_rr[23].text) == 0;
                show_page(P_DISPLAY); /* unsichtbar -> kein Tooltip */
                tip_text(ID_CTRL_MOUSEOFF, tt, sizeof tt);
                ok51 = ok51 && tt[0] == '\0' && g_tip != NULL;
                if (!ok51) {
                    _snprintf_s(why51, sizeof why51, _TRUNCATE, "Tooltip '%.80s'", tt);
                }
            }
            g_env_fix = NULL;
            /* --- 49: Packs-Seite --- */
            gt_setenv("SB_PACKS_DIR", pkd);
            g_pk_sel = 0;
            show_page(P_PACKS);
            ok49 = g_pk_n == 4 &&
                   (int)SendMessageW(ctl(ID_PK_LIST), LB_GETCOUNT, 0, 0) == 4 &&
                   packs_status_of(0) == 2 && packs_status_of(3) == 0 &&
                   packs_is_fx(2) && g_pk_covby[1 * PK_MAXN + 0] == 1 &&
                   g_pk_cov[1].cat[5] == 1 && g_pk_cov[1].cat[1] == 1 &&
                   g_pk_fx_hit[2] && g_pk_fx_val[2] == 40 &&
                   ui_visible_on(ui_find(ID_PK_LIST), P_PACKS) &&
                   !ui_visible_on(ui_find(ID_PK_CREATOR), P_PACKS);
            if (!ok49) {
                _snprintf_s(why49, sizeof why49, _TRUNCATE,
                            "Laden: n=%d st=%d/%d fx=%d covby=%d cat=%d/%d fxhit=%d/%d",
                            g_pk_n, packs_status_of(0), packs_status_of(3), packs_is_fx(2),
                            g_pk_covby[PK_MAXN], g_pk_cov[1].cat[5], g_pk_cov[1].cat[1],
                            g_pk_fx_hit[2], g_pk_fx_val[2]);
            }
            if (ok49) { /* Alt+Pfeil runter: ta <-> tb, packs.json + Konflikt neu */
                sb_pack_order_t ord[8];
                char lv[8], pj[1200];
                ListBox_SetCurSel(ctl(ID_PK_LIST), 0);
                g_pk_sel = 0;
                SendMessageW(ctl(ID_PK_LIST), WM_SYSKEYDOWN, VK_DOWN, 0);
                _snprintf_s(pj, sizeof pj, _TRUNCATE, "%s/packs.json", pkd);
                ok49 = sb_pack_read_cfg(pj, ord, 8, lv, sizeof lv, NULL, 0, NULL, 0) == 4 &&
                       strcmp(ord[0].id, "t.tb") == 0 && strcmp(ord[1].id, "t.ta") == 0 &&
                       g_pk_sel == 1 && g_pk_covby[1 * PK_MAXN + 0] == 1 &&
                       g_pk_covby[0 * PK_MAXN + 1] == 0;
                packs_move_to(1, 0); /* Ziehen-Ziel: zurueck nach oben */
                ok49 = ok49 && strcmp(g_pk_ord[0].id, "t.ta") == 0 && g_pk_sel == 0;
                if (!ok49) {
                    strcpy_s(why49, sizeof why49, "Reihenfolge (Alt+Pfeil/Ziehen)");
                }
            }
            if (ok49) { /* Klick auf den Schalter: aus; Leertaste: wieder an */
                int ih = (int)SendMessageW(ctl(ID_PK_LIST), LB_GETITEMHEIGHT, 0, 0);
                SendMessageW(ctl(ID_PK_LIST), WM_LBUTTONDOWN, MK_LBUTTON,
                             MAKELPARAM(SX(20), ih + ih / 2));
                ok49 = g_pk_ord[1].enabled == 0 && g_pk_sel == 1;
                SendMessageW(ctl(ID_PK_LIST), WM_KEYDOWN, VK_SPACE, 0);
                ok49 = ok49 && g_pk_ord[1].enabled == 1;
                if (!ok49) {
                    strcpy_s(why49, sizeof why49, "Schalter (Klick/Leertaste)");
                }
            }
            if (ok49) { /* abgelehnt: aus geht, an nicht (R21, Klartext-Grund) */
                ListBox_SetCurSel(ctl(ID_PK_LIST), 3);
                on_command(ID_PK_LIST, LBN_SELCHANGE);
                on_command(ID_PK_TOGGLE, BN_CLICKED); /* aus */
                ok49 = g_pk_ord[3].enabled == 0 && g_rr[21].active &&
                       !IsWindowEnabled(ctl(ID_PK_TOGGLE)) &&
                       strstr(g_rr[21].text, LT("gui.pkv_missing")) != NULL;
                packs_toggle_idx(3); /* an -> verweigert */
                ok49 = ok49 && g_pk_ord[3].enabled == 0 && g_status_kind == 2;
                if (!ok49) {
                    _snprintf_s(why49, sizeof why49, _TRUNCATE, "abgelehnt: '%.80s'",
                                g_rr[21].text);
                }
            }
            if (ok49) { /* Texturdetail-Segment + Grenzen (nur senken, effects+effect) */
                char lv[8], pj[1200];
                sb_pack_order_t ord[8];
                sb_pack_cap_t caps[8];
                int ncap;
                memset(caps, 0, sizeof caps);
                on_command(ID_PK_LVL2, BN_CLICKED);
                lcfg_defaults(&b);
                ui_to_settings(&b);
                ok49 = strcmp(g_pk_level, "x2") == 0 && b.hd_scale == 2 &&
                       ui_checked(ID_PK_LVL2) && !ui_checked(ID_PK_LVL4) &&
                       ComboBox_GetCount(ctl(ID_PK_CAT0)) == 2;
                ComboBox_SetCurSel(ctl(ID_PK_CAT0 + 5), 0); /* Effekte: Original */
                on_command(ID_PK_CAT0 + 5, CBN_SELCHANGE);
                on_command(ID_PK_LVL4, BN_CLICKED);
                _snprintf_s(pj, sizeof pj, _TRUNCATE, "%s/packs.json", pkd);
                sb_pack_read_cfg(pj, ord, 8, lv, sizeof lv, caps, 8, NULL, 0);
                for (ncap = 0; ncap < 8 && caps[ncap].name[0]; ncap++) {
                }
                ok49 = ok49 && strcmp(lv, "x4") == 0 && ncap == 2 &&
                       packs_cap_stored(5) == 0 && packs_cap_of(0) == 4 &&
                       ComboBox_GetCount(ctl(ID_PK_CAT0)) == 3 &&
                       ComboBox_GetCurSel(ctl(ID_PK_CAT0 + 5)) == 0;
                ComboBox_SetCurSel(ctl(ID_PK_CAT0), 1); /* HUD: 2x */
                on_command(ID_PK_CAT0, CBN_SELCHANGE);
                ok49 = ok49 && packs_cap_stored(0) == 2 && packs_cap_of(0) == 2;
                ComboBox_SetCurSel(ctl(ID_PK_CAT0), 2); /* HUD: 4x = keine Grenze */
                on_command(ID_PK_CAT0, CBN_SELCHANGE);
                ok49 = ok49 && packs_cap_stored(0) < 0;
                on_command(ID_PK_CAPRESET, BN_CLICKED);
                ok49 = ok49 && g_pk_ncaps == 0;
                if (!ok49) {
                    _snprintf_s(why49, sizeof why49, _TRUNCATE,
                                "Texturdetail/Grenzen: lv=%s ncap=%d cap5=%d", lv, ncap,
                                packs_cap_stored(5));
                }
            }
            if (ok49) { /* B5: installierte Packs zeigen die Remaster-Bedienung */
                int keep = g_rem_pkg;
                g_rem_pkg = 0;
                ok49 = rem_avail() && g_tex_src > 0 && ui_visible_on(ui_find(ID_GFX_TEX), P_GFX) &&
                       !ui_visible_on(ui_find(ID_REM_SCALE), P_GFX);
                gt_setenv("SB_PACKS_DIR", pke);
                show_page(P_PACKS); /* leer */
                ok49 = ok49 && !rem_avail() && g_tex_src == 0 &&
                       ui_visible_on(ui_find(ID_GFX_TEX), P_GFX) &&
                       !ui_visible_on(ui_find(ID_PK_LIST), P_PACKS) &&
                       g_rr[29].active &&
                       ui_visible_on(ui_find(ID_PK_CREATOR), P_PACKS) == (!SB_EDITION_COMMUNITY && g_creator == 1) &&
                       g_rr[32].active == (!SB_EDITION_COMMUNITY && g_creator != 1);
                { /* W201: simulate a neighbouring creator, never launch it. */
                    int creator_keep = g_creator, present, edition_ok = 1;
                    for (present = 0; present <= 1; present++) {
                        g_creator = present;
                        show_page(P_PACKS);
                        edition_ok = edition_ok &&
                            ui_visible_on(ui_find(ID_PK_CREATOR), P_PACKS) ==
                                (!SB_EDITION_COMMUNITY && present) &&
                            g_rr[32].active == (!SB_EDITION_COMMUNITY && !present);
                        if (SB_EDITION_COMMUNITY) {
                            edition_ok = edition_ok && !ui_find(ID_PK_CREATOR) &&
                                !strstr(g_rr[29].text, "Pack Creator") &&
                                strstr(g_rr[29].text, ".sbpack");
                        }
                    }
                    g_creator = creator_keep;
                    printf("%s W201 creator-edition (absent/present, %s)\n",
                           edition_ok ? "PASS" : "FAIL", SB_EDITION_BADGE);
                    ok49 = ok49 && edition_ok;
                }
                g_rem_pkg = keep;
                if (!ok49) {
                    strcpy_s(why49, sizeof why49, "B5/leerer Zustand");
                }
            }
            if (ok49) { /* Hinzufuegen (Ordner verknuepfen) + sofort geprueft; Entfernen */
                char src[1300];
                _snprintf_s(src, sizeof src, _TRUNCATE, "%s/ta", pkd);
                g_confirm_auto = 1;
                ok49 = packs_add_path(src, 1) == 0 && g_pk_n == 1 &&
                       g_pk_chk[0] >= 0 && strcmp(g_pk_ord[0].id, "t.ta") == 0 &&
                       packs_add_path(src, 1) != 0 && g_pk_n == 1; /* doppelt */
                g_pk_sel = 0;
                packs_del();
                ok49 = ok49 && g_pk_n == 0 && is_dir_u8(src); /* Ordner bleibt */
                g_confirm_auto = 0;
                if (!ok49) {
                    _snprintf_s(why49, sizeof why49, _TRUNCATE,
                                "Hinzufuegen/Entfernen: n=%d chk=%d", g_pk_n,
                                g_pk_n ? g_pk_chk[0] : -9);
                }
            }
            /* --- 50: Erweitert --- */
            {
                char path[1100], peek[1100], u8p[1100], root[1100];
                const char *mk2;
                adv_data_loc(path, sizeof path, &mk2); /* Selbsttest: legacy */
                ok50 = sb_userdata_mode() == SB_UD_LEGACY &&
                       strcmp(mk2, "gui.ud_legacy") == 0 && strcmp(path, g_client_disp) == 0;
                _snprintf_s(root, sizeof root, _TRUNCATE, "%sw172_ud_root", tmp);
                SetEnvironmentVariableA("SB_USERDATA", "profile");
                gt_setenv("SB_USERDATA_ROOT", root);
                sb_userdata_reset();
                adv_data_loc(path, sizeof path, &mk2);
                peek[0] = '\0';
                if (sb_userdata_path_peek(SB_UD_ROOT, peek, sizeof peek) == 1) {
                    u8_from_acp(peek, u8p, sizeof u8p);
                } else {
                    u8p[0] = '\0';
                }
                ok50 = ok50 && strcmp(mk2, "gui.ud_profile") == 0 && u8p[0] &&
                       strcmp(path, u8p) == 0; /* = --userdata "root" */
                SetEnvironmentVariableA("SB_USERDATA", "legacy");
                SetEnvironmentVariableA("SB_USERDATA_ROOT", NULL);
                sb_userdata_reset();
                if (!ok50) {
                    _snprintf_s(why50, sizeof why50, _TRUNCATE, "Datenort '%.70s' / '%.70s'",
                                path, u8p);
                }
                /* Schalter "beim Start laden" folgt der Auswahl (ohne Dateien) */
                strcpy_s(g_dflt_setup, sizeof g_dflt_setup, "mein_4k");
                ui_settext(ctl(ID_PROF_COMBO), "mein_4k");
                setup_auto_sync(0);
                ok50 = ok50 && ui_checked(ID_PROF_DFLT);
                ui_settext(ctl(ID_PROF_COMBO), "anderes");
                setup_auto_sync(0);
                ok50 = ok50 && !ui_checked(ID_PROF_DFLT) &&
                       ui_find(ID_PROF_DFLT)->kind == K_TOGGLE;
                g_dflt_setup[0] = '\0';
                if (ok50 == 0 && !why50[0]) {
                    strcpy_s(why50, sizeof why50, "Schalter beim Start laden");
                }
            }
            /* --- 57 (W187 KIA-LOAD): Spielstand-Pruefung + "Zuruecksetzen"
             * ueber den Launcher-Weg (Rueckfrage nein/ja, Sicherung, Meldung,
             * Pfad mit Umlaut) --- */
            {
                char m57[300], hd[1300], f57[1400], why57[200] = "";
                int ok57 = sb_savecheck_selftest(m57, sizeof m57) == 0, rc57;
                u8_find_t fd57;
                FILE *fp;
                if (!ok57) {
                    strcpy_s(why57, sizeof why57, m57);
                }
                _snprintf_s(hd, sizeof hd, _TRUNCATE, "%sw187_hdd_" U8_UE "_%lu", tmp,
                            GetCurrentProcessId());
                _snprintf_s(f57, sizeof f57, _TRUNCATE, "%s/TDATA", hd);
                u8_CreateDirectory(hd);
                u8_CreateDirectory(f57);
                _snprintf_s(f57, sizeof f57, _TRUNCATE, "%s/TDATA/43430002", hd);
                u8_CreateDirectory(f57);
                _snprintf_s(f57, sizeof f57, _TRUNCATE, "%s/TDATA/43430002/death00.sav", hd);
                if (u8_fopen_s(&fp, f57, "wb") == 0 && fp) {
                    fputs("kaputt", fp); /* abgeschnittener Alpha-1-Stand */
                    fclose(fp);
                }
                g_confirm_auto = 0; /* Nein: nichts passiert */
                rc57 = adv_saves_reset_at(hd, 0);
                if (ok57 && (rc57 != 2 || u8_GetFileAttributes(f57) == INVALID_FILE_ATTRIBUTES)) {
                    ok57 = 0;
                    _snprintf_s(why57, sizeof why57, _TRUNCATE, "Nein: rc=%d", rc57);
                }
                g_confirm_auto = 1; /* Ja: gesichert + leer neu angelegt */
                rc57 = adv_saves_reset_at(hd, 0);
                if (ok57 && (rc57 != 0 || g_status_kind != 1 ||
                             u8_GetFileAttributes(f57) != INVALID_FILE_ATTRIBUTES)) {
                    ok57 = 0;
                    _snprintf_s(why57, sizeof why57, _TRUNCATE, "Ja: rc=%d kind=%d", rc57,
                                g_status_kind);
                }
                _snprintf_s(f57, sizeof f57, _TRUNCATE, "%s/_backup/saves-*", hd);
                if (ok57 && !u8_find_first(f57, &fd57)) {
                    ok57 = 0;
                    strcpy_s(why57, sizeof why57, "keine Sicherung saves-*");
                } else if (ok57) {
                    char bf[1400];
                    _snprintf_s(bf, sizeof bf, _TRUNCATE,
                                "%s/_backup/%s/TDATA/43430002/death00.sav", hd, fd57.name);
                    u8_find_close(&fd57);
                    if (u8_GetFileAttributes(bf) == INVALID_FILE_ATTRIBUTES) {
                        ok57 = 0;
                        strcpy_s(why57, sizeof why57, "death00.sav fehlt in der Sicherung");
                    } else {
                        u8_DeleteFile(bf);
                        printf("     (57: Sicherung %s/_backup/%s)\n", hd, fd57.name);
                        { /* Sicherungsordner abbauen */
                            static const char *const sub57[] = {
                                "TDATA/43430002", "TDATA", "" };
                            int k;
                            for (k = 0; k < 3; k++) {
                                wchar_t w57[1400];
                                _snprintf_s(bf, sizeof bf, _TRUNCATE, "%s/_backup/%s/%s", hd,
                                            fd57.name, sub57[k]);
                                u8_to_w(bf, w57, 1400);
                                RemoveDirectoryW(w57);
                            }
                        }
                    }
                }
                rc57 = adv_saves_reset_at(hd, 0); /* zweimal: nichts zu sichern */
                if (ok57 && rc57 != 1) {
                    ok57 = 0;
                    _snprintf_s(why57, sizeof why57, _TRUNCATE, "zweimal: rc=%d", rc57);
                }
                g_confirm_auto = 0;
                { /* Testordner abbauen (leere TDATA/UDATA-Gerueste) */
                    static const char *const sub57[] = { "TDATA/43430002", "TDATA",
                        "UDATA/43430002", "UDATA", "_backup", "" };
                    int k;
                    for (k = 0; k < 6; k++) {
                        wchar_t w57[1400];
                        _snprintf_s(f57, sizeof f57, _TRUNCATE, "%s/%s", hd, sub57[k]);
                        u8_to_w(f57, w57, 1400);
                        RemoveDirectoryW(w57);
                    }
                }
                if (ok57) {
                    GT_OK("57 save-reset (sb_savecheck-Selbsttest, Rueckfrage, Sicherung, Umlaut-Pfad)");
                } else {
                    GT_FAIL("57 save-reset", "%s", why57);
                }
            }
            { /* W199: both transfer actions stop before opening any dialog. */
                int ok199;
                g_save_test_busy = 1;
                adv_saves_transfer(0);
                ok199 = g_status_kind == 2 && !strcmp(g_status, LT("gui.saves_reset_running"));
                set_status_k("", 0);
                adv_saves_transfer(1);
                ok199 = ok199 && g_status_kind == 2 && !strcmp(g_status, LT("gui.saves_reset_running")) &&
                        ui_visible_on(ui_find(ID_SAVES_IMPORT), P_ADV) &&
                        ui_visible_on(ui_find(ID_SAVES_EXPORT), P_ADV);
                g_save_test_busy = -1;
                if (ok199) { GT_OK("W199 transfer UI: visible, import/export blocked during play"); }
                else { GT_FAIL("W199 transfer UI", "%s", "running-client guard"); }
            }
            { /* W254 REPLAY-LAUNCHER: Knopf auf Erweitert, Text, Set-Kette des Startwegs */
                char t254[80], a254[200];
                int ok254 = ui_visible_on(ui_find(ID_SAVES_REPLAYS), P_ADV);
                ui_gettext(ctl(ID_SAVES_REPLAYS), t254, sizeof t254);
                ok254 = ok254 && strcmp(t254, LT("gui.rp_btn")) == 0;
                ls_replay_set_args(L"0A0B0C0D0E01", 3, a254, sizeof a254);
                ok254 = ok254 && strcmp(a254, "replay_autoplay=0A0B0C0D0E01;replay_camera=3") == 0;
                if (ok254) { GT_OK("W254 replay UI: button on Advanced, start args"); }
                else { GT_FAIL("W254 replay UI", "%s", t254); }
            }
            { /* W255 VIDEO-EXPORT: Standardordner = Fotoordner\Videos, Kette Replay + Video, Exit-Meldungen */
                char d255[700], p255[700], a255[1000], v255[300];
                int ok255;
                extras_dir(p255, sizeof p255);
                rp_video_default_dir(d255, sizeof d255);
                ok255 = strlen(d255) == strlen(p255) + 7 && !strncmp(d255, p255, strlen(p255)) &&
                        !strcmp(d255 + strlen(p255), "\\Videos");
                ls_replay_set_args(L"0A0B0C0D0E01", -1, a255, sizeof a255);
                ok255 = ok255 && !ls_video_set_args("", 1, 60, v255, sizeof v255);
                strcat_s(a255, sizeof a255, ";");
                strcat_s(a255, sizeof a255, v255);
                ok255 = ok255 && !strcmp(a255, "replay_autoplay=0A0B0C0D0E01;video_export=auto;video_width=2560;"
                                               "video_height=1440;video_fps=60");
                ok255 = ok255 && LT("gui.rp_vx_btn")[0] && strstr(LT("gui.rp_vx_done"), "%s") &&
                        strstr(LT("gui.rp_vx_failx"), "%lu");
                strcpy_s(g_vx_dir_open, sizeof g_vx_dir_open, d255);
                g_vx_pending = 1;
                rp_export_done(3); /* Testbetrieb: Meldung nur auf der Konsole */
                ok255 = ok255 && g_vx_pending == 0 && g_status_kind == 2 && !strcmp(g_status, LT("gui.rp_vx_fail3"));
                rp_export_done(0);
                ok255 = ok255 && g_status_kind == 1 && !strcmp(g_status, LT("gui.rp_vx_done_status"));
                set_status_k("", 0);
                if (ok255) { GT_OK("W255 video export: default folder, start args, result messages"); }
                else { GT_FAIL("W255 video export", "%s", a255); }
            }
            /* --- 58 (W190 SAVE-LOCK): Schalter "Spielstaende schuetzen" auf
             * Erweitert (immer sichtbar, auch bei eingeklappter Diagnose),
             * Klick -> Einstellung, user.cfg-Rundweg [game] save_lock,
             * Arbeitskopie-Selbsttest (sb_savelock_selftest) --- */
            {
                char m58[300], why58[200] = "", f58[1300];
                launcher_settings_t a58, b58;
                int ok58 = sb_savelock_selftest(m58, sizeof m58) == 0, od58 = g_diag_open;
                uictl_t *u58 = ui_find(ID_SAVE_LOCK);
                FILE *fp58;
                if (!ok58) {
                    strcpy_s(why58, sizeof why58, m58);
                }
                if (ok58 && (!u58 || u58->kind != K_TOGGLE)) {
                    ok58 = 0;
                    strcpy_s(why58, sizeof why58, "kein Schalter ID_SAVE_LOCK");
                }
                if (ok58) {
                    lcfg_defaults(&a58);
                    if (a58.save_lock != 0) {
                        ok58 = 0;
                        strcpy_s(why58, sizeof why58, "Standard nicht 0");
                    }
                }
                if (ok58) { /* Einstellung -> Schalter -> Klick -> Einstellung */
                    a58.save_lock = 1;
                    ui_from_settings(&a58);
                    g_diag_open = 0;
                    if (!ui_checked(ID_SAVE_LOCK) || !ui_visible_on(u58, P_ADV)) {
                        ok58 = 0;
                        strcpy_s(why58, sizeof why58, "save_lock=1 nicht angezeigt (eingeklappt)");
                    }
                    on_command(ID_SAVE_LOCK, BN_CLICKED);
                    ui_to_settings(&b58);
                    if (ok58 && (ui_checked(ID_SAVE_LOCK) || b58.save_lock != 0)) {
                        ok58 = 0;
                        strcpy_s(why58, sizeof why58, "Klick schaltet nicht aus");
                    }
                    on_command(ID_SAVE_LOCK, BN_CLICKED);
                    ui_to_settings(&b58);
                    if (ok58 && b58.save_lock != 1) {
                        ok58 = 0;
                        strcpy_s(why58, sizeof why58, "Klick schaltet nicht ein");
                    }
                    g_diag_open = od58;
                }
                if (ok58) { /* user.cfg-Rundweg */
                    char line[256];
                    int seen = 0;
                    _snprintf_s(f58, sizeof f58, _TRUNCATE, "%sw190_user_%lu.cfg", tmp,
                                GetCurrentProcessId());
                    u8_DeleteFile(f58);
                    lcfg_defaults(&a58);
                    a58.save_lock = 1;
                    if (lcfg_save_user(f58, &a58) != 0) {
                        ok58 = 0;
                        strcpy_s(why58, sizeof why58, "lcfg_save_user");
                    }
                    lcfg_defaults(&b58);
                    if (ok58 && (lcfg_load_file(f58, &b58) != 0 || b58.save_lock != 1)) {
                        ok58 = 0;
                        strcpy_s(why58, sizeof why58, "Rundweg save_lock=1");
                    }
                    if (ok58 && u8_fopen_s(&fp58, f58, "r") == 0 && fp58) {
                        while (fgets(line, sizeof line, fp58)) {
                            if (strncmp(line, "save_lock=1", 11) == 0) {
                                seen = 1;
                            }
                        }
                        fclose(fp58);
                    }
                    if (ok58 && !seen) {
                        ok58 = 0;
                        strcpy_s(why58, sizeof why58, "Zeile save_lock=1 fehlt");
                    }
                    lcfg_apply_set(&b58, "save_lock=0");
                    if (ok58 && b58.save_lock != 0) {
                        ok58 = 0;
                        strcpy_s(why58, sizeof why58, "--set save_lock=0");
                    }
                    u8_DeleteFile(f58);
                }
                if (ok58) {
                    GT_OK("58 save-lock (Schalter Erweitert, Klick, user.cfg-Rundweg, Arbeitskopie-Selbsttest)");
                } else {
                    GT_FAIL("58 save-lock", "%s", why58);
                }
            }
            /* --- 59 (W195 PHOTO-MODE): Karte "Extras" auf Ton & Extras: Fotoordner (Feld,
             * Durchsuchen, Ordner oeffnen), Standard-Aufnahmeaufloesung, Format; Einstellung
             * <-> Oberflaeche, user.cfg-Rundweg [photo] (Umlaut-Pfad, x4, jpg), --set,
             * Standardordner gesetzt, "Ordner oeffnen" ohne Ordner = Hinweis (kein Absturz) --- */
            {
                char why59[200] = "", f59[1300], dd59[700];
                launcher_settings_t a59, b59;
                int ok59 = 1, k59;
                static const int ids59[] = { ID_PH_DIR, ID_PH_BROWSE, ID_PH_OPEN, ID_PH_RES, ID_PH_FMT, ID_PH_ASP };
                for (k59 = 0; k59 < 6 && ok59; k59++) {
                    uictl_t *u59 = ui_find(ids59[k59]);
                    if (!u59 || !ui_visible_on(u59, P_AUDIO) || ui_visible_on(u59, P_ADV)) {
                        ok59 = 0;
                        _snprintf_s(why59, sizeof why59, _TRUNCATE, "Element %d fehlt/falsche Seite", ids59[k59]);
                    }
                }
                lcfg_defaults(&a59);
                if (ok59 && (a59.photo_dir[0] || a59.photo_scale != 1 || a59.photo_jpg != 0)) {
                    ok59 = 0;
                    strcpy_s(why59, sizeof why59, "Standard nicht leer/x1/PNG");
                }
                if (ok59) { /* Einstellung -> Oberflaeche -> Einstellung */
                    strcpy_s(a59.photo_dir, sizeof a59.photo_dir, "C:\\Fotos \xC3\xBC" "ber\\SB");
                    a59.photo_scale = 4;
                    a59.photo_size = 4; /* W290: Groesse = photo_size (photo_scale wird passend mitgeschrieben) */
                    a59.photo_jpg = 1;
                    a59.photo_aspect = 2;
                    ui_from_settings(&a59);
                    lcfg_defaults(&b59);
                    ui_to_settings(&b59);
                    if (strcmp(b59.photo_dir, a59.photo_dir) != 0 || b59.photo_scale != 4 || b59.photo_size != 4 ||
                        b59.photo_jpg != 1 || b59.photo_aspect != 2) {
                        ok59 = 0;
                        strcpy_s(why59, sizeof why59, "Oberflaeche-Rundweg");
                    }
                }
                extras_default_dir(dd59, sizeof dd59);
                if (ok59 && (!dd59[0] || !strstr(dd59, "Screenshots"))) {
                    ok59 = 0;
                    strcpy_s(why59, sizeof why59, "Standardordner ohne Screenshots");
                }
                if (ok59) { /* user.cfg-Rundweg */
                    _snprintf_s(f59, sizeof f59, _TRUNCATE, "%sw195_user_%lu.cfg", tmp, GetCurrentProcessId());
                    u8_DeleteFile(f59);
                    if (lcfg_save_user(f59, &a59) != 0) {
                        ok59 = 0;
                        strcpy_s(why59, sizeof why59, "lcfg_save_user");
                    }
                    lcfg_defaults(&b59);
                    if (ok59 && (lcfg_load_file(f59, &b59) != 0 || strcmp(b59.photo_dir, a59.photo_dir) != 0 ||
                                 b59.photo_scale != 4 || b59.photo_jpg != 1 || b59.photo_aspect != 2)) {
                        ok59 = 0;
                        strcpy_s(why59, sizeof why59, "Rundweg photo_dir/photo_scale/photo_format");
                    }
                    lcfg_apply_set(&b59, "photo_scale=3");
                    if (ok59 && b59.photo_scale != 1) {
                        ok59 = 0;
                        strcpy_s(why59, sizeof why59, "--set photo_scale=3 nicht auf x1");
                    }
                    lcfg_apply_set(&b59, "photo_format=png");
                    if (ok59 && b59.photo_jpg != 0) {
                        ok59 = 0;
                        strcpy_s(why59, sizeof why59, "--set photo_format=png");
                    }
                    u8_DeleteFile(f59);
                }
                if (ok59) { /* Ordner oeffnen ohne Ordner -> Hinweis */
                    ui_settext(ctl(ID_PH_DIR), "C:\\gibt_es_nicht_w195\\x");
                    on_command(ID_PH_OPEN, BN_CLICKED);
                    ui_settext(ctl(ID_PH_DIR), "");
                }
                lcfg_defaults(&a59);
                ui_from_settings(&a59);
                if (ok59) {
                    GT_OK("59 photo-extras (Karte Extras, Rundweg Oberflaeche/user.cfg, Umlaut-Pfad, --set, Standardordner)");
                } else {
                    GT_FAIL("59 photo-extras", "%s", why59);
                }
            }
            /* --- 63 (W250 UPDATE-CHECK): Erstabfrage / Hinweis / Einstellung.
             * Testbetrieb (shot != 0): kein Netz, nichts wird gespeichert. */
            {
                char why63[160] = "";
                const char *v0 = g_upd_ver;
                g_upd_ver = "0.2.0"; /* W301 */
                g_upd_cfg = -1;
                g_upd_ask = 1;
                memset(&g_upd_res, 0, sizeof g_upd_res);
                /* W397 LAUNCHER-STRUCT: Erstabfrage "Online & Updates" mit drei Stufen (SB- und LoC-Spielen),
                 * Stufe = abgeleitet aus launcher_loc_online/update_check, Uebernahme alter Antworten. */
                {
                    static const struct { const char *cfg; int mode, ask; } kMig[] = {
                        { "update_check=1\nlauncher_loc_online=1\n", NETM_STEEL, 0 }, /* ja + Online -> steelnet */
                        { "update_check=1\nlauncher_loc_online=0\n", NETM_VER, 0 },   /* nur Update -> Version */
                        { "update_check=0\n", NETM_OFF, 0 },                          /* nein -> offline */
                        { "update_check=0\nlauncher_loc_online=1\n", NETM_STEEL, 0 }, /* Online an bleibt */
                        { "# nie gefragt\n", NETM_OFF, 1 },                           /* neu: fragen */
                        { "launcher_loc_online=1\n", NETM_STEEL, 0 },                 /* Online an: nicht fragen */
                    };
                    char tmp63[1024], cfg63[1200], keep63[1200];
                    int fs0 = g_onl_fix_srv, on0 = g_loc_online, m;
                    u8_GetTempPath(tmp63, sizeof tmp63);
                    _snprintf_s(cfg63, sizeof cfg63, _TRUNCATE, "%sw397_netq_user.cfg", tmp63);
                    strcpy_s(keep63, sizeof keep63, g_ucfg_test);
                    strcpy_s(g_ucfg_test, sizeof g_ucfg_test, cfg63);
                    g_onl_fix_srv = 1;
                    for (m = 0; m < (int)(sizeof kMig / sizeof kMig[0]) && !why63[0]; m++) {
                        FILE *f = NULL;
                        if (u8_fopen_s(&f, cfg63, "w") == 0 && f) {
                            fputs(kMig[m].cfg, f);
                            fclose(f);
                        }
                        onl_load();
                        g_upd_cfg = lupd_cfg_get(cfg63);
                        if (netm_get() != kMig[m].mode || netq_should_ask("0.3.0", g_upd_cfg, g_loc_online) != kMig[m].ask)
                            _snprintf_s(why63, sizeof why63, _TRUNCATE, "Uebernahme %d: Stufe %d (soll %d), Frage %d",
                                        m, netm_get(), kMig[m].mode, netq_should_ask("0.3.0", g_upd_cfg, g_loc_online));
                    }
                    if (!why63[0] && netq_should_ask("", -1, 0)) /* ohne Release-Version nie fragen (wie W250) */
                        strcpy_s(why63, sizeof why63, "ohne Version gefragt");
                    /* Waehlen schreibt beide alten Schluessel (Temp-Datei) */
                    for (m = NETM_N - 1; m >= 0 && !why63[0]; m--) {
                        static const int kOn[NETM_N] = { 1, 0, 0 }, kUp[NETM_N] = { 1, 1, 0 };
                        on_command(ID_NETM0 + m, BN_CLICKED);
                        pump();
                        g_loc_online = -1;
                        onl_load();
                        if (g_loc_online != kOn[m] || g_upd_cfg != kUp[m] || netm_get() != m ||
                            !ui_checked(ID_NETM0 + m) || ui_checked(ID_NETM0 + (m + 1) % NETM_N))
                            _snprintf_s(why63, sizeof why63, _TRUNCATE, "Stufe %d: online=%d update=%d", m,
                                        g_loc_online, g_upd_cfg);
                    }
                    /* ohne steelnet-Pins: Stufe steelnet grau, Klick aendert nichts */
                    g_onl_fix_srv = 0;
                    g_loc_online = 0;
                    netm_sync_ui();
                    on_command(ID_NETM0 + NETM_STEEL, BN_CLICKED);
                    if (!why63[0] && (g_loc_online || IsWindowEnabled(ctl(ID_NETM0 + NETM_STEEL))))
                        strcpy_s(why63, sizeof why63, "ohne Dienst: steelnet waehlbar");
                    g_onl_fix_srv = 1;
                    u8_DeleteFile(cfg63);
                    strcpy_s(g_ucfg_test, sizeof g_ucfg_test, keep63);
                    /* Erstabfrage sichtbar auf SB- und LoC-Spielen, Antwort beendet sie */
                    for (m = 0; m < 2 && !why63[0]; m++) {
                        int i3;
                        g_game = m ? LGAME_LOC : LGAME_SB;
                        g_upd_cfg = -1;
                        g_loc_online = 0;
                        g_upd_ask = 1;
                        loctab_nav_layout();
                        show_page(P_PLAY);
                        pump();
                        for (i3 = 0; i3 < NETM_N; i3++) {
                            if (!IsWindowVisible(ctl(ID_NETQ0 + i3)))
                                _snprintf_s(why63, sizeof why63, _TRUNCATE, "%s: Erstabfrage Knopf %d fehlt",
                                            m ? "LoC" : "SB", i3);
                        }
                        if (!why63[0] && (IsWindowVisible(ctl(ID_UPD_OPEN)) || ui_find(ID_UPD_YES) || ui_find(ID_UPD_AUTO)))
                            strcpy_s(why63, sizeof why63, "alte Update-Frage/Schalter noch da");
                        on_command(ID_NETQ0 + (m ? NETM_STEEL : NETM_VER), BN_CLICKED);
                        pump();
                        if (!why63[0] && (g_upd_ask || g_upd_cfg != 1 || g_upd_busy || IsWindowVisible(ctl(ID_NETQ0)) ||
                                          netm_get() != (m ? NETM_STEEL : NETM_VER)))
                            _snprintf_s(why63, sizeof why63, _TRUNCATE, "%s: Antwort nicht uebernommen", m ? "LoC" : "SB");
                        if (!why63[0] && m && !IsWindowVisible(ctl(ID_LOC_MP_GO)))
                            strcpy_s(why63, sizeof why63, "LoC: Karte Multiplayer fehlt nach der Antwort");
                    }
                    g_game = LGAME_SB;
                    loctab_nav_layout();
                    on_command(ID_NETQ0 + NETM_OFF, BN_CLICKED);
                    if (!why63[0] && (g_upd_cfg != 0 || g_loc_online || netm_get() != NETM_OFF))
                        strcpy_s(why63, sizeof why63, "offline: Zustand falsch");
                    g_onl_fix_srv = fs0;
                    g_loc_online = on0;
                }
                /* Hinweis mit fremder Adresse: Knopf sichtbar, Oeffnen verweigert */
                g_upd_cfg = 1;
                g_upd_res.result = LUPD_R_NEWER;
                strcpy_s(g_upd_res.rel.tag, sizeof g_upd_res.rel.tag, "v0.2.1");
                strcpy_s(g_upd_res.rel.url, sizeof g_upd_res.rel.url,
                         "https://github.com/someone/Steelbound/releases/tag/v0.2.1");
                show_page(P_PLAY);
                pump();
                if (!why63[0] && !IsWindowVisible(ctl(ID_UPD_OPEN)))
                    strcpy_s(why63, sizeof why63, "Hinweis: Knopf fehlt");
                on_command(ID_UPD_OPEN, BN_CLICKED);
                if (!why63[0] && strcmp(g_status, LT("gui.upd_badurl")) != 0)
                    strcpy_s(why63, sizeof why63, "fremde URL nicht abgewiesen");
                /* Launcher-Einstellungen (Zahnrad): Stufen, "Jetzt pruefen" und Release-Seite nur mit Release-Version */
                on_command(ID_LSET_OPEN, BN_CLICKED);
                pump();
                if (!why63[0] && (g_page != P_LSET || !ui_checked(ID_LSET_OPEN) || !IsWindowVisible(ctl(ID_NETM0)) ||
                                  !IsWindowVisible(ctl(ID_UPD_NOW)) || !IsWindowVisible(ctl(ID_LSET_RELPAGE)) ||
                                  !IsWindowVisible(ctl(ID_BG)) || !IsWindowVisible(ctl(ID_LANG))))
                    strcpy_s(why63, sizeof why63, "Zahnrad: Seite/Elemente fehlen");
                g_upd_ver = "";
                ui_refresh_visibility();
                pump();
                if (!why63[0] && (IsWindowVisible(ctl(ID_UPD_NOW)) || IsWindowVisible(ctl(ID_LSET_RELPAGE))))
                    strcpy_s(why63, sizeof why63, "ohne Version: Pruefen sichtbar");
                show_page(P_ADV);
                pump();
                if (!why63[0] && (IsWindowVisible(ctl(ID_BG)) || IsWindowVisible(ctl(ID_LANG)) || ui_checked(ID_LSET_OPEN)))
                    strcpy_s(why63, sizeof why63, "Erweitert zeigt noch Launcher-Teile");
                g_upd_ver = v0;
                g_upd_ask = 0;
                g_upd_cfg = -1;
                memset(&g_upd_res, 0, sizeof g_upd_res);
                ui_refresh_visibility();
                show_page(P_PLAY);
                if (!why63[0]) {
                    GT_OK("63 W250 update-check (Erstabfrage ja/nein, Schalter, Hinweis, fremde URL abgewiesen, ohne Version ausgeblendet)");
                } else {
                    GT_FAIL("63 W250 update-check", "%s", why63);
                }
            }
            loctab_gui_selftest(pass, fail); /* W276 LOC-LAUNCH (Fall 65) */
            gameswitch_gui_selftest(pass, fail); /* W398 LAUNCHER-GAMESWITCH (Fall 65c) */
            loc_gate_gui_selftest(pass, fail); /* W400 LAUNCHER-LOC-ONLINE-GATE (Fall 65d) */
            loc_lan_gui_selftest(pass, fail); /* W454 LOC-LAN-MODE (Fall 65g) */
            r1_polish_gui_selftest(pass, fail); /* W403 LAUNCHER-R1-POLISH (Fall 65e) */
            locver_gui_selftest(pass, fail); /* W421 LOC-VERSION-GATE (Fall 65f) */
            header_polish_selftest(pass, fail); /* W401 LAUNCHER-HEADER-POLISH (Fall 65d) */
            loc_save_gui_selftest(pass, fail); /* W375 LOC-SAVE #50 (Fall 65b) */
            pergame_gui_selftest(pass, fail); /* W402 LAUNCHER-PERGAME (Fall 65e) */
#if defined(SB_HAVE_LOCTP) && SB_HAVE_LOCTP
            loctp_gui_selftest(pass, fail); /* W336 LOC-TESTPARTNER (Fall 74) */
            loctp_mode_selftest(pass, fail); /* W351 LOC-TP-FIX (Fall 74b) */
#else
            if (!ctl(ID_LOCTP_OPEN) && !ctl(ID_LOCTP_STOP)) { /* W336: Community-Ausgabe ohne Testpartner */
                GT_OK("74 W336 loctp: Community-Ausgabe ohne Testpartner-Knoepfe");
            } else {
                GT_FAIL("74 W336 loctp", "%s", "Testpartner-Knopf in der Community-Ausgabe");
            }
#endif
            orig_gui_selftest(pass, fail); /* W278 ORIG-FLAG (Fall 66) */
            lay_gui_selftest(pass, fail); /* W281 LAUNCHER-SIZE (Fall 67) */
            xicon_gui_selftest(pass, fail); /* W287 EXP-ICON (Fall 68) */
            adv_gui_selftest(pass, fail); /* W291 ESC-REWORK (Fall 69) */
            locl_gui_selftest(pass, fail); /* W293 LOC-LAYOUT (Fall 70) */
            porig_gui_selftest(pass, fail); /* W296 PRESET-ORIG (Fall 71) */
            fpsmon_gui_selftest(pass, fail); /* W309 FPS-MONITOR (Fall 72) */
            locip_gui_selftest(pass, fail); /* W323 LOC-INTERP (Fall 73) */
            locwide_gui_selftest(pass, fail); /* W326 LOC-WIDE (Fall 74) */
            loc169_gui_selftest(pass, fail);  /* W414 LOC-DEFAULT-169 (Fall 74c) */
            locsblook_gui_selftest(pass, fail); /* W459 LOC-FX-LIGHT (Fall 74d) */
            w465_gui_selftest(pass, fail); /* W465 LOC-ENHANCED (Fall 74e) */
            w476_gui_selftest(pass, fail); /* W476 LAUNCHER-CUSTOM-PRESET (Fall 74f) */
            w478_gui_selftest(pass, fail); /* W478 LOC-ENHANCED-VIVID (Fall 74g) */
            acc_gui_selftest(pass, fail); /* W328 STEELNET-ACCOUNT (Fall 75/76) */
            accpage_gui_selftest(pass, fail); /* W399 LAUNCHER-ACCOUNT-PAGE (Fall 75b) */
            accux_gui_selftest(pass, fail);   /* W418 LAUNCHER-ACCOUNT-UX (Fall 75c) */
            onl_gui_selftest(pass, fail); /* W334 LOC-SEC: Online (steelnet) (Fall 77) */
            smooth_gui_selftest(pass, fail); /* W387 LOC-SMOOTH-OPP: Gegner glaetten (Fall 77b) */
            lnet_gui_selftest(pass, fail);   /* W392 LAUNCHER-NETTAB: Seite Netzwerk (Fall 77c) */
            privacy_gui_selftest(pass, fail); /* W450 LAUNCHER-PRIVACY-INFO (Fall 77d) */
            hub_gui_selftest(pass, fail); /* W470 LAUNCHER-HUB (Fall 77e) */
            mppolish_gui_selftest(pass, fail); /* W473 LAUNCHER-MP-POLISH (Fall 77f) */
            /* --- 64 (W259 SETTINGS-RESET): Launcher bleibt offen, im Spiel wird
             * per ESC "Als Standard speichern" gesichert, danach startet derselbe
             * Launcher erneut. Vorher schrieb er seine alten Werte zurueck
             * (light_world 150 -> 100); jetzt bleiben Spielwerte, eigene
             * Aenderungen des Launchers gewinnen, Client-Ende zeigt sie an. --- */
            {
                char why64[200] = "", f64[1300], f64b[1300];
                static char b64[16384];
                launcher_settings_t a64, o64;
                int n64;
                _snprintf_s(f64, sizeof f64, _TRUNCATE, "%sw259_user_%lu.cfg", tmp, GetCurrentProcessId());
                _snprintf_s(f64b, sizeof f64b, _TRUNCATE, "%sw259_old_%lu.cfg", tmp, GetCurrentProcessId());
                u8_DeleteFile(f64);
                lcfg_defaults(&a64);
                if (lcfg_save_user(f64, &a64) != 0) {
                    strcpy_s(why64, sizeof why64, "user.cfg anlegen");
                }
                strcpy_s(g_ucfg_test, sizeof g_ucfg_test, f64);
                g_s = a64; /* Launcher-Fenster mit diesem Stand geoeffnet */
                ui_from_settings(&g_s);
                disk_base_sync(&g_s);
                /* 1. Launch (unveraendert) -> Spiel -> ESC speichert */
                if (!why64[0] && save_user_merged(f64) != 0) {
                    strcpy_s(why64, sizeof why64, "1. Start: unerwartete Uebernahme");
                }
                gt_cfg_set(f64, "light_world", "150");
                gt_cfg_set(f64, "color_sat", "120");
                gt_cfg_set(f64, "vr_shell", "0"); /* nicht verwaltet (W242) */
                /* Vorher-Beleg: alter Weg (lcfg_save_user mit Fensterwerten) */
                gt_read(f64, b64, sizeof b64);
                write_file(f64b, b64);
                o64 = g_s;
                ui_to_settings(&o64);
                lcfg_save_user(f64b, &o64);
                gt_read(f64b, b64, sizeof b64);
                if (!why64[0] && (!strstr(b64, "\nlight_world=100\n") || !strstr(b64, "\ncolor_sat=100\n"))) {
                    strcpy_s(why64, sizeof why64, "alter Weg setzt nicht zurueck (Testaufbau?)");
                }
                u8_DeleteFile(f64b);
                /* 2. Launch aus demselben Fenster: Spielwerte bleiben */
                ui_to_settings(&g_s);
                n64 = save_user_merged(f64);
                gt_read(f64, b64, sizeof b64);
                if (!why64[0] && (n64 != 2 || !strstr(b64, "\nlight_world=150\n") ||
                                  !strstr(b64, "\ncolor_sat=120\n") || !strstr(b64, "\nvr_shell=0\n"))) {
                    _snprintf_s(why64, sizeof why64, _TRUNCATE, "2. Start: Spielwerte verloren (n=%d)", n64);
                }
                if (!why64[0] && (g_gfx.shx[LSHX_LWORLD] != 150 || g_s.shx[LSHX_SAT] != 120)) {
                    strcpy_s(why64, sizeof why64, "Oberflaeche zeigt Spielwerte nicht");
                }
                /* 3. Launcher-Aenderung gewinnt, andere Spielwerte bleiben */
                gt_cfg_set(f64, "light_world", "170");
                gt_cfg_set(f64, "color_sat", "130");
                g_gfx.shx[LSHX_LWORLD] = 80; /* im Launcher geaendert */
                ui_to_settings(&g_s);
                n64 = save_user_merged(f64);
                gt_read(f64, b64, sizeof b64);
                if (!why64[0] && (n64 != 1 || !strstr(b64, "\nlight_world=80\n") ||
                                  !strstr(b64, "\ncolor_sat=130\n"))) {
                    _snprintf_s(why64, sizeof why64, _TRUNCATE, "3. Start: Vorrang falsch (n=%d)", n64);
                }
                /* 4. Client beendet: Launcher zeigt den neuen Spielwert sofort */
                gt_cfg_set(f64, "light_world", "60");
                n64 = disk_refresh();
                if (!why64[0] && (n64 != 1 || g_gfx.shx[LSHX_LWORLD] != 60 || g_s.shx[LSHX_LWORLD] != 60)) {
                    _snprintf_s(why64, sizeof why64, _TRUNCATE, "Client-Ende: nicht nachgeladen (n=%d)", n64);
                }
                ui_to_settings(&g_s);
                n64 = save_user_merged(f64);
                gt_read(f64, b64, sizeof b64);
                if (!why64[0] && (n64 != 0 || !strstr(b64, "\nlight_world=60\n"))) {
                    _snprintf_s(why64, sizeof why64, _TRUNCATE, "nach Client-Ende: Wert nicht stabil (n=%d)", n64);
                }
                u8_DeleteFile(f64);
                g_ucfg_test[0] = '\0';
                g_disk_base_ok = 0;
                lcfg_defaults(&a64);
                g_s = a64;
                ui_from_settings(&g_s);
                if (!why64[0]) {
                    GT_OK("64 W259 settings-reset (ESC speichert -> gleicher Launcher startet erneut -> Wert bleibt; Launcher-Aenderung gewinnt; Client-Ende laedt nach)");
                } else {
                    GT_FAIL("64 W259 settings-reset", "%s", why64);
                }
            }
            ShowWindow(g_hwnd, SW_HIDE);
            gui_teardown();
        }
        g_env_fix = NULL;
        if (had_env) {
            gt_setenv("SB_PACKS_DIR", senv);
        } else {
            SetEnvironmentVariableA("SB_PACKS_DIR", NULL);
        }
        g_pk_n = 0;
        { /* Test-Packs entfernen */
            static const char *const f[] = { "ta/a.dds", "ta/manifest.json",
                "tb/a.dds", "tb/manifest.json", "fx/manifest.json", "fx/params.json",
                "packs.json" };
            static const char *const d[] = { "ta", "tb", "fx", "bad", "" };
            wchar_t w[1300];
            int k;
            for (k = 0; k < (int)(sizeof f / sizeof f[0]); k++) {
                _snprintf_s(q, sizeof q, _TRUNCATE, "%s/%s", pkd, f[k]);
                u8_DeleteFile(q);
            }
            for (k = 0; k < (int)(sizeof d / sizeof d[0]); k++) {
                _snprintf_s(q, sizeof q, _TRUNCATE, "%s/%s", pkd, d[k]);
                u8_to_w(q, w, 1300);
                RemoveDirectoryW(w);
            }
            _snprintf_s(q, sizeof q, _TRUNCATE, "%s/packs.json", pke);
            u8_DeleteFile(q);
            u8_to_w(pke, w, 1300);
            RemoveDirectoryW(w);
        }
        if (ok48) {
            GT_OK("48 play-setup (Einrichtungskarte, Sprunglinks)");
        } else {
            GT_FAIL("48 play-setup", "%s", why48);
        }
        if (ok62) {
            GT_OK("62 play-layout (Einrichtungskarte je Zustand: Sichtbarkeit, Bild, Pfad, Texte, Ueberlappung, Durchsuchen)");
        } else {
            GT_FAIL("62 play-layout", "%s", why62);
        }
        if (ok49) {
            GT_OK("49 packs-ui (Liste, Reihenfolge, Konflikte, Grenzen, abgelehnt, leer, B5)");
        } else {
            GT_FAIL("49 packs-ui", "%s", why49);
        }
        if (ok50) {
            GT_OK("50 advanced (Datenort wie --userdata, beim Start laden)");
        } else {
            GT_FAIL("50 advanced", "%s", why50);
        }
        if (ok51) {
            GT_OK("51 tooltips (Begruendung gesperrter Elemente)");
        } else {
            GT_FAIL("51 tooltips", "%s", why51);
        }
    }

    /* 23 (W178 COMM-BG: beide Editionen; W162: groessen-unabhaengig):
     * Hintergrundbild: Ressource dekodiert
     * (beliebige Quellgroesse, z.B. 1672x941 seit 6.11.), Zuschnitt ohne
     * Strecken (Ausschnitt-Seitenverhaeltnis = Ziel-Seitenverhaeltnis, im
     * Quellbild), ausgeblichen (dunkler als die Quelle). */
    {
        int ok = 1;
        WICRect cr;
        int sizes[4][2] = { { 960, 640 }, { 1440, 960 }, { 400, 900 },
                                { 1920, 1080 } };
        int i;
        if (!bg_load() || g_img_w <= 0 || g_img_h <= 0) {
            ok = 0;
        }
        for (i = 0; ok && i < 4; i++) {
            double da = (double)sizes[i][0] / sizes[i][1];
            double ca;
            bg_cover_rect(g_img_w, g_img_h, sizes[i][0], sizes[i][1], &cr);
            ca = (double)cr.Width / cr.Height;
            if (fabs(ca / da - 1.0) > 0.005 || cr.X < 0 || cr.Y < 0 ||
                cr.X + cr.Width > g_img_w || cr.Y + cr.Height > g_img_h) {
                ok = 0;
            }
        }
        if (ok && bg_scaled(960, 640)) {
            double ms = 0, md = 0;
            int k;
            for (k = 0; k < g_img_w * g_img_h; k += 7) {
                ms += (g_img[k] >> 8) & 255;
            }
            ms /= (g_img_w * g_img_h + 6) / 7;
            for (k = 0; k < 960 * 640; k += 7) {
                md += (g_bgs[k] >> 8) & 255;
            }
            md /= (960 * 640 + 6) / 7;
            if (!(md < ms * 0.6)) {
                ok = 0;
            }
            printf("     (23: Quelle %dx%d, Helligkeit %.0f -> %.0f)\n",
                   g_img_w, g_img_h, ms, md);
        } else {
            ok = 0;
        }
        if (ok) {
            GT_OK("23 bg-image");
        } else {
            GT_FAIL("23 bg-image", "%s", "Ressource/Zuschnitt/Abdunkeln");
        }
    }

    /* 24: Launcher-eigene Einstellung (Hintergrund an/aus) */
    {
        int ok = ui_cfg_load_bg(dir) == 1 && ui_cfg_save_bg(dir, 0) == 0 &&
                 ui_cfg_load_bg(dir) == 0 && ui_cfg_save_bg(dir, 1) == 0 &&
                 ui_cfg_load_bg(dir) == 1;
        ui_cfg_path(dir, p, sizeof p);
        u8_DeleteFile(p);
        if (ok) {
            GT_OK("24 ui-cfg");
        } else {
            GT_FAIL("24 ui-cfg", "%s", "bg_image nicht gespeichert");
        }
    }

    /* 25: Vorschau-Mathematik (nie strecken) + Offscreen-Position */
    {
        unsigned bw, bh;
        int ok = 1, x, y;
        aspect_box_for(2560, 1080, &bw, &bh);
        if (bw != 1440 || bh != 1080) {
            ok = 0;
        }
        aspect_box_for(1280, 1024, &bw, &bh);
        if (bw != 1280 || bh != 960) {
            ok = 0;
        }
        offscreen_pos(&x, &y);
        if (x < GetSystemMetrics(SM_XVIRTUALSCREEN) +
                    GetSystemMetrics(SM_CXVIRTUALSCREEN)) {
            ok = 0;
        }
        if (ok) {
            GT_OK("25 aspect+offscreen");
        } else {
            GT_FAIL("25 aspect+offscreen", "bw=%u bh=%u x=%d", bw, bh, x);
        }
    }
    /* 27 (W116 EDITIONS): Badge passt zur Edition; Remaster-Paket wird
     * nur mit textures/x2|x4|manual erkannt (auto: Vorfahr mit poc-native,
     * sonst remaster_dir aus user.cfg). */
    {
        int ok = 1, r0, r1, r2;
        char root[1100], cl[1200], rd[1024], q[1300];
        wchar_t w[1300];
        ok = strcmp(SB_EDITION_BADGE, SB_EDITION_COMMUNITY ? "COMMUNITY EDITION"
                                                            : "PRIVATE EDITION") == 0;
        _snprintf_s(root, sizeof root, _TRUNCATE, "%sw116_rem_test", tmp);
        _snprintf_s(cl, sizeof cl, _TRUNCATE, "%s/client", root);
        _snprintf_s(q, sizeof q, _TRUNCATE, "%s/poc-native", root);
        lcfg_mkdir_p(cl);
        lcfg_mkdir_p(q);
        r0 = rem_pkg_detect(cl, rd, sizeof rd);
        _snprintf_s(q, sizeof q, _TRUNCATE, "%s/remaster/textures/x4", root);
        lcfg_mkdir_p(q);
        r1 = rem_pkg_detect(cl, rd, sizeof rd);
        if (!strstr(rd, "w116_rem_test/remaster")) {
            ok = 0;
        }
        _snprintf_s(q, sizeof q, _TRUNCATE, "%s/user.cfg", cl);
        write_file(q, "remaster_dir=w116_gibt_es_nicht/remaster\n");
        r2 = rem_pkg_detect(cl, rd, sizeof rd);
        u8_DeleteFile(q);
        if (r0 != 0 || r1 != 1 || r2 != 0) {
            ok = 0;
        }
        printf("     (27: %s, Paket ohne/mit/fremd = %d/%d/%d)\n", SB_EDITION_BADGE,
               r0, r1, r2);
        {
            static const char *const rm[] = { "remaster/textures/x4",
                                              "remaster/textures", "remaster",
                                              "poc-native", "client", "" };
            int k;
            for (k = 0; k < 6; k++) {
                _snprintf_s(q, sizeof q, _TRUNCATE, "%s/%s", root, rm[k]);
                u8_to_w(q, w, 1300);
                RemoveDirectoryW(w);
            }
        }
        if (ok) {
            GT_OK("27 edition+remaster-paket");
        } else {
            GT_FAIL("27 edition+remaster-paket", "%s", "Badge/Paketerkennung falsch");
        }
    }
    /* 53 (W179 KEYS-HTML): Browser-Export ohne Fenster (Fixture, kein
     * Fenster, nie oeffnen, nie Fokus): alle Abschnitte, Primaerfeuer,
     * Escaping (<&> im Profilnamen), keine Verweise. */
    {
        static char html[393216];
        char fx[1300], why53[300] = "";
        int ok53 = 1;
        const char *fprof = "w179_<t&>st";
        const char *ftext =
            "# fixture (keine Spieldaten)\n"
            "[buttons]\n"
            "bi0 = KEY:SPACE, MOUSE:LEFT, PAD:RB\n"
            "bi3 = PAD:BACK+PAD:Y # <tag> & \"anf\"\n";
        keyshtml_build(LT("gui.keys_memo"), fprof, ftext, NULL,
                       LT("gui.mouse_abs"), 4, "fixture-pad <x>",
                       "live & werte", "sbc <ok> & bereit",
                       html, sizeof html);
        _snprintf_s(fx, sizeof fx, _TRUNCATE, "%sw179_keyshtml.html", tmp);
        write_file(fx, html);
        { /* Datei-Roundtrip (wie keyshtml_open schreibt) */
            FILE *f = NULL;
            static char back[393216];
            size_t n = 0;
            if (u8_fopen_s(&f, fx, "rb") == 0 && f) {
                n = fread(back, 1, sizeof back - 1, f);
                fclose(f);
            }
            back[n] = '\0';
            if (n == 0 || strcmp(back, html) != 0) {
                ok53 = 0;
                strcpy_s(why53, sizeof why53, "Datei-Roundtrip");
            }
        }
        if (ok53) {
            static const char *const secs[] = {
                "gui.keys_h_menus", "gui.start_mech", "gui.keys_h_combat",
                "gui.keys_h_move", "gui.keys_h_cockpit", "gui.ctl_mouse",
                "gui.ctl_pad", "gui.ctl_device",
            };
            int k;
            for (k = 0; k < 8; k++) {
                if (!strstr(html, LT(secs[k]))) {
                    ok53 = 0;
                    _snprintf_s(why53, sizeof why53, _TRUNCATE,
                                "Abschnitt fehlt: %s", secs[k]);
                    break;
                }
            }
        }
        if (ok53 && (!strstr(html, LT("gui.ctl_prim")) ||
                     !strstr(html, LT("gui.ctl_prim_keys")) ||
                     !strstr(html, "class=\"prim\""))) {
            ok53 = 0;
            strcpy_s(why53, sizeof why53, "Primaerfeuer fehlt");
        }
        if (ok53 && (!strstr(html, "w179_&lt;t&amp;&gt;st") ||
                     strstr(html, "w179_<t&>st") ||
                     !strstr(html, "&lt;tag&gt; &amp; &quot;anf&quot;") ||
                     !strstr(html, "fixture-pad &lt;x&gt;"))) {
            ok53 = 0;
            strcpy_s(why53, sizeof why53, "Escaping falsch");
        }
        if (ok53 && (strstr(html, "http") || strstr(html, "HTTP"))) {
            ok53 = 0;
            strcpy_s(why53, sizeof why53, "Verweis enthalten");
        }
        if (ok53 && (!strstr(html, SB_EDITION_COMMUNITY ? "Steelbound"
                                                        : "Steel Battalion") ||
                     !strstr(html, LT("gui.page_controls")))) {
            ok53 = 0;
            strcpy_s(why53, sizeof why53, "Titel/Edition falsch");
        }
        if (ok53) { /* Standardbelegung: kein Gamepad, Escaping (&) */
            keyshtml_build(LT("gui.keys_memo"), "", NULL, NULL,
                           LT("gui.mouse_abs"), 4, "", "", "",
                           html, sizeof html);
            if (strstr(html, LT("gui.ctl_pad")) ||
                !strstr(html, LT("gui.keys_h_menus")) ||
                !strstr(html, "&amp;")) {
                ok53 = 0;
                strcpy_s(why53, sizeof why53, "Standard-Variante falsch");
            }
        }
        if (ok53) { /* W211: default keys, remapping, disabled action and collisions. */
            static lctrl_bind_t bindings; /* W414: static (~31 KB + 20 KB, Stapelrahmen gui_selftest) */
            static char conflicts[128][160];
            int n, k;
            if (!strstr(html, LT("bind.feedback_shot")) || !strstr(html, "KEY:F11") ||
                !strstr(html, LT("bind.feedback_clip")) || !strstr(html, "KEY:F9")) {
                ok53 = 0;
                strcpy_s(why53, sizeof why53, "Feedback defaults missing");
            }
            lctrl_bind_init(&bindings);
            lctrl_bind_load_text(ks_legacy_text, &bindings);
            n = lctrl_bind_conflicts(&bindings, conflicts, 128);
            for (k = 0; k < n && k < 128; ++k) {
                if (strstr(conflicts[k], "feedback_")) {
                    ok53 = 0;
                    strcpy_s(why53, sizeof why53, "Feedback default collision");
                }
            }
            keyshtml_build(LT("gui.keys_memo"), "feedback_test", "feedback_shot = KEY:F7\n", NULL,
                           "", 4, "", "", "", html, sizeof html);
            if (!strstr(html, "KEY:F7") || strstr(html, "KEY:F11") || strstr(html, "KEY:F9") ||
                !strstr(html, LT("bind.feedback_shot")) || !strstr(html, LT("bind.feedback_clip"))) {
                ok53 = 0;
                strcpy_s(why53, sizeof why53, "Feedback export ignores profile");
            }
        }
        if (ok53) {
            GT_OK("53 keys-html (Abschnitte, Primaerfeuer, Escaping, kein http)");
        } else {
            GT_FAIL("53 keys-html", "%s", why53);
        }
        { /* Beispiel-HTML (englisch, Fixture) fuers Doku-Archiv */
            char keep[8];
            strcpy_s(keep, sizeof keep, sb_lang_code());
            sb_lang_set("en");
            keyshtml_build(sb_lang("gui.keys_memo"), fprof, ftext, NULL,
                           sb_lang("gui.mouse_abs"), 4, "fixture-pad <x>",
                           "live & werte", "sbc <ok> & bereit",
                           html, sizeof html);
            _snprintf_s(fx, sizeof fx, _TRUNCATE,
                        "%sw179_controls_example.html", tmp);
            write_file(fx, html);
            printf("     (53: Beispiel %s)\n", fx);
            sb_lang_set(keep);
        }
        _snprintf_s(fx, sizeof fx, _TRUNCATE, "%sw179_keyshtml.html", tmp);
        u8_DeleteFile(fx);
    }
    /* 54 (W182 KEYS-SVG): Controller-Zeichnung im Browser-Export (Fixture,
     * nie oeffnen, nie Fokus): SVG vorhanden, jedes Mapping-Element hat
     * Form + Callout + Tabellenzeile, Belegung je Element, Escaping, keine
     * Verweise; Profil- und Standard-Variante; eingebaute Standard-
     * belegung == harness/profiles/keyboard_default.ini (privat). */
    {
        static char html[393216], ftext[8192];
        char fx[1300], why54[300] = "";
        const char *unc = ks_uncovered();
        int ok54 = 1;
        if (unc) {
            ok54 = 0;
            _snprintf_s(why54, sizeof why54, _TRUNCATE,
                        "Mapping-Element ohne Zeichnung: %s", unc);
        }
        if (ok54) { /* Profil-Variante + Escaping-Probe */
            _snprintf_s(ftext, sizeof ftext, _TRUNCATE, "%s%s",
                        ks_fixture_text, "bi15 = KEY:A<&> # Probe\n");
            keyshtml_build(LT("gui.keys_memo"), "w182_<p&d>", ftext, NULL,
                           LT("gui.mouse_abs"), 4, "", "", "", html,
                           sizeof html);
            ok54 = ks_selfcheck(html, 1, why54, sizeof why54);
            if (ok54 && (!strstr(html, "A&lt;&amp;&gt;") ||
                         strstr(html, "A<&>") ||
                         !strstr(html, "w182_&lt;p&amp;d&gt;") ||
                         strstr(html, "w182_<p&d>"))) {
                ok54 = 0;
                strcpy_s(why54, sizeof why54, "Escaping falsch");
            }
        }
        if (ok54) { /* Standardbelegung (kein Profil) */
            keyshtml_build(LT("gui.keys_memo"), "", NULL, NULL,
                           LT("gui.mouse_abs"), 4, "", "", "", html,
                           sizeof html);
            ok54 = ks_selfcheck(html, 0, why54, sizeof why54);
            if (!ok54) {
                char t[300];
                _snprintf_s(t, sizeof t, _TRUNCATE, "Standard: %s", why54);
                strcpy_s(why54, sizeof why54, t);
            }
        }
        if (ok54) { /* W332 SEC (Fuzz-Fund keyshtml): Memo-Zeile mit ueberlangem Tastenfeld (Sprachdatei)
                     * -> Zeile verworfen, frueher strcpy_s-Abbruch des Launchers */
            static char m332[1200];
            char lk[200];
            memset(lk, 'K', 150);
            lk[150] = '\0';
            _snprintf_s(m332, sizeof m332, _TRUNCATE, "COMBAT\n%s\tW332 zu lang\nF9\tW332 ok\n", lk);
            keyshtml_build(m332, "", NULL, NULL, LT("gui.mouse_abs"), 4, "", "", "", html, sizeof html);
            if (!strstr(html, "W332 ok") || strstr(html, "W332 zu lang")) {
                ok54 = 0;
                strcpy_s(why54, sizeof why54, "W332: ueberlange Memo-Zeile nicht verworfen");
            }
        }
#ifdef SB_HARNESS_DIR_DEFAULT
        if (ok54) { /* eingebaut == Datei (nur privat, Datei vorhanden) */
            static lctrl_bind_t bf, bb;
            char kp[1200];
            _snprintf_s(kp, sizeof kp, _TRUNCATE,
                        "%s/../../profiles/keyboard_default.ini",
                        SB_HARNESS_DIR_DEFAULT);
            if (lctrl_bind_load_file(kp, &bf) == 0) {
                int r, a;
                lctrl_bind_load_text(ks_legacy_text, &bb);
                for (r = 0; r < LCTRL_ELEM_N && ok54; r++) {
                    if (bf.row[r].n_alt != bb.row[r].n_alt) {
                        ok54 = 0;
                    }
                    for (a = 0; ok54 && a < bf.row[r].n_alt; a++) {
                        ok54 = strcmp(bf.row[r].alt[a], bb.row[r].alt[a]) == 0;
                    }
                    if (!ok54) {
                        _snprintf_s(why54, sizeof why54, _TRUNCATE,
                                    "Standardbelegung != keyboard_default.ini "
                                    "(%s)", kCtrlElems[r].key);
                    }
                }
            } else {
                printf("     (54: %s fehlt, Abgleich uebersprungen)\n", kp);
            }
        }
#endif
        if (ok54) {
            GT_OK("54 keys-svg (Zeichnung, Callouts je Element, Escaping, "
                  "kein http, Profil + Standard)");
        } else {
            GT_FAIL("54 keys-svg", "%s", why54);
        }
        { /* 55 (W184 KEYS-LAYOUT): physische Anordnung, keine Ueberlappung */
            char why55[200] = "";
            if (ks_layoutcheck(why55, sizeof why55)) {
                GT_OK("55 keys-layout (Bloecke wie Original, Felder, keine "
                      "Callout-Ueberlappung)");
            } else {
                GT_FAIL("55 keys-layout", "%s", why55);
            }
        }
        { /* Beispiel-HTML (englisch, Fixture) fuers Doku-Archiv */
            char keep[8];
            strcpy_s(keep, sizeof keep, sb_lang_code());
            sb_lang_set("en");
            keyshtml_build(sb_lang("gui.keys_memo"), "w182_fixture",
                           ks_fixture_text, NULL, sb_lang("gui.mouse_abs"), 4,
                           "fixture: gamepad connected (port 0)", "",
                           "fixture: original controller not connected",
                           html, sizeof html);
            _snprintf_s(fx, sizeof fx, _TRUNCATE,
                        "%sw184_controls_example.html", tmp);
            write_file(fx, html);
            printf("     (54: Beispiel %s)\n", fx);
            sb_lang_set(keep);
        }
    }
    /* 56 (W185 ZADIG-HELP): Einrichtungsanleitung als lokale HTML-Seite
     * (nur bauen, nie oeffnen, nie Fokus): alle 6 Abschnitte, Warnung
     * mit 0A7B:D000, genau ein Link (<a href> auf die Zadig-Seite),
     * keine Skripte/externen Verweise, Escaping (& -> &amp;);
     * Datei-Roundtrip wie zadig_open schreibt; beide Sprachen. */
    {
        static char html[65536];
        static const char *const zsecs[] = {
            "gui.zadig_need_h", "gui.zadig_driver_h", "gui.zadig_launch_h",
            "gui.zadig_assist_h", "gui.zadig_start_h", "gui.zadig_fix_h",
        };
        static const char *const zbad[] = {
            "<script", "SCRIPT", "<link", "<img", "src=", "url(",
            "javascript:", "onload=", "onclick=", "onerror=",
        };
        static const char *const zlangs[] = { "en", "de" };
        char fx[1300], keep[8], why56[300] = "", e[2048], hl[32];
        int ok56 = 1, li, k;
        strcpy_s(keep, sizeof keep, sb_lang_code());
        for (li = 0; li < 2 && ok56; li++) {
            const char *q;
            int nlink = 0, nurl = 0;
            sb_lang_set(zlangs[li]);
            zadig_build(html, sizeof html);
            for (k = 0; k < 6 && ok56; k++) {
                keyshtml_escape(sb_lang(zsecs[k]), e, sizeof e);
                if (!strstr(html, e)) {
                    ok56 = 0;
                    _snprintf_s(why56, sizeof why56, _TRUNCATE,
                                "%s: Abschnitt fehlt: %s", zlangs[li],
                                zsecs[k]);
                }
            }
            if (ok56 && !strstr(html, "0A7B:D000")) {
                ok56 = 0;
                _snprintf_s(why56, sizeof why56, _TRUNCATE,
                            "%s: Warnung 0A7B:D000 fehlt", zlangs[li]);
            }
            if (ok56) { /* genau ein Link, sonst kein http */
                for (q = html; (q = strstr(q, "<a href=\"")) != NULL; q++) {
                    nlink++;
                }
                for (q = html; (q = strstr(q, ZADIG_URL)) != NULL; q++) {
                    nurl++;
                }
                if (nlink != 1 || nurl != 2 ||
                    !strstr(html, "<a href=\"" ZADIG_URL "\">" ZADIG_URL
                            "</a>")) {
                    ok56 = 0;
                    _snprintf_s(why56, sizeof why56, _TRUNCATE,
                                "%s: Link falsch (a=%d, url=%d)", zlangs[li],
                                nlink, nurl);
                }
            }
            if (ok56) { /* keine Skripte/externen Verweise */
                for (k = 0; k < 10 && ok56; k++) {
                    if (strstr(html, zbad[k])) {
                        ok56 = 0;
                        _snprintf_s(why56, sizeof why56, _TRUNCATE,
                                    "%s: verbotener Verweis: %s", zlangs[li],
                                    zbad[k]);
                    }
                }
            }
            if (ok56) { /* Escaping: jedes & gehoert zu einer Entitaet */
                for (q = html; (q = strchr(q, '&')) != NULL; q++) {
                    if (strncmp(q, "&amp;", 5) && strncmp(q, "&lt;", 4) &&
                        strncmp(q, "&gt;", 4) && strncmp(q, "&quot;", 6)) {
                        ok56 = 0;
                        _snprintf_s(why56, sizeof why56, _TRUNCATE,
                                    "%s: rohes &", zlangs[li]);
                        break;
                    }
                }
                if (ok56 && !strstr(html, "&amp;")) {
                    ok56 = 0; /* kein & im Text = Probe wirkungslos */
                    _snprintf_s(why56, sizeof why56, _TRUNCATE,
                                "%s: kein &amp; (Probe leer)", zlangs[li]);
                }
            }
            if (ok56) { /* Titel/Edition, Sprache, Fuss, Druck */
                keyshtml_escape(sb_lang("gui.zadig_title"), e, sizeof e);
                _snprintf_s(hl, sizeof hl, _TRUNCATE, "<html lang=\"%s\">",
                            sb_lang_code());
                if (!strstr(html, SB_EDITION_COMMUNITY ? "Steelbound"
                                                      : "Steel Battalion") ||
                    !strstr(html, e) || !strstr(html, hl) ||
                    !strstr(html, sb_lang("gui.keys_local")) ||
                    !strstr(html, "@media print")) {
                    ok56 = 0;
                    _snprintf_s(why56, sizeof why56, _TRUNCATE,
                                "%s: Titel/Sprache/Fuss/Druck falsch",
                                zlangs[li]);
                }
            }
        }
        if (ok56) { /* Datei-Roundtrip (wie zadig_open schreibt) */
            FILE *f = NULL;
            static char back[65536];
            size_t n = 0;
            _snprintf_s(fx, sizeof fx, _TRUNCATE, "%sw185_zadig_self.html",
                        tmp);
            write_file(fx, html);
            if (u8_fopen_s(&f, fx, "rb") == 0 && f) {
                n = fread(back, 1, sizeof back - 1, f);
                fclose(f);
            }
            back[n] = '\0';
            if (n == 0 || strcmp(back, html) != 0) {
                ok56 = 0;
                strcpy_s(why56, sizeof why56, "Datei-Roundtrip");
            }
            u8_DeleteFile(fx);
        }
        if (ok56) {
            GT_OK("56 zadig-guide (6 Abschnitte, Warnung 0A7B:D000, ein "
                  "Link, kein Skript, Escaping, en+de)");
        } else {
            GT_FAIL("56 zadig-guide", "%s", why56);
        }
        { /* Beispiel-HTML (englisch) fuers Doku-Archiv */
            sb_lang_set("en");
            zadig_build(html, sizeof html);
            _snprintf_s(fx, sizeof fx, _TRUNCATE,
                        "%sw185_zadig_guide.html", tmp);
            write_file(fx, html);
            printf("     (56: Beispiel %s)\n", fx);
            sb_lang_set(keep);
        }
    }
    { /* W202: synthetic certificate, no game bytes. Same folder check as launch. */
        unsigned char xb[0x300] = {0};
        uint32_t value;
        sb_gamever_t gv;
        launcher_settings_t cfg;
        char msg[1024], keep[16], iso[1300], acp[1300];
        int ok = 1, li, ri;
        const char *langs[] = { "en", "de" };
        const unsigned regions[] = { 1, 2, 4, 7, 0 };
        strcpy_s(keep, sizeof keep, sb_lang_code());
#define W202_PUT(off, v) do { value = (v); memcpy(xb + (off), &value, 4); } while (0)
        W202_PUT(0, 0x48454258u);
        W202_PUT(0x104, 0x10000u);
        W202_PUT(0x118, 0x10200u);
        W202_PUT(0x208, 0x43430002u);
        W202_PUT(0x2AC, 42u);
        for (li = 0; li < 2; li++) {
            sb_lang_set(langs[li]);
            for (ri = 0; ri < 5; ri++) {
                FILE *f = NULL;
                W202_PUT(0x2A0, regions[ri]);
                if (sb_gamever_check(xb, sizeof xb, &gv) || !gv.xbe_ok ||
                    gv.version != 42 || gv.region != regions[ri]) ok = 0;
                _snprintf_s(p, sizeof p, _TRUNCATE, "%s/default.xbe", dir);
                if (u8_fopen_s(&f, p, "wb") != 0 || !f) { ok = 0; continue; }
                if (fwrite(xb, 1, sizeof xb, f) != sizeof xb) ok = 0;
                fclose(f);
                lcfg_defaults(&cfg);
                cfg.game_image[0] = 0;
                strcpy_s(cfg.xbe_path, sizeof cfg.xbe_path, p);
                strcpy_s(cfg.media_root, sizeof cfg.media_root, dir);
                strcpy_s(cfg.hdd_root, sizeof cfg.hdd_root, dir);
                if (lcfg_validate(&cfg, msg, sizeof msg) == 0 ||
                    !strstr(msg, regions[ri] == 1 ? (li ? "Diese US-Fassung" : "This US copy") /* W231 */
                                                   : (li ? "Diese Spielversion" : "This game version")) ||
                    !strstr(msg, "43430002") || !strstr(msg, "42") ||
                    !strstr(msg, sb_gamever_region_name(regions[ri])) ||
                    !strstr(msg, "Steel Battalion (Europe, PAL)")) ok = 0;
                _snprintf_s(iso, sizeof iso, _TRUNCATE, "%s/unknown.iso", dir);
                u8_to_acp(iso, acp, sizeof acp);
                if (iso_vfs_make_test_image(acp, xb, sizeof xb) != 0 ||
                    lcfg_image_check(iso, msg, sizeof msg) != 4 ||
                    !strstr(msg, regions[ri] == 1 ? (li ? "Diese US-Fassung" : "This US copy") /* W231 */
                                                   : (li ? "Diese Spielversion" : "This game version")) ||
                    !strstr(msg, "43430002") || !strstr(msg, "42") ||
                    !strstr(msg, sb_gamever_region_name(regions[ri])) ||
                    !strstr(msg, "Steel Battalion (Europe, PAL)")) ok = 0;
                u8_DeleteFile(iso);
            }
        }
        W202_PUT(0x104, 0u);
        W202_PUT(0x118, 0xFFFFFFF0u); /* addition would wrap in a 32-bit parser */
        if (sb_gamever_check(xb, sizeof xb, &gv) || gv.xbe_ok) ok = 0;
        if (sb_gamever_check(xb, 16, &gv) || gv.xbe_ok) ok = 0;
#undef W202_PUT
        sb_lang_set(keep);
        if (ok) { GT_OK("61 unbekannte Version (Ordner/ISO, EN/DE, U/J/PAL/multi, Grenzen)"); }
        else { GT_FAIL("61 unbekannte Version", "%s", "Version/Meldung unvollstaendig"); }
    }
    { /* 64 W260 MOUSE-WHEEL: Lernmodus-Quelle, Anzeige EN/DE, Standard ohne Rad */
        char keep[8], t[256], why[128] = "";
        int ok = 1, src = 0, i, a, tu = lctrl_bind_elem_index("tuner.up");
        int td = lctrl_bind_elem_index("tuner.down");
        static lctrl_bind_t b; /* W414: static (~31 KB, Stapelrahmen gui_selftest) */
        char conf[8][160];
        static const struct { UINT m; int d; const char *want; } wc[] = {
            { WM_MOUSEWHEEL, 120, "MOUSE:WHEEL_UP" }, { WM_MOUSEWHEEL, -120, "MOUSE:WHEEL_DOWN" },
            { WM_MOUSEWHEEL, 30, "MOUSE:WHEEL_UP" }, { WM_MOUSEWHEEL, -240, "MOUSE:WHEEL_DOWN" },
            { WM_MOUSEHWHEEL, 120, "MOUSE:WHEEL_RIGHT" }, { WM_MOUSEHWHEEL, -120, "MOUSE:WHEEL_LEFT" },
        };
        for (i = 0; i < (int)(sizeof wc / sizeof wc[0]); i++) {
            const char *got = bind_wheel_src(wc[i].m, MAKEWPARAM(0, (WORD)(SHORT)wc[i].d));
            if (!got || strcmp(got, wc[i].want) != 0) {
                ok = 0;
                _snprintf_s(why, sizeof why, _TRUNCATE, "Lernmodus %d -> %s", i, got ? got : "NULL");
            }
        }
        if (bind_wheel_src(WM_MOUSEWHEEL, 0) || bind_wheel_src(WM_LBUTTONDOWN, MAKEWPARAM(0, 120))) {
            ok = 0;
            strcpy_s(why, sizeof why, "Lernmodus ohne Raste liefert Quelle");
        }
        strcpy_s(keep, sizeof keep, sb_lang_code());
        sb_lang_set("en");
        ks_atom("MOUSE:WHEEL_UP", t, sizeof t, &src);
        if (strcmp(t, "Mouse wheel up") != 0 || src != KS_SRC_MOUSE) { ok = 0; strcpy_s(why, sizeof why, "EN WHEEL_UP"); }
        ks_atom("MOUSE:WHEEL_RIGHT", t, sizeof t, &src);
        if (strcmp(t, "Mouse wheel right") != 0) { ok = 0; strcpy_s(why, sizeof why, "EN WHEEL_RIGHT"); }
        bind_src_disp("MOUSE:WHEEL_DOWN", t, sizeof t);
        if (strcmp(t, "MOUSE:WHEEL_DOWN  (Mouse wheel down)") != 0) { ok = 0; strcpy_s(why, sizeof why, "EN Liste"); }
        bind_src_disp("KEY:T", t, sizeof t);
        if (strcmp(t, "KEY:T") != 0) { ok = 0; strcpy_s(why, sizeof why, "Liste ohne Rad veraendert"); }
        sb_lang_set("de");
        ks_atom("MOUSE:WHEEL_DOWN", t, sizeof t, &src);
        if (strcmp(t, "Mausrad runter") != 0) { ok = 0; strcpy_s(why, sizeof why, "DE WHEEL_DOWN"); }
        bind_src_disp("mouse:wheel_up", t, sizeof t);
        if (strcmp(t, "mouse:wheel_up  (Mausrad hoch)") != 0) { ok = 0; strcpy_s(why, sizeof why, "DE Liste klein"); }
        sb_lang_set(keep);
        /* Standard: kein Rad belegt; Rad als Quelle setzbar, Konflikt erkannt. */
        lctrl_bind_init(&b);
        for (i = 0; i < LCTRL_ELEM_N; i++) {
            for (a = 0; a < b.row[i].n_alt; a++) {
                if (strstr(b.row[i].alt[a], "WHEEL")) { ok = 0; strcpy_s(why, sizeof why, "Standard belegt das Rad"); }
            }
        }
        if (tu < 0 || td < 0 || lctrl_bind_set(&b, tu, "KEY:T, MOUSE:WHEEL_UP") != 0 ||
            b.row[tu].n_alt != 2 || strcmp(b.row[tu].alt[1], "MOUSE:WHEEL_UP") != 0) {
            ok = 0; strcpy_s(why, sizeof why, "Rad-Quelle nicht setzbar");
        }
        if (td >= 0 && lctrl_bind_set(&b, td, "KEY:G, MOUSE:WHEEL_UP") == 0 &&
            lctrl_bind_conflicts(&b, conf, 8) < 1) {
            ok = 0; strcpy_s(why, sizeof why, "Konflikt Rad doppelt nicht erkannt");
        }
        if (ok) { GT_OK("64 W260 mouse-wheel (Lernmodus hoch/runter/links/rechts, Anzeige EN/DE, Standard ohne Rad, Konflikt)"); }
        else { GT_FAIL("64 W260 mouse-wheel", "%s", why); }
    }
    _snprintf_s(p, sizeof p, _TRUNCATE, "%s/default.xbe", dir);
    u8_DeleteFile(p);
    {
        wchar_t w[1300];
        _snprintf_s(p, sizeof p, _TRUNCATE, "%s/profiles", dir);
        u8_to_w(p, w, 1300);
        RemoveDirectoryW(w);
        u8_to_w(dir, w, 1300);
        RemoveDirectoryW(w);
    }
}

/* ------------------------------------------------------------------ */
/* W282 ISSUE6: Selbsttest-Faelle zu GitHub-Issue #6                   */
/* ------------------------------------------------------------------ */
#define W282_OK(name) do { printf("PASS %s\n", name); (*pass)++; } while (0)
#define W282_FAIL(name, why) do { printf("FAIL %s: %s\n", name, why); (*fail)++; } while (0)

/* Punkt 2: Kurznamen im Controller-Test folgen der Funktion (bi0 =
 * Report "MainWeapon" = Hauptwaffe/Daumentaste oben, bi1 = Report "Fire"
 * = Sub-Waffe/Abzug), Zeichnung: bi0 oben, bi1 vorn darunter. */
static void w282_sbc_labels(int *pass, int *fail)
{
    char why[160] = "";
    const ks_elem_t *e0 = NULL, *e1 = NULL;
    int i;
    for (i = 0; i < (int)(sizeof ks_elems / sizeof ks_elems[0]); i++) {
        if (strcmp(ks_elems[i].id, "bi0") == 0) e0 = &ks_elems[i];
        if (strcmp(ks_elems[i].id, "bi1") == 0) e1 = &ks_elems[i];
    }
    if (strcmp(kBtnShort[0], "MAIN") != 0 || strcmp(kBtnShort[1], "SUB") != 0) {
        _snprintf_s(why, sizeof why, _TRUNCATE, "Kurznamen '%s'/'%s'", kBtnShort[0], kBtnShort[1]);
    } else if (!lctrl_sbc_btn_name(0) || strcmp(lctrl_sbc_btn_name(0), "MAINWEAPON") != 0 ||
               !lctrl_sbc_btn_name(1) || strcmp(lctrl_sbc_btn_name(1), "SUBWEAPON") != 0) {
        strcpy_s(why, sizeof why, "SBC-Quellnamen bi0/bi1");
    } else if (strcmp(kCtrlElems[0].key, "bi0") != 0 || strcmp(kCtrlElems[0].lkey, "bind.bi0") != 0 ||
               strcmp(kCtrlElems[1].key, "bi1") != 0 || strcmp(kCtrlElems[1].lkey, "bind.bi1") != 0) {
        strcpy_s(why, sizeof why, "Elementtabelle bi0/bi1");
    } else if (!e0 || !e1 || e0->y >= e1->y || strcmp(e0->lkey, "bind.bi0") != 0 ||
               strcmp(e1->lkey, "bind.bi1") != 0) {
        strcpy_s(why, sizeof why, "Zeichnung: Hauptwaffe nicht oben bzw. Namen vertauscht");
    } else {
        const char *keep = sb_lang_code();
        char k[8];
        strcpy_s(k, sizeof k, keep);
        sb_lang_set("en");
        if (strcmp(LT("bind.bi0"), "Main weapon") != 0 || strcmp(LT("bind.bi1"), "Sub-weapon") != 0)
            strcpy_s(why, sizeof why, "Texte bind.bi0/bi1 en");
        sb_lang_set(k);
    }
    if (!why[0]) W282_OK("W282 ISSUE6 sbc-labels (bi0 MAIN = Hauptwaffe/Daumentaste oben, bi1 SUB = Sub-Waffe/Abzug, Funktion unveraendert)");
    else W282_FAIL("W282 ISSUE6 sbc-labels", why);
}

/* Punkt 3: ungueltiges Abbild sperrt die Ordnerwahl unter Erweitert nicht
 * mehr (echter Regelweg, kein Fixture-Umfeld); ein getippter/gewaehlter
 * Ordner loest das ungueltige Abbild ab; gueltiges Abbild sperrt weiter
 * (R1), ein danach ungueltiges gibt wieder frei - alles ohne Neustart. */
static void w282_iso_recover(const char *client_override, int *pass, int *fail)
{
    char tmp[1024], dir[1100], bad[1200], xbe[1200], t[1200], why[200] = "";
    launcher_settings_t s;
    u8_GetTempPath(tmp, sizeof tmp);
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%sw282_iso_folder", tmp);
    lcfg_mkdir_p(dir);
    _snprintf_s(xbe, sizeof xbe, _TRUNCATE, "%s/default.xbe", dir);
    write_file(xbe, "XBEH");
    _snprintf_s(bad, sizeof bad, _TRUNCATE, "%sw282_kein_spiel.iso", tmp);
    write_file(bad, "das ist kein Xbox-Abbild");
    if (!(gui_init(client_override) && gui_create(1))) {
        strcpy_s(why, sizeof why, "Fenster nicht erzeugt");
    } else {
        build_controls();
        tips_build();
        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE); /* ausserhalb des Bildschirms */
        g_env_fix = NULL; /* echter Regelweg: lcfg_validate auf dem Abbild */
        lcfg_defaults(&s);
        s.xbe_path[0] = s.media_root[0] = 0;
        strcpy_s(s.game_image, sizeof s.game_image, bad);
        show_page(P_ADV);
        ui_from_settings(&s);
        ui_apply_rules();
        pump();
        if (g_data_ok || !g_rr[3].active) {
            strcpy_s(why, sizeof why, "ungueltiges Abbild nicht abgelehnt");
        } else if (g_rr[1].disable || !IsWindowEnabled(ctl(ID_MEDIA)) ||
                   !IsWindowEnabled(ctl(ID_MEDIA_B)) || !IsWindowEnabled(ctl(ID_XBE_B))) {
            strcpy_s(why, sizeof why, "Ordnerwahl bei ungueltigem Abbild gesperrt (Befund Issue #6)");
        }
        if (!why[0]) { /* Ordner tippen + Feld verlassen = loest Abbild ab */
            ui_settext(ctl(ID_MEDIA), dir);
            on_command(ID_MEDIA, EN_KILLFOCUS);
            pump();
            ui_gettext(ctl(ID_IMAGE), t, sizeof t);
            if (t[0] || g_cur.game_image[0] || strcmp(g_cur.media_root, dir) != 0) {
                _snprintf_s(why, sizeof why, _TRUNCATE, "Abbild nicht abgeloest ('%.60s')", t);
            } else if (g_rr[1].active || !IsWindowEnabled(ctl(ID_MEDIA))) {
                strcpy_s(why, sizeof why, "Ordner-Modus nach Abloesung gesperrt");
            }
        }
        if (!why[0]) { /* gueltiges Abbild (Fixture) sperrt weiter ... */
            w220_fixture(W220_ISO, 0);
            ui_settext(ctl(ID_IMAGE), W220_ISO);
            on_command(ID_IMAGE_C, BN_CLICKED);
            pump();
            if (!g_data_ok || !g_rr[1].disable || IsWindowEnabled(ctl(ID_MEDIA_B))) {
                strcpy_s(why, sizeof why, "gueltiges Abbild sperrt den Ordner-Modus nicht mehr");
            } else {
                on_command(ID_MEDIA, EN_KILLFOCUS); /* gueltiges Abbild bleibt */
                ui_gettext(ctl(ID_IMAGE), t, sizeof t);
                if (strcmp(t, W220_ISO) != 0) {
                    strcpy_s(why, sizeof why, "gueltiges Abbild wurde geleert");
                }
            }
        }
        if (!why[0]) { /* ... und ein danach ungueltiges gibt ohne Neustart frei */
            ui_settext(ctl(ID_IMAGE), bad);
            on_command(ID_IMAGE_C, BN_CLICKED);
            pump();
            if (g_data_ok || g_rr[1].disable || !IsWindowEnabled(ctl(ID_MEDIA_B))) {
                strcpy_s(why, sizeof why, "Ordnerwahl erholt sich nicht ohne Neustart");
            }
        }
        g_imgfix_path[0] = 0;
        ShowWindow(g_hwnd, SW_HIDE);
        gui_teardown();
    }
    u8_DeleteFile(bad);
    u8_DeleteFile(xbe);
    {
        wchar_t w[1200];
        u8_to_w(dir, w, 1200);
        RemoveDirectoryW(w);
    }
    if (!why[0]) W282_OK("W282 ISSUE6 iso-recover (ungueltiges Abbild sperrt Ordnerwahl nicht, Ordner loest es ab, gueltiges sperrt weiter, Erholung ohne Neustart)");
    else W282_FAIL("W282 ISSUE6 iso-recover", why);
}

/* Punkte 6+7: Belegungsdialog (offscreen, Temp-Client-Ordner). */
static char g_w282_why[200];
static int g_w282_stage;
static char g_w282_prof[1200];

static void w282_bind_probe(HWND hd)
{
    char t[128];
    int i, bad = 0;
    lctrl_bind_t rb;
    ui_gettext(bctl(hd, IDB_NAME), t, sizeof t);
    if (g_w282_stage == 2) { /* mitgeliefertes Layout: Feld bleibt leer */
        if (t[0]) {
            _snprintf_s(g_w282_why, sizeof g_w282_why, _TRUNCATE,
                        "Name bei keyboard_default vorbelegt ('%s')", t);
        }
        return;
    }
    if (strcmp(t, "w282Test2") != 0) { /* Punkt 6 */
        _snprintf_s(g_w282_why, sizeof g_w282_why, _TRUNCATE,
                    "Name nach Oeffnen '%s' statt 'w282Test2'", t);
        return;
    }
    if (!bctl(hd, IDB_CLEAR) || !IsWindowEnabled(bctl(hd, IDB_CLEAR))) {
        strcpy_s(g_w282_why, sizeof g_w282_why, "Knopf Alle leeren fehlt");
        return;
    }
    SendMessageW(hd, WM_COMMAND, MAKEWPARAM(IDB_CLEAR, BN_CLICKED),
                 (LPARAM)bctl(hd, IDB_CLEAR)); /* Punkt 7 */
    for (i = 0; i < LCTRL_ELEM_N; i++) {
        if (kCtrlElems[i].kind != LCTRL_ACT && g_bind.row[i].n_alt) bad++;
    }
    i = lctrl_bind_elem_index("pause");
    if (bad || i < 0 || g_bind.row[i].n_alt != 1 || g_bind.joy[0][0]) {
        _snprintf_s(g_w282_why, sizeof g_w282_why, _TRUNCATE,
                    "Leeren: %d Spiel-Elemente belegt, pause %d, joy0 '%s'", bad,
                    i >= 0 ? g_bind.row[i].n_alt : -1, g_bind.joy[0]);
        return;
    }
    ui_gettext(bctl(hd, IDB_NAME), t, sizeof t);
    if (strcmp(t, "w282Test2") != 0) {
        strcpy_s(g_w282_why, sizeof g_w282_why, "Name nach Leeren verloren");
        return;
    }
    if (bind_save(hd) != 0) { /* gleicher Name: ohne Rueckfrage */
        strcpy_s(g_w282_why, sizeof g_w282_why, "Speichern unter dem geoeffneten Namen scheiterte");
        return;
    }
    lctrl_bind_load_file(g_w282_prof, &rb);
    for (i = 0, bad = 0; i < LCTRL_ELEM_N; i++) {
        if (kCtrlElems[i].kind != LCTRL_ACT && rb.row[i].n_alt) bad++;
    }
    i = lctrl_bind_elem_index("pause");
    if (bad || rb.row[i].n_alt != 1 || strcmp(rb.row[i].alt[0], "KEY:PAUSE") != 0 ||
        rb.joy[0][0]) {
        _snprintf_s(g_w282_why, sizeof g_w282_why, _TRUNCATE,
                    "Datei nach Speichern: %d Spiel-Elemente, pause %d", bad, rb.row[i].n_alt);
    }
}

static void w282_bind_dialog(const char *client_override, int *pass, int *fail)
{
    char tmp[1024], dir[1100], pd[1200], keepdir[sizeof g_client_dir];
    gui_opts_t keepo = g_o;
    u8_GetTempPath(tmp, sizeof tmp);
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%sw282_bind", tmp);
    _snprintf_s(pd, sizeof pd, _TRUNCATE, "%s/profiles", dir);
    lcfg_mkdir_p(pd);
    _snprintf_s(g_w282_prof, sizeof g_w282_prof, _TRUNCATE, "%s/user_w282Test2.ini", pd);
    write_file(g_w282_prof, "[joystick]\njoy0 = Example Stick\n[buttons]\n"
                            "bi0 = KEY:SPACE, JOY0:B1\nbi1 = KEY:F\n[axes]\n"
                            "rot = JOY0:RZ:dz=0.05\n[client]\npause = KEY:PAUSE\n");
    g_w282_why[0] = 0;
    if (!(gui_init(client_override) && gui_create(1))) {
        strcpy_s(g_w282_why, sizeof g_w282_why, "Fenster nicht erzeugt");
    } else {
        build_controls();
        tips_build();
        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE); /* ausserhalb des Bildschirms */
        strcpy_s(keepdir, sizeof keepdir, g_client_dir);
        strcpy_s(g_client_dir, sizeof g_client_dir, dir); /* nie das echte Profil */
        g_o.shot = 3; /* Dialog offscreen, ohne Aktivierung, ohne Rueckfragen */
        g_o.bind_sel = -1;
        g_o.bind_learn = g_o.bind_joydemo = g_o.bind_stickdemo = g_o.bind_wheeldemo = g_o.bind_combodemo = 0;
        g_bind_testfn = w282_bind_probe;
        g_w282_stage = 1;
        bind_dialog_modal("user_w282Test2", g_w282_prof);
        if (!g_w282_why[0]) {
            g_w282_stage = 2;
            bind_dialog_modal("keyboard_default", NULL);
        }
        g_bind_testfn = NULL;
        g_o = keepo;
        strcpy_s(g_client_dir, sizeof g_client_dir, keepdir);
        ShowWindow(g_hwnd, SW_HIDE);
        gui_teardown();
    }
    { /* Pure Helfer: Namensableitung */
        char r[64];
        if (!g_w282_why[0] &&
            (!lctrl_user_rest_of("user_abc-1", r, sizeof r) || strcmp(r, "abc-1") != 0 ||
             lctrl_user_rest_of("keyboard_default", r, sizeof r) || r[0] ||
             lctrl_user_rest_of("user_", r, sizeof r) || lctrl_user_rest_of("user_a b", r, sizeof r) ||
             lctrl_user_rest_of(NULL, r, sizeof r))) {
            strcpy_s(g_w282_why, sizeof g_w282_why, "lctrl_user_rest_of");
        }
    }
    u8_DeleteFile(g_w282_prof);
    {
        wchar_t w[1200];
        u8_to_w(pd, w, 1200);
        RemoveDirectoryW(w);
        u8_to_w(dir, w, 1200);
        RemoveDirectoryW(w);
    }
    if (!g_w282_why[0]) W282_OK("W282 ISSUE6 bind-name+empty (Name des geoeffneten Layouts bleibt, Alle leeren: Spiel-Elemente leer, Client-Tasten bleiben, joyN entfaellt, Speichern ohne Rueckfrage, Datei leer)");
    else W282_FAIL("W282 ISSUE6 bind-name+empty", g_w282_why);
}

/* Punkte 4+5: Mehrgeraete-Achsen mit simulierten DirectInput-Zustaenden.
 * Geraet A = Arduino-artig (Standardbereich, Mitte 0), Geraet B =
 * Wingman-artig (Treiber nimmt DIPROP_RANGE nicht an: Rohbereich
 * 0..65535 bzw. 0..1023). Beide Lerner laufen wie bind_cap_joy je Takt. */
static int w282_learn2(long blo, long bhi, int bmove, int normalize, char *out, size_t cap,
                       int *who)
{
    lctrl_joy_learner_t la, lb;
    imap_joy_state_t sa, sb;
    int idx = lctrl_bind_elem_index("aim_x"), t, r;
    long bc = (blo + bhi) / 2, bv;
    lctrl_joy_learner_reset(&la);
    lctrl_joy_learner_reset(&lb);
    *who = -1;
    out[0] = 0;
    for (t = 0; t < 20; t++) {
        memset(&sa, 0, sizeof sa);
        memset(&sb, 0, sizeof sb);
        memset(sa.pov, 0xFF, sizeof sa.pov);
        memset(sb.pov, 0xFF, sizeof sb.pov);
        sa.axis[0] = 120; sa.axis[1] = -80; sa.axis[2] = -32768; /* A ruht */
        bv = (bmove && t >= 6) ? bhi : bc; /* B: X ab Takt 6 ganz rechts */
        if (normalize) {
            sb.axis[0] = ijoy_norm_axis(bv, blo, bhi);
            sb.axis[1] = ijoy_norm_axis(bc, blo, bhi);
            sb.axis[6] = ijoy_norm_axis(blo, blo, bhi); /* Slider am Anschlag */
        } else { /* alter Weg: Rohwert nur auf int16 begrenzt */
            sb.axis[0] = (int16_t)(bv > 32767 ? 32767 : bv < -32768 ? -32768 : bv);
            sb.axis[1] = (int16_t)(bc > 32767 ? 32767 : bc < -32768 ? -32768 : bc);
            sb.axis[6] = (int16_t)(blo > 32767 ? 32767 : blo < -32768 ? -32768 : blo);
        }
        r = lctrl_joy_learner_step(&la, 1, &sa, 0x07, 0, idx, out, cap);
        if (r) { *who = 0; return r; }
        r = lctrl_joy_learner_step(&lb, 1, &sb, 0x43, 0, idx, out, cap);
        if (r) { *who = 1; return r; }
    }
    return 0;
}

static void w282_joy_multi(int *pass, int *fail)
{
    char why[200] = "", out[64];
    int who, r, slot[16], i;
    { /* Bereichs-Umrechnung */
        if (ijoy_norm_axis(-40000, -32768, 32767) != -32768 || ijoy_norm_axis(1234, -32768, 32767) != 1234 ||
            ijoy_norm_axis(40000, -32768, 32767) != 32767)
            strcpy_s(why, sizeof why, "Normalbereich nicht bitgleich");
        else if (ijoy_norm_axis(0, 0, 255) != -32768 || ijoy_norm_axis(255, 0, 255) != 32767 ||
                 abs(ijoy_norm_axis(128, 0, 255)) > 200 || ijoy_norm_axis(65535, 0, 65535) != 32767 ||
                 abs(ijoy_norm_axis(32768, 0, 65535)) > 1 || ijoy_norm_axis(5, 7, 7) != 0)
            strcpy_s(why, sizeof why, "Umrechnung 0..255 / 0..65535");
    }
    if (!why[0]) { /* Achsen-Zuordnung (Rueckfall bei unbekannten GUIDs) */
        static const int k1[4] = {0, 1, 7, 6}; /* X Y unbekannt Slider */
        static const int k2[3] = {0, 0, 1};    /* doppelte X-Achse */
        static const int k3[10] = {0, 1, 2, 3, 4, 5, 6, 6, 7, 7};
        unsigned m1 = ijoy_plan_axes(k1, 4, slot);
        if (m1 != 0xC3 || slot[0] != 0 || slot[1] != 1 || slot[2] != 7 || slot[3] != 6)
            strcpy_s(why, sizeof why, "Plan X/Y/unbekannt/Slider");
        else if (ijoy_plan_axes(k2, 3, slot) != 0x43 || slot[1] != 6)
            strcpy_s(why, sizeof why, "Plan doppelte Achse");
        else if (ijoy_plan_axes(k3, 10, slot) != 0xFF || slot[8] != -1 || slot[9] != -1)
            strcpy_s(why, sizeof why, "Plan mehr als 8 Achsen");
    }
    if (!why[0]) { /* Wingman-Befund nachgestellt: Rohbereich 0..65535 */
        r = w282_learn2(0, 65535, 1, 0, out, sizeof out, &who);
        if (r != 0) /* Gegenprobe alter Weg: Achse rechts wird nie gelernt */
            _snprintf_s(why, sizeof why, _TRUNCATE, "alter Weg lernte '%s' (Nachstellung falsch)", out);
        r = w282_learn2(0, 65535, 1, 1, out, sizeof out, &who);
        if (!why[0] && (r != 3 || who != 1 || strncmp(out, "X:", 2) != 0 || strstr(out, "inv=1")))
            _snprintf_s(why, sizeof why, _TRUNCATE, "Geraet B (0..65535) r=%d who=%d '%s'", r, who, out);
        r = w282_learn2(0, 1023, 1, 1, out, sizeof out, &who);
        if (!why[0] && (r != 3 || who != 1 || strncmp(out, "X:", 2) != 0))
            _snprintf_s(why, sizeof why, _TRUNCATE, "Geraet B (0..1023) r=%d who=%d '%s'", r, who, out);
        r = w282_learn2(0, 1023, 0, 1, out, sizeof out, &who);
        if (!why[0] && r != 0)
            _snprintf_s(why, sizeof why, _TRUNCATE, "Ruhe lernte '%s' (Geraet %d)", out, who);
    }
    if (!why[0]) { /* Slot fuer Geraet 2 bekommt eigene joyN-Zeile */
        lctrl_bind_t b;
        ijoy_info_t L[2];
        int s;
        memset(L, 0, sizeof L);
        strcpy_s(L[0].product, sizeof L[0].product, "Arduino Leonardo");
        strcpy_s(L[0].name, sizeof L[0].name, "Arduino Leonardo");
        strcpy_s(L[1].product, sizeof L[1].product, "Logitech WingMan Extreme Digital 3D");
        strcpy_s(L[1].name, sizeof L[1].name, "Logitech WingMan Extreme Digital 3D");
        lctrl_bind_init(&b);
        s = lctrl_joy_slot_for(&b, L, 2, 1);
        if (s < 0 || !strstr(b.joy[s], "WingMan"))
            _snprintf_s(why, sizeof why, _TRUNCATE, "Slot Geraet 2: %d '%s'", s, s >= 0 ? b.joy[s] : "");
    }
    if (!why[0]) { /* Gamepad-Tastencodes (Hori: KEY:201/209) */
        static const int g[] = {0xC3, 201, 209, 211, 0xDA};
        static const int k[] = {VK_SPACE, VK_F1, VK_PRIOR, VK_NEXT, 0xC2, 0xDB, 'A'};
        for (i = 0; i < 5; i++) if (!lctrl_vk_is_gamepad(g[i])) strcpy_s(why, sizeof why, "VK_GAMEPAD nicht erkannt");
        for (i = 0; i < 7; i++) if (lctrl_vk_is_gamepad(k[i])) strcpy_s(why, sizeof why, "Tastatur als Gamepad erkannt");
    }
    if (!why[0]) W282_OK("W282 ISSUE6 joy-multi (zwei Geraete: Rohbereich 0..65535/0..1023 umgerechnet und gelernt, alter Weg nachgestellt, Achsen-Plan, Slot 2, VK_GAMEPAD ignoriert)");
    else W282_FAIL("W282 ISSUE6 joy-multi", why);
}

void w282_gui_selftest(const char *client_override, int *pass, int *fail)
{
    w282_joy_multi(pass, fail);
    w282_sbc_labels(pass, fail);
    w282_iso_recover(client_override, pass, fail);
    w282_bind_dialog(client_override, pass, fail);
}

/* --- W283 ORIG-KEY-BIND: Vergleichstaste im Belegungsdialog ---------------
 * Offscreen-Dialog im Temp-Client-Ordner (nie echte Profile): altes Profil
 * ohne orig_view zeigt die Standardbelegung, Lernen von Zusatztaste + Taste,
 * Zusatztaste allein, einfache Taste, Hinzufuegen, andere Zeilen lernen
 * Umschalt weiter sofort, "Alle leeren" behaelt die Zeile, Speichern schreibt
 * sie (leer = "orig_view =", kein Rueckfall). Tastenliste zeigt die Belegung. */
static char g_w283_why[220];
static char g_w283_prof[1200];

#define W283_WHY(...) do { if (!g_w283_why[0]) _snprintf_s(g_w283_why, sizeof g_w283_why, _TRUNCATE, __VA_ARGS__); } while (0)

static int w283_row_is(int idx, const char *want)
{
    char t[256];
    lctrl_bind_row_text(&g_bind, idx, t, sizeof t);
    return strcmp(t, want) == 0;
}

static void w283_row_why(const char *what, int idx)
{
    char t[256];
    lctrl_bind_row_text(&g_bind, idx, t, sizeof t);
    W283_WHY("%s: '%s' (cap=%d)", what, t, g_cap_mode);
}

/* Zeile idx in der Liste waehlen (bind_refresh_cur liest die Auswahl). */
static void w283_select(HWND hd, int idx)
{
    HWND lb = bctl(hd, IDB_LIST);
    int n = ListBox_GetCount(lb), at;
    for (at = 0; at < n; at++) {
        if ((int)ListBox_GetItemData(lb, at) == idx) {
            ListBox_SetCurSel(lb, at);
            g_bind_lb_prev = at;
            break;
        }
    }
    g_bind_sel = idx;
}

static void w283_learn(HWND hd, int idx, int mode, const int *vk, int n, int up_vk)
{
    int i;
    w283_select(hd, idx);
    bind_start_capture(hd, mode);
    for (i = 0; i < n && g_cap_mode; i++) {
        bind_cap_key(hd, vk[i], 0);
    }
    if (up_vk && g_cap_mode) {
        bind_cap_keyup(hd, up_vk);
    }
}

static void w283_bind_probe(HWND hd)
{
    static const int k_ctrl_f7[] = { VK_CONTROL, VK_F7 }, k_shift[] = { VK_SHIFT }, k_f6[] = { VK_F6 },
                     k_shift_f8[] = { VK_SHIFT, VK_F8 };
    int ov = lctrl_bind_elem_index("orig_view"), pz = lctrl_bind_elem_index("pause");
    char buf[4096];
    lctrl_bind_t rb;
    lctrl_conf1_t cf[4];
    HWND lb = bctl(hd, IDB_LIST);
    int i, n, found = 0;
    FILE *f;
    if (ov < 0 || pz < 0) { W283_WHY("orig_view/pause fehlt in kCtrlElems"); return; }
    for (i = 0, n = ListBox_GetCount(lb); i < n; i++) {
        if ((int)ListBox_GetItemData(lb, i) == ov) found = 1;
    }
    if (!found) { W283_WHY("orig_view nicht in der Liste des Dialogs"); return; }
    if (!w283_row_is(ov, "KEY:SHIFT+KEY:F8")) { w283_row_why("altes Profil ohne Zeile", ov); return; }
    /* Strg (VK_CONTROL ohne Ext-Bit = links) + F7 */
    w283_learn(hd, ov, 1, k_ctrl_f7, 2, 0);
    if (g_cap_mode || !w283_row_is(ov, "KEY:CTRL+KEY:F7")) { w283_row_why("Lernen Strg+F7", ov); return; }
    /* Umschalt gedrueckt: Lernen laeuft weiter */
    w283_select(hd, ov);
    bind_start_capture(hd, 1);
    bind_cap_key(hd, VK_SHIFT, 0);
    if (!g_cap_mode || !w283_row_is(ov, "KEY:CTRL+KEY:F7")) { w283_row_why("Umschalt-Druck beendete das Lernen", ov); return; }
    bind_end_capture(hd, NULL);
    /* Umschalt allein: beim Loslassen */
    w283_learn(hd, ov, 1, k_shift, 1, VK_SHIFT);
    if (g_cap_mode || !w283_row_is(ov, "KEY:SHIFT")) { w283_row_why("Umschalt allein", ov); return; }
    /* einfache Taste, dann hinzufuegen Umschalt+F8 */
    w283_learn(hd, ov, 1, k_f6, 1, 0);
    w283_learn(hd, ov, 2, k_shift_f8, 2, 0);
    if (!w283_row_is(ov, "KEY:F6, KEY:SHIFT+KEY:F8")) { w283_row_why("F6 + hinzu Umschalt+F8", ov); return; }
    /* andere Zeile (pause): Umschalt wird sofort gelernt wie bisher */
    w283_learn(hd, pz, 1, k_shift_f8, 2, 0);
    if (!w283_row_is(pz, "KEY:SHIFT")) { w283_row_why("pause (Verhalten anderer Zeilen)", pz); return; }
    lctrl_bind_set(&g_bind, pz, "KEY:PAUSE");
    /* Konflikte: F8 (vr_recenter) und Umschalt+F8 sind verschieden */
    if (lctrl_bind_conflicts_for(&g_bind, ov, cf, 4) != 0) { W283_WHY("Konflikt gemeldet fuer F6/Umschalt+F8"); return; }
    SendMessageW(hd, WM_COMMAND, MAKEWPARAM(IDB_CLEAR, BN_CLICKED), (LPARAM)bctl(hd, IDB_CLEAR));
    if (!w283_row_is(ov, "KEY:F6, KEY:SHIFT+KEY:F8")) { w283_row_why("Alle leeren", ov); return; }
    if (bind_save(hd) != 0) { W283_WHY("Speichern scheiterte"); return; }
    buf[0] = 0;
    if (u8_fopen_s(&f, g_w283_prof, "rb") == 0 && f) {
        size_t got = fread(buf, 1, sizeof buf - 1, f);
        buf[got] = 0;
        fclose(f);
    }
    if (!strstr(buf, "[client]") || !strstr(buf, "orig_view = KEY:F6, KEY:SHIFT+KEY:F8")) {
        W283_WHY("Datei ohne orig_view-Zeile in [client]");
        return;
    }
    lctrl_bind_clear(&g_bind, ov); /* leer = aus */
    if (bind_save(hd) != 0) { W283_WHY("Speichern (leer) scheiterte"); return; }
    lctrl_bind_load_file(g_w283_prof, &rb);
    if (rb.row[ov].n_alt != 0) { W283_WHY("leere Zeile nach Laden wieder belegt (%d)", rb.row[ov].n_alt); return; }
}

static void w283_bind_dialog(const char *client_override, int *pass, int *fail)
{
    char tmp[1024], dir[1100], pd[1200], keepdir[sizeof g_client_dir];
    gui_opts_t keepo = g_o;
    u8_GetTempPath(tmp, sizeof tmp);
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%sw283_bind", tmp);
    _snprintf_s(pd, sizeof pd, _TRUNCATE, "%s/profiles", dir);
    lcfg_mkdir_p(pd);
    _snprintf_s(g_w283_prof, sizeof g_w283_prof, _TRUNCATE, "%s/user_w283Test.ini", pd);
    write_file(g_w283_prof, "[buttons]\nbi0 = KEY:SPACE\n[client]\npause = KEY:PAUSE\nphoto = KEY:F12\n");
    g_w283_why[0] = 0;
    if (!(gui_init(client_override) && gui_create(1))) {
        strcpy_s(g_w283_why, sizeof g_w283_why, "Fenster nicht erzeugt");
    } else {
        build_controls();
        tips_build();
        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE); /* ausserhalb des Bildschirms */
        strcpy_s(keepdir, sizeof keepdir, g_client_dir);
        strcpy_s(g_client_dir, sizeof g_client_dir, dir); /* nie das echte Profil */
        g_o.shot = 3;
        g_o.bind_sel = -1;
        g_o.bind_learn = g_o.bind_joydemo = g_o.bind_stickdemo = g_o.bind_wheeldemo = g_o.bind_combodemo = 0;
        g_bind_testfn = w283_bind_probe;
        bind_dialog_modal("user_w283Test", g_w283_prof);
        g_bind_testfn = NULL;
        g_o = keepo;
        strcpy_s(g_client_dir, sizeof g_client_dir, keepdir);
        ShowWindow(g_hwnd, SW_HIDE);
        gui_teardown();
    }
    u8_DeleteFile(g_w283_prof);
    {
        wchar_t w[1200];
        u8_to_w(pd, w, 1200);
        RemoveDirectoryW(w);
        u8_to_w(dir, w, 1200);
        RemoveDirectoryW(w);
    }
    if (!g_w283_why[0]) W282_OK("W283 ORIGKEY bind-dialog (altes Profil -> Standard Shift+F8, Lernen Strg+F7 / Umschalt allein / F6 + hinzu Umschalt+F8, pause lernt Umschalt sofort, kein Konflikt mit F8, Alle leeren behaelt, Datei [client], leer bleibt leer)");
    else W282_FAIL("W283 ORIGKEY bind-dialog", g_w283_why);
}

static void w283_pure(int *pass, int *fail)
{
    static const struct { int m, k; const char *want; } kc[] = {
        { VK_LSHIFT, VK_F8, "KEY:SHIFT+KEY:F8" }, { VK_SHIFT, VK_F8, "KEY:SHIFT+KEY:F8" },
        { VK_RCONTROL, 'O', "KEY:CTRL+KEY:O" }, { VK_LCONTROL, VK_F7, "KEY:CTRL+KEY:F7" },
        { VK_MENU, VK_F8, "KEY:ALT+KEY:F8" }, { 0, VK_F6, "KEY:F6" },
        { 0, VK_LCONTROL, "KEY:LCTRL" }, { VK_SHIFT, VK_SHIFT, "KEY:SHIFT" },
        { VK_SHIFT, VK_RCONTROL, "KEY:RCTRL" },
    };
    char why[200] = "", t[64], *html;
    int ov = lctrl_bind_elem_index("orig_view"), i;
    lctrl_bind_t b;
    for (i = 0; i < (int)(sizeof kc / sizeof kc[0]) && !why[0]; i++) {
        lctrl_key_combo(kc[i].m, kc[i].k, t, sizeof t);
        if (strcmp(t, kc[i].want) != 0)
            _snprintf_s(why, sizeof why, _TRUNCATE, "lctrl_key_combo(%d,%d)='%s' statt '%s'", kc[i].m, kc[i].k, t, kc[i].want);
    }
    if (!why[0] && (ov < 0 || kCtrlElems[ov].kind != LCTRL_ACT || lctrl_elem_group(ov) != 5 ||
                    strcmp(kCtrlElems[ov].lkey, "bind.orig_view") != 0 || !LT("bind.orig_view")[0] ||
                    strcmp(LT("bind.orig_view"), "bind.orig_view") == 0 ||
                    strcmp(LT("gui.bind_combo_hint"), "gui.bind_combo_hint") == 0))
        strcpy_s(why, sizeof why, "orig_view: Element/Gruppe/Text");
    if (!why[0]) {
        for (i = 0; i < LCTRL_ELEM_N; i++)
            if (lctrl_bind_combo_row(i) != (kCtrlElems[i].kind != LCTRL_AXIS)) { /* W365: alle digitalen Zeilen */
                strcpy_s(why, sizeof why, "lctrl_bind_combo_row nicht alle digitalen Zeilen (W365)");
                break;
            }
    }
    if (!why[0] && (!lctrl_vk_is_modifier(VK_SHIFT) || !lctrl_vk_is_modifier(VK_RMENU) ||
                    !lctrl_vk_is_modifier(VK_LCONTROL) || lctrl_vk_is_modifier(VK_F8) || lctrl_vk_is_modifier('A')))
        strcpy_s(why, sizeof why, "lctrl_vk_is_modifier");
    if (!why[0]) { /* Laden: fehlt -> Standard, vorhanden -> wie Datei, leer -> aus */
        lctrl_bind_load_text("pause = KEY:PAUSE\n", &b);
        if (b.row[ov].n_alt != 1 || strcmp(b.row[ov].alt[0], LCTRL_ORIG_VIEW_DEFAULT) != 0)
            strcpy_s(why, sizeof why, "Laden ohne Zeile -> kein Standard");
        lctrl_bind_load_text("orig_view = KEY:F6, PAD:BACK+PAD:Y\n", &b);
        if (!why[0] && (b.row[ov].n_alt != 2 || strcmp(b.row[ov].alt[1], "PAD:BACK+PAD:Y") != 0))
            strcpy_s(why, sizeof why, "Laden mit Zeile");
        lctrl_bind_load_text("orig_view =\n", &b);
        if (!why[0] && b.row[ov].n_alt != 0)
            strcpy_s(why, sizeof why, "Laden leere Zeile -> nicht aus");
    }
    html = (char *)malloc(600000);
    if (!why[0] && html) { /* Tastenliste: Zeile mit der echten Belegung (Akkord als zwei kbd) */
        static const char kdef[] = "<kbd>KEY:SHIFT</kbd>+<kbd>KEY:F8</kbd>";
        keyshtml_build(LT("gui.keys_memo"), "", NULL, NULL, LT("gui.mouse_abs"), 4, "", "", "", html, 600000);
        if (!strstr(html, LT("bind.orig_view")) || !strstr(html, kdef))
            strcpy_s(why, sizeof why, "Tastenliste Standard ohne Vergleichstaste");
        keyshtml_build(LT("gui.keys_memo"), "w283_test", "orig_view = KEY:F6\n", NULL, LT("gui.mouse_abs"), 4, "", "", "",
                       html, 600000);
        if (!why[0] && (!strstr(html, LT("bind.orig_view")) || !strstr(html, "KEY:F6") || strstr(html, kdef)))
            strcpy_s(why, sizeof why, "Tastenliste zeigt die Profil-Belegung nicht");
        keyshtml_build(LT("gui.keys_memo"), "w283_old", "pause = KEY:PAUSE\n", NULL, LT("gui.mouse_abs"), 4, "", "", "",
                       html, 600000);
        if (!why[0] && !strstr(html, kdef))
            strcpy_s(why, sizeof why, "Tastenliste altes Profil ohne Standard");
    }
    free(html);
    if (!why[0]) W282_OK("W283 ORIGKEY pure (Kombinationsnamen, Element/Gruppe/Text, nur orig_view lernt Kombinationen, Laden Standard/Datei/leer, Tastenliste mit Belegung)");
    else W282_FAIL("W283 ORIGKEY pure", why);
}

void w283_gui_selftest(const char *client_override, int *pass, int *fail)
{
    w283_pure(pass, fail);
    w283_bind_dialog(client_override, pass, fail);
}
#undef W283_WHY

/* --- W365 BIND-COMBO / BIND-KEYNAMES ---------------------------------------------------------------
 * Rein: Anzeigenamen (Caps Lock, Num-Block, F13-F24, Medien/Browser, OEM, unbekannt de/en), Profil bleibt
 * kanonisch (KEY:20), Rueckrichtung, lesbare Quelle, Kombi-Lerner (Pad gehalten/allein/gleichzeitig,
 * Trigger), Konflikt-Hinweis (auch LCTRL ~ CTRL), welche Zeilen was lernen.
 * Dialog (offscreen, Temp-Client-Ordner, nie echte Profile): Strg+S, Umschalt+1, Umschalt allein,
 * Caps Lock (Anzeige "Caps Lock"), Pad LB+X und A allein, Gang 1 per BACK+DU, Umschalt+Maus links,
 * Konflikt-Hinweis bei Strg+Leertaste (bi0 = Leertaste), pause lernt keine Tastatur-Kombi, Speichern kanonisch. */
static char g_w365_why[240];
static char g_w365_prof[1200];

#define W365_WHY(...) do { if (!g_w365_why[0]) _snprintf_s(g_w365_why, sizeof g_w365_why, _TRUNCATE, __VA_ARGS__); } while (0)

static void w365_pure(int *pass, int *fail)
{
    static const struct { int vk; const char *want; } kn[] = {
        { VK_CAPITAL, "Caps Lock" }, { VK_NUMLOCK, "Num Lock" }, { VK_SCROLL, "Scroll Lock" },
        { VK_NUMPAD5, "Num 5" }, { VK_NUMPAD0, "Num 0" }, { VK_ADD, "Num +" }, { VK_DIVIDE, "Num /" },
        { VK_F13, "F13" }, { VK_F24, "F24" }, { VK_F1, "F1" }, { VK_VOLUME_MUTE, "Volume Mute" },
        { VK_MEDIA_PLAY_PAUSE, "Play/Pause" }, { VK_BROWSER_BACK, "Browser Back" },
        { VK_LWIN, "Left Win" }, { VK_APPS, "Menu" }, { VK_SNAPSHOT, "Print Screen" },
        { VK_PRIOR, "Page Up" }, { VK_LCONTROL, "Left Ctrl" }, { 'Q', "Q" }, { '7', "7" },
    };
    char why[240] = "", t[160], keep[8];
    int i, row = -1;
    lctrl_combo_t cb;
    lctrl_pad_t p0, p1, p2;
    lctrl_bind_t b;
    strcpy_s(keep, sizeof keep, sb_lang_code());
    sb_lang_set("en");
    for (i = 0; i < (int)(sizeof kn / sizeof kn[0]) && !why[0]; i++) {
        lctrl_key_disp(kn[i].vk, t, sizeof t);
        if (strcmp(t, kn[i].want) != 0) {
            _snprintf_s(why, sizeof why, _TRUNCATE, "lctrl_key_disp(%d)='%s' statt '%s'", kn[i].vk, t, kn[i].want);
        }
    }
    if (!why[0]) { /* OEM nach Layout: nicht leer, keine Nummer */
        lctrl_key_disp(VK_OEM_PERIOD, t, sizeof t);
        if (!t[0] || strstr(t, "Key ")) _snprintf_s(why, sizeof why, _TRUNCATE, "OEM-Punkt '%s'", t);
    }
    if (!why[0]) {
        lctrl_key_disp(0xFF, t, sizeof t);
        if (strcmp(t, "Key 255") != 0) _snprintf_s(why, sizeof why, _TRUNCATE, "EN unbekannt '%s'", t);
        sb_lang_set("de");
        lctrl_key_disp(0xFF, t, sizeof t);
        if (!why[0] && strcmp(t, "Taste 255") != 0) _snprintf_s(why, sizeof why, _TRUNCATE, "DE unbekannt '%s'", t);
    }
    if (!why[0]) { /* Profil kanonisch, Rueckrichtung */
        lctrl_key_name(VK_CAPITAL, t, sizeof t);
        if (strcmp(t, "20") != 0) _snprintf_s(why, sizeof why, _TRUNCATE, "lctrl_key_name(Caps)='%s' statt 20", t);
        else if (lctrl_key_vk("20") != VK_CAPITAL || lctrl_key_vk("0x14") != VK_CAPITAL ||
                 lctrl_key_vk("F5") != VK_F5 || lctrl_key_vk("space") != VK_SPACE || lctrl_key_vk("s") != 'S' ||
                 lctrl_key_vk("LCTRL") != VK_LCONTROL || lctrl_key_vk("XYZ") != -1)
            strcpy_s(why, sizeof why, "lctrl_key_vk Rueckrichtung");
    }
    if (!why[0]) { /* lesbare Quelle (Sprache de) */
        static const struct { const char *src, *want; int diff; } sp[] = {
            { "KEY:20", "Caps Lock", 1 }, { "KEY:CTRL+KEY:S", "Ctrl + S", 1 }, { "KEY:T", "T", 0 },
            { "KEY:SHIFT+KEY:1", "Shift + 1", 1 }, { "PAD:LB+PAD:X", "LB + X", 1 }, { "PAD:A", "A", 0 },
            { "PAD:DU", "Steuerkreuz oben", 1 }, { "KEY:SHIFT+MOUSE:LEFT", "Shift + MOUSE:LEFT", 1 },
        };
        for (i = 0; i < (int)(sizeof sp / sizeof sp[0]) && !why[0]; i++) {
            int d = lctrl_src_pretty(sp[i].src, t, sizeof t);
            if (d != sp[i].diff || strcmp(t, sp[i].want) != 0)
                _snprintf_s(why, sizeof why, _TRUNCATE, "lctrl_src_pretty(%s)='%s'/%d", sp[i].src, t, d);
        }
        if (!why[0] && (lctrl_src_pretty("PAD:L3", t, sizeof t) != 1 || strncmp(t, "Linker Stick", 12) != 0))
            _snprintf_s(why, sizeof why, _TRUNCATE, "lctrl_src_pretty(PAD:L3)='%s'", t);
    }
    if (!why[0]) {
        bind_src_disp("KEY:20", t, sizeof t);
        if (strcmp(t, "KEY:20  (Caps Lock)") != 0) _snprintf_s(why, sizeof why, _TRUNCATE, "Liste KEY:20 '%s'", t);
    }
    if (!why[0]) {
        int src;
        ks_atom("KEY:20", t, sizeof t, &src);
        if (strcmp(t, "Caps Lock") != 0) _snprintf_s(why, sizeof why, _TRUNCATE, "Uebersicht KEY:20 '%s'", t);
        ks_atom("KEY:SPACE", t, sizeof t, &src);
        if (!why[0] && strcmp(t, "SPACE") != 0) _snprintf_s(why, sizeof why, _TRUNCATE, "Uebersicht SPACE veraendert '%s'", t);
    }
    if (!why[0]) { /* Pad-Kombi-Lerner */
        memset(&p0, 0, sizeof p0);
        p1 = p0;
        p1.buttons = LCTRL_PB_LB;
        p2 = p1;
        p2.buttons |= LCTRL_PB_X;
        lctrl_combo_reset(&cb);
        if (lctrl_pad_combo_step(&cb, &p0, &p1, t, sizeof t) != 0) strcpy_s(why, sizeof why, "Pad: LB allein sofort belegt");
        else if (lctrl_pad_combo_step(&cb, &p1, &p1, t, sizeof t) != 0) strcpy_s(why, sizeof why, "Pad: LB gehalten belegt");
        else if (lctrl_pad_combo_step(&cb, &p1, &p2, t, sizeof t) != 1 || strcmp(t, "PAD:LB+PAD:X") != 0)
            _snprintf_s(why, sizeof why, _TRUNCATE, "Pad: LB+X '%s'", t);
    }
    if (!why[0]) {
        p1 = p0;
        p1.buttons = LCTRL_PB_A;
        lctrl_combo_reset(&cb);
        if (lctrl_pad_combo_step(&cb, &p0, &p1, t, sizeof t) != 0 ||
            lctrl_pad_combo_step(&cb, &p1, &p0, t, sizeof t) != 1 || strcmp(t, "PAD:A") != 0)
            _snprintf_s(why, sizeof why, _TRUNCATE, "Pad: A allein '%s'", t);
    }
    if (!why[0]) { /* gleichzeitig in einem Takt; Trigger als erste Taste */
        p2 = p0;
        p2.buttons = LCTRL_PB_LB | LCTRL_PB_Y;
        lctrl_combo_reset(&cb);
        if (lctrl_pad_combo_step(&cb, &p0, &p2, t, sizeof t) != 1 || !strstr(t, "PAD:LB") || !strstr(t, "PAD:Y") ||
            !strchr(t, '+'))
            _snprintf_s(why, sizeof why, _TRUNCATE, "Pad: LB+Y gleichzeitig '%s'", t);
        p1 = p0;
        p1.lt = 255;
        p2 = p1;
        p2.buttons = LCTRL_PB_A;
        lctrl_combo_reset(&cb);
        if (!why[0] && (lctrl_pad_combo_step(&cb, &p0, &p1, t, sizeof t) != 0 ||
                        lctrl_pad_combo_step(&cb, &p1, &p2, t, sizeof t) != 1 || strcmp(t, "PAD:LT+PAD:A") != 0))
            _snprintf_s(why, sizeof why, _TRUNCATE, "Pad: LT+A '%s'", t);
    }
    if (!why[0]) { /* allgemeiner Lerner (Original-Controller, Joystick) */
        lctrl_combo_reset(&cb);
        if (lctrl_combo_feed(&cb, "SBC:CHAFF", 1, t, sizeof t) != 0 ||
            lctrl_combo_feed(&cb, "SBC:EJECT", 1, t, sizeof t) != 1 || strcmp(t, "SBC:CHAFF+SBC:EJECT") != 0)
            _snprintf_s(why, sizeof why, _TRUNCATE, "feed: Kombi '%s'", t);
        lctrl_combo_reset(&cb);
        if (!why[0] && (lctrl_combo_feed(&cb, "SBC:CHAFF", 1, t, sizeof t) != 0 ||
                        lctrl_combo_feed(&cb, NULL, 1, t, sizeof t) != 0 ||
                        lctrl_combo_feed(&cb, NULL, 0, t, sizeof t) != 1 || strcmp(t, "SBC:CHAFF") != 0))
            _snprintf_s(why, sizeof why, _TRUNCATE, "feed: allein '%s'", t);
    }
    if (!why[0]) { /* Konflikt-Hinweis */
        lctrl_bind_load_text("[buttons]\nbi0 = KEY:S\npedal_l = KEY:LCTRL\n", &b);
        if (!lctrl_bind_combo_single_use(&b, "KEY:CTRL+KEY:S", t, sizeof t, &row) || row < 0)
            strcpy_s(why, sizeof why, "Konflikt Strg+S nicht erkannt");
        else if (lctrl_bind_combo_single_use(&b, "KEY:ALT+KEY:Q", t, sizeof t, &row))
            strcpy_s(why, sizeof why, "Konflikt Alt+Q gemeldet");
        else if (lctrl_bind_combo_single_use(&b, "KEY:S", t, sizeof t, &row))
            strcpy_s(why, sizeof why, "Einzeltaste als Kombi gemeldet");
        lctrl_bind_load_text("[buttons]\npedal_l = KEY:LCTRL\n", &b);
        if (!why[0] && (!lctrl_bind_combo_single_use(&b, "KEY:CTRL+KEY:S", t, sizeof t, &row) ||
                        row != lctrl_bind_elem_index("pedal_l")))
            strcpy_s(why, sizeof why, "Konflikt CTRL ~ LCTRL (pedal_l) nicht erkannt");
    }
    if (!why[0]) { /* Zeilen */
        int pz = lctrl_bind_elem_index("pause"), ph = lctrl_bind_elem_index("photo"),
            ac = lctrl_bind_elem_index("aim_center"), ax = lctrl_bind_elem_index("aim_x"),
            g1 = lctrl_bind_elem_index("gear.1"), fs = lctrl_bind_elem_index("feedback_shot");
        if (!lctrl_bind_combo_row(0) || !lctrl_bind_key_combo_row(0) || !lctrl_bind_mouse_combo_row(0) ||
            lctrl_bind_combo_row(ax) || !lctrl_bind_combo_row(pz) || lctrl_bind_key_combo_row(pz) ||
            lctrl_bind_key_combo_row(fs) || !lctrl_bind_key_combo_row(ph) || lctrl_bind_mouse_combo_row(ph) ||
            !lctrl_bind_mouse_combo_row(ac) || !lctrl_bind_key_combo_row(g1))
            strcpy_s(why, sizeof why, "Zeilen-Regeln (combo/key/mouse)");
    }
    sb_lang_set(keep);
    if (!why[0]) W282_OK("W365 BIND names+combo pure (Caps Lock/Num/F13-24/Medien/OEM, Profil KEY:20, Pad LB+X/A/LT+A, SBC-Lerner, Konflikt CTRL~LCTRL, Zeilen)");
    else W282_FAIL("W365 BIND names+combo pure", why);
}

static int w365_row_is(int idx, const char *want)
{
    char t[256];
    lctrl_bind_row_text(&g_bind, idx, t, sizeof t);
    if (strcmp(t, want) != 0) {
        W365_WHY("Zeile %s: '%s' statt '%s' (cap=%d)", kCtrlElems[idx].key, t, want, g_cap_mode);
        return 0;
    }
    return 1;
}

static void w365_select(HWND hd, int idx)
{
    HWND lb = bctl(hd, IDB_LIST);
    int n = ListBox_GetCount(lb), at;
    for (at = 0; at < n; at++) {
        if ((int)ListBox_GetItemData(lb, at) == idx) {
            ListBox_SetCurSel(lb, at);
            g_bind_lb_prev = at;
            break;
        }
    }
    g_bind_sel = idx;
}

static void w365_keys(HWND hd, int idx, const int *vk, int n, int up_vk)
{
    int i;
    w365_select(hd, idx);
    bind_start_capture(hd, 1);
    for (i = 0; i < n && g_cap_mode; i++) {
        bind_cap_key(hd, vk[i], 0);
    }
    if (up_vk && g_cap_mode) {
        bind_cap_keyup(hd, up_vk);
    }
}

static void w365_result(HWND hd, char *out, size_t cap)
{
    wchar_t w[1024];
    w[0] = 0;
    GetWindowTextW(bctl(hd, IDB_RESULT), w, 1024);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL);
}

static void w365_pad(HWND hd, int idx, unsigned short first, unsigned short second)
{
    lctrl_pad_t p0, p1, p2;
    memset(&p0, 0, sizeof p0);
    p1 = p0;
    p1.buttons = first;
    p2 = p1;
    p2.buttons = (unsigned short)(first | second);
    w365_select(hd, idx);
    bind_start_capture(hd, 1);
    g_cap_prev = p0;
    bind_cap_pad_state(hd, &p1);
    if (!g_cap_mode) {
        W365_WHY("Pad: erste Taste allein beendete das Lernen");
        return;
    }
    bind_cap_pad_state(hd, second ? &p2 : &p0);
}

static void w365_bind_probe(HWND hd)
{
    static const int k_ctrl_s[] = { VK_CONTROL, 'S' }, k_shift_1[] = { VK_SHIFT, '1' }, k_shift[] = { VK_SHIFT },
                     k_caps[] = { VK_CAPITAL }, k_ctrl_sp[] = { VK_CONTROL, VK_SPACE };
    int b1 = lctrl_bind_elem_index("bi1"), b2 = lctrl_bind_elem_index("bi2"), b3 = lctrl_bind_elem_index("bi3"),
        b4 = lctrl_bind_elem_index("bi4"), b5 = lctrl_bind_elem_index("bi5"), b6 = lctrl_bind_elem_index("bi6"),
        b7 = lctrl_bind_elem_index("bi7"), g1 = lctrl_bind_elem_index("gear.1"), pz = lctrl_bind_elem_index("pause");
    char res[1024], buf[4096];
    FILE *f;
    w365_keys(hd, b1, k_ctrl_s, 2, 0);
    if (g_cap_mode || !w365_row_is(b1, "KEY:CTRL+KEY:S")) return;
    w365_keys(hd, b2, k_shift_1, 2, 0);
    if (g_cap_mode || !w365_row_is(b2, "KEY:SHIFT+KEY:1")) return;
    w365_keys(hd, b3, k_shift, 1, VK_SHIFT);
    if (g_cap_mode || !w365_row_is(b3, "KEY:SHIFT")) return;
    w365_keys(hd, b3, k_caps, 1, 0);
    if (g_cap_mode || !w365_row_is(b3, "KEY:20")) return;
    w365_result(hd, res, sizeof res);
    if (!strstr(res, "Caps Lock")) { W365_WHY("Ergebnis ohne 'Caps Lock': '%s'", res); return; }
    /* Pad: LB halten + X -> Kombi; A allein beim Loslassen; Gang 1 per BACK+DU */
    w365_pad(hd, b4, LCTRL_PB_LB, LCTRL_PB_X);
    if (g_cap_mode || !w365_row_is(b4, "PAD:LB+PAD:X")) return;
    w365_pad(hd, b5, LCTRL_PB_A, 0);
    if (g_cap_mode || !w365_row_is(b5, "PAD:A")) return;
    w365_pad(hd, g1, LCTRL_PB_BACK, LCTRL_PB_DU);
    if (g_cap_mode || !w365_row_is(g1, "PAD:BACK+PAD:DU")) return;
    /* Konflikt: bi0 = KEY:SPACE einzeln, Strg+Leertaste -> Hinweis, trotzdem eingetragen */
    w365_keys(hd, b6, k_ctrl_sp, 2, 0);
    if (g_cap_mode || !w365_row_is(b6, "KEY:CTRL+KEY:SPACE")) return;
    w365_result(hd, res, sizeof res);
    if (!strstr(res, "bi0") || !strstr(res, "Space")) { W365_WHY("kein Konflikt-Hinweis: '%s'", res); return; }
    /* Umschalt + Maus links */
    w365_select(hd, b7);
    bind_start_capture(hd, 1);
    bind_cap_key(hd, VK_SHIFT, 0);
    bind_cap_mouse(hd, WM_LBUTTONDOWN, 0);
    if (g_cap_mode || !w365_row_is(b7, "KEY:SHIFT+MOUSE:LEFT")) return;
    /* pause: Tastatur-Kombi nicht (Fenster kennt nur einfache Tasten) -> Strg sofort */
    w365_keys(hd, pz, k_ctrl_s, 2, 0);
    if (g_cap_mode || !w365_row_is(pz, "KEY:LCTRL")) return;
    lctrl_bind_set(&g_bind, pz, "KEY:PAUSE");
    if (bind_save(hd) != 0) { W365_WHY("Speichern scheiterte"); return; }
    buf[0] = 0;
    if (u8_fopen_s(&f, g_w365_prof, "rb") == 0 && f) {
        size_t got = fread(buf, 1, sizeof buf - 1, f);
        buf[got] = 0;
        fclose(f);
    }
    if (!strstr(buf, "KEY:CTRL+KEY:S") || !strstr(buf, "KEY:SHIFT+KEY:1") || !strstr(buf, "KEY:20") ||
        !strstr(buf, "PAD:LB+PAD:X") || !strstr(buf, "PAD:BACK+PAD:DU") || !strstr(buf, "KEY:SHIFT+MOUSE:LEFT") ||
        strstr(buf, "Caps Lock")) {
        W365_WHY("Datei nicht kanonisch / Kombis fehlen");
        return;
    }
}

static void w365_bind_dialog(const char *client_override, int *pass, int *fail)
{
    char tmp[1024], dir[1100], pd[1200], keepdir[sizeof g_client_dir], keepl[8];
    gui_opts_t keepo = g_o;
    u8_GetTempPath(tmp, sizeof tmp);
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%sw365_bind", tmp);
    _snprintf_s(pd, sizeof pd, _TRUNCATE, "%s/profiles", dir);
    lcfg_mkdir_p(pd);
    _snprintf_s(g_w365_prof, sizeof g_w365_prof, _TRUNCATE, "%s/user_w365Test.ini", pd);
    write_file(g_w365_prof, "[buttons]\nbi0 = KEY:SPACE\n[client]\npause = KEY:PAUSE\n");
    g_w365_why[0] = 0;
    strcpy_s(keepl, sizeof keepl, sb_lang_code());
    sb_lang_set("en");
    if (!(gui_init(client_override) && gui_create(1))) {
        strcpy_s(g_w365_why, sizeof g_w365_why, "Fenster nicht erzeugt");
    } else {
        build_controls();
        tips_build();
        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE); /* ausserhalb des Bildschirms */
        strcpy_s(keepdir, sizeof keepdir, g_client_dir);
        strcpy_s(g_client_dir, sizeof g_client_dir, dir); /* nie das echte Profil */
        g_o.shot = 3;
        g_o.bind_sel = -1;
        g_o.bind_learn = g_o.bind_joydemo = g_o.bind_stickdemo = g_o.bind_wheeldemo = g_o.bind_combodemo = 0;
        g_bind_testfn = w365_bind_probe;
        bind_dialog_modal("user_w365Test", g_w365_prof);
        g_bind_testfn = NULL;
        g_o = keepo;
        strcpy_s(g_client_dir, sizeof g_client_dir, keepdir);
        ShowWindow(g_hwnd, SW_HIDE);
        gui_teardown();
    }
    sb_lang_set(keepl);
    u8_DeleteFile(g_w365_prof);
    {
        wchar_t w[1200];
        u8_to_w(pd, w, 1200);
        RemoveDirectoryW(w);
        u8_to_w(dir, w, 1200);
        RemoveDirectoryW(w);
    }
    if (!g_w365_why[0]) W282_OK("W365 BIND combo bind-dialog (Strg+S, Umschalt+1, Umschalt allein, Caps Lock angezeigt, Pad LB+X / A allein / Gang 1 BACK+DU, Konflikt-Hinweis Strg+Leertaste, Umschalt+Maus, pause ohne Tastatur-Kombi, Datei kanonisch)");
    else W282_FAIL("W365 BIND combo bind-dialog", g_w365_why);
}

void w365_gui_selftest(const char *client_override, int *pass, int *fail)
{
    w365_pure(pass, fail);
    w365_bind_dialog(client_override, pass, fail);
}
#undef W365_WHY

/* --- W432 HOTAS-AXES: Belegungsdialog mit nachgebauten Tester-Geraeten -------
 * Offscreen-Dialog im Temp-Client-Ordner (nie echte Profile). Fake-DirectInput
 * (WingMan Force 3D + Arduino Micro gleichzeitig, launcher.c w432_fixture_*):
 * Lernen ueber den echten Dialogweg bind_start_capture -> bind_cap_joy je Takt;
 * zentrierte Achsen mit kurzem Weg, Schub am Ende, zwei Geraete -> zwei joyN-
 * Zeilen. Gamepad-Weg (Xbox-Modus) ueber bind_cap_pad_state. */
static char g_w432_why[300];
static char g_w432_prof[1200];
#define W432_WHY(...) do { if (!g_w432_why[0]) _snprintf_s(g_w432_why, sizeof g_w432_why, _TRUNCATE, __VA_ARGS__); } while (0)

static void w432_joy_cap(HWND hd, const char *elem, int dev, int obj, long peak, const char *want)
{
    int idx = lctrl_bind_elem_index(elem), t;
    char row[256];
    if (g_w432_why[0]) {
        return;
    }
    w365_select(hd, idx);
    bind_start_capture(hd, 1);
    bind_cap_joy_init(); /* Bild-Modus (shot) zaehlt selbst keine Geraete auf */
    for (t = 0; t < 40 && g_cap_mode; t++) {
        w432_fixture_tick(t, dev, obj, peak, -1, 0);
        bind_cap_joy(hd);
    }
    if (g_cap_mode) {
        bind_end_capture(hd, NULL);
    }
    lctrl_bind_row_text(&g_bind, idx, row, sizeof row);
    if (strcmp(row, want) != 0) {
        W432_WHY("%s: '%s' statt '%s'", elem, row, want);
    }
}

static void w432_bind_probe(HWND hd)
{
    int sx = lctrl_bind_elem_index("sight_x"), k;
    char row[256];
    if (w432_fixture_plug(1, 0) != 2) {
        W432_WHY("Fake-Geraete nicht aufgezaehlt");
        ijoy_test_end();
        return;
    }
    /* WingMan = Geraet 0 -> JOY0, Arduino = Geraet 1 -> JOY1 (neue joyN-Zeilen) */
    w432_joy_cap(hd, "aim_x", 0, 0, 223, "JOY0:X:dz=0.05");
    w432_joy_cap(hd, "aim_y", 1, 1, 9830, "JOY1:Y:dz=0.05");
    w432_joy_cap(hd, "rot", 0, 2, 198, "JOY0:RZ:dz=0.05");
    w432_joy_cap(hd, "pedal_l", 0, 3, 0, "JOY0:S0:dz=0.05:inv=1");
    w432_joy_cap(hd, "pedal_r", 1, 2, 30000, "JOY1:S0:dz=0.05");
    if (!g_w432_why[0] && (!strstr(g_bind.joy[0], "Test Stick 0") || !strstr(g_bind.joy[1], "Test Stick 1"))) {
        W432_WHY("joyN-Zeilen: '%s' / '%s'", g_bind.joy[0], g_bind.joy[1]);
    }
    ijoy_test_end();
    /* Gamepad-Weg: Drehachse eines Flugsticks im Xbox-Modus (RX, 49 %), Schub ruht auf RY = -32768 */
    if (!g_w432_why[0]) {
        lctrl_pad_t rest, mv;
        memset(&rest, 0, sizeof rest);
        rest.ry = -32768;
        mv = rest;
        mv.rx = 16000;
        w365_select(hd, sx);
        bind_start_capture(hd, 1);
        lctrl_pad_learner_reset(&g_cap_plrn);
        g_cap_prev = rest;
        for (k = 0; k < 8 && g_cap_mode; k++) {
            bind_cap_pad_state(hd, k < 4 ? &rest : &mv);
        }
        if (g_cap_mode) {
            bind_end_capture(hd, NULL);
        }
        lctrl_bind_row_text(&g_bind, sx, row, sizeof row);
        if (strcmp(row, "PAD:RX:dz=0.12:curve=1.3") != 0) {
            W432_WHY("sight_x (Pad): '%s'", row);
        }
    }
    if (!g_w432_why[0]) {
        static char buf[4096];
        FILE *f = NULL;
        buf[0] = 0;
        if (bind_save(hd) != 0) {
            W432_WHY("Speichern scheiterte");
            return;
        }
        if (u8_fopen_s(&f, g_w432_prof, "rb") == 0 && f) {
            size_t got = fread(buf, 1, sizeof buf - 1, f);
            buf[got] = 0;
            fclose(f);
        }
        if (!strstr(buf, "JOY0:X:dz=0.05") || !strstr(buf, "JOY1:S0:dz=0.05") || !strstr(buf, "Test Stick 1") ||
            !strstr(buf, "PAD:RX:dz=0.12")) {
            W432_WHY("Datei ohne JOY-Achsen/joyN-Zeilen");
        }
    }
}

void w432_gui_selftest(const char *client_override, int *pass, int *fail)
{
    char tmp[1024], dir[1100], pd[1200], keepdir[sizeof g_client_dir], keepl[8];
    gui_opts_t keepo = g_o;
    u8_GetTempPath(tmp, sizeof tmp);
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%sw432_bind", tmp);
    _snprintf_s(pd, sizeof pd, _TRUNCATE, "%s/profiles", dir);
    lcfg_mkdir_p(pd);
    _snprintf_s(g_w432_prof, sizeof g_w432_prof, _TRUNCATE, "%s/user_w432Test.ini", pd);
    write_file(g_w432_prof, "[buttons]\nbi0 = KEY:SPACE\n");
    g_w432_why[0] = 0;
    strcpy_s(keepl, sizeof keepl, sb_lang_code());
    sb_lang_set("en");
    if (!(gui_init(client_override) && gui_create(1))) {
        strcpy_s(g_w432_why, sizeof g_w432_why, "Fenster nicht erzeugt");
    } else {
        build_controls();
        tips_build();
        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE); /* ausserhalb des Bildschirms */
        strcpy_s(keepdir, sizeof keepdir, g_client_dir);
        strcpy_s(g_client_dir, sizeof g_client_dir, dir); /* nie das echte Profil */
        g_o.shot = 3;
        g_o.bind_sel = -1;
        g_o.bind_learn = g_o.bind_joydemo = g_o.bind_stickdemo = g_o.bind_wheeldemo = g_o.bind_combodemo = 0;
        g_bind_testfn = w432_bind_probe;
        bind_dialog_modal("user_w432Test", g_w432_prof);
        g_bind_testfn = NULL;
        g_o = keepo;
        strcpy_s(g_client_dir, sizeof g_client_dir, keepdir);
        ShowWindow(g_hwnd, SW_HIDE);
        gui_teardown();
    }
    ijoy_test_end(); /* falls der Dialog vorher endete */
    sb_lang_set(keepl);
    u8_DeleteFile(g_w432_prof);
    {
        wchar_t w[1200];
        u8_to_w(pd, w, 1200);
        RemoveDirectoryW(w);
        u8_to_w(dir, w, 1200);
        RemoveDirectoryW(w);
    }
    if (!g_w432_why[0]) W282_OK("W432 HOTAS-AXES bind-dialog (WingMan X 75 %/Y/RZ 55 %/Schub + Arduino Y 70 %/Throttle 37 % ueber bind_cap_joy, JOY0/JOY1-Zeilen, Pad-Drehachse RX 49 %, Datei)");
    else W282_FAIL("W432 HOTAS-AXES bind-dialog", g_w432_why);
}
#undef W432_WHY

/* --- W290 QUICK-SHOT: Schnell-Screenshot + Fotomodus im Belegungsdialog, Karte "Fotos & Screenshots" ---------
 * Rein: Uebergang alter Profile (photo=F12 -> Shift+F12 + quick_shot=F12), eigene Belegung bleibt, leere Zeile =
 * aus ("quick_shot =" beim Speichern), "Alle leeren" behaelt beide, Tastenliste mit der echten Belegung.
 * Dialog (offscreen, Temp-Client-Ordner): Zeilen vorhanden, Lernen Strg+F12 / Druck auf quick_shot und
 * Umschalt+F12 auf photo (Akkord), kein Konflikt F12 <-> Umschalt+F12, Datei [client].
 * Oberflaeche: Karte auf Ton & Extras (Elemente, Groessen-Liste, Rundweg Oberflaeche/user.cfg, alte cfg). */
static char g_w290_why[220];
static char g_w290_prof[1200];
#define W290_WHY(...) do { if (!g_w290_why[0]) _snprintf_s(g_w290_why, sizeof g_w290_why, _TRUNCATE, __VA_ARGS__); } while (0)

static int w290_row(const lctrl_bind_t *b, const char *key, const char *want)
{
    char t[256];
    lctrl_bind_row_text(b, lctrl_bind_elem_index(key), t, sizeof t);
    return strcmp(t, want) == 0;
}

static void w290_pure(int *pass, int *fail)
{
    char why[220] = "", *html;
    int qs = lctrl_bind_elem_index("quick_shot"), ph = lctrl_bind_elem_index("photo");
    lctrl_bind_t b;
    if (qs < 0 || ph < 0 || kCtrlElems[qs].kind != LCTRL_ACT || lctrl_elem_group(qs) != lctrl_elem_group(ph) ||
        !lctrl_bind_combo_row(qs) || !lctrl_bind_combo_row(ph) || !lctrl_bind_empty_line_row(qs) ||
        lctrl_bind_empty_line_row(ph) || strcmp(LT("bind.quick_shot"), "bind.quick_shot") == 0)
        strcpy_s(why, sizeof why, "quick_shot: Element/Gruppe/Akkord/Text");
    if (!why[0]) {
        lctrl_bind_load_text("[client]\npause = KEY:PAUSE\nphoto = KEY:F12\n", &b);
        if (!w290_row(&b, "photo", "KEY:SHIFT+KEY:F12") || !w290_row(&b, "quick_shot", "KEY:F12"))
            strcpy_s(why, sizeof why, "Uebergang photo=F12 -> Shift+F12 + quick_shot=F12");
    }
    if (!why[0]) {
        lctrl_bind_load_text("photo = KEY:F7\n", &b);
        if (!w290_row(&b, "photo", "KEY:F7") || !w290_row(&b, "quick_shot", "KEY:F12"))
            strcpy_s(why, sizeof why, "eigene photo-Taste bleibt, quick_shot=F12");
    }
    if (!why[0]) {
        lctrl_bind_load_text("photo = KEY:F12, PAD:Y\n", &b);
        if (!w290_row(&b, "photo", "KEY:F12, PAD:Y") || b.row[qs].n_alt != 0)
            strcpy_s(why, sizeof why, "photo mit F12 + Pad bleibt, quick_shot leer");
    }
    if (!why[0]) {
        lctrl_bind_load_text("photo = KEY:SHIFT+KEY:F12\nquick_shot =\n", &b);
        if (b.row[qs].n_alt != 0 || !w290_row(&b, "photo", "KEY:SHIFT+KEY:F12"))
            strcpy_s(why, sizeof why, "leere quick_shot-Zeile = aus");
    }
    if (!why[0]) {
        lctrl_bind_load_text("quick_shot = KEY:CTRL+KEY:F12, PAD:BACK+PAD:X\nphoto = KEY:SHIFT+KEY:F12\nbi0 = KEY:SPACE\n", &b);
        lctrl_bind_clear_game(&b);
        if (!w290_row(&b, "quick_shot", "KEY:CTRL+KEY:F12, PAD:BACK+PAD:X") || !w290_row(&b, "photo", "KEY:SHIFT+KEY:F12") ||
            b.row[0].n_alt != 0)
            strcpy_s(why, sizeof why, "Alle leeren behaelt quick_shot/photo nicht");
    }
    html = (char *)malloc(600000);
    if (!why[0] && html) { /* Tastenliste: F12 / Shift+F12 mit der echten Belegung */
        keyshtml_build(LT("gui.keys_memo"), "", NULL, NULL, LT("gui.mouse_abs"), 4, "", "", "", html, 600000);
        if (!strstr(html, LT("bind.quick_shot")) || !strstr(html, LT("bind.photo")) ||
            !strstr(html, "<kbd>KEY:SHIFT</kbd>+<kbd>KEY:F12</kbd>"))
            strcpy_s(why, sizeof why, "Tastenliste Standard ohne Schnell-Screenshot/Fotomodus");
        keyshtml_build(LT("gui.keys_memo"), "w290_test", "quick_shot = KEY:F7\nphoto = KEY:F6\n", NULL, LT("gui.mouse_abs"), 4,
                       "", "", "", html, 600000);
        if (!why[0] && (!strstr(html, "KEY:F7") || !strstr(html, "KEY:F6") ||
                        strstr(html, "<kbd>KEY:SHIFT</kbd>+<kbd>KEY:F12</kbd>")))
            strcpy_s(why, sizeof why, "Tastenliste zeigt die Profil-Belegung nicht");
    }
    free(html);
    if (!why[0]) W282_OK("W290 QUICKSHOT pure (Element/Gruppe/Akkord, Uebergang alter Profile, eigene Belegung bleibt, leer = aus, Alle leeren behaelt, Tastenliste F12/Shift+F12)");
    else W282_FAIL("W290 QUICKSHOT pure", why);
}

static void w290_bind_probe(HWND hd)
{
    static const int k_ctrl_f12[] = { VK_CONTROL, VK_F12 }, k_shift_f12[] = { VK_SHIFT, VK_F12 }, k_snap[] = { VK_SNAPSHOT };
    int qs = lctrl_bind_elem_index("quick_shot"), ph = lctrl_bind_elem_index("photo");
    char buf[4096];
    lctrl_bind_t rb;
    lctrl_conf1_t cf[4];
    HWND lb = bctl(hd, IDB_LIST);
    int i, n, fq = 0, fp = 0;
    FILE *f;
    for (i = 0, n = ListBox_GetCount(lb); i < n; i++) {
        if ((int)ListBox_GetItemData(lb, i) == qs) fq = 1;
        if ((int)ListBox_GetItemData(lb, i) == ph) fp = 1;
    }
    if (!fq || !fp) { W290_WHY("quick_shot/photo nicht in der Liste des Dialogs"); return; }
    if (!w290_row(&g_bind, "photo", "KEY:SHIFT+KEY:F12") || !w290_row(&g_bind, "quick_shot", "KEY:F12")) {
        W290_WHY("altes Profil (photo=F12) nicht auf F12/Shift+F12 gehoben");
        return;
    }
    if (lctrl_bind_conflicts_for(&g_bind, qs, cf, 4) != 0) { W290_WHY("Konflikt F12 <-> Shift+F12 gemeldet"); return; }
    w283_learn(hd, qs, 1, k_ctrl_f12, 2, 0);
    if (g_cap_mode || !w290_row(&g_bind, "quick_shot", "KEY:CTRL+KEY:F12")) { W290_WHY("Lernen Strg+F12 auf quick_shot"); return; }
    w283_learn(hd, qs, 2, k_snap, 1, 0);
    if (!w290_row(&g_bind, "quick_shot", "KEY:CTRL+KEY:F12, KEY:SNAPSHOT")) { W290_WHY("hinzu Druck-Taste auf quick_shot"); return; }
    w283_learn(hd, ph, 1, k_shift_f12, 2, 0);
    if (g_cap_mode || !w290_row(&g_bind, "photo", "KEY:SHIFT+KEY:F12")) { W290_WHY("Lernen Umschalt+F12 auf photo"); return; }
    SendMessageW(hd, WM_COMMAND, MAKEWPARAM(IDB_CLEAR, BN_CLICKED), (LPARAM)bctl(hd, IDB_CLEAR));
    if (!w290_row(&g_bind, "quick_shot", "KEY:CTRL+KEY:F12, KEY:SNAPSHOT") || !w290_row(&g_bind, "photo", "KEY:SHIFT+KEY:F12")) {
        W290_WHY("Alle leeren hat quick_shot/photo geleert");
        return;
    }
    if (bind_save(hd) != 0) { W290_WHY("Speichern scheiterte"); return; }
    buf[0] = 0;
    if (u8_fopen_s(&f, g_w290_prof, "rb") == 0 && f) {
        size_t got = fread(buf, 1, sizeof buf - 1, f);
        buf[got] = 0;
        fclose(f);
    }
    if (!strstr(buf, "[client]") || !strstr(buf, "quick_shot = KEY:CTRL+KEY:F12, KEY:SNAPSHOT") ||
        !strstr(buf, "photo = KEY:SHIFT+KEY:F12")) {
        W290_WHY("Datei ohne quick_shot/photo in [client]");
        return;
    }
    lctrl_bind_clear(&g_bind, qs);
    if (bind_save(hd) != 0) { W290_WHY("Speichern (leer) scheiterte"); return; }
    lctrl_bind_load_file(g_w290_prof, &rb);
    if (rb.row[qs].n_alt != 0) { W290_WHY("leere quick_shot-Zeile nach Laden wieder belegt (%d)", rb.row[qs].n_alt); return; }
}

/* Karte "Fotos & Screenshots" (Fenster muss gebaut sein) */
static void w290_card(void)
{
    static const int ids[] = { ID_PH_DIR, ID_PH_VIEW, ID_PH_RES, ID_PH_FMT, ID_PH_ASP, ID_RP_LABEL, ID_RP_LOGO, ID_WM_MEDIA, ID_PH_HUD, ID_XEXP_PHOTO };
    launcher_settings_t a, b;
    char f[1300], tmp[1024], t[1200];
    int k;
    for (k = 0; k < (int)(sizeof ids / sizeof ids[0]); k++) {
        uictl_t *u = ui_find(ids[k]);
        if (!u || !ui_visible_on(u, P_AUDIO) || ui_visible_on(u, P_GFX)) { W290_WHY("Element %d fehlt/falsche Seite", ids[k]); return; }
    }
    if (ComboBox_GetCount(ctl(ID_PH_RES)) != 6 || ComboBox_GetCount(ctl(ID_PH_VIEW)) != 4 || ComboBox_GetCount(ctl(ID_PH_ASP)) != 2) { /* W428: 4 Ansichten */
        W290_WHY("Listen Groesse/Ansicht/Bildformat (%d/%d/%d)", ComboBox_GetCount(ctl(ID_PH_RES)),
                 ComboBox_GetCount(ctl(ID_PH_VIEW)), ComboBox_GetCount(ctl(ID_PH_ASP)));
        return;
    }
    GetWindowTextA(ctl(ID_XEXP_PHOTO), t, sizeof t);
    if (!t[0]) { W290_WHY("Kolben ohne Tooltip-Text"); return; }
    lcfg_defaults(&a);
    if (a.photo_view != 0 || lcfg_photo_size_eff(&a) != -4 || a.photo_aspect != 2 || !a.replay_label || !a.replay_logo ||
        !a.watermark_media) {
        W290_WHY("Standard nicht wie im Spiel/4K/4:3/Original");
        return;
    }
    a.photo_view = 1;
    a.photo_size = -8;
    a.photo_aspect = 1;
    a.photo_jpg = 1;
    a.replay_label = 0;
    a.replay_logo = 0;
    a.watermark_media = 0;
    a.photo_hud = 0;
    ui_from_settings(&a);
    lcfg_defaults(&b);
    ui_to_settings(&b);
    if (b.photo_view != 1 || b.photo_size != -8 || b.photo_aspect != 1 || b.photo_jpg != 1 || b.replay_label || b.replay_logo ||
        b.watermark_media || b.photo_hud || b.photo_scale != 1) {
        W290_WHY("Rundweg Oberflaeche");
        return;
    }
    u8_GetTempPath(tmp, sizeof tmp);
    _snprintf_s(f, sizeof f, _TRUNCATE, "%sw290_user_%lu.cfg", tmp, GetCurrentProcessId());
    u8_DeleteFile(f);
    if (lcfg_save_user(f, &b) != 0) { W290_WHY("lcfg_save_user"); return; }
    {
        FILE *fp;
        char buf[60000];
        size_t got = 0;
        buf[0] = 0;
        if (u8_fopen_s(&fp, f, "rb") == 0 && fp) { got = fread(buf, 1, sizeof buf - 1, fp); buf[got] = 0; fclose(fp); }
        if (!strstr(buf, "photo_size=8k") || !strstr(buf, "photo_view=1") || !strstr(buf, "replay_label=0") ||
            !strstr(buf, "replay_logo=0") || !strstr(buf, "watermark_media=0") || !strstr(buf, "photo_hud=0")) {
            W290_WHY("user.cfg ohne photo_size=8k/photo_view/replay_*/watermark_media");
        }
    }
    lcfg_defaults(&a);
    if (!g_w290_why[0] && (lcfg_load_file(f, &a) != 0 || a.photo_size != -8 || a.photo_view != 1 || a.replay_label || a.watermark_media))
        W290_WHY("Rundweg user.cfg");
    u8_DeleteFile(f);
    if (!g_w290_why[0]) { /* alte cfg: photo_scale=4 ohne photo_size -> x4; photo_size gewinnt */
        write_file(f, "[photo]\nphoto_scale=4\nphoto_format=jpg\nphoto_aspect=1\n");
        lcfg_defaults(&a);
        if (lcfg_load_file(f, &a) != 0 || a.photo_size != 4) W290_WHY("alte cfg photo_scale=4 -> x4 (%d)", a.photo_size);
        if (!g_w290_why[0] && a.photo_aspect != 2) W290_WHY("alte cfg photo_aspect=1 ohne photo_view -> 4:3 (%d)", a.photo_aspect);
        write_file(f, "[photo]\nphoto_view=2\nphoto_aspect=1\n");
        lcfg_defaults(&a);
        if (!g_w290_why[0] && (lcfg_load_file(f, &a) != 0 || a.photo_aspect != 1)) W290_WHY("photo_aspect=1 mit photo_view bleibt breit");
        write_file(f, "[photo]\nphoto_scale=4\nphoto_size=4k\n");
        lcfg_defaults(&a);
        if (!g_w290_why[0] && (lcfg_load_file(f, &a) != 0 || a.photo_size != -4)) W290_WHY("photo_size gewinnt");
        u8_DeleteFile(f);
    }
    lcfg_defaults(&a);
    ui_from_settings(&a);
}

/* W428 PHOTO-CINEMATIC: Ansicht "Cinematic" (photo_view=3) in der Karte Fotos & Screenshots: Listeneintrag mit
 * Sprachtext, Rundweg Oberflaeche und user.cfg, ungueltiger Wert -> wie im Spiel. Fenster muss gebaut sein. */
static char g_w428_why[256];
static void w428_cine_card(void)
{
    launcher_settings_t a, b;
    char f[1300], tmp[1024];
    wchar_t it[128];
    g_w428_why[0] = 0;
    if (ComboBox_GetCount(ctl(ID_PH_VIEW)) != 4) { strcpy_s(g_w428_why, sizeof g_w428_why, "Ansicht-Liste ohne 4. Eintrag"); return; }
    it[0] = 0;
    if (SendMessageW(ctl(ID_PH_VIEW), CB_GETLBTEXTLEN, 3, 0) < 128) { SendMessageW(ctl(ID_PH_VIEW), CB_GETLBTEXT, 3, (LPARAM)it); }
    if (wcscmp(it, L"Cinematic") != 0) { strcpy_s(g_w428_why, sizeof g_w428_why, "4. Eintrag nicht Cinematic"); return; }
    /* W445: Liste "Reichweite eigene VT-Teile" (Aus + Raster), nur bei Cinematic frei, Standard 4.0 m */
    if (ComboBox_GetCount(ctl(ID_PH_CSELF)) != LCFG_CSELF_N) {
        _snprintf_s(g_w428_why, sizeof g_w428_why, _TRUNCATE, "W445 Reichweite: %d Eintraege statt %d", ComboBox_GetCount(ctl(ID_PH_CSELF)), LCFG_CSELF_N);
        return;
    }
    lcfg_defaults(&a);
    if (a.photo_cine_self != 40 || lcfg_cself_index(a.photo_cine_self) != 7) { strcpy_s(g_w428_why, sizeof g_w428_why, "W445 Standard nicht 4.0 m"); return; }
    ui_from_settings(&a);
    ui_apply_rules();
    if (IsWindowEnabled(ctl(ID_PH_CSELF))) { strcpy_s(g_w428_why, sizeof g_w428_why, "W445 Reichweite frei ohne Cinematic"); return; }
    lcfg_defaults(&a);
    a.photo_view = 3;
    a.photo_cine_self = 65; /* W445 */
    ui_from_settings(&a);
    ui_apply_rules();
    if (!IsWindowEnabled(ctl(ID_PH_CSELF))) { strcpy_s(g_w428_why, sizeof g_w428_why, "W445 Reichweite grau bei Cinematic"); return; }
    lcfg_defaults(&b);
    ui_to_settings(&b);
    if (b.photo_view != 3) { _snprintf_s(g_w428_why, sizeof g_w428_why, _TRUNCATE, "Rundweg Oberflaeche (%d)", b.photo_view); return; }
    if (b.photo_cine_self != 65) { _snprintf_s(g_w428_why, sizeof g_w428_why, _TRUNCATE, "W445 Rundweg Oberflaeche Reichweite (%d)", b.photo_cine_self); return; }
    u8_GetTempPath(tmp, sizeof tmp);
    _snprintf_s(f, sizeof f, _TRUNCATE, "%sw428_user_%lu.cfg", tmp, GetCurrentProcessId());
    u8_DeleteFile(f);
    if (lcfg_save_user(f, &b) != 0) { strcpy_s(g_w428_why, sizeof g_w428_why, "lcfg_save_user"); return; }
    lcfg_defaults(&a);
    if (lcfg_load_file(f, &a) != 0 || a.photo_view != 3 || a.photo_cine_self != 65) {
        _snprintf_s(g_w428_why, sizeof g_w428_why, _TRUNCATE, "Rundweg user.cfg (%d, Reichweite %d)", a.photo_view, a.photo_cine_self);
    }
    if (!g_w428_why[0]) {
        write_file(f, "[photo]\nphoto_view=4\nphoto_cine_self=999\n");
        lcfg_defaults(&a);
        if (lcfg_load_file(f, &a) != 0 || a.photo_view != 0 || a.photo_cine_self != LCFG_CSELF_HI) {
            _snprintf_s(g_w428_why, sizeof g_w428_why, _TRUNCATE, "photo_view=4 -> %d statt 0 / Reichweite 999 -> %d", a.photo_view,
                        a.photo_cine_self);
        }
    }
    if (!g_w428_why[0]) { /* W445: 0 = aus bleibt, krumme Werte aufs Raster */
        write_file(f, "[photo]\nphoto_cine_self=0\n");
        lcfg_defaults(&a);
        if (lcfg_load_file(f, &a) != 0 || a.photo_cine_self != 0 || lcfg_cself_clamp(7) != 10 || lcfg_cself_clamp(63) != 65) {
            _snprintf_s(g_w428_why, sizeof g_w428_why, _TRUNCATE, "W445 Reichweite 0/Raster (%d)", a.photo_cine_self);
        }
    }
    u8_DeleteFile(f);
    lcfg_defaults(&a);
    ui_from_settings(&a);
}

/* --- W299 REPLAY: Client-Taste "Replay beenden" (replay_exit) im Belegungsdialog -------------------------------
 * Rein: Element/Gruppe 5/Text, fehlende Zeile -> Standard Ruecktaste, eigene Belegung, leere Zeile bleibt beim
 * Speichern "replay_exit =" (sonst Standard im Client), "Alle leeren" behaelt sie, Tastenliste zeigt die Belegung. */
void w299_bind_selftest(int *pass, int *fail)
{
    char why[220] = "", tmp[1024], path[1200], *html, *txt;
    int re = lctrl_bind_elem_index("replay_exit");
    lctrl_bind_t b, rb;
    if (re < 0 || kCtrlElems[re].kind != LCTRL_ACT || lctrl_elem_group(re) != 5 || !lctrl_bind_combo_row(re) || /* W365: Kombis */
        !lctrl_bind_empty_line_row(re) || strcmp(LT("bind.replay_exit"), "bind.replay_exit") == 0)
        strcpy_s(why, sizeof why, "replay_exit: Element/Gruppe/Text");
    if (!why[0]) {
        lctrl_bind_load_text("[client]\npause = KEY:PAUSE\n", &b);
        if (!w290_row(&b, "replay_exit", LCTRL_REPLAY_EXIT_DEFAULT))
            strcpy_s(why, sizeof why, "altes Profil ohne Zeile -> Standard Ruecktaste");
    }
    if (!why[0]) {
        lctrl_bind_load_text("replay_exit = KEY:END, PAD:BACK+PAD:A\n", &b);
        if (!w290_row(&b, "replay_exit", "KEY:END, PAD:BACK+PAD:A"))
            strcpy_s(why, sizeof why, "eigene Belegung");
    }
    if (!why[0]) {
        lctrl_bind_load_text("replay_exit =\n", &b);
        if (b.row[re].n_alt != 0) strcpy_s(why, sizeof why, "leere Zeile = aus");
    }
    if (!why[0]) {
        lctrl_bind_load_text("replay_exit = KEY:END\nbi0 = KEY:SPACE\n", &b);
        lctrl_bind_clear_game(&b);
        if (!w290_row(&b, "replay_exit", "KEY:END") || b.row[0].n_alt != 0)
            strcpy_s(why, sizeof why, "Alle leeren behaelt replay_exit nicht");
    }
    if (!why[0]) { /* Rundweg Datei: leer bleibt leer, Standard bleibt Standard, Abschnitt [client] */
        u8_GetTempPath(tmp, sizeof tmp);
        _snprintf_s(path, sizeof path, _TRUNCATE, "%sw299_bind_test.ini", tmp);
        lctrl_bind_load_text("replay_exit =\n", &b);
        if (lctrl_bind_save(path, &b, "test", NULL, 0) != 0 || lctrl_bind_load_file(path, &rb) != 0 || rb.row[re].n_alt != 0)
            strcpy_s(why, sizeof why, "geleert gespeichert -> nach dem Laden nicht mehr leer");
        txt = (char *)malloc(65536);
        if (!why[0] && txt) {
            FILE *f = NULL;
            size_t n = 0;
            lctrl_bind_load_text("[client]\npause = KEY:PAUSE\n", &b);
            if (lctrl_bind_save(path, &b, "test", NULL, 0) == 0 && u8_fopen_s(&f, path, "rb") == 0 && f) {
                n = fread(txt, 1, 65535, f);
                fclose(f);
            }
            txt[n] = 0;
            if (!strstr(txt, "[client]") || !strstr(txt, "replay_exit = KEY:BACKSPACE"))
                strcpy_s(why, sizeof why, "Standard nicht als replay_exit-Zeile in [client] gespeichert");
        }
        free(txt);
        u8_DeleteFile(path);
    }
    html = (char *)malloc(600000);
    if (!why[0] && html) {
        keyshtml_build(LT("gui.keys_memo"), "", NULL, NULL, LT("gui.mouse_abs"), 4, "", "", "", html, 600000);
        if (!strstr(html, LT("bind.replay_exit")) || !strstr(html, "KEY:BACKSPACE"))
            strcpy_s(why, sizeof why, "Tastenliste Standard ohne Replay beenden");
        keyshtml_build(LT("gui.keys_memo"), "w299_test", "replay_exit = KEY:F7\n", NULL, LT("gui.mouse_abs"), 4,
                       "", "", "", html, 600000);
        if (!why[0] && (!strstr(html, "KEY:F7") || strstr(html, "KEY:BACKSPACE")))
            strcpy_s(why, sizeof why, "Tastenliste zeigt die Profil-Belegung nicht");
    }
    free(html);
    if (!why[0]) W282_OK("W299 REPLAYEXIT bind (Element/Gruppe 5, Standard Ruecktaste, eigene Belegung, leer = aus, Rundweg Datei, Alle leeren behaelt, Tastenliste)");
    else W282_FAIL("W299 REPLAYEXIT bind", why);
}

void w290_gui_selftest(const char *client_override, int *pass, int *fail)
{
    char tmp[1024], dir[1100], pd[1200], keepdir[sizeof g_client_dir];
    gui_opts_t keepo = g_o;
    w290_pure(pass, fail);
    u8_GetTempPath(tmp, sizeof tmp);
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%sw290_bind", tmp);
    _snprintf_s(pd, sizeof pd, _TRUNCATE, "%s/profiles", dir);
    lcfg_mkdir_p(pd);
    _snprintf_s(g_w290_prof, sizeof g_w290_prof, _TRUNCATE, "%s/user_w290Test.ini", pd);
    write_file(g_w290_prof, "[buttons]\nbi0 = KEY:SPACE\n[client]\npause = KEY:PAUSE\nphoto = KEY:F12\n");
    g_w290_why[0] = 0;
    if (!(gui_init(client_override) && gui_create(1))) {
        strcpy_s(g_w290_why, sizeof g_w290_why, "Fenster nicht erzeugt");
    } else {
        build_controls();
        tips_build();
        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE); /* ausserhalb des Bildschirms */
        w290_card();
        if (!g_w290_why[0]) { w428_cine_card(); } /* W428 */
        else { strcpy_s(g_w428_why, sizeof g_w428_why, "W290-Karte vorher rot"); }
        strcpy_s(keepdir, sizeof keepdir, g_client_dir);
        strcpy_s(g_client_dir, sizeof g_client_dir, dir); /* nie das echte Profil */
        g_o.shot = 3;
        g_o.bind_sel = -1;
        g_o.bind_learn = g_o.bind_joydemo = g_o.bind_stickdemo = g_o.bind_wheeldemo = g_o.bind_combodemo = 0;
        if (!g_w290_why[0]) {
            g_bind_testfn = w290_bind_probe;
            bind_dialog_modal("user_w290Test", g_w290_prof);
            g_bind_testfn = NULL;
        }
        g_o = keepo;
        strcpy_s(g_client_dir, sizeof g_client_dir, keepdir);
        ShowWindow(g_hwnd, SW_HIDE);
        gui_teardown();
    }
    u8_DeleteFile(g_w290_prof);
    {
        wchar_t w[1200];
        u8_to_w(pd, w, 1200);
        RemoveDirectoryW(w);
        u8_to_w(dir, w, 1200);
        RemoveDirectoryW(w);
    }
    if (!g_w290_why[0]) W282_OK("W290 QUICKSHOT gui (Karte Fotos & Screenshots auf Ton & Extras: Elemente, Kolben, Listen, Standard, Rundweg Oberflaeche/user.cfg, alte cfg; Belegungsdialog: altes Profil -> F12/Shift+F12, Lernen Strg+F12 / Druck / Umschalt+F12, kein Konflikt, Alle leeren behaelt, Datei [client], leer bleibt leer)");
    else W282_FAIL("W290 QUICKSHOT gui", g_w290_why);
    if (!g_w428_why[0]) W282_OK("W428 CINEMATIC gui (Ansicht Cinematic als 4. Eintrag, Rundweg Oberflaeche/user.cfg photo_view=3, photo_view=4 -> wie im Spiel)");
    else W282_FAIL("W428 CINEMATIC gui", g_w428_why);
}
#undef W290_WHY

/* ---- W303 AUDIO-VOLUME: Karte "Ton & Lautstaerke" (Seite Ton & Extras) ----
 * Elemente auf der Seite, Listen 100..0 %, Standard 100 (Original), Lage
 * ueber der Karte "Fotos & Screenshots", Rundweg Oberflaeche <-> user.cfg,
 * nicht auf 5 gerundete Werte bleiben, Klemmen beim Laden. */
static char g_w303_why[300];
#define W303_WHY(...) do { if (!g_w303_why[0]) _snprintf_s(g_w303_why, sizeof g_w303_why, _TRUNCATE, __VA_ARGS__); } while (0)
static void w303_card(void)
{
    launcher_settings_t a, b;
    char f[1300], tmp[1024];
    int k;
    for (k = 0; k < 4; k++) {
        uictl_t *u = ui_find(kVolId[k]);
        if (!u || !ui_visible_on(u, P_AUDIO) || ui_visible_on(u, P_GFX)) { W303_WHY("Regler %d fehlt/falsche Seite", kVolId[k]); return; }
        if (ComboBox_GetCount(u->h) != VOL_N) { W303_WHY("Regler %d: %d Eintraege", kVolId[k], ComboBox_GetCount(u->h)); return; }
        if (u->y + u->h_ > CY0 + 140 + EXY || u->y + u->h_ > CY0 + 152 + EXY) { W303_WHY("Regler %d ragt in die Fotokarte", kVolId[k]); return; }
    }
    {
        uictl_t *d = ui_find(ID_PH_DIR), *t = ui_find(ID_AUDIO);
        if (!d || d->y < CY0 + 152 + EXY || !t || t->y + t->h_ > CY0 + 82) { W303_WHY("Lage Ton-Schalter/Fotokarte"); return; }
    }
    { /* W356 AUDIO-2: Schalter "Kopfhoerer" rechts neben "Ton an", in der Tonkarte, ueber den Reglern */
        uictl_t *h = ui_find(ID_HRTF), *t = ui_find(ID_AUDIO);
        if (!h || !t || !ui_visible_on(h, P_AUDIO) || ui_visible_on(h, P_GFX) || h->x < t->x + t->w ||
            h->y + h->h_ > CY0 + 82 || h->x + h->w > CX0 + COLW - 18) {
            W303_WHY("W356 Kopfhoerer-Schalter fehlt/Lage");
            return;
        }
    }
    lcfg_defaults(&a);
    if (a.volume_master != 100 || a.volume_music != 100 || a.volume_sfx != 100 || a.volume_voice != 100) {
        W303_WHY("Standard nicht 100 (Original)");
        return;
    }
    if (a.audio_hrtf != 0) { W303_WHY("W356 Kopfhoerer standardmaessig an"); return; }
    ui_from_settings(&a);
    for (k = 0; k < 4; k++) {
        if (ComboBox_GetCurSel(ctl(kVolId[k])) != 0) { W303_WHY("Standard-Auswahl %d nicht 100 %%", kVolId[k]); return; }
    }
    a.volume_master = 80;
    a.volume_music = 45;
    a.volume_sfx = 0;
    a.volume_voice = 83; /* nicht auf 5: Anzeige 85, Wert bleibt */
    a.audio_hrtf = 1; /* W356 */
    ui_from_settings(&a);
    lcfg_defaults(&b);
    ui_to_settings(&b);
    if (b.audio_hrtf != 1 || !ui_checked(ID_HRTF)) { W303_WHY("W356 Rundweg Kopfhoerer (Oberflaeche)"); return; }
    if (b.volume_master != 80 || b.volume_music != 45 || b.volume_sfx != 0 || b.volume_voice != 83) {
        W303_WHY("Rundweg Oberflaeche (%d/%d/%d/%d)", b.volume_master, b.volume_music, b.volume_sfx, b.volume_voice);
        return;
    }
    if (ComboBox_GetCurSel(ctl(ID_VOL_VOICE)) != 3) { W303_WHY("83 nicht als 85 %% angezeigt"); return; }
    ComboBox_SetCurSel(ctl(ID_VOL_VOICE), 2); /* Nutzer waehlt 90 % */
    ui_to_settings(&b);
    if (b.volume_voice != 90) { W303_WHY("Auswahl 90 %% -> %d", b.volume_voice); return; }
    u8_GetTempPath(tmp, sizeof tmp);
    _snprintf_s(f, sizeof f, _TRUNCATE, "%sw303_user_%lu.cfg", tmp, GetCurrentProcessId());
    u8_DeleteFile(f);
    if (lcfg_save_user(f, &b) != 0) { W303_WHY("lcfg_save_user"); return; }
    {
        FILE *fp;
        char buf[60000];
        size_t got;
        buf[0] = 0;
        if (u8_fopen_s(&fp, f, "rb") == 0 && fp) { got = fread(buf, 1, sizeof buf - 1, fp); buf[got] = 0; fclose(fp); }
        if (!strstr(buf, "volume_master=80") || !strstr(buf, "volume_music=45") || !strstr(buf, "volume_sfx=0") ||
            !strstr(buf, "volume_voice=90") || !strstr(buf, "[audio]")) {
            W303_WHY("user.cfg ohne volume_* unter [audio]");
        }
        if (!strstr(buf, "audio_hrtf=1"))
            W303_WHY("W356 user.cfg ohne audio_hrtf=1");
    }
    lcfg_defaults(&a);
    if (!g_w303_why[0] && (lcfg_load_file(f, &a) != 0 || a.volume_master != 80 || a.volume_music != 45 || a.volume_sfx != 0 ||
                           a.volume_voice != 90 || a.audio_hrtf != 1))
        W303_WHY("Rundweg user.cfg");
    if (!g_w303_why[0]) {
        write_file(f, "[audio]\nvolume_master=150\nvolume_music=-3\nvolume_sfx=abc\n");
        lcfg_defaults(&a);
        if (lcfg_load_file(f, &a) != 0 || a.volume_master != 100 || a.volume_music != 0 || a.volume_sfx != 0 ||
            a.volume_voice != 100)
            W303_WHY("Klemmen (%d/%d/%d/%d)", a.volume_master, a.volume_music, a.volume_sfx, a.volume_voice);
    }
    u8_DeleteFile(f);
    lcfg_defaults(&a);
    ui_from_settings(&a);
}

void w303_gui_selftest(const char *client_override, int *pass, int *fail)
{
    g_w303_why[0] = 0;
    if (!(gui_init(client_override) && gui_create(1))) {
        strcpy_s(g_w303_why, sizeof g_w303_why, "Fenster nicht erzeugt");
    } else {
        build_controls();
        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE); /* ausserhalb des Bildschirms */
        w303_card();
        ShowWindow(g_hwnd, SW_HIDE);
        gui_teardown();
    }
    if (!g_w303_why[0]) W282_OK("W303 AUDIO-VOLUME gui (Karte Ton & Lautstaerke: 4 Regler auf Ton & Extras, Listen 100..0 %, Standard 100 = Original, Lage ueber Fotokarte, Rundweg Oberflaeche/user.cfg [audio], Rohwert 83 bleibt, Klemmen; W356 Kopfhoerer-Schalter: Lage, Standard aus, Rundweg audio_hrtf)");
    else W282_FAIL("W303 AUDIO-VOLUME gui", g_w303_why);
}
#undef W303_WHY

/* ---- W419 CTRL-EXPORT: Steuerung exportieren / importieren ----------------------------------------------
 * Rein: Rundreise Export -> Import (Belegung, joyN, Seitenwerte, user.cfg-Werte gleich; keine Pfade/fremden
 * Werte in der Datei), user.cfg-Uebernahme (fremde Zeilen + fremde Geraete-Kalibrierung bleiben), kaputte bzw.
 * boesartige Dateien (zu gross, Pfad im Namen, unbekannt, ausserhalb des Bereichs, Binaermuell, abgeschnitten,
 * kein Kopf, neueres Format, Zeile zu lang, fremde Zeichen) -> Fehler + leere Daten, Namenskollision,
 * fehlendes Geraet. Oberflaeche: Knoepfe in der Karte (EN/DE ohne Ueberlappung), Import-Pruefung aendert bei
 * Fehler nichts, Import legt neues Profil an (-2 bei Kollision), waehlt es, uebernimmt die Seitenwerte. */
static char g_w419_why[260];
#define W419_WHY(...) do { if (!g_w419_why[0]) _snprintf_s(g_w419_why, sizeof g_w419_why, _TRUNCATE, __VA_ARGS__); } while (0)

static int w419_bind_eq(const lctrl_bind_t *a, const lctrl_bind_t *b)
{
    int i, k;
    for (i = 0; i < IMAP_JOY_MAX; i++) {
        if (strcmp(a->joy[i], b->joy[i]) != 0) { return 0; }
    }
    for (i = 0; i < LCTRL_ELEM_N; i++) {
        if (a->row[i].n_alt != b->row[i].n_alt) { return 0; }
        for (k = 0; k < a->row[i].n_alt; k++) {
            if (strcmp(a->row[i].alt[k], b->row[i].alt[k]) != 0) { return 0; }
        }
    }
    return 1;
}

static size_t w419_slurp(const char *path, char *buf, size_t cap)
{
    FILE *f = NULL;
    size_t n = 0;
    buf[0] = '\0';
    if (u8_fopen_s(&f, path, "rb") == 0 && f) {
        n = fread(buf, 1, cap - 1, f);
        fclose(f);
    }
    buf[n] = '\0';
    return n;
}

/* Kaputte Datei: erwarteter Fehler, Daten leer, verstaendliche Meldung (nicht der Schluessel selbst). */
static void w419_bad(const char *what, const char *text, size_t n, int want)
{
    static lcx_data_t d;
    char err[300];
    int rc = lcx_parse(text, n, &d, err, sizeof err);
    if (rc != want) { W419_WHY("%s: rc %d statt %d (%s)", what, rc, want, err); return; }
    if (!err[0] || strncmp(err, "gui.", 4) == 0 || d.n_set || d.profile[0] || d.bind.row[0].n_alt) {
        W419_WHY("%s: Meldung/Daten nicht leer", what);
    }
}

static void w419_pure(const char *dir)
{
    static lcx_data_t d, e;
    static lctrl_bind_t b0, b1;
    static char buf[LCX_MAX_BYTES + 8192], big[LCX_MAX_BYTES + 4096], cfg[8192];
    static launcher_settings_t s0, s1;
    static ijoy_info_t list[2];
    char uc[1200], uc2[1200], fx[1200], pp[1200], name[64], err[300], miss[8][96];
    int rc, i, n;
    size_t len;
    _snprintf_s(uc, sizeof uc, _TRUNCATE, "%s/user.cfg", dir);
    _snprintf_s(uc2, sizeof uc2, _TRUNCATE, "%s/user2.cfg", dir);
    _snprintf_s(fx, sizeof fx, _TRUNCATE, "%s/test.sbcontrols", dir);
    /* 1. Rundreise: Fixture-Profil + Seitenwerte + user.cfg (mit fremden Werten und Pfaden) */
    lctrl_bind_load_text("[joystick]\njoy0 = T.16000M\njoy2 = {B679044F-0000-0000-0000-504944564944}\n"
                         "bi0 = KEY:SPACE, MOUSE:LEFT, JOY0:B1, PAD:RB\nbi1 = KEY:F, KEY:CTRL+KEY:S\n"
                         "aim_x = MOUSE:X:mode=aim, PAD:LX:dz=0.15:curve=1.5:mode=rate, JOY0:X:dz=0.05\n"
                         "pedal_r = KEY:W, JOY2:Z:zone=20-40:inv=1\ngear.up = KEY:E, JOY0:B2+JOY0:B3\n"
                         "chaff = SBC:CHAFF\norig_view =\nquick_shot = KEY:F12\nnet_hud =\n", &b0);
    lcfg_defaults(&s0);
    s0.mouse_mode = 0;
    s0.mouse_sens = 11;
    s0.stick_aim = 1;
    s0.stick_aim_sens = 7;
    s0.aim_assist = 2;
    s0.auto_gear = 1;
    s0.sbc_usb = 1;
    s0.sbc_mouse_off = 1;
    {
        static const char t[] = "[render]\r\nwidth=1280\r\n[paths]\r\nxbe_path=Someone\\Games\\default.xbe\r\n"
                                "[input]\r\ninput_profile=user_secret\r\npad_dz_stick=12\r\npad_dz_trig=7\r\n"
                                "sbc_cal_rot=-100,0,100\r\nsbc_dead_rot=5\r\njoy_cal0=044F:B10A,X,-30000,200,31000,4,0\r\n"
                                "mouse_invert_y=1\r\nstick_aim_accel=2\r\naim_assist_edge=60\r\nmouse_capture=1\r\n"
                                "pad_dz_bogus=3\r\njoy_cal1=kaputt\r\n[net]\r\nsteelnet_token=abc\r\n";
        cx_put_text(uc, t, sizeof t - 1);
    }
    memset(&d, 0, sizeof d);
    d.format = LCX_FORMAT;
    strcpy_s(d.launcher, sizeof d.launcher, "0.3.2");
    strcpy_s(d.game, sizeof d.game, "sb");
    strcpy_s(d.profile, sizeof d.profile, "user_w419");
    d.bind = b0;
    lcx_put_settings(&d, &s0);
    n = lcx_put_cfg(&d, uc);
    if (n != 8) { W419_WHY("user.cfg-Werte: %d statt 8", n); }
    if (lcx_write_file(fx, &d) != 0) { W419_WHY("Export schreiben"); return; }
    len = w419_slurp(fx, buf, sizeof buf);
    if (strstr(buf, "xbe_path") || strstr(buf, "Someone") || strstr(buf, "width") || strstr(buf, "steelnet") ||
        strstr(buf, "input_profile") || strstr(buf, "mouse_capture") || strstr(buf, "bogus") || strstr(buf, "kaputt") ||
        strchr(buf, '\\') || !strstr(buf, "[end]") || !strstr(buf, "format = 1") || !strstr(buf, "game = sb")) {
        W419_WHY("Exportdatei: fremde Werte/Pfade oder Kopf fehlt");
    }
    rc = lcx_read_file(fx, &e, err, sizeof err);
    if (rc != LCX_OK) { W419_WHY("Rundreise lesen: %d %s", rc, err); return; }
    if (!w419_bind_eq(&b0, &e.bind)) { W419_WHY("Rundreise: Belegung/joyN verschieden"); }
    if (e.n_set != d.n_set || e.unknown != 0 || strcmp(e.profile, "user_w419") != 0 || strcmp(e.game, "sb") != 0) {
        W419_WHY("Rundreise: Kopf/Werte %d/%d unbekannt %d", e.n_set, d.n_set, e.unknown);
    }
    for (i = 0; i < d.n_set && i < e.n_set; i++) {
        if (strcmp(d.set[i], e.set[i]) != 0) { W419_WHY("Rundreise: Wert %s != %s", d.set[i], e.set[i]); }
    }
    lcfg_defaults(&s1);
    if (lcx_get_settings(&e, &s1) != 8 || s1.mouse_mode != 0 || s1.mouse_sens != 11 || s1.stick_aim != 1 ||
        s1.stick_aim_sens != 7 || s1.aim_assist != 2 || s1.auto_gear != 1 || s1.sbc_usb != 1 || s1.sbc_mouse_off != 1) {
        W419_WHY("Rundreise: Seitenwerte");
    }
    _snprintf_s(pp, sizeof pp, _TRUNCATE, "%s/rt.ini", dir); /* Profil wie der Import es speichert */
    if (lctrl_bind_save(pp, &e.bind, "user_w419", NULL, 0) != 0 || lctrl_bind_load_file(pp, &b1) != 0 ||
        !w419_bind_eq(&b0, &b1)) {
        W419_WHY("Rundreise: gespeichertes Profil verschieden");
    }
    u8_DeleteFile(pp);
    /* 2. user.cfg-Uebernahme: fremde Zeilen + fremde Geraete-Kalibrierung bleiben, gleiche ersetzt */
    {
        static const char t2[] = "# eigene Notiz\r\n[input]\r\npad_dz_stick=30\r\njoy_cal0=046D:C283,X,-26000,1500,30500,5,0\r\n"
                                 "joy_cal1=044F:B10A,X,-1000,-,1000,0,0\r\nfremd=1\r\n[audio]\r\nvolume_master=80\r\n";
        cx_put_text(uc2, t2, sizeof t2 - 1);
    }
    if (lcx_apply_cfg(&e, uc2) != 0) { W419_WHY("user.cfg schreiben"); }
    w419_slurp(uc2, cfg, sizeof cfg);
    if (!strstr(cfg, "pad_dz_stick=12") || strstr(cfg, "pad_dz_stick=30") || !strstr(cfg, "pad_dz_trig=7") ||
        !strstr(cfg, "sbc_cal_rot=-100,0,100") || !strstr(cfg, "mouse_invert_y=1") || !strstr(cfg, "stick_aim_accel=2") ||
        !strstr(cfg, "aim_assist_edge=60") || !strstr(cfg, "046D:C283,X,-26000,1500,30500,5,0") ||
        !strstr(cfg, "044F:B10A,X,-30000,200,31000,4,0") || strstr(cfg, "044F:B10A,X,-1000") ||
        !strstr(cfg, "fremd=1") || !strstr(cfg, "# eigene Notiz") || !strstr(cfg, "volume_master=80") ||
        strstr(cfg, "mouse_sens") || strstr(cfg, "sbc_usb")) {
        W419_WHY("user.cfg nach Import: %.120s", cfg);
    }
    /* 3. Kaputte bzw. boesartige Dateien */
    memset(big, '#', sizeof big);
    memcpy(big, "[steelbound_controls]\nformat = 1\nprofile = x\n", 45);
    for (i = 60; i < (int)sizeof big; i += 80) { big[i] = '\n'; }
    memcpy(big + sizeof big - 8, "\n[end]\n", 7);
    w419_bad("zu gross", big, sizeof big - 1, LCX_E_TOOBIG);
    cx_put_text(fx, big, sizeof big - 1);
    if (lcx_read_file(fx, &e, err, sizeof err) != LCX_E_TOOBIG) { W419_WHY("zu gross (Datei)"); }
    {
        static const char *const hdr = "[steelbound_controls]\nformat = 1\n";
        static char t[2048];
        _snprintf_s(t, sizeof t, _TRUNCATE, "%sprofile = ..\\..\\evil\n[profile]\nbi0 = KEY:A\n[end]\n", hdr);
        w419_bad("Pfad im Namen", t, strlen(t), LCX_E_NAME);
        _snprintf_s(t, sizeof t, _TRUNCATE, "%sprofile = C:/Windows/x\n[end]\n", hdr);
        w419_bad("Laufwerk im Namen", t, strlen(t), LCX_E_NAME);
        _snprintf_s(t, sizeof t, _TRUNCATE, "%s[profile]\nbi0 = KEY:A\n[end]\n", hdr);
        w419_bad("Name fehlt", t, strlen(t), LCX_E_NAME);
        _snprintf_s(t, sizeof t, _TRUNCATE, "%sprofile = a\n[settings]\nmouse_sens = 99\n[end]\n", hdr);
        w419_bad("mouse_sens 99", t, strlen(t), LCX_E_VALUE);
        _snprintf_s(t, sizeof t, _TRUNCATE, "%sprofile = a\n[settings]\nmouse_mode = fast\n[end]\n", hdr);
        w419_bad("mouse_mode fast", t, strlen(t), LCX_E_VALUE);
        _snprintf_s(t, sizeof t, _TRUNCATE, "%sprofile = a\n[settings]\njoy_cal3 = 1:2,Q,9,9,1,0,0\n[end]\n", hdr);
        w419_bad("joy_cal kaputt", t, strlen(t), LCX_E_VALUE);
        _snprintf_s(t, sizeof t, _TRUNCATE, "%sprofile = a\n[settings]\nsbc_cal_rot = 5;del\n[end]\n", hdr);
        w419_bad("sbc_cal Zeichen", t, strlen(t), LCX_E_VALUE);
        _snprintf_s(t, sizeof t, _TRUNCATE, "%sprofile = a\n[profile]\naim_x = PAD:LX:dz=5\n[end]\n", hdr);
        w419_bad("dz 5", t, strlen(t), LCX_E_VALUE);
        _snprintf_s(t, sizeof t, _TRUNCATE, "%sprofile = a\n[profile]\nbi0 = KEY:A;calc.exe\n[end]\n", hdr);
        w419_bad("Quelle mit fremden Zeichen", t, strlen(t), LCX_E_VALUE);
        _snprintf_s(t, sizeof t, _TRUNCATE, "%sprofile = a\n[profile]\nbi0 = KEY:A, KEY:B, KEY:C, KEY:D, KEY:E\n[end]\n", hdr);
        w419_bad("5 Quellen", t, strlen(t), LCX_E_VALUE);
        _snprintf_s(t, sizeof t, _TRUNCATE, "%sprofile = a\n[profile]\npedal_r = JOY0:Z:zone=40-200\n[end]\n", hdr);
        w419_bad("zone 200", t, strlen(t), LCX_E_VALUE);
        _snprintf_s(t, sizeof t, _TRUNCATE, "[steelbound_controls]\nformat = 2\nprofile = a\n[end]\n");
        w419_bad("neueres Format", t, strlen(t), LCX_E_NEWER);
        _snprintf_s(t, sizeof t, _TRUNCATE, "[buttons]\nbi0 = KEY:SPACE\n");
        w419_bad("Profil statt Export", t, strlen(t), LCX_E_NOTCX);
        w419_bad("leer", "", 0, LCX_E_NOTCX);
        _snprintf_s(t, sizeof t, _TRUNCATE, "%sprofile = a\n[profile]\nbi0 = KEY:", hdr);
        {
            size_t p0 = strlen(t);
            memset(t + p0, 'A', 700);
            t[p0 + 700] = '\0';
        }
        strcat_s(t, sizeof t, "\n[end]\n");
        w419_bad("Zeile zu lang", t, strlen(t), LCX_E_LINE);
        /* unbekannte Abschnitte/Schluessel/Quellen: ignoriert, gezaehlt, Rest gilt */
        _snprintf_s(t, sizeof t, _TRUNCATE, "%sprofile = user_x\nfuture = 1\n[settings]\nmouse_sens = 3\nwidth = 3840\n"
                    "xbe_path = x.xbe\n[graphics]\nshading = 1\n[profile]\nbi0 = KEY:A, MIDI:C4, PAD:A\nlaser = KEY:L\n"
                    "[end]\n", hdr);
        rc = lcx_parse(t, strlen(t), &e, err, sizeof err);
        if (rc != LCX_OK || e.unknown != 7 || e.n_set != 1 || strcmp(e.set[0], "mouse_sens=3") != 0 ||
            e.bind.row[0].n_alt != 2 || strcmp(e.bind.row[0].alt[1], "PAD:A") != 0) {
            W419_WHY("unbekannt: rc %d unbekannt %d Werte %d", rc, e.unknown, e.n_set);
        }
    }
    {
        static char bin[600];
        for (i = 0; i < (int)sizeof bin; i++) { bin[i] = (char)((i * 37 + 11) & 0xFF); }
        memcpy(bin, "[steelbound_controls]\n", 22);
        w419_bad("Binaermuell", bin, sizeof bin, LCX_E_BINARY);
    }
    len = w419_slurp(fx, buf, sizeof buf); /* fx = zu grosse Datei: neu schreiben */
    cx_put_text(fx, kCxFixture, sizeof kCxFixture - 1);
    len = w419_slurp(fx, buf, sizeof buf);
    w419_bad("abgeschnitten", buf, len / 2, LCX_E_TRUNC);
    w419_bad("abgeschnitten (ohne [end])", buf, len - 7, LCX_E_TRUNC);
    rc = lcx_parse(buf, len, &e, err, sizeof err);
    if (rc != LCX_OK || e.unknown != 2 || strcmp(e.profile, "user_hotas") != 0) {
        W419_WHY("Beispieldatei: rc %d unbekannt %d", rc, e.unknown);
    }
    /* 4. Namenskollision + Bereinigung */
    _snprintf_s(pp, sizeof pp, _TRUNCATE, "%s/profiles", dir);
    lcfg_mkdir_p(pp);
    _snprintf_s(pp, sizeof pp, _TRUNCATE, "%s/profiles/user_hotas.ini", dir);
    cx_put_text(pp, "bi0 = KEY:A\n", 12);
    _snprintf_s(pp, sizeof pp, _TRUNCATE, "%s/profiles/user_hotas-2.ini", dir);
    cx_put_text(pp, "bi0 = KEY:A\n", 12);
    if (lcx_import_name(dir, "user_hotas", name, sizeof name) != 0 || strcmp(name, "user_hotas-3") != 0) {
        W419_WHY("Kollision: %s", name);
    }
    if (lcx_import_name(dir, "My Layout \xC3\xBC!", name, sizeof name) != 0 || strcmp(name, "user_My_Layout___") != 0) {
        W419_WHY("Bereinigung: %s", name);
    }
    if (lcx_import_name(dir, "keyboard_default", name, sizeof name) != 0 || strcmp(name, "user_keyboard_default") != 0) {
        W419_WHY("mitgeliefert -> user_: %s", name);
    }
    if (lcx_import_name(dir, "...", name, sizeof name) != 0 || strcmp(name, "user_imported") != 0) {
        W419_WHY("nur Punkte: %s", name);
    }
    if (lcx_import_name(dir, "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJ", name, sizeof name) != 0 ||
        strlen(name) != 45 || !lctrl_user_rest_ok(name + 5)) {
        W419_WHY("lang: %s", name);
    }
    u8_DeleteFile(pp); /* user_hotas-2.ini */
    _snprintf_s(pp, sizeof pp, _TRUNCATE, "%s/profiles/user_hotas.ini", dir);
    u8_DeleteFile(pp);
    /* 5. Fehlende Geraete (nur Namen) */
    memset(list, 0, sizeof list);
    strcpy_s(list[0].name, sizeof list[0].name, "Logitech Extreme 3D");
    strcpy_s(list[0].product, sizeof list[0].product, "Logitech Extreme 3D");
    n = lcx_missing(&e, list, 1, 0, miss, 8); /* Beispieldatei: T.16000M + TWCS Throttle genutzt */
    if (n != 2 || strcmp(miss[0], "T.16000M") != 0 || strcmp(miss[1], "TWCS Throttle") != 0) {
        W419_WHY("fehlend: %d", n);
    }
    strcpy_s(list[1].name, sizeof list[1].name, "Thrustmaster T.16000M");
    strcpy_s(list[1].product, sizeof list[1].product, "T.16000M");
    n = lcx_missing(&e, list, 2, 0, miss, 8);
    if (n != 1 || strcmp(miss[0], "TWCS Throttle") != 0) { W419_WHY("fehlend mit Geraet: %d", n); }
    rc = lcx_read_file(fx, &e, err, sizeof err);
    lctrl_bind_set(&e.bind, lctrl_bind_elem_index("chaff"), "SBC:CHAFF");
    n = lcx_missing(&e, NULL, 0, 0, miss, 8);
    if (n != 3 || strcmp(miss[2], LT("gui.cx_sbc_name")) != 0 || lcx_missing(&e, NULL, 0, 1, miss, 8) != 2) {
        W419_WHY("Original-Controller fehlt: %d", n);
    }
    u8_DeleteFile(fx);
    u8_DeleteFile(uc);
    u8_DeleteFile(uc2);
}

/* Oberflaeche: Knoepfe, Import-Pruefung ohne Aenderung, Import mit Kollision. */
static void w419_gui(const char *dir)
{
    static char before[4096], after[4096];
    uictl_t *ex = ui_find(ID_CTRL_EXPORT), *im = ui_find(ID_CTRL_IMPORT), *pr = ui_find(ID_CTRL_PROF);
    char fx[1200], pp[1200], q[1400], msg[600], keepdir[sizeof g_client_dir], lang[8];
    int l, sel;
    if (!ex || !im || !pr || !ui_visible_on(ex, P_CTRL) || !ui_visible_on(im, P_CTRL) || ui_visible_on(ex, P_GFX)) {
        W419_WHY("Knoepfe fehlen/falsche Seite");
        return;
    }
    if (ex->y < CY0 + 260 || ex->y + ex->h_ > pr->y || im->y != ex->y || ex->x + ex->w > im->x ||
        im->x + im->w > CX0 + COLW - 18) {
        W419_WHY("Lage der Knoepfe (Karte/Ueberlappung)");
        return;
    }
    strcpy_s(lang, sizeof lang, sb_lang_code());
    for (l = 0; l < 2; l++) { /* Kartentitel EN/DE endet vor den Knoepfen */
        int tw;
        sb_lang_set(l ? "de" : "en");
        tw = hdr_text_w(F_CARD, LT("gui.ctl_profile"), 0);
        if (CX0 + 18 + tw + 6 > ex->x) { W419_WHY("Titel %s ueberlappt Export (%d)", l ? "DE" : "EN", tw); }
        if (hdr_text_w(F_BODY, LT("gui.cx_export"), 0) + 12 > ex->w || hdr_text_w(F_BODY, LT("gui.cx_import"), 0) + 12 > im->w) {
            W419_WHY("Knopftext %s zu breit", l ? "DE" : "EN");
        }
    }
    sb_lang_set(lang);
    strcpy_s(keepdir, sizeof keepdir, g_client_dir);
    strcpy_s(g_client_dir, sizeof g_client_dir, dir); /* nie die echten Profile */
    _snprintf_s(pp, sizeof pp, _TRUNCATE, "%s/profiles/user_hotas.ini", dir);
    cx_put_text(pp, "bi0 = KEY:A\n", 12);
    w419_slurp(pp, before, sizeof before);
    _snprintf_s(fx, sizeof fx, _TRUNCATE, "%s/gui.sbcontrols", dir);
    /* kaputt: Meldung, nichts geaendert (kein neues Profil, Auswahl bleibt) */
    refresh_ctrl_profiles("user_hotas");
    sel = ComboBox_GetCurSel(ctl(ID_CTRL_PROF));
    cx_put_text(fx, kCxFixture, sizeof kCxFixture / 3);
    if (cx_import_check(fx, NULL, 0, 0, q, sizeof q) == 0 || !strstr(q, LT("gui.cx_nochange")) || g_cx_name[0]) {
        W419_WHY("kaputte Datei nicht abgelehnt");
    }
    _snprintf_s(pp, sizeof pp, _TRUNCATE, "%s/profiles/user_hotas-2.ini", dir);
    if (u8_GetFileAttributes(pp) != INVALID_FILE_ATTRIBUTES || ComboBox_GetCurSel(ctl(ID_CTRL_PROF)) != sel) {
        W419_WHY("kaputte Datei hat etwas geaendert");
    }
    /* gueltig: Rueckfrage nennt Namen -2, fehlende Geraete; Import legt an + waehlt + Seitenwerte */
    cx_put_text(fx, kCxFixture, sizeof kCxFixture - 1);
    if (cx_import_check(fx, NULL, 0, 0, q, sizeof q) != 0 || strcmp(g_cx_name, "user_hotas-2") != 0 ||
        !strstr(q, "user_hotas-2") || !strstr(q, "T.16000M") || !strstr(q, "TWCS Throttle")) {
        W419_WHY("Rueckfrage: %.160s", q);
    }
    if (cx_import_apply(0, msg, sizeof msg) != 0) {
        W419_WHY("Import: %s", msg);
    } else {
        char cur[64];
        launcher_settings_t t = g_s;
        ui_to_settings(&t);
        ctrl_profile_from_ui(cur, sizeof cur);
        if (strcmp(cur, "user_hotas-2") != 0 || u8_GetFileAttributes(pp) == INVALID_FILE_ATTRIBUTES ||
            t.mouse_sens != 6 || t.mouse_mode != 0 || t.stick_aim != 1 || t.stick_aim_sens != 5 ||
            strcmp(t.input_profile, "user_hotas-2") != 0) {
            W419_WHY("nach Import: Auswahl %s, sens %d", cur, t.mouse_sens);
        }
        _snprintf_s(pp, sizeof pp, _TRUNCATE, "%s/profiles/user_hotas.ini", dir);
        w419_slurp(pp, after, sizeof after);
        if (strcmp(before, after) != 0) { W419_WHY("bestehendes Profil veraendert"); }
        /* Export des neuen Profils = gleiche Belegung */
        if (cx_export_to(fx, msg, sizeof msg) != 0) {
            W419_WHY("Export: %s", msg);
        } else {
            static lcx_data_t e;
            static lctrl_bind_t b;
            char err[300];
            _snprintf_s(pp, sizeof pp, _TRUNCATE, "%s/profiles/user_hotas-2.ini", dir);
            lctrl_bind_load_file(pp, &b);
            if (lcx_read_file(fx, &e, err, sizeof err) != LCX_OK || !w419_bind_eq(&b, &e.bind) ||
                strcmp(e.profile, "user_hotas-2") != 0) {
                W419_WHY("Export nach Import verschieden (%s)", err);
            }
        }
        u8_DeleteFile(pp);
    }
    _snprintf_s(pp, sizeof pp, _TRUNCATE, "%s/profiles/user_hotas.ini", dir);
    u8_DeleteFile(pp);
    u8_DeleteFile(fx);
    strcpy_s(g_client_dir, sizeof g_client_dir, keepdir);
}

void w419_gui_selftest(const char *client_override, int *pass, int *fail)
{
    char tmp[1024], dir[1100], pd[1200];
    u8_GetTempPath(tmp, sizeof tmp);
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%sw419_cx_%lu", tmp, GetCurrentProcessId());
    _snprintf_s(pd, sizeof pd, _TRUNCATE, "%s/profiles", dir);
    lcfg_mkdir_p(pd);
    g_w419_why[0] = 0;
    w419_pure(dir);
    if (!g_w419_why[0]) W282_OK("W419 CTRL-EXPORT pure (Rundreise Belegung/joyN/Seitenwerte/user.cfg, keine Pfade/fremden Werte, user.cfg-Uebernahme mit fremden Zeilen und fremder Geraete-Kalibrierung, 20 kaputte/boesartige Dateien abgelehnt + leer, Unbekanntes gezaehlt, Kollision -3, Bereinigung, fehlende Geraete)");
    else W282_FAIL("W419 CTRL-EXPORT pure", g_w419_why);
    g_w419_why[0] = 0;
    g_in_gui_selftest = 1;
    if (!(gui_init(client_override) && gui_create(1))) {
        strcpy_s(g_w419_why, sizeof g_w419_why, "Fenster nicht erzeugt");
    } else {
        build_controls();
        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE); /* ausserhalb des Bildschirms */
        w419_gui(dir);
        ShowWindow(g_hwnd, SW_HIDE);
        gui_teardown();
    }
    g_in_gui_selftest = 0;
    {
        wchar_t w[1200];
        u8_to_w(pd, w, 1200);
        RemoveDirectoryW(w);
        u8_to_w(dir, w, 1200);
        RemoveDirectoryW(w);
    }
    if (!g_w419_why[0]) W282_OK("W419 CTRL-EXPORT gui (Export/Import in der Karte Tastenbelegung, Titel EN/DE ohne Ueberlappung, kaputte Datei aendert nichts, Rueckfrage mit Name -2 und fehlenden Geraeten, neues Profil gewaehlt, bestehendes unveraendert, Seitenwerte uebernommen, Export danach gleich)");
    else W282_FAIL("W419 CTRL-EXPORT gui", g_w419_why);
}
#undef W419_WHY

/* ---- W430 CTRL-IMPORT-FUZZ: deterministischer Mutations-Fuzzer fuer lcx_parse (.sbcontrols) ----------------
 * Korpus: Beispieldatei + die kaputten/boesartigen Dateien aus W419 (+ ein paar gueltige Minimalfaelle).
 * Mutationen (1..4 gestapelt): Bitflip, Byte setzen, Einfuegen, Loeschen, Abschneiden, Block verdoppeln, lange
 * Zeile, NUL/Steuerzeichen, Unicode (gueltig + kaputt), riesige Zahlen, Zeilen aus anderen Korpusdateien.
 * Fester Seed (xorshift32). Geprueft je Eingabe: kein Absturz, Rueckgabecode im Bereich, Fehlermeldung gesetzt
 * und terminiert, bei Fehler d vollstaendig leer, bei Erfolg Name ohne Pfadzeichen, Werte gueltig, nur erlaubte
 * Zeichen in Quellen, Rundreise lcx_build -> lcx_parse stabil, Laufzeit je Eingabe < 1 s. lcx_parse selbst
 * schreibt nichts auf die Platte (reiner Pufferweg); lcx_read_file nur lesend. */
static const char *const kW430Seeds[] = {
    "[steelbound_controls]\nformat = 1\nprofile = x\n[end]\n",
    "[steelbound_controls]\nformat = 1\nlauncher = 0.3.2\ngame = loc\nprofile = user_a\n[settings]\nmouse_sens = 3\n"
    "pad_dz_stick = 10\njoy_cal0 = 044F:B10A,X,-30000,200,31000,4,0\nsbc_cal_rot = -100,0,100\n"
    "[profile]\njoy0 = Pad One\nbi0 = KEY:A, MOUSE:LEFT, PAD:A\naim_x = JOY0:X:dz=0.05:curve=1.5:sens=2:inv=1\n"
    "pedal_r = JOY0:Z:zone=10-90\nchaff = KEY:C, PAD:LB+PAD:X\n[end]\n",
    "[steelbound_controls]\nformat = 1\nprofile = ..\\..\\evil\n[profile]\nbi0 = KEY:A\n[end]\n",
    "[steelbound_controls]\nformat = 1\nprofile = C:/Windows/x\n[end]\n",
    "[steelbound_controls]\nformat = 1\n[profile]\nbi0 = KEY:A\n[end]\n",
    "[steelbound_controls]\nformat = 1\nprofile = a\n[settings]\nmouse_sens = 99\n[end]\n",
    "[steelbound_controls]\nformat = 1\nprofile = a\n[settings]\nmouse_mode = fast\n[end]\n",
    "[steelbound_controls]\nformat = 1\nprofile = a\n[settings]\njoy_cal3 = 1:2,Q,9,9,1,0,0\n[end]\n",
    "[steelbound_controls]\nformat = 1\nprofile = a\n[settings]\nsbc_cal_rot = 5;del\n[end]\n",
    "[steelbound_controls]\nformat = 1\nprofile = a\n[profile]\naim_x = PAD:LX:dz=5\n[end]\n",
    "[steelbound_controls]\nformat = 1\nprofile = a\n[profile]\nbi0 = KEY:A;calc.exe\n[end]\n",
    "[steelbound_controls]\nformat = 1\nprofile = a\n[profile]\nbi0 = KEY:A, KEY:B, KEY:C, KEY:D, KEY:E\n[end]\n",
    "[steelbound_controls]\nformat = 1\nprofile = a\n[profile]\npedal_r = JOY0:Z:zone=40-200\n[end]\n",
    "[steelbound_controls]\nformat = 2\nprofile = a\n[end]\n",
    "[buttons]\nbi0 = KEY:SPACE\n",
    "[steelbound_controls]\nformat = 1\nprofile = user_x\nfuture = 1\n[settings]\nmouse_sens = 3\nwidth = 3840\n"
    "xbe_path = x.xbe\n[graphics]\nshading = 1\n[profile]\nbi0 = KEY:A, MIDI:C4, PAD:A\nlaser = KEY:L\n[end]\n",
    "\xEF\xBB\xBF[steelbound_controls]\r\nformat = 1\r\nprofile = bom\r\n[end]\r\n",
};
#define W430_NSEED ((int)(sizeof kW430Seeds / sizeof kW430Seeds[0]))
/* Einsatzstuecke fuer Einfuegen/Ersetzen (Schluessel, Zahlen, Unicode, Steuerzeichen) */
static const char *const kW430Bits[] = {
    "[end]", "[settings]", "[profile]", "[steelbound_controls]", "[x]", "\n", "\r\n", "=", ",", "#", ";", ":", "+",
    "99999999999999999999", "-2147483648", "2147483647", "4294967296", "-0", "1e999", "nan", "0x7fffffff", "-1",
    "000000000000000000001", "\xC3\xBC\xC3\xA4", "\xE2\x82\xAC", "\xF0\x9F\x8E\xAE", "\xC0\xAF", "\xFF\xFE", "\xED\xA0\x80",
    "\xEF\xBB\xBF", "format", "profile", "game", "launcher", "mouse_sens", "joy_cal0", "sbc_cal_rot", "joy0", "joy9",
    "KEY:", "MOUSE:", "PAD:", "SBC:", "JOY0:", "JOY9:", ":dz=", ":curve=", ":sens=", ":inv=", ":half=", ":zone=",
    "zone=-5--3", "zone=1-1-1", "dz=0.9", "dz=-0.1", "bi0", "aim_x", "pedal_r", "chaff", "\\", "/", "..", "%s%n",
    "\x01", "\x1F", "\x7F", "\t", "\xA0", "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
};
#define W430_NBITS ((int)(sizeof kW430Bits / sizeof kW430Bits[0]))
#define W430_MAXLEN (LCX_MAX_BYTES + 2048)
#define W430_SELFTEST_N 20000

static unsigned g_w430_rng;
static unsigned w430_rnd(void)
{
    unsigned x = g_w430_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_w430_rng = x;
    return x;
}
static size_t w430_below(size_t n) { return n ? (size_t)(w430_rnd() % (unsigned)n) : 0; }

/* Einfuegen von s (sl Bytes) an Stelle at; Rueckgabe neue Laenge (Grenze W430_MAXLEN). */
static size_t w430_ins(char *b, size_t len, size_t at, const char *s, size_t sl)
{
    if (at > len) { at = len; }
    if (len + sl > W430_MAXLEN) { return len; }
    memmove(b + at + sl, b + at, len - at);
    memcpy(b + at, s, sl);
    return len + sl;
}

static size_t w430_mutate(char *b, size_t len)
{
    int k, rounds = 1 + (int)w430_below(4);
    for (k = 0; k < rounds; k++) {
        size_t at = w430_below(len + 1), a, n;
        switch (w430_below(12)) {
        case 0: /* Bitflip(s) */
            for (n = 1 + w430_below(4); n && len; n--) { b[w430_below(len)] ^= (char)(1u << w430_below(8)); }
            break;
        case 1: /* Byte setzen */
            if (len) { b[w430_below(len)] = (char)w430_rnd(); }
            break;
        case 2: { /* Zufallsbytes einfuegen */
            char r[16];
            n = 1 + w430_below(16);
            for (a = 0; a < n; a++) { r[a] = (char)w430_rnd(); }
            len = w430_ins(b, len, at, r, n);
            break;
        }
        case 3: /* Bereich loeschen */
            if (len) {
                n = 1 + w430_below(len < 64 ? len : 64);
                if (at + n > len) { at = len - n; }
                memmove(b + at, b + at + n, len - at - n);
                len -= n;
            }
            break;
        case 4: /* abschneiden */
            len = w430_below(len + 1);
            break;
        case 5: /* Block verdoppeln (wiederholt: Groesse treiben) */
            if (len && len < W430_MAXLEN / 2) {
                n = 1 + w430_below(len < 200 ? len : 200);
                if (at + n > len) { at = len - n; }
                for (a = 1 + w430_below(8); a; a--) {
                    char tmp[200];
                    memcpy(tmp, b + at, n);
                    len = w430_ins(b, len, at, tmp, n);
                }
            }
            break;
        case 6: { /* lange Zeile */
            static char longl[70000];
            n = (size_t)(w430_below(4) == 0 ? 65000 : 480 + w430_below(120));
            memset(longl, w430_below(2) ? 'A' : '=', n);
            len = w430_ins(b, len, at, longl, n);
            break;
        }
        case 7: { /* NUL/Steuerzeichen */
            static const char cc[] = { 0, 1, 7, 8, 11, 12, 27, 31, 127 };
            char c = cc[w430_below(sizeof cc)];
            len = w430_ins(b, len, at, &c, 1);
            break;
        }
        case 8: case 9: { /* Einsatzstueck (Unicode, Zahlen, Schluessel ...) */
            const char *s = kW430Bits[w430_below((size_t)W430_NBITS)];
            len = w430_ins(b, len, at, s, strlen(s));
            break;
        }
        case 10: { /* Zeile aus anderer Korpusdatei */
            const char *o = kW430Seeds[w430_below((size_t)W430_NSEED)];
            size_t ol = strlen(o), s0 = w430_below(ol), s1 = s0;
            while (s0 > 0 && o[s0 - 1] != '\n') { s0--; }
            while (s1 < ol && o[s1] != '\n') { s1++; }
            len = w430_ins(b, len, at, o + s0, s1 - s0 + (s1 < ol ? 1 : 0));
            break;
        }
        default: { /* Zeile durch Einsatzstueck ersetzen (Schluessel/Wert-Teil) */
            const char *s = kW430Bits[w430_below((size_t)W430_NBITS)];
            size_t e = at, st = at;
            while (st > 0 && b[st - 1] != '\n') { st--; }
            while (e < len && b[e] != '\n') { e++; }
            if (e > st) { memmove(b + st, b + e, len - e); len -= e - st; }
            len = w430_ins(b, len, st, s, strlen(s));
            break;
        }
        }
    }
    return len;
}

/* Pruefung eines Ergebnisses; 0 = ok, sonst Text in why. */
static int w430_check(const char *in, size_t len, const lcx_data_t *d, int rc, const char *err, char *why, size_t wc)
{
    static lcx_data_t z;
    static lcx_data_t d2;
    static char out[LCX_MAX_BYTES * 2];
    char err2[300];
    size_t i;
    int a, r, rc2;
    (void)in;
    if (rc < LCX_OK || rc > LCX_E_LINE) { _snprintf_s(why, wc, _TRUNCATE, "rc %d ausserhalb", rc); return 1; }
    if (memchr(err, 0, 300) == NULL) { _snprintf_s(why, wc, _TRUNCATE, "Meldung nicht terminiert"); return 1; }
    if (rc != LCX_OK) {
        if (!err[0]) { _snprintf_s(why, wc, _TRUNCATE, "Fehler %d ohne Meldung", rc); return 1; }
        if (memcmp(d, &z, sizeof z) != 0) { _snprintf_s(why, wc, _TRUNCATE, "Fehler %d: Daten nicht leer", rc); return 1; }
        return 0;
    }
    if (d->format < 1 || d->format > LCX_FORMAT || !d->profile[0] || strlen(d->profile) > 63 ||
        d->n_set < 0 || d->n_set > LCX_SET_MAX) {
        _snprintf_s(why, wc, _TRUNCATE, "OK mit ungueltigem Kopf (format %d, n_set %d)", d->format, d->n_set);
        return 1;
    }
    for (i = 0; d->profile[i]; i++) {
        unsigned char c = (unsigned char)d->profile[i];
        if (c < 0x20 || c == 0x7F || strchr("/\\:*?\"<>|", c)) { _snprintf_s(why, wc, _TRUNCATE, "Name mit Zeichen %02X", c); return 1; }
    }
    if (strstr(d->profile, "..")) { _snprintf_s(why, wc, _TRUNCATE, "Name mit .."); return 1; }
    for (a = 0; a < d->n_set; a++) {
        char k[96];
        const char *eq = strchr(d->set[a], '=');
        if (!eq || (size_t)(eq - d->set[a]) >= 48) { _snprintf_s(why, wc, _TRUNCATE, "Wert ohne Schluessel"); return 1; }
        memcpy(k, d->set[a], (size_t)(eq - d->set[a]));
        k[eq - d->set[a]] = '\0';
        if (lcx_set_check(k, eq + 1) != 1) { _snprintf_s(why, wc, _TRUNCATE, "OK mit ungueltigem Wert %.40s", d->set[a]); return 1; }
    }
    for (a = 0; a < LCTRL_ELEM_N; a++) {
        if (d->bind.row[a].n_alt < 0 || d->bind.row[a].n_alt > LCTRL_ALT_N) { _snprintf_s(why, wc, _TRUNCATE, "n_alt %d", d->bind.row[a].n_alt); return 1; }
        for (r = 0; r < d->bind.row[a].n_alt; r++) {
            const unsigned char *c = (const unsigned char *)d->bind.row[a].alt[r];
            if (memchr(c, 0, 96) == NULL || !*c) { _snprintf_s(why, wc, _TRUNCATE, "Quelle leer/unterminiert"); return 1; }
            for (; *c; c++) {
                if (!(isalnum(*c) || strchr(":+=._!-", *c))) { _snprintf_s(why, wc, _TRUNCATE, "Quelle mit Zeichen %02X", *c); return 1; }
            }
        }
    }
    /* Rundreise: bauen und erneut lesen -> gleiche Daten */
    if (lcx_build(d, out, sizeof out) != 0) { _snprintf_s(why, wc, _TRUNCATE, "Rundreise: lcx_build voll"); return 1; }
    rc2 = lcx_parse(out, strlen(out), &d2, err2, sizeof err2);
    if (rc2 != LCX_OK || d2.n_set != d->n_set || strcmp(d2.profile, d->profile) != 0 || !w419_bind_eq(&d2.bind, &d->bind)) {
        _snprintf_s(why, wc, _TRUNCATE, "Rundreise: rc %d, Werte %d/%d", rc2, d2.n_set, d->n_set);
        return 1;
    }
    (void)len;
    return 0;
}

/* n Mutationen mit Seed; Rueckgabe 0 = ok. stat[0] = OK-Importe, stat[1] = abgelehnt. */
int w430_ctrl_fuzz(long n, unsigned seed, char *why, size_t wc, long *stat)
{
    static lcx_data_t d;
    static char buf[W430_MAXLEN + 16];
    char err[300];
    long it;
    why[0] = '\0';
    stat[0] = stat[1] = 0;
    g_w430_rng = seed ? seed : 0x430u;
    for (it = 0; it < n; it++) {
        const char *s = it % 7 == 0 ? kCxFixture : kW430Seeds[w430_below((size_t)W430_NSEED)];
        size_t len = strlen(s), outlen;
        ULONGLONG t0;
        int rc;
        memcpy(buf, s, len);
        outlen = it < W430_NSEED ? len : w430_mutate(buf, len); /* erste Runde: Korpus unveraendert */
        buf[outlen] = '\0';
        memset(&d, 0xA5, sizeof d); /* Vorbelegung: lcx_parse muss d vollstaendig selbst setzen */
        memset(err, 0xA5, sizeof err);
        t0 = GetTickCount64();
        rc = lcx_parse(buf, outlen, &d, err, sizeof err);
        if (GetTickCount64() - t0 > 1000) { _snprintf_s(why, wc, _TRUNCATE, "Eingabe %ld: %llu ms (Laufzeit)", it, (unsigned long long)(GetTickCount64() - t0)); return 1; }
        if (w430_check(buf, outlen, &d, rc, err, why, wc)) {
            char tmp[160], p[1200];
            FILE *f = NULL;
            u8_GetTempPath(tmp, sizeof tmp);
            _snprintf_s(p, sizeof p, _TRUNCATE, "%sw430_fund_%lu.sbcontrols", tmp, GetCurrentProcessId());
            if (u8_fopen_s(&f, p, "wb") == 0 && f) { fwrite(buf, 1, outlen, f); fclose(f); }
            { char w2[300]; _snprintf_s(w2, sizeof w2, _TRUNCATE, "Eingabe %ld (Seed %u, %u Bytes): %s", it, seed, (unsigned)outlen, why); strcpy_s(why, wc, w2); }
            return 1;
        }
        stat[rc == LCX_OK ? 0 : 1]++;
    }
    return 0;
}

/* Selbsttest: kurz (feste Anzahl, deutlich unter 5 s). Lang: launcher --controls-fuzz N [--seed S]. */
void w430_selftest(int *pass, int *fail)
{
    char why[400];
    long stat[2];
    ULONGLONG t0 = GetTickCount64();
    int rc = w430_ctrl_fuzz(W430_SELFTEST_N, 0x430u, why, sizeof why, stat);
    ULONGLONG ms = GetTickCount64() - t0;
    if (!rc && stat[0] > 100 && stat[1] > 100 && ms < 5000) {
        printf("PASS W430 CTRL-IMPORT-FUZZ (%ld Mutationen fester Seed, %ld Importe, %ld abgelehnt, %llu ms; Invarianten: Rueckgabe, Meldung, leere Daten bei Fehler, Name, Werte, Quellen, Rundreise)\n",
               (long)W430_SELFTEST_N, stat[0], stat[1], (unsigned long long)ms);
        (*pass)++;
    } else {
        printf("FAIL W430 CTRL-IMPORT-FUZZ: %s (%ld/%ld, %llu ms)\n", why[0] ? why : "zu wenig Vielfalt oder zu langsam", stat[0], stat[1], (unsigned long long)ms);
        (*fail)++;
    }
}

/* W438 CTRL-RESET-040: Startpfad des Einmal-Resets (ctrl040_reset wie in gui_fill) mit Temp-Client-Ordner und
 * Temp-user.cfg: Hinweis mit altem Namen genau einmal, ohne default_full.ini kein Reset, Belegungsdialog zeigt
 * menu_back (default_full: Insert) und meldet START + Replay beenden auf Backspace nicht als Konflikt. */
void w438_gui_selftest(const char *client_override, int *pass, int *fail)
{
    char tmp[1024], dir[1100], pd[1200], pf[1300], uc[1300], keep[1024], why[200] = { 0 };
    static lctrl_bind_t b;
    lctrl_conf1_t cf[LCTRL_ALT_N];
    FILE *f = NULL;
    (void)client_override;
    u8_GetTempPath(tmp, sizeof tmp);
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%sw438_gui_%lu", tmp, GetCurrentProcessId());
    _snprintf_s(pd, sizeof pd, _TRUNCATE, "%s/profiles", dir);
    _snprintf_s(pf, sizeof pf, _TRUNCATE, "%s/default_full.ini", pd);
    _snprintf_s(uc, sizeof uc, _TRUNCATE, "%s/user.cfg", dir);
    lcfg_mkdir_p(pd);
    strcpy_s(keep, sizeof keep, g_client_dir);
    strcpy_s(g_client_dir, sizeof g_client_dir, dir);
    strcpy_s(g_ucfg_test, sizeof g_ucfg_test, uc);
    if (u8_fopen_s(&f, uc, "wb") == 0 && f) {
        fputs("[input]\ninput_profile=user_mein\n", f);
        fclose(f);
    }
    ctrl040_reset(); /* a: default_full.ini fehlt -> nichts */
    if (g_ctrl040_rc != 0 || g_ctrl040_msg[0] || lgame_ctrl040_marked(uc)) {
        strcpy_s(why, sizeof why, "a: Reset ohne default_full.ini");
    }
    if (!why[0]) {
        if (u8_fopen_s(&f, pf, "wb") == 0 && f) {
            fputs("[buttons]\nbi6 = KEY:BACKSPACE\n", f);
            fclose(f);
        }
        ctrl040_reset(); /* b: Reset + Hinweis mit altem Namen */
        if (g_ctrl040_rc != 1 || !strstr(g_ctrl040_msg, "user_mein") || !lgame_ctrl040_marked(uc))
            _snprintf_s(why, sizeof why, _TRUNCATE, "b: rc=%d Hinweis '%s'", g_ctrl040_rc, g_ctrl040_msg);
    }
    if (!why[0]) {
        ctrl040_reset(); /* c: zweiter Start ohne Hinweis */
        if (g_ctrl040_rc != 0 || g_ctrl040_msg[0])
            _snprintf_s(why, sizeof why, _TRUNCATE, "c: zweiter Start rc=%d", g_ctrl040_rc);
    }
    if (!why[0]) { /* d: default_full im Dialog: menu_back = Insert, Backspace bei START ohne Konfliktmeldung */
        int st = lctrl_bind_elem_index("bi6"), mb = lctrl_bind_elem_index("menu_back");
        char conf[8][160];
        lctrl_bind_init(&b);
        lctrl_bind_load_text("[buttons]\nbi6 = KEY:BACKSPACE\n[client]\nreplay_exit = KEY:BACKSPACE\nmenu_back = KEY:INSERT\n", &b);
        if (mb < 0 || b.row[mb].n_alt != 1 || strcmp(b.row[mb].alt[0], "KEY:INSERT") != 0 ||
            lctrl_bind_conflicts(&b, conf, 8) != 0 || lctrl_bind_conflicts_for(&b, st, cf, LCTRL_ALT_N) != 0 ||
            strcmp(LT("bind.menu_back"), "bind.menu_back") == 0)
            strcpy_s(why, sizeof why, "d: default_full im Dialog");
        lctrl_bind_init(&b); /* ohne Zeile: Standard Backspace (START auf Enter) bzw. Insert (START auf Backspace) */
        lctrl_bind_load_text("[buttons]\nbi6 = KEY:ENTER\n", &b);
        if (!why[0] && (b.row[mb].n_alt != 1 || strcmp(b.row[mb].alt[0], "KEY:BACKSPACE") != 0))
            strcpy_s(why, sizeof why, "d: Standard ohne Zeile (Enter-START)");
        lctrl_bind_init(&b);
        lctrl_bind_load_text("[buttons]\nbi6 = KEY:BACKSPACE\nbi0 = KEY:INSERT\n", &b);
        if (!why[0] && (b.row[mb].n_alt != 1 || strcmp(b.row[mb].alt[0], "KEY:INSERT") != 0 ||
                        lctrl_bind_conflicts(&b, conf, 8) != 1))
            strcpy_s(why, sizeof why, "d: Ausweichtaste/echter Konflikt");
    }
    strcpy_s(g_client_dir, sizeof g_client_dir, keep);
    g_ucfg_test[0] = '\0';
    g_ctrl040_msg[0] = '\0';
    u8_DeleteFile(pf);
    u8_DeleteFile(uc);
    {
        wchar_t w[1300];
        u8_to_w(pd, w, 1300);
        RemoveDirectoryW(w);
        u8_to_w(dir, w, 1300);
        RemoveDirectoryW(w);
    }
    if (!why[0]) {
        printf("PASS W438 CTRL-RESET-040 gui (Startpfad: ohne default_full.ini nichts, Reset mit Hinweis + altem Namen genau einmal; Belegung menu_back, START + Replay beenden ohne Konflikt)\n");
        (*pass)++;
    } else {
        printf("FAIL W438 CTRL-RESET-040 gui: %s\n", why);
        (*fail)++;
    }
}
