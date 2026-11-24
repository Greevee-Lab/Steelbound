/* launcher_gui_content.c - Replays (W254) + Seite Packs (W148/W172) + Sprache der Oberflaeche.
 * W462 LAUNCHER-SPLIT: aus launcher_gui.c verschoben (unveraendert), gemeinsamer Zustand
 * in launcher_gui_int.h. */
#include "launcher_gui_int.h"
/* W254 REPLAY-LAUNCHER (experimentell): gespeicherte Replays der       */
/* gewaehlten Fassung auflisten und direkt abspielen. Der Client fuehrt */
/* das Spielmenue selbst bis zur Wiedergabe ([input] replay_autoplay).  */
/* Eigener Dialog (die Seite Erweitert hat keinen Platz fuer eine Liste)*/
/* ------------------------------------------------------------------ */
#define IDRP_LIST 701
#define IDRP_INFO 702
#define IDRP_PLAY 703
#define IDRP_CLOSE 704
#define IDRP_EXP 705
#define IDRP_CAM 706
#define IDRP_CAMLBL 707
#define IDRP_REFRESH 708
#define IDRP_HEAD 709
#define IDRP_VHEAD 710 /* W255 VIDEO-EXPORT */
#define IDRP_VRES 711
#define IDRP_VFPS 712
#define IDRP_VDIRLBL 713
#define IDRP_VDIR 714
#define IDRP_VBROWSE 715
#define IDRP_VHINT 716
#define IDRP_VEXPORT 717
#define RP_MAX 64

static int g_rp_done, g_rp_n, g_rp_demo, g_rp_go; /* g_rp_go: 1 = abspielen, 2 = Video exportieren (W255) */
static ls_replay g_rp_list[RP_MAX];
char g_rp_args[1000];
/* W255 VIDEO-EXPORT: Auswahl bleibt fuer die Sitzung; laufender Export (Ordner fuer "Open folder") */
static int g_vx_res, g_vx_fps60;
char g_vx_dir[700], g_vx_dir_open[700], g_vx_id[40];

/* Nur Shot-Modus: Beispieleintraege (keine echten Spielstaende im Bild). */
static void rp_demo_fill(void)
{
    static const char *const nm[3] = { "MIS00 00 11/14/26", "MIS04 00 11/2/26", "MIS01 01 11/13/26" };
    static const int st[3] = { 0, 1, 0 };
    SYSTEMTIME t = { 2026, 11, 6, 14, 13, 40, 0, 0 };
    int i;
    memset(g_rp_list, 0, sizeof g_rp_list);
    for (i = 0; i < 3; i++) {
        _snwprintf_s(g_rp_list[i].id, 33, _TRUNCATE, L"0A0B0C0D0E0%d", i + 1);
        strcpy_s(g_rp_list[i].name, sizeof g_rp_list[i].name, nm[i]);
        g_rp_list[i].mission = (nm[i][3] - '0') * 10 + (nm[i][4] - '0') + 1;
        g_rp_list[i].state = st[i];
        t.wDay = (WORD)(14 - i * 3);
        t.wHour = (WORD)(13 - i);
        SystemTimeToFileTime(&t, &g_rp_list[i].time); /* als UTC (nur Bild) */
    }
    g_rp_n = 3;
}

static void rp_time_str(const FILETIME *ft, char *out, size_t cap)
{
    FILETIME lt;
    SYSTEMTIME st;
    if (!FileTimeToLocalFileTime(ft, &lt) || !FileTimeToSystemTime(&lt, &st)) {
        strcpy_s(out, cap, "-");
        return;
    }
    _snprintf_s(out, cap, _TRUNCATE, "%04u-%02u-%02u %02u:%02u", st.wYear, st.wMonth, st.wDay,
                st.wHour, st.wMinute);
}

static int rp_sel(HWND hd)
{
    HWND lb = bctl(hd, IDRP_LIST);
    int s = ListBox_GetCurSel(lb);
    int i = s >= 0 ? (int)ListBox_GetItemData(lb, s) : -1;
    return (i >= 0 && i < g_rp_n) ? i : -1;
}

static void rp_refresh_info(HWND hd)
{
    char t[900], d[40], id8[40];
    int i = rp_sel(hd);
    EnableWindow(bctl(hd, IDRP_PLAY), i >= 0 && g_rp_list[i].state != 2);
    EnableWindow(bctl(hd, IDRP_VEXPORT), i >= 0 && g_rp_list[i].state != 2); /* W255 */
    if (i < 0) {
        ui_settext(bctl(hd, IDRP_INFO), g_rp_n ? "" : LT("gui.rp_none"));
        return;
    }
    rp_time_str(&g_rp_list[i].time, d, sizeof d);
    w_to_u8(g_rp_list[i].id, id8, sizeof id8);
    t[0] = 0;
    if (g_rp_list[i].mission > 0) {
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.rp_mission"), g_rp_list[i].mission);
        strcat_s(t, sizeof t, "\r\n");
    }
    if (g_rp_list[i].name[0]) {
        _snprintf_s(t + strlen(t), sizeof t - strlen(t), _TRUNCATE, LT("gui.rp_i_name"), g_rp_list[i].name);
        strcat_s(t, sizeof t, "\r\n");
    }
    _snprintf_s(t + strlen(t), sizeof t - strlen(t), _TRUNCATE, LT("gui.rp_i_date"), d);
    strcat_s(t, sizeof t, "\r\n");
    _snprintf_s(t + strlen(t), sizeof t - strlen(t), _TRUNCATE, LT("gui.rp_i_id"), id8);
    strcat_s(t, sizeof t, "\r\n\r\n");
    strcat_s(t, sizeof t, LT(g_rp_list[i].state == 1 ? "gui.rp_st_xbox"
                             : g_rp_list[i].state == 2 ? "gui.rp_st_bad" : "gui.rp_st_ok"));
    ui_settext(bctl(hd, IDRP_INFO), t);
}

static void rp_rescan(HWND hd)
{
    HWND lb = bctl(hd, IDRP_LIST);
    int i;
    ListBox_ResetContent(lb);
    if (g_rp_demo) {
        rp_demo_fill();
    } else {
        char hdd[1024];
        wchar_t w[1024];
        adv_hdd_path(hdd, sizeof hdd); /* Fassung des eingestellten Spiels (EU/US) */
        u8_to_w(hdd, w, 1024);
        g_rp_n = ls_replays(w, g_rp_list, RP_MAX);
    }
    for (i = 0; i < g_rp_n; i++) {
        char e[200], d[40], m[64];
        rp_time_str(&g_rp_list[i].time, d, sizeof d);
        if (g_rp_list[i].mission > 0) {
            _snprintf_s(m, sizeof m, _TRUNCATE, LT("gui.rp_mission"), g_rp_list[i].mission);
        } else {
            strncpy_s(m, sizeof m, LT("gui.rp_unknown"), _TRUNCATE);
        }
        _snprintf_s(e, sizeof e, _TRUNCATE, "%s \xC2\xB7 %s%s", m, d,
                    g_rp_list[i].state == 1 ? " \xC2\xB7 Xbox" : "");
        list_add(lb, e, (LPARAM)i, 1);
    }
    if (g_rp_n > 0) {
        ListBox_SetCurSel(lb, 0);
    }
    rp_refresh_info(hd);
}

static void rp_play(HWND hd)
{
    int i = rp_sel(hd), cam = ComboBox_GetCurSel(bctl(hd, IDRP_CAM));
    char hdd[1024];
    wchar_t w[1024];
    if (i < 0) {
        return;
    }
    cam = cam <= 0 ? -1 : cam - 1; /* Eintrag 0 = wie aufgezeichnet, dann Ansicht 0..6 */
    ls_replay_set_args(g_rp_list[i].id, cam, g_rp_args, sizeof g_rp_args);
    if (g_rp_demo) {
        g_rp_go = 1;
        g_rp_done = 1;
        return;
    }
    if (client_running()) {
        ui_msgbox(hd, LT("gui.rp_running"), LT("gui.rp_title"), MB_OK | MB_ICONWARNING);
        return;
    }
    adv_hdd_path(hdd, sizeof hdd);
    u8_to_w(hdd, w, 1024);
    if (!ls_replay_exists(w, g_rp_list[i].id)) { /* zwischendurch geloescht/verschoben */
        ui_msgbox(hd, LT("gui.rp_missing"), LT("gui.rp_title"), MB_OK | MB_ICONWARNING);
        rp_rescan(hd);
        return;
    }
    g_rp_go = 1;
    g_rp_done = 1;
}

/* W255 VIDEO-EXPORT: Standard-Videoordner = <Fotoordner>\Videos (wie der Client bei video_export=auto) */
void rp_video_default_dir(char *out, size_t cap)
{
    char ph[700];
    extras_dir(ph, sizeof ph);
    _snprintf_s(out, cap, _TRUNCATE, "%s\\Videos", ph);
}

/* W255: "Export video..." - Set-Kette Replay + Video, Pruefungen wie "Play replay" */
static void rp_export(HWND hd)
{
    int i = rp_sel(hd), cam = ComboBox_GetCurSel(bctl(hd, IDRP_CAM));
    char dir[700], def[700], acp[700], va[800], hdd[1024], t[1200];
    wchar_t w[1024];
    if (i < 0) {
        return;
    }
    cam = cam <= 0 ? -1 : cam - 1;
    g_vx_res = ComboBox_GetCurSel(bctl(hd, IDRP_VRES));
    if (g_vx_res < 0 || g_vx_res >= LS_VIDEO_RES_N) {
        g_vx_res = 0;
    }
    g_vx_fps60 = ComboBox_GetCurSel(bctl(hd, IDRP_VFPS)) == 1;
    ui_gettext(bctl(hd, IDRP_VDIR), dir, sizeof dir);
    trim_u8(dir);
    rp_video_default_dir(def, sizeof def);
    strcpy_s(g_vx_dir, sizeof g_vx_dir, dir);
    acp[0] = 0; /* leer bzw. Standardordner -> "auto" (Client rechnet denselben Ordner) */
    if (dir[0] && _stricmp(dir, def) != 0 && u8_to_acp(dir, acp, sizeof acp) == 2) {
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("cfg.ansi"), LT("gui.rp_vx_what"), dir);
        ui_msgbox(hd, t, LT("gui.rp_vx_title"), MB_OK | MB_ICONWARNING);
        return;
    }
    if (ls_video_set_args(acp, g_vx_res, g_vx_fps60 ? 60 : 30, va, sizeof va) != 0) {
        ui_msgbox(hd, LT("gui.rp_vx_badchar"), LT("gui.rp_vx_title"), MB_OK | MB_ICONWARNING);
        return;
    }
    ls_replay_set_args(g_rp_list[i].id, cam, g_rp_args, sizeof g_rp_args);
    strcat_s(g_rp_args, sizeof g_rp_args, ";");
    strcat_s(g_rp_args, sizeof g_rp_args, va);
    strcpy_s(g_vx_dir_open, sizeof g_vx_dir_open, dir[0] ? dir : def);
    w_to_u8(g_rp_list[i].id, g_vx_id, sizeof g_vx_id);
    if (g_rp_demo) {
        g_rp_go = 2;
        g_rp_done = 1;
        return;
    }
    if (client_running()) {
        ui_msgbox(hd, LT("gui.rp_running"), LT("gui.rp_vx_title"), MB_OK | MB_ICONWARNING);
        return;
    }
    adv_hdd_path(hdd, sizeof hdd);
    u8_to_w(hdd, w, 1024);
    if (!ls_replay_exists(w, g_rp_list[i].id)) {
        ui_msgbox(hd, LT("gui.rp_missing"), LT("gui.rp_vx_title"), MB_OK | MB_ICONWARNING);
        rp_rescan(hd);
        return;
    }
    g_rp_go = 2;
    g_rp_done = 1;
}

/* W255: Export beendet (Client-Exitcode): Meldung, bei Erfolg "Ordner oeffnen?" */
void rp_export_done(unsigned long code)
{
    char t[1400];
    g_vx_pending = 0;
    if (code == 0) {
        set_status_k(LT("gui.rp_vx_done_status"), 1);
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.rp_vx_done"), g_vx_dir_open);
        if (ui_msgbox(g_hwnd, t, LT("gui.rp_vx_title"), MB_YESNO | MB_ICONINFORMATION) == IDYES) {
            wchar_t wp[700];
            u8_to_w(g_vx_dir_open, wp, 700);
            ShellExecuteW(g_hwnd, L"open", wp, NULL, NULL, SW_SHOWNORMAL);
        }
        return;
    }
    if (g_vx_id[0] && g_vx_dir_open[0]) { /* abgebrochener Client (z. B. beendet): nur eigene Reste dieses Replays */
        wchar_t pat[800], f[800], *slash;
        WIN32_FIND_DATAW fd;
        HANDLE h;
        char p8[800];
        _snprintf_s(p8, sizeof p8, _TRUNCATE, "%s\\Steelbound_Replay_%s_*.mp4.part", g_vx_dir_open, g_vx_id);
        u8_to_w(p8, pat, 800);
        slash = wcsrchr(pat, L'\\');
        h = FindFirstFileW(pat, &fd);
        if (h != INVALID_HANDLE_VALUE && slash) {
            do {
                if (!(fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
                    _snwprintf_s(f, 800, _TRUNCATE, L"%.*s\\%s", (int)(slash - pat), pat, fd.cFileName);
                    DeleteFileW(f);
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    if (code == 2) {
        strncpy_s(t, sizeof t, LT("gui.rp_vx_fail2"), _TRUNCATE);
    } else if (code == 3) {
        strncpy_s(t, sizeof t, LT("gui.rp_vx_fail3"), _TRUNCATE);
    } else {
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.rp_vx_failx"), code);
    }
    set_status_k(t, 2);
    ui_msgbox(g_hwnd, t, LT("gui.rp_vx_title"), MB_OK | MB_ICONWARNING);
}

static LRESULT CALLBACK rp_proc(HWND hd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (id == IDRP_CLOSE || id == IDCANCEL) {
            g_rp_done = 1;
        } else if (id == IDRP_REFRESH) {
            rp_rescan(hd);
        } else if (id == IDRP_PLAY || (id == IDRP_LIST && code == LBN_DBLCLK)) {
            rp_play(hd);
        } else if (id == IDRP_VEXPORT) { /* W255 */
            rp_export(hd);
        } else if (id == IDRP_VBROWSE) { /* W255: Videoordner waehlen */
            BROWSEINFOW bi;
            LPITEMIDLIST pidl;
            wchar_t wd[MAX_PATH], title[160];
            if (g_rp_demo || g_o.shot != 0) {
                printf("[browse] video_dir\n");
                return 0;
            }
            memset(&bi, 0, sizeof bi);
            bi.hwndOwner = hd;
            u8_to_w(LT("gui.rp_vx_pick"), title, 160);
            bi.lpszTitle = title;
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            pidl = SHBrowseForFolderW(&bi);
            if (pidl) {
                if (SHGetPathFromIDListW(pidl, wd)) { SetWindowTextW(bctl(hd, IDRP_VDIR), wd); }
                CoTaskMemFree(pidl);
            }
        } else if (id == IDRP_LIST && code == LBN_SELCHANGE) {
            rp_refresh_info(hd);
        }
        return 0;
    }
    case WM_DRAWITEM:
        draw_item((const DRAWITEMSTRUCT *)lp);
        return TRUE;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORSTATIC:
        return ctlcolor((HDC)wp, (HWND)lp, msg == WM_CTLCOLORSTATIC);
    case WM_ERASEBKGND: {
        RECT r;
        GetClientRect(hd, &r);
        if (!g_br_dialog) {
            g_br_dialog = CreateSolidBrush(C_DIALOG);
        }
        FillRect((HDC)wp, &r, g_br_dialog);
        return 1;
    }
    case WM_TIMER:
        if (wp == 9 && g_rp_demo) {
            char path[1200];
            int rc;
            KillTimer(hd, 9);
            RedrawWindow(hd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
            _snprintf_s(path, sizeof path, _TRUNCATE, "%s/w254_replays_%d.png", g_o.out_dir,
                        g_dpi * 100 / 96);
            rc = shot_window(hd, path, 1);
            printf("replay-shot: %s%s\n", rc == 0 ? "" : "FEHLER ", path);
            g_rp_done = 1;
        }
        return 0;
    case WM_CLOSE:
        g_rp_done = 1;
        return 0;
    }
    return DefWindowProcW(hd, msg, wp, lp);
}

/* Modal; 1 = "Abspielen" gewaehlt (g_rp_args = --set-Teil). shot = Bild mit Beispieldaten. */
int replay_dialog_modal(int shot)
{
    static int reg;
    HWND hd, c;
    RECT rc = { 0, 0, 0, 0 };
    MSG msg;
    int ui_mark = g_ui_n, k;
    DWORD exs = shot ? (WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW) : 0;
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    if (!reg) {
        WNDCLASSW wc;
        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = rp_proc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = L"SbReplays";
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        RegisterClassW(&wc);
        reg = 1;
    }
    g_rp_done = 0;
    g_rp_go = 0;
    g_rp_demo = shot;
    g_rp_args[0] = 0;
    rc.right = SX(640);
    rc.bottom = SX(552); /* W255: + Video-Export */
    AdjustWindowRectEx(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, exs);
    if (shot) {
        offscreen_pos(&x, &y);
    } else if (g_hwnd) {
        RECT pr;
        GetWindowRect(g_hwnd, &pr);
        x = pr.left + ((pr.right - pr.left) - (rc.right - rc.left)) / 2;
        y = pr.top + ((pr.bottom - pr.top) - (rc.bottom - rc.top)) / 2;
    }
    {
        wchar_t title[96];
        u8_to_w(LT("gui.rp_title"), title, 96);
        hd = CreateWindowExW(exs, L"SbReplays", title,
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, x, y,
                             rc.right - rc.left, rc.bottom - rc.top, g_hwnd, NULL,
                             GetModuleHandleW(NULL), NULL);
    }
    if (!hd) {
        return 0;
    }
    {
        BOOL dark = TRUE;
        DwmSetWindowAttribute(hd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
    }
    mk(hd, K_STATIC, IDRP_EXP, PG_DIALOG, 12, 10, 616, 52, LT("gui.rp_exp"), WS_VISIBLE);
    mk(hd, K_STATIC, IDRP_HEAD, PG_DIALOG, 12, 72, 300, 18, LT("gui.rp_list"), WS_VISIBLE);
    mk(hd, K_LIST, IDRP_LIST, PG_DIALOG, 12, 94, 300, 400, "", WS_VISIBLE);
    mk(hd, K_BTN, IDRP_REFRESH, PG_DIALOG, 12, 510, 130, 30, LT("gui.rp_refresh"), WS_VISIBLE);
    mk(hd, K_STATIC, IDRP_INFO, PG_DIALOG, 326, 94, 302, 150, "", WS_VISIBLE);
    mk(hd, K_STATIC, IDRP_CAMLBL, PG_DIALOG, 326, 250, 302, 18, LT("gui.rp_cam"), WS_VISIBLE);
    c = mk(hd, K_COMBO, IDRP_CAM, PG_DIALOG, 326, 272, 302, 30, "", WS_VISIBLE);
    combo_add(c, LT("gui.rp_cam_free"));
    combo_add(c, LT("gui.rp_cam_0"));
    combo_add(c, LT("gui.rp_cam_1"));
    for (k = 1; k <= 5; k++) {
        char t[80];
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.rp_cam_n"), k);
        combo_add(c, t);
    }
    ComboBox_SetCurSel(c, 0);
    { /* W255 VIDEO-EXPORT (experimentell): Aufloesung, Bildrate, Ordner */
        char def[700];
        unsigned vw, vh;
        mk(hd, K_STATIC, IDRP_VHEAD, PG_DIALOG, 326, 316, 302, 18, LT("gui.rp_vx_head"), WS_VISIBLE);
        c = mk(hd, K_COMBO, IDRP_VRES, PG_DIALOG, 326, 338, 150, 30, "", WS_VISIBLE);
        for (k = 0; k < LS_VIDEO_RES_N; k++) {
            char t[40];
            ls_video_res(k, &vw, &vh);
            _snprintf_s(t, sizeof t, _TRUNCATE, "%u \xC3\x97 %u", vw, vh);
            combo_add(c, t);
        }
        ComboBox_SetCurSel(c, g_vx_res);
        c = mk(hd, K_COMBO, IDRP_VFPS, PG_DIALOG, 482, 338, 146, 30, "", WS_VISIBLE);
        combo_add(c, LT("gui.rp_vx_fps30"));
        combo_add(c, LT("gui.rp_vx_fps60"));
        ComboBox_SetCurSel(c, g_vx_fps60 ? 1 : 0);
        mk(hd, K_STATIC, IDRP_VDIRLBL, PG_DIALOG, 326, 376, 302, 18, LT("gui.rp_vx_dir"), WS_VISIBLE);
        if (shot) { /* Bild: neutraler Beispielordner statt echter Nutzerpfade */
            strcpy_s(def, sizeof def, "D:\\Videos\\Steelbound");
        } else {
            rp_video_default_dir(def, sizeof def);
        }
        mk(hd, K_EDIT, IDRP_VDIR, PG_DIALOG, 326, 398, 258, 26, g_vx_dir[0] && !shot ? g_vx_dir : def, WS_VISIBLE);
        mk(hd, K_BTN, IDRP_VBROWSE, PG_DIALOG, 590, 398, 38, 26, "\xE2\x80\xA6", WS_VISIBLE);
        mk(hd, K_STATIC, IDRP_VHINT, PG_DIALOG, 326, 430, 302, 70, LT("gui.rp_vx_hint"), WS_VISIBLE);
        mk(hd, K_BTN, IDRP_VEXPORT, PG_DIALOG, 158, 510, 170, 30, LT("gui.rp_vx_btn"), WS_VISIBLE);
    }
    mk(hd, K_ACCENT, IDRP_PLAY, PG_DIALOG, 338, 510, 160, 30, LT("gui.rp_play"), WS_VISIBLE);
    mk(hd, K_BTN, IDRP_CLOSE, PG_DIALOG, 508, 510, 120, 30, LT("gui.btn_close"), WS_VISIBLE);
    rp_rescan(hd);
    if (g_hwnd) {
        EnableWindow(g_hwnd, FALSE);
    }
    if (shot) {
        if (!is_offscreen(hd)) {
            printf("replay-shot: FEHLER Fenster nicht au" U8_SZ "erhalb des Bildschirms - Abbruch\n");
            g_rp_done = 1;
        } else {
            ShowWindow(hd, SW_SHOWNOACTIVATE);
            SetTimer(hd, 9, 400, NULL);
        }
    } else {
        ShowWindow(hd, SW_SHOW);
        SetFocus(bctl(hd, IDRP_LIST));
    }
    UpdateWindow(hd);
    while (!g_rp_done && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (IsDialogMessageW(hd, &msg)) {
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_hwnd) {
        EnableWindow(g_hwnd, TRUE);
        if (!shot) {
            SetFocus(ctl(ID_SAVES_REPLAYS));
        }
    }
    DestroyWindow(hd);
    g_ui_n = ui_mark;
    return shot ? 0 : g_rp_go; /* 1 = abspielen, 2 = Video exportieren (W255) */
}

void replay_dialog_open(void)
{
    int go = replay_dialog_modal(0);
    if (go == 1) {
        if (do_start_ex(0, 0, g_rp_args) == 0) { /* Bildschirmweg, nur dieser Start */
            set_status_k(LT("gui.rp_started"), 1);
        }
    } else if (go == 2) { /* W255: Export - Client unsichtbar (window_hidden setzt er selbst), Meldung am Ende */
        if (do_start_ex(0, 0, g_rp_args) == 0) {
            g_vx_pending = g_client_proc != NULL;
            set_status_k(LT("gui.rp_vx_started"), 1);
        }
    }
}

void open_bind_dialog(void)
{
    char prof[64], path[1200];
    const char *base = NULL;
    const char *name;
    ctrl_profile_from_ui(prof, sizeof prof);
    name = prof[0] ? prof : "keyboard_default"; /* Legacy-Basis */
    if (lctrl_input_profile_path(g_client_dir, name, path, sizeof path) == 0 &&
        u8_GetFileAttributes(path) != INVALID_FILE_ATTRIBUTES) {
        base = path;
    }
    bind_dialog_modal(name, base);
}

/* ---- W419 CTRL-EXPORT: Steuerung exportieren / importieren (.sbcontrols) ----
 * Export: aktives Profil (Legacy = keyboard_default, bitgleich) + Steuerungswerte der Seite + Kalibrierung/
 * Totzonen aus user.cfg (dort schreiben Assistent und HOTAS-Dialog). Import: Datei streng pruefen
 * (lcx_parse), Rueckfrage "als neues Profil", dann wie der Belegungsdialog: Profildatei profiles/user_<x>.ini
 * schreiben (nie ueberschreiben) und auswaehlen; Seitenwerte in die Oberflaeche (gespeichert erst mit
 * Speichern/Starten), Kalibrierung/Totzonen direkt in user.cfg (wie Assistent/HOTAS-Dialog). */
int g_in_gui_selftest; /* (vorlaeufig, Definition W403 unten) */
static lcx_data_t g_cx;    /* gross: nie auf dem Stapel */
char g_cx_name[64]; /* Name des neuen Profils */
static char g_cx_miss[8][96];
static int g_cx_nmiss;

/* 0 = ok; msg = Statuszeile. Ohne Dialog (Selbsttest/Shots rufen das direkt). */
int cx_export_to(const char *path, char *msg, size_t cap)
{
    static launcher_settings_t cur;
    char prof[64], pp[1200], uc[1200];
    cur = g_s;
    ui_to_settings(&cur);
    memset(&g_cx, 0, sizeof g_cx);
    ctrl_profile_from_ui(prof, sizeof prof);
    strcpy_s(g_cx.profile, sizeof g_cx.profile, prof[0] ? prof : "keyboard_default");
    if (lctrl_input_profile_path(g_client_dir, g_cx.profile, pp, sizeof pp) != 0 ||
        u8_GetFileAttributes(pp) == INVALID_FILE_ATTRIBUTES || lctrl_bind_load_file(pp, &g_cx.bind) != 0) {
        _snprintf_s(msg, cap, _TRUNCATE, LT("gui.cx_noprof"), g_cx.profile);
        return 1;
    }
    g_cx.format = LCX_FORMAT;
    strcpy_s(g_cx.launcher, sizeof g_cx.launcher, lupd_version()[0] ? lupd_version() : "dev");
    strcpy_s(g_cx.game, sizeof g_cx.game, g_game == LGAME_LOC ? "loc" : "sb");
    lcx_put_settings(&g_cx, &cur);
    lcfg_user_cfg_path(g_client_dir, uc, sizeof uc);
    lcx_put_cfg(&g_cx, uc);
    if (lcx_write_file(path, &g_cx) != 0) {
        strncpy_s(msg, cap, LT("gui.cx_nowrite"), _TRUNCATE);
        return 1;
    }
    _snprintf_s(msg, cap, _TRUNCATE, LT("gui.cx_exported"), g_cx.profile);
    return 0;
}

/* Pruefen ohne jede Aenderung. 0 = ok (g_cx, g_cx_name, g_cx_miss; Rueckfrage in q), 1 = Fehler (Meldung in q). */
int cx_import_check(const char *path, const ijoy_info_t *list, int nlist, int sbc_present, char *q,
                           size_t qcap)
{
    char err[400];
    int i, nset = 0;
    size_t o;
    g_cx_name[0] = '\0';
    g_cx_nmiss = 0;
    if (lcx_read_file(path, &g_cx, err, sizeof err) != LCX_OK) {
        _snprintf_s(q, qcap, _TRUNCATE, "%s %s", err, LT("gui.cx_nochange"));
        return 1;
    }
    if (lcx_import_name(g_client_dir, g_cx.profile, g_cx_name, sizeof g_cx_name) != 0) {
        _snprintf_s(q, qcap, _TRUNCATE, "%s %s", LT("gui.cx_noname"), LT("gui.cx_nochange"));
        memset(&g_cx, 0, sizeof g_cx);
        return 1;
    }
    for (i = 0; i < g_cx.n_set; i++) { /* W442: Zielhilfe zaehlt auch in LoC (seit W441 Gang-Automatik) */
        const char *eq = strchr(g_cx.set[i], '=');
        if (!eq || (size_t)(eq - g_cx.set[i]) >= 48u) { continue; }
        nset++;
    }
    g_cx_nmiss = lcx_missing(&g_cx, list, nlist, sbc_present, g_cx_miss, 8);
    o = (size_t)_snprintf_s(q, qcap, _TRUNCATE, LT("gui.cx_ask"), g_cx_name);
    if (nset > 0 && o < qcap) {
        o += (size_t)_snprintf_s(q + o, qcap - o, _TRUNCATE, "\r\n\r\n");
        o += (size_t)_snprintf_s(q + o, qcap - o, _TRUNCATE, LT("gui.cx_ask_set"), nset);
    }
    if (g_cx.game[0] && strcmp(g_cx.game, g_game == LGAME_LOC ? "loc" : "sb") != 0 && o < qcap) {
        o += (size_t)_snprintf_s(q + o, qcap - o, _TRUNCATE, "\r\n");
        o += (size_t)_snprintf_s(q + o, qcap - o, _TRUNCATE, LT("gui.cx_ask_game"),
                                 strcmp(g_cx.game, "loc") == 0 ? "Line of Contact" : "Steel Battalion");
    }
    if (g_cx.unknown > 0 && o < qcap) {
        o += (size_t)_snprintf_s(q + o, qcap - o, _TRUNCATE, "\r\n");
        o += (size_t)_snprintf_s(q + o, qcap - o, _TRUNCATE, LT("gui.cx_ask_unknown"), g_cx.unknown);
    }
    if (g_cx_nmiss > 0 && o < qcap) {
        o += (size_t)_snprintf_s(q + o, qcap - o, _TRUNCATE, "\r\n");
        o += (size_t)_snprintf_s(q + o, qcap - o, _TRUNCATE, "%s", LT("gui.cx_ask_missing"));
        for (i = 0; i < g_cx_nmiss && o < qcap; i++) {
            o += (size_t)_snprintf_s(q + o, qcap - o, _TRUNCATE, "%s%s", i ? ", " : " ", g_cx_miss[i]);
        }
    }
    return 0;
}

/* Nach der Rueckfrage: Profil anlegen + waehlen, Werte uebernehmen. cfg = 0: user.cfg nicht schreiben (Shots).
 * 0 = ok, msg = Statuszeile. */
int cx_import_apply(int cfg, char *msg, size_t cap)
{
    char pp[1200], dir[1100], uc[1200], err[512];
    int i;
    size_t o;
    if (!g_cx_name[0] || lctrl_input_profile_path(g_client_dir, g_cx_name, pp, sizeof pp) != 0) {
        _snprintf_s(msg, cap, _TRUNCATE, "%s %s", LT("gui.cx_noname"), LT("gui.cx_nochange"));
        return 1;
    }
    if (u8_GetFileAttributes(pp) != INVALID_FILE_ATTRIBUTES) { /* nie ein bestehendes Profil ueberschreiben */
        _snprintf_s(msg, cap, _TRUNCATE, "%s %s", LT("gui.cx_noname"), LT("gui.cx_nochange"));
        return 1;
    }
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s/profiles", g_client_dir);
    lcfg_mkdir_p(dir);
    if (lctrl_bind_save(pp, &g_cx.bind, g_cx.profile, err, sizeof err) != 0) {
        _snprintf_s(msg, cap, _TRUNCATE, "%s %s", err, LT("gui.cx_nochange"));
        return 1;
    }
    ui_to_settings(&g_s);
    lcx_get_settings(&g_cx, &g_s); /* W442: Zielhilfe und Gang-Automatik auch in LoC uebernommen (wirken in Arcade-Lobbys) */
    strcpy_s(g_s.input_profile, sizeof g_s.input_profile, g_cx_name);
    ui_from_settings(&g_s);
    refresh_ctrl_profiles(g_cx_name);
    update_ctrl_overview();
    ui_apply_rules();
    redraw_all();
    if (cfg) {
        lcfg_user_cfg_path(g_client_dir, uc, sizeof uc);
        if (lcx_apply_cfg(&g_cx, uc) != 0) {
            _snprintf_s(msg, cap, _TRUNCATE, LT("gui.cx_cfgfail"), g_cx_name);
            return 1;
        }
    }
    o = (size_t)_snprintf_s(msg, cap, _TRUNCATE, LT("gui.cx_imported"), g_cx_name);
    if (g_cx_nmiss > 0 && o < cap) {
        o += (size_t)_snprintf_s(msg + o, cap - o, _TRUNCATE, " %s", LT("gui.cx_ask_missing"));
        for (i = 0; i < g_cx_nmiss && o < cap; i++) {
            o += (size_t)_snprintf_s(msg + o, cap - o, _TRUNCATE, "%s%s", i ? ", " : " ", g_cx_miss[i]);
        }
    }
    return 0;
}

/* Datei-Dialog (Speichern/Oeffnen), Filter "*.sbcontrols". 1 = gewaehlt. Nie in Shots/Selbsttests. */
static int cx_file_dialog(int save, const char *defname, char *out, size_t cap)
{
    OPENFILENAMEW ofn;
    static wchar_t file[1024];
    wchar_t filter[256], title[128], *w;
    const char *flt = LT("gui.cx_filter");
    const char *bar = strchr(flt, '|');
    if (g_o.shot || g_in_gui_selftest) {
        return 0;
    }
    file[0] = 0;
    if (defname) { u8_to_w(defname, file, 1024); }
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFile = file;
    ofn.nMaxFile = 1024;
    u8_to_w(LT(save ? "gui.cx_export_title" : "gui.cx_import_title"), title, 128);
    filter[0] = 0;
    if (bar) {
        char a[128];
        size_t n = (size_t)(bar - flt), n1;
        if (n > sizeof a - 1) { n = sizeof a - 1; }
        memcpy(a, flt, n);
        a[n] = '\0';
        u8_to_w(a, filter, 128);
        n1 = wcslen(filter) + 1;
        w = filter + n1;
        u8_to_w(bar + 1, w, (int)(256 - n1 - 2));
        w += wcslen(w) + 1;
        *w = 0;
        ofn.lpstrFilter = filter;
    }
    ofn.lpstrTitle = title;
    ofn.nFilterIndex = 1;
    ofn.lpstrDefExt = L"sbcontrols";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (!(save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn))) {
        return 0;
    }
    w_to_u8(file, out, (int)cap);
    return 1;
}

void ctrl_export(void)
{
    char prof[64], def[96], path[1200], msg[400];
    ctrl_profile_from_ui(prof, sizeof prof);
    _snprintf_s(def, sizeof def, _TRUNCATE, "%s.sbcontrols", prof[0] ? prof : "keyboard_default");
    if (!cx_file_dialog(1, def, path, sizeof path)) {
        return;
    }
    if (cx_export_to(path, msg, sizeof msg) != 0) {
        set_status_k(msg, 2);
        ui_msgbox(g_hwnd, msg, LT("gui.cx_export_title"), MB_ICONERROR | MB_OK);
        return;
    }
    set_status_k(msg, 1);
}

void ctrl_import(void)
{
    static ijoy_info_t list[IJOY_MAX_DEV];
    char path[1200], q[1400], msg[600], st[200];
    int n = 0, k, sbc;
    if (!cx_file_dialog(0, NULL, path, sizeof path)) {
        return;
    }
    if (ijoy_init() == 0) { /* fehlende Geraete melden (nur Namen) */
        n = ijoy_scan();
        if (n > IJOY_MAX_DEV) { n = IJOY_MAX_DEV; }
        for (k = 0; k < n; k++) {
            const ijoy_info_t *in = ijoy_info(k);
            if (in) { list[k] = *in; } else { memset(&list[k], 0, sizeof list[k]); }
        }
    }
    sbc = lctrl_sbc_status(st, sizeof st) == 0;
    if (cx_import_check(path, list, n, sbc, q, sizeof q) != 0) {
        set_status_k(q, 2);
        ui_msgbox(g_hwnd, q, LT("gui.cx_import_title"), MB_ICONWARNING | MB_OK);
        return;
    }
    if (ui_msgbox(g_hwnd, q, LT("gui.cx_import_title"), MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON1) != IDYES) {
        set_status(LT("gui.cx_cancel"));
        return;
    }
    if (cx_import_apply(1, msg, sizeof msg) != 0) {
        set_status_k(msg, 2);
        ui_msgbox(g_hwnd, msg, LT("gui.cx_import_title"), MB_ICONERROR | MB_OK);
        return;
    }
    set_status_k(msg, 1);
}

/* W419: Beispieldatei (Fixture, keine persoenlichen Daten): HOTAS + Pad, Steuerungswerte, zwei unbekannte
 * Eintraege (kuenftige Version). Selbsttest und Shots. */

int cx_put_text(const char *path, const char *text, size_t n)
{
    FILE *f = NULL;
    if (u8_fopen_s(&f, path, "wb") != 0 || !f) { return 1; }
    fwrite(text, 1, n, f);
    fclose(f);
    return 0;
}

/* --ui-shot controls --state cx_import|cx_bad: Import einer Beispieldatei in einen Temp-Client-Ordner (die
 * echten Profile und user.cfg bleiben unberuehrt), Ergebnis in der Statuszeile, Rueckfrage auf stdout. */
void cx_shot_state(const char *state)
{
    char tmp[1024], dir[1100], pd[1200], src[1300], dst[1300], fx[1200], q[1400], msg[600];
    static const char *const ship[3] = { "keyboard_default", "xbox_pad_default", "default_full" };
    int k, bad = strcmp(state, "cx_bad") == 0;
    u8_GetTempPath(tmp, sizeof tmp);
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%sw419_shot_client", tmp);
    _snprintf_s(pd, sizeof pd, _TRUNCATE, "%s/profiles", dir);
    lcfg_mkdir_p(pd);
    for (k = 0; k < 3; k++) { /* nur die mitgelieferten Profile in den Temp-Ordner (Liste wie beim Spieler) */
        wchar_t ws[1300], wd[1300];
        _snprintf_s(src, sizeof src, _TRUNCATE, "%s/profiles/%s.ini", g_client_dir, ship[k]);
        _snprintf_s(dst, sizeof dst, _TRUNCATE, "%s/%s.ini", pd, ship[k]);
        u8_to_w(src, ws, 1300);
        u8_to_w(dst, wd, 1300);
        CopyFileW(ws, wd, FALSE);
    }
    _snprintf_s(dst, sizeof dst, _TRUNCATE, "%s/user_hotas.ini", pd);
    u8_DeleteFile(dst);
    strcpy_s(g_client_dir, sizeof g_client_dir, dir);
    refresh_ctrl_profiles("keyboard_default");
    _snprintf_s(fx, sizeof fx, _TRUNCATE, "%sw419_shot.sbcontrols", tmp);
    if (bad) { /* abgeschnitten: Endmarke fehlt */
        cx_put_text(fx, kCxFixture, sizeof kCxFixture / 2);
    } else {
        cx_put_text(fx, kCxFixture, sizeof kCxFixture - 1);
    }
    if (cx_import_check(fx, NULL, 0, 0, q, sizeof q) != 0) {
        printf("[cx import] %s\n", q);
        set_status_k(q, 2);
    } else {
        printf("[cx ask] %s\n", q);
        if (cx_import_apply(0, msg, sizeof msg) == 0) {
            printf("[cx done] %s\n", msg);
            set_status_k(msg, 1);
        } else {
            set_status_k(msg, 2);
        }
    }
    u8_DeleteFile(fx);
}

/* ------------------------------------------------------------------ */
/* Seiten                                                             */
/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */
/* W148 PACK-LAUNCHER: Seite "Packs" (Zustand + Engine)                 */
/* W172 LAUNCHER-P5: Liste mit Zustand je Zeile (Schalter, Typ,          */
/* Abdeckung, Status), Ziehen + Alt+Pfeil, Texturdetail als Segment,     */
/* Grenzen je Kategorie mit Klarnamen, Detailbereich (Abdeckung,         */
/* Konflikte, Effektwerte mit Schloss, passt zum Spiel), Klartext-       */
/* Pruefung, Import per Knopf/Ziehen & Ablegen, leerer Zustand.          */
/* ------------------------------------------------------------------ */
#define PK_MAXCAP 32
#define PK_MAXOVER 512
sb_pack_order_t g_pk_ord[PK_MAXN];
int g_pk_n;
char g_pk_level[8];
static sb_pack_cap_t g_pk_caps[PK_MAXCAP];
int g_pk_ncaps;
static sb_pack_over_t g_pk_overs[PK_MAXOVER];
static int g_pk_novers;
static sb_pack_scan_t g_pk_scan[PK_MAXN];
static int g_pk_ne[PK_MAXN], g_pk_sh[PK_MAXN];
int g_pk_chk[PK_MAXN]; /* -2 = ungeprueft, sonst 0/1/2 */
static char g_pk_chk_reason[PK_MAXN][64];
static char g_pk_chk_detail[PK_MAXN][300];
static int g_pk_chk_xbe[PK_MAXN];        /* W172: mit Spielversion geprueft; W256: SB_PACK_FIT_* (>0) */
static char g_pk_chk_made[PK_MAXN][16];  /* W256: Fassung des Pakets bei FIT_OTHER */
static char g_pk_chk_act[PK_MAXN][16];   /* W256: aktive Fassung bei der Pruefung */
sb_pack_cov_t g_pk_cov[PK_MAXN];  /* W172: Abdeckung je Kategorie */
int g_pk_covby[PK_MAXN * PK_MAXN]; /* W172: [i * N + w] = verdeckt durch w */
int g_pk_fx_hit[SB_FXN], g_pk_fx_val[SB_FXN]; /* W172: Effektwerte (Gewinner) */
static char g_pk_fx_src[SB_FXN][128];
int g_pk_sel = -1;
static char g_pk_pj[1100]; /* packs.json (UTF-8, Anzeige + Dialoge) */
static int g_pk_have;      /* 1 = packs.json lesbar */
static int g_pk_drag = -1, g_pk_drop = -1; /* W172: Ziehen (Quelle, Ziel) */
static int g_pk_dragging;

/* Klarnamen der Kategorien (Reihenfolge = kSbPackCat, Glossar 7.1). */
static const char *const kPkCatKey[SB_PACK_NCAT] = {
    "gui.pkcat_hud", "gui.pkcat_cockpit", "gui.pkcat_ground",
    "gui.pkcat_buildings", "gui.pkcat_vehicles", "gui.pkcat_effects",
    "gui.pkcat_menu", "gui.pkcat_world", "gui.pkcat_other"
};
/* Klarnamen der Effektwerte (Reihenfolge = kSbFxParams). */
static const char *const kPkFxKey[SB_FXN] = {
    "gui.fxp_soft", "gui.fxp_embers", "gui.fxp_light", "gui.fxp_lightckp",
    "gui.fxp_interp", "gui.fxp_bloom", "gui.fxp_sat", "gui.fxp_temp",
    "gui.fxp_lift", "gui.fxp_gamma", "gui.fxp_gain"
};

/* UTF-8 -> ANSI fuer die sb_pack-Aufrufe (A-API, ASCII verlustfrei). */
static void pk_acp(const char *u8, char *out, size_t cap)
{
    u8_to_acp(u8, out, cap);
}

void packs_json_path(char *out, size_t cap)
{
    char dir[1024], env[1024];
    if (u8_GetEnv("SB_PACKS_DIR", env, sizeof env) > 0 && env[0]) {
        /* Test-Haken (Shots, Proben): Packs-Ordner vorgeben. */
        _snprintf_s(out, cap, _TRUNCATE, "%s/packs.json", env);
        return;
    }
    { /* W150 USER-DATA: Profil/portabel -> <Datenordner>/packs */
        char acp[1024];
        if (sb_userdata_path(SB_UD_PACKS, acp, sizeof acp) == 1) {
            u8_from_acp(acp, dir, sizeof dir);
            _snprintf_s(out, cap, _TRUNCATE, "%s/packs.json", dir);
            return;
        }
    }
    rem_pkg_detect(g_client_disp, dir, sizeof dir);
    _snprintf_s(out, cap, _TRUNCATE, "%s/packs/packs.json", dir);
}

static void packs_dir_of(char *out, size_t cap)
{
    char *s;
    strncpy_s(out, cap, g_pk_pj, _TRUNCATE);
    for (s = out; *s; s++) {
        if (*s == '\\') {
            *s = '/';
        }
    }
    s = strrchr(out, '/');
    if (s) {
        *s = '\0';
    }
}

int packs_count(void)
{
    return g_pk_n;
}

/* Pruefstand gewinnt ueber den Schnellstatus (2 gruen, 1 gelb, 0 rot). */
int packs_status_of(int i)
{
    if (i < 0 || i >= g_pk_n) {
        return -1;
    }
    return g_pk_chk[i] >= -1 ? g_pk_chk[i] : g_pk_scan[i].status;
}

int packs_enabled_of(int i)
{
    return i >= 0 && i < g_pk_n && g_pk_ord[i].enabled;
}

int packs_is_fx(int i)
{
    return i >= 0 && i < g_pk_n && strcmp(g_pk_scan[i].type, "effect") == 0;
}

static int packs_level_num(void)
{
    return strcmp(g_pk_level, "orig") == 0 ? 0
           : strcmp(g_pk_level, "x2") == 0 ? 2 : 4;
}

static const char *packs_cap_str(int cap)
{
    if (cap <= 0) {
        return LT("gui.pk_orig");
    }
    return cap <= 2 ? LT("gui.pk_x2") : LT("gui.pk_x4");
}

const char *packs_level_word(void)
{
    return packs_cap_str(packs_level_num());
}

/* Gespeicherte Grenze einer Kategorie (-1 = keine). "effects" + "effect"
 * sind eine Kategorie (Kunden-Grenze gilt nur exakt je Name, s. Speichern). */
int packs_cap_stored(int c)
{
    int i;
    for (i = 0; i < g_pk_ncaps; i++) {
        if (strcmp(g_pk_caps[i].name, kSbPackCat[c]) == 0 ||
            (c == 5 && strcmp(g_pk_caps[i].name, "effect") == 0)) {
            return g_pk_caps[i].cap;
        }
    }
    return -1;
}

/* Wirksame Grenze je Kategorie (nie ueber dem Texturdetail). */
int packs_cap_of(int c)
{
    int lv = packs_level_num(), cap = packs_cap_stored(c);
    return (cap >= 0 && cap < lv) ? cap : lv;
}

/* Grenze setzen (v >= Texturdetail = keine Grenze). "effects" schreibt
 * auch "effect" (Manifeste nutzen beide Namen, der Client vergleicht exakt). */
static void packs_cap_name(const char *name, int v)
{
    int i, k;
    for (i = 0; i < g_pk_ncaps; i++) {
        if (strcmp(g_pk_caps[i].name, name) == 0) {
            if (v < 0) {
                for (k = i; k + 1 < g_pk_ncaps; k++) {
                    g_pk_caps[k] = g_pk_caps[k + 1];
                }
                g_pk_ncaps--;
            } else {
                g_pk_caps[i].cap = v;
            }
            return;
        }
    }
    if (v >= 0 && g_pk_ncaps < PK_MAXCAP) {
        strncpy_s(g_pk_caps[g_pk_ncaps].name, sizeof g_pk_caps[g_pk_ncaps].name,
                  name, _TRUNCATE);
        g_pk_caps[g_pk_ncaps].cap = v;
        g_pk_ncaps++;
    }
}

void packs_cap_set(int c, int v)
{
    int lim = v >= packs_level_num() ? -1 : v;
    packs_cap_name(kSbPackCat[c], lim);
    if (c == 5) {
        packs_cap_name("effect", lim);
    }
}

/* Pruefcode -> Klartext (Konzept 6.6); Code bleibt fuer die Diagnose. */
static const char *packs_reason_text(const char *code)
{
    static const struct {
        const char *code;
        const char *key;
    } kMap[] = {
        { "game_mismatch", "gui.pkv_game" },
        { "bad_variant_path", "gui.pkv_files" }, { "deny_suffix", "gui.pkv_files" },
        { "zip_encrypted", "gui.pkv_files" },
        { "file_missing", "gui.pkv_missing" }, { "file_unreadable", "gui.pkv_missing" },
        { "entries_file_missing", "gui.pkv_missing" },
        { "missing_manifest", "gui.pkv_missing" },
        { "pkg_unreadable", "gui.pkv_unreadable" },
        { "aspect_mismatch", "gui.pkv_shape" },
        { "sha_mismatch", "gui.pkv_sha" },
        { "dds_bad_header", "gui.pkv_dds" }, { "fmt_mismatch", "gui.pkv_dds" },
        { "size_mismatch", "gui.pkv_dds" }, { "dds_not_stored", "gui.pkv_dds" },
        { "dup_hash", "gui.pkv_dup" },
        { "size_not_factor", "gui.pkv_minor" }, { "mips_mismatch", "gui.pkv_minor" },
        { "unknown_category", "gui.pkv_minor" },
        { "unknown_param", "gui.pkv_fx" }, { "params_ignored", "gui.pkv_fx" },
        { "param_range", "gui.pkv_fx" }, { "params_bad_value", "gui.pkv_fx" },
        { "params_not_int", "gui.pkv_fx" },
    };
    int i;
    if (!code || !code[0]) {
        return "";
    }
    for (i = 0; i < (int)(sizeof kMap / sizeof kMap[0]); i++) {
        if (strcmp(code, kMap[i].code) == 0) {
            return LT(kMap[i].key);
        }
    }
    return LT("gui.pkv_manifest"); /* uebrige V1-Codes: Beschreibung ungueltig */
}

/* Statuswort, Farbe und Rohgrund eines Eintrags. */
static void packs_state(int i, const char **word, const char **reason, COLORREF *col)
{
    int s = packs_status_of(i);
    COLORREF c;
    if (!g_pk_ord[i].enabled) {
        *word = LT("gui.pk_off");
        c = C_FAINT;
    } else if (s == 2) {
        *word = LT("gui.pk_ok");
        c = C_OK;
    } else if (s == 1) {
        *word = LT("gui.pk_warn");
        c = C_WARN;
    } else if (s == 0) {
        *word = LT("gui.pk_err");
        c = C_ERR;
    } else {
        *word = LT("gui.pk_unchecked");
        c = C_DIM;
    }
    if (col) {
        *col = c;
    }
    *reason = g_pk_chk[i] >= -1 ? g_pk_chk_reason[i] : g_pk_scan[i].reason;
}

/* Pruefstand ueber das Neuladen retten (per Quelle zugeordnet). */
typedef struct {
    char file[600];
    int chk, xbe;
    char reason[64], detail[300];
    char made[16], act[16]; /* W256 */
} pk_stash_t;

void packs_load(void)
{
    static pk_stash_t old[PK_MAXN];
    int nold, i;
    char pj[1100];
    nold = g_pk_n;
    for (i = 0; i < nold && i < PK_MAXN; i++) {
        strcpy_s(old[i].file, sizeof old[i].file, g_pk_ord[i].file);
        old[i].chk = g_pk_chk[i];
        old[i].xbe = g_pk_chk_xbe[i];
        strcpy_s(old[i].made, sizeof old[i].made, g_pk_chk_made[i]);
        strcpy_s(old[i].act, sizeof old[i].act, g_pk_chk_act[i]);
        strcpy_s(old[i].reason, sizeof old[i].reason, g_pk_chk_reason[i]);
        strcpy_s(old[i].detail, sizeof old[i].detail, g_pk_chk_detail[i]);
    }
    packs_json_path(g_pk_pj, sizeof g_pk_pj);
    pk_acp(g_pk_pj, pj, sizeof pj);
    memset(g_pk_caps, 0, sizeof g_pk_caps);
    memset(g_pk_overs, 0, sizeof g_pk_overs);
    g_pk_n = sb_pack_read_cfg(pj, g_pk_ord, PK_MAXN, g_pk_level,
                              sizeof g_pk_level, g_pk_caps, PK_MAXCAP,
                              g_pk_overs, PK_MAXOVER);
    if (g_pk_n < 0) {
        g_pk_n = 0;
        g_pk_have = 0;
        strcpy_s(g_pk_level, sizeof g_pk_level, "x4");
    } else {
        g_pk_have = 1;
        if (g_pk_n > PK_MAXN) {
            g_pk_n = PK_MAXN;
        }
    }
    g_pk_ncaps = 0;
    for (i = 0; i < PK_MAXCAP && g_pk_caps[i].name[0]; i++) {
        g_pk_ncaps++;
    }
    g_pk_novers = 0;
    for (i = 0; i < PK_MAXOVER && g_pk_overs[i].hash[0]; i++) {
        g_pk_novers++;
    }
    for (i = 0; i < g_pk_n; i++) {
        int k;
        g_pk_chk[i] = -2;
        g_pk_chk_xbe[i] = 0;
        g_pk_chk_made[i][0] = g_pk_chk_act[i][0] = '\0';
        g_pk_chk_reason[i][0] = '\0';
        g_pk_chk_detail[i][0] = '\0';
        for (k = 0; k < nold; k++) { /* Pruefstand je Quelle retten */
            if (strcmp(g_pk_ord[i].file, old[k].file) == 0) {
                g_pk_chk[i] = old[k].chk;
                g_pk_chk_xbe[i] = old[k].xbe;
                strcpy_s(g_pk_chk_made[i], 16, old[k].made);
                strcpy_s(g_pk_chk_act[i], 16, old[k].act);
                strcpy_s(g_pk_chk_reason[i], 64, old[k].reason);
                strcpy_s(g_pk_chk_detail[i], 300, old[k].detail);
                break;
            }
        }
    }
    memset(g_pk_scan, 0, sizeof g_pk_scan);
    sb_pack_scan(pj, g_pk_scan, g_pk_n);
    sb_pack_shadow(pj, g_pk_ne, g_pk_sh, g_pk_n);
    /* W172: Abdeckung je Kategorie + "verdeckt durch" (gleiche Aufloesung
     * wie der Lader) und die gewinnenden Effektwerte. */
    memset(g_pk_cov, 0, sizeof g_pk_cov);
    if (sb_pack_coverage(pj, g_pk_cov, g_pk_covby, PK_MAXN) < 0) {
        memset(g_pk_covby, 0, sizeof g_pk_covby);
    }
    memset(g_pk_fx_hit, 0, sizeof g_pk_fx_hit);
    memset(g_pk_fx_src, 0, sizeof g_pk_fx_src);
    if (g_pk_n > 0) {
        sb_pack_fxparams(pj, g_pk_fx_val, g_pk_fx_hit, g_pk_fx_src);
    }
    if (g_pk_sel >= g_pk_n) {
        g_pk_sel = g_pk_n - 1;
    }
    if (g_pk_sel < 0 && g_pk_n > 0) {
        g_pk_sel = 0;
    }
}

void packs_save(void)
{
    char dir[1100], pj[1100];
    packs_dir_of(dir, sizeof dir);
    if (dir[0]) {
        lcfg_mkdir_p(dir);
    }
    pk_acp(g_pk_pj, pj, sizeof pj);
    if (sb_pack_write_cfg(pj, g_pk_ord, g_pk_n, g_pk_level, g_pk_caps,
                          g_pk_ncaps, g_pk_overs, g_pk_novers)) {
        g_pk_have = 1;
    }
}

/* Varianten eines geladenen Packs: Bit 1 = 2x, Bit 2 = 4x. */
static int packs_variants(int i)
{
    int c, m = 0;
    for (c = 0; c < SB_PACK_NCAT; c++) {
        if (g_pk_cov[i].cat2[c] > 0) {
            m |= 1;
        }
        if (g_pk_cov[i].cat4[c] > 0) {
            m |= 2;
        }
    }
    return m;
}

/* Effekt-Pack setzt Farben (color_*, bloom) oder nur Effekte? */
static int packs_fx_colors(int i)
{
    return strstr(g_pk_scan[i].paramsum, "color_") != NULL ||
           strstr(g_pk_scan[i].paramsum, "bloom_") != NULL;
}

/* Zweite Zeile einer Listenzeile: Version, Abdeckung, verdeckt. */
static void packs_row2(int i, char *out, size_t cap)
{
    int nt = g_pk_ne[i] >= 0 ? g_pk_ne[i] : g_pk_scan[i].nentries;
    size_t n;
    if (nt < 0) {
        nt = 0;
    }
    _snprintf_s(out, cap, _TRUNCATE, "%s", g_pk_scan[i].version);
    n = strlen(out);
    if (packs_is_fx(i)) {
        _snprintf_s(out + n, cap - n, _TRUNCATE, " \xC2\xB7 %s",
                    LT(packs_fx_colors(i) ? "gui.pk_cov_colfx" : "gui.pk_cov_fx"));
        n = strlen(out);
        if (nt > 0) {
            _snprintf_s(out + n, cap - n, _TRUNCATE, LT("gui.pk_cov_plustex"), nt);
        }
        return;
    }
    _snprintf_s(out + n, cap - n, _TRUNCATE, LT("gui.pk_cov_tex"), nt);
    n = strlen(out);
    {
        int v = g_pk_ord[i].enabled ? packs_variants(i) : 0;
        if (v) {
            _snprintf_s(out + n, cap - n, _TRUNCATE, " \xC2\xB7 %s",
                        v == 3 ? "2\xC3\x97/4\xC3\x97" : v == 2 ? "4\xC3\x97" : "2\xC3\x97");
            n = strlen(out);
        }
    }
    if (g_pk_sh[i] > 0) {
        _snprintf_s(out + n, cap - n, _TRUNCATE, LT("gui.pk_cov_covered"), g_pk_sh[i]);
    }
}

/* Listenzeile (eigene Zeichnung): Schalter, Typ T/E, Name + Zeile 2,
 * Status rechts. Ziehen: Ziel-Linie ueber der Zeile g_pk_drop. */
void packs_draw_row(const DRAWITEMSTRUCT *d)
{
    canvas_t c;
    int i = (int)d->itemID, w = d->rcItem.right - d->rcItem.left,
        h = d->rcItem.bottom - d->rcItem.top;
    int sel = (d->itemState & ODS_SELECTED) != 0;
    int focus = (d->itemState & ODS_FOCUS) != 0;
    const char *word, *reason;
    COLORREF scol;
    char row2[300];
    memset(&c, 0, sizeof c);
    if (i < 0 || i >= g_pk_n || w <= 0 || h <= 0 || !cv_make(&c, w, h)) {
        return;
    }
    GdiFlush();
    cv_fill(&c, 0, 0, w, h, C_INPUT, 255, 0);
    if (sel) {
        cv_fill(&c, 0, 0, w, h, RGB(255, 255, 255), 18, SX(5));
        cv_fill(&c, 0, SX(6), SX(3), h - SX(12), C_ACCENT, 255, SX(1));
    }
    packs_state(i, &word, &reason, &scol);
    { /* Schalter (wie K_TOGGLE): abgelehnt + aus = blass */
        int tw = SX(30), th = SX(16), ty = (h - th) / 2, x0 = SX(10);
        int on = g_pk_ord[i].enabled, rej = packs_status_of(i) == 0;
        COLORREF track = on ? C_ACCENT : RGB(70, 77, 90);
        int kd = th - SX(6), kx = on ? x0 + tw - SX(3) - kd : x0 + SX(3);
        cv_fill(&c, x0, ty, tw, th, track, rej && !on ? 110 : 255, th / 2);
        cv_fill(&c, kx, ty + SX(3), kd, kd, on ? C_ON_ACCENT : RGB(230, 233, 238),
                rej && !on ? 140 : 255, kd / 2);
    }
    { /* Typ-Symbol T/E */
        int bx = SX(48), by = (h - SX(18)) / 2, fx = packs_is_fx(i);
        cv_fill(&c, bx, by, SX(18), SX(18), fx ? RGB(120, 92, 200) : RGB(44, 92, 128),
                255, SX(3));
        cv_text(&c, g_f[F_TINY], fx ? "E" : "T", bx, by, SX(18), SX(18), C_TEXT,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    {
        int nx = SX(74), sw = SX(92), nw = w - nx - sw - SX(6);
        cv_text(&c, g_f[F_SMALL], g_pk_scan[i].name, nx, SX(3), nw, SX(18),
                g_pk_ord[i].enabled ? C_TEXT : C_DIM,
                DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        packs_row2(i, row2, sizeof row2);
        cv_text(&c, g_f[F_TINY], row2, nx, SX(20), nw, SX(15), C_DIM,
                DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        cv_fill(&c, w - sw, (h - SX(8)) / 2, SX(8), SX(8), scol, 255, SX(4));
        cv_text(&c, g_f[F_TINY], word, w - sw + SX(12), 0, sw - SX(14), h, scol,
                DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }
    if (g_pk_dragging && g_pk_drop >= 0) { /* Ziel-Linie */
        if (g_pk_drop == i) {
            cv_fill(&c, 0, 0, w, SX(2), C_ACCENT_HI, 255, 0);
        } else if (g_pk_drop == g_pk_n && i == g_pk_n - 1) {
            cv_fill(&c, 0, h - SX(2), w, SX(2), C_ACCENT_HI, 255, 0);
        }
    }
    if (focus && !(d->itemState & ODS_NOFOCUSRECT)) {
        cv_ring(&c, 0, 0, w, h, C_ACCENT, 160, SX(5), 1);
    }
    cv_fill(&c, SX(8), h - 1, w - SX(16), 1, RGB(255, 255, 255), 12, 0);
    GdiFlush();
    BitBlt(d->hDC, d->rcItem.left, d->rcItem.top, w, h, c.dc, 0, 0, SRCCOPY);
    cv_free(&c);
}

static void packs_refresh_lists(void)
{
    HWND lb = ctl(ID_PK_LIST);
    int i, c, lv = packs_level_num();
    g_updating++;
    if (lb) {
        SendMessageW(lb, WM_SETREDRAW, FALSE, 0);
        ListBox_ResetContent(lb);
        for (i = 0; i < g_pk_n; i++) {
            SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)i); /* Daten = Index */
        }
        ListBox_SetCurSel(lb, g_pk_sel);
        SendMessageW(lb, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(lb, NULL, FALSE);
    }
    /* Segment Texturdetail (alte Stufe "orig": keines gewaehlt) */
    ui_set_checked(ID_PK_LVL2, lv == 2);
    ui_set_checked(ID_PK_LVL4, lv == 4);
    /* Grenzen je Kategorie: nur Werte <= Texturdetail in der Liste */
    for (c = 0; c < SB_PACK_NCAT; c++) {
        HWND cb = ctl(ID_PK_CAT0 + c);
        int cap = packs_cap_of(c);
        if (!cb) {
            continue;
        }
        ComboBox_ResetContent(cb);
        combo_add(cb, LT("gui.pk_orig"));
        if (lv >= 2) {
            combo_add(cb, LT("gui.pk_x2"));
        }
        if (lv >= 4) {
            combo_add(cb, LT("gui.pk_x4"));
        }
        ComboBox_SetCurSel(cb, cap <= 0 ? 0 : cap <= 2 ? 1 : 2);
    }
    g_updating--;
}

void packs_refresh(int load)
{
    if (load) {
        packs_load();
    }
    packs_refresh_lists();
    ui_apply_rules(); /* W171: R17-R22/R29-R32 haengen am Packs-Zustand */
    redraw_all();
}

/* Auswahl (Liste) -> g_pk_sel. */
int packs_sel(void)
{
    HWND lb = ctl(ID_PK_LIST);
    int sel = lb ? (int)SendMessageW(lb, LB_GETCURSEL, 0, 0) : -1;
    if (sel >= 0 && sel < g_pk_n) {
        g_pk_sel = sel;
    } else if (g_pk_n == 0) {
        g_pk_sel = -1;
    }
    return g_pk_sel;
}

/* id eines Pakets per Scan bestimmen (nach dem Hinzufuegen). */
static void packs_fix_id(int idx)
{
    char pj[1100];
    static sb_pack_scan_t s[PK_MAXN];
    int n, i;
    pk_acp(g_pk_pj, pj, sizeof pj);
    n = sb_pack_scan(pj, s, PK_MAXN);
    for (i = 0; i < n && i < PK_MAXN; i++) {
        if (strcmp(s[i].src, g_pk_ord[idx].file) == 0 && s[i].id[0]) {
            strncpy_s(g_pk_ord[idx].id, sizeof g_pk_ord[idx].id, s[i].id,
                      _TRUNCATE);
            return;
        }
    }
}

static const char *packs_xbe(char *hex, size_t cap);
static void packs_check_idx(int i, int quiet);

/* W172: Pack hinzufuegen (Dialog, Ziehen & Ablegen, Selbsttest).
 * Datei (.sbpack) wird in den Packs-Ordner kopiert, Ordner verknuepft.
 * Danach sofort pruefen (Konzept 6.6). 0 = ok. */
int packs_add_path(const char *src_u8, int is_dir)
{
    char u8[2048], dir[1100], base[600], dest[1400], *s;
    int i;
    if (g_pk_n >= PK_MAXN) {
        set_status_k(LT("gui.rule_r31"), 2);
        return 1;
    }
    strncpy_s(u8, sizeof u8, src_u8, _TRUNCATE);
    for (s = u8; *s; s++) {
        if (*s == '\\') {
            *s = '/';
        }
    }
    s = strrchr(u8, '/');
    strncpy_s(base, sizeof base, s ? s + 1 : u8, _TRUNCATE);
    for (i = 0; i < g_pk_n; i++) {
        if (_stricmp(g_pk_ord[i].file, is_dir ? u8 : base) == 0) {
            g_pk_sel = i;
            set_status_k(LT("gui.pk_dup"), 2);
            packs_refresh(0);
            return 1;
        }
    }
    if (!is_dir) {
        wchar_t wsrc[MAX_PATH * 2], wdst[MAX_PATH * 2];
        packs_dir_of(dir, sizeof dir);
        if (dir[0]) {
            lcfg_mkdir_p(dir);
        }
        _snprintf_s(dest, sizeof dest, _TRUNCATE, "%s/%s", dir, base);
        u8_to_w(u8, wsrc, MAX_PATH * 2);
        u8_to_w(dest, wdst, MAX_PATH * 2);
        if (!path_eq(u8, dest) && !CopyFileW(wsrc, wdst, FALSE)) {
            set_status_k(LT("gui.pk_nocopy"), 2);
            return 1;
        }
        strncpy_s(g_pk_ord[g_pk_n].file, sizeof g_pk_ord[g_pk_n].file, base,
                  _TRUNCATE);
    } else {
        strncpy_s(g_pk_ord[g_pk_n].file, sizeof g_pk_ord[g_pk_n].file, u8,
                  _TRUNCATE);
    }
    strcpy_s(g_pk_ord[g_pk_n].id, sizeof g_pk_ord[g_pk_n].id, "?");
    g_pk_ord[g_pk_n].enabled = 1;
    g_pk_n++;
    packs_save();
    packs_fix_id(g_pk_n - 1);
    packs_save();
    g_pk_sel = g_pk_n - 1;
    packs_load();
    packs_check_idx(g_pk_sel, 1); /* sofort pruefen, Ergebnis in der Liste */
    {
        const char *w, *r;
        char t[400];
        packs_state(g_pk_sel, &w, &r, NULL);
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.pk_added"),
                    g_pk_scan[g_pk_sel].name, w);
        set_status_k(t, packs_status_of(g_pk_sel) >= 1 ? 1 : 2);
    }
    packs_refresh(0);
    return 0;
}

static void packs_add_file(void)
{
    OPENFILENAMEW ofn;
    static wchar_t file[MAX_PATH * 2];
    wchar_t filter[256], f1[128], f2[64], title[128], *w;
    size_t n1, n2;
    char u8[2048];
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g_hwnd;
    file[0] = 0;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH * 2;
    u8_to_w(LT("gui.pk_dlg_filter"), f1, 128);
    u8_to_w(LT("gui.dlg_allfiles"), f2, 64);
    u8_to_w(LT("gui.pk_dlg_file"), title, 128);
    w = filter;
    n1 = wcslen(f1) + 1;
    n2 = wcslen(f2) + 1;
    if (n1 + 10 + n2 + 4 < 256) {
        wcscpy_s(w, 256, f1);
        w += n1;
        wcscpy_s(w, 256 - n1, L"*.sbpack");
        w += 9;
        wcscpy_s(w, 256 - n1 - 9, f2);
        w += n2;
        wcscpy_s(w, 256 - n1 - 9 - n2, L"*.*");
        w[4] = 0;
    } else {
        wcscpy_s(filter, 256, L"*.sbpack");
    }
    ofn.lpstrFilter = filter;
    ofn.lpstrTitle = title;
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) {
        return;
    }
    w_to_u8(file, u8, sizeof u8);
    packs_add_path(u8, 0);
}

static void packs_add_dir(void)
{
    BROWSEINFOW bi;
    LPITEMIDLIST pidl;
    static wchar_t dir[MAX_PATH];
    char u8[2048];
    memset(&bi, 0, sizeof bi);
    bi.hwndOwner = g_hwnd;
    {
        static wchar_t title[160];
        u8_to_w(LT("gui.pk_dlg_dir"), title, 160);
        bi.lpszTitle = title;
    }
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    pidl = SHBrowseForFolderW(&bi);
    if (!pidl) {
        return;
    }
    dir[0] = 0;
    if (SHGetPathFromIDListW(pidl, dir)) {
        w_to_u8(dir, u8, sizeof u8);
        packs_add_path(u8, 1);
    }
    CoTaskMemFree(pidl);
}

void packs_add_menu(void)
{
    HMENU m = CreatePopupMenu();
    RECT r;
    wchar_t w1[128], w2[128];
    int cmd;
    if (!m) {
        return;
    }
    u8_to_w(LT("gui.pk_add_file"), w1, 128);
    u8_to_w(LT("gui.pk_add_dir"), w2, 128);
    AppendMenuW(m, MF_STRING, 1, w1);
    AppendMenuW(m, MF_STRING, 2, w2);
    GetWindowRect(ctl(ID_PK_ADD), &r);
    cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                              r.left, r.bottom, 0, g_hwnd, NULL);
    DestroyMenu(m);
    if (cmd == 1) {
        packs_add_file();
    } else if (cmd == 2) {
        packs_add_dir();
    }
}

/* W172: Ziehen & Ablegen aus dem Explorer (.sbpack-Datei oder Pack-Ordner
 * mit manifest.json). */
void packs_drop(HDROP hd)
{
    UINT n = DragQueryFileW(hd, 0xFFFFFFFF, NULL, 0), k;
    int added = 0, bad = 0;
    for (k = 0; k < n; k++) {
        wchar_t w[MAX_PATH * 2];
        char u8[2048], mf[2100];
        size_t L;
        if (!DragQueryFileW(hd, k, w, MAX_PATH * 2)) {
            continue;
        }
        w_to_u8(w, u8, sizeof u8);
        L = strlen(u8);
        _snprintf_s(mf, sizeof mf, _TRUNCATE, "%s\\manifest.json", u8);
        if (is_dir_u8(u8) && u8_GetFileAttributes(mf) != INVALID_FILE_ATTRIBUTES) {
            added += packs_add_path(u8, 1) == 0;
        } else if (L > 7 && _stricmp(u8 + L - 7, ".sbpack") == 0) {
            added += packs_add_path(u8, 0) == 0;
        } else {
            bad++;
        }
    }
    DragFinish(hd);
    if (g_page != P_PACKS) {
        show_page(P_PACKS);
    }
    if (bad && !added) {
        set_status_k(LT("gui.pk_drop_bad"), 2);
    }
}

/* W172: Entfernen mit Rueckfrage. Datei-Pack: kopierte Datei loeschen;
 * Ordner-Pack: nur die Verknuepfung, Dateien bleiben (Konzept 6.5). */
void packs_del(void)
{
    int i = packs_sel(), k, is_file;
    char q[600], full[1400], dir[1100];
    if (i < 0 || i >= g_pk_n) {
        set_status(LT("gui.pk_nosel"));
        return;
    }
    packs_dir_of(dir, sizeof dir);
    is_file = !((strlen(g_pk_ord[i].file) > 1 && g_pk_ord[i].file[1] == ':') ||
                g_pk_ord[i].file[0] == '/' || g_pk_ord[i].file[0] == '\\');
    _snprintf_s(full, sizeof full, _TRUNCATE, "%s/%s", dir, g_pk_ord[i].file);
    if (is_file && is_dir_u8(full)) {
        is_file = 0; /* Ordner im Packs-Ordner: nicht loeschen */
    }
    _snprintf_s(q, sizeof q, _TRUNCATE,
                LT(is_file ? "gui.pk_ask_delfile" : "gui.pk_ask_unlink"),
                g_pk_scan[i].name);
    if (!ui_confirm(q, LT("gui.pk_del"))) {
        return;
    }
    if (is_file) {
        u8_DeleteFile(full);
    }
    for (k = i; k + 1 < g_pk_n; k++) {
        g_pk_ord[k] = g_pk_ord[k + 1];
        g_pk_chk[k] = g_pk_chk[k + 1];
        g_pk_chk_xbe[k] = g_pk_chk_xbe[k + 1];
        strcpy_s(g_pk_chk_made[k], 16, g_pk_chk_made[k + 1]);
        strcpy_s(g_pk_chk_act[k], 16, g_pk_chk_act[k + 1]);
        strcpy_s(g_pk_chk_reason[k], 64, g_pk_chk_reason[k + 1]);
        strcpy_s(g_pk_chk_detail[k], 300, g_pk_chk_detail[k + 1]);
    }
    g_pk_n--;
    packs_save();
    packs_refresh(1);
}

/* W172: Eintrag from an Stelle to (0..n, to = vor diesem Eintrag). */
void packs_move_to(int from, int to)
{
    sb_pack_order_t t;
    int k;
    if (from < 0 || from >= g_pk_n || to < 0 || to > g_pk_n) {
        return;
    }
    if (to > from) {
        to--; /* Einfuegestelle nach dem Herausnehmen */
    }
    if (to == from) {
        return;
    }
    t = g_pk_ord[from];
    if (to < from) {
        for (k = from; k > to; k--) {
            g_pk_ord[k] = g_pk_ord[k - 1];
        }
    } else {
        for (k = from; k < to; k++) {
            g_pk_ord[k] = g_pk_ord[k + 1];
        }
    }
    g_pk_ord[to] = t;
    g_pk_sel = to;
    packs_save();
    packs_refresh(1); /* Konflikte sofort neu (Konzept 6.3) */
    {
        char m[300];
        _snprintf_s(m, sizeof m, _TRUNCATE, LT("gui.pk_moved"),
                    g_pk_scan[to].name, to + 1, g_pk_n);
        set_status(m);
    }
}

void packs_move(int dir)
{
    int i = packs_sel();
    if (i < 0 || i >= g_pk_n) {
        set_status(LT("gui.pk_nosel"));
        return;
    }
    if (i + dir < 0 || i + dir >= g_pk_n) {
        return;
    }
    packs_move_to(i, dir < 0 ? i - 1 : i + 2);
}

/* An/aus (R21: abgelehnt laesst sich nicht einschalten, aus geht immer). */
void packs_toggle_idx(int i)
{
    if (i < 0 || i >= g_pk_n) {
        set_status(LT("gui.pk_nosel"));
        return;
    }
    if (!g_pk_ord[i].enabled && packs_status_of(i) == 0) {
        char t[400];
        const char *w, *r;
        packs_state(i, &w, &r, NULL);
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.rule_r21"), packs_reason_text(r));
        set_status_k(t, 2);
        return;
    }
    g_pk_sel = i;
    g_pk_ord[i].enabled = !g_pk_ord[i].enabled;
    packs_save();
    packs_refresh(1);
}

void packs_toggle(void)
{
    packs_toggle_idx(packs_sel());
}

/* XBE-Hash fuer V2 (NULL = unbekannt, V2 entfaellt). */
static const char *packs_xbe(char *hex, size_t cap)
{
    launcher_settings_t s;
    uint8_t dg[32];
    char xp[1024], full[1200], eff[1024];
    s = g_s;
    ui_to_settings(&s);
    if (s.game_image[0]) { /* W150 ISO-DIRECT: default.xbe aus dem Abbild */
        return lcfg_image_xbe_sha256(s.game_image, hex) ? hex : NULL;
    }
    lcfg_xbe_effective(&s, eff, sizeof eff); /* W172: auch abgeleitet */
    if (!eff[0]) {
        return NULL;
    }
    if ((strlen(eff) > 1 && eff[1] == ':') || eff[0] == '/' || eff[0] == '\\') {
        strncpy_s(full, sizeof full, eff, _TRUNCATE);
    } else {
        _snprintf_s(full, sizeof full, _TRUNCATE, "%s/%s", g_client_dir, eff);
    }
    pk_acp(full, xp, sizeof xp);
    if (!sb_sha256_file(xp, dg)) {
        return NULL;
    }
    sb_sha256_hex(dg, hex);
    (void)cap;
    return hex;
}

/* Vollpruefung V1-V14 eines Eintrags (quiet = keine Statuszeile). */
static void packs_check_idx(int i, int quiet)
{
    char full[1400], afull[1400], dir[1100];
    char xhex[65], reason[64], detail[300];
    const char *xh;
    int st, fit = SB_PACK_FIT_UNKNOWN;
    if (i < 0 || i >= g_pk_n) {
        if (!quiet) {
            set_status(LT("gui.pk_nosel"));
        }
        return;
    }
    packs_dir_of(dir, sizeof dir);
    if ((strlen(g_pk_ord[i].file) > 1 && g_pk_ord[i].file[1] == ':') ||
        g_pk_ord[i].file[0] == '/' || g_pk_ord[i].file[0] == '\\') {
        strncpy_s(full, sizeof full, g_pk_ord[i].file, _TRUNCATE);
    } else {
        _snprintf_s(full, sizeof full, _TRUNCATE, "%s/%s", dir,
                    g_pk_ord[i].file);
    }
    pk_acp(full, afull, sizeof afull);
    xh = packs_xbe(xhex, sizeof xhex);
    reason[0] = detail[0] = '\0';
    st = sb_pack_check_fit(afull, xh, reason, sizeof reason, detail,
                           sizeof detail, &fit, g_pk_chk_made[i],
                           sizeof g_pk_chk_made[i]);
    g_pk_chk[i] = st;
    g_pk_chk_xbe[i] = xh != NULL && fit > 0 ? fit : 0; /* W256: Passungsart */
    {
        const char *an = xh ? sb_gamever_family_name(sb_gamever_family_idx(xh)) : NULL;
        strcpy_s(g_pk_chk_act[i], sizeof g_pk_chk_act[i], an ? an : "");
    }
    strcpy_s(g_pk_chk_reason[i], sizeof g_pk_chk_reason[i], reason);
    strcpy_s(g_pk_chk_detail[i], sizeof g_pk_chk_detail[i], detail);
    if (!quiet) {
        char t[400];
        const char *w, *r;
        packs_state(i, &w, &r, NULL);
        if (reason[0]) {
            _snprintf_s(t, sizeof t, _TRUNCATE, "%s: %s \xE2\x80\x93 %s",
                        g_pk_scan[i].name, w, packs_reason_text(reason));
        } else {
            _snprintf_s(t, sizeof t, _TRUNCATE, "%s: %s", g_pk_scan[i].name, w);
        }
        if (!xh) {
            size_t n = strlen(t);
            _snprintf_s(t + n, sizeof t - n, _TRUNCATE, " \xC2\xB7 %s", LT("gui.rule_r22"));
        }
        set_status_k(t, st == 0 ? 2 : 1);
    }
}

void packs_check(void)
{
    packs_check_idx(packs_sel(), 0);
    packs_refresh_lists();
    ui_apply_rules();
    redraw_all();
}

/* W172: alle Packs pruefen (Konzept 6.6 "Check all"). */
void packs_check_all(void)
{
    int i, ok = 0, warn = 0, bad = 0;
    char t[300];
    for (i = 0; i < g_pk_n; i++) {
        int st;
        packs_check_idx(i, 1);
        st = packs_status_of(i);
        ok += st == 2;
        warn += st == 1;
        bad += st == 0;
    }
    _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.pk_checked_all"), ok, warn, bad);
    set_status_k(t, bad ? 2 : 1);
    packs_refresh_lists();
    ui_apply_rules();
    redraw_all();
}

void packs_folder(void)
{
    char dir[1100];
    wchar_t w[1100];
    packs_dir_of(dir, sizeof dir);
    if (dir[0]) {
        lcfg_mkdir_p(dir);
    }
    if (g_o.shot != 0) { /* Test/Shot: nie ein Explorer-Fenster */
        printf("[open] %s\n", dir);
        return;
    }
    u8_to_w(dir[0] ? dir : ".", w, 1100);
    ShellExecuteW(g_hwnd, L"open", w, NULL, NULL, SW_SHOWNORMAL);
}

/* W172: Pack Creator starten (nur wenn neben dem Launcher, PO-Entsch. 6). */
#if !SB_EDITION_COMMUNITY
void packs_creator_open(void)
{
    char p[1200];
    wchar_t w[1200];
    if (!creator_path(p, sizeof p)) {
        set_status_k(LT("gui.rule_r32"), 2);
        return;
    }
    if (g_o.shot != 0) {
        printf("[open] %s\n", p);
        return;
    }
    u8_to_w(p, w, 1200);
    ShellExecuteW(g_hwnd, L"open", w, NULL, NULL, SW_SHOWNORMAL);
}

#endif
/* W172: Texturdetail 2x/4x (einzige Stelle, Konzept 6.4). Wirkt im Client
 * ueber hd_scale/hd_textures (wie bisher gespiegelt); Grenzen ueber dem
 * neuen Detail entfallen (nur senken). */
void packs_set_level(int lv)
{
    if (lv != 2 && lv != 4) {
        return;
    }
    strcpy_s(g_pk_level, sizeof g_pk_level, lv == 2 ? "x2" : "x4");
    {
        int c;
        for (c = 0; c < SB_PACK_NCAT; c++) {
            int cap = packs_cap_stored(c);
            if (cap >= lv) {
                packs_cap_set(c, lv); /* entfernt die Grenze */
            }
        }
    }
    packs_save();
    /* W173: nur die Stufe (hd_scale); HD-Texturen an/aus gehoert dem
     * Grafik-Preset (Konzept 8.2: hd_textures = Preset != Original). */
    g_updating++;
    ComboBox_SetCurSel(ctl(ID_REM_SCALE), lv == 2 ? 0 : 1);
    g_updating--;
    ui_to_settings(&g_s);
    update_summary();
    packs_refresh(0);
}

void packs_cap_reset(void)
{
    g_pk_ncaps = 0;
    packs_save();
    packs_refresh(0);
    set_status(LT("gui.pk_caps_reset"));
}

/* W173 LAUNCHER-P4: Preset-Schluessel, die aktive Effect Packs setzen
 * (Effect-Pack-Ausnahme, Konzept 3.3; Selbsttest: feste Umgebung). */
lgp_mask_t gfx_lock_now(void)
{
    lgp_mask_t m = 0;
    int k;
    if (g_env_fix) {
        m = g_env_fix->gfx_lock;
    } else {
        for (k = 0; k < SB_FXN; k++) {
            if (g_pk_fx_hit[k]) {
                lcfg_gfx_lock_key(&m, kSbFxParams[k].key);
            }
        }
    }
    if (pergame_box_169()) { /* W414: LoC 16:9 - Raender gesperrt (W402), zaehlen nicht fuers Preset und bleiben beim Anwenden */
        lcfg_gfx_lock_key(&m, "edge_mode");
        lcfg_gfx_lock_key(&m, "edge_fade_width");
        lcfg_gfx_lock_key(&m, "edge_fade_strength");
    }
    return m;
}

/* W171: Packs-Zustand fuer das Regelwerk (R17-R22, R29-R31). */
void packs_env(lr_env_t *e)
{
    int i;
    e->gfx_lock = gfx_lock_now(); /* W173 */
    e->n_packs = g_pk_n;
    e->tex_packs = 0;
    e->fx_pack[0] = '\0';
    for (i = 0; i < g_pk_n; i++) {
        int st = packs_status_of(i);
        if (!g_pk_ord[i].enabled || st < 1) {
            continue; /* aus oder abgelehnt */
        }
        if (packs_is_fx(i)) {
            if (!e->fx_pack[0]) { /* oberstes aktives Effekt-Pack gewinnt */
                strncpy_s(e->fx_pack, sizeof e->fx_pack,
                          g_pk_scan[i].name[0] ? g_pk_scan[i].name
                                               : g_pk_ord[i].id,
                          _TRUNCATE);
            }
        } else {
            e->tex_packs++;
        }
    }
    e->pk_level = packs_level_num();
    e->pk_sel = (g_pk_sel >= 0 && g_pk_sel < g_pk_n) ? g_pk_sel : -1;
    e->pk_sel_rejected = 0;
    e->pk_sel_enabled = 0;
    e->pk_sel_reason[0] = '\0';
    if (e->pk_sel >= 0) {
        const char *w, *r;
        packs_state(e->pk_sel, &w, &r, NULL);
        e->pk_sel_rejected = packs_status_of(e->pk_sel) == 0;
        e->pk_sel_enabled = g_pk_ord[e->pk_sel].enabled;
        /* W172: Grund im Klartext (Code nur in der Diagnose) */
        strncpy_s(e->pk_sel_reason, sizeof e->pk_sel_reason,
                  packs_reason_text(r), _TRUNCATE);
    }
}

/* W172: Listen-Unterklasse: Klick auf den Schalter = an/aus, Ziehen =
 * Reihenfolge, Leertaste = an/aus, Alt+Pfeil = verschieben. */
static int packs_item_at(HWND lb, int y)
{
    int top = (int)SendMessageW(lb, LB_GETTOPINDEX, 0, 0);
    int ih = (int)SendMessageW(lb, LB_GETITEMHEIGHT, 0, 0);
    int i;
    if (ih <= 0) {
        return -1;
    }
    i = top + y / ih;
    return i;
}

LRESULT CALLBACK pklist_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp,
                                    UINT_PTR sid, DWORD_PTR ref)
{
    (void)sid;
    (void)ref;
    switch (msg) {
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        int i = packs_item_at(h, y);
        if (i >= 0 && i < g_pk_n && x < SX(44)) {
            SetFocus(h);
            ListBox_SetCurSel(h, i);
            packs_toggle_idx(i);
            return 0;
        }
        g_pk_drag = (i >= 0 && i < g_pk_n) ? i : -1;
        g_pk_drop = -1;
        g_pk_dragging = 0;
        break;
    }
    case WM_MOUSEMOVE:
        if ((wp & MK_LBUTTON) && g_pk_drag >= 0) {
            int y = GET_Y_LPARAM(lp), ih = (int)SendMessageW(h, LB_GETITEMHEIGHT, 0, 0);
            int top = (int)SendMessageW(h, LB_GETTOPINDEX, 0, 0), to;
            if (ih <= 0) {
                break;
            }
            to = top + (y + ih / 2) / ih;
            if (to < 0) {
                to = 0;
            }
            if (to > g_pk_n) {
                to = g_pk_n;
            }
            if (!g_pk_dragging && to != g_pk_drag && to != g_pk_drag + 1) {
                g_pk_dragging = 1;
            }
            if (g_pk_dragging && to != g_pk_drop) {
                g_pk_drop = to;
                InvalidateRect(h, NULL, FALSE);
            }
            if (g_pk_dragging) {
                SetCursor(LoadCursor(NULL, IDC_SIZENS));
                return 0; /* keine Mehrfach-Auswahl durch Ziehen */
            }
        }
        break;
    case WM_LBUTTONUP:
        if (g_pk_dragging) {
            int from = g_pk_drag, to = g_pk_drop;
            g_pk_dragging = 0;
            g_pk_drag = g_pk_drop = -1;
            DefSubclassProc(h, msg, wp, lp);
            packs_move_to(from, to);
            return 0;
        }
        g_pk_drag = -1;
        break;
    case WM_CAPTURECHANGED:
        if (g_pk_dragging) {
            g_pk_dragging = 0;
            g_pk_drag = g_pk_drop = -1;
            InvalidateRect(h, NULL, FALSE);
        }
        break;
    case WM_KEYDOWN:
        if (wp == VK_SPACE) {
            packs_toggle();
            return 0;
        }
        break;
    case WM_SYSKEYDOWN: /* Alt+Pfeil hoch/runter = verschieben */
        if (wp == VK_UP || wp == VK_DOWN) {
            packs_move(wp == VK_UP ? -1 : 1);
            SetFocus(ctl(ID_PK_LIST));
            return 0;
        }
        break;
    case WM_GETDLGCODE: /* Leertaste/Pfeile selbst behandeln */
        return DefSubclassProc(h, msg, wp, lp) | DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_NCDESTROY:
        RemoveWindowSubclass(h, pklist_proc, 2);
        break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

/* Zeile im Detail: Bezeichnung links, Wert rechts. */
static int pk_line(int font, const char *t, int x, int y, int w, COLORREF c)
{
    label1(font, t, x, y, w, 16, c);
    return y + 16;
}

void paint_packs(void)
{
    int i = (g_pk_sel >= 0 && g_pk_sel < g_pk_n) ? g_pk_sel : -1;
    int x = PKR_X + 18, w = PKR_W - 36, y, ymax = CY0 + 426;
    char t[700];
    /* --- links oben: installierte Packs --- */
    card(CX0, CY0, PKL_W, 284, LT("gui.pk_list"));
    /* W287: Packs experimentell = Symbol ID_XEXP_PACKS */
    if (g_pk_n > 0) {
        if (!paint_reason(31, CX0 + 18, CY0 + 38, PKL_W - 36, 16)) {
            label1(F_TINY, LT("gui.pk_list_hint"), CX0 + 18, CY0 + 38, PKL_W - 36,
                   16, C_DIM);
        }
    } else { /* leerer Zustand (Konzept 6.7, R29/R32) */
        if (g_rr[29].active) { /* R29 als Ueberschrift des leeren Zustands */
            label(F_SEMI, g_rr[29].text, CX0 + 18, CY0 + 52, PKL_W - 36, 48, C_TEXT);
        }
#if !SB_EDITION_COMMUNITY
        if (!g_rr[32].active) {
            label(F_TINY, LT("gui.pk_creator_txt"), CX0 + 18, CY0 + 152,
                  PKL_W - 36, 30, C_DIM);
        } else {
            paint_reason(32, CX0 + 18, CY0 + 118, PKL_W - 36, 64);
        }
#endif
        label(F_TINY, LT("gui.pk_drop_hint"), CX0 + 18, CY0 + 192, PKL_W - 36, 44,
              C_FAINT);
    }
    /* --- links unten: Texturdetail + Grenzen je Kategorie --- */
    card(CX0, CY0 + 296, PKL_W, 200, LT("gui.pk_level"));
    if (!paint_reason(18, CX0 + 18, CY0 + 334, PKL_W - 36, 16)) {
        label1(F_TINY, LT(packs_level_num() == 0 ? "gui.pk_level_orig"
                                                 : "gui.pk_level_hint"),
               CX0 + 18, CY0 + 334, PKL_W - 36, 16,
               packs_level_num() == 0 ? C_WARN : C_DIM);
    }
    if (!paint_reason(19, CX0 + 18, CY0 + 354, PKL_W - 36 - 118, 18)) {
        label1(F_SMALL, LT("gui.pk_caps"), CX0 + 18, CY0 + 354, PKL_W - 36 - 118,
               18, C_DIM);
    }
    {
        int c;
        for (c = 0; c < SB_PACK_NCAT; c++) {
            int cx = CX0 + 18 + (c % 3) * 122, cy = CY0 + 377 + (c / 3) * 37;
            label1(F_TINY, LT(kPkCatKey[c]), cx, cy, 112, 14, C_DIM);
        }
    }
    /* --- rechts: gewaehltes Pack / "Was sind Packs?" --- */
    if (i < 0) {
        card(PKR_X, CY0, PKR_W, 496, LT("gui.pk_what"));
        label(F_SMALL, LT("gui.pk_what_txt"), x, CY0 + 44, w, 300, C_DIM);
        return;
    }
    card(PKR_X, CY0, PKR_W, 496, NULL);
    {
        const char *word, *reason;
        COLORREF scol;
        int fx = packs_is_fx(i);
        packs_state(i, &word, &reason, &scol);
        label1(F_CARD, g_pk_scan[i].name, x, CY0 + 12, w, 24, C_TEXT);
        _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.pk_head"),
                    LT(fx ? "gui.pk_type_fx" : "gui.pk_type_tex"),
                    g_pk_scan[i].version, g_pk_scan[i].author);
        y = pk_line(F_TINY, t, x, CY0 + 40, w, C_DIM);
        if (strstr(g_pk_scan[i].license, "rivate") ||
            strstr(g_pk_scan[i].license, "not distribute")) {
            y = pk_line(F_TINY, LT("gui.pk_private"), x, y, w, C_WARN);
        } else {
            _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.pk_lic"), g_pk_scan[i].license);
            y = pk_line(F_TINY, t, x, y, w, C_DIM);
        }
        /* Passt zum Spiel (V2) */
        {
            const char *fit;
            COLORREF fc = C_DIM;
            if (g_pk_chk[i] >= 0 && strcmp(g_pk_chk_reason[i], "game_mismatch") == 0) {
                fit = LT("gui.pk_fit_no");
                fc = C_ERR;
            } else if (g_pk_chk[i] >= 1 && g_pk_chk_xbe[i]) {
                /* W256 PACK-ANYVER: genau / andere Fassung / Familie */
                static char ft[200];
                lcfg_pack_fit_text(g_pk_chk_xbe[i], g_pk_chk_made[i],
                                   g_pk_chk_act[i], ft, sizeof ft);
                fit = ft;
                fc = C_OK;
            } else if (g_rr[22].active) {
                fit = g_rr[22].text;
            } else {
                fit = LT("gui.pk_fit_unk");
            }
            _snprintf_s(t, sizeof t, _TRUNCATE, "%s: %s", LT("gui.pk_fit"), fit);
            y = pk_line(F_TINY, t, x, y, w, fc);
        }
        /* Status im Klartext (Code nur mit offener Diagnose) */
        if (reason && reason[0]) {
            _snprintf_s(t, sizeof t, _TRUNCATE, "%s \xE2\x80\x93 %s", word,
                        packs_reason_text(reason));
        } else {
            strcpy_s(t, sizeof t, word);
        }
        label(F_SMALL, t, x, y + 4, w, 34, scol);
        y += 40;
        if (g_diag_open && reason && reason[0]) {
            _snprintf_s(t, sizeof t, _TRUNCATE, "%s %s", reason,
                        g_pk_chk[i] >= -1 ? g_pk_chk_detail[i] : "");
            y = pk_line(F_TINY, t, x, y, w, C_FAINT);
        }
        if (!g_pk_ord[i].enabled || g_pk_cov[i].cat[0] < 0) {
            label(F_TINY, LT(g_pk_ord[i].enabled ? "gui.pk_cov_unread" : "gui.pk_cov_off"),
                  x, y + 6, w, 32, C_FAINT);
        } else if (fx) {
            /* Effektwerte mit Schloss (Muster D): gelten vor den eigenen
             * Einstellungen; gewinnt ein hoeheres Pack, steht es dabei. */
            int k, any = 0;
            label1(F_SEMI, LT("gui.pk_fx_title"), x, y + 4, w, 20, C_TEXT);
            y += 26;
            for (k = 0; k < SB_FXN && y < ymax - 16; k++) {
                char key[40], *p;
                int v;
                _snprintf_s(key, sizeof key, _TRUNCATE, "%s=", kSbFxParams[k].key);
                p = strstr(g_pk_scan[i].paramsum, key);
                if (!p || (p != g_pk_scan[i].paramsum && p[-1] != ' ')) {
                    continue;
                }
                v = atoi(p + strlen(key));
                any = 1;
                if (k == 4) {
                    _snprintf_s(t, sizeof t, _TRUNCATE, "%s: %s", LT(kPkFxKey[k]),
                                LT(v ? "gui.on" : "gui.off"));
                } else if (k <= 3 || k == 6) {
                    _snprintf_s(t, sizeof t, _TRUNCATE, "%s: %d %%", LT(kPkFxKey[k]), v);
                } else {
                    _snprintf_s(t, sizeof t, _TRUNCATE, "%s: %+d", LT(kPkFxKey[k]), v);
                }
                if (g_pk_fx_hit[k] && g_pk_fx_src[k][0] &&
                    strcmp(g_pk_fx_src[k], g_pk_ord[i].id) != 0) {
                    int wi, n = (int)strlen(t);
                    const char *wn = g_pk_fx_src[k];
                    for (wi = 0; wi < g_pk_n; wi++) {
                        if (strcmp(g_pk_ord[wi].id, g_pk_fx_src[k]) == 0) {
                            wn = g_pk_scan[wi].name;
                        }
                    }
                    _snprintf_s(t + n, sizeof t - n, _TRUNCATE, LT("gui.pk_fx_also"), wn);
                }
                if (g_has_icons) {
                    wchar_t ic = 0xE72E;
                    RECT q; /* W281 */
                    lay_px(x, y, 14, 16, 0, 0, &q);
                    cv_textw(&g_bb, g_f[F_ICONSM], &ic, 1, q.left, q.top, SX(14), SX(16),
                             C_ACCENT_HI, DT_LEFT | DT_TOP | DT_SINGLELINE);
                }
                y = pk_line(F_TINY, t, x + 16, y, w - 16, C_TEXT);
            }
            if (!any) {
                y = pk_line(F_TINY, LT("gui.pk_fx_none"), x, y, w, C_DIM);
            }
            label(F_TINY, LT("gui.pk_fx_over"), x, y + 4, w, 30, C_DIM);
            y += 36;
        } else {
            /* Abdeckung: Kategorie, Anzahl, wirksame Stufe (* = eigene Grenze) */
            int c, lv = packs_level_num(), star = 0;
            label1(F_SEMI, LT("gui.pk_cov_title"), x, y + 4, w, 20, C_TEXT);
            y += 26;
            for (c = 0; c < SB_PACK_NCAT && y < ymax - 16; c++) {
                int n = g_pk_cov[i].cat[c], eff = packs_cap_of(c);
                char num[16];
                if (n <= 0) {
                    continue;
                }
                if (eff >= 4 && g_pk_cov[i].cat4[c] == 0) {
                    eff = g_pk_cov[i].cat2[c] ? 2 : 0; /* faellt zurueck */
                } else if (eff == 2 && g_pk_cov[i].cat2[c] == 0) {
                    eff = 0;
                }
                label1(F_TINY, LT(kPkCatKey[c]), x, y, w - 110, 16, C_DIM);
                _snprintf_s(num, sizeof num, _TRUNCATE, "%d", n);
                L_text(F_TINY, num, x + w - 110, y, 50, 16, C_TEXT,
                       DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
                _snprintf_s(t, sizeof t, _TRUNCATE, "%s%s", packs_cap_str(eff),
                            packs_cap_stored(c) >= 0 && packs_cap_stored(c) < lv ? " *" : "");
                if (packs_cap_stored(c) >= 0 && packs_cap_stored(c) < lv) {
                    star = 1;
                }
                L_text(F_TINY, t, x + w - 50, y, 50, 16, C_TEXT,
                       DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
                y += 16;
            }
            if (star) {
                y = pk_line(F_TINY, LT("gui.pk_cov_star"), x, y, w, C_FAINT);
            }
        }
        /* Konflikte: "verdeckt durch" (hoeher) und "verdeckt selbst" */
        if (g_pk_ord[i].enabled && y < ymax - 36) {
            int k, any = 0;
            label1(F_SEMI, LT("gui.pk_conf_title"), x, y + 6, w, 20, C_TEXT);
            y += 28;
            for (k = 0; k < g_pk_n && y < ymax - 16; k++) {
                int a = g_pk_covby[i * PK_MAXN + k], b = g_pk_covby[k * PK_MAXN + i];
                if (k == i) {
                    continue;
                }
                if (a > 0) {
                    _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.pk_conf_by"), a,
                                g_pk_scan[k].name);
                    label(F_TINY, t, x, y, w, 30, C_WARN);
                    y += 30;
                    any = 1;
                }
                if (b > 0 && y < ymax - 16) {
                    _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.pk_conf_over"), b,
                                g_pk_scan[k].name);
                    label(F_TINY, t, x, y, w, 30, C_DIM);
                    y += 30;
                    any = 1;
                }
            }
            if (!any) {
                pk_line(F_TINY, LT("gui.pk_conf_none"), x, y, w, C_DIM);
            }
        }
    }
    paint_reason(21, x, CY0 + 430, w, 16);
}

void show_page(int p)
{
    int i;
    if (p < 0 || p > P_HUB_LAST) { /* W397: P_LSET = Launcher-Einstellungen (Zahnrad); W470: P_NETI = steelnet */
        p = P_PLAY;
    }
    if (p == P_ALPHA && !g_alpha_tools) { /* W272: Seite ausgeblendet */
        p = P_ADV;
    }
    /* W276: Packs nur Steel Battalion; W392: bei LoC steht auf diesem Platz die Seite Netzwerk (P_LNET) */
    g_page = p;
    hub_page_shown(p); /* W470: Spielseite merken (Rueckweg), Lobbyliste nur solange steelnet sichtbar */
    nav_sub_layout(); /* W399: Unterpunkte von Multiplayer auf-/zuklappen (vor der Sichtbarkeit) */
    g_setup_force = 0; /* W172: Einrichtungskarte nur, wenn Regel R3 greift */
    if ((p == P_PLAY || p == P_LNET) && g_game == LGAME_LOC) { /* W397: Kopieren jetzt auf Multiplayer */
        onl_status_refresh(); /* W334: Raumcode/Sicherheitsmeldung vor der Sichtbarkeit (Kopieren) */
    }
    if (g_game == LGAME_LOC) {
        loc_gate_sync(); /* W403: Disc geprueft/geaendert -> Startknopf passend (nie aus dem Zeichnen) */
    }
    for (i = 0; i < g_ui_n; i++) {
        uictl_t *u = &g_ui[i];
        if (u->parent != g_hwnd) {
            continue;
        }
        ShowWindow(u->h, ui_visible_on(u, p) ? SW_SHOWNA : SW_HIDE);
    }
    if (p == P_CTRL) {
        static int once;
        if (!once) {
            once = 1;
            update_sbc_status();
            joy_card_scan(); /* W282 */
        }
        update_pad_status();
    }
    if (p == P_DISPLAY) {
        update_aspect_info();
    }
    if (p == P_PLAY) {
        update_summary();
        update_vr_button(); /* W159 */
    }
    if (p == P_PACKS && g_game != LGAME_LOC) { /* W148 (W392: bei LoC Netzwerk) */
        packs_refresh(1);
    }
    if (p == P_LNET && g_game == LGAME_LOC) {
        onl_sync_ui(); /* W392: Schalter/Grau zum gemerkten Stand */
    }
    if (p == P_ADV) {
        refresh_profiles();
    }
    if (p == P_ALPHA) {
        alpha_page_shown(); /* W272: VT-Liste, A/B-Werte */
    }
    if (p == P_LNET && g_game == LGAME_LOC) {
        acc_page_shown(); /* W328: Konto laden, Status holen (W397: Multiplayer statt Erweitert) */
    }
    if (p == P_PLAY && g_game == LGAME_LOC) {
        acc_play_shown(); /* W418: Knopf der Karte Multiplayer (Konto erstellen / Konto) */
    }
    { /* W227: auf Spielen fehlt Launch in der Fussleiste -> Speichern und
       * Rueckgaengig an die rechte Kante statt mittig im Leeren */
        static const int ids[2] = { ID_SAVE, ID_UNDO };
        int k, dx = p == P_PLAY ? 150 + 12 : 0;
        for (k = 0; k < 2; k++) {
            uictl_t *u = ui_find(ids[k]);
            int x0 = ids[k] == ID_SAVE ? WIN_W - 28 - 150 - 12 - 130
                                       : WIN_W - 28 - 150 - 12 - 130 - 10 - 112;
            if (u && u->x != x0 + dx) {
                RECT q; /* W281: Layout b am rechten Rand */
                u->x = x0 + dx;
                ui_px_rect(u, &q);
                SetWindowPos(u->h, NULL, q.left, q.top, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
    }
    netm_sync_ui(); /* W397: Zahnrad gewaehlt auf P_LSET, Stufen zum Zustand */
    ui_apply_rules(); /* W171 */
    redraw_all();
}

/* W397 LAUNCHER-STRUCT: Seitenleiste je Spiel gleich aufgebaut. SB: Play, Display, Graphics, Packs, Controls,
 * Audio & extras, Advanced. LoC: Play, Display, Graphics, Controls, Audio & extras, Multiplayer (Packs-Platz),
 * Advanced. Test tools liegen unter dem Zahnrad (nicht mehr in der Leiste). */
static const int kNavSb[NAV_ORDER_N] = { P_PLAY, P_DISPLAY, P_GFX, P_PACKS, P_CTRL, P_AUDIO, P_ADV };
static const int kNavLoc[NAV_ORDER_N] = { P_PLAY, P_DISPLAY, P_GFX, P_CTRL, P_AUDIO, P_LNET, P_ADV };
const int *nav_order(void)
{
    return g_game == LGAME_LOC ? kNavLoc : kNavSb;
}

/* W171: Strg+Tab / Strg+Umschalt+Tab = naechste/vorige Seite (zyklisch, W397: in der Reihenfolge der Leiste;
 * von Test tools/Launcher-Einstellungen aus zur ersten bzw. letzten Seite). */
int nav_next(int page, int dir)
{
    const int *o = nav_order();
    int i;
    for (i = 0; i < NAV_ORDER_N; i++) {
        if (o[i] == page) {
            return o[(i + (dir < 0 ? NAV_ORDER_N - 1 : 1)) % NAV_ORDER_N];
        }
    }
    return dir < 0 ? o[NAV_ORDER_N - 1] : o[0];
}

/* W142: Fenster-Texte in der aktiven Sprache neu setzen (ohne Neustart).
 * Auswahl und Nutzereingaben bleiben; gemalte Texte folgen per Neuzeichnen. */
static void combo_refill(int id, const char *const *keys, int n)
{
    HWND c = ctl(id);
    int sel, i;
    if (!c) {
        return;
    }
    sel = ComboBox_GetCurSel(c);
    ComboBox_ResetContent(c);
    for (i = 0; i < n; i++) {
        combo_add(c, LT(keys[i]));
    }
    ComboBox_SetCurSel(c, (sel >= 0 && sel < n) ? sel : 0);
}

/* Nur Eintrag 0 ersetzen (Rest sind Nutzerdaten: Profile, Roheintraege). */
static void combo_set0(int id, const char *key)
{
    HWND c = ctl(id);
    wchar_t *w;
    int sel;
    if (!c || ComboBox_GetCount(c) < 1) {
        return;
    }
    sel = ComboBox_GetCurSel(c);
    ComboBox_DeleteString(c, 0);
    w = u8_wdup(LT(key));
    if (w) {
        ComboBox_InsertString(c, 0, w);
        free(w);
    }
    ComboBox_SetCurSel(c, sel);
}

void ui_apply_language(void)
{
    static const char *const kVsync[] = { "gui.off", "gui.on" };
    static const char *const kWinmode[] = { "gui.wm_win", "gui.wm_borderless", "gui.wm_excl" };
    static const char *const kMonfx[] = { "gui.monfx_orig", "gui.monfx_grain", "gui.monfx_glitch", "gui.off" };
    static const char *const kCview[] = { "gui.cview_orig", "gui.cview_flat", "gui.cview_mon", "gui.cview_mon2" }; /* W319 */
    static const char *const kAa[] = { "gui.aa_off", "gui.aa_final", "gui.aa_world", "gui.aa_both" };
    static const char *const kLight[] = { "gui.gfx_light_on", "gui.gfx_light_off" }; /* W173 */
    static const char *const kOnOff[] = { "gui.off", "gui.on" };
    static const char *const kFxl[] = { "gui.gfx_fx_orig", "gui.gfx_fx_subtle", "gui.gfx_fx_strong" };
    static const char *const kFxlLoc[] = { "gui.gfx_fx_orig", "gui.gfx_p_enh", "gui.gfx_p_vivid" }; /* W466: LoC Original/Enhanced, W478: Vivid */
    int nfx = 3, fxloc = g_game == LGAME_LOC;
    static const char *const kTex[] = { "gui.gfx_tex_o", "gui.gfx_tex_hd" };
    static const char *const kScale[] = { "gui.scale2", "gui.scale4" };
    static const char *const kEdge[] = { "gui.edge_off", "gui.edge_cut", "gui.edge_fade" };
    static const char *const kVr[] = { "gui.vr_prev_sbs", "gui.vr_prev_left", "gui.vr_prev_right", "gui.vr_prev_off" };
    static const char *const kVrPre[] = { "gui.vrp_perf", "gui.vrp_bal", "gui.vrp_qual", "gui.vrp_custom" }; /* W183 */
    static const char *const kMouse[] = { "gui.mouse_abs", "gui.mouse_rel" };
    static const char *const kStick[] = { "gui.stick_pos", "gui.stick_rel" }; /* W271 */
    static const char *const kAssist[] = { "gui.ctl_assist0", "gui.ctl_assist1",
                                           "gui.ctl_assist2" }; /* W236 */
    static const char *const kLang[] = { "gui.lang_en", "gui.lang_de" };
    HWND c;
    int i, sel;
    /* Navigation + Titelleiste + Start */
    for (i = 0; i < P_N; i++) {
        ui_settext(ctl(ID_NAV0 + i), LT(page_name_key(i))); /* W392 */
    }
    ui_settext(ctl(ID_NAVSUB_MATCH), LT("gui.mp_sub_match")); /* W399 */
    ui_settext(ctl(ID_NAVSUB_ACC), LT("gui.mp_sub_acc"));
    ui_settext(ctl(ID_MIN), LT("gui.win_min"));
    ui_settext(ctl(ID_CLOSE), LT("gui.btn_close"));
    ui_settext(ctl(ID_START_BIG), LT("gui.btn_bigstart"));
    ui_settext(ctl(ID_START_VR), LT("gui.btn_vrstart")); /* W159 */
    ui_settext(ctl(ID_BG), LT("gui.bg"));
    ui_settext(ctl(ID_UPD_NOW), LT("gui.upd_now")); /* W250 */
    ui_settext(ctl(ID_UPD_OPEN), LT("gui.upd_open"));
    lset_relabel(); /* W397 */
    hub_relabel(); /* W470 */
    /* Grafik-Kombis (Reihenfolge wie build_controls) */
    c = ctl(ID_PRESET);
    if (c) {
        sel = ComboBox_GetCurSel(c);
        ComboBox_ResetContent(c);
        for (i = 0; i < KPRESETS_N; i++) {
            combo_add(c, kPresets[i].lkey ? LT(kPresets[i].lkey) : kPresets[i].label);
        }
        ComboBox_SetCurSel(c, (sel >= 0 && sel < KPRESETS_N) ? sel : KPRESETS_N - 1);
    }
    combo_refill(ID_VSYNC, kVsync, 2);
    { static const char *const kPacing[] = { "gui.pacing_timer", "gui.pacing_monitor", "gui.pacing_vrr" }; /* W468 */
      combo_refill(ID_PACING, kPacing, 3); }
    c = ctl(ID_FPS);
    if (c) { /* 0 = Text, 1..3 = Zahlen */
        static const char *const n[] = { "30", "60", "120" };
        sel = ComboBox_GetCurSel(c);
        ComboBox_ResetContent(c);
        combo_add(c, LT("gui.fps_off"));
        for (i = 0; i < 3; i++) {
            combo_add(c, n[i]);
        }
        ComboBox_SetCurSel(c, (sel >= 0 && sel < 4) ? sel : 0);
    }
    combo_refill(ID_WINMODE, kWinmode, 3);
    combo_set0(ID_FSHZ, "gui.fshz_desktop");
    if (ctl(ID_IPFPS)) { /* W223; W309: ip_fill behaelt die Wahl */
        ip_fill(ctl(ID_IPFPS));
    }
    combo_refill(ID_MONFX, kMonfx, 4);
    combo_refill(ID_AAMODE, kAa, 4);
    combo_refill(ID_REM_SCALE, kScale, 2);
    combo_refill(ID_GFX_LIGHT, kLight, 2); /* W173 */
    combo_refill(ID_GFX_SHADOW, kOnOff, 2);
    combo_refill(ID_GFX_SOFT, kOnOff, 2);
    while (ctl(ID_GFX_FX) && ComboBox_GetCount(ctl(ID_GFX_FX)) > nfx) { /* "Benutzerdefiniert" setzt gfx_sync_ui */
        ComboBox_DeleteString(ctl(ID_GFX_FX), ComboBox_GetCount(ctl(ID_GFX_FX)) - 1);
    }
    combo_refill(ID_GFX_FX, fxloc ? kFxlLoc : kFxl, nfx);
    combo_refill(ID_GFX_TEX, kTex, 2);
    ui_settext(ctl(ID_GP_RESET), LT("gui.gfx_reset"));
    ui_settext(ctl(ID_REM_LABEL), LT("gui.rem_label"));
    combo_refill(ID_CHIDE, kCview, 4); /* W268, W319 */
    combo_refill(ID_EDGE, kEdge, 3);
    if (ctl(ID_CPERSP)) { cpersp_fill(ctl(ID_CPERSP)); } /* W191 */
    if (ctl(ID_CPANINI)) { cpersp_fill(ctl(ID_CPANINI)); }
    if (ctl(ID_CWIDTH)) { cwidth_fill(ctl(ID_CWIDTH)); } /* W235 */
    if (ctl(ID_VRRES)) { vrres_fill(ctl(ID_VRRES)); } /* W227 */
    c = ctl(ID_EDGE_W);
    if (c) { /* Zahlenformate */
        sel = ComboBox_GetCurSel(c);
        ComboBox_ResetContent(c);
        for (i = 0; i < KEDGEW_N; i++) {
            char t[32];
            _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.edge_w_fmt"), kEdgeW[i]);
            combo_add(c, t);
        }
        ComboBox_SetCurSel(c, (sel >= 0 && sel < KEDGEW_N) ? sel : 2);
    }
    c = ctl(ID_EDGE_S);
    if (c) {
        sel = ComboBox_GetCurSel(c);
        ComboBox_ResetContent(c);
        for (i = 0; i < KEDGES_N; i++) {
            char t[32];
            _snprintf_s(t, sizeof t, _TRUNCATE, LT("gui.edge_s_fmt"), kEdgeS[i]);
            combo_add(c, t);
        }
        ComboBox_SetCurSel(c, (sel >= 0 && sel < KEDGES_N) ? sel : KEDGES_N - 1);
    }
    combo_refill(ID_VR, kVr, 4);
    combo_refill(ID_VRPRESET, kVrPre, 4); /* W183 */
    c = ctl(ID_RTSCALE); /* W148: 2 Texte + Zahlen 1..4 */
    if (c) {
        sel = ComboBox_GetCurSel(c);
        ComboBox_ResetContent(c);
        combo_add(c, LT("gui.rs_dyn"));
        combo_add(c, LT("gui.rs_autof"));
        combo_add(c, "1");
        combo_add(c, "2");
        combo_add(c, "3");
        combo_add(c, "4");
        ComboBox_SetCurSel(c, (sel >= 0 && sel < 6) ? sel : 0);
    }
    /* Ton */
    ui_settext(ctl(ID_AUDIO), LT("gui.ton_on"));
    ui_settext(ctl(ID_LIVE), LT("gui.ton_live"));
    ui_settext(ctl(ID_HRTF), LT("gui.hrtf")); /* W356 */
    ui_settext(ctl(ID_SAVE_LOCK), LT("gui.save_lock")); /* W190 */
    ui_settext(ctl(ID_PH_BROWSE), LT("gui.btn_browse")); /* W195 */
    ui_settext(ctl(ID_PH_OPEN), LT("gui.extras_open"));
    {
        HWND c = ctl(ID_PH_RES); /* W290: Fenster, 4K, 8K, x2, x4, x8 */
        if (c) {
            int cur = ComboBox_GetCurSel(c);
            ComboBox_ResetContent(c);
            combo_add(c, LT("gui.extras_res_1"));
            combo_add(c, "4K");
            combo_add(c, "8K");
            combo_add(c, LT("gui.extras_res_2"));
            combo_add(c, LT("gui.extras_res_4"));
            combo_add(c, LT("gui.extras_res_8"));
            ComboBox_SetCurSel(c, cur);
        }
    }
    {
        static const char *const kPhAsp[] = { "ovl.ph_asp_square", "ovl.ph_asp_wide" }; /* W290: 4:3 / breit */
        static const char *const kPhView[] = { "ovl.ph_v_game", "ovl.ph_v_flat", "ovl.ph_v_world", "ovl.ph_v_cine" }; /* W428: Cinematic */
        combo_refill(ID_PH_ASP, kPhAsp, 2);
        combo_refill(ID_PH_VIEW, kPhView, 4);
    }
    { /* W445: erster Eintrag "Aus" in der Sprache */
        HWND c = ctl(ID_PH_CSELF);
        if (c) {
            int cur = ComboBox_GetCurSel(c);
            ComboBox_ResetContent(c);
            cself_fill(c);
            ComboBox_SetCurSel(c, cur);
        }
    }
    ui_settext(ctl(ID_RP_LABEL), LT("gui.rp_label"));
    ui_settext(ctl(ID_RP_LOGO), LT("gui.rp_logo"));
    ui_settext(ctl(ID_WM_MEDIA), LT("gui.wm_media"));
    ui_settext(ctl(ID_PH_HUD), LT("gui.ph_hud"));
    ui_settext(ctl(ID_DIAG), LT(g_diag_open ? "gui.adv_diag_hide" : "gui.adv_diag_show"));
    ui_settext(ctl(ID_UNDO), LT("gui.undo"));
    /* Steuerung */
    combo_set0(ID_CTRL_PROF, "gui.legacy_label");
    ui_settext(ctl(ID_CTRL_REFRESH), LT("gui.ctl_reload"));
    ui_settext(ctl(ID_CTRL_BIND), LT("gui.ctl_bind"));
    ui_settext(ctl(ID_CTRL_JOY), LT("gui.joy_btn")); /* W221 */
    ui_settext(ctl(ID_CTRL_EXPORT), LT("gui.cx_export")); /* W419 */
    ui_settext(ctl(ID_CTRL_IMPORT), LT("gui.cx_import"));
    ui_settext(ctl(ID_CTRL_ZADIG), LT("gui.zadig_btn")); /* W185 */
    ui_settext(ctl(ID_CTRL_SBC), LT("gui.sbc"));
    ui_settext(ctl(ID_CTRL_SBCCHK), LT("gui.btn_check"));
    ui_settext(ctl(ID_CTRL_MOUSEOFF), LT("gui.ctl_mouseoff"));
    combo_refill(ID_CTRL_AIMASSIST, kAssist, 3); /* W236 */
    ui_settext(ctl(ID_CTRL_AUTOGEAR), LT("gui.ctl_autogear"));
    ui_settext(ctl(ID_CTRL_ALLY), LT("gui.ctl_ally")); /* W424 */
    ui_settext(ctl(ID_CTRL_DESPIKE), LT("gui.ctl_despike")); /* W439 */
    ui_settext(ctl(ID_PAL50), LT("gui.pal50")); /* W234 */
    ui_settext(ctl(ID_CTRL_ASSIST), LT("gui.ctl_assist"));
    ui_settext(ctl(ID_CTRL_BROWSER), LT("gui.keys_browser")); /* W179 */
    combo_refill(ID_MOUSE_MODE, kMouse, 2);
    combo_refill(ID_STICK_AIM, kStick, 2); /* W271 */
    /* Profile + Pfade + Fussleiste */
    ui_settext(ctl(ID_PROF_LOAD), LT("gui.adv_setup_load"));
    ui_settext(ctl(ID_PROF_SAVE), LT("gui.adv_setup_save"));
    ui_settext(ctl(ID_PROF_DEL), LT("gui.btn_delete"));
    ui_settext(ctl(ID_PROF_DFLT), LT("gui.adv_setup_auto"));
    ui_settext(ctl(ID_XBE_B), LT("gui.btn_browse"));
    ui_settext(ctl(ID_IMAGE_B), LT("gui.btn_browse")); /* W150 */
    ui_settext(ctl(ID_IMAGE_C), LT("gui.btn_check"));
    ui_settext(ctl(ID_MEDIA_B), LT("gui.btn_browse"));
    ui_settext(ctl(ID_SAVE), LT("gui.btn_save"));
    /* W172 LAUNCHER-P567: Spielen, Erweitert, Packs */
    for (i = 0; i < SUM_GO_N; i++) {
        ui_settext(ctl(ID_SUM_GO0 + i), LT("gui.play_change"));
    }
    ui_settext(ctl(ID_PLAY_FOLDER), LT("gui.play_use_folder"));
    ui_settext(ctl(ID_HDD_OPEN), LT("gui.adv_open"));
    ui_settext(ctl(ID_DATA_OPEN), LT("gui.adv_open_folder"));
    ui_settext(ctl(ID_SAVES_IMPORT), LT("gui.si_import"));
    ui_settext(ctl(ID_SAVES_EXPORT), LT("gui.si_export"));
    ui_settext(ctl(ID_SAVES_REPLAYS), LT("gui.rp_btn")); /* W254 */
    alpha_relabel(); /* W272 */
    dg_relabel(); /* W279 */
    ui_settext(ctl(ID_SAVES_RESET), LT("gui.saves_reset"));
    ui_settext(ctl(ID_GFX_PACKS), LT("gui.gfx_to_packs"));
    ui_settext(ctl(ID_PK_UP), "\xE2\x86\x91");
    ui_settext(ctl(ID_PK_DOWN), "\xE2\x86\x93");
    ui_settext(ctl(ID_PK_TOGGLE), LT("gui.pk_toggle"));
    ui_settext(ctl(ID_PK_ADD), LT("gui.pk_add"));
    ui_settext(ctl(ID_PK_DEL), LT("gui.pk_del"));
    ui_settext(ctl(ID_PK_CHECK), LT("gui.pk_check"));
    ui_settext(ctl(ID_PK_CHECKALL), LT("gui.pk_checkall"));
    ui_settext(ctl(ID_PK_FOLDER), LT("gui.pk_folder"));
    ui_settext(ctl(ID_PK_CAPRESET), LT("gui.pk_capreset"));
    ui_settext(ctl(ID_PK_LVL2), LT("gui.pk_x2"));
    ui_settext(ctl(ID_PK_LVL4), LT("gui.pk_x4best"));
#if !SB_EDITION_COMMUNITY
    ui_settext(ctl(ID_PK_CREATOR), LT("gui.pk_creator"));
#endif
    xbe_cue_update();
    packs_refresh_lists();
    ui_settext(ctl(ID_START), LT("gui.btn_start"));
    loctab_relabel(); /* W276 */
    orig_relabel(); /* W278 */
    lay_combo_fill(ctl(ID_LAYOUT)); /* W281 */
    ui_settext(ctl(ID_LAYOUT_LBL), LT("gui.lay_size")); /* W287 */
    /* Sprachwahl selbst */
    ui_settext(ctl(ID_LANG_LBL), LT("gui.lang_label"));
    combo_refill(ID_LANG, kLang, 2);
    /* Abgeleitete Texte neu (liest nur, aendert keine Eingaben) */
    {
        launcher_settings_t tmp;
        char note[256] = { 0 };
        int kind = 0;
        app_load_effective(g_client_dir, &tmp, note, sizeof note, &kind,
                           sb_lang_code());
        strcpy_s(g_note, sizeof g_note, note);
        g_note_kind = kind; /* W173 */
        if (kind == 2) {
            strcpy_s(g_profile_line, sizeof g_profile_line, note);
        } else if (kind == 1) {
            _snprintf_s(g_profile_line, sizeof g_profile_line, _TRUNCATE,
                        LT("gui.fill_writes"), note);
        } else {
            _snprintf_s(g_profile_line, sizeof g_profile_line, _TRUNCATE,
                        "%s", LT("gui.fill_nodefaults"));
        }
    }
    update_aspect_info();
    update_sbc_status();
    update_pad_status();
    update_ctrl_overview();
    refresh_profiles();
    update_summary();
    update_vr_button(); /* W159: Hinweistext in neuer Sprache */
    if (!g_has_ldr) {
        set_status_k(LT("gui.fill_noldr"), 2);
    } else { /* W173 LAUNCHER-P8: ohne Schluesselzahl (steht unter Diagnose) */
        set_status(g_note_kind == 2 ? g_note : LT("gui.fill_ready"));
    }
    base_build(); /* statischer Hintergrund (Fusshinweis) */
    redraw_all();
}

/* ------------------------------------------------------------------ */
