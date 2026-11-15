/* launcher_diag.inc - W279 DIAG-CRASH: Oberflaeche der Absturzberichte und des
 * Diagnosepakets. Eingebunden in launcher_gui.c (nutzt mk, card, label, LT,
 * set_status_k, ui_msgbox, ui_refresh_visibility ...); Logik ohne Fenster in
 * launcher_diag.c.
 *  - Spielen: Karte "The last session crashed" an der Stelle der Update-Karte
 *    (hat Vorrang), einmal je Bericht (last_seen.txt beim Zeigen).
 *  - Alpha Test: Karte "Problem reports" mit "Create diagnostic package" und
 *    "Open reports folder". Nichts wird hochgeladen. */

#define ID_DG_OPEN 1320
#define ID_DG_GH 1321
#define ID_DG_DISMISS 1322
#define ID_DG_PKG 1323
#define ID_DG_REPORTS 1324

#define DG_X (AL_C2)               /* Alpha Test: rechte Haelfte unter dem Teststart */
#define DG_W (CX0 + CW - AL_C2)
#define DG_SHOT_NAME "20261115-071503-crash"

static int g_dg_show;        /* Karte auf Spielen sichtbar */
static int g_dg_test;        /* Selbsttest: Erkennung auch im Testbetrieb */
static sb_crash_ent_t g_dg_ent;
static char g_dg_sum[400];
static int g_dg_nrep = -1;   /* Berichte im Ordner (Alpha-Karte), -1 = ungeprueft */
static char g_dg_newest[64];

static void dg_crash_dir(char *out, size_t cap)
{
    ld_dirs(g_client_disp, out, NULL, NULL, cap);
}

/* Start: neuer Bericht seit dem letzten Hinweis? Shots: --state crash = Fixture. */
static void dg_init(void)
{
    char cd[1100];
    g_dg_show = 0;
    g_dg_nrep = -1;
    if (g_o.shot == 2 && strcmp(g_o.state, "crash") == 0) {
        memset(&g_dg_ent, 0, sizeof g_dg_ent);
        strcpy_s(g_dg_ent.name, sizeof g_dg_ent.name, DG_SHOT_NAME);
        g_dg_ent.fatal = 1;
        g_dg_ent.has_dump = 1;
        strcpy_s(g_dg_sum, sizeof g_dg_sum, "exception: code=0xC0000005 at 0x0012ABCD (no module) game code");
        g_dg_show = 1;
        return;
    }
    if (g_o.shot != 0 && !g_dg_test) {
        return;
    }
    dg_crash_dir(cd, sizeof cd);
    if (ld_crash_new(cd, &g_dg_ent)) {
        ld_crash_summary(cd, g_dg_ent.name, g_dg_sum, sizeof g_dg_sum);
        g_dg_show = 1;
        ld_crash_mark_seen(cd, g_dg_ent.name); /* einmalig: naechster Start zeigt ihn nicht mehr */
    }
}

/* ui_visible_on: Knoepfe der Karten (Update-Karte weicht der Absturz-Karte). */
static int dg_ctl_visible(int id)
{
    if (id == ID_DG_OPEN || id == ID_DG_GH || id == ID_DG_DISMISS) {
        return g_dg_show && g_game != LGAME_LOC;
    }
    if ((id == ID_UPD_YES || id == ID_UPD_NO || id == ID_UPD_OPEN) && g_dg_show) {
        return 0;
    }
    return 1;
}

/* Spielen: 1 = Karte gezeichnet (Update-Karte entfaellt). */
static int dg_paint_play(void)
{
    char t[600];
    if (!g_dg_show) {
        return 0;
    }
    card(UPD_X, UPD_Y, UPD_W, 134,
         LT(g_dg_ent.kind == SB_CRASH_KIND_HANG ? "gui.dg_title_hang" : "gui.dg_title"));
    L_fill(UPD_X + 4, UPD_Y + 14, 3, 20, C_WARN, 255, 1);
    label1(F_TINY, LT(g_dg_ent.kind == SB_CRASH_KIND_HANG ? "gui.dg_saved_hang" : "gui.dg_saved"), UPD_X + 18,
           UPD_Y + 36, UPD_W - 36, 16, C_TEXT);
    _snprintf_s(t, sizeof t, _TRUNCATE, "%s", g_dg_sum[0] ? g_dg_sum : g_dg_ent.name);
    L_text(F_TINY, t, UPD_X + 18, UPD_Y + 100, UPD_W - 36 - 96, 16, C_DIM,
           DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    return 1;
}

/* Alpha Test: Karte "Problem reports" (rechte Haelfte unter dem Teststart). */
static void dg_paint_alpha(int y, int h)
{
    char t[300];
    card(DG_X, y, DG_W, h, LT("gui.dg_card"));
    label(F_TINY, LT("gui.dg_card_txt"), DG_X + 18, y + 38, DG_W - 36, 48, C_DIM);
    if (g_dg_nrep < 0) {
        char cd[1100];
        sb_crash_ent_t l[1];
        if (g_o.shot == 2) { /* Bild: Beispielstand (kein Nutzerordner) */
            g_dg_nrep = 2;
            strcpy_s(g_dg_newest, sizeof g_dg_newest, DG_SHOT_NAME);
        } else {
            static sb_crash_ent_t all[64];
            dg_crash_dir(cd, sizeof cd);
            g_dg_nrep = sb_diag_crash_list(cd, all, 64);
            g_dg_newest[0] = '\0';
            if (g_dg_nrep > 0) {
                strcpy_s(g_dg_newest, sizeof g_dg_newest, all[0].name);
            }
            (void)l;
        }
    }
    if (g_dg_nrep > 0 && strlen(g_dg_newest) >= 15) {
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.dg_count"), g_dg_nrep, g_dg_newest[6], g_dg_newest[7],
                    g_dg_newest[4], g_dg_newest[5], g_dg_newest[9], g_dg_newest[10], g_dg_newest[11],
                    g_dg_newest[12]);
    } else {
        strcpy_s(t, sizeof t, LT("gui.dg_none"));
    }
    label1(F_TINY, t, DG_X + 18, y + 88, DG_W - 36, 16, C_TEXT);
}

static void dg_build_controls(HWND w)
{
    mk(w, K_BTN, ID_DG_OPEN, P_PLAY, UPD_X + 18, UPD_Y + 58, 160, 28, LT("gui.dg_open"), 0);
    mk(w, K_BTN, ID_DG_GH, P_PLAY, UPD_X + 18 + 160 + 8, UPD_Y + 58, UPD_W - 36 - 168, 28, LT("gui.dg_gh"), 0);
    mk(w, K_BTN, ID_DG_DISMISS, P_PLAY, UPD_X + UPD_W - 18 - 88, UPD_Y + 96, 88, 28, LT("gui.dg_dismiss"), 0);
    mk(w, K_ACCENT, ID_DG_PKG, P_ALPHA, DG_X + 18, AL_Y2 + 110, DG_W - 36, 30, LT("gui.dg_pkg"), 0);
    mk(w, K_BTN, ID_DG_REPORTS, P_ALPHA, DG_X + 18, AL_Y2 + 146, DG_W - 36, 28, LT("gui.dg_reports"), 0);
}

static void dg_relabel(void)
{
    ui_settext(ctl(ID_DG_OPEN), LT("gui.dg_open"));
    ui_settext(ctl(ID_DG_GH), LT("gui.dg_gh"));
    ui_settext(ctl(ID_DG_DISMISS), LT("gui.dg_dismiss"));
    ui_settext(ctl(ID_DG_PKG), LT("gui.dg_pkg"));
    ui_settext(ctl(ID_DG_REPORTS), LT("gui.dg_reports"));
}

/* Ordner im Explorer (sel = Datei markieren). Shots/Tests: nur Konsole. */
static void dg_explore(const char *path, int sel)
{
    wchar_t wp[1100], arg[1200];
    if (g_o.shot != 0) {
        printf("[open] %s\n", path);
        return;
    }
    u8_to_w(path, wp, 1100);
    if (sel) {
        _snwprintf_s(arg, 1200, _TRUNCATE, L"/select,\"%s\"", wp);
        ShellExecuteW(g_hwnd, L"open", L"explorer.exe", arg, NULL, SW_SHOWNORMAL);
    } else {
        ShellExecuteW(g_hwnd, L"open", wp, NULL, NULL, SW_SHOWNORMAL);
    }
}

static void dg_report_github(void)
{
    char cd[1100], zip[1200], url[8192], t[1400];
    const char *zn;
    dg_crash_dir(cd, sizeof cd);
    if (!ld_crash_zip(cd, g_dg_ent.name, zip, sizeof zip)) {
        set_status_k(LT("gui.dg_zip_fail"), 2);
        return;
    }
    zn = strrchr(zip, '\\');
    zn = zn ? zn + 1 : zip;
    if (!ld_issue_url(cd, g_dg_ent.name, zn, url, sizeof url)) {
        set_status_k(LT("gui.dg_zip_fail"), 2);
        return;
    }
    dg_explore(zip, 1);
    if (g_o.shot != 0) {
        printf("[url] %s\n", url);
    } else {
        wchar_t *wu = u8_wdup(url);
        if (wu) {
            ShellExecuteW(g_hwnd, L"open", wu, NULL, NULL, SW_SHOWNORMAL); /* Nutzeraktion, nur die Issue-Seite */
            free(wu);
        }
    }
    _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.dg_gh_done"), zn);
    set_status_k(t, 1);
}

/* Diagnosepaket: Inhalt zeigen, bestaetigen lassen, bauen, Ordner oeffnen. */
static int dg_package(int ask)
{
    ld_pkg_t r;
    char pj[1100], t[1600];
    if (ask && ui_msgbox(g_hwnd, LT("gui.dg_pkg_confirm"), LT("gui.dg_pkg_title"), MB_ICONINFORMATION | MB_OKCANCEL) !=
                   IDOK) {
        return 0;
    }
    packs_json_path(pj, sizeof pj);
    if (!ld_pkg_build(g_client_disp, pj, &r)) {
        set_status_k(LT("gui.dg_pkg_fail"), 2);
        return 0;
    }
    _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.dg_pkg_done"), r.files, (unsigned)((r.bytes + 1023) / 1024));
    set_status_k(t, 1);
    dg_explore(r.zip, 1);
    return 1;
}

/* 1 = Element dieses Moduls (behandelt). */
static int dg_on_command(int id)
{
    char cd[1100], sub[1200];
    switch (id) {
    case ID_DG_OPEN:
        dg_crash_dir(cd, sizeof cd);
        _snprintf_s(sub, sizeof sub, _TRUNCATE, "%s\\%s", cd, g_dg_ent.name);
        dg_explore(is_dir_u8(sub) ? sub : cd, 0);
        return 1;
    case ID_DG_GH:
        dg_report_github();
        return 1;
    case ID_DG_DISMISS:
        g_dg_show = 0;
        ui_refresh_visibility();
        redraw_all();
        return 1;
    case ID_DG_PKG:
        dg_package(1);
        return 1;
    case ID_DG_REPORTS:
        dg_crash_dir(cd, sizeof cd);
        if (!is_dir_u8(cd)) {
            set_status_k(LT("gui.dg_none"), 0);
        } else {
            dg_explore(cd, 0);
        }
        return 1;
    }
    return 0;
}

/* CLI --diag-package: Paket ohne Fenster (Pfade wie die Oberflaeche). */
int gui_diag_package_cli(const char *client_override)
{
    gui_opts_t o;
    ld_pkg_t r;
    char pj[1100];
    int ok = 0;
    memset(&o, 0, sizeof o);
    o.shot = 5;
    g_o = o;
    if (gui_init(client_override)) {
        packs_json_path(pj, sizeof pj);
        ok = ld_pkg_build(g_client_disp, pj, &r);
        if (ok) {
            printf("diag: %s (%d Dateien, %llu Bytes, %d Logs, %d Berichte, Profil %d, %d Ersetzungen, %d Eingabe-Zeilen "
                   "entfernt, Nachpruefung Benutzername %d)\n",
                   r.zip, r.files, r.bytes, r.logs, r.crashes, r.profile, r.repl, r.dropped, r.clean_hits);
        } else {
            printf("diag: Paket nicht geschrieben\n");
        }
    }
    return ok ? 0 : 1;
}

/* Selbsttest (Oberflaeche): Karte erscheint einmal, Knoepfe nur mit Karte,
 * Update-Knoepfe weichen, Verwerfen blendet aus, Alpha-Karte vorhanden. */
void dg_gui_selftest(const char *client_override, int *pass, int *fail)
{
    gui_opts_t o;
    char why[300] = "", tmp[1024], cd[1200];
    int ok = 0;
    memset(&o, 0, sizeof o);
    o.shot = 5;
    g_o = o;
    u8_GetTempPath(tmp, sizeof tmp);
    _snprintf_s(cd, sizeof cd, _TRUNCATE, "%sw279_gui_%lu\\crash", tmp, GetCurrentProcessId());
    if (gui_init(client_override) && gui_create(1)) {
        char sub[1300], sp[1300];
        FILE *f = NULL;
        build_controls();
        lcfg_mkdir_p(cd);
        _snprintf_s(sub, sizeof sub, _TRUNCATE, "%s\\20261115-071503-crash", cd);
        lcfg_mkdir_p(sub);
        _snprintf_s(sp, sizeof sp, _TRUNCATE, "%s\\status.txt", sub);
        if (u8_fopen_s(&f, sp, "wb") == 0 && f) {
            fputs("fatal", f);
            fclose(f);
        }
        ld_test_dirs(cd, NULL, NULL);
        g_dg_test = 1;
        ok = 1;
        dg_init(); /* 1. Start nach dem Absturz */
        ui_refresh_visibility();
        show_page(P_PLAY);
        if (!g_dg_show || !ui_visible_on(ui_find(ID_DG_GH), P_PLAY) || !ui_visible_on(ui_find(ID_DG_DISMISS), P_PLAY) ||
            ui_visible_on(ui_find(ID_DG_GH), P_ADV)) {
            ok = 0;
            strcpy_s(why, sizeof why, "Karte/Knoepfe nach dem Absturz");
        }
        if (ok) {
            g_upd_ask = 1; /* Update-Erstabfrage weicht der Absturz-Karte */
            if (ui_visible_on(ui_find(ID_UPD_YES), P_PLAY)) {
                ok = 0;
                strcpy_s(why, sizeof why, "Update-Knoepfe trotz Absturz-Karte");
            }
            g_upd_ask = 0;
        }
        if (ok) {
            on_command(ID_DG_DISMISS, BN_CLICKED);
            if (g_dg_show || ui_visible_on(ui_find(ID_DG_GH), P_PLAY)) {
                ok = 0;
                strcpy_s(why, sizeof why, "Verwerfen");
            }
        }
        if (ok) {
            dg_init(); /* 2. Start: derselbe Bericht nicht noch einmal */
            if (g_dg_show) {
                ok = 0;
                strcpy_s(why, sizeof why, "Hinweis beim zweiten Start erneut");
            }
        }
        if (ok && (!ui_visible_on(ui_find(ID_DG_PKG), P_ALPHA) || !ui_visible_on(ui_find(ID_DG_REPORTS), P_ALPHA) ||
                   ui_visible_on(ui_find(ID_DG_PKG), P_PLAY))) {
            ok = 0;
            strcpy_s(why, sizeof why, "Alpha-Karte Problem reports");
        }
        g_dg_test = 0;
        ld_test_dirs(NULL, NULL, NULL);
        u8_DeleteFile(sp);
        _snprintf_s(sp, sizeof sp, _TRUNCATE, "%s\\last_seen.txt", cd);
        u8_DeleteFile(sp);
        {
            wchar_t w[1300];
            u8_to_w(sub, w, 1300);
            RemoveDirectoryW(w);
            u8_to_w(cd, w, 1300);
            RemoveDirectoryW(w);
            *wcsrchr(w, L'\\') = 0;
            RemoveDirectoryW(w);
        }
        gui_teardown();
    } else {
        strcpy_s(why, sizeof why, "Fenster");
    }
    if (ok) {
        printf("PASS W279 crash card (shown once after a crash, update card yields, Dismiss hides, Problem reports on "
               "Alpha Test)\n");
        (*pass)++;
    } else {
        printf("FAIL W279 crash card: %s\n", why);
        (*fail)++;
    }
}
