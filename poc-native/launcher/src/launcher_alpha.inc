/* launcher_alpha.inc - W272 ALPHA-TEST: Seite "Test tools" (W301 BETA-VERSION,
 * vorher "Alpha Test"; Testwerkzeuge, nicht Teil des Originalspiels). Eingebunden in launcher_gui.c
 * an einer Stelle vor den Selbsttests (nutzt die statischen Helfer: mk,
 * card, label, do_start_ex, client_running, user_cfg_path, nav_next ...;
 * Vorwaertsdeklarationen oben in launcher_gui.c). Logik ohne Fenster:
 * launcher_alpha.c (VT-Liste, Pruefung, Testordner, Start-Kette).
 *  - Teststart: Mission + VT + Schwierigkeit direkt, frischer Testordner
 *    <Datenordner>\alpha_test\HDD(+_US), Start ueber do_start_ex mit
 *    Zusatz-Kette (nur dieser Start, user.cfg bleibt).
 *  - A/B-Schalter der Zwischenbild-Fixes (interp_zcons, interp_hlerp,
 *    interp_guard nur wenn der Client ihn kennt) in user.cfg; das ESC-Menue
 *    bietet sie nicht live an, sie gelten beim naechsten Start.
 *  - launcher_ui.cfg test_tools=0 blendet die Seite aus (W301; alte cfg mit
 *    alpha_tools=0 gelten weiter, ui_cfg_load_tools). Seitenname "tools",
 *    Alias "alpha" (--ui-shot/--page). */

#define ID_AL_MIS 1300
#define ID_AL_VT 1301
#define ID_AL_DIFF 1302
#define ID_AL_SKIP 1303
#define ID_AL_START 1304
#define ID_AL_ZCONS 1305
#define ID_AL_HLERP 1306
#define ID_AL_GUARD 1307

#define AL_Y1 (CY0 + 78)           /* Karte Teststart (W279: Kopf 12 px flacher) */
#define AL_H1 226
#define AL_Y2 (AL_Y1 + AL_H1 + 12) /* Karte A/B */
#define AL_LW 96                   /* Beschriftung links */
#define AL_CW 230                  /* Auswahl */
#define AL_C2 (CX0 + 364)          /* rechte Spalte */
#define AL_W2 (CX0 + CW - 18 - AL_C2)
#define AL_W1 (AL_C2 - 12 - CX0)   /* W279: A/B-Karte links, rechts "Problem reports" */
#define AL_SHOT_ROOT "D:\\Steelbound\\alpha_test" /* Bild: Beispielordner (kein Nutzerpfad) */

static la_vtlist_t g_al_vt;
static char g_al_vt_key[2200];
static int g_al_vt_valid;
static int g_al_model[LA_VT_MAX + 2]; /* Combo-Index -> Modell (-1 = Vorgabe) */
static int g_al_nmodel;
static int g_al_guard = -1;  /* Client kennt interp_guard: -1 ungeprueft */


/* Einstellungen der Oberflaeche (auch ungespeicherte Spielwahl). */
static void alpha_ui_settings(launcher_settings_t *s)
{
    *s = g_s;
    if (g_hwnd && ui_find(ID_IMAGE)) {
        ui_to_settings(s);
    }
}

static void alpha_vt_fill(void)
{
    HWND c = ctl(ID_AL_VT);
    int keep = -1, i, m, sel = 0;
    if (!c) {
        return;
    }
    i = ComboBox_GetCurSel(c);
    if (i >= 0 && i < g_al_nmodel) {
        keep = g_al_model[i];
    }
    ComboBox_ResetContent(c);
    g_al_nmodel = 0;
    g_al_model[g_al_nmodel++] = -1;
    combo_add(c, LT("gui.alpha_vt_default"));
    for (m = 1; m <= LA_VT_MAX; m++) {
        char t[120];
        if (!g_al_vt.m[m].present) {
            continue;
        }
        if (g_al_vt.m[m].gen >= 0 && g_al_vt.m[m].gen <= 2) {
            _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.alpha_vt_entry"), m, la_vt_name(m),
                        g_al_vt.m[m].gen + 1);
        } else {
            _snprintf_s(t, sizeof t, _TRUNCATE, "%02d %s", m, la_vt_name(m));
        }
        if (m == keep) {
            sel = g_al_nmodel;
        }
        g_al_model[g_al_nmodel++] = m;
        combo_add(c, t);
    }
    ComboBox_SetCurSel(c, sel);
}

/* VT-Liste aus dem eingestellten Spiel (Cache je Abbild/Ordner). Bilder
 * (--ui-shot): feste Tabelle als Beispiel der Spieldaten (kein Nutzerpfad). */
static void alpha_vt_refresh(int force)
{
    launcher_settings_t s;
    char key[2200];
    alpha_ui_settings(&s);
    _snprintf_s(key, sizeof key, _TRUNCATE, "%d|%s|%s", g_o.shot == 2, s.game_image, s.media_root);
    if (!force && g_al_vt_valid && strcmp(key, g_al_vt_key) == 0) {
        return;
    }
    if (g_o.shot == 2) {
        la_vt_fallback(&g_al_vt);
        g_al_vt.src = LA_SRC_IMAGE;
    } else {
        la_vt_scan(&s, &g_al_vt);
    }
    strcpy_s(g_al_vt_key, sizeof g_al_vt_key, key);
    g_al_vt_valid = 1;
    alpha_vt_fill();
}

static void alpha_sel_from_ui(la_sel_t *sel)
{
    int i = ComboBox_GetCurSel(ctl(ID_AL_VT));
    sel->mission = ComboBox_GetCurSel(ctl(ID_AL_MIS));
    sel->vt = (i >= 0 && i < g_al_nmodel) ? g_al_model[i] : -1;
    sel->diff = ComboBox_GetCurSel(ctl(ID_AL_DIFF)) - 1;
    sel->skip_intro = ui_checked(ID_AL_SKIP);
    if (sel->mission < 0) {
        sel->mission = 0;
    }
    if (sel->diff < -1) {
        sel->diff = -1;
    }
}

/* Testordner + echte Spielstand-Ordner (EU und US) wie der Startweg. */
static void alpha_paths(char *root, char *eu, char *us, size_t cap, char *real_eu, char *real_us,
                        size_t rcap)
{
    launcher_settings_t s;
    char data[1100];
    alpha_ui_settings(&s);
    la_data_root(g_client_disp, data, sizeof data);
    la_test_dirs(data, root, eu, us, cap);
    app_hdd_base(g_client_disp, s.hdd_root, real_eu, rcap);
    lcfg_hdd_for_edition(real_eu, LCFG_ED_US, real_us, rcap);
}

/* Teststart (Knopf, --gui-start-dry --alpha-mission). sel = NULL: Wahl der
 * Seite. 0 = ok, 1 = Fehler, 3 = ungueltige Kombination (kein Start). */
static int alpha_start(int dry, const la_sel_t *sel_in)
{
    la_sel_t sel;
    la_check_t r;
    char msg[2000], root[1100], eu[1100], us[1100], reu[1100], rus[1100], extra[1600];
    const char *reals[2];
    const char *ek = NULL;
    int rc;
    if (sel_in) {
        sel = *sel_in;
    } else {
        alpha_sel_from_ui(&sel);
    }
    alpha_vt_refresh(sel_in != NULL);
    if (la_check(&sel, &g_al_vt, &r)) {
        la_check_text(&sel, &r, msg, sizeof msg);
        set_status_k(msg, 2);
        ui_msgbox(g_hwnd, msg, LT("gui.alpha_page"), MB_ICONWARNING | MB_OK);
        return 3;
    }
    if (!dry && client_running()) { /* wie beim Replay-Start */
        set_status_k(LT("gui.alpha_running"), 2);
        ui_msgbox(g_hwnd, LT("gui.alpha_running"), LT("gui.alpha_page"), MB_ICONWARNING | MB_OK);
        return 1;
    }
    alpha_paths(root, eu, us, sizeof root, reu, rus, sizeof reu);
    reals[0] = reu;
    reals[1] = rus;
    if (la_prepare(root, reals, 2, dry, &ek) != 0) {
        set_status_k(LT(ek), 2);
        ui_msgbox(g_hwnd, LT(ek), LT("gui.alpha_page"), MB_ICONERROR | MB_OK);
        return 1;
    }
    if (la_extra_args(&sel, eu, us, extra, sizeof extra) != 0) {
        set_status_k(LT("gui.alpha_err_path"), 2);
        ui_msgbox(g_hwnd, LT("gui.alpha_err_path"), LT("gui.alpha_page"), MB_ICONERROR | MB_OK);
        return 1;
    }
    rc = do_start_ex(dry, 0, extra); /* Bildschirmweg, Zusatz nur fuer diesen Start */
    if (rc == 0 && !dry) {
        char m[200];
        if (sel.mission == 0) {
            strcpy_s(m, sizeof m, LT("gui.alpha_mis_training"));
        } else {
            _snprintf_s(m, sizeof m, _TRUNCATE, LT("gui.alpha_mis_n"), sel.mission);
        }
        _snprintf_s(msg, sizeof msg, _TRUNCATE, LT("gui.alpha_started"), m);
        set_status_k(msg, 1);
    }
    return rc;
}

/* A/B-Schalter: Werte aus user.cfg (sonst sb_native.cfg, sonst Client-Standard). */
static void alpha_ab_sync(void)
{
    char uc[1200], nc[1200];
    uictl_t *g = ui_find(ID_AL_GUARD);
    if (!ui_find(ID_AL_ZCONS)) {
        return;
    }
    user_cfg_path(uc, sizeof uc);
    _snprintf_s(nc, sizeof nc, _TRUNCATE, "%s\\sb_native.cfg", g_client_dir);
    ui_set_checked(ID_AL_ZCONS, la_cfg_get_int(uc, nc, "interp_zcons", 1) != 0);
    ui_set_checked(ID_AL_HLERP, la_cfg_get_int(uc, nc, "interp_hlerp", 0) != 0);
    ui_set_checked(ID_AL_GUARD, la_cfg_get_int(uc, nc, "interp_guard", 0) != 0);
    if (g_al_guard < 0) {
        g_al_guard = la_client_knows_key(g_client_disp, "interp_guard");
    }
    if (g) { /* unbekannter Schluessel: Schalter nie zeigen (Seite ohne Platzhalter) */
        g->page = (g_al_guard || strcmp(g_o.state, "alpha_demo") == 0) ? P_ALPHA : PG_DIALOG; /* Bild: --state alpha_demo */
        ShowWindow(g->h, ui_visible_on(g, g_page) ? SW_SHOWNA : SW_HIDE);
    }
}

static void alpha_ab_toggle(int id, const char *key)
{
    char uc[1200];
    int on = !ui_checked(id);
    user_cfg_path(uc, sizeof uc);
    if (la_cfg_set(uc, key, on ? "1" : "0") != 0) {
        set_status_k(LT("gui.alpha_ab_nowrite"), 2);
        return;
    }
    game_cfg_path(LGAME_LOC, uc, sizeof uc); /* W402: Test-Tools sind global -> auch Line of Contact (falls Datei da) */
    if (u8_GetFileAttributes(uc) != INVALID_FILE_ATTRIBUTES) {
        (void)la_cfg_set(uc, key, on ? "1" : "0");
    }
    ui_set_checked(id, on);
    set_status_k(LT("gui.alpha_ab_saved"), 1);
}

/* launcher_ui.cfg test_tools (Rueckfall alpha_tools): 0 = Seite + Navigation aus. */
static void alpha_tools_apply(void)
{
    uictl_t *n = ui_find(ID_NAV0 + P_ALPHA);
    if (n) {
        n->page = g_alpha_tools ? PG_ALL : PG_DIALOG;
        ShowWindow(n->h, ui_visible_on(n, g_page) ? SW_SHOWNA : SW_HIDE);
    }
    if (!g_alpha_tools && g_page == P_ALPHA) {
        show_page(P_ADV);
    }
}

void alpha_page_shown(void)
{
    alpha_vt_refresh(0);
    alpha_ab_sync();
    if (g_o.shot == 2 && strcmp(g_o.state, "alpha_demo") == 0) { /* Bild: Mission 05, VT 13 */
        int i;
        ComboBox_SetCurSel(ctl(ID_AL_MIS), 5);
        for (i = 0; i < g_al_nmodel; i++) {
            if (g_al_model[i] == 13) {
                ComboBox_SetCurSel(ctl(ID_AL_VT), i);
            }
        }
    }
}

void paint_alpha(void)
{
    char t[1400];
    int vt = -1, i = ComboBox_GetCurSel(ctl(ID_AL_VT));
    if (i >= 0 && i < g_al_nmodel) {
        vt = g_al_model[i];
    }
    /* Kopf: klar als Testwerkzeug gekennzeichnet */
    card(CX0, CY0, CW, 66, LT("gui.alpha_head"));
    L_fill(CX0 + CW - 18 - 64, CY0 + 14, 64, 20, C_ACCENT, 34, 3);
    L_text(F_TINY, "BETA", CX0 + CW - 18 - 64, /* W301: Beta statt Alpha */ CY0 + 14, 64, 20, C_ACCENT,
           DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    label1(F_SMALL, LT("gui.alpha_notice"), CX0 + 18, CY0 + 36, CW - 36, 20, C_WARN);

    /* Teststart */
    card(CX0, AL_Y1, CW, AL_H1, LT("gui.alpha_start_card"));
    label1(F_TINY, LT("gui.alpha_start_txt"), CX0 + 18, AL_Y1 + 38, CW - 36, 16, C_DIM);
    label1(F_SMALL, LT("gui.alpha_mission"), CX0 + 18, AL_Y1 + 62, AL_LW - 6, 30, C_DIM);
    label1(F_SMALL, LT("gui.alpha_vt"), CX0 + 18, AL_Y1 + 100, AL_LW - 6, 30, C_DIM);
    label1(F_SMALL, LT("gui.alpha_diff"), CX0 + 18, AL_Y1 + 138, AL_LW - 6, 30, C_DIM);
    if (vt < 1) {
        label1(F_TINY, LT("gui.alpha_vt_def_info"), CX0 + 18, AL_Y1 + 172, AL_LW + AL_CW, 16, C_DIM);
    } else {
        if (g_al_vt.m[vt].gen >= 0 && g_al_vt.m[vt].gen <= 2) {
            _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.alpha_vt_gen"), g_al_vt.m[vt].gen + 1);
            label1(F_TINY, t, CX0 + 18, AL_Y1 + 172, AL_LW + AL_CW, 16, C_TEXT);
        }
        if (g_al_vt.m[vt].size && g_al_vt.m[vt].size < LA_VT_FULL_SIZE) {
            _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.alpha_vt_reduced"), g_al_vt.m[vt].size);
            label1(F_TINY, t, CX0 + 18, AL_Y1 + 188, AL_LW + AL_CW, 16, C_WARN);
        }
    }
    if (g_al_vt.src == LA_SRC_FALLBACK) {
        label1(F_TINY, LT("gui.alpha_vt_src_fallback"), CX0 + 18, AL_Y1 + 204, AL_LW + AL_CW, 16, C_WARN);
    } else {
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.alpha_vt_src_game"), g_al_vt.n);
        label1(F_TINY, t, CX0 + 18, AL_Y1 + 204, AL_LW + AL_CW, 16, C_FAINT);
    }
    label1(F_TINY, LT("gui.alpha_skip_txt"), AL_C2, AL_Y1 + 90, AL_W2, 16, C_DIM);
    _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.alpha_edition"),
                LT(g_edition == LCFG_ED_US ? "gui.ed_us" : "gui.ed_eu"));
    label1(F_TINY, t, AL_C2, AL_Y1 + 116, AL_W2, 16, C_DIM);
    label1(F_TINY, LT("gui.alpha_folder"), AL_C2, AL_Y1 + 136, AL_W2, 16, C_DIM);
    if (g_o.shot == 2) {
        strcpy_s(t, sizeof t, AL_SHOT_ROOT);
    } else {
        char data[1100];
        la_data_root(g_client_disp, data, sizeof data);
        la_test_dirs(data, t, NULL, NULL, sizeof t);
    }
    L_text(F_TINY, t, AL_C2, AL_Y1 + 152, AL_W2, 16, C_TEXT,
           DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_PATH_ELLIPSIS);

    /* A/B-Schalter */
    card(CX0, AL_Y2, AL_W1, CY0 + 496 - AL_Y2, LT("gui.alpha_ab_card"));
    label(F_TINY, LT("gui.alpha_ab_txt"), CX0 + 18, AL_Y2 + 38, AL_W1 - 36, 32, C_DIM);
    if (!ui_visible_on(ui_find(ID_AL_GUARD), P_ALPHA)) { /* W279: Platz des Schalters interp_guard */
        label(F_TINY, LT("gui.alpha_ab_std"), CX0 + 18, AL_Y2 + 138, AL_W1 - 36, 40, C_FAINT);
    }
    dg_paint_alpha(AL_Y2, CY0 + 496 - AL_Y2); /* W279 DIAG-CRASH: Karte "Problem reports" */
}

static void alpha_fill_static(void)
{
    HWND c = ctl(ID_AL_MIS);
    int i, sel;
    if (c) {
        sel = ComboBox_GetCurSel(c);
        ComboBox_ResetContent(c);
        combo_add(c, LT("gui.alpha_mis_training"));
        for (i = 1; i <= LA_MISSION_MAX; i++) {
            char t[64];
            _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.alpha_mis_n"), i);
            combo_add(c, t);
        }
        ComboBox_SetCurSel(c, sel >= 0 ? sel : 1);
    }
    c = ctl(ID_AL_DIFF);
    if (c) {
        sel = ComboBox_GetCurSel(c);
        ComboBox_ResetContent(c);
        combo_add(c, LT("gui.alpha_diff_default"));
        for (i = 0; i <= LA_DIFF_MAX; i++) {
            combo_add(c, la_diff_name(i));
        }
        ComboBox_SetCurSel(c, sel >= 0 ? sel : 0);
    }
}

void alpha_build_controls(HWND w)
{
    /* Reihenfolge = Tab-Reihenfolge: links Mission/VT/Schwierigkeit, rechts
     * Schalter + Start, darunter A/B */
    mk(w, K_COMBO, ID_AL_MIS, P_ALPHA, CX0 + 18 + AL_LW, AL_Y1 + 62, AL_CW, 30, "", 0);
    mk(w, K_COMBO, ID_AL_VT, P_ALPHA, CX0 + 18 + AL_LW, AL_Y1 + 100, AL_CW, 30, "", 0);
    mk(w, K_COMBO, ID_AL_DIFF, P_ALPHA, CX0 + 18 + AL_LW, AL_Y1 + 138, AL_CW, 30, "", 0);
    mk(w, K_TOGGLE, ID_AL_SKIP, P_ALPHA, AL_C2, AL_Y1 + 62, AL_W2, 28, LT("gui.alpha_skip"), 0);
    mk(w, K_ACCENT, ID_AL_START, P_ALPHA, AL_C2, AL_Y1 + 178, 170, 34, LT("gui.alpha_btn_start"), 0);
    mk(w, K_TOGGLE, ID_AL_ZCONS, P_ALPHA, CX0 + 18, AL_Y2 + 74, AL_W1 - 36, 28,
       LT("gui.alpha_ab_zcons"), 0);
    mk(w, K_TOGGLE, ID_AL_HLERP, P_ALPHA, CX0 + 18, AL_Y2 + 106, AL_W1 - 36, 28,
       LT("gui.alpha_ab_hlerp"), 0);
    mk(w, K_TOGGLE, ID_AL_GUARD, P_ALPHA, CX0 + 18, AL_Y2 + 138, AL_W1 - 36, 28, LT("gui.alpha_ab_guard"), 0);
    alpha_fill_static();
    ui_set_checked(ID_AL_SKIP, 1);
    g_al_vt_valid = 0; /* VT-Liste beim Zeigen der Seite (Spiel steht dann fest) */
    g_al_nmodel = 0;
    alpha_vt_fill();
    alpha_ab_sync();
    alpha_tools_apply();
}

void alpha_relabel(void)
{
    ui_settext(ctl(ID_AL_SKIP), LT("gui.alpha_skip"));
    ui_settext(ctl(ID_AL_START), LT("gui.alpha_btn_start"));
    ui_settext(ctl(ID_AL_ZCONS), LT("gui.alpha_ab_zcons"));
    ui_settext(ctl(ID_AL_HLERP), LT("gui.alpha_ab_hlerp"));
    ui_settext(ctl(ID_AL_GUARD), LT("gui.alpha_ab_guard"));
    alpha_fill_static();
    alpha_vt_fill();
}

/* 1 = Element der Seite (behandelt). */
static int alpha_on_command(int id, int code)
{
    switch (id) {
    case ID_AL_VT:
        if (code == CBN_SELCHANGE) {
            redraw_all(); /* Cockpit-Generation / Hinweis */
        }
        return 1;
    case ID_AL_MIS:
    case ID_AL_DIFF:
        return 1;
    case ID_AL_SKIP:
        ui_set_checked(ID_AL_SKIP, !ui_checked(ID_AL_SKIP));
        return 1;
    case ID_AL_START:
        alpha_start(0, NULL);
        return 1;
    case ID_AL_ZCONS:
        alpha_ab_toggle(ID_AL_ZCONS, "interp_zcons");
        return 1;
    case ID_AL_HLERP:
        alpha_ab_toggle(ID_AL_HLERP, "interp_hlerp");
        return 1;
    case ID_AL_GUARD:
        alpha_ab_toggle(ID_AL_GUARD, "interp_guard");
        return 1;
    }
    return 0;
}

/* Selbsttest (a): Seite vorhanden + in der Navigation (bei alpha_tools=0
 * weg), Auswahllisten, Start-Kette der Seite, ungueltig/laufender Client
 * ohne Start, A/B-Schalter schreiben user.cfg, launcher_ui.cfg alpha_tools. */
void alpha_gui_selftest(const char *client_override, int *pass, int *fail)
{
    gui_opts_t o;
    char why[300] = "";
    int ok = 0;
    memset(&o, 0, sizeof o);
    o.shot = 5;
    g_o = o;
    if (gui_init(client_override) && gui_create(1)) {
        uictl_t *nav;
        char t[200] = "", tmp[1024] = "", uc[1200], buf[2000];
        la_sel_t s = { 5, 16, -1, 1 };
        int z0, keep_busy = g_save_test_busy;
        build_controls();
        ok = 1;
        nav = ui_find(ID_NAV0 + P_ALPHA);
        if (nav) {
            ui_gettext(nav->h, t, sizeof t);
        }
        /* W397 LAUNCHER-STRUCT: kein Navigationseintrag mehr; Zugang ueber Zahnrad > Test tools */
        if (!nav || ui_visible_on(nav, P_PLAY) || strcmp(t, LT("gui.alpha_page")) != 0 ||
            !ui_visible_on(ui_find(ID_LSET_TOOLS), P_LSET)) {
            ok = 0;
            strcpy_s(why, sizeof why, "Zugang (Zahnrad > Test tools)");
        }
        on_command(ID_LSET_TOOLS, BN_CLICKED);
        if (ok && (g_page != P_ALPHA || !ui_visible_on(ui_find(ID_AL_START), P_ALPHA) ||
                   ui_visible_on(ui_find(ID_AL_START), P_ADV) || !ui_visible_on(ui_find(ID_AL_MIS), P_ALPHA))) {
            ok = 0;
            strcpy_s(why, sizeof why, "Seite/Elemente");
        }
        if (ok && (nav_next(P_ADV, 1) != P_PLAY || nav_next(P_ALPHA, 1) != P_PLAY || /* W397: nicht im Zyklus */
                   nav_next(P_PLAY, -1) != P_ADV || gui_page_from_name("alpha") != P_ALPHA ||
                   gui_page_from_name("tools") != P_ALPHA || /* W301: neuer Name + alter Alias */
                   strcmp(gui_page_key(P_ALPHA), "tools") != 0)) {
            ok = 0;
            strcpy_s(why, sizeof why, "Navigation/Name");
        }
        if (ok && (ComboBox_GetCount(ctl(ID_AL_MIS)) != 24 || ComboBox_GetCount(ctl(ID_AL_DIFF)) != 7 ||
                   ComboBox_GetCount(ctl(ID_AL_VT)) != 1 + g_al_vt.n || g_al_vt.n < 1 ||
                   ComboBox_GetCurSel(ctl(ID_AL_MIS)) != 1 || !ui_checked(ID_AL_SKIP))) {
            ok = 0;
            _snprintf_s(why, sizeof why, _TRUNCATE, "Listen %d/%d/%d", ComboBox_GetCount(ctl(ID_AL_MIS)),
                        ComboBox_GetCount(ctl(ID_AL_DIFF)), ComboBox_GetCount(ctl(ID_AL_VT)));
        }
        /* Kette der Seite: Pfade wie der Startweg */
        if (ok) {
            char root[1100], eu[1100], us[1100], reu[1100], rus[1100], x[1600];
            alpha_paths(root, eu, us, sizeof root, reu, rus, sizeof reu);
            static const char pre[] = "mission_autostart=5;mission_vt=16;mission_difficulty=-1;"
                                      "mission_skip_intro=1;hdd_root=";
            if (la_extra_args(&s, eu, us, x, sizeof x) != 0 || strncmp(x, pre, sizeof pre - 1) != 0 ||
                !strstr(x, "\\alpha_test\\HDD;hdd_root_us=") || la_path_rel(root, reu) != 0) {
                ok = 0;
                _snprintf_s(why, sizeof why, _TRUNCATE, "Kette '%.120s'", x);
            }
        }
        /* ungueltig bzw. laufender Client: kein Start (Kette bleibt) */
        if (ok) {
            la_sel_t bad = { 5, 12, -1, 1 };
            int r1, r2;
            strcpy_s(g_start_args, sizeof g_start_args, "unveraendert");
            r1 = alpha_start(1, &bad);
            g_save_test_busy = 1;
            r2 = alpha_start(0, &s);
            g_save_test_busy = keep_busy;
            if (r1 != 3 || r2 != 1 || strcmp(g_start_args, "unveraendert") != 0 ||
                strcmp(g_status, LT("gui.alpha_running")) != 0) {
                ok = 0;
                _snprintf_s(why, sizeof why, _TRUNCATE, "kein Start: rc %d/%d", r1, r2);
            }
            set_status_k("", 0);
        }
        /* A/B-Schalter -> user.cfg (Testdatei) */
        if (ok) {
            FILE *f = NULL;
            size_t n = 0;
            u8_GetTempPath(tmp, sizeof tmp);
            _snprintf_s(uc, sizeof uc, _TRUNCATE, "%sw272_user_%lu.cfg", tmp, GetCurrentProcessId());
            u8_DeleteFile(uc);
            strcpy_s(g_ucfg_test, sizeof g_ucfg_test, uc);
            alpha_ab_sync();
            z0 = ui_checked(ID_AL_ZCONS);
            on_command(ID_AL_ZCONS, BN_CLICKED);
            on_command(ID_AL_HLERP, BN_CLICKED);
            if (u8_fopen_s(&f, uc, "r") == 0 && f) {
                n = fread(buf, 1, sizeof buf - 1, f);
                fclose(f);
            }
            buf[n] = '\0';
            if (ui_checked(ID_AL_ZCONS) == z0 || !strstr(buf, z0 ? "interp_zcons=0" : "interp_zcons=1") ||
                !strstr(buf, ui_checked(ID_AL_HLERP) ? "interp_hlerp=1" : "interp_hlerp=0") ||
                g_status_kind != 1) {
                ok = 0;
                _snprintf_s(why, sizeof why, _TRUNCATE, "A/B '%.80s'", buf);
            }
            on_command(ID_AL_ZCONS, BN_CLICKED);
            alpha_ab_sync();
            if (ok && ui_checked(ID_AL_ZCONS) != z0) {
                ok = 0;
                strcpy_s(why, sizeof why, "A/B Rundweg");
            }
            u8_DeleteFile(uc);
            g_ucfg_test[0] = '\0';
            set_status_k("", 0);
        }
        /* alpha_tools=0: Seite + Navigation weg, Strg+Tab ueberspringt sie */
        if (ok) {
            g_alpha_tools = 0;
            alpha_tools_apply();
            show_page(P_ALPHA);
            if (ui_visible_on(nav, P_PLAY) || g_page == P_ALPHA || ui_visible_on(ui_find(ID_LSET_TOOLS), P_LSET) || nav_next(P_ADV, 1) != P_PLAY ||
                nav_next(P_PLAY, -1) != P_ADV) {
                ok = 0;
                strcpy_s(why, sizeof why, "alpha_tools=0");
            }
            g_alpha_tools = 1;
            alpha_tools_apply();
            if (ok && (!ui_visible_on(ui_find(ID_LSET_TOOLS), P_LSET) || ui_visible_on(nav, P_PLAY))) { /* W397 */
                ok = 0;
                strcpy_s(why, sizeof why, "alpha_tools=1 zurueck");
            }
        }
        /* launcher_ui.cfg: alpha_tools bleibt beim Speichern erhalten */
        if (ok) {
            char d[1100], p[1200];
            _snprintf_s(d, sizeof d, _TRUNCATE, "%sw272_ui_%lu", tmp, GetCurrentProcessId());
            lcfg_mkdir_p(d);
            g_alpha_tools = 0;
            ui_cfg_save_bg(d, 1);
            g_alpha_tools = 1;
            if (ui_cfg_load_tools(d) != 0 || ui_cfg_load_key(d, "test_tools", 1) != 0 || ui_cfg_load_bg(d) != 1) {
                ok = 0;
                strcpy_s(why, sizeof why, "launcher_ui.cfg test_tools");
            }
            ui_cfg_path(d, p, sizeof p);
            u8_DeleteFile(p);
            if (ok && ui_cfg_load_tools(d) != 1) {
                ok = 0;
                strcpy_s(why, sizeof why, "test_tools Standard");
            }
            /* W301: alte launcher_ui.cfg (nur alpha_tools) gilt weiter; test_tools gewinnt */
            if (ok) {
                FILE *f = NULL;
                if (u8_fopen_s(&f, p, "w") == 0 && f) {
                    fputs("bg_image=1\nalpha_tools=0\n", f);
                    fclose(f);
                }
                if (ui_cfg_load_tools(d) != 0) {
                    ok = 0;
                    strcpy_s(why, sizeof why, "alte cfg alpha_tools=0 nicht gelesen");
                }
                if (ok && u8_fopen_s(&f, p, "w") == 0 && f) {
                    fputs("alpha_tools=0\ntest_tools=1\n", f);
                    fclose(f);
                }
                if (ok && ui_cfg_load_tools(d) != 1) {
                    ok = 0;
                    strcpy_s(why, sizeof why, "test_tools gewinnt nicht");
                }
                u8_DeleteFile(p);
            }
            {
                wchar_t wd[1100];
                u8_to_w(d, wd, 1100);
                RemoveDirectoryW(wd);
            }
        }
        printf("     (W272 a: %d Elemente im Fenster, VT-Liste %d Modelle, Quelle %d, interp_guard %s)\n",
               g_ui_n, g_al_vt.n, g_al_vt.src, g_al_guard > 0 ? "bekannt" : "unbekannt (Schalter aus)");
        gui_teardown();
    } else {
        strcpy_s(why, sizeof why, "Fenster");
    }
    if (ok) {
        printf("PASS W272 test tools page (navigation after Advanced, name tools + alias alpha, lists, page start args, no start when invalid/running, A/B -> user.cfg, test_tools=0 hides, old alpha_tools read)\n");
        (*pass)++;
    } else {
        printf("FAIL W272 alpha page: %s\n", why);
        (*fail)++;
    }
}
